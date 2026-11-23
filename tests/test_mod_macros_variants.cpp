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
#include "doctest.h"

#include "mod/macro1/macro1_types.h"
#include "mod/macros/macros.h"
#include "mod/macros8/macros8_types.h"
#include "persist_rig.h"
#include "plugin/device_registry.h"
#include "rack_endpoints.h"

#include <array>
#include <cmath>
#include <string>
#include <vector>

using namespace feathers;
namespace mc = feathers::mod::macros;

namespace {

struct Variant {
	const device::Descriptor* d;
	size_t n;
};

[[nodiscard]] std::vector<Variant> variants() {
	return { { &mod::macro1::kDescriptor, 1 }, { &mod::macros8::kDescriptor, 8 } };
}

/// THE DEFAULT ROW, then macro n set to a value of this test's own choosing (distinct per macro).
[[nodiscard]] float valueOf(size_t n, size_t count) { return static_cast<float>(n + 1) / static_cast<float>(count + 1); }

[[nodiscard]] std::vector<float> rowFor(const Variant& v, const std::vector<bool>& bipolar) {
	std::vector<float> row;
	for (const device::ParamDesc& p : v.d->params) {
		row.push_back(static_cast<float>(p.def));
	}
	for (size_t i = 0; i < v.n; ++i) {
		row[mc::macroField(i)] = valueOf(i, v.n);
		row[mc::bipolarField(v.n, i)] = bipolar[i] ? 1.0f : 0.0f;
	}
	return row;
}

} // namespace

TEST_CASE("the variants declare the four-macro device's conventions at their own count") {
	for (const Variant& v : variants()) {
		CAPTURE(v.n);
		REQUIRE(v.d->params.size() == 3 * v.n + 1);
		REQUIRE(v.d->outputCount() == v.n);
		CHECK(v.d->category == device::Category::Modulators);
		for (size_t i = 0; i < v.n; ++i) {
			const std::string k = std::to_string(i + 1);
			CHECK(std::string(v.d->params[mc::macroField(i)].key) == "macro_" + k);
			CHECK(std::string(v.d->params[mc::bipolarField(v.n, i)].key) == "bipolar_" + k);
			CHECK(std::string(v.d->params[mc::typeField(v.n, i)].key) == "type_" + k);
			CHECK(std::string(v.d->outputs[i].key) == "macro_" + k);
			CHECK(v.d->params[mc::macroField(i)].modulatable);
			CHECK_FALSE(v.d->params[mc::bipolarField(v.n, i)].modulatable);
			CHECK(v.d->outputs[i].bipolarControl == static_cast<int>(mc::bipolarField(v.n, i)));
		}
		CHECK(std::string(v.d->params[mc::enableField(v.n)].key) == "enabled");
		CHECK(v.d->traits.enableControl == static_cast<int>(mc::enableField(v.n)));
		REQUIRE(deviceForTag(v.d->traits.tag) != nullptr);
	}
	// THE FOUR'S LAYOUT IS THE FORMULA AT FOUR: its declared order is what the shared code assumes.
	CHECK(static_cast<size_t>(mc::Param::Bipolar1) == mc::bipolarField(4, 0));
	CHECK(static_cast<size_t>(mc::Param::Type4) == mc::typeField(4, 3));
	CHECK(static_cast<size_t>(mc::Param::Enabled) == mc::enableField(4));
}

TEST_CASE("each variant output is its macro's value, through its own polarity") {
	for (const Variant& v : variants()) {
		for (int pass = 0; pass < 2; ++pass) {
			// pass 0: alternate polarity (a mapping that ignored or shared the flag is wrong somewhere); pass 1: all bipolar
			std::vector<bool> bip(v.n);
			for (size_t i = 0; i < v.n; ++i) {
				bip[i] = pass == 1 || i % 2 == 1;
			}
			CAPTURE(v.n);
			CAPTURE(pass);
			persist::Rig rig;
			rig.seat(v.d->traits.tag, rowFor(v, bip));
			rig.block();
			const size_t lane = rig.engine.slotOf(rig.id);
			for (size_t i = 0; i < v.n; ++i) {
				CAPTURE(i);
				const double value = valueOf(i, v.n);
				const double want = bip[i] ? 2.0 * value - 1.0 : value;
				CHECK(rig.engine.publishedOutputAt(lane, i) == doctest::Approx(want));
			}
		}
	}
}

TEST_CASE("a route into the LAST macro moves that output and no other") {
	// AT THE ENGINE, as MACROS' own route case drives it: the published output is the resolved value.
	for (const Variant& v : variants()) {
		CAPTURE(v.n);
		rack::RackEngine engine { persist::Rig::kAxes, hostCatalogue() };
		const size_t lane = engine.addDevice(v.d->traits.tag);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		engine.prepare(48000.0, 4096);
		const size_t last = v.n - 1;
		const std::vector<float> row = rowFor(v, std::vector<bool>(v.n, false));
		for (size_t c = 0; c < row.size(); ++c) {
			engine.setField(lane, c, row[c]);
		}
		engine.setRouteAmount(test::macro(0), test::controlOf(engine, lane, mc::macroField(last)), 0.05f);
		engine.setMacro(0, 1.0f);
		std::array<float, 64> l {};
		std::array<float, 64> r {};
		engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		const double base = valueOf(last, v.n);
		CHECK(engine.publishedOutputAt(lane, last) == doctest::Approx(base + 0.05));
		// NOT VACUOUS: the route moved it off its base.
		CHECK(engine.publishedOutputAt(lane, last) != doctest::Approx(base).epsilon(1e-6));
		for (size_t i = 0; i < last; ++i) {
			CAPTURE(i);
			CHECK(engine.publishedOutputAt(lane, i) == doctest::Approx(valueOf(i, v.n)));
		}
	}
}

