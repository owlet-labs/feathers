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

#include "backplane/block_context.h"
#include "device/ears.h"
#include "mod/lfo/lfo_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/stereo/stereo_types.h"
#include "fx/filter/filter_types.h"
#include "fx/filter/multimode_filter.h"
#include "fx/owltt/owltt_types.h"
#include "mod/macros/macros_types.h"
#include "mod/math/math_types.h"
#include "mod/noteexpr/noteexpr_types.h"
#include "plugin/device_registry.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"

#include <array>
#include <cmath>
#include <string>

// STEREO MODULATION (backlog KU). Every number here is read through
// a seam and restated from the rule — never inferred from a render.

using namespace feathers;
using rack::RackEngine;

namespace {

[[maybe_unused]] constexpr backplane::Axes kAxes {
	.coreSources = 8,
	.coreDestinations = 0,
	.deviceSlots = 8,
	.outputStride = 8,
};

[[maybe_unused]] void block(RackEngine& e, int frames = 64) {
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	e.processStereo(l.data(), r.data(), frames, backplane::unknownBlock());
}

[[maybe_unused]] bool near(double a, double b, double tol = 1e-6) { return std::abs(a - b) <= tol; }

} // namespace

TEST_CASE("a control mask holds every control a device can declare") {
	device::ControlMask m;
	CHECK_FALSE(m.any());
	m.set(0);
	m.set(device::kMaxDeviceFields - 1);
	CHECK(m.test(0));
	CHECK(m.test(device::kMaxDeviceFields - 1));
	CHECK_FALSE(m.test(1));
	CHECK(m.any());
}

TEST_CASE("every stereo control is a destination, and every pairFrom bit names one of the device's stereo controls") {
	size_t stereoSeen = 0;
	for (const device::Descriptor& d : kDevices) {
		CAPTURE(std::string(d.traits.name));
		for (size_t c = 0; c < d.params.size(); ++c) {
			if (d.params[c].stereo) {
				++stereoSeen;
				CHECK(d.params[c].modulatable);
			}
		}
		for (const device::SourceDesc& s : d.outputs) {
			for (size_t c = 0; c < 32; ++c) {
				if ((s.pairFrom >> c) & 1u) {
					REQUIRE(c < d.params.size());
					CHECK(d.params[c].stereo);
				}
			}
		}
	}
	// NOT VACUOUS since the Stereo Tool (its In is stereo).
	CHECK(stereoSeen > 0);
}

TEST_CASE("a mono source speaks the same number in both ears, every tick") {
	// NO RIGHT-EAR TICK WINDOW THIS ROUND (Q3 2026-10-04: nothing reads it; it arrives with windowed stereo),
	// so only the published cell is asserted.
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lfo = e.addDevice(mod::lfo::kTraits.tag);
	REQUIRE(lfo != RackEngine::kNoSlot);
	int moved = 0;
	float last = e.publishedOutputAt(lfo, 0);
	for (int b = 0; b < 64; ++b) {
		block(e);
		const float l = e.publishedOutputAt(lfo, 0);
		CHECK(e.publishedOutputRAt(lfo, 0) == l);
		moved += l != last ? 1 : 0;
		last = l;
	}
	REQUIRE(moved > 0); // NOT VACUOUS: the LFO really moved, so equal ears are a copy and not two rests
}

namespace st = feathers::mod::stereo;

namespace {

/// SPLIT, RESTATED from the design (2026-10-05), in double and independent of `evaluate`'s branches: with i = In
/// and a = Amount, a ≤ 1 → L = i, R = i·(1 − 2a); a > 1 → L = i·a, R = −i·a.
struct SplitWant {
	double left;
	double right;
};
SplitWant splitWant(double i, double a) {
	return a <= 1.0 ? SplitWant { i, i * (1.0 - 2.0 * a) } : SplitWant { i * a, -i * a };
}

/// A STEREO TOOL WHOSE SPLIT IS THE UNIT PAIR — In 100%, Amount at its 100% default: (+1, −1). The old Identity,
/// mirrored: identity is both knobs at 100%.
size_t addUnitSplit(RackEngine& e) {
	const size_t tool = e.addDevice(st::kTraits.tag);
	REQUIRE(tool != RackEngine::kNoSlot);
	e.setField(tool, static_cast<size_t>(st::Param::In), 1.0f);
	REQUIRE(e.fieldAt(tool, static_cast<size_t>(st::Param::Amount)) == 1.0f);
	return tool;
}
} // namespace

