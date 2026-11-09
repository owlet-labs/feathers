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

// THE SMOOTH SHAPER — the reference's sine shaper, ported.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/smooth/smooth.h"
#include "fx/smooth/smooth_types.h"

#include <algorithm>
#include <complex>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace {

/// THE REFERENCE'S 0 dBFS — `EFFECTIVE_0DBFS_Q31 = ONE_Q31 / 128`, the level a full voice enters at.
constexpr int32_t kZeroDbfs = 1 << 24;
constexpr int32_t kOneQ31 = 2147483647;
/// One of eight zones of a 0..ONE_Q31 knob, as `computeZoneWidth(8)` has it.
constexpr int32_t kZoneWidth = kOneQ31 / 8;

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS AND THE CODEC.                                                                        //
//--------------------------------------------------------------------------------------------------//

namespace {
namespace smooth = feathers::fx::smooth;
using smooth::Param;

smooth::Row rowWith(std::initializer_list<std::pair<Param, float>> values) {
	smooth::Row row = smooth::rowOf(smooth::State {});
	for (const auto& [p, v] : values) {
		row[static_cast<size_t>(p)] = v;
	}
	return row;
}
} // namespace

TEST_CASE("the smooth shaper's defaults are the firmware's, except Mix and Poly, which are decisions") {
	const smooth::Settings d = smooth::stateOf(smooth::Row {}, 0).settings;
	CHECK(d == smooth::Settings {});
	CHECK(d.enabled);
	// Per voice by default (design: Placement), and full Mix (the firmware's 127; its 0 means "off").
	CHECK(d.poly);
	CHECK(d.mix == 1.0f);
	// The firmware's own defaults: drive at unity, harmonic and twist at 0, no phases.
	CHECK(d.drive == 0.5f); // the fader at unity
	CHECK(d.harmonicMode == 0);
	CHECK(d.harmonic == 0.0f);
	CHECK(d.sweep == 0.0f);
	// Twist's sweep and the four pills are OFF: an added device colours nothing until a power is pressed.
	CHECK_FALSE(d.sweepOn);
	for (size_t i = 0; i < smooth::kPillCount; ++i) {
		CHECK_FALSE(d.pillOn[i]);
	}
	CHECK(d.harmonicPhase == 0.0f);
	CHECK(d.gamma == 0.0f);
	// And the Settings initialisers agree with the descriptors, control by control.
	const smooth::Row row = smooth::rowOf(smooth::State {});
	for (size_t i = 0; i < std::size(smooth::kParamDescs); ++i) {
		INFO(smooth::kParamDescs[i].name);
		CHECK(row[i] == static_cast<float>(smooth::kParamDescs[i].def));
	}
}

TEST_CASE("the smooth shaper's codec round-trips, coerces and fills what is absent") {
	smooth::State s;
	s.settings.enabled = false;
	s.settings.poly = false;
	s.settings.drive = 0.25f;
	s.settings.harmonicMode = 5;
	s.settings.harmonic = 2.75f;
	s.settings.sweep = -1.4f;
	s.settings.sweepOn = true;
	s.settings.pillAmount = { 0.1f, 0.2f, 0.3f, 0.4f };
	s.settings.pillOn = { true, false, true, false };
	s.settings.mix = 0.3f;
	s.settings.harmonicPhase = 3.0f;
	s.settings.gamma = 7.0f;
	CHECK(smooth::stateOf(smooth::rowOf(s), smooth::rowOf(s).size()) == s);

	// Out of range clamps; a stepped control rounds; NaN is the default. The two coordinates and the two phases
	// are UNBOUNDED — a negative lap is a lap — and stop only at the drag window.
	const smooth::Settings c = smooth::stateOf(rowWith({ { Param::Drive, 3.0f },
	                                                     { Param::HarmonicMode, 2.6f },
	                                                     { Param::Harmonic, -1.0f },
	                                                     { Param::Sweep, 1.0e9f },
	                                                     { Param::Rect, 2.0f },
	                                                     { Param::Mix, NAN },
	                                                     { Param::Gamma, -5.0f } }),
	                                           smooth::rowOf(s).size())
	                               .settings;
	CHECK(c.drive == 1.0f);
	CHECK(c.harmonicMode == 3);
	CHECK(c.harmonic == -1.0f);
	CHECK(c.sweep == static_cast<float>(smooth::kPhaseMax));
	CHECK(c.pillAmount[static_cast<size_t>(smooth::Pill::Rect)] == 1.0f);
	CHECK(c.mix == 1.0f);
	CHECK(c.gamma == -5.0f);

	// Fields past `fieldsPresent` are their declared defaults.
	const smooth::Settings partial = smooth::stateOf(smooth::rowOf(s), 3).settings;
	CHECK(partial.drive == 0.25f);
	CHECK(partial.harmonicMode == 0);
	CHECK(partial.mix == 1.0f);

	smooth::State set;
	smooth::setValueOf(&set, Param::Sweep, 0.4f);
	CHECK(smooth::valueOf(set, Param::Sweep) == 0.4f);
	smooth::setValueOf(&set, Param::Sweep, NAN);
	CHECK(smooth::valueOf(set, Param::Sweep) == 0.4f);
}

TEST_CASE("the controls land on the firmware's q31 knobs exactly where the design says") {
	smooth::Settings s;
	// DRIVE: the FULL law, a fader — silent at 0, 0.25x, unity at the centre, 2.25x, and 4x at the top (the q31
	// saturates one short of 2^31, and the firmware's float of it is 4.0 exactly).
	s.drive = 0.0f;
	CHECK(smooth::firmwareControlsOf(s).drive == INT32_MIN);
	CHECK(smooth::driveGainOf(smooth::firmwareControlsOf(s).drive) == 0.0f);
	s.drive = 0.25f;
	CHECK(smooth::firmwareControlsOf(s).drive == -(1 << 30));
	CHECK(smooth::driveGainOf(smooth::firmwareControlsOf(s).drive) == 0.25f);
	s.drive = 0.5f;
	CHECK(smooth::firmwareControlsOf(s).drive == 0);
	CHECK(smooth::driveGainOf(0) == 1.0f);
	s.drive = 0.75f;
	CHECK(smooth::firmwareControlsOf(s).drive == (1 << 30));
	CHECK(smooth::driveGainOf(smooth::firmwareControlsOf(s).drive) == 2.25f);
	s.drive = 1.0f;
	CHECK(smooth::firmwareControlsOf(s).drive == INT32_MAX);
	CHECK(smooth::driveGainOf(smooth::firmwareControlsOf(s).drive) == 4.0f);

	// MIX: 1.0 is the firmware's 127 << 24, never quite wet; 0 is off.
	s.mix = 1.0f;
	CHECK(smooth::firmwareControlsOf(s).mix == 127 << 24);
	s.mix = 0.0f;
	CHECK(smooth::firmwareControlsOf(s).mix == 0);

	// HARMONIC: the mode is the zone, the coordinate's position IN ITS LAP the offset inside it, and the lap
	// whole units of `harmonicPhaseOffset`. Just short of a lap stays in its zone, where
	// `computeZoneQ31` stops the offset; a whole lap is the zone's start with the offset one higher.
	for (int mode = 0; mode < 8; ++mode) {
		s.harmonicMode = mode;
		s.harmonic = std::nextafter(1.0f, 0.0f);
		const int32_t top = smooth::firmwareControlsOf(s).harmonic;
		CHECK(smooth::zoneOf(top).index == mode);
		CHECK(smooth::firmwareControlsOf(s).harmonicPhaseOffset == 0.0f);
		s.harmonic = 0.0f;
		CHECK(smooth::firmwareControlsOf(s).harmonic == mode * kZoneWidth);
		for (const float lap : { 1.0f, 3.0f, -2.0f }) {
			s.harmonic = lap + 0.25f;
			const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
			CAPTURE(lap);
			CHECK(fc.harmonic == mode * kZoneWidth + static_cast<int32_t>(std::llround(0.25 * kZoneWidth)));
			CHECK(fc.harmonicPhaseOffset == lap);
		}
	}
	s.harmonic = 0.0f;

	// THE SWEEP: its LAP the firmware's `twistPhaseOffset`, counted as the encoder's auto-wrap counts it — one per
	// lap, negative below zero — and its position over the whole q31 off the named lap, in the meta zones on it.
	s.sweepOn = true;
	for (const float lap : { 1.0f, 5.0f, -1.0f, -3.0f }) {
		s.sweep = lap + 0.75f;
		const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
		CAPTURE(lap);
		CHECK(fc.twist == static_cast<int32_t>(std::llround(0.75 * kOneQ31)));
		CHECK(fc.twistPhaseOffset == lap);
	}
	s.sweep = 0.75f;
	CHECK(smooth::firmwareControlsOf(s).twist
	      == kOneQ31 / 2 + static_cast<int32_t>(std::llround(0.75 * (kOneQ31 - kOneQ31 / 2))));
	CHECK(smooth::firmwareControlsOf(s).twistPhaseOffset == 0.0f);
	// Off, the knob at zero with no lap, wherever the coordinate stands.
	s.sweepOn = false;
	CHECK(smooth::firmwareControlsOf(s).twist == 0);
	CHECK(smooth::firmwareControlsOf(s).twistPhaseOffset == 0.0f);
	s.sweep = 0.0f;

	// THE TWO PHASES, in the menu's clicks — and H Phase's eighths ride on Harmonic's laps.
	s.harmonicPhase = 3.0f;
	s.gamma = 3.0f;
	CHECK(smooth::firmwareControlsOf(s).twistPhaseOffset == 0.0f);
	CHECK(smooth::firmwareControlsOf(s).harmonicPhaseOffset == 0.375f);
	CHECK(smooth::firmwareControlsOf(s).gammaPhase == 3.0f);
	s.harmonic = 2.5f;
	CHECK(smooth::firmwareControlsOf(s).harmonicPhaseOffset == 2.375f);
}

TEST_CASE("a lap is exact in float: position in [0, 1), whole a whole number, and the two sum back to the coordinate") {
	for (const float x : { 0.0f, 0.3f, 0.999999f, 1.0f, 1.3f, 17.625f, -0.25f, -1.0f, -3.7f, 1000.001f,
	                       std::nextafter(0.0f, -1.0f), std::nextafter(2.0f, 0.0f) }) {
		const smooth::Lap l = smooth::lapOf(x);
		CAPTURE(x);
		CHECK(l.position >= 0.0f);
		CHECK(l.position < 1.0f);
		CHECK(l.whole == std::floor(l.whole));
		// The guard: the smallest negative float's position rounds up to 1, so it is on the next lap at 0.
		if (x != std::nextafter(0.0f, -1.0f)) {
			CHECK(l.whole + l.position == x);
		}
	}
	CHECK(smooth::lapOf(std::nextafter(0.0f, -1.0f)).whole == 0.0f);
	CHECK(smooth::lapOf(NAN).whole == 0.0f);
}

