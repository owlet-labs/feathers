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

#include "fx/conv/convolver.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace feathers::fx::featherverb {

//--------------------------------------------------------------------------------------------------//
// **2x, FOR THE CORE** (backlog JE: take oversampling with enable button). The firmware's //
// core works on paired samples, so its images sit about a quarter of the rate; at 44.1/48 kHz they are heard, //
// at 96 kHz they are above the band. Oversampled, the core runs at twice the host rate and they are filtered.  //
//--------------------------------------------------------------------------------------------------//
//
// ONE HALFBAND BOTH WAYS — the convolver's (`conv::halfband`, 47 taps, DC gain 1, every other tap zero but the
// centre), so each direction computes only its 24 live taps. Up: the even outputs are the filter's even phase
// (doubled, for the zero-stuffing), the odd ones the centre tap alone — the input, 11 samples late. Down: the
// even phase plus the centre. Each way delays by `conv::kHalfbandDelay` samples at the doubled rate, so the pair
// delays the wet by 23 host samples (under half a millisecond at 48 kHz).
//
// In and out as the core's int32 at the reference's scale, rounded and saturated. Nothing allocates.

inline constexpr size_t kOversampleMaxFrames = 256; ///< host frames per call (the device's chunk)
inline constexpr int kOversampleDelayFrames = static_cast<int>(conv::kHalfbandDelay); ///< the pair's, in host frames

namespace detail {
inline int32_t toInt32Sat(float v) noexcept {
	constexpr float kHi = 2147483520.0f; // the largest float below 2^31
	const float r = std::nearbyint(std::clamp(v, -kHi, kHi));
	return static_cast<int32_t>(r);
}
} // namespace detail

/// HOST RATE TO TWICE IT: `n` samples in, `2n` out.
class HalfbandUp {
public:
	void reset() noexcept { hist_.fill(0.0f); }
	void run(const int32_t* x, int32_t* y, size_t n) noexcept {
		const std::array<float, conv::kHalfbandTaps>& h = conv::halfband();
		n = std::min(n, kOversampleMaxFrames);
		for (size_t m = 0; m < n; ++m) {
			hist_[kHist + m] = static_cast<float>(x[m]);
		}
		for (size_t m = 0; m < n; ++m) {
			const size_t at = kHist + m;
			float acc = 0.0f;
			for (size_t j = 0; j <= kHist; ++j) {
				acc += h[2 * j] * hist_[at - j];
			}
			y[2 * m] = detail::toInt32Sat(2.0f * acc);
			y[2 * m + 1] = detail::toInt32Sat(hist_[at - kHist / 2]);
		}
		std::copy(hist_.begin() + static_cast<std::ptrdiff_t>(n), hist_.begin() + static_cast<std::ptrdiff_t>(n + kHist),
		          hist_.begin());
	}

private:
	/// The even phase reaches 23 input samples back; the centre, 11.
	static constexpr size_t kHist = conv::kHalfbandDelay;
	static_assert(kHist % 2 == 1, "the centre sits on an odd tap, so the odd outputs are the centre alone");
	std::array<float, kHist + kOversampleMaxFrames> hist_ {};
};

/// TWICE THE HOST RATE BACK TO IT: `2n` samples in, `n` out.
class HalfbandDown {
public:
	void reset() noexcept { hist_.fill(0.0f); }
	void run(const int32_t* v, int32_t* y, size_t n) noexcept {
		const std::array<float, conv::kHalfbandTaps>& h = conv::halfband();
		n = std::min(n, kOversampleMaxFrames);
		for (size_t i = 0; i < 2 * n; ++i) {
			hist_[kHist + i] = static_cast<float>(v[i]);
		}
		for (size_t m = 0; m < n; ++m) {
			const size_t at = kHist + 2 * m;
			float acc = h[conv::kHalfbandDelay] * hist_[at - conv::kHalfbandDelay];
			for (size_t k = 0; k < conv::kHalfbandTaps; k += 2) {
				acc += h[k] * hist_[at - k];
			}
			y[m] = detail::toInt32Sat(acc);
		}
		std::copy(hist_.begin() + static_cast<std::ptrdiff_t>(2 * n),
		          hist_.begin() + static_cast<std::ptrdiff_t>(2 * n + kHist), hist_.begin());
	}

private:
	static constexpr size_t kHist = conv::kHalfbandTaps - 1;
	std::array<float, kHist + 2 * kOversampleMaxFrames> hist_ {};
};

} // namespace feathers::fx::featherverb
