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

#include "fx/smooth/smooth_types.h"
#include "character/fixedpoint.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace feathers::fx::smooth {

namespace {

const device::ParamDesc* descOf(Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= std::size(kParamDescs)) {
		return nullptr;
	}
	return &kParamDescs[index];
}

float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	const float clamped = device::coerceControl(desc, value);
	return desc.stepped ? static_cast<float>(std::lround(clamped)) : clamped;
}

//--------------------------------------------------------------------------------------------------//
// TWO'S-COMPLEMENT ARITHMETIC, SPELLED OUT. The firmware writes plain `+`, `-`, `*` on int32 and the  //
// ARM wraps; in C++ an overflow is undefined, so every sum that can leave the range goes through     //
// `uint32_t` here. Shifts are left as shifts: C++20 defines `<<` on a negative int as modular.       //
//--------------------------------------------------------------------------------------------------//

int32_t wadd(int32_t a, int32_t b) noexcept {
	return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
int32_t wsub(int32_t a, int32_t b) noexcept {
	return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
int32_t wmul(int32_t a, int32_t k) noexcept {
	return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(k));
}
int32_t wneg(int32_t a) noexcept {
	return static_cast<int32_t>(0u - static_cast<uint32_t>(a));
}
int32_t wshl(int32_t a, int n) noexcept {
	return static_cast<int32_t>(static_cast<uint32_t>(a) << n);
}

/// The high word of a * b, and the same rounded to nearest: `character/fixedpoint.h`'s operations.
int32_t mulhi(int32_t a, int32_t b) noexcept {
	return character::multiply_32x32_rshift32(a, b);
}
int32_t mulhiRounded(int32_t a, int32_t b) noexcept {
	return character::multiply_accumulate_32x32_rshift32_rounded(0, a, b);
}

/// a + b, saturating at the ends of int32.
int32_t addSaturate(int32_t a, int32_t b) noexcept {
	return character::add_saturate(a, b);
}

/// `kMinus3dB = static_cast<q31_t>(0.707 * ONE_Q31)` — Zone 7's trim.
constexpr int32_t kMinus3dBQ31 = static_cast<int32_t>(0.707 * kOneQ31);
/// `static_cast<float>(ONE_Q31)`, which is 2^31: every float↔q31 scale in the firmware is this.
constexpr float kOneQ31F = static_cast<float>(kOneQ31);

//--------------------------------------------------------------------------------------------------//
// THE FIRMWARE'S TRIANGLES AND PHI BANK — `dsp/util.hpp` and `dsp/phi_triangle.hpp`, transcribed.    //
//--------------------------------------------------------------------------------------------------//
//
// NOT `character/phi_triangle.hpp`: Feathers' copy has diverged from the firmware's (backlog EH moved
// its bank's wrap into double, and `character/util.hpp` restructured the triangles), and the blend
// weights must come out of the same float operations the reference's do. `test_fx_smooth.cpp` compares
// these with the reference's bit for bit.

namespace fw {

constexpr float kPhi = 1.6180340f;
constexpr float kPhiN050 = 0.7861513f;
constexpr float kPhiN025 = 0.8872984f;
constexpr float kPhi025 = 1.1271566f;
constexpr float kPhi033 = 1.1746627f;
constexpr float kPhi050 = 1.2720196f;
constexpr float kPhi067 = 1.3871872f;
constexpr float kPhi075 = 1.4352958f;
constexpr float kPhi100 = 1.6180340f;

float wrapPhase(double phase) {
	const auto truncated = static_cast<int64_t>(phase);
	const double floored = (phase < 0.0 && phase != static_cast<double>(truncated)) ? static_cast<double>(truncated - 1)
	                                                                              : static_cast<double>(truncated);
	return static_cast<float>(phase - floored);
}

float triangleSimpleUnipolar(float phase, float duty = 1.0f) {
	phase = phase - floorf(phase);
	const float halfDuty = duty * 0.5f;
	const float invHalfDuty = 2.0f / duty;
	if (phase < halfDuty) {
		return phase * invHalfDuty;
	}
	if (phase < duty) {
		return (duty - phase) * invHalfDuty;
	}
	return 0.0f;
}

float triangleFloat(float phase, float duty = 1.0f) {
	phase = phase - floorf(phase);
	const float quarterDuty = duty * 0.25f;
	const float halfDuty = duty * 0.5f;
	if (phase < quarterDuty) {
		return phase / quarterDuty;
	}
	if (phase < halfDuty) {
		return (halfDuty - phase) / quarterDuty;
	}
	if (phase < halfDuty + quarterDuty) {
		return -(phase - halfDuty) / quarterDuty;
	}
	if (phase < duty) {
		return -(duty - phase) / quarterDuty;
	}
	return 0.0f;
}

struct PhiTriConfig {
	float phiFreq;
	float duty;
	float phaseOffset;
};

/// `evalTriangle`, unipolar (every entry of the blend bank is): `wrapPhase((phase + offset) · freq · mult)`.
float evalTriangle(double phase, float freqMult, const PhiTriConfig& cfg) {
	const float wrapped = wrapPhase((phase + cfg.phaseOffset) * cfg.phiFreq * freqMult);
	return triangleSimpleUnipolar(wrapped, cfg.duty);
}

/// `kBlendWeightBank`, VERBATIM — {phi frequency, duty, offset}. These twelve numbers are which of the four
/// harmonics or operators leads where; a transposed digit is a different instrument that still makes a
/// plausible noise, so a case pins the weights they produce against the reference's.
constexpr PhiTriConfig kBlendWeightBank[4] = {
	{ 2.019f, 0.8f, 0.00f }, // w0: √29/2 · 0.75
	{ 2.356f, 0.8f, 0.94f }, // w1: π · 0.75
	{ 2.771f, 0.8f, 0.05f }, // w2: e²/2 · 0.75
	{ 2.771f, 0.8f, 0.55f }, // w3: the same frequency, half a cycle on
};

} // namespace fw

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CODEC.                                                                                         //
//--------------------------------------------------------------------------------------------------//

Row rowOf(const State& state) {
	Row row {};
	const Settings& s = state.settings;
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Poly)] = s.poly ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Drive)] = s.drive;
	row[static_cast<size_t>(Param::HarmonicMode)] = static_cast<float>(s.harmonicMode);
	row[static_cast<size_t>(Param::Harmonic)] = s.harmonic;
	row[static_cast<size_t>(Param::Sweep)] = s.sweep;
	row[static_cast<size_t>(Param::Mix)] = s.mix;
	row[static_cast<size_t>(Param::HarmonicPhase)] = s.harmonicPhase;
	row[static_cast<size_t>(Param::Gamma)] = s.gamma;
	row[static_cast<size_t>(Param::SweepOn)] = s.sweepOn ? 1.0f : 0.0f;
	for (size_t i = 0; i < kPillCount; ++i) {
		const auto pill = static_cast<Pill>(i);
		row[static_cast<size_t>(pillAmountParam(pill))] = s.pillAmount[i];
		row[static_cast<size_t>(pillOnParam(pill))] = s.pillOn[i] ? 1.0f : 0.0f;
	}
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto read = [&](Param p) {
		const device::ParamDesc* d = descOf(p);
		if (d == nullptr) {
			return 0.0f;
		}
		return static_cast<size_t>(p) < fieldsPresent ? coerce(*d, row[static_cast<size_t>(p)])
		                                              : static_cast<float>(d->def);
	};
	Settings& s = out.settings;
	s.enabled = read(Param::Enabled) >= 0.5f;
	s.poly = read(Param::Poly) >= 0.5f;
	s.drive = read(Param::Drive);
	s.harmonicMode = static_cast<int>(read(Param::HarmonicMode));
	s.harmonic = read(Param::Harmonic);
	s.sweep = read(Param::Sweep);
	s.mix = read(Param::Mix);
	s.harmonicPhase = read(Param::HarmonicPhase);
	s.gamma = read(Param::Gamma);
	s.sweepOn = read(Param::SweepOn) >= 0.5f;
	for (size_t i = 0; i < kPillCount; ++i) {
		const auto pill = static_cast<Pill>(i);
		s.pillAmount[i] = read(pillAmountParam(pill));
		s.pillOn[i] = read(pillOnParam(pill)) >= 0.5f;
	}
	return out;
}

