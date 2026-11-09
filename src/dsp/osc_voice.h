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

#pragma once

#include "dsp/character_bank.h"
#include "dsp/halfband.h"
#include "dsp/drum_voice.h"
#include "dsp/ks_string.h"
#include "dsp/modifiers.h"
#include "dsp/partial_bank.h"
#include "dsp/spectral_shaper.h"
#include "dsp/weave_string.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>

// One oscillator voice: a phase accumulator, the oversampling that makes continuous evaluation of a
// hard-edged waveform survive being sampled, and a position within whatever crossfade the shared
// CharacterBank is running. The tables themselves are the bank's — see character_bank.h for why
// they are shared and this is not.
//
// Everything here is plugin-owned. In particular the crossfade logic is ours: on hardware the
// firmware drives its equivalent from AudioEngine::audioSampleTimer, but both float evaluators are
// pure functions of params and phase, and nothing in this file reads or advances that timer. It
// exists in the shim solely so phi_stair.cpp links.
//
// NOTHING HERE MAY DEPEND ON HOW THE HOST CHOPS ITS BLOCKS. Every schedule the voice runs — the
// fade, the glide, the mip switch, the noise hold — advances on generated samples, and the count of
// those is a function of how many samples the caller has consumed and nothing else: the decimator
// works in fours, so the voice has always generated exactly ceil4(consumed) of them, whatever
// sequence of block lengths got it there.

namespace feathers {

/// Concert A, and the key that plays it. The one place the tuning reference is written down.
inline constexpr double kConcertAHz = 440.0;
inline constexpr double kConcertAKey = 69.0;

/// Pitch in semitones (69 = A4 = 440 Hz) to frequency, and back.
///
/// Semitones rather than Hz is the currency for anything that MOVES pitch — glide, bend, transpose —
/// because those are all linear there and none of them are in Hz.
double pitchToHz(double semitones);
double hzToPitch(double hz);

/// The soonest two bakes may be, in generated samples — a RATE CAP, not the schedule itself.
///
/// What decides whether to bake at all is the cache key (see OscVoice::voxKey_): a note that has
/// not moved has nothing new to bake, and that is the case almost every voice is in almost all the
/// time. This is what stops a fast glide from turning the key into a treadmill — three octaves in a
/// quarter second crosses a key step every eighth of a host sample, which would bake four times
/// more often than the measured cost can afford.
///
/// A chunk's worth, written against the decimator's own block so the two cannot drift apart: at 4x
/// and 48 kHz, 32 host samples, about 0.67 ms. Far finer than the ~20 ms formant slew the recipe
/// itself carries.
inline constexpr int kVoxBakeIntervalSamples = 4 * Halfband4x::kMaxBlock;

/// Steps per octave the bake's fundamental is quantised to, which is what makes a parked note free.
///
/// CHOSEN FROM WHAT THE GRID CAN SEE, not from a guess about hearing. The bake depends on
/// formantHz/fundamentalHz, and what the 256-slot grid actually resolves is WHERE EACH PULSE LANDS:
/// pulse k starts at phase k/ppc, so a relative change e in the fundamental moves that edge by
/// k·e/ppc cycles. The pulse index sticks at kVoxMaxPulses − 1 and a pulse only lands inside the
/// cycle while k < ppc, so the largest edge movement over the whole audible burst is
/// min(15, ppc)·e/ppc — at most e cycles, for ANY formant and ANY note. Keep e below half a slot,
/// 1/512, and no pulse edge can move more than half a slot between bakes.
///
/// 512 steps per octave is a step of 2.34 cents, comfortably inside that bound and a clean power of
/// two. It is a statement about the NOTE, so it is deterministic given the note and therefore the
/// same at every block size — which is the property that let it replace a per-chunk decision.
inline constexpr double kVoxBakeStepsPerOctave = 512.0;

/// The bake's identity: which BUILD of a character, and which note to within
/// kVoxBakeStepsPerOctave.
///
/// A voice rebakes exactly when this changes — so a parked note bakes ONCE and never again, which
/// is the difference between the vox road costing three quarters of a core at sixteen voices and
/// costing nothing.
///
/// THE CHARACTER HALF IS A BUILD SERIAL AND NOT A POINTER, and the difference is a bug that
/// shipped. The bank double-buffers into tables_[2][2] and flips on every build, so only two
/// addresses per ear ever exist: the third build lands back on the first's address, a pointer key
/// compares EQUAL to a bake two rebuilds old, and the voice never rebakes. Moving the puck through
/// vowel country then stopped changing the sound at all until a new note retriggered the voice —
/// which is exactly how it was reported. A serial cannot do that, because contents and serial move
/// together by construction.
///
/// The two ears still dedupe by POINTER inequality where they are compared as objects
/// (freshR_ != fresh_), which is a different question — "are these the same table" — and is not
/// cached across time, so slot reuse cannot reach it.
struct VoxBakeKey {
	uint64_t buildSerial = 0;
	long note = 0;

	friend bool operator==(const VoxBakeKey&, const VoxBakeKey&) = default;
};

/// The note half of that key. Not a note at all quantises to a fixed value rather than a NaN.
inline long voxBakeNoteKey(double fundamentalHz) {
	if (!(fundamentalHz > 0.0) || !std::isfinite(fundamentalHz)) {
		return std::numeric_limits<long>::min();
	}
	return std::lround(std::log2(fundamentalHz) * kVoxBakeStepsPerOctave);
}

/// A KEY'S NOTE FOR A GRID COPIED RATHER THAN BAKED (`OscVoice::syncToBank`, the bake budget): no real note has it, so
/// the copy never passes for a bake of any character.
inline constexpr long kVoxCopiedNote = std::numeric_limits<long>::min() + 1;

/// ONE GRID THIS VOICE BAKED FOR ITSELF — the note-dependent half of vox (decision V22a).
///
/// THIS PARAGRAPH ONCE SAID "ONE MIP, NOT THREE", eight lines above the member comment that says
/// three and explains why one was a hazard. It was the design before a glide was thought through,
/// and it was not revisited when the design changed — the ordinary way a comment becomes a lie, and
/// the same shape `device_instance_pool.h` records finding in itself: one file, two answers, and the
/// wrong one on top where a reader meets it first. The argument it made (the bank cannot know which
/// rung a voice wants, and a voice can) is still true and still worth reading; what stopped being
/// true is the conclusion, because `stairMipForFrequency` is re-resolved MID-BLOCK. See `slots`.
///
/// FIXED-SIZE, decision V10b: this lives on a voice, on the audio thread, and nothing after activate()
/// may allocate. The trailing guard slot is the house grid contract, so the sample loop's idx + 1
/// needs no wrap test.
struct VoxGrid {
	/// ALL THREE MIPS, and a first draft baked only the one the note was playing. That was wrong,
	/// and a glide is what breaks it: advanceGlide moves the pitch every evaluation sample and
	/// refreshGrids re-resolves stairMipForFrequency from the live frequency, so a glide crossing
	/// 700 Hz or 1800 Hz changes the rung MID-BLOCK, after the bake — and the voice would then read
	/// a grid it never baked. The saving was not worth defending anyway: the two binomial passes are
	/// about two thousand flops against a bake costing tens of thousands, so the hazard was bought
	/// for roughly nothing.
	float slots[3][kFlagshipSlots + 1] {};

	/// What this bake IS — the character and the note it was baked for. A bake whose key still
	/// matches is a bake nobody needs to do again.
	VoxBakeKey key {};

	const float* mip(int level) const {
		return slots[level < 0 ? 0 : (level > 2 ? 2 : level)];
	}
};

/// THE PITCH ARITHMETIC OF ONE ADDITIVE SIDE AT ONE PITCH (OscVoice::controlPartials' steps 1 and 2, and the plot's): which
/// partials render, each one's increment, frequency and Nyquist fade. ONE FUNCTION FOR THE VOICE AND THE PLOT, so the plot
/// cannot draw partials the voice does not play (spectral-ops condition 4).
struct PartialPitches {
	int count{0}; ///< the partials that render: [0, count)
	/// One past the HIGHEST partial at full Nyquist gain (0: none). Not a prefix of full-gain partials under a downward
	/// Shift: the partials at or below 0 Hz have a fade of 0 and sit below it, silent. The phase control's fold is gated on
	/// that highest one.
	int full{0};
	/// EACH PARTIAL'S EDGE GAIN, or null where every one is 1: the Partials operator's top partial at its fraction (so a count
	/// moving through an integer fades a partial in rather than stepping it), and Shift's LOW-EDGE FADE (a partial shifted
	/// toward 0 Hz fades out between kLowFadeFullHz and kLowFadeZeroHz, and is silent at or below 0 Hz). Scales the raw
	/// amplitude before base is built (partialTargets), so the compensation sees the spectrum the bank renders.
	const float* edge{nullptr};
	/// EACH PARTIAL'S RATIO AS IT SOUNDS, hz / f0, for all kMaxPartials (the phase control turns every live partial by it):
	/// the spectrum's own `ratio` when no frequency operator acts, else `ratioScratch` with Stretch and Shift applied.
	const float* ratio{nullptr};
};
/// **PEAKS' KEEP MASK, GLIDING** (backlog PD, 2026-10-30): the voice's memory of each partial's Peaks keep, so a partial the
/// ranking drops or takes up ramps over `step`'s reciprocal in control blocks (one character fade, kCharacterFadeSeconds)
/// instead of being gated in one 32-sample block. The ranking flips the moment two partials cross in loudness, and a puck
/// moving between two places walks every crossing on the way, at any speed: measured on the default patch at Peaks 8, a
/// partial of ~0.07 gated over 32 samples, four times on one partial in a slow sweep of Path. Not `live` (a note-on, or
/// Peaks just switched on): the keep is taken as ranked, at once, which is the road it always had.
struct PeaksGlide {
	alignas(16) float keep[partials::kMaxPartials] {};
	/// The most any partial's keep moves in this block (frames / the fade's length); written by the caller every block.
	float step{1.0f};
	bool live{false};
};
/// `ratioScratch` and `edgeScratch` are kMaxPartials floats each that the caller owns, written only when needed; under a
/// frequency operator `ratio` is valid for the partials that render and on to `keepRatios` (the bank's live count, which
/// the phase control turns), rounded up to a group of four. `otherEar`, when the ears' spectra differ, is the right ear's: the
/// Partials operator's Peaks then ranks on both ears' energy, so both keep the same partials (P1b). `glide`, when given, smooths
/// Peaks' keep mask through it (PeaksGlide); the plot passes none and reads the ranking as it stands. `glideFrom` (the stale
/// side's ratios, kMaxPartials, during a rebuild's fade) blends the base ratios by `glideT` toward this spectrum's, backlog PD.
PartialPitches pitchPartials(const partials::PartialSpectrum& spec, const spectral::ShaperParams& drawer, double f0Hz,
                             double sampleRate, float* inc, float* hz, float* fade, float* ratioScratch,
                             float* edgeScratch, int keepRatios, const partials::PartialSpectrum* otherEar = nullptr,
                             PeaksGlide* glide = nullptr, const float* glideFrom = nullptr, float glideT = 1.0f);

/// **A-PH3: UNDER SUB PRIORITY THE SUB PARTIAL (partial 0) STAYS AT ITS OWN PITCH** — its spectrum ratio, exempt from Shift and
/// Stretch (and from Shift's low edge): its increment, frequency, Nyquist fade, sounding ratio and edge gain rewritten after
/// pitchPartials; a count of 0 (every partial shifted to or below 0 Hz) becomes 1, the sub alone. `glideFrom` and `glideT` are
/// pitchPartials' own: during a rebuild's fade the sub's ratio glides from the stale build's as every other partial's does.
void pinSubPitch(const partials::PartialSpectrum& spec, double f0Hz, double sampleRate, float* inc, float* hz, float* fade,
                 float* ratioScratch, float* edgeScratch, PartialPitches& pitches, const float* glideFrom = nullptr,
                 float glideT = 1.0f);

/// **A-PH3: THE SUB AS A LEVEL** (design option (b)) — the rest's root-sum-square OF THE SHAPE ITSELF, taken
/// UPSTREAM of every drawer operator and of the Nyquist cull: Σ (envAtRatio[i]·fine[i]·damp[i])² over [1, kMaxPartials), the
/// whole spectrum whether it renders or not. So a drawer Low Pass, Odd/Even, Comb, the Partials count or a note so high only
/// the sub fits below Nyquist leave the sub at its set level; only the shape, Damp (`damp`, the reference's per-partial gains,
/// null for none), Sub and Ceiling move it. `stale` and `t`: an engine rebuild's crossfade, the amplitudes blended as the
/// targets are (null for none).
double shapeRestRss(const partials::PartialSpectrum& shape, const partials::PartialSpectrum* stale, float t, const float* damp);
/// target[0] = level × `rest` (shapeRestRss). The bank still applies the sub's own Nyquist fade.
inline void subLevelTarget(float level, double rest, float* target) {
	target[0] = static_cast<float>(static_cast<double>(level) * rest);
}

class OscVoice {
public:
	OscVoice();

