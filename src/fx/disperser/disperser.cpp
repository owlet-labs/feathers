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

#include "fx/disperser/disperser.h"
#include "fx/scrub.h"

#include "character/phi_triangle.hpp"
#include "character/util.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace phi = owlet::dsp::phi;

namespace feathers::fx::disperser {

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
// THE FIRMWARE'S ARITHMETIC, AT ITS LEVEL. 1.0 IS THE Q31 CEILING; A FULL-SCALE SIGNAL IS 1/128.     //
//--------------------------------------------------------------------------------------------------//

/// `qadd`, `vqadd_s32`, `vqsub_s32` — saturate at the ceiling.
float sat(float x) noexcept { return std::clamp(x, -1.0f, 1.0f); }

/// `signed_saturate<30>` — ARM `ssat #30`, the signed 30-bit range: a QUARTER of the ceiling (the
/// firmware's comment says "±0.5"; `ssat #N` is ±2^(N-1)).
float sat30(float x) noexcept { return std::clamp(x, -0.25f, 0.25f); }

/// **`vshl_n_s32(x, 1)` DOES NOT SATURATE** — it wraps. The firmware doubles every stage's output with
/// it after a halving multiply (`stageGains_` are stored as half), and doubles every delay write the
/// same way. Past half the ceiling the result comes back in at the other end, as the hardware's would.
float wrap(float x) noexcept {
	if (x >= -1.0f && x < 1.0f) {
		return x;
	}
	return x - 2.0f * std::floor((x + 1.0f) * 0.5f);
}


/// **A DENORMAL GUARD AND NOTHING MORE — SILENCE IS `kRestLevel`'s JOB, NOT THIS.** Two drafts used a
/// flush as the road to silence and both made a floor instead, MEASURED: at 1e-12 a Chirp tail sat at
/// 3.5e-8 of full scale for ten seconds, and at one q31 LSB (4.7e-10) it sat HIGHER, at 1.6e-5. Zeroing
/// one DF2T state while its partner is not is a step into the stage, and a stage at the 14 Hz floor —
/// poles 0.001 from the unit circle — amplifies a step by up to 10^6. So the flush is here only to keep
/// the arithmetic out of the denormals, where what it injects is 10^-24 after that gain.
constexpr float kFlush = 1.0e-30f;
constexpr double kFlushD = 1.0e-30;

/// `sat` for the allpass's double arithmetic — see `AllpassCoeffs` on why it is double.
double satD(double x) noexcept { return std::clamp(x, -1.0, 1.0); }

// `disperser.cpp`'s three local helpers, VERBATIM in their types: `pos` a float, `phRaw` a double.
float freqMod(float pos, double phRaw, float phiFreq, float range = 0.5f, float duty = 0.8f) {
	return 1.0f + owlet::dsp::triangleSimpleUnipolar(phi::wrapPhase((pos + phRaw) * phiFreq), duty) * range;
}

float phiTri(float pos, double phRaw, float phiFreq, float fm, float offset, float duty = 0.8f) {
	const double phase = static_cast<double>(pos) * phiFreq * fm + (phRaw + offset) * phiFreq;
	return owlet::dsp::triangleSimpleUnipolar(phi::wrapPhase(phase), duty);
}

float phiTriBi(float pos, double phRaw, float phiFreq, float fm, float offset, float duty = 0.5f) {
	const double phase = static_cast<double>(pos) * phiFreq * fm + (phRaw + offset) * phiFreq;
	return owlet::dsp::triangleFloat(phi::wrapPhase(phase), duty);
}

/// The two LFOs' shape: a triangle over one cycle, 0 → +1 → -1 → 0.
float lfoTriangle(double phase) {
	const auto p = static_cast<float>(phase);
	if (p < 0.25f) {
		return p * 4.0f;
	}
	if (p < 0.75f) {
		return 2.0f - p * 4.0f;
	}
	return p * 4.0f - 4.0f;
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CODEC.                                                                                         //
//--------------------------------------------------------------------------------------------------//

Row rowOf(const State& state) {
	Row row {};
	const Settings& s = state.settings;
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::TopoMode)] = static_cast<float>(s.topoMode);
	row[static_cast<size_t>(Param::TopoPhase)] = s.topoPhase;
	row[static_cast<size_t>(Param::TwistMode)] = static_cast<float>(s.twistMode);
	row[static_cast<size_t>(Param::TwistPos)] = s.twistPos;
	row[static_cast<size_t>(Param::TwistPhase)] = s.twistPhase;
	row[static_cast<size_t>(Param::Gamma)] = s.gamma;
	row[static_cast<size_t>(Param::Stages)] = static_cast<float>(s.stages);
	row[static_cast<size_t>(Param::KeyTrack)] = s.keyTrack ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Freq)] = s.freqLog2;
	row[static_cast<size_t>(Param::Offset)] = s.offsetOctaves;
	row[static_cast<size_t>(Param::QOffset)] = s.qOffsetOctaves;
	for (size_t i = 0; i < kPillCount; ++i) {
		row[static_cast<size_t>(pillAmountParam(static_cast<Pill>(i)))] = s.pillAmount[i];
		row[static_cast<size_t>(pillOnParam(static_cast<Pill>(i)))] = s.pillOn[i] ? 1.0f : 0.0f;
	}
	// THE FURNITURE'S DEFAULTS: the slot reads these, the device never does, and a zero Mix would be silence.
	row[static_cast<size_t>(Param::Mix)] = 1.0f;
	row[static_cast<size_t>(Param::WetGain)] = 0.5f;
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
	const auto choice = [&](Param p, size_t count) {
		return static_cast<uint32_t>(std::clamp<long>(std::lround(read(p)), 0, static_cast<long>(count) - 1));
	};
	Settings& s = out.settings;
	s.enabled = read(Param::Enabled) >= 0.5f;
	s.topoMode = choice(Param::TopoMode, std::size(kTopoModeNames));
	s.topoPhase = read(Param::TopoPhase);
	s.twistMode = choice(Param::TwistMode, std::size(kTwistModeNames));
	s.twistPos = read(Param::TwistPos);
	s.twistPhase = read(Param::TwistPhase);
	const TopoPlace topoPlace = topoPlaceOf(s.topoMode, s.topoPhase);
	s.topo = static_cast<float>(topoPlace.coordinate);
	s.topoLap = static_cast<float>(topoPlace.lap);
	s.twist = static_cast<float>(twistCoordinateOf(s.twistMode, s.twistPos, s.twistPhase));
	s.gamma = read(Param::Gamma);
	s.stages = std::clamp(static_cast<int>(std::lround(read(Param::Stages))), kMinStages, kMaxStages);
	s.keyTrack = read(Param::KeyTrack) >= 0.5f;
	s.freqLog2 = read(Param::Freq);
	s.offsetOctaves = read(Param::Offset);
	s.qOffsetOctaves = read(Param::QOffset);
	for (size_t i = 0; i < kPillCount; ++i) {
		s.pillAmount[i] = read(pillAmountParam(static_cast<Pill>(i)));
		s.pillOn[i] = read(pillOnParam(static_cast<Pill>(i))) >= 0.5f;
	}
	return out;
}

double topoCoordinateOf(uint32_t mode, double pos) noexcept {
	const double m = static_cast<double>(std::min<uint32_t>(mode, kZones - 1));
	return (m + std::clamp(pos, 0.0, 1.0) * kFencedSpan) / static_cast<double>(kZones);
}

TopoPlace topoPlaceOf(uint32_t mode, double phase) noexcept {
	if (!std::isfinite(phase)) {
		return { topoCoordinateOf(mode, 0.0), 0.0 };
	}
	const double inZone = std::clamp(phase, 0.0, 1.0);
	return { topoCoordinateOf(mode, inZone), phase - inZone };
}

double twistCoordinateOf(uint32_t mode, double pos, double phase) noexcept {
	if (mode >= kTwistInfinite) {
		return kTwistInfiniteStart + std::max(phase, 0.0);
	}
	return topoCoordinateOf(mode, pos);
}

double overrideField(double field, double amount) noexcept {
	// `dsp::fieldWithOverride`, restated in double for the pills: the Shaper's law, bit for its shape.
	const double k = std::clamp(amount, 0.0, 1.0);
	const double f = std::clamp(field, 0.0, 1.0);
	if (k <= 0.5) {
		return f * (k * 2.0);
	}
	return f + (1.0 - f) * ((k - 0.5) * 2.0);
}

void applyPills(const Settings& s, TopoParams& topo, TwistParams& twist) noexcept {
	const auto on = [&](Pill p) { return s.pillOn[static_cast<size_t>(p)]; };
	const auto amount = [&](Pill p) { return static_cast<double>(s.pillAmount[static_cast<size_t>(p)]); };
	// A LEAN FROM A NEUTRAL: the pill scales its size and keeps its side (a zero lean lifts to the positive).
	const auto lean = [](double value, double neutral, double most, double k) {
		const double d = value - neutral;
		const double side = d < 0.0 ? -1.0 : 1.0;
		return neutral + side * most * overrideField(std::abs(d) / most, k);
	};
	// OFF DISABLES THE EXTRA (`kPillOff`). ON AT EXACTLY 0.5 — the default — LEAVES THE ZONES' VALUE UNTOUCHED:
	// every law below is the identity there only up to rounding (the lean's divide, Q's log and pow) and the
	// default must be the firmware bit for bit, which the transcription comparisons assert.
	const auto apply = [&](Pill p, double& value, auto law) {
		if (!on(p)) {
			value = kPillOff[static_cast<size_t>(p)];
		}
		else if (amount(p) != 0.5) {
			value = law(value, amount(p));
		}
	};
	apply(Pill::Width, twist.width, overrideField);
	apply(Pill::Punch, twist.punch, overrideField);
	apply(Pill::Chirp, twist.chirpAmount, overrideField);
	apply(Pill::Curve, twist.spreadCurve, [&](double v, double k) { return lean(v, 0.5, 0.5, k); });
	apply(Pill::QTilt, twist.qTilt, [&](double v, double k) { return lean(v, 0.0, 0.8, k); });
	apply(Pill::Q, topo.q, [](double v, double k) {
		// Q IS `0.5·40^t`, t in 0..1 (`computeTopoParams`): the pill acts on t.
		const double t = std::log(std::max(v, 0.5) / 0.5) / std::log(40.0);
		return 0.5 * std::pow(40.0, overrideField(t, k));
	});
	apply(Pill::Detune, topo.detuning, overrideField);
	apply(Pill::Emphasis, topo.emphasis, [&](double v, double k) { return lean(v, 0.0, 0.7, k); });
	apply(Pill::Harmonic, topo.harmonicBlend, overrideField);
}

