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

#include "dsp/weave_string.h"

#include "character/util.hpp"
#include "dsp/portable_trig.h"
#include "dsp/stair_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace feathers {

using owlet::dsp::kPhiWeaveMaxDisplacement;
using owlet::dsp::kPhiWeaveMaxVelocity;
using owlet::dsp::PhiWeaveParams;
using owlet::dsp::WeaveBow;

namespace {

constexpr int kN = WeaveString::kNodes;
constexpr float kPi = 3.14159265f;

// The firmware's kPhiWeaveRefAmplitude (2^30, square parity) in the plugin's ±1 full scale, which is
// q31's rail: HALF of full scale, the convention morph_renderer and stair_renderer already normalise
// against (a nominal zone near ±0.5, the rail at ±1.0).
constexpr float kRefAmplitude = 0.5f;
// The hard limit on a table entry: the firmware's 2^31 clamp, twice the reference. It is the ceiling
// and not the reference that sits at 1.0 because the reference is exceeded in ordinary playing —
// outGain runs 0.70..1.25 and the AGC's scale is slewed. MEASURED 2026-09-25 (64 phases x 30 s): the
// pre-clamp table reaches 1.18x the reference parked with the laws' bow, 1.24x with a pluck every 2 s,
// and 1.26x under the bounded case's stress (bow x2, crossfade swept), and passes the reference on 9.9%
// of ticks parked. A ceiling AT the reference (the plan's first draft: "reference 1.0, ceiling 1.0")
// would hard-clip every one of those where the firmware never clipped. DO NOT "fix" this back to 1.0.
constexpr float kTableCeiling = 1.0f;

// Wrap to [-0.5, 0.5): the firmware's truncation form, which is what its bow and pluck windows use.
float wrapHalf(float d) {
	return d - (static_cast<float>(static_cast<int32_t>(d + 1.5f)) - 1.0f);
}

// t^2 (3 - 2t): zero derivative at both ends, so each fade segment joins the next C1.
float smoothstep(float t) {
	return t * t * (3.0f - 2.0f * t);
}

float lcgUnit(uint32_t& state) {
	state = state * 1664525u + 1013904223u;
	return static_cast<float>(static_cast<int32_t>(state)) * (1.0f / 2147483648.0f);
}

} // namespace

int WeaveString::tickLengthFor(double sampleRate, double speed) {
	return std::clamp(static_cast<int>(std::lround(sampleRate / (kTickHz * speed))), 1, kMaxTickLength);
}

void WeaveString::applyTickLength(int length) {
	tickLength_ = length;
	pendingTickLength_ = length;
	// The fade and travel positions of every window offset, by the expressions the per-call fade used
	// (division, not a reciprocal multiply, so the tables are bit-identical to what they replace).
	const int half = tickLength_ >> 1;
	for (int k = 0; k < tickLength_; ++k) {
		fadeT_[k] = (k < half) ? smoothstep(static_cast<float>(k + 1) / static_cast<float>(half))
		                       : smoothstep(static_cast<float>(k - half + 1) / static_cast<float>(tickLength_ - half));
		travelFrac_[k] = static_cast<float>(k + 1) / static_cast<float>(tickLength_);
	}
}

void WeaveString::setSpeed(double speed) {
	if (!std::isfinite(speed)) {
		return;
	}
	speed_ = std::clamp(speed, kMinSpeed, kMaxSpeed);
	if (sampleRate_ > 0.0) {
		pendingTickLength_ = tickLengthFor(sampleRate_, speed_);
	}
}

void WeaveString::prepare(double sampleRate) {
	sampleRate_ = sampleRate;
	applyTickLength(tickLengthFor(sampleRate, speed_));
	if (!lawsSet_) {
		const PhiWeaveParams p = owlet::dsp::buildPhiWeaveLaws(0.0);
		setLaws(p, p);
	}
	reset();
}

void WeaveString::setLaws(const PhiWeaveParams& a, const PhiWeaveParams& b) {
	a_ = a;
	b_ = b;
	lawsSet_ = true;
	effCfCached_ = -2.f; // forces the rebuild at the next tick
}

void WeaveString::setCrossfade(float cf) {
	if (std::isfinite(cf)) {
		cf_ = std::clamp(cf, 0.0f, 1.0f);
	}
}

void WeaveString::reseatCrossfade(float cf) {
	if (std::isfinite(cf)) {
		cf_ = std::clamp(cf, 0.0f, 1.0f);
		if (prevTickCf_ >= 0.0f) {
			prevTickCf_ = cf_;
		}
	}
}

void WeaveString::setExcitation(const Excitation& e) {
	exc_.bowScale = std::isfinite(e.bowScale) ? std::clamp(e.bowScale, 0.0f, kMaxBowScale) : Excitation {}.bowScale;
}

void WeaveString::pluck(float amp, float pos) {
	pluckPending_ = true;
	// Negative or non-finite means "the laws' own"; a finite position wraps into [0, 1) here, before any
	// window's int cast can see it.
	pluckAmpReq_ = (std::isfinite(amp) && amp >= 0.0f) ? std::min(amp, kMaxPluckAmp) : -1.0f;
	pluckPosReq_ = (std::isfinite(pos) && pos >= 0.0f) ? pos - std::floor(pos) : -1.0f;
}

