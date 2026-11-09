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

#include "device/route_resolve.h"
#include "fx/filter/filter_types.h"
#include "mod/arp/pattern_types.h"
#include "plugin/device_registry.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/mod_readout.h"
#include "plugin/ui/mod_readout_layer.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// BACKLOG PU — THE AS-MODULATED READOUT, BY VALUE.
//
// Every case asks a NUMBER, never a picture: the readout's value against (1) an independent restatement of the
// intended arithmetic, written here from the spec (base + amount × source × span, rounded if stepped, clamped unless
// the control may leave its range), and (2) what the ENGINE resolved once it adopted the same amount
// (`modulatedFieldAt`). The readout is computed BEFORE the engine adopts the amount, which is the moment it is for.
// The text is then the destination's own print of that value (`DeviceControl::textAt`).

using namespace feathers;
namespace pat = mod::arp::pat;
namespace filt = fx::filter;

namespace {

constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 8,
	.outputStride = 4,
};

struct Rig {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	ui::ModState mod;
	ui::EditHistory history;
	std::vector<float> l = std::vector<float>(512, 0.0f);
	std::vector<float> r = std::vector<float>(512, 0.0f);

	Rig() {
		engine.prepare(48000.0, 4096);
		ui::LiveAmount::reset();
		ui::LiveAmount::setClockForTest(nullptr);
	}
	DeviceInstanceId add(const char* tag) {
		const DeviceInstanceId added = bridge.addDevice(tag);
		bridge.drain();
		return added;
	}
	/// One block, then the editor's publish — the plugin's order.
	void block() {
		engine.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
		bridge.publish(engine);
	}
	/// THE READOUT FOR `amount`, ASKED BEFORE THE ENGINE HAS IT: the amount is written to the record, the pair is
	/// watched and a block answers the probe — no drain, so the engine still holds the previous amount.
	std::optional<ui::ModReadout> readoutAt(ui::DeviceControl& c, backplane::Source s, float amount) {
		bridge.setRouteAmount(s, c.destination(), amount);
		(void)c.routeReadout(s); // names the pair
		block();
		return c.routeReadout(s);
	}
	/// AND WHAT THE ENGINE RESOLVES ONCE IT HAS ADOPTED IT.
	float engineAfterDrain(DeviceInstanceId device, size_t control) {
		bridge.drain();
		block();
		return bridge.modulatedFieldAt(device, control);
	}
};

size_t findParam(const device::Descriptor& d, const char* name) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (std::string(d.params[i].name) == name) {
			return i;
		}
	}
	return d.params.size();
}

} // namespace

TEST_CASE("PU: a Choice destination (Pattern's Scale) with a toggle source at on reads the label the engine resolves, "
          "at every amount") {
	Rig rig;
	const DeviceInstanceId p = rig.add(pat::kTraits.tag);
	REQUIRE(p != kNoInstance);
	const size_t scale = pat::at(0, pat::Field::Scale);
	const device::ParamDesc& desc = pat::kDescriptor.params[scale];
	REQUIRE(desc.unit == device::Unit::Choice);
	REQUIRE(desc.modulatable);
	// THE TOGGLE, AT ON: a Latch macro at 1.
	rig.bridge.setMacroType(0, device::MacroType::Latch);
	rig.bridge.setMacro(0, 1.0f);
	const backplane::Source toggle = backplane::Source::core(0);

	ui::DeviceControl c(rig.bridge, p, pat::kDescriptor, scale, &rig.mod, &rig.history);
	const double span = device::routeSpanOf(desc);
	const int last = static_cast<int>(desc.max);
	for (const double base : { 0.0, 4.0 }) {
		rig.bridge.setField(p, scale, static_cast<float>(base));
		rig.bridge.drain();
		for (const float amount : { 0.05f, 0.1f, 0.15f, 0.2f, 0.35f, 0.5f, 0.75f, 1.0f, 2.0f, -0.1f, -0.25f }) {
			CAPTURE(base);
			CAPTURE(amount);
			const std::optional<ui::ModReadout> got = rig.readoutAt(c, toggle, amount);
			REQUIRE(got.has_value());
			// THE RESTATEMENT: base + amount × 1 × span, on a step, inside the list.
			const int expected = std::clamp(static_cast<int>(std::lround(base + static_cast<double>(amount) * span)), 0, last);
			CHECK(got->value == doctest::Approx(static_cast<double>(expected)));
			CHECK(got->text == c.textAt(static_cast<double>(expected)));
			CHECK(got->text.find(pat::kScaleNames[static_cast<size_t>(expected)]) != std::string::npos);
			// THE RANGE: a toggle sweeps from the base (off) to the value (on).
			CHECK(got->loText == c.textAt(std::min(base, static_cast<double>(expected))));
			CHECK(got->hiText == c.textAt(std::max(base, static_cast<double>(expected))));
			// AND THE ENGINE AGREES ONCE IT HAS THE AMOUNT.
			const float engine = rig.engineAfterDrain(p, scale);
			CHECK(static_cast<double>(engine) == doctest::Approx(got->value));
			CHECK(c.textAt(static_cast<double>(engine)) == got->text);
		}
	}
	// NOT VACUOUS: the readout moved off the base, so it is not the base reprinted.
	rig.bridge.setField(p, scale, 0.0f);
	rig.bridge.drain();
	const std::optional<ui::ModReadout> moved = rig.readoutAt(c, toggle, 0.5f);
	REQUIRE(moved.has_value());
	CHECK(moved->text != c.textAt(0.0));
}