float valueOf(const State& state, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= static_cast<size_t>(Param::kCount)) {
		return 0.0f;
	}
	return rowOf(state)[index];
}

void setValueOf(State* state, Param param, float value) {
	const auto index = static_cast<size_t>(param);
	if (state == nullptr || index >= static_cast<size_t>(Param::kCount)) {
		return;
	}
	const device::ParamDesc* desc = descOf(param);
	if (desc == nullptr || !std::isfinite(value)) {
		return;
	}
	Row row = rowOf(*state);
	row[index] = coerce(*desc, value);
	*state = stateOf(row, static_cast<size_t>(Param::kCount));
}

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS IN THE FIRMWARE'S UNITS.                                                              //
//--------------------------------------------------------------------------------------------------//

FirmwareControls firmwareControlsOf(const Settings& s) noexcept {
	FirmwareControls out;
	const auto clampToQ31 = [](double v, double lo, double hi) {
		if (!(v == v)) {
			return static_cast<int32_t>(lo);
		}
		return static_cast<int32_t>(std::llround(std::clamp(v, lo, hi)));
	};
	const double fader = std::isfinite(s.drive) ? static_cast<double>(s.drive) : 0.5;
	out.drive = clampToQ31((2.0 * fader - 1.0) * kDriveQ31Full, -kDriveQ31Full, kDriveQ31Full - 1.0);
	// THE MODE IS THE ZONE AND HARMONIC'S POSITION IN ITS LAP IS THE OFFSET INSIDE IT, stopped one short of
	// the next zone (where `computeZoneQ31` stops the offset anyway); the LAP is whole units of
	// `harmonicPhaseOffset`, beside H Phase's eighths. So every setting is a state the firmware can be in.
	const int32_t mode = std::clamp(s.harmonicMode, 0, kZones - 1);
	const Lap harmonic = lapOf(s.harmonic);
	const int32_t offset =
	    clampToQ31(static_cast<double>(harmonic.position) * kZoneWidthQ31, 0.0, kZoneWidthQ31 - 1.0);
	out.harmonic = mode * kZoneWidthQ31 + offset;
	// `mix << 24` over 0..127, continuous: 1.0 is the firmware's 127 exactly.
	constexpr double kMixFull = 127.0 * 16777216.0;
	out.mix = clampToQ31(static_cast<double>(s.mix) * kMixFull, 0.0, kMixFull);
	out.harmonicPhaseOffset = harmonic.whole * kPhasePerLap + s.harmonicPhase * kHarmonicPhasePerClick;
	out.gammaPhase = s.gamma * kGammaPerClick;
	// THE SWEEP'S LAP IS `twistPhaseOffset` — the encoder's auto-wrap — and its position is the knob: in the meta
	// zones (4-7) on the lap where the firmware's phase is zero, over the whole knob (its phi path) on every other.
	// Never below zone 4, so never a named zone. Off, it is the knob at zero with no lap: no twist at all.
	out.sweepOn = s.sweepOn;
	if (s.sweepOn) {
		const Lap sweep = lapOf(s.sweep);
		out.twistPhaseOffset = sweep.whole * kPhasePerLap;
		constexpr int32_t kMetaStart = kOneQ31 / 2;
		const bool namedLap = static_cast<double>(out.twistPhaseOffset) + 1024.0 * out.gammaPhase == 0.0;
		out.twist = namedLap ? kMetaStart
		                           + clampToQ31(static_cast<double>(sweep.position) * (kOneQ31 - kMetaStart), 0.0,
		                                        static_cast<double>(kOneQ31 - kMetaStart))
		                     : clampToQ31(static_cast<double>(sweep.position) * kOneQ31, 0.0, static_cast<double>(kOneQ31));
	}
	for (size_t i = 0; i < kPillCount; ++i) {
		const float amount = std::isfinite(s.pillAmount[i]) ? std::clamp(s.pillAmount[i], 0.0f, 1.0f) : 0.0f;
		out.pills[i] = s.pillOn[i] ? amount : 0.0f;
	}
	return out;
}

Lap lapOf(float coordinate) noexcept {
	if (!std::isfinite(coordinate)) {
		return {};
	}
	Lap out { std::floor(coordinate), 0.0f };
	out.position = coordinate - out.whole; // exact: a float minus its own floor
	if (out.position >= 1.0f) {
		out.whole += 1.0f;
		out.position = 0.0f;
	}
	return out;
}