double qWithOffset(double q, double octaves) noexcept {
	if (octaves == 0.0 || !std::isfinite(octaves)) {
		return q;
	}
	return std::clamp(q * std::exp2(std::clamp(octaves, -kQOffsetOctaves, kQOffsetOctaves)), kQMin, kQMax);
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
// THE MAPPING.                                                                                       //
//--------------------------------------------------------------------------------------------------//

Coordinate splitCoordinate(double c) noexcept {
	if (!std::isfinite(c)) {
		return {};
	}
	const double lap = std::floor(c);
	return { std::clamp(c - lap, 0.0, 1.0), lap };
}

namespace {

constexpr int32_t kOneQ31 = 2147483647;
constexpr int32_t kZoneWidthQ31 = kOneQ31 / static_cast<int32_t>(kZones);

/// **THE KNOB AS THE FIRMWARE HOLDS IT — A q31.** Its zone width is `ONE_Q31 / 8`, seven short of a
/// power of two, so a position derived in double differs from the firmware's in the ninth digit; and
/// the delay offsets are TRUNCATED to whole samples downstream, where a ninth-digit difference can move
/// an echo by a sample (MEASURED: 7657 against 7656). So the zone arithmetic is the firmware's own.
int32_t knobQ31(double knob) noexcept {
	return static_cast<int32_t>(std::clamp(knob, 0.0, 1.0) * static_cast<double>(kOneQ31));
}

} // namespace

Zone zoneOf(double knob) noexcept {
	// `computeZoneQ31`: the index is clamped to the last zone, and the offset inside it to one short of it.
	const int32_t value = knobQ31(knob);
	const int32_t index = std::min(value / kZoneWidthQ31, static_cast<int32_t>(kZones) - 1);
	const int32_t offset = std::min(value - index * kZoneWidthQ31, kZoneWidthQ31 - 1);
	const float position = static_cast<float>(offset * static_cast<int32_t>(kZones)) / static_cast<float>(kOneQ31);
	return { index, std::clamp(static_cast<double>(position), 0.0, 1.0) };
}

TopoParams computeTopoParams(double knob, double phRaw, double twistPhaseOffset) noexcept {
	const Zone z = zoneOf(knob);
	TopoParams r;
	r.zone = z.index;
	const auto pos = static_cast<float>(z.position);
	const double phRawDet = phRaw + twistPhaseOffset;

	const float fm0 = freqMod(pos, phRaw, phi::kPhi025);
	const float fm1 = freqMod(pos, phRaw, phi::kPhi050);
	const float fm2 = freqMod(pos, phRaw, phi::kPhi075);
	const float fmDet = freqMod(pos, phRawDet, phi::kPhi033);
	const float fmHarm = freqMod(pos, phRawDet, phi::kPhi067);
	const float fmEmph = freqMod(pos, phRawDet, phi::kPhi100);

	// Q — SHARED BY EVERY ZONE: 0.5 → 20 as the position crosses the zone, at phase zero.
	constexpr float kPhiN150 = 1.0f / phi::kPhi150;
	constexpr float kLog40 = 3.6888794541f;
	const float qTriangle = owlet::dsp::triangleSimpleUnipolar(phi::wrapPhase((pos + phRaw) * kPhiN150), 1.0f);
	r.q = 0.5f * std::exp(qTriangle * kLog40);

	float p0 = 0.0f;
	float p1 = 0.0f;
	float p2 = 0.0f;
	float lr = 0.0f;
	float det = 0.0f;
	float harm = 0.0f;
	float emph = 0.0f;
	switch (z.index) {
	case 0: // Cascade
		p0 = phiTri(pos, phRaw, phi::kPhi025, fm0, 0.1f);
		p1 = phiTri(pos, phRaw, phi::kPhi050, fm1, 0.3f);
		det = phiTri(pos, phRawDet, phi::kPhi033 * 0.5f, fmDet, 0.2f, 0.63f) * (1.0f - pos * 0.7f);
		harm = phiTri(pos, phRawDet, phi::kPhi067, fmHarm, 0.4f) * pos;
		emph = phiTriBi(pos, phRawDet, phi::kPhi100, fmEmph, 0.55f, 0.35f) * pos * 0.6f;
		break;
	case 1: // Ladder
		p0 = phiTri(pos, phRaw, phi::kPhi025, fm0, 0.1f, 0.8f);
		p1 = phiTri(pos, phRaw, phi::kPhi050, fm1, 0.3f, 0.7f);
		p2 = phiTri(pos, phRaw, phi::kPhi075, fm2, 0.6f, 0.6f);
		lr = p1 * 0.5f;
		det = phiTri(pos, phRawDet, phi::kPhi050 * 0.5f, fmDet, 0.3f, 0.54f);
		harm = phiTri(pos, phRawDet, phi::kPhi033, fmHarm, 0.5f, 0.5f);
		emph = phiTriBi(pos, phRawDet, phi::kPhi075, fmEmph, 0.4f, 0.6f) * 0.5f;
		break;
	case 2: // Owlpass
		p0 = pos * 5.0f;
		p1 = phiTri(pos, phRaw, phi::kPhi075, fm1, 0.5f, 0.6f);
		lr = pos * 0.4f;
		det = phiTri(pos, phRawDet, phi::kPhi025 * 0.5f, fmDet, 0.15f, 0.59f) * (0.3f + pos * 0.7f);
		harm = phiTri(pos, phRawDet, phi::kPhi075, fmHarm, 0.5f, 0.6f);
		emph = phiTriBi(pos, phRawDet, phi::kPhi050, fmEmph, 0.3f, 0.45f) * (0.2f + pos * 0.6f);
		break;
	case 3: // Cross
		p0 = phiTri(pos, phRaw, phi::kPhi050, fm0, 0.2f);
		p1 = phiTriBi(pos, phRaw, phi::kPhi075, fm1, 0.5f) * 0.5f + 0.5f;
		p2 = phiTri(pos, phRaw, phi::kPhi025, fm2, 0.8f);
		lr = (p1 - 0.5f) * 0.3f;
		det = std::abs(p1 - 0.5f) * phiTri(pos, phRawDet, phi::kPhi067 * 0.5f, fmDet, 0.4f, 0.5f);
		harm = phiTri(pos, phRawDet, phi::kPhi050, fmHarm, 0.2f) * 0.8f;
		emph = (p1 - 0.5f) * phiTriBi(pos, phRawDet, phi::kPhi033, fmEmph, 0.6f, 0.7f) * 0.7f;
		break;
	case 4: // Parallel
		p0 = phiTri(pos, phRaw, phi::kPhi050, fm0, 0.3f, 0.6f);
		p1 = phiTri(pos, phRaw, phi::kPhi075, fm1, 0.5f, 0.5f);
		p2 = phiTri(pos, phRaw, phi::kPhi025, fm2, 0.4f, 0.7f);
		lr = pos * 0.5f;
		det = phiTri(pos, phRawDet, phi::kPhi067 * 0.5f, fmDet, 0.5f, 0.55f);
		harm = phiTri(pos, phRawDet, phi::kPhi033, fmHarm, 0.4f, 0.6f);
		emph = phiTriBi(pos, phRawDet, phi::kPhi050, fmEmph, 0.4f, 0.5f) * 0.5f;
		break;
	case 5: // Nested
		p0 = phiTri(pos, phRaw, phi::kPhi033, fm0, 0.2f);
		p1 = phiTri(pos, phRaw, phi::kPhi075, fm1, 0.45f);
		p2 = phiTri(pos, phRaw, phi::kPhi050, fm2, 0.7f);
		lr = p2 * 0.2f;
		det = phiTri(pos, phRawDet, phi::kPhi033 * 0.5f, fmDet, 0.3f, 0.54f);
		harm = phiTri(pos, phRawDet, phi::kPhi067, fmHarm, 0.45f, 0.5f);
		emph = phiTriBi(pos, phRawDet, phi::kPhi100, fmEmph, 0.5f, 0.4f) * 0.55f;
		break;
	case 6: // Diffuse
		p0 = phiTri(pos, phRaw, phi::kPhi050, fm0, 0.15f);
		p1 = phiTri(pos, phRaw, phi::kPhi033, fm1, 0.4f);
		p2 = phiTri(pos, phRaw, phi::kPhi075, fm2, 0.65f);
		lr = p0 * 0.4f;
		det = phiTri(pos, phRawDet, phi::kPhi050 * 0.5f, fmDet, 0.4f, 0.63f);
		harm = phiTri(pos, phRawDet, phi::kPhi025, fmHarm, 0.5f, 0.6f);
		emph = phiTriBi(pos, phRawDet, phi::kPhi067, fmEmph, 0.35f, 0.65f) * 0.7f;
		break;
	case 7: // Spring
		p0 = phiTri(pos, phRaw, phi::kPhi025, fm0, 0.1f);
		p1 = phiTri(pos, phRaw, phi::kPhi050, fm1, 0.3f);
		p2 = phiTri(pos, phRaw, phi::kPhi075, fm2, 0.55f);
		lr = p2 * 0.15f;
		det = phiTri(pos, phRawDet, phi::kPhi075 * 0.5f, fmDet, 0.25f, 0.45f);
		harm = phiTri(pos, phRawDet, phi::kPhi033, fmHarm, 0.35f, 0.6f);
		emph = phiTriBi(pos, phRawDet, phi::kPhi050, fmEmph, 0.45f, 0.5f) * 0.45f;
		break;
	default:
		break;
	}
	r.param0 = p0;
	r.param1 = p1;
	r.param2 = p2;
	r.lrOffset = lr;
	r.detuning = det;
	r.harmonicBlend = harm;
	r.emphasis = emph;
	return r;
}

namespace {

/// The combined evolution — the meta zones, and every zone once the phase is off zero.
TwistParams combinedTwist(float pos, double phRaw) {
	TwistParams r;
	const float fmW = freqMod(pos, phRaw, phi::kPhi025);
	const float fmP = freqMod(pos, phRaw, phi::kPhi033);
	const float fmC = freqMod(pos, phRaw, phi::kPhi050);
	const float fmQ = freqMod(pos, phRaw, phi::kPhi067);
	const float fmD = freqMod(pos, phRaw, phi::kPhi075);

	const float wS = std::min(phiTri(pos, phRaw, phi::kPhi025, fmW, 0.166f, 0.8f) * 2.0f, 1.0f);
	const float wP = phiTri(pos, phRaw, phi::kPhi050, fmW, 0.984f, 0.7f);
	r.width = wS * wP;
	r.punch = phiTri(pos, phRaw, phi::kPhi033, fmP, 0.3f, 0.7f);
	const float curveRaw = phiTriBi(pos, phRaw, phi::kPhi050, fmC, 0.5f);
	r.spreadCurve = 0.5f + curveRaw * 0.5f;
	// "Chirp feedback evolves" — on the fifth fm, not the fourth: `fmD`, as the firmware has it.
	r.chirpAmount = phiTri(pos, phRaw, phi::kPhi067, fmD, 0.4f, 0.6f);
	r.qTilt = phiTriBi(pos, phRaw, phi::kPhiN025, fmQ, 0.7f) * 0.8f;
	r.phaseOffset = pos * 5.0f;
	const float fmLfo = freqMod(pos, phRaw, phi::kPhi100);
	r.lfoRateScale = 0.25f + phiTri(pos, phRaw, phi::kPhi150, fmLfo, 0.6f, 0.70f) * 1.75f;
	return r;
}

} // namespace

TwistParams computeTwistParams(double knob, double phRaw) noexcept {
	const int32_t value = knobQ31(knob);
	if (phRaw != 0.0) {
		return combinedTwist(std::clamp(static_cast<float>(value) / static_cast<float>(kOneQ31), 0.0f, 1.0f), phRaw);
	}
	const Zone z = zoneOf(knob);
	if (z.index < 5) {
		TwistParams r;
		const auto pos = static_cast<float>(z.position);
		switch (z.index) {
		case 0: r.width = pos; break;
		case 1: r.punch = pos; break;
		case 2: r.spreadCurve = pos; break;
		case 3: r.chirpAmount = pos; break;
		case 4: r.qTilt = pos < 0.5f ? pos * 2.0f : 3.0f - pos * 4.0f; break;
		default: break;
		}
		return r;
	}
	constexpr int32_t kZone5Start = kZoneWidthQ31 * 5;
	const float pos = static_cast<float>(value - kZone5Start) / static_cast<float>(kOneQ31 - kZone5Start);
	return combinedTwist(std::clamp(pos, 0.0f, 1.0f), phRaw);
}

Routing routingOf(const TopoParams& topo, const TwistParams& twist) noexcept {
	Routing r;
	auto lr = static_cast<float>(twist.width);
	auto spread = 1.0f;
	const auto p0 = static_cast<float>(topo.param0);
	const auto tlr = static_cast<float>(topo.lrOffset);
	switch (topo.zone) {
	case 0: spread = p0; break;
	case 1:
		spread = p0 * 0.7f;
		lr += tlr * 0.3f;
		break;
	case 2:
		spread = p0;
		lr += tlr;
		break;
	case 3: spread = p0 * 0.5f; break;
	case 4: spread = p0 * 0.3f; break;
	case 5: spread = 0.3f + p0 * 0.7f; break;
	case 6: spread = p0; break;
	case 7: spread = 0.2f + p0 * 0.5f; break;
	default: break;
	}
	r.spread = std::clamp(spread, 0.0f, 1.0f);
	r.lrOffset = lr;
	r.bimodalSeparation = topo.zone == 2 ? topo.param0 : 0.0;
	r.crossMix = topo.zone == 3 ? static_cast<double>(0.3f + p0 * 0.5f) : 0.0;
	return r;
}

double freqIndexOf(const Settings& s, bool haveNote, double noteSemitones) noexcept {
	const double perOctave = kFreqIndexMax / kFreqOctaves;
	if (!s.keyTrack) {
		return std::clamp(static_cast<double>(s.freqLog2) * perOctave, 0.0, kFreqIndexMax);
	}
	const auto offset = static_cast<double>(s.offsetOctaves);
	if (!haveNote) {
		// THE SAME KNOB, READ ABSOLUTELY — what the reference does with `noteCode = -1`.
		return std::clamp(kOffsetCentreIndex + offset * kOffsetCentreIndex / kOffsetOctavesPerHalf, 0.0, kFreqIndexMax);
	}
	// `noteCodeToHz(noteCode) * exp2f(offset)`, then `hzToDisperserFreq` without its `uint8_t`.
	const double hz = 440.0 * std::exp2((noteSemitones - 69.0) / 12.0 + offset);
	return std::clamp(std::log2(std::max(hz, 1.0)) * perOctave, 0.0, kFreqIndexMax);
}

double foldedDelaySamples(double samples, double sampleRate) noexcept {
	// IN THE FIRMWARE'S FLOAT AND ITS 44.1 kHz SAMPLES, and back: at 44.1 kHz the result is its result.
	const double toReference = kReferenceRate / sampleRate;
	constexpr float kMinDelayF = 441.0f;
	constexpr float kMaxDelayF = 8819.0f;
	constexpr auto kMaxOctaves = static_cast<float>(kFoldMaxOctaves);
	float delay = std::max(static_cast<float>(samples * toReference), 1.0f);
	float octavesFromMin = std::log2(delay / kMinDelayF);
	if (octavesFromMin < 0.0f || octavesFromMin > kMaxOctaves) {
		const float doubled = 2.0f * kMaxOctaves;
		octavesFromMin = std::fmod(std::fmod(octavesFromMin, doubled) + doubled, doubled);
		if (octavesFromMin > kMaxOctaves) {
			octavesFromMin = doubled - octavesFromMin;
		}
	}
	const float targetDelay = kMinDelayF * std::exp2(octavesFromMin);
	constexpr float kFifth = 0.5849625f;
	const float idealShift = std::log2(targetDelay / delay);
	const float quantizedShift = std::round(idealShift / kFifth) * kFifth;
	float folded = delay * std::exp2(quantizedShift);
	if (folded > kMaxDelayF) {
		folded *= 0.5f;
	}
	if (folded < kMinDelayF) {
		folded *= 2.0f;
	}
	return static_cast<double>(folded) / toReference;
}

AllpassCoeffs allpassCoeffs(double hz, double q, double sampleRate) noexcept {
	// THE ANGLE'S CLAMP AS HERTZ, and 1.4 rad at any rate — see `kAllpassLowHz`.
	const double lo = std::numbers::pi * kAllpassLowHz / sampleRate;
	const double hi = std::min(std::numbers::pi * kAllpassHighHz / sampleRate, kAllpassMaxAngle);
	const double w = std::clamp(std::numbers::pi * hz / sampleRate, lo, hi);
	const double k = std::tan(w);
	const double k2 = k * k;
	const double kQ = k / std::max(q, 0.1);
	const double norm = 1.0 / (1.0 + kQ + k2);
	const double a1 = 2.0 * (k2 - 1.0) * norm;
	const double a2 = (1.0 - kQ + k2) * norm;
	// **`a2` IS NOT CLAMPED — THE FIRMWARE'S 0.9999 CEILING MAKES THE STAGE UNSTABLE.** It is there so a2
	// fits a q31, and it clamps a2 without a1: near the 14 Hz angle floor at high Q the true a2 is above
	// 0.9999, and lowering it alone pushes 1 + a1 + a2 (which is 4k²/(1 + k/Q + k²), 3.4e-6 there at
	// 48 kHz) through zero — a pole ON or OUTSIDE the unit circle. MEASURED: the firmware's loop, single
	// write and unbiased, still at -25 to -40 dBFS six seconds after the input stopped in 160 of 288
	// settings; a first draft of this port holding a DC plateau near -70 dBFS for as long as it ran.
	// Unclamped, the stage is the exact allpass the formula describes, stable for every Q and angle.
	AllpassCoeffs c;
	c.a1Half = std::clamp(a1 * 0.5, -1.0, 0.9999);
	c.a2 = a2;
	// THE LATTICE'S REFLECTIONS, from sums that do not cancel: near the 14 Hz floor `a2` is 1 - 1e-6 and
	// `1 - a2*a2` taken directly would keep five digits of the answer. `1 - a2 = 2(k/Q)·norm` and
	// `1 + a2 = 2(1 + k²)·norm` exactly, and `k1`'s two sides are formed the same way.
	constexpr double kInside = 1.0 - 1e-15;
	const double onePlusA2 = 2.0 * (1.0 + k2) * norm;
	const double oneMinusA2 = 2.0 * kQ * norm;
	c.k2 = std::clamp(a2, -kInside, kInside);
	c.c2 = std::sqrt(std::max(0.0, oneMinusA2 * onePlusA2));
	c.k1 = std::clamp(2.0 * c.a1Half / onePlusA2, -kInside, kInside);
	c.c1 = std::sqrt(std::max(0.0, (1.0 - c.k1) * (1.0 + c.k1)));
	return c;
}

float centerHzOf(double freqIndex) noexcept {
	return static_cast<float>(std::exp2(freqIndex * kFreqOctaves / kFreqIndexMax));
}

float restingHalfSpread(double lrOffset) noexcept {
	return std::clamp(static_cast<float>(lrOffset) * 0.5f + static_cast<float>(kBaselineStereoOffset),
	                  -static_cast<float>(kMaxLrOffset), static_cast<float>(kMaxLrOffset));
}

StageLayout layoutStages(float centerHz, float spreadOctaves, float halfSpread, float q, int stages, float spreadCurve,
                         float qTilt, float bimodalSeparation, float detuning, float emphasis) noexcept {
	StageLayout out;
	const float lOffsetOct = -halfSpread;
	const float rOffsetOct = halfSpread;
	const float qBase = std::clamp(q, static_cast<float>(kQMin), static_cast<float>(kQMax));
	const size_t numActive = static_cast<size_t>(std::clamp(stages, 1, kMaxStages));
	const float curveClamped = std::clamp(spreadCurve, 0.0f, 1.0f);
	constexpr float kLog10 = 2.302585092994f;
	const float exponent = std::exp((1.0f - curveClamped * 2.0f) * kLog10);
	const float qTiltClamped = std::clamp(qTilt, -1.0f, 1.0f);

	const bool isBimodal = bimodalSeparation > 0.02f;
	float modeAHz = centerHz;
	float modeBHz = centerHz;
	if (isBimodal) {
		const float sepMult = std::exp2(bimodalSeparation * 0.5f);
		modeAHz = centerHz / sepMult;
		modeBHz = centerHz * sepMult;
	}
	const bool useDetuning = detuning > 0.001f;
	const float maxDetuneOct = useDetuning ? (50.0f * detuning / 1200.0f) : 0.0f;
	const float emphasisClamped = std::clamp(emphasis, -1.0f, 1.0f);

	out.count = static_cast<int>(numActive);
	for (size_t i = 0; i < numActive; ++i) {
		const float t = static_cast<float>(i) / std::max(1.0f, static_cast<float>(numActive - 1));
		float stageHzBase = 0.0f;
		if (isBimodal) {
			const bool isModeB = (i % 2) == 1;
			const float modeCenter = isModeB ? modeBHz : modeAHz;
			const size_t stagesInMode = (numActive + (isModeB ? 0 : 1)) / 2;
			const size_t indexInMode = i / 2;
			// `stagesInMode - 1` IS A `size_t` in the firmware; at zero it wraps huge and the max keeps it.
			const float tLocal = static_cast<float>(indexInMode) / std::max(1.0f, static_cast<float>(stagesInMode - 1));
			const float curved = tLocal > 0.001f ? std::exp(exponent * std::log(tLocal)) : 0.0f;
			const float localPosition = isModeB ? -curved : curved;
			stageHzBase = modeCenter * std::exp2(localPosition * spreadOctaves * 0.5f);
		}
		else {
			const float curved = t > 0.001f ? std::exp(exponent * std::log(t)) : 0.0f;
			const float stagePosition = curved * 2.0f - 1.0f;
			stageHzBase = centerHz * std::exp2(stagePosition * spreadOctaves);
		}
		const float qFactor = std::exp2(qTiltClamped * 2.0f * (t * 2.0f - 1.0f));
		const float detuneOct = useDetuning ? (((i & 1u) != 0u ? -1.0f : 1.0f) * maxDetuneOct) : 0.0f;
		out.hzBase[i] = stageHzBase;
		out.q[i] = std::clamp(qBase * qFactor, static_cast<float>(kQMin), static_cast<float>(kQMax));
		out.hzL[i] = std::clamp(stageHzBase * std::exp2(lOffsetOct + detuneOct), static_cast<float>(kStageHzMin),
		                        static_cast<float>(kStageHzMax));
		out.hzR[i] = std::clamp(stageHzBase * std::exp2(rOffsetOct + detuneOct), static_cast<float>(kStageHzMin),
		                        static_cast<float>(kStageHzMax));
		// THE SPECTRAL TILT, ±2.5 / 2 dB across the stages.
		float stageGain = 1.0f;
		if (std::abs(emphasisClamped) > 0.01f) {
			const float tiltDb = emphasisClamped > 0.0f ? emphasisClamped * 2.5f : emphasisClamped * 2.0f;
			const float stageDb = tiltDb * (t * 2.0f - 1.0f);
			constexpr float kLog10Over20 = 0.1151292546f;
			stageGain = std::exp(stageDb * kLog10Over20);
		}
		out.gainTarget[i] = std::clamp(stageGain, 0.6f, 1.7f);
	}
	return out;
}

//--------------------------------------------------------------------------------------------------//
// THE DEVICE.                                                                                        //
//--------------------------------------------------------------------------------------------------//

void Disperser::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	blockFrames_ = std::max(1, static_cast<int>(std::lround(kBlockSeconds * sampleRate_)));
	crossfadeFrames_ = std::max(1, static_cast<int>(std::lround(kWriteCrossfadeSeconds * sampleRate_)));
	minHarmonicQ16_ = static_cast<uint64_t>(std::lround(kHarmonicMinSeconds * sampleRate_)) << 16;
	restFrames_ = std::max(1, static_cast<int>(std::lround(kRestSeconds * sampleRate_)));
	riderRelease_ = static_cast<float>(std::exp(std::log(1e-3) / (kRiderReleaseSeconds * sampleRate_)));
	routeFadeFrames_ = std::max(1, static_cast<int>(std::lround(kRouteFadeSeconds * sampleRate_)));
	gainGlide_ = static_cast<float>(1.0 - std::exp(-1.0 / (kGainGlideSeconds * sampleRate_)));
	riderAttack_ = static_cast<float>(1.0 - std::exp(-1.0 / (kRiderAttackSeconds * sampleRate_)));
	riderGainRelease_ = static_cast<float>(1.0 - std::exp(-1.0 / (kRiderReleaseGainSeconds * sampleRate_)));
	// THE ONE ALLOCATION: as many samples as are 200 ms, PLUS TWO. The fold's longest delay is 8819/44100 s
	// and the fractional write lands on `offset` and `offset + 1`; at 200 ms exactly (the firmware's 8820)
	// that second cell is the HEAD itself whenever the rate makes the longest delay's whole part the line
	// length less one — at 22.05 kHz, 4409.5 samples in a 4410 line. The punch write comes BEFORE the head
	// is read, so the dry signal came straight back out in the same sample (a review found it). Two spare
	// cells keep `offset + 1` off the head at every rate; delays are head-relative, so nothing else moves.
	const auto length = static_cast<size_t>(std::lround(kLineSeconds * sampleRate_)) + 2;
	lineL_.assign(std::max<size_t>(length, 16), 0.0f);
	lineR_.assign(lineL_.size(), 0.0f);
	reset();
}

