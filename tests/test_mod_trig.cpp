/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE TRIG CONVENTION (backlog MV): a fireable device's Trig control fires when its resolved value rises to 0.5 and
// re-arms below 0.25. Every case asserts a VALUE — the tick a fire lands on, a playhead's stage, a phase — because a
// restart is not recoverable from a rendered output (a looping shape passes its start every cycle anyway).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "device/registry.h"
#include "mod/dahdsr/dahdsr.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/lfo/lfo.h"
#include "mod/lfo/lfo_types.h"
#include "mod/mseg/mseg.h"
#include "mod/mseg/mseg_types.h"
#include "mod/trig.h"
#include "plugin/device_registry.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace feathers;
namespace dahdsr = feathers::mod::dahdsr;
namespace lfo = feathers::mod::lfo;
namespace mseg = feathers::mod::mseg;

namespace {

constexpr double kRate = 48000.0;
constexpr int kTicksPerSecond = static_cast<int>(kRate) / mod::kBlockSamples; // 750

/// ONE MODULATOR, DRIVEN THROUGH ITS OPS TABLES as the rack drives it (test_mod_retrigger_smoothing's rig).
struct Rig {
	const device::ModOps& mod;
	const device::NoteOps& notes;
	void* mem = nullptr;
	device::ModTickContext ctx {};

	Rig(const device::ModOps& m, const device::NoteOps& n) : mod(m), notes(n) {
		const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, kRate);
	}
	~Rig() {
		mod.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Rig(const Rig&) = delete;
	Rig& operator=(const Rig&) = delete;

	template <typename Row>
	void apply(const Row& row) {
		mod.setFields(mem, row.data(), row.size());
		notes.setFields(mem, row.data(), row.size());
	}
	void feed(device::NoteEvent::Kind kind, int16_t key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = -1;
		e.channel = 0;
		e.key = key;
		device::NoteEvent out[4];
		size_t n = 0;
		notes.processNotes(mem, &e, 1, out, 4, &n);
		REQUIRE(n == 1);
	}
	void tick(int times = 1) {
		for (int i = 0; i < times; ++i) {
			mod.tick(mem, ctx);
		}
	}
	float shared() const { return mod.outputAt(mem, 0); }
	float note(int16_t key) const { return mod.outputForNote(mem, 0, -1, 0, key); }
	uint32_t ordinal() const {
		uint32_t o = mod::Playhead::kIdle;
		float p = 0.0f;
		mod.playhead(mem, &o, &p);
		return o;
	}
	float phase() const {
		uint32_t o = mod::Playhead::kIdle;
		float p = -1.0f;
		mod.playhead(mem, &o, &p);
		return p;
	}
};

constexpr uint32_t stageOrdinal(dahdsr::Stage s) {
	return static_cast<uint32_t>(s);
}

/// A DAHDSR rig with `settings`, the trig reading `trig`.
void setEnv(Rig& rig, dahdsr::Settings s, float trig) {
	s.trig = trig;
	rig.apply(dahdsr::rowOf(s));
}

} // namespace

TEST_CASE("trig edge: fires on the reading that reaches 0.5, never before, and re-arms only below 0.25") {
	mod::TrigEdge edge;
	// RISING TOWARD THE LEVEL: nothing until the reading that reaches it.
	CHECK_FALSE(edge.step(0.0f));
	CHECK_FALSE(edge.step(0.3f));
	CHECK_FALSE(edge.step(0.4999f));
	CHECK(edge.step(0.5f)); // inclusive: a pulse to 1 at route depth 0.5 lands exactly here
	// HELD HIGH, AND DIPPING INTO THE GAP: no second fire.
	CHECK_FALSE(edge.step(1.0f));
	CHECK_FALSE(edge.step(0.3f));
	CHECK_FALSE(edge.step(0.25f)); // the re-arm level itself does not re-arm: strictly below
	CHECK_FALSE(edge.step(0.9f));
	// BELOW THE RE-ARM LEVEL, THEN UP: fires again.
	CHECK_FALSE(edge.step(0.2499f));
	CHECK(edge.step(0.5f));
	// A NaN neither fires nor re-arms.
	CHECK_FALSE(edge.step(std::nanf("")));
	CHECK_FALSE(edge.step(0.0f));
	CHECK_FALSE(edge.step(std::nanf("")));
	CHECK(edge.step(0.7f));
}

