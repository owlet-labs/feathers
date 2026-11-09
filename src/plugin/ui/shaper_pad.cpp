/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/shaper_pad.h"
#include "plugin/ui/explore_pad_impl.h"

#include "dsp/slice_basis.h"

#include "fx/shaper/shaper_curve.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace shp = fx::shaper;

namespace {

/// THE ACTIVITY THAT COUNTS AS "EVERYTHING ALIVE". The six weights each run 0..1, but they are
/// half-rectified at 50% duty, so all six at once is vanishingly rare — MEASURED at 2.7% of
/// positions. Normalising against 6 would make the whole map dark; against the value three quarters
/// of positions actually reach, it uses its range.
/// **THE TERRAIN READS THE CURVE, NOT THE WEIGHTS — total variation of the transfer.**
///
/// The terrain uses curve analysis similar to Feathers'. This summed
/// the six blend weights, which answers "how much is present here" — a fair question, and not the one
/// a player is asking. Total variation is the summed absolute step along the transfer itself: **2.0
/// for the identity, rising with folding, continuously.** For a device that is a shaper/folder that IS
/// the character, and it is measured FROM the curve rather than inferred from its ingredients.
///
/// MEASURED across the field at 33x33 (`BI PROBE 15`): min 0.99, max 21.0, mean 4.27. The normaliser
/// is a DISPLAY choice taken from that spread — subtract the identity's 2.0 and divide by 8, which
/// puts the mean near a quarter and leaves the folded country bright, where dividing by the maximum
/// would compress almost the whole field into the bottom of the range.
constexpr float kIdentityVariation = 2.0f;
constexpr float kVariationFull = 8.0f;

/// HOW MANY POINTS THE ANALYSIS SAMPLES. MEASURED: the busiest curves reverse sixteen times across the
/// domain, so 48 steps is three per fold at the worst position in the field.
constexpr int kVariationSamples = 48;

/// THE MEASURED CEILINGS THE EXTRAS ARE NORMALISED BY — a 33x33 field grid at knee 0.5, harmonics
/// 0.5. Not laws of the device: readings, and they move if the field's tuning moves.
constexpr float kFeedbackCeiling = 0.8f;
constexpr float kRotationBits = 31.0f;

/// **HOW MUCH THE EXTRAS TERM WEIGHS AGAINST THE CURVE'S OWN VARIATION.** A DISPLAY choice, taken
/// from the two measured spreads rather than picked: across the same grid the base terrain has mean
/// 0.215 and the extras term with all five on has mean 0.372, so adding it whole would put the
/// average field at 0.59 and leave the bright country nowhere to go. At a half it lands near 0.40.
///
/// **AT ZERO ENABLED EXTRAS THE TERM IS EXACTLY ZERO**, so a device with its switches off draws the
/// terrain it drew before this existed — which is what makes this an addition rather than a
/// recalibration of a picture already in use.
constexpr float kExtrasWeight = 0.5f;
/// **WHAT THE TERRAIN SAYS ABOUT ONE CURVE — the transfer's total variation, normalised.**
///
/// Split out from `activityAt` when the terrain moved from the coordinate square to the plane: the
/// MEASURE is the same either way, and only the question of which curve to measure changed. One
/// definition so the two callers cannot drift.
[[nodiscard]] float variationOf(const shp::CurveParams& p) {
	float variation = 0.0f;
	float previous = shp::transferAt(p, -1.0f);
	for (int i = 1; i <= kVariationSamples; ++i) {
		const float x = -1.0f + 2.0f * static_cast<float>(i) / static_cast<float>(kVariationSamples);
		const float value = shp::transferAt(p, x);
		variation += std::fabs(value - previous);
		previous = value;
	}
	return std::clamp((variation - kIdentityVariation) / kVariationFull, 0.0f, 1.0f);
}

/// THE FIVE SCALERS AS THE STALENESS KEY — the amounts themselves since they became field scalers
/// (backlog II), because a picture weighted by an amount is stale when the amount moves, not only when
/// something crosses zero.
[[nodiscard]] std::array<float, 5> extrasKeyOf(const shp::Settings& s) {
	return { s.sub, s.feedback, s.mangle, s.lowpass, s.integrator };
}

/// **HOW MUCH THE TURNED-UP EXTRAS ARE DOING AT A POINT, 0..1.**
///
/// The terrain was rendered before the extras, so changing an extra did not change the terrain.
/// It could not do otherwise: the terrain is the total variation of
/// `transferAt`, and `transferAt` reads only the six weights, asymmetry, drive and preExpand. The
/// modifier amounts `deriveCurve` also produces — sub, comb, bit rotation, slew — are applied
/// per-sample AFTER the table read and appear in no static transfer at all.
///
/// **AND THEY WERE HIDING A LOT.** MEASURED across a 33x33 field grid at knee 0.5, harmonics 0.5:
/// subIntensity spans 0.000-0.993, feedback 0.000-0.800, slewIntensity 0.000-0.999, rotation 0-30
/// bits and the comb's stride 1-127 samples. With a switch on, most of what varies across the plane
/// was invisible in the picture the player navigates by.
///
/// **FOUR OF THE FIVE CANNOT ENTER A TRANSFER, WHICH IS WHY THIS IS A SEPARATE TERM RATHER THAN A
/// BIGGER CURVE.** The comb has a delay, the sub tracks zero crossings, and the lowpass and
/// integrator have memory — they depend on where the signal came from, not only where it is, which is
/// the same reason the PLOT draws no modifiers. Only the bit rotation is memoryless. So this does not
/// pretend to be a transfer; it is an activity reading placed beside one.
///
/// **AVERAGED OVER THE ENABLED TERMS, NOT ALL OF THEM**, so turning on one extra shows that one's
/// structure at full strength instead of diluting it with three zeros. Each is normalised by its own
/// MEASURED ceiling, above. Lowpass and integrator share `slewIntensity` — the field's own note says
/// it drives both — so they count once between them, at the larger of their two scaled amounts.
[[nodiscard]] float extrasActivityOf(const shp::CurveParams& p, const shp::Settings& s) {
	float sum = 0.0f;
	int terms = 0;
	const auto add = [&](float value) {
		sum += std::clamp(value, 0.0f, 1.0f);
		++terms;
	};
	// EACH WEIGHED BY WHAT IS HEARD — the field scaled by its knob, by the law `curveOf` uses, so the
	// picture at a point is the activity the engine would run there (backlog II). Comb depth and
	// rotation scale the field's TRIANGLE, as `curveOf` does, and are re-mapped by the firmware's own.
	namespace phi = owlet::dsp::phi;
	if (s.sub > 0.0f) {
		add(feathers::fieldWithOverride(p.subIntensity, s.sub));
	}
	if (s.feedback > 0.0f) {
		add(phi::feedbackFromTriangle(feathers::fieldWithOverride(p.extrasField[2], s.feedback)) / kFeedbackCeiling);
	}
	if (s.mangle > 0.0f) {
		add(static_cast<float>(phi::rotationFromTriangle(feathers::fieldWithOverride(p.extrasField[3], s.mangle)))
		    / kRotationBits);
	}
	if (s.lowpass > 0.0f || s.integrator > 0.0f) {
		add(std::max(feathers::fieldWithOverride(p.slewIntensity, s.lowpass),
		             feathers::fieldWithOverride(p.slewIntensity, s.integrator)));
	}
	return terms == 0 ? 0.0f : sum / static_cast<float>(terms);
}

/// THE FIVE SCALERS AS THE TERRAIN READS THEM — each switch's amount while it is on, zero while it is off.
[[nodiscard]] shp::Settings extrasOf(const ShaperPadPolicy::RowReader& row) {
	const auto f = [&](shp::Param p) { return row(static_cast<size_t>(p)); };
	shp::Settings extras;
	extras.sub = f(shp::Param::SubOn) >= 0.5f ? f(shp::Param::Sub) : 0.0f;
	extras.feedback = f(shp::Param::FeedbackOn) >= 0.5f ? f(shp::Param::Feedback) : 0.0f;
	extras.mangle = f(shp::Param::MangleOn) >= 0.5f ? f(shp::Param::Mangle) : 0.0f;
	extras.lowpass = f(shp::Param::LowpassOn) >= 0.5f ? f(shp::Param::Lowpass) : 0.0f;
	extras.integrator = f(shp::Param::IntegratorOn) >= 0.5f ? f(shp::Param::Integrator) : 0.0f;
	return extras;
}

/// Colour's mode as the terrain reads it: -1 in Free, the Inflate amount in Inflate — see `Key::inflate`.
[[nodiscard]] double inflateOf(const ShaperPadPolicy::RowReader& row) {
	const long mode = std::lround(row(static_cast<size_t>(shp::Param::ColourMode)));
	if (mode != static_cast<long>(shp::kColourInflate)) {
		return -1.0;
	}
	return static_cast<double>(std::clamp(row(static_cast<size_t>(shp::Param::InflateAmount)), 0.0f, 1.0f));
}

} // namespace