void WeaveString::restPose(const float* stiffness, const float* coupling, const float* home, float* out) {
	// The Ring's system (weave_models.cpp): row i is -c[i] x[i-1] + (2c[i] + k[i]) x[i] - c[i] x[i+1] =
	// k[i] home[i], cyclic, solved densely in double.
	weave::NodeLaws laws {};
	for (int i = 0; i < kNodes; ++i) {
		laws.k[i] = stiffness[i];
		laws.gain[i] = coupling[i];
		laws.home[i] = home[i];
	}
	weave::RestPoseScratch scratch; // off the audio thread: on the caller's stack
	weave::restPose(WeaveModel::Ring, laws, scratch, out);
}

void WeaveString::restPose(const PhiWeaveParams& laws, float* out) {
	restPose(laws.stiffness, laws.coupling, laws.home, out);
}

void WeaveString::restScanPath(WeaveModel m, const PhiWeaveParams& laws, float* out) {
	weave::NodeLaws nodeLaws {};
	weave::mapLaws(m, laws.stiffness, laws.coupling, laws.damping, laws.home, laws.bowBalance, nodeLaws);
	float rest[kMaxNodes];
	weave::RestPoseScratch scratch; // off the audio thread: on the caller's stack
	weave::restPose(m, nodeLaws, scratch, rest);
	weave::scanPath(m, rest, out);
}

// The model's nodes seated at its rest pose under the current effective laws, at rest, with the
// smoother settled on it and the path refreshed.
void WeaveString::seatAtRest() {
	// Solved into the cached pose itself, which is then current: a reset or a model switch costs one
	// solve, not two.
	weave::restPose(model_, eff_, restScratch_, restNodes_);
	restStale_ = false;
	restSolvedTick_ = ticks_;
	const int n = weave::nodeCount(model_);
	for (int i = 0; i < n; ++i) {
		x_[i] = restNodes_[i];
		v_[i] = 0.f;
		xs1_[i] = restNodes_[i];
		xs_[i] = restNodes_[i];
	}
	refreshPath();
}

// THE REST-POSE SOLVE, ON ASK. Never in rebuildEff: a crossfade that moves every tick (a knob or a
// modulation on Morph or Character, the synth's puck) rebuilds the laws every tick, and a dense solve
// there cost 3.9 us a tick into the Ring and 20 into the Membrane (bench_weave_models) — paid by the synth,
// which never reads the pose. On ask: at once if the laws did not move at the last tick (so a pose read
// with the laws still is always exact), otherwise once the cached one is kRestPoseMaxAgeTicks old.
//
// WHY RESONATE'S GATE TOLERATES A POSE THAT OLD. The pose is stale only while the laws move, and the moment
// they stop the next ask solves exactly. The gate reads an ENVELOPE of this energy that falls at most
// kGateRelease (0.95) a tick and latches shut only below 1e-3 of full in amplitude (1e-6 in energy), which it
// cannot reach from full in fewer than ln(1e-6) / ln(0.95) = 270 ticks. A moving MORPH is itself excitation,
// holding the gate open whatever this reads. A moving CHARACTER is not; a pose at most 32 ticks (~93 ms) old
// can then misread the energy, but the envelope falls at most 0.95^32 = 0.19 over those ticks, so the shut
// can come early only for a string already within 5.2x (7 dB of amplitude) of the floor: quieter than 53 dB
// under full.
// Everywhere else it moves the level the gate opens to while the laws sweep, and not when it shuts.
void WeaveString::refreshRestPose() const {
	if (!restStale_) {
		return;
	}
	const bool lawsStill = lawsMovedTick_ + 1 < ticks_; // the last tick did not rebuild them
	if (lawsStill || ticks_ - restSolvedTick_ >= kRestPoseMaxAgeTicks) {
		weave::restPose(model_, eff_, restScratch_, restNodes_);
		restStale_ = false;
		restSolvedTick_ = ticks_;
		++restSolves_;
	}
}

const float* WeaveString::restNodes() const {
	refreshRestPose();
	return restNodes_;
}

float WeaveString::bowDeparture() const {
	const float* rest = restNodes();
	const int n = weave::nodeCount(model_);
	float sum = 0.f;
	float weight = 0.f;
	for (int i = 0; i < n; ++i) {
		const float w = std::abs(effBowWin_[i]);
		sum += w * (x_[i] - rest[i]);
		weight += w;
	}
	return weight > 0.f ? sum / weight : 0.f;
}

// How far the string is from rest as of the last tick: sum over nodes of (x - rest)^2 + v^2. Nothing moves
// the nodes between ticks, so the first ask after a tick works it out and the rest read it back.
float WeaveString::motionEnergy() const {
	if (energyTick_ != ticks_) {
		refreshRestPose();
		const int n = weave::nodeCount(model_);
		float e = 0.f;
		for (int i = 0; i < n; ++i) {
			const float d = x_[i] - restNodes_[i];
			e += d * d + v_[i] * v_[i];
		}
		motionEnergy_ = e;
		energyTick_ = ticks_;
	}
	return motionEnergy_;
}

