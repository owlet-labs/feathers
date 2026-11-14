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

#include "device/latency.h"
#include "plugin/device_registry.h"
#include "mod/arp/pattern_device.h" // the soak's Pattern readings (`soakReadingForTest`) only
#include "plugin/feathers_test_chain.h"
#include "plugin/synth.h"

#include "plugin/stack_index.h"
#include "plugin/character_params.h"
#include "dsp/vox_anchors.h"

#include "sst/voicemanager/voicemanager_constraints.h"
#include <cstdio>
#include <clap/events.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace feathers {

namespace {

/// WHICH OF THE DAHDSR'S DECLARED OUTPUTS ITS VALUE IS.
///
/// NAMED RATHER THAN A BARE 0, because a slot is no longer a source. A device declares what it
/// emits (device::SourceDesc) and may emit more than one — an XY pad — so an unlabelled zero at
/// these call sites would stop saying which signal was meant on the day there are two.
///
/// The static_assert is what keeps the name honest: if the envelope ever grows a second output, the
/// three publish sites below have to say which is which rather than silently all meaning the first.
constexpr size_t kEnvelopeOutput = 0;
/// BELOW THIS, A PER-VOICE ROUTE INTO LEVEL NO LONGER HOLDS A RELEASING VOICE (backlog EF) — a thousandth of
/// full level, under anything a fader could hear, so a finished tail frees its voice promptly.
constexpr double kRouteHeldFloor = 1e-3;
static_assert(std::size(mod::dahdsr::kOutputs) == 1,
              "the DAHDSR declares more than one output now — the publish sites must say which");

/// The only note port the plugin declares, so every event arrives on it. The voice manager
/// pre-allocates port 0's key state at construction and would have to allocate for any other, which
/// its own documentation forbids on the audio thread.
constexpr uint16_t kPort = 0;

/// The polyphony group every voice belongs to. Groups exist so a synth can budget its layers
/// separately; feathers has one layer, so it has one group, and group 0 is the one the manager
/// guarantees for free.
constexpr uint64_t kGroup = 0;
/// THE DRUM PADS' POLYPHONY GROUPS (drum proposal §4.2): pad N's voices are group kDrumPadGroup + N, a ROOT group of its own
/// (bounded by the pool alone, not by Voice Count), POLY, its limit the pad's Voices. The manager then steals a pad's own oldest
/// hit, through the house steal fade, when one more arrives.
constexpr uint64_t kDrumPadGroup = 0x100;

/// A voice's noise seed, from its slot and nothing else.
///
/// Distinct per voice because identical streams would sum coherently: sixteen voices drawing the
/// same jitter would move their phases in lockstep, which reads as one loud modulated oscillator
/// rather than as an ensemble. The hardware gets this for free by reseeding each voice from its own
/// phase accumulator; we cannot copy that (it would make the noise a function of block boundaries)
/// so the seed is spread deliberately instead.
///
/// Odd multiplier, so the sequence of seeds visits the whole 32-bit range rather than a subgroup of
/// it. Slot 0 keeps the firmware's own constant, which is the seed a monophonic patch will hear.
uint32_t seedForSlot(int slot) {
	return CharacterNoise::kDefaultSeed + static_cast<uint32_t>(slot) * 0x9E3779B9u;
}

/// A stacked part's noise seed (backlog KE): its voice's, spread again by the part, for the reason above — eight
/// parts drawing one stream would jitter in lockstep. Part 0 IS the voice's seed, so a stack of one is the
/// voice it always was. A different odd multiplier from the slot's, so no (slot, part) lands on another slot.
uint32_t seedForPart(int slot, int part) {
	return seedForSlot(slot) + static_cast<uint32_t>(part) * 0x85EBCA6Bu;
}

/// WHERE PART `part` OF A STACK OF `count` STANDS IN THE FAN, −½..+½ of the total detune: evenly spaced,
/// lowest first, so the middle part of an odd stack is exactly 0 — on the note, as in the firmware's
/// `setupUnisonDetuners` (voice-stacking doc §1.2). A part beyond the stack has no place in it and reads 0.
double fanPosition(int part, int count) {
	if (count <= 1 || part >= count) {
		return 0.0;
	}
	return static_cast<double>(part) / static_cast<double>(count - 1) - 0.5;
}

/// A PART'S TWO CHANNEL GAINS AT PAN `pan` (−1 left .. +1 right) — a balance law, unity at the centre and never
/// above it: the near channel stays at 1 and the far one falls to 1 − |pan| (voice-stacking doc §7, assumption
/// 11). One function for the render and its test seam, so the law read back is the law applied.
void stackPanGains(float pan, float& toLeft, float& toRight) {
	toLeft = std::min(1.0f, 1.0f - pan);
	toRight = std::min(1.0f, 1.0f + pan);
}

/// Each part's share of the level in a stack of `count`: 1/√N, so N uncorrelated parts sum to
/// the loudness of one. Exactly 1 for a stack of one.
float stackGain(int count) {
	return count <= 1 ? 1.0f : static_cast<float>(1.0 / std::sqrt(static_cast<double>(count)));
}

/// MIDI's sixteen channels and 128 keys, which are also the dimensions of the voice manager's
/// per-key state array. Anything outside them is not a note it can be asked about.
bool isPlayableChannel(int16_t channel) {
	return channel >= 0 && channel < 16;
}

bool isPlayableKey(int16_t key) {
	return key >= 0 && key < 128;
}

/// Per-sample increment of an amplitude edge lasting `seconds`. At least one sample, so a zero-length
/// edge is one sample long rather than an infinite loop, and the ramp always terminates.
float edgeStep(double seconds, double sampleRate) {
	const double samples = std::max(1.0, std::round(seconds * sampleRate));
	return static_cast<float>(1.0 / samples);
}

} // namespace

Synth::Synth(ParamSet& params)
    : params_{params}, responder_{*this}, monoResponder_{*this} {
	static_assert(sst::voicemanager::constraints::ConstraintsChecker<VoiceManagerConfig, Responder,
	                                                                 MonoResponder>::satisfies());

	partOscs_ = std::make_unique<StackOscillators>();
	standBanks_ = std::make_unique<StandBanks>();
	coherenceCycles_ = std::make_unique<CoherenceCycles>();
	phases_ = std::make_unique<PhaseService>(); // A-PH: its thread starts at activate
	phaseSketchCache_ = std::make_unique<spectral::GainCache<partials::kMaxPartials>>();
	vocodeAnalyser_ = std::make_unique<fx::spectral::SidechainEnvelope>(); // OE: prepared at activate
	for (int i = 0; i < kVoiceSlots; i++) {
		Voice& v = voices_[static_cast<size_t>(i)];
		v.slot = i;
		for (int part = 0; part < kMaxStack; ++part) {
			partOsc(v, part).setCharacterBank(&bank_);
			// EVERY PART SCANS THE ONE STRING (by design, 2026-09-26): stacked parts are detuned scan heads on it.
			partOsc(v, part).setWeaveString(&weave_);
			partOsc(v, part).setNoiseSeed(seedForPart(i, part));
			partOsc(v, part).setPhaseProfiles(&phases_->profiles()); // A-PH: read at a note-on, never after
			partOsc(v, part).setVocodeEnvelope(&vocodeEnv_); // OE: the one sidechain envelope, read at each voice's partials
			partOsc(v, part).setDrumKit(&drum_->kit); // the drum engine's kit, read every control block
			// AND EVERY PART BUT THE FIRST READS THE FIRST PART'S VOWEL BAKE (backlog KE step 4, §3.10): the parts stand on
			// one stand, so their vowel is one vowel, and a stack of eight bakes as often as a stack of one.
			if (part > 0) {
				partOsc(v, part).shareVoxFrom(&v.osc);
			}
		}
	}

	// Its constructor registers the voice-end callback and pre-allocates port 0 and group 0; the two
	// guarantees below are belt and braces, and both are no-ops on a key that already exists.
	voiceManager_ = std::make_unique<VoiceManager>(responder_, monoResponder_);
	voiceManager_->guaranteePort(static_cast<int16_t>(kPort));
	voiceManager_->guaranteeGroup(kGroup);
	// THE DRUM PADS' GROUPS, made here because making one allocates (never on the audio thread); POLY for good.
	for (uint64_t pad = 0; pad < kMaxSounds; ++pad) {
		voiceManager_->guaranteeGroup(kDrumPadGroup + pad);
		voiceManager_->setPlaymode(kDrumPadGroup + pad, VoiceManager::PlayMode::POLY_VOICES);
	}

	// THE LANE RESOLVES THROUGH FEATHERS' OWN PUBLISHED ROUTES — one list, read by both resolves.
}

void Synth::setDspConnector(
    std::function<void(const char* tag, size_t slot, void* dspStorage)> connector) {
	lane_.setDspConnector(std::move(connector));
}

void Synth::deactivate() {
	phases_->stop();
}

void Synth::activate(double sampleRate, int maxFrames) {
	// A REACTIVATION AT THE RATE ALREADY RUNNING KEEPS THE VOICES (see the header): no reset, and nothing that holds
	// state is re-prepared — the string, the oscillators, the lane's devices and modulators. What is re-derived below
	// is idempotent at an unchanged rate, and the provision at the end still adopts what the chain has gained.
	//
	// **THE LONGEST BLOCK AT THE SAME RATE IS GROW-ONLY (backlog GO, decided 2026-10-11).** The voices
	// are kept whatever the block does — the host still holds the key. The lane's devices size their scratch to the
	// block, so they are re-prepared ONLY when the new one EXCEEDS what they were prepared with: a latency restart (the
	// same block) and a SHRINK cost nothing, and every device keeps its state and its tail, because a scratch that fits
	// a longer block fits a shorter one. Only a GROWTH cuts a tail in flight, which is a buffer-size change the host
	// interrupts the audio for anyway. A new RATE re-prepares everything at the block it is handed.
	// A-PH: THE OPTIMISER'S THREAD, on the main thread where a thread may be made (a no-op if it runs already).
	phases_->start();
	const bool sameRate = activeRate_ == sampleRate;
	const bool blockGrew = maxFrames > preparedMaxFrames_;
	activeRate_ = sampleRate;
	const bool prepareLane = !sameRate || blockGrew;
	if (prepareLane) {
		preparedMaxFrames_ = maxFrames;
	}
	if (!sameRate) {
		reset();
	}
	sampleRate_ = sampleRate;
	bank_.setSampleRate(sampleRate);
	for (StandBank& stand : *standBanks_) {
		stand.setSampleRate(sampleRate);
	}
	motion_.setSampleRate(sampleRate);
	// THE CHARACTER FIRST (quality review A4), so the string is prepared under the patch's own laws and its
	// reset seats it at the patch's own rest pose rather than at phase 0's.
	updateCharacter();
	// And publishes NO shares: before a block is rendered the plugin has played nothing, and the readout
	// says so ("the synth reads back how much of each sound it is playing"). The character built here is
	// the one the first block would build; only the string needed it early.
	weights_ = {};
	// prepare() resets, and a reset leaves the firmware's start-up pluck pending — replaced with a silent one, as
	// reset() does: the string is plucked by notes, not by the plugin opening.
	if (!sameRate) {
		ks_->pluckBuilder.prepare(sampleRate); // allocates: here, never on the audio thread
		settleWeaveLaws(); // prepare resets the string, and its rest pose reads the laws (see `reset`)
		weave_.prepare(sampleRate);
		weave_.pluck(0.0f);
		for (Voice& v : voices_) {
			for (int part = 0; part < kMaxStack; ++part) {
				partOsc(v, part).setSampleRate(sampleRate);
				partOsc(v, part).setPluckBuilder(&ks_->pluckBuilder);
			}
		}
	}
	// THE LANE AND THE STRIP — one engine — AND ONE INSTANCE PER VOICE SLOT for a device with a
	// per-voice road. kVoiceSlots and not kMaxVoices: a steal-fade spare is a voice that is still
	// sounding and still owes its note a filter state of its own. A device is PREPARED at a rate; the
	// engine re-prepares what it holds in place.
	if (prepareLane) { // grow-only at the same rate: see the top of this function
		lane_.prepare(sampleRate, static_cast<size_t>(kVoiceSlots), maxFrames);
		// AND EVERY KIT PAD'S LANE that exists (drum proposal §17); one made later is prepared by ensurePadLane at this rate.
		padLaneRate_ = sampleRate;
		padLaneFrames_ = maxFrames;
		if (padBusL_.empty()) { // the buses, once: a pad's chunk is at most kRenderChunk
			padBusL_.assign(kMaxSounds * static_cast<size_t>(kRenderChunk), 0.0f);
			padBusR_.assign(kMaxSounds * static_cast<size_t>(kRenderChunk), 0.0f);
		}
		for (PadLane& p : padLanes_) {
			if (p.owned != nullptr) {
				p.owned->prepare(sampleRate, maxFrames);
				padAlign_[static_cast<size_t>(&p - padLanes_.data())].prepare(sampleRate);
				directAlign_.prepare(sampleRate);
			}
		}
	}
	if (!sameRate) {
		masterMeter_.prepare(sampleRate);
		// OE: THE SIDECHAIN'S ANALYSIS at this rate (message thread: it allocates), and a held envelope RE-ARMED from the last
		// published block when Freeze is on (the Spectral effect's Z10c: a new rate re-prepares, which would drop it silently).
		vocodeAnalyser_->prepare(sampleRate);
		std::array<float, kVocodeFreezeFloats> words {};
		VocodeFreezeBlock held;
		if (vocodeFreezeSnapshot_.read(words.data(), words.size()) != 0
		    && readVocodeFreezeBlock(words.data(), words.size(), &held) && held.holding) {
			primeVocode(held);
		}
	}
	if (!sameRate) {
		// THE PER-VOICE GAIN RINGS, for `device::perVoiceLatencyBound` at this rate — the bound the lane admits latent per-voice
		// devices up to (LaneRack::prepare), so the delay is never clamped (review, Important 4). Message thread, so the allocation
		// is allowed here and nowhere else.
		size_t cap = 1;
		while (cap < static_cast<size_t>(device::perVoiceLatencyBound(sampleRate)) + 1) {
			cap <<= 1;
		}
		gainRingCap_ = cap;
		gainRing_.assign(cap * static_cast<size_t>(kVoiceSlots), 0.0f);
		gainWrite_.fill(0);
		gainDelay_ = 0;
	}
	// AND THE DEVICES THE LANE DOES NOT HOLD YET. The lane re-prepares what it holds in place; the
	// default lane has a filter in it, so a synth that never got another main-thread visit would
	// otherwise render through an empty chain. Main thread, which is where activate is.
	provisionDevices(fxChain_, rack_);
	++rackRevision_; // the lane's devices may have been provisioned: `modulated_` is copied again at the next tick
	drainRetiredDevices();
}

void Synth::reset() {
	voiceManager_->allSoundsOff();
	outputLevelPrimed_ = false; // the Level fader's ramp starts over at its value, as the voices do from silence
	for (Voice& v : voices_) {
		// allSoundsOff terminates what the manager knows about, which leaves the steal fades it just
		// started still sounding. A reset is not a musical event, so they go too.
		v.stage = Voice::Stage::Idle;
		v.level = 0.0f;
		v.levelMod = 0.0;
		v.haveTarget = false;
		v.noteId = -1;
		for (int part = 0; part < kMaxStack; ++part) {
			partOsc(v, part).reset();
			v.parts[static_cast<size_t>(part)] = {};
		}
		v.respaceTicksLeft = 0;
		v.stacked = false;
	}
	heldNotes_.fill({}); // every note forgotten — see the declaration
	hostDown_.fill(false);
	hostIssued_.fill(kNoHostNote); // a reset is the host's own: it drops what it tracked, so nothing is ended to it
	noteEndCount_ = 0;
	// The note-on's draws start over, so a bounce after a reset deals every note the seed it dealt before.
	stackSeedState_ = 0x9E3779B9u;
	// And no stand is keyed: the next route re-keys from an empty pool.
	standReady_.fill(false);
	standKeyed_.fill(false);
	standUsed_.fill(false);
	bendWheel_.fill(0.0);
	// A reset is not a swipe across the pad: the gesture is forgotten, but where the puck is standing
	// is not, so the next tick does not read the reset itself as a lurch.
	motion_.reset();
	// THE STRING TOO, at rest in its rest pose and not plucked — a reset is not a note. Its rest pose reads the laws, so a
	// frozen string is handed the ones it is owed first (`settleWeaveLaws`).
	settleWeaveLaws();
	weave_.reset();
	weave_.pluck(0.0f);
	// The filter's STATE goes, its settings stay — same rule as the puck. A reset means the room is
	// silent, and a filter that kept its resonant ring across one would sing into the silence. EVERY
	// instance — the bus one and every voice's — because a per-voice filter that kept its ring across a
	// reset would sing into that silence sixteen times over.
	// AND THE ENVELOPES, on the filter's own rule: state goes, settings stay. An envelope left
	// half-open across a reset would go on pushing its destinations in a room where nothing is
	// playing, which is the same defect as a filter singing into the silence. One engine, so one reset.
	lane_.reset();
	envRetrigger_ = false;
	ks_->strikes.fill(0); // a reset or a load starts every key's plucks from the first (KS design §6)
	drum_->strikes.fill(0); // and every pad's hits from the first (drum proposal §7)
	envGate_ = false;
	publishEnvelopes();
}

//---------------//
// Note events   //
//---------------//

void Synth::noteOn(int16_t channel, int16_t key, int32_t noteId, double velocity) {
	// The steal handoff lives within ONE note-on: anything left from an earlier terminate that
	// created nothing is a dead phase no new note should inherit.
	haveStolenPhase_ = false;
	// A concrete channel and key are not a formality here. The voice manager stores per-key state in
	// a plain 16x128 array and indexes it with these directly, so anything outside those bounds —
	// including the -1 a host is allowed to use as a wildcard on a note-off — would be a write past
	// the end of it. A note-on has no wildcard meaning to begin with, so it is simply dropped.
	if (!isPlayableChannel(channel) || !isPlayableKey(key)) {
		return;
	}
	// A LATCH THE NEXT CONTROL TICK CONSUMES — see Synth::envRetrigger_. Set after the playability
	// checks above, so a note the synth refuses to play does not restart an envelope either.
	//
	// **IT STAYS HERE, AHEAD OF THE CAPTURE AND THE DELIVERY, AND THAT ORDERING IS PINNED.** See
	// "a note re-struck inside one control tick still retriggers the envelope" in tests/
	// test_mod_rack.cpp, written before this buffer existed precisely so that inserting it could not
	// move the latch without something going red. Nothing in the repository could see that property
	// go until that case existed.
	envRetrigger_ = true;
	hostIssued_[static_cast<size_t>(channel) * 128u + static_cast<size_t>(key)] = noteId;
	hostDown_[static_cast<size_t>(channel) * 128u + static_cast<size_t>(key)] = true;
	const device::NoteEvent event { device::NoteEvent::Kind::On, noteOffsetInTick(),       noteId, kPort,
	                                channel,                    key,     velocity };
	// THE LANE SEES IT ONLY IF THE SYNTH WOULD PLAY IT, which is the latch's own rule one line up: a
	// note refused for an unplayable channel or key restarts no envelope, and a modulator has no more
	// business hearing it than the envelope does.
	captureNote(event);
	runLaneFor(&event, 1);
}

void Synth::noteOff(int16_t channel, int16_t key, int32_t noteId, double velocity) {
	// CAPTURED AS THE HOST SENT IT, WILDCARDS AND ALL. The expansion below is how the VOICE MANAGER
	// has to be addressed, not what the host said — and registry.h is explicit that the host does not
	// interpret: every device is given every note and decides for itself what to do with it. A device
	// receiving nine concrete offs where one wildcard was sent would be reading the
	// synth's voice allocation, which is none of its business.
	const device::NoteEvent event { device::NoteEvent::Kind::Off, noteOffsetInTick(),   noteId, kPort,
	                                channel,                     key, velocity };
	captureNote(event);
	runLaneFor(&event, 1);
	for (size_t i = 0; i < hostDown_.size(); ++i) {
		const int ch = static_cast<int>(i / 128u);
		const int k = static_cast<int>(i % 128u);
		if ((channel < 0 || channel == ch) && (key < 0 || key == k)) {
			hostDown_[i] = false;
		}
	}
	// A NOTE THE LANE SWALLOWED NEVER GOT A VOICE, so no voice will end it: the host's note is over now.
	if (isPlayableChannel(channel) && isPlayableKey(key)
	    && hostIssued_[static_cast<size_t>(channel) * 128u + static_cast<size_t>(key)] != kNoHostNote
	    && !keyHasLiveVoice(channel, key, nullptr)) {
		hostNoteEnded(channel, key, hostIssued_[static_cast<size_t>(channel) * 128u + static_cast<size_t>(key)], nullptr, false);
	}
}

bool Synth::keyHasLiveVoice(int16_t channel, int16_t key, const Voice* except) const noexcept {
	for (const Voice& v : voices_) {
		if (&v != except && v.stage != Voice::Stage::Idle && v.stage != Voice::Stage::Stealing && v.channel == channel
		    && v.key == key) {
			return true;
		}
	}
	return false;
}

void Synth::hostNoteEnded(int16_t channel, int16_t key, int32_t noteId, const Voice* except, bool keyStaysLive) noexcept {
	if (!isPlayableChannel(channel) || !isPlayableKey(key)) {
		return;
	}
	int32_t& record = hostIssued_[static_cast<size_t>(channel) * 128u + static_cast<size_t>(key)];
	if (record == kNoHostNote) {
		return; // the host never issued a note on this key: an invented note's end is nobody's to hear
	}
	const bool others = keyStaysLive || keyHasLiveVoice(channel, key, except)
	                    || (pendingNoteOn_.valid && pendingNoteOn_.channel == channel && pendingNoteOn_.key == key);
	// An id-less note (a MIDI host's) is addressed by its key alone, so while another note holds the key its end
	// would end that one too: the header's own example sends none then.
	// A voice the lane played on this key carries no id (an invented note's is -1): the host's own id for the key is
	// the one it is told.
	const int32_t id = noteId >= 0 ? noteId : record;
	if (id < 0 && others) {
		return;
	}
	if (noteEndCount_ < noteEnds_.size()) {
		noteEnds_[(noteEndHead_ + noteEndCount_) % noteEnds_.size()] = NoteEnd { channel, key, id };
		++noteEndCount_;
	}
	if (!others) {
		record = kNoHostNote;
	}
}

void Synth::choke(int16_t channel, int16_t key, int32_t noteId) {
	const device::NoteEvent event { device::NoteEvent::Kind::Choke, noteOffsetInTick(),   noteId, kPort,
	                                channel,                       key, 0.0 };
	captureNote(event);
	runLaneFor(&event, 1);
}

//----------------------------------------------------------------------------------------------//
// THE NOTE LANE — its two halves, and the seam between them.                                     //
//----------------------------------------------------------------------------------------------//

void Synth::retriggerLaneNote(const Voice& voice) noexcept {
	if (!isPlayableChannel(voice.channel) || !isPlayableKey(voice.key)) {
		return;
	}
	const HeldNote& held = heldNote(voice.channel, voice.key);
	if (!held.down) {
		return;
	}
	// TO THE LANE'S DEVICES ONLY, on `chokeLaneNote`'s terms and through its scratch: what leaves the lane is
	// discarded, because the voice manager has already started the voice this note plays on. And the latch the
	// rack's one shared shape restarts by, as a host note-on sets it (`noteOn`).
	envRetrigger_ = true;
	const device::NoteEvent event { device::NoteEvent::Kind::On, noteOffsetInTick(), held.noteId, kPort,
	                                voice.channel,              voice.key,          held.velocity };
	(void)lane_.processNotes(&event, 1, chokeScratch_.data(), chokeScratch_.size());
}

int32_t Synth::heldNoteIdOr(int16_t channel, int16_t key, int32_t fallback) noexcept {
	if (!isPlayableChannel(channel) || !isPlayableKey(key)) {
		return fallback;
	}
	const HeldNote& held = heldNote(channel, key);
	return held.down ? held.noteId : fallback;
}

void Synth::liftHeldNotes(int16_t channel, int16_t key, int32_t noteId) noexcept {
	const auto lift = [noteId](HeldNote& held) {
		if (held.down && (noteId == -1 || held.noteId == -1 || held.noteId == noteId)) {
			held = {};
		}
	};
	if (isPlayableChannel(channel) && isPlayableKey(key)) {
		lift(heldNote(channel, key));
		return;
	}
	for (int16_t ch = 0; ch < 16; ++ch) {
		for (int16_t k = 0; k < 128; ++k) {
			if ((channel < 0 || channel == ch) && (key < 0 || key == k)) {
				lift(heldNote(ch, k));
			}
		}
	}
}

void Synth::chokeLaneNote(const Voice& voice) noexcept {
	// **NOT THE NOTE THAT IS TAKING THIS VOICE'S PLACE** (backlog FG). The lane's devices match a choke by id,
	// channel and key with -1 as a wildcard, and the new note's On has already reached them — so a steal in
	// Mono re-pressing the same key with keyboard ids (-1) choked the entry the new note had just re-used,
	// and the new note played silent under a routed Level. When this choke would name the incoming note, the
	// old note's entry IS the new note's, and there is nothing of the old one left to end.
	if (pendingNoteOn_.valid && voice.channel == pendingNoteOn_.channel && voice.key == pendingNoteOn_.key
	    && (voice.noteId == -1 || pendingNoteOn_.noteId == -1 || voice.noteId == pendingNoteOn_.noteId)) {
		return;
	}
	// **NOR A NOTE MONO HAS ONLY SET ASIDE**.
	// Mono steals the held note's voice for the new note and RETURNS to the held key when the new note is let go,
	// so the held note has not ended: its key is still down and its own note-off will come. Choked here, its
	// envelope was reset and marked done, and the voice that returned to it read the envelope at rest — silent,
	// under a routed Voice Level. Only Mono and Legato return; a Poly steal ends the note (and the host may never
	// lift it, which is what the choke is for).
	if (params_.voiceMode() != VoiceMode::Poly && isPlayableChannel(voice.channel) && isPlayableKey(voice.key)) {
		const HeldNote& held = heldNote(voice.channel, voice.key);
		if (held.down && (voice.noteId == -1 || held.noteId == -1 || voice.noteId == held.noteId)) {
			return;
		}
	}
	// A CHOKE FOR THE NOTE THIS VOICE PLAYS, to the lane's devices ONLY — never to the voices, which
	// is why this does not go through `runLaneFor`: that road delivers what leaves the lane to the
	// voice manager, and a choke arriving there mid-steal would be a second steal. Its own scratch,
	// because `laneNotes_` may be the buffer `deliverNotes` is walking when the steal that called
	// this happens. The lane's output is discarded — nothing plays from a note that has ended.
	device::NoteEvent event { device::NoteEvent::Kind::Choke, noteOffsetInTick(), voice.noteId, kPort,
	                          voice.channel,                 voice.key,          0.0 };
	event.fromSteal = true; // THE SYNTH'S OWN STEAL, not the host's choke: a held key stays held (`StepEmitter`)
	(void)lane_.processNotes(&event, 1, chokeScratch_.data(), chokeScratch_.size());
}

uint32_t Synth::noteOffsetInTick() const noexcept {
	// **WHERE IN THE CONTROL TICK THIS NOTE LANDED, AND IT IS NOT ZERO.** `NoteEvent::timeSamples`
	// says why the field exists at all: *"a note's PLACE in a block is part of it: an arp that
	// emitted everything at offset 0 would quantise itself to the block rate."* A first draft of this
	// capture stamped every note 0, which is exactly the quantisation that comment forbids — the lane
	// runs once per tick, so without this every note in a tick would arrive indistinguishable.
	//
	// THE CALLER HAS ALREADY ADVANCED THE MOTION TO THIS EVENT. `Synth::render` cuts the block at
	// every event and advances the puck for the samples before it, so the tick position read here is
	// the position the note actually arrived at rather than the start of the chunk containing it.
	const int into = PuckMotion::kTickSamples - motion_.samplesUntilTick();
	return into < 0 ? 0u : static_cast<uint32_t>(into);
}

void Synth::captureNote(const device::NoteEvent& event) noexcept {
	// **A DROP HERE COSTS A DEVICE'S KNOWLEDGE, NOT A STUCK NOTE**, and the distinction is worth
	// stating because `NoteOps` states the opposite rule for a device's own output: there, a dropped
	// note-OFF is a stuck note, so later notes must go rather than earlier ones. This buffer is the
	// lane's INPUT and the voice manager is fed on its own road, so an overflow means a modulator
	// missed a note, never that a key stayed down.
	//
	// FULL IS A REAL STATE AND NOT AN ERROR. One control tick is 64 samples; 256 events inside it is
	// not a musical stream, and the honest answer to more is to carry what fits.
	if (pendingNoteCount_ >= pendingNotes_.size()) {
		return;
	}
	pendingNotes_[pendingNoteCount_] = event;
	++pendingNoteCount_;
}

void Synth::deliverNotes(const device::NoteEvent* events, size_t count) {
	// **THIS IS THE SEAM, AND IT IS THE WHOLE POINT OF THE SHAPE ABOVE.**
	//
	// Today every caller is one of the three event methods, handing over the host's note the instant
	// it arrives — so the voice manager hears exactly what it has always heard, in the order it has
	// always heard it. That is reading (1) of the design: devices SEE the notes, and what PLAYS is
	// the host's own stream.
	//
	// **THAT PARAGRAPH WAS STALE AND IT MISLED A READER — 2026-09-10.** It said reading (2) was
	// "deliberately not wired, because it is the decision to make". It was made on 2026-09-02
	// (reading 2, as destination) and it IS wired: the sole caller of this function is `runLaneFor`,
	// with the events that came out of the lane. **What leaves the lane is what plays** — an arp in
	// the strip plays the synth, and a device that swallows a note silences it. `runNoteLane` says so
	// at the line that does it.
	//
	// It is corrected rather than deleted because the correction is the useful part: a comment
	// describing a fork as open, three commits after it was closed, reads exactly like current fact.
	// Somebody quoted it as one.
	//
	// WHAT MOVED WITH (2), kept because it is still the subtle part: the `envRetrigger_` latch is set
	// in `noteOn` against the HOST's note, so a note the lane INVENTS must set it too and a host note
	// the lane SWALLOWS must not.
	//
	// **EXPRESSIONS ARE NOT ON (2) YET.** They reach the lane (so devices see them) and are ALSO
	// routed straight to the manager by `Synth::noteExpression`. See the `Expression` arm below.
	for (size_t i = 0; i < count; ++i) {
		const device::NoteEvent& e = events[i];
		deliveredLog_[static_cast<size_t>(deliveredCount_ % kDeliveredLog)] = DeliveredNote { e, samplesRendered_ };
		++deliveredCount_;
		switch (e.kind) {
		case device::NoteEvent::Kind::On:
			syncVoiceManagerSettings();
			if (isPlayableChannel(e.channel) && isPlayableKey(e.key)) {
				heldNote(e.channel, e.key) = HeldNote { e.noteId, e.velocity, true };
			}
			pendingNoteOn_ = PendingNoteOn { e.noteId, e.channel, e.key, true };
			drum_->startedCount = 0;
			voiceManager_->processNoteOnEvent(static_cast<int16_t>(kPort), e.channel, e.key, e.noteId,
			                                  static_cast<float>(e.velocity), 0.0f);
			pendingNoteOn_.valid = false;
			applyDrumChokes(); // D6: once the manager has returned, never inside its transaction
			break;

		case device::NoteEvent::Kind::Off: {
			syncVoiceManagerSettings();
			// THE KEY IS UP BEFORE THE MANAGER HEARS IT, so a return it makes inside this call finds the key it
			// returns to and not this one (see `heldNotes_`). Every key the off names, wildcards included.
			liftHeldNotes(e.channel, e.key, e.noteId);
			if (isPlayableChannel(e.channel) && isPlayableKey(e.key)) {
				voiceManager_->processNoteOffEvent(static_cast<int16_t>(kPort), e.channel, e.key,
				                                   e.noteId, static_cast<float>(e.velocity));
				break;
			}
			// A wildcard note-off, which CLAP does allow: release everything that matches, addressing
			// each voice by the channel and key it is actually playing so the manager only ever sees
			// concrete ones. Dropping these instead would leave the notes stuck down.
			for (Voice& v : voices_) {
				if (!v.isSounding() || v.stage == Voice::Stage::Stealing) {
					continue;
				}
				if ((e.noteId != -1 && v.noteId != e.noteId)
				    || (e.channel >= 0 && v.channel != e.channel)
				    || (e.key >= 0 && v.key != e.key)) {
					continue;
				}
				voiceManager_->processNoteOffEvent(static_cast<int16_t>(kPort), v.channel, v.key,
				                                   v.noteId, static_cast<float>(e.velocity));
			}
			break;
		}

		case device::NoteEvent::Kind::Choke:
			// Done against our own pool rather than through the manager, which has no choke of its
			// own. A choked voice is ended the same way a stolen one is: the manager is told
			// immediately that the voice is gone, and the voice itself fades out of the way.
			liftHeldNotes(e.channel, e.key, e.noteId);
			for (Voice& v : voices_) {
				if (v.stage == Voice::Stage::Idle || v.stage == Voice::Stage::Stealing) {
					continue;
				}
				const bool matches = (e.noteId == -1 || v.noteId == e.noteId)
				                     && (e.channel == -1 || v.channel == e.channel)
				                     && (e.key == -1 || v.key == e.key);
				if (matches) {
					responder_.terminateVoice(&v);
				}
			}
			break;

		case device::NoteEvent::Kind::Expression:
			// **NOTHING, AND THE COMPILER IS WHAT MADE THIS A DECISION RATHER THAN AN OMISSION.**
			// `-Werror=switch` refused the build until this arm existed, which is the right way round:
			// a new note-event kind must not reach delivery by falling through a default.
			//
			// **(b), 2026-09-10: WHAT LEFT THE LANE IS WHAT THE VOICE FEELS.** The event routed
			// here is the one that came OUT of the devices, so a modulator that scales pressure or
			// remaps brightness changes the note — exactly as one that changes a key already does,
			// because notes have been on reading (2) since 2026-09-02.
			//
			// `Synth::noteExpression` used to call `routeNoteExpression` itself and this arm did
			// nothing. Moving the call here is the whole of (b): one road instead of two, and the
			// device sits on it rather than beside it.
			//
			// **NO `syncVoiceManagerSettings()`, unlike the On and Off arms.** Those may allocate or
			// steal a voice and must do it under current settings; an expression only addresses voices
			// that already exist. `routeNoteExpression` walks the manager's own table and matches
			// nothing when the note is gone, which is the right answer for an expression aimed at a
			// note that has ended.
			voiceManager_->routeNoteExpression(e.port, e.channel, e.key, e.noteId, e.expressionId,
			                                   e.expressionValue);
			break;
		}
	}
}

void Synth::runLaneFor(const device::NoteEvent* in, size_t count) {
	// **THE LANE RUNS WHERE THE NOTE ARRIVES, NOT AT THE NEXT TICK — and that is a correction, made
	// with a measurement in hand rather than by argument.**
	//
	// The first version of reading (2) buffered a tick's notes and delivered what left the lane at
	// the tick boundary. It quantised EVERY note to the 64-sample control grid: the pinned reference
	// from a0d96e4 read 0 where it should have read 0.133333 and 0.133333 where it should have read
	// 0.266667 — a clean one-tick delay — and fifteen note-timing cases failed with it. That is the
	// block-rate quantisation `NoteEvent::timeSamples` exists to forbid, arriving at the VOICES
	// instead of at a device.
	//
	// `Synth::render` already cuts the block at every event, so the moment a note reaches this
	// function IS its sample. Running the lane here keeps a pass-through bit-for-bit identical to
	// having no lane at all — same notes, same order, same offsets — which is the property that makes
	// the switch safe to have made.
	//
	// **A DEVICE THAT EMITS ON ITS OWN CLOCK — THE ARP — HAS ITS OWN ROAD NOW, `emitLaneNotes`** (2026-10-29). This
	// one only runs when a host note arrives; that one runs at the start of every render chunk, and the chunk is cut
	// where the device says its next event falls. This paragraph used to record the road as missing; it is kept as a
	// pointer so a reader who meets the old sentence quoted elsewhere finds where it went.
	laneNoteCount_ = lane_.processNotes(in, count, laneNotes_.data(), laneNotes_.size());
	// **AND WHAT LEFT THE LANE IS WHAT PLAYS.** A device
	// that transforms the stream transforms what the synth plays; one that swallows a note silences
	// it. The three event methods do not address the voice manager at all any more — that is what the
	// previous commit's seam was for.
	deliverNotes(laneNotes_.data(), laneNoteCount_);
}

uint32_t Synth::emitLaneNotes() {
	// **THE ROAD `runLaneFor`'S COMMENT NAMED AS MISSING**. Called at the top of every
	// render chunk, so a device's step lands on its own sample: the device says how far away its next event is and the
	// chunk stops there. Nothing in the chain emits → nothing delivered and no limit, so the chunking (and the render)
	// of every patch without an arp is the one it always was.
	uint32_t until = std::numeric_limits<uint32_t>::max();
	const size_t made = lane_.emitNotes(rackClock(), samplesRendered_, emittedNotes_.data(), emittedNotes_.size(),
	                                    &until);
	if (made == 0) {
		return until;
	}
	const uint32_t at = noteOffsetInTick();
	bool anyOn = false;
	for (size_t i = 0; i < made; ++i) {
		emittedNotes_[i].timeSamples = at;
		anyOn = anyOn || emittedNotes_[i].kind == device::NoteEvent::Kind::On;
	}
	// A NOTE THE LANE INVENTS SETS THE RETRIGGER LATCH, `deliverNotes`' own rule for reading (2): the strip's shared
	// shapes restart on an arp step as they do on a played note.
	if (anyOn) {
		envRetrigger_ = true;
	}
	deliverNotes(emittedNotes_.data(), made);
	return until;
}