void Disperser::reset() noexcept {
	placed_ = false;
	untilTick_ = 0;
	activeStages_ = target_.stages;
	topology_ = Topology::Cascade;
	lastTopoZone_ = -1;
	smoothedTopo_ = smoothedTwist_ = smoothedFreq_ = smoothedSpread_ = 0.0;
	smoothedTopoLap_ = 0.0;
	smoothedQOffset_ = 0.0;
	for (auto& s : s1_) {
		s = { 0.0f, 0.0f };
	}
	for (auto& s : s2_) {
		s = { 0.0f, 0.0f };
	}
	coeffsL_.fill({});
	coeffsR_.fill({});
	for (auto& stage : lattice_) {
		stage.fill({});
	}
	latticeRemaining_ = 0;
	latticeCount_ = 0;
	latticeSnap_ = true;
	routeFadeRemaining_ = 0;
	punchNow_ = chirpFNow_ = chirp2FNow_ = readNow_ = 0.0f;
	for (auto& s : s1Old_) {
		s = { 0.0, 0.0 };
	}
	for (auto& s : s2Old_) {
		s = { 0.0, 0.0 };
	}
	for (auto& h : stageHz_) {
		h = { 0.0, 0.0 };
	}
	stageQ_.fill(0.0);
	stageOffsets_.fill(0);
	ladderCrossGains_.fill(0.0f);
	stageGains_.fill(1.0f);
	coeffUpdateCounter_ = kCoeffUpdateStride;
	lastFreq_ = lastSpread_ = -1.0;
	lastActiveStages_ = -1;
	lastLrOffset_ = 0.0f;
	lastQ_ = -1.0f;
	lastSpreadCurve_ = -1.0f;
	lastQTilt_ = 999.0f;
	lastBimodal_ = -1.0f;
	lastDetuning_ = -1.0f;
	lastEmphasis_ = 999.0f;
	widthLfoPhase_ = detuneLfoPhase_ = 0.0;
	widthLock_.release();
	detuneLock_.release();
	std::fill(lineL_.begin(), lineL_.end(), 0.0f);
	std::fill(lineR_.begin(), lineR_.end(), 0.0f);
	headPos_ = 0;
	activityCounter_ = 0;
	currentDelayQ16_ = prevDelayQ16_ = pendingDelayQ16_ = 0;
	crossfadeRemaining_ = 0;
	fastEnv_ = slowEnv_ = 0.0f;
	riderEnv_ = 0.0f;
	riderGain_ = 1.0f;
	fullPath_ = false;
	punch_ = chirp_ = 0.0f;
	crossGain_ = 0.4f;
	punchGain_ = chirpGainF_ = chirpGain2F_ = 0.0f;
	useHarmonic2F_ = false;
	block_ = {};
	linePins_ = 0;
	riderEngaged_ = 0;
	quietFrames_ = 0;
}

