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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/arp/pattern_types.h"
#include "plugin/bridge_device_host.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/synth.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/mod_readout.h"
#include "plugin/ui/param_control.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// BACKLOG PU IN FEATHERS ITSELF: the readout's probe answered by the plugin's own block road
// (`publishEngineReadings` → `Synth::answerRouteProbe` → the lane engine), through `BridgeDeviceHost` — the host a
// Feathers card's controls hold. The motivating case exactly: a toggle macro at on into a Pattern's Scale.

using namespace feathers;
namespace pat = mod::arp::pat;

TEST_CASE("PU in Feathers: a Pattern's Scale in the strip, a macro at on, reads the label the engine plays") {
	ParamSet params;
	Synth synth { params };
	synth.activate(48000.0, 4096);
	ParamBridge bridge;
	BridgeDeviceHost host { bridge };
	const size_t slot = bridge.addRackDevice(pat::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	const DeviceInstanceId id = bridge.rackDeviceAt(slot);
	const size_t scale = pat::at(0, pat::Field::Scale);
	const device::ParamDesc& desc = pat::kDescriptor.params[scale];
	params.setBase(static_cast<clap_id>(macroParam(0)), 1.0);
	const backplane::Source macro = backplane::Source::core(0);
	ui::ModState mod;
	ui::EditHistory history;
	ui::DeviceControl c(host, id, pat::kDescriptor, scale, &mod, &history);
	std::vector<float> l(256), r(256);
	const auto block = [&] {
		(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
		publishEditorLanes(bridge, params, synth);
		synth.render(l.data(), r.data(), 256);
		publishEngineReadings(bridge, synth);
	};
	const double span = device::routeSpanOf(desc);
	for (const float amount : { 0.1f, 0.2f, 0.45f, 1.0f }) {
		CAPTURE(amount);
		host.setRouteAmount(macro, c.destination(), amount);
		(void)c.routeReadout(macro);
		block();
		const std::optional<ui::ModReadout> got = c.routeReadout(macro);
		REQUIRE(got.has_value());
		const int expected = std::clamp(static_cast<int>(std::lround(static_cast<double>(amount) * span)), 0,
		                                static_cast<int>(desc.max));
		CHECK(got->value == doctest::Approx(static_cast<double>(expected)));
		CHECK(got->text == c.textAt(static_cast<double>(expected)));
		block();
		CHECK(static_cast<double>(host.modulatedFieldAt(id, scale)) == doctest::Approx(got->value));
	}
}

TEST_CASE("PU in Feathers: a CORE parameter's readout is ParamSet's own resolve — macro curves, host-free, a clamped "
          "and a range-leaving destination") {
	// The synth's own knobs (`ParamControl`) resolve through `ParamSet`, not the lane engine: span `max − min`, a
	// macro read as a double through `throughTransfer`, no stepping, the range unless `offsetsMayLeaveRange`. The
	// readout calls `coreRouteTerm` and `landCore`, the two functions `ParamSet::resolved` calls, and the engine's
	// reference here is `ParamSet::resolved` itself.
	ParamSet params;
	Synth synth { params };
	synth.activate(48000.0, 4096);
	ParamBridge bridge;
	ui::ModState mod;
	ui::EditHistory history;
	std::vector<float> l(256), r(256);
	const auto block = [&] {
		publishEditorLanes(bridge, params, synth);
		synth.render(l.data(), r.data(), 256);
		publishEngineReadings(bridge, synth);
	};
	constexpr double kMacro = 0.6;
	params.setBase(static_cast<clap_id>(macroParam(1)), kMacro);
	bridge.setValue(static_cast<clap_id>(macroParam(1)), kMacro);
	const backplane::Source macro = backplane::Source::core(1);
	for (const ParamId id : { ParamId::FineTune, ParamId::OscPhase }) {
		const ParamDesc* found = findParam(static_cast<clap_id>(id));
		REQUIRE(found != nullptr);
		const ParamDesc& desc = *found;
		const int destIndex = modDestinationIndexOf(id);
		REQUIRE(destIndex >= 0);
		const backplane::Destination dest = backplane::Destination::core(static_cast<size_t>(destIndex));
		ui::ParamControl c(bridge, &desc, &mod, &history);
		const double span = desc.max - desc.min;
		const double base = desc.min + 0.75 * span;
		params.setBase(static_cast<clap_id>(id), base);
		bridge.setValue(static_cast<clap_id>(id), base);
		for (const device::Transfer t : { device::Transfer::Lin, device::Transfer::Exp, device::Transfer::Log }) {
			for (const float amount : { 0.2f, 0.6f, -0.9f }) {
				CAPTURE(static_cast<int>(id));
				CAPTURE(static_cast<int>(t));
				CAPTURE(amount);
				bridge.setRouteAmount(macro, dest, amount);
				REQUIRE(bridge.setRouteTransfer(macro, dest, t));
				(void)c.routeReadout(macro);
				block();
				const std::optional<ui::ModReadout> got = c.routeReadout(macro);
				REQUIRE(got.has_value());
				// THE RESTATEMENT: base + amount × curve(macro) × (max − min), clamped unless it may leave.
				const double curved =
				    t == device::Transfer::Lin ? kMacro : static_cast<double>(device::shapeTransfer(t, static_cast<float>(kMacro)));
				double expected = base + static_cast<double>(amount) * curved * span;
				if (!desc.offsetsMayLeaveRange) {
					expected = std::clamp(expected, desc.min, desc.max);
				}
				CHECK(got->value == doctest::Approx(expected).epsilon(1e-9));
				// AND `ParamSet::resolved` — the engine's own answer at the amount the route now holds.
				block();
				CHECK(params.resolved(static_cast<clap_id>(id)) == doctest::Approx(got->value).epsilon(1e-12));
			}
		}
		bridge.setRouteAmount(macro, dest, 0.0f);
	}
}

TEST_CASE("bipolar-macro-core: a BIPOLAR macro into a core parameter — the readout, its sweep and ParamSet's resolve agree, "
          "both ways") {
	// The core resolve read a bipolar macro as its raw 0..1 position, so it played unipolar
	// while its ring drew both ways — and this readout's sweep had been written to match the engine (0..1 for a macro).
	// Now both read 2v - 1. THE RESTATEMENT: base + amount x (2v - 1) x span, its sweep base ± amount x span.
	// KNOWN-BAD: `ParamSet::macroReading` without the bipolar mask — the value and the engine checks go red.
	// KNOWN-BAD: the sweep's `lo` back to `(!probe.macro && bipolar) ? -1 : 0` — the lo check goes red.
	ParamSet params;
	Synth synth { params };
	synth.activate(48000.0, 4096);
	ParamBridge bridge;
	ui::ModState mod;
	ui::EditHistory history;
	std::vector<float> l(256), r(256);
	const auto block = [&] {
		publishEditorLanes(bridge, params, synth);
		synth.render(l.data(), r.data(), 256);
		publishEngineReadings(bridge, synth);
	};
	const ParamDesc& desc = *findParam(static_cast<clap_id>(ParamId::FineTune));
	const backplane::Destination dest =
	    backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::FineTune)));
	const backplane::Source macro = backplane::Source::core(2);
	const double span = desc.max - desc.min;
	bridge.setMacroBipolar(2, true);
	ui::ParamControl c(bridge, &desc, &mod, &history);
	constexpr float kAmount = 0.3f;
	bridge.setRouteAmount(macro, dest, kAmount);
	for (const double v : { 0.0, 0.2, 0.5, 0.9, 1.0 }) {
		CAPTURE(v);
		params.setBase(static_cast<clap_id>(macroParam(2)), v);
		bridge.setValue(static_cast<clap_id>(macroParam(2)), v);
		(void)c.routeReadout(macro);
		block();
		const std::optional<ui::ModReadout> got = c.routeReadout(macro);
		REQUIRE(got.has_value());
		const double expected = static_cast<double>(kAmount) * (2.0 * v - 1.0) * span; // base 0
		CHECK(got->value == doctest::Approx(expected).epsilon(1e-9));
		CHECK(params.resolved(static_cast<clap_id>(ParamId::FineTune)) == doctest::Approx(got->value).epsilon(1e-12));
		CHECK(got->lo == doctest::Approx(-static_cast<double>(kAmount) * span));
		CHECK(got->hi == doctest::Approx(static_cast<double>(kAmount) * span));
	}
}