	/// The bank whose tables this voice plays. Every voice in a synth shares one; a voice with none
	/// renders silence.
	void setCharacterBank(const CharacterBank* bank);

	/// **MOVE TO ANOTHER BANK MID-NOTE, CROSS-FADING** (voice stacking step 4: a part stepping onto a stand of its own,
	/// or back onto the voice's). Unlike `setCharacterBank`, which forgets everything, the next sync treats the move
	/// as a flip: the table it was playing becomes the stale side of an ordinary character fade into the new bank's,
	/// so the move is exactly as smooth as the puck moving. The old table is safe to read for the fade: a bank
	/// writes only into its non-current slot, and two builds cannot land inside one fade guard.
	void moveToBank(const TableSource* bank) {
		if (bank == bank_ || bank == nullptr) {
			return;
		}
		bank_ = bank;
		bankMoved_ = haveSynced_;
	}
	[[nodiscard]] const TableSource* bank() const noexcept { return bank_; }
	[[nodiscard]] const TableSource* bankForTest() const noexcept { return bank_; }

	/// **WHAT THIS VOICE PLAYS NOW, AS ONE CYCLE** — the left ear's table as of its last sync, through the grid it reads
	/// (its vowel bake included), with no fade, jitter, noise or FM. Silence before the first sync. For the synth's stack
	/// coherence (`Synth::updateStackCoherence`), which compares the cycles a stack's parts play when their tables
	/// change — never per sample. The table's build serial and the bake count are what say it has changed: the
	/// pointers alone are addresses in double buffers, which is not an identity.
	///
	/// A PARTIALS SIDE HAS NO TABLE TO READ (sideEval answers 0 for it), so it answers with its SPECTRUM summed as one cycle
	/// (partials::cycleOf): the partials that render at this voice's pitch (at most kPartialsCycleCap), each at its Nyquist
	/// fade, at Newman's flat phases for a FIXED N = kPartialsCycleCap. Fixed, not this part's count: two parts with one
	/// spectrum detuned across a count boundary would otherwise sum at different phases and read as unlike (quality
	/// review, 2026-10-10). So two parts compare as the likeness of their spectra, the Nyquist gains alone carrying their
	/// pitch difference, as two tables read at one phase compare as the likeness of their shapes. TEST/SLOW ONLY for a
	/// Partials side (a sine per partial per call); the synth reads `playingCycle`, which sums the same thing in one pass.
	[[nodiscard]] float playingCycleSample(double phase01) const {
		if (fresh_ == nullptr) {
			return 0.0f;
		}
		if (fresh_->engine == OscEngine::Partials) {
			return partialsCycleSample(phase01);
		}
		return sideEval(*fresh_, stairGrid_, phase01);
	}
	/// The most partials a Partials side's playing cycle sums, and the N of its fixed Newman phases. At most half the synth's
	/// 256-point coherence reading (Synth asserts it), so the reading resolves every partial it sums. 64, NOT 128, BY
	/// MEASUREMENT (bench_coherence_read, 2026-10-10): at 128 a Partials read cost 45 us at C4 and 66 us at C2 against
	/// 1.1 us for a V5 table part, on the audio thread, and at 64 it costs 33 us. The price: partials 65 and up (present
	/// below a fundamental of ~360 Hz) do not count toward the likeness. Not measured how much a likeness moves for it.
	static constexpr int kPartialsCycleCap = 64;
	/// The same cycle, `points` samples of it at phase j / points: what the synth's coherence reads, in one pass — a
	/// Partials side sums its partials by recurrence (partials::cycleOf), not one sine per sample per partial.
	void playingCycle(float* out, int points) const;
	[[nodiscard]] const PreparedCharacter* playingTable() const noexcept { return fresh_; }
	[[nodiscard]] const float* playingGrid() const noexcept { return stairGrid_; }
	[[nodiscard]] uint32_t voxBakeCount() const noexcept { return voxBakes_; }

	/// THE RESONATE ENGINE'S STRING — the synth's ONE WeaveString, which every voice on a Weave
	/// character scans at its own pitch and phase (by design: one string, shared, continuous
	/// across notes). Null renders a Weave side as silence.
	void setWeaveString(const WeaveString* string) { weave_ = string; }
	/// RESONATE'S WIDTH: the right ear's scan head sits `tap` cycles round the ring from the left's (the
	/// firmware's dual-tap, its simple offset form). From `from` at the chunk's first sample it walks toward
	/// `target` at `perSample` cycles a host sample and stops there: a trajectory that is a function of time
	/// alone, so it is the same however the host cut its blocks. Only a Weave side reads it, and only the
	/// right ear of a stereo render.
	void setWeaveTap(double from, double target, double perSample) {
		weaveTapFrom_ = from;
		weaveTapTo_ = target;
		weaveTapRate_ = perSample;
	}
	/// **THE READ PHASE'S OFFSET FOR THE NEXT RENDER CALL, IN CYCLES** (ParamId::OscPhase, 2026-10-03). `run` holds
	/// one value per HOST sample from the call's first consumed sample, `length` of them, or is null when the
	/// offset is `constant` for the whole call. Read at the GENERATED position — a render starts that at the carry it
	/// already holds, which can run up to three samples past the call; the caller's run reaches to the end of
	/// its control tick, which the lookahead never crosses (tick ends are multiples of four), and the last value
	/// holds past `length` should it ever be short. Interpolated linearly to the oversampled rate, so a moving
	/// offset is phase modulation with no steps. Added to the READ, never to the accumulator, so the osc modes'
	/// own arithmetic is untouched; and skipped outright while it is zero and was zero, so a patch that never
	/// touches Phase renders through the arithmetic of the day before it existed. A Weave side ignores it.
	void setReadPhase(float constant, const float* run, int length) {
		readConstant_ = constant;
		readRun_ = run;
		readRunLength_ = length;
	}
	/// **PHASE WIDTH** (Q10): the right ear reads at `base + m·(1 − 2·width)` where the left reads at `base + m` —
	/// `base` the knob, `m` everything the routes add. Read only by a stereo render; at width 0 the ears agree.
	void setReadPhaseWidth(float base, float width) {
		readBase_ = base;
		readWidth_ = width;
	}
	/// **PHASE AS A STEREO DESTINATION** (backlog KU, 2026-10-05): `mR − mL`, what the stereo routes add to the right ear
	/// beyond the left, in cycles — `run` one value per host sample exactly as `setReadPhase`'s, or `constant`. ADDED to
	/// Phase Width's mirror, so the right ear reads `base + m·(1 − 2w) + (mR − mL)`; ramped to the oversampled rate on
	/// the same counters as the left ear, so it moves per sample with no step. Read only by a stereo render; zero (the
	/// default, and every patch with no stereo route into Phase) leaves the right ear the arithmetic it was.
	void setReadPhaseEar(float constant, const float* run, int length) {
		earConstant_ = constant;
		earRun_ = run;
		earRunLength_ = length;
	}
	/// **WHERE THE NEXT RENDER CALL STARTS IN THE SYNTH'S CONTROL TICK** (plan Task 9b: the voice's control blocks aligned to
	/// the synth's ticks): the tick phase, 0 .. tick − 1, of the call's first CONSUMED sample — what `setReadPhase`'s run is
	/// indexed from. The Partials road then ends every control block on a multiple of `partials::kControlBlock` in tick
	/// phase: the first block after a note-on or a flip is SHORT, so no block ever straddles a tick end, where the synth's
	/// Phase run stops (it reaches only to the tick's end) and a straddling block would ramp toward the clamped value.
	/// Negative (the default, and any caller that never says) keeps whole blocks counted from the note-on or flip, which
	/// is what a bare voice with no tick has. The tick must be a multiple of the block (the synth asserts it).
	void setTickPhase(int phase) { tickPhase_ = phase; }
	/// TEST ONLY. The length, in host samples, of the last Partials control block the voice began.
	int partialsBlockLengthForTest() const { return partialsBlockLength_; }
	/// TEST ONLY. The read offset (cycles, unwrapped) the Partials bank's phases carry after the last control block began: the
	/// Phase run's value at that block's last sample (what the 4x path's read offset has reached there).
	double partialsOffsetForTest() const { return partialsOffset_; }
	/// TEST ONLY. The right ear's offset and read phase at the last stereo evaluation.
	double readOffsetRightForTest() const { return readOffsetR_; }
	double lastReadPhaseRightForTest() const { return lastReadPhaseR_; }
	/// TEST ONLY. The offset the last evaluation read at, in cycles.
	double readOffsetForTest() const { return readOffset_; }
	/// TEST ONLY. The phase the last evaluation READ its tables at — the accumulator plus the offset when it applied,
	/// so a case can tell an offset that was computed from one that was used.
	double lastReadPhaseForTest() const { return lastReadPhase_; }
	double weaveTapForTest() const { return currentTap_; }
	const WeaveString* weaveStringForTest() const { return weave_; }
	/// The mip this voice scans the string on: 0 below 700 Hz, 1 to 1800 Hz, 2 above (WeaveString::mipForFrequency).
	int weaveMipForTest() const { return weaveMip_; }
	/// How many fade entries this voice was last handed (1 while the string is frozen).
	int weaveFadeCountForTest() const { return weaveFadeCount_; }
	/// The fade entry the voice's last evaluation read (default-constructed if none).
	WeaveString::Fade weaveFadeReadForTest() const { return currentFade_ != nullptr ? *currentFade_ : WeaveString::Fade {}; }

