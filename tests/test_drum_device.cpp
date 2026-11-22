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

// THE DRUM DEVICE: one drum, a generator in a chain (src/fx/drum). Every claim about a sound is asserted against the engine's own hit
// (`drum::Hit`) driven DIRECTLY with laws from an independent call to `lawsFor`, restated here from the row; a rendered difference
// appears only as a second assertion that the first is not vacuous. The whole file is held to bit-for-bit, because the device adds
// nothing to the hit but its Level, Pan and High Pass, which are restated below.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "device/registry.h"
#include "dsp/drum_voice.h"
#include "dsp/field_override.h"
#include "dsp/ks_string.h"
#include "dsp/slice_build.h"
#include "fx/drum/drum.h"
#include "fx/drum/drum_types.h"
#include "persist_rig.h"
#include "plugin/device_registry.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <numbers>
#include <vector>

//--------------------------------------------------------------------------------------------------//
// THE ALLOCATION COUNTER — a replacement operator new for THIS TEST BINARY ONLY, counting while a   //
// case holds the window open.                                                                       //
//--------------------------------------------------------------------------------------------------//
namespace {
std::atomic<bool> g_countAllocs { false };
std::atomic<uint64_t> g_allocs { 0 };
struct AllocWindow {
	AllocWindow() {
		g_allocs.store(0);
		g_countAllocs.store(true);
	}
	~AllocWindow() { g_countAllocs.store(false); }
	[[nodiscard]] uint64_t allocs() const { return g_allocs.load(); }
};
void noteAlloc() noexcept {
	if (g_countAllocs.load(std::memory_order_relaxed)) {
		g_allocs.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
[[gnu::noinline]] void* operator new(std::size_t n) {
	noteAlloc();
	void* p = feathers::test::heapAlloc(n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n) { return operator new(n); }
[[gnu::noinline]] void* operator new(std::size_t n, std::align_val_t al) {
	noteAlloc();
	void* p = feathers::test::heapAlignedAlloc(static_cast<std::size_t>(al), n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n, std::align_val_t al) { return operator new(n, al); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#pragma GCC diagnostic pop

using namespace feathers;
namespace dd = feathers::fx::drumdevice;
using dd::Param;
using NoteEvent = device::NoteEvent;

namespace {

constexpr double kSr = 48000.0;

constexpr size_t at(Param p) {
	return static_cast<size_t>(p);
}

dd::Row defaultRow() {
	return dd::rowOf(dd::stateOf(dd::Row {}, 0));
}

NoteEvent on(int key, uint32_t t, double vel = 1.0, int id = -1) {
	NoteEvent e;
	e.kind = NoteEvent::Kind::On;
	e.timeSamples = t;
	e.key = static_cast<int16_t>(key);
	e.velocity = vel;
	e.noteId = id;
	return e;
}
NoteEvent off(int key, uint32_t t, int id = -1) {
	NoteEvent e = on(key, t, 0.0, id);
	e.kind = NoteEvent::Kind::Off;
	return e;
}
NoteEvent choke(int key, uint32_t t) {
	NoteEvent e = on(key, t, 0.0);
	e.kind = NoteEvent::Kind::Choke;
	return e;
}
NoteEvent tuning(int key, double semis) {
	NoteEvent e = on(key, 0, 0.0);
	e.kind = NoteEvent::Kind::Expression;
	e.expressionId = dd::kTuningExpression;
	e.expressionValue = semis;
	return e;
}

/// THE DEVICE, DRIVEN THROUGH ITS OWN OPS TABLES as a rack drives it.
struct Dev {
	void* st = nullptr;
	dd::Row row = defaultRow();

	explicit Dev(double rate = kSr) {
		const device::AudioOps& a = dd::kAudioOps;
		st = test::heapAlignedAlloc(a.stateAlign, a.stateBytes);
		a.construct(st);
		a.prepare(st, rate, 512);
		apply();
	}
	~Dev() {
		dd::kAudioOps.destruct(st);
		test::heapFree(st);
	}
	Dev(const Dev&) = delete;
	Dev& operator=(const Dev&) = delete;

	void set(Param p, double v) { row[at(p)] = static_cast<float>(v); }
	void apply() {
		dd::kAudioOps.setFields(st, row.data(), row.size());
		dd::kNoteOps.setFields(st, row.data(), row.size());
	}
	dd::DrumDsp& dsp() { return *static_cast<dd::DrumDsp*>(st); }
	std::vector<NoteEvent> notes(const std::vector<NoteEvent>& in) {
		std::vector<NoteEvent> out(in.size() + 8);
		size_t n = 0;
		dd::kNoteOps.processNotes(st, in.data(), in.size(), out.data(), out.size(), &n);
		out.resize(n);
		return out;
	}
	/// One call of `frames` over a buffer of zeros (or `inL`/`inR` when given), `phase` the host's stamp or -1.
	void render(float* l, float* r, int frames, int phase = -1) {
		apply();
		if (phase >= 0) {
			device::NoteContext c;
			c.tickPhase = phase;
			dd::kAudioOps.setNoteContext(st, c);
		}
		dd::kAudioOps.processStereo(st, l, r, frames);
	}
	bool holds() const { return dd::kAudioOps.holdsTail(st); }
};

struct Out {
	std::vector<float> l, r;
	explicit Out(int n) : l(static_cast<size_t>(n), 0.0f), r(static_cast<size_t>(n), 0.0f) {}
};

/// A drum run on `notes` for `frames` in one call.
Out run(Dev& d, const std::vector<NoteEvent>& notes, int frames, int phase = -1) {
	Out o(frames);
	(void)d.notes(notes);
	d.render(o.l.data(), o.r.data(), frames, phase);
	return o;
}

/// THE ENGINE'S OWN HIT, standing alone, with its own string and pluck builder.
struct RefHit {
	ks::KsString string;
	ks::PluckBuilder builder;
	drum::Hit hit;
	explicit RefHit(double rate = kSr) {
		builder.prepare(rate);
		string.prepare(rate);
		hit.prepare(rate);
		hit.setString(&string, &builder);
	}
};

/// THE TERRAIN'S DECODE, RESTATED from the row: home is zone/1023 + offset + gamma, the frame is the Additive plane at that home and
/// rotate, the seven fields are read at the travel, then each goes through its scaler.
drum::Fields refFields(const dd::Row& r) {
	const double home = static_cast<double>(std::lround(r[at(Param::Zone)])) / 1023.0 + static_cast<double>(r[at(Param::ZoneOffset)])
	                    + static_cast<double>(r[at(Param::Gamma)]);
	const PartialBasis frame = sliceFrameFor<kPartialTriangles>(static_cast<double>(r[at(Param::Rotate)]), home,
	                                                            SliceResidue<kPartialTriangles> {});
	const drum::Fields raw = readDrumFields(frame, home, static_cast<double>(r[at(Param::TravelX)]),
	                                        static_cast<double>(r[at(Param::TravelY)]));
	return {
		fieldWithOverride(raw.pitch, r[at(Param::FieldPitch)]),   fieldWithOverride(raw.length, r[at(Param::FieldLength)]),
		fieldWithOverride(raw.sweep, r[at(Param::FieldSweep)]),   fieldWithOverride(raw.tone, r[at(Param::FieldTone)]),
		fieldWithOverride(raw.noise, r[at(Param::FieldNoise)]),   fieldWithOverride(raw.snap, r[at(Param::FieldSnap)]),
		fieldWithOverride(raw.metal, r[at(Param::FieldMetal)]),
	};
}

/// THE PAD'S CONTROLS RESTATED from the row, one line per control, as the synth's `fillControls` writes them.
drum::Controls refControls(const dd::Row& r) {
	const auto v = [&](Param p) { return static_cast<double>(r[at(p)]); };
	drum::Controls c;
	c.decaySeconds = std::exp2(v(Param::Decay));
	c.sweepSeconds = std::exp2(v(Param::SweepTime));
	c.drive = v(Param::Drive);
	c.count = static_cast<int>(std::lround(v(Param::Claps)));
	c.spreadSeconds = std::exp2(v(Param::Spread));
	c.pitchRangeSemitones = 3.0;
	c.direct = true;
	c.velTone = v(Param::VelTone);
	c.velSnap = v(Param::VelSnap);
	c.velTension = v(Param::VelTension);
	c.wireContour = static_cast<int>(std::lround(v(Param::WireContour)));
	c.wireShape = v(Param::WireGate);
	c.bodyShape = v(Param::BodyGate);
	c.wireBuzz = v(Param::WireBuzz);
	c.buzzThreshold = v(Param::WireThreshold);
	c.bell1Level = v(Param::Bell1Level);
	c.bell2Level = v(Param::Bell2Level);
	c.bellQScale = v(Param::BellQ);
	c.bellLinked = r[at(Param::BellLink)] >= 0.5f;
	c.wireDecaySeconds = std::exp2(v(Param::WireDecay));
	c.clickDecaySeconds = std::exp2(v(Param::ClickDecay));
	c.bellStartHz = std::exp2(v(Param::BellStart));
	c.bellEndHz = std::exp2(v(Param::BellEnd));
	c.bellSeconds = std::exp2(v(Param::BellTime));
	c.kickHoldSeconds = v(Param::KickHold) / 1000.0;
	c.kickDip = v(Param::KickDip);
	c.kickDipSeconds = v(Param::KickDipTime) / 1000.0;
	c.clickSweepOctaves = v(Param::ClickSweep);
	c.clickSweepSeconds = std::exp2(v(Param::ClickSweepTime));
	c.startPhaseDegrees = v(Param::StartPhase);
	c.driveCurve = static_cast<int>(std::lround(v(Param::DriveCurve)));
	c.driveTone = v(Param::DriveTone);
	c.clapQ = v(Param::ClapQ);
	c.clapHighPassHz = std::exp2(v(Param::ClapHighPass));
	c.bell2Hz = std::exp2(v(Param::Bell2Hz));
	c.bell2Seconds = std::exp2(v(Param::Bell2Time));
	c.velocityNoise = v(Param::VelocityNoise);
	c.particleDensity = std::exp2(v(Param::Density));
	c.grainSeconds = std::exp2(v(Param::Grain));
	c.pressure = v(Param::Pressure);
	c.speed = v(Param::Speed);
	c.play = static_cast<int>(std::lround(v(Param::Trigger)));
	c.width = v(Param::Width);
	c.pitchMode = static_cast<int>(std::lround(v(Param::PitchMode)));
	return c;
}

drum::Kind kindOf(const dd::Row& r) {
	return static_cast<drum::Kind>(std::lround(r[at(Param::Kind)]));
}

double refBaseHz(const dd::Row& r, double moved = 0.0) {
	return drum::kindBaseHz(kindOf(r))
	       * std::exp2((static_cast<double>(r[at(Param::Tune)]) + static_cast<double>(r[at(Param::Bend)]) * moved) / 12.0);
}

drum::Laws refLaws(const dd::Row& r, double velocity, double moved = 0.0) {
	return drum::lawsFor(kindOf(r), refFields(r), refControls(r), refBaseHz(r, moved), velocity);
}

/// The seed the device derives for a strike with no transport: the Seed control, the key, the kind and the key's strike count.
uint64_t refSeed(const dd::Row& r, int key, uint32_t count) {
	return dd::strikeSeed(static_cast<uint64_t>(std::lround(r[at(Param::Seed)])), key, kindOf(r), -1, count);
}

/// A HIT STRUCK AND RENDERED `frames` samples, left and right, with the ear that the kind does not write copied (the device's own rule).
Out refRender(const dd::Row& r, double velocity, int frames, uint32_t count = 0, int key = 36, double rate = kSr) {
	RefHit h(rate);
	// THE STRIKE LEVEL, restated: 1 - depth x (1 - velocity).
	const double level = 1.0 - static_cast<double>(r[at(Param::VelLevel)]) * (1.0 - velocity);
	h.hit.strike(kindOf(r), refLaws(r, velocity), refSeed(r, key, count), static_cast<float>(level));
	Out o(frames);
	for (int s = 0; s < frames; s += 64) {
		const int n = std::min(64, frames - s);
		h.hit.render(o.l.data() + s, n, o.r.data() + s);
	}
	const drum::Kind k = kindOf(r);
	if (!(k == drum::Kind::Membrane || k == drum::Kind::Snare || k == drum::Kind::Clap)) {
		o.r = o.l;
	}
	return o;
}

int firstNonzero(const std::vector<float>& v) {
	for (size_t i = 0; i < v.size(); ++i) {
		if (v[i] != 0.0f) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

bool allZero(const std::vector<float>& v, size_t from = 0, size_t to = SIZE_MAX) {
	for (size_t i = from; i < std::min(v.size(), to); ++i) {
		if (v[i] != 0.0f) {
			return false;
		}
	}
	return true;
}

/// A ROW EVERY CONTROL OF WHICH IS OFF ITS DEFAULT, inside its range, so a decode that dropped one cannot hide behind a default.
dd::Row variedRow(drum::Kind kind) {
	dd::Row r = defaultRow();
	const auto put = [&](Param p, double v) { r[at(p)] = static_cast<float>(v); };
	put(Param::Kind, static_cast<double>(kind));
	put(Param::Level, 0.0); // unity: the gain is its own case
	put(Param::Tune, 2.0);
	put(Param::Decay, -1.6);
	put(Param::FieldTone, 0.62);
	put(Param::FieldNoise, 0.41);
	put(Param::Drive, 0.35);
	put(Param::Seed, 17);
	put(Param::Zone, 300);
	put(Param::ZoneOffset, 0.25);
	put(Param::Gamma, 3.5);
	put(Param::Rotate, 7.5);
	put(Param::TravelX, 50.0);
	put(Param::TravelY, -1234.5);
	put(Param::FieldPitch, 0.58);
	put(Param::FieldLength, 0.47);
	put(Param::FieldSweep, 0.7);
	put(Param::FieldSnap, 0.36);
	put(Param::FieldMetal, 0.66);
	put(Param::SweepTime, -4.1);
	put(Param::DriveCurve, 1);
	put(Param::DriveTone, 0.4);
	put(Param::VelLevel, 0.8);
	put(Param::VelTone, 0.6);
	put(Param::VelSnap, -0.3);
	put(Param::VelTension, 0.7);
	put(Param::VelocityNoise, 0.3);
	put(Param::Width, 0.6);
	put(Param::Claps, 4);
	put(Param::Spread, -4.6);
	put(Param::ClapQ, 2.2);
	put(Param::ClapHighPass, 7.0);
	put(Param::WireDecay, -2.0);
	put(Param::ClickDecay, -7.0);
	put(Param::WireContour, 1);
	put(Param::WireBuzz, 0.4);
	put(Param::WireThreshold, 0.3);
	put(Param::WireGate, 0.25);
	put(Param::BodyGate, 0.2);
	put(Param::Bell1Level, 0.6);
	put(Param::Bell2Level, 0.5);
	put(Param::BellQ, 1.6);
	put(Param::BellLink, 0);
	put(Param::BellStart, 11.0);
	put(Param::BellEnd, 9.7);
	put(Param::BellTime, -4.3);
	put(Param::Bell2Hz, 9.3);
	put(Param::Bell2Time, -5.3);
	put(Param::KickHold, 12.0);
	put(Param::KickDip, 0.3);
	put(Param::KickDipTime, 20.0);
	put(Param::ClickSweep, 1.5);
	put(Param::ClickSweepTime, -8.0);
	put(Param::StartPhase, 30.0);
	return r;
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE DECLARATION                                                                                   //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("drum: each control is gated on the kinds the engine says read it") {
	// Restated from `kindReads` / `kindControls`, the engine's own statement of what a kind reads, against the descriptor's gate.
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		const auto kind = static_cast<drum::Kind>(k);
		CAPTURE(k);
		const drum::FieldUse f = drum::kindReads(kind);
		const drum::ControlUse c = drum::kindControls(kind);
		const auto shows = [&](Param p) {
			const device::ParamDesc& d = dd::kParamDescs[at(p)];
			return device::relevantFor(d, d.gateControl == dd::kKindControl ? static_cast<double>(k) : 0.0);
		};
		const struct {
			Param p;
			bool reads;
		} expected[] = {
			{ Param::FieldPitch, f.pitch },   { Param::FieldLength, f.length }, { Param::FieldSweep, f.sweep },
			{ Param::FieldTone, f.tone },     { Param::FieldNoise, f.noise },   { Param::FieldSnap, f.snap },
			{ Param::FieldMetal, f.metal },   { Param::SweepTime, c.sweepTime }, { Param::Drive, c.drive },
			{ Param::DriveCurve, c.drive },   { Param::DriveTone, c.drive },    { Param::Claps, c.count },
			{ Param::Spread, c.spread },      { Param::ClapQ, c.clapFilter },   { Param::ClapHighPass, c.clapFilter },
			{ Param::WireDecay, c.snareEnvelopes }, { Param::ClickDecay, c.snareEnvelopes }, { Param::BellStart, c.bell },
			{ Param::BellEnd, c.bell },       { Param::BellTime, c.bell },      { Param::Bell2Hz, c.bell },
			{ Param::Bell2Time, c.bell },     { Param::Bell1Level, c.bell },    { Param::Bell2Level, c.bell },
			{ Param::BellQ, c.bell },         { Param::BellLink, c.bell },      { Param::WireContour, c.wireContour },
			{ Param::WireBuzz, c.wireContour }, { Param::WireThreshold, c.wireContour }, { Param::WireGate, c.wireContour },
			{ Param::BodyGate, c.wireContour }, { Param::VelLevel, true },      { Param::VelTone, true },
			{ Param::VelSnap, true },         { Param::VelTension, kind == drum::Kind::Kick || kind == drum::Kind::Membrane || kind == drum::Kind::Snare },
			{ Param::KickHold, c.kickShape }, { Param::KickDip, c.kickShape },  { Param::KickDipTime, c.kickShape },
			{ Param::ClickSweep, c.kickShape }, { Param::ClickSweepTime, c.kickShape }, { Param::StartPhase, c.kickShape },
			{ Param::VelocityNoise, c.velNoise }, { Param::Width, c.width },
		};
		for (const auto& e : expected) {
			CAPTURE(dd::kParamDescs[at(e.p)].key);
			CHECK(shows(e.p) == e.reads);
		}
	}
	// AND RELEASE IS SHOWN ONLY UNDER GATE.
	const device::ParamDesc& rel = dd::kParamDescs[at(Param::Release)];
	CHECK_FALSE(device::relevantFor(rel, 0.0));
	CHECK(device::relevantFor(rel, 1.0));
}

TEST_CASE("drum: the descriptor is a generator with notes and audio on one storage, and every key is unique and well formed") {
	CHECK(dd::kDescriptor.audio != nullptr);
	CHECK(dd::kDescriptor.notes != nullptr);
	CHECK(dd::kAudioOps.stateBytes == dd::kNoteOps.stateBytes);
	CHECK(dd::kAudioOps.stateAlign == dd::kNoteOps.stateAlign);
	std::vector<std::string> keys;
	for (const device::ParamDesc& d : dd::kParamDescs) {
		const std::string k = d.key;
		CHECK_FALSE(k.empty());
		for (const char ch : k) {
			CHECK(((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '.'));
		}
		CHECK(std::find(keys.begin(), keys.end(), k) == keys.end());
		keys.push_back(k);
	}
	// THE UNBOUNDED COORDINATES MAY LEAVE THEIR RANGE (a route carries them past any value).
	CHECK(dd::kParamDescs[at(Param::TravelX)].offsetsMayLeaveRange);
	CHECK(dd::kParamDescs[at(Param::TravelY)].offsetsMayLeaveRange);
}

//--------------------------------------------------------------------------------------------------//
// THE NOTE ROAD                                                                                     //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("drum: a note-on for its key at sample k sounds from exactly k, silence before, and another key sounds nothing") {
	constexpr int kFrames = 640;
	for (const int k : { 0, 1, 37, 63, 64, 301 }) {
		CAPTURE(k);
		Dev d;
		const Out got = run(d, { on(36, static_cast<uint32_t>(k)) }, kFrames);
		const Out ref = refRender(d.row, 1.0, kFrames - k);
		const int first = firstNonzero(ref.l);
		REQUIRE(first >= 0);
		CHECK(allZero(got.l, 0, static_cast<size_t>(k)));
		CHECK(firstNonzero(got.l) == k + first);
		// THE VALUE: the whole tail is the engine's hit, bit for bit, shifted to the note.
		bool same = true;
		for (int i = 0; i < kFrames - k; ++i) {
			same = same && got.l[static_cast<size_t>(k + i)] == ref.l[static_cast<size_t>(i)]
			       && got.r[static_cast<size_t>(k + i)] == ref.r[static_cast<size_t>(i)];
		}
		CHECK(same);
	}
	// ANOTHER KEY: nothing, and nothing held.
	Dev d;
	const Out other = run(d, { on(37, 5), on(35, 9) }, kFrames);
	CHECK(allZero(other.l));
	CHECK(allZero(other.r));
	CHECK_FALSE(d.holds());
	CHECK(d.dsp().strikesForTest() == 0);
}

TEST_CASE("drum: a host that stamps its tick phase gets the note on its sample; a note before the call's start is dropped") {
	constexpr int kFrames = 256;
	Dev d;
	// timeSamples is an offset in the TICK: phase 20 and a note at 20 + 37 is sample 37 of this call.
	const Out got = run(d, { on(36, 20 + 37) }, kFrames, 20);
	const Out ref = refRender(d.row, 1.0, kFrames - 37);
	CHECK(allZero(got.l, 0, 37));
	CHECK(firstNonzero(got.l) == 37 + firstNonzero(ref.l));
	CHECK(got.l[37 + static_cast<size_t>(firstNonzero(ref.l))] == ref.l[static_cast<size_t>(firstNonzero(ref.l))]);
	// A STALE NOTE: queued for a call that never ran (phase 30, the note at 10) is not struck late.
	Dev s;
	const Out stale = run(s, { on(36, 10) }, 128, 30);
	CHECK(allZero(stale.l));
	CHECK(s.dsp().staleDroppedForTest() == 1);
	CHECK(s.dsp().strikesForTest() == 0);
}

TEST_CASE("drum: a note past the call's end plays at its place in the next call (a host that says nothing of its tick)") {
	Dev d;
	Out first = run(d, { on(36, 300) }, 256);
	CHECK(allZero(first.l));
	CHECK(d.holds()); // a note still to place is sound to come
	Out second(256);
	d.render(second.l.data(), second.r.data(), 256);
	const Out ref = refRender(d.row, 1.0, 256 - 44);
	CHECK(allZero(second.l, 0, 44));
	CHECK(firstNonzero(second.l) == 44 + firstNonzero(ref.l));
}

TEST_CASE("drum: every note passes through unchanged, count and fields") {
	Dev d;
	std::vector<NoteEvent> in = { on(36, 3, 0.8, 11), on(40, 5, 0.5, 12), off(36, 9, 11), choke(36, 10), tuning(36, 1.5),
		                          on(36, 12, 1.0, 14), off(40, 70, 12) };
	in[4].noteId = 11;
	in[0].channel = 2;
	in[0].port = 1;
	const std::vector<NoteEvent> out = d.notes(in);
	REQUIRE(out.size() == in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		CAPTURE(i);
		CHECK(std::memcmp(&out[i], &in[i], sizeof(NoteEvent)) == 0);
	}
	CHECK(dd::kNoteOps.listensOnly == false);
}

TEST_CASE("drum: a hit equals the engine's own hit driven directly, every kind, to the bit") {
	// The laws are an independent `lawsFor` over the restated decode (refFields) and the restated controls (refControls); the seed is
	// the one the device derives. Nothing of the device's own decode is on this side of the comparison.
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		const auto kind = static_cast<drum::Kind>(k);
		CAPTURE(drum::kKindNames[k]);
		for (const double velocity : { 1.0, 0.55 }) {
			CAPTURE(velocity);
			Dev d;
			d.row = variedRow(kind);
			d.row[at(Param::Level)] = 0.0f; // unity: the gain is its own case
			const Out got = run(d, { on(36, 11, velocity) }, 4096);
			const Out ref = refRender(d.row, velocity, 4096 - 11);
			REQUIRE(firstNonzero(ref.l) >= 0);
			bool same = allZero(got.l, 0, 11) && allZero(got.r, 0, 11);
			size_t worst = 0;
			for (int i = 0; i < 4096 - 11; ++i) {
				const bool ok = got.l[static_cast<size_t>(11 + i)] == ref.l[static_cast<size_t>(i)]
				                && got.r[static_cast<size_t>(11 + i)] == ref.r[static_cast<size_t>(i)];
				if (!ok && same) {
					worst = static_cast<size_t>(i);
				}
				same = same && ok;
			}
			CHECK_MESSAGE(same, "first difference at sample ", worst);
		}
	}
	// NON-VACUITY: the terrain is read. A different Zone is a different sound (the first assertion would pass for a silent hit).
	Dev a;
	a.row = variedRow(drum::Kind::Membrane);
	Dev b;
	b.row = a.row;
	b.row[at(Param::Zone)] = 700.0f;
	b.row[at(Param::TravelX)] = 51.0f;
	CHECK(run(a, { on(36, 0) }, 1024).l != run(b, { on(36, 0) }, 1024).l);
	// AND THE DECODE IS REALLY THE TERRAIN'S: restated fields differ from the neutral decode, and the device holds the restated ones.
	CHECK(a.dsp().fieldsForTest() == refFields(a.row));
	CHECK_FALSE(refFields(a.row) == refFields(defaultRow()));
}

TEST_CASE("drum: Level, Pan and the pan law") {
	for (const double pan : { -1.0, -0.4, 0.0, 0.5, 1.0 }) {
		for (const double level : { 0.0, -9.0 }) {
			CAPTURE(pan);
			CAPTURE(level);
			Dev d;
			d.row = variedRow(drum::Kind::Snare);
			d.set(Param::Pan, pan);
			d.set(Param::Level, level);
			const Out got = run(d, { on(36, 0) }, 1024);
			const Out ref = refRender(d.row, 1.0, 1024);
			const float gain = static_cast<float>(std::pow(10.0, static_cast<double>(d.row[at(Param::Level)]) / 20.0));
			const double theta = (static_cast<double>(d.row[at(Param::Pan)]) + 1.0) * std::numbers::pi / 4.0;
			float gl = gain * static_cast<float>(std::numbers::sqrt2 * std::cos(theta));
			float gr = gain * static_cast<float>(std::numbers::sqrt2 * std::sin(theta));
			if (pan == 0.0) {
				gl = gr = gain;
			}
			bool same = true;
			for (size_t i = 0; i < 1024; ++i) {
				same = same && got.l[i] == gl * ref.l[i] && got.r[i] == gr * ref.r[i];
			}
			CHECK(same);
			CHECK(d.dsp().gainLeftForTest() == gl);
			CHECK(d.dsp().gainRightForTest() == gr);
			// EQUAL POWER: the two ears' gains square to twice the level's (the centre is unity in each ear).
			if (pan != 0.0) {
				CHECK(static_cast<double>(gl) * gl + static_cast<double>(gr) * gr
				      == doctest::Approx(2.0 * static_cast<double>(gain) * gain).epsilon(1e-6));
			}
		}
	}
}

TEST_CASE("drum: velocity reaches the laws and the level, as the engine's hit takes it") {
	Dev lo;
	lo.row = variedRow(drum::Kind::Membrane);
	Dev hi;
	hi.row = lo.row;
	const Out a = run(lo, { on(36, 0, 0.3) }, 2048);
	const Out b = run(hi, { on(36, 0, 1.0) }, 2048);
	const Out refA = refRender(lo.row, 0.3, 2048);
	const Out refB = refRender(hi.row, 1.0, 2048);
	CHECK(a.l == refA.l);
	CHECK(b.l == refB.l);
	CHECK(a.l != b.l);
	// THE LAWS DIFFER BY VELOCITY (the tone moves by tone x (velocity - 0.5)), so the equality above is not the same sound twice.
	CHECK(refLaws(lo.row, 0.3).tone != refLaws(lo.row, 1.0).tone);
}

TEST_CASE("drum: Pitch mode Off pins the hit's pitch to Tune, whatever the terrain's Pitch says") {
	Dev d;
	d.row = variedRow(drum::Kind::Membrane);
	d.set(Param::FieldPitch, 1.0); // as far as the terrain pushes it
	d.set(Param::Tune, 3.0);
	d.set(Param::PitchMode, 2.0); // Off
	(void)run(d, { on(36, 0) }, 64);
	REQUIRE(d.dsp().newestForTest() != nullptr);
	const double pinned = drum::kindBaseHz(drum::Kind::Membrane) * std::exp2(3.0 / 12.0);
	CHECK(d.dsp().newestForTest()->laws().hz == doctest::Approx(pinned).epsilon(1e-12));
	const drum::SettledPitch settled = drum::settledPitch(drum::Kind::Membrane, refFields(d.row), refControls(d.row), pinned);
	REQUIRE(settled.defined);
	CHECK(settled.hz == doctest::Approx(pinned).epsilon(1e-9));
	// NON-VACUITY: Free follows the terrain, so it is somewhere else.
	Dev f;
	f.row = d.row;
	f.set(Param::PitchMode, 0.0);
	(void)run(f, { on(36, 0) }, 64);
	CHECK(std::fabs(f.dsp().newestForTest()->laws().hz - pinned) > 1.0);
}

TEST_CASE("drum: Bend moves the hit by the key's tuning expression, held at the strike and followed while it rings") {
	Dev d;
	d.row = variedRow(drum::Kind::Membrane);
	d.set(Param::Bend, 1.0);
	(void)run(d, { on(36, 0), tuning(36, 12.0) }, 64);
	REQUIRE(d.dsp().newestForTest() != nullptr);
	CHECK(d.dsp().newestForTest()->laws().hz == refLaws(d.row, 1.0, 12.0).hz);
	// WHILE IT RINGS: a later tuning moves it, at the next call's refresh.
	(void)run(d, { tuning(36, -5.0) }, 64);
	CHECK(d.dsp().newestForTest()->laws().hz == refLaws(d.row, 1.0, -5.0).hz);
	CHECK(refLaws(d.row, 1.0, -5.0).hz != refLaws(d.row, 1.0, 12.0).hz);
	// BEND 0 IGNORES IT.
	Dev z;
	z.row = d.row;
	z.set(Param::Bend, 0.0);
	(void)run(z, { on(36, 0), tuning(36, 12.0) }, 64);
	CHECK(z.dsp().newestForTest()->laws().hz == refLaws(z.row, 1.0, 0.0).hz);
	// ANOTHER KEY'S TUNING DOES NOT TOUCH THIS ONE.
	Dev o;
	o.row = d.row;
	(void)run(o, { on(36, 0), tuning(40, 12.0) }, 64);
	CHECK(o.dsp().newestForTest()->laws().hz == refLaws(o.row, 1.0, 0.0).hz);
}

TEST_CASE("drum: Delay strikes the hit that long after the note, on the sample") {
	Dev d;
	d.row = variedRow(drum::Kind::Snare);
	d.set(Param::Delay, 5.0); // 240 samples at 48 kHz
	const Out got = run(d, { on(36, 100) }, 2000);
	const Out ref = refRender(d.row, 1.0, 2000 - 340);
	CHECK(allZero(got.l, 0, 340));
	bool same = true;
	for (size_t i = 0; i < 2000 - 340; ++i) {
		same = same && got.l[340 + i] == ref.l[i] && got.r[340 + i] == ref.r[i];
	}
	CHECK(same);
	CHECK(d.dsp().strikesForTest() == 1);
}

TEST_CASE("drum: High Pass is a one-pole on the hit, and at its bottom nothing is taken") {
	Dev d;
	d.row = variedRow(drum::Kind::Membrane);
	d.set(Param::Level, 0.0);
	d.set(Param::HighPass, std::log2(2000.0));
	const Out got = run(d, { on(36, 0) }, 3000);
	Out ref = refRender(d.row, 1.0, 3000);
	const double hz = std::exp2(static_cast<double>(d.row[at(Param::HighPass)]));
	const float a = static_cast<float>(1.0 / (1.0 + 2.0 * std::numbers::pi * hz / kSr));
	for (auto* ear : { &ref.l, &ref.r }) {
		float px = 0.0f;
		float py = 0.0f;
		for (float& x : *ear) {
			const float y = a * (py + x - px);
			px = x;
			py = y;
			x = y;
		}
	}
	CHECK(got.l == ref.l);
	CHECK(got.r == ref.r);
	// NON-VACUITY: it is not the plain hit.
	CHECK(got.l != refRender(d.row, 1.0, 3000).l);
}

TEST_CASE("drum: Trigger One-shot ignores the key's release; Gate damps the hit to Release") {
	const double releaseLog2 = std::log2(0.02);
	// ONE-SHOT: an off changes nothing.
	Dev a;
	a.row = variedRow(drum::Kind::Membrane);
	Dev b;
	b.row = a.row;
	const Out plain = run(a, { on(36, 0) }, 4096);
	const Out released = run(b, { on(36, 0), off(36, 700) }, 4096);
	CHECK(plain.l == released.l);
	// GATE: the hit renders to the off, is released, and renders on, as the engine's gate does it.
	Dev g;
	g.row = a.row;
	g.set(Param::Trigger, 1.0);
	g.set(Param::Release, releaseLog2);
	const Out gated = run(g, { on(36, 0), off(36, 700) }, 4096);
	RefHit h;
	h.hit.strike(kindOf(g.row), refLaws(g.row, 1.0), refSeed(g.row, 36, 0), 1.0f);
	Out ref(4096);
	h.hit.render(ref.l.data(), 700, ref.r.data());
	h.hit.release(std::exp2(static_cast<double>(g.row[at(Param::Release)])));
	h.hit.render(ref.l.data() + 700, 4096 - 700, ref.r.data() + 700);
	CHECK(gated.l == ref.l);
	CHECK(gated.l != plain.l);
	CHECK(allZero(gated.l, 0, 0) );
	// A FASTER RELEASE ENDS THE HIT SOONER (the value: the hit goes idle, the device lets go).
	Dev slow;
	slow.row = g.row;
	slow.set(Param::Release, std::log2(0.5));
	(void)run(slow, { on(36, 0), off(36, 700) }, 4096);
	(void)run(g, {}, 0);
	CHECK(slow.holds());
	// AN OFF FOR ANOTHER KEY OR ANOTHER NOTE ID DOES NOTHING.
	Dev k;
	k.row = g.row;
	const Out wrong = run(k, { on(36, 0, 1.0, 5), off(40, 700), off(36, 800, 6) }, 4096);
	CHECK(wrong.l == plain.l);
}

TEST_CASE("drum: a gate released while its hit waits out the Delay is damped as it strikes") {
	Dev g;
	g.row = variedRow(drum::Kind::Membrane);
	g.set(Param::Trigger, 1.0);
	g.set(Param::Release, std::log2(0.02));
	g.set(Param::Delay, 5.0);
	const Out got = run(g, { on(36, 0), off(36, 100) }, 4096);
	RefHit h;
	h.hit.strike(kindOf(g.row), refLaws(g.row, 1.0), refSeed(g.row, 36, 0), 1.0f);
	h.hit.release(std::exp2(static_cast<double>(g.row[at(Param::Release)])));
	Out ref(4096 - 240);
	h.hit.render(ref.l.data(), 4096 - 240, ref.r.data());
	CHECK(allZero(got.l, 0, 240));
	bool same = true;
	for (size_t i = 0; i < 4096 - 240; ++i) {
		same = same && got.l[240 + i] == ref.l[i];
	}
	CHECK(same);
}

TEST_CASE("drum: Voices overlap, and one more fades the oldest out over the short fade") {
	constexpr int kFade = 240; // 5 ms
	const auto fadeGain = [&](int j) { return j < kFade ? static_cast<float>(kFade - j) / static_cast<float>(kFade) : 0.0f; };
	// VOICES 1: the second note fades the first.
	Dev d;
	d.row = variedRow(drum::Kind::Membrane);
	d.set(Param::Voices, 0.0);
	d.set(Param::Level, 0.0);
	const Out got = run(d, { on(36, 0), on(36, 100) }, 2000);
	const Out r1 = refRender(d.row, 1.0, 2000, 0);
	const Out r2 = refRender(d.row, 1.0, 2000 - 100, 1);
	bool same = true;
	for (int i = 0; i < 2000; ++i) {
		float expect = i < 100 ? r1.l[static_cast<size_t>(i)] : 0.0f;
		if (i >= 100) {
			const float first = i - 100 < kFade ? r1.l[static_cast<size_t>(i)] * fadeGain(i - 100) : 0.0f;
			expect = first + r2.l[static_cast<size_t>(i - 100)];
		}
		same = same && got.l[static_cast<size_t>(i)] == expect;
	}
	CHECK(same);
	CHECK(d.dsp().soundingForTest() == 1);
	CHECK(d.dsp().fadingForTest() == 0);
	// VOICES 4: four notes ring together, a fifth fades the oldest.
	Dev q;
	q.row = d.row;
	q.set(Param::Voices, 2.0);
	(void)run(q, { on(36, 0), on(36, 10), on(36, 20), on(36, 30) }, 64);
	CHECK(q.dsp().soundingForTest() == 4);
	CHECK(q.dsp().fadingForTest() == 0);
	(void)run(q, { on(36, 0) }, 64);
	CHECK(q.dsp().soundingForTest() == 4);
	CHECK(q.dsp().fadingForTest() == 1);
	Out rest(2 * kFade);
	q.render(rest.l.data(), rest.r.data(), 2 * kFade);
	CHECK(q.dsp().fadingForTest() == 0);
	CHECK(q.dsp().soundingForTest() == 4);
	// VOICES 2 with three notes: two sound, one fading.
	Dev t;
	t.row = d.row;
	t.set(Param::Voices, 1.0);
	(void)run(t, { on(36, 0), on(36, 10), on(36, 20) }, 64);
	CHECK(t.dsp().soundingForTest() == 2);
	CHECK(t.dsp().fadingForTest() == 1);
}

TEST_CASE("drum: a Choke for its key (or the wildcard) fades every hit out; another key's does nothing") {
	constexpr int kFade = 240;
	for (const int chokeKey : { 36, -1 }) {
		CAPTURE(chokeKey);
		Dev d;
		d.row = variedRow(drum::Kind::Membrane);
		d.set(Param::Level, 0.0);
		const Out got = run(d, { on(36, 0), choke(chokeKey, 300) }, 3000);
		const Out r1 = refRender(d.row, 1.0, 3000);
		bool same = true;
		for (int i = 0; i < 3000; ++i) {
			float expect = r1.l[static_cast<size_t>(i)];
			if (i >= 300) {
				expect = i - 300 < kFade ? r1.l[static_cast<size_t>(i)] * (static_cast<float>(kFade - (i - 300)) / static_cast<float>(kFade)) : 0.0f;
			}
			same = same && got.l[static_cast<size_t>(i)] == expect;
		}
		CHECK(same);
		CHECK(allZero(got.l, 300 + kFade));
		CHECK_FALSE(d.holds());
	}
	Dev o;
	o.row = variedRow(drum::Kind::Membrane);
	const Out kept = run(o, { on(36, 0), choke(40, 300) }, 3000);
	Dev p;
	p.row = o.row;
	CHECK(kept.l == run(p, { on(36, 0) }, 3000).l); // the same hit, never choked
	CHECK_FALSE(allZero(kept.l, 3000 - 64));
}

TEST_CASE("drum: two drums on one key sum, each as it sounds alone, and each passes the other's notes") {
	Dev a;
	a.row = variedRow(drum::Kind::Kick);
	Dev b;
	b.row = variedRow(drum::Kind::Snare);
	b.set(Param::Seed, 99);
	const std::vector<NoteEvent> in = { on(36, 21, 0.9) };
	Out chain(1500);
	{
		const std::vector<NoteEvent> mid = a.notes(in);
		const std::vector<NoteEvent> out = b.notes(mid);
		REQUIRE(out.size() == 1);
		// FIELD BY FIELD, not memcmp: the struct's padding bytes are indeterminate, and differ between compilers.
		CHECK(out[0].kind == in[0].kind);
		CHECK(out[0].timeSamples == in[0].timeSamples);
		CHECK(out[0].noteId == in[0].noteId);
		CHECK(out[0].port == in[0].port);
		CHECK(out[0].channel == in[0].channel);
		CHECK(out[0].key == in[0].key);
		CHECK(out[0].velocity == in[0].velocity);
		CHECK(out[0].expressionId == in[0].expressionId);
		CHECK(out[0].expressionValue == in[0].expressionValue);
		CHECK(out[0].fromSteal == in[0].fromSteal);
		a.render(chain.l.data(), chain.r.data(), 1500);
		b.render(chain.l.data(), chain.r.data(), 1500);
	}
	Dev a2;
	a2.row = a.row;
	Dev b2;
	b2.row = b.row;
	const Out onlyA = run(a2, in, 1500);
	const Out onlyB = run(b2, in, 1500);
	bool same = true;
	for (size_t i = 0; i < 1500; ++i) {
		same = same && chain.l[i] == onlyA.l[i] + onlyB.l[i] && chain.r[i] == onlyA.r[i] + onlyB.r[i];
	}
	CHECK(same);
	CHECK(onlyA.l != onlyB.l);
	// ADDS TO ITS INPUT: a buffer already carrying a signal keeps it.
	Dev c;
	c.row = a.row;
	Out in2(1500);
	for (size_t i = 0; i < 1500; ++i) {
		in2.l[i] = 0.25f * std::sin(0.01f * static_cast<float>(i));
		in2.r[i] = -in2.l[i];
	}
	const Out base = in2;
	(void)c.notes(in);
	c.render(in2.l.data(), in2.r.data(), 1500);
	same = true;
	for (size_t i = 0; i < 1500; ++i) {
		same = same && in2.l[i] == base.l[i] + onlyA.l[i] && in2.r[i] == base.r[i] + onlyA.r[i];
	}
	CHECK(same);
}

TEST_CASE("drum: the seed is the Seed control, the key, the kind and the song's place, never anything incidental") {
	// NO TRANSPORT: the key's own strike count; two fresh devices strike the same noise.
	Dev a;
	Dev b;
	a.row = b.row = variedRow(drum::Kind::Clap);
	const Out x = run(a, { on(36, 0), on(36, 700) }, 3000);
	const Out y = run(b, { on(36, 0), on(36, 700) }, 3000);
	CHECK(x.l == y.l);
	CHECK(a.dsp().newestSeedForTest() == refSeed(a.row, 36, 1));
	// THE SONG'S PLACE: a transport at a position gives the same noise at the same tick, whatever else happened before.
	device::TransportClock clock;
	clock.playing = true;
	clock.songPositionSeconds = 12.0;
	Dev c;
	c.row = a.row;
	dd::kAudioOps.setTransportClock(c.st, clock);
	(void)run(c, { on(36, 5) }, 256);
	const int64_t tick = static_cast<int64_t>(std::floor((12.0 * kSr + 5.0) / 64.0));
	CHECK(c.dsp().newestSeedForTest() == dd::strikeSeed(static_cast<uint64_t>(std::lround(c.row[at(Param::Seed)])), 36, drum::Kind::Clap, tick, 0));
	// AND A DIFFERENT SEED, KEY OR KIND IS A DIFFERENT NOISE.
	const uint64_t base = dd::strikeSeed(7, 36, drum::Kind::Clap, tick, 0);
	CHECK(base != dd::strikeSeed(8, 36, drum::Kind::Clap, tick, 0));
	CHECK(base != dd::strikeSeed(7, 37, drum::Kind::Clap, tick, 0));
	CHECK(base != dd::strikeSeed(7, 36, drum::Kind::Snare, tick, 0));
	CHECK(base != dd::strikeSeed(7, 36, drum::Kind::Clap, tick + 1, 0));
}

//--------------------------------------------------------------------------------------------------//
// THE TAIL AND THE REAL-TIME RULES                                                                   //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("drum: the tail reaches exact zero, holdsTail is true exactly while it sounds, in every kind and with a high-pass and a delay") {
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		for (const bool extras : { false, true }) {
			CAPTURE(drum::kKindNames[k]);
			CAPTURE(extras);
			Dev d;
			d.row = variedRow(static_cast<drum::Kind>(k));
			d.set(Param::Level, 0.0);
			d.set(Param::Decay, -2.5); // a short body: the sweep is about the END
			if (extras) {
				d.set(Param::HighPass, std::log2(1500.0));
				d.set(Param::Delay, 3.0);
			}
			(void)d.notes({ on(36, 0), on(36, 200) });
			int lastNonzero = -1;
			int block = 0;
			for (; block < 3000 && (block == 0 || d.holds()); ++block) {
				Out o(64);
				d.render(o.l.data(), o.r.data(), 64);
				if (firstNonzero(o.l) >= 0 || firstNonzero(o.r) >= 0) {
					lastNonzero = block;
				}
			}
			REQUIRE(block < 3000);
			REQUIRE(lastNonzero >= 0);
			// TRUE ONLY WHILE IT SOUNDS: holdsTail turned false soon after the last sample that was not zero. MEASURED at this row: one block
			// in every case but the Snare without the high-pass, where the hit's own silence check lets 17 blocks (23 ms) of exact zeros
			// pass before it calls itself idle; the bound is that, with room, and it is the hit's, not the device's.
			CHECK(block - lastNonzero <= 24);
			// holdsTail went false: from here on, exact zero (the next hundred blocks).
			CHECK_FALSE(d.holds());
			for (int more = 0; more < 100; ++more) {
				Out o(64);
				d.render(o.l.data(), o.r.data(), 64);
				REQUIRE(allZero(o.l));
				REQUIRE(allZero(o.r));
			}
		}
	}
}

TEST_CASE("drum: holdsTail never goes false in a block that has sound still to come") {
	Dev d;
	d.row = variedRow(drum::Kind::Membrane);
	d.set(Param::Decay, -2.0);
	(void)d.notes({ on(36, 0) });
	std::vector<Out> blocks;
	std::vector<bool> held;
	for (int i = 0; i < 1500; ++i) {
		Out o(64);
		d.render(o.l.data(), o.r.data(), 64);
		blocks.push_back(o);
		held.push_back(d.holds());
	}
	for (size_t i = 0; i + 1 < blocks.size(); ++i) {
		if (!held[i]) {
			// THE DEVICE SAID "DONE" AFTER BLOCK i: nothing in any later block.
			for (size_t j = i + 1; j < blocks.size(); ++j) {
				REQUIRE(allZero(blocks[j].l));
				REQUIRE(allZero(blocks[j].r));
			}
			break;
		}
	}
	CHECK_FALSE(held.back());
	CHECK(held.front());
}

TEST_CASE("drum: a busy pattern allocates nothing on the audio thread after activate") {
	Dev d;
	d.row = variedRow(drum::Kind::Kick);
	d.set(Param::Voices, 2.0);
	d.set(Param::HighPass, std::log2(800.0));
	d.set(Param::Delay, 2.0);
	d.set(Param::Trigger, 1.0);
	std::vector<NoteEvent> out(16);
	float l[64];
	float r[64];
	AllocWindow window;
	for (int block = 0; block < 3000; ++block) {
		d.set(Param::Kind, static_cast<double>((block / 50) % 6));
		d.set(Param::TravelX, static_cast<double>(block % 97) * 3.5 - 100.0); // a route moving the terrain
		d.set(Param::Tune, static_cast<double>(block % 13) - 6.0);
		std::vector<NoteEvent> in;
		NoteEvent e = on(36, static_cast<uint32_t>(block % 50), 0.3 + 0.7 * static_cast<double>(block % 7) / 7.0);
		NoteEvent* ev = &e;
		size_t produced = 0;
		d.apply();
		if (block % 3 == 0) {
			dd::kNoteOps.processNotes(d.st, ev, 1, out.data(), out.size(), &produced);
		}
		if (block % 11 == 5) {
			NoteEvent c = choke(36, 10);
			dd::kNoteOps.processNotes(d.st, &c, 1, out.data(), out.size(), &produced);
		}
		if (block % 7 == 3) {
			NoteEvent o = off(36, 20);
			dd::kNoteOps.processNotes(d.st, &o, 1, out.data(), out.size(), &produced);
		}
		std::fill(l, l + 64, 0.0f);
		std::fill(r, r + 64, 0.0f);
		device::NoteContext c;
		c.tickPhase = 0;
		dd::kAudioOps.setNoteContext(d.st, c);
		dd::kAudioOps.processStereo(d.st, l, r, 64);
	}
	CHECK(window.allocs() == 0);
	CHECK(d.dsp().strikesForTest() > 100);
}

TEST_CASE("drum: reset is as if just switched on — nothing struck, nothing queued, the strike count and the tunings forgotten") {
	Dev used;
	used.row = variedRow(drum::Kind::Clap);
	(void)run(used, { on(36, 0), tuning(36, 5.0), on(36, 90) }, 700);
	(void)used.notes({ on(36, 5) }); // queued, never rendered
	REQUIRE(used.holds());
	dd::kAudioOps.reset(used.st);
	CHECK_FALSE(used.holds());
	CHECK(used.dsp().soundingForTest() == 0);
	CHECK(used.dsp().keyTuningForTest(36) == 0.0f);
	Dev fresh;
	fresh.row = used.row;
	const Out a = run(used, { on(36, 12) }, 2000);
	const Out b = run(fresh, { on(36, 12) }, 2000);
	CHECK(a.l == b.l);
	CHECK(a.r == b.r);
	CHECK_FALSE(allZero(a.l));
}

TEST_CASE("drum: the sample rate reaches the hit — at 96 kHz the device is the engine's hit prepared at 96 kHz, and a choke's fade is 5 ms of it") {
	Dev d(96000.0);
	d.row = variedRow(drum::Kind::Membrane);
	const Out got = run(d, { on(36, 0) }, 4000);
	const Out ref = refRender(d.row, 1.0, 4000, 0, 36, 96000.0);
	CHECK(got.l == ref.l);
	Dev lo;
	lo.row = d.row;
	CHECK(run(lo, { on(36, 0) }, 4000).l != got.l);
	Dev c(96000.0);
	c.row = d.row;
	const Out choked = run(c, { on(36, 0), choke(36, 300) }, 3000);
	CHECK_FALSE(allZero(choked.l, 300 + 479, 300 + 480));
	CHECK(allZero(choked.l, 300 + 480));
}

TEST_CASE("drum: every control changes the sound in SOME arrangement, with notes — a control that changes nothing is not drawn") {
	// THE HONESTY RULE, ASKED OF THE DSP WITH A DRUM'S OWN ARRANGEMENTS: every kind, both triggers, the snare's wire and bell zones in
	// each of theirs; the script strikes, tunes and re-strikes the key (Voices, Bend, Velocity), then lets it go (Trigger, Release).
	// The enable is the engine's (`engineEnable`), so the device's own row never sees it off, and it is the one named exemption.
	const std::vector<std::pair<std::vector<NoteEvent>, int>> script = {
		{ { on(36, 0, 0.9), tuning(36, 7.0) }, 600 }, { { on(36, 0, 0.6) }, 600 }, { { on(36, 0, 0.8) }, 600 }, { { off(36, 0) }, 1200 },
	};
	Dev d;
	const auto play = [&](const dd::Row& row) {
		dd::kAudioOps.reset(d.st);
		d.row = row;
		Out all(0);
		for (const auto& [ev, frames] : script) {
			Out o(frames);
			(void)d.notes(ev);
			d.render(o.l.data(), o.r.data(), frames);
			all.l.insert(all.l.end(), o.l.begin(), o.l.end());
			all.r.insert(all.r.end(), o.r.begin(), o.r.end());
		}
		return all;
	};
	std::vector<dd::Row> bases;
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		for (const double trig : { 0.0, 1.0 }) {
			for (const double bells : { 0.4, 0.8 }) {
				for (const double wires : { 0.0, 0.5 }) {
					if (k != static_cast<size_t>(drum::Kind::Snare) && (bells != 0.4 || wires != 0.0)) {
						continue;
					}
					dd::Row r = variedRow(static_cast<drum::Kind>(k));
					r[at(Param::Trigger)] = static_cast<float>(trig);
					r[at(Param::Release)] = static_cast<float>(std::log2(0.05));
					r[at(Param::Bell1Level)] = static_cast<float>(bells);
					r[at(Param::WireGate)] = static_cast<float>(wires);
					if (wires > 0.0) { // a short head and wires, so a gate at a share of their decay falls inside the script
						r[at(Param::Decay)] = -6.6f;
						r[at(Param::WireDecay)] = -6.6f;
					}
					bases.push_back(r);
				}
			}
		}
	}
	std::vector<Out> baseline;
	for (const dd::Row& b : bases) {
		baseline.push_back(play(b));
		REQUIRE_FALSE(allZero(baseline.back().l)); // every arrangement sounds, or "no difference" is a claim about silence
	}
	std::vector<std::string> dead;
	int arrangements = 0;
	for (size_t p = 0; p < std::size(dd::kParamDescs); ++p) {
		const device::ParamDesc& desc = dd::kParamDescs[p];
		if (static_cast<int>(p) == dd::kTraits.enableControl) {
			continue;
		}
		std::vector<double> values;
		if (desc.stepped) {
			for (double v = desc.min; v <= desc.max; v += 1.0) {
				values.push_back(v);
			}
		}
		else if (desc.max - desc.min > 1.0e6) {
			values = { -1234.5, -3.7, 3.7, 50.0, 1234.5 };
		}
		else {
			values = { desc.min, desc.max, desc.min + 0.37 * (desc.max - desc.min), desc.min + 0.71 * (desc.max - desc.min) };
		}
		bool moved = false;
		for (size_t b = 0; b < bases.size() && !moved; ++b) {
			for (const double v : values) {
				dd::Row r = bases[b];
				r[at(static_cast<Param>(p))] = static_cast<float>(v);
				++arrangements;
				const Out o = play(r);
				if (o.l != baseline[b].l || o.r != baseline[b].r) {
					moved = true;
					break;
				}
			}
		}
		if (!moved) {
			dead.push_back(desc.key);
		}
	}
	CHECK(arrangements > 300); // (a control that moves the sound ends its own search early)
	CHECK(dead == std::vector<std::string> {});
	CHECK(dd::kTraits.enableControl == static_cast<int>(Param::Enabled));
}

TEST_CASE("drum: the same host notes in any block sizes are the same sound, to the bit (the rack cuts the call on every tick)") {
	constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 8 };
	constexpr int kTotal = 1500;
	// Absolute host sample, event: a strike, a re-strike under it, a release (Gate), a tuning, a choke.
	const std::vector<std::pair<int, NoteEvent>> script = { { 50, on(36, 0, 0.9) }, { 333, on(36, 0, 0.5) }, { 700, off(36, 0) },
		                                                    { 1000, on(36, 0, 1.0) }, { 1200, choke(36, 0) }, { 1210, on(40, 0) } };
	const auto render = [&](const std::vector<int>& blocks) {
		rack::RackEngine engine(kAxes, hostCatalogue());
		engine.prepare(kSr, 4096);
		const size_t slot = engine.addDevice(dd::kTraits.tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		engine.prepare(kSr, 4096);
		engine.setField(slot, at(Param::Trigger), 1.0f);
		engine.setField(slot, at(Param::Voices), 1.0f);
		engine.setField(slot, at(Param::Release), static_cast<float>(std::log2(0.03)));
		Out out(kTotal);
		int start = 0;
		for (const int n : blocks) {
			std::vector<NoteEvent> here;
			for (const auto& [t, e] : script) {
				if (t >= start && t < start + n) {
					NoteEvent ev = e;
					ev.timeSamples = static_cast<uint32_t>(t - start);
					here.push_back(ev);
				}
			}
			engine.setBlockNotes(here.data(), here.size());
			engine.processStereo(out.l.data() + start, out.r.data() + start, n, backplane::unknownBlock());
			start += n;
		}
		REQUIRE(start == kTotal);
		return out;
	};
	const Out whole = render({ kTotal });
	CHECK_FALSE(allZero(whole.l));
	const std::vector<std::vector<int>> cuts = {
		{ 500, 500, 500 }, { 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 28 },
		{ 37, 1, 100, 62, 300, 5, 995 }, { 1499, 1 }, { 333, 367, 300, 500 },
	};
	for (const auto& cut : cuts) {
		const Out got = render(cut);
		CHECK(got.l == whole.l);
		CHECK(got.r == whole.r);
	}
}

TEST_CASE("drum: a sample that is not a number is not passed on, and the drum is added to a clean input unchanged") {
	Dev d;
	d.row = variedRow(drum::Kind::Membrane);
	Out in(512);
	for (size_t i = 0; i < 512; ++i) {
		in.l[i] = 0.25f * std::sin(0.02f * static_cast<float>(i));
		in.r[i] = -in.l[i];
	}
	in.l[5] = std::numeric_limits<float>::quiet_NaN();
	in.r[77] = std::numeric_limits<float>::infinity();
	in.l[300] = -std::numeric_limits<float>::infinity();
	const Out clean = in;
	(void)d.notes({ on(36, 0) });
	d.render(in.l.data(), in.r.data(), 512);
	const Out ref = refRender(d.row, 1.0, 512);
	for (size_t i = 0; i < 512; ++i) {
		REQUIRE(std::isfinite(in.l[i]));
		REQUIRE(std::isfinite(in.r[i]));
		const float wantL = std::isfinite(clean.l[i]) ? clean.l[i] : 0.0f;
		const float wantR = std::isfinite(clean.r[i]) ? clean.r[i] : 0.0f;
		REQUIRE(in.l[i] == wantL + ref.l[i]);
		REQUIRE(in.r[i] == wantR + ref.r[i]);
	}
}

TEST_CASE("drum: at its longest decay and release, every kind reaches exact zero on both channels within a bound, stays zero, and lets go of holdsTail") {
	// THE GENERIC SILENCE SWEEP (test_fx_devices, "the tail reaches exact silence") ASKED WITH NOTES: a burst, then silence; the
	// output must be exactly 0.0f from some block on and holdsTail false. The longest settings: Decay at its maximum (10 s), the
	// terrain's Length at its maximum, and for the Gate the longest Release (2 s). Bounds in blocks of 64 at 48 kHz.
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		for (const bool gate : { true, false }) {
			CAPTURE(drum::kKindNames[k]);
			CAPTURE(gate);
			Dev d;
			d.row = variedRow(static_cast<drum::Kind>(k));
			d.set(Param::Decay, dd::kParamDescs[at(Param::Decay)].max);
			d.set(Param::FieldLength, 1.0);
			d.set(Param::Release, dd::kParamDescs[at(Param::Release)].max);
			d.set(Param::Trigger, gate ? 1.0 : 0.0);
			d.set(Param::HighPass, std::log2(300.0));
			(void)d.notes({ on(36, 0), on(36, 150) });
			int lastNonzero = -1;
			int block = 0;
			// A gate is let go after 100 blocks; a one-shot rings its whole (very long) body.
			const int kBound = gate ? 6000 : 100000;
			for (; block < kBound && (block == 0 || d.holds()); ++block) {
				if (gate && block == 100) {
					(void)d.notes({ off(36, 0) });
				}
				Out o(64);
				d.render(o.l.data(), o.r.data(), 64);
				if (firstNonzero(o.l) >= 0 || firstNonzero(o.r) >= 0) {
					lastNonzero = block;
				}
			}
			REQUIRE(block < kBound);
			REQUIRE(lastNonzero >= 100);
			CHECK_FALSE(d.holds());
			for (int more = 0; more < 400; ++more) {
				Out o(64);
				d.render(o.l.data(), o.r.data(), 64);
				REQUIRE(allZero(o.l));
				REQUIRE(allZero(o.r));
			}
			CHECK_FALSE(d.holds());
		}
	}
}

TEST_CASE("drum: NaN and infinity on the input or in a control do not latch the device, and a later hit equals a clean instance's") {
	// THE GENERIC SWEEP ("one NaN and one +Inf sample do not latch") ASKED WITH A DRUM SOUNDING: bad samples go through a ringing hit,
	// then clean silence, then a new hit. The twin heard the same notes with a clean input.
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		CAPTURE(drum::kKindNames[k]);
		Dev bad;
		Dev clean;
		bad.row = clean.row = variedRow(static_cast<drum::Kind>(k));
		for (Dev* d : { &bad, &clean }) {
			(void)d->notes({ on(36, 0) });
		}
		Out a(512);
		Out b(512);
		a.l[3] = std::numeric_limits<float>::quiet_NaN();
		a.r[10] = std::numeric_limits<float>::infinity();
		a.l[200] = -std::numeric_limits<float>::infinity();
		a.r[300] = std::numeric_limits<float>::quiet_NaN();
		bad.render(a.l.data(), a.r.data(), 512);
		clean.render(b.l.data(), b.r.data(), 512);
		CHECK(a.l[3] == b.l[3]); // the NaN became a zero, so it is the hit alone
		for (size_t i = 0; i < 512; ++i) {
			REQUIRE(std::isfinite(a.l[i]));
			REQUIRE(std::isfinite(a.r[i]));
		}
		// A NON-FINITE CONTROL is its declared default, as a row holding it is cleaned.
		bad.row[at(Param::Tune)] = std::numeric_limits<float>::quiet_NaN();
		bad.row[at(Param::Decay)] = std::numeric_limits<float>::infinity();
		bad.row[at(Param::Pan)] = -std::numeric_limits<float>::infinity();
		bad.row[at(Param::TravelX)] = std::numeric_limits<float>::quiet_NaN();
		Out c(256);
		bad.render(c.l.data(), c.r.data(), 256);
		for (size_t i = 0; i < 256; ++i) {
			REQUIRE(std::isfinite(c.l[i]));
			REQUIRE(std::isfinite(c.r[i]));
		}
		bad.row = clean.row;
		Out d2(256);
		clean.render(d2.l.data(), d2.r.data(), 256);
		// NEW HITS ON BOTH, after the bad input has stopped. The hits' own history differs only through the bad controls above, which
		// never reach the struck hit (it is struck from the restored row): the same notes must give the same sound.
		Dev bad2;
		Dev clean2;
		bad2.row = clean2.row = clean.row;
		Out x(1024);
		Out y(1024);
		(void)bad2.notes({ on(36, 0) });
		(void)clean2.notes({ on(36, 0) });
		bad2.render(x.l.data(), x.r.data(), 512);
		clean2.render(y.l.data(), y.r.data(), 512);
		Out junk(512);
		junk.l[7] = std::numeric_limits<float>::quiet_NaN();
		junk.r[9] = std::numeric_limits<float>::infinity();
		bad2.render(junk.l.data(), junk.r.data(), 512); // bad input, then
		Out z1(512), z2(512);
		clean2.render(z2.l.data(), z2.r.data(), 512);
		Out fresh1(512), fresh2(512);
		(void)bad2.notes({ on(36, 20) });
		(void)clean2.notes({ on(36, 20) });
		bad2.render(fresh1.l.data(), fresh1.r.data(), 512);
		clean2.render(fresh2.l.data(), fresh2.r.data(), 512);
		for (size_t i = 0; i < 512; ++i) {
			REQUIRE(std::isfinite(fresh1.l[i]));
		}
		CHECK(fresh1.l == fresh2.l);
		CHECK(fresh1.r == fresh2.r);
		CHECK_FALSE(allZero(fresh1.l));
	}
}

TEST_CASE("drum: every control at each end of its range, in every kind, stays a finite signal") {
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		CAPTURE(drum::kKindNames[k]);
		for (size_t p = 0; p < std::size(dd::kParamDescs); ++p) {
			if (static_cast<int>(p) == dd::kTraits.enableControl) {
				continue;
			}
			const device::ParamDesc& desc = dd::kParamDescs[p];
			for (const double v : { desc.min, desc.max, desc.max - desc.min > 1.0e6 ? 1.0e6 : desc.max }) {
				CAPTURE(desc.key);
				Dev d;
				d.row = variedRow(static_cast<drum::Kind>(k));
				d.row[p] = static_cast<float>(v);
				d.set(Param::Trigger, 1.0);
				(void)d.notes({ on(36, 0), on(36, 100) });
				Out o(1024);
				d.render(o.l.data(), o.r.data(), 1024);
				for (size_t i = 0; i < 1024; ++i) {
					REQUIRE(std::isfinite(o.l[i]));
					REQUIRE(std::isfinite(o.r[i]));
				}
			}
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// STATE                                                                                              //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("drum: the row round-trips with the unbounded travel, and is cleaned into its range") {
	dd::Row row = variedRow(drum::Kind::Snare);
	for (const double x : { 50.0, -1234.5, 8388608.0, -8388608.0, 0.001 }) {
		row[at(Param::TravelX)] = static_cast<float>(x);
		row[at(Param::TravelY)] = static_cast<float>(-x);
		const dd::Settings s = dd::stateOf(row, row.size());
		CHECK(s.at(Param::TravelX) == static_cast<float>(x));
		CHECK(s.at(Param::TravelY) == static_cast<float>(-x));
		CHECK(dd::stateOf(dd::rowOf(s), row.size()) == s);
	}
	// A SHORT ROW takes the declared defaults; a non-finite value is its default; stepped values round; a range is held.
	dd::Row bad = defaultRow();
	bad[at(Param::Level)] = std::numeric_limits<float>::quiet_NaN();
	bad[at(Param::Kind)] = 2.4f;
	bad[at(Param::Pan)] = 5.0f;
	bad[at(Param::TravelX)] = 1.0e12f;
	const dd::Settings s = dd::stateOf(bad, at(Param::Pan) + 1);
	CHECK(s.at(Param::Level) == 0.0f);
	CHECK(s.at(Param::Kind) == 2.0f);
	CHECK(s.at(Param::Pan) == 1.0f);
	CHECK(s.at(Param::TravelX) == static_cast<float>(dd::kParamDescs[at(Param::TravelX)].def)); // past the row: the default
	CHECK(dd::stateOf(dd::Row {}, 0) == dd::stateOf(defaultRow(), defaultRow().size()));
}

TEST_CASE("drum: a patch saved and loaded into a fresh rack gives the same settings, travel included, and the same sound") {
	persist::Rig a;
	a.seat(dd::kTraits.tag);
	const dd::Row want = variedRow(drum::Kind::Metal);
	for (size_t i = 0; i < want.size(); ++i) {
		a.bridge->setField(a.id, i, want[i]);
	}
	a.block(2);
	const dd::Settings saved = a.as<dd::DrumDsp>().settingsForTest();
	REQUIRE(saved.at(Param::TravelX) == 50.0f);
	REQUIRE(saved.at(Param::TravelY) == -1234.5f);
	const std::string text = a.save();
	persist::Rig b;
	b.load(text);
	b.block(2);
	const dd::Settings loaded = b.as<dd::DrumDsp>().settingsForTest();
	CHECK(loaded == saved);
	CHECK(loaded.at(Param::TravelX) == 50.0f);
	CHECK(loaded.at(Param::TravelY) == -1234.5f);
	CHECK(b.as<dd::DrumDsp>().fieldsForTest() == a.as<dd::DrumDsp>().fieldsForTest());
	CHECK(a.as<dd::DrumDsp>().fieldsForTest() == refFields(want));
}

//--------------------------------------------------------------------------------------------------//
// THE RACK                                                                                           //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("drum: in a rack, a host note lands on its own sample whatever the block sizes and the tick phase") {
	// A BLOCK OF 37 FIRST, so the tick phase is 37 and the next block's pieces start inside a tick: the note at host sample 10 is
	// `timeSamples` 47 in its tick, and the device places it at 10 only by reading the stamp.
	constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 8 };
	for (const int lead : { 0, 37, 63, 64, 100 }) {
		for (const int at0 : { 0, 10, 40, 130 }) {
			CAPTURE(lead);
			CAPTURE(at0);
			rack::RackEngine engine(kAxes, hostCatalogue());
			engine.prepare(kSr, 4096);
			const size_t slot = engine.addDevice(dd::kTraits.tag);
			REQUIRE(slot != rack::RackEngine::kNoSlot);
			engine.prepare(kSr, 4096);
			std::vector<float> l(256, 0.0f);
			std::vector<float> r(256, 0.0f);
			if (lead > 0) {
				std::vector<float> a(static_cast<size_t>(lead), 0.0f);
				std::vector<float> b(static_cast<size_t>(lead), 0.0f);
				engine.setBlockNotes(nullptr, 0);
				engine.processStereo(a.data(), b.data(), lead, backplane::unknownBlock());
			}
			const NoteEvent n = on(36, static_cast<uint32_t>(at0));
			engine.setBlockNotes(&n, 1);
			engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
			const dd::Row row = defaultRow();
			const Out ref = refRender(row, 1.0, 256 - at0);
			REQUIRE(firstNonzero(ref.l) >= 0);
			CHECK(allZero(l, 0, static_cast<size_t>(at0)));
			CHECK(firstNonzero(l) == at0 + firstNonzero(ref.l));
			CHECK(l[static_cast<size_t>(at0 + firstNonzero(ref.l))] == ref.l[static_cast<size_t>(firstNonzero(ref.l))]);
			// THE NOTE PASSED THROUGH THE CHAIN.
			const auto outNotes = engine.blockNotesOut();
			REQUIRE(outNotes.size() == 1);
			CHECK(outNotes[0].key == 36);
			CHECK(outNotes[0].timeSamples == static_cast<uint32_t>(at0));
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// PITCHED KEYS                                                                                        //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A PITCHED ROW: a Kick with no sweep (a sine at the key's frequency, so the fundamental IS the key; a Membrane's lowest mode sits at a
/// ratio above its head), every key answered. The terrain's own fine pitch (half a semitone either way, in Pitched)
/// is left where the row puts it.
dd::Row pitchedRow(int note = 36) {
	dd::Row r = defaultRow();
	r[at(Param::Kind)] = static_cast<float>(drum::Kind::Kick);
	r[at(Param::Keys)] = 2.0f; // Pitched
	r[at(Param::ClickSweep)] = 0.0f;
	r[at(Param::Note)] = static_cast<float>(note);
	r[at(Param::FieldSweep)] = 0.0f;
	r[at(Param::Decay)] = static_cast<float>(std::log2(1.5));
	return r;
}

/// THE FUNDAMENTAL, MEASURED: the lag of the strongest normalised autocorrelation peak between `lo` and `hi` Hz, over a stretch of the
/// render past the strike's first milliseconds. An independent road to a frequency, not the engine's own laws.
double measuredHz(const std::vector<float>& x, double lo, double hi) {
	const size_t from = static_cast<size_t>(0.02 * kSr);
	const size_t len = static_cast<size_t>(0.25 * kSr);
	REQUIRE(x.size() >= from + len + static_cast<size_t>(kSr / lo) + 2);
	const size_t lagLo = static_cast<size_t>(kSr / hi);
	const size_t lagHi = static_cast<size_t>(kSr / lo);
	std::vector<double> ac(lagHi + 2, 0.0);
	for (size_t lag = lagLo; lag <= lagHi + 1; ++lag) {
		double s = 0.0;
		for (size_t i = 0; i < len; ++i) {
			s += static_cast<double>(x[from + i]) * static_cast<double>(x[from + i + lag]);
		}
		ac[lag] = s;
	}
	// THE FIRST PEAK THAT REACHES 90% OF THE BEST (the fundamental's lag, not its multiples), refined by a parabola.
	double best = 0.0;
	for (size_t lag = lagLo; lag <= lagHi; ++lag) {
		best = std::max(best, ac[lag]);
	}
	for (size_t lag = lagLo + 1; lag <= lagHi; ++lag) {
		if (ac[lag] >= 0.9 * best && ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1]) {
			const double a = ac[lag - 1];
			const double b = ac[lag];
			const double c = ac[lag + 1];
			const double shift = 0.5 * (a - c) / (a - 2.0 * b + c);
			return kSr / (static_cast<double>(lag) + shift);
		}
	}
	return 0.0;
}

std::vector<float> renderKey(const dd::Row& row, int key, int frames = 16384) {
	Dev d;
	d.row = row;
	d.apply();
	(void)d.notes({ on(key, 0) });
	Out o(frames);
	d.render(o.l.data(), o.r.data(), frames);
	return o.l;
}

double keyHz(int key, double tuneSemis = 0.0) {
	return 440.0 * std::exp2((static_cast<double>(key) - 69.0 + tuneSemis) / 12.0);
}

} // namespace

TEST_CASE("drum Pitched: it answers a key other than Note, and One Key still does not") {
	for (const bool pitched : { true, false }) {
		CAPTURE(pitched);
		dd::Row r = pitchedRow(36);
		r[at(Param::Keys)] = pitched ? 2.0f : 0.0f;
		const std::vector<float> got = renderKey(r, 52, 4096);
		// KEY 52 IS NOT NOTE (36): Pitched strikes, One Key lets the note pass untouched and makes no sound.
		CHECK(allZero(got) == !pitched);
		const std::vector<float> own = renderKey(r, 36, 4096);
		CHECK_FALSE(allZero(own));
	}
	// AND A CHOKE ON ANOTHER KEY reaches a Pitched hit on that key: the hit fades out.
	Dev d;
	d.row = pitchedRow(36);
	d.apply();
	(void)d.notes({ on(70, 0) });
	Out warm(512);
	d.render(warm.l.data(), warm.r.data(), 512);
	REQUIRE(d.dsp().soundingForTest() == 1);
	(void)d.notes({ choke(70, 0) });
	Out after(2048);
	d.render(after.l.data(), after.r.data(), 2048);
	CHECK(d.dsp().soundingForTest() == 0);
}

TEST_CASE("drum Pitched: the fundamental is the key's own frequency, Tune moves it, an octave up doubles it") {
	for (const int key : { 48, 60, 72 }) {
		CAPTURE(key);
		const double want = keyHz(key);
		const double hz = measuredHz(renderKey(pitchedRow(), key), want * 0.7, want * 1.4);
		CAPTURE(hz);
		CAPTURE(want);
		// THE VALUE, to within the half semitone the terrain's own fine pitch may move it (Pitched's reach), and not a hair outside.
		CHECK(hz >= want * std::exp2(-0.5 / 12.0) * 0.998);
		CHECK(hz <= want * std::exp2(0.5 / 12.0) * 1.002);
	}
	// TWO KEYS AN OCTAVE APART: the terrain moves them alike, so the ratio is exact.
	const double lowHz = measuredHz(renderKey(pitchedRow(), 48), keyHz(48) * 0.7, keyHz(48) * 1.4);
	const double highHz = measuredHz(renderKey(pitchedRow(), 60), keyHz(60) * 0.7, keyHz(60) * 1.4);
	CHECK(highHz / lowHz == doctest::Approx(2.0).epsilon(0.004));
	// TUNE (+7 semitones) ON THE SAME KEY: the ratio is the fifth, 2^(7/12).
	dd::Row tuned = pitchedRow();
	tuned[at(Param::Tune)] = 7.0f;
	const double fifth = measuredHz(renderKey(tuned, 48), keyHz(48) * 0.7, keyHz(48, 7.0) * 1.4);
	CHECK(fifth / lowHz == doctest::Approx(std::exp2(7.0 / 12.0)).epsilon(0.004));
	// AND THE KEY'S TUNING EXPRESSION moves it as a bend moves a synth voice (the whole of it: Bend is the kit's alone).
	Dev d;
	d.row = pitchedRow();
	d.row[at(Param::Bend)] = 0.0f; // would silence the move in One Key; Pitched does not read it
	d.apply();
	(void)d.notes({ on(48, 0), tuning(48, 12.0) });
	Out o(16384);
	d.render(o.l.data(), o.r.data(), 16384);
	// THE TUNING ARRIVES AFTER THE STRIKE and is read when the laws refresh: the ring is an octave up from the start of the next tick.
	const double bent = measuredHz(o.l, keyHz(48) * 1.4, keyHz(60) * 1.4);
	CHECK(bent / lowHz == doctest::Approx(2.0).epsilon(0.006));
}

TEST_CASE("drum Pitched: One Key is untouched, bit for bit, by the Keys control being there") {
	for (const drum::Kind kind : { drum::Kind::Kick, drum::Kind::Snare, drum::Kind::Clap, drum::Kind::String }) {
		CAPTURE(static_cast<int>(kind));
		const dd::Row want = variedRow(kind); // the row as it was before Keys existed (Keys at its default, One Key)
		REQUIRE(want[at(Param::Keys)] == 0.0f);
		// A PATCH FROM BEFORE KEYS carries 54 fields: the new one is its default, One Key, and the sound is the independent restatement's.
		Dev a;
		a.row = want;
		a.apply();
		(void)a.notes({ on(36, 0, 0.8) });
		Out oa(2048);
		a.render(oa.l.data(), oa.r.data(), 2048);
		const Out ref = refRender(want, 0.8, 2048);
		CHECK(oa.l == ref.l);
		CHECK(oa.r == ref.r);
		const dd::Settings old = dd::stateOf(want, static_cast<size_t>(Param::Keys));
		CHECK_FALSE(old.pitched());
		CHECK(old.index(Param::Keys) == 0);
	}
}

TEST_CASE("drum Pitched: the same keys in any block sizes are the same sound, to the bit") {
	constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 8 };
	constexpr int kTotal = 1500;
	// KEYS THE DRUM IS NOT ASKED FOR BY NOTE: a chord, a re-strike on one of its keys, a release (Gate), a choke.
	const std::vector<std::pair<int, NoteEvent>> script = { { 50, on(48, 0, 0.9) }, { 51, on(55, 0, 0.7) }, { 333, on(48, 0, 0.5) },
		                                                    { 700, off(55, 0) },    { 1000, on(60, 0, 1.0) }, { 1200, choke(48, 0) } };
	const auto render = [&](const std::vector<int>& blocks) {
		rack::RackEngine engine(kAxes, hostCatalogue());
		engine.prepare(kSr, 4096);
		const size_t slot = engine.addDevice(dd::kTraits.tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		engine.prepare(kSr, 4096);
		const dd::Row row = pitchedRow();
		for (size_t i = 0; i < row.size(); ++i) {
			engine.setField(slot, i, row[i]);
		}
		engine.setField(slot, at(Param::Trigger), 1.0f);
		engine.setField(slot, at(Param::Voices), 1.0f);
		engine.setField(slot, at(Param::Release), static_cast<float>(std::log2(0.03)));
		Out out(kTotal);
		int start = 0;
		for (const int n : blocks) {
			std::vector<NoteEvent> here;
			for (const auto& [t, e] : script) {
				if (t >= start && t < start + n) {
					NoteEvent ev = e;
					ev.timeSamples = static_cast<uint32_t>(t - start);
					here.push_back(ev);
				}
			}
			engine.setBlockNotes(here.data(), here.size());
			engine.processStereo(out.l.data() + start, out.r.data() + start, n, backplane::unknownBlock());
			start += n;
		}
		REQUIRE(start == kTotal);
		return out;
	};
	const Out whole = render({ kTotal });
	CHECK_FALSE(allZero(whole.l));
	// NON-VACUITY: the strikes on the keys the Note does not name are what is in it (One Key would make none of this sound).
	const std::vector<std::vector<int>> cuts = {
		{ 500, 500, 500 }, { 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 28 },
		{ 37, 1, 100, 62, 300, 5, 995 }, { 1499, 1 }, { 333, 367, 300, 500 },
	};
	for (const auto& cut : cuts) {
		const Out got = render(cut);
		CHECK(got.l == whole.l);
		CHECK(got.r == whole.r);
	}
}

TEST_CASE("drum Pitched: Voices is per key, the drum holds four across keys, and the pool never allocates") {
	Dev d;
	d.row = pitchedRow();
	d.row[at(Param::Voices)] = 1.0f; // two per key
	d.apply();
	Out o(256);
	// THREE STRIKES ON ONE KEY: the third fades the first (two live, one fading).
	(void)d.notes({ on(48, 0), on(48, 0), on(48, 0) });
	d.render(o.l.data(), o.r.data(), 64);
	CHECK(d.dsp().soundingForTest() == 2);
	CHECK(d.dsp().fadingForTest() == 1);
	// A SECOND KEY IS ITS OWN COUNT: two more live on it, and the drum is at its four.
	(void)d.notes({ on(55, 0), on(55, 0) });
	d.render(o.l.data(), o.r.data(), 64);
	CHECK(d.dsp().soundingForTest() == 4);
	// A FIFTH ACROSS KEYS fades the oldest, so the drum never rings more than four.
	(void)d.notes({ on(60, 0) });
	d.render(o.l.data(), o.r.data(), 64);
	CHECK(d.dsp().soundingForTest() == 4);
	{
		(void)d.notes({ on(62, 0), on(64, 0), on(65, 0) }); // the vector this makes is the test's own
		AllocWindow w;
		d.render(o.l.data(), o.r.data(), 64);
		CHECK(w.allocs() == 0);
	}
}

TEST_CASE("drum Pitched: a patch saved and loaded into a fresh rack keeps Keys, and a patch from before it is One Key") {
	persist::Rig a;
	a.seat(dd::kTraits.tag);
	const dd::Row want = pitchedRow(41);
	for (size_t i = 0; i < want.size(); ++i) {
		a.bridge->setField(a.id, i, want[i]);
	}
	a.block(2);
	REQUIRE(a.as<dd::DrumDsp>().settingsForTest().pitched());
	const std::string text = a.save();
	persist::Rig b;
	b.load(text);
	b.block(2);
	CHECK(b.as<dd::DrumDsp>().settingsForTest().pitched());
	CHECK(b.as<dd::DrumDsp>().settingsForTest() == a.as<dd::DrumDsp>().settingsForTest());
	CHECK(b.as<dd::DrumDsp>().settingsForTest().index(Param::Note) == 41);
	// THE SAVED TEXT NAMES THE CONTROL BY ITS KEY (a patch is by key, so its order is free).
	CHECK(text.find("drum_keys") != std::string::npos);
	// A PATCH FROM BEFORE KEYS has no such key: it loads as One Key, the control's default.
	std::string old = text;
	for (size_t at = old.find("drum_keys"); at != std::string::npos; at = old.find("drum_keys", at)) {
		old.replace(at, 9, "drum_zzzz");
		at += 9;
	}
	persist::Rig c;
	c.load(old);
	c.block(2);
	CHECK_FALSE(c.as<dd::DrumDsp>().settingsForTest().pitched());
}

TEST_CASE("drum Any Key: every key is answered and sounds exactly as One Key does on its Note, bit for bit") {
	for (const drum::Kind kind : { drum::Kind::Kick, drum::Kind::Snare, drum::Kind::Clap, drum::Kind::Metal }) {
		CAPTURE(static_cast<int>(kind));
		dd::Row one = variedRow(kind);
		one[at(Param::Note)] = 36.0f;
		const auto strike = [&](const dd::Row& row, int key) {
			Dev d;
			d.row = row;
			d.apply();
			(void)d.notes({ on(key, 0, 0.8) });
			Out o(2048);
			d.render(o.l.data(), o.r.data(), 2048);
			return o;
		};
		const Out reference = strike(one, 36);
		REQUIRE_FALSE(allZero(reference.l));
		dd::Row any = one;
		any[at(Param::Keys)] = 1.0f;
		for (const int key : { 20, 36, 52, 99 }) {
			CAPTURE(key);
			const Out got = strike(any, key);
			CHECK(got.l == reference.l);
			CHECK(got.r == reference.r);
		}
		// NON-VACUITY: One Key does not answer those keys at all.
		CHECK(allZero(strike(one, 52).l));
	}
}

TEST_CASE("drum Any Key: the same keys in any block sizes are the same sound, to the bit") {
	constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 8 };
	constexpr int kTotal = 1500;
	const std::vector<std::pair<int, NoteEvent>> script = { { 50, on(48, 0, 0.9) }, { 333, on(55, 0, 0.5) }, { 700, off(48, 0) },
		                                                    { 1000, on(60, 0, 1.0) }, { 1200, choke(55, 0) } };
	const auto render = [&](const std::vector<int>& blocks) {
		rack::RackEngine engine(kAxes, hostCatalogue());
		engine.prepare(kSr, 4096);
		const size_t slot = engine.addDevice(dd::kTraits.tag);
		REQUIRE(slot != rack::RackEngine::kNoSlot);
		engine.prepare(kSr, 4096);
		dd::Row row = pitchedRow();
		row[at(Param::Keys)] = 1.0f; // Any Key
		for (size_t i = 0; i < row.size(); ++i) {
			engine.setField(slot, i, row[i]);
		}
		engine.setField(slot, at(Param::Trigger), 1.0f);
		engine.setField(slot, at(Param::Release), static_cast<float>(std::log2(0.03)));
		Out out(kTotal);
		int start = 0;
		for (const int n : blocks) {
			std::vector<NoteEvent> here;
			for (const auto& [t, e] : script) {
				if (t >= start && t < start + n) {
					NoteEvent ev = e;
					ev.timeSamples = static_cast<uint32_t>(t - start);
					here.push_back(ev);
				}
			}
			engine.setBlockNotes(here.data(), here.size());
			engine.processStereo(out.l.data() + start, out.r.data() + start, n, backplane::unknownBlock());
			start += n;
		}
		REQUIRE(start == kTotal);
		return out;
	};
	const Out whole = render({ kTotal });
	CHECK_FALSE(allZero(whole.l));
	for (const auto& cut : std::vector<std::vector<int>> { { 500, 500, 500 }, { 37, 1, 100, 62, 300, 5, 995 }, { 1499, 1 }, { 333, 367, 300, 500 } }) {
		const Out got = render(cut);
		CHECK(got.l == whole.l);
		CHECK(got.r == whole.r);
	}
}

TEST_CASE("drum Any Key: a patch saved and loaded into a fresh rack keeps it") {
	persist::Rig a;
	a.seat(dd::kTraits.tag);
	dd::Row want = defaultRow();
	want[at(Param::Keys)] = 1.0f;
	for (size_t i = 0; i < want.size(); ++i) {
		a.bridge->setField(a.id, i, want[i]);
	}
	a.block(2);
	REQUIRE(a.as<dd::DrumDsp>().settingsForTest().index(Param::Keys) == 1);
	const std::string text = a.save();
	persist::Rig b;
	b.load(text);
	b.block(2);
	CHECK(b.as<dd::DrumDsp>().settingsForTest().index(Param::Keys) == 1);
	CHECK_FALSE(b.as<dd::DrumDsp>().settingsForTest().pitched());
	CHECK(b.as<dd::DrumDsp>().settingsForTest() == a.as<dd::DrumDsp>().settingsForTest());
}

//--------------------------------------------------------------------------------------------------//
// THE VELOCITY ROW: depths, not a mode                                                              //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("drum: the velocity row's defaults are the old default zones' sound, at every velocity and kind") {
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		for (const double vel : { 0.01, 0.5, 1.0 }) {
			CAPTURE(k);
			CAPTURE(vel);
			dd::Row r = defaultRow();
			r[at(Param::Kind)] = static_cast<float>(k);
			drum::Controls direct = refControls(r);
			drum::Controls zones = direct;
			zones.direct = false; // the zone road at its own defaults (velocity Tension, wires Tight, bells Off)
			zones.velocityZone = 2.0 / 3.0;
			zones.wires = 0.0;
			zones.bells = 0.0;
			const drum::Laws a = drum::lawsFor(kindOf(r), refFields(r), direct, refBaseHz(r), vel);
			const drum::Laws b = drum::lawsFor(kindOf(r), refFields(r), zones, refBaseHz(r), vel);
			CHECK(a.tone == b.tone);
			CHECK(a.snap == b.snap);
			CHECK(a.sweepOctaves == b.sweepOctaves);
			CHECK(a.noise == b.noise);
		}
	}
}