TEST_CASE("wrapUnit is fmod at and above zero, bit for bit, and wraps by floor below it") {
	// AT AND ABOVE ZERO: the firmware's `fmod(x, 1)` exactly — so every setting the firmware can reach is its.
	for (int i = 0; i < 20000; ++i) {
		const float x = static_cast<float>(i) * 0.000731f + 1.0f;
		CHECK(smooth::wrapUnit(x) == std::fmod(x, 1.0f));
	}
	CHECK(smooth::wrapUnit(1.0f) == 0.0f);
	// BELOW: into [0, 1), where `fmod` would hand back a negative position.
	CHECK(smooth::wrapUnit(-0.75f) == 0.25f);
	CHECK(smooth::wrapUnit(-2.25f) == 0.75f);
	CHECK(std::fmod(-0.75f, 1.0f) == -0.75f); // what the port refuses
	CHECK(smooth::wrapUnit(std::nextafter(0.0f, -1.0f)) == 0.0f);
	CHECK(smooth::wrapUnit(-3.0f) == 0.0f);
}

TEST_CASE("unbounded Harmonic: the Chebyshev blend runs straight on across a lap, and the FM input gain saws back") {
	// WITH H PHASE AT ZERO THE BLEND BANK'S PHASE IS THE COORDINATE ITSELF: the firmware evaluates it at
	// `pos + freqMod`, and the port hands it pos = frac(h), freqMod = floor(h). So across a whole lap the
	// Chebyshev coefficients move by no more than they do across any other step of the same size — where the
	// clamped 0..1 control stopped dead, and a phase offset of one would have JUMPED from the top to the bottom.
	smooth::Settings s;
	s.harmonicMode = 0;
	const float eps = 1.0e-4f;
	const auto targets = [&](float h) {
		s.harmonic = h;
		const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
		const smooth::TwistParams tw = smooth::twistParamsOf(fc.twist, fc.twistPhaseOffset, fc.harmonicPhaseOffset,
		                                                    fc.gammaPhase);
		return smooth::targetsOf(0, smooth::zoneOf(fc.harmonic).position, tw, 0.0f);
	};
	const auto biggestStep = [](const smooth::CoefficientTargets& a, const smooth::CoefficientTargets& b) {
		float d = 0.0f;
		for (size_t i = 0; i < 5; ++i) {
			d = std::max(d, std::abs(a[i] - b[i]));
		}
		return d;
	};
	// CONTINUOUS MEANS THE STEP SHRINKS WITH THE GAP: a tenth of the gap, about a tenth of the step (the blend's
	// triangles are piecewise linear in phase before the cube). A jump would stay the size of the jump.
	const float acrossLap = biggestStep(targets(1.0f - eps), targets(1.0f + eps));
	const float acrossLapFine = biggestStep(targets(1.0f - 0.1f * eps), targets(1.0f + 0.1f * eps));
	CAPTURE(acrossLap);
	CAPTURE(acrossLapFine);
	CHECK(acrossLapFine < 0.25f * acrossLap);
	// AND THE SIZE OF THE JUMP THE CLAMPED CONTROL WOULD HAVE MADE — the top of the lap against its bottom on the
	// SAME bank offset, which is what a 1.0 → 0.0 wrap with no carry would be — is far larger than either.
	s.harmonic = 1.0f - eps;
	const smooth::CoefficientTargets top = targets(1.0f - eps);
	const smooth::CoefficientTargets wrappedWithoutCarry = targets(0.0f + eps);
	CHECK(biggestStep(top, wrappedWithoutCarry) > 10.0f * acrossLap);
	// THE FM ZONES' INPUT GAIN IS `1 + 3 · pos` AND SAWS BACK AT EVERY LAP — the firmware's own jump whenever H
	// Phase carries the position across its wrap, kept by decision (2026-09-26).
	s.harmonicMode = 2;
	const auto fmGain = [&](float h) {
		s.harmonic = h;
		const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
		const smooth::TwistParams tw = smooth::twistParamsOf(fc.twist, fc.twistPhaseOffset, fc.harmonicPhaseOffset,
		                                                    fc.gammaPhase);
		return smooth::targetsOf(2, smooth::zoneOf(fc.harmonic).position, tw, 0.0f)[0];
	};
	CHECK(fmGain(1.0f - eps) == doctest::Approx(4.0f).epsilon(1e-3));
	CHECK(fmGain(1.0f + eps) == doctest::Approx(1.0f).epsilon(1e-3));
	CHECK(fmGain(2.5f) == doctest::Approx(2.5f).epsilon(1e-5));

	// AND POLY'S CASCADE BLEND IS THE POSITION, SO IT SAWS TOO; its self-multiplication blend is H Phase's.
	s.harmonicMode = 7;
	s.harmonic = 3.25f;
	s.harmonicPhase = 2.0f; // a quarter
	const smooth::Picture poly = smooth::pictureOf(s);
	CHECK(poly.weights.c1 == doctest::Approx(0.25f).epsilon(1e-6));
	CHECK(poly.weights.c3 == doctest::Approx(0.75f).epsilon(1e-6)); // wrap(3 + 0.25 + 0.5)
}

TEST_CASE("below zero the port wraps by floor, where the firmware's fmod would stand the blend at a negative position") {
	// h = -1.75: position 0.25 on lap -2. The firmware never gets here (its phases are clamped at zero); its fmod
	// would give pos = -0.75, an FM input gain of -1.25 and a clamped-to-zero blend. The port's position is 0.25,
	// and the bank phase is h, continuous with the laps above.
	smooth::Settings s;
	s.harmonicMode = 3; // Fold
	s.harmonic = -1.75f;
	const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
	CHECK(fc.harmonicPhaseOffset == -2.0f);
	const smooth::TwistParams tw = smooth::twistParamsOf(fc.twist, fc.twistPhaseOffset, fc.harmonicPhaseOffset,
	                                                    fc.gammaPhase);
	const smooth::CoefficientTargets t = smooth::targetsOf(3, smooth::zoneOf(fc.harmonic).position, tw, 0.0f);
	CHECK(t[0] == doctest::Approx(1.75f).epsilon(1e-6)); // 1 + 3 · 0.25
	const std::array<float, 4> w = smooth::blendWeightsOf(0.25f, -2.0);
	CHECK(t[1] == w[0]);
	CHECK(t[4] == w[3]);
	// AND POLY'S SELF-MULTIPLICATION BLEND stays in [0, 1): wrap(-2 + 0.5) = 0.5, not fmod's -0.5.
	s.harmonicMode = 7;
	CHECK(smooth::pictureOf(s).weights.c3 == doctest::Approx(0.5f).epsilon(1e-6));
}

namespace {

/// THE OLD TWIST COORDINATE `t`, AS THE NEW CONTROLS — so the frame-for-frame sweeps keep their coverage. On the
/// named lap (whole part plus 1024 · Gamma zero) zones 0-3 are their pill (Width and Evens at the zone's own depth
/// at the extras phase, so the case still IS a firmware state) and zones 4-7 the sweep's lap 0; elsewhere the
/// sweep at `t`. Call it AFTER Harmonic, H Phase and Gamma are set: the extras phase reads them.
void applyTwist(smooth::Settings& s, float t) {
	s.sweepOn = false;
	s.pillOn = {};
	const smooth::Lap lap = smooth::lapOf(t);
	const bool named = static_cast<double>(lap.whole) + 1024.0 * static_cast<double>(s.gamma) == 0.0;
	if (!named) {
		s.sweepOn = true;
		s.sweep = t;
		return;
	}
	const int zone = std::min(static_cast<int>(lap.position * 8.0f), 7);
	const float p = lap.position * 8.0f - static_cast<float>(zone);
	if (zone >= 4) {
		s.sweepOn = true;
		s.sweep = (lap.position - 0.5f) * 2.0f;
		return;
	}
	const auto pill = static_cast<smooth::Pill>(zone);
	float amount = p;
	if ((pill == smooth::Pill::Width || pill == smooth::Pill::Evens) && p > 0.0f) {
		const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
		const smooth::TwistParams r = smooth::twistParamsOf(
		    smooth::pillQ31Of(pill, 0.0f, smooth::twistExtrasPhaseOf(fc)), 0.0f, fc.harmonicPhaseOffset, 0.0f);
		amount = pill == smooth::Pill::Width ? r.stereoWidth : std::max(r.evenAmount, r.evenDryBlend);
	}
	s.pillAmount[static_cast<size_t>(pill)] = amount;
	s.pillOn[static_cast<size_t>(pill)] = amount > 0.0f;
}

} // namespace

TEST_CASE("Twist is the sweep alone: no lap of it, at any Gamma, is a named zone") {
	// Width, Evens, Rect and Fdbk are pills now, and a coarse drag of Twist never lands in one.
	// The firmware branches on a named zone only when `twistPhaseOffset + 1024 · gammaPhase` is zero AND the knob
	// is below zone 4 — so the claim is: whenever the first holds, the knob is at or past zone 4.
	constexpr int32_t kZone4 = kOneQ31 / 2;
	size_t named = 0;
	size_t swept = 0;
	for (float g : { 0.0f, 1.0f, 0.37f, -2.0f }) {
		for (float hph : { 0.0f, 3.0f }) {
			for (int i = -40; i <= 40; ++i) {
				smooth::Settings s;
				s.sweepOn = true;
				s.sweep = 0.0371f * static_cast<float>(i) + (g == -2.0f ? 2048.0f : 0.0f);
				s.gamma = g;
				s.harmonic = 0.3f;
				s.harmonicPhase = hph;
				const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
				CAPTURE(s.sweep);
				CAPTURE(g);
				const bool namedLap = static_cast<double>(fc.twistPhaseOffset) + 1024.0 * fc.gammaPhase == 0.0;
				if (namedLap) {
					CHECK(fc.twist >= kZone4);
					++named;
				}
				else {
					++swept;
				}
			}
		}
	}
	// BOTH BRANCHES WERE VISITED, so neither claim above is vacuous.
	CHECK(named > 20);
	CHECK(swept > 20);
	// OFF, the sweep adds nothing at all, wherever it stands and whatever Gamma is.
	smooth::Settings off;
	off.sweep = 0.6f;
	off.gamma = 3.0f;
	const smooth::TwistParams z = smooth::twistOf(smooth::firmwareControlsOf(off));
	CHECK(z.stereoWidth == 0.0f);
	CHECK(z.evenAmount == 0.0f);
	CHECK(z.evenDryBlend == 0.0f);
	CHECK(z.rectAmount == 0.0f);
	CHECK(z.feedbackAmount == 0.0f);
}

