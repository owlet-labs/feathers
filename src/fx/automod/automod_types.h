/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/device.h"
#include "device/registry.h"
#include "fx/automod/automod_lfo.h"

#include <cmath>
#include <algorithm>
#include <array>
#include <utility>
#include <cstddef>

// THE AUTOMODULATOR — an auto-wah whose filter, comb and envelope response are all read out of one
// three-coordinate position in the phi field.
//
// WHAT MAKES IT ONE DEVICE RATHER THAN A CHAIN. A filter swept by an LFO, a comb swept by the same
// LFO at a different phase, and an envelope follower steering both, are three effects if you build
// them separately. Here they share a waveform, a set of springs, and a coordinate — so moving the
// Mod knob redraws the LFO that drives all of them at once, which is a gesture no chain of three
// devices can offer.
//
// ---------------------------------------------------------------------------------------------
// THE CONTROLS ARE THREE COORDINATES AND SIX PERFORMANCE KNOBS. THAT IS THE WHOLE SURFACE.
// ---------------------------------------------------------------------------------------------
//
// TYPE, FLAVOUR and MOD are zone coordinates: each names a region of an infinite line, each has its
// own phase offset, and everything the device sounds like is derived from where they sit — see
// `automod_derived.h`, which is the entire character layer as one pure function. Forty-odd numbers
// come out of three positions, and NONE of them is a knob. That is deliberate and it is the same
// argument `shaper_types.h` makes about its extras: "a knob per amount would be nine more controls,
// and it would break the one claim this device makes: that the position IS the sound."
//
// THE DEVICE GAMMA IS A FOURTH PHASE, IT IS COARSE ONLY, AND IT GETS A PLAIN `ValueBox`: gamma
// controls are coarse only, and are value boxes.
//
// WHY, AND THE REASON GENERALISES TO THE WHOLE WIDGET FAMILY. A `ZoneCoordBox`'s coarse:fine split
// exists so a hand can land on a NAMED region — "Width", "Punch", "Air" — with the fine half
// positioning you inside it and the coarse half carrying you between them. **A device-wide phase has
// no such vocabulary to land on. Gamma has no named regions, so there is nothing for a fine half to
// be fine ABOUT** — the pair would be a colon with nothing on one side of it.
//
// So: **`ZoneCoordBox` is for a quantity that has both granularities. A phase on its own is one
// box.** That is why `Param::Gamma` below declares no zone count, where the three coordinates do.
//
// ---------------------------------------------------------------------------------------------
// NO SYNC CONTROL, AND THAT IS THE HONESTY RULE RATHER THAN AN OMISSION.
// ---------------------------------------------------------------------------------------------
//
// The firmware offers a tempo-synced rate. The fx lane's contract is `prepare(sampleRate)` plus
// `processStereo` — no clock reaches a device here, where the modulator rack gets a
// `mod::TransportClock`. So a Sync switch would be a control that cannot change the sound, and a
// control is enabled iff it does. `Lfo::setRateHz` is the seam a synced rate will arrive through
// when the clock is plumbed; nothing else in the device changes when it is.
//
// NO TREMOLO EITHER, and for a harder reason: the firmware's tremolo depth triangle has duty 0.0 and
// is identically zero at every coordinate — measured — with `// Disabled: band mixing provides AM`
// beside it and a `TODO` from its author. A Tremolo Depth knob would be a control that cannot change
// the sound. Its absence is recorded so the next reader diffing the two files does not conclude we
// dropped a feature.

