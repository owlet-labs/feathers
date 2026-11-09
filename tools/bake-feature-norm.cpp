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

// BAKE THE FEATURE STANDARDISATION, and print what the features look like while it is in there.
//
// The standardisation is part of artifact generation rather than a thing anyone has
// to remember: this writes the checked-in table, and `feature-norm is not stale` in the widget suite
// recomputes it and compares, so the suite goes red the day the world moves under it. No CI involved.
//
//   ninja -C build bake-feature-norm && build/bake-feature-norm > src/plugin/ui/feature_norm_baked.cpp
//   build/bake-feature-norm --report     # the distributions and the correlation table, to stderr

#include "plugin/ui/feature_norm.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace feathers::ui;

namespace {

void emit(const char* name, const NormKnots& knots) {
	std::printf("\t\t{ ");
	for (int i = 0; i < kNormKnots; ++i) {
		// %e RATHER THAN %g, and it is not a style choice: %g prints an exact 1.0 as "1", and "1f" is
		// not a float literal — the first bake of this table would not compile, which is a fine way to
		// find out and a poor way to ship. An exponent is always present in %e, so the suffix is always
		// legal.
		std::printf("%.9ef%s", static_cast<double>(knots[static_cast<size_t>(i)]),
		            i + 1 == kNormKnots ? " " : ", ");
		if ((i + 1) % 6 == 0 && i + 1 != kNormKnots) {
			std::printf("\n\t\t  ");
		}
	}
	std::printf("}, // %s\n", name);
}

void emitTable(const char* name, const FeatureNorm& norm) {
	std::printf("const FeatureNorm %s = {\n", name);
	emit("energy", norm.energy);
	emit("centroid", norm.centroid);
	emit("roughness", norm.roughness);
	emit("parity", norm.parity);
	emit("fundamental", norm.fundamental);
	std::printf("\t\t%.9ef, // parityResidualSlope\n", static_cast<double>(norm.parityResidualSlope));
	std::printf("\t\t%.9ef, // parityResidualIntercept\n", static_cast<double>(norm.parityResidualIntercept));
	emit("parityResidual", norm.parityResidual);
	std::printf("};\n\n");
}

} // namespace

int main(int argc, char** argv) {
	const bool report = argc > 1 && std::strcmp(argv[1], "--report") == 0;
	const FeatureNorm norm = measureFeatureNorm();

	if (report) {
		// The distributions, so the shape the standardisation is correcting is on the record.
		const std::array<std::pair<const char*, const NormKnots*>, 5> all = { {
		    { "energy", &norm.energy },
		    { "centroid", &norm.centroid },
		    { "roughness", &norm.roughness },
		    { "parity", &norm.parity },
		    { "fundamental", &norm.fundamental },
		} };
		std::fprintf(stderr, "%-12s %8s %8s %8s %8s %8s\n", "feature", "min", "p25", "median", "p75",
		             "max");
		for (const auto& [name, knots] : all) {
			std::fprintf(stderr, "%-12s %8.4f %8.4f %8.4f %8.4f %8.4f\n", name, (*knots)[0],
			             (*knots)[8], (*knots)[16], (*knots)[24], (*knots)[kNormKnots - 1]);
		}

		// AND HOW INDEPENDENT THEY ACTUALLY ARE, because "a genuinely independent axis" should be a
		// number rather than a hope. Correlation is taken on the STANDARDISED values, which is what a
		// colour map will see — two features can be nonlinearly related and still fill a map's square,
		// and it is the filled square that matters here.
		std::vector<std::array<float, 5>> rows;
		forEachFrameSample([&](const feathers::ui::TerrainFeatures& f) {
			rows.push_back({ standardize(norm.energy, f.energy),
			                 standardize(norm.centroid, f.centroid),
			                 standardize(norm.roughness, f.roughness),
			                 standardize(norm.parity, f.parity),
			                 standardize(norm.fundamental, f.fundamental) });
		});
		const char* names[5] = { "energy", "centroid", "roughness", "parity", "fundamental" };
		std::fprintf(stderr, "\ncorrelation of the STANDARDISED features (r), over %zu samples\n",
		             rows.size());
		std::fprintf(stderr, "%-12s", "");
		for (const char* n : names) {
			std::fprintf(stderr, "%12s", n);
		}
		std::fprintf(stderr, "\n");
		for (int a = 0; a < 5; ++a) {
			std::fprintf(stderr, "%-12s", names[a]);
			for (int b = 0; b < 5; ++b) {
				double ma = 0.0;
				double mb = 0.0;
				for (const auto& row : rows) {
					ma += row[static_cast<size_t>(a)];
					mb += row[static_cast<size_t>(b)];
				}
				ma /= static_cast<double>(rows.size());
				mb /= static_cast<double>(rows.size());
				double cov = 0.0;
				double va = 0.0;
				double vb = 0.0;
				for (const auto& row : rows) {
					const double da = row[static_cast<size_t>(a)] - ma;
					const double db = row[static_cast<size_t>(b)] - mb;
					cov += da * db;
					va += da * da;
					vb += db * db;
				}
				std::fprintf(stderr, "%12.3f", cov / std::sqrt(va * vb));
			}
			std::fprintf(stderr, "\n");
		}
		return 0;
	}

	std::printf("// GENERATED by tools/bake-feature-norm — do not edit by hand.\n");
	std::printf("//\n");
	std::printf("// Regenerate with:\n");
	std::printf("//   ninja -C build bake-feature-norm && build/bake-feature-norm > "
	            "src/plugin/ui/feature_norm_baked.cpp\n");
	std::printf("//\n");
	std::printf("// The suite's `feature-norm is not stale` case recomputes this table from the world\n");
	std::printf("// and compares, so a character-layer change that moves the distributions turns the\n");
	std::printf("// suite red rather than silently re-colouring every map.\n\n");
	std::printf("#include \"plugin/ui/feature_norm.h\"\n\n");
	std::printf("namespace feathers::ui {\n\n");
	emitTable("kBakedFeatureNorm", norm);
	// AND RESONATE'S OWN (2026-09-25), over its own frame — the same homes, rotations and travels, once per
	// physics model. The table above is printed first and exactly as before, so its bytes do not move.
	emitTable("kBakedResonateFeatureNorm", measureResonateFeatureNorm());
	// AND ADDITIVE'S OWN (backlog LY, 2026-10-26), over its own frame; the two tables above are printed exactly as before.
	emitTable("kBakedAdditiveFeatureNorm", measureAdditiveFeatureNorm());
	// AND PLUCK'S OWN (2026-10-31), over its own frame; the tables above are printed exactly as before.
	emitTable("kBakedPluckFeatureNorm", measurePluckFeatureNorm());
	// AND THE DRUM'S OWN (2026-11-05), every kind pooled; the tables above are printed exactly as before.
	emitTable("kBakedDrumFeatureNorm", measureDrumFeatureNorm());
	std::printf("} // namespace feathers::ui\n");
	return 0;
}