// A DELIBERATE DIFFERENCE FROM THE FIRMWARE: its x[] started at ZERO and grew into the pose over its
// first second or so (stiffness pulling a flat string toward home), with its first tables all zero.
// Here the ring starts AT REST in restPose(), with valid tables built from it, so a freshly reset ring
// sounds its settled cycle at once and there is no startup swell to fade from.
void WeaveString::reset() {
	model_ = pendingModel_;
	rebuildEff(cf_);
	effCfCached_ = cf_;
	seatAtRest();
	bowPhase_ = 0.f;
	bowLP_ = 0.f;
	bowWalk_ = 0.f;
	prevTickCf_ = -1.f;
	noiseState_ = 0x2545F491u;
	travelPhase_ = 0.f;
	travelPrev_ = 0.f;
	pluck();

	seatAgc();
	for (int set = 0; set < 3; ++set) {
		buildTables(scanPath(), agcScale_, set);
		if (restTables_) {
			buildRestTables(agcScale_, set);
			buildMotionTables(set, set);
		}
	}

	windowPos_ = 0;
	ticks_ = 0;
	restSolvedTick_ = 0;
	lawsMovedTick_ = 0;
	motionEnergy_ = 0.f; // as of tick 0: at rest
	energyTick_ = 0;
	xRailHits_ = 0;
	vRailHits_ = 0;
	bridgeContacts_ = 0;
	lineBudgetBound_ = 0;
	lineContactSteps_ = lineCleanSteps_ = 0;
	lineSumDriftMax_ = 0.0;
	lineRef_ = 0.f;
	std::fill(std::begin(contactEnv_), std::end(contactEnv_), 0.0f);
	bridgeMaxGain_ = std::numeric_limits<float>::lowest();
}

// The AGC settled on the current path, by updateAgc's rule without its slew, so the next tick does not
// slew from a guess: at reset(), and at a model switch, where the old model's scale would otherwise be
// carried onto the new one (MEASURED 2026-09-25: Ring -> Membrane at phase 0.37 opened at 0.46x the
// Membrane's settled level and Membrane -> Bar at 2.65x, each recovering over ~1.5 s of release).
void WeaveString::seatAgc() {
	const float* path = scanPath();
	float peak = 0.f;
	float sumSq = 0.f;
	for (int i = 0; i < kN; ++i) {
		peak = std::max(peak, std::abs(path[i]));
		sumSq += path[i] * path[i];
	}
	agcPeak_ = peak;
	agcRms_ = std::sqrt(sumSq * (1.0f / static_cast<float>(kN)));
	agcScale_ = agcTarget(agcPeak_, agcRms_, effOutGain_);
}

// Rebuild the crossfaded walk laws. Called only when the crossfade actually moves (epsilon-guarded):
// a parked crossfade costs nothing here.
void WeaveString::rebuildEff(float cf) {
	const PhiWeaveParams& a = a_;
	const PhiWeaveParams& b = b_;
	const float cfInv = 1.0f - cf;

	effBowDepth_ = cfInv * a.bowDepth + cf * b.bowDepth;
	effBowRate_ = cfInv * a.bowRate + cf * b.bowRate;
	effTravelRate_ = cfInv * a.travelRate + cf * b.travelRate;
	effOutGain_ = cfInv * a.outGain + cf * b.outGain;
	effShimmer_ = cfInv * a.shimmerAlpha + cf * b.shimmerAlpha;

	const float bowPos = cfInv * a.bowPos + cf * b.bowPos;
	effBowPos_ = bowPos;
	const float bowSpread = cfInv * a.bowSpread + cf * b.bowSpread;
	const float invSpread = 1.0f / bowSpread;
	// The laws crossfade on their own 32 entries, then land on the model's nodes.
	float k[kN];
	float c[kN];
	float d[kN];
	float home[kN];
	float bowBal[kN];
	for (int i = 0; i < kN; ++i) {
		k[i] = cfInv * a.stiffness[i] + cf * b.stiffness[i];
		c[i] = cfInv * a.coupling[i] + cf * b.coupling[i];
		d[i] = cfInv * a.damping[i] + cf * b.damping[i];
		home[i] = cfInv * a.home[i] + cf * b.home[i];
		bowBal[i] = cfInv * a.bowBalance[i] + cf * b.bowBalance[i];
	}
	weave::mapLaws(model_, k, c, d, home, bowBal, eff_);
	weave::windows(model_, bowPos, invSpread, effBowWin_);
	const int n = weave::nodeCount(model_);
	for (int i = 0; i < n; ++i) {
		effBowWin_[i] = eff_.bowBal[i] * effBowWin_[i];
	}
	// The rest pose these laws settle to is NOT solved here: it is marked stale, and solved on ask
	// (refreshRestPose).
	restStale_ = true;
	lawsMovedTick_ = ticks_;
	// THE BRIDGE SITS UNDER THE BOW, so it moves with the laws.
	rebuildBridge();
}

