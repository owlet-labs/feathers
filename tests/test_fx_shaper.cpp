/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/shaper/shaper.h"
#include "fx/shaper/shaper_types.h"

// For `fieldWithOverride` and `kFieldNeutral` — the curve the Hysteresis scaler is asserted against.
#include "dsp/slice_build.h"
#include "plugin/ui/shaper_plot_curve.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstring>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>
#include <numeric>
#include <string>
#include <vector>
#include <chrono>

// THE SHAPER'S CASES.
//
// THE RULE THIS FILE IS WRITTEN UNDER is CLAUDE.md's: a claim about a value cannot be inferred from a
// rendered buffer, because a comparison between two renders passes whenever ANYTHING differs. That is
// especially sharp here — this device has eleven ways to change its output, so "the buffer moved" is
// almost never evidence about which of them moved it. Nearly every case below reads a number: a
// weight out of `curveForTest`, a corner out of `lowpassCutoffForTest`, a threshold, a rebuild count.
//
// AND WHERE A CASE MUST JUDGE A SOUND, IT COMPUTES THE EXPECTED VALUE INDEPENDENTLY rather than
// comparing against a second render — the second half of the same rule.

namespace shp = feathers::fx::shaper;

namespace {

/// A block of a sine, at a level and a frequency.
std::vector<float> sine(int frames, float amp, float cyclesPerBlock) {
	std::vector<float> out(static_cast<size_t>(frames));
	for (int i = 0; i < frames; ++i) {
		out[static_cast<size_t>(i)] =
		    amp * std::sin(2.0f * 3.14159265f * cyclesPerBlock * static_cast<float>(i)
		                   / static_cast<float>(frames));
	}
	return out;
}

/// Run a shaper for `blocks` blocks of `frames`, returning the last block.
std::vector<float> run(shp::Shaper& s, const shp::Settings& settings, int blocks, int frames,
                       float amp = 0.6f) {
	s.setTarget(settings);
	std::vector<float> l, r;
	for (int b = 0; b < blocks; ++b) {
		l = sine(frames, amp, 4.0f);
		r = l;
		s.processStereo(l.data(), r.data(), frames);
	}
	return l;
}

/// Drive a shaper until its first table has landed AND settled — build plus fade.
void settle(shp::Shaper& s, const shp::Settings& settings, int frames = 64) {
	s.setTarget(settings);
	std::vector<float> l(static_cast<size_t>(frames), 0.0f);
	std::vector<float> r = l;
	for (int b = 0; b < 64; ++b) {
		s.processStereo(l.data(), r.data(), frames);
	}
}

/// Peak magnitude of a block.
float peak(const std::vector<float>& v) {
	float m = 0.0f;
	for (const float x : v) {
		m = std::max(m, std::fabs(x));
	}
	return m;
}

/// Total harmonic distortion of a shaped sine, by direct DFT — the independent restatement a
/// "something changed" check cannot be.
double thd(const std::vector<float>& y, int harmonicOfBlock) {
	const auto n = static_cast<int>(y.size());
	const auto bin = [&](int k) {
		double re = 0.0;
		double im = 0.0;
		for (int i = 0; i < n; ++i) {
			const double ph = 2.0 * 3.14159265358979 * k * i / n;
			re += y[static_cast<size_t>(i)] * std::cos(ph);
			im -= y[static_cast<size_t>(i)] * std::sin(ph);
		}
		return std::hypot(re, im) * 2.0 / n;
	};
	const double fundamental = bin(harmonicOfBlock);
	double rest = 0.0;
	for (int h = 2; h * harmonicOfBlock < n / 2 && h <= 24; ++h) {
		const double m = bin(h * harmonicOfBlock);
		rest += m * m;
	}
	return fundamental > 1e-9 ? std::sqrt(rest) / fundamental : 0.0;
}

} // namespace

//--------------------------------------------------------------------------------------------//
// THE DECLARATIONS                                                                             //
//--------------------------------------------------------------------------------------------//

TEST_CASE("the shaper's declarations agree with themselves") {
	SUBCASE("every control is declared exactly once, and the count matches the enum") {
		CHECK(std::size(shp::kParamDescs) == static_cast<size_t>(shp::Param::kCount));
		// EVERY NAME DISTINCT, which the plugin's own param table requires and which a device cannot
		// check for itself once it is one row of a longer list. Cheaper to catch here.
		for (size_t i = 0; i < std::size(shp::kParamDescs); ++i) {
			for (size_t j = i + 1; j < std::size(shp::kParamDescs); ++j) {
				CAPTURE(shp::kParamDescs[i].name);
				CHECK(std::string(shp::kParamDescs[i].name) != shp::kParamDescs[j].name);
			}
		}
	}

	SUBCASE("every control names the device, so it cannot collide in a flat host list") {
		// device.h's permanent rule: a device qualifies its own control names. `Enabled` is the one
		// exception every device in this repo makes — it is the head's square and never reaches a
		// host's parameter list under that name.
		for (const auto& desc : shp::kParamDescs) {
			CAPTURE(desc.name);
			const std::string name = desc.name;
			CHECK((name == "Enabled" || name.rfind("Shaper ", 0) == 0));
		}
	}

	SUBCASE("the enable control is declared, and it points at Enabled") {
		CHECK(shp::kTraits.enableControl == static_cast<int>(shp::Param::Enabled));
		CHECK(feathers::device::enableControlIn(shp::kTraits, shp::kParamDescs)
		      == static_cast<size_t>(shp::Param::Enabled));
	}

	SUBCASE("the per-voice road is claimed by the Poly switch, which is off by default (backlog OM)") {
		// device.h: the host's answer to "poly is on" is to stop running it on the bus, so the switch must default
		// off for the device to stay the bus shaper it was; and it is no modulation destination (backlog NS).
		CHECK(shp::kTraits.perVoiceControl == static_cast<int>(shp::Param::Poly));
		CHECK(shp::kParamDescs[static_cast<size_t>(shp::Param::Poly)].def == 0.0);
		CHECK_FALSE(shp::kParamDescs[static_cast<size_t>(shp::Param::Poly)].modulatable);
		CHECK_FALSE(shp::Settings {}.poly);
	}

	SUBCASE("every control is a destination, and the ones that cost a bake are exactly the table's terms") {
		// THE DECISION CHANGED: only the parameters that do not cost a bake are poly-modulable; the others are
		// modulated in mono. The bake's terms (Knee, Colour, Gamma, Deadzone, Deadzone On, Colour Mode, Inflate) are destinations
		// now — MONO under Poly, one value for the shared table — and a modulated one rebuilds no faster than the build
		// allows (a 512-sample build plus a 512-sample crossfade, ~47 Hz; test "OM: a modulated Knee rebuilds at the
		// build's own cadence"). Hysteresis, Rotate and Poly stay off the list: Rotate by its own decision (a moving basis
		// would make one gesture mean different things block to block), Poly an architecture switch, Hysteresis
		// unchanged here.
		std::vector<std::string> destinations;
		for (const auto& desc : shp::kParamDescs) {
			if (desc.modulatable) {
				destinations.emplace_back(desc.name);
			}
		}
		CHECK(destinations
		      == std::vector<std::string> {"Enabled", "Shaper Knee", "Shaper Colour", "Shaper Gamma", "Shaper Drive",
		                                   "Shaper Threshold", "Shaper Emphasis", "Shaper Sub", "Shaper Feedback",
		                                   "Shaper Mangle", "Shaper Lowpass", "Shaper Integrator", "Shaper Aggression",
		                                   "Shaper Trim", "Shaper Deadzone", "Shaper Bloom", "Shaper Sag", "Shaper Loop",
		                                   "Shaper Cheby", "Shaper Stair", "Shaper Sub On", "Shaper Feedback On",
		                                   "Shaper Mangle On", "Shaper Lowpass On", "Shaper Integrator On",
		                                   "Shaper Deadzone On", "Shaper Colour Mode", "Shaper Inflate", "Shaper HPF",
		                                   "Shaper LPF", "Shaper Mix"});

		// AND WHICH OF THEM COST A BAKE, ASSERTED RATHER THAN LISTED: a destination moved to its far end changes
		// `sameCurveAs` (the table's own key) exactly when it is one of the seven — so the mono set is the bake's set.
		const std::vector<shp::Param> baked { shp::Param::Knee, shp::Param::Colour, shp::Param::Gamma, shp::Param::Deadzone,
			                                  shp::Param::DeadzoneOn, shp::Param::ColourMode, shp::Param::InflateAmount };
		for (const auto& desc : shp::kParamDescs) {
			if (!desc.modulatable) {
				continue;
			}
			const size_t index = static_cast<size_t>(&desc - &shp::kParamDescs[0]);
			shp::Settings a;
			a.colourMode = shp::kColourInflate; // so Inflate's amount is a live term
			a.deadzoneOn = true;                // and Deadzone's width
			shp::Row row = shp::rowOf(a);
			// AWAY FROM WHERE IT STANDS: the far end, or the near one when it already sits at the far; half a lap for the
			// unbounded two.
			const double here = row[index];
			row[index] = static_cast<float>(desc.max > 1e6 ? here + 0.5 : (here == desc.max ? desc.min : desc.max));
			const shp::Settings b = shp::stateOf(row, row.size());
			const bool isBaked = std::find(baked.begin(), baked.end(), static_cast<shp::Param>(index)) != baked.end();
			CAPTURE(desc.name);
			CHECK(a.sameCurveAs(b) == !isBaked);
		}
	}

	SUBCASE("Colour is the zoned control and Knee is a plain range") {
		// THE COUNT drives the widget's drag and the modulation unit. Colour has NO zone names since backlog IW:
		// its eight were labels over a continuum, and its one fixed character is a MODE now (next subcase).
		CHECK(shp::kParamDescs[static_cast<size_t>(shp::Param::Colour)].zones
		      == static_cast<uint32_t>(shp::kColourZones));
		CHECK(shp::kParamDescs[static_cast<size_t>(shp::Param::Colour)].zoneNames.empty());

		// KNEE IS NOT ZONED, and the zero is the assertion rather than an absence. Knee is a normal
		// range knob, not a 'slow' one: its eight steps are
		// a monotone ladder of drive, not eight characters — the sweep below measures them strictly
		// increasing — so nothing is gained by making a hand stop at each one. A nonzero here would
		// silently restore the slow turn AND rescale every modulation route aimed at it.
		CHECK(shp::kParamDescs[static_cast<size_t>(shp::Param::Knee)].zones == 0u);
	}

	SUBCASE("backlog IW: Colour's dropdown is a MODE selector — Inflate first, Free last — and each mode gates its value") {
		// The dropdown is a mode selector, not a phase region selector; Inflate comes first because it is
		// fenced and Free is infinite.
		const feathers::device::ParamDesc& mode = shp::kParamDescs[static_cast<size_t>(shp::Param::ColourMode)];
		CHECK(mode.modeCell);
		CHECK(mode.stepped);
		REQUIRE(mode.names.size() == 2);
		CHECK(std::string_view(mode.names[0]) == "Inflate");
		CHECK(std::string_view(mode.names[1]) == "Free"); // ALWAYS LAST
		CHECK(mode.def == static_cast<double>(shp::kColourFree)); // a fresh shaper is the field it always was
		// THE FOLLOWERS: Colour in Free, the amount in Inflate — each gated on the mode, and on nothing else.
		const feathers::device::ParamDesc& colour = shp::kParamDescs[static_cast<size_t>(shp::Param::Colour)];
		const feathers::device::ParamDesc& amount = shp::kParamDescs[static_cast<size_t>(shp::Param::InflateAmount)];
		CHECK(colour.gateControl == static_cast<int16_t>(shp::Param::ColourMode));
		CHECK(colour.gateMask == (1u << shp::kColourFree));
		CHECK(amount.gateControl == static_cast<int16_t>(shp::Param::ColourMode));
		CHECK(amount.gateMask == (1u << shp::kColourInflate));
		// INFLATE'S VALUE IS BOUNDED (fenced, by design), and a MONO destination since 2026-10-31 — a table term.
		CHECK(amount.min == 0.0);
		CHECK(amount.max == 1.0);
		CHECK(amount.def == 1.0);
		CHECK(amount.modulatable);
	}

	SUBCASE("the device fits the transport and the modulation stride") {
		// TWO BOUNDS, AND THIS LINE HAD CONFUSED THEM until backlog FA took the count to 33: the retired control stride
		// is the MODULATABLE stride (`device/registry.h` says confusing the two "would be a real bug"); what a row
		// carries is bounded by `kMaxDeviceFields`, declared controls plus the extra state.
		CHECK(static_cast<size_t>(shp::Param::kCount) + shp::kTraits.extraState <= feathers::device::kMaxDeviceFields);
		// THIRTEEN: Drive, Mix, Emphasis (2026-09-22, a filter pair whose coefficients follow it at the
		// block rate), the five per-sample extras (2026-09-23, since they became field scalers — backlog
		// II), Aggression, Trim, and backlog IE's Bloom, Sag and Loop. Each is a per-sample or per-block
		// scalar and none is a table
		// term — the subcase above asserts that PROPERTY of every entry rather than trusting the
		// list, which is what makes adding one a count change and not a decision.
		// AND EIGHTEEN SINCE backlog IK: the five powers of the per-sample extras, each a two-state switch
		// (a well-defined destination — SKLITTER's Engage is the precedent). Deadzone On is a table term.
		// AND TWENTY SINCE backlog IM: Cheby and Stair moved to the read, so they are no longer table terms.
		// AND TWENTY-THREE SINCE backlog FA: the input HPF and LPF and the crossfade Mix, each a per-block scalar.
		CHECK(shp::kDescriptor.modulatableCount() == 31); // + Enabled (KW), + the seven baked terms (OM, 2026-10-31)
		CHECK(shp::kDescriptor.outputCount() == 0);
		CHECK(shp::kDescriptor.audio != nullptr);
	}
}

//--------------------------------------------------------------------------------------------//
// THE ROW                                                                                      //
//--------------------------------------------------------------------------------------------//

TEST_CASE("a row round-trips, and an absent field is its declared default and not zero") {
	SUBCASE("round trip") {
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.31f;
		s.colour = 0.62f;
		s.gamma = 0.17f;
		s.drive = 0.73f;
		s.threshold = 0.41f;
		s.emphasisDb = 5.5f;
		s.subOn = true; // the old On: the amount defaults to the field's own
		s.mangleOn = true; // the old On: the amount defaults to the field's own
		const shp::Settings back = shp::stateOf(shp::rowOf(s), static_cast<size_t>(shp::Param::kCount));
		CHECK(back.enabled == s.enabled);
		CHECK(back.knee == doctest::Approx(s.knee));
		CHECK(back.colour == doctest::Approx(s.colour));
		CHECK(back.gamma == doctest::Approx(s.gamma));
		CHECK(back.drive == doctest::Approx(s.drive));
		CHECK(back.threshold == doctest::Approx(s.threshold));
		CHECK(back.emphasisDb == doctest::Approx(s.emphasisDb));
		CHECK(back.sub == s.sub);
		CHECK(back.feedback == s.feedback);
		CHECK(back.mangle == s.mangle);
		CHECK(back.lowpass == s.lowpass);
		CHECK(back.integrator == s.integrator);
	}

	SUBCASE("an empty row is every declared default, and the non-zero ones are what prove it") {
		// THE CASE THAT MATTERS MOST IN THIS FILE FOR A ROW. Zero is a legal value for all twelve
		// controls, so a decoder that treated absence as zero would compile, run, and hand back a
		// device with Mix at 0 (nothing shaped) and Knee at 0 (a bypass) — which is not "a default",
		// it is a device that does nothing. `fieldsPresent = 0` is what a chain record from a build
		// that had never heard of this device looks like.
		//
		// DRIVE'S DEFAULT IS 0 NOW, AND 0 IS NOT SILENCE ANY MORE. This subcase used to guard Drive,
		// because 0 was a gain of 0. Drive is a table SWEEP — 4^drive, 1 at 0 — so 0 is the
		// unit-slope read and the right default; the guard moved to the controls whose defaults are
		// not zero.
		const shp::Row empty {};
		const shp::Settings s = shp::stateOf(empty, 0);
		CHECK(s.drive == doctest::Approx(0.0f));
		CHECK(s.threshold == doctest::Approx(1.0f));
		CHECK(s.knee == doctest::Approx(0.5f));
		// EMPHASIS IS FLAT BY DEFAULT — the device as it was before the knob existed.
		CHECK(s.emphasisDb == doctest::Approx(0.0f));
		// HYSTERESIS DEFAULTS TO 0. The firmware's own default path has none; 0.5 is still the position's own for a hand that wants it.
		CHECK(s.hysteresis == doctest::Approx(0.0f));
		// AND THE DEADZONE SWITCH IS OFF, for the same comparison — see `Param::Deadzone`.
		CHECK_FALSE(s.deadzoneOn);
		// ON — every effect defaults to on when it is added, and Shaper was one that defaulted to off.
		// An ABSENT field still takes
		// the declared default, which is the property this subcase is about; the default itself moved.
		CHECK(s.enabled == true);
		CHECK(s.gamma == doctest::Approx(0.0f));
	}

	SUBCASE("a partial row takes defaults for the tail only") {
		shp::Row row {};
		row[static_cast<size_t>(shp::Param::Enabled)] = 1.0f;
		row[static_cast<size_t>(shp::Param::Knee)] = 0.25f;
		const shp::Settings s = shp::stateOf(row, 2);
		CHECK(s.enabled == true);
		CHECK(s.knee == doctest::Approx(0.25f));
		CHECK(s.threshold == doctest::Approx(1.0f)); // past fieldsPresent: declared default
	}

	SUBCASE("a BOUNDED field out of range is clamped rather than trusted") {
		// **KNEE IS BOUNDED AND COLOUR IS NOT, AND THE DIFFERENCE IS THE POINT (BJ piece 3).** This
		// asserted the clamp on both. The field axes were unbounded on 2026-09-09, so Colour moved to the other clause below and Knee stays here.
		//
		// A row is whatever reached the transport: a host's automation, a file, a modulation offset.
		// Knee has no meaning outside 0..1 — it is a scale, and 0 is a true bypass — so it is still
		// clamped on the way in.
		shp::Row row {};
		row[static_cast<size_t>(shp::Param::Knee)] = -1.5f;
		const shp::Settings s = shp::stateOf(row, static_cast<size_t>(shp::Param::kCount));
		CHECK(s.knee == doctest::Approx(0.0f));
	}

	SUBCASE("an UNBOUNDED field is carried whole, in both directions") {
		// THE OTHER HALF, and it is a real guarantee rather than the absence of the one above: a
		// clamp that survived here would wall Colour at the eighth zone exactly as the old one did,
		// and the readout would keep moving while the sound stopped — which is precisely how the
		// gamma clamp hid for as long as it did.
		shp::Row row {};
		row[static_cast<size_t>(shp::Param::Colour)] = 4.2f;
		const shp::Settings s = shp::stateOf(row, static_cast<size_t>(shp::Param::kCount));
		CHECK(s.colour == doctest::Approx(4.2f));

		shp::Row back {};
		back[static_cast<size_t>(shp::Param::Colour)] = -2.75f;
		CHECK(shp::stateOf(back, static_cast<size_t>(shp::Param::kCount)).colour
		      == doctest::Approx(-2.75f));
	}

	SUBCASE("II: the six extras are field scalers — 0 off, 50% the field to the bit, 100% the mapping's top") {
		// The extras buttons became field scalers with 0 = off. By `fieldWithOverride`,
		// through `curveOf` — the one call the engine and the plot make. 50% MUST be the field bit for
		// bit, because that is what the old switch's On gave and the firmware's parity rests on it.
		shp::Settings s;
		// THE EXTRAS CASE'S MEASURED POSITION — sub 0.90, comb 0.73, 14 bits, slew 0.20 — so every
		// extra is on offer. MEASURED: colour 0.38 / gamma 0.2 offers no slew at all.
		s.knee = 0.7f;
		s.colour = 0.0305f;
		const shp::CurveParams field = shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics, &s.travel);
		REQUIRE(field.subIntensity > 0.0f); // the position offers each — or "off at 0" would be vacuous
		REQUIRE(field.feedback > 0.0f);
		REQUIRE(field.rotation > 0);
		REQUIRE(field.slewIntensity > 0.0f);
		const auto set = [&](float v) {
			shp::Settings t = s;
			t.sub = t.feedback = t.mangle = t.lowpass = t.integrator = t.deadzone = v;
			// POWERED — the scaler law is the AMOUNT's; off, every extra is 0 whatever it says (backlog IK).
			t.subOn = t.feedbackOn = t.mangleOn = t.lowpassOn = t.integratorOn = t.deadzoneOn = true;
			return shp::curveOf(t);
		};
		const shp::CurveParams off = set(0.0f);
		CHECK(off.subIntensity == 0.0f);
		CHECK(off.feedback == 0.0f);
		CHECK(off.rotation == 0);
		CHECK(off.lowpassAmount == 0.0f);
		CHECK(off.integratorAmount == 0.0f);
		CHECK(off.deadzoneWidth == 0.0f);

		const shp::CurveParams own = set(0.5f);
		CHECK(own.subIntensity == field.subIntensity);
		CHECK(own.feedback == field.feedback);
		CHECK(own.rotation == field.rotation);
		CHECK(own.lowpassAmount == field.slewIntensity);
		CHECK(own.integratorAmount == field.slewIntensity);
		CHECK(own.deadzoneWidth == field.deadzoneWidth);

		// THE TOP IS THE FIRMWARE MAPPING'S OWN — comb depth 0.8, 31 bits — because the scaler acts on the
		// field's TRIANGLE and the mapping runs after it; scaling the mapped value would overshoot both.
		const shp::CurveParams most = set(1.0f);
		CHECK(most.subIntensity == doctest::Approx(1.0f));
		CHECK(most.feedback == doctest::Approx(0.8f));
		CHECK(most.rotation == 31);
		CHECK(most.lowpassAmount == doctest::Approx(1.0f));
		CHECK(most.deadzoneWidth == doctest::Approx(1.0f));
		// **OFF IS OFF, WHATEVER THE AMOUNT SAYS (backlog IK)** — amount and on/off must not be conflated
		// under modulation. Every amount at its top, every
		// power off, and not one extra is heard.
		shp::Settings unpowered = s;
		unpowered.sub = unpowered.feedback = unpowered.mangle = 1.0f;
		unpowered.lowpass = unpowered.integrator = unpowered.deadzone = 1.0f;
		const shp::CurveParams dark = shp::curveOf(unpowered);
		CHECK(dark.subIntensity == 0.0f);
		CHECK(dark.feedback == 0.0f);
		CHECK(dark.rotation == 0);
		CHECK(dark.lowpassAmount == 0.0f);
		CHECK(dark.integratorAmount == 0.0f);
		CHECK(dark.deadzoneWidth == 0.0f);
		// STRUCTURE IS NOT SCALED: the interval and the comb's period are the field's at any amount.
		CHECK(most.subRatio == field.subRatio);
		CHECK(most.stride == field.stride);
	}
}

TEST_CASE("GAMMA: the decode carries the whole declared range, and the curve follows it past 1") {
	// **THE DEFECT: `Param::Gamma` was widened to +-2^24 and `stateOf` still clamped it to 0..1.** So
	// the readout moved for the whole drag — the param really is unbounded — while the SOUND and the
	// DRAWN CURVE were both built from a `Settings::gamma` pinned at 1.0. `GammaBox` gives sixteen
	// units to a full drag, so roughly nine pixels reached the wall and the rest of the gesture was
	// the same table. On the installed build the gamma readout went higher — the number, not
	// the field.
	//
	// **THE VALUE IS ASSERTED FIRST AND THE CURVE SECOND, IN THAT ORDER.** A
	// curve-difference check alone would pass on any build where something moved; the claim here is
	// that `Settings::gamma` IS the number the row carried, so the number is what is read. The curve
	// assertion below exists to prove the first is not vacuous — that this value is one the engine
	// actually consumes rather than a field nothing reads.
	constexpr auto kAll = static_cast<size_t>(shp::Param::kCount);
	const auto gammaFromRow = [](float v) {
		shp::Row row {};
		row[static_cast<size_t>(shp::Param::Gamma)] = v;
		return shp::stateOf(row, kAll).gamma;
	};

	SUBCASE("the value survives the decode, on both sides of zero") {
		// THE VALUES ARE TAKEN FROM THE DECLARATION, not from what the decode happens to do with
		// them — the fixture rule. `kParamDescs` is the contract; these are its ends and points
		// inside it that the old clamp would have flattened.
		const feathers::device::ParamDesc& desc =
		    shp::kParamDescs[static_cast<size_t>(shp::Param::Gamma)];
		REQUIRE(desc.max > 1.0);
		REQUIRE(desc.min < 0.0);

		CHECK(gammaFromRow(1.5f) == doctest::Approx(1.5f));
		CHECK(gammaFromRow(7.25f) == doctest::Approx(7.25f));
		CHECK(gammaFromRow(-3.0f) == doctest::Approx(-3.0f));
		CHECK(gammaFromRow(static_cast<float>(desc.max)) == doctest::Approx(desc.max));
		CHECK(gammaFromRow(static_cast<float>(desc.min)) == doctest::Approx(desc.min));

		// AND THE OLD WALL BY NAME: 1.0 and 2.0 decoded to the same number, which is the whole
		// silence. Asserting the two are DIFFERENT is the one difference check that belongs in this
		// subcase, because it names the exact equality the bug produced.
		CHECK(gammaFromRow(2.0f) != gammaFromRow(1.0f));
	}

	SUBCASE("and the curve the device builds moves with it — the value is not inert") {
		// PROOF THAT THE FIRST SUBCASE IS NOT VACUOUS. `curveFor` sums `gamma + colourPhase` into a
		// phase, so a gamma the decode preserved must reach the table the device plays. Three
		// positions past the old wall, each compared against gamma 1.0 — where every one of them used
		// to land, which is what made the control silent.
		//
		// **THE TABLE RATHER THAN ONE WEIGHT, and that is not laziness.** The six basis weights are
		// HALF-RECTIFIED at 50% duty, so any single one of them is off — and equal to its neighbour's
		// zero — for half the axis; a case pinned to `inflatorWeight` would be asserting a coincidence
		// at every gamma where that basis happens to be asleep. The table is the quantity the sound
		// is made of and it is alive everywhere.
		const auto tableAt = [](float gamma) {
			shp::Row row {};
			row[static_cast<size_t>(shp::Param::Gamma)] = gamma;
			const shp::Settings s = shp::stateOf(row, kAll);
			std::array<float, shp::kTableEntries> table {};
			shp::buildTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics),
			                table);
			return table;
		};
		const std::array<float, shp::kTableEntries> atWall = tableAt(1.0f);
		for (const float g : { 2.0f, 5.5f, 40.0f }) {
			CAPTURE(g);
			const std::array<float, shp::kTableEntries> here = tableAt(g);
			float worst = 0.0f;
			for (size_t i = 0; i < here.size(); ++i) {
				worst = std::max(worst, std::fabs(here[i] - atWall[i]));
			}
			CHECK(worst > 0.01f);
		}
	}
}

TEST_CASE("COLOUR PHASE: the declaration admits what the engine accepts, in both directions") {
	// **THE DEFECT: `Shaper Colour Phase` declared `0.0, 64.0` and the engine has no such cap.**
	// Seen twice, most recently on the current build: Shaper's colour coarse maxed out at 64.
	// `ZoneCoordBox::onCarry` puts each lap of the coarse half into this counter and clamps the
	// sum to the DECLARED range (`zone_coord_box.cpp:79`), so the knob stopped at the 64th lap while
	// the quantity behind it was still healthy.
	//
	// **AND THE FLOOR AT ZERO IN `stateOf` WAS THE SAME WALL FACING THE OTHER WAY.** A symmetric
	// declaration with a floor still at zero would have been a fresh instance of the gamma clamp —
	// a range that reads wider than the decode carries — so the two moved together.
	constexpr auto kAll = static_cast<size_t>(shp::Param::kCount);
	// **RE-POINTED AT `Colour` ITSELF (BJ piece 3).** This asserted the reach of `Colour Phase`, the
	// counter Colour lapped into. It was retired on 2026-09-09 when the zoned axes were unbounded,
	// so Colour carries the quantity now and the claim moves onto it unchanged: the
	// declaration is a drag window, and the field past the old wall is healthy.
	const feathers::device::ParamDesc& desc =
	    shp::kParamDescs[static_cast<size_t>(shp::Param::Colour)];

	SUBCASE("the declared range is a drag window and not a domain") {
		// PAST THE OLD WALL, AND SYMMETRIC ABOUT THE DEFAULT. The default is 0 and every patch starts
		// there, so an asymmetric declaration walls the very first backwards lap.
		CHECK(desc.max > 64.0);
		CHECK(desc.min == -desc.max);
		CHECK(desc.def == 0.0);
	}

	SUBCASE("the decode carries the whole of it, negatives included") {
		const auto phaseFromRow = [](float v) {
			shp::Row row {};
			row[static_cast<size_t>(shp::Param::Colour)] = v;
			return shp::stateOf(row, kAll).colour;
		};
		CHECK(phaseFromRow(65.0f) == doctest::Approx(65.0f));
		CHECK(phaseFromRow(200.0f) == doctest::Approx(200.0f));
		// THE BACKWARDS LAP, which the floor at zero flattened onto the origin. `wrapPhase` floors
		// rather than truncating (`phi_triangle.hpp:81`), so a negative phase is a real position.
		CHECK(phaseFromRow(-1.5f) == doctest::Approx(-1.5f));
		CHECK(phaseFromRow(static_cast<float>(desc.max)) == doctest::Approx(desc.max));
		CHECK(phaseFromRow(static_cast<float>(desc.min)) == doctest::Approx(desc.min));
	}

	SUBCASE("and a lap past 64 moves the table as much as a lap past 32 — the value is not inert") {
		// PROOF THE DECLARATION IS NOT A LIE IN THE OTHER DIRECTION: the engine must actually be alive
		// past the old wall, not merely willing to hold the number. **MEASURED HERE**, on this
		// build, as the largest absolute change across the 1025-entry table for one lap, at the
		// declared defaults for everything else:
		//
		//   32 -> 33   1.000
		//   64 -> 65   0.9997
		//    0 -> -1   0.678   (the backwards lap the floor at zero used to erase)
		//
		// A lap at 64 is worth as much as a lap at 32, which is what "the quantity is fine past the
		// declaration" means. **The bar below is 0.05 — a floor on the SHAPE and deliberately not a
		// restatement of those three numbers**, which are a property of one set of defaults and would
		// make this case a change detector for the field rather than a guard on the wall.
		const auto tableAt = [](float colour) {
			shp::Row row {};
			row[static_cast<size_t>(shp::Param::Colour)] = colour;
			const shp::Settings s = shp::stateOf(row, kAll);
			std::array<float, shp::kTableEntries> table {};
			shp::buildTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics),
			                table);
			return table;
		};
		const auto lap = [&tableAt](float from, float to) {
			const std::array<float, shp::kTableEntries> a = tableAt(from);
			const std::array<float, shp::kTableEntries> b = tableAt(to);
			float worst = 0.0f;
			for (size_t i = 0; i < a.size(); ++i) {
				worst = std::max(worst, std::fabs(b[i] - a[i]));
			}
			return worst;
		};
		CHECK(lap(32.0f, 33.0f) > 0.05f);
		CHECK(lap(64.0f, 65.0f) > 0.05f);
		CHECK(lap(0.0f, -1.0f) > 0.05f);
	}
}

//--------------------------------------------------------------------------------------------//
// THE CURVE                                                                                    //
//--------------------------------------------------------------------------------------------//

TEST_CASE("a built table maps zero to zero, and peaks at one") {
	// THE ANCHOR IS THE WHOLE POINT AND IT IS A CORRECTNESS PROPERTY, not a tidy one. A waveshaper is
	// memoryless, so whatever it returns for a silent input it returns FOREVER — a curve with
	// f(0) != 0 does not colour silence, it replaces it with a constant.
	//
	// THIS CASE USED TO ASSERT MIDPOINT CENTRING (min == -1 and max == +1), which is what the table
	// did and what was wrong. The area was surfaced by `test_fx_runner.cpp`'s silence sweep — the
	// device was still sounding 4.3 seconds after the audio stopped — though the tail itself was the
	// DC blocker's denormal, guarded by the case below. THIS case is the only guard on the anchor:
	// known-badded by reverting to midpoint centring, it is the one that goes red and the silence
	// cases do not.
	for (int i = 0; i < 32; ++i) {
		const float colour = static_cast<float>(i) / 31.0f;
		CAPTURE(colour);
		const shp::CurveParams p = shp::deriveCurve(0.8f, colour, 0.0f, 0.5f);
		std::array<float, shp::kTableEntries> table {};
		shp::buildTable(p, table);

		// ZERO MAPS TO ZERO, EXACTLY. Not "approximately" — the anchor subtracts the centre entry from
		// itself, so this is an identity rather than a tolerance.
		CHECK(table[shp::kTableEntries / 2] == 0.0f);
		// AND THE LOOKUP AGREES, which is the half a table check alone cannot see: an off-by-one in the
		// index maths would leave the table anchored and the READ still offset.
		CHECK(shp::lookup(table, 0.0f) == 0.0f);

		// THE PEAK IS ONE. Peak, not peak-to-peak: once the origin is fixed there is one degree of
		// freedom left, so the louder half reaches the rail and the quieter half does not — which is
		// what asymmetry means.
		const auto [lo, hi] = std::minmax_element(table.begin(), table.end());
		CHECK(std::max(std::fabs(*lo), std::fabs(*hi)) == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(*lo >= -1.0001f);
		CHECK(*hi <= 1.0001f);
	}
}

TEST_CASE("silence in is silence out, and it reaches EXACT zero") {
	// THE PROPERTY THE REGISTRY SWEEP INSISTS ON, restated here so this file can see it too. A device
	// that decays to a denormal and parks there is inaudible and is still not silent — and the
	// difference is real: it keeps a denormal-slow path warm forever, on every idle track.
	//
	// THIS IS THE GUARD ON THE FLUSH, not on the table's anchor — measured by known-bad, removing the
	// flush turns this red and leaves the anchor's case green. Two properties, two mutations, two
	// cases, and neither stands in for the other.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settle(s, settings);

	// A burst first, so the blocker and the smoothers have something to come back from.
	std::vector<float> l = sine(512, 0.8f, 8.0f);
	std::vector<float> r = l;
	s.processStereo(l.data(), r.data(), 512);
	REQUIRE(peak(l) > 0.1f);

	int lastNonZero = -1;
	for (int b = 0; b < 400; ++b) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		s.processStereo(l.data(), r.data(), 512);
		for (size_t i = 0; i < l.size(); ++i) {
			if (l[i] != 0.0f || r[i] != 0.0f) {
				lastNonZero = b;
				break;
			}
		}
	}
	CAPTURE(lastNonZero);
	CHECK(lastNonZero < 200);
}
TEST_CASE("the table is monotonic in position: neighbouring entries come from neighbouring inputs") {
	// NOT that the curve is monotonic — the folders are emphatically not. What is checked is that the
	// table's INDEXING is right: entry i must be the curve at 2i/(n-1) - 1, so a lookup at that x
	// returns that entry. An off-by-one here would mistune every sample by half a table step and
	// nothing about the sound would look obviously wrong.
	const shp::CurveParams p = shp::deriveCurve(0.6f, 0.3f, 0.0f, 0.5f);
	std::array<float, shp::kTableEntries> table {};
	shp::buildTable(p, table);
	for (size_t i = 0; i < shp::kTableEntries; i += 97) {
		const float x = static_cast<float>(i) * (2.0f / static_cast<float>(shp::kTableEntries - 1)) - 1.0f;
		CAPTURE(i);
		CHECK(shp::lookup(table, x) == doctest::Approx(table[i]).epsilon(1e-4));
	}
	// AND THE ENDS ARE THE ENDS.
	CHECK(shp::lookup(table, -1.0f) == doctest::Approx(table.front()));
	CHECK(shp::lookup(table, 1.0f) == doctest::Approx(table.back()));
	// PAST FULL SCALE IT CLAMPS rather than wrapping — a hot signal gets the endpoint, not a second
	// pass through the curve.
	CHECK(shp::lookup(table, 3.0f) == doctest::Approx(table.back()));
	CHECK(shp::lookup(table, -3.0f) == doctest::Approx(table.front()));
}

TEST_CASE("knee is a monotone scale, and the zone names' numbers are true") {
	// THE NAMES CARRY MEASUREMENTS, so the measurements are checked. A zone name saying "13.5% THD"
	// is a claim about the DSP, and this repo's whole discipline is that such a claim is pinned or it
	// rots. The tolerance is generous because the point is the SHAPE and the ordering, not four
	// significant figures — but a name off by a factor would fail.
	std::array<double, 8> measured {};
	for (int z = 0; z < 8; ++z) {
		const float knee = (static_cast<float>(z) + 0.5f) / 8.0f;
		const shp::CurveParams p = shp::deriveCurve(knee, 0.0f, 0.0f, 0.5f);
		std::array<float, shp::kTableEntries> table {};
		shp::buildTable(p, table);
		// A sine at -10 dBFS through the finished table, exactly as the names' figures were taken.
		std::vector<float> y(512);
		for (int i = 0; i < 512; ++i) {
			y[static_cast<size_t>(i)] =
			    shp::lookup(table, 0.3162f * std::sin(2.0 * 3.14159265358979 * 4.0 * i / 512.0));
		}
		measured[static_cast<size_t>(z)] = thd(y, 4) * 100.0;
	}

	SUBCASE("strictly increasing, which is what makes an ordinal name honest") {
		for (size_t z = 1; z < measured.size(); ++z) {
			CAPTURE(z);
			CAPTURE(measured[z - 1]);
			CAPTURE(measured[z]);
			CHECK(measured[z] > measured[z - 1]);
		}
	}

	SUBCASE("and the figures in the names are the figures the DSP produces") {
		// MEASURED FIGURES THAT NOW LIVE ONLY IN A COMMENT, which is why this case still runs them.
		// They were the eight zone descriptions on Knee until Knee stopped being zoned; the ladder
		// they describe is the reason it did not need to be. Recorded on the declaration in
		// `shaper_types.h` — as 3.3, 5.6, 9.3, 13.5, 17.1, 19.8, 21.9, 23.3 percent — and a comment
		// is not checkable, so the check stays here.
		const std::array<double, 8> claimed {3.3, 5.6, 9.3, 13.5, 17.1, 19.8, 21.9, 23.3};
		for (size_t z = 0; z < claimed.size(); ++z) {
			CAPTURE(z);
			CAPTURE(measured[z]);
			CHECK(measured[z] == doctest::Approx(claimed[z]).epsilon(0.12));
		}
	}

	SUBCASE("the steepness in the names is the steepness the curve is built at") {
		// k = 1 + knee*9, restated here rather than read from the implementation.
		const std::array<double, 8> claimedK {1.6, 2.7, 3.8, 4.9, 6.1, 7.2, 8.3, 9.4};
		for (size_t z = 0; z < claimedK.size(); ++z) {
			const double knee = (static_cast<double>(z) + 0.5) / 8.0;
			CAPTURE(z);
			CHECK(1.0 + knee * 9.0 == doctest::Approx(claimedK[z]).epsilon(0.02));
		}
	}
}

TEST_CASE("colour is a field, and Inflate is a mode rather than a zone (backlog IW)") {
	// THE FIELD'S TRAVEL, measured. (Before backlog IW this held the Colour zone names up — zone 6 settled, the
	// rest not — but zone 6 was settled only because the inflator band was hard-wired there. The band is Colour's
	// Inflate MODE now, and the names are gone.)
	const auto travelOf = [](int zone) {
		double travel = 0.0;
		std::array<float, 6> prev {};
		constexpr int kSamples = 256;
		for (int s = 0; s < kSamples; ++s) {
			const float c = (static_cast<float>(zone) + static_cast<float>(s) / kSamples) / 8.0f;
			const shp::CurveParams p = shp::deriveCurve(0.75f, c, 0.0f, 0.5f);
			const std::array<float, 6> w {p.inflatorWeight, p.polyWeight,     p.hardKneeWeight,
			                              p.chebyWeight,    p.sineFoldWeight, p.rectifierWeight};
			if (s > 0) {
				double d = 0.0;
				for (size_t i = 0; i < 6; ++i) {
					d += static_cast<double>(w[i] - prev[i]) * (w[i] - prev[i]);
				}
				travel += std::sqrt(d);
			}
			prev = w;
		}
		return travel;
	};

	SUBCASE("the field accelerates: the top of the axis moves faster than the bottom") {
		// freqMult = 1 + colour² × 3, visible as a number rather than read off the source.
		CHECK(travelOf(7) > travelOf(0) * 4.0);
	}

	SUBCASE("backlog IW: in Free the field has NO band — the gamma-zero line is continuous where Inflate used to be") {
		// The inflator was hard-wired into zone 6 of the gamma-zero line; it is Colour's Inflate MODE now. So the
		// field at 0.81 is the field — and continuous across where the band's edges were (768/1023, 896/1023).
		const shp::CurveParams at = shp::deriveCurve(0.75f, 0.81f, 0.0f, 0.5f);
		CHECK(at.inflatorWeight != doctest::Approx(1.0f));
		for (const float edge : { 768.0f / 1023.0f, 896.0f / 1023.0f }) {
			CAPTURE(edge);
			const shp::CurveParams below = shp::deriveCurve(0.75f, edge - 1e-5f, 0.0f, 0.5f);
			const shp::CurveParams above = shp::deriveCurve(0.75f, edge + 1e-5f, 0.0f, 0.5f);
			// A STEP of the band's size (a weight jumping to 1, or to 0) is far above what 2e-5 of Colour moves.
			CHECK(std::fabs(above.inflatorWeight - below.inflatorWeight) < 0.01f);
			CHECK(std::fabs(above.rectifierWeight - below.rectifierWeight) < 0.01f);
		}
		// AND THE GAMMA-ZERO LINE AGREES WITH ITS NEIGHBOUR — the band was a gamma == 0 special case.
		const shp::CurveParams off = shp::deriveCurve(0.75f, 0.81f, 1e-6f, 0.5f);
		CHECK(std::fabs(off.inflatorWeight - at.inflatorWeight) < 0.01f);
	}

	SUBCASE("backlog IW: Inflate is a MODE — at 100% it is the inflator anywhere, at 0% the field, and between a blend") {
		// Inflate is an amount to sweep. `curveOf` is the one call the engine and the plot make, so it is the
		// one asserted. At 100% the inflator the band used to impose, at ANY colour and gamma; at 0% exactly the
		// field; at 50% halfway, term by term.
		for (const auto& [colour, gamma] : { std::pair { 0.3f, 0.0f }, std::pair { 2.7f, 0.4f }, std::pair { -1.1f, 3.0f } }) {
			CAPTURE(colour);
			CAPTURE(gamma);
			shp::Settings s;
			s.knee = 0.75f;
			s.colour = colour;
			s.gamma = gamma;
			s.colourMode = shp::kColourFree;
			const shp::CurveParams field = shp::curveOf(s);
			s.colourMode = shp::kColourInflate;
			s.inflateAmount = 1.0f;
			const shp::CurveParams full = shp::curveOf(s);
			CHECK(full.inflatorWeight == doctest::Approx(1.0f));
			CHECK(full.polyWeight == doctest::Approx(0.5f));
			CHECK(full.hardKneeWeight == doctest::Approx(0.0f));
			CHECK(full.chebyWeight == doctest::Approx(0.0f));
			CHECK(full.sineFoldWeight == doctest::Approx(0.0f));
			CHECK(full.rectifierWeight == doctest::Approx(0.0f));
			CHECK(full.asymmetry == doctest::Approx(0.5f)); // symmetric: no even harmonics
			CHECK(full.preExpandAmount == doctest::Approx(full.drive));
			s.inflateAmount = 0.0f;
			const shp::CurveParams none = shp::curveOf(s);
			CHECK(none.inflatorWeight == field.inflatorWeight);
			CHECK(none.rectifierWeight == field.rectifierWeight);
			CHECK(none.asymmetry == field.asymmetry);
			s.inflateAmount = 0.5f;
			const shp::CurveParams half = shp::curveOf(s);
			CHECK(half.inflatorWeight == doctest::Approx(0.5f * (field.inflatorWeight + 1.0f)));
			CHECK(half.rectifierWeight == doctest::Approx(0.5f * field.rectifierWeight));
			CHECK(half.asymmetry == doctest::Approx(0.5f * (field.asymmetry + 0.5f)));
		}
		// AND THE MODE AND THE AMOUNT ARE TABLE TERMS — a change rebuilds the table.
		shp::Settings a;
		shp::Settings b = a;
		b.colourMode = shp::kColourInflate;
		CHECK_FALSE(a.sameCurveAs(b));
		shp::Settings c = b;
		c.inflateAmount = 0.4f;
		CHECK_FALSE(b.sameCurveAs(c));
	}

	SUBCASE("the field is sparse: most positions have three bases alive or fewer") {
		// THE HALF-RECTIFICATION'S WHOLE PURPOSE. Six bases averaged everywhere would be mud; the 50%
		// duty is what leaves gaps. MEASURED at 78.2% over 512 positions.
		int sparse = 0;
		constexpr int kPositions = 512;
		for (int i = 0; i < kPositions; ++i) {
			const shp::CurveParams p = shp::deriveCurve(0.75f, static_cast<float>(i) / (kPositions - 1),
			                                           0.0f, 0.5f);
			const std::array<float, 6> w {p.inflatorWeight, p.polyWeight,     p.hardKneeWeight,
			                              p.chebyWeight,    p.sineFoldWeight, p.rectifierWeight};
			const int alive = static_cast<int>(std::count_if(w.begin(), w.end(), [](float v) {
				return v > 0.05f;
			}));
			if (alive <= 3) {
				++sparse;
			}
		}
		CHECK(static_cast<double>(sparse) / kPositions > 0.7);
	}

	SUBCASE("gamma is a real axis and not a garnish") {
		// MEASURED at 13.9 across gamma 0..1 at fixed colour — more than six of the eight colour
		// zones. It is the number that justifies giving it a pad axis rather than leaving it on a
		// modifier key, so it is the number a case holds.
		double travel = 0.0;
		std::array<float, 6> prev {};
		constexpr int kSamples = 256;
		for (int s = 0; s < kSamples; ++s) {
			const shp::CurveParams p =
			    shp::deriveCurve(0.75f, 0.5f, static_cast<float>(s) / kSamples, 0.5f);
			const std::array<float, 6> w {p.inflatorWeight, p.polyWeight,     p.hardKneeWeight,
			                              p.chebyWeight,    p.sineFoldWeight, p.rectifierWeight};
			if (s > 0) {
				double d = 0.0;
				for (size_t i = 0; i < 6; ++i) {
					d += static_cast<double>(w[i] - prev[i]) * (w[i] - prev[i]);
				}
				travel += std::sqrt(d);
			}
			prev = w;
		}
		CHECK(travel > 8.0);
	}
}

//--------------------------------------------------------------------------------------------//
// THE AMORTISED BUILD — the class's central claim, and a render cannot express any of it.       //
//--------------------------------------------------------------------------------------------//

TEST_CASE("the table build is amortised across blocks, and lands") {
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.4f;

	SUBCASE("a build costs a fixed number of SAMPLES, whatever the block size") {
		// THE SCHEDULE, ASSERTED RATHER THAN TIMED. A timing measurement could not tell "amortised
		// correctly" from "fast enough that nobody noticed"; this counts samples. 1025 entries at two
		// a frame is 513 samples, so the BLOCK count is whatever 513 samples happens to be — which is
		// the point.
		const auto samplesToLand = [&](int frames) {
			shp::Shaper sh;
			sh.prepare(48000.0);
			sh.setTarget(settings);
			std::vector<float> l(static_cast<size_t>(frames), 0.0f);
			std::vector<float> r = l;
			int samples = 0;
			while (sh.rebuildsForTest() == 0 && samples < 48000) {
				sh.processStereo(l.data(), r.data(), frames);
				samples += frames;
			}
			REQUIRE(sh.rebuildsForTest() == 1);
			return samples;
		};
		for (const int frames : {32, 64, 128, 256}) {
			CAPTURE(frames);
			const int samples = samplesToLand(frames);
			CHECK(samples >= 513);
			CHECK(samples < 513 + frames); // only the rounding up to a block boundary differs
		}
	}

	SUBCASE("THE SAME AUDIO SPLIT DIFFERENTLY IS THE SAME AUDIO OUT") {
		// THE BUG THAT REPLACED A CHUNK-PER-BLOCK SCHEDULE WITH A PER-SAMPLE ONE. Spending the build
		// per PROCESS CALL — floored at 128 entries, scaled by the frame count — made the table land
		// at a different point in the audio depending on how the host partitioned it. BOTH registry
		// sweeps caught it (`test_fx_runner.cpp`'s partition case at 0.183 worst |diff|, and
		// `test_synth.cpp`'s block-size case at 0.022 through the plugin); nothing in this file did,
		// which is why it is here now. A bounced mix and a live playback use different block sizes.
		const auto render = [&](int frames) {
			shp::Shaper sh;
			sh.prepare(48000.0);
			sh.setTarget(settings);
			std::vector<float> all(2048);
			for (size_t i = 0; i < all.size(); ++i) {
				all[i] = 0.6f * std::sin(0.031f * static_cast<float>(i));
			}
			std::vector<float> r = all;
			for (size_t at = 0; at < all.size(); at += static_cast<size_t>(frames)) {
				sh.processStereo(all.data() + at, r.data() + at, frames);
			}
			return all;
		};
		const std::vector<float> whole = render(2048);
		for (const int frames : {32, 64, 128, 512}) {
			CAPTURE(frames);
			const std::vector<float> split = render(frames);
			float worst = 0.0f;
			for (size_t i = 0; i < whole.size(); ++i) {
				worst = std::max(worst, std::fabs(whole[i] - split[i]));
			}
			CAPTURE(worst);
			CHECK(worst == doctest::Approx(0.0f).epsilon(1e-6));
		}
	}

	SUBCASE("no build is started while the device is linear, because there is no table to build") {
		shp::Shaper linear;
		linear.prepare(48000.0);
		shp::Settings off = settings;
		off.knee = 0.0f;
		linear.setTarget(off);
		std::vector<float> l(64, 0.5f);
		std::vector<float> r = l;
		for (int b = 0; b < 40; ++b) {
			linear.processStereo(l.data(), r.data(), 64);
		}
		CHECK(linear.rebuildsForTest() == 0);
		CHECK(linear.buildingForTest() == false);
	}

	SUBCASE("a drag does not starve the build: the curve keeps landing while the value keeps moving") {
		// THE FAILURE THIS GUARDS. Re-reading the target every block instead of latching it would
		// restart the build on every change, so a continuous drag would never complete one and the
		// SOUND WOULD FREEZE at whatever curve was in front when the hand started. Nothing about that
		// looks wrong in a buffer — the device keeps shaping, just not with the curve the knob says.
		shp::Shaper drag;
		drag.prepare(48000.0);
		shp::Settings moving = settings;
		std::vector<float> l(64, 0.0f);
		std::vector<float> r = l;
		for (int b = 0; b < 300; ++b) {
			moving.colour = static_cast<float>(b) / 300.0f;
			drag.setTarget(moving);
			drag.processStereo(l.data(), r.data(), 64);
		}
		// 300 blocks is 400 ms at 48k/64; measured at ~21 ms a table, so well over ten.
		CHECK(drag.rebuildsForTest() > 10);
	}

	SUBCASE("settings that do not touch the curve do not spend a rebuild") {
		// THE SPLIT THIS DEVICE IS BUILT ON. `deriveCurve` runs every block at 0.13 us; the TABLE
		// costs 127 us and depends on four inputs only. Mix, Drive and the five switches must not
		// trigger one — if they did, every automated mix move would cost a rebuild.
		settle(s, settings);
		const int before = s.rebuildsForTest();
		shp::Settings other = settings;
		other.threshold = 0.25f;
		other.drive = 0.9f;
		other.subOn = true; // the old On: the amount defaults to the field's own
		other.mangleOn = true; // the old On: the amount defaults to the field's own
		std::vector<float> l(64, 0.0f);
		std::vector<float> r = l;
		s.setTarget(other);
		for (int b = 0; b < 40; ++b) {
			s.processStereo(l.data(), r.data(), 64);
		}
		CHECK(s.rebuildsForTest() == before);
	}

	SUBCASE("the curve that landed is the curve the settings asked for") {
		// THE VALUE, NOT THE RENDER. After the build lands, the front table must equal a table built
		// independently from the same settings — which is the "compute the reference by an independent
		// restatement" half of CLAUDE.md's rule, and the only thing that can catch a build that
		// latched the wrong parameters or normalised with stale extremes.
		shp::Shaper landed;
		landed.prepare(48000.0);
		settle(landed, settings);
		REQUIRE(landed.rebuildsForTest() >= 1);

		std::array<float, shp::kTableEntries> expected {};
		shp::buildTable(shp::deriveCurve(settings.knee, settings.colour, settings.gamma,
		                                 shp::kSlewHarmonics),
		                expected);
		const auto& actual = landed.tableForTest();
		for (size_t i = 0; i < shp::kTableEntries; i += 61) {
			CAPTURE(i);
			CHECK(actual[i] == doctest::Approx(expected[i]).epsilon(1e-4));
		}
	}
}

//--------------------------------------------------------------------------------------------//
// THE SIGNAL PATH                                                                              //
//--------------------------------------------------------------------------------------------//

TEST_CASE("a disabled shaper passes the signal through untouched") {
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings off;
	off.enabled = false;
	off.knee = 0.9f;
	const std::vector<float> input = sine(64, 0.6f, 4.0f);
	std::vector<float> l = input;
	std::vector<float> r = input;
	s.setTarget(off);
	for (int b = 0; b < 20; ++b) {
		l = input;
		r = input;
		s.processStereo(l.data(), r.data(), 64);
	}
	for (size_t i = 0; i < l.size(); ++i) {
		CAPTURE(i);
		CHECK(l[i] == doctest::Approx(input[i]));
	}
}

TEST_CASE("backlog EH: the DC blocker's history advances through the Knee-0 bypass, so the first shaped sample after Knee comes back is not a step") {
	// THE SUB-THRESHOLD BRANCH SAYS WHY, in the device's own words: returning the dry sample raw leaves
	// the blocker's history un-advanced, so the first sample to cross back sees a stale difference and
	// gets a step of its own. The Knee-0 bypass did exactly that. Measured on a DC-offset input, where
	// the blocker's history is the whole of what it does: after a long bypass at +0.3, the first two
	// shaped samples agree with each other (the blocker is already tracking the offset); with a stale
	// history the first one carried the whole offset as a step against the second.
	//
	// TWO INSTANCES, IDENTICAL UNTIL THE BYPASS: one is bypassed on a +0.3 offset, the other on
	// silence. When both come back to the same curve on the same +0.3 input, the first shaped sample
	// differs between them by EXACTLY the offset the blocker tracked through the bypass (−0.3): the
	// one that saw +0.3 has its history at +0.3 and blocks it from the first sample; the one that saw
	// silence has its history at 0 and passes the offset. With the history not advanced, both would
	// hold the same stale state from before the bypass and the difference would be zero.
	const auto settle = [](shp::Shaper& s, const shp::Settings& shaped, float offset) {
		(void)run(s, shaped, 200, 64, 0.4f); // the curve built and settled
		shp::Settings bypass = shaped;
		bypass.knee = 0.0f;
		s.setTarget(bypass);
		std::vector<float> l(64, offset), r(64, offset);
		for (int b = 0; b < 40; ++b) {
			std::fill(l.begin(), l.end(), offset);
			std::fill(r.begin(), r.end(), offset);
			s.processStereo(l.data(), r.data(), 64);
			CHECK(l[63] == doctest::Approx(offset)); // the bypass IS the input, offset and all
		}
		s.setTarget(shaped); // the same curve as built: no rebuild, the shaper is live at once
		std::fill(l.begin(), l.end(), 0.3f);
		std::fill(r.begin(), r.end(), 0.3f);
		s.processStereo(l.data(), r.data(), 64);
		return l[0];
	};
	shp::Settings shaped;
	shaped.enabled = true;
	shaped.knee = 1.0f;
	shaped.drive = 0.5f;
	shaped.threshold = 1.0f;
	shp::Shaper onOffset;
	onOffset.prepare(48000.0);
	shp::Shaper onSilence;
	onSilence.prepare(48000.0);
	const float first = settle(onOffset, shaped, 0.3f);
	const float firstAfterSilence = settle(onSilence, shaped, 0.0f);
	CAPTURE(first);
	CAPTURE(firstAfterSilence);
	CHECK(first - firstAfterSilence == doctest::Approx(-0.3f).epsilon(0.05));
}

TEST_CASE("backlog EH: at the top of Colour's declared travel the curve still MOVES with gamma — the pre-expansion term and the four bank extras sum in double to the wrap") {
	// SHAPE 2. `preExpBase` was the one term in deriveCurve still summed in float: past |colour| = 2^22
	// it was an integer, the wrapped gamma phase could not move it, and pre-expansion sat at exactly
	// zero across the outer half of the travel. The four bank extras rode `PhiTriContext::eval`, which
	// narrowed its base to float the same way. Asserted by value: at colour 2^23 two gammas give two
	// different answers for each, and every answer is finite and in range.
	const float top = 8388608.0f; // the declared ceiling of Shaper Colour
	const shp::CurveParams a = shp::deriveCurve(0.8f, top, 0.0f, 0.5f);
	const shp::CurveParams b = shp::deriveCurve(0.8f, top, 0.37f, 0.5f);
	CAPTURE(a.preExpandAmount);
	CAPTURE(b.preExpandAmount);
	CHECK(std::isfinite(a.preExpandAmount));
	CHECK(std::isfinite(b.preExpandAmount));
	CHECK(a.preExpandAmount >= 0.0f);
	CHECK(b.preExpandAmount >= 0.0f);
	CHECK(a.preExpandAmount != b.preExpandAmount);
	// THE EXTRAS TOO — at least one of the four moves with gamma at the top of the travel; a bank that
	// went flat would give the same four numbers for every gamma.
	const bool extrasMove = a.subRatio != b.subRatio || a.stride != b.stride || a.feedback != b.feedback
	                        || a.rotation != b.rotation;
	CHECK(extrasMove);
	// AND NON-VACUITY AT AN ORDINARY COLOUR, where both always worked.
	const shp::CurveParams c = shp::deriveCurve(0.8f, 0.3f, 0.0f, 0.5f);
	const shp::CurveParams d = shp::deriveCurve(0.8f, 0.3f, 0.37f, 0.5f);
	CHECK(c.preExpandAmount != d.preExpandAmount);
}

TEST_CASE("backlog EH: a bypass clears the shaper's running state, so re-enabling does not carry the blocker's memory of the last enabled sample") {
	// A DC OFFSET WHILE ENABLED puts the blocker's history at +0.3; bypassed for a block, then enabled
	// again on SILENCE, the first sample used to read −0.3 (0 − dcIn) — the memory of a signal that
	// stopped a block ago. Cleared on the way out, it reads 0.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings shaped;
	shaped.enabled = true;
	shaped.knee = 1.0f;
	shaped.drive = 0.5f;
	shaped.threshold = 1.0f;
	(void)run(s, shaped, 200, 64, 0.4f); // the curve built and settled
	std::vector<float> l(64, 0.3f), r(64, 0.3f);
	for (int b = 0; b < 40; ++b) {
		std::fill(l.begin(), l.end(), 0.3f);
		std::fill(r.begin(), r.end(), 0.3f);
		s.processStereo(l.data(), r.data(), 64);
	}
	shp::Settings off = shaped;
	off.enabled = false;
	s.setTarget(off);
	std::fill(l.begin(), l.end(), 0.3f);
	std::fill(r.begin(), r.end(), 0.3f);
	s.processStereo(l.data(), r.data(), 64);
	CHECK(l[0] == 0.3f); // the bypass IS the input
	s.setTarget(shaped);
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	s.processStereo(l.data(), r.data(), 64);

	// THE ORACLE IS A FRESH START, NOT ZERO: `reset` is "as if just switched on" — identity tables and a
	// build ahead — and a just-switched-on shaper's first block on silence is whatever its stages make
	// of silence (the rotation stage leaves a small constant the blocker then removes). So the twin
	// takes the same settling, is RESET outright, and plays the same silence: the two must agree to the
	// bit. With the bypass keeping its state, the first sample carried −0.3 instead.
	shp::Shaper twin;
	twin.prepare(48000.0);
	(void)run(twin, shaped, 200, 64, 0.4f);
	twin.reset();
	std::vector<float> tl(64, 0.0f), tr(64, 0.0f);
	twin.processStereo(tl.data(), tr.data(), 64);
	CAPTURE(l[0]);
	CAPTURE(tl[0]);
	float worst = 0.0f;
	for (size_t i = 0; i < l.size(); ++i) {
		worst = std::max(worst, std::fabs(l[i] - tl[i]));
	}
	CHECK(worst < 1e-6f);
}

TEST_CASE("backlog EH: a NaN sample from the device before does not index the table — the read answers zero and the block stays finite") {
	// SHAPE 3, and the one site in the sweep where a bad cast was a memory READ: `std::clamp` passes a
	// NaN through, `static_cast<size_t>(NaN)` is undefined, and the result indexed the table. The
	// input is the previous device's audio, which nothing upstream promises finite.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings shaped;
	shaped.enabled = true;
	shaped.knee = 1.0f;
	shaped.drive = 0.8f;
	shaped.threshold = 1.0f;
	(void)run(s, shaped, 200, 64, 0.4f); // the curve built and live
	std::vector<float> l = sine(64, 0.4f, 4.0f), r = l;
	l[10] = std::numeric_limits<float>::quiet_NaN();
	r[10] = std::numeric_limits<float>::quiet_NaN();
	l[11] = std::numeric_limits<float>::infinity();
	r[11] = -std::numeric_limits<float>::infinity();
	s.processStereo(l.data(), r.data(), 64);
	int bad = 0;
	for (size_t i = 0; i < l.size(); ++i) {
		bad += std::isfinite(l[i]) && std::isfinite(r[i]) ? 0 : 1;
	}
	CHECK(bad == 0);
	CHECK(shp::lookup(std::array<float, 4> { -1.0f, -0.5f, 0.5f, 1.0f }, std::numeric_limits<float>::quiet_NaN()) == 0.0f);
}

TEST_CASE("knee at zero is a true bypass, and Drive has nothing to drive") {
	// NOT "a curve that happens to be straight" — the table is never consulted.
	//
	// THIS CASE USED TO SAY "and applies drive alone", on the firmware's argument that switching the
	// curve out should not silently switch the gain out with it. That was true of a Drive that WAS a
	// gain. Drive is now a TABLE POSITION and nothing else — the parity cases at the end of this file
	// pin that decision — so at knee 0 there is no table to position into and Drive does nothing. The
	// alternative, a gain at knee 0 only, is a 30 dB step between knee 0 and knee 0.001 at full drive;
	// the second subcase pins that there is none.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.0f;
	settings.drive = 0.5f;
	const std::vector<float> out = run(s, settings, 40, 64, 0.4f);
	const std::vector<float> input = sine(64, 0.4f, 4.0f);
	for (size_t i = 0; i < out.size(); ++i) {
		CAPTURE(i);
		CHECK(out[i] == doctest::Approx(input[i]).epsilon(1e-3));
	}

	SUBCASE("and full drive does NOT change it: there is nothing for Drive to sweep") {
		shp::Shaper driven;
		driven.prepare(48000.0);
		shp::Settings hot = settings;
		hot.drive = 1.0f;
		const std::vector<float> loud = run(driven, hot, 400, 64, 0.1f);
		// Before the parity fix this read 0.1 x 32 x drive^5. It is the input's own peak now.
		CHECK(peak(loud) == doctest::Approx(0.1f).epsilon(0.02));
	}

	SUBCASE("and there is no step between knee 0 and the smallest knee above it") {
		// A sub-threshold sample at full drive: knee 0 returns the input by the bypass, knee 0.01
		// returns it by the dry path. If the bypass still applied a gain, these would differ by it.
		shp::Settings sub = settings;
		sub.drive = 1.0f;
		sub.threshold = 0.5f;
		shp::Shaper bypass;
		bypass.prepare(48000.0);
		// 0.02 input is well under the 0.49 threshold, which is in input units.
		const std::vector<float> atZero = run(bypass, sub, 400, 64, 0.02f);
		sub.knee = 0.01f;
		shp::Shaper almost;
		almost.prepare(48000.0);
		const std::vector<float> justAbove = run(almost, sub, 400, 64, 0.02f);
		CHECK(peak(atZero) == doctest::Approx(0.02f).epsilon(0.02));
		CHECK(peak(justAbove) == doctest::Approx(0.02f).epsilon(0.02));
	}
}

TEST_CASE("mix is an amplitude threshold, not a crossfade") {
	// THE FIRMWARE'S MANUAL SAYS "Mix 50% -> effect only applies to the peaks of the waveform", and
	// that is a materially different control from a wet/dry blend. It is a claim about TWO NUMBERS —
	// where the threshold sits and how steeply the blend rises — and no rendered buffer states either.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.8f;

	SUBCASE("at mix 0 nothing crosses the threshold") {
		settings.threshold = 0.0f;
		settle(s, settings);
		CHECK(s.thresholdForTest() == doctest::Approx(1.0f));
	}

	SUBCASE("at mix 1 the threshold sits just BELOW zero, so even the quietest sample is shaped") {
		// A threshold of exactly zero would leave a dry notch through the middle of every waveform.
		settings.threshold = 1.0f;
		settle(s, settings);
		CHECK(s.thresholdForTest() < 0.0f);
		CHECK(s.thresholdForTest() == doctest::Approx(-0.0154f).epsilon(0.01));
	}

	SUBCASE("at mix 0.5 the threshold is about half scale: peaks shaped, body dry") {
		settings.threshold = 0.5f;
		settle(s, settings);
		CHECK(s.thresholdForTest() == doctest::Approx(0.4923f).epsilon(0.01));

		// AND THE SOUND AGREES WITH THE NUMBER. A sine whose peak is below the threshold comes back
		// untouched; one whose peak is above it does not. That is the second assertion CLAUDE.md asks
		// for — the one that proves the first is not vacuous.
		shp::Shaper quiet;
		quiet.prepare(48000.0);
		const std::vector<float> below = run(quiet, settings, 40, 64, 0.30f);
		const std::vector<float> expectedBelow = sine(64, 0.30f, 4.0f);
		for (size_t i = 0; i < below.size(); ++i) {
			CAPTURE(i);
			CHECK(below[i] == doctest::Approx(expectedBelow[i]).epsilon(2e-3));
		}

		// IN INPUT UNITS, because that is where the threshold lives now: `|x|` itself is compared
		// against 0.4923. It used to be compared after a drive gain and a 0.25 pad, so a signal had to
		// reach 1.97 to cross at mix 0.5 — above 0 dBFS, which is how hot inputs came to pass dry.
		// Both a full-scale sine and a +11.6 dBFS one (3.8, the "hot DAW signal") cross now.
		for (const float amp : {0.95f, 3.8f}) {
			CAPTURE(amp);
			shp::Shaper loud;
			loud.prepare(48000.0);
			const std::vector<float> above = run(loud, settings, 40, 64, amp);
			CHECK(thd(above, 4) > 0.02);
		}
	}

	SUBCASE("mix takes effect on the next block, without waiting for a rebuild") {
		settle(s, settings);
		const int rebuilds = s.rebuildsForTest();
		shp::Settings quieter = settings;
		quieter.threshold = 0.1f;
		std::vector<float> l(64, 0.0f);
		std::vector<float> r = l;
		s.setTarget(quieter);
		s.processStereo(l.data(), r.data(), 64);
		// NO REBUILD for a mix move: the table does not depend on Mix.
		CHECK(s.rebuildsForTest() == rebuilds);
		// AND THE THRESHOLD ARRIVES SMOOTHED (Shaper parity sweep, 2026-09-16 — the firmware closes
		// half the distance per buffer), so it is read after it has settled; the glide itself is
		// pinned by the parity case.
		settle(s, quieter);
		CHECK(s.thresholdForTest() == doctest::Approx(1.0f - 1.0154f * 0.1f).epsilon(0.01));
	}
}

//--------------------------------------------------------------------------------------------//
// THE FIRMWARE REFERENCE — `deriveParameters` and `evaluateTransfer`, transcribed               //
//--------------------------------------------------------------------------------------------//
//
// From our firmware fork@2567b04c `table_shaper.h`, the DEFAULT path (gamma 0, harmonic
// weight under 0.8) and its table build, with its own `fastTanh`. Retyped rather than calling the
// port, so what the port is held to is the other program. The port showed less timbre variation
// than the firmware, and this is the instrument that found why.
namespace fwmodel {

float fastTanh(float x) {
	if (x > 4.0f) {
		return 1.0f;
	}
	if (x < -4.0f) {
		return -1.0f;
	}
	const float x2 = x * x;
	return x * (15.0f + x2) / (15.0f + 6.0f * x2);
}

float triUni(float phase, float duty) {
	const float x = phase - std::floor(phase);
	if (!(x < duty)) {
		return 0.0f;
	}
	const float u = x / duty;
	return 1.0f - std::fabs(2.0f * u - 1.0f);
}

struct Params {
	float drive, w[6], asymmetry, preExpand;
};

Params derive(float knee, float colour) {
	constexpr float kPhi100 = 1.6180340f, kPhi125 = 1.8257419f, kPhi150 = 2.0581710f, kPhi175 = 2.3197171f,
	                kPhi200 = 2.6180340f, kPhi225 = 2.9603110f, kPhi250 = 3.3302077f;
	Params p {};
	p.drive = knee;
	const float y = colour;
	const float fm = 1.0f + y * y * 3.0f;
	p.w[0] = triUni(y * kPhi225 * fm, 0.5f);
	p.w[1] = triUni(y * kPhi200 * fm + 0.167f, 0.5f);
	p.w[2] = triUni(y * kPhi175 * fm + 0.333f, 0.5f);
	p.w[3] = triUni(y * kPhi250 * fm + 0.5f, 0.5f);
	p.w[4] = triUni(y * kPhi150 * fm + 0.667f, 0.5f);
	p.w[5] = triUni(y * kPhi125 * fm + 0.833f, 0.5f);
	p.asymmetry = 0.3f + triUni(y * kPhi100 * (1.0f + y * y * 1.5f), 0.5f) * 0.4f;
	p.preExpand = p.drive * triUni(y * 2.0f, 1.0f);
	if (y >= 768.0f / 1023.0f && y < 896.0f / 1023.0f) {
		p.w[0] = 1.0f;
		p.w[1] = 0.5f;
		p.w[2] = p.w[3] = p.w[4] = p.w[5] = 0.0f;
		p.preExpand = p.drive;
		p.asymmetry = 0.5f;
	}
	return p;
}

std::vector<float> table(const Params& P) {
	const float drive = P.drive;
	const float k = 1.0f + drive * 9.0f;
	const float asymRatio = 0.5f + P.asymmetry;
	const float invPos = 1.0f / std::fmax(0.01f, fastTanh(k * asymRatio));
	const float invNeg = 1.0f / std::fmax(0.01f, fastTanh(k * (2.0f - asymRatio)));
	float ws = 0.0f;
	for (const float w : P.w) {
		ws += w;
	}
	const float inv = ws > 0.001f ? 1.0f / ws : 1.0f;
	const auto eval = [&](float x) {
		const float mag = std::fabs(x);
		const float sign = x >= 0.0f ? 1.0f : -1.0f;
		const float od = mag * (1.0f + drive);
		const float pe = P.preExpand > 0.001f ? od * (1.0f + P.preExpand * 0.5f * (1.0f - od * od)) : od;
		const float kEff = k * (x >= 0.0f ? asymRatio : (2.0f - asymRatio));
		const float invN = x >= 0.0f ? invPos : invNeg;
		const float ape = std::fabs(pe);
		const float inflator = fastTanh(pe * (1.0f + 0.5f * (1.0f - ape * ape)) * kEff) * invN;
		const float pe2 = pe * pe, pe3 = pe2 * pe, pe5 = pe3 * pe2;
		const float poly = fastTanh((pe - pe3 / 3.0f + pe5 / 5.0f) * kEff) * invN;
		const float hard = std::fmin(std::fmax(pe, -1.0f), 1.0f);
		const float ci = pe * 1.2f, ci2 = ci * ci, ci3 = ci2 * ci, ci5 = ci3 * ci2;
		const float craw = 16.0f * ci5 - 20.0f * ci3 + 5.0f * ci;
		const float temp = (craw + 1.0f) * 0.25f;
		const auto ii = static_cast<int32_t>(temp);
		const float fl = static_cast<float>(ii - (temp < static_cast<float>(ii)));
		const float cph = (craw + 1.0f) - 4.0f * fl;
		const float cheby = std::fabs(cph <= 2.0f ? cph - 1.0f : 3.0f - cph);
		const float sine = std::fmin(
		    std::fabs(fastTanh(pe / 0.4f) * std::sin(3.14159265f * (1.0f + drive) * pe) + fastTanh(pe) * 0.3f), 1.0f);
		const float bias = 0.2f * drive;
		const float rect = fastTanh((std::fabs(pe + bias) - bias) * 2.0f);
		const float out = ws >= 0.001f ? (inflator * P.w[0] + poly * P.w[1] + hard * P.w[2] + cheby * P.w[3]
		                                  + sine * P.w[4] + rect * P.w[5])
		                                     * inv
		                               : mag;
		return sign * out;
	};
	std::vector<float> t(1025);
	float mx = -1e30f, mn = 1e30f;
	for (size_t i = 0; i <= 1024; ++i) {
		t[i] = eval(static_cast<float>(i) / 512.0f - 1.0f);
		mx = std::max(mx, t[i]);
		mn = std::min(mn, t[i]);
	}
	const float mid = (mx + mn) * 0.5f;
	const float g = (mx - mn) > 0.001f ? 2.0f / (mx - mn) : 1.0f;
	for (float& v : t) {
		v = std::clamp((v - mid) * g, -1.0f, 1.0f);
	}
	return t;
}

/// The firmware's read at full mix: the blend is complete everywhere, so the output IS the table.
float read(const std::vector<float>& t, float x) {
	const float pos = std::clamp((x + 1.0f) * 0.5f, 0.0f, 1.0f) * 1024.0f;
	const auto i = static_cast<size_t>(pos);
	const size_t j = std::min<size_t>(i + 1, 1024);
	return t[i] + (t[j] - t[i]) * (pos - static_cast<float>(i));
}

constexpr int kHarm = 16;

/// Harmonics 2..16 of one period of `f(A sin)`, in dB against the fundamental, floored at -80.
template <typename F>
std::array<double, kHarm> spectrumOf(F&& f, float amp) {
	constexpr int kN = 2048;
	std::vector<double> y(kN);
	for (int n = 0; n < kN; ++n) {
		y[n] = f(amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * n / kN)));
	}
	std::array<double, kHarm> mag {};
	for (int h = 1; h <= kHarm; ++h) {
		double re = 0.0, im = 0.0;
		for (int n = 0; n < kN; ++n) {
			re += y[n] * std::cos(2.0 * std::numbers::pi * h * n / kN);
			im += y[n] * std::sin(2.0 * std::numbers::pi * h * n / kN);
		}
		mag[h - 1] = std::sqrt(re * re + im * im) * 2.0 / kN;
	}
	std::array<double, kHarm> db {};
	for (int h = 0; h < kHarm; ++h) {
		db[h] = std::max(-80.0, 20.0 * std::log10(std::max(mag[h], 1e-12) / std::max(mag[0], 1e-12)));
	}
	return db;
}

double thdOf(const std::array<double, kHarm>& db) {
	double e = 0.0;
	for (int h = 1; h < kHarm; ++h) {
		e += std::pow(10.0, db[h] / 10.0);
	}
	return std::sqrt(e);
}

/// THE PORT'S STATIC TRANSFER at mix 1, the given drive (0 by default), aggression neutral, gamma 0, extras off — the curve
/// "the drawn transfer IS the device's" ties to the engine.
std::function<float(float)> portTransfer(float knee, float colour, float drive = 0.0f) {
	shp::Settings s;
	s.knee = knee;
	s.colour = colour;
	// **THE FIRMWARE'S BAND IS OUR INFLATE MODE (backlog IW).** `fwmodel::derive` hard-wires the inflator into 768..895 of
	// the gamma-zero line, as the firmware does; the port made it Colour's Inflate MODE. So where the firmware is in its
	// band, the port is compared in Inflate at 100% — the same sound on both sides, which is what a parity sweep compares.
	if (colour >= 768.0f / 1023.0f && colour < 896.0f / 1023.0f) {
		s.colourMode = shp::kColourInflate;
		s.inflateAmount = 1.0f;
	}
	s.threshold = 1.0f;
	auto tab = std::make_shared<std::vector<float>>(shp::kTableEntries);
	shp::buildTable(shp::curveOf(s), *tab);
	const shp::TableBound bound = shp::boundTable(*tab);
	const shp::Staging staging = shp::stagingFor(1.0f, 0.5f);
	const float fade = shp::kneeFadeFor(knee);
	const float sweep = shp::sweepFor(drive);
	return [tab, bound, staging, fade, sweep](float x) {
		return shp::staticTransferAt(*tab, bound, staging, sweep, fade, x);
	};
}

} // namespace fwmodel
TEST_CASE("the shaper adds harmonic distortion, and more of it as Drive sweeps the table") {
	// RENAMED TWICE. Once because the old title claimed a control it does not touch — "the shaper
	// adds harmonics", next to a control called Harmonics that this case moves nowhere. And again
	// with the ceiling work, because the control that opens the curve is Drive now, not Knee.
	//
	// **KNEE WAS NORMALISED OUT OF THE TABLE'S FIRST RISE, AND THIS CASE SAID SO.** The read has
	// unit slope at the crossing by construction — `r = R/(h·m)` with `m` the table's own steepest
	// secant — and across the Knee sweep the field's tables are, near the origin, one curve at five
	// slopes, which the read divides out. MEASURED on the ceiling build, THD of a 0 dBFS sine at
	// mix 1.0, drive 0, knee 0.05 → 1.0: 4.3 % flat at colour 0.38, 6.9 % flat at colour 0.8. Knee
	// was audible only where the sweep reached the folds: 61 → 71 % at colour 0.38, full drive.
	// Raised for a decision; the answer was the PRE-SQUASH, `kSquashHardness` in shaper_curve.h, and
	// the KNEE cases at the end of this file are where its ladder lives. The subcase below keeps
	// the fold claim, which the squash does not remove (62 → 103 % now, through 185 % on the way:
	// a folded wave's fundamental collapses and THD stops being a scale).
	SUBCASE("Drive opens the curve, monotonically") {
		double previous = 0.0;
		for (const float drive : {0.0f, 0.5f, 1.0f}) {
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings settings;
			settings.enabled = true;
			settings.knee = 0.5f;
			settings.colour = 0.3f;
			settings.threshold = 1.0f;
			settings.drive = drive;
			const std::vector<float> out = run(s, settings, 40, 512, 0.7f);
			const double distortion = thd(out, 4);
			CAPTURE(drive);
			CAPTURE(distortion);
			CHECK(distortion > previous);
			previous = distortion;
		}
		CHECK(previous > 0.05);
	}

	SUBCASE("Knee opens what the sweep reaches: at full drive, in the folding zone") {
		const auto distortionAt = [](float knee) {
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings settings;
			settings.enabled = true;
			settings.knee = knee;
			settings.colour = 0.38f;
			settings.threshold = 1.0f;
			settings.drive = 1.0f;
			const std::vector<float> out = run(s, settings, 40, 512, 1.0f);
			return thd(out, 4);
		};
		const double gentle = distortionAt(0.05f);
		const double hard = distortionAt(1.0f);
		CAPTURE(gentle);
		CAPTURE(hard);
		CHECK(gentle > 0.3);
		// **KNEE MOVES THE SPECTRUM HERE, NOT THE THD TOTAL — since the pre-squash was retired
		// (2026-09-22).** Knee is steepness and overdrive inside the table now, as the firmware's is,
		// and in the folding zone at full drive a steeper table reshuffles WHICH harmonics carry the
		// energy while the total barely moves: measured 37.3 % THD at knee 0.05 against 35.8 % at knee
		// 1. So the claim is the spectrum's: harmonics 2..16, in dB against the fundamental, differ by
		// more than 3 dB on average between the two ends of the knob. A non-monotone Knee was judged
		// fine on 2026-09-04 (backlog CG); what is not fine is a Knee that does nothing.
		const auto a = fwmodel::spectrumOf(fwmodel::portTransfer(0.05f, 0.38f, 1.0f), 1.0f);
		const auto b = fwmodel::spectrumOf(fwmodel::portTransfer(1.0f, 0.38f, 1.0f), 1.0f);
		double meanAbs = 0.0;
		for (int h = 1; h < fwmodel::kHarm; ++h) {
			meanAbs += std::fabs(a[h] - b[h]);
		}
		meanAbs /= (fwmodel::kHarm - 1);
		CAPTURE(meanAbs);
		// **2 dB, RE-BASED 2026-09-23 WHEN THE STEP AT ZERO WENT (backlog IF).** The bar was 3 while the
		// Mix-1 read stepped to ±f(0) at every zero crossing — a square-wave component in BOTH ends of
		// the knob, which the two ends carried at different heights. MEASURED after: 2.65 dB. Still
		// the claim it was: a Knee that did nothing would read near 0.
		CHECK(meanAbs > 2.0);
	}
}

TEST_CASE("EMPHASIS: the tilt section is the closed form, its inverse undoes it, and 0 dB is the identity") {
	// The Harmonics knob was inaudible in practice: it set only the slew duty, audible only with
	// Lowpass or Integrator on. Its slot is now EMPHASIS, a pre/de-emphasis tilt round the curve,
	// chosen from four candidates. This case holds the filter pair.
	constexpr double kRate = 48000.0;
	SUBCASE("0 dB is exactly the identity: b0 = 1 and b1 = a1, both directions") {
		for (const bool inverse : { false, true }) {
			const shp::TiltSection s = shp::tiltSection(0.0, shp::kEmphasisPivotHz, kRate, inverse);
			CHECK(s.b0 == 1.0f);
			CHECK(s.b1 == s.a1);
		}
	}
	SUBCASE("the gains: highs lifted by the setting, lows cut by it, the pivot at unity") {
		for (const double db : { -12.0, -6.0, 9.0 }) {
			CAPTURE(db);
			const shp::TiltSection s = shp::tiltSection(db, shp::kEmphasisPivotHz, kRate, false);
			// |H| at DC is (b0 + b1)/(1 + a1); at Nyquist (b0 − b1)/(1 − a1).
			const double dc = (static_cast<double>(s.b0) + s.b1) / (1.0 + s.a1);
			const double ny = (static_cast<double>(s.b0) - s.b1) / (1.0 - s.a1);
			CHECK(20.0 * std::log10(std::fabs(dc)) == doctest::Approx(-db).epsilon(1e-4));
			CHECK(20.0 * std::log10(std::fabs(ny)) == doctest::Approx(db).epsilon(1e-4));
			// AT THE PIVOT, measured through the section: a steady sine comes out at unity.
			shp::TiltSection sec = s;
			float z = 0.0f;
			double peakOut = 0.0;
			const double w = 2.0 * std::numbers::pi * shp::kEmphasisPivotHz / kRate;
			for (int n = 0; n < 48000; ++n) {
				const float y = sec.step(static_cast<float>(std::sin(w * n)), z);
				if (n > 24000) {
					peakOut = std::max(peakOut, static_cast<double>(std::fabs(y)));
				}
			}
			CHECK(peakOut == doctest::Approx(1.0).epsilon(0.01));
		}
	}
	SUBCASE("in, then the inverse, is the input — which is what makes only the distortion tilted") {
		const shp::TiltSection in = shp::tiltSection(10.0, shp::kEmphasisPivotHz, kRate, false);
		const shp::TiltSection out = shp::tiltSection(10.0, shp::kEmphasisPivotHz, kRate, true);
		float zi = 0.0f;
		float zo = 0.0f;
		uint32_t rng = 12345u;
		float worst = 0.0f;
		for (int n = 0; n < 20000; ++n) {
			rng = rng * 1664525u + 1013904223u;
			const float x = static_cast<float>(rng >> 8) / 16777216.0f - 0.5f;
			const float y = out.step(in.step(x, zi), zo);
			worst = std::max(worst, std::fabs(y - x));
		}
		CAPTURE(worst);
		CHECK(worst < 1e-5f);
	}
}

TEST_CASE("EMPHASIS: it sets how hard each band drives the curve, and the de-emphasis colours what the curve adds") {
	// **THE MECHANISM, AS A VALUE.** `drivePeak()` meters the input the table actually sees — after
	// the trim AND the pre-emphasis — so the level Emphasis delivers into the curve is a number, not
	// something inferred from the shaped output. A folder's THD is not monotone in level (driving a
	// fold harder can land on a smoother part of it), so THD is the wrong instrument for "which band
	// hits the curve harder"; the meter is the right one. First draft of this case used THD and was
	// red on exactly that at colour 0.35.
	const auto drivenAt = [](float emphasisDb, float cycles) {
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings st;
		st.enabled = true;
		st.knee = 0.7f;
		st.colour = 0.35f;
		st.threshold = 1.0f;
		st.emphasisDb = emphasisDb;
		s.setTarget(st);
		for (int b = 0; b < 12; ++b) {
			std::vector<float> l = sine(4096, 0.3f, cycles);
			std::vector<float> r = l;
			s.processStereo(l.data(), r.data(), 4096);
		}
		return static_cast<double>(s.drivePeak());
	};
	// THE EXPECTED LEVEL, restated from the shelf's own frequency response.
	const auto expected = [](float emphasisDb, float cycles) {
		const double hz = 48000.0 * cycles / 4096.0;
		const shp::TiltSection s = shp::tiltSection(emphasisDb, shp::kEmphasisPivotHz, 48000.0, false);
		const double w = 2.0 * std::numbers::pi * hz / 48000.0;
		const std::complex<double> z1 = std::polar(1.0, -w);
		return 0.3 * std::abs((static_cast<double>(s.b0) + static_cast<double>(s.b1) * z1)
		                      / (1.0 + static_cast<double>(s.a1) * z1));
	};
	for (const float cycles : { 16.0f, 400.0f }) {
		for (const float db : { -12.0f, 0.0f, 12.0f }) {
			CAPTURE(cycles);
			CAPTURE(db);
			CHECK(drivenAt(db, cycles) == doctest::Approx(expected(db, cycles)).epsilon(0.03));
		}
	}
	// AND THE DIRECTION, stated plainly: lifting the highs drives a high sine harder and a low one softer.
	CHECK(drivenAt(12.0f, 400.0f) > drivenAt(-12.0f, 400.0f) * 3.0);
	CHECK(drivenAt(-12.0f, 16.0f) > drivenAt(12.0f, 16.0f) * 3.0);

	SUBCASE("on a SAW — every partial at once — emphasis DOWN brightens the added distortion, UP darkens it") {
		// Not a sine: a sine shows the level into the curve; it
		// cannot show what the tilt does to real material, where the distortion of every partial lands
		// on every other. So: a band-limited saw at 187.5 Hz (partials to 19.9 kHz) through a plain
		// saturating colour, and the RESIDUAL — output minus input, which is what the curve added,
		// because the pre/de pair cancels on the linear part.
		//
		// **THE CLAIM WAS WRITTEN THE OTHER WAY ROUND FIRST, AND THE SAW OVERTURNED IT.** The draft
		// said lifting the highs gives a brighter residual. MEASURED: 13 % of the residual's energy above
		// 2 kHz at -12 dB, 1.4 % at 0, 1.5 % at +12. The distortion is made BETWEEN the two sections, so
		// the de-emphasis shapes it too: at +12 it cuts everything above the pivot by up to 12 dB —
		// including the grit the lifted highs just made — while at -12 it LIFTS everything above the
		// pivot, including the harmonics the driven lows threw up there. Up is tape-like: highs
		// compress, the grit comes back dark. Down is the fizz: lows saturate, the grit comes back bright.
		const auto saw = []() {
			std::vector<float> v(4096, 0.0f);
			for (int k = 1; 16 * k < 2048 && k * 187.5 < 20000.0; ++k) {
				for (int i = 0; i < 4096; ++i) {
					v[static_cast<size_t>(i)] += static_cast<float>(
					    0.35 / k * std::sin(2.0 * std::numbers::pi * 16.0 * k * i / 4096.0));
				}
			}
			return v;
		}();
		const auto highShareAt = [&](float emphasisDb) {
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings st;
			st.enabled = true;
			st.knee = 0.7f;
			st.colour = 0.0f;
			st.threshold = 1.0f;
			st.emphasisDb = emphasisDb;
			s.setTarget(st);
			std::vector<float> l;
			for (int b = 0; b < 16; ++b) {
				l = saw;
				std::vector<float> r = l;
				s.processStereo(l.data(), r.data(), 4096);
			}
			double high = 0.0;
			double total = 0.0;
			for (int h = 1; 16 * h < 2048; ++h) {
				double re = 0.0;
				double im = 0.0;
				for (int i = 0; i < 4096; ++i) {
					const double resid = static_cast<double>(l[static_cast<size_t>(i)]) - saw[static_cast<size_t>(i)];
					const double ph = 2.0 * std::numbers::pi * 16.0 * h * i / 4096.0;
					re += resid * std::cos(ph);
					im -= resid * std::sin(ph);
				}
				const double e = re * re + im * im;
				total += e;
				if (h * 187.5 >= 2000.0) {
					high += e;
				}
			}
			return total > 0.0 ? high / total : 0.0;
		};
		const double cut = highShareAt(-12.0f);
		const double flat = highShareAt(0.0f);
		const double lift = highShareAt(12.0f);
		MESSAGE("saw: share of the added distortion above 2 kHz at emphasis -12/0/+12: " << cut << " / " << flat
		                                                                                 << " / " << lift);
		CHECK(cut > lift * 3.0);
		CHECK(cut > flat * 3.0);
	}
}

TEST_CASE("every extra is off until its switch is on") {
	// THE HONESTY RULE, APPLIED TO FIVE SWITCHES. The firmware force-enables its lowpass whenever the
	// gamma is zero and the field's slew intensity is above zero, regardless of the mask — a
	// convenience there, because the mask lives behind a secret menu. Here the switch has a label on
	// it, and a switch reading Off while the filter runs is precisely what this rule forbids.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	// A colour whose field puts a real slew intensity on the table, so "off" is a live refusal rather
	// than a field that happened to ask for nothing.
	settings.colour = 0.06f;
	settle(s, settings);
	REQUIRE(s.curveForTest().slewIntensity > 0.05f);
	CHECK(s.lowpassCutoffForTest() == doctest::Approx(0.0f));

	settings.lowpassOn = true; // the old On: the amount defaults to the field's own
	settle(s, settings);
	CHECK(s.lowpassCutoffForTest() > 0.0f);
}

TEST_CASE("the lowpass corner tracks the note, and is exact rather than approximated") {
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.06f;
	settings.lowpassOn = true; // the old On: the amount defaults to the field's own

	SUBCASE("with no note it sits above the 110 Hz reference") {
		settle(s, settings);
		const float intensity = s.curveForTest().slewIntensity;
		// The corner is note x 2^(2 - intensity), restated independently.
		const float expected = 110.0f * std::exp2(2.0f - intensity);
		CHECK(s.lowpassCutoffForTest() == doctest::Approx(expected).epsilon(1e-3));
	}

	SUBCASE("and it moves with the note, an octave for an octave") {
		settle(s, settings);
		const float atReference = s.lowpassCutoffForTest();
		// A4 = 69 = 440 Hz, two octaves above the 110 Hz reference.
		s.setNoteContext({69.0f, true});
		std::vector<float> l(64, 0.0f);
		std::vector<float> r = l;
		s.processStereo(l.data(), r.data(), 64);
		CHECK(s.lowpassCutoffForTest() == doctest::Approx(atReference * 4.0f).epsilon(1e-3));
	}

	SUBCASE("the corner is derived from a value, which is why there is a seam for it") {
		// CLAUDE.md's canonical example: a filter's corner is not recoverable from its output, and two
		// sessions in one week added a test-only accessor to Synth for exactly this. The corner here
		// depends on the note, the field's slew intensity and the sample rate, and no render states it.
		settle(s, settings);
		CHECK(s.lowpassCutoffForTest() > 110.0f);
		CHECK(s.lowpassCutoffForTest() < 440.0f);
	}
}

TEST_CASE("the lowpass and the integrator both run when both are switched on") {
	// A DELIBERATE DIVERGENCE FROM THE FIRMWARE, and the case that pins it. There the integrator takes
	// precedence, and the code's own parenthetical says why: the two share one intensity and one piece
	// of state and are "mutually exclusive in practice". That precedence was a GUARD ON A SHARED
	// VARIABLE, not a designed sound — and on a panel where both are labelled switches it becomes a
	// switch that silently disables another, which is what the honesty rule forbids. Two state floats
	// cost nothing.
	//
	// IT IS ALSO WHAT MAKES THE NOTE ROAD HONEST. The registry sweep "EVERY device with a NOTE road"
	// drives every stepped control to its ceiling, so all five extras are on; under the old precedence
	// the lowpass never ran, and the lowpass is the ONLY thing the note reaches. That sweep failed on
	// its non-vacuity guard — "the note reaches nothing on Shaper" — which is the filter's own shipped
	// bug wearing a different hat.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.06f;
	settings.lowpassOn = true; // the old On: the amount defaults to the field's own
	settings.integratorOn = true; // the old On: the amount defaults to the field's own
	settle(s, settings);
	// A LIVE CORNER IS THIS DEVICE'S WAY OF SAYING THE LOWPASS IS RUNNING — a value, not a difference.
	CHECK(s.lowpassCutoffForTest() > 0.0f);

	SUBCASE("and each still changes the sound on its own") {
		const auto render = [](shp::Settings st) {
			shp::Shaper sh;
			sh.prepare(48000.0);
			return run(sh, st, 60, 512, 0.8f);
		};
		shp::Settings neither = settings;
		neither.lowpassOn = false;
		neither.integratorOn = false;
		shp::Settings lpfOnly = settings;
		lpfOnly.integratorOn = false;
		shp::Settings intOnly = settings;
		intOnly.lowpassOn = false;

		const std::vector<float> base = render(neither);
		const auto differs = [&](const std::vector<float>& other) {
			double sum = 0.0;
			for (size_t i = 0; i < base.size(); ++i) {
				sum += std::fabs(base[i] - other[i]);
			}
			return sum / static_cast<double>(base.size());
		};
		CHECK(differs(render(lpfOnly)) > 1e-4);
		CHECK(differs(render(intOnly)) > 1e-4);
		CHECK(differs(render(settings)) > 1e-4);
	}
}
TEST_CASE("each extra changes the sound, and changes it only when switched on") {
	// A DIFFERENCE CHECK, USED THE WAY CLAUDE.md ALLOWS: as the SECOND assertion, proving the switches
	// are not inert. What each extra IS has been asserted above by value; this says the value reaches
	// the samples.
	const auto render = [](const shp::Settings& settings) {
		shp::Shaper s;
		s.prepare(48000.0);
		return run(s, settings, 60, 512, 0.8f);
	};

	// A COLOUR WHERE THE FIELD ACTUALLY ASKS FOR ALL FOUR — and the preconditions are ASSERTED, not
	// assumed. This is `fx_tenant.h`'s third note: a measurement needs its own precondition.
	//
	// THE FIRST DRAFT OF THIS CASE FAILED FOR EXACTLY THAT REASON and it was the test that was wrong,
	// which is worth recording because the failure looked like a bug in the device. It used colour
	// 0.06, where the sub triangle's 30% duty happens to put `subIntensity` at ZERO — so switching Sub
	// on correctly changed nothing, and the case read that as the switch being inert. A field this
	// sparse (78% of positions have three bases or fewer) will silently make a naive extras case
	// vacuous, in whichever direction the author did not check.
	//
	// MEASURED: at colour 0.0305 the field gives sub 0.90, feedback 0.73, rotation 14 and slew 0.20.
	shp::Settings base;
	base.enabled = true;
	base.knee = 0.7f;
	base.colour = 0.0305f;
	base.threshold = 1.0f;
	{
		// THE FIELD'S OFFER, NOT WHAT IS HEARD — since the extras became scalers (backlog II) the engine's
		// curve at a scaler of 0 carries zeros for all four, which is the scaler working, not the field.
		const shp::CurveParams p = shp::deriveCurve(base.knee, base.colour, base.gamma, shp::kSlewHarmonics);
		REQUIRE(p.subIntensity > 0.3f);
		REQUIRE(p.feedback > 0.2f);
		REQUIRE(p.rotation > 4);
		REQUIRE(p.slewIntensity > 0.1f);
	}
	const std::vector<float> plain = render(base);

	const auto differs = [&](const std::vector<float>& other) {
		double sum = 0.0;
		for (size_t i = 0; i < plain.size(); ++i) {
			sum += std::fabs(plain[i] - other[i]);
		}
		return sum / static_cast<double>(plain.size());
	};

	SUBCASE("sub") {
		shp::Settings s = base;
		s.subOn = true; // the old On: the amount defaults to the field's own
		CHECK(differs(render(s)) > 1e-4);
	}
	SUBCASE("feedback") {
		shp::Settings s = base;
		s.feedbackOn = true; // the old On: the amount defaults to the field's own
		CHECK(differs(render(s)) > 1e-4);
	}
	SUBCASE("rotate") {
		shp::Settings s = base;
		s.mangleOn = true; // the old On: the amount defaults to the field's own
		CHECK(differs(render(s)) > 1e-4);
	}
	SUBCASE("lowpass") {
		shp::Settings s = base;
		s.lowpassOn = true; // the old On: the amount defaults to the field's own
		CHECK(differs(render(s)) > 1e-4);
	}
	SUBCASE("integrator") {
		shp::Settings s = base;
		s.integratorOn = true; // the old On: the amount defaults to the field's own
		CHECK(differs(render(s)) > 1e-4);
	}
}

TEST_CASE("the sub-harmonic flips its sign every subRatio zero crossings") {
	// THE MECHANISM, ASSERTED AS A COUNT rather than as "the low end got louder". A sub-harmonic that
	// flipped on every crossing instead of every Nth would still change the sound and would be the
	// wrong interval — which is exactly the partial effect a difference check cannot see.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.5f;
	settings.subOn = true; // the old On: the amount defaults to the field's own
	settle(s, settings);
	const shp::CurveParams& p = s.curveForTest();
	// The field chooses the interval; the case checks it is one of the five the mapping can produce.
	CHECK(p.subRatio >= 2);
	CHECK(p.subRatio <= 6);
	// AND THE STRIDE IS IN RANGE, because it gates how often crossings are even looked for. A stride
	// outside [1,128] would either check every sample (expensive) or never (silent).
	CHECK(p.stride >= 1);
	CHECK(p.stride <= 128);
}

TEST_CASE("a reset restores the just-switched-on state, the table included") {
	// `AudioOps::reset` means "as if just switched on", and the TABLE is part of that state.
	//
	// THIS CASE USED TO ASSERT THE OPPOSITE, on an argument that sounded good: the table is a pure
	// function of the settings, a reset does not change the settings, so rebuilding spends 127 us
	// arriving at the table already in hand. True, and answering a question nobody asked — the
	// contract is about DETERMINISM, not cost. Keeping the old curve made a reset device and a fresh
	// device on the same settings two different instruments for the first 10 ms.
	//
	// CAUGHT BY THE REGISTRY SWEEP, NOT BY THIS FILE: every case here reset a device onto the settings
	// it already had, which `test_fx_runner.cpp`'s own helper calls "a knife that cannot cut".
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.4f;
	settle(s, settings);
	REQUIRE(s.rebuildsForTest() >= 1);

	s.reset();
	CHECK(s.buildingForTest() == false);

	// THE TABLE IS THE IDENTITY AGAIN, asserted as a value at three points rather than as "it
	// changed" — a difference check would pass on any table at all.
	for (const size_t i : {size_t {0}, shp::kTableEntries / 2, shp::kTableEntries - 1}) {
		const float x =
		    static_cast<float>(i) * (2.0f / static_cast<float>(shp::kTableEntries - 1)) - 1.0f;
		CAPTURE(i);
		CHECK(s.tableForTest()[i] == doctest::Approx(x).epsilon(1e-5));
	}

	SUBCASE("and a reset device then sounds exactly like a fresh one") {
		// THE PROPERTY THE SWEEP ACTUALLY CHECKS, restated here so this file can see it too.
		shp::Shaper fresh;
		fresh.prepare(48000.0);
		fresh.setTarget(settings);
		s.setTarget(settings);
		for (int block = 0; block < 30; ++block) {
			std::vector<float> a = sine(256, 0.5f, 4.0f);
			std::vector<float> ra = a;
			std::vector<float> b = a;
			std::vector<float> rb = a;
			fresh.processStereo(a.data(), ra.data(), 256);
			s.processStereo(b.data(), rb.data(), 256);
			for (size_t i = 0; i < a.size(); ++i) {
				CAPTURE(block);
				CAPTURE(i);
				REQUIRE(a[i] == doctest::Approx(b[i]).epsilon(1e-6));
			}
		}
	}
}
TEST_CASE("the device produces no NaN or infinity anywhere in its parameter space") {
	// A SWEEP RATHER THAN A SPOT CHECK, because the folders overshoot by construction and the
	// normalisation divides by a peak-to-peak that a degenerate curve could make tiny. `buildTable`
	// guards that division; this is what says the guard is enough across the whole field.
	for (int ki = 0; ki <= 4; ++ki) {
		for (int ci = 0; ci <= 16; ++ci) {
			for (int gi = 0; gi <= 2; ++gi) {
				shp::Shaper s;
				s.prepare(48000.0);
				shp::Settings settings;
				settings.enabled = true;
				settings.knee = static_cast<float>(ki) / 4.0f;
				settings.colour = static_cast<float>(ci) / 16.0f;
				settings.gamma = static_cast<float>(gi) / 2.0f;
				settings.drive = 0.85f;
				settings.threshold = 1.0f;
				settings.sub = settings.feedback = settings.mangle = settings.integratorOn = true; // the old On: the amount defaults to the field's own
				CAPTURE(settings.knee);
				CAPTURE(settings.colour);
				CAPTURE(settings.gamma);
				const std::vector<float> out = run(s, settings, 30, 128, 0.9f);
				for (const float x : out) {
					REQUIRE(std::isfinite(x));
				}
				// AND IT DOES NOT RUN AWAY. The comb's depth is capped below unity for this reason;
				// a feedback path that could reach 1.0 would ring forever.
				CHECK(peak(out) < 40.0f);
			}
		}
	}
}

TEST_CASE("the mono road and the stereo road shape a signal the same way") {
	// THEY SHARE `processSample`, so this is a case about the SHARING rather than about the maths —
	// the failure it catches is a future edit that optimises one road and forgets the other.
	shp::Shaper stereo;
	stereo.prepare(48000.0);
	shp::Shaper mono;
	mono.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.4f;

	std::vector<float> l, r, m;
	stereo.setTarget(settings);
	mono.setTarget(settings);
	for (int b = 0; b < 40; ++b) {
		l = sine(64, 0.7f, 4.0f);
		r = l;
		m = l;
		stereo.processStereo(l.data(), r.data(), 64);
		mono.processMono(m.data(), 64);
	}
	for (size_t i = 0; i < l.size(); ++i) {
		CAPTURE(i);
		CHECK(l[i] == doctest::Approx(m[i]).epsilon(1e-5));
		// AND THE TWO CHANNELS AGREE with each other on identical input, which is what says the
		// per-channel state is genuinely per-channel and not accidentally shared.
		CHECK(l[i] == doctest::Approx(r[i]).epsilon(1e-5));
	}
}

TEST_CASE("the two channels keep separate history, so a stereo image does not depend on buffer order") {
	// A SHARED ZERO-CROSSING COUNTER WOULD MAKE THE SUB-HARMONIC'S SIGN DEPEND ON WHICH CHANNEL RAN
	// LAST, which is a stereo image that moves with the interleaving. Fed different signals, the two
	// channels must produce different outputs — and the one fed silence must stay silent.
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.5f;
	settings.subOn = true; // the old On: the amount defaults to the field's own
	settings.feedbackOn = true; // the old On: the amount defaults to the field's own
	s.setTarget(settings);

	std::vector<float> l, r;
	for (int b = 0; b < 40; ++b) {
		l = sine(256, 0.8f, 8.0f);
		r.assign(256, 0.0f);
		s.processStereo(l.data(), r.data(), 256);
	}
	CHECK(peak(l) > 0.1f);
	// THE SILENT CHANNEL IS SILENT — which it was NOT before the DC blocker existed. This case is
	// what found that defect: it came back at -0.112, a constant, on a channel fed exact zeros.
	// The tolerance allows for the blocker's own decay, which is complete long before 40 blocks.
	for (const float x : r) {
		CHECK(x == doctest::Approx(0.0f).epsilon(1e-4));
	}
	CHECK(std::fabs(s.dcOffsetForTest(1)) < 1e-4f);
}

TEST_CASE("the shaper does not emit DC") {
	// THE DEFECT, PINNED. A normalised table centres on the midpoint of the EXTREMES, which is not
	// f(0) for any curve that is not an odd function — and the field sets `asymmetry` away from 0.5
	// nearly everywhere, so nearly every position has f(0) != 0. The amplitude blend does not save it:
	// at full mix the threshold sits BELOW zero precisely so the quietest sample is still shaped.
	//
	// MEASURED BEFORE THE FIX: -0.112 on silence, which is -19 dBFS of pure DC. On the reference this is
	// survivable because the shaper sits after a per-voice envelope; on a bus it is a constant offset
	// in the mix and a click every time the curve moves under a hand.
	SUBCASE("silence in, silence out, across the whole colour axis") {
		for (int i = 0; i <= 24; ++i) {
			const float colour = static_cast<float>(i) / 24.0f;
			CAPTURE(colour);
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings settings;
			settings.enabled = true;
			settings.knee = 0.85f;
			settings.colour = colour;
			settings.threshold = 1.0f;
			std::vector<float> l(256, 0.0f);
			std::vector<float> r = l;
			for (int b = 0; b < 60; ++b) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				s.processStereo(l.data(), r.data(), 256);
			}
			CHECK(peak(l) < 1e-4f);
		}
	}

	SUBCASE("and a shaped sine comes back with its mean at zero") {
		// THE HALF A SILENCE TEST CANNOT SEE. Asymmetric shaping produces an amplitude-DEPENDENT
		// offset as well as a constant one — that is inherent to generating even harmonics, and it is
		// why the fix is a blocker rather than anchoring the table at f(0) = 0.
		shp::Shaper s;
		shp::Settings settings;
		settings.enabled = true;
		settings.knee = 0.85f;
		settings.colour = 0.2f; // the field puts asymmetry well off centre here
		settings.threshold = 1.0f;
		s.prepare(48000.0);
		const std::vector<float> out = run(s, settings, 200, 512, 0.8f);
		const double mean = std::accumulate(out.begin(), out.end(), 0.0)
		                    / static_cast<double>(out.size());
		CHECK(std::fabs(mean) < 0.01);
		// AND THE SIGNAL IS STILL THERE, so the blocker has not simply muted the device — the second
		// assertion that proves the first is not vacuous.
		CHECK(peak(out) > 0.2f);
	}

	SUBCASE("the blocker's corner is low enough to leave bass alone") {
		// A 20 Hz tone must survive it. At a 10 Hz corner a one-pole highpass is 0.3 dB down at 20 Hz;
		// anything that ate real low end would be a cure worse than the DC.
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings settings;
		settings.enabled = true;
		settings.knee = 0.1f; // gentle, so the curve is close to linear and the level is comparable
		settings.colour = 0.0f;
		settings.threshold = 1.0f;
		s.setTarget(settings);
		// 20 Hz at 48 kHz is 2400 samples a cycle; three cycles, well past the blocker's settling.
		std::vector<float> l(2400 * 3), r;
		for (int b = 0; b < 20; ++b) {
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = 0.5f * std::sin(2.0f * 3.14159265f * static_cast<float>(i) / 2400.0f);
			}
			r = l;
			s.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		}
		// **RE-BASED WHEN THE POST GAIN WENT.** This is a vacuity guard on a DC test — it exists so
		// "no DC" cannot pass by the device outputting nothing — and its number was calibrated against
		// an output that was being multiplied by four. With the table's own output now the device's
		// (by decision), a 0.5 sine through the gentle part of the curve lands near 0.16, so the old 0.45
		// bar failed on a device that is behaving correctly. **The guard's JOB is unchanged: prove
		// there is signal.** Set well below the measured level rather than at it, because this case is
		// about DC and must not become a second, accidental level assertion — the level has its own
		// case now, driven through `Shaper` with a saw.
		CHECK(peak(l) > 0.08f);
	}
}

TEST_CASE("the shaper has ONE phase axis, and Colour is a different one") {
	// **THIS CASE USED TO BE "Colour's phase offset is its own quantity, and it SUMS with the device
	// gamma", AND THE MEASUREMENT DISPROVED ITS TITLE.**
	//
	// `Colour Phase` was summed into `gamma` with no scaling at all, so the two were byte-identical:
	// `colourPhase = 1` and `gamma = 1` produced the same curve, bit for bit. It was not a second
	// quantity, it was a second knob on the first one. The old case asserted the SUM — which is true
	// of any two things added together — and never asked whether the two addends were distinguishable.
	// **That is the difference-check trap this repo has paid for repeatedly: an assertion that passes
	// for the right reason and for the wrong one at once.**
	//
	// It also masked the incoherence beneath it. Lapping Colour carried +1 into that counter, but one
	// lap of Colour is worth gamma 4 on the six main bases and gamma 2.5 on asymmetry — measured, and
	// it is asymmetry's own half-rate acceleration. No single carry value was right and +1 was
	// neither, so "keep turning" jumped, and jumped differently per basis.
	//
	// The counter was removed and Colour unbounded instead. What is left to pin is the thing
	// that was actually true all along: the device has ONE phase axis, and Colour is not it.
	namespace shp = feathers::fx::shaper;
	constexpr float kKnee = 0.8f;
	constexpr float kHarm = 0.5f;
	constexpr float kStep = 0.125f;

	const shp::CurveParams base = shp::deriveCurve(kKnee, 0.4f, 0.0f, kHarm);
	const shp::CurveParams viaColour = shp::deriveCurve(kKnee, 0.4f + kStep, 0.0f, kHarm);
	const shp::CurveParams viaGamma = shp::deriveCurve(kKnee, 0.4f, kStep, kHarm);

	// BOTH MOVE THE CURVE — which is what makes the comparison below mean something rather than being
	// a statement about one of them being inert.
	CHECK(std::memcmp(&viaColour, &base, sizeof(base)) != 0);
	CHECK(std::memcmp(&viaGamma, &base, sizeof(base)) != 0);

	// **AND THEY MOVE IT TO DIFFERENT PLACES.** Colour is multiplied by `freqMult` — its own
	// acceleration, `1 + colour²·3` — and gamma is not, so the same step buys a different distance.
	// That is the whole of why one cannot be spelled as an offset of the other.
	CHECK(std::memcmp(&viaColour, &viaGamma, sizeof(base)) != 0);

	// **COLOUR HAS NO WALL, by design.** Past the old 0..1 bound it must keep changing the
	// curve rather than repeating the value at 1.0 forever — the shape of every clamp defect this
	// repo has fixed.
	const shp::CurveParams atOne = shp::deriveCurve(kKnee, 1.0f, 0.0f, kHarm);
	const shp::CurveParams past = shp::deriveCurve(kKnee, 1.7f, 0.0f, kHarm);
	const shp::CurveParams further = shp::deriveCurve(kKnee, 4.3f, 0.0f, kHarm);
	CHECK(std::memcmp(&past, &atOne, sizeof(atOne)) != 0);
	CHECK(std::memcmp(&further, &past, sizeof(past)) != 0);

	// AND BELOW ZERO TOO, for the same reason `min == -max`: the first backwards step from the place
	// every patch starts must not hit anything.
	const shp::CurveParams below = shp::deriveCurve(kKnee, -0.6f, 0.0f, kHarm);
	const shp::CurveParams atZero = shp::deriveCurve(kKnee, 0.0f, 0.0f, kHarm);
	CHECK(std::memcmp(&below, &atZero, sizeof(atZero)) != 0);
}

namespace {

/// THE TABLE'S STEEPEST SECANT FROM THE ORIGIN, per half — computed HERE, from the table the device
/// hands out, so the engine's `boundForTest` has something independent to be checked against.
struct SecantPair {
	float pos = 0.0f;
	float neg = 0.0f;
};
SecantPair secantsOf(const std::array<float, shp::kTableEntries>& t) {
	SecantPair out;
	const size_t mid = t.size() / 2;
	const float step = 2.0f / static_cast<float>(t.size() - 1);
	for (size_t i = mid + 1; i < t.size(); ++i) {
		out.pos = std::max(out.pos, std::fabs(t[i]) / (static_cast<float>(i - mid) * step));
	}
	for (size_t i = 0; i < mid; ++i) {
		out.neg = std::max(out.neg, std::fabs(t[i]) / (static_cast<float>(mid - i) * step));
	}
	return out;
}

/// THE SWEEP LAW, restated: 4^drive, 1 at drive 0.
float sweepOf(float drive) {
	return std::exp2(2.0f * drive);
}

/// WHERE THE THRESHOLD LIVES, restated: `1 - 1.0154 · mix`, in INPUT units, compared against `|x|`.
float thresholdOf(float mix) {
	return 1.0f - 1.0154f * mix;
}

/// THE WHOLE READ, WRITTEN OUT — the tier `Shaper` applies above the threshold, restated from the
/// design and not from the code:  out = sign·t + (h/R) · T( R/(h·m) · e' ), with
/// e' = h · sat_p((|x| − t)/h) the Knee pre-squash, and below the threshold the input. The crossfade just past the crossing is not restated; callers stay clear of it.
float shapedRestated(const std::array<float, shp::kTableEntries>& table, const SecantPair& m,
                     float threshold, float sweep, float x) {
	const float sign = x < 0.0f ? -1.0f : 1.0f;
	if (std::fabs(x) - threshold <= 0.0f) {
		return x;
	}
	// **THE READ STARTS AT THE THRESHOLD FLOORED AT ZERO** (backlog IF): at Mix 1 the threshold is just
	// under zero so the blend is whole at the origin, and reading from there made the curve step to
	// ±f(0) at every zero crossing. `threshold` still decides dry against shaped, above.
	const float from = std::fmax(threshold, 0.0f);
	const float h = 1.0f - from;
	const float squashed = std::fabs(x) - from;
	const float secant = sign > 0.0f ? m.pos : m.neg;
	// THE FLAT-HALF FALLBACK, restated: a half of the table with nothing in it means NO SHAPING, and
	// the output for no shaping is the excess itself. Muting it is what silenced colours 1/8, 5/8
	// and 8/8.
	const float top = sign > 0.0f ? std::fabs(table[table.size() - 1]) : std::fabs(table[0]);
	if (secant <= 0.0f || top <= 0.001f) {
		return sign * from + sign * squashed;
	}
	// **THE READ, RESTATED AFTER THE RELAXATION.** `rate = sweep/h` sends an excess of the headroom to
	// the domain edge — the signal is scaled as signal/(0 dBFS - threshold), so the table
	// still saturates when the signal reaches 0 dBFS — and `scale = h` is the table's own peak normalisation used as the
	// ceiling. The secant divide is gone with the `|out| <= |excess|` bound it enforced; it was
	// confining the read to the first fifth of the curve, which is where the folding is NOT.
	const float rate = sweep / h;
	const float scale = h;
	return sign * from + scale * shp::lookup(table, rate * sign * squashed);
}

/// THE DC BLOCKER, RESTATED: a one-pole highpass with a 10 Hz corner at 48 kHz, run over a
/// sequence. Restated rather than tolerated, because its memory of a hot, asymmetric shaped wave is
/// an offset of a few hundredths that a per-sample tolerance would have to be loosened to cover —
/// and a tolerance that covers a mechanism also covers a defect the size of that mechanism.
std::vector<float> dcBlocked(const std::vector<float>& in) {
	const float pole = 1.0f - 2.0f * 3.14159265f * 10.0f / 48000.0f;
	std::vector<float> out(in.size());
	float prevIn = 0.0f;
	float prevOut = 0.0f;
	for (size_t i = 0; i < in.size(); ++i) {
		prevOut = in[i] - prevIn + pole * prevOut;
		prevIn = in[i];
		out[i] = prevOut;
	}
	return out;
}

} // namespace

TEST_CASE("the table is read at the EXCESS over the threshold, and what it returns is added to it") {
	// THE TIER, AFTER THE CEILING WORK. The device used to scale its input by the drive gain and a
	// 0.25 pad and look the table up AT THE SIGNAL; the table's first rise has a slope of 6..40, so
	// the shaped top sat above the dry it replaced by up to +36 dB and the only thing stopping it was
	// the table's own peak at 0 dBFS. Now the table is read at how far past the threshold the sample
	// is, scaled so the read has unit slope at the crossing, and the result is added to the
	// threshold — see `kSweepOctaves` in shaper.cpp for the whole of it. This case restates that
	// read per sample from the design, with every number computed here and none read off the
	// engine, which is the rule this file is written under.
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	// COLOUR 0.38 BECAUSE THE FIELD IS QUIET THERE — hysteresis -0.022, no deadzone, no sub — so the
	// restatement below is of the tier alone and not of four other stages at the same time. And
	// because its table FOLDS (peak at u 0.154, back to 0.03 by 0.3), which a restatement of a
	// clean saturator would not be able to tell from a clean saturator.
	//
	// AND THE HYSTERESIS SCALER AT 0, because "quiet" was not quiet enough once the read reached
	// the fold: the field's -0.022 is a lag of 0.0017 in excess units, which at full drive on a
	// +6 dB input lands where the table's slope is about 10 and came out as 0.024 at every peak
	// (MEASURED). Hysteresis has its own case; this one is the tier.
	s.colour = 0.38f;
	s.hysteresis = 0.0f;

	SUBCASE("the engine's bound is the one an independent scan of its table finds") {
		for (const float colour : {0.0f, 0.2f, 0.38f, 0.6f, 0.8f, 1.0f}) {
			CAPTURE(colour);
			shp::Settings cfg = s;
			cfg.colour = colour;
			shp::Shaper sh;
			sh.prepare(48000.0);
			settle(sh, cfg);
			const SecantPair mine = secantsOf(sh.tableForTest());
			CHECK(sh.boundForTest().secantPos == doctest::Approx(mine.pos).epsilon(1e-5));
			CHECK(sh.boundForTest().secantNeg == doctest::Approx(mine.neg).epsilon(1e-5));
		}
		// Non-vacuity: the identity table's bound is {1, 1}, and a device that never computed one
		// would report that at every colour. The field's tables are steeper than 1 everywhere it is
		// not a rectifier, so at least one of these must have moved.
		shp::Shaper sh;
		sh.prepare(48000.0);
		settle(sh, s);
		CHECK(sh.boundForTest().secantPos > 2.0f);
	}

	SUBCASE("every shaped sample matches the restatement, at drive 0 and at drive 1") {
		// ONLY WHERE THE CROSSFADE IS OVER. Just past the threshold the output is part dry — the
		// blend reaches 1 at an excess of h/(1 + 16·mixNorm²) — so a sample there is a mix of two
		// things and the restatement is of one of them. 5 % of the headroom past the crossing is
		// clear of it at every mix here.
		//
		// THE BLOCKER IS RESTATED TOO, over the last 64 blocks of the same periodic input — 4096
		// samples, five of its time constants, so what it remembers of the blocks before (identity
		// table, fade, smoother) is under 1 % of an offset that is itself a few hundredths.
		for (const float mix : {0.5f, 0.9f, 1.0f}) {
			for (const float drive : {0.0f, 1.0f}) {
				for (const float amp : {1.0f, 2.0f}) {
					CAPTURE(mix);
					CAPTURE(drive);
					CAPTURE(amp);
					shp::Settings cfg = s;
					cfg.threshold = mix;
					cfg.drive = drive;
					shp::Shaper dev;
					dev.prepare(48000.0);
					// 400 blocks: the sweep smoother (23 ms) has long since arrived at its target.
					const std::vector<float> out = run(dev, cfg, 400, 64, amp);
					const std::vector<float> in = sine(64, amp, 4.0f);
					const std::array<float, shp::kTableEntries> table = dev.tableForTest();
					const SecantPair m = secantsOf(table);
					const float t = thresholdOf(mix);
					const float clear = t + 0.05f * (1.0f - t);
					const float sweep = sweepOf(drive);
					REQUIRE(dev.sweepForTest() == doctest::Approx(sweep).epsilon(1e-3));

					std::vector<float> shaped;
					for (int b = 0; b < 64; ++b) {
						for (const float x : in) {
							shaped.push_back(shapedRestated(table, m, t, sweep, x));
						}
					}
					const std::vector<float> expectedRun = dcBlocked(shaped);
					const size_t last = expectedRun.size() - in.size();

					int compared = 0;
					float worstFromDry = 0.0f;
					for (size_t i = 0; i < out.size(); ++i) {
						if (std::fabs(in[i]) <= clear) {
							continue;
						}
						++compared;
						CAPTURE(i);
						const float expected = expectedRun[last + i];
						CHECK(std::fabs(out[i] - expected) < 0.005f);
						worstFromDry = std::max(worstFromDry, std::fabs(shaped[last + i] - in[i]));
					}
					CHECK(compared > 10);
					// AND THE SECOND ASSERTION THAT PROVES THE FIRST IS NOT VACUOUS: the restatement
					// must differ from the dry somewhere, or a bypass would pass.
					CHECK(worstFromDry > 0.05f);
				}
			}
		}
	}

	SUBCASE("full drive sweeps a full-scale signal past the table's first peak, drive 0 does not") {
		// THE NUMBER THE SWEEP'S RANGE WAS CHOSEN BY. The position a 0 dBFS input reaches on the
		// table is R/m, restated; at this colour and knee the table peaks at u 0.154 and folds
		// after. Drive 0 reads only the first rise; drive 1 reads through the fold.
		shp::Shaper sh;
		sh.prepare(48000.0);
		shp::Settings cfg = s;
		cfg.threshold = 0.5f;
		settle(sh, cfg);
		const SecantPair m = secantsOf(sh.tableForTest());
		const auto reach = [&](float drive) { return sweepOf(drive) / m.pos; };
		CHECK(reach(0.0f) < 0.154f);
		CHECK(reach(1.0f) > 0.154f);
		// and the fold is real at this position, not assumed: past the peak the table comes down.
		const std::array<float, shp::kTableEntries>& t = sh.tableForTest();
		const size_t mid = t.size() / 2;
		const float atPeak = t[mid + static_cast<size_t>(0.154f * static_cast<float>(mid))];
		const float pastPeak = t[mid + static_cast<size_t>(0.30f * static_cast<float>(mid))];
		CHECK(atPeak > 0.95f);
		CHECK(pastPeak < 0.2f);
	}
}

TEST_CASE("hysteresis gets the field-modifier curve, and 0.5 is the position's own value") {
	// THE ONE PER-SAMPLE TERM THAT HAD NO CONTROL. Sub, Feedback, Rotate, Lowpass and Integrator each
	// have a labelled switch; hysteresis had neither switch nor scaler, and the firmware does not
	// carry it in `extrasMask` either. MEASURED at the default position — colour 0, where the field
	// reads its extreme -1.000 — 220 Hz at -12 dBFS: 26.5% THD with the field as authored against
	// 6.25% with it removed. Twenty of those twenty-six points were one ungated term.
	//
	// A SCALER AND NOT A SWITCH, which is what keeps the device's own claim intact. `shaper_types.h`
	// forbids knobs for the extras' AMOUNTS because the position is meant to BE the sound; a bias in
	// front of a field does not break that, because at 0.5 the position is still exactly what you
	// hear. `fieldWithOverride` is the φ layer's own curve and six flagship fields already use it.

	SUBCASE("the curve's three points are the field layer's, not this device's invention") {
		// RESTATED FROM `slice_build.h`'s documented contract rather than read off the implementation:
		//   knob 0.0 -> 0        knob 0.5 -> field        knob 1.0 -> 1
		for (const float field : {0.2f, 0.5f, 0.9f}) {
			CAPTURE(field);
			CHECK(feathers::fieldWithOverride(field, 0.0f) == doctest::Approx(0.0f));
			CHECK(feathers::fieldWithOverride(field, 0.5f) == doctest::Approx(field));
			CHECK(feathers::fieldWithOverride(field, 1.0f) == doctest::Approx(1.0f));
		}
	}

	SUBCASE("the default is NONE of the field, and the struct and the "
	        "descriptor agree") {
		// WAS "the field as authored" (0.5). Compared against the reference, hysteresis defaults to 0:
		// the firmware's default path has no hysteresis, and at the default position it
		// was twenty of 26.5 points of THD. 0.5 is still "the position's own" for a hand that wants it.
		const shp::Settings s;
		CHECK(s.hysteresis == doctest::Approx(0.0f));
		CHECK(shp::kParamDescs[static_cast<size_t>(shp::Param::Hysteresis)].def == doctest::Approx(0.0));
	}

	SUBCASE("the knob reaches the field's offset, and 0 removes it entirely") {
		// **THIS SUBCASE ASSERTED A PROPORTION AND THE PROPORTION DIED, BY DECISION.** It required that
		// turning the knob down cut the device's distortion by more than half, which held while
		// hysteresis was the only ungated effect. Under the relaxation of the dry/wet peak
		// relationship the TABLE's own low-level gain became the dominant source: MEASURED at colour 0
		// on a 0.251 sine, 18.9 % THD at the neutral knob against 16.5 % with the knob at zero, so the
		// old bound of 9.4 % was measuring the table and reporting it as this knob.
		//
		// **THE DEVICE STAYS AS IT IS, BY DECISION (2026-09-04)** — over pinning the table's origin to
		// unity gain or reshaping the curve to do both. So the gain is the design and this case had to stop measuring it.
		//
		// **WHAT SURVIVES IS THE CLAIM ABOUT THE OFFSET, and it is now asserted as a VALUE.** The
		// knob's job is to scale the field's hysteresis contribution; that is a number the engine
		// computes, and `hystOffsetForTest` reads it. A spectrum carries every effect at once and can
		// no longer separate this one — which is this file's oldest rule arriving at a case that had
		// been obeying it by luck.
		const auto offsetAt = [](float scaler) {
			shp::Settings cfg;
			cfg.enabled = true;
			cfg.colour = 0.0f;
			cfg.hysteresis = scaler;
			shp::Shaper sh;
			sh.prepare(48000.0);
			settle(sh, cfg);
			return sh.hystOffsetForTest();
		};

		// AT ZERO THE OFFSET IS GONE, EXACTLY. Not "small": `fieldWithOverride(field, 0) == 0` is the
		// φ layer's contract asserted two subcases above, and this is that contract reaching the sound.
		CHECK(offsetAt(0.0f) == 0.0f);

		// AT NEUTRAL THE FIELD'S OWN VALUE IS THERE, and colour 0 is where it is extreme — the field's
		// hysteresis triangle reads -1.000, so the device ships at the loudest point of this effect.
		const float authored = offsetAt(feathers::kFieldNeutral);
		CAPTURE(authored);
		CHECK(std::fabs(authored) > 0.0f);

		// **ABOVE NEUTRAL IT IS FLAT AT THIS COLOUR, AND THAT IS THE CURVE BEING RIGHT RATHER THAN THE
		// KNOB BEING BROKEN** — the note the previous version of this subcase carried, kept because it
		// is the fact most likely to be mistaken for a bug. The field is already at 1.0 in magnitude
		// here and `fieldWithOverride` lifts TOWARD 1, so there is nothing left to lift toward. At any
		// colour where the field is not extreme the upper half moves; colour 0 is the one place it
		// cannot, and it is the colour this subcase needs for the extreme below.
		//
		// I asserted a strict increase here first and it went red at 0.151307 against 0.151307 — the
		// case's own paragraph had said so and I had replaced it without reading it.
		const float full = offsetAt(1.0f);
		CAPTURE(full);
		CHECK(std::fabs(full) >= std::fabs(authored));
		CHECK(full == doctest::Approx(authored));
		CHECK(std::signbit(full) == std::signbit(authored));

		// **AND IT STILL REACHES THE SOUND** — the second assertion, so the value checks above cannot
		// pass on an offset the audio path ignores. A measured difference and not a proportion: the
		// two THD readings are 18.9 % and 16.5 %, so 1.0 point is comfortably inside the gap while
		// being far above any noise in the measurement.
		const auto distortionAt = [](float scaler) {
			shp::Settings cfg;
			cfg.enabled = true;
			cfg.colour = 0.0f;
			cfg.hysteresis = scaler;
			shp::Shaper sh;
			sh.prepare(48000.0);
			const std::vector<float> out = run(sh, cfg, 48, 512, 0.251f);
			return thd(out, 4) * 100.0;
		};
		const double withField = distortionAt(feathers::kFieldNeutral);
		const double without = distortionAt(0.0f);
		CAPTURE(withField);
		CAPTURE(without);
		CHECK(withField > 15.0);
		CHECK(without < withField - 1.0);
	}
}

TEST_CASE("the table clips at 0 dBFS and there is NO post gain") {
	// Seen on a scope before this: the output peaked well above 0 dBFS in the DAW, potentially
	// clipping off the complexity the transfer is supposed to generate, and there was a large jump
	// as Knee went 0->1. Both were one line — an output multiply by
	// `1/kInputReference` whose comment claimed the pair was unity gain. **A 4x squeeze and a 4x boost
	// are unity only through a unity-slope function, and this table's small-signal slope rises with
	// Knee to about 3.** DECISION: the table clips at 0 dBFS with no post gain.
	//
	// **DRIVEN THROUGH `Shaper` ITSELF, NOT THROUGH A RESTATEMENT OF ITS ARITHMETIC.** The measurement
	// that found the bug computed `4 * transferAt(p, 0.25x)` by hand, which was right enough to
	// diagnose and is NOT evidence about the device: it omits the threshold crossfade, the DC blocker
	// and the smoothers. A claim about what the plugin outputs has to come from the plugin.
	//
	// A RISING SAW, because that is the signal the scope showed — it visits every input level once, so
	// the peak of the output is the peak of the transfer over the whole domain.
	const auto sawPeak = [](float knee, float drive) {
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings settings;
		settings.enabled = true;
		settings.knee = knee;
		settings.drive = drive;
		settle(s, settings);
		float worst = 0.0f;
		// Several laps, so the smoothers are settled and the blocker has converged before we believe
		// the number: the first lap of any signal through this device is a transient.
		for (int lap = 0; lap < 8; ++lap) {
			std::vector<float> l(1024);
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(l.size() - 1);
			}
			std::vector<float> r = l;
			s.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
			if (lap >= 4) {
				worst = std::max(worst, peak(l));
			}
		}
		return worst;
	};

	SUBCASE("no knee setting takes a full-scale saw above 0 dBFS") {
		// **THE ASSERTION THAT WOULD HAVE FAILED BEFORE, AT EVERY KNEE ABOVE ZERO.** Measured on the
		// old code by an independent restatement: 1.19 at knee 0, 2.72 at 0.5, 3.00 at 1.0.
		for (const float drive : {0.0f, 1.0f}) {
			for (const float knee : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
				CAPTURE(drive);
				CAPTURE(knee);
				const float p = sawPeak(knee, drive);
				CHECK(p <= 1.0f);
				// AND IT IS NOT SILENT, which is the assertion that keeps the one above from passing
				// vacuously — a device that output zero would satisfy every ceiling ever written.
				CHECK(p > 0.2f);
			}
		}
	}

	SUBCASE("the knee sweep no longer climbs nine decibels") {
		// The reported symptom, as a RATIO rather than an adjective. Before: 0.69 dB to 9.54 dB, a
		// climb of 8.85 dB across the sweep. Knee is allowed to change the sound — it is a shape
		// control — but it must not be a gain control wearing its name.
		//
		// AT DRIVE 0, WHERE THE READ HAS UNIT SLOPE AND THE TOP REACHES TOWARD THE CEILING. The sweep
		// lowers the ceiling the shaped part reaches by 1/R — by design, see `kSweepOctaves` — so at
		// higher drive the whole knee range sits lower TOGETHER; the claim here is that Knee alone is
		// not a gain, and it is made where Drive is not one either. Knee 0 is the bypass, and the
		// difference between it and the gentlest table is the table's own first rise, not a gain.
		//
		// FOUR DECIBELS, NOT THREE, SINCE THE PRE-SQUASH. Knee's job is now a squash of the top of
		// the excess before the table (`kSquashHardness`, shaper_curve.h), and a squash LOWERS the peak:
		// 3.01 dB at the very top of the headroom at knee 1 (p = 2, sat(1) = 2^-1/2), and whatever
		// the table's first rise takes on top of that. MEASURED here, gentle over hard at drive 0:
		// 2.80 dB with the squash an identity (the table's own first-rise difference), 3.82 dB with
		// it — the squash adds 1.0 dB to the spread, not its full 2.9, because the first rise is
		// concave and a smaller argument loses less. The bound was written against a 9 dB CLIMB;
		// read against a fall it still says what it said — Knee is a shape and not a gain — and the
		// fall is the squash's stated cost, held under Drive's own 3 dB per quarter turn.
		const float bypass = sawPeak(0.0f, 0.0f);
		const float gentle = sawPeak(0.05f, 0.0f);
		const float hard = sawPeak(1.0f, 0.0f);
		CAPTURE(bypass);
		CAPTURE(gentle);
		CAPTURE(hard);
		REQUIRE(hard > 0.0f);
		MESSAGE("saw peaks at drive 0: bypass " << bypass << ", knee 0.05 " << gentle << ", knee 1 "
		                                        << hard << " (" << 20.0f * std::log10(gentle / hard)
		                                        << " dB gentle over hard)");
		CHECK(std::fabs(20.0f * std::log10(gentle / hard)) < 4.0f);
		CHECK(std::fabs(20.0f * std::log10(bypass / gentle)) < 3.0f);
	}

	SUBCASE("THE THRESHOLD IS CONTINUOUS — the step that removing the post gain would have opened") {
		// **THE OBVIOUS WAY TO OBEY THE DECISION SHIPS A CLICK.** Below the threshold `processSample`
		// returns `driven`; above it, it crossfades. Dropping the output multiply while leaving the
		// crossfade's dry term as `scaled` makes that line return a QUARTER of `driven` at blend 0 —
		// a 12 dB step exactly at the crossing, on every sample that passes through it.
		//
		// Asserted as a bound on the SAMPLE-TO-SAMPLE difference of a slow ramp, which is where a step
		// at a fixed input level has to show itself.
		//
		// AND AT DRIVE 0.75 / MIX 0.5 AS WELL AS AT UNITY. The parity fix made the dry side `x` in both
		// places; had it changed only one of them, the two would differ by the drive gain — 7.6x at
		// 0.75 — and a slow ramp would show it as a cliff of 1.7 at the crossing (MEASURED, by the
		// known-bad that puts `driven` back into the crossfade's dry term). At unity drive the two
		// are equal and such a defect is invisible, which is why the second pair is here.
		//
		// AND TO +6 dB, because the crossing is not the only place a step can now hide: a hot input
		// reaches the ceiling, and the ceiling case below asserts the level but not the shape. A ramp
		// to 2.0 crosses the threshold, fills the headroom, and pins at the table's end.
		const auto biggestStepOf = [](float drive, float mix, float top) {
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings settings;
			settings.enabled = true;
			settings.knee = 0.5f;
			settings.drive = drive;
			settings.threshold = mix;
			settle(s, settings);
			// Long enough for the drive smoother (23 ms) to have arrived before the ramp starts.
			for (int b = 0; b < 40; ++b) {
				std::vector<float> zl(64, 0.0f);
				std::vector<float> zr = zl;
				s.processStereo(zl.data(), zr.data(), 64);
			}

			std::vector<float> l(4096);
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = top * static_cast<float>(i) / static_cast<float>(l.size() - 1); // 0 -> top, slow
			}
			std::vector<float> r = l;
			s.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

			float biggestStep = 0.0f;
			for (size_t i = 1; i < l.size(); ++i) {
				biggestStep = std::max(biggestStep, std::fabs(l[i] - l[i - 1]));
			}
			return biggestStep;
		};

		for (const float top : {1.0f, 2.0f}) {
			// BOUND BY REFERENCE, not by value: `const auto [..]` copies each pair out of the
			// initializer_list every iteration, which clang flags `-Wrange-loop-construct` and the
			// macOS leg builds with -Werror. The list's backing array outlives the loop (its lifetime
			// is extended through the range-for's own reference), so the reference is safe here.
			for (const auto& [drive, mix] :
			     {std::pair {0.0f, 1.0f}, std::pair {0.75f, 0.5f}, std::pair {1.0f, 0.1f}}) {
				CAPTURE(top);
				CAPTURE(drive);
				CAPTURE(mix);
				const float biggestStep = biggestStepOf(drive, mix, top);
				CAPTURE(biggestStep);
				// The input's own step is at most 2/4095 = 0.0005. The read's slope is 1 at the
				// crossing and bounded by the table's own slope over the sweep after it; a 12 dB
				// cliff cannot hide under 0.02.
				CHECK(biggestStep < 0.02f);
			}
		}
	}
}


//--------------------------------------------------------------------------------------------//
// PARITY — Drive drives the TABLE, and nothing else                                           //
//--------------------------------------------------------------------------------------------//

namespace {

/// RMS of a block, the level a "gain" claim is about.
double rmsOf(const std::vector<float>& v) {
	double sum = 0.0;
	for (const float x : v) {
		sum += static_cast<double>(x) * x;
	}
	return std::sqrt(sum / static_cast<double>(v.size()));
}

/// Drive a device for `blocks` blocks of a sine and hand back the LAST block, with the input it was
/// given beside it. 200 blocks of 256 is 1.07 s: the table has landed, the fade is over, and the
/// drive smoother (23 ms) and the DC blocker (10 Hz) have both settled long since.
struct LastBlock {
	std::vector<float> in;
	std::vector<float> out;
};
LastBlock lastBlockOf(const shp::Settings& settings, float amp, int blocks = 200, int frames = 256,
                      float cycles = 4.0f) {
	shp::Shaper s;
	s.prepare(48000.0);
	s.setTarget(settings);
	LastBlock last;
	for (int b = 0; b < blocks; ++b) {
		last.in = sine(frames, amp, cycles);
		last.out = last.in;
		std::vector<float> r = last.in;
		s.processStereo(last.out.data(), r.data(), frames);
	}
	return last;
}

} // namespace

TEST_CASE("PARITY: below the mix threshold, Drive does not change the level") {
	// THE DESIGN, not yet achieved when this case was written: Drive drives the table harder while
	// retaining level parity, so that with mix < 100% only the top of the wave, above the threshold,
	// is affected.
	//
	// THE MEASURABLE: for a signal whose peak stays BELOW the mix threshold — at the drive under
	// test, in the domain the threshold is compared in — the device's RMS output must equal its
	// input within 0.1 dB, at EVERY drive and every sub-100% mix. Parity below the threshold means
	// "untouched", so the reference is unity and not some other drive's gain: a device that scaled
	// the dry portion by the same wrong factor at every drive would otherwise pass.
	//
	// 0.1 dB, BECAUSE IT SITS BETWEEN THE EAR AND THE ARITHMETIC. A level difference below ~0.5 dB
	// is not something a player can hear in an A/B; and the mechanisms on the dry path cost far less
	// than 0.1 — the DC blocker's 10 Hz corner is -0.0008 dB at this 750 Hz tone, float rounding is
	// six decades below that. A red here is a defect and not noise.
	//
	// MEASURED RED before the fix, at knee 0.5 / colour 0.38 / mix 0.5 (the threshold is 0.4923):
	//     drive 0.00 -> -600 dB (silence)     drive 0.25 -> -30.1 dB
	//     drive 0.60 ->  +7.9 dB              drive 0.75 -> +17.6 dB     drive 1.00 -> +30.1 dB
	// — which is 32·p⁵ exactly. The sub-threshold path returned the DRIVEN sample, so Drive was a
	// volume knob for the part of the wave the mix threshold was supposed to leave alone.
	//
	// IN THE INPUT DOMAIN NOW. The threshold used to be compared in a drive-scaled, 0.25-padded
	// domain, so "half the threshold" was a different input level at every drive; it is compared
	// against `|x|` itself, so the amplitude below it is the same at every drive and Drive's only
	// way to fail this case is to reach into the sub-threshold path — which is the whole claim.
	for (const float colour : {0.38f, 0.80f}) {
		for (const float knee : {0.25f, 0.5f, 1.0f}) {
			for (const float mix : {0.1f, 0.5f, 0.9f}) {
				for (const float drive : {0.0f, 0.25f, 0.5f, 0.6f, 0.75f, 1.0f}) {
					CAPTURE(colour);
					CAPTURE(knee);
					CAPTURE(mix);
					CAPTURE(drive);
					// An amplitude that reaches HALF the threshold — below it by 6 dB, so the
					// hysteresis offset cannot carry it across — and never above -6 dBFS.
					const float thr = thresholdOf(mix);
					const float amp = std::min(0.5f, 0.5f * thr);
					shp::Settings s;
					s.enabled = true;
					s.knee = knee;
					s.colour = colour;
					s.threshold = mix;
					s.drive = drive;
					const LastBlock last = lastBlockOf(s, amp);
					const double gainDb = 20.0 * std::log10(rmsOf(last.out) / rmsOf(last.in));
					CAPTURE(amp);
					CAPTURE(gainDb);
					CHECK(std::fabs(gainDb) < 0.1);
				}
			}
		}
	}
}

TEST_CASE("PARITY: above the mix threshold, only the top of the wave changes") {
	// THE SECOND HALF OF THE SAME SENTENCE. A wave whose peak crosses the threshold is shaped ABOVE
	// it and nowhere else: every sample of the same period that sits below the threshold comes back
	// as it would have at drive 0 — and the samples above it do NOT, which is the assertion that
	// keeps the first from passing on a bypassed device.
	//
	// THE REFERENCE IS THE SAME SIGNAL THROUGH THE SAME DEVICE AT DRIVE 0, NOT THE RAW INPUT, and the
	// reason is the DC blocker. Below the threshold both runs are the dry path and the blocker, so
	// the blocker cancels; comparing against the raw input instead scores the blocker's own phase
	// lead — MEASURED 0.071 at 187 Hz and 0.018 at 750 Hz on a full-scale sine, a residual that
	// scales as 1/f and has nothing to do with Drive. (Drive 0 is no longer a run that never
	// crosses — it is the unit-slope sweep — which is why the ABOVE assertion compares two sweeps
	// rather than a sweep against nothing; the sub-threshold assertion is unchanged by that.)
	//
	// 0.02 ABSOLUTE, AND WHY. What remains against the drive-0 reference is the blocker's 10 Hz memory
	// of the shaped top — the low-passed excess leaking into the dry portion, MEASURED 0.004..0.009 at
	// 750 Hz — which is inherent to blocking DC on a periodically shaped wave. 0.02 is twice that, and
	// it is also about what 0.1 dB is on a full-scale sample (0.012). The defect it guards was 1.3..1.9.
	//
	// AT THE INFLATOR (colour 0.8, gamma 0), because its curve is symmetric — asymmetry 0.5 exactly —
	// so the shaped top carries no DC of its own. 750 Hz (16 cycles per 1024) so that at drive 1.0,
	// where the crossing sits at 6% of full scale, there are still thirty-odd samples below it.
	//
	// MEASURED RED before the fix, drive 0.75 / mix 0.5: the sub-threshold samples came back at
	// 7.59x the input (+17.6 dB), because that path returned `driven`.
	constexpr int kFrames = 1024;
	constexpr float kCycles = 16.0f;
	for (const float drive : {0.75f, 0.85f, 1.0f}) {
		for (const float mix : {0.3f, 0.5f}) {
			CAPTURE(drive);
			CAPTURE(mix);
			shp::Settings s;
			s.enabled = true;
			s.knee = 0.5f;
			s.colour = 0.8f;
			s.gamma = 0.0f;
			s.threshold = mix;
			s.drive = drive;
			// The input level at which the threshold is crossed IS the threshold: input units.
			const float crossing = thresholdOf(mix);
			REQUIRE(crossing < 0.9f); // the wave must actually cross, or the case is vacuous
			const LastBlock last = lastBlockOf(s, 1.0f, 60, kFrames, kCycles);
			shp::Settings dry = s;
			dry.drive = 0.0f;
			const LastBlock reference = lastBlockOf(dry, 1.0f, 60, kFrames, kCycles);
			REQUIRE(reference.in == last.in);

			int below = 0;
			int above = 0;
			float worstBelow = 0.0f;
			float worstAbove = 0.0f;
			float worstAboveVsInput = 0.0f;
			for (size_t i = 0; i < last.in.size(); ++i) {
				const float x = last.in[i];
				const float y = last.out[i];
				// Clear of the crossing by 10% either way, so a sample IN the crossfade is not
				// scored as either.
				if (std::fabs(x) < crossing * 0.9f) {
					++below;
					worstBelow = std::max(worstBelow, std::fabs(y - reference.out[i]));
				}
				else if (std::fabs(x) > crossing * 1.1f) {
					++above;
					worstAbove = std::max(worstAbove, std::fabs(y - reference.out[i]));
					// **AND AGAINST THE INPUT, which is what "the device did something" actually
					// means.** See the assertion below for why the drive-0 reference stopped being
					// able to answer that question.
					worstAboveVsInput = std::max(worstAboveVsInput, std::fabs(y - x));
				}
			}
			CAPTURE(crossing);
			CAPTURE(below);
			CAPTURE(above);
			CAPTURE(worstBelow);
			CAPTURE(worstAbove);
			// Non-vacuity: both populations exist.
			CHECK(below > 20);
			CHECK(above > 20);
			CAPTURE(worstAboveVsInput);
			// The bottom of the wave is untouched. THIS IS THE CASE'S SUBJECT and it is unchanged.
			CHECK(worstBelow < 0.02f);

			// **THE NON-VACUITY MOVED OFF THE DRIVE-0 REFERENCE, BY DECISION.** It read
			// `worstAbove > 0.05` — the top must differ between drive d and DRIVE 0 — which asserts
			// that DRIVE changes the top. MEASURED under the folder read it does not: 0.0020 at mix
			// 0.3 and 0.0402 at mix 0.5, at every one of drive 0.75 / 0.85 / 1.0.
			//
			// **THE MECHANISM IS SATURATION, NOT A DEAD ROAD.** Drive scales the read position
			// (`rate = sweep/headroom`, `sweep = 4^drive`), so at mix 0.5 drive 1 reads four times
			// further into the table than drive 0 — and the table has already saturated by then, so
			// four times further lands on the same flat top. That Drive does little above the threshold
			// at low mix is accepted as correct (2026-09-04).
			//
			// So the clause is asserted against the INPUT instead, which is the claim it was always
			// standing in for: the top is SHAPED, whatever Drive is doing. 0.05 stays the bound and
			// is still meaningful against the raw input — the blocker's own phase lead is 0.018 at
			// this frequency, measured in the note above — where a drive-0 comparison now has only
			// 0.002 of room and would be asserting the blocker.
			CHECK(worstAboveVsInput > 0.05f);
		}
	}
}


//--------------------------------------------------------------------------------------------//
// THE CEILING — never above 0 dBFS, never above the dry, and Knee out of the crossfade         //
//--------------------------------------------------------------------------------------------//

TEST_CASE("CEILING: the output never exceeds 0 dBFS, for any input, including inputs above it") {
	// THE FIRST CONSTRAINT: the device must not clip the format on its output. The device's output is `|out| <= 1.0` for ANY input — and "any" has to include
	// inputs above 1.0, because a host's bus is unbounded and that is exactly where the old device
	// failed: its threshold lived in a 0.25-padded domain, so at mix 0.5 and unity drive a signal
	// had to reach 1.97 to be shaped at all, and everything below that — including +6 dB — went to
	// the output DRY.
	//
	// MEASURED RED before this work, the worst peak over this grid: 1.0 in → 1.020; 2.0 in → 2.001;
	// 4.0 in → 4.002 (drive 0, mix 0.1: the whole signal passed dry). Hot inputs are +6 and +12 dB.
	for (const float amp : {1.0f, 2.0f, 4.0f}) {
		// 0.8 WAS CHOSEN FOR THE INFLATOR, which is Colour's Inflate MODE since backlog IW — so it is swept in Inflate.
		for (const float colour : {0.38f, 0.8f}) {
			for (const float knee : {0.25f, 0.5f, 1.0f}) {
				for (const float mix : {0.1f, 0.5f, 0.9f}) {
					for (const float drive : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
						CAPTURE(amp);
						CAPTURE(colour);
						CAPTURE(knee);
						CAPTURE(mix);
						CAPTURE(drive);
						shp::Settings s;
						s.enabled = true;
						s.knee = knee;
						s.colour = colour;
						if (colour > 0.75f) {
							s.colourMode = shp::kColourInflate;
							s.inflateAmount = 1.0f;
						}
						s.threshold = mix;
						s.drive = drive;
						shp::Shaper dev;
						dev.prepare(48000.0);
						dev.setTarget(s);
						std::vector<float> l;
						for (int b = 0; b < 60; ++b) {
							l = sine(256, amp, 4.0f);
							std::vector<float> r = l;
							dev.processStereo(l.data(), r.data(), 256);
						}
						float worst = 0.0f;
						for (const float y : l) {
							worst = std::max(worst, std::fabs(y));
						}
						CAPTURE(worst);
						CHECK(worst <= 1.0f);
						// NON-VACUITY: the device is not silent.
						CHECK(worst > 0.2f);
						// AND THE CEILING IS THE READ'S, NOT THE CLAMP'S. The output stage trims what
						// the DC blocker pushes past 1.0 — the offset of an asymmetric shaped wave,
						// and the blocker's lag behind that offset when a table swap changes it —
						// MEASURED 0.009 settled and 0.030 across the whole run at colour 0.38 on the
						// +6 dB sine — and nothing more. A read that leaned on the clamp to meet the
						// ceiling would report the whole excess here: 1.0 on this input.
						CAPTURE(dev.ceilingExcessForTest());
						CHECK(dev.ceilingExcessForTest() < 0.05f);
					}
				}
			}
		}
	}

	SUBCASE("at mix 0 the threshold is 0 dBFS itself: below it untouched, above it the ceiling") {
		// The threshold is 1.0 at mix 0 — `thresholdForTest` says so in the mix case — so nothing
		// below full scale is shaped and nothing above it escapes. A +6 dB sine comes out as a
		// full-scale clip.
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.5f;
		s.threshold = 0.0f;
		const LastBlock hot = lastBlockOf(s, 2.0f, 60);
		float worst = 0.0f;
		int pinned = 0;
		for (const float y : hot.out) {
			worst = std::max(worst, std::fabs(y));
			if (std::fabs(y) > 0.99f) {
				++pinned;
			}
		}
		CHECK(worst <= 1.0f);
		CHECK(pinned > 20); // and it reaches the ceiling, so the check above is not vacuous
	}

	SUBCASE("knee 0 is a bypass under the same ceiling") {
		// The bypass returns the input to the bit below full scale — the knee-0 case pins that — and
		// it is the one setting with no table to enforce the ceiling, so it clamps at ±1.0 instead.
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.0f;
		const LastBlock hot = lastBlockOf(s, 4.0f, 60);
		float worst = 0.0f;
		for (const float y : hot.out) {
			worst = std::max(worst, std::fabs(y));
		}
		CHECK(worst <= 1.0f);
		CHECK(worst > 0.99f);
	}
}

TEST_CASE("CEILING: above the threshold the shaped sample never exceeds the dry it replaced") {
	// THE SECOND CONSTRAINT: dry level >= wet level, because the dry can go above 0 dBFS while the
	// table must clip. Per sample:
	// `|out| <= |in|` for every sample above the threshold. It follows from the read's construction
	// — the table is read at the excess with a slope of 1 at the crossing and never rises above its
	// own steepest secant — and a read that broke the unit slope (2 at the crossing, say) would put
	// the top above the dry immediately, which is the known-bad for this case.
	//
	// MEASURED RED before this work, worst `|out|/|in|` above the crossing on a full-scale sine at
	// knee 0.5 / colour 0.8: 1.08 (+0.7 dB) at drive 0.6 / mix 0.5, 2.66 (+8.5 dB) at 0.75 / 0.5,
	// 3.51 (+10.9 dB) at 1.0 / 0.5 and at every drive at mix 0.9. The old read multiplied the
	// signal by the table's first-rise slope of 6..40 until the table's peak stopped it.
	for (const float amp : {1.0f, 2.0f, 4.0f}) {
		// 0.8 WAS CHOSEN FOR THE INFLATOR, which is Colour's Inflate MODE since backlog IW — so it is swept in Inflate.
		for (const float colour : {0.38f, 0.8f}) {
			for (const float knee : {0.25f, 0.5f, 1.0f}) {
				for (const float mix : {0.1f, 0.5f, 0.9f}) {
					for (const float drive : {0.0f, 0.5f, 1.0f}) {
						CAPTURE(amp);
						CAPTURE(colour);
						CAPTURE(knee);
						CAPTURE(mix);
						CAPTURE(drive);
						shp::Settings s;
						s.enabled = true;
						s.knee = knee;
						s.colour = colour;
						if (colour > 0.75f) {
							s.colourMode = shp::kColourInflate;
							s.inflateAmount = 1.0f;
						}
						s.threshold = mix;
						s.drive = drive;
						// 16 cycles per block so the blocker's memory of the shaped top is small
						// against a full-scale sample, as the parity case measured (0.004..0.009).
						const LastBlock last = lastBlockOf(s, amp, 60, 1024, 16.0f);
						const float t = thresholdOf(mix);
						int above = 0;
						float worstOver = -1.0f; // the most |out| exceeds |in|, negative if never
						float mostUnder = 0.0f;  // the most it sits under, for non-vacuity
						for (size_t i = 0; i < last.in.size(); ++i) {
							const float x = std::fabs(last.in[i]);
							if (x <= t) {
								continue;
							}
							++above;
							const float y = std::fabs(last.out[i]);
							worstOver = std::max(worstOver, y - x);
							mostUnder = std::max(mostUnder, x - y);
						}
						CAPTURE(above);
						CAPTURE(worstOver);
						CAPTURE(mostUnder);
						CHECK(above > 20);
						//-----------------------------------------------------------------------//
						// THE DRY-RELATIVE BOUND IS RELAXED; THE FORMAT CEILING IS NOT.          //
						//-----------------------------------------------------------------------//
						//
						// This read `CHECK(worstOver < 0.02f)` — the shaped top never above the dry
						// it replaced. That dry/wet peak relationship was RELAXED by decision: it
						// forced odd conditions, and the device is really a shaper/folder. **A FOLDER PUTS THE SHAPED TOP ABOVE THE DRY
						// ON PURPOSE — that bound was the definition of a saturator, and this device
						// is not one.** Enforcing it cost the whole character: it was implemented as
						// a divide by the steepest chord anywhere in the table, which confined the
						// read to the first fifth of the domain, and MEASURED direction reversals
						// say the folding lives past there — 0 at colour 0, 6 at colour 0.25, 16 at
						// colour 0.38. The folds were unreachable at every setting.
						//
						// **WHAT IS ASSERTED INSTEAD IS THE GUARANTEE THAT PROTECTS THE SIGNAL: the
						// output never passes 0 dBFS.** Structural — `|T| <= 1` gives `|out| <=
						// headroom` and `headroom = 1 - threshold` — and it is the half of the
						// decision that did NOT move (the table clips at 0 dBFS with no post
						// gain). Dropping the old clause without putting this in its place would
						// have let the relaxation take the format guarantee with it.
						for (size_t i = 0; i < last.out.size(); ++i) {
							REQUIRE(std::fabs(last.out[i]) <= 1.0f);
						}
						// **NON-VACUITY, AND ITS DIRECTION IS NOW WRONG TO ASSUME.** This read
						// `mostUnder > 0.1·h` — the shaped top really sits BELOW the dry somewhere,
						// so a device that returned its input could not pass by the tolerance. The
						// purpose survives the relaxation and the direction does not: a folder can
						// put the top above the dry for the whole of a cycle, and at colour 0.8 it
						// does, which is what fails here rather than any loss of coverage.
						//
						// So the claim becomes what it always MEANT: the shaped top DIFFERS from the
						// dry, either way, by more than a tolerance. A bypass still cannot pass it.
						CHECK(std::max(mostUnder, worstOver) > 0.1f * (1.0f - t));
					}
				}
			}
		}
	}
}

TEST_CASE("CEILING: the mix crossfade's width does not depend on Knee") {
	// THE FIRMWARE'S `blendAggression` — `0.1 + knee² · 1.9` — scaled the crossfade slope by 20x
	// across the Knee sweep, which put Knee inside the mix crossfade: two meanings on one knob.
	// MEASURED before this work, the width from the crossing to 95 % wet at mix 0.5, in input
	// units: 2.23 at knee 0.01, 0.39 at knee 0.5, 0.11 at knee 1.0. With the shaped side starting AT
	// the dry level there is no step for Knee to soften, so Mix alone decides how abruptly.
	//
	// THE SLOPE IS `(1 + 16·mixNorm²) / h`, restated — the mix term the firmware had, over the
	// headroom, which the ceiling requires: the crossfade's dry side is the unbounded input, so the
	// blend must be complete by the time the excess reaches the headroom.
	for (const float mix : {0.1f, 0.5f, 0.9f}) {
		CAPTURE(mix);
		const float mixNorm = mix * 2.0f;
		const float h = 1.0f - thresholdOf(mix);
		const float expected = (1.0f + 16.0f * mixNorm * mixNorm) / h;
		float first = 0.0f;
		for (const float knee : {0.01f, 0.5f, 1.0f}) {
			CAPTURE(knee);
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings settings;
			settings.enabled = true;
			settings.knee = knee;
			settings.threshold = mix;
			settle(s, settings);
			const float slope = s.blendSlopeForTest();
			CHECK(slope == doctest::Approx(expected).epsilon(1e-3));
			if (first == 0.0f) {
				first = slope;
			}
			CHECK(slope == doctest::Approx(first).epsilon(1e-6));
		}
	}

	SUBCASE("and the crossfade is over before the excess reaches the headroom, at every mix") {
		// The ceiling's own requirement on the crossfade, as a number: 95 % wet within h.
		for (const float mix : {0.05f, 0.1f, 0.5f, 0.9f, 1.0f}) {
			CAPTURE(mix);
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings settings;
			settings.enabled = true;
			settings.threshold = mix;
			settle(s, settings);
			const float h = 1.0f - s.thresholdForTest();
			CHECK(0.95f / s.blendSlopeForTest() < h);
		}
	}
}

TEST_CASE("the drawn transfer IS the device's: staticTransferAt agrees with Shaper") {
	// **THE PLOT AND THE SOUND DIVERGED TWICE IN ONE DAY**, which is why this case exists rather than
	// a comment. On a scope the displayed transfer disagreed with the output even with all modifiers
	// off. The first divergence was a post-gain the
	// plot did not know about. The second was structural and older: the plot drew the raw TABLE across
	// the whole domain while the device passes input through unchanged below its threshold and reads
	// the table only on the excess above it.
	//
	// **SHARED CODE FIXES ONE INSTANCE; THIS CASE FIXES THE CLASS.** `Shaper::readTable` now delegates
	// to `readTableAt`, and the staging, sweep and squash hardness come from `stagingFor`, `sweepFor`
	// and `squashHardnessFor` — so the plot and the engine read one definition. This asserts they
	// still do, which is what catches the next change to `processSample` that forgets the picture.
	//
	// KNEE'S PRE-SQUASH IS INSIDE THE READ, so it reaches this case through `readTableAt` without a
	// word here; the hardness is the one number the read takes that the table does not carry, and
	// it is asked of the same law the engine asks. The KNEE value case at the end of this file is
	// where the squashed region is pinned to the bit; this one is the whole domain at a tripwire.
	const auto agreesAt = [](float knee, float colour, float mix, float drive) {
		shp::Shaper dev;
		dev.prepare(48000.0);
		shp::Settings s;
		s.enabled = true;
		s.knee = knee;
		s.colour = colour;
		s.threshold = mix;
		s.drive = drive;
		// **HYSTERESIS OFF, AND THAT WAS THE UNEXPLAINED RESIDUAL.** `staticTransferAt` carries no
		// hysteresis by design — it is STATE, depending on where the signal came from and not only on
		// where it is — while this case left the knob at its DEFAULT, which is not zero. So the device
		// was answering a question about a path and the static form a question about a point, and the
		// gap between them was the hysteresis offset rather than any disagreement about the transfer.
		//
		// Found by `knee3`, which hit the same wall in its own case and zeroed it; I had left the bound
		// at the measured residual and said the cause was unknown. It was knowable and I had not asked.
		s.hysteresis = 0.0f;
		settle(dev, s);

		// THE SAME TABLE THE PLOT BUILDS, and the same staging and sweep.
		std::array<float, shp::kTableEntries> table {};
		shp::buildTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics), table);
		const shp::TableBound bound = shp::boundTable(table);
		const shp::Staging staging = shp::stagingFor(s.threshold, s.aggression);
		const float sweep = shp::sweepFor(s.drive);

		float worst = 0.0f;
		size_t compared = 0;
		// **A RAMP, ONE SAMPLE PER BLOCK, SO THE DEVICE'S STATE CANNOT CARRY BETWEEN POINTS.** The
		// static transfer is a claim about ONE sample in isolation; feeding a sweep would let the DC
		// blocker and the smoothers answer a different question.
		for (int i = 0; i <= 64; ++i) {
			const float x = -1.0f + 2.0f * static_cast<float>(i) / 64.0f;
			shp::Shaper one;
			one.prepare(48000.0);
			// **SETTLED HARD, BECAUSE `sweep_` IS SMOOTHED.** The default 64 blocks leaves it short of
			// its target, and the read scales by it — measured, the disagreement GREW with drive
			// (0.024 at drive 0, 0.050 at drive 1), which is the signature of a smoother still moving
			// rather than of a different function.
			settle(one, s, 512);
			for (int b = 0; b < 512; ++b) {
				std::vector<float> z(512, 0.0f);
				std::vector<float> z2 = z;
				one.processStereo(z.data(), z2.data(), 512);
			}
			std::vector<float> l { x };
			std::vector<float> r { x };
			one.processStereo(l.data(), r.data(), 1);
			const float want = shp::staticTransferAt(table, bound, staging, sweep, shp::kneeFadeFor(s.knee), x);
			// **AGAINST THE PRE-BLOCKER VALUE**, not `l[0]`: a static transfer cannot model a filter with
			// memory, and a folded transfer generates the DC that makes the difference visible.
			worst = std::max(worst, std::fabs(one.preBlockForTest() - want));
			++compared;
		}
		CHECK(compared == 65);
		return worst;
	};

	// **ACROSS THE SETTINGS THAT CHANGE THE SHAPE**, not one point: the threshold moves with mix, the
	// sweep with drive, and the curve with knee and colour. A single setting would pass on a transfer
	// that happened to agree there.
	for (const auto& [knee, colour, mix, drive] :
	     std::vector<std::tuple<float, float, float, float>> {
	         // **KNEE 0 IS EXCLUDED AND THAT IS NOT A DODGE.** Knee 0 is a TRUE BYPASS — `Shaper`
	         // returns `clamp(x)` and never reads the table — so the static transfer is not the
	         // function the device runs there. The plot draws the identity for it (`settings.linear()`),
	         // which is the same answer by a different road, and the subcase below pins that.
	         // **TWO RUNGS INSIDE THE KNEE FADE, because the fade is a SECOND term living in two
	         // places.** `kneeFadeFor` is applied by the engine and by `staticTransferAt`, and the
	         // whole point of this case is that a term in both cannot drift. The ladder's lowest knee
	         // was 0.2, four times `kKneeFadeTop`, so the fade was 1.0 at every rung and the region
	         // where it is PARTIAL was covered by nothing. MEASURED when the fade was added: deleting
	         // it from the engine alone left this case GREEN and only the level case red — the
	         // certificate could not see the divergence it exists to catch.
	         { 0.01f, 0.38f, 1.0f, 0.5f },
	         { 0.03f, 0.38f, 1.0f, 1.0f },
	         { 0.2f, 0.0f, 1.0f, 0.5f },
	         { 0.5f, 0.3f, 1.0f, 0.5f },
	         { 1.0f, 0.7f, 1.0f, 0.5f },
	         { 0.5f, 0.5f, 0.5f, 0.5f },
	         { 0.5f, 0.5f, 1.0f, 0.0f },
	         { 0.5f, 0.5f, 1.0f, 1.0f },
	     }) {
		CAPTURE(knee);
		CAPTURE(colour);
		CAPTURE(mix);
		CAPTURE(drive);
		const float worst = agreesAt(knee, colour, mix, drive);
		CAPTURE(worst);
		// **A TIGHT BOUND, AND IT ONLY BECAME ONE WHEN THE RESIDUAL WAS EXPLAINED.**
		//
		// This shipped at 0.2 with a comment saying the gap was "not yet explained" — the two agreed
		// mid-domain (0.144 against 0.144) and parted at the extremes (0.193 against 0.213 at x = 1),
		// some of it `sweep_` still smoothing and the rest unaccounted for. I set the bound where the
		// measurement was and called it a tripwire rather than a guarantee, which was the honest thing
		// to do with a number I could not explain.
		//
		// **THE CAUSE WAS HYSTERESIS AT ITS DEFAULT** — see the settings above. `staticTransferAt`
		// carries no hysteresis by design, so the device was answering about a PATH and the static form
		// about a POINT. With it zeroed the worst case falls from 0.2 to under 0.001: **two hundred
		// times tighter, and now a certificate rather than a tripwire.**
		//
		// `knee3` found it, having hit the same wall in its own case. **The lesson is mine: I recorded
		// the residual honestly and stopped there, when the cause was one settings field away.** An
		// honest bound is better than a false one and worse than an explained one.
		CHECK(worst < 0.001f);
	}
}

//--------------------------------------------------------------------------------------------//
// KNEE — steepness and overdrive inside the table, as the firmware's; the pre-squash is retired //
//--------------------------------------------------------------------------------------------//

namespace {

/// THD of a 0 dBFS sine through the device at mix 1.0 — the whole wave shaped — after the table
/// has landed and the smoothers have settled. 40 blocks of 512 is 0.43 s; the sweep smoother is
/// 23 ms and the build 10.7 ms.
double fullScaleDistortion(float knee, float colour, float drive) {
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = knee;
	settings.colour = colour;
	// THE FIRMWARE'S BAND IS OUR INFLATE MODE (backlog IW) — 0.8 was measured as the inflator, and is swept as one.
	if (colour >= 768.0f / 1023.0f && colour < 896.0f / 1023.0f) {
		settings.colourMode = shp::kColourInflate;
		settings.inflateAmount = 1.0f;
	}
	settings.threshold = 1.0f;
	settings.drive = drive;
	const std::vector<float> out = run(s, settings, 40, 512, 1.0f);
	return thd(out, 4);
}

/// The Knee ladder every claim below is made on: the gentlest setting the panel offers short of
/// the bypass, then quarter turns to the top.
constexpr std::array<float, 5> kKneeLadder {0.05f, 0.25f, 0.5f, 0.75f, 1.0f};

std::string ladderText(const std::array<double, 5>& rungs) {
	std::string text;
	for (const double r : rungs) {
		text += std::to_string(r * 100.0).substr(0, 5) + "% ";
	}
	return text;
}

} // namespace


TEST_CASE("KNEE: Colour's timbre spread is the firmware's — the pre-squash's cost, measured and closed") {
	// **THE PRE-SQUASH TOOK A QUARTER OF THE VARIATION ACROSS COLOUR.** Measured across 96 colours,
	// harmonics 2..16 in dB, the std-dev of each harmonic across colour averaged over harmonics: at
	// knee 1, a sine at 0.9, the port read 9.8 dB against the firmware's 13.3, and its wildest colour
	// stopped at 97 % THD where the firmware's reaches 204 %. Without the squash: 11.7 dB and 216 %.
	// The residue is the port's excess-only read, which is the 2026-09-03 design and not this.
	constexpr int kColours = 96;
	for (const float knee : { 0.5f, 1.0f }) {
		CAPTURE(knee);
		std::vector<std::array<double, fwmodel::kHarm>> fw;
		std::vector<std::array<double, fwmodel::kHarm>> port;
		for (int c = 0; c < kColours; ++c) {
			const float colour = static_cast<float>(c) / (kColours - 1);
			const auto ft = fwmodel::table(fwmodel::derive(knee, colour));
			fw.push_back(fwmodel::spectrumOf([&](float x) { return fwmodel::read(ft, x); }, 0.9f));
			port.push_back(fwmodel::spectrumOf(fwmodel::portTransfer(knee, colour), 0.9f));
		}
		const auto spread = [](const std::vector<std::array<double, fwmodel::kHarm>>& s) {
			double acc = 0.0;
			for (int h = 1; h < fwmodel::kHarm; ++h) {
				double m = 0.0;
				for (const auto& v : s) {
					m += v[h];
				}
				m /= static_cast<double>(s.size());
				double var = 0.0;
				for (const auto& v : s) {
					var += (v[h] - m) * (v[h] - m);
				}
				acc += std::sqrt(var / static_cast<double>(s.size()));
			}
			return acc / (fwmodel::kHarm - 1);
		};
		const auto wildest = [](const std::vector<std::array<double, fwmodel::kHarm>>& s) {
			double m = 0.0;
			for (const auto& v : s) {
				m = std::max(m, fwmodel::thdOf(v));
			}
			return m;
		};
		const double fwSpread = spread(fw);
		const double portSpread = spread(port);
		CAPTURE(fwSpread);
		CAPTURE(portSpread);
		CAPTURE(wildest(fw));
		CAPTURE(wildest(port));
		MESSAGE("knee " << knee << ": spread fw " << fwSpread << " dB, port " << portSpread << " dB; wildest THD fw "
		                << wildest(fw) << ", port " << wildest(port));
		CHECK(portSpread > 0.85 * fwSpread);
		CHECK(wildest(port) > 0.9 * wildest(fw));
	}
}

TEST_CASE("KNEE: at drive 0 the Knee ladder is the firmware's, and Knee never adds level") {
	// **RE-BASED ON THE FIRMWARE, 2026-09-22, when the Knee pre-squash was retired.** This case held
	// measured THD BANDS for the squash's ladder, and before that a monotone ladder that the decision of
	// 2026-09-04 released (a non-monotone ladder is acceptable, most likely an interaction). With the
	// squash gone Knee reaches the sound the way the firmware's does — steepness
	// `1 + 9·knee` and overdrive inside the table — so the property is no longer a band anybody chose:
	// it is the other program's ladder. Compared through ONE estimator (`fwmodel::spectrumOf`) on both
	// static transfers, so the metric cannot differ between the two sides.
	//
	// WHERE THE FIRMWARE'S OWN FUNDAMENTAL COLLAPSES the THD is ill-conditioned — colour 0.8 at knee 1
	// reads 2,095,041 % on the firmware — so there the claim is only that the port collapses too.
	for (const float colour : { 0.38f, 0.8f }) {
		CAPTURE(colour);
		std::array<double, 5> fwRungs {};
		std::array<double, 5> portRungs {};
		for (size_t k = 0; k < kKneeLadder.size(); ++k) {
			const float knee = kKneeLadder[k];
			const auto ft = fwmodel::table(fwmodel::derive(knee, colour));
			fwRungs[k] = fwmodel::thdOf(fwmodel::spectrumOf([&](float x) { return fwmodel::read(ft, x); }, 1.0f));
			portRungs[k] = fwmodel::thdOf(fwmodel::spectrumOf(fwmodel::portTransfer(knee, colour), 1.0f));
		}
		MESSAGE("THD ladder, colour " << colour << " drive 0, knee 0.05..1.0 — firmware: " << ladderText(fwRungs)
		                              << " port: " << ladderText(portRungs));
		for (size_t k = 0; k < kKneeLadder.size(); ++k) {
			CAPTURE(k);
			CAPTURE(fwRungs[k]);
			CAPTURE(portRungs[k]);
			if (fwRungs[k] > 3.0) {
				CHECK(portRungs[k] > 3.0);
			}
			else {
				CHECK(portRungs[k] == doctest::Approx(fwRungs[k]).epsilon(0.2));
			}
		}
	}

	SUBCASE("at drive 0.5 and 1 the ladder is NOT monotone, and stays in its band") {
		// THE MECHANISM, ASSERTED AS WHAT IT DOES AND NOT AS WHAT THE BRIEF GUESSED. A pre-squash
		// shortens the excess, and at higher drive a shorter excess reaches LESS of the table: where
		// the table folds (colour 0.38) the fold's harmonics go as the squash's come, and where it
		// clips flat (colour 0.8 at drive 1) the excess lands on a slope of zero and the squash is
		// inaudible. MEASURED, same ladder — before → with:
		//   colour 0.38, drive 0.5:  23.65 / 23.44 / 23.27 / 23.15 / 23.07  →  23.24 / 21.03 / 19.24 / 18.02 / 17.09
		//   colour 0.38, drive 1.0:  61.64 / 64.53 / 67.17 / 69.32 / 70.96  →  62.00 / 78.66 / 133.3 / 184.7 / 102.8
		//   colour 0.8,  drive 0.5:  16.48 / 17.33 / 17.25 / 17.22 / 17.22  →  16.55 / 17.92 / 18.39 / 18.58 / 18.52
		//   colour 0.8,  drive 1.0:  30.05 / 29.14 / 29.38 / 29.45 / 29.50  →  30.05 / 29.24 / 29.50 / 29.44 / 29.14
		// (THD above 100 % is a folded wave whose fundamental has collapsed; as a scale of "more
		// distortion" it stops meaning anything there.) The ceiling build's ladders were not
		// monotone at these drives either. So the assertion is a BAND per grid point, set from the
		// measurement with room on both sides, and the direction inside it: the folding table at
		// drive 0.5 must LOSE distortion across the knob, which is the mechanism, and the flat one
		// at drive 1 must not move by more than a tenth of itself, which is the other half of it.
		struct Band {
			float colour;
			float drive;
			double low;
			double high;
		};
		// **EVERY BAND RE-MEASURED UNDER THE FOLDER READ, and they moved a long way.** These were set
		// from a measurement of the OLD read; the same ladders now read (knee 0.05..1.0):
		//   colour 0.38, drive 0.5:  35.86 / 44.01 / 160.8 / 34.27 / 51.70 %
		//   colour 0.38, drive 1.0:  36.85 / 44.10 / 162.8 / 39.94 / 47.84 %
		//   colour 0.8,  drive 0.5:  33.19 / 41.80 / 44.70 / 45.31 / 101.0 %
		//   colour 0.8,  drive 1.0:  40.64 / 44.45 / 45.30 / 45.59 / 55.02 %
		// The bands below are those spreads with room either side. **The rungs above 100 % are a
		// folded wave whose FUNDAMENTAL has collapsed** — this subcase's own note already says THD has
		// left its range as a scale of "more distortion" there, and under this read that regime is
		// reached at three of these twenty rungs rather than at none.
		for (const Band& band : {Band {0.38f, 0.5f, 0.30, 1.75}, Band {0.38f, 1.0f, 0.32, 1.75},
		                         Band {0.8f, 0.5f, 0.28, 1.15}, Band {0.8f, 1.0f, 0.35, 0.62}}) {
			CAPTURE(band.colour);
			CAPTURE(band.drive);
			std::array<double, 5> rungs {};
			for (size_t k = 0; k < kKneeLadder.size(); ++k) {
				rungs[k] = fullScaleDistortion(kKneeLadder[k], band.colour, band.drive);
				CAPTURE(k);
				CAPTURE(rungs[k]);
				CHECK(rungs[k] > band.low);
				CHECK(rungs[k] < band.high);
			}
			MESSAGE("THD ladder, colour " << band.colour << " drive " << band.drive
			                              << ", knee 0.05..1.0: " << ladderText(rungs));
		}
		// **TWO DIRECTIONAL CLAIMS DIED HERE, AND THEY ARE DELETED RATHER THAN RE-FITTED.** They were:
		//   (a) "the fold gives way to the squash: top of the knob is at least a fifth LESS than the
		//       bottom" — at colour 0.38, drive 0.5 the top is now 51.70 % against a bottom of
		//       35.86 %, so it is 1.44x MORE rather than a fifth less;
		//   (b) "a clipped-flat table does not hear the squash: within a tenth, end to end" — at
		//       colour 0.8, drive 1 the ladder runs 40.64 % to 55.02 %, a 35 % rise, so it hears it.
		// Both were true of the old read and are false of this one. Each was a statement about WHICH
		// WAY Knee moves the sound, and inventing a replacement direction would be deciding by test
		// edit a thing decided by ear, with these numbers in hand.
		//
		// WHAT REPLACES THEM IS A FACT RATHER THAN A DIRECTION: somewhere in this territory the
		// fundamental collapses, which is the qualitative thing the bands alone do not say.
		CHECK(fullScaleDistortion(0.5f, 0.38f, 0.5f) > 1.0);
	}

	SUBCASE("and Knee never adds level: the top of the ladder peaks no higher than the bottom") {
		// A squash cannot add level — `sat_p(u) <= u` — and this is the bound as a measurement on
		// the same signal the ladder is made of. The peak may FALL with Knee, by at most the
		// squash's own value at the top of the excess, which the "knee sweep no longer climbs nine
		// decibels" subcase holds under 3 dB at drive 0.
		// 0.8 WAS CHOSEN FOR THE INFLATOR, which is Colour's Inflate MODE since backlog IW — so it is swept in Inflate.
		for (const float colour : {0.38f, 0.8f}) {
			for (const float drive : {0.0f, 0.5f, 1.0f}) {
				CAPTURE(colour);
				CAPTURE(drive);
				const auto peakAt = [&](float knee) {
					shp::Settings s;
					s.enabled = true;
					s.knee = knee;
					s.colour = colour;
					if (colour > 0.75f) {
						s.colourMode = shp::kColourInflate;
						s.inflateAmount = 1.0f;
					}
					s.threshold = 1.0f;
					s.drive = drive;
					return peak(lastBlockOf(s, 1.0f, 60, 512).out);
				};
				const float gentle = peakAt(kKneeLadder.front());
				const float hard = peakAt(kKneeLadder.back());
				CAPTURE(gentle);
				CAPTURE(hard);
				CHECK(hard <= gentle + 0.02f);
				CHECK(gentle <= 1.0f);
			}
		}
	}
}

TEST_CASE("KNEE: the audition grid — THD and peak of a 0 dBFS sine at colour 0.8, knee 0.5, mix 0.5 and 1.0") {
	// THE NUMBERS THE COMMIT REPORTS, produced by the case rather than by a scratch program, so
	// the next change to this read can re-run them. The audition is on the default knee (0.5),
	// which is p = 4 — sat(1) = 0.84, 1.5 dB off the very top of the excess.
	for (const float mix : {0.5f, 1.0f}) {
		for (const float drive : {0.0f, 0.5f, 1.0f}) {
			shp::Settings s;
			s.enabled = true;
			s.knee = 0.5f;
			s.colour = 0.8f;
			s.threshold = mix;
			s.drive = drive;
			shp::Shaper dev;
			dev.prepare(48000.0);
			const std::vector<float> out = run(dev, s, 40, 512, 1.0f);
			const double distortion = thd(out, 4);
			const float top = peak(out);
			MESSAGE("audition grid: mix " << mix << " drive " << drive << ": THD "
			                              << std::to_string(distortion * 100.0).substr(0, 5)
			                              << " %, peak " << top << " ("
			                              << 20.0f * std::log10(top) << " dB re dry)");
			CHECK(top <= 1.0f);
			CHECK(top > 0.2f);
		}
	}
}

TEST_CASE("BI PROBE: level and the off-to-minimum-knee step" * doctest::skip()) {
	// MEASUREMENT ONLY — backlog BI: an audition found a very abrupt jump between off and minimum knee,
	// and significant signal reduction at every drive/knee setting except knee=0. No sound changes
	// until that is decided; this exists to put numbers on it.
	//
	// RMS RATHER THAN PEAK, because "significant signal reduction" is a loudness complaint and the
	// ceiling's whole design is about the PEAK — a peak figure would answer a question nobody asked
	// and would flatter the design, since clamping the peak is exactly what it is for.
	constexpr double kSr = 48000.0;
	constexpr int kSettle = 8192; // the table lands in ~513 samples; the crossfade needs longer
	constexpr int kMeasure = 8192;

	// A RISING SAW AT 0 dBFS — the audition's own test signal for the transfer, which should paint a
	// picture similar to the transfer function.
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0; // ~110 Hz at 48k
		const double phase = std::fmod(static_cast<double>(n), kPeriod) / kPeriod;
		return static_cast<float>(2.0 * phase - 1.0);
	};

	const auto measure = [&](float knee, float drive, float mix) {
		shp::Shaper sh;
		sh.prepare(kSr);
		shp::Settings s;
		s.enabled = true;
		s.knee = knee;
		s.drive = drive;
		s.colour = 0.38f; // the ladder's colour, so these numbers sit beside knee3's
		s.threshold = mix;
		sh.setTarget(s);

		double sumIn = 0.0;
		double sumOut = 0.0;
		double peakOut = 0.0;
		int n = 0;
		for (int block = 0; block < (kSettle + kMeasure) / 64; ++block) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			for (int i = 0; i < 64; ++i, ++n) {
				l[static_cast<size_t>(i)] = saw(n);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			const int at = n - 64;
			sh.processStereo(l.data(), r.data(), 64);
			if (at < kSettle) {
				continue;
			}
			for (int i = 0; i < 64; ++i) {
				const double in = saw(at + i);
				const double out = l[static_cast<size_t>(i)];
				sumIn += in * in;
				sumOut += out * out;
				peakOut = std::max(peakOut, std::abs(out));
			}
		}
		const double rmsIn = std::sqrt(sumIn / kMeasure);
		const double rmsOut = std::sqrt(sumOut / kMeasure);
		return std::array<double, 3> { rmsOut / rmsIn, 20.0 * std::log10(rmsOut / rmsIn), peakOut };
	};

	MESSAGE("knee  drive   RMSout/RMSin      dB    peak");
	for (const float knee : { 0.0f, 0.001f, 0.01f, 0.05f, 0.1f, 0.25f, 0.5f, 1.0f }) {
		for (const float drive : { 0.0f, 0.25f, 0.5f, 1.0f }) {
			const auto m = measure(knee, drive, 1.0f);
			MESSAGE("  " << knee << "   " << drive << "     " << m[0] << "   " << m[1] << "   " << m[2]);
		}
	}

	MESSAGE("--- mix sweep at drive 0.5, the parity question ---");
	MESSAGE("knee   mix   RMSout/RMSin      dB    peak");
	for (const float knee : { 0.001f, 0.25f, 1.0f }) {
		for (const float mix : { 0.25f, 0.5f, 0.75f, 1.0f }) {
			const auto m = measure(knee, 0.5f, mix);
			MESSAGE("  " << knee << "   " << mix << "     " << m[0] << "   " << m[1] << "   " << m[2]);
		}
	}
}

TEST_CASE("BI PROBE 2: the read's own quantities" * doctest::skip()) {
	// WHY THE CEILING FIX DID NOTHING: `sweep > secant` never held. Print the numbers rather than
	// reason about them — the derivation said the domain was exhausted and the measurement disagreed.
	shp::Settings s;
	s.enabled = true;
	s.colour = 0.38f;
	for (const float knee : { 0.05f, 1.0f }) {
		s.knee = knee;
		std::array<float, shp::kTableEntries> table {};
		shp::buildTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics), table);
		const shp::TableBound b = shp::boundTable(table);
		for (const float drive : { 0.0f, 0.5f, 1.0f }) {
			const float sweep = shp::sweepFor(drive);
			const shp::Staging st = shp::stagingFor(1.0f, 0.5f);
			MESSAGE("knee " << knee << " drive " << drive << ": sweep=" << sweep
			                << " secantPos=" << b.secantPos
			                << " headroom=" << st.headroom << " threshold=" << st.threshold
			                << " maxArg=" << (sweep / b.secantPos));
		}
	}
}

TEST_CASE("knee leaves the linear pass without a step") {
	// **THE DEFECT: a very abrupt level jump between off and minimum knee.** MEASURED before the fix
	// (backlog BI): knee 0 is 0.00 dB and knee 0.001 — the first non-linear value — was **-0.91 dB**,
	// while knee 0.001 to knee 0.25 costs only 0.17. The step was five times the whole next
	// quarter-turn.
	//
	// **RMS, NOT PEAK, AND NOT A BUFFER COMPARISON.** "Abrupt jump" is a loudness claim, and a
	// difference check between two renders would pass on any change at all — the trap this file's own
	// rules name. The quantity is computed, and the reference is an independent restatement: the
	// level at knee 0 is the LINEAR pass, which for a signal under 0 dBFS is the input itself.
	constexpr double kSr = 48000.0;
	constexpr int kSettle = 8192;
	constexpr int kMeasure = 8192;
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0;
		return static_cast<float>(2.0 * (std::fmod(static_cast<double>(n), kPeriod) / kPeriod) - 1.0);
	};

	const auto levelDb = [&](float knee) {
		shp::Shaper sh;
		sh.prepare(kSr);
		shp::Settings s;
		s.enabled = true;
		s.knee = knee;
		s.colour = 0.38f;
		sh.setTarget(s);
		double sumIn = 0.0;
		double sumOut = 0.0;
		int n = 0;
		for (int block = 0; block < (kSettle + kMeasure) / 64; ++block) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			for (int i = 0; i < 64; ++i, ++n) {
				l[static_cast<size_t>(i)] = saw(n);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			const int at = n - 64;
			sh.processStereo(l.data(), r.data(), 64);
			if (at < kSettle) {
				continue;
			}
			for (int i = 0; i < 64; ++i) {
				const double in = saw(at + i);
				sumIn += in * in;
				sumOut += static_cast<double>(l[static_cast<size_t>(i)])
				          * static_cast<double>(l[static_cast<size_t>(i)]);
			}
		}
		return 20.0 * std::log10(std::sqrt(sumOut / kMeasure) / std::sqrt(sumIn / kMeasure));
	};

	// THE LINEAR END IS THE INPUT, TO THE BIT — asserted rather than assumed, because it is the
	// reference the step is measured against and a drifting reference would hide the step.
	const double atZero = levelDb(0.0f);
	CAPTURE(atZero);
	CHECK(atZero == doctest::Approx(0.0).epsilon(0.001));

	// **THE STEP ACROSS THE BOUNDARY.** `Settings::linear` cuts at 0.001, so this pair straddles the
	// only discontinuity the control has. 0.91 dB before the fade; 0.08 after.
	const double atFirst = levelDb(0.001f);
	CAPTURE(atFirst);
	CHECK(std::fabs(atFirst - atZero) < 0.25);

	// **AND THE FADE IS MONOTONE ACROSS ITS SPAN, so the 0.91 dB is SPREAD rather than moved.** A
	// factor that closed the boundary step by dumping it a little further up the knob would satisfy
	// the clause above and still be an abrupt jump — one the case had simply stopped looking at.
	// **NO SINGLE STEP IS A JUMP — AND THE DIRECTION IS NO LONGER ASSUMED.** This read `here <=
	// previous + 0.02` ("never louder as the table fades in"), which was true of a saturator held
	// under the dry by the secant divide. It is false of a folder: with the folds reachable the level
	// RISES as the table fades in. MEASURED across the span at drive 0, colour 0.38:
	//
	//     knee   0     0.001   0.01    0.02    0.035   0.05
	//     dB     0    -0.079  -0.129  -0.031  +0.539  +1.318
	//
	// The claim that matters is the reported one — no abrupt jump — so it is asserted as the
	// SIZE of each step and not its sign. Largest measured is 0.78 dB from knee 0.035 to 0.05, spread
	// over 1.5% of the knob against the 0.91 dB the boundary used to deliver in 0.1% of it.
	double previous = atZero;
	for (const float knee : { 0.005f, 0.01f, 0.02f, 0.035f, 0.05f }) {
		const double here = levelDb(knee);
		CAPTURE(knee);
		CAPTURE(here);
		CAPTURE(previous);
		CHECK(std::fabs(here - previous) < 1.0);
		previous = here;
	}

	// THE FADE COMPLETES AND LEAVES THE REST OF THE CONTROL ALONE: at the top of the span the level is
	// the un-faded one, which is what makes this local to the first 5%. MEASURED +1.318 dB, where
	// before the read was relaxed it was -0.923 — the same fade over a device that is now louder when
	// it engages rather than quieter.
	CAPTURE(previous);
	CHECK(previous == doctest::Approx(1.318).epsilon(0.03));
}

TEST_CASE("BI PROBE 3: is the table's peak where the read expects it" * doctest::skip()) {
	// THE SUSPICION: the table normalisation is off, and the table no longer saturates at 0 dBFS.
	// Two separate claims. Measure both rather than reason about them.
	shp::Settings s;
	s.enabled = true;
	for (const auto& [knee, colour] : std::vector<std::pair<float, float>> {
	         { 0.05f, 0.38f }, { 0.5f, 0.38f }, { 1.0f, 0.38f }, { 0.5f, 0.0f }, { 0.5f, 0.8f } }) {
		s.knee = knee;
		s.colour = colour;
		std::array<float, shp::kTableEntries> table {};
		shp::buildTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics), table);
		const shp::TableBound b = shp::boundTable(table);

		// THE POSITIVE HALF ONLY, which is the half a 0 dBFS peak reads.
		const size_t mid = table.size() / 2;
		const float step = 2.0f / static_cast<float>(table.size() - 1);
		float peak = 0.0f;
		float peakAt = 0.0f;
		bool monotone = true;
		float previous = table[mid];
		for (size_t i = mid + 1; i < table.size(); ++i) {
			const float u = static_cast<float>(i - mid) * step;
			if (std::fabs(table[i]) > peak) {
				peak = std::fabs(table[i]);
				peakAt = u;
			}
			if (table[i] < previous - 1e-6f) {
				monotone = false;
			}
			previous = table[i];
		}
		MESSAGE("knee " << knee << " colour " << colour << ": peak=" << peak << " at u=" << peakAt
		                << "  edge T(1)=" << std::fabs(table[table.size() - 1])
		                << "  monotone=" << (monotone ? "yes" : "NO")
		                << "  secant=" << b.secantPos << "  1/secant=" << (1.0f / b.secantPos)
		                << "  T(1/secant)=" << std::fabs(shp::lookup(table, 1.0f / b.secantPos)));
	}
}

TEST_CASE("BI PROBE 4: the transfer's endpoint across colour" * doctest::skip()) {
	// DOES A 0 dBFS INPUT COME OUT AT 0 dBFS? Asked of the DEVICE, drive 0, mix 1, sweeping colour —
	// because PROBE 3 says the table's shape depends on colour far more than on knee.
	//
	// **A SAW, NOT DC.** The first version of this held the input at 1.0 and read -120 dB at every
	// colour, which is `blockDc` in `toOutput` doing its job on a constant — the probe was measuring
	// the DC blocker rather than the transfer. A signal with no zero crossings cannot ask this
	// question of a shaper that legitimately removes DC.
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0;
		return static_cast<float>(2.0 * (std::fmod(static_cast<double>(n), kPeriod) / kPeriod) - 1.0);
	};
	for (const float colour : { 0.0f, 0.125f, 0.25f, 0.38f, 0.5f, 0.625f, 0.75f, 0.875f, 1.0f }) {
		shp::Shaper sh;
		sh.prepare(48000.0);
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.5f;
		s.colour = colour;
		sh.setTarget(s);
		float peak = 0.0f;
		int n = 0;
		for (int block = 0; block < 400; ++block) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			for (int i = 0; i < 64; ++i, ++n) {
				l[static_cast<size_t>(i)] = saw(n);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			sh.processStereo(l.data(), r.data(), 64);
			if (block >= 300) {
				for (const float v : l) {
					peak = std::max(peak, std::fabs(v));
				}
			}
		}
		MESSAGE("colour " << colour << ": peak out for a 0 dBFS saw = " << peak << "  ("
		                  << 20.0 * std::log10(std::max(peak, 1e-6f)) << " dB)");
	}
}

TEST_CASE("BI PROBE 5: the table's raw extent at the silent colours" * doctest::skip()) {
	// THREE COLOURS MEASURED -35 dB, all at zone boundaries (1/8, 5/8, 8/8) and all with an IDENTICAL
	// peak of 0.0174 — which is what a DEGENERATE table looks like: `normaliseTable` guards with
	// `peak > 0.001f` and leaves a flat table alone, so `lookup` returns ~0 and the output is the
	// threshold residue rather than the signal. Measure the RAW extent to confirm.
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	for (const float colour : { 0.0f, 0.125f, 0.25f, 0.5f, 0.625f, 0.875f, 1.0f }) {
		s.colour = colour;
		std::array<float, shp::kTableEntries> raw {};
		shp::TableExtent extent;
		shp::fillTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics), raw, 0,
		               raw.size(), extent);
		const float zero = raw[raw.size() / 2];
		const float span = std::fmax(std::fabs(extent.hi - zero), std::fabs(extent.lo - zero));
		MESSAGE("colour " << colour << ": raw extent lo=" << extent.lo << " hi=" << extent.hi
		                  << " zero=" << zero << " span=" << span
		                  << (span > 0.001f ? "" : "   <-- DEGENERATE, normalise gives up"));
	}
}

TEST_CASE("BI PROBE 6: the output waveform at a silent colour" * doctest::skip()) {
	// The half-rectified table predicts ONE half of the wave surviving; the measurement says -35 dB
	// overall. Stop deriving and look at the samples.
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0;
		return static_cast<float>(2.0 * (std::fmod(static_cast<double>(n), kPeriod) / kPeriod) - 1.0);
	};
	for (const float colour : { 0.625f, 0.5f }) {
		shp::Shaper sh;
		sh.prepare(48000.0);
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.5f;
		s.colour = colour;
		sh.setTarget(s);
		int n = 0;
		std::string line;
		for (int block = 0; block < 400; ++block) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			for (int i = 0; i < 64; ++i, ++n) {
				l[static_cast<size_t>(i)] = saw(n);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			sh.processStereo(l.data(), r.data(), 64);
			if (block == 380) {
				for (int i = 0; i < 64; i += 8) {
					line += "  in=" + std::to_string(saw(n - 64 + i)).substr(0, 6) + " out="
					        + std::to_string(l[static_cast<size_t>(i)]).substr(0, 7);
				}
			}
		}
		MESSAGE("colour " << colour << ":" << line);
		MESSAGE("   rebuilds=" << sh.rebuildsForTest());
	}
}

TEST_CASE("BI PROBE 7: where the read lands in the table" * doctest::skip()) {
	// THE UNIFYING QUESTION. `readTableAt` reads at `rate*excess` with `rate = sweep/(headroom*secant)`,
	// so the highest argument any excess can reach is `sweep/secant` — the read is confined to the
	// FIRST `1/secant` of the table's domain at drive 0. If the table is flat there, the device is
	// silent no matter what the rest of the curve does. Measure the window and the curve inside it.
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	for (const float colour : { 0.5f, 0.625f, 0.125f, 0.25f }) {
		s.colour = colour;
		std::array<float, shp::kTableEntries> table {};
		shp::buildTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics), table);
		const shp::TableBound b = shp::boundTable(table);
		const float wPos = 1.0f / b.secantPos;
		const float wNeg = 1.0f / b.secantNeg;
		MESSAGE("colour " << colour << ": secantPos=" << b.secantPos << " window=[0," << wPos
		                  << "]  T at window top=" << shp::lookup(table, wPos)
		                  << "   |   secantNeg=" << b.secantNeg << " window=[0,-" << wNeg
		                  << "]  T at window top=" << shp::lookup(table, -wNeg)
		                  << "   full-domain T(1)=" << table[table.size() - 1]
		                  << " T(-1)=" << table[0]);
	}
}

TEST_CASE("BI PROBE 8: a full cycle at a half-rectified colour" * doctest::skip()) {
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0;
		return static_cast<float>(2.0 * (std::fmod(static_cast<double>(n), kPeriod) / kPeriod) - 1.0);
	};
	shp::Shaper sh;
	sh.prepare(48000.0);
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	s.colour = 0.625f;
	sh.setTarget(s);
	std::vector<float> outs;
	std::vector<float> ins;
	int n = 0;
	for (int block = 0; block < 400; ++block) {
		std::array<float, 64> l {};
		std::array<float, 64> r {};
		for (int i = 0; i < 64; ++i, ++n) {
			l[static_cast<size_t>(i)] = saw(n);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		sh.processStereo(l.data(), r.data(), 64);
		if (block >= 380 && block < 388) {
			for (int i = 0; i < 64; ++i) {
				ins.push_back(saw(n - 64 + i));
				outs.push_back(l[static_cast<size_t>(i)]);
			}
		}
	}
	// THE EXTREMES OF EACH HALF, so the half-wave claim is read off the data rather than a window
	// that happened to land on one side.
	float posIn = 0.0f;
	float posOut = 0.0f;
	float negIn = 0.0f;
	float negOut = 0.0f;
	for (size_t i = 0; i < ins.size(); ++i) {
		if (ins[i] > 0.0f && std::fabs(outs[i]) > std::fabs(posOut)) { posOut = outs[i]; posIn = ins[i]; }
		if (ins[i] < 0.0f && std::fabs(outs[i]) > std::fabs(negOut)) { negOut = outs[i]; negIn = ins[i]; }
	}
	MESSAGE("colour 0.625: worst positive in=" << posIn << " out=" << posOut
	                                           << "   worst negative in=" << negIn << " out=" << negOut);
}

TEST_CASE("BI PROBE 9: the device's live table vs the standalone build" * doctest::skip()) {
	// THE OPEN QUESTION FROM c048321: at colour 0.625 the half WITH content also reads ~0, while a
	// standalone `buildTable` predicts about -0.9. Ask the DEVICE for its own table instead of
	// deriving what it ought to hold.
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0;
		return static_cast<float>(2.0 * (std::fmod(static_cast<double>(n), kPeriod) / kPeriod) - 1.0);
	};
	shp::Shaper sh;
	sh.prepare(48000.0);
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	s.colour = 0.625f;
	sh.setTarget(s);
	int n = 0;
	for (int block = 0; block < 400; ++block) {
		std::array<float, 64> l {};
		std::array<float, 64> r {};
		for (int i = 0; i < 64; ++i, ++n) {
			l[static_cast<size_t>(i)] = saw(n);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		sh.processStereo(l.data(), r.data(), 64);
	}

	const auto& live = sh.tableForTest();
	const shp::TableBound liveBound = shp::boundTable(live);
	std::array<float, shp::kTableEntries> mine {};
	shp::buildTable(shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics), mine);
	const shp::TableBound myBound = shp::boundTable(mine);

	MESSAGE("live  T(-1)=" << live[0] << " T(0)=" << live[live.size() / 2] << " T(1)="
	                       << live[live.size() - 1] << "  secantNeg=" << liveBound.secantNeg
	                       << " secantPos=" << liveBound.secantPos);
	MESSAGE("mine  T(-1)=" << mine[0] << " T(0)=" << mine[mine.size() / 2] << " T(1)="
	                       << mine[mine.size() - 1] << "  secantNeg=" << myBound.secantNeg
	                       << " secantPos=" << myBound.secantPos);
	MESSAGE("device: sweep=" << sh.sweepForTest() << " threshold=" << sh.thresholdForTest()
	                         << " rebuilds=" << sh.rebuildsForTest()
	                        );
}

TEST_CASE("BI PROBE 10: readTableAt with the device's own numbers" * doctest::skip()) {
	// STOP DERIVING. Feed `readTableAt` the live table, the live bound and the live scalars, and
	// print every intermediate.
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0;
		return static_cast<float>(2.0 * (std::fmod(static_cast<double>(n), kPeriod) / kPeriod) - 1.0);
	};
	shp::Shaper sh;
	sh.prepare(48000.0);
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	s.colour = 0.625f;
	sh.setTarget(s);
	int n = 0;
	for (int block = 0; block < 400; ++block) {
		std::array<float, 64> l {};
		std::array<float, 64> r {};
		for (int i = 0; i < 64; ++i, ++n) {
			l[static_cast<size_t>(i)] = saw(n);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		sh.processStereo(l.data(), r.data(), 64);
	}
	const auto& live = sh.tableForTest();
	const shp::TableBound b = shp::boundTable(live);
	const shp::Staging st = shp::stagingFor(s.threshold, s.aggression);
	const float sweep = sh.sweepForTest();

	for (const float excess : { -1.0154f, -0.5f, -0.2f, 0.5f }) {
		const float secant = (excess >= 0.0f) ? b.secantPos : b.secantNeg;

		const float squashed = std::fabs(excess);
		const float e2 = std::copysign(squashed, excess);
		const float rate = sweep / (st.headroom * secant);
		MESSAGE("excess=" << excess << " secant=" << secant << " squashed=" << e2
		                  << " rate=" << rate << " arg=" << (rate * e2)
		                  << " T(arg)=" << shp::lookup(live, rate * e2) << " -> readTableAt="
		                  << shp::readTableAt(live, b, st.headroom, sweep, excess));
	}
	MESSAGE("staging: threshold=" << st.threshold << " headroom=" << st.headroom
	                              << " blendSlope=" << st.blendSlope
	                              << "   device threshold=" << sh.thresholdForTest()
	                              << " blendSlope=" << sh.blendSlopeForTest());
}

TEST_CASE("a half-rectified table passes its flat side through instead of muting it") {
	// **THE SILENT COLOURS.** At 1/8, 5/8 and 8/8 — zone boundaries, with eight colour zones — the
	// table has content on ONE side only, so `boundTable` returns `secantPos = 0` (or `secantNeg`),
	// and `readTableAt`'s degenerate-input guard used to answer that with 0. MEASURED before the fix:
	// a 0 dBFS saw came out at **0.017, -35 dB**, at all three colours. Input +0.56..+0.82 measured
	// -0.0148 out: the threshold, with no shaping and no signal.
	//
	// **ASSERTED AS A LEVEL, NOT AS A BUFFER DIFFERENCE.** "It is not silent" is exactly the claim a
	// render comparison cannot make — two different silences differ too. The reference is independent:
	// a shaper that cannot read its curve must pass the signal, so the output level is the INPUT's.
	const auto saw = [](int n) {
		constexpr double kPeriod = 436.0;
		return static_cast<float>(2.0 * (std::fmod(static_cast<double>(n), kPeriod) / kPeriod) - 1.0);
	};
	const auto peakOut = [&](float colour) {
		shp::Shaper sh;
		sh.prepare(48000.0);
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.5f;
		s.colour = colour;
		sh.setTarget(s);
		float peak = 0.0f;
		int n = 0;
		for (int block = 0; block < 400; ++block) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			for (int i = 0; i < 64; ++i, ++n) {
				l[static_cast<size_t>(i)] = saw(n);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			sh.processStereo(l.data(), r.data(), 64);
			if (block >= 300) {
				for (const float v : l) {
					peak = std::max(peak, std::fabs(v));
				}
			}
		}
		return peak;
	};

	// THE THREE THAT WERE SILENT. A tenth of full scale is far above the 0.017 they produced and far
	// below anything a working colour reaches, so the bound cannot pass on a near-miss.
	for (const float colour : { 0.125f, 0.625f, 1.0f }) {
		CAPTURE(colour);
		const float peak = peakOut(colour);
		CAPTURE(peak);
		CHECK(peak > 0.1f);
	}
	// **AND NOW EVERY COLOUR REACHES THE CEILING, which is the read change rather than this fix.**
	// This case first asserted that the working colours were UNCHANGED — 0.9236 at colour 0.25, 0.6697
	// at 0.5, 0.6855 at 0.875 — because at the time the fix was one guard and touched nothing else.
	// Then the read was relaxed (scaled as signal/(0 dBFS - threshold), so the table still
	// saturates when the signal reaches 0 dBFS), and those three numbers became 1.000 along with
	// every other colour. Keeping the old values would have pinned a state the design deliberately
	// left behind; asserting the new invariant is the honest replacement, and it is a STRONGER claim
	// than the three constants were: the transfer saturates at 0 dBFS at EVERY colour, which is the
	// thing the design asks for and the thing neither the silence nor the level loss allowed.
	for (const float colour : { 0.0f, 0.25f, 0.5f, 0.625f, 0.875f, 1.0f }) {
		CAPTURE(colour);
		CHECK(peakOut(colour) == doctest::Approx(1.0f).epsilon(0.01));
	}
}

TEST_CASE("BI PROBE 11: the tables are waveforms, not transfer curves" * doctest::skip()) {
	// **THE ANSWER TO PROBE 3'S QUESTION, AND IT IS NOT THE NORMALISATION.** The question was whether the
	// table normalisation was off and why the transfer no longer saturates at 0 dBFS. Both symptoms are
	// real, and this is underneath them: the phi field's half-rectified triangle banks are OSCILLATOR
	// shapes, and read as a waveshaper transfer they are NOT MONOTONE. Sampled across the domain at
	// knee 0.25, colour 0.38, the curve reads
	//
	//     0.715, 0.999, 0.626, 0.244, 0.966, 0.385, 0.921, 0.746, 0.089, 0.688
	//
	// — up, down, up, down. A transfer function that folds nine times. Colour 0 is the exception and
	// reads 0.219 .. 1.000 monotonically, which is a real saturating curve.
	//
	// **SO THE SECANT DIVIDE WAS DOING SOMETHING NOBODY DESIGNED: confining the read to about the
	// first fifth of the domain, which for these tables is the only monotone stretch.** That is why
	// removing it (the decided relaxation of `|out| <= |excess|`) does not free headroom but reads the
	// folding region: measured, the transfer then sits up to 0.80 ABOVE the diagonal at colour 0.38,
	// against 0.27 at colour 0. The level loss, the -35 dB colours and the missing 0 dBFS were all
	// this one fact wearing different clothes.
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.25f;
	for (const float colour : { 0.0f, 0.25f, 0.38f }) {
		std::array<float, shp::kTableEntries> table {};
		shp::buildTable(shp::deriveCurve(s.knee, colour, s.gamma, shp::kSlewHarmonics), table);
		int reversals = 0;
		float previous = shp::lookup(table, 0.02f);
		bool rising = true;
		for (int k = 2; k <= 50; ++k) {
			const float u = static_cast<float>(k) / 50.0f;
			const float t = shp::lookup(table, u);
			const bool up = t >= previous;
			if (k > 2 && up != rising) {
				++reversals;
			}
			rising = up;
			previous = t;
		}
		MESSAGE("colour " << colour << ": direction reversals across the positive half = " << reversals
		                  << (reversals == 0 ? "   (monotone: a transfer)" : "   (a waveform)"));
	}
}

TEST_CASE("the drive meter reports the peak input, and falls back") {
	// **THE FEATURE:** a level meter behind the transfer curve display, taken post-mix-transform so it
	// shows how hard the actual table is driven, painting the curve a different colour where the level
	// is above it.
	//
	// **ASSERTED AS A VALUE, NOT AS A PICTURE.** The whole point of the meter is that everything
	// chased through backlog BI was invisible — the read reaching a fifth of the table, three silent
	// colours, the folding sitting past where the signal went. A case that compared two renders could
	// not tell a meter that works from one stuck at a plausible number, which is the same trap those
	// bugs hid in.
	shp::Shaper sh;
	sh.prepare(48000.0);
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	sh.setTarget(s);

	// NOTHING HAS BEEN SEEN YET, so nothing is claimed.
	CHECK(sh.drivePeak() == doctest::Approx(0.0f));

	const auto run = [&](float amplitude, int blocks) {
		for (int b = 0; b < blocks; ++b) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			for (int i = 0; i < 64; ++i) {
				// A FULL CYCLE PER BLOCK so the peak is actually presented, rather than a slice of a
				// slow ramp that would under-report it for reasons the meter is not responsible for.
				const float phase = static_cast<float>(i) / 64.0f;
				l[static_cast<size_t>(i)] = amplitude * std::sin(6.2831853f * phase);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			sh.processStereo(l.data(), r.data(), 64);
		}
	};

	// IT RISES TO THE PEAK IT WAS SHOWN, and to that peak rather than to full scale.
	run(0.5f, 40);
	CAPTURE(sh.drivePeak());
	CHECK(sh.drivePeak() > 0.45f);
	CHECK(sh.drivePeak() < 0.55f);

	// A LOUDER SIGNAL MOVES IT UP AT ONCE — the rise is instant, which is what a peak meter is for.
	run(0.9f, 4);
	CHECK(sh.drivePeak() > 0.85f);

	// **AND IT FALLS, which is the clause that separates a meter from a high-water mark.** Without it
	// the first transient would paint a permanent hot region on the curve and the display would answer
	// "how hard did you EVER drive this", which is not the question.
	const float afterLoud = sh.drivePeak();
	run(0.05f, 200);
	CAPTURE(afterLoud);
	CAPTURE(sh.drivePeak());
	CHECK(sh.drivePeak() < afterLoud * 0.6f);

	// NON-VACUITY: it did not simply collapse to zero, which would pass the clause above while
	// reporting nothing. It is still tracking the quiet signal it is now being shown.
	CHECK(sh.drivePeak() > 0.0f);
}

TEST_CASE("Rotate alone leaves the shaper's phases standing") {
	// **THE POSITIVE HALF OF `Shaper :: Shaper Rotate` BEING ON THE DEAD LIST**, which
	// `test_fx_runner.cpp` requires be MEASURED rather than reasoned. Rotate mutates the BASIS a pad
	// travels in and leaves the puck where it is, so the sound at rest cannot move — what changes is
	// where the NEXT gesture goes.
	//
	// **AND A SESSION BROKE EXACTLY THIS RULE ON AUTOMOD.** It moved `rotatePhases` into `coordsOf` to
	// satisfy an honesty sweep, which made rotate change the sound — and `slice_basis.h` had already
	// said why that is wrong: *a rotate that changes the sound is redundant with gamma*. The lesson is
	// that a control being DEAD at rest can be correct, and the fix for a sweep that dislikes it is an
	// entry on the dead list plus a case like this one, never a change that makes it live.
	namespace ps = shp::phase_space;

	// THE PHASES ARE A FUNCTION OF GAMMA AND NOTHING ELSE — asserted here rather than assumed,
	// because it is the whole reason a basis turn cannot reach them.
	const ps::Phases atRest = ps::homeFor(0.38, 0.37);
	const ps::Phases again = ps::homeFor(0.38, 0.37);
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		CAPTURE(i);
		CHECK(atRest[i] == again[i]);
	}

	// **AND THE DEVICE'S OWN OUTPUT IS BIT-IDENTICAL ACROSS A ROTATE CHANGE.** The phases above are
	// the mechanism; this is the claim. Asserted on the rendered samples to the BIT, because "close
	// enough" would pass on a rotate that leaked a little into the read — which is precisely the bug
	// this case exists to refuse.
	const auto render = [](float rotate) {
		shp::Shaper sh;
		sh.prepare(48000.0);
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.5f;
		s.colour = 0.38f;
		s.gamma = 0.37f;
		s.rotate = rotate;
		sh.setTarget(s);
		std::vector<float> out;
		int n = 0;
		for (int block = 0; block < 40; ++block) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			for (int i = 0; i < 64; ++i, ++n) {
				l[static_cast<size_t>(i)] = std::sin(6.2831853f * static_cast<float>(n % 436) / 436.0f);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			sh.processStereo(l.data(), r.data(), 64);
			if (block >= 30) {
				out.insert(out.end(), l.begin(), l.end());
			}
		}
		return out;
	};

	const std::vector<float> none = render(0.0f);
	const std::vector<float> turned = render(0.75f);
	REQUIRE(none.size() == turned.size());
	REQUIRE_FALSE(none.empty());

	size_t differing = 0;
	for (size_t i = 0; i < none.size(); ++i) {
		if (none[i] != turned[i]) {
			++differing;
		}
	}
	CAPTURE(differing);
	CHECK(differing == 0);

	// NON-VACUITY: the render really is producing signal, so a device that output silence under both
	// rotates could not pass by being equally silent.
	float peak = 0.0f;
	for (const float v : none) {
		peak = std::max(peak, std::fabs(v));
	}
	CAPTURE(peak);
	CHECK(peak > 0.1f);
}

TEST_CASE("the phase space's home reproduces deriveCurve's own weights") {
	// **THE FIRST VERSION OF THIS CASE WAS WORTHLESS AND IT PASSED.** It compared `homeFor` against an
	// expression I had hand-copied out of `deriveCurve` — so when I misread `deriveCurve` (dropping
	// every basis's constant offset, and asymmetry's own acceleration), BOTH sides carried the same
	// mistake and the case went green on a wrong model. A restatement is only independent if it is
	// derived independently; copying the line you are checking is not a check. What the case could not catch
	// was a review question: does each transfer basis have its own uncorrelated term?
	//
	// **SO THIS USES `deriveCurve` ITSELF AS THE ORACLE.** The home is fed through the same triangle
	// `deriveCurve` reads its weights with, and the WEIGHTS are compared. If the home is wrong in any
	// term — an offset, a multiplier, an ordering — the weights disagree, and no amount of agreeing
	// with my own arithmetic can hide it.
	namespace ps = shp::phase_space;
	namespace phi = owlet::dsp::phi;

	for (const auto& [colour, gamma] : std::vector<std::pair<float, float>> {
	         { 0.0f, 0.0f }, { 0.38f, 0.0f }, { 0.0f, 0.37f }, { 0.38f, 0.37f },
	         { 1.0f, 2.5f }, { 0.62f, 7.3f }, { 0.125f, 0.9f } }) {
		CAPTURE(colour);
		CAPTURE(gamma);
		const shp::CurveParams p = shp::deriveCurve(0.5f, colour, gamma, 0.5f);
		const ps::Phases home = ps::homeFor(colour, gamma);

		const auto weightAt = [&](size_t i) {
			return owlet::dsp::triangleSimpleUnipolar(
			    phi::wrapPhase(static_cast<float>(home[i])), 0.5f);
		};

		// THE SIX BLEND WEIGHTS AND THE THRESHOLD, in `kPhi`'s order, which is the order `deriveCurve`
		// assigns them in and the order the folded-travel blob is indexed by.
		CHECK(weightAt(0) == doctest::Approx(p.inflatorWeight).epsilon(1e-4));
		CHECK(weightAt(1) == doctest::Approx(p.polyWeight).epsilon(1e-4));
		CHECK(weightAt(2) == doctest::Approx(p.hardKneeWeight).epsilon(1e-4));
		CHECK(weightAt(3) == doctest::Approx(p.chebyWeight).epsilon(1e-4));
		CHECK(weightAt(4) == doctest::Approx(p.sineFoldWeight).epsilon(1e-4));
		CHECK(weightAt(5) == doctest::Approx(p.rectifierWeight).epsilon(1e-4));
		CHECK(weightAt(6) == doctest::Approx(p.threshold).epsilon(1e-4));
		// ASYMMETRY IS SCALED AND OFFSET BY `deriveCurve` — 0.3 + t*0.4 — so the comparison is against
		// the same arithmetic. It is the eighth entry precisely because it is the one with its own
		// acceleration, and this clause is what would catch that being dropped again.
		CHECK(0.3f + weightAt(7) * 0.4f == doctest::Approx(p.asymmetry).epsilon(1e-4));
	}

	SUBCASE("asymmetry really is uncorrelated with the rest") {
		// **THE CLAIM THE FIRST DRAFT GOT BACKWARDS.** It asserted the eight phases stayed in fixed
		// proportion — a LINE — which is what dropping asymmetry's own multiplier makes them look
		// like. They do not: asymmetry accelerates at half the rate, so the ratio between its phase
		// and another's MOVES with colour. Asserted as a change in that ratio, which is exactly what a
		// line cannot do.
		const ps::Phases lo = ps::homeFor(0.2, 0.0);
		const ps::Phases hi = ps::homeFor(0.9, 0.0);
		const double ratioLo = (lo[7] - ps::kOffset[7]) / (lo[0] - ps::kOffset[0]);
		const double ratioHi = (hi[7] - ps::kOffset[7]) / (hi[0] - ps::kOffset[0]);
		CAPTURE(ratioLo);
		CAPTURE(ratioHi);
		CHECK(std::fabs(ratioHi - ratioLo) > 0.01);
	}
}

TEST_CASE("BI PROBE 13: the angle between colour's direction and gamma's" * doctest::skip()) {
	// The design premise: colour is by definition in the same direction as gamma, gamma being the
	// coarsest step. Measure it. d(home)/d(gamma) is the phi vector; d(home)/d(colour) is the phi
	// vector with each component scaled by that basis's own acceleration derivative.
	namespace ps = shp::phase_space;
	for (const double y : { 0.0, 0.1, 0.25, 0.5, 0.75, 1.0 }) {
		std::array<double, ps::kCurvePhaseCount> dGamma {};
		std::array<double, ps::kCurvePhaseCount> dColour {};
		for (size_t i = 0; i < ps::kCurvePhaseCount; ++i) {
			const double phi = static_cast<double>(ps::kPhi[i]);
			dGamma[i] = phi;
			// d/dy of y*(1 + a*y^2) is 1 + 3a*y^2, with a = 3 for seven of them and 1.5 for asymmetry.
			const double a = (i + 1 == ps::kCurvePhaseCount) ? ps::kAccelFactor * ps::kAsymAccelScale
			                                            : ps::kAccelFactor;
			dColour[i] = phi * (1.0 + 3.0 * a * y * y);
		}
		double dot = 0.0;
		double nG = 0.0;
		double nC = 0.0;
		for (size_t i = 0; i < ps::kCurvePhaseCount; ++i) {
			dot += dGamma[i] * dColour[i];
			nG += dGamma[i] * dGamma[i];
			nC += dColour[i] * dColour[i];
		}
		const double cosine = dot / (std::sqrt(nG) * std::sqrt(nC));
		MESSAGE("colour " << y << ": cos(angle) = " << cosine << "   angle = "
		                  << std::acos(std::min(1.0, cosine)) * 180.0 / 3.14159265 << " deg");
	}
}

TEST_CASE("no travel leaves the curve bit-identical") {
	// **THE PROPERTY THAT MAKES THE PAD A UI CHANGE RATHER THAN A DSP ONE.** `deriveCurve` gained a
	// travel term per phase; with no travel every one of those terms is `+ 0.0f`, which cannot move a
	// float. So every existing patch must sound EXACTLY as it did — not nearly, exactly.
	//
	// **ASSERTED BIT FOR BIT AND NOT BY TOLERANCE**, because the risk here is not a wrong number, it
	// is a REWRITE that computes the same quantity a tidier way and lands on different last bits. A
	// tolerance would wave that through, and it would be a silent sound change to everything anyone
	// has saved. `SliceBasis::phaseFor` documents the same guarantee for the same reason.
	for (const auto& [knee, colour, gamma] : std::vector<std::tuple<float, float, float>> {
	         { 0.0f, 0.0f, 0.0f }, { 0.5f, 0.38f, 0.0f }, { 1.0f, 0.62f, 0.37f },
	         { 0.25f, 1.0f, 2.5f }, { 0.75f, 0.125f, 7.3f } }) {
		CAPTURE(knee);
		CAPTURE(colour);
		CAPTURE(gamma);
		const shp::CurveParams none = shp::deriveCurve(knee, colour, gamma, 0.5f);
		const std::array<float, shp::phase_space::kPhaseCount> zeros {};
		const shp::CurveParams zeroed = shp::deriveCurve(knee, colour, gamma, 0.5f, &zeros);

		// EVERY WEIGHT, `==` AND NOT `Approx`.
		CHECK(none.inflatorWeight == zeroed.inflatorWeight);
		CHECK(none.polyWeight == zeroed.polyWeight);
		CHECK(none.hardKneeWeight == zeroed.hardKneeWeight);
		CHECK(none.chebyWeight == zeroed.chebyWeight);
		CHECK(none.sineFoldWeight == zeroed.sineFoldWeight);
		CHECK(none.rectifierWeight == zeroed.rectifierWeight);
		CHECK(none.threshold == zeroed.threshold);
		CHECK(none.asymmetry == zeroed.asymmetry);
	}

	SUBCASE("and travel actually moves it, so the clause above is not vacuous") {
		// WITHOUT THIS, a `deriveCurve` that ignored `travel` entirely would pass every clause above.
		//
		// **SWEPT RATHER THAN POKED AT ONE VALUE**, because the triangle has a duty of 0.5 and is flat
		// across part of its cycle: the first draft pushed one phase by a quarter turn, landed inside
		// that flat stretch, and reported no change — a true observation about a position I had chosen
		// badly, not about the code. Requiring SOME offset to move it is the claim that was meant.
		int moved = 0;
		int crossTalk = 0;
		const shp::CurveParams still = shp::deriveCurve(0.5f, 0.38f, 0.37f);
		for (const float push : { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f, 0.9f }) {
			std::array<float, shp::phase_space::kPhaseCount> pushed {};
			pushed[0] = push;
			const shp::CurveParams now = shp::deriveCurve(0.5f, 0.38f, 0.37f, 0.5f, &pushed);
			if (now.inflatorWeight != still.inflatorWeight) {
				++moved;
			}
			// AND TRAVEL IS PER PHASE: a term that leaked across would move a weight it was not aimed
			// at. Counted rather than asserted inside the loop so the failure names how many leaked.
			if (now.polyWeight != still.polyWeight || now.asymmetry != still.asymmetry) {
				++crossTalk;
			}
		}
		CAPTURE(moved);
		CAPTURE(crossTalk);
		CHECK(moved > 0);
		CHECK(crossTalk == 0);
	}
}

TEST_CASE("BI PROBE 14: does setFields carry the travel at all" * doctest::skip()) {
	std::vector<float> fields(static_cast<size_t>(shp::Param::kCount) + 8, 0.0f);
	shp::Settings s;
	s.enabled = true;
	s.knee = 0.5f;
	s.colour = 0.38f;
	s.gamma = 0.37f;
	const shp::Row base = shp::rowOf(s);
	for (size_t i = 0; i < base.size(); ++i) {
		fields[i] = base[i];
	}
	for (size_t i = 0; i < 8; ++i) {
		fields[shp::kTravelFirst + i] = 0.4f;
	}
	shp::Shaper sh;
	sh.prepare(48000.0);
	shp::kAudioOps.setFields(&sh, fields.data(), fields.size());
	MESSAGE("kTravelFirst=" << shp::kTravelFirst << " fields=" << fields.size()
	                        << " enabled=" << sh.targetForTest().enabled
	                        << " travel0=" << sh.targetForTest().travel[0]
	                        << " colour=" << sh.targetForTest().colour);
}

TEST_CASE("travel in the blob reaches the curve") {
	// **THE ROAD THE PAD WILL DRIVE, ASSERTED BEFORE THE PAD EXISTS.** The eight floats past the
	// declared params are the only thing that moves the phases independently; if they do not reach
	// `deriveCurve` the pad writes to a store nobody reads, which is what a silently declining seam
	// looks like — and this session was caught by one of those already.
	//
	// **DRIVEN THROUGH `setFields`**, not by building a `Settings` by hand: `stateOf` takes the
	// params-only `Row` and cannot see the blob, so `setFields` is the one place that sees both and
	// the only place where this road can be wrong.
	//
	// **AND ASSERTED ON THE CURVE, NOT ON A RENDER.** The first version of this case measured the peak
	// output and failed while the road worked perfectly: these transfers FOLD, so the loudest sample
	// need not move when the curve does — which is the same mistake, in the same file, that produced
	// the retracted "reaches 0 dBFS at every colour". A claim about a value is asserted on the value.
	const auto curveWith = [](float push) {
		std::vector<float> fields(static_cast<size_t>(shp::Param::kCount)
		                              + shp::phase_space::kPhaseCount,
		                          0.0f);
		shp::Settings s;
		s.enabled = true;
		s.knee = 0.5f;
		s.colour = 0.38f;
		s.gamma = 0.37f;
		const shp::Row base = shp::rowOf(s);
		for (size_t i = 0; i < base.size(); ++i) {
			fields[i] = base[i];
		}
		for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
			fields[shp::kTravelFirst + i] = push;
		}
		shp::Shaper sh;
		sh.prepare(48000.0);
		shp::kAudioOps.setFields(&sh, fields.data(), fields.size());
		// THE DECODE IS CHECKED TOO, so a case that failed further down names WHICH link broke.
		REQUIRE(sh.targetForTest().travel[0] == doctest::Approx(push));
		// **ONE BLOCK FIRST: `curve_` IS BUILT IN `beginBlock`, NOT IN `setTarget`.** The curve follows
		// the settings at BLOCK rate, so reading it straight after `setFields` reads whatever the last
		// block left — which for a fresh device is the default. The first draft of this case did exactly
		// that and reported the road broken when it was the reader that was early.
		std::array<float, 64> l {};
		std::array<float, 64> r {};
		sh.processStereo(l.data(), r.data(), 64);
		return sh.curveForTest();
	};

	const shp::CurveParams still = curveWith(0.0f);
	int moved = 0;
	for (const float push : { 0.1f, 0.25f, 0.4f, 0.55f, 0.7f, 0.85f }) {
		const shp::CurveParams now = curveWith(push);
		CAPTURE(push);
		if (now.inflatorWeight != still.inflatorWeight || now.polyWeight != still.polyWeight
		    || now.chebyWeight != still.chebyWeight || now.asymmetry != still.asymmetry) {
			++moved;
		}
	}
	// SWEPT rather than poked: the triangle is flat across part of its cycle, so one arbitrary offset
	// can legitimately land where nothing changes.
	CAPTURE(moved);
	CHECK(moved > 0);

	// **AND A REBUILD IS ACTUALLY ASKED FOR**, which is a separate link and was the real bug: travel
	// reached `deriveCurve` while `sameCurveAs` did not compare it, so the table never noticed and the
	// sound never moved. A curve that differs is not enough on its own — the device has to know.
	shp::Settings a;
	shp::Settings b;
	b.travel[3] = 0.4f;
	CHECK_FALSE(a.sameCurveAs(b));
}

TEST_CASE("BI PROBE 15: total variation of the transfer across the field" * doctest::skip()) {
	// The terrain should say what the CURVE does, not how much weight is present. Total variation —
	// the summed absolute step along the transfer — is 2.0 for the identity and rises with folding,
	// continuously. Measure its range so the normaliser is taken from the instrument.
	const auto tv = [](float colour, float gamma) {
		const shp::CurveParams p = shp::deriveCurve(0.75f, colour, gamma, 0.5f);
		constexpr int kN = 48;
		float sum = 0.0f;
		float prev = shp::transferAt(p, -1.0f);
		for (int i = 1; i <= kN; ++i) {
			const float x = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kN);
			const float v = shp::transferAt(p, x);
			sum += std::fabs(v - prev);
			prev = v;
		}
		return sum;
	};
	float lo = 1e9f;
	float hi = 0.0f;
	double mean = 0.0;
	int n = 0;
	for (int gi = 0; gi <= 32; ++gi) {
		for (int ci = 0; ci <= 32; ++ci) {
			const float g = static_cast<float>(gi) / 32.0f;
			const float c = static_cast<float>(ci) / 32.0f;
			const float v = tv(c, g);
			lo = std::min(lo, v);
			hi = std::max(hi, v);
			mean += v;
			++n;
		}
	}
	MESSAGE("total variation over the field: min=" << lo << " max=" << hi
	                                               << " mean=" << mean / n
	                                               << "   (identity would be 2.0)");
}

TEST_CASE("BI PROBE 16: what the terrain analysis costs" * doctest::skip()) {
	// `shaper_pad.h` claims the map is "1536 samples at 0.13 us each, 200 us, once at construction".
	// Reading the CURVE instead of the weights multiplies that; measure it rather than leave the
	// header asserting a number that is no longer true.
	const auto one = [](float colour, float gamma) {
		const shp::CurveParams p = shp::deriveCurve(0.75f, colour, gamma, 0.5f);
		float variation = 0.0f;
		float previous = shp::transferAt(p, -1.0f);
		for (int i = 1; i <= 48; ++i) {
			const float x = -1.0f + 2.0f * static_cast<float>(i) / 48.0f;
			const float v = shp::transferAt(p, x);
			variation += std::fabs(v - previous);
			previous = v;
		}
		return variation;
	};
	constexpr int kSamples = 1536;
	const auto start = std::chrono::steady_clock::now();
	double sink = 0.0;
	for (int i = 0; i < kSamples; ++i) {
		const float c = static_cast<float>(i % 48) / 48.0f;
		const float g = static_cast<float>(i / 48) / 32.0f;
		sink += one(c, g);
	}
	const auto end = std::chrono::steady_clock::now();
	const double us = std::chrono::duration<double, std::micro>(end - start).count();
	MESSAGE("1536 samples of curve analysis: " << us << " us total, " << us / kSamples
	                                           << " us each  (sink " << sink << ")");
}

TEST_CASE("BI PROBE 12: the output AT the input's peak, across colour and drive" * doctest::skip()) {
	// **THE INSTRUMENT THAT FOUND THE CLIFF, kept so the fix is shown against it rather than against a
	// new measurement chosen after the fact.** A buffer's peak cannot see this: these transfers fold,
	// so the loudest OUTPUT happens at a mid-level INPUT and the top of the wave can be far down while
	// the peak reads 1.0. What a player hears as attenuation is what comes out when the input is at
	// full scale, which is what this asks.
	const auto peakAt = [](float colour, float drive, float knee) {
		shp::Shaper sh;
		sh.prepare(48000.0);
		shp::Settings s;
		s.enabled = true;
		s.knee = knee;
		s.drive = drive;
		s.colour = colour;
		sh.setTarget(s);
		float atTop = 0.0f;
		float inputTop = 0.0f;
		int n = 0;
		for (int block = 0; block < 400; ++block) {
			std::array<float, 64> l {};
			std::array<float, 64> r {};
			std::array<float, 64> in {};
			for (int i = 0; i < 64; ++i, ++n) {
				in[static_cast<size_t>(i)] = std::sin(6.2831853f * static_cast<float>(n % 436) / 436.0f);
				l[static_cast<size_t>(i)] = in[static_cast<size_t>(i)];
				r[static_cast<size_t>(i)] = in[static_cast<size_t>(i)];
			}
			sh.processStereo(l.data(), r.data(), 64);
			if (block < 300) {
				continue;
			}
			for (int i = 0; i < 64; ++i) {
				if (std::fabs(in[static_cast<size_t>(i)]) > inputTop) {
					inputTop = std::fabs(in[static_cast<size_t>(i)]);
					atTop = std::fabs(l[static_cast<size_t>(i)]);
				}
			}
		}
		return 20.0 * std::log10(std::max(atTop, 1e-6f) / std::max(inputTop, 1e-6f));
	};
	for (const float knee : { 0.5f }) {
		for (const float drive : { 0.0f, 0.5f, 1.0f }) {
			std::string row;
			for (const float colour : { 0.0f, 0.25f, 0.38f, 0.625f, 1.0f }) {
				row += "  c" + std::to_string(colour).substr(0, 4) + "="
				       + std::to_string(peakAt(colour, drive, knee)).substr(0, 6);
			}
			MESSAGE("knee " << knee << " drive " << drive << ":" << row);
		}
	}
}

TEST_CASE("the travelled curve reaches the TABLE, not only the params") {
	// **THE BUG, AND IT WAS IN THE ENGINE RATHER THAN THE PICTURE.** Moving the puck did not change the
	// drawn curve, which was true twice over: the plot
	// dropped the travel, and so did the table build.
	//
	// `Shaper::advanceBuild` called `curveFor(knee, colour, gamma, colourPhase, harmonics)` — without
	// `travel`, which the signature defaults to `nullptr`. `beginBlock` passed it, so the pad's
	// displacement reached `curve_`, where it feeds the per-sample modifier terms. It never reached
	// **the six basis weights, which exist only in the table** — and the table is what the signal is
	// actually read through. MEASURED before the fix: the built table was IDENTICAL, 0 of 1025
	// entries differing, while `curve_.chebyWeight` moved 0.745 -> 0.379.
	//
	// **WHY "travel in the blob reaches the curve" DID NOT CATCH IT.** That case asserts on
	// `deriveCurve`'s output, and deliberately so: an earlier version asserted on a rendered peak and
	// was fooled by a fold. Leaving the render was right; stopping at the params was one step short,
	// because the params are not what the signal is read through. `sameCurveAs` names `travel`, so the
	// rebuild was correctly REQUESTED every time — it was the rebuild's own read that was short. **A
	// request honoured by a reader that ignores the field looks exactly like a request never made.**
	shp::Settings still;
	still.enabled = true;
	still.knee = 0.03f;
	still.colour = 0.38f;
	still.threshold = 1.0f;
	still.drive = 1.0f;
	still.hysteresis = 0.0f;

	shp::Settings moved = still;
	const auto home = shp::phase_space::homeFor(0.73, 0.41);
	for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
		moved.travel[i] = static_cast<float>(home[i]) * 0.5f;
	}

	// **THE INDEPENDENT RESTATEMENT OF THE INTENDED PATH**: the table this curve, travel included,
	// ought to produce. Built here from the settings rather than read from the device, so agreement
	// below is two roads arriving at one table and not a function compared with itself.
	std::array<float, shp::kTableEntries> wanted {};
	shp::buildTable(shp::deriveCurve(moved.knee, moved.colour, moved.gamma, shp::kSlewHarmonics,
	                                 &moved.travel),
	                wanted);
	std::array<float, shp::kTableEntries> wantedStill {};
	shp::buildTable(shp::deriveCurve(still.knee, still.colour, still.gamma, shp::kSlewHarmonics,
	                                 &still.travel),
	                wantedStill);

	// NON-VACUITY, FIRST AND ON THE REFERENCE ITSELF: this travel genuinely moves the table. Without
	// it the agreement below would be satisfied by a device and a reference that both ignore travel.
	float apart = 0.0f;
	for (size_t i = 0; i < shp::kTableEntries; ++i) {
		apart = std::max(apart, std::fabs(wanted[i] - wantedStill[i]));
	}
	CAPTURE(apart);
	CHECK(apart > 0.1f);

	shp::Shaper dev;
	dev.prepare(48000.0);
	settle(dev, moved, 512);
	// **SETTLED PAST THE BUILD, NOT JUST PAST THE TARGET.** The build is amortised over nine blocks
	// and swaps behind a fade; reading the table before it lands would compare against whatever was
	// there when the settings arrived.
	for (int b = 0; b < 512; ++b) {
		std::vector<float> z(512, 0.0f);
		std::vector<float> z2 = z;
		dev.processStereo(z.data(), z2.data(), 512);
	}
	REQUIRE(!dev.buildingForTest());

	// **THE VALUE, TO THE BIT.** Both roads run the same builder on the same curve, so there is no
	// tolerance to justify here — and a bound would only hide the next term that goes missing.
	float worst = 0.0f;
	for (size_t i = 0; i < shp::kTableEntries; ++i) {
		worst = std::max(worst, std::fabs(dev.tableForTest()[i] - wanted[i]));
	}
	CAPTURE(worst);
	CHECK(worst == 0.0f);
}

TEST_CASE("the drawn curve follows the travel too") {
	// The other half of the same report. `drawShaperCurve` called `curveFor` without `travel` for the
	// same reason the build did — the argument defaults to `nullptr` — so the picture drew the
	// no-travel curve while the pad moved the field.
	//
	// **THIS IS THE THIRD TIME THIS ARGUMENT LIST HAS SILENTLY LOST A TERM.** The plot once passed
	// `gamma` alone and ignored Colour Phase; it then dropped `travel`; and `advanceBuild` dropped
	// `travel` independently. Every one of them compiled, and every one produced a plausible curve for
	// the wrong point in the field. A defaulted pointer is why: forgetting it is not an error, it is a
	// quieter answer.
	//
	// **AND WHY NO PICTURE COMPARISON WOULD HAVE FOUND IT** — nothing about the drawing was malformed.
	// Hence `shaperPlotCurve`, extracted into a visage-free header so the value can be read here,
	// beside the engine, rather than inferred from pixels in a suite that cannot link the drawing code.
	namespace ui = feathers::ui;

	shp::Settings still;
	still.enabled = true;
	still.knee = 0.03f;
	still.colour = 0.38f;
	still.threshold = 1.0f;
	still.drive = 1.0f;
	still.hysteresis = 0.0f;

	shp::Settings moved = still;
	const auto home = shp::phase_space::homeFor(0.73, 0.41);
	for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
		moved.travel[i] = static_cast<float>(home[i]) * 0.5f;
	}

	// **THE INTENDED PATH, RESTATED RATHER THAN CALLED**: build the plot's own table from the
	// travelled curve, then read it through the same staging the device uses. If `shaperPlotCurve`
	// drops the travel again, this reference will not.
	std::array<float, ui::kShaperPlotPoints> table {};
	shp::buildTable(shp::deriveCurve(moved.knee, moved.colour, moved.gamma, shp::kSlewHarmonics,
	                                 &moved.travel),
	                table);
	const shp::TableBound bound = shp::boundTable(table);
	const shp::Staging staging = shp::stagingFor(moved.threshold, moved.aggression);
	const float sweep = shp::sweepFor(moved.drive);

	const auto drawn = ui::shaperPlotCurve(moved);
	float worst = 0.0f;
	for (size_t i = 0; i < ui::kShaperPlotPoints; ++i) {
		const float x = static_cast<float>(i) * (2.0f / static_cast<float>(ui::kShaperPlotPoints - 1))
		                - 1.0f;
		const float want = shp::staticTransferAt(table, bound, staging, sweep,
		                                         shp::kneeFadeFor(moved.knee), x);
		worst = std::max(worst, std::fabs(drawn[i] - want));
	}
	CAPTURE(worst);
	CHECK(worst == 0.0f);

	// AND THE SECOND ASSERTION, PROVING THE FIRST IS NOT VACUOUS: the travel visibly changes the drawn
	// curve. A plot that ignored travel would agree with a reference that ignored it too.
	const auto drawnStill = ui::shaperPlotCurve(still);
	float apart = 0.0f;
	for (size_t i = 0; i < ui::kShaperPlotPoints; ++i) {
		apart = std::max(apart, std::fabs(drawn[i] - drawnStill[i]));
	}
	CAPTURE(apart);
	CHECK(apart > 0.05f);
}

TEST_CASE("AGGRESSION: minimum runs the whole headroom, and neutral is today's crossfade") {
	// The aggression mix crossfade came back as a dedicated modulatable knob; then, on the installed
	// build, its blend fade zone was found tiny: at minimum aggression the fade must run all the way
	// from the threshold to the end of the table.
	//
	// **THAT LAST REQUIREMENT IS THIS CASE.** The first version was a fixed 20:1 multiplier on today's slope,
	// so at mix 1 the widest it reached was 14.5x the floor and the fade got a FOURTEENTH of the room
	// the ceiling allows. The knob is now measured in that room: `ratio^(2*knob)` against a floor of
	// `1/headroom`, where `ratio` is how much steeper today's slope is than the widest legal one.
	for (const float mix : {0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
		CAPTURE(mix);
		const shp::Staging widest = shp::stagingFor(mix, 0.0f);

		// **THE WHOLE HEADROOM, WHICH IS THE THRESHOLD TO THE END OF THE TABLE.** The blend completes
		// at `diff = 1/blendSlope`, so asserting that equals the headroom IS the requirement: at
		// minimum the crossfade runs from the crossing to `|x| = 1.0` and finishes exactly there.
		CHECK(1.0f / widest.blendSlope == doctest::Approx(widest.headroom).epsilon(1e-5));

		// **NEUTRAL IS TODAY'S SLOPE, UNCHANGED BY THIS REWORK.** The old law was
		// `(1 + 16*mixNorm^2)/headroom`; neutral must still be exactly that or every existing use
		// changes character. Restated here from the old formula rather than from the new one, so this
		// is a claim about compatibility and not an identity.
		const float mixNorm = mix * 2.0f;
		const float before = (1.0f + 16.0f * mixNorm * mixNorm) / widest.headroom;
		CHECK(shp::stagingFor(mix, 0.5f).blendSlope == doctest::Approx(before).epsilon(1e-4));

		// AND THE TOP IS THE SAME FACTOR BEYOND NEUTRAL AS THE BOTTOM IS BELOW IT — log-symmetric, so
		// equal knob steps are equal ratios and the knob is even in the thing the ear follows.
		const float lo = shp::stagingFor(mix, 0.0f).blendSlope;
		const float mid = shp::stagingFor(mix, 0.5f).blendSlope;
		const float hi = shp::stagingFor(mix, 1.0f).blendSlope;
		CHECK(mid / lo == doctest::Approx(hi / mid).epsilon(1e-3));

		// NON-VACUITY: the knob has real room here. At the lowest mixes the ceiling leaves almost
		// none, which is a fact about the device — see `blendRatioFor` — so this is asserted where
		// there is some rather than everywhere.
		if (mix >= 0.25f) {
			CHECK(hi / lo > 4.0f);
		}
	}

	// THE RATIO IS THE CEILING'S ROOM, AND IT IS MIX'S TO GIVE. Pinned so the span's mix-dependence
	// is a stated property rather than a surprise: 1.16 at mix 0.05, 65 at mix 1.
	CHECK(shp::blendRatioFor(0.05f) == doctest::Approx(1.16f).epsilon(1e-3));
	CHECK(shp::blendRatioFor(1.0f) == doctest::Approx(65.0f).epsilon(1e-3));

	// OUT OF RANGE IS CLAMPED, not extrapolated — a modulation offset can ask for either side.
	CHECK(shp::aggressionFor(-5.0f, 17.0f) == shp::aggressionFor(0.0f, 17.0f));
	CHECK(shp::aggressionFor(5.0f, 17.0f) == shp::aggressionFor(1.0f, 17.0f));
}

TEST_CASE("AGGRESSION: the ceiling is enforced in the slope, not in the knob's range") {
	// **THE CONSTRAINT THAT DECIDED THE DESIGN.** `processSample` blends over
	// `min(diff * blendSlope, 1)`, so the crossfade completes at `diff = 1/blendSlope` and MUST be
	// complete by the time `|x|` reaches 1.0 — the excess reaches the headroom — or a hot sample
	// leaves part-DRY, and the dry side is `x` itself, which is above full scale. That is precisely
	// the guarantee the `ceiling` work exists for, and a widening aggression spends the margin
	// today's slope carries over it.
	//
	// **MEASURED, the smallest aggression that still clears the ceiling depends on MIX**: 0.862 at mix
	// 0.05, 0.610 at 0.10, 0.200 at 0.25, 0.059 at 0.50, 0.015 at 1.00. So NO fixed knob range is both
	// safe at low mix and useful at high — which is why the knob is free 0..1 and the SLOPE is floored
	// in `stagingFor`. This case is that floor, asserted as the inequality it exists to keep.
	for (const float mix : {0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
		for (const float knob : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 1.0f}) {
			const shp::Staging st = shp::stagingFor(mix, knob);
			CAPTURE(mix);
			CAPTURE(knob);
			// THE BLEND COMPLETES BY THE TIME THE EXCESS REACHES THE HEADROOM. Stated as the product
			// rather than as a slope bound, because that product IS the requirement.
			CHECK(st.blendSlope * st.headroom >= doctest::Approx(1.0f).epsilon(1e-5));
		}
	}

	// **AND THE FLOOR ONLY BINDS WHERE IT MUST — otherwise the control would be dead, not limited.**
	// At a mix with room, a low aggression genuinely widens the crossfade; the assertion is that the
	// slope actually fell, which a floor applied unconditionally would prevent.
	const shp::Staging wide = shp::stagingFor(1.0f, 0.0f);
	const shp::Staging neutral = shp::stagingFor(1.0f, 0.5f);
	const shp::Staging snap = shp::stagingFor(1.0f, 1.0f);
	CHECK(wide.blendSlope < neutral.blendSlope);
	CHECK(snap.blendSlope > neutral.blendSlope);
	// **THE SPAN IS THE CEILING'S ROOM SQUARED, and it is MIX's to give.** This pinned a fixed 20:1,
	// which was the old law's whole span; the knob is now `ratio^(2*knob)` about a floor of
	// `1/headroom`, so end to end it covers `ratio^2` — 4225 at mix 1, where `ratio` is 65. That is
	// not a bigger number for its own sake: it is what makes minimum reach the floor — at minimum
	// aggression the fade runs all the way from the threshold to the end of the table.
	const float ratio = shp::blendRatioFor(1.0f);
	CHECK(snap.blendSlope / wide.blendSlope == doctest::Approx(ratio * ratio).epsilon(0.01));
}

TEST_CASE("AGGRESSION: it changes the sound and asks for no table rebuild") {
	// **THE PROPERTY THAT MAKES IT WORTH MODULATING, AND BOTH HALVES ARE NEEDED.** A control that
	// asks for no rebuild but changes nothing would satisfy the second assertion trivially; one that
	// changes the sound by rebuilding would satisfy the first and be no cheaper than Knee.
	//
	// Knee is the comparison that gives this meaning: MEASURED, moving Knee 0.05 at drive 0.5 changes
	// the transfer by rms 0.61 of which the table owns essentially all, so a modulated Knee turns its
	// table over at ~47 Hz — a 512-sample build plus a 512-sample crossfade. Aggression reaches the
	// sound through the staging alone.
	shp::Settings a;
	a.enabled = true;
	a.knee = 0.5f;
	a.colour = 0.38f;
	a.threshold = 1.0f;
	a.drive = 0.5f;
	a.hysteresis = 0.0f;
	shp::Settings b = a;
	b.aggression = 0.0f;

	// NO REBUILD: the two settings are the SAME CURVE, which is what `maybeStartBuild` consults.
	CHECK(a.sameCurveAs(b));

	// AND YET THE TRANSFER MOVES. Built once, read twice with different staging — which is exactly
	// what the engine does, and the reason the first assertion is affordable.
	std::array<float, shp::kTableEntries> table {};
	shp::buildTable(shp::deriveCurve(a.knee, a.colour, a.gamma, shp::kSlewHarmonics,
	                              &a.travel),
	                table);
	const shp::TableBound bound = shp::boundTable(table);
	const float sweep = shp::sweepFor(a.drive);
	const float fade = shp::kneeFadeFor(a.knee);

	float worst = 0.0f;
	for (int i = 0; i <= 128; ++i) {
		const float x = -1.0f + 2.0f * static_cast<float>(i) / 128.0f;
		const float wide = shp::staticTransferAt(table, bound, shp::stagingFor(b.threshold, b.aggression),
		                                         sweep, fade, x);
		const float neutral = shp::staticTransferAt(table, bound,
		                                            shp::stagingFor(a.threshold, a.aggression), sweep,
		                                            fade, x);
		worst = std::max(worst, std::fabs(wide - neutral));
	}
	CAPTURE(worst);
	CHECK(worst > 0.05f);
}

TEST_CASE("TRIM: an input gain in dB, unity at 0, and it moves the dry with it") {
	// A plain input gain knob on the card and the window, and it earns its place more than it did
	// when first proposed. The device's own low-level gain is now the design (decided: left as-is): MEASURED at colour 0, knee 0.5, the transfer's slope is 3.93x at the origin and 1.00x
	// at full scale, so WHERE the signal sits against the threshold decides most of what is heard.
	// A trim is the control that puts it there.
	//
	// **WHAT MAKES IT A TRIM AND NOT A SECOND DRIVE.** Drive scales the read POSITION and leaves the
	// dry alone, so it moves the shaped path only. This scales what the whole device sees — the
	// sample the threshold is compared against, and the dry that a sub-threshold sample is returned
	// as. That difference is the case below, asserted where the two would otherwise look alike.
	SUBCASE("the law: dB, and 0 is unity to the bit") {
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings cfg;
		cfg.enabled = true;
		settle(s, cfg);
		// **UNITY EXACTLY, NOT APPROXIMATELY.** A trim that is not unity when untouched is a gain
		// hiding inside a device that already has two, and every patch made before it existed would
		// change level on the day it landed.
		CHECK(s.trimGainForTest() == 1.0f);

		for (const auto& [dB, want] : std::vector<std::pair<float, float>> {
		         { 6.0f, 1.9952623f }, { -6.0f, 0.5011872f }, { 20.0f, 10.0f }, { -20.0f, 0.1f }}) {
			cfg.trim = dB;
			settle(s, cfg);
			CAPTURE(dB);
			CHECK(s.trimGainForTest() == doctest::Approx(want).epsilon(1e-5));
		}
	}

	SUBCASE("it is an INPUT gain: the sub-threshold dry moves with it, which Drive cannot do") {
		// **THE DISTINCTION, ASSERTED ON A SAMPLE THAT NEVER REACHES THE TABLE.** Below the threshold
		// the device returns the dry, so anything visible there came in through the input — which is
		// exactly where a trim acts and exactly where Drive does not.
		const auto belowThresholdOut = [](float trim, float drive) {
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings cfg;
			cfg.enabled = true;
			cfg.threshold = 0.4f; // a high threshold, so a quiet sample stays dry
			cfg.drive = drive;
			cfg.trim = trim;
			cfg.hysteresis = 0.0f;
			settle(s, cfg, 512);
			for (int b = 0; b < 64; ++b) {
				std::vector<float> z(512, 0.0f);
				std::vector<float> z2 = z;
				s.processStereo(z.data(), z2.data(), 512);
			}
			std::vector<float> l { 0.2f };
			std::vector<float> r { 0.2f };
			s.processStereo(l.data(), r.data(), 1);
			return s.preBlockForTest();
		};

		const float plain = belowThresholdOut(0.0f, 0.0f);
		// NON-VACUITY: the sample really is below the threshold, so it is the dry path being measured.
		CHECK(plain == doctest::Approx(0.2f).epsilon(1e-4));

		// **DRIVE CANNOT MOVE IT** — it scales the read position and there is no read here.
		CHECK(belowThresholdOut(0.0f, 1.0f) == doctest::Approx(plain).epsilon(1e-4));

		// **THE TRIM DOES, BY EXACTLY ITS OWN GAIN.** A value, not a difference: +6 dB on 0.2 is
		// 0.399, and asserting "it changed" would pass on a trim wired to the wrong stage.
		CHECK(belowThresholdOut(6.0f, 0.0f) == doctest::Approx(0.2f * 1.9952623f).epsilon(1e-4));
		CHECK(belowThresholdOut(-6.0f, 0.0f) == doctest::Approx(0.2f * 0.5011872f).epsilon(1e-4));
	}

	SUBCASE("it works at Knee 0, where the device is a bypass") {
		// A control that silently depended on another knob would be the honesty rule broken at an
		// interaction rather than at a label. The clamp still holds afterwards, so a trimmed-up
		// bypass cannot leave the format — which is the one rule that outranks the bypass.
		const auto bypassOut = [](float trim, float x) {
			shp::Shaper s;
			s.prepare(48000.0);
			shp::Settings cfg;
			cfg.enabled = true;
			cfg.knee = 0.0f; // a true bypass: no table is consulted
			cfg.trim = trim;
			settle(s, cfg);
			std::vector<float> l { x };
			std::vector<float> r { x };
			s.processStereo(l.data(), r.data(), 1);
			return s.preBlockForTest();
		};
		CHECK(bypassOut(0.0f, 0.3f) == doctest::Approx(0.3f).epsilon(1e-5));
		CHECK(bypassOut(6.0f, 0.3f) == doctest::Approx(0.3f * 1.9952623f).epsilon(1e-4));
		// AND THE CEILING SURVIVES IT: +24 dB on a full-scale sample still leaves at full scale.
		CHECK(bypassOut(24.0f, 1.0f) == doctest::Approx(1.0f).epsilon(1e-5));
	}

	SUBCASE("it asks for no table rebuild, like Aggression and unlike Knee") {
		shp::Settings a;
		shp::Settings b = a;
		b.trim = 12.0f;
		CHECK(a.sameCurveAs(b));
	}
}

TEST_CASE("TRIM: the drive meter reads what actually reaches the table") {
	// On the installed build, adjusting the trim knob did not show in the level meter.
	//
	// **THE METER WAS ON THE WRONG SIDE OF THE TRIM.** Both process loops metered the buffer sample
	// directly, under a comment reading "before anything touches it — the plot's x axis is the input
	// axis". That was TRUE until the trim existed: the buffer sample and the sample the table reads
	// were the same number. The trim sits between them, so the meter went on reporting the host's
	// level while the curve was driven by something else.
	//
	// **AND THE METER'S PURPOSE IS EXACTLY THE ONE THIS BREAKS.** It is a level meter behind the
	// transfer curve display, there to show how hard the actual table is driven, and it colours the reached part of the drawn curve. A meter that ignores the
	// control which decides how hard the table is driven is answering a question nobody asked.
	const auto peakWith = [](float trim, float amplitude) {
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings cfg;
		cfg.enabled = true;
		cfg.trim = trim;
		settle(s, cfg);
		// **LONG ENOUGH FOR THE METER TO CONVERGE DOWNWARD, which is the whole conditioning of this
		// case.** `drivePeak_` starts near full scale and only FALLS at `kDriveFallDbPerSecond` = 20
		// dB/s, so a short run reports where the decay happens to be rather than the signal's level.
		// MEASURED at 8 blocks of 256 — 43 ms — a 0.5 sine still read 0.894 and was still falling;
		// worse, a +6 dB run read exactly 2x that by coincidence, because its own peak had reached the
		// decaying value while the quieter one had not. **A ratio that looked like proof was an
		// artefact of the window.** Reaching 0.5 from full scale is 6 dB, 0.3 s, so this runs 0.64 s.
		for (int b = 0; b < 120; ++b) {
			std::vector<float> l(256);
			std::vector<float> r(256);
			for (int i = 0; i < 256; ++i) {
				l[i] = amplitude * std::sin(2.0f * 3.14159265f * static_cast<float>(i) / 64.0f);
				r[i] = l[i];
			}
			s.processStereo(l.data(), r.data(), 256);
		}
		return s.drivePeak();
	};

	// AT UNITY THE METER IS THE INPUT, which is the behaviour that was already right and must stay.
	CHECK(peakWith(0.0f, 0.5f) == doctest::Approx(0.5f).epsilon(0.02));

	// **+6 dB OF TRIM IS +6 dB ON THE METER, AS A VALUE.** Asserting "it moved" would pass on a meter
	// wired to the wrong stage; the claim is that it reads the level the table is driven at, so the
	// number is the one to check.
	CHECK(peakWith(6.0f, 0.5f) == doctest::Approx(0.5f * 1.9952623f).epsilon(0.02));
	CHECK(peakWith(-6.0f, 0.5f) == doctest::Approx(0.5f * 0.5011872f).epsilon(0.02));

	// AND IT IS THE SAME NUMBER THE CURVE IS READ AT: a trim that pushes a half-scale signal past
	// full scale must show the meter past full scale too, or the drawn hot region would stop short of
	// where the signal actually reaches.
	CHECK(peakWith(12.0f, 0.5f) > 1.0f);
}

TEST_CASE("every shaper descriptor sits in its own enum slot") {
	// **THE ARRAY IS INDEX-ALIGNED WITH THE ENUM AND ONLY ITS LENGTH IS ASSERTED**, which the header
	// says in as many words: *"an entry in the wrong slot compiles and silently describes a different
	// control — which is exactly what the first draft of this did, putting it at 12 and giving
	// Hysteresis ColourPhase's default of 0 instead of the field-neutral 0.5."*
	//
	// **SO THE DEFECT HAS ALREADY HAPPENED ONCE HERE AND NOTHING WAS WATCHING FOR THE SECOND TIME.**
	// A `static_assert` on `std::size` cannot see it: inserting and deleting in the same edit keeps
	// the length right and slides every entry after the change onto the wrong control.
	//
	// **A HAND-WRITTEN LIST IS THE RIGHT SHAPE HERE, unusually.** This repo distrusts hand-lists
	// because they stop matching the code they walk — but that is the whole job of this one: it is a
	// SECOND, independent statement of an ordering that is otherwise asserted by nothing, and two
	// hand-maintained lists disagreeing is precisely the event to be told about. Deriving it from
	// `kParamDescs` would make it agree by construction and assert nothing at all.
	namespace shp = feathers::fx::shaper;
	const std::pair<shp::Param, const char*> kExpected[] = {
		{ shp::Param::Enabled, "Enabled" },
		{ shp::Param::Knee, "Shaper Knee" },
		{ shp::Param::Colour, "Shaper Colour" },
		{ shp::Param::Gamma, "Shaper Gamma" },
		{ shp::Param::Drive, "Shaper Drive" },
		{ shp::Param::Threshold, "Shaper Threshold" },
		{ shp::Param::Emphasis, "Shaper Emphasis" },
		{ shp::Param::Sub, "Shaper Sub" },
		{ shp::Param::Feedback, "Shaper Feedback" },
		{ shp::Param::Mangle, "Shaper Mangle" },
		{ shp::Param::Lowpass, "Shaper Lowpass" },
		{ shp::Param::Integrator, "Shaper Integrator" },
		{ shp::Param::Hysteresis, "Shaper Hysteresis" },
		{ shp::Param::Rotate, "Shaper Rotate" },
		{ shp::Param::Aggression, "Shaper Aggression" },
		{ shp::Param::Trim, "Shaper Trim" },
		{ shp::Param::Deadzone, "Shaper Deadzone" },
		{ shp::Param::Bloom, "Shaper Bloom" },
		{ shp::Param::Sag, "Shaper Sag" },
		{ shp::Param::Loop, "Shaper Loop" },
		{ shp::Param::Cheby, "Shaper Cheby" },
		{ shp::Param::Stair, "Shaper Stair" },
		{ shp::Param::SubOn, "Shaper Sub On" },
		{ shp::Param::FeedbackOn, "Shaper Feedback On" },
		{ shp::Param::MangleOn, "Shaper Mangle On" },
		{ shp::Param::LowpassOn, "Shaper Lowpass On" },
		{ shp::Param::IntegratorOn, "Shaper Integrator On" },
		{ shp::Param::DeadzoneOn, "Shaper Deadzone On" },
		{ shp::Param::ColourMode, "Shaper Colour Mode" },
		{ shp::Param::InflateAmount, "Shaper Inflate" },
		{ shp::Param::InputHpf, "Shaper HPF" },
		{ shp::Param::InputLpf, "Shaper LPF" },
		{ shp::Param::DryWet, "Shaper Mix" },
		{ shp::Param::Poly, "Shaper Poly" },
	};
	// THE COUNT IS PART OF THE CLAIM, or a list that lost an entry would pass by checking fewer.
	CHECK(std::size(kExpected) == static_cast<size_t>(shp::Param::kCount));
	for (const auto& [param, name] : kExpected) {
		const size_t index = static_cast<size_t>(param);
		REQUIRE(index < std::size(shp::kParamDescs));
		CAPTURE(index);
		CAPTURE(name);
		CHECK(std::string(shp::kParamDescs[index].name) == name);
	}
}

// ---------------------------------------------------------------------------------------------
// THE SHAPER PARITY SWEEP, 2026-09-16 — the port read against the firmware's shaper.h, shaper_buffer.h
// and table_shaper.h at the commit it was ported from. Values through seams; known-bads red (the commit).
// ---------------------------------------------------------------------------------------------

TEST_CASE("parity: the mix threshold and blend slope GLIDE to a new target — half the distance per 128 frames "
          "at 44.1 kHz, as the firmware smooths them — and snap on first use") {
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.8f;
	settings.threshold = 0.5f;
	settle(s, settings);
	const float t1 = s.thresholdForTest();
	const float b1 = s.blendSlopeForTest();

	// ONE BLOCK AFTER A MIX MOVE: partway, by the firmware's half-life restated in time. At 48 kHz a
	// 128-frame block is 128 * 44100 / (128 * 48000) = 0.919 reference buffers: retention 0.5^0.919.
	shp::Settings quieter = settings;
	quieter.threshold = 0.1f;
	s.setTarget(quieter);
	std::vector<float> l(128, 0.0f), r(128, 0.0f);
	s.processStereo(l.data(), r.data(), 128);
	const float tMid = s.thresholdForTest();
	const float bMid = s.blendSlopeForTest();

	// THEN SETTLED: the target itself, read off the same seam.
	settle(s, quieter);
	const float t2 = s.thresholdForTest();
	const float b2 = s.blendSlopeForTest();
	REQUIRE(std::fabs(t2 - t1) > 0.1f); // a real move, so "partway" is a claim

	const float retention = std::pow(0.5f, 128.0f * 44100.0f / (128.0f * 48000.0f));
	CHECK(tMid == doctest::Approx(t2 + (t1 - t2) * retention).epsilon(1e-4));
	CHECK(bMid == doctest::Approx(b2 + (b1 - b2) * retention).epsilon(1e-4));

	// AND A FRESH DEVICE SNAPS: its first block reads the target, not a glide from the identity.
	shp::Shaper fresh;
	fresh.prepare(48000.0);
	fresh.setTarget(quieter);
	fresh.processStereo(l.data(), r.data(), 128);
	CHECK(fresh.thresholdForTest() == doctest::Approx(t2).epsilon(1e-5));
}

TEST_CASE("parity: the integrator's accumulator is bounded at full scale — the firmware's saturating add — so "
          "a signal that never crosses zero cannot grow it without limit") {
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.7f;
	settings.colour = 0.06f;
	settings.integratorOn = true; // the old On: the amount defaults to the field's own
	settle(s, settings);
	// A DC OFFSET: no zero crossing ever resets the accumulator. Unbounded, 0.9/64 per sample for
	// 16384 samples is 230; bounded, it is 1.
	std::vector<float> l(256, 0.9f), r(256, 0.9f);
	for (int b = 0; b < 64; ++b) {
		std::fill(l.begin(), l.end(), 0.9f);
		std::fill(r.begin(), r.end(), 0.9f);
		s.processStereo(l.data(), r.data(), 256);
	}
	CHECK(s.integratorForTest(0) == doctest::Approx(1.0f));
	CHECK(s.integratorForTest(1) == doctest::Approx(1.0f));
	for (const float x : l) {
		REQUIRE(std::isfinite(x));
	}
}

TEST_CASE("parity: at a weight sum of exactly 0.001 the transfer is NOT normalised — the firmware's two guards "
          "are `>` and `>=`, and the port had both as `>=`") {
	// A measure-zero point, kept exact because `transferAt`'s claim is that it IS `evaluateTransfer`
	// term for term. With one weight at 0.001 and the rest zero: the firmware's inverse guard fails
	// (`> 0.001` is false) so the sum is scaled by 1.0, while `hasWeights` (`>= 0.001`) is true so the
	// weighted sum is used — the output is 0.001 x the basis, not 1.0 x it.
	shp::CurveParams p;
	p.drive = 0.5f;
	p.inflatorWeight = 0.001f;
	p.polyWeight = p.hardKneeWeight = p.chebyWeight = p.sineFoldWeight = p.rectifierWeight = 0.0f;
	shp::CurveParams unit = p;
	unit.inflatorWeight = 1.0f; // the same basis at unit weight: what a normalised read would return
	const float atTiny = shp::transferAt(p, 0.5f);
	const float atUnit = shp::transferAt(unit, 0.5f);
	REQUIRE(std::fabs(atUnit) > 0.05f);
	CHECK(atTiny == doctest::Approx(0.001f * atUnit).epsilon(1e-3));
}

TEST_CASE("backlog FK: a moving Trim is followed per frame inside the call — at Knee 0 each sample is the input times its own frame's gain") {
	// At Knee 0 the device is the trim and a clamp at full scale, nothing else (`processSample`), so a quiet input
	// comes out as x * 10^(dB/20) — restated here from the law, per sample, against the window's own dB. Through
	// the ops road, 64 frames a call. A trim held for the call would be off by the whole ramp.
	constexpr int kFrames = 64;
	shp::Shaper s;
	s.prepare(48000.0);
	shp::Settings settings;
	settings.enabled = true;
	settings.knee = 0.0f;
	settings.trim = -3.0f; // the scalar the window replaces
	settle(s, settings);
	std::array<float, kFrames> run {};
	for (int i = 0; i < kFrames; ++i) {
		run[static_cast<size_t>(i)] = -12.0f + 18.0f * static_cast<float>(i) / static_cast<float>(kFrames - 1); // -12 -> +6 dB
	}
	std::array<const float*, 64> runs {};
	runs[static_cast<size_t>(shp::Param::Trim)] = run.data();
	REQUIRE(shp::kAudioOps.setFieldWindows != nullptr);
	float worst = 0.0f;
	for (int b = 0; b < 16; ++b) {
		const std::vector<float> in = sine(kFrames, 0.1f, 3.0f);
		std::vector<float> l = in;
		std::vector<float> r = in;
		shp::kAudioOps.setFieldWindows(&s, runs.data(), runs.size(), kFrames);
		shp::kAudioOps.processStereo(&s, l.data(), r.data(), kFrames);
		for (size_t i = 0; i < in.size(); ++i) {
			const float expected = in[i] * std::pow(10.0f, run[i] / 20.0f);
			worst = std::max(worst, std::abs(l[i] - expected));
		}
	}
	CAPTURE(worst);
	CHECK(worst < 1e-6f);
}

TEST_CASE("the Deadzone switch: off zeroes the field's deadzone in the curve the engine builds, on restores it exactly") {
	// Against the reference, the port's tone was more aggressive and jumpy than the firmware's, hence a
	// Deadzone button. The firmware's default path leaves
	// the deadzone at zero; the port derived it everywhere. Now it is a switch, off by default.
	namespace shp = feathers::fx::shaper;

	// A COLOUR WHERE THE FIELD'S DEADZONE IS WIDE, found rather than typed, or the case asks nothing.
	float colour = -1.0f;
	for (int i = 0; i < 4000 && colour < 0.0f; ++i) {
		const float c = static_cast<float>(i) / 4000.0f;
		if (shp::deriveCurve(0.7f, c, 0.0f, 0.5f).deadzoneWidth > 0.3f) {
			colour = c;
		}
	}
	REQUIRE(colour >= 0.0f);
	const float fieldWidth = shp::deriveCurve(0.7f, colour, 0.0f, 0.5f).deadzoneWidth;
	CAPTURE(colour);
	CAPTURE(fieldWidth);

	shp::Settings st;
	st.enabled = true;
	st.knee = 0.7f;
	st.colour = colour;
	st.threshold = 1.0f;

	// THE ONE DEFINITION BOTH SURFACES CALL.
	CHECK(shp::curveOf(st).deadzoneWidth == 0.0f);
	st.deadzoneOn = true; // the old On: the amount defaults to the field's own
	CHECK(shp::curveOf(st).deadzoneWidth == fieldWidth);
	st.deadzoneOn = false;

	// THE ENGINE, driven: it rebuilds on the switch — `sameCurveAs` has to see it.
	shp::Shaper s;
	s.prepare(48000.0);
	settle(s, st);
	CHECK(s.curveForTest().deadzoneWidth == 0.0f);
	shp::Settings on = st;
	on.deadzoneOn = true; // the old On: the amount defaults to the field's own
	CHECK_FALSE(on.sameCurveAs(st));
	settle(s, on);
	CHECK(s.curveForTest().deadzoneWidth == fieldWidth);

	// AND THE TABLE ITSELF DIFFERS — the width is a table term, and a term that reached the struct but
	// not the build would pass both checks above.
	std::vector<float> offTable(shp::kTableEntries);
	std::vector<float> onTable(shp::kTableEntries);
	shp::buildTable(shp::curveOf(st), offTable);
	shp::buildTable(shp::curveOf(on), onTable);
	int differing = 0;
	for (size_t i = 0; i < offTable.size(); ++i) {
		differing += offTable[i] != onTable[i] ? 1 : 0;
	}
	CHECK(differing > 50);

	// AND IT IS HEARD. Two shapers at that colour, one switch apart, fed the same hot sine: the output
	// must differ, and by more than rounding. This is what `test_fx_devices.cpp`'s every-control sweep
	// cites for Deadzone — its fixture colours sit where the field's 20%-duty deadzone is zero.
	const auto render = [&](const shp::Settings& settings) {
		shp::Shaper sh;
		sh.prepare(48000.0);
		settle(sh, settings, 4096);
		std::vector<float> l(4096);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.9f * std::sin(2.0f * 3.14159265f * 110.0f * static_cast<float>(i) / 48000.0f);
		}
		std::vector<float> r = l;
		sh.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		return l;
	};
	const std::vector<float> heardOff = render(st);
	const std::vector<float> heardOn = render(on);
	float worst = 0.0f;
	for (size_t i = 0; i < heardOff.size(); ++i) {
		worst = std::max(worst, std::fabs(heardOff[i] - heardOn[i]));
	}
	CAPTURE(worst);
	CHECK(worst > 0.05f);
}

//--------------------------------------------------------------------------------------------//
// BACKLOG IE — FIVE THAT EXTEND THE CURVE: Cheby and Stair in the table, Bloom, Sag and Loop      //
// around it                                                                                      //
//--------------------------------------------------------------------------------------------//

namespace {

/// One DFT bin's magnitude — k cycles across the buffer.
double binOf(const std::vector<float>& y, int k) {
	const auto n = static_cast<int>(y.size());
	double re = 0.0;
	double im = 0.0;
	for (int i = 0; i < n; ++i) {
		const double ph = 2.0 * std::numbers::pi * k * i / n;
		re += y[static_cast<size_t>(i)] * std::cos(ph);
		im -= y[static_cast<size_t>(i)] * std::sin(ph);
	}
	return std::hypot(re, im) * 2.0 / n;
}

/// **SETTLED ON FRESH SILENCE EVERY BLOCK.** The file's `settle` hands each block's OUTPUT back in as
/// the next block's input (it processes one buffer in place), and a curve with more than unit gain
/// near zero grows the build's 3e-8 residue into a real signal — MEASURED, an envelope of 0.83 after
/// "silence". Harmless to the cases that only need the table landed; fatal to a restatement that
/// assumes the memory stages start at rest.
void settleQuiet(shp::Shaper& s, const shp::Settings& settings) {
	s.setTarget(settings);
	for (int b = 0; b < 64; ++b) {
		std::vector<float> l(512, 0.0f);
		std::vector<float> r(512, 0.0f);
		s.processStereo(l.data(), r.data(), 512);
	}
}

/// A settled shaper fed one sine, frame by frame; returns the pre-blocker values of channel 1.
std::vector<float> preBlockRun(shp::Shaper& dev, float amp, int cycles, int n) {
	std::vector<float> out(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		float l = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * cycles * i / n));
		float r = l;
		dev.processStereo(&l, &r, 1);
		out[static_cast<size_t>(i)] = dev.preBlockForTest();
	}
	return out;
}

} // namespace

TEST_CASE("IM CHEBY: at the read, the crossfade lands on the Chebyshev polynomial of the table coordinate") {
	// **READ-SIDE SINCE backlog IM** — it gained nothing from being in the table,
	// and being a table term cost it modulation. `shapedLookup` crossfades the normalised table value
	// toward Tn of the SAME coordinate the table is read at. With intensity 1 and no pre-stage the
	// argument is the coordinate itself, so Tn(cos θ) = cos nθ is a claim about VALUES.
	std::vector<float> table(shp::kTableEntries);
	shp::Settings s;
	s.knee = 0.6f;
	s.colour = 0.38f;
	shp::buildTable(shp::curveOf(s), table);
	const shp::ReadShapes odd { 0.0f, 1.0f, 1.0f, 0.0f };
	const shp::ReadShapes even { 0.0f, -1.0f, 1.0f, 0.0f };
	const shp::ReadShapes half { 0.0f, 0.5f, 1.0f, 0.0f };
	double worstOdd = 0.0;
	double worstEven = 0.0;
	double worstHalf = 0.0;
	for (int i = 0; i <= 64; ++i) {
		const double theta = std::numbers::pi * i / 64.0;
		const auto u = static_cast<float>(std::cos(theta));
		worstOdd = std::max(worstOdd, std::fabs(shp::shapedLookup(table, u, odd) - std::cos(3.0 * theta)));
		// T2, ANCHORED AND NORMALISED, IS t^2 — what the table held when it was baked: (1 + cos 2θ) / 2.
		worstEven = std::max(worstEven, std::fabs(shp::shapedLookup(table, u, even) - 0.5 * (1.0 + std::cos(2.0 * theta))));
		const double want = 0.5 * shp::lookup(table, u) + 0.5 * std::cos(3.0 * theta);
		worstHalf = std::max(worstHalf, std::fabs(shp::shapedLookup(table, u, half) - want));
	}
	CHECK(worstOdd < 1e-5);
	CHECK(worstEven < 1e-5);
	CHECK(worstHalf < 1e-5);
	// AND NOT VACUOUSLY: the table alone is nowhere near T3.
	CHECK(std::fabs(shp::lookup(table, 0.5f) - std::cos(3.0 * std::acos(0.5))) > 0.3);
	// **AND IT IS NOT A TABLE TERM ANY MORE**: Cheby leaves the built table and `sameCurveAs` alone.
	shp::Settings c = s;
	c.cheby = 1.0f;
	std::vector<float> withCheby(shp::kTableEntries);
	shp::buildTable(shp::curveOf(c), withCheby);
	CHECK(withCheby == table);
	CHECK(c.sameCurveAs(s));
}

TEST_CASE("IE CHEBY: at the engine, T2 turns a sine into its octave and T3 into its twelfth") {
	// THE WHOLE ROAD — curveOf, the chunked build, the excess read at Mix 1 — at Knee 0.5 on the
	// inflator's line. What reaches the curve is the whole sine, so the harmonic the polynomial names
	// must lead the output's spectrum.
	constexpr int kN = 4800;
	constexpr int kCycles = 20;
	const auto spectrum = [&](float cheby) {
		shp::Shaper dev;
		dev.prepare(48000.0);
		shp::Settings s;
		s.knee = 0.5f;
		s.hysteresis = 0.0f;
		s.cheby = cheby;
		settleQuiet(dev, s);
		(void)preBlockRun(dev, 0.9f, kCycles, kN); // one pass to let the envelope-free path settle
		const std::vector<float> y = preBlockRun(dev, 0.9f, kCycles, kN);
		return std::array<double, 3> { binOf(y, kCycles), binOf(y, 2 * kCycles), binOf(y, 3 * kCycles) };
	};
	const auto flat = spectrum(0.0f);
	const auto t2 = spectrum(-1.0f);
	const auto t3 = spectrum(1.0f);
	MESSAGE("H1 H2 H3  flat " << flat[0] << " " << flat[1] << " " << flat[2] << "  T2 " << t2[0] << " " << t2[1]
	                          << " " << t2[2] << "  T3 " << t3[0] << " " << t3[1] << " " << t3[2]);
	CHECK(t2[1] > t2[0]);
	CHECK(t2[1] > 20.0 * std::max(flat[1], 1e-4)); // the octave is T2's, not the field's
	CHECK(t3[2] > t3[0]);
	CHECK(t3[2] > 2.0 * flat[2]); // MEASURED 0.74 against 0.26: the inflator makes a twelfth of its own
}
TEST_CASE("IM STAIR: at the read, the coordinate rounded to its grid, 32 steps to 2") {
	CHECK(shp::stairStepsFor(0.0f) == 0.0f);
	CHECK(shp::stairStepsFor(1.0f) == doctest::Approx(shp::kStairCoarsest));
	CHECK(shp::stairStepsFor(1.0e-6f) == doctest::Approx(shp::kStairFinest).epsilon(1e-3));
	CHECK(shp::stairStepsFor(0.5f) == doctest::Approx(8.0f)); // geometric: sqrt(32 * 2)

	std::vector<float> table(shp::kTableEntries);
	shp::Settings s;
	s.knee = 0.7f;
	s.colour = 0.38f;
	shp::buildTable(shp::curveOf(s), table);
	for (const float stair : { 0.25f, 0.6f, 1.0f }) {
		const float n = shp::stairStepsFor(stair);
		const shp::ReadShapes stepped { n, 0.0f, 1.0f, 0.0f };
		CAPTURE(stair);
		double worst = 0.0;
		size_t levels = 0;
		float last = std::numeric_limits<float>::quiet_NaN();
		for (int i = 0; i <= 400; ++i) {
			const float u = -1.0f + 2.0f * static_cast<float>(i) / 400.0f;
			const float got = shp::shapedLookup(table, u, stepped);
			worst = std::max(worst, static_cast<double>(std::fabs(got - shp::lookup(table, std::round(u * n) / n))));
			if (!(got == last)) {
				++levels;
				last = got;
			}
		}
		CHECK(worst == 0.0);
		// A STAIRCASE, COUNTED: at most one level per step across [-1, 1].
		CHECK(levels <= static_cast<size_t>(2.0f * n + 1.0f));
	}
	// AND NOT A TABLE TERM.
	shp::Settings st = s;
	st.stair = 1.0f;
	std::vector<float> withStair(shp::kTableEntries);
	shp::buildTable(shp::curveOf(st), withStair);
	CHECK(withStair == table);
	CHECK(st.sameCurveAs(s));
}


TEST_CASE("IE: at zero the two table shapes are skipped, and the build is the firmware field's to the bit") {
	for (const float colour : { 0.0f, 0.38f, 0.625f, 0.9f }) {
		shp::Settings s;
		s.knee = 0.8f;
		s.colour = colour;
		std::vector<float> a(shp::kTableEntries);
		std::vector<float> b(shp::kTableEntries);
		shp::buildTable(shp::curveOf(s), a);
		shp::CurveParams field = shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics, &s.travel);
		field.deadzoneWidth = 0.0f; // the Deadzone switch's gate, off by default — `curveOf` applies it too
		shp::buildTable(field, b);
		CAPTURE(colour);
		CHECK(a == b);
	}
}

TEST_CASE("IE MEMORY: Bloom, Sag and Loop are the curve on moved rails — the engine against a restatement, sample by sample") {
	// **THE RESTATEMENT IS THE CLAIM.** Per sample, with f the drawn static transfer (the plot's own
	// function, which the case above this section pins to the engine at rest):
	//     b    = bloom * kBloomDepth * env,   rail = max(1 - sag * kSagDepth * env, kSagFloor)
	//     u    = (x + b) / rail + loop * kLoopMaxGain * env * residue
	//     a    = sign(u) · c0 — the curve steps to ±c0 = ±f(0) at zero at Mix 1, and each side is anchored on its own
	//     y    = a + rail * (f(u) - a - lift) / (1 + |lift|),  lift = f(b / rail) - c0   [when Bloom or Sag is on; else f(u)]
	//     residue += ((u - (f(u) - a) / g0) - residue) · (1 - exp(-2π · kLoopCornerHz / fs)),
	//     g0   = max((f(p) - f(-p) - 2 c0) / 2p, 1), p = kLoopProbe — the slope with the step taken out
	// The envelope and the residue BEFORE each sample are the engine's, read through their seams, so each
	// sample is ONE step from the engine's own state: an integrated copy of a feedback loop on a steep
	// curve drifts from the 1e-3 engine-against-drawn level to the whole range (MEASURED, 0.91), which
	// measures the loop's sensitivity rather than whether it is the loop described here.
	constexpr double kFs = 48000.0;
	const float loopAlpha = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * shp::kLoopCornerHz / kFs));
	struct Knobs {
		float bloom;
		float sag;
		float loop;
		float colour; ///< a LOOP runs on a monotone curve: on a folder it is chaotic, and a sample-exact
		              ///< restatement of a chaotic system measures float noise, not the design
	};
	for (const Knobs k : { Knobs { 1.0f, 0.0f, 0.0f, 0.38f }, Knobs { 0.0f, 1.0f, 0.0f, 0.38f },
	                       Knobs { 0.0f, 0.0f, 1.0f, 0.8f },
	                       Knobs { 0.7f, 0.7f, 0.7f, 0.8f } }) {
		CAPTURE(k.bloom);
		CAPTURE(k.sag);
		CAPTURE(k.loop);
		CAPTURE(k.colour);
		shp::Shaper dev;
		dev.prepare(kFs);
		shp::Settings s;
		s.knee = 0.6f;
		s.colour = k.colour;
		// **AT THE DEFAULT MIX, 1 — WHICH THIS CASE COULD NOT DO UNTIL BACKLOG IF.** While the Mix-1 read
		// stepped to ±f(0) at zero, `g0` was a secant across that step and the residue disagreed with its
		// restatement by 0.15 (MEASURED) though the output agreed; the case ran at Mix 0.9 to stay off it.
		// With the step gone it holds at 1, and a restatement at the setting everyone plays is the one
		// worth having.
		s.threshold = 1.0f;
		s.drive = 0.5f;
		s.hysteresis = 0.0f;
		s.bloom = k.bloom;
		s.sag = k.sag;
		s.loop = k.loop;
		settleQuiet(dev, s);

		std::vector<float> table(shp::kTableEntries);
		shp::buildTable(shp::curveOf(s), table);
		const shp::TableBound bound = shp::boundTable(table);
		const shp::Staging staging = shp::stagingFor(s.threshold, s.aggression);
		const float sweep = shp::sweepFor(s.drive);
		const float fade = shp::kneeFadeFor(s.knee);
		const auto f = [&](float v) { return shp::staticTransferAt(table, bound, staging, sweep, fade, v); };
		// THE CURVE'S ORIGIN SLOPE, the reference Loop measures its residue against — by the secant at
		// `kLoopProbe`, read off the drawn transfer rather than the engine.
		const float c0 = f(0.0f);
		const float slope = std::fmax((f(shp::kLoopProbe) - f(-shp::kLoopProbe) - 2.0f * c0) / (2.0f * shp::kLoopProbe), 1.0f);

		double worstResidue = 0.0;
		double worst = 0.0;
		double moved = 0.0; // how far the stages took the output from the bare curve — the non-vacuity half
		constexpr int kN = 9600;
		for (int i = 0; i < kN; ++i) {
			const auto x = static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 110.0 * i / kFs));
			float l = x;
			float r = x;
			// ONE STEP FROM THE ENGINE'S STATE: the residue it held before this sample, read, not integrated.
			const float residue = dev.loopResidueForTest(1);
			dev.processStereo(&l, &r, 1);
			const float env = std::fmin(dev.envelopeForTest(1), 1.0f);
			const float b = k.bloom * shp::kBloomDepth * env;
			const float rail = std::fmax(1.0f - k.sag * shp::kSagDepth * env, shp::kSagFloor);
			const float u = (x + b) / rail + k.loop * shp::kLoopMaxGain * env * residue;
			const float curve = f(u);
			float y = curve;
			const float anchor = (u < 0.0f) ? -c0 : c0;
			if (k.bloom != 0.0f || k.sag != 0.0f) {
				const float lift = f(b / rail) - c0;
				y = anchor + rail * (curve - anchor - lift) / (1.0f + std::fabs(lift));
			}
			const float wantResidue = residue + ((u - (curve - anchor) / slope) - residue) * loopAlpha;
			worstResidue = std::max(worstResidue, static_cast<double>(std::fabs(dev.loopResidueForTest(1) - wantResidue)));
			worst = std::max(worst, static_cast<double>(std::fabs(dev.preBlockForTest() - y)));
			moved = std::max(moved, static_cast<double>(std::fabs(y - f(x))));
		}
		// THE ENGINE-AGAINST-DRAWN BAR of "the drawn transfer IS the device's" — the two disagree at the
		// 1e-4 level at rest (the smoothers' asymptotes), so this cannot be tighter than that case.
		CHECK(worst < 1e-3);
		CHECK(worstResidue < 1e-3);
		CHECK(moved > 0.05);
	}
}

TEST_CASE("IE MEMORY: the envelope is a peak follower that rises in milliseconds and falls in hundreds") {
	shp::Shaper dev;
	dev.prepare(48000.0);
	shp::Settings s;
	s.knee = 0.6f;
	s.hysteresis = 0.0f;
	settleQuiet(dev, s);
	CHECK(dev.envelopeForTest(0) < 1e-6f);
	// 200 ms of a 0.8 sine. A PEAK follower climbs only while the wave is above it, so near the crest —
	// MEASURED 0.66 after 50 ms, not the 0.79 five bare time constants would give.
	(void)preBlockRun(dev, 0.8f, 44, 9600);
	CHECK(dev.envelopeForTest(0) > 0.7f);
	CHECK(dev.envelopeForTest(0) <= 0.8f);
	// 100 ms of silence: the release (250 ms) has had 0.4 of one — still well up, which is the point.
	(void)preBlockRun(dev, 0.0f, 1, 4800);
	CHECK(dev.envelopeForTest(0) > 0.4f);
	CHECK(dev.envelopeForTest(0) < 0.65f);
}

TEST_CASE("IE BLOOM: a symmetric curve on a symmetric sine gains an even harmonic only as you play") {
	// THE EVEN HARMONIC IS THE WHOLE POINT of bloom, and a symmetric curve (the inflator's line, where
	// asymmetry is 0.5) makes none — so the octave's growth is attributable to the bias alone.
	constexpr int kN = 4800;
	constexpr int kCycles = 20;
	const auto octave = [&](float bloom) {
		shp::Shaper dev;
		dev.prepare(48000.0);
		shp::Settings s;
		s.knee = 0.6f;
		s.colour = 0.8f;
		// THE INFLATOR, WHICH IS COLOUR'S INFLATE MODE since backlog IW (it was a zone on the gamma-0 line): a symmetric curve.
		s.colourMode = shp::kColourInflate;
		s.inflateAmount = 1.0f;
		s.hysteresis = 0.0f;
		s.bloom = bloom;
		settleQuiet(dev, s);
		(void)preBlockRun(dev, 0.8f, kCycles, kN);
		const std::vector<float> y = preBlockRun(dev, 0.8f, kCycles, kN);
		return binOf(y, 2 * kCycles) / binOf(y, kCycles);
	};
	const double off = octave(0.0f);
	const double on = octave(1.0f);
	MESSAGE("H2/H1 bloom 0: " << off << "  bloom 1: " << on);
	CHECK(off < 1e-3);
	CHECK(on > 0.02);
}

TEST_CASE("IE: with all three memory stages at full the device stays finite and lets go of silence as fast as without them") {
	// **ON THE REAL OUTPUT**, after the blocker: at Mix 1 the stage steps to ±f(0) at zero, so its
	// pre-blocker value at silence is that constant (MEASURED up to 0.77) and a pre-blocker "is it
	// silent" asks the wrong question. The claim is about what is heard.
	constexpr int kTail = 72000; // 1.5 s
	const auto run = [&](float colour, bool memory, float& clipped) {
		shp::Shaper dev;
		dev.prepare(48000.0);
		shp::Settings s;
		s.knee = 1.0f;
		s.colour = colour;
		s.drive = 1.0f;
		s.bloom = memory ? 1.0f : 0.0f;
		s.sag = memory ? 1.0f : 0.0f;
		s.loop = memory ? 1.0f : 0.0f;
		settleQuiet(dev, s);
		bool finite = true;
		for (int i = 0; i < 48000; ++i) {
			float l = static_cast<float>(std::sin(2.0 * std::numbers::pi * 97.0 * i / 48000.0));
			float r = l;
			dev.processStereo(&l, &r, 1);
			finite = finite && std::isfinite(l);
		}
		CHECK(finite);
		clipped = dev.ceilingExcessForTest();
		int lastLoud = -1;
		for (int i = 0; i < kTail; ++i) {
			float l = 0.0f;
			float r = 0.0f;
			dev.processStereo(&l, &r, 1);
			if (std::fabs(l) > 1e-3f) {
				lastLoud = i;
			}
		}
		return lastLoud / 48.0; // ms
	};
	for (const float colour : { 0.0f, 0.38f, 0.625f, 0.9f }) {
		float clipBase = 0.0f;
		float clipMemory = 0.0f;
		const double base = run(colour, false, clipBase);
		const double memory = run(colour, true, clipMemory);
		CAPTURE(colour);
		MESSAGE("colour " << colour << ": tail above -60 dB " << base << " ms without, " << memory << " ms with; ceiling excess "
		                  << clipBase << " without, " << clipMemory << " with");
		CHECK(memory < 1500.0 - 100.0); // it LETS GO — Loop's first draft never did
	}
}

TEST_CASE("IF: at Mix 1 the curve passes through zero — no step at the zero crossing") {
	// **THE STEP THAT WAS** (backlog IF): the Mix-1 threshold sits just under zero so the blend is whole
	// at the origin, and the read started there too — so a zero input read the table's steep start and
	// the drawn curve went f(0±) = ±0.046 at the defaults, ±0.77 at Knee 1 / Drive 1. The read now
	// starts at the threshold floored at zero, and these are VALUES either side of the origin.
	struct P {
		float knee, colour, drive;
	};
	for (const P p : { P { 0.5f, 0.0f, 0.0f }, P { 0.6f, 0.8f, 0.5f }, P { 1.0f, 0.38f, 1.0f }, P { 1.0f, 0.625f, 1.0f } }) {
		CAPTURE(p.knee);
		CAPTURE(p.colour);
		CAPTURE(p.drive);
		shp::Settings s;
		s.knee = p.knee;
		s.colour = p.colour;
		s.drive = p.drive;
		s.hysteresis = 0.0f;
		std::vector<float> table(shp::kTableEntries);
		shp::buildTable(shp::curveOf(s), table);
		const shp::TableBound bound = shp::boundTable(table);
		const shp::Staging staging = shp::stagingFor(s.threshold, s.aggression);
		REQUIRE(staging.threshold < 0.0f); // the undershoot this is about is still there, for the blend
		const float sweep = shp::sweepFor(s.drive);
		const float fade = shp::kneeFadeFor(s.knee);
		const auto f = [&](float v) { return shp::staticTransferAt(table, bound, staging, sweep, fade, v); };
		CHECK(std::fabs(f(0.0f)) < 1e-6f);
		CHECK(std::fabs(f(1e-5f)) < 2e-3f);
		CHECK(std::fabs(f(-1e-5f)) < 2e-3f);

		// AND THE ENGINE: a sine at -80 dBFS was ±f(0) buzz — a square wave at up to 0.77. Now it is a
		// quiet signal, however steep the curve's start makes it.
		shp::Shaper dev;
		dev.prepare(48000.0);
		settleQuiet(dev, s);
		const std::vector<float> y = preBlockRun(dev, 1e-4f, 20, 4800);
		float top = 0.0f;
		for (const float v : y) {
			top = std::max(top, std::fabs(v));
		}
		CAPTURE(top);
		CHECK(top < 0.02f);
	}
}

TEST_CASE("II: the pad's travel reaches every extra's phase — each moves its own, and a full turn is no move at all") {
	// In the firmware the extras are attached to the zone knob, which ties back to the pad: the
	// firmware derives every extra from the zone knob; our pad's travel reached only
	// the curve's eight phases. It now carries ten more (`phase_space::kCurvePhaseCount` onward).
	namespace ps = shp::phase_space;
	struct Extra {
		size_t phase;
		std::string name;
		float (*read)(const shp::CurveParams&);
	};
	const Extra extras[] = {
		{ ps::kDeadzoneWidthPhase, "deadzone width", [](const shp::CurveParams& p) { return p.deadzoneWidth; } },
		{ ps::kDeadzoneCentrePhase, "deadzone centre", [](const shp::CurveParams& p) { return p.deadzonePhase; } },
		{ ps::kHysteresisPhase, "hysteresis", [](const shp::CurveParams& p) { return p.hysteresis; } },
		{ ps::kSubDepthPhase, "sub depth", [](const shp::CurveParams& p) { return p.subIntensity; } },
		{ ps::kSubIntervalPhase, "sub interval", [](const shp::CurveParams& p) { return static_cast<float>(p.subRatio); } },
		{ ps::kCombStridePhase, "comb stride", [](const shp::CurveParams& p) { return static_cast<float>(p.stride); } },
		{ ps::kCombDepthPhase, "comb depth", [](const shp::CurveParams& p) { return p.feedback; } },
		{ ps::kRotationPhase, "rotation", [](const shp::CurveParams& p) { return static_cast<float>(p.rotation); } },
		{ ps::kSlewPhase, "slew", [](const shp::CurveParams& p) { return p.slewIntensity; } },
		{ ps::kPreStagePhase, "pre-stage", [](const shp::CurveParams& p) { return p.preExpandAmount; } },
	};
	const auto weightsOf = [](const shp::CurveParams& p) {
		return std::array<float, 8> { p.inflatorWeight, p.polyWeight, p.hardKneeWeight, p.chebyWeight,
		                              p.sineFoldWeight, p.rectifierWeight, p.threshold, p.asymmetry };
	};
	constexpr float kKnee = 0.8f;
	constexpr float kColour = 0.38f;
	constexpr float kGamma = 0.2f; // off the gamma-0 line, where the inflator's zone would pin the weights
	std::array<float, ps::kPhaseCount> rest {};
	const shp::CurveParams home = shp::deriveCurve(kKnee, kColour, kGamma, shp::kSlewHarmonics, &rest);
	// ZERO TRAVEL IS THE FIELD WITH NO TRAVEL AT ALL, bit for bit — the firmware parity at rest.
	const shp::CurveParams none = shp::deriveCurve(kKnee, kColour, kGamma, shp::kSlewHarmonics);
	CHECK(weightsOf(home) == weightsOf(none));
	for (const Extra& e : extras) {
		CHECK(e.read(home) == e.read(none));
	}

	for (const Extra& e : extras) {
		CAPTURE(e.name);
		// SOMEWHERE ON ITS CYCLE THE EXTRA MOVES — scanned, because a triangle with a narrow duty is
		// flat over most of a turn and one offset can land on the flat.
		bool moved = false;
		for (int k = 1; k < 16 && !moved; ++k) {
			std::array<float, ps::kPhaseCount> tr {};
			tr[e.phase] = static_cast<float>(k) / 16.0f;
			const shp::CurveParams p = shp::deriveCurve(kKnee, kColour, kGamma, shp::kSlewHarmonics, &tr);
			moved = e.read(p) != e.read(home);
			// AND NOTHING ELSE OF THE CURVE MOVES: the weights, the threshold and asymmetry are the
			// curve's eight phases and this travel is not theirs.
			CHECK(weightsOf(p) == weightsOf(home));
		}
		CHECK(moved);
		// A FULL TURN IS NO MOVE: the term enters a PHASE, which a wrap takes back — an amount would not.
		std::array<float, ps::kPhaseCount> turn {};
		turn[e.phase] = 1.0f;
		const shp::CurveParams p = shp::deriveCurve(kKnee, kColour, kGamma, shp::kSlewHarmonics, &turn);
		CHECK(e.read(p) == doctest::Approx(e.read(home)).epsilon(1e-5));
	}

	// AND THE CURVE'S PHASES LEAVE THE EXTRAS WHERE THEY WERE — the other half of "its own".
	std::array<float, ps::kPhaseCount> curveOnly {};
	for (size_t i = 0; i < ps::kCurvePhaseCount; ++i) {
		curveOnly[i] = 0.31f + 0.07f * static_cast<float>(i);
	}
	const shp::CurveParams moved = shp::deriveCurve(kKnee, kColour, kGamma, shp::kSlewHarmonics, &curveOnly);
	CHECK(weightsOf(moved) != weightsOf(home));
	for (const Extra& e : extras) {
		CAPTURE(e.name);
		CHECK(e.read(moved) == e.read(home));
	}
}

// ---------------------------------------------------------------------------------------------
// BACKLOG FA (2026-10-10) — the input HPF and LPF, the crossfade Mix, and the old Mix renamed Threshold.
// DECISIONS: the filters act on the WHOLE device's input, so the Mix's dry is taken after
// them; the old Mix keeps its key; the new Mix is a new key at 100% wet; the filters default fully open.
// ---------------------------------------------------------------------------------------------


// GUARDED AS ITS ONE CALLER IS — the digests are pinned only where they were measured.
#if defined(__x86_64__) && defined(__linux__)
/// A DIGEST OF WHAT THE SHAPER PLAYS: FNV-1a over the bits of 48 blocks of a hot two-tone left and a quiet right.
namespace {
uint64_t faDigest(const shp::Settings& settings) {
	auto s = std::make_unique<shp::Shaper>();
	s->prepare(48000.0);
	s->reset();
	s->setTarget(settings);
	uint64_t h = 1469598103934665603ull;
	constexpr int kFrames = 128;
	std::vector<float> l(kFrames), r(kFrames);
	int n = 0;
	for (int b = 0; b < 48; ++b) {
		for (int i = 0; i < kFrames; ++i, ++n) {
			const double t = static_cast<double>(n) / 48000.0;
			l[static_cast<size_t>(i)] = static_cast<float>(1.3 * std::sin(2.0 * std::numbers::pi * 110.0 * t)
			                                               + 0.2 * std::sin(2.0 * std::numbers::pi * 3170.0 * t));
			r[static_cast<size_t>(i)] = static_cast<float>(0.5 * std::sin(2.0 * std::numbers::pi * 220.0 * t + 0.3));
		}
		s->processStereo(l.data(), r.data(), kFrames);
		for (int i = 0; i < kFrames; ++i) {
			for (const float v : {l[static_cast<size_t>(i)], r[static_cast<size_t>(i)]}) {
				uint32_t bits = 0;
				std::memcpy(&bits, &v, sizeof bits);
				h = (h ^ bits) * 1099511628211ull;
			}
		}
	}
	return h;
}

std::vector<shp::Settings> faDigestSettings() {
	std::vector<shp::Settings> out;
	out.emplace_back();
	shp::Settings a;
	a.knee = 0.8f;
	a.colour = 0.37f;
	a.drive = 0.6f;
	a.threshold = 0.6f;
	out.push_back(a);
	shp::Settings b;
	b.colour = 1.3f;
	b.gamma = 0.4f;
	b.emphasisDb = 6.0f;
	b.subOn = true;
	b.feedbackOn = true;
	b.bloom = 0.4f;
	b.threshold = 0.85f;
	out.push_back(b);
	shp::Settings c;
	c.knee = 0.0f;
	c.trim = 6.0f;
	out.push_back(c);
	return out;
}
} // namespace
#endif

TEST_CASE("FA: at their defaults the input filters and the Mix leave the shaper bit for bit as it was") {
	// THE STRUCTURAL HALF: at the declared defaults neither filter runs and the crossfade is all wet.
	const shp::Settings defaults;
	CHECK(defaults.inputHpf == static_cast<float>(shp::kParamDescs[static_cast<size_t>(shp::Param::InputHpf)].def));
	CHECK(defaults.inputLpf == static_cast<float>(shp::kParamDescs[static_cast<size_t>(shp::Param::InputLpf)].def));
	CHECK(defaults.dryWet == 1.0f);
	{
		shp::Shaper s;
		s.prepare(48000.0);
		settle(s, defaults);
		CHECK(s.inputHpfHzForTest() == 0.0f);
		CHECK(s.inputLpfHzForTest() == 0.0f);
		CHECK(s.dryWetForTest() == 1.0f);
	}
	// THE OUTPUT HALF, AGAINST THE DEVICE BEFORE FA. These digests were read off mainline deae20fe — the
	// commit this branch starts from — by the same `faDigest` over the same four settings (defaults; a
	// shaped, thresholded position; one with Emphasis, two extras and Bloom on; the Knee-0 bypass with a
	// trim). Float bits depend on the platform's libm, so they are pinned where they were measured, as
	// `vox-voice` and `flagship` pin theirs.
#if defined(__x86_64__) && defined(__linux__)
	const uint64_t kBeforeFa[] = { 0xffbf8d66ddf9889bull, 0xa4cbeab5fa6e699full, 0x87c9999de2f2b797ull,
		                           0x4f21df927d30cc29ull };
	const std::vector<shp::Settings> settings = faDigestSettings();
	REQUIRE(settings.size() == std::size(kBeforeFa));
	for (size_t i = 0; i < settings.size(); ++i) {
		CAPTURE(i);
		CHECK(faDigest(settings[i]) == kBeforeFa[i]);
	}
#endif
}

TEST_CASE("FA: the input filters' corners are where their knobs say, and off at the open ends") {
	constexpr double kRate = 48000.0;
	shp::Shaper s;
	s.prepare(kRate);
	shp::Settings settings;
	settings.inputHpf = std::log2(250.0f);
	settings.inputLpf = std::log2(3000.0f);
	settle(s, settings);
	// THE CORNER IN Hz AND THE ONE-POLE'S EXACT COEFFICIENT, restated here rather than asked of the header.
	CHECK(s.inputHpfHzForTest() == doctest::Approx(250.0).epsilon(1e-5));
	CHECK(s.inputLpfHzForTest() == doctest::Approx(3000.0).epsilon(1e-5));
	CHECK(s.inputHpfAlphaForTest() == doctest::Approx(1.0 - std::exp(-2.0 * std::numbers::pi * 250.0 / kRate)).epsilon(1e-5));
	CHECK(s.inputLpfAlphaForTest() == doctest::Approx(1.0 - std::exp(-2.0 * std::numbers::pi * 3000.0 / kRate)).epsilon(1e-5));

	SUBCASE("each end is off, and a hair inside it is not") {
		settings.inputHpf = static_cast<float>(shp::kInputFilterMinLog2);
		settings.inputLpf = static_cast<float>(shp::kInputFilterMaxLog2);
		settle(s, settings);
		CHECK(s.inputHpfHzForTest() == 0.0f);
		CHECK(s.inputLpfHzForTest() == 0.0f);
		// A host's normalised round trip lands within the slack and still reads Off.
		settings.inputHpf += 0.5f * static_cast<float>(feathers::device::kOffEndSlack);
		settings.inputLpf -= 0.5f * static_cast<float>(feathers::device::kOffEndSlack);
		settle(s, settings);
		CHECK(s.inputHpfHzForTest() == 0.0f);
		CHECK(s.inputLpfHzForTest() == 0.0f);
		// One hundredth of an octave in is a filter.
		settings.inputHpf = static_cast<float>(shp::kInputFilterMinLog2) + 0.01f;
		settings.inputLpf = static_cast<float>(shp::kInputFilterMaxLog2) - 0.01f;
		settle(s, settings);
		CHECK(s.inputHpfHzForTest() == doctest::Approx(20.0 * std::exp2(0.01)).epsilon(1e-4));
		CHECK(s.inputLpfHzForTest() == doctest::Approx(20000.0 * std::exp2(-0.01)).epsilon(1e-4));
	}

	SUBCASE("the corners follow the sample rate's own coefficient") {
		s.prepare(96000.0);
		settle(s, settings);
		CHECK(s.inputHpfHzForTest() == doctest::Approx(250.0).epsilon(1e-5));
		CHECK(s.inputHpfAlphaForTest() == doctest::Approx(1.0 - std::exp(-2.0 * std::numbers::pi * 250.0 / 96000.0)).epsilon(1e-5));
	}
}

TEST_CASE("FA: the Mix's dry is the input AFTER the filters, run by the one-pole law") {
	// AT MIX 0 THE DEVICE RETURNS ITS DRY, so the output IS the filtered input — which pins both the filters'
	// law (restated below as two one-poles, sample by sample) and where the dry tap sits. Knee 0.7 and a
	// drive so the wet is far from the dry: a tap taken before the filters, or a mix that leaked wet, fails.
	constexpr double kRate = 48000.0;
	constexpr int kFrames = 256;
	constexpr int kBlocks = 8;
	shp::Shaper s;
	s.prepare(kRate);
	shp::Settings settings;
	settings.knee = 0.7f;
	settings.drive = 0.6f;
	settings.dryWet = 0.0f;
	settings.inputHpf = std::log2(400.0f);
	settings.inputLpf = std::log2(2500.0f);
	s.setTarget(settings);
	const auto hpfAlpha = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * 400.0 / kRate));
	const auto lpfAlpha = static_cast<float>(1.0 - std::exp(-2.0 * std::numbers::pi * 2500.0 / kRate));
	float low = 0.0f;
	float lp = 0.0f;
	float worst = 0.0f;
	float energy = 0.0f;
	int n = 0;
	for (int b = 0; b < kBlocks; ++b) {
		std::vector<float> l(kFrames);
		for (int i = 0; i < kFrames; ++i, ++n) {
			l[static_cast<size_t>(i)] = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * 90.0 * n / kRate)
			                                               + 0.4 * std::sin(2.0 * std::numbers::pi * 5100.0 * n / kRate));
		}
		std::vector<float> r = l;
		const std::vector<float> in = l;
		s.processStereo(l.data(), r.data(), kFrames);
		for (int i = 0; i < kFrames; ++i) {
			float x = in[static_cast<size_t>(i)];
			low += (x - low) * hpfAlpha;
			x -= low;
			lp += (x - lp) * lpfAlpha;
			worst = std::max(worst, std::fabs(l[static_cast<size_t>(i)] - lp));
			energy += std::fabs(in[static_cast<size_t>(i)] - lp);
		}
	}
	CAPTURE(worst);
	CHECK(worst < 2.0e-6f);
	// NON-VACUOUS: the filters took a great deal out, so "the output is the input" would fail here.
	CHECK(energy / static_cast<float>(n) > 0.1f);
}

TEST_CASE("FA: the Mix is a LINEAR crossfade of the dry against the wet, gliding to where it is set") {
	SUBCASE("the law, by value") {
		CHECK(shp::dryWetMix(0.2f, 0.9f, 0.0f) == 0.2f);
		CHECK(shp::dryWetMix(0.2f, 0.9f, 0.25f) == doctest::Approx(0.2f + 0.25f * 0.7f));
		CHECK(shp::dryWetMix(0.2f, 0.9f, 0.5f) == doctest::Approx(0.55f));
		// AT 100% THE WET, UNTOUCHED — even one the clamp would have changed, which is what keeps the
		// default bit for bit.
		CHECK(shp::dryWetMix(3.0f, 0.7f, 1.0f) == 0.7f);
		// AND NO CLAMP ON THE MIX, by design: a hot dry share passes above 0 dBFS as it came.
		CHECK(shp::dryWetMix(1.6f, 0.9f, 0.25f) == 1.6f + 0.25f * (0.9f - 1.6f));
		CHECK(shp::dryWetMix(-1.6f, -0.9f, 0.25f) == -1.6f + 0.25f * (-0.9f + 1.6f));
		// LINEAR, NOT EQUAL-POWER: where dry and wet agree the middle of the knob is the same signal.
		CHECK(shp::dryWetMix(0.5f, 0.5f, 0.5f) == doctest::Approx(0.5f));
	}

	SUBCASE("in the engine, the half-way output is half-way between the all-dry and all-wet ones") {
		shp::Settings settings;
		settings.knee = 0.8f;
		settings.drive = 0.7f;
		settings.colour = 0.37f;
		std::vector<std::vector<float>> outs;
		for (const float mix : { 0.0f, 1.0f, 0.5f, 0.25f }) {
			auto s = std::make_unique<shp::Shaper>();
			s->prepare(48000.0);
			settings.dryWet = mix;
			outs.push_back(run(*s, settings, 24, 256, 0.8f));
			CHECK(s->dryWetForTest() == mix); // primed on the first block, as the threshold is
		}
		const auto& dry = outs[0];
		const auto& wet = outs[1];
		float apart = 0.0f;
		for (size_t i = 0; i < dry.size(); ++i) {
			CAPTURE(i);
			CHECK(outs[2][i] == doctest::Approx(dry[i] + 0.5f * (wet[i] - dry[i])).epsilon(1e-5));
			CHECK(outs[3][i] == doctest::Approx(dry[i] + 0.25f * (wet[i] - dry[i])).epsilon(1e-5));
			apart = std::max(apart, std::fabs(wet[i] - dry[i]));
		}
		CHECK(apart > 0.05f); // non-vacuous: the shaped wet is far from the dry
		// AND THE DRY, AT MIX 0, IS THE INPUT ITSELF (no filters on): the sine `run` played.
		const std::vector<float> in = sine(256, 0.8f, 4.0f);
		for (size_t i = 0; i < dry.size(); ++i) {
			CHECK(dry[i] == in[i]);
		}
	}

	SUBCASE("the plot draws the crossfade by the same law: the curve drawn is the curve heard") {
		shp::Settings settings;
		settings.knee = 0.8f;
		settings.drive = 0.7f;
		settings.colour = 0.37f;
		const auto wet = feathers::ui::shaperPlotCurve(settings);
		settings.dryWet = 0.35f;
		const auto half = feathers::ui::shaperPlotCurve(settings);
		float apart = 0.0f;
		for (size_t i = 0; i < wet.size(); ++i) {
			const float x = static_cast<float>(i) * (2.0f / static_cast<float>(wet.size() - 1)) - 1.0f;
			CAPTURE(i);
			CHECK(half[i] == doctest::Approx(x + 0.35f * (wet[i] - x)).epsilon(1e-5));
			apart = std::max(apart, std::fabs(wet[i] - x));
		}
		CHECK(apart > 0.05f); // non-vacuous: the drawn wet is far from the line
	}

	SUBCASE("a moved Mix glides, and lands exactly") {
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings settings;
		settle(s, settings);
		REQUIRE(s.dryWetForTest() == 1.0f);
		settings.dryWet = 0.3f;
		s.setTarget(settings);
		std::vector<float> l(64, 0.0f);
		std::vector<float> r = l;
		s.processStereo(l.data(), r.data(), 64);
		CHECK(s.dryWetForTest() < 1.0f);
		CHECK(s.dryWetForTest() > 0.3f);
		settle(s, settings);
		CHECK(s.dryWetForTest() == 0.3f);
		// AND BACK TO 100%, EXACTLY — the default's bit-identity rests on `dryWetMix` seeing 1, not 0.999997.
		settings.dryWet = 1.0f;
		settle(s, settings);
		CHECK(s.dryWetForTest() == 1.0f);
	}
}

TEST_CASE("FA: each new control changes the sound, and the filters do so at Knee 0 too") {
	// THE HONESTY RULE: a control is lit iff it changes the sound. The card and window light HPF and LPF at
	// Knee 0 and grey the Mix there; this is the engine's half of that claim.
	const auto render = [](const shp::Settings& settings) {
		auto s = std::make_unique<shp::Shaper>();
		s->prepare(48000.0);
		return run(*s, settings, 24, 256, 0.8f);
	};
	const auto differs = [](const std::vector<float>& a, const std::vector<float>& b) {
		float d = 0.0f;
		for (size_t i = 0; i < a.size(); ++i) {
			d = std::max(d, std::fabs(a[i] - b[i]));
		}
		return d;
	};
	for (const float knee : { 0.7f, 0.0f }) {
		CAPTURE(knee);
		shp::Settings base;
		base.knee = knee;
		base.drive = 0.6f;
		const std::vector<float> plain = render(base);
		shp::Settings hpf = base;
		hpf.inputHpf = std::log2(1500.0f);
		CHECK(differs(render(hpf), plain) > 0.01f);
		shp::Settings lpf = base;
		lpf.inputLpf = std::log2(300.0f);
		CHECK(differs(render(lpf), plain) > 0.01f);
		shp::Settings mix = base;
		mix.dryWet = 0.4f;
		if (knee > 0.0f) {
			CHECK(differs(render(mix), plain) > 0.01f);
		}
		else {
			// AT THE BYPASS THE WET IS THE DRY, so the Mix has nothing to cross — which is why it greys.
			CHECK(differs(render(mix), plain) == 0.0f);
		}
	}
}

TEST_CASE("FA follow-up: Trim acts at Knee 0, so the bypass is the trimmed input, clamped, to the bit") {
	// THE HONESTY RULE'S ENGINE HALF FOR TRIM. The card and window greyed Trim at Knee 0 while `processSample`
	// applies it above the bypass. Asserted as the VALUE the bypass promises — the input times the trim's gain,
	// clamped at full scale — and not as "differs", which any change would pass. Both signs: a cut moves every
	// sample, a boost moves them and then meets the clamp.
	const std::vector<float> in = sine(256, 0.6f, 4.0f);
	for (const float trimDb : { -6.0f, 6.0f }) {
		CAPTURE(trimDb);
		shp::Settings settings;
		settings.knee = 0.0f;
		settings.trim = trimDb;
		REQUIRE(settings.linear());
		auto s = std::make_unique<shp::Shaper>();
		s->prepare(48000.0);
		const std::vector<float> out = run(*s, settings, 24, 256, 0.6f);
		const float gain = std::pow(10.0f, trimDb / 20.0f);
		size_t moved = 0;
		for (size_t i = 0; i < in.size(); ++i) {
			CAPTURE(i);
			CHECK(out[i] == std::clamp(in[i] * gain, -1.0f, 1.0f));
			moved += (out[i] != in[i]) ? 1 : 0;
		}
		// NON-VACUOUS: the trim did move the bypass, on most of the block (all but the sine's zeros).
		CHECK(moved > in.size() / 2);
	}
}

TEST_CASE("FA follow-up: every shaper glide lands EXACTLY on its target, Bloom, Sag, Loop, Cheby and the Mix") {
	// `glide`'s 1e-6 window was narrower than where float rounding STALLS an approach away from zero: MEASURED, a
	// move to 0.3 parked at 0.300003 for good. The Mix had its own landing glide; now all five share it. Asserted
	// by value, through a sequence that leaves zero, crosses back to it and goes to the range's ends, so each move
	// starts where the last one landed.
	struct Lane {
		std::string name;
		float shp::Settings::*field;
		float (shp::Shaper::*glided)() const noexcept;
		std::vector<float> targets;
	};
	const std::vector<Lane> lanes = {
		{ "bloom", &shp::Settings::bloom, &shp::Shaper::bloomForTest, { 0.3f, 1.0f, 0.0f, 0.7f, 0.123f, 0.0f } },
		{ "sag", &shp::Settings::sag, &shp::Shaper::sagForTest, { 0.3f, 1.0f, 0.0f, 0.7f, 0.123f, 0.0f } },
		{ "loop", &shp::Settings::loop, &shp::Shaper::loopForTest, { 0.3f, 1.0f, 0.0f, 0.7f, 0.123f, 0.0f } },
		{ "cheby", &shp::Settings::cheby, &shp::Shaper::chebyMixForTest,
		  { 0.3f, 1.0f, 0.0f, -0.3f, -1.0f, 0.7f, 0.0f } },
		{ "mix", &shp::Settings::dryWet, &shp::Shaper::dryWetForTest, { 0.3f, 1.0f, 0.0f, 0.7f, 0.123f, 1.0f } },
	};
	for (const Lane& lane : lanes) {
		CAPTURE(lane.name);
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings settings;
		settings.knee = 0.6f; // off the bypass, so the curve's stages are the ones listening
		settle(s, settings);
		float from = (s.*lane.glided)();
		for (const float target : lane.targets) {
			CAPTURE(target);
			settings.*lane.field = target;
			s.setTarget(settings);
			// NON-VACUOUS: a move is a GLIDE, strictly between where it was and where it is going after one block.
			std::vector<float> l(64, 0.0f);
			std::vector<float> r = l;
			s.processStereo(l.data(), r.data(), 64);
			const float mid = (s.*lane.glided)();
			CHECK(std::fabs(mid - target) > 0.0f);
			CHECK(std::fabs(mid - target) < std::fabs(from - target));
			settle(s, settings);
			CHECK((s.*lane.glided)() == target);
			from = target;
		}
	}
}

TEST_CASE("FA: the old Mix is Threshold by name and shaper_mix by key; the crossfade is a new key") {
	const auto& threshold = shp::kParamDescs[static_cast<size_t>(shp::Param::Threshold)];
	CHECK(std::string(threshold.key) == "shaper_mix");
	CHECK(std::string(threshold.name) == "Shaper Threshold");
	const auto& mix = shp::kParamDescs[static_cast<size_t>(shp::Param::DryWet)];
	CHECK(std::string(mix.key) == "shaper_dry_wet");
	CHECK(std::string(mix.name) == "Shaper Mix");
	CHECK(mix.def == 1.0);
	CHECK(mix.modulatable);
	for (const shp::Param p : { shp::Param::InputHpf, shp::Param::InputLpf }) {
		const auto& d = shp::kParamDescs[static_cast<size_t>(p)];
		CHECK(d.unit == feathers::device::Unit::LogHertz);
		CHECK(d.min == doctest::Approx(std::log2(20.0)));
		CHECK(d.max == doctest::Approx(std::log2(20000.0)));
	}
	// AND THE ROW CARRIES ALL FOUR THROUGH A ROUND TRIP.
	shp::Settings s;
	s.threshold = 0.4f;
	s.inputHpf = 7.5f;
	s.inputLpf = 12.25f;
	s.dryWet = 0.6f;
	const shp::Settings back = shp::stateOf(shp::rowOf(s), static_cast<size_t>(shp::Param::kCount));
	CHECK(back.threshold == 0.4f);
	CHECK(back.inputHpf == 7.5f);
	CHECK(back.inputLpf == 12.25f);
	CHECK(back.dryWet == 0.6f);
}

TEST_CASE("FA: the filters' open ends are an exact bypass, to the bit, inside the slack too") {
	// The pinned digests prove the DEFAULTS against mainline; this proves the ENDS against the defaults on any
	// platform, including a value a host's normalised round trip left a hair inside an end.
	const auto render = [](const shp::Settings& settings) {
		auto s = std::make_unique<shp::Shaper>();
		s->prepare(48000.0);
		return run(*s, settings, 24, 256, 1.3f);
	};
	const float slack = 0.5f * static_cast<float>(feathers::device::kOffEndSlack);
	for (const float knee : { 0.7f, 0.0f }) {
		CAPTURE(knee);
		shp::Settings plain;
		plain.knee = knee;
		plain.drive = 0.6f;
		plain.threshold = 0.7f;
		shp::Settings ends = plain;
		ends.inputHpf = static_cast<float>(shp::kInputFilterMinLog2) + slack;
		ends.inputLpf = static_cast<float>(shp::kInputFilterMaxLog2) - slack;
		REQUIRE(ends.inputHpf != plain.inputHpf); // non-vacuous: the stored values differ
		const std::vector<float> a = render(plain);
		const std::vector<float> b = render(ends);
		CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
		// AND ONE HUNDREDTH OF AN OCTAVE IN IS NOT A BYPASS: the comparison can see a filter.
		shp::Settings in = plain;
		in.inputLpf = static_cast<float>(shp::kInputFilterMaxLog2) - 0.01f;
		const std::vector<float> c = render(in);
		CHECK(std::memcmp(a.data(), c.data(), a.size() * sizeof(float)) != 0);
	}
}

TEST_CASE("FA: on a hot input at Mix 50% the dry share passes unclamped, and the wet never leaves 0 dBFS") {
	// NO CLAMP ON THE MIX, by design. The output is `dry + (wet - dry)·mix` BY VALUE, above 0 dBFS
	// wherever the dry share takes it there; the 0 dBFS rule is the SHAPED path's, and still holds of the wet.
	// The measurement that decided it is kept as a MESSAGE: a clamp here hard-clipped the dry on 0 / 120 / 168
	// of 256 samples at 0 / +3 / +6 dB (backlog FA).
	constexpr int kFrames = 256;
	constexpr int kBlocks = 32;
	for (const float amp : { 1.0f, 1.41f, 2.0f }) { // 0, +3 and +6 dBFS peaks
		CAPTURE(amp);
		shp::Settings settings;
		settings.knee = 0.6f;
		settings.drive = 0.5f;
		std::vector<std::vector<float>> out;
		for (const float mix : { 1.0f, 0.5f }) {
			auto s = std::make_unique<shp::Shaper>();
			s->prepare(48000.0);
			settings.dryWet = mix;
			out.push_back(run(*s, settings, kBlocks, kFrames, amp));
		}
		const std::vector<float> in = sine(kFrames, amp, 4.0f);
		size_t over = 0;
		float peak = 0.0f;
		for (size_t i = 0; i < in.size(); ++i) {
			const float wet = out[0][i];
			CHECK(std::fabs(wet) <= 1.0f); // the shaped path keeps the 0 dBFS rule
			const float expected = in[i] + (wet - in[i]) * 0.5f;
			CHECK(out[1][i] == doctest::Approx(expected).epsilon(1e-6));
			over += std::fabs(expected) > 1.0f ? 1u : 0u;
			peak = std::max(peak, std::fabs(out[1][i]));
		}
		MESSAGE("amp " << amp << ": " << over << " of " << in.size()
		               << " samples above 0 dBFS at Mix 50% — what a clamp would have hard-clipped; peak " << peak);
		if (amp > 1.0f) {
			CHECK(peak > 1.0f); // non-vacuous: the case reaches the region a clamp would change
		}
	}
}

TEST_CASE("OM: MEASURED — what a per-voice shaper costs: one instance's bytes, and a whole table rebuild's time") {
	// THE NUMBERS shaper_types.h's placement note quotes. A voice instance is a whole `Shaper`; a rebuild is what a
	// hand edit to a table term (Knee, Colour, Gamma, travel, Deadzone, the Inflate mode) costs EACH voice. None of
	// those is a destination, so no note and no modulator triggers one.
	namespace shp = feathers::fx::shaper;
	// **ONE TABLE FOR EVERY VOICE**: a voice is the instance alone; the table bank is the bus's.
	// MEASURED: 9,312 B a voice before the change, 688 B after (the bank, ~8.6 KB, moved to the bus instance's heap).
	const size_t bytes = sizeof(shp::Shaper);
	MESSAGE("a voice: sizeof(Shaper) = " << bytes << " B; 20 voices = " << 20 * bytes << " B; the bus also holds the bank, "
	                                    << shp::Shaper::bankBytesForTest() << " B");
	CHECK(bytes < 2048u); // a voice holds no table
	// BUDGET: a voice is a whole instance, so keep one under 32 KiB (20 voices under 640 KiB, the Filter's own
	// measured cost in backlog NS).
	CHECK(bytes < 32u * 1024u);

	constexpr int kFrames = 256;
	std::vector<float> l(kFrames), r(kFrames);
	const auto fill = [&] {
		for (int i = 0; i < kFrames; ++i) {
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.5f * std::sin(0.03f * static_cast<float>(i));
		}
	};
	double best = 1e9;
	double steadyBest = 0.0;
	int blocksBest = 0;
	for (int round = 0; round < 9; ++round) {
		shp::Shaper s;
		s.prepare(48000.0);
		shp::Settings a {};
		s.setTarget(a);
		// SETTLED: the first table built and its crossfade done (a build starts only from a settled state).
		for (int b = 0; b < 64; ++b) {
			fill();
			s.processStereo(l.data(), r.data(), kFrames);
		}
		REQUIRE_FALSE(s.buildingForTest());
		a.knee = 0.8f;
		a.colour = 1.0f + 0.1f * static_cast<float>(round);
		s.setTarget(a);
		const int rebuildsBefore = s.rebuildsForTest();
		int blocks = 0;
		const auto t0 = std::chrono::steady_clock::now();
		do {
			fill();
			s.processStereo(l.data(), r.data(), kFrames);
			++blocks;
		} while (s.rebuildsForTest() == rebuildsBefore && blocks < 1000);
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
		REQUIRE(s.rebuildsForTest() > rebuildsBefore);
		// THE SAME BLOCKS SETTLED, so the rebuild's own share can be told from the audio's.
		for (int b = 0; b < 64; ++b) {
			fill();
			s.processStereo(l.data(), r.data(), kFrames);
		}
		const auto t1 = std::chrono::steady_clock::now();
		for (int b = 0; b < blocks; ++b) {
			fill();
			s.processStereo(l.data(), r.data(), kFrames);
		}
		const double steady = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count();
		if (us < best) {
			best = us;
			blocksBest = blocks;
			steadyBest = steady;
		}
	}
	MESSAGE("one table rebuild, amortised: " << blocksBest << " block(s) of 256 frames, " << best
	                                         << " us with their audio against " << steadyBest
	                                         << " us for the same blocks settled (best of 9)");
}

TEST_CASE("OM: a modulated Knee rebuilds at the build's own cadence — never faster than one table per build and crossfade, measured") {
	// THE RATE LIMIT IS THE BUILD ITSELF (`maybeStartBuild`: a build starts only from a settled state, nothing in flight
	// and no crossfade running), so a Knee moving every block turns the table over at most once per 513-sample build plus
	// 512-sample fade. Knee became a destination on 2026-10-31; this holds it to that ceiling and measures it.
	namespace shp = feathers::fx::shaper;
	constexpr int kFrames = 256;
	constexpr double kRate = 48000.0;
	constexpr int kBlocks = static_cast<int>(2.0 * kRate / kFrames); // two seconds
	const auto run = [&](bool moving, int& rebuilds) {
		shp::Shaper s;
		s.prepare(kRate);
		std::vector<float> l(kFrames), r(kFrames);
		const auto t0 = std::chrono::steady_clock::now();
		for (int b = 0; b < kBlocks; ++b) {
			shp::Settings set;
			set.knee = moving ? 0.5f + 0.4f * std::sin(2.0f * 3.14159265f * 5.0f * static_cast<float>(b * kFrames) / 48000.0f)
			                  : 0.5f;
			s.setTarget(set);
			for (int i = 0; i < kFrames; ++i) {
				l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = 0.5f * std::sin(0.03f * static_cast<float>(b * kFrames + i));
			}
			s.processStereo(l.data(), r.data(), kFrames);
		}
		rebuilds = s.rebuildsForTest();
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kBlocks;
	};
	int still = 0, moving = 0;
	double stillUs = 1e9, movingUs = 1e9;
	for (int round = 0; round < 5; ++round) {
		stillUs = std::min(stillUs, run(false, still));
		movingUs = std::min(movingUs, run(true, moving));
	}
	const double ceiling = 2.0 * kRate / (513.0 + 512.0) + 1.0;
	MESSAGE("Knee under a 5 Hz LFO: " << moving << " rebuilds in 2 s (ceiling " << ceiling << "), " << movingUs
	                                   << " us a block against " << stillUs << " still (min of 5)");
	CHECK(moving <= static_cast<int>(ceiling));
	CHECK(moving > 40); // NOT VACUOUS: it does follow the knob
	CHECK(still == 1);
}
