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

#include "character/phi_triangle.hpp"
#include "character/phi_weave.hpp"
#include "dsp/weave_string.h"
#include "fx/automod/automod_envelope.h"
#include "fx/dyn/meter_ramp.h"
#include "fx/resonate/resonate_ear.h"
#include "fx/resonate/resonate_node_string.h"
#include "fx/resonate/resonate_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

struct PFFFT_Setup;

// RESONATE'S DSP — one WeaveString the input plays, read at held notes for a tone and used as an FIR
// kernel for a filter, blended.
//
// HOW THE INPUT REACHES A STRING THAT MOVES AT HAPTIC RATE. The string ticks ~344 times a second, so
// what of the input can reach it is its DYNAMICS: an envelope follower's level bows it (Drive, on top of
// the laws' own bow scaled by Bow) and a sharp enough rise in that level plucks it (Pluck Sens).
//
// **THE TONE IS PLAYED BY THE STRING'S MOTION.** Each head scans the whole string — its shape is the
// timbre, as in the synth engine — but the tone's LEVEL follows how far the string is from rest
// (`WeaveString::motionEnergy`): sqrt(E / kGateEnergyRef), clamped to 1, with a floor where it reaches
// exact zero. So a still string is silent whatever notes are held (the Bow tooltip: "at 0 the string
// moves only when the input plays it, and falls still in silence"), a pluck rings on after the input
// stops and fades, and Bow > 0 keeps the string — and so the tone — going for ever, by design. SUSTAIN
//  blends the NOTE heads toward a steady tone: gain = gate + (1 - gate) x Sustain, so at 1 a
// held note plays the whole string whether it moves or not.
//
// **THE DRONE IS A SET OF SYMPATHETIC STRINGS** : the Chord's zone tunes up to six of them
// above Root, each a head of its own beside the note heads and never one of them — never stolen, never ended
// by a note. The INPUT plays each through a harmonic ear (a comb) at its own pitch — a sympathetic string's TUNED
// RESPONSE, so a note you play lights up the strings it matches — and then its own exponential follower
// (Drone Attack a time constant up, Drone Decay a T60 down). A string's gain is Drone x its follower x its
// level, and nothing else — not the motion gate, not Sustain; under kDroneFloor it is exactly silent. The
// POSITION within the Chord's zone runs tight -> blooming: the ear wide -> sharp, the times x0.5 -> x2
// of the knobs, the upper strings quiet -> loud (droneResponseFor).
//
// **REST IS DECIDED BY THE OUTPUT AND THE STRING, NEVER BY THE INPUT ALONE** (CLAUDE.md, and the plan's
// TAILS addendum). The device PARKS — output exact zero, follower and FIR history cleared — after
// kRestSilentSeconds in which the input was exactly zero, Bow was 0, Morph was still, every output sample
// was under kRestLevel and the tone added nothing (the drone's gain at 0, and no note head sounding or Sustain 0
// and the gate shut). A
// STILL string is seated at rest as it parks; a string still MOVING (it can park only because no head is
// reading it) is frozen where it is, and resumes from there when something wakes the device. Anything else
// keeps it awake, and a self-sustaining state (Bow > 0, or Sustain > 0 with a head sounding) never parks:
// there is deliberately no timeout.
//
// PARTITION-FREE: every smoother and counter runs per sample and the string runs on its own tick clock.
// NOTES act at the START of the piece they arrive in: a note's timeSamples is ignored, as DAHDSR ignores
// it, because the piece is the rack's cut — so the same samples and the same notes in the same pieces
// are the same samples out.
//
// NO ALLOCATION: every buffer is a member, sized at compile time.