TEST_CASE("one pill alone: Width takes the pill's depth at its zone's LFO, and Harmonic's position picks Evens' flavour") {
	// THE PILL SETS THE DEPTH; HARMONIC'S POSITION (with H Phase and Gamma) SETS THE CHARACTER.
	// Width at a depth its zone does not have at this phase keeps the zone's LFO character and takes the pill's depth.
	smooth::Settings w;
	w.harmonic = 0.9f; // the zone's speed-up quarter: its own width is 0.4 here, its LFO faster
	w.pillOn[0] = true;
	w.pillAmount[0] = 1.0f;
	const smooth::TwistParams ours = smooth::twistOf(smooth::firmwareControlsOf(w));
	CHECK(ours.stereoWidth == 1.0f);
	CHECK(ours.stereoFreqMult > 2.0f);
	// AND HARMONIC'S POSITION PICKS EVENS' FLAVOUR: the positive half compressed below 0.5, the negative half
	// blended toward the dry above it.
	smooth::Settings ev;
	ev.pillOn[1] = true;
	ev.pillAmount[1] = 0.6f;
	ev.harmonic = 0.2f;
	const smooth::TwistParams lo = smooth::twistOf(smooth::firmwareControlsOf(ev));
	CHECK(lo.evenAmount == 0.6f);
	CHECK(lo.evenDryBlend == 0.0f);
	ev.harmonic = 0.7f;
	const smooth::TwistParams hi = smooth::twistOf(smooth::firmwareControlsOf(ev));
	CHECK(hi.evenAmount == 0.0f);
	CHECK(hi.evenDryBlend == 0.6f);
}

TEST_CASE("the four pills sound together, and over the sweep they add, clamped to the firmware's ranges") {
	// THE DELIBERATE DEPARTURE (2026-10-03): the firmware applies one twist at a time. The four write disjoint
	// fields, so together each is what it is alone — restated here from the single-pill road, not from `twistOf`.
	smooth::Settings s;
	s.harmonic = 0.3f;
	s.pillAmount = { 0.8f, 0.4f, 0.6f, 0.5f };
	const auto alone = [&](size_t i) {
		smooth::Settings one = s;
		one.sweepOn = false;
		one.pillOn = {};
		one.pillOn[i] = true;
		return smooth::twistOf(smooth::firmwareControlsOf(one));
	};
	s.pillOn = { true, true, true, true };
	const smooth::TwistParams all = smooth::twistOf(smooth::firmwareControlsOf(s));
	CHECK(all.stereoWidth == alone(0).stereoWidth);
	CHECK(all.stereoFreqMult == alone(0).stereoFreqMult);
	CHECK(all.stereoPhaseOffset == alone(0).stereoPhaseOffset);
	CHECK(all.evenAmount == alone(1).evenAmount);
	CHECK(all.evenDryBlend == alone(1).evenDryBlend);
	CHECK(all.rectAmount == alone(2).rectAmount);
	CHECK(all.feedbackAmount == alone(3).feedbackAmount);
	// ALL FOUR ARE LIVE AT ONCE — the thing no firmware state is.
	CHECK(all.stereoWidth == 0.8f);
	CHECK(all.evenAmount == 0.4f);
	CHECK(all.rectAmount > 0.5f);
	CHECK(all.feedbackAmount > 0.12f);

	// OVER THE SWEEP: the sweep's own field plus the pill's, capped at the firmware's ceilings (1, and 0.25 for
	// feedback); Width's LFO speed multiplies; its phase adds and wraps.
	smooth::Settings sweep = s;
	sweep.pillOn = {};
	sweep.sweepOn = true;
	sweep.sweep = 0.4f;
	const smooth::TwistParams under = smooth::twistOf(smooth::firmwareControlsOf(sweep));
	CHECK(under.stereoWidth + under.evenAmount + under.rectAmount + under.feedbackAmount > 0.0f); // not vacuous
	s.sweepOn = true;
	s.sweep = 0.4f;
	const smooth::TwistParams over = smooth::twistOf(smooth::firmwareControlsOf(s));
	CHECK(over.stereoWidth == std::min(under.stereoWidth + 0.8f, 1.0f));
	CHECK(over.stereoFreqMult == under.stereoFreqMult * alone(0).stereoFreqMult);
	CHECK(over.stereoPhaseOffset == smooth::wrapUnit(under.stereoPhaseOffset + alone(0).stereoPhaseOffset));
	CHECK(over.stereoLfoRate == under.stereoLfoRate);
	CHECK(over.evenAmount == std::min(under.evenAmount + 0.4f, 1.0f));
	CHECK(over.evenDryBlend == under.evenDryBlend);
	CHECK(over.rectAmount == std::min(under.rectAmount + alone(2).rectAmount, 1.0f));
	CHECK(over.feedbackAmount == std::min(under.feedbackAmount + alone(3).feedbackAmount, 0.25f));
	CHECK(over.phaseHarmonic == under.phaseHarmonic);
	// FULL PILLS OVER IT PIN EVERY CEILING.
	s.pillAmount = { 1.0f, 1.0f, 1.0f, 1.0f };
	const smooth::TwistParams full = smooth::twistOf(smooth::firmwareControlsOf(s));
	CHECK(full.stereoWidth == 1.0f);
	CHECK(full.rectAmount == 1.0f);
	CHECK(full.feedbackAmount <= 0.25f);
	CHECK(full.feedbackAmount >= 0.2499f);
}

TEST_CASE("a full-depth route moves Harmonic by one lap and Twist by a quarter lap, and neither stops at a lap") {
	const feathers::device::ParamDesc& h = smooth::kParamDescs[static_cast<size_t>(Param::Harmonic)];
	const feathers::device::ParamDesc& t = smooth::kParamDescs[static_cast<size_t>(Param::Sweep)];
	CHECK(feathers::device::routeSpanOf(h) == 1.0);
	CHECK(feathers::device::routeSpanOf(t) == 0.25);
	// THE FIRMWARE'S CLIP-TO-ZONE IS GONE FROM THE COORDINATE (the Mode menu holds the algorithm now), and its
	// cable clamp at the knob's ends with it: a route carries into the next lap.
	CHECK(feathers::device::withRouteOffset(h, 0.9, 0.3) == doctest::Approx(1.2));
	CHECK(feathers::device::withRouteOffset(t, 0.95, 0.25) == doctest::Approx(1.2));
	CHECK(feathers::device::withRouteOffset(t, 0.05, -0.25) == doctest::Approx(-0.2));
	// THE DIGIT ROAD NEEDS A READOUT THAT IS THE VALUE: both are Numbers, not Percents (the "coarse/fine" that was
	// missing).
	CHECK(h.unit == feathers::device::Unit::Number);
	CHECK(t.unit == feathers::device::Unit::Number);
}

//--------------------------------------------------------------------------------------------------//
// SETTINGS FROM THE OLD KNOBS.                                                                       //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A q31 the port can be handed EXACTLY: one a float holds, so `x · 2^24` in the port is this integer.
int32_t representable(double q) {
	const double c = std::clamp(q, -2147483520.0, 2147483520.0);
	return static_cast<int32_t>(static_cast<float>(c));
}

/// THE OLD DRIVE KNOB (-1..1 over the voice path's 0.25..2.25x) AS THE FADER: 0.5 + knob / 4 lands on the SAME q31,
/// knob · 2^30, so the sweeps below mean what they meant before Drive became a fader (2026-09-26).
float faderOf(float knob) {
	return 0.5f + 0.25f * knob;
}

/// `drive` IS IN THE OLD KNOB'S UNITS (see `faderOf`).
smooth::Settings settingsWith(int mode, float harmonic, float drive, float twist) {
	smooth::Settings s;
	s.harmonicMode = mode;
	s.harmonic = harmonic;
	s.drive = faderOf(drive);
	applyTwist(s, twist);
	return s;
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE GAIN STAGING — asserted as values through the probe, because it is the part that matters.      //
//--------------------------------------------------------------------------------------------------//

namespace {

/// The drive knob whose gain is `gain`: the inverse of `(1 + knob / 2)^2`.
float knobForGain(double gain) {
	return static_cast<float>(2.0 * (std::sqrt(gain) - 1.0));
}

/// Run `value` through a fresh shaper for `frames` frames, one frame per call, and return the largest
/// `driven` and `zoneInput` the probe saw on the left.
std::pair<int32_t, int32_t> probePeaks(const smooth::Settings& s, float value, int frames = 256) {
	smooth::SmoothShaper sh;
	sh.prepare(48000.0);
	sh.setTarget(s);
	int32_t driven = 0;
	int32_t zoneInput = 0;
	for (int i = 0; i < frames; ++i) {
		float l = value;
		float r = value;
		sh.processStereo(&l, &r, 1);
		driven = std::max(driven, sh.probeForTest(0).driven);
		zoneInput = std::max(zoneInput, sh.probeForTest(0).zoneInput);
	}
	return { driven, zoneInput };
}

} // namespace

TEST_CASE("a full-scale voice at Drive centre enters the Chebyshevs at x = 2 and is clipped to 1") {
	// THE DESIGN'S GAIN STAGING TABLE, AS VALUES. 1.0 here is the reference's 0 dBFS, 2^24. The Chebyshev zones
	// read x = scaledInput / 2^31 after `<< 8` saturation, so x = driven / 2^23 before the clip.
	for (int mode : { 0, 1 }) {
		smooth::Settings s = settingsWith(mode, 0.5f, 0.0f, 0.0f);
		const auto [driven, zoneInput] = probePeaks(s, 1.0f);
		INFO("mode " << mode);
		// Unity drive: the input as it came, 2^24.
		CHECK(driven == kZeroDbfs);
		// x = 2 before the clip...
		CHECK(static_cast<double>(driven) / 8388608.0 == 2.0);
		// ...and 1 after it: the `<< 8` saturation's ceiling, 1 - 2^-23.
		CHECK(zoneInput == 0x7FFFFF00);
		CHECK(static_cast<double>(zoneInput) / 2147483648.0 == doctest::Approx(1.0).epsilon(1.0e-6));
	}

	// DRIVE 0.5x — the knob at about -0.59 — PUTS A FULL VOICE EXACTLY AT x = 1, the exact-harmonics point.
	{
		smooth::Settings s = settingsWith(0, 0.5f, knobForGain(0.5), 0.0f);
		const float gain = smooth::driveGainOf(smooth::firmwareControlsOf(s).drive);
		CHECK(gain == doctest::Approx(0.5).epsilon(1.0e-6));
		const auto [driven, zoneInput] = probePeaks(s, 1.0f);
		CHECK(static_cast<double>(driven) / 8388608.0 == doctest::Approx(1.0).epsilon(1.0e-5));
		// At x = 1 the saturation is at its knee: the input passes or is one step under the ceiling.
		CHECK(static_cast<double>(zoneInput) / 2147483648.0 == doctest::Approx(1.0).epsilon(1.0e-5));
	}

	// ZONE 7 (Poly) CLIPS AT THE REFERENCE'S 0 dBFS, 1.0 — not at half of it.
	{
		smooth::Settings s = settingsWith(7, 0.5f, 0.0f, 0.0f);
		const auto full = probePeaks(s, 1.0f);
		CHECK(full.first == kZeroDbfs);
		CHECK(full.second == 0x7FFFFF80); // (2^24 - 1) << 7: at the ceiling
		const auto half = probePeaks(s, 0.5f);
		CHECK(half.second == 1 << 30); // 0.5 passes untouched: x = 0.5
	}

	// AND THE FM ZONES ARE NOT CLIPPED AT ALL: the driven input is their phase, so 1.0 at 2.25x drive is
	// still 2.25 of the reference's 0 dBFS into the operator.
	{
		smooth::Settings s = settingsWith(2, 0.5f, 1.0f, 0.0f);
		const auto [driven, zoneInput] = probePeaks(s, 1.0f);
		CHECK(driven == static_cast<int32_t>(2.25 * kZeroDbfs));
		CHECK(zoneInput == driven);
	}
}

//--------------------------------------------------------------------------------------------------//
// WHAT THE PORT PROMISES THAT THE FIRMWARE DOES NOT.                                                 //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A passage with automation at fixed frames, rendered with the host cutting its calls by `cuts`.
std::vector<float> renderPassage(double rate, bool stereo, const std::vector<int>& cuts, int frames) {
	smooth::SmoothShaper sh;
	sh.prepare(rate);
	std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
	for (int i = 0; i < frames; ++i) {
		const double t = static_cast<double>(i) / rate;
		l[static_cast<size_t>(i)] = static_cast<float>(0.9 * std::sin(2.0 * M_PI * 180.0 * t));
		r[static_cast<size_t>(i)] = static_cast<float>(0.7 * std::sin(2.0 * M_PI * 250.0 * t));
	}
	// Automation every 300 frames, crossing zones — the smoother is travelling for most of it.
	const auto settingsAt = [](int segment) {
		smooth::Settings s;
		s.harmonicMode = (segment / 2) % 8;
		s.harmonic = std::fmod(0.21f * static_cast<float>(segment), 1.0f);
		s.drive = faderOf(std::sin(0.9f * static_cast<float>(segment)));
		applyTwist(s, std::fmod(0.117f * static_cast<float>(segment), 1.0f));
		return s;
	};
	int at = 0;
	size_t cut = 0;
	while (at < frames) {
		sh.setTarget(settingsAt(at / 300));
		const int segmentEnd = std::min(frames, (at / 300 + 1) * 300);
		int n = std::min(cuts[cut % cuts.size()], segmentEnd - at);
		++cut;
		if (stereo) {
			sh.processStereo(l.data() + at, r.data() + at, n);
		}
		else {
			sh.processMono(l.data() + at, n);
		}
		at += n;
	}
	if (stereo) {
		l.insert(l.end(), r.begin(), r.end());
	}
	return l;
}

} // namespace

