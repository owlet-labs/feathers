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

#include "dsp/vocode.h"

#include "dsp/spectral_shaper.h"
#include "dsp/vocode_bands.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace feathers::spectral {

namespace {

constexpr double kLog2Low = 4.321928094887362; // log2(20)
constexpr double kCellsPerOctave = (VocodeEnvelope::kPoints - 1) / VocodeEnvelope::kOctaves;
constexpr double kLog2Of10Over20 = 0.16609640474436813; // log2(10) / 20: dB to log2 amplitude
constexpr int kMax = 512;
static_assert(kMax >= 320);
/// The bands within reach of a point: the nearest and four either side (the Spectral effect's kBandReach).
constexpr int kBandReach = 9;

/// log2 for x > 0, by the exponent and an odd series in t = (m − 1)/(m + 1) to t^7 over m in [1, 2): error under 2e-5 (1e-4 dB).
/// Branch-free floats, so the loops below vectorise. A non-positive x is the caller's to exclude.
inline float log2Fast(float x) {
	const uint32_t b = std::bit_cast<uint32_t>(x);
	const float e = static_cast<float>(static_cast<int>((b >> 23) & 0xFFu) - 127);
	const float m = std::bit_cast<float>((b & 0x007FFFFFu) | 0x3F800000u);
	const float t = (m - 1.0f) / (m + 1.0f);
	const float t2 = t * t;
	return e + t * (2.8853900818f + t2 * (0.9617966939f + t2 * (0.5770780164f + t2 * 0.4121985831f)));
}

/// The grid in dBFS at grid coordinate u (0..kPoints − 1), linear between points.
inline float gridAt(const std::array<float, VocodeEnvelope::kPoints>& grid, float u) {
	const float clamped = std::clamp(u, 0.0f, static_cast<float>(VocodeEnvelope::kPoints - 1));
	const int i = std::min(static_cast<int>(clamped), VocodeEnvelope::kPoints - 2);
	const float f = clamped - static_cast<float>(i);
	const float a = grid[static_cast<size_t>(i)];
	const float b = grid[static_cast<size_t>(i + 1)];
	return a + (b - a) * f;
}

double uOf(double hz) { return (std::log2(std::max(hz, 1e-3)) - kLog2Low) * kCellsPerOctave; }

/// The envelope's peak over the range it is read in (Depth past 1 reads it).
float peakDb(const std::array<float, VocodeEnvelope::kPoints>& grid, double uLow, double uHigh) {
	float peak = std::max(gridAt(grid, static_cast<float>(uLow)), gridAt(grid, static_cast<float>(uHigh)));
	for (int g = 0; g < VocodeEnvelope::kPoints; ++g) {
		if (static_cast<double>(g) > uLow && static_cast<double>(g) < uHigh) {
			peak = std::max(peak, grid[static_cast<size_t>(g)]);
		}
	}
	return peak;
}

/// Key Track's transposition: the held envelope read at f · frozenHz / f0.
double keyRatio(const VocodeEnvelope& env, double f0Hz) {
	return env.keyTrack && f0Hz > 0.0 && env.frozenHz > 0.0 ? f0Hz / env.frozenHz : 1.0;
}

/// **THE BANDED ROAD AT THE PARTIALS** (OE2, additive design §9.8): the Spectral effect's layout (dsp/vocode_bands.h) laid over the
/// Range at this note, each band's level the envelope at its centre (read through Formant Shift and Key Track there, held to the
/// Range, closed by the Gate), and each partial the partition-of-unity mix of the bands within reach of its frequency — the effect's
/// `bandEnvelope`, at partials rather than bins. Writes each partial's level in dBFS (`db`, kSilentDb where silent) and, for Spread,
/// its band coordinate (`bandU`: band b centred at u = b, linear between centres). HARMONIC picks each group's loudest partial of the
/// sound's own shape (the carrier is the voice), the gaps their middle member (no partial sounds there).
void bandLevels(const VocodeOp& op, const VocodeEnvelope& env, const std::array<float, VocodeEnvelope::kPoints>& grid,
                const ShaperParams& drawer, const float* hz, const float* shape, const float* fade, int first, int n,
                double f0Hz, double low, double high, double key, float* db, float* bandU) {
	BandSpec spec;
	spec.count = static_cast<int>(std::lround(std::clamp(env.bands, 8.0, 128.0)));
	spec.low = low;
	spec.high = std::max(high, low * 1.01);
	spec.bandwidth = std::clamp(static_cast<double>(op.bandwidth), 0.0, 1.0);
	spec.zone = std::clamp(op.zone, 0, 6);
	spec.zoneDrive = op.zoneDrive();
	spec.widthZone = std::clamp(op.widthZone, 0, 5);
	spec.widthDrive = op.widthDrive();
	spec.f0 = f0Hz;
	bool inharmonic = false;
	BandGrouping grouping;
	const bool harmonic = layoutHarmonic(spec, &inharmonic);
	if (harmonic) {
		grouping.regroup(spec.count, spec.low, spec.high, inharmonic, f0Hz);
		if (grouping.grouped() && !inharmonic) {
			// THE LOUDEST MEMBER per group, as the voice will render it: member m is harmonic m + firstMember + 1, partial m + firstMember.
			for (int b = 0; b < grouping.bands(); ++b) {
				int best = grouping.pick(b);
				float bestLevel = -1.0f;
				for (int m = grouping.first(b); m <= grouping.last(b); ++m) {
					const int i = m + grouping.firstMember();
					const float l = i >= 0 && i < n ? shape[i] * fade[i] : 0.0f;
					if (l > bestLevel) {
						best = m;
						bestLevel = l;
					}
				}
				(void)grouping.setPick(b, best);
			}
		}
	}
	std::array<double, kMaxVocodeBands> centre {};
	std::array<double, kMaxVocodeBands> sigma {};
	const int count = layoutBands(spec, harmonic ? &grouping : nullptr, centre.data(), sigma.data());
	// EACH BAND'S LEVEL: the envelope at its centre, read where Formant Shift and Key Track send it, held to the Range, gated.
	const bool gated = op.gateDb > VocodeOp::kGateOffDb + 1e-6f;
	const auto uLow = static_cast<float>(uOf(low));
	const auto uHigh = static_cast<float>(uOf(std::max(high, low)));
	std::array<double, kMaxVocodeBands> level {};
	for (int b = 0; b < count; ++b) {
		const double hzC = std::exp2(centre[static_cast<size_t>(b)]);
		const double read = hzC / std::max(formantRatio(drawer, hzC) * key, 1e-6);
		const float d = gridAt(grid, std::clamp(static_cast<float>(uOf(read)), uLow, uHigh));
		const bool silent = d <= VocodeEnvelope::kSilentDb || (gated && d < op.gateDb);
		level[static_cast<size_t>(b)] = silent ? 0.0 : std::exp2(static_cast<double>(d) * kLog2Of10Over20);
	}
	// EACH PARTIAL: the bands within reach of its frequency (held at the outermost centres), their Gaussians divided by their sum.
	int nearest = 0;
	for (int i = first; i < n; ++i) {
		const double x = std::clamp(std::log2(std::max(static_cast<double>(hz[i]), 1.0)), centre[0],
		                            centre[static_cast<size_t>(count - 1)]);
		while (nearest + 1 < count && std::fabs(centre[static_cast<size_t>(nearest + 1)] - x) <= std::fabs(centre[static_cast<size_t>(nearest)] - x)) {
			++nearest;
		}
		while (nearest > 0 && std::fabs(centre[static_cast<size_t>(nearest - 1)] - x) < std::fabs(centre[static_cast<size_t>(nearest)] - x)) {
			--nearest; // the frequencies rise, but a pinned sub may sit below its neighbours: walk back as far as it asks
		}
		{
			const int j = x >= centre[static_cast<size_t>(nearest)] ? nearest : std::max(nearest - 1, 0);
			const double gap = j + 1 < count ? centre[static_cast<size_t>(j + 1)] - centre[static_cast<size_t>(j)] : 0.0;
			bandU[i] = static_cast<float>(j + (gap > 0.0 ? (x - centre[static_cast<size_t>(j)]) / gap : 0.0));
		}
		const int reach = (kBandReach - 1) / 2;
		const int lo = std::max(0, nearest - reach);
		const int hi = std::min(count - 1, nearest + reach);
		double sum = 0.0;
		double mix = 0.0;
		for (int b = lo; b <= hi; ++b) {
			const double d = (x - centre[static_cast<size_t>(b)]) / sigma[static_cast<size_t>(b)];
			const double g = std::exp(-0.5 * d * d);
			sum += g;
			mix += g * level[static_cast<size_t>(b)];
		}
		const double l = sum < 1e-30 ? level[static_cast<size_t>(nearest)] : mix / sum;
		db[i] = l > 0.0 ? static_cast<float>(20.0 * std::log10(l)) : VocodeEnvelope::kSilentDb;
	}
}

} // namespace

