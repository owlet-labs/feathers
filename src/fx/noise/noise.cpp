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

#include "fx/noise/noise_dsp.h"
#include "fx/noise/noise_types.h"

#include "fx/dyn/meter_ramp.h"
#include "device/tick_window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

// NOISE'S RENDER PATH — generate, tilt or click, filter, add.
//
// **ONE SAMPLE AT A TIME, AND NOTHING KEYED ON THE BLOCK.** The ramps, the generators and the filter's
// coefficient refresh (every `kCoeffStep` samples of a running count, not of the block) all advance per sample,
// so a host that cuts a block anywhere gets the same bits (the partition sweep's rule).

namespace feathers::fx::noise {

namespace {

/// HOW OFTEN THE FILTER'S CORNER IS RE-DESIGNED FROM ITS RAMP, in samples — a `tan` every 16 samples.
constexpr int kCoeffStep = 16;
/// HOW OFTEN THE TILT IS RE-DESIGNED WHILE COLOUR IS MOVING, in samples. Its design integrates a power grid, so
/// it is the costlier of the two and Colour is a slow control.
constexpr int kTiltStep = 64;

/// **FREE'S STREAMS START FROM ONE FIXED SEED, NOT THE SEED CONTROL AND NOT THE INSTANCE.** What makes Free free is that
/// nothing restarts it — a reset (a note, per voice) leaves the streams running — not that two instances differ at
/// birth. Seeding from the instance would make a device depend on how many were built before it, so two hosts building
/// the same patch would not render the same bits. The cost, reasoned rather than measured: a per-voice chord on voices that have
/// never sounded opens on one noise in every voice; voices that have played diverge at once.
constexpr uint64_t kFreeSeed = 0x5eedf4ee5eedf4eeull;

struct OpsNoise {
	Settings state {};
	double sampleRate = 48000.0;
	double secondsPerBeat = 0.0;
	float notePitch = kKeyTrackReferenceKey;

	WhiteNoise whiteL, whiteR;
	dsp::RandomEvents eventsL, eventsR;
	dsp::RandomEventSettings events {};

	Tilt tilt {};
	float tiltColour = 0.5f;
	TiltState tiltL, tiltR;

	Svf svf {};
	SvfState svfL, svfR;

	dyn::LinearRamp level, stereo, bypass, colour, cutoff, resonance;
	/// THE EAR MIX: the right ear is cos(theta) x left + sin(theta) x its own stream.
	float stereoKey = -1.0f, stereoCos = 1.0f, stereoSin = 0.0f;

	/// A RUNNING SAMPLE COUNT, so the coefficient refreshes land on the same samples however a block is cut.
	uint32_t clock = 0;
	/// WHAT THE STREAMS WERE LAST SEEDED FROM, so a Stable seed that moves reseeds once.
	int seededWith = -1;
	/// TRUE ONCE THE PASS-THROUGH HAS CLEARED THE MEMORY, so it is cleared once per landing.
	bool idle = false;
	/// **FREE: EACH NOTE'S VOICE IS TURNED ONTO ITS OWN STREAMS.** Voice instances are built alike, and a chord of
	/// identical noises sums coherently (+6 dB a doubling, and phasey) instead of as independent ones. So the host's
	/// VOICE SLOT (`AudioOps::setVoice`) is folded into Free's seed — voices started together on one note differ — and
	/// at each start (a reset) the note's PITCH and this instance's count of starts are folded into the running streams:
	/// not a restart, a change of course, so a voice's next note does not replay its last.
	/// Deterministic: the same events render the same bits. An instance no host told its voice (the bus one) folds no
	/// slot.
	uint64_t voiceKey = 0;
	/// THE SLOT THE HOST NAMED, or -1 for an instance never told (the bus one). Read by tests only.
	int64_t voiceSlot = -1;
	uint32_t starts = 0;
	bool mixPending = false;

