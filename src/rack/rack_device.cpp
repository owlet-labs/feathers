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

#include "rack/rack_device.h"

#include "backplane/block_context.h"
#include "rack/export_table.h"
#include "rack/inner_chain.h"
#include "rack/nesting.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"

#include "clap/stream.h"
#include "device/latency.h"
#include "device/tick_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace feathers::rack {
namespace {

/// A FADER POSITION TO A LINEAR GAIN — `fx::busutil`'s curve, copied rather than shared because a
/// device includes nothing from its neighbours. Unity at 12 o'clock, silence at 0, +12 dB at 1, and
/// continuous at the bottom (a first draft elsewhere used pow(4, …) and was 0.25 just above zero).
constexpr float faderGain(float position) {
	const float p = position < 0.0f ? 0.0f : (position > 1.0f ? 1.0f : position);
	return (2.0f * p) * (2.0f * p);
}
static_assert(faderGain(static_cast<float>(composite::kFaderUnity)) == 1.0f,
              "the centre of the fader must be EXACTLY unity, or a rack at its defaults is not the "
              "identity");

/// 5 ms, a TIME counted in samples and carried across blocks — busutil's reasons: long enough that a
/// gain step is a fade, short enough that a hand feels the value, and partition-independent by
/// construction. Its header records that a per-block ramp failed the partition sweep.
constexpr double kRampSeconds = 0.005;


struct State {
	/// BUILT WITH NO CATALOGUE, because whoever builds a device constructs it before it can say what
	/// devices exist; `adopt` hands it one before anything is added. See `ContainerOps::adopt`.
	RackEngine engine { kRackAxes, device::Catalogue {} };
	/// THE EDITING ROAD INTO `engine` (rack roadmap §6, Stage B): record + converger + drain, the standalone
	/// plugin's exact pair. Built at `adopt`, once the catalogue is known — a record built before it would
	/// hold an empty catalogue. Declared AFTER the engine it references, so it is destroyed first.
	std::unique_ptr<RackBridge> bridge;
	double sampleRate = 0.0;
	/// What `prepare` was told the longest block is — kept for the bridge `adopt` builds later.
	int maxFrames = device::kDefaultMaxFrames;
	device::TransportClock clock {};
	/// **WHAT THIS CHAIN EXPORTS TO ITS HOST** (backlog NZ (b)): its inner modulators, read by the devices after it.
	ExportTable exports;

	bool enabled = true;
	float mix = 1.0f;
	float gain = static_cast<float>(composite::kFaderUnity);

	/// **THE SCRATCH, SIZED BY `prepare` TO THE HOST'S LONGEST BLOCK** (backlog GO) and nowhere else; a call longer
	/// than it is cut to its length — the guard a promise needs (`device::AudioOps::prepare`).
	std::vector<float> dryLeft;
	std::vector<float> dryRight;
	/// **THE DRY WAITS FOR THE CHAIN** (backlog JM): delayed by what the devices inside declare, so the Mix and
	/// the bypass blend two signals that line up. Does nothing at all while nothing inside is latent.
	device::CompensationDelay dryDelay;
	/// THE PER-VOICE STAGE'S PAIR (rack roadmap §8): a stereo voice's two channels run here; a mono voice's one
	/// channel is mirrored into both, run, and averaged back, because the chain inside runs stereo.
	std::vector<float> monoLeft;
	std::vector<float> monoRight;
	/// WHETHER AN INNER BLOCK IS IN FLIGHT. One inner block per bus chunk: begun at the first stage call
	/// that needs one (the host's begin, a per-voice call, or the bus stage itself), ended by the bus
	/// stage once its chunk is through and the inner modulators have advanced by it. See `beginInner`.
	bool blockBegun = false;
	/// WHETHER A HOST WITH THE TWO-STAGE ROAD HOLDS THIS RACK — it has called `beginPerVoiceBlock`, and
	/// that call is the drain point from then on. A host without it (the bus is the whole block) drains
	/// at the bus stage, as this device always did.
	bool hosted = false;
	/// WHERE THE BUS STAGE STANDS INSIDE THE INNER CONTROL TICK (backlog FK). The inner modulators advance a whole
	/// tick when it closes, so every inner modulator ticks on one grid, and the inner devices are handed the tick's
	/// windows at this phase — one tick behind the audio, as the inner rows always were.
	int tickPhase = 0;