size_t Synth::runNoteLane() {
	// THE BLOCK'S NOTES, OFFERED TO THE RACK IN ORDER. Run AFTER the strip's settings so a device has
	// its settings before it sees a note — `RackEngine::runNoteLane` sets fields first for the same
	// reason — and BEFORE the modulators tick, so a device that turns notes into a signal has
	// published this block's answer by the time anything reads it.
	// THE TICK'S RECORD, KEPT FOR THE SEAM RATHER THAN FOR DELIVERY — the notes themselves were
	// carried at the instant they arrived, by runLaneFor. Snapshotted into its own buffer rather than
	// read out of the live one, because the live one is cleared below: a seam that reported the
	// in-progress tick would answer empty to every case that renders a whole tick before asking,
	// which is every case.
	const size_t seen = pendingNoteCount_;
	for (size_t i = 0; i < seen; ++i) {
		laneNotes_[i] = pendingNotes_[i];
	}
	laneNoteCount_ = seen;
	// CONSUMED HERE AND NOWHERE ELSE. The buffer is per control tick, not per block: a note left in
	// it would be offered to the rack again on the next tick, which is the latch's own failure mode
	// wearing different clothes.
	pendingNoteCount_ = 0;
	// **AND WHAT LEFT THE LANE IS WHAT PLAYS.** The voice
	// manager hears the lane's output rather than the host's stream, so a device that emits notes —
	// an arp — plays the synth, and one that swallows them silences it. This is the one line the
	// previous commit was shaped around: the three event methods were not touched to make this
	// happen, because they had already stopped addressing the voice manager themselves.
	//
	// WITH NO NOTE DEVICE IN THE RACK THIS IS A NO-OP BY CONSTRUCTION, not by luck: the lane is a
	// pass-through when nothing claims the stream, so the voices receive the same notes in the same
	// order at the same offsets. The case beside this asserts that field by field rather than
	// trusting the argument.
	return seen;
}

void Synth::pitchBend(int16_t channel, double wheel) {
	if (channel < 0 || channel >= static_cast<int16_t>(bendWheel_.size())) {
		return;
	}
	bendWheel_[static_cast<size_t>(channel)] = std::clamp(wheel, -1.0, 1.0);
}

void Synth::pitchBend14(int16_t channel, int bend14bit) {
	const int offset = bend14bit - 8192;
	pitchBend(channel, offset >= 0 ? static_cast<double>(offset) / 8191.0
	                               : static_cast<double>(offset) / 8192.0);
}

void Synth::sustainPedal(int16_t channel, bool down) {
	// Same bounds argument as the note events: the manager keeps one sustain flag per channel in a
	// sixteen-entry array and indexes it without checking.
	if (!isPlayableChannel(channel)) {
		return;
	}
	voiceManager_->updateSustainPedal(static_cast<int16_t>(kPort), channel, down ? 127 : 0);
}

void Synth::paramModForNote(int16_t channel, int16_t key, int32_t noteId, clap_id paramId,
                            double amount) {
	// The bounds rule the note doors keep, kept here too. Neither of the manager's parameter routers
	// indexes anything by channel or key today — one matches on voice id, the other applies to every
	// active voice — so nothing here is out of bounds as things stand. That is exactly why the guard
	// belongs to us: without it, our own rule that the manager never receives a channel or key it
	// could not index with would be upheld by somebody else's implementation detail, and a version
	// bump could retract it silently.
	//
	// -1 stays legal and must: CLAP uses it for "this field does not address anything", and a
	// modulation aimed at a note id carries it in both fields routinely.
	const bool addressable =
	    (channel == -1 || isPlayableChannel(channel)) && (key == -1 || isPlayableKey(key));
	if (!addressable) {
		return;
	}

	// Routed through the manager rather than matched here, because matching a note id to a voice is
	// exactly what it keeps its note-id stack for: in legato and piano modes one voice answers to
	// several ids at once, and a match written here would get that wrong.
	if (noteId != -1) {
		voiceManager_->routePolyphonicParameterModulation(static_cast<int16_t>(kPort), channel, key,
		                                                  noteId, paramId, amount);
		return;
	}
	voiceManager_->routeMonophonicParameterModulation(static_cast<int16_t>(kPort), channel, key,
	                                                  paramId, amount);
}

void Synth::noteExpression(int16_t channel, int16_t key, int32_t noteId, int32_t expression,
                           double value) {
	// **THE SAME BOUNDS RULE THE NOTE DOORS KEEP, and for `paramModForNote`'s stated reason**: the
	// manager indexes nothing by channel or key today, so a bad one is harmless BY SOMEBODY ELSE'S
	// implementation detail, and a version bump could retract that silently. -1 stays legal and must —
	// CLAP uses it for "this field does not address anything", and an expression aimed at a note id
	// carries -1 in the others routinely.
	const bool addressable =
	    (channel == -1 || isPlayableChannel(channel)) && (key == -1 || isPlayableKey(key));
	if (!addressable) {
		return;
	}
	// **ONTO THE LANE, AND THE LANE IS THE ONLY ROAD — (b),.**
	//
	// This function used to call `routeNoteExpression` right here, and the lane got a copy. That was
	// (a): a device could SEE an expression and not change it. The call now lives in `deliverNotes`'s
	// `Expression` arm, which is fed what came OUT of the devices — so a modulator that scales
	// pressure changes what the voice feels.
	//
	// **THIS IS WHAT NOTES ALREADY DO.** Reading (2), the lane as destination, has been live since
	// 2026-09-02: an arp in the strip plays the synth, and a device that swallows a note silences
	// it. Expressions were the one part of the stream that bypassed all of it — they went from the
	// host straight to the voice manager and no device was ever offered one.
	//
	// **AND THE ADDRESS CHECK ABOVE IS WHAT GATES LANE ENTRY**, which is `noteOn`'s rule in as many
	// words: *"THE LANE SEES IT ONLY IF THE SYNTH WOULD PLAY IT."* An expression aimed at an
	// unplayable channel or key reaches no device, because it would have reached no voice.
	device::NoteEvent event {};
	event.kind = device::NoteEvent::Kind::Expression;
	event.timeSamples = noteOffsetInTick();
	event.noteId = noteId;
	event.port = static_cast<int16_t>(kPort);
	event.channel = channel;
	event.key = key;
	event.expressionId = expression;
	event.expressionValue = value;
	// **`velocity` IS LEFT AT ITS DEFAULT AND THAT IS DELIBERATE.** An expression has no velocity;
	// stamping one would invent a number a device might read. The kind is what says which fields mean
	// anything, and `NoteEvent` documents that.
	captureNote(event);
	// **AND OFFERED TO THE DEVICES AT THE INSTANT IT ARRIVES, which is the half that actually reaches
	// them.** `captureNote` fills the seam's snapshot; `runLaneFor` is what puts the event in front of
	// a device. Doing only the first was a first draft of this commit, and it produced a lane a TEST
	// could see an expression on and a DEVICE could not — the exact shape of a road that looks wired
	// and is not. Both, in the order `noteOn` does them.
	runLaneFor(&event, 1);
}

OscMode Synth::oscMode() const {
	// The shared derivation, as everywhere: a homespun threshold here could disagree with the
	// Engine rail row about which mode the patch is in.
	return oscModeFromValue(params_.resolved(ParamId::OscMode));
}

//------------//
// Rendering  //
//------------//

void Synth::updateCharacter() {
	const rack::StageScope stage(rack::Stage::Rebake); // the spike recorder's (backlog SR): inert unless one is bound
	// The mapping itself lives in character_params.h, shared with the editor's wave display so that
	// what is drawn and what is played cannot drift apart. This side reads ParamSet::resolved, which
	// is base plus modulation, on the audio thread.
	//
	// THE VELOCITY IS A TICK BEHIND, and deliberately: the character is built from the gesture as the
	// last tick measured it, and then the puck's new position is handed to the smoother for the next
	// one. Building it the other way round would mean computing the puck twice, once to measure it and
	// once to use it.
	const auto read = [this](ParamId id) { return params_.resolved(id); };
	const OscType type = params_.oscType();

	// **WHERE EACH SOUND STANDS, WITH MODULATION IN IT — and this is the ONE reader** (2026-09-21).
	//
	// `drift_.offset` is where each sound was PUT: patch state the Explore chart scrubs, handed over
	// once a block, costing no parameter. `drift_.orbit` is the sound's own polar offset FROM there —
	// the Radius and Theta corners, independent of the place by design (sound_travel.h). Radius and
	// Theta are also the core's INTERNAL destinations (params.h): what reaches them is added to that
	// polar offset, so a ramp on Theta carries the sound round its own place at its own radius.
	//
	// ONE READER ON PURPOSE. A later workstream re-connects "a small amount" of these to
	// host-facing CLAP parameters; with one reader that is a term ADDED to `radiusMod`/`thetaMod`
	// below, in the same units, and nothing downstream knows the difference.
	//
	// **THE ANGLE IS CONSUMED ONLY BY cos/sin, AND THAT IS THE WHOLE OF THE WRAP HANDLING.** They are
	// periodic, so a ramp that RESETS is positionally continuous — `cos(2pi) == cos(0)` — and there is
	// nothing to wrap because nothing anybody observes is discontinuous. Clamping the angle, or
	// normalising it and then smoothing it, would each put the discontinuity back.
	//
	// THE RADIUS HAS A FLOOR: a modulator pulling below zero parks the sound on its own place rather
	// than reflecting it through, since a negative radius is the same point half a turn round.
	TravelPulls pulls;
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		pulls.radius[slot] = params_.internalOffset(travelInternalIndex(slot, TravelAxis::Radius));
		pulls.theta[slot] = params_.internalOffset(travelInternalIndex(slot, TravelAxis::Theta));
	}
	// `playedTravel` keeps a sound with no radius on its authored pair BIT FOR BIT, and is the same
	// function the chart and the scope place the sound with — sound_travel.h.
	const SoundTravel moved = playedTravel(drift_, pulls);
	// STAMPED WHERE THEY ARE PRODUCED — see `playedTravelForTest`. The pulls go to the editor once a
	// block (`travelPulls`), so the chart moves the star by the engine's own numbers.
	playedTravel_ = moved;
	travelPulls_ = pulls;

	// **THE STANDS (voice stacking step 4)**: each keyed slot's bank is handed the character at the puck displaced by
	// its offset, through the same mapping and the same `read` with Pos X and Pos Y moved — so a stand is exactly what
	// the puck would hear standing there, held to the pad as the puck is. Mono: a part is one stand (§6 A).
	const auto requestStands = [&](bool earsApart) {
		// The Pos amounts read HERE too, not only with the pitches (which run after this): a route made between two
		// renders seats its parts on this pass rather than a tick later.
		stackPosXAmount_ = params_.stackIndexAmount(ParamId::PosX);
		stackPosYAmount_ = params_.stackIndexAmount(ParamId::PosY);
		widthStands_ = earsApart;
		const bool anyOff = assignStands(earsApart);
		for (int s = 0; s < kStandPool; ++s) {
			if (standFresh_[static_cast<size_t>(s)]) {
				standReady_[static_cast<size_t>(s)] = false; // re-keyed: its table is the old key's until built
			}
		}
		if (anyOff) {
			// THE BUILDS (§3.11). A slot not yet built for its key — keyed on this pass, or held by the cap on an earlier
			// one — builds now, up to kStandNewBuildsPerPass (the note-on cap). Of the built ones, kStandBuildsPerTick are
			// asked for their character again, round-robin from the cursor, and the rest keep their table until their
			// turn. A slot asked is ready once its bank holds exactly what was asked (a build behind the guard is not).
			int asked = 0;
			int fresh = 0;
			// THE CURSOR IS READ ONCE: moving it inside the walk would re-map every later index and skip slots —
			// which it did, and a stand could stay unbuilt for good (found by the Width case, step 4c).
			const int start = standBuildCursor_;
			for (int i = 0; i < kStandPool; ++i) {
				const int s = (start + i) % kStandPool;
				const auto ss = static_cast<size_t>(s);
				if (!standUsed_[ss]) {
					continue;
				}
				if (!standReady_[ss]) {
					if (fresh >= kStandNewBuildsPerPass) {
						continue;
					}
					++fresh;
				}
				else {
					if (asked >= kStandBuildsPerTick) {
						continue;
					}
					++asked;
					standBuildCursor_ = (s + 1) % kStandPool;
				}
				const StandKey at = standOffset_[ss];
				const auto shifted = [&](ParamId id) {
					if (id == ParamId::PosX) {
						return std::clamp(read(id) + at[0], -1.0, 1.0);
					}
					if (id == ParamId::PosY) {
						return std::clamp(read(id) + at[1], -1.0, 1.0);
					}
					return read(id);
				};
				Character c = characterFromParamsAtSide(shifted, type, tuning_, motion_.velocity(), moved, planes_,
				                                              residue_, gamma_, preview_, at[2]);
				attachLikeWarps(c, type);
				StandBank& bank = (*standBanks_)[ss];
				bank.setCharacter(c);
				standWeights_[ss] = soundWeightsOf(c); // the blend this stand's parts hear, for their drawers
				standReady_[ss] = standReady_[ss] || bank.holds(c);
			}
		}
		// EVERY SOUNDING VOICE'S PARTS ONTO THEIR STANDS — back onto the puck too, when the route has gone — but only onto
		// a slot built for its key: a part whose slot is still waiting stays where it is.
		for (Voice& v : voices_) {
			if (v.isSounding()) {
				seatPartsOnStands(v);
			}
		}
		// THE OWN-STAND BAKE BUDGET (§3.11): of the parts that bake a vowel for a stand of their own, kOwnBakesPerTick
		// may rebake on this pass, served round-robin over (voice, part) from the cursor; the rest keep their last real
		// bake until their turn. A part on the puck is never held (it is today's schedule), nor is a first bake.
		{
			constexpr int total = kVoiceSlots * kMaxStack;
			int granted = 0;
			int lastGranted = -1;
			for (int i = 0; i < total; ++i) {
				const int at = (ownBakeCursor_ + i) % total;
				Voice& v = voices_[static_cast<size_t>(at / kMaxStack)];
				const int part = at % kMaxStack;
				const Voice::StackPart& p = v.parts[static_cast<size_t>(part)];
				OscVoice& osc = partOsc(v, part);
				const bool ownStand = v.isSounding() && p.live && p.stand >= 0 && osc.voxLender() == nullptr;
				const bool permit = !ownStand || granted < kOwnBakesPerTick;
				osc.setVoxDeferred(!permit);
				osc.setVoxBudgeted(ownStand);
				if (ownStand && permit) {
					++granted;
					lastGranted = at;
				}
			}
			if (lastGranted >= 0) {
				ownBakeCursor_ = (lastGranted + 1) % total;
			}
		}
	};

	// TERRAIN STEREO (§5.5, decisions V11/V13): a V5 patch with a wide sound is two listeners standing
	// apart on the disk's X — each ear a FULL character evaluation with every sound's anchor at its
	// own per-ear displacement, so the weights, the weather travel, the operator fields and the blue
	// shift all differ per sound by how wide THAT sound is. V5 ONLY, by design: the frozen species stay
	// strictly mono at every width, which keeps their render path structurally today's. And not while
	// previewing (V12a) — the displacement would be suspended inside the mapping anyway, but building
	// an equal pair would still open the stereo window for nothing.
	//
	// THE PREDICATE IS THE ALGEBRA (V13b): effective separation for sound k is global × w_k, so
	// stereo is live iff the master scale is up AND some ENABLED sound is wide. A parked sound's
	// width must not open the window — a disabled sound is not in the character at all. Read off the
	// same resolved() lambda the mapping runs on, so the two cannot disagree about what is wide.
	const bool anyEnabledSoundWide = [&] {
		if (read(ParamId::Width) <= 0.0) {
			return false;
		}
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			if (steppedIndex(read(soundParam(slot, SoundField::Enable)), kEnableNames.size()) != 0
			    && read(soundParam(slot, SoundField::Width)) > 0.0) {
				return true;
			}
		}
		return false;
	}();
	// AND ADDITIVE (2026-10-10): its bank builds each ear's SPECTRUM from that ear's character, and the
	// voice renders the two ears as amplitudes over shared phases, so a mono sum cannot cancel. The frozen species stay
	// mono by design.
	// AND BASIC SHAPES (backlog KV): each ear is its own Shapes build from that ear's character, so the ears
	// differ in what the displaced weights blend — FM ratio and stereo width, warp depth, grit and jitter. The shape mix
	// is engine-wide and the same in both. The voice's two-ear road already reads Shapes sides (OscVoice::earSample).
	if ((type == OscType::V5 || type == OscType::Additive || type == OscType::BasicShapes) && anyEnabledSoundWide
	    && !preview_.active()) {
		Character l = characterFromParams(read, type, tuning_, motion_.velocity(), moved,
		                                        planes_, residue_, gamma_, preview_, EarSide::Left);
		Character r = characterFromParams(read, type, tuning_, motion_.velocity(), moved,
		                                        planes_, residue_, gamma_, preview_, EarSide::Right);
		attachLikeWarps(l, type);
		attachLikeWarps(r, type);
		// THE HEAD feeds the motion smoother, and since V13 either ear carries it exactly: the
		// displacement lands on the anchors, so both ears' pucks ARE the head rather than two stands
		// a midpoint has to recover.
		motion_.setPosition(l.puck);
		// AND THE WEIGHTS ARE THE TWO EARS' MEAN, which is the honest centre of a blend that is
		// genuinely two blends. There is no head character to read here — a wide patch is evaluated per
		// ear and never at the middle — and picking one ear would show the player a mix that only half
		// of them is hearing. At width 0 the ears are identical, so the mean is exactly the mono
		// reading; a wide patch's meter sits between what each ear has, which is what a single number
		// about a stereo image can honestly be.
		weights_ = meanSoundWeights(soundWeightsOf(l), soundWeightsOf(r));
		// AND EACH EAR'S OWN, for each ear's drawer (Synth::drawerFor): the blend each ear's spectrum is.
		earWeights_ = { soundWeightsOf(l), soundWeightsOf(r) };
		bank_.setCharacterStereo(l, r);
		requestStands(true);
		postPhaseRequest();
		pushWeaveLaws(l); // the left ear: a V5 or Additive patch, so this only keeps the frozen string current
		return;
	}

	Character c = characterFromParams(read, type, tuning_, motion_.velocity(), moved, planes_,
	                                        residue_, gamma_, preview_, EarSide::Head);
	attachLikeWarps(c, type); // the likes' phase warps (dsp/phase_warp.h)
	motion_.setPosition(c.puck);
	// READ OFF THE CHARACTER THAT IS ABOUT TO BE PLAYED, not recomputed beside it. The blend is the
	// one quantity here that a second reading could get wrong without sounding wrong.
	weights_ = soundWeightsOf(c);
	earWeights_ = { weights_, weights_ };
	bank_.setCharacter(c);
	requestStands(false);
	postPhaseRequest();
	pushWeaveLaws(c);
}

fx::spectral::SidechainEnvelope::Settings Synth::vocodeSettings() const noexcept {
	fx::spectral::SidechainEnvelope::Settings s;
	static_assert(kAdditiveVocodeResolutionNames.size() == static_cast<size_t>(fx::spectral::kResolutionCount));
	s.resolution = static_cast<fx::spectral::Resolution>(
	    steppedIndex(params_.resolved(ParamId::AdditiveVocodeResolution), kAdditiveVocodeResolutionNames.size()));
	s.bands = params_.resolved(ParamId::AdditiveVocodeBands);
	s.attackMs = params_.resolved(ParamId::AdditiveVocodeAttack);
	s.releaseMs = params_.resolved(ParamId::AdditiveVocodeRelease);
	s.freeze = steppedIndex(params_.resolved(ParamId::AdditiveVocodeFreeze), kEnableNames.size()) != 0;
	s.stereo = steppedIndex(params_.resolved(ParamId::AdditiveVocodeStereo), kAdditiveVocodeStereoNames.size()) == 1;
	s.sibilance = params_.resolved(ParamId::AdditiveVocodeSibilance) > 0.0;
	return s;
}

bool Synth::vocodeLive() const noexcept {
	if (params_.oscType() != OscType::Additive || !spectralState_.on) {
		return false;
	}
	for (size_t s = 0; s < kMaxSounds; ++s) {
		if (steppedIndex(params_.resolved(spectralParam(s, SpectralControl::VocodeOn)), kEnableNames.size()) != 0) {
			return true;
		}
	}
	return false;
}

void Synth::primeVocode(const VocodeFreezeBlock& held) noexcept {
	// THE LOAD RULES (the Spectral effect's Z10b): a held envelope is primed if Freeze is on and dropped if it is off.
	const fx::spectral::SidechainEnvelope::Settings s = vocodeSettings();
	vocodeAnalyser_->setSettings(s);
	if (held.holding && s.freeze) {
		vocodeAnalyser_->prime(held.grid.data(), held.stereo ? held.gridRight.data() : nullptr, held.binHz, held.flatness,
		                       held.flatnessRight);
		vocodeFrozenHz_ = held.frozenHz;
		vocodeWasHolding_ = true;
	}
	else {
		vocodeAnalyser_->reset();
		vocodeWasHolding_ = false;
	}
	vocodePublished_ = false; // what is held now is published at the next update, whatever was published before
}

void Synth::publishVocodeFreeze() noexcept {
	// ONLY WHEN IT CHANGED: holding or not, and while holding, a new envelope (a prime, a stop's re-expression). A fresh instance that
	// never froze publishes nothing, so its save writes no key.
	const bool holding = vocodeAnalyser_->holding();
	if (!holding && !vocodeEverHeld_) {
		return;
	}
	if (vocodePublished_ && holding == vocodeWasPublishedHolding_ && (!holding || vocodeAnalyser_->serial() == vocodePublishedSerial_)) {
		return;
	}
	VocodeFreezeBlock b;
	b.holding = holding;
	b.binHz = vocodeAnalyser_->binHz();
	b.frozenHz = vocodeFrozenHz_;
	b.stereo = vocodeAnalyser_->settings().stereo;
	b.grid = vocodeAnalyser_->gridDbfs(0);
	b.gridRight = vocodeAnalyser_->gridDbfs(b.stereo ? 1 : 0);
	b.flatness = vocodeAnalyser_->flatness(0);
	b.flatnessRight = vocodeAnalyser_->flatness(b.stereo ? 1 : 0);
	std::array<float, kVocodeFreezeFloats> words {};
	writeVocodeFreeze(b, words.data());
	vocodeFreezeSnapshot_.publish(words.data(), words.size());
	vocodePublished_ = true;
	vocodeWasPublishedHolding_ = holding;
	vocodePublishedSerial_ = vocodeAnalyser_->serial();
	vocodeEverHeld_ = vocodeEverHeld_ || holding;
}

void Synth::addVocodeNoise(float* left, float* right, int n) noexcept {
	// OE2 SIBILANCE (additive design §9.10; approved by the lead, 2026-10-27): ONE NOISE LAYER FOR THE SYNTH, scaled by Sibilance and
	// GATED BY THE LOUDEST VOCODING VOICE — its edge times the Voice Level the render just applied (the amp envelope's road). No voice
	// sounding, no gate: no note means no sibilance, the output exactly silent, and SLEEP decided as it always was. Ramped across the
	// chunk from where the last chunk left it, so a note's attack and release reach the noise without a step.
	float gate = 0.0f;
	if (vocodeNoiseLive_) {
		for (const Voice& v : voices_) {
			if (v.isSounding()) {
				gate = std::max(gate, std::clamp(v.level * v.gain, 0.0f, 1.0f));
			}
		}
	}
	const float was = vocodeGateWas_;
	vocodeGateWas_ = gate;
	if (!vocodeNoiseLive_ || (gate == 0.0f && was == 0.0f) || n <= 0) {
		return;
	}
	const auto amount = static_cast<float>(params_.resolved(ParamId::AdditiveVocodeSibilance));
	const float step = (gate - was) / static_cast<float>(n);
	for (int i = 0; i < n; ++i) {
		const float g = amount * (was + step * static_cast<float>(i + 1));
		left[i] += g * vocodeNoiseL_[static_cast<size_t>(i)];
		right[i] += g * vocodeNoiseR_[static_cast<size_t>(i)];
	}
}

void Synth::updateVocode(int offset, int frames) noexcept {
	// A LOAD THAT CARRIED A HELD ENVELOPE, ONCE (SpectralState::vocodeFreezeLoads counts them).
	const uint32_t loads = spectralState_.vocodeFreezeLoads;
	if (loads != vocodeLoadsTaken_.load(std::memory_order_relaxed)) {
		VocodeFreezeBlock held;
		if (!readVocodeFreezeBlock(spectralState_.vocodeFreeze.data(), spectralState_.vocodeFreeze.size(), &held)) {
			held = {};
		}
		primeVocode(held);
		vocodeLoadsTaken_.store(loads, std::memory_order_release);
	}
	vocodeNoiseLive_ = false; // the push below says whether this chunk has noise
	// OFF, NOTHING: no sound vocodes, nothing is held and nothing analysed is left over — the engine as it was without OE.
	const bool live = vocodeLive();
	// OE2: THE STEREO ROAD while some sound vocodes in Stereo or Spread (the voices' ears read apart).
	vocodeStereoLive_ = live && steppedIndex(params_.resolved(ParamId::AdditiveVocodeStereo), kAdditiveVocodeStereoNames.size()) != 0;
	if (!live && !vocodeAnalyser_->primed()) {
		return;
	}
	// THE NOTE FREEZE'S KEY TRACK MEASURES FROM: the last held note's pitch as Freeze takes hold (C3 until one).
	const fx::spectral::SidechainEnvelope::Settings s = vocodeSettings();
	if (s.freeze && !vocodeWasHolding_) {
		const device::NoteContext note = busNoteContext();
		if (note.gate) {
			vocodeFrozenHz_ = 440.0 * std::exp2((static_cast<double>(note.pitchSemitones) - 69.0) / 12.0);
		}
	}
	vocodeAnalyser_->setSettings(s);
	if (live) {
		vocodeAnalyser_->push(sideL_ != nullptr ? sideL_ + offset : nullptr, sideR_ != nullptr ? sideR_ + offset : nullptr, frames,
		                      vocodeNoiseL_.data(), vocodeNoiseR_.data());
		vocodeNoiseLive_ = s.sibilance;
	}
	else if (!vocodeAnalyser_->holding() && vocodeAnalyser_->primed()) {
		// NOTHING READS IT: zero cost, and the next Vocode on starts from silence (its attack), never from a stale envelope.
		vocodeAnalyser_->reset();
	}
	vocodeWasHolding_ = vocodeAnalyser_->holding();
	// THE ENVELOPE THE VOICES READ, copied only when it moved.
	const bool keyTrack = vocodeWasHolding_ && steppedIndex(params_.resolved(ParamId::AdditiveVocodeKeyTrack), kEnableNames.size()) != 0;
	if (vocodeAnalyser_->serial() != vocodeEnvSerial_ || keyTrack != vocodeEnv_.keyTrack || vocodeEnv_.frozenHz != vocodeFrozenHz_
	    || vocodeEnv_.bands != s.bands || vocodeEnv_.stereo != s.stereo) {
		vocodeEnv_.db = vocodeAnalyser_->gridDbfs(0);
		vocodeEnv_.stereo = vocodeAnalyser_->settings().stereo;
		vocodeEnv_.dbRight = vocodeAnalyser_->gridDbfs(vocodeEnv_.stereo ? 1 : 0);
		vocodeEnv_.bands = vocodeAnalyser_->settings().bands;
		vocodeEnv_.binHz = vocodeAnalyser_->binHz();
		vocodeEnv_.keyTrack = keyTrack;
		vocodeEnv_.frozenHz = vocodeFrozenHz_;
		++vocodeEnv_.serial;
		vocodeEnvSerial_ = vocodeAnalyser_->serial();
	}
	publishVocodeFreeze();
}

void Synth::postPhaseRequest() noexcept {
	// A-PH3: THE SPECTRUM A NOTE WOULD START WITH, AFTER THE DRAWER — at the last note's pitch (keytracked operators make it per
	// note; the closeness rule sends a far note to the closed form), re-posted when the table, the drawer or that note moves.
	// OscVoice::sketchTargets is the voice's own road to its first block, so the profile is optimised for what a note at that
	// pitch weighs at its note-on. A fixed copy into the slot, no allocation; the latest wins.
	// THE KIND OF BUILD IS ASKED OF ITS CHARACTER, which makes no table (a bank makes a build when first read, backlog PB):
	// a build's engine and phasing are its character's. Only a phased Partials build reads the table itself.
	const Character& built = bank_.currentCharacter(0);
	if (built.engine != OscEngine::Partials || built.phasing.mode == partials::PhaseMode::Natural) {
		return;
	}
	const PreparedCharacter& table = bank_.current(0);
	if (table.engine != OscEngine::Partials || table.phasing.mode == partials::PhaseMode::Natural) {
		return;
	}
	const device::NoteContext note = busNoteContext();
	if (note.gate) {
		phaseSketchSemis_ = static_cast<double>(note.pitchSemitones);
	}
	const spectral::ShaperParams drawer = drawerFor(-1, -1, false);
	// OE (additive design §9.5): WITH VOCODE ON, THE SKETCH IS VOCODED AT THE ENVELOPE OF THIS MOMENT, and a moving envelope alone
	// re-posts only once the sketch has moved past HALF the closeness bound from the last one posted — so a sounding sidechain asks
	// the optimiser for what a note-on would now weigh without keeping it busy every hop.
	const bool vocoding = drawer.enabled && drawer.vocode.on;
	const bool same = table.buildSerial == lastPhaseSerial_ && drawer == lastPhaseDrawer_ && phaseSketchSemis_ == lastPhaseSemis_;
	if (same && (!vocoding || vocodeEnv_.serial == vocodeSketchSerial_)) {
		return;
	}
	vocodeSketchSerial_ = vocodeEnv_.serial;
	std::array<float, partials::kMaxPartials> amp {};
	const double f0 = 440.0 * std::exp2((phaseSketchSemis_ - 69.0) / 12.0);
	const int count = OscVoice::sketchTargets(table, drawer, f0, sampleRate_, *phaseSketchCache_, amp.data(),
	                                          vocoding ? &vocodeEnv_ : nullptr);
	if (same) {
		// THE UNIT-RSS L2 DISTANCE profileDistance judges a note-on by, between this sketch and the last posted.
		double a2 = 0.0;
		double b2 = 0.0;
		const int n = std::max(count, vocodeSketchCount_);
		for (int i = 0; i < n; ++i) {
			const double a = i < count ? static_cast<double>(amp[static_cast<size_t>(i)]) : 0.0;
			const double b = i < vocodeSketchCount_ ? static_cast<double>(vocodeSketch_[static_cast<size_t>(i)]) : 0.0;
			a2 += a * a;
			b2 += b * b;
		}
		double d2 = 0.0;
		const double ra = a2 > 0.0 ? 1.0 / std::sqrt(a2) : 0.0;
		const double rb = b2 > 0.0 ? 1.0 / std::sqrt(b2) : 0.0;
		for (int i = 0; i < n; ++i) {
			const double a = i < count ? static_cast<double>(amp[static_cast<size_t>(i)]) * ra : 0.0;
			const double b = i < vocodeSketchCount_ ? static_cast<double>(vocodeSketch_[static_cast<size_t>(i)]) * rb : 0.0;
			d2 += (a - b) * (a - b);
		}
		if (std::sqrt(d2) <= 0.5 * static_cast<double>(partials::kProfileTolerance)) {
			return;
		}
	}
	lastPhaseSerial_ = table.buildSerial;
	lastPhaseDrawer_ = drawer;
	lastPhaseSemis_ = phaseSketchSemis_;
	vocodeSketch_ = amp;
	vocodeSketchCount_ = count;
	++vocodeSketchPosts_;
	phases_->post(table.buildSerial, amp.data(), count);
}

ResonateShaping Synth::resonateShaping() const {
	// The nine engine-wide knobs, resolved (base plus modulation) as the engine reads every param. The log
	// ones are stored as log2 of their multiplier so the knob's middle is x1; Decay is "how many times
	// LONGER", so turning it up lengthens the ring.
	const auto read = [this](ParamId id) { return params_.resolved(id); };
	ResonateShaping s;
	s.width = static_cast<float>(read(ParamId::ResonateWidth));
	s.bow = static_cast<float>(read(ParamId::ResonateBow));
	s.bowMode = static_cast<int>(steppedIndex(read(ParamId::ResonateBowMode), kResonateBowModeNames.size()));
	s.shimmer = static_cast<float>(std::exp2(read(ParamId::ResonateShimmer)));
	s.stiffness = static_cast<float>(std::exp2(read(ParamId::ResonateStiffness)));
	s.coupling = static_cast<float>(std::exp2(read(ParamId::ResonateCoupling)));
	s.decay = static_cast<float>(std::exp2(read(ParamId::ResonateDecay)));
	s.pluck = static_cast<float>(read(ParamId::ResonatePluck));
	s.travel = static_cast<float>(read(ParamId::ResonateTravel));
	return s;
}

void Synth::pushWeaveLaws(const Character& c) {
	// THE STRING'S LAWS, from the same character the bank was just handed — each sound read through its own
	// plane, as Stair's staircases are (buildWeaveFor). ON EVERY ENGINE, deliberately (quality review A3):
	// off Resonate the string is frozen, but its laws and crossfade keep following the puck, so switching
	// back resumes at the character's present laws. Frozen, the crossfade is RE-SEATED rather than set, so a
	// Path or puck move made while nothing was vibrating does not jolt the string with the whole of that
	// motion as morph-bow at its first live tick. The cost is a laws build per character update on a
	// non-Resonate patch; step() does not run while frozen. AND ONLY FOR A SOUND WHOSE OWN INPUTS MOVED (backlog LZ):
	// `weaveMemo_` hands the rest their laws back, the same bits — a puck moves the weights, never a sound's laws.
	// AND RESONATE'S OWN SHAPING (2026-09-26) applied to both sides, where the laws are built: the string
	// integrates the shaped laws from its next tick. No smoothing is added here and none is needed: a law
	// moves the FORCES, which the string integrates, and the scan reads it through the tick fades
	// (smoothstep, C1), so a knob step reaches the audio as motion, never as a step. Width is the one
	// shaper that moves the READ rather than the string, and it is slewed (the render loop).
	//
	// **FROZEN, THE LAWS ARE OWED RATHER THAN BUILT** (the idle-overhead pass, 2026-11-05). A frozen string reads its laws
	// only at a reset (its rest pose), and a sound on another engine whose travel a modulator moves missed the memo on
	// every tick: MEASURED about half the Rebake stage of an Additive patch at rest. So off Resonate the character
	// and shaping are kept (`weaveLawsOwed_`) and `settleWeaveLaws` builds them before a reset, and the crossfade the
	// string is re-seated to comes from the weights alone (`weaveCrossfadeFor`, the blend's own arithmetic). Every
	// setter is last-wins, so the string holds the same laws, crossfade and seat at every moment it reads them.
	const ResonateShaping shaping = resonateShaping();
	const bool live = c.engine == OscEngine::Weave;
	WeaveCharacter laws {};
	if (live) {
		laws = buildWeaveFor(c, &weaveMemo_);
		shapeWeaveLaws(shaping, laws.a);
		shapeWeaveLaws(shaping, laws.b);
		weave_.setLaws(laws.a, laws.b);
		weaveLawsOwed_ = false;
	}
	else {
		weaveOwedCharacter_ = c;
		weaveOwedShaping_ = shaping;
		weaveLawsOwed_ = true;
		laws.crossfade = weaveCrossfadeFor(c);
	}
	weave_.setExcitation({ shaping.bow });
	// AND HOW FAST IT TICKS (Resonate Speed, log2 x): not a law and not in ResonateShaping, so a picture keyed on
	// the shaping is not rebuilt by it — the rest pose a picture draws does not depend on the rate. Taken at the
	// string's next tick boundary, which sits on its own fixed clock, so this lands at the same sample whatever
	// the host's blocks.
	weave_.setSpeed(std::exp2(params_.resolved(ParamId::ResonateSpeed)));
	resonateWidthTarget_ = shaping.width;
	if (c.engine == OscEngine::Weave) {
		weave_.setCrossfade(laws.crossfade);
	}
	else {
		weave_.reseatCrossfade(laws.crossfade);
	}
	// WHAT VIBRATES — taken at the string's next tick, and heard only on Resonate.
	weave_.setModel(static_cast<WeaveModel>(
	    steppedIndex(params_.resolved(ParamId::ResonateModel), kResonateModelNames.size())));
}

void Synth::settleWeaveLaws() {
	if (!weaveLawsOwed_) {
		return;
	}
	// The laws the last frozen `pushWeaveLaws` would have handed over, from what it kept — through the memo, which hands
	// back the same bits it would have built.
	WeaveCharacter laws = buildWeaveFor(weaveOwedCharacter_, &weaveMemo_);
	shapeWeaveLaws(weaveOwedShaping_, laws.a);
	shapeWeaveLaws(weaveOwedShaping_, laws.b);
	weave_.setLaws(laws.a, laws.b);
	weaveLawsOwed_ = false;
}

//-------------------//
// The modulator rack //
//-------------------//

// THE ENVELOPE'S TICK AND THE PUCK'S ARE THE SAME TICK, and this is where that is checked rather
// than described. Synth::render caps every chunk at motion_.samplesUntilTick() so a moving puck
// sounds identical at every buffer size; an envelope on a different grid would either need a second
// cap in that loop or would land its changes wherever the host's buffer happened to end, which is
// the exact bug the moving-puck invariance test was written to find.
static_assert(mod::dahdsr::kBlockSamples == PuckMotion::kTickSamples,
              "the envelope's block and the puck's control tick have to be the same number of "
              "samples, or the render loop is capped on one grid and stepping on another");
static_assert(device::kTickWindow == PuckMotion::kTickSamples && device::kTickWindow == mod::kBlockSamples,
              "a tick window is one control tick: device/tick_window.h, the puck and the modulators must agree");

void Synth::setModRack(const ModRack& rack) {
	rack_ = rack;
	++rackRevision_;
	chainsFromPublish_ = false; // handed in as a value: no publish to name
	++structureRevision_;
}

void Synth::setFxChain(const FxChain& chain) {
	fxChain_ = chain;
	chainsFromPublish_ = false;
	++structureRevision_;
}
void Synth::compileRoutes() {
	// **THE PLAN, REBUILT WHEN THE ROUTES OR THE STRUCTURE MOVED (kill-lanes §2.4b).** Two counters,
	// compared: the publishes `ParamSet::adoptRoutes` has taken, and this synth's structure revision.
	// Either changes which node an input belongs to or which cell it points at; neither changes often.
	const uint32_t publishes = static_cast<uint32_t>(params_.routePublishes());
	if (publishes == routesSeen_ && structureRevision_ == routesStructure_) {
		return;
	}
	routesSeen_ = publishes;
	routesStructure_ = structureRevision_;
	++routesCompiles_; // what every cache of the core ranges is keyed on (`drawerScanAt_`)
	// THE DEVICE NODES' INPUTS ARE THE ENGINE'S TO BUILD, from the same snapshot, with the host node's
	// cells handed over so a route from a macro points into `ParamSet`'s storage; and the host node's own
	// — the synth's parameters — are `ParamSet`'s, with the engine's cells handed the other way.
	lane_.setHostRoutes(params_.stagedRoutes());
	// AND THE ENGINE'S STEREO-LIVE FIXED POINT SETTLED NOW (backlog KU): this node's plans read which outputs are pairs,
	// and they are built at this block's top — before the lane's own begin would have rebuilt the engine's inputs.
	lane_.settleInputs();
	params_.rebuildCoreInputs(ParamSet::DeviceCells {
	    this,
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputCellOf(id, output);
	    },
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputCellForVoiceOf(id, output);
	    },
	    lane_.voices(),
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputWindowForVoiceOf(id, output);
	    },
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputWindowOf(id, output);
	    },
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputCellROf(id, output);
	    },
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputWindowROf(id, output);
	    },
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputPairsOf(id, output);
	    },
	    [](const void* h, DeviceInstanceId id, size_t output) {
		    return static_cast<const Synth*>(h)->lane_.outputWindowRForVoiceOf(id, output);
	    } });
}

