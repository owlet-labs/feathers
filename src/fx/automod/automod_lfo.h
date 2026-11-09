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

#include "character/phi_lfo_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

// THE AUTOMODULATOR'S LFO — one phi-authored shape, one phase, and FOUR SIMULTANEOUS TAPS.
//
// WHAT THIS OWNS AND WHAT IT DOES NOT. `character/phi_lfo_shape.hpp` owns the WAVEFORM — where the
// corners are, computed from a zone coordinate. This owns WHEN IT IS READ: the phase, the rate, the
// run mode, and the four offsets the effect reads it at. The split is the reason neither file is a
// second MSEG: the shape is geometry with no clock, and this is a clock with no geometry.
//
// FOUR TAPS IS THE WHOLE REASON THIS IS NOT A RACK MODULATOR. `automodulator.cpp:846-857` reads one
// waveform at four phases in the same block — left, right, comb left, comb right — where a modulator
// on the bus publishes ONE value per tick. A state machine cannot be tapped, only cloned; a pure
// function of phase can, and that is what `evalLfoShape` is.
//
// ---------------------------------------------------------------------------------------------
// THE PHASE IS A WRAPPING 32-BIT INTEGER, WHICH IS WHAT MAKES BLOCK INVARIANCE EXACT.
// ---------------------------------------------------------------------------------------------
//
// `multimode_filter.h` states the rule this obeys — "a coefficient smoother stepped once per block,
// with a rate taken from the block length, is invariant only in algebra and not in floating point"
// — and `tests/test_synth.cpp` pins that a performance renders identically in blocks of 1 and 37.
//
// TWO EARLIER CLAIMS HERE WERE WRONG AND ARE CORRECTED RATHER THAN QUIETLY DROPPED, because both
// would have misled the next reader:
//
//   1. This file accused the firmware of block dependence for doing `lfoPhase += phaseInc *
//      bufferSize` once per buffer. IT IS NOT. That is a per-sample increment accumulated over the
//      buffer in UINT32 arithmetic, where `inc*(a+b) == inc*a + inc*b` exactly, so the firmware's
//      LFO is already block-invariant. The accusation was about the shape of the code rather than
//      about its arithmetic.
//
//   2. A float phase accumulated per sample was then called "block-invariant BY CONSTRUCTION...
//      nothing to drift". MEASURED, one second at 3 Hz and 48 kHz: blocks of 1 landed 3.0e-14 away
//      from blocks of 37, because 48,000 additions round differently from 94. Small, and real, and
//      not what "by construction" means.
//
// So the phase is a `uint32_t` turn, exactly as the firmware has it, and the invariance is a
// property of integer arithmetic rather than a tolerance anybody chose. It costs one multiply and
// one add per BLOCK, which is cheaper than the float version it replaced.
//
// ---------------------------------------------------------------------------------------------
// FREE-RUNNING Hz ONLY. TEMPO SYNC IS A SEAM, NOT A CONTROL.
// ---------------------------------------------------------------------------------------------
//
// The firmware offers free Hz OR a tempo-synced subdivision. Only the first is buildable today: the
// fx lane's whole contract is `prepare(sampleRate)` plus `processStereo(left, right, frames)` — no
// clock reaches a device here, verified across `multimode_filter.h`, `shaper.h` and `owltt.h`, where
// the modulator rack does get a `mod::TransportClock`. `mod/modulator.h` predicted exactly this:
// "the fx lane will want it the day anything there syncs".
//
// So `setRateHz` is the only rate road, and there is deliberately NO sync switch — the honesty rule
// says a control is enabled iff it changes sound, and one that cannot work must not be declared.
// When the clock is plumbed, `setRateHz` is the seam: a synced rate resolves to a frequency and
// arrives here, and nothing else in this file changes.

namespace feathers::fx::automod {

/// WHAT ONE BLOCK'S WORTH OF LFO LOOKS LIKE — four readings of one waveform.
///
/// READ AT THE BLOCK'S START, which is what the firmware does and is not merely an approximation of
/// per-sample modulation. These values drive a filter cutoff and a comb delay through spring
/// smoothers that interpolate across the block, so a per-sample LFO evaluation would be four
/// evaluations per sample to feed a smoother that only wants the endpoints.
struct LfoTaps {
	float left = 0.0f;
	float right = 0.0f;
	float combLeft = 0.0f;
	float combRight = 0.0f;

