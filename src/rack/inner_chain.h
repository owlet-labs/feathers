/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/axes.h"
#include "device/latency.h"
#include "device/registry.h"
// FOR `kMaxDepth` AND `kMaxBodyBytes`, WHICH ARE THE RACK FORMAT'S AND NOT THE RACK DEVICE'S: a chain
// held here saves exactly what a rack saves, so a second copy of either number would be a second
// answer to one question. The externs this header also declares cost nothing.
#include "rack/rack_device.h"
#include "rack/rack_engine.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// **A CHAIN A DEVICE HOLDS, AS ONE UNIT** — the engine, the editing bridge, the drain, the chunking
// and the wet/dry ramp, packaged so that a device which wants an inner effects chain fills its
// `device::ContainerOps` by forwarding rather than by writing three hundred lines again.
//
// **WHY IT EXISTS, AND IT IS NOT A REFACTOR OF `rack_device.cpp`.** That file IS this code today —
// `rack::kDeviceAudioOps` + `kDeviceContainerOps`, a hand-written wrapper around `RackEngine` +
// `RackBridge` — and none of it was factored out, so the SECOND device to want a chain copies it.
// That makes it the most valuable extraction available: the drain placement alone is four interacting
// booleans whose comments record two prior wrong versions, and a second container getting it NEARLY
// right produces a device that works until a host uses the two-stage road. This will be a common
// pattern.
//
// **`rack_device.cpp` IS DELIBERATELY NOT PORTED ONTO THIS YET.** Doing so would change live drain
// semantics in the shipped rack before this unit has a second caller in anyone's hands. The two are
// expected to converge; until they do, a change to the drain logic belongs in BOTH, and the cases in
// `tests/test_inner_chain.cpp` pin this one against the behaviour `rack_device.cpp` documents.
//
// **WHAT IT IS NOT.** It is not a second modulation system, it is not a "chain in chain" concept, and
// it holds exactly ONE chain. A device wanting two must invent framing for N bodies inside the single
// `ContainerOps::saveState` (gap G2) and a lane argument for the route verbs (gap G3); neither is
// designed, and this unit deliberately does not pretend to anticipate them.

namespace feathers::rack {

class RackBridge;

/// **HOW DEEP THE LOAD IN PROGRESS IS, COUNTED ONCE FOR EVERY KIND OF CONTAINER.** Loading is
/// recursive — a body holds chains whose bodies hold chains — and runs on one thread, so a counter on
/// that thread is the whole of the guard.
///
/// **IT IS A FREE FUNCTION AND NOT A MEMBER FOR THE REASON GAP G4 GIVES.** `rack_device.cpp` keeps a
/// `thread_local loadDepth` of its OWN, so two DIFFERENT container kinds alternating (A → B → A → …)
/// each see depth 1 at every level and neither guard ever fires — *"even two correct copies do not
/// compose"*. One counter that every container increments composes by construction. `rack_device.cpp`
/// is deliberately not ported onto it yet (see the banner); the day it is, that hole closes with it.
[[nodiscard]] size_t& containerLoadDepth() noexcept;

/// **THE CHAIN, AS ITS HOLDER SEES IT.** One object per holding device instance, living in the
/// device's own storage. Every method below is the body of one `AudioOps` or `ContainerOps` entry;
/// a holder's table is a set of one-line forwards.
///
/// THREADS, and they are the rack's own: everything under MESSAGE THREAD is called where the host
/// provisions, and everything under AUDIO THREAD between `beginPerVoiceBlock` (when the host has one)
/// and the end of `process`. `adopt` allocates; `prepare` sizes the scratch; nothing else allocates,
/// which is trap T2 — a chain built lazily inside a process call is a click under load rather than a
/// crash.
class InnerChain final {
public:
	/// **BUILT WITH NO CATALOGUE**, because whoever builds a device constructs it before it can say
	/// what devices exist; `adopt` hands one over before anything is added (trap T5 — `adopt` is the
	/// only moment a container may build its record, and a bridge built in `construct` holds an empty
	/// catalogue and offers nothing, with no error).
	///
	/// `axes` is COPIED (into the engine, which holds its own by value), so a caller may pass a
	/// temporary. Its `coreSources` are the holder's own published values — see `publishCoreSource`.
	///
	/// **`sharedIds` IS THE HOLDER'S INSTANCE COUNTER, OR NULL FOR A CHAIN THAT COUNTS ALONE** — gap
	/// G3, and it is the ONE thing a holder of TWO chains must remember to pass. Each chain otherwise
	/// owns its counter and starts at `kFirstInstance`, so two of them name their first device
	/// identically; `ContainerOps`' route verbs carry a `device::Destination{deviceId, index}` with no
	/// lane argument, so the holder dispatches on the id alone and — measured before the fix — BOTH
	/// chains answered `descriptorOf(1)` non-null. One counter per HOLDER (not per tree: there is no
	/// seam by which a parent's counter could arrive, since `ContainerOps::adopt` hands over a
	/// `Catalogue` and nothing else) makes the id unambiguous, which is exactly what those four verbs
	/// need and no more.
	///
	/// **IT IS BORROWED, SO IT MUST OUTLIVE THIS CHAIN — and a holder that is MOVABLE must not keep it
	/// as a plain member**, or the chain is left pointing into a moved-from device. `Sklitter` holds
	/// its counter behind a `unique_ptr` for that reason, and its move constructor is why.
	explicit InnerChain(const backplane::Axes& axes, backplane::InstanceCounter* sharedIds = nullptr);
	~InnerChain();

