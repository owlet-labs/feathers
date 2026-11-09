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

#include "rack/inner_chain.h"

#include "backplane/block_context.h"
#include "composite/composite_types.h"
#include "rack/nesting.h"
#include "rack/rack_bridge.h"
#include "rack/rack_state.h"

#include "clap/stream.h"
#include "device/tick_window.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace feathers::rack {
namespace {

/// A FADER POSITION TO A LINEAR GAIN — `fx::busutil`'s curve. Unity at 12 o'clock, silence at 0,
/// +12 dB at 1, and continuous at the bottom (a first draft elsewhere used pow(4, …) and was 0.25 just
/// above zero).
constexpr float faderGain(float position) {
	const float p = position < 0.0f ? 0.0f : (position > 1.0f ? 1.0f : position);
	return (2.0f * p) * (2.0f * p);
}
static_assert(faderGain(static_cast<float>(composite::kFaderUnity)) == 1.0f,
              "the centre of the fader must be EXACTLY unity, or a chain at its defaults is not the "
              "identity");

/// 5 ms, a TIME counted in samples and carried across blocks — busutil's reasons: long enough that a
/// gain step is a fade, short enough that a hand feels the value, and partition-independent by
/// construction.
constexpr double kRampSeconds = 0.005;


/// A clap stream that forwards straight into the host's sink — so `writeRack` writes the body once.
struct SinkStream {
	const device::ByteSink* sink = nullptr;
	static int64_t CLAP_ABI write(const clap_ostream* stream, const void* buffer, uint64_t size) {
		auto* self = static_cast<SinkStream*>(stream->ctx);
		(*self->sink)(static_cast<const uint8_t*>(buffer), static_cast<size_t>(size));
		return static_cast<int64_t>(size);
	}
};

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

} // namespace

size_t& containerLoadDepth() noexcept {
	thread_local size_t depth = 0;
	return depth;
}

InnerChain::InnerChain(const backplane::Axes& axes, backplane::InstanceCounter* sharedIds)
    // BUILT WITH NO CATALOGUE — see the constructor's declaration, and trap T5.
    : engine_(axes, device::Catalogue {}), sharedIds_(sharedIds),
      gain_(static_cast<float>(composite::kFaderUnity)) {}

InnerChain::~InnerChain() = default;

//------------------------------------------------------------------------------------------------//
// MESSAGE THREAD                                                                                   //
//------------------------------------------------------------------------------------------------//

void InnerChain::adopt(const device::Catalogue& catalogue) {
	engine_.setCatalogue(catalogue);
	// THE BRIDGE, NOW THAT THE CATALOGUE IS KNOWN — its record copies the engine's catalogue at
	// construction, which is why it cannot be built any earlier (trap T5). Message thread (a
	// provision), where it may allocate. Prepared at the rate the holder already has.
	// **AND THE HOLDER'S COUNTER GOES IN HERE, NOT AT CONSTRUCTION** (gap G3) — the bridge is the thing
	// that issues ids, and this is the first moment it exists.
	bridge_ = std::make_unique<RackBridge>(engine_, sharedIds_);
	bridge_->setOuterGate(outerGate_);
	bridge_->setHostSlots(slotLink_);
	if (sampleRate_ > 0.0) {
		bridge_->prepare(sampleRate_, maxFrames_);
	}
}

void InnerChain::prepare(double sampleRate, int maxFrames) {
	sampleRate_ = sampleRate;
	maxFrames_ = maxFrames;
	engine_.prepare(sampleRate, maxFrames);
	// THE BRIDGE'S PREPARE provisions what its record holds at this rate (message thread, as this is).
	if (bridge_ != nullptr) {
		bridge_->prepare(sampleRate, maxFrames);
	}
	// **THE SCRATCH IS SIZED HERE AND NOWHERE ELSE** — trap T2. Everything below this line runs on the
	// audio thread and allocates nothing. Sized to the host's longest block (backlog GO): `process` and
	// `processVoice` cut anything longer to it.
	const auto scratch = static_cast<size_t>(device::scratchFrames(maxFrames));
	dryLeft_.assign(scratch, 0.0f);
	dryRight_.assign(scratch, 0.0f);
	// SIZED TO WHAT THE CHAIN DECLARES (backlog NR) — the devices it already runs; a later rise restarts the host.
	dryDelay_.prepare(sampleRate, engine_.latencySamples(sampleRate));
	monoLeft_.assign(scratch, 0.0f);
	monoRight_.assign(scratch, 0.0f);
	rampSamples_ = std::max(1, static_cast<int>(std::lround(kRampSeconds * std::max(sampleRate, 1.0))));
}