// One physics sub-tick at HALF the original per-buffer dt: spring constants scale by 1/4 (dt^2),
// first-order terms (damping, bow/travel rates, velocity bounds) by 1/2, AGC release by its square root.
// The string's audible mode frequencies and decay are unchanged - but its motion is sampled at ~688 Hz
// instead of ~344 Hz, pushing tick-sampling images of fast modes up an octave and halving their
// amplitude. Returns the AGC output scale.
float WeaveString::step(float cf) {
	const PhiWeaveParams& a = a_;
	const PhiWeaveParams& b = b_;
	const float cfInv = 1.0f - cf;

	// Morph-bow: the crossfade's MOTION injects energy — turning the morph bows the string.
	float morphBow = 0.0f;
	if (prevTickCf_ >= 0.0f) {
		morphBow = std::abs(cf - prevTickCf_) * (cfInv * a.morphBowGain + cf * b.morphBowGain);
	}
	lastMorphBow_ += morphBow;
	prevTickCf_ = cf;

	// Bow excitation: shared state advances once (half-dt rate); each bank's MODE shapes it, then the two
	// forces crossfade (click-free mixed morphs)
	bowPhase_ += effBowRate_ * 0.5f;
	if (bowPhase_ >= 1.0f) {
		bowPhase_ -= 1.0f;
	}
	const float bowRand = lcgUnit(noiseState_);
	bowLP_ += std::min(1.0f, effBowRate_ * 8.0f) * (bowRand - bowLP_);
	bowWalk_ += effBowRate_ * 2.0f * bowRand;
	if (bowWalk_ > 1.0f) {
		bowWalk_ = 2.0f - bowWalk_;
	}
	else if (bowWalk_ < -1.0f) {
		bowWalk_ = -2.0f - bowWalk_;
	}

	auto bowShape = [this](WeaveBow mode) -> float {
		const float p = bowPhase_;
		switch (mode) {
		case WeaveBow::StickSlip: // slow drag, fast snap
			return (p < 0.85f) ? (p * (2.0f / 0.85f) - 1.0f) : ((1.0f - p) * (2.0f / 0.15f) - 1.0f);
		case WeaveBow::Noise: // lowpassed turbulence
			return std::clamp(bowLP_ * 3.0f, -1.0f, 1.0f);
		case WeaveBow::Pulses: // alternating-sign raised-cosine taps
			if (p < 0.25f) {
				return 0.5f - 0.5f * dsp::portableCos(p * (2.0f * kPi / 0.25f));
			}
			if (p >= 0.5f && p < 0.75f) {
				return -(0.5f - 0.5f * dsp::portableCos((p - 0.5f) * (2.0f * kPi / 0.25f)));
			}
			return 0.0f;
		case WeaveBow::Walk: // Brownian pressure
			return bowWalk_;
		case WeaveBow::Triangle:
		default:
			return owlet::dsp::triangleSimpleUnipolar(p, 1.0f) * 2.0f - 1.0f;
		}
	};
	const float shapeA = bowShape(a.bowMode);
	const float shapeB = (b.bowMode == a.bowMode) ? shapeA : bowShape(b.bowMode);
	const float bowVal = (cfInv * shapeA + cf * shapeB) * effBowDepth_ * exc_.bowScale;

	applyPluck(cf);
	integrate(bowVal, morphBow);
	if (bridgeCount_ > 0) {
		applyBridge();
	}
	smooth();
	refreshPath();
	const float scale = updateAgc(scanPath());

	// Ring rotation under the scan head (half-dt rate)
	travelPhase_ += effTravelRate_ * 0.5f;
	if (travelPhase_ >= 1.0f) {
		travelPhase_ -= 1.0f;
	}
	else if (travelPhase_ < 0.0f) {
		travelPhase_ += 1.0f;
	}
	return scale;
}

// ============================================================================
// THE MODEL SEAM: what depends on what is vibrating is asked of weave_models.h
// (the node count, the windows, the elastic force, the scan path). Everything
// after refreshPath() — the AGC, the tables, the fades, the scan — sees only
// the kNodes-point path.
// ============================================================================

// Pluck: displace a raised-cosine bump into the string, in its own pass so the acceleration loop sees a
// consistent post-pluck shape
void WeaveString::applyPluck(float cf) {
	if (!pluckPending_) {
		return;
	}
	pluckPending_ = false;
	const PhiWeaveParams& a = a_;
	const PhiWeaveParams& b = b_;
	const float cfInv = 1.0f - cf;
	const float pluckPos = (pluckPosReq_ >= 0.0f) ? pluckPosReq_ : cfInv * a.pluckPos + cf * b.pluckPos;
	const float pluckWidth = cfInv * a.pluckWidth + cf * b.pluckWidth;
	const float pluckAmp = (pluckAmpReq_ >= 0.0f) ? pluckAmpReq_ : cfInv * a.pluckAmp + cf * b.pluckAmp;
	const float invPluckWidth = 1.0f / pluckWidth;
	float win[kMaxNodes];
	weave::windows(model_, pluckPos, invPluckWidth, win);
	const int n = weave::nodeCount(model_);
	for (int i = 0; i < n; ++i) {
		x_[i] += pluckAmp * win[i];
	}
}