	InnerChain(const InnerChain&) = delete;
	InnerChain& operator=(const InnerChain&) = delete;

	//--------------------------------------------------------------------------------------------//
	// MESSAGE THREAD                                                                               //
	//--------------------------------------------------------------------------------------------//

	/// `ContainerOps::adopt`. Builds the bridge — the only place it may be built.
	void adopt(const device::Catalogue& catalogue);
	/// `AudioOps::prepare`. Sizes the scratch (to the host's longest block) and the ramp; safe before or after `adopt`.
	void prepare(double sampleRate, int maxFrames);
	/// `AudioOps::reset`. Drops the inner tails and settles the ramp where it was aimed.
	void reset() noexcept;
	/// `ContainerOps::setVoices` — the pass-through model: the holder's voices are the chain's.
	void setVoices(size_t voices);
	/// `ContainerOps::setOuterGate`. Remembered when it arrives before `adopt` built the bridge.
	void setOuterGate(const device::OuterGate& gate);
	/// `ContainerOps::setHostSlots` (host slots T11) — the holder's link, with this chain's `lane` stamped on
	/// it. Remembered when it arrives before `adopt` built the bridge.
	void setHostSlots(const device::HostSlotLink& link);
	/// `ContainerOps::service` — reap, and drop inner routes the outer gate no longer admits.
	device::RouteDrop service();
	/// `ContainerOps::saveState`.
	bool saveState(const device::ByteSink& sink) const;
	/// `ContainerOps::loadState`. Validates all-or-nothing and refuses a body deeper than `kMaxDepth`.
	bool loadState(const uint8_t* data, size_t bytes);

	/// **WOULD `loadState` TAKE THESE BYTES? — asked WITHOUT touching anything.** The validate half of
	/// `loadState`, split out so a holder of SEVERAL chains can check every lane before it applies any
	/// of them, which is the all-or-nothing `device::ContainerOps::loadState` demands of a body that
	/// spans more than one (gap G2, `rack::readChainBundle`).
	///
	/// **IT IS CHEAP UNDER NESTING, AND THAT IS NOT OBVIOUS.** `saveState`'s vocabulary comment records
	/// a 2^depth blow-up from asking twice, so asking twice here deserves an answer: `parseRack` does
	/// NOT recurse — it reads each inner container's body as an opaque blob and copies it — so this
	/// parses one level and costs 2x in total rather than 2^depth. A nested chain's own bytes are
	/// parsed twice by ITS holder, once, for the same reason.
	[[nodiscard]] bool canLoad(const uint8_t* data, size_t bytes) const;

	/// `ContainerOps::applyState` — **A BODY INTO A CHAIN THAT MAY BE RUNNING.** The record becomes the
	/// patch (`RackBridge::loadPatch`) and nothing else happens here: no drain, no publish, because a
	/// block may be inside the engine and the drain is the audio thread's (`beginPerVoiceBlock`,
	/// `process`). The same road the rack's own `stateLoad` takes while active. EMPTY BYTES empty the
	/// chain. False — nothing touched — without a bridge, or for bytes `canLoad` would refuse.
	bool applyState(const uint8_t* data, size_t bytes);