float wrapUnit(float x) noexcept {
	float r = std::fmod(x, 1.0f);
	if (r < 0.0f) {
		r += 1.0f;
		if (r >= 1.0f) { // -ε + 1 rounds to 1
			r = 0.0f;
		}
	}
	return r;
}

Zone zoneOf(int32_t harmonicQ31) noexcept {
	// `computeZoneQ31(value, 8)`, verbatim in integers; the position is a float division.
	constexpr int32_t zoneWidth = kOneQ31 / kZones;
	const int32_t value = std::max(harmonicQ31, 0);
	const int32_t index = std::min(value / zoneWidth, kZones - 1);
	const int32_t zoneStart = index * zoneWidth;
	const int32_t offset = std::min(value - zoneStart, zoneWidth - 1);
	return { index, static_cast<float>(offset * kZones) / kOneQ31F };
}

float driveGainOf(int32_t driveQ31) noexcept {
	const float normalizedDrive = (static_cast<float>(driveQ31) + 2147483648.0f) / 4294967296.0f;
	return normalizedDrive * normalizedDrive * 4.0f;
}

TwistParams twistParamsOf(int32_t twist, float twistPhaseOffset, float harmonicPhaseOffset, float gammaPhase) noexcept {
	// `computeSineShaperTwistParams`, statement for statement.
	constexpr int32_t kZone1 = kOneQ31 / 8;
	constexpr int32_t kZone2 = kOneQ31 / 4;
	constexpr int32_t kZone3 = (kOneQ31 / 8) * 3;
	constexpr int32_t kZone4 = kOneQ31 / 2;
	using fw::triangleFloat;
	using fw::triangleSimpleUnipolar;
	using fw::wrapPhase;

	TwistParams result;
	const float phH = harmonicPhaseOffset;
	result.phaseHarmonic = phH;
	result.phaseHarmonicFreqMod = phH;

	// "Use 1024x multiplier for gamma (non-overlapping zones, since range is 1024)".
	const double phRaw = static_cast<double>(twistPhaseOffset) + 1024.0 * gammaPhase;

	if (phRaw != 0.0) {
		float pos = static_cast<float>(twist) / kOneQ31F;
		pos = std::clamp(pos, 0.0f, 1.0f);

		const float fmW = 1.0f + pos * (0.25f + 0.25f * wrapPhase(phRaw * fw::kPhi025));
		const float fmE = 1.0f + pos * (0.25f + 0.25f * wrapPhase(phRaw * fw::kPhi033));
		const float fmR = 1.0f + pos * (0.25f + 0.25f * wrapPhase(phRaw * fw::kPhi067));
		const float fmF = 1.0f + pos * (0.25f + 0.25f * wrapPhase(phRaw * fw::kPhiN025));

		const float ph025 = wrapPhase(phRaw * fw::kPhi025);
		const float ph033 = wrapPhase(phRaw * fw::kPhi033);
		const float ph050 = wrapPhase(phRaw * fw::kPhi050);
		const float ph067 = wrapPhase(phRaw * fw::kPhi067);
		const float ph075 = wrapPhase(phRaw * fw::kPhi075);
		const float ph100 = wrapPhase(phRaw * fw::kPhi100);
		const float phN025 = wrapPhase(phRaw * fw::kPhiN025);
		const float phN050 = wrapPhase(phRaw * fw::kPhiN050);

		result.phaseHarmonic += pos * 5.0f + ph100;
		result.phaseHarmonicFreqMod += static_cast<float>(phRaw);

		const float wS = std::min(triangleSimpleUnipolar(pos * fw::kPhi025 * fmW + ph025 + 0.166f, 0.8f) * 2.0f, 1.0f);
		const float wP = triangleSimpleUnipolar(pos * fw::kPhi050 * fmW + ph050 + 0.984f, 0.7f);
		result.stereoWidth = wS * wP;
		result.stereoPhaseOffset = triangleSimpleUnipolar(pos * fw::kPhi067 * fmW + ph067 + 0.720f, 0.5f);
		result.stereoFreqMult = 1.0f + 0.5f * triangleSimpleUnipolar(pos * fw::kPhi100 * fmW + ph100 + 0.590f);
		result.stereoLfoRate = triangleSimpleUnipolar(pos * fw::kPhiN050 * fmW + phN050 + 0.333f, 0.8f);

		const float eS = triangleSimpleUnipolar(pos * fw::kPhi033 * fmE + ph033 + 0.970f, 0.5f);
		const float eT = triangleFloat(pos * fw::kPhi075 * fmE + ph075 + 0.896f, 0.5f);
		const float eAbs = std::abs(eT);
		result.evenAmount = eS * ((eT > 0.0f) ? eAbs : 0.0f);
		result.evenDryBlend = eS * ((eT < 0.0f) ? eAbs : 0.0f);

		const float rS = triangleSimpleUnipolar(pos * fw::kPhi067 * fmR + ph067 + 0.910f, 0.5f);
		const float rP = triangleSimpleUnipolar(pos * fw::kPhi100 * fmR + ph100 + 0.845f, 0.5f);
		result.rectAmount = rS * rP;
		const float fS = triangleSimpleUnipolar(pos * fw::kPhiN025 * fmF + phN025 + 0.001f, 0.8f);
		const float fP = triangleSimpleUnipolar(pos * fw::kPhiN050 * fmF + phN050 + 0.058f, 0.8f);
		result.feedbackAmount = fS * fP * fP * 0.25f;
		return result;
	}

	if (twist < kZone1) {
		// Zone 0: Width.
		const float pos = static_cast<float>(twist) / static_cast<float>(kZone1);
		result.stereoWidth = (pos < 0.25f) ? pos * 4.0f : (pos < 0.75f) ? 1.0f : (1.0f - pos) * 4.0f;
		result.stereoFreqMult = (pos <= 0.75f) ? 1.0f : 1.0f + (pos - 0.75f) * 12.0f;
		result.stereoPhaseOffset = pos;
	}
	else if (twist < kZone2) {
		// Zone 1: Evens — dry, evens one way, dry, evens the other.
		const float pos = static_cast<float>(twist - kZone1) / static_cast<float>(kZone2 - kZone1);
		const float tri = triangleFloat(pos, 1.0f);
		const float absTri = std::abs(tri);
		result.evenAmount = (tri > 0.0f) ? absTri : 0.0f;
		result.evenDryBlend = (tri < 0.0f) ? absTri : 0.0f;
	}
	else if (twist < kZone3) {
		// Zone 2: Rect — dry, rectified, dry.
		const float pos = static_cast<float>(twist - kZone2) / static_cast<float>(kZone3 - kZone2);
		result.rectAmount = triangleSimpleUnipolar(pos, 1.0f);
	}
	else if (twist < kZone4) {
		// Zone 3: Feedback, up to a quarter.
		const float pos = static_cast<float>(twist - kZone3) / static_cast<float>(kZone4 - kZone3);
		result.feedbackAmount = pos * 0.25f;
	}
	else {
		// Zones 4-7: Meta. `phRaw` is zero here, so every phase below is too; kept as the firmware
		// writes them so the two read the same.
		const float pos = static_cast<float>(twist - kZone4) / static_cast<float>(kOneQ31 - kZone4);

		const float ph025 = wrapPhase(phRaw * fw::kPhi025);
		const float ph033 = wrapPhase(phRaw * fw::kPhi033);
		const float ph050 = wrapPhase(phRaw * fw::kPhi050);
		const float ph067 = wrapPhase(phRaw * fw::kPhi067);
		const float ph075 = wrapPhase(phRaw * fw::kPhi075);
		const float ph100 = wrapPhase(phRaw * fw::kPhi100);
		const float phN025 = wrapPhase(phRaw * fw::kPhiN025);
		const float phN050 = wrapPhase(phRaw * fw::kPhiN050);

		const float fmW = 1.0f + pos * (0.25f + 0.25f * ph025);
		const float fmE = 1.0f + pos * (0.25f + 0.25f * ph033);
		const float fmR = 1.0f + pos * (0.25f + 0.25f * ph067);
		const float fmF = 1.0f + pos * (0.25f + 0.25f * phN025);

		result.phaseHarmonic += pos * 5.0f;

		const float wS = std::min(triangleSimpleUnipolar(pos * fw::kPhi025 * fmW + ph025 + 0.166f, 0.8f) * 2.0f, 1.0f);
		const float wP = triangleSimpleUnipolar(pos * fw::kPhi050 * fmW + ph050 + 0.984f, 0.7f);
		result.stereoWidth = wS * wP;
		result.stereoPhaseOffset = triangleSimpleUnipolar(pos * fw::kPhi067 * fmW + ph067 + 0.720f, 0.5f);
		result.stereoFreqMult = 1.0f + 0.5f * triangleSimpleUnipolar(pos * fw::kPhi100 * fmW + ph100 + 0.590f);
		result.stereoLfoRate = triangleSimpleUnipolar(pos * fw::kPhiN050 * fmW + phN050 + 0.333f, 0.8f);

		const float eS = triangleSimpleUnipolar(pos * fw::kPhi033 * fmE + ph033 + 0.970f, 0.5f);
		const float eT = triangleFloat(pos * fw::kPhi075 * fmE + ph075 + 0.896f, 0.5f);
		const float eAbs = std::abs(eT);
		result.evenAmount = eS * ((eT > 0.0f) ? eAbs : 0.0f);
		result.evenDryBlend = eS * ((eT < 0.0f) ? eAbs : 0.0f);

		const float rS = triangleSimpleUnipolar(pos * fw::kPhi067 * fmR + ph067 + 0.910f, 0.5f);
		const float rP = triangleSimpleUnipolar(pos * fw::kPhi100 * fmR + ph100 + 0.845f, 0.5f);
		result.rectAmount = rS * rP;

		const float fS = triangleSimpleUnipolar(pos * fw::kPhiN025 * fmF + phN025 + 0.001f, 0.8f);
		const float fP = triangleSimpleUnipolar(pos * fw::kPhiN050 * fmF + phN050 + 0.058f, 0.8f);
		result.feedbackAmount = fS * fP * fP * 0.25f;
	}
	return result;
}