TEST_CASE("PU: a Hz destination (the filter's cutoff) reads the source's PRESENT value times the amount, past the "
          "knob's end where the control allows it") {
	Rig rig;
	const DeviceInstanceId f = rig.add(filt::kTraits.tag);
	REQUIRE(f != kNoInstance);
	const size_t cutoff = findParam(filt::kDescriptor, "Filter Cutoff");
	REQUIRE(cutoff < filt::kDescriptor.params.size());
	const device::ParamDesc& desc = filt::kDescriptor.params[cutoff];
	REQUIRE(desc.offsetsMayLeaveRange);
	// A KNOB MACRO PART-WAY, so a readout that took the source as 1 (the amount alone) is wrong.
	constexpr float kSource = 0.6f;
	rig.bridge.setMacro(1, kSource);
	const backplane::Source knob = backplane::Source::core(1);
	ui::DeviceControl c(rig.bridge, f, filt::kDescriptor, cutoff, &rig.mod, &rig.history);
	const double span = device::routeSpanOf(desc);
	for (const double base : { desc.def, desc.max - 0.5 }) {
		rig.bridge.setField(f, cutoff, static_cast<float>(base));
		rig.bridge.drain();
		for (const float amount : { 0.1f, 0.3f, -0.4f, 1.0f }) {
			CAPTURE(base);
			CAPTURE(amount);
			const std::optional<ui::ModReadout> got = rig.readoutAt(c, knob, amount);
			REQUIRE(got.has_value());
			const double expected = static_cast<double>(static_cast<float>(base))
			                        + static_cast<double>(amount) * static_cast<double>(kSource) * span;
			CHECK(got->value == doctest::Approx(expected).epsilon(1e-9));
			CHECK(got->text == c.textAt(expected));
			// (The filter prints its corner in its own words — "1.5k" — which is the point: the device's print, not ours.)
			const float engine = rig.engineAfterDrain(f, cutoff);
			CHECK(static_cast<double>(engine) == doctest::Approx(got->value).epsilon(1e-6));
			// THE RANGE: a unipolar knob sweeps from the base (0) to the full amount (1).
			const double full = static_cast<double>(static_cast<float>(base)) + static_cast<double>(amount) * span;
			CHECK(got->lo == doctest::Approx(std::min(static_cast<double>(static_cast<float>(base)), full)));
			CHECK(got->hi == doctest::Approx(std::max(static_cast<double>(static_cast<float>(base)), full)));
		}
	}
	// PAST THE KNOB'S END: from near the top a full route carries the corner beyond `max`, as the engine does.
	rig.bridge.setField(f, cutoff, static_cast<float>(desc.max - 0.5));
	rig.bridge.drain();
	const std::optional<ui::ModReadout> past = rig.readoutAt(c, knob, 1.0f);
	REQUIRE(past.has_value());
	CHECK(past->value > desc.max);
	CHECK(static_cast<double>(rig.engineAfterDrain(f, cutoff)) == doctest::Approx(past->value).epsilon(1e-6));
}