TEST_CASE("drum: each velocity depth reaches its destination by the stated law at depth 0, 50 and 100 percent, velocity 1, 64 and 127") {
	const double vels[] = { 1.0 / 127.0, 64.0 / 127.0, 1.0 };
	for (const double depth : { 0.0, 0.5, 1.0 }) {
		for (const double vel : vels) {
			CAPTURE(depth);
			CAPTURE(vel);
			dd::Row r = variedRow(drum::Kind::Membrane);
			r[at(Param::VelTone)] = static_cast<float>(depth);
			r[at(Param::VelSnap)] = static_cast<float>(-depth); // its own sign, so a swap with Tone's depth is seen
			r[at(Param::VelTension)] = static_cast<float>(depth);
			dd::Row flat = r;
			flat[at(Param::VelTone)] = 0.0f;
			flat[at(Param::VelSnap)] = 0.0f;
			flat[at(Param::VelTension)] = 0.0f;
			const drum::Fields f = refFields(r);
			const drum::Laws got = refLaws(r, vel);
			const drum::Laws base = refLaws(flat, vel);
			// RESTATED: tone and snap move by depth x (velocity - 0.5), clamped to 0..1, from the field's own value.
			const double shift = depth * (vel - 0.5);
			CHECK(got.tone == doctest::Approx(std::clamp(static_cast<double>(f.tone) + shift, 0.0, 1.0)).epsilon(1e-6));
			CHECK(got.snap == doctest::Approx(std::clamp(static_cast<double>(f.snap) - shift, 0.0, 1.0)).epsilon(1e-6));
			// TENSION: the glide's depth is scaled by 1 + depth x (v^2 - 1); at depth 0 it is the flat glide.
			CHECK(got.sweepOctaves == doctest::Approx(base.sweepOctaves * (1.0 + depth * (vel * vel - 1.0))).epsilon(1e-9));
			// THE DEVICE PLAYS THE RESTATED LAWS: its render is the restated hit's, to the bit.
			Dev d;
			d.row = r;
			CHECK(run(d, { on(36, 0, vel) }, 512).l == refRender(r, vel, 512).l);
		}
	}
}

