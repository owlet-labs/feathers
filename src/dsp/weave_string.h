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

#include "character/phi_weave.hpp"
#include "dsp/weave_models.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// THE WEAVE RING — the stateful half of PhiWeave: 32 masses on springs obeying the laws of
// character/phi_weave.hpp, simulated at control rate and scanned at audio rate by any number of
// readers. A float port of the firmware's PhiWeaveCache, tickPhiWeave and the scan half of
// renderPhiWeave (reference firmware 2567b04c, dsp/phi_weave.{hpp,cpp}).
//
// WHAT VIBRATES IS A CHOICE (setModel, weave_models.h): the firmware's Ring by default, or a Bar, a
// Tube, a Membrane, or the Ring with Stiffening springs. Every model hands the scan the same 32-point
// path, so the tables, fades, loudness control and scan below are shared; the Ring's output is pinned
// bit-for-bit to the code before the switch existed (test_weave_ring_pin).
//
// WHAT DIFFERS FROM THE FIRMWARE, AND WHY:
//
// - THE TICK CLOCK IS THE RING'S OWN. The firmware ticks once per render buffer — 128 samples on the
//   hardware, ≈344.5 Hz — guarded by AudioEngine::audioSampleTimer, which the plugin never reads. A tick
//   per HOST block would make the string's modes, decay and bow rates follow the DAW's buffer size, so
//   the ring ticks every tickLength() = round(sampleRate / kTickHz) samples, counted by its own clock
//   (the position in the current window, windowPos_, plus the ticks run, ticks_) and cut out of whatever
//   blocks arrive. Output is identical at any block size (tested).
// - FLOAT END TO END, on the plugin's q31-rail = ±1.0 scale (as Morph and Stair): the firmware's
//   kPhiWeaveRefAmplitude (2^30, square parity) is 0.5 here and its 2^31 table clamp is the ±1.0
//   ceiling; its 0.55 RMS target and 0.20 / 0.35 floors are unchanged. See the .cpp for why the
//   ceiling is not at the reference.
// - TABLES ARE VALID FROM reset(): the ring starts AT REST, in its restPose() (the firmware's started
//   flat, at zero, and spent its first second settling), and reset() builds all three table sets from
//   it. The firmware's "first tick: nothing to fade from" copy existed because its first tables were
//   zeros; here the first tick fades from the rest pose instead, so there is nothing to step from.
// - AN EXCITATION SEAM: a bow-force scale, held until changed, and pluck requests with an amplitude
//   and position. The synth feeds the firmware's own bow and a pluck per note; the WEAVE effect feeds
//   it audio. Every setter sanitises what it is given (a host drives bowScale from audio, and one inf
//   would otherwise leave the string NaN until reset()).
//
// HOW A READER USES IT. Ticks happen at multiples of tickLength() samples since reset(); tick w runs at
// the boundary and the window of samples that follows fades prev -> mid over its first half and
// mid -> cur over its second (smoothstep, so node motion is C1 at every join). A caller cuts its block
// at samplesUntilTick(), fills fade[k] = fadeAt(k) for k = 0..m-1 of an m-sample chunk ONCE, has every
// voice and head read sample(fade[k], phase, mip), then calls advance(m) — which runs the tick if the
// chunk ended on a boundary. Offsets are in HOST samples: a 4x oversampled voice reads fade[j / 4].
//
// THREADING. Every mutator (prepare aside) runs on the audio thread, BETWEEN chunks: setLaws copies two
// PhiWeaveParams and is not atomic against a reader, setModel/setCrossfade/setExcitation/pluck are
// read at the next tick. The const readers (fadeAt, sample, travelPhaseAt, smoothedNodes) may be called
// by any number of voices within a chunk, on that same thread. motionEnergy() and restNodes() are const but
// fill lazy caches (a sum, and a rest-pose solve), so they too belong to that thread alone.
//
// No allocation anywhere: every table, and the rest-pose solve's scratch, is a member array.

namespace feathers {

class WeaveString {
public:
	/// Points on the scan path, and so entries in every table: 32 whatever the model.
	static constexpr int kNodes = kWeavePathPoints;
	/// The most physics nodes any model has (the Membrane's 8x8).
	static constexpr int kMaxNodes = kWeaveMaxNodes;
	/// The firmware's per-buffer rate: 128 samples at the hardware's 44100 Hz.
	static constexpr double kTickHz = 44100.0 / 128.0;
	static constexpr int kMips = 3;

