/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#include "dsp/osc_voice.h"

#include "dsp/fm_carrier.h"

#include <algorithm>
#include <array>
#include <numbers>
#include <limits>
#include <cmath>
#include <cstring>
#include <type_traits>

namespace feathers {

namespace {

/// Below this the glide has arrived: a thousandth of a cent, which is a hundred times finer than
/// anything the tuning params can express and far below what a phase increment resolves. Snapping
/// there rather than approaching forever is what lets a settled voice skip the per-sample pitch work
/// entirely.
constexpr double kGlideDoneSemitones = 1e-5;

} // namespace

double pitchToHz(double semitones) {
	return kConcertAHz * std::exp2((semitones - kConcertAKey) / 12.0);
}

double hzToPitch(double hz) {
	return kConcertAKey + 12.0 * std::log2(hz / kConcertAHz);
}

OscVoice::OscVoice() : vocodeGains_(std::make_unique<VocodeGains>()), peaksGlide_(std::make_unique<PeaksGlide>()) {
	std::fill(std::begin(carry_), std::end(carry_), 0.0f);
	std::fill(std::begin(oversampled_), std::end(oversampled_), 0.0f);
	std::fill(std::begin(carryR_), std::end(carryR_), 0.0f);
	std::fill(std::begin(oversampledR_), std::end(oversampledR_), 0.0f);
}

void OscVoice::setCharacterBank(const CharacterBank* bank) {
	bank_ = bank;
	fresh_ = nullptr;
	stale_ = nullptr;
	freshR_ = nullptr;
	staleR_ = nullptr;
	stereoStream_ = false;
	haveSynced_ = false;
	bankMoved_ = false;
	fadeTotal_ = 0;
	fadeLeft_ = 0;
	gridsDirty_ = true;
	voxBakeDue_ = 0;
	voxLive_ = false;
	invalidateVoxBakes();
	partialsPrimed_ = false; // a new bank: the next Partials block resets the rotators, as a note-on does
	partialsControlLeft_ = 0;
}

void OscVoice::setSampleRate(double hostSampleRate) {
	hostSampleRate_ = hostSampleRate;
	glideCoeffDirty_ = true;
	dampCount_ = 0; // Damp's per-block factors read the rate: recompute them (they are keyed on the scale, not on it)
	// THE STRING IS SIZED HERE, at activate, for C0 at this rate (KS design K13): it allocates, so never again on
	// the audio thread, and only when the rate moved.
	if (ks_->string.capacity() == 0 || ks_->string.sampleRate() != hostSampleRate) {
		ks_->string.prepare(hostSampleRate);
	}
	ks_->hz = -1.0;
	ks_->lawsSet = false;
	// THE DRUM HIT borrows the voice's string for its String kind (and the synth's pluck builder, handed with setPluckBuilder).
	drum_->hit.prepare(hostSampleRate);
	drum_->hit.setString(&ks_->string, ks_->pluckBuilder);
	drum_->layer.prepare(hostSampleRate); // never a String: a layer's kinds leave the voice's one string to its pad
}

void OscVoice::setFrequency(double hz) {
	// Frequency is kept as it was handed over, not round-tripped through semitones: a voice that is
	// not gliding plays exactly the number it was given.
	frequencyHz_ = hz;
	pitch_ = hzToPitch(hz);
	pitchTarget_ = pitch_;
	gliding_ = false;
	rampHostSamplesLeft_ = 0.0;
	gridsDirty_ = true;
}

void OscVoice::setPitch(double semitones) {
	pitch_ = semitones;
	pitchTarget_ = semitones;
	frequencyHz_ = pitchToHz(semitones);
	gliding_ = false;
	rampHostSamplesLeft_ = 0.0;
	gridsDirty_ = true;
}

void OscVoice::glideToPitch(double semitones) {
	if (glideSeconds_ <= 0.0) {
		setPitch(semitones);
		return;
	}
	rampHostSamplesLeft_ = 0.0;
	pitchTarget_ = semitones;
	gliding_ = std::abs(pitchTarget_ - pitch_) > kGlideDoneSemitones;
}

void OscVoice::rampToPitch(double semitones, int hostSamples) {
	if (hostSamples <= 0) {
		setPitch(semitones);
		return;
	}
	pitchTarget_ = semitones;
	rampHostSamplesLeft_ = static_cast<double>(hostSamples);
	gliding_ = std::abs(pitchTarget_ - pitch_) > kGlideDoneSemitones;
	if (!gliding_) {
		setPitch(semitones);
	}
}

void OscVoice::setGlideSeconds(double seconds) {
	const double clamped = std::max(0.0, seconds);
	if (clamped == glideSeconds_) {
		// Cheap to check and worth checking: the synth re-asserts this every slice, and taking the
		// long way would dirty the cached coefficient often enough to cost an exp() per sample of
		// every glide.
		return;
	}
	glideSeconds_ = clamped;
	glideCoeffDirty_ = true;
	if (glideSeconds_ <= 0.0 && gliding_ && rampHostSamplesLeft_ <= 0.0) {
		setPitch(pitchTarget_); // Glide switched off mid-flight: arrive rather than hang.
	}
}

void OscVoice::reset() {
	phase_ = 0.0;
	phaseCatchup_ = 0.0;
	// The modulator too — a reset voice is a new note, and §2 measured what a modulator that survives
	// one costs: notes differing only in its phase come out 2.40 apart against a 2.0 swing.
	fmPhase_ = 0.0;
	fadeTotal_ = 0;
	fadeLeft_ = 0;
	carryCount_ = 0;
	carryRead_ = 0;
	decimator_.reset();
	decimatorR_.reset();
	stereoStream_ = false;
	readPrimed_ = false; // a new note starts AT its read offset, not on a ramp from the last note's
	noise_.seed(noiseSeed_);
	noiseHold_ = 0;
	// A reset voice has no history to fade from, so the next sync adopts the bank's table outright.
	haveSynced_ = false;
	bankMoved_ = false;
	gridsDirty_ = true;
	voxBakeDue_ = 0;
	// A RESET VOICE IS A NEW NOTE. The keys name a character and a note, and the character pointers
	// survive a reset — so without this a voice handed to a different note could match a key baked
	// for the note it used to play. Cheap, and the one way a pointer key can go stale while still
	// comparing equal.
	invalidateVoxBakes();
	// THE NOTE-ON OF THE PARTIALS ROAD (design §5.2): the bank is reset at the first control block, when the note's
	// pitch is known (the synth sets it after this), to Schroeder phases weighted by that block's targets (§2.4), and
	// its amplitudes snap to them. The decimators are reset above, so nothing of theirs is idle any more; the phase is
	// zero, so nothing is owed to it.
	partialsPrimed_ = false;
	partialsControlLeft_ = 0;
	decimatorsIdle_ = false;
	partialsOwed_ = 0;
	partialsNoteOn_ = true; // the next priming is a note-on's: it starts at the accumulator's phase (backlog LY)
	// THE STRING STARTS SILENT AND UNRELEASED; the synth plucks it (pluckString) after this, once the note's pitch is
	// set. A string already idle is all zeros, so it is not cleared again (a non-KS note-on pays nothing for it).
	if (!ks_->string.idle()) {
		ks_->string.reset();
	}
	ks_->pluckPending = false;
	ks_->released = false;
	ks_->releaseApplied = false;
	ks_->controlLeft = 0;
	ks_->lawsSet = false;
	ks_->hz = -1.0;
	ks_->pickL = -1.0;
	ks_->pickR = -1.0;
	// THE DRUM HIT: a note-on is a strike, so nothing of the last one carries over.
	if (!drum_->hit.idle()) {
		drum_->hit.reset();
	}
	if (!drum_->layer.idle()) {
		drum_->layer.reset();
	}
	drum_->layerPending = false;
	drum_->strikePending = false;
	drum_->released = false;
	drum_->releaseApplied = false;
	drum_->controlLeft = 0;
}

void OscVoice::invalidateVoxBakes() {
	for (auto& slot : voxGrids_) {
		for (VoxGrid& grid : slot) {
			grid.key = VoxBakeKey {};
		}
	}
}

void OscVoice::setNoiseSeed(uint32_t seed) {
	noiseSeed_ = seed;
	noise_.seed(seed);
}

bool OscVoice::syncToBank(int oversampling) {
	if (bank_ == nullptr || !bank_->hasTable()) {
		return false;
	}
	if (haveSynced_ && !bankMoved_ && bank_->generation() == syncedGeneration_) {
		return true;
	}
	// A MOVE BETWEEN BANKS (`moveToBank`) IS A FLIP like any other, whatever the two banks' generations happen to be.
	const bool movedBank = bankMoved_;
	bankMoved_ = false;
	// A NEW TABLE STARTS A NEW PARTIALS CONTROL BLOCK, here at the flip: a Partials side's new spectrum (and its fade)
	// then begins at the flip and lasts exactly the bank's fade length, inside the guard that keeps the stale slot from
	// being rebuilt while it is read (see partialsControlLeft_). No other road reads it.
	// AND A BLOCK CUT SHORT KEEPS THE PHASE MOVE IT MADE, NO MORE: a folded Phase ramp turns the partials by Δ over the
	// whole block, and the block set partialsOffset_ to its END. Cut after m of its L samples, only m/L of Δ has turned, so
	// the offset the phases carry is put back to start + Δ·m/L and the next block folds the rest (L the block's length:
	// 32, or less for the short block that aligns a note-on or a flip to the synth's tick). Without it every flip
	// at a chunk boundary (a tick cap, a host cut: the ordinary case) would lose part of Δ for good, and two oscillators
	// with Phase modulated during puck motion would drift apart in relative phase. (A stepped move turned all of Δ.)
	if (partialsControlLeft_ > 0 && partialsBlockDelta_ != 0.0) {
		partialsOffset_ = partialsBlockStart_
		                  + partialsBlockDelta_ * static_cast<double>(partialsBlockLength_ - partialsControlLeft_)
		                        / static_cast<double>(partialsBlockLength_);
	}
	// AND THE EARS' DIFFERENCE, the same way (backlog OZ): the right ear's ramp turned m/L of its Δ.
	if (partialsControlLeft_ > 0 && partialsEarDelta_ != 0.0) {
		partialsEar_ = partialsEarStart_
		               + partialsEarDelta_ * static_cast<double>(partialsBlockLength_ - partialsControlLeft_)
		                     / static_cast<double>(partialsBlockLength_);
	}
	partialsControlLeft_ = 0;

	stale_ = fresh_;
	fresh_ = &bank_->current(0);
	// The right ear's pointers ride every sync, mono renders included — pointer writes only, so the
	// mono path's arithmetic is untouched. On a mono build the bank answers with ear 0's table for
	// ear 1, so these are the same pointers and a later stereo render finds them already right.
	// THE RIGHT EAR'S POINTERS RIDE EVERY SYNC, MONO RENDERS INCLUDED, AND THAT IS LOAD-BEARING
	// RATHER THAN TIDY. Refreshing them unconditionally looks like a convenience — pointer writes
	// only, so the mono path's arithmetic is untouched — and making it conditional on stereoStream_
	// is an obvious-looking optimisation that would break something else entirely: the equality
	// tests below (`freshR_ == fresh_`, `staleR_ == stale_`) would then be comparing a pointer read
	// THIS sync against one left over from an older one, which is the cross-time comparison the bake
	// cache's ABA bug was made of. Kept unconditional so that every pointer in those tests is
	// contemporaneous. See the note at the stale-side comparison in refreshGridsWithVox.
	staleR_ = freshR_;
	freshR_ = &bank_->current(1);
	gridsDirty_ = true;
	// A new character is a new vowel: the standing bake is for a place this voice has left, so the
	// next sample rebakes rather than waiting out the cadence.
	voxBakeDue_ = 0;

	if (haveSynced_) {
		// THE SWAP, not a rebake (the vox road's double buffer). What was fresh is now what the
		// fade mixes OUT of, and it was baked last chunk — so the slot index flips and that bake is
		// banked as the stale side. Only the incoming character is baked below.
		voxSlot_ = 1 - voxSlot_;
		// AND AFTER A MOVE BETWEEN BANKS THE SLOT NOW FRESH IS FORGOTTEN: its bake is keyed on the OTHER bank's build
		// serial, and serials are per bank, so a coincidence could otherwise pass it off as this bank's. The slot now
		// stale keeps the old bank's current bake, which is exactly what the fade mixes out.
		if (movedBank) {
			for (VoxGrid& grid : voxGrids_[voxSlot_]) {
				grid.key = VoxBakeKey {};
			}
		}
		// A HELD-OFF BAKER NEVER MEETS AN EMPTY FRESH SLOT (voice stacking's bake budget): an empty slot would have to bake
		// now, budget or not, and every part seated on a stand before its first sync has one. It takes a COPY of the bake
		// it was just playing instead — its last real bake, which is what a deferral promises — keyed so that it never
		// matches a real key, so the part rebakes the moment its turn comes.
		if (voxDeferred_) {
			for (int ear = 0; ear < 2; ++ear) {
				VoxGrid& now = voxGrids_[voxSlot_][ear];
				const VoxGrid& was = voxGrids_[1 - voxSlot_][ear];
				if (now.key.buildSerial == 0 && was.key.buildSerial != 0) {
					now = was;
					now.key.note = kVoxCopiedNote;
				}
			}
		}
		// The fade is the bank's length, not this block's, restarted from the top. Within ONE bank that cannot
		// interrupt a fade: the bank will not flip while any voice can still be fading, which is the invariant that
		// replaced "a fade finishes inside the call that armed it" when fades were allowed to span blocks.
		// BUT `moveToBank` (a stacked part stepping onto a stand, or back) IS REACHABLE MID-FADE, because it is two banks:
		// then what was fresh becomes stale and the side that was fading out is dropped in one sample, a step of (1 − t)
		// of it. On the Partials road the same move also changes the road: an interrupted MIXED fade (one side Partials)
		// whose new pair is Partials -> Partials goes PURE, so the bank's output jumps from its weight t to 1 at once;
		// one whose new pair is Partials -> other stays MIXED with the bank's weight restarting near 1 — a step of the
		// same (1 − t) order as every other engine's, and the price of a move the fade guard cannot see.
		fadeTotal_ = bank_->fadeLengthSamples() * oversampling;
		fadeLeft_ = fadeTotal_;
	}
	else {
		// Nothing to fade from. `stale_` is left pointing at the live table rather than at nothing, so
		// the sample loop never has a null to test for.
		stale_ = fresh_;
		staleR_ = freshR_;
		haveSynced_ = true;
	}
	// THE MODULATOR'S RATE, refreshed with the table that carries it — the FM interval is per SOUND
	// (FM doc §6's state split) and the table IS the sound, so this is where it arrives. Held rather
	// than read per sample so that advancePhase stays one multiply and so a bank flip cannot change
	// the rate between two samples of one block.
	fmRatio_ = static_cast<double>(fresh_->fmFrequencyRatio());
	syncedGeneration_ = bank_->generation();
	return true;
}

void OscVoice::bakeVoxInto(const PreparedCharacter& c, VoxGrid& into) {
	// A KILOBYTE OF STACK, not an allocation: the cycle is scratch that the grids replace, so
	// nothing has to keep it and rule V10b — which is about the heap — is untouched.
	VoxCycle cycle;
	bakeVoxCycle(c.vox.params, voxBakeHz(), cycle);

	// An unvoiced cycle at a position the bank called audible means the FUNDAMENTAL is not a note:
	// bakeVoxCycle's only other refusal is a gain-free recipe, and `audible` has already excluded that. Bake the vowel-free cycle rather than blending toward a flat line, which keeps "audible
	// iff this voice has a grid" true with no second condition anywhere.
	const FlagshipSource source =
	    cycle.voiced ? FlagshipSource { &cycle, c.vox.presence } : FlagshipSource {};

	// The bank's recipe, copied so the staircase can be RE-DERIVED against the blend. This is the
	// half that is easy to miss and impossible to hear as a bug: `held` is the chain sampled at the
	// step centres, and the bank sampled it at character-build time on the vowel-free cycle. Reusing
	// it would quantize the blended cycle onto a staircase built from a waveform nobody plays — the
	// final combine drags the output back toward those held values, so the vowel would come out
	// attenuated by (1 − quantize) and vanish entirely at quantize 1. Plausible sound, half-applied
	// blend, nothing failing.
	++voxBakes_;
	// ONE PASS FOR ALL THREE RUNGS. A first draft called the single-mip bake three times, which
	// rebuilt the base from scratch each time and ran the operator chain 768 times for an answer
	// that needs 256 — measured as most of a sixteen-voice drag's cost. Same grids, to the bit.
	// AND THE PREPARE'S NORMALISATION PROBE IS TAKEN OFF THE SAME 256 (backlog LZ): it evaluated the
	// cycle again at 128 of those phases, a third of every bake. Same recipe and grids, to the bit.
	FlagshipRecipe r = c.recipe;
	// AND THE SMOOTH SOURCE IS THE BANK'S (backlog PB): note-free, so read once per build for every voice, and read
	// off `c` itself — the table being baked — so it is this build's and no other's. Same grids, to the bit.
	prepareAndBakeFlagshipLadder(r, source, into.slots[0], into.slots[1], into.slots[2], &c.smooth);
	into.key = VoxBakeKey { c.buildSerial, voxBakeNoteKey(voxBakeHz()) };
}

void OscVoice::bakeVoxIfStale(const PreparedCharacter& c, VoxGrid& into, bool outgoing) {
	const VoxBakeKey want { c.buildSerial, voxBakeNoteKey(voxBakeHz()) };
	if (into.key == want) {
		// A parked note, which is what almost every voice is almost all the time: the character has
		// not moved and neither has the note, so the grids already on this voice ARE the answer.
		return;
	}
	// A DEFERRED REBAKE (voice stacking's own-stand budget, `setVoxDeferred`) keeps the last REAL bake it has — never
	// an empty grid: a side with no bake at all bakes now, whatever the budget says.
	// AND A BUDGETED BAKER'S OUTGOING SIDE never spends a turn: it is heard for one fade (kCharacterFadeSeconds), fading out, and an older bake of it is
	// no worse than the fade itself — so one turn is one bake, of the side that stays.
	if ((voxDeferred_ || (outgoing && voxBudgeted_)) && into.key.buildSerial != 0) {
		return;
	}
	bakeVoxInto(c, into);
}

double OscVoice::voxBakeHz() const {
	// Zero is every unstacked voice, and it returns the very number it always baked at.
	return voxBakeOffset_ == 0.0 ? frequencyHz_ : frequencyHz_ * std::exp2(-voxBakeOffset_ / 12.0);
}

void OscVoice::updateVoxGrids() {
	voxBakeDue_ = kVoxBakeIntervalSamples;
	// The ordinary case, and the one the identity claim is about: nothing this voice can read has a
	// throat in it, so there is no road to be on. Four flag loads per interval, and — the invariant
	// the pin leans on — this branch writes no float the sample loop reads and does not dirty the
	// grids.
	if (!fresh_->vox.audible && !stale_->vox.audible && !freshR_->vox.audible
	    && !staleR_->vox.audible) {
		voxLive_ = false;
		return;
	}
	// Settled before the bake because the bake needs it. All three rungs are baked, so this only
	// decides what the sample loop READS — but it has to be current, because a glide moves it.
	stairMip_ = stairMipForFrequency(frequencyHz_);
	const uint32_t bakesBefore = voxBakes_;

	// A SIDE THE LENDER HAS BAKED IS NOT BAKED HERE (voice stacking, §3.10) — `lentSide` says which, and the grid
	// resolve asks the same function, so the two cannot disagree about whose grid a side reads.
	// BOTH SIDES OF THE FADE, and the cache is what makes that affordable. A voice crossing from one
	// vocal character to another reads the outgoing bake and the incoming one at once; leaving the
	// outgoing side stale would make every character change an audible vowel swell across the 2 ms
	// crossfade. In practice the swap has usually already banked it, so its key still matches and
	// nothing is recomputed.
	if (fresh_->vox.audible && !lentSide(0)) {
		bakeVoxIfStale(*fresh_, voxGrids_[voxSlot_][0]);
	}
	// ONLY WHILE A FADE IS ACTUALLY RUNNING. The stale side is read by earSample exactly when
	// fadeLeft_ is positive; outside that window stale_ still points at the character this voice
	// faded out of, and rebaking it every time the note moved would be a second bake per glide step
	// for a grid nobody reads.
	const bool fading = fadeLeft_ > 0;
	if (fading && stale_ != fresh_ && stale_->vox.audible && !lentSide(1)) {
		bakeVoxIfStale(*stale_, voxGrids_[1 - voxSlot_][0], true);
	}
	// The second ear only when it is a second CHARACTER — pointer inequality, the bank's own rule,
	// tested PER PAIR rather than against one width flag, because during a mono/stereo transition
	// exactly one of the two pairs is unequal.
	if (freshR_ != fresh_ && freshR_->vox.audible && !lentSide(2)) {
		bakeVoxIfStale(*freshR_, voxGrids_[voxSlot_][1]);
	}
	if (fading && staleR_ != freshR_ && staleR_ != stale_ && staleR_->vox.audible && !lentSide(3)) {
		bakeVoxIfStale(*staleR_, voxGrids_[1 - voxSlot_][1], true);
	}
	// ONE TURN IS ONE ROUND OF BAKES (the own-stand budget, §3.11): a budgeted baker that has just rebaked holds what it
	// has until the budget's next pass hands it a turn again. Without this a turn spanned every flip of its stand until
	// that pass, and a stand flipping during it (a deferred part catching up, then its stand moving on) baked twice on
	// one grant — found when the rebuild interval grew to 256 samples (backlog PB (c)) and two flips fit in one turn.
	if (voxBudgeted_ && voxBakes_ != bakesBefore) {
		voxDeferred_ = true;
	}
	voxLive_ = true;
	gridsDirty_ = true;
}

bool OscVoice::lentSide(int side) const {
	if (voxLender_ == nullptr) {
		return false;
	}
	const OscVoice& l = *voxLender_;
	// Side 0 fresh, 1 stale, 2 fresh right, 3 stale right. THE LENDER COVERS A SIDE when it is reading the very same
	// table there (so the same bank and the same build) and holds a bake of it:
	// - the FRESH side, any bake it has — it may be an older one the budget has held (`setVoxDeferred`), and then this
	//   voice plays the same older vowel the lender plays, rather than baking one of its own;
	// - the STALE side, only a bake of exactly that build: it was banked at the flip, and a lender that was not
	//   fading then never baked it.
	const PreparedCharacter* mine = side == 0 ? fresh_ : side == 1 ? stale_ : side == 2 ? freshR_ : staleR_;
	const PreparedCharacter* theirs = side == 0 ? l.fresh_ : side == 1 ? l.stale_ : side == 2 ? l.freshR_ : l.staleR_;
	if (mine != theirs || mine == nullptr) {
		return false;
	}
	const bool freshSide = side == 0 || side == 2;
	const int slot = freshSide ? l.voxSlot_ : 1 - l.voxSlot_;
	const VoxGrid& bake = l.voxGrids_[slot][side >= 2 ? 1 : 0];
	return freshSide ? bake.key.buildSerial != 0 : bake.key.buildSerial == mine->buildSerial;
}

const float* OscVoice::gridForSide(const PreparedCharacter& c, const VoxGrid& bake) const {
	return c.vox.audible ? bake.mip(stairMip_) : c.gridFor(stairMip_);
}

void OscVoice::refreshGridsWithVox() {
	// EACH SIDE FROM WHOEVER BAKED IT (voice stacking, §3.10): the lender's slot where `lentSide` says it covers the side,
	// read at the lender's CURRENT slot index — never one remembered — and this voice's own otherwise.
	const auto grid = [this](int side) -> const VoxGrid* {
		const bool freshSide = side == 0 || side == 2;
		const OscVoice& baker = lentSide(side) ? *voxLender_ : *this;
		return &baker.voxGrids_[freshSide ? baker.voxSlot_ : 1 - baker.voxSlot_][side >= 2 ? 1 : 0];
	};
	// EVERY POINTER-EQUALITY BRANCH HERE IS A SIDE THAT WAS NEVER BAKED SEPARATELY, and each is the
	// same equality the bank and the sync already lean on: a voice that has nothing to fade from
	// has stale_ == fresh_, a mono build answers ear 1 with ear 0's table, and a voice on its first
	// sync has both at once. Resolving those by equality rather than by baking again is what keeps
	// the bake count at one per voice per chunk.
	stairGrid_ = gridForSide(*fresh_, *grid(0));
	// AND `stale_ == fresh_` IS AN ADDRESS COMPARISON THAT IS SAFE ONLY BECAUSE A VOICE CANNOT MISS
	// A GENERATION. It is the same ABA shape that shipped as a bug in the bake cache — two slots, an
	// address reused — and what saves it here is that these pointers are read together in the same
	// sync rather than one of them being remembered from an earlier one.
	//
	// WHAT KEEPS A GENERATION UNMISSABLE, since nothing else states it: the bank's guard runs for
	// lround(0.002 * rate) + 4 host samples after a flip, and a voice can be holding at most
	// Halfband4x::kMaxBlock finished samples before it syncs. The guard therefore has to exceed the
	// carry, which it does while 0.002 * rate + 4 > 32 — that is, ABOVE ABOUT 14 kHz. Every rate a
	// host offers clears it by a wide margin, but setSampleRate's own comment claims robustness at
	// absurd rates, so the code advertises a range wider than this invariant covers. Written here
	// rather than there because this is the line that would break.
	stairGridStale_ = (stale_ == fresh_) ? stairGrid_ : gridForSide(*stale_, *grid(1));
	if (stereoStream_) {
		stairGridR_ = (freshR_ == fresh_) ? stairGrid_ : gridForSide(*freshR_, *grid(2));
		stairGridStaleR_ = (staleR_ == freshR_)  ? stairGridR_
		                   : (staleR_ == stale_) ? stairGridStale_
		                                         : gridForSide(*staleR_, *grid(3));
	}
	gridsDirty_ = false;
}

void OscVoice::refreshGrids() {
	// Resolved from the fundamental, not from the oversampled rate: the mip is a statement about the
	// note. The original fixed-point Stair renderer picks the same way, and switches hard when it changes.
	stairMip_ = stairMipForFrequency(frequencyHz_);
	// AND THE STRING'S, by the same thresholds and at the same moment — here, ahead of the vowel road's
	// early return, because refreshGrids is what every frequency change (note start, setPitch, each glide
	// step) dirties its way into. A Weave side never takes the vowel road, so this is its only door.
	weaveMip_ = WeaveString::mipForFrequency(frequencyHz_);
	// AND THE SHAPE LADDER'S, from the highest instantaneous frequency FM can drive this note to. Every side a
	// fade or a stereo stream may read is asked, and the deepest wins, so neither half of a fade can alias.
	// The limit is the host rate — half the 4x Nyquist — which is what makes a switch between octave-spaced
	// mips move only partials the decimator removes (basic_shapes.h).
	{
		const auto topHz = [this](const PreparedCharacter* side) {
			// Only a Shapes side reads a ladder (a Partials side's partials are faded at Nyquist by the bank, and it has no FM).
			if (side == nullptr || side->engine != OscEngine::Shapes) {
				return 0.0;
			}
			const double index = static_cast<double>(side->fmIndexCyclesFor(frequencyHz_));
			const double ratio = static_cast<double>(side->fmFrequencyRatio());
			// AND THE WARP'S STEEPEST READ RATE (backlog KV): the warp bends the FM'd phase, so the two multiply. Exactly
			// 1 with the warp off, so an unwarped voice picks the mip it always did.
			const double warp = warpMaxSlope(side->recipe.warpMode, side->recipe.warpDepth);
			return frequencyHz_ * (1.0 + 2.0 * std::numbers::pi * index * ratio) * warp;
		};
		double top = std::max(topHz(fresh_), topHz(stale_));
		if (stereoStream_) {
			top = std::max(top, std::max(topHz(freshR_), topHz(staleR_)));
		}
		shapesMip_ = shapesMipFor(top, hostSampleRate_);
	}
	if (voxLive_) {
		refreshGridsWithVox();
		return;
	}
	// Both tables, because a fading voice evaluates both — and EACH SIDE PICKS ITS OWN grid, because
	// a fade can be running from a stair table into an operated flagship one, or between any other
	// pair. The addresses are the bank's and a rebuild only rewrites their contents, so nothing here
	// outlives its target.
	stairGrid_ = fresh_->gridFor(stairMip_);
	stairGridStale_ = stale_->gridFor(stairMip_);
	if (stereoStream_) {
		// Only while a stereo stream runs: this resolves per evaluation sample during a glide, and
		// the mono path — every patch at width 0 — is not asked to pay for an ear it isn't playing.
		stairGridR_ = freshR_->gridFor(stairMip_);
		stairGridStaleR_ = staleR_->gridFor(stairMip_);
	}
	gridsDirty_ = false;
}


void OscVoice::stepReadOffset(int oversampling) {
	// A NEW HOST SAMPLE: its value is the target the next `oversampling` evaluations ramp to, from the last one's.
	if (readSub_ == 0) {
		float next = readConstant_;
		if (readRun_ != nullptr && readRunLength_ > 0) {
			next = readRun_[std::min(readIndex_, readRunLength_ - 1)];
		}
		// AND THE EARS' DIFFERENCE, on the same counters (setReadPhaseEar).
		float ear = earConstant_;
		if (earRun_ != nullptr && earRunLength_ > 0) {
			ear = earRun_[std::min(readIndex_, earRunLength_ - 1)];
		}
		if (!readPrimed_) {
			readTo_ = next; // a fresh note starts AT its offset
			earTo_ = ear;
			readPrimed_ = true;
		}
		readFrom_ = readTo_;
		readTo_ = next;
		earFrom_ = earTo_;
		earTo_ = ear;
	}
	if (readFrom_ == 0.0f && readTo_ == 0.0f && readWidth_ == 0.0f && earFrom_ == 0.0f && earTo_ == 0.0f) {
		readOffset_ = 0.0; // the off road: nothing is added below, so nothing changes
		readOffsetR_ = 0.0;
	}
	else {
		const double along = static_cast<double>(readSub_ + 1) / static_cast<double>(oversampling);
		const double x = static_cast<double>(readFrom_)
		                 + static_cast<double>(readTo_ - readFrom_) * static_cast<double>(readSub_ + 1)
		                       / static_cast<double>(oversampling);
		// WRAPPED, and the ramp is taken BEFORE the wrap: 0.9 → 1.1 passes through 1.0 as 0.0, the same place on
		// the cycle, so a route carrying the offset past either end is continuous in the sound.
		readOffset_ = x - std::floor(x);
		// THE RIGHT EAR, MIRRORED (Q10): base + m·(1 − 2w), with m = x − base — the modulation, not the knob. AND THE
		// STEREO ROUTES' EAR DIFFERENCE ADDED (backlog KU): R = base + m·(1 − 2w) + (mR − mL), ramped like x.
		double right = x - 2.0 * static_cast<double>(readWidth_) * (x - static_cast<double>(readBase_));
		if (earFrom_ != 0.0f || earTo_ != 0.0f) {
			right += static_cast<double>(earFrom_) + static_cast<double>(earTo_ - earFrom_) * along;
		}
		readOffsetR_ = right - std::floor(right);
	}
	if (++readSub_ >= oversampling) {
		readSub_ = 0;
		++readIndex_;
	}
}

void OscVoice::stepWeaveFade(int oversampling) {
	if (weaveFades_ == nullptr || weaveFadeCount_ <= 0) {
		currentFade_ = nullptr;
		return;
	}
	currentFade_ = &weaveFades_[std::min(weaveIndex_, weaveFadeCount_ - 1)];
	// The right ear's tap at this host sample: `from`, walked toward the target at the fixed rate.
	const double reach = weaveTapRate_ * static_cast<double>(weaveIndex_);
	currentTap_ = weaveTapFrom_ + std::clamp(weaveTapTo_ - weaveTapFrom_, -reach, reach);
	if (++weaveSub_ >= oversampling) {
		weaveSub_ = 0;
		++weaveIndex_;
	}
}

float OscVoice::weaveSample(double phase) const {
	if (weave_ == nullptr || currentFade_ == nullptr) {
		return 0.0f;
	}
	// The travel is the reader's to add (WeaveString::sample): the ring rotating under this voice's scan
	// head, the same rotation under every voice.
	return weave_->sample(*currentFade_, phase + static_cast<double>(currentFade_->travel), weaveMip_);
}
void OscVoice::advanceGlide(int oversampling) {
	// A RAMP (`rampToPitch`) CLOSES AN EVEN SHARE OF WHAT REMAINS each evaluation sample — a straight line that lands on
	// its last sample, at any oversampling.
	if (rampHostSamplesLeft_ > 0.0) {
		const double step = 1.0 / static_cast<double>(oversampling);
		if (rampHostSamplesLeft_ <= step) {
			pitch_ = pitchTarget_;
			rampHostSamplesLeft_ = 0.0;
			gliding_ = false;
		}
		else {
			pitch_ += (pitchTarget_ - pitch_) * (step / rampHostSamplesLeft_);
			rampHostSamplesLeft_ -= step;
		}
		frequencyHz_ = pitchToHz(pitch_);
		gridsDirty_ = true;
		return;
	}
	// One-pole approach with the time constant in seconds: after t seconds the remaining distance is
	// down by e^-1, so the ~3 tau a listener calls "arrived" is three times the glide time. The
	// coefficient is cached because its inputs — glide time, sample rate, oversampling — all change
	// between notes at the fastest, and this runs per evaluation sample.
	if (glideCoeffDirty_ || glideCoeffOversampling_ != oversampling) {
		const double evalRate = hostSampleRate_ * static_cast<double>(oversampling);
		glideCoeff_ = 1.0 - std::exp(-1.0 / (glideSeconds_ * evalRate));
		glideCoeffOversampling_ = oversampling;
		glideCoeffDirty_ = false;
	}
	pitch_ += (pitchTarget_ - pitch_) * glideCoeff_;
	if (std::abs(pitchTarget_ - pitch_) <= kGlideDoneSemitones) {
		pitch_ = pitchTarget_;
		gliding_ = false;
	}
	frequencyHz_ = pitchToHz(pitch_);
	gridsDirty_ = true;
}

void OscVoice::advanceGlideBy(int hostSamples) {
	const double n = static_cast<double>(hostSamples);
	// THE RAMP: advanceGlide closes (step / left) of what remains each step, which is a straight line in the samples, so
	// one step of n host samples lands where n steps of one (or 4n of a quarter) do.
	if (rampHostSamplesLeft_ > 0.0) {
		if (rampHostSamplesLeft_ <= n) {
			pitch_ = pitchTarget_;
			rampHostSamplesLeft_ = 0.0;
			gliding_ = false;
		}
		else {
			pitch_ += (pitchTarget_ - pitch_) * (n / rampHostSamplesLeft_);
			rampHostSamplesLeft_ -= n;
		}
		frequencyHz_ = pitchToHz(pitch_);
		gridsDirty_ = true;
		return;
	}
	// THE ONE-POLE: each of advanceGlide's steps keeps (1 − c) of the distance, c = 1 − exp(−1/(τ·evalRate)), so n
	// host samples (4n evaluations at 4x) keep exp(−n/(τ·hostRate)) of it, whatever the oversampling. The same done test.
	const double keep = std::exp(-n / (glideSeconds_ * hostSampleRate_));
	pitch_ = pitchTarget_ - (pitchTarget_ - pitch_) * keep;
	if (std::abs(pitchTarget_ - pitch_) <= kGlideDoneSemitones) {
		pitch_ = pitchTarget_;
		gliding_ = false;
	}
	frequencyHz_ = pitchToHz(pitch_);
	gridsDirty_ = true;
}

void OscVoice::advancePhaseBy(double increment, int hostSamples) {
	const double n = static_cast<double>(hostSamples);
	if (phaseCatchup_ != 0.0) {
		// advancePhase moves the error by error·inc/16 a step: after n steps (1 − inc/16)^n of it remains.
		const double moved = phaseCatchup_ * (1.0 - std::pow(1.0 - increment * (1.0 / 16.0), n));
		phase_ += moved;
		phaseCatchup_ -= moved;
		if (std::abs(phaseCatchup_) < 1e-9) {
			phase_ += phaseCatchup_;
			phaseCatchup_ = 0.0;
		}
	}
	phase_ += increment * n;
	phase_ -= std::floor(phase_);
	fmPhase_ += increment * n * fmRatio_;
	fmPhase_ -= std::floor(fmPhase_);
}

void OscVoice::advancePhase(double increment) {
	// THE §5.8 CATCHUP, the firmware HARM arithmetic in this voice's double phase (harm.h:392): a
	// retrigger's outstanding error bleeds in at error x increment / 16 per sample — an exponential
	// with a 16-cycle time constant, ~24 cycles to substantial convergence, a brief pitch
	// inflection whose peak detune at a half-cycle error is increment/32 (~3%), decaying. The floor
	// finishes the tail outright where the firmware's +-1 LSB floor would: convergence COMPLETES in
	// bounded samples rather than asymptoting forever, which is what the completion pin measures.
	// With no error outstanding this is one compare — and the OFF path never banks an error, so a
	// patch with catchup off renders through arithmetic identical to the day before this existed.
	if (phaseCatchup_ != 0.0) {
		constexpr double kDone = 1e-9; // cycles — the double-world +-1 LSB
		double correction = phaseCatchup_ * increment * (1.0 / 16.0);
		phase_ += correction;
		phaseCatchup_ -= correction;
		if (std::abs(phaseCatchup_) < kDone) {
			phase_ += phaseCatchup_;
			phaseCatchup_ = 0.0;
		}
	}
	phase_ += increment;
	if (phase_ >= 1.0 || phase_ < 0.0) {
		phase_ -= std::floor(phase_);
	}
	// THE MODULATOR RUNS AT ITS OWN RATE, from its own accumulator — which is the whole reason it is
	// an accumulator and not a multiple of the carrier's phase. The engine's existing "ratio" (sync,
	// flagship.cpp) reads the source at `p * ratio`, making the modulator a FUNCTION of the carrier:
	// bit-identical at integer ratios, and at 2/3 an 8.6x jump in peak sample step — 0.674 of full
	// scale in one sample at 48 kHz (FM doc §2). A subharmonic composite's period is q carrier
	// cycles, and a phase that wraps every one cannot express it.
	//
	// ADVANCED UNCONDITIONALLY, not behind a depth test, so that turning the knob up mid-note joins a
	// modulator that has been running all along rather than one that starts from zero under the hand.
	// It is one multiply and one wrap.
	fmPhase_ += increment * fmRatio_;
	if (fmPhase_ >= 1.0 || fmPhase_ < 0.0) {
		fmPhase_ -= std::floor(fmPhase_);
	}
}

float OscVoice::nextSample(int oversampling) {
	if (gliding_) {
		advanceGlide(oversampling);
	}
	stepWeaveFade(oversampling);
	stepReadOffset(oversampling);
	// THE BAKE, on its own schedule in generated samples — see kVoxBakeIntervalSamples. One
	// decrement and a compare for a voice with no vowel anywhere near it, which is most of them.
	if (--voxBakeDue_ <= 0) {
		updateVoxGrids();
	}
	if (gridsDirty_) {
		refreshGrids();
	}

	// THE READ, AT THE ACCUMULATOR PLUS PHASE'S OFFSET (setReadPhase) — zero takes no branch, and a Weave side
	// reads the string, which has no cycle for an offset to move. (A Partials fresh side reaches this line only inside an
	// engine-switch fade, where the read is the OTHER side's and the offset is that side's to honour; the bank turns
	// its own phases by the offset at control rate, controlPartials.)
	const double phase = readOffset_ != 0.0 && fresh_->engine != OscEngine::Weave ? phase_ + readOffset_ : phase_;
	lastReadPhase_ = phase; // the seam a case reads: the phase the tables were READ at, not the accumulator
	advancePhase(frequencyHz_ / (hostSampleRate_ * static_cast<double>(oversampling)));

	// Unconditionally, whether or not this table uses the draws: the stream then depends only on how
	// many samples have been rendered, so a zone that turns the modifiers on mid-note gets the same
	// noise it would have got had they been on all along, and a fade between an on table and an off
	// one cannot make the two sides disagree about where in the stream they are.
	if (noiseHold_ <= 0) {
		heldJitter_ = noise_.next();
		heldGrit_ = noise_.next(); // Jitter first, then grit, the order the original fixed-point Morph renderer drew them in.
		noiseHold_ = oversampling;
	}
	noiseHold_--;

	const bool fading = fadeLeft_ > 0;

	// Linear, and reaching exactly 1 on the fade's last sample, so what follows is the new table alone
	// rather than the new table plus a sliver of the old.
	double t = 1.0;
	if (fading) {
		t = static_cast<double>(fadeTotal_ - fadeLeft_ + 1) / static_cast<double>(fadeTotal_);
		fadeLeft_--;
	}

	// The modifier depths ride the same ramp as the waveforms. They have to move continuously across
	// a rebuild for the same reason the waveform does: zone position 0.25 of zone 0 carries a third of
	// full jitter and its neighbours carry none, so dropping the depth in instantly would be a step in
	// the noise floor exactly where the crossfade is busy hiding a step in the signal.
	float jitterAmt = fresh_->phaseJitter();
	float gritAmt = fresh_->ampNoise();
	if (fading) {
		const float tf = static_cast<float>(t);
		const float staleJitter = stale_->phaseJitter();
		const float staleGrit = stale_->ampNoise();
		jitterAmt = staleJitter + (jitterAmt - staleJitter) * tf;
		gritAmt = staleGrit + (gritAmt - staleGrit) * tf;
	}

	// Off is off: a table with the fields at zero — which most of them are — takes neither branch, so
	// its samples are the ones the tree produced before the modifiers existed, bit for bit.
	double evalPhase = phase;
	if (jitterAmt > 0.0f) {
		evalPhase += jitterOffsetCycles(jitterAmt, heldJitter_);
	}
	// FM, DOWNSTREAM OF THE CYCLE READ AND ON THE SAME SEAM AS JITTER. A depth of zero takes no branch
	// and the samples are the ones this voice produced before FM existed, bit for bit — the same
	// "off is off" the modifiers above are written to.
	//
	// MONO TAKES THE LEFT EAR'S POLARITY, +1, rather than a fold-down of the two: this path renders
	// ONE signal, and a mono renderer that averaged the two modulator polarities would be summing a
	// stereo image nobody asked it to make. At w = 0 the two ears agree anyway, which is what makes
	// that choice invisible where it could matter.
	// THE INDEX CROSSFADES DURING A FADE, as the jitter depth above does (backlog EH, shape 5) — the
	// stereo twin says why.
	float fmIndex = fresh_->fmIndexCyclesFor(frequencyHz_);
	if (fading) {
		const float staleIndex = stale_->fmIndexCyclesFor(frequencyHz_);
		fmIndex = staleIndex + (fmIndex - staleIndex) * static_cast<float>(t);
	}
	lastFmIndex_ = fmIndex;
	if (fmIndex > 0.0f) {
		evalPhase += fmOffsetCycles(fmIndex, fmPhase_, 1.0);
	}

	const float fresh = sideEval(*fresh_, stairGrid_, evalPhase);
	float out = fresh;
	// (A Partials side reads 0 through sideEval, so in an engine-switch fade this mixes the other side by its own share
	// alone, and the Partials road adds the bank's complement.)
	// Both sides on the string read the same string at the same phase — the same value — so a fade between
	// two Weave characters skips the second read (the blend of equal values is that value, to the bit).
	if (fading && !(fresh_->engine == OscEngine::Weave && stale_->engine == OscEngine::Weave)) {
		const double stale = sideEval(*stale_, stairGridStale_, evalPhase);
		out = static_cast<float>(stale + (static_cast<double>(fresh) - stale) * t);
	}

	if (gritAmt > 0.0f) {
		out = applyAmpNoise(out, gritAmt, heldGrit_);
	}
	return out;
}

float OscVoice::earSample(const PreparedCharacter* fresh, const PreparedCharacter* stale,
                          const float* grid, const float* gridStale, double phase, bool fading,
                          double t, double polarity, double weaveTap) const {
	// One ear of nextSamplePair, and the same arithmetic in the same order as the tail of
	// nextSample: depths ride the fade ramp, jitter moves the eval phase, the two tables mix at t,
	// grit lands on the way out. The held draws are the voice's — one listener's random world, read
	// at whatever depth each ear's table carries.
	float jitterAmt = fresh->phaseJitter();
	float gritAmt = fresh->ampNoise();
	if (fading) {
		const float tf = static_cast<float>(t);
		const float staleJitter = stale->phaseJitter();
		const float staleGrit = stale->ampNoise();
		jitterAmt = staleJitter + (jitterAmt - staleJitter) * tf;
		gritAmt = staleGrit + (gritAmt - staleGrit) * tf;
	}

	double evalPhase = phase;
	if (jitterAmt > 0.0f) {
		evalPhase += jitterOffsetCycles(jitterAmt, heldJitter_);
	}
	// FM, with THIS ear's modulator polarity — see fmOffsetCycles. THE INDEX CROSSFADES DURING A FADE
	// AS THE JITTER DEPTH ABOVE DOES (backlog EH, shape 5): this read the fresh table's index alone
	// under a comment claiming the jitter's behaviour, so every character rebuild stepped the carrier's
	// phase offset in one sample — a discontinuity in the tone, not a step in the noise floor.
	float fmIndex = fresh->fmIndexCyclesFor(frequencyHz_);
	if (fading) {
		const float staleIndex = stale->fmIndexCyclesFor(frequencyHz_);
		fmIndex = staleIndex + (fmIndex - staleIndex) * static_cast<float>(t);
	}
	lastFmIndex_ = fmIndex;
	if (fmIndex > 0.0f) {
		evalPhase += fmOffsetCycles(fmIndex, fmPhase_, polarity);
	}

	const float freshOut = sideEval(*fresh, grid, evalPhase, weaveTap);
	float out = freshOut;
	if (fading && !(fresh->engine == OscEngine::Weave && stale->engine == OscEngine::Weave)) {
		const double staleOut = sideEval(*stale, gridStale, evalPhase, weaveTap);
		out = static_cast<float>(staleOut + (static_cast<double>(freshOut) - staleOut) * t);
	}

	if (gritAmt > 0.0f) {
		out = applyAmpNoise(out, gritAmt, heldGrit_);
	}
	return out;
}

void OscVoice::nextSamplePair(float& outL, float& outR) {
	// The pair twin of nextSample, always at 4x — see the header note: the mono loop stays exactly
	// what it was, so the shared-state half here mirrors it line for line and advances everything
	// ONCE for the two ears.
	if (gliding_) {
		advanceGlide(4);
	}
	stepWeaveFade(4);
	stepReadOffset(4);
	if (--voxBakeDue_ <= 0) {
		updateVoxGrids();
	}
	if (gridsDirty_) {
		refreshGrids();
	}

	// The read offset, as nextSample adds it — and the right ear's, which Phase Width mirrors (Q10). At width 0 the
	// two are the same number and both ears read at the same moved phase.
	// (A Partials fresh side reaches here only inside an engine-switch fade: the offset is the other side's, as in
	// nextSample.)
	const bool weave = fresh_->engine == OscEngine::Weave;
	const double phase = readOffset_ != 0.0 && !weave ? phase_ + readOffset_ : phase_;
	const double phaseR = readOffsetR_ != 0.0 && !weave ? phase_ + readOffsetR_ : phase_;
	lastReadPhase_ = phase;
	lastReadPhaseR_ = phaseR;
	advancePhase(frequencyHz_ / (hostSampleRate_ * 4.0));

	if (noiseHold_ <= 0) {
		heldJitter_ = noise_.next();
		heldGrit_ = noise_.next(); // Jitter first, then grit, the order the original fixed-point Morph renderer drew them in.
		noiseHold_ = 4;
	}
	noiseHold_--;

	const bool fading = fadeLeft_ > 0;
	double t = 1.0;
	if (fading) {
		t = static_cast<double>(fadeTotal_ - fadeLeft_ + 1) / static_cast<double>(fadeTotal_);
		fadeLeft_--;
	}

	// THE STEREO FLIP LIVES HERE, as the one place both ears are named together. The left ear is
	// modulated by +I and the right by (1 - 2w)·I — §4's partial flip, which is a continuous width
	// with mono compatibility at w = 0 rather than a switch.
	const double rightPolarity = fmRightPolarityFor(freshR_->fmStereoWidth());
	outL = earSample(fresh_, stale_, stairGrid_, stairGridStale_, phase, fading, t, 1.0);
	outR = earSample(freshR_, staleR_, stairGridR_, stairGridStaleR_, phaseR, fading, t, rightPolarity, currentTap_);
}

void OscVoice::partialTargets(const PreparedCharacter& side, const spectral::GainCache<partials::kMaxPartials>& cache,
                              const spectral::PreparedShaper& shaper, const float* fade, const float* formant, int count,
                              const float* edge, float* target, float* shape) {
	const partials::PartialSpectrum& s = side.partials;
	// THE NEUTRAL DRAWER, which is the default: the terrain's amplitude, nothing read twice and nothing multiplied.
	if (shaper.identity && formant == nullptr) {
		for (int i = 0; i < count; ++i) {
			target[i] = s.envAtRatio[static_cast<size_t>(i)] * s.fine[static_cast<size_t>(i)];
			if (shape != nullptr) {
				shape[i] = target[i]; // the SHAPE, before the edge (Vocode's Flatten divides by it, §9.2)
			}
		}
		if (edge != nullptr) {
			for (int i = 0; i < count; ++i) {
				target[i] *= edge[i]; // the Partials operator's fraction and Shift's low edge (pitchPartials)
			}
		}
		return;
	}
	// Scratch on the stack (see controlPartials for the frame's size). Only [0, count) of each is written and read.
	alignas(16) float raw[partials::kMaxPartials];
	alignas(16) float base[partials::kMaxPartials];
	for (int i = 0; i < count; ++i) {
		const size_t u = static_cast<size_t>(i);
		float env = s.envAtRatio[u];
		// FORMANT SHIFT AT A RATIO OF EXACTLY 1 TAKES THE FAST PATH, not the re-read: for a blend, the blended table read
		// at the blended ratio is not the blend of each sound's reads, so the re-read at no shift would step the sound
		// the moment the switch engaged (Task 5's review). formantRatios writes exactly 1.0f there, so this is an equality.
		if (formant != nullptr && formant[i] != 1.0f) {
			// The UNMOVED ratio, while the shift amount was read at the moved hz: Stretch and Shift carry a partial's
			// amplitude with it (design §3.2), so its envelope reading stays its own; only the shift follows where it
			// sounds.
			env = partials::envelopeRead(s, static_cast<double>(s.ratio[u]) / static_cast<double>(formant[i]));
		}
		raw[i] = env * s.fine[u];
		if (shape != nullptr) {
			shape[i] = raw[i];
		}
		if (edge != nullptr) {
			raw[i] *= edge[i]; // before base: the compensation sees the edges as the bank will render them
		}
		// BASE IS WHAT THE BANK WILL RENDER before the drawer: the Nyquist fade applied, because Tilt's compensation in
		// gains() is computed against it (spectral_shaper.h, the contract on base and count).
		base[i] = raw[i] * fade[i];
	}
	if (shaper.identity) {
		std::copy_n(raw, count, target);
		return;
	}
	// INTO THE VOICE'S OWN GAIN BUFFER, not over `base`: the target needs the amplitude afterwards (quality review,
	// 2026-10-08). THE DRAWER'S FACTORS ARE THE CACHE'S (k = i + 1: partial i is harmonic i + 1, off-grid or not,
	// spectral_shaper.h's contract on k and hz), updated once for this block in controlPartials; this side pays only
	// their product and Tilt's compensation against its own base, which is gains(shaper, k, hz, base) to the bit.
	alignas(16) float gain[partials::kMaxPartials];
	// apply() reads base[0, count), every element of which the loop above wrote; g++ cannot see that through the call and
	// warns, and zero-filling 320 floats per side per block to quiet it would be work for the compiler's sake.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
	cache.apply(base, count, gain);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
	// The bank multiplies by the Nyquist fade itself (setTargets), so it is handed the UNFADED amplitude × the gain, and
	// renders base × gain: the spectrum gains() compensated is the one heard.
	for (int i = 0; i < count; ++i) {
		target[i] = raw[i] * gain[i];
	}
}

static_assert(spectral::PartialsOp::kMaxCount == static_cast<float>(partials::kMaxPartials),
              "the Partials operator's no-op is every partial the bank has");

PartialPitches pitchPartials(const partials::PartialSpectrum& spec, const spectral::ShaperParams& drawer, double f0Hz,
                             double sampleRate, float* inc, float* hz, float* fade, float* ratioScratch,
                             float* edgeScratch, int keepRatios, const partials::PartialSpectrum* otherEar,
                             PeaksGlide* glide, const float* glideFrom, float glideT) {
	PartialPitches out;
	// THE FREQUENCY OPERATORS (design §3.2), the same in both ears, computed as far as the loop below reads them and then on
	// to `keepRatios` (rounded up to a group of four), because the phase control turns every live partial by its ratio,
	// including ones ramping out past the count: Stretch moves partial i (harmonic k = i + 1) to its ratio × √(1 + B·k²),
	// then Shift adds the same Hz to every partial, which is shift / f0 on the ratio. B >= 0 and the shift is one constant,
	// so the ratios still never decrease (the loop below relies on it). A NaN shift is no shift.
	const bool stretch = drawer.enabled && drawer.stretch.on && drawer.stretch.b > 0.0f;
	const float shiftHz = drawer.enabled && drawer.shift.on && std::fabs(drawer.shift.hz) > 0.0f
	                          ? std::clamp(drawer.shift.hz, -spectral::ShiftOp::kMaxHz, spectral::ShiftOp::kMaxHz)
	                          : 0.0f;
	// THE RATIOS GLIDE ACROSS A REBUILD'S FADE (backlog PD): `glideFrom` is the stale side's ratios and `glideT` the fade's t at
	// this block's end, so a partial's frequency walks from the stale ratio to the fresh one as its amplitude does, instead of
	// stepping at the flip. A partial whose two ratios agree reads its ratio to the bit (s + 0·t). Both sides never decrease,
	// and a convex blend of two such sequences never decreases either, so the loop below may still stop on the first silent one.
	const bool gliding = glideFrom != nullptr;
	const bool moved = stretch || shiftHz != 0.0f || gliding;
	const float b = stretch ? std::min(drawer.stretch.b, static_cast<float>(spectral::StretchOp::kMaxB)) : 0.0f;
	const float offset = static_cast<float>(static_cast<double>(shiftHz) / f0Hz);
	const auto sounding = [&](int i) {
		const float k = static_cast<float>(i + 1);
		const float fresh = spec.ratio[static_cast<size_t>(i)];
		const float r = gliding ? glideFrom[i] + (fresh - glideFrom[i]) * glideT : fresh;
		return ratioScratch[i] = (stretch ? r * std::sqrt(1.0f + b * k * k) : r) + offset;
	};
	const float* ratio = moved ? ratioScratch : spec.ratio.data();
	out.ratio = ratio;
	// THE PARTIALS OPERATOR COUNTS BY AUDIBILITY (counts audible partials; refined by the lead in the
	// E/F/G review): each partial counts as its EDGE GAIN — 1 for an ordinary partial, the low edge's fraction for one a
	// downward Shift has faded toward 0 Hz, 0 for one at or below 0 Hz — and the partials run on until they have counted
	// c, the last one carrying the remainder as its gain. Without a shift every edge is 1, so this is ⌈c⌉ partials with the
	// top one at c − ⌊c⌋, as it always was; with one, a partial crossing 0 Hz hands its count to the top partial smoothly,
	// so nothing pops in at the top. Read only while the master switch is on. A NaN count reads as every partial.
	const bool counting = drawer.enabled && drawer.partialCount.on;
	// PEAKS (backlog OH): the count is spent on the LOUDEST partials, chosen after the loop, so the loop runs to Nyquist. ITS
	// SHARE (P1b, by decision: a morph between schemes never steps) blends the two schemes' keep masks: 0 is Harmonics
	// alone (the old road, to the bit), 1 Peaks alone, between them (1 − s)·Harmonics + s·Peaks. A NaN share is Harmonics.
	const float peaksShare = counting && drawer.partialCount.peaks > 0.0f ? std::min(drawer.partialCount.peaks, 1.0f) : 0.0f;
	// CENTERS (OI) blends the same way, Harmonics keeping what the two shares leave: (1 − p − c)·H + p·P + c·C.
	const float centersShare =
	    counting && drawer.partialCount.centers > 0.0f ? std::min(drawer.partialCount.centers, 1.0f - peaksShare) : 0.0f;
	const bool peaks = peaksShare > 0.0f || centersShare > 0.0f; // a scheme other than Harmonics acts: the loop runs to Nyquist
	// DEPTH (backlog OF): below 1 the culled partials are only turned down, so the loop runs to Nyquist for them too (the keep masks below
	// then do the counting); at 1, and for a NaN, this is not taken and the road is the old one to the bit.
	const float depthAsked = counting ? drawer.partialCount.depth : 1.0f;
	const float depth = depthAsked < 1.0f ? std::max(depthAsked, 0.0f) : 1.0f;
	const bool wide = peaks || depth < 1.0f;
	float budget = 0.0f;
	if (counting) {
		using spectral::PartialsOp;
		const float asked = drawer.partialCount.count;
		budget = std::isnan(asked) ? PartialsOp::kMaxCount : std::clamp(asked, PartialsOp::kMinCount, PartialsOp::kMaxCount);
	}
	const float peaksCount = budget; // Peaks spends the whole count on its own ranking; Harmonics walks `budget` down
	std::array<float, partials::kMaxPartials> harmonic {}; // Harmonics' counted gain per partial, while the two blend
	// SHIFT'S LOW EDGE (only while a shift acts, so the unshifted road keeps its lowest partials as they were, A0's 27.5 Hz
	// fundamental included). It RAMPS IN WITH THE SHIFT over its first kLowFadeZeroHz, so a sub-20 Hz partial does not step
	// from 1 to its edge as the shift leaves 0 (spectral-ops Shift review); a partial at or below 0 Hz is silent.
	constexpr float kFull = spectral::ShiftOp::kLowFadeFullHz;
	constexpr float kZero = spectral::ShiftOp::kLowFadeZeroHz;
	const float reach = std::min(1.0f, std::fabs(shiftHz) / kZero);
	const auto edgeAt = [&](float h) {
		if (shiftHz == 0.0f || h >= kFull) {
			return 1.0f;
		}
		const float edge = h > kZero ? (h - kZero) / (kFull - kZero) : 0.0f;
		return h > 0.0f ? 1.0f + (edge - 1.0f) * reach : 0.0f;
	};
	// INCREMENTS from the pitch, w_i = 2π·f0·ratio_i / sr, and THE NYQUIST FADE per partial. THE LOOP STOPS AT THE FIRST
	// PARTIAL THE FADE SILENCES ABOVE THE BAND, which relies on the ratios NEVER DECREASING: each sound's ratio_k is k plus
	// an off-grid drift of at most kOffGridDepth/2 = ±0.25 (so k − 0.25 < k + 1 − 0.25), a shape's are k exactly, and a
	// blend of non-decreasing sequences with non-negative weights is non-decreasing (pinned by the case "the spectrum's
	// ratios never decrease"). An empty spectrum has ratio 0 everywhere, whose fade is 0: count 0.
	// A DOWNWARD SHIFT PUTS THE LOWEST PARTIALS AT OR BELOW 0 Hz: they are silent (fade 0, and the bank's own fade of a
	// non-positive increment is 0) but the loop runs ON past them, to the partials above that still sound.
	const bool startsLow = shiftHz < 0.0f;
	const double perRatio = 2.0 * std::numbers::pi * f0Hz / sampleRate;
	bool anyEdge = false;
	int count = 0;
	for (; count < partials::kMaxPartials; ++count) {
		if (counting && !wide && !(budget > 0.0f)) {
			break; // the count is spent
		}
		const double r = static_cast<double>(moved ? sounding(count) : ratio[count]);
		const float w = static_cast<float>(perRatio * r);
		const float f = partials::nyquistFade(w);
		if (!(f > 0.0f) && !(startsLow && !(w > 0.0f))) {
			break;
		}
		inc[count] = w;
		hz[count] = static_cast<float>(f0Hz * r);
		fade[count] = f;
		if (f >= 1.0f) {
			out.full = count + 1;
		}
		const float g = edgeAt(hz[count]);
		float counted = g;
		if (counting && g > 0.0f) {
			if (g <= budget) {
				budget -= g;
			}
			else {
				counted = budget; // the last partial carries the remainder (and every one past it, nothing)
				budget = 0.0f;
			}
		}
		harmonic[static_cast<size_t>(count)] = counted;
		edgeScratch[count] = wide ? g : counted;
		anyEdge = anyEdge || edgeScratch[count] != 1.0f;
	}
	out.count = count;
	if (wide && count > 0) {
		// THE N LOUDEST, as the Spectral device's Peaks keeps the N strongest (spectral_stft.cpp's partialsSpectrum, on the same
		// counting): each partial's own amplitude (the envelope at its ratio × its fine structure, before the drawer's operators),
		// through its Nyquist fade and Shift's low edge. ⌈N⌉ are kept, the weakest of them at N's fraction, the rest silenced. The
		// top set and its weakest by nth_element, not a sort: the order within it is not read. No allocation: a fixed index array.
		// BOTH EARS RANKED TOGETHER (P1b, by decision, as Stretch and Shift are the same in both ears): with `otherEar` the
		// loudness is the two ears' energy, √(aL² + aR²), so a wide patch keeps the same partials in both.
		std::array<int, partials::kMaxPartials> order {};
		std::array<float, partials::kMaxPartials> loud {};
		for (int i = 0; peaksShare > 0.0f && i < count; ++i) { // the ranking is Peaks' alone: Depth and Centers do not read it
			const auto u = static_cast<size_t>(i);
			order[u] = i;
			const float a = spec.envAtRatio[u] * spec.fine[u];
			const float b = otherEar != nullptr ? otherEar->envAtRatio[u] * otherEar->fine[u] : 0.0f;
			loud[u] = (otherEar != nullptr ? std::sqrt(a * a + b * b) : std::fabs(a)) * fade[i] * edgeScratch[i];
		}
		std::array<float, partials::kMaxPartials> keep {};
		if (peaksShare > 0.0f) {
			const int whole = std::min(count, static_cast<int>(std::ceil(peaksCount - 1e-6f)));
			const float lastWeight = peaksCount - static_cast<float>(whole - 1);
			const auto louder = [&](int a, int b) { return loud[static_cast<size_t>(a)] > loud[static_cast<size_t>(b)]; };
			std::nth_element(order.begin(), order.begin() + (whole - 1), order.begin() + count, louder);
			for (int r = 0; r < whole; ++r) {
				keep[static_cast<size_t>(order[static_cast<size_t>(r)])] = r == whole - 1 ? std::min(1.0f, lastWeight) : 1.0f;
			}
			// THE KEEP GLIDES (backlog PD): each partial's keep moves at most `step` from where the last block left it, so a
			// partial the ranking drops ramps out over one character fade instead of one control block. The partials past the
			// count are silent and are remembered as 0, so one coming back under Nyquist ramps in.
			if (glide != nullptr) {
				if (glide->live) {
					const float step = glide->step;
					for (int i = 0; i < count; ++i) {
						const auto u = static_cast<size_t>(i);
						keep[u] = std::clamp(keep[u], glide->keep[i] - step, glide->keep[i] + step);
					}
				}
				std::copy_n(keep.begin(), count, glide->keep);
				std::fill(glide->keep + count, glide->keep + partials::kMaxPartials, 0.0f);
				glide->live = true;
			}
		}
		// THE BLEND: (1 − p − c)·Harmonics' counted gain + p·Peaks' + c·Centers' (each its edge times its keep). Centers' keep is the
		// terrain sparse field's generator at the set; past harmonic 7 it is 0. A scheme alone (share 1) leaves the others exact 0s.
		const float rest = std::max(0.0f, 1.0f - peaksShare - centersShare);
		for (int i = 0; i < count; ++i) {
			const auto u = static_cast<size_t>(i);
			const float centre =
			    centersShare > 0.0f ? static_cast<float>(partials::sparseKeep(i + 1, drawer.partialCount.centersSet)) : 0.0f;
			const float edge = edgeScratch[i];
			const float kept = rest * harmonic[u] + peaksShare * (edge * keep[u]) + centersShare * (edge * centre);
			// DEPTH: what the cull took away (edge - kept) comes back by 1 - depth; at depth 1 the term is an exact 0.
			edgeScratch[i] = kept + (1.0f - depth) * (edge - kept);
		}
		anyEdge = true;
	}
	if (moved) {
		const int keep = std::min(partials::kMaxPartials, (std::max(count + 1, keepRatios) + 3) / 4 * 4);
		for (int i = count; i < keep; ++i) {
			sounding(i);
		}
	}
	// NO PEAKS RANKING THIS BLOCK: the glide forgets, so the next ranking is taken as it stands (a note-on's road).
	if (glide != nullptr && !(wide && count > 0 && peaksShare > 0.0f)) {
		glide->live = false;
	}
	// THE EDGE GAINS, scaling each partial's raw amplitude before base (partialTargets): null where every one is 1.
	out.edge = anyEdge ? edgeScratch : nullptr;
	return out;
}

namespace {

/// The plot's reading of pitchPartials: the frequencies, fades and sounding ratios, the increments discarded.
PartialPitches plotFades(const partials::PartialSpectrum& spec, const spectral::ShaperParams& drawer, double f0Hz,
                         double sampleRate, float* hz, float* fade, float* ratioScratch, float* edgeScratch) {
	alignas(16) float inc[partials::kMaxPartials];
	return pitchPartials(spec, drawer, f0Hz, sampleRate, inc, hz, fade, ratioScratch, edgeScratch, 0);
}

/// The bank's silence floor (PartialBank::setTargets), so a reference reading floors as a voice's target does.
float plotFloored(float v) {
	const float mag = std::fabs(v);
	return mag >= partials::kSilenceFloor && mag < std::numeric_limits<float>::infinity() ? v : 0.0f;
}

} // namespace

int OscVoice::plotPartials(float* ratio, float* terrain, float* left, float* right) const {
	if (fresh_ == nullptr || stale_ == nullptr) {
		return 0;
	}
	const bool freshP = fresh_->engine == OscEngine::Partials;
	if (!freshP && stale_->engine != OscEngine::Partials) {
		return 0;
	}
	const partials::PartialSpectrum& spec = (freshP ? *fresh_ : *stale_).partials;
	alignas(16) float hz[partials::kMaxPartials];
	alignas(16) float fade[partials::kMaxPartials];
	alignas(16) float sounding[partials::kMaxPartials];
	alignas(16) float edge[partials::kMaxPartials];
	const PartialPitches pitches = plotFades(spec, drawer_, frequencyHz_, hostSampleRate_, hz, fade, sounding, edge);
	const int count = pitches.count;
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		ratio[i] = pitches.ratio[i]; // as the partial sounds: the frequency operators applied
		terrain[i] = spec.envAtRatio[u] * spec.fine[u] * fade[i];
		left[i] = partialBank_.target(i, 0);
		right[i] = partialBank_.target(i, 1);
	}
	return count;
}