TEST_CASE("trig edge: a noisy source hovering at the fire level fires once, not once per wobble") {
	mod::TrigEdge edge;
	// 0.5 ± 0.2 — every reading inside the hysteresis gap or above it, crossing 0.5 hundreds of times.
	uint32_t lcg = 12345u;
	int fires = 0;
	int crossings = 0;
	float last = 0.0f;
	for (int i = 0; i < 4000; ++i) {
		lcg = lcg * 1664525u + 1013904223u;
		const float noise = (static_cast<float>(lcg >> 8) / 16777216.0f) * 0.4f - 0.2f;
		const float v = 0.5f + noise;
		crossings += (last < mod::kTrigFireLevel && v >= mod::kTrigFireLevel) ? 1 : 0;
		last = v;
		fires += edge.step(v) ? 1 : 0;
	}
	REQUIRE(crossings > 100); // not vacuous: the source really does cross the fire level over and over
	CHECK(fires == 1);
}

TEST_CASE("DAHDSR Trig: a one-shot fires on the tick the Trig crosses, with no key held") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Once;
	s.poly = false;
	setEnv(rig, s, 0.0f);
	rig.tick(20);
	REQUIRE(rig.ordinal() == mod::Playhead::kIdle);
	REQUIRE(rig.shared() == 0.0f);

	// THE TICK BEFORE: 0.49 does not fire.
	setEnv(rig, s, 0.49f);
	rig.tick();
	CHECK(rig.ordinal() == mod::Playhead::kIdle);
	CHECK(rig.shared() == 0.0f);

	// THE CROSSING TICK: the attack is running and the output has left zero, on this tick and not a later one.
	setEnv(rig, s, 0.5f);
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
	CHECK(rig.shared() > 0.0f);

	// AND IT RUNS ITS WHOLE SHOT WITH NO KEY: attack, decay, release, idle — inside two seconds at the defaults.
	setEnv(rig, s, 0.0f);
	bool sawRelease = false;
	for (int i = 0; i < 2 * kTicksPerSecond; ++i) {
		rig.tick();
		sawRelease = sawRelease || rig.ordinal() == stageOrdinal(dahdsr::Stage::Release);
	}
	CHECK(sawRelease);
	CHECK(rig.ordinal() == mod::Playhead::kIdle);
}

TEST_CASE("DAHDSR Trig: under a held gate it re-attacks once per edge, and not again until below 0.25") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = false;
	rig.ctx.gate = true;
	setEnv(rig, s, 0.0f);
	rig.tick(kTicksPerSecond);
	REQUIRE(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));

	const auto settle = [&] {
		rig.tick(kTicksPerSecond);
		REQUIRE(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));
	};

	setEnv(rig, s, 1.0f); // fires
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
	settle();

	// HELD HIGH, then dipping to 0.3 — inside the gap — and back up: no re-attack.
	rig.tick(10);
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));
	setEnv(rig, s, 0.3f);
	rig.tick();
	setEnv(rig, s, 1.0f);
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));

	// BELOW 0.25, then up: re-attacks on that tick.
	setEnv(rig, s, 0.2f);
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));
	setEnv(rig, s, 0.6f);
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
}

TEST_CASE("DAHDSR Trig: a noisy Trig source at the fire level restarts the envelope once") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = false;
	rig.ctx.gate = true;
	setEnv(rig, s, 0.0f);
	rig.tick(kTicksPerSecond);
	REQUIRE(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));

	uint32_t lcg = 777u;
	int attacks = 0;
	uint32_t was = rig.ordinal();
	for (int i = 0; i < 2 * kTicksPerSecond; ++i) {
		lcg = lcg * 1664525u + 1013904223u;
		const float noise = (static_cast<float>(lcg >> 8) / 16777216.0f) * 0.4f - 0.2f;
		setEnv(rig, s, 0.5f + noise);
		rig.tick();
		const uint32_t now = rig.ordinal();
		attacks += (now == stageOrdinal(dahdsr::Stage::Attack) && was != now) ? 1 : 0;
		was = now;
	}
	CHECK(attacks == 1);
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));
}

TEST_CASE("DAHDSR Trig: under Gate with no key held a fire does nothing — the gate is what holds the shape open") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = false;
	setEnv(rig, s, 1.0f);
	rig.tick(5);
	CHECK(rig.ordinal() == mod::Playhead::kIdle);
	CHECK(rig.shared() == 0.0f);
}

