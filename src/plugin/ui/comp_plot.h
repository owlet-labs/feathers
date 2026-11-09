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

#include "fx/comp/comp_types.h"
#include "plugin/ui/dyn_meter.h"

#include <visage/graphics.h>

#include <algorithm>
#include <cmath>

// COMP'S TRANSFER PLOT, ONCE (backlog QG) — the card's small one and the window's large one draw the same picture from
// the same numbers. Input dB across, output dB up, `kFloorDb` to 0 dBFS on both axes.
//
// WHAT IT DRAWS, BY MODE: Compress's knee curve; Limit's ceiling; in Compander the ACTIVE TOPOLOGY'S halves — the first
// half's curve, and in the paired topologies the second half's (on its own input) and the NET curve through both. Linked
// with an empty lane the net is the unity line, which is the truth; unlinked it is what the two ratios leave. Drive in
// front, makeup and Mix after, as the DSP does.

namespace feathers::ui::comp_plot {

inline constexpr float kFloorDb = -60.0f;

/// THE CURVES FOR ONE SETTINGS, at an input level — the picture's numbers, so a case asserts them rather than pixels.
struct Curves {
	fx::comp::Settings s {};

	[[nodiscard]] bool compander() const { return s.mode == static_cast<int>(fx::comp::Mode::Compander); }
	[[nodiscard]] bool paired() const { return compander() && fx::comp::isPaired(s.topology); }

	/// THE FIRST HALF (Compander) on a driven level, in dB.
	[[nodiscard]] float firstDb(float drivenDb) const {
		return fx::comp::firstExpands(s.topology)
		           ? fx::comp::expanderCurveDb(drivenDb, s.thresholdDb, s.cmpRatio, s.cmpRangeDb)
		           : fx::comp::companderCurveDb(drivenDb, s.thresholdDb, s.cmpRatio, s.cmpRangeDb);
	}
	/// THE SECOND HALF (paired) on ITS input level — the first half's inverse at its own ratio (Ratio 2 while unlinked).
	[[nodiscard]] float secondDb(float inDb) const {
		const float r2 = s.cmpLink ? s.cmpRatio : s.cmpRatio2;
		return fx::comp::firstExpands(s.topology) ? fx::comp::companderCurveDb(inDb, s.thresholdDb, r2, s.cmpRangeDb)
		                                          : fx::comp::expanderCurveDb(inDb, s.thresholdDb, r2, s.cmpRangeDb);
	}
	/// THE DEVICE'S STATIC CURVE AT STEADY STATE, input dB to output dB, with an EMPTY lane.
	[[nodiscard]] float netDb(float xDb) const {
		const float driven = xDb + s.driveDb;
		if (s.mode == static_cast<int>(fx::comp::Mode::Limit)) {
			return std::min(driven, s.ceilingDb);
		}
		float outDb;
		if (compander()) {
			outDb = firstDb(driven);
			if (paired()) {
				outDb = secondDb(outDb);
			}
		}
		else {
			outDb = fx::comp::staticCurveDb(driven, s.thresholdDb, s.ratio, s.kneeDb);
		}
		// MIX IS A SUM OF AMPLITUDES, so the wet's gain over the dry is taken in linear and the sum brought back to dB.
		const float wetOverDry = std::pow(10.0f, (outDb + s.makeupDb - xDb) / 20.0f);
		const float sum = (1.0f - s.mix) + s.mix * wetOverDry;
		return xDb + 20.0f * std::log10(std::max(sum, 1e-9f));
	}
};

/// THE SETTINGS A HOST HOLDS FOR AN INSTANCE, decoded as the DSP decodes them (`stateOf`), so the picture cannot promise
/// a range the device clamps away. `field(i)` reads control i.
template <typename Field>
[[nodiscard]] Curves curvesOf(const Field& field) {
	fx::comp::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(field(i));
	}
	return Curves { fx::comp::stateOf(row, row.size()) };
}

/// **THE METER'S THREE BARS, AS FRACTIONS** (the GR/meter shows on the popout window, not just the card) —
/// IN and OUT on the level scale, GR on the reduction scale, from the device's published `Reading`s and nothing else, so
/// the card and the window cannot draw two different meters. `has` is false while nothing is published (no meter is
/// drawn rather than a meter of zeros).
///
/// **GR IS THE NET REDUCTION** — `Reading::GainReduction`, which in Compander and Excomper is the two halves' gains
/// summed (`CompDsp::processCompander`), positive dB taken away; a net boost reads 0. Each half's own gain is on the plot
/// (its two faint curves); a second GR pair would need a second published reading, and the meter is one road.
struct Meter {
	bool has = false;
	float in = 0.0f;
	float out = 0.0f;
	float gr = 0.0f;
	friend bool operator==(const Meter&, const Meter&) = default;
};
/// THE METER FOR AN INSTANCE: `count` readings, `at(i)` reading i.
template <typename At>
[[nodiscard]] Meter meterOf(size_t count, const At& at) {
	if (count < static_cast<size_t>(fx::comp::Reading::kCount)) {
		return {};
	}
	const auto r = [&](fx::comp::Reading which) { return static_cast<float>(at(static_cast<size_t>(which))); };
	return { true, dyn_meter::levelFraction(r(fx::comp::Reading::InPeak)), dyn_meter::levelFraction(r(fx::comp::Reading::OutPeak)),
		     dyn_meter::grFraction(r(fx::comp::Reading::GainReduction)) };
}
/// DRAW IT INTO `box`: three bars and their captions, or "no signal".
void paintMeter(visage::Canvas& canvas, const visage::Bounds& box, const Meter& m);

/// PAINT THE PLOT INTO `box`. `inPeak` (linear) places the dot on the net curve when `hasPeak`.
void paint(visage::Canvas& canvas, const visage::Bounds& box, const Curves& c, bool hasPeak, float inPeak);

} // namespace feathers::ui::comp_plot