void Synth::publishEnvelopes() {
	// **ONLY THE PLAYHEADS, NOW.** This copied every strip device's outputs into `ParamSet`'s source
	// table (`setDeviceSource`) so the resolve could read them by coordinate. A node publishes its own
	// outputs after its tick (`RackEngine::advanceModulators`) and every input reads the cell — so the
	// value half of this function has no job. The playhead is the editor's, and stays.
	for (size_t slot = 0; slot < playheads_.size(); ++slot) {
		playheads_[slot] = playheadOf(slot);
	}
	for (size_t position = 0; position < fxPlayheads_.size(); ++position) {
		fxPlayheads_[position] = fxPlayheadOf(position);
	}
}

DeviceInstanceId Synth::stripDevice(size_t slot) const noexcept {
	if (slot >= rack_.count || slot >= rack_.device.size()) {
		return kNoInstance;
	}
	// **BY ITS PLACE IN THE CHAIN** (kill-lanes step 3b): the state is each chain's ordered run, and
	// `stateOfChains` lays the strip's out in SLOT order — so strip slot `n` is chain-0 position `n`.
	return lane_.deviceOfStatePosition(kStripChain, slot);
}

DeviceInstanceId Synth::fxDevice(size_t position) const noexcept {
	if (position >= fxChainLength(fxChain_)) {
		return kNoInstance;
	}
	// The fx lane's own order is the chain's order, so a position is a position.
	return lane_.deviceOfStatePosition(kLaneChain, position);
}

size_t Synth::stripEngineSlot(size_t slot) const noexcept {
	if (slot >= rack_.count || slot >= rack_.device.size()) {
		return kNoEngineSlot;
	}
	// **THROUGH THE STATE THE ENGINE CONVERGED TO**, which is what names each device — the record's id from
	// a publish, its own from chains handed in as values. Asking that way needs no naming rule here, and
	// the two roads cannot disagree about which device stands where.
	const size_t at = lane_.slotOfStatePosition(kStripChain, slot);
	return at; // `kNoSlot` when it runs nowhere, which is `kNoEngineSlot`
}

size_t Synth::fxEngineSlot(size_t position) const noexcept {
	if (position >= fxChainLength(fxChain_)) {
		return kNoEngineSlot;
	}
	const size_t at = lane_.slotOfStatePosition(kLaneChain, position);
	return at; // `kNoSlot` when it runs nowhere, which is `kNoEngineSlot`
}

namespace {

/// ONE ENGINE SLOT'S RIGHT EAR, read the way the left is read into `modulatedFx_`/`modulated_` (backlog KU).
device::ControlMask rightEarOf(const rack::RackEngine& engine, size_t engineSlot, const device::Descriptor* desc,
                               size_t fieldsPresent, float* out, size_t n) noexcept {
	device::ControlMask mask {};
	if (desc == nullptr || engineSlot >= engine.slotCount()) {
		return mask;
	}
	const size_t controls = std::min({ fieldsPresent, desc->params.size(), n, device::kControlMaskBits });
	for (size_t c = 0; c < controls; ++c) {
		float right = 0.0f;
		if (engine.modulatedFieldRAt(engineSlot, c, right)) {
			out[c] = right;
			mask.set(c);
		}
	}
	return mask;
}

} // namespace

device::ControlMask Synth::fxRightEar(size_t position, float* out, size_t n) const noexcept {
	if (position >= fxChainLength(fxChain_) || position >= fxChain_.device.size()) {
		return {};
	}
	return rightEarOf(lane_.engineForTest(), fxEngineSlot(position), lane_.descriptorOfForTest(fxDevice(position)),
	                  fxChain_.device[position].row.fieldsPresent, out, n);
}

device::ControlMask Synth::stripRightEar(size_t slot, float* out, size_t n) const noexcept {
	if (slot >= rack_.count || slot >= rack_.device.size()) {
		return {};
	}
	return rightEarOf(lane_.engineForTest(), stripEngineSlot(slot), lane_.descriptorOfForTest(stripDevice(slot)),
	                  rack_.device[slot].fieldsPresent, out, n);
}

namespace {

/// A CHAIN INTO ONE GROWN BIGGER — the rows it holds, its count and its next serial. The destination's
/// storage is already the size it will be, so this copies and allocates nothing.
void copyGrown(const FxChain& from, FxChain& into) noexcept {
	into.count = from.count;
	into.nextInstanceId = from.nextInstanceId;
	std::copy_n(from.device.begin(), std::min(from.device.size(), into.device.size()), into.device.begin());
}
void copyGrown(const ModRack& from, ModRack& into) noexcept {
	into.count = from.count;
	into.nextInstanceId = from.nextInstanceId;
	std::copy_n(from.device.begin(), std::min(from.device.size(), into.device.size()), into.device.begin());
}
template <typename T>
void copyGrown(const std::vector<T>& from, std::vector<T>& into) noexcept {
	std::copy_n(from.begin(), std::min(from.size(), into.size()), into.begin());
}

} // namespace

bool Synth::requestChainCapacity(size_t fxCapacity, size_t stripCapacity, size_t stateCapacity) {
	if (fxCapacity <= requestedFx_ && stripCapacity <= requestedStrip_ && stateCapacity <= requestedState_) {
		return true; // already there, or on its way: a capacity never shrinks
	}
	(void)reapChainGrowth();
	// AND EVERY KIT PAD'S LANE'S PARKED DEVICES (drum proposal §17): an InnerChain is reaped by its holder's service.
	for (PadLane& p : padLanes_) {
		if (p.owned != nullptr) {
			(void)p.owned->service();
		}
	}
	if (growthState_.load(std::memory_order_acquire) != kGrowthIdle) {
		return false; // the last one has not met its drain yet
	}
	// THE CHAINS AT LEAST DOUBLE, so a run of single adds costs a handful of growths; the STATE is exactly the
	// shape asked for, because a published state of any other shape is refused by `adopt`.
	const size_t fx = fxCapacity > requestedFx_ ? std::max(fxCapacity, 2 * requestedFx_) : requestedFx_;
	const size_t strip = stripCapacity > requestedStrip_ ? std::max(stripCapacity, 2 * requestedStrip_) : requestedStrip_;
	const size_t state = std::max(stateCapacity, requestedState_);
	auto g = std::make_unique<ChainGrowth>();
	for (FxChain* c : { &g->fx, &g->adoptedFx, &g->modulatedFx }) {
		c->device.resize(fx);
	}
	for (ModRack* r : { &g->rack, &g->adoptedRack, &g->modulated }) {
		r->device.resize(strip);
	}
	g->state = rack::ChainState(state, chainState_.chainCount());
	g->staging = rack::ChainState(state, chainState_.chainCount());
	g->playheads.assign(strip, mod::Playhead {});
	g->fxPlayheads.assign(fx, mod::Playhead {});
	g->drivePeaks.assign(fx, -1.0f);
	g->outputPeaks.assign(fx, -1.0f);
	growth_ = std::move(g);
	requestedFx_ = fx;
	requestedStrip_ = strip;
	requestedState_ = state;
	growthState_.store(kGrowthPending, std::memory_order_release);
	return true;
}

void Synth::adoptChainGrowth() noexcept {
	if (growthState_.load(std::memory_order_acquire) != kGrowthPending) {
		return;
	}
	ChainGrowth& g = *growth_;
	// WHAT THE OLD BUFFERS HOLD, INTO THE NEW ONES — then swap, so `g` leaves holding the old storage for the
	// main thread to free. A swap of two vectors exchanges three pointers; nothing is allocated or freed.
	copyGrown(fxChain_, g.fx);
	copyGrown(adoptedFx_, g.adoptedFx);
	copyGrown(modulatedFx_, g.modulatedFx);
	copyGrown(rack_, g.rack);
	copyGrown(adoptedRack_, g.adoptedRack);
	copyGrown(modulated_, g.modulated);
	(void)g.state.assignGrown(chainState_);
	(void)g.staging.assignGrown(chainStaging_);
	copyGrown(playheads_, g.playheads);
	copyGrown(fxPlayheads_, g.fxPlayheads);
	copyGrown(fxDrivePeaks_, g.drivePeaks);
	copyGrown(fxOutputPeaks_, g.outputPeaks);
	std::swap(fxChain_, g.fx);
	std::swap(adoptedFx_, g.adoptedFx);
	std::swap(modulatedFx_, g.modulatedFx);
	std::swap(rack_, g.rack);
	++rackRevision_; // the same rows in new storage: `modulated_` is copied again rather than reasoned about
	std::swap(adoptedRack_, g.adoptedRack);
	std::swap(modulated_, g.modulated);
	swap(chainState_, g.state);
	swap(chainStaging_, g.staging);
	playheads_.swap(g.playheads);
	fxPlayheads_.swap(g.fxPlayheads);
	fxDrivePeaks_.swap(g.drivePeaks);
	fxOutputPeaks_.swap(g.outputPeaks);
	growthState_.store(kGrowthTaken, std::memory_order_release);
}

bool Synth::reapChainGrowth() noexcept {
	if (growthState_.load(std::memory_order_acquire) != kGrowthTaken) {
		return false;
	}
	growth_.reset();
	growthState_.store(kGrowthIdle, std::memory_order_release);
	return true;
}

bool Synth::adoptChains(const rack::PublishedChainState& published) noexcept {
	// A GROWTH FIRST, so a publish that needed the room lands in it on this same block.
	adoptChainGrowth();
	if (published.sequence() == chainsSequence_) {
		return false; // every block on which nobody touched a chain
	}
	// INTO STAGING, AND SWAPPED IN ONLY WHOLE: a publish landing while we read leaves `chainState_` as it
	// was — which the lane syncs from every block. (This snapshotted straight into `chainState_`, so a
	// refused read left the lane a torn state to sync from; see `PublishedChainState`.)
	if (!published.adopt(&chainState_, &chainStaging_, &chainsSequence_)) {
		return false; // keep what we have, ask again next block
	}
	chainsAdoptedMirror_.store(chainsSequence_, std::memory_order_release);
	++structureRevision_;
	chainsOfState(chainState_, &adoptedFx_, &adoptedRack_);
	rack_ = adoptedRack_;
	fxChain_ = adoptedFx_;
	// AND THE LANE SYNCS FROM THIS SNAPSHOT, NAMING ITS PUBLISH — so a device the editor's visit built for
	// a later publish than this one is kept for the sync that has it, not handed back (5b C5b).
	chainsFromPublish_ = true;
	return true;
}

size_t Synth::provisionDevices(const FxChain& chain, const ModRack& rack,
                               ContainerBodies* bodies) {
	// THE LANE BUILDS EVERY DEVICE WITH ITS VOICE INSTANCES BESIDE IT: the poly switch flips on the audio
	// thread, and a device that had to be built the first time it became per-voice would allocate
	// there. See `RackEngine::Slot::voices`.
	//
	// **AND THE MOD LANE, IN THE SAME CALL RATHER THAN IN A SECOND ONE THE CALLER MUST REMEMBER.**
	// Every caller of this is the once-per-main-thread-visit place; a separate entry point for the
	// rack would be a fourth site that has to agree with three others, which is the shape this file
	// already carries two scars from — `setModRack` exists because a pair of sites that had to agree
	// disagreed. A rack device left unprovisioned is a modulator that silently produces nothing.
	return lane_.provision(chain, rack, bodies);
}

size_t Synth::provisionDevices(const rack::ChainState& state, uint32_t forPublish,
                               ContainerBodies* bodies) {
	// THE SAME BUILD FROM THE BRIDGE'S RECORD — its state and the publish it went out as. See
	// `LaneRack::provision`.
	//
	// **AND ROOM FOR IT, ASKED HERE, WHERE ALLOCATION IS ALLOWED** (unbounded devices step 5): the chains this
	// synth copies on the audio thread grow to the state's shape before the drain is asked to take it. A
	// growth still waiting for its drain refuses a second; the next provision asks again.
	(void)requestChainCapacity(std::max(state.length(kLaneChain), kMaxFxSlots),
	                           std::max(state.length(kStripChain), kMaxRackSlots), state.capacity());
	return lane_.provision(state, forPublish, bodies);
}

void Synth::setOuterGateMaker(device::OuterGate (*make)(const void* host, DeviceInstanceId self), const void* host) noexcept {
	lane_.setOuterGateMaker(make, host);
}

rack::RouteRefusals Synth::takeOuterDrops() noexcept {
	return lane_.takeOuterDrops();
}

const mod::mseg::Shape& Synth::msegShapeForTest(size_t slot) const noexcept {
	// A SLOT NOT RUNNING AN MSEG HAS NO SHAPE, and this is what it says so with. See the declaration:
	// the array this replaced held one per slot whether or not the slot wanted it, so the seam
	// answered for an envelope with a default shape that read exactly like a stopped MSEG.
	static const mod::mseg::Shape kNone {};
	if (!rack_.holds(slot, mod::mseg::kTraits.tag)) {
		return kNone;
	}
	const void* storage = lane_.storageOfForTest(stripDevice(slot));
	if (storage == nullptr) {
		return kNone;
	}
	// THE RUNNER'S OWN STORAGE, READ BACK AS WHAT WAS CONSTRUCTED IN IT. `ModOps::construct` for this
	// tag placement-news an `mseg::Shape` there — mseg.cpp's table says so — and the tag test above is
	// what makes that the right type rather than a hope.
	return *static_cast<const mod::mseg::Shape*>(storage);
}

void Synth::drainRetiredDevices() {
	// THE ENGINE'S PARKING AND THE LANE'S SPENT PROVISIONS — every device the lane and the strip let go.
	lane_.reap();
	// AND THE OLD CHAIN BUFFERS A GROWTH LEFT BEHIND (unbounded devices step 3).
	(void)reapChainGrowth();
	// AND EVERY KIT PAD'S LANE'S PARKED DEVICES (drum proposal §17): an InnerChain is reaped by its holder's service.
	for (PadLane& p : padLanes_) {
		if (p.owned != nullptr) {
			(void)p.owned->service();
		}
	}
}

bool Synth::noteStackHeld() const {
	for (const Voice& v : voices_) {
		// Attack and Sustain are "the key is down, or the pedal is holding it". Release is not, and
		// neither is Stealing — a voice getting out of the way of a newer note is not a note anybody
		// is still holding, and counting it would hold the gate open through every steal.
		if (v.stage == Voice::Stage::Attack || v.stage == Voice::Stage::Sustain) {
			return true;
		}
	}
	return false;
}

mod::Playhead Synth::playheadOf(size_t slot) const {
	// WHICHEVER DEVICE THE SLOT HOLDS, asked ONCE. Both roads below call this rather than each
	// branching on the tag themselves — a second copy of that branch is how `publishEnvelopes` and
	// `tickEnvelopes` would come to disagree about which device a slot is running, which is the exact
	// shape the lane bug next door was.
	//
	// A SLOT PAST THE RACK'S COUNT IS IDLE, not stale: a shrunk rack must stop reporting a position
	// for a device that is no longer there, or a marker goes on standing where nothing is playing.
	if (slot >= std::min(rack_.count, rack_.device.size())) {
		return {};
	}
	// **A SWITCHED-OFF MODULATOR REPORTS NO POSITION,
	//
	// THE DEVICE IS STILL RUNNING AND THAT STAYS TRUE — `publishedSourceAt` explains why, and it is
	// the rack's reasoning rather than an economy: switching back on should put an LFO where the music
	// is, not where it left. So the tick is untouched here too. What was wrong is that the position
	// was published ANYWAY, so the card drew a star sweeping a shape that was reaching nothing. That
	// star read as proof the modulator was live, and an evening went on looking for the break
	// downstream of a device that was simply switched off.
	//
	// THE HONESTY RULE, WHICH IS THE ONE THIS BREAKS: a control is enabled iff it changes sound. The
	// bypass square does change sound, so the switch was honest; the STAR was not, because a moving
	// playhead is the card's way of saying "this is what I am contributing right now" and it was
	// contributing nothing. The display must follow what is PUBLISHED, not what the device is doing
	// privately.
	//
	// IDLE RATHER THAN FROZEN, deliberately. A star held still at its last position is a claim that
	// the shape is parked THERE, which is not true either — the device has run on underneath. `{}` is
	// kIdle, the one answer registry.h blesses for "cannot say", and it is what the slot past the
	// count above already returns.
	if (!modulatorSwitchedOnAt(slot)) {
		return {};
	}
	// ASKED OF THE DEVICE, WHICH IS WHAT REMOVED THE BRANCH. The comment above says both roads call
	// this rather than branching on the tag themselves; now nothing branches on the tag at all.
	//
	// **FALSE IS "CANNOT SAY", AND IDLE IS THE HONEST ANSWER TO IT** — registry.h forbids reading a
	// missing `playhead` as zero, because ordinal 0 is Idle for a DAHDSR and a valid segment for an
	// MSEG. `mod::Playhead{}` is kIdle, so the default-constructed return is already that answer.
	mod::Playhead where {};
	if (!lane_.playheadOf(stripDevice(slot), &where.ordinal, &where.phase)) {
		return {};
	}
	// AND WHERE IT STANDS, for a device whose picture cannot say it (`mod::Playhead::level`).
	where.haveLevel = lane_.playheadLevelOf(stripDevice(slot), &where.level);
	where.haveTrigFires = lane_.trigFiresOf(stripDevice(slot), &where.trigFires); // the Trig pad's light (MV)
	(void)lane_.recentEventsOf(stripDevice(slot), where.recent.data(), where.recent.size()); // the frame's events
	return where;
}

float Synth::publishedSourceAt(size_t slot) const {
	// **A DISABLED MODULATOR PUBLISHES ITS REST VALUE — it does not freeze, and it does not go on
	// modulating.** The rack has said this since it was written (`RackEngine::sourceValue`) and
	// Feathers did not say it at all: the mod strip published `outputAt` unconditionally, so the
	// bypass square the shared head draws was a control that changed nothing. The switches were added;
	// this is what makes them mean something, and the honesty rule is explicit that a control is
	// enabled iff it changes sound through a real param.
	//
	// **THE DEVICE KEEPS RUNNING, WHICH IS THE RACK'S REASONING AND NOT AN ECONOMY.** Its comment:
	// switching back on "makes the switch a state-editing gesture rather than a mute; an LFO in
	// particular should be where the music is when it comes back, not where it was when it left." So
	// the tick is untouched and only the PUBLICATION is gated.
	//
	// READ FROM THE MODULATED ROW, so a routed enable would be honoured too — the row is the resolved
	// one and the question is what the device is actually set to this block. That read now lives in
	// `modulatorSwitchedOnAt`, because the PLAYHEAD has to ask the same question and two spellings of
	// it would be a card whose star and whose sound disagree about whether the device is on.
	return publishedSourceAt(slot, kEnvelopeOutput);
}

float Synth::publishedSourceAt(size_t slot, size_t output) const {
	// **EVERY OUTPUT, NOT JUST THE FIRST — and this was a real bug found by asking what the Note
	// Expressions device's Rule knob was for.**
	//
	// The publish loop wrote `kEnvelopeOutput` and nothing else, which was right for as long as every
	// modulator published exactly one signal — the loop's own comment said so: *"a modulator is a
	// source of one signal whatever shape it makes."* That stopped being true the moment a device
	// declared seven. Six of the Note Expressions device's outputs had NO collapsed value at all, so a
	// route from Pressure to anything that is not per-voice read zero — the device looked wired and
	// was one-seventh wired.
	//
	// The gate is the same one: a disabled modulator publishes its rest value on every output rather
	// than only on its first.
	// THE NODE'S PUBLISHED CELL — the disable rule applied where it was published (`RackEngine::advanceModulators`),
	// which is the one number every input pointing at the device reads. `modulatorSwitchedOnAt` still answers
	// the playhead's question; the value no longer needs a second gate here.
	return lane_.publishedOutputOf(stripDevice(slot), output);
}

uint64_t Synth::stripExtraState(size_t slot, float* extra, size_t capacity, size_t* count,
                                DeviceInstanceId* device) const {
	*device = stripDevice(slot);
	*count = 0;
	return *device == kNoInstance ? 0 : lane_.extraStateOf(*device, extra, capacity, count);
}

uint64_t Synth::fxExtraState(size_t position, float* extra, size_t capacity, size_t* count,
                             DeviceInstanceId* device) const {
	*device = fxDevice(position);
	*count = 0;
	return *device == kNoInstance ? 0 : lane_.extraStateOf(*device, extra, capacity, count);
}

mod::Playhead Synth::fxPlayheadOf(size_t position) const {
	const DeviceInstanceId id = fxDevice(position);
	const device::Descriptor* desc = lane_.descriptorOfForTest(id);
	if (desc == nullptr || desc->mod == nullptr) {
		return {}; // nothing there, or an audio device: its live values go through `readingAt`
	}
	// SWITCHED OFF IS IDLE, `playheadOf`'s rule — asked of the ENGINE'S resolved row, which is what decides
	// that the device publishes its rest (`RackEngine::advanceModulators`), so the marker and the value agree.
	if (const int enable = desc->traits.enableControl; enable >= 0) {
		const size_t slot = fxEngineSlot(position);
		if (lane_.engineForTest().modulatedFieldAt(slot, static_cast<size_t>(enable)) < 0.5f) {
			return {};
		}
	}
	mod::Playhead where {};
	// THE TRIG'S COUNT WHETHER OR NOT THERE IS A PLAYHEAD: NOISE has a Trig and no position (backlog MZ).
	uint32_t fires = 0;
	const bool haveFires = lane_.trigFiresOf(id, &fires); // the Trig pad's light (MV)
	if (lane_.playheadOf(id, &where.ordinal, &where.phase)) {
		where.haveLevel = lane_.playheadLevelOf(id, &where.level); // `mod::Playhead::level`
		(void)lane_.recentEventsOf(id, where.recent.data(), where.recent.size());
	}
	else {
		where = {};
	}
	where.haveTrigFires = haveFires;
	where.trigFires = fires;
	return where;
}

bool Synth::modulatorSwitchedOnAt(size_t slot) const {
	if (slot >= modulated_.device.size()) {
		return true;
	}
	const device::Descriptor* desc = descriptorForRow(modulated_.device[slot]);
	if (desc == nullptr || desc->traits.enableControl < 0) {
		return true; // a device with no bypass is always publishing
	}
	const auto control = static_cast<size_t>(desc->traits.enableControl);
	const DeviceRow& row = modulated_.device[slot];
	if (control >= row.fieldsPresent) {
		// A CONTROL THE ROW DOES NOT REACH IS AT ITS DECLARED DEFAULT — device_row.h's rule — and the
		// declared default is on, so a narrow row modulates rather than falling silent.
		return true;
	}
	return row.field[control] >= 0.5f;
}

mod::mseg::Settings Synth::modulatedMsegSettings(size_t slot) const {
	// **DECODED FROM THE ROW THE ENGINE ACTUALLY RUNS, NOT WALKED AGAIN.** This used to be its own
	// copy of the modulation walk, ending in `mseg::setValueOf` on a typed Settings; the DAHDSR had
	// a second copy beside it. One of the two was once wrong about which control a descriptor named
	// (see the case at tests/test_mod_rack.cpp) — which is what two copies of a walk are for.
	//
	// IT IS A TEST SEAM AND THAT IS EXACTLY WHY IT MUST NOT BE A SECOND IMPLEMENTATION. A seam that
	// recomputes what it is meant to observe can agree with itself while the engine does something
	// else, and the case reading it would go green on a road nothing runs.
	// **THE ENGINE'S OWN RESOLVED ROW** — what the MSEG was last handed, read back rather than resolved
	// again here (the strip runs on the lane's engine now).
	DeviceRow row = slot < rack_.device.size() ? rack_.device[slot] : DeviceRow {};
	{
		const size_t lane = stripEngineSlot(slot);
		const device::Descriptor* desc = lane_.descriptorOfForTest(stripDevice(slot));
		if (desc != nullptr) {
			const size_t controls = std::min(row.fieldsPresent, desc->params.size());
			for (size_t c = 0; c < controls; ++c) {
				row.field[c] = lane_.engineForTest().modulatedFieldAt(lane, c);
			}
		}
	}
	// COPIED INTO THE DEVICE'S OWN ROW WIDTH rather than handed the carrier's: DeviceRow is 64 fields
	// wide because it must hold the widest device, and the count travels so the device puts fields it
	// did not receive at their declared defaults.
	mod::mseg::Row narrow {};
	const size_t take = std::min(row.fieldsPresent, narrow.size());
	for (size_t i = 0; i < take; ++i) {
		narrow[i] = row.field[i];
	}
	return mod::mseg::settingsOf(narrow, take);
}

const void* Synth::fxStorageForTagForTest(const char* tag) const {
	// WHAT THE BUS IS RUNNING, and null for a device that is in its per-voice arrangement instead.
	// It walked every CONSTRUCTED slot until the per-voice runner existed, which was the same answer
	// then and is the wrong one now: both runners construct the whole chain and only the
	// responsibility differs, so a constructed-only search hands back a device the bus is not running
	// and whose target is whatever it was last left at. The null IS the complementary-halves claim.
	return lane_.busStorageForTag(tag, /*running=*/true);
}

const void* Synth::voiceFxStorageForTest(int slot, const char* tag) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return nullptr;
	}
	return lane_.voiceStorageForTag(static_cast<size_t>(slot), tag);
}

fx::filter::Settings Synth::busFilterTargetForTest() const {
	// THE BUS INSTANCE WHETHER OR NOT THE BUS RAN IT — what this seam has always answered.
	const void* storage = lane_.busStorageForTag(fx::filter::kTraits.tag, /*running=*/false);
	return storage != nullptr ? fx::filter::opsTargetForTest(storage) : fx::filter::Settings {};
}

device::NoteContext Synth::busNoteContext() const {
	// THE LAST NOTE STILL HELD (2026-08-24, replacing the lowest-note rule): mono follows the 'last' active note rather than the
	// lowest. E.g. d0 is held, f0 is played temporarily and released: the sub should porta up to f0 and back down to d0.
	//
	// THAT EXAMPLE IS THE WHOLE SPECIFICATION and it is a STACK, not a scalar: on a release the
	// device does not go silent and does not stay at the released pitch — it returns to the most
	// recent note that is still down. Three notes held and the middle one released, nothing moves.
	//
	// THE STACK IS THE MAXIMUM SERIAL AMONG HELD VOICES, which needs no structure and no allocation:
	// startSerial only climbs, so the newest held voice is the top by construction, and a release
	// simply removes a candidate. A "previous note" variable would get the three-note case wrong and
	// would pass every two-note test — the degenerate-fixture shape this repo keeps meeting.
	//
	// HELD VOICES ONLY. A voice in Release is still sounding but its key is up; letting a decaying
	// note go on defining the pitch would make the device follow a note the player has let go of, and
	// the gate below already says the phrase has ended.
	device::NoteContext note;
	note.gate = false;
	uint64_t newest = 0;
	double pitchOf = 0.0;
	for (const Voice& v : voices_) {
		if (v.stage != Voice::Stage::Attack && v.stage != Voice::Stage::Sustain) {
			continue;
		}
		if (!note.gate || v.startSerial > newest) {
			newest = v.startSerial;
			pitchOf = v.haveTarget ? v.lastTargetSemitones : static_cast<double>(v.key);
		}
		note.gate = true;
	}
	if (!note.gate) {
		// THE PITCH IS HELD RATHER THAN ZEROED when the phrase ends, so a release tail does not slide
		// to the bottom of the keyboard on its way out. Zero semitones is not "no note", it is a very
		// low one — the same trap Synth::renderVoices names on the keytrack road.
		return device::NoteContext {lastBusPitch_, false};
	}
	lastBusPitch_ = static_cast<float>(pitchOf);
	return device::NoteContext {lastBusPitch_, true};
}


void Synth::pluckForNoteOn() {
	// THE FIRMWARE'S PLUCK PER NOTE-ON, on Resonate, at the laws' own amplitude and position, and only for
	// a note that actually STARTS SOUNDING. It is called from the voice manager's own answers to a note-on
	// (a voice started or retriggered, or a mono/legato move it accepted, all inside deliverNotes' On arm
	// where pendingNoteOn_ is valid), so:
	//  - a note the lane invents plucks and one it swallows does not (what leaves the lane is what plays);
	//  - a note mono priority REJECTS plucks nothing, because the manager never answers it;
	//  - a note-off that hands a mono voice back to a held note plucks nothing (no note-on is pending).
	// Queued for the string's next sub-tick, so it lands at the same sample at every block size; plucks
	// do not stack, so a chord's several starts are one pluck; every voice hears it, there being one string.
	if (pendingNoteOn_.valid && params_.oscType() == OscType::Resonate) {
		weave_.pluck();
	}
}

void Synth::pluckStrings(Voice& voice) {
	// ON PLUCK ALONE: a pluck left waiting in a voice of another engine would sound the moment an engine switch reached it,
	// and an engine switch plucks nothing (K12, by design). Each part's seed is its own (its fan index in
	// the hash), so a stack of strings does not phase-lock into one louder string.
	if (params_.oscType() != OscType::Pluck) {
		return;
	}
	const auto seed = static_cast<uint64_t>(std::lround(params_.base(ParamId::KsSeed)));
	const int64_t tick = songTickNow();
	const auto key = static_cast<uint64_t>(static_cast<uint16_t>(voice.key));
	// THE NOTE'S VELOCITY, as its key went down (a mono return to a held key reads that key's own).
	const double held = heldNote(voice.channel, voice.key).velocity;
	const double velocity = held > 0.0 ? std::clamp(held, 0.0, 1.0) : 1.0;
	uint64_t when = 0;
	if (tick >= 0) {
		when = (static_cast<uint64_t>(tick) << 1) | 1u; // the song's place, never colliding with a count
	}
	else {
		uint32_t& strikes = ks_->strikes[static_cast<size_t>(voice.key & 127)];
		when = static_cast<uint64_t>(strikes) << 1;
		++strikes;
	}
	for (int part = 0; part < kMaxStack; ++part) {
		uint64_t z = seed * 0x9E3779B97F4A7C15ull ^ (key << 40) ^ (static_cast<uint64_t>(static_cast<uint8_t>(voice.channel)) << 32)
		             ^ (when * 0xC2B2AE3D27D4EB4Full) ^ (static_cast<uint64_t>(part) << 56);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		z ^= z >> 31;
		OscVoice& osc = partOsc(voice, part);
		osc.pluckString(z, velocity);
		osc.releaseString(false);
	}
}

void Synth::strikeDrums(Voice& voice, uint8_t pad) {
	// ON DRUM ALONE: an engine switch strikes nothing (as for Pluck, K12). Each part's seed is its own.
	if (params_.oscType() != OscType::Drum) {
		return;
	}
	const bool kit = params_.drumKit();
	const auto seed = static_cast<uint64_t>(std::lround(params_.base(ParamId::DrumSeed)));
	const int64_t tick = songTickNow();
	// THE HIT'S IDENTITY: in Kit mode the PAD (two keys mapped to one pad are one drum), in Pitched mode the key.
	const auto who = static_cast<uint64_t>(kit ? pad : static_cast<uint16_t>(voice.key & 127));
	const double held = heldNote(voice.channel, voice.key).velocity;
	const double velocity = held > 0.0 ? std::clamp(held, 0.0, 1.0) : 1.0;
	uint64_t when = 0;
	if (tick >= 0) {
		when = (static_cast<uint64_t>(tick) << 1) | 1u; // the song's place, never colliding with a count
	}
	else {
		uint32_t& strikes = drum_->strikes[static_cast<size_t>(who & 127)];
		when = static_cast<uint64_t>(strikes) << 1;
		++strikes;
	}
	for (int part = 0; part < kMaxStack; ++part) {
		uint64_t z = (seed + 0xD5A1u) * 0x9E3779B97F4A7C15ull ^ (who << 40) ^ (when * 0xC2B2AE3D27D4EB4Full)
		             ^ (static_cast<uint64_t>(part) << 56) ^ (kit ? 0x4B17ull : 0ull);
		z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
		z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
		z ^= z >> 31;
		// THE KEY'S OWN PITCH (key and retune): a kit pad is moved by its Bend share of whatever the voice adds past it and the
		// kit's transpose, from the strike on (2026-11-07: a bend held into a hit is the 808 glide).
		partOsc(voice, part).strikeDrum(pad, z, velocity, static_cast<double>(voice.key) + voice.retuneSemitones);
	}
	// THE HIT, PUBLISHED FOR THE PADS' AND CARDS' PULSE: a count and the velocity it came at,
	// per pad, read by the editor through the bridge. Plain members on the audio thread; nothing allocates. A muted pad is
	// never struck (beginVoiceCreationTransaction), so it never counts; a choked pad's count is its own last hit's.
	if (kit && pad < kMaxSounds) {
		++padHits_.count[pad];
		padHits_.velocity[pad] = static_cast<float>(velocity);
	}
}

bool Synth::padAudible(size_t slot) const {
	if (slot >= kMaxSounds) {
		return false;
	}
	const bool on = std::lround(params_.resolved(drumParam(slot, DrumControl::Mute))) == 0;
	return on && (!preview_.active() || preview_.slot == slot);
}

void Synth::fillDrumKit() {
	drum::KitSetup& kit = drum_->kit;
	kit.kit = params_.drumKit();
	kit.panned = false;
	// THE KIT-WIDE TRANSPOSE (2026-11-07): the Engine rail's Oct, St, Coarse and Fine move every pad, in full.
	kit.transposeSemitones = 12.0 * params_.resolved(ParamId::TransposeOct) + params_.resolved(ParamId::TransposeSemi)
	                         + params_.resolved(ParamId::Coarse) + params_.resolved(ParamId::FineTune) / 100.0;
	const auto kindOf = [](double v) { return static_cast<drum::Kind>(steppedIndex(v, kDrumKindNames.size())); };
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		drum::PadSetup& p = kit.pads[slot];
		const auto r = [&](DrumControl c) { return params_.resolved(drumParam(slot, c)); };
		// PITCHED MODE IS PAD A'S BLOCK UNDER THE ENGINE'S KIND (proposal §7, as built §11): Kind, Note, Choke and Voices are
		// the kit's, everything else the one drum's.
		p.kind = kit.kit || slot != 0 ? kindOf(r(DrumControl::Kind)) : kindOf(params_.resolved(ParamId::DrumPitchedKind));
		p.controls.decaySeconds = std::exp2(r(DrumControl::Decay));
		p.controls.sweepSeconds = std::exp2(r(DrumControl::SweepTime));
		p.controls.drive = r(DrumControl::Drive);
		p.controls.count = static_cast<int>(std::lround(r(DrumControl::Count)));
		p.controls.spreadSeconds = std::exp2(r(DrumControl::Spread));
		p.controls.pitchRangeSemitones = kit.kit ? 3.0 : 0.5;
		// THE THREE ZONE COORDINATES (drum proposal §14): what velocity reaches, the wires, the bells (lawsFor reads them).
		p.controls.velocityZone = r(DrumControl::VelocityZone);
		p.controls.wires = r(DrumControl::Wires);
		p.controls.bells = r(DrumControl::Bells);
		// THE SNARE'S ENVELOPES AND BELL (read by a Snare alone).
		p.controls.wireDecaySeconds = std::exp2(r(DrumControl::WireDecay));
		p.controls.clickDecaySeconds = std::exp2(r(DrumControl::ClickDecay));
		p.controls.bellStartHz = std::exp2(r(DrumControl::BellStart));
		p.controls.bellEndHz = std::exp2(r(DrumControl::BellEnd));
		p.controls.bellSeconds = std::exp2(r(DrumControl::BellTime));
		// THE KICK'S SHAPE (read by a Kick alone): Hold and the dip's time in ms, the click sweep, the start phase in degrees.
		p.controls.kickHoldSeconds = r(DrumControl::KickHold) / 1000.0;
		p.controls.kickDip = r(DrumControl::KickDip);
		p.controls.kickDipSeconds = r(DrumControl::KickDipTime) / 1000.0;
		p.controls.clickSweepOctaves = r(DrumControl::ClickSweep);
		p.controls.clickSweepSeconds = std::exp2(r(DrumControl::ClickSweepTime));
		p.controls.startPhaseDegrees = r(DrumControl::StartPhase);
		// THE DRIVE'S CURVE AND TONE (every kind) AND THE CLAP'S FILTER (a Clap alone): the high-pass in Hz.
		p.controls.driveCurve = static_cast<int>(steppedIndex(r(DrumControl::DriveCurve), kDrumDriveCurveNames.size()));
		p.controls.driveTone = r(DrumControl::DriveTone);
		p.controls.clapQ = r(DrumControl::ClapQ);
		p.controls.clapHighPassHz = std::exp2(r(DrumControl::ClapHighPass));
		// THE SECOND BELL (a Snare alone): Hz and Time from their logs.
		p.controls.bell2Hz = std::exp2(r(DrumControl::Bell2Hz));
		p.controls.bell2Seconds = std::exp2(r(DrumControl::Bell2Time));
		p.controls.velocityNoise = r(DrumControl::VelocityNoise);
		// THE WIRES' CONTOUR, BUZZ AND THRESHOLD (a Snare), WIDTH (a Snare, a Clap).
		p.controls.width = r(DrumControl::Width);
		p.tuneSemitones = r(DrumControl::Tune);
		p.gain = static_cast<float>(std::pow(10.0, r(DrumControl::Level) / 20.0));
		p.pan = static_cast<float>(r(DrumControl::Pan));
		p.gate = steppedIndex(r(DrumControl::Trigger), kDrumTriggerNames.size()) == 1;
		p.releaseSeconds = std::exp2(r(DrumControl::Release));
		p.bendAmount = r(DrumControl::Bend);
		// THE LAYER (stage (c)): Kit mode alone. Its kinds are Off then Kick..Clap in Kind's own order, so index − 1 is the Kind.
		const size_t layerKind = steppedIndex(r(DrumControl::LayerKind), kDrumLayerKindNames.size());
		p.layer.on = kit.kit && layerKind > 0;
		p.layer.kind = static_cast<drum::Kind>(layerKind > 0 ? layerKind - 1 : 0);
		const double layerDb = r(DrumControl::LayerLevel);
		p.layer.gain = layerDb <= -60.0 ? 0.0f : static_cast<float>(std::pow(10.0, layerDb / 20.0));
		p.layer.delaySeconds = r(DrumControl::LayerDelay) / 1000.0;
		p.layer.highPassHz = std::exp2(r(DrumControl::LayerHighPass));
		// A PAD WITH WIDTH TAKES THE STEREO ROAD too: its two ears differ by its own decorrelated noise.
		if ((kit.kit || slot == 0) && (p.pan != 0.0f || (p.controls.width > 0.0 && drum::kindControls(p.kind).width))) {
			kit.panned = true;
		}
	}
}


