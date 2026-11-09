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

#include "mod/arp/humanize.h"

#include "fx/sklitter/groove.h"
#include "mod/arp/arp_core.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::arp::humanize {

namespace {

/// A UNIFORM DRAW in [−1, 1] for (seed, slot, lane): the classic arp's dice, so the two arps hash alike.
double draw(int seed, int64_t g, int lane) noexcept {
	const uint64_t h = stepDice(seed + 7919 * lane, g);
	return static_cast<double>(h >> 11) / 9007199254740992.0 * 2.0 - 1.0;
}

double frac(double x) noexcept {
	return x - std::floor(x);
}

/// A SMOOTH WALK in [−1, 1] over the slots: two phi triangles at slow strides, phased by the Seed.
double drift(int seed, int64_t g, int lane) noexcept {
	const auto tri = [](double p) {
		const double f = frac(p);
		return f < 0.5 ? 4.0 * f - 1.0 : 3.0 - 4.0 * f;
	};
	const double s = static_cast<double>(seed) * 0.6180339887498949 + 0.37 * lane;
	return 0.5 * (tri(static_cast<double>(g) * 0.0688 + s) + tri(static_cast<double>(g) * 0.0425 + 1.7 * s));
}

double msToSteps(double ms, double stepSeconds) noexcept {
	return stepSeconds > 0.0 ? ms * 0.001 / stepSeconds : 0.0;
}

/// SKLITTER'S FEEL FOR A MODE OF OURS: the first ten ARE its modes, in its order.
fx::sklitter::GrooveFeel grooveOf(const Feel& f) noexcept {
	static_assert(kGrooveFeels == static_cast<size_t>(fx::sklitter::kGrooveModeCount), "the groove feels are SKLITTER's");
	const auto mode = isGroove(f.mode) ? static_cast<fx::sklitter::GrooveMode>(f.mode) : fx::sklitter::GrooveMode::Straight;
	return { mode, fx::sklitter::isFree(mode) ? f.freePhase : f.amount }; // Free walks to a position, the rest take a depth
}

} // namespace

double shiftSteps(const Feel& feel, int64_t g, double stepSeconds, double stepBeats) noexcept {
	const double a = std::clamp(feel.amount, 0.0, 1.0);
	double shift = 0.0;
	switch (feel.mode) {
	case Mode::Straight:
	case Mode::StrumUp:
	case Mode::StrumDown: break;
	case Mode::Swing4:
	case Mode::Swing8:
	case Mode::Swing16:
	case Mode::Groove4:
	case Mode::Groove8:
	case Mode::Groove16:
	case Mode::Free4:
	case Mode::Free8:
	case Mode::Free16: {
		// THE BAR'S WARP (SKLITTER's): this slot's straight phase in its bar in, its swung phase out.
		const double stepsPerBar = stepBeats > 0.0 ? 4.0 / stepBeats : 16.0;
		const double atBar = static_cast<double>(g) / stepsPerBar;
		const double phase = frac(atBar);
		const double warped = fx::sklitter::grooveWarp(grooveOf(feel), phase);
		shift = (warped - phase) * stepsPerBar;
		break;
	}
	case Mode::Tight: shift = msToSteps(draw(feel.seed, g, 0) * a * kTightMs, stepSeconds); break;
	case Mode::Loose: shift = msToSteps(draw(feel.seed, g, 0) * a * kLooseMs, stepSeconds); break;
	case Mode::Drunk: shift = msToSteps(drift(feel.seed, g, 0) * a * kDrunkMs, stepSeconds); break;
	case Mode::Push:
		shift = msToSteps(-a * kPushMs + draw(feel.seed, g, 0) * a * kPushJitterMs, stepSeconds);
		break;
	case Mode::Lazy:
		shift = msToSteps(a * kPushMs + draw(feel.seed, g, 0) * a * kPushJitterMs, stepSeconds);
		break;
	case Mode::Flam: shift = -flamSteps(feel, stepSeconds); break;
	case Mode::kCount: break;
	}
	// WOBBLE (zone 2, every feel): skipped outright at 0, so a patch that never moves it is bit-identical to before.
	if (feel.wobble != 0.0 && std::isfinite(feel.wobble)) {
		const int32_t grid = wobble::gridOf(stepBeats);
		shift += wobble::shiftSteps(feel.wobble, wobble::stepInBar(g, grid), grid);
	}
	return std::clamp(std::isfinite(shift) ? shift : 0.0, -kMaxShiftSteps, kMaxShiftSteps);
}

double velocityScale(const Feel& feel, int64_t g, double stepBeats) noexcept {
	const double a = std::clamp(feel.amount, 0.0, 1.0);
	double scale = 1.0;
	switch (feel.mode) {
	case Mode::Tight: scale = 1.0 + draw(feel.seed, g, 1) * a * kTightVelocity; break;
	case Mode::Loose: scale = 1.0 + draw(feel.seed, g, 1) * a * kLooseVelocity; break;
	case Mode::Drunk: scale = 1.0 + drift(feel.seed, g, 1) * a * kDrunkVelocity; break;
	default: break;
	}
	if (feel.wobble != 0.0 && std::isfinite(feel.wobble)) {
		scale *= wobble::velocity(feel.wobble, wobble::stepInBar(g, wobble::gridOf(stepBeats)));
	}
	return scale;
}