// One half-dt force + integration step: spring terms and forcing x1/4, damping x1/2, then the velocity
// and displacement rails. The elastic forces are all computed from the positions before any of them
// moves (keeps neighbour reads consistent).
void WeaveString::integrate(float bowVal, float morphBow) {
	const int n = weave::nodeCount(model_);
	weave::elasticForce(model_, x_, eff_.gain, force_);
	for (int i = 0; i < n; ++i) {
		// Bow force: the precomputed balance-weighted window
		float bowForce = bowVal * effBowWin_[i];

		// Morph-bow: broadband agitation (cheap LCG noise)
		const float noise = lcgUnit(noiseState_);
		bowForce += noise * morphBow * 0.02f;

		const float accel = 0.25f * (force_[i] - eff_.k[i] * (x_[i] - eff_.home[i]) + bowForce) - 0.5f * eff_.d[i] * v_[i];
		const float vNew = v_[i] + accel;
		v_[i] = std::clamp(vNew, -kPhiWeaveMaxVelocity * 0.5f, kPhiWeaveMaxVelocity * 0.5f);
		vRailHits_ += (v_[i] != vNew) ? 1u : 0u;
	}
	for (int i = 0; i < n; ++i) {
		const float xNew = x_[i] + v_[i];
		x_[i] = std::clamp(xNew, -kPhiWeaveMaxDisplacement, kPhiWeaveMaxDisplacement);
		xRailHits_ += (x_[i] != xNew) ? 1u : 0u;
	}
}

// THE JAWARI BRIDGE (setBridge). A node's surface is its rest pose less its offset, so the bridge rides the
// rest pose as the laws move it: it is always on the far side of rest, which is what makes a contact unable to
// gain energy. The pose is the lazily solved one (restNodes()), at most kRestPoseMaxAgeTicks old while the laws
// move; a pose that moves under a node puts it back without a kick (bridgeContact).
void WeaveString::setBridge(const Bridge& b) {
	bridge_.jawari = std::isfinite(b.jawari) ? std::clamp(b.jawari, 0.0f, 1.0f) : 0.0f;
	bridge_.curve = std::isfinite(b.curve) ? std::clamp(b.curve, 0.0f, 1.0f) : 0.0f;
	bridge_.restitution = std::isfinite(b.restitution) ? std::clamp(b.restitution, 0.0f, 1.0f) : kBridgeRestitution;
	rebuildBridge();
}

void WeaveString::rebuildBridge() {
	if (!(bridge_.jawari > 0.0f)) {
		bridgeCount_ = 0;
		return;
	}
	// THE LINE (like a line over the string which jawari moves closer to the string): under EVERY
	// node, gap below its rest; Curve lifts it away from the string by up to kLineLift x the reach everywhere but under the
	// bow (a raised-cosine window kLineHalfWidth of the path wide), so at 0 the whole string meets a straight line and
	// toward 1 only the stretch under the bow does.
	const int n = weave::nodeCount(model_);
	float w[kMaxNodes];
	weave::windows(model_, effBowPos_, 1.0f / kLineHalfWidth, w);
	const float gap = 1.0f - kLineDepth * bridge_.jawari;
	const float lift = kLineLift * bridge_.curve;
	for (int i = 0; i < n; ++i) {
		bridgeNode_[i] = i;
		bridgeOffset_[i] = gap + lift * (1.0f - w[i]);
	}
	bridgeCount_ = n;
}

int WeaveString::lineNeighbours(int i, int* out) const {
	const int n = weave::nodeCount(model_);
	switch (weave::traits(model_).topology) {
		case weave::Topology::Loop:
			out[0] = (i + n - 1) % n;
			out[1] = (i + 1) % n;
			return 2;
		case weave::Topology::PinnedChain:
		case weave::Topology::FreeBar: {
			int m = 0;
			if (i > 0) {
				out[m++] = i - 1;
			}
			if (i + 1 < n) {
				out[m++] = i + 1;
			}
			return m;
		}
		case weave::Topology::Grid: {
			const int col = i % 8;
			const int row = i / 8;
			int m = 0;
			if (col > 0) {
				out[m++] = weave::membraneNode(col - 1, row);
			}
			if (col < 7) {
				out[m++] = weave::membraneNode(col + 1, row);
			}
			if (row > 0) {
				out[m++] = weave::membraneNode(col, row - 1);
			}
			if (row < 7) {
				out[m++] = weave::membraneNode(col, row + 1);
			}
			return m;
		}
	}
	return 0;
}