float ShaperPad::activityAt(float colour, float gamma) {
	const shp::CurveParams p = shp::deriveCurve(0.75f, colour, gamma, 0.5f);
	// KNEE IS PASSED AS A CONSTANT AND THAT IS CORRECT, not a shortcut. The six weights are functions
	// of colour and gamma alone — knee reaches `drive`, `preExpandAmount` and the basis internals, and
	// none of the weights. Which is exactly why this map can be cached at all.
	// THE TRANSFER, WALKED — see `variationOf`, which both this and the plane terrain call.
	// `transferAt` is the same per-point evaluator `fillTable` uses, so this is
	// the curve the device will actually build and not a second opinion about it.
	return variationOf(p);
}

SliceBasis<ShaperPadPolicy::kPhaseCount> ShaperPadPolicy::basisAt(const RowReader& row, double rotate,
                                                                  const Phases& origin) {
	namespace ps = shp::phase_space;
	// THE SHAPER'S HOME MOVES WITH COLOUR AND GAMMA, and the standing travel rides on it.
	const double colour = static_cast<double>(row(static_cast<size_t>(shp::Param::Colour)));
	const double gamma = static_cast<double>(row(static_cast<size_t>(shp::Param::Gamma)));
	ps::Phases home = ps::homeFor(colour, gamma);
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		home[i] += origin[i];
	}
	return feathers::sliceBasisFor<ps::kPhaseCount>(rotate, ps::planeHomeOf(home));
}

