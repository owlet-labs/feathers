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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// FEATHERS_FORCE_SIMDE takes the arm64 road on x86-64 too, so scripts/clang-preflight.sh compiles what macOS does.
#if !defined(FEATHERS_FORCE_SIMDE)                                                                        \
    && (defined(__SSE2__) || defined(_M_AMD64) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2))
#include <emmintrin.h>
#else
#define SIMDE_ENABLE_NATIVE_ALIASES
#include "simde/x86/sse2.h"
#endif

namespace feathers::fx::resonate {

// A NOTE HEAD'S OWN STRING (v10, node strings per voice, capped): a finite-difference string at audio
// rate — Bilbao's explicit scheme for the ideal string with two-parameter loss (Numerical Sound Synthesis, 2009, ch. 6),
// fixed ends, a unit length split into N segments:
//
//     (1 + s0 k) u+ = 2u - (1 - s0 k) u- + L^2 dxx u + (2 s1 k / h^2)(dxx u - dxx u-)
//
// with k = 1/sr, h = 1/N, L the Courant number. THE CAP: N = min(floor(kCourant sr / 2f), kMaxNodes) — every node the
// note's bandwidth can use up to kMaxNodes (128), so from sr / 256 down (187.5 Hz at 48 kHz) a note has fewer nodes than it
// could use and L falls below 1: at 55 Hz L = 0.29 and the scheme's highest mode is sr/pi asin(L) = 4.5 kHz, its partials
// running flat the higher they go: MEASURED (test_fx_resonate) the 10th -3.9 cents, the 20th -15.9, the 40th -64.4, each
// within 0.1 cent of the scheme's own modes.
// THE PITCH IS EXACT: L = sin(pi f / sr) / sin(pi / 2N) puts the lowest mode, (sr/pi) asin(L sin(pi/2N)), on f.
//
// THE BRIDGE IS A POINT (v17, can we instead make the jawari a point on the string?): ONE node, kPointPos of the
// length from the bridge end, meets an obstacle below rest at gap(Jawari) x (1 - kProximity x Jawari x proximity) of the
// string's scale there (its swing x the node's mode shape, pi l / N) — proximity being the radius
// of the ring's orbit (0..1), so the main string's slow orbit brings this one's bridge closer. A travelling wave meets the
// point only as it passes, so a pluck does not slap the bridge: the contact grows as the string's motion finds it (the
// bloom) and comes and goes with where the waves are (the breathing). Until v16 a LINE ran under the whole string, which
// every node met at once on a pluck. The node past the point has its excess split to its two neighbours and its step
// reversed at Bounce, never landing back below it (contact()).
// BOUNDED, BY AN ENERGY GUARD: the scheme's own energy for the ideal string,
//     H = sum (u+ - u)^2 + L^2 sum (dx u+)(dx u),
// is positive definite for L < 1 (kCourant keeps it there) and the loss terms only lower it. The contact changes only the
// next level, by c, so H(u+ + t c) - H(u+) = t L + t^2 Q is a parabola in t with Q > 0; the contact is kept only as far as
// the largest t <= 1 with t L + t^2 Q <= 0. So a contact never adds energy, whatever Bounce, Jawari or the transfer do,
// and the string stays bounded by what its forcing put in.
//
// GLIDE: the Courant number follows the pitch every sample, so a glide within one node count is continuous. The node count
// changes only when it must (L would pass kCourant) or when the note has fallen far enough that a quarter more nodes fit;
// then both time levels are RESAMPLED onto the new grid by linear interpolation at the same positions along the string, so
// its shape — and so its sound — carries across.
//
// THE BRIDGE YIELDS (v18, setYield): the bridge-end node, u[0], is no longer held at 0 but IS the shared bridge's
// displacement, which ResonateDsp steps from every string's pull on it (stepBridge there) and hands each string before its
// step (setBridgeNext). Then H less that node's own kinetic term, summed over the strings, plus the bridge's own energy, is
// the energy of the whole instrument — non-increasing (the bridge is a mass on a damped spring: nothing is added). A string
// left alone is flushed only once its bridge end is at rest too, so a string the bridge is moving keeps being stepped.
struct NodeString {
	static constexpr int kMaxNodes = 128;
	/// The largest Courant number used: below 1 so the frequency-dependent loss fits (L^2 + 4 s1 k N^2 <= 1) and H stays
	/// positive definite.
	static constexpr double kCourant = 0.98;
	/// THE FREQUENCY-DEPENDENT LOSS, s1 (s): the upper partials decay faster, as a real string's do. By ear (unauditioned);
	/// held under the stability bound.
	static constexpr double kSigma1 = 2e-4;
	/// THE POINT (v17): kPointPos of the length from the bridge end; below rest by gap(J) = kPointGap + (kPointGapAway -
	/// kPointGap)(1 - J)^2 of the string's scale there — clear of it at Jawari 0, kPointGap of it at 1, evenly across the
	/// middle of the knob (where it is played: dial that into the sweet spot); and the radius of the ring's orbit (its swing from rest under the
	/// bow, either way, over its lately largest: ResonateDsp::onTick) brings it up to kProximity x Jawari closer again (the
	/// ring's excursion stands in for the eccentric radius vector that brings the string closer to the bridge).
	/// MEASURED 2026-10-05 in the scratch prototype against v16's line (14 keys A1..C5, Jawari 0.8, the median): the notes'
	/// contacts per 10 ms 112 at the pluck rising to 142 (the line: 6.7 falling to 1.0), the buzz share 0.026 rising to 0.051,
	/// in tune within 3 cents; it was preferred it by ear over a line at the end and over a whole-string line. kPointGap 0.2 by
	/// the mid-Jawari sweep (14 keys, 2026-10-05): drone and note buzz rising over 0.25 / 0.5 / 0.75 (drones 0.034 0.052
	/// 0.052, notes 0.005 0.015 0.030), the level cost within 4.7 dB struck and fed, within 3.2 cents; 0.1 drained an
	/// input-fed drone 6-10 dB at Jawari 1, 0.3 buzzed less.
	static constexpr double kPointPos = 0.03;
	/// THE THREAD'S TRAVEL (setPointPos), BY MEASUREMENT (v21; four drone strings A1..E3 at Jawari 0.5, the mean): the buzz share
	/// and its centroid rise from 1% (0.058, 4.25 harmonics) through 3% (0.055, 4.53) to 7% (0.103, 5.16); at 10% the pitch
	/// moved 4.7 cents (the 5-cent bound) and from 15% the buzz is most of the sound (0.57), so the top is 8%. Contact persists
	/// throughout (2880 / 2089 / 1523 a second at 1 / 3 / 7%).
	static constexpr double kPointPosMin = 0.01;
	static constexpr double kPointPosMax = 0.08;
	static constexpr float kPointGap = 0.2f;
	static constexpr float kPointGapAway = 1.0f;
	/// JAWARI'S BOTTOM (v20): v17's gap was 1 (the string's own swing scale) at any Jawari above 0, so the point touched the moment
	/// Jawari left 0 — a step. Now kPointGapBottom more is added, falling as (1 - J / kPointGapBottomTo)^2 to nothing at
	/// kPointGapBottomTo: the point starts well clear (3x the swing scale at 0+) and comes in smoothly, and from
	/// kPointGapBottomTo up the gap is v17's exactly (the mid-range tuning: Jawari 0.25 / 0.5 / 0.75 unchanged).
	static constexpr float kPointGapBottom = 2.0f;
	static constexpr float kPointGapBottomTo = 0.25f;
	static float gapOf(float jawari) {
		const float away = 1.0f - jawari;
		const float bottom = std::max(0.0f, 1.0f - jawari / kPointGapBottomTo);
		return kPointGap + (kPointGapAway - kPointGap) * away * away + kPointGapBottom * bottom * bottom;
	}
	static constexpr float kProximity = 0.5f;
	/// THE PICKUP (setPickup): 0 hears the string at kPickLeft, 1 at kPickNear, near its bridge end, where its upper
	/// partials are strongest.
	static constexpr double kPickNear = 0.1;
	/// THE STIFFEST STRING (tune's B): partial 10 about 2% sharp, partial 20 about 8% (v11, the drone's String Stiffness).
	static constexpr double kMaxInharmonicity = 4e-4;
	/// THE HELD SWING'S RELEASE BEFORE A STRING IS TUNED (prepare); tune sets it from the T60 (v22; 2 s for every string until then).
	static constexpr double kHoldSeconds = 2.0;
	/// THE DRAG (setDrag): its rate at twice the level (per second, on the loss s0), and its envelope's release.
	static constexpr double kDragRate = 5000.0;
	static constexpr double kDragSeconds = 0.02;
	static constexpr float kFloor = 1e-6f;
	/// THE PASSES a contact's excess may travel this sample (a neighbour it pushes through is taken on the second; what is
	/// still past after that is taken next sample). MEASURED 2026-09-28, 8 voices at 128 nodes driven hard at Jawari 1: 250
	/// us a block at 1 or 2 passes, 710 at 3.
	static constexpr int kPasses = 2;
	/// WHERE IT IS DRIVEN and where it is heard, as fractions of its length: near one end, as a plucked string is, and
	/// two pickups for the two channels (the right one moved along by Width).
	static constexpr double kDrivePos = 0.13;
	static constexpr double kPickLeft = 0.3;
	static constexpr double kPickSpread = 0.25;