namespace feathers::fx::automod {

/// HOW THE LFO ANSWERS THE GATE, as the player picks it. Four, where `RunMode` has three: STOP is not
/// an LFO mode, it is the EFFECT deciding to read the Manual knob instead of the LFO, which is why
/// the LFO itself has no idea it exists.
enum class LfoModeChoice : int {
	Free,      ///< runs continuously, ignoring notes
	Retrigger, ///< restarts on a note
	Once,      ///< one cycle per note, then holds
	Stop,      ///< frozen: Manual drives the modulation directly
	kCount,
};

inline constexpr const char* kLfoModeNames[] = { "Free", "Retrigger", "Once", "Stop" };
static_assert(std::size(kLfoModeNames) == static_cast<size_t>(LfoModeChoice::kCount));

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// THE SYNC CHOICES: OFF, THEN EVERY MUSICAL DIVISION. Built from `device::kDivisionNames` rather
/// than retyped, so the two can never disagree about what the seventh entry means — the table moved
/// to `device/` precisely so an fx device could read it.
///
/// A `constexpr` FUNCTION RATHER THAN A LITERAL LIST, because 28 hand-written strings is a list
/// nobody can check against the table it shadows.
/// THE FREE RATES THE DROPDOWN CARRIES — **EXACTLY TWO, BECAUSE EXACTLY TWO.**
///
/// As in Bitwig, the sync dropdown also carries non-synced time constants (e.g. 1 Hz), which are
/// naturally scaled. The free rate lives in the LIST, not in a second mode of the Rate knob, and that is what
/// gives Rate one meaning everywhere: a multiplier on whatever the list selected.
///
/// **THERE IS NO `Off` ANY MORE, AND ITS ABSENCE IS FORCED RATHER THAN CHOSEN.** `Off` used to mean
/// "read the Rate knob as a frequency" — the second meaning this scheme exists to remove. With Rate
/// always a multiplier there is nothing for `Off` to select, so it would be a choice with no base
/// rate behind it. Two free entries replace it and cover what it was for.
inline constexpr float kFreeRateHz[] = { 0.1f, 1.0f };
inline constexpr const char* kFreeRateNames[] = { "0.1 Hz", "1 Hz" };
inline constexpr size_t kFreeRateCount = std::size(kFreeRateHz);
static_assert(std::size(kFreeRateNames) == kFreeRateCount);

/// THE FREE RATES FIRST, THEN EVERY MUSICAL DIVISION.
inline constexpr size_t kSyncChoiceCount = kFreeRateCount + device::kDivisionCount;

constexpr std::array<const char*, kSyncChoiceCount> makeSyncNames() {
	std::array<const char*, kSyncChoiceCount> out {};
	for (size_t i = 0; i < kFreeRateCount; ++i) {
		out[i] = kFreeRateNames[i];
	}
	for (size_t i = 0; i < device::kDivisionCount; ++i) {
		out[kFreeRateCount + i] = device::kDivisionNames[i];
	}
	return out;
}
inline constexpr auto kSyncNames = makeSyncNames();

/// WHICH KIND OF ENTRY A CHOICE IS. One place converts, so a reader never does the arithmetic by hand
/// — the offset between the choice index and the division index is exactly the trap this hides.
constexpr bool isFreeRate(int choice) {
	return choice >= 0 && static_cast<size_t>(choice) < kFreeRateCount;
}
constexpr bool isSyncedRate(int choice) {
	return static_cast<size_t>(choice) >= kFreeRateCount
	       && static_cast<size_t>(choice) < kSyncChoiceCount;
}
constexpr float freeRateHzFor(int choice) {
	return isFreeRate(choice) ? kFreeRateHz[choice] : kFreeRateHz[0];
}
constexpr double syncDivisionBeats(int choice) {
	return isSyncedRate(choice)
	           ? device::kDivisionBeats[static_cast<size_t>(choice) - kFreeRateCount]
	           : 0.0;
}

/// HOW MANY NAMED REGIONS A ZONE COORDINATE HAS. Eight, as every zone control in this project and in
/// the firmware has — and it is DECLARED rather than implied, because it is what makes a route's
/// amount mean one zone rather than the whole range, and what tells the widget where the names sit.
inline constexpr int kZones = 8;

/// **HOW MUCH COORDINATE ONE FULL PASS THROUGH THE EIGHT NAMES IS WORTH.**
///
/// One, and it is read off the CONSUMPTION rather than chosen: the offsets are consumed as
/// `wrapPhase(offset * kPhiNNN)`, a phi multiplier per field, so the natural quantum is 1 — each lap
/// lands the fields on a different, unrepeating set of phases, which is what the firmware's encoder
/// does when it wraps. `ui::ZoneSelect::kCoordinatePerLap` is the same number on the widget side and a
/// `static_assert` there pins them together.
inline constexpr double kLapCoordinate = 1.0;

/// **ONE ZONE, IN THE COORDINATE'S OWN UNITS — the modulation span of a collapsed coordinate.**
///
/// A lap over the zone count: eight named regions make one lap, so a zone is an eighth of one.
///
/// **RE-DERIVED FOR THE COLLAPSE (BJ piece 3) AND IT LANDS ON THE SAME NUMBER, which is worth saying
/// because it did not have to.** It used to be justified as "one zone of `Type`", where `Type` was a
/// bounded 0..1 half — so `range / zones` and `lap / zones` agreed by coincidence of the range being
/// 1.0 wide. The survivor is declared ±2^24 and that coincidence is gone: `range / zones` is now 4.19
/// MILLION. The quantity is what fixes the number, not the declaration.
///
/// **IT CANNOT BE SPELLED AS A ZONE COUNT OVER A RANGE**, which is why `ParamDesc::modSpan` exists —
/// and the collapse is what turned that from a wrinkle of the pair into the only way to say it.
inline constexpr double kOneZone = kLapCoordinate / static_cast<double>(kZones);

/// HOW FAR A PHASE OFFSET MAY BE DRIVEN — WHERE THE FLOAT ROW STOPS COUNTING, AND NOT ONE STEP MORE.
///
/// THIS CONSTANT HAS BEEN THREE DIFFERENT KINDS OF NUMBER AND ONLY THE THIRD IS A FACT.
///
///   64      copied from OWLTT, justified by a comment claiming the field "never repeats within a
///           session's worth of turning" — an assumption written as a fact. gamma only goes
///           up to 64?
///   1024    a reach limit, chosen for the hand. gamma still stops at 1024. The aim was not
///           a bigger number but no wall.
///   2^24-1  where the STORAGE stops. Measured, and it is the only value here that is not taste.
///
/// **IT IS NOT THE FIELD.** Measured at the default coordinate from gamma 64 to 1e9: the spread of a
/// derived value across 200 consecutive representable neighbours rises to full range and STAYS
/// there — 0.998 at 1e5, 0.996 at 1e9 — and nothing goes non-finite anywhere. The phi evaluation
/// does not flatten, alias or lose structure. **The field is still rich past everywhere we can
/// reach, so it is not the ceiling and never was.**
///
/// **IT IS THE FLOAT ROW.** A device row is `std::array<float, kMaxDeviceFields>`, and a float's
/// step grows with magnitude. At 1e5 one representable step moves the sound by 0.099 where a whole
/// lap moves it 1.000 — still a fine adjustment. At 1e6 one step moves it **1.323, more than a lap
/// does**: the smallest possible change is already an arbitrary jump.
///
/// **AND AT 2^24 THE CONTROL IS DEAD AT ITS OWN MAXIMUM.** Verified in float32:
///
///     16,777,215   ulp 1   +1 lap -> 16,777,216   moves
///     16,777,216   ulp 2   +1 lap -> 16,777,216   DEAD, a lap does nothing
///     16,777,218   ulp 2   +1 lap -> 16,777,220   RAGGED, a lap travels two
///
/// So the cap is 2^24 **minus one**: the last value at which a lap still moves. Capping at 2^24
/// itself would make the topmost reachable value the one place it provably does nothing — the same
/// defect as the old static default, at the other end of the range. **And past it the control is not
/// merely coarse, it is ragged: some nudges do nothing and others travel double.**
///
/// WHY THE DEATH POINT RATHER THAN A CONTROLLABILITY POINT LIKE 2^20. Gamma is a TYPED VALUE BOX, so
/// a hand reaches the upper decades by typing rather than by dragging — per-click granularity up
/// there costs little, where a wall costs everything. Sub-lap nudges stop being representable above
/// 2^20; that is what the fine half of a coordinate is for.
///
/// IF SOMEBODY EVER WANTS PAST THIS, THE ANSWER IS NOT A BIGGER CONSTANT. The field is rich at 1e9
/// and unreachable because the row is float — so the change would be a wider field, which is a
/// design conversation rather than a tuning one. **16.7 million turns of an eight-zone control is
/// 134 million named regions, and nobody arrives there by dragging.**
///
/// FOUR CONTROLS SHARE THIS: gamma and the three per-coordinate phase offsets.
///
/// **AND ALL FOUR DECLARE `-kPhaseMax .. kPhaseMax`, WHICH THEY DID NOT — THE BOTTOM WAS 0 AND THAT
/// WAS A WALL AT THE PLACE EVERY PATCH STARTS.** All four default to 0, so the very first BACKWARDS
/// lap hit the declared minimum and `ZoneCoordBox::onCarry`, which clamps each carried lap into
/// `min..max`, simply stopped carrying. The wall was invisible in the way this shape always is: turn
/// the knob the other way first and nothing looks wrong.
///
/// FOUND BY `tests/test_device_ranges.cpp` RATHER THAN BY A REPORT, which is the whole point of that
/// file. It is the sixth, seventh and eighth instance of a defect whose first five — Shaper Gamma,
/// Shaper Rotate, Shaper Colour Phase, OWLTT Character Phase, OWLTT Vibe Phase — were every one of them
/// reported from turning a knob by hand. Gamma below is the ninth and the sweep did NOT catch it: it has
/// no `coarseOf` partner, so only the second clause (an unbounded declaration must be unbounded both
/// ways) reaches it.
///
/// **THE DERIVATION ABOVE IS UNTOUCHED BY THE MIRRORING.** Float ULP is symmetric about zero, so
/// -kPhaseMax is exactly as far as +kPhaseMax and the "last value at which a lap still moves"
/// argument holds identically at the bottom. The engine wants nothing here either: `pushAlong` adds
/// these into phases that are reduced mod 1 by `wrapPhase`, which floors rather than truncating
/// (`phi_triangle.hpp:81`), so a negative offset is a real position and not a mirrored one.
inline constexpr double kPhaseMax = 16777215.0; // 2^24 - 1

/// THE THREE PHASE OFFSETS A PAD POSITION AND A ROTATION DECODE INTO.
struct PhaseOffsets {
	float type = 0.0f;
	float flavour = 0.0f;
	float mod = 0.0f;