void Disperser::setTarget(const Settings& settings) noexcept { target_ = settings; }

void Disperser::setNoteContext(const device::NoteContext& note) noexcept {
	if (!std::isfinite(note.pitchSemitones)) {
		return;
	}
	if (note.gate) {
		heardNote_ = true;
	}
	if (heardNote_) {
		noteSemitones_ = static_cast<double>(note.pitchSemitones);
	}
}

void Disperser::processStereo(float* left, float* right, int frames) noexcept {
	if (left == nullptr || right == nullptr || frames <= 0 || lineL_.empty()) {
		return;
	}
	const Settings s = target_;
	if (!s.enabled) {
		return;
	}
	// A NON-FINITE INPUT SAMPLE IS TREATED AS 0 before anything sees it (as EQ and Comp do; `fx::scrubNonFinite`).
	fx::scrubNonFinite(left, right, frames);
	// THE BLOCK GRID, CARRIED ACROSS CALLS — the Eroder's `untilTick_`, and for its reason: once per
	// call, as the firmware runs once per buffer, would make the output a function of the host's
	// block size.
	int done = 0;
	while (done < frames) {
		if (untilTick_ <= 0) {
			tickOffset_ = done;
			tick(s, left[done], right[done]);
			untilTick_ = blockFrames_;
		}
		const int take = std::min(frames - done, untilTick_);
		renderChunk(left + done, right + done, take);
		untilTick_ -= take;
		done += take;
	}
	publishReadings();
}

void Disperser::publishReadings() noexcept {
	if (sink_.publish == nullptr) {
		return;
	}
	float values[static_cast<size_t>(Reading::kCount)] {};
	values[static_cast<size_t>(Reading::NoteHeard)] = heardNote_ ? 1.0f : 0.0f;
	values[static_cast<size_t>(Reading::NoteSemitones)] = static_cast<float>(noteSemitones_);
	values[static_cast<size_t>(Reading::PunchGain)] = punchGain_;
	values[static_cast<size_t>(Reading::ChirpGain)] = chirpGainF_;
	values[static_cast<size_t>(Reading::Chirp2FGain)] = chirpGain2F_;
	values[static_cast<size_t>(Reading::LoopGain)] = static_cast<float>(block_.loopGainRaw);
	values[static_cast<size_t>(Reading::EchoMs)] = static_cast<float>(block_.foldedDelaySamples / sampleRate_ * 1000.0);
	for (size_t i = 0; i < static_cast<size_t>(kMaxStages); ++i) {
		values[static_cast<size_t>(Reading::StageGain0) + i] = stageGains_[i];
	}
	sink_.publish(sink_.context, values, static_cast<size_t>(Reading::kCount));
}