namespace feathers::fx::resonate {

// ============================================================================
// THE DRONE STRINGS' φ-TRIANGLE BANK (scale them per string with phi triangles) — in
// phi_weave.hpp's style. Each string's four multipliers are these triangles, unipolar 0..1, evaluated by
// `phi::evalTriangle(position + interval x kPhiDroneStringSpread, 1, cfg)`: the zone's position is the phase,
// and the string's INTERVAL places its landscape (as phi_weave's evalSpatial places one per node) — so a
// string at a given interval behaves the same in every chord, and two strings at one position differ.
// BY-EAR FIRST CUTS, every entry.
// ============================================================================

/// Each semitone of interval moves a string's phase by 1/φ of a cycle: irrational, so no two intervals in
/// an octave and a half line up.
inline constexpr float kPhiDroneStringSpread = owlet::dsp::phi::kPhiN100;
/// Its follower's attack scale (x0.5..x2), decay scale (x0.5..x2), level (-12..+6 dB) and ear Q (2..32, the comb's sharpness: resonate_ear.h).
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiDroneAttack = { owlet::dsp::phi::kPhi125, 0.85f, 0.130f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiDroneDecay = { owlet::dsp::phi::kPhi075, 0.90f, 0.570f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiDroneLevel = { owlet::dsp::phi::kPhi100, 1.00f, 0.310f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiDroneQ = { owlet::dsp::phi::kPhi050, 0.85f, 0.790f, false };
/// Its place between left and right: a bipolar triangle on the string's INDEX in its chord (index x 1/φ), so every
/// string of a chord, a Tanpura's two Sa included, has a side of its own; Drone Width scales it. A first cut.
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiDronePan = { owlet::dsp::phi::kPhi150, 1.00f, 0.430f, true };
/// THE GOURD BODY'S BANK (2026-09-27): three peaks — a low body resonance, a warm band, an upper one — each a frequency,
/// a Q and a gain, every one a φ-triangle at the Drone Character's coordinate (so the body changes with it, continuously
/// across its zones). The ranges are ResonateDsp::kBody*; all by-ear first cuts, not measured from a gourd.
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyLowHz = { owlet::dsp::phi::kPhi033, 1.0f, 0.11f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyLowQ = { owlet::dsp::phi::kPhi067, 1.0f, 0.29f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyLowGain = { owlet::dsp::phi::kPhi025, 1.0f, 0.47f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyWarmHz = { owlet::dsp::phi::kPhi125, 1.0f, 0.61f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyWarmQ = { owlet::dsp::phi::kPhi150, 1.0f, 0.73f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyWarmGain = { owlet::dsp::phi::kPhi075, 1.0f, 0.83f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyUpperHz = { owlet::dsp::phi::kPhi175, 1.0f, 0.19f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyUpperQ = { owlet::dsp::phi::kPhi200, 1.0f, 0.37f, false };
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiBodyUpperGain = { owlet::dsp::phi::kPhi100, 1.0f, 0.53f, false };
/// THE TWIN GOURDS' SPREAD: how far apart (in octaves, each side) the left and right gourds' peaks sit.
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiTwinSpread = { owlet::dsp::phi::kPhi150, 1.0f, 0.07f, false };
/// THE CYCLE'S STEP STRENGTHS: step s of the pattern plucks at 0.5 + 0.5 x this triangle at (8 x the Cycle coordinate
/// + s x kPhiDroneStringSpread) — the landscape runs on across the zones, as the coordinate does.
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiCycleVelocity = { owlet::dsp::phi::kPhi125, 1.0f, 0.0f, false };
/// WHERE EACH DRONE STRING'S CYCLE PLUCK LANDS ON THE RING: its interval through this triangle, 0..1 along the path.
inline constexpr owlet::dsp::phi::PhiTriConfig kPhiCycleRingPos = { owlet::dsp::phi::kPhi075, 1.0f, 0.17f, false };


class ResonateDsp {
public:
	/// FIR taps at the most (Size's range) and the history ring behind them, a power of two above it.
	static constexpr int kMaxTaps = 256;
	static constexpr int kHistory = 512;
	/// NOTES QUEUED BETWEEN TWO PROCESS CALLS; later ones are dropped past this (registry.h's rule).
	static constexpr int kMaxQueuedNotes = 128;
	/// KEYS HELD AT ONCE, physically, whatever the Voicing: what Mono takes last-note priority from and what
	/// a Voicing change replays. A seventeenth key drops the oldest.
	static constexpr int kMaxHeldKeys = 16;

	// THE FEEL CONSTANTS — first cuts, to be set by ear (the design's backlog). Each is used in one place.
	/// Bow 1 is the laws' own bow doubled.
	static constexpr float kBowGain = 2.0f;
	/// Drive 1 at a full-scale level adds eight times the laws' bow.
	static constexpr float kDriveGain = 8.0f;
	/// The follower: 1 ms up, 100 ms down.
	static constexpr float kFollowAttack = 0.001f;
	static constexpr float kFollowRelease = 0.100f;
	/// THE RING IS PLUCKED BY A NOTE-ON, NOT BY THE INPUT (v19, by design; the level-based onset detector is gone: it fired
	/// extra plucks on a polyphonic input's beating — the flash seen at Jawari 0, Sympathy 0 — and re-attacked the notes
	/// ringing). A note-on plucks the ring once, at the laws' pluck position, velocity^2 x pluckGain(Pluck) x kPluckMax; with no
	/// MIDI nothing plucks it — it moves only with Drive, the Bow, the Cycle and the puck.
	static constexpr float kPluckMax = 1.0f;
	/// PLUCK (v13, maybe we replace the pluck knob which is kinda useless as is): how hard every ring pluck is, a played
	/// note's and the Cycle's alike — x 2 Pluck up to its middle (the old amount, bit for bit), then geometrically to
	/// kPluckTop at 1, just short of where the ring's rails start to bite — MEASURED 2026-10-04, every model played hard by a
	/// plucked input and the Cycle at Bow 0: no rail at 1.5x; the Bar's displacement rail on 7e-5 of its node-steps at 1.75x and
	/// 0.27% at 2x, which is also where the ring caps a pluck (WeaveString::kMaxPluckAmp). So +3.5 dB over the old top, not
	/// more: the ring has no more room.
	static constexpr float kPluckTop = 1.5f;
	static float pluckGain(float pluck) {
		const float p = std::isfinite(pluck) ? std::clamp(pluck, 0.0f, 1.0f) : 0.0f;
		return p <= 0.5f ? 2.0f * p : std::pow(kPluckTop, 2.0f * p - 1.0f);
	}
	/// VELOCITY (v13, we should also scale for velocity): a played note's pluck is (velocity)^2 as hard — 127 the
	/// old level, 64 a quarter (-12 dB), 40 a tenth (-20 dB) — on its node string's drive, its ring pluck and its scan burst.
	/// A NOTE'S STRING IS PLAYED BY ITS SCANNED VOICE ONLY AS A PLUCK (v14, the audit's R1): a burst of it, kDrivePluckGain
	/// at the pluck and falling over kDrivePluckSeconds, at each ring pluck (a note-on's or the Cycle's). Until then it was
	/// driven by the scanned voice all the time — which, before the gate, reads the ring's REST shape, a waveform at the
	/// note's own pitch that never stops — so the string charged up for about 0.6 s and held near 0 dBFS: MEASURED at String
	/// Excite 0, -25.9 dB at 20..40 ms rising to -1.5 dB at 1 s. As a 5 ms burst at x64 it peaks within 20 ms at about -5 dB
	/// and falls on its own T60 (-20 dB at 1 s); longer bursts peaked later (30 ms: at 80 ms). Measured against the drive
	/// scaled by the ring's motion (still +1.4..3.4 dB of swell at the default).
	static constexpr float kDrivePluckSeconds = 0.005f;
	static constexpr float kDrivePluckGain = 64.0f;
	/// THE BURST FOLLOWS ITS RING PLUCK (v19, resonate-audit: strum/tremolo sometimes). It was 1 at every ring pluck
	/// whatever its strength, so with the Cycle on every Cycle pluck re-attacked each held note (MEASURED +5.8 dB median at the
	/// defaults, +6.8 at Pluck 0.1, where the ring pluck is small and the burst was not). A NOTE-ON's burst is x Pluck x its
	/// velocity, and goes to ITS OWN head only (Requirement: playing a note re-attacked the notes ringing, +10..12 dB). A CYCLE
	/// pluck's, to every head, is its ring pluck's amplitude (step x kCycleRingPluck x Pluck) x kCycleBurst: MEASURED the
	/// held note's jump at each Cycle pluck 1.3 dB median at the defaults, 1.1 at Pluck 0.1, 1.8 at Pluck 1 (proportional alone,
	/// kCycleBurst 1: 4.2, 1.9, 5.3; 0.5: 2.3, 1.1, 3.2; 0.13: 0.8, 1.1, 1.1).
	static constexpr float kCycleBurst = 0.25f;
	/// AND BY THE RING'S MOTION, ALL THE TIME (v15, by design: motion only): the head's scan less the scanned REST shape at
	/// the same phase (WeaveString::sampleMotion: the same tables, scale, mips and interpolation, so exactly 0 at rest), so a
	/// bowed held note's string sounds and breathes again with the bow, and a still ring plays it nothing — the rest shape
	/// that charged R1's swell is gone. kMotionDriveGain: the most that keeps the swell case's bound (+1.5 dB of energy after
	/// 20 ms): MEASURED 2026-10-04 the worst +1.08 dB at x64 (A2, Bow 0 and 0.25), +3.2 at x96. kMotionSmoothing: the motion
	/// eased across sub-ticks — a small difference of two large tables, it is rough (its buzz share's window-to-window sd
	/// 0.026..0.039 on the bowed fixtures at 1, 0.015..0.035 at 0.2, the level's unchanged).
	static constexpr float kMotionDriveGain = 64.0f;
	static constexpr float kMotionSmoothing = 0.2f;
	/// THE MOTION DRIVE'S CAP (v15): each head's drive scaled by min(1, kDriveCap / its envelope) — instant attack,
	/// kDriveCapRelease — so a hard pluck's ring motion (drive 8.9 against a bowed note's 1.9 and a bow's 0.13) cannot ring the
	/// string past 0 dBFS (MEASURED: a 1 ms click at Blend 0 peaked 1.16..1.76 uncapped, at or under the motion-off peak capped;
	/// 1.0 failed the swell case's fall, 0.25 the bowed case's level).
	static constexpr float kDriveCap = 0.5f;
	static constexpr float kDriveCapRelease = 0.05f;
	/// THE NOTES' STRINGS' DRAG LEVEL (NodeString::setDrag, v15), in the string's heard units: a 0 dBFS sine held on a note's own
	/// pitch rang its string 25x past a pluck (the output 22.2 at Blend 0, String and Excite 1). The lowest that leaves a -12 dBFS
	/// pluck (A1..A5 over String, Excite and Jawari) within 0.5 dB: MEASURED 2026-10-04 the held sine's worst 1.27 (0.9 would cost
	/// the loud plucks 1-2.5 dB; 0.36 moved an A1 pluck's RMS 0.82 dB). Not on the drone's strings: any level a held sine would
	/// notice, a Cycle plucking at a high Level reaches too (the Fixed and Cycle Level cases).
	static constexpr float kNoteDragLevel = 0.39f;
	/// THE DRONE'S FEED BOUND (v15): a drone string's ear-fed drive is scaled by 1 - kDroneFeedSlope (heard / kDroneFeedBound - 1)
	/// once its heard level (NodeString::heardEnv, after the makeup) passes kDroneFeedBound, eased over kDroneFeedSeconds — so a
	/// sine held on its pitch (its ear and string both resonant: 13x at Drone Input +12 dB) cannot ring it past about
	/// (1 + 1 / kDroneFeedSlope) x the bound, while a Cycle strike (a strike, not a feed) is untouched, at any Cycle Level.
	/// MEASURED 2026-10-04: the held root sine's worst 1.35 (from 9.3), -12 dBFS plucked input and the Cycle exactly unchanged.
	static constexpr float kDroneFeedBound = 0.75f;
	static constexpr float kDroneFeedSlope = 4.0f;
	static constexpr float kDroneFeedSeconds = 0.005f;
	static float velocityGain(double velocity) {
		const float v = std::isfinite(velocity) ? std::clamp(static_cast<float>(velocity), 0.0f, 1.0f) : 1.0f;
		return v * v;
	}
	/// The tone's gate: full at kGateEnergyRef, exact zero below kGateFloor of it (in amplitude). By-ear
	/// first cuts, both, and kGateRelease with them.
	static constexpr float kGateEnergyRef = 0.05f;
	static constexpr float kGateFloor = 1.0e-3f;
	/// The energy envelope the gate reads falls at most this much a tick (~-76 dB a second at 344 Hz).
	static constexpr float kGateRelease = 0.95f;
	/// A head's edges.
	static constexpr float kHeadAttackSeconds = 0.005f;
	static constexpr float kHeadReleaseSeconds = 0.030f;
	/// A STOLEN HEAD fades out this fast before it starts its new note, so a steal never jumps phase at gain.
	/// A by-ear first cut.
	static constexpr float kStealSeconds = 0.002f;
	/// WIDTH 1 reads the right channel this far round the string (a quarter of a cycle) from the left, in
	/// the tone and in the right channel's kernel alike.
	static constexpr float kWidthCycles = 0.25f;
	/// Size's floor: the shortest kernel the filter runs.
	static constexpr int kMinTaps = 32;
	/// THE DRONE CUTS TO EXACT ZERO below this gain (-100 dB), so a drone ringing down ends and the device can
	/// rest: a one-pole only approaches zero.
	static constexpr float kDroneFloor = 1.0e-5f;
	// EACH STRING'S RANGES (scale them per string with phi triangles). Every string's four
	// multipliers come from φ-triangles (`kPhiDrone*`, above the class) evaluated at the zone's position, each
	// string's placed by its own interval — so moving within a zone moves every string differently, some
	// tightening while others bloom. A triangle's 0..1 maps into these ranges: geometrically for the Q and the
	// times, in dB for the level. By-ear first cuts.
	/// A string's ear Q (a comb's, resonate_ear.h, matched to a band-pass of this Q at f): wide (Q 2, a note within ~a third excites it) to narrow (Q 32, within a
	/// semitone or so).
	static constexpr float kChordQTight = 2.0f;
	static constexpr float kChordQBloom = 32.0f;
	/// Drone Attack's and Decay's scale: x0.5 (quick to swell, quick to go) to x2 (slow to swell, long to ring).
	static constexpr float kChordTimeTight = 0.5f;
	static constexpr float kChordTimeBloom = 2.0f;
	/// A string's level, in dB, before the chord is normalised to one string's power (so it never gets louder
	/// by having more strings): -12 (quiet) to +6 (loud).
	static constexpr float kChordLevelLowDb = -12.0f;
	static constexpr float kChordLevelHighDb = 6.0f;
	/// A string whose pitch the Chord changes fades out this fast, retunes from silence, and fades back in —
	/// so a zone change never jumps a sounding string's pitch. And a string's level moves toward a new one over
	/// this time constant, so a chord with another string count never steps the others' levels.
	static constexpr float kRetuneSeconds = 0.010f;
	static constexpr float kStringLevelSeconds = 0.010f;
	/// THE SCANNED DRONE VOICE'S LEVEL IS ITS STRING'S (v12: "it seems to be applying an env to the entire
	/// drone output which causes a weird pluck on the decay of any ringing strings which are already going"). Until v11 it
	/// was kDroneMakeup (6) x a follower of the INPUT (its ear, Drone Attack/Decay, a kick per Cycle pluck), so every new
	/// excitation re-attacked the scanned voice of every string already ringing — MEASURED: a Sa string ringing while a
	/// sine plucked Pa rose 14 dB at Voice 0.2 and 18 dB at Voice 1, its own string 3 dB. Now it is kScanMakeup x its
	/// node string's own level (an RMS over kScanLevelPeriods of its own period): the scanned voice rings and decays with its
	/// string, and Drone Attack and Decay shape only the excitation and the string.
	/// MEASURED 2026-10-03: 5.3 sounds the scanned voice as loud as its string per unit of Voice (the defaults' Cycle drone:
	/// 0.35 each), so Voice is a crossfade between the two at one level.
	static constexpr float kScanMakeup = 5.3f;
	/// The RMS's time constant, in the string's periods: short enough to fall as fast as the string (a one-pole on the
	/// power falls at most 4.34 / tau dB a second: over a fixed 100 ms it fell 43 dB a second, MEASURED -12 dB where a 0.64
	/// s T60 string fell -30), long enough that the scanned voice does not ripple with the string's own swing (about 2%).
	static constexpr double kScanLevelPeriods = 2.0;
	/// THE NODE STRING'S MAKEUP (v11, the drone's strings become the notes' node strings; the plucks dont sound as
	/// good as the string with jawari on): the feed is this x the string's force, and NodeString scales a force by its own
	/// loss and Courant number, so a feed held on a string's pitch rings it at a level its decay does not set. v11 kept
	/// v10's contract (a feed held on a string's pitch rings it at 2x its size: 2 / 2.7, the node string's gain from force
	/// to its bridge read being 2.62..2.76 at the fundamental, MEASURED) and the follower's kDroneMakeup (6) lifted the
	/// scanned voice of an input-played drone. v12 takes that makeup off the follower (kScanMakeup) and puts it here, on
	/// the feed: 2.0, MEASURED 2026-10-03 at the defaults, sounds a plucked input's drone at 0.0182 RMS against v11's
	/// 0.0165 (+0.8 dB), and the Cycle-played drone (struck, not fed) at 0.296 against 0.350 (-1.4 dB). The bridge loop's
	/// coupling (v18) is the yielding bridge's, which does not read this.
	static constexpr float kNodeMakeup = 2.0f;
	/// THE DRONE'S STRINGS RING BRIGHT (v17): their frequency-dependent loss x this (a tanpura's strings ring long and bright).
	static constexpr double kDroneHighLoss = 0.5;
	/// THE BRIDGE YIELDS (v18, tuned conserve cycle d2 0.25 low is my favourite). Every string — the drone's and every
	/// sounding note's — ends on ONE bridge: its bridge-end node IS the bridge's displacement w, a mass m (in node masses) on a
	/// spring to the soundboard at kBridgeHz, lightly damped (kBridgeDamping a step, a T60 of about 15 s):
	///     w+ = 2w - w- + F / m - (2 pi kBridgeHz / sr)^2 w - kBridgeDamping (w - w-),   F = sum over strings of L^2 (u[1] - w)
	/// and each string steps with its end at w+ (NodeString::setYield). Energy only moves between the strings through it, and
	/// the whole (stepBridge) never rises: what one string sets ringing is what the others gave. A string tuned to a partial
	/// of another is driven at its resonance and takes up much more than a detuned one, with no ear and no gain in the loop.
	/// The old coupling (an additive bus heard through every string's ear, the notes one way onto it, the drone one way back
	/// onto the held notes, sized by a small-gain argument) is gone with it.
	/// SYMPATHY IS THE BRIDGE'S LIGHTNESS, on a log law: m from kBridgeMassBottom at the bottom of the knob to kBridgeMassTop at
	/// its top, and at 0 the bridge is RIGID (it returns to rest and the strings are independent). The prototype's tested law was
	/// m = 2 x 1000^(1 - s): it was liked s 0.25 (356 node masses: the default, mid-travel) and found s 0.5 and 1 "pretty bad", so
	/// the knob spans s 0.125 (843) to 0.375 (150) — the low part of that law, where no slow exchange (the "breathing", at s
	/// 0.75 and over) happens. v20 (testing at the top: "these sound good" of renders down to mass 2, the string's buzz
	/// MEASURED collapsing at mass 6 on D2 and 20 on C3): the top is now 63 and the default 150 (v18's top), so every v18 setting
	/// is still on the knob, lower. Measured in the plugin: stated in test_fx_resonate.
	static constexpr double kBridgeMassBottom = 843.0;
	static constexpr double kBridgeMassTop = 63.0;
	static constexpr double kBridgeHz = 3000.0;
	static constexpr double kBridgeDamping = 2e-5;
	/// RIGID (Sympathy 0): the bridge falls back to rest over about this long, then the strings are let off it.
	static constexpr double kBridgeRestSeconds = 0.005;
	/// THE BRIDGE'S MASS at Sympathy s (0 rigid: none).
	/// A GROUP'S LEVER ONTO THE BRIDGE (v21): sqrt(bridge mass / the mass its own knob names), so the group sees exactly its own
	/// mass; 1 (to the bit) when its knob is the lighter, 0 when its knob is 0 (off the bridge).
	static double bridgeCouplingFor(double mass, double ownMass) { return ownMass > 0.0 ? std::sqrt(mass / ownMass) : 0.0; }
	static double bridgeMass(float s) {
		return s > 0.0f ? kBridgeMassBottom * std::pow(kBridgeMassTop / kBridgeMassBottom, static_cast<double>(std::min(s, 1.0f))) : 0.0;
	}
	/// THE NOTES' SYMPATHY REACHES FURTHER (v21, more transfer from the played notes into the drones): the same log law from
	/// kBridgeMassBottom, to kBridgeMassTopNotes at its top; its default the mass 150 the shared law's did (kSympathyDefault there,
	/// kSympathyNotesDefault here). Drone Sympathy keeps bridgeMass.
	/// MEASURED 2026-10-05 (a Sa note re the drone string it sets ringing, Drone Sympathy at its default; the notes' buzz share and
	/// worst pitch over A2 E3 A3 at Jawari 0.5): mass 150 -18.5 dB, 0.0085, 0.7 cents; 63 -15.0, 0.0065, 1.6; 40 -13.4, 0.0065, 2.5;
	/// 35 -13.0, 0.0064, 2.9; 30 -12.5, 0.0047 (the buzz falls), 3.3; 20 -11.7, 0.0046, 5.0 (the pitch bound); 10 -11.2, 0.0023,
	/// 10.0. The top is 35, just before the buzz falls.
	static constexpr double kBridgeMassTopNotes = 35.0;
	static double bridgeMassNotes(float s) {
		return s > 0.0f ? kBridgeMassBottom * std::pow(kBridgeMassTopNotes / kBridgeMassBottom, static_cast<double>(std::min(s, 1.0f))) : 0.0;
	}
	/// THE BEND (v18, MPE): a head follows its key's tuning expression on a one-pole this long — a host sends it at block rate,
	/// and this smooths those steps (at most 0.4 cents a sample for a 1-semitone step at 48 kHz) without lagging a bend by ear.
	static constexpr double kBendSeconds = 0.005;
	/// A CYCLE PLUCK (v11): the string STRUCK where it is driven — a raised-cosine push kDroneStrikeSeconds long
	/// (NodeString::strike), kCyclePluckAmp at its middle x the step's strength x Cycle Level — as the notes' strings are
	/// played (by a force at their drive point). MEASURED 2026-10-03 (the median of seven Unison strings A1..A4 after one
	/// pluck, the share of harmonics 3..40): pulled aside at the pickup instead, a node string hardly meets its line — 0.004
	/// at Jawari 0 and 0.004..0.012 at any Jawari, with or without the ring's orbit; struck for 3 ms, 0.003 at 0 (as pure
	/// as v10's comb pluck) rising to 0.084 at 1, the notes' strings' range. Shorter strikes are bright at Jawari 0 (0.15 at
	/// 1 ms, 0.20 at 0.3 ms). kCyclePluckAmp: until v15 the defaults' Cycle-played drone as loud as v10's (0.0738: 0.350 RMS against 0.352) —
	/// which peaked 2.40 at the defaults (a Unison string struck every quarter at 0.5 s a beat rings up across its strikes over
	/// its 12 s Decay) and 1.45 on the Tanpura. v15: 0.024 (-9.7 dB), the defaults' peak at 0.78 (MEASURED 2026-10-04).
	static constexpr double kDroneStrikeSeconds = 0.003;
	static constexpr float kCyclePluckAmp = 0.0225f;
	/// THE RING'S ORBIT REACHES THE DRONE'S BRIDGES AT HALF THE NOTES' REACH (NodeString::kProximity x this). v14 measured full
	/// reach (the fix 5) both ways: on a Cycle-plucked drone at Bow 0 it pressed the strings flat — the median buzz over
	/// Jawari 0..1 stopped rising (0.068 at 0.5, 0.053 at 0.75) and a string went 8 cents sharp — while under the default
	/// Bow 0.25 (the audit's fixture, the Tanpura's low Sa at Jawari 1) it buzzed more, 0.150 against 0.090. Kept at half,
	/// as v11 had it (decided: keep half where full measures worse).
	static constexpr float kDroneOrbitReach = 0.5f;
	/// BRIDGE FIXED'S HEIGHTS (v13, NodeString::setFixed), at 165 Hz: the TYPICAL held swing of each kind of string at the
	/// defaults, so Fixed 1 buzzes about as Fixed 0 at a typical level and less softer, more harder. MEASURED 2026-10-04 at
	/// Jawari 1, the median over seven strings A1..A4 of the held swing over 0.2..3.2 s, each brought to 165 Hz as 1 / f: a
	/// drone string after one Cycle pluck at the default Cycle Level, 0.36 (raw 2.10 at A1 .. 0.127 at A4, so f^-1.35); a
	/// note's string after the plucked-input fixture, 0.0101 (0.024 .. 0.0053, f^-0.74). As 1 / f the residual tilt over the
	/// strings is about f^+-0.35. The drone's scaled with kCyclePluckAmp in v15 (0.36 x 0.0225 / 0.0738).
	/// v17: the drone's strings stand on it ALWAYS (a geometric bridge: Bridge Fixed is the notes' alone).
	static constexpr float kFixedDroneRef = 0.11f;
	static constexpr double kFixedDroneSlope = 1.35;
	static constexpr float kFixedNoteRef = 0.0101f;
	/// THE CYCLE'S RELOCK WINDOW: a song position more than half of this behind the last grid point plucked has jumped
	/// (a loop back, a locate). It was half a v10 pluck's bump, the furthest a led pluck sat ahead of its grid.
	static constexpr double kCycleRelockSeconds = 0.025;
	/// DRONE ATTACK'S SWELL (the process loop): the feed counts as present above kSwellFloor, and with nothing fed the swell
	/// falls back over kSwellFallSeconds, so the next note swells in again.
	static constexpr float kSwellFloor = 1.0e-6f;
	/// DRONE INPUT'S SMOOTHING: a one-pole over 10 ms, per sample (process()).
	static constexpr float kDroneInputSeconds = 0.01f;
	/// THE NOTES' STRINGS: String and String Excite followed over 10 ms, per sample; their T60 at Ring Decay 0 (Ring Decay
	/// scales it by 2^value, as it does the ring's damping); the ring's held swing for the orbit, released over 2 s; the
	/// strings' output level against the scanned voice. By measurement (test_fx_resonate).
	static constexpr float kStringSeconds = 0.01f;
	static constexpr double kVoiceStringT60 = 4.0;
	static constexpr double kProximityHoldSeconds = 2.0;
	static constexpr float kVoiceStringLevel = 2.0f;
	/// THE INPUT'S DRIVE of the notes' strings against the scanned voice's: +12 dB, so a -12 dBFS input plays them about as hard
	/// as the ring's full-scale voice does (MEASURED 2026-09-28: a -12 dBFS plucked input rang them 15-20 dB under the scan).
	static constexpr float kStringInputGain = 4.0f;
	static constexpr float kSwellFallSeconds = 0.25f;
	/// THE GOURD BODY'S RANGES (the φ bank kPhiBody*: a triangle's 0..1 lands geometrically in Hz and Q, linearly in
	/// dB, the dB then times Body). v11 (twin and body have very little impact on the sound; MEASURED at Body 1 on
	/// a drone: +4.3 dB in the 125 Hz octave, +2.3 at 250, under 2 dB elsewhere, and the notes untouched): the peaks are
	/// deeper and the low one narrower (it rings: the bloom), the body sits on the notes too, and its level is matched.
	static constexpr double kBodyLowHz[2] = { 90.0, 180.0 };
	static constexpr double kBodyLowQ[2] = { 2.0, 6.0 };
	static constexpr double kBodyLowDb[2] = { 6.0, 12.0 };
	static constexpr double kBodyWarmHz[2] = { 250.0, 600.0 };
	static constexpr double kBodyWarmQ[2] = { 0.8, 1.6 };
	static constexpr double kBodyWarmDb[2] = { 4.0, 8.0 };
	static constexpr double kBodyUpperHz[2] = { 1800.0, 3500.0 };
	static constexpr double kBodyUpperQ[2] = { 1.2, 3.0 };
	static constexpr double kBodyUpperDb[2] = { 3.0, 7.0 };
	/// THE TWIN GOURDS (v11): the body as two gourds, the left one's peaks Twin x spread octaves below the centre and the
	/// right one's as far above, so the two sides colour differently (width) and their sum has two of each formant
	/// (thickness). The spread 0.1..0.25 octave each side, from the Character.
	static constexpr double kTwinOctaves[2] = { 0.05, 0.12 };
	/// THE GOURD BODY'S PEAKS AND THE TWIN GOURDS' RESONANCES at a Character coordinate — a pure function, the one the
	/// DSP runs on, public so a case can restate every number from the bank.
	struct BodyPeak {
		double hz = 0.0;
		double q = 1.0;
		double db = 0.0;
	};
	struct Body {
		std::array<BodyPeak, 3> peaks {};
		double twinOctaves = 0.0; ///< each side's shift at Twin 1
	};
	/// THE BODY ON ONE CHANNEL at Body `amount`, its peaks shifted by `octaves`: the cascade's level-matching gain, the
	/// reciprocal of its mean power gain over 64 log-spaced points from 60 Hz to `topHz`.
	static double bodyMakeup(const Body& b, double amount, double octaves, double sampleRate, double topHz = 8000.0);
	/// The body's power gain at `hz`, at Body `amount` and moved by `octaves`.
	static double bodyPower(const Body& b, double amount, double octaves, double sampleRate, double hz);
	static Body bodyFor(double coordinate);
	/// THE CYCLE: the string step `step` of pattern `pattern` plucks, for a chord of `count` strings, and how hard.
	static int cycleString(CyclePattern pattern, int count, int step);
	static float cycleVelocity(double coordinate, int step);
	/// WHERE ON THE RING a Cycle pluck of the string at `interval` lands (0..1 along its path).
	static float cycleRingPluckPos(int interval);
	/// A Cycle pluck's ring pluck: step x Cycle Level x this, capped at kPluckMax (a full-velocity note's). By measurement: a
	/// default step landed at about a medium input onset's pluck (v13-v18, when the input plucked it).
	static constexpr float kCycleRingPluck = 0.6f;
	/// THE PATTERN THE CYCLE COORDINATE'S ZONE NAMES.
	static CyclePattern cyclePatternAt(double coordinate);


	/// WHAT THE CHORD CONTROL ASKS OF THE DRONE'S STRINGS NOW — a pure function of the settings, and the one the
	/// DSP runs on. Public so a case can read it and restate every number from the controls' meaning.
	struct DroneResponse {
		int zone = 0;
		float position = 0.0f;
		/// The continuous coordinate in zones (8 x the control), laps included: every string's triangles run on it.
		double coordinate = 0.0;
		int count = 1;
		std::array<int, kMaxDroneStrings> intervals {};
		// PER STRING, 0 past `count`:
		/// Its ear's Q.
		std::array<float, kMaxDroneStrings> q {};
		/// Its pitch above Root in semitones, in the tuning (fractional in Just), and its voice's brightness 0..1 (the
		/// triangle its Q is read from: a sharp ear, a bright string).
		std::array<double, kMaxDroneStrings> pitch {};
		std::array<float, kMaxDroneStrings> brightness {};
		/// Its follower's attack time constant and decay T60, in seconds: the knobs times its own scales.
		std::array<float, kMaxDroneStrings> attackScale {};
		std::array<float, kMaxDroneStrings> decayScale {};
		std::array<float, kMaxDroneStrings> attackSeconds {};
		std::array<float, kMaxDroneStrings> t60Seconds {};
		/// Its level in dB from its triangle, and the level it plays at — the chord normalised to one string's power.
		std::array<float, kMaxDroneStrings> levelDb {};
		std::array<float, kMaxDroneStrings> level {};
		/// Its detune in cents (the unison pair of a Tanpura, ±kDroneUnisonDetuneCents; 0 otherwise) and its place
		/// between left (-1) and right (+1) before Drone Width scales it (kPhiDronePan on its index).
		std::array<float, kMaxDroneStrings> detuneCents {};
		std::array<float, kMaxDroneStrings> pan {};
	};
	static DroneResponse droneResponseFor(const Settings& s);
	/// droneResponseFor(settings_), MADE AGAIN ONLY WHEN ONE OF THE FIVE SETTINGS IT READS MOVED (backlog PS): it was made at every
	/// host call and every picture, ~40 pow, exp2 and triangle evaluations each time. A pure function of those five, so the same bits.
	const DroneResponse& droneResponse();
	/// A CYCLE PLUCK, as logged.
	struct CyclePluck {
		uint64_t at = 0;
		int string = -1;
		float velocity = 0.0f;
	};
	/// A KERNEL `length` taps long (continuous, Size) has this many taps: those whose centre, t + 1/2, lies
	/// inside it. Its window reaches zero at the length, so a tap appears or goes at zero weight.
	static int tapsFor(float length) { return static_cast<int>(std::ceil(length - 0.5f)); }
	/// Rest: CLIP's numbers (-180 dBFS, 50 ms).
	static constexpr float kRestLevel = 1.0e-9f;
	static constexpr double kRestSilentSeconds = 0.05;

	ResonateDsp() = default;
	~ResonateDsp();
	ResonateDsp(const ResonateDsp&) = delete;
	ResonateDsp& operator=(const ResonateDsp&) = delete;

	/// Makes the FFT setup the kernel's normalisation uses (once; the only allocation this class makes).
	void prepare(double sampleRate);
	/// AS IF JUST SWITCHED ON: the next process call restarts everything from the settings it then has.
	void reset();
	void setSettings(const Settings& s) { settings_ = s; }
	/// Queued; they act at the start of the next process call.
	void queueNotes(const device::NoteEvent* in, size_t count);
	void setSink(device::ReadingsSink sink) { sink_ = sink; }
	/// THE HOST'S TRANSPORT, once a block before process(): the Cycle's grid (resonate_dsp.cpp).
	void setTransportClock(const device::TransportClock& clock) { clock_ = clock; }
	void process(float* left, float* right, int frames);

	/// **THE FX LANES' TAP** (take the fx). Set, `process` ALSO writes, for each of its samples from
	/// index 0, the dry it heard, the wet's TONE part (Level x the tone/filter blend), the DRONE, and the Mix and Enable it
	/// applied — so its holder can run a chain on the drone and one on the wet and do the mix itself (resonate.cpp). What
	/// `process` writes to left/right is unchanged, and so is when it parks: the tap is a copy, never a road. Each
	/// buffer must hold the call's frames. Null (the default): nothing is written.
	struct LaneTap {
		float* dry[2];
		float* tone[2];
		float* drone[2];
		float* mix;
		float* enable;
	};
	void setLaneTap(const LaneTap* tap) { tap_ = tap; }

	// --- test seams ---
	const WeaveString& stringForTest() const { return ring_; }
	/// A drone string's follower and ear, and the bridge's displacement now.
	const HarmonicEar& droneEarForTest(size_t i) const { return strings_[i].ear; }
	int droneIntervalForTest(size_t i) const { return strings_[i].active ? strings_[i].interval : -999; }
	/// A drone string's node string (its node count and pitch: Drone Detail, backlog PS).
	const NodeString& droneNodeForTest(size_t i) const { return strings_[i].node; }
	bool droneActiveForTest(size_t i) const { return strings_[i].active; }
	double bridgeForTest() const { return bridgeW_; }
	double bridgeMassForTest() const { return bridgeMass_; }
	double bridgeCouplingNotesForTest() const { return bridgeCouplingNotes_; }
	double bridgeCouplingDroneForTest() const { return bridgeCouplingDrone_; }
	/// THE WHOLE INSTRUMENT'S ENERGY (v18): every string on the bridge less its bridge-end node's kinetic term, every string
	/// off it, and the bridge's own — never rising once nothing drives it.
	double bridgeEnergyForTest() const;
	/// Drone string `k` struck as a Cycle pluck strikes it (x `velocity`, at Cycle Level), the device woken; nothing else plucked.
	void strikeDroneForTest(size_t k, float velocity = 1.0f);
	/// A drone string's voice: its node string, its last output, and what it sounds at.
	const NodeString& droneStringForTest(size_t i) const { return strings_[i].node; }
	const NodeString& voiceStringForTest(size_t slot) const { return voiceStrings_[slot]; }
	/// The ring's orbit toward its bridge as the notes' and drone's strings last read it, 0..1.
	float proximityForTest() const { return proximity_; }
	/// The last sample's note-string part of the tone (left), before the heads' gains are summed.
	float voiceStringLForTest() const { return lastVoiceStringL_; }
	float droneVoiceOutForTest(size_t i) const { return strings_[i].voiceOut; }
	float droneOutLForTest(size_t i) const { return strings_[i].outL; }
	float droneFeedForTest(size_t i) const { return strings_[i].feed; }
	float lastRingPluckForTest() const { return lastRingPluckAmp_; }
	float headOutLForTest(size_t slot) const { return headOutL_[slot]; }
	float headScanGainForTest(size_t slot) const { return headScanGain_[slot]; }
	float droneSwellCoeffForTest(size_t i) const { return strings_[i].swellCoeff; }
	float droneSoundingForTest(size_t i) const { return strings_[i].sounding; }
	double dronePitchForTest(size_t i) const { return strings_[i].pitch; }
	float droneLevelTargetForTest(size_t i) const { return strings_[i].levelTarget; }
	/// The Cycle's plucks so far (the last 64): the sample each landed on (counted from prepare), the string, the strength.
	int cyclePluckCountForTest() const { return pluckLogCount_; }
	/// Ring plucks the Cycle has made.
	int cycleRingPlucksForTest() const { return cycleRingPlucks_; }
	/// The last sample's drone (left, before the body) split by voice: the node strings' part and the scanned part.
	float droneVoiceLForTest() const { return lastDroneVoiceL_; }
	float droneScanLForTest() const { return lastDroneScanL_; }
	/// TEST ONLY: turn the Cycle's ring pluck off, to measure what it adds.
	void setCycleRingPluckForTest(bool on) { cycleRingPluckOn_ = on; }
	/// A KNOWN-BAD THE BREATHING CASE RUNS BESIDE THE REAL THING: every bridge deaf to the ring's orbit.
	void setOrbitOnBridgeForTest(bool on) { orbitOnBridge_ = on; }
	/// The ring's motion no longer drives the notes' strings (v15's known-bad: the bursts alone, v14).
	void setMotionDriveForTest(bool on) { motionDriveOn_ = on; }
	/// No drag on any string (v15's known-bad: a held on-pitch input rang them 25x past a pluck).
	void setDragForTest(bool on) { dragOn_ = on; }
	/// The motion drive uncapped (v15's known-bad: a hard click peaked 1.16..1.76).
	void setDriveCapForTest(bool on) { driveCapOn_ = on; }
	/// No feed bound on the drone's strings (v15's known-bad: a held root sine rang them to 9.3).
	void setFeedBoundForTest(bool on) { feedBoundOn_ = on; }
	/// The played notes' strings off the bridge (v18's known-bad: only the drone's strings ride it).
	void setNotesOnBridgeForTest(bool on) { notesOnBridge_ = on; }
	/// The notes' strings back on v21's 2 s hold (v22's known-bad and its Fixed 1 control).
	void setHoldFollowsForTest(bool on) {
		for (NodeString& v : voiceStrings_) {
			v.setHoldFollowsForTest(on);
		}
	}
	/// THE PATH BEFORE THE PERF PASS (backlog PB 2), for its equivalence case: the FIR summed one sample at a time and every string
	/// on NodeString's reference road (setReferenceForTest). The two must give the same bits.
	void setReferenceForTest(bool on) {
		referenceForTest_ = on;
		for (DroneString& d : strings_) {
			d.node.setReferenceForTest(on);
		}
		for (NodeString& v : voiceStrings_) {
			v.setReferenceForTest(on);
		}
	}
	/// The motion each head's string was driven by at the last sample, before its gain and velocity (0 for a released head).
	float headMotionDriveForTest(size_t slot) const { return headMotion_[slot]; }
	CyclePluck cyclePluckForTest(int i) const { return pluckLog_[static_cast<size_t>(i) % pluckLog_.size()]; }
	/// The body's peaks and the twin resonators as the DSP set them.
	bool bodyOnForTest() const { return bodyOn_; }
	bool twinOnForTest() const { return twinOn_; }
	int pluckCountForTest() const { return pluckCount_; }
	/// A head's per-sample phase increment, 0 for an idle slot. Slots kMaxHeads + i are the drone's strings.
	double headIncrementForTest(int slot) const;
	/// The key a live head plays, -1 for an idle slot.
	int headKeyForTest(int slot) const { return heads_[static_cast<size_t>(slot)].live ? heads_[static_cast<size_t>(slot)].key : -1; }
	float headGainForTest(int slot) const;
	int heldNotesForTest() const;
	bool restingForTest() const { return resting_; }
	/// **WHETHER THE STRING WILL SOUND AGAIN WITH NO NEW INPUT** — `AudioOps::holdsTail` (backlog MC): only the Cycle
	/// plucks on its own, and between its plucks the drone can fall to exact zero. A Bowed string with no note held and
	/// no input is moving but unread (no head, the drone silent), so it waits for an event, as a parked one does.
	/// It holds only while the Cycle would really pluck (`process`'s `cycleOn`, transport included) AND has a drone string
	/// to pluck (`cyclePluck` plucks nothing with none active).
	bool holdsTail() const;
	/// Whether the device is switched on — off, its output is the dry alone and nothing it holds is heard.
	bool enabled() const { return settings_.enabled; }
	float gateForTest() const { return gate_; }
	/// The kernel the NEXT window fades to, channel 0 or 1, `sizeTaps()` entries.
	const float* kernelForTest(int ch) const { return kernel_[1][ch].data(); }
	const float* kernelBeforeForTest(int ch) const { return kernel_[0][ch].data(); }
	int kernelTapsForTest() const { return kernelTaps_; }

	/// THE KERNEL RECIPE: the scan path rotated by `rotate` of a cycle, its mean removed, stretched over
	/// `length` taps (continuous: tapsFor(length) of them), Hann-windowed over that same length — so the
	/// kernel is a continuous function of Size — and normalised so its largest gain at ANY frequency is under 1 — the peak of a
	/// 2048-point spectrum, refined by a parabola, with a margin — so the filter path never boosts (all zero if
	/// the kernel is nil, or before prepare). PUBLIC ONLY SO A PROBE CAN FEED IT A PATH AND CHECK THE BOUND;
	/// it is not a reference implementation, and the FIR case restates the filter independently.
	void buildKernel(const float* path, float rotate, float length, float* out);

private:
	struct Head {
		bool held = false;   // a key is down on it (Poly) / it is the mono voice
		bool live = false;   // sounding or fading out
		bool ringing = false; // RELEASED IN RING: its key is up, and it sounds on until the string's gate has closed
		// RELEASED (v14; its key is up, in Gate or Ring): its scanned voice's gain may only FALL — under a HOLD, its gain at the
		// note-off decaying at the ring's own rate (the strings' T60) — so a new onset that lifts the shared gate cannot lift it;
		// and its node string is no longer driven and rings on its own loss, ungated (startRelease, the process loop). AND NEVER
		// BACK UP: scanCap is the gain it last used, so a gate that dips and recovers under the hold cannot lift it either (Feedback,
		// 2026-10-04: the GAIN only falls; the sound inside it may breathe with the bridge). The hold alone closes the head.
		bool released = false;
		float hold = 0.0f;
		float holdRef = 0.0f;
		float scanCap = 0.0f;
		float stringGain = 1.0f; // the tone's gain its string was heard under at its note-off, carried (v15: no step on release)
		float driveEnv = 0.0f; // its motion drive's envelope (peak, kDriveCapRelease): the cap's reading (v15)
		int key = 60;        // the key it plays, or (Mono) glides toward
		double semis = 60.0; // THE PITCH IT SOUNDS, before the octave/semitone/fine offset: Mono's glide moves it
		float drivePluck = 0.0f; // ITS SCAN BURST into its string (kDrivePluckSeconds), set at a ring pluck that is its own (v19)
		double bend = 0.0;   // ITS KEY'S TUNING EXPRESSION as it has reached it, in semitones (v18, MPE: glide)
		double phase = 0.0;
		double inc = 0.0;
		float gain = 0.0f;
		uint64_t onset = 0;  // for stealing the oldest
		// A STEAL IN PROGRESS: the head fades out fast, then starts `nextKey` (held if `nextHeld` still is).
		bool stealing = false;
		int nextKey = 60;
		bool nextHeld = false;
	};

	void restart();
	void applySettings(bool snap);
	void applyNotes();
	/// A NOTE-ON PLUCKS THE RING (v19): velocity^2 x pluckGain(Pluck), at the laws' pluck position, and bursts its own head's
	/// string by the same (keyBurst_, taken when its head starts).
	void notePluck(int key, float velocityGain);
	struct HeldKey {
		int key = 60;
		int16_t channel = -1;
		int32_t noteId = -1;
	};
	void physicalOn(const device::NoteEvent& e);
	/// Releases (or with `choke`, silences) every held key the event addresses; -1 is a wildcard (CLAP).
	void physicalOff(const device::NoteEvent& e, bool choke);
	void voiceOn(int key);
	void voiceOff(int key);
	/// A held_ entry carries this key (on any channel, under any id).
	bool keyHeld(int key) const;
	/// Silences at once every head sounding `key` that no held key keeps — except that a head fading `key`
	/// out under a steal starts its new key now instead.
	void chokeKey(int key);
	Head* allocate(int key);
	void startHead(Head& h, int key);
	/// A Voicing change: every head releases through its own ramp, then the held keys play in the new mode.
	void revoice();

	/// Both channels' next kernels (kernel_[1]) from the string's path, at kernelTaps_.
	void buildKernels();
	/// Both kernels built afresh and the window's fade collapsed onto them: at a restart, and when Blend
	/// leaves 0 after ticks that built none.
	void seatKernels();
	/// THE FILTER IS UNHEARD: Blend's ramp has landed on 0.
	bool blendIdle() const { return blend_.settled() && blend_.value() == 0.0f; }
	/// Size, in taps, clamped to the kernel's range: the length the next kernel is built at.
	float sizeLength() const {
		return std::isfinite(settings_.size) ? std::clamp(settings_.size, static_cast<float>(kMinTaps), static_cast<float>(kMaxTaps))
		                                     : static_cast<float>(kMinTaps);
	}
	void rebuildLaws();
	double semitonesOffset() const;
	double hzOf(double semis) const;
	void onTick();
	/// ANY NOTE HEAD SOUNDING OR FADING (the drone is not one: its gain is its follower's).
	bool anyNoteHeadLive() const;
	/// THE STRING IS STILL as the tone sees it: the gate shut at both ends of the window.
	bool ringStill() const { return gateTarget_ == 0.0f && gatePrev_ == 0.0f; }
	void publish();

	double sr_ = 48000.0;
	Settings settings_ {};
	bool restartPending_ = true;

	WeaveString ring_ {};
	owlet::dsp::PhiWeaveParams lawsA_ {};
	owlet::dsp::PhiWeaveParams lawsB_ {};
	float lawsCharacter_ = -1.0f;
	float lawsDecay_ = 0.0f;
	float lawsTension_ = 0.0f;
	// DRONE INPUT, ramped across each block toward the fader law (process()).
	float droneInputGain_ = 1.0f;
	// THE NOTES' STRINGS (v10): one a head, their blend and excite followed per sample, and the ring's orbit toward its
	// bridge — its bow departure over its held swing, read each tick and interpolated across the window (onTick()).
	std::array<NodeString, kMaxHeads> voiceStrings_ {};
	std::array<float, 128> keyVelocity_ {}; // each key's last note-on velocityGain, 1 until a note says otherwise (prepare)
	std::array<float, 128> keyTuning_ {};   // each key's tuning expression (semitones), 0 from its note-on (v18, MPE)
	std::array<float, 128> keyBurst_ {};    // a note-on's scan burst, waiting for its head to start (notePluck)
	float lastRingPluckAmp_ = 0.0f; // the last pluck the ring was handed (for the cases)
	float pickupApplied_ = 0.0f;
	float stringBlend_ = 0.0f;
	float stringExcite_ = 0.0f;
	float proxPrev_ = 0.0f;
	float proxTarget_ = 0.0f;
	float proxRef_ = 0.0f;
	float proximity_ = 0.0f;
	float lastVoiceStringL_ = 0.0f;
	int lawsModel_ = -1;

	automod::EnvelopeFollower follower_ {};
	// The last tick's input level, for the picture's meter (the follower's: what Drive bows the ring with).
	float meterLevel_ = 0.0f;
	int pluckCount_ = 0;
	float lastPluckPos_ = 0.0f;
	float pluckAge_ = kPluckAgeMax;

	std::array<Head, kMaxHeads> heads_ {};
	/// ONE OF THE DRONE'S STRINGS: a head at Root + its interval, its own harmonic ear on the input (resonate_ear.h),
	/// its own follower, a level that glides, and a fade that lets it retune without a jump.
	struct DroneString {
		bool active = false;
		bool retuning = false;  // fading out, to retune to `pending` (or to go, if `leaving`)
		bool leaving = false;
		int interval = 0;
		int pending = 0;
		double pitch = 0.0;        // semitones above Root in the tuning (fractional in Just)
		double pendingPitch = 0.0;
		// THE ROOT IT IS TUNED TO (v11: Root is automatable). A new Root retunes a sounding string IN PLACE (its node string
		// glides there as it rings); only a new note for the slot (a chord change) goes through the 10 ms retune fade.
		int root = 48;
		int pendingRoot = 48;
		double phase = 0.0;
		double inc = 0.0;
		double semis = 0.0;
		HarmonicEar ear {}; // its line is the device's (earLines_), attached by resetString
		NodeString node {}; // ITS VOICE (v11: a node string like the notes', where v6-v10 had a comb string)
		float t60 = 1.0f;       // its decay, seconds
		float brightness = 0.5f; // its loop low-pass, 0..1
		float swellCoeff = 1.0f; // Drone Attack: its swell's rise (the process loop)
		float swellFast = 0.0f; // the feed's presence envelope
		float swellSlow = 0.0f; // the swell, 0..1
		float voiceOut = 0.0f;  // its voice's last output
		float voiceEnv = 0.0f;  // a peak envelope of it, for the picture and the floor
		float voiceMs = 0.0f;   // its voice's mean square over kScanLevelPeriods: its own level, the scanned voice's (v12)
		float motion = 0.0f;    // the main string's motion as its scanned voice hears it: up no faster than its Attack (v14)
		float motionFall = 1.0f; // and down over its Decay's T60
		float levelOwn = 1.0f;  // that mean's one-pole coefficient, from its period (configureString)
		float feed = 0.0f;      // what its ear fed it at the last sample, before the swell (for the cases)
		float feedGain = 1.0f;  // its FEED BOUND (kDroneFeedBound, v15): 1 until its heard level passes it, then falling
		float outL = 0.0f;      // what it put on the drone's left, both voices, before Drone Out's body (for the cases)
		float sounding = 0.0f;  // what it sounds at, both voices: 0 under kDroneFloor
		float q = 8.0f;
		float level = 0.0f;
		float levelTarget = 0.0f;
		float fade = 1.0f;
		float detune = 0.0f;        // cents off its interval (a Tanpura's two unison Sa)
		float pendingDetune = 0.0f;
		float panL = 1.0f;          // equal-power, unity at the centre (Drone Width 0: exactly 1 and 1)
		float panR = 1.0f;
		float gain = 0.0f;
		// WHAT configureString LAST TUNED ITS EAR, LEVEL POLE AND NODE STRING FROM (backlog PS): the same inputs make the same
		// tunings, so it skips them (an exp, an atan2, a sin, a cos and a pow at every host call, for every string).
		bool tunedValid = false;
		int tunedCap = 0;
		double tunedHz = 0.0;
		double tunedSr = 0.0;
		double tunedStiffness = 0.0;
		float tunedQ = 0.0f;
		float tunedT60 = 0.0f;
		float tunedBrightness = 0.0f;
	};
	/// The chord's strings for this block: activate, retune or release each slot; set pitch, band and level.
	void tuneDrone(const DroneResponse& resp, double offset);
	/// A string's pitch and ear from its interval and its own Q.
	void configureString(DroneString& d, double offset);
	bool anyDroneSounding() const;
	std::array<DroneString, kMaxDroneStrings> strings_ {};
	// droneResponse()'s memo and what it was made from.
	DroneResponse droneResp_ {};
	bool droneRespValid_ = false;
	int droneRespChord_ = 0;
	int droneRespTuning_ = 0;
	float droneRespCharacter_ = 0.0f;
	float droneRespAttack_ = 0.0f;
	float droneRespDecay_ = 0.0f;
	/// A string back to silence: fresh state, its ear on its own line and cleared.
	void resetString(size_t k);
	/// EVERY STRING'S EAR LINE AND VOICE LINE, a block each of kMaxDroneStrings x earLength_, allocated in prepare().
	std::vector<float> earLines_ {};
	int earLength_ = 0;
	/// THE BRIDGE (v18): its displacement now and a sample ago (see kBridgeMassBottom), its mass this sample (0 rigid), and
	/// whether the strings ride it (until a rigid bridge is back at rest).
	double bridgeW_ = 0.0;
	double bridgeWPrev_ = 0.0;
	double bridgeMass_ = 0.0;
	double bridgeCouplingNotes_ = 1.0; // each group's lever onto the bridge this sample (v21)
	double bridgeCouplingDrone_ = 1.0;
	float bridgeSympathyAt_ = -1.0f; // the knobs the three above were computed at
	float bridgeDroneSympathyAt_ = -1.0f;
	bool bridgeYields_ = false;
	// THE CYCLE (the self-playing drone): a countdown to the next pluck, the pattern's step, a sample
	// clock and a log of the plucks (for the cases).
	bool ringRelease() const { return settings_.headRelease == static_cast<int>(HeadRelease::Ring); }
	/// A HEAD LET GO (v14, Gate and Ring): its hold starts at its gain now (Head::hold); in Ring it rings on until the hold or
	/// the ring has fallen away.
	void startRelease(Head& h);
	float lastToneGain_ = 0.0f; // the motion gate x Sustain at the last sample: a released head's hold starts from it
	std::array<float, kMaxHeads> headOutL_ {}; // each head's part of the tone's left at the last sample, after its gains (the cases)
	std::array<float, kMaxHeads> headScanGain_ {}; // each head's scanned-voice gain at the last sample (the cases: a gain may only fall)
	std::array<float, kMaxHeads> headMotion_ {};
	/// A RINGING HEAD CLOSES FOR GOOD once its hold has fallen this far under its level at the note-off (-60 dB).
	static constexpr float kRingCloseLevel = 1.0e-3f;
	/// RING DECAY'S DAMPING RANGE: the laws builder's ceiling, and a quarter of its floor (rebuildLaws says why).
	static constexpr float kRingDampMin = 0.0015f;
	static constexpr float kRingDampMax = 0.09f;
	/// RUNNING: a division chosen and a Cycle Level above 0 (at 0 it plucks nothing, so it is off, and the device can rest).
	bool cycleRunning() const { return cyclePeriodBeats(settings_.droneCycleSync) > 0.0 && cycleGain() > 0.0f; }
	/// CYCLE LEVEL AS A GAIN: the fader law, (2 x level)^2, unity at the centre.
	float cycleGain() const {
		const float v = std::isfinite(settings_.droneCycleLevel) ? std::clamp(settings_.droneCycleLevel, 0.0f, 1.0f) : 0.0f;
		return (2.0f * v) * (2.0f * v);
	}
	/// THE HOST'S CLOCK FOR THIS BLOCK (setTransportClock), and the grid index of the last pluck while locked to it.
	device::TransportClock clock_ {};
	double cycleNextIndex_ = 0.0; // the next grid index due while locked (v11)
	bool cycleHasIndex_ = false;
	// A CYCLE PLUCK, LED (v11): it may start `lead` samples BEFORE its grid sample so that it PEAKS on the grid, its ring
	// pluck, follower kick and log entry landing ON the grid, `lead` samples later (deliverCyclePluck). pluckLead() is that
	// lead: half a period while the drone's voice was a comb string fed a one-period bump, 0 since its strings are node
	// strings plucked by displacement, which peak on the sample they are plucked (NodeString::pluck). The schedule keeps
	// the lead, so a pluck that needs one again has it.
	void cyclePluck(int lead = 0);
	void deliverCyclePluck();
	int pluckLead() const;
	struct PendingPluck {
		int countdown = -1; // samples until its grid sample, -1 none
		int string = 0;
		float velocity = 0.0f;
	};
	PendingPluck pendingPluck_ {};
	uint64_t lastPluckAt_ = ~uint64_t { 0 }; // the sample the last Cycle pluck landed on
	int cycleCountdown_ = 0;
	int cycleStep_ = 0;
	uint64_t sampleClock_ = 0;
	std::array<CyclePluck, 64> pluckLog_ {};
	int pluckLogCount_ = 0;
	int cycleRingPlucks_ = 0;
	bool cycleRingPluckOn_ = true;
	float lastDroneVoiceL_ = 0.0f;
	float lastDroneScanL_ = 0.0f;
	// THE GOURD BODY (three peaks, both channels) AND THE TWIN GOURDS (a band-pass each side), their coefficients set
	// once a block from the Character (updateBody), skipped entirely at Body 0 and Twin 0.
	struct Biquad {
		float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
		float z1[2] {}, z2[2] {};
		float run(float x, int ch);
	};
	void updateBody();
	void clearBody();
	// [channel][peak]: each channel its own coefficients (the twin gourds differ left from right), each path its own
	// state — the drone's and the notes' (v11: a body colours what is played on its instrument).
	using BodyBank = std::array<std::array<Biquad, 3>, 2>;
	void bodyStep(float& l, float& r, BodyBank& bank, const float (&makeup)[2]);
	BodyBank bodyDrone_ {};
	BodyBank bodyTone_ {};
	float bodyMakeup_[2] { 1.0f, 1.0f };
	float bodyToneMakeup_[2] { 1.0f, 1.0f };
	bool bodyOn_ = false;
	bool twinOn_ = false;
	double bodyKey_[4] { -1.0, -1.0, -1.0, -1.0 }; // the coordinate, Body, Twin and drone signature the body was built for
	HeldKey held_[kMaxHeldKeys] {};
	int heldCount_ = 0;
	/// The head Mono plays on, -1 for none yet.
	int monoSlot_ = -1;
	int lastVoicing_ = -1;
	/// Morph at the last tick, and where it stood when the device parked: Morph MOTION excites the string.
	float lastTickMorph_ = 0.0f;
	float parkedMorph_ = 0.0f;

	uint64_t onsetCounter_ = 0;

	device::NoteEvent queue_[kMaxQueuedNotes] {};
	int queued_ = 0;

	/// The fx lanes' tap, or null — see `setLaneTap`.
	const LaneTap* tap_ = nullptr;

	// Ramps, per sample: 5 ms each.
	dyn::LinearRamp morph_ {}, blend_ {}, levelGain_ {}, mix_ {}, width_ {}, enable_ {}, sustain_ {}, droneOut_ {}, sympathy_ {}, droneSympathy_ {};
	/// Every ramp, for the loops that treat them alike; applySettings' targets are in this order.
	dyn::LinearRamp* const ramps_[10] = { &morph_, &blend_, &levelGain_, &mix_, &width_, &enable_, &sustain_, &droneOut_, &sympathy_, &droneSympathy_ };

	// The tone's gate: from the energy at the last tick to the energy at this one, across the window.
	float gate_ = 0.0f;
	float gatePrev_ = 0.0f;
	float gateTarget_ = 0.0f;
	float gateEnergy_ = 0.0f;
	bool gateLatched_ = false;
	bool excitedThisWindow_ = false;
	bool playedThisWindow_ = false;
	bool played_ = false;
	bool orbitOnBridge_ = true;
	bool motionDriveOn_ = true;
	bool dragOn_ = true;
	bool driveCapOn_ = true;
	bool feedBoundOn_ = true;
	bool notesOnBridge_ = true;
	bool referenceForTest_ = false;

	// The FIR: [0] the kernel the window fades from, [1] the one it fades to; per channel.
	std::array<std::array<std::array<float, kMaxTaps>, 2>, 2> kernel_ {};
	/// THE NEXT KERNEL'S LENGTH (Size, continuous) and its tapsFor().
	float kernelLength_ = 64.0f;
	int kernelTaps_ = 64;
	/// The taps of the kernel the window fades FROM: when Size shrinks, its dropped taps fade out too.
	int kernelTapsFrom_ = 64;
	/// BLEND SAT AT 0 at a tick, so no kernel was built: the filter's kernels are rebuilt from the string
	/// the moment Blend moves off 0, before it is heard.
	bool kernelStale_ = false;
	/// MIRRORED: each input sample is written at pos and pos + kHistory, so the last `taps` samples before
	/// pos + kHistory are contiguous and the FIR reads them without wrapping.
	std::array<std::array<float, 2 * kHistory>, 2> history_ {};
	int historyPos_ = 0;
	/// THE FIR AHEAD (backlog PB 2): between two ticks the kernels stand still, so the filter's next kFirAhead samples are
	/// computed together, four samples to an SSE register — each lane its own sample's sum, in the per-sample loop's order,
	/// so the same bits. `firAhead` writes those samples' history (the first's is written already) and their two sums.
	static constexpr int kFirAhead = 64;
	alignas(16) std::array<std::array<float, kFirAhead>, 2> firFrom_ {};
	alignas(16) std::array<std::array<float, kFirAhead>, 2> firTo_ {};
	void firAhead(const float* left, const float* right, int count);

	// THE KERNEL'S PEAK FINDER: a real 2048-point pffft, set up in prepare, with its buffers here rather than
	// on the audio thread's stack.
	static constexpr int kPeakFftSize = 2048;
	PFFFT_Setup* fft_ = nullptr;
	alignas(64) std::array<float, kPeakFftSize> fftIn_ {};
	alignas(64) std::array<float, kPeakFftSize> fftOut_ {};
	alignas(64) std::array<float, kPeakFftSize> fftWork_ {};
	float peakMagnitude(const float* kernel, int taps);

	std::array<WeaveString::Fade, WeaveString::kMaxTickLength> fade_ {};

	int idleSamples_ = 0;
	bool resting_ = false;
	int restPublishCount_ = 0;

	device::ReadingsSink sink_ {};
	std::array<float, kReadingCount> packed_ {};
};

/// THE DSP INSIDE A RESONATE DEVICE'S STORAGE, for a case that drives it through a host (the rack) and reads what it did.
const ResonateDsp& dspForTest(const void* storage);
ResonateDsp& dspForTest(void* storage);

} // namespace feathers::fx::resonate
