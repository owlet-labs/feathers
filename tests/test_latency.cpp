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
 */

// LATENCY — backlog JM 1–3. A device declares how late it is; an engine
// sums that along its serial path; a rack device and an inner chain delay the dry they mix against by the
// same count; and both plugins report the total through `clap_plugin_latency`, asking the host for a restart
// when it moves while active. Every claim here is a VALUE — a count, an index, an amplitude — never "the
// render differs".
//
// THE DEVICE THAT DECLARES A LATENCY IS WRITTEN HERE, because none ships yet (the EQ's linear phase will be
// the first): `latN` is a pure delay by its `Latency` control, and it declares exactly that.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "backplane/block_context.h"
#include "composite/composite_types.h"
#include "fx/delay/delay_types.h"
#include "device/latency.h"
#include "device/registry.h"
#include "plugin/device_registry.h"
#include "plugin/device_row.h"
#include "plugin/feathers_test_chain.h"
#include "plugin/fx_chain.h"
#include "plugin/mod_rack.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#include "rack/inner_chain.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_clap.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/rack_test_ext.h"
#include "rack/site_chains.h"

#include "clap/clap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

//------------------------------------------------------------------------------------------------//
// THE ALLOCATION PROBE — `test_rack_voices.cpp`'s, counting only while armed.                       //
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
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h.
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
#include "nothrow_new_forwarding.h"

using namespace feathers;
using rack::RackEngine;