	float wetNow = 1.0f;
	float dryNow = 0.0f;
	float wetTarget = 1.0f;
	float dryTarget = 0.0f;
	float wetStep = 0.0f;
	float dryStep = 0.0f;
	int remaining = 0;
	int rampSamples = 1;
	/// **THE PER-VOICE ROAD'S OWN FADE (backlog EH, shape 6)** — 1 while enabled, 0 while bypassed,
	/// ramped over the same `rampSamples` as the bus's wet and dry. The bus blends its own devices
	/// against the signal that ENTERED the bus stage, which on the per-voice road already carries the
	/// inner per-voice devices, so a bypass that faded on the bus alone CUT those devices out in one
	/// sample while the comment above promised a fade. Kept apart from `wetNow` because that one
	/// carries the mix and the fader, which the voice road must not apply.
	float voiceWetNow = 1.0f;
	float voiceWetTarget = 1.0f;
	float voiceWetStep = 0.0f;
	bool primed = false;
	/// THE GATE OUTSIDE (rack roadmap §7), kept here because it can arrive before `adopt` builds the bridge.
	device::OuterGate outerGate {};
	/// THE POOL OUTSIDE (host slots T11), kept for the same reason.
	device::HostSlotLink slotLink {};
};


State& stateOf(void* storage) { return *static_cast<State*>(storage); }
const State& stateOf(const void* storage) { return *static_cast<const State*>(storage); }

/// AIM THE OUTPUT RAMP at what the controls say, from wherever it is. Enabled: the rack's output at
/// `mix` and the signal that entered at `1 - mix`, through the fader. Bypassed: the signal that
/// entered, at unity — a bypass fades to the dry path rather than cutting.
void aim(State& s) {
	const float g = faderGain(s.gain);
	s.wetTarget = s.enabled ? s.mix * g : 0.0f;
	s.dryTarget = s.enabled ? (1.0f - s.mix) * g : 1.0f;
	s.voiceWetTarget = s.enabled ? 1.0f : 0.0f;
	if (!s.primed) {
		s.wetNow = s.wetTarget;
		s.dryNow = s.dryTarget;
		s.voiceWetNow = s.voiceWetTarget;
		s.wetStep = 0.0f;
		s.dryStep = 0.0f;
		s.voiceWetStep = 0.0f;
		s.remaining = 0;
		return;
	}
	const float n = static_cast<float>(s.rampSamples);
	s.wetStep = (s.wetTarget - s.wetNow) / n;
	s.dryStep = (s.dryTarget - s.dryNow) / n;
	s.voiceWetStep = (s.voiceWetTarget - s.voiceWetNow) / n;
	s.remaining = s.rampSamples;
}

//----------------------------------------------------------------------------------//
// AudioOps                                                                           //
//----------------------------------------------------------------------------------//

void construct(void* storage) { new (storage) State(); }
void destruct(void* storage) { stateOf(storage).~State(); }

void prepare(void* storage, double sampleRate, int maxFrames) {
	State& s = stateOf(storage);
	s.sampleRate = sampleRate;
	s.maxFrames = maxFrames;
	s.engine.prepare(sampleRate, maxFrames);
	// THE BRIDGE'S PREPARE provisions what its record holds at this rate (message thread, as this is).
	if (s.bridge != nullptr) {
		s.bridge->prepare(sampleRate, maxFrames);
	}
	const auto scratch = static_cast<size_t>(device::scratchFrames(maxFrames));
	s.dryLeft.assign(scratch, 0.0f);
	s.dryRight.assign(scratch, 0.0f);
	s.monoLeft.assign(scratch, 0.0f);
	s.monoRight.assign(scratch, 0.0f);
	// SIZED TO WHAT THE CHAIN DECLARES (backlog NR) — the devices it already runs; a later rise restarts the host.
	s.dryDelay.prepare(sampleRate, s.engine.latencySamples(sampleRate));
	s.rampSamples =
	    std::max(1, static_cast<int>(std::lround(kRampSeconds * std::max(sampleRate, 1.0))));
}

void reset(void* storage) {
	State& s = stateOf(storage);
	s.engine.reset();
	s.dryDelay.reset();
	s.primed = false;
	s.wetNow = s.wetTarget;
	s.dryNow = s.dryTarget;
	s.voiceWetNow = s.voiceWetTarget;
	s.wetStep = 0.0f;
	s.dryStep = 0.0f;
	s.voiceWetStep = 0.0f;
	s.remaining = 0;
}

void setFields(void* storage, const float* fields, size_t fieldsPresent) {
	State& s = stateOf(storage);
	const auto at = [&](composite::Param p) {
		const auto i = static_cast<size_t>(p);
		return i < fieldsPresent && fields != nullptr
		           ? fields[i]
		           : static_cast<float>(composite::kParamDescs[i].def);
	};
	s.enabled = at(composite::Param::Enabled) >= 0.5f;
	s.mix = std::clamp(at(composite::Param::Mix), 0.0f, 1.0f);
	s.gain = std::clamp(at(composite::Param::Gain), 0.0f, 1.0f);
	// **THE MACROS ARE THE RACK'S OWN** — the engine's core sources, routed inside by the rack's edges
	// exactly as in the standalone rack. Setting one here is the same call the plugin's parameter makes.
	for (size_t n = 0; n < composite::kMacroCount; ++n) {
		const auto p = static_cast<composite::Param>(static_cast<size_t>(composite::Param::Macro1) + n);
		s.engine.setMacro(n, std::clamp(at(p), 0.0f, 1.0f));
	}
	aim(s);
}

void setNoteContext(void* storage, const device::NoteContext& note) {
	// THE WHOLE NOTE, not only its gate: a keytracking filter inside this rack needs the pitch.
	stateOf(storage).engine.setExternalNote(note);
}

void setTransportClock(void* storage, const device::TransportClock& clock) {
	stateOf(storage).clock = clock;
}

/// **WHAT THE CHAIN INSIDE RUNS**, whatever this rack's own controls say — Enabled included (assumption L3).
uint32_t latencySamples(const void* storage, const float*, size_t, double sampleRate) {
	return stateOf(storage).engine.latencySamples(sampleRate);
}

/// **WHETHER A DEVICE INSIDE STILL HOLDS SOUND TO COME** (backlog MC) — the inner engine's answer, unless this rack is
/// bypassed and settled: then nothing inside runs (`processStereo`), a line frozen there cannot come out, and only the
/// dry passes, through the delay this rack reports as latency.
bool holdsTail(const void* storage) {
	const State& s = stateOf(storage);
	if (!s.enabled && s.remaining == 0 && s.dryNow == 1.0f && s.wetNow == 0.0f) {
		return false;
	}
	return s.engine.holdsTail();
}

void setOuterSources(void* storage, const device::OuterSources& outer) {
	// THE READER, HANDED THROUGH TO THE INNER ENGINE — rack roadmap §7: an inner control may read an
	// outer source, and this is how the inner engine finds its cell and asks whether it is upstream.
	stateOf(storage).engine.setOuterSources(outer);
}


//----------------------------------------------------------------------------------//
// THE PASS-THROUGH MODEL (rack roadmap §8): a chain is not Poly; its   //
// devices are. The host's voice count runs inside, and the host's two stages run       //
// inside — per voice for the devices that ask, on the bus for the rest — on ONE instance. //
//----------------------------------------------------------------------------------//

void setVoices(void* storage, size_t voices) {
	// THE HOST'S VOICES ARE THIS RACK'S VOICES: an inner device that declares Poly gets one instance per
	// outer voice, and a per-voice outer source's run is indexed by the same voice. Message thread, at
	// provision, before anything inside is provisioned.
	stateOf(storage).engine.setVoices(voices);
}

/// End the inner block in flight, if one is.
void endBegun(State& s) {
	if (s.blockBegun) {
		s.engine.endBlock();
		s.blockBegun = false;
	}
}

/// Begin an inner block, if none is in flight. NO FRAMES: the inner modulators advance at the END of
/// each bus chunk, by that chunk (`processStereo`), so a block begun here resolves its rows against
/// where the modulators stand after the audio so far — the lane's own convention, its strip ticked
/// between segments and its rows resolved at the next begin. A begin is not a drain: pending edits
/// are adopted at the host's begin (`beginPerVoiceBlock`), the one drain point while hosted.
void beginInner(State& s) {
	if (!s.blockBegun) {
		s.engine.beginBlock(0, backplane::BlockContext { s.clock });
		s.blockBegun = true;
	}
}

bool beginPerVoiceBlock(void* storage) {
	// THE HOST'S BEGIN-BLOCK, FORWARDED: the drain point, and the answer. A chunk's block left in
	// flight (the host skipped the bus stage) is ended here; then pending edits are adopted, and the
	// first inner block of the host's block is begun to resolve the inner rows, whose Poly fields are
	// the answer. The host's per-voice calls and the bus stage then run inside that block.
	State& s = stateOf(storage);
	s.hosted = true;
	endBegun(s);
	if (s.bridge != nullptr) {
		s.bridge->drain();
	}
	beginInner(s);
	// AND WHILE A BYPASS IS STILL FADING, the per-voice road keeps running so the fade has something
	// to fade (backlog EH, shape 6); `processVoice` blends by `voiceWetNow`.
	return (s.enabled || s.remaining > 0) && s.engine.anyPerVoice();
}

void processVoice(void* storage, size_t voice, float* left, float* right, int frames, const device::NoteContext& note) {
	// ONE VOICE THROUGH THE INNER DEVICES THAT RUN PER VOICE, with the voice's note. A STEREO voice runs as its
	// pair; a MONO voice's channel is mirrored into a pair (the inner rack is stereo) and averaged back — a stereo
	// per-voice device inside is heard at its mid. The chain's mix and gain apply on the bus, where there is a dry
	// to mix against; here the voice carries the wet. Inside the chunk's inner block, begun here if the bus stage
	// ended the last one.
	//
	// **THE RIGHT CHANNEL WAS NEVER HANDED IN BEFORE 2026-10-03 (backlog KT)**: a stereo voice's right ear passed
	// every Poly device inside the chain dry, so a Poly filter in a chain under a spread stack filtered one ear.
	State& s = stateOf(storage);
	if (left == nullptr || frames <= 0 || (!s.enabled && s.remaining == 0) || s.monoLeft.empty()) {
		return;
	}
	beginInner(s);
	// BLENDED BY THE VOICE ROAD'S OWN FADE (backlog EH, shape 6): 1 while enabled, so this is exactly
	// the inner devices' output; falling to 0 across the bypass ramp, so the inner devices fade out
	// of each voice over the same 5 ms the bus fades over, rather than leaving in one sample. Read
	// once per call — the bus stage advances it after the voices have run this block.
	const float wet = s.voiceWetNow;
	int done = 0;
	while (done < frames) {
		const int n = std::min(frames - done, static_cast<int>(s.monoLeft.size()));
		const auto bytes = static_cast<size_t>(n) * sizeof(float);
		std::memcpy(s.monoLeft.data(), left + done, bytes);
		std::memcpy(s.monoRight.data(), (right != nullptr ? right : left) + done, bytes);
		if (s.sampleRate > 0.0) { // this piece's own sample — see `RackEngine::setClock`
			s.engine.setClock(RackEngine::advanced(s.clock, static_cast<double>(done) / s.sampleRate));
		}
		s.engine.processVoice(voice, s.monoLeft.data(), s.monoRight.data(), n, note);
		for (int i = 0; i < n; ++i) {
			const auto k = static_cast<size_t>(i);
			if (right != nullptr) {
				left[done + i] += wet * (s.monoLeft[k] - left[done + i]);
				right[done + i] += wet * (s.monoRight[k] - right[done + i]);
			}
			else {
				const float dry = left[done + i];
				const float processed = 0.5f * (s.monoLeft[k] + s.monoRight[k]);
				left[done + i] = dry + wet * (processed - dry);
			}
		}
		done += n;
	}
}

void processStereo(void* storage, float* left, float* right, int frames) {
	State& s = stateOf(storage);
	// **THE DRAIN AND THE PUBLISH, THE STANDALONE PLUGIN'S PAIR AROUND ITS PROCESS**: pending edits from the
	// chain's editor are adopted here, on the audio thread, before the inner block runs; the mirror the
	// editor reads (ids, readings, playheads) is published after it. Every return path publishes.
	struct Publish {
		State& state;
		~Publish() {
			if (state.bridge != nullptr) {
				state.bridge->publish(state.engine);
			}
			// THE INNER MODULATORS HAVE TICKED: what this chain exports, into the cells the host reads (NZ (b)).
			state.exports.publish(
			    [](const void* e, backplane::Source src) { return static_cast<const RackEngine*>(e)->sourceCell(src); },
			    &state.engine);
		}
	} publish { s };
	// HOSTED, THE DRAIN HAPPENED AT THE HOST'S BEGIN (`beginPerVoiceBlock`, rack roadmap §8) and this is
	// the bus stage of a chunk whose inner block may already be in flight from the per-voice stage. A
	// host without the two-stage road never begins; then the bus is the whole block and drains here.
	if (!s.hosted && !s.blockBegun && s.bridge != nullptr) {
		s.bridge->drain();
	}
	// AND NOTHING BEFORE `prepare` HAS SIZED THE SCRATCH: an empty one would cut every piece to zero frames for ever.
	if (frames <= 0 || left == nullptr || right == nullptr || s.dryLeft.empty()) {
		return;
	}
	// **HOW LATE THE CHAIN INSIDE IS, AFTER THIS BLOCK'S DRAIN** (backlog JM) — the count the dry is delayed
	// by, and the one this rack reports up. Zero, and so nothing below changes, while nothing inside is latent.
	const uint32_t latency = s.engine.latencySamples(s.sampleRate);
	// BYPASSED AND SETTLED IS A TRUE PASS, bit for bit; bypassed and still ramping finishes its fade.
	// Nothing runs inside, so nothing stays in flight: the inner editor can reap while the chain is off.
	// **A PASS THAT WAITS AS LONG AS THE CHAIN WOULD** when something inside is latent (assumption L3): a
	// bypass that dropped the delay would move the latency the host was told, and restart it on every toggle.
	if (!s.enabled && s.remaining == 0 && s.dryNow == 1.0f && s.wetNow == 0.0f) {
		s.primed = true;
		endBegun(s);
		s.dryDelay.process(left, right, frames, latency);
		return;
	}
	s.primed = true;

	const backplane::BlockContext block { s.clock };
	size_t done = 0;
	const auto total = static_cast<size_t>(frames);
	while (done < total) {
		// THE RACK'S BUS STAGE, IN THIS CHUNK'S INNER BLOCK — begun by the host's begin or the per-voice
		// stage, or here. Chunks are cut on the inner control tick (`tickPhase`): the inner devices read the tick's
		// windows at their phase, and when a tick closes every inner modulator advances by it and the block ends, so
		// the next chunk's block resolves its rows against them — the grid Feathers' lane and the standalone rack run on.
		beginInner(s);
		s.engine.setTickPhase(s.tickPhase);
		// THIS PIECE'S OWN SAMPLE, not the call's first — see `RackEngine::setClock` (the 2026-09-24 crush).
		if (s.sampleRate > 0.0) {
			s.engine.setClock(RackEngine::advanced(s.clock, static_cast<double>(done) / s.sampleRate));
		}
		// AND CUT WHERE A LATE DEVICE INSIDE IS HANDED ITS NOTE OR ITS ROW (backlog LW): `UINT32_MAX` with nothing late.
		const size_t n = std::min({ s.dryLeft.size(), total - done, static_cast<size_t>(device::kTickWindow - s.tickPhase),
		                            static_cast<size_t>(s.engine.samplesUntilLateNote()) });
		float* l = left + done;
		float* r = right + done;
		std::copy(l, l + n, s.dryLeft.begin());
		std::copy(r, r + n, s.dryRight.begin());
		s.dryDelay.process(s.dryLeft.data(), s.dryRight.data(), static_cast<int>(n), latency);
		s.engine.processBus(l, r, static_cast<int>(n));
		s.tickPhase = (s.tickPhase + static_cast<int>(n)) % device::kTickWindow;
		s.engine.tickModulators(s.tickPhase == 0 ? device::kTickWindow : 0, block);
		endBegun(s);

		for (size_t i = 0; i < n; ++i) {
			if (s.remaining > 0) {
				s.wetNow += s.wetStep;
				s.dryNow += s.dryStep;
				s.voiceWetNow += s.voiceWetStep;
				if (--s.remaining == 0) {
					s.wetNow = s.wetTarget;
					s.dryNow = s.dryTarget;
					s.voiceWetNow = s.voiceWetTarget;
				}
			}
			l[i] = l[i] * s.wetNow + s.dryLeft[i] * s.dryNow;
			r[i] = r[i] * s.wetNow + s.dryRight[i] * s.dryNow;
		}
		done += n;
	}
}

//----------------------------------------------------------------------------------//
// THE RACK'S FILE FORMAT, OVER BYTES.                                                //
//----------------------------------------------------------------------------------//


struct ByteSource {
	const uint8_t* data = nullptr;
	size_t size = 0;
	size_t at = 0;
	static int64_t CLAP_ABI read(const clap_istream* stream, void* buffer, uint64_t size) {
		auto* self = static_cast<ByteSource*>(stream->ctx);
		const size_t n = std::min<size_t>(static_cast<size_t>(size), self->size - self->at);
		std::memcpy(buffer, self->data + self->at, n);
		self->at += n;
		return static_cast<int64_t>(n);
	}
};

// **THE DEPTH COUNTER MOVED OUT OF THIS FILE, AND THE MOVE IS THE WHOLE FIX** (the bolt-on
// chains' gap G4). It was a `thread_local` HERE, private to this device,
// which is correct for one container kind and wrong for two: two DIFFERENT kinds alternating
// (A -> B -> A -> ...) each counted only their own levels, so each saw depth 1 at every level and
// neither guard ever fired — *"even two correct copies do not compose."* One counter that every
// container increments composes by construction, and `rack::containerLoadDepth()` is it.
//
// **IT STOPPED BEING HYPOTHETICAL WHEN SKLITTER GAINED A POST CHAIN** (backlog EW): there are two
// container kinds now, and a rack inside SKLITTER's chain inside a rack is a structure a hand can
// build. `tests/test_inner_chain.cpp` pins the alternating composition.
//
// NOTHING ELSE IN THIS FILE CHANGED. The drain placement — the four interacting booleans whose
// comments record two prior wrong versions — is untouched; a recursion counter is not drain
// semantics, which is why this could be lifted without porting the rest onto `rack::InnerChain`.

// **THE WHOLE OF WHAT THIS RACK NEEDS FROM WHOEVER HOLDS IT.** Feathers' lane, another rack, or a third
device::RouteDrop service(void* storage) {
	// THE HOST'S BEAT REACHES INSIDE (rack roadmap §7, hygiene): reap, and drop routes whose outer source
	// the gate no longer admits.
	State& s = stateOf(storage);
	return s.bridge != nullptr ? s.bridge->service() : device::RouteDrop {};
}


// THE ROUTES INSIDE, AS THE HOST OUTSIDE SEES THEM (rack roadmap §7): the outer source arrives spelled the
// outer way and is looked up as this rack's table holds it — with the scope bit on.
void forEachRouteFrom(const void* storage, device::Source outer, void* ctx,
                      void (*fn)(void* ctx, device::Destination inner, float amount)) {
	const State& s = stateOf(storage);
	if (s.bridge == nullptr || fn == nullptr) {
		return;
	}
	const backplane::Source key = outer.asOuter();
	s.bridge->forEachRoute([&](const backplane::Route& r) {
		if (r.source == key) {
			fn(ctx, r.destination, r.amount);
		}
	});
}

float routeAmountFrom(const void* storage, device::Source outer, device::Destination inner) {
	const State& s = stateOf(storage);
	return s.bridge != nullptr ? s.bridge->routeAmount(outer.asOuter(), inner) : 0.0f;
}

bool setRouteFrom(void* storage, device::Source outer, device::Destination inner, float amount) {
	State& s = stateOf(storage);
	if (s.bridge == nullptr) {
		return false;
	}
	const backplane::Source key = outer.asOuter();
	if (amount != 0.0f && !s.bridge->mayRoute(key, inner)) {
		return false; // the gate inside, asked as the inner editor would ask it
	}
	s.bridge->setRouteAmount(key, inner, amount);
	return true;
}

device::Transfer routeTransferFrom(const void* storage, device::Source outer, device::Destination inner) {
	const State& s = stateOf(storage);
	return s.bridge != nullptr ? s.bridge->routeTransfer(outer.asOuter(), inner) : device::Transfer::Lin;
}

bool setRouteTransferFrom(void* storage, device::Source outer, device::Destination inner, device::Transfer t) {
	State& s = stateOf(storage);
	return s.bridge != nullptr && s.bridge->setRouteTransfer(outer.asOuter(), inner, t);
}

size_t nameDestination(const void* storage, device::Destination inner, char* out, size_t capacity) {
	if (out == nullptr || capacity == 0) {
		return 0;
	}
	out[0] = '\0';
	const State& s = stateOf(storage);
	const device::Descriptor* d = s.bridge != nullptr ? s.bridge->descriptorOf(inner.device) : nullptr;
	if (d == nullptr || inner.index >= d->params.size()) {
		return 0;
	}
	const int n = std::snprintf(out, capacity, "%s · %s", d->traits.name, d->params[inner.index].name);
	return n > 0 ? std::min(static_cast<size_t>(n), capacity - 1) : 0;
}

// party's chain: each hands over its list of devices, and from here this rack is the same rack the
void setOuterGate(void* storage, const device::OuterGate& gate) {
	// THE MESSAGE-THREAD HALF OF THE HOST OUTSIDE — the inner record's gate (rack roadmap §7).
	State& s = stateOf(storage);
	s.outerGate = gate;
	if (s.bridge != nullptr) {
		s.bridge->setOuterGate(gate);
	}
}

void setHostSlots(void* storage, const device::HostSlotLink& link) {
	// THE POOL OUTSIDE (host slots T11): this rack's controls take their slots from the outermost plugin.
	State& s = stateOf(storage);
	s.slotLink = link;
	if (s.bridge != nullptr) {
		s.bridge->setHostSlots(link);
	}
}

// standalone plugin runs.
void adopt(void* storage, const device::Catalogue& catalogue) {
	State& s = stateOf(storage);
	s.engine.setCatalogue(catalogue);
	// THE BRIDGE, NOW THAT THE CATALOGUE IS KNOWN — its record copies the engine's catalogue at construction.
	// Message thread (a provision), where it may allocate. Prepared at the rate the device already has.
	s.bridge = std::make_unique<RackBridge>(s.engine);
	s.bridge->setOuterGate(s.outerGate);
	s.bridge->setOwnerExports(&s.exports); // a re-exported row of this chain keeps the inner row it names (NZ, any depth)
	// THIS RACK'S MACROS ARE THIS DEVICE'S `Macro1..8` (T11): a rename relabels a slot on one.
	s.bridge->setMacrosAreDeviceControls(true);
	s.bridge->setHostSlots(s.slotLink);
	if (s.sampleRate > 0.0) {
		s.bridge->prepare(s.sampleRate, s.maxFrames);
	}
}

/// A clap stream that forwards straight into the host's sink — so the rack's own `saveRack` writes the
/// body, and writes it once.
struct SinkStream {
	const device::ByteSink* sink = nullptr;
	static int64_t CLAP_ABI write(const clap_ostream* stream, const void* buffer, uint64_t size) {
		auto* self = static_cast<SinkStream*>(stream->ctx);
		(*self->sink)(static_cast<const uint8_t*>(buffer), static_cast<size_t>(size));
		return static_cast<int64_t>(size);
	}
};

bool saveState(const void* storage, const device::ByteSink& sink) {
	SinkStream forward { &sink };
	const clap_ostream stream { &forward, &SinkStream::write };
	const State& s = stateOf(storage);
	// THE RECORD'S TRUTH, as the standalone saves: an edit made through the chain's editor and not yet
	// drained is in the record and not in the engine. Message thread, where the record lives.
	if (s.bridge != nullptr) {
		RackPatch patch = s.bridge->patch();
		s.exports.save(&patch.exports); // the export table rides the body (NZ (b))
		return writeRack(&stream, patch, false); // compact: a body (see `rackToText`)
	}
	return saveRack(&stream, s.engine, false);
}

bool loadState(void* storage, const uint8_t* data, size_t bytes) {
	State& s = stateOf(storage);
	if (data == nullptr || bytes == 0 || bytes > kMaxBodyBytes) {
		return false;
	}
	// **THE DEPTH LIMIT, ENFORCED** — see `containerLoadDepth`, which every container kind shares. A
	// rack this deep refuses its body rather than recurse into it.
	size_t& depth = containerLoadDepth();
	if (loadRefuses(depth) || !chainHolds(s.engine.catalogue().depth)) {
		return false; // by where it stands too, as `InnerChain::loadState` (2026-10-31)
	}
	struct Guard {
		size_t& d;
		explicit Guard(size_t& depth) : d(depth) { ++d; }
		~Guard() { --d; }
	} guard { depth };

	ByteSource source { data, bytes, 0 };
	const clap_istream stream { &source, &ByteSource::read };
	// THROUGH THE RECORD WHEN THERE IS ONE: the patch is applied to the record, which provisions the members
	// (message thread — this is a provision of the outer host, before the instance is adopted, so the drain
	// here runs on an engine nothing else is running), exactly the standalone plugin's load. Without a bridge
	// — no catalogue adopted yet — the engine is loaded directly, as before.
	if (s.bridge != nullptr) {
		RackPatch patch;
		if (!parseRack(&stream, &patch)) {
			return false;
		}
		(void)s.bridge->loadPatch(patch);
		s.exports.load(patch.exports);
		s.bridge->drain();
		s.bridge->publish(s.engine);
		return source.at == bytes;
	}
	LoadReport refused;
	if (!loadRack(&stream, &s.engine, &refused)) {
		return false;
	}
	return source.at == bytes;
}

/// `ContainerOps::applyState` — **A BODY INTO A CHAIN THAT MAY BE RUNNING** (backlog OU, a chain preset): the record
/// becomes the patch and the engine converges at the device's own drain, `InnerChain::applyState`'s road. EMPTY BYTES
/// empty the chain. False — nothing touched — before the catalogue is adopted, or for bytes that are not a chain.
bool applyState(void* storage, const uint8_t* data, size_t bytes) {
	State& s = stateOf(storage);
	if (s.bridge == nullptr || bytes > kMaxBodyBytes) {
		return false;
	}
	RackPatch patch;
	if (bytes == 0) {
		patch.present = true; // an empty chain, not "a fresh instance" (which `loadPatch` would leave as it is)
	}
	else {
		ByteSource source { data, bytes, 0 };
		const clap_istream stream { &source, &ByteSource::read };
		if (!parseRack(&stream, &patch) || source.at != bytes) {
			return false;
		}
	}
	// THE DEPTH GATE (`fitsNesting`): a preset too tall for where this Chain stands is refused whole.
	if (!fitsNesting(s.engine.catalogue().depth, patchHeight(patch, s.engine.catalogue()))) {
		return false;
	}
	(void)s.bridge->loadPatch(patch);
	s.exports.load(patch.exports);
	return true;
}

/// **THE ROW EXPORTING `inner`** (backlog NZ (b)) — one of this chain's modulator outputs or macros; nothing else
/// inside reaches out (an audio device's output is not a source).
size_t exportRow(void* storage, backplane::Source inner) {
	State& s = stateOf(storage);
	if (s.bridge == nullptr || inner.isOuter()) {
		return ExportTable::kNoRow;
	}
	// A HOLDER INSIDE, RE-EXPORTED ONE HOP OUT (NZ, any depth): only a row that holder actually has.
	if (inner.isExport()) {
		return s.bridge->namesLiveExport(inner) ? s.exports.mint(inner) : ExportTable::kNoRow;
	}
	if (inner.isDevice()) {
		const device::Descriptor* d = s.bridge->descriptorOf(inner.device);
		if (d == nullptr || d->mod == nullptr || inner.index >= d->outputCount()) {
			return ExportTable::kNoRow;
		}
	}
	else if (inner.index >= s.bridge->axes().coreSources) {
		return ExportTable::kNoRow;
	}
	return s.exports.mint(inner);
}
} // namespace