	/// Where the string's fade stands at each host sample of the chunk about to render: the synth fills
	/// ONE array per chunk (WeaveString::fadeAt) and every voice reads it, so no voice recomputes the
	/// smoothstep or the travel. `count` covers the chunk AND the up-to-three samples a voice generates
	/// past its end (the decimator works in groups of four) — fadeAt clamps those to the window's last
	/// sample, which is deterministic at every block size because a tick is always a chunk boundary and
	/// the group straddling it is always generated before the tick runs.
	void setWeaveFades(const WeaveString::Fade* fades, int count) {
		weaveFades_ = fades;
		weaveFadeCount_ = count;
	}

	void setSampleRate(double hostSampleRate);

	// ---- THE KARPLUS-STRONG STRING --------------------------------------------------------------------------------------
	/// The synth's pluck builder, shared by every voice (null: plucks are noise alone).
	void setPluckBuilder(ks::PluckBuilder* builder) {
		ks_->pluckBuilder = builder;
		drum_->hit.setString(&ks_->string, builder); // the drum's String kind plucks with it too
	}
	/// The engine-wide string controls, for every block from the next.
	void setStringControls(const ks::StringControls& c) { ks_->controls = c; }
	/// PLUCK at the next String block, with the noise from `seed` (design §6: derived by the synth, never incidental).
	/// A note-on and a mono retrigger pluck; a legato move does not. Ignored by every other engine.
	void pluckString(uint64_t seed, double velocity = 1.0) {
		ks_->pluckPending = true;
		ks_->seed = seed;
		ks_->velocity = velocity;
	}
	/// THE FINGER: a released string's loop moves to Release's T60 over 5 ms (K9); a retrigger lifts it again.
	void releaseString(bool released) { ks_->released = released; }
	/// TEST ONLY: the voice's string (its tuning, its loop gain, whether it is idle), how many chunks took the String road,
	/// how many plucks it made, the laws its last block used, and an engine-switch fade's String weight at host sample h.
	const ks::KsString& stringForTest() const { return ks_->string; }
	uint32_t stringChunksForTest() const { return ks_->chunks; }
	uint32_t stringPlucksForTest() const { return ks_->plucks; }
	uint64_t stringSeedForTest() const { return ks_->seed; }
	float stringPluckHardnessForTest() const { return ks_->lastHardness; }
	double stringPluckPickForTest() const { return ks_->lastPick; }
	bool stringReleasedForTest() const { return ks_->released; }
	const ks::StringLaws& stringLawsForTest() const { return ks_->laws; }
	float mixStringWeightForTest(int h) const { return ks_->mixWeight[h]; }
	/// Whether the string is silent (nothing ringing, nothing to inject): a released voice whose string is idle sounds
	/// of nothing, and the synth may end it (design §1.7).
	bool stringIdle() const { return ks_->string.idle() && !ks_->pluckPending; }

	// ---- THE DRUM HIT -----------------------------------------------------------------------------------------------------
	/// The synth's kit, read every control block (one pointer for every voice; null: a Drum side is silent).
	void setDrumKit(const drum::KitSetup* kit) { drum_->kit = kit; }
	/// STRIKE pad `pad` at the next Drum block, its noise from `seed` (derived by the synth, never incidental), at `velocity`
	/// (0..1). `keySemitones` is the key's own pitch (key and retune; no transpose, bend or expression): in Kit mode a pad's
	/// pitch is its Tune and the kit's transpose, moved by its Bend share of what
	/// the voice's pitch adds past that (the wheel, an expression, a pitch route). Ignored by every other engine.
	void strikeDrum(uint8_t pad, uint64_t seed, double velocity, double keySemitones) {
		drum_->pad = pad < 8 ? pad : 0;
		drum_->seed = seed;
		drum_->velocity = velocity;
		drum_->keySemitones = keySemitones;
		drum_->strikePending = true;
		drum_->released = false;
	}
	/// THE KEY WAS LET GO: a Gate pad damps its hit to Release; a One-shot pad ignores it.
	void releaseDrum() { drum_->released = true; }
	/// Whether the hit is silent and nothing is waiting to strike: the synth may end the voice.
	bool drumIdle() const {
		return drum_->hit.idle() && drum_->layer.idle() && !drum_->strikePending && !drum_->layerPending;
	}
	/// The pad this voice is playing.
	uint8_t drumPad() const { return drum_->pad; }
	/// TEST ONLY.
	const drum::Hit& drumHitForTest() const { return drum_->hit; }
	const drum::Hit& drumLayerForTest() const { return drum_->layer; }
	bool drumLayerPendingForTest() const { return drum_->layerPending; }
	uint32_t drumLayerStrikesForTest() const { return drum_->layerStrikes; }
	float drumLayerHighPassForTest() const { return drum_->layerHpA; }
	uint32_t drumStrikesForTest() const { return drum_->strikes; }
	uint32_t drumChunksForTest() const { return drum_->chunks; }

	/// Play `hz` from the next generated sample, ending any glide in flight.
	void setFrequency(double hz);

	/// The same in semitones, which is what a note is.
	void setPitch(double semitones);

	/// Move towards `semitones` instead of jumping there, at the rate setGlideSeconds last fixed.
	/// With glide off this is exactly setPitch.
	void glideToPitch(double semitones);

	/// **A STRAIGHT LINE TO `semitones` OVER `hostSamples` HOST SAMPLES** — a routed pitch arriving on the control tick
	/// (backlog FK): the tick window's shape, where a jump would step the pitch once a tick. Advanced per evaluation
	/// sample like a glide; `setPitch`, `glideToPitch` and a glide time switched off end it.
	void rampToPitch(double semitones, int hostSamples);

	/// Time constant of that approach. The glide is exponential in semitone space — the shape a
	/// portamento has on every synth that has one, and the shape a listener hears as constant-speed
	/// because pitch perception is logarithmic. It is advanced per evaluation sample rather than per
	/// block, which is what keeps a glide the same glide at any block size.
	void setGlideSeconds(double seconds);

	/// True while a glide is still moving. Once it converges the voice stops doing per-sample pitch
	/// work at all.
	bool isGliding() const { return gliding_; }

	double frequencyHz() const { return frequencyHz_; }
	double pitchSemitones() const { return pitch_; }

	/// Phase to zero, decimator state cleared, noise stream back to its seed, no fade in flight, and
	/// the next render adopts the bank's current table without fading into it — which is right for a
	/// voice being handed to a new note, and wrong for one that is playing.
	void reset();

	/// Where the accumulator stands, 0..1. The steal handoff reads it at terminate time so the
	/// replacing voice can adopt the running sound's phase instead of cutting across it.
	double phase() const noexcept { return phase_; }

	/// Adopt a running phase outright — the fresh-voice half of the §5.8 catchup: a voice replacing
	/// a sounding one (mono retrigger, steal) starts WHERE THE SOUND IS, then retriggerPhase() bends
	/// it home. A raw snap, so callers outside that pairing are writing a discontinuity on purpose
	/// (one test does, as its known-bad).
	void adoptPhase(double phase) noexcept { phase_ = phase - std::floor(phase); }

	/// THE MODULATOR STARTS OVER, WHATEVER THE OSC MODE SAYS ABOUT THE CARRIER — and that is measured
	/// rather than assumed. §2 of the FM doc took 64 note-ons differing ONLY in modulator phase, with
	/// the carrier reset to zero, and found the worst pairwise difference over the first 10.7 ms to be
	/// 2.40 against a full-scale swing of 2.0, at every ratio tested. A free-running modulator makes a
	/// note sound different every time you play it.
	///
	/// SO ALL THREE MODES RESET IT, including Analog, whose whole contract is that the CARRIER keeps
	/// running. That is not a contradiction: Analog's free run is what makes two notes differ by a
	/// phase relationship a player can hear as one instrument; a modulator free-running against it
	/// makes them differ by an amount nobody can predict or play. Digi's "retrigs the osc for every
	/// note" is simply not met without this.
	void restartModulator() noexcept { fmPhase_ = 0.0; }
	/// DAMP STARTS THE NOTE AGAIN (spectral_shaper.h, DampOp): a Mono retrigger restarts the envelope but not the bank, so it
	/// restarts Damp here — the next damped block is age 0 for every partial. A legato move never calls it.
	void restartDamp() noexcept { dampLive_ = false; }

	/// Where the modulator stands, for a case that needs to see it rather than infer it from a
	/// render — the seam this file's siblings already grew for pitch and phase, and for the same
	/// reason: a claim about a value cannot be recovered from a buffer that carries every value at
	/// once.
	double modulatorPhase() const noexcept { return fmPhase_; }

	/// DIGI'S VERB: back to the start, now. A deliberate discontinuity — "retrigs the osc for every
	/// note" is the mode's whole content, and the click at the seam is the character being asked for
	/// rather than a defect to be smoothed. Named rather than spelled adoptPhase(0.0) at three call
	/// sites, so the dispatch reads as the three modes it is choosing between.
	///
	/// IT CANCELS ANY BEND STILL IN FLIGHT, which is not decoration: a patch switched from Catchup to
	/// Digi mid-bend would otherwise snap to zero and then have the surviving accumulator drag it
	/// straight back off again — a restart that visibly fails to restart. The two mechanisms are
	/// alternatives, so taking one clears the other.
	void restartPhase() noexcept {
		phase_ = 0.0;
		phaseCatchup_ = 0.0;
	}

	/// §5.8, THE FIRMWARE'S HARM MECHANISM: aim the phase at its start (0) WITHOUT jumping — the
	/// signed shortest-path error is banked and bled in as a per-sample rate correction with a
	/// 16-cycle time constant (~24 cycles to substantial convergence), a brief pitch inflection
	/// instead of a waveform discontinuity. Re-triggering mid-catchup recomputes from where the
	/// phase stands, as the firmware does.
	void retriggerPhase() noexcept {
		double error = -phase_;
		if (error < -0.5) {
			error += 1.0;
		}
		phaseCatchup_ = error;
	}

	/// Seed for this voice's character-noise stream, which reset() returns to.
	///
	/// Voices own their generator rather than sharing one, so two plugin instances cannot interleave
	/// each other's draws and a render is reproducible from its seed alone. Changing the seed takes
	/// effect immediately, without disturbing phase or the fade.
	void setNoiseSeed(uint32_t seed);

	/// Render `n` host-rate samples, evaluating `4 * n` oversampled ones and decimating.
	///
	/// Allocates nothing and accepts any `n`; blocks that are not a multiple of four leave up to three
	/// already-decimated samples buffered for the next call, since the decimator can only work in
	/// multiples of four.
	void render(float* out, int n);