	friend bool operator==(const LfoTaps&, const LfoTaps&) = default;
};

/// HOW THE LFO ANSWERS THE GATE. The firmware's `AutomodLfoMode` minus STOP, and the omission is
/// deliberate rather than an oversight: STOP means "the effect ignores the LFO and uses its Manual
/// knob directly", which is a decision the EFFECT makes about which signal to read. An LFO that
/// knows how to not be an LFO is a mode in the wrong object.
enum class RunMode : uint8_t {
	/// Ignores the gate, runs continuously. The default, and what an auto-wah usually wants.
	Free,
	/// Resets to the start phase on a note.
	Retrigger,
	/// Plays one cycle from the start phase and then holds. Retriggered by a note.
	Once,
};

/// THE FREE RATE'S RANGE, and it is the firmware's exactly: 0.01 Hz (a 100-second sweep) to 20 Hz.
///
/// The firmware spells it `hz = 0.01 * 2^((rate-1)/127 * log2(2000))` over a 1..128 encoder. Written
/// here as the same curve over a 0..1 control, because 128 steps is a hardware encoder's resolution
/// and not a property of the sound.
inline constexpr float kRateHzMin = 0.01f;
/// **80 Hz, AND IT IS A MEASURED STOP WHERE 20 WAS AN INHERITED ONE.**
///
/// 20 came from the firmware, whose modulation grid is not ours. MEASURED here by driving the real
/// `Spring` with a sine and reading the settled amplitude: the springs pass cleanly to 80 — −0.5 dB
/// at 20 Hz, −2.9 dB at 50 — and only start eating the modulation at 100 (−8.2 dB). So 80 is where
/// this device's own smoothing begins to refuse, which is the honest place to stop.
///
/// ABOVE IT, FOR THE READER WHO WONDERS: it degrades gracefully rather than breaking — quieter and
/// rounder to about 375 Hz, which is the tick grid's Nyquist (`sampleRate / kTickSamples`, and the
/// host's buffer does not appear in that because the grid is stream-anchored). Only past 375 would it
/// alias. Audio rate is two removed levers away and was declined: the springs are what make this
/// device sweep rather than stutter, so a modulator fast enough to fight them is a different
/// instrument, not a wider range on this one.
inline constexpr float kRateHzMax = 80.0f;

/// The free rate a 0..1 control names. Exponential, so equal turns are equal RATIOS — the same
/// reason every other time and frequency control in this project is logarithmic.
/// HOW FAR THE RATE CONTROL SCALES ITS SELECTED BASE, in octaves either way. The same span as
/// `kFreqOctaves`, so the device's two bipolar performance controls move by the same amount over the
/// same travel.
inline constexpr float kRateMultiplierOctaves = 2.0f;

/// THE MULTIPLIER A RATE CONTROL VALUE MEANS. `value` is OCTAVES, the declared ±`kRateMultiplierOctaves`,
/// displayed as the multiple itself (`Unit::LogMultiple`: "0.25X", "1X", "4X").
///
/// **IT WAS 0..2 DISPLAYED AS 0%..200% (backlog DT, fixed 2026-09-15)**, and the readout and the curve
/// disagreed everywhere but 50% and 100%: "0%" ran at quarter speed and "200%" at four times. A
/// percentage of a linear range cannot print an exponential multiplier; the MSEG's Rate took
/// `LogMultiple` for the same reason, and now the stored number IS the octave.
///
/// SYMMETRIC IN RATIO ABOUT UNITY: equal travel either side gives reciprocal multipliers, so −1 and +1
/// octave sit the same distance from 1:1 in opposite directions. **Unity is EXACT at zero** — `exp2(0)`
/// is exactly one — which is what makes the default 1:1 by construction rather than by rounding.
inline float rateMultiplierFor(float value) {
	if (!std::isfinite(value)) {
		return 1.0f;
	}
	return std::exp2(std::clamp(value, -kRateMultiplierOctaves, kRateMultiplierOctaves));
}

class Lfo {
public:
	/// Told the sample rate, which is the only thing a frequency needs to become a phase increment.
	/// RESETS, for the reason `mseg::Shape::prepare` states: a rate change is not a gesture, and
	/// carrying a phase across one would invent a sweep out of a sample-rate switch.
	void prepare(double sampleRate) {
		sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
		reset();
	}