TEST_CASE("DAHDSR Trig under Poly: a fire re-attacks every sounding note and leaves a finished one at rest") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = true;
	setEnv(rig, s, 0.0f);
	rig.ctx.gate = true;
	rig.feed(device::NoteEvent::Kind::On, 60);
	rig.feed(device::NoteEvent::Kind::On, 62);
	rig.feed(device::NoteEvent::Kind::On, 64);
	rig.feed(device::NoteEvent::Kind::Off, 64);
	rig.tick(2 * kTicksPerSecond);
	const float sustain = s.sustain;
	REQUIRE(rig.note(60) == doctest::Approx(sustain));
	REQUIRE(rig.note(62) == doctest::Approx(sustain));
	REQUIRE(rig.note(64) == 0.0f);

	setEnv(rig, s, 1.0f);
	rig.tick(3);
	// RE-ATTACKED FROM WHERE THEY STOOD, toward the peak: above the sustain level within three ticks.
	CHECK(rig.note(60) > sustain + 0.05f);
	CHECK(rig.note(62) > sustain + 0.05f);
	// THE FINISHED NOTE STAYS FINISHED.
	CHECK(rig.note(64) == 0.0f);
}

TEST_CASE("DAHDSR Trig: a manual fire is the pad's press — held for many ticks it fires once, pressed again it fires again") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = false;
	rig.ctx.gate = true;
	setEnv(rig, s, 0.0f);
	rig.tick(kTicksPerSecond);
	REQUIRE(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));

	setEnv(rig, s, 1.0f); // pressed: the base held at 1 for 80 ms
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
	rig.tick(kTicksPerSecond);
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain)); // the held press did not fire again
	setEnv(rig, s, 0.0f); // released
	rig.tick();
	setEnv(rig, s, 1.0f); // pressed again
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
}

TEST_CASE("LFO Trig resets the phase to the start phase, on the crossing tick") {
	lfo::Settings s;
	s.mode = mod::Mode::Free;
	s.phase = 0.25f;
	// THE REFERENCE: a fresh wave's phase after its first tick, which is the start phase plus one tick's advance.
	float reference = -1.0f;
	{
		Rig fresh(lfo::kModOps, lfo::kNoteOps);
		fresh.apply(lfo::rowOf(s));
		fresh.tick();
		reference = fresh.phase();
	}
	REQUIRE(reference > s.phase);

	Rig rig(lfo::kModOps, lfo::kNoteOps);
	rig.apply(lfo::rowOf(s));
	rig.tick(kTicksPerSecond / 3 + 7);
	const float before = rig.phase();
	REQUIRE(std::abs(before - reference) > 0.05f); // not vacuous: the wave has moved away from its start

	s.trig = 0.49f;
	rig.apply(lfo::rowOf(s));
	rig.tick();
	CHECK(std::abs(rig.phase() - reference) > 0.05f); // not yet

	s.trig = 0.5f;
	rig.apply(lfo::rowOf(s));
	rig.tick();
	CHECK(rig.phase() == doctest::Approx(reference).epsilon(1e-6));
}

TEST_CASE("LFO Trig on a Free wave the song holds: the reset survives the lock and follows the song's step from there") {
	lfo::Settings s;
	s.mode = mod::Mode::Free;
	s.phase = 0.25f;
	s.timeBase = 13; // "1/4": a window of one beat
	s.timeScale = 0.0f;
	REQUIRE(std::string(lfo::kTimeBaseNames[s.timeBase]) == "1/4");

	Rig rig(lfo::kModOps, lfo::kNoteOps);
	rig.apply(lfo::rowOf(s));
	rig.ctx.clock.playing = true;
	rig.ctx.clock.secondsPerBeat = 0.5;
	const double stepBeats = (static_cast<double>(mod::kBlockSamples) / kRate) / 0.5;
	double position = 10.6;
	for (int i = 0; i < 40; ++i) {
		rig.ctx.clock.songPositionBeats = position;
		rig.tick();
		position += stepBeats;
	}
	// LOCKED: the phase is the song's, plus the Phase control (backlog NJ follow-up, 2026-10-17: synced Free adds it).
	const double locked = (position - stepBeats) + static_cast<double>(s.phase);
	REQUIRE(rig.phase() == doctest::Approx(locked - std::floor(locked)).epsilon(1e-5));

	s.trig = 1.0f;
	rig.apply(lfo::rowOf(s));
	rig.ctx.clock.songPositionBeats = position;
	rig.tick();
	position += stepBeats;
	CHECK(rig.phase() == doctest::Approx(s.phase).epsilon(1e-6));

	// THE NEXT TICK: one song step on from the reset, not back on the song's own phase.
	rig.ctx.clock.songPositionBeats = position;
	rig.tick();
	CHECK(rig.phase() == doctest::Approx(s.phase + stepBeats).epsilon(1e-5));
}