	/// Render `n` host-rate samples into TWO EARS — the terrain-stereo path (decision V11), for
	/// while the bank's stereo window is open. One phase, one glide, one noise stream and one fade
	/// schedule advance exactly as in render(); only the table reads fork, each ear evaluating its
	/// own prepared table through its own decimator.
	///
	/// The first call after mono rendering ADOPTS the mono stream for the right ear — decimator
	/// state and undelivered carry copied from the left — because until that moment the right
	/// channel WAS the mono stream, duplicated. Continuing it is what makes a width turned up
	/// mid-note click-free by construction; the rebuild crossfade then walks each ear from the
	/// shared table to its own. Dropping back is the same seam in reverse: the caller returns to
	/// render() only once the bank says every fade out of a stereo build has drained, and by then
	/// both ears are the same table again, so the mono stream simply continues the left one.
	void renderStereo(float* outL, float* outR, int n);

	/// TEST ONLY. The same path with the oversampling removed — evaluate straight at host rate.
	///
	/// This exists so the alias test can show its own teeth: an anti-aliasing measurement that never
	/// sees a failing case is not evidence of anything. Nothing outside tests may call it.
	void renderWithoutOversamplingForTest(float* out, int n);

	/// True while this voice is still mixing the table it faded from. Tests assert on it; the synth
	/// does not need it, because the bank's own guard already covers every voice's fade.
	bool isFadingForTest() const { return fadeLeft_ > 0; }
	/// TEST ONLY. Whether the last render evaluated two ears (a stereo stream) rather than one.
	bool stereoStreamForTest() const { return stereoStream_; }
	/// THE FM INDEX THE LAST RENDERED SAMPLE USED — crossfaded during a fade (backlog EH). A value
	/// seam, because a rendered buffer cannot say what index made it.
	float fmIndexNowForTest() const { return lastFmIndex_; }

	/// TEST ONLY. The grid the sample loop is reading for `ear`, and the one it is fading FROM.
	///
	/// The voice's own bake when the character has a vowel in it, the bank's shared grid otherwise —
	/// resolved, so this is literally what the evaluator loads rather than a second derivation of
	/// what it ought to be. Exposed because the per-voice bake is the first thing here whose
	/// correctness is not visible in a rendered buffer: a staircase built from the wrong cycle, or a
	/// grid baked at the wrong mip, still renders a perfectly plausible sound. Null before the first
	/// render. Nothing outside tests may call these.
	const float* freshGridForTest(int ear) const { return ear == 0 ? stairGrid_ : stairGridR_; }
	const float* staleGridForTest(int ear) const {
		return ear == 0 ? stairGridStale_ : stairGridStaleR_;
	}
	int mipForTest() const { return stairMip_; }
	int shapesMipForTest() const { return shapesMip_; }

	/// TEST ONLY. How many vox bakes this voice has performed since it was constructed.
	///
	/// The cost of the vox road is not the cost of one bake — it is that number times how often it
	/// happens, and how often it happens is the whole of what the cache changed. A measurement of
	/// the first without the second says nothing about a patch. Nothing outside tests may call it.
	uint32_t voxBakesForTest() const { return voxBakes_; }

	/// **THE SPECTRAL DRAWER THIS VOICE APPLIES TO A PARTIALS SIDE**, handed over by the synth every chunk (Synth::seatDrawers), resolved for this voice. The default is neutral,
	/// which is the engine's own sound to the bit (spectral_shaper.h: the identity is exact).
	///
	/// `right` IS THE RIGHT EAR'S DRAWER, or null when no S control is stereo-live (design §1.4, §4.3): then both ears read
	/// `left`, as before. A right drawer equal to the left is the same thing, and is treated as null, so the second cache
	/// only ever works while the ears really differ. Read only by a stereo render (the synth puts a stereo-live drawer on
	/// the stereo road).
	void setDrawer(const spectral::ShaperParams& left, const spectral::ShaperParams* right) {
		drawer_ = left;
		drawerStereo_ = right != nullptr && !(*right == left);
		if (drawerStereo_) {
			drawerR_ = *right;
		}
	}
	/// TEST SEAM over setDrawer: one drawer for both ears.
	void setShaperParamsForTest(const spectral::ShaperParams& p) { setDrawer(p, nullptr); }
	/// TEST ONLY. The drawer each ear was last handed (the right is the left while the ears share one).
	const spectral::ShaperParams& drawerForTest(int ear) const { return ear != 0 && drawerStereo_ ? drawerR_ : drawer_; }

	/// **THE SPECTRUM DRAWER'S PLOT, READ FROM THIS VOICE** (additive design §4.2, D2): per partial of the side the bank
	/// plays (the fresh one, or a Partials side an engine switch is fading out of), its frequency `ratio`, its `terrain`
	/// (envelope · fine · the Nyquist fade at the voice's pitch: what the bank renders with no drawer) and the bank's
	/// TARGET per ear (`left`, `right`: what it renders, the drawer and Formant Shift applied — PartialBank::target).
	/// Answers how many partials render at the voice's pitch; 0, writing nothing, when no side is Partials. Read on the
	/// audio thread, or in a flush, which never overlaps it.
	int plotPartials(float* ratio, float* terrain, float* left, float* right) const;
	/// **THE SAME READING WITH NO VOICE**: `side` (per ear; equal references for a mono build) at `f0Hz` through `drawer`
	/// (`drawerR` for a right ear whose drawer differs, else null), each ear's targets computed as a voice computes them
	/// (partialTargets) and taken through the fade and the bank's silence floor. The caches are the caller's — a held
	/// drawer recomputes no gain, and the drawer's work happens only when what it reads moves (GainCache).
	static int plotReference(const PreparedCharacter& side, const PreparedCharacter& sideR, double f0Hz, double sampleRate,
	                         spectral::GainCache<partials::kMaxPartials>& cache,
	                         spectral::GainCache<partials::kMaxPartials>& cacheR, const spectral::ShaperParams& drawer,
	                         const spectral::ShaperParams* drawerR, float* ratio, float* terrain, float* left, float* right,
	                         const spectral::VocodeEnvelope* vocode = nullptr);

	/// TEST ONLY: THE PARTIALS ROAD'S VALUES, read from the voice's own rotator bank (plan Task 9) — a partial's angle
	/// per host sample, its current amplitude per ear (0 L, 1 R; in mono R is L), its phase, and how many partials the
	/// bank is rendering (a multiple of four). Values, not a rendered buffer.
	float partialIncrementForTest(int i) const { return partialBank_.incrementForTest(i); }
	float partialAmplitudeForTest(int i, int ear) const { return partialBank_.amplitudeForTest(i, ear); }
	float partialPhaseForTest(int i) const { return partialBank_.phaseForTest(i); }
	/// TEST ONLY (backlog OZ). The right ear's phase for partial i (the shared one's unless this block's ear phase is live),
	/// whether it is live, and the ears' difference (cycles) the right ear's phases carry at the last block's end.
	float partialEarPhaseForTest(int i) const { return partialBank_.earPhaseForTest(i); }
	bool partialsEarLiveForTest() const { return partialBank_.earForTest(); }
	double partialsEarOffsetForTest() const { return partialsEar_; }
	int partialsLiveForTest() const { return partialBank_.liveForTest(); }
	/// TEST ONLY. The lowest group the bank renders (the groups below it are silent: a downward Shift's).
	int partialsFirstForTest() const { return partialBank_.firstForTest(); }
	/// TEST ONLY. How many chunks have taken the Partials road (pure or mixed) since construction: a voice that never
	/// played a Partials side reads 0, which is the proof that the other engines never enter it.
	uint32_t partialsChunksForTest() const { return partialsChunks_; }
	/// TEST ONLY. The drawer's work since construction (spectral::GainCache): prepare() runs and each factor group's
	/// recomputes, so a case can see that a held note recomputes nothing and a neutral drawer does no group work.
	spectral::GainCache<partials::kMaxPartials>::Work drawerWorkForTest() const { return drawerCache_.workForTest(); }
	/// TEST ONLY. The right ear's drawer work (its own GainCache, which works only while the ears' drawers differ).
	spectral::GainCache<partials::kMaxPartials>::Work drawerWorkRightForTest() const { return drawerCacheR_.workForTest(); }
	/// TEST ONLY. Damp's accumulated gain on partial i (1 at the note-on, then each block's decay multiplied in), and how
	/// many times its ratio powers were recomputed (only when the sounding ratios or the tilt moved: a pitch move is not one).
	float dampGainForTest(int i) const { return damp_[i]; }
	uint32_t dampRecomputesForTest() const { return dampRecomputes_; }
	/// TEST ONLY. The partial count the bank was last reset with (the partials live at note-on: design §2.4).
	int partialsResetCountForTest() const { return partialsResetCount_; }
	/// TEST ONLY. How many times the bank has been reset (primed): once per note-on, and once per return to the road.
	uint32_t partialsResetsForTest() const { return partialsResets_; }
	/// A-PH: WHERE THE NOTE-ON'S OPTIMISED PHASES COME FROM (the synth's PhaseService; null: always the closed form). Read only
	/// where the bank's phases are reset anyway — a note-on or a return to the Partials road — never on a sounding note.
	void setPhaseProfiles(const partials::PhaseProfileStore* store) { phaseProfiles_ = store; }
	/// **OE: THE SIDECHAIN'S ENVELOPE** the drawer's Vocode reads at this voice's partials (the synth's, one for every voice; null:
	/// Vocode acts as on a silent sidechain). Read each control block on the audio thread, where the synth writes it between renders.
	void setVocodeEnvelope(const spectral::VocodeEnvelope* env) { vocodeEnv_ = env; }
	/// **OE (additive design §9.2): VOCODE ON ONE SIDE'S TARGETS** — ear `ear` (0 L, 1 R: Stereo and Spread read them apart), partial
	/// i at `hz[i]` read at hz / `formant[i]` (null: 1), its shape `shape[i]`, the target multiplied by the gain (also written to
	/// `gainOut` when given); under Sub priority partial 0 is left to the sub (by design). A null `env` is a silent sidechain. Nothing at
	/// all while the drawer's Vocode is off.
	static void applyVocode(const spectral::ShaperParams& drawer, const spectral::VocodeEnvelope* env, int ear, const float* hz,
	                        const float* formant, const float* shape, const float* fade, int count, bool subPriority, double f0Hz,
	                        float* target, float* gainOut = nullptr);
	/// TEST ONLY: how many times this voice computed its Vocode gains (the memory: a block whose inputs did not move reuses them).
	uint32_t vocodeComputesForTest() const { return vocodeComputes_; }
	/// **A-PH3: THE SPECTRUM A NOTE AT `f0Hz` WOULD START WITH** — the pitches (the sub pinned under Sub priority), the drawer at that
	/// pitch (keytracked operators included), Formant Shift, the sub's level, each through its Nyquist fade: exactly what a
	/// note-on weighs (Damp aside, which is 1 at a note-on). The synth posts it to the optimiser (Synth::postPhaseRequest).
	/// Writes `out[0, count)`, returns count. No allocation; `cache` is the caller's.
	/// OE: and Vocode at `vocode` (null: the sketch without it, as a note-on with Vocode off weighs).
	static int sketchTargets(const PreparedCharacter& side, const spectral::ShaperParams& drawer, double f0Hz, double sampleRate,
	                         spectral::GainCache<partials::kMaxPartials>& cache, float* out,
	                         const spectral::VocodeEnvelope* vocode = nullptr);
	/// TEST ONLY: how many note-ons took an optimised profile, and the ceiling scale the current note took (1 unless Sub
	/// priority had to turn it down).
	uint32_t profileNoteOnsForTest() const { return profileNoteOns_; }
	float ceilingScaleForTest() const { return ceilingScale_; }
	/// TEST ONLY. AN ENGINE-SWITCH FADE'S TWO WEIGHTS at host sample `h` of the last chunk that took the MIXED road:
	/// the 4x side's (`table`, the mean of its four evaluations' weights) and the bank's (`partials`). They sum to 1 by
	/// construction; this is the seam that lets a case say so.
	void mixWeightsForTest(int h, float& table, float& partials) const {
		table = mixTableWeight_[h];
		partials = mixPartialsWeight_[h];
	}