	/// Sizes the tick (max(1, round(sampleRate / kTickHz))) and resets. If no laws have been set yet,
	/// installs the laws at phase 0 on both sides first, so the ring is never lawless.
	void prepare(double sampleRate);

	/// The string at rest in its model's rest pose (at the current crossfade), velocities zero, the
	/// clock at zero, the AGC settled on it, all three table sets built from it, and a pluck pending (the
	/// firmware's pluckPending starts true). The pluck lands at the first tick; replace it with pluck(0)
	/// to start silent-still. DELIBERATELY UNLIKE THE FIRMWARE, whose string started at zero and grew
	/// into its pose — see the .cpp.
	void reset();

	/// Copies both sides' laws; the effective (crossfaded) laws are rebuilt at the next tick.
	void setLaws(const owlet::dsp::PhiWeaveParams& a, const owlet::dsp::PhiWeaveParams& b);

	/// 0..1 (clamped), read at each tick. Its MOTION bows the string (the firmware's morph-bow). A
	/// non-finite value is ignored — the crossfade holds where it was, since jumping to a default would
	/// itself be crossfade motion and bow the string.
	void setCrossfade(float cf);
	/// MOVES THE CROSSFADE WITHOUT BOWING: as setCrossfade, and the last tick's crossfade is re-seated to it
	/// too, so the next tick sees no motion. For a reader whose crossfade moved while the string was not
	/// ticking (the synth, off Resonate): that move happened while nothing was vibrating, and a resumed
	/// string must not be jolted by the whole of it at its first tick.
	void reseatCrossfade(float cf);
	/// The crossfade as last set.
	float crossfade() const { return cf_; }

	/// The physics model (weave_models.h). A STEPPED CHOICE, NOT A MODULATABLE ONE: a switch DISCARDS
	/// THE STRING'S MOTION. It takes effect at the next tick, where the new model's nodes are seated at its
	/// rest pose with every velocity zero (a ringing pluck or a bowed swell stops there; the bow, which
	/// carries on, builds the new model's motion up again from rest) and the loudness control is seated
	/// on it (as reset() does). The tables, travel and the clock carry on: the FIRST HALF of the window
	/// after the switch fades from the old model's last tables to the new model's first (smoothstep,
	/// ~1.5 ms at 44.1 kHz), so a switch is a half-window crossfade, not a step — for tickLength() >= 3.
	/// At 1 or 2 samples a window (a sample rate under ~860 Hz) the first half has no sample that reads
	/// the old tables, and a switch is a step. Before prepare() it simply selects the model.
	void setModel(WeaveModel m) { pendingModel_ = m; }
	WeaveModel model() const { return model_; }
	int nodeCount() const { return weave::nodeCount(model_); }

	struct Excitation {
		/// Scales the laws' own bow force (which is balance-weighted per node: see bowBalance). 0 = the
		/// laws never bow. Clamped to [0, kMaxBowScale]; a non-finite value becomes the default, 1.
		float bowScale = 1.f;
	};
	static constexpr float kMaxBowScale = 16.f;
	/// Held until changed; applied in every sub-tick.
	void setExcitation(const Excitation& e);

	/// Queues a pluck for the next sub-tick: a raised-cosine displacement bump at path position `pos`,
	/// placed by the model's own geometry (weave::window). A negative or non-finite amp or pos means
	/// "the laws' own" (crossfaded at the tick); amp is clamped to [0, kMaxPluckAmp] and pos wrapped into
	/// [0, 1). Width is always the laws'. A second request before that sub-tick replaces the first —
	/// plucks do not stack.
	void pluck(float amp = -1.f, float pos = -1.f);
	static constexpr float kMaxPluckAmp = 2.f;