TEST_CASE("drum: the Level depth scales the strike by 1 - depth x (1 - velocity), and the device renders it") {
	for (const double depth : { 0.0, 0.5, 1.0 }) {
		for (const double vel : { 1.0 / 127.0, 64.0 / 127.0, 1.0 }) {
			CAPTURE(depth);
			CAPTURE(vel);
			Dev d;
			d.row = variedRow(drum::Kind::Snare);
			d.set(Param::VelLevel, depth);
			const Out got = run(d, { on(36, 0, vel) }, 1024);
			CHECK(got.l == refRender(d.row, vel, 1024).l);
			if (depth == 0.0) { // every hit full: velocity moves nothing but the laws, and the level is unity
				Dev u;
				u.row = d.row;
				const Out full = run(u, { on(36, 0, 1.0) }, 1024);
				Dev w;
				w.row = d.row;
				w.set(Param::VelTone, 0.0);
				w.set(Param::VelSnap, 0.0);
				w.set(Param::VelTension, 0.0);
				w.set(Param::VelocityNoise, 0.0);
				Dev x;
				x.row = w.row;
				CHECK(run(w, { on(36, 0, vel) }, 1024).l == run(x, { on(36, 0, 1.0) }, 1024).l);
				(void)full;
			}
		}
	}
	// NON-VACUITY: a soft hit at full depth is quieter than at none.
	Dev a, b;
	a.row = variedRow(drum::Kind::Snare);
	b.row = a.row;
	a.set(Param::VelLevel, 1.0);
	b.set(Param::VelLevel, 0.0);
	const Out qa = run(a, { on(36, 0, 0.3) }, 1024);
	const Out qb = run(b, { on(36, 0, 0.3) }, 1024);
	double ea = 0.0, eb = 0.0;
	for (size_t i = 0; i < qa.l.size(); ++i) {
		ea += static_cast<double>(qa.l[i]) * qa.l[i];
		eb += static_cast<double>(qb.l[i]) * qb.l[i];
	}
	CHECK(ea < eb);
}