void InnerChain::reset() noexcept {
	engine_.reset();
	dryDelay_.reset();
	primed_ = false;
	wetNow_ = wetTarget_;
	dryNow_ = dryTarget_;
	voiceWetNow_ = voiceWetTarget_;
	wetStep_ = 0.0f;
	dryStep_ = 0.0f;
	voiceWetStep_ = 0.0f;
	remaining_ = 0;
}

void InnerChain::setVoices(size_t voices) { engine_.setVoices(voices); }

void InnerChain::setOuterGate(const device::OuterGate& gate) {
	outerGate_ = gate;
	if (bridge_ != nullptr) {
		bridge_->setOuterGate(gate);
	}
}

void InnerChain::setHostSlots(const device::HostSlotLink& link) {
	slotLink_ = link;
	if (bridge_ != nullptr) {
		bridge_->setHostSlots(link);
	}
}

device::RouteDrop InnerChain::service() {
	return bridge_ != nullptr ? bridge_->service() : device::RouteDrop {};
}

bool InnerChain::saveState(const device::ByteSink& sink) const {
	SinkStream forward { &sink };
	const clap_ostream stream { &forward, &SinkStream::write };
	// THE RECORD'S TRUTH: an edit made through the chain's editor and not yet drained is in the record
	// and not in the engine. Message thread, where the record lives.
	if (bridge_ != nullptr) {
		return writeRack(&stream, bridge_->patch(), false); // compact: a body (see `rackToText`)
	}
	return saveRack(&stream, engine_, false);
}

bool InnerChain::loadState(const uint8_t* data, size_t bytes) {
	if (data == nullptr || bytes == 0 || bytes > kMaxBodyBytes) {
		return false;
	}
	// **THE DEPTH LIMIT, ENFORCED** — see `containerLoadDepth`. A chain this deep refuses its body
	// rather than recurse into it. AND BY WHERE IT STANDS, not only by how many loads are open (2026-10-31): a body
	// loaded live (a paste, a preset) opens no outer load, so the count alone let a chain too deep for any file fill
	// up live and come back empty. `chainHolds` is the loader's own test, so the two roads agree by construction.
	size_t& depth = containerLoadDepth();
	if (loadRefuses(depth) || !chainHolds(engine_.catalogue().depth)) {
		return false;
	}
	struct Guard {
		size_t& d;
		explicit Guard(size_t& depth) : d(depth) { ++d; }
		~Guard() { --d; }
	} guard { depth };

	ByteSource source { data, bytes, 0 };
	const clap_istream stream { &source, &ByteSource::read };
	// THROUGH THE RECORD WHEN THERE IS ONE: the patch is applied to the record, which provisions the
	// members (message thread — a provision of the outer host, before the instance is adopted, so the
	// drain here runs on an engine nothing else is running). Without a bridge — no catalogue adopted
	// yet — the engine is loaded directly.
	if (bridge_ != nullptr) {
		RackPatch patch;
		if (!parseRack(&stream, &patch)) {
			return false;
		}
		(void)bridge_->loadPatch(patch);
		bridge_->drain();
		bridge_->publish(engine_);
		return source.at == bytes;
	}
	LoadReport refused;
	if (!loadRack(&stream, &engine_, &refused)) {
		return false;
	}
	return source.at == bytes;
}

bool InnerChain::canLoad(const uint8_t* data, size_t bytes) const {
	// THE SAME GATES `loadState` APPLIES, in the same order, so the two cannot disagree about what is
	// acceptable — which is the only thing that makes "check them all, then apply them all" sound.
	if (data == nullptr || bytes == 0 || bytes > kMaxBodyBytes) {
		return false;
	}
	if (loadRefuses(containerLoadDepth()) || !chainHolds(engine_.catalogue().depth)) {
		return false;
	}
	ByteSource source { data, bytes, 0 };
	const clap_istream stream { &source, &ByteSource::read };
	RackPatch patch;
	// `parseRack` writes only into `patch` and walks the stream; it touches no record and no engine,
	// which is what makes this question askable without answering it. AND THE DEPTH GATE: content too tall for
	// where this chain stands is refused whole, as a preset or a site's apply asks it to be (`fitsNesting`).
	return parseRack(&stream, &patch) && source.at == bytes
	       && fitsNesting(engine_.catalogue().depth, patchHeight(patch, engine_.catalogue()));
}