	/// **READ ANOTHER VOICE'S VOWEL BAKE INSTEAD OF BAKING ONE** (voice stacking, backlog KE step 4; the doc's §3.10).
	/// A stacked note's parts stand on one stand, so their vowels are the same vowel: part 0 bakes it and every
	/// other part reads that bake at its own pitch and its own mip. Null (the default) is a voice that bakes its own.
	///
	/// WHAT MAKES THE LENDER'S GRIDS RIGHT FOR THE BORROWER: both are synced to the same bank in the same chunk,
	/// the lender first (it renders first), so both hold the same fresh and stale characters, and the borrower
	/// resolves the lender's CURRENT slot every time it refreshes rather than remembering one. A borrower born
	/// mid-note (reset, first sync) has nothing to fade from, so it never reads the stale side the lender may lack.
	void shareVoxFrom(const OscVoice* lender) {
		if (voxLender_ != lender) {
			voxLender_ = lender;
			gridsDirty_ = true;
			voxBakeDue_ = 0;
		}
	}
	[[nodiscard]] const OscVoice* voxLender() const noexcept { return voxLender_; }
	[[nodiscard]] const OscVoice* voxLenderForTest() const noexcept { return voxLender_; }
	/// TEST ONLY. The frequency this voice's next bake is made at.
	[[nodiscard]] double voxBakeHzForTest() const { return voxBakeHz(); }

	/// **HOLD OFF REBAKING** (voice stacking step 4: the own-stand bake budget, the doc's §3.11). While set, a side that
	/// already holds a bake keeps it rather than rebaking for a moved character or note; a side with none still bakes.
	/// Cleared, the next bake interval catches up at once.
	void setVoxDeferred(bool deferred) {
		if (voxDeferred_ && !deferred) {
			voxBakeDue_ = 0;
		}
		voxDeferred_ = deferred;
	}
	[[nodiscard]] bool voxDeferredForTest() const noexcept { return voxDeferred_; }
	/// Whether this voice's rebakes are under the budget at all (an own-stand baker): its OUTGOING side then never rebakes.
	void setVoxBudgeted(bool budgeted) { voxBudgeted_ = budgeted; }

	/// **BAKE THE VOWEL AT THE NOTE `semitones` BELOW THIS VOICE'S PITCH** — the note a stack is centred on, when
	/// this voice is a part fanned away from it and lends its bake to the rest (§3.10: the bake is keyed on the
	/// VOICE's note). Zero, the default, bakes at the voice's own pitch, which is every unstacked voice.
	void setVoxBakeOffset(double semitones) {
		if (voxBakeOffset_ != semitones) {
			voxBakeOffset_ = semitones;
			voxBakeDue_ = 0;
		}
	}

private:
	/// Adopt the bank's current table if it has moved on, arming a fade from the one it replaced.
	/// Returns false if there is no table to play at all.
	bool syncToBank(int oversampling);

	/// Re-resolve what the sample loop reads: which stair mip this pitch plays, and that choice
	/// applied to both prepared tables so the loop does two loads and a lerp and never re-decides
	/// which grid it is on.
	void refreshGrids();

	/// The same resolve where a vowel is in play — split off rather than branched into, so that the
	/// road every patch without one takes is the function it was before vox existed, statement for
	/// statement. `voxLive_` is the only thing standing between them.
	void refreshGridsWithVox();

	/// One side's grid: this voice's own bake when the character has a vowel in it, the bank's
	/// shared one otherwise. The same fact PreparedCharacter::eval branches on, so the grid handed
	/// over and the evaluator reading it cannot be on different roads.
	const float* gridForSide(const PreparedCharacter& c, const VoxGrid& bake) const;

	/// Bake `c` into `into` unless `into` already holds that character at that note.
	void bakeVoxIfStale(const PreparedCharacter& c, VoxGrid& into, bool outgoing = false);

	/// Forget every cached bake. Needed wherever a key could stay equal while ceasing to mean what
	/// it meant — a new bank, or a voice handed to a new note.
	void invalidateVoxBakes();

	/// THE NOTE, BAKED — once per chunk, decision V22c(e).
	///
	/// Deliberately not per sample and deliberately not per note: a glide moves the pitch every
	/// evaluation sample and marks the grids dirty every evaluation sample, and 256 runs of the
	/// operator chain per sample is not a thing that can exist. The chunk boundary is where this
	/// voice already makes its block decisions, so the note enters the bake there.
	void updateVoxGrids();

	/// Bake `c`'s vowel for THIS voice's current note and mip, through the operator chain.
	void bakeVoxInto(const PreparedCharacter& c, VoxGrid& into);

	/// One evaluation at the current phase, then advance. `oversampling` picks which rate the phase
	/// increment, the glide and the noise hold are all against.
	float nextSample(int oversampling);

	/// One evaluation of BOTH EARS at the current phase, then advance — the pair twin of nextSample,
	/// always at 4x. Kept beside it rather than folded into it: the mono loop is today's render to
	/// the bit and stays untouched (the width-0 guarantee is structural), so a change to either of
	/// these must be mirrored in the other. Shared state — phase, glide, noise hold, fade — advances
	/// ONCE here; only the table reads fork, through earSample.
	void nextSamplePair(float& outL, float& outR);

	/// One ear's evaluation at `phase`: its own jitter depth into the eval phase, its own fresh and
	/// stale tables mixed at the shared fade position, its own grit depth on the way out. The per-ear
	/// body of both sample loops' arithmetic, with the held noise draws shared — one listener's
	/// random world, read at two depths.
	/// `polarity` is this ear's modulator sign for FM's stereo flip: +1 on the left, (1 - 2w) on the
	/// right. See fmOffsetCycles in the .cpp for why it is a scalar rather than a bool.
	float earSample(const PreparedCharacter* fresh, const PreparedCharacter* stale, const float* grid,
	                const float* gridStale, double phase, bool fading, double t,
	                double polarity, double weaveTap = 0.0) const;

	/// First stereo render after mono ones: the right ear adopts the left's decimator state and
	/// undelivered carry, so the right channel continues the mono stream it was a duplicate of.
	void beginStereoStream();

	/// Advance the glide by one evaluation sample. Called only while one is running.
	void advanceGlide(int oversampling);
	/// THE SAME GLIDE, `hostSamples` host samples at once — the Partials road's control-rate pitch (plan Task 9). The
	/// same arithmetic in closed form: the ramp closes `hostSamples` of its remaining host samples' share (a straight
	/// line, so one step of n is n steps of one), and the exponential glide's remaining distance falls by
	/// exp(−n / (τ·rate)), which is n of advanceGlide's steps at any oversampling. Called only while one is running.
	void advanceGlideBy(int hostSamples);

	// ---- THE PARTIALS ROAD (plan Task 9; design §1.3, §5.2-§5.4) ---------------------------------------------------
	// A Partials side is not a cycle: it is the voice's own rotator bank, rendered at HOST rate into the carry, so
	// the carry/sync schedule every other engine relies on is unchanged. Three roads per chunk, decided at the sync:
	//   - NONE: no side is Partials. The code that was here before, untouched.
	//   - PURE: the fresh side is Partials and it is not fading from another engine (a Partials -> Partials rebuild
	//     included: no crossfade, the bank retargets over the bank's fade length, design §1.3).
	//   - MIXED: an engine switch, one side Partials and the other not. The other side renders through the 4x path
	//     weighted by the fade's t as it always has (sideEval answers 0 for the Partials side), and the bank adds
	//     the Partials side at host rate with the complementary weight.
	enum class PartialsRoad : uint8_t { None, Pure, Mixed };
	PartialsRoad partialsRoad() const {
		const bool freshP = fresh_->engine == OscEngine::Partials;
		if (fadeLeft_ <= 0) {
			return freshP ? PartialsRoad::Pure : PartialsRoad::None;
		}
		const bool staleP = stale_->engine == OscEngine::Partials;
		return freshP && staleP ? PartialsRoad::Pure : (freshP || staleP) ? PartialsRoad::Mixed : PartialsRoad::None;
	}
	/// THE STRING ROAD, the Partials road's three cases for a String side (KS design §8 S4): PURE renders the string
	/// alone at host rate; MIXED adds it, weighted by the fade's complement, after the 4x path (where a String side
	/// evaluates to 0) has rendered the other side — and when the other side is Partials, its own mixed road adds it, so
	/// the three weights still sum to 1.
	enum class StringRoad : uint8_t { None, Pure, Mixed };
	StringRoad stringRoad() const {
		const bool freshS = fresh_->engine == OscEngine::String;
		if (fadeLeft_ <= 0) {
			return freshS ? StringRoad::Pure : StringRoad::None;
		}
		const bool staleS = stale_->engine == OscEngine::String;
		return freshS && staleS ? StringRoad::Pure : (freshS || staleS) ? StringRoad::Mixed : StringRoad::None;
	}
	/// One control block of the string: the pitch, the laws from the String side's material, the pickups, a pending
	/// pluck. `side` is the String side being played (the fresh one, or the stale one fading out).
	void controlString(const PreparedCharacter& side);
	void renderStringPure(int chunk, bool stereoRender);
	void addStringMixed(int chunk, int fadeLeft0, bool stereoRender);
	/// The string's control block, in host samples: pitch and laws move at most this often.
	static constexpr int kStringControlBlock = 16;