// ---- THE PER-PAD FX LANES (drum proposal §17, stage (d)) ---------------------------------------------------------------------

namespace {
/// A PAD LANE'S AXES: Delay's feedback lane's (no core sources or destinations yet; the holder road is §17's P4), sixteen slots.
constexpr backplane::Axes kPadLaneAxes {
	.coreSources = 0,
	.coreDestinations = 0,
	.deviceSlots = 16,
	.outputStride = device::kMaxDeviceOutputs,
};
} // namespace

rack::InnerChain* Synth::ensurePadLane(size_t pad) {
	if (pad >= padLanes_.size()) {
		return nullptr;
	}
	PadLane& p = padLanes_[pad];
	if (p.owned == nullptr) {
		auto lane = std::make_unique<rack::InnerChain>(kPadLaneAxes);
		if (havePadCatalogue_) {
			lane->adopt(padCatalogue_);
		}
		lane->setVoices(1); // BUS ONLY: a pad lane runs once, on its pad's bus
		if (padLaneRate_ > 0.0) {
			lane->prepare(padLaneRate_, padLaneFrames_);
			padAlign_[pad].prepare(padLaneRate_); // its latency's alignment, and the plain mix's (drum proposal §17 P4)
			// THE PLAIN MIX'S RING ONCE ONLY: a lane already live may be delaying through it on the audio thread right now, and the
			// lane being made here is not published yet, so its own ring above is safe.
			if (directAlign_.capacity() == 0) {
				directAlign_.prepare(padLaneRate_);
			}
		}
		p.owned = std::move(lane);
		p.published.store(p.owned.get(), std::memory_order_release);
	}
	return p.owned.get();
}

bool Synth::padLanesHoldTail() const noexcept {
	for (const PadLane& p : padLanes_) {
		const rack::InnerChain* lane = p.published.load(std::memory_order_acquire);
		if (lane != nullptr && lane->holdsTail()) {
			return true;
		}
	}
	return false;
}

void Synth::beginPadLanes() {
	// EVERY PUBLISHED LANE IS DRAINED AT THE BLOCK'S START (beginHostBlock), live or not, so an edit lands; a lane is LIVE
	// while it holds a device that is on (Delay's rule: one whose every device is off and parked is no lane), and only in a
	// kit. Its pad's voices then sum into its bus.
	padLanesLive_ = false;
	padLatency_ = 0;
	const bool kit = params_.drumKit();
	for (PadLane& p : padLanes_) {
		p.running = p.published.load(std::memory_order_acquire);
		p.live = false;
		if (p.running == nullptr) {
			continue;
		}
		p.running->setTransportClock(rackClock());
		p.running->beginHostBlock();
		p.live = kit && p.running->engine().chainLength(0) > 0 && !p.running->engine().allParked();
		p.latency = p.live ? p.running->hostBlockLatency() : 0u;
		padLatency_ = std::max(padLatency_, p.latency);
		padLanesLive_ = padLanesLive_ || p.live;
	}
}

void Synth::endPadLanes() {
	for (PadLane& p : padLanes_) {
		if (p.running != nullptr) {
			p.running->endHostBlock();
		}
	}
}

void Synth::runPadLanes(float* left, float* right, int n) {
	// EACH LIVE PAD'S BUS THROUGH ITS LANE, then into the mix. Run every chunk while live, voices or not, so a tail rings out.
	// LATENCY ALIGNED (P4): while a live lane is latent, the plain mix waits for the latest one, and each bus for the rest of it.
	if (padLatency_ > 0) {
		directAlign_.process(left, right, n, padLatency_);
	}
	for (size_t pad = 0; pad < padLanes_.size(); ++pad) {
		PadLane& p = padLanes_[pad];
		if (!p.live) {
			continue;
		}
		float* busL = padBusL_.data() + pad * static_cast<size_t>(kRenderChunk);
		float* busR = padBusR_.data() + pad * static_cast<size_t>(kRenderChunk);
		p.running->process(busL, busR, n);
		if (padLatency_ > p.latency) {
			padAlign_[pad].process(busL, busR, n, padLatency_ - p.latency);
		}
		for (int i = 0; i < n; ++i) {
			left[i] += busL[i];
			right[i] += busR[i];
		}
	}
}
void Synth::endDrumVoice(Voice& v) {
	if (v.stage != Voice::Stage::Attack && v.stage != Voice::Stage::Sustain) {
		return;
	}
	chokeLaneNote(v);
	beginFade(v, Voice::Stage::Release, releaseEdgeSeconds_, sampleRate_, 0);
	++drum_->ended;
}

void Synth::applyDrumChokes() {
	const int n = drum_->startedCount;
	drum_->startedCount = 0;
	if (n == 0 || !params_.drumKit()) {
		return;
	}
	for (int i = 0; i < n; ++i) {
		const uint8_t pad = drum_->startedPads[static_cast<size_t>(i)];
		const size_t group = steppedIndex(params_.resolved(drumParam(pad, DrumControl::Choke)), kDrumChokeNames.size());
		if (group == 0) {
			continue;
		}
		for (Voice& v : voices_) {
			if (v.stage == Voice::Stage::Idle || v.stage == Voice::Stage::Stealing) {
				continue;
			}
			// NOT A VOICE THIS NOTE STARTED (a layered note's two pads do not choke each other), AND NOT THE SAME PAD (its
			// own repeats are its Voices' business, proposal §4.2).
			const bool startedNow = std::find(drum_->started.begin(), drum_->started.begin() + n, &v)
			                        != drum_->started.begin() + n;
			const uint8_t other = v.osc.drumPad();
			if (startedNow || other == pad) {
				continue;
			}
			if (steppedIndex(params_.resolved(drumParam(other, DrumControl::Choke)), kDrumChokeNames.size()) == group) {
				responder_.terminateVoice(&v);
				++drum_->chokes;
			}
		}
	}
}

ks::StringControls Synth::stringControls() const {
	ks::StringControls c;
	c.decaySeconds = std::exp2(params_.resolved(ParamId::KsDecay));
	c.decayTrack = params_.resolved(ParamId::KsDecayTrack);
	c.releaseSeconds = std::exp2(params_.resolved(ParamId::KsRelease));
	c.hardness = static_cast<float>(params_.resolved(ParamId::KsHardness));
	c.pickup = params_.resolved(ParamId::KsPickup);
	c.width = params_.resolved(ParamId::KsWidth);
	c.velocity = params_.resolved(ParamId::KsVelocity);
	// THE TOPS ARE "RING": the declared maxima are exactly kDecayRing and kReleaseRing in log2, so the knob at its top lands
	// on them; rounding from the exp2 is absorbed here rather than by an inequality that could miss by an ulp.
	if (params_.resolved(ParamId::KsDecay) >= std::log2(ks::kDecayRing) - 1.0e-9) {
		c.decaySeconds = ks::kDecayRing;
	}
	if (params_.resolved(ParamId::KsRelease) >= std::log2(ks::kReleaseRing) - 1.0e-9) {
		c.releaseSeconds = ks::kReleaseRing;
	}
	return c;
}

void Synth::startVoiceSources(const Voice& voice) {
	// A NOTE THAT STARTS SOUNDING PLUCKS THE ONE STRING (see pluckForNoteOn).
	pluckForNoteOn();
	// **WHY (backlog FF).** A per-voice cell is written at the tick (`tickEnvelopes`), so a voice handed a new note — a
	// steal, a mono retrigger, a re-struck note under a new id — went on reading the OLD note's value until the next
	// tick: up to 1.3 ms of the previous note's routed level under the new note's attack, a blip. The lane has already
	// taken the note (it runs before the voice manager), so the source answers the new note's start now — a fresh
	// envelope's rest, or under Leg the held envelope it continues — and the cell starts there, flat, from this sample.
	// A LEGATO MOVE IS NOT HERE: it has no attack to cover a jump, so its routed value ramps over the tick as before.
	// A DEVICE RUNNING INSIDE THE VOICE is handed the started value at the lane's next begin, which is the render call
	// the host's event cut starts right after this note (`applyParams`) — the second FF case holds that.
	if (voice.slot < 0) {
		return;
	}
	// EVERY CHAIN'S DEVICES — a modulator in the fx lane has per-voice outputs too (2026-10-04).
	forEachChainDevice([&](DeviceInstanceId id) {
		const device::Descriptor* desc = lane_.descriptorOfForTest(id);
		if (desc == nullptr) {
			return;
		}
		for (size_t output = 0; output < desc->outputCount(); ++output) {
			if (!desc->outputs[output].perVoice) {
				continue;
			}
			lane_.startOutputForVoice(id, output, static_cast<size_t>(voice.slot),
			                          lane_.outputForNoteOf(id, output, voice.noteId, voice.channel, voice.key));
			lane_.writeOutputPairForVoice(id, output, static_cast<size_t>(voice.slot), voice.noteId, voice.channel,
			                              voice.key, true);
		}
	});
}

bool Synth::tickEnvelopes() {
	const rack::StageScope stage(rack::Stage::Mods); // the spike recorder's (backlog SR): inert unless one is bound
	const bool gate = noteStackHeld();
	// CONSUMED HERE AND NOWHERE ELSE, and consumed even when the rack is empty — a latch left set
	// would fire the next time an envelope appeared, restarting it for a note struck seconds ago.
	const bool retrigger = envRetrigger_;
	envRetrigger_ = false;
	envGate_ = gate;

	const size_t count = std::min({ rack_.count, rack_.device.size(), playheads_.size() });

	// **EACH STRIP DEVICE'S VIEW OF THE SOURCES, BEFORE IT TICKS** — Feathers' per-node bus, filled
	// from what was published LAST tick, which is what makes the strip a synchronous update with a
	// one-tick delay between modulators (edges plan §19.1). The engine resolves each modulator's row
	// against it inside the tick; this used to be `modulatedRackRowAt`, a second resolve here whose
	// result a runner was fed. Only when something is routed at all — with no edge, nothing reads a bus.
	// **THE HOST NODE PUBLISHES, THEN THE PLAN IS BROUGHT UP TO DATE — and there is no bus to fill
	// (kill-lanes §2.4b).** This filled every strip device's bus with every source and masked off the
	// ones it must not hear; a node reads the cells its inputs point at, so a modulator ticking below
	// resolves against whatever its upstream published — this tick, in chain order, which is pass-through
	// (edges doc §20) rather than the one-tick delay the bus imposed.
	params_.publishCoreSources();
	// AND INTO THE LANE'S OWN CELLS, the one road a rack's macros arrive by (`LaneRack::setCoreSources`).
	lane_.setCoreSources(params_.coreSourceCells());
	compileRoutes();

	// THE NOTE LANE'S RECORD, BETWEEN THE SETTINGS AND THE TICK. See runNoteLane for why that is the
	// order. (The notes themselves reached the lane's note devices the instant they arrived.)
	runNoteLane();

	// **THE STRIP ADVANCES ONE CONTROL TICK ON THE ENGINE** — each modulator at its own declared rate,
	// with ONE gate, retrigger and clock for all of them: every modulator in the
	// rack is standing in the same world, so two reading different tempos would be a bug that only
	// appeared when a tempo automation crossed a block boundary.
	device::ModTickContext context {};
	context.gate = gate;
	context.retrigger = retrigger;
	context.clock = rackClock();
	// **WHICH NOTE EACH VOICE HOLDS, BEFORE THE TICK** (backlog MV, B): a per-voice source routed into a Poly modulator's
	// Trig fires that note's shape alone, and only the synth knows which voice plays which note. By the whole note, as
	// the per-voice writes below are; a voice not sounding a note holds none.
	for (const Voice& v : voices_) {
		if (v.slot < 0) {
			continue;
		}
		if (v.stage == Voice::Stage::Idle) {
			lane_.clearVoiceNote(static_cast<size_t>(v.slot));
		}
		else {
			lane_.setVoiceNote(static_cast<size_t>(v.slot), v.noteId, v.channel, v.key);
		}
	}
	lane_.tickModulators(context, static_cast<int>(PuckMotion::kTickSamples));

	// **AND WHAT THE ENGINE RESOLVED, FOR THE EDITOR** — the strip's modulated rows, read back rather
	// than computed beside it (see modulatedRack()). Copying the rack carries the tags and the instance
	// ids across; only the fields are modulation's. (It carried a route lane too until kill-lanes step
	// 3c, and that was the half most easily lost by rebuilding rows instead of copying them.)
	//
	// **COPIED ONLY WHEN WHAT IT COPIES CAN HAVE MOVED** (the idle-overhead pass, 2026-11-05): `rack_` (`rackRevision_`),
	// the lane's devices (`structureRevision_`, which every convergence that moves one bumps) and the row count. With all
	// three as they were, every byte of `modulated_` outside the fields rewritten below is already `rack_`'s — those fields
	// are each row's first `min(fieldsPresent, params)`, the same controls every tick for the same row and device — so the
	// copy is skipped and only the fields are read again. The rows are read whole from the engine (`modulatedRowAt`).
	if (modulatedRackAt_ != rackRevision_ || modulatedStructureAt_ != structureRevision_ || modulatedCountAt_ != count) {
		modulated_ = rack_;
		modulatedRackAt_ = rackRevision_;
		modulatedStructureAt_ = structureRevision_;
		modulatedCountAt_ = count;
		++modulatedCopies_;
	}
	modulated_.count = count;
	{
		const rack::RackEngine& engine = lane_.engineForTest();
		for (size_t slot = 0; slot < count; ++slot) {
			// TWO INDICES AGAIN — the view's row is at the strip SLOT, the engine is read at the slot IT
			// keeps the device in. Spelled `lane` before kill-lanes step 3c, which is the one word that
			// could not mean either of them.
			const size_t engineSlot = stripEngineSlot(slot);
			const device::Descriptor* desc = lane_.descriptorOfForTest(stripDevice(slot));
			if (desc == nullptr) {
				continue;
			}
			DeviceRow& row = modulated_.device[slot];
			const size_t controls = std::min(row.fieldsPresent, desc->params.size());
			engine.modulatedRowAt(engineSlot, row.field.data(), controls);
		}
	}

	for (size_t slot = 0; slot < count; ++slot) {
		// EVERY DEVICE PUBLISHES ON THE SAME OUTPUT ORDINAL — a modulator is a source of one signal
		// whatever shape it makes, so nothing downstream has to know which device produced it. That
		// is why kEnvelopeOutput is an ordinal rather than a device's name, and it is what lets this
		// loop be one line instead of two branches.
		// **THE OUTPUTS ARE THE NODE'S OWN NOW (kill-lanes §2.4b).** This copied every declared output into
		// `ParamSet`'s source table — and the lesson it carried stays true where the publish moved: EVERY
		// output the device declares, not just the first (six of the Note Expressions device's seven were
		// once left unpublished, and it was found by asking what the Rule knob was for). The engine
		// publishes all of them after the tick (`RackEngine::advanceModulators`), into the cells every
		// input reads; there is nothing left for the synth to copy.
		// AFTER THE TICK, so the position is where the device now IS rather than where it was when
		// the block began. The opposite choice is right for the automod's LFO — its taps are READ at
		// the phase published and then advanced — and it is wrong here: nothing downstream reads a
		// modulator at a phase, the value has already been taken, and a marker one tick behind the
		// number beside it would be the two disagreeing on screen.
		playheads_[slot] = playheadOf(slot);
	}

	// **AND THE PER-VOICE HALF — backlog DU's migration, the publish side.**
	//
	// **DEVICE-AGNOSTIC BY CONSTRUCTION.** This names no device: it asks each slot's DESCRIPTOR which
	// of its outputs vary per voice (`SourceDesc::perVoice`) and asks the RUNNER for each sounding
	// voice's note. `Synth` learning a device's tag is the pin this codebase has spent three arcs
	// pulling out of the modulation road, and putting one back here to reach one device would be
	// putting it back for every device after it.
	//
	// **THE WHOLE LOOP IS SKIPPED WHEN NOTHING VARIES**, which is every patch written before
	// 2026-09-10 — so "day one is bit-identical" is a property of the code rather than a claim about
	// it. `anyPerVoiceSource` is the branch, the same shape `anyRoute` has.
	// **INTO THE NODE'S PER-VOICE CELLS (kill-lanes §2.4b) — "voices within an edge".** Whether a source
	// varies per voice is DECLARED (`SourceDesc::perVoice`), not inferred from a write: an input built from
	// a declared-per-voice output points at the run of cells the engine keeps for it, and only those cells
	// are written here. The synth writes them because only the synth knows its voices and the note each
	// is holding; the engine, which owns the cells, knows neither.
	//
	// **AND WHICH RELEASING VOICES HAVE BEEN SUPERSEDED ON THEIR KEY.** A keyboard's notes all wear id -1, so a
	// per-voice source answers for the KEY, and once the key is struck again that is the new note: the old voice,
	// still releasing, would read the new note's envelope and — Voice Level above the floor — be held on it, two
	// voices for one key. A voice superseded so runs its own 2 ms edge (`heldByRoute`), which is what it did when
	// the hold was told from a falling value (the new note's attack read as a rise, not a tail).
	for (Voice& v : voices_) {
		v.superseded = false;
		if (v.stage != Voice::Stage::Release) {
			continue;
		}
		for (const Voice& other : voices_) {
			const bool sounding = other.stage == Voice::Stage::Attack || other.stage == Voice::Stage::Sustain;
			const bool sameNote = other.key == v.key && other.channel == v.channel
			                      && (other.noteId == -1 || v.noteId == -1 || other.noteId == v.noteId);
			if (&other != &v && sounding && sameNote) {
				v.superseded = true;
				break;
			}
		}
	}
	// **EVERY CHAIN'S DEVICES, BY ID** — the strip's and, since 2026-10-04, the fx lane's modulators (`forEachChainDevice`).
	forEachChainDevice([&](DeviceInstanceId id) {
		const device::Descriptor* desc = lane_.descriptorOfForTest(id); // the engine's
		if (desc == nullptr) {
			return;
		}
		for (size_t output = 0; output < desc->outputCount(); ++output) {
			if (!desc->outputs[output].perVoice) {
				continue;
			}
			// EVERY SOUNDING VOICE, BY THE NOTE IT IS PLAYING. A voice with no note contributes nothing and
			// is skipped rather than published as zero — zero is a real pressure.
			for (const Voice& v : voices_) {
				if (v.stage == Voice::Stage::Idle || v.slot < 0) {
					continue;
				}
				// **A STOLEN VOICE KEEPS THE PER-VOICE VALUES IT HAD** (backlog EH, shape 6). Its note was
				// choked at the lane the instant it was stolen, so the device now answers rest for it — and a
				// steal fade that multiplied a level already at rest was a fade of nothing: a click. The cell
				// simply stops being written, and the fade runs down from where the voice stood.
				if (v.stage == Voice::Stage::Stealing) {
					continue;
				}
				// BY THE WHOLE NOTE — id, channel, key — because a keyboard's notes all wear id -1 and
				// by id alone every voice read the first note the device held.
				lane_.writeOutputForVoice(id, output, static_cast<size_t>(v.slot),
				                          lane_.outputForNoteOf(id, output, v.noteId, v.channel, v.key));
				// AND ITS RIGHT EAR, while the output is a pair (backlog PA: the LFO's Stereo Phase); nothing otherwise.
				lane_.writeOutputPairForVoice(id, output, static_cast<size_t>(v.slot), v.noteId, v.channel, v.key, false);
			}
		}
	});
	// AND EVERY SLOT PAST THE COUNT IS CLEARED: a rack that has shrunk must stop reporting positions for
	// devices it no longer holds, or the editor keeps drawing a marker for a shape that is gone.
	for (size_t slot = count; slot < playheads_.size(); ++slot) {
		playheads_[slot] = {};
	}

	// THE FX LANE'S DRIVE PEAKS AND PLAYHEADS, for the meters and the markers — read once per tick, beside the
	// strip's. A position past the chain's length reads idle (`fxDevice` names nothing there).
	for (size_t slot = 0; slot < fxDrivePeaks_.size(); ++slot) {
		float peak = 0.0f;
		fxDrivePeaks_[slot] = lane_.drivePeakOf(fxDevice(slot), peak) ? peak : -1.0f;
	}
	// AND EVERY POSITION'S COURTESY OUTPUT METER (backlog MK) — the engine's held peak, negative where it meters nothing.
	for (size_t slot = 0; slot < fxOutputPeaks_.size(); ++slot) {
		float peak = 0.0f;
		fxOutputPeaks_[slot] = lane_.outputPeakOf(fxDevice(slot), peak) ? peak : -1.0f;
	}
	for (size_t position = 0; position < fxPlayheads_.size(); ++position) {
		fxPlayheads_[position] = fxPlayheadOf(position);
	}
	// (A DEPARTED DEVICE'S OUTPUTS WENT TO ZERO HERE TOO — every engine slot running no strip device had its
	// source cells cleared, so a stale reading could not stand. A node's outputs are its own now and the
	// engine zeroes them when the device leaves (`removeDeviceAtLane`); nothing here to clear.)

	// WHETHER ANY OF THIS CAN REACH ANYTHING, which is a question about COST rather than about sound.
	// The answer only changes when the routing table does, so it is computed there (ParamSet::
	// setRoutes) and only read here.
	return params_.anyDeviceSourceRouted();
}

void Synth::applyParams() {
	const rack::StageScope stage(rack::Stage::Params); // the spike recorder's (backlog SR): inert unless one is bound
	applyParamsRoad(true);
}

void Synth::applyParamsOnTick() {
	const rack::StageScope stage(rack::Stage::Params); // the spike recorder's (backlog SR): inert unless one is bound
	applyParamsRoad(false);
}

void Synth::applyParamsRoad(bool converge) {
	updateCharacter();
	syncVoiceManagerSettings();
	syncStack();
	applyPitches(!converge); // the tick road is the one that does not converge
	// The device is handed a TARGET, not a state: its own smoother walks to it on a fixed sample
	// grid, so a host that sends one parameter event per buffer and one that sends none render the
	// same samples. Set here rather than in the chunk loop because the params cannot move inside a
	// render call — the binding cuts the call at every event.
	//
	// POLY DECIDES WHICH OF TWO ARRANGEMENTS IS RUNNING, and never both. Off: one filter on the
	// summed bus, which is the cheap and usual case. On: one filter per voice, before the sum. The
	// settings are the same object either way — the same knobs mean the same things — and only the
	// `enabled` word differs, so the two can never be running at once and switching between them
	// cannot change anything else.
	//
	// THIS WAS KEYTRACK UNTIL 2026-08-23 AND THAT CONFLATED TWO CONTROLS. Keytrack is TONAL — does a voice's corner
	// follow its note. Poly is ARCHITECTURAL — how many filters there are. Reading keytrack here
	// shipped two of the four combinations, and the missing one is the interesting one: per-voice
	// WITHOUT keytracking, which on a NONLINEAR filter is a different sound rather than a tidier
	// arrangement. Saturation of a sum is not the sum of saturations.
	// READ FROM THE DEVICE'S OWN ROW, not from the parameter table. The swallowing rule:
	// docked effects' CLAP params are not on the surface of Feathers, they are swallowed
	// and reached by internal modulations. The filter's controls are the device's state now, so
	// the engine asks the chain what is in the slot rather than asking the host what a knob is set to.
	//
	// AND IT HAD TO BE THIS WAY BEFORE ANY SECOND EFFECT COULD EXIST. HARM has no clap ids and never
	// will, so its settings can only live in a row; an engine reading the filter from params and HARM
	// from a row would need to know which device is which to choose between them, which is the
	// compile-time slot branch reincarnated.
	//
	// AND THE ENGINE NO LONGER DECODES THE FILTER'S ROW AT ALL. It kept a decoded fx::filter::State
	// here for one reader — the per-voice road, which needed the keytrack flag and the corner to set
	// each voice's key offset by hand. That road takes a NoteContext now and every device folds the
	// note in with its own rule, so the last thing in this engine that knew what a filter IS has gone
	// with it. Both runners are handed rows and neither is told which device is in them.

	// THE WHOLE LANE, NOT ONE FILTER. This set a hardcoded MultimodeFilter's target until 2026-08-23,
	// which is why a chain holding anything else rendered nothing at all — HARM was inaudible not
	// because its DSP was missing but because the engine had no way to reach it.
	//
	// **THE LANE RESOLVES ITS OWN DEVICES NOW — THROUGH FEATHERS' ROUTES AND FEATHERS' BUS.** The lane
	// is handed the chain's BASE rows; the engine asks `ParamSet`'s published edges what reaches each
	// control (`LaneRack::setRouting`) and reads each source through the lane's own bus. This used to be
	// a second resolve here (`modulatedRowAt`), whose result was written into the runner as a base.
	//
	// **THE LANE IS SETTLED FOR THE NEXT SEGMENT, ON THE RACK ENGINE** — one engine, bus and per-voice
	// both, deciding each device's arrangement from its own RESOLVED row. With the note the BUS follows
	// and what time it is, from the SAME `rackClock()`
	// the modulator rack is fed at — one derivation of the tempo, read in one place. Each voice's own
	// note is delivered per chunk in applyVoiceFx, the cadence a note moves at; the settings move at
	// this one.
	// FROM THE SNAPSHOT AND ITS PUBLISH when the chains came from the bridge's record; from the values
	// when a caller handed them in (a case, a synth with no bridge), which have no publish to name.
	//
	// **IN TWO HALVES, WITH THE ROUTES BETWEEN THEM** (kill-lanes step 3a). The engine chooses the slot a
	// device lives in when it adopts it, so the plan `ParamSet` compiled at the top of the block (against
	// the slots the engine had then) does not know where a device that arrives HERE lives. When the
	// convergence moved anything, the staged route list is compiled again against the engine's new
	// layout BEFORE the segment begins — so a route to a device added this block is resolved on this
	// block, not the next one.
	// ON A TICK, THE SEGMENT ONLY ENDS: nothing reaches the chains between two ticks of one render (see `applyParamsOnTick`).
	bool moved = false;
	if (converge && laneSettled_) {
		// SETTLED BEFORE THIS BLOCK'S EVENTS (`settleLaneBeforeEvents`): the segment before ended and the devices converged
		// there, so neither happens twice — a second `finish` ends a segment that is not in flight.
		laneSettled_ = false;
	}
	else if (converge) {
		moved = chainsFromPublish_ ? lane_.converge(chainState_, chainsSequence_) : lane_.converge(fxChain_, rack_);
	}
	else {
		lane_.finish();
	}
	if (moved) {
		++structureRevision_;
	}
	// **THE PLAN, AFTER THE CONVERGENCE** (kill-lanes §2.4b) — so an input to or from a device that has
	// just arrived points at the cell it arrived with, in the engine slot it arrived in. The host node's
	// outputs first, so a macro routed anywhere reads this block's value.
	params_.publishCoreSources();
	// AND INTO THE LANE'S OWN CELLS, the one road a rack's macros arrive by (`LaneRack::setCoreSources`).
	lane_.setCoreSources(params_.coreSourceCells());
	compileRoutes();
	// Level's inputs gathered once, for the per-sample resolve in the voice loops. Here because
	// this runs at the top of every render and on every tick a device source is routed.
	params_.buildWindowPlan(ParamId::VoiceLevel, levelPlan_);
	params_.buildWindowPlan(ParamId::OscPhase, phasePlan_);
	// THE DRAWER'S TWO PATCH-WIDE ANSWERS, here because the routes move here: whether a per-voice source reaches any of its
	// continuous controls (else ONE drawer serves every voice), and whether a pair reaches one of its S controls (else no
	// ear is built twice). Only on Additive with the master switch on — off, the drawer is the identity in both ears.
	//
	// **SCANNED ONLY WHEN THE ROUTES WERE COMPILED AGAIN** (the idle-overhead pass, 2026-11-05). Which destinations vary per
	// voice, and which have any input at all, move only with the core ranges, which only `compileRoutes` rebuilds
	// (`routesCompiles_`). Whether a routed S control's input is a PAIR can move between compiles (`pairNow` reads its
	// source's live mask), so that half is asked again every time — of the routed S controls only, since an unrouted
	// one is never stereo-live. Same answers, each read the same way.
	drawerVaries_ = false;
	drawerPairLive_ = false;
	if (params_.oscType() == OscType::Additive && spectralState_.on) {
		if (drawerScanAt_ != routesCompiles_) {
			drawerScanAt_ = routesCompiles_;
			++drawerScans_;
			drawerVariesScanned_ = false;
			drawerRoutedPairs_ = 0;
			for (size_t slot = 0; slot < kMaxSounds; ++slot) {
				for (const SpectralControl c : kSpectralContinuousControls) {
					drawerVariesScanned_ = drawerVariesScanned_ || params_.variesPerVoice(spectralParam(slot, c));
				}
				for (const SpectralControl c : kSpectralStereoControls) {
					if (params_.isRouted(spectralParam(slot, c))) {
						drawerRoutedS_[drawerRoutedPairs_++] = spectralParam(slot, c);
					}
				}
			}
		}
		drawerVaries_ = drawerVariesScanned_;
		for (size_t i = 0; i < drawerRoutedPairs_; ++i) {
			drawerPairLive_ = drawerPairLive_ || params_.stereoLive(drawerRoutedS_[i]);
		}
	}
	lane_.begin(busNoteContext(), rackClock());
	// THE PER-VOICE ROAD'S LATENCY FOR THIS BLOCK, as beginBlock found it. A change clears every ring: the device that changed
	// latency restarted its own memory too, and the restart path follows (the reported latency moved).
	{
		const uint32_t delay = gainRingCap_ == 0 ? 0u : std::min<uint32_t>(lane_.perVoiceLatencySamples(), static_cast<uint32_t>(gainRingCap_ - 1));
		if (delay != gainDelay_) {
			std::fill(gainRing_.begin(), gainRing_.end(), 0.0f);
			gainWrite_.fill(0);
			// AND NO VOICE IS STILL DRAINING A TAIL THE CLEARED RINGS NO LONGER HOLD (a delay that fell to 0 mid-drain left a voice
			// in Release at level 0 for ever: the phase-2 review's Critical).
			for (Voice& v : voices_) {
				v.draining = false;
				v.drainLeft = 0;
			}
			gainDelay_ = delay;
		}
	}

	// **AND WHAT IT RESOLVED, FOR THE EDITOR** — `modulatedFxChain()` feeds the rings. Read back from the
	// engine rather than computed beside it: one resolve, and the picture is the number the device got.
	// KEYED THE WAY `device[]` IS, WHICH IS BY POSITION SINCE KILL-LANES STEP 3C. The reorder lesson
	// was a modulated row written at
	// one index while the editor read another — the pair was position-and-lane then, and the lane is
	// gone. What the lesson leaves is the discipline below: the two indices are still TWO.
	modulatedFx_ = fxChain_;
	const rack::RackEngine& engine = lane_.engineForTest();
	// **TWO INDICES, AND THEY ARE STILL DIFFERENT NUMBERS** (kill-lanes step 3a): the VIEW's row is at the
	// device's POSITION (the record's lane until 3b, the array's own subscript since 3c), and the ENGINE is
	// read at the slot it keeps the device in — the engine chooses that, so it tracks no chain's order. One
	// variable standing for both is the CS/DA family's shape exactly, and it remains available to write.
	for (size_t position = 0; position < fxChainLength(fxChain_); ++position) {
		const size_t slot = fxEngineSlot(position);
		const device::Descriptor* desc = lane_.descriptorOfForTest(fxDevice(position));
		if (position >= modulatedFx_.device.size() || desc == nullptr) {
			continue;
		}
		DeviceRow& row = modulatedFx_.device[position].row;
		const size_t controls = std::min(row.fieldsPresent, desc->params.size());
		for (size_t c = 0; c < controls; ++c) {
			row.field[c] = engine.modulatedFieldAt(slot, c);
		}
	}
}

void Synth::syncVoiceManagerSettings() {
	// VOICE COUNT UNDER THE STACK'S GUARD (backlog KE; by design, voices steal sooner): a stack of N sounds at most
	// ⌊B / N⌋ notes, so a deep stack steals through this very line — the manager's own road, with its fade.
	// EACH DRUM PAD'S VOICES, its own group's limit: one more hit steals the pad's own oldest.
	for (size_t pad = 0; pad < kMaxSounds; ++pad) {
		const int limit = kDrumVoicesCounts[steppedIndex(params_.resolved(drumParam(pad, DrumControl::Voices)),
		                                                 kDrumVoicesCounts.size())];
		if (limit != drum_->appliedLimit[pad]) {
			voiceManager_->setPolyphonyGroupVoiceLimit(kDrumPadGroup + pad, limit);
			drum_->appliedLimit[pad] = limit;
		}
	}
	const int count = params_.voiceLimit();
	if (count != appliedVoiceCount_) {
		// Lowering it steals the excess immediately rather than waiting for the next note-on, which
		// is the manager's own behaviour and the one a player expects from a polyphony knob.
		voiceManager_->setPolyphonyGroupVoiceLimit(kGroup, count);
		appliedVoiceCount_ = count;
	}

	const VoiceMode mode = params_.voiceMode();
	const NotePriority priority = params_.notePriority();
	if (haveAppliedModes_ && mode == appliedVoiceMode_ && priority == appliedNotePriority_) {
		return;
	}
	appliedVoiceMode_ = mode;
	appliedNotePriority_ = priority;
	haveAppliedModes_ = true;

	// Note Priority names the note that SURVIVES, and it has to be said to the manager twice, in two
	// vocabularies that are opposites of each other. Stealing is named for the voice that DIES, so
	// keeping the low notes means stealing the highest. Mono priority is named for the note that
	// wins, so it matches directly.
	using Manager = VoiceManager;
	switch (priority) {
	case NotePriority::Last:
		voiceManager_->setStealingPriorityMode(kGroup, Manager::StealingPriorityMode::OLDEST);
		voiceManager_->setMonoPriorityMode(kGroup, Manager::MonoPriorityMode::LATEST);
		break;
	case NotePriority::Low:
		voiceManager_->setStealingPriorityMode(kGroup, Manager::StealingPriorityMode::HIGHEST);
		voiceManager_->setMonoPriorityMode(kGroup, Manager::MonoPriorityMode::LOWEST);
		break;
	case NotePriority::High:
		voiceManager_->setStealingPriorityMode(kGroup, Manager::StealingPriorityMode::LOWEST);
		voiceManager_->setMonoPriorityMode(kGroup, Manager::MonoPriorityMode::HIGHEST);
		break;
	case NotePriority::kCount:
		break;
	}

	// Which note the single voice falls back to when a key is lifted while others are still held.
	// The same choice as above, said a third time — the manager keeps it in the play mode's feature
	// flags rather than beside the priority it belongs with.
	uint64_t releaseTo = static_cast<uint64_t>(Manager::MonoPlayModeFeatures::ON_RELEASE_TO_LATEST);
	if (priority == NotePriority::Low) {
		releaseTo = static_cast<uint64_t>(Manager::MonoPlayModeFeatures::ON_RELEASE_TO_LOWEST);
	}
	else if (priority == NotePriority::High) {
		releaseTo = static_cast<uint64_t>(Manager::MonoPlayModeFeatures::ON_RELEASE_TO_HIGHEST);
	}

	switch (mode) {
	case VoiceMode::Poly:
		voiceManager_->setPlaymode(kGroup, Manager::PlayMode::POLY_VOICES);
		break;
	case VoiceMode::Mono:
		// MONO_RETRIGGER: a new key ends the playing voice and starts a new one, so the amp edges run
		// again. The manager hands the new voice the key the old one was on, which is where its glide
		// starts from — see initializeMultipleVoices.
		voiceManager_->setPlaymode(kGroup, Manager::PlayMode::MONO_NOTES,
		                           static_cast<uint64_t>(Manager::MonoPlayModeFeatures::MONO_RETRIGGER)
		                               | releaseTo);
		break;
	case VoiceMode::Legato:
		// MONO_LEGATO: a new key MOVES the playing voice instead, which is exactly the design's rule
		// that an overlapping note does not retrigger the edges. The manager only retriggers when the
		// voice it would move is already releasing, which is the one case where not retriggering
		// would mean playing a note that is on its way out.
		voiceManager_->setPlaymode(kGroup, Manager::PlayMode::MONO_NOTES,
		                           static_cast<uint64_t>(Manager::MonoPlayModeFeatures::MONO_LEGATO)
		                               | releaseTo);
		break;
	case VoiceMode::kCount:
		break;
	}
}

double Synth::semitonesForKey(int16_t key, int16_t channel, double retuneSemitones,
                              int voice) const {
	const double bend = bendWheel_[static_cast<size_t>(channel) % bendWheel_.size()]
	                    * static_cast<double>(params_.bendRangeSemitones());
	return static_cast<double>(key) + retuneSemitones
	       + 12.0 * params_.resolved(ParamId::TransposeOct, voice)
	       + params_.resolved(ParamId::TransposeSemi, voice)
	       + params_.resolved(ParamId::Coarse, voice)
	       + params_.resolved(ParamId::FineTune, voice) / 100.0 + bend;
}

double Synth::targetSemitonesFor(const Voice& voice) const {
	// **THE PER-NOTE EXPRESSION IS ADDED HERE AND NOWHERE ELSE — backlog DS.** One place, so a
	// slide reaches the oscillator and `NoteContext::pitchSemitones` by the same arithmetic: this is
	// what `applyPitches` writes into `lastTargetSemitones`, and that is what `applyVoiceFx` hands
	// every per-voice device. Harm follows without harm knowing anything about expressions.
	//
	// NOT INSIDE `semitonesForKey`, which is also called with an explicit retune to compute where a
	// mono retrigger glides FROM — that is the previous key's resting pitch and the expression is not
	// part of it.
	// **THE VOICE'S OWN SLOT GOES THROUGH — this is where per-voice modulation becomes audible.** A
	// source declared `perVoice` resolves to THIS voice's value here; every other source resolves
	// exactly as it did. See backlog DU's migration table for why pitch went first.
	return semitonesForKey(voice.key, voice.channel, voice.retuneSemitones, voice.slot)
	       + voice.expressionSemitones;
}

double Synth::glideSecondsNow() const {
	return params_.voiceMode() == VoiceMode::Poly ? 0.0 : params_.glideTimeSeconds();
}