	/// BACK TO ZERO, gate down. What activate() means.
	///
	/// **ZERO RATHER THAN THE SHAPE'S START PHASE, and the distinction is the whole of a defect.**
	/// This used to go to `startPhase_`, which reads as the more thoughtful choice and is wrong: a
	/// FRESHLY CONSTRUCTED Lfo has phase 0 and `setShape` sets `startPhase_` WITHOUT moving the
	/// phase, so a reset device started somewhere a just-switched-on device did not. `test_fx_devices`
	/// caught it — "reset means AS IF JUST SWITCHED ON, not merely with the memory cleared" — and it
	/// only bit once the default coordinate produced a non-zero start phase.
	///
	/// The start phase is where a RETRIGGER lands, which is what `trigger()` is for and what
	/// `automod.h` calls it. A stream beginning is not a retrigger.
	void reset() {
		phase_ = 0;
		travelledSinceTrigger_ = 0;
		cyclesDone_ = false;
	}

	/// REBUILD THE WAVEFORM. Called when the zone coordinate moves, which is a knob gesture and not
	/// an audio-thread event — but it allocates nothing, so calling it per block is merely wasteful
	/// rather than unsafe.
	///
	/// THE START PHASE COMES FROM THE FIELD TOO, which is why it is set here rather than by a
	/// separate call that could be forgotten. The firmware derives it from the same coordinate
	/// (`getLfoInitialPhaseFromMod`), so moving the coordinate moves both the shape and where a
	/// retrigger starts in it — one gesture, one coherent change.
	/// **HANDED THE WAVEFORM RATHER THAN BUILDING A SECOND ONE**, and this was the other way round
	/// until a known-bad proved it could not be.
	///
	/// `deriveAutomod` ALREADY BUILDS THIS SHAPE — `AutomodDerived::shape`, which the assembly reads
	/// every tick to modulate the band mix. This object used to build its OWN from the coordinate,
	/// so the device carried two copies of one waveform that agreed only because two call sites
	/// happened to pass the same three arguments.
	///
	/// AND THE DRIFT WAS INVISIBLE. A mutation that dropped the device gamma from `coordsOf` — so
	/// the derive's shape moved and this one did not — left **555 cases green across four binaries**,
	/// including the honesty sweep, which still saw Gamma change the sound because it reached it
	/// through the road that had NOT been cut. Two roads to one quantity is exactly the redundancy
	/// CLAUDE.md says to check before trusting; this one was load-bearing for nothing.
	///
	/// The overload below keeps the coordinate form for the cases that are about THIS object rather
	/// than about the assembly, and it is written in terms of this one so it cannot become a second
	/// definition again.
	///
	/// **AND THAT PHRASING IS THE GUARANTEE — NO TEST COVERS THIS.** Putting the duplicate back is
	/// BEHAVIOURALLY INERT: both roads compute the same waveform from the same coordinate, so every
	/// suite stays green (measured — 499 + 17 + 13 + 28 cases, all passing with the duplicate
	/// restored). **That is exactly why the duplicate was dangerous rather than wrong.** A structural
	/// fault is not a behavioural one, and no assertion can be written against "there are two
	/// definitions that currently agree". What holds it shut is that the coordinate overload DELEGATES
	/// rather than repeating the call — it cannot fork by construction, which is the only kind of
	/// guarantee available here. A comment asking the next reader not to inline it would not be one.
	void setShape(const character::LfoShape& shape, float startPhase = 0.0f) {
		shape_ = shape;
		startPhase_ = turnsToU32(startPhase);
	}