	void prepare(double sampleRate) {
		sr_ = sampleRate;
		release_ = holdRelease(t60_);
		dragRelease_ = static_cast<float>(std::exp(-1.0 / (kDragSeconds * sampleRate)));
		clear();
	}
	/// THE HOLD'S RELEASE A SAMPLE for a string of T60 `t60` (v22): its amplitude time constant, T60 / ln(1000); before a tune (t60
	/// 0), or with the hold made v21's again for a case (setHoldFollowsForTest(false)), kHoldSeconds.
	float holdRelease(double t60) const {
		return static_cast<float>(holdFollows_ && t60 > 0.0 ? std::exp(-std::log(1000.0) / (std::max(t60, 1e-3) * sr_))
		                                                     : std::exp(-1.0 / (kHoldSeconds * sr_)));
	}
	/// v21'S 2 s HOLD, FOR A CASE (Bridge Fixed 1 must not depend on which).
	void setHoldFollowsForTest(bool on) {
		holdFollows_ = on;
		release_ = holdRelease(t60_);
	}
	/// THE PATH BEFORE ITS CACHES (backlog PB 2), for the equivalence case: setFixed's pow and log, pointHeight's exp of the
	/// whole exponent, and the previous level's dxx made afresh, every call.
	void setReferenceForTest(bool on) { reference_ = on; }
	void clear() {
		for (auto& level : buf_) {
			level.fill(0.0f);
		}
		ref_ = 0.0f;
		dragEnv_ = 0.0f;
		quiet_ = true;
		rowValid_ = false;
		strikeAt_ = strikeLength_ = 0;
		yield_ = false; // off the bridge until it is seated on it again (setYield)
		bridgeNext_ = 0.0f;
	}
	/// THE NODE CAP (Resonate's Drone Detail, backlog PS): at most `cap` nodes (2..kMaxNodes; kMaxNodes, the default, is the string as
	/// it always was). Taken at the next tune(), which regrids a ringing string onto the new count.
	void setNodeCap(int cap) { cap_ = std::clamp(cap, 2, kMaxNodes); }
	int nodeCap() const { return cap_; }
	/// THE NOTE AND ITS T60. Cheap when nothing moved; a pitch change retunes (and, only when it must, regrids).
	/// `lossScale` scales the frequency-dependent loss (kSigma1: under 1 brighter, over 1 darker; the drone's strings take
	/// their brightness here). `inharmonicity` is a stiff string's B: partial m sits at m f sqrt(1 + B m^2) (v11, the
	/// drone's String Stiffness), 0 the ideal string, bit for bit.
	void tune(double hz, double t60, double lossScale = 1.0, double inharmonicity = 0.0) {
		const double f = std::clamp(hz, 1.0, 0.45 * sr_);
		const double B = std::isfinite(inharmonicity) ? std::clamp(inharmonicity, 0.0, kMaxInharmonicity) : 0.0;
		if (f == hz_ && t60 == t60_ && lossScale == lossScale_ && B == b_inh_ && capApplied_ == cap_) {
			return;
		}
		// A NEW CAP REGRIDS AT ONCE, past the hysteresis below (a cap lifted onto a string a little under its natural count would
		// otherwise keep the capped grid).
		const bool capMoved = capApplied_ != cap_;
		capApplied_ = cap_;
		hz_ = f;
		t60_ = t60;
		// THE HELD SWING FALLS AS THE STRING DOES (v22, resonate-audit: the notes' jawari cut out 0.3-0.6 s after a note-off, the
		// string decaying ~15 dB/s while a 2 s hold fell ~4.3, so it dropped clear of a point still set by its old swing): its
		// release is the string's own amplitude time constant, T60 / ln(1000), so the point stays where the string's swing is.
		// A drone string (Fixed 1: a fixed height) never reads it.
		release_ = holdRelease(t60);
		lossScale_ = lossScale;
		b_inh_ = B;
		const double pi = 3.14159265358979323846;
		// THE NODE COUNT: as many as keep the scheme stable. Ideal: lambda = 2 N f / sr <= kCourant. Stiff: lambda^2 (1 +
		// 4 r^2) <= kCourant^2 with mu = r lambda and r = N sqrt(B) / pi (B = pi^2 kappa^2 / c^2 on a unit length), a
		// quadratic in N^2.
		int want = 0;
		if (B > 0.0) {
			const double g = 4.0 * f * f / (sr_ * sr_);
			const double qa = g * 4.0 * B / (pi * pi);
			const double x = (-g + std::sqrt(g * g + 4.0 * qa * kCourant * kCourant)) / (2.0 * qa);
			want = std::clamp(static_cast<int>(std::sqrt(x)), 2, cap_);
		}
		else {
			want = std::clamp(static_cast<int>(kCourant * sr_ / (2.0 * f)), 2, cap_);
		}
		if (n_ == 0) {
			n_ = want;
		}
		else if (want < n_ || want * 4 >= n_ * 5 || (capMoved && want != n_)) {
			regrid(want);
		}
		// THE PITCH IS EXACT: the scheme's lowest mode solves sin^2(pi f / sr) = lambda^2 s^2 + 4 mu^2 s^4, s = sin(pi / 2N).
		const double s = std::sin(pi / (2.0 * n_));
		const double r = static_cast<double>(n_) * std::sqrt(B) / pi;
		const double S = std::sin(pi * f / sr_);
		double lam2 = 0.0;
		if (B > 0.0) {
			lam2 = S * S / (s * s + 4.0 * r * r * s * s * s * s);
			lam2 = std::min(lam2, kCourant * kCourant / (1.0 + 4.0 * r * r));
		}
		else {
			const double l = std::min(S / s, kCourant); // the ideal string as v10, bit for bit
			lam2 = l * l;
		}
		lam2_ = lam2;
		mu2_ = r * r * lam2;
		const double lam = std::sqrt(lam2);
		// THE DRIVE, SCALED BY THE LOSS AND THE COURANT NUMBER: a sine held on the note rings the string to kForceGain x
		// its own size or so at any T60 and any node count (MEASURED: the steady level went as 1 / s0 and as 1 / L).
		driveScale_ = kForceGain * (3.0 * std::log(10.0) / std::max(t60, 1e-3) / sr_) * lam / kCourant;
		const double k = 1.0 / sr_;
		s0k_ = 3.0 * std::log(10.0) / std::max(t60, 1e-3) * k;
		const double s1 = std::min(kSigma1 * std::max(lossScale, 0.0), 0.9 * (1.0 - lam2_ - 4.0 * mu2_) / (4.0 * k * n_ * n_));
		c1_ = 2.0 * s1 * k * n_ * n_;
		a_ = 1.0 / (1.0 + s0k_);
		b_ = 1.0 - s0k_;
		drive_ = std::clamp(static_cast<int>(std::lround(kDrivePos * n_)), 1, n_ - 1);
		point_ = std::clamp(static_cast<int>(std::lround(pointPos_ * n_)), 1, n_ - 1);
	}
	/// THE THREAD (v21, Requirement: as a tanpura's, where the thread under the string sets where it first meets the jawari): the
	/// point's place as a fraction of the length from the bridge end (kPointPos, 3%, unless set). The gap law stays relative to
	/// the string's swing scale at the new place (its mode shape there, pi l / N).
	void setPointPos(double pos) {
		const double p = std::isfinite(pos) ? std::clamp(pos, kPointPosMin, kPointPosMax) : kPointPos;
		if (p == pointPos_) {
			return;
		}
		pointPos_ = p;
		if (n_ > 1) {
			point_ = std::clamp(static_cast<int>(std::lround(pointPos_ * n_)), 1, n_ - 1);
		}
	}
	double pointPosForTest() const { return pointPos_; }
	void setBridge(float jawari, float bounce, float proximity) {
		jawari_ = std::isfinite(jawari) ? std::clamp(jawari, 0.0f, 1.0f) : 0.0f;
		bounce_ = std::isfinite(bounce) ? std::clamp(bounce, 0.0f, 1.0f) : 1.0f;
		proximity_ = std::isfinite(proximity) ? std::clamp(proximity, 0.0f, 1.0f) : 0.0f;
	}
	/// SCALED TO FIXED (v13; it was asked whether any control sets how hard a pluck presses the string into the bridge): at 0
	/// the line stands at a fraction of the string's OWN held swing, so contact is scale-invariant (a harder pluck buzzes
	/// alike, only louder); at 1 at that fraction of a FIXED height, `refAt165` at 165 Hz and as 1 / f elsewhere (the
	/// displacement of equal energy, so every pitch at one loudness meets it alike), so a harder pluck presses further into
	/// the bridge and a soft one only grazes it. Between, the scale is held^(1 - fixed) x height^fixed: geometric, so the
	/// line's ratio to the swing goes as (swing / height)^fixed and Fixed is how much level matters, as an exponent.
	/// `slope`: the height goes as f^-slope (1, equal energy; the drone's struck strings measured f^-1.35, kFixedDroneSlope).
	void setFixed(float fixed, float refAt165, double slope = 1.0) {
		fixed_ = std::isfinite(fixed) ? std::clamp(fixed, 0.0f, 1.0f) : 0.0f;
		// THE HEIGHT AND ITS LOG ONLY WHEN WHAT THEY ARE MADE OF MOVED (backlog PB 2): Resonate calls this every sample for every
		// string, and its pow (with pointHeight's two logs and an exp) was ~20 ns of a string's ~110 ns step (MEASURED, a 128-node
		// drone string alone). The same expressions on the same inputs, so the same bits (test_resonate_presets' "RESONATE perf
		// pass" case, against setReferenceForTest's road).
		if (!reference_ && refAt165 == fixedRefAt_ && slope == fixedSlopeAt_ && hz_ == fixedHzAt_) {
			return;
		}
		fixedRefAt_ = refAt165;
		fixedSlopeAt_ = slope;
		fixedHzAt_ = hz_;
		fixedHeight_ = std::max(kFloor, static_cast<float>(static_cast<double>(refAt165) * std::pow(kFixedRefHz / std::max(hz_, 1.0), slope)));
		fixedLog_ = std::log(fixedHeight_);
		fixedScale_ = std::exp(fixedLog_);
	}
	static constexpr double kFixedRefHz = 165.0;
	/// WHERE IT IS HEARD: 0 at kPickLeft (the v10 place), 1 at kPickNear, by the bridge end — brighter, more bite.
	void setPickup(float p) {
		const double t = std::isfinite(p) ? std::clamp(static_cast<double>(p), 0.0, 1.0) : 0.0;
		pick_ = kPickLeft + (kPickNear - kPickLeft) * t;
		// HELD TO THE FUNDAMENTAL'S LEVEL: its mode shape is sin(pi x), so nearer the end it is heard quieter by that.
		pickGain_ = static_cast<float>(std::sin(3.14159265358979 * kPickLeft) / std::sin(3.14159265358979 * pick_));
	}
	/// A STRIKE (v11, the drone's Cycle): a raised-cosine push `samples` long at the drive point, `amp` at its middle, on
	/// top of whatever drives it — the string played as the notes' are (by a force where it is driven), not pulled aside.
	void strike(float amp, int samples) {
		if (!(std::abs(amp) > 0.0f) || samples < 1) {
			return;
		}
		strikeAmp_ = amp;
		strikeLength_ = samples;
		strikeAt_ = 0;
		quiet_ = false;
	}
	/// AMPLITUDE-DEPENDENT LOSS (v15): once the string's heard level (its pickup, held with kDragSeconds'
	/// release) passes `level`, its loss rises by kDragRate x (excess / level)^2 per second — so a drive held on its pitch, which
	/// its resonance would otherwise ring 25x past a pluck, settles near `level`, while a pluck that never reaches it is
	/// untouched. More loss only: the energy never rises by it. 0: none.
	void setDrag(float level) { dragLevel_ = std::isfinite(level) ? std::max(level, 0.0f) : 0.0f; }
	/// The heard level the drag reads (its pickup, instant attack, kDragSeconds' release) — also what a front end
	/// bounds a string's FEED by (Resonate's drone, v15).
	float heardEnv() const { return dragEnv_; }
	float dragEnvForTest() const { return dragEnv_; }
	/// A STRIKE STOPPED WHERE IT IS (a Cycle pluck withdrawn before its grid).
	void endStrike() { strikeAt_ = strikeLength_; }
	/// WHERE IT IS HEARD, as a fraction of its length (for the time a strike takes to reach the pickup).
	double pickPosition() const { return pick_; }
	/// THE YIELDING BRIDGE (v18): on, u[0] is the bridge's displacement as handed by setBridgeNext before each step; joining,
	/// the end is seated where the bridge is now and was (so it joins without a kink). Off, the end is at rest.
	void setYield(bool on, float bridgeNow = 0.0f, float bridgePrev = 0.0f) {
		if (on && !yield_) {
			level(cur_)[0] = bridgeNow;
			level(prev_)[0] = bridgePrev;
			rowValid_ = false; // the previous level's end moved: its row is not its dxx any more
			quiet_ = quiet_ && bridgeNow == 0.0f && bridgePrev == 0.0f;
		}
		yield_ = on;
	}
	bool yielding() const { return yield_; }
	void setBridgeNext(float w) { bridgeNext_ = w; }
	/// ITS PULL ON THE BRIDGE: L^2 (u[1] - u[0]), the force its first segment exerts on its end (a node's mass is 1).
	float bridgePull() const {
		const float* u = level(cur_);
		return static_cast<float>(lam2_) * (u[1] - u[0]);
	}
	/// H less the bridge-end node's kinetic term (the bridge's own: ResonateDsp::bridgeEnergyForTest).
	double energyOffBridge() const {
		const float d = level(cur_)[0] - level(prev_)[0];
		return energy() - static_cast<double>(d) * d;
	}
	/// One sample: `force` drives it at kDrivePos (scaled by its loss, so a note held on its pitch rings at a level its
	/// decay does not set); returns the two pickups.
	void process(float force, float width, float& outL, float& outR) {
		const int n = n_;
		float drive = static_cast<float>(force * driveScale_);
		if (strikeAt_ < strikeLength_) {
			drive += strikeAmp_ * (0.5f - 0.5f * std::cos(6.2831853f * (static_cast<float>(strikeAt_) + 0.5f) / static_cast<float>(strikeLength_)));
			++strikeAt_;
		}
		if (quiet_ && drive == 0.0f && !(yield_ && bridgeNext_ != 0.0f)) {
			outL = outR = 0.0f;
			return;
		}
		const float lam2 = static_cast<float>(lam2_);
		const float c1 = static_cast<float>(c1_);
		// DRAG (v15): past setDrag's level the loss grows with the square of the excess, as air drag does with amplitude.
		// WITH NO DRAG the loss is s0k_ itself, and 1 / (1 + s0k_) and 1 - s0k_ are tune()'s a_ and b_, the same expressions on
		// the same double (backlog PS): no division per step.
		double s0 = s0k_;
		const bool dragging = dragLevel_ > 0.0f && dragEnv_ > dragLevel_;
		if (dragging) {
			const double x = static_cast<double>(dragEnv_ / dragLevel_) - 1.0;
			s0 += kDragRate * x * x / sr_;
		}
		const float a = (dragging || reference_) ? static_cast<float>(1.0 / (1.0 + s0)) : static_cast<float>(a_);
		const float b = (dragging || reference_) ? static_cast<float>(1.0 - s0) : static_cast<float>(b_);
		float* __restrict u = level(next_);
		const float* __restrict u1 = level(cur_);
		const float* __restrict u2 = level(prev_);
		// THE SCHEME, and in the same pass the swing (for the line and for silence) and the lowest point (for contact) —
		// four nodes at a time (SSE: the reductions are what the compiler will not vectorise without fast-math), the last
		// few one at a time. The same arithmetic, in the same order, either way.
		float swing = 0.0f;
		float lowest = 0.0f;
		int l = 1;
		// dxx OF THE PREVIOUS LEVEL IS LAST STEP'S dxx OF THE CURRENT ONE (backlog PB 2): the same buffer, the same expression,
		// so the same bits — kept in a row (rows_) and read back rather than made again, unless something wrote that level in
		// between (rowValid_: setYield's seat, a regrid, a clear).
		float* __restrict dxxNow = rows_[static_cast<size_t>(row_)].data();
		const float* __restrict dxxThen = rows_[static_cast<size_t>(1 - row_)].data();
		const bool reuse = rowValid_ && !reference_;
		// A STIFF STRING (mu^2 > 0): the same scheme less mu^2 dxxxx u, dxxxx = dxx(dxx u) with dxx u = 0 at the ends (the
		// ends simply supported): dxx u into a row first, then four nodes at a time as below, the last few one at a time.
		if (mu2_ > 0.0) {
			const float mu2 = static_cast<float>(mu2_);
			float* w = dxxNow;
			w[0] = 0.0f;
			for (int i = 1; i < n; ++i) {
				w[static_cast<size_t>(i)] = (u1[i + 1] + u1[i - 1]) - 2.0f * u1[i];
			}
			w[static_cast<size_t>(n)] = 0.0f;
			const float* __restrict wp = w;
			const __m128 va = _mm_set1_ps(a);
			const __m128 vb = _mm_set1_ps(b);
			const __m128 vl = _mm_set1_ps(lam2);
			const __m128 vm = _mm_set1_ps(mu2);
			const __m128 vc = _mm_set1_ps(c1);
			const __m128 two = _mm_set1_ps(2.0f);
			const __m128 sign = _mm_set1_ps(-0.0f);
			__m128 vmax = _mm_setzero_ps();
			__m128 vmin = _mm_setzero_ps();
			for (; l + 3 < n; l += 4) {
				const __m128 c = _mm_loadu_ps(u1 + l);
				const __m128 p = _mm_loadu_ps(u2 + l);
				const __m128 dxx = _mm_loadu_ps(wp + l);
				const __m128 dxxp = reuse ? _mm_loadu_ps(dxxThen + l)
				                          : _mm_sub_ps(_mm_add_ps(_mm_loadu_ps(u2 + l + 1), _mm_loadu_ps(u2 + l - 1)), _mm_mul_ps(two, p));
				const __m128 d4 = _mm_sub_ps(_mm_add_ps(_mm_loadu_ps(wp + l + 1), _mm_loadu_ps(wp + l - 1)), _mm_mul_ps(two, dxx));
				const __m128 inner = _mm_add_ps(
				    _mm_sub_ps(_mm_add_ps(_mm_sub_ps(_mm_mul_ps(two, c), _mm_mul_ps(vb, p)), _mm_mul_ps(vl, dxx)), _mm_mul_ps(vm, d4)),
				    _mm_mul_ps(vc, _mm_sub_ps(dxx, dxxp)));
				const __m128 v = _mm_mul_ps(va, inner);
				_mm_storeu_ps(u + l, v);
				vmax = _mm_max_ps(vmax, _mm_andnot_ps(sign, v));
				vmin = _mm_min_ps(vmin, v);
			}
			alignas(16) float mx[4];
			alignas(16) float mn[4];
			_mm_store_ps(mx, vmax);
			_mm_store_ps(mn, vmin);
			swing = std::max(std::max(mx[0], mx[1]), std::max(mx[2], mx[3]));
			lowest = std::min(std::min(mn[0], mn[1]), std::min(mn[2], mn[3]));
			for (; l < n; ++l) {
				const size_t i = static_cast<size_t>(l);
				const float dxx = w[i];
				const float dxxp = reuse ? dxxThen[i] : (u2[l + 1] + u2[l - 1]) - 2.0f * u2[l];
				const float d4 = (w[i + 1] + w[i - 1]) - 2.0f * w[i];
				const float v = a * ((((2.0f * u1[l] - b * u2[l]) + lam2 * dxx) - mu2 * d4) + c1 * (dxx - dxxp));
				u[l] = v;
				swing = std::max(swing, std::abs(v));
				lowest = std::min(lowest, v);
			}
		}
		else {
			{
				const __m128 va = _mm_set1_ps(a);
				const __m128 vb = _mm_set1_ps(b);
				const __m128 vl = _mm_set1_ps(lam2);
				const __m128 vc = _mm_set1_ps(c1);
				const __m128 two = _mm_set1_ps(2.0f);
				const __m128 sign = _mm_set1_ps(-0.0f);
				__m128 vmax = _mm_setzero_ps();
				__m128 vmin = _mm_setzero_ps();
				for (; l + 3 < n; l += 4) {
					const __m128 c = _mm_loadu_ps(u1 + l);
					const __m128 p = _mm_loadu_ps(u2 + l);
					const __m128 dxx = _mm_sub_ps(_mm_add_ps(_mm_loadu_ps(u1 + l + 1), _mm_loadu_ps(u1 + l - 1)), _mm_mul_ps(two, c));
					_mm_storeu_ps(dxxNow + l, dxx);
					const __m128 dxxp = reuse ? _mm_loadu_ps(dxxThen + l)
					                          : _mm_sub_ps(_mm_add_ps(_mm_loadu_ps(u2 + l + 1), _mm_loadu_ps(u2 + l - 1)), _mm_mul_ps(two, p));
					const __m128 inner = _mm_add_ps(_mm_add_ps(_mm_sub_ps(_mm_mul_ps(two, c), _mm_mul_ps(vb, p)), _mm_mul_ps(vl, dxx)),
					                                _mm_mul_ps(vc, _mm_sub_ps(dxx, dxxp)));
					const __m128 v = _mm_mul_ps(va, inner);
					_mm_storeu_ps(u + l, v);
					vmax = _mm_max_ps(vmax, _mm_andnot_ps(sign, v));
					vmin = _mm_min_ps(vmin, v);
				}
				alignas(16) float mx[4];
				alignas(16) float mn[4];
				_mm_store_ps(mx, vmax);
				_mm_store_ps(mn, vmin);
				swing = std::max(std::max(mx[0], mx[1]), std::max(mx[2], mx[3]));
				lowest = std::min(std::min(mn[0], mn[1]), std::min(mn[2], mn[3]));
			}
			for (; l < n; ++l) {
				const float dxx = (u1[l + 1] + u1[l - 1]) - 2.0f * u1[l];
				dxxNow[l] = dxx;
				const float dxxp = reuse ? dxxThen[l] : (u2[l + 1] + u2[l - 1]) - 2.0f * u2[l];
				const float v = a * (((2.0f * u1[l] - b * u2[l]) + lam2 * dxx) + c1 * (dxx - dxxp));
				u[l] = v;
				swing = std::max(swing, std::abs(v));
				lowest = std::min(lowest, v);
			}
		}
		u[0] = yield_ ? bridgeNext_ : 0.0f; // THE BRIDGE END: at rest, or riding the yielding bridge
		u[n] = 0.0f;
		u[drive_] += drive;
		lowest = std::min(lowest, u[drive_]);
		swing = std::max(swing, std::abs(u[drive_]));
		if (jawari_ > 0.0f) {
			// THE POINT: below rest at the string's scale there x gap(J) x (1 - kProximity Jawari proximity). A node clear of
			// it (the common case) costs nothing more.
			ref_ = std::max(swing, ref_ * release_);
			const float h = pointHeight();
			if (u[point_] < -h) {
				contact(h);
			}
		}
		// THE TIME LEVELS ROTATE: previous <- current <- next (indices, not copies).
		const int oldPrev = prev_;
		prev_ = cur_;
		cur_ = next_;
		next_ = oldPrev;
		// AND THE ROWS: this step's dxx of the level now previous is the next step's dxxp.
		row_ = 1 - row_;
		rowValid_ = true;
		// EXACT ZERO: a string left alone is flushed, and then skipped until it is driven.
		if (swing < 1e-15f && lastSwing_ < 1e-15f && (!yield_ || (u[0] == 0.0f && u1[0] == 0.0f))) {
			clear();
		}
		else {
			quiet_ = false;
		}
		lastSwing_ = swing;
		// THE PICKUPS' NODES AND FRACTIONS move only with the pickup, the width or the grid (backlog PS): pick()'s own arithmetic,
		// made when one of them moves and read back otherwise, so the same bits.
		if (reference_) {
			outL = pick(pick_) * kOutGain * pickGain_;
			outR = pick(pick_ + kPickSpread * std::clamp(width, 0.0f, 1.0f)) * kOutGain * pickGain_;
		}
		else {
			if (pickN_ != n_ || pickAt_ != pick_ || !(pickWidth_ == width)) {
				pickN_ = n_;
				pickAt_ = pick_;
				pickWidth_ = width;
				pickPlace(pick_, pickI_[0], pickFr_[0]);
				pickPlace(pick_ + kPickSpread * std::clamp(width, 0.0f, 1.0f), pickI_[1], pickFr_[1]);
			}
			const float* uc = level(cur_);
			outL = (uc[pickI_[0]] + pickFr_[0] * (uc[pickI_[0] + 1] - uc[pickI_[0]])) * kOutGain * pickGain_;
			outR = (uc[pickI_[1]] + pickFr_[1] * (uc[pickI_[1] + 1] - uc[pickI_[1]])) * kOutGain * pickGain_;
		}
		dragEnv_ = std::max(std::abs(outL), dragEnv_ * dragRelease_); // the HEARD level
	}