namespace {

constexpr double kRate = 48000.0;

//------------------------------------------------------------------------------------------------//
// `latN` — A PURE DELAY THAT SAYS HOW LONG IT IS.                                                    //
//------------------------------------------------------------------------------------------------//

constexpr size_t kRing = 1u << 14;

struct Latent {
	std::vector<float> l;
	std::vector<float> r;
	size_t write = 0;
	uint32_t delay = 0;
};

Latent& latentAt(void* s) { return *static_cast<Latent*>(s); }

uint32_t declared(const float* fields, size_t present) {
	if (fields == nullptr || present == 0 || !(fields[0] > 0.0f)) {
		return 0;
	}
	return static_cast<uint32_t>(std::lround(std::min(fields[0], static_cast<float>(kRing - 1))));
}

void delayOne(Latent& d, float* x, std::vector<float>& ring, int frames, size_t start) {
	size_t w = start;
	for (int i = 0; i < frames; ++i) {
		ring[w] = x[i];
		x[i] = ring[(w - d.delay) & (kRing - 1)];
		w = (w + 1) & (kRing - 1);
	}
}

constexpr device::AudioOps kLatentOps {
	.stateBytes = sizeof(Latent),
	.stateAlign = alignof(Latent),
	.construct = [](void* s) { new (s) Latent {}; },
	.destruct = [](void* s) { static_cast<Latent*>(s)->~Latent(); },
	.prepare = [](void* s, double, int) {
		latentAt(s).l.assign(kRing, 0.0f);
		latentAt(s).r.assign(kRing, 0.0f);
	},
	.reset = [](void* s) {
		std::fill(latentAt(s).l.begin(), latentAt(s).l.end(), 0.0f);
		std::fill(latentAt(s).r.begin(), latentAt(s).r.end(), 0.0f);
	},
	.setSlot = nullptr,
	.setFields = [](void* s, const float* f, size_t n) { latentAt(s).delay = declared(f, n); },
	.processStereo = [](void* s, float* l, float* r, int frames) {
		Latent& d = latentAt(s);
		if (d.l.size() < kRing) {
			return;
		}
		delayOne(d, l, d.l, frames, d.write);
		delayOne(d, r, d.r, frames, d.write);
		d.write = (d.write + static_cast<size_t>(frames)) & (kRing - 1);
	},
	.processMono = [](void* s, float* mono, int frames) {
		Latent& d = latentAt(s);
		if (d.l.size() < kRing) {
			return;
		}
		delayOne(d, mono, d.l, frames, d.write);
		d.write = (d.write + static_cast<size_t>(frames)) & (kRing - 1);
	},
	.latencySamples = [](const void*, const float* fields, size_t present, double) { return declared(fields, present); },
};

constexpr std::array<device::ParamDesc, 2> kLatentParams { {
	{ "latency", "Latency", 0.0, 8192.0, 0.0, true, device::Unit::Number, {} },
	{ "poly", "Poly", 0.0, 1.0, 0.0, true, device::Unit::Number, {} },
} };

constexpr size_t kLatencyControl = 0;
constexpr size_t kPolyControl = 1;

const device::Descriptor kLatent {
	.traits = { .tag = "latN", .name = "Latent", .hasCard = false, .hasExpanded = false, .perVoiceControl = 1 },
	.params = kLatentParams,
	.outputs = {},
	.audio = &kLatentOps,
};

/// THE SEAT, FOR THE LIFE OF A CASE: `hostCatalogue()` serves `latN` beside the registry (assumption L11), so
/// the standalone rack, a rack device and an inner chain all build it the way they build a shipped device.
struct Seat {
	Seat() { seatedDeviceForTest() = &kLatent; }
	~Seat() { seatedDeviceForTest() = nullptr; }
	Seat(const Seat&) = delete;
	Seat& operator=(const Seat&) = delete;
};

const std::string kRackTag(composite::kTraits.tag);

//------------------------------------------------------------------------------------------------//
// ENGINE AND RACK-DEVICE HELPERS — `test_rack_nesting.cpp`'s shapes.                                //
//------------------------------------------------------------------------------------------------//

RackEngine& rackIn(RackEngine& outer, size_t lane) {
	const rack::Slot& slot = outer.slotAtLane(lane);
	REQUIRE(slot.storage != nullptr);
	return rack::engineForTest(slot.storage.get());
}

rack::RackBridge& bridgeIn(RackEngine& outer, size_t lane) {
	rack::RackBridge* bridge = rack::bridgeOf(outer.slotAtLane(lane).storage.get());
	REQUIRE(bridge != nullptr);
	return *bridge;
}

/// A latent device inside the rack device at `lane`, `samples` late, drained into the inner engine.
DeviceInstanceId latentInside(RackEngine& outer, size_t lane, float samples) {
	rack::RackBridge& b = bridgeIn(outer, lane);
	const DeviceInstanceId id = b.addDevice(kLatent.traits.tag);
	REQUIRE(id != kNoInstance);
	b.setField(id, kLatencyControl, samples);
	b.drain();
	b.publish(rackIn(outer, lane));
	return id;
}

size_t rackDeviceIn(RackEngine& outer, float mix) {
	const size_t lane = outer.addDevice(kRackTag.c_str());
	REQUIRE(lane != RackEngine::kNoSlot);
	outer.setField(lane, static_cast<size_t>(composite::Param::Mix), mix);
	return lane;
}

/// An impulse of `frames` samples through `engine`, in blocks of `block`: the left channel back.
std::vector<float> impulseThrough(RackEngine& engine, size_t frames, size_t block = 256) {
	std::vector<float> l(frames, 0.0f);
	std::vector<float> r(frames, 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	for (size_t at = 0; at < frames; at += block) {
		const size_t n = std::min(block, frames - at);
		engine.processStereo(l.data() + at, r.data() + at, static_cast<int>(n), backplane::unknownBlock());
	}
	return l;
}

/// Every index whose sample is not exactly zero.
std::vector<size_t> nonZero(const std::vector<float>& x) {
	std::vector<size_t> at;
	for (size_t i = 0; i < x.size(); ++i) {
		if (x[i] != 0.0f) {
			at.push_back(i);
		}
	}
	return at;
}

} // namespace

//------------------------------------------------------------------------------------------------//
// THE DELAY ITSELF                                                                                  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("JM: CompensationDelay delays by exactly the count it is handed, does nothing at all at zero, and clamps at one second") {
	device::CompensationDelay d;
	d.prepare(kRate);
	// ONE SECOND, AS A POWER OF TWO: 65536 at 48 kHz, so the most it can delay is 65535.
	CHECK(d.capacity() == 65535u);

	// AT ZERO THE BUFFER IS NOT TOUCHED — a NaN passes through as the same NaN, which a copy-through-a-ring
	// that happened to read back what it wrote could not promise for a signalling payload, and -0.0 keeps its sign.
	std::array<float, 4> l { -0.0f, 1.0f, 2.0f, 3.0f };
	std::array<float, 4> r { 4.0f, 5.0f, 6.0f, 7.0f };
	d.process(l.data(), r.data(), 4, 0);
	CHECK(std::signbit(l[0]));
	CHECK(l[3] == 3.0f);
	CHECK(r[0] == 4.0f);

	// A DELAY OF 5, ACROSS TWO CALLS: the impulse lands at index 5 of the joined stream.
	std::vector<float> x(16, 0.0f);
	std::vector<float> y(16, 0.0f);
	x[0] = 1.0f;
	y[0] = -1.0f;
	d.process(x.data(), y.data(), 3, 5);
	d.process(x.data() + 3, y.data() + 3, 13, 5);
	CHECK(nonZero(x) == std::vector<size_t> { 5 });
	CHECK(x[5] == 1.0f);
	CHECK(y[5] == -1.0f);
	CHECK(d.delayForTest() == 5u);

