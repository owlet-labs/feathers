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

// **A DEVICE'S KEYED BLOCKS, ON EVERY ROAD** (backlog NM step 1, `device/state_blocks.h`). A fixture modulator whose
// state is a block LARGER THAN THE ROW (300 floats against `kMaxDeviceFields` = 128) and a second, small block that
// asks to be harvested on the tick. Each case is the rule's shape: capture, save, load into a FRESH instance, same
// value by value — through the standalone rack's bridge (the record's road) and through an engine with no bridge
// (`saveRack`/`loadRack`, a rack inside a rack).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "plugin/device_registry.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "persist_rig.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace feathers;

namespace {

constexpr double kRate = 48000.0;
constexpr size_t kBig = 400; // past the row: 320 since 2026-11-06
static_assert(kBig > device::kMaxDeviceFields, "the point of the fixture is a block the row could not hold");

/// THE FIXTURE: each tick writes one cell of a 400-float ring from a deterministic walk, and publishes the ring (block
/// 0) and its tick count with the last cell (block 1, `harvestOnTick`).
struct Walker {
	std::array<float, kBig> ring {};
	uint32_t ticks = 0;
	float last = 0.0f;
	device::BlockSnapshot<kBig> ringSnap;
	device::BlockSnapshot<2> countSnap;

	void tick() {
		last = std::sin(static_cast<float>(ticks) * 0.37f) * 1000.0f + 0.123f * static_cast<float>(ticks % 7);
		ring[ticks % kBig] = last;
		++ticks;
		ringSnap.publish(ring.data(), ring.size());
		const std::array<float, 2> count { static_cast<float>(ticks), last };
		countSnap.publish(count.data(), count.size());
	}
};

Walker& walker(void* s) { return *static_cast<Walker*>(s); }
const Walker& walker(const void* s) { return *static_cast<const Walker*>(s); }

const device::ModOps kWalkerMod {
	.stateBytes = sizeof(Walker),
	.stateAlign = alignof(Walker),
	.construct = [](void* s) { new (s) Walker(); },
	.destruct = [](void* s) { walker(s).~Walker(); },
	.prepare = [](void*, double) {},
	.reset = [](void*) {},
	.setFields = [](void*, const float*, size_t) {},
	.blockSamples = 32,
	.tick = [](void* s, const device::ModTickContext&) { walker(s).tick(); },
	.outputAt = [](const void* s, size_t) { return walker(s).last; },
	.playhead =
	    [](const void* s, uint32_t* ordinal, float* phase) {
		    *ordinal = walker(s).ticks;
		    *phase = 0.0f;
	    },
};

uint64_t readWalker(const void* s, size_t index, float* out, size_t capacity) {
	return index == 0 ? walker(s).ringSnap.read(out, capacity) : index == 1 ? walker(s).countSnap.read(out, capacity) : 0;
}

bool restoreWalker(void* s, size_t index, const float* in, size_t n) {
	Walker& w = walker(s);
	if (index == 0 && n == kBig) {
		std::memcpy(w.ring.data(), in, sizeof(float) * kBig);
		return true;
	}
	if (index == 1 && n == 2) {
		w.ticks = static_cast<uint32_t>(in[0]);
		w.last = in[1];
		return true;
	}
	return false;
}

constexpr device::StateBlockDesc kWalkerBlocks[] = {
	{ .key = "ring", .floats = kBig },
	{ .key = "count", .floats = 2, .harvestOnTick = true },
};
constexpr device::StateOps kWalkerState { .blocks = kWalkerBlocks, .read = &readWalker, .restore = &restoreWalker };
constexpr device::SourceDesc kWalkerOutputs[] = { { .key = "last", .name = "Last" } };
constexpr device::Descriptor kWalker {
	.traits = { .tag = "TWLK", .name = "Walker", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = kWalkerOutputs,
	.mod = &kWalkerMod,
	.state = &kWalkerState,
	.category = device::Category::Modulators,
};

const device::Descriptor* findWithWalker(void*, const char* tag) {
	return device::sameTag(tag, kWalker.traits.tag) ? &kWalker : deviceForTag(tag);
}
device::Catalogue catalogue() { return device::Catalogue { nullptr, &findWithWalker }; }

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 8,
};


std::vector<uint32_t> bitsOf(const Walker& w) {
	std::vector<uint32_t> out;
	for (const float f : w.ring) {
		out.push_back(std::bit_cast<uint32_t>(f));
	}
	out.push_back(w.ticks);
	out.push_back(std::bit_cast<uint32_t>(w.last));
	return out;
}

} // namespace