	/// `ContainerOps::forEachRouteFrom` / `routeAmountFrom` / `setRouteFrom` / `nameDestination`.
	void forEachRouteFrom(device::Source outer, void* ctx,
	                      void (*fn)(void* ctx, device::Destination inner, float amount)) const;
	[[nodiscard]] float routeAmountFrom(device::Source outer, device::Destination inner) const;
	bool setRouteFrom(device::Source outer, device::Destination inner, float amount);
	[[nodiscard]] device::Transfer routeTransferFrom(device::Source outer, device::Destination inner) const;
	bool setRouteTransferFrom(device::Source outer, device::Destination inner, device::Transfer t);
	size_t nameDestination(device::Destination inner, char* out, size_t capacity) const;

	//--------------------------------------------------------------------------------------------//
	// THE CHAIN'S OWN OUTWARD CONTROLS — the holder drives these from its own parameters           //
	//--------------------------------------------------------------------------------------------//

	/// **THE CHAIN CARRIES ITS OWN MIX** (decision D2). A holder whose dry/wet is blended per-sample
	/// and in place cannot put a chain on its WET path without a wet scratch buffer and a rewrite of
	/// every renderer's write-back; the chain's own mix costs nothing structural and does not foreclose
	/// a wet-only road later.
	///
	/// Each of these AIMS a 5 ms ramp rather than stepping — a step in a gain is a zipper, and the
	/// ramp is counted in SAMPLES so it is partition-independent (`fx::busutil`'s reasons, whose
	/// header records that a per-block ramp failed the partition sweep).
	void setEnabled(bool enabled);
	/// 0 dry, 1 the chain's output. Clamped.
	void setMix(float mix);
	/// A FADER POSITION, 0..1, unity at `composite::kFaderUnity`. Left at unity by a holder that has
	/// no such control; it is here because the ramp already carries it and a second caller should not
	/// have to re-derive the curve.
	void setGain(float position);

	//--------------------------------------------------------------------------------------------//
	// AUDIO THREAD                                                                                 //
	//--------------------------------------------------------------------------------------------//

	void setTransportClock(const device::TransportClock& clock);
	/// The WHOLE note, not only its gate: a keytracking filter inside needs the pitch.
	void setNoteContext(const device::NoteContext& note);
	/// The reader for an inner control that volunteers to read one of the OUTER host's sources.
	void setOuterSources(const device::OuterSources& outer);

	/// **THE HOLDER'S OWN PUBLISHED VALUES, AS THE CHAIN'S CORE SOURCES** (decision D3(a)).
	///
	/// **WHY THIS AND NOT `Source::outerOutput`, WHICH READS AS THE PROPER ROAD.** Trap T1, measured:
	/// `RackEngine::upstreamOf(self, Source::output(self, n))` answers FALSE for every output a device
	/// publishes into its OWN inner chain, because `Graph::reaches(from, to)` returns true when
	/// `from == to` — *"a device is not upstream of itself"* — and `wouldCycle` uses that bare form. A
	/// SIBLING earlier in the chain answers true; only the self-case is refused. So the outer road
	/// needs a carve-out in a rule that is currently uniform, and this one works today. (Since backlog
	/// HS `upstreamOf` says the self-case outright, before the cycle test — same answer, see there.)
	///
	/// `value` is the source's OWN reading: 0..1 for a unipolar source, -1..+1 for a bipolar one. The
	/// mapping onto the engine's macro cell (which stores `2v-1` for a bipolar macro) is done here, so
	/// a holder hands its published value straight in and the polarity it declared in its `SourceDesc`
	/// is the polarity the chain reads. Call `setCoreBipolar` once, at adopt, to declare it.
	void setCoreBipolar(size_t index, bool bipolar);
	void publishCoreSource(size_t index, float value);

	/// `AudioOps::beginPerVoiceBlock` — **the drain point once a host has one** (trap T3). Returns
	/// whether any inner device runs per voice.
	bool beginPerVoiceBlock();
	/// `AudioOps::processVoice` — one voice through the inner per-voice devices: both channels, or `right` null for a mono voice.
	void processVoice(size_t voice, float* left, float* right, int frames, const device::NoteContext& note);