TEST_CASE("Split restated: Amount 0 is mono, 50% silences the right, 100% inverts it, 200% doubles both — for In of either sign") {
	using O = st::Output;
	for (const float i : { 0.7f, -0.4f, 1.0f }) {
		for (const float a : { 0.0f, 0.25f, 0.5f, 1.0f, 1.5f, 2.0f }) {
			CAPTURE(i);
			CAPTURE(a);
			const SplitWant want = splitWant(i, a);
			// In's RIGHT ear is deliberately different: Split reads the LEFT.
			const device::EarPair p = st::evaluate(O::Split, i, -0.9f, a, 1.0f, false);
			CHECK(near(p.left, want.left));
			CHECK(near(p.right, want.right));
		}
	}
	// THE NAMED POINTS, spelled out for i = 0.7.
	CHECK(st::evaluate(O::Split, 0.7f, 0.7f, 0.0f, 1.0f, false).right == 0.7f);  // 0%: mono
	CHECK(st::evaluate(O::Split, 0.7f, 0.7f, 0.5f, 1.0f, false).right == 0.0f);  // 50%: right silent
	CHECK(st::evaluate(O::Split, 0.7f, 0.7f, 1.0f, 1.0f, false).right == -0.7f); // 100%: full inversion
	CHECK(near(st::evaluate(O::Split, 0.7f, 0.7f, 2.0f, 1.0f, false).left, 1.4));   // 200%: NOT clamped
	CHECK(near(st::evaluate(O::Split, 0.7f, 0.7f, 2.0f, 1.0f, false).right, -1.4));
}

TEST_CASE("Split is continuous at 100%: either side of it lands on (i, −i)") {
	constexpr float i = -0.6f;
	const device::EarPair at = st::evaluate(st::Output::Split, i, i, 1.0f, 1.0f, false);
	CHECK(at.left == i);
	CHECK(at.right == -i);
	for (const float eps : { 1e-3f, 1e-5f }) {
		CAPTURE(eps);
		const device::EarPair below = st::evaluate(st::Output::Split, i, i, 1.0f - eps, 1.0f, false);
		const device::EarPair above = st::evaluate(st::Output::Split, i, i, 1.0f + eps, 1.0f, false);
		CHECK(near(below.left, at.left, 3.0 * eps));
		CHECK(near(above.left, at.left, 3.0 * eps));
		CHECK(near(below.right, at.right, 3.0 * eps));
		CHECK(near(above.right, at.right, 3.0 * eps));
	}
}

TEST_CASE("Width restated: In re-imaged as mid ± width·side, clamped, and Swap exchanges the ears") {
	using O = st::Output;
	// WIDTH: mid ± width·side, side = (R − L)/2, clamped to ±1; Swap exchanges the ears.
	const float inL = -0.1f;
	const float inR = 0.9f;
	const auto width = [&](float w, bool swap) { return st::evaluate(O::Width, inL, inR, 1.0f, w, swap); };
	CHECK(near(width(0.0f, false).left, 0.4));
	CHECK(near(width(0.0f, false).right, 0.4));
	CHECK(near(width(1.0f, false).left, -0.1));
	CHECK(near(width(1.0f, false).right, 0.9));
	CHECK(near(width(1.0f, true).left, 0.9));
	CHECK(near(width(1.0f, true).right, -0.1));
	CHECK(near(width(2.0f, false).left, -0.6));
	CHECK(near(width(2.0f, false).right, 1.0)); // 1.4, clamped
}

TEST_CASE("a Stereo Tool in a rack publishes Split in both ears, and its mono reading is the left") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t split = static_cast<size_t>(st::Output::Split);
	const size_t tool = e.addDevice(st::kTraits.tag);
	REQUIRE(tool != RackEngine::kNoSlot);
	// AT REST In is 0, so Split rests at 0 in both ears.
	block(e);
	CHECK(e.publishedOutputAt(tool, split) == 0.0f);
	CHECK(e.publishedOutputRAt(tool, split) == 0.0f);
	// IN AT 100%, AMOUNT AT ITS 100% DEFAULT: (+1, −1).
	e.setField(tool, static_cast<size_t>(st::Param::In), 1.0f);
	block(e);
	CHECK(e.publishedOutputAt(tool, split) == 1.0f);
	CHECK(e.publishedOutputRAt(tool, split) == -1.0f);
	// AMOUNT 150%: restated.
	e.setField(tool, static_cast<size_t>(st::Param::Amount), 1.5f);
	block(e);
	CHECK(near(e.publishedOutputAt(tool, split), splitWant(1.0, 1.5).left));
	CHECK(near(e.publishedOutputRAt(tool, split), splitWant(1.0, 1.5).right));
	// DISABLED PUBLISHES REST IN BOTH EARS.
	e.setField(tool, static_cast<size_t>(st::Param::Enabled), 0.0f);
	block(e);
	CHECK(e.publishedOutputAt(tool, split) == 0.0f);
	CHECK(e.publishedOutputRAt(tool, split) == 0.0f);
}

namespace fm = feathers::mod::math;
namespace fmac = feathers::mod::macros;
namespace flt = feathers::fx::filter;