	// PAST THE CEILING IT CLAMPS rather than reading outside the ring.
	std::vector<float> big(8, 0.0f);
	std::vector<float> bigR(8, 0.0f);
	d.process(big.data(), bigR.data(), 8, 1u << 20);
	CHECK(d.delayForTest() == d.capacity());
}

//------------------------------------------------------------------------------------------------//
// THE SUM                                                                                           //
//------------------------------------------------------------------------------------------------//

TEST_CASE("JM: an engine's latency is the SERIAL SUM of what its devices declare — two latent devices report N1 + N2, and the audio is that late") {
	Seat seat;
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.prepare(kRate, 4096);
	CHECK(e.latencySamples(kRate) == 0u); // empty

	const size_t a = e.addDevice(kLatent.traits.tag);
	const size_t b = e.addDevice(kLatent.traits.tag);
	REQUIRE(a != RackEngine::kNoSlot);
	REQUIRE(b != RackEngine::kNoSlot);
	CHECK(e.latencySamples(kRate) == 0u); // declared at their default of zero
	e.setField(a, kLatencyControl, 100.0f);
	e.setField(b, kLatencyControl, 250.0f);
	CHECK(e.latencySamples(kRate) == 350u);

	// AND A DEVICE THAT DECLARES NOTHING ADDS NOTHING (the filter: null `latencySamples`).
	REQUIRE(e.addDevice("flt1") != RackEngine::kNoSlot);
	CHECK(e.latencySamples(kRate) == 350u);

	// THE DECLARATION AGREES WITH THE AUDIO: the filter is at its defaults, so ask for the impulse's
	// position, not its value.
	const std::vector<float> out = impulseThrough(e, 1024);
	size_t peak = 0;
	for (size_t i = 1; i < out.size(); ++i) {
		if (std::fabs(out[i]) > std::fabs(out[peak])) {
			peak = i;
		}
	}
	CHECK(peak >= 350u);
	for (size_t i = 0; i < 350; ++i) {
		CHECK(out[i] == 0.0f);
	}
}

//------------------------------------------------------------------------------------------------//
// COMPENSATION WHERE BRANCHES MEET                                                                  //
//------------------------------------------------------------------------------------------------//

TEST_CASE("JM: a rack device's Mix aligns its dry with a latent chain — an impulse at mix 0.5 comes out ONCE, at the chain's latency, at full height") {
	Seat seat;
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(kRate, 4096);
	const size_t lane = rackDeviceIn(outer, 0.5f);
	latentInside(outer, lane, 64.0f);

	// THE RACK DEVICE REPORTS WHAT IT HOLDS, and the outer engine sums it.
	CHECK(outer.latencySamples(kRate) == 64u);

	// UNCOMPENSATED this is two half-height impulses, at 0 (the dry) and 64 (the chain): a comb.
	const std::vector<float> out = impulseThrough(outer, 512);
	CHECK(nonZero(out) == std::vector<size_t> { 64 });
	CHECK(out[64] == doctest::Approx(1.0f));
}

TEST_CASE("JM: racks nest without a special case — a rack in a rack, both at mix 0.5, with a latent device at the bottom, gives ONE impulse at its latency") {
	Seat seat;
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(kRate, 4096);
	const size_t lane = rackDeviceIn(outer, 0.5f);
	rack::RackBridge& b = bridgeIn(outer, lane);
	const DeviceInstanceId middle = b.addDevice(kRackTag.c_str());
	REQUIRE(middle != kNoInstance);
	b.setField(middle, static_cast<size_t>(composite::Param::Mix), 0.5f);
	b.drain();
	RackEngine& inner = rackIn(outer, lane);
	const size_t middleLane = inner.slotOf(middle);
	REQUIRE(middleLane != RackEngine::kNoSlot);
	latentInside(inner, middleLane, 32.0f);

	CHECK(inner.latencySamples(kRate) == 32u);
	CHECK(outer.latencySamples(kRate) == 32u);
	const std::vector<float> out = impulseThrough(outer, 256, 64);
	CHECK(nonZero(out) == std::vector<size_t> { 32 });
	CHECK(out[32] == doctest::Approx(1.0f));
}