int OscVoice::plotReference(const PreparedCharacter& side, const PreparedCharacter& sideR, double f0Hz, double sampleRate,
                            spectral::GainCache<partials::kMaxPartials>& cache,
                            spectral::GainCache<partials::kMaxPartials>& cacheR, const spectral::ShaperParams& drawer,
                            const spectral::ShaperParams* drawerR, float* ratio, float* terrain, float* left,
                            float* right, const spectral::VocodeEnvelope* vocode) {
	if (side.engine != OscEngine::Partials || !(f0Hz > 0.0) || !(sampleRate > 0.0)) {
		return 0;
	}
	const partials::PartialSpectrum& spec = side.partials;
	alignas(16) float hz[partials::kMaxPartials];
	alignas(16) float fade[partials::kMaxPartials];
	alignas(16) float sounding[partials::kMaxPartials];
	alignas(16) float edge[partials::kMaxPartials];
	const PartialPitches pitches = plotFades(spec, drawer, f0Hz, sampleRate, hz, fade, sounding, edge);
	const int count = pitches.count;
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		ratio[i] = pitches.ratio[i]; // as the partial sounds: the frequency operators applied
		terrain[i] = spec.envAtRatio[u] * spec.fine[u] * fade[i];
	}
	// ONE EAR AS controlPartials computes it: the drawer brought up to date at this pitch over the LEFT side's frequencies
	// (the ears share one set, design §1.2), Formant Shift's ratios, then the targets, through the fade and the floor.
	const auto ear = [&](const PreparedCharacter& s, spectral::GainCache<partials::kMaxPartials>& c,
	                     const spectral::ShaperParams& d, float* out, int earIndex) {
		c.update(d, f0Hz, hz, count);
		const spectral::PreparedShaper& shaper = c.prepared();
		alignas(16) float formant[partials::kMaxPartials];
		const float* fr = nullptr;
		if (!shaper.formantIdentity) {
			spectral::formantRatios(shaper, hz, count, formant);
			fr = formant;
		}
		alignas(16) float target[partials::kMaxPartials];
		alignas(16) float shape[partials::kMaxPartials];
		partialTargets(s, c, shaper, fade, fr, count, pitches.edge, target, shape);
		if (vocode != nullptr) { // OE: at the envelope of this moment, as the voice's step 4a
			applyVocode(d, vocode, earIndex, hz, fr, shape, fade, count, s.phasing.mode == partials::PhaseMode::SubPriority, f0Hz,
			            target);
		}
		for (int i = 0; i < count; ++i) {
			out[i] = plotFloored(target[i] * fade[i]);
		}
	};
	ear(side, cache, drawer, left, 0);
	if (&sideR == &side && drawerR == nullptr && !(vocode != nullptr && drawer.enabled && drawer.vocode.stereoEars())) {
		std::copy_n(left, count, right);
	}
	else if (drawerR != nullptr) {
		ear(sideR, cacheR, *drawerR, right, 1);
	}
	else {
		ear(sideR, cache, drawer, right, 1);
	}
	return count;
}