	void setShape(float coord, float phaseOffset, float startPhase = 0.0f) {
		setShape(character::lfoShapeAt(coord, phaseOffset), startPhase);
	}

	/// The free-running rate, in Hz. THE ONLY RATE ROAD — see the file header on why there is no
	/// synced one yet, and why this is the seam a synced one would arrive through.
	void setRateHz(float hz) {
		const float safe = std::isfinite(hz) ? std::clamp(hz, kRateHzMin, kRateHzMax) : kRateHzMin;
		// Turns per sample, as a fraction of a full 2^32 turn. At the slowest rate this is 894 —
		// three orders above the 1-count quantum, so the taper never collapses onto a step.
		increment_ = static_cast<uint32_t>(
		    std::llround(static_cast<double>(safe) / sampleRate_ * 4294967296.0));
	}

	/// How far the right channel trails the left, in turns. The firmware derives this from the Mod
	/// coordinate; this object only needs the number.
	void setStereoOffset(float turns) { stereoOffset_ = finiteOr(turns, 0.0f); }

	/// How far the comb taps trail the filter taps, in turns. Derived from the Flavor coordinate.
	void setCombOffset(float turns) { combOffset_ = finiteOr(turns, 0.0f); }

	void setRunMode(RunMode mode) { mode_ = mode; }
	[[nodiscard]] RunMode runMode() const noexcept { return mode_; }

	/// A NOTE ARRIVED. Retrigger and Once restart from the shape's start phase; Free ignores it,
	/// which is the whole of what Free means.
	void trigger() {
		if (mode_ == RunMode::Free) {
			return;
		}
		phase_ = startPhase_;
		travelledSinceTrigger_ = 0;
		cyclesDone_ = false;
	}

	/// READ THE FOUR TAPS AT THE CURRENT PHASE, then advance by `frames` samples.
	///
	/// READ-THEN-ADVANCE, and the order is load-bearing: the first block after a trigger must report
	/// the shape's START, not one block past it. A retrigger that skipped its own first value would
	/// be inaudible on a fast LFO and quietly wrong on a slow one.
	LfoTaps tick(int frames) {
		const LfoTaps taps = read();
		advance(frames);
		return taps;
	}

	/// The four taps WITHOUT advancing — what a card draws, and what a case asserts. Exposed because
	/// a value cannot be inferred from a rendered buffer.
	[[nodiscard]] LfoTaps read() const {
		const float base = turnsOf(phase_);
		return LfoTaps {
			character::evalLfoShape(shape_, base),
			character::evalLfoShape(shape_, base + stereoOffset_),
			character::evalLfoShape(shape_, base + combOffset_),
			character::evalLfoShape(shape_, base + combOffset_ + stereoOffset_),
		};
	}

	/// **PUT THE PHASE WHERE THE SONG SAYS — backlog DQ.**
	///
	/// `turns` is how far through the cycle the playhead is, 0..1, and the caller computes it from the
	/// timeline its own period is measured in. The shape's own start phase is added here rather than
	/// by the caller, so song position zero is the shape's START and not an arbitrary point in it.
	///
	/// **UINT32 WRAPAROUND IS THE ARITHMETIC, NOT AN ACCIDENT.** The phase is kept as a wrapping
	/// integer precisely so a sum of turns needs no `fmod` and cannot drift: adding the start phase to
	/// a locked position wraps exactly, at any position, forever.
	///
	/// **IT DOES NOT TOUCH `Once`'s HOLD.** Locking is a Free-mode idea — a shape that has finished
	/// and is holding has an answer already, and moving it would restart something the mode says is
	/// over. The caller gates on the mode; this is stated here because the gate is not visible from
	/// inside.
	void lockPhaseTo(double turns) {
		const double wrapped = turns - std::floor(turns);
		// `wrapped` CAN BE EXACTLY 1.0 — for turns in (-2^-54, 0) the subtraction rounds up — and
		// 1.0 * 2^32 is one past uint32's range, an undefined cast (backlog EH, shape 3). The sibling
		// `turnsToU32` below already carries the `< 1` guard this one lacked.
		const double w = std::isfinite(wrapped) && wrapped < 1.0 ? wrapped : 0.0;
		phase_ = startPhase_ + static_cast<uint32_t>(w * 4294967296.0);
	}