TEST_CASE("a variant's patch survives save and load into a fresh instance") {
	for (const Variant& v : variants()) {
		CAPTURE(v.n);
		std::vector<bool> bip(v.n);
		for (size_t i = 0; i < v.n; ++i) {
			bip[i] = i % 2 == 0;
		}
		std::vector<float> row = rowFor(v, bip);
		for (size_t i = 0; i < v.n; ++i) {
			row[mc::typeField(v.n, i)] = static_cast<float>(i % 3);
		}
		persist::Rig first;
		first.seat(v.d->traits.tag, row);
		first.block();
		const std::string text = first.save();
		// BY KEY: the document names this device's keys, so a save is meaningful beside the ledger.
		CHECK(text.find("\"tag\": \"" + std::string(v.d->traits.tag) + "\"") != std::string::npos);
		CHECK(text.find("\"macro_" + std::to_string(v.n) + "\"") != std::string::npos);

		persist::Rig second;
		second.load(text);
		second.block();
		const size_t lane = second.engine.slotOf(second.id);
		for (size_t c = 0; c < v.d->params.size(); ++c) {
			CAPTURE(c);
			CHECK(second.bridge->fieldAt(second.id, c) == doctest::Approx(row[c]));
		}
		for (size_t i = 0; i < v.n; ++i) {
			CAPTURE(i);
			const double value = valueOf(i, v.n);
			CHECK(second.engine.publishedOutputAt(lane, i) == doctest::Approx(bip[i] ? 2.0 * value - 1.0 : value));
		}
	}
}

TEST_CASE("a stored four-macro MACROS patch loads exactly as it did before the variants") {
	// A LITERAL, written from the key names MACROS has always stored — not produced by this build's writer.
	const std::string patch = R"({
	"format": "feathers.rack",
	"major": 1,
	"chains": [
		[
			{
				"tag": "MACROS",
				"id": 1,
				"schema": 1,
				"view": 0,
				"values": {
					"macro_1": 0.25,
					"macro_2": 0,
					"macro_3": 0.75,
					"macro_4": 0,
					"bipolar_1": "Off",
					"bipolar_2": "On",
					"bipolar_3": "Off",
					"bipolar_4": "Off",
					"type_1": "Knob",
					"type_2": "Momentary",
					"type_3": "Knob",
					"type_4": "Knob",
					"enabled": "On"
				},
				"body": null
			}
		]
	]
})";
	persist::Rig rig;
	rig.load(patch);
	rig.block();
	const device::Descriptor& d = mod::macros::kDescriptor;
	REQUIRE(std::string(d.traits.tag) == "MACROS");
	const auto field = [&](mc::Param p) { return rig.bridge->fieldAt(rig.id, static_cast<size_t>(p)); };
	CHECK(field(mc::Param::Macro1) == doctest::Approx(0.25));
	CHECK(field(mc::Param::Macro2) == doctest::Approx(0.0));
	CHECK(field(mc::Param::Macro3) == doctest::Approx(0.75));
	CHECK(field(mc::Param::Bipolar2) == doctest::Approx(1.0));
	CHECK(field(mc::Param::Bipolar1) == doctest::Approx(0.0));
	CHECK(field(mc::Param::Type2) == doctest::Approx(2.0)); // Momentary
	CHECK(field(mc::Param::Enabled) == doctest::Approx(1.0));
	const size_t lane = rig.engine.slotOf(rig.id);
	CHECK(rig.engine.publishedOutputAt(lane, 0) == doctest::Approx(0.25));
	CHECK(rig.engine.publishedOutputAt(lane, 1) == doctest::Approx(-1.0)); // bipolar at 0: 2*0-1
	CHECK(rig.engine.publishedOutputAt(lane, 2) == doctest::Approx(0.75));
	CHECK(rig.engine.publishedOutputAt(lane, 3) == doctest::Approx(0.0));
}

TEST_CASE("the right ear of each macro is carried at every count") {
	// THE STEREO ROAD (backlog KU) lives in the shared core: a macro marked live takes its right value
	// through its own polarity, and one not marked keeps its left in both ears.
	mc::MacrosCore<8> core;
	std::vector<float> row(3 * 8 + 1, 0.0f);
	for (size_t i = 0; i < 8; ++i) {
		row[mc::macroField(i)] = 0.5f;
		row[mc::bipolarField(8, i)] = i == 7 ? 1.0f : 0.0f;
	}
	core.setFields(row.data(), row.size());
	std::vector<float> right = row;
	right[mc::macroField(7)] = 0.9f;
	right[mc::macroField(3)] = 0.2f;
	device::ControlMask live;
	live.set(mc::macroField(7));
	core.setFieldsR(right.data(), right.size(), live);
	const device::EarPair p7 = core.pairAt(7);
	CHECK(p7.left == doctest::Approx(0.0)); // 2*0.5-1
	CHECK(p7.right == doctest::Approx(0.8)); // 2*0.9-1
	const device::EarPair p3 = core.pairAt(3);
	CHECK(p3.left == doctest::Approx(0.5));
	CHECK(p3.right == doctest::Approx(0.5)); // not live: its right ear stays where the left put it
	CHECK(core.pairAt(8).left == 0.0f);
}