void OscVoice::applyDamp(const float* ratio, double f0Hz, int count, int frames, bool noteOn, float* targetL, float* targetR,
                         const float* refRatio) {
	const spectral::DampOp& d = drawer_.damp;
	// A NaN or negative rate is no decay, a NaN tilt every partial alike.
	const double rate = d.rate > 0.0f ? static_cast<double>(d.rate) : 0.0;
	const double tilt = d.tilt > 0.0f ? std::min(static_cast<double>(d.tilt), double(spectral::DampOp::kMaxTilt)) : 0.0;
	// THE RATE SPLIT IN TWO, so a moving pitch costs no pow: 60 dB in 1 / (rate·(hz / 1 kHz)^tilt) seconds, and
	// (hz / 1 kHz)^tilt = ratio^tilt · (f0 / 1 kHz)^tilt. The ratios' powers `dampQ_` move only with the spectrum or the
	// tilt; the rest is one scale per block, so a block's factor is 2^(scale·q_i) and a gain at an age 2^(ageScale·q_i).
	// A ratio at or below 0 (a downward Shift) is floored, as the frequency was, at 1e-3 Hz.
	const auto bytes = sizeof(float) * static_cast<std::size_t>(count);
	if (!(tilt == dampTilt_) || count > dampQCount_ || std::memcmp(ratio, dampRatio_, bytes) != 0) {
		const double floorRatio = 1e-3 / f0Hz;
		for (int i = 0; i < count; ++i) {
			dampRatio_[i] = ratio[i];
			dampQ_[i] = static_cast<float>(std::pow(std::max(static_cast<double>(ratio[i]), floorRatio), tilt));
		}
		dampTilt_ = tilt;
		dampQCount_ = count;
		dampCount_ = 0; // the factors read q
		++dampRecomputes_;
	}
	// −log2(1000) · rate · (f0 / 1 kHz)^tilt per second: the exponent of 2 per unit q.
	const double perSecond = -std::log2(1000.0) * rate * std::pow(f0Hz / spectral::DampOp::kReferenceHz, tilt);
	const bool restart = noteOn || !dampLive_;
	if (restart) {
		// THE NOTE STARTS UNDAMPED: its first block is age 0, and every later block multiplies its own length's decay in.
		std::fill_n(damp_, partials::kMaxPartials, 1.0f);
		dampLive_ = true;
		dampAgeFrames_ = 0.0;
		dampLiveCount_ = count;
	}
	else {
		dampAgeFrames_ += static_cast<double>(frames);
		const double scale = perSecond * static_cast<double>(frames) / hostSampleRate_;
		if (!(scale == dampScale_) || count > dampCount_) {
			spectral::exp2Scaled(dampQ_, scale, count, dampFactor_); // the block's factor per partial
			dampScale_ = scale;
			dampCount_ = count;
		}
		// EXACTLY ZERO BELOW THE BANK'S SILENCE FLOOR, so a long note's top partials leave the live count and the
		// accumulator never walks down through the denormals (design §5.4).
		const int carried = std::min(count, dampLiveCount_);
		for (int i = 0; i < carried; ++i) {
			const float g = damp_[i] * dampFactor_[i];
			damp_[i] = g >= partials::kSilenceFloor ? g : 0.0f;
		}
		// A PARTIAL ENTERING THE COUNT MID-NOTE (the Partials count rising, a glide or bend bringing it back under Nyquist)
		// has not been decaying while it was out, so its gain is taken from the note's AGE at its current frequency: what
		// it would be had it decayed at this rate all along (spectral-ops Damp review, 2026-10-12; design §3.2, "needs the
		// voice's note age").
		if (carried < count) {
			const double ageScale = perSecond * dampAgeFrames_ / hostSampleRate_;
			spectral::exp2Scaled(dampQ_ + carried, ageScale, count - carried, damp_ + carried);
			for (int i = carried; i < count; ++i) {
				damp_[i] = damp_[i] >= partials::kSilenceFloor ? damp_[i] : 0.0f;
			}
		}
		dampLiveCount_ = count;
	}
	// A-PH3, SUB PRIORITY'S REFERENCE: the same decay over the whole spectrum at the shape's own ratios (shapeRestRss), so the
	// sub falls with the rest it is set from — including the partials the cull or the drawer keep from rendering.
	if (refRatio == nullptr) {
		dampRefLive_ = false;
	}
	else {
		constexpr int n = partials::kMaxPartials;
		if (!(tilt == dampRefTilt_) || std::memcmp(refRatio, dampRefRatio_, sizeof(float) * n) != 0) {
			const double floorRatio = 1e-3 / f0Hz;
			for (int i = 0; i < n; ++i) {
				dampRefRatio_[i] = refRatio[i];
				dampRefQ_[i] = static_cast<float>(std::pow(std::max(static_cast<double>(refRatio[i]), floorRatio), tilt));
			}
			dampRefTilt_ = tilt;
		}
		if (restart) {
			std::fill_n(dampRef_, n, 1.0f);
		}
		else if (!dampRefLive_) {
			// SUB PRIORITY ARRIVING MID-NOTE: each partial's gain at the note's age, as a partial entering the count is.
			spectral::exp2Scaled(dampRefQ_, perSecond * dampAgeFrames_ / hostSampleRate_, n, dampRef_);
		}
		else {
			alignas(16) float factor[n];
			spectral::exp2Scaled(dampRefQ_, perSecond * static_cast<double>(frames) / hostSampleRate_, n, factor);
			for (int i = 0; i < n; ++i) {
				dampRef_[i] *= factor[i];
			}
		}
		for (int i = 0; i < n; ++i) {
			dampRef_[i] = dampRef_[i] >= partials::kSilenceFloor ? dampRef_[i] : 0.0f;
		}
		dampRefLive_ = true;
	}
	for (int i = 0; i < count; ++i) {
		targetL[i] *= damp_[i];
	}
	if (targetR != nullptr) {
		for (int i = 0; i < count; ++i) {
			targetR[i] *= damp_[i];
		}
	}
}
void pinSubPitch(const partials::PartialSpectrum& spec, double f0Hz, double sampleRate, float* inc, float* hz, float* fade,
                 float* ratioScratch, float* edgeScratch, PartialPitches& pitches, const float* glideFrom, float glideT) {
	// THE SUB GLIDES WITH THE REST (backlog PD): the same blend as pitchPartials', so a rebuild that moves its ratio ramps it.
	const float fresh0 = spec.ratio[0];
	const float r0 = glideFrom != nullptr ? glideFrom[0] + (fresh0 - glideFrom[0]) * glideT : fresh0;
	const float w = static_cast<float>(2.0 * std::numbers::pi * f0Hz / sampleRate * static_cast<double>(r0));
	inc[0] = w;
	hz[0] = static_cast<float>(f0Hz * static_cast<double>(r0));
	fade[0] = partials::nyquistFade(w);
	if (pitches.ratio == ratioScratch) {
		ratioScratch[0] = r0; // under a frequency operator the bank turns partial 0 by its own ratio, not the moved one
	}
	if (pitches.edge != nullptr) {
		edgeScratch[0] = 1.0f; // no low edge and no Partials share: the sub is not the rest
	}
	if (pitches.count == 0 && fade[0] > 0.0f) {
		edgeScratch[0] = 1.0f;
		pitches.count = 1;
	}
	if (fade[0] >= 1.0f && pitches.full == 0) {
		pitches.full = 1;
	}
}

