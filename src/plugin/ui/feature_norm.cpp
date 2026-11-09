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

#include "plugin/ui/feature_norm.h"

#include "dsp/slice_build.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace feathers::ui {

namespace {

/// THE SAMPLING FRAME, written out rather than tuned, because a standardisation is only as honest as
/// the population it was measured over.
///
/// Homes span sixty-four units — some sixty-five thousand zones, which is several countries rather
/// than a neighbourhood — so the table describes the world instead of one corner of it. Travels cover
/// the plane a hand can reach either side of home. Rotations go round the circle, because the plane
/// turns and the features turn with it. Both engines, one table (see the header).
constexpr int kHomes = 32;
constexpr int kTravels = 16;
constexpr int kRotations = 8;
constexpr double kHomeSpan = 64.0;
constexpr double kTravelReach = 1.5;

void collect(std::vector<float>& into, float value) {
	into.push_back(value);
}

NormKnots quantilesOf(std::vector<float>& values) {
	std::sort(values.begin(), values.end());
	NormKnots knots {};
	for (int k = 0; k < kNormKnots; ++k) {
		// The k-th of kNormKnots evenly spaced quantiles, nearest-rank. Exact endpoints at both ends so
		// the map's domain is the observed range rather than an extrapolation off the end of the data.
		const double q = static_cast<double>(k) / static_cast<double>(kNormKnots - 1);
		const auto at = static_cast<size_t>(
		    std::llround(q * static_cast<double>(values.size() - 1)));
		knots[static_cast<size_t>(k)] = values[at];
	}
	// STRICTLY NON-DECREASING BY CONSTRUCTION — sorting guarantees it, and `standardize` relies on it
	// to search. Flat runs are legitimate: a feature that spends a tenth of the world at one value has
	// a flat tenth of table, and the standardisation maps that whole run to one place, which is the
	// honest thing for a value that carries no information there.
	return knots;
}

} // namespace

float standardize(const NormKnots& knots, float raw) {
	if (raw <= knots.front()) {
		return 0.0f;
	}
	if (raw >= knots.back()) {
		return 1.0f;
	}
	const auto upper = std::upper_bound(knots.begin(), knots.end(), raw);
	const size_t hi = static_cast<size_t>(std::distance(knots.begin(), upper));
	const size_t lo = hi - 1;
	const float span = knots[hi] - knots[lo];
	// A flat run means every value in it is the same measurement; put it at the run's own place rather
	// than dividing by zero to find out.
	const float within = span > 0.0f ? (raw - knots[lo]) / span : 0.0f;
	return (static_cast<float>(lo) + within) / static_cast<float>(kNormKnots - 1);
}

float destandardize(const NormKnots& knots, float u) {
	if (!(u > 0.0f)) {
		return knots.front();
	}
	if (u >= 1.0f) {
		return knots.back();
	}
	const float at = u * static_cast<float>(kNormKnots - 1);
	const auto lo = static_cast<size_t>(at);
	const float within = at - static_cast<float>(lo);
	return knots[lo] + (knots[lo + 1] - knots[lo]) * within;
}

TerrainFeatures channelFeaturesFor(OscType engine, const TerrainFeatures& features) {
	// RESONATE (2026-09-26) AND ADDITIVE (backlog LY, 2026-10-26) are each quantile-matched from their own table onto the shared one, so
	// their brightest ground glows as brightly and their roughest carries as much grain as the shared world's do.
	// AND PLUCK (2026-10-31), from its own table, every channel from its own feature: the string as heard is dimmed by how long
	// it rings (ks::heardSpectrum), so its energy is not degenerate the way a normalised partial sum is.
	// AND THE DRUM (2026-11-05), from its own table pooled over its six kinds.
	if (engine != OscType::Resonate && engine != OscType::Additive && engine != OscType::Pluck && engine != OscType::Drum) {
		return features;
	}
	TerrainFeatures matched = features;
	const FeatureNorm& own = engine == OscType::Resonate ? kBakedResonateFeatureNorm
	                         : engine == OscType::Pluck  ? kBakedPluckFeatureNorm
	                         : engine == OscType::Drum   ? kBakedDrumFeatureNorm
	                                                     : kBakedAdditiveFeatureNorm;
	const FeatureNorm& shared = kBakedFeatureNorm;
	// ADDITIVE'S ENERGY IS DEGENERATE (a normalised partial sum: about 0.25 for 40% of the world, measured), so no table can spread it; its
	// LUMINANCE IS READ FROM THE SPECTRAL CENTROID instead, through Additive's own baked centroid knots and onto the shared
	// energy knots, so a brighter-sounding place glows as brightly as the shared world's brightest. Every other channel keeps its own feature.
	matched.energy = destandardize(shared.energy, engine == OscType::Additive ? standardize(own.centroid, features.centroid)
	                                                                         : standardize(own.energy, features.energy));
	matched.centroid = destandardize(shared.centroid, standardize(own.centroid, features.centroid));
	matched.roughness = destandardize(shared.roughness, standardize(own.roughness, features.roughness));
	return matched;
}

