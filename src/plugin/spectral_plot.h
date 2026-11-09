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

#include "dsp/partial_spectrum.h"

#include <array>
#include <cstdint>
#include <cstring>

namespace feathers {

/// **WHAT THE SPECTRUM DRAWER'S PLOT DRAWS** (additive design §4.2, D2): one frame of what is HEARD, published by the engine
/// (Synth::plotFrame, through ParamBridge::publishPlot) rather than re-derived by the editor, so the blended drawer, its
/// compensated Tilt, Formant Shift, per-voice modulation and the stereo ears are all exactly the voice's.
///
/// The newest sounding voice's partials (stack part 0) — or, with no note, the bank's spectrum at the most recently played
/// note's pitch, through the drawer the next note would get. Per partial: its frequency ratio, the TERRAIN amplitude (the
/// drawer-off amplitude the bank would render: envelope · fine · the Nyquist fade) and the RESULT per ear (what the bank
/// renders). `count` partials are meaningful; the rest are zero, so two frames compare by value.
struct SpectralPlotFrame {
	static constexpr int kPartials = partials::kMaxPartials;
	float f0 = 0.0f;
	int32_t count = 0;
	/// The ears differ (the result's two rows are drawn apart, in the stereo pair's tints).
	int32_t stereo = 0;
	/// 1: a voice is playing and this is its note; 0: no note, the reference pitch.
	int32_t playing = 0;
	std::array<float, kPartials> ratio {};
	std::array<float, kPartials> terrain {};
	std::array<float, kPartials> left {};
	std::array<float, kPartials> right {};

	/// Bitwise, so "nothing changed" is exact: a frame that differs in any float is a new frame.
	[[nodiscard]] bool sameAs(const SpectralPlotFrame& o) const noexcept {
		return std::memcmp(this, &o, sizeof(SpectralPlotFrame)) == 0;
	}
};
static_assert(sizeof(SpectralPlotFrame) == 4 * sizeof(int32_t) + 4 * partials::kMaxPartials * sizeof(float),
              "no padding: sameAs compares the bytes");

/// The frame's floats in one flat order — what the bridge's per-element atomics carry (ParamBridge::publishPlot).
inline constexpr size_t kSpectralPlotFloats = 4 + 4 * static_cast<size_t>(partials::kMaxPartials);

} // namespace feathers