double shapeRestRss(const partials::PartialSpectrum& shape, const partials::PartialSpectrum* stale, float t, const float* damp) {
	double rest = 0.0;
	for (int i = 1; i < partials::kMaxPartials; ++i) {
		const size_t u = static_cast<size_t>(i);
		float a = shape.envAtRatio[u] * shape.fine[u];
		if (stale != nullptr) {
			const float s = stale->envAtRatio[u] * stale->fine[u];
			a = s + (a - s) * t; // as controlPartials blends the targets
		}
		const double g = damp != nullptr ? static_cast<double>(damp[i]) : 1.0;
		const double x = static_cast<double>(a) * g;
		rest += x * x;
	}
	// UPSTREAM OF THE NYQUIST FADE, the sub's own included: the bank applies the sub's, so a sub pushed up into the fade (a
	// very high note) fades out like any partial there rather than being boosted to make up for it.
	return std::sqrt(rest);
}

/// A silent sidechain, for a voice handed no envelope.
const spectral::VocodeEnvelope kSilentVocode {};

void OscVoice::applyVocode(const spectral::ShaperParams& drawer, const spectral::VocodeEnvelope* env, int ear, const float* hz,
                           const float* formant, const float* shape, const float* fade, int count, bool subPriority,
                           double f0Hz, float* target, float* gainOut) {
	if (!drawer.enabled || !drawer.vocode.on || count <= 0) {
		return;
	}
	// A NULL ENVELOPE IS A SILENT SIDECHAIN: the rest silent (the Spectral effect's rule), the sub left to Sub priority.
	// (A file-scope constant, so the audio thread never runs a guarded first-use initialiser.)
	alignas(16) float scratch[partials::kMaxPartials];
	float* gain = gainOut != nullptr ? gainOut : scratch;
	spectral::vocodeGains(drawer.vocode, env != nullptr ? *env : kSilentVocode, ear, hz, formant, drawer, shape, fade,
	                      subPriority ? 1 : 0, count, f0Hz, gain);
	for (int i = 0; i < count; ++i) {
		target[i] *= gain[i];
	}
}