TEST_CASE("LFO Trig on a Free wave in Hz the song's SECONDS hold: the reset survives that lock too (MV × NJ)") {
	// The merge of MV and NJ: NJ's Hz road follows song_pos_seconds through the same lock, so a Trig there must
	// restart against it as the beat road does, or the next tick's lock undoes the reset.
	lfo::Settings s;
	s.mode = mod::Mode::Free;
	s.phase = 0.25f;
	s.timeBase = 0; // "Hz"
	s.timeScale = 1.0f; // log2 of the rate: 2 Hz
	REQUIRE(std::string(lfo::kTimeBaseNames[s.timeBase]) == "Hz");
	constexpr double kHz = 2.0;

	Rig rig(lfo::kModOps, lfo::kNoteOps);
	rig.apply(lfo::rowOf(s));
	rig.ctx.clock.playing = true;
	rig.ctx.clock.secondsPerBeat = 0.5;
	const double step = static_cast<double>(mod::kBlockSamples) / kRate;
	double seconds = 7.3;
	for (int i = 0; i < 40; ++i) {
		rig.ctx.clock.songPositionSeconds = seconds;
		rig.tick();
		seconds += step;
	}
	const double locked = kHz * (seconds - step) + static_cast<double>(s.phase);
	REQUIRE(rig.phase() == doctest::Approx(locked - std::floor(locked)).epsilon(1e-5));

	s.trig = 1.0f;
	rig.apply(lfo::rowOf(s));
	rig.ctx.clock.songPositionSeconds = seconds;
	rig.tick();
	seconds += step;
	CHECK(rig.phase() == doctest::Approx(s.phase).epsilon(1e-6));

	rig.ctx.clock.songPositionSeconds = seconds;
	rig.tick();
	CHECK(rig.phase() == doctest::Approx(s.phase + kHz * step).epsilon(1e-5));
}

TEST_CASE("MSEG Trig: a finished one-shot restarts from its start on the crossing tick") {
	Rig rig(mseg::kModOps, mseg::kNoteOps);
	mseg::Settings s;
	s.mode = mod::Mode::Once;
	s.poly = false;
	rig.apply(mseg::rowOf(s));
	rig.ctx.gate = true;
	rig.tick();
	rig.ctx.gate = false;
	rig.tick(30 * kTicksPerSecond);
	REQUIRE(rig.ordinal() == mod::Playhead::kIdle); // the shot has run out

	s.trig = 0.49f;
	rig.apply(mseg::rowOf(s));
	rig.tick();
	CHECK(rig.ordinal() == mod::Playhead::kIdle);

	s.trig = 0.5f;
	rig.apply(mseg::rowOf(s));
	rig.tick();
	CHECK(rig.ordinal() == 0u); // running, in its first segment
	CHECK(rig.phase() < 0.2f);
}

TEST_CASE("Trig through a route: the fire lands on the tick the routed source crosses, and the depth sets where") {
	// THE THRESHOLD LIVES IN THE ROUTE (by design): at depth 1 a macro fires the envelope at 0.5; at depth 0.5 the
	// same ramp must reach 1.0. Asserted as the TICK each fire lands on.
	for (const float depth : { 1.0f, 0.5f }) {
		CAPTURE(depth);
		const backplane::Axes axes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, 
			                         .outputStride = 4 };
		rack::RackEngine rack(axes, hostCatalogue());
		rack.prepare(kRate, 4096);
		const size_t slot = rack.addDevice(dahdsr::kTraits.tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		rack.setField(slot, static_cast<size_t>(dahdsr::Param::Mode), static_cast<float>(mod::Mode::Once));
		rack.setField(slot, static_cast<size_t>(dahdsr::Param::Poly), 0.0f);
		const size_t trig = static_cast<size_t>(dahdsr::Param::Trig);
		const backplane::Destination dest =
		    test::ordinalOf(rack, slot, dahdsr::kDescriptor.nthModulatableOfControl(trig));
		rack.setRouteAmount(test::macro(0), dest, depth);

		int firedAt = -1;
		for (int i = 0; i <= 20; ++i) {
			rack.setMacro(0, static_cast<float>(i) / 20.0f);
			rack.tickModulators(mod::kBlockSamples, backplane::unknownBlock());
			uint32_t ordinal = mod::Playhead::kIdle;
			float phase = 0.0f;
			const bool running = rack.playheadAt(slot, ordinal, phase) && ordinal != mod::Playhead::kIdle;
			if (running && firedAt < 0) {
				firedAt = i;
			}
		}
		CHECK(firedAt == (depth == 1.0f ? 10 : 20));
	}
}