TEST_CASE("JM: a bypassed rack device keeps its latency — the settled pass-through is delayed by what the chain would take, and the report does not move") {
	Seat seat;
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(kRate, 4096);
	const size_t lane = rackDeviceIn(outer, 1.0f);
	latentInside(outer, lane, 48.0f);
	outer.setField(lane, static_cast<size_t>(composite::Param::Enabled), 0.0f);
	CHECK(outer.latencySamples(kRate) == 48u);

	// THROUGH AN ENGINE, AS A HOST RUNS IT: an impulse, one sample, 48 late, at unity.
	std::vector<float> warmL(1024, 0.0f);
	std::vector<float> warmR(1024, 0.0f);
	outer.processStereo(warmL.data(), warmR.data(), 1024, backplane::unknownBlock());
	const std::vector<float> out = impulseThrough(outer, 256);
	CHECK(nonZero(out) == std::vector<size_t> { 48 });
	CHECK(out[48] == 1.0f);

	// **AND THE SETTLED "TRUE PASS" ITSELF, which the engine above never reaches**: `setFields` re-aims the
	// 5 ms ramp every block (`aim` sets `remaining` whatever the target), so a rack device run by an engine
	// is never "settled" at the top of its process and takes the blend road at dry 1, wet 0. Driven through
	// its own ops with its fields handed once, the ramp runs out and the next call is the pass.
	void* storage = outer.slotAtLane(lane).storage.get();
	REQUIRE(storage != nullptr);
	const device::AudioOps& ops = rack::kDeviceAudioOps;
	std::array<float, static_cast<size_t>(composite::Param::kCount)> fields {};
	fields[static_cast<size_t>(composite::Param::Enabled)] = 0.0f;
	fields[static_cast<size_t>(composite::Param::Mix)] = 1.0f;
	fields[static_cast<size_t>(composite::Param::Gain)] = static_cast<float>(composite::kFaderUnity);
	ops.setFields(storage, fields.data(), fields.size());
	ops.processStereo(storage, warmL.data(), warmR.data(), 1024); // the ramp runs out
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	ops.processStereo(storage, l.data(), r.data(), 256);
	CHECK(nonZero(l) == std::vector<size_t> { 48 });
	CHECK(l[48] == 1.0f);
}

TEST_CASE("JM: an inner chain (SKLITTER's post chain) aligns its own mix, and reports what it holds") {
	Seat seat;
	rack::InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(hostCatalogue());
	rack::RackBridge* b = chain.bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kLatent.traits.tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, kLatencyControl, 40.0f);
	b->drain();
	chain.setMix(0.5f);
	CHECK(chain.latencySamples(kRate) == 40u);

	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	chain.process(l.data(), r.data(), 256);
	CHECK(nonZero(l) == std::vector<size_t> { 40 });
	CHECK(l[40] == doctest::Approx(1.0f));

	// AND BYPASSED, SETTLED: the pass waits as long as the chain would (assumption L3), at unity.
	chain.setEnabled(false);
	std::vector<float> warmL(1024, 0.0f);
	std::vector<float> warmR(1024, 0.0f);
	chain.process(warmL.data(), warmR.data(), 1024);
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	chain.process(l.data(), r.data(), 256);
	CHECK(chain.latencySamples(kRate) == 40u);
	CHECK(nonZero(l) == std::vector<size_t> { 40 });
	CHECK(l[40] == 1.0f);
}

TEST_CASE("JM: with NO latent device the rack device's mix is the plain blend bit for bit — the delay never engages") {
	// THE INVARIANT THE WHOLE DESIGN WAS BUILT AGAINST: a zero-latency chain at mix 0.5 is exactly
	// `x·0.5 + x·0.5` per sample, computed here apart from the code, with the dry undelayed.
	Seat seat;
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(kRate, 4096);
	const size_t lane = rackDeviceIn(outer, 0.5f);
	latentInside(outer, lane, 0.0f); // present, declaring nothing: a pass-through
	CHECK(outer.latencySamples(kRate) == 0u);

	std::vector<float> l(300);
	std::vector<float> r(300);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = std::sin(0.01f * static_cast<float>(i)) * 0.7f;
		r[i] = -l[i];
	}
	const std::vector<float> inL = l;
	const std::vector<float> inR = r;
	outer.processStereo(l.data(), r.data(), static_cast<int>(l.size()), backplane::unknownBlock());
	for (size_t i = 0; i < l.size(); ++i) {
		CHECK(l[i] == inL[i] * 0.5f + inL[i] * 0.5f);
		CHECK(r[i] == inR[i] * 0.5f + inR[i] * 0.5f);
	}
}

TEST_CASE("JM: a latent device never runs per voice — its Poly switch reads as off while it declares a latency, and on again when it declares none") {
	Seat seat;
	RackEngine e { rack::kRackAxes, hostCatalogue() };
	e.setVoices(4);
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kLatent.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, kPolyControl, 1.0f);

	// THE CONTROL: at zero latency the Poly switch puts it in the voices, so the refusal below is not vacuous.
	e.beginBlock(0, backplane::unknownBlock());
	CHECK(e.polyForTest(lane));
	e.endBlock();

	e.setField(lane, kLatencyControl, 16.0f);
	e.beginBlock(0, backplane::unknownBlock());
	CHECK_FALSE(e.polyForTest(lane));
	e.endBlock();
}