void Disperser::tick(const Settings& s, float firstL, float firstR) noexcept {
	// `processDisperser`, top to bottom.
	const auto topoTarget = static_cast<double>(s.topo);
	const auto twistTarget = static_cast<double>(s.twist);
	const auto topoLapTarget = static_cast<double>(s.topoLap);
	const auto qOffsetTarget = static_cast<double>(s.qOffsetOctaves);
	const auto stages = std::clamp(s.stages, kMinStages, kMaxStages);

	// SMOOTH THE TWO COORDINATES. The firmware smooths the KNOB (its phase offset jumps); the
	// coordinate is the knob and the lap together here, so a turn across a lap boundary glides.
	// TOPO'S PHASE PAST ITS ZONE glides the same way, on its own: it is the offset alone, never the knob.
	if (!placed_) {
		smoothedTopo_ = topoTarget;
		smoothedTwist_ = twistTarget;
		smoothedTopoLap_ = topoLapTarget;
		smoothedQOffset_ = qOffsetTarget;
	}
	else {
		smoothedTopo_ += (topoTarget - smoothedTopo_) * kSmoothingAlpha;
		smoothedTwist_ += (twistTarget - smoothedTwist_) * kSmoothingAlpha;
		smoothedTopoLap_ += (topoLapTarget - smoothedTopoLap_) * kSmoothingAlpha;
		smoothedQOffset_ += (qOffsetTarget - smoothedQOffset_) * kSmoothingAlpha;
	}

	const double gammaTerm = kGammaScale * static_cast<double>(s.gamma);
	const Coordinate topoC = splitCoordinate(smoothedTopo_);
	const Coordinate twistC = splitCoordinate(smoothedTwist_);
	TwistParams twist = computeTwistParams(twistC.knob, twistC.lap + gammaTerm);
	TopoParams topo = computeTopoParams(topoC.knob, topoC.lap + smoothedTopoLap_ + gammaTerm, twist.phaseOffset);
	applyPills(s, topo, twist); // the pills (`Param::WidthAmount`): on at 0.5, the firmware's own; off, the extra disabled
	topo.q = qWithOffset(topo.q, smoothedQOffset_); // the Q offset (`Param::QOffset`), glided as the coordinates are

	// A ROUTING CHANGE. The firmware's "soft reset on zone boundary" keeps an eighth of every state (`>> 3`) and
	// rewires in one sample; the port crossfades the old routing into the new (`route`). The firmware road
	// (`setDf2tForTest`, which the transcription comparisons run) keeps the eighth.
	if (lastTopoZone_ != topo.zone && lastTopoZone_ >= 0) {
		if (df2tForTest_) {
			for (size_t i = 0; i < kMaxStages; ++i) {
				for (int c = 0; c < 2; ++c) {
					s1_[i][c] *= 0.125f;
					s2_[i][c] *= 0.125f;
				}
			}
		}
		else {
			s1Old_ = s1_;
			s2Old_ = s2_;
			for (auto& s : s1_) {
				s = { 0.0, 0.0 };
			}
			for (auto& s : s2_) {
				s = { 0.0, 0.0 };
			}
			oldTopology_ = topology_;
			routeFadeRemaining_ = routeFadeFrames_;
		}
	}
	lastTopoZone_ = topo.zone;
	topology_ = static_cast<Topology>(topo.zone);

	const Routing routing = routingOf(topo, twist);
	const double freqIndex = freqIndexOf(s, heardNote_, noteSemitones_);

	// `updateCoefficientsSmoothed`: the two smoothers every block, the coefficients every fourth.
	if (!placed_) {
		smoothedFreq_ = freqIndex;
		smoothedSpread_ = routing.spread;
		placed_ = true;
	}
	else {
		smoothedFreq_ += (freqIndex - smoothedFreq_) * kSmoothingAlpha;
		smoothedSpread_ += (routing.spread - smoothedSpread_) * kSmoothingAlpha;
	}
	// A STAGE LEAVING THE CHAIN LEAVES SILENT. Its states are out of the signal path from here, and without
	// this a Stages automation brought one back holding whatever it held when it left — a transient from
	// the past re-injected through today's coefficients.
	for (int i = std::max(stages, 0); i < activeStages_ && i < static_cast<int>(kMaxStages); ++i) {
		s1_[static_cast<size_t>(i)] = { 0.0, 0.0 };
		s2_[static_cast<size_t>(i)] = { 0.0, 0.0 };
		s1Old_[static_cast<size_t>(i)] = { 0.0, 0.0 };
		s2Old_[static_cast<size_t>(i)] = { 0.0, 0.0 };
	}
	activeStages_ = stages;
	if (++coeffUpdateCounter_ >= kCoeffUpdateStride) {
		coeffUpdateCounter_ = 0;
		updateCoefficients(smoothedFreq_, smoothedSpread_, routing.lrOffset, topo.q, stages, twist.spreadCurve,
		                   twist.qTilt, routing.bimodalSeparation, topo.detuning, topo.emphasis, twist.lfoRateScale);
	}

	// `processBuffer`'s prelude.
	punch_ = static_cast<float>(twist.punch);
	chirp_ = static_cast<float>(twist.chirpAmount);
	crossGain_ = 0.4f + static_cast<float>(routing.crossMix) * 0.5f;
	const size_t centerStage = static_cast<size_t>(stages) / 2;
	const double delaySamples = static_cast<double>(stageOffsets_[centerStage] * 2);

	block_.topo = topo;
	block_.twist = twist;
	block_.routing = routing;
	block_.transient = 0.0;
	block_.punchGain = block_.chirpGain2F = block_.chirpGainF = 0.0;

	fullPath_ = punch_ > 0.01f || chirp_ > 0.01f || hasEnergy();
	block_.fullPath = fullPath_;
	punchGain_ = chirpGainF_ = chirpGain2F_ = 0.0f;
	if (fullPath_) {
		const double folded = foldedDelaySamples(delaySamples, sampleRate_);
		block_.foldedDelaySamples = folded;
		const auto foldedF = static_cast<float>(folded);
		// THE DELAY'S TARGET, once per block; the snap-or-crossfade runs per sample in `processSample`,
		// where the firmware has it.
		pendingDelayQ16_ = static_cast<uint64_t>(foldedF * 65536.0f);

		// THE DETECTOR SEES THE BLOCK'S FIRST SAMPLE PLUS THE FEEDBACK AT THE HEAD — `buffer[0]`.
		const float fbAmount = std::max(punch_, chirp_);
		const float fbGain = fbAmount * 0.99f;
		const float detectL = sat(firstL / static_cast<float>(kHeadroom) + lineL_[headPos_] * fbGain);
		const float detectR = sat(firstR / static_cast<float>(kHeadroom) + lineR_[headPos_] * fbGain);
		const float transient = detectTransient(detectL, detectR, folded);
		block_.transient = transient;

		useHarmonic2F_ = topo.harmonicBlend > 0.01;
		if (punch_ > 0.01f) {
			punchGain_ = std::min(punch_ * (0.5f + transient * 0.49f), 0.99f);
			activityCounter_ = kActivityBlocks;
		}
		if (chirp_ > 0.01f) {
			const float writeAmount = std::min(chirp_ * (0.50f + transient * 0.50f), 0.99f);
			const auto hb = static_cast<float>(topo.harmonicBlend);
			chirpGainF_ = writeAmount * (1.0f - hb * 0.5f);
			chirpGain2F_ = writeAmount * (hb * 0.8f);
			activityCounter_ = kActivityBlocks;
		}
		block_.chirpGainFRaw = chirpGainF_;
		block_.chirpGain2FRaw = chirpGain2F_;
		// **THE CHIRP LOOP'S GAIN, CAPPED JUST UNDER UNITY — THE STRATEGIC LIMITER** ("you
		// may add a limiter somewhere strategic ... but try not to sacrifice long tails"). One round trip
		// multiplies by at most the doubled writes' sum, times the head's gain, times what the routing can
		// amplify (`routingBound`). Where that product passes `kChirpLoopCeiling` both writes are scaled to
		// meet it — `min(1, ceiling / gain)`, continuous in every input, so sweeping Chirp or the 2f blend
		// through the engage point steps no more than the firmware's own per-block gains do. It engages
		// only near the top of the Chirp zone; below it the firmware's (doubled, long) tails are untouched,
		// and above it the tail rings on without growing.
		//
		// **THE ROUTING'S FACTOR WAS FIRST TAKEN AS 1, AND THAT HOLDS ONLY IN SERIES.** A review MEASURED
		// Nested at topo 0.64 and Spring at -0.0203, Twist at the top of Chirp, 32 stages: +31 to +39 dBFS,
		// the line pinned 350k-750k times, still at that level ten seconds after the input stopped. The
		// emphasis tilt is balanced over the whole chain, not over a half-chain or a tap, so a parallel
		// half, a nested half or a spring tap can carry a product of gains well over 1 (routing magnitudes
		// 1.79, 2.33 and 1.34 there). Floored at 1, so every series routing — where the product is 1 — keeps
		// exactly the cap it had.
		const float loopGain =
		    2.0f * (chirpGainF_ + chirpGain2F_) * (std::max(punch_, chirp_) * 0.99f) * std::max(1.0f, routingBound());
		const float capScale = loopGain > kChirpLoopCeiling ? kChirpLoopCeiling / loopGain : 1.0f;
		chirpGainF_ *= capScale;
		chirpGain2F_ *= capScale;
		if (writesOff_) {
			punchGain_ = chirpGainF_ = chirpGain2F_ = 0.0f;
		}
		block_.loopGainRaw = loopGain;
		block_.punchGain = punchGain_;
		block_.chirpGainF = chirpGainF_;
		block_.chirpGain2F = chirpGain2F_;
	}
	// `tickActivity()` — at the end of the firmware's buffer; nothing reads it in between.
	if (activityCounter_ > 0) {
		--activityCounter_;
	}
}