bool InnerChain::applyState(const uint8_t* data, size_t bytes) {
	if (bridge_ == nullptr) {
		return false;
	}
	RackPatch patch;
	if (bytes == 0) {
		// AN EMPTY BODY IS AN EMPTY CHAIN — present, with nothing in it. `present == false` would be
		// "a fresh instance" to `loadPatch`, which changes nothing, and that is not what was asked.
		patch.present = true;
	}
	else {
		if (!canLoad(data, bytes)) {
			return false;
		}
		ByteSource source { data, bytes, 0 };
		const clap_istream stream { &source, &ByteSource::read };
		if (!parseRack(&stream, &patch)) {
			return false;
		}
	}
	// THE RECORD ONLY. What the routing rule refused is kept by the bridge until a surface says it.
	(void)bridge_->loadPatch(patch);
	return true;
}

void InnerChain::forEachRouteFrom(device::Source outer, void* ctx,
                                  void (*fn)(void* ctx, device::Destination inner, float amount)) const {
	if (bridge_ == nullptr || fn == nullptr) {
		return;
	}
	// THE OUTER SOURCE ARRIVES SPELLED THE OUTER WAY and is looked up as this chain's table holds it —
	// with the scope bit on.
	const backplane::Source key = outer.asOuter();
	bridge_->forEachRoute([&](const backplane::Route& r) {
		if (r.source == key) {
			fn(ctx, r.destination, r.amount);
		}
	});
}

float InnerChain::routeAmountFrom(device::Source outer, device::Destination inner) const {
	return bridge_ != nullptr ? bridge_->routeAmount(outer.asOuter(), inner) : 0.0f;
}

bool InnerChain::setRouteFrom(device::Source outer, device::Destination inner, float amount) {
	if (bridge_ == nullptr) {
		return false;
	}
	const backplane::Source key = outer.asOuter();
	if (amount != 0.0f && !bridge_->mayRoute(key, inner)) {
		return false; // the gate inside, asked as the inner editor would ask it
	}
	bridge_->setRouteAmount(key, inner, amount);
	return true;
}

device::Transfer InnerChain::routeTransferFrom(device::Source outer, device::Destination inner) const {
	return bridge_ != nullptr ? bridge_->routeTransfer(outer.asOuter(), inner) : device::Transfer::Lin;
}

bool InnerChain::setRouteTransferFrom(device::Source outer, device::Destination inner, device::Transfer t) {
	return bridge_ != nullptr && bridge_->setRouteTransfer(outer.asOuter(), inner, t);
}

size_t InnerChain::nameDestination(device::Destination inner, char* out, size_t capacity) const {
	if (out == nullptr || capacity == 0) {
		return 0;
	}
	out[0] = '\0';
	const device::Descriptor* d = bridge_ != nullptr ? bridge_->descriptorOf(inner.device) : nullptr;
	if (d == nullptr || inner.index >= d->params.size()) {
		return 0;
	}
	const int n = std::snprintf(out, capacity, "%s · %s", d->traits.name, d->params[inner.index].name);
	return n > 0 ? std::min(static_cast<size_t>(n), capacity - 1) : 0;
}

//------------------------------------------------------------------------------------------------//
// THE OUTWARD CONTROLS                                                                             //
//------------------------------------------------------------------------------------------------//