namespace {

/// The frame itself, for any list of engines and (for Resonate) models: home, rotation, travel, in that
/// nesting, exactly as it has always been walked. The Morph/Stair walk below is this with those two
/// engines and the default model, so its visiting order — and therefore its table — is unchanged.
void walkFrame(std::initializer_list<OscType> engines, std::initializer_list<WeaveModel> models,
               const std::function<void(const TerrainFeatures&)>& visit) {
	for (const OscType engine : engines) {
		for (const WeaveModel model : models) {
			for (int h = 0; h < kHomes; ++h) {
				const double home =
				    kHomeSpan * (static_cast<double>(h) + 0.5) / static_cast<double>(kHomes);
				const auto zone = static_cast<uint16_t>(
				    std::clamp(std::llround(home * 1023.0) % 1024, 0LL, 1023LL));
				const double base = home - static_cast<double>(zone) / 1023.0;
				for (int r = 0; r < kRotations; ++r) {
					SliceOrientationPin pin;
					pin.pinned = true;
					pin.generator.rotate =
					    static_cast<float>(6.283185307 * static_cast<double>(r)
					                       / static_cast<double>(kRotations));
					for (int t = 0; t < kTravels; ++t) {
						const double travel =
						    kTravelReach
						    * (2.0 * (static_cast<double>(t) + 0.5) / static_cast<double>(kTravels) - 1.0);
						visit(terrainFeaturesAt(engine, pin, zone, 0.0f, travel, 0.0, kNoPlaneResidue, base,
						                        {}, WarpMode::Off, model));
					}
				}
			}
		}
	}
}

} // namespace

void forEachFrameSample(const std::function<void(const TerrainFeatures&)>& visit) {
	walkFrame({ OscType::Morph, OscType::Stair }, { WeaveModel::Ring }, visit);
}

void forEachAdditiveFrameSample(const std::function<void(const TerrainFeatures&)>& visit) {
	walkFrame({ OscType::Additive }, { WeaveModel::Ring }, visit);
}

void forEachPluckFrameSample(const std::function<void(const TerrainFeatures&)>& visit) {
	walkFrame({ OscType::Pluck }, { WeaveModel::Ring }, visit);
}

void forEachDrumFrameSample(const std::function<void(const TerrainFeatures&)>& visit) {
	// walkFrame's nesting exactly (home, rotation, travel), once per kind, each at neutral scalers.
	for (size_t k = 0; k < drum::kKindCount; ++k) {
		AdditivePicture picture;
		picture.drumKind = static_cast<drum::Kind>(k);
		for (int h = 0; h < kHomes; ++h) {
			const double home = kHomeSpan * (static_cast<double>(h) + 0.5) / static_cast<double>(kHomes);
			const auto zone = static_cast<uint16_t>(std::clamp(std::llround(home * 1023.0) % 1024, 0LL, 1023LL));
			const double base = home - static_cast<double>(zone) / 1023.0;
			for (int r = 0; r < kRotations; ++r) {
				SliceOrientationPin pin;
				pin.pinned = true;
				pin.generator.rotate = static_cast<float>(6.283185307 * static_cast<double>(r) / static_cast<double>(kRotations));
				for (int t = 0; t < kTravels; ++t) {
					const double travel =
					    kTravelReach * (2.0 * (static_cast<double>(t) + 0.5) / static_cast<double>(kTravels) - 1.0);
					visit(terrainFeaturesAt(OscType::Drum, pin, zone, 0.0f, travel, 0.0, kNoPlaneResidue, base, {},
					                        WarpMode::Off, WeaveModel::Ring, {}, {}, picture));
				}
			}
		}
	}
}