int OscVoice::sketchTargets(const PreparedCharacter& side, const spectral::ShaperParams& drawer, double f0Hz, double sampleRate,
                            spectral::GainCache<partials::kMaxPartials>& cache, float* out,
                            const spectral::VocodeEnvelope* vocode) {
	using partials::kMaxPartials;
	alignas(16) float inc[kMaxPartials];
	alignas(16) float hz[kMaxPartials];
	alignas(16) float fade[kMaxPartials];
	alignas(16) float sounding[kMaxPartials];
	alignas(16) float edge[kMaxPartials];
	PartialPitches pitches = pitchPartials(side.partials, drawer, f0Hz, sampleRate, inc, hz, fade, sounding, edge, 0);
	const bool subPriority = side.phasing.mode == partials::PhaseMode::SubPriority;
	if (subPriority) {
		pinSubPitch(side.partials, f0Hz, sampleRate, inc, hz, fade, sounding, edge, pitches);
	}
	const int count = pitches.count;
	cache.update(drawer, f0Hz, hz, count);
	const spectral::PreparedShaper& shaper = cache.prepared();
	alignas(16) float formant[kMaxPartials];
	const float* fr = nullptr;
	if (!shaper.formantIdentity) {
		spectral::formantRatios(shaper, hz, count, formant);
		fr = formant;
	}
	alignas(16) float shape[kMaxPartials];
	partialTargets(side, cache, shaper, fade, fr, count, pitches.edge, out, shape);
	// OE (§9.5): VOCODE AT THE ENVELOPE OF THIS MOMENT, when the synth asks with one — the note-on's own step 4a.
	if (vocode != nullptr) {
		applyVocode(drawer, vocode, 0, hz, fr, shape, fade, count, subPriority, f0Hz, out);
	}
	if (subPriority) {
		subLevelTarget(partials::subLevelOf(side.phasing), shapeRestRss(side.partials, nullptr, 0.0f, nullptr), out);
	}
	// AS THE NOTE-ON WEIGHS THEM (controlPartials step 5): each target through its Nyquist fade.
	for (int i = 0; i < count; ++i) {
		out[i] *= fade[i];
	}
	return count;
}