/// AIM THE OUTPUT RAMP at what the controls say, from wherever it is. Enabled: the chain's output at
/// `mix` and the signal that entered at `1 - mix`, through the fader. Bypassed: the signal that
/// entered, at unity — a bypass fades to the dry path rather than cutting (trap T6).
void InnerChain::aim() {
	const float g = faderGain(gain_);
	wetTarget_ = enabled_ ? mix_ * g : 0.0f;
	dryTarget_ = enabled_ ? (1.0f - mix_) * g : 1.0f;
	voiceWetTarget_ = enabled_ ? 1.0f : 0.0f;
	if (!primed_) {
		wetNow_ = wetTarget_;
		dryNow_ = dryTarget_;
		voiceWetNow_ = voiceWetTarget_;
		wetStep_ = 0.0f;
		dryStep_ = 0.0f;
		voiceWetStep_ = 0.0f;
		remaining_ = 0;
		return;
	}
	const float n = static_cast<float>(rampSamples_);
	wetStep_ = (wetTarget_ - wetNow_) / n;
	dryStep_ = (dryTarget_ - dryNow_) / n;
	voiceWetStep_ = (voiceWetTarget_ - voiceWetNow_) / n;
	remaining_ = rampSamples_;
}

void InnerChain::setEnabled(bool enabled) {
	enabled_ = enabled;
	aim();
}

void InnerChain::setMix(float mix) {
	mix_ = std::clamp(mix, 0.0f, 1.0f);
	aim();
}

void InnerChain::setGain(float position) {
	gain_ = std::clamp(position, 0.0f, 1.0f);
	aim();
}

//------------------------------------------------------------------------------------------------//
// AUDIO THREAD                                                                                     //
//------------------------------------------------------------------------------------------------//

void InnerChain::setTransportClock(const device::TransportClock& clock) { clock_ = clock; }

void InnerChain::setNoteContext(const device::NoteContext& note) { engine_.setExternalNote(note); }

void InnerChain::setOuterSources(const device::OuterSources& outer) { engine_.setOuterSources(outer); }

void InnerChain::setCoreBipolar(size_t index, bool bipolar) { engine_.setMacroBipolar(index, bipolar); }

void InnerChain::publishCoreSource(size_t index, float value) {
	// **THE SOURCE'S OWN READING IN, THE MACRO CELL'S SPELLING OUT.** `RackEngine::setMacro` stores
	// `2v - 1` for a cell declared bipolar, because a macro is a 0..1 parameter a hand holds and the
	// bipolar switch is about how a ROUTE reads it (backlog EG). A holder's published source is not a
	// parameter: a bipolar one already reads -1..+1. Undoing the engine's mapping here is what makes
	// `publishCoreSource(i, performOutput(i))` correct for both polarities with no arithmetic at the
	// call site — and getting it wrong is silent, since `2x - 1` over -1..+1 lands in -3..+1 and only
	// a value assertion sees it.
	const float v = engine_.macroBipolar(index) ? 0.5f * (std::clamp(value, -1.0f, 1.0f) + 1.0f)
	                                            : std::clamp(value, 0.0f, 1.0f);
	engine_.setMacro(index, v);
}

/// End the inner block in flight, if one is.
void InnerChain::endBegun() {
	if (blockBegun_) {
		engine_.endBlock();
		blockBegun_ = false;
	}
}

/// Begin an inner block, if none is in flight. NO FRAMES: the inner modulators advance at the END of
/// each bus chunk, by that chunk, so a block begun here resolves its rows against where the modulators
/// stand after the audio so far. A begin is not a drain: pending edits are adopted at the host's begin
/// (`beginPerVoiceBlock`), the one drain point while hosted.
void InnerChain::beginInner() {
	if (!blockBegun_) {
		engine_.beginBlock(0, backplane::BlockContext { clock_ });
		blockBegun_ = true;
		++blocksBegun_;
	}
}

void InnerChain::beginHostBlock() {
	// THE PER-CALL DRAIN'S CONDITION, ONCE: a two-stage host drains at its own begin.
	if (!hosted_ && !blockBegun_ && bridge_ != nullptr) {
		bridge_->drain();
	}
	hostLatency_ = engine_.latencySamples(sampleRate_);
	inHostBlock_ = true;
}

void InnerChain::endHostBlock() {
	inHostBlock_ = false;
	if (bridge_ != nullptr) {
		bridge_->publish(engine_);
	}
}

