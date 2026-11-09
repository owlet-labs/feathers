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

#include "mod/lfo/lfo_types.h"
#include "mod/macros/macros.h"
#include "mod/macros/macros_types.h"
#include "plugin/device_registry.h"
#include "plugin/device_row.h"
#include "plugin/mod_rack.h"
#include "rack/device_browser.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

// THE MACROS MODULATOR — four macros whose outputs are sources and whose values are destinations.
//
// Every claim about an output is asserted as the NUMBER the device or the engine publishes, restated
// independently here (`v` or `2v − 1`), never inferred from a render.

using namespace feathers;
namespace macros = feathers::mod::macros;

namespace {

/// THE DEVICE THROUGH ITS OPS — the table a host reaches it by.
struct Ops {
	void* mem = nullptr;
	Ops() {
		const device::ModOps& mod = macros::kModOps;
		const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, 48000.0);
	}
	~Ops() {
		macros::kModOps.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Ops(const Ops&) = delete;
	Ops& operator=(const Ops&) = delete;

	/// Hand the row over and tick once, as the host does, then read output n.
	void set(const std::vector<float>& row) {
		macros::kModOps.setFields(mem, row.data(), row.size());
		device::ModTickContext ctx;
		macros::kModOps.tick(mem, ctx);
	}
	[[nodiscard]] float out(size_t n) const { return macros::kModOps.outputAt(mem, n); }
};

std::vector<float> defaults() {
	std::vector<float> row;
	for (const device::ParamDesc& p : macros::kDescriptor.params) {
		row.push_back(static_cast<float>(p.def));
	}
	return row;
}

constexpr std::array<float, macros::kMacroCount> kValues { 0.1f, 0.35f, 0.8f, 1.0f };

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 8,
};

} // namespace

TEST_CASE("each output is its macro") {
	Ops ops;
	std::vector<float> row = defaults();
	for (size_t n = 0; n < macros::kMacroCount; ++n) {
		row[macros::macroParam(n)] = kValues[n];
	}
	ops.set(row);
	for (size_t n = 0; n < macros::kMacroCount; ++n) {
		CAPTURE(n);
		CHECK(ops.out(n) == kValues[n]);
	}
	// PAST THE FOURTH IS NOTHING, not a read off the end.
	CHECK(ops.out(macros::kMacroCount) == 0.0f);
}

TEST_CASE("bipolar maps 2v-1 per output") {
	Ops ops;
	std::vector<float> row = defaults();
	for (size_t n = 0; n < macros::kMacroCount; ++n) {
		row[macros::macroParam(n)] = kValues[n];
	}
	// ONLY 2 AND 4 FLAGGED, so a mapping that ignored the flag — or applied one output's flag to all —
	// is wrong on at least one of the four.
	row[macros::bipolarParam(1)] = 1.0f;
	row[macros::bipolarParam(3)] = 1.0f;
	ops.set(row);
	CHECK(ops.out(0) == kValues[0]);
	CHECK(ops.out(1) == doctest::Approx(2.0 * kValues[1] - 1.0)); // -0.3
	CHECK(ops.out(2) == kValues[2]);
	CHECK(ops.out(3) == doctest::Approx(2.0 * kValues[3] - 1.0)); // +1
	// AND THE REST POINT: a bipolar macro at its middle publishes zero.
	row[macros::macroParam(1)] = 0.5f;
	ops.set(row);
	CHECK(ops.out(1) == 0.0f); // exact: 2 × 0.5 − 1
}

TEST_CASE("outputs are independent") {
	Ops ops;
	std::vector<float> row = defaults();
	for (size_t n = 0; n < macros::kMacroCount; ++n) {
		row[macros::macroParam(n)] = kValues[n];
	}
	ops.set(row);
	for (size_t moved = 0; moved < macros::kMacroCount; ++moved) {
		CAPTURE(moved);
		std::vector<float> edited = row;
		edited[macros::macroParam(moved)] = 0.5f;
		edited[macros::bipolarParam(moved)] = 1.0f;
		ops.set(edited);
		for (size_t n = 0; n < macros::kMacroCount; ++n) {
			CAPTURE(n);
			CHECK(ops.out(n) == doctest::Approx(n == moved ? 0.0 : kValues[n]));
		}
	}
}