	friend bool operator==(const PhaseOffsets&, const PhaseOffsets&) = default;
};

/// HOW MUCH PHASE A FULL PUSH OF THE PAD ADDS, in laps. Eight is a hand's worth of travel over a
/// quantity that has no ends — `kPhaseMax` is a 2^24 counter, which is a range for a lapping encoder
/// and not for a pad.
inline constexpr float kPadPhaseSpan = 8.0f;

/// **WHAT AN XY PAD AND A ROTATION MEAN FOR THREE PHASE OFFSETS.**
///
/// THE SHAPE IS DERIVED FROM `deriveAutomod`, NOT INVENTED. It folds the phases as
/// `type + typePhase + gammaPhase` and the same for the other two — so GAMMA IS THE COMMON MODE, the
/// term that moves all three together, and the three offsets are what is left. The part of three
/// numbers that a common term cannot express is exactly TWO-DIMENSIONAL, which is why a pad is the
/// right widget and why it needs no third axis: it spans precisely the space gamma does not.
///
/// THREE AXES AT 120 DEGREES, and `rotation` re-aims them. That is Feathers' own sense of the word —
/// `explore_stage.h`'s rotate is "a re-aiming rather than an amount", an unbounded compass rather
/// than a slider — so turning it changes WHICH coordinate the puck is pointing at without moving the
/// puck.
///
/// **RAISED-COSINE, NOT HALF-RECTIFIED, AND THE ROUND-TRIP IS WHAT DECIDED IT.** A phase is a lapping
/// counter floored at zero, so the lobes must be non-negative and the centre must be zero — and
/// `max(0, cos)` gives both. It was the first form here, borrowed from `shaper_curve.h`'s six bases,
/// and it is NOT INVERTIBLE: with three axes 120 degrees apart each rectified lobe is alive over 180,
/// so within 30 degrees of an axis exactly ONE lobe is non-zero, and one number cannot carry both a
/// radius and an angle. **The pad would have been unable to show its own position over a third of its
/// face.** Measured before anything was built on it: worst round-trip error 0.99 of the pad's radius.
///
/// `(1 + cos) / 2` FIXES IT BY CONSTRUCTION, because the three lobes then SUM TO A CONSTANT —
/// `3/2 * span * radius`, since the three cosines at 120 degrees cancel. The sum therefore names the
/// radius outright and the differences name the angle, which is what makes the inverse exact rather
/// than fitted. All three coordinates take some phase at any push, weighted by where the puck points,
/// which for a PHASE is a blend rather than the mud that argument warns about in a timbre basis.
[[nodiscard]] inline PhaseOffsets phaseOffsetsFor(float x, float y, float rotation) {
	const auto finite = [](float v) { return std::isfinite(v) ? v : 0.0f; };
	const float px = std::clamp(finite(x), -1.0f, 1.0f);
	const float py = std::clamp(finite(y), -1.0f, 1.0f);
	// THE PUCK AS A DIRECTION AND A DISTANCE. Clamped at one so a corner of a square pad does not
	// reach further than an edge — the decode is polar and the widget is not.
	const float radius = std::min(std::sqrt(px * px + py * py), 1.0f);
	if (radius <= 0.0f) {
		return {};
	}
	const float angle = std::atan2(py, px) + finite(rotation);
	constexpr float kThird = 2.0943951f; // 2*pi/3
	const auto lobe = [&](int which) {
		const float toward = std::cos(angle - static_cast<float>(which) * kThird);
		return kPadPhaseSpan * radius * 0.5f * (1.0f + toward);
	};
	return {lobe(0), lobe(1), lobe(2)};
}

/// WHERE THE PUCK MUST BE FOR THESE THREE OFFSETS — the inverse of `phaseOffsetsFor`.
///
/// **IT EXISTS BECAUSE THE PHASES ARE THE STATE AND THE PUCK IS A VIEW OF THEM.** The alternative was
/// storing a pad position as its own pair of parameters, which would make the same fact
/// representable twice — and two sources of truth for one quantity is the shape that lets a panel and
/// a patch disagree about where a device is standing.
///
/// **THE INVERSE EXISTS ONLY BECAUSE THE DECODE IS HALF-RECTIFIED**, which is worth stating because
/// it is the second thing that choice bought. Three unrestricted projections would be a 2-to-3 map
/// with no inverse; half-rectified, at most TWO lobes are ever non-zero, so the pair that IS non-zero
/// names the sector and their ratio names the angle inside it. A degenerate decode would have made
/// the pad unable to show its own position.
///
/// `rotation` MUST BE THE SAME ONE THE DECODE WAS CALLED WITH. Only `angle + rotation` is observable
/// in the offsets, so the puck's angle can be recovered only relative to a known re-aiming — which is
/// exactly why rotation is stored rather than being a transient of the widget.
// **ROTATE A PHASE TRIPLE ABOUT ITS OWN MEAN. EXACT FOR ANY TRIPLE, AND THE IDENTITY AT ZERO.**
//
// This is NOT `phaseOffsetsFor(puckForOffsets(o))` with an angle added, which was the first draft and
// was WRONG in a way that only showed off the pad. That composition is the identity only on the pad's
// own manifold, and the card's three ZoneCoordBox widgets write phases that are not on it: (8, 0, 0)
// is a legal thing to dial there, and the round-trip form silently rewrote it to (5.33, 1.33, 1.33)
// AT ROTATION ZERO — a rotation control that moves the sound while pointing at "no rotation", and a
// regression on three controls that already worked.
//
// Three values sitting at 120 degrees have exactly three degrees of freedom: a mean, and one complex
// component. Take those, turn the component, put them back. Nothing is projected away because nothing
// is discarded, and theta = 0 reproduces the input identically rather than approximately.
[[nodiscard]] inline PhaseOffsets rotatePhases(const PhaseOffsets& o, float rotation) {
	// NOT the `finite` of the sibling functions — that one is a lambda LOCAL to each of them, and at
	// namespace scope the name resolves instead to POSIX `::finite` out of <math.h>, which returns an
	// int. Every phase would have been truncated to 0 or 1 and the rotation would have been a staircase.
	// `-Werror=narrowing` is the only thing that said so.
	const auto fin = [](float v) { return std::isfinite(v) ? v : 0.0f; };
	const float theta = fin(rotation);
	const float lobes[3] = { fin(o.type), fin(o.flavour), fin(o.mod) };
	const float mean = (lobes[0] + lobes[1] + lobes[2]) / 3.0f;
	constexpr float kThird = 2.0943951f; // 2*pi/3
	float cx = 0.0f;
	float cy = 0.0f;
	for (int i = 0; i < 3; ++i) {
		const float axis = static_cast<float>(i) * kThird;
		cx += (lobes[i] - mean) * std::cos(axis);
		cy += (lobes[i] - mean) * std::sin(axis);
	}
	cx *= 2.0f / 3.0f;
	cy *= 2.0f / 3.0f;
	const float rx = cx * std::cos(theta) - cy * std::sin(theta);
	const float ry = cx * std::sin(theta) + cy * std::cos(theta);
	PhaseOffsets out {};
	float* dst[3] = { &out.type, &out.flavour, &out.mod };
	for (int i = 0; i < 3; ++i) {
		const float axis = static_cast<float>(i) * kThird;
		*dst[i] = mean + rx * std::cos(axis) + ry * std::sin(axis);
	}
	return out;
}

[[nodiscard]] inline std::pair<float, float> puckForOffsets(const PhaseOffsets& o, float rotation) {
	const auto finite = [](float v) { return std::isfinite(v) ? std::max(v, 0.0f) : 0.0f; };
	const float lobes[3] = {finite(o.type), finite(o.flavour), finite(o.mod)};
	const float sum = lobes[0] + lobes[1] + lobes[2];
	if (sum <= 0.0f) {
		return {0.0f, 0.0f};
	}
	// **THE SUM IS THE RADIUS, EXACTLY.** Three raised cosines 120 degrees apart sum to
	// `3/2 * span * radius` for every angle, because the cosine terms cancel — so this is a reading
	// and not an estimate.
	const float radius = std::clamp(sum / (1.5f * std::max(kPadPhaseSpan, 1e-6f)), 0.0f, 1.0f);
	// AND THE DIFFERENCES ARE THE DIRECTION. Subtracting the mean removes the constant half of each
	// lobe, leaving `span * radius / 2 * cos(angle - axis)` — a clean projection onto each axis, which
	// three vectors at 120 degrees recombine exactly.
	constexpr float kThird = 2.0943951f;
	const float mean = sum / 3.0f;
	float vx = 0.0f;
	float vy = 0.0f;
	for (int i = 0; i < 3; ++i) {
		const float axis = static_cast<float>(i) * kThird;
		vx += (lobes[i] - mean) * std::cos(axis);
		vy += (lobes[i] - mean) * std::sin(axis);
	}
	const float angle = std::atan2(vy, vx) - (std::isfinite(rotation) ? rotation : 0.0f);
	return {radius * std::cos(angle), radius * std::sin(angle)};
}

//------------------------------------------------------------------------------------------------//

/// THE ORDER IS THE STREAM ORDER AND THE DESCRIPTOR ORDER, as every device here has it.
enum class Param : int {
	Enabled,