void Synth::applyPitches(bool onTick) {
	const rack::StageScope stage(rack::Stage::Params); // the spike recorder's (backlog SR): inert unless one is bound
	const double glide = glideSecondsNow();
	// WHAT THE STACK INDEX REACHES, read once for every voice (step 3): two relaxed walks over two short ranges.
	stackFineAmount_ = params_.stackIndexAmount(ParamId::FineTune);
	stackLevelAmount_ = params_.stackIndexAmount(ParamId::VoiceLevel);
	stackPhaseAmount_ = params_.stackIndexAmount(ParamId::OscPhase);
	stackPosXAmount_ = params_.stackIndexAmount(ParamId::PosX);
	stackPosYAmount_ = params_.stackIndexAmount(ParamId::PosY);
	for (Voice& v : voices_) {
		if (!v.isSounding()) {
			continue;
		}
		// A RE-SPACE WALKS ON THE CONTROL TICK, and only there, so a Count change lands on the same samples
		// whatever the host's block size (the fixed grid `PuckMotion` already keeps).
		if (onTick && v.respaceTicksLeft > 0) {
			advanceRespace(v);
		}
		// THE PARTS' STACK INDEX, before any pitch is decided from it — and Drift's step, on the tick.
		if (v.stacked) {
			updateStackIndex(v, onTick);
		}

		const double target = targetSemitonesFor(v);
		const bool wasTargeted = v.haveTarget;
		v.lastTargetSemitones = target;
		v.haveTarget = true;
		// A MOVED TUNING EXPRESSION RAMPS, ON ANY ROAD: a host's MPE slide or TB-Glide's bend writes it every few dozen
		// samples, and jumping to each value made a staircase (T10 in test_pattern_device).
		const bool tuningMoved = v.expressionSemitones != v.appliedExpressionSemitones;
		v.appliedExpressionSemitones = v.expressionSemitones;
		// EVERY PART OF THE STACK, EACH AT THE NOTE'S PITCH PLUS ITS PLACE IN THE FAN (backlog KE). Each part keeps
		// the road the voice always had, so part 0 of a stack of one — fan 0, the note exactly — is the voice's
		// oscillator doing precisely what it did. A glide carries every part the same way: the offset is a
		// constant in semitones and both roads below move in semitones.
		for (int part = 0; part < kMaxStack; ++part) {
			Voice::StackPart& p = v.parts[static_cast<size_t>(part)];
			if (!p.live) {
				continue;
			}
			OscVoice& osc = partOsc(v, part);
			osc.setGlideSeconds(glide);
			const double partTarget = partTargetFor(v, target, part);
			// PART 0 BAKES THE STACK'S VOWEL AT THE NOTE, not at its own place at the bottom of the fan (§3.10: the
			// bake is keyed on the voice's note), so the parts reading it sit at most half the fan away. Zero for a
			// stack of one, whose part 0 is the note.
			if (part == 0) {
				osc.setVoxBakeOffset(partTarget - target);
			}
			// Only on a change. Re-asserting an unchanged pitch would be harmless but costs a conversion
			// per voice per slice, and the common case is that nothing has moved. A voice mid-glide is
			// unaffected either way: the target it is moving towards is what is being compared.
			const bool partWasTargeted = wasTargeted && p.haveTarget;
			if (partWasTargeted && partTarget == p.lastTargetSemitones) {
				continue;
			}
			p.lastTargetSemitones = partTarget;
			p.haveTarget = true;
			// **ON THE CONTROL TICK, A VOICE THAT DOES NOT GLIDE REACHES A MOVED TARGET BY A STRAIGHT LINE OVER THE
			// TICK** (backlog FK): the target moved because a routed source did, and a jump once a tick is a pitch
			// staircase — an LFO into Fine Tune stepped audibly. The line is the tick window's shape. Only for a
			// voice that already had a target (a note starts where it starts) and only on the tick (a knob or a
			// bend lands at the top of a render). A re-spacing fan takes the same line, which is what makes its
			// walk a glide rather than a staircase of fifteen small jumps.
			if ((onTick || tuningMoved) && glide <= 0.0 && partWasTargeted) {
				osc.rampToPitch(partTarget, PuckMotion::kTickSamples);
				continue;
			}
			// glideToPitch is a jump when the glide time is zero, so this is one call rather than a
			// branch — and the branch would be the place the two could disagree.
			osc.glideToPitch(partTarget);
		}
	}
}

//-----------------//
// Voice stacking  //
//-----------------//

OscVoice& Synth::partOsc(Voice& voice, int part) {
	return part == 0 ? voice.osc : (*partOscs_)[static_cast<size_t>(voice.slot)][static_cast<size_t>(part - 1)];
}

const OscVoice& Synth::partOsc(const Voice& voice, int part) const {
	return part == 0 ? voice.osc : (*partOscs_)[static_cast<size_t>(voice.slot)][static_cast<size_t>(part - 1)];
}

double Synth::partTargetFor(const Voice& voice, double target, int part) const {
	const Voice::StackPart& p = voice.parts[static_cast<size_t>(part)];
	// A STACK INDEX ROUTE INTO FINE TUNE (step 3): this part's own Fine Tune is the voice's plus amount × index ×
	// span, held to the parameter's range as the voice's own resolve is, and the difference is what moves it.
	double indexed = 0.0;
	if (stackFineAmount_ != 0.0 && p.index != 0.0f) {
		const double fine = params_.resolved(ParamId::FineTune, voice.slot);
		const double own = std::clamp(fine + stackFineAmount_ * static_cast<double>(p.index) * 2.0 * kFineTuneMaxCents,
		                              -kFineTuneMaxCents, kFineTuneMaxCents);
		indexed = (own - fine) / 100.0;
	}
	const double fan = p.fan;
	if (fan == 0.0) {
		// THE NOTE ITSELF, and not `target + 0.0`, which is the same number but says so less plainly: part 0 of
		// a stack of one, and the middle part of an odd stack, are on the note exactly.
		return indexed == 0.0 ? target : target + indexed;
	}
	// The detune is resolved FOR THIS VOICE, so a per-voice source (an envelope) can open one note's fan.
	const double detuned = target + params_.resolved(ParamId::StackDetune, voice.slot) * fan / 100.0;
	return indexed == 0.0 ? detuned : detuned + indexed;
}

void Synth::seatStack(Voice& voice, int count) {
	const float gain = stackGain(count);
	for (int part = 0; part < kMaxStack; ++part) {
		Voice::StackPart& p = voice.parts[static_cast<size_t>(part)];
		p = {};
		// A NEW NOTE STARTS ON THE PUCK (§3.11), every part, whatever stand the slot's last note left it on; the next tick
		// seats each part on its own. Part 0 lends the vowel meanwhile.
		OscVoice& seated = partOsc(voice, part);
		seated.moveToBank(&bank_);
		seated.shareVoxFrom(part == 0 ? nullptr : &voice.osc);
		if (part < count) {
			p.fan = p.fanFrom = p.fanTo = fanPosition(part, count);
			p.gain = p.gainTo = gain;
			p.live = true;
		}
	}
	voice.stackCount = count;
	voice.respaceTicksLeft = 0;
	voice.stacked = count > 1;
	// AND EVERY PART AT ITS PLACE IN THE STEREO FIELD FROM THE FIRST SAMPLE: a new note is not a Spread move, so
	// nothing slews in from the centre (step 2).
	if (voice.stacked) {
		aimStackPans(voice);
		for (Voice::StackPart& p : voice.parts) {
			p.pan = p.panTo;
			// AND AT ITS SHARE OF A LEVEL ROUTE FROM THE FIRST SAMPLE (step 3): no walk in from unity.
			p.levelScale = kLevelScaleUnseated;
		}
		updateStackIndex(voice, false);
	}
}

void Synth::updateStackIndex(Voice& voice, bool onTick) {
	// RESOLVED FOR THIS VOICE (Shape is per note): the amounts, Free's phase and the Curve are per-voice
	// destinations, so an envelope can move one note's index while another's stands.
	const int slot = voice.slot;
	StackIndexSettings s;
	s.count = voice.stackCount;
	s.shape = params_.stackShape();
	s.shapeAmount = static_cast<float>(params_.resolved(ParamId::StackShapeAmount, slot));
	s.freePhase = params_.resolved(ParamId::StackFreePhase, slot);
	s.map = params_.stackMap();
	s.mapAmount = static_cast<float>(params_.resolved(ParamId::StackMapAmount, slot));
	s.curve = static_cast<float>(params_.resolved(ParamId::StackCurve, slot));
	s.freshSeed = voice.freshSeed;
	const StackValues values = stackIndexSpread(s);
	const bool drifting = s.shape == StackShape::Drift;
	if (drifting && onTick) {
		StackValues offsets {};
		for (int part = 0; part < kMaxStack; ++part) {
			offsets[static_cast<size_t>(part)] = voice.parts[static_cast<size_t>(part)].drift;
		}
		// The walk's rate is the Shape Amount (the firmware's meta), stepped once per control tick — the SONG's ticks
		// while it plays (Q9), so the walk at a given song tick is the same on every pass: as many steps as the song
		// has ticked since the last, usually one. A step backwards or a leap (a loop, a seek, mid-note) re-anchors on
		// one step. With no song, one step per engine tick, as before.
		int steps = 1;
		const int64_t songTick = songTickNow();
		if (songTick >= 0 && voice.driftSongTick >= 0) {
			const int64_t ahead = songTick - voice.driftSongTick;
			steps = ahead >= 0 && ahead <= 4 ? static_cast<int>(ahead) : 1;
		}
		voice.driftSongTick = songTick;
		for (int step = 0; step < steps; ++step) {
			stackDriftStep(s.shapeAmount, s.count, PuckMotion::kTickSamples, voice.driftRng, offsets);
		}
		for (int part = 0; part < kMaxStack; ++part) {
			voice.parts[static_cast<size_t>(part)].drift = offsets[static_cast<size_t>(part)];
		}
	}
	for (int part = 0; part < s.count && part < kMaxStack; ++part) {
		Voice::StackPart& p = voice.parts[static_cast<size_t>(part)];
		// DRIFT WALKS AROUND ITS BASE and never forgets it (the fix): the mapped, curved fan plus this part's walk.
		const float walked = drifting ? p.drift : 0.0f;
		p.index = std::clamp(values[static_cast<size_t>(part)] + walked, -1.0f, 1.0f);
	}
}

int Synth::standsUntilBuild() const {
	int until = std::numeric_limits<int>::max();
	for (int s = 0; s < kStandPool; ++s) {
		if (standKeyed_[static_cast<size_t>(s)]) {
			until = std::min(until, (*standBanks_)[static_cast<size_t>(s)].samplesUntilPendingBuild());
		}
	}
	return until;
}

const TableSource& Synth::bankForStand(int stand) const {
	if (stand < 0) {
		return bank_;
	}
	return (*standBanks_)[static_cast<size_t>(stand)];
}

bool Synth::assignStands(bool widthStands) {
	standUsed_.fill(false);
	standOverflow_ = 0;
	standFresh_.fill(false);
	// A PART'S OFFSET FROM THE PUCK: amount × its index × Pos's span (2), per axis. Nothing routed is every part on
	// the puck, which is today's one bank and costs one comparison here.
	// AND, WHILE TERRAIN STEREO IS LIVE (step 4c, §6 A), AN EAR SIDE: the part's place in the fan doubled, −1 at the
	// lowest part and +1 at the highest, displacing each sound's anchor as an ear does. Then EVERY part of a stacked
	// voice stands on a slot — the centre part too, at side 0 (the head), since the main bank's ear 0 is the LEFT ear.
	const bool routed = stackPosXAmount_ != 0.0 || stackPosYAmount_ != 0.0 || widthStands;
	std::array<std::array<bool, kMaxStack>, kVoiceSlots> pending {};
	std::array<std::array<StandKey, kMaxStack>, kVoiceSlots> want {};
	bool anyOff = false;
	for (Voice& v : voices_) {
		const auto vs = static_cast<size_t>(v.slot);
		for (int part = 0; part < kMaxStack; ++part) {
			Voice::StackPart& p = v.parts[static_cast<size_t>(part)];
			const auto ps = static_cast<size_t>(part);
			if (!v.isSounding() || !v.stacked || !p.live || !routed) {
				p.stand = -1;
				continue;
			}
			const StandKey o { stackPosXAmount_ * static_cast<double>(p.index) * 2.0,
				               stackPosYAmount_ * static_cast<double>(p.index) * 2.0,
				               widthStands ? 2.0 * p.fan : 0.0 };
			if (!widthStands && o[0] == 0.0 && o[1] == 0.0) {
				p.stand = -1; // the centre part of an odd stack, or a part at index 0: the puck itself
				continue;
			}
			anyOff = true;
			want[vs][ps] = o;
			// PASS ONE: A PART WHOSE SLOT STILL STANDS WHERE IT WANTS TO KEEPS IT, so a steady stack never moves.
			const int s = p.stand;
			if (s >= 0 && standKeyed_[static_cast<size_t>(s)] && standOffset_[static_cast<size_t>(s)] == o) {
				standUsed_[static_cast<size_t>(s)] = true;
			}
			else {
				pending[vs][ps] = true;
			}
		}
	}
	if (!anyOff) {
		return false;
	}
	// PASS TWO: A SLOT ALREADY STANDING THERE — another part's, another voice's — is shared.
	// PASS THREE: the part's own slot, if nobody kept it, moves with it (a sliding stand re-keys rather than hops);
	// else any slot nobody is using this tick, keyed or not; else the part stays on the puck (the pool is full).
	for (int pass = 2; pass <= 3; ++pass) {
		for (Voice& v : voices_) {
			const auto vs = static_cast<size_t>(v.slot);
			for (int part = 0; part < kMaxStack; ++part) {
				const auto ps = static_cast<size_t>(part);
				if (!pending[vs][ps]) {
					continue;
				}
				Voice::StackPart& p = v.parts[ps];
				const StandKey& o = want[vs][ps];
				int found = -1;
				for (int s = 0; s < kStandPool && found < 0; ++s) {
					if (standKeyed_[static_cast<size_t>(s)] && standOffset_[static_cast<size_t>(s)] == o
					    && (pass == 2 ? standUsed_[static_cast<size_t>(s)] : true)) {
						found = s;
					}
				}
				if (found < 0 && pass == 3) {
					if (p.stand >= 0 && !standUsed_[static_cast<size_t>(p.stand)]) {
						found = p.stand;
					}
					for (int s = 0; s < kStandPool && found < 0; ++s) {
						if (!standUsed_[static_cast<size_t>(s)]) {
							found = s;
						}
					}
				}
				if (found < 0) {
					if (pass == 3) {
						p.stand = -1;
						pending[vs][ps] = false;
						++standOverflow_;
					}
					continue;
				}
				const auto fs = static_cast<size_t>(found);
				standFresh_[fs] = standFresh_[fs] || !standKeyed_[fs] || standOffset_[fs] != o;
				standKeyed_[fs] = true;
				standOffset_[static_cast<size_t>(found)] = o;
				standUsed_[static_cast<size_t>(found)] = true;
				p.stand = found;
				pending[vs][ps] = false;
			}
		}
	}
	return true;
}

void Synth::seatPartsOnStands(Voice& voice) {
	for (int part = 0; part < kMaxStack; ++part) {
		Voice::StackPart& p = voice.parts[static_cast<size_t>(part)];
		if (!p.live) {
			continue;
		}
		OscVoice& osc = partOsc(voice, part);
		// ONTO ITS STAND ONLY ONCE THE STAND IS BUILT FOR ITS KEY (step 4c): until then it stays where it is — on the puck,
		// for a new note — rather than playing a table built for somewhere else.
		if (p.stand < 0 || standReady_[static_cast<size_t>(p.stand)]) {
			osc.moveToBank(&bankForStand(p.stand));
		}
		// THE VOWEL COMES FROM THE FIRST PART OF THIS VOICE READING THE SAME BANK (§3.10: parts with equal inputs share one
		// bake). It renders first, so its bake is current when this part reads it; the first on its stand bakes.
		const OscVoice* lender = nullptr;
		for (int earlier = 0; earlier < part && lender == nullptr; ++earlier) {
			const Voice::StackPart& e = voice.parts[static_cast<size_t>(earlier)];
			if (e.live && partOsc(voice, earlier).bank() == osc.bank()) {
				lender = &partOsc(voice, earlier);
			}
		}
		osc.shareVoxFrom(lender);
	}
}

void Synth::updateStackCoherence(Voice& voice, bool all) {
	// **HOW ALIKE THE PARTS ARE AT 0 CENTS**. The parts are summed at
	// 1/√N each; how loud that sum is depends on how alike their cycles are, and parts on stands of their own (a Pos
	// route, Width) play different tables. MEASURED, V5: at 0 cents they summed with a coherence of −0.25 to 0.75, never
	// the 1 the trim had assumed, and this reading of their tables predicted all 45 measured cells within 0.13.
	//
	// EVERY PART ON ONE BANK IS ONE CHARACTER, and the answer is 1 exactly — a plain stack, whose trim is then the one it
	// always was, to the bit. Parts on one bank may still read different mips; those are band limits of one sound, not
	// two sounds, and their vowel is one shared bake.
	const int count = std::min(voice.stackCount, kMaxStack);
	Voice::CoherenceKey key;
	int sounding = 0;
	const void* firstBank = nullptr;
	bool oneBank = true;
	for (int part = 0; part < count; ++part) {
		const OscVoice& osc = partOsc(voice, part);
		const PreparedCharacter* table = osc.playingTable();
		if (!voice.parts[static_cast<size_t>(part)].live || table == nullptr) {
			continue; // not sounding, or not synced yet: nothing is heard from it
		}
		const auto k = static_cast<size_t>(part);
		key.bank[k] = osc.bank();
		key.serial[k] = table->buildSerial;
		key.grid[k] = osc.playingGrid();
		// ITS VOWEL MAY BE AN EARLIER PART'S BAKE (`seatPartsOnStands`: the first part on the same bank lends it), whose
		// rebake changes this part's cycle without touching anything of this part's own.
		key.bakes[k] = osc.voxBakeCount();
		for (int earlier = 0; earlier < part; ++earlier) {
			if (key.bank[static_cast<size_t>(earlier)] == key.bank[k]) {
				key.bakes[k] += partOsc(voice, earlier).voxBakeCount();
				break;
			}
		}
		firstBank = firstBank == nullptr ? key.bank[k] : firstBank;
		oneBank = oneBank && key.bank[k] == firstBank;
		++sounding;
	}
	if (sounding < 2 || oneBank) {
		voice.stackCoherence = 1.0f;
		voice.coherenceKey = {};
		return;
	}
	if (key == voice.coherenceKey) {
		return; // nothing any part plays has moved since the last reading
	}
	// A PART'S CYCLE IS RE-READ ONLY WHEN ITS OWN KEY MOVED, AND AT MOST ONE PER CALL — the render calls this once per
	// voice per chunk, and eight reads at once cost ~250 µs of one chunk. Moved parts are taken in turn from the cursor,
	// so the reading converges within N chunks; the trim glides through the drive smoothing, so the lag is not heard.
	// `voice.coherenceKey` holds, per part, the key of the cycle stored for it — not the key it plays now.
	CoherenceSlot& store = (*coherenceCycles_)[static_cast<size_t>(voice.slot)];
	Voice::CoherenceKey& was = voice.coherenceKey;
	const auto moved = [&](size_t k) {
		return key.bank[k] != was.bank[k] || key.serial[k] != was.serial[k] || key.grid[k] != was.grid[k]
		       || key.bakes[k] != was.bakes[k];
	};
	for (size_t k = 0; k < static_cast<size_t>(kMaxStack); ++k) {
		if (key.bank[k] == nullptr && moved(k)) { // fell silent: dropped from the sums, no read
			was.bank[k] = nullptr;
			was.serial[k] = 0;
			was.grid[k] = nullptr;
			was.bakes[k] = 0;
		}
	}
	const int from = store.cursor;
	for (int step = 0; step < count; ++step) {
		const int part = (from + step) % count;
		const auto k = static_cast<size_t>(part);
		if (key.bank[k] == nullptr || !moved(k)) {
			continue;
		}
		const OscVoice& osc = partOsc(voice, part);
		++coherenceCycleReads_;
		auto& wa = store.cycles[k];
		// ONE PASS (OscVoice::playingCycle): a table side is the same 256 reads as before, to the bit; a Partials side is its
		// spectrum summed as one cycle, which no table read could give (plan Task 9b).
		osc.playingCycle(wa.data(), kCoherencePoints);
		static_assert(2 * OscVoice::kPartialsCycleCap <= kCoherencePoints,
		              "a Partials part's playing cycle must be resolved by the reading it is compared in");
		was.bank[k] = key.bank[k];
		was.serial[k] = key.serial[k];
		was.grid[k] = key.grid[k];
		was.bakes[k] = key.bakes[k];
		// ITS ROW of c_ij = the cycle sum of w_i · w_j, against every part stored now (itself included).
		for (size_t b = 0; b < static_cast<size_t>(kMaxStack); ++b) {
			if (was.bank[b] == nullptr) {
				continue;
			}
			const auto& wb = store.cycles[b];
			double c = 0.0;
			for (int i = 0; i < kCoherencePoints; ++i) {
				c += static_cast<double>(wa[static_cast<size_t>(i)]) * static_cast<double>(wb[static_cast<size_t>(i)]);
			}
			store.sums[k][b] = c;
			store.sums[b][k] = c;
		}
		store.cursor = (part + 1) % count;
		if (!all) {
			break;
		}
	}
	// ρ0 = Σ_{i≠j} c_ij / ((N − 1) Σ_i c_ii), from the stored sums — the POWER-WEIGHTED form, which is exactly what
	// brings 1/√(1 + (N − 1) ρ0) to the parts' power average when their stands differ in loudness. Until two parts are
	// stored there is no likeness to read, and the last reading stands.
	double diag = 0.0;
	double off = 0.0;
	int stored = 0;
	for (size_t a = 0; a < static_cast<size_t>(kMaxStack); ++a) {
		if (was.bank[a] == nullptr) {
			continue;
		}
		++stored;
		diag += store.sums[a][a];
		for (size_t b = a + 1; b < static_cast<size_t>(kMaxStack); ++b) {
			if (was.bank[b] != nullptr) {
				off += 2.0 * store.sums[a][b];
			}
		}
	}
	if (stored >= 2) {
		voice.stackCoherence = diag > 0.0 ? static_cast<float>(off / (static_cast<double>(stored - 1) * diag)) : 1.0f;
	}
}

spectral::ShaperParams Synth::drawerFor(int voice, int stand, bool rightEar) const {
	spectral::ShaperParams p;
	p.enabled = spectralState_.on;
	if (!p.enabled) {
		return p; // the master switch: the engine's own road, every sound's settings kept
	}
	const SoundWeights& weights = stand >= 0 ? standWeights_[static_cast<size_t>(stand)] : earWeights_[rightEar ? 1 : 0];
	// THE SOUNDS IN THE BLEND: the live ones with a weight, and their total, which a partial operator's mix is divided by.
	// An operator on in EVERY sound is given a mix of the literal 1.0 (mixOf), not this ratio, so it is D1's road to the bit.
	std::array<size_t, kMaxSounds> live {};
	size_t count = 0;
	double total = 0.0;
	for (size_t s = 0; s < kMaxSounds; ++s) {
		if (weights.weight[s] > 0.0) {
			live[count++] = s;
			total += weights.weight[s];
		}
	}
	if (count == 0) {
		return p; // nothing sounds: the identity
	}
	// ONE SOUND'S CONTROL, as this voice and ear hear it: per voice only where a per-voice source reaches it (the VoiceLevel
	// pattern); the right ear the left plus what the pairs add beyond it (resolvedRight − resolved: the pairs are block-rate,
	// never per voice), clamped as the left ear is.
	const auto value = [&](size_t s, SpectralControl c) {
		const ParamId id = spectralParam(s, c);
		double v = voice >= 0 && params_.variesPerVoice(id) ? params_.resolved(id, voice) : params_.resolved(id);
		if (rightEar && params_.stereoLive(id)) {
			const ParamDesc* desc = findParam(static_cast<clap_id>(id));
			v = std::clamp(v + params_.resolvedRight(id) - params_.resolved(id), desc->min, desc->max);
		}
		return v;
	};
	// The switches, Mode and Link are stepped and never destinations: one reading for every voice and both ears.
	const auto isOn = [&](size_t s, SpectralControl c) {
		return steppedIndex(params_.resolved(spectralParam(s, c)), kEnableNames.size()) != 0;
	};
	// AN OPERATOR'S BLEND over the sounds `in` picks: its weight W and the weighted mean of a control over them (a lone
	// contributor's value exactly, so one sound's drawer is that sound's drawer to the bit), and its MIX, W over the total —
	// returned as the LITERAL 1.0 when every sound in the blend contributes (no division is trusted to land on 1).
	struct Group {
		std::array<size_t, kMaxSounds> slot {};
		size_t count = 0;
		double weight = 0.0;
	};
	const auto mean = [&](const Group& g, SpectralControl c) {
		if (g.count == 1) {
			return value(g.slot[0], c);
		}
		double sum = 0.0;
		for (size_t i = 0; i < g.count; ++i) {
			sum += weights.weight[g.slot[i]] * value(g.slot[i], c);
		}
		return sum / g.weight;
	};
	const auto mixOf = [&](const Group& g) { return g.count == count ? 1.0 : g.weight / total; };
	const auto gather = [&](auto&& in) {
		Group g;
		for (size_t i = 0; i < count; ++i) {
			const size_t s = live[i];
			if (in(s)) {
				g.slot[g.count++] = s;
				g.weight += weights.weight[s];
			}
		}
		return g;
	};

	// Every Mode must have a filter to land in, and spectralControlInert names Notch and Bell by their indices.
	static_assert(kSpectralFilterModeNames.size() <= 1 + std::tuple_size_v<decltype(spectral::ShaperParams::moreFilters)>);
	static_assert(kSpectralFilterModeNames.size() <= 1 + std::tuple_size_v<decltype(spectral::ShaperParams::moreFilters2)>);
	static_assert(static_cast<size_t>(spectral::FilterMode::Notch) == 3);
	static_assert(static_cast<size_t>(spectral::FilterMode::Bell) == 4);
	// THE ROUTING (F2, additive design §3.6): the share of the blend set to Parallel, over the sounds where it is heard — both
	// filters on (spectralRoutingInert: with one filter the routing changes nothing, so it may not move the blend). Blended in dB by
	// the shaper (Series^(1−r)·Parallel^r), so nothing steps between sounds that disagree. Every such sound in Series: exactly 0.
	{
		const Group both = gather([&](size_t s) {
			return !spectralRoutingInert(isOn(s, SpectralControl::FilterOn), isOn(s, SpectralControl::Filter2On));
		});
		const Group parallel = gather([&](size_t s) {
			return !spectralRoutingInert(isOn(s, SpectralControl::FilterOn), isOn(s, SpectralControl::Filter2On))
			       && isOn(s, SpectralControl::FilterRouting);
		});
		if (parallel.count > 0) {
			p.parallel = parallel.count == both.count ? 1.0f : static_cast<float>(parallel.weight / both.weight);
		}
	}
	// THE FILTERS, EACH GROUPED BY MODE (lead's call, 2026-10-10, revisable): Mode cannot be averaged, so each mode present is a
	// filter of its own, at its group's mean settings and its group's mix, and the gains multiply — a 50/50 LP/HP is both
	// at half depth and nothing steps. The first group present is the side's primary; the others its further modes. Filter 2 is
	// the same, over its own controls (spectralFilterControl), into `filter2` and `moreFilters2`; each side's PRESENCE is the share
	// of the blend whose filter is on (the literal 1.0 when every sound's is), which Parallel reads.
	const auto filterSide = [&](int side) {
		using C = SpectralControl;
		const auto ctl = [side](C c) { return spectralFilterControl(side, c); };
		const Group on = gather([&](size_t s) { return isOn(s, ctl(C::FilterOn)); });
		if (on.count > 0) {
			p.filterPresence[static_cast<size_t>(side)] = static_cast<float>(mixOf(on));
		}
		size_t placed = 0;
		for (size_t m = 0; m < kSpectralFilterModeNames.size(); ++m) {
			const Group g = gather([&](size_t s) {
				return isOn(s, ctl(C::FilterOn))
				       && steppedIndex(params_.resolved(spectralParam(s, ctl(C::FilterMode))), kSpectralFilterModeNames.size()) == m;
			});
			if (g.count == 0) {
				continue;
			}
			// **A SOUND AT ITS FILTER'S OPEN END HAS NO FILTER (Z7d)** — `spectral::filterOpenness` by its control, the rule the
			// panel greys its Resonance, Slope and Key Track by (spectralControlInert's `filterOpen`); a Bell's is its Gain at 0 dB
			// (F2). So a sound's share of the group is its weight times (1 - openness): at the end it moves nothing (a greyed knob
			// may never move the sound), and on the way there it fades out of the blend. The mix is that share of the total, over
			// the openness `prepare` will apply to the blended op again, so one sound's drawer fades exactly as the device's
			// filter does. Away from the ends every openness is 0 and this is the old blend, to the bit.
			const auto mode = static_cast<spectral::FilterMode>(m);
			std::array<double, kMaxSounds> share {};
			double shared = 0.0;
			bool opening = false;
			for (size_t i = 0; i < g.count; ++i) {
				const double cutoff = value(g.slot[i], ctl(C::FilterCutoff));
				const double open = spectral::filterOpenness(mode, cutoff, cutoff, value(g.slot[i], ctl(C::FilterGain)));
				opening = opening || open > 0.0;
				share[i] = weights.weight[g.slot[i]] * (1.0 - open);
				shared += share[i];
			}
			const bool allOpen = opening && !(shared > 0.0);
			if (allOpen && !(p.parallel > 0.0f)) {
				continue; // every contributor at its open end: no filter at all (in Series; in Parallel a wire, below)
			}
			const auto blend = [&](C c) {
				if (!opening) {
					return mean(g, c);
				}
				if (g.count == 1 || allOpen) {
					return value(g.slot[0], c);
				}
				double sum = 0.0;
				for (size_t i = 0; i < g.count; ++i) {
					sum += share[i] * value(g.slot[i], c);
				}
				return sum / shared;
			};
			spectral::FilterOp& f = side == 0 ? (placed == 0 ? p.filter : p.moreFilters[placed - 1])
			                                  : (placed == 0 ? p.filter2 : p.moreFilters2[placed - 1]);
			f.on = true;
			f.mode = mode;
			const double log2Cutoff = blend(ctl(C::FilterCutoff));
			f.cutoffHz = static_cast<float>(std::exp2(log2Cutoff)); // a log-frequency blend
			f.resonance = static_cast<float>(blend(ctl(C::FilterResonance)));
			f.slope = static_cast<float>(blend(ctl(C::FilterSlope)));
			f.keyTrack = static_cast<float>(blend(ctl(C::FilterKeyTrack)));
			if (mode == spectral::FilterMode::Bell) {
				f.gainDb = static_cast<float>(blend(ctl(C::FilterGain))); // the mean in dB; the mix fades it in dB again
			}
			if (allOpen) {
				// IN PARALLEL, EVERY CONTRIBUTOR AT ITS OPEN END IS A WIRE BESIDE THE OTHER FILTER: a filter of exponent 0 (its gain
				// exactly 1), present for the routing — the device's road, where an open filter is a wire too (prepare).
				f.mix = 0.0f;
			}
			else if (!opening) {
				f.mix = static_cast<float>(mixOf(g));
			}
			else {
				const double again =
				    1.0 - spectral::filterOpenness(mode, log2Cutoff, log2Cutoff, static_cast<double>(f.gainDb));
				f.mix = static_cast<float>(again > 0.0 ? std::min(1.0, (shared / total) / again) : 1.0);
			}
			++placed;
		}
	};
	filterSide(0);
	filterSide(1);
	// THE OTHERS FADE THROUGH THEIR OWN DEPTH: the mean over the sounds where the operator is on, times its mix.
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::CombOn); });
		if (g.count > 0) {
			p.comb.on = true;
			p.comb.spacing = static_cast<float>(mean(g, SpectralControl::CombSpacing));
			p.comb.phase = static_cast<float>(mean(g, SpectralControl::CombPhase));
			p.comb.depth = static_cast<float>(mean(g, SpectralControl::CombDepth) * mixOf(g));
		}
	}
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::FormantShiftOn); });
		if (g.count > 0) {
			// A LINKED SOUND SHIFTS ITS WHOLE ENVELOPE BY ITS LOW SHIFT, so it contributes its low shift as its high one too;
			// the blend is linked only when every contributing sound is, and nothing steps between the two readings.
			bool allLinked = true;
			double high = 0.0;
			for (size_t i = 0; i < g.count; ++i) {
				const size_t s = g.slot[i];
				const bool linked = isOn(s, SpectralControl::FormantShiftLink);
				const bool highInert = spectralControlInert(SpectralControl::FormantShiftHigh, 0, linked);
				allLinked = allLinked && linked;
				const double h = highInert ? value(s, SpectralControl::FormantShiftLow) : value(s, SpectralControl::FormantShiftHigh);
				high += (g.count == 1 ? 1.0 : weights.weight[s] / g.weight) * h;
			}
			const double mix = mixOf(g);
			p.formantShift.on = true;
			p.formantShift.link = allLinked;
			p.formantShift.lowSemitones = static_cast<float>(mean(g, SpectralControl::FormantShiftLow) * mix);
			p.formantShift.highSemitones = static_cast<float>(high * mix);
			// THE CROSSOVER OVER THE UNLINKED CONTRIBUTORS ONLY: a linked sound's crossover is inert (its knob greyed — the
			// honesty rule), so it may not move the blend. All linked, the blend is linked and the crossover unread.
			const Group unlinked = gather([&](size_t s) {
				// THE ONE INERTNESS RULE the drawer greys by (spectralControlInert, params.h).
				return isOn(s, SpectralControl::FormantShiftOn)
				       && !spectralControlInert(SpectralControl::FormantShiftCrossover, 0,
				                                isOn(s, SpectralControl::FormantShiftLink));
			});
			if (unlinked.count > 0) {
				p.formantShift.crossoverHz =
				    static_cast<float>(std::exp2(mean(unlinked, SpectralControl::FormantShiftCrossover)));
			}
		}
	}
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::TiltOn); });
		if (g.count > 0) {
			p.tilt.on = true;
			p.tilt.dbPerOctave = static_cast<float>(mean(g, SpectralControl::TiltAmount) * mixOf(g));
			// No pivot control: under Tilt's level compensation the pivot is a constant gain the compensation removes.
		}
	}
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::OddEvenOn); });
		if (g.count > 0) {
			p.oddEven.on = true;
			p.oddEven.balance = static_cast<float>(mean(g, SpectralControl::OddEvenBalance) * mixOf(g));
		}
	}
	// THE VOWEL: each sound's position walked to its two formants (vowelFormants, PhiVox's anchors), and the formants
	// blended as a LOG-Hz mean (vox_bake's own precedent), so a morph between two vowels glides the peaks rather than
	// walking through the vowels between them on the position axis. EACH SOUND'S FORMANTS ARE WEIGHTED BY w·Depth — how much
	// of its vowel the blend hears (E/F/G review, 2026-10-12): a sound at Depth 0 weighs nothing, so its position cannot
	// move the blend (its knob greys: spectralControlInert), and its weight GROWS FROM 0 as its Depth does, so the formants
	// never jump as a Depth crosses 0 (gathering only the "heard" sounds stepped f1 250 -> 306 Hz between Depth 0 and 1e-6).
	// A lone sound with Depth above 0 gives its formants exactly; with none, the formants are unread (the depth is 0). Depth
	// itself fades by the mix, as every other depth does.
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::FormantOn); });
		if (g.count > 0) {
			p.vowel.on = true;
			double sumWd = 0.0;
			double log1 = 0.0;
			double log2 = 0.0;
			size_t heard = 0;
			size_t only = 0;
			for (size_t i = 0; i < g.count; ++i) {
				const size_t s = g.slot[i];
				const double depth = value(s, SpectralControl::FormantDepth);
				if (!(depth > 0.0)) {
					continue;
				}
				const VowelFormants v = vowelFormants(value(s, SpectralControl::FormantVowel));
				const double wd = weights.weight[s] * depth;
				sumWd += wd;
				log1 += wd * std::log2(v.f1Hz);
				log2 += wd * std::log2(v.f2Hz);
				++heard;
				only = s;
			}
			if (heard == 1) {
				const VowelFormants v = vowelFormants(value(only, SpectralControl::FormantVowel));
				p.vowel.f1Hz = static_cast<float>(v.f1Hz);
				p.vowel.f2Hz = static_cast<float>(v.f2Hz);
			}
			else if (sumWd > 0.0) {
				p.vowel.f1Hz = static_cast<float>(std::exp2(log1 / sumWd));
				p.vowel.f2Hz = static_cast<float>(std::exp2(log2 / sumWd));
			}
			p.vowel.depth = static_cast<float>(mean(g, SpectralControl::FormantDepth) * mixOf(g));
		}
	}
	// THE PARTIAL COUNT (lead's call, 2026-10-12, revisable): per sound, c = Σ w·count over the blend with an off sound
	// counting as every partial (its no-op), so the timbre and the CPU follow the puck and the voice's fractional top
	// partial keeps a moving count from stepping. On in every sound, it is their weighted mean exactly.
	{
		const auto schemeOf = [&](size_t s) {
			return static_cast<spectral::PartialsScheme>(
			    steppedIndex(params_.resolved(spectralParam(s, SpectralControl::PartialsMode)), kSpectralPartialsModeNames.size()));
		};
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::PartialsOn); });
		if (g.count > 0) {
			p.partialCount.on = true;
			// THE COUNT OVER THE SOUNDS WHOSE SCHEME READS IT (Peaks, Harmonics: Centers' count is inert, its knob hidden — the honesty
			// rule), an off sound counting as every partial, over the weight Centers does not take. With no Centers sound in the blend
			// this is the old blend, to the bit.
			const Group reads = gather([&](size_t s) {
				return isOn(s, SpectralControl::PartialsOn) && schemeOf(s) != spectral::PartialsScheme::Centers;
			});
			double centred = 0.0;
			for (size_t i = 0; i < g.count; ++i) {
				centred += schemeOf(g.slot[i]) == spectral::PartialsScheme::Centers ? weights.weight[g.slot[i]] : 0.0;
			}
			if (centred == 0.0) {
				const double on = mean(g, SpectralControl::PartialsCount);
				p.partialCount.count = static_cast<float>(
				    g.count == count ? on
				                     : (g.weight * on + (total - g.weight) * double(spectral::PartialsOp::kMaxCount)) / total);
			}
			else if (reads.count > 0) {
				const double on = mean(reads, SpectralControl::PartialsCount);
				const double rest = total - centred;
				p.partialCount.count =
				    static_cast<float>((reads.weight * on + (rest - reads.weight) * double(spectral::PartialsOp::kMaxCount)) / rest);
			}
			// CENTERS' SHARE AND SET (OI): its weight among the on-sounds, and the set's weighted mean over the Centers sounds (a
			// coordinate blended continuously, so a morph glides between sets).
			if (centred > 0.0) {
				double set = 0.0;
				for (size_t i = 0; i < g.count; ++i) {
					const size_t s = g.slot[i];
					if (schemeOf(s) == spectral::PartialsScheme::Centers) {
						set += weights.weight[s] / centred * value(s, SpectralControl::PartialsSet);
					}
				}
				p.partialCount.centers = centred == g.weight ? 1.0f : static_cast<float>(centred / g.weight);
				p.partialCount.centersSet = static_cast<float>(set);
			}
			// THE SCHEME (backlog OH) BLENDS ITS MASKS (P1b, by design: no step): Peaks' share of the on-sounds' weight, the
			// voice blending the two keep masks by it. Every on-sound in one scheme: the literal 0 or 1.
			double peaks = 0.0;
			for (size_t i = 0; i < g.count; ++i) {
				const size_t s = g.slot[i];
				if (steppedIndex(params_.resolved(spectralParam(s, SpectralControl::PartialsMode)), kSpectralPartialsModeNames.size())
				    == static_cast<size_t>(spectral::PartialsScheme::Peaks)) {
					peaks += weights.weight[s];
				}
			}
			p.partialCount.peaks = peaks == 0.0 ? 0.0f : peaks == g.weight ? 1.0f : static_cast<float>(peaks / g.weight);
			// DEPTH (backlog OF): every scheme's, so the weighted mean over the on-sounds, an off sound counting as 1 (its no-op, as it
			// counts as every partial above). On in every sound it is their mean exactly; one sound's is its own value.
			{
				const double on = mean(g, SpectralControl::PartialsDepth);
				p.partialCount.depth =
				    static_cast<float>(g.count == count ? on : (g.weight * on + (total - g.weight) * 1.0) / total);
			}
		}
	}
	// DAMP: the RATE (1 / T60, so an off sound's "no decay" is a rate of 0 and the blend is linear in how fast the partials
	// fall, not in how long they last) is the weighted mean over the on-sounds times the mix; the tilt their weighted mean.
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::DampOn); });
		if (g.count > 0) {
			double rate = 0.0;
			for (size_t i = 0; i < g.count; ++i) {
				const size_t s = g.slot[i];
				rate += (g.count == 1 ? 1.0 : weights.weight[s] / g.weight) * std::exp2(-value(s, SpectralControl::DampTime));
			}
			p.damp.on = true;
			p.damp.rate = static_cast<float>(rate * mixOf(g));
			p.damp.tilt = static_cast<float>(mean(g, SpectralControl::DampTilt));
		}
	}
	// STRETCH: B itself (not the Amount, which is B's cube root) blends by weight × the mix, so the morph is linear in the
	// inharmonicity and an off sound pulls toward harmonic.
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::StretchOn); });
		if (g.count > 0) {
			double b = 0.0;
			for (size_t i = 0; i < g.count; ++i) {
				const size_t s = g.slot[i];
				b += (g.count == 1 ? 1.0 : weights.weight[s] / g.weight)
				     * spectral::StretchOp::bOf(value(s, SpectralControl::StretchAmount));
			}
			p.stretch.on = true;
			p.stretch.b = static_cast<float>(b * mixOf(g));
		}
	}
	// SHIFT: the Hz blend by weight × the mix, an off sound counting as no shift.
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::ShiftOn); });
		if (g.count > 0) {
			p.shift.on = true;
			p.shift.hz = static_cast<float>(mean(g, SpectralControl::ShiftHz) * mixOf(g));
		}
	}
	// VOCODE (OE, additive design §9.4): DEPTH by weight × the mix, so an off sound pulls toward no vocoding in dB; Flatten, the
	// Gate and the Range the weighted means over the on-sounds (the Range in log Hz). The analysis it reads is the synth's.
	{
		const Group g = gather([&](size_t s) { return isOn(s, SpectralControl::VocodeOn); });
		if (g.count > 0) {
			p.vocode.on = true;
			p.vocode.depth = static_cast<float>(mean(g, SpectralControl::VocodeDepth) * mixOf(g));
			p.vocode.flatten = static_cast<float>(mean(g, SpectralControl::VocodeFlatten));
			p.vocode.gateDb = static_cast<float>(mean(g, SpectralControl::VocodeGate));
			p.vocode.lowHz = static_cast<float>(std::exp2(mean(g, SpectralControl::VocodeLow)));
			p.vocode.highHz = static_cast<float>(std::exp2(mean(g, SpectralControl::VocodeHigh)));
			// OE2: THE BANDED ROAD — Bandwidth and the zones' amounts and Free coordinates the weighted means over the on-sounds (a
			// coordinate glides), the zones and the stereo the engine's (choices: an enum cannot morph).
			p.vocode.bandwidth = static_cast<float>(mean(g, SpectralControl::VocodeBandwidth));
			p.vocode.zoneAmount = static_cast<float>(mean(g, SpectralControl::VocodeSpacingAmount));
			p.vocode.zoneFree = static_cast<float>(mean(g, SpectralControl::VocodeSpacingFree));
			p.vocode.widthAmount = static_cast<float>(mean(g, SpectralControl::VocodeWidthsAmount));
			p.vocode.widthFree = static_cast<float>(mean(g, SpectralControl::VocodeWidthsFree));
			p.vocode.zone = static_cast<int>(
			    steppedIndex(params_.resolved(ParamId::AdditiveVocodeSpacing), kAdditiveVocodeSpacingNames.size()));
			p.vocode.widthZone = static_cast<int>(
			    steppedIndex(params_.resolved(ParamId::AdditiveVocodeWidths), kAdditiveVocodeWidthsNames.size()));
			p.vocode.stereo = static_cast<int>(
			    steppedIndex(params_.resolved(ParamId::AdditiveVocodeStereo), kAdditiveVocodeStereoNames.size()));
		}
	}
	// THE DRAWN CURVES, per band: the weighted sum of the sounds whose curve is on, an off one counting as unity — LINEAR
	// IN AMPLITUDE, because a band drawn to zero would swallow a blend in dB.
	{
		const Group g = gather([&](size_t s) { return spectralState_.curve[s].on; });
		if (g.count > 0) {
			p.curve.on = true;
			if (g.count == 1 && g.count == count) {
				p.curve.gain = spectralState_.curve[g.slot[0]].gain;
			}
			else {
				const double rest = 1.0 - g.weight / total;
				for (size_t b = 0; b < spectral::kCurveBands; ++b) {
					double sum = rest;
					for (size_t i = 0; i < g.count; ++i) {
						const size_t s = g.slot[i];
						sum += weights.weight[s] / total * static_cast<double>(spectralState_.curve[s].gain[b]);
					}
					p.curve.gain[b] = static_cast<float>(sum);
				}
			}
		}
	}
	return p;
}

