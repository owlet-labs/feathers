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

// HOW MANY DIMENSIONS OF THE CHARACTER SPACE CAN ACTUALLY BE REACHED.
//
// THE QUESTION. Each engine evaluates N φ-triangles, and the slice machinery gives every one of them
// a phase coordinate and a rate coordinate — so the full recipe space is 2N-dimensional. The question
// is whether rotation makes that whole space explorable, because if it does not an unbounded
// gamma is wanted instead. The number decides which project exists, so it is measured here rather than argued.
//
// THREE THINGS ARE COMPUTED, per engine, over both coordinate halves:
//
//   1. THE HOME DIRECTION'S RANK. Moving home sends every triangle's phase along its own rate, so the
//      home line's closure in the N-torus is a subtorus whose dimension is the RATIONAL RANK of the
//      rate vector — the dimension of the ℚ-vector space the rates span. The frame this was set in
//      says that rank is 2, because every φ-power is a + b·φ and ℚ(φ) is two-dimensional over ℚ.
//      That argument holds for INTEGER powers. The banks are checked here against it rather than
//      assumed to obey it.
//
//   2. THE BASIS SPAN. Travel reaches a 2-plane; the union of those planes over every generator is
//      what rotation can address. Measured two ways that have to agree: STRUCTURALLY, by reading
//      which coordinate indices the rotation planes name at all, and NUMERICALLY, by sampling
//      generators, stacking every basis row, and taking singular values of the stack.
//
//   3. THE VERDICT. Reachable dimension d against the 2N total, which coordinates are unreachable
//      whatever anyone does, and what widening the reach would cost.
//
// MEASUREMENT OVER PREDICTION, which is the day's law and was earned: a float-vs-double period
// already cost this project an afternoon of confident wrong reasoning. Every number below is
// computed from the shipped constants and the shipped basis code, in this process, and printed with
// the arithmetic that produced it.
//
//   ninja -C build rank-census && build/rank-census

#include "dsp/slice_build.h"

#include "character/phi_triangle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace owlet::dsp;
using feathers::kMorphTriangles;
using feathers::kStairTriangles;

//----------------------------------------------------------------------------------------------//
// The rates, in slot order                                                                       //
//----------------------------------------------------------------------------------------------//

/// Every triangle's frequency multiplier, indexed exactly as the basis indexes it.
///
/// Assembled from the same banks `slice_build.cpp` reads and in the same slot order, because a rate
/// table transcribed by hand would be a measurement of my typing. The two single-triangle spatial
/// patterns are written inline at their call sites there, so they are written inline here too and
/// the values are quoted beside them.
std::vector<double> morphRates() {
	std::vector<double> rates;
	for (const auto& cfg : kPhiMorphPhaseBank) {
		rates.push_back(cfg.phiFreq);
	}
	for (const auto& cfg : kPhiMorphAmplitudeBank) {
		rates.push_back(cfg.phiFreq);
	}
	for (const auto& cfg : kPhiMorphCurvatureBank) {
		rates.push_back(cfg.phiFreq);
	}
	rates.push_back(kPhiMorphGainBank[0].phiFreq);
	rates.push_back(kPhiMorphEndpointBank[0].phiFreq);
	rates.push_back(kPhiMorphAmpOvershootBank[0].phiFreq);
	rates.push_back(kPhiMorphCurvBoostBank[0].phiFreq);
	rates.push_back(kPhiMorphPhaseDistortBank[0].phiFreq);
	rates.push_back(kPhiMorphSineBlendBank[0].phiFreq);
	rates.push_back(kPhiMorphOddSymBank[0].phiFreq);
	rates.push_back(kPhiMorphWindowBank[0].phiFreq);
	rates.push_back(kPhiMorphSlopeMatchBank[0].phiFreq);
	rates.push_back(kPhiMorphPhaseJitterBank[0].phiFreq);
	rates.push_back(kPhiMorphAmpNoiseBank[0].phiFreq);
	rates.push_back(kPhiMorphAsymGainBank[0].phiFreq);
	return rates;
}

