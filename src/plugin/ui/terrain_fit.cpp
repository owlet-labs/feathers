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

#include "plugin/ui/terrain_fit.h"

#include "dsp/partial_spectrum.h"
#include "dsp/phase_warp.h"
#include "dsp/slice_build.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {

std::atomic<uint64_t> g_evaluations { 0 };

float clampUnit(float x) {
	return x > 0.0f ? std::min(x, 1.0f) : 0.0f; // the engine's own clamp, NaN to 0
}

int decodeAdditive(const TerrainFrames& frames, double home, double travelX, double travelY,
                   const feathers::FieldScales& scales, const AdditivePicture& additive,
                   const feathers::PhaseWarp* warp, double* out) {
	const feathers::AngleWarpScope likes(warp);
	const feathers::AngleKindScope sites(feathers::AngleKind::Partials);
	// What buildPartialSound hands buildSound, and what buildSound does with it (clamp, then smooth) — and nothing
	// after: no detail triangles, no envelope, no partial is built.
	partials::PartialFields f = feathers::partialFieldsWithOverrides(
	    feathers::readPartialFields(frames.partials, home, travelX, travelY), scales.smoothness, additive.bias);
	for (float* v : { &f.rolloff, &f.formant1Centre, &f.formant1Width, &f.formant1Gain, &f.formant2Centre,
	                  &f.formant2Width, &f.formant2Gain, &f.oddEven, &f.combSpacing, &f.combDepth, &f.detail,
	                  &f.offGrid, &f.smoothness, &f.knee, &f.shelf, &f.shelfAt, &f.body, &f.bodyAt, &f.sparsity,
	                  &f.sparseSet }) {
		*v = clampUnit(*v);
	}
	f = partials::smoothed(f);
	const float v[kFitAdditiveInputs] = { f.rolloff, f.formant1Centre, f.formant1Width, f.formant1Gain,
		                                  f.formant2Centre, f.formant2Width, f.formant2Gain, f.oddEven,
		                                  f.combSpacing, f.combDepth, f.detail, f.offGrid, f.smoothness, f.knee,
		                                  f.shelf, f.shelfAt, f.body, f.bodyAt, f.sparsity, f.sparseSet };
	for (int i = 0; i < kFitAdditiveInputs; ++i) {
		out[i] = static_cast<double>(v[i]);
	}
	return kFitAdditiveInputs;
}

} // namespace

bool terrainFitAvailable(OscType engine) {
	const FitEngineTable* table = terrainFitTableFor(engine);
	return table != nullptr && table->version == kFitFormatVersion;
}

int terrainFitInputs(OscType engine) {
	// A property of the decode, not of the table: the offline tool decodes before any table exists.
	return engine == OscType::Additive ? kFitAdditiveInputs : 0;
}

int terrainFitDecode(OscType engine, const TerrainFrames& frames, uint16_t zone, float offset, double travelX,
                     double travelY, double gammaBase, const feathers::FieldScales& scales,
                     const AdditivePicture& additive, const feathers::PhaseWarp* warp, double* out) {
	const double home = feathers::sliceHome(zone, offset, gammaBase);
	if (engine == OscType::Additive) {
		return decodeAdditive(frames, home, travelX, travelY, scales, additive, warp, out);
	}
	return 0;
}

std::array<double, kFitChannels> terrainFitPredict(const FitEngineTable& table, const double* inputs) {
	const int n = table.inputs;
	double x[kFitMaxInputs];
	for (int i = 0; i < n; ++i) {
		x[i] = (inputs[i] - table.mean[i]) / table.sd[i];
	}
	// THE DESIGN VECTOR, built once and read by every channel: 1, x, the upper triangle of x x^T, then the cubes. A
	// quadratic channel reads the front of it.
	constexpr int kMaxTerms = 1 + kFitMaxInputs + kFitMaxInputs * (kFitMaxInputs + 1) / 2 + kFitMaxInputs;
	double design[kMaxTerms];
	int c = 0;
	design[c++] = 1.0;
	for (int i = 0; i < n; ++i) {
		design[c++] = x[i];
	}
	for (int i = 0; i < n; ++i) {
		for (int j = i; j < n; ++j) {
			design[c++] = x[i] * x[j];
		}
	}
	for (int i = 0; i < n; ++i) {
		design[c++] = x[i] * x[i] * x[i];
	}
	std::array<double, kFitChannels> out {};
	for (int ch = 0; ch < kFitChannels; ++ch) {
		const FitChannelModel& model = table.channel[static_cast<size_t>(ch)];
		// FOUR RUNNING SUMS, because one is a dependency chain of adds the compiler may not reorder (no fast-math here):
		// the dot is the cost of the fitted road, and this is the same polynomial to a few ulp.
		double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
		int k = 0;
		for (; k + 4 <= model.ncoef; k += 4) {
			s0 += model.coef[k] * design[k];
			s1 += model.coef[k + 1] * design[k + 1];
			s2 += model.coef[k + 2] * design[k + 2];
			s3 += model.coef[k + 3] * design[k + 3];
		}
		for (; k < model.ncoef; ++k) {
			s0 += model.coef[k] * design[k];
		}
		const double y = (s0 + s1) + (s2 + s3);
		out[static_cast<size_t>(ch)] = std::clamp(y, model.lo, model.hi);
	}
	return out;
}

TerrainFeatures terrainFeaturesFitted(OscType engine, const TerrainFrames& frames, uint16_t zone, float offset,
                                      double travelX, double travelY, double gammaBase, const feathers::FieldScales& scales,
                                      const AdditivePicture& additive, const feathers::PhaseWarp* warp) {
	g_evaluations.fetch_add(1, std::memory_order_relaxed);
	const FitEngineTable* table = terrainFitTableFor(engine);
	TerrainFeatures features;
	if (table == nullptr) {
		return features;
	}
	double in[kFitMaxInputs];
	terrainFitDecode(engine, frames, zone, offset, travelX, travelY, gammaBase, scales, additive, warp, in);
	const std::array<double, kFitChannels> p = terrainFitPredict(*table, in);
	features.energy = static_cast<float>(p[0]);
	features.centroid = static_cast<float>(p[1]);
	features.roughness = static_cast<float>(p[2]);
	features.parity = static_cast<float>(p[3]);
	features.fundamental = static_cast<float>(p[4]);
	return features;
}

uint64_t terrainFitEvaluations() {
	return g_evaluations.load(std::memory_order_relaxed);
}

} // namespace feathers::ui
