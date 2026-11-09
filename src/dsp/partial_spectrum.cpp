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

#include "dsp/partial_spectrum.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::partials {

namespace {
constexpr double kStep = (kEnvelopeLog2Max - kEnvelopeLog2Min) / (kEnvelopePoints - 1);

double smoothstep01(double x) {
	const double t = std::clamp(x, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

double formantTerm(double log2k, float centre, float width, float gain) {
	if (gain <= 0.0f) {
		return 0.0;
	}
	const double c = 1.0 + 5.0 * static_cast<double>(centre); // log2 of the centre: harmonic 2 .. 64
	const double w = 0.25 + 1.5 * static_cast<double>(width); // octaves
	const double z = (log2k - c) / w;
	return 4.0 * static_cast<double>(gain) * std::exp(-0.5 * z * z);
}

/// envelopeAt with log2 k already in hand: the table is built on a log2 grid and Formant Shift reads in log2,
/// so k^-p is exp2(-p·l) and no log2 is taken twice.
double envelopeAtLog2(const PartialFields& f, double l) {
	const double p = rolloffExponent(f.rolloff);
	const double formants = formantTerm(l, f.formant1Centre, f.formant1Width, f.formant1Gain)
	                        + formantTerm(l, f.formant2Centre, f.formant2Width, f.formant2Gain);
	double exponent = -p * l;
	// THE KNEE (PAL): flat from the fundamental up to 2^l0, the power law above. Written as −p·(max(l, l0) − max(l, 0)) on top of
	// −p·l, so knee 0 (l0 = 0) adds exactly nothing anywhere, below the fundamental included, and the knee is continuous.
	if (f.knee > 0.0f) {
		const double l0 = 4.0 * static_cast<double>(f.knee);
		exponent -= p * (std::max(l, l0) - std::max(l, 0.0));
	}
	double g = std::exp2(exponent) * (1.0 + formants);
	// THE SHELF (PAL): up to +12 dB above its point, stepping over an octave (a smoothstep in log k).
	if (f.shelf > 0.0f) {
		const double ls = 1.5 + 3.0 * static_cast<double>(f.shelfAt);
		g *= std::pow(10.0, 12.0 * static_cast<double>(f.shelf) * smoothstep01(l - ls + 0.5) / 20.0);
	}
	// THE BODY (PAL): a broad bump, σ 0.75 octave, up to ×4 (+12 dB) at its centre.
	if (f.body > 0.0f) {
		const double z = (l - (1.0 + 3.6 * static_cast<double>(f.bodyAt))) / 0.75;
		g *= 1.0 + 3.0 * static_cast<double>(f.body) * std::exp(-0.5 * z * z);
	}
	return g;
}

/// envelopeRead with log2 k already in hand. The ends are tested BEFORE the int cast, written so NaN fails
/// the first test: casting a NaN or an out-of-range double to int is undefined.
float envelopeReadLog2(const PartialSpectrum& s, double l) {
	const double pos = (l - kEnvelopeLog2Min) / kStep;
	if (!(pos > 0.0)) {
		return s.envelope[0];
	}
	if (pos >= kEnvelopePoints - 1) {
		return s.envelope[kEnvelopePoints - 1];
	}
	const int i = static_cast<int>(pos);
	const float t = static_cast<float>(pos - i);
	return s.envelope[i] + (s.envelope[i + 1] - s.envelope[i]) * t;
}

/// [0, 1], NaN to 0 (NaN fails `x > 0`).
float clampUnit(float x) {
	return x > 0.0f ? std::min(x, 1.0f) : 0.0f;
}

/// [-1, 1], NaN to 0.
float clampBipolar(float x) {
	if (std::isnan(x)) {
		return 0.0f;
	}
	return std::clamp(x, -1.0f, 1.0f);
}

PartialFields clampFields(const PartialFields& f) {
	PartialFields c = f;
	for (float* v : {&c.rolloff, &c.formant1Centre, &c.formant1Width, &c.formant1Gain, &c.formant2Centre,
	                 &c.formant2Width, &c.formant2Gain, &c.oddEven, &c.combSpacing, &c.combDepth, &c.detail,
	                 &c.offGrid, &c.smoothness, &c.knee, &c.shelf, &c.shelfAt, &c.body, &c.bodyAt, &c.sparsity, &c.sparseSet}) {
		*v = clampUnit(*v);
	}
	return c;
}
} // namespace

double envelopeAt(const PartialFields& f, double k) {
	return envelopeAtLog2(f, std::log2(k));
}

double rolloffExponent(float field) {
	const double u = 2.0 * static_cast<double>(std::clamp(std::isnan(field) ? 0.0f : field, 0.0f, 1.0f)) - 1.0;
	const double bend = std::pow(std::abs(u), 1.6);
	return u < 0.0 ? 1.0 - 1.2 * bend : 1.0 + 1.5 * bend;
}

double sparseKeep(int k, float sparseSet) {
	if (k <= 1) {
		return 1.0;
	}
	if (k > 7) {
		return 0.0;
	}
	const double x = k * std::numbers::phi + 1.5 * static_cast<double>(sparseSet);
	const double tri = std::abs(2.0 * (x - std::floor(x)) - 1.0); // 0..1, continuous in x
	return smoothstep01((tri - 0.6) / 0.4 + 0.5);               // kept above ~0.6, a soft edge 0.4 wide (a glide)
}

double fineAt(const PartialFields& f, int k, float detailTriangle) {
	double g = 1.0;
	// ONE-SIDED (design §2.1): the terrain only removes evens; the even-heavy direction is the drawer's.
	if (f.oddEven > 0.0f && k % 2 == 0) {
		g *= 1.0 - static_cast<double>(f.oddEven);
	}
	if (f.combDepth > 0.0f) {
		const double spacing = 2.0 + 10.0 * static_cast<double>(f.combSpacing);
		g *= 1.0 - static_cast<double>(f.combDepth) * 0.5 * (1.0 + std::cos(2.0 * std::numbers::pi * k / spacing));
	}
	if (f.detail > 0.0f) {
		const double w = 0.2 + 0.8 * std::min(1.0, (k - 1) / 7.0);
		g *= std::max(0.0, 1.0 + static_cast<double>(f.detail) * w * static_cast<double>(detailTriangle));
	}
	// SPARSE COUNTRY (PAL, the fundamental plus a few partials): every partial but the fundamental and the kept
	// few of 2..7 falls by the sparsity, and the kept few rise toward a DRAWBAR's level — their rolloff undone by up to 0.8 of p — so
	// 1+3+5 at full sparsity is three strong partials, not one and two whispers. Fine structure: it stays with the partials when
	// Formant Shift slides the envelope. Continuous in the sparsity and the set.
	if (f.sparsity > 0.0f && k > 1) {
		// THE REST FALL AS (1 − s)³ (−18 dB at s ½, silence at 1), so sparse country is sparse well before the field's peak; the kept
		// few are blended in by s, so the crossfade is continuous.
		const double s = static_cast<double>(f.sparsity);
		const double drawbar = std::pow(static_cast<double>(k), 0.8 * s * std::max(0.0, rolloffExponent(f.rolloff)));
		const double rest = (1.0 - s) * (1.0 - s) * (1.0 - s);
		const double kept = sparseKeep(k, f.sparseSet);
		g *= rest * (1.0 - kept) + kept * ((1.0 - s) * rest + s * drawbar);
	}
	return g;
}

double offGridStart(float offGrid) {
	// EXACTLY the top at zero: exp2(log2(320)) need not round back to 320, and "field 0 is exactly harmonic"
	// is a bit-level promise.
	if (offGrid <= 0.0f) {
		return static_cast<double>(kMaxPartials);
	}
	const double top = std::log2(static_cast<double>(kMaxPartials));
	const double bottom = std::log2(kOffGridLowestStart);
	return std::exp2(top - static_cast<double>(std::clamp(offGrid, 0.0f, 1.0f)) * (top - bottom));
}

double offGridRatio(int k, double start) {
	if (k <= start) {
		return static_cast<double>(k);
	}
	const double rampLength = std::max(kOffGridMinRamp, kOffGridRampShare * start);
	const double r = smoothstep01((k - start) / rampLength);
	const double kphi = k * std::numbers::phi;
	return k + kOffGridDepth * r * ((kphi - std::floor(kphi)) - 0.5);
}

PartialFields smoothed(const PartialFields& clamped) {
	PartialFields c = clamped;
	const float keep = 1.0f - c.smoothness;
	c.detail *= keep;
	c.combDepth *= keep;
	c.oddEven *= keep;
	c.formant1Gain *= keep;
	c.formant2Gain *= keep;
	c.offGrid *= keep;
	// PAL's: the smooth spectrum is the plain power law, so Smoothness takes the knee, shelf, body and sparse country away too.
	c.knee *= keep;
	c.shelf *= keep;
	c.body *= keep;
	c.sparsity *= keep;
	c.rolloff = c.rolloff + (1.0f - c.rolloff) * c.smoothness;
	return c;
}

float envelopeRead(const PartialSpectrum& s, double k) {
	return envelopeReadLog2(s, std::log2(k));
}

void buildSound(const PartialFields& fIn, const std::array<float, kMaxPartials>& detail, PartialSpectrum& out) {
	const PartialFields f = smoothed(clampFields(fIn));
	for (int j = 0; j < kEnvelopePoints; ++j) {
		out.envelope[j] = static_cast<float>(envelopeAtLog2(f, kEnvelopeLog2Min + j * kStep));
	}
	// log2 of each STORED ratio, once: both reads below use it, and envelopeRead(s, ratio[i]) takes the same
	// log2 of the same double, so the fast path stays exactly the table read at each ratio.
	std::array<double, kMaxPartials> log2Ratio;
	const double start = offGridStart(f.offGrid);
	for (int i = 0; i < kMaxPartials; ++i) {
		const int k = i + 1;
		out.ratio[i] = static_cast<float>(offGridRatio(k, start));
		log2Ratio[i] = std::log2(static_cast<double>(out.ratio[i]));
		out.fine[i] = static_cast<float>(fineAt(f, k, clampBipolar(detail[i])));
	}
	for (int i = 0; i < kMaxPartials; ++i) {
		out.envAtRatio[i] = envelopeReadLog2(out, log2Ratio[i]);
	}
	// NORMALISE: Rolloff and the formants change colour, not loudness. One scale on the envelope (table and
	// fast path alike), so envAtRatio stays exactly the table read at each ratio.
	double power = 0.0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double a = static_cast<double>(out.envAtRatio[i]) * static_cast<double>(out.fine[i]);
		power += a * a;
	}
	const float scale = power > 0.0 ? static_cast<float>(kTargetRss / std::sqrt(power)) : 0.0f;
	for (float& e : out.envelope) {
		e *= scale;
	}
	for (int i = 0; i < kMaxPartials; ++i) {
		out.envAtRatio[i] = envelopeReadLog2(out, log2Ratio[i]);
	}
}

void normaliseRss(PartialSpectrum& s) {
	double power = 0.0;
	for (int i = 0; i < kMaxPartials; ++i) {
		const double a = static_cast<double>(s.envAtRatio[i]) * static_cast<double>(s.fine[i]);
		power += a * a;
	}
	if (!(power > 0.0)) {
		return;
	}
	const auto scale = static_cast<float>(kTargetRss / std::sqrt(power));
	for (float& e : s.envelope) {
		e *= scale;
	}
	for (float& e : s.envAtRatio) {
		e *= scale;
	}
}

void buildShapeSpectrum(ShapeKind kind, PartialSpectrum& out) {
	// The rolloff exponent and the mask, by kind. Anything out of range builds a Saw rather than reading garbage.
	const double p = kind == ShapeKind::Triangle ? 2.0 : 1.0;
	for (int j = 0; j < kEnvelopePoints; ++j) {
		out.envelope[j] = static_cast<float>(std::exp2(-p * (kEnvelopeLog2Min + j * kStep)));
	}
	for (int i = 0; i < kMaxPartials; ++i) {
		const int k = i + 1;
		const bool on = kind == ShapeKind::Sine                                  ? k == 1
		                : kind == ShapeKind::Square || kind == ShapeKind::Triangle ? k % 2 == 1
		                                                                           : true;
		out.ratio[i] = static_cast<float>(k);
		out.fine[i] = on ? 1.0f : 0.0f;
		out.envAtRatio[i] = static_cast<float>(std::pow(static_cast<double>(k), -p));
	}
	normaliseRss(out);
}

void blendTowardShape(PartialSpectrum& sound, const PartialSpectrum& shape, float amount) {
	// NaN fails `amount > 0`, so it is the zero amount: the terrain untouched.
	if (!(amount > 0.0f)) {
		return;
	}
	const float a = std::min(amount, 1.0f);
	const float keep = 1.0f - a;
	for (int j = 0; j < kEnvelopePoints; ++j) {
		sound.envelope[j] = keep * sound.envelope[j] + a * shape.envelope[j];
	}
	for (int i = 0; i < kMaxPartials; ++i) {
		sound.fine[i] = keep * sound.fine[i] + a * shape.fine[i];
		sound.ratio[i] = keep * sound.ratio[i] + a * shape.ratio[i];
		sound.envAtRatio[i] = keep * sound.envAtRatio[i] + a * shape.envAtRatio[i];
	}
	// The blended envelope times the blended mask has cross terms, so the mix is not at the target level.
	normaliseRss(sound);
}

// A blended envAtRatio is the blend of each sound's reads, NOT the blended table read at the blended ratio.
// That is deliberate (Σ w·field(coordinate), the house rule), and it is why the "envAtRatio is the table read"
// case runs on ONE sound.
void accumulate(PartialSpectrum& into, const PartialSpectrum& sound, double weight) {
	const float w = static_cast<float>(weight);
	for (int j = 0; j < kEnvelopePoints; ++j) {
		into.envelope[j] += w * sound.envelope[j];
	}
	for (int i = 0; i < kMaxPartials; ++i) {
		into.fine[i] += w * sound.fine[i];
		into.ratio[i] += w * sound.ratio[i];
		into.envAtRatio[i] += w * sound.envAtRatio[i];
	}
}

} // namespace feathers::partials

