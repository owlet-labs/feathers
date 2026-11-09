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

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <vector>

namespace device = feathers::device;
namespace mod = feathers::mod;

namespace {

constexpr double kRate = 48000.0;
constexpr int kTicksPerSecond = static_cast<int>(kRate) / mod::kBlockSamples; // 750

/// ONE MODULATOR DEVICE, DRIVEN THROUGH ITS OPS TABLES the way the rack drives it: the mono shape by the tick
/// context's gate and retrigger, the per-note shapes by the note lane's events.
struct Rig {
	const device::ModOps& mod;
	const device::NoteOps& notes;
	void* mem = nullptr;

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
	void tick(int times, bool gate, bool retrigger = false) {
		device::ModTickContext ctx;
		ctx.gate = gate;
		for (int i = 0; i < times; ++i) {
			ctx.retrigger = retrigger && i == 0;
			mod.tick(mem, ctx);
		}
	}
	float shared() const { return mod.outputAt(mem, 0); }
	float note(int16_t key) const { return mod.outputForNote(mem, 0, -1, 0, key); }
};

/// WHAT ONE RETRIGGER DOES TO THE OUTPUT — the reading just before, the new note's reading at its start (what
/// `startOutputForVoice` hands its voice's cell), and after one tick.
struct Step {
	float before = 0.0f;
	float atStart = 0.0f;
	float afterTick = 0.0f;
};

/// MONO: one shape, held, retriggered by the tick context 0.3 s in.
Step monoRetrigger(Rig& rig) {
	rig.tick(kTicksPerSecond * 3 / 10, true);
	Step s;
	s.before = rig.shared();
	s.atStart = s.before;
	rig.tick(1, true, true);
	s.afterTick = rig.shared();
	return s;
}

/// POLY: note A held 0.3 s, then note B struck — the voice B lands on reads B's shape.
Step polyNewNote(Rig& rig) {
	rig.feed(device::NoteEvent::Kind::On, 60);
	rig.tick(kTicksPerSecond * 3 / 10, true);
	Step s;
	s.before = rig.note(60);
	rig.feed(device::NoteEvent::Kind::On, 62);
	s.atStart = rig.note(62);
	rig.tick(1, true);
	s.afterTick = rig.note(62);
	return s;
}

/// LEGATO (the DAHDSR's "Leg"): note A held; note B struck under it and released, so B is the NEWEST note but not a
/// held one; then note C struck while A is still held — C takes up A, the newest HELD note, whole. The carry would
/// read B (the newest of all), so the two are told apart: B is falling in its release while A sits at sustain.
Step legatoNewNote(Rig& rig) {
	rig.feed(device::NoteEvent::Kind::On, 60);
	rig.tick(kTicksPerSecond * 3 / 10, true);
	rig.feed(device::NoteEvent::Kind::On, 64);
	rig.tick(kTicksPerSecond / 10, true);
	rig.feed(device::NoteEvent::Kind::Off, 64);
	rig.tick(kTicksPerSecond / 10, true);
	Step s;
	s.before = rig.note(60);
	REQUIRE(std::abs(rig.note(64) - s.before) > 0.05f); // B has moved away from A, or the case cannot tell them apart
	rig.feed(device::NoteEvent::Kind::On, 62);
	s.atStart = rig.note(62);
	rig.tick(1, true);
	s.afterTick = rig.note(62);
	return s;
}

/// 100 ms — log2(0.1), the stored form of the Smoothing control.
const float kSmooth = static_cast<float>(std::log2(0.1));

/// THE ONE-POLE'S STEP PER TICK AT `kSmooth`, restated from its definition rather than from `mod::smoothingCoeff`:
/// a 64-sample tick at 48 kHz against a 100 ms time constant.
double coeff() {
	const double tau = std::exp2(static_cast<double>(kSmooth));
	return 1.0 - std::exp(-(static_cast<double>(mod::kBlockSamples) / kRate) / tau);
}

/// EVERY KNOB A CASE TURNS, the same for all three devices.
struct Knobs {
	bool poly = true;
	float smoothing = kSmooth;
	bool carry = false;
	bool legato = false; // the DAHDSR's Single Trigger; the other two have none
};

/// ONE DEVICE: its tables and how it spells `Knobs` as a row. A one-second rise from 0 in Gate mode for each, so
/// 0.3 s in it stands well away from where a fresh shape starts (LFO 0.6, MSEG 0.3, DAHDSR 0.6 unsmoothed).
struct Device {
	const char* name;
	const device::ModOps& mod;
	const device::NoteOps& notes;
	std::function<std::vector<float>(const Knobs&)> row;
};

template <typename Row>
std::vector<float> asVector(const Row& r) {
	return std::vector<float>(r.begin(), r.end());
}

const std::vector<Device>& devices() {
	static const std::vector<Device> k {
		{ "LFO", mod::lfo::kModOps, mod::lfo::kNoteOps,
		  [](const Knobs& k) {
			  mod::lfo::Settings s;
			  s.wave = mod::lfo::Waveform::Triangle;
			  s.mode = mod::Mode::Gate;
			  s.timeBase = 0;
			  s.timeScale = 0.0f; // one second
			  s.smoothing = k.smoothing;
			  s.poly = k.poly;
			  s.smoothCarry = k.carry;
			  return asVector(mod::lfo::rowOf(s));
		  } },
		{ "MSEG", mod::mseg::kModOps, mod::mseg::kNoteOps,
		  [](const Knobs& k) {
			  mod::mseg::Settings s;
			  s.startLevel = 0.0f;
			  s.count = 1;
			  s.sustainSegment = mod::mseg::kNoSustain;
			  s.mode = mod::Mode::Gate;
			  s.segment[0].share = mod::timeValueFor(1.0);
			  s.segment[0].level = 1.0f;
			  s.segment[0].shape = 0.0f;
			  s.timeScale = mod::timeValueFor(1.0);
			  s.smoothing = k.smoothing;
			  s.poly = k.poly;
			  s.smoothCarry = k.carry;
			  mod::mseg::sanitize(&s);
			  return asVector(mod::mseg::rowOf(s));
		  } },
		{ "DAHDSR", mod::dahdsr::kModOps, mod::dahdsr::kNoteOps,
		  [](const Knobs& k) {
			  mod::dahdsr::Settings s;
			  s.attack = std::log2(0.5f);
			  s.sustain = 1.0f;
			  s.mode = mod::Mode::Gate;
			  s.smoothing = k.smoothing;
			  s.poly = k.poly;
			  s.smoothCarry = k.carry;
			  s.singleTrigger = k.legato;
			  return asVector(mod::dahdsr::rowOf(s));
		  } },
	};
	return k;
}

template <typename Scenario>
Step run(const Device& d, const Knobs& k, Scenario scenario) {
	Rig rig(d.mod, d.notes);
	rig.apply(d.row(k));
	return scenario(rig);
}

/// FLOAT READINGS OF A DOUBLE ONE-POLE: the device keeps its smoother in double and publishes it as float.
constexpr double kTol = 1.0e-6;

} // namespace

