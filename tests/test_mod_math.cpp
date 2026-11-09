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

#include "mod/math/math.h"
#include "mod/math/math_types.h"
#include "plugin/device_registry.h"
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

// THE MATH MODULATOR — `A op B`, clamped, mapped, scaled.
//
// **THE REFERENCE IS RESTATED HERE, NOT SHARED.** `reference()` below is the design written out again in
// doubles and if/else, from the words (op, clamp to the output's range, bend |r| keeping the sign, times
// amount) — so a mistake in `evaluate`'s order or in one of its curves disagrees with it rather than
// being copied into it. Tolerances are ABSOLUTE: doctest's `Approx` adds a scale of 1.0 to its epsilon,
// which passes a small wrong value against a small target.

using namespace feathers;
namespace fm = feathers::mod::math;

namespace {

constexpr double kTol = 1e-6;
/// THE φ MAP'S TOLERANCE — float phases against a double restatement with the true φ, at up to 128 periods.
constexpr double kPhiTol = 2e-3;

/// **THE φ-TRIANGLE CURVE, RESTATED BY HAND** (backlog PY) from its words: the house unipolar triangle
/// (0 → 1 → 0 over a cycle) read at P·u plus a warp of two centred house triangles at P·φ⁻¹·u and P·φ^−½·u,
/// depths 0.2 and 0.12. In doubles, with φ from its definition — not `phiTriangleCurve`, not `PhiTriContext`.
double triRestated(double p) {
	const double x = p - std::floor(p);
	return x < 0.5 ? 2.0 * x : 2.0 * (1.0 - x);
}
double phiRestated(double u, double periods) {
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	const double warp = 0.2 * (2.0 * triRestated(periods / phi * u + 0.25) - 1.0)
	                    + 0.12 * (2.0 * triRestated(periods / std::sqrt(phi) * u + 0.25) - 1.0);
	return triRestated(periods * u + warp);
}

/// THE DESIGN, RESTATED. Exponent 2 is the provisional `kMapExponent`, written as a square on purpose.
double reference(double a, double b, int op, int map, double amount, bool bipolar) {
	double r = 0.0;
	if (op == 0) {
		r = a * b;
	} else if (op == 1) {
		r = a + b;
	} else if (op == 2) {
		r = a - b;
	} else if (op == 3) {
		r = a < b ? a : b;
	} else if (op == 4) {
		r = a > b ? a : b;
	} else if (op == 5) {
		r = a > b ? 1.0 : 0.0;
	} else if (op == 6) {
		r = a < b ? 1.0 : 0.0;
	} else if (op == 7) {
		r = a >= b ? 1.0 : 0.0;
	} else {
		r = a <= b ? 1.0 : 0.0;
	}
	const double lo = bipolar ? -1.0 : 0.0;
	if (r < lo) {
		r = lo;
	}
	if (r > 1.0) {
		r = 1.0;
	}
	const double m = r < 0.0 ? -r : r;
	double bent = m;
	if (map == 1) {
		bent = 1.0 - (1.0 - m) * (1.0 - m);
	} else if (map == 2) {
		bent = m * m;
	} else if (map == 3) {
		bent = phiRestated(m, std::exp2(4.0)); // the Periods control's default, 16
	}
	if (r < 0.0) {
		bent = -bent;
	}
	return bent * amount;
}

float eval(double a, double b, fm::Op op, fm::Map map, double amount, bool bipolar) {
	return fm::evaluate(static_cast<float>(a), static_cast<float>(b), op, map, static_cast<float>(amount), bipolar);
}

} // namespace

TEST_CASE("every op, map and polarity on a grid agrees with the design restated") {
	static_assert(fm::kMapExponent == 2.0f, "the reference squares; restate it if the ear moves the exponent");
	size_t points = 0;
	size_t misses = 0;
	for (int op = 0; op < static_cast<int>(fm::Op::kCount); ++op) {
		for (int map = 0; map < static_cast<int>(fm::Map::kCount); ++map) {
			for (const bool bipolar : { false, true }) {
				for (const double amount : { 0.0, 0.5, 1.0 }) {
					for (int ia = 0; ia <= 16; ++ia) {
						for (int ib = 0; ib <= 16; ++ib) {
							const double a = -1.0 + 0.125 * ia;
							const double b = -1.0 + 0.125 * ib;
							const double want = reference(a, b, op, map, amount, bipolar);
							const double got = eval(a, b, static_cast<fm::Op>(op), static_cast<fm::Map>(map), amount,
							                        bipolar);
							++points;
							if (std::abs(got - want) > (map == 3 ? kPhiTol : kTol)) {
								++misses;
								CAPTURE(op);
								CAPTURE(map);
								CAPTURE(bipolar);
								CAPTURE(amount);
								CAPTURE(a);
								CAPTURE(b);
								CHECK(got == want);
							}
						}
					}
				}
			}
		}
	}
	CHECK(points == 9u * 4u * 2u * 3u * 17u * 17u);
	CHECK(misses == 0u);
}