TEST_CASE("PU: a stepped destination (Pattern's Length) lands on the step the engine lands on") {
	Rig rig;
	const DeviceInstanceId p = rig.add(pat::kTraits.tag);
	REQUIRE(p != kNoInstance);
	const size_t length = pat::at(0, pat::Field::Length);
	const device::ParamDesc& desc = pat::kDescriptor.params[length];
	REQUIRE(desc.stepped);
	REQUIRE(desc.unit == device::Unit::Number);
	constexpr float kSource = 0.37f;
	rig.bridge.setMacro(2, kSource);
	const backplane::Source knob = backplane::Source::core(2);
	ui::DeviceControl c(rig.bridge, p, pat::kDescriptor, length, &rig.mod, &rig.history);
	const double span = device::routeSpanOf(desc);
	rig.bridge.setField(p, length, 16.0f);
	rig.bridge.drain();
	bool sawOffBase = false;
	for (const float amount : { 0.05f, 0.11f, 0.2f, 0.33f, 0.6f, 1.0f, 2.0f, -0.2f, -1.0f }) {
		CAPTURE(amount);
		const std::optional<ui::ModReadout> got = rig.readoutAt(c, knob, amount);
		REQUIRE(got.has_value());
		const double expected =
		    std::clamp(std::round(16.0 + static_cast<double>(amount) * static_cast<double>(kSource) * span), desc.min, desc.max);
		CHECK(got->value == doctest::Approx(expected));
		CHECK(got->value == std::round(got->value)); // ON a step
		CHECK(got->text == c.textAt(expected));
		sawOffBase = sawOffBase || expected != 16.0;
		CHECK(static_cast<double>(rig.engineAfterDrain(p, length)) == doctest::Approx(got->value));
	}
	CHECK(sawOffBase);
}

TEST_CASE("PU: the readout is the engine's arithmetic, other routes and order included — asModulated against a "
          "hand-built probe") {
	// THE PURE HALF, NO ENGINE: what the box adds to what the engine reported. A second route already reaching the
	// control sits in `before`; the readout's own term goes after it; the range is over the source's reach.
	device::ParamDesc p {};
	p.min = 0.0;
	p.max = 10.0;
	p.stepped = true;
	backplane::RouteProbe probe;
	probe.source = 0.5f;
	probe.before = 1.0;
	probe.after = 0.0;
	probe.span = 10.0;
	// base 2 + other 1 + 0.3 × 0.5 × 10 = 4.5 → 5 (round half away from zero, `std::round`, the engine's rule).
	CHECK(ui::asModulated(p, 2.0, probe, 0.3f, device::Transfer::Lin, probe.source) == doctest::Approx(5.0));
	// Bipolar source: the sweep is over −1..1 → 2 + 1 − 3 = 0 to 2 + 1 + 3 = 6.
	const ui::ModReadout r =
	    ui::readoutFor(p, 2.0, probe, 0.3f, device::Transfer::Lin, true, [](double v) { return std::to_string(v); });
	CHECK(r.lo == doctest::Approx(0.0));
	CHECK(r.hi == doctest::Approx(6.0));
	CHECK(r.sweeps());
	// Clamped at an end unless the control says otherwise.
	CHECK(ui::asModulated(p, 9.0, probe, 1.0f, device::Transfer::Lin, 1.0f) == doctest::Approx(10.0));
	p.offsetsMayLeaveRange = true;
	CHECK(ui::asModulated(p, 9.0, probe, 1.0f, device::Transfer::Lin, 1.0f) == doctest::Approx(20.0));
}

TEST_CASE("PU: no route, no readout; a host that cannot say shows nothing rather than a guess") {
	Rig rig;
	const DeviceInstanceId p = rig.add(pat::kTraits.tag);
	const size_t scale = pat::at(0, pat::Field::Scale);
	ui::DeviceControl c(rig.bridge, p, pat::kDescriptor, scale, &rig.mod, &rig.history);
	rig.bridge.setMacro(0, 1.0f);
	// No route at all.
	(void)c.routeReadout(backplane::Source::core(0));
	rig.block();
	CHECK_FALSE(c.routeReadout(backplane::Source::core(0)).has_value());
	// A route, but the probe not yet answered for it (no block since it was named): nothing.
	rig.bridge.setRouteAmount(backplane::Source::core(3), c.destination(), 0.5f);
	CHECK_FALSE(c.routeReadout(backplane::Source::core(3)).has_value());
}