TEST_CASE("the output does not depend on how the host cuts its blocks") {
	for (bool stereo : { true, false }) {
		const std::vector<float> reference = renderPassage(48000.0, stereo, { 128 }, 9600);
		for (const std::vector<int>& cuts : std::vector<std::vector<int>> { { 1 }, { 7, 64, 3, 500 }, { 4096 }, { 33 } }) {
			INFO("stereo " << stereo << ", first cut " << cuts.front());
			CHECK(renderPassage(48000.0, stereo, cuts, 9600) == reference);
		}
	}
}

TEST_CASE("the firmware's 44.1 kHz time constants are its own numbers there and the same TIMES elsewhere") {
	smooth::SmoothShaper at44;
	at44.prepare(44100.0);
	// Bit for bit at the rate they were written for.
	CHECK(at44.perSampleAlphaForTest() == 0.005f);
	CHECK(at44.hpfAlphaForTest() == static_cast<int32_t>(0.01425 * kOneQ31));
	CHECK(at44.feedbackPoleForTest() == 1 << 30); // the firmware's `>> 1`

	smooth::SmoothShaper at96;
	at96.prepare(96000.0);
	// The per-frame IIRs and the highpass scale with the frame's length...
	CHECK(at96.perSampleAlphaForTest() == doctest::Approx(0.005 * 44100.0 / 96000.0).epsilon(1e-6));
	CHECK(static_cast<double>(at96.hpfAlphaForTest())
	      == doctest::Approx(static_cast<double>(at44.hpfAlphaForTest()) * 44100.0 / 96000.0).epsilon(1e-6));
	// ...and the feedback tap's pole as a power: one second of it decays by the same amount at both rates.
	const auto decayPerSecond = [](const smooth::SmoothShaper& s, double rate) {
		return rate * std::log(1.0 - static_cast<double>(s.feedbackPoleForTest()) / 2147483648.0);
	};
	CHECK(decayPerSecond(at96, 96000.0) == doctest::Approx(decayPerSecond(at44, 44100.0)).epsilon(1e-6));

	// AND IN MOTION: the drive's glide covers the same ground in 20 ms at either rate.
	const auto glideAfter = [](double rate, double seconds) {
		smooth::SmoothShaper sh;
		sh.prepare(rate);
		smooth::Settings s = settingsWith(0, 0.5f, -1.0f, 0.0f);
		sh.setTarget(s);
		std::vector<float> l(static_cast<size_t>(rate), 0.0f), r = l;
		sh.processStereo(l.data(), r.data(), 128); // placed at 0.25x
		s.drive = faderOf(1.0f);
		sh.setTarget(s);
		const int frames = static_cast<int>(std::lround(seconds * rate / 128.0)) * 128;
		sh.processStereo(l.data(), r.data(), frames);
		return sh.driveGainForTest();
	};
	const float g44 = glideAfter(44100.0, 0.02);
	const float g96 = glideAfter(96000.0, 0.02);
	CHECK(g44 > 1.5f); // it has moved a long way from 0.25 toward 2.25...
	CHECK(g44 < 2.24f); // ...and not all of it
	CHECK(g96 == doctest::Approx(g44).epsilon(0.01));

	// AND THE STEREO LFO turns at the same rate in hertz: Width at the start of its zone, 0.05 Hz.
	const auto lfoAdvancePerSecond = [](double rate) {
		smooth::SmoothShaper sh;
		sh.prepare(rate);
		sh.setTarget(settingsWith(0, 0.5f, 0.0f, 0.05f));
		std::vector<float> l(128, 0.0f), r = l;
		sh.processStereo(l.data(), r.data(), 128);
		const float start = sh.lfoPhaseForTest();
		const int blocks = static_cast<int>(rate / 128.0);
		for (int i = 0; i < blocks; ++i) {
			sh.processStereo(l.data(), r.data(), 128);
		}
		return static_cast<double>(sh.lfoPhaseForTest() - start) / (blocks * 128.0 / rate);
	};
	CHECK(lfoAdvancePerSecond(44100.0) == doctest::Approx(0.05).epsilon(1e-3));
	CHECK(lfoAdvancePerSecond(96000.0) == doctest::Approx(0.05).epsilon(1e-3));
}

TEST_CASE("a new note starts the shaper as if just switched on, whatever the slot played before") {
	// THE PER-VOICE RESET (`voice.cpp:377` clears SineShaperVoiceState; here the smoother is the
	// instance's too, so it goes as well): a slot that played one note, reset, then a second, renders the
	// second exactly as a fresh instance does.
	//
	// EACH PIECE OF STATE MUST BE BOTH LEFT BEHIND AND READ, or the case passes with its reset deleted — a
	// first draft's second note sat in the Width zone, where the feedback amount is zero, the stale tap is
	// overwritten before anything reads it, and a reset that forgot the feedback state went GREEN. So both
	// notes are played in the feedback zone (which leaves and reads the tap) and in the Width zone (which
	// leaves the LFO turning, and reads it on the next note's first block, before its seeding).
	const auto note = [](smooth::SmoothShaper& sh, const smooth::Settings& s) {
		sh.setTarget(s);
		std::vector<float> l(2000), r(2000);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = static_cast<float>(std::sin(0.03 * static_cast<double>(i)));
			r[i] = static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
		}
		sh.processStereo(l.data(), r.data(), 700);
		sh.processStereo(l.data() + 700, r.data() + 700, 1300);
		l.insert(l.end(), r.begin(), r.end());
		return l;
	};
	for (int mode : { 0, 3, 7 }) {
		for (float twistA : { 0.43f, 0.06f }) {     // feedback zone, width zone
			for (float twistB : { 0.47f, 0.06f }) { // the same two
				smooth::SmoothShaper used;
				used.prepare(48000.0);
				(void)note(used, settingsWith(mode, 0.2f, 1.0f, twistA));
				if (twistA > 0.4f) {
					CHECK(used.feedbackStateForTest(0) != 0); // the first note left something behind
				}
				else {
					CHECK(used.lfoPhaseForTest() != 0.0f);
				}
				CHECK(used.dcStateForTest(0) != 0);
				used.reset();
				smooth::SmoothShaper fresh;
				fresh.prepare(48000.0);
				const smooth::Settings b = settingsWith(mode, 0.8f, -0.4f, twistB);
				INFO("mode " << mode << ", twists " << twistA << " then " << twistB);
				CHECK(note(used, b) == note(fresh, b));
			}
		}
	}
}

TEST_CASE("NaN, infinity and silence are all handled: nothing escapes and silence stays silence") {
	for (int mode = 0; mode < 8; ++mode) {
		for (float twist : { 0.05f, 0.2f, 0.3f, 0.45f, 0.8f }) {
			smooth::SmoothShaper sh;
			sh.prepare(48000.0);
			sh.setTarget(settingsWith(mode, 0.7f, 1.0f, twist));
			// SILENCE IN IS EXACTLY SILENCE OUT, in every zone and Twist — every shaper maps 0 to 0, and the
			// integer state never leaves zero, so there is no tail to denormalise.
			std::vector<float> l(1000, 0.0f), r(1000, 0.0f);
			sh.processStereo(l.data(), r.data(), 1000);
			INFO("mode " << mode << " twist " << twist);
			CHECK(std::all_of(l.begin(), l.end(), [](float v) { return v == 0.0f; }));
			CHECK(std::all_of(r.begin(), r.end(), [](float v) { return v == 0.0f; }));
			// NaN and infinities: the input becomes 0 and the rails, the output stays finite.
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = (i % 3 == 0) ? NAN : (i % 3 == 1 ? INFINITY : -1.0e30f);
				r[i] = (i % 2 == 0) ? -INFINITY : NAN;
			}
			sh.processStereo(l.data(), r.data(), 1000);
			CHECK(std::all_of(l.begin(), l.end(), [](float v) { return std::isfinite(v); }));
			CHECK(std::all_of(r.begin(), r.end(), [](float v) { return std::isfinite(v); }));
			// AND THE INPUT READING: a NaN or an infinity never becomes the held peak the card draws.
			CHECK(std::isfinite(sh.inputPeakForTest()));
			// And a NaN in the SETTINGS is a default, never a NaN in the DSP.
			smooth::Row row = smooth::rowOf(smooth::State {});
			for (float& v : row) {
				v = NAN;
			}
			sh.setTarget(smooth::stateOf(row, row.size()).settings);
			std::fill(l.begin(), l.end(), 0.5f);
			std::fill(r.begin(), r.end(), -0.5f);
			sh.processStereo(l.data(), r.data(), 1000);
			CHECK(std::all_of(l.begin(), l.end(), [](float v) { return std::isfinite(v); }));
		}
	}
}