TEST_CASE("JM: compensating allocates nothing on the audio thread — a latent chain at mix 0.5, its latency changed between blocks") {
	Seat seat;
	RackEngine outer { rack::kRackAxes, hostCatalogue() };
	outer.prepare(kRate, 4096);
	const size_t lane = rackDeviceIn(outer, 0.5f);
	const DeviceInstanceId id = latentInside(outer, lane, 64.0f);
	std::vector<float> l(512, 0.25f);
	std::vector<float> r(512, 0.25f);
	outer.processStereo(l.data(), r.data(), 512, backplane::unknownBlock()); // warm

	// THE EDIT IS THE MESSAGE THREAD'S (an allocation there is legal); the DRAIN that adopts it, the latency
	// that moves, and the delay that follows are the audio thread's — inside the armed window.
	bridgeIn(outer, lane).setField(id, kLatencyControl, 200.0f);
	allocs.allocations = 0;
	allocs.armed = true;
	for (int block = 0; block < 8; ++block) {
		outer.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	}
	allocs.armed = false;
	CHECK(outer.latencySamples(kRate) == 200u);
	CHECK(allocs.allocations == 0);
}

//------------------------------------------------------------------------------------------------//
// FEATHERS' LANE                                                                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("JM: Feathers' lane reports what its fx devices declare — the number `clap_plugin_latency` answers") {
	Seat seat;
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	synth.setCatalogue(hostCatalogue());
	CHECK(synth.latencySamples() == 0u);

	FxChain chain {};
	DeviceRow row = defaultRowFor(kLatent, { kFirstFreeInstance + 7 });
	row.field[kLatencyControl] = 123.0f;
	chain.device[0].row = row;
	chain.count = 1;
	synth.provisionDevices(chain, defaultModRack());
	synth.setFxChain(chain);
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	synth.render(l.data(), r.data(), 64); // the block whose drain adopts it
	CHECK(synth.latencySamples() == 123u);
}

//------------------------------------------------------------------------------------------------//
// THE CLAP EXTENSION, ON THE STANDALONE RACK                                                        //
//------------------------------------------------------------------------------------------------//

namespace {

/// A HOST THAT OFFERS `clap_host_latency` AND COUNTS what the plugin tells it: `changed` and `request_restart`.
struct LatencyHost {
	int changed = 0;
	int restarts = 0;
	clap_host host {};

	LatencyHost() {
		host.clap_version = CLAP_VERSION_INIT;
		host.host_data = this;
		host.name = "latency-venue";
		host.vendor = "Owlet";
		host.url = "";
		host.version = "1";
		host.get_extension = [](const clap_host*, const char* id) -> const void* {
			static const clap_host_latency kLatency {
				[](const clap_host* h) { ++static_cast<LatencyHost*>(h->host_data)->changed; },
			};
			if (id != nullptr && std::strcmp(id, CLAP_EXT_LATENCY) == 0) {
				return &kLatency;
			}
			return nullptr;
		};
		host.request_restart = [](const clap_host* h) { ++static_cast<LatencyHost*>(h->host_data)->restarts; };
		host.request_process = [](const clap_host*) {};
		host.request_callback = [](const clap_host*) {};
	}
	LatencyHost(const LatencyHost&) = delete;
	LatencyHost& operator=(const LatencyHost&) = delete;
};

/// The standalone rack on that host, initialised but NOT activated — a case adds devices first or not.
struct Rack {
	LatencyHost host;
	const clap_plugin* plugin = nullptr;
	std::vector<float> l = std::vector<float>(256, 0.0f);
	std::vector<float> r = std::vector<float>(256, 0.0f);
	bool active = false;

	Rack() {
		plugin = rack::create(&host.host);
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
	}
	~Rack() {
		stop();
		plugin->destroy(plugin);
	}
	Rack(const Rack&) = delete;
	Rack& operator=(const Rack&) = delete;

	void start() {
		REQUIRE(plugin->activate(plugin, kRate, 1, 256));
		REQUIRE(plugin->start_processing(plugin));
		active = true;
	}
	void stop() {
		if (active) {
			plugin->stop_processing(plugin);
			plugin->deactivate(plugin);
			active = false;
		}
	}
	/// What the host does after `request_restart`.
	void restart() {
		stop();
		start();
	}