void OscVoice::controlPartials(int readIndex, int frames, bool ownsFade, bool stereoRender) {
	using partials::kMaxPartials;
	// THE STACK FRAME: this and partialTargets hold about fourteen kMaxPartials float arrays between them, ~18 KB at the
	// deepest (increments, frequencies, fades, the sounding ratios and edge gains, formant ratios, two ears' targets, a
	// stale side's, the power, and partialTargets' raw/base/gain), on the audio thread's stack rather than on the voice:
	// as members they would cost that again on every one of the synth's ~670 oscillators, used or not. No allocation
	// either way.
	const bool freshP = fresh_->engine == OscEngine::Partials;
	// The side the bank plays: the fresh one, unless an engine switch is fading OUT of a Partials side.
	const PreparedCharacter& lead = freshP ? *fresh_ : *stale_;
	const PreparedCharacter& leadR = freshP ? *freshR_ : *staleR_;
	const partials::PartialSpectrum& spec = lead.partials;
	// A PARTIALS -> PARTIALS REBUILD (pure road only): no crossfade of two renders, the bank retargets (design §1.3) —
	// each block's targets are the two spectra's at the fade's t at the END of the block, so the bank's linear ramp
	// over the block walks the amplitudes along the fade exactly, over the bank's fade length.
	const bool blend = ownsFade && fadeLeft_ > 0 && freshP && stale_->engine == OscEngine::Partials;
	// Stereo only when the ears' spectra differ (pointer inequality read in this sync, the bank's own rule) OR THEIR
	// DRAWERS DO (a stereo-live S control, design §4.3): two ears' gains over the one spectrum.
	const bool earDrawer = stereoRender && drawerStereo_;
	// AND WHILE VOCODE READS THE EARS APART (OE2: Stereo, Spread), whose gains differ over the one spectrum.
	const bool vocodeEars = drawer_.enabled && drawer_.vocode.stereoEars();
	// AND WHILE THE EARS READ AT DIFFERENT PHASES (backlog OZ: Phase Width with something to mirror, or a stereo route into
	// Phase): the ears' difference where the right ear's phases stand (the last block's end; on a note-on, this block's
	// first sample) and where this block ends. Exactly 0 on both, and so not stereo for it, for every patch whose Phase is
	// not stereo-live (earOffsetAt).
	const int block = frames; // kControlBlock, or the short block that aligns this one to the synth's tick
	const double earEnd = stereoRender ? earOffsetAt(readIndex + block - 1) : 0.0;
	const double earStart = !stereoRender ? 0.0 : partialsPrimed_ ? partialsEar_ : earOffsetAt(readIndex);
	const bool earPhase = earStart != 0.0 || earEnd != 0.0;
	const bool stereo =
	    stereoRender && (&leadR != &lead || (blend && staleR_ != stale_) || earDrawer || vocodeEars || earPhase);

	// 1-2. THE PITCH ARITHMETIC: increments, frequencies, Nyquist fades and the partials that render, the lead side's
	//    ratios for both ears (the ears get different amplitudes over SHARED frequencies, design §1.2), the drawer's
	//    Partials count applied — pitchPartials, which the plot shares.
	alignas(16) float inc[kMaxPartials];
	alignas(16) float hz[kMaxPartials];
	alignas(16) float fade[kMaxPartials];
	const double f0 = frequencyHz_;
	const double perRatio = 2.0 * std::numbers::pi * f0 / hostSampleRate_;
	alignas(16) float sounding[kMaxPartials];
	alignas(16) float edge[kMaxPartials];
	// THE REBUILD FADE'S t AT THIS BLOCK'S END (the targets below walk by it), and THE RATIOS WITH THEM (backlog PD): while it
	// runs, each partial's frequency glides from the stale side's ratio to the fresh one's (pitchPartials' glideFrom), so a
	// rebuild that moves an off-grid ratio ramps the pitch instead of stepping it. At t = 1 the fresh ratios are read as they are.
	const int ahead = blend ? std::min(fadeLeft_, 4 * frames) : 0; // the fade counts evaluation samples, 4 per host
	const float fadeT =
	    blend ? static_cast<float>(static_cast<double>(fadeTotal_ - fadeLeft_ + ahead) / static_cast<double>(fadeTotal_)) : 1.0f;
	const float* glideFrom = blend && fadeT < 1.0f ? stale_->partials.ratio.data() : nullptr;
	// PEAKS' KEEP GLIDES OVER ONE CHARACTER FADE (backlog PD, PeaksGlide): this block's share of it; a note-on ranks afresh.
	peaksGlide_->step = static_cast<float>(static_cast<double>(frames) / (kCharacterFadeSeconds * hostSampleRate_));
	if (!partialsPrimed_) {
		peaksGlide_->live = false;
	}
	// PEAKS RANKS BOTH EARS TOGETHER when their spectra differ (P1b), so the shared edge gains keep one set for both ears.
	PartialPitches pitches = pitchPartials(spec, drawer_, f0, hostSampleRate_, inc, hz, fade, sounding, edge, partialBank_.live(),
	                                       &leadR != &lead ? &leadR.partials : nullptr, peaksGlide_.get(), glideFrom, fadeT);
	// A-PH3: UNDER SUB PRIORITY THE SUB PARTIAL STAYS AT ITS OWN PITCH, exempt from Shift and Stretch (pinSubPitch).
	const partials::Phasing& phasing = lead.phasing;
	const bool subPriority = phasing.mode == partials::PhaseMode::SubPriority;
	if (subPriority) {
		pinSubPitch(spec, f0, hostSampleRate_, inc, hz, fade, sounding, edge, pitches, glideFrom, fadeT);
	}
	const int count = pitches.count;
	const int full = pitches.full;

	// 3. THE DRAWER, brought up to date for this block (re-prepared when its params or the gliding pitch moved, its key
	//    track following; each factor group recomputed only when what it reads moved), then Formant Shift's ratios
	//    (before base: the re-read is part of it).
	drawerCache_.update(drawer_, f0, hz, count);
	const spectral::PreparedShaper& shaper = drawerCache_.prepared();
	alignas(16) float formant[kMaxPartials];
	const float* fr = nullptr;
	if (!shaper.formantIdentity) {
		spectral::formantRatios(shaper, hz, count, formant);
		fr = formant;
	}
	// AND THE RIGHT EAR'S DRAWER, while it differs from the left's: its own cache and its own Formant Shift ratios. Else
	// the right ear reads the left's (the cache, the shaper and the ratios are shared, as they always were).
	const spectral::GainCache<kMaxPartials>* cacheR = &drawerCache_;
	const spectral::PreparedShaper* shaperR = &shaper;
	alignas(16) float formantR[kMaxPartials];
	const float* frR = fr;
	if (earDrawer) {
		drawerCacheR_.update(drawerR_, f0, hz, count);
		cacheR = &drawerCacheR_;
		shaperR = &drawerCacheR_.prepared();
		frR = nullptr;
		if (!shaperR->formantIdentity) {
			spectral::formantRatios(*shaperR, hz, count, formantR);
			frR = formantR;
		}
	}
	// 4. BASE AND GAINS PER EAR -> TARGETS.
	alignas(16) float tL[kMaxPartials];
	alignas(16) float tR[kMaxPartials];
	// AND, WHILE VOCODE IS ON, EACH SIDE'S SHAPE (the terrain as read, before the drawer), which its Flatten divides by (§9.2).
	const bool vocoding = drawer_.enabled && (drawer_.vocode.on || (earDrawer && drawerR_.vocode.on));
	alignas(16) float sL[kMaxPartials];
	alignas(16) float sR[kMaxPartials];
	partialTargets(lead, drawerCache_, shaper, fade, fr, count, pitches.edge, tL, vocoding ? sL : nullptr);
	if (stereo) {
		partialTargets(leadR, *cacheR, *shaperR, fade, frR, count, pitches.edge, tR, vocoding ? sR : nullptr);
	}
	float blendT = 0.0f; // the rebuild crossfade's t, which Sub priority's reference blends by too (4c)
	if (blend) {
		const float t = fadeT;
		blendT = t;
		alignas(16) float s[kMaxPartials];
		alignas(16) float ss[kMaxPartials];
		partialTargets(*stale_, drawerCache_, shaper, fade, fr, count, pitches.edge, s, vocoding ? ss : nullptr);
		for (int i = 0; i < count; ++i) {
			tL[i] = s[i] + (tL[i] - s[i]) * t;
		}
		if (vocoding) {
			for (int i = 0; i < count; ++i) {
				sL[i] = ss[i] + (sL[i] - ss[i]) * t; // the shape blended as the targets are
			}
		}
		if (stereo) {
			partialTargets(*staleR_, *cacheR, *shaperR, fade, frR, count, pitches.edge, s, vocoding ? ss : nullptr);
			for (int i = 0; i < count; ++i) {
				tR[i] = s[i] + (tR[i] - s[i]) * t;
			}
			if (vocoding) {
				for (int i = 0; i < count; ++i) {
					sR[i] = ss[i] + (sR[i] - ss[i]) * t;
				}
			}
		}
	}

	// 4a. VOCODE (OE, additive design §9.2): the sidechain's envelope read at each partial's own frequency (hz / Formant Shift's
	//     ratio), the shape divided out by Flatten — AFTER the drawer's gains and their compensation, BEFORE Damp, so a vocoded
	//     pluck still decays. Under Sub priority partial 0 is the sub's (step 4c sets it), never vocoded. Each ear by its drawer.
	//     OE2: THROUGH THE GAINS' MEMORY (VocodeMemo): recomputed only when something they are made from moved.
	if (vocoding) {
		const spectral::ShaperParams& right = earDrawer ? drawerR_ : drawer_;
		VocodeMemo memo;
		memo.env = vocodeEnv_;
		memo.envSerial = vocodeEnv_ != nullptr ? vocodeEnv_->serial : 0;
		memo.fresh = lead.buildSerial;
		memo.stale = blend ? stale_->buildSerial : 0;
		memo.freshR = stereo ? leadR.buildSerial : 0;
		memo.staleR = stereo && blend ? staleR_->buildSerial : 0;
		memo.blendT = blendT;
		memo.f0 = f0;
		memo.sampleRate = hostSampleRate_;
		memo.count = count;
		memo.stereo = stereo;
		memo.subPriority = subPriority;
		memo.enabled = drawer_.enabled;
		memo.vocodeL = drawer_.vocode;
		memo.vocodeR = stereo ? right.vocode : spectral::VocodeOp {};
		memo.shiftL = drawer_.formantShift;
		memo.shiftR = stereo ? right.formantShift : spectral::FormantShiftOp {};
		memo.stretch = drawer_.stretch;
		memo.shift = drawer_.shift;
		memo.partialCount = drawer_.partialCount;
		if (!vocodeMemoValid_ || !(memo == vocodeMemo_)) {
			std::fill_n(vocodeGains_->left, count, 1.0f);
			applyVocode(drawer_, vocodeEnv_, 0, hz, fr, sL, fade, count, subPriority, f0, tL, vocodeGains_->left);
			if (stereo) {
				std::fill_n(vocodeGains_->right, count, 1.0f);
				applyVocode(right, vocodeEnv_, 1, hz, frR, sR, fade, count, subPriority, f0, tR, vocodeGains_->right);
			}
			vocodeMemo_ = memo;
			vocodeMemoValid_ = true;
			++vocodeComputes_;
		}
		else {
			for (int i = 0; i < count; ++i) {
				tL[i] *= vocodeGains_->left[i];
			}
			if (stereo) {
				for (int i = 0; i < count; ++i) {
					tR[i] *= vocodeGains_->right[i];
				}
			}
		}
	}
	else {
		vocodeMemoValid_ = false;
	}

	// 4b. DAMP (spectral_shaper.h, DampOp): the note's age, AFTER the drawer and its compensation, so the compensation
	//     never lifts the decay back up. The lead ear's drawer for both ears. Off (or the master off), the next on starts at 1.
	if (drawer_.enabled && drawer_.damp.on) {
		applyDamp(pitches.ratio, f0, count, frames, !partialsPrimed_, tL, stereo ? tR : nullptr,
		          subPriority ? spec.ratio.data() : nullptr);
	}
	else {
		dampLive_ = false;
		dampRefLive_ = false;
	}

	// 5. THE NOTE-ON (or a return to this road): the phases are SCHROEDER'S, weighted by the power this first block
	//    will render (decided 2026-10-10, after Newman's flat pattern measured a crest factor of 5 on a
	//    bright spectrum). In stereo the power is the MEAN OF THE TWO EARS' (L² + R²)/2, so neither ear's spectrum is
	//    favoured. Amplitudes snap to these targets (the amp envelope owns the attack, §5.2).
	partialsBlockLength_ = frames;
	double startOffset = partialsOffset_;
	const bool priming = !partialsPrimed_;
	// 4c. A-PH3, SUB PRIORITY'S SUB AS A LEVEL (decision (b), 2026-10-18): the sub partial is SET at Sub
	//     dB against the SHAPE's rest — its root-sum-square upstream of every drawer operator and of the Nyquist cull, Damp's
	//     decay applied (dampRef_) — so no drawer operator and no high note moves it, and it follows Damp by construction.
	//     Each ear from its own shape. Nothing at all off Sub priority.
	if (subPriority) {
		const float level = partials::subLevelOf(phasing);
		const float* refDamp = dampRefLive_ ? dampRef_ : nullptr;
		subLevelTarget(level, shapeRestRss(lead.partials, blend ? &stale_->partials : nullptr, blendT, refDamp), tL);
		if (stereo) {
			subLevelTarget(level, shapeRestRss(leadR.partials, blend ? &staleR_->partials : nullptr, blendT, refDamp), tR);
		}
	}
	if (priming) {
		alignas(16) float power[kMaxPartials];
		for (int i = 0; i < count; ++i) {
			const float l = tL[i] * fade[i];
			const float r = stereo ? tR[i] * fade[i] : l;
			power[i] = 0.5f * (l * l + r * r);
		}
		// A-PH: THE OPTIMISED PROFILE, WHEN THERE IS ONE FOR (NEARLY) THIS SPECTRUM — only here, where the phases are reset
		// anyway, so a legato note, a glide or a held note is never re-phased. Otherwise the closed form, as always.
		bool profiled = false;
		ceilingScale_ = 1.0f;
		if (phasing.mode != partials::PhaseMode::Natural && phaseProfiles_ != nullptr && count > 0
		    && phaseProfiles_->read(profileScratch_)) {
			alignas(16) float amp[kMaxPartials];
			double rss = 0.0;
			for (int i = 0; i < count; ++i) {
				amp[i] = std::sqrt(power[i]);
				rss += static_cast<double>(power[i]);
			}
			if (partials::profileDistance(profileScratch_, amp, count) < partials::kProfileTolerance) {
				partialBank_.resetPhases(profileScratch_.phase.data(), count);
				profiled = true;
				++profileNoteOns_;
				// THE CEILING (Sub priority): the note's predicted peak is its RSS times the profile's peak per unit RSS; a
				// voice over the ceiling is turned down WHOLE for the note (its balance kept), never up (rule 4).
				if (subPriority) {
					const double peak = std::sqrt(rss) * static_cast<double>(profileScratch_.peakPerRss);
					const double ceiling = std::pow(10.0, static_cast<double>(phasing.ceilingDb) / 20.0);
					ceilingScale_ = peak > ceiling ? static_cast<float>(ceiling / peak) : 1.0f;
				}
			}
		}
		if (!profiled) {
			partialBank_.reset(power, count);
		}
		partialBank_.snapNextTargets();
		partialsResetCount_ = count;
		++partialsResets_;
		partialsPrimed_ = true;
		// The note starts AT its read offset (as the 4x path's readPrimed_), turned in as one step below — AND AT THE
		// ACCUMULATOR'S PHASE (backlog LY, the Osc Mode on Additive): the waveform read `phase_` cycles in, which is 0 for a
		// Digi note (and a Catchup note with nothing to adopt) and the song's or the slot's phase for an Analog one. The
		// accumulator's own advance is the bank's rotation at f0, so only its DEPARTURES from that are folded in later
		// (partialsAccum_): a Catchup bend, a Digi restart, an Analog re-lock.
		// A RETURN TO THIS ROAD (an engine switch back) is not a note: the bank starts at its read offset alone, as it always
		// did, and the accumulator's later departures are measured from where it stands now.
		partialsAccum_ = partialsNoteOn_ ? phase_ : 0.0;
		partialsAccumRef_ = phase_ - partialsAccum_;
		partialsNoteOn_ = false;
		startOffset = readOffsetAt(readIndex) + partialsAccum_;
	}

	// 6. THE PHASE CONTROL (the read offset), AS A RAMP: the offset at the block's END (host sample readIndex + L − 1, L the
	//    block's length, which is the value the 4x path has reached after that sample) against where the phases stand,
	//    folded into the block's increments as ratio_i·2π·Δ/L per sample, so the phase moves linearly like the 4x path's stepReadOffset rather
	//    than stepping once a block. For a harmonic partial that is the waveform read Δ cycles later. Unwrapped, so an
	//    offset carried past 1 keeps turning rather than jumping back. Every rendering partial's bent increment is
	//    ratio_i·(2π·f0/sr + bend), one sign for all of them, so the fold is possible iff that is positive and the
	//    highest FULL-GAIN partial's stays at or below π; otherwise (a jump too big to fold) the move is one step,
	//    rotate(). Partials up in the Nyquist fade may be bent past π: the bank wraps their angle, so their phase at the
	//    block's end is still exact and only their (faded) slope inside the block aliases. Judged on the highest rendering
	//    partial instead, every rising move at a low note would step, since that partial sits at up to 0.98π.
	//    PHASE WIDTH and the ear routes are step 7's, on the right ear's own rotators.
	// A-PH: THE NOTE'S CEILING SCALE, fixed at its note-on, on every block's targets (Sub priority only; 1 otherwise).
	if (subPriority && ceilingScale_ != 1.0f) {
		for (int i = 0; i < count; ++i) {
			tL[i] *= ceilingScale_;
		}
		if (stereo) {
			for (int i = 0; i < count; ++i) {
				tR[i] *= ceilingScale_;
			}
		}
	}
	// THE ACCUMULATOR'S DEPARTURE FROM ITS OWN f0 RUN (backlog LY), carried with the read offset: `phase_` against the
	// reference that advanced exactly as it would with nothing disturbing it (settlePartialsOwed), unwrapped by the nearest
	// whole cycle from where the bank already stands. With no catchup, restart or re-lock the two run through identical
	// arithmetic and the difference is exactly 0, so the patch's blocks are the old ones to the bit. ON THE MIXED ROAD the
	// 4x loop owns the accumulator, so the reference is re-seated and nothing is folded.
	if (ownsFade) {
		double move = (phase_ - partialsAccumRef_) - partialsAccum_;
		move -= std::round(move);
		partialsAccum_ += move;
	}
	else {
		partialsAccumRef_ = phase_ - partialsAccum_;
	}
	const double endOffset = readOffsetAt(readIndex + block - 1) + partialsAccum_;
	const double delta = endOffset - startOffset;
	const float bend = static_cast<float>(2.0 * std::numbers::pi * delta / static_cast<double>(block));
	const double slope = perRatio + static_cast<double>(bend);
	const int top = (full > 0 ? full : count) - 1; // the highest FULL-GAIN partial (any rendering one if none is)
	// AND ONLY WHILE SOMETHING CAN SOUND: under a deep downward Shift every rendering partial may sit at or below 0 Hz, and
	// a negative ratio passes the gate below whatever the slope, so a fold there would be gated by nothing. The ratios never
	// decrease, so the top one says whether any is above 0 Hz; if none is, the move is the step (rotate), which is exact.
	const bool canSound = count > 0 && pitches.ratio[count - 1] > 0.0f;
	const bool fold = delta != 0.0 && canSound && slope > 0.0
	                  && static_cast<double>(pitches.ratio[top]) * slope <= std::numbers::pi;
	partialBank_.setTargets(inc, tL, stereo ? tR : nullptr, count, block, stereo, fold ? pitches.ratio : nullptr,
	                        fold ? bend : 0.0f);
	partialsStereo_ = stereo;
	if (priming && startOffset != 0.0) {
		partialBank_.rotate(pitches.ratio, static_cast<float>(startOffset), kMaxPartials);
	}
	if (!fold && delta != 0.0) {
		partialBank_.rotate(pitches.ratio, static_cast<float>(delta), kMaxPartials);
	}
	partialsOffset_ = endOffset;
	partialsBlockStart_ = startOffset; // for a flip that cuts this block short (syncToBank)
	partialsBlockDelta_ = fold ? delta : 0.0;

	// 7. THE RIGHT EAR'S PHASE (backlog OZ, 2026-10-29): Phase as a stereo destination, R = base + m·(1 − 2w) + (mR − mL),
	//    as the other engines read it. The left ear's phases stand where step 6 put them; the right ear reads its own
	//    rotators, the shared ones turned by ratio_i·2π·(the ears' difference), from earStart at the block's first sample
	//    ramping to earEnd at its last, derived afresh from the shared ones every block. This gives up design §1.4's "a
	//    mono sum cannot cancel" for the patches that ask for it, which is what V5 already does. Not asked (both 0), the
	//    bank renders both ears off the shared rotator, as it always did.
	if (earPhase) {
		partialBank_.setEarPhase(pitches.ratio, static_cast<float>(earStart), static_cast<float>(earEnd), block,
		                         kMaxPartials);
	}
	partialsEar_ = earEnd;
	partialsEarStart_ = earStart;
	partialsEarDelta_ = earEnd - earStart;
}

