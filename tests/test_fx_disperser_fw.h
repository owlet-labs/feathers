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

#pragma once

// **THE INSTRUMENT: THE FIRMWARE'S DISPERSER, TRANSCRIBED IN Q31.**
//
// The Eroder's first release was inaudible because its suite held the parameter derivation to the
// firmware and never compared the SAMPLE LOOP. So this is
// built FIRST and it is the loop itself: our firmware fork @ `2567b04c`,
// `dsp/disperser.{h,cpp}` and `dsp/filter/ladder_components.h`'s biquad allpass, statement
// for statement, in the firmware's integer arithmetic — ARM's `qadd` and `ssat`, NEON's `vqrdmulh`,
// `vqadd` and the NON-saturating `vshl`, and the saturating float-to-int casts ARM's VCVT makes. It is
// fed at the firmware's own level (`EFFECTIVE_0DBFS_Q31 = ONE_Q31 / 128`).
//
// **NOTHING HERE READS THE PORT.** Every constant is retyped from the reference, the phi triangles are
// the firmware's own `util.hpp` versions (Feathers' `character/util.hpp` has been rewritten since), and
// the kPhi literals are copied, not included.
//
// THREE SWITCHES, each OFF-by-default meaning "as the firmware does it", and each naming one deliberate
// deviation of the port so that a comparison can remove exactly that one and nothing else:
//   - `biasedIndex`   — the firmware's `+ 0x80000000 >> 25` on a UNIPOLAR freq/spread (see the port doc).
//   - `fastTan`       — the firmware's Padé tangent in the allpass prewarp.
//   - `quantiseIndex` — the firmware's 7-bit freq and spread indices.

#include "character/fixedpoint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace fwdisp {

using q31_t = int32_t;
constexpr q31_t ONE_Q31 = 2147483647;
constexpr double kZeroDbfs = 2147483647.0 / 128.0; // EFFECTIVE_0DBFS_Q31

// ---------------------------------------------------------------------------------------------------
// THE PRIMITIVES, AS THE HARDWARE DOES THEM.
// ---------------------------------------------------------------------------------------------------

inline q31_t sat32(int64_t v) {
	return v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : static_cast<q31_t>(v));
}
/// `qadd` / `q31_sat_add` / `vqadd_s32`.
inline q31_t qadd(q31_t a, q31_t b) { return sat32(static_cast<int64_t>(a) + b); }
/// `vqsub_s32`.
inline q31_t qsub(q31_t a, q31_t b) { return sat32(static_cast<int64_t>(a) - b); }
/// `smmul` — `multiply_32x32_rshift32`, NOT doubled.
inline q31_t smmul(q31_t a, q31_t b) { return static_cast<q31_t>((static_cast<int64_t>(a) * b) >> 32); }
/// `vqrdmulh_s32` — the doubling, rounding, saturating high multiply: a full q31 product.
inline q31_t vqrdmulh(q31_t a, q31_t b) {
	return sat32((2 * static_cast<int64_t>(a) * b + (INT64_C(1) << 31)) >> 32);
}
/// `vshl_n_s32(x, 1)` — a plain shift, which WRAPS.
inline q31_t shl1(q31_t x) { return static_cast<q31_t>(static_cast<uint32_t>(x) << 1); }
/// `ssat #bits`.
template <int bits>
inline q31_t ssat(q31_t v) {
	constexpr int32_t hi = static_cast<int32_t>((static_cast<uint32_t>(1) << (bits - 1)) - 1);
	constexpr int32_t lo = -hi - 1;
	return v > hi ? hi : (v < lo ? lo : v);
}
/// ARM VCVT: a float-to-int conversion that SATURATES (x86's yields INT_MIN). Also toward zero.
inline q31_t cvt(float v) {
	if (!(v == v)) {
		return 0;
	}
	if (v >= 2147483647.0f) {
		return INT32_MAX;
	}
	if (v <= -2147483648.0f) {
		return INT32_MIN;
	}
	return static_cast<q31_t>(v);
}
inline uint32_t cvtu(float v) {
	if (!(v > 0.0f)) {
		return 0;
	}
	if (v >= 4294967295.0f) {
		return UINT32_MAX;
	}
	return static_cast<uint32_t>(v);
}
inline size_t cvtSize(float v) { return static_cast<size_t>(cvtu(v)); }

// ---------------------------------------------------------------------------------------------------
// THE PHI LAYER, AS THE FIRMWARE HAS IT (`dsp/util.hpp`, `dsp/phi_triangle.hpp`).
// ---------------------------------------------------------------------------------------------------

constexpr float kPhiN025 = 0.8872984f;
constexpr float kPhi025 = 1.1271566f;
constexpr float kPhi033 = 1.1746627f;
constexpr float kPhi050 = 1.2720196f;
constexpr float kPhi067 = 1.3871872f;
constexpr float kPhi075 = 1.4352958f;
constexpr float kPhi100 = 1.6180340f;
constexpr float kPhi150 = 2.0581710f;

inline float wrapPhase(double phase) {
	const auto truncated = static_cast<int64_t>(phase);
	const double floored = (phase < 0.0 && phase != static_cast<double>(truncated)) ? static_cast<double>(truncated - 1)
	                                                                                : static_cast<double>(truncated);
	return static_cast<float>(phase - floored);
}

