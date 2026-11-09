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
#include "device/transport_lock.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>

// WHAT EVERY MODULATOR IN THE RACK SHARES, as opposed to what any one of them is.
//
// THIS FILE EXISTS BECAUSE THE SECOND DEVICE ASKED FOR IT, which is the whole argument for building a
// second one. The device-browser design puts it plainly: the MSEG
// "is not a bonus — it is the TEST. A second modulator type with a different control count is the
// only thing that demonstrates pin #1 is actually gone rather than merely widened."
//
// The first thing that test found was not in the rack at all. It was here: `dahdsr_types.h` owned
// `Trigger`, the control tick, the log2-seconds range a time knob runs on, the shape range, and the
// off/on names — and every one of those is a fact about MODULATORS rather than about a DAHDSR. A
// second device could not have any of them without either including its neighbour's header or
// restating the numbers, and restating them is how two devices come to disagree about what "10
// seconds" or "one tick" means.
//
// SO THE RULE THIS FILE ENCODES: a constant belongs here when a DIFFERENT modulator would need the
// same answer, and stays in its device's own header when the answer is that device's business.
// `kStageLog2SecondsMax` is here because a time is a time. `kSustainShare` — how wide a card draws a
// plateau — is not, because it is one card's drawing decision.
//
// It is deliberately NOT `device/device.h`. That file is the vocabulary a device declares itself in,
// and an fx unit uses it too; a trigger mode and an envelope tick are the modulator tier's, and an
// fx unit has no business with either.