TEST_CASE("drum: a scaler stops at the top of its range instead of wrapping into another mode") {
	// Past the last zone the coordinate used to take its fractional part and land back on the first. The zone road now holds at the ends.
	for (const double top : { 1.0, 1.5, 3.0, 8.0 }) {
		CAPTURE(top);
		const drum::WiresMix last = drum::wiresAt(3.0 / 4.0);
		const drum::WiresMix over = drum::wiresAt(top);
		CHECK(over.contour == last.contour);
		CHECK(over.wireShape == last.wireShape);
		CHECK(over.buzz == last.buzz);
		const drum::BellsMix lastB = drum::bellsAt(4.0 / 5.0);
		const drum::BellsMix overB = drum::bellsAt(top);
		CHECK(overB.level1 == lastB.level1);
		CHECK(overB.level2 == lastB.level2);
		const drum::VelocityMix lastV = drum::velocityAt(2.0 / 3.0);
		const drum::VelocityMix overV = drum::velocityAt(top);
		CHECK(overV.tone == lastV.tone);
		CHECK(overV.tension == lastV.tension);
	}
	// AND THE DEVICE'S OWN SCALERS ARE CLAMPED: a value past the top reads as the top.
	Dev a, b;
	a.row = variedRow(drum::Kind::Snare);
	b.row = a.row;
	a.set(Param::WireBuzz, 1.0);
	b.row[at(Param::WireBuzz)] = 1.0f;
	CHECK(run(a, { on(36, 0) }, 512).l == run(b, { on(36, 0) }, 512).l);
	// NON-VACUITY: the first and the last zone differ.
	CHECK(drum::wiresAt(0.0).buzz != drum::wiresAt(3.0 / 4.0).buzz);
}