	/// **THE TRIG** (backlog MZ, MV's convention): its edge, watched where the row arrives (`setFields`), and the hits it
	/// fires — one per rising edge under Impulses, from `fire`, a stream of their own seeded beside the left ear's
	/// (`mod::rtrig::fireSeed`, Random Trigger's rule, so the two fired alike on one Seed hit alike). A fire is OWED until
	/// the next tick publishes it and hands it to the audio (`fireAudio`), so its click and its Impulse Trig share a tick
	/// as the generator's do; an instance nothing ticks draws it at its next sample instead.
	mod::TrigEdge trigEdge;
	mod::rtrig::FireStream fire;
	bool firePending = false;
	/// A FIRED HIT THE AUDIO PLAYS AT ITS NEXT SAMPLE, in both ears alike: a click is not a stream, so it is not spread.
	float fireAudio = 0.0f;
	/// HOW MANY TIMES THE TRIG HAS FIRED — the card's pad lights on it (`ModOps::trigFires`).
	uint32_t fires = 0;

	void mixVoice(float pitch) {
		uint32_t bits = 0;
		std::memcpy(&bits, &pitch, sizeof(bits));
		const uint64_t k = streamSeed(kFreeSeed ^ voiceKey ^ (static_cast<uint64_t>(starts) << 32) ^ bits, Stream::NoiseLeft);
		for (WhiteNoise* w : { &whiteL, &whiteR }) {
			w->state ^= streamSeed(k, w == &whiteL ? Stream::NoiseLeft : Stream::NoiseRight);
			if (w->state == 0) {
				w->state = 0x9e3779b97f4a7c15ull;
			}
		}
		eventsL.mix(streamSeed(k, Stream::EventsLeft));
		eventsR.mix(streamSeed(k, Stream::EventsRight));
		fire.setState(fire.state() ^ mod::rtrig::fireSeed(streamSeed(k, Stream::EventsLeft)));
	}

	/// THE STREAMS START AGAIN FROM A SEED — the Stable seed, or Free's fixed one.
	void reseed() {
		const bool stable = state.seedMode == static_cast<int>(SeedMode::Stable);
		const uint64_t seed = stable ? static_cast<uint64_t>(state.seed)
		                             : (kFreeSeed ^ voiceKey);
		whiteL.seed(streamSeed(seed, Stream::NoiseLeft));
		whiteR.seed(streamSeed(seed, Stream::NoiseRight));
		eventsL.seed(streamSeed(seed, Stream::EventsLeft));
		eventsR.seed(streamSeed(seed, Stream::EventsRight));
		fire.seed(mod::rtrig::fireSeed(streamSeed(seed, Stream::EventsLeft)));
		seededWith = stable ? state.seed : -1;
	}

	/// THE STABLE SEED MOVED (or the mode came back to Stable): the pattern restarts from it now.
	void reseedIfMoved() {
		const bool stable = state.seedMode == static_cast<int>(SeedMode::Stable);
		// AND INTO FREE, ONCE, onto Free's own streams — so a Free device opens on the same noise whatever Stable seed it
		// was switched from.
		if ((stable && state.seed != seededWith) || (!stable && seededWith != -1)) {
			reseed();
		}
	}

	[[nodiscard]] double cornerHz(float cutoffLog2) const {
		const float semis = state.keyTrack * (notePitch - kKeyTrackReferenceKey);
		return std::exp2(static_cast<double>(cutoffLog2) + static_cast<double>(semis) / 12.0);
	}

	/// FROM THE STORED SETTINGS, not the ramps: what a fresh or reset device opens on, and what a new response starts
	/// from. A moving corner is re-designed from its ramp in `process`.
	void designFilter() { svf.design(state.filterType, cornerHz(state.cutoff), state.resonance, sampleRate); }

	void designTilt(float c) {
		tiltColour = c;
		tilt.design(c, sampleRate);
	}

	void aim() {
		level.set(state.level);
		stereo.set(state.stereo);
		bypass.set(state.enabled ? 1.0f : 0.0f);
		colour.set(state.colour);
		cutoff.set(state.cutoff);
		resonance.set(state.resonance);
		events = eventSettingsOf(state, secondsPerBeat);
	}