TEST_CASE("PU: the box appears beside the destination's face on its card while the amount is set, not over it, and "
          "goes after the linger") {
	Rig rig;
	double clock = 1000.0;
	ui::LiveAmount::setClockForTest([&clock] { return clock; });
	const DeviceInstanceId f = rig.add(filt::kTraits.tag);
	const size_t cutoff = findParam(filt::kDescriptor, "Filter Cutoff");
	REQUIRE(cutoff < filt::kDescriptor.params.size());
	auto card = std::make_unique<ui::GenericCard>(rig.bridge, f, filt::kDescriptor, nullptr);
	card->setModulation(&rig.mod, &rig.history);
	card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	card->setBounds(0, 0, 400, 320);
	rig.bridge.setMacro(1, 0.6f);
	const backplane::Source knob = backplane::Source::core(1);
	const backplane::Destination dest = backplane::Destination::control(f, cutoff);
	rig.bridge.setRouteAmount(knob, dest, 0.3f);
	rig.bridge.drain();

	// THE HAND: an amount gesture on the destination, as every face's road spells it.
	ui::LiveAmount::begin(rig.bridge.routeIdentity(), knob, dest);
	(void)card->update(); // names the pair through the face's control
	rig.block();
	(void)card->update();
	ui::ModReadoutLayer& layer = card->modReadoutLayer();
	REQUIRE(layer.shown());
	ui::DeviceControl c(rig.bridge, f, filt::kDescriptor, cutoff, &rig.mod, &rig.history);
	const std::optional<ui::ModReadout> want = c.routeReadout(knob);
	REQUIRE(want.has_value());
	CHECK(layer.valueLine() == want->text);
	CHECK(layer.rangeLine() == want->loText + " … " + want->hiText);
	// BESIDE, NOT OVER: the box and the face it names do not intersect, and both are inside the card.
	const visage::Bounds box = layer.box();
	const visage::Bounds face = layer.anchor();
	CHECK(face.width() > 0.0f);
	CHECK_FALSE(box.overlaps(face));
	CHECK(box.x() >= 0.0f);
	CHECK(box.right() <= card->width());
	CHECK(box.y() >= 0.0f);
	CHECK(box.bottom() <= card->height());

	// THE HAND LETS GO: it lingers, then goes.
	ui::LiveAmount::end(rig.bridge.routeIdentity(), knob, dest);
	clock += ui::LiveAmount::kLingerMs * 0.5;
	(void)card->update();
	CHECK(layer.shown());
	clock += ui::LiveAmount::kLingerMs;
	(void)card->update();
	CHECK_FALSE(layer.shown());
	ui::LiveAmount::setClockForTest(nullptr);
}