	/// THE JAWARI BRIDGE — v9, the LINE (2026-09-28: "in bar mode, like a line over the string which jawari moves closer
	/// to the string. when the string impacts the line, it doesnt fold, it puts half of its energy which exceeds the line to
	/// each of its neighbor nodes"). A one-sided barrier under EVERY node, below its rest
	/// pose by (1 - kLineDepth x Jawari) of the string's own swing (kLineDepth). CURVE lifts the line away by up to kLineLift
	/// of that swing everywhere but under the bow (a raised-cosine window kLineHalfWidth of the path wide, centred on the
	/// laws' crossfaded bow position) — 0 a straight line the whole string meets, 1 a bridge only the stretch under the bow
	/// reaches (where the string moves most: MEASURED 2026-09-27, bow x2, the Ring's RMS departure from rest is 10-30x
	/// larger at the bow than across it). A node past the line is set on it, its velocity into the line reversed at
	/// Bridge::restitution (Resonate's Bounce), and the excess it was past by goes to its neighbours in equal shares
	/// (applyBridge in the .cpp: the neighbours, the passes, and why energy is never gained). The kinks this puts in the
	/// string are short spatial modes, which the scan hears as brighter partials; they come and go as the wander swings.
	/// Jawari 0 is NO BRIDGE and changes no bit (test_weave_ring_pin). All read at the next sub-tick; a non-finite value
	/// reads as 0; all clamped to [0, 1].
	struct Bridge {
		float jawari = 0.f; ///< how close the line sits: 0 none, 1 at its closest (kLineDepth)
		float curve = 0.5f; ///< 0 a straight line under the whole string .. 1 lifted away everywhere but under the bow
		/// how much of a node's speed into the bridge comes back off it: 1 elastic, 0 a dead stop (Resonate's Bounce);
		/// clamped to [0, 1], non-finite reads as kBridgeRestitution
		float restitution = kBridgeRestitution;
	};
	void setBridge(const Bridge& b);
	/// THE DEFAULT RESTITUTION, for a caller that does not set one: 80% of the speed comes back off the bridge. MEASURED
	/// 2026-09-27 against 0.5 on the point bridge (Ring, bow x2, Jawari 0.8, 8 phases): more of the bounce comes back as
	/// ripple rather than being lost.
	static constexpr float kBridgeRestitution = 0.8f;
	/// The gap at the lowest Jawari, in displacement units: about the wander's swing under the bow (MEASURED
	/// 2026-09-27, the Ring's peak departure from rest near its bow at bow x2..x8 is 0.03..0.09).
	static constexpr float kBridgeReach = 0.03f;
	/// THE LINE'S HEIGHT: (1 - kLineDepth x Jawari) of the string's OWN SWING below rest — its largest departure from rest
	/// over its nodes, held over kLineHoldSeconds (never under kLineFloor) — so Jawari means the same at any Bow or Drive
	/// (the comb strings' convention). MEASURED 2026-09-28: a line at a FIXED height brought right up to
	/// the resting string pressed it flat and the share above spatial mode 4 FELL past Jawari ~0.6, and a quiet string never
	/// reached it; against the swing, at depth 0.9, the share rises at every step (test_weave_jawari).
	static constexpr float kLineDepth = 0.9f;
	static constexpr double kLineHoldSeconds = 2.0;
	static constexpr float kLineFloor = 0.005f;
	/// CURVE'S LIFT at 1, as a fraction of the swing, and the half-width (in path units) of the stretch under the bow
	/// it leaves where it was. By ear (unauditioned).
	static constexpr float kLineLift = 1.0f;
	static constexpr float kLineHalfWidth = 0.25f;
	/// THE PASSES a contact's excess may travel: a neighbour pushed through is taken on the next, then a last pass only
	/// sets what is still past onto the line.
	static constexpr int kLinePasses = 3;
	/// LETTING THE BUZZ THROUGH (letBuzzThrough in the .cpp): at Jawari 1 and a fresh contact a bridge node's smoothing
	/// pole moves this far from the laws' shimmer toward 1; the contact envelope relaxes by kContactRelax a sub-step
	/// (half-life about 7 sub-steps, ~10 ms). By ear (unauditioned); the brightening it buys is measured in
	/// test_weave_jawari.
	static constexpr float kBuzzThrough = 0.8f;
	static constexpr float kContactRelax = 0.9f;
	/// THE CONTACT LAW, one node against a surface under it: past it, the node is put on it and a velocity into
	/// it is reflected at `restitution`; a node already moving away (the surface rose under it) is only put
	/// back. Returns whether it touched.
	static bool bridgeContact(float& x, float& v, float surface, float restitution = kBridgeRestitution) {
		if (!(x < surface)) {
			return false;
		}
		x = surface;
		if (v < 0.f) {
			v = -restitution * v;
		}
		return true;
	}