	/// **THE BUS ROAD: DRAIN, CHUNK, RUN, BLEND, PUBLISH.**
	///
	/// Chunks are cut on `device::kTickWindow` (trap T4) so the inner modulators run on the same
	/// 64-sample grid as everything else. A naive one-call-per-host-block gives the inner chain's
	/// modulators ONE tick per host block — at 512 frames, 8x too slow and host-buffer-dependent,
	/// which is the class of bug this repo calls "the host changing the sound".
	///
	/// The publish is an RAII destructor, so EVERY return path publishes (trap T3): the inner editor
	/// must not freeze in exactly the states a player reaches.
	void process(float* left, float* right, int frames);
	/// **ONE HOST BLOCK OF MANY `process` CALLS** (the Delay's feedback lane calls once per loop chunk, sixteen times in a
	/// 1024-frame block; the cost pass, 2026-11-06). Between the two, `process` skips its per-call drain, mirror publish and
	/// latency walk: the drain and the walk happen once at the begin, the publish once at the end. Nothing audible moves —
	/// an edit is adopted at the next host block's drain, which is where the holder's own drain already adopted it. A
	/// latency that a MODULATED control changes mid-block is read once per host block here (the holder hands the loop one
	/// latency per block already, `LoopInsert::latency`). AUDIO THREAD.
	void beginHostBlock();
	void endHostBlock();
	/// THE LATENCY TAKEN AT `beginHostBlock`.
	[[nodiscard]] uint32_t hostBlockLatency() const noexcept { return hostLatency_; }

	/// **HOW LATE `process` LEAVES ITS OUTPUT** (backlog JM): what the devices inside declare, whatever the
	/// chain's own Enabled says (assumption L3) — a holder whose output this chain is in series with adds it
	/// to its own `AudioOps::latencySamples`. The chain's mix is aligned inside `process`.
	[[nodiscard]] uint32_t latencySamples(double sampleRate) const noexcept {
		return engine_.latencySamples(sampleRate);
	}
	/// **WHETHER A DEVICE INSIDE STILL HOLDS SOUND TO COME** (backlog MC) — a holder's `AudioOps::holdsTail` asks it.
	/// Bypassed and settled, nothing inside runs (`process`), so nothing frozen there can come out.
	[[nodiscard]] bool holdsTail() const noexcept {
		if (!enabled_ && remaining_ == 0 && dryNow_ == 1.0f && wetNow_ == 0.0f) {
			return false;
		}
		return engine_.holdsTail();
	}

	//--------------------------------------------------------------------------------------------//
	// SEAMS                                                                                        //
	//--------------------------------------------------------------------------------------------//

	/// The engine inside — what a case puts devices into, and what a per-voice instance has instead of
	/// a bridge.
	[[nodiscard]] RackEngine& engine() noexcept { return engine_; }
	[[nodiscard]] const RackEngine& engine() const noexcept { return engine_; }
	/// **THE EDITING ROAD**, or null before `adopt`. What a chain's expanded view edits through.
	[[nodiscard]] RackBridge* bridge() noexcept { return bridge_.get(); }
	[[nodiscard]] const RackBridge* bridge() const noexcept { return bridge_.get(); }

	/// TEST ONLY — the host clock this chain was last told about. **It exists because a holder with
	/// MORE THAN ONE chain can forward the clock to one of them and not the other, which nothing else
	/// can see**: the clock reaches no output until a tempo-synced device is inside, so the gap is
	/// silent and only shows up as a delay running at the wrong time in one lane. Found exactly that
	/// way in `Sklitter` (the grain lane's first full read); five lines, per CLAUDE.md's rule that a
	/// claim about a value is asserted rather than inferred.
	[[nodiscard]] const device::TransportClock& clockForTest() const noexcept { return clock_; }