	int nodes() const { return n_; }
	double courant() const { return std::sqrt(lam2_); }
	double tunedHz() const { return hz_; }
	double t60() const { return t60_; }
	float heldSwing() const { return ref_; }
	/// The scheme's energy for the ideal string at the current time levels (see the top).
	double energy() const { return energyOf(level(cur_), level(prev_)); }
	const float* displacement() const { return level(cur_); }
	bool quiet() const { return quiet_; }

	/// THE LEVEL SCALES, by measurement (test_fx_resonate): the drive per unit of force per unit of loss, and the pickups.
	static constexpr double kForceGain = 25.0;
	static constexpr float kOutGain = 1.0f;
	uint64_t projectionsForTest() const { return projections_; }
	/// Samples on which the point met the string (v17: the bloom is a contact RATE that grows after a pluck).
	uint64_t contactsForTest() const { return contacts_; }
	int pointNodeForTest() const { return point_; }
	/// THE POINT'S DEPTH below rest now (process's own): its scale x its mode shape x gap(Jawari) x the orbit's term. FIXED (v13):
	/// the scale moves from the held swing (0) to a fixed height (1), geometrically — at Fixed 1, exp(0 x log(held) + log(height)),
	/// so the held swing (and its release) cannot reach it.
	float pointHeight() const {
		const float held = std::max(ref_, kFloor);
		// AT FIXED 1 (the drone's, always) the exponent is 0 x log(held) + log(height): +-0 plus log(height), which is log(height)
		// itself, so its exp is setFixed's cached one, the same bits — unless held is a NaN, which the full expression carries.
		const float scale = !(fixed_ > 0.0f)                       ? held
		                    : (fixed_ == 1.0f && !reference_ && !std::isnan(held)) ? fixedScale_
		                    : std::exp((1.0f - fixed_) * std::log(held) + fixed_ * (reference_ ? std::log(fixedHeight_) : fixedLog_));
		const float shape = 3.14159265f * static_cast<float>(point_) / static_cast<float>(n_);
		return scale * shape * gapOf(jawari_) * (1.0f - kProximity * proximity_ * jawari_);
	}
	uint64_t regridsForTest() const { return regrids_; }

private:
	float* level(int i) { return buf_[static_cast<size_t>(i)].data(); }
	const float* level(int i) const { return buf_[static_cast<size_t>(i)].data(); }

