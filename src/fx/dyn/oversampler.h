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

#pragma once

#include "dsp/halfband.h"

#include <type_traits>

namespace feathers::fx::dyn {

/// A STEREO 4x OVERSAMPLER around a per-sample nonlinearity: two halfband stages up, the shape on
/// every one of the four oversampled frames, two halfband stages down.
///
/// **OUR OWN HALFBANDS** (`src/dsp/halfband.h`, the clean-room polyphase-allpass design of the licence
/// migration): `HalfbandInterpolator2x` up, `HalfbandDecimator2x` down, on the voice decimator's own two
/// coefficient sets. Rebuilt 2026-09-25 when the dynamics branch met the migration; it was on a vendored
/// GPL halfband before.
///
/// **THE STAGES MIRROR `Halfband4x`:** the steep set (`kHalfbandStage2`, 14 coefficients, transition
/// centred on a quarter of 96 kHz) works between 1x and 2x, where the images and folds sit just past the
/// host Nyquist; the wide set (`kHalfbandStage1`, 6) works between 2x and 4x, where everything the steep
/// stage passed is far below its transition. Down is `Halfband4x::decimate`'s order exactly, and
/// `fx-dyn` pins the two BIT-IDENTICAL, so this decimates as the voice does.
///
/// **WHY NOT `Halfband4x` ITSELF:** it is decimate-only and needs host blocks in multiples of 4.
/// Per-sample stages make any frame count work with no carry — a host that cuts a block anywhere gets
/// the same bits.
///
/// **GAIN:** unity end to end with no compensation: the interpolator's branches carry 2·H (the zero-stuff
/// halving undone), the decimator's half-sum carries H. `fx-dyn` measures the whole path at 1 kHz.
///
/// No allocation: every stage by value, its state fixed-size, trivially copyable. **DENORMALS:** the
/// allpass tails decay into subnormals on silence; nothing here flushes them. The plugin's
/// `DenormalGuard` (`plugin/denormal_guard.h`, FTZ/DAZ around the render) is what keeps that cheap; a
/// tail-silence case run outside the plugin needs the guard or a flush of its own.
class Oversampler4x {
public:
	void reset() noexcept {
		upSteepL_.reset();
		upSteepR_.reset();
		upWideL_.reset();
		upWideR_.reset();
		downWideL_.reset();
		downWideR_.reset();
		downSteepL_.reset();
		downSteepR_.reset();
	}

	/// `shape(float& l, float& r)` runs on every one of the four oversampled frames, in place.
	template <class Shape>
	void process(float* left, float* right, int frames, Shape&& shape) noexcept {
		for (int i = 0; i < frames; ++i) {
			float l2[2], r2[2], l4[4], r4[4];
			upSteepL_.process(left[i], l2[0], l2[1]);
			upSteepR_.process(right[i], r2[0], r2[1]);
			upWideL_.process(l2[0], l4[0], l4[1]);
			upWideR_.process(r2[0], r4[0], r4[1]);
			upWideL_.process(l2[1], l4[2], l4[3]);
			upWideR_.process(r2[1], r4[2], r4[3]);
			for (int k = 0; k < 4; ++k) {
				shape(l4[k], r4[k]);
			}
			const float dl0 = downWideL_.process(l4[0], l4[1]);
			const float dr0 = downWideR_.process(r4[0], r4[1]);
			const float dl1 = downWideL_.process(l4[2], l4[3]);
			const float dr1 = downWideR_.process(r4[2], r4[3]);
			left[i] = downSteepL_.process(dl0, dl1);
			right[i] = downSteepR_.process(dr0, dr1);
		}
	}

private:
	// Named by their coefficient set, not by `halfband.h`'s stage numbers (which count from the 4x
	// side): Steep sits between 1x and 2x, Wide between 2x and 4x.
	HalfbandInterpolator2x<kHalfbandStage2Coefs> upSteepL_ {kHalfbandStage2}, upSteepR_ {kHalfbandStage2};
	HalfbandInterpolator2x<kHalfbandStage1Coefs> upWideL_ {kHalfbandStage1}, upWideR_ {kHalfbandStage1};
	HalfbandDecimator2x<kHalfbandStage1Coefs> downWideL_ {kHalfbandStage1}, downWideR_ {kHalfbandStage1};
	HalfbandDecimator2x<kHalfbandStage2Coefs> downSteepL_ {kHalfbandStage2}, downSteepR_ {kHalfbandStage2};
};
static_assert(std::is_trivially_copyable_v<Oversampler4x>,
              "a device holds this in host-owned storage; no hidden ownership may ride along");

} // namespace feathers::fx::dyn