namespace feathers::mod {

/// THE CONTROL TICK, in samples, for every modulator in the rack.
///
/// One number, and it has to be one: `synth.cpp` static_asserts it against `PuckMotion::kTickSamples`
/// so the rack and the puck advance on the same grid. 64 samples is 750 Hz at 48 kHz.
///
/// It is also a power of two, which the rack's block arithmetic assumes and test_mod_dahdsr restates
/// — which 64 is.
inline constexpr int kBlockSamples = 64;

/// WHERE A TIME KNOB RUNS, in log2 seconds, and why these two numbers.
///
/// Even in octaves rather than in seconds, which is this project's rule for every time and frequency
/// control it has (see the filter's LogHertz, and glideSeconds). A knob linear in seconds spends four
/// fifths of its travel between one second and ten, where nobody needs resolution, and gives the
/// millisecond end — where an attack actually lives — a pixel.
///
/// The bottom is 2^-11 ≈ 0.49 ms: shorter than the shortest transient the ear resolves, so the knob
/// reaches "instant" without needing a special case. The top is 10 s, Bitwig's own ceiling for an
/// AHDSR stage and the parity target.
///
/// A knob at EXACTLY THE BOTTOM is not 0.49 ms, it is a stage that does not happen — device.h states
/// that convention for every LogSeconds range and both modulators keep it.
inline constexpr float kTimeLog2SecondsMin = -11.0f;
inline constexpr float kTimeLog2SecondsMax = 3.32192809489f; // log2(10)

/// WHERE A SMOOTHING TIME RUNS, on the same kind of taper: 2^-11 ≈ 0.49 ms to 2^0 = 1 s.
///
/// SMOOTHING IS ABOUT THE DESTINATION, NOT ABOUT THE MODULATOR — which is why it is shared rather
/// than each device's own. A DAHDSR is already continuous and so is a well-drawn MSEG; what is not
/// continuous is what a fast one does at the far end of a route. Point one at Path and the character
/// is rebuilt every tick, and a 1 ms attack asks the bank for a new build 750 times a second.
inline constexpr float kSmoothingLog2SecondsMin = -11.0f;
inline constexpr float kSmoothingLog2SecondsMax = 0.0f; // log2(1)

/// How far a shape control bends its segment, each way. Bipolar, like Bitwig's per-stage curve.
///
/// POSITIVE MEANS FULLER, ON EVERY SEGMENT, RISING OR FALLING — a convention rather than a
/// consequence, and it needed choosing. One curve read the same way on every segment bends a rising
/// segment and a falling one in opposite senses relative to "full"; two
/// controls side by side on one card have to answer to the same hand. The DAHDSR reads its curve
/// backwards on its falling stages for exactly this. An MSEG has no fixed idea of which of its segments rise, so it
/// needs the convention stated even more than the DAHDSR did.
inline constexpr float kShapeMax = 1.0f;

/// THE HOST'S CLOCK — **MOVED TO `device/device.h`, AND THIS ALIAS IS WHY NOTHING ELSE CHANGED.**
///
/// This file's own comment predicted the move and gave the reason: *"NAMED FOR WHAT IT IS RATHER
/// THAN FOR WHO PASSES IT... The fx lane will want it the day anything there syncs, and a device in
/// that lane should not have to read 'rack' to find out what time it is."* That day arrived — automod's sync was asked for —
/// so the type now lives in the vocabulary BOTH lanes share rather than in
/// the modulator tier, and `device/registry.h` can name it without an fx device reaching into `mod/`.
///
/// THE ALIAS STAYS RATHER THAN THE 41 REFERENCES MOVING. `mod::TransportClock` is what the DAHDSR,
/// the MSEG, the synth and three test files call it, and renaming all of them to prove a point about
/// filing would be a large diff carrying no meaning. The type moved; the name it is known by here
/// did not.
using TransportClock = device::TransportClock;

/// WHAT STARTS A MODULATOR'S SHAPE, AND HOW IT ANSWERS THE GATE. Five modes, one dropdown — the
/// three old controls (`Trigger`, `Loop`, `SingleTrigger`) collapsed into one because their combinations
/// overlapped: "Once" was `SingleTrigger=On + Loop=Off`, and "Free + retrigger" was not expressible at
/// all. The modes are mutually exclusive answers to "when does it run and what restarts it," which is
/// what the three toggles were trying to say.
///
/// The gate in this rack is GLOBAL rather than per-note, so "the whole rack restarts together" is a
/// concept every device here has from its first day. The modes differ in whether they hear the gate at
/// all, not in what a gate means.
enum class Mode : uint8_t {
	/// Runs while gated, restarts on every note. What an envelope is, and the default.
	///
	/// **ON THE LFO: AT THE KEY THE OUTPUT GOES TO NEUTRAL AT ONCE** — 0, or the centre when bipolar, reached through the
	/// device's own smoothing so it does not click; the phase stops. Per note under Poly; the mono wave when the
	/// rack's gate falls. Before this the LFO heard the gate only as a restart and cycled for ever after the key.
	Gate,
	/// Cycles while gated, restarts on every note. A looping envelope — the gate has to be held for
	/// it to keep turning.
	///
	/// **ON THE LFO: AT THE KEY IT FINISHES THE CYCLE IT IS IN, THEN RESTS AT NEUTRAL**,
	/// reaching neutral through the smoothing when the cycle's end is not already there (a phase offset, a saw). A
	/// cycle ends where it began, one turn from the start phase; a random walk's cycle is its step. This is what
	/// makes Loop differ from Gate and from LFO Retrig, which keeps running after the key.
	Loop,
	/// One full cycle, then holds at the end. Ignores everything after — a stab that cannot be
	/// re-fired by the next note in a held chord; only a gate that has fallen and risen again starts
	/// a new one.
	///
	/// **ONE SHOT PER NOTE UNDER POLY, PER PHRASE UNDER MONO** — the device's Poly switch decides
	/// (`mod::PolyShape`). Poly: each note runs its own shape, so Once is one shot per NOTE.
	/// Mono: one shape gated by the rack's global gate, where `retrigger` is the only evidence a second note
	/// exists, so Once is one shot per PHRASE. Before the MSEG had a switch it was
	/// mono only, and a second note restarted its one shape under every voice still ringing (2026-10-03: with Once set,
	/// a subsequent note in poly mode cut the earlier one off).
	///
	/// **AND ON A SUSTAINING ENVELOPE IT HAS TO MAKE ITS OWN END.** "Holds at the end" is plain for a
	/// shape of fixed length, which the MSEG and the LFO both have. A DAHDSR's last stage IS the
	/// release and the release is what note-off MEANS, so it had no end of its own and Once quietly
	/// became "Gate minus retrigger minus loop" — measured holding at sustain under a held key. It
	/// lets go of its own gate at sustain now (`dahdsr.cpp`), so A → D → R runs on one clock and the
	/// keyboard can neither lengthen nor shorten it.
	Once,
	/// Runs continuously, ignores the gate. A free-running LFO — no retrigger, no sustain, just
	/// cycles.
	Free,
	/// Runs continuously AND restarts on every note. A phase-reset LFO: it ignores the gate (keeps
	/// running when nothing is held) but a note-on resets the phase to the start, so every note begins
	/// the same point in the cycle. The mode the old three-control layout could not build.
	LfoRetrig,
	kCount,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kModeNames[] = { "Gate", "Loop", "Once", "Free", "LFO Retrig" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

/// Seconds from a log2-seconds control, with the bottom of the range meaning "no time at all" — the
/// convention device.h states for every LogSeconds range. Shared because both modulators decode a
/// time the same way, and a second copy is a second place for the convention to be forgotten.
inline double timeSeconds(float log2Seconds) {
	// AT THE MINIMUM THE TIME DOES NOT HAPPEN — the one special case, and it lives here so nobody else
	// has to know it. There is no zero in a logarithmic domain, so the bottom of the travel is what
	// carries "off"; reading back 0.49 ms instead would be a card promising a stage the state machine
	// is about to skip.
	//
	// NOT constexpr, deliberately: std::exp2 and std::isfinite are not, and a constexpr function that
	// can never be constant-evaluated is ill-formed. The DAHDSR's version was a plain function for the
	// same reason and this is that function, moved rather than rewritten.
	if (!std::isfinite(log2Seconds) || log2Seconds <= kTimeLog2SecondsMin) {
		return 0.0;
	}
	return std::exp2(std::min(static_cast<double>(log2Seconds),
	                          static_cast<double>(kTimeLog2SecondsMax)));
}

/// **THE ONE-POLE COEFFICIENT A SMOOTHING CONTROL ASKS FOR — one law, and the bottom of the travel is
/// OFF.**
///
/// **THERE WAS A SWITCH BESIDE EVERY ONE OF THESE KNOBS AND IT WAS A TRAP.** A control whose value is ignored until a second control says
/// otherwise is two facts where the player is holding one, and the failure mode is silent in the
/// direction that wastes time: the knob moves, the number changes, the sound does not.
///
/// **IT COULD BE DELETED BECAUSE THE RANGE ALREADY CARRIED "OFF".** `kSmoothingLog2SecondsMin` is the
/// same "a stage that does not happen" convention `timeSeconds` states above and `device.h` states for
/// every LogSeconds range — the switch was a second spelling of a position the knob already had.
///
/// **AND THE THREE DEVICES DID NOT AGREE ON THAT POSITION, WHICH IS THE OTHER REASON THIS IS ONE
/// FUNCTION.** Before it: the MSEG read the bottom as zero and passed through (right); the DAHDSR
/// clamped through its own `smoothingSeconds` and so bottomed out at 0.49 ms rather than off; the LFO
/// tested `smoothing > -10.0f`, a literal one octave above its own declared minimum, so the bottom
/// OCTAVE of its travel did nothing while the box went on printing a time. Three laws for one idea is
/// how they came to disagree, which is the argument this whole file is built on.
/// **THE SMOOTHING TIME CONSTANT IN SECONDS, OR ZERO FOR OFF** — one place that knows what "off" is.
///
/// Split out of `smoothingCoeff` because a SURFACE needs the tau without the coefficient: the LFO's
/// card draws a one-pole's steady-state response to a periodic wave, which depends on `tau / period`
/// and not on any per-tick step. Before this there were three spellings of the same test in the tree
/// — the coefficient's, `randomTrace`'s, and the engine's old `> -10.0f` literal — and the third was
/// wrong by an octave. A predicate this small is exactly the kind that drifts.
inline double smoothingTau(float log2Seconds) {
	if (!std::isfinite(log2Seconds) || log2Seconds <= kSmoothingLog2SecondsMin) {
		return 0.0; // the bottom of the travel is OFF, which is the whole of the control
	}
	return std::exp2(std::min(static_cast<double>(log2Seconds),
	                          static_cast<double>(kSmoothingLog2SecondsMax)));
}

inline double smoothingCoeff(float log2Seconds, double tickSeconds) {
	const double tau = smoothingTau(log2Seconds);
	if (!(tau > 0.0) || !(tickSeconds > 0.0)) {
		return 1.0; // pass-through — the new value IS the output, which is what "off" means here
	}
	// THE EXACT POLE, not `T / (T + tau)`. The approximation is the first term of this one's series and
	// they part company exactly where a fast smoothing lives — at tau = T it reads 0.5 against 0.632,
	// so the shortest usable settings were the ones most wrong. The MSEG and the DAHDSR already used
	// this form; the LFO used the other.
	return 1.0 - std::exp(-tickSeconds / tau);
}

/// **THE ONE-POLE'S REST: within this of its target it IS its target** — a millionth of full scale,
/// -120 dB, and the second half of the law `smoothingCoeff` is the first half of. A one-pole approaches
/// and never arrives, and all three
/// modulators showed it, MEASURED: the DAHDSR froze a finished note mid-tail; the MSEG, in double,
/// was still 1.8e-6 short of its last level 14 s after a 1 s smoothing began to settle and reported
/// idle while it crawled; and the LFO, in FLOAT, STALLED for ever 2.2e-5 short of where a Once ended,
/// because a step of `c · diff` below half a float ulp adds nothing. Reached at most
/// tau · ln(10^6) ≈ 13.8 tau after the shape stops moving, and the snap cuts off less than a
/// destination can resolve, so the curve while it moves is the one-pole's.
inline constexpr double kSmoothingRest = 1.0e-6;

/// ONE STEP OF THE ONE-POLE, SNAPPED AT REST — `y` toward `target` by `coeff` (`smoothingCoeff`), and
/// onto it exactly once within `kSmoothingRest`. In DOUBLE, because in float the step stalls short of
/// the snap at slow settings (see above). Every modulator's smoothing is this line.
inline void smoothingStep(double& y, double target, double coeff) {
	y += (target - y) * coeff;
	if (std::abs(target - y) < kSmoothingRest) {
		y = target;
	}
}

/// The inverse, for a card or a case that wants to say "250 ms" and get the stored number. Clamped,
/// so a duration past the top of the range lands on the top rather than off it.
inline float timeValueFor(double seconds) {
	if (!(seconds > 0.0)) {
		return kTimeLog2SecondsMin; // "no time", which is the bottom of the travel
	}
	return static_cast<float>(std::clamp(std::log2(seconds),
	                                     static_cast<double>(kTimeLog2SecondsMin),
	                                     static_cast<double>(kTimeLog2SecondsMax)));
}

/// **HOW A FREE SHAPE FOLLOWS THE SONG** — `device/transport_lock.h`, which says why. Aliased like
/// `TransportClock`, because automod's LFO in the fx lane needs the same answer.
using TransportLock = device::TransportLock;

/// WHERE A MODULATOR STANDS, IN THE ONE VOCABULARY BOTH DEVICES CAN ANSWER IN.
///
/// **AN ORDINAL AND A PHASE WITHIN IT.** An MSEG answers *(which segment, how far through it)*; a
/// DAHDSR answers *(which stage, how far through it)*. The two devices have nothing else in common
/// at this level — one has a variable segment list, the other a fixed six-stage machine — and they
/// do not need anything else, because a playhead is a position on a drawn line and both lines are
/// drawn as an ordered run of pieces.
///
/// **NEITHER IS RECOVERABLE FROM THE VALUE, WHICH IS WHY THIS TYPE EXISTS AT ALL.** `mseg.h`'s own
/// `Position` states it first: *"a value alone cannot say which segment produced it — an MSEG can
/// pass through the same level three times"*. An envelope is no better — decay and release cross the
/// same levels going the same direction. CLAUDE.md's rule, arriving as a data type.
///
/// **IDLE IS A STATE AND NOT A COORDINATE.** A finished shape and a `Stage::Idle` envelope are not
/// AT the end, they are nowhere, and a marker drawn at "the end" would be saying something the
/// device is not doing — the honesty rule applied to ink. `kIdle` is the ordinal for that, which
/// keeps the whole reading in one word and out of a second field a reader could forget to check.
struct Playhead {
	/// The ordinal that means "not running".
	static constexpr uint32_t kIdle = 0xFFFFFFFFu;

	/// Which segment (MSEG) or stage (DAHDSR) is running.
	uint32_t ordinal = kIdle;
	/// How far through it, 0..1.
	float phase = 0.0f;
	/// **WHERE THE SHAPE STANDS, when the drawn line cannot say** — the RAW level, 0..1, before smoothing and any
	/// bipolar rescale, published only by a device whose position along its picture does not fix its level. The
	/// DAHDSR's release runs from wherever the gate fell and its attack from wherever it was restarted, while its
	/// card draws both from the stage corners: the star
	/// was placed on the drawn release, which starts at sustain. `haveLevel` false is "not published".
	float level = 0.0f;
	bool haveLevel = false;
	/// **HOW MANY TIMES THE DEVICE'S TRIG HAS FIRED** (backlog MV), carried beside the position because it travels the
	/// same road to the same card — the Trig pad lights when it moves. `haveTrigFires` false is "no Trig".
	uint32_t trigFires = 0;
	bool haveTrigFires = false;
	/// **AND THE NEWEST EVENTS, NEWEST FIRST** (`ModOps::recentEvents`), on the same road for the same card: what a frame
	/// that saw several samples or hits draws them from. All zero where a device offers none.
	std::array<device::RecentEvent, device::kRecentEvents> recent {};

	[[nodiscard]] bool running() const noexcept { return ordinal != kIdle; }

	friend bool operator==(const Playhead&, const Playhead&) = default;
};

} // namespace feathers::mod