	/// EVERYTHING WITH MEMORY FORGETS IT. The streams are the caller's business: Stable reseeds, Free does not.
	void clearFilters() {
		tiltL.clear();
		tiltR.clear();
		svfL.clear();
		svfR.clear();
	}

	/// THE DESIGNS FROM THE STORED SETTINGS. The ramps are left unprimed, so the first block after a construction or a
	/// reset SNAPS to its settings (`LinearRamp::set`) rather than gliding in from a default nobody asked for.
	void designFromState() {
		designTilt(state.colour);
		designFilter();
	}

	/// ONE EAR'S SOURCE SAMPLE, before the filter.
	[[nodiscard]] float generate(WhiteNoise& white, TiltState& t, dsp::RandomEvents& ev) {
		if (state.source == static_cast<int>(Source::Impulses)) {
			return ev.next(events);
		}
		return t.step(tilt, white.next());
	}

	//--------------------------------------------------------------------------------------------//
	// THE MOD ROAD (backlog NA (b)) — the left ear's hits, drawn a tick AHEAD and then played      //
	//--------------------------------------------------------------------------------------------//
	//
	// **THE CLICK AND ITS EVENT LAND ON THE SAME TICK, BY CONSTRUCTION.** A host ticks the mod road at the start of a
	// control window, before the window's audio. So the tick draws the window's `kWindow` samples of the left ear's
	// generator into `schedule` and publishes them, and the audio then PLAYS the schedule instead of drawing: one draw,
	// two readers, and a filter routed from Impulse Trig opens on the window its click sounds in rather than the next.
	// An instance nothing ticks (a test on the audio road alone) never has a schedule and draws for itself, to the bit.
	// The cost, approved: on a ticked instance the generator runs while Level is 0, where it used to rest.
	/// THE HOST'S CONTROL WINDOW (`device::kTickWindow`), the length the mod road declares and the rack ticks on.
	static constexpr int kWindow = device::kTickWindow;
	std::array<float, kWindow> schedule {};
	/// THE NEXT SAMPLE OF THE SCHEDULE THE AUDIO TAKES; `kWindow` once it is spent (the audio then draws for itself).
	int cursor = kWindow;
	float publishedTrig = 0.0f;
	float publishedSize = 0.0f;
	/// THE IMPULSE DECAY OUTPUT — Random Trigger's Decay, the same `HitDecay` at the same per-tick factor.
	mod::rtrig::HitDecay publishedDecay {};

	/// THE HELD IMPULSE AS LAST PUBLISHED, for the patch (`kStateOps`; backlog NM): written at every tick.
	device::BlockSnapshot<kHeldWords> heldSnapshot;

	void publishHeld() noexcept {
		std::array<float, kHeldWords> words {};
		device::BlockWriter w(words.data(), words.size());
		w.f32(publishedSize);
		w.f32(publishedDecay.value());
		heldSnapshot.publish(words.data(), words.size());
	}

	[[nodiscard]] bool loadHeld(const float* in, size_t n) noexcept {
		device::BlockReader r(in, n);
		const float size = r.f32();
		const float decay = r.f32();
		if (!r.ok() || !std::isfinite(size) || !std::isfinite(decay)) {
			return false;
		}
		publishedSize = size;
		publishedDecay.hit(decay);
		publishHeld();
		return true;
	}

	void forgetSchedule() {
		cursor = kWindow;
		publishedTrig = 0.0f;
		publishedSize = 0.0f;
		publishedDecay.reset();
	}