TEST_CASE("backlog MM: Smooth Carry off is today's behaviour, by value: a Poly note's smoothing starts from ZERO, a mono retrigger's runs through") {
	const double c = coeff();
	for (const Device& d : devices()) {
		CAPTURE(d.name);

		// POLY: the new note's voice reads 0 at its start whatever the last note stood at, and its first tick is one
		// step of the one-pole from 0 toward the unsmoothed shape's first value.
		const Step raw = run(d, Knobs { .poly = true, .smoothing = mod::kSmoothingLog2SecondsMin }, polyNewNote);
		const Step off = run(d, Knobs { .poly = true }, polyNewNote);
		REQUIRE(off.before > 0.2f); // the last note stood well away from zero, so a start at zero is a step
		CHECK(off.atStart == 0.0f);
		CHECK(std::abs(static_cast<double>(off.afterTick) - c * static_cast<double>(raw.afterTick)) < kTol);
		CHECK(off.before - off.afterTick > 0.2f); // THE STEP that was audible

		// MONO: the one shape's smoother never restarted — one step of the one-pole from where it stood toward the
		// retriggered shape's first value.
		const Step monoRaw = run(d, Knobs { .poly = false, .smoothing = mod::kSmoothingLog2SecondsMin }, monoRetrigger);
		const Step mono = run(d, Knobs { .poly = false }, monoRetrigger);
		const double expected = static_cast<double>(mono.before)
		                        + c * (static_cast<double>(monoRaw.afterTick) - static_cast<double>(mono.before));
		CHECK(std::abs(static_cast<double>(mono.afterTick) - expected) < kTol);
	}
}

TEST_CASE("backlog MM: Smooth Carry on, a Poly note's smoothing starts where the last note's output stood and glides from there, by value") {
	const double c = coeff();
	for (const Device& d : devices()) {
		CAPTURE(d.name);
		const Step raw = run(d, Knobs { .poly = true, .smoothing = mod::kSmoothingLog2SecondsMin }, polyNewNote);
		const Step on = run(d, Knobs { .poly = true, .carry = true }, polyNewNote);
		REQUIRE(on.before > 0.2f);

		// AT ITS START the new note's voice reads exactly what the last note read: no step at all.
		CHECK(on.atStart == on.before);
		// ONE TICK ON, one step of the one-pole from there toward the new shape's own first value.
		const double expected = static_cast<double>(on.before)
		                        + c * (static_cast<double>(raw.afterTick) - static_cast<double>(on.before));
		CHECK(std::abs(static_cast<double>(on.afterTick) - expected) < kTol);
		// AND IT IS A GLIDE, NOT A JUMP: the first tick moves it by a tick's share of the distance (c ≈ 0.013).
		CHECK(std::abs(on.afterTick - on.before) < 0.01f);

		// THE SHAPE UNDERNEATH IS UNTOUCHED: long after the smoothing has settled the carried note reads what an
		// uncarried one does.
		Rig a(d.mod, d.notes);
		Rig b(d.mod, d.notes);
		a.apply(d.row(Knobs { .poly = true, .carry = true }));
		b.apply(d.row(Knobs { .poly = true }));
		polyNewNote(a);
		polyNewNote(b);
		a.tick(kTicksPerSecond * 2, true);
		b.tick(kTicksPerSecond * 2, true);
		CHECK(a.note(62) == b.note(62));
	}
}