	// IN FLOAT, so it vectorises: it is compared only with itself a moment later, never accumulated.
	double energyOf(const float* x, const float* y) const {
		float kin = 0.0f;
		float pot = 0.0f;
		for (int l = 0; l < n_; ++l) {
			const float d = x[l] - y[l];
			kin += d * d;
			pot += (x[l + 1] - x[l]) * (y[l + 1] - y[l]);
		}
		double stiff = 0.0;
		if (mu2_ > 0.0) {
			// THE STIFF STRING'S PART, mu^2 sum (dxx x)(dxx y), dxx 0 at the ends.
			for (int l = 1; l < n_; ++l) {
				stiff += static_cast<double>(((x[l + 1] + x[l - 1]) - 2.0f * x[l]) * ((y[l + 1] + y[l - 1]) - 2.0f * y[l]));
			}
		}
		return static_cast<double>(kin) + lam2_ * static_cast<double>(pot) + mu2_ * stiff;
	}

	// THE CONTACT, LOCALLY (only when the point's node is past it). The excess at the point goes to its neighbours, and kPasses
	// of transfer reach at most kPasses further, so every change is inside [point - kPasses, point + kPasses] (clamped to the
	// free nodes); the next level there is kept, the contact applied, and the change accepted only as far as the energy guard
	// (at the end) allows. A velocity reversal written into the current level (u) fired the guard on nearly every sample
	// (MEASURED 2026-09-28), so the reversal is kept to the next level.
	void contact(float B) {
		const int P = point_;
		const auto line = [&](int l) { return l == P ? B : 1e30f; };
		const int n = n_;
		float* u = level(next_);
		const float* u1 = level(cur_);
		if (!(u[P] < -B)) {
			return;
		}
		const int lo = std::max(1, P - kPasses);
		const int hi = std::min(n - 1, P + kPasses);
		++contacts_;
		std::array<float, kMaxNodes + 1> keepU;
		for (int l = lo; l <= hi; ++l) {
			keepU[static_cast<size_t>(l)] = u[l];
		}
		for (int pass = 0; pass < kPasses; ++pass) {
			bool any = false;
			for (int l = lo; l <= hi; ++l) {
				const float Bl = line(l);
				if (!(u[l] < -Bl)) {
					continue;
				}
				any = true;
				// PAST THE LINE BY d: the excess goes to the neighbours, half each (it doesnt fold, it puts half of
				// its energy which exceeds the line to each of its neighbor nodes), and the node's step into the line,
				// v = u+ - u, is REVERSED at Bounce — it comes off at u - Bounce v, never back below the line. Bounce 0 is
				// the node left on the line.
				const float d = -Bl - u[l];
				const float v = u[l] - u1[l];
				u[l] = (v < 0.0f) ? std::max(-Bl, u1[l] - bounce_ * v) : -Bl;
				if (l - 1 >= lo) {
					u[l - 1] -= 0.5f * d;
				}
				if (l + 1 <= hi) {
					u[l + 1] -= 0.5f * d;
				}
			}
			if (!any) {
				break;
			}
		}
		// THE ENERGY GUARD. Only the next level moved (by c, zero outside the window), so with the current level b fixed,
		// H(a + t c) - H(a) = t L + t^2 Q with L = sum 2 (a - b) c + L^2 sum dx c dx b and Q = sum c^2 > 0. The largest
		// t <= 1 with t L + t^2 Q <= 0 is kept: all of the contact when L + Q <= 0, else t = -L / Q (or none if L >= 0).
		double lin = 0.0;
		double quad = 0.0;
		// c over lo-1..hi+1, zero at the two ends: the change, in a plain array so the sums below are straight loops.
		std::array<float, kMaxNodes + 3> c {};
		for (int l = lo; l <= hi; ++l) {
			c[static_cast<size_t>(l - lo + 1)] = u[l] - keepU[static_cast<size_t>(l)];
		}
		for (int l = lo; l <= hi; ++l) {
			const float cl = c[static_cast<size_t>(l - lo + 1)];
			lin += 2.0 * static_cast<double>((keepU[static_cast<size_t>(l)] - u1[l]) * cl);
			quad += static_cast<double>(cl * cl);
		}
		for (int l = lo - 1; l <= hi; ++l) {
			const float dc = c[static_cast<size_t>(l - lo + 2)] - c[static_cast<size_t>(l - lo + 1)];
			lin += lam2_ * static_cast<double>(dc * (u1[l + 1] - u1[l]));
		}
		if (mu2_ > 0.0) {
			// AND THE STIFF STRING'S: mu^2 sum (dxx c)(dxx b), over where dxx c can be nonzero.
			const auto cAt = [&](int l) { return (l < lo || l > hi) ? 0.0f : c[static_cast<size_t>(l - lo + 1)]; };
			for (int l = std::max(1, lo - 1); l <= std::min(n - 1, hi + 1); ++l) {
				const float dc = (cAt(l + 1) + cAt(l - 1)) - 2.0f * cAt(l);
				lin += mu2_ * static_cast<double>(dc * ((u1[l + 1] + u1[l - 1]) - 2.0f * u1[l]));
			}
		}
		if (lin + quad <= 0.0) {
			return;
		}
		const double t = (lin < 0.0 && quad > 0.0) ? std::min(1.0, -lin / quad) : 0.0;
		for (int l = lo; l <= hi; ++l) {
			const size_t i = static_cast<size_t>(l);
			u[l] = keepU[i] + static_cast<float>(t) * (u[l] - keepU[i]);
		}
		++projections_;
	}