void Synth::seatDrawers() {
	// ADDITIVE ONLY: no other engine reads a voice's drawer (design R9). Off it, the voices keep the drawer they last had,
	// which only a Partials side fading out of an engine switch still reads.
	if (params_.oscType() != OscType::Additive) {
		drawerStereoLive_ = false;
		return;
	}
	++drawerSeatings_;
	// THE EARS DIFFER when a pair reaches a sound's S control (the routes: applyParamsRoad) or when the ears' blends do
	// (terrain stereo: each ear's drawer is its own blend of the sounds').
	drawerStereoLive_ = spectralState_.on && (drawerPairLive_ || earWeights_[0].weight != earWeights_[1].weight);
	// ONE DRAWER FOR EVERY VOICE ON THE MAIN BANK unless a per-voice source reaches the drawer; a part on a stand is blended
	// by its stand's weights. The voice's GainCache compares what it is handed, so a held drawer recomputes nothing.
	const spectral::ShaperParams once = drawerFor(-1, -1, false);
	const spectral::ShaperParams onceRight = drawerStereoLive_ ? drawerFor(-1, -1, true) : once;
	// EACH STAND'S DRAWER ONCE PER SEATING, not once per part on it (quality review, 2026-10-10: assembling one per stack part
	// every tick cost ~10% of a stacked render for drawers that had not changed). Memoised by the seating's serial, so a
	// stand asked for again in this pass is handed what the first ask built; per voice it cannot be shared.
	const auto standDrawer = [&](int stand) -> const spectral::ShaperParams& {
		const auto s = static_cast<size_t>(stand);
		if (standDrawerSeating_[s] != drawerSeatings_) {
			standDrawer_[s] = drawerFor(-1, stand, false);
			standDrawerSeating_[s] = drawerSeatings_;
		}
		return standDrawer_[s];
	};
	for (Voice& v : voices_) {
		if (!v.isSounding()) {
			continue;
		}
		// THE VOICE'S OWN, once for all its parts on the main bank.
		const spectral::ShaperParams left = drawerVaries_ ? drawerFor(v.slot, -1, false) : once;
		const spectral::ShaperParams right = drawerVaries_ && drawerStereoLive_ ? drawerFor(v.slot, -1, true) : onceRight;
		for (int part = 0; part < kMaxStack; ++part) {
			const Voice::StackPart& sp = v.parts[static_cast<size_t>(part)];
			if (part != 0 && !sp.live) {
				continue; // part 0 always plays; the rest when stacked
			}
			if (sp.stand >= 0) {
				// A STAND IS MONO (voice stacking §6 A): one blend, both ears.
				partOsc(v, part).setDrawer(drawerVaries_ ? drawerFor(v.slot, sp.stand, false) : standDrawer(sp.stand), nullptr);
				continue;
			}
			partOsc(v, part).setDrawer(left, drawerStereoLive_ ? &right : nullptr);
		}
	}
}

void Synth::seatReadPhase(const Voice& voice, OscVoice& osc, int part) {
	// FROM THIS CHUNK'S PLACE IN THE CONTROL TICK TO THE TICK'S END: the oscillator reads at its GENERATED position,
	// which can run up to three samples past the chunk but never past the tick (osc_voice.h, setReadPhase).
	const int tickPhase = PuckMotion::kTickSamples - motion_.samplesUntilTick();
	const int reach = PuckMotion::kTickSamples - tickPhase;
	// AND WHERE IN THE TICK THIS CALL STARTS, so a Partials side's control blocks end on the tick's block boundaries and
	// none straddles the tick end, where the run below stops (plan Task 9b). A tick must hold whole blocks for that.
	static_assert(PuckMotion::kTickSamples % partials::kControlBlock == 0,
	              "the additive voice's control blocks must tile the synth's control tick");
	osc.setTickPhase(tickPhase);
	// A STACK INDEX ROUTE INTO PHASE: part k reads amount × index cycles from the voice's phase (Phase's span is 1).
	const double partOffset = stackPhaseAmount_ != 0.0
	                              ? stackPhaseAmount_ * static_cast<double>(voice.parts[static_cast<size_t>(part)].index)
	                              : 0.0;
	// THE KNOB, apart from what the routes add, for Phase Width's mirror (Q10) — the right ear mirrors the modulation only.
	osc.setReadPhaseWidth(static_cast<float>(params_.base(static_cast<clap_id>(ParamId::OscPhase))),
	                      phaseSpreadLive_ ? phaseWidth_ : 0.0f);
	// **AND THE STEREO ROUTES' EAR DIFFERENCE** (backlog KU, 2026-10-05), per sample to the tick's end like the left ear's
	// run — asked only while Phase is stereo-live, so a patch with no stereo route into Phase pays one store here.
	float* ear = part == 0 ? earRun_.data() : partEarRun_.data();
	if (phaseStereoLive_ && reach > 0 && ParamSet::resolveEarDifferencePlanned(phasePlan_, voice.slot, tickPhase, reach, ear)) {
		osc.setReadPhaseEar(0.0f, ear, reach);
	}
	else {
		osc.setReadPhaseEar(0.0f, nullptr, 0);
	}
	float* run = part == 0 ? readRun_.data() : partReadRun_.data();
	if (reach > 0 && ParamSet::resolveWindowPlanned(phasePlan_, voice.slot, tickPhase, reach, partOffset, run)) {
		osc.setReadPhase(0.0f, run, reach);
		return;
	}
	const double base = phaseVaries_ ? params_.resolved(ParamId::OscPhase, voice.slot) : phaseOnce_;
	osc.setReadPhase(static_cast<float>(base + partOffset), nullptr, 0);
}

float Synth::stackVoiceLevel(const Voice& voice, bool levelVaries, double levelOnce) const {
	// THE LOOP'S OWN ARITHMETIC (`renderChunk`'s `gain`), asked before the parts are summed rather than after.
	const double base = levelVaries ? params_.resolved(ParamId::VoiceLevel, voice.slot) : levelOnce;
	return static_cast<float>(std::clamp(base + voice.levelMod, 0.0, 1.0));
}

float Synth::voiceStackIndexForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return 0.0f;
	}
	return voices_[static_cast<size_t>(slot)].parts[static_cast<size_t>(part)].index;
}

float Synth::voiceStackLevelScaleForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return 0.0f;
	}
	return voices_[static_cast<size_t>(slot)].parts[static_cast<size_t>(part)].levelScale;
}

bool Synth::stackIndexLive() const {
	if (stackFineAmount_ == 0.0 && stackLevelAmount_ == 0.0 && stackPosXAmount_ == 0.0 && stackPosYAmount_ == 0.0
	    && stackPhaseAmount_ == 0.0) {
		return false;
	}
	for (const Voice& v : voices_) {
		if (v.isSounding() && v.stacked) {
			return true;
		}
	}
	return false;
}

void Synth::respaceStack(Voice& voice, int count) {
	const int ticks = std::max(
	    1, static_cast<int>(std::lround(kStackRespaceSeconds * sampleRate_ / PuckMotion::kTickSamples)));
	const float samples = static_cast<float>(ticks * PuckMotion::kTickSamples);
	const float gain = stackGain(count);
	const double target = voice.haveTarget ? voice.lastTargetSemitones : targetSemitonesFor(voice);
	// WHERE THE FAN STANDS NOW, read before any part moves, so a part being born takes a neighbour's CURRENT
	// place rather than one another birth in this same pass has just invented.
	std::array<double, kMaxStack> standing {};
	std::array<bool, kMaxStack> wasLive {};
	std::array<float, kMaxStack> standingPan {};
	for (int part = 0; part < kMaxStack; ++part) {
		standing[static_cast<size_t>(part)] = voice.parts[static_cast<size_t>(part)].fan;
		standingPan[static_cast<size_t>(part)] = voice.parts[static_cast<size_t>(part)].pan;
		wasLive[static_cast<size_t>(part)] = voice.parts[static_cast<size_t>(part)].live;
	}
	for (int part = 0; part < kMaxStack; ++part) {
		Voice::StackPart& p = voice.parts[static_cast<size_t>(part)];
		if (part < count) {
			const double to = fanPosition(part, count);
			if (!p.live) {
				// BORN WHERE THE FAN ALREADY IS: at the sounding part nearest its own new place, from silence.
				// Silence is also why its phase is free — a part fading in from 0 cannot click, whatever phase
				// it starts at — so a reset oscillator is the honest start.
				double born = 0.0;
				float bornPan = 0.0f;
				double nearest = std::numeric_limits<double>::infinity();
				for (int other = 0; other < kMaxStack; ++other) {
					if (!wasLive[static_cast<size_t>(other)]) {
						continue;
					}
					const double distance = std::abs(standing[static_cast<size_t>(other)] - to);
					if (distance < nearest) {
						nearest = distance;
						born = standing[static_cast<size_t>(other)];
						bornPan = standingPan[static_cast<size_t>(other)];
					}
				}
				p = {};
				p.fan = born;
				p.pan = p.panTo = bornPan; // and in the stereo field where it stands too, then slews to its own place
				p.levelScale = kLevelScaleUnseated; // and at its share of a Level route, from its first sample
				p.gain = 0.0f;
				p.live = true;
				OscVoice& osc = partOsc(voice, part);
				osc.reset();
				// ON THE PUCK, reading part 0's vowel, until the next tick seats it on its own stand (§3.11: a new part starts
				// on the centre stand and crosses to its own).
				osc.moveToBank(&bank_);
				osc.shareVoxFrom(&voice.osc);
				osc.setGlideSeconds(glideSecondsNow());
				const double pitch = partTargetFor(voice, target, part);
				osc.setPitch(pitch);
				p.lastTargetSemitones = pitch;
				p.haveTarget = true;
			}
			p.fanFrom = p.fan;
			p.fanTo = to;
			p.gainTo = gain;
		}
		else if (p.live) {
			// LEAVING: it holds its place and fades out, and stops rendering only once silent (`renderStackParts`).
			p.fanFrom = p.fanTo = p.fan;
			p.gainTo = 0.0f;
		}
		p.gainStep = std::abs(p.gainTo - p.gain) / samples;
	}
	voice.stackCount = count;
	voice.respaceTicksLeft = ticks;
	voice.stacked = true;
	// THE INDEX FOR THE NEW COUNT (step 3): every value moves with N, and the pitches follow on the next pass.
	updateStackIndex(voice, false);
}

void Synth::advanceRespace(Voice& voice) {
	voice.respaceTicksLeft--;
	const int ticks = std::max(
	    1, voice.respaceTicksLeft + 1); // the walk's remaining span, this tick included
	for (Voice::StackPart& p : voice.parts) {
		if (!p.live) {
			continue;
		}
		if (voice.respaceTicksLeft <= 0) {
			p.fan = p.fanTo; // exactly, so a finished walk leaves no rounding in anyone's pitch
		}
		else {
			p.fan += (p.fanTo - p.fan) / static_cast<double>(ticks);
		}
	}
}

bool Synth::anyStackRespacing() const {
	for (const Voice& v : voices_) {
		if (v.isSounding() && v.respaceTicksLeft > 0) {
			return true;
		}
	}
	return false;
}

void Synth::syncStack() {
	const int count = params_.stackCount();
	if (count == appliedStackCount_) {
		return;
	}
	appliedStackCount_ = count;
	for (Voice& v : voices_) {
		// A voice seated since the change (a note-on reads Stack itself) is spread for it already.
		if (v.isSounding() && v.stage != Voice::Stage::Stealing && v.stackCount != count) {
			respaceStack(v, count);
		}
	}
}

void Synth::aimStackPans(Voice& voice) {
	// WHERE EACH PART SITS IN THE STEREO FIELD (backlog KE step 2): its place in the fan, doubled to −1..+1, with
	// the sign alternating by distance from the fan's edge — the firmware's "−2 +1 0 −1 +2" (`setupUnisonStereo
	// Spread`, voice-stacking doc §1.2), so neighbours in pitch are not neighbours in space — times Stack Spread,
	// resolved FOR THIS VOICE. A part leaving the stack keeps the place it had while it fades.
	const double spread = params_.resolved(ParamId::StackSpread, voice.slot);
	const int count = voice.stackCount;
	bool panned = false;
	for (int part = 0; part < kMaxStack; ++part) {
		Voice::StackPart& p = voice.parts[static_cast<size_t>(part)];
		if (!p.live) {
			continue;
		}
		if (part < count) {
			const int fromEdge = std::min(part, count - 1 - part);
			const double sign = (fromEdge & 1) != 0 ? -1.0 : 1.0;
			p.panTo = static_cast<float>(sign * 2.0 * p.fan * spread);
		}
		panned = panned || p.panTo != 0.0f || p.pan != 0.0f;
	}
	voice.panned = panned;
}

bool Synth::anyStackPanned() const {
	for (const Voice& v : voices_) {
		if (v.isSounding() && v.stacked && v.panned) {
			return true;
		}
	}
	return false;
}

void Synth::renderStackParts(Voice& voice, float* left, float* right, int n, bool earsLive, float level) {
	// PART 0 IS ALREADY IN THE BUFFER, at unity — both ears when they are live, the left alone otherwise, which
	// a stereo road without live ears copies across before anything is scaled (the part is one mono signal).
	if (right != nullptr && !earsLive) {
		std::copy_n(left, n, right);
	}
	// THE PAN SLEWS PER SAMPLE, at a full swing (−1 → +1) per re-space time, so a Count change's sign flips and
	// a Spread move glide rather than step, the same samples at any host block size.
	const float slew = static_cast<float>(2.0 / std::max(1.0, kStackRespaceSeconds * sampleRate_));
	// THE PAN LAW IS A BALANCE, unity at the centre and never above it: a part panned by p keeps the near channel
	// at 1 and turns the far one down to 1 − |p| (voice-stacking doc §7, assumption 11). The conservative choice —
	// a stack can never get louder by being spread — and a part at pan 0 is the unpanned part to the bit.
	auto walk = [n, slew](Voice::StackPart& p, float* gains, float* toLeft, float* toRight) {
		for (int i = 0; i < n; ++i) {
			if (p.gain < p.gainTo) {
				p.gain = std::min(p.gainTo, p.gain + p.gainStep);
			}
			else if (p.gain > p.gainTo) {
				p.gain = std::max(p.gainTo, p.gain - p.gainStep);
			}
			if (p.pan < p.panTo) {
				p.pan = std::min(p.panTo, p.pan + slew);
			}
			else if (p.pan > p.panTo) {
				p.pan = std::max(p.panTo, p.pan - slew);
			}
			gains[i] = p.gain;
			stackPanGains(p.pan, toLeft[i], toRight[i]);
		}
	};
	// A STACK INDEX ROUTE INTO LEVEL (step 3): part k sounds at the voice's level plus amount × index × span (Level's
	// span is 1), held to 0..1 as the voice's own is. The voice multiplies its level in after the sum, so a part
	// carries the RATIO of its own level to the voice's, walked across the chunk from where the last one left it. At
	// a silent voice level there is nothing to scale and the ratio stays 1 (a part cannot sound above a silent
	// voice: provisional, voice-stacking doc §7, assumption 19). Unrouted, every part is at 1 and nothing multiplies.
	auto levelShare = [this, level, n](Voice::StackPart& p, float* gains) {
		float to = 1.0f;
		if (stackLevelAmount_ != 0.0 && level > 0.0f) {
			const double own = std::clamp(static_cast<double>(level) + stackLevelAmount_ * static_cast<double>(p.index),
			                              0.0, 1.0);
			to = static_cast<float>(own / static_cast<double>(level));
		}
		if (p.levelScale == kLevelScaleUnseated) {
			p.levelScale = to;
		}
		if (p.levelScale == 1.0f && to == 1.0f) {
			return;
		}
		const float from = p.levelScale;
		const float step = (to - from) / static_cast<float>(n);
		for (int i = 0; i < n; ++i) {
			gains[i] *= from + step * static_cast<float>(i + 1);
		}
		p.levelScale = to;
	};
	std::array<float, kRenderChunk> gains {};
	std::array<float, kRenderChunk> toLeft {};
	std::array<float, kRenderChunk> toRight {};
	walk(voice.parts[0], gains.data(), toLeft.data(), toRight.data());
	levelShare(voice.parts[0], gains.data());
	for (int i = 0; i < n; ++i) {
		const auto s = static_cast<size_t>(i);
		if (right != nullptr) {
			left[i] *= gains[s] * toLeft[s];
			right[i] *= gains[s] * toRight[s];
		}
		else {
			left[i] *= gains[s]; // the mono road is only taken while nothing is panned
		}
	}
	bool settled = voice.respaceTicksLeft <= 0 && voice.parts[0].gain == voice.parts[0].gainTo;
	for (int part = 1; part < kMaxStack; ++part) {
		Voice::StackPart& p = voice.parts[static_cast<size_t>(part)];
		if (!p.live) {
			continue;
		}
		OscVoice& osc = partOsc(voice, part);
		seatReadPhase(voice, osc, part);
		if (earsLive) {
			osc.renderStereo(partScratch_, partScratchR_, n);
		}
		else {
			osc.render(partScratch_, n);
		}
		const float* ofRight = earsLive ? partScratchR_ : partScratch_;
		walk(p, gains.data(), toLeft.data(), toRight.data());
		levelShare(p, gains.data());
		for (int i = 0; i < n; ++i) {
			const auto s = static_cast<size_t>(i);
			if (right != nullptr) {
				left[i] += partScratch_[i] * gains[s] * toLeft[s];
				right[i] += ofRight[i] * gains[s] * toRight[s];
			}
			else {
				left[i] += partScratch_[i] * gains[s];
			}
		}
		if (p.gain == 0.0f && p.gainTo == 0.0f) {
			p.live = false; // faded out of the stack: silent, and no longer rendered
		}
		settled = settled && p.gain == p.gainTo;
	}
	// A STACK OF ONE AT REST IS THE PLAIN VOICE AGAIN: its one part at exactly unity and the centre, nobody else
	// live, so the stack road can step aside and the render is the render it always was.
	if (settled && voice.stackCount == 1 && voice.parts[0].gain == 1.0f && voice.parts[0].pan == 0.0f) {
		voice.stacked = false;
	}
}

void Synth::applyVoiceFx(Voice& voice, float* left, float* right, int n) {
	// NOTHING TO DO IN THE USUAL PATCH, and this is the line that makes that free: with no device
	// switched to its per-voice arrangement the lane has nothing per voice, so a voice pays one
	// predictable branch rather than a walk over eight empty ones per chunk.
	if (!lane_.anyPerVoice()) {
		return;
	}

	// THE NOTE'S TARGET PITCH, NOT THE PITCH THE OSCILLATOR IS AT THIS INSTANT. A gliding voice moves
	// its pitch once per generated sample, so anything read from `osc.pitchSemitones()` would land
	// wherever the chunk boundaries fell — the block-size bug the cases in test_synth.cpp exist to
	// catch, arriving through a door nobody was watching. The target moves only in applyPitches, once
	// per slice, and the binding cuts a render call at every event, so it cannot move inside one.
	//
	// It costs a glide its filter sweep: the corner arrives at the destination note in the filter's
	// own ten milliseconds while the pitch is still travelling. That is a real limitation and the
	// honest trade for a render that does not depend on the host's buffer size.
	//
	// The key is the fallback rather than a zero: a voice is given its target at the top of every
	// slice it sounds in, so this should never be reached, and reading zero semitones would put a
	// tracked corner five octaves below where it belongs if it ever were.
	const double pitch = voice.haveTarget ? voice.lastTargetSemitones : static_cast<double>(voice.key);

	// ONE PITCH AND A GATE, AND NO DEVICE NAMED. This used to compute the filter's key offset here —
	// `pitch - kKeytrackReferenceKey` gated on the keytrack flag — which is device knowledge sitting
	// in the engine, and the reason HARM could not join: its note road wants the pitch itself, not an
	// offset from a filter's reference key. Each device folds the note in with its own rule now.
	//
	// THE GATE IS THIS VOICE'S OWN STAGE, which is what the rack learned and wrote down: derive the
	// gate from the voices' own stages rather than counting note-ons beside them, and the sustain
	// pedal, mono, legato and all-notes-off all answer once instead of five times.
	device::NoteContext note;
	note.pitchSemitones = static_cast<float>(pitch);
	note.gate = voice.stage == Voice::Stage::Attack || voice.stage == Voice::Stage::Sustain;
	// THE STACK, as the parts were seated: how many, and their fan — the level a per-voice device is handed depends on
	// how coherently they sum (`device::NoteContext`).
	note.stackCount = voice.stackCount;
	note.stackDetuneCents = static_cast<float>(params_.resolved(ParamId::StackDetune, voice.slot));
	// And how alike the parts are at 0 cents — read here, after this chunk's parts have rendered, so every part has
	// synced to the table it now plays. Recomputed only when one of those tables moved; exactly 1 on one bank.
	updateStackCoherence(voice);
	note.stackCoherence = voice.stackCoherence;
	// THE LANE'S VOICE PASS: the note delivered now, per chunk, then every per-voice device in chain
	// order — stereo, or the mono road when the chunk is mono (`right` null).
	lane_.processVoice(static_cast<size_t>(voice.slot), left, right, n, note);
}

void Synth::renderChunk(float* mix, int n) {
	const rack::StageScope stage(rack::Stage::Voices); // the spike recorder's (backlog SR): inert unless one is bound
	std::fill_n(mix, n, 0.0f);
	const bool levelVaries = params_.variesPerVoice(ParamId::VoiceLevel);
	const double levelOnce = params_.resolved(ParamId::VoiceLevel);
	phaseVaries_ = params_.variesPerVoice(ParamId::OscPhase);
	phaseOnce_ = params_.resolved(ParamId::OscPhase);


	for (Voice& v : voices_) {
		if (!v.isSounding()) {
			continue;
		}

		seatReadPhase(v, v.osc, 0);
		v.osc.render(scratch_, n);
		// THE STACK'S OTHER PARTS, summed in before the voice's devices — they are one voice's sound, and a
		// filter after the sum is the firmware's arrangement too (voice-stacking doc §1.1). Not reached at all
		// by a voice that is not stacked, which is what keeps this loop today's render to the bit.
		if (v.stacked) {
			renderStackParts(v, scratch_, nullptr, n, false, stackVoiceLevel(v, levelVaries, levelOnce));
		}
		applyVoiceFx(v, scratch_, nullptr, n);

		// Base level and the per-note offset, clamped as a pair: the offset is a distance and can be
		// negative, and only their sum is a setting.
		// LEVEL RESOLVED FOR THIS VOICE WHEN A PER-VOICE SOURCE REACHES IT (backlog DU, 2026-09-13), and ONCE
		// otherwise: resolved above the loop it moved every note by the last note's pressure; resolved N times
		// unconditionally it cost ~5% of a block on a patch with twenty routes into Level and nothing per voice
		// among them (measured). `variesPerVoice` is the design's "default trivial handling", kept literal.
		const double levelBase = levelVaries ? params_.resolved(ParamId::VoiceLevel, v.slot) : levelOnce;
		const auto gain = static_cast<float>(std::clamp(levelBase + v.levelMod, 0.0, 1.0));
		v.gain = gain;
		// LEVEL, PER SAMPLE, when a moving windowed source reaches it — per-voice or block-rate (backlog FK).
		std::array<float, kRenderChunk> gainRun {};
		const bool tracing = gainTrace_ != nullptr && v.slot == gainTraceSlot_; // TEST ONLY, decided once per chunk
		const bool windowed = n <= kRenderChunk // per-voice windows AND block-rate ones (an MSEG, a tracker)
		                      && ParamSet::resolveWindowPlanned(levelPlan_, v.slot, PuckMotion::kTickSamples - motion_.samplesUntilTick(), n,
		                                                       v.levelMod, gainRun.data());
		// **A RELEASED VOICE LIVES WHILE ITS VOICE LEVEL DOES**. The voice's own 2 ms edge waits, level held, for as
		// long as what is ROUTED into this voice's Voice Level — every route, per-voice or shared, whatever device
		// makes it — stands above the floor (-60 dB), then closes. Once per chunk per voice. A shape that ends above
		// the floor holds the voice by design; one that crosses zero frees it there.
		//
		// WHAT IT REPLACED, and why each was wrong: the hold read only the PER-VOICE part of the routes (a macro or
		// a mono envelope pulling Voice Level up let the voice die at the key — MEASURED, a macro at 0.8 left no
		// voice 0.2 s after it), then asked the routed DEVICE whether its note was still playing (which a deleted
		// and replaced envelope, a mono one, or anything that never heard the note could not answer). Before
		// that it held only a FALLING value, so a one-shot still rising at the key died there.
		//
		// TWO THINGS DO NOT HOLD IT, neither about an envelope: the BASE — Voice Level with nothing routed is a
		// level, not a shape, and the voice is the organ gate the voice-level design states (2 ms off at the key);
		// and a voice SUPERSEDED on its key (`Voice::superseded`), whose Voice Level now reads the new note's.
		bool heldByRoute = false;
		if (v.stage == Voice::Stage::Release && !v.superseded) {
			heldByRoute = params_.routedOffset(ParamId::VoiceLevel, v.slot) > kRouteHeldFloor;
		}

		// THE PER-VOICE ROAD'S GAIN DELAY (`gainDelay_`): null at 0, and the loop below is today's to the bit.
		float* ring = gainDelay_ == 0 ? nullptr : gainRing_.data() + static_cast<size_t>(v.slot) * gainRingCap_;
		for (int i = 0; i < n; i++) {
			switch (v.stage) {
			case Voice::Stage::Attack:
				v.level += v.step;
				if (v.level >= 1.0f) {
					v.level = 1.0f;
					v.stage = Voice::Stage::Sustain;
				}
				break;
			case Voice::Stage::Release:
				if (heldByRoute) {
					break; // the routed envelope still sounds this voice — the built-in release waits (backlog EF)
				}
				[[fallthrough]];
			case Voice::Stage::Stealing:
				v.level -= v.step;
				if (v.level <= 0.0f) {
					v.level = 0.0f;
				}
				break;
			case Voice::Stage::Idle:
			case Voice::Stage::Sustain:
				break;
			}
			if (tracing) {
				gainTrace_->push_back(windowed ? gainRun[static_cast<size_t>(i)] : gain);
			}
			if (ring == nullptr) {
				mix[i] += scratch_[i] * v.level * (windowed ? gainRun[static_cast<size_t>(i)] : gain);
			}
			else {
				mix[i] += scratch_[i] * delayedGain(v, ring, v.level * (windowed ? gainRun[static_cast<size_t>(i)] : gain));
			}
		}

		// Checked after the chunk rather than inside it: a voice that reached silence part way through
		// contributes zeroes for the rest, which is what it should contribute anyway.
		// A RELEASED VOICE DRAINS ITS DELAYED TAIL FIRST: its gain closed `gainDelay_` samples before its audio does.
		if (ring != nullptr && v.level <= 0.0f && v.stage == Voice::Stage::Release) {
			if (!v.draining) {
				v.draining = true;
				v.drainLeft = gainDelay_;
			}
			else {
				v.drainLeft = v.drainLeft > static_cast<uint32_t>(n) ? v.drainLeft - static_cast<uint32_t>(n) : 0u;
			}
		}
		const bool drained = ring == nullptr || !v.draining || v.drainLeft == 0 || v.stage != Voice::Stage::Release;
		// A STEAL ON THE LATE ROAD ENDS WITH ITS OWN FADE (delayedGain), not with the level.
		const bool faded = ring != nullptr && v.stage == Voice::Stage::Stealing ? v.stealFade <= 0.0f : v.level <= 0.0f;
		// A RELEASED PLUCK WHOSE STRINGS ARE EXACTLY SILENT ENDS NOW (by design): its
		// string has nothing left to sound, and its edge's release would only render zeros. Only when this chunk's own output
		// was exact silence too (a per-voice device may still be ringing it) and off the late road (whose gain runs ahead).
		const bool stringDone = ring == nullptr && v.stage == Voice::Stage::Release && stringsSilent(v)
		                        && std::all_of(scratch_, scratch_ + n, [](float x) { return x == 0.0f; });
		// A DRUM HIT THAT HAS RUN OUT TO EXACT SILENCE ENDS ITS VOICE (drum proposal §4.4): a One-shot voice is never released
		// by its key, so this is the road it ends by — through a release of silence, which the roads below finish and report.
		const bool drumDone = drumsSilent(v) && std::all_of(scratch_, scratch_ + n, [](float x) { return x == 0.0f; });
		// AND A ONE-SHOT LET GO ON DRUM AND CARRIED ONTO ANOTHER ENGINE ENDS NOW (`Voice::oneShotLetGo`): no drum road will.
		if (drumDone || letGoOffDrum(v)) {
			endDrumVoice(v);
		}
		if ((stringDone || faded || (drumDone && ring == nullptr && v.stage == Voice::Stage::Release))
		    && (v.stage == Voice::Stage::Release || v.stage == Voice::Stage::Stealing)
		    && drained) {
			const bool wasReleasing = v.stage == Voice::Stage::Release;
			v.draining = false;
			v.stage = Voice::Stage::Idle;
			v.haveTarget = false;
			v.levelMod = 0.0;
			if (wasReleasing && voiceEnded_) {
				// A stolen voice was handed back the moment it was stolen; a released one is only free
				// now. Telling the manager late would cost polyphony, telling it twice would corrupt its
				// accounting, so exactly one of the two paths reports here.
				voiceEnded_(&v);
			}
			if (wasReleasing && isPlayableChannel(v.channel) && isPlayableKey(v.key)
			    && !hostDown_[static_cast<size_t>(v.channel) * 128u + static_cast<size_t>(v.key)]) {
				hostNoteEnded(v.channel, v.key, v.noteId, &v, false);
			}
		}
	}

	// THE MASTER GAIN IS NOT HERE ANY MORE. It moved up into render(), because the fx chain is a
	// PRE-MASTER insert and a filter that ran after the fader would be driven by the fader. Nothing
	// about the arithmetic changed: the same multiply, in the same order, once per sample, applied to
	// the same values whenever the chain is empty — which is what the bit-exactness cases pin.
}

