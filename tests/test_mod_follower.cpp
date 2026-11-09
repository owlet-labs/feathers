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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "fx/busutil/busutil_types.h"
#include "fx/filter/filter_types.h"
#include "fx/inject/inject_types.h"
#include "mod/follower/follower.h"
#include "mod/follower/follower_core.h"
#include "mod/follower/follower_types.h"
#include "mod/trig.h"
#include "plugin/device_registry.h"
#include "plugin/feathers_chains.h"
#include "plugin/mod_rack.h"
#include "rack/device_browser.h"
#include "rack/chain_record.h"
#include "rack/rack_state.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

// THE FOLLOWER (backlog MY) — every claim asserted as a VALUE, the reference restated from the words: a one-pole
// covers 1 − 1/e of a step in one time constant; Peak reads a sine's amplitude and RMS its amplitude over √2;
// out = clamp((gain · level − floor) / (1 − floor), 0, 1); Trig fires on the tick after the output reaches the
// threshold and re-arms only below half of it. Signed (backlog OP) is a one-pole of the signal itself, Attack while
// rising and Release while falling; Mode decides what the outputs may reach and a flip deletes what it forbids.
// Tolerances are ABSOLUTE (doctest's Approx adds a scale of 1).

//------------------------------------------------------------------------------------------------//
// THE ALLOCATION PROBE — `test_fx_eq.cpp`'s, counting only while armed.                           //
//------------------------------------------------------------------------------------------------//

namespace {

struct Allocs {
	bool armed = false;
	size_t allocations = 0;
};
Allocs allocs;

void* countedAlloc(size_t bytes, size_t align) {
	if (allocs.armed) {
		++allocs.allocations;
	}
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}

} // namespace