	/// THE TICK RATE AS A MULTIPLE OF kTickHz (the synth's Resonate Speed). The firmware
	/// ticked once per RENDER WINDOW, whose length the reference's audio routine varies under 128 samples, so on
	/// the hardware the string ran faster than kTickHz — and everything it does per tick (its modes, decay,
	/// bow, travel, smoothing, loudness control) ran faster with it. Speed restores that on a FIXED clock:
	/// tickLength() = round(sampleRate / (kTickHz x speed)), so the output stays identical at any block size.
	/// The rate moves in whole-sample windows (128 / 64 / 16 at 44.1 kHz for x1 / x2 / x8: steps of one sample
	/// in the window, about 6% at x8). Clamped to [kMinSpeed, kMaxSpeed]; a non-finite value is ignored. Taken
	/// at the next tick boundary, where the window that follows is the new length — nothing jumps, the string
	/// only starts moving at the new rate. The default, 1, is the tick before Speed existed, to the bit.
	void setSpeed(double speed);
	double speed() const { return speed_; }
	static constexpr double kMinSpeed = 1.0;
	static constexpr double kMaxSpeed = 8.0;
	/// round(sampleRate / (kTickHz x speed)), clamped to [1, kMaxTickLength] — the window a speed asks for.
	static int tickLengthFor(double sampleRate, double speed);

	int tickLength() const { return tickLength_; }
	/// 1..tickLength(): samples left in the current window.
	int samplesUntilTick() const { return tickLength_ - windowPos_; }
	/// Moves the clock n samples, running a tick at every boundary reached (n may span several).
	void advance(int n);

	/// Where one sample of the current chunk sits in the table fade: which segment (0 = prev -> mid,
	/// 1 = mid -> cur), the smoothstep position in it, and the travel phase. It depends only on the clock,
	/// so a caller with many readers fills ONE array of these per chunk (fadeAt(k), k = 0..m-1) and every
	/// voice or head reuses it, rather than each recomputing the smoothstep and the travel.
	struct Fade {
		uint8_t segment = 0;
		float t = 0.f;
		float travel = 0.f;
	};
	/// `offset` HOST samples into the current chunk (0 <= offset < samplesUntilTick(); clamped).
	Fade fadeAt(int offset) const;

	/// The scan: the path at `phase` (cycles; wrapped; a non-finite phase reads phase 0) on mip 0..2, at
	/// fade position `f`. Linear between path points, as the firmware's final scan is. Travel is NOT added
	/// here — a reader adds f.travel to its phase. THIS is the reader's API.
	float sample(const Fade& f, double phase, int mip) const;
	/// sample(fadeAt(offset), phase, mip), `offset` in HOST samples — a convenience for a single reader
	/// (and the tests); a caller with several readers per chunk fills the Fade array once instead.
	float sample(double phase, int mip, int offset) const;
	/// THE STRING'S MOTION AT THE SAME POINT (Resonate v15): the scan less the scan of the string's rest pose, whose tables are
	/// built beside the scan's at each sub-tick with the same scale, clamp and mips and read with the same interpolation, so
	/// exactly 0 at rest — eased across sub-ticks by a one-pole of coefficient `a` (1: none). Only while setRestTables(true)
	/// (a rest-pose solve a tick, which a reader of motionEnergy() pays anyway); 0 otherwise.
	void setRestTables(bool on) { restTables_ = on; }
	float sampleMotion(const Fade& f, double phase, int mip) const;
	void setMotionSmoothing(float a) { motionSmooth_ = std::clamp(a, 0.0f, 1.0f); }

	/// The path's rotation under the scan head, advanced continuously across the window from the tick's
	/// start offset to its end offset (a SIGNED delta — firmware :477-482), `offset` in HOST samples.
	float travelPhaseAt(int offset) const;

	/// 700 / 1800 Hz: the thresholds Stair shares (stairMipForFrequency).
	static int mipForFrequency(double hz);