void ShaperPadPolicy::paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane) {
	// EACH CELL THE CURVE A PRESS THERE WOULD BUILD, walked for its total variation, plus the extras' activity.
	// **EVERY INPUT FROM THE KEY**, each the float the row held: the key widened knee, harmonics and the scalers to
	// double or kept them as float, so narrowing back is exact and the arithmetic below is the row-read draft's.
	const double colour = key.colour;
	const double gamma = key.gamma;
	const auto knee = static_cast<float>(key.knee);
	const auto harmonics = static_cast<float>(key.harmonics);
	shp::Settings extras;
	extras.sub = key.extras[0];
	extras.feedback = key.extras[1];
	extras.mangle = key.extras[2];
	extras.lowpass = key.extras[3];
	extras.integrator = key.extras[4];
	const double inflate = key.inflate;
	map.build([&](float ux, float uy) {
		const std::array<float, kPhaseCount> travel = plane.travelAt(ux, uy);
		shp::CurveParams p = shp::deriveCurve(knee, static_cast<float>(colour), static_cast<float>(gamma), harmonics,
		                                      &travel);
		// COLOUR'S INFLATE MODE (backlog IW) — the terrain shows the curve the device builds, mode and all.
		if (inflate >= 0.0) {
			shp::applyInflate(p, static_cast<float>(inflate));
		}
		return std::clamp(variationOf(p) + kExtrasWeight * extrasActivityOf(p, extras), 0.0f, 1.0f);
	});
}

ShaperPadPolicy::Key ShaperPadPolicy::keyOf(const RowReader& row, double rotate, double zoom, const Phases& standing) {
	// EVERY INPUT `paint` READS, read the same way it reads them — see `Key`'s own rule.
	Key key { static_cast<double>(row(static_cast<size_t>(shp::Param::Colour))),
		      static_cast<double>(row(static_cast<size_t>(shp::Param::Gamma))),
		      rotate,
		      zoom,
		      static_cast<double>(row(static_cast<size_t>(shp::Param::Knee))),
		      static_cast<double>(shp::kSlewHarmonics),
		      extrasKeyOf(extrasOf(row)),
		      standing,
		      inflateOf(row) };
	return key;
}

template class ExplorePad<ShaperPadPolicy>;

} // namespace feathers::ui