	uint32_t latency() const {
		const auto* ext = static_cast<const clap_plugin_latency*>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
		REQUIRE(ext != nullptr);
		return ext->get(plugin);
	}

	/// A latent device, `samples` late, through the chain venue (the record; the engine at the next drain).
	void addLatent(double samples) {
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		const uint32_t position = chain->add_device(plugin, kLatent.traits.tag);
		REQUIRE(position != UINT32_MAX);
		REQUIRE(chain->set_control(plugin, position, "Latency", samples));
	}

	void run() {
		clap_audio_buffer out {};
		float* ptr[2] { l.data(), r.data() };
		out.data32 = ptr;
		out.channel_count = 2;
		clap_input_events in {};
		in.size = [](const clap_input_events*) -> uint32_t { return 0; };
		in.get = [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; };
		clap_output_events sink {};
		sink.try_push = [](const clap_output_events*, const clap_event_header*) { return true; };
		clap_process p {};
		p.frames_count = static_cast<uint32_t>(l.size());
		p.audio_outputs = &out;
		p.audio_outputs_count = 1;
		p.in_events = &in;
		p.out_events = &sink;
		REQUIRE(plugin->process(plugin, &p) == CLAP_PROCESS_CONTINUE);
	}
};

} // namespace

TEST_CASE("JM: the standalone rack with a device declaring N samples reports exactly N through clap_plugin_latency, and tells the host at activate") {
	Seat seat;
	Rack rack;
	rack.addLatent(777.0);
	rack.start();
	CHECK(rack.latency() == 777u);
	CHECK(rack.host.changed == 1);
	// AND NOTHING MOVED AFTERWARDS, so nothing asks for a restart.
	for (int i = 0; i < 4; ++i) {
		rack.run();
	}
	CHECK(rack.host.restarts == 0);
}

TEST_CASE("JM: the standalone rack's serial sum — two latent devices report N1 + N2") {
	Seat seat;
	Rack rack;
	rack.addLatent(300.0);
	rack.addLatent(45.0);
	rack.start();
	CHECK(rack.latency() == 345u);
}