	/// THE REST POSE of the RING: where it comes to rest under these laws with no bow — NOT the home
	/// shape. Each node is pulled toward home by its stiffness AND toward its neighbours by its coupling,
	/// so the string settles where k(x - home) = c(x[i-1] + x[i+1] - 2x[i]): the home shape low-passed
	/// round the ring, more so where coupling dominates stiffness. Solved exactly (a 32x32 diagonally
	/// dominant system). For every model, weave::restPose; for a picture, restScanPath below. All three
	/// statics (these two and restScanPath) put the solve's 33 KB scratch on the caller's stack: for the
	/// message thread, not the audio thread.
	static void restPose(const float* stiffness, const float* coupling, const float* home, float* out);
	static void restPose(const owlet::dsp::PhiWeaveParams& laws, float* out);
	/// What the scan reads from a parked, unbowed string of model `m` under `laws`: its rest pose along
	/// its scan path (kNodes points, before the loudness control). What a picture of "what this
	/// coordinate selects" should draw.
	static void restScanPath(WeaveModel m, const owlet::dsp::PhiWeaveParams& laws, float* out);

	/// The two-pole-smoothed scan path the tables are built from (before the AGC scale), kNodes points.
	/// For the Ring these are the smoothed nodes themselves. Public because the WEAVE effect filters its
	/// input through this shape.
	const float* smoothedNodes() const { return scanPath(); }