std::vector<double> stairRates() {
	return {
		kPhiStairCount.phiFreq,      // 0 count
		kPhiStairSlope.phiFreq,      // 1 slope
		kPhiStairTilt.phiFreq,       // 2 tilt
		kPhiStairAsym.phiFreq,       // 3 asym
		kPhiStairLandCycles.phiFreq, // 4 land cycles
		kPhiStairHeightLand.phiFreq, // 5 height landscape
		kPhiStairAsymLand.phiFreq,   // 6 asymmetry landscape
		phi::kPhi300,                // 7 glyph  — inline in slice_build.cpp's stairSpatial
		phi::kPhi200,                // 8 shard  — inline likewise
	};
}

//----------------------------------------------------------------------------------------------//
// 1. The home direction's rational rank                                                          //
//----------------------------------------------------------------------------------------------//

/// The φ exponent a rate represents, in HUNDREDTHS, or a miss.
///
/// Every named constant in phi_triangle.hpp is φ raised to a multiple of 0.01, so the exponent is
/// recovered as log_φ(rate) and rounded to hundredths. The residual is reported: a rate that is not
/// a φ-power at all would show up as a fat one rather than being quietly rounded into the story.
struct Exponent {
	long hundredths = 0;
	double residual = 0.0;
	bool integerPower = false;
};

Exponent exponentOf(double rate) {
	const double kPhiExact = (1.0 + std::sqrt(5.0)) / 2.0;
	const double e = std::log(rate) / std::log(kPhiExact);
	Exponent out;
	out.hundredths = std::lround(e * 100.0);
	out.residual = std::abs(e - static_cast<double>(out.hundredths) / 100.0);
	out.integerPower = out.hundredths % 100 == 0;
	return out;
}

/// EXACT ℚ-RANK of the rate set, computed in ℤ[ψ]/(ψ^200 − ψ^100 − 1) where ψ = φ^(1/100).
///
/// Every rate is φ^(k/100) = ψ^k, and ψ^200 = ψ^100 + 1 follows from φ² = φ + 1. So each rate is a
/// polynomial in ψ of degree < 200 with integer coefficients, and the ℚ-rank of the rate set is the
/// rank of that integer matrix — computed by exact elimination over the rationals, no floats, so the
/// answer cannot be an artefact of precision.
///
/// UPPER BOUND, STATED AS SUCH. This is exact in ℚ[x]/(x^200 − x^100 − 1). If that polynomial is
/// reducible then ψ's true minimal polynomial is a factor of it and additional relations could exist,
/// which could only LOWER the true rank. It is quoted as a bound rather than a certainty because
/// proving irreducibility is not something this tool does.
std::vector<std::vector<long long>> psiVectors(const std::vector<double>& rates, int shift) {
	std::vector<std::vector<long long>> rows;
	for (double rate : rates) {
		// Shifted by ψ^shift so negative exponents become non-negative. Scaling every element of a
		// set by one nonzero constant leaves linear independence exactly as it was.
		long k = exponentOf(rate).hundredths + shift;
		std::vector<long long> poly(600, 0);
		poly[static_cast<size_t>(k)] = 1;
		// x^d = x^(d-100) + x^(d-200), applied from the top down until nothing of degree ≥ 200 is left.
		for (size_t d = poly.size(); d-- > 200;) {
			if (poly[d] != 0) {
				poly[d - 100] += poly[d];
				poly[d - 200] += poly[d];
				poly[d] = 0;
			}
		}
		poly.resize(200);
		rows.push_back(poly);
	}
	return rows;
}