void Disperser::updateCoefficients(double freqIndex, double spread, double lrOffset, double q, int activeStages,
                                   double spreadCurve, double qTilt, double bimodalSeparation, double detuning,
                                   double emphasis, double lfoRateScale) noexcept {
	const auto lrF = static_cast<float>(lrOffset);
	const auto qF = static_cast<float>(q);
	const auto curveF = static_cast<float>(spreadCurve);
	const auto tiltF = static_cast<float>(qTilt);
	const auto bimodalF = static_cast<float>(bimodalSeparation);
	const auto detuneF = static_cast<float>(detuning);
	const auto emphasisF = static_cast<float>(emphasis);

	// "Force recalc if any parameter changed". **THE FREQ AND SPREAD TESTS ARE ON THE FIRMWARE'S 7-BIT
	// GRID** while the values used below are continuous: when nothing is moving the recalculation
	// stops — and with it the two LFOs, which advance only here — exactly as the firmware's does once
	// its `uint8_t` indices settle.
	const double freqStep = std::floor(freqIndex);
	const double spreadStep = std::floor(spread * kFreqIndexMax);
	const bool lrChanged = std::abs(lrF - lastLrOffset_) > 0.01f;
	const bool qChanged = std::abs(qF - lastQ_) > 0.05f;
	const bool stagesChanged = activeStages != lastActiveStages_;
	const bool curveChanged = std::abs(curveF - lastSpreadCurve_) > 0.02f;
	const bool tiltChanged = std::abs(tiltF - lastQTilt_) > 0.02f;
	const bool bimodalChanged = std::abs(bimodalF - lastBimodal_) > 0.02f;
	const bool detuneChanged = std::abs(detuneF - lastDetuning_) > 0.01f;
	const bool emphasisChanged = std::abs(emphasisF - lastEmphasis_) > 0.02f;
	const bool detuneActive = detuneF > 0.001f;
	if (freqStep == lastFreq_ && spreadStep == lastSpread_ && !lrChanged && !qChanged && !stagesChanged
	    && !curveChanged && !tiltChanged && !bimodalChanged && !detuneChanged && !emphasisChanged && !detuneActive) {
		return;
	}
	++block_.coefficientUpdates;
	lastFreq_ = freqStep;
	lastSpread_ = spreadStep;
	lastLrOffset_ = lrF;
	lastQ_ = qF;
	lastActiveStages_ = activeStages;
	if (stagesChanged) {
		for (int i = 0; i < activeStages && i < kMaxStages; ++i) {
			ladderCrossGains_[static_cast<size_t>(i)] =
			    0.05f + 0.20f * static_cast<float>(i) / static_cast<float>(activeStages);
		}
	}
	lastSpreadCurve_ = curveF;
	lastQTilt_ = tiltF;
	lastBimodal_ = bimodalF;
	lastDetuning_ = detuneF;
	lastEmphasis_ = emphasisF;

	const float centerHz = centerHzOf(freqIndex);
	const auto spreadOctaves = static_cast<float>(spread * kSpreadOctaves);

	// THE TWO LFOs, advanced by one coefficient period — four blocks, as a TIME, and in the firmware's
	// float in the firmware's order (`kWidthLfoBaseRate * numSamples * kCoeffUpdateStride / 44100.0f`).
	const float lfoInc = static_cast<float>(kWidthLfoHz) * static_cast<float>(blockFrames_)
	                     * static_cast<float>(kCoeffUpdateStride) / static_cast<float>(sampleRate_);
	const float lfoStep = lfoFrozen_ ? 0.0f : lfoInc; // the test seam holds both at their centre
	// **ON A PLAYING TRANSPORT THE WOBBLES ARE THE SONG'S** (backlog NM 2026-10-18: determinism first): each
	// phase follows the song's seconds at its own rate (`TransportLock`), so pressing play anywhere, or reloading
	// mid-song, lands on the same wobble. Stopped, they free-run by the firmware's increment, as before.
	lastLfoRateScale_ = lfoRateScale;
	const double widthHz = kWidthLfoHz * lfoRateScale;
	if (!lfoFrozen_ && clock_.playing && clock_.havePositionSeconds() && widthHz > 0.0) {
		const double at = clock_.songPositionSeconds + static_cast<double>(tickOffset_) / sampleRate_;
		const double period = static_cast<double>(blockFrames_) * kCoeffUpdateStride / sampleRate_;
		const double width = widthLock_.follow(at, 1.0 / widthHz, period, 2);
		const double detune = detuneLock_.follow(at, 1.0 / (widthHz * kDetuneLfoRatio), period, 2);
		widthLfoPhase_ = static_cast<float>(width - std::floor(width));
		detuneLfoPhase_ = static_cast<float>(detune - std::floor(detune));
	}
	else {
		widthLock_.release();
		detuneLock_.release();
		widthLfoPhase_ += lfoStep * static_cast<float>(lfoRateScale);
		if (widthLfoPhase_ >= 1.0f) {
			widthLfoPhase_ -= 1.0f;
		}
		detuneLfoPhase_ += lfoStep * static_cast<float>(kDetuneLfoRatio) * static_cast<float>(lfoRateScale);
		if (detuneLfoPhase_ >= 1.0f) {
			detuneLfoPhase_ -= 1.0f;
		}
	}
	const float widthLfoOffset = lfoTriangle(widthLfoPhase_) * static_cast<float>(kWidthLfoDepth);
	const float detuneLfoOffset = lfoTriangle(detuneLfoPhase_) * static_cast<float>(kDetuneLfoDepth);
	const float halfSpread =
	    std::clamp(lrF * 0.5f + widthLfoOffset + detuneLfoOffset + static_cast<float>(kBaselineStereoOffset),
	               -static_cast<float>(kMaxLrOffset), static_cast<float>(kMaxLrOffset));
	const StageLayout layout = layoutStages(centerHz, spreadOctaves, halfSpread, qF, activeStages, curveF, tiltF, bimodalF,
	                                        detuneF, emphasisF);
	// **THE LATTICE RAMPS TO EACH UPDATE, IT DOES NOT STEP.** The firmware steps its coefficients every fourth
	// block, and the stereo LFOs move them at every step: MEASURED on a sine, that stepping was the largest thing
	// in the output that is not the tone (see `advanceLattice`). Each section's (c, k) now travels in a straight
	// line to the new target across the interval before the next update — and a straight line between two unit
	// vectors never leaves the unit disc, so the section stays lossless-or-better the whole way.
	const int rampFrames = std::max(1, blockFrames_ * kCoeffUpdateStride);
	const bool ramp = latticeRampForTest_ && !latticeSnap_;
	for (size_t i = 0; i < static_cast<size_t>(layout.count); ++i) {
		coeffsL_[i] = allpassCoeffs(layout.hzL[i], layout.q[i], sampleRate_);
		coeffsR_[i] = allpassCoeffs(layout.hzR[i], layout.q[i], sampleRate_);
		for (int ch = 0; ch < 2; ++ch) {
			const AllpassCoeffs& c = ch == 0 ? coeffsL_[i] : coeffsR_[i];
			Lattice& now = lattice_[i][static_cast<size_t>(ch)];
			Lattice& to = latticeTarget_[i][static_cast<size_t>(ch)];
			Lattice& step = latticeStep_[i][static_cast<size_t>(ch)];
			to = { c.k1, c.c1, c.k2, c.c2 };
			// A STAGE ARRIVING (Stages grew) SNAPS: it has no last value to travel from.
			if (!ramp || static_cast<int>(i) >= latticeCount_) {
				now = to;
				step = { 0.0, 0.0, 0.0, 0.0 }; // NOT `{}`: a Lattice defaults to c = 1, which as a step is +1 a sample
				continue;
			}
			const double inv = 1.0 / static_cast<double>(rampFrames);
			step = { (to.k1 - now.k1) * inv, (to.c1 - now.c1) * inv, (to.k2 - now.k2) * inv, (to.c2 - now.c2) * inv };
		}
		stageHz_[i] = { layout.hzL[i], layout.hzR[i] };
		stageQ_[i] = layout.q[i];

		// THE ECHO'S DELAY FROM THE UNDETUNED STAGE, whole samples at the running rate.
		// IN FLOAT, AS `kSampleRate / stageHzBase` IS: the offset is TRUNCATED to a whole sample next, so a
		// double quotient a hair above an integer landed one sample off the firmware's (MEASURED: 7657
		// against 7656, and the echoes parted from the reference at the first return).
		const auto rawOffset = static_cast<double>(static_cast<float>(sampleRate_) / layout.hzBase[i]);
		const auto offset = static_cast<size_t>(foldedDelaySamples(rawOffset, sampleRate_));
		stageOffsets_[i] = std::clamp(offset, size_t { 1 }, lineL_.size() - 1);

		// THE SPECTRAL TILT, gliding a sixteenth of the way to its target per update.
		stageGains_[i] += (layout.gainTarget[i] - stageGains_[i]) * (1.0f / 16.0f);
	}
	block_.freqIndex = freqIndex;
	block_.spreadIndex = spread;
	block_.centerHz = centerHz;
	block_.halfSpreadOctaves = halfSpread;
	latticeRemaining_ = ramp ? rampFrames : 0;
	latticeCount_ = layout.count;
	latticeSnap_ = false;
}

void Disperser::advanceLattice() noexcept {
	// **MEASURED, the reason this exists:** a -12 dBFS sine through every Topo × Twist mode at three positions
	// (the survey in the port doc, §4.12). With the coefficients stepped every fourth block as the firmware
	// steps them, the output held the stereo LFOs' stair as sidebands; frozen LFOs took the residual from
	// -82 dB to -103 dB on average, and the ramp takes most of that difference back without freezing anything.
	if (latticeRemaining_ <= 0) {
		return;
	}
	const auto n = static_cast<size_t>(std::clamp(latticeCount_, 0, kMaxStages));
	if (--latticeRemaining_ == 0) {
		for (size_t i = 0; i < n; ++i) {
			lattice_[i] = latticeTarget_[i]; // home exactly, no drift from summing steps
		}
		return;
	}
	for (size_t i = 0; i < n; ++i) {
		for (size_t ch = 0; ch < 2; ++ch) {
			Lattice& now = lattice_[i][ch];
			const Lattice& step = latticeStep_[i][ch];
			now.k1 += step.k1;
			now.c1 += step.c1;
			now.k2 += step.k2;
			now.c2 += step.c2;
		}
	}
}

bool Disperser::hasEnergy() const noexcept {
	if (activityCounter_ > 0) {
		return true;
	}
	// **EXACT, NOT THE FIRMWARE'S SPARSE SAMPLE.** The firmware looks at sixteen cells against a threshold
	// 54 dB under full scale (`kEnergyThreshold`) and, when it sees nothing, takes the fast path, where the
	// head does not move — so an echo quieter than that, or between the sixteen, is FROZEN in the line
	// and replays whenever the delay road next runs. A gate may skip work, never sound: here the fast
	// path runs only when every cell is exactly zero, and then the two paths' outputs are identical. It
	// is asked only once the activity counter has run out with Punch and Chirp both off.
	for (size_t i = 0; i < lineL_.size(); ++i) {
		if (lineL_[i] != 0.0f || lineR_[i] != 0.0f) {
			return true;
		}
	}
	return false;
}

bool Disperser::holdsTail() const noexcept {
	// THE WRITE-AHEAD LINE IS WHAT IS STILL TO COME: Punch and Chirp write ahead of the head and the head clears what
	// it reads, so a non-zero cell is an echo not yet played. Scanned exactly, as `hasEnergy` does; the host asks only
	// after an exactly silent block, so this is never paid while the plugin sounds. Off, the head does not move.
	if (!target_.enabled) {
		return false;
	}
	for (size_t i = 0; i < lineL_.size(); ++i) {
		if (lineL_[i] != 0.0f || lineR_[i] != 0.0f) {
			return true;
		}
	}
	return false;
}

float Disperser::detectTransient(float inL, float inR, double delaySamples) noexcept {
	const float absIn = std::abs(inL) * 0.5f + std::abs(inR) * 0.5f;
	// **HALF OF WHAT IT SAYS, BOTH TIMES**: `multiply_32x32_rshift32(env, 0.998)` with no `<< 1` is
	// env × 0.499, and the slow follower's step is half its coefficient's. Kept — see the port doc.
	if (absIn > fastEnv_) {
		fastEnv_ = absIn;
	}
	else {
		fastEnv_ *= 0.998f * 0.5f;
	}
	const double toReference = kReferenceRate / sampleRate_;
	const double delay44 = std::clamp(std::floor(delaySamples) * toReference, 441.0, 8819.0);
	const auto delayNorm = static_cast<float>((delay44 - 441.0) / (8819.0 - 441.0));
	const float slowCoeff = 0.999f + delayNorm * 0.0009f;
	slowEnv_ += (absIn - slowEnv_) * (1.0f - slowCoeff) * 0.5f;
	const float diff = fastEnv_ - slowEnv_;
	if (diff <= 0.0f) {
		return 0.0f;
	}
	return std::clamp(diff, 0.0f, 1.0f);
}

void Disperser::runStage(size_t i, float& l, float& r) noexcept {
	const AllpassCoeffs& cl = coeffsL_[i];
	const AllpassCoeffs& cr = coeffsR_[i];
	// **A NORMALIZED LATTICE, NOT THE FIRMWARE'S DIRECT FORM II TRANSPOSED — THE SAME ALLPASS, HELD STILL.**
	// A review MEASURED the DF2T form pumping energy when its coefficients move: a Stages automation or a
	// fast Freq route re-reads states that hold hundreds of times the signal (a stage at the 14 Hz floor has
	// its poles 0.001 from the circle) through NEW coefficients, and the chain alone threw +24 dBFS spikes
	// from a −10 dBFS input; with the chirp loop round it, the line pinned at +42 dBFS and stayed there. Each
	// section here is a rotation, `[f; b] = [c, -k; k, c]·[in; state]` with c² + k² = 1, so the output and the
	// new states carry exactly the energy of the input and the old states, WHATEVER the coefficients do
	// between samples: no modulation can pump it. Its static transfer function is the DF2T's (asserted
	// against it, and against the q31 transcription, in `test_fx_disperser`). The output saturates where
	// the firmware's does; the states need no clamp, since they cannot outgrow what went in.
	auto lattice = [](float xf, const Lattice& c, double& d1, double& d2) {
		const auto x = static_cast<double>(xf);
		const double f1 = c.c2 * x - c.k2 * d2;
		const double y = c.k2 * x + c.c2 * d2;
		const double f0 = c.c1 * f1 - c.k1 * d1;
		const double b1 = c.k1 * f1 + c.c1 * d1;
		d2 = std::abs(b1) < kFlushD ? 0.0 : b1;
		d1 = std::abs(f0) < kFlushD ? 0.0 : f0;
		return static_cast<float>(satD(y));
	};
	if (!df2tForTest_) {
		l = lattice(l, lattice_[i][0], (runOld_ ? s1Old_ : s1_)[i][0], (runOld_ ? s2Old_ : s2_)[i][0]);
		r = lattice(r, lattice_[i][1], (runOld_ ? s1Old_ : s1_)[i][1], (runOld_ ? s2Old_ : s2_)[i][1]);
		// THE EMPHASIS GAIN: a halving multiply, then a WRAPPING doubling.
		const float g = stageGains_[i];
		l = wrap(l * g);
		r = wrap(r * g);
		return;
	}
	// `StereoBiquadAllpass::processLR` — Direct Form II Transposed, every sum saturating. KEPT ONLY AS THE
	// REFERENCE the lattice is held to (`setDf2tForTest`), never on the audio road.
	auto one = [](float xf, const AllpassCoeffs& c, double& s1, double& s2) {
		const auto x = static_cast<double>(xf);
		const double y = satD(c.a2 * x + s1);
		const double diff = satD(x - y);
		const double half = c.a1Half * diff;
		s1 = satD(satD(half + half) + s2);
		s2 = satD(x - c.a2 * y);
		if (std::abs(s1) < kFlushD) {
			s1 = 0.0;
		}
		if (std::abs(s2) < kFlushD) {
			s2 = 0.0;
		}
		return static_cast<float>(y);
	};
	l = one(l, cl, (runOld_ ? s1Old_ : s1_)[i][0], (runOld_ ? s2Old_ : s2_)[i][0]);
	r = one(r, cr, (runOld_ ? s1Old_ : s1_)[i][1], (runOld_ ? s2Old_ : s2_)[i][1]);
	// THE EMPHASIS GAIN: a halving multiply, then a WRAPPING doubling.
	const float g = stageGains_[i];
	l = wrap(l * g);
	r = wrap(r * g);
}