	// --- test seams ---
	/// The raw nodes, nodeCount() of them.
	const float* nodesForTest() const { return x_; }
	const float* velocitiesForTest() const { return v_; }
	/// The model's mapped per-node stiffness and stencil gain (nodeCount() each).
	const float* nodeStiffnessForTest() const { return eff_.k; }
	const float* nodeGainForTest() const { return eff_.gain; }
	const float* nodeHomeForTest() const { return eff_.home; }
	/// The laws as last handed in (side A, side B) and the excitation's bow scale, as values. (Added 2026-09-26
	/// for the synth's Resonate shaping cases.)
	const owlet::dsp::PhiWeaveParams& lawsAForTest() const { return a_; }
	const owlet::dsp::PhiWeaveParams& lawsBForTest() const { return b_; }
	float bowScaleForTest() const { return exc_.bowScale; }
	const Bridge& bridgeForTest() const { return bridge_; }
	const float* nodeBowBalanceForTest() const { return eff_.bowBal; }
	const float* nodeDampingForTest() const { return eff_.d; }
	/// The smoothed NODES (nodeCount()), which the scan path is read from — for the Ring the same array
	/// as smoothedNodes().
	const float* smoothedModelNodesForTest() const { return xs_; }
	/// The smoother's first pole (nodeCount()), so a test can restate the second from it.
	const float* smoothedFirstPoleForTest() const { return xs1_; }
	/// Moves one node by dx, now, as a pluck at a point the path never names would (the Membrane's
	/// centre).
	void displaceForTest(int node, float dx) { x_[node] += dx; }
	/// sum over nodes of (x - rest)^2 + v^2, rest being the model's rest pose under the effective
	/// (crossfaded) laws. About the rest pose rather than about home: a ring parked at its equilibrium
	/// sits about 2.0 "about home" (measured, phase 0.3) and never decays below it.
	float energyForTest() const;
	/// THE SAME QUANTITY, KEPT: sum over nodes of (x - rest)^2 + v^2 as of the last tick. 0 at reset.
	/// What a front end reads to know whether the string is moving at all — Resonate gates its tone by it.
	/// LAZY: worked out at the first ask after a tick (a sum), so a front end that never asks — the synth —
	/// never pays for it; and the rest pose it is measured about is re-solved lazily too (restNodes()).
	float motionEnergy() const;
	/// THE STRING'S DEPARTURE FROM REST UNDER THE BOW (v10): the bow window's weighted mean of x - rest over the nodes.
	/// Negative is TOWARD THE BRIDGE, which lies below rest (Bridge) — the orbit's excursion Resonate's voice strings read.
	float bowDeparture() const;
	/// THE STATE IN THE MODEL'S OWN NODE LAYOUT (nodeCount(): the Membrane 8x8 row-major, the rest 32), for
	/// a picture: the smoothed nodes, and the rest pose they settle toward under the current laws.
	const float* modelNodes() const { return xs_; }
	/// THE REST POSE, SOLVED ON ASK. A solve is a dense one (up to ~30 us into the Membrane), so it is never
	/// made where the laws are rebuilt — a moving crossfade rebuilds them every tick — only when this or
	/// motionEnergy() is asked: at once if the laws have stopped moving or the model has just changed, and
	/// while the laws keep moving, at most once every kRestPoseMaxAgeTicks ticks. So the pose a reader sees
	/// is exact whenever the laws are still, and at most that many ticks old while they move.
	const float* restNodes() const;
	/// 32 ticks, ~93 ms. Why a front end's gate tolerates a pose this old is in weave_string.cpp (rest-pose
	/// solve); the cost it buys back is in bench_weave_models.cpp.
	static constexpr uint64_t kRestPoseMaxAgeTicks = 32;
	/// Rest-pose solves made for motionEnergy()/restNodes() since construction (a reset or a model switch
	/// seats the string at a pose it solves anyway, and that one is not counted: it is not on ask).
	uint64_t restPoseSolvesForTest() const { return restSolves_; }
	uint64_t ticksForTest() const { return ticks_; }
	/// The morph-bow force the last TICK applied, summed over its two sub-ticks (|cf motion| x the laws'
	/// gain).
	float lastMorphBowForTest() const { return lastMorphBow_; }
	/// Node-sub-steps at which the displacement / velocity rail engaged since reset(): the clamp changed
	/// the value. Counted, not acted on — the rails are a safety net and this says how often it caught.
	uint64_t displacementRailHitsForTest() const { return xRailHits_; }
	uint64_t velocityRailHitsForTest() const { return vRailHits_; }
	/// Node-sub-steps at which a node touched the bridge since reset().
	uint64_t bridgeContactsForTest() const { return bridgeContacts_; }
	/// Contacts whose transfer to the neighbours the energy budget cut short (applyBridge).
	uint64_t lineBudgetBoundForTest() const { return lineBudgetBound_; }
	/// Sub-steps with a contact; those whose every excess went on to the neighbours in full (no budget cut, no last-pass
	/// set); and over those, the largest change in the sum of the displacements (0 but for float).
	uint64_t lineContactStepsForTest() const { return lineContactSteps_; }
	uint64_t lineCleanStepsForTest() const { return lineCleanSteps_; }
	double lineSumDriftForTest() const { return lineSumDriftMax_; }
	/// TEST SEAMS for the line's contact law: a node set by hand, then the contact pass alone, at the state as it is.
	void setNodeForTest(int i, float x) { x_[i] = x; }
	void setVelocityForTest(int i, float v) { v_[i] = v; }
	void applyBridgeForTest() {
		if (bridgeCount_ > 0) {
			applyBridge();
		}
	}
	/// The largest change in energy (about the rest pose, over the touched nodes) any one sub-step's contacts
	/// made since reset(); lowest() until the first contact. Never above 0 by construction (setBridge).
	float bridgeMaxEnergyGainForTest() const { return bridgeMaxGain_; }
	/// The bridge's surface under a node, against the rest pose as restNodes() now reads it; -inf for a node the
	/// bridge does not reach, or with no bridge.
	float bridgeSurfaceForTest(int node) const;
	/// The AGC scale the current (cur) tables were built with.
	float scaleForTest() const { return agcScale_; }
	/// A cur-table entry, path point 0..kNodes-1, mip 0..2.
	float tableForTest(int mip, int node) const { return tables_[kCur][mip][node]; }
	/// An entry of any table set: 0 = prev, 1 = mid, 2 = cur.
	float tableSetForTest(int set, int mip, int node) const { return tables_[set][mip][node]; }
	/// The travel phase after the last sub-step (what the window's last sample reaches).
	float travelPhaseForTest() const { return travelPhase_; }
	float travelRateForTest() const { return effTravelRate_; }

	/// The longest tick window the fade tables hold: 4096 samples, a tick at 1.4 MHz.
	static constexpr int kMaxTickLength = 4096;

private:
	static constexpr int kPrev = 0;
	static constexpr int kMid = 1;
	static constexpr int kCur = 2;
	/// The loudness rule's target scale for a path whose (tracked) peak and RMS are these: RMS toward
	/// 0.55 of the reference, the peak never above it, both floored, times the laws' output gain. Shared
	/// by seatAgc() (reset() and a model switch settle on it) and updateAgc() (which slews toward it).
	static float agcTarget(float peak, float rms, float outGain);