float twistExtrasPhaseOf(const FirmwareControls& fc) noexcept {
	// The blend's own position (`targetsOf`'s `pos + phaseHarmonic`), and Gamma on a phi multiple — whole clicks of
	// it would otherwise wrap to nothing. Exactly the harmonic position when both phases are zero.
	const float gamma = wrapUnit(static_cast<float>(static_cast<double>(fc.gammaPhase) * fw::kPhi100));
	return wrapUnit(zoneOf(fc.harmonic).position + fc.harmonicPhaseOffset + gamma);
}

int32_t pillQ31Of(Pill pill, float amount, float extrasPhase) noexcept {
	// `computeSineShaperTwistParams`'s own zone starts — not multiples of one width: ONE_Q31 is odd.
	constexpr int32_t kZone1 = kOneQ31 / 8;
	constexpr int32_t kZone2 = kOneQ31 / 4;
	constexpr int32_t kZone3 = (kOneQ31 / 8) * 3;
	constexpr int32_t kWidth = kZone1;
	const auto offset = [](double position) {
		return static_cast<int32_t>(std::llround(std::clamp(position, 0.0, 1.0) * kWidth));
	};
	const double e = std::isfinite(extrasPhase) ? extrasPhase : 0.0;
	const double a = std::isfinite(amount) ? amount : 0.0;
	switch (pill) {
	case Pill::Width: return std::min(offset(e), kWidth - 1);
	case Pill::Evens: return kZone1 + std::min(offset(e), kWidth);
	case Pill::Rect: return kZone2 + offset(0.5 * a); // the triangle's rising side: 0 at dry, 1 at its peak
	case Pill::Fdbk: return kZone3 + std::min(offset(a), kWidth);
	case Pill::kCount: break;
	}
	return 0;
}