namespace feathers::partials {

void cycleOf(const PartialSpectrum& s, int count, int phaseN, const float* gain, int points, double period, float* out) {
	if (points <= 0) {
		return;
	}
	std::fill_n(out, points, 0.0f);
	const int n = std::clamp(count, 0, kMaxPartials);
	if (n == 0 || !(period > 0.0)) {
		return;
	}
	const double big = static_cast<double>(phaseN > 0 ? phaseN : n);
	// ONE PARTIAL AT A TIME: a phasor (c, s) in double turned by the partial's step per sample, its sine added into `out`.
	// The recurrence's rounding over a few thousand steps is ~1e-13 of the amplitude, far below a float's last place. NO
	// ALLOCATION: the voice calls this on the audio thread (the stack's likeness reading), so the sum is `out` itself.
	for (int k = 0; k < n; ++k) {
		const double a = static_cast<double>(s.envAtRatio[static_cast<size_t>(k)])
		                 * static_cast<double>(s.fine[static_cast<size_t>(k)])
		                 * (gain != nullptr ? static_cast<double>(gain[k]) : 1.0);
		if (a == 0.0) {
			continue;
		}
		const double theta = std::numbers::pi * static_cast<double>(k) * static_cast<double>(k) / big;
		const double step = 2.0 * std::numbers::pi * static_cast<double>(s.ratio[static_cast<size_t>(k)]) / period;
		const double sc = std::cos(step), ss = std::sin(step);
		double c = std::cos(theta), v = std::sin(theta);
		for (int j = 0; j < points; ++j) {
			out[j] += static_cast<float>(a * v);
			const double nc = c * sc - v * ss;
			v = v * sc + c * ss;
			c = nc;
		}
	}
}

} // namespace feathers::partials