TEST_CASE("Trig timing, MEASURED on the rack's own road: a crossing is acted on at the next 64-sample tick, 0..63 samples late") {
	// THE JITTER BACKLOG MV ASKED FOR, measured rather than read off `kBlockSamples`: one-frame host blocks through
	// `processStereo` (the road that cuts a host block into tick pieces), the macro stepped to 1 before frame `k`,
	// and the frame on which the envelope is first seen running. The lateness is `ceil(k / 64) * 64 - k`.
	const backplane::Axes axes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, 
		                         .outputStride = 4 };
	int worst = -1;
	int best = 1 << 20;
	for (const int k : { 0, 1, 31, 63, 64, 65, 100, 127 }) {
		CAPTURE(k);
		rack::RackEngine rack(axes, hostCatalogue());
		rack.prepare(kRate, 4096);
		const size_t slot = rack.addDevice(dahdsr::kTraits.tag);
		rack.setField(slot, static_cast<size_t>(dahdsr::Param::Mode), static_cast<float>(mod::Mode::Once));
		rack.setField(slot, static_cast<size_t>(dahdsr::Param::Poly), 0.0f);
		const size_t trig = static_cast<size_t>(dahdsr::Param::Trig);
		rack.setRouteAmount(test::macro(0), test::ordinalOf(rack, slot, dahdsr::kDescriptor.nthModulatableOfControl(trig)),
		                    1.0f);
		int seenAt = -1;
		for (int f = 0; f < 256 && seenAt < 0; ++f) {
			if (f == k) {
				rack.setMacro(0, 1.0f);
			}
			float l = 0.0f;
			float r = 0.0f;
			rack.processStereo(&l, &r, 1, backplane::unknownBlock());
			uint32_t ordinal = mod::Playhead::kIdle;
			float phase = 0.0f;
			if (rack.playheadAt(slot, ordinal, phase) && ordinal != mod::Playhead::kIdle) {
				seenAt = f;
			}
		}
		REQUIRE(seenAt >= k);
		const int late = seenAt - k;
		CHECK(late == ((k + mod::kBlockSamples - 1) / mod::kBlockSamples) * mod::kBlockSamples - k);
		worst = std::max(worst, late);
		best = std::min(best, late);
	}
	CHECK(best == 0);
	CHECK(worst == mod::kBlockSamples - 1);
	MESSAGE("trig lateness: 0.." << worst << " samples = " << 1000.0 * worst / 44100.0 << " ms at 44.1 kHz, "
	                             << 1000.0 * worst / 48000.0 << " ms at 48 kHz");
}

namespace {

/// A RACK WITH VOICES: a DAHDSR source in front of a Poly DAHDSR whose Trig the source reaches at depth 1, two notes
/// held on two voices, both envelopes at sustain. The host's half — which voice holds which note, and each voice's
/// reading of the per-voice source — is written by the case, as the synth writes it.
struct PolyRig {
	static constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4,
		                                     .outputStride = 4 };
	rack::RackEngine rack { kAxes, hostCatalogue() };
	size_t src = 0;
	size_t dst = 0;

	explicit PolyRig(bool perVoiceSource) {
		rack.setVoices(2);
		rack.prepare(kRate, 4096);
		src = rack.addDevice(dahdsr::kTraits.tag);
		dst = rack.addDevice(dahdsr::kTraits.tag);
		REQUIRE(src != rack::RackEngine::kNoSlot);
		REQUIRE(dst != rack::RackEngine::kNoSlot);
		const size_t trig = static_cast<size_t>(dahdsr::Param::Trig);
		const backplane::Destination dest = test::ordinalOf(rack, dst, dahdsr::kDescriptor.nthModulatableOfControl(trig));
		rack.setRouteAmount(perVoiceSource ? test::outputOf(rack, src, 0) : test::macro(0), dest, 1.0f);
		rack.setGate(true);
		device::NoteEvent on[2] {};
		for (size_t i = 0; i < 2; ++i) {
			on[i].kind = device::NoteEvent::Kind::On;
			on[i].noteId = -1;
			on[i].channel = 0;
			on[i].key = static_cast<int16_t>(60 + 2 * i);
			rack.setVoiceNote(i, -1, 0, on[i].key);
		}
		device::NoteEvent out[8];
		REQUIRE(rack.processNotesNow(on, 2, out, 8) == 2);
		tick(kTicksPerSecond);
		REQUIRE(note(60) == doctest::Approx(0.5f)); // the default sustain
		REQUIRE(note(62) == doctest::Approx(0.5f));
	}
	void tick(int times) {
		for (int i = 0; i < times; ++i) {
			rack.tickModulators(mod::kBlockSamples, backplane::unknownBlock());
		}
	}
	float note(int16_t key) const { return rack.outputForNoteAt(dst, 0, -1, 0, key); }
};

} // namespace