	//-- the three coordinates. ONE PARAM EACH, collapsed from a pair — backlog BJ, piece 3. ----//
	//
	// **EACH OF THESE WAS TWO PARAMS AND THE ENGINE ONLY EVER SAW THEIR SUM.** `deriveAutomod`
	// computed `homeFor(type + typePhase, …)`, and this header's own `kOneZone` note measured the
	// consequence: `type + 0.125` and `typePhase = 0.125` came out BIT-IDENTICAL. Two params, one
	// quantity, and a widget pretending the split was a granularity.
	//
	// So the collapse is arithmetic, not taste. The single control carries what the sum carried, its
	// default is what the sum's default was, and nothing downstream can tell the difference — which
	// is what made it legal under option (A), where no device changes how it sounds.
	//
	// **THIS SHRANK THE DEVICE ROW BY THREE FLOATS AND BROKE THE PATCH FORMAT**, which is free while
	// nothing is saved — the project's standing patch-compatibility policy, confirmed rather than assumed.
	Type,
	Flavor,
	Mod,
	/// The device-wide phase. Coarse only — see the header.
	Gamma,

	//-- what plays them. ----------------------------------------------------------------------//
	/// WHERE THE WHOLE DEVICE SITS IN THE SPECTRUM — the firmware's `GLOBAL_AUTOMOD_FREQ`, and the
	/// first control of its page 2. Bipolar: it brightens the filter and SHORTENS the comb together.
	Freq,
	Rate,
	/// Off, or which musical division the LFO's cycle is.
	SyncDivision,
	LfoMode,
	Depth,
	Manual,
	Mix,

	/// THE ENVELOPE FOLLOWER'S INPUT TRIM, in dB — what level of incoming signal the follower should
	/// treat as full.
	///
	/// **IT EXISTS BECAUSE THE FIRMWARE'S ENVELOPE TERM IS SCALED BY A HEADROOM FACTOR THIS PORT DOES
	/// NOT HAVE, and the factor is 128.** `automodulator.cpp:570` builds the envelope from RAW q31
	/// sample peaks and then multiplies it in as a FRACTION of 2^31 — while `fixedpoint.h` puts the
	/// hardware's 0 dBFS at `ONE_Q31 / 128` = 2^24. So at the hardware's own full scale the envelope
	/// enters the depth term at 2^24/2^31 = 0.0078, a ±0.78% trim on the modulation depth. This port's
	/// float envelope reads 1.0 at 0 dBFS and enters at ±100% — 42.1 dB stronger, which is exactly
	/// `ONE_Q31 / EFFECTIVE_0DBFS_Q31`.
	///
	/// A KNOB RATHER THAN A BAKED CONSTANT, because the right operating point is a listening decision
	/// and neither end of it is obviously correct: exact parity is a sub-1% trim nobody would hear,
	/// and the port's current authority inverts the LFO on any signal past 0 dBFS. Declaring it makes
	/// the operating point VISIBLE, which is the thing that was missing everywhere this went wrong.
	///
	/// dB, DECLARED AS dB (backlog DL, 2026-09-16). This carried `Unit::Number` "because there is no
	/// Decibel unit"; Bus Utility brought one and the box prints "-18.0dB" for it — the stored float is
	/// untouched. The range reaches parity at its bottom and unity at its top, so both edges of the
	/// argument are on the knob.
	EnvTrim,