//--------------------------------------------------------------------------------------------------//
// PLAY MODE: One-shot | Gate | Drone, and the Particle kind                                          //
//--------------------------------------------------------------------------------------------------//

namespace {

dd::Row particleRow(double trigger) {
	dd::Row r = defaultRow();
	r[at(Param::Kind)] = static_cast<float>(drum::Kind::Particle);
	r[at(Param::Trigger)] = static_cast<float>(trigger);
	r[at(Param::Seed)] = 23.0f;
	r[at(Param::Decay)] = -1.6f;
	r[at(Param::Release)] = static_cast<float>(std::log2(0.05));
	return r;
}

float peakOfOut(const Out& o) {
	float p = 0.0f;
	for (const float x : o.l) {
		p = std::max(p, std::fabs(x));
	}
	return p;
}

} // namespace

TEST_CASE("drone: with Trigger on Drone a Particle sounds with no key, stays awake for ever, answers no key and takes no voice") {
	Dev d;
	d.row = particleRow(2.0);
	REQUIRE(d.holds() == false); // nothing yet: the drone starts with the first block the device runs
	float quietest = 1.0f;
	for (int block = 0; block < 400; ++block) { // about 3 s of 512-frame blocks, far past any hit's own length
		const Out o = run(d, {}, 512);
		if (block >= 8) {
			quietest = std::min(quietest, peakOfOut(o) > 0.0f ? 1.0f : 0.0f);
		}
		CHECK(d.holds()); // sustains by design: never asleep, no timeout
	}
	CHECK(quietest == 1.0f); // and a block of exact silence never came in the steady state (a density of 25 a second against 512-frame blocks)
	REQUIRE(d.dsp().droneForTest() != nullptr);
	CHECK(d.dsp().soundingForTest() == 0); // outside the pool
	// A KEY CHANGES NOTHING: the notes are not struck (the drone has no key), and the strike count stays where it was.
	const uint32_t strikes = d.dsp().strikesForTest();
	(void)run(d, { on(36, 0), off(36, 100) }, 512);
	CHECK(d.dsp().strikesForTest() == strikes);
	CHECK(d.dsp().soundingForTest() == 0);
}