void forEachResonateFrameSample(const std::function<void(const TerrainFeatures&)>& visit) {
	walkFrame({ OscType::Resonate },
	          { WeaveModel::Ring, WeaveModel::Bar, WeaveModel::Tube, WeaveModel::Membrane, WeaveModel::Stiffening },
	          visit);
}

namespace {

using FrameWalk = std::function<void(const std::function<void(const TerrainFeatures&)>&)>;

/// The standardisation of whatever population `walk` visits: the knots, the parity projection and its
/// residual's knots, in three passes over the same samples.
FeatureNorm measureFrom(const FrameWalk& walk) {
	std::vector<float> energy;
	std::vector<float> centroid;
	std::vector<float> roughness;
	std::vector<float> parity;
	std::vector<float> fundamental;
	const size_t reserve = static_cast<size_t>(kHomes) * kTravels * kRotations * 2;
	energy.reserve(reserve);
	centroid.reserve(reserve);
	roughness.reserve(reserve);
	parity.reserve(reserve);
	fundamental.reserve(reserve);

	walk([&](const TerrainFeatures& f) {
		collect(energy, f.energy);
		collect(centroid, f.centroid);
		collect(roughness, f.roughness);
		collect(parity, f.parity);
		collect(fundamental, f.fundamental);
	});

	FeatureNorm norm;
	norm.energy = quantilesOf(energy);
	norm.centroid = quantilesOf(centroid);
	norm.roughness = quantilesOf(roughness);
	norm.parity = quantilesOf(parity);
	norm.fundamental = quantilesOf(fundamental);

	// SECOND PASS: the projection, fitted on the STANDARDISED pair, because that is the pair a colour
	// map sees. Ordinary least squares — the residual of an OLS fit is exactly uncorrelated with the
	// regressor, which is the property being bought, and nothing fancier buys more of it.
	double sx = 0.0;
	double sy = 0.0;
	double sxx = 0.0;
	double sxy = 0.0;
	double n = 0.0;
	walk([&](const TerrainFeatures& f) {
		const double x = standardize(norm.centroid, f.centroid);
		const double y = standardize(norm.parity, f.parity);
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
		n += 1.0;
	});
	const double denominator = n * sxx - sx * sx;
	const double slope = denominator != 0.0 ? (n * sxy - sx * sy) / denominator : 0.0;
	const double intercept = (sy - slope * sx) / n;
	norm.parityResidualSlope = static_cast<float>(slope);
	norm.parityResidualIntercept = static_cast<float>(intercept);

	// THIRD PASS: the residual has its own lumpy distribution — it is a difference of two things, so it
	// piles up in the middle — and it gets the same quantile treatment as everything else, or the map
	// would fill its square unevenly in a new way instead of the old one.
	std::vector<float> residual;
	residual.reserve(static_cast<size_t>(n));
	walk([&](const TerrainFeatures& f) {
		const float x = standardize(norm.centroid, f.centroid);
		const float y = standardize(norm.parity, f.parity);
		residual.push_back(y - (norm.parityResidualSlope * x + norm.parityResidualIntercept));
	});
	norm.parityResidual = quantilesOf(residual);
	return norm;
}

} // namespace

FeatureNorm measureFeatureNorm() {
	return measureFrom(forEachFrameSample);
}

FeatureNorm measureAdditiveFeatureNorm() {
	return measureFrom(forEachAdditiveFrameSample);
}

FeatureNorm measureResonateFeatureNorm() {
	return measureFrom(forEachResonateFrameSample);
}

FeatureNorm measurePluckFeatureNorm() {
	return measureFrom(forEachPluckFrameSample);
}

FeatureNorm measureDrumFeatureNorm() {
	return measureFrom(forEachDrumFrameSample);
}

const FeatureNorm& featureNormFor(OscType engine) {
	// Resonate and Additive (backlog LY) read their own knots; every other engine reads the shared ones.
	return engine == OscType::Resonate ? kBakedResonateFeatureNorm
	       : engine == OscType::Additive ? kBakedAdditiveFeatureNorm
	       : engine == OscType::Pluck    ? kBakedPluckFeatureNorm
	       : engine == OscType::Drum     ? kBakedDrumFeatureNorm
	                                     : kBakedFeatureNorm;
}

float residualParity(const FeatureNorm& norm, float centroid, float parity) {
	const float x = standardize(norm.centroid, centroid);
	const float y = standardize(norm.parity, parity);
	return standardize(norm.parityResidual, y - (norm.parityResidualSlope * x + norm.parityResidualIntercept));
}

} // namespace feathers::ui