TEST_CASE("a control is stereo-live only when it is stereo AND a pair reaches it — and the bit passes through Math and a macro") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t math = e.addDevice(fm::kTraits.tag);
	const size_t macros = e.addDevice(fmac::kTraits.tag);
	const size_t lfo = e.addDevice(mod::lfo::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t owltt = e.addDevice(fx::owltt::kTraits.tag);
	for (const size_t s : { tool, math, macros, lfo, f, owltt }) {
		REQUIRE(s != RackEngine::kNoSlot);
	}
	const size_t split = static_cast<size_t>(st::Output::Split);
	const size_t b = static_cast<size_t>(fm::Param::B);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const size_t res = static_cast<size_t>(flt::Param::Resonance);
	const size_t m1 = static_cast<size_t>(flt::Param::Macro1);

	// THE CONTROL: a mono source into a stereo control is not stereo-live.
	e.setRouteAmount(test::outputOf(e, lfo, 0), test::controlOf(e, f, cutoff), 0.2f);
	block(e);
	CHECK_FALSE(e.stereoLiveForTest(f, cutoff));

	// A PAIR INTO MATH'S B: B is live, and Math's output now emits a pair…
	e.setRouteAmount(test::outputOf(e, tool, split), test::controlOf(e, math, b), 0.5f);
	// …which reaches MACRO 1, making MACROS' first output a pair…
	e.setRouteAmount(test::outputOf(e, math, 0), test::controlOf(e, macros, fmac::macroParam(0)), 0.25f);
	// …which reaches the filter's Resonance. Two hops from Split.
	e.setRouteAmount(test::outputOf(e, macros, 0), test::controlOf(e, f, res), 0.3f);
	// AND MACROS' SECOND OUTPUT, which no pair reaches, into Macro 1 of the filter: stays mono.
	e.setRouteAmount(test::outputOf(e, macros, 1), test::controlOf(e, f, m1), 0.3f);
	// A PAIR INTO A MONO DESTINATION (OWLTT's first modulatable control): never stereo-live.
	const size_t owlttControl = fx::owltt::kDescriptor.controlOfNthModulatableBesideEnable(0);
	e.setRouteAmount(test::outputOf(e, tool, split), test::controlOf(e, owltt, owlttControl), 0.1f);
	block(e);

	CHECK(e.stereoLiveForTest(math, b));
	CHECK(e.emitsPairForTest(math, 0));
	CHECK(e.stereoLiveForTest(macros, fmac::macroParam(0)));
	CHECK(e.emitsPairForTest(macros, 0));
	CHECK_FALSE(e.emitsPairForTest(macros, 1));
	CHECK(e.stereoLiveForTest(f, res));
	CHECK_FALSE(e.stereoLiveForTest(f, m1));
	CHECK_FALSE(e.stereoLiveForTest(f, cutoff));
	CHECK_FALSE(e.stereoLiveForTest(owltt, owlttControl));
	// AND IT GOES AWAY WITH THE ROUTE: cut Split and the whole chain goes mono at the next drain.
	e.setRouteAmount(test::outputOf(e, tool, split), test::controlOf(e, math, b), 0.0f);
	block(e);
	CHECK_FALSE(e.emitsPairForTest(math, 0));
	CHECK_FALSE(e.stereoLiveForTest(f, res));
}

TEST_CASE("the stereo-live bit does not depend on lane order — a pair routed leftward still arrives") {
	// Backlog IZ lets a route run from a modulator into one to its LEFT, so the fixed point cannot be a single walk.
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t macros = e.addDevice(fmac::kTraits.tag);
	const size_t math = e.addDevice(fm::kTraits.tag);
	const size_t tool = addUnitSplit(e);
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)),
	                 test::controlOf(e, math, static_cast<size_t>(fm::Param::A)), 0.5f);
	e.setRouteAmount(test::outputOf(e, math, 0), test::controlOf(e, macros, fmac::macroParam(2)), 0.5f);
	e.setRouteAmount(test::outputOf(e, macros, 2), test::controlOf(e, f, static_cast<size_t>(flt::Param::Spread)), 0.5f);
	block(e);
	// If the graph refuses a route as it stands, the case must say so rather than pass vacuously.
	REQUIRE(e.routeAmount(test::outputOf(e, macros, 2), test::controlOf(e, f, static_cast<size_t>(flt::Param::Spread))) == 0.5f);
	CHECK(e.stereoLiveForTest(f, static_cast<size_t>(flt::Param::Spread)));
}