void Synth::renderChunkStereo(float* left, float* right, int n, bool earsLive) {
	const rack::StageScope stage(rack::Stage::Voices); // the spike recorder's (backlog SR): inert unless one is bound
	// The stereo twin of renderChunk — same voice walk, same edge arithmetic, and the edge advances
	// ONCE per sample for the pair, because a voice has one amplitude however many ears hear it.
	// Mirrored rather than shared with the mono loop for the reason the header states: that loop is
	// the Width-0 bit-exactness guarantee and stays untouched.
	std::fill_n(left, n, 0.0f);
	std::fill_n(right, n, 0.0f);
	// A LIVE PAD LANE'S BUS starts each chunk empty (drum proposal §17).
	if (padLanesLive_) {
		for (size_t pad = 0; pad < padLanes_.size(); ++pad) {
			if (padLanes_[pad].live) {
				std::fill_n(padBusL_.data() + pad * static_cast<size_t>(kRenderChunk), n, 0.0f);
				std::fill_n(padBusR_.data() + pad * static_cast<size_t>(kRenderChunk), n, 0.0f);
			}
		}
	}
	const bool levelVaries = params_.variesPerVoice(ParamId::VoiceLevel);
	const double levelOnce = params_.resolved(ParamId::VoiceLevel);
	phaseVaries_ = params_.variesPerVoice(ParamId::OscPhase);
	phaseOnce_ = params_.resolved(ParamId::OscPhase);


	for (Voice& v : voices_) {
		if (!v.isSounding()) {
			continue;
		}
		seatReadPhase(v, v.osc, 0);

		// BOTH EARS WHEN THE TERRAIN HAS TWO; OTHERWISE ONE SIGNAL IN BOTH CHANNELS. This road is also taken for a
		// spread stack with the terrain mono (backlog KE step 2), and there the oscillator's ears would be one table
		// evaluated twice for nothing — so it renders once, and the two channels differ only by the stack's pans.
		// AND A STACKED VOICE WITH WIDTH STANDS HAS NO EARS OF ITS OWN (step 4c, §6 A): each part hears ONE stand, displaced
		// by its place in the fan, and Spread places it — so every part renders once, mono.
		const bool voiceEars = earsLive && !(v.stacked && widthStands_);
		if (voiceEars) {
			v.osc.renderStereo(scratch_, scratchR_, n);
		}
		else {
			v.osc.render(scratch_, n);
			std::copy_n(scratch_, n, scratchR_);
		}
		// The stack, as in the mono loop — each part mono from its own stand while terrain stereo is live (step 4c).
		if (v.stacked) {
			renderStackParts(v, scratch_, scratchR_, n, voiceEars, stackVoiceLevel(v, levelVaries, levelOnce));
		}
		applyVoiceFx(v, scratch_, scratchR_, n);

		// LEVEL RESOLVED FOR THIS VOICE WHEN A PER-VOICE SOURCE REACHES IT (backlog DU, 2026-09-13), and ONCE
		// otherwise: resolved above the loop it moved every note by the last note's pressure; resolved N times
		// unconditionally it cost ~5% of a block on a patch with twenty routes into Level and nothing per voice
		// among them (measured). `variesPerVoice` is the design's "default trivial handling", kept literal.
		const double levelBase = levelVaries ? params_.resolved(ParamId::VoiceLevel, v.slot) : levelOnce;
		const auto gain = static_cast<float>(std::clamp(levelBase + v.levelMod, 0.0, 1.0));
		v.gain = gain;
		// LEVEL, PER SAMPLE, when a moving windowed source reaches it — per-voice or block-rate (backlog FK).
		std::array<float, kRenderChunk> gainRun {};
		const bool tracing = gainTrace_ != nullptr && v.slot == gainTraceSlot_; // TEST ONLY, decided once per chunk
		const bool windowed = n <= kRenderChunk // per-voice windows AND block-rate ones (an MSEG, a tracker)
		                      && ParamSet::resolveWindowPlanned(levelPlan_, v.slot, PuckMotion::kTickSamples - motion_.samplesUntilTick(), n,
		                                                       v.levelMod, gainRun.data());
		// **A RELEASED VOICE LIVES WHILE ITS VOICE LEVEL DOES**. The voice's own 2 ms edge waits, level held, for as
		// long as what is ROUTED into this voice's Voice Level — every route, per-voice or shared, whatever device
		// makes it — stands above the floor (-60 dB), then closes. Once per chunk per voice. A shape that ends above
		// the floor holds the voice by design; one that crosses zero frees it there.
		//
		// WHAT IT REPLACED, and why each was wrong: the hold read only the PER-VOICE part of the routes (a macro or
		// a mono envelope pulling Voice Level up let the voice die at the key — MEASURED, a macro at 0.8 left no
		// voice 0.2 s after it), then asked the routed DEVICE whether its note was still playing (which a deleted
		// and replaced envelope, a mono one, or anything that never heard the note could not answer). Before
		// that it held only a FALLING value, so a one-shot still rising at the key died there.
		//
		// TWO THINGS DO NOT HOLD IT, neither about an envelope: the BASE — Voice Level with nothing routed is a
		// level, not a shape, and the voice is the organ gate the voice-level design states (2 ms off at the key);
		// and a voice SUPERSEDED on its key (`Voice::superseded`), whose Voice Level now reads the new note's.
		bool heldByRoute = false;
		if (v.stage == Voice::Stage::Release && !v.superseded) {
			heldByRoute = params_.routedOffset(ParamId::VoiceLevel, v.slot) > kRouteHeldFloor;
		}

		// THE PER-VOICE ROAD'S GAIN DELAY (`gainDelay_`): null at 0, and the loop below is today's to the bit.
		// A KIT PAD WITH A LIVE LANE SUMS INTO ITS BUS (drum proposal §17); every other voice into the mix, as ever.
		float* outL = left;
		float* outR = right;
		if (padLanesLive_) {
			const size_t pad = v.osc.drumPad();
			if (pad < padLanes_.size() && padLanes_[pad].live) {
				outL = padBusL_.data() + pad * static_cast<size_t>(kRenderChunk);
				outR = padBusR_.data() + pad * static_cast<size_t>(kRenderChunk);
			}
		}
		float* ring = gainDelay_ == 0 ? nullptr : gainRing_.data() + static_cast<size_t>(v.slot) * gainRingCap_;
		for (int i = 0; i < n; i++) {
			switch (v.stage) {
			case Voice::Stage::Attack:
				v.level += v.step;
				if (v.level >= 1.0f) {
					v.level = 1.0f;
					v.stage = Voice::Stage::Sustain;
				}
				break;
			case Voice::Stage::Release:
				if (heldByRoute) {
					break; // the routed envelope still sounds this voice — the built-in release waits (backlog EF)
				}
				[[fallthrough]];
			case Voice::Stage::Stealing:
				v.level -= v.step;
				if (v.level <= 0.0f) {
					v.level = 0.0f;
				}
				break;
			case Voice::Stage::Idle:
			case Voice::Stage::Sustain:
				break;
			}
			if (tracing) {
				gainTrace_->push_back(windowed ? gainRun[static_cast<size_t>(i)] : gain);
			}
			const float now = v.level * (windowed ? gainRun[static_cast<size_t>(i)] : gain);
			const float amp = ring == nullptr ? now : delayedGain(v, ring, now);
			outL[i] += scratch_[i] * amp;
			outR[i] += scratchR_[i] * amp;
		}

		// Checked after the chunk rather than inside it, exactly as the mono loop checks — and one of
		// the two loops reports a released voice's end, never both, because only one runs a chunk.
		// A RELEASED VOICE DRAINS ITS DELAYED TAIL FIRST: its gain closed `gainDelay_` samples before its audio does.
		if (ring != nullptr && v.level <= 0.0f && v.stage == Voice::Stage::Release) {
			if (!v.draining) {
				v.draining = true;
				v.drainLeft = gainDelay_;
			}
			else {
				v.drainLeft = v.drainLeft > static_cast<uint32_t>(n) ? v.drainLeft - static_cast<uint32_t>(n) : 0u;
			}
		}
		const bool drained = ring == nullptr || !v.draining || v.drainLeft == 0 || v.stage != Voice::Stage::Release;
		// A STEAL ON THE LATE ROAD ENDS WITH ITS OWN FADE (delayedGain), not with the level.
		const bool faded = ring != nullptr && v.stage == Voice::Stage::Stealing ? v.stealFade <= 0.0f : v.level <= 0.0f;
		// A RELEASED PLUCK WHOSE STRINGS ARE EXACTLY SILENT ENDS NOW: the mono loop's rule, over both ears' output.
		const bool stringDone = ring == nullptr && v.stage == Voice::Stage::Release && stringsSilent(v)
		                        && std::all_of(scratch_, scratch_ + n, [](float x) { return x == 0.0f; })
		                        && std::all_of(scratchR_, scratchR_ + n, [](float x) { return x == 0.0f; });
		const bool drumDone = drumsSilent(v) && std::all_of(scratch_, scratch_ + n, [](float x) { return x == 0.0f; })
		                      && std::all_of(scratchR_, scratchR_ + n, [](float x) { return x == 0.0f; });
		if (drumDone || letGoOffDrum(v)) { // the mono loop's two roads
			endDrumVoice(v);
		}
		if ((stringDone || faded || (drumDone && ring == nullptr && v.stage == Voice::Stage::Release))
		    && (v.stage == Voice::Stage::Release || v.stage == Voice::Stage::Stealing)
		    && drained) {
			const bool wasReleasing = v.stage == Voice::Stage::Release;
			v.draining = false;
			v.stage = Voice::Stage::Idle;
			v.haveTarget = false;
			v.levelMod = 0.0;
			if (wasReleasing && voiceEnded_) {
				voiceEnded_(&v);
			}
			if (wasReleasing && isPlayableChannel(v.channel) && isPlayableKey(v.key)
			    && !hostDown_[static_cast<size_t>(v.channel) * 128u + static_cast<size_t>(v.key)]) {
				hostNoteEnded(v.channel, v.key, v.noteId, &v, false);
			}
		}
	}

	// THE KIT PADS' FX LANES (drum proposal §17): each live pad's bus through its lane, into the voices' mix.
	if (padLanesLive_) {
		runPadLanes(left, right, n);
	}
	// The master gain moved to render() here too, for the reason the mono loop states.
}

void Synth::setTransport(double songPositionSeconds, double songPositionBeats, bool playing) {
	// THE MASTER METER STARTS OVER WHEN THE TRANSPORT STARTS (backlog DN) — the
	// rising edge, so a clip from the last take does not glow into this one, and the loudness window
	// begins on what is played now. A transport already playing is not an edge.
	if (playing && !transportPlaying_) {
		masterMeter_.reset();
	}
	// THE PLUCKS' STRIKE COUNTS START OVER AT EVERY PLAY-STATE EDGE (KS design §6): a stopped transport's plucks are drawn
	// from the count since it last changed state, so a take started from silence draws the same plucks every time.
	if (playing != transportPlaying_) {
		ks_->strikes.fill(0);
		drum_->strikes.fill(0);
	}
	songBlockStartSeconds_ = songPositionSeconds;
	songBlockStartBeats_ = songPositionBeats;
	songSamplesIntoBlock_ = 0;
	// **DERIVED FROM THE SECONDS, NOT SET UNCONDITIONALLY, AND THAT IS A BEHAVIOUR-PRESERVING FIX.**
	// This read `haveTransport_ = true` because the caller only called it when a timeline existed;
	// under DQ the caller reports every block and says which timelines it has, so an unconditional
	// true would tell the oscillator lock it had a position and then hand it NaN.
	//
	// The seconds are the right test rather than either timeline: `lockedPhaseFor` is the consumer of
	// this flag and it reads `songSecondsNow()`.
	haveTransport_ = std::isfinite(songPositionSeconds);
	transportPlaying_ = playing;
}

void Synth::setTempo(double bpm) noexcept {
	// STORED RAW, VALIDATED ON READ. A host sending a nonsense tempo must not be able to poison a
	// field the audio thread divides by, and rackClock() below is the one place that decides.
	tempoBpm_ = bpm;
}

mod::TransportClock Synth::rackClock() const noexcept {
	// NOT FINITE, OR NOT POSITIVE, IS "THE HOST DID NOT SAY". Same answer as no transport at all, and
	// the device applies its own fallback — see mseg's window. Deciding it here keeps every reader
	// from having to remember the test.
	// **THE POSITION RIDES ALONG WHATEVER THE TEMPO SAYS — backlog DQ.** A host can offer a timeline
	// and no tempo, or a tempo and no timeline, and the two early returns below used to make the
	// second case swallow the first: with no tempo this handed back `unknown()`, which now would also
	// throw away a perfectly good seconds position.
	//
	// So the position is filled first and the tempo decides only the tempo.
	mod::TransportClock clock = (std::isfinite(tempoBpm_) && tempoBpm_ > 0.0)
	    ? mod::TransportClock::fromSecondsPerBeat(60.0 / tempoBpm_)
	    : mod::TransportClock::unknown();
	if (haveTransport_) {
		// **THE SAME "NOW" THE OSCILLATOR LOCK USES**, block start plus what has been rendered since,
		// so a device locking its phase and a voice locking its phase agree about what time it is.
		clock.songPositionSeconds = songSecondsNow();
		clock.songPositionBeats = songBeatsNow();
		clock.playing = transportPlaying_;
	}
	return clock;
}

/// The instant the next sample to be rendered sits at, in seconds of song.
///
/// THE ONE PLACE THE TWO HALVES ARE ADDED, so that "block start plus what we have rendered since"
/// is stated once rather than at every caller who wants to know the time.
double Synth::songSecondsNow() const {
	return songBlockStartSeconds_
	       + static_cast<double>(songSamplesIntoBlock_) / sampleRate_;
}

int64_t Synth::songTickNow() const {
	if (!haveTransport_ || !transportPlaying_) {
		return -1;
	}
	const double ticks = std::floor(songSecondsNow() * sampleRate_ / static_cast<double>(PuckMotion::kTickSamples));
	return std::isfinite(ticks) && ticks >= 0.0 ? static_cast<int64_t>(ticks) : -1;
}

double Synth::songBeatsNow() const {
	// **THE SAME "BLOCK START PLUS WHAT WE HAVE RENDERED SINCE", IN THE OTHER UNIT.** Beats advance at
	// the tempo, so this needs one where the seconds road does not — and a host offering a beats
	// timeline without a tempo cannot be moved forward within a block at all. NaN then, which is the
	// same answer as having no beats timeline: a reader asks `havePositionBeats()` either way.
	if (!std::isfinite(songBlockStartBeats_) || !std::isfinite(tempoBpm_) || tempoBpm_ <= 0.0) {
		return device::TransportClock::kNoPosition;
	}
	const double secondsPerBeat = 60.0 / tempoBpm_;
	return songBlockStartBeats_
	       + (static_cast<double>(songSamplesIntoBlock_) / sampleRate_) / secondsPerBeat;
}

void Synth::lockPhaseToSong(Voice& voice) {
	// THE SAME-VOICE ROADS. Analog used to do nothing at all here — the phase was already running
	// and the mode's whole content was declining to disturb it. Under the transport lock it has
	// something to say after all: a retrigger is a new note, and a new note starts where the song
	// says. With no transport this is the old do-nothing, restored exactly.
	const double target = targetSemitonesFor(voice);
	// Every live part at the song's phase for ITS pitch (backlog KE); part 0 of a stack of one is the voice.
	for (int part = 0; part < kMaxStack; ++part) {
		if (!voice.parts[static_cast<size_t>(part)].live) {
			continue;
		}
		const std::optional<double> locked = lockedPhaseFor(partTargetFor(voice, target, part));
		if (locked.has_value()) {
			partOsc(voice, part).adoptPhase(*locked);
		}
	}
}

std::optional<double> Synth::lockedPhaseFor(double semitones) const {
	if (!haveTransport_) {
		return std::nullopt;
	}
	const double hz = pitchToHz(semitones);
	if (!(hz > 0.0) || !std::isfinite(hz)) {
		return std::nullopt;
	}
	// A STOPPED TRANSPORT IS NOT A POSITION TO LOCK TO. It is the same position for as long as the
	// user leaves it there, so locking would hand every audition of a note the identical start
	// phase — the oscillator would stop being an oscillator between takes. The free-running counter
	// is what it locks to instead: still a clock, just not the song's. Determinism is a property
	// bounces need and live playing does not.
	const double seconds =
	    transportPlaying_ ? songSecondsNow() : freeRunSamples_ / sampleRate_;
	// PRECISION, STATED BECAUSE IT DEGRADES SILENTLY: the product is turns since the song began, and
	// only its fraction survives. A double carries about sixteen digits, so at an hour in at 4 kHz —
	// 1.4e7 turns — there are still eight or nine digits of fraction left, far below anything the
	// 256-slot grid resolves. It is worth knowing that this gets worse the longer the song, and that
	// it would matter first for the highest notes.
	const double turns = hz * seconds;
	return turns - std::floor(turns);
}

void Synth::render(float* left, float* right, int n) {
	applyParams();
	beginPadLanes(); // the kit pads' fx lanes, taken and drained for this render call (drum proposal §17)
	// IS THE ONE STRING LIVE? Only on Resonate. Params cannot move inside a render call (the binding cuts
	// the call at every event), so this is one answer for the whole call.
	const bool stringLive = params_.oscType() == OscType::Resonate;

	int done = 0;
	while (done < n) {
		// Capped by the bank as well as by the chunk size, for exactly the reason our own caller caps
		// this call at the next event: a character change held behind a fade takes effect the moment
		// the guard reaches zero, and rendering past that moment would land it at the end of a piece
		// whose length the HOST chose. The chunk size itself is free — the oscillator renders
		// identically at any of them — but this boundary is not.
		// CAPPED BY THE MOTION AS WELL AS BY THE BANK, and for the same reason both of them state: the
		// velocity changes on a fixed sample grid, and a character rebuilt whenever the renderer
		// happened to look would carry that change into the sound at a moment the HOST chose. Rendering
		// no further than the next tick is what makes a swipe sound the same at every buffer size — and
		// it is a real constraint rather than a precaution, found by a moving-puck invariance test that
		// diverged within a hundred samples of the gesture starting.
		// AND BY THE STRING'S TICK, while the string is live: the ring moves on its own fixed clock
		// (dsp/weave_string.h) and its tables change at a tick, so a chunk that ran across one would read the
		// new window from the old one. OFF RESONATE THE STRING IS FROZEN (stringLive, above): no cut, no tick,
		// no fade fill, so another engine renders on exactly the chunking it always had, and the string resumes
		// where it stopped when Resonate comes back.
		// AND BY EVERY KEYED STAND'S BANK (voice stacking step 4), for the main bank's reason: a stand's build lands at a
		// chunk boundary, never inside a chunk the host chose.
		// AND BY THE NOTE LANE'S NEXT EMISSION (the arp): what a self-clocked device
		// emits at this sample is played first, and the chunk stops where its next event falls, so a step lands on its
		// own sample. No emitter → no limit, and the chunking is the one it always was.
		const uint32_t untilEmit = emitLaneNotes();
		const int m = std::min({ n - done, kRenderChunk, bank_.samplesUntilPendingBuild(),
			                     motion_.samplesUntilTick(),
			                     stringLive ? weave_.samplesUntilTick() : std::numeric_limits<int>::max(),
			                     standsUntilBuild(),
			                     static_cast<int>(std::min<uint32_t>(untilEmit, std::numeric_limits<int>::max())) });
		// THE STRING'S FADE FOR THIS CHUNK, once for every voice — see OscVoice::setWeaveFades for the three
		// samples past the end.
		bool resonateStereo = false; // a second scan head is sounding this chunk (Width)
		if (stringLive) {
			const int fades = m + 3;
			for (int k = 0; k < fades; ++k) {
				weaveFades_[static_cast<size_t>(k)] = weave_.fadeAt(k);
			}
			// RESONATE'S WIDTH, the right ear's head in cycles round the ring: half the ring at full width (the
			// firmware's widest stereo zones set their pickups opposite). It walks to the knob at a fixed rate —
			// the whole half-ring in 30 ms — so a knob move is a walk and the trajectory is the same at every
			// block size.
			constexpr double kHalfRing = 0.5;
			const double tapPerSample = kHalfRing / (0.030 * sampleRate_);
			const double tapFrom = resonateWidthNow_ * kHalfRing;
			const double tapTarget = resonateWidthTarget_ * kHalfRing;
			resonateStereo = tapFrom > 0.0 || tapTarget > 0.0;
			for (Voice& v : voices_) {
				for (int part = 0; part < kMaxStack; ++part) { // every stacked part reads the one string
					partOsc(v, part).setWeaveFades(weaveFades_.data(), fades);
					partOsc(v, part).setWeaveTap(tapFrom, tapTarget, tapPerSample);
				}
			}
			const double reach = tapPerSample * static_cast<double>(m);
			resonateWidthNow_ = (tapFrom + std::clamp(tapTarget - tapFrom, -reach, reach)) / kHalfRing;
			weaveFrozen_ = false;
		}
		else if (!weaveFrozen_) {
			// FROZEN, THE VOICES ARE HANDED ONE ENTRY: the fade where the string stopped (its clock stands at
			// the end of the last live chunk), count 1, so a voice still fading out of a Resonate side reads
			// ONE static cycle. Handing them the last live chunk's array instead would replay its path from the
			// top at every render call (a voice restarts its index there), a chunk-rate sawtooth under the
			// bank's fade. Done once per freeze, not per chunk.
			frozenFade_ = weave_.fadeAt(0);
			for (Voice& v : voices_) {
				for (int part = 0; part < kMaxStack; ++part) {
					partOsc(v, part).setWeaveFades(&frozenFade_, 1);
				}
			}
			weaveFrozen_ = true;
		}
		// THE KARPLUS-STRONG CONTROLS for this chunk, once for every voice; and PLUCK WIDTH takes the stereo road with live
		// ears, the right ear reading the string at its own pickup. At width 0, or on another engine, the road it was.
		ks_->stereo = false;
		if (params_.oscType() == OscType::Pluck) {
			const ks::StringControls controls = stringControls();
			ks_->stereo = controls.width > 0.0;
			for (Voice& v : voices_) {
				for (int part = 0; part < kMaxStack; ++part) {
					partOsc(v, part).setStringControls(controls);
				}
			}
		}
		// THE DRUM KIT for this chunk, once for every voice (each reads it through one pointer); a panned pad takes the stereo
		// road, its voice writing both ears. Centred and on any other engine, the road it was.
		bool drumStereo = false;
		if (params_.oscType() == OscType::Drum) {
			fillDrumKit();
			drumStereo = drum_->kit.panned;
		}
		// WHERE THIS CHUNK STARTS inside the control tick, so a device reading windows — in the voices and on the bus — reads
		// its own samples.
		lane_.setTickPhase(PuckMotion::kTickSamples - motion_.samplesUntilTick());
		// **AND THE CLOCK AT THIS CHUNK'S FIRST SAMPLE, NOT THE SEGMENT'S** — `lane_.begin` stamps the
		// clock once per segment and a segment is cut here into ≤32-frame chunks. Without this every chunk
		// of a block read the block's first sample, and a device that seats itself on the song (SKLITTER's
		// write head) wrote the same 32 frames sixteen times a block. See `RackEngine::setClock`.
		lane_.setClock(rackClock());
		// The path is picked PER CHUNK, off the bank's stereo window, and the window only moves when
		// a build lands — which is always at a chunk boundary, because the loop never renders past
		// samplesUntilPendingBuild. While the window is shut every ear pointer in every voice is ear
		// 0's table, so the mono path below is exact, not approximate — width 0 and the frozen
		// engines render today's samples through today's code.
		// AND A STACK SPREAD IN STEREO TAKES THE STEREO ROAD TOO (backlog KE step 2), with the terrain's ears live or
		// not as the bank says. The pans are aimed first, once a chunk, so the road is chosen by where the parts
		// are going as well as where they are; a stack with nothing panned stays on the mono road, bit for bit.
		for (Voice& v : voices_) {
			if (v.isSounding() && v.stacked) {
				aimStackPans(v);
			}
		}
		// AND RESONATE'S WIDTH takes the stereo road with live ears: the right ear then reads the ring at the
		// second head (OscVoice::setWeaveTap). At width 0 it is not taken, so a mono Resonate patch stays on the
		// mono road, bit for bit.
		// AND PHASE WIDTH (Q10) with something to mirror: the ears read at different phases, so they render apart. With
		// the terrain mono both ears read one table — the difference is the mirrored phase and nothing else. At width
		// 0, or with nothing moving Phase off its knob, the road is the one it was.
		phaseWidth_ = static_cast<float>(params_.resolved(ParamId::OscPhaseWidth));
		phaseSpreadLive_ = phaseWidth_ > 0.0f
		                   && (phasePlan_.windows > 0 || phasePlan_.blockWindows > 0 || phasePlan_.cells > 0
		                       || phasePlan_.scalar != params_.base(ParamId::OscPhase) || stackPhaseAmount_ != 0.0);
		// AND PHASE AS A STEREO DESTINATION (backlog KU, 2026-10-05): a pair routed into Phase reads the ears at different
		// phases at any width, so it takes the stereo road with live ears too. No pair, no change: the road it was.
		phaseStereoLive_ = phasePlan_.stereoLive();
		phaseSpreadLive_ = phaseSpreadLive_ || phaseStereoLive_;
		// AND A PER-VOICE DEVICE WITH A STEREO ROUTE (backlog KU): its right ear needs a right channel. Not an ear of
		// the terrain, so `earsLive` is not touched — the voice renders once and the device splits it.
		// AND THE SPECTRAL DRAWER (additive design §3.3, §4.3): each sounding voice is handed its drawer — per voice where a
		// per-voice source reaches it — ONCE A CONTROL TICK (PuckMotion::kTickSamples, 64 samples: every per-voice source moves
		// at that cadence, in tickEnvelopes, so a chunk inside a tick would only be handed the same drawer again) and at the
		// top of every render call (an event between calls may have moved a param). Ears whose drawers differ take the stereo
		// road with live ears, as a stereo Phase does.
		// OE: THE SIDECHAIN'S ENVELOPE for this chunk's voices, from this chunk's sidechain (one analysis for all of them).
		updateVocode(done, m);
		if (done == 0 || motion_.samplesUntilTick() == PuckMotion::kTickSamples) {
			seatDrawers();
		}
		// THE BANK'S FM HALF IS ASKED ONLY WHILE A VOICE SOUNDS (backlog PB): it reads the tables, and a table is made
		// when first read, so asking over silence would make every build a moved character records. With nothing
		// sounding both roads write the same silence, so the answer cannot change a sample.
		const bool bankStereo = soundingVoiceCount() > 0 ? bank_.stereoActive() : bank_.stereoWindow();
		const bool stereoChunk = bankStereo || anyStackPanned() || resonateStereo || phaseSpreadLive_ || ks_->stereo || drumStereo
		                         || padLanesLive_ // a pad lane's output may be stereo, so its chunk is
		                         || lane_.anyPerVoiceStereo() || drawerStereoLive_ || vocodeStereoLive_;
		lastChunkStereo_ = stereoChunk; // TEST ONLY reader: `lastChunkStereoForTest`
		if (stereoChunk) {
			renderChunkStereo(left + done, right + done, m,
			                  bankStereo || resonateStereo || phaseSpreadLive_ || drawerStereoLive_ || vocodeStereoLive_ || ks_->stereo
			                      || drumStereo);
		}
		else {
			renderChunk(left + done, m);
			// One oscillator per voice and one table per ear pair, so the right channel is the left one.
			std::copy_n(left + done, m, right + done);
		}
		// OE2 SIBILANCE: the noise layer for this chunk, gated by the vocoding voices (addVocodeNoise).
		addVocodeNoise(left + done, right + done, m);
		// **LEVEL, THE FADER**, on the voices' sum before the lane — where the old Level's multiply
		// stood, so the fx are driven as they were at unity. Ramped across the chunk from where the last chunk left
		// it, so automation does not zipper; at rest at unity it is skipped, and the render is the one before it.
		const auto levelNow = static_cast<float>(params_.resolved(ParamId::Level));
		if (!outputLevelPrimed_) {
			outputLevelWas_ = levelNow; // the first chunk after activation starts AT the fader, not on a ramp to it
			outputLevelPrimed_ = true;
		}
		if (levelNow != 1.0f || outputLevelWas_ != 1.0f) {
			const float step = (levelNow - outputLevelWas_) / static_cast<float>(m);
			for (int i = 0; i < m; i++) {
				const float g = outputLevelWas_ + step * static_cast<float>(i + 1);
				left[done + i] *= g;
				right[done + i] *= g;
			}
			outputLevelWas_ = levelNow;
		}

		// THE FX CHAIN, AFTER THE MONO COPY AND BEFORE THE MASTER GAIN.
		//
		// After the copy so a device always sees two real channels, whichever path rendered them —
		// otherwise the mono path would hand a stereo device one buffer and a stale second one. And
		// before the master so the fader stays a fader: a filter driven by the master gain would change
		// its own drive every time somebody set a level, which is not what a mixer control means.
		//
		// It is a hard bypass when the lane is empty or the device is off, so this line reads and
		// writes nothing at all in the default patch, and the master multiply below is the same
		// multiply the two chunk tails used to do, per sample and in the same order. That is a
		// bit-exactness claim rather than a closeness one, and tests/test_synth.cpp holds it: every
		// case there renders through this seam now.
		lane_.setSidechain(sideL_ != nullptr ? sideL_ + done : nullptr, sideR_ != nullptr ? sideR_ + done : nullptr, sideSounding_);
		{
			const rack::StageScope stage(rack::Stage::Bus); // its devices taken out (backlog SR)
			lane_.processBus(left + done, right + done, m);
		}

		const auto master = static_cast<float>(params_.masterGain());
		for (int i = done; i < done + m; i++) {
			left[i] *= master;
			right[i] *= master;
		}
		// THE MASTER METER READS WHAT LEAVES — after the master, because a level meter that saw the
		// signal before the fader would be a meter of something nobody hears (backlog DN).
		masterMeter_.process(left + done, right + done, m);

		{
			const rack::StageScope stage(rack::Stage::Rebake); // the bank's deferred builds land here (backlog SR)
			bank_.advance(m);
			postPhaseRequest(); // a deferred build may have just landed
			for (int s = 0; s < kStandPool; ++s) {
				if (standKeyed_[static_cast<size_t>(s)]) {
					(*standBanks_)[static_cast<size_t>(s)].advance(m);
				}
			}
		}
		// The string, after every voice has read this chunk of it — and only while it is live.
		if (stringLive) {
			weave_.advance(m);
		}
		// THE SONG CLOCK ADVANCES HERE, WITH THE OTHER TWO AND AFTER THE SAMPLES ARE RENDERED — which
		// is the difference between "the time of the next note-on" and "the time one chunk later".
		// A note-on arrives BETWEEN render calls, because the caller cuts the block at every event,
		// so what a starting voice must read is how much has already been rendered. Advancing before
		// the chunk would hand it the end of a chunk it has not heard yet, and the error would be
		// invisible at small block sizes and audible at large ones — the exact shape of bug this
		// engine's block-size rules exist to prevent.
		songSamplesIntoBlock_ += m;
		freeRunSamples_ += static_cast<double>(m);
		samplesRendered_ += static_cast<uint64_t>(m);
		// Told the same number of samples as the bank, from the same place, so the two clocks in this
		// engine cannot drift apart.
		const bool ticked = m >= motion_.samplesUntilTick();
		motion_.advance(m);
		if (ticked) {
			// THE MODULATORS MOVE FIRST, THEN WHAT THEY REACH IS RE-DECIDED. The order matters: an
			// envelope published after the character was rebuilt would land a tick late on the puck,
			// and one tick of a 1 ms attack is most of the attack.
			const bool envelopesReachSomething = tickEnvelopes();

			// A new velocity, at an absolute sample position rather than at a block boundary. Only the
			// character is rebuilt: the voice settings and the pitches have not moved, and re-deciding
			// them here would be work at eight hundred times a second for an answer that cannot change.
			//
			// UNLESS A MODULATOR HAS MADE THAT ANSWER ABLE TO CHANGE, which is what the branch is. Fine
			// Tune is a destination and so is the filter's corner, and neither is reached by
			// updateCharacter — so while an envelope is routed anywhere, the whole of applyParams is
			// what has to run here. The sentence above stays true for every patch that has no envelope
			// routed, which is every patch written before today: those pay one relaxed load and take
			// the same road they always took.
			if (envelopesReachSomething) {
				applyParamsOnTick();
			}
			else {
				updateCharacter();
				// A STACK BEING RE-SPACED WALKS ON THIS TICK TOO (backlog KE): its fan moves whether or not anything
				// is routed, so the pitches it reaches are re-decided here for exactly as long as a walk is in flight.
				// AND WHILE A STACK INDEX ROUTE REACHES A STACKED VOICE (step 3): its index moves with a per-voice source, and
				// Drift walks on this tick, so the pitches and level shares are re-decided each tick for as long as one does.
				if (anyStackRespacing() || stackIndexLive()) {
					applyPitches(true);
				}
			}
		}
		done += m;
	}
	endPadLanes(); // and every pad lane's block ends with the render too
	// THE LANE'S LAST SEGMENT ENDS WITH THE RENDER, so a device the drain parked during it is reapable
	// by the message thread's next visit rather than held until the next render begins.
	lane_.finish();
}

int Synth::soundingVoiceCount() const {
	int count = 0;
	for (const Voice& v : voices_) {
		count += v.isSounding() ? 1 : 0;
	}
	return count;
}

int Synth::managedVoiceCount() const {
	return static_cast<int>(voiceManager_->getVoiceCount());
}

bool Synth::soakReadingForTest(uint32_t what, double* out) const {
	const auto stageCount = [this](Voice::Stage stage) {
		int n = 0;
		for (const Voice& v : voices_) {
			n += v.stage == stage ? 1 : 0;
		}
		return static_cast<double>(n);
	};
	const rack::RackEngine& lane = lane_.engineForTest();
	switch (what) {
	case CLAP_FEATHERS_SYNTH_MANAGED_VOICES: *out = managedVoiceCount(); return true;
	case CLAP_FEATHERS_SYNTH_SOUNDING_VOICES: *out = soundingVoiceCount(); return true;
	case CLAP_FEATHERS_SYNTH_ATTACK: *out = stageCount(Voice::Stage::Attack); return true;
	case CLAP_FEATHERS_SYNTH_SUSTAIN: *out = stageCount(Voice::Stage::Sustain); return true;
	case CLAP_FEATHERS_SYNTH_RELEASE: *out = stageCount(Voice::Stage::Release); return true;
	case CLAP_FEATHERS_SYNTH_STEALING: *out = stageCount(Voice::Stage::Stealing); return true;
	case CLAP_FEATHERS_SYNTH_KEYS_DOWN: {
		int n = 0;
		for (const HeldNote& h : heldNotes_) {
			n += h.down ? 1 : 0;
		}
		*out = n;
		return true;
	}
	case CLAP_FEATHERS_SYNTH_LANE_INVENTED: *out = static_cast<double>(lane.inventedHeldForTest()); return true;
	case CLAP_FEATHERS_SYNTH_LANE_MOD_RINGS: *out = static_cast<double>(lane.modRingsInUseForTest()); return true;
	case CLAP_FEATHERS_SYNTH_LANE_RETIRED: *out = static_cast<double>(lane.retiredCountForTest()); return true;
	case CLAP_FEATHERS_SYNTH_LANE_PENDING: *out = static_cast<double>(lane.pendingFieldsForTest()); return true;
	case CLAP_FEATHERS_SYNTH_PATTERN_HELD:
	case CLAP_FEATHERS_SYNTH_PATTERN_SOUNDING:
	case CLAP_FEATHERS_SYNTH_PATTERN_LOWEST:
	case CLAP_FEATHERS_SYNTH_PATTERN_NEWEST: {
		*out = -1.0; // no Pattern on the lane
		for (size_t slot = 0; slot < lane.slotsForTest(); ++slot) {
			const device::Descriptor* d = lane.descriptorForTest(slot);
			if (d == nullptr || std::strcmp(d->traits.tag, mod::arp::pat::kTraits.tag) != 0) {
				continue;
			}
			const auto* p = static_cast<const mod::arp::pat::PatternDevice*>(lane.storageForTest(slot));
			if (p == nullptr) {
				continue;
			}
			const mod::arp::HeldChord& chord = p->chordForTest();
			if (what == CLAP_FEATHERS_SYNTH_PATTERN_HELD) {
				*out = static_cast<double>(chord.size());
			}
			else if (what == CLAP_FEATHERS_SYNTH_PATTERN_SOUNDING) {
				*out = static_cast<double>(p->soundingForTest());
			}
			else {
				int lowest = -1, newest = -1;
				uint32_t newestOrder = 0;
				for (size_t i = 0; i < chord.size(); ++i) {
					const int k = chord[i].key;
					lowest = lowest < 0 ? k : std::min(lowest, k);
					if (newest < 0 || chord[i].order >= newestOrder) {
						newest = k;
						newestOrder = chord[i].order;
					}
				}
				*out = what == CLAP_FEATHERS_SYNTH_PATTERN_LOWEST ? lowest : newest;
			}
			return true;
		}
		return true;
	}
	default: return false;
	}
}

double Synth::voiceLevelModForTest(int slot) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return 0.0;
	}
	return voices_[static_cast<size_t>(slot)].levelMod;
}

double Synth::voiceGainForTest(int slot) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return 0.0;
	}
	return voices_[static_cast<size_t>(slot)].gain;
}

int Synth::slotOfKeyForTest(int16_t key) const {
	for (const Voice& v : voices_) {
		// A stolen voice keeps its key while it fades, and it is not the one a test asking "what is
		// playing this note" means.
		if (v.isSounding() && v.stage != Voice::Stage::Stealing && v.key == key) {
			return v.slot;
		}
	}
	return -1;
}

double Synth::voiceStackPitchForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return 0.0;
	}
	return partOsc(voices_[static_cast<size_t>(slot)], part).pitchSemitones();
}

double Synth::voiceStackFanForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return 0.0;
	}
	return voices_[static_cast<size_t>(slot)].parts[static_cast<size_t>(part)].fan;
}

float Synth::voiceStackGainForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return 0.0f;
	}
	return voices_[static_cast<size_t>(slot)].parts[static_cast<size_t>(part)].gain;
}

bool Synth::voiceStackLiveForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return false;
	}
	return voices_[static_cast<size_t>(slot)].parts[static_cast<size_t>(part)].live;
}

float Synth::voiceStackPanForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return 0.0f;
	}
	return voices_[static_cast<size_t>(slot)].parts[static_cast<size_t>(part)].pan;
}

void Synth::voiceStackChannelGainsForTest(int slot, int part, float* toLeft, float* toRight) const {
	stackPanGains(voiceStackPanForTest(slot, part), *toLeft, *toRight);
}

double Synth::voicePitchForTest(int slot) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return 0.0;
	}
	return voices_[static_cast<size_t>(slot)].osc.pitchSemitones();
}

float Synth::voiceEdgeLevelForTest(int slot) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return 0.0f;
	}
	return voices_[static_cast<size_t>(slot)].level;
}

double Synth::voiceExpressionForTest(int slot, int32_t expression) const {
	if (slot < 0 || slot >= kVoiceSlots || expression < 0
	    || static_cast<size_t>(expression) >= kNoteExpressionCount) {
		return 0.0;
	}
	return voices_[static_cast<size_t>(slot)].expression[static_cast<size_t>(expression)];
}

float Synth::envelopeValueForTest(size_t slot) const {
	if (slot >= rack_.device.size()) {
		return 0.0f;
	}
	// WHICHEVER DEVICE THE SLOT HOLDS. This read the envelope unconditionally, which was true while a
	// slot could only hold one — and it meant the first case written for the MSEG's tick reported
	// SILENCE from a device that was running correctly. A seam that can only see one device answers
	// zero for the other and reads exactly like a feature that does not work.
	return lane_.outputOf(stripDevice(slot), kEnvelopeOutput);
}

bool Synth::envelopeGateForTest() const {
	return envGate_;
}

double Synth::voicePhaseForTest(int slot) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return 0.0;
	}
	return voices_[static_cast<size_t>(slot)].osc.phase();
}

int Synth::voiceWeaveMipForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return -1;
	}
	return partOsc(voices_[static_cast<size_t>(slot)], part).weaveMipForTest();
}

int Synth::voiceWeaveFadeCountForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return -1;
	}
	return partOsc(voices_[static_cast<size_t>(slot)], part).weaveFadeCountForTest();
}