	/// **THE PHASE PAD'S RE-AIMING** — which plane the pad travels in. A PLANE TURN, NOT AN ANGLE (2026-10-11):
	/// since IJ the pad decodes through `ps::basisFor` (`sliceBasisFor`, Explore's own), where each coordinate
	/// turns at its own phi-spaced rate, so the plane has no period and no value of this is "a full turn". It
	/// reads as a bare number, as Shaper Rotate does. The polar paragraphs below describe the decode it replaced
	/// (`phaseOffsetsFor`, kept for its tests).
	///
	/// A RE-AIMING AND NOT AN AMOUNT, which is Feathers' own sense of the word: `explore_stage.h`
	/// calls its rotate "a re-aiming rather than an amount… the mock's own knob is unbounded too".
	/// Turning this moves nothing by itself — it changes what the SAME puck position means, which is
	/// why the pad draws its three axis spokes turning: a control that re-aims an invisible basis
	/// would look dead while doing the most interesting thing on the surface.
	///
	/// ITS PERIOD IS A FULL TURN, NOT A THIRD OF ONE. The three axes are 120 degrees apart, so a third
	/// of a turn maps each axis onto the next — but they are not interchangeable: axis 0 is Type's
	/// offset and axis 1 is Flavour's. A third of a turn therefore PERMUTES which coordinate receives
	/// which offset, which is a different sound and not the same one. Only a whole turn is the
	/// identity.
	///
	/// ZERO IS "STANDARD", the un-rotated basis — Type's axis pointing along the pad's positive X.
	Rotate,

	/// **HOW FAR APART THE TWO EARS' MODULATION RUNS** (backlog IS 2026-09-23: a stereo width knob, so
	/// the stereo spread can be disabled). It scales the field's `stereoPhaseOffset` — the phase the right
	/// ear's LFO trails the left's by — for the filter AND the comb. 0 is mono modulation; 100% is the
	/// field's own offset. It became worth having when the filter stopped averaging the two ears into one
	/// corner: before that the field's stereo reached only the comb.
	Width,

	/// **KEYTRACK — ONE SWITCH FOR BOTH FILTERS** (backlog IS.5 2026-09-23: one global keytrack switch
	/// enables or disables it for all the filters, not just the comb). On, the SVF's corner
	/// rises with the note and the comb's delay falls with it — each keeping the offset the field gave it
	/// from the note, which is the point of tracking rather than tuning (the phi triangles might
	/// offset the comb intentionally away from the note). Off, both sit where the field puts them at C4.
	Keytrack,