// A NODE PAST THE LINE ("it doesnt fold, it puts half of its energy which exceeds the line to each of
// its neighbor nodes"): it is set ON the line, its velocity into the line reversed at the restitution, and the excess d
// it was past by goes to its neighbours, d / m each (m = 2 along a string, 4 across the Membrane's grid, fewer at an end or
// a rim) — so the sum of the displacements is what it was. A neighbour pushed through in turn is taken on the next pass,
// kLinePasses at most; a last pass only sets anything still past onto the line (the sum is then not kept; counted).
// THE LINE'S HEIGHT IS THE STRING'S OWN SWING (lineRef_): the largest departure from rest over its nodes, held over
// kLineHoldSeconds, so Jawari means the same at any Bow or Drive — the comb strings' convention (resonate_ear.h).
// NEVER AMPLIFIES: in energyForTest's measure (departure from rest squared, plus velocity squared) the node set on the
// line loses drop = dev0^2 - dev1^2 >= 0 (the line is on the far side of rest), and the restitution only loses. The
// transfer changes its neighbours' share by -2 t s sum(a) + t^2 s^2 m (s = d / m, a their departures); it is scaled by
// the largest t <= 1 that keeps that change within the drop, so a contact never raises the measure (the budget cuts are
// counted: then the sum is not quite kept either).
void WeaveString::applyBridge() {
	const float* rest = restNodes();
	const int n = bridgeCount_;
	float swing = 0.0f;
	for (int i = 0; i < n; ++i) {
		swing = std::max(swing, std::abs(x_[i] - rest[i]));
	}
	lineRef_ = std::max(swing, lineRef_ * lineRelease_);
	const float ref = std::max(lineRef_, kLineFloor);
	double sum0 = 0.0;
	for (int i = 0; i < n; ++i) {
		sum0 += x_[i];
	}
	double gain = 0.0;
	bool touched = false;
	bool clean = true;
	for (int pass = 0; pass <= kLinePasses; ++pass) {
		const bool transfer = pass < kLinePasses;
		bool any = false;
		for (int i = 0; i < n; ++i) {
			const float surface = rest[i] - ref * bridgeOffset_[i];
			if (!(x_[i] < surface)) {
				continue;
			}
			any = true;
			touched = true;
			++bridgeContacts_;
			contactEnv_[i] = 1.0f;
			const float d = surface - x_[i];
			const double dev0 = static_cast<double>(x_[i]) - rest[i];
			const double dev1 = static_cast<double>(surface) - rest[i];
			x_[i] = surface;
			const float v0 = v_[i];
			if (v_[i] < 0.0f) {
				v_[i] = -bridge_.restitution * v_[i];
			}
			const double drop = dev0 * dev0 - dev1 * dev1;
			gain += (static_cast<double>(v_[i]) * v_[i] - static_cast<double>(v0) * v0) - drop;
			int nb[4];
			const int m = transfer ? lineNeighbours(i, nb) : 0;
			if (m == 0) {
				clean = false; // set on the line without passing its excess on
				continue;
			}
			const double s = static_cast<double>(d) / m;
			double sumA = 0.0;
			for (int k = 0; k < m; ++k) {
				sumA += static_cast<double>(x_[nb[k]]) - rest[nb[k]];
			}
			// THE LARGEST t <= 1 WITH  s^2 m t^2 - 2 s sumA t <= drop (a hair inside it, for the float the nodes are).
			const double qa = s * s * m;
			const double qb = -2.0 * s * sumA;
			const double budget = drop * (1.0 - 1e-6);
			double t = 1.0;
			if (qa + qb > budget) {
				t = qa > 0.0 ? std::clamp((-qb + std::sqrt(std::max(0.0, qb * qb + 4.0 * qa * budget))) / (2.0 * qa), 0.0, 1.0) : 0.0;
				++lineBudgetBound_;
				clean = false;
			}
			for (int k = 0; k < m; ++k) {
				const float pushed = x_[nb[k]] - static_cast<float>(t * s);
				// THE DISPLACEMENT RAIL, as integrate() keeps it: a neighbour pushed past it stops there (the sum not kept).
				x_[nb[k]] = std::max(pushed, -kPhiWeaveMaxDisplacement);
				if (x_[nb[k]] != pushed) {
					clean = false;
					++xRailHits_;
				}
			}
			gain += qa * t * t + qb * t;
		}
		if (!any) {
			break;
		}
	}
	if (touched) {
		bridgeMaxGain_ = std::max(bridgeMaxGain_, static_cast<float>(gain));
		++lineContactSteps_;
		if (clean) {
			double sum1 = 0.0;
			for (int i = 0; i < n; ++i) {
				sum1 += x_[i];
			}
			++lineCleanSteps_;
			lineSumDriftMax_ = std::max(lineSumDriftMax_, std::abs(sum1 - sum0));
		}
	}
}

float WeaveString::bridgeSurfaceForTest(int node) const {
	const float* rest = restNodes();
	for (int j = 0; j < bridgeCount_; ++j) {
		if (bridgeNode_[j] == node) {
			return rest[node] - std::max(lineRef_, kLineFloor) * bridgeOffset_[j];
		}
	}
	return -std::numeric_limits<float>::infinity();
}