bool InnerChain::beginPerVoiceBlock() {
	// THE HOST'S BEGIN-BLOCK, FORWARDED: the drain point, and the answer. A chunk's block left in
	// flight (the host skipped the bus stage) is ended here; then pending edits are adopted, and the
	// first inner block of the host's block is begun to resolve the inner rows, whose Poly fields are
	// the answer.
	hosted_ = true;
	endBegun();
	if (bridge_ != nullptr) {
		bridge_->drain();
	}
	beginInner();
	// AND WHILE A BYPASS IS STILL FADING, the per-voice road keeps running so the fade has something
	// to fade (trap T6); `processVoice` blends by `voiceWetNow_`.
	return (enabled_ || remaining_ > 0) && engine_.anyPerVoice();
}

void InnerChain::processVoice(size_t voice, float* left, float* right, int frames, const device::NoteContext& note) {
	// ONE VOICE THROUGH THE INNER DEVICES THAT RUN PER VOICE, with the voice's note. A STEREO voice runs as its
	// pair; a MONO voice's channel (`right` null) is mirrored into a pair (the chain is stereo) and averaged back —
	// a stereo per-voice device inside is heard at its mid. The chain's mix and gain apply on the bus, where there
	// is a dry to mix against; here the voice carries the wet. The chain body's own road, kept its twin (backlog KT:
	// both took one channel, so a stereo voice's right ear passed the inner Poly devices dry).
	if (left == nullptr || frames <= 0 || (!enabled_ && remaining_ == 0) || monoLeft_.empty()) {
		return;
	}
	beginInner();
	// Read once per call — the bus stage advances it after the voices have run this block.
	const float wet = voiceWetNow_;
	int done = 0;
	while (done < frames) {
		const int n = std::min(frames - done, static_cast<int>(monoLeft_.size()));
		const auto bytes = static_cast<size_t>(n) * sizeof(float);
		std::memcpy(monoLeft_.data(), left + done, bytes);
		std::memcpy(monoRight_.data(), (right != nullptr ? right : left) + done, bytes);
		if (sampleRate_ > 0.0) { // this piece's own sample — see `RackEngine::setClock`
			engine_.setClock(rack::RackEngine::advanced(clock_, static_cast<double>(done) / sampleRate_));
		}
		engine_.processVoice(voice, monoLeft_.data(), monoRight_.data(), n, note);
		for (int i = 0; i < n; ++i) {
			const auto k = static_cast<size_t>(i);
			if (right != nullptr) {
				left[done + i] += wet * (monoLeft_[k] - left[done + i]);
				right[done + i] += wet * (monoRight_[k] - right[done + i]);
			}
			else {
				const float dry = left[done + i];
				const float processed = 0.5f * (monoLeft_[k] + monoRight_[k]);
				left[done + i] = dry + wet * (processed - dry);
			}
		}
		done += n;
	}
}