double lengthScale(const Feel& feel, int64_t g, double stepBeats) noexcept {
	if (feel.wobble == 0.0 || !std::isfinite(feel.wobble)) {
		return 1.0;
	}
	return wobble::length(feel.wobble, wobble::stepInBar(g, wobble::gridOf(stepBeats)));
}

bool wobbleFires(const Feel& feel, wobble::Field f, int64_t g, double stepBeats) noexcept {
	if (feel.wobble == 0.0 || !std::isfinite(feel.wobble)) {
		return false;
	}
	return wobble::fires(f, wobble::stepInBar(g, wobble::gridOf(stepBeats)), feel.wobble);
}

double wobbleSign(wobble::Field f, int64_t g, double stepBeats) noexcept {
	return wobble::sign(f, wobble::stepInBar(g, wobble::gridOf(stepBeats)));
}

//------------------------------------------------------------------------------------------------//
// Wobble — the second groove zone (design §12-ter)                                               //
//------------------------------------------------------------------------------------------------//

namespace wobble {

double wave(Field f, int32_t j, double phase) noexcept {
	// A STEP'S DUTY BY ITS METRICAL LEVEL, as zone 1's Free: the weakest steps move all the time, stronger ones in rarer
	// rises (`fx::sklitter::freeDuty`); its rate is the field's own, φ^(1/(j + c_f)), scaled by the duty so every rise
	// lasts the same stretch of phase.
	const double duty = j > 0 ? fx::sklitter::freeDuty(j) : 1.0;
	const double rate = std::pow(fx::sklitter::kGoldenRatio, 1.0 / (static_cast<double>(j) + kFieldOffset[static_cast<size_t>(f)]));
	const double p = frac(std::fabs(phase) * rate * duty);
	if (p < 1.0 - duty) {
		return 0.0;
	}
	const double q = (p - (1.0 - duty)) / duty;
	return 1.0 - std::fabs(2.0 * q - 1.0);
}

double sign(Field f, int32_t j) noexcept {
	return frac((static_cast<double>(j) + kFieldOffset[static_cast<size_t>(f)]) * fx::sklitter::kGoldenRatio) < 0.5 ? -1.0 : 1.0;
}

double run(int32_t j, int32_t grid, double phase) noexcept {
	const double reach = std::min(1.0, kRunRamp * std::fabs(phase));
	const double phrase = frac(std::fabs(phase) / fx::sklitter::kGoldenRatio);
	constexpr double kTau = 6.283185307179586;
	return reach * std::sin(kTau * (static_cast<double>(j) / static_cast<double>(std::max(1, grid)) + phrase));
}

double shiftSteps(double phase, int32_t j, int32_t grid) noexcept {
	const double own = kPushDrag * sign(Field::Timing, j) * wave(Field::Timing, j, phase);
	return std::clamp(own + kRun * run(j, grid, phase), -kMaxShift, kMaxShift);
}

double velocity(double phase, int32_t j) noexcept {
	const double w = wave(Field::Velocity, j, phase);
	if (sign(Field::Velocity, j) > 0.0 || j == 0) {
		return 1.0 + kRing * w; // RINGS (the downbeat only ever rings: it never dips)
	}
	const double weight = (j % 2 == 1) ? 1.0 : 0.5; // a weak step ghosts deeper
	return 1.0 - kDip * weight * w;
}

double length(double phase, int32_t j) noexcept {
	return 1.0 - kClip * wave(Field::Length, j, phase);
}

double extraWave(Field f, int32_t j, double phase) noexcept {
	const double duty = kExtraDuty[static_cast<size_t>(f)];
	if (!(duty > 0.0)) {
		return 0.0;
	}
	const double rate = std::pow(fx::sklitter::kGoldenRatio, 1.0 / (static_cast<double>(j) + kFieldOffset[static_cast<size_t>(f)]));
	const double p = frac(std::fabs(phase) * rate * duty);
	if (p < 1.0 - duty) {
		return 0.0;
	}
	const double q = (p - (1.0 - duty)) / duty;
	return 1.0 - std::fabs(2.0 * q - 1.0);
}

bool fires(Field f, int32_t j, double phase) noexcept {
	return extraWave(f, j, phase) > 0.5;
}

int32_t gridOf(double stepBeats) noexcept {
	return stepBeats > 0.0 && std::isfinite(stepBeats) ? std::max<int32_t>(1, static_cast<int32_t>(std::lround(4.0 / stepBeats))) : 16;
}

int32_t stepInBar(int64_t g, int32_t grid) noexcept {
	const auto n = static_cast<int64_t>(std::max(1, grid));
	return static_cast<int32_t>(((g % n) + n) % n);
}

} // namespace wobble

double strumShare(const Feel& feel, int j, int n) noexcept {
	if ((feel.mode != Mode::StrumUp && feel.mode != Mode::StrumDown) || n < 2) {
		return 0.0;
	}
	return std::clamp(feel.amount, 0.0, 1.0) * kStrumMaxShare * static_cast<double>(j) / static_cast<double>(n - 1);
}

double flamSteps(const Feel& feel, double stepSeconds) noexcept {
	if (feel.mode != Mode::Flam) {
		return 0.0;
	}
	return std::min(kMaxShiftSteps, msToSteps(std::clamp(feel.amount, 0.0, 1.0) * kFlamMs, stepSeconds));
}

} // namespace feathers::mod::arp::humanize