TEST_CASE("drone: a moved control follows without a restart, a new Seed restarts it, and leaving Drone fades it to exact zero") {
	Dev d;
	d.row = particleRow(2.0);
	(void)run(d, {}, 4096);
	const drum::Hit* drone = d.dsp().droneForTest();
	REQUIRE(drone != nullptr);
	const int64_t before = drone->age();
	// A MOVED CONTROL (Tune, Decay, a field) is the laws' business: the drone's age runs on and its stream is the same stream.
	d.set(Param::Tune, 5.0);
	d.set(Param::Decay, -2.5);
	(void)run(d, {}, 2048);
	REQUIRE(d.dsp().droneForTest() != nullptr);
	CHECK(d.dsp().droneForTest()->age() == before + 2048);
	CHECK(d.dsp().droneForTest()->laws().hz > drone->laws().hz * 0.0); // (the pointer is the same hit: it follows)
	const uint64_t seedBefore = d.dsp().droneForTest()->seed();
	// A CHANGED SEED RESTARTS IT: the age is small again, and the stream is the new seed's.
	d.set(Param::Seed, 24.0);
	(void)run(d, {}, 512);
	REQUIRE(d.dsp().droneForTest() != nullptr);
	CHECK(d.dsp().droneForTest()->age() == 512);
	CHECK(d.dsp().droneForTest()->seed() != seedBefore);
	// LEAVING THE MODE fades it out: silent to the exact sample, and the device may sleep.
	d.set(Param::Trigger, 0.0);
	(void)run(d, {}, 4096);
	CHECK(d.dsp().droneForTest() == nullptr);
	const Out after = run(d, {}, 512);
	CHECK(peakOfOut(after) == 0.0f);
	CHECK_FALSE(d.holds());
}