TEST_CASE("unipolar clamps negatives to zero") {
	for (int op = 0; op < static_cast<int>(fm::Op::kCount); ++op) {
		CAPTURE(op);
		// A COMPARISON IS NEVER NEGATIVE (0 or 1) — its own case holds it.
		if (fm::isComparison(static_cast<fm::Op>(op))) {
			continue;
		}
		// INPUTS WHOSE RESULT IS NEGATIVE UNDER THIS OPERATOR — two negatives for every one but the
		// product, whose negative needs mixed signs — so every one must land on exactly 0.
		const double b = static_cast<fm::Op>(op) == fm::Op::Multiply ? 0.5 : -0.25;
		CHECK(eval(-0.5, b, static_cast<fm::Op>(op), fm::Map::Lin, 1.0, false) == 0.0f);
		// AND BIPOLAR KEEPS IT — the same inputs are not zero there, so the line above is the clamp's.
		CHECK(eval(-0.5, b, static_cast<fm::Op>(op), fm::Map::Lin, 1.0, true) < -0.1f);
	}
}

TEST_CASE("sum clamps at one") {
	CHECK(eval(0.75, 0.75, fm::Op::Add, fm::Map::Lin, 1.0, true) == 1.0f);
	CHECK(eval(0.75, 0.75, fm::Op::Add, fm::Map::Lin, 1.0, false) == 1.0f);
	CHECK(eval(-0.75, -0.75, fm::Op::Add, fm::Map::Lin, 1.0, true) == -1.0f);
	CHECK(eval(1.0, -1.0, fm::Op::Subtract, fm::Map::Lin, 1.0, true) == 1.0f);
	// CLAMPED BEFORE THE AMOUNT, so half an amount of a saturated sum is a half, not 0.75.
	CHECK(std::abs(eval(0.75, 0.75, fm::Op::Add, fm::Map::Lin, 0.5, true) - 0.5) <= kTol);
}

TEST_CASE("map keeps the sign") {
	for (int map = 0; map < static_cast<int>(fm::Map::kCount); ++map) {
		CAPTURE(map);
		const float up = eval(0.5, 0.0, fm::Op::Add, static_cast<fm::Map>(map), 1.0, true);
		const float down = eval(-0.5, 0.0, fm::Op::Add, static_cast<fm::Map>(map), 1.0, true);
		CHECK(up > 0.0f);
		CHECK(down == -up);
	}
}

TEST_CASE("amount zero silences") {
	for (int op = 0; op < static_cast<int>(fm::Op::kCount); ++op) {
		for (int map = 0; map < static_cast<int>(fm::Map::kCount); ++map) {
			CAPTURE(op);
			CAPTURE(map);
			CHECK(eval(0.7, -0.4, static_cast<fm::Op>(op), static_cast<fm::Map>(map), 0.0, true) == 0.0f);
		}
	}
}

TEST_CASE("pinned values") {
	CHECK(std::abs(eval(0.5, 0.5, fm::Op::Multiply, fm::Map::Lin, 1.0, true) - 0.25) <= kTol);
	CHECK(std::abs(eval(0.5, 0.0, fm::Op::Add, fm::Map::Exp, 1.0, true) - 0.25) <= kTol);
	CHECK(std::abs(eval(0.5, 0.0, fm::Op::Add, fm::Map::Log, 1.0, true) - 0.75) <= kTol);
	CHECK(eval(-0.5, 0.0, fm::Op::Add, fm::Map::Lin, 1.0, false) == 0.0f);
	// AN OPERATOR OR MAP NO MENU CAN PRODUCE IS SILENCE.
	CHECK(eval(0.5, 0.5, fm::Op::kCount, fm::Map::Lin, 1.0, true) == 0.0f);
	CHECK(eval(0.5, 0.5, fm::Op::Add, fm::Map::kCount, 1.0, true) == 0.0f);
}