TwistParams twistOf(const FirmwareControls& fc) noexcept {
	// THE SWEEP, OR NOTHING: off, the firmware's knob at zero with no phase — no width, no evens, no rect, no
	// feedback, and `phaseHarmonic` the harmonic offset alone, as on every named zone.
	TwistParams out = fc.sweepOn ? twistParamsOf(fc.twist, fc.twistPhaseOffset, fc.harmonicPhaseOffset, fc.gammaPhase)
	                             : twistParamsOf(0, 0.0f, fc.harmonicPhaseOffset, 0.0f);
	const float e = twistExtrasPhaseOf(fc);
	for (size_t i = 0; i < kPillCount; ++i) {
		const float a = fc.pills[i];
		if (!(a > 0.0f)) {
			continue;
		}
		const auto pill = static_cast<Pill>(i);
		// THE FIRMWARE'S ZONE AT THE PILL'S POSITION, then its depth swapped for the pill's amount.
		const TwistParams z = twistParamsOf(pillQ31Of(pill, a, e), 0.0f, fc.harmonicPhaseOffset, 0.0f);
		switch (pill) {
		case Pill::Width:
			out.stereoWidth = std::min(out.stereoWidth + a, 1.0f);
			out.stereoFreqMult *= z.stereoFreqMult;
			out.stereoPhaseOffset = wrapUnit(out.stereoPhaseOffset + z.stereoPhaseOffset);
			break;
		case Pill::Evens:
			// THE FLAVOUR IS THE ZONE'S SIGN AT THE EXTRAS PHASE; at its zero crossings, the half it is in.
			if (z.evenAmount > 0.0f || (z.evenDryBlend <= 0.0f && e < 0.5f)) {
				out.evenAmount = std::min(out.evenAmount + a, 1.0f);
			}
			else {
				out.evenDryBlend = std::min(out.evenDryBlend + a, 1.0f);
			}
			break;
		case Pill::Rect: out.rectAmount = std::min(out.rectAmount + z.rectAmount, 1.0f); break;
		case Pill::Fdbk: out.feedbackAmount = std::min(out.feedbackAmount + z.feedbackAmount, 0.25f); break;
		case Pill::kCount: break;
		}
	}
	return out;
}

std::array<float, 4> blendWeightsOf(float posInZone, double gammaPhase) noexcept {
	// `computeBlendWeights4`.
	posInZone = std::clamp(posInZone, 0.0f, 1.0f);
	constexpr float kMinWeight = 0.01f; // -40 dB floor, which is also what keeps the sum off zero
	const auto linearToLog = [](float linear) {
		if (linear <= 0.0f) {
			return kMinWeight;
		}
		const float x3 = linear * linear * linear;
		return kMinWeight + x3 * (1.0f - kMinWeight);
	};
	const double phase = static_cast<double>(posInZone) + gammaPhase;
	const float w0 = linearToLog(fw::evalTriangle(phase, 1.0f, fw::kBlendWeightBank[0]));
	const float w1 = linearToLog(fw::evalTriangle(phase, 1.0f, fw::kBlendWeightBank[1]));
	const float w2 = linearToLog(fw::evalTriangle(phase, 1.0f, fw::kBlendWeightBank[2]));
	const float w3 = linearToLog(fw::evalTriangle(phase, 1.0f, fw::kBlendWeightBank[3]));
	const float wSum = w0 + w1 + w2 + w3;
	return { w0 / wSum, w1 / wSum, w2 / wSum, w3 / wSum };
}

float stereoLfoValueOf(float phase) noexcept {
	constexpr float kStereoLfoDuty = 0.5f;
	return fw::triangleFloat(phase, kStereoLfoDuty);
}

float stereoLfoSeedOf(float phaseHarmonic) noexcept {
	return fw::wrapPhase(static_cast<double>(phaseHarmonic) * fw::kPhi);
}

Coefficients chebyshevCoefficientsOf(float posInZone, double gammaPhase) noexcept {
	// `computeShaperWeightsFromPos` → `weightsToCoeffs`. c1 = Σw is what cancels the fundamental the
	// higher terms put back: each normalised Chebyshev extracts sin(nθ)/n from sin(θ).
	posInZone = std::clamp(posInZone, 0.0f, 1.0f);
	const std::array<float, 4> w = blendWeightsOf(posInZone, gammaPhase);
	const float w3 = w[0];
	const float w5 = w[1];
	const float w7 = w[2];
	const float w9 = w[3];
	Coefficients c;
	c.c1 = w3 + w5 + w7 + w9;
	c.c3 = -(w3 * (4.0f / 3.0f) + w5 * 4.0f + w7 * 8.0f + w9 * (40.0f / 3.0f));
	c.c5 = w5 * 3.2f + w7 * 16.0f + w9 * 48.0f;
	c.c7 = -w7 * (64.0f / 7.0f) - w9 * 64.0f;
	c.c9 = w9 * (256.0f / 9.0f);
	return c;
}

void Coefficients::updateQ31() noexcept {
	c3q = saturatingQ31(c3 * kOneQ31F);
	c5q = saturatingQ31(c5 * kOneQ31F);
	c7q = saturatingQ31(c7 * kOneQ31F);
	c9q = saturatingQ31(c9 * kOneQ31F);
	c1q = saturatingQ31(c1 * static_cast<float>(kOneQ31 >> 2));
}

//--------------------------------------------------------------------------------------------------//
// THE PRIMITIVES.                                                                                    //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE SINE TABLE, GENERATED FROM ITS DEFINITION: 256 points of one cycle, each `round(32767 · sin(2π i / 256))`,
/// and a 257th equal to the first so that point `i + 1` always exists for the blend below. Built once, when the
/// library loads — never on the audio thread. It is NOT copied from anywhere: it was checked equal to the firmware's
/// table at every one of its 257 points when it was written, which is what makes the reference's colour
/// reproducible without carrying its text.
const std::array<int16_t, 257> kSineTable = [] {
	std::array<int16_t, 257> t {};
	for (int i = 0; i < 256; ++i) {
		const double angle = 2.0 * 3.14159265358979323846 * static_cast<double>(i) / 256.0;
		t[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(32767.0 * std::sin(angle)));
	}
	t[256] = t[0];
	return t;
}();

} // namespace