TEST_CASE("Trig under Poly, per voice: a per-voice source fires the note on the voice whose value crossed, and only it") {
	// BACKLOG MV, B (Trig must work under Poly). This case pinned the
	// RESTART-ALL collapse (A) the day it shipped; it now asserts the per-voice answer by value.
	PolyRig rig(true);
	rig.rack.writeOutputForVoice(test::idAt(rig.rack, rig.src), 0, 0, 1.0f); // voice 0 (key 60) crosses
	rig.rack.writeOutputForVoice(test::idAt(rig.rack, rig.src), 0, 1, 0.0f); // voice 1 (key 62) does not
	rig.tick(3);
	CHECK(rig.note(60) > 0.55f);                      // re-attacked toward the peak
	CHECK(rig.note(62) == doctest::Approx(0.5f));     // still at sustain

	// AND VOICE 1's OWN EDGE: it crosses later and fires its note then, while voice 0, held high, does not re-fire.
	rig.tick(kTicksPerSecond);
	REQUIRE(rig.note(60) == doctest::Approx(0.5f));
	rig.rack.writeOutputForVoice(test::idAt(rig.rack, rig.src), 0, 1, 1.0f);
	rig.tick(3);
	CHECK(rig.note(62) > 0.55f);
	CHECK(rig.note(60) == doctest::Approx(0.5f));
}

TEST_CASE("Trig under Poly with a source that is one value for every voice: a fire restarts every sounding note") {
	PolyRig rig(false);
	rig.rack.setMacro(0, 1.0f);
	rig.tick(3);
	CHECK(rig.note(60) > 0.55f);
	CHECK(rig.note(62) > 0.55f);
}

//------------------------------------------------------------------------------------------------//
// NOTES FIRE TRIG — the normal, and the switch that breaks it (backlog NU)                       //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NU: Notes Trig is a keyed switch on all three shapes, on by default, and a row carries it both ways") {
	const auto check = [](const device::ParamDesc& p, const char* key, bool settingsDefault) {
		CAPTURE(key);
		CHECK(std::string(p.key) == key);
		CHECK(p.def == 1.0);
		CHECK(p.stepped);
		CHECK(p.unit == device::Unit::Choice);
		CHECK_FALSE(p.modulatable);
		CHECK(settingsDefault);
	};
	check(dahdsr::kParamDescs[static_cast<size_t>(dahdsr::Param::NotesTrig)], "env_notes_trig",
	      dahdsr::Settings {}.notesTrig);
	check(lfo::kParamDescs[static_cast<size_t>(lfo::Param::NotesTrig)], "notes_trig", lfo::Settings {}.notesTrig);
	check(mseg::kParamDescs[static_cast<size_t>(mseg::Param::NotesTrig)], "notes_trig", mseg::Settings {}.notesTrig);

	dahdsr::Settings d;
	d.notesTrig = false;
	const auto dr = dahdsr::rowOf(d);
	CHECK(dr[static_cast<size_t>(dahdsr::Param::NotesTrig)] == 0.0f);
	CHECK_FALSE(dahdsr::settingsOf(dr, dr.size()).notesTrig);
	CHECK(dahdsr::settingsOf(dr, static_cast<size_t>(dahdsr::Param::NotesTrig)).notesTrig); // a short row: the default
	lfo::Settings l;
	l.notesTrig = false;
	const auto lr = lfo::rowOf(l);
	CHECK(lr[static_cast<size_t>(lfo::Param::NotesTrig)] == 0.0f);
	CHECK_FALSE(lfo::settingsOf(lr, lr.size()).notesTrig);
	mseg::Settings m;
	m.notesTrig = false;
	const auto mr = mseg::rowOf(m);
	CHECK(mr[static_cast<size_t>(mseg::Param::NotesTrig)] == 0.0f);
	CHECK_FALSE(mseg::settingsOf(mr, mr.size()).notesTrig);
}