float Disperser::routingBound() const noexcept {
	// AN H-INFINITY BOUND, BY THE TRIANGLE INEQUALITY: a stage is an allpass (magnitude exactly 1 at every
	// frequency) times its emphasis gain, so a chain of stages is bounded by the product of its gains, and a
	// blend by the sum of its weights' magnitudes times what each input can carry. Ladder's and Cross's
	// channel blends and every final L/R blend are convex (weights summing to 1), so they add nothing.
	const auto n = static_cast<size_t>(std::clamp(activeStages_, 1, kMaxStages));
	const auto product = [this](size_t from, size_t to) {
		float p = 1.0f;
		for (size_t i = from; i < to; ++i) {
			p *= std::abs(stageGains_[i]);
		}
		return p;
	};
	switch (topology_) {
	case Topology::Parallel: {
		const size_t half = std::max<size_t>(n / 2, 1);
		return 0.5f * product(0, half) + 0.5f * product(half, n);
	}
	case Topology::Nested: {
		const size_t half = std::max<size_t>(n / 2, 1);
		return (0.5f + 0.5f * product(0, half)) * product(half, n) * (kNested80 + kNested20);
	}
	case Topology::Spring: {
		size_t tap1 = n / 3;
		size_t tap2 = (n * 2) / 3;
		if (tap1 == 0) {
			tap1 = 1;
		}
		if (tap2 <= tap1) {
			tap2 = tap1 + 1;
		}
		// A tap past the last stage never fires in `route` and holds 0.
		const float t1 = tap1 < n ? product(0, tap1 + 1) : 0.0f;
		const float t2 = tap2 < n ? product(0, tap2 + 1) : 0.0f;
		return (kSpring70 * product(0, n) + kSpring15 * t1 + kSpring15 * t2) * (kSpring875 + kSpring125);
	}
	default: return product(0, n);
	}
}

void Disperser::route(float inL, float inR, float& outL, float& outR) noexcept {
	if (routeFadeRemaining_ <= 0) {
		routeOnce(topology_, inL, inR, outL, outR);
		return;
	}
	// **A ROUTING CHANGE IS A CROSSFADE, NOT A CUT (bad sounding artifacts).** The firmware
	// rewires in one sample and keeps an eighth of every state (`>> 3`): MEASURED, stepping the Topo mode on a
	// 110 Hz tone put a corner 3,700x an ideal sine's into the output (the transcription 4,200x). Here the old
	// routing runs on beside the new for `kRouteFadeSeconds`, on its own states (moved out at the change), the
	// new starting clean, and the two are faded — equal gain, since they carry the same signal.
	float newL = 0.0f;
	float newR = 0.0f;
	routeOnce(topology_, inL, inR, newL, newR);
	float oldL = 0.0f;
	float oldR = 0.0f;
	runOld_ = true;
	routeOnce(oldTopology_, inL, inR, oldL, oldR);
	runOld_ = false;
	const float g = 1.0f - static_cast<float>(routeFadeRemaining_) / static_cast<float>(routeFadeFrames_);
	outL = oldL + (newL - oldL) * g;
	outR = oldR + (newR - oldR) * g;
	--routeFadeRemaining_;
}

void Disperser::routeOnce(Topology t, float inL, float inR, float& outL, float& outR) noexcept {
	const auto n = static_cast<size_t>(std::clamp(activeStages_, 1, kMaxStages));
	float l = inL;
	float r = inR;
	switch (t) {
	case Topology::Ladder:
		for (size_t i = 0; i < n; ++i) {
			const float crossG = ladderCrossGains_[i];
			const float keepG = 1.0f - crossG;
			const float bl = sat(l * keepG + r * crossG);
			const float br = sat(r * keepG + l * crossG);
			l = bl;
			r = br;
			runStage(i, l, r);
		}
		break;
	case Topology::Cross:
		for (size_t i = 0; i < n; ++i) {
			runStage(i, l, r);
			if ((i & 3u) == 3u) {
				const float keep = 1.0f - crossGain_;
				const float nl = sat(l * keep + r * crossGain_);
				const float nr = sat(r * keep + l * crossGain_);
				l = nl;
				r = nr;
			}
		}
		break;
	case Topology::Parallel: {
		const size_t half = std::max<size_t>(n / 2, 1);
		float al = inL;
		float ar = inR;
		for (size_t i = 0; i < half; ++i) {
			runStage(i, al, ar);
		}
		float bl = inL;
		float br = inR;
		for (size_t i = half; i < n; ++i) {
			runStage(i, bl, br);
		}
		l = sat(al * 0.5f + bl * 0.5f);
		r = sat(ar * 0.5f + br * 0.5f);
		break;
	}
	case Topology::Nested: {
		const size_t half = std::max<size_t>(n / 2, 1);
		float il = inL;
		float ir = inR;
		for (size_t i = 0; i < half; ++i) {
			runStage(i, il, ir);
		}
		float ol = sat(inL * 0.5f + il * 0.5f);
		float orr = sat(inR * 0.5f + ir * 0.5f);
		for (size_t i = half; i < n; ++i) {
			runStage(i, ol, orr);
		}
		l = sat(ol * kNested80 + orr * kNested20);
		r = sat(orr * kNested80 + ol * kNested20);
		break;
	}
	case Topology::Spring: {
		size_t tap1 = n / 3;
		size_t tap2 = (n * 2) / 3;
		if (tap1 == 0) {
			tap1 = 1;
		}
		if (tap2 <= tap1) {
			tap2 = tap1 + 1;
		}
		float t1l = 0.0f;
		float t1r = 0.0f;
		float t2l = 0.0f;
		float t2r = 0.0f;
		for (size_t i = 0; i < n; ++i) {
			runStage(i, l, r);
			if (i == tap1) {
				t1l = l;
				t1r = r;
			}
			else if (i == tap2) {
				t2l = l;
				t2r = r;
			}
		}
		const float bl = sat(sat(l * kSpring70 + t1l * kSpring15) + t2l * kSpring15);
		const float br = sat(sat(r * kSpring70 + t1r * kSpring15) + t2r * kSpring15);
		l = sat(bl * kSpring875 + br * kSpring125);
		r = sat(br * kSpring875 + bl * kSpring125);
		break;
	}
	default: // Cascade, Owlpass, Diffuse
		for (size_t i = 0; i < n; ++i) {
			runStage(i, l, r);
		}
		break;
	}
	outL = l;
	outR = r;
}

void Disperser::writeFractional(float inL, float inR, uint64_t offsetQ16) noexcept {
	// `writeAtOffsetFractionalQ16`, **DOUBLED, AS THE FIRMWARE'S IS — AND THAT IS DELIBERATE.** `vqrdmulh` is
	// already a full q31 multiply and the `vshl_n_s32(..., 1)` after it doubles every write (copied from
	// the stage-gain idiom, where the gain IS stored halved). It is also what makes the firmware's chirp
	// tails long: a single write was tried and REJECTED, MEASURED at ~0.7x the firmware's -60 dB time at
	// Chirp 0.5, ~0.5x at 0.7 and 0.2-0.4x at 0.85 (port doc §4.2). The runaway the doubling causes past
	// Chirp ~0.9 is stopped instead by the loop-gain cap in `tick` (`kChirpLoopCeiling`).
	const size_t n = lineL_.size();
	const size_t offsetInt = offsetQ16 >> 16;
	const float gain1 = static_cast<float>(offsetQ16 & 0xFFFFu) / 65536.0f;
	const float gain0 = 1.0f - gain1;
	const size_t pos0 = (headPos_ + offsetInt) % n;
	const size_t pos1 = (headPos_ + offsetInt + 1) % n;
	const auto put = [this](float& cell, float add) {
		const float summed = sat(cell + add);
		const float pinned = sat30(summed);
		if (pinned != summed) {
			++linePins_;
		}
		cell = pinned;
	};
	put(lineL_[pos0], wrap(2.0f * (inL * gain0)));
	put(lineL_[pos1], wrap(2.0f * (inL * gain1)));
	put(lineR_[pos0], wrap(2.0f * (inR * gain0)));
	put(lineR_[pos1], wrap(2.0f * (inR * gain1)));
}

