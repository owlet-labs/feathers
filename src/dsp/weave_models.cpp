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

#include "dsp/weave_models.h"

#include "dsp/portable_trig.h"

#include <algorithm>
#include <cmath>

namespace feathers::weave {

namespace {

constexpr int kRingNodes = kWeavePathPoints;
constexpr int kChainNodes = 32;
constexpr int kGrid = 8;
constexpr float kPi = 3.14159265f;
constexpr float kTwoPi = 6.283185307f;
// The Membrane's scan circle, in the grid's own coordinates (the rim at +-1).
constexpr float kCircleRadius = 0.6f;
// Stiffening's Newton rest pose: at most this many steps, stopping when the worst residual is below the
// tolerance, which is ABSOLUTE. MEASURED 2026-09-26 over 4096 phases plus 15,904 random crossfaded pairs:
// it stops after 2 steps (4060 of the 4096 phases) or 3 (36); the residuals here are O(0.1) terms, so
// their floor after 11 steps is <= 1.3e-16 and 1e-13 sits three decades ABOVE double's rounding floor,
// not at it. A relative tolerance (against max |k home|, 0.016..0.093) was tried and cannot keep
// today's stopping points: the residuals it stops at reach 6.0e-12 of that scale while ones it steps past
// go down to 2.2e-12, so any relative threshold moves where some pose stops.
constexpr int kNewtonMaxIterations = 12;
constexpr double kNewtonTolerance = 1e-13;

// Wrap to [-0.5, 0.5): the firmware's truncation form, which is what its bow and pluck windows use.
float wrapHalf(float d) {
	return d - (static_cast<float>(static_cast<int32_t>(d + 1.5f)) - 1.0f);
}

// The bow, pluck and line windows are state the ring integrates, so their trig is portable (dsp/portable_trig.h):
// a C library's cosf rounding one window value the other way made macOS render a different Resonate.
float raisedCosineAt(float w) {
	return (w < 1.0f) ? (0.5f + 0.5f * dsp::portableCos(kPi * w)) : 0.0f;
}

float wrap01(float p) {
	return p - std::floor(p);
}

// The out-and-back path: position 0..1 round the cycle to a point 0..1 along the chain.
float outAndBack(float p) {
	const float t = wrap01(p);
	return (t < 0.5f) ? 2.0f * t : 2.0f - 2.0f * t;
}

// A Membrane node's point in grid coordinates, the rim at +-1.
void gridPoint(int node, float& gx, float& gy) {
	gx = (static_cast<float>(node % kGrid) + 0.5f) * (2.0f / kGrid) - 1.0f;
	gy = (static_cast<float>(node / kGrid) + 0.5f) * (2.0f / kGrid) - 1.0f;
}

// Everything about a Membrane node that depends only on where it sits, computed once at static
// initialisation (no allocation) so a law rebuild on the audio thread pays no sqrt, atan2 or sin.
struct MembraneGeometry {
	float gx[kGrid * kGrid];
	float gy[kGrid * kGrid];
	float lawU[kGrid * kGrid];    // fractional law index by radius: 0 at the centre, 31 at the corner node
	float homeU[kGrid * kGrid];   // fractional home index by angle, 0..32
	float profile[kGrid * kGrid]; // sin(pi min(r, 1)): 0 at the centre and at the rim
};

MembraneGeometry makeMembraneGeometry() {
	MembraneGeometry g {};
	const float cornerRadius = std::sqrt(2.0f) * (1.0f - 1.0f / kGrid); // the outermost node centre
	for (int i = 0; i < kGrid * kGrid; ++i) {
		gridPoint(i, g.gx[i], g.gy[i]);
		const float r = std::sqrt(g.gx[i] * g.gx[i] + g.gy[i] * g.gy[i]);
		g.lawU[i] = r / cornerRadius * static_cast<float>(kRingNodes - 1);
		float theta = std::atan2(g.gy[i], g.gx[i]);
		if (theta < 0.0f) {
			theta += kTwoPi;
		}
		g.homeU[i] = theta / kTwoPi * kRingNodes;
		g.profile[i] = dsp::portableSin(kPi * std::min(r, 1.0f));
	}
	return g;
}

const MembraneGeometry kMembrane = makeMembraneGeometry();

// Linear read of a 32-entry law array at fractional index u, non-cyclic (clamped at the ends).
float lawAt(const float* law, float u) {
	u = std::clamp(u, 0.0f, static_cast<float>(kRingNodes - 1));
	const int i = std::min(static_cast<int>(u), kRingNodes - 2);
	const float f = u - static_cast<float>(i);
	return law[i] + (law[i + 1] - law[i]) * f;
}

// Linear read of a 32-entry law array at fractional index u, cyclic.
float lawAtCyclic(const float* law, float u) {
	u = u - std::floor(u / kRingNodes) * kRingNodes;
	int i = static_cast<int>(u);
	float f = u - static_cast<float>(i);
	if (i >= kRingNodes) {
		i = kRingNodes - 1;
		f = 1.0f;
	}
	return law[i] + (law[(i + 1) % kRingNodes] - law[i]) * f;
}

using Matrix = double[kWeaveMaxNodes][kWeaveMaxNodes + 1];

// Gaussian elimination without pivoting on an n x n system with its right-hand side in column n. Every
// system here has one of two properties, and EITHER SUFFICES for elimination without pivoting (each is
// inherited by every Schur complement, so no pivot vanishes and the entries cannot grow past twice their
// start):
// - STRICT ROW DIAGONAL DOMINANCE, by k_i >= 0.016 (the laws' clamp): the Loop, the pinned chain and the
//   Grid are K = diag(c) L with L a Laplacian, so row i has c_i (2 or 4) on the diagonal plus k_i and
//   -c_i per neighbour. NOT symmetric where c varies. Stiffening's Newton Jacobian too: the cubic adds
//   3 beta (dp^2 + dm^2) to the diagonal and exactly 3 beta dp^2 and 3 beta dm^2 to the two neighbours.
// - SYMMETRIC POSITIVE DEFINITE: the Bar, sum_j b_j d_j d_j^T plus diag(k), k > 0. It is NOT diagonally
//   dominant (its interior row is b (1 -4 6 -4 1)).
void solve(Matrix& m, int n, double* sol) {
	for (int col = 0; col < n; ++col) {
		for (int row = col + 1; row < n; ++row) {
			const double f = m[row][col] / m[col][col];
			if (f != 0.0) {
				for (int j = col; j <= n; ++j) {
					m[row][j] -= f * m[col][j];
				}
			}
		}
	}
	for (int row = n - 1; row >= 0; --row) {
		double acc = m[row][n];
		for (int j = row + 1; j < n; ++j) {
			acc -= m[row][j] * sol[j];
		}
		sol[row] = acc / m[row][row];
	}
}

// The linear elastic stiffness K (force = -K x) added into m, plus diag(k) and the rhs k * home.
void fillLinearSystem(WeaveModel model, const NodeLaws& laws, Matrix& m) {
	const int n = nodeCount(model);
	for (int i = 0; i < n; ++i) {
		for (int j = 0; j <= n; ++j) {
			m[i][j] = 0.0;
		}
	}
	switch (traits(model).topology) {
	case Topology::Loop:
		// Row i: -c[i] x[i-1] + (2c[i] + k[i]) x[i] - c[i] x[i+1] = k[i] home[i], cyclic.
		for (int i = 0; i < n; ++i) {
			const double c = laws.gain[i];
			m[i][(i + n - 1) & (n - 1)] -= c;
			m[i][(i + 1) & (n - 1)] -= c;
			m[i][i] += 2.0 * c + static_cast<double>(laws.k[i]);
			m[i][n] = static_cast<double>(laws.k[i]) * static_cast<double>(laws.home[i]);
		}
		return;
	case Topology::PinnedChain:
		for (int i = 0; i < n; ++i) {
			const double c = laws.gain[i];
			if (i > 0) {
				m[i][i - 1] -= c;
			}
			if (i < n - 1) {
				m[i][i + 1] -= c;
			}
			m[i][i] += 2.0 * c + static_cast<double>(laws.k[i]);
			m[i][n] = static_cast<double>(laws.k[i]) * static_cast<double>(laws.home[i]);
		}
		return;
	case Topology::FreeBar:
		// Bending energy 1/2 sum_j b_j kappa_j^2 over the interior curvatures kappa_j = x[j-1] - 2x[j] +
		// x[j+1], j = 1..n-2: K = sum_j b_j d_j d_j^T. No curvature term at the ends is what makes them free.
		for (int j = 1; j < n - 1; ++j) {
			const double b = laws.gain[j];
			const int idx[3] = { j - 1, j, j + 1 };
			const double w[3] = { 1.0, -2.0, 1.0 };
			for (int r = 0; r < 3; ++r) {
				for (int s = 0; s < 3; ++s) {
					m[idx[r]][idx[s]] += b * w[r] * w[s];
				}
			}
		}
		for (int i = 0; i < n; ++i) {
			m[i][i] += static_cast<double>(laws.k[i]);
			m[i][n] = static_cast<double>(laws.k[i]) * static_cast<double>(laws.home[i]);
		}
		return;
	case Topology::Grid:
		for (int i = 0; i < n; ++i) {
			const double c = laws.gain[i];
			const int col = i % kGrid;
			const int row = i / kGrid;
			if (col > 0) {
				m[i][i - 1] -= c;
			}
			if (col < kGrid - 1) {
				m[i][i + 1] -= c;
			}
			if (row > 0) {
				m[i][i - kGrid] -= c;
			}
			if (row < kGrid - 1) {
				m[i][i + kGrid] -= c;
			}
			m[i][i] += 4.0 * c + static_cast<double>(laws.k[i]);
			m[i][n] = static_cast<double>(laws.k[i]) * static_cast<double>(laws.home[i]);
		}
		return;
	}
}

} // namespace

void mapLaws(WeaveModel m, const float* k, const float* c, const float* d, const float* home, const float* bowBal,
             NodeLaws& out) {
	const ModelTraits& t = traits(m);
	if (t.topology != Topology::Grid) {
		for (int i = 0; i < t.nodes; ++i) {
			out.k[i] = k[i] * t.homeScale;
			out.gain[i] = c[i] * t.gainScale;
			out.d[i] = d[i];
			out.home[i] = home[i];
			out.bowBal[i] = bowBal[i] * t.bowScale;
		}
		return;
	}
	const MembraneGeometry& g = kMembrane;
	for (int i = 0; i < t.nodes; ++i) {
		const float u = g.lawU[i];
		out.k[i] = lawAt(k, u) * t.homeScale;
		out.gain[i] = lawAt(c, u) * t.gainScale;
		out.d[i] = lawAt(d, u);
		out.bowBal[i] = lawAt(bowBal, u) * t.bowScale;
		out.home[i] = lawAtCyclic(home, g.homeU[i]) * g.profile[i];
	}
}

float window(WeaveModel m, int node, float pos, float invHalfWidth) {
	switch (traits(m).topology) {
	case Topology::Loop: {
		const float nf = static_cast<float>(node) / static_cast<float>(kRingNodes);
		return raisedCosineAt(std::abs(wrapHalf(nf - pos)) * invHalfWidth);
	}
	case Topology::PinnedChain:
	case Topology::FreeBar: {
		const float along = static_cast<float>(node) / static_cast<float>(kChainNodes - 1);
		// One path unit covers the chain twice, so a half-width of h path units is 2h along the chain.
		return raisedCosineAt(std::abs(along - outAndBack(pos)) * invHalfWidth * 0.5f);
	}
	case Topology::Grid: {
		const float a = kTwoPi * wrap01(pos);
		const float dx = kMembrane.gx[node] - kCircleRadius * dsp::portableCos(a);
		const float dy = kMembrane.gy[node] - kCircleRadius * dsp::portableSin(a);
		// One path unit is the scan circle's circumference.
		return raisedCosineAt(std::sqrt(dx * dx + dy * dy) * invHalfWidth / (kTwoPi * kCircleRadius));
	}
	}
	return 0.0f;
}

void windows(WeaveModel m, float pos, float invHalfWidth, float* out) {
	const int n = nodeCount(m);
	if (traits(m).topology != Topology::Grid) {
		for (int i = 0; i < n; ++i) {
			out[i] = window(m, i, pos, invHalfWidth);
		}
		return;
	}
	// The same arithmetic as window(), with the centre's cos and sin taken once.
	const float a = kTwoPi * wrap01(pos);
	const float cx = kCircleRadius * dsp::portableCos(a);
	const float cy = kCircleRadius * dsp::portableSin(a);
	const float scale = invHalfWidth / (kTwoPi * kCircleRadius);
	for (int i = 0; i < n; ++i) {
		const float dx = kMembrane.gx[i] - cx;
		const float dy = kMembrane.gy[i] - cy;
		out[i] = raisedCosineAt(std::sqrt(dx * dx + dy * dy) * scale);
	}
}

void elasticForce(WeaveModel m, const float* x, const float* gain, float* force) {
	switch (traits(m).topology) {
	case Topology::Loop:
		if (!traits(m).nonlinear) {
			for (int i = 0; i < kRingNodes; ++i) {
				const float xm1 = x[(i + kRingNodes - 1) & (kRingNodes - 1)];
				const float xp1 = x[(i + 1) & (kRingNodes - 1)];
				force[i] = gain[i] * (xm1 + xp1 - 2.0f * x[i]);
			}
			return;
		}
		for (int i = 0; i < kRingNodes; ++i) {
			const float xm1 = x[(i + kRingNodes - 1) & (kRingNodes - 1)];
			const float xp1 = x[(i + 1) & (kRingNodes - 1)];
			const float dp = xp1 - x[i];
			const float dm = x[i] - xm1;
			force[i] = gain[i] * (xm1 + xp1 - 2.0f * x[i]) + kBetaOverCoupling * gain[i] * (dp * dp * dp - dm * dm * dm);
		}
		return;
	case Topology::PinnedChain:
		for (int i = 0; i < kChainNodes; ++i) {
			const float xm1 = (i > 0) ? x[i - 1] : 0.0f;
			const float xp1 = (i < kChainNodes - 1) ? x[i + 1] : 0.0f;
			force[i] = gain[i] * (xm1 + xp1 - 2.0f * x[i]);
		}
		return;
	case Topology::FreeBar: {
		// Bending moments at the interior nodes, zero at the free ends; the force is minus their second
		// difference.
		float moment[kChainNodes];
		moment[0] = 0.0f;
		moment[kChainNodes - 1] = 0.0f;
		for (int j = 1; j < kChainNodes - 1; ++j) {
			moment[j] = gain[j] * (x[j - 1] - 2.0f * x[j] + x[j + 1]);
		}
		for (int i = 0; i < kChainNodes; ++i) {
			const float mm1 = (i > 0) ? moment[i - 1] : 0.0f;
			const float mp1 = (i < kChainNodes - 1) ? moment[i + 1] : 0.0f;
			force[i] = -(mm1 - 2.0f * moment[i] + mp1);
		}
		return;
	}
	case Topology::Grid:
		for (int i = 0; i < kGrid * kGrid; ++i) {
			const int col = i % kGrid;
			const int row = i / kGrid;
			const float l = (col > 0) ? x[i - 1] : 0.0f;
			const float r = (col < kGrid - 1) ? x[i + 1] : 0.0f;
			const float u = (row > 0) ? x[i - kGrid] : 0.0f;
			const float dn = (row < kGrid - 1) ? x[i + kGrid] : 0.0f;
			force[i] = gain[i] * (l + r + u + dn - 4.0f * x[i]);
		}
		return;
	}
}

void restPose(WeaveModel m, const NodeLaws& laws, RestPoseScratch& scratch, float* out) {
	const int n = nodeCount(m);
	Matrix& mat = scratch.m;
	fillLinearSystem(m, laws, mat);
	double sol[kWeaveMaxNodes];
	solve(mat, n, sol);
	if (traits(m).nonlinear) {
		// Newton on r(x) = k(x - home) - linear(x) - cubic(x), from the linear solution. The linear system
		// is spent, so each Jacobian is built in the same storage (fillLinearSystem clears it first).
		double linear[kWeaveMaxNodes];
		std::copy(sol, sol + n, linear);
		for (int iter = 0; iter < kNewtonMaxIterations; ++iter) {
			Matrix& jac = scratch.m;
			fillLinearSystem(m, laws, jac); // diag(k) + K_linear; rhs overwritten below
			double worst = 0.0;
			for (int i = 0; i < n; ++i) {
				const double xm1 = sol[(i + n - 1) & (n - 1)];
				const double xp1 = sol[(i + 1) & (n - 1)];
				const double c = laws.gain[i];
				const double beta = static_cast<double>(kBetaOverCoupling) * c;
				const double dp = xp1 - sol[i];
				const double dm = sol[i] - xm1;
				const double residual = static_cast<double>(laws.k[i]) * (sol[i] - static_cast<double>(laws.home[i]))
				                        - c * (xm1 + xp1 - 2.0 * sol[i]) - beta * (dp * dp * dp - dm * dm * dm);
				// d(-cubic_i)/dx: x[i+1] -> -3 beta dp^2, x[i] -> +3 beta (dp^2 + dm^2), x[i-1] -> -3 beta dm^2
				jac[i][(i + 1) & (n - 1)] -= 3.0 * beta * dp * dp;
				jac[i][i] += 3.0 * beta * (dp * dp + dm * dm);
				jac[i][(i + n - 1) & (n - 1)] -= 3.0 * beta * dm * dm;
				jac[i][n] = -residual;
				worst = std::max(worst, std::abs(residual));
			}
			if (worst < kNewtonTolerance) {
				break;
			}
			double step[kWeaveMaxNodes];
			solve(jac, n, step);
			for (int i = 0; i < n; ++i) {
				sol[i] += step[i];
			}
		}
		// A Newton step that overflowed leaves inf or NaN, and a NaN rest pose would seat a NaN string
		// until the next switch or reset. The linear pose is always finite for finite laws (a dominant
		// system), so it is the fallback: wrong by the cubic's share, but at rest.
		bool finite = true;
		for (int i = 0; i < n; ++i) {
			finite = finite && std::isfinite(sol[i]) && std::isfinite(static_cast<float>(sol[i]));
		}
		if (!finite) {
			std::copy(linear, linear + n, sol);
		}
	}
	for (int i = 0; i < n; ++i) {
		out[i] = static_cast<float>(sol[i]);
	}
}

void scanPath(WeaveModel m, const float* xs, float* path) {
	switch (traits(m).topology) {
	case Topology::Loop:
		for (int j = 0; j < kWeavePathPoints; ++j) {
			path[j] = xs[j];
		}
		return;
	case Topology::PinnedChain:
	case Topology::FreeBar:
		for (int j = 0; j < kWeavePathPoints; ++j) {
			const float u = outAndBack(static_cast<float>(j) / kWeavePathPoints) * (kChainNodes - 1);
			const int i = std::min(static_cast<int>(u), kChainNodes - 2);
			const float f = u - static_cast<float>(i);
			path[j] = xs[i] + (xs[i + 1] - xs[i]) * f;
		}
		return;
	case Topology::Grid:
		for (int j = 0; j < kWeavePathPoints; ++j) {
			const float a = kTwoPi * static_cast<float>(j) / kWeavePathPoints;
			// Grid coordinates to node-index coordinates: node centres sit at (index + 0.5) * 2/8 - 1.
			const float cx = (kCircleRadius * dsp::portableCos(a) + 1.0f) * (kGrid / 2.0f) - 0.5f;
			const float cy = (kCircleRadius * dsp::portableSin(a) + 1.0f) * (kGrid / 2.0f) - 0.5f;
			const int ix = std::clamp(static_cast<int>(cx), 0, kGrid - 2);
			const int iy = std::clamp(static_cast<int>(cy), 0, kGrid - 2);
			const float fx = cx - static_cast<float>(ix);
			const float fy = cy - static_cast<float>(iy);
			const float a00 = xs[membraneNode(ix, iy)];
			const float a10 = xs[membraneNode(ix + 1, iy)];
			const float a01 = xs[membraneNode(ix, iy + 1)];
			const float a11 = xs[membraneNode(ix + 1, iy + 1)];
			const float top = a00 + (a10 - a00) * fx;
			const float bottom = a01 + (a11 - a01) * fx;
			path[j] = top + (bottom - top) * fy;
		}
		return;
	}
}

int bridgeNodes(WeaveModel m, float pos, int* nodes, float* dist) {
	int n = 0;
	switch (traits(m).topology) {
	case Topology::Loop:
		for (int i = 0; i < kRingNodes; ++i) {
			const float d = std::abs(wrapHalf(static_cast<float>(i) / static_cast<float>(kRingNodes) - pos))
			                * static_cast<float>(kRingNodes);
			if (d <= kBridgeSpan) {
				nodes[n] = i;
				dist[n] = d;
				++n;
			}
		}
		break;
	case Topology::PinnedChain:
	case Topology::FreeBar: {
		const float centre = outAndBack(pos) * static_cast<float>(kChainNodes - 1);
		for (int i = 0; i < kChainNodes; ++i) {
			const float d = std::abs(static_cast<float>(i) - centre);
			if (d <= kBridgeSpan) {
				nodes[n] = i;
				dist[n] = d;
				++n;
			}
		}
		break;
	}
	case Topology::Grid: {
		const float a = kTwoPi * wrap01(pos);
		const float cx = kCircleRadius * dsp::portableCos(a);
		const float cy = kCircleRadius * dsp::portableSin(a);
		constexpr float kSpacing = 2.0f / kGrid;
		for (int i = 0; i < kGrid * kGrid; ++i) {
			const float dx = kMembrane.gx[i] - cx;
			const float dy = kMembrane.gy[i] - cy;
			const float d = std::sqrt(dx * dx + dy * dy) / kSpacing;
			if (d <= kBridgeGridSpan && n < kBridgeMaxNodes) {
				nodes[n] = i;
				dist[n] = d;
				++n;
			}
		}
		break;
	}
	}
	return n;
}

} // namespace feathers::weave