TEST_CASE("an operator or map past its menu answers zero") {
	// EVERY OTHER INPUT CHOSEN TO BE LOUD — 0.5 and 0.5 give 0.25 to 1.0 under every real operator — so a
	// zero here is the out-of-range branch and not the arithmetic.
	for (const int bad : { static_cast<int>(fm::Op::kCount), 200, 255 }) {
		CAPTURE(bad);
		CHECK(eval(0.5, 0.5, static_cast<fm::Op>(bad), fm::Map::Lin, 1.0, true) == 0.0f);
		CHECK(eval(0.5, 0.5, fm::Op::Add, static_cast<fm::Map>(bad), 1.0, true) == 0.0f);
	}
	CHECK(eval(0.5, 0.5, fm::Op::Add, fm::Map::Lin, 1.0, true) == 1.0f);
}

TEST_CASE("unipolar Log and Exp on a negative result are zero") {
	// THE CLAMP COMES BEFORE THE MAP: a negative result is 0 before Log bends it, and Log's 1 − (1 − 0)²
	// is 0 — where bending |r| first and clamping after would have published nothing either way for Exp
	// but 0.75 for Log on −0.5 had the sign been dropped.
	for (const fm::Map map : { fm::Map::Log, fm::Map::Exp }) {
		CAPTURE(static_cast<int>(map));
		CHECK(eval(-0.5, 0.0, fm::Op::Add, map, 1.0, false) == 0.0f);
		CHECK(eval(0.25, 0.75, fm::Op::Subtract, map, 1.0, false) == 0.0f);
		// AND BIPOLAR, THE SAME INPUTS ARE NOT ZERO — so the lines above are the polarity's.
		CHECK(eval(-0.5, 0.0, fm::Op::Add, map, 1.0, true) < -0.2f);
	}
}

// ---------------------------------------------------------------------------------------------------
// THE DEVICE.

namespace {

/// THE DEVICE THROUGH ITS OPS — the table a host reaches it by.
struct Ops {
	void* mem = nullptr;
	Ops() {
		const device::ModOps& mod = fm::kModOps;
		const size_t align = std::max<size_t>(mod.stateAlign, alignof(std::max_align_t));
		mem = feathers::test::heapAlignedAlloc(align, ((mod.stateBytes + align - 1) / align) * align);
		REQUIRE(mem != nullptr);
		mod.construct(mem);
		mod.prepare(mem, 48000.0);
	}
	~Ops() {
		fm::kModOps.destruct(mem);
		feathers::test::heapFree(mem);
	}
	Ops(const Ops&) = delete;
	Ops& operator=(const Ops&) = delete;

	/// Hand the row over and tick once, as the host does, then read the output.
	void set(const std::vector<float>& row) {
		fm::kModOps.setFields(mem, row.data(), row.size());
		device::ModTickContext ctx;
		fm::kModOps.tick(mem, ctx);
	}
	[[nodiscard]] float out(size_t n = 0) const { return fm::kModOps.outputAt(mem, n); }
};

std::vector<float> defaults() {
	std::vector<float> row;
	for (const device::ParamDesc& p : fm::kDescriptor.params) {
		row.push_back(static_cast<float>(p.def));
	}
	return row;
}

float& field(std::vector<float>& row, fm::Param p) { return row[static_cast<size_t>(p)]; }

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 4,
	.outputStride = 8,
};

} // namespace