int32_t tableSine(uint32_t phase) noexcept {
	// A 32-bit phase is one cycle. Its top byte chooses the table point at or below it; the sixteen bits under that
	// byte say how far the phase has travelled toward the NEXT point, as a weight out of 65536. The result is the two
	// neighbours mixed by that weight, left in 16-bit × 16-bit units — so a full-scale point, 32767, comes out as
	// 32767 · 65536, just under 2^31. Linear interpolation, and deliberately so: its small error is part of the sound
	// the reference makes with this shaper, which `std::sin` would not reproduce.
	const auto point = static_cast<size_t>(phase >> 24);
	const auto towardNext = static_cast<int32_t>((phase >> 8) & 0xFFFFu);
	const int32_t below = kSineTable[point];
	const int32_t above = kSineTable[point + 1];
	return below * (65536 - towardNext) + above * towardNext;
}
namespace {

/// One pass of the odd cubic 3v - 8v^3 in Q31. The square and the cube are each doubled back to Q31
/// and wrapped to 32 bits before they are used again; the final difference is scaled by eight and
/// wrapped once. Every intermediate is held in int64, where it cannot overflow, and reduced to 32
/// bits by a modular conversion. The wrapping is the fold the shaper is meant to make when driven.
int32_t cubicFoldStage(int32_t v) noexcept {
	// Three quarters of the largest Q31 value (2^31 - 1), truncated: 1610612735, one below 0.75 * 2^31.
	// The pinned values need this exact constant; at 0.75 * 2^31 every small positive input comes out
	// one step (of eight) higher after each pass.
	constexpr int32_t kThreeQuarters = 1610612735;
	const auto high = [](int32_t p, int32_t q) -> int64_t {
		return feathers::character::multiply_32x32_rshift32(p, q);
	};
	const auto wrapTo32 = [](int64_t value) -> int32_t {
		return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(value)));
	};
	const int32_t square = wrapTo32(2 * high(v, v));
	const int32_t cube = wrapTo32(2 * high(square, v));
	return wrapTo32(8 * (high(kThreeQuarters, v) - static_cast<int64_t>(cube)));
}

} // namespace

int32_t cascadePolynomial(int32_t x) noexcept {
	// Two passes of the folding cubic, the second fed the first's wrapped output: about nine times
	// gain near zero, folding over once the products leave the int32 range.
	return cubicFoldStage(cubicFoldStage(x));
}

int32_t shiftSaturate(int32_t x, int shift) noexcept {
	// Clamp x to a signed integer of (32 - shift) bits, then multiply by 2^shift. The clamped value
	// times the scale always fits in int32; the arithmetic is in int64 so no negative value is
	// shifted. Valid for shift in 0..31.
	const int64_t limit = int64_t { 1 } << (31 - shift);
	const int64_t clamped = std::clamp<int64_t>(x, -limit, limit - 1);
	return static_cast<int32_t>(clamped * (int64_t { 1 } << shift));
}

int32_t saturatingQ31(float x) noexcept {
	if (x != x) {
		return 0;
	}
	if (x >= 2147483648.0f) {
		return INT32_MAX;
	}
	if (x <= -2147483648.0f) {
		return INT32_MIN;
	}
	return static_cast<int32_t>(x);
}