// Scan smoother, two-pole (12 dB/oct), per node: the physics can run hot, but the scan head follows at
// haptic rates - fast modes shape the energy, not the sidebands. One pole leaked underdamped ~50 Hz
// modes nearly intact.
void WeaveString::smooth() {
	const int n = weave::nodeCount(model_);
	for (int i = 0; i < n; ++i) {
		xs1_[i] += effShimmer_ * (x_[i] - xs1_[i]);
		xs_[i] += effShimmer_ * (xs1_[i] - xs_[i]);
	}
	if (bridgeCount_ > 0) {
		letBuzzThrough();
	}
}

// LET THE BUZZ THROUGH : on the raw nodes a contact moved the energy above mode 4 up by x3.6, but the
// smoother the scan reads through kept most of it back (x1.1-1.27). So where and while the string is in contact the
// smoother is LIGHTENED: a bridge node's pole moves from the laws' shimmer toward 1 (no smoothing) by a lift of
// kBuzzThrough x Jawari x its contact envelope, which is 1 at a contact and relaxes by kContactRelax a sub-step. Applied after the
// ordinary pass, as the extra step the lighter pole takes, so a string with no bridge (Jawari 0) never comes here.
void WeaveString::letBuzzThrough() {
	for (int j = 0; j < bridgeCount_; ++j) {
		if (contactEnv_[j] <= 0.0f) {
			continue;
		}
		const int i = bridgeNode_[j];
		const float lift = kBuzzThrough * bridge_.jawari * contactEnv_[j];
		// THE POLE, LIGHTENED: an extra step of (1 - shimmer) x lift toward the raw node from where the ordinary pass left
		// each pole, so its step is 1 - (1 - shimmer)(1 - (1 - shimmer) lift) in all: the shimmer at lift 0, near 1 at 1.
		const float extra = (1.0f - effShimmer_) * lift;
		xs1_[i] += extra * (x_[i] - xs1_[i]);
		xs_[i] += extra * (xs1_[i] - xs_[i]);
		contactEnv_[j] *= kContactRelax;
		if (contactEnv_[j] < 1e-3f) {
			contactEnv_[j] = 0.0f;
		}
	}
}

// The path the scan reads, from the smoothed nodes. The Ring and Stiffening ARE their path (scanPath()
// returns xs_ itself), so nothing is copied for them.
void WeaveString::refreshPath() {
	if (!weave::pathIsNodes(model_)) {
		weave::scanPath(model_, xs_, path_);
	}
}

// ============================================================================
// Model-independent: everything below reads a kNodes-point scan path.
// ============================================================================

float WeaveString::agcTarget(float peak, float rms, float outGain) {
	const float scaleByRms = 0.55f * kRefAmplitude / std::max(rms, 0.20f);
	const float scaleByPeak = kRefAmplitude / std::max(peak, 0.35f);
	return outGain * std::min(scaleByRms, scaleByPeak);
}

// Slow AGC: quiet laws (soft home shapes, heavy damping) and violent ones land at comparable loudness.
// Attack instant, release ~1.5 s of sub-ticks. RMS-targeting (saw-class energy, 0.55 of the reference)
// with the PEAK target at the reference. Returns the (slewed) scale.
float WeaveString::updateAgc(const float* path) {
	float peak = 0.0f;
	float sumSq = 0.0f;
	for (int i = 0; i < kN; ++i) {
		peak = std::max(peak, std::abs(path[i]));
		sumSq += path[i] * path[i];
	}
	agcPeak_ = std::max(peak, agcPeak_ * 0.999f); // sqrt(0.998) per half-dt step
	const float rms = std::sqrt(sumSq * (1.0f / static_cast<float>(kN)));
	agcRms_ = std::max(rms, agcRms_ * 0.999f);
	const float scaleTarget = agcTarget(agcPeak_, agcRms_, effOutGain_);
	// Slewed: an instant-attack scale stepped the level per sub-tick (AM hash); ~15 ms of slew still
	// catches swells before the table clamp does the hard limiting.
	agcScale_ += 0.10f * (scaleTarget - agcScale_);
	return agcScale_;
}

void WeaveString::buildTables(const float* path, float scale, int set) {
	fillTables(tables_[set], path, scale);
}

// The rest pose's scan path through the very same fill (sampleMotion reads the difference).
void WeaveString::buildRestTables(float scale, int set) {
	const float* rest = restNodes();
	if (!weave::pathIsNodes(model_)) {
		weave::scanPath(model_, rest, restPath_);
		rest = restPath_;
	}
	fillTables(rest_[set], rest, scale);
}

// The motion tables: the scan less the rest, each point eased from set `from` toward it by motionSmooth_ (from == set: seated).
void WeaveString::buildMotionTables(int set, int from) {
	for (int m = 0; m < kMips; ++m) {
		for (int i = 0; i <= kN; ++i) {
			const float target = tables_[set][m][i] - rest_[set][m][i];
			const float was = motion_[from][m][i];
			float now = (from == set) ? target : was + motionSmooth_ * (target - was);
			if (std::abs(now) < 1e-20f) {
				now = 0.0f; // EXACTLY 0 AT REST, and no denormals on the way
			}
			motion_[set][m][i] = now;
		}
	}
}