TEST_CASE("the device's output is evaluate() of its fields") {
	// EVERY OPERATOR, MAP AND POLARITY, at a half Amount and distinct A and B — so a decode that dropped a
	// field, swapped A for B, read the wrong menu or ignored the Amount answers differently from the call.
	// `evaluate` itself is held to the design by the grid above; this case is about the row reaching it.
	Ops ops;
	for (int op = 0; op < static_cast<int>(fm::Op::kCount); ++op) {
		for (int map = 0; map < static_cast<int>(fm::Map::kCount); ++map) {
			for (const bool bipolar : { false, true }) {
				std::vector<float> row = defaults();
				field(row, fm::Param::A) = 0.8f;
				field(row, fm::Param::B) = -0.3f;
				field(row, fm::Param::Operator) = static_cast<float>(op);
				field(row, fm::Param::Map) = static_cast<float>(map);
				field(row, fm::Param::Amount) = 0.5f;
				field(row, fm::Param::Bipolar) = bipolar ? 1.0f : 0.0f;
				ops.set(row);
				CAPTURE(op);
				CAPTURE(map);
				CAPTURE(bipolar);
				CHECK(ops.out() == fm::evaluate(0.8f, -0.3f, static_cast<fm::Op>(op), static_cast<fm::Map>(map), 0.5f,
				                                bipolar));
				// AND THE DESIGN RESTATED, for the value rather than only the call.
				CHECK(std::abs(ops.out() - reference(0.8, -0.3, op, map, 0.5, bipolar)) <= (map == 3 ? kPhiTol : kTol));
			}
		}
	}
	// THE DEFAULT ROW IS SILENT: A and B rest at 0, and every operator of two zeros is zero.
	ops.set(defaults());
	CHECK(ops.out() == 0.0f);
	// ONE OUTPUT, and nothing past it.
	CHECK(fm::kDescriptor.outputCount() == 1u);
	CHECK(ops.out(1) == 0.0f);
}

TEST_CASE("a NaN field reads as its default and the output stays a number") {
	Ops ops;
	const float nan = std::nanf("");
	// A NaN A over B = 0.5, Add: the default A is 0, so the output is B.
	std::vector<float> row = defaults();
	field(row, fm::Param::A) = nan;
	field(row, fm::Param::B) = 0.5f;
	field(row, fm::Param::Operator) = static_cast<float>(fm::Op::Add);
	ops.set(row);
	CHECK(std::abs(ops.out() - 0.5) <= kTol);
	// EVERY FIELD NaN: the default row, which is silent — and a number.
	std::vector<float> all(defaults().size(), nan);
	ops.set(all);
	CHECK(ops.out() == 0.0f);
	// NaN MENUS fall to their defaults (×, Lin): 0.5 × 0.5.
	row = defaults();
	field(row, fm::Param::A) = 0.5f;
	field(row, fm::Param::B) = 0.5f;
	field(row, fm::Param::Operator) = nan;
	field(row, fm::Param::Map) = nan;
	ops.set(row);
	CHECK(std::abs(ops.out() - 0.25) <= kTol);
}

TEST_CASE("M1 into A and M2 into B, operator ×, gives M1·M2 at the output") {
	// **THE PUBLISHED OUTPUT IS COMPUTED FROM THE RESOLVED A AND B, NOT THEIR BASES** — asserted at the
	// engine, where the resolve happens. A and B span −1..1, so a route's full depth moves them by 2: at
	// amount 0.5 a macro's value lands on the input exactly (base 0 + 0.5 × m × 2).
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t lane = engine.addDevice(fm::kTraits.tag);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	engine.setField(lane, static_cast<size_t>(fm::Param::Operator), static_cast<float>(fm::Op::Multiply));

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	// UNROUTED FIRST — the rest the routed value must differ from.
	engine.setMacro(0, 0.6f);
	engine.setMacro(1, 0.7f);
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	const float unrouted = engine.publishedOutputAt(lane, 0);
	CHECK(unrouted == 0.0f);

	const size_t a = static_cast<size_t>(fm::Param::A);
	const size_t b = static_cast<size_t>(fm::Param::B);
	engine.setRouteAmount(test::macro(0), test::controlOf(engine, lane, a), 0.5f);
	engine.setRouteAmount(test::macro(1), test::controlOf(engine, lane, b), 0.5f);
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());

	// RESTATED: each input is base + amount × source × span, then the product.
	const double m1 = 0.6;
	const double m2 = 0.7;
	const double resolvedA = 0.0 + 0.5 * m1 * 2.0;
	const double resolvedB = 0.0 + 0.5 * m2 * 2.0;
	REQUIRE(std::abs(engine.resolvedFieldForTest(lane, a) - resolvedA) <= kTol);
	REQUIRE(std::abs(engine.resolvedFieldForTest(lane, b) - resolvedB) <= kTol);
	const double want = resolvedA * resolvedB; // 0.42 — bipolar on, Lin, Amount 1: the defaults
	CHECK(std::abs(engine.publishedOutputAt(lane, 0) - want) <= kTol);
	// NOT VACUOUS: the routed output is far from the unrouted one.
	CHECK(std::abs(engine.publishedOutputAt(lane, 0) - unrouted) > 0.1f);
}

