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

#include "fx/shaper/shaper_types.h"

#include <algorithm>

namespace feathers::fx::shaper {

namespace {

/// THE FIELD, OR ITS DECLARED DEFAULT — the one place absence is turned into a value.
///
/// device_row.h's rule, in the form every device implements it: a row carries `fieldsPresent`
/// entries and the rest are ABSENT, not zero. It matters here more than usual because zero is a
/// legal and meaningful value for all fourteen of these controls and means something different for
/// each — a shaper decoded with absent fields at zero would come back with Mix at zero, which
/// shapes nothing, and Knee at zero, which is a bypass, rather than at 1.0 and 0.5.
[[nodiscard]] float fieldOr(const Row& row, size_t fieldsPresent, Param p) {
	const auto i = static_cast<size_t>(p);
	return i < fieldsPresent ? row[i] : static_cast<float>(kParamDescs[i].def);
}

[[nodiscard]] bool switchOr(const Row& row, size_t fieldsPresent, Param p) {
	return fieldOr(row, fieldsPresent, p) >= 0.5f;
}

} // namespace

Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = switchOr(row, fieldsPresent, Param::Enabled);
	// CLAMPED ON THE WAY IN, not trusted. A row is whatever reached the transport — a host's
	// automation, a file, or a modulation offset that a device declaring `offsetsMayLeaveRange` would
	// be allowed to send. None of these six has a meaning outside 0..1, and `deriveCurve` reading a
	// colour of 1.4 would index a triangle nobody designed.
	s.knee = std::clamp(fieldOr(row, fieldsPresent, Param::Knee), 0.0f, 1.0f);
	// **COLOUR IS NOT ONE OF THEM EITHER, AS OF 2026-09-09 — none of these should be
	// bounded.** The clamp above was written when Colour was a bounded 0..1 axis with a separate
	// counter to lap into; the counter is gone and Colour carries the quantity, so this line would be
	// the same defect the paragraph below describes, on the very next control.
	//
	// **AND `deriveCurve` NO LONGER READS A COLOUR OF 1.4 AS "a triangle nobody designed"**, which was
	// the stated reason for the bound. The position and the ACCELERATION were split there: the
	// position drives the triangles and has no ends, while `freqMult` saturates — so an unbounded
	// colour reduces mod 1 like any other phase instead of growing cubically. The guard moved to
	// where the danger actually was.
	//
	// THE CASE THAT CAUGHT THIS ONE was the declaration-versus-decode case immediately below, run
	// against `Colour` after the widening: it read `1` where it asked for `65`.
	s.colour = fieldOr(row, fieldsPresent, Param::Colour);
	// **GAMMA IS NOT ONE OF THEM, AND THE CLAMP THAT SAID IT WAS SILENCED THE CONTROL FOR SIXTY-FOUR
	// SIXTY-FIFTHS OF ITS TRAVEL.** `Param::Gamma` was widened to +-2^24 — the standing rule that the
	// field is infinite too — and the same change did not come here, so the DECLARATION was
	// unbounded and the DECODE was not. The readout moved, because the param really was unbounded;
	// the sound and the drawn curve did not, because both are built from `Settings` and this line
	// pinned `Settings::gamma` at 1.0. `GammaBox` gives sixteen units to a full drag, so about nine
	// pixels reached the wall and everything past it was the same table.
	//
	// THE ENGINE NEVER WANTED THE BOUND. `curveFor` sums `gamma + colourPhase` into a PHASE and
	// `deriveCurve` reduces it mod 1 in double — the firmware's own comment puts the safe ceiling at
	// 10^15, which is a double's, and 2^24 is nowhere near it. A phase has no ends; that is the whole
	// reason the declaration is symmetric.
	//
	// **AND THIS IS THE SHAPE TO LOOK FOR RATHER THAN THE INSTANCE**: a widening applied to a
	// declaration and not to its decode leaves a control that READS unbounded and SOUNDS bounded, and
	// nothing in either file mentions the other. `s.rotate` eleven lines below survived the same
	// commit correctly; the registry-wide round-trip case is what now asks the question of every
	// control in every device rather than of the one somebody noticed.
	s.gamma = fieldOr(row, fieldsPresent, Param::Gamma);
	// NOT CLAMPED TO A ZONE OF ANYTHING — the offset is a phase and its declared range is a window on
	// the value box, not a limit on the quantity.
	//
	// **AND NOT FLOORED AT ZERO EITHER, WHICH IT WAS.** The floor's stated reason — *"`wrapPhase`
	// takes a magnitude and a negative lap is the firmware's own floor"* — is false about the
	// function it names. `phi_triangle.hpp:81` computes a true floor-mod and its own comment says so
	// outright: *"For negative values, truncation goes toward zero (not floor), so we subtract 1."* A
	// negative lap wraps exactly as a positive one does.
	//
	// So this was a second wall of the gamma clamp's kind, one line below it, and it is the one that
	// would have stopped a hand lapping BACKWARDS from the 0 every patch starts at — which is the
	// whole reason the declaration next door is symmetric rather than 0..N.
	s.drive = std::clamp(fieldOr(row, fieldsPresent, Param::Drive), 0.0f, 1.0f);
	s.threshold = std::clamp(fieldOr(row, fieldsPresent, Param::Threshold), 0.0f, 1.0f);
	s.inputHpf = std::clamp(fieldOr(row, fieldsPresent, Param::InputHpf), static_cast<float>(kInputFilterMinLog2),
	                        static_cast<float>(kInputFilterMaxLog2));
	s.inputLpf = std::clamp(fieldOr(row, fieldsPresent, Param::InputLpf), static_cast<float>(kInputFilterMinLog2),
	                        static_cast<float>(kInputFilterMaxLog2));
	s.dryWet = std::clamp(fieldOr(row, fieldsPresent, Param::DryWet), 0.0f, 1.0f);
	s.emphasisDb = std::clamp(fieldOr(row, fieldsPresent, Param::Emphasis), static_cast<float>(-kEmphasisMaxDb),
	                          static_cast<float>(kEmphasisMaxDb));
	s.hysteresis = std::clamp(fieldOr(row, fieldsPresent, Param::Hysteresis), 0.0f, 1.0f);
	s.sub = std::clamp(fieldOr(row, fieldsPresent, Param::Sub), 0.0f, 1.0f);
	s.feedback = std::clamp(fieldOr(row, fieldsPresent, Param::Feedback), 0.0f, 1.0f);
	s.mangle = std::clamp(fieldOr(row, fieldsPresent, Param::Mangle), 0.0f, 1.0f);
	s.rotate = fieldOr(row, fieldsPresent, Param::Rotate);
	s.aggression = fieldOr(row, fieldsPresent, Param::Aggression);
	s.trim = fieldOr(row, fieldsPresent, Param::Trim);
	s.lowpass = std::clamp(fieldOr(row, fieldsPresent, Param::Lowpass), 0.0f, 1.0f);
	s.integrator = std::clamp(fieldOr(row, fieldsPresent, Param::Integrator), 0.0f, 1.0f);
	s.deadzone = std::clamp(fieldOr(row, fieldsPresent, Param::Deadzone), 0.0f, 1.0f);
	s.bloom = std::clamp(fieldOr(row, fieldsPresent, Param::Bloom), 0.0f, 1.0f);
	s.sag = std::clamp(fieldOr(row, fieldsPresent, Param::Sag), 0.0f, 1.0f);
	s.loop = std::clamp(fieldOr(row, fieldsPresent, Param::Loop), 0.0f, 1.0f);
	s.cheby = std::clamp(fieldOr(row, fieldsPresent, Param::Cheby), -1.0f, 1.0f);
	s.stair = std::clamp(fieldOr(row, fieldsPresent, Param::Stair), 0.0f, 1.0f);
	s.subOn = switchOr(row, fieldsPresent, Param::SubOn);
	s.feedbackOn = switchOr(row, fieldsPresent, Param::FeedbackOn);
	s.mangleOn = switchOr(row, fieldsPresent, Param::MangleOn);
	s.lowpassOn = switchOr(row, fieldsPresent, Param::LowpassOn);
	s.integratorOn = switchOr(row, fieldsPresent, Param::IntegratorOn);
	s.deadzoneOn = switchOr(row, fieldsPresent, Param::DeadzoneOn);
	// COLOUR'S MODE, ROUNDED AND CLAMPED INTO THE CHOICE (backlog IW) — a row is what a patch load hands over.
	s.colourMode = static_cast<uint32_t>(
	    std::clamp(std::lround(fieldOr(row, fieldsPresent, Param::ColourMode)), 0L, static_cast<long>(kColourFree)));
	s.inflateAmount = std::clamp(fieldOr(row, fieldsPresent, Param::InflateAmount), 0.0f, 1.0f);
	s.poly = switchOr(row, fieldsPresent, Param::Poly);
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Knee)] = s.knee;
	row[static_cast<size_t>(Param::Colour)] = s.colour;
	row[static_cast<size_t>(Param::Gamma)] = s.gamma;
	row[static_cast<size_t>(Param::Drive)] = s.drive;
	row[static_cast<size_t>(Param::Threshold)] = s.threshold;
	row[static_cast<size_t>(Param::InputHpf)] = s.inputHpf;
	row[static_cast<size_t>(Param::InputLpf)] = s.inputLpf;
	row[static_cast<size_t>(Param::DryWet)] = s.dryWet;
	row[static_cast<size_t>(Param::Poly)] = s.poly ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Emphasis)] = s.emphasisDb;
	row[static_cast<size_t>(Param::Hysteresis)] = s.hysteresis;
	row[static_cast<size_t>(Param::Sub)] = s.sub;
	row[static_cast<size_t>(Param::Feedback)] = s.feedback;
	row[static_cast<size_t>(Param::Mangle)] = s.mangle;
	row[static_cast<size_t>(Param::Rotate)] = s.rotate;
	row[static_cast<size_t>(Param::Aggression)] = s.aggression;
	row[static_cast<size_t>(Param::Trim)] = s.trim;
	row[static_cast<size_t>(Param::Lowpass)] = s.lowpass;
	row[static_cast<size_t>(Param::Integrator)] = s.integrator;
	row[static_cast<size_t>(Param::Deadzone)] = s.deadzone;
	row[static_cast<size_t>(Param::Bloom)] = s.bloom;
	row[static_cast<size_t>(Param::Sag)] = s.sag;
	row[static_cast<size_t>(Param::Loop)] = s.loop;
	row[static_cast<size_t>(Param::Cheby)] = s.cheby;
	row[static_cast<size_t>(Param::Stair)] = s.stair;
	row[static_cast<size_t>(Param::SubOn)] = s.subOn ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::FeedbackOn)] = s.feedbackOn ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::MangleOn)] = s.mangleOn ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::LowpassOn)] = s.lowpassOn ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::IntegratorOn)] = s.integratorOn ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::DeadzoneOn)] = s.deadzoneOn ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::ColourMode)] = static_cast<float>(s.colourMode);
	row[static_cast<size_t>(Param::InflateAmount)] = s.inflateAmount;
	return row;
}

} // namespace feathers::fx::shaper
