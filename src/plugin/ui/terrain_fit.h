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

#include "plugin/ui/terrain.h"

#include <array>
#include <atomic>
#include <cstdint>

// THE FITTED TERRAIN MAP (an audition): the Explore map's per-cell features for Additive and V5, predicted from the
// engine's DECODED parameters by polynomials fitted offline (tools/fit-terrain-map.cpp) against the sound the engine
// really renders, instead of measured from a 32-sample cycle built at every cell.
//
// THE FIT IS A PICTURE OF THE SOUND AT DEFAULT CONTROLS, and says so: it was trained on the engine at its neutral
// knobs, so what it reads from the selected sound is the six Additive bias knobs and Smoothness (they act on the
// decoded fields, which the decode here applies exactly as the bank does) and nothing else. V5's field scalers,
// the Shape Amount and the likes' phase warp are not modelled. Where the fit cannot resolve a view, it draws what it
// gives: the report (docs/terrain-fit-report.txt) lists the zooms.
//
// NOT SAVED WITH THE PATCH, and nothing here is state: the table is generated code, the switch is the editor's.

namespace feathers::ui {

/// How many features a cell carries. They are TerrainFeatures' five members, in this order, and a fitted map must supply
/// every one: the colour reads energy (luminance), centroid (hue and tilt), roughness (grain, ember) and parity (the
/// second axis); fundamental is carried through the cell and the upsampler.
inline constexpr int kFitChannels = 5;
inline constexpr int kFitAdditiveInputs = 20; ///< the clamped, smoothed Additive fields, in PartialFields' order
inline constexpr int kFitV5Inputs = 38;       ///< 26 morph-table reads, 7 operator authors, 5 vowel reads
inline constexpr int kFitMaxInputs = 40;
/// The generated table's layout version: the evaluator refuses a table of another one.
inline constexpr int kFitFormatVersion = 1;

/// One channel's model: a polynomial in the standardised inputs. Layout of `coef`: 1, then x_i, then x_i*x_j for i <= j,
/// then (kind == kCubic only) x_i^3 — the same order the tool's design matrix has, so the table IS the fit.
struct FitChannelModel {
	int kind = 0;          ///< kQuadratic or kCubic
	int ncoef = 0;
	double lo = 0.0;       ///< the range the target was seen to take: a prediction is held inside it
	double hi = 0.0;
	const double* coef = nullptr;
};
inline constexpr int kFitQuadratic = 1;
inline constexpr int kFitCubic = 4; ///< "restricted cubic": per-input cubes beside the quadratic terms

struct FitEngineTable {
	int version = 0;
	int engine = 0; ///< OscType
	int inputs = 0;
	const double* mean = nullptr;
	const double* sd = nullptr;
	std::array<FitChannelModel, kFitChannels> channel {};
};

/// The generated tables (terrain_fit_table.cpp), and null for an engine with none.
const FitEngineTable* terrainFitTableFor(OscType engine);

/// Whether the engine has a fitted table: the editor offers the switch for those and hides it for the rest.
bool terrainFitAvailable(OscType engine);
int terrainFitInputs(OscType engine);

/// THE LEAN DECODE, once per cell and shared by every channel: what the engine reads from the place before it builds
/// anything. Additive: the 20 fields through the sound's bias and Smoothness, clamped and smoothed as buildSound does
/// (no spectrum is built). V5: the morph table's seven phases, amplitudes and eight curvatures and four scalars, the
/// seven operator authors, and the vowel's species and formants. Writes `terrainFitInputs(engine)` values to `out`
/// and returns that count (0 for an engine the decode does not cover). It does not need a table: the offline tool
/// decodes with it before one exists.
int terrainFitDecode(OscType engine, const TerrainFrames& frames, uint16_t zone, float offset, double travelX,
                     double travelY, double gammaBase, const feathers::FieldScales& scales,
                     const AdditivePicture& additive, const feathers::PhaseWarp* warp, double* out);

/// The five predictions for decoded inputs, in TerrainFeatures' order, each held inside its training range.
/// Takes the RAW decode (the table standardises it). No allocation.
std::array<double, kFitChannels> terrainFitPredict(const FitEngineTable& table, const double* inputs);

/// THE FITTED CELL: decode, predict, and hand back what terrainFeaturesOn would have measured. Counts itself
/// (terrainFitEvaluations) so a case can say the path was, or was not, taken.
TerrainFeatures terrainFeaturesFitted(OscType engine, const TerrainFrames& frames, uint16_t zone, float offset,
                                      double travelX, double travelY, double gammaBase, const feathers::FieldScales& scales,
                                      const AdditivePicture& additive, const feathers::PhaseWarp* warp);

/// HOW MANY CELLS THE FITTED PATH HAS EVALUATED, process-wide: a test seam (relaxed; read it settled).
uint64_t terrainFitEvaluations();

} // namespace feathers::ui