TEST_CASE("a route into Macro 2 moves output 2") {
	// **THE PUBLISHED OUTPUT IS THE RESOLVED VALUE, NOT THE BASE** — asserted at the engine, where the
	// resolve happens, as a number restated here: base + amount × source × span.
	const size_t macro2 = macros::macroParam(1);

	SUBCASE("a rack macro, exactly") {
		rack::RackEngine engine { kAxes, hostCatalogue() };
		const size_t lane = engine.addDevice(macros::kTraits.tag);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		engine.prepare(48000.0, 4096);
		const float base = 0.25f;
		engine.setField(lane, macro2, base);
		engine.setField(lane, macros::macroParam(0), 0.6f);
		engine.setRouteAmount(test::macro(0), test::controlOf(engine, lane, macro2), 0.5f);
		engine.setMacro(0, 1.0f);

		std::array<float, 64> l {};
		std::array<float, 64> r {};
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		const float resolved = base + 0.5f * 1.0f * 1.0f;
		REQUIRE(engine.resolvedFieldForTest(lane, macro2) == doctest::Approx(resolved));
		CHECK(engine.publishedOutputAt(lane, 1) == doctest::Approx(resolved));
		// NOT VACUOUS: the base is far from the resolved value, so publishing the base fails the line above.
		CHECK(engine.publishedOutputAt(lane, 1) != doctest::Approx(base));
		// AND ONLY OUTPUT 2 MOVED.
		CHECK(engine.publishedOutputAt(lane, 0) == doctest::Approx(0.6f));
		CHECK(engine.publishedOutputAt(lane, 2) == doctest::Approx(0.0f));

		// BIPOLAR ON, THE SAME ROUTE: the resolved value seen as 2v − 1.
		engine.setField(lane, macros::bipolarParam(1), 1.0f);
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		CHECK(engine.publishedOutputAt(lane, 1) == doctest::Approx(2.0 * resolved - 1.0));
	}

	SUBCASE("an LFO, block by block") {
		// THE CASE THE DEVICE EXISTS FOR — another modulator riding a macro. The LFO runs free and fast, so
		// its output differs block to block, and output 2 must follow it in the same block.
		rack::RackEngine engine { kAxes, hostCatalogue() };
		const size_t lane = engine.addDevice(macros::kTraits.tag);
		const size_t lfo = engine.addDevice(mod::lfo::kTraits.tag);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		REQUIRE(lfo != rack::RackEngine::kNoSlot);
		engine.prepare(48000.0, 4096);
		const float base = 0.5f;
		engine.setField(lane, macro2, base);
		const auto lfoSet = [&](mod::lfo::Param p, double value) {
			const size_t c = static_cast<size_t>(p);
			engine.setField(lfo, c, static_cast<float>(std::max(value, mod::lfo::kDescriptor.params[c].min)));
		};
		lfoSet(mod::lfo::Param::Mode, static_cast<double>(mod::Mode::Free));
		lfoSet(mod::lfo::Param::TimeScale, -4.0);
		lfoSet(mod::lfo::Param::Smoothing, -100.0);
		const backplane::Source s = test::outputOf(engine, lfo, 0);
		const backplane::Destination to = test::controlOf(engine, lane, macro2);
		REQUIRE(engine.mayRoute(s, to));
		const float amount = 0.25f;
		engine.setRouteAmount(s, to, amount);

		std::array<float, 64> l {};
		std::array<float, 64> r {};
		size_t moved = 0;
		for (int b = 0; b < 16; ++b) {
			engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
			const float source = engine.publishedOutputAt(lfo, 0);
			const double expected = std::clamp(static_cast<double>(base) + amount * source, 0.0, 1.0);
			CAPTURE(b);
			CHECK(engine.publishedOutputAt(lane, 1) == doctest::Approx(expected));
			moved += std::abs(expected - base) > 1e-3 ? 1 : 0;
		}
		// NOT VACUOUS: the LFO displaced the macro in most blocks, so publishing the base fails above.
		CHECK(moved >= 8);
	}
}

TEST_CASE("a disabled MACROS publishes rest on every output, and keeps running underneath") {
	// THE HOST GATES AT THE PUBLISH (`RackEngine::advanceModulators`): a disabled modulator's every output
	// reads 0 — additive modulation, so every destination sits at its own base. The device itself is not
	// told, so its own `outputAt` still reports the resolved value, and switching back on lands there.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t lane = engine.addDevice(macros::kTraits.tag);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	const size_t macro2 = macros::macroParam(1);
	for (size_t n = 0; n < macros::kMacroCount; ++n) {
		engine.setField(lane, macros::macroParam(n), kValues[n]);
	}
	engine.setRouteAmount(test::macro(0), test::controlOf(engine, lane, macro2), 0.25f);
	engine.setMacro(0, 1.0f);
	const float resolved2 = kValues[1] + 0.25f;

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	// ENABLED FIRST, so the zeros below are the switch's and not a device that never published.
	REQUIRE(engine.publishedOutputAt(lane, 1) == doctest::Approx(resolved2));
	REQUIRE(engine.publishedOutputAt(lane, 3) == doctest::Approx(kValues[3]));

	engine.setField(lane, static_cast<size_t>(macros::Param::Enabled), 0.0f);
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	const void* storage = engine.slotAtLane(lane).storage.get();
	REQUIRE(storage != nullptr);
	for (size_t n = 0; n < macros::kMacroCount; ++n) {
		CAPTURE(n);
		CHECK(engine.publishedOutputAt(lane, n) == 0.0f);
		// THE DEVICE KEEPS RUNNING: its own answer is the resolved value, route included.
		CHECK(macros::kModOps.outputAt(storage, n) == doctest::Approx(n == 1 ? resolved2 : kValues[n]));
	}
}

TEST_CASE("MACROS is offered in the modulator rail and the rack") {
	const device::Descriptor* d = deviceForTag("MACROS");
	REQUIRE(d != nullptr);
	CHECK(std::string(d->traits.name) == "Macros");
	CHECK(d->outputCount() == macros::kMacroCount);

	// FEATHERS' MODULATOR RAIL: the picker's own predicate, and the strip's own seat.
	CHECK(maySitInModRack(*d));
	CHECK(isTaggedModulator(*d));
	ModRack strip {};
	CHECK(addRackDevice(&strip, "MACROS") != kNoPosition);

	// THE RACK: its browser lists it, and its engine seats it.
	bool listed = false;
	for (const rack::BrowserGroup& g : rack::browseDevices(kDevices)) {
		for (const rack::BrowserEntry& e : g.entries) {
			listed = listed || device::sameTag(e.deviceTag, "MACROS");
		}
	}
	CHECK(listed);
	rack::RackEngine engine { kAxes, hostCatalogue() };
	CHECK(engine.addDevice("MACROS") != rack::RackEngine::kNoSlot);
}