	/// THE DISPLACEMENT AT A FRACTION OF THE LENGTH, linearly between its two nodes.
	float pick(double at) const {
		const float* u = level(cur_);
		int i = 0;
		float fr = 0.0f;
		pickPlace(at, i, fr);
		return u[i] + fr * (u[i + 1] - u[i]);
	}
	/// WHERE `at` FALLS ON THE GRID: its node and the fraction to the next (pick's, and process's cache of it).
	void pickPlace(double at, int& i, float& fr) const {
		const double x = std::clamp(at, 0.0, 1.0) * n_;
		i = std::min(static_cast<int>(x), n_ - 1);
		fr = static_cast<float>(x - i);
	}

	/// BOTH TIME LEVELS ONTO `m` SEGMENTS, by linear interpolation at the same fractions of the length.
	void regrid(int m) {
		if (m == n_) {
			return;
		}
		float* a = level(next_); // the free level, as scratch
		const float* cur = level(cur_);
		const float* prev = level(prev_);
		std::array<float, kMaxNodes + 1> b {};
		for (int l = 0; l <= m; ++l) {
			const double x = static_cast<double>(l) / m * n_;
			const int i = std::min(static_cast<int>(x), n_ - 1);
			const float fr = static_cast<float>(x - i);
			a[l] = l == m ? 0.0f : cur[i] + fr * (cur[i + 1] - cur[i]);
			b[static_cast<size_t>(l)] = l == m ? 0.0f : prev[i] + fr * (prev[i + 1] - prev[i]);
		}
		for (int l = m + 1; l <= kMaxNodes; ++l) {
			a[l] = 0.0f;
		}
		std::copy(b.begin(), b.end(), level(prev_));
		// the resampled current level becomes current; the old current is free
		const int oldCur = cur_;
		cur_ = next_;
		next_ = oldCur;
		n_ = m;
		++regrids_;
		rowValid_ = false; // a new grid: no row describes it
	}