TEST_CASE("disabled publishes rest") {
	// THE HOST GATES AT THE PUBLISH (`RackEngine::advanceModulators`), as for MACROS: disabled, the output
	// reads 0 while the device's own answer is still the row's.
	rack::RackEngine engine { kAxes, hostCatalogue() };
	const size_t lane = engine.addDevice(fm::kTraits.tag);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	engine.setField(lane, static_cast<size_t>(fm::Param::A), 0.5f);
	engine.setField(lane, static_cast<size_t>(fm::Param::B), 0.25f);
	engine.setField(lane, static_cast<size_t>(fm::Param::Operator), static_cast<float>(fm::Op::Add));
	const double want = 0.75; // 0.5 + 0.25, Lin, Amount 1

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	// ENABLED FIRST, so the zero below is the switch's and not a device that never published.
	REQUIRE(std::abs(engine.publishedOutputAt(lane, 0) - want) <= kTol);

	engine.setField(lane, static_cast<size_t>(fm::Param::Enabled), 0.0f);
	engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
	CHECK(engine.publishedOutputAt(lane, 0) == 0.0f);
	const void* storage = engine.slotAtLane(lane).storage.get();
	REQUIRE(storage != nullptr);
	CHECK(std::abs(fm::kModOps.outputAt(storage, 0) - want) <= kTol);
}

TEST_CASE("MATH is offered in the modulator rail and the rack") {
	const device::Descriptor* d = deviceForTag("MATH");
	REQUIRE(d != nullptr);
	CHECK(std::string(d->traits.name) == "Math");
	// NO HOST AMOUNT — `evaluate` applies Amount; a declared `amountControl` would scale it twice.
	CHECK(d->traits.amountControl == -1);
	CHECK(maySitInModRack(*d));
	CHECK(isTaggedModulator(*d));
	ModRack strip {};
	CHECK(addRackDevice(&strip, "MATH") != kNoPosition);
	bool listed = false;
	for (const rack::BrowserGroup& g : rack::browseDevices(kDevices)) {
		for (const rack::BrowserEntry& e : g.entries) {
			listed = listed || device::sameTag(e.deviceTag, "MATH");
		}
	}
	CHECK(listed);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG PY — THE COMPARISONS AND THE φ MAP.                                                        //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("PY: the comparisons are a 0/1 gate — at, above and below the threshold, either polarity") {
	struct Row {
		fm::Op op;
		double below; // A < B
		double at;    // A == B
		double above; // A > B
	};
	for (const Row r : { Row { fm::Op::Greater, 0.0, 0.0, 1.0 }, Row { fm::Op::Less, 1.0, 0.0, 0.0 },
	                     Row { fm::Op::GreaterEq, 0.0, 1.0, 1.0 }, Row { fm::Op::LessEq, 1.0, 1.0, 0.0 } }) {
		CAPTURE(static_cast<int>(r.op));
		CHECK(fm::isComparison(r.op));
		for (const bool bipolar : { false, true }) {
			CAPTURE(bipolar);
			// THE THRESHOLD IS B, at a negative, a zero and a positive value; A a hair either side and on it.
			for (const float b : { -0.4f, 0.0f, 0.3f }) {
				CAPTURE(b);
				CHECK(eval(b - 0.01f, b, r.op, fm::Map::Lin, 1.0, bipolar) == static_cast<float>(r.below));
				CHECK(eval(b, b, r.op, fm::Map::Lin, 1.0, bipolar) == static_cast<float>(r.at));
				CHECK(eval(b + 0.01f, b, r.op, fm::Map::Lin, 1.0, bipolar) == static_cast<float>(r.above));
			}
			// AMOUNT SCALES THE GATE.
			CHECK(eval(0.9, 0.1, r.op, fm::Map::Lin, 0.5, bipolar) == static_cast<float>(0.5 * r.above));
		}
	}
	for (const fm::Op op : { fm::Op::Multiply, fm::Op::Add, fm::Op::Subtract, fm::Op::Min, fm::Op::Max }) {
		CHECK_FALSE(fm::isComparison(op));
	}
	// STORED BY LABEL, so the four appended rows are new labels and the old five keep theirs.
	CHECK(std::string(fm::kOpNames[static_cast<size_t>(fm::Op::Greater)]) == ">");
	CHECK(std::string(fm::kOpNames[static_cast<size_t>(fm::Op::LessEq)]) == "<=");
	CHECK(fm::kParamDescs[static_cast<size_t>(fm::Param::Operator)].max == static_cast<double>(fm::Op::kCount) - 1.0);
}