TEST_CASE("drone: the same row from the same start is the same sound, to the bit, in a fresh instance (the seed is the whole of its state)") {
	Dev a;
	Dev b;
	a.row = b.row = particleRow(2.0);
	a.row[at(Param::Seed)] = b.row[at(Param::Seed)] = 777.0f;
	std::vector<float> xa;
	std::vector<float> xb;
	for (int block = 0; block < 40; ++block) {
		const Out oa = run(a, {}, 256);
		const Out ob = run(b, {}, 256);
		xa.insert(xa.end(), oa.l.begin(), oa.l.end());
		xb.insert(xb.end(), ob.l.begin(), ob.l.end());
	}
	CHECK(xa == xb);
	// NON-VACUITY: another seed is another shower.
	Dev c;
	c.row = a.row;
	c.row[at(Param::Seed)] = 778.0f;
	std::vector<float> xc;
	for (int block = 0; block < 40; ++block) {
		const Out oc = run(c, {}, 256);
		xc.insert(xc.end(), oc.l.begin(), oc.l.end());
	}
	CHECK(xa != xc);
	// AND A CHAIN RESET (the engine's park, a transport restart) puts it back at its start: the same bits again.
	dd::kAudioOps.reset(a.st);
	std::vector<float> xr;
	for (int block = 0; block < 40; ++block) {
		const Out o = run(a, {}, 256);
		xr.insert(xr.end(), o.l.begin(), o.l.end());
	}
	CHECK(xr == xa);
}