void* operator new(size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new[](size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new(size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void* operator new[](size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }

using namespace feathers;
namespace ff = feathers::mod::follower;

namespace {

constexpr double kRate = 48000.0;
constexpr size_t kTick = static_cast<size_t>(mod::kBlockSamples);

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 8,
};

/// A CORE WITH THE GIVEN SETTINGS, prepared at 48 kHz.
ff::Core coreWith(float attack, float release, ff::Detector detector, float gain = 1.0f, float floor = 0.0f,
                  float threshold = 0.5f) {
	ff::Core core;
	core.prepare(kRate);
	core.configure(attack, release, detector, gain, floor, threshold);
	return core;
}

std::vector<float> defaults() {
	std::vector<float> row;
	for (const device::ParamDesc& p : ff::kDescriptor.params) {
		row.push_back(static_cast<float>(p.def));
	}
	return row;
}

float& field(std::vector<float>& row, ff::Param p) { return row[static_cast<size_t>(p)]; }

/// THE DEVICE THROUGH ITS OPS, the way a host reaches it.
struct Ops {
	void* mem = nullptr;
	Ops() {
		const device::ModOps& mod = ff::kModOps;
		const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, kRate);
	}
	~Ops() {
		ff::kModOps.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Ops(const Ops&) = delete;
	Ops& operator=(const Ops&) = delete;

	void set(const std::vector<float>& row) { ff::kModOps.setFields(mem, row.data(), row.size()); }
	void tick() {
		device::ModTickContext ctx;
		ff::kModOps.tick(mem, ctx);
	}
	[[nodiscard]] float out(ff::Output o) const { return ff::kModOps.outputAt(mem, static_cast<size_t>(o)); }
};

/// AN INPUT-SOURCE ROW with no smoothing at all and the given threshold — the decoder at its plainest. Peak unless
/// asked: the rectifying detector, whose claims these rows were written for.
std::vector<float> inputRow(float input, float threshold = 0.5f, ff::Detector detector = ff::Detector::Peak) {
	std::vector<float> row = defaults();
	field(row, ff::Param::Detector) = static_cast<float>(detector);
	field(row, ff::Param::Input) = input;
	field(row, ff::Param::Attack) = static_cast<float>(ff::kAttackLog2Min);
	field(row, ff::Param::Release) = static_cast<float>(ff::kReleaseLog2Min);
	field(row, ff::Param::Threshold) = threshold;
	return row;
}

} // namespace

TEST_CASE("Follower: one Attack and one Release time constant cover 1 - 1/e of a step, at the sample") {
	// ATTACK 10 ms, RELEASE 100 ms. After 480 samples of a unit step the level is 1 − e^(−1); after 4800 more of
	// silence it is that times e^(−1). The discrete one-pole's coefficient is 1 − exp(−1/(T·fs)), so N = T·fs steps
	// give exactly exp(−1) of the distance left: the reference is the closed form, not the follower run again.
	ff::Core core = coreWith(0.010f, 0.100f, ff::Detector::Peak);
	for (int i = 0; i < 480; ++i) {
		core.push(1.0f, 1.0f);
	}
	const double rise = 1.0 - std::exp(-1.0);
	CHECK(std::abs(core.level() - rise) <= 1e-4);
	// NOT VACUOUS: one sample earlier is measurably short of it.
	ff::Core early = coreWith(0.010f, 0.100f, ff::Detector::Peak);
	for (int i = 0; i < 240; ++i) {
		early.push(1.0f, 1.0f);
	}
	CHECK(std::abs(early.level() - (1.0 - std::exp(-0.5))) <= 1e-4);
	for (int i = 0; i < 4800; ++i) {
		core.push(0.0f, 0.0f);
	}
	CHECK(std::abs(core.level() - rise * std::exp(-1.0)) <= 1e-4);
	// AND THE TIMES ARE THE CONTROLS' — a stored log2(seconds) decodes to seconds, the bottom to "no smoothing".
	CHECK(std::abs(ff::secondsOf(static_cast<float>(std::log2(0.010)), ff::kAttackLog2Min) - 0.010f) <= 1e-6f);
	CHECK(ff::secondsOf(static_cast<float>(ff::kAttackLog2Min), ff::kAttackLog2Min) == 0.0f);
	ff::Core instant = coreWith(0.0f, 0.0f, ff::Detector::Peak);
	instant.push(0.37f, -0.2f);
	CHECK(instant.level() == 0.37f);
}

TEST_CASE("Follower: Peak reads a sine's amplitude and RMS reads its amplitude over root two") {
	// A 1 kHz sine at 0.8, ten seconds. Peak with an instant attack and a 2 s release holds the crests (the ripple
	// between crests is at most 1 − exp(−(1/1000)/2) ≈ 0.05%). RMS with 0.5 s on both sides settles on the mean
	// power 0.32 and reads √0.32 = 0.8/√2.
	const auto run = [](ff::Detector d, float attack, float release) {
		ff::Core core = coreWith(attack, release, d);
		const double w = 2.0 * 3.14159265358979323846 * 1000.0 / kRate;
		for (int i = 0; i < 480000; ++i) {
			const auto x = static_cast<float>(0.8 * std::sin(w * i));
			core.push(x, x);
		}
		return core.level();
	};
	const float peak = run(ff::Detector::Peak, 0.0f, 2.0f);
	const float rms = run(ff::Detector::Rms, 0.5f, 0.5f);
	CHECK(std::abs(peak - 0.8) <= 0.8 * 0.002);
	CHECK(std::abs(rms - 0.8 / std::sqrt(2.0)) <= 0.8 / std::sqrt(2.0) * 0.01);
	// THE TWO DETECTORS DISAGREE BY √2 — the difference the switch exists for.
	CHECK(std::abs(peak / rms - std::sqrt(2.0)) <= 0.02);
	// AND RMS OF A STEREO PAIR IS THE MEAN POWER OF THE EARS: (0.6² + 0.8²) / 2 = 0.5, √0.5.
	ff::Core pair = coreWith(0.0f, 0.0f, ff::Detector::Rms);
	pair.push(0.6f, -0.8f);
	CHECK(std::abs(pair.level() - std::sqrt(0.5)) <= 1e-6);
	ff::Core louder = coreWith(0.0f, 0.0f, ff::Detector::Peak);
	louder.push(0.6f, -0.8f);
	CHECK(std::abs(louder.level() - 0.8) <= 1e-6); // Peak: the louder ear's magnitude
}

TEST_CASE("Follower: Gain scales the level and Floor is where the output leaves zero") {
	// out = clamp((g · level − f) / (1 − f), 0, 1), restated per line.
	const auto out = [](float level, float gainDb, float floorDb) {
		ff::Core core = coreWith(0.0f, 0.0f, ff::Detector::Peak, ff::gainOf(gainDb), ff::floorOf(floorDb));
		core.push(level, level);
		return core.envelope();
	};
	CHECK(std::abs(out(0.25f, 0.0f, static_cast<float>(ff::kFloorDbMin)) - 0.25) <= 1e-6);
	CHECK(std::abs(out(0.25f, 6.0206f, static_cast<float>(ff::kFloorDbMin)) - 0.5) <= 1e-4);   // +6 dB doubles
	CHECK(std::abs(out(0.25f, -6.0206f, static_cast<float>(ff::kFloorDbMin)) - 0.125) <= 1e-4); // −6 dB halves
	CHECK(out(0.9f, 12.0f, static_cast<float>(ff::kFloorDbMin)) == 1.0f);                       // clamped at 1
	const double f = std::pow(10.0, -12.0 / 20.0); // −12 dB ≈ 0.2512
	CHECK(out(0.25f, 0.0f, -12.0f) == 0.0f);       // under the floor: rest
	CHECK(std::abs(out(0.5f, 0.0f, -12.0f) - (0.5 - f) / (1.0 - f)) <= 1e-5);
	CHECK(std::abs(out(1.0f, 0.0f, -12.0f) - 1.0) <= 1e-6); // full level still reaches 1 over the floor
	// THE BOTTOM OF THE FLOOR'S TRAVEL IS NO FLOOR: a −80 dB level passes untouched.
	CHECK(ff::floorOf(static_cast<float>(ff::kFloorDbMin)) == 0.0f);
	CHECK(std::abs(out(1.0e-4f, 0.0f, static_cast<float>(ff::kFloorDbMin)) - 1.0e-4) <= 1e-9);
}

TEST_CASE("Follower: Trig fires on the tick after the crossing and re-arms only below half the threshold (Peak rectifies)") {
	// THE DECODER AT ITS PLAINEST — no smoothing, threshold 0.5 — ticked one value at a time. The
	// expected pulse train is written out from the rule: fire at ≥ 0.5 while armed, re-arm below 0.25.
	Ops ops;
	struct Step {
		float input;
		float trig;
	};
	const Step steps[] = {
		{ 0.40f, 0.0f }, // under the line
		{ 0.50f, 1.0f }, // reaches it: the pulse, on THIS tick
		{ 0.50f, 0.0f }, // held above: one tick only
		{ 0.30f, 0.0f }, // fell, but not through the hysteresis — not re-armed
		{ 0.60f, 0.0f }, // so rising again does NOT fire
		{ 0.20f, 0.0f }, // through 0.25: re-armed
		{ 0.60f, 1.0f }, // fires again
		{ -0.70f, 0.0f }, // a negative input is followed by its magnitude: still above, still held
		{ 0.0f, 0.0f },
		{ -0.70f, 1.0f }, // re-armed at 0, and |−0.7| fires
	};
	int tick = 0;
	for (const Step& s : steps) {
		ops.set(inputRow(s.input));
		ops.tick();
		INFO("tick " << tick << " input " << s.input);
		CHECK(ops.out(ff::Output::Trig) == s.trig);
		CHECK(std::abs(ops.out(ff::Output::Env) - std::abs(s.input)) <= 1e-6);
		++tick;
	}
	CHECK(ff::kModOps.trigFires(ops.mem) == 3u);
	// THE RE-ARM LEVEL IS THE CONVENTION'S RATIO (`mod/trig.h`: 0.25 of 0.5), not a constant of its own.
	CHECK(ff::kRearmRatio == mod::kTrigRearmLevel / mod::kTrigFireLevel);
}

TEST_CASE("Follower: a crossing inside a tick is caught per sample and pulses once at the next tick") {
	// A 10-SAMPLE BURST AT 0.9 INSIDE ONE TICK'S WORTH OF SILENCE, Release instant: the envelope is back at 0 by the tick,
	// but the fire was held and the tick publishes it once. (The core's own claim; both modes run it.)
	ff::Core core = coreWith(0.0f, 0.0f, ff::Detector::Peak);
	for (size_t i = 0; i < kTick; ++i) {
		const float v = i >= 20 && i < 30 ? 0.9f : 0.0f;
		core.push(v, v);
	}
	CHECK(core.tick());
	CHECK(core.envelope() == 0.0f);
	CHECK(core.pulse() == 1.0f);
	for (size_t i = 0; i < kTick; ++i) {
		core.push(0.0f, 0.0f);
	}
	CHECK_FALSE(core.tick());
	CHECK(core.pulse() == 0.0f);
}

TEST_CASE("Follower: a modulation source routed into Input is followed and decoded in the engine") {
	// THE CONTROL-DOMAIN ROAD END TO END: macro 0 into Input at 0.5 (Input spans −1..1, so the macro's value lands
	// on it exactly), no smoothing. One 64-frame block is one tick; the published Env is the macro's
	// value and Trig pulses the block the macro first reaches the threshold.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t lane = engine.addDevice(ff::kTraits.tag);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	engine.prepare(kRate, 4096);
	const std::vector<float> row = inputRow(0.0f, 0.6f);
	for (size_t i = 0; i < row.size(); ++i) {
		engine.setField(lane, i, row[i]);
	}
	engine.setRouteAmount(test::macro(0), test::controlOf(engine, lane, static_cast<size_t>(ff::Param::Input)), 0.5f);
	REQUIRE(engine.routeAmount(test::macro(0), test::controlOf(engine, lane, static_cast<size_t>(ff::Param::Input)))
	        == 0.5f);
	std::array<float, kTick> l {};
	std::array<float, kTick> r {};
	const auto block = [&](float macro) {
		engine.setMacro(0, macro);
		engine.processStereo(l.data(), r.data(), static_cast<int>(kTick), backplane::unknownBlock());
	};
	block(0.3f);
	CHECK(std::abs(engine.publishedOutputAt(lane, 0) - 0.3) <= 1e-6);
	CHECK(engine.publishedOutputAt(lane, 1) == 0.0f);
	block(0.7f);
	CHECK(std::abs(engine.publishedOutputAt(lane, 0) - 0.7) <= 1e-6);
	CHECK(engine.publishedOutputAt(lane, 1) == 1.0f); // crossed 0.6 this tick
	block(0.7f);
	CHECK(engine.publishedOutputAt(lane, 1) == 0.0f); // one tick
	// DISABLED, BOTH OUTPUTS REST — the host's publish rule.
	engine.setField(lane, static_cast<size_t>(ff::Param::Enabled), 0.0f);
	block(0.7f);
	CHECK(engine.publishedOutputAt(lane, 0) == 0.0f);
}

//------------------------------------------------------------------------------------------------//
// SIGNED — the control detector (backlog OP)                                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Follower: Signed lags a bipolar input unrectified, Attack while rising and Release while falling") {
	// ATTACK 10 ms, RELEASE 100 ms, from rest. The reference is the closed form of the discrete one-pole, segment by
	// segment: a fall to −1 over 480 samples (release: 480 / 4800 = 0.1 time constants), a rise to +1 over 480
	// (attack: one time constant), a fall to −0.5 over 4800 (release: one). Peak would read every one of these as a
	// magnitude; Signed reads the signal.
	ff::Core core = coreWith(0.010f, 0.100f, ff::Detector::Signed);
	for (int i = 0; i < 480; ++i) {
		core.push(-1.0f, -1.0f);
	}
	const double l0 = -(1.0 - std::exp(-0.1));
	CHECK(std::abs(core.level() - l0) <= 1e-4);
	CHECK(std::abs(core.envelope() - l0) <= 1e-4); // the ENV is bipolar: negative, not folded
	for (int i = 0; i < 480; ++i) {
		core.push(1.0f, 1.0f);
	}
	const double l1 = 1.0 + (l0 - 1.0) * std::exp(-1.0);
	CHECK(std::abs(core.level() - l1) <= 1e-4);
	for (int i = 0; i < 4800; ++i) {
		core.push(-0.5f, -0.5f);
	}
	const double l2 = -0.5 + (l1 + 0.5) * std::exp(-1.0);
	CHECK(std::abs(core.level() - l2) <= 1e-4);
	// THE TWO TIMES ARE NOT SWAPPED: a rise of 240 samples at the ATTACK covers 1 − e^(−0.5) of the way; at the
	// release it would cover 1 − e^(−0.05).
	ff::Core rise = coreWith(0.010f, 0.100f, ff::Detector::Signed);
	for (int i = 0; i < 240; ++i) {
		rise.push(0.8f, 0.8f);
	}
	CHECK(std::abs(rise.level() - 0.8 * (1.0 - std::exp(-0.5))) <= 1e-4);
	// AN LFO'S −0.6, instant: Signed reads −0.6, Peak 0.6; the ears are averaged, not maxed.
	ff::Core lfo = coreWith(0.0f, 0.0f, ff::Detector::Signed);
	lfo.push(-0.6f, -0.6f);
	CHECK(lfo.envelope() == -0.6f);
	ff::Core peak = coreWith(0.0f, 0.0f, ff::Detector::Peak);
	peak.push(-0.6f, -0.6f);
	CHECK(peak.envelope() == 0.6f);
	ff::Core pair = coreWith(0.0f, 0.0f, ff::Detector::Signed);
	pair.push(0.6f, -0.2f);
	CHECK(std::abs(pair.level() - 0.2f) <= 1e-6f);
}

TEST_CASE("Follower: under Signed Gain scales and clamps at ±1, Floor is not applied, and Threshold acts on the signed value") {
	const auto out = [](float level, float gainDb, float floorDb) {
		ff::Core core = coreWith(0.0f, 0.0f, ff::Detector::Signed, ff::gainOf(gainDb), ff::floorOf(floorDb));
		core.push(level, level);
		return core.envelope();
	};
	CHECK(std::abs(out(-0.25f, 6.0206f, static_cast<float>(ff::kFloorDbMin)) + 0.5) <= 1e-4); // +6 dB doubles, sign kept
	CHECK(out(-0.9f, 12.0f, static_cast<float>(ff::kFloorDbMin)) == -1.0f);                     // clamped at −1
	CHECK(out(0.9f, 12.0f, static_cast<float>(ff::kFloorDbMin)) == 1.0f);
	CHECK(std::abs(out(0.1f, 0.0f, -12.0f) - 0.1) <= 1e-6); // a −12 dB floor would rest this; Signed passes it
	// THE THRESHOLD ON THE SIGNED VALUE: −0.7 is far below a 50% line and never fires; +0.6 does.
	ff::Core core = coreWith(0.0f, 0.0f, ff::Detector::Signed);
	core.push(-0.7f, -0.7f);
	CHECK_FALSE(core.tick());
	core.push(0.6f, 0.6f);
	CHECK(core.tick());
}

TEST_CASE("Follower: a default row is Control and Signed, so a negative Input is followed as negative") {
	// The follower appeared to rectify its input. The default row follows its Input unrectified.
	const std::vector<float> row = defaults();
	CHECK(row[static_cast<size_t>(ff::Param::Mode)] == static_cast<float>(ff::Mode::Control));
	CHECK(row[static_cast<size_t>(ff::Param::Detector)] == static_cast<float>(ff::Detector::Signed));
	Ops ops;
	ops.set(inputRow(-0.6f, 0.5f, ff::Detector::Signed));
	ops.tick();
	CHECK(std::abs(ops.out(ff::Output::Env) + 0.6f) <= 1e-6f);
	CHECK(ops.out(ff::Output::Trig) == 0.0f);
	// AND THE CARD'S LEVEL IS THE DRAWN ONE: −0.6 drawn at 0.2 (zero at 0.5), since the telemetry carries no negative.
	CHECK(std::abs(ff::kModOps.playheadLevel(ops.mem) - 0.2f) <= 1e-6f);
	// AND ENV IS DECLARED BIPOLAR, BY THE DETECTOR'S SIGNED ENTRY — Peak and RMS publish 0..1.
	CHECK(ff::kOutputs[0].bipolar);
	CHECK(ff::kOutputs[0].bipolarControl == static_cast<int>(ff::Param::Detector));
	CHECK(ff::kOutputs[0].bipolarChoice == static_cast<int>(ff::Detector::Signed));
}

TEST_CASE("Follower: Signed under Audio runs as Peak, and Input is not followed in Audio") {
	ff::Settings s = ff::settingsOf(defaults().data(), defaults().size());
	CHECK(s.running() == ff::Detector::Signed);
	std::vector<float> row = inputRow(0.7f, 0.5f, ff::Detector::Signed);
	field(row, ff::Param::Mode) = static_cast<float>(ff::Mode::Audio);
	s = ff::settingsOf(row.data(), row.size());
	CHECK(s.running() == ff::Detector::Peak);
	Ops ops;
	ops.set(row);
	ops.tick();
	CHECK(ops.out(ff::Output::Env) == 0.0f); // Audio follows the audio road, and none reached it
}

//------------------------------------------------------------------------------------------------//
// THE MODE DECIDES WHAT IT MAY REACH (backlog OP)                                                 //
//------------------------------------------------------------------------------------------------//

TEST_CASE("Follower: in Control it reads no chain audio, so a route into a device BEFORE it is admitted; in Audio refused") {
	// THE GRAPH'S HALF OF THE MODE: Control's MOD instant precedes its AUDIO instant, so a route into an fx upstream
	// closes no cycle; Audio's follows it, so the same route would read its own input and is refused.
	const std::vector<float> row = defaults();
	CHECK_FALSE(ff::kModOps.readsAudio);
	CHECK_FALSE(device::modReadsAudio(ff::kDescriptor, row.data(), row.size()));
	std::vector<float> audio = row;
	field(audio, ff::Param::Mode) = static_cast<float>(ff::Mode::Audio);
	CHECK(device::modReadsAudio(ff::kDescriptor, audio.data(), audio.size()));

	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t filter = engine.addDevice(fx::filter::kTraits.tag);
	const size_t follower = engine.addDevice(ff::kTraits.tag);
	const size_t after = engine.addDevice(fx::filter::kTraits.tag);
	REQUIRE(after != rack::RackEngine::kNoSlot);
	engine.prepare(kRate, 4096);
	const auto cutoffOf = [&](size_t lane) {
		return test::controlOf(engine, lane, static_cast<size_t>(fx::filter::Param::Cutoff));
	};
	engine.setRouteAmount(test::outputOf(engine, follower, 0), cutoffOf(filter), 0.4f);
	CHECK(engine.routeAmount(test::outputOf(engine, follower, 0), cutoffOf(filter)) == 0.4f);
	// FLIPPED TO AUDIO, THE ENGINE'S OWN GRAPH DELETES IT — and refuses it anew, while one downstream is admitted.
	engine.setField(follower, static_cast<size_t>(ff::Param::Mode), static_cast<float>(ff::Mode::Audio));
	CHECK(engine.routeAmount(test::outputOf(engine, follower, 0), cutoffOf(filter)) == 0.0f);
	engine.setRouteAmount(test::outputOf(engine, follower, 0), cutoffOf(filter), 0.4f);
	CHECK(engine.routeAmount(test::outputOf(engine, follower, 0), cutoffOf(filter)) == 0.0f);
	engine.setRouteAmount(test::outputOf(engine, follower, 0), cutoffOf(after), 0.4f);
	CHECK(engine.routeAmount(test::outputOf(engine, follower, 0), cutoffOf(after)) == 0.4f);
}

TEST_CASE("Follower: a Mode flip deletes exactly the routes the new mode forbids, hands them over, and they are restorable") {
	// THE RECORD (the one every host edits through). Lane: a filter, follower A, follower B. A's Env reaches the filter
	// BEFORE it and B's Attack AFTER it.
	backplane::InstanceCounter ids;
	rack::ChainRecord record { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids, &backplane::permitAll };
	const DeviceInstanceId filter = record.insert(kLaneChain, 0, fx::filter::kTraits.tag);
	const DeviceInstanceId a = record.insert(kLaneChain, 1, ff::kTraits.tag);
	const DeviceInstanceId b = record.insert(kLaneChain, 2, ff::kTraits.tag);
	REQUIRE(b != kNoInstance);
	const auto mode = static_cast<size_t>(ff::Param::Mode);
	const auto attack = static_cast<size_t>(ff::Param::Attack);
	const backplane::Source envA = backplane::Source::output(a, 0);
	const backplane::Destination cutoff =
	    backplane::Destination::control(filter, static_cast<size_t>(fx::filter::Param::Cutoff));
	const backplane::Destination attackB = backplane::Destination::control(b, attack);
	REQUIRE(record.setRoute(envA, cutoff, 0.4f));
	REQUIRE(record.setRoute(envA, attackB, 0.3f));
	(void)record.takePrunedRoutes();

	// TO AUDIO: A's route into the filter before it goes, with its amount; the one into B after it stays.
	REQUIRE(record.setField(a, mode, static_cast<float>(ff::Mode::Audio)));
	CHECK(record.routeAmount(envA, cutoff) == 0.0f);
	CHECK(record.routeAmount(envA, attackB) == 0.3f);
	std::vector<backplane::Route> gone = record.takePrunedRoutes();
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].source == envA);
	CHECK(gone[0].destination == cutoff);
	CHECK(gone[0].amount == 0.4f);
	CHECK(record.takePrunedRoutes().empty()); // taken once
	// A WRITE THAT CHANGES NOTHING OF WHAT IT MAY REACH DELETES NOTHING.
	REQUIRE(record.setField(a, attack, -6.0f));
	CHECK(record.takePrunedRoutes().empty());

	// BACK TO CONTROL: nothing Audio admitted is forbidden in Control (a source computed from audio AFTER A reaching
	// A's controls is refused in both, by the one order the host walks), so the flip deletes nothing.
	REQUIRE(record.setField(a, mode, static_cast<float>(ff::Mode::Control)));
	CHECK(record.takePrunedRoutes().empty());
	CHECK(record.routeAmount(envA, attackB) == 0.3f);
	// AND IN CONTROL A'S ROUTE INTO THE FILTER IS ADMITTED AGAIN — what an undo leans on.
	CHECK(record.setRoute(envA, cutoff, 0.4f));
	CHECK(record.routeAmount(envA, cutoff) == 0.4f);
	CHECK(record.consistentForTest());
}

