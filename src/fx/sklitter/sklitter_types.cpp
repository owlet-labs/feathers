/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "fx/sklitter/sklitter_types.h"

// FOR `Corner` ALONE — kCfg1, kCfg2, kCfg3, kCfg4. The perform arrays are indexed by it and naming the enumerator
// at every use is what keeps `Settings`, `Param` and `perform.h` in ONE order rather than three that
// happen to agree today.
#include "fx/sklitter/perform.h"
// FOR `loopSlicesFor` AND `rateDivisionFor` — the Rate readout's two rules, shared with the engine.
#include "fx/sklitter/engage.h"
#include "fx/sklitter/slice_plan.h"

#include <cstdio>
#include <algorithm>
#include <iterator>
#include <cmath>

namespace feathers::fx::sklitter {

// **THE `Corner`-WIDTH ASSERTS THAT USED TO SIT HERE ARE IN `perform.h` NOW, AND THE INCLUDE ABOVE IS WHY
// THEY STILL COVER THIS FILE.** They were here while this was the only translation unit holding both
// declarations — the enum in `perform.h`, the storage in `sklitter_types.h`. Task 6 gave `perform.h` the
// types (`performPushOf` reads `Settings`), so the header that indexes by `Corner` can assert its own
// precondition; every TU that includes it, this one included, gets the check instead of just this one.
//
// **THE DIRECTION IS STILL ONE-WAY AND STILL PART OF THE DESIGN:** `sklitter_types.h` must stay free of
// `perform.h`. The types header is what every card, the registry and the tests include, and the circle's
// maths depends on the types rather than the other way round.

namespace {

/// A CHOICE ARRIVES AS A FLOAT AND MUST LAND ON A VALID ENUMERATOR. Rounding alone is not enough:
/// automation and modulation can both hand over a value outside the declared range, and an index
/// past the end reads a mode that does not exist.
inline uint8_t choiceIndex(float value, uint8_t count) { // not constexpr: std::lround is not, on MSVC
	const long rounded = std::lround(value);
	if (rounded < 0) {
		return 0;
	}
	if (rounded >= static_cast<long>(count)) {
		return static_cast<uint8_t>(count - 1);
	}
	return static_cast<uint8_t>(rounded);
}

/// A SWITCH IS TRUE ABOVE THE HALFWAY POINT, which is what a 0..1 stepped control means when a host
/// automates it with a ramp rather than with two values.
constexpr bool switchOf(float value) { return value >= 0.5f; }

constexpr size_t idx(Param p) { return static_cast<size_t>(p); }

} // namespace

Settings stateOf(const Row& row) {
	Settings s;
	s.zoneA = row[idx(Param::ZoneA)];
	s.zoneB = row[idx(Param::ZoneB)];
	// `Macro Cfg` and `Macro` were decoded here and are DELETED (2026-09-19): the perform circle's centre
	// is a zero push, so there is no knob pair left to read. The four waypoints below are the macro now.
	s.pWrite = row[idx(Param::PWrite)];
	s.density = row[idx(Param::Density)];

	s.rate = row[idx(Param::Rate)];
	s.enabled = switchOf(row[idx(Param::Enabled)]);
	s.engage = switchOf(row[idx(Param::Engage)]);
	s.mode = static_cast<Mode>(choiceIndex(row[idx(Param::Mode)], static_cast<uint8_t>(Mode::kCount)));

	s.quantized = switchOf(row[idx(Param::Quantized)]);

	s.gamma = row[idx(Param::Gamma)];

	s.pitchScale = row[idx(Param::PitchScale)];
	s.stereoWidth = row[idx(Param::StereoWidth)];

	// **THE LADDERS ARE DECODED HERE AND NOWHERE ELSE.** `choiceIndex` is the same clamp every other
	// stepped control on this device goes through, so a host ramping the param lands on a rung rather
	// than between two of them.
	s.seqBars = kSeqLengthBars[choiceIndex(row[idx(Param::SeqLength)],
	                                      static_cast<uint8_t>(std::size(kSeqLengthBars)))];
	s.bufferBars = kBufferLengthBars[choiceIndex(row[idx(Param::BufferLength)],
	                                             static_cast<uint8_t>(std::size(kBufferLengthBars)))];
	s.transportLocked = switchOf(row[idx(Param::TransportLocked)]);

	// **DECODED FROM THE LOG HERE, ONCE.** `Unit::LogSeconds` holds log2(seconds), and the bottom of
	// the range means NO TIME AT ALL — so the floor becomes a hard zero rather than 0.1 ms, which is
	// what lets the engine test `> 0` instead of every reader knowing the convention.
	const double logAttack = row[idx(Param::MinEnv)];
	s.minEnvSeconds =
	    logAttack <= kMinEnvFloor + 1e-6 ? 0.0f : static_cast<float>(std::exp2(logAttack));
	s.mix = std::clamp(row[idx(Param::Mix)], 0.0f, 1.0f);
	// **UNCLAMPED ON PURPOSE** — `overriddenProbability` clamps it where it is read, so a modulated
	// value out of range is bounded once rather than in two places free to disagree.
	s.sendOverride = row[idx(Param::SendOverride)];
	// INPUT GAIN, CLAMPED to its declaration: it scales what the ring records, and a route can carry it past ±24.
	s.inputGainDb = std::clamp(row[idx(Param::InputGain)], -24.0f, 24.0f);
	s.freeze = switchOf(row[idx(Param::Freeze)]); // backlog KX: a route crossing 0.5 freezes
	s.recordGate = std::clamp(row[idx(Param::RecordGate)], 0.0f, 1.0f); // backlog MJ: 0 is off

	// **THE TWO CONTINUOUS WRITERS, CLAMPED HERE — backlog GU/GV.** Unlike `sendOverride` above there
	// is no downstream owner of the bound: these two are blend amounts consumed directly by
	// `ring_.writeAt`'s crossfade, and a value outside [0, 1] there is not a loud effect, it is an
	// AMPLIFYING one — `old + (src − old)·1.4` extrapolates past the source and the loop diverges.
	// Neither is a modulation destination today, so nothing can currently deliver one; the clamp is
	// what makes that a fact about the arithmetic rather than about today's routing.
	s.dryWrite = std::clamp(row[idx(Param::DryWrite)], 0.0f, 1.0f);
	s.feedback = std::clamp(row[idx(Param::Feedback)], 0.0f, 1.0f);
	s.feedbackPostMaster = switchOf(row[idx(Param::FeedbackTap)]);
	s.grainPWriteLast = switchOf(row[idx(Param::GrainOrder)]); // AUDITION, see Param::GrainOrder
	// HELD AS THE CONTROLS — `Settings::grooveFeel` owns the clamp and the de-NaN, at the one place they
	// are decoded (backlog IO: a mode and an amount).
	s.grooveMode = row[idx(Param::GrooveMode)];
	s.grooveAmount = row[idx(Param::GrooveAmount)];
	s.freePhase = row[idx(Param::FreePhase)];

	// **THE PERFORM CIRCLE, IN `Corner` ORDER — N, E, S, W** (`perform.h`). One line per waypoint term
	// rather than a loop, because the enum's four are declared and a loop over them would be arithmetic
	// on a `Param` value: the order would then be an assumption instead of something written down.
	//
	// A `cornerMacro` block stood above this one and is deleted with its four controls (2026-09-19): a
	// waypoint is a configuration, its push is built at macro 1.0, and the puck's radius is the amount.
	s.cornerCfg[kCfg1] = row[idx(Param::Cfg1)];
	s.cornerCfg[kCfg2] = row[idx(Param::Cfg2)];
	s.cornerCfg[kCfg3] = row[idx(Param::Cfg3)];
	s.cornerCfg[kCfg4] = row[idx(Param::Cfg4)];

	// **THE PUCK IS NOT CLAMPED HERE, AND THAT IS DELIBERATE.** `performWeightsOf` clamps to the unit
	// disc and turns a non-finite coordinate into the centre — it owns that contract and it is tested
	// against it. A second clamp here would be a second road to the same rule, and the two would be
	// free to disagree the day one of them is edited.
	s.perfX = row[idx(Param::PerfX)];
	s.perfY = row[idx(Param::PerfY)];
	// Depth IS clamped, the way `mix` above is: it scales a blend, nothing downstream bounds it, and a
	// modulated depth can arrive outside its declared range.
	s.perfDepth = std::clamp(row[idx(Param::PerfDepth)], 0.0f, 1.0f);
	return s;
}

Row rowOf(const Settings& s) {
	Row row {};
	row[idx(Param::ZoneA)] = s.zoneA;
	row[idx(Param::ZoneB)] = s.zoneB;
	row[idx(Param::PWrite)] = s.pWrite;
	row[idx(Param::Density)] = s.density;

	row[idx(Param::Rate)] = s.rate;
	row[idx(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[idx(Param::Engage)] = s.engage ? 1.0f : 0.0f;
	row[idx(Param::Mode)] = static_cast<float>(static_cast<uint8_t>(s.mode));

	row[idx(Param::Quantized)] = s.quantized ? 1.0f : 0.0f;

	row[idx(Param::Gamma)] = s.gamma;

	row[idx(Param::PitchScale)] = s.pitchScale;
	row[idx(Param::StereoWidth)] = s.stereoWidth;

	// **BACK TO A RUNG, BY SEARCHING THE LADDER RATHER THAN BY ARITHMETIC.** The buffer ladder is not
	// uniform (0.25, 0.5, 1, 2, 4), so there is no divide that inverts it; and a rung that is not on
	// the ladder at all can only come from a corrupt row, which falls back to the default rather than
	// indexing past the end.
	row[idx(Param::SeqLength)] = static_cast<float>(kSeqLengthDefault);
	for (size_t i = 0; i < std::size(kSeqLengthBars); ++i) {
		if (kSeqLengthBars[i] == s.seqBars) {
			row[idx(Param::SeqLength)] = static_cast<float>(i);
			break;
		}
	}
	row[idx(Param::MinEnv)] =
	    s.minEnvSeconds > 0.0f ? static_cast<float>(std::log2(s.minEnvSeconds))
	                              : static_cast<float>(kMinEnvFloor);
	row[idx(Param::Mix)] = s.mix;
	row[idx(Param::SendOverride)] = s.sendOverride;
	row[idx(Param::InputGain)] = s.inputGainDb;
	row[idx(Param::Freeze)] = s.freeze ? 1.0f : 0.0f;
	row[idx(Param::RecordGate)] = s.recordGate;
	row[idx(Param::DryWrite)] = s.dryWrite;
	row[idx(Param::Feedback)] = s.feedback;
	row[idx(Param::FeedbackTap)] = s.feedbackPostMaster ? 1.0f : 0.0f;
	row[idx(Param::GrainOrder)] = s.grainPWriteLast ? 1.0f : 0.0f;
	row[idx(Param::GrooveMode)] = s.grooveMode;
	row[idx(Param::GrooveAmount)] = s.grooveAmount;
	row[idx(Param::FreePhase)] = s.freePhase;
	row[idx(Param::BufferLength)] = static_cast<float>(kBufferLengthDefault);
	for (size_t i = 0; i < std::size(kBufferLengthBars); ++i) {
		if (kBufferLengthBars[i] == s.bufferBars) {
			row[idx(Param::BufferLength)] = static_cast<float>(i);
			break;
		}
	}
	row[idx(Param::TransportLocked)] = s.transportLocked ? 1.0f : 0.0f;

	// THE PERFORM CIRCLE BACK OUT, in the same `Corner` order `stateOf` reads it in.
	row[idx(Param::Cfg1)] = s.cornerCfg[kCfg1];
	row[idx(Param::Cfg2)] = s.cornerCfg[kCfg2];
	row[idx(Param::Cfg3)] = s.cornerCfg[kCfg3];
	row[idx(Param::Cfg4)] = s.cornerCfg[kCfg4];
	row[idx(Param::PerfX)] = s.perfX;
	row[idx(Param::PerfY)] = s.perfY;
	row[idx(Param::PerfDepth)] = s.perfDepth;
	return row;
}

//--------------------------------------------------------------------------------------------------//
// RATE, PRINTED AS THE NOTE VALUE IT PLAYS — backlog GS.                                             //
//--------------------------------------------------------------------------------------------------//

namespace {

/// **THE BAR THE REPEAT COUNT IS TAKEN AGAINST WHEN THE EDITOR HAS NO DEVICE TO ASK** — four beats at
/// 120 BPM and 48 kHz, the convention `kAssumedSecondsPerBeat` states in `sklitter.cpp`. It only
/// reaches the answer through Repeat's 256-frame floor, which binds above Rate ~0.95; everywhere else
/// the count is a ratio and the bar cancels.
constexpr size_t kNominalBarFrames = static_cast<size_t>(kBeatsPerBar) * 24000u;

/// **WHAT THE WHOLE RATE CONTROL READS AS.** `1/N`, with N the nearest whole — exact at the five
/// divisions, triplets falling out on their own (1/12, 1/24), and an honest reciprocal between (1/21).
/// One slice of a bar reads "1 bar", `device::kDivisionNames`' own spelling.
///
/// **FROM THE DEVICE WHILE A GESTURE IS LIVE, FROM THE KNOB AT REST.** During a gesture the position
/// playing is not the knob's — a quantized press snapped it, and the knob is an offset from there
/// since — so only the device's `Reading::RateDivision` can say what is heard. At rest there is no
/// such difference and the knob is read directly, so the readout follows a turn even when no audio is
/// running to publish one; it takes the same two rules the device's resting picture takes
/// (`loopSlicesFor`, `rateDivisionFor`), so the two cannot disagree.
bool formatText(size_t control, double value, const device::TextView& view, char* out, size_t size) {
	// **GROOVE PRINTS HOW MUCH, IN THE FEEL'S OWN TERMS (backlog IG).** The zone's NAME is the selector's
	// row under the box, so the box says the amount: a Swing as the share of the pair its first step takes
	// — 50% straight, 67% the triplet, 75% heavy, which is how a drum machine prints swing — and a Groove as
	// how much of the template. Straight has no amount to print, and says so.
	//
	// **TO A TENTH OF A PERCENT (backlog IO)** — the digit fractional precision drag grabs. The
	// box's digit road converts a grabbed place through this print's slope across the zone
	// (`ValueBox::fencedReadoutScale`), so the tenths column moves a tenth of a percent per digit and the
	// MPC's whole percents are still exact readings (`58.0%`).
	//
	// **SINCE BACKLOG IO IT IS THE AMOUNT'S PRINT, AND IT READS THE MODE BESIDE IT** — the one control whose
	// meaning follows another's. Free Phase is a position along its walk, so it prints as the number it is
	// (`12.050`), to a thousandth: a readout that IS the value, which the box's digit drag takes as it is.
	if (control == idx(Param::FreePhase)) {
		std::snprintf(out, size, "%.3f", std::isfinite(value) ? value : 0.0);
		return true;
	}
	if (control == idx(Param::GrooveAmount)) {
		const GrooveMode mode = view.field != nullptr
		                            ? grooveModeOf(static_cast<double>(view.field(view.host, idx(Param::GrooveMode))))
		                            : GrooveMode::Straight;
		const double amount = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
		if (mode == GrooveMode::Straight) {
			std::snprintf(out, size, "straight");
		}
		else if (isSwing(mode)) {
			std::snprintf(out, size, "%.1f%%", swingRatioOf(amount) * 100.0);
		}
		else {
			std::snprintf(out, size, "%.1f%%", amount * 100.0);
		}
		return true;
	}
	if (control != idx(Param::Rate)) {
		return false;
	}
	const bool published = view.readingCount != nullptr && view.reading != nullptr
	                       && view.readingCount(view.host) > static_cast<size_t>(Reading::RateDivision);
	const bool live = published
	                  && std::lround(view.reading(view.host, static_cast<size_t>(Reading::Status)))
	                         != static_cast<long>(Status::Off);
	double n = 0.0;
	if (live) {
		n = static_cast<double>(view.reading(view.host, static_cast<size_t>(Reading::RateDivision)));
	}
	else {
		const float modeField = view.field != nullptr ? view.field(view.host, idx(Param::Mode))
		                                              : static_cast<float>(Mode::Shuffle);
		const auto mode = static_cast<Mode>(choiceIndex(modeField, static_cast<uint8_t>(Mode::kCount)));
		const double position = std::clamp(value, 0.0, 1.0);
		n = rateDivisionFor(mode, position, loopSlicesFor(mode, static_cast<float>(position), kNominalBarFrames));
	}
	const long whole = std::lround(n);
	if (whole <= 1) {
		std::snprintf(out, size, "1 bar");
	}
	else {
		std::snprintf(out, size, "1/%ld", whole);
	}
	return true;
}

} // namespace

const device::TextOps kTextOps { .format = &formatText };

} // namespace feathers::fx::sklitter