TEST_CASE("state blocks: the fixture's keys pass the registry's key rule") {
	CHECK(deviceKeysValid(kWalker));
	CHECK(retiredKeysUnused(kWalker));
}

TEST_CASE("state blocks: a block larger than the row survives a save and a load into a fresh rack, value by value, "
          "and the fresh instance goes on from it") {
	persist::Rig run { catalogue() };
	run.seat(kWalker.traits.tag);
	run.block(400); // 400 x 64 samples at 32 a tick: 800 ticks, the ring written round more than twice
	const std::string saved = run.save();
	const std::vector<uint32_t> atSave = bitsOf(run.as<Walker>());
	REQUIRE(run.as<Walker>().ticks == 800u);

	persist::Rig back { catalogue() };
	back.load(saved);
	back.block(); // the drain adopts the fresh instance, then it ticks twice
	run.block();
	// THE FRESH INSTANCE WAS HANDED THE SAVED STATE AND WENT ON FROM IT: after the same block, the two are one.
	CHECK(bitsOf(back.as<Walker>()) == bitsOf(run.as<Walker>()));
	// NOT VACUOUS: the block moved the state, so it is not merely two fresh instances agreeing.
	CHECK(bitsOf(run.as<Walker>()) != atSave);
	CHECK(back.as<Walker>().ticks == 802u);
}

TEST_CASE("state blocks: a load over a running instance under the SAME id plays the file's state, not the run's") {
	persist::Rig run { catalogue() };
	run.seat(kWalker.traits.tag);
	run.block(100);
	const std::string saved = run.save();
	const std::vector<uint32_t> atSave = bitsOf(run.as<Walker>());
	run.block(300); // it moves on past what was saved
	REQUIRE(bitsOf(run.as<Walker>()) != atSave);
	run.load(saved); // the same id, the same tag: only the stored generation says it is new state
	run.bridge->drain();
	CHECK(bitsOf(run.as<Walker>()) == atSave);
}

TEST_CASE("state blocks: a block of the wrong length is said and left out, and the device starts fresh") {
	persist::Rig run { catalogue() };
	run.seat(kWalker.traits.tag);
	run.block(50);
	std::string text = run.save();
	rack::RackPatch patch;
	REQUIRE(rack::parseRackText(text, &patch));
	bool cut = false;
	for (rack::RackPatch::Device& d : patch.device) {
		for (rack::RackPatch::Value& v : d.extra) {
			if (v.key == "ring") {
				v.text = "[1,2,3]";
				cut = true;
			}
		}
	}
	REQUIRE(cut);
	persist::Rig back { catalogue() };
	const rack::LoadReport report = back.bridge->loadPatch(patch);
	back.id = back.bridge->deviceAtPosition(0);
	back.bridge->drain();
	CHECK(report.count[static_cast<size_t>(rack::LoadReport::Kind::ValueCoerced)] == 1);
	// THE RING IS FRESH (zeros) — and the other block, which was whole, was still taken.
	const Walker& w = back.as<Walker>();
	CHECK(std::all_of(w.ring.begin(), w.ring.end(), [](float f) { return f == 0.0f; }));
	CHECK(w.ticks == 100u);
}

TEST_CASE("state blocks: an engine with no bridge saves and loads them too (saveRack / loadRack, a rack in a rack)") {
	rack::RackEngine engine { kAxes, catalogue() };
	engine.prepare(kRate, 4096);
	rack::RackEngine::Provision p = engine.provision(kWalker.traits.tag);
	REQUIRE(p.live());
	const size_t lane = engine.adoptDevice(0, std::move(p));
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	for (int b = 0; b < 300; ++b) {
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	}
	rack::RackPatch patch;
	REQUIRE(rack::patchOf(engine, &patch));
	const std::vector<uint32_t> atSave = bitsOf(walker(static_cast<const void*>(engine.slotAtLane(lane).storage.get())));

	rack::RackEngine fresh { kAxes, catalogue() };
	fresh.prepare(kRate, 4096);
	rack::LoadReport report;
	rack::applyRack(patch, &fresh, &report);
	REQUIRE(fresh.chainLength(0) == 1);
	const size_t back = fresh.deviceAtPosition(0);
	CHECK(bitsOf(walker(static_cast<const void*>(fresh.slotAtLane(back).storage.get()))) == atSave);
}