TEST_CASE("backlog MM: Smooth Carry changes nothing under mono, with smoothing off, or against a legato take-up") {
	for (const Device& d : devices()) {
		CAPTURE(d.name);
		// MONO: the one shape's smoother already runs through, so the switch has nothing to do.
		const Step monoOff = run(d, Knobs { .poly = false }, monoRetrigger);
		const Step monoOn = run(d, Knobs { .poly = false, .carry = true }, monoRetrigger);
		CHECK(monoOn.afterTick == monoOff.afterTick);

		// SMOOTHING OFF: nothing to glide, so a carried start would only hand the voice the last note's value until
		// the next tick — backlog FF's blip. The new note starts where it does without the switch.
		const Step bareOff = run(d, Knobs { .poly = true, .smoothing = mod::kSmoothingLog2SecondsMin }, polyNewNote);
		const Step bareOn = run(d, Knobs { .poly = true, .smoothing = mod::kSmoothingLog2SecondsMin, .carry = true },
		                        polyNewNote);
		CHECK(bareOn.atStart == bareOff.atStart);
		CHECK(bareOn.afterTick == bareOff.afterTick);
	}

	// LEGATO (the DAHDSR's only): a note struck under a held one takes that one up WHOLE, smoother and all, and the
	// carry must not overwrite it.
	const Device& env = devices()[2];
	const Step legOff = run(env, Knobs { .poly = true, .legato = true }, legatoNewNote);
	const Step legOn = run(env, Knobs { .poly = true, .carry = true, .legato = true }, legatoNewNote);
	REQUIRE(legOff.atStart == legOff.before); // the take-up itself
	CHECK(legOn.atStart == legOff.atStart);
	CHECK(legOn.afterTick == legOff.afterTick);
}

TEST_CASE("backlog MM: Smooth Carry is a stored switch, off by default, gated on Poly") {
	CHECK_FALSE(mod::lfo::Settings {}.smoothCarry);
	CHECK_FALSE(mod::mseg::Settings {}.smoothCarry);
	CHECK_FALSE(mod::dahdsr::Settings {}.smoothCarry);

	// ROUND TRIP: on survives the row, and a row too short to hold it reads the default.
	for (const Device& d : devices()) {
		CAPTURE(d.name);
		const std::vector<float> on = d.row(Knobs { .carry = true });
		const std::vector<float> off = d.row(Knobs { .carry = false });
		CHECK(on != off);
	}
	{
		mod::lfo::Settings s;
		s.smoothCarry = true;
		const mod::lfo::Row r = mod::lfo::rowOf(s);
		CHECK(mod::lfo::settingsOf(r, r.size()).smoothCarry);
		CHECK_FALSE(mod::lfo::settingsOf(r, static_cast<size_t>(mod::lfo::Param::SmoothCarry)).smoothCarry);
		const auto& p = mod::lfo::kParamDescs[static_cast<size_t>(mod::lfo::Param::SmoothCarry)];
		CHECK(p.gateControl == static_cast<int16_t>(mod::lfo::Param::Poly));
		CHECK(p.gateMask == (1u << 1));
	}
	{
		mod::mseg::Settings s;
		s.smoothCarry = true;
		const mod::mseg::Row r = mod::mseg::rowOf(s);
		CHECK(mod::mseg::settingsOf(r, r.size()).smoothCarry);
		const auto& p = mod::mseg::kParamDescs[static_cast<size_t>(mod::mseg::Param::SmoothCarry)];
		CHECK(p.gateControl == static_cast<int16_t>(mod::mseg::Param::Poly));
		CHECK(p.gateMask == (1u << 1));
	}
	{
		mod::dahdsr::Settings s;
		s.smoothCarry = true;
		const mod::dahdsr::Row r = mod::dahdsr::rowOf(s);
		CHECK(mod::dahdsr::settingsOf(r, r.size()).smoothCarry);
		CHECK_FALSE(mod::dahdsr::settingsOf(r, static_cast<size_t>(mod::dahdsr::Param::SmoothCarry)).smoothCarry);
		const auto& p = mod::dahdsr::kParamDescs[static_cast<size_t>(mod::dahdsr::Param::SmoothCarry)];
		CHECK(p.gateControl == static_cast<int16_t>(mod::dahdsr::Param::Poly));
		CHECK(p.gateMask == (1u << 1));
	}
}
