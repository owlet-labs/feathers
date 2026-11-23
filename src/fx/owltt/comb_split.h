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

/// **THE COMB CROSSOVER (OWLTT Mode 15): THREE BANDS THAT ARE HARMONIC CLASSES OF A FUNDAMENTAL f0, NOT FREQUENCY RANGES.**
///
/// A feedforward comb `H(z) = (1 + z^-D) / 2` with the delay D = fs / (2 f0) has the response (1 + e^(-j pi k)) / 2 at the k-th harmonic of f0: 1 for
/// an even k, 0 for an odd one. So `E = H x` carries the even harmonics (and DC), and the RESIDUAL `O = x - E` the odd ones. The same cut one
/// octave finer, on E alone, with D2 = D / 2 = fs / (4 f0), splits the even harmonics by whether k/2 is even:
///
///   odd  = x - E                      harmonics 1, 3, 5, 7 ...            (k odd; the fundamental is here)
///   two  = E - A                      harmonics 2, 6, 10, 14 ...          (k = 2 mod 4)
///   four = A = (E + E z^-D2) / 2      harmonics 4, 8, 12 ... and DC       (k = 0 mod 4)
///
/// THE THREE SUM TO THE INPUT EXACTLY, whatever the delay and however it is interpolated: the second and third bands are `E - A` and `A`, so
/// `two + four = E`, and `odd + E = x`. A fractional delay that is off by a fraction of a sample, or a window that rolls the top octave off,
/// only blurs which band a component lands in; nothing is ever lost or doubled. The classes mod 4 are each their own mirror (k and 4 - k are the
/// same class), which is what a real filter requires: a split into classes mod 3 would have to separate k from 3 - k, and no real filter can.
///
/// No latency: both combs are feedforward and read the past only. The fractional delay is the codebase's windowed-sinc line
/// (`dsp::SincDelayLine`) at bandwidth 1.0, which at a whole-sample delay is the sample itself to the bit, and elsewhere is a flat twelve-tap
/// interpolator out to about a third of the rate (the ripple near Nyquist is the price; it moves energy between classes there and loses none).
class CombSplit {
public:
	/// Each line holds 16384 samples: a 20 Hz fundamental needs fs / 40 = 1200 at 48 kHz, 4800 at 192 kHz, and up to 655 kHz fits.
	static constexpr int kLineSize = 16384;
	/// The lowest fundamental: 20 Hz where the lines reach, never below what they can hold.
	static constexpr double kMinF0Hz = 20.0;

	/// The three bands of one sample, in OWLTT's Low, Mid, High slots.
	struct Bands {
		float odd = 0.0f;  ///< Low slot: the odd harmonics
		float two = 0.0f;  ///< Mid slot: harmonics 2, 6, 10 ... (k = 2 mod 4)
		float four = 0.0f; ///< High slot: harmonics 4, 8, 12 ... and DC (k = 0 mod 4)
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
				channel->first.clear();
				channel->second.clear();
			}
		}
	}

	/// The fundamentals the lines can serve: the second comb's delay fs / (4 f0) must be at least the interpolator's six samples, and the first
	/// comb's, twice that, at most what a line holds.
	[[nodiscard]] double minF0(double sampleRate) const noexcept {
		return std::max(kMinF0Hz, sampleRate / (2.0 * static_cast<double>(kLineSize - 16)));
	}
	[[nodiscard]] double maxF0(double sampleRate) const noexcept { return sampleRate / (4.0 * static_cast<double>(dsp::SincDelayLine<kLineSize>::kMinDelay)); }

	/// THE FIRST COMB'S DELAY, samples, for a fundamental (clamped to what the lines serve): fs / (2 f0).
	[[nodiscard]] double delayFor(double f0Hz, double sampleRate) const noexcept {
		const double f0 = std::clamp(f0Hz, minF0(sampleRate), maxF0(sampleRate));
		return sampleRate / (2.0 * f0);
	}

	/// One sample of one channel at the first comb's delay `d1` (the second runs at d1 / 2).
	[[nodiscard]] Bands process(size_t channel, float x, float d1) {
		Channel& c = *channels_[channel];
		const float delayed1 = c.first.read(d1);
		c.first.write(x);
		const float even = 0.5f * (x + delayed1);
		const float delayed2 = c.second.read(0.5f * d1);
		c.second.write(even);
		const float four = 0.5f * (even + delayed2);
		return { x - even, even - four, four };
	}

private:
	struct Channel {
		explicit Channel(const dsp::SincTable& table) : first(table), second(table) {}
		dsp::SincDelayLine<kLineSize> first;  ///< x, read at D
		dsp::SincDelayLine<kLineSize> second; ///< E, read at D / 2
	};
	double sampleRate_ = 48000.0;
	std::unique_ptr<dsp::SincTable> table_;
	std::array<std::unique_ptr<Channel>, 2> channels_;
};

} // namespace feathers::fx::owltt