TEST_CASE("the unit Split on a cutoff puts the left ear at base + a·span and the right at base − a·span, exactly") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const device::ParamDesc& p = flt::kDescriptor.params[cutoff];
	constexpr float kAmount = 0.1f;
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)), test::controlOf(e, f, cutoff), kAmount);
	block(e);
	const double base = e.fieldAt(f, cutoff);
	const double span = device::routeSpanOf(p);
	// RESTATED from the rule — amount × source × span, landed by `withRouteOffset` — not borrowed from the resolve.
	const auto wantL = static_cast<float>(device::withRouteOffset(p, base, static_cast<double>(kAmount) * 1.0 * span));
	const auto wantR = static_cast<float>(device::withRouteOffset(p, base, static_cast<double>(kAmount) * -1.0 * span));
	REQUIRE(e.stereoLiveForTest(f, cutoff));
	CHECK(e.resolvedFieldForTest(f, cutoff) == wantL);
	CHECK(e.resolvedFieldRForTest(f, cutoff) == wantR);
	float right = 0.0f;
	CHECK(e.modulatedFieldRAt(f, cutoff, right));
	CHECK(right == wantR);
	CHECK(wantL != wantR); // NOT VACUOUS
}

TEST_CASE("a mono destination reads the left ear, and a mono source gives the same number in both") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t lfo = e.addDevice(mod::lfo::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t owltt = e.addDevice(fx::owltt::kTraits.tag);
	const size_t mono = fx::owltt::kDescriptor.controlOfNthModulatableBesideEnable(0);
	const device::ParamDesc& pm = fx::owltt::kDescriptor.params[mono];
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)), test::controlOf(e, owltt, mono), 0.1f);
	e.setRouteAmount(test::outputOf(e, lfo, 0), test::controlOf(e, f, static_cast<size_t>(flt::Param::Drive)), 0.3f);
	block(e);
	// MONO DESTINATION: the LEFT ear (+1), a constant +amount offset — the design, asserted as a value.
	const auto wantMono = static_cast<float>(device::withRouteOffset(pm, e.fieldAt(owltt, mono), 0.1 * 1.0 * device::routeSpanOf(pm)));
	CHECK(e.resolvedFieldForTest(owltt, mono) == wantMono);
	float right = 0.0f;
	CHECK_FALSE(e.modulatedFieldRAt(owltt, mono, right));
	// MONO SOURCE INTO A STEREO CONTROL: not stereo-live, so no right ear of its own is published.
	CHECK_FALSE(e.modulatedFieldRAt(f, static_cast<size_t>(flt::Param::Drive), right));
	CHECK(e.resolvedFieldRForTest(f, static_cast<size_t>(flt::Param::Drive)) == e.resolvedFieldForTest(f, static_cast<size_t>(flt::Param::Drive)));
}

TEST_CASE("with no stereo route the right-ear pass runs zero times") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t lfo = e.addDevice(mod::lfo::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t tool = addUnitSplit(e); // present, routed nowhere
	REQUIRE(tool != RackEngine::kNoSlot);
	e.setRouteAmount(test::outputOf(e, lfo, 0), test::controlOf(e, f, static_cast<size_t>(flt::Param::Cutoff)), 0.2f);
	for (int b = 0; b < 32; ++b) {
		block(e);
	}
	CHECK(e.rightPassesForTest() == 0u);
	// NOT VACUOUS: one stereo route and the counter moves.
	e.setRouteAmount(test::outputOf(e, tool, 0), test::controlOf(e, f, static_cast<size_t>(flt::Param::Resonance)), 0.1f);
	block(e);
	CHECK(e.rightPassesForTest() > 0u);
}

TEST_CASE("Abs on the unit Split reads 1 in both ears") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const backplane::Source src = test::outputOf(e, tool, static_cast<size_t>(st::Output::Split));
	e.setRouteAmount(src, test::controlOf(e, f, cutoff), 0.1f);
	REQUIRE(e.setRouteTransfer(src, test::controlOf(e, f, cutoff), device::Transfer::Abs));
	block(e);
	const device::ParamDesc& p = flt::kDescriptor.params[cutoff];
	const auto want = static_cast<float>(device::withRouteOffset(p, e.fieldAt(f, cutoff), 0.1 * 1.0 * device::routeSpanOf(p)));
	CHECK(e.resolvedFieldForTest(f, cutoff) == want);
	CHECK(e.resolvedFieldRForTest(f, cutoff) == want);
}

TEST_CASE("per voice, a per-voice source and a pair add up in each ear") {
	namespace nx = mod::noteexpr;
	RackEngine e { kAxes, hostCatalogue() };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t nxs = e.addDevice(nx::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	e.setField(f, static_cast<size_t>(flt::Param::Poly), 1.0f);
	REQUIRE(nx::kDescriptor.outputs[0].perVoice);
	e.setRouteAmount(test::outputOf(e, nxs, 0), test::controlOf(e, f, cutoff), 0.5f);
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)), test::controlOf(e, f, cutoff), 0.1f);
	e.writeOutputForVoice(test::idAt(e, nxs), 0, 0, 1.0f);
	e.writeOutputForVoice(test::idAt(e, nxs), 0, 1, 0.0f);
	block(e);
	const device::ParamDesc& p = flt::kDescriptor.params[cutoff];
	const double base = e.fieldAt(f, cutoff);
	const double span = device::routeSpanOf(p);
	for (const auto& [voice, pressure] : { std::pair<size_t, double> { 0, 1.0 }, { 1, 0.0 } }) {
		CAPTURE(voice);
		const auto wantL = static_cast<float>(device::withRouteOffset(p, base, 0.5 * pressure * span + 0.1 * 1.0 * span));
		const auto wantR = static_cast<float>(device::withRouteOffset(p, base, 0.5 * pressure * span + 0.1 * -1.0 * span));
		CHECK(e.resolvedFieldForVoiceForTest(f, cutoff, voice) == wantL);
		CHECK(e.resolvedFieldRForVoiceForTest(f, cutoff, voice) == wantR);
	}
}