	/// TEST ONLY — the wet and dry coefficients the last blended sample used. A claim about a mix is a
	/// claim about a VALUE, and a comparison of two renders is evidence that something changed, never
	/// that the right thing did (CLAUDE.md's rule, paid for four times).
	struct Blend {
		float wet = 0.0f;
		float dry = 0.0f;
		float voiceWet = 0.0f;
	};
	[[nodiscard]] Blend blendForTest() const noexcept { return { wetNow_, dryNow_, voiceWetNow_ }; }
	/// TEST ONLY — where the bus stage stands inside the inner control tick. `kTickWindow` chunking is
	/// invisible in the output of a chain holding nothing, so the claim is read here rather than
	/// inferred from audio.
	[[nodiscard]] int tickPhaseForTest() const noexcept { return tickPhase_; }
	/// TEST ONLY — how many inner blocks have been begun and ended since `prepare`. What "chunked on
	/// the tick window" MEANS, as a number: 512 frames is eight, not one.
	[[nodiscard]] uint64_t blocksBegunForTest() const noexcept { return blocksBegun_; }
	/// TEST ONLY — how long the scratch is (backlog GO): the bus pair and the per-voice pair, which `prepare` sizes
	/// together; 0 when they disagree, so a case cannot read one and miss the other.
	/// TEST ONLY — how far the dry can wait for the chain (backlog NR: sized to the declared latency, not a second).
	[[nodiscard]] uint32_t dryCapacityForTest() const noexcept { return dryDelay_.capacity(); }
	[[nodiscard]] size_t scratchFramesForTest() const noexcept {
		const size_t n = dryLeft_.size();
		return dryRight_.size() == n && monoLeft_.size() == n && monoRight_.size() == n ? n : 0;
	}

private:
	void aim();
	void beginInner();
	void endBegun();

	RackEngine engine_;
	/// Declared AFTER the engine it references, so it is destroyed first.
	std::unique_ptr<RackBridge> bridge_;
	/// **THE HOLDER'S COUNTER, REMEMBERED UNTIL `adopt` CAN HAND IT TO THE BRIDGE** — null for a chain
	/// that counts alone. Kept here for the same reason `outerGate_` is: the bridge does not exist until
	/// a catalogue arrives (trap T5), and this is known at construction.
	backplane::InstanceCounter* sharedIds_ = nullptr;

	double sampleRate_ = 0.0;
	/// What `prepare` was told the longest block is — kept for the bridge `adopt` builds later.
	int maxFrames_ = device::kDefaultMaxFrames;
	device::TransportClock clock_ {};
	/// Kept here because it can arrive before `adopt` builds the bridge.
	device::OuterGate outerGate_ {};
	device::HostSlotLink slotLink_ {};

	bool enabled_ = true;
	float mix_ = 1.0f;
	float gain_ = 0.0f; // set to unity in the constructor, from `composite::kFaderUnity`

	/// **THE SCRATCH, SIZED BY `prepare` TO THE HOST'S LONGEST BLOCK** (backlog GO) and nowhere else. A call longer
	/// than it is cut to its length — the guard a promise needs (`device::AudioOps::prepare`).
	std::vector<float> dryLeft_;
	std::vector<float> dryRight_;
	/// **THE DRY WAITS FOR THE CHAIN** (backlog JM) — `rack_device.cpp`'s, for the same mix.
	device::CompensationDelay dryDelay_;
	std::vector<float> monoLeft_;
	std::vector<float> monoRight_;

	/// See `beginInner` — one inner block per bus chunk.
	bool blockBegun_ = false;
	/// See `beginPerVoiceBlock` — whether a host with the two-stage road holds this chain.
	bool hosted_ = false;
	/// INSIDE `beginHostBlock` .. `endHostBlock`, and the latency taken there.
	bool inHostBlock_ = false;
	uint32_t hostLatency_ = 0;
	int tickPhase_ = 0;
	uint64_t blocksBegun_ = 0;

	float wetNow_ = 1.0f;
	float dryNow_ = 0.0f;
	float wetTarget_ = 1.0f;
	float dryTarget_ = 0.0f;
	float wetStep_ = 0.0f;
	float dryStep_ = 0.0f;
	int remaining_ = 0;
	int rampSamples_ = 1;
	/// **THE PER-VOICE ROAD'S OWN FADE** — 1 while enabled, 0 while bypassed, over the same ramp. The
	/// bus blends the chain's output against the signal that ENTERED the bus stage, which on the
	/// per-voice road already carries the inner per-voice devices; a bypass that faded on the bus alone
	/// would CUT those devices in one sample (trap T6, backlog EH shape 6). Kept apart from `wetNow_`
	/// because that one carries the mix and the fader, which the voice road must not apply.
	float voiceWetNow_ = 1.0f;
	float voiceWetTarget_ = 1.0f;
	float voiceWetStep_ = 0.0f;
	bool primed_ = false;
};

} // namespace feathers::rack