TEST_CASE("PU: a route's TRANSFER curve is in the readout exactly as the engine applies it — Log and Exp, bipolar "
          "Abs and Neg half") {
	// The readout includes a route's transfer function when one is applied. The curve sits between the source and the amount
	// (`device::shapeTransfer`, inside `device::routeTerm`); the readout reads the route's curve from the record and
	// hands it to the same `routeTerm` the engine calls.
	Rig rig;
	const DeviceInstanceId f = rig.add(filt::kTraits.tag);
	const size_t cutoff = findParam(filt::kDescriptor, "Filter Cutoff");
	REQUIRE(cutoff < filt::kDescriptor.params.size());
	const device::ParamDesc& desc = filt::kDescriptor.params[cutoff];
	const double span = device::routeSpanOf(desc);
	ui::DeviceControl c(rig.bridge, f, filt::kDescriptor, cutoff, &rig.mod, &rig.history);
	rig.bridge.setField(f, cutoff, static_cast<float>(desc.def));
	struct Leg {
		size_t macro;
		float value;
		bool bipolar;
		device::Transfer transfer;
	};
	// A bipolar macro's cell reads (2v − 1): 0.2 is −0.6, which Abs folds up; 0.8 is +0.6, which Neg half silences.
	for (const Leg leg : { Leg { 1, 0.6f, false, device::Transfer::Log }, Leg { 1, 0.6f, false, device::Transfer::Exp },
	                       Leg { 2, 0.2f, true, device::Transfer::Abs }, Leg { 2, 0.8f, true, device::Transfer::Neg } }) {
		CAPTURE(static_cast<int>(leg.transfer));
		rig.bridge.setMacro(leg.macro, leg.value);
		rig.bridge.setMacroBipolar(leg.macro, leg.bipolar);
		const backplane::Source src = backplane::Source::core(leg.macro);
		rig.bridge.drain();
		rig.bridge.setRouteAmount(src, c.destination(), 0.5f);
		REQUIRE(rig.bridge.setRouteTransfer(src, c.destination(), leg.transfer));
		rig.bridge.drain();
		rig.block();
		const std::optional<ui::ModReadout> got = rig.readoutAt(c, src, 0.7f);
		REQUIRE(got.has_value());
		// THE RESTATEMENT: the cell's reading through the curve, times the amount and the span.
		const float reading = leg.bipolar ? 2.0f * leg.value - 1.0f : leg.value;
		const double curved = static_cast<double>(device::shapeTransfer(leg.transfer, reading));
		CHECK(curved != doctest::Approx(static_cast<double>(reading))); // the curve really bends this reading
		const double expected = static_cast<double>(static_cast<float>(desc.def)) + static_cast<double>(0.7f) * curved * span;
		CHECK(got->value == doctest::Approx(expected).epsilon(1e-6));
		CHECK(static_cast<double>(rig.engineAfterDrain(f, cutoff)) == doctest::Approx(got->value).epsilon(1e-6));
		rig.bridge.setRouteAmount(src, c.destination(), 0.0f);
		rig.bridge.drain();
	}
}

TEST_CASE("PY: a route's φ curve is in the readout as the engine applies it, at its periods, and the range reaches "
          "the curve's inside peak") {
	Rig rig;
	const DeviceInstanceId f = rig.add(filt::kTraits.tag);
	const size_t cutoff = findParam(filt::kDescriptor, "Filter Cutoff");
	REQUIRE(cutoff < filt::kDescriptor.params.size());
	const device::ParamDesc& desc = filt::kDescriptor.params[cutoff];
	const double span = device::routeSpanOf(desc);
	ui::DeviceControl c(rig.bridge, f, filt::kDescriptor, cutoff, &rig.mod, &rig.history);
	// LOW ENOUGH THAT THE WHOLE REACH LANDS INSIDE THE RANGE, so the range is the curve's and not the clamp's.
	const double base = desc.min + 0.1 * (desc.max - desc.min);
	rig.bridge.setField(f, cutoff, static_cast<float>(base));
	const backplane::Source src = backplane::Source::core(1);
	for (const float octaves : { 1.0f, 5.5f }) {
		CAPTURE(octaves);
		const device::Transfer t = device::phiTransfer(octaves);
		rig.bridge.setMacro(1, 0.43f);
		rig.bridge.setMacroBipolar(1, false);
		rig.bridge.drain();
		rig.bridge.setRouteAmount(src, c.destination(), 0.5f);
		REQUIRE(rig.bridge.setRouteTransfer(src, c.destination(), t));
		rig.bridge.drain();
		rig.block();
		const float amount = 0.2f;
		const std::optional<ui::ModReadout> got = rig.readoutAt(c, src, amount);
		REQUIRE(got.has_value());
		// THE RESTATEMENT: 0.43 through the φ curve at these periods, times the amount and the span.
		const double curved = static_cast<double>(device::phiTriangleCurve(0.43f, device::phiPeriods(t)));
		CHECK(curved != doctest::Approx(0.43));
		const double expected = static_cast<double>(static_cast<float>(base))
		                        + static_cast<double>(amount) * curved * span;
		CHECK(got->value == doctest::Approx(expected).epsilon(1e-6));
		// THE RANGE: 0 at rest and the curve's peak (≈ 1) at its top, which no end of the source's range reaches.
		CHECK(got->lo == doctest::Approx(static_cast<double>(static_cast<float>(base))).epsilon(1e-6));
		CHECK(got->hi >= static_cast<double>(static_cast<float>(base)) + static_cast<double>(amount) * 0.995 * span);
		rig.bridge.setRouteAmount(src, c.destination(), 0.0f);
		rig.bridge.drain();
	}
}