TEST_CASE("the filter is handed both ears: its target carries the right ear for exactly the stereo-live controls") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)), test::controlOf(e, f, cutoff), 0.1f);
	block(e);
	const flt::Settings& t = flt::opsTargetForTest(e.storageForTest(f));
	CHECK(t.cutoffLog2Hz == e.resolvedFieldForTest(f, cutoff));
	CHECK(t.right.cutoffLog2Hz == e.resolvedFieldRForTest(f, cutoff));
	CHECK(t.cutoffLog2Hz != t.right.cutoffLog2Hz); // NOT VACUOUS
	CHECK(t.rightLive == flt::earBit(flt::Ear::Cutoff));
	// AND MONO AGAIN THE MOMENT THE ROUTE GOES — `setFields` forgets the right ear.
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)), test::controlOf(e, f, cutoff), 0.0f);
	block(e);
	CHECK(flt::opsTargetForTest(e.storageForTest(f)).rightLive == 0);
}

TEST_CASE("Math A × B with B = the unit Split gives (+A, −A)") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t math = e.addDevice(fm::kTraits.tag);
	e.setField(math, static_cast<size_t>(fm::Param::Operator), static_cast<float>(fm::Op::Multiply));
	e.setField(math, static_cast<size_t>(fm::Param::A), 0.6f);
	// B spans −1..1, so amount 0.5 lands the unit Split on B exactly: B_L = +1, B_R = −1.
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)),
	                 test::controlOf(e, math, static_cast<size_t>(fm::Param::B)), 0.5f);
	block(e);
	block(e); // Split is published in the first pass; Math reads it in the same pass by chain order, twice is margin
	CHECK(near(e.publishedOutputAt(math, 0), 0.6));
	CHECK(near(e.publishedOutputRAt(math, 0), -0.6));
}

TEST_CASE("a macro carries a pair through") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t tool = addUnitSplit(e);
	const size_t macros = e.addDevice(fmac::kTraits.tag);
	e.setField(macros, fmac::bipolarParam(0), 1.0f);
	e.setField(macros, fmac::macroParam(0), 0.5f);
	// Macro 1 spans 0..1: amount 0.25 puts L at 0.75 and R at 0.25; bipolar is 2v − 1 → (+0.5, −0.5).
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)),
	                 test::controlOf(e, macros, fmac::macroParam(0)), 0.25f);
	block(e);
	block(e);
	CHECK(near(e.publishedOutputAt(macros, 0), 0.5));
	CHECK(near(e.publishedOutputRAt(macros, 0), -0.5));
	// AND A MACRO NO PAIR REACHES IS MONO.
	CHECK(e.publishedOutputRAt(macros, 1) == e.publishedOutputAt(macros, 1));
}

TEST_CASE("Width 0 gives both ears the mid, and Swap swaps — In fed a pair by a second Stereo Tool") {
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t a = addUnitSplit(e);
	const size_t b = e.addDevice(st::kTraits.tag);
	const size_t in = static_cast<size_t>(st::Param::In);
	const size_t width = static_cast<size_t>(st::Output::Width);
	e.setField(b, in, 0.4f);
	// In spans −1..1 (span 2): amount 0.25 adds ±0.5 → In_L = 0.9, In_R = −0.1; mid 0.4, side −0.5.
	e.setRouteAmount(test::outputOf(e, a, static_cast<size_t>(st::Output::Split)), test::controlOf(e, b, in), 0.25f);
	e.setField(b, static_cast<size_t>(st::Param::Width), 0.0f);
	block(e);
	block(e);
	CHECK(near(e.publishedOutputAt(b, width), 0.4));
	CHECK(near(e.publishedOutputRAt(b, width), 0.4));
	e.setField(b, static_cast<size_t>(st::Param::Width), 1.0f);
	e.setField(b, static_cast<size_t>(st::Param::Swap), 1.0f);
	block(e);
	CHECK(near(e.publishedOutputAt(b, width), -0.1)); // swapped: In_R
	CHECK(near(e.publishedOutputRAt(b, width), 0.9)); // swapped: In_L
}