TEST_CASE("Follower: in the strip, Audio hears the voice, so a route into a voice parameter goes with the flip") {
	// FEATHERS' STRIP: the host's node (the voice) precedes the first audio-carrying device of every chain, and an
	// Audio-mode follower is one. Its Env into the voice's Level would read the voice before the voice is written.
	backplane::InstanceCounter ids;
	rack::ChainRecord record { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids, &backplane::permitAll };
	const DeviceInstanceId f = record.insert(kStripChain, 0, ff::kTraits.tag);
	REQUIRE(f != kNoInstance);
	const backplane::Source env = backplane::Source::output(f, 0);
	const backplane::Destination level =
	    backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::VoiceLevel)));
	REQUIRE(record.setRoute(env, level, 0.5f));
	REQUIRE(record.setField(f, static_cast<size_t>(ff::Param::Mode), static_cast<float>(ff::Mode::Audio)));
	CHECK(record.routeAmount(env, level) == 0.0f);
	const std::vector<backplane::Route> gone = record.takePrunedRoutes();
	REQUIRE(gone.size() == 1);
	CHECK(gone[0].destination == level);
}

TEST_CASE("Follower: a row put back in Audio (a load, an undo) is in Audio BEFORE its routes arrive") {
	backplane::InstanceCounter ids;
	rack::ChainRecord record { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids, &backplane::permitAll };
	const DeviceInstanceId filter = record.insert(kLaneChain, 0, fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	rack::DeviceState row;
	row.tag = device::tagOf(ff::kTraits.tag);
	row.instanceId = 77;
	const std::vector<float> fields = defaults();
	row.fieldsPresent = fields.size();
	std::copy(fields.begin(), fields.end(), row.field.begin());
	row.field[static_cast<size_t>(ff::Param::Mode)] = static_cast<float>(ff::Mode::Audio);
	REQUIRE(record.insertRow(kLaneChain, 1, row));
	const backplane::Destination cutoff =
	    backplane::Destination::control(filter, static_cast<size_t>(fx::filter::Param::Cutoff));
	CHECK_FALSE(record.setRoute(backplane::Source::output(77, 0), cutoff, 0.4f));
	CHECK(record.routeAmount(backplane::Source::output(77, 0), cutoff) == 0.0f);
}

TEST_CASE("Follower: Mode, Poly and the Detector persist by key, and a load puts the mode in place before its routes") {
	// SAVE: an Audio-mode follower under Poly on RMS, its Env into a filter AFTER it (admitted in Audio). Then the
	// same patch with a second route, into a filter BEFORE it, which Audio forbids: the loader refuses that one.
	backplane::InstanceCounter ids;
	rack::ChainRecord a { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids, &backplane::permitAll };
	const DeviceInstanceId before = a.insert(kLaneChain, 0, fx::filter::kTraits.tag);
	const DeviceInstanceId f = a.insert(kLaneChain, 1, ff::kTraits.tag);
	const DeviceInstanceId after = a.insert(kLaneChain, 2, fx::filter::kTraits.tag);
	REQUIRE(after != kNoInstance);
	const auto cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	const backplane::Source env = backplane::Source::output(f, 0);
	// THE ROUTE AUDIO WILL FORBID IS MADE FIRST, IN CONTROL, so the saved list holds it for the second half.
	REQUIRE(a.setRoute(env, backplane::Destination::control(before, cutoff), 0.25f));
	backplane::RouteList withForbidden;
	a.publishRoutes(withForbidden);
	REQUIRE(a.setField(f, static_cast<size_t>(ff::Param::Mode), static_cast<float>(ff::Mode::Audio)));
	REQUIRE(a.setField(f, static_cast<size_t>(ff::Param::Poly), 1.0f));
	REQUIRE(a.setField(f, static_cast<size_t>(ff::Param::Detector), static_cast<float>(ff::Detector::Rms)));
	REQUIRE(a.setRoute(env, backplane::Destination::control(after, cutoff), 0.5f));
	backplane::RouteList routes;
	a.publishRoutes(routes);
	const rack::CoreCodec codec = feathersCoreCodec();
	const rack::RackPatch patch = rack::patchOfRecord(a, nullptr, {}, routes, codec);

	backplane::InstanceCounter ids2;
	rack::ChainRecord b { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids2, &backplane::permitAll };
	CHECK(rack::applyPatchToRecord(patch, &b, nullptr, &ids2, codec).routes.count == 0);
	REQUIRE(b.holds(f));
	const rack::DeviceState& row = b.rowOf(f);
	CHECK(row.field[static_cast<size_t>(ff::Param::Mode)] == static_cast<float>(ff::Mode::Audio));
	CHECK(row.field[static_cast<size_t>(ff::Param::Poly)] == 1.0f);
	CHECK(row.field[static_cast<size_t>(ff::Param::Detector)] == static_cast<float>(ff::Detector::Rms));
	CHECK(b.routeAmount(env, backplane::Destination::control(after, cutoff)) == 0.5f);

	// THE FORBIDDEN ROUTE IN THE FILE (the record that saved it held it before its flip): refused at the load, said.
	rack::RackPatch stale = rack::patchOfRecord(a, nullptr, {}, withForbidden, codec);
	backplane::InstanceCounter ids3;
	rack::ChainRecord c { feathersAxes(), kFeathersChainCapacities, hostCatalogue(), ids3, &backplane::permitAll };
	CHECK(rack::applyPatchToRecord(stale, &c, nullptr, &ids3, codec).routes.count == 1);
	CHECK(c.routeAmount(env, backplane::Destination::control(before, cutoff)) == 0.0f);
}

TEST_CASE("Follower: no allocation on the audio thread") {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t lane = engine.addDevice(ff::kTraits.tag);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	engine.prepare(kRate, 4096);
	std::array<float, 256> l {};
	std::array<float, 256> r {};
	// WARMED FIRST, so a lazily built table on the first block is the engine's and not counted as the device's.
	engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
	engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
	allocs.allocations = 0;
	allocs.armed = true;
	for (int b = 0; b < 8; ++b) {
		engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
	}
	// AND IN AUDIO, the audio road listening.
	engine.setField(lane, static_cast<size_t>(ff::Param::Mode), static_cast<float>(ff::Mode::Audio));
	for (int b = 0; b < 8; ++b) {
		engine.processStereo(l.data(), r.data(), 256, backplane::unknownBlock());
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0u);
}

TEST_CASE("Follower: a NaN field reads as its default") {
	Ops ops;
	std::vector<float> row(defaults().size(), std::nanf(""));
	ops.set(row);
	ops.tick();
	CHECK(ops.out(ff::Output::Env) == 0.0f);
}

TEST_CASE("Follower: offered in the modulator strip and the browser, filed under Modulators") {
	const device::Descriptor* d = deviceForTag(ff::kTraits.tag);
	REQUIRE(d != nullptr);
	CHECK(std::string(d->traits.name) == "Follower");
	CHECK(d->category == device::Category::Modulators);
	CHECK(maySitInModRack(*d));
	CHECK(isTaggedModulator(*d));
	CHECK(maySitInLane(*d));
	ModRack strip {};
	CHECK(addRackDevice(&strip, ff::kTraits.tag) != kNoPosition);
	bool listed = false;
	for (const rack::BrowserGroup& g : rack::browseDevices(kDevices)) {
		for (const rack::BrowserEntry& e : g.entries) {
			listed = listed || device::sameTag(e.deviceTag, ff::kTraits.tag);
		}
	}
	CHECK(listed);
	// THE TAP IS RETIRED (backlog OP): its tag names no device.
	CHECK(deviceForTag("FOLTAP") == nullptr);
}

//------------------------------------------------------------------------------------------------//
// AUDIO MODE — the audio at its slot (what the Follower Tap was)                                  //
//------------------------------------------------------------------------------------------------//

namespace {

/// AN AUDIO-MODE ROW with no smoothing, so a constant level reads back exactly (Peak, as Audio's flip leaves it).
void instantAudio(rack::RackEngine& engine, size_t lane) {
	engine.setField(lane, static_cast<size_t>(ff::Param::Mode), static_cast<float>(ff::Mode::Audio));
	engine.setField(lane, static_cast<size_t>(ff::Param::Detector), static_cast<float>(ff::Detector::Peak));
	engine.setField(lane, static_cast<size_t>(ff::Param::Attack), static_cast<float>(ff::kAttackLog2Min));
	engine.setField(lane, static_cast<size_t>(ff::Param::Release), static_cast<float>(ff::kReleaseLog2Min));
}

} // namespace

TEST_CASE("Follower in Audio after a Sidechain Inject follows the sidechain's level, one tick late, with the input gone") {
	// BACKLOG NW: following a sidechain is Inject plus an Audio-mode Follower after it. The input is a different level
	// from the sidechain, and Mix 1 replaces it, so the only level the follower can report is the sidechain's.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t inject = engine.addDevice(fx::inject::kTraits.tag);
	const size_t tap = engine.addDevice(ff::kTraits.tag);
	REQUIRE(inject != rack::RackEngine::kNoSlot);
	REQUIRE(tap != rack::RackEngine::kNoSlot);
	engine.prepare(kRate, 4096);
	instantAudio(engine, tap);
	std::array<float, kTick> side {};
	side.fill(0.5f);
	std::array<float, kTick> l {};
	std::array<float, kTick> r {};
	const auto block = [&](bool sidechain) {
		l.fill(0.9f);
		r.fill(0.9f);
		engine.setSidechain(sidechain ? side.data() : nullptr, sidechain ? side.data() : nullptr, sidechain);
		engine.processStereo(l.data(), r.data(), static_cast<int>(kTick), backplane::unknownBlock());
	};
	block(true);
	CHECK(engine.publishedOutputAt(tap, 0) == 0.0f); // ticked before this block's audio reached it
	block(true);
	CHECK(std::abs(engine.publishedOutputAt(tap, 0) - 0.5) <= 1e-6); // the sidechain's 0.5, not the input's 0.9
	CHECK(engine.publishedOutputAt(tap, 1) == 1.0f); // 0.5 reached the default threshold 0.5
	// NO SIDECHAIN IS SILENCE, and the envelope falls to 0.
	block(false);
	block(false);
	CHECK(engine.publishedOutputAt(tap, 0) == 0.0f);
}

TEST_CASE("Follower in Audio: it follows the audio AT ITS SLOT, after the device before it, one tick late; in Control it does not listen") {
	// A BUS UTILITY AT A QUARTER (fader 0.25 → gain (2·0.25)² = 0.25) BEFORE THE FOLLOWER: a constant 0.8 on the bus
	// reaches it at 0.2. It publishes at the next tick what this block carried.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t trim = engine.addDevice(fx::busutil::kTraits.tag);
	const size_t tap = engine.addDevice(ff::kTraits.tag);
	REQUIRE(trim != rack::RackEngine::kNoSlot);
	REQUIRE(tap != rack::RackEngine::kNoSlot);
	engine.prepare(kRate, 4096);
	engine.setField(trim, static_cast<size_t>(fx::busutil::Param::Gain), 0.25f);
	std::array<float, kTick> l {};
	std::array<float, kTick> r {};
	const auto block = [&] {
		l.fill(0.8f);
		r.fill(0.8f);
		engine.processStereo(l.data(), r.data(), static_cast<int>(kTick), backplane::unknownBlock());
	};
	// CONTROL (the default), Input at rest: the bus's 0.2 is not what it follows.
	block();
	block();
	CHECK(engine.publishedOutputAt(tap, 0) == 0.0f);
	instantAudio(engine, tap);
	block();
	const float out = l[kTick - 1];
	block();
	CHECK(std::abs(engine.publishedOutputAt(tap, 0) - 0.2) <= 1e-5);
	// IT WRITES NOTHING: what leaves is what the utility made of it.
	CHECK(std::abs(out - 0.2f) <= 1e-5f);
	CHECK(ff::kAudioOps.listensOnly);
}

TEST_CASE("Follower in Audio: under Poly each voice's follower follows its own voice and answers for its note; in Control Poly does nothing") {
	// TWO VOICES THROUGH THE ENGINE'S PER-VOICE ROAD: voice 0 carries 0.8, voice 1 carries 0.3. Each voice instance
	// hears its voice, ticks on the mod road, and answers for the note that voice holds (`outputForNoteAt`, through
	// `setVoiceNote`). The collapsed cell is not either voice's.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.setVoices(2);
	engine.prepare(kRate, 4096);
	const size_t tap = engine.addDevice(ff::kTraits.tag);
	REQUIRE(tap != rack::RackEngine::kNoSlot);
	engine.setField(tap, static_cast<size_t>(ff::Param::Poly), 1.0f);
	engine.setVoiceNote(0, 11, 0, 60);
	engine.setVoiceNote(1, 12, 0, 67);
	std::array<float, kTick> l {};
	std::array<float, kTick> r {};
	device::NoteContext note;
	const auto block = [&] {
		engine.beginBlock(static_cast<int>(kTick), backplane::unknownBlock());
		for (size_t v = 0; v < 2; ++v) {
			l.fill(v == 0 ? 0.8f : 0.3f);
			r.fill(v == 0 ? 0.8f : 0.3f);
			engine.processVoice(v, l.data(), r.data(), static_cast<int>(kTick), note);
		}
		l.fill(0.0f);
		r.fill(0.0f);
		engine.processBus(l.data(), r.data(), static_cast<int>(kTick));
		engine.endBlock();
	};
	// IN CONTROL, POLY ON IS ONE ON THE BUS (`Traits::perVoiceRequires`).
	block();
	CHECK_FALSE(engine.polyForTest(tap));
	instantAudio(engine, tap);
	block();
	block();
	REQUIRE(engine.polyForTest(tap));
	CHECK(std::abs(engine.outputForNoteAt(tap, 0, 11, 0, 60) - 0.8) <= 1e-5);
	CHECK(std::abs(engine.outputForNoteAt(tap, 0, 12, 0, 67) - 0.3) <= 1e-5);
	// AND EACH VOICE'S TRIG IS ITS OWN: 0.8 crossed the default 50% line on its voice; 0.3 never did on the other.
	CHECK(engine.outputForNoteAt(tap, 1, 11, 0, 60) == 1.0f);
	CHECK(engine.outputForNoteAt(tap, 1, 12, 0, 67) == 0.0f);
	// AND BACK TO CONTROL, POLY READS OFF AGAIN.
	engine.setField(tap, static_cast<size_t>(ff::Param::Mode), static_cast<float>(ff::Mode::Control));
	block();
	CHECK_FALSE(engine.polyForTest(tap));
}

TEST_CASE("Follower in Audio: under Poly the plot's fire count is the newest note's voice's, latched so a change of voice adds nothing") {
	// BACKLOG MY/OP: `trigFiresAt` read the slot's own instance, which under Poly hears nothing, so the Follower plot's
	// pulse marks were blank. It now counts the newest note's voice's pulses — and since every voice counts its own
	// history, the count is LATCHED: a new newest voice adds nothing on the tick it takes over, only its fires after.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.setVoices(2);
	engine.prepare(kRate, 4096);
	const size_t tap = engine.addDevice(ff::kTraits.tag);
	REQUIRE(tap != rack::RackEngine::kNoSlot);
	engine.setField(tap, static_cast<size_t>(ff::Param::Poly), 1.0f);
	instantAudio(engine, tap);
	std::array<float, kTick> l {};
	std::array<float, kTick> r {};
	device::NoteContext note;
	std::array<float, 2> drive { 0.0f, 0.0f };
	const size_t trig = static_cast<size_t>(ff::Output::Trig);
	const auto block = [&] {
		engine.beginBlock(static_cast<int>(kTick), backplane::unknownBlock());
		for (size_t v = 0; v < 2; ++v) {
			l.fill(drive[v]);
			r.fill(drive[v]);
			engine.processVoice(v, l.data(), r.data(), static_cast<int>(kTick), note);
		}
		l.fill(0.0f);
		r.fill(0.0f);
		engine.processBus(l.data(), r.data(), static_cast<int>(kTick));
		engine.endBlock();
	};
	const auto fires = [&] {
		uint32_t n = 0;
		REQUIRE(engine.trigFiresAt(tap, n));
		return n;
	};
	// A SQUARE ON ONE VOICE: `count` crossings of the 50% line, each fully below the re-arm level between. Returns how
	// many ticks the note's own Trig output read 1 — the pulses, read off the output rather than the count.
	const auto square = [&](size_t voice, int count, int32_t id, int16_t key) {
		int pulses = 0;
		for (int c = 0; c < count; ++c) {
			for (int k = 0; k < 8; ++k) {
				drive[voice] = k < 4 ? 0.8f : 0.0f;
				block();
				pulses += engine.outputForNoteAt(tap, trig, id, 0, key) == 1.0f ? 1 : 0;
			}
		}
		drive[voice] = 0.0f;
		return pulses;
	};
	engine.setVoiceNote(0, 11, 0, 60);
	block();
	block();
	REQUIRE(engine.polyForTest(tap));
	// VOICE 1 PULSES WHILE IT IS NOT THE NEWEST: its fires are not the plot's. MORE of them than voice 0 will make, so a
	// count read raw from whichever voice is newest would JUMP UP at the change below.
	const uint32_t before = fires();
	(void)square(1, 9, 12, 67);
	CHECK(fires() == before);
	// VOICE 0, THE NEWEST, PULSES FIVE TIMES: the count moves by exactly its pulses.
	const int c4 = square(0, 5, 11, 60);
	REQUIRE(c4 == 5);
	CHECK(fires() == before + 5u);
	// VOICE 1 TAKES A NOTE AND BECOMES THE NEWEST, carrying nine fires of its own history: nothing flashes.
	const uint32_t atChange = fires();
	engine.setVoiceNote(1, 13, 0, 64);
	block();
	block();
	CHECK(fires() == atChange);
	// AND ITS OWN PULSES FROM HERE ARE COUNTED.
	const int e4 = square(1, 2, 13, 64);
	REQUIRE(e4 == 2);
	CHECK(fires() == atChange + 2u);
}

TEST_CASE("Follower in Audio: under Poly the card's level is the newest note's voice, not the silent bus instance") {
	// BACKLOG MY's open item: `playheadLevel` was read from the slot's own instance, which under Poly hears nothing, so
	// the plot lay flat while the voices followed. The card now reads the voice a destination with no voice reads
	// (`RackEngine::newestVoice`, c7b9a4b9's rule): the voice that took a note most recently.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	engine.setVoices(2);
	engine.prepare(kRate, 4096);
	const size_t tap = engine.addDevice(ff::kTraits.tag);
	REQUIRE(tap != rack::RackEngine::kNoSlot);
	engine.setField(tap, static_cast<size_t>(ff::Param::Poly), 1.0f);
	instantAudio(engine, tap);
	std::array<float, kTick> l {};
	std::array<float, kTick> r {};
	device::NoteContext note;
	const auto block = [&] {
		engine.beginBlock(static_cast<int>(kTick), backplane::unknownBlock());
		for (size_t v = 0; v < 2; ++v) {
			l.fill(v == 0 ? 0.8f : 0.3f);
			r.fill(v == 0 ? 0.8f : 0.3f);
			engine.processVoice(v, l.data(), r.data(), static_cast<int>(kTick), note);
		}
		l.fill(0.0f);
		r.fill(0.0f);
		engine.processBus(l.data(), r.data(), static_cast<int>(kTick));
		engine.endBlock();
	};
	const size_t env = static_cast<size_t>(ff::Output::Env);
	float level = -1.0f;

	// BEFORE ANY VOICE HAS HELD A NOTE: rest, as the publish rests.
	block();
	REQUIRE(engine.playheadLevelAt(tap, level));
	CHECK(level == 0.0f);

	// VOICE 0 TAKES C4, THEN VOICE 1 TAKES G4: G4's voice is the newest, and the level is its 0.3.
	engine.setVoiceNote(0, 11, 0, 60);
	engine.setVoiceNote(1, 12, 0, 67);
	block();
	block();
	REQUIRE(engine.polyForTest(tap));
	REQUIRE(engine.playheadLevelAt(tap, level));
	CHECK(level == engine.outputForNoteAt(tap, env, 12, 0, 67));
	CHECK(std::abs(level - 0.3f) <= 1e-5f);

	// VOICE 0 TAKES A NEW NOTE: now it is the newest, and the level is its 0.8.
	engine.setVoiceNote(0, 13, 0, 62);
	block();
	REQUIRE(engine.playheadLevelAt(tap, level));
	CHECK(level == engine.outputForNoteAt(tap, env, 13, 0, 62));
	CHECK(std::abs(level - 0.8f) <= 1e-5f);

	// AND OFF POLY THE BUS INSTANCE IS READ AGAIN — it hears the bus, which is silent here.
	engine.setField(tap, static_cast<size_t>(ff::Param::Poly), 0.0f);
	for (int k = 0; k < 4; ++k) {
		block();
	}
	REQUIRE(engine.playheadLevelAt(tap, level));
	CHECK(level == 0.0f);
}