TEST_CASE("Mix at zero and the switch off both pass the signal through untouched") {
	std::vector<float> in(1000);
	for (size_t i = 0; i < in.size(); ++i) {
		in[i] = static_cast<float>(0.8 * std::sin(0.02 * static_cast<double>(i)));
	}
	for (int which = 0; which < 2; ++which) {
		smooth::SmoothShaper sh;
		sh.prepare(48000.0);
		smooth::Settings s = settingsWith(2, 0.5f, 1.0f, 0.3f);
		if (which == 0) {
			s.mix = 0.0f;
		}
		else {
			s.enabled = false;
		}
		sh.setTarget(s);
		std::vector<float> l = in;
		std::vector<float> r = in;
		sh.processStereo(l.data(), r.data(), 1000);
		CHECK(l == in);
		CHECK(r == in);
		sh.processMono(l.data(), 1000);
		CHECK(l == in);
	}
	// And Mix at full is NOT all wet — the firmware's 127/128: a trace of the dry is still in it.
	smooth::SmoothShaper sh;
	sh.prepare(48000.0);
	sh.setTarget(settingsWith(0, 0.5f, 0.0f, 0.0f));
	CHECK(smooth::firmwareControlsOf(sh.target()).mix == 127 << 24);
}

TEST_CASE("a new voice's coefficient smoother starts at its targets, not at the firmware's power-on values") {
	// THE PER-INSTANCE DEPARTURE, AS A VALUE. The firmware's smoother is per Sound and so is already at its
	// targets whenever the patch is still; a per-voice one gliding up from {1, 0, 0, 0, 0} would put ~5 ms of
	// clean fundamental on every note. After the first block the coefficients ARE the targets.
	smooth::SmoothShaper sh;
	sh.prepare(48000.0);
	const smooth::Settings s = settingsWith(0, 0.37f, 0.0f, 0.0f);
	sh.setTarget(s);
	std::vector<float> l(128, 0.25f), r = l;
	sh.processStereo(l.data(), r.data(), 128);
	const smooth::FirmwareControls fc = smooth::firmwareControlsOf(s);
	const smooth::TwistParams twist = smooth::twistOf(fc);
	const float pos = std::fmod(smooth::zoneOf(fc.harmonic).position + twist.phaseHarmonic + 1.0f, 1.0f);
	const smooth::Coefficients want = smooth::chebyshevCoefficientsOf(pos, twist.phaseHarmonicFreqMod);
	const auto got = sh.coefficientsForTest();
	CHECK(got[0] == want.c1);
	CHECK(got[1] == want.c3);
	CHECK(got[2] == want.c5);
	CHECK(got[3] == want.c7);
	CHECK(got[4] == want.c9);
	// And not the power-on values it would otherwise have glided from (c1 is Σw = 1 in every Chebyshev
	// blend, so it is c3 that tells them apart).
	CHECK(got[1] != 0.0f);
}

namespace {

/// ONE BURST AND THEN SILENCE, through the stereo road or the mono road (the per-voice road when the bank is
/// shut). Returns the time from the end of the burst to the last non-zero output sample, in seconds, or -1 if
/// the output was exactly silent throughout the tail; `tailPeak` is the loudest sample of the last second.
struct Tail {
	double lastSoundSeconds = -1.0;
	float tailPeak = 0.0f;
	int32_t dc = 0;
	int32_t fb = 0;
};
Tail burstThenSilence(const smooth::Settings& s, double rate, bool mono, double burstSeconds = 0.1,
                      double tailSeconds = 3.0) {
	smooth::SmoothShaper sh;
	sh.prepare(rate);
	sh.setTarget(s);
	const int burst = static_cast<int>(burstSeconds * rate);
	const int total = burst + static_cast<int>(tailSeconds * rate);
	std::vector<float> l(97), r(97);
	Tail t;
	int at = 0;
	while (at < total) {
		const int n = std::min(97, total - at); // an odd cut, on purpose: the rest is per sample
		for (int i = 0; i < n; ++i) {
			const int k = at + i;
			// A LOUD BURST: +6 dB over the reference's 0 dBFS, two tones.
			const double v = k < burst ? 2.0 * std::sin(0.071 * k) * std::cos(0.0031 * k) : 0.0;
			l[static_cast<size_t>(i)] = static_cast<float>(v);
			r[static_cast<size_t>(i)] = static_cast<float>(-v * 0.8);
		}
		if (mono) {
			sh.processMono(l.data(), n);
		}
		else {
			sh.processStereo(l.data(), r.data(), n);
		}
		for (int i = 0; i < n; ++i) {
			const int k = at + i;
			const bool sounding = l[static_cast<size_t>(i)] != 0.0f || (!mono && r[static_cast<size_t>(i)] != 0.0f);
			if (k >= burst && sounding) {
				t.lastSoundSeconds = static_cast<double>(k - burst) / rate;
			}
			if (k >= total - static_cast<int>(rate)) {
				t.tailPeak = std::max({ t.tailPeak, std::fabs(l[static_cast<size_t>(i)]),
				                        mono ? 0.0f : std::fabs(r[static_cast<size_t>(i)]) });
			}
		}
		at += n;
	}
	t.dc = sh.dcStateForTest(0);
	t.fb = sh.feedbackStateForTest(0);
	return t;
}

/// THE REST'S OWN LENGTH AND ONE FRAME AT THE SLOWER RATE: the bound a tail must meet.
constexpr double kRestSilentSecondsPlusAFrame = smooth::kRestSilentSeconds + 1.0 / 44100.0;

} // namespace

TEST_CASE("after a loud burst the output reaches EXACT zero within 50 ms, on both roads, with no denormal guard") {
	// **THE PLUGIN TELLS THE HOST IT IS DONE (CLAP_PROCESS_SLEEP) ONLY ON AN EXACTLY SILENT BLOCK** (since
	// 2026-09-25), so a tail that parks on a denormal or a DC keeps the plugin awake for ever. This suite runs
	// WITHOUT the plugin's DenormalGuard — nothing here sets flush-to-zero — so the only thing that can make the
	// tail exact zero is the device's own arithmetic: its states are integers, and the REST clears the one
	// residue integers leave (the wet highpass's dead band; see `SmoothShaper::renderFrames`).
	//
	// EVERY MODE, Twist in its feedback zone (at its maximum, 0.4999, and mid-zone), in Width and in a meta zone
	// and at zero, on the stereo road AND the mono road, at 44.1 and 96 kHz. Two arrangements do not end and
	// are not meant to: Fold and Mod with the feedback up SELF-OSCILLATE, as the firmware does (kept by decision),
	// and a sustaining device correctly stays awake. They are told apart by LEVEL — a drone holds the output
	// tens of dB above anything a residue could be — and counted, so the list cannot grow unnoticed.
#if defined(__SSE__) || defined(__x86_64__)
	// NO DENORMAL GUARD IN FORCE: MXCSR's flush-to-zero (bit 15) and denormals-are-zero (bit 6) are clear.
	CHECK((__builtin_ia32_stmxcsr() & ((1u << 15) | (1u << 6))) == 0u);
#endif
	std::vector<std::string> drones;
	double slowest = 0.0;
	for (double rate : { 44100.0, 96000.0 }) {
		for (bool mono : { false, true }) {
			for (int mode = 0; mode < 8; ++mode) {
				for (float twist : { 0.0f, 0.06f, 0.43f, 0.4999f, 0.8f }) {
					for (float drive : { 0.0f, 1.0f }) {
						smooth::Settings s = settingsWith(mode, 0.6f, drive, twist);
						const Tail t = burstThenSilence(s, rate, mono);
						INFO("rate " << rate << (mono ? " mono" : " stereo") << " mode " << mode << " twist " << twist
						             << " drive " << drive << ": last sound " << t.lastSoundSeconds << " s, tail peak "
						             << t.tailPeak);
						if (t.tailPeak > 1.0e-3f) {
							// A DRONE — only where the firmware's own feedback sustains itself: ONLY WITH THE FEEDBACK UP.
							CHECK(twist > 0.375f);
							CHECK(twist < 0.5f);
							drones.push_back(std::to_string(mode));
							continue;
						}
						CHECK(t.lastSoundSeconds < kRestSilentSecondsPlusAFrame);
						CHECK(t.tailPeak == 0.0f);
						CHECK(t.dc == 0);
						CHECK(t.fb == 0);
						slowest = std::max(slowest, t.lastSoundSeconds);
					}
				}
			}
		}
	}
	MESSAGE("slowest tail to exact zero: " << slowest * 1000.0 << " ms; drones: " << drones.size());
	CHECK(slowest > 0.0);   // non-vacuity: tails exist and were timed
	CHECK(drones.size() < 40);
}

TEST_CASE("the rest never cuts a real signal, however quiet: 1e-6 in still sounds") {
	// -120 dB of the reference's 0 dBFS is 17 q31 steps: non-zero, so it restarts the rest's count every sample it
	// is non-zero. Held for two seconds through the default patch (3579, Drive centre), the wet it makes must
	// still be reaching the output at the end — and the reference agrees it would.
	for (bool mono : { false, true }) {
		smooth::SmoothShaper sh;
		sh.prepare(48000.0);
		sh.setTarget(smooth::Settings {});
		std::vector<float> l(96000), r(96000);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = r[i] = static_cast<float>(1.0e-6 * std::sin(0.013 * static_cast<double>(i)));
		}
		if (mono) {
			sh.processMono(l.data(), static_cast<int>(l.size()));
		}
		else {
			sh.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		}
		size_t lateNonZero = 0;
		for (size_t i = l.size() - 4800; i < l.size(); ++i) {
			lateNonZero += l[i] != 0.0f ? 1 : 0;
		}
		INFO((mono ? "mono" : "stereo"));
		CHECK(lateNonZero > 2400);
	}
}