inline float triangleSimpleUnipolar(float phase, float duty = 1.0f) {
	phase = phase - std::floor(phase);
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

inline float triangleFloat(float phase, float duty = 1.0f) {
	phase = phase - std::floor(phase);
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

inline float freqMod(float pos, double phRaw, float phiFreq, float range = 0.5f, float duty = 0.8f) {
	return 1.0f + triangleSimpleUnipolar(wrapPhase((pos + phRaw) * phiFreq), duty) * range;
}
inline float phiTri(float pos, double phRaw, float phiFreq, float fm, float offset, float duty = 0.8f) {
	const double phase = static_cast<double>(pos) * phiFreq * fm + (phRaw + offset) * phiFreq;
	return triangleSimpleUnipolar(wrapPhase(phase), duty);
}
inline float phiTriBi(float pos, double phRaw, float phiFreq, float fm, float offset, float duty = 0.5f) {
	const double phase = static_cast<double>(pos) * phiFreq * fm + (phRaw + offset) * phiFreq;
	return triangleFloat(wrapPhase(phase), duty);
}

// ---------------------------------------------------------------------------------------------------
// `zone_param.hpp`.
// ---------------------------------------------------------------------------------------------------

struct ZoneInfo {
	int32_t index;
	float position;
};
inline ZoneInfo computeZoneQ31(q31_t value, int32_t numZones) {
	const q31_t zoneWidth = ONE_Q31 / numZones;
	const int32_t index = std::min(value / zoneWidth, numZones - 1);
	const q31_t zoneStart = index * zoneWidth;
	const q31_t offset = std::min(value - zoneStart, zoneWidth - 1);
	const float position = static_cast<float>(offset * numZones) / static_cast<float>(ONE_Q31);
	return { index, position };
}
constexpr q31_t getZoneStart(int32_t zoneIndex, int32_t numZones) { return (ONE_Q31 / numZones) * zoneIndex; }

// ---------------------------------------------------------------------------------------------------
// `computeDisperserTopoParams` / `computeDisperserTwistParams`, VERBATIM.
// ---------------------------------------------------------------------------------------------------

struct TopoP {
	int32_t zone { 0 };
	float q { 2.0f };
	float param0 { 0 }, param1 { 0 }, param2 { 0 }, lrOffset { 0 }, detuning { 0 }, harmonicBlend { 0 }, emphasis { 0 };
};
struct TwistP {
	float width { 0 }, punch { 0 }, chirpAmount { 0 }, spreadCurve { 0.5f }, qTilt { 0.0f }, phaseOffset { 0.0f },
	    lfoRateScale { 1.0f };
};

inline TopoP computeTopo(q31_t smoothedTopo, double effectiveTopo, float twistPhaseOffset) {
	auto zoneInfo = computeZoneQ31(smoothedTopo, 8);
	TopoP result;
	result.zone = zoneInfo.index;
	float pos = std::clamp(zoneInfo.position, 0.0f, 1.0f);
	double phRaw = effectiveTopo;
	double phRawDet = phRaw + static_cast<double>(twistPhaseOffset);
	float fm0 = freqMod(pos, phRaw, kPhi025);
	float fm1 = freqMod(pos, phRaw, kPhi050);
	float fm2 = freqMod(pos, phRaw, kPhi075);
	float fmDet = freqMod(pos, phRawDet, kPhi033);
	float fmHarm = freqMod(pos, phRawDet, kPhi067);
	float fmEmph = freqMod(pos, phRawDet, kPhi100);
	constexpr float kPhiN150 = 1.0f / kPhi150;
	constexpr float kLog40 = 3.6888794541f;
	float qTriangle = triangleSimpleUnipolar(wrapPhase((pos + phRaw) * kPhiN150), 1.0f);
	result.q = 0.5f * expf(qTriangle * kLog40);
	switch (result.zone) {
	case 0:
		result.param0 = phiTri(pos, phRaw, kPhi025, fm0, 0.1f);
		result.param1 = phiTri(pos, phRaw, kPhi050, fm1, 0.3f);
		result.param2 = 0.0f;
		result.lrOffset = 0.0f;
		result.detuning = phiTri(pos, phRawDet, kPhi033 * 0.5f, fmDet, 0.2f, 0.63f) * (1.0f - pos * 0.7f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi067, fmHarm, 0.4f) * pos;
		result.emphasis = phiTriBi(pos, phRawDet, kPhi100, fmEmph, 0.55f, 0.35f) * pos * 0.6f;
		break;
	case 1:
		result.param0 = phiTri(pos, phRaw, kPhi025, fm0, 0.1f, 0.8f);
		result.param1 = phiTri(pos, phRaw, kPhi050, fm1, 0.3f, 0.7f);
		result.param2 = phiTri(pos, phRaw, kPhi075, fm2, 0.6f, 0.6f);
		result.lrOffset = result.param1 * 0.5f;
		result.detuning = phiTri(pos, phRawDet, kPhi050 * 0.5f, fmDet, 0.3f, 0.54f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi033, fmHarm, 0.5f, 0.5f);
		result.emphasis = phiTriBi(pos, phRawDet, kPhi075, fmEmph, 0.4f, 0.6f) * 0.5f;
		break;
	case 2:
		result.param0 = pos * 5.0f;
		result.param1 = phiTri(pos, phRaw, kPhi075, fm1, 0.5f, 0.6f);
		result.lrOffset = pos * 0.4f;
		result.detuning = phiTri(pos, phRawDet, kPhi025 * 0.5f, fmDet, 0.15f, 0.59f) * (0.3f + pos * 0.7f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi075, fmHarm, 0.5f, 0.6f);
		result.emphasis = phiTriBi(pos, phRawDet, kPhi050, fmEmph, 0.3f, 0.45f) * (0.2f + pos * 0.6f);
		break;
	case 3:
		result.param0 = phiTri(pos, phRaw, kPhi050, fm0, 0.2f);
		result.param1 = phiTriBi(pos, phRaw, kPhi075, fm1, 0.5f) * 0.5f + 0.5f;
		result.param2 = phiTri(pos, phRaw, kPhi025, fm2, 0.8f);
		result.lrOffset = (result.param1 - 0.5f) * 0.3f;
		result.detuning = std::abs(result.param1 - 0.5f) * phiTri(pos, phRawDet, kPhi067 * 0.5f, fmDet, 0.4f, 0.5f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi050, fmHarm, 0.2f) * 0.8f;
		result.emphasis = (result.param1 - 0.5f) * phiTriBi(pos, phRawDet, kPhi033, fmEmph, 0.6f, 0.7f) * 0.7f;
		break;
	case 4:
		result.param0 = phiTri(pos, phRaw, kPhi050, fm0, 0.3f, 0.6f);
		result.param1 = phiTri(pos, phRaw, kPhi075, fm1, 0.5f, 0.5f);
		result.param2 = phiTri(pos, phRaw, kPhi025, fm2, 0.4f, 0.7f);
		result.lrOffset = pos * 0.5f;
		result.detuning = phiTri(pos, phRawDet, kPhi067 * 0.5f, fmDet, 0.5f, 0.55f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi033, fmHarm, 0.4f, 0.6f);
		result.emphasis = phiTriBi(pos, phRawDet, kPhi050, fmEmph, 0.4f, 0.5f) * 0.5f;
		break;
	case 5:
		result.param0 = phiTri(pos, phRaw, kPhi033, fm0, 0.2f);
		result.param1 = phiTri(pos, phRaw, kPhi075, fm1, 0.45f);
		result.param2 = phiTri(pos, phRaw, kPhi050, fm2, 0.7f);
		result.lrOffset = result.param2 * 0.2f;
		result.detuning = phiTri(pos, phRawDet, kPhi033 * 0.5f, fmDet, 0.3f, 0.54f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi067, fmHarm, 0.45f, 0.5f);
		result.emphasis = phiTriBi(pos, phRawDet, kPhi100, fmEmph, 0.5f, 0.4f) * 0.55f;
		break;
	case 6:
		result.param0 = phiTri(pos, phRaw, kPhi050, fm0, 0.15f);
		result.param1 = phiTri(pos, phRaw, kPhi033, fm1, 0.4f);
		result.param2 = phiTri(pos, phRaw, kPhi075, fm2, 0.65f);
		result.lrOffset = result.param0 * 0.4f;
		result.detuning = phiTri(pos, phRawDet, kPhi050 * 0.5f, fmDet, 0.4f, 0.63f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi025, fmHarm, 0.5f, 0.6f);
		result.emphasis = phiTriBi(pos, phRawDet, kPhi067, fmEmph, 0.35f, 0.65f) * 0.7f;
		break;
	case 7:
		result.param0 = phiTri(pos, phRaw, kPhi025, fm0, 0.1f);
		result.param1 = phiTri(pos, phRaw, kPhi050, fm1, 0.3f);
		result.param2 = phiTri(pos, phRaw, kPhi075, fm2, 0.55f);
		result.lrOffset = result.param2 * 0.15f;
		result.detuning = phiTri(pos, phRawDet, kPhi075 * 0.5f, fmDet, 0.25f, 0.45f);
		result.harmonicBlend = phiTri(pos, phRawDet, kPhi033, fmHarm, 0.35f, 0.6f);
		result.emphasis = phiTriBi(pos, phRawDet, kPhi050, fmEmph, 0.45f, 0.5f) * 0.45f;
		break;
	default:
		break;
	}
	return result;
}

inline TwistP computeTwist(q31_t smoothedTwist, double effectiveMeta) {
	auto zoneInfo = computeZoneQ31(smoothedTwist, 8);
	constexpr q31_t kZone5Start = getZoneStart(5, 8);
	TwistP result;
	double phRaw = effectiveMeta;
	auto meta = [&](float pos) {
		float fmW = freqMod(pos, phRaw, kPhi025);
		float fmP = freqMod(pos, phRaw, kPhi033);
		float fmC = freqMod(pos, phRaw, kPhi050);
		float fmQ = freqMod(pos, phRaw, kPhi067);
		float fmD = freqMod(pos, phRaw, kPhi075);
		float wS = std::min(phiTri(pos, phRaw, kPhi025, fmW, 0.166f, 0.8f) * 2.0f, 1.0f);
		float wP = phiTri(pos, phRaw, kPhi050, fmW, 0.984f, 0.7f);
		result.width = wS * wP;
		result.punch = phiTri(pos, phRaw, kPhi033, fmP, 0.3f, 0.7f);
		float curveRaw = phiTriBi(pos, phRaw, kPhi050, fmC, 0.5f);
		result.spreadCurve = 0.5f + curveRaw * 0.5f;
		result.chirpAmount = phiTri(pos, phRaw, kPhi067, fmD, 0.4f, 0.6f);
		result.qTilt = phiTriBi(pos, phRaw, kPhiN025, fmQ, 0.7f) * 0.8f;
		result.phaseOffset = pos * 5.0f;
		float fmLfo = freqMod(pos, phRaw, kPhi100);
		result.lfoRateScale = 0.25f + phiTri(pos, phRaw, kPhi150, fmLfo, 0.6f, 0.70f) * 1.75f;
	};
	if (phRaw != 0.0) {
		float pos = static_cast<float>(smoothedTwist) / static_cast<float>(ONE_Q31);
		meta(std::clamp(pos, 0.0f, 1.0f));
	}
	else if (zoneInfo.index < 5) {
		float pos = std::clamp(zoneInfo.position, 0.0f, 1.0f);
		switch (zoneInfo.index) {
		case 0: result.width = pos; break;
		case 1: result.punch = pos; break;
		case 2: result.spreadCurve = pos; break;
		case 3: result.chirpAmount = pos; break;
		case 4: result.qTilt = pos < 0.5f ? pos * 2.0f : 3.0f - pos * 4.0f; break;
		default: break;
		}
	}
	else {
		float pos = static_cast<float>(smoothedTwist - kZone5Start) / static_cast<float>(ONE_Q31 - kZone5Start);
		meta(std::clamp(pos, 0.0f, 1.0f));
	}
	return result;
}

// ---------------------------------------------------------------------------------------------------
// `ladder_components.h`: the biquad allpass.
// ---------------------------------------------------------------------------------------------------

struct Coeffs {
	q31_t a1 { 0 };
	q31_t a2 { 0 };
};
inline Coeffs computeCoeffs(float fc, float Q, float fs, bool fastTan, bool clampA2 = true) {
	float w = 3.14159265358979f * fc / fs;
	const float wc = std::clamp(w, 0.001f, 1.4f);
	float k = 0.0f;
	if (fastTan) {
		const float w2 = wc * wc;
		k = wc * (1.0f + w2 * 0.0666667f) / (1.0f - w2 * 0.2666667f);
	}
	else {
		k = static_cast<float>(std::tan(static_cast<double>(wc)));
	}
	float k2 = k * k;
	float kQ = k / std::max(Q, 0.1f);
	float norm = 1.0f / (1.0f + kQ + k2);
	float a1f = 2.0f * (k2 - 1.0f) * norm;
	float a2f = (1.0f - kQ + k2) * norm;
	Coeffs c;
	c.a1 = cvt(std::clamp(a1f * 0.5f, -1.0f, 0.9999f) * static_cast<float>(ONE_Q31));
	c.a2 = cvt((clampA2 ? std::clamp(a2f, -1.0f, 0.9999f) : a2f) * static_cast<float>(ONE_Q31));
	return c;
}

struct Stage {
	int32_t s1[2] {};
	int32_t s2[2] {};
	void processLR(q31_t in[2], const Coeffs& cl, const Coeffs& cr) {
		const Coeffs* c[2] = { &cl, &cr };
		for (int ch = 0; ch < 2; ++ch) {
			const q31_t x = in[ch];
			const q31_t a2x = vqrdmulh(x, c[ch]->a2);
			const q31_t y = qadd(a2x, s1[ch]);
			const q31_t diff = qsub(x, y);
			const q31_t a1diff = vqrdmulh(diff, c[ch]->a1);
			const q31_t a1s = qadd(a1diff, a1diff);
			const q31_t ns1 = qadd(a1s, s2[ch]);
			const q31_t a2y = vqrdmulh(y, c[ch]->a2);
			const q31_t ns2 = qsub(x, a2y);
			s1[ch] = ns1;
			s2[ch] = ns2;
			in[ch] = y;
		}
	}
	void fadeState() {
		for (int ch = 0; ch < 2; ++ch) {
			s1[ch] >>= 3;
			s2[ch] >>= 3;
		}
	}
};

// ---------------------------------------------------------------------------------------------------
// THE DISPERSER: `DisperserDelayState`, `Disperser` and `processDisperser`.
// ---------------------------------------------------------------------------------------------------

struct Options {
	bool biasedIndex = true;
	bool fastTan = true;
	bool quantiseIndex = true;
	/// The firmware glides every smoother up from 0 at power-on; the port snaps its first block. On,
	/// the transcription snaps too, so a comparison is not of two power-on glides.
	bool snapFirst = false;
	/// The firmware's `vshl_n_s32(vqrdmulh(in, gain), 1)` in the fractional delay write — a doubling,
	/// since `vqrdmulh` is already a full q31 product. Off, the write is the single gain its comments
	/// describe. A CAUSAL probe for the port doc, not a port deviation.
	bool doubledWrite = true;
	/// `BiquadAllpassCoeffs::compute`'s `std::clamp(a2f, -1.0f, 0.9999f)`. Off, a2 is left as computed
	/// (saturated only at the q31 ceiling a cast imposes) — the port's decision, since the clamp alone puts
	/// a pole on or outside the unit circle at the 14 Hz floor.
	bool clampA2 = true;
	/// The PORT's chirp loop-gain cap at 0.97 (`kChirpLoopCeiling`). Off, as the firmware has none.
	bool capLoop = false;
};

struct Fw {
	static constexpr size_t kMax = 8820;
	static constexpr size_t kMaxStages = 32;
	Options opt {};

	// DisperserParams
	q31_t topoValue = 0, twistValue = 0;
	float topoPhaseOffset = 0, twistPhaseOffset = 0, gammaPhase = 0;
	uint8_t freq = 64;
	uint8_t stages = 8;
	q31_t smoothedTopo = 0, smoothedTwist = 0, smoothedFreq = 0, smoothedSpread = 0;
	double smoothedFreqD = 0.0, smoothedSpreadD = 0.0; ///< `quantiseIndex = false` only
	/// `quantiseIndex = false` only: the centre index the knob gives, when >= 0. The port's knob is a
	/// float log2(Hz), not an integer, and its offsets truncate to whole samples — so a comparison hands
	/// the transcription the port's knob VALUE rather than the integer it rounds to.
	double effectiveFreqOverride = -1.0;
	int32_t lastTopoZone = -1;
	bool first = true;

	// DisperserDelayState
	std::vector<q31_t> bufL = std::vector<q31_t>(kMax, 0);
	std::vector<q31_t> bufR = std::vector<q31_t>(kMax, 0);
	size_t headPos = 0;
	uint16_t activityCounter = 0;

	// Disperser
	std::array<Stage, kMaxStages> st {};
	std::array<Coeffs, kMaxStages> coeffsL {}, coeffsR {};
	std::array<size_t, kMaxStages> stageOffsets {};
	std::array<q31_t, kMaxStages> ladderCrossGains {};
	std::array<q31_t, kMaxStages> stageGains = [] {
		std::array<q31_t, kMaxStages> g {};
		g.fill(ONE_Q31 / 2);
		return g;
	}();
	uint32_t currentDelayQ16 = 0, prevDelayQ16 = 0;
	uint16_t crossfadeRemaining = 0;
	q31_t fastEnv = 0, slowEnv = 0;
	double lastFreq = 255, lastSpread = 255; ///< the firmware's `uint8_t`s, as doubles for the continuous mode
	uint8_t lastActiveStages = 255;
	float lastLrOffset = 0.0f, lastQ = -1.0f, lastSpreadCurve = -1.0f, lastQTilt = 999.0f, lastBimodal = -1.0f,
	      lastDetuning = -1.0f, lastEmphasis = 999.0f;
	uint8_t coeffUpdateCounter = 4;
	float detuneLfoPhase = 0.0f, widthLfoPhase = 0.0f;

	// WHAT THE LAST BUFFER RAN ON — read by the suite.
	TopoP topo {};
	TwistP twist {};
	float centerHz = 0.0f;
	float stageHzL[kMaxStages] {};
	float stageHzR[kMaxStages] {};
	float stageQ[kMaxStages] {};
	float lastHalfSpread = 0.0f;
	uint64_t linePins = 0;
	uint64_t wraps = 0;
	bool fullPath = false;

	// --- DisperserDelayState --------------------------------------------------------------------
	bool hasEnergy() const {
		if (activityCounter > 0) {
			return true;
		}
		constexpr q31_t kEnergyThreshold = ONE_Q31 / 65536;
		constexpr size_t kSampleCount = 16;
		constexpr size_t kStride = kMax / kSampleCount;
		for (size_t i = 0; i < kSampleCount; ++i) {
			size_t pos = (headPos + i * kStride) % kMax;
			if (std::abs(bufL[pos]) > kEnergyThreshold || std::abs(bufR[pos]) > kEnergyThreshold) {
				return true;
			}
		}
		return false;
	}
	q31_t shl1Count(q31_t x) {
		const int64_t exact = static_cast<int64_t>(x) * 2;
		if (exact > INT32_MAX || exact < INT32_MIN) {
			++wraps;
		}
		return shl1(x);
	}
	q31_t pin(q31_t v) {
		const q31_t p = ssat<30>(v);
		if (p != v) {
			++linePins;
		}
		return p;
	}
	void writeFrac(q31_t inL, q31_t inR, uint32_t offsetQ16) {
		size_t offsetInt = offsetQ16 >> 16;
		uint32_t frac16 = offsetQ16 & 0xFFFF;
		size_t pos0 = (headPos + offsetInt) % kMax;
		size_t pos1 = (headPos + offsetInt + 1) % kMax;
		q31_t gain1 = static_cast<q31_t>(frac16) << 15;
		q31_t gain0 = ONE_Q31 - gain1;
		const auto scale = [&](q31_t in, q31_t g) { return opt.doubledWrite ? shl1Count(vqrdmulh(in, g)) : vqrdmulh(in, g); };
		const q31_t s0L = scale(inL, gain0);
		const q31_t s0R = scale(inR, gain0);
		const q31_t s1L = scale(inL, gain1);
		const q31_t s1R = scale(inR, gain1);
		bufL[pos0] = pin(qadd(bufL[pos0], s0L));
		bufL[pos1] = pin(qadd(bufL[pos1], s1L));
		bufR[pos0] = pin(qadd(bufR[pos0], s0R));
		bufR[pos1] = pin(qadd(bufR[pos1], s1R));
	}

	// --- Disperser ----------------------------------------------------------------------------------
	static float foldedDelay(float delaySamples) {
		constexpr float kMinDelayF = 441.0f;
		constexpr float kMaxDelayF = 8819.0f;
		constexpr float kMaxOctaves = 4.32f;
		delaySamples = std::max(delaySamples, 1.0f);
		float octavesFromMin = log2f(delaySamples / kMinDelayF);
		if (octavesFromMin < 0.0f || octavesFromMin > kMaxOctaves) {
			float doubled = 2.0f * kMaxOctaves;
			octavesFromMin = std::fmod(std::fmod(octavesFromMin, doubled) + doubled, doubled);
			if (octavesFromMin > kMaxOctaves) {
				octavesFromMin = doubled - octavesFromMin;
			}
		}
		float targetDelay = kMinDelayF * exp2f(octavesFromMin);
		constexpr float kFifth = 0.5849625f;
		float idealShift = log2f(targetDelay / delaySamples);
		float quantizedShift = std::round(idealShift / kFifth) * kFifth;
		float folded = delaySamples * exp2f(quantizedShift);
		if (folded > kMaxDelayF) {
			folded *= 0.5f;
		}
		if (folded < kMinDelayF) {
			folded *= 2.0f;
		}
		return folded;
	}

	void updateCoefficients(double freqIdx, double spreadIdx, float lrOffset, float q, uint8_t activeStages,
	                        float spreadCurve, float qTilt, float bimodalSeparation, float detuning, float emphasis,
	                        float lfoRateScale, size_t numSamples) {
		const double freqTest = std::floor(freqIdx);
		const double spreadTest = opt.quantiseIndex ? std::floor(spreadIdx) : std::floor(spreadIdx * 127.0);
		bool lrChanged = std::abs(lrOffset - lastLrOffset) > 0.01f;
		bool qChanged = std::abs(q - lastQ) > 0.05f;
		bool stagesChanged = activeStages != lastActiveStages;
		bool curveChanged = std::abs(spreadCurve - lastSpreadCurve) > 0.02f;
		bool tiltChanged = std::abs(qTilt - lastQTilt) > 0.02f;
		bool bimodalChanged = std::abs(bimodalSeparation - lastBimodal) > 0.02f;
		bool detuneChanged = std::abs(detuning - lastDetuning) > 0.01f;
		bool emphasisChanged = std::abs(emphasis - lastEmphasis) > 0.02f;
		bool detuneActive = detuning > 0.001f;
		if (freqTest == lastFreq && spreadTest == lastSpread && !lrChanged && !qChanged && !stagesChanged && !curveChanged
		    && !tiltChanged && !bimodalChanged && !detuneChanged && !emphasisChanged && !detuneActive) {
			return;
		}
		lastFreq = freqTest;
		lastSpread = spreadTest;
		lastLrOffset = lrOffset;
		lastQ = q;
		lastActiveStages = activeStages;
		if (stagesChanged) {
			for (size_t i = 0; i < activeStages && i < kMaxStages; ++i) {
				float crossAmt = 0.05f + 0.20f * static_cast<float>(i) / static_cast<float>(activeStages);
				ladderCrossGains[i] = cvt(crossAmt * static_cast<float>(ONE_Q31));
			}
		}
		lastSpreadCurve = spreadCurve;
		lastQTilt = qTilt;
		lastBimodal = bimodalSeparation;
		lastDetuning = detuning;
		lastEmphasis = emphasis;

		constexpr float k127Recip = 1.0f / 127.0f;
		const float cHz = opt.quantiseIndex ? exp2f(static_cast<float>(freqIdx) * k127Recip * 13.0f)
		                                    : static_cast<float>(std::exp2(freqIdx * 13.0 / 127.0));
		const float spreadOctaves = opt.quantiseIndex ? static_cast<float>(spreadIdx) * k127Recip * 4.0f
		                                              : static_cast<float>(spreadIdx * 4.0);
		centerHz = cHz;

		constexpr float kWidthLfoBaseRate = 0.06f;
		const float kWidthLfoBaseInc = kWidthLfoBaseRate * static_cast<float>(numSamples) * 4 / 44100.0f;
		constexpr float kWidthLfoDepth = 0.06f;
		widthLfoPhase += kWidthLfoBaseInc * lfoRateScale;
		if (widthLfoPhase >= 1.0f) {
			widthLfoPhase -= 1.0f;
		}
		auto tri = [](float p) { return p < 0.25f ? p * 4.0f : (p < 0.75f ? 2.0f - p * 4.0f : p * 4.0f - 4.0f); };
		float widthLfoVal = tri(widthLfoPhase);
		constexpr float kDetuneLfoRateRatio = 1.1746627f;
		const float kDetuneLfoBaseInc = kWidthLfoBaseInc * kDetuneLfoRateRatio;
		constexpr float kDetuneLfoDepth = 0.04f;
		detuneLfoPhase += kDetuneLfoBaseInc * lfoRateScale;
		if (detuneLfoPhase >= 1.0f) {
			detuneLfoPhase -= 1.0f;
		}
		float detuneLfoVal = tri(detuneLfoPhase);
		constexpr float kBaselineStereoOffset = 0.01f;
		constexpr float kMaxLrOffset = 0.3f;
		float halfSpread = std::clamp(lrOffset * 0.5f + widthLfoVal * kWidthLfoDepth + detuneLfoVal * kDetuneLfoDepth
		                                  + kBaselineStereoOffset,
		                              -kMaxLrOffset, kMaxLrOffset);
		lastHalfSpread = halfSpread;
		float lOffsetOct = -halfSpread;
		float rOffsetOct = halfSpread;
		float qBase = std::clamp(q, 0.5f, 20.0f);
		size_t numActive = std::max(static_cast<size_t>(activeStages), size_t { 1 });
		float curveClamped = std::clamp(spreadCurve, 0.0f, 1.0f);
		constexpr float kLog10 = 2.302585092994f;
		float exponent = expf((1.0f - curveClamped * 2.0f) * kLog10);
		float qTiltClamped = std::clamp(qTilt, -1.0f, 1.0f);
		bool isBimodal = bimodalSeparation > 0.02f;
		float modeAHz = cHz;
		float modeBHz = cHz;
		if (isBimodal) {
			float halfSep = bimodalSeparation * 0.5f;
			float sepMult = exp2f(halfSep);
			modeAHz = cHz / sepMult;
			modeBHz = cHz * sepMult;
		}
		bool useDetuning = detuning > 0.001f;
		float maxDetuneOct = useDetuning ? (50.0f * detuning / 1200.0f) : 0.0f;
		for (size_t i = 0; i < numActive; ++i) {
			float t = static_cast<float>(i) / std::max(1.0f, static_cast<float>(numActive - 1));
			float stageHzBase;
			if (isBimodal) {
				bool isModeB = (i % 2 == 1);
				float modeCenter = isModeB ? modeBHz : modeAHz;
				size_t stagesInMode = (numActive + (isModeB ? 0 : 1)) / 2;
				size_t indexInMode = i / 2;
				float tLocal = static_cast<float>(indexInMode) / std::max(1.0f, static_cast<float>(stagesInMode - 1));
				float curved = (tLocal > 0.001f) ? expf(exponent * logf(tLocal)) : 0.0f;
				float localPosition = isModeB ? -curved : curved;
				float localSpread = spreadOctaves * 0.5f;
				stageHzBase = modeCenter * exp2f(localPosition * localSpread);
			}
			else {
				float curved = (t > 0.001f) ? expf(exponent * logf(t)) : 0.0f;
				float stagePosition = curved * 2.0f - 1.0f;
				stageHzBase = cHz * exp2f(stagePosition * spreadOctaves);
			}
			float qFactor = exp2f(qTiltClamped * 2.0f * (t * 2.0f - 1.0f));
			float stageQv = std::clamp(qBase * qFactor, 0.5f, 20.0f);
			float detuneOct = useDetuning ? (((i & 1) ? -1.0f : 1.0f) * maxDetuneOct) : 0.0f;
			float hzL = std::clamp(stageHzBase * exp2f(lOffsetOct + detuneOct), 1.0f, 16000.0f);
			float hzR = std::clamp(stageHzBase * exp2f(rOffsetOct + detuneOct), 1.0f, 16000.0f);
			coeffsL[i] = computeCoeffs(hzL, stageQv, 44100.0f, opt.fastTan, opt.clampA2);
			coeffsR[i] = computeCoeffs(hzR, stageQv, 44100.0f, opt.fastTan, opt.clampA2);
			stageHzL[i] = hzL;
			stageHzR[i] = hzR;
			stageQ[i] = stageQv;
			float rawOffset = 44100.0f / stageHzBase;
			float folded = foldedDelay(rawOffset);
			size_t offset = cvtSize(folded);
			stageOffsets[i] = std::clamp(offset, size_t { 1 }, kMax - 1);
			float emphasisClamped = std::clamp(emphasis, -1.0f, 1.0f);
			float stageGain = 1.0f;
			if (std::abs(emphasisClamped) > 0.01f) {
				float tiltDb = (emphasisClamped > 0) ? (emphasisClamped * 2.5f) : (emphasisClamped * 2.0f);
				float stageDb = tiltDb * (t * 2.0f - 1.0f);
				constexpr float kLog10Over20 = 0.1151292546f;
				stageGain = expf(stageDb * kLog10Over20);
			}
			q31_t targetGain = cvt(std::clamp(stageGain, 0.6f, 1.7f) * static_cast<float>(ONE_Q31 / 2));
			q31_t currentGain = stageGains[i];
			q31_t delta = (targetGain - currentGain) >> 4;
			if (delta == 0 && targetGain != currentGain) {
				delta = (targetGain > currentGain) ? 1 : -1;
			}
			stageGains[i] = currentGain + delta;
		}
	}

	float detectTransient(q31_t inL, q31_t inR, size_t delaySamples) {
		q31_t absIn = std::abs(inL >> 1) + std::abs(inR >> 1);
		constexpr q31_t kFastDecay = static_cast<q31_t>(0.998 * ONE_Q31);
		if (absIn > fastEnv) {
			fastEnv = absIn;
		}
		else {
			fastEnv = smmul(fastEnv, kFastDecay);
		}
		constexpr size_t kMinDelay = 441;
		constexpr size_t kMaxDelay = 8819;
		float delayNorm = static_cast<float>(std::clamp(delaySamples, kMinDelay, kMaxDelay) - kMinDelay)
		                  / static_cast<float>(kMaxDelay - kMinDelay);
		float slowCoeffF = 0.999f + delayNorm * 0.0009f;
		q31_t slowCoeff = cvt(slowCoeffF * static_cast<float>(ONE_Q31));
		slowEnv = slowEnv + smmul(absIn - slowEnv, ONE_Q31 - slowCoeff);
		q31_t diff = fastEnv - slowEnv;
		if (diff <= 0) {
			return 0.0f;
		}
		return std::clamp(static_cast<float>(diff) / static_cast<float>(ONE_Q31), 0.0f, 1.0f);
	}

	void gainStage(q31_t p[2], size_t i) {
		p[0] = shl1Count(vqrdmulh(p[0], stageGains[i]));
		p[1] = shl1Count(vqrdmulh(p[1], stageGains[i]));
	}

	void processWithTopology(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR, size_t numStages, int32_t topology,
	                         q31_t crossGain) {
		numStages = std::min(numStages, kMaxStages);
		switch (topology) {
		case 1: { // Ladder
			q31_t p[2] = { inL, inR };
			for (size_t i = 0; i < numStages; ++i) {
				q31_t crossG = ladderCrossGains[i];
				q31_t keepG = ONE_Q31 - crossG;
				q31_t bl = qadd(smmul(p[0], keepG) << 1, smmul(p[1], crossG) << 1);
				q31_t br = qadd(smmul(p[1], keepG) << 1, smmul(p[0], crossG) << 1);
				p[0] = bl;
				p[1] = br;
				st[i].processLR(p, coeffsL[i], coeffsR[i]);
				gainStage(p, i);
			}
			outL = p[0];
			outR = p[1];
			break;
		}
		case 3: { // Cross
			q31_t p[2] = { inL, inR };
			for (size_t i = 0; i < numStages; ++i) {
				st[i].processLR(p, coeffsL[i], coeffsR[i]);
				gainStage(p, i);
				if ((i & 3) == 3) {
					q31_t keepGain = ONE_Q31 - crossGain;
					q31_t nl = qadd(smmul(p[0], keepGain) << 1, smmul(p[1], crossGain) << 1);
					q31_t nr = qadd(smmul(p[1], keepGain) << 1, smmul(p[0], crossGain) << 1);
					p[0] = nl;
					p[1] = nr;
				}
			}
			outL = p[0];
			outR = p[1];
			break;
		}
		case 4: { // Parallel
			size_t half = numStages / 2;
			if (half == 0) {
				half = 1;
			}
			q31_t a[2] = { inL, inR };
			for (size_t i = 0; i < half; ++i) {
				st[i].processLR(a, coeffsL[i], coeffsR[i]);
				gainStage(a, i);
			}
			q31_t b[2] = { inL, inR };
			for (size_t i = half; i < numStages; ++i) {
				st[i].processLR(b, coeffsL[i], coeffsR[i]);
				gainStage(b, i);
			}
			outL = qadd(vqrdmulh(a[0], 0x40000000), vqrdmulh(b[0], 0x40000000));
			outR = qadd(vqrdmulh(a[1], 0x40000000), vqrdmulh(b[1], 0x40000000));
			break;
		}
		case 5: { // Nested
			size_t half = numStages / 2;
			if (half == 0) {
				half = 1;
			}
			q31_t inner[2] = { inL, inR };
			for (size_t i = 0; i < half; ++i) {
				st[i].processLR(inner, coeffsL[i], coeffsR[i]);
				gainStage(inner, i);
			}
			q31_t outer[2] = { qadd(vqrdmulh(inL, 0x40000000), vqrdmulh(inner[0], 0x40000000)),
				               qadd(vqrdmulh(inR, 0x40000000), vqrdmulh(inner[1], 0x40000000)) };
			for (size_t i = half; i < numStages; ++i) {
				st[i].processLR(outer, coeffsL[i], coeffsR[i]);
				gainStage(outer, i);
			}
			outL = qadd(smmul(outer[0], 0x66666666) << 1, smmul(outer[1], 0x19999999) << 1);
			outR = qadd(smmul(outer[1], 0x66666666) << 1, smmul(outer[0], 0x19999999) << 1);
			break;
		}
		case 7: { // Spring
			q31_t p[2] = { inL, inR };
			size_t tap1 = numStages / 3;
			size_t tap2 = (numStages * 2) / 3;
			if (tap1 == 0) {
				tap1 = 1;
			}
			if (tap2 <= tap1) {
				tap2 = tap1 + 1;
			}
			q31_t t1[2] = { 0, 0 };
			q31_t t2[2] = { 0, 0 };
			for (size_t i = 0; i < numStages; ++i) {
				st[i].processLR(p, coeffsL[i], coeffsR[i]);
				gainStage(p, i);
				if (i == tap1) {
					t1[0] = p[0];
					t1[1] = p[1];
				}
				else if (i == tap2) {
					t2[0] = p[0];
					t2[1] = p[1];
				}
			}
			q31_t bl = qadd(qadd(vqrdmulh(p[0], 0x59999999), vqrdmulh(t1[0], 0x13333333)), vqrdmulh(t2[0], 0x13333333));
			q31_t br = qadd(qadd(vqrdmulh(p[1], 0x59999999), vqrdmulh(t1[1], 0x13333333)), vqrdmulh(t2[1], 0x13333333));
			outL = qadd(smmul(bl, 0x70000000) << 1, smmul(br, 0x10000000) << 1);
			outR = qadd(smmul(br, 0x70000000) << 1, smmul(bl, 0x10000000) << 1);
			break;
		}
		default: { // 0 Cascade, 2 Owlpass, 6 Diffuse
			q31_t p[2] = { inL, inR };
			for (size_t i = 0; i < numStages; ++i) {
				st[i].processLR(p, coeffsL[i], coeffsR[i]);
				gainStage(p, i);
			}
			outL = p[0];
			outR = p[1];
			break;
		}
		}
	}

	void processSample(q31_t inL, q31_t inR, q31_t& outL, q31_t& outR, uint8_t nStages, float punch, float chirp,
	                   float folded, int32_t topology, q31_t crossGain, q31_t punchGain, q31_t chirpGainF,
	                   q31_t chirpGain2F, bool useHarmonic2F) {
		constexpr uint32_t kMinDelayQ16 = 441u << 16;
		constexpr uint16_t kFade = 256;
		uint32_t newDelayQ16 = cvtu(folded * 65536.0f);
		if (currentDelayQ16 == 0) {
			currentDelayQ16 = newDelayQ16;
		}
		else if (newDelayQ16 != currentDelayQ16 && crossfadeRemaining == 0) {
			uint32_t curInt = currentDelayQ16 >> 16;
			uint32_t newInt = newDelayQ16 >> 16;
			uint32_t diff = (newInt > curInt) ? (newInt - curInt) : (curInt - newInt);
			if (diff > curInt / 10) {
				prevDelayQ16 = currentDelayQ16;
				currentDelayQ16 = newDelayQ16;
				crossfadeRemaining = kFade;
			}
			else {
				currentDelayQ16 = newDelayQ16;
			}
		}
		auto fades = [&](q31_t& fadeNew, q31_t& fadeOld) {
			fadeNew = cvt((1.0f - static_cast<float>(crossfadeRemaining) / kFade) * static_cast<float>(ONE_Q31));
			fadeOld = ONE_Q31 - fadeNew;
		};
		q31_t procL = inL;
		q31_t procR = inR;
		if (punchGain != 0) {
			q31_t writeL = smmul(inL, punchGain) << 1;
			q31_t writeR = smmul(inR, punchGain) << 1;
			if (crossfadeRemaining > 0) {
				q31_t fn, fo;
				fades(fn, fo);
				writeFrac(smmul(writeL, fn) << 1, smmul(writeR, fn) << 1, currentDelayQ16);
				writeFrac(smmul(writeL, fo) << 1, smmul(writeR, fo) << 1, prevDelayQ16);
			}
			else {
				writeFrac(writeL, writeR, currentDelayQ16);
			}
		}
		q31_t fbL = bufL[headPos];
		q31_t fbR = bufR[headPos];
		bufL[headPos] = 0;
		bufR[headPos] = 0;
		float fbAmount = std::max(punch, chirp);
		if (fbAmount > 0.01f) {
			q31_t fbGain = cvt(fbAmount * 0.99f * 1.0f * static_cast<float>(ONE_Q31));
			q31_t sl = ssat<30>(smmul(fbL, fbGain) << 1);
			q31_t sr = ssat<30>(smmul(fbR, fbGain) << 1);
			procL = qadd(procL, sl);
			procR = qadd(procR, sr);
		}
		else if (fbL != 0 || fbR != 0) {
			procL = qadd(procL, fbL);
			procR = qadd(procR, fbR);
		}
		processWithTopology(procL, procR, outL, outR, nStages, topology, crossGain);
		if (chirpGainF != 0) {
			q31_t wLF = smmul(outL, chirpGainF) << 1;
			q31_t wRF = smmul(outR, chirpGainF) << 1;
			if (crossfadeRemaining > 0) {
				q31_t fn, fo;
				fades(fn, fo);
				writeFrac(smmul(wLF, fn) << 1, smmul(wRF, fn) << 1, currentDelayQ16);
				writeFrac(smmul(wLF, fo) << 1, smmul(wRF, fo) << 1, prevDelayQ16);
				if (useHarmonic2F) {
					uint32_t new2F = std::max(currentDelayQ16 >> 1, kMinDelayQ16);
					uint32_t old2F = std::max(prevDelayQ16 >> 1, kMinDelayQ16);
					q31_t wL2 = smmul(outL, chirpGain2F) << 1;
					q31_t wR2 = smmul(outR, chirpGain2F) << 1;
					writeFrac(smmul(wL2, fn) << 1, smmul(wR2, fn) << 1, new2F);
					writeFrac(smmul(wL2, fo) << 1, smmul(wR2, fo) << 1, old2F);
				}
				crossfadeRemaining--;
			}
			else {
				writeFrac(wLF, wRF, currentDelayQ16);
				if (useHarmonic2F) {
					uint32_t d2F = std::max(currentDelayQ16 >> 1, kMinDelayQ16);
					writeFrac(smmul(outL, chirpGain2F) << 1, smmul(outR, chirpGain2F) << 1, d2F);
				}
			}
		}
		else if (crossfadeRemaining > 0) {
			crossfadeRemaining--;
		}
		headPos = (headPos + 1) % kMax;
	}

	/// `Disperser::routingBound`, restated: the product of the stages' gains along each path, summed with each
	/// blend's weights. The port's own float arithmetic over the gains the q31 stages hold (stored halved).
	float routingBound(uint8_t nStages, int32_t topology) const {
		const size_t n = std::clamp<size_t>(nStages, 1, kMaxStages);
		const auto product = [this](size_t from, size_t to) {
			float p = 1.0f;
			for (size_t i = from; i < to; ++i) {
				p *= std::abs(2.0f * static_cast<float>(stageGains[i]) / static_cast<float>(ONE_Q31));
			}
			return p;
		};
		const auto w = [](uint32_t q) { return static_cast<float>(static_cast<double>(q) / 2147483648.0); };
		switch (topology) {
		case 4: {
			const size_t half = std::max<size_t>(n / 2, 1);
			return 0.5f * product(0, half) + 0.5f * product(half, n);
		}
		case 5: {
			const size_t half = std::max<size_t>(n / 2, 1);
			return (0.5f + 0.5f * product(0, half)) * product(half, n) * (w(0x66666666u) + w(0x19999999u));
		}
		case 7: {
			size_t tap1 = n / 3;
			size_t tap2 = (n * 2) / 3;
			if (tap1 == 0) {
				tap1 = 1;
			}
			if (tap2 <= tap1) {
				tap2 = tap1 + 1;
			}
			const float t1 = tap1 < n ? product(0, tap1 + 1) : 0.0f;
			const float t2 = tap2 < n ? product(0, tap2 + 1) : 0.0f;
			return (w(0x59999999u) * product(0, n) + w(0x13333333u) * t1 + w(0x13333333u) * t2)
			       * (w(0x70000000u) + w(0x10000000u));
		}
		default: return product(0, n);
		}
	}

	void processBuffer(q31_t* l, q31_t* r, size_t n, uint8_t nStages, float punch, float chirp, size_t delaySamples,
	                   float harmonicBlend, int32_t topology, float crossMix) {
		q31_t crossGain = cvt((0.4f + crossMix * 0.5f) * static_cast<float>(ONE_Q31));
		bool needsFeedback = (punch > 0.01f || chirp > 0.01f || hasEnergy());
		fullPath = needsFeedback;
		if (!needsFeedback) {
			for (size_t i = 0; i < n; ++i) {
				processWithTopology(l[i], r[i], l[i], r[i], nStages, topology, crossGain);
			}
			if (activityCounter > 0) {
				activityCounter--;
			}
			return;
		}
		float folded = foldedDelay(static_cast<float>(delaySamples));
		q31_t fbL = bufL[headPos];
		q31_t fbR = bufR[headPos];
		float fbAmount = std::max(punch, chirp);
		q31_t fbGain = cvt(fbAmount * 0.99f * static_cast<float>(ONE_Q31));
		q31_t detectL = qadd(l[0], smmul(fbL, fbGain) << 1);
		q31_t detectR = qadd(r[0], smmul(fbR, fbGain) << 1);
		float transient = detectTransient(detectL, detectR, cvtSize(folded));
		q31_t punchGain = 0, chirpGainF = 0, chirpGain2F = 0;
		bool useHarmonic2F = (harmonicBlend > 0.01f);
		if (punch > 0.01f) {
			float punchWrite = punch * (0.5f + transient * 0.49f);
			punchGain = cvt(std::min(punchWrite, 0.99f) * static_cast<float>(ONE_Q31));
			activityCounter = 700;
		}
		if (chirp > 0.01f) {
			float writeAmount = std::min(chirp * (0.50f + transient * 0.50f), 0.99f);
			float fGain = 1.0f - harmonicBlend * 0.5f;
			float f2Gain = harmonicBlend * 0.8f;
			float gF = writeAmount * fGain;
			float g2F = writeAmount * f2Gain;
			if (opt.capLoop) {
				// THE PORT'S LOOP-GAIN CAP (not the firmware's) — the same float expression as `Disperser::tick`,
				// its routing factor included (`Disperser::routingBound`, restated here over the q31 gains).
				const float loopGain =
				    2.0f * (gF + g2F) * (fbAmount * 0.99f) * std::max(1.0f, routingBound(nStages, topology));
				const float scale = loopGain > 0.97f ? 0.97f / loopGain : 1.0f;
				gF *= scale;
				g2F *= scale;
			}
			chirpGainF = cvt(gF * static_cast<float>(ONE_Q31));
			chirpGain2F = cvt(g2F * static_cast<float>(ONE_Q31));
			activityCounter = 700;
		}
		for (size_t i = 0; i < n; ++i) {
			q31_t oL, oR;
			processSample(l[i], r[i], oL, oR, nStages, punch, chirp, folded, topology, crossGain, punchGain, chirpGainF,
			              chirpGain2F, useHarmonic2F);
			l[i] = oL;
			r[i] = oR;
		}
		if (activityCounter > 0) {
			activityCounter--;
		}
	}

	/// `processDisperser`, one buffer. `noteCode` < 0 is "no note". Cables are zero.
	void process(q31_t* l, q31_t* r, size_t n, int32_t noteCode) {
		constexpr q31_t smoothingAlpha = static_cast<q31_t>(0.03f * static_cast<float>(ONE_Q31));
		if (first && opt.snapFirst) {
			smoothedTopo = topoValue;
			smoothedTwist = twistValue;
		}
		smoothedTopo += (smmul(topoValue - smoothedTopo, smoothingAlpha) << 1);
		smoothedTwist += (smmul(twistValue - smoothedTwist, smoothingAlpha) << 1);
		const double effMeta = static_cast<double>(twistPhaseOffset) + 1024.0 * static_cast<double>(gammaPhase);
		const double effTopo = static_cast<double>(topoPhaseOffset) + 1024.0 * static_cast<double>(gammaPhase);
		twist = computeTwist(smoothedTwist, effMeta);
		topo = computeTopo(smoothedTopo, effTopo, twist.phaseOffset);
		if (lastTopoZone != topo.zone && lastTopoZone >= 0) {
			for (auto& s : st) {
				s.fadeState();
			}
		}
		lastTopoZone = topo.zone;
		float lrSpreadOffset = twist.width;
		float spreadMod = 1.0f;
		switch (topo.zone) {
		case 0: spreadMod = topo.param0; break;
		case 1:
			spreadMod = topo.param0 * 0.7f;
			lrSpreadOffset += topo.lrOffset * 0.3f;
			break;
		case 2:
			spreadMod = topo.param0;
			lrSpreadOffset += topo.lrOffset;
			break;
		case 3: spreadMod = topo.param0 * 0.5f; break;
		case 4: spreadMod = topo.param0 * 0.3f; break;
		case 5: spreadMod = 0.3f + topo.param0 * 0.7f; break;
		case 6: spreadMod = topo.param0; break;
		case 7: spreadMod = 0.2f + topo.param0 * 0.5f; break;
		default: break;
		}
		const float spreadClamped = std::clamp(spreadMod, 0.0f, 1.0f);
		q31_t dispSpread = cvt(spreadClamped * static_cast<float>(ONE_Q31));

		// PITCH TRACKING. `effectiveFreq` is a `uint8_t` in the firmware; the continuous mode keeps a double.
		double effectiveFreq = freq;
		if (noteCode >= 0 && noteCode < 128) {
			float noteHz = 440.0f * exp2f((static_cast<float>(noteCode) - 69.0f) / 12.0f);
			float offsetOctaves = (static_cast<float>(freq) - 64.0f) / 64.0f * 6.5f;
			float offsetHz = noteHz * exp2f(offsetOctaves);
			float octaves = log2f(std::max(offsetHz, 1.0f));
			const float idx = std::clamp(octaves * 127.0f / 13.0f, 0.0f, 127.0f);
			effectiveFreq = opt.quantiseIndex ? static_cast<double>(static_cast<uint8_t>(idx)) : static_cast<double>(idx);
		}
		if (!opt.quantiseIndex && effectiveFreqOverride >= 0.0) {
			effectiveFreq = effectiveFreqOverride;
		}
		const float bimodal = (topo.zone == 2) ? topo.param0 : 0.0f;

		// `updateCoefficientsSmoothed`.
		double freqIdx = 0.0;
		double spreadIdx = 0.0;
		if (opt.quantiseIndex) {
			q31_t dispFreq = static_cast<q31_t>(static_cast<uint8_t>(effectiveFreq)) << 24;
			if (first && opt.snapFirst) {
				smoothedFreq = dispFreq;
				smoothedSpread = dispSpread;
			}
			smoothedFreq = smoothedFreq + (smmul(dispFreq - smoothedFreq, smoothingAlpha) << 1);
			smoothedSpread = smoothedSpread + (smmul(dispSpread - smoothedSpread, smoothingAlpha) << 1);
			if (opt.biasedIndex) {
				freqIdx = static_cast<double>((static_cast<uint32_t>(smoothedFreq) + 0x80000000u) >> 25);
				spreadIdx = static_cast<double>((static_cast<uint32_t>(smoothedSpread) + 0x80000000u) >> 25);
			}
			else {
				freqIdx = static_cast<double>(static_cast<uint32_t>(smoothedFreq) >> 24);
				spreadIdx = static_cast<double>(static_cast<uint32_t>(smoothedSpread) >> 24);
			}
		}
		else {
			// THE CONTINUOUS MODE SMOOTHS THE SPREAD AS A FRACTION (0..1), as the port does, and hands
			// `updateCoefficients` that fraction: its offsets are truncated to whole samples, so an
			// expression that differs from the port's in the last bit can land a delay a sample away.
			const auto sp = static_cast<double>(spreadClamped);
			if (first && opt.snapFirst) {
				smoothedFreqD = effectiveFreq;
				smoothedSpreadD = sp;
			}
			else {
				smoothedFreqD += (effectiveFreq - smoothedFreqD) * 0.03;
				smoothedSpreadD += (sp - smoothedSpreadD) * 0.03;
			}
			freqIdx = opt.biasedIndex ? 64.0 + smoothedFreqD * 0.5 : smoothedFreqD;
			spreadIdx = opt.biasedIndex ? (64.0 + smoothedSpreadD * 63.5) / 127.0 : smoothedSpreadD;
		}
		first = false;
		if (++coeffUpdateCounter >= 4) {
			coeffUpdateCounter = 0;
			updateCoefficients(freqIdx, spreadIdx, lrSpreadOffset, topo.q, stages, twist.spreadCurve, twist.qTilt, bimodal,
			                   topo.detuning, topo.emphasis, twist.lfoRateScale, n);
		}
		float crossMix = (topo.zone == 3) ? (0.3f + topo.param0 * 0.5f) : 0.0f;
		size_t centerStage = stages / 2;
		size_t delaySamples = stageOffsets[centerStage] * 2;
		processBuffer(l, r, n, stages, twist.punch, twist.chirpAmount, delaySamples, topo.harmonicBlend, topo.zone,
		              crossMix);
	}
};

} // namespace fwdisp
