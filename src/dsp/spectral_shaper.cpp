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

#include "dsp/spectral_shaper.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>

// FEATHERS_FORCE_SIMDE takes the arm64 road on x86-64 too, so scripts/clang-preflight.sh compiles what macOS does.
#if !defined(FEATHERS_FORCE_SIMDE)                                                                        \
    && (defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#else
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse2.h"
// The kernels' NaN handling is x86's, exactly: _mm_max_pd returns its SECOND operand when either is NaN (the hz floor
// and the filter's floor at 0 rely on it), and the truncating conversions' range is relied on by the |k| < 2^30 guard.
// SIMDe's fast modes trade both away, so they are refused here rather than silently changing what a NaN becomes.
#if defined(SIMDE_FAST_NANS) || defined(SIMDE_FAST_CONVERSION_RANGE)
#error "spectral_shaper.cpp needs SIMDe's exact NaN and conversion semantics (the NaN floors use x86's max-returns-second rule)"
#endif
#endif

namespace feathers::spectral {

namespace {
constexpr double kDbPerOctaveOfUnitExponent = 6.020599913279624; // 20·log10(2)
constexpr double kResonanceInvTwoWidthSq = 1.0 / (2.0 * kResonanceWidthOctaves * kResonanceWidthOctaves);
// The filter's exponent is held here before exp2: 2^±1000 is finite, so no lp/hp/bp expression meets inf/inf.
constexpr double kFilterMaxExponent = 1000.0;

// Each bound is written so that a NaN lands on the floor rather than passing through: a comparison with NaN
// is false, so the "else" side is the bound.
double floorHz(double hz) {
	return hz > kMinHz ? hz : kMinHz;
}
double clamp01(double v) {
	return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0;
}
double atLeastOne(double v) {
	return v > 1.0 ? v : 1.0;
}
double vowelCentre(float hz) {
	return std::min(atLeastOne(static_cast<double>(hz)), kVowelMaxHz);
}

double smoothstep01(double x) {
	const double t = clamp01(x);
	return t * t * (3.0 - 2.0 * t);
}

double filterOrder(const FilterOp& f) {
	if (f.mode == FilterMode::Bell) {
		return bellSkirtOrder(static_cast<double>(f.slope)); // the classic bell at the default Slope (F2, by decision)
	}
	return 1.0 + 15.0 * clamp01(static_cast<double>(f.slope));
}

double trackedCutoff(const FilterOp& f, double f0Hz) {
	const double track = std::pow(floorHz(f0Hz) / kKeyTrackPivotHz, clamp01(static_cast<double>(f.keyTrack)));
	return atLeastOne(atLeastOne(static_cast<double>(f.cutoffHz)) * track);
}

double resonanceAmp(const FilterOp& f) {
	return f.mode == FilterMode::Notch || f.mode == FilterMode::Bell ? 0.0
	                                                                 : kMaxResonanceGain * clamp01(static_cast<double>(f.resonance));
}

// A Bell's Gain in dB, held to ±kBellMaxDb (a NaN is 0 dB: no bell).
double bellDb(const FilterOp& f) {
	const double db = static_cast<double>(f.gainDb);
	return std::isnan(db) ? 0.0 : std::clamp(db, -kBellMaxDb, kBellMaxDb);
}

// band b is harmonic b + 1. A NaN or sub-1 k reads band 0; a huge one reads the last band, and neither reaches
// the int cast.
float curveAt(const std::array<float, kCurveBands>& g, double k) {
	const double pos = k - 1.0;
	if (!(pos > 0.0)) {
		return g[0];
	}
	if (pos >= static_cast<double>(kCurveBands - 1)) {
		return g[kCurveBands - 1];
	}
	const int i = static_cast<int>(pos);
	const float frac = static_cast<float>(pos - i);
	return g[i] + (g[i + 1] - g[i]) * frac;
}

// A NaN harmonic index reads as harmonic 1 (it never reaches llround, whose NaN result is unspecified).
double harmonicIndex(double k) {
	return std::isnan(k) ? 1.0 : k;
}
bool isOddHarmonic(double k) {
	return (std::llround(harmonicIndex(k)) % 2) != 0;
}

// Tilt's exponent per octave, the slope clamped to ±kTiltMaxDbPerOctave (a NaN slope is no tilt).
double tiltExponentOf(const TiltOp& t) {
	const double db = static_cast<double>(t.dbPerOctave);
	return std::isnan(db) ? 0.0 : std::clamp(db, -kTiltMaxDbPerOctave, kTiltMaxDbPerOctave) / kDbPerOctaveOfUnitExponent;
}

double formantT(double log2Hz, double log2Crossover) {
	return smoothstep01((log2Hz - log2Crossover) / kFormantBlendWidthOctaves + 0.5);
}
} // namespace

// ---- The reference ---------------------------------------------------------------------------------------

double filterOpenness(FilterMode mode, double log2Control, double log2Corner) {
	const auto towardEnd = [](double distanceOctaves) { return std::clamp(1.0 - distanceOctaves / kFilterOpenOctaves, 0.0, 1.0); };
	double w = 0.0;
	if (mode == FilterMode::LowPass) {
		const double top = std::log2(kFilterOpenHighHz);
		w = std::max(towardEnd(top - log2Control), towardEnd(top - log2Corner));
	}
	else if (mode == FilterMode::HighPass) {
		const double bottom = std::log2(kFilterOpenLowHz);
		w = std::max(towardEnd(log2Control - bottom), towardEnd(log2Corner - bottom));
	}
	else if (mode == FilterMode::Notch) {
		// A NOTCH OPENS AT BOTH ENDS (Z9b, the other filter modes should get the same treatment): its dip leaves the band
		// either way, and at a gentle slope it is wide — at 6 dB/oct a notch at 20 kHz takes 10 kHz down by 14 dB.
		const double top = std::log2(kFilterOpenHighHz);
		const double bottom = std::log2(kFilterOpenLowHz);
		w = std::max({ towardEnd(top - log2Control), towardEnd(top - log2Corner), towardEnd(log2Control - bottom),
		               towardEnd(log2Corner - bottom) });
	}
	return w >= 1.0 - 1e-5 ? 1.0 : w;
}

double filterOpenness(FilterMode mode, double log2Control, double log2Corner, double gainDb) {
	if (mode == FilterMode::Bell) {
		FilterOp f {};
		f.mode = mode;
		f.gainDb = static_cast<float>(gainDb);
		return bellDb(f) == 0.0 ? 1.0 : 0.0;
	}
	return filterOpenness(mode, log2Control, log2Corner);
}

double filterOpenness(const FilterOp& f, double f0Hz) {
	// A BELL'S OPEN END IS 0 dB (F2): there it is exactly 1 everywhere, whatever its centre, width or skirt.
	if (f.mode == FilterMode::Bell) {
		return bellDb(f) == 0.0 ? 1.0 : 0.0;
	}
	return filterOpenness(f.mode, std::log2(atLeastOne(static_cast<double>(f.cutoffHz))), std::log2(trackedCutoff(f, f0Hz)));
}

double bellSkirtOrder(double slope) {
	return std::exp2(kBellSkirtOctaves * (clamp01(slope) - kBellClassicSlope) / (1.0 - kBellClassicSlope));
}

double bellHalfWidthOctaves(double resonance) {
	return 0.5 * kBellWidestOctaves * std::pow(kBellNarrowestOctaves / kBellWidestOctaves, clamp01(resonance));
}

double routedFilterGain(double s1, double s2, double p1, double p2, double r) {
	const double series = s1 * s2;
	if (!(r > 0.0)) {
		return series;
	}
	p1 = p1 > 0.0 ? std::min(p1, 1.0) : 1.0;
	p2 = p2 > 0.0 ? std::min(p2, 1.0) : 1.0;
	double par = 0.0;
	if (p1 == 1.0 && p2 == 1.0) {
		par = parallelGain(s1, s2);
	}
	else {
		const double g1 = std::pow(s1, 1.0 / p1);
		const double g2 = std::pow(s2, 1.0 / p2);
		par = std::pow(s1, 1.0 - p2) * std::pow(s2, 1.0 - p1) * std::pow(parallelGain(g1, g2), p1 * p2);
	}
	return r >= 1.0 ? par : std::pow(series, 1.0 - r) * std::pow(par, r);
}

float filterGain(const FilterOp& f, double hz, double f0Hz) {
	const double fc = trackedCutoff(f, f0Hz);
	const double x = floorHz(hz) / fc;
	const double order = filterOrder(f);
	if (f.mode == FilterMode::Bell) {
		const double d = std::abs(std::log2(x)) / bellHalfWidthOctaves(static_cast<double>(f.resonance));
		const double b = 1.0 / (1.0 + std::pow(d, 2.0 * order));
		return static_cast<float>(std::pow(10.0, bellDb(f) / 20.0 * b));
	}
	const double lp = 1.0 / std::sqrt(1.0 + std::pow(x, 2.0 * order));
	const double hp = 1.0 / std::sqrt(1.0 + std::pow(x, -2.0 * order));
	double g = 1.0;
	switch (f.mode) {
	case FilterMode::LowPass: g = lp; break;
	case FilterMode::HighPass: g = hp; break;
	case FilterMode::BandPass: g = 2.0 * lp * hp; break; // exactly 1 at the cutoff: lp·hp = 1/2 there
	case FilterMode::Notch: g = 1.0 - 2.0 * lp * hp; break;
	case FilterMode::Bell: break; // drawn above
	}
	const double res = clamp01(static_cast<double>(f.resonance));
	if (f.mode != FilterMode::Notch && res > 0.0) {
		const double octaves = std::log2(x);
		g += kMaxResonanceGain * res
		     * std::exp(-(octaves * octaves) / (2.0 * kResonanceWidthOctaves * kResonanceWidthOctaves));
	}
	return static_cast<float>(std::max(0.0, g));
}

float combGain(const CombOp& c, double k) {
	const double spacing = atLeastOne(static_cast<double>(c.spacing));
	const double teeth =
	    0.5 * (1.0 + std::cos(2.0 * std::numbers::pi * (harmonicIndex(k) / spacing + static_cast<double>(c.phase))));
	return static_cast<float>(1.0 - clamp01(static_cast<double>(c.depth)) * teeth);
}

float tiltGain(const TiltOp& t, double hz) {
	const double ratio = std::clamp(floorHz(hz) / atLeastOne(static_cast<double>(t.pivotHz)),
	                                std::exp2(-kTiltMaxOctaves), std::exp2(kTiltMaxOctaves));
	return static_cast<float>(std::pow(ratio, tiltExponentOf(t)));
}

float oddEvenGain(const OddEvenOp& o, double k) {
	const double b = std::clamp(static_cast<double>(o.balance), -1.0, 1.0);
	const bool odd = isOddHarmonic(k);
	return static_cast<float>(odd ? std::min(1.0, 1.0 - b) : std::min(1.0, 1.0 + b));
}

float curveGain(const DrawnCurve& c, double k) {
	return curveAt(c.gain, k);
}

float vowelGain(const VowelOp& v, double hz) {
	const double f = floorHz(hz);
	const double depth = clamp01(static_cast<double>(v.depth));
	const auto peak = [&](float centre) {
		const double c = vowelCentre(centre);
		// (f² − c²)/(f·B), written so an infinite f gives an infinite x (a peak of 0) rather than inf/inf.
		const double x = (f - c * c / f) / (kVowelBandwidthBaseHz + kVowelBandwidthPerHz * c);
		return 1.0 / (1.0 + x * x); // a resonance's power response: 1 at the centre, 1/2 at ±half its bandwidth
	};
	const double floorAndPeaks = 1.0 + kVowelPeakGain * (kVowelF1Level * peak(v.f1Hz) + kVowelF2Level * peak(v.f2Hz));
	return static_cast<float>(std::pow(floorAndPeaks, depth));
}

float gain(const ShaperParams& p, double k, double hz, double f0Hz) {
	if (!p.enabled) {
		return 1.0f;
	}
	hz = floorHz(hz);
	f0Hz = floorHz(f0Hz);
	// THE FILTER GROUP: the primary and the morph's further modes, each to its mix (a gain to a power fades it in dB). Each side's
	// product alone; with both acting they are routed (routedFilterGain), with one it is that side's product, the old road.
	bool acted[2] = { false, false };
	bool wired[2] = { false, false };
	const auto filtered = [&](const FilterOp& f, float& g, bool& acts, bool& wire) {
		const double open = f.on ? filterOpenness(f, f0Hz) : 1.0;
		if (open >= 1.0) {
			wire = wire || f.on; // on at its open end: a wire, which Parallel hears (below)
			return; // off, or at its open end: exactly transparent
		}
		acts = true;
		const double mix = clamp01(static_cast<double>(f.mix)) * (1.0 - open);
		const double fg = static_cast<double>(filterGain(f, hz, f0Hz));
		g *= static_cast<float>(mix == 1.0 ? fg : std::pow(fg, mix));
	};
	float side1 = 1.0f;
	float side2 = 1.0f;
	filtered(p.filter, side1, acted[0], wired[0]);
	for (const FilterOp& f : p.moreFilters) {
		filtered(f, side1, acted[0], wired[0]);
	}
	filtered(p.filter2, side2, acted[1], wired[1]);
	for (const FilterOp& f : p.moreFilters2) {
		filtered(f, side2, acted[1], wired[1]);
	}
	// A FILTER ON AT ITS OPEN END IS A WIRE, AND IN PARALLEL A WIRE BESIDE THE OTHER FILTER IS HEARD (additive design §3.6): the
	// side is present with a gain of exactly 1. In Series a wire is nothing, which is the old road.
	if (p.parallel > 0.0f && (acted[0] || acted[1])) {
		acted[0] = acted[0] || wired[0];
		acted[1] = acted[1] || wired[1];
	}
	float g = 1.0f;
	if (acted[0] && acted[1]) {
		g = static_cast<float>(routedFilterGain(static_cast<double>(side1), static_cast<double>(side2),
		                                        static_cast<double>(p.filterPresence[0]), static_cast<double>(p.filterPresence[1]),
		                                        clamp01(static_cast<double>(p.parallel))));
	}
	else {
		g *= acted[1] ? side2 : side1;
	}
	if (p.comb.on) {
		g *= combGain(p.comb, k);
	}
	if (p.tilt.on) {
		g *= tiltGain(p.tilt, hz);
	}
	if (p.oddEven.on) {
		g *= oddEvenGain(p.oddEven, k);
	}
	if (p.curve.on) {
		g *= curveGain(p.curve, k);
	}
	if (p.vowel.on) {
		g *= vowelGain(p.vowel, hz);
	}
	return g;
}

double formantRatio(const ShaperParams& p, double hz) {
	if (!p.enabled || !p.formantShift.on) {
		return 1.0;
	}
	const FormantShiftOp& f = p.formantShift;
	const double low = std::exp2(static_cast<double>(f.lowSemitones) / 12.0);
	const double high = f.link ? low : std::exp2(static_cast<double>(f.highSemitones) / 12.0);
	if (low == high) {
		return low;
	}
	const double t = formantT(std::log2(floorHz(hz)), std::log2(atLeastOne(static_cast<double>(f.crossoverHz))));
	return std::pow(low, 1.0 - t) * std::pow(high, t); // a geometric blend: the ratio is a multiplier
}


// ---- The prepared batch ----------------------------------------------------------------------------------

PreparedShaper prepare(const ShaperParams& p, double f0Hz) {
	PreparedShaper s {};
	if (!p.enabled) {
		return s; // both identity flags stand
	}
	// A FILTER AT ITS OPEN END IS OFF (`filterOpenness`): it neither costs nor colours.
	const auto acting = [&](const FilterOp& f) { return f.on && filterOpenness(f, f0Hz) < 1.0; };
	bool side1Acts = acting(p.filter);
	for (const FilterOp& f : p.moreFilters) {
		side1Acts = side1Acts || acting(f);
	}
	bool side2Acts = acting(p.filter2);
	for (const FilterOp& f : p.moreFilters2) {
		side2Acts = side2Acts || acting(f);
	}
	const bool anyFilter = side1Acts || side2Acts;
	// AND A FILTER ON AT ITS OPEN END, A WIRE: in Parallel it is present beside an acting filter on the other side (gain()).
	const auto wire = [&](const FilterOp& f) { return f.on && !acting(f); };
	bool side1Wire = wire(p.filter);
	for (const FilterOp& f : p.moreFilters) {
		side1Wire = side1Wire || wire(f);
	}
	bool side2Wire = wire(p.filter2);
	for (const FilterOp& f : p.moreFilters2) {
		side2Wire = side2Wire || wire(f);
	}
	const bool vowelOn = p.vowel.on && clamp01(static_cast<double>(p.vowel.depth)) > 0.0;
	s.identity = !(anyFilter || p.comb.on || p.tilt.on || p.oddEven.on || p.curve.on || vowelOn);
	s.formantIdentity = !p.formantShift.on;

	// An operator that is off keeps its group's defaults (the header: they are GainCache's keys).
	// THE FILTERS THAT ARE ON, PACKED: the first is `filter`, the rest the morph's further modes (their product is the
	// group's factor, so the order is free).
	const auto prepared = [&](const FilterOp& f) {
		PreparedShaper::Filter out;
		out.on = true;
		out.mode = f.mode;
		out.log2Cutoff = std::log2(trackedCutoff(f, f0Hz));
		out.twoOrder = 2.0 * filterOrder(f);
		out.resonanceAmp = resonanceAmp(f);
		// FADING OUT TOWARD AN OPEN END: 1 - openness of its mix. Exactly the mix away from the ends (1 - 0), so nothing moves there.
		out.mix = clamp01(static_cast<double>(f.mix)) * (1.0 - filterOpenness(f, f0Hz));
		if (f.mode == FilterMode::Bell) {
			out.bellLog2Gain = bellDb(f) / kDbPerOctaveOfUnitExponent;
			out.bellInvHalf = 1.0 / bellHalfWidthOctaves(static_cast<double>(f.resonance));
		}
		return out;
	};
	// THE FIRST SIDE IS WHICHEVER ACTS FIRST: with filter 1 silent, filter 2 alone is packed where filter 1 would be, so one
	// acting filter is the old road wherever it came from; the second side exists only while both act.
	const auto place = [&](const FilterOp& f, bool second) {
		if (!acting(f)) {
			return;
		}
		if (!s.filter.on) {
			s.filter = prepared(f);
		}
		else if (s.moreFilterCount + s.side2Count < static_cast<int>(s.moreFilters.size())) {
			s.moreFilters[static_cast<std::size_t>(s.moreFilterCount + s.side2Count)] = prepared(f);
			++(second ? s.side2Count : s.moreFilterCount);
		}
	};
	const bool parallelWires = p.parallel > 0.0f && anyFilter;
	const bool routed = parallelWires ? (side1Acts || side1Wire) && (side2Acts || side2Wire) : side1Acts && side2Acts;
	// A SIDE PRESENT ONLY AS A WIRE gets one filter of exponent 0 (its gain is exactly 1), so the routing sees it.
	const auto placeWire = [&](const FilterOp& f, bool second) {
		if (!wire(f)) {
			return false;
		}
		PreparedShaper::Filter w = prepared(f);
		w.mix = 0.0;
		if (!s.filter.on) {
			s.filter = w;
		}
		else {
			s.moreFilters[static_cast<std::size_t>(s.moreFilterCount + s.side2Count)] = w;
			++(second ? s.side2Count : s.moreFilterCount);
		}
		return true;
	};
	const auto placeSideWire = [&](const FilterOp& primary, const auto& more, bool second) {
		if (placeWire(primary, second)) {
			return;
		}
		for (const FilterOp& f : more) {
			if (placeWire(f, second)) {
				return;
			}
		}
	};
	if (routed && !side1Acts) {
		placeSideWire(p.filter, p.moreFilters, false);
	}
	place(p.filter, false);
	for (const FilterOp& f : p.moreFilters) {
		place(f, false);
	}
	if (routed && !side2Acts) {
		placeSideWire(p.filter2, p.moreFilters2, true);
	}
	place(p.filter2, routed);
	for (const FilterOp& f : p.moreFilters2) {
		place(f, routed);
	}
	if (routed) {
		const auto presence = [](float v) {
			const double d = static_cast<double>(v);
			return d > 0.0 ? std::min(d, 1.0) : 1.0; // a NaN or non-positive presence is a whole filter
		};
		s.parallel = clamp01(static_cast<double>(p.parallel));
		s.presence1 = presence(p.filterPresence[0]);
		s.presence2 = presence(p.filterPresence[1]);
	}
	if (p.comb.on) {
		s.harmonic.combOn = true;
		s.harmonic.combInvSpacing = 1.0 / atLeastOne(static_cast<double>(p.comb.spacing));
		const double phase = static_cast<double>(p.comb.phase);
		// Reduced into [0, 1] (the comb is periodic in it). The phase is a float, so the reduction is exact unless the phase
		// is negative and within ~2^-30 of zero, where it rounds by less than that (and may land on 1, which is 0).
		// A NaN or infinite phase becomes NaN, as cos would make it.
		s.harmonic.combPhase = phase - std::floor(phase);
		s.harmonic.combDepth = clamp01(static_cast<double>(p.comb.depth));
	}
	if (p.oddEven.on) {
		s.harmonic.oddEvenOn = true;
		s.harmonic.oddGain = static_cast<double>(oddEvenGain(p.oddEven, 1.0));
		s.harmonic.evenGain = static_cast<double>(oddEvenGain(p.oddEven, 2.0));
	}
	if (p.curve.on) {
		s.harmonic.curveOn = true;
		s.harmonic.curve = p.curve.gain;
	}
	if (p.tilt.on) {
		s.tilt.on = true;
		s.tilt.exponent = tiltExponentOf(p.tilt);
		s.tilt.log2Pivot = std::log2(atLeastOne(static_cast<double>(p.tilt.pivotHz)));
	}
	if (vowelOn) {
		s.vowel.on = true;
		s.vowel.f1 = vowelCentre(p.vowel.f1Hz);
		s.vowel.f2 = vowelCentre(p.vowel.f2Hz);
		s.vowel.inv1 = 1.0 / (kVowelBandwidthBaseHz + kVowelBandwidthPerHz * s.vowel.f1);
		s.vowel.inv2 = 1.0 / (kVowelBandwidthBaseHz + kVowelBandwidthPerHz * s.vowel.f2);
		s.vowel.depth = clamp01(static_cast<double>(p.vowel.depth));
	}

	const FormantShiftOp& f = p.formantShift;
	s.formantLowSemitones = static_cast<double>(f.lowSemitones);
	s.formantHighSemitones = f.link ? s.formantLowSemitones : static_cast<double>(f.highSemitones);
	s.formantConstant = s.formantLowSemitones == s.formantHighSemitones;
	s.formantConstantRatio = static_cast<float>(std::exp2(s.formantLowSemitones / 12.0));
	s.formantLog2Crossover = std::log2(atLeastOne(static_cast<double>(f.crossoverHz)));
	return s;
}

namespace {


// ---- Two partials per vector -------------------------------------------------------------------------------
//
// The polynomials, each bounded where it is evaluated (truncation, before rounding; rounding adds a few ulp):
//   exp2Pd: 2^f for f in (−1, 1) by Taylor's series of e^(f·ln 2) to degree 11. The first omitted term is
//           (ln 2)^12/12! = 2.8e-11 relative.
//   log2Pd: ln m for m in [√½, √2] as 2·atanh(s), s = (m − 1)/(m + 1), |s| <= 0.1716, through s^11. The first
//           omitted term is s^13/13 against s: 0.1716^12/13 = 5.0e-11 relative (so ~2.5e-11 absolute in log2).
//   cos2PiPd: cos x for x in [0, π/2] by Taylor's series through x^18. The first omitted term is (π/2)^20/20! = 3.5e-15,
//           ABSOLUTE, which is what Comb's teeth need: 1 − depth·(1 + cos)/2 cancels to nothing at a tooth.
// The filter multiplies log2's error by its 2·order (<= 32): ~8e-10 in the exponent, so ~6e-10 relative in its gain.
// So each factor is the reference's to ~1e-9 relative wherever it is not flushed, and the tests hold it to that in
// dB (test_spectral_shaper.cpp, "the kernels are the reference ...").
//
// EVALUATED BY ESTRIN'S SCHEME, not Horner's: the kernels are bound by the LATENCY of one partial's chain (log2, then
// exp2, then a square root and a division), and Estrin's pairs evaluate side by side, so the chain is ~log2(degree)
// multiply-adds deep instead of degree. Measured on the filter kernel: tests/bench_spectral_shaper.cpp.

constexpr double kLog2E = 1.4426950408889634; // 1 / ln 2

constexpr int kExp2Degree = 11;
constexpr int kLog2TopOdd = 11; ///< the series runs s, s^3/3, .., s^kLog2TopOdd/kLog2TopOdd
constexpr int kCosTerms = 10;   ///< x^0 .. x^18

// Taylor's coefficients (ln 2)^n / n!, n = 0..kExp2Degree.
constexpr std::array<double, kExp2Degree + 1> kExp2Taylor = [] {
	std::array<double, kExp2Degree + 1> c {};
	double term = 1.0;
	for (int n = 0; n <= kExp2Degree; ++n) {
		c[static_cast<std::size_t>(n)] = term;
		term *= std::numbers::ln2 / static_cast<double>(n + 1);
	}
	return c;
}();

// 1/(2j + 1), j = 0..: the atanh series in s² (1, 1/3, 1/5, ..).
constexpr std::array<double, (kLog2TopOdd + 1) / 2> kAtanhSeries = [] {
	std::array<double, (kLog2TopOdd + 1) / 2> c {};
	for (std::size_t j = 0; j < c.size(); ++j) {
		c[j] = 1.0 / static_cast<double>(2 * j + 1);
	}
	return c;
}();

// (−1)^j / (2j)!, j = 0..kCosTerms − 1.
constexpr std::array<double, kCosTerms> kCosTaylor = [] {
	std::array<double, kCosTerms> c {};
	double term = 1.0;
	for (int j = 0; j < kCosTerms; ++j) {
		c[static_cast<std::size_t>(j)] = term;
		term = -term / static_cast<double>((2 * j + 1) * (2 * j + 2));
	}
	return c;
}();

/// Σ c[i]·x^i by Estrin's scheme: neighbouring coefficients paired as c[i] + x·c[i+1], then those paired with x², and
/// so on, every pairing at one level independent of the others.
template <std::size_t N>
inline __m128d estrin(const std::array<double, N>& c, __m128d x) {
	__m128d v[N];
	for (std::size_t i = 0; i < N; ++i) {
		v[i] = _mm_set1_pd(c[i]);
	}
	std::size_t n = N;
	__m128d power = x;
	while (n > 1) {
		std::size_t m = 0;
		for (std::size_t i = 0; i < n; i += 2) {
			v[m++] = i + 1 < n ? _mm_add_pd(v[i], _mm_mul_pd(power, v[i + 1])) : v[i];
		}
		n = m;
		power = _mm_mul_pd(power, power);
	}
	return v[0];
}

inline __m128d selectPd(__m128d mask, __m128d a, __m128d b) {
	return _mm_or_pd(_mm_and_pd(mask, a), _mm_andnot_pd(mask, b));
}

/// floor(x) for |x| < 2^31, independent of the MXCSR rounding mode: truncate, then step down the lanes truncation
/// moved up (negative non-integers).
inline __m128d floorPd(__m128d x) {
	const __m128d tf = _mm_cvtepi32_pd(_mm_cvttpd_epi32(x));
	return _mm_sub_pd(tf, _mm_and_pd(_mm_cmpgt_pd(tf, x), _mm_set1_pd(1.0)));
}

/// 2^x for x in [−1020, 1020] (the callers clamp). x = n + f with n = trunc(x), so f is in (−1, 1) and exact, without
/// the rounding mode and without the floor's correction on the chain.
inline __m128d exp2Pd(__m128d x) {
	const __m128i n = _mm_cvttpd_epi32(x);
	const __m128d f = _mm_sub_pd(x, _mm_cvtepi32_pd(n));
	const __m128d p = estrin(kExp2Taylor, f);
	// 2^n as a double's bits: (n + 1023) << 52, the int32 lanes widened to 64.
	const __m128i biased = _mm_unpacklo_epi32(_mm_add_epi32(n, _mm_set1_epi32(1023)), _mm_setzero_si128());
	return _mm_mul_pd(p, _mm_castsi128_pd(_mm_slli_epi64(biased, 52)));
}

/// log2 x for x positive and normal, or +inf (which reads as 1024: every caller clamps far below that).
inline __m128d log2Pd(__m128d x) {
	const __m128i bits = _mm_castpd_si128(x);
	// The biased exponent (the sign bit is 0), each 64-bit lane's low half into int32 lanes 0 and 1.
	const __m128i expBits = _mm_shuffle_epi32(_mm_srli_epi64(bits, 52), _MM_SHUFFLE(3, 3, 2, 0));
	__m128d e = _mm_sub_pd(_mm_cvtepi32_pd(expBits), _mm_set1_pd(1023.0));
	// The mantissa as m in [1, 2), then into [√½, √2] so s stays small.
	__m128d m = _mm_castsi128_pd(_mm_or_si128(_mm_and_si128(bits, _mm_set1_epi64x(0x000FFFFFFFFFFFFFLL)),
	                                          _mm_set1_epi64x(0x3FF0000000000000LL)));
	const __m128d big = _mm_cmpgt_pd(m, _mm_set1_pd(std::numbers::sqrt2));
	m = selectPd(big, _mm_mul_pd(m, _mm_set1_pd(0.5)), m);
	e = _mm_add_pd(e, _mm_and_pd(big, _mm_set1_pd(1.0)));
	const __m128d one = _mm_set1_pd(1.0);
	const __m128d s = _mm_div_pd(_mm_sub_pd(m, one), _mm_add_pd(m, one));
	const __m128d ln = _mm_mul_pd(_mm_add_pd(s, s), estrin(kAtanhSeries, _mm_mul_pd(s, s)));
	return _mm_add_pd(e, _mm_mul_pd(ln, _mm_set1_pd(kLog2E)));
}

/// cos(2π·u) for |u| < 2^30: u reduced to t in [−0.5, 0.5] (exact), then a = |t| folded to [0, 0.25] about 0.25 with
/// the sign it costs, then the Taylor polynomial at x = 2π·a in [0, π/2].
inline __m128d cos2PiPd(__m128d u) {
	const __m128d t = _mm_sub_pd(u, floorPd(_mm_add_pd(u, _mm_set1_pd(0.5))));
	const __m128d a = _mm_andnot_pd(_mm_set1_pd(-0.0), t);
	const __m128d past = _mm_cmpgt_pd(a, _mm_set1_pd(0.25));
	const __m128d b = selectPd(past, _mm_sub_pd(_mm_set1_pd(0.5), a), a);
	const __m128d x = _mm_mul_pd(b, _mm_set1_pd(2.0 * std::numbers::pi));
	const __m128d c = estrin(kCosTaylor, _mm_mul_pd(x, x));
	return _mm_xor_pd(c, _mm_and_pd(past, _mm_set1_pd(-0.0)));
}

/// Two floats at p (one, and a zero beside it, when only one is left) as doubles.
inline __m128d load2(const float* p, int left) {
	if (left >= 2) {
		return _mm_cvtps_pd(_mm_castsi128_ps(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(p))));
	}
	return _mm_set_pd(0.0, static_cast<double>(p[0]));
}

inline void store2(float* p, int left, __m128d v) {
	alignas(16) double d[2];
	_mm_store_pd(d, v);
	p[0] = static_cast<float>(d[0]);
	if (left >= 2) {
		p[1] = static_cast<float>(d[1]);
	}
}

inline void store2(double* p, int left, __m128d v) {
	if (left >= 2) {
		_mm_storeu_pd(p, v);
	}
	else {
		_mm_store_sd(p, v);
	}
}

/// hz floored at kMinHz, a NaN onto the floor too (_mm_max_pd returns its SECOND operand when either is NaN).
inline __m128d floorHzPd(__m128d hz) {
	return _mm_max_pd(hz, _mm_set1_pd(kMinHz));
}

/// The harmonic group at one partial, by the formulas as the reference writes them: the road for a k too large for
/// the vector road's integer conversions (|k| >= 2^30), where no real partial lives.
double harmonicScalar(const PreparedShaper::Harmonic& h, double k) {
	const double kk = harmonicIndex(k);
	double g = 1.0;
	if (h.combOn) {
		g *= 1.0 - h.combDepth * 0.5 * (1.0 + std::cos(2.0 * std::numbers::pi * (kk * h.combInvSpacing + h.combPhase)));
	}
	if (h.oddEvenOn) {
		g *= isOddHarmonic(kk) ? h.oddGain : h.evenGain;
	}
	if (h.curveOn) {
		g *= static_cast<double>(curveAt(h.curve, kk));
	}
	return g;
}

/// The harmonic group for one pair of indices kk (NaN already read as 1).
inline __m128d harmonicPair(const PreparedShaper::Harmonic& h, __m128d kk) {
	__m128d g = _mm_set1_pd(1.0);
	if (h.combOn) {
		const __m128d u = _mm_add_pd(_mm_mul_pd(kk, _mm_set1_pd(h.combInvSpacing)), _mm_set1_pd(h.combPhase));
		const __m128d teeth = _mm_mul_pd(_mm_set1_pd(0.5), _mm_add_pd(_mm_set1_pd(1.0), cos2PiPd(u)));
		g = _mm_sub_pd(g, _mm_mul_pd(_mm_set1_pd(h.combDepth), teeth));
	}
	if (h.oddEvenOn) {
		// llround's parity: |k| + 0.5 truncated is |k| rounded half away from zero (exact for a k a float can hold).
		const __m128i r = _mm_cvttpd_epi32(_mm_add_pd(_mm_andnot_pd(_mm_set1_pd(-0.0), kk), _mm_set1_pd(0.5)));
		const __m128i odd32 = _mm_cmpeq_epi32(_mm_and_si128(r, _mm_set1_epi32(1)), _mm_set1_epi32(1));
		const __m128d odd = _mm_castsi128_pd(_mm_shuffle_epi32(odd32, _MM_SHUFFLE(1, 1, 0, 0)));
		g = _mm_mul_pd(g, selectPd(odd, _mm_set1_pd(h.oddGain), _mm_set1_pd(h.evenGain)));
	}
	if (h.curveOn) {
		alignas(16) double k2[2];
		_mm_store_pd(k2, kk);
		g = _mm_mul_pd(g, _mm_set_pd(static_cast<double>(curveAt(h.curve, k2[1])), static_cast<double>(curveAt(h.curve, k2[0]))));
	}
	return g;
}

// The flush: below −140 dB the partial is silent, and exactly 0 lets the bank skip it. A NaN flushes too.
float flushed(double g) {
	return g >= static_cast<double>(kFlushGain) ? static_cast<float>(g) : 0.0f;
}
} // namespace

void harmonicFactors(const PreparedShaper::Harmonic& h, const float* k, int count, float* out) {
	for (int i = 0; i < count; i += 2) {
		const int left = count - i;
		__m128d kk = load2(k + i, left);
		kk = selectPd(_mm_cmpord_pd(kk, kk), kk, _mm_set1_pd(1.0)); // a NaN index reads as harmonic 1
		const __m128d inRange = _mm_cmplt_pd(_mm_andnot_pd(_mm_set1_pd(-0.0), kk), _mm_set1_pd(1073741824.0));
		const int mask = _mm_movemask_pd(inRange);
		// The pair runs with any out-of-range lane read as harmonic 1, then ONLY that lane is overwritten from the scalar
		// road, so a lane's value never depends on its neighbour (the header: elementwise in value).
		store2(out + i, left, harmonicPair(h, selectPd(inRange, kk, _mm_set1_pd(1.0))));
		if ((mask & 1) == 0) {
			out[i] = static_cast<float>(harmonicScalar(h, static_cast<double>(k[i])));
		}
		if (left >= 2 && (mask & 2) == 0) {
			out[i + 1] = static_cast<float>(harmonicScalar(h, static_cast<double>(k[i + 1])));
		}
	}
}

void harmonicFactorsAtIndex(const PreparedShaper::Harmonic& h, int count, float* out) {
	__m128d kk = _mm_set_pd(2.0, 1.0);
	for (int i = 0; i < count; i += 2) {
		store2(out + i, count - i, harmonicPair(h, kk)); // i + 1 <= 2^30 for any count an int holds below that
		kk = _mm_add_pd(kk, _mm_set1_pd(2.0));
	}
}

namespace {
/// log2 of two partials' frequencies, floored as the reference floors them.
inline __m128d log2HzPair(const float* hz, int left) {
	return log2Pd(floorHzPd(load2(hz, left)));
}

/// The filter's gain for two partials at log2 frequency L.
inline __m128d filterPair(const PreparedShaper::Filter& f, __m128d L) {
	const __m128d one = _mm_set1_pd(1.0);
	const __m128d d = _mm_sub_pd(L, _mm_set1_pd(f.log2Cutoff)); // octaves from the cutoff
	const __m128d e = _mm_min_pd(_mm_max_pd(_mm_mul_pd(_mm_set1_pd(f.twoOrder), d), _mm_set1_pd(-kFilterMaxExponent)),
	                             _mm_set1_pd(kFilterMaxExponent));
	if (f.mode == FilterMode::Bell) {
		// b = 1 / (1 + (|d|/h)^(2n)), the power by exp2(2n · log2(|d|/h)) with |d|/h held above 2^−1000 (log2Pd wants a normal
		// number; the bell is its full A there) and the exponent at ±kFilterMaxExponent; then A^b = 2^(b · log2 A).
		const __m128d u = _mm_max_pd(_mm_mul_pd(_mm_andnot_pd(_mm_set1_pd(-0.0), d), _mm_set1_pd(f.bellInvHalf)),
		                             _mm_set1_pd(0x1p-1000));
		const __m128d pe = _mm_min_pd(_mm_max_pd(_mm_mul_pd(_mm_set1_pd(f.twoOrder), log2Pd(u)), _mm_set1_pd(-kFilterMaxExponent)),
		                              _mm_set1_pd(kFilterMaxExponent));
		const __m128d b = _mm_div_pd(one, _mm_add_pd(one, exp2Pd(pe)));
		return exp2Pd(_mm_mul_pd(b, _mm_set1_pd(f.bellLog2Gain)));
	}
	const __m128d y = exp2Pd(e);
	const __m128d onePlusY = _mm_add_pd(one, y);
	__m128d g;
	switch (f.mode) {
	case FilterMode::LowPass: g = _mm_div_pd(one, _mm_sqrt_pd(onePlusY)); break;
	case FilterMode::HighPass: g = _mm_sqrt_pd(_mm_div_pd(y, onePlusY)); break;
	case FilterMode::BandPass: g = _mm_div_pd(_mm_mul_pd(_mm_set1_pd(2.0), _mm_sqrt_pd(y)), onePlusY); break; // 2·lp·hp
	case FilterMode::Notch:
	default: g = _mm_sub_pd(one, _mm_div_pd(_mm_mul_pd(_mm_set1_pd(2.0), _mm_sqrt_pd(y)), onePlusY)); break;
	}
	// THE BUMP IS SKIPPED WHERE IT IS BELOW THE FILTER'S OWN ROUNDING: at |d| >= 2 octaves it is at most
	// 3·2^(−d²·log2 e/2w²) = 3·2^(−32.06·d²), while the filter's gain there is at least 2^(−16·|d| − 1/2) (order 16, the
	// steepest) or 2^−500 (the exponent's hold), so the bump is below 2^−94 of the gain. Both lanes, or neither.
	if (f.resonanceAmp > 0.0 && _mm_movemask_pd(_mm_cmplt_pd(_mm_andnot_pd(_mm_set1_pd(-0.0), d), _mm_set1_pd(2.0))) != 0) {
		// e^(−d²/2w²) as 2^(−d²·log2 e/2w²), held at 2^−1020 (below that the bump is nothing a gain can show).
		const __m128d x = _mm_max_pd(_mm_mul_pd(_mm_mul_pd(d, d), _mm_set1_pd(-kResonanceInvTwoWidthSq * kLog2E)),
		                             _mm_set1_pd(-1020.0));
		g = _mm_add_pd(g, _mm_mul_pd(_mm_set1_pd(f.resonanceAmp), exp2Pd(x)));
	}
	return _mm_max_pd(g, _mm_setzero_pd()); // floored at 0 (a NaN too: max returns its second operand)
}

/// Tilt's uncompensated factor for two partials at log2 frequency L.
inline __m128d tiltPair(const PreparedShaper::Tilt& t, __m128d L) {
	const __m128d octaves = _mm_sub_pd(L, _mm_set1_pd(t.log2Pivot));
	const __m128d held = _mm_min_pd(_mm_max_pd(octaves, _mm_set1_pd(-kTiltMaxOctaves)), _mm_set1_pd(kTiltMaxOctaves));
	return exp2Pd(_mm_mul_pd(_mm_set1_pd(t.exponent), held));
}
} // namespace

namespace {
/// A filter's gain to its mix: the exponent fades its curve in dB. Skipped at mix 1, so a lone filter is the old road.
void applyMix(double mix, double* out, int count) {
	if (mix == 1.0) {
		return;
	}
	for (int i = 0; i < count; ++i) {
		out[i] = std::pow(out[i], mix);
	}
}
} // namespace

void filterFactors(const PreparedShaper::Filter& f, const float* hz, int count, double* out) {
	for (int i = 0; i < count; i += 2) {
		store2(out + i, count - i, filterPair(f, log2HzPair(hz + i, count - i)));
	}
	applyMix(f.mix, out, count);
}

void filterGroupFactors(const PreparedShaper& s, const float* hz, int count, double* out) {
	filterFactors(s.filter, hz, count, out);
	alignas(16) double more[64];
	for (int m = 0; m < s.moreFilterCount; ++m) {
		for (int i0 = 0; i0 < count; i0 += 64) {
			const int n = std::min(64, count - i0);
			filterFactors(s.moreFilters[static_cast<std::size_t>(m)], hz + i0, n, more);
			for (int i = 0; i < n; ++i) {
				out[i0 + i] *= more[i];
			}
		}
	}
	if (s.side2Count == 0) {
		return;
	}
	// THE SECOND SIDE'S PRODUCT, then routed against the first's (Series: the product, the old road's arithmetic).
	alignas(16) double side2[64];
	for (int i0 = 0; i0 < count; i0 += 64) {
		const int n = std::min(64, count - i0);
		for (int m = 0; m < s.side2Count; ++m) {
			double* into = m == 0 ? side2 : more;
			filterFactors(s.moreFilters[static_cast<std::size_t>(s.moreFilterCount + m)], hz + i0, n, into);
			if (m > 0) {
				for (int i = 0; i < n; ++i) {
					side2[i] *= more[i];
				}
			}
		}
		for (int i = 0; i < n; ++i) {
			out[i0 + i] = routedFilterGain(out[i0 + i], side2[i], s.presence1, s.presence2, s.parallel);
		}
	}
}

void tiltFactors(const PreparedShaper::Tilt& t, const float* hz, int count, float* out) {
	for (int i = 0; i < count; i += 2) {
		store2(out + i, count - i, tiltPair(t, log2HzPair(hz + i, count - i)));
	}
}

void vowelFactors(const PreparedShaper::Vowel& v, const float* hz, int count, float* out) {
	const __m128d one = _mm_set1_pd(1.0);
	// One formant's peak, 1 / (1 + x²) with x = (f − c²/f) / B: the reference's arithmetic (vowelGain), the division by B
	// taken as a multiply by its prepared inverse.
	const auto peak = [&](__m128d f, double c, double inv) {
		const __m128d x = _mm_mul_pd(_mm_sub_pd(f, _mm_div_pd(_mm_set1_pd(c * c), f)), _mm_set1_pd(inv));
		return _mm_div_pd(one, _mm_add_pd(one, _mm_mul_pd(x, x)));
	};
	for (int i = 0; i < count; i += 2) {
		const __m128d f = floorHzPd(load2(hz + i, count - i));
		const __m128d peaks = _mm_add_pd(_mm_mul_pd(_mm_set1_pd(kVowelPeakGain * kVowelF1Level), peak(f, v.f1, v.inv1)),
		                                 _mm_mul_pd(_mm_set1_pd(kVowelPeakGain * kVowelF2Level), peak(f, v.f2, v.inv2)));
		// V^depth as 2^(depth·log2 V): V is in [1, ~13], so the exponent is in [0, ~3.7], well inside exp2Pd's range.
		store2(out + i, count - i, exp2Pd(_mm_mul_pd(_mm_set1_pd(v.depth), log2Pd(_mm_add_pd(one, peaks)))));
	}
}

void filterTiltFactors(const PreparedShaper::Filter& f, const PreparedShaper::Tilt& t, const float* hz, int count,
                       double* filterOut, float* tiltOut) {
	for (int i = 0; i < count; i += 2) {
		const __m128d L = log2HzPair(hz + i, count - i);
		store2(filterOut + i, count - i, filterPair(f, L));
		store2(tiltOut + i, count - i, tiltPair(t, L));
	}
	applyMix(f.mix, filterOut, count);
}

void factorProduct(const float* h, const double* f, const float* t, const float* v, const float* base, int count,
                   float* out, double& powerBefore, double& powerAfter) {
	if (t == nullptr && v == nullptr) {
		for (int i = 0; i < count; ++i) {
			const double g = (h != nullptr ? static_cast<double>(h[i]) : 1.0) * (f != nullptr ? f[i] : 1.0);
			out[i] = flushed(g);
		}
		return;
	}
	// One running sum in partial order, continued from the caller's: gains()' chunks and GainCache's single call then
	// add the same terms in the same order, so the two roads' compensation is the same to the bit.
	double before2 = powerBefore;
	double after2 = powerAfter;
	for (int i = 0; i < count; ++i) {
		const double g = (h != nullptr ? static_cast<double>(h[i]) : 1.0) * (f != nullptr ? f[i] : 1.0);
		// The compensated factor: Tilt's, times the vowel's. Either alone is its own factor to the bit (× 1.0 is exact).
		const double tt = (t != nullptr ? static_cast<double>(t[i]) : 1.0) * (v != nullptr ? static_cast<double>(v[i]) : 1.0);
		const double before = static_cast<double>(base[i]) * g;
		const double after = before * tt;
		before2 += before * before;
		after2 += after * after;
		out[i] = static_cast<float>(g * tt);
	}
	powerBefore = before2;
	powerAfter = after2;
}

void exp2Scaled(const float* q, double scale, int count, float* out) {
	const __m128d s = _mm_set1_pd(scale);
	for (int i = 0; i < count; i += 2) {
		const __m128d x = _mm_mul_pd(s, load2(q + i, count - i));
		// Held to exp2Pd's range; a NaN lands on the floor (max returns its second operand), an exact 0 out.
		const __m128d held = _mm_min_pd(_mm_max_pd(x, _mm_set1_pd(-1020.0)), _mm_set1_pd(1020.0));
		store2(out + i, count - i, exp2Pd(held));
	}
}

void compensate(float* out, int count, double powerBefore, double powerAfter) {
	double c = std::sqrt(powerBefore / powerAfter);
	if (!(powerAfter > 0.0) || !std::isfinite(c)) {
		c = 1.0;
	}
	for (int i = 0; i < count; ++i) {
		out[i] = flushed(static_cast<double>(out[i]) * c);
	}
}

void gains(const PreparedShaper& s, const float* k, const float* hz, const float* base, int count, float* out) {
	if (s.identity) {
		std::fill(out, out + count, 1.0f);
		return;
	}
	// IN CHUNKS, so any count runs on a fixed scratch: each chunk's k and hz are read into the factors before its out is
	// written, and base[i] before out[i], so out may alias any of them (the header).
	constexpr int kChunk = 64;
	alignas(16) float h[kChunk];
	alignas(16) double f[kChunk];
	alignas(16) float t[kChunk];
	alignas(16) float v[kChunk];
	const bool harmonic = s.harmonic.any();
	double powerBefore = 0.0;
	double powerAfter = 0.0;
	for (int i0 = 0; i0 < count; i0 += kChunk) {
		const int n = std::min(kChunk, count - i0);
		if (harmonic) {
			harmonicFactors(s.harmonic, k + i0, n, h);
		}
		if (s.singleFilter() && s.tilt.on) {
			filterTiltFactors(s.filter, s.tilt, hz + i0, n, f, t);
		}
		else {
			if (s.filter.on) {
				filterGroupFactors(s, hz + i0, n, f);
			}
			if (s.tilt.on) {
				tiltFactors(s.tilt, hz + i0, n, t);
			}
		}
		if (s.vowel.on) {
			vowelFactors(s.vowel, hz + i0, n, v);
		}
		factorProduct(harmonic ? h : nullptr, s.filter.on ? f : nullptr, s.tilt.on ? t : nullptr, s.vowel.on ? v : nullptr,
		              s.compensated() ? base + i0 : nullptr, n, out + i0, powerBefore, powerAfter);
	}
	if (s.compensated()) {
		compensate(out, count, powerBefore, powerAfter);
	}
}

void formantRatios(const PreparedShaper& s, const float* hz, int count, float* out) {
	if (s.formantIdentity) {
		std::fill(out, out + count, 1.0f);
		return;
	}
	if (s.formantConstant) {
		std::fill(out, out + count, s.formantConstantRatio);
		return;
	}
	const double span = s.formantHighSemitones - s.formantLowSemitones;
	for (int i = 0; i < count; ++i) {
		const double t = formantT(std::log2(floorHz(static_cast<double>(hz[i]))), s.formantLog2Crossover);
		// One exp2 in semitones: 2^(lerp(low, high, t)/12) == pow(low ratio, 1−t)·pow(high ratio, t).
		out[i] = static_cast<float>(std::exp2((s.formantLowSemitones + span * t) / 12.0));
	}
}

} // namespace feathers::spectral