int exactRank(std::vector<std::vector<long long>> rows) {
	if (rows.empty()) {
		return 0;
	}
	const size_t cols = rows[0].size();
	size_t pivotRow = 0;
	for (size_t c = 0; c < cols && pivotRow < rows.size(); ++c) {
		size_t sel = rows.size();
		for (size_t r = pivotRow; r < rows.size(); ++r) {
			if (rows[r][c] != 0) {
				sel = r;
				break;
			}
		}
		if (sel == rows.size()) {
			continue;
		}
		std::swap(rows[pivotRow], rows[sel]);
		for (size_t r = 0; r < rows.size(); ++r) {
			if (r == pivotRow || rows[r][c] == 0) {
				continue;
			}
			// Fraction-free: scale both rows so the pivot column cancels in integers. The entries
			// here start as 0/1/2/3 and the matrices are tiny, so growth is not a concern.
			const long long a = rows[pivotRow][c];
			const long long b = rows[r][c];
			const long long g = std::gcd(a < 0 ? -a : a, b < 0 ? -b : b);
			const long long fa = b / g;
			const long long fb = a / g;
			for (size_t j = 0; j < cols; ++j) {
				rows[r][j] = rows[r][j] * fb - rows[pivotRow][j] * fa;
			}
		}
		++pivotRow;
	}
	return static_cast<int>(pivotRow);
}

//----------------------------------------------------------------------------------------------//
// 2. The basis span                                                                              //
//----------------------------------------------------------------------------------------------//