TEST_CASE("The audition gesture: a mono source into In with the knob at 0 gives a cutoff's ears opposite offsets — base + k·x and base − k·x") {
	// THE AUDITION THAT RETIRED PAN (2026-10-05): a bipolar source routed through the Stereo Tool must move the two
	// ears in OPPOSITE directions. A bipolar macro is the source here, so x is a value the case can name; the LFO
	// half below is the same road with a moving x.
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t macros = e.addDevice(fmac::kTraits.tag);
	const size_t tool = e.addDevice(st::kTraits.tag); // In at its resting 0, Amount at its 100% default
	const size_t f = e.addDevice(flt::kTraits.tag);
	REQUIRE(tool != RackEngine::kNoSlot);
	REQUIRE(e.fieldAt(tool, static_cast<size_t>(st::Param::In)) == 0.0f);
	const size_t in = static_cast<size_t>(st::Param::In);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const device::ParamDesc& pIn = st::kDescriptor.params[in];
	const device::ParamDesc& pCut = flt::kDescriptor.params[cutoff];
	constexpr float kIn = 0.5f;  // macro → In
	constexpr float kCut = 0.1f; // Split → cutoff
	e.setField(macros, fmac::bipolarParam(0), 1.0f);
	e.setRouteAmount(test::outputOf(e, macros, 0), test::controlOf(e, tool, in), kIn);
	e.setRouteAmount(test::outputOf(e, tool, static_cast<size_t>(st::Output::Split)), test::controlOf(e, f, cutoff), kCut);
	for (const float macro : { 0.8f, 0.35f }) {
		CAPTURE(macro);
		e.setField(macros, fmac::macroParam(0), macro);
		block(e);
		block(e);
		const double x = 2.0 * macro - 1.0; // the bipolar macro, restated
		// In's left ear: the route lands x on In's resting 0; Split at 100% sends it +1 left and −1 right.
		const double i = device::withRouteOffset(pIn, 0.0, kIn * x * device::routeSpanOf(pIn));
		const double base = e.fieldAt(f, cutoff);
		const double wantL = device::withRouteOffset(pCut, base, kCut * i * device::routeSpanOf(pCut));
		const double wantR = device::withRouteOffset(pCut, base, kCut * -i * device::routeSpanOf(pCut));
		REQUIRE(e.stereoLiveForTest(f, cutoff));
		CHECK(near(e.resolvedFieldForTest(f, cutoff), wantL, 1e-4));
		CHECK(near(e.resolvedFieldRForTest(f, cutoff), wantR, 1e-4));
		// THE PROPERTY HEARD MISSING IN AUDITION: the ears move OPPOSITE ways from the base, by the same distance.
		CHECK((e.resolvedFieldForTest(f, cutoff) - base) * (e.resolvedFieldRForTest(f, cutoff) - base) < 0.0);
		CHECK(near(e.resolvedFieldForTest(f, cutoff) - base, base - e.resolvedFieldRForTest(f, cutoff), 1e-4));
	}

	// AND A BIPOLAR LFO, the reported gesture, in a rack of its own (the LFO first in the lane): over many blocks the
	// ears always sit either side of the base, mirrored.
	RackEngine g { kAxes, hostCatalogue() };
	g.prepare(48000.0, 4096);
	const size_t lfo = g.addDevice(mod::lfo::kTraits.tag);
	const size_t tool2 = g.addDevice(st::kTraits.tag);
	const size_t f2 = g.addDevice(flt::kTraits.tag);
	REQUIRE(lfo != RackEngine::kNoSlot);
	REQUIRE(f2 != RackEngine::kNoSlot);
	g.setField(lfo, static_cast<size_t>(mod::lfo::Param::Bipolar), 1.0f);
	g.setField(lfo, static_cast<size_t>(mod::lfo::Param::Mode), static_cast<float>(mod::lfo::Mode::Free)); // runs with no note
	g.setRouteAmount(test::outputOf(g, lfo, 0), test::controlOf(g, tool2, in), kIn);
	g.setRouteAmount(test::outputOf(g, tool2, static_cast<size_t>(st::Output::Split)), test::controlOf(g, f2, cutoff), kCut);
	const double base = g.fieldAt(f2, cutoff);
	int above = 0;
	int below = 0;
	for (int b = 0; b < 1500; ++b) { // 2 s at 48 kHz: whole cycles of the default rate
		block(g);
		const double dl = g.resolvedFieldForTest(f2, cutoff) - base;
		const double dr = g.resolvedFieldRForTest(f2, cutoff) - base;
		CHECK(near(dl, -dr, 1e-4));
		above += dl > 1e-3 ? 1 : 0;
		below += dl < -1e-3 ? 1 : 0;
	}
	// NOT VACUOUS: the LFO swung the left ear both ways, so the mirror held for both signs of x.
	CHECK(above > 0);
	CHECK(below > 0);
}