TEST_CASE("the mono road is the stereo road's left, sample for sample, with the Width LFO live") {
	// **DECIDED 2026-09-25: the house contract, not the firmware's mono overload.** `test_fx_devices`
	// asks this of every device, but with a probe too short to reach the Width LFO's positive half — so it
	// passed the firmware's mono overload by luck. This probe is long enough by construction: Twist mid-Width,
	// two seconds, and the case CHECKS that position stereo engaged (the two stereo outputs differ with the same
	// input), so it cannot pass on a probe where the LFO never moved.
	for (int mode : { 0, 1, 2, 5 }) {
		for (float twist : { 0.06f, 0.6f }) {
			const smooth::Settings s = settingsWith(mode, 0.4f, 0.3f, twist);
			std::vector<float> in(96000);
			for (size_t i = 0; i < in.size(); ++i) {
				in[i] = static_cast<float>(0.9 * std::sin(0.021 * static_cast<double>(i)));
			}
			smooth::SmoothShaper st;
			st.prepare(48000.0);
			st.setTarget(s);
			std::vector<float> l = in;
			std::vector<float> r = in;
			for (size_t at = 0; at < in.size(); at += 500) {
				st.processStereo(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(500, in.size() - at)));
			}
			smooth::SmoothShaper mo;
			mo.prepare(48000.0);
			mo.setTarget(s);
			std::vector<float> m = in;
			for (size_t at = 0; at < in.size(); at += 333) {
				mo.processMono(m.data() + at, static_cast<int>(std::min<size_t>(333, in.size() - at)));
			}
			INFO("mode " << mode << " twist " << twist);
			CHECK(l != r); // NON-VACUITY: the stereo effect engaged somewhere in the probe
			CHECK(m == l);
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE PICTURE THE CARD AND THE PANEL DRAW — asserted as values.                                       //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("the ceiling sits where the zone saturates: x = 1 into the Chebyshevs, 1.0 in Poly, a reference in FM") {
	for (float d : { -1.0f, -0.5f, 0.0f, 0.5f, 1.0f }) {
		const float g = smooth::driveGainOf(smooth::firmwareControlsOf(settingsWith(0, 0.5f, d, 0.0f)).drive);
		const smooth::Picture cheby = smooth::pictureOf(settingsWith(0, 0.5f, d, 0.0f));
		const smooth::Picture warm = smooth::pictureOf(settingsWith(1, 0.5f, d, 0.0f));
		const smooth::Picture poly = smooth::pictureOf(settingsWith(7, 0.5f, d, 0.0f));
		const smooth::Picture fm = smooth::pictureOf(settingsWith(2, 0.5f, d, 0.0f));
		INFO("drive knob " << d << ", gain " << g);
		CHECK(cheby.ceilingInput == doctest::Approx(0.5 / g).epsilon(1e-6));
		CHECK(warm.ceilingInput == doctest::Approx(0.5 / g).epsilon(1e-6));
		CHECK(poly.ceilingInput == doctest::Approx(1.0 / g).epsilon(1e-6));
		CHECK(cheby.clips);
		CHECK(poly.clips);
		CHECK_FALSE(fm.clips);
		// AND IT IS WHERE THE CORE ACTUALLY STARTS TO SATURATE: just under the ceiling the zone's input is still
		// moving, just over it the probe reads the ceiling.
		smooth::ShapeProbe under;
		smooth::ShapeProbe over;
		const auto zoneInputAt = [&](const smooth::Picture& p, float input, smooth::ShapeProbe* probe) {
			(void)smooth::shapeSample(representable(static_cast<double>(input) * kZeroDbfs), p.driveGain, p.zone, p.weights,
			                          0.0f, 0.0f, 0.0f, probe);
		};
		zoneInputAt(cheby, cheby.ceilingInput * 0.99f, &under);
		zoneInputAt(cheby, cheby.ceilingInput * 1.01f, &over);
		CHECK(under.zoneInput < 0x7FFFFF00);
		CHECK(over.zoneInput == 0x7FFFFF00);
		zoneInputAt(poly, poly.ceilingInput * 0.99f, &under);
		zoneInputAt(poly, poly.ceilingInput * 1.01f, &over);
		CHECK(under.zoneInput < 0x7FFFFF80);
		CHECK(over.zoneInput == 0x7FFFFF80);
	}
	// DRAGGING THE CEILING WRITES THE DRIVE THAT PUTS IT THERE — the inverse, round trip.
	for (float c : { 0.13f, 0.25f, 0.4f, 0.5f, 0.9f, 3.0f }) {
		smooth::Settings s = settingsWith(0, 0.5f, 0.0f, 0.0f);
		s.drive = smooth::driveKnobForCeiling(0, c);
		CHECK(smooth::pictureOf(s).ceilingInput == doctest::Approx(c).epsilon(1e-4));
	}
	// Past the fader's travel it stops at the top (4x); a ceiling at infinity is the bottom.
	CHECK(smooth::driveKnobForCeiling(0, 0.01f) == 1.0f);
	CHECK(smooth::driveKnobForCeiling(0, std::numeric_limits<float>::infinity()) == 0.0f);
}

TEST_CASE("Drive at 0 is silence into the shaper and an off-scale ceiling; at 1 it is the firmware's 4x") {
	// AT 0: the firmware's law gives a gain of exactly zero, the ceiling is +infinity — off the plot, never a
	// division by zero — and the drag's inverse maps it back to 0.
	for (const int mode : { 0, 2, 7 }) {
		smooth::Settings s = settingsWith(mode, 0.5f, 0.0f, 0.0f);
		s.drive = 0.0f;
		const smooth::Picture p = smooth::pictureOf(s);
		CAPTURE(mode);
		CHECK(p.driveGain == 0.0f);
		CHECK(std::isinf(p.ceilingInput));
		CHECK(p.ceilingInput > 0.0f);
		CHECK(std::isinf(smooth::ceilingInputOf(mode, 0.0f)));
		CHECK(smooth::driveKnobForCeiling(mode, p.ceilingInput) == 0.0f);
		// NOTHING REACHES THE SHAPER: the core's output for a full voice is its output for silence.
		CHECK(smooth::transferAt(p, 1.0f) == smooth::transferAt(p, 0.0f));
		// AND THE DEVICE GLIDES TO IT: its drive gain settles at zero.
		smooth::SmoothShaper sh;
		sh.setTarget(s);
		std::vector<float> l(4800, 0.3f), r(4800, 0.3f);
		sh.processStereo(l.data(), r.data(), 4800);
		CHECK(sh.driveGainForTest() == 0.0f);
	}
	// AT 1: 4x, bit for bit the firmware's `computeDriveGain` of the saturated q31.
	smooth::Settings top = settingsWith(0, 0.5f, 0.0f, 0.0f);
	top.drive = 1.0f;
	CHECK(smooth::pictureOf(top).driveGain == 4.0f);
	CHECK(smooth::pictureOf(top).ceilingInput == 0.125f);
	CHECK(smooth::ceilingInputOf(7, 1.0f) == 0.25f);
}

TEST_CASE("the harmonic bars are a DFT of one cycle of a full voice through the transfer curve the card draws") {
	for (int mode = 0; mode < 8; ++mode) {
		for (float tw : { 0.0f, 0.2f, 0.3f }) {
			const smooth::Settings s = settingsWith(mode, 0.45f, 0.0f, tw);
			const auto bars = smooth::harmonicsOf(smooth::pictureOf(s), 1.0f);
			// THE TEST'S OWN DFT, over the drawn curve's samples.
			const smooth::Picture picture = smooth::pictureOf(s);
			constexpr int kN = 256;
			std::array<double, kN> y {};
			for (int n = 0; n < kN; ++n) {
				y[static_cast<size_t>(n)] = smooth::transferAt(picture, static_cast<float>(std::sin(2.0 * M_PI * n / kN)));
			}
			for (int k = 1; k <= 9; ++k) {
				std::complex<double> acc {};
				for (int n = 0; n < kN; ++n) {
					acc += y[static_cast<size_t>(n)] * std::polar(1.0, -2.0 * M_PI * k * n / kN);
				}
				const double want = 2.0 * std::abs(acc) / kN;
				INFO("mode " << mode << " twist " << tw << " H" << k);
				CHECK(std::fabs(static_cast<double>(bars[static_cast<size_t>(k - 1)]) - want) < 1.0e-5);
			}
		}
	}
	// NON-VACUITY: 3579 is ODD harmonics — H3..H9 present, every even absent (no evens without Twist). MEASURED at
	// the centre: H5 0.046, H9 0.095, the evens about 1e-7.
	const auto odd = smooth::harmonicsOf(smooth::pictureOf(settingsWith(0, 0.5f, 0.0f, 0.0f)), 1.0f);
	CHECK(odd[2] + odd[4] + odd[6] + odd[8] > 0.1f);
	for (size_t even : { 1u, 3u, 5u, 7u }) {
		CHECK(odd[even] < 1.0e-4f);
	}
	// And the Evens pill brings the evens in — at H = 0.5 its flavour is the negative half's dry blend.
	smooth::Settings withEvens = settingsWith(0, 0.5f, 0.0f, 0.0f);
	withEvens.pillOn[static_cast<size_t>(smooth::Pill::Evens)] = true;
	const auto evens = smooth::harmonicsOf(smooth::pictureOf(withEvens), 1.0f);
	CHECK(evens[1] > 1.0e-3f);
}

TEST_CASE("after the Width LFO's positive half the two sides come back together — the firmware's cache kept them apart") {
	// **THE FIX, AS A VALUE.** The firmware reuses its last targets when position
	// stereo turns off and its key has not moved, and the key omits the LFO's offset — so the left and right
	// targets it smooths toward stay apart after a positive half-cycle. The port computes them afresh: with the
	// LFO no longer positive the offset is zero and the two sides converge on the SAME coefficients.
	//
	// Twist mid-Width gives the full width; the LFO starts at phase 0 and is positive for its first eighth.
	// 44.1 kHz, 128-frame buffers, run long enough for the LFO to leave its positive half and settle.
	const smooth::Settings s = settingsWith(0, 0.4f, 0.0f, 0.06f);
	smooth::SmoothShaper port;
	port.prepare(44100.0);
	port.setTarget(s);
	std::vector<int32_t> ql(128), qr(128);
	std::vector<float> l(128), r(128);
	bool apartDuringPositive = false;
	for (int block = 0; block < 44100 * 8 / 128; ++block) {
		for (int i = 0; i < 128; ++i) {
			const double t = static_cast<double>(block * 128 + i);
			ql[i] = qr[i] = representable(0.5 * std::sin(0.03 * t) * kZeroDbfs);
			l[i] = r[i] = static_cast<float>(ql[i]) / 16777216.0f;
		}
		port.processStereo(l.data(), r.data(), 128);
		const auto c = port.coefficientsForTest();
		apartDuringPositive = apartDuringPositive || c[1] != c[5];
	}
	// NON-VACUITY: the stereo offset did engage — the sides were apart at some point.
	CHECK(apartDuringPositive);
	// The LFO has left its positive half by now: at duty 0.5 the triangle is positive over the first quarter of its
	// cycle, 5 s of every 20 at 0.05 Hz; eight seconds is past it.
	REQUIRE(smooth::stereoLfoValueOf(port.lfoPhaseForTest()) <= 0.0f);
	const auto ours = port.coefficientsForTest();
	// OURS: back together, left and right the same coefficients.
	for (size_t k = 1; k <= 4; ++k) {
		CHECK(ours[k] == doctest::Approx(ours[k + 4]).epsilon(1e-5));
	}
}

//--------------------------------------------------------------------------------------------------//
// THE STACK TRIM (law (b)).                                                                        //
//--------------------------------------------------------------------------------------------------//

TEST_CASE("the stack trim is 1 for one voice, 1/sqrt(N) at 0 cents, and toward 1 as the parts decorrelate") {
	CHECK(smooth::stackTrimOf(1, 0.0f, 60.0f) == 1.0f);
	CHECK(smooth::stackTrimOf(1, 50.0f, 36.0f) == 1.0f);
	for (int n : { 2, 4, 8 }) {
		CHECK(smooth::stackTrimOf(n, 0.0f, 60.0f) == doctest::Approx(1.0 / std::sqrt(n)).epsilon(1e-6));
		// Monotone toward 1 with detune, and with pitch (a higher note beats faster at the same cents).
		CHECK(smooth::stackTrimOf(n, 1.0f, 60.0f) > smooth::stackTrimOf(n, 0.5f, 60.0f));
		CHECK(smooth::stackTrimOf(n, 1.0f, 84.0f) > smooth::stackTrimOf(n, 1.0f, 36.0f));
		CHECK(smooth::stackTrimOf(n, 50.0f, 60.0f) == doctest::Approx(1.0).epsilon(1e-6));
	}
}

TEST_CASE("the stack trim's fitted time constant holds the measured stacks within 1.5 dB of one voice") {
	// **THE PIN ON τ** (`kStackCoherenceSeconds`, 8.1 s, fitted to V5 at a one-second horizon). MEASURED on the
	// Synth, 2026-09-26, by the probe the design doc records: the RMS entering the per-voice devices, in dB relative to
	// a stack of one, one second after note-on. The trim applied to each must land within ±1.5 dB of 0 — the fit's
	// residual was 0.56 dB. A change to τ, or to how the synth sums its parts, that breaks the fit fails here.
	struct Measured {
		int n;
		float cents;
		int key;
		double rmsDb;
	};
	constexpr Measured kTable[] = {
		{ 2, 0.0f, 36, 3.0 },  { 2, 0.0f, 60, 3.0 },  { 2, 0.0f, 84, 3.0 },  { 2, 0.5f, 60, 0.8 },
		{ 2, 1.0f, 60, 0.2 },  { 2, 2.0f, 60, -0.2 }, { 2, 5.0f, 60, -0.1 }, { 2, 10.0f, 36, -0.4 },
		{ 2, 10.0f, 60, 0.0 }, { 2, 10.0f, 84, 0.0 }, { 2, 50.0f, 60, 0.0 }, { 4, 0.0f, 36, 6.0 },
		{ 4, 0.0f, 60, 6.0 },  { 4, 0.0f, 84, 6.0 },  { 4, 0.5f, 60, 3.4 },  { 4, 1.0f, 60, 2.1 },
		{ 4, 2.0f, 60, 0.6 },  { 4, 5.0f, 60, -0.4 }, { 4, 10.0f, 36, 0.2 }, { 4, 10.0f, 60, -0.2 },
		{ 4, 10.0f, 84, 0.0 }, { 4, 50.0f, 60, 0.0 }, { 8, 0.0f, 36, 9.0 },  { 8, 0.0f, 60, 9.0 },
		{ 8, 0.0f, 84, 9.0 },  { 8, 0.5f, 60, 6.6 },  { 8, 1.0f, 60, 5.0 },  { 8, 2.0f, 60, 2.9 },
		{ 8, 5.0f, 60, -0.1 }, { 8, 10.0f, 36, 2.0 }, { 8, 10.0f, 60, -1.3 }, { 8, 10.0f, 84, -0.5 },
		{ 8, 50.0f, 60, -0.1 },
	};
	CHECK(smooth::kStackCoherenceSeconds == 8.1);
	double worst = 0.0;
	for (const Measured& m : kTable) {
		const double trimDb = 20.0 * std::log10(smooth::stackTrimOf(m.n, m.cents, static_cast<float>(m.key)));
		const double after = m.rmsDb + trimDb;
		INFO("N " << m.n << " " << m.cents << " c key " << m.key << ": " << m.rmsDb << " dB raw, " << after << " dB trimmed");
		CHECK(std::fabs(after) <= 1.5);
		worst = std::max(worst, std::fabs(after));
	}
	MESSAGE("worst trimmed residual " << worst << " dB over " << std::size(kTable) << " measured cells");
}

TEST_CASE("the stack trim's coherence is 1 for a plain stack, to the bit, and scales rho down to nothing at 0") {
	// ρ = ρ0 · exp(−Δf · τ). A plain stack reports ρ0 = 1, and the trim must then be law (b)'s to the
	// bit, which is what keeps the τ pin above and every plain-stack comparison untouched.
	for (int n : { 2, 4, 8 }) {
		for (float cents : { 0.0f, 0.5f, 1.0f, 10.0f, 50.0f }) {
			for (float key : { 36.0f, 60.0f, 84.0f }) {
				CHECK(smooth::stackTrimOf(n, cents, key, 1.0f) == smooth::stackTrimOf(n, cents, key));
			}
		}
		// ρ0 = 0: the parts are already decorrelated at 0 cents — no trim at all, exactly.
		CHECK(smooth::stackTrimOf(n, 0.0f, 60.0f, 0.0f) == 1.0f);
		// ρ0 between: 1/√(1 + (N−1)·ρ0) at 0 cents, restated here.
		CHECK(smooth::stackTrimOf(n, 0.0f, 60.0f, 0.3f) == doctest::Approx(1.0 / std::sqrt(1.0 + (n - 1) * 0.3)).epsilon(1e-6));
	}
	// A NEGATIVE ρ0 (two stands in near-opposite phase measured −0.24) sums QUIETER than decorrelated, so the trim lifts it.
	CHECK(smooth::stackTrimOf(2, 0.0f, 60.0f, -0.24f) == doctest::Approx(1.0 / std::sqrt(0.76)).epsilon(1e-6));
	// And the floor: a sum's power cannot be negative, so an impossible ρ0 is held to +6 dB rather than dividing by zero.
	CHECK(smooth::stackTrimOf(2, 0.0f, 60.0f, -2.0f) == doctest::Approx(2.0).epsilon(1e-6));
	// A stack of one is 1 whatever the coherence says.
	CHECK(smooth::stackTrimOf(1, 0.0f, 60.0f, 0.2f) == 1.0f);
}

TEST_CASE("the stack trim with the parts' coherence holds stands and width at the stands' own level") {
	// **THE PIN ON ρ0**. MEASURED on the Synth, V5 defaults, 1 s window, the RMS entering the
	// per-voice devices in dB relative to the parts' decorrelated level — a stack of one at each part's stand,
	// power-averaged, for a Stack Index route into Pos X (`stand.1/.25/.5`: route amount); the 50-cent cell for
	// Random Fresh stands (route 0.5) and for Width (terrain stereo, a mono stand per part). `rho0` is what the synth
	// computed for that voice (`Synth::updateStackCoherence`). The stands' own loudness is NOT corrected — it is the
	// sound of the stand — so the target is the parts' own level, not a stack of one on the puck.
	//
	// TOLERANCES (approved 2026-09-26): ±1.0 dB at 0 cents, where ρ0 is the whole story (worst 0.51 dB). ±2.0 dB when
	// detuned: the detune half of the law, exp(−Δf · τ), was fitted on PLAIN stacks, and between parts whose cycles
	// differ the cross terms depend on their relative phase, which a slow beat sweeps only partly inside one second.
	// The misses are mostly UNDER-trim — the louder side (worst +1.94 dB; worst over-trim −1.98 dB).
	struct Measured {
		const char* setup;
		int n;
		float cents;
		int key;
		double rmsDbVsParts;
		float rho0;
	};
	constexpr Measured kTable[] = {
		{ "stand.1", 2, 0.0f, 36, 1.98, 0.5496f },
		{ "stand.1", 4, 0.0f, 36, 4.33, 0.5504f },
		{ "stand.1", 8, 0.0f, 36, 7.33, 0.6069f },
		{ "stand.1", 8, 1.0f, 36, 6.86, 0.6069f },
		{ "stand.1", 8, 10.0f, 36, 1.74, 0.6069f },
		{ "stand.1", 2, 0.0f, 60, 1.99, 0.5496f },
		{ "stand.1", 2, 1.0f, 60, -0.49, 0.5496f },
		{ "stand.1", 2, 10.0f, 60, 0.01, 0.5496f },
		{ "stand.1", 4, 0.0f, 60, 4.34, 0.5504f },
		{ "stand.1", 4, 1.0f, 60, 1.93, 0.5504f },
		{ "stand.1", 4, 10.0f, 60, -0.31, 0.5504f },
		{ "stand.1", 8, 0.0f, 60, 7.35, 0.6069f },
		{ "stand.1", 8, 1.0f, 60, 4.89, 0.6069f },
		{ "stand.1", 8, 10.0f, 60, -1.98, 0.6069f },
		{ "stand.1", 2, 0.0f, 84, 2.42, 0.6219f },
		{ "stand.1", 4, 0.0f, 84, 4.82, 0.5997f },
		{ "stand.1", 8, 0.0f, 84, 7.93, 0.6619f },
		{ "stand.1", 8, 1.0f, 84, -0.04, 0.6619f },
		{ "stand.1", 8, 10.0f, 84, -0.42, 0.6619f },
		{ "stand.25", 2, 0.0f, 36, 0.34, 0.0801f },
		{ "stand.25", 4, 0.0f, 36, 1.67, 0.1485f },
		{ "stand.25", 8, 0.0f, 36, 4.61, 0.2596f },
		{ "stand.25", 8, 1.0f, 36, 4.37, 0.2596f },
		{ "stand.25", 8, 10.0f, 36, 1.28, 0.2596f },
		{ "stand.25", 2, 0.0f, 60, 0.34, 0.0801f },
		{ "stand.25", 2, 1.0f, 60, -0.26, 0.0801f },
		{ "stand.25", 2, 10.0f, 60, -0.04, 0.0801f },
		{ "stand.25", 4, 0.0f, 60, 1.68, 0.1485f },
		{ "stand.25", 4, 1.0f, 60, 0.52, 0.1485f },
		{ "stand.25", 4, 10.0f, 60, -0.10, 0.1485f },
		{ "stand.25", 8, 0.0f, 60, 4.63, 0.2596f },
		{ "stand.25", 8, 1.0f, 60, 3.25, 0.2596f },
		{ "stand.25", 8, 10.0f, 60, -1.38, 0.2596f },
		{ "stand.25", 2, 0.0f, 84, 0.28, 0.0811f },
		{ "stand.25", 4, 0.0f, 84, 2.02, 0.1673f },
		{ "stand.25", 8, 0.0f, 84, 5.14, 0.2846f },
		{ "stand.25", 8, 1.0f, 84, 0.03, 0.2846f },
		{ "stand.25", 8, 10.0f, 84, -0.29, 0.2846f },
		{ "stand.5", 2, 0.0f, 36, -1.18, -0.2298f },
		{ "stand.5", 4, 0.0f, 36, 0.86, 0.0801f },
		{ "stand.5", 8, 0.0f, 36, 1.13, 0.0409f },
		{ "stand.5", 8, 1.0f, 36, 1.26, 0.0409f },
		{ "stand.5", 8, 10.0f, 36, 1.77, 0.0409f },
		{ "stand.5", 2, 0.0f, 60, -1.18, -0.2298f },
		{ "stand.5", 2, 1.0f, 60, -0.70, -0.2298f },
		{ "stand.5", 2, 10.0f, 60, 0.12, -0.2298f },
		{ "stand.5", 4, 0.0f, 60, 0.85, 0.0801f },
		{ "stand.5", 4, 1.0f, 60, 0.52, 0.0801f },
		{ "stand.5", 4, 10.0f, 60, 0.04, 0.0801f },
		{ "stand.5", 8, 0.0f, 60, 1.14, 0.0409f },
		{ "stand.5", 8, 1.0f, 60, 1.95, 0.0409f },
		{ "stand.5", 8, 10.0f, 60, -0.15, 0.0409f },
		{ "stand.5", 2, 0.0f, 84, -1.27, -0.2469f },
		{ "stand.5", 4, 0.0f, 84, 0.67, 0.0642f },
		{ "stand.5", 8, 0.0f, 84, 1.32, 0.0450f },
		{ "stand.5", 8, 1.0f, 84, 1.61, 0.0450f },
		{ "stand.5", 8, 10.0f, 84, -0.41, 0.0450f },
		{ "fresh.5", 2, 0.0f, 36, 0.42, 0.0667f },
		{ "fresh.5", 4, 0.0f, 36, 2.47, 0.2500f },
		{ "fresh.5", 8, 0.0f, 36, 2.64, 0.1315f },
		{ "fresh.5", 8, 1.0f, 36, 2.81, 0.1315f },
		{ "fresh.5", 8, 10.0f, 36, 2.10, 0.1315f },
		{ "fresh.5", 2, 0.0f, 60, 0.34, 0.0667f },
		{ "fresh.5", 2, 1.0f, 60, 0.93, 0.0667f },
		{ "fresh.5", 2, 10.0f, 60, -0.07, 0.0667f },
		{ "fresh.5", 4, 0.0f, 60, 2.56, 0.2500f },
		{ "fresh.5", 4, 1.0f, 60, 2.64, 0.2500f },
		{ "fresh.5", 4, 10.0f, 60, -0.24, 0.2500f },
		{ "fresh.5", 8, 0.0f, 60, 2.89, 0.1315f },
		{ "fresh.5", 8, 1.0f, 60, 3.00, 0.1315f },
		{ "fresh.5", 8, 10.0f, 60, 0.18, 0.1315f },
		{ "fresh.5", 2, 0.0f, 84, 0.98, 0.1138f },
		{ "fresh.5", 4, 0.0f, 84, 2.96, 0.2782f },
		{ "fresh.5", 8, 0.0f, 84, 3.03, 0.1398f },
		{ "fresh.5", 8, 1.0f, 84, 1.94, 0.1398f },
		{ "fresh.5", 8, 10.0f, 84, -0.20, 0.1398f },
		{ "width", 2, 0.0f, 36, 0.12, 0.0366f },
		{ "width", 4, 0.0f, 36, 2.73, 0.2583f },
		{ "width", 8, 0.0f, 36, 5.33, 0.2958f },
		{ "width", 8, 1.0f, 36, 4.94, 0.2958f },
		{ "width", 8, 10.0f, 36, 0.86, 0.2958f },
		{ "width", 2, 0.0f, 60, 0.15, 0.0366f },
		{ "width", 2, 1.0f, 60, -0.25, 0.0366f },
		{ "width", 2, 10.0f, 60, 0.01, 0.0366f },
		{ "width", 4, 0.0f, 60, 2.57, 0.2583f },
		{ "width", 4, 1.0f, 60, 0.71, 0.2583f },
		{ "width", 4, 10.0f, 60, -0.19, 0.2583f },
		{ "width", 8, 0.0f, 60, 5.00, 0.2958f },
		{ "width", 8, 1.0f, 60, 2.89, 0.2958f },
		{ "width", 8, 10.0f, 60, -1.12, 0.2958f },
		{ "width", 2, 0.0f, 84, 0.11, 0.0384f },
		{ "width", 4, 0.0f, 84, 3.05, 0.2942f },
		{ "width", 8, 0.0f, 84, 5.60, 0.3316f },
		{ "width", 8, 1.0f, 84, -0.17, 0.3316f },
		{ "width", 8, 10.0f, 84, -0.31, 0.3316f },
	};
	double worst0 = 0.0;
	double worstDetuned = 0.0;
	int coherent = 0;
	for (const Measured& m : kTable) {
		const double trimDb = 20.0 * std::log10(smooth::stackTrimOf(m.n, m.cents, static_cast<float>(m.key), m.rho0));
		const double after = m.rmsDbVsParts + trimDb;
		INFO(m.setup << " N " << m.n << " " << m.cents << " c key " << m.key << ": " << m.rmsDbVsParts << " dB raw, " << after
		              << " dB trimmed");
		if (m.cents == 0.0f) {
			CHECK(std::fabs(after) <= 1.0);
			worst0 = std::max(worst0, std::fabs(after));
		}
		else {
			CHECK(std::fabs(after) <= 2.0);
			worstDetuned = std::max(worstDetuned, std::fabs(after));
		}
		// NOT VACUOUS: law (b) — ρ0 taken as 1 — misses these by more; at 0 cents by at least 1 dB in all but the
		// most coherent stands.
		const double plainDb = 20.0 * std::log10(smooth::stackTrimOf(m.n, m.cents, static_cast<float>(m.key)));
		if (m.cents == 0.0f && std::fabs(m.rmsDbVsParts + plainDb) > 1.0) {
			++coherent;
		}
	}
	CHECK(coherent >= 40); // 44 of the 45, measured
	MESSAGE("worst trimmed residual " << worst0 << " dB at 0 cents, " << worstDetuned << " dB detuned, over "
	                                   << std::size(kTable) << " measured cells; law (b) missed " << coherent
	                                   << " of the 0-cent cells by more than 1 dB");
}

TEST_CASE("the port's drive target carries the stack trim per voice, exactly 1 at one voice and on the bus") {
	const auto trimAfter = [](bool poly, int stack, float cents, float pitch, float coherence = 1.0f) {
		smooth::SmoothShaper sh;
		sh.prepare(48000.0);
		smooth::Settings s;
		s.poly = poly;
		sh.setTarget(s);
		feathers::device::NoteContext note;
		note.pitchSemitones = pitch;
		note.stackCount = stack;
		note.stackDetuneCents = cents;
		note.stackCoherence = coherence;
		sh.setNoteContext(note);
		std::vector<float> l(4800, 0.3f), r(4800, 0.3f);
		sh.processStereo(l.data(), r.data(), 4800);
		return std::pair { sh.stackTrimForTest(), sh.driveGainForTest() };
	};
	const auto [t8, g8] = trimAfter(true, 8, 0.0f, 60.0f);
	CHECK(t8 == doctest::Approx(1.0 / std::sqrt(8.0)).epsilon(1e-6));
	CHECK(g8 == doctest::Approx(t8).epsilon(1e-6)); // drive centre is unity, so the gain IS the trim
	CHECK(trimAfter(true, 1, 0.0f, 60.0f).first == 1.0f);
	CHECK(trimAfter(false, 8, 0.0f, 60.0f).first == 1.0f); // the bus: never
	// AND THE PARTS' COHERENCE RIDES THE SAME ROAD: a voice whose parts stand apart (ρ0 0.3) is trimmed
	// 1/√(1 + 7 · 0.3), not 1/√8 — and on the bus, still never.
	CHECK(trimAfter(true, 8, 0.0f, 60.0f, 0.3f).first == doctest::Approx(1.0 / std::sqrt(1.0 + 7.0 * 0.3)).epsilon(1e-6));
	CHECK(trimAfter(false, 8, 0.0f, 60.0f, 0.3f).first == 1.0f);
	CHECK(trimAfter(false, 8, 0.0f, 60.0f, -0.2f).first == 1.0f);
	// A STACK OF ONE IS BIT-IDENTICAL TO NO NOTE AT ALL.
	smooth::SmoothShaper a;
	smooth::SmoothShaper b;
	a.prepare(48000.0);
	b.prepare(48000.0);
	feathers::device::NoteContext one;
	one.stackCount = 1;
	one.stackDetuneCents = 25.0f;
	one.pitchSemitones = 40.0f;
	b.setNoteContext(one);
	std::vector<float> la(2000), lb(2000), ra(2000), rb(2000);
	for (size_t i = 0; i < la.size(); ++i) {
		la[i] = lb[i] = ra[i] = rb[i] = static_cast<float>(0.9 * std::sin(0.02 * static_cast<double>(i)));
	}
	a.processStereo(la.data(), ra.data(), 2000);
	b.processStereo(lb.data(), rb.data(), 2000);
	CHECK(la == lb);
	CHECK(ra == rb);
}

TEST_CASE("NM: on a playing transport the bus instance's stereo LFO is the song's — one started mid-song lands where "
          "one that played through is; a voice instance stays note-anchored") {
	constexpr double kRate = 48000.0;
	const smooth::Settings s = settingsWith(0, 0.5f, 0.0f, 0.05f); // Width's zone: the stereo LFO is live
	std::vector<float> l(128, 0.0f), r = l;
	const auto block = [&](smooth::SmoothShaper& sh, double at, bool playing) {
		feathers::device::TransportClock clock {};
		clock.secondsPerBeat = 0.5;
		clock.playing = playing;
		clock.songPositionSeconds = at;
		sh.setTransportClock(clock);
		sh.processStereo(l.data(), r.data(), 128);
	};
	const double t = 91.0;
	// THROUGH THE SONG FROM 85 s, and STARTED AT 91 s: after the block at 91 s, one phase.
	smooth::SmoothShaper through;
	through.prepare(kRate);
	through.setTarget(s);
	double at = 85.0;
	for (; at < t - 1e-9; at += 128.0 / kRate) {
		block(through, at, true);
	}
	block(through, at, true);
	smooth::SmoothShaper started;
	started.prepare(kRate);
	started.setTarget(s);
	block(started, at, true);
	CHECK(std::abs(started.lfoPhaseNowForTest() - through.lfoPhaseNowForTest()) < 1e-4f);
	// NOT VACUOUS: the phase has moved off its start, and a stopped instance at the same place free-runs from 0.
	smooth::SmoothShaper stopped;
	stopped.prepare(kRate);
	stopped.setTarget(s);
	block(stopped, at, false);
	CHECK(std::abs(stopped.lfoPhaseNowForTest() - through.lfoPhaseNowForTest()) > 1e-3f);
	// A VOICE IS NOTE-ANCHORED: on the same playing transport it starts where its Twist puts it, not on the song.
	smooth::SmoothShaper voice;
	voice.setVoice();
	voice.prepare(kRate);
	voice.setTarget(s);
	block(voice, at, true);
	CHECK(std::abs(voice.lfoPhaseNowForTest() - through.lfoPhaseNowForTest()) > 1e-3f);
}