void WeaveString::fillTables(Table* t, const float* path, float scale) {
	for (int i = 0; i < kN; ++i) {
		t[0][i] = std::clamp(path[i] * scale, -kTableCeiling, kTableCeiling);
	}
	t[0][kN] = t[0][0];
	// Anti-aliasing mips: circular 3-tap binomial smoothing. One pass nulls the spatial Nyquist
	// (adjacent-node zigzag) entirely; the second clears the top octave for the highest notes. Circular
	// because the scan reads the path as one cycle, whatever the model's own topology.
	for (int m = 1; m < kMips; ++m) {
		const float* src = t[m - 1];
		for (int i = 0; i < kN; ++i) {
			t[m][i] = 0.25f * (src[(i + kN - 1) & (kN - 1)] + 2.0f * src[i] + src[(i + 1) & (kN - 1)]);
		}
		t[m][kN] = t[m][0];
	}
}

// At a boundary: two physics sub-ticks building the mid and cur table sets; the window that follows
// fades prev -> mid -> cur.
void WeaveString::tick() {
	// A model switch lands here, at a boundary: the new model's nodes are seated at its rest pose and the
	// AGC seated on it, while the tables, travel and the clock carry on — so the FIRST HALF of the window
	// that follows fades from the old model's last tables (prev, at the old scale) to the new model's
	// first (mid), and the second half runs on the new model alone (for tickLength() >= 3: see setModel).
	if (pendingModel_ != model_) {
		model_ = pendingModel_;
		rebuildEff(cf_);
		effCfCached_ = cf_;
		seatAtRest();
		seatAgc();
	}
	// Effective-law cache: rebuild only when the crossfade moves meaningfully (epsilon ~0.0005 also cuts
	// off a smoother's asymptotic tail)
	if (std::abs(cf_ - effCfCached_) > 0.0005f) {
		rebuildEff(cf_);
		effCfCached_ = cf_;
	}
	travelPrev_ = travelPhase_;
	std::memcpy(tables_[kPrev], tables_[kCur], sizeof(tables_[kCur]));
	if (restTables_) {
		std::memcpy(rest_[kPrev], rest_[kCur], sizeof(rest_[kCur]));
		std::memcpy(motion_[kPrev], motion_[kCur], sizeof(motion_[kCur]));
	}
	lastMorphBow_ = 0.f; // summed over this tick's two sub-ticks (lastMorphBowForTest)
	const float scaleMid = step(cf_);
	buildTables(scanPath(), scaleMid, kMid);
	const float scaleCur = step(cf_);
	buildTables(scanPath(), scaleCur, kCur);
	if (restTables_) {
		buildRestTables(scaleMid, kMid);
		buildRestTables(scaleCur, kCur);
		buildMotionTables(kMid, kPrev);
		buildMotionTables(kCur, kMid);
	}
	++ticks_;
}

void WeaveString::advance(int n) {
	while (n > 0) {
		const int m = std::min(n, tickLength_ - windowPos_);
		windowPos_ += m;
		n -= m;
		if (windowPos_ >= tickLength_) {
			windowPos_ = 0;
			tick();
			// A SPEED CHANGE LANDS HERE, between windows: the window that follows is the new length. The
			// tables tick() just built do not depend on it, only the fade's positions across the window do.
			if (pendingTickLength_ != tickLength_) {
				applyTickLength(pendingTickLength_);
			}
		}
	}
}

WeaveString::Fade WeaveString::fadeAt(int offset) const {
	const int k = std::min(windowPos_ + std::max(offset, 0), tickLength_ - 1);
	Fade f;
	// The firmware's forward-difference fade reaches its segment's end ON the segment's last sample:
	// sample k of a segment of length len reads smoothstep((k + 1) / len) — tabulated in prepare().
	f.segment = (k < (tickLength_ >> 1)) ? 0 : 1;
	f.t = fadeT_[k];
	f.travel = travelPhaseAt(offset);
	return f;
}

float WeaveString::sample(double phase, int mip, int offset) const {
	return sample(fadeAt(offset), phase, mip);
}

float WeaveString::travelPhaseAt(int offset) const {
	const int k = std::min(windowPos_ + std::max(offset, 0), tickLength_ - 1);
	const float delta = wrapHalf(travelPhase_ - travelPrev_);
	float p = travelPrev_ + delta * travelFrac_[k];
	p -= std::floor(p);
	return p;
}

int WeaveString::mipForFrequency(double hz) {
	return stairMipForFrequency(hz);
}

float WeaveString::energyForTest() const {
	float rest[kMaxNodes];
	weave::RestPoseScratch scratch; // a test seam, and const: its own
	weave::restPose(model_, eff_, scratch, rest);
	const int n = weave::nodeCount(model_);
	float e = 0.f;
	for (int i = 0; i < n; ++i) {
		const float d = x_[i] - rest[i];
		e += d * d + v_[i] * v_[i];
	}
	return e;
}

} // namespace feathers