// ---------------------------------------------------------------------------------------------
// BACKLOG PA: THE LFO'S STEREO PHASE — a native pair (`SourceDesc::pairWhile`), mono and free at 0.
TEST_CASE("backlog PA: an LFO is a pair exactly while its Stereo Phase is not zero — the knob crossing zero re-runs the drain, both ways") {
	namespace lfo = mod::lfo;
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t l = e.addDevice(lfo::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const size_t phase = static_cast<size_t>(lfo::Param::StereoPhase);
	e.setField(l, static_cast<size_t>(lfo::Param::Mode), static_cast<float>(static_cast<int>(lfo::Mode::Free)));
	e.setRouteAmount(test::outputOf(e, l, 0), test::controlOf(e, f, cutoff), 0.1f);
	block(e);
	// AT 0: MONO — no pair, no stereo-live cutoff, and the right ear is the left.
	CHECK_FALSE(e.emitsPairForTest(l, 0));
	CHECK_FALSE(e.stereoLiveForTest(f, cutoff));
	CHECK(e.publishedOutputRAt(l, 0) == e.publishedOutputAt(l, 0));
	e.setField(l, phase, static_cast<float>(M_PI / 2.0));
	for (int b = 0; b < 8; ++b) {
		block(e);
	}
	CHECK(e.emitsPairForTest(l, 0));
	CHECK(e.stereoLiveForTest(f, cutoff));
	CHECK(e.publishedOutputRAt(l, 0) != e.publishedOutputAt(l, 0));
	// THE CUTOFF'S RIGHT EAR from the LFO's right ear, restated: base + amount × span × R.
	{
		const device::ParamDesc& p = flt::kDescriptor.params[cutoff];
		const double base = e.fieldAt(f, cutoff);
		const double span = device::routeSpanOf(p);
		const float wantL = static_cast<float>(device::withRouteOffset(p, base, 0.1 * e.publishedOutputAt(l, 0) * span));
		const float wantR = static_cast<float>(device::withRouteOffset(p, base, 0.1 * e.publishedOutputRAt(l, 0) * span));
		CHECK(e.resolvedFieldForTest(f, cutoff) == doctest::Approx(wantL).epsilon(1e-6));
		CHECK(e.resolvedFieldRForTest(f, cutoff) == doctest::Approx(wantR).epsilon(1e-6));
	}
	// AND BACK TO 0: mono again.
	e.setField(l, phase, 0.0f);
	block(e);
	CHECK_FALSE(e.emitsPairForTest(l, 0));
	CHECK_FALSE(e.stereoLiveForTest(f, cutoff));
	block(e);
	CHECK(e.publishedOutputRAt(l, 0) == e.publishedOutputAt(l, 0));
}

TEST_CASE("backlog PA: an LFO's per-voice right ear reaches a Poly filter's right ear, voice by voice") {
	namespace lfo = mod::lfo;
	RackEngine e { kAxes, hostCatalogue() };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	const size_t l = e.addDevice(lfo::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	e.setField(f, static_cast<size_t>(flt::Param::Poly), 1.0f);
	e.setField(l, static_cast<size_t>(lfo::Param::Mode), static_cast<float>(static_cast<int>(lfo::Mode::Free)));
	e.setField(l, static_cast<size_t>(lfo::Param::StereoPhase), static_cast<float>(M_PI / 2.0));
	REQUIRE(lfo::kDescriptor.outputs[0].perVoice);
	e.setRouteAmount(test::outputOf(e, l, 0), test::controlOf(e, f, cutoff), 0.1f);
	for (int b = 0; b < 12; ++b) {
		block(e);
	}
	REQUIRE(e.emitsPairForTest(l, 0));
	// THE HOST WRITES EACH VOICE'S READING (the synth's job): both ears, from the device, for a note no voice holds —
	// which is the device's one shape, so the per-voice pair is the collapsed one.
	const DeviceInstanceId id = test::idAt(e, l);
	const float wantedL = e.outputForNoteAt(l, 0, -1, -1, -1);
	for (size_t v = 0; v < 2; ++v) {
		e.writeOutputForVoice(id, 0, v, wantedL);
		e.writeOutputPairForVoice(id, 0, v, -1, -1, -1, false);
	}
	const float wantedR = e.publishedOutputRAt(l, 0);
	REQUIRE(std::abs(wantedR - wantedL) > 1e-3);
	block(e);
	const device::ParamDesc& p = flt::kDescriptor.params[cutoff];
	const double base = e.fieldAt(f, cutoff);
	const double span = device::routeSpanOf(p);
	for (size_t v = 0; v < 2; ++v) {
		CAPTURE(v);
		CHECK(e.resolvedFieldForVoiceForTest(f, cutoff, v)
		      == static_cast<float>(device::withRouteOffset(p, base, 0.1 * wantedL * span)));
		CHECK(e.resolvedFieldRForVoiceForTest(f, cutoff, v)
		      == static_cast<float>(device::withRouteOffset(p, base, 0.1 * wantedR * span)));
	}
}

// ---------------------------------------------------------------------------------------------
// BACKLOG PA ON THE MSEG: its Stereo Phase is the same native pair (`SourceDesc::pairWhile`), mono and free at 0.
TEST_CASE("backlog PA (MSEG): an MSEG is a pair exactly while its Stereo Phase is not zero — the knob crossing zero re-runs the drain, both ways") {
	namespace mseg = mod::mseg;
	RackEngine e { kAxes, hostCatalogue() };
	e.prepare(48000.0, 4096);
	const size_t m = e.addDevice(mseg::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const size_t phase = static_cast<size_t>(mseg::Param::StereoPhase);
	// FREE, so the shape runs without a note and both ears move.
	e.setField(m, static_cast<size_t>(mseg::Param::Mode), static_cast<float>(static_cast<int>(mod::Mode::Free)));
	e.setRouteAmount(test::outputOf(e, m, 0), test::controlOf(e, f, cutoff), 0.1f);
	block(e);
	CHECK_FALSE(e.emitsPairForTest(m, 0));
	CHECK_FALSE(e.stereoLiveForTest(f, cutoff));
	CHECK(e.publishedOutputRAt(m, 0) == e.publishedOutputAt(m, 0));
	e.setField(m, phase, static_cast<float>(M_PI / 2.0));
	for (int b = 0; b < 8; ++b) {
		block(e);
	}
	CHECK(e.emitsPairForTest(m, 0));
	CHECK(e.stereoLiveForTest(f, cutoff));
	CHECK(e.publishedOutputRAt(m, 0) != e.publishedOutputAt(m, 0));
	{
		const device::ParamDesc& p = flt::kDescriptor.params[cutoff];
		const double base = e.fieldAt(f, cutoff);
		const double span = device::routeSpanOf(p);
		const float wantL = static_cast<float>(device::withRouteOffset(p, base, 0.1 * e.publishedOutputAt(m, 0) * span));
		const float wantR = static_cast<float>(device::withRouteOffset(p, base, 0.1 * e.publishedOutputRAt(m, 0) * span));
		CHECK(e.resolvedFieldForTest(f, cutoff) == doctest::Approx(wantL).epsilon(1e-6));
		CHECK(e.resolvedFieldRForTest(f, cutoff) == doctest::Approx(wantR).epsilon(1e-6));
	}
	e.setField(m, phase, 0.0f);
	block(e);
	CHECK_FALSE(e.emitsPairForTest(m, 0));
	CHECK_FALSE(e.stereoLiveForTest(f, cutoff));
	block(e);
	CHECK(e.publishedOutputRAt(m, 0) == e.publishedOutputAt(m, 0));
}

TEST_CASE("backlog PA (MSEG): an MSEG's per-voice right ear reaches a Poly filter's right ear, voice by voice") {
	namespace mseg = mod::mseg;
	RackEngine e { kAxes, hostCatalogue() };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	const size_t m = e.addDevice(mseg::kTraits.tag);
	const size_t f = e.addDevice(flt::kTraits.tag);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	e.setField(f, static_cast<size_t>(flt::Param::Poly), 1.0f);
	e.setField(m, static_cast<size_t>(mseg::Param::Mode), static_cast<float>(static_cast<int>(mod::Mode::Free)));
	e.setField(m, static_cast<size_t>(mseg::Param::StereoPhase), static_cast<float>(M_PI / 2.0));
	REQUIRE(mseg::kDescriptor.outputs[0].perVoice);
	e.setRouteAmount(test::outputOf(e, m, 0), test::controlOf(e, f, cutoff), 0.1f);
	for (int b = 0; b < 12; ++b) {
		block(e);
	}
	REQUIRE(e.emitsPairForTest(m, 0));
	const DeviceInstanceId id = test::idAt(e, m);
	const float wantedL = e.outputForNoteAt(m, 0, -1, -1, -1);
	for (size_t v = 0; v < 2; ++v) {
		e.writeOutputForVoice(id, 0, v, wantedL);
		e.writeOutputPairForVoice(id, 0, v, -1, -1, -1, false);
	}
	const float wantedR = e.publishedOutputRAt(m, 0);
	REQUIRE(std::abs(wantedR - wantedL) > 1e-3);
	block(e);
	const device::ParamDesc& p = flt::kDescriptor.params[cutoff];
	const double base = e.fieldAt(f, cutoff);
	const double span = device::routeSpanOf(p);
	for (size_t v = 0; v < 2; ++v) {
		CAPTURE(v);
		CHECK(e.resolvedFieldForVoiceForTest(f, cutoff, v)
		      == static_cast<float>(device::withRouteOffset(p, base, 0.1 * wantedL * span)));
		CHECK(e.resolvedFieldRForVoiceForTest(f, cutoff, v)
		      == static_cast<float>(device::withRouteOffset(p, base, 0.1 * wantedR * span)));
	}
}