WeaveString::Fade Synth::voiceWeaveFadeReadForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return {};
	}
	return partOsc(voices_[static_cast<size_t>(slot)], part).weaveFadeReadForTest();
}

double Synth::voiceWeaveTapForTest(int slot, int part) const {
	if (slot < 0 || slot >= kVoiceSlots || part < 0 || part >= kMaxStack) {
		return 0.0;
	}
	return partOsc(voices_[static_cast<size_t>(slot)], part).weaveTapForTest();
}

double Synth::voiceModulatorPhaseForTest(int slot) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return 0.0;
	}
	return voices_[static_cast<size_t>(slot)].osc.modulatorPhase();
}

fx::filter::Settings Synth::voiceFilterTargetForTest(int slot) const {
	if (slot < 0 || slot >= kVoiceSlots) {
		return {};
	}
	// ASKED OF THE POOL, NOT OF A MEMBER, exactly as busFilterTargetForTest already was — the voice's
	// filter is whatever the lane holds in its per-voice arrangement, and a lane is reorderable, so
	// "the filter" has no fixed address.
	const void* storage = lane_.voiceStorageForTag(static_cast<size_t>(slot), fx::filter::kTraits.tag);
	return storage != nullptr ? fx::filter::opsTargetForTest(storage) : fx::filter::Settings {};
}

Voice* Synth::claimFreeVoice() {
	// A STEALING VOICE IS NOT IDLE, AND THAT IS WHAT STOPS THE MANAGER'S REPLACEMENT NOTE LANDING ON
	// A SLOT THAT IS STILL SOUNDING.
	//
	// terminateVoice reports the note's end IMMEDIATELY, before any fade begins, and the manager
	// nulls the cookie on that report — so the voice becomes unaddressable and unstealable at once,
	// while its audio is still fading out. That leaves a deliberate window in which the manager has
	// forgotten the voice and the pool has not yet freed it, and in that window the object belongs
	// to nobody but itself. The four steal-fade spares are what pays for the window; reclaiming the
	// slot early is what they exist to avoid.
	//
	// SO THE Idle TEST BELOW IS THE GUARD, not a formality. "Let claimFreeVoice take Stealing voices
	// too, and stop paying for four spare slots" is a natural-looking optimisation and it would hand
	// note B a voice still fading out note A — two note-ids on one object, and a note-off for one
	// silencing the other. That is the notes-cutting-out bug, and this line is the only thing
	// between here and it.
	for (Voice& v : voices_) {
		if (v.stage == Voice::Stage::Idle) {
			return &v;
		}
	}

	// AND WHEN THERE IS NO IDLE VOICE, THE OLDEST FADING ONE IS CUT RATHER THAN THE NOTE DROPPED.
	//
	// **THE PARAGRAPH ABOVE IS ABOUT PREFERENCE, NOT ABOUT PROHIBITION**, and the distinction is the
	// whole of this block. Taking a `Stealing` voice while an `Idle` one exists is the bug it
	// describes — two note-ids on one object. Taking one when there is no alternative cannot be: by
	// the time a voice reaches `Stealing`, `terminateVoice` has already reported its note's end and
	// the manager has nulled the cookie, so there is no second note-id left to collide with. The cost
	// is that its remaining fade is cut instead of finished — one click.
	//
	// WITHOUT IT THE NOTE IS SILENTLY LOST, and it costs a SECOND one because the manager commits the
	// steal before it asks for the voice: MEASURED at `VoiceCount = 16`, twenty-one simultaneous notes
	// left the synth holding **fifteen** — one below its own polyphony limit — because the victim was
	// already gone when `claimFreeVoice` returned null. Reachable by hand: hold sixteen notes, tap
	// Poly -> Mono -> Poly, play a five-note chord, and four sound.
	//
	// AND `kStealFadeSpares`' OWN COMMENT ALREADY PROMISED THIS: *"Past that the extra steals are
	// instant cuts rather than fades: one click, and the pool is whole again 2 ms later."* It was
	// describing behaviour the code did not have. It does now.
	//
	// THE OLDEST BY `startSerial`, which is stamped in `beginFade` on every road into Attack and only
	// ever climbs — so "oldest" is the note that started first rather than the slot that happens to be
	// lowest, and a burst of steals gives up its fades in the order it began them.
	Voice* oldest = nullptr;
	for (Voice& v : voices_) {
		if (v.stage != Voice::Stage::Stealing) {
			continue;
		}
		if (oldest == nullptr || v.startSerial < oldest->startSerial) {
			oldest = &v;
		}
	}
	return oldest;
}

void Synth::beginFade(Voice& voice, Voice::Stage stage, double seconds, double sampleRate,
                      uint64_t attackSerial) {
	// A NOTE STARTING IS WHAT STAMPS THE SERIAL — every road into Attack comes through here, which is
	// why it is stamped here rather than at each of the call sites. See Voice::startSerial.
	if (stage == Voice::Stage::Attack) {
		voice.startSerial = attackSerial;
	}
	// A NEW STAGE ENDS A DRAIN: a retrigger or a steal mid-drain must not carry it into the next release (review, Important 3).
	voice.draining = false;
	// AND ENDS A ONE-SHOT'S LET-GO: a new note's Attack is a key down, and a Release or a steal is the end it was waiting for.
	voice.oneShotLetGo = false;
	voice.drainLeft = 0;
	if (stage == Voice::Stage::Stealing) {
		// THE STEAL FADE ON THE DELAYED OUTPUT RUNS FROM 1, ON ITS OWN MULTIPLIER (delayedGain). With the per-voice road late the
		// level is not what sounds: a draining voice sits at level 0 while its delayed gain still plays, so a fade scaled from the
		// level would be a hard cut (review, Important 2).
		voice.stealFade = 1.0f;
	}
	voice.stage = stage;
	// From wherever the level is, at the rate a full-scale fade would take: a voice cut short during
	// its attack finishes sooner, which is right — the ramp's job is a bounded, click-free descent,
	// not a fixed duration.
	voice.step = edgeStep(seconds, sampleRate);
}

//-------------------------------//
// Voice manager responder       //
//-------------------------------//

void Synth::Responder::setVoiceEndCallback(std::function<void(Voice*)> callback) {
	synth.voiceEnded_ = std::move(callback);
}

int32_t Synth::Responder::beginVoiceCreationTransaction(
    typename sst::voicemanager::VoiceBeginBufferEntry<VoiceManagerConfig>::buffer_t& buffer,
    uint16_t, uint16_t, uint16_t key, int32_t, float) {
	// A DRUM KIT'S NOTE PLAYS THE PADS MAPPED TO IT (drum proposal §5): none (an unmapped note is silent), one, or several (a
	// layer), each in its pad's own group, in slot order. Remembered for initializeMultipleVoices, which is handed the voices
	// in this order.
	synth.drum_->notePadCount = 0;
	if (synth.params_.drumKit()) {
		int n = 0;
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			const auto note = std::lround(synth.params_.resolved(drumParam(slot, DrumControl::Note)));
			// MUTE AND SOLO: a pad whose sound card is switched off is muted, and while a card
			// is soloed (its S, the preview) only that pad is struck. Every sound is still a PAD (its fields, its place); a
			// muted one simply takes no hit.
			if (!synth.padAudible(slot)) {
				continue;
			}
			if (note == static_cast<long>(key)) {
				buffer[static_cast<size_t>(n)].polyphonyGroup = kDrumPadGroup + slot;
				synth.drum_->notePads[static_cast<size_t>(n)] = static_cast<uint8_t>(slot);
				++n;
			}
		}
		synth.drum_->notePadCount = n;
		return n;
	}
	// One voice per note: v1 has no unison, so a note is never more than one oscillator.
	buffer[0].polyphonyGroup = kGroup;
	return 1;
}

int32_t Synth::Responder::initializeMultipleVoices(
    int32_t voices,
    const typename sst::voicemanager::VoiceInitInstructionsEntry<VoiceManagerConfig>::buffer_t&
        instructions,
    typename sst::voicemanager::VoiceInitBufferEntry<VoiceManagerConfig>::buffer_t& working,
    uint16_t, uint16_t channel, uint16_t key, int32_t noteId, float velocity, float retune) {
	(void)velocity; // Velocity is not wired to anything in v1 — there is no velocity destination yet.

	int32_t created = 0;
	for (int32_t i = 0; i < voices; i++) {
		const auto index = static_cast<size_t>(i);
		if (instructions[index].instruction
		    == sst::voicemanager::VoiceInitInstructionsEntry<VoiceManagerConfig>::Instruction::SKIP) {
			working[index].voice = nullptr;
			continue;
		}

		Voice* voice = synth.claimFreeVoice();
		if (voice == nullptr) {
			// Every slot busy, including the spares held for steal fades. Reporting the shortfall is
			// the honest answer: the manager takes it as the note not sounding rather than being handed
			// a voice that is still finishing someone else's.
			working[index].voice = nullptr;
			continue;
		}

		// ANALOG KEEPS THIS VOICE'S OWN PHASE ACROSS THE RESET, and only the phase: the decimator
		// state, the noise stream and the fade schedule all belong to the note that just ended and
		// must go. So the phase is lifted out, reset() does its work, and the phase is put back —
		// which is a free-running VCO in a voice pool, the thing "phase absolute" names.
		const double standingPhase = voice->osc.phase();
		// AND EVERY STACKED PART'S, each its own free-running oscillator (backlog KE); part 0 is the line above.
		std::array<double, kMaxStack> standingPartPhases {};
		for (int part = 0; part < kMaxStack; ++part) {
			standingPartPhases[static_cast<size_t>(part)] = synth.partOsc(*voice, part).phase();
			if (part > 0) {
				synth.partOsc(*voice, part).reset();
			}
		}
		voice->osc.reset(); // Phase to zero, noise back to this slot's seed, no fade inherited.
		// THE NOTE'S OWN DRAWS (step 3): Random Fresh's seed and Drift's noise stream.
		// **WHILE THE SONG PLAYS, FROM THE NOTE AND WHERE IN THE SONG IT STARTS** (2026-10-03, Q9: consistent
		// through the track): hash(key, channel, song tick), so the same note at the same place draws the same values on
		// every pass — after a loop, a seek, or a different run of notes before it. With no transport (or a stopped one),
		// one engine stream in note-on order, as before: every note differs and a bounce from the top repeats.
		const int64_t songTick = synth.songTickNow();
		voice->driftSongTick = songTick;
		if (songTick >= 0) {
			uint64_t z = (static_cast<uint64_t>(songTick) << 16) ^ (static_cast<uint64_t>(static_cast<uint16_t>(key)) << 8)
			             ^ static_cast<uint64_t>(static_cast<uint8_t>(channel)) ^ 0x9E3779B97F4A7C15ull;
			// splitmix64's finaliser: every input bit reaches every output bit.
			z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
			z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
			z ^= z >> 31;
			voice->freshSeed = static_cast<uint32_t>(z);
			voice->driftRng = static_cast<uint32_t>(z >> 32) | 1u; // xorshift's stream never starts from zero
		}
		else {
			for (uint32_t* draw : { &voice->freshSeed, &voice->driftRng }) {
				uint32_t& s = synth.stackSeedState_;
				s ^= s << 13;
				s ^= s >> 17;
				s ^= s << 5;
				*draw = s;
			}
		}
		// THE NOTE'S STACK, AS STACK SAYS NOW: every part at its place in the fan and at its share of the level,
		// with no walk in flight — a new note is not a re-space.
		synth.seatStack(*voice, synth.params_.stackCount());
		// AND NO RESONANT TAIL FROM WHATEVER THIS SLOT WAS PLAYING BEFORE — every per-voice device of
		// it, not just the filter. `reset` means "as if just switched on", so HARM's oscillator does
		// not portamento up from the last note's frequency either.
		synth.lane_.resetVoice(static_cast<size_t>(voice->slot));
		synth.clearGainRing(voice->slot);
		const OscMode mode = synth.oscMode();
		// THE FRESH-VOICE HALF OF §5.8: a voice replacing a sounding one — a mono retrigger, any
		// steal — adopts the DYING voice's phase and bends home over ~24 cycles, so the steal
		// crossfade crosses two nearly identical waveforms instead of two uncorrelated ones. ONE
		// slot, consumed by the first voice of the transaction and cleared whatever the mode, so a
		// Digi or Analog patch leaks no state. A note with nothing sounding takes the plain phase-0
		// start with its attack from silence, exactly as always.
		//
		// THE OTHER TWO MODES DELIBERATELY IGNORE THE STOLEN PHASE. Digi wants the hard edge, and
		// Analog's phase is per-oscillator: the dying voice's phase belongs to a DIFFERENT
		// oscillator, so adopting it would be continuity — which is Catchup's job, not Analog's.
		if (synth.haveStolenPhase_) {
			if (mode == OscMode::Catchup) {
				voice->osc.adoptPhase(synth.stolenPhase_);
				voice->osc.retriggerPhase();
				// Part k from part k of the voice it replaces — the same handoff, per oscillator.
				for (int part = 1; part < voice->stackCount; ++part) {
					OscVoice& osc = synth.partOsc(*voice, part);
					osc.adoptPhase(synth.stolenPartPhases_[static_cast<size_t>(part)]);
					osc.retriggerPhase();
				}
			}
			synth.haveStolenPhase_ = false;
		}
		voice->channel = static_cast<int16_t>(channel);
		voice->key = static_cast<int16_t>(key);
		// **A MONO RETURN COMES WITH NO ID** — the voice manager starts the voice for the held key it returns to
		// with -1 — so the voice takes the id that key went down with, and answers to that note (see `heldNotes_`).
		voice->noteId = noteId != -1 ? noteId : synth.heldNoteIdOr(voice->channel, voice->key, -1);
		voice->retuneSemitones = static_cast<double>(retune);
		// **A FRESH NOTE IS UNEXPRESSED**, and this line is what makes that true of a REUSED voice.
		// The pool hands the same `Voice` back, so a slide left on it by the note before would be
		// inherited by this one — silently, and only on the notes that happened to land on a voice
		// somebody had bent.
		voice->expressionSemitones = 0.0;
		voice->appliedExpressionSemitones = 0.0;
		// AND EVERY OTHER EXPRESSION WITH IT, for exactly the same reason — a reused voice must not
		// inherit the pressure or the brightness the note before it was carrying. One line rather than
		// seven, and it cannot fall behind `kNoteExpressionCount`.
		voice->expression.fill(0.0);
		voice->levelMod = 0.0;
		voice->level = 0.0f;
		voice->haveTarget = false;

		const double target = synth.targetSemitonesFor(*voice);
		const double glide = synth.glideSecondsNow();
		// EVERY PART THE SAME WAY, EACH AT ITS PLACE IN THE FAN (backlog KE): the offset is added to both ends of
		// a retrigger's glide, so the whole stack slides as one. Part 0 of a stack of one is the voice exactly.
		for (int part = 0; part < voice->stackCount; ++part) {
			OscVoice& osc = synth.partOsc(*voice, part);
			Voice::StackPart& p = voice->parts[static_cast<size_t>(part)];
			const double partTarget = synth.partTargetFor(*voice, target, part);
			osc.setGlideSeconds(glide);
			if (glide > 0.0 && instructions[index].fromPlayingVoice) {
				// A mono retrigger: this voice is replacing one that was already sounding, and the manager
				// hands over the key it was on. Starting there and gliding to the new note is what makes
				// portamento work across a retrigger — starting at the new note would glide from nowhere.
				osc.setPitch(synth.partTargetFor(*voice,
				                                 synth.semitonesForKey(
				                                     static_cast<int16_t>(instructions[index].continuationData),
				                                     voice->channel, voice->retuneSemitones),
				                                 part));
				osc.glideToPitch(partTarget);
			}
			else {
				osc.setPitch(partTarget);
			}
			p.lastTargetSemitones = partTarget;
			p.haveTarget = true;
		}
		voice->lastTargetSemitones = target;
		voice->haveTarget = true;

		// ANALOG, AND IT IS THE SONG'S PHASE NOW RATHER THAN THIS SLOT'S (V27). Placed here rather
		// than beside the reset because it needs the note's TARGET pitch, which is settled just
		// above — and the target is deliberately what it locks to, since under glide the oscillator
		// starts at whatever key was playing before, which is history.
		//
		// WITH NO TRANSPORT it falls back to what Analog always did: keep this voice slot's own
		// phase across the reset. Not phase zero — that would be Digi wearing Analog's name — and
		// not an error either, since a host is entitled to offer no transport at all.
		if (mode == OscMode::Analog) {
			const std::optional<double> locked = synth.lockedPhaseFor(target);
			voice->osc.adoptPhase(locked.value_or(standingPhase));
			// Each other part locks at ITS OWN pitch — the song's phase for that frequency — or keeps its own
			// free-running phase, exactly as part 0 does.
			for (int part = 1; part < voice->stackCount; ++part) {
				const std::optional<double> partLocked =
				    synth.lockedPhaseFor(synth.partTargetFor(*voice, target, part));
				synth.partOsc(*voice, part).adoptPhase(
				    partLocked.value_or(standingPartPhases[static_cast<size_t>(part)]));
			}
		}

		beginFade(*voice, Voice::Stage::Attack, synth.attackEdgeSeconds_, synth.sampleRate_,
		          synth.nextNoteSerial_++);
		voice->level = 0.0f; // beginFade sets the rate; an attack starts from silence.
		synth.pluckStrings(*voice); // KS: a note that starts or re-strikes plucks its strings (a legato move never comes here)
		// DRUM: the pad this voice is (Kit: the transaction's, in order; Pitched: pad 0), struck, and its chokes owed.
		const uint8_t pad = index < static_cast<size_t>(synth.drum_->notePadCount) ? synth.drum_->notePads[index] : 0;
		synth.strikeDrums(*voice, pad);
		if (synth.drum_->startedCount < static_cast<int>(kMaxSounds)) {
			synth.drum_->started[static_cast<size_t>(synth.drum_->startedCount)] = voice;
			synth.drum_->startedPads[static_cast<size_t>(synth.drum_->startedCount)] = pad;
			++synth.drum_->startedCount;
		}
		synth.startVoiceSources(*voice); // backlog FF: the new note's routed values from its first sample

		working[index].voice = voice;
		created++;
	}
	// **A VOICE STARTED OUTSIDE ANY NOTE-ON IS MONO RETURNING TO A HELD KEY**: the
	// note above it was let go. Mono is the retrigger mode, so the held note's envelope restarts with the voice,
	// where Legato resumes it where it stands (`moveVoice`). The lane never hears a return otherwise.
	if (created > 0 && !synth.pendingNoteOn_.valid && synth.params_.voiceMode() == VoiceMode::Mono) {
		for (int32_t i = 0; i < voices; i++) {
			if (const Voice* v = working[static_cast<size_t>(i)].voice; v != nullptr) {
				synth.retriggerLaneNote(*v);
				break;
			}
		}
	}
	return created;
}

void Synth::Responder::endVoiceCreationTransaction(uint16_t, uint16_t, uint16_t, int32_t, float) {}

void Synth::Responder::terminateVoice(Voice* voice) {
	if (voice == nullptr || voice->stage == Voice::Stage::Idle) {
		return;
	}
	// The manager is told now, not when the fade ends. It counts voices against the polyphony limit,
	// and a voice it still counted while it faded would cost the note that stole it its own slot.
	if (synth.voiceEnded_) {
		synth.voiceEnded_(voice);
	}
	synth.hostNoteEnded(voice->channel, voice->key, voice->noteId, voice, false); // before the id is given up
	// **THE NOTE LANE IS TOLD THE NOTE IS OVER**. This voice ends without the host's note-off — a steal, a
	// choke, a cut, the polyphony lowered — and the host, told the note ended, may never send one. A
	// per-note device (the DAHDSR's table) would keep the note gated at sustain forever, and any voice
	// that later read that entry would be held open by it. Said BEFORE the id is given up, while the
	// voice still names the note the lane knows.
	synth.chokeLaneNote(*voice);
	// The dying voice's running phase, banked for the note that is stealing it (§5.8): the manager
	// terminates strictly before it creates within one note-on, so the replacement can start WHERE
	// THE SOUND IS and bend home instead of cutting across it.
	synth.stolenPhase_ = voice->osc.phase();
	for (int part = 0; part < kMaxStack; ++part) {
		synth.stolenPartPhases_[static_cast<size_t>(part)] = synth.partOsc(*voice, part).phase();
	}
	synth.haveStolenPhase_ = true;
	voice->noteId = -1;
	beginFade(*voice, Voice::Stage::Stealing, kStealFadeSeconds, synth.sampleRate_, 0);
}

void Synth::Responder::releaseVoice(Voice* voice, float velocity) {
	(void)velocity; // Release velocity has no destination in v1.
	if (voice == nullptr || !voice->isSounding() || voice->stage == Voice::Stage::Stealing) {
		return;
	}
	// THE FINGER (KS K9): a released string is damped toward Release's T60; a re-strike lifts it (pluckStrings).
	synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.releaseString(true); });
	// THE DRUM'S TRIGGER (drum proposal D4): a One-shot pad plays its whole hit whatever the key does, so its voice is not
	// released at all (the drum road ends it at exact silence); a Gate pad damps its hit to Release and releases as ever.
	if (synth.params_.oscType() == OscType::Drum) {
		synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.releaseDrum(); });
		const size_t pad = synth.params_.drumKit() ? voice->osc.drumPad() : 0;
		if (steppedIndex(synth.params_.resolved(drumParam(pad, DrumControl::Trigger)), kDrumTriggerNames.size()) == 0) {
			voice->oneShotLetGo = true; // ended by the drum road at silence, or off Drum by `letGoOffDrum`
			return;
		}
	}
	beginFade(*voice, Voice::Stage::Release, synth.releaseEdgeSeconds_, synth.sampleRate_,
	          0);
}

void Synth::Responder::retriggerVoiceWithNewNoteID(Voice* voice, int32_t noteId, float) {
	if (voice == nullptr) {
		return;
	}
	// THE OLD NOTE ENDS HERE WITHOUT AN OFF — the host retriggers under a new id and will not lift
	// the old one — so the lane is told, as `terminateVoice` tells it. The new id's own note-on
	// reaches the lane through the ordinary road and starts its own entry.
	if (voice->noteId != noteId) {
		synth.chokeLaneNote(*voice);
		if (voice->noteId >= 0) {
			synth.hostNoteEnded(voice->channel, voice->key, voice->noteId, voice, true); // the old id; the key lives on
		}
	}
	voice->noteId = noteId;
	// The old note's per-note modulation does not carry over to the new one. It was addressed to a
	// note id this voice no longer answers to, and leaving it would let a released note's expression
	// set the level of whatever is played next on the same key.
	voice->levelMod = 0.0;
	// THE SAME-VOICE HALF OF §5.8, and the road where all three modes are visibly different: the
	// phase is already continuous here — what it lacked was the RESTART character. Catchup aims it
	// home without jumping (the firmware's own retrigger semantics, and a named behaviour change: a
	// default-patch mono restrike bends briefly toward phase 0 where it used to play on unmoved).
	// Digi snaps. Analog does nothing at all, which is the mode stated exactly.
	switch (synth.oscMode()) {
	case OscMode::Digi: synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.restartPhase(); }); break;
	case OscMode::Catchup: synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.retriggerPhase(); }); break;
	case OscMode::Analog: synth.lockPhaseToSong(*voice); break;
	case OscMode::kCount: break;
	}
	// AND THE MODULATOR RESTARTS IN ALL THREE, outside the switch because it is not one of the three
	// choices. FM doc §2 measured it: 64 note-ons differing only in modulator phase came out 2.40
	// apart at worst over the first 10.7 ms, against a full-scale swing of 2.0. A note that sounds
	// different every time you play it is not a mode, and Digi's "retrigs the osc for every note" is
	// simply not met without this.
	synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.restartModulator(); });
	// AND DAMP: a fresh non-legato pluck starts with every partial undecayed (spectral-ops Damp review, 2026-10-12).
	synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.restartDamp(); });
	beginFade(*voice, Voice::Stage::Attack, synth.attackEdgeSeconds_, synth.sampleRate_,
		          synth.nextNoteSerial_++);
	synth.pluckStrings(*voice); // KS: a note that starts or re-strikes plucks its strings (a legato move never comes here)
	synth.strikeDrums(*voice, voice->osc.drumPad()); // DRUM: a re-strike strikes the same pad again
	synth.startVoiceSources(*voice); // backlog FF
}

void Synth::Responder::moveVoice(Voice* voice, uint16_t, uint16_t channel, uint16_t key, float) {
	if (voice == nullptr) {
		return;
	}
	if (voice->channel != static_cast<int16_t>(channel) || voice->key != static_cast<int16_t>(key)) {
		// THE KEY IT LEAVES IS OVER for the host (a legato move keeps one voice for two notes).
		synth.hostNoteEnded(voice->channel, voice->key, voice->noteId, voice, false);
	}
	voice->channel = static_cast<int16_t>(channel);
	voice->key = static_cast<int16_t>(key);
	voice->haveTarget = false; // The next slice retargets the oscillator.
	voice->oneShotLetGo = false; // moved to a key that is down (a new note, or a legato return to a held one)
	// **AND THE VOICE ANSWERS TO THE NOTE IT MOVED TO** (backlog FG). A legato move carries no note id, so the
	// voice kept the old note's — and read that note's per-note envelope, following its release to the end
	// while the new note's own envelope ran unread (the envelope seemed to hold on until the entire env
	// had run). The incoming note is the one being dispatched.
	if (synth.pendingNoteOn_.valid && synth.pendingNoteOn_.channel == static_cast<int16_t>(channel)
	    && synth.pendingNoteOn_.key == static_cast<int16_t>(key)) {
		voice->noteId = synth.pendingNoteOn_.noteId;
		// A legato note change the manager ACCEPTED: the note moved to plays, so it plucks.
		synth.pluckForNoteOn();
	}
	else {
		// **A LEGATO RETURN**: the note above was let go and the voice goes back to a key still
		// held. It answers to THAT note now. MEASURED before, with note ids: the voice kept the released note's id,
		// so a per-note modulator matched nothing at (released id, held key) and the voice read the device's mono
		// shape — restarted by the interstitial note, and a different envelope from the one keyboard ids (-1, found
		// by key) read for the same playing.
		voice->noteId = synth.heldNoteIdOr(static_cast<int16_t>(channel), static_cast<int16_t>(key), voice->noteId);
	}
}

void Synth::Responder::moveAndRetriggerVoice(Voice* voice, uint16_t port, uint16_t channel,
                                             uint16_t key, float velocity) {
	// A mono move WITH retrigger: the same-voice dispatch applies exactly as in
	// retriggerVoiceWithNewNoteID. LEGATO IS EXPLICITLY OUT (by design): a legato move goes through
	// moveVoice alone — no retrigger, no seed, the phase just keeps running — because no retrigger
	// character is the mode's whole point, and the pin holds it bit-exact in EVERY mode. That is a
	// stronger claim than the two-state one it replaces: legato is the road where Analog and a
	// gated move must be indistinguishable, and they are, because neither touches the phase.
	moveVoice(voice, port, channel, key, velocity);
	if (voice != nullptr) {
		// **AT THE NEW NOTE AT ONCE: A RELEASED KEY WAS NOT OVERLAPPED, SO NOTHING GLIDES**. The manager takes this road exactly when the voice it moves is no longer
		// gated — its key was let go (and no pedal holds it) before this note arrived, and the voice is only sounding
		// its tail. Glide is what an OVERLAP means, so this is a fresh note. Left to `moveVoice`, the next slice's
		// `glideToPitch` carried it from the released key at the glide time — measured 60.01 at the onset of a C5
		// and 67.59 one time constant later. The same arithmetic `applyPitches` uses, settled here so it agrees.
		const double target = synth.targetSemitonesFor(*voice);
		for (int part = 0; part < kMaxStack; ++part) {
			Voice::StackPart& p = voice->parts[static_cast<size_t>(part)];
			if (!p.live) {
				continue;
			}
			const double partTarget = synth.partTargetFor(*voice, target, part);
			synth.partOsc(*voice, part).setPitch(partTarget);
			p.lastTargetSemitones = partTarget;
			p.haveTarget = true;
		}
		voice->lastTargetSemitones = target;
		voice->haveTarget = true;
		switch (synth.oscMode()) {
		case OscMode::Digi: synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.restartPhase(); }); break;
		case OscMode::Catchup: synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.retriggerPhase(); }); break;
		case OscMode::Analog: synth.lockPhaseToSong(*voice); break;
		case OscMode::kCount: break;
		}
		// The modulator restarts here too, and for the reason the sibling dispatch gives. Legato is
		// still out: it never reaches this branch, so no retrigger character of any kind applies.
		synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.restartModulator(); });
		synth.forEachLivePart(*voice, [](OscVoice& osc) { osc.restartDamp(); }); // and Damp, as above
		beginFade(*voice, Voice::Stage::Attack, synth.attackEdgeSeconds_, synth.sampleRate_,
		          synth.nextNoteSerial_++);
		synth.pluckStrings(*voice); // KS: a note that starts or re-strikes plucks its strings (a legato move never comes here)
		synth.strikeDrums(*voice, voice->osc.drumPad()); // DRUM: a re-strike strikes the same pad again
		synth.startVoiceSources(*voice); // backlog FF
	}
}

void Synth::Responder::discardHostVoice(int32_t) {
	// v1 has no host-side voice-info extension, so there is no host voice to discard.
}

void Synth::Responder::setNoteExpression(Voice* voice, int32_t expression, double value) {
	// **TUNING IS TAKEN AND THE REST ARE STILL IGNORED — backlog DS,
	//
	// This whole function was a no-op under a note reading *"v1 declares no note expressions, and a
	// host that sends them anyway gets a synth that keeps playing rather than one that misinterprets
	// them"* — which was the right call while nothing consumed them, and stopped being one the day
	// the request arrived. The plumbing was already complete: the manager routes by
	// (port, channel, key, note_id) with CLAP's -1 wildcards, and `voicemanager_constraints.h`
	// REQUIRES this method, so the only thing missing was a body.
	//
	// **ALL SEVEN ARE NOW KEPT, AND THAT IS NOT THE SAME AS ALL SEVEN BEING WIRED.** This used to take
	// TUNING and drop the rest, under a note saying the others *"each need a decision about what they
	// map to on THIS instrument ... wiring them blind would be inventing behaviour"*. **That reasoning
	// still stands and is not being overturned** — it is about what an expression DOES, and nothing
	// here decides that. What changes is that the values are no longer thrown away before anybody can
	// ask for them: they become per-voice modulation SOURCES, and a patch decides what each one
	// reaches. The decision moves from us to the player, which is where DU says it belongs.
	if (voice == nullptr || !std::isfinite(value)) {
		// **NOT FINITE IS NOT AN EXPRESSION.** The value crosses from the host and lands in arithmetic
		// the oscillator and the modulation resolve both read; a NaN would poison the voice's target
		// for the rest of the note, and `glideToPitch` would carry it into the phase accumulator.
		return;
	}
	// **BY CLAP'S OWN ID, so a host's number is the index and nothing maps between two numberings.**
	// An id this build does not know is dropped rather than clamped into a neighbour's slot — a
	// future CLAP expression must not arrive as a wrong one.
	if (expression >= 0 && static_cast<size_t>(expression) < kNoteExpressionCount) {
		// ASSIGNED, NOT ACCUMULATED. Every CLAP note expression is an absolute reading of a continuous
		// controller, so the last value received IS the current one. Adding would ratchet.
		voice->expression[static_cast<size_t>(expression)] = value;
	}
	// AND TUNING KEEPS ITS OWN FIELD, which is the PITCH road and is a different consumer from the
	// modulation one above — see `Voice::expression`. Both are written here so they cannot disagree
	// about what the host last said.
	if (expression == CLAP_NOTE_EXPRESSION_TUNING) {
		// SEMITONES, ABSOLUTE IN THE FIELD AND RELATIVE IN MEANING: CLAP's TUNING is an offset from
		// the note's own pitch, so the last value received IS the current bend.
		// CLAMPED TO TWENTY OCTAVES EITHER WAY: `pitchToHz` is an exp2, so a finite but absurd bend
		// (past roughly twelve thousand semitones) becomes an infinite frequency and a phase
		// increment that never recovers. No instrument reaches 240 semitones from its note.
		voice->expressionSemitones = std::clamp(value, -240.0, 240.0);
	}
}

void Synth::Responder::setVoicePolyphonicParameterModulation(Voice* voice, uint32_t parameter,
                                                             double value) {
	if (voice == nullptr || parameter != static_cast<uint32_t>(ParamId::VoiceLevel)) {
		// Voice Level is the only param declared CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID. Anything else
		// addressed to a note is dropped rather than folded into the global lane, which belongs to the
		// host's automation and would then be wrong for every other voice.
		return;
	}
	if (!std::isfinite(value)) {
		// NOT FINITE IS NOT A MODULATION: `levelMod` is added to the base Level and `std::clamp`
		// passes a NaN straight through to the voice gain, where it would stay for the note.
		return;
	}
	voice->levelMod = value;
}

void Synth::Responder::setVoiceMonophonicParameterModulation(Voice* voice, uint32_t parameter,
                                                             double value) {
	setVoicePolyphonicParameterModulation(voice, parameter, value);
}

void Synth::Responder::setPolyphonicAftertouch(Voice*, int8_t) {}
void Synth::Responder::setVoiceMIDIMPEChannelPitchBend(Voice*, uint16_t) {}
void Synth::Responder::setVoiceMIDIMPEChannelPressure(Voice*, int8_t) {}
void Synth::Responder::setVoiceMIDIMPETimbre(Voice*, int8_t) {}

void Synth::MonoResponder::setMIDIPitchBend(int16_t channel, int16_t bend14bit) {
	synth.pitchBend14(channel, bend14bit);
}

void Synth::MonoResponder::setMIDI1CC(int16_t, int16_t, int8_t) {
	// v1 has no MIDI learn and no CC destinations; the sustain pedal reaches the manager through
	// updateSustainPedal rather than through here.
}

void Synth::MonoResponder::setMIDIChannelPressure(int16_t, int16_t) {}

} // namespace feathers

namespace feathers {
int Synth::newestVoiceSlot() const {
	int best = -1;
	uint64_t serial = 0;
	for (const Voice& v : voices_) {
		if (v.isSounding() && v.stage != Voice::Stage::Stealing && (best < 0 || v.startSerial > serial)) {
			best = v.slot;
			serial = v.startSerial;
		}
	}
	return best;
}

int Synth::newestVoiceSlotForTest() const {
	return newestVoiceSlot();
}

void Synth::plotFrame(SpectralPlotFrame& out) const {
	out = SpectralPlotFrame {};
	if (params_.oscType() != OscType::Additive) {
		return;
	}
	const auto finish = [&out](int count, double f0, bool playing) {
		out.count = count;
		out.f0 = static_cast<float>(f0);
		out.playing = playing ? 1 : 0;
		out.stereo = std::equal(out.left.begin(), out.left.begin() + count, out.right.begin()) ? 0 : 1;
	};
	// THE NEWEST VOICE, AS ITS BANK RENDERS IT: the newest because it is the one the player just asked for.
	if (const int slot = newestVoiceSlot(); slot >= 0) {
		const OscVoice& osc = voices_[static_cast<size_t>(slot)].osc;
		const int count = osc.plotPartials(out.ratio.data(), out.terrain.data(), out.left.data(), out.right.data());
		if (count > 0) {
			finish(count, osc.frequencyHz(), true);
			return;
		}
	}
	// NO NOTE: what the next note on the main bank would sound at the last one's pitch, through the drawer it would get. The
	// last note is the voice started last, sounding or not (`startSerial` only climbs; a voice keeps its pitch when it
	// falls idle); before any, A2.
	double f0 = 110.0;
	uint64_t latest = 0;
	for (const Voice& v : voices_) {
		if (v.startSerial > latest) {
			latest = v.startSerial;
			f0 = v.osc.frequencyHz();
		}
	}
	const spectral::ShaperParams drawer = drawerFor(-1, -1, false);
	const spectral::ShaperParams drawerR = drawerFor(-1, -1, true);
	const int count = OscVoice::plotReference(bank_.current(0), bank_.current(1), f0, sampleRate_, plotCache_, plotCacheR_,
	                                          drawer, drawerR == drawer ? nullptr : &drawerR, out.ratio.data(),
	                                          out.terrain.data(), out.left.data(), out.right.data(),
	                                          drawer.vocode.on ? &vocodeEnv_ : nullptr); // OE: vocoded as a voice is now
	finish(count, f0, false);
}
} // namespace feathers

namespace feathers {

void Synth::clearGainRing(int slot) noexcept {
	if (gainRingCap_ == 0 || slot < 0 || slot >= kVoiceSlots) {
		return;
	}
	float* ring = gainRing_.data() + static_cast<size_t>(slot) * gainRingCap_;
	std::fill_n(ring, gainRingCap_, 0.0f);
	gainWrite_[static_cast<size_t>(slot)] = 0;
	Voice& v = voices_[static_cast<size_t>(slot)];
	v.draining = false;
	v.drainLeft = 0;
	v.stealFade = 1.0f;
}

float Synth::delayedGain(Voice& v, float* ring, float now) noexcept {
	// THE GAIN RUN `gainDelay_` SAMPLES LATE, so it meets the audio the per-voice devices delayed by as much. A STEAL is the
	// exception: its fade is applied to the output AT ONCE (the slot is wanted back at the usual time), so while Stealing the
	// ring holds the last gain before the steal and the fade multiplies what comes out.
	const size_t mask = gainRingCap_ - 1;
	size_t& w = gainWrite_[static_cast<size_t>(v.slot)];
	const bool stealing = v.stage == Voice::Stage::Stealing;
	ring[w] = stealing ? ring[(w - 1) & mask] : now;
	float out = ring[(w - gainDelay_) & mask];
	w = (w + 1) & mask;
	if (stealing) {
		out *= v.stealFade;
		v.stealFade = std::max(0.0f, v.stealFade - v.step);
	}
	return out;
}

} // namespace feathers

namespace feathers {

void Synth::settleLaneBeforeEvents() noexcept {
	// THE SAME CONVERGE `applyParamsRoad` RUNS, and the same structure count, so its plan rebuild (which reads the count, not
	// whether this call moved anything) sees the installed devices; its own converge is then skipped once (`laneSettled_`).
	// ALREADY SETTLED, with no render since (a block the host gave no buffers): nothing more — a second `finish` would end a
	// segment that is not in flight.
	if (laneSettled_) {
		return;
	}
	const bool moved = chainsFromPublish_ ? lane_.converge(chainState_, chainsSequence_) : lane_.converge(fxChain_, rack_);
	if (moved) {
		++structureRevision_;
	}
	laneSettled_ = true; // `render`'s converge is this one: skipped once
}

} // namespace feathers