TEST_CASE("PY: Log and Exp bend by the Exponent control, through the device's row — restated at several exponents") {
	Ops ops;
	for (const int map : { 1, 2 }) {
		for (const float k : { 1.0f, 2.0f, 3.3f, 8.0f }) {
			for (const float a : { 0.2f, 0.5f, -0.7f }) {
				CAPTURE(map);
				CAPTURE(k);
				CAPTURE(a);
				std::vector<float> row = defaults();
				field(row, fm::Param::A) = a;
				field(row, fm::Param::Operator) = static_cast<float>(fm::Op::Add);
				field(row, fm::Param::Map) = static_cast<float>(map);
				field(row, fm::Param::Exponent) = k;
				ops.set(row);
				const double m = std::abs(static_cast<double>(a));
				const double bent = map == 1 ? 1.0 - std::pow(1.0 - m, static_cast<double>(k))
				                             : std::pow(m, static_cast<double>(k));
				CHECK(std::abs(static_cast<double>(ops.out()) - (a < 0.0f ? -bent : bent)) <= 1e-6);
			}
		}
	}
	const device::ParamDesc& p = fm::kParamDescs[static_cast<size_t>(fm::Param::Exponent)];
	CHECK(std::string(p.key) == "exponent");
	CHECK(p.min == 1.0);
	CHECK(p.max == 8.0);
	CHECK(p.def == 2.0);
	CHECK(device::relevantFor(p, static_cast<double>(fm::Map::Log)));
	CHECK(device::relevantFor(p, static_cast<double>(fm::Map::Exp)));
	CHECK_FALSE(device::relevantFor(p, static_cast<double>(fm::Map::Lin)));
	CHECK_FALSE(device::relevantFor(p, static_cast<double>(fm::Map::Phi)));
}

TEST_CASE("PY: the φ map is the house φ-triangle restated, at the Periods control, through the device's row") {
	Ops ops;
	for (const float octaves : { 0.0f, 2.5f, 4.0f, 7.0f }) {
		CAPTURE(octaves);
		for (const float a : { 0.03f, 0.21f, 0.5f, 0.77f, 0.999f, -0.61f }) {
			CAPTURE(a);
			std::vector<float> row = defaults();
			field(row, fm::Param::A) = a;
			field(row, fm::Param::B) = 0.0f;
			field(row, fm::Param::Operator) = static_cast<float>(fm::Op::Add);
			field(row, fm::Param::Map) = static_cast<float>(fm::Map::Phi);
			field(row, fm::Param::Periods) = octaves;
			field(row, fm::Param::Amount) = 1.0f;
			field(row, fm::Param::Bipolar) = 1.0f;
			ops.set(row);
			const double m = std::abs(static_cast<double>(a));
			const double want = (a < 0.0f ? -1.0 : 1.0) * phiRestated(m, std::exp2(static_cast<double>(octaves)));
			CHECK(std::abs(static_cast<double>(ops.out()) - want) <= kPhiTol);
		}
	}
	// THE PERIODS REACH THE OUTPUT: the same A at two periods answers two different values (and each is the
	// restated one above — this is only the proof the first assertion was not vacuous).
	std::vector<float> row = defaults();
	field(row, fm::Param::A) = 0.37f;
	field(row, fm::Param::Operator) = static_cast<float>(fm::Op::Add);
	field(row, fm::Param::Map) = static_cast<float>(fm::Map::Phi);
	field(row, fm::Param::Periods) = 1.0f;
	ops.set(row);
	const float lowPeriods = ops.out();
	field(row, fm::Param::Periods) = 6.0f;
	ops.set(row);
	CHECK(ops.out() != lowPeriods);
	// THE CONTROL: 0..7 octaves, default 4 (16 periods), gated to the φ map and not modulatable.
	const device::ParamDesc& p = fm::kParamDescs[static_cast<size_t>(fm::Param::Periods)];
	CHECK(std::string(p.key) == "phi_periods");
	CHECK(p.min == 0.0);
	CHECK(p.max == 7.0);
	CHECK(p.def == 4.0);
	CHECK_FALSE(p.modulatable);
	CHECK(device::relevantFor(p, static_cast<double>(fm::Map::Phi)));
	CHECK_FALSE(device::relevantFor(p, static_cast<double>(fm::Map::Lin)));
}
