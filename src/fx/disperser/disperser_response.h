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

#include "fx/disperser/disperser_types.h"

#include <array>
#include <complex>

// **WHAT THE DISPERSER'S PICTURE IS — A PURE FUNCTION OF THE SETTINGS, ON THE MESSAGE THREAD.**
//
// The Disperser's character IS its group delay: a transient comes out smeared into a chirp whose shape
// is the delay-against-frequency curve. So the card plots that curve, and computes it from the SAME
// `layoutStages` and `allpassCoeffs` the engine runs — not from a snapshot of audio-thread state, and
// not from a restatement that could drift.
//
// WHAT IT SHOWS, HONESTLY:
//   - the group delay of the WHOLE ROUTING for a centred (mono) input, per output channel — Parallel's
//     sum, Nested's half-dry feed, the Ladder/Cross/Spring blends and the emphasis gains included;
//   - the stages AT REST: the smoothers landed, both stereo LFOs at their centre (they wobble the L/R
//     split by at most ±0.1 octave), the emphasis gains at their targets.
// WHAT IT DOES NOT: the echoes (Punch and Chirp write into a delay line — `echoMs` says where, the curve
// does not draw them), and the saturations (they are 30 dB and more above full scale).

namespace feathers::fx::disperser {

struct ResponseModel {
	Topology topology = Topology::Cascade;
	StageLayout layout {};
	std::array<AllpassCoeffs, kMaxStages> coeffsL {};
	std::array<AllpassCoeffs, kMaxStages> coeffsR {};
	std::array<double, kMaxStages> ladderCross {};
	/// **THE EMPHASIS GAINS DRAWN — the engine's own when it has published them** (`Reading::StageGain0`),
	/// the targets otherwise. They are not a function of the settings: see `Reading::StageGain0`.
	std::array<float, kMaxStages> gains {};
	double crossGain = 0.4;
	double sampleRate = 48000.0;
	double centerHz = 0.0;
	/// The centre the Freq handle marks: the knob's hertz, or the tracked note's times 2^Offset.
	double markerHz = 0.0;
	bool tracking = false; ///< Key Track on AND a note heard
	/// Where Punch and Chirp write, ms — the doubled centre stage's delay, folded (`processDisperser`).
	double echoMs = 0.0;
	TopoParams topo {};
	TwistParams twist {};
	Routing routing {};
};

[[nodiscard]] ResponseModel responseModelOf(const Settings& s, bool haveNote, double noteSemitones,
                                            double sampleRate) noexcept;
/// THE ENGINE'S GAINS INTO A MODEL, `kMaxStages` of them.
void useStageGains(ResponseModel& m, const float* gains) noexcept;

/// THE ROUTING'S COMPLEX RESPONSE to a mono input at `hz`, per output channel.
[[nodiscard]] std::array<std::complex<double>, 2> routingResponse(const ResponseModel& m, double hz) noexcept;

/// THE GROUP DELAY at `hz`, per output channel, in milliseconds — `-dφ/dω` by a central difference of
/// the routing's phase.
[[nodiscard]] std::array<double, 2> groupDelayMs(const ResponseModel& m, double hz) noexcept;

} // namespace feathers::fx::disperser