	/// THE DRUM ROAD, the String road's three cases for a Drum side.
	enum class DrumRoad : uint8_t { None, Pure, Mixed };
	DrumRoad drumRoad() const {
		const bool freshD = fresh_->engine == OscEngine::Drum;
		if (fadeLeft_ <= 0) {
			return freshD ? DrumRoad::Pure : DrumRoad::None;
		}
		const bool staleD = stale_->engine == OscEngine::Drum;
		return freshD && staleD ? DrumRoad::Pure : (freshD || staleD) ? DrumRoad::Mixed : DrumRoad::None;
	}
	/// One control block of the hit: its pad's laws from the Drum side's fields and the kit, a pending strike, the Gate.
	void controlDrum(const PreparedCharacter& side);
	/// Render `span` samples of the hit into `l` (and `r`), panned and at the pad's Level.
	void renderDrumSpan(float* l, float* r, int span);
	void renderDrumPure(int chunk, bool stereoRender);
	void addDrumMixed(int chunk, int fadeLeft0, bool stereoRender);
	/// One chunk of the carry by whichever road partialsRoad picks — render() and renderStereo() share it, so the two
	/// cannot disagree about the roads (the 4x loop inside is still the mono or the pair loop, untouched).
	void generateChunk(int chunk, bool stereo);
	/// The accumulator and the glide catch up on the host samples the pure road owes them (partialsOwed_).
	void settlePartialsOwed();
	/// PURE: `chunk` samples of the bank into carry_ (and carryR_ for a stereo render), control blocks on their own
	/// countdown; the fade and the read index advance per span, the accumulator and the glide at the next block.
	void renderPartialsPure(int chunk, bool stereoRender);
	/// MIXED: after the 4x path has filled carry_ for `chunk`, add the bank's Partials side weighted by the fade's
	/// complement. `fadeLeft0` is the fade's state before the 4x loop ran (that loop owns the fade and the glide);
	/// `readIndex0` the host index of the chunk's first sample.
	void addPartialsMixed(int chunk, int fadeLeft0, int readIndex0, bool stereoRender);
	/// One control block (kControlBlock host samples) of the bank: pitch -> increments -> Nyquist fade -> Formant
	/// Shift's ratios -> base -> the drawer's gains -> targets -> (at note-on) Schroeder phases from those targets ->
	/// setTargets with the Phase control's ramp folded in. `ownsFade` is the pure road, where a Partials -> Partials fade
	/// is this block's to blend; `stereoRender` whether R is wanted.
	void controlPartials(int readIndex, int frames, bool ownsFade, bool stereoRender);
	/// How long the control block starting at host index `index` of this render call is: to the next multiple of
	/// kControlBlock in tick phase when the tick phase is known (setTickPhase), a whole block otherwise.
	int partialsBlockFrames(int index) const {
		if (tickPhase_ < 0) {
			return partials::kControlBlock;
		}
		return partials::kControlBlock - (tickPhase_ + index) % partials::kControlBlock;
	}
	/// One side's targets for this block into `target` (raw amplitude × the drawer's gain; the bank applies the
	/// Nyquist fade itself, so the rendered amplitude is base × gain, base the faded one the gains were compensated
	/// against). `cache` is the ear's drawer memory (drawerCache_, or drawerCacheR_ for a right ear whose drawer differs),
	/// updated for this block, and `shaper` its prepared shaper.
	/// `edge` is pitchPartials' edge gains (null: all 1), scaling the raw amplitude before base and target are built, so
	/// the compensation sees the spectrum the bank will render.
	static void partialTargets(const PreparedCharacter& side, const spectral::GainCache<partials::kMaxPartials>& cache,
	                           const spectral::PreparedShaper& shaper, const float* fade, const float* formant, int count,
	                           const float* edge, float* target, float* shape = nullptr);
	/// The read offset's raw (unwrapped) value at host index `index` of this render call: what setReadPhase holds.
	double readOffsetAt(int index) const {
		if (readRun_ != nullptr && readRunLength_ > 0) {
			return static_cast<double>(readRun_[std::min(index, readRunLength_ - 1)]);
		}
		return static_cast<double>(readConstant_);
	}
	/// THE EARS' DIFFERENCE AT HOST SAMPLE `index` of the call, in cycles: the right ear's offset less the left's, as
	/// stepReadOffset composes them — −2w·(x − base) + (mR − mL) — unwrapped. Exactly 0 while no width and no ear route
	/// was handed over (every patch whose Phase is not stereo-live), without reading either run.
	double earOffsetAt(int index) const {
		const bool ear = earRun_ != nullptr && earRunLength_ > 0;
		if (readWidth_ == 0.0f && !ear && earConstant_ == 0.0f) {
			return 0.0;
		}
		const double x = readOffsetAt(index);
		const double e = ear ? static_cast<double>(earRun_[std::min(index, earRunLength_ - 1)])
		                     : static_cast<double>(earConstant_);
		return e - 2.0 * static_cast<double>(readWidth_) * (x - static_cast<double>(readBase_));
	}
	/// Advance the accumulator (and the modulator's) by `hostSamples` at once — the Partials road keeps `phase()`
	/// meaningful for the synth's readers (steal handoff, Analog's standing phase) without any per-sample work. The
	/// §5.8 catchup in closed form: n of advancePhase's steps scale the outstanding error by (1 − inc/16)^n.
	void advancePhaseBy(double increment, int hostSamples);


	/// One side's value at `phase`: the string for a Weave side, the side's own table otherwise. The ONE
	/// place the engine split is made, so a fade between a table and the string is the fade every other
	/// pair of sides takes.
	float sideEval(const PreparedCharacter& side, const float* grid, double phase, double weaveTap = 0.0) const {
		if (side.engine == OscEngine::Weave) {
			return weaveSample(phase + weaveTap);
		}
		// AN OPERATED SHAPE MIX (backlog KV) falls through to its baked grid, V5's road.
		if (side.engine == OscEngine::Shapes && !side.flagship.operated) {
			return side.evalShapesAt(shapesMip_, phase);
		}
		// A PARTIALS SIDE FALLS THROUGH TO eval(), WHICH ANSWERS 0 FOR IT (character_bank.cpp), and that zero is load-
		// bearing: the side is the voice's rotator bank, rendered at host rate (the Partials road), never a value at a
		// phase, and inside an engine-switch fade the 4x path still evaluates both sides — the zero is what leaves the
		// other side weighted by exactly its own share, the bank adding the complement. No arm here on purpose: this
		// runs per evaluation sample for every engine, and the answer is already the right one. (playingCycleSample
		// and playingCycle do not come here for a Partials side: they sum its spectrum, partials::cycleOf.)
		return side.eval(grid, phase);
	}
	float weaveSample(double phase) const;
	/// A Partials fresh side's playing cycle at one phase (playingCycleSample), the slow per-sample road of playingCycle.
	float partialsCycleSample(double phase01) const;
	/// The partials a Partials side's playing cycle sums and each one's Nyquist fade at this pitch, capped at `cap`.
	int partialsCycleGains(int cap, float* gain) const;
	/// Moves to the fade entry of the evaluation about to run — once per evaluation, whichever sides read it.
	void stepWeaveFade(int oversampling);
	/// The read phase's offset for the evaluation about to run — once per evaluation, as the fade above.
	void stepReadOffset(int oversampling);

	// THE READ OFFSET (setReadPhase). `readIndex_`/`readSub_` count like `weaveIndex_`/`weaveSub_`, kept apart so
	// neither road depends on the other being live. `readFrom_` is the last host sample's value, which the next
	// host sample's four evaluations ramp away from; `readPrimed_` is false on a fresh note so it starts AT its
	// value rather than gliding in from the last note's.
	float readConstant_{0.0f};
	const float* readRun_{nullptr};
	int readRunLength_{0};
	int readIndex_{0};
	int readSub_{0};
	float readFrom_{0.0f};
	float readTo_{0.0f};
	bool readPrimed_{false};
	double readOffset_{0.0};
	double lastReadPhase_{0.0};
	float readBase_{0.0f};
	float readWidth_{0.0f};
	// THE EARS' DIFFERENCE (setReadPhaseEar), ramped on `readIndex_`/`readSub_` beside the left ear's offset.
	float earConstant_{0.0f};
	const float* earRun_{nullptr};
	int earRunLength_{0};
	float earFrom_{0.0f};
	float earTo_{0.0f};
	double readOffsetR_{0.0};
	double lastReadPhaseR_{0.0};

	// The string and where this voice is in the chunk's fade array. `weaveIndex_` is the HOST sample
	// of the chunk the next generated sample belongs to — a render starts it at the carry it already
	// holds — and `weaveSub_` counts the oversampled evaluations inside that host sample.
	const WeaveString* weave_{nullptr};
	const WeaveString::Fade* weaveFades_{nullptr};
	int weaveFadeCount_{0};
	int weaveIndex_{0};
	int weaveSub_{0};
	int weaveMip_{0};
	const WeaveString::Fade* currentFade_{nullptr};
	double weaveTapFrom_{0.0};
	double weaveTapTo_{0.0};
	double weaveTapRate_{0.0};
	double currentTap_{0.0};
	const TableSource* bank_{nullptr};

	// Which tables this voice is currently playing, and fading from. Both belong to the bank; they
	// stay valid because the bank will not rebuild a slot while any voice can still be fading out of
	// it. See CharacterBank::setCharacter.
	//
	// The unsuffixed pair is EAR 0 and the whole of what the mono path reads. The R pair mirrors it
	// against the bank's ear-1 accessors on every sync, mono builds included — where they are the
	// same pointers as ear 0's, per the bank's by-build-kind rule — so a stereo render never finds
	// them stale, however long the voice rendered mono in between.
	const PreparedCharacter* fresh_{nullptr};
	const PreparedCharacter* stale_{nullptr};
	const PreparedCharacter* freshR_{nullptr};
	const PreparedCharacter* staleR_{nullptr};
	uint32_t syncedGeneration_{0};
	bool haveSynced_{false};
	/// A `moveToBank` not yet synced: the next sync fades from the old bank's table whatever the generations say.
	bool bankMoved_{false};

	// Both counted in evaluation samples, so at 4x a 2 ms fade is 4 * 96 of them at 48 kHz.
	int fadeTotal_{0};
	int fadeLeft_{0};
	/// The FM index the last sample used — see `fmIndexNowForTest`. Mutable because the stereo
	/// render is const; a seam that records what a const road computed has no other spelling.
	mutable float lastFmIndex_{0.0f};

	/// Advance the accumulator by one sample's increment, catchup correction included — THE one
	/// road both the mono and stereo renders walk, so the two cannot disagree about where the
	/// phase is (their shared-state mirror rule, made structural for this member).
	void advancePhase(double increment);

	double phase_{0.0};
	/// THE MODULATOR'S OWN ACCUMULATOR (FM doc §2/§3/§6), beside the carrier's because that is where
	/// the state layout section puts it: ratio, denominator, depth and the stereo flip are per SOUND
	/// and shared by every voice; the phase is per VOICE, like this one.
	///
	/// It exists because the engine's only existing "ratio" is the wrong form. Sync reads the source
	/// at `p * ratio`, which makes the modulator's phase a FUNCTION of the carrier's — bit-identical
	/// to a real accumulator at integer ratios, and wrong everywhere else: §2 measured 8.6x the peak
	/// sample step at 2/3 (0.674 of full scale in ONE sample at 48 kHz) and 10.6x at 1/3. The
	/// subharmonics asked for are exactly the ones the existing idiom breaks on.
	double fmPhase_{0.0};
	/// The modulator's rate as a multiple of the carrier's, refreshed from the prepared table each
	/// time the voice syncs — held here rather than read per sample so advancePhase stays one
	/// multiply, and so a table swapped mid-note cannot change the rate between two samples of the
	/// same block.
	double fmRatio_{1.0};
	/// The retrigger's outstanding phase error, in cycles, signed — the §5.8 catchup. Zero is the
	/// ordinary state and costs one branch per sample; see advancePhase.
	double phaseCatchup_{0.0};
	double hostSampleRate_{48000.0};
	double frequencyHz_{kConcertAHz};