	kCount,
};

/// ZONE NAMES. Each says what its region DOES, and where the behaviour has not been measured the
/// name is left empty rather than invented — the same discipline `owltt_types.h` keeps, and for the
/// same reason: an evocative name for an unmeasured region is a promise the sound may not keep.
struct ZoneName {
	const char* name;
	const char* description;
};

/// **NO ZONE NAMES ANY MORE (backlog IW).** Type, Flavour and Mod are continuous coordinates — nothing
/// in the DSP branches on a zone — so under IW they are FREE-ONLY: no dropdown, and a sigil for the lap where the
/// selector was. The two names that existed (`Open` at Type's first eighth, `Rest` where Mod's waveform flattens
/// at the very bottom) described points of a continuum, not modes. `kZones` stays: it is still the drag and
/// modulation unit.

//------------------------------------------------------------------------------------------------//

inline constexpr device::ParamDesc kParamDescs[] = {
	// **ON BY DEFAULT — all effects should default to on when they are added.**
	//
	// THIS DEVICE'S FIRST DRAFT DEFAULTED IT ON and it was changed to off to match the unanimous
	// convention of the other four. The convention is what has now been reversed, so the first draft
	// was right and the note that stood here — *"a device that starts processing the moment it is
	// added is a surprise"* — records a judgement since made the opposite way. The registry-wide
	// case it cited is still there and still walks `kDevices`; it now asserts ON.
	//
	// `Settings::enabled` was left at true through all of that, so this device and OWLTT were the two
	// whose descriptor and struct initialiser disagreed. The static_assert below is the fix for THAT.
	{ "enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
	  "Bypass switch — off passes the signal through untouched" },

	// THE THREE COORDINATES ARE MODULATABLE AND THEIR PHASES ARE NOT, which is the split
	// `zone_coord_box.h` records: the fine half is a real host parameter with a bounded range an
	// automation lane can hold, and the coarse half "carries no id, and is unbounded... no host would
	// know how to draw an unbounded range". The phases are still device STATE and still persist.
	// THE THREE DEFAULT COORDINATES ARE CHOSEN BY MEASUREMENT, and (0, 0, 0) was not.
	//
	// THE DEFECT: some settings, including the default/init setting, were completely quiet. The level
	// half of that was two porting bugs and is fixed. THE OTHER HALF SURVIVED
	// THE FIX: at (0, 0, 0) the LFO's waveform collapses to a flat line — `test_phi_lfo_shape.cpp`
	// pins that the origin is the one coordinate where it does — so the device was a STATIC filter at
	// its own default. MEASURED: **0.000 octaves of cutoff travel.** A wah that does not sweep reads
	// as broken however loud it is.
	//
	// CHOSEN THE WAY THE TEST FIXTURE WAS, because that method is the one good thing to come out of
	// the deadzone problem: shortlist 41^3 coordinates from the derive for every road live, then
	// RENDER the finalists rather than trusting the shortlist — a model is evidence about mechanism,
	// not about magnitude. Five coordinates met all three bars; this one has the most travel.
	//
	// AT (0.30, 0.70, 0.10), MEASURED: **1.97 octaves of cutoff travel, -3.4 dB against the input**,
	// and every road comfortably live — filter sweep 0.567, envelope steer -0.404, comb mix 0.549,
	// comb range 134.6 samples. A case pins all three so a bank retune that moves it fails loudly.
	// **THE THREE COLLAPSED COORDINATES — one param each where there were two. Backlog BJ, piece 3.**
	//
	// **UNBOUNDED, WHICH IS WHAT THE PHASE HALF ALREADY WAS.** The old zoned half declared 0..1 and
	// the old phase half declared ±kPhaseMax; the SUM the engine read was therefore unbounded all
	// along, and the 0..1 was a window on a box rather than a limit on the quantity. Declaring the
	// survivor 0..1 would have introduced a wall that never existed — the exact defect
	// `test_device_ranges.cpp` was written for, and the one that walled `Gamma`, `Rotate`, both OWLTT
	// phases and both shaper phases before it.
	//
	// **`Unit::Number` RATHER THAN `Percent`, AND IT IS NOT COSMETIC.** A Percent readout prints a
	// hundred times its value, which is a fine caption for a bounded 0..1 knob and a lie about an
	// unbounded coordinate — and `ValueBox::readoutTracksValue` (BJ piece 1) declines digit-place
	// dragging on any readout that is not the value, so a Percent declaration here would have
	// silently withheld the gesture the collapse exists to enable.
	//
	// **`modulatable` MOVES TO THE SURVIVOR, because there is now only one control to aim at.** The
	// old pair put the flag on the phase half; the rule behind that — only the right-hand half is
	// modulatable — was about which HALF of a pair a route may point at, and a device with no pair
	// has no half. The ambiguity is gone by construction rather than by convention.
	//
	// **`modSpan` IS RE-DERIVED AND LANDS ON THE SAME NUMBER, which is worth saying because the
	// proposal warned it might not.** `kOneZone` is `1/kZones` = 0.125 and it was measured against
	// the phase half; the collapsed control is the SUM of the two halves, and the header's own note
	// records that the halves were already the same scale — `type + 0.125` and `typePhase = 0.125`
	// bit-identical. So one zone of the collapsed control is 0.125 for the same reason it was 0.125
	// before, not by carrying the constant across. OWLTT's `kOnePhaseUnit` is 1.0 and is NOT a
	// disagreement: its phase is consumed on a different scale, which is why `modSpan` is per-param.
	{ "type", "Type", -kPhaseMax, kPhaseMax, 0.30, false, device::Unit::Number, {}, true, false,
	  "Comb feedback, comb mix, filter self-feedback and the band blend — eight named zones, and it "
	  "keeps turning past them",
	  kZones, kOneZone },

	{ "flavour", "Flavour", -kPhaseMax, kPhaseMax, 0.70, false, device::Unit::Number, {}, true, false,
	  "Filter corner, resonance, comb tuning and envelope speed — eight named zones, and it keeps "
	  "turning past them",
	  kZones, kOneZone },

	{ "mod", "Mod", -kPhaseMax, kPhaseMax, 0.10, false, device::Unit::Number, {}, true, false,
	  "The LFO's waveform, the stereo spread and the spring character — eight named zones, and it "
	  "keeps turning past them",
	  kZones, kOneZone },

	// COARSE ONLY, BY DESIGN. No zone count declared, because it has no named regions to
	// count — that is the whole reason it is one box rather than a coarse:fine pair.
	{ "gamma", "Gamma", -kPhaseMax, kPhaseMax, 0.0, false, device::Unit::Number, {}, false, false,
	  "Moves all three coordinates' phases together — a different gesture from moving one" },

	// -------------------------------------------------------------------------------------------- //
	// FREQ — THE ONE CONTROL THE PORT MISSED, AND THE REASON IT WAS MISSABLE.
	// -------------------------------------------------------------------------------------------- //
	//
	// The freq macro was reported missing on the first install. That was right and it was not ported.
	//
	// **IT IS INVISIBLE TO A PORT THAT WORKS FROM THE PARAMS STRUCT, WHICH IS HOW THIS ONE WAS DONE.** The
	// firmware keeps it in the PARAM SYSTEM as `GLOBAL_AUTOMOD_FREQ` rather than in
	// `AutomodulatorParams`, deliberately, so the mod matrix can reach it — `automodulator.hpp:512`
	// says so in as many words: *"freqOffset not copied here - it's in param system
	// (GLOBAL_AUTOMOD_FREQ)"*, and again at the clone, the move, and the state block. Every one of
	// those notes is about what is ABSENT, so a reader porting the struct sees four exclusions and no
	// control. `automodulator.hpp:577` is the only line that names the surface: *"Page 2: Freq (via
	// patched param), Rate, Manual, Depth"*. **A control held outside the state that describes a
	// device is a control a port cannot see; the next port from that codebase should read the page
	// listing, not the struct.**
	//
	// WHAT IT DOES, from `automodulator.cpp:1026-1045`: it moves the filter and the comb in OPPOSITE
	// directions — brighter corner, shorter delay — which is the same gesture note tracking already
	// makes here, because a comb's delay is the reciprocal of its pitch. So this is not new DSP: it
	// is a SECOND CONTRIBUTOR to `filterPitchRatio_` and `combPitchRatio_`, two ratios that already
	// existed.
	//
	// **±2 OCTAVES, WHICH IS THE FIRMWARE'S NUMBER FOR THE COMB AND OUR CHOICE FOR THE FILTER.** The
	// firmware offsets the filter ADDITIVELY in q31 cutoff space, across half the filter's range —
	// linear in Hz, so its octave span depends on where the corner already is. This port is
	// logarithmic throughout for the reason `kFilterModOctaves` states, and applying that consistently
	// means one octave figure serving both sides: the same ±2 the firmware gives the comb. It also
	// makes the control symmetric, which the additive form is not.
	{ "freq", "Freq", -1.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
	  "Moves the whole device up or down the spectrum — brighter filter and shorter comb together" },

	// -------------------------------------------------------------------------------------------- //
	// RATE IS A MULTIPLIER ON WHATEVER THE DROPDOWN SELECTED — ±2 OCTAVES, 1:1 AT ZERO, READ AS "1X".
	// -------------------------------------------------------------------------------------------- //
	//
	// It used to be a free frequency that the sync setting IGNORED, which is why it was said: "make
	// the default rate 100% so when you select sync, its actually in sync"* about a control that had
	// no effect under sync at all. Now it scales the selected entry, synced or free alike, and has one
	// meaning everywhere.
	//
	// **THE STORED NUMBER IS THE OCTAVE, AND THE READOUT IS THE MULTIPLE** (`Unit::LogMultiple`, the
	// MSEG's Rate's unit: "0.25X", "1X", "4X"). It was declared 0..2 and printed as 0%..200% (backlog
	// DT, 2026-09-15): the comment argued the range from the readout and, four lines on, promised ±2
	// octaves — and a percentage of a linear range cannot print an exponential multiplier, so the box
	// said "0%" for quarter speed and "200%" for four times. The stored MEANING changed with this, so
	// `kStateVersion` moved (7).
	//
	// **EXPONENTIAL AND SYMMETRIC IN RATIO ABOUT UNITY**, by design: equal travel either side gives
	// RECIPROCAL multipliers, so −1 and +1 are the same distance from unity in opposite directions.
	// That is a different curve from the old one, which was exponential from a FLOOR.
	//
	// ±2 OCTAVES (x0.25 to x4) — `kRateMultiplierOctaves`, and `automod.cpp` asserts the two agree —
	// and the reason is about the instrument rather than the number: it is the same span as
	// `kFreeOctaves`, so **both bipolar performance controls move by the same amount over the same
	// travel** and a hand learns one gesture.
	//
	// AND ZERO IS EXACTLY 1X, not nearly: `exp2(0)` is exactly 1, so the default is unity by
	// construction rather than by rounding. There is a case on it, because a symmetric curve that is
	// subtly wrong in the middle looks right at both ends.
	{ "rate", "Rate", -2.0, 2.0, 0.0, false, device::Unit::LogMultiple, {}, true, false,
	  "Scales the rate the Sync list selected — 1X is 1:1, 0.5X is half speed, 4X is four times" },

	{ "sync", "Sync", 0.0, static_cast<double>(kSyncChoiceCount) - 1.0, 0.0, true, device::Unit::Choice,
	  kSyncNames, false, false,
	  "Off runs the LFO at the Rate knob's free frequency; any division locks its cycle to the "
	  "host's tempo" },

	{ "lfo_mode", "LFO Mode", 0.0, static_cast<double>(LfoModeChoice::kCount) - 1.0, 0.0, true,
	  device::Unit::Choice, kLfoModeNames, false, false,
	  "Free runs continuously; Retrigger and Once restart on a note; Stop freezes it and hands the "
	  "modulation to Manual" },

	// DEPTH DEFAULTS TO A HALF RATHER THAN TO FULL, and that is a correctness default rather than a
	// compatibility one: an auto-wah at maximum depth on first load is a sweep nobody asked for.
	{ "depth", "Depth", 0.0, 1.0, 0.5, false, device::Unit::Percent, {}, true, false,
	  "How far the LFO moves the filter and the comb" },
	// MANUAL IS BIPOLAR AND CENTRES AT ZERO, so a patch that never touches it hears only the LFO.
	{ "manual", "Manual", -1.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
	  "A fixed offset added to the modulation — and the whole of it when LFO Mode is Stop" },

	// MIX DEFAULTS TO FULLY WET, AND THE FIRST DRAFT HAD IT AT ZERO — which was the same instinct as
	// `Enabled` applied twice. `Enabled` already provides "does nothing until asked"; a Mix of zero on
	// top of it means switching the device ON is still silent, which is a worse first minute, not a
	// safer one. OWLTT's `Blend` defaults to 1.0 for exactly this reason and is the precedent.
	//
	// AND IT MADE FIVE OTHER CONTROLS READ AS DEAD. With no wet signal, nothing downstream of the
	// blend can change the sound — so `test_fx_runner`'s honesty sweep reported Mod, Rate, LFO Mode
	// and Depth as controls that do nothing, which was true of the FIXTURE rather than of them.
	{ "mix", "Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
	  "How much of the processed signal is heard" },

	// -42.1 dB IS THE FIRMWARE'S OWN OPERATING POINT, not a round number: it is
	// 20*log10(EFFECTIVE_0DBFS_Q31 / ONE_Q31) = 20*log10(1/128). The range runs from there to unity
	// so the knob spans "as quiet as the hardware" to "as loud as this port has been", and the
	// default sits between them where the steering is audible without being able to cancel itself.
	// -42.1 dB IS **DERIVED, NOT MEASURED**, and the distinction is stated because this repo has been
	// bitten by conflating them: it is 20*log10(EFFECTIVE_0DBFS_Q31 / ONE_Q31) = 20*log10(1/128),
	// computed from two constants in the firmware's `fixedpoint.h`. What IS measured is the
	// consequence — at that trim a 0.9-amplitude input moves the filter corner by under a fifth of
	// what it moves at 0 dB, which `test_fx_automod.cpp` pins.
	//
	// THE DEFAULT IS -18 dB AND NOT PARITY, which is a choice about how the device sounds out of the
	// box rather than a compatibility concession — CLAUDE.md's standing permission to pick the RIGHT
	// default and say so. Parity is inaudible: at -42.1 a full-scale input steers the depth by 0.8%,
	// which is a control nobody would find. Full sensitivity is the other failure — it lets the
	// envelope cancel the modulation entirely at 0 dBFS. -18 dB puts a 0 dBFS bus at 12.6% steering:
	// audible, and 18 dB of margin before it could reach cancellation. It is a starting point to be
	// set by ear, not a derived quantity.
	{ "env", "Env", -48.0, 0.0, -18.0, false, device::Unit::Decibel, {}, false, false,
	  "Input trim on the envelope follower, in dB. -42 is the hardware-reference level; 0 is full sensitivity" },

	// **AFTER `Env`, BECAUSE THE ENUM PUTS `Rotate` AFTER `EnvTrim` AND THIS ARRAY IS INDEXED BY IT.**
	// The first draft put this block before `Env` and the two went out of step: `kParamDescs[EnvTrim]`
	// returned ROTATE's descriptor — wrong name, wrong range, wrong default — so the Env knob was
	// reading another control's declaration. The `static_assert` below pins this array's LENGTH and
	// cannot see alignment, which is the same trap the shaper's Hysteresis entry fell into hours
	// earlier. The reachability sweep is what caught it, by reporting a control named `Env`
	// unreachable at an index nothing addressed under that name.
	// NO PERIOD (see `Param::Rotate`: a plane turn through `sliceBasisFor`, not an angle). Was "two pi is one
	// full turn" while the pad decoded through `phaseOffsetsFor`; true of that decode, not of this one.
	// Not modulatable: it re-aims a basis, and a basis that
	// moved under an LFO would make the pad's own axes a moving target.
	// **A SYMMETRIC DRAG WINDOW, NOT A DOMAIN**, like Explore's and Shaper's Rotate; the defect was
	// that AutoMod's rotate was bounded 0-100%. It was declared `0 .. 2pi`, one turn
	// and no further, which the editor drew as a knob running 0-100% of that turn: a re-aiming that
	// stops. Both references are unbounded and say why — Explore's rotate face is a COMPASS because
	// "the base is unbounded, so there is no end of travel", and `Shaper Rotate` declares this same
	// +/-8,388,608 window for the same reason.
	//
	// THE ENGINE ALREADY ACCEPTED ANY OF IT. `settingsOf` wraps rather than clamps — "a patch
	// carrying 7 radians means the same basis as one carrying 7 - 2pi, and clamping would silently
	// make those two patches different" — so nothing downstream changes here and no value newly
	// becomes reachable that the derive could not already take. Only the declaration was the wall.
	//
	// AND AN OLD PATCH READS AS ITSELF: every value the old range could hold (0 .. 2pi) is INSIDE
	// this one, so a stored number means exactly the basis it always meant. Nothing is rescaled and
	// nothing is migrated — the format is not broken here, it is widened.
	{ "rotate", "Rotate", -8388608.0, 8388608.0, 0.0, false, device::Unit::Number, {}, false, false,
	  "Turns the plane the phase pad travels in. Changes what a move MEANS, never the sound at rest; it "
	  "does not stop" },
	// **100% BY DEFAULT: THE FIELD'S OWN STEREO.** Zero is the mono it can be turned down to; the default is
	// what the field says, which is the device's premise. Modulatable — it is an amount, not a re-aiming.
	{ "width", "Width", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
	  "How far apart the two ears' modulation runs. 0 is mono; 100% is the field's own stereo" },
	// **ON BY DEFAULT: both filters tracked the note before the switch existed**, so a fresh device sounds as
	// it did. A two-state switch, which the multivalue-toggle rule allows.
	{ "keytrack", "Keytrack", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, false, false,
	  "Whether the filter and the comb follow the note's pitch. Off, both sit where the field puts them" },



};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// WHAT THE DEVICE SAYS ABOUT ITSELF.
/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "modulation", "fx" };