	void tickEvents(const device::TransportClock& clock) {
		secondsPerBeat = clock.secondsPerBeat;
		publishedTrig = 0.0f;
		if (state.source != static_cast<int>(Source::Impulses)) {
			cursor = kWindow;
			publishedSize = 0.0f;
			publishedDecay.reset();
			firePending = false;
			publishHeld();
			return;
		}
		// THE DECAY FALLS FIRST, then the window's hits start it again — `rtrig::Shape::tick`'s order, so a click stands at
		// its full value for the tick it sounds in.
		publishedDecay.fall(mod::rtrig::decayFactor(state.decay, static_cast<double>(kWindow) / sampleRate));
		const dsp::RandomEventSettings e = eventSettingsOf(state, secondsPerBeat);
		// A FIRED HIT LEADS THE WINDOW (`rtrig::Shape::tick`'s order): published now, played at the window's first sample.
		if (std::exchange(firePending, false)) {
			const float v = fire.next(e);
			publishedTrig = 1.0f;
			publishedSize = v;
			publishedDecay.hit(v);
			fireAudio = v;
		}
		for (int i = 0; i < kWindow; ++i) {
			const float v = eventsL.next(e);
			schedule[static_cast<size_t>(i)] = v;
			if (v != 0.0f) {
				publishedTrig = 1.0f;
				publishedSize = v;
				publishedDecay.hit(v);
			}
		}
		cursor = 0;
		publishHeld();
	}

	void process(float* left, float* right, int frames) {
		aim();
		for (int i = 0; i < frames; ++i) {
			// THE SCHEDULE'S SAMPLE IS TAKEN WHETHER OR NOT ANYTHING IS WRITTEN, so the cursor walks the window in step
			// with the audio however a block is cut.
			const bool scheduled = cursor < kWindow;
			const float hit = scheduled ? schedule[static_cast<size_t>(cursor++)] : 0.0f;
			// A FIRE NO TICK WILL PUBLISH (nothing ticks this instance, or its window is spent) is drawn here. Either way
			// the owed hit is taken now, written or not, so a fire under Level 0 is spent rather than kept for later.
			if (firePending && !scheduled) {
				firePending = false;
				fireAudio = fire.next(events);
			}
			const float fired = std::exchange(fireAudio, 0.0f);
			// **SWITCHED OFF, OR AT LEVEL 0, THE BUFFER IS NOT WRITTEN** — the pass-through is the input to the bit,
			// decided per sample (so a block cut cannot move it). The memory is cleared once, so the noise comes
			// back from rest rather than from whatever it was ringing with.
			const bool off = !state.enabled && bypass.settled() && bypass.value() == 0.0f;
			const bool silent = level.settled() && level.value() == 0.0f;
			if (off || silent) {
				if (!idle) {
					clearFilters();
					idle = true;
				}
				++clock;
				continue;
			}
			idle = false;
			const float b = bypass.next();
			const float gain = levelGain(level.next()) * b;
			const float st = stereo.next();
			const float c = colour.next();
			const float co = cutoff.next();
			const float re = resonance.next();
			if (clock % static_cast<uint32_t>(kTiltStep) == 0 && c != tiltColour) {
				designTilt(c);
			}
			if (clock % static_cast<uint32_t>(kCoeffStep) == 0) {
				svf.design(state.filterType, cornerHz(co), re, sampleRate);
			}
			++clock;

			const bool clicks = state.source == static_cast<int>(Source::Impulses);
			const float a = clicks && scheduled ? hit : generate(whiteL, tiltL, eventsL);
			left[i] += gain * svfL.step(svf, a + fired);
			if (clicks) {
				svfL.flushTiny();
			}
			if (right == nullptr) {
				continue;
			}
			if (st != stereoKey) {
				stereoKey = st;
				const float theta = 0.5f * std::numbers::pi_v<float> * st;
				stereoCos = std::cos(theta);
				stereoSin = std::sin(theta);
			}
			// THE RIGHT EAR'S OWN STREAM IS DRAWN ONLY WHILE IT IS HEARD: at Stereo 0 the ears are one signal.
			const float own = stereoSin != 0.0f ? generate(whiteR, tiltR, eventsR) : 0.0f;
			right[i] += gain * svfR.step(svf, stereoCos * a + stereoSin * own + fired);
			if (clicks) {
				svfR.flushTiny();
			}
		}
	}
};

OpsNoise& asNoise(void* storage) { return *static_cast<OpsNoise*>(storage); }
const OpsNoise& asNoise(const void* storage) { return *static_cast<const OpsNoise*>(storage); }

} // namespace

double tiltGainForTest(const void* storage, double hz) { return asNoise(storage).tilt.gain(hz); }

double filterCornerForTest(const void* storage) { return asNoise(storage).svf.cornerHz(); }

double filterGainForTest(const void* storage, double hz) { return asNoise(storage).svf.gain(hz); }

double hitRateForTest(const void* storage) { return asNoise(storage).events.rateHz; }

int64_t voiceForTest(const void* storage) { return asNoise(storage).voiceSlot; }

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity) {
	return index == 0 ? asNoise(storage).heldSnapshot.read(out, capacity) : 0;
}