TEST_CASE("NU DAHDSR Gate, normal broken: a held key starts nothing, a Trig starts it, and the key up still releases") {
	for (const bool poly : { false, true }) {
		CAPTURE(poly);
		Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
		dahdsr::Settings s;
		s.mode = mod::Mode::Gate;
		s.poly = poly;
		s.notesTrig = false;
		setEnv(rig, s, 0.0f);
		rig.ctx.gate = true;
		rig.ctx.retrigger = true;
		rig.feed(device::NoteEvent::Kind::On, 60);
		rig.tick();
		rig.ctx.retrigger = false;
		rig.tick(kTicksPerSecond);
		// THE KEY IS DOWN AND NOTHING RAN.
		CHECK(rig.ordinal() == mod::Playhead::kIdle);
		CHECK(rig.note(60) == 0.0f);

		setEnv(rig, s, 1.0f); // the Trig
		rig.tick();
		CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
		CHECK(rig.note(60) > 0.0f);
		rig.tick(kTicksPerSecond);
		REQUIRE(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));

		// A SECOND NOTE UNDER THE HELD GATE IS NO RETRIGGER.
		rig.ctx.retrigger = true;
		rig.feed(device::NoteEvent::Kind::On, 62);
		rig.tick();
		rig.ctx.retrigger = false;
		CHECK(rig.note(60) == doctest::Approx(s.sustain));
		if (!poly) {
			CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));
		}

		// THE KEY UP STILL RELEASES: the gate is still heard.
		rig.ctx.gate = false;
		rig.feed(device::NoteEvent::Kind::Off, 60);
		rig.feed(device::NoteEvent::Kind::Off, 62);
		rig.tick();
		CHECK(rig.note(60) < s.sustain);
	}
}

TEST_CASE("NU DAHDSR Once, normal broken: a note fires no shot; a Trig fires the whole shot, with or without a key") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Once;
	s.poly = false;
	s.notesTrig = false;
	setEnv(rig, s, 0.0f);
	rig.ctx.gate = true;
	rig.ctx.retrigger = true;
	rig.tick();
	rig.ctx.retrigger = false;
	rig.tick(20);
	CHECK(rig.ordinal() == mod::Playhead::kIdle);
	CHECK(rig.shared() == 0.0f);

	setEnv(rig, s, 1.0f);
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
	setEnv(rig, s, 0.0f);
	rig.ctx.gate = false; // a one-shot ignores the key going up
	bool sawRelease = false;
	for (int i = 0; i < 2 * kTicksPerSecond; ++i) {
		rig.tick();
		sawRelease = sawRelease || rig.ordinal() == stageOrdinal(dahdsr::Stage::Release);
	}
	CHECK(sawRelease);
	CHECK(rig.ordinal() == mod::Playhead::kIdle);
}

TEST_CASE("NU DAHDSR, normal made: a held key starts it on its first tick and a re-press retriggers, as before") {
	Rig rig(dahdsr::kModOps, dahdsr::kNoteOps);
	dahdsr::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = false;
	REQUIRE(s.notesTrig);
	setEnv(rig, s, 0.0f);
	rig.ctx.gate = true;
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
	rig.tick(kTicksPerSecond);
	REQUIRE(rig.ordinal() == stageOrdinal(dahdsr::Stage::Sustain));
	rig.ctx.retrigger = true;
	rig.tick();
	CHECK(rig.ordinal() == stageOrdinal(dahdsr::Stage::Attack));
}

TEST_CASE("NU LFO Gate, normal broken: a held key leaves the wave at rest, a Trig starts it, the key up rests it again") {
	Rig rig(lfo::kModOps, lfo::kNoteOps);
	lfo::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = false;
	s.notesTrig = false;
	s.timeScale = 0.0f; // 1 Hz
	rig.apply(lfo::rowOf(s));
	rig.ctx.gate = true;
	rig.ctx.retrigger = true;
	rig.tick();
	rig.ctx.retrigger = false;
	rig.tick(100);
	CHECK(rig.ordinal() == mod::Playhead::kIdle);
	CHECK(rig.shared() == 0.0f);

	s.trig = 1.0f;
	rig.apply(lfo::rowOf(s));
	rig.tick();
	CHECK(rig.ordinal() == 0u);
	const float started = rig.phase();
	CHECK(started < 0.01f);
	rig.tick(100);
	const float running = rig.phase();
	CHECK(running > started + 0.1f);

	// A RE-PRESS UNDER THE HELD KEY DOES NOT SEND IT BACK TO THE START.
	rig.ctx.retrigger = true;
	rig.tick();
	rig.ctx.retrigger = false;
	CHECK(rig.phase() > running);

	rig.ctx.gate = false;
	rig.tick();
	CHECK(rig.ordinal() == mod::Playhead::kIdle);
}