TEST_CASE("JM: with no latent device the standalone rack reports 0, tells the host nothing and asks for no restart — a real device in the chain changes none of it") {
	Rack rack;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    rack.plugin->get_extension(rack.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(chain->add_device(rack.plugin, "flt1") != UINT32_MAX);
	rack.start();
	CHECK(rack.latency() == 0u);
	for (int i = 0; i < 4; ++i) {
		rack.run();
	}
	CHECK(rack.latency() == 0u);
	CHECK(rack.host.changed == 0);
	CHECK(rack.host.restarts == 0);
}

TEST_CASE("JM: a latency that moves while active asks the host to restart ONCE, and the next activate reports it and calls clap_host_latency.changed") {
	Seat seat;
	Rack rack;
	rack.start();
	CHECK(rack.latency() == 0u);
	CHECK(rack.host.changed == 0);

	rack.addLatent(512.0); // while active: the record now, the engine at the next block's drain
	rack.run();
	CHECK(rack.host.restarts == 1);
	// STILL THE OLD NUMBER until the host restarts — CLAP lets it change only during activate.
	CHECK(rack.latency() == 0u);
	rack.run();
	rack.run();
	CHECK(rack.host.restarts == 1); // once, not once a block

	rack.restart();
	CHECK(rack.latency() == 512u);
	CHECK(rack.host.changed == 1);
	rack.run();
	CHECK(rack.host.restarts == 1); // settled: nothing more to ask
}

//------------------------------------------------------------------------------------------------//
// HOLDING A TAIL THROUGH A CONTAINER — backlog MC                                                   //
//------------------------------------------------------------------------------------------------//

namespace {

/// A CLEAN DELAY, WET ONLY, A 1 s TAP: a click, then a second of exact silence with the repeat still in the line.
void wetDelay(rack::RackBridge& b, DeviceInstanceId id) {
	b.setField(id, static_cast<size_t>(fx::delay::Param::Enabled), 1.0f);
	b.setField(id, static_cast<size_t>(fx::delay::Param::Mix), 1.0f);
	b.setField(id, static_cast<size_t>(fx::delay::Param::Sync), 2.0f); // 1 s
	b.setField(id, static_cast<size_t>(fx::delay::Param::Feedback), 0.3f);
}

/// A click through `run`, then `blocks` silent blocks of 32: whether every one of them was exactly silent.
template <typename Run>
bool clickThenSilence(Run&& run, int blocks) {
	std::vector<float> l(32, 0.0f);
	std::vector<float> r(32, 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	run(l.data(), r.data(), 32);
	bool silent = true;
	for (int b = 0; b < blocks; ++b) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		run(l.data(), r.data(), 32);
		for (int i = 0; i < 32; ++i) {
			silent = silent && l[i] == 0.0f && r[i] == 0.0f;
		}
	}
	return silent;
}

} // namespace

TEST_CASE("MC: a rack device and an inner chain answer holdsTail for the devices inside — a delay in its silent gap, and nothing once bypassed and settled") {
	// **A CONTAINER ANSWERS FOR WHAT IT HOLDS** (`AudioOps::holdsTail`): Feathers' SLEEP asks the lane, the lane asks
	// each device, and a rack device or an inner chain (SKLITTER's, Resonate's lanes) must pass the question down, or
	// a delay inside one is the defect again, one level deeper. Asserted in the gap — every block exactly silent,
	// half a second after the click, a second before the repeat — where only the answer can tell.
	SUBCASE("a rack device in an engine") {
		RackEngine outer { rack::kRackAxes, hostCatalogue() };
		outer.prepare(kRate, 4096);
		const size_t lane = rackDeviceIn(outer, 1.0f);
		rack::RackBridge& b = bridgeIn(outer, lane);
		const DeviceInstanceId id = b.addDevice(fx::delay::kTraits.tag);
		REQUIRE(id != kNoInstance);
		wetDelay(b, id);
		b.drain();
		b.publish(rackIn(outer, lane));
		CHECK_FALSE(outer.holdsTail()); // nothing heard, nothing held
		const bool silent = clickThenSilence(
		    [&](float* l, float* r, int n) { outer.processStereo(l, r, n, backplane::unknownBlock()); }, 750);
		CHECK(silent);              // the gap is real: half a second of exact zeroes...
		CHECK(outer.holdsTail());   // ...and the engine says the repeat is still to come, through the rack device
		CHECK(rackIn(outer, lane).holdsTail());

		// BYPASSED AND SETTLED, nothing inside runs, so the frozen repeat cannot come out (`kDeviceAudioOps`, driven
		// through its own ops as the latency case above does, because an engine re-aims the ramp every block).
		void* storage = outer.slotAtLane(lane).storage.get();
		REQUIRE(storage != nullptr);
		const device::AudioOps& ops = rack::kDeviceAudioOps;
		REQUIRE(ops.holdsTail != nullptr);
		std::array<float, static_cast<size_t>(composite::Param::kCount)> fields {};
		fields[static_cast<size_t>(composite::Param::Enabled)] = 0.0f;
		fields[static_cast<size_t>(composite::Param::Mix)] = 1.0f;
		fields[static_cast<size_t>(composite::Param::Gain)] = static_cast<float>(composite::kFaderUnity);
		ops.setFields(storage, fields.data(), fields.size());
		std::vector<float> warmL(1024, 0.0f);
		std::vector<float> warmR(1024, 0.0f);
		ops.processStereo(storage, warmL.data(), warmR.data(), 1024); // the ramp runs out
		CHECK(rackIn(outer, lane).holdsTail()); // the delay inside still holds its repeat...
		CHECK_FALSE(ops.holdsTail(storage));    // ...and the rack, bypassed, will never play it
	}
	SUBCASE("an inner chain") {
		rack::InnerChain chain { rack::kRackAxes };
		chain.prepare(kRate, 4096);
		chain.adopt(hostCatalogue());
		rack::RackBridge* b = chain.bridge();
		REQUIRE(b != nullptr);
		const DeviceInstanceId id = b->addDevice(fx::delay::kTraits.tag);
		REQUIRE(id != kNoInstance);
		wetDelay(*b, id);
		b->drain();
		CHECK_FALSE(chain.holdsTail());
		const bool silent = clickThenSilence([&](float* l, float* r, int n) { chain.process(l, r, n); }, 750);
		CHECK(silent);
		CHECK(chain.holdsTail());

		chain.setEnabled(false);
		std::vector<float> warmL(1024, 0.0f);
		std::vector<float> warmR(1024, 0.0f);
		chain.process(warmL.data(), warmR.data(), 1024); // the ramp runs out
		CHECK(chain.engine().holdsTail());
		CHECK_FALSE(chain.holdsTail());
	}
}

TEST_CASE("NR: an inner chain's dry delay is sized to what the chain declares at prepare — 0.1 s for an empty one, the declared latency past it, a second at most") {
	Seat seat;
	rack::InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	// 0.1 s at 48 kHz is 4800 samples: the next power of two is 8192, and a ring reaches one less.
	CHECK(chain.dryCapacityForTest() == 8191u);

	chain.adopt(hostCatalogue());
	rack::RackBridge* b = chain.bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kLatent.traits.tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, kLatencyControl, 8192.0f);
	b->drain();
	REQUIRE(chain.latencySamples(kRate) == 8192u);
	chain.prepare(kRate, 4096); // the host's restart after a latency change
	CHECK(chain.dryCapacityForTest() == 16383u); // 8193 rounds up to 16384

	// AND THE SECOND STILL BOUNDS IT, as `prepare(rate)` does.
	device::CompensationDelay d;
	d.prepare(kRate, 1000000u);
	CHECK(d.capacity() == 65535u);
	d.prepare(kRate, 0u);
	CHECK(d.capacity() == 8191u);
}