void OscVoice::settlePartialsOwed() {
	if (partialsOwed_ > 0) {
		const double increment = frequencyHz_ / hostSampleRate_;
		advancePhaseBy(increment, partialsOwed_);
		// THE REFERENCE THE BANK'S OWN ROTATION KEEPS (backlog LY): the accumulator as it would be undisturbed, by advancePhaseBy's
		// own arithmetic without the catchup term, so the two stay bit-equal until something moves the real one.
		partialsAccumRef_ += increment * static_cast<double>(partialsOwed_);
		partialsAccumRef_ -= std::floor(partialsAccumRef_);
		if (gliding_) {
			advanceGlideBy(partialsOwed_);
		}
		partialsOwed_ = 0;
	}
}

void OscVoice::renderPartialsPure(int chunk, bool stereoRender) {
	++partialsChunks_;
	// Nothing feeds the 4x decimators on this road; they are reset before the 4x path next runs (an engine switch away).
	decimatorsIdle_ = true;
	std::fill_n(carry_, chunk, 0.0f);
	if (stereoRender) {
		std::fill_n(carryR_, chunk, 0.0f);
	}
	int s = 0;
	while (s < chunk) {
		if (partialsControlLeft_ <= 0) {
			// THE ACCUMULATOR AND THE GLIDE CATCH UP HERE, by the host samples rendered since the last block — always a
			// whole block in steady state, whatever the chunks were, so the pitch a block reads is the same at every host
			// block size TO THE BIT (advancing them per chunk would split one exp() into several, which rounds differently).
			settlePartialsOwed();
			const int frames = partialsBlockFrames(readIndex_);
			controlPartials(readIndex_, frames, true, stereoRender);
			partialsControlLeft_ = frames;
		}
		const int span = std::min(partialsControlLeft_, chunk - s);
		// A MONO render() passes carryR_ too: after a stereo render the bank may still be stereo until its next block,
		// and then writes R into carryR_, which a mono stream never delivers (beginStereoStream re-adopts from L).
		partialBank_.render(carry_ + s, carryR_ + s, span);
		if (stereoRender && !partialsStereo_) {
			std::copy_n(carry_ + s, span, carryR_ + s);
		}
		// THE VOICE'S SCHEDULES: the accumulator and glide are owed (above); a Partials -> Partials fade advances here (4
		// evaluations a host sample, the unit it is counted in; integers, so exact at any split), and the read index the
		// Phase control reads at. FM, jitter, grit, the noise stream, Warp and the vowel bake do not exist on this road
		// and are not run (design C4).
		partialsOwed_ += span;
		if (fadeLeft_ > 0) {
			fadeLeft_ = std::max(0, fadeLeft_ - 4 * span);
		}
		readIndex_ += span;
		partialsControlLeft_ -= span;
		s += span;
	}
}

void OscVoice::addPartialsMixed(int chunk, int fadeLeft0, int readIndex0, bool stereoRender) {
	++partialsChunks_;
	alignas(16) float bankL[Halfband4x::kMaxBlock] {};
	alignas(16) float bankR[Halfband4x::kMaxBlock] {};
	int s = 0;
	int index = readIndex0;
	while (s < chunk) {
		// The 4x loop that just ran owns the glide and the fade, so this block only reads them.
		if (partialsControlLeft_ <= 0) {
			const int frames = partialsBlockFrames(index);
			controlPartials(index, frames, false, stereoRender);
			partialsControlLeft_ = frames;
		}
		const int span = std::min(partialsControlLeft_, chunk - s);
		partialBank_.render(bankL + s, bankR + s, span);
		if (stereoRender && !partialsStereo_) {
			std::copy_n(bankL + s, span, bankR + s);
		}
		index += span;
		partialsControlLeft_ -= span;
		s += span;
	}
	// THE TWO WEIGHTS FROM THE ONE t. Evaluation j of the chunk was mixed at t_j = (total − left0 + j + 1)/total while
	// the fade ran (nextSample's arithmetic), 1 after it; the Partials side read 0 there, so the 4x side carries
	// (1 − t) when it is the stale one and t when it is the fresh one. Host sample h is evaluations 4h..4h+3, and its
	// 4x weight is their mean; the bank takes the complement, so the two sum to 1 at every host sample.
	const bool freshP = fresh_->engine == OscEngine::Partials;
	const double total = static_cast<double>(fadeTotal_);
	for (int h = 0; h < chunk; ++h) {
		double sum = 0.0;
		for (int j = 4 * h; j < 4 * h + 4; ++j) {
			sum += j < fadeLeft0 ? static_cast<double>(fadeTotal_ - fadeLeft0 + j + 1) / total : 1.0;
		}
		const double t = 0.25 * sum;
		const float table = static_cast<float>(freshP ? 1.0 - t : t);
		const float weight = 1.0f - table;
		mixTableWeight_[h] = table;
		mixPartialsWeight_[h] = weight;
		carry_[h] += weight * bankL[h];
		if (stereoRender) {
			carryR_[h] += weight * bankR[h];
		}
	}
}

// OUT OF LINE, all three: the String road's scratch (a 320-float amplitude table, two 32-float ears) must not join the frame of
// generateChunk, which every engine's render runs through — tests build Synths on a stack with kilobytes to spare.
[[gnu::noinline]] void OscVoice::controlString(const PreparedCharacter& side) {
	// OUTSIDE THE STRING'S RANGE THE NOTE IS SILENT — above its top (ks::kShortestPeriod), as Additive culls what it cannot
	// render, and below its floor (ks::kLowestHz, C0) — never a clamped, wrong pitch. A pending pluck waits, so a note bent
	// back into range sounds.
	if (frequencyHz_ > hostSampleRate_ / ks::kShortestPeriod || frequencyHz_ < ks::kLowestHz) {
		if (!ks_->string.idle()) {
			ks_->string.reset();
		}
		ks_->hz = -1.0;
		return;
	}
	if (frequencyHz_ != ks_->hz) {
		ks_->string.setFrequency(frequencyHz_);
		ks_->hz = frequencyHz_;
	}
	const StringFields& m = side.string;
	const ks::StringLaws laws = ks::lawsFor(m.brightness, m.sustain, m.pick, m.noise, m.stretch, ks_->controls,
	                                        ks_->string.frequencyHz(), ks_->released);
	if (!ks_->lawsSet || laws.dispersion != ks_->laws.dispersion) {
		ks_->string.setDispersion(laws.dispersion);
	}
	if (!ks_->lawsSet || laws.damping != ks_->laws.damping) {
		ks_->string.setDamping(laws.damping);
	}
	if (!ks_->lawsSet || laws.t60 != ks_->laws.t60) {
		// THE FINGER'S EDGE IS A 5 ms RAMP (K9); every other change of T60 (a moving puck, a glide under Decay Track)
		// ramps over the block, so the loop gain never steps.
		const bool edge = ks_->released != ks_->releaseApplied;
		const int ramp = edge ? static_cast<int>(std::lround(0.005 * hostSampleRate_)) : kStringControlBlock;
		ks_->string.setDecay(laws.t60, ks_->lawsSet ? ramp : 0);
	}
	ks_->laws = laws;
	ks_->lawsSet = true;
	ks_->releaseApplied = ks_->released;
	const double pickL = ks_->controls.pickup - 0.5 * ks_->controls.width;
	const double pickR = ks_->controls.pickup + 0.5 * ks_->controls.width;
	if (pickL != ks_->pickL || pickR != ks_->pickR) {
		ks_->string.setPickups(pickL, pickR);
		ks_->pickL = pickL;
		ks_->pickR = pickR;
	}
	if (ks_->pluckPending) {
		ks_->pluckPending = false;
		if (ks_->pluckBuilder != nullptr) {
			// THE TONE IS THE SIDE'S SPECTRUM ON THE HARMONICS (design §2.2): each partial's amplitude at harmonic i + 1,
			// an off-grid partial folded onto the harmonic it was drifting from (a periodic fill cannot hold it).
			alignas(16) float amplitude[partials::kMaxPartials];
			for (int i = 0; i < partials::kMaxPartials; ++i) {
				amplitude[i] = side.partials.envAtRatio[static_cast<size_t>(i)] * side.partials.fine[static_cast<size_t>(i)];
			}
			ks::Excitation e;
			e.tone = ks_->pluckBuilder->toneCycle(amplitude, partials::kMaxPartials, hostSampleRate_ / ks_->string.frequencyHz());
			e.tonePoints = e.tone != nullptr ? ks::kToneCyclePoints : 0;
			e.noise = e.tone != nullptr ? laws.noise : 1.0f;
			// VELOCITY MOVES HARDNESS by its depth (by design): a hard note plucks brighter, a soft one duller.
			e.hardness = static_cast<float>(std::clamp(static_cast<double>(ks_->controls.hardness)
			                                               + ks_->controls.velocity * (ks_->velocity - 0.5),
			                                           0.0, 1.0));
			ks_->lastHardness = e.hardness;
			// A SUBTLE, SEEDED VARIATION PER PLUCK (by design): the pick lands within ±5% of where the
			// material puts it and the level within ±0.25 dB, each drawn from this pluck's own seed (derived, never incidental:
			// design §6), so no two plucks of a phrase are identical and a song's plucks are the same on every pass.
			ks::PluckNoise vary(ks_->seed ^ 0xA5A5F00DC0FFEEull);
			const double pickJitter = 1.0 + ks::kPluckPickVariation * vary.next();
			const double levelJitter = std::pow(10.0, ks::kPluckLevelVariationDb * vary.next() / 20.0);
			e.pick = laws.pick * pickJitter;
			e.level = static_cast<float>(levelJitter);
			ks_->lastPick = e.pick;
			e.seed = ks_->seed;
			ks_->pluckBuilder->pluck(ks_->string, e);
			++ks_->plucks;
		}
	}
}

[[gnu::noinline]] void OscVoice::renderStringPure(int chunk, bool stereoRender) {
	++ks_->chunks;
	settlePartialsOwed();
	// Nothing feeds the 4x decimators on this road either (renderPartialsPure's reason).
	decimatorsIdle_ = true;
	int s = 0;
	while (s < chunk) {
		if (ks_->controlLeft <= 0) {
			// THE PITCH MOVES AT CONTROL RATE, the block's glide taken at its start; the blocks count themselves, so the
			// pitch a block reads is the same at every host block size.
			if (gliding_) {
				advanceGlideBy(kStringControlBlock);
			}
			advancePhaseBy(frequencyHz_ / hostSampleRate_, kStringControlBlock);
			controlString(*fresh_);
			ks_->controlLeft = kStringControlBlock;
		}
		const int span = std::min(ks_->controlLeft, chunk - s);
		ks_->string.render(carry_ + s, stereoRender ? carryR_ + s : nullptr, span);
		if (fadeLeft_ > 0) {
			fadeLeft_ = std::max(0, fadeLeft_ - 4 * span);
		}
		readIndex_ += span;
		ks_->controlLeft -= span;
		s += span;
	}
}

[[gnu::noinline]] void OscVoice::addStringMixed(int chunk, int fadeLeft0, bool stereoRender) {
	++ks_->chunks;
	const bool freshS = fresh_->engine == OscEngine::String;
	const PreparedCharacter& side = freshS ? *fresh_ : *stale_;
	alignas(16) float l[Halfband4x::kMaxBlock] {};
	alignas(16) float r[Halfband4x::kMaxBlock] {};
	int s = 0;
	while (s < chunk) {
		// The 4x loop that just ran owns the glide and the fade, so this block only reads them.
		if (ks_->controlLeft <= 0) {
			controlString(side);
			ks_->controlLeft = kStringControlBlock;
		}
		const int span = std::min(ks_->controlLeft, chunk - s);
		ks_->string.render(l + s, stereoRender ? r + s : nullptr, span);
		ks_->controlLeft -= span;
		s += span;
	}
	// addPartialsMixed's weights from the one t: the 4x side read 0 for the String side, so the String side takes t
	// when it is the fresh one and 1 − t when it is the stale one.
	const double total = static_cast<double>(fadeTotal_);
	for (int h = 0; h < chunk; ++h) {
		double sum = 0.0;
		for (int j = 4 * h; j < 4 * h + 4; ++j) {
			sum += j < fadeLeft0 ? static_cast<double>(fadeTotal_ - fadeLeft0 + j + 1) / total : 1.0;
		}
		const double t = 0.25 * sum;
		const auto weight = static_cast<float>(freshS ? t : 1.0 - t);
		ks_->mixWeight[h] = weight;
		carry_[h] += weight * l[h];
		if (stereoRender) {
			carryR_[h] += weight * r[h];
		}
	}
}

[[gnu::noinline]] void OscVoice::controlDrum(const PreparedCharacter& side) {
	DrumRoadState& d = *drum_;
	if (d.kit == nullptr) {
		return;
	}
	const bool kit = d.kit->kit;
	const drum::PadSetup& pad = d.kit->pads[kit ? d.pad : 0];
	const drum::Fields& fields = side.drum.pads[side.drum.kit ? d.pad : 0];
	// THE PITCH (decision 8, 2026-11-07): in Pitched mode the voice's own, Tune on top. In Kit mode the
	// kind's resting pitch at the pad's Tune and the kit's transpose, moved by the pad's Bend share of what the voice adds past
	// its key and that transpose (the wheel, an expression, a pitch route), held at the strike as much as moved while it
	// rings: an 808's bend into a hit. Never by the key itself.
	double baseHz = frequencyHz_ * std::exp2(pad.tuneSemitones / 12.0);
	if (kit) {
		const double moved = pitch_ - d.keySemitones - d.kit->transposeSemitones;
		baseHz = drum::kindBaseHz(pad.kind)
		         * std::exp2((pad.tuneSemitones + d.kit->transposeSemitones + pad.bendAmount * moved) / 12.0);
	}
	const drum::Laws laws = drum::lawsFor(pad.kind, fields, pad.controls, baseHz, d.velocity);
	// THE LAYER (stage (c)): its own kind at its own place's fields, the pad's controls, the pad's pitch movement on its kind's
	// resting pitch. Struck its Delay after the pad's strike (renderDrumSpan counts it down to the sample); gated with the pad.
	const drum::PadSetup::Layer& layer = pad.layer;
	if (kit && layer.on && d.strikePending) {
		d.layerKind = layer.kind;
		d.layerPending = true;
		d.layerDelayLeft = static_cast<int>(std::lround(std::max(0.0, layer.delaySeconds) * hostSampleRate_));
		d.layerHpX[0] = d.layerHpX[1] = d.layerHpY[0] = d.layerHpY[1] = 0.0f;
	}
	if (kit && (d.layerPending || !d.layer.idle())) {
		const double layerHz = drum::kindBaseHz(d.layerKind) * (baseHz / drum::kindBaseHz(pad.kind));
		d.layerLaws = drum::lawsFor(d.layerKind, side.drum.layers[d.pad], pad.controls, layerHz, d.velocity);
		if (!d.layer.idle()) {
			d.layer.setLaws(d.layerLaws);
		}
		d.layerGain = layer.gain;
		// THE ONE-POLE HIGH-PASS, y = a·(y' + x − x'); at its bottom (20 Hz) bypassed, so the layer is its hit exactly.
		d.layerHpA = layer.highPassHz <= 20.0 + 1.0e-6
		                 ? 1.0f
		                 : static_cast<float>(1.0 / (1.0 + 2.0 * std::numbers::pi * layer.highPassHz / hostSampleRate_));
	}
	if (d.strikePending) {
		d.strikePending = false;
		d.hit.strike(pad.kind, laws, d.seed, static_cast<float>(std::clamp(d.velocity, 0.0, 1.0)));
		d.releaseApplied = false;
		++d.strikes;
	}
	else if (!d.hit.idle()) {
		d.hit.setLaws(laws);
	}
	// THE GATE (D4): a Gate pad's released key damps the hit to Release; a One-shot pad's never does.
	if (d.released && pad.gate && !d.releaseApplied && !d.hit.idle()) {
		d.hit.release(pad.releaseSeconds);
		d.releaseApplied = true;
	}
	// AND ITS LAYER, once that has struck (a layer still waiting out its Delay is damped at the first block after it strikes).
	if (d.released && pad.gate && !d.layerReleaseApplied && !d.layer.idle()) {
		d.layer.release(pad.releaseSeconds);
		d.layerReleaseApplied = true;
	}
	// LEVEL AND PAN, equal power with the centre at unity in each ear (so a centred pad is the mono road's level exactly).
	const double theta = (static_cast<double>(std::clamp(pad.pan, -1.0f, 1.0f)) + 1.0) * std::numbers::pi / 4.0;
	d.gainL = pad.gain * static_cast<float>(std::numbers::sqrt2 * std::cos(theta));
	d.gainR = pad.gain * static_cast<float>(std::numbers::sqrt2 * std::sin(theta));
	if (pad.pan == 0.0f) {
		d.gainL = pad.gain;
		d.gainR = pad.gain;
	}
}