/// **HOW MANY FLOATS OF FOLDED TRAVEL THIS DEVICE CARRIES — one per phi triangle it drives.**
///
/// **NAMED `foldedTravel` AND NOT `drift`, DELIBERATELY, AND NOT `residue` EITHER.** On what
/// drift is: drift is a modulatable offset vector added to the current position in one direction on
/// the rotate plane — a 1-D vernier that semantically lies in the exploration plane. That is a live,
/// modulatable control IN the plane.
/// This is not that: it is what the pad's travel became when it was folded in on release — a position,
/// not a control, and not modulatable.
///
/// **AND NOT `residue`, WHICH WAS THE OTHER SUGGESTION.** Explore's `SliceResidue` is COMPENSATION —
/// *"what this sound's rotations banked, so that re-aiming its plane left it standing where it was"*.
/// A device that re-anchors on release has nothing to compensate, so borrowing that word would repeat
/// the homonym in the other direction: two different quantities under one name, one file apart, which
/// is exactly the trap the consolidation survey found with `offset` and the one that nearly landed
/// here with `drift`.
///
/// `foldedTravel` says what it is and collides with nothing.

inline constexpr size_t kFoldedTravelFloats = 39;

/// WHERE IT STARTS IN THE ROW: immediately past the declared parameters, which is where the blob
/// contract puts a device's opaque state (`device_row.h`, `kMaxDeviceFields`).
inline constexpr size_t kFoldedTravelFirst = static_cast<size_t>(Param::kCount);