bool restoreBlock(void* storage, size_t index, const float* in, size_t n) {
	return index == 0 && n == kHeldWords && asNoise(storage).loadHeld(in, n);
}

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(OpsNoise),
    .stateAlign = alignof(OpsNoise),
    .construct =
        [](void* storage) {
	        new (storage) OpsNoise();
	        OpsNoise& ops = asNoise(storage);
	        ops.reseed();
	        ops.designFromState();
	        ops.events = eventSettingsOf(ops.state, ops.secondsPerBeat);
        },
    .destruct = [](void* storage) { asNoise(storage).~OpsNoise(); },
    .prepare =
        [](void* storage, double sampleRate, int) {
	        OpsNoise& ops = asNoise(storage);
	        ops.sampleRate = std::max(sampleRate, 1.0);
	        for (dyn::LinearRamp* r : { &ops.level, &ops.stereo, &ops.bypass, &ops.colour, &ops.cutoff,
		                                     &ops.resonance }) {
		        r->prepare(ops.sampleRate);
	        }
	        ops.eventsL.setSampleRate(ops.sampleRate);
	        ops.eventsR.setSampleRate(ops.sampleRate);
	        ops.clearFilters();
	        ops.designFromState();
        },
    // **AS IF JUST SWITCHED ON** — and for a Stable seed that includes the pattern: a per-voice instance is reset
    // at every note, so Stable means every note opens on the same noise. Free keeps its streams running.
    .reset =
        [](void* storage) {
	        OpsNoise& ops = asNoise(storage);
	        ops.clearFilters();
	        ops.idle = false;
	        ops.forgetSchedule(); // a reset is a note start per voice: the audio draws until the next tick
	        ops.clock = 0;
	        ops.trigEdge.reset();
	        ops.firePending = false;
	        ops.fireAudio = 0.0f;
	        if (ops.state.seedMode == static_cast<int>(SeedMode::Stable)) {
		        ops.reseed();
	        }
	        else {
		        ++ops.starts;
		        ops.mixPending = true; // folded in when the note arrives (`setNoteContext`)
	        }
	        ops.eventsL.setSampleRate(ops.sampleRate);
	        ops.eventsR.setSampleRate(ops.sampleRate);
	        for (dyn::LinearRamp* r : { &ops.level, &ops.stereo, &ops.bypass, &ops.colour, &ops.cutoff,
		                                     &ops.resonance }) {
		        r->reset();
	        }
	        ops.designFromState();
        },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        // THE DEVICE DECODES ITS OWN ROW, so absent fields land on their DECLARED defaults.
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        OpsNoise& ops = asNoise(storage);
	        const int typeWas = ops.state.filterType;
	        ops.state = stateOf(row, take);
	        ops.reseedIfMoved();
	        // THE TRIG'S EDGE, ON THE ROW AS RESOLVED — both roads hand a dual instance the same row, and an edge read
	        // twice on one value fires once. It fires only where it is read: under Impulses.
	        if (ops.trigEdge.step(ops.state.trig) && ops.state.source == static_cast<int>(Source::Impulses)) {
		        ops.firePending = true;
		        ++ops.fires;
	        }
	        if (ops.state.filterType != typeWas) {
		        // A NEW RESPONSE STARTS FROM REST: the old one's memory read through new outputs is a click.
		        ops.svfL.clear();
		        ops.svfR.clear();
		        ops.designFilter();
	        }
        },
    // THE NOTE KEY TRACK FOLLOWS — per voice, the voice's own; on the bus, the one the host picked.
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) {
	        OpsNoise& ops = asNoise(storage);
	        ops.notePitch = note.pitchSemitones;
	        if (ops.mixPending && ops.state.seedMode == static_cast<int>(SeedMode::Free)) {
		        ops.mixVoice(note.pitchSemitones);
	        }
	        ops.mixPending = false;
        },
    .setTransportClock = [](void* storage,
                            const device::TransportClock& clock) { asNoise(storage).secondsPerBeat = clock.secondsPerBeat; },
    .processStereo = [](void* storage, float* left, float* right, int frames) { asNoise(storage).process(left, right, frames); },
    // THE MONO ROAD IS THE LEFT EAR: the same stream, the same filter, with no right ear drawn.
    .processMono = [](void* storage, float* mono, int frames) { asNoise(storage).process(mono, nullptr, frames); },
    .windowedControls = {},
    .setFieldWindows = nullptr,
    // **IT GENERATES, SO IT HOLDS SOUND WHENEVER IT CAN MAKE ANY** (backlog MZ). Sparse impulses leave whole blocks
    // exactly silent between clicks, and a host that slept on one would drop the next click. Enabled with Level up
    // is "more is coming"; off or at 0 it adds nothing, and nothing it holds can come out without an event.
    // Asked only of a bus instance: a per-voice instance ends with its voice.
    .holdsTail =
        [](const void* storage) {
	        const OpsNoise& ops = asNoise(storage);
	        return ops.state.enabled && ops.state.level > 0.0f;
        },
    .setSidechain = nullptr,
    // THE VOICE SLOT, folded into Free's seed (see `voiceKey`). A Free instance already running is turned onto the
    // slot's streams at once; Stable does not read it.
    .setVoice =
        [](void* storage, size_t voice) {
	        OpsNoise& ops = asNoise(storage);
	        ops.voiceSlot = static_cast<int64_t>(voice);
	        ops.voiceKey = streamSeed(static_cast<uint64_t>(voice) + 1u, Stream::NoiseRight);
	        if (ops.state.seedMode == static_cast<int>(SeedMode::Free)) {
		        ops.reseed();
	        }
        },
};