const device::AudioOps kDeviceAudioOps {
	.stateBytes = sizeof(State),
	.stateAlign = alignof(State),
	.construct = &construct,
	.destruct = &destruct,
	.prepare = &prepare,
	.reset = &reset,
	.setSlot = nullptr,
	.setFields = &setFields,
	.setNoteContext = &setNoteContext,
	.drivePeak = nullptr,
	.setTransportClock = &setTransportClock,
	.processStereo = &processStereo,
	.processMono = nullptr,
	.setOuterSources = &setOuterSources,
	.beginPerVoiceBlock = &beginPerVoiceBlock,
	.processVoice = &processVoice,
	.latencySamples = &latencySamples,
	.holdsTail = &holdsTail,
	// THE SIDECHAIN REACHES THE DEVICES INSIDE (design §10.1): the inner engine offsets it per piece as the outer one does.
	.setSidechain = [](void* storage, const float* left, const float* right, int, bool sounding) {
		stateOf(storage).engine.setSidechain(left, right, sounding);
	},
};

const device::ContainerOps kDeviceContainerOps {
	.adopt = &adopt,
	.saveState = &saveState,
	.loadState = &loadState,
	.maxBodyBytes = kMaxBodyBytes,
	.setOuterGate = &setOuterGate,
	.setHostSlots = &setHostSlots,
	.service = &service,
	.setVoices = &setVoices,
	.forEachRouteFrom = &forEachRouteFrom,
	.routeAmountFrom = &routeAmountFrom,
	.setRouteFrom = &setRouteFrom,
	.routeTransferFrom = &routeTransferFrom,
	.setRouteTransferFrom = &setRouteTransferFrom,
	.nameDestination = &nameDestination,
	.macroLabel =
	    [](const void* storage, size_t macro, char* out, size_t capacity) {
		    if (out == nullptr || capacity == 0) {
			    return size_t { 0 };
		    }
		    out[0] = '\0';
		    const State& s = stateOf(storage);
		    if (s.bridge == nullptr || !s.bridge->macroRenamed(macro)) {
			    return size_t { 0 };
		    }
		    const std::string name = s.bridge->macroName(macro);
		    const size_t n = std::min(name.size(), capacity - 1);
		    std::memcpy(out, name.data(), n);
		    out[n] = '\0';
		    return n;
	    },
	.applyState = &applyState,
	.exportRow = &exportRow,
	.exportedSource = [](const void* storage, size_t row) { return stateOf(storage).exports.source(row); },
	.exportCell = [](const void* storage, size_t row) { return stateOf(storage).exports.cell(row); },
	.sweepExports = [](void* storage, const void* usedRows, uint32_t published, uint32_t drained) {
		const auto& used = *static_cast<const std::vector<bool>*>(usedRows);
		return stateOf(storage).exports.sweep([&](size_t r) { return r < used.size() && used[r]; }, published, drained);
	},
};

RackEngine& engineForTest(void* storage) { return stateOf(storage).engine; }
RackBridge* bridgeOf(void* storage) { return storage != nullptr ? stateOf(storage).bridge.get() : nullptr; }
RackEngine* engineOf(void* storage) { return storage != nullptr ? &stateOf(storage).engine : nullptr; }

Reading readingForTest(const void* storage) {
	const State& s = stateOf(storage);
	return { s.enabled, s.mix, s.gain };
}

} // namespace feathers::rack
