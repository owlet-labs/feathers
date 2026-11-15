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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/ui/terrain.h"
#include "plugin/ui/terrain_fit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>

// THE FITTED TERRAIN MAP'S EVALUATOR, against the offline fit it implements. The fixture below is WRITTEN BY THE OFFLINE
// TOOL (tools/fit-terrain-map.cpp): per held-out stance, the decoded inputs and the fit's own prediction, computed there by
// a dense dot of the design row with the coefficients and not by this code. The evaluator and the tool share nothing but
// the table.

#include "fixtures/terrain_fit_sample.inc"

using namespace feathers;
using namespace feathers::ui;

namespace {

struct Sample {
	OscType engine;
	const FitSampleRow* rows;
	size_t count;
};

const Sample kSamples[] = {
	{ OscType::Additive, kFitSample_additive, std::size(kFitSample_additive) },
	{ OscType::V5, kFitSample_v5, std::size(kFitSample_v5) },
};

int termCountOf(int n, int kind) {
	return 1 + n + n * (n + 1) / 2 + (kind == kFitCubic ? n : 0);
}

} // namespace

TEST_CASE("the evaluator reproduces the offline fit's predictions on the committed sample within 1e-5") {
	for (const Sample& sample : kSamples) {
		REQUIRE(sample.count >= 200);
		const FitEngineTable* table = terrainFitTableFor(sample.engine);
		REQUIRE(table != nullptr);
		double worst = 0.0;
		double spread = 0.0; // how far the predictions range, so the bound is not met by a constant
		std::array<double, kFitChannels> lo, hi;
		lo.fill(1e300);
		hi.fill(-1e300);
		for (size_t r = 0; r < sample.count; ++r) {
			const FitSampleRow& row = sample.rows[r];
			const std::array<double, kFitChannels> got = terrainFitPredict(*table, row.in);
			for (size_t ch = 0; ch < kFitChannels; ++ch) {
				worst = std::max(worst, std::abs(got[ch] - row.predicted[ch]));
				lo[ch] = std::min(lo[ch], row.predicted[ch]);
				hi[ch] = std::max(hi[ch], row.predicted[ch]);
			}
		}
		for (size_t ch = 0; ch < kFitChannels; ++ch) {
			spread = std::max(spread, hi[ch] - lo[ch]);
			CHECK(hi[ch] - lo[ch] > 0.05); // every channel moves across the sample
		}
		INFO("engine " << static_cast<int>(sample.engine) << ": worst |evaluator - offline| " << worst << ", prediction spread " << spread);
		CHECK(worst < 1e-5);
	}
}

TEST_CASE("the decode gives the inputs the offline fit was shown, from the stance alone") {
	for (const Sample& sample : kSamples) {
		const int n = terrainFitInputs(sample.engine);
		REQUIRE(n > 0);
		double worst = 0.0;
		for (size_t r = 0; r < sample.count; ++r) {
			const FitSampleRow& row = sample.rows[r];
			SliceOrientationPin pin;
			pin.pinned = true;
			pin.generator.rotate = static_cast<float>(row.rotate);
			const float phaseOffset = static_cast<float>(row.offset + row.gamma);
			const uint16_t zone = static_cast<uint16_t>(row.zone);
			const TerrainFrames frames = terrainFramesAt(pin, sliceHome(zone, phaseOffset, 0.0), kNoPlaneResidue);
			double in[kFitMaxInputs];
			REQUIRE(terrainFitDecode(sample.engine, frames, zone, phaseOffset, row.tx, row.ty, 0.0, {}, {}, nullptr, in) == n);
			for (int i = 0; i < n; ++i) {
				worst = std::max(worst, std::abs(in[i] - row.in[i]));
			}
		}
		INFO("engine " << static_cast<int>(sample.engine) << ": worst decode difference " << worst);
		CHECK(worst < 1e-4);
	}
}

TEST_CASE("terrainFeaturesFitted maps the five predictions onto the cell's features, in order, and counts each evaluation") {
	for (const Sample& sample : kSamples) {
		for (size_t r = 0; r < 40; ++r) {
			const FitSampleRow& row = sample.rows[r];
			SliceOrientationPin pin;
			pin.pinned = true;
			pin.generator.rotate = static_cast<float>(row.rotate);
			const float phaseOffset = static_cast<float>(row.offset + row.gamma);
			const uint16_t zone = static_cast<uint16_t>(row.zone);
			const TerrainFrames frames = terrainFramesAt(pin, sliceHome(zone, phaseOffset, 0.0), kNoPlaneResidue);
			const uint64_t before = terrainFitEvaluations();
			const TerrainFeatures f = terrainFeaturesFitted(sample.engine, frames, zone, phaseOffset, row.tx, row.ty, 0.0, {}, {}, nullptr);
			CHECK(terrainFitEvaluations() == before + 1);
			// Order: energy, centroid, roughness, parity, fundamental (TerrainFeatures' own); the offline predictions are the
			// reference, to the decode's own tolerance.
			CHECK(std::abs(static_cast<double>(f.energy) - row.predicted[0]) < 1e-3);
			CHECK(std::abs(static_cast<double>(f.centroid) - row.predicted[1]) < 1e-3);
			CHECK(std::abs(static_cast<double>(f.roughness) - row.predicted[2]) < 1e-3);
			CHECK(std::abs(static_cast<double>(f.parity) - row.predicted[3]) < 1e-3);
			CHECK(std::abs(static_cast<double>(f.fundamental) - row.predicted[4]) < 1e-3);
		}
	}
}

TEST_CASE("the generated table is the shape the evaluator reads, and only Additive and V5 have one") {
	for (const Sample& sample : kSamples) {
		const FitEngineTable* table = terrainFitTableFor(sample.engine);
		REQUIRE(table != nullptr);
		CHECK(terrainFitAvailable(sample.engine));
		CHECK(table->version == kFitFormatVersion);
		CHECK(table->engine == static_cast<int>(sample.engine));
		CHECK(table->inputs == terrainFitInputs(sample.engine));
		for (int i = 0; i < table->inputs; ++i) {
			CHECK(table->sd[i] > 0.0);
		}
		for (const FitChannelModel& channel : table->channel) {
			CHECK((channel.kind == kFitQuadratic || channel.kind == kFitCubic));
			CHECK(channel.ncoef == termCountOf(table->inputs, channel.kind));
			CHECK(channel.lo < channel.hi);
			CHECK(channel.coef != nullptr);
		}
	}
	for (const OscType engine : { OscType::Morph, OscType::Stair, OscType::Resonate, OscType::BasicShapes, OscType::Pluck, OscType::Drum }) {
		CHECK_FALSE(terrainFitAvailable(engine));
		CHECK(terrainFitTableFor(engine) == nullptr);
		CHECK(terrainFitInputs(engine) == 0);
	}
}