/// **THE MOD ROAD, OVER THE SAME OBJECT** (backlog NA (b)) — SKLITTER's shape. A host reaches a dual device's lifetime
/// through the AUDIO table (`RackEngine` builds, prepares and resets with `kAudioOps` when it is there), so the entries
/// below that duplicate it describe the same object rather than a second one, and route to the audio table's own.
const device::ModOps kModOps {
    .stateBytes = sizeof(OpsNoise),
    .stateAlign = alignof(OpsNoise),
    .construct = [](void* storage) { kAudioOps.construct(storage); },
    .destruct = [](void* storage) { kAudioOps.destruct(storage); },
    .prepare = [](void* storage, double sampleRate) { kAudioOps.prepare(storage, sampleRate, device::kDefaultMaxFrames); },
    .reset = [](void* storage) { kAudioOps.reset(storage); },
    // THE SAME ROW THE AUDIO ROAD IS HANDED — the Source, the timing and the seed the schedule is drawn with.
    .setFields = [](void* storage, const float* fields, size_t fieldsPresent) { kAudioOps.setFields(storage, fields, fieldsPresent); },
    .blockSamples = static_cast<size_t>(OpsNoise::kWindow),
    .tick = [](void* storage, const device::ModTickContext& ctx) { asNoise(storage).tickEvents(ctx.clock); },
    .outputAt =
        [](const void* storage, size_t output) {
	        const OpsNoise& ops = asNoise(storage);
	        switch (static_cast<Out>(output)) {
	        case Out::Trig: return ops.publishedTrig;
	        case Out::Size: return ops.publishedSize;
	        case Out::Decay: return ops.publishedDecay.value();
	        case Out::kCount: break;
	        }
	        return 0.0f;
        },
    // NO PLAYHEAD: the card draws the generator's own hits from the row (`NoisePlot`).
    .playhead = nullptr,
    // THE TRIG'S FIRES — the card's pad lights on them (under Poly the newest voice's, `RackEngine::trigFiresAt`).
    .trigFires = [](const void* storage) { return asNoise(storage).fires; },
};

} // namespace feathers::fx::noise