	double pitch_{kConcertAKey};
	double pitchTarget_{kConcertAKey};
	double glideSeconds_{0.0};
	bool gliding_{false};
	/// Host samples left on a `rampToPitch` line, 0 when the approach is the exponential glide (or none).
	double rampHostSamplesLeft_{0.0};

	double glideCoeff_{0.0};
	int glideCoeffOversampling_{0};
	bool glideCoeffDirty_{true};

	/// Which stair mip the current pitch plays. Re-resolved whenever the pitch moves — which, off a
	/// glide, is never — and switched hard when it changes, as the firmware switches it.
	int stairMip_{0};
	/// THE SHAPE LADDER'S MIP for this note: chosen from the
	/// EFFECTIVE fundamental f0·(1 + 2π·I·r), I the largest FM index any live side carries, so FM never drives
	/// a partial past the host rate. Resolved in refreshGrids, which every frequency change and sync reaches.
	int shapesMip_{0};

	/// Null until the first block, and only read for stair.
	const float* stairGrid_{nullptr};
	const float* stairGridStale_{nullptr};
	/// The right ear's grids, refreshed only while a stereo stream is running — the mono path's
	/// refreshGrids stays two resolves, and it runs per evaluation sample during a glide.
	const float* stairGridR_{nullptr};
	const float* stairGridStaleR_{nullptr};
	bool gridsDirty_{true};

	/// THIS VOICE'S OWN BAKES, [buffer slot][ear] — the vox road's whole per-voice state.
	///
	/// DOUBLE-BUFFERED BY INDEX, NOT BY REBAKE. A voice fades from the character it had to the one
	/// it has, and both sides are read while the fade runs. The outgoing side was baked last block
	/// and is still exactly right, so a rearm FLIPS `voxSlot_` and banks it rather than computing it
	/// again — the swap is an xor, and only the incoming side is baked. What the stale side keeps is
	/// the note and the mip it had when the fade started, which is what a 2 ms crossfade is: the
	/// sound as it stood, mixed out.
	///
	/// EAR 1 IS ONLY EVER FILLED WHEN IT IS A SECOND CHARACTER. The note is shared by both ears —
	/// one phase, one pitch — so a note never splits the bake; terrain stereo does, because each ear
	/// reads a displaced position and so has its own vowel and its own presence. At width 0 the bank
	/// hands back ear 0's table for both and the pointers are EQUAL, and that equality is what skips
	/// the second bake — structural, the same test every other ear-1 decision here is made on.
	VoxGrid voxGrids_[2][2];
	int voxSlot_{0};
	/// The voice whose bakes this one reads instead of its own (`shareVoxFrom`), and the note offset this voice
	/// bakes at when it lends (`setVoxBakeOffset`).
	const OscVoice* voxLender_{nullptr};
	double voxBakeOffset_{0.0};
	bool voxDeferred_{false};
	bool voxBudgeted_{false};
	/// The frequency a bake is made at: this voice's, less the offset.
	[[nodiscard]] double voxBakeHz() const;
	/// Whether the lender has baked side `side` (0 fresh, 1 stale, 2 fresh right, 3 stale right) of the very table this
	/// voice reads there — then this voice reads the lender's bake of it, and bakes nothing of its own.
	[[nodiscard]] bool lentSide(int side) const;

	/// THE FADE NEEDS BOTH SIDES BAKED. A voice crossing between two vocal characters reads the
	/// outgoing bake and the incoming one at once, and a stale side left un-baked would make every
	/// character change an audible vowel swell across the 2 ms crossfade. The swap banks the
	/// outgoing bake so it is usually already valid; the key is what says whether it is.

	/// GENERATED SAMPLES UNTIL THE NEXT BAKE — a countdown, not a chunk boundary, and the difference
	/// is the header's own rule rather than a refinement of it.
	///
	/// The cadence is a chunk's worth of samples, which is what decision V22c(e) specifies. What the
	/// counter buys is that it is the SAME cadence at every block size. Baking at chunk boundaries
	/// would bake wherever the host's buffer lengths happened to fall — and a glide moves the pitch
	/// every evaluation sample, so the vowel would be baked at a different pitch, at a different
	/// sample, depending on how the host chopped its buffer. The fade, the glide, the mip switch and
	/// the noise hold all advance on generated samples for exactly this reason; the bake joins them
	/// rather than becoming the one schedule the host can reach.
	int voxBakeDue_{0};

	/// Counted for the measurement cases; the sample loop never reads it.
	uint32_t voxBakes_{0};

	/// True while any table this voice can read has a vowel in it. False is the promise that the
	/// grid resolve and the sample loop are the ones that shipped before vox, and it is false over
	/// most of the terrain — the species duty leaves half of it vowel-free by construction.
	bool voxLive_{false};

	CharacterNoise noise_;
	uint32_t noiseSeed_{CharacterNoise::kDefaultSeed};

	// One draw of each held across the whole oversampled group that becomes a single host-rate
	// sample, so the modifiers update at 48 kHz however many evaluations sit behind that. The
	// firmware draws once per output sample at 44.1 kHz; drawing once per oversampled sample instead
	// would put three quarters of the noise power above 24 kHz for the decimator to throw away, which
	// measures as 6 dB less grit than the hardware has and hears as a different, thinner character.
	//
	// The countdown is a voice member rather than a loop index because it has to survive a block
	// boundary: the group must stay aligned to the decimator's groups of four no matter how the host
	// chops its blocks up. It does, because every chunk this voice generates is a multiple of four.
	//
	// The corollary is that it assumes one oversampling factor for the life of a voice. Alternating
	// render() with renderWithoutOversamplingForTest() on the same instance mid-stream would leave a
	// partly spent 4x group to be finished at 1x, skewing one group's worth of hold. Test-only
	// concern today: nothing in the plugin path calls the 1x entry point, and the suites that do give
	// it a fresh voice.
	float heldJitter_{0.0f};
	float heldGrit_{0.0f};
	int noiseHold_{0};

	Halfband4x decimator_;
	alignas(16) float oversampled_[4 * Halfband4x::kMaxBlock];

	// Host-rate samples produced past the end of a block, because the decimator rounds up to a
	// multiple of four. At most three, but the buffer is a whole chunk so the arithmetic stays simple.
	float carry_[Halfband4x::kMaxBlock];
	int carryCount_{0};
	int carryRead_{0};

	// THE RIGHT EAR'S STREAM: its own decimator, oversample scratch and carry, consumed in lockstep
	// with the left's (one carryCount_/carryRead_ serves both — the two streams always advance
	// together). Dead weight while the voice renders mono, which is the deal terrain stereo made:
	// the default patch pays memory, never work. `stereoStream_` says whether these currently
	// continue the mono stream (beginStereoStream copies state on the way in); a mono render drops
	// the flag so the next stereo one re-adopts.
	bool stereoStream_{false};
	Halfband4x decimatorR_;
	alignas(16) float oversampledR_[4 * Halfband4x::kMaxBlock];
	float carryR_[Halfband4x::kMaxBlock];