TEST_CASE("NU LFO, normal made: a held key restarts the phase, as before") {
	Rig rig(lfo::kModOps, lfo::kNoteOps);
	lfo::Settings s;
	s.mode = mod::Mode::Gate;
	s.poly = false;
	rig.apply(lfo::rowOf(s));
	rig.ctx.gate = true;
	rig.tick(100);
	REQUIRE(rig.phase() > 0.1f);
	rig.ctx.retrigger = true;
	rig.tick();
	CHECK(rig.phase() < 0.01f);
}

TEST_CASE("NU LFO Retrig, normal broken: notes no longer restart it, it runs free, and it is mono whatever Poly says") {
	Rig rig(lfo::kModOps, lfo::kNoteOps);
	lfo::Settings s;
	s.mode = mod::Mode::LfoRetrig;
	s.poly = true;
	s.notesTrig = false;
	rig.apply(lfo::rowOf(s));
	rig.tick(100);
	const float before = rig.phase();
	REQUIRE(before > 0.1f);
	rig.ctx.gate = true;
	rig.ctx.retrigger = true;
	rig.feed(device::NoteEvent::Kind::On, 60);
	rig.tick();
	rig.ctx.retrigger = false;
	CHECK(rig.phase() > before); // ran on: no restart
	CHECK(rig.note(60) == rig.shared()); // one wave, no shape of the note's own

	s.trig = 1.0f;
	rig.apply(lfo::rowOf(s));
	rig.tick();
	CHECK(rig.phase() < 0.01f); // the Trig still restarts it
}

TEST_CASE("NU MSEG Gate, normal broken: a held key does not start the shape, a Trig does, and a re-press does not") {
	for (const bool poly : { false, true }) {
		CAPTURE(poly);
		Rig rig(mseg::kModOps, mseg::kNoteOps);
		mseg::Settings s;
		s.mode = mod::Mode::Gate;
		s.poly = poly;
		s.notesTrig = false;
		rig.apply(mseg::rowOf(s));
		rig.ctx.gate = true;
		rig.ctx.retrigger = true;
		rig.feed(device::NoteEvent::Kind::On, 60);
		rig.tick();
		rig.ctx.retrigger = false;
		rig.tick(10);
		CHECK(rig.ordinal() == mod::Playhead::kIdle);

		s.trig = 1.0f;
		rig.apply(mseg::rowOf(s));
		rig.tick();
		CHECK(rig.ordinal() == 0u);
		rig.tick(30);
		const float running = rig.phase();
		REQUIRE(running > 0.05f);
		rig.ctx.retrigger = true;
		rig.feed(device::NoteEvent::Kind::On, 60); // the same key again, under its own held gate
		rig.tick();
		rig.ctx.retrigger = false;
		CHECK(rig.ordinal() == 0u);
		CHECK(rig.phase() > running);
	}
}

TEST_CASE("NU: a route into Trig never moves the normal, in either direction") {
	const backplane::Axes axes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, 
		                         .outputStride = 4 };
	rack::RackEngine rack(axes, hostCatalogue());
	rack.prepare(kRate, 4096);
	const size_t slot = rack.addDevice(dahdsr::kTraits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	const size_t normal = static_cast<size_t>(dahdsr::Param::NotesTrig);
	const size_t trig = static_cast<size_t>(dahdsr::Param::Trig);
	const backplane::Destination dest = test::ordinalOf(rack, slot, dahdsr::kDescriptor.nthModulatableOfControl(trig));
	for (const float stored : { 1.0f, 0.0f }) {
		CAPTURE(stored);
		rack.setField(slot, normal, stored);
		rack.setRouteAmount(test::macro(0), dest, 1.0f);
		rack.tickModulators(mod::kBlockSamples, backplane::unknownBlock());
		CHECK(rack.fieldAt(slot, normal) == stored);
		rack.setRouteAmount(test::macro(0), dest, 0.0f);
		rack.tickModulators(mod::kBlockSamples, backplane::unknownBlock());
		CHECK(rack.fieldAt(slot, normal) == stored);
	}
}