void vocodeGains(const VocodeOp& op, const VocodeEnvelope& env, int ear, const float* hz, const float* formant,
                 const ShaperParams& drawer, const float* shape, const float* fade, int first, int count, double f0Hz,
                 float* gain) noexcept {
	first = std::clamp(first, 0, count);
	std::fill_n(gain, first, 1.0f);
	const double depth = std::clamp(static_cast<double>(op.depth), 0.0, 2.0);
	const bool spread = op.stereo == 2;
	if (!op.on || (depth == 0.0 && !spread) || count <= first) {
		std::fill(gain + first, gain + std::max(count, first), 1.0f);
		return;
	}
	const std::array<float, VocodeEnvelope::kPoints>& grid = env.gridFor(ear, op.stereo);
	// THE MEAN SHAPE ā: the RSS of what renders over the partials vocoded, per partial.
	double power = 0.0;
	for (int i = first; i < count; ++i) {
		const double a = static_cast<double>(shape[i]) * static_cast<double>(fade[i]);
		power += a * a;
	}
	const double mean = std::sqrt(power / static_cast<double>(count - first));
	if (!(mean > 0.0) || !(f0Hz > 0.0) || !(env.binHz > 0.0)) {
		std::fill(gain + first, gain + count, 0.0f);
		return;
	}
	const double low = std::clamp(static_cast<double>(op.lowHz), VocodeEnvelope::kLowHz, 20000.0);
	const double high = std::max(std::clamp(static_cast<double>(op.highHz), VocodeEnvelope::kLowHz, 20000.0), low);
	const auto uLow = static_cast<float>(uOf(low));
	const auto uHigh = static_cast<float>(uOf(high));
	const float flatten = std::clamp(op.flatten, 0.0f, 1.0f);
	const double key = keyRatio(env, f0Hz);
	// THE PER-NOTE CONSTANTS IN LOG2: the calibration (a line at 0 dBFS), ā's share.
	const double calib = std::log2((2.0 / 3.141592653589793) * std::sqrt(2.0 * f0Hz / env.binHz)) + kVocodeNoiseBiasDb * kLog2Of10Over20;
	const auto constant = static_cast<float>(calib - (1.0 - static_cast<double>(flatten)) * std::log2(mean));
	const bool gated = op.gateDb > VocodeOp::kGateOffDb + 1e-6f;
	const float gate = op.gateDb;
	const auto depthF = static_cast<float>(depth);
	const float peak = depth > 1.0 ? peakDb(grid, uLow, uHigh) : 0.0f;
	constexpr auto kDbToLog2 = static_cast<float>(kLog2Of10Over20);
	constexpr auto kCells = static_cast<float>(kCellsPerOctave);
	// IN PASSES, so the arithmetic ones vectorise (the grid read is a gather): the shape's log2; each partial's level in dB, on the
	// smooth road the grid at the partial's read point, on the banded road the bands' mix (bandLevels); then the exponent.
	alignas(16) float ls[kMax];
	alignas(16) float db[kMax];
	alignas(16) float bandU[kMax];
	alignas(16) float q[kMax];
	const int n = std::min(count, kMax);
	for (int i = first; i < n; ++i) {
		const float s = shape[i];
		ls[i] = log2Fast(s > 0.0f ? s : 1.0f);
	}
	const bool banded = op.banded();
	if (banded || spread) {
		bandLevels(op, env, grid, drawer, hz, shape, fade, first, n, f0Hz, low, high, key, db, bandU);
	}
	if (!banded) {
		alignas(16) float u[kMax];
		const auto shift = static_cast<float>(std::log2(key) * kCellsPerOctave + kLog2Low * kCellsPerOctave);
		for (int i = first; i < n; ++i) {
			u[i] = log2Fast(std::max(hz[i], 1.0f)) * kCells - shift;
		}
		if (formant != nullptr) {
			for (int i = first; i < n; ++i) {
				u[i] -= log2Fast(std::max(formant[i], 1e-6f)) * kCells;
			}
		}
		for (int i = first; i < n; ++i) {
			const float d = gridAt(grid, std::clamp(u[i], uLow, uHigh));
			db[i] = gated && d < gate ? VocodeEnvelope::kSilentDb : d;
		}
	}
	const float depthGain = depthF < 1.0f ? depthF : 1.0f;
	const float peakGain = depthF > 1.0f ? (depthF - 1.0f) * kDbToLog2 : 0.0f;
	for (int i = first; i < n; ++i) {
		const float d = db[i];
		const bool silent = depthGain > 0.0f && (d <= VocodeEnvelope::kSilentDb || !(shape[i] > 0.0f)); // Depth 0: untouched
		const float l = (d * kDbToLog2 + constant - flatten * ls[i]) * depthGain + peakGain * (d - peak);
		q[i] = silent ? -2000.0f : l;
	}
	exp2Scaled(q + first, 1.0, n - first, gain + first);
	for (int i = first; i < n; ++i) {
		gain[i] = q[i] <= -1500.0f ? 0.0f : gain[i];
	}
	// SPREAD (the effect's §10.9): band b, centred at u = b, to the left when even — sqrt(2)·|cos(πu/2)| left, sqrt(2)·|sin(πu/2)|
	// right, so the two ears keep the power. A partial's own band coordinate decides its ear.
	if (spread) {
		for (int i = first; i < n; ++i) {
			const double u = static_cast<double>(bandU[i]);
			const double w = ear == 0 ? std::cos(1.5707963267948966 * u) : std::sin(1.5707963267948966 * u);
			gain[i] *= static_cast<float>(1.4142135623730951 * std::fabs(w));
		}
	}
}

} // namespace feathers::spectral