void Disperser::processSample(float inL, float inR, float& outL, float& outR) noexcept {
	// "Delay crossfade: snap to new delay time, crossfade writes to avoid pitch artifacts."
	const uint64_t newDelayQ16 = pendingDelayQ16_;
	if (currentDelayQ16_ == 0) {
		currentDelayQ16_ = newDelayQ16;
	}
	else if (newDelayQ16 != currentDelayQ16_ && crossfadeRemaining_ == 0) {
		const uint64_t curInt = currentDelayQ16_ >> 16;
		const uint64_t newInt = newDelayQ16 >> 16;
		const uint64_t diff = newInt > curInt ? newInt - curInt : curInt - newInt;
		// THE FIRMWARE SNAPS A CHANGE UNDER A TENTH AND CROSSFADES ONLY A LARGER ONE; the port crossfades every
		// whole-sample change — a snapped write position is a step in the echo stream (MEASURED with the gains
		// below: turning Twist's phase put corners of 5,200x an ideal sine's into the output, 2,000x with this
		// alone). The firmware road keeps the tenth.
		if (diff > (df2tForTest_ ? curInt / 10 : 0)) {
			prevDelayQ16_ = currentDelayQ16_;
			currentDelayQ16_ = newDelayQ16;
			crossfadeRemaining_ = crossfadeFrames_;
		}
		else {
			currentDelayQ16_ = newDelayQ16;
		}
	}
	const float fadeNew = 1.0f - static_cast<float>(crossfadeRemaining_) / static_cast<float>(crossfadeFrames_);
	const float fadeOld = 1.0f - fadeNew;

	// **THE DELAY ROAD'S GAINS GLIDE, THEY DO NOT STEP.** The firmware sets the punch and chirp write gains, and
	// the head's feedback gain, once a block — and when Punch and Chirp both fall under 0.01 the head is read
	// back WHOLE ("drain mode"), a step from 1% to 100% in one sample. MEASURED while turning Twist's phase:
	// the worst corners in the output sat exactly at those steps. Each gain here is a one-pole of
	// `kGainGlideSeconds` toward the block's value; the firmware road (`setDf2tForTest`) keeps the steps.
	const float fbAmount = std::max(punch_, chirp_);
	if (df2tForTest_) {
		punchNow_ = punchGain_;
		chirpFNow_ = chirpGainF_;
		chirp2FNow_ = chirpGain2F_;
	}
	else {
		punchNow_ += (punchGain_ - punchNow_) * gainGlide_;
		chirpFNow_ += (chirpGainF_ - chirpFNow_) * gainGlide_;
		chirp2FNow_ += (chirpGain2F_ - chirp2FNow_) * gainGlide_;
	}

	// PUNCH: the dry, scaled, written ahead.
	float procL = inL;
	float procR = inR;
	if (punchNow_ != 0.0f) {
		const float wl = inL * punchNow_;
		const float wr = inR * punchNow_;
		if (crossfadeRemaining_ > 0) {
			writeFractional(wl * fadeNew, wr * fadeNew, currentDelayQ16_);
			writeFractional(wl * fadeOld, wr * fadeOld, prevDelayQ16_);
		}
		else {
			writeFractional(wl, wr, currentDelayQ16_);
		}
	}

	// THE HEAD: read, cleared, BEFORE the allpasses so echoes are dispersed too.
	float fbL = lineL_[headPos_];
	float fbR = lineR_[headPos_];
	if (std::abs(fbL) < kFlush) {
		fbL = 0.0f;
	}
	if (std::abs(fbR) < kFlush) {
		fbR = 0.0f;
	}
	lineL_[headPos_] = 0.0f;
	lineR_[headPos_] = 0.0f;

	// **THE SAFETY NET: A GAIN RIDER ON THE LOOP, KEYED ON WHAT COMES BACK FROM THE LINE.** The cap bounds the
	// loop while the settings are held and the lattice keeps a moving chain from pumping; this is for what
	// neither foresaw. A peak envelope of the head (instant attack, `kRiderReleaseSeconds` to -60 dB) and, above `kRiderLevel`
	// — +24.1 dBFS, 6 dB under where the line pins — the feedback scaled by `kRiderLevel / envelope`: a
	// smooth gain, never a clip, and exactly 1 below the threshold, so at every normal level it does nothing
	// (the suite counts its engaged samples on every static case and asserts zero). The firmware had one
	// (`fbEnvelope_`, `fbLimitFactor`) and ships it disabled at 1.0, so there was nothing to port.
	riderEnv_ = std::max({ std::abs(fbL), std::abs(fbR), riderEnv_ * riderRelease_ });
	float target = 1.0f;
	if (riderEnv_ > kRiderLevel && !riderOff_) {
		// THE FOURTH POWER: 1 at the threshold, 1/16 where the line pins. MEASURED on the old DF2T form (the
		// case this net exists for, a chain that makes energy of its own): a rider of `level / envelope`
		// halved the loop at the pin and the line still pinned 17k-24k times, since the pumping chain carried
		// more than twice what went round.
		const float ratio = kRiderLevel / riderEnv_;
		target = (ratio * ratio) * (ratio * ratio);
	}
	// **THE GAIN IS SMOOTHED, NOT THE ENVELOPE'S STEP.** The envelope's attack is instant, so its target
	// steps sample by sample as the head's peaks arrive; applied directly it shaped the loop's waveform
	// (a review MEASURED the click proxy, max |2nd difference| / peak, at 0.0134 against 0.0008 without
	// the rider on a 110 Hz sine at +30 dBFS). A one-pole on the GAIN — `kRiderAttackSeconds` down,
	// `kRiderReleaseGainSeconds` up — turns those steps into ramps, and the line still does not pin.
	riderGain_ += (target - riderGain_) * (target < riderGain_ ? riderAttack_ : riderGainRelease_);
	if (target == 1.0f && riderGain_ > 1.0f - 1e-6f) {
		riderGain_ = 1.0f; // home, exactly: a one-pole otherwise sits a float step short of it for good
	}
	if (riderGain_ < 1.0f) {
		++riderEngaged_;
	}
	// THE HEAD'S GAIN: the feedback's while there is any, whole ("drain mode": what is left comes out and is not
	// written back) when there is not — gliding between them, as above.
	const float readTarget = fbAmount > 0.01f ? fbAmount * 0.99f : 1.0f; // the rider acts on top, unglided
	readNow_ = df2tForTest_ ? readTarget : readNow_ + (readTarget - readNow_) * gainGlide_;
	if (fbAmount > 0.01f) {
		procL = sat(procL + sat30(fbL * readNow_ * riderGain_));
		procR = sat(procR + sat30(fbR * readNow_ * riderGain_));
	}
	else if (fbL != 0.0f || fbR != 0.0f) {
		procL = sat(procL + fbL * readNow_);
		procR = sat(procR + fbR * readNow_);
	}

	route(procL, procR, outL, outR);

	// CHIRP: the OUTPUT written ahead at f, and at 2f (half the delay) when the blend asks.
	if (chirpFNow_ != 0.0f || chirp2FNow_ != 0.0f) {
		const float wlf = outL * chirpFNow_;
		const float wrf = outR * chirpFNow_;
		if (crossfadeRemaining_ > 0) {
			writeFractional(wlf * fadeNew, wrf * fadeNew, currentDelayQ16_);
			writeFractional(wlf * fadeOld, wrf * fadeOld, prevDelayQ16_);
			if (useHarmonic2F_ || chirp2FNow_ != 0.0f) {
				const uint64_t new2F = std::max(currentDelayQ16_ >> 1, minHarmonicQ16_);
				const uint64_t old2F = std::max(prevDelayQ16_ >> 1, minHarmonicQ16_);
				const float wl2 = outL * chirp2FNow_;
				const float wr2 = outR * chirp2FNow_;
				writeFractional(wl2 * fadeNew, wr2 * fadeNew, new2F);
				writeFractional(wl2 * fadeOld, wr2 * fadeOld, old2F);
			}
			--crossfadeRemaining_;
		}
		else {
			writeFractional(wlf, wrf, currentDelayQ16_);
			if (useHarmonic2F_ || chirp2FNow_ != 0.0f) {
				const uint64_t d2F = std::max(currentDelayQ16_ >> 1, minHarmonicQ16_);
				writeFractional(outL * chirp2FNow_, outR * chirp2FNow_, d2F);
			}
		}
	}
	else if (crossfadeRemaining_ > 0) {
		--crossfadeRemaining_;
	}

	headPos_ = (headPos_ + 1) % lineL_.size();
}

void Disperser::renderChunk(float* left, float* right, int frames) noexcept {
	const float in = 1.0f / static_cast<float>(kHeadroom);
	const auto out = static_cast<float>(kHeadroom);
	for (int i = 0; i < frames; ++i) {
		advanceLattice();
		const float inL = left[i] * in;
		const float inR = right[i] * in;
		float outL = 0.0f;
		float outR = 0.0f;
		if (fullPath_) {
			processSample(inL, inR, outL, outR);
		}
		else {
			// THE FAST PATH: no punch, no chirp, nothing in the line — the allpasses alone, and the
			// head does not move (the firmware's does not either).
			route(inL, inR, outL, outR);
		}
		const float wetL = outL * out;
		const float wetR = outR * out;
		// THE REST: see `kRestLevel`. Counted per sample, so it is a function of the audio and not of
		// where the host cut it.
		if (inL == 0.0f && inR == 0.0f && std::abs(wetL) < kRestLevel && std::abs(wetR) < kRestLevel) {
			// ONCE, ON THE WAY IN. A first draft cleared on every quiet sample after the window (it pinned
			// the count at the threshold), and a review MEASURED what that costs: one second of silence took
			// 0.17-0.57 s of CPU at 48 kHz and up to 0.79 s at 96 kHz — the whole line wiped per sample.
			if (quietFrames_ < restFrames_ && ++quietFrames_ == restFrames_ && restEnabled_) {
				clearAudioState();
				++restClears_;
			}
		}
		else {
			quietFrames_ = 0;
		}
		// RIGHT THEN LEFT — `processMono` runs this over one buffer and the survivor must be the left.
		right[i] = wetR;
		left[i] = wetL;
	}
}

void Disperser::clearAudioState() noexcept {
	// THE SIGNAL'S MEMORY, and none of the control path's: the smoothers, the coefficients, the gains
	// and the two LFOs go on where they were.
	for (auto& s : s1_) {
		s = { 0.0, 0.0 };
	}
	for (auto& s : s2_) {
		s = { 0.0, 0.0 };
	}
	s1Old_ = s1_;
	s2Old_ = s2_;
	routeFadeRemaining_ = 0;
	punchNow_ = chirpFNow_ = chirp2FNow_ = readNow_ = 0.0f;
	std::fill(lineL_.begin(), lineL_.end(), 0.0f);
	std::fill(lineR_.begin(), lineR_.end(), 0.0f);
	fastEnv_ = slowEnv_ = 0.0f;
	riderEnv_ = 0.0f;
	riderGain_ = 1.0f;
}

void Disperser::processMono(float* mono, int frames) noexcept {
	if (mono == nullptr || frames <= 0) {
		return;
	}
	processStereo(mono, mono, frames);
}

//--------------------------------------------------------------------------------------------------//
// THE OPS TABLE.                                                                                     //
//--------------------------------------------------------------------------------------------------//

namespace {
Disperser& asDisperser(void* storage) { return *static_cast<Disperser*>(storage); }
} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(Disperser),
    .stateAlign = alignof(Disperser),
    .construct = [](void* storage) { new (storage) Disperser(); },
    .destruct = [](void* storage) { asDisperser(storage).~Disperser(); },
    .prepare = [](void* storage, double sampleRate, int) { asDisperser(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asDisperser(storage).reset(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asDisperser(storage).setTarget(stateOf(row, take).settings);
        },
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) { asDisperser(storage).setNoteContext(note); },
    .setTransportClock = [](void* storage,
                            const device::TransportClock& clock) { asDisperser(storage).setTransportClock(clock); },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asDisperser(storage).processStereo(left, right, frames);
        },
    .processMono =
        [](void* storage, float* mono, int frames) { asDisperser(storage).processMono(mono, frames); },
    .setReadingsSink =
        [](void* storage, device::ReadingsSink sink) { asDisperser(storage).setReadingsSink(sink); },
    .holdsTail = [](const void* storage) { return static_cast<const Disperser*>(storage)->holdsTail(); },
};

} // namespace feathers::fx::disperser