void OscVoice::renderDrumSpan(float* l, float* r, int span) {
	DrumRoadState& d = *drum_;
	if (r != nullptr) {
		// THE STEREO ROAD: the hit's own right ear (Width's decorrelated wires or clap band), then the pad's pan.
		d.hit.render(l, span, r);
		for (int i = 0; i < span; ++i) {
			r[i] *= d.gainR;
			l[i] *= d.gainL;
		}
	}
	else {
		d.hit.render(l, span);
		for (int i = 0; i < span; ++i) {
			l[i] *= d.gainL;
		}
	}
	// THE LAYER (stage (c)): struck at the sample its Delay ends, through its high-pass, at its Level on the pad's Level and pan.
	if (!d.layerPending && d.layer.idle()) {
		return;
	}
	int from = 0;
	if (d.layerPending) {
		if (d.layerDelayLeft >= span) {
			d.layerDelayLeft -= span;
			return;
		}
		from = d.layerDelayLeft;
		d.layerDelayLeft = 0;
		d.layerPending = false;
		d.layer.strike(d.layerKind, d.layerLaws, d.seed ^ 0x1A7E2C0FFEEull, static_cast<float>(std::clamp(d.velocity, 0.0, 1.0)));
		d.layerReleaseApplied = false;
		++d.layerStrikes;
	}
	const int n = span - from;
	d.layer.render(d.layerL, n, r != nullptr ? d.layerR : nullptr);
	const auto pass = [&](float* x, int ear) {
		if (d.layerHpA >= 1.0f) {
			return;
		}
		float px = d.layerHpX[ear];
		float py = d.layerHpY[ear];
		for (int i = 0; i < n; ++i) {
			const float y = d.layerHpA * (py + x[i] - px);
			px = x[i];
			py = y;
			x[i] = y;
		}
		d.layerHpX[ear] = px;
		d.layerHpY[ear] = py;
	};
	pass(d.layerL, 0);
	const float gl = d.layerGain * d.gainL;
	for (int i = 0; i < n; ++i) {
		l[from + i] += gl * d.layerL[i];
	}
	if (r != nullptr) {
		pass(d.layerR, 1);
		const float gr = d.layerGain * d.gainR;
		for (int i = 0; i < n; ++i) {
			r[from + i] += gr * d.layerR[i];
		}
	}
}

[[gnu::noinline]] void OscVoice::renderDrumPure(int chunk, bool stereoRender) {
	++drum_->chunks;
	settlePartialsOwed();
	decimatorsIdle_ = true; // nothing feeds the 4x decimators on this road (renderPartialsPure's reason)
	int s = 0;
	while (s < chunk) {
		if (drum_->controlLeft <= 0) {
			if (gliding_) {
				advanceGlideBy(kStringControlBlock);
			}
			advancePhaseBy(frequencyHz_ / hostSampleRate_, kStringControlBlock);
			controlDrum(*fresh_);
			drum_->controlLeft = kStringControlBlock;
		}
		const int span = std::min(drum_->controlLeft, chunk - s);
		renderDrumSpan(carry_ + s, stereoRender ? carryR_ + s : nullptr, span);
		if (fadeLeft_ > 0) {
			fadeLeft_ = std::max(0, fadeLeft_ - 4 * span);
		}
		readIndex_ += span;
		drum_->controlLeft -= span;
		s += span;
	}
}

[[gnu::noinline]] void OscVoice::addDrumMixed(int chunk, int fadeLeft0, bool stereoRender) {
	++drum_->chunks;
	const bool freshD = fresh_->engine == OscEngine::Drum;
	const PreparedCharacter& side = freshD ? *fresh_ : *stale_;
	alignas(16) float l[Halfband4x::kMaxBlock] {};
	alignas(16) float r[Halfband4x::kMaxBlock] {};
	int s = 0;
	while (s < chunk) {
		if (drum_->controlLeft <= 0) {
			controlDrum(side);
			drum_->controlLeft = kStringControlBlock;
		}
		const int span = std::min(drum_->controlLeft, chunk - s);
		renderDrumSpan(l + s, stereoRender ? r + s : nullptr, span);
		drum_->controlLeft -= span;
		s += span;
	}
	// addStringMixed's weights from the one t.
	const double total = static_cast<double>(fadeTotal_);
	for (int h = 0; h < chunk; ++h) {
		double sum = 0.0;
		for (int j = 4 * h; j < 4 * h + 4; ++j) {
			sum += j < fadeLeft0 ? static_cast<double>(fadeTotal_ - fadeLeft0 + j + 1) / total : 1.0;
		}
		const double t = 0.25 * sum;
		const auto weight = static_cast<float>(freshD ? t : 1.0 - t);
		carry_[h] += weight * l[h];
		if (stereoRender) {
			carryR_[h] += weight * r[h];
		}
	}
}

void OscVoice::generateChunk(int chunk, bool stereo) {
	// THE DRUM ROAD FIRST (drum proposal §1): ahead of the string's, because the drum's String kind plays the voice's string
	// and the string road would reset a string it is not driving.
	const DrumRoad droad = drumRoad();
	if (droad == DrumRoad::Pure) {
		renderDrumPure(chunk, stereo);
		return;
	}
	if (droad == DrumRoad::None && !drum_->hit.idle()) {
		drum_->hit.reset(); // left the road: nothing will read it
	}
	if (droad == DrumRoad::None && (!drum_->layer.idle() || drum_->layerPending)) {
		drum_->layer.reset();
		drum_->layerPending = false;
	}
	// THE STRING ROAD FIRST (KS design §8 S4). AN ENGINE SWITCH PLUCKS NOTHING (by design, K12): a
	// held note switched into Pluck crossfades into its silent string, and the next note plucks.
	const StringRoad sroad = stringRoad();
	if (sroad == StringRoad::Pure) {
		renderStringPure(chunk, stereo);
		return;
	}
	if (sroad == StringRoad::None && !ks_->string.idle()) {
		ks_->string.reset(); // left the road: nothing will read it, and a return is a pluck
		ks_->lawsSet = false;
		ks_->hz = -1.0;
	}
	// THE PARTIALS ROAD, decided here where every other block decision is (see partialsRoad), into the same carry.
	const PartialsRoad road = partialsRoad();
	if (road == PartialsRoad::Pure) {
		renderPartialsPure(chunk, stereo);
		return;
	}
	// THE 4x PATH (NONE is the code that was here, with these few flag tests around it). The accumulator and the glide
	// catch up first on what a pure stretch owed them, since this loop advances them per evaluation sample from here.
	settlePartialsOwed();
	if (decimatorsIdle_) {
		decimator_.reset();
		decimatorR_.reset();
		decimatorsIdle_ = false;
	}
	const int fadeLeft0 = fadeLeft_;
	const int readIndex0 = readIndex_;
	if (stereo) {
		for (int i = 0; i < 4 * chunk; i++) {
			nextSamplePair(oversampled_[i], oversampledR_[i]);
		}
		decimator_.decimate(oversampled_, carry_, chunk);
		decimatorR_.decimate(oversampledR_, carryR_, chunk);
	}
	else {
		for (int i = 0; i < 4 * chunk; i++) {
			oversampled_[i] = nextSample(4);
		}
		decimator_.decimate(oversampled_, carry_, chunk);
	}
	if (road == PartialsRoad::Mixed) {
		addPartialsMixed(chunk, fadeLeft0, readIndex0, stereo);
	}
	else {
		partialsPrimed_ = false; // off the road: a return to it is a note-on for the bank
	}
	if (sroad == StringRoad::Mixed) {
		addStringMixed(chunk, fadeLeft0, stereo);
	}
	if (droad == DrumRoad::Mixed) {
		addDrumMixed(chunk, fadeLeft0, stereo);
	}
}

void OscVoice::beginStereoStream() {
	// Until this moment the right channel WAS the mono stream, duplicated by whoever mixed it. The
	// right ear therefore continues that stream: same decimator history, same undelivered carry —
	// which is what makes a width turned up mid-note click-free by construction, with the rebuild
	// crossfade left to mask only what it always masks. A plain copy: the decimator is a few arrays of
	// floats, and the compile-time check keeps it that way (no allocation, nothing to alias).
	static_assert(std::is_trivially_copyable_v<Halfband4x>,
	              "the right ear adopts the left's decimator by copying its state");
	decimatorR_ = decimator_;
	std::copy(carry_ + carryRead_, carry_ + carryRead_ + carryCount_, carryR_ + carryRead_);
	stereoStream_ = true;
	gridsDirty_ = true; // the right ear's grids have not been resolved while the flag was down
}

void OscVoice::renderStereo(float* outL, float* outR, int n) {
	if (!stereoStream_) {
		beginStereoStream();
	}
	// render()'s loop with the right ear alongside: same carry-first order, same sync point, same
	// chunk rounding, so the generated-sample schedules the header promises are the same schedules.
	weaveIndex_ = carryCount_;
	weaveSub_ = 0;
	readIndex_ = carryCount_;
	readSub_ = 0;
	int done = 0;
	while (done < n) {
		if (carryCount_ > 0) {
			const int take = std::min(carryCount_, n - done);
			std::copy_n(carry_ + carryRead_, take, outL + done);
			std::copy_n(carryR_ + carryRead_, take, outR + done);
			carryRead_ += take;
			carryCount_ -= take;
			done += take;
			continue;
		}

		if (!syncToBank(4)) {
			std::fill_n(outL + done, n - done, 0.0f);
			std::fill_n(outR + done, n - done, 0.0f);
			return;
		}

		const int want = std::min(n - done, Halfband4x::kMaxBlock);
		const int chunk = (want + 3) & ~3;
		generateChunk(chunk, true);
		carryCount_ = chunk;
		carryRead_ = 0;
	}
}

void OscVoice::render(float* out, int n) {
	// A stereo stream that was running is over: the caller has gone back to mono, and the next
	// stereo render must re-adopt the (by then shared) stream rather than trust a right ear that
	// stopped advancing here. One store, nothing the mono arithmetic reads.
	stereoStream_ = false;
	// The carry already holds the chunk's first samples, so the next one GENERATED is that far in.
	weaveIndex_ = carryCount_;
	weaveSub_ = 0;
	readIndex_ = carryCount_;
	readSub_ = 0;
	int done = 0;
	while (done < n) {
		if (carryCount_ > 0) {
			const int take = std::min(carryCount_, n - done);
			std::copy_n(carry_ + carryRead_, take, out + done);
			carryRead_ += take;
			carryCount_ -= take;
			done += take;
			continue;
		}

		// Checked here rather than once at the top, so a character change takes effect at the first
		// GENERATED sample after it arrives. That instant is the same one at every block size —
		// generated position is always ceil4(consumed) — where a check per render call would land it
		// wherever the host's buffer boundaries happened to fall.
		if (!syncToBank(4)) {
			std::fill_n(out + done, n - done, 0.0f);
			return;
		}

		// Round up to the multiple of four the decimator insists on. kMaxBlock is itself a multiple of
		// four, so rounding up can never overshoot the chunk limit.
		const int want = std::min(n - done, Halfband4x::kMaxBlock);
		const int chunk = (want + 3) & ~3;
		generateChunk(chunk, false);
		carryCount_ = chunk;
		carryRead_ = 0;
	}
}

// A Partials side renders SILENCE here (sideEval -> eval answers 0): the bank's road is host rate already, so there
// is no oversampling to remove, and the alias test this exists for never plays one.
void OscVoice::renderWithoutOversamplingForTest(float* out, int n) {
	weaveIndex_ = 0;
	weaveSub_ = 0;
	readIndex_ = 0;
	readSub_ = 0;
	if (!syncToBank(1)) {
		std::fill_n(out, n, 0.0f);
		return;
	}
	for (int i = 0; i < n; i++) {
		out[i] = nextSample(1);
	}
}

} // namespace feathers

namespace feathers {

int OscVoice::partialsCycleGains(int cap, float* gain) const {
	// THE PARTIALS THAT RENDER AT THIS PITCH, each at its Nyquist fade — controlPartials' own loop (the ratios never
	// decrease, so the first silenced partial ends it), read from the fresh side's spectrum.
	// NOT pitchPartials, ON PURPOSE FOR NOW: this is the scope's and the stack coherence's cycle OF THE TERRAIN, read with
	// no drawer (it ignores the Partials count, Stretch and Shift alike, and a stretched or shifted spectrum has no one cycle
	// to draw). With no drawer, pitchPartials' loop is this loop; backlog LY carries the open question of drawing the
	// drawer here (spectral-ops review, 2026-10-12).
	const partials::PartialSpectrum& spec = fresh_->partials;
	const double perRatio = 2.0 * std::numbers::pi * frequencyHz_ / hostSampleRate_;
	const int limit = std::min(cap, partials::kMaxPartials);
	int count = 0;
	for (; count < limit; ++count) {
		const float w = static_cast<float>(perRatio * static_cast<double>(spec.ratio[static_cast<size_t>(count)]));
		const float f = partials::nyquistFade(w);
		if (!(f > 0.0f)) {
			break;
		}
		gain[count] = f;
	}
	return count;
}

float OscVoice::partialsCycleSample(double phase01) const {
	alignas(16) float gain[partials::kMaxPartials] {}; // zeroed: GCC cannot see that only gain[0, count) is read
	const int count = partialsCycleGains(kPartialsCycleCap, gain);
	const partials::PartialSpectrum& spec = fresh_->partials;
	double sum = 0.0;
	for (int k = 0; k < count; ++k) {
		const double a = static_cast<double>(spec.envAtRatio[static_cast<size_t>(k)])
		                 * static_cast<double>(spec.fine[static_cast<size_t>(k)]) * static_cast<double>(gain[k]);
		const double theta = std::numbers::pi * static_cast<double>(k) * static_cast<double>(k) / static_cast<double>(kPartialsCycleCap);
		sum += a * std::sin(2.0 * std::numbers::pi * static_cast<double>(spec.ratio[static_cast<size_t>(k)]) * phase01 + theta);
	}
	return static_cast<float>(sum);
}

void OscVoice::playingCycle(float* out, int points) const {
	if (fresh_ == nullptr) {
		std::fill_n(out, points, 0.0f);
		return;
	}
	if (fresh_->engine == OscEngine::Partials) {
		alignas(16) float gain[partials::kMaxPartials] {}; // zeroed for GCC, as in partialsCycleSample
		// THE SAME CAP AND THE SAME FIXED N as partialsCycleSample, so the two roads are one cycle.
		const int count = partialsCycleGains(kPartialsCycleCap, gain);
		partials::cycleOf(fresh_->partials, count, kPartialsCycleCap, gain, points, static_cast<double>(points), out);
		return;
	}
	for (int i = 0; i < points; ++i) {
		out[i] = sideEval(*fresh_, stairGrid_, static_cast<double>(i) / static_cast<double>(points));
	}
}

} // namespace feathers