	// [set][mip][point]; [kNodes] repeats [0] so the scan reads i and i+1 without a wrap.
	using Table = float[kNodes + 1];

	void rebuildEff(float cf);
	void seatAtRest();
	/// The rest pose re-solved if it is stale and due (restNodes()).
	void refreshRestPose() const;
	void seatAgc();
	float step(float cf); // one half-dt sub-tick; returns the AGC scale

	// THE MODEL SEAM — the one inventory of it. Everything that depends on what is vibrating goes
	// through weave_models.h: the node count, the laws' placement (rebuildEff -> weave::mapLaws), the
	// bow and pluck windows (weave::windows), the elastic force (integrate), the rest pose (seatAtRest,
	// energyForTest) and the scan path (refreshPath). Downstream of scanPath() — updateAgc, buildTables,
	// the fades, sample(), travel and the tick clock — reads only the kNodes-point path and knows nothing
	// of the model.
	void applyPluck(float cf);
	void integrate(float bowVal, float morphBow);
	/// The bridge's nodes and each one's surface offset below rest, for the current model and Bridge.
	void rebuildBridge();
	void applyBridge();
	/// A node's neighbours on the line (the model's topology): 2 along a Loop, 1..2 along a chain, 2..4 on the grid.
	int lineNeighbours(int i, int* out) const;
	void letBuzzThrough();
	void smooth();
	void refreshPath();
	const float* scanPath() const { return weave::pathIsNodes(model_) ? xs_ : path_; }

	float updateAgc(const float* path);
	void buildTables(const float* path, float scale, int set);
	void buildRestTables(float scale, int set);
	void buildMotionTables(int set, int from);
	static void fillTables(Table* t, const float* path, float scale);
	static float readTables(const Table* from, const Table* to, float t, double phase, int mip);
	void tick();
	/// Sizes the window and tabulates its fade and travel positions (prepare, and a Speed change at a boundary).
	void applyTickLength(int length);

	owlet::dsp::PhiWeaveParams a_ {};
	owlet::dsp::PhiWeaveParams b_ {};
	bool lawsSet_ = false;
	float cf_ = 0.f;
	WeaveModel model_ = WeaveModel::Ring;
	WeaveModel pendingModel_ = WeaveModel::Ring;
	Excitation exc_ {};

	float x_[kMaxNodes] {};
	float v_[kMaxNodes] {};
	float xs1_[kMaxNodes] {};
	float xs_[kMaxNodes] {};
	float force_[kMaxNodes] {};
	float path_[kNodes] {};
	Table tables_[3][kMips] {};
	bool restTables_ = false;
	Table rest_[3][kMips] {};
	Table motion_[3][kMips] {};
	float motionSmooth_ = 1.0f;
	float restPath_[kNodes] {};
	// Per window position, filled by prepare() with the very expressions fadeAt() used to compute per
	// call (so the tables change no bit): the smoothstep fade position and the travel fraction (k+1)/len.
	float fadeT_[kMaxTickLength] {};
	float travelFrac_[kMaxTickLength] {};

	// Effective (crossfaded) laws on the model's nodes, rebuilt only when the crossfade moves past an
	// epsilon (or the laws or the model change).
	weave::NodeLaws eff_ {};
	// The rest-pose solve's 33 KB of working storage, here rather than on the audio thread's stack (a
	// model switch solves it inside tick()).
	// MUTABLE with the rest pose below: the lazy solve runs inside the const readers (on the audio thread).
	mutable weave::RestPoseScratch restScratch_ {};
	float effBowWin_[kMaxNodes] {};
	float effBowDepth_ = 0.f;
	float effBowRate_ = 0.f;
	float effBowPos_ = 0.f;
	float effTravelRate_ = 0.f;
	float effOutGain_ = 1.f;
	float effShimmer_ = 0.2f;
	float effCfCached_ = -2.f;