	/// WHERE THE PHASE IS, 0..1. A test-only seam in the sense CLAUDE.md means: "if the claim is
	/// 'this value is X at this instant', read the value". Four taps of one shape cannot be
	/// unpicked back into a phase from the outside.
	[[nodiscard]] double phase() const noexcept {
		return static_cast<double>(phase_) / 4294967296.0;
	}

	/// The waveform currently loaded, so a case can assert the shape and the reading separately.
	[[nodiscard]] const character::LfoShape& shape() const noexcept { return shape_; }

	/// Once mode has finished its cycle and is holding.
	[[nodiscard]] bool finished() const noexcept { return mode_ == RunMode::Once && cyclesDone_; }

private:
	static float finiteOr(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

	/// A 0..1 turn as the wrapping integer the phase is kept in.
	static uint32_t turnsToU32(float v) {
		if (!std::isfinite(v)) {
			return 0u;
		}
		const float w = v - std::floor(v);
		return static_cast<uint32_t>(static_cast<double>(w < 1.0f ? w : 0.0f) * 4294967296.0);
	}

	/// And back, for the shape's evaluator, which reads a 0..1 phase.
	static float turnsOf(uint32_t u) { return static_cast<float>(static_cast<double>(u) / 4294967296.0); }

	/// PER SAMPLE, NOT PER BLOCK — see the file header. The loop is one add and a compare; it is
	/// written as a closed form plus a wrap rather than a loop because the closed form is exact for
	/// a constant increment and the two cannot disagree.
	void advance(int frames) {
		if (frames <= 0 || (mode_ == RunMode::Once && cyclesDone_)) {
			return;
		}
		// EXACT, AND EXACTLY WHAT A PER-SAMPLE LOOP WOULD PRODUCE: unsigned multiply and add are
		// modular, so any decomposition of `frames` gives the identical result. Widened to 64 bits
		// for the Once accumulator below, which needs the un-wrapped distance.
		const uint64_t travelled = static_cast<uint64_t>(increment_) * static_cast<uint64_t>(frames);

		if (mode_ == RunMode::Once) {
			// ONE CYCLE FROM THE START PHASE, and the distance is measured from where the cycle
			// BEGAN rather than by watching for a wrap. A wrap test cannot tell "finished one cycle"
			// from "started just below 1.0 and crossed it immediately", which is what a non-zero
			// start phase makes the common case rather than the corner one.
			travelledSinceTrigger_ += travelled;
			if (travelledSinceTrigger_ >= 4294967296ull) {
				cyclesDone_ = true;
				// EXACTLY ONE CYCLE LANDS BACK ON THE START PHASE, so that is where it holds — not
				// at a pinned zero, which an earlier draft of this comment claimed. The shape's
				// endpoints are pinned at rest, but the START PHASE need not be an endpoint: the
				// field picks it, so a Once sweep holds at whatever the shape reads there. Snapping
				// to the exact start rather than to `phase_` keeps the hold independent of how the
				// block boundaries happened to fall.
				phase_ = startPhase_;
				return;
			}
		}

		// Wrapping is what a uint32 does on its own; there is no branch and no epsilon.
		phase_ += static_cast<uint32_t>(travelled);
	}

	character::LfoShape shape_ {};
	double sampleRate_ = 48000.0;
	/// Turns per sample, as a fraction of 2^32.
	uint32_t increment_ = 0;
	/// Where in the cycle we are, as a fraction of 2^32. Wraps by construction.
	uint32_t phase_ = 0;
	uint32_t startPhase_ = 0;
	/// UN-wrapped distance since the last trigger, for Once. 64-bit because it must be able to
	/// exceed one full turn in order to notice that it has.
	uint64_t travelledSinceTrigger_ = 0;
	float stereoOffset_ = 0.0f;
	float combOffset_ = 0.0f;
	RunMode mode_ = RunMode::Free;
	bool cyclesDone_ = false;
};

} // namespace feathers::fx::automod