TEST_CASE("NR: Feathers' lane reports a device's site on top of the device — 123 + 40, the number `clap_plugin_latency` answers") {
	Seat seat;
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	synth.setCatalogue(hostCatalogue());

	FxChain chain {};
	const DeviceInstanceId id = kFirstFreeInstance + 7;
	DeviceRow row = defaultRowFor(kLatent, { id });
	row.field[kLatencyControl] = 123.0f;
	chain.device[0].row = row;
	chain.count = 1;
	// THE SITE IS FILED IN THE CARRIER, as `ParamBridge::openFxSite` files it: empty, which is "it has one".
	ContainerBodies bodies;
	REQUIRE(bodies.store(siteKeyOf(id), nullptr, 0));
	synth.provisionDevices(chain, defaultModRack(), &bodies);
	synth.setFxChain(chain);
	auto* site = static_cast<rack::SiteChains*>(bodies.surfaceOf(siteKeyOf(id)));
	REQUIRE(site != nullptr);
	rack::RackBridge* post = site->post().bridge();
	REQUIRE(post != nullptr);
	const DeviceInstanceId inner = post->addDevice(kLatent.traits.tag);
	REQUIRE(inner != kNoInstance);
	post->setField(inner, kLatencyControl, 40.0f);
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	synth.render(l.data(), r.data(), 64); // the lane adopts the device and installs its site; the site's lane drains
	synth.render(l.data(), r.data(), 64);
	CHECK(synth.latencySamples() == 163u);
}

TEST_CASE("NR: a device taken out of Feathers' lane and put back under its id gets its site back WITH what the site held") {
	Seat seat;
	ParamSet params;
	Synth synth { params };
	synth.activate(kRate, 4096);
	synth.setCatalogue(hostCatalogue());
	FxChain chain {};
	const DeviceInstanceId id = kFirstFreeInstance + 9;
	chain.device[0].row = defaultRowFor(kLatent, { id });
	chain.count = 1;
	ContainerBodies bodies;
	REQUIRE(bodies.store(siteKeyOf(id), nullptr, 0));
	synth.provisionDevices(chain, defaultModRack(), &bodies);
	synth.setFxChain(chain);
	auto* site = static_cast<rack::SiteChains*>(bodies.surfaceOf(siteKeyOf(id)));
	REQUIRE(site != nullptr);
	rack::RackBridge* post = site->post().bridge();
	const DeviceInstanceId inner = post->addDevice(kLatent.traits.tag);
	REQUIRE(inner != kNoInstance);
	post->setField(inner, kLatencyControl, 40.0f);
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	synth.render(l.data(), r.data(), 64);
	synth.render(l.data(), r.data(), 64);
	REQUIRE(synth.latencySamples() == 40u);

	// OUT: the lane empties, the site is let go and — once nothing can be reading it — freed, its contents filed back.
	FxChain empty {};
	synth.provisionDevices(empty, defaultModRack(), &bodies);
	synth.setFxChain(empty);
	for (int pass = 0; pass < 4; ++pass) {
		synth.render(l.data(), r.data(), 64);
		synth.provisionDevices(empty, defaultModRack(), &bodies);
		synth.drainRetiredDevices();
	}
	CHECK(bodies.surfaceOf(siteKeyOf(id)) == nullptr);
	CHECK(synth.latencySamples() == 0u);

	// BACK, under the same id (an undo): the site is rebuilt from what it held, the latent device in its POST included.
	synth.provisionDevices(chain, defaultModRack(), &bodies);
	synth.setFxChain(chain);
	synth.render(l.data(), r.data(), 64);
	synth.render(l.data(), r.data(), 64);
	auto* back = static_cast<rack::SiteChains*>(bodies.surfaceOf(siteKeyOf(id)));
	REQUIRE(back != nullptr);
	CHECK(back->deviceCount(rack::SiteChains::kPostLane) == 1u);
	CHECK(synth.latencySamples() == 40u);
}