int32_t shapeSample(int32_t input, float driveGain, int zone, const Coefficients& w, float evenAmount,
                    float evenDryBlend, float rectAmount, ShapeProbe* probe) noexcept {
	// `sineShapeCore`, statement for statement. The stereo core is the same arithmetic run twice with a
	// shared branch, so one channel's function serves both.
	const float inputF = static_cast<float>(input) * driveGain;
	constexpr float kMaxQ31 = 2147483647.0f; // which is 2^31 as a float: the conversion saturates
	const float inputFClamped = std::clamp(inputF, -kMaxQ31, kMaxQ31);
	const int32_t driven = saturatingQ31(inputFClamped);
	if (probe != nullptr) {
		probe->driven = driven;
		probe->zoneInput = driven;
	}

	int32_t shaped = 0;

	if (zone == 7) {
		// POLY: saturate at 0 dB, optionally self-multiply toward x·|x| (evens), the cascade, optionally
		// the cascade again, then -3 dB. INTEGER, AND IT MUST STAY SO: the cascade's wrap is the sound.
		int32_t scaledInput = shiftSaturate(driven, 7);
		if (probe != nullptr) {
			probe->zoneInput = scaledInput;
		}
		const float selfMulBlend = w.c3;
		if (selfMulBlend > 0.01f) {
			const int32_t absInput = (scaledInput < 0) ? wneg(scaledInput) : scaledInput;
			const int32_t selfMulInput = wshl(mulhiRounded(scaledInput, absInput), 1);
			const int32_t blendQ = saturatingQ31(selfMulBlend * kOneQ31F);
			scaledInput = wadd(scaledInput, wmul(mulhiRounded(wsub(selfMulInput, scaledInput), blendQ), 2));
		}
		shaped = cascadePolynomial(scaledInput) >> 8;
		const int32_t cascadeBlend = saturatingQ31(w.c1 * kOneQ31F);
		if (cascadeBlend > 0) {
			const int32_t moreHarmonics = cascadePolynomial(wshl(shaped, 7)) >> 7;
			shaped = wadd(shaped, wmul(mulhiRounded(wsub(moreHarmonics, shaped), cascadeBlend), 2));
		}
		shaped = wshl(mulhi(shaped, kMinus3dBQ31), 1);
	}
	else if (zone == 0 || zone == 1) {
		// 3579 / 3579wm: THE INPUT SATURATES AT HALF THE REFERENCE'S 0 dBFS (<< 8), which is x = 1, where the
		// normalised Chebyshevs give exactly their harmonics. Past it the input is flat-topped, not the
		// polynomial exploded.
		const int32_t scaledInput = shiftSaturate(driven, 8);
		if (probe != nullptr) {
			probe->zoneInput = scaledInput;
		}
		const float rawX = static_cast<float>(scaledInput) / kOneQ31F;
		float x = rawX;
		if (zone == 1) {
			// sin(x · 3π/4) from the table: the phase is the input times 0.75.
			const int32_t phaseInt = (scaledInput >> 1) + (scaledInput >> 2);
			constexpr float kInvQ31 = 1.0f / kOneQ31F;
			x = static_cast<float>(tableSine(static_cast<uint32_t>(phaseInt))) * kInvQ31;
		}
		const float x2 = x * x;
		const float result = x * (w.c1 + x2 * (w.c3 + x2 * (w.c5 + x2 * (w.c7 + w.c9 * x2))));
		// "Tune empirically": 0.027 · 0.707 of the q31 ceiling, 2.443 in these units.
		constexpr float kChebyBoost = 0.027f;
		constexpr float kMinus3dB = 0.707f;
		constexpr float kChebyGainScaled = kChebyBoost * kMinus3dB * kOneQ31F;
		const float scaled = result * kChebyGainScaled;
		shaped = saturatingQ31(std::clamp(scaled, -2147483648.0f, 2147483647.0f));
	}
	else if (zone >= 2 && zone <= 6) {
		// THE FM ZONES: the driven input times 1..4 becomes a PHASE into the sine table, and four operators
		// of it are blended — each peaking near half the table's ceiling, then >> 7 into the reference's level.
		const int32_t w0q = w.c3q;
		const int32_t w1q = w.c5q;
		const int32_t w2q = w.c7q;
		const int32_t w3q = w.c9q;
		const int32_t gained = wshl(mulhi(driven, w.c1q), 2);
		const uint32_t phase1 = static_cast<uint32_t>(gained) << 8;
		const int32_t sine1 = tableSine(phase1);
		const auto u = [](int32_t v) { return static_cast<uint32_t>(v); };

		int32_t m0 = 0;
		int32_t m1 = 0;
		int32_t m2 = 0;
		int32_t m3 = 0;
		if (zone == 2) {
			// FM: add, ring, FM, fold, each at twice the input's phase.
			const uint32_t phase2 = phase1 << 1;
			const int32_t sine2 = tableSine(phase2);
			m0 = ((sine1 >> 1) + (sine2 >> 1)) >> 1;
			m1 = mulhi(sine1, tableSine(phase2 + 0x40000000u));
			m2 = tableSine(phase1 + u(sine2 >> 1)) >> 1;
			m3 = tableSine(u(sine1) << 1) >> 1;
		}
		else if (zone == 3) {
			// Fold, k = 1..4.
			m0 = tableSine(u(sine1)) >> 1;
			m1 = tableSine(u(sine1) << 1) >> 1;
			m2 = tableSine(u(sine1) + (u(sine1) << 1)) >> 1;
			m3 = tableSine(u(sine1) << 2) >> 1;
		}
		else if (zone == 4) {
			// Ring: sin(x) · sin(nx), n = 2..5.
			m0 = mulhi(sine1, tableSine(phase1 << 1));
			m1 = mulhi(sine1, tableSine(phase1 + (phase1 << 1)));
			m2 = mulhi(sine1, tableSine(phase1 << 2));
			m3 = mulhi(sine1, tableSine((phase1 << 2) + phase1));
		}
		else if (zone == 5) {
			// Add: sin(x) + sin(nx), n = 2..5.
			const int32_t s2 = tableSine(phase1 << 1);
			const int32_t s3 = tableSine(phase1 + (phase1 << 1));
			const int32_t s4 = tableSine(phase1 << 2);
			const int32_t s5 = tableSine((phase1 << 2) + phase1);
			m0 = ((sine1 >> 1) + (s2 >> 1)) >> 1;
			m1 = ((sine1 >> 1) + (s3 >> 1)) >> 1;
			m2 = ((sine1 >> 1) + (s4 >> 1)) >> 1;
			m3 = ((sine1 >> 1) + (s5 >> 1)) >> 1;
		}
		else {
			// Mod: FM at depths 0.25, 0.5, 0.75, 1 with the modulator at twice the phase.
			const int32_t mod = tableSine(phase1 << 1);
			m0 = tableSine(phase1 + u(mod >> 2)) >> 1;
			m1 = tableSine(phase1 + u(mod >> 1)) >> 1;
			m2 = tableSine(phase1 + u((mod >> 1) + (mod >> 2))) >> 1;
			m3 = tableSine(phase1 + u(mod)) >> 1;
		}

		int32_t blended = 0;
		blended = addSaturate(blended, wshl(mulhi(m0, w0q), 1));
		blended = addSaturate(blended, wshl(mulhi(m1, w1q), 1));
		blended = addSaturate(blended, wshl(mulhi(m2, w2q), 1));
		blended = addSaturate(blended, wshl(mulhi(m3, w3q), 1));
		shaped = blended >> 7;
	}

	// EVENS AND RECT (Twist zones 1 and 2), fused in float as the firmware fuses them.
	const bool hasEven = (evenAmount > 0.0f || evenDryBlend > 0.0f);
	const bool hasRect = (rectAmount > 0.0f);
	if (hasEven || hasRect) {
		float shapedF = static_cast<float>(shaped);
		if (hasEven) {
			if (shapedF > 0.0f && evenAmount > 0.0f) {
				constexpr float kNormFactor = 1.0f / kOneQ31F;
				constexpr float kBoost = 256.0f;
				const float x = std::clamp(shapedF * kNormFactor * kBoost, 0.0f, 1.0f);
				constexpr float kEvenGain = 0.85f;
				const float ratio = 1.0f - evenAmount * (1.0f - x);
				shapedF = ratio * shapedF * kEvenGain;
			}
			else if (shapedF < 0.0f && evenDryBlend > 0.0f) {
				const float absShapedF = -shapedF;
				const float absDrivenF = std::abs(static_cast<float>(driven));
				const float cappedDrivenAbs = std::min(absDrivenF, absShapedF * 2.0f);
				shapedF = -(absShapedF * (1.0f - evenDryBlend) + cappedDrivenAbs * evenDryBlend);
			}
		}
		if (hasRect) {
			const float absVal = std::abs(shapedF);
			shapedF = shapedF + (absVal - shapedF) * rectAmount;
		}
		shaped = saturatingQ31(shapedF);
	}
	return shaped;
}

//--------------------------------------------------------------------------------------------------//
// THE TARGETS, AND THE PICTURE BUILT ON THEM.                                                        //
//--------------------------------------------------------------------------------------------------//