	float agcScale_ = 0.f;
	float agcPeak_ = 1.f;
	float agcRms_ = 0.f;
	float travelPhase_ = 0.f;
	float travelPrev_ = 0.f;
	float bowPhase_ = 0.f;
	float bowLP_ = 0.f;
	/// NOT SAVED WITH THE PATCH (backlog NM 2026-10-18: not persisted): the string is note- or input-excited
	/// physics, a tail, and the Walk bow's position goes with it — a drone reloads from rest.
	float bowWalk_ = 0.f;
	float prevTickCf_ = -1.f;
	float lastMorphBow_ = 0.f;
	uint32_t noiseState_ = 0x2545F491u;
	bool pluckPending_ = true;
	float pluckAmpReq_ = -1.f;
	float pluckPosReq_ = -1.f;

	int tickLength_ = 128;
	double sampleRate_ = 0.0; // 0 until prepare(): setSpeed then only records the speed
	double speed_ = kMinSpeed;
	int pendingTickLength_ = 128; // the window setSpeed asked for, taken at the next boundary
	int windowPos_ = 0;
	uint64_t ticks_ = 0;
	// THE LAZY REST POSE AND ENERGY (restNodes(), motionEnergy()): caches the const readers fill on ask.
	mutable float restNodes_[kMaxNodes] {};
	mutable bool restStale_ = true;          // the laws have changed since restNodes_ was solved
	mutable uint64_t restSolvedTick_ = 0;    // ticks_ when restNodes_ was last solved
	uint64_t lawsMovedTick_ = 0;             // ticks_ during the last tick that rebuilt the laws
	mutable uint64_t restSolves_ = 0;
	mutable uint64_t energyTick_ = 0;        // ticks_ when motionEnergy_ was worked out
	mutable float motionEnergy_ = 0.f;
	uint64_t xRailHits_ = 0;
	uint64_t vRailHits_ = 0;
	// THE BRIDGE: as set, and resolved onto the current model's nodes (rebuildBridge).
	Bridge bridge_ {};
	int bridgeCount_ = 0; // 0: no bridge
	int bridgeNode_[kMaxNodes] {};
	float bridgeOffset_[kMaxNodes] {};
	float contactEnv_[kMaxNodes] {}; // 1 at a contact, relaxing (letBuzzThrough)
	uint64_t bridgeContacts_ = 0;
	uint64_t lineBudgetBound_ = 0;
	uint64_t lineContactSteps_ = 0;
	uint64_t lineCleanSteps_ = 0;
	double lineSumDriftMax_ = 0.0;
	float lineRef_ = 0.f; // the string's held swing (applyBridge)
	float lineRelease_ = static_cast<float>(std::exp(-1.0 / (kLineHoldSeconds * 2.0 * kTickHz))); // per sub-step
	float bridgeMaxGain_ = 0.f;
};

// THE READS, INLINE (backlog PS): every scan head and drone voice of Resonate calls these per sample, and out of line in their
// own TU they were 10% of the whole Wood Elves patch (gprof, the device and the Resonate engine together). The same code,
// moved, so the same bits.
inline float WeaveString::sample(const Fade& f, double phase, int mip) const {
	return readTables((f.segment == 0) ? tables_[kPrev] : tables_[kMid], (f.segment == 0) ? tables_[kMid] : tables_[kCur], f.t, phase, mip);
}

inline float WeaveString::sampleMotion(const Fade& f, double phase, int mip) const {
	if (!restTables_) {
		return 0.0f;
	}
	return readTables((f.segment == 0) ? motion_[kPrev] : motion_[kMid], (f.segment == 0) ? motion_[kMid] : motion_[kCur], f.t, phase, mip);
}

inline float WeaveString::readTables(const Table* from, const Table* to, float t, double phase, int mip) {
	const int m = std::clamp(mip, 0, kMips - 1);
	const float* a = from[m];
	const float* b = to[m];

	double wrapped = phase - std::floor(phase);
	if (!std::isfinite(wrapped)) { // a NaN or infinite phase reads the path at 0, not garbage
		wrapped = 0.0;
	}
	const double pos = wrapped * static_cast<double>(kNodes);
	int i = static_cast<int>(pos);
	float frac = static_cast<float>(pos - static_cast<double>(i));
	if (i >= kNodes) { // wrapped rounded up to 1.0
		i = kNodes - 1;
		frac = 1.0f;
	}
	const float na = a[i] + (b[i] - a[i]) * t;
	const float nb = a[i + 1] + (b[i + 1] - a[i + 1]) * t;
	return na + (nb - na) * frac;
}

} // namespace feathers