TEST_CASE("state blocks: only a harvestOnTick block reaches the store between saves; refreshBlocks catches up the rest") {
	ContainerBodies bodies;
	Walker w;
	const DeviceInstanceId id = 7;
	for (int t = 0; t < 10; ++t) {
		w.tick();
	}
	CHECK(rack::refreshBlocks(kWalker, &w, &bodies, id, /*onTickOnly=*/true) == 1);
	REQUIRE(bodies.blocksOf(id) != nullptr);
	REQUIRE(bodies.blocksOf(id)->size() == 1);
	CHECK(bodies.blocksOf(id)->front().index == 1);
	CHECK(bodies.blocksOf(id)->front().floats[0] == 10.0f);
	// NOTHING MOVED, NOTHING WRITTEN: the serial is the one last taken.
	CHECK(rack::refreshBlocks(kWalker, &w, &bodies, id, /*onTickOnly=*/true) == 0);
	// THE WHOLE SET, as a restart's rebuild takes it — and the generation did not move, it is the same state moved on.
	const uint64_t generation = bodies.generationOf(id);
	CHECK(rack::refreshBlocks(kWalker, &w, &bodies, id, /*onTickOnly=*/false) == 1);
	CHECK(bodies.blocksOf(id)->size() == 2);
	CHECK(bodies.generationOf(id) == generation);
	// AND A FRESH INSTANCE RESTORED FROM THE STORE HOLDS IT, to the bit.
	Walker fresh;
	CHECK(rack::restoreBlocks(kWalker, &fresh, bodies, id) == 2);
	CHECK(bitsOf(fresh) == bitsOf(w));
}

TEST_CASE("state blocks: the 16-bit words round-trip every bit of a u64, a double and a float, and refuse a damaged one") {
	std::array<float, 16> buf {};
	device::BlockWriter w(buf.data(), buf.size());
	w.u64(0xFEDCBA9876543210ull);
	w.f64(-0.1234567890123456789);
	w.f32(std::numeric_limits<float>::denorm_min());
	w.flag(true);
	REQUIRE(w.ok());
	REQUIRE(w.size() == 11);
	device::BlockReader r(buf.data(), w.size());
	CHECK(r.u64() == 0xFEDCBA9876543210ull);
	CHECK(std::bit_cast<uint64_t>(r.f64()) == std::bit_cast<uint64_t>(-0.1234567890123456789));
	CHECK(std::bit_cast<uint32_t>(r.f32()) == std::bit_cast<uint32_t>(std::numeric_limits<float>::denorm_min()));
	CHECK(r.flag());
	CHECK(r.ok());
	buf[2] = 0.5f; // not a whole word
	device::BlockReader bad(buf.data(), w.size());
	(void)bad.u64();
	CHECK_FALSE(bad.ok());
}

//==================================================================================================
// THE WIDEST ROW THE CARRIER HOLDS (backlog NR: kMaxDeviceFields 128 -> 160; 2026-10-31: -> 256;
// 2026-11-06: -> 320)
//==================================================================================================