TEST_CASE("play mode: Drone on a kind that cannot hold one plays as One-shot (no drone, notes strike), and Gate on every kind damps to Release as it did") {
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		const auto kind = static_cast<drum::Kind>(k);
		CAPTURE(k);
		Dev d;
		d.row = particleRow(2.0);
		d.row[at(Param::Kind)] = static_cast<float>(k);
		if (drum::kindSustains(kind)) {
			(void)run(d, {}, 512);
			CHECK(d.dsp().droneForTest() != nullptr);
			continue;
		}
		const Out idle = run(d, {}, 512);
		CHECK(peakOfOut(idle) == 0.0f); // no key, no sound: there is no drone here
		CHECK(d.dsp().droneForTest() == nullptr);
		const Out hit = run(d, { on(36, 0) }, 512);
		CHECK(peakOfOut(hit) > 0.0f); // and a note still strikes it
	}
}

TEST_CASE("particle: One-shot and Gate reach exact zero and let the device sleep, and a held Gate does not until its key goes") {
	for (const double trig : { 0.0, 1.0 }) {
		CAPTURE(trig);
		Dev d;
		d.row = particleRow(trig);
		(void)run(d, { on(36, 0) }, 512);
		if (trig == 1.0) {
			for (int block = 0; block < 100; ++block) { // held: still sounding after far longer than Decay
				(void)run(d, {}, 512);
				CHECK(d.holds());
			}
			(void)run(d, { off(36, 0) }, 512);
		}
		bool slept = false;
		for (int block = 0; block < 1200 && !slept; ++block) { // about 12 s
			(void)run(d, {}, 512);
			slept = !d.holds();
		}
		CHECK(slept);
		const Out after = run(d, {}, 512);
		CHECK(peakOfOut(after) == 0.0f);
	}
}

TEST_CASE("drone: every kind that can hold a hit sustains in Drone, stays awake, and leaving it fades to exact zero") {
	size_t held = 0;
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		const auto kind = static_cast<drum::Kind>(k);
		if (!drum::kindSustains(kind)) {
			continue;
		}
		++held;
		CAPTURE(k);
		Dev d;
		d.row = particleRow(2.0);
		d.row[at(Param::Kind)] = static_cast<float>(k);
		int sounding = 0;
		for (int block = 0; block < 300; ++block) {
			const Out o = run(d, {}, 512);
			sounding += peakOfOut(o) > 0.0f ? 1 : 0;
			CHECK(d.holds());
		}
		CHECK(sounding > 250); // a drone is not a hit: nearly every block carries sound
		d.set(Param::Trigger, 0.0);
		(void)run(d, {}, 4096);
		CHECK(d.dsp().droneForTest() == nullptr);
		CHECK(peakOfOut(run(d, {}, 512)) == 0.0f);
		CHECK_FALSE(d.holds());
	}
	CHECK(held >= 2); // Particle and Friction (and the kinds that follow)
}