CoefficientTargets targetsOf(int zone, float posInZone, const TwistParams& twist, float stereoMod) noexcept {
	// `sineShapeBuffer`'s target selection, statement for statement — every zone repurposes the same five
	// numbers (see `Coefficients`).
	Coefficients left {};
	Coefficients right {};
	if (zone == 7) {
		left.c1 = posInZone;                                           // cascade blend
		left.c3 = wrapUnit(twist.phaseHarmonic + 0.5f + 1.0f); // self-multiplication blend
		right = left;
	}
	else if (zone == 0 || zone == 1) {
		const float pos = wrapUnit(posInZone + twist.phaseHarmonic + 1.0f);
		constexpr float kMaxPosOffset = 0.06f;
		const float posOffset = twist.stereoWidth * stereoMod * kMaxPosOffset;
		left = chebyshevCoefficientsOf(std::clamp(pos - posOffset, 0.0f, 1.0f), twist.phaseHarmonicFreqMod);
		right = chebyshevCoefficientsOf(std::clamp(pos + posOffset, 0.0f, 1.0f), twist.phaseHarmonicFreqMod);
	}
	else {
		const float pos = wrapUnit(posInZone + twist.phaseHarmonic + 1.0f);
		constexpr float kMaxPosOffset = 0.08f;
		const float posOffset = twist.stereoWidth * stereoMod * kMaxPosOffset;
		const std::array<float, 4> wL = blendWeightsOf(std::clamp(pos - posOffset, 0.0f, 1.0f), twist.phaseHarmonicFreqMod);
		const std::array<float, 4> wR = blendWeightsOf(std::clamp(pos + posOffset, 0.0f, 1.0f), twist.phaseHarmonicFreqMod);
		const float inputGain = 1.0f + pos * 3.0f; // "inputGainMult 1.0-4.0"
		left = { inputGain, wL[0], wL[1], wL[2], wL[3] };
		right = { inputGain, wR[0], wR[1], wR[2], wR[3] };
	}
	return { left.c1, left.c3, left.c5, left.c7, left.c9, right.c3, right.c5, right.c7, right.c9 };
}

namespace {
// THE SATURATION POINTS OF `shapeSample`, as input levels: `<< 8` saturates at driven = 2^23 (0.5 here),
// `<< 7` at 2^24 (1.0).
float ceilingReferenceOf(int zone) noexcept { return (zone == 0 || zone == 1) ? 0.5f : 1.0f; }
} // namespace

Picture pictureOf(const Settings& settings) noexcept {
	const FirmwareControls fc = firmwareControlsOf(settings);
	const Zone zone = zoneOf(fc.harmonic);
	Picture p;
	p.zone = zone.index;
	p.driveGain = driveGainOf(fc.drive);
	p.twist = twistOf(fc);
	const CoefficientTargets t = targetsOf(p.zone, zone.position, p.twist, 0.0f);
	p.weights = { t[0], t[1], t[2], t[3], t[4] };
	p.weights.updateQ31();
	p.clips = p.zone == 0 || p.zone == 1 || p.zone == 7;
	p.ceilingInput = ceilingOfGain(p.zone, p.driveGain);
	return p;
}

float transferAt(const Picture& picture, float input) noexcept {
	const int32_t q = saturatingQ31(input * kQ31PerUnit);
	const int32_t shaped = shapeSample(q, picture.driveGain, picture.zone, picture.weights, picture.twist.evenAmount,
	                                   picture.twist.evenDryBlend, picture.twist.rectAmount);
	return static_cast<float>(shaped) / kQ31PerUnit;
}

float ceilingOfGain(int zone, float gain) noexcept {
	return gain > 0.0f ? ceilingReferenceOf(zone) / gain : std::numeric_limits<float>::infinity();
}

float driveKnobForCeiling(int zone, float ceilingInput) noexcept {
	if (!(ceilingInput > 0.0f) || std::isinf(ceilingInput)) {
		return ceilingInput > 0.0f ? 0.0f : 1.0f; // +inf is silence; zero (or NaN) asks for all the gain there is
	}
	const float gain = std::clamp(ceilingReferenceOf(zone) / ceilingInput, 0.0f, 4.0f);
	// `driveGainOf` of the fader's q31 is 4v², so the fader is √gain / 2.
	return std::clamp(0.5f * std::sqrt(gain), 0.0f, 1.0f);
}

float ceilingInputOf(int zone, float driveKnob) noexcept {
	// THE SAME ROAD AS `firmwareControlsOf` → `driveGainOf`, so the drawn ceiling is the one the shaper runs at.
	Settings s;
	s.drive = driveKnob;
	return ceilingOfGain(zone, driveGainOf(firmwareControlsOf(s).drive));
}

std::array<float, kHarmonicCount> harmonicsOf(const Picture& picture, float level) noexcept {
	std::array<float, kHarmonicSamples> y {};
	for (int n = 0; n < kHarmonicSamples; ++n) {
		const double phase = 2.0 * M_PI * static_cast<double>(n) / kHarmonicSamples;
		y[static_cast<size_t>(n)] = transferAt(picture, static_cast<float>(level * std::sin(phase)));
	}
	std::array<float, kHarmonicCount> out {};
	for (int k = 1; k <= kHarmonicCount; ++k) {
		double re = 0.0;
		double im = 0.0;
		for (int n = 0; n < kHarmonicSamples; ++n) {
			const double a = 2.0 * M_PI * static_cast<double>(k * n) / kHarmonicSamples;
			re += y[static_cast<size_t>(n)] * std::cos(a);
			im -= y[static_cast<size_t>(n)] * std::sin(a);
		}
		out[static_cast<size_t>(k - 1)] = static_cast<float>(2.0 * std::sqrt(re * re + im * im) / kHarmonicSamples);
	}
	return out;
}

float stackTrimOf(int stackCount, float detuneCents, float pitchSemitones, float coherence) noexcept {
	if (stackCount <= 1) {
		return 1.0f; // exactly: a stack of one is not trimmed at all
	}
	const double hz = 440.0 * std::exp2((static_cast<double>(pitchSemitones) - 69.0) / 12.0);
	const double beat = hz * (std::exp2(std::max(0.0, static_cast<double>(detuneCents)) / 1200.0) - 1.0);
	const double rho = static_cast<double>(coherence) * std::exp(-beat * kStackCoherenceSeconds);
	// A SUM'S POWER CANNOT BE NEGATIVE, so 1 + (N − 1) ρ0 ≥ 0 for any real tables; the floor holds a
	// pathological ρ0 to +6 dB of boost rather than a division by zero.
	const double sum = std::max(0.25, 1.0 + static_cast<double>(stackCount - 1) * rho);
	return static_cast<float>(1.0 / std::sqrt(sum));
}

} // namespace feathers::fx::smooth