/// Cyclic Jacobi on a symmetric matrix. Enough for a 70x70 Gram matrix and short enough to read,
/// which beats pulling in a linear-algebra dependency for one measurement.
std::vector<double> symmetricEigenvalues(std::vector<std::vector<double>> a) {
	const size_t n = a.size();
	for (int sweep = 0; sweep < 100; ++sweep) {
		double off = 0.0;
		for (size_t p = 0; p < n; ++p) {
			for (size_t q = p + 1; q < n; ++q) {
				off += a[p][q] * a[p][q];
			}
		}
		if (off < 1e-30) {
			break;
		}
		for (size_t p = 0; p < n; ++p) {
			for (size_t q = p + 1; q < n; ++q) {
				if (std::abs(a[p][q]) < 1e-300) {
					continue;
				}
				const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
				const double t = (theta >= 0.0 ? 1.0 : -1.0)
				                 / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
				const double c = 1.0 / std::sqrt(t * t + 1.0);
				const double s = t * c;
				for (size_t k = 0; k < n; ++k) {
					const double akp = a[k][p];
					const double akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (size_t k = 0; k < n; ++k) {
					const double apk = a[p][k];
					const double aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
			}
		}
	}
	std::vector<double> eigen;
	eigen.reserve(n);
	for (size_t i = 0; i < n; ++i) {
		eigen.push_back(a[i][i]);
	}
	std::sort(eigen.begin(), eigen.end(), std::greater<double>());
	return eigen;
}

/// Which coordinates a turn can touch AT ALL — measured by turning and looking, rather than read off a
/// pairing table.
///
/// THE TABLE IT USED TO READ IS GONE, and the change is the point of this section rather than a
/// breakage in it. The old decode turned pairs of coordinates named by `rotationPlaneA/B`, so which
/// coordinates were reachable was a question about strides — and the answer was bad: the A stride
/// shared a factor with 35, so its orbit covered 7 of morph's 35 phase coordinates however many angles
/// were stored. M writes every coordinate of both rows from its own φ triangle, so the honest way to
/// ask is to compare planes at two rotates and count what moved. A coordinate that never moves is a
/// structural zero exactly as before.
template <size_t N>
std::set<size_t> touchedCoordinates() {
	std::set<size_t> touched;
	const feathers::SliceBasis<N> rest = feathers::sliceBasisFor<N>(0.0, 0.37);
	for (int step = 1; step <= 64; ++step) {
		const feathers::SliceBasis<N> turned =
		    feathers::sliceBasisFor<N>(static_cast<double>(step) * 0.17, 0.37);
		for (size_t i = 0; i < 2 * N; ++i) {
			if (turned.x[i] != rest.x[i] || turned.y[i] != rest.y[i]) {
				touched.insert(i);
			}
		}
	}
	return touched;
}

template <size_t N>
void report(const char* engine, const std::vector<double>& rates) {
	std::printf("\n================ %s : N = %zu triangles, full space 2N = %zu ================\n",
	            engine, N, 2 * N);

	//------------------------------------------------------------------------------------------//
	std::printf("\n[1] THE HOME DIRECTION — rational rank of the rate vector\n");

	std::set<long> distinct;
	int integerPowers = 0;
	double worstResidual = 0.0;
	for (double r : rates) {
		const Exponent e = exponentOf(r);
		distinct.insert(e.hundredths);
		integerPowers += e.integerPower ? 1 : 0;
		worstResidual = std::max(worstResidual, e.residual);
	}
	std::printf("    rates: %zu total, %zu DISTINCT\n", rates.size(), distinct.size());
	std::printf("    exponents (in units of 1/100 of a power of phi):");
	int shown = 0;
	for (long h : distinct) {
		if (shown++ % 12 == 0) {
			std::printf("\n     ");
		}
		std::printf(" %+5ld", h);
	}
	std::printf("\n");
	// THE CONSTANTS ARE NOT EXACT PHI POWERS, and the residual says by how much. kPhi225 is really
	// phi^2.2596 and kPhi375 is phi^3.7396 — the names round, the values do not. Reported because a
	// rank computed from the NAMES is a rank of what was intended, and this is the gap between that
	// and what ships.
	std::printf("    largest gap between a constant and the phi power it is named for: %.2e\n",
	            worstResidual);
	std::printf("    (so the exponents above are the INTENDED ladder positions, rounded from the\n");
	std::printf("     shipped floats — kPhi225 is really phi^2.2596, kPhi375 really phi^3.7396)\n");
	std::printf("    INTEGER powers: %d of %zu.  NON-INTEGER: %zu\n", integerPowers, rates.size(),
	            rates.size() - static_cast<size_t>(integerPowers));

	const int rank = exactRank(psiVectors(rates, 100));
	std::printf("    exact Q-rank in Q[psi]/(psi^200 - psi^100 - 1), psi = phi^(1/100):  %d\n", rank);
	if (rank > 2) {
		std::printf("    => the rank-2 reading does NOT hold. It needs every rate to be an INTEGER\n");
		std::printf("       power (those live in Q(phi), which is 2-dimensional over Q). Fractional\n");
		std::printf("       powers are not in Q(phi), and the banks are mostly fractional.\n");
	}
	std::printf("    => the home line's closure is a %d-torus, not a 2-torus: home ALONE already\n", rank);
	std::printf("       varies %d independent directions of the %zu phase coordinates.\n", rank, N);
	std::printf("\n    THREE FRAMES, and only one of them is the shipped engine:\n");
	std::printf("      exact phi powers at the INTENDED exponents  -> rank %d   (computed above)\n", rank);
	std::printf("      the constants AS WRITTEN                    -> not exact phi powers at all;\n");
	std::printf("                                                     off by up to %.1e in exponent\n",
	            worstResidual);
	std::printf("      the shipped floats, taken literally         -> every float is a RATIONAL, so\n");
	std::printf("                                                     the strict Q-rank is 1 and the\n");
	std::printf("                                                     torus argument does not apply\n");
	std::printf("    The third is a fact about floating point rather than about the instrument: over\n");
	std::printf("    any range home can actually reach, doubles carry these rates as incommensurate.\n");
	std::printf("    The first is the one that describes the design. Both are far above 2.\n");

	//------------------------------------------------------------------------------------------//
	std::printf("\n[2a] THE BASIS SPAN — structurally, from the rotation pairing\n");

	const std::set<size_t> touched = touchedCoordinates<N>();
	std::set<size_t> phases;
	std::set<size_t> ratesTouched;
	for (size_t i : touched) {
		if (i < N) {
			phases.insert(i);
		}
		else {
			ratesTouched.insert(i - N);
		}
	}
	std::printf("    M(rotate, home): every coordinate of both rows is its own phi triangle, read at\n");
	std::printf("    rate phi^-(1+i%%5) in the turn and phi^-(1+i%%7) in home. Counted by turning.\n");
	std::printf("    phase coordinates any rotation touches (%zu of %zu):", phases.size(), N);
	for (size_t i : phases) {
		std::printf(" %zu", i);
	}
	std::printf("\n    rate  coordinates any rotation touches (%zu of %zu):", ratesTouched.size(), N);
	for (size_t i : ratesTouched) {
		std::printf(" %zu", i);
	}
	std::printf("\n    STRUCTURAL ZEROS: %zu of %zu coordinates are named by no rotation plane and\n",
	            2 * N - touched.size(), 2 * N);
	std::printf("    keep their degenerate value forever — phase rows stay 1, rate rows stay 0.\n");
	std::printf("    A rate row stuck at 0 means that triangle's rate is EXACTLY 1 always: no\n");
	std::printf("    generator, no travel and no residue can change how fast it varies.\n");

	//------------------------------------------------------------------------------------------//
	std::printf("\n[2b] THE BASIS SPAN — numerically, over sampled generators\n");

	std::mt19937 rng(20260813);
	std::uniform_real_distribution<double> turn(-3.14159, 3.14159);
	// M's two arguments, so the sweep covers what the plane is actually a function of. Home ranges over
	// several countries rather than a neighbourhood: the plane drifts at phi^-1 and below, so a span
	// this wide is what makes the sample a measurement of the whole space rather than of one region.
	std::uniform_real_distribution<double> where(-32.0, 32.0);

	constexpr int kSamples = 4000;
	std::vector<std::vector<double>> gram(2 * N, std::vector<double>(2 * N, 0.0));
	std::vector<std::array<double, 2 * N>> rows;
	rows.reserve(static_cast<size_t>(kSamples) * 4);

	feathers::SliceBasis<N> reference = feathers::degenerateSliceBasis<N>();
	for (int s = 0; s < kSamples; ++s) {
		const feathers::SliceBasis<N> basis = feathers::sliceBasisFor<N>(turn(rng), where(rng));

		// The rows themselves, AND their differences from the degenerate basis. The differences are
		// what the residue fold accumulates — (oldBasis − newBasis)·drift — so including them is what
		// makes this a measurement of everything reachable rather than of travel alone.
		std::array<double, 2 * N> rx {};
		std::array<double, 2 * N> ry {};
		std::array<double, 2 * N> dx {};
		std::array<double, 2 * N> dy {};
		for (size_t i = 0; i < 2 * N; ++i) {
			rx[i] = basis.x[i];
			ry[i] = basis.y[i];
			dx[i] = static_cast<double>(basis.x[i]) - static_cast<double>(reference.x[i]);
			dy[i] = static_cast<double>(basis.y[i]) - static_cast<double>(reference.y[i]);
		}
		rows.push_back(rx);
		rows.push_back(ry);
		rows.push_back(dx);
		rows.push_back(dy);
	}
	for (const auto& r : rows) {
		for (size_t i = 0; i < 2 * N; ++i) {
			for (size_t j = 0; j < 2 * N; ++j) {
				gram[i][j] += r[i] * r[j];
			}
		}
	}

	const std::vector<double> eigen = symmetricEigenvalues(gram);
	const double top = eigen.front();
	// THE THRESHOLD IS 1e-6 AND THE FIRST ATTEMPT AT IT WAS WRONG, which is worth leaving written
	// down. At 1e-9 this counted 23 for morph — eleven more than the structure permits — because the
	// basis is built in FLOAT, whose epsilon is about 1.2e-7, so rows that are identical in exact
	// arithmetic differ in their last bits and the Gram turns that into singular values around 1e-9.
	// Those are rounding, not reach. The measured spectrum makes the right cut obvious rather than a
	// matter of taste: there are twelve values above 1.8e-1 and then a cliff of seven orders of
	// magnitude to 8.7e-9, so any threshold between them gives the same answer.
	//
	// This is exactly why the structural count is computed alongside: it is exact, it needs no
	// threshold, and it is what caught the numerical answer being wrong.
	const double threshold = 1e-6;
	int numericalRank = 0;
	for (double e : eigen) {
		if (e > 0.0 && std::sqrt(e / top) > threshold) {
			++numericalRank;
		}
	}
	std::printf("    %d generators sampled, %zu rows stacked (x, y, and both differences)\n",
	            kSamples, rows.size());
	std::printf("    singular values, largest first (sigma / sigma_max):\n     ");
	for (size_t i = 0; i < eigen.size() && i < 20; ++i) {
		const double sv = eigen[i] > 0.0 ? std::sqrt(eigen[i] / top) : 0.0;
		std::printf(" %.1e", sv);
		if ((i + 1) % 10 == 0) {
			std::printf("\n     ");
		}
	}
	std::printf("\n    NUMERICAL RANK at sigma/sigma_max > %.0e:  %d\n", threshold, numericalRank);

	// The two measurements have to agree, and saying so out loud is the point of doing both.
	//
	// THE "+1" IS GONE, and it is the same deletion as the verdict below rather than a second one. It
	// counted the ONE FIXED DIRECTION every basis used to share: under the old decode the untouched
	// phase rows sat at 1 in every generator, so their common vector was in the span while their
	// individual coordinates were not free. M writes every phase coordinate from its own φ triangle —
	// nothing sits at 1 across generators any more, there are no untouched rows to share a direction,
	// and the span is exactly the coordinates that move. Leaving the term in made the tool print
	// DISAGREES on a healthy decode, which is worse than printing nothing: a tripwire that fires on
	// correct code is one nobody reads twice.
	const size_t predicted = touched.size();
	std::printf("    structural prediction: %zu touched coordinates = %zu   [%s]\n", touched.size(),
	            predicted,
	            predicted == static_cast<size_t>(numericalRank) ? "AGREES" : "DISAGREES — investigate");

	//------------------------------------------------------------------------------------------//
	std::printf("\n[3] VERDICT\n");
	const size_t reachable = static_cast<size_t>(numericalRank);
	std::printf("    reachable dimension d = %zu of 2N = %zu  (%.1f%%)\n", reachable, 2 * N,
	            100.0 * static_cast<double>(reachable) / static_cast<double>(2 * N));
	std::printf("    forever untouched: %zu coordinates — %zu phase, %zu rate\n",
	            2 * N - touched.size(), N - phases.size(), N - ratesTouched.size());
	// WHAT THE OLD DECODE PRINTED HERE IS DELETED RATHER THAN UPDATED, and the deletion is the
	// finding. This paragraph used to compute the ceiling from `kSliceAngles`: six angles name six
	// planes, so at most twelve coordinates move however the generator is set, and covering the space
	// would need either eighteen angles or "a per-triangle mixing scheme rather than one plane per
	// angle". M IS THAT SCHEME. Every coordinate of both rows is written by its own φ triangle read at
	// the turn and the home, so there is no plane-counting arithmetic left to do and the symbol the
	// sentence was built on does not exist. Printing it against the M decode would state a ceiling that
	// was removed by the thing being measured.
	//
	// What replaces it is the measurement immediately above, said plainly: the count comes from
	// turning the shipped basis and looking at which coordinates moved (touchedCoordinates), so it
	// reports what the decode does rather than what a pairing table promised.
	if (touched.size() == 2 * N) {
		std::printf("    NO STRUCTURAL CEILING: every one of the %zu coordinates moves under some turn,\n",
		            2 * N);
		std::printf("    measured by turning the shipped basis rather than read off a pairing table.\n");
	}
	else {
		std::printf("    %zu of %zu coordinates never move under any turn sampled here. That is a\n",
		            2 * N - touched.size(), 2 * N);
		std::printf("    structural zero in the decode, not a sampling limit — investigate before\n");
		std::printf("    trusting any span number above it.\n");
	}
}

} // namespace

int main() {
	std::printf("REACHABLE DIMENSION OF THE CHARACTER SPACE\n");
	std::printf("computed from the shipped banks and the shipped basis code\n");

	const std::vector<double> morph = morphRates();
	const std::vector<double> stair = stairRates();
	if (morph.size() != kMorphTriangles || stair.size() != kStairTriangles) {
		std::printf("FAIL  rate tables are the wrong size: morph %zu (want %zu), stair %zu (want %zu)\n",
		            morph.size(), kMorphTriangles, stair.size(), kStairTriangles);
		return 1;
	}

	report<kMorphTriangles>("PHI MORPH", morph);
	report<kStairTriangles>("PHI STAIR", stair);

	std::printf("\n");
	return 0;
}