inline constexpr device::Traits kTraits {
	.tag = "AMOD",
	.name = "Automod",
	.hasCard = true,
	// DECLINED FOR NOW, and device.h's own test applies: declining costs nothing, so decline until
	// the surface exists. Three coordinate boxes and six knobs fit a card; what an expanded view
	// would ADD — a picture of the LFO's waveform as the Mod coordinate turns — is worth building
	// and is not built, and declaring it would promise a surface the host cannot draw.
	// **TRUE SINCE THE PHASE PLANE EXISTS.** break out the window. use shaper as a guide.
	// The rack draws a card's expand mark from this declaration, so flipping it is what makes the
	// window reachable rather than merely present.
	.hasExpanded = true,
	// BUS ONLY. The comb holds two 2048-sample lines and the filter one SVF; per-voice would be that
	// times the voice count, and nothing has measured whether it is worth it. -1 is the honest
	// answer, spelled out so the next reader knows it was decided rather than forgotten.
	.perVoiceControl = -1,
	// THE OPAQUE BLOB: one float per phi triangle, folded travel. See `kFoldedTravelFloats` for why it
	// is not called drift and not called residue.
	.extraState = kFoldedTravelFloats,
	.extraStateKey = "travel",
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.mixInDevice = true,
};

/// THE FLATTENED STATE, one float per control, index-aligned with Param.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// THE DEVICE'S SETTINGS, in its own units. What a card edits and what the DSP reads.
struct Settings {
	bool enabled = true;

	/// **THE FOLDED TRAVEL — the device's opaque state blob, one float per phi triangle.**
	///
	/// NOT A PARAMETER AND NOT MODULATABLE. It is where the pad's travel goes when a gesture ends: the
	/// chart re-anchors at the current position, so what the hand covered is added here and the puck
	/// returns to centre. Zero for a device nobody has dragged, which is what makes every patch that
	/// predates it read identically.
	///
	/// STORED AS FLOAT, ACCUMULATED IN DOUBLE. By design, unbounded accumulation is fine as long
	/// as the surfaces are engineered not to saturate a double. So there is no bound and no
	/// "return home" verb; the obligation is on the arithmetic instead — see `deriveAutomod`.
	std::array<float, kFoldedTravelFloats> foldedTravel {};

	/// THE MEASURED DEFAULTS — see `kParamDescs` for how they were chosen and what they measure.
	///
	/// **ONE FIELD EACH, COLLAPSED FROM A PAIR (BJ piece 3).** The defaults are unchanged because the
	/// engine only ever read the SUM and the phase halves defaulted to zero: `0.30 + 0.0` is 0.30.
	/// That is the whole of why this collapse could not move the sound.
	float type = 0.30f;
	float flavor = 0.70f;
	float mod = 0.10f;
	float gamma = 0.0f;

	/// BIPOLAR, ZERO MEANING "WHERE THE FIELD PUT IT". A device that moved its own spectrum on load
	/// would be a device whose coordinates did not mean what they say.
	float freq = 0.0f;

	/// 100%, WHICH IS 1:1 WITH WHATEVER THE SYNC LIST SELECTED — by design, and the reason the
	/// double-click gesture needs no code: `knob.cpp:126` already resets to the declared default.
	float rate = 0.0f; // octaves: 1X (backlog DT)
	/// WHICH BASE RATE — a free Hz entry or a musical division. **DEFAULTS TO `0.1 Hz`, the entry
	/// closest to what shipped**: the old init was 0.143 Hz, a 7-second cycle, and defaulting to the
	/// `1 Hz` entry instead would have made the device seven times livelier at load by accident.
	int syncDivision = 0;
	LfoModeChoice lfoMode = LfoModeChoice::Free;
	float depth = 0.5f;
	float manual = 0.0f;
	float mix = 0.0f;
	/// THE ENVELOPE FOLLOWER'S INPUT TRIM, in dB. See Param::EnvTrim for why it exists and why -42.1
	/// is the number the firmware effectively uses.
	float envTrim = -18.0f;
	/// THE PHASE PAD'S PLANE TURN — `ps::basisFor`'s generator, no period. 0 is the standard basis. See Param::Rotate.
	float rotate = 0.0f;
	/// HOW FAR APART THE EARS RUN, 0..1. See Param::Width.
	float width = 1.0f;
	/// WHETHER BOTH FILTERS FOLLOW THE NOTE. See Param::Keytrack.
	bool keytrack = true;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// **THE ENABLE'S TWO DECLARATIONS, PINNED TOGETHER.** This device was one of the two whose numbers
/// had drifted apart — struct true, descriptor 0 — see `fx::owltt` for the full statement of why that
/// gives a device two different fresh states depending on which road it arrived by.
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

/// THE RATE A SURFACE WITH NO TEMPO SHOULD ASSUME.
///
/// A card reads the row and the row has no tempo, so a SYNCED entry has no answer there. This gives
/// the free entry's Hz times the multiplier, and falls back to the first free entry when the choice
/// is a division — a stated guess rather than a silent zero. Nothing a surface DRAWS depends on it:
/// `lfoHz` reaches only the spring frequencies, which `test_automod_derived.cpp` pins.
inline float nominalRateHz(const Settings& s) {
	const int choice = isFreeRate(s.syncDivision) ? s.syncDivision : 0;
	return freeRateHzFor(choice) * rateMultiplierFor(s.rate);
}

/// Settings from a flattened row, and back. Defined in automod.cpp beside the DSP that reads them.
[[nodiscard]] Settings settingsOf(const Row& row);
[[nodiscard]] Row rowOf(const Settings& settings);

/// HOW THE HOST RUNS THIS DEVICE'S AUDIO — defined in automod.cpp, where the DSP class is.
extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::ModulationFx,
};

} // namespace feathers::fx::automod