namespace {

/// A DEVICE AS WIDE AS THE TRANSPORT ALLOWS, AND NOT ONE FIELD NARROWER: 320 declared controls, each keyed `w<n>`. It
/// keeps every field `setFields` hands it, so a case can read, VALUE BY VALUE, what a fresh instance was given.
constexpr size_t kWide = 320;
/// PATTERN'S COMING WIDTH (about 268 with each tab's Span, Fill, Start Phase and Fill Trigger), rounded up: the case that
/// says the raise is enough for what asked for it, read as values rather than as a constant compare.
constexpr size_t kPatternWide = 272;
static_assert(kPatternWide <= kWide);

struct WideKeys {
	char key[kWide][6] {};
};
constexpr WideKeys kWideKeys = [] {
	WideKeys k {};
	for (size_t i = 0; i < kWide; ++i) {
		k.key[i][0] = 'w';
		k.key[i][1] = static_cast<char>('0' + i / 100);
		k.key[i][2] = static_cast<char>('0' + (i / 10) % 10);
		k.key[i][3] = static_cast<char>('0' + i % 10);
	}
	return k;
}();
constexpr std::array<device::ParamDesc, kWide> kWideParams = [] {
	std::array<device::ParamDesc, kWide> p {};
	for (size_t i = 0; i < kWide; ++i) {
		p[i] = { kWideKeys.key[i], kWideKeys.key[i], -1000.0, 1000.0, 0.0, false, device::Unit::Number, {} };
	}
	return p;
}();

struct Wide {
	std::array<float, kWide> got {};
	size_t present = 0;
};
Wide& wideAt(void* s) { return *static_cast<Wide*>(s); }

constexpr device::AudioOps kWideOps {
	.stateBytes = sizeof(Wide),
	.stateAlign = alignof(Wide),
	.construct = [](void* s) { new (s) Wide {}; },
	.destruct = [](void* s) { wideAt(s).~Wide(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setSlot = nullptr,
	.setFields =
	    [](void* s, const float* f, size_t n) {
		    Wide& w = wideAt(s);
		    w.present = n;
		    for (size_t i = 0; i < std::min(n, kWide); ++i) {
			    w.got[i] = f[i];
		    }
	    },
	.processStereo = [](void*, float*, float*, int) {},
};

const device::Descriptor kWideDesc {
	.traits = { .tag = "TWID", .name = "Wide", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = kWideParams,
	.outputs = {},
	.audio = &kWideOps,
};

/// THE SAME DEVICE AT 272 FIELDS: the first 272 of the wide one's controls, so nothing about it but its width is new.
const device::Descriptor kPatternWideDesc {
	.traits = { .tag = "TWPT", .name = "Wide 272", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = std::span<const device::ParamDesc>(kWideParams.data(), kPatternWide),
	.outputs = {},
	.audio = &kWideOps,
};

const device::Descriptor* findWithWide(void*, const char* tag) {
	return device::sameTag(tag, kWideDesc.traits.tag)          ? &kWideDesc
	       : device::sameTag(tag, kPatternWideDesc.traits.tag) ? &kPatternWideDesc
	                                                           : deviceForTag(tag);
}

/// FIELD n's VALUE: distinct for every n, exact in a float, and in range.
[[nodiscard]] float wideValue(size_t n) {
	return static_cast<float>(n) * 3.25f - 250.5f;
}

} // namespace

TEST_CASE("NR: a device at the carrier's full width (320 fields) round-trips through save and load into a fresh instance, value by value") {
	CHECK(device::kMaxDeviceFields == kWide);
	CHECK(deviceKeysValid(kWideDesc));
	const device::Catalogue wide { nullptr, &findWithWide };
	std::array<float, kWide> row {};
	for (size_t i = 0; i < kWide; ++i) {
		row[i] = wideValue(i);
	}
	persist::Rig run { wide };
	run.seat(kWideDesc.traits.tag, row);
	run.block(2);
	// THE RUNNING INSTANCE HAS ALL 160 — the carrier into the engine is the whole row.
	REQUIRE(run.as<Wide>().present == kWide);
	const std::string saved = run.save();

	persist::Rig back { wide };
	back.load(saved);
	back.block(2);
	const Wide& w = back.as<Wide>();
	CHECK(w.present == kWide);
	size_t same = 0;
	for (size_t i = 0; i < kWide; ++i) {
		CAPTURE(i);
		CHECK(w.got[i] == wideValue(i));
		same += w.got[i] == wideValue(i) ? 1u : 0u;
	}
	CHECK(same == kWide);
	// THE LAST FIELD IS THE ONE A 128-WIDE CARRIER DROPPED: not its default, so the check above is not vacuous.
	CHECK(w.got[kWide - 1] != 0.0f);
}

TEST_CASE("field cap 320: a 272-field device (Pattern's coming width) round-trips through save and load into a fresh instance, value by value") {
	// THE RAISE'S REASON, AS VALUES (2026-11-06): Pattern grows to about 268
	// fields. A device of 272 is seated, its whole row reaches the running instance, and a fresh instance loaded from the
	// save holds every one of the 272 — the last 16 being exactly what a 256-wide carrier would drop.
	CHECK(deviceKeysValid(kPatternWideDesc));
	const device::Catalogue wide { nullptr, &findWithWide };
	std::array<float, kPatternWide> row {};
	for (size_t i = 0; i < kPatternWide; ++i) {
		row[i] = wideValue(i);
	}
	persist::Rig run { wide };
	run.seat(kPatternWideDesc.traits.tag, row);
	run.block(2);
	REQUIRE(run.as<Wide>().present == kPatternWide);
	const std::string saved = run.save();
	persist::Rig back { wide };
	back.load(saved);
	back.block(2);
	const Wide& w = back.as<Wide>();
	CHECK(w.present == kPatternWide);
	size_t same = 0;
	for (size_t i = 0; i < kPatternWide; ++i) {
		same += w.got[i] == wideValue(i) ? 1u : 0u;
	}
	CHECK(same == kPatternWide);
	// PAST 256: the fields a 256-wide carrier drops, each its own value (none of them the default 0).
	for (size_t i = 256; i < kPatternWide; ++i) {
		CAPTURE(i);
		CHECK(w.got[i] == wideValue(i));
		CHECK(w.got[i] != 0.0f);
	}
}