void InnerChain::process(float* left, float* right, int frames) {
	// **THE DRAIN AND THE PUBLISH, THE STANDALONE PLUGIN'S PAIR AROUND ITS PROCESS**: pending edits
	// from the chain's editor are adopted here, on the audio thread, before the inner block runs; the
	// mirror the editor reads (ids, readings, playheads) is published after it. **EVERY RETURN PATH
	// PUBLISHES**, which is what the destructor buys and a call at the bottom would not (trap T3).
	struct Publish {
		InnerChain& chain;
		~Publish() {
			if (chain.bridge_ != nullptr && !chain.inHostBlock_) {
				chain.bridge_->publish(chain.engine_);
			}
		}
	} publish { *this };
	// HOSTED, THE DRAIN HAPPENED AT THE HOST'S BEGIN and this is the bus stage of a chunk whose inner
	// block may already be in flight from the per-voice stage. A host without the two-stage road never
	// begins; then the bus is the whole block and drains here.
	if (!hosted_ && !blockBegun_ && !inHostBlock_ && bridge_ != nullptr) {
		bridge_->drain();
	}
	if (frames <= 0 || left == nullptr || right == nullptr || dryLeft_.empty()) {
		return;
	}
	// HOW LATE THE CHAIN IS, AFTER THIS BLOCK'S DRAIN (backlog JM) — see `rack_device.cpp`, whose mix this is.
	const uint32_t latency = inHostBlock_ ? hostLatency_ : engine_.latencySamples(sampleRate_);
	// BYPASSED AND SETTLED IS A TRUE PASS, bit for bit; bypassed and still ramping finishes its fade.
	// Nothing runs inside, so nothing stays in flight: the inner editor can reap while the chain is off.
	// A PASS THAT WAITS AS LONG AS THE CHAIN WOULD when something inside is latent (assumption L3).
	if (!enabled_ && remaining_ == 0 && dryNow_ == 1.0f && wetNow_ == 0.0f) {
		primed_ = true;
		endBegun();
		dryDelay_.process(left, right, frames, latency);
		return;
	}
	primed_ = true;

	// **A CHAIN HOLDING NOTHING IS A TRUE PASS, BIT FOR BIT, AND THIS IS ABOUT CORRECTNESS BEFORE IT IS
	// ABOUT COST.** The blend below is `x·wetNow + dryCopy·dryNow`, which at wet 1 and dry 0 is
	// `x·1 + y·0` — equal to `x` for every finite x EXCEPT −0.0, where `−0.0 + 0.0` is `+0.0`. A holder
	// that gained a post chain would therefore have changed the sign of its zeros on every block, for a
	// chain nobody had put anything in. The cost saved (a buffer copy and two multiply-adds per sample, per
	// instance, forever) is real too, but it is the second reason.
	//
	// **THE TICK PHASE STILL ADVANCES**, because it is this chain's position in the HOST's stream and
	// not a count of the work done: a chain that stood still while empty would put its inner grid at an
	// arbitrary offset from the outer one the moment a device was added (backlog FK).
	//
	// GUARDED ON ALL FOUR: empty, settled, and the ramp actually AT unity/zero — a chain whose mix is
	// 0.5 still has to blend even with nothing in it, because half of nothing is not the input.
	if (engine_.chainLength(0) == 0 && remaining_ == 0 && wetNow_ == 1.0f && dryNow_ == 0.0f) {
		endBegun();
		tickPhase_ = (tickPhase_ + frames) % device::kTickWindow;
		return;
	}

	const backplane::BlockContext block { clock_ };
	size_t done = 0;
	const auto total = static_cast<size_t>(frames);
	while (done < total) {
		// THE CHAIN'S BUS STAGE, IN THIS CHUNK'S INNER BLOCK. Chunks are cut on the inner control tick
		// (trap T4): the inner devices read the tick's windows at their phase, and when a tick closes
		// every inner modulator advances by it and the block ends, so the next chunk's block resolves
		// its rows against them — the grid Feathers' lane and the standalone rack run on.
		beginInner();
		engine_.setTickPhase(tickPhase_);
		// THIS PIECE'S OWN SAMPLE, not the call's first — see `RackEngine::setClock` (the 2026-09-24 crush).
		if (sampleRate_ > 0.0) {
			engine_.setClock(rack::RackEngine::advanced(clock_, static_cast<double>(done) / sampleRate_));
		}
		// AND CUT WHERE A LATE DEVICE INSIDE IS HANDED ITS NOTE OR ITS ROW (backlog LW): `UINT32_MAX` with nothing late.
		const size_t n = std::min({ dryLeft_.size(), total - done, static_cast<size_t>(device::kTickWindow - tickPhase_),
		                            static_cast<size_t>(engine_.samplesUntilLateNote()) });
		float* l = left + done;
		float* r = right + done;
		std::copy(l, l + n, dryLeft_.begin());
		std::copy(r, r + n, dryRight_.begin());
		dryDelay_.process(dryLeft_.data(), dryRight_.data(), static_cast<int>(n), latency);
		engine_.processBus(l, r, static_cast<int>(n));
		tickPhase_ = (tickPhase_ + static_cast<int>(n)) % device::kTickWindow;
		engine_.tickModulators(tickPhase_ == 0 ? device::kTickWindow : 0, block);
		endBegun();

		for (size_t i = 0; i < n; ++i) {
			if (remaining_ > 0) {
				wetNow_ += wetStep_;
				dryNow_ += dryStep_;
				voiceWetNow_ += voiceWetStep_;
				if (--remaining_ == 0) {
					wetNow_ = wetTarget_;
					dryNow_ = dryTarget_;
					voiceWetNow_ = voiceWetTarget_;
				}
			}
			l[i] = l[i] * wetNow_ + dryLeft_[i] * dryNow_;
			r[i] = r[i] * wetNow_ + dryRight_[i] * dryNow_;
		}
		done += n;
	}
}

} // namespace feathers::rack
