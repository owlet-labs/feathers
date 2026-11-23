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
 */

#pragma once

#include "dsp/sinc_delay_line.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>

namespace feathers::fx::owltt {

/// **THE COMB CROSSOVER (OWLTT Modes 15 and 16): A TREE OF TWO COMPLEMENTARY COMBS, SO THE BANDS ARE HARMONIC CLASSES, NOT FREQUENCY RANGES.**
///
/// A feedforward comb `H(z) = (1 + z^-D) / 2` with D = fs / fc has unity gain at every multiple of fc (the TEETH) and a null half way between.
/// Split 1 runs at spacing fc1 on the input x: `E = H1 x` holds what sits on the teeth, and the residual `x - E` what sits between them.
/// Split 2 runs at spacing fc2 on E alone: `A = H2 E` and the residual `E - A`. The three bands are
///
///   Low  = x - E     between the teeth of fc1                          (at fc2 = 2 fc1: everything between the harmonics)
///   Mid  = E - A     on fc1's teeth but not fc2's                      (at fc2 = 2 fc1: the odd harmonics)
///   High = A         on both                                           (at fc2 = 2 fc1: the even harmonics)
///
/// THEY SUM TO THE INPUT EXACTLY for any fc1 and fc2, however the delays are interpolated: Mid + High = E and Low + E = x, the residuals being taken
/// as differences. Both combs are feedforward and read the past only, so nothing is delayed. A real filter cannot tell harmonic k from N - k, so
/// only the pairs (on, off) of one comb are offered: a third class by mod-3 would need a Hilbert split.
///
/// RESONANCE (Vibe and Res Home, `kCombMaxResonance`): each comb is followed by a feedback comb of the same delay and a gain g in [0, 0.95]; the bands stay the
/// residuals above, so they sum to the input at any g, moving or not.
///
/// `CombShape::stages` is the sharpness: 1 is the plain comb; 2 runs each comb twice, `((1 + z^-D) / 2)^2`, whose teeth are narrower and whose
/// nulls are flat (a double zero). THE SHAPE'S VALUES LIVE HERE, in this one struct, so that anything that later varies them (a tooth depth, a
/// resonance) changes this and not the call sites.
struct CombShape {
	int stages = 1;
};

class CombSplit {
public:
	/// Each line holds 32768 samples. The longest delay asked for is fs / fcMin, so the lowest spacing served is fs / (kLineSize - 16): 1.5 Hz at 48 kHz, 5.9 Hz
	/// at 192 kHz; that is the bound on the lines: the lowest note the device tracks (20 Hz) at x1/8 is 2.5 Hz, which 48 kHz serves and 192 kHz clamps.
	static constexpr int kLineSize = 32768;
	/// A floor under that, whatever the rate.
	static constexpr double kSpacingFloorHz = 1.0;
	/// The three bands of one sample, in OWLTT's Low, Mid, High slots.
	struct Bands {
		float between = 0.0f; ///< Low slot
		float partB = 0.0f;   ///< Mid slot: on fc1's teeth, off fc2's
		float partA = 0.0f;   ///< High slot: on both
	};

	/// Allocates the lines. The only allocation; `process` and the setters never allocate.
	void prepare(double sampleRate) {
		sampleRate_ = sampleRate;
		table_ = std::make_unique<dsp::SincTable>(1.0);
		for (auto& channel : channels_) {
			channel = std::make_unique<Channel>(*table_);
		}
	}

	[[nodiscard]] bool prepared() const noexcept { return table_ != nullptr; }

	void clear() {
		for (auto& channel : channels_) {
			if (channel) {
				for (auto& line : channel->lines) {
					line.clear();
				}
			}
		}
	}

	/// The tooth spacings the lines can serve: the delay fs / fc is at least the interpolator's six samples and at most what a line holds.
	[[nodiscard]] static double minSpacing(double sampleRate) noexcept {
		return std::max(kSpacingFloorHz, sampleRate / static_cast<double>(kLineSize - 16));
	}
	[[nodiscard]] static double maxSpacing(double sampleRate) noexcept { return sampleRate / static_cast<double>(dsp::SincDelayLine<kLineSize>::kMinDelay); }

	/// A COMB'S DELAY, samples, for a tooth spacing (clamped to what the lines serve): fs / fc.
	[[nodiscard]] static double delayFor(double spacingHz, double sampleRate) noexcept {
		return sampleRate / std::clamp(spacingHz, minSpacing(sampleRate), maxSpacing(sampleRate));
	}

	/// One sample of one channel; `d1` and `d2` are the two combs' delays in samples.
	[[nodiscard]] Bands process(size_t channel, float x, float d1, float d2, const CombShape& shape, float g1 = 0.0f, float g2 = 0.0f) {
		Channel& c = *channels_[channel];
		const float on = comb(c, 0, x, d1, shape, g1);
		const float a = comb(c, 2, on, d2, shape, g2);
		return { x - on, on - a, a };
	}

private:
	struct Channel {
		explicit Channel(const dsp::SincTable& table)
		    : lines { dsp::SincDelayLine<kLineSize>(table), dsp::SincDelayLine<kLineSize>(table), dsp::SincDelayLine<kLineSize>(table),
			          dsp::SincDelayLine<kLineSize>(table), dsp::SincDelayLine<kLineSize>(table), dsp::SincDelayLine<kLineSize>(table) } {}
		std::array<dsp::SincDelayLine<kLineSize>, 6> lines; ///< two feedforward stages for each of the two combs, then each comb's feedback line
	};

	/// The comb at line pair `first`: `stages` passes of (v + v[-D]) / 2.
	static float comb(Channel& c, size_t first, float v, float d, const CombShape& shape, float g) {
		for (int s = 0; s < shape.stages && s < 2; ++s) {
			auto& line = c.lines[first + static_cast<size_t>(s)];
			const float delayed = line.read(d);
			line.write(v);
			v = 0.5f * (v + delayed);
		}
		// THE RESONANCE: a feedback comb y = (1 - g) v + g y[-D] on the teeth-passing signal. Its gain is 1 at every tooth (where v passes whole) and its
		// nulls stay nulls, so it only lengthens the ringing of what the teeth let through. The line is written at every g, 0 included, so a g that moves
		// off 0 finds its past. The caller clamps g below 1 (`kCombMaxResonance`); the complement is taken from the INPUT, so the sum stays exact.
		auto& fb = c.lines[4 + first / 2];
		const float y = (1.0f - g) * v + g * fb.read(d);
		fb.write(y);
		v = y;
		return v;
	}

	double sampleRate_ = 48000.0;
	std::unique_ptr<dsp::SincTable> table_;
	std::array<std::unique_ptr<Channel>, 2> channels_;
};

} // namespace feathers::fx::owltt