	// ---- THE PARTIALS ROAD'S STATE (see partialsRoad) ------------------------------------------------------------
	/// THE VOICE'S ROTATOR BANK, one per oscillator (a stacked part has its own, design §5.2). Fixed-size, ~13 KB:
	/// idle weight on a voice that never plays a Partials side, which is the deal terrain stereo's right ear made.
	partials::PartialBank partialBank_;
	/// The drawer (setDrawer). Default: neutral.
	spectral::ShaperParams drawer_ {};
	/// THE RIGHT EAR'S DRAWER, live only while `drawerStereo_` (a stereo-live S control made the ears' drawers differ).
	spectral::ShaperParams drawerR_ {};
	bool drawerStereo_{false};
	/// THE DRAWER'S MEMORY (spectral_shaper.h): its per-partial factors, recomputed only when what they read moved, keyed by
	/// value. +7,232 bytes per oscillator (sizeof(OscVoice) 28,144 -> 35,376, measured 2026-10-10), idle weight like the
	/// bank's; shared by every side a block targets (both ears, a fade's stale sides), because the factors read only the
	/// params, f0 and the lead side's frequencies, and only Tilt's compensation reads each side's own amplitudes.
	spectral::GainCache<partials::kMaxPartials> drawerCache_;
	/// AND THE RIGHT EAR'S, used only while `drawerStereo_`: a drawer per ear has factors per ear. Another ~7.2 KB of
	/// idle weight per oscillator, the terrain-stereo deal again (memory, never work, for a patch whose drawer is mono).
	spectral::GainCache<partials::kMaxPartials> drawerCacheR_;
	/// DAMP'S MEMORY (spectral_shaper.h, DampOp): each partial's gain so far this note (`damp_`, 1 at the note-on, each block's
	/// factor multiplied in, exactly 0 once below the bank's silence floor); the powers ratio^tilt `dampQ_`, kept while the
	/// sounding ratios (bitwise) and the tilt hold; and the per-block factors 2^(scale·q) `dampFactor_`, kept while the
	/// block's scale holds (rate, f0, block length and sample rate in one number). Keyed by VALUE, as the GainCache is.
	/// +5,120 bytes per oscillator (four kMaxPartials float arrays, derived), idle weight like the caches'.
	alignas(16) float damp_[partials::kMaxPartials] {};
	alignas(16) float dampFactor_[partials::kMaxPartials] {};
	alignas(16) float dampRatio_[partials::kMaxPartials] {};
	alignas(16) float dampQ_[partials::kMaxPartials] {};
	double dampTilt_{-1.0};    ///< the tilt dampQ_ was raised to (−1: none yet)
	int dampQCount_{0};        ///< dampRatio_ and dampQ_ hold partials [0, dampQCount_)
	double dampScale_{0.0};    ///< the scale dampFactor_ was computed at
	int dampCount_{0};         ///< dampFactor_ holds partials [0, dampCount_); 0: none
	bool dampLive_{false}; ///< damp_ is this note's: false until the first damped block after a note-on (or the switch on)
	double dampAgeFrames_{0.0}; ///< host samples of the note's damped blocks after its first (the accumulator's age)
	int dampLiveCount_{0};     ///< the partials the last damped block decayed: [0, dampLiveCount_)
	uint32_t dampRecomputes_{0};
	/// A-PH3, SUB PRIORITY'S REFERENCE (shapeRestRss): Damp over the WHOLE spectrum at the shape's own ratios, upstream of
	/// Stretch, Shift and the cull — the same accumulator as damp_, so the sub decays in step with the rest it is set from.
	/// Kept only under Sub priority (applyDamp's `refRatio`).
	alignas(16) float dampRef_[partials::kMaxPartials] {};
	alignas(16) float dampRefRatio_[partials::kMaxPartials] {};
	alignas(16) float dampRefQ_[partials::kMaxPartials] {};
	double dampRefTilt_{-1.0}; ///< the tilt dampRefQ_ was raised to (−1: none yet)
	bool dampRefLive_{false};  ///< dampRef_ is this note's (false: the next reference block starts it at 1)
	/// Damp, one control block: start the note at 1 (on a note-on, or the switch coming on), else multiply in this block's
	/// decay, then scale the targets. Both ears by the same gain (the lead ear's drawer, design §3.2).
	/// `refRatio` (the shape's own ratios, all kMaxPartials; null off Sub priority) also advances dampRef_.
	void applyDamp(const float* ratio, double f0Hz, int count, int frames, bool noteOn, float* targetL, float* targetR,
	               const float* refRatio);
	/// FALSE UNTIL THE BANK HAS BEEN RESET FOR THIS NOTE — the note-on (`reset()`), and every return to the Partials
	/// road after leaving it, which is a note-on for the bank: Schroeder phases weighted by the note's first targets,
	/// then amplitudes snapped to them (the amp envelope owns the attack, design §5.2). A legato note or a retrigger does
	/// not reset the voice, so the bank's phases run on — and so the osc modes' phase handling (Digi's restart,
	/// Catchup's bend, Analog's lock) is inert on this engine (backlog LY).
	bool partialsPrimed_{false};
	/// HOST SAMPLES LEFT IN THE CURRENT CONTROL BLOCK — a countdown in generated samples, not a chunk boundary, for the
	/// header's rule: a chunk is as long as the host's block allows, so control blocks cut at chunk boundaries would
	/// move with the host's buffer size. A sync that flips the character zeroes it, so a new spectrum (and its fade)
	/// starts at the flip and lasts exactly the bank's fade length from there: the guard that keeps the stale slot
	/// unbuilt is that long, so the stale side is never read after it can have been rebuilt.
	int partialsControlLeft_{0};
	/// The read offset (cycles, unwrapped) the bank's phases already carry: the Phase control ramps them from here to the
	/// offset at each block's end.
	double partialsOffset_{0.0};
	/// THE OSC MODE ON THE PARTIALS ROAD (backlog LY): the accumulator's phase the bank's phases carry (cycles, unwrapped; part
	/// of every offset above), and the reference it is measured from — the accumulator as its own f0 run would have left it,
	/// advanced exactly as `phase_` is. Their difference is what a Catchup bend, a Digi restart or an Analog re-lock moved.
	double partialsAccum_{0.0};
	double partialsAccumRef_{0.0};
	/// Whether the next priming is a NOTE-ON's (reset), which starts at the accumulator's phase, or a return to the road.
	bool partialsNoteOn_{true};
	/// The current block's Phase ramp: the offset it started from and the Δ it folds (0 when the move was a step, which
	/// turned all of it at once). A flip that cuts the block short puts partialsOffset_ back to the part that turned.
	double partialsBlockStart_{0.0};
	double partialsBlockDelta_{0.0};
	/// THE EARS' DIFFERENCE THE RIGHT EAR'S PHASES CARRY (backlog OZ: earOffsetAt at the last block's end, 0 off a
	/// stereo render), and the current block's ramp of it, which a flip that cuts the block short puts back like the
	/// offset's. The right ear's rotators are derived from the shared ones at every block's start, so this is all the
	/// state the ear needs.
	double partialsEar_{0.0};
	double partialsEarStart_{0.0};
	double partialsEarDelta_{0.0};
	/// The current block's length (partialsBlockFrames), which a flip that cuts it short divides the turned part by.
	int partialsBlockLength_{partials::kControlBlock};
	/// The tick phase of this render call's first consumed sample (setTickPhase), or −1 when nobody said.
	int tickPhase_{-1};
	/// The bank's count at its last reset (the partials live at note-on), and how many resets there have been, for seams.
	int partialsResetCount_{0};
	uint32_t partialsResets_{0};
	/// A-PH (dsp/phase_profile.h): the synth's latest optimised profile, read at a note-on and never after; a copy of it (the
	/// seqlock's read target, a member so the note-on does not put 2.5 KB on the stack); the note's Sub-priority ceiling scale,
	/// fixed at its note-on; and how many note-ons took a profile (a test seam).
	const partials::PhaseProfileStore* phaseProfiles_{nullptr};
	/// OE: the synth's sidechain envelope (setVocodeEnvelope).
	const spectral::VocodeEnvelope* vocodeEnv_{nullptr};
	/// **OE2: THE VOCODE GAINS' MEMORY** (additive design §9.9): what they were computed from, by value — the envelope's serial (the
	/// synth stamps it at every change: the ABA rule's key, never its address alone), the tables' build serials and the rebuild
	/// fade's t, the note, the rate, the count, the road, and every drawer operator that moves a partial's frequency, its shape or the
	/// read (Vocode, Formant Shift, Stretch, Shift, Partials) — and the gains per ear. A block whose key matches multiplies by them and
	/// computes nothing: the envelope moves once a hop, every few control blocks. Gains equal to the bit, so no sound changes.
	struct VocodeMemo {
		const spectral::VocodeEnvelope* env{nullptr};
		uint64_t envSerial{0};
		uint64_t fresh{0}, stale{0}, freshR{0}, staleR{0};
		float blendT{0.0f};
		double f0{0.0};
		double sampleRate{0.0};
		int count{0};
		bool stereo{false};
		bool subPriority{false};
		bool enabled{false};
		spectral::VocodeOp vocodeL{}, vocodeR{};
		spectral::FormantShiftOp shiftL{}, shiftR{};
		spectral::StretchOp stretch{};
		spectral::ShiftOp shift{};
		spectral::PartialsOp partialCount{};
		friend bool operator==(const VocodeMemo&, const VocodeMemo&) = default;
	};
	VocodeMemo vocodeMemo_{};
	bool vocodeMemoValid_{false};
	uint32_t vocodeComputes_{0};
	/// The gains themselves, ON THE HEAP (allocated with the voice, on the message thread): 2.5 KB per oscillator inline would push a
	/// Synth past a test rig's stack (test_synth's Rig note).
	struct VocodeGains {
		alignas(16) float left[partials::kMaxPartials] {};
		alignas(16) float right[partials::kMaxPartials] {};
	};
	std::unique_ptr<VocodeGains> vocodeGains_;
	/// PEAKS' GLIDING KEEP MASK (backlog PD, PeaksGlide), on the heap for VocodeGains' reason (1.3 KB per oscillator).
	std::unique_ptr<PeaksGlide> peaksGlide_;
	partials::PhaseProfile profileScratch_{};
	float ceilingScale_{1.0f};
	uint32_t profileNoteOns_{0};
	/// HOST SAMPLES THE PURE ROAD HAS RENDERED SINCE THE ACCUMULATOR AND THE GLIDE LAST MOVED. They catch up at the next
	/// control block, or before the 4x path runs, in one step (settlePartialsOwed): advancing them per chunk would split
	/// one closed-form step into chunk-sized ones that round differently, and the pitch a block reads would then depend
	/// on the host's buffer size. THE PRICE: a reader between calls (phase() for the steal handoff and Analog's standing
	/// phase, the pitch seam) can see values up to 31 samples stale on the pure road; and the synth's per-tick rampToPitch
	/// is charged the previous tick's owed samples when the next block settles, which cuts each tick's corner by at most
	/// m/64 of it (m the owed samples) and still lands on every target.
	int partialsOwed_{0};
	/// The 4x decimators have not been fed since a pure Partials chunk: their state is a sound long gone, and the 4x
	/// path resets them before it next runs rather than letting that history ring into an engine switch.
	bool decimatorsIdle_{false};
	/// Whether the last control block ran the bank in stereo (the ears' spectra differ). A mono bank writes only L, so
	/// a stereo render copies L to R for those samples.
	bool partialsStereo_{false};
	uint32_t partialsChunks_{0};
	float mixTableWeight_[Halfband4x::kMaxBlock] {};
	float mixPartialsWeight_[Halfband4x::kMaxBlock] {};

	// ---- THE STRING ROAD (KS design §1, §3) ---------------------------------------------------------------------------
	/// EVERYTHING THE STRING ROAD KEEPS, BEHIND ONE POINTER (made with the voice, on the thread that builds the synth): the voice
	/// is one of 160 inside a Synth that tests build on the stack, several to a case, and this state inline (the string's own
	/// members, the controls, the laws, the fade weights) would have cost each Synth 46 KB of a stack that had about 300 to
	/// spare (MEASURED: test_terrain_stereo's case with five Synths went from passing at 8 MB to crashing).
	struct StringRoadState {
		ks::KsString string;
		/// The synth's pluck builder (shared by every voice; null: no pluck is made).
		ks::PluckBuilder* pluckBuilder{nullptr};
		ks::StringControls controls {};
		bool pluckPending{false};
		uint64_t seed{1};
		/// The pluck's note velocity, 0..1: it moves Hardness by the Velocity control's depth.
		double velocity{1.0};
		/// The Hardness the last pluck was built with (a test seam: velocity's effect read as a value).
		float lastHardness{0.0f};
		double lastPick{0.0};
		bool released{false};
		/// What the string was last given, so a block that changes nothing retunes nothing (setFrequency costs trig).
		double hz{-1.0};
		ks::StringLaws laws {};
		bool lawsSet{false};
		bool releaseApplied{false};
		double pickL{-1.0};
		double pickR{-1.0};
		int controlLeft{0};
		uint32_t chunks{0};
		uint32_t plucks{0};
		float mixWeight[Halfband4x::kMaxBlock] {};
	};
	std::unique_ptr<StringRoadState> ks_ = std::make_unique<StringRoadState>();

	// ---- THE DRUM ROAD (drum proposal §1) ----------------------------------------------------------------------------------
	/// Behind one pointer for the string road's reason: a Synth on a test's stack has kilobytes to spare.
	struct DrumRoadState {
		drum::Hit hit;
		const drum::KitSetup* kit{nullptr};
		uint8_t pad{0};
		uint64_t seed{1};
		double velocity{1.0};
		double keySemitones{69.0};
		bool strikePending{false};
		bool released{false};
		bool releaseApplied{false};
		bool prepared{false};
		int controlLeft{0};
		float gainL{1.0f};
		float gainR{1.0f};
		uint32_t chunks{0};
		uint32_t strikes{0};
		// THE LAYER SLOT (drum proposal §12, stage (c)): a second hit, struck its Delay after the pad's, at its own place's laws
		// (taken each control block like the pad's), through a one-pole high-pass, mixed into the pad's span before its Level
		// and pan. Its scratch is a control block long, the longest span renderDrumSpan is handed.
		drum::Hit layer;
		drum::Laws layerLaws{};
		drum::Kind layerKind{drum::Kind::Kick};
		bool layerPending{false};
		bool layerReleaseApplied{false};
		uint32_t layerStrikes{0};
		int layerDelayLeft{0};
		float layerGain{1.0f};
		float layerHpA{1.0f}; ///< the one-pole high-pass's coefficient; 1 bypasses it
		float layerHpX[2] {}; ///< the last input and output, per ear: x then y for L, then R
		float layerHpY[2] {};
		alignas(16) float layerL[kStringControlBlock] {};
		alignas(16) float layerR[kStringControlBlock] {};
	};
	std::unique_ptr<DrumRoadState> drum_ = std::make_unique<DrumRoadState>();
};

} // namespace feathers