	std::array<std::array<float, kMaxNodes + 1>, 3> buf_ {};
	int next_ = 0;
	int cur_ = 1;
	int prev_ = 2;
	double sr_ = 48000.0;
	double hz_ = 0.0;
	double t60_ = 0.0;
	int n_ = 0;
	double lam2_ = 0.0;
	double s0k_ = 0.0;
	double driveScale_ = 0.0;
	double c1_ = 0.0;
	double a_ = 1.0;
	double b_ = 1.0;
	int drive_ = 1;
	float jawari_ = 0.0f;
	float bounce_ = 1.0f;
	float proximity_ = 0.0f;
	float fixed_ = 0.0f;
	float fixedHeight_ = 1.0f;
	float fixedLog_ = 0.0f;   // std::log(fixedHeight_), with it
	float fixedScale_ = 1.0f; // std::exp(fixedLog_): pointHeight's scale at Fixed 1
	float fixedRefAt_ = -1.0f; // what setFixed last built them from (none yet: no ref is negative)
	double fixedSlopeAt_ = 0.0;
	double fixedHzAt_ = -1.0;
	float strikeAmp_ = 0.0f;
	int strikeLength_ = 0;
	int strikeAt_ = 0;
	double lossScale_ = 1.0;
	double b_inh_ = 0.0;
	double mu2_ = 0.0;
	double pick_ = kPickLeft;
	float pickGain_ = 1.0f;
	// process's cache of the two pickups' places (pickPlace), and what it was made from (none yet: n 0 is no grid).
	int cap_ = kMaxNodes;
	int capApplied_ = kMaxNodes;
	int pickN_ = 0;
	double pickAt_ = -1.0;
	float pickWidth_ = -1.0f;
	int pickI_[2] {};
	float pickFr_[2] {};
	float ref_ = 0.0f;
	float lastSwing_ = 0.0f;
	float dragLevel_ = 0.0f;
	float dragEnv_ = 0.0f;
	float dragRelease_ = 1.0f;
	float release_ = 0.9999f;
	bool holdFollows_ = true;
	bool reference_ = false; // setReferenceForTest
	// dxx of the current level, this step's (rows_[row_]) and the last (the other), and whether the last still describes the level now
	// previous (process).
	alignas(16) std::array<std::array<float, kMaxNodes + 4>, 2> rows_ {};
	int row_ = 0;
	bool rowValid_ = false;
	bool quiet_ = true;
	bool yield_ = false;
	float bridgeNext_ = 0.0f;
	uint64_t projections_ = 0;
	uint64_t contacts_ = 0;
	int point_ = 1;
	double pointPos_ = kPointPos;
	uint64_t regrids_ = 0;
};

} // namespace feathers::fx::resonate
