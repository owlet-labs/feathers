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

#include "dsp/warp.h"

#include <cmath>
#include <cstddef>

// THE BASIC SHAPES ENGINE'S WAVEFORMS.
//
// Sine, Triangle, Saw and Square as BAND-LIMITED MIP LADDERS, built once per process by additive synthesis
// from each shape's Fourier series, and mixed at read time by four normalised weights. Not waypoints: a
// piecewise-linear table cannot make a saw's or a square's jump, which is how the first attempt failed.
//
// WHICH MIP: the richest one whose top harmonic stays under the band limit for the note's EFFECTIVE
// fundamental — the highest instantaneous frequency FM drives the carrier to. With the limit at the host
// rate (half the voice's 4x Nyquist) nothing folds, and because the mips are an octave apart a switch only
// moves partials between the host's Nyquist and the host rate, which the decimator removes anyway.

namespace feathers {

/// Mip m holds harmonics 1 .. (1024 >> m), so mip 10 is the fundamental alone; 1024 partials are the full
/// audible band down to ~20 Hz.
inline constexpr int kShapesMipCount = 11;
inline constexpr int kShapesTopHarmonics = 1024;
/// THE SINE'S TABLE, and the shortest a ladder's mip may be.
inline constexpr int kShapesTableSize = 2048;
/// HOW MANY SAMPLES EACH MIP SPENDS PER CYCLE OF ITS TOP PARTIAL. The voice reads with linear interpolation,
/// which droops partial k by sinc²(k/length) and images it at length ± k. MEASURED on 2048-sample tables for
/// every mip: a 59 Hz saw read 0.64 dB low at 18 kHz and imaged up to −98.5 dB. At 32 samples per top-partial
/// cycle an audible partial (≤ 20 kHz, while the top is ≥ 24 kHz) droops under 0.02 dB; and no mip goes below
/// kShapesTableSize, which keeps the FUNDAMENTAL's images at (1/2048)² — under −130 dB — on the dull mips.
inline constexpr int kShapesSamplesPerTopCycle = 32;
/// A mip's table length: 32768 samples for the 1024-partial mip down to 2048 from mip 4 on.
constexpr int shapesMipLength(int mip) {
	const int wanted = kShapesSamplesPerTopCycle * (kShapesTopHarmonics >> mip);
	return wanted > kShapesTableSize ? wanted : kShapesTableSize;
}

/// THE ADDED SINE WEIGHT (decision B): what makes all four knobs at zero a pure sine rather than silence or a
/// division by zero. Small enough that one shape alone at 1 % of its travel is already 99 % that shape.
inline constexpr float kShapesSineEpsilon = 1.0e-4f;

/// The four knobs as the patch holds them, 0..1 each. Default: a sine (decided 2026-10-03).
struct ShapesMix {
	float sine = 1.0f;
	float triangle = 0.0f;
	float saw = 0.0f;
	float square = 0.0f;

	friend bool operator==(const ShapesMix&, const ShapesMix&) = default;
};

/// The same four as the voice reads them: normalised to sum to one, with the epsilon in Sine's share.
struct ShapesWeights {
	float sine = 1.0f;
	float triangle = 0.0f;
	float saw = 0.0f;
	float square = 0.0f;

	friend bool operator==(const ShapesWeights&, const ShapesWeights&) = default;
};

/// `out = Σ wᵢ·shapeᵢ / Σ wᵢ` with the epsilon on Sine (decision B). Negative knobs are clamped to zero.
ShapesWeights normaliseShapes(const ShapesMix& mix);

/// Which ladder a shape lives in.
enum class Shape : int { Sine, Triangle, Saw, Square, kCount };

/// Build the tables if they are not built yet. Called from the banks' setSampleRate so the first note never
/// pays for it; safe to call from anywhere and any number of times.
void warmShapeTables();

/// One shape's mip `mip`: shapesMipLength(mip) + 1 samples (kShapesTableSize + 1 for the sine, at every mip),
/// the last a guard repeating the first.
const float* shapeTable(Shape shape, int mip);

/// The richest mip whose top harmonic stays at or below `limitHz` for a fundamental of `topHz`.
int shapesMipFor(double topHz, double limitHz);

/// Where mip `mip` starts inside a ladder (each mip carries its guard sample).
constexpr int shapesMipOffset(int mip) {
	int at = 0;
	for (int m = 0; m < mip; ++m) {
		at += shapesMipLength(m) + 1;
	}
	return at;
}
inline constexpr int kShapesLadderLength = shapesMipOffset(kShapesMipCount);

namespace shapes_detail {
/// THE STORAGE, filled once by warmShapeTables. Read here, inline and unguarded, because the voice reads it
/// four times per host sample per shape and a thread-safe-static check per read was measurable; everything
/// that reads it (the banks, the wave display, the terrain) warms it first.
extern float gSine[kShapesTableSize + 1];
extern float gLadders[3][kShapesLadderLength]; // Triangle, Saw, Square

inline float lerpAt(const float* row, int len, double wrapped) {
	const double u = wrapped * static_cast<double>(len);
	// The guard covers idx + 1; a wrapped phase of 1 − 1e-17 rounds up to exactly len, hence the clamp.
	int idx = static_cast<int>(u);
	if (idx >= len) {
		idx = len - 1;
	}
	const float frac = static_cast<float>(u - static_cast<double>(idx));
	return row[idx] + (row[idx + 1] - row[idx]) * frac;
}
} // namespace shapes_detail

/// THE ONE DECODE: the normalised blend at `phase01` (any range — wrapped here), read from mip `mip`.
/// Zero-weight shapes are not read. warmShapeTables must have run (it has, wherever a bank exists).
inline float evalShapes(const ShapesWeights& w, int mip, double phase01) {
	using namespace shapes_detail;
	const double wrapped = phase01 - std::floor(phase01);
	float out = 0.0f;
	if (w.sine != 0.0f) {
		out += w.sine * lerpAt(gSine, kShapesTableSize, wrapped);
	}
	if (w.triangle == 0.0f && w.saw == 0.0f && w.square == 0.0f) {
		return out;
	}
	const int m = mip < 0 ? 0 : (mip >= kShapesMipCount ? kShapesMipCount - 1 : mip);
	const int at = shapesMipOffset(m);
	const int len = shapesMipLength(m);
	if (w.triangle != 0.0f) {
		out += w.triangle * lerpAt(gLadders[0] + at, len, wrapped);
	}
	if (w.saw != 0.0f) {
		out += w.saw * lerpAt(gLadders[1] + at, len, wrapped);
	}
	if (w.square != 0.0f) {
		out += w.square * lerpAt(gLadders[2] + at, len, wrapped);
	}
	return out;
}

/// The same read through a warp (decision E): the read phase bent by `warpRead`, exactly the bend V5's chain
/// applies to its source. `WarpMode::Off` is evalShapes to the bit.
inline float evalShapesWarped(const ShapesWeights& w, int mip, double phase01, WarpMode mode, float warpDepth) {
	if (mode == WarpMode::Off) {
		return evalShapes(w, mip, phase01);
	}
	const double wrapped = phase01 - std::floor(phase01);
	const WarpRead r = warpRead(mode, wrapped, warpDepth);
	return r.sign * evalShapes(w, mip, r.phase);
}

} // namespace feathers
