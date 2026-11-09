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

#include "fx/filter/multimode_filter.h"

#include "dsp/sinc_delay_line.h"

#include <algorithm>
#include <cmath>

// THE AUTOMODULATOR'S COMB — a feedback delay whose time the LFO sweeps.
//
// NOTHING HERE IS A NEW DELAY LINE. `feathers::dsp::SincDelayLine` is the primitive, and
// it is the same one `multimode_filter.cpp`'s own Comb type already runs — so this is a second CALLER
// of a line the repository already has, not a second line.
//
// AND IT IS A DIFFERENT OBJECT FROM THAT COMB, which is worth saying because the names collide. The
// filter's `Type::Comb` is a FILTER: one delay tuned to a corner frequency, with damping, sitting in
// a dropdown beside eight other filters. This is an EFFECT STAGE: a delay swept by an LFO, mixed
// ADDITIVELY into the signal that fed it, with a stereo-collapse control. Same primitive, different
// instrument — which is why this does not try to reuse `MultimodeFilter` and does reuse the line
// underneath it.
//
// THE MIX IS AN ADDITION, NOT A CROSSFADE, and that is the firmware's and it is the character.
// `automodulator.cpp:1255` is `out = out + combOut * mix` — the comb is ADDED on top of the signal
// rather than blended against it, so raising Mix makes the sound bigger as well as combier. A
// crossfade would be the obvious "correct" reading and it would sound wrong.
//
// ---------------------------------------------------------------------------------------------
// SINC INTERPOLATION, BY DECISION — AND IT COST FOUR SAMPLES OF RANGE AT THE BOTTOM.
// ---------------------------------------------------------------------------------------------
//
// The sinc line once offered `read()` (sinc-interpolated) and `readLinear()`. The firmware
// interpolates linearly between two integer taps, which is an ARM CPU decision — sinc measurably
// reduces the aliasing a fast delay sweep produces and on a desktop costs nothing. This file first
// shipped with `readLinear` deliberately, because a linearly interpolated modulated delay has a
// particular grainy character and quietly upgrading it would have been changing the instrument while
// claiming to port it. Decided alongside the filter core: modern on both, so: sinc.
//
// IT IS NOT A ONE-LINE SWAP, AND THE REASON IS WHY THE MINIMUM DELAY MOVED FROM 2 TO 6.
// `SincDelayLine::read` centres a twelve-tap window SIX SAMPLES BEHIND the read point —
// its oldest tap is `iDelay + kHalfTaps` writes back — so a delay under six reaches past the write pointer
// for samples that have not been written yet, and comes back holding whatever was in those slots a
// whole line ago. `readLinear` reads only two adjacent taps and is safe at 2. **So sinc FORCES the
// shorter delays away**, and `filter_types.h` already records the same constraint for the same
// primitive, arriving at the same 6.
//
// THE AUDIBLE CONSEQUENCE, STATED BECAUSE IT IS A RANGE REDUCTION NOBODY ASKED FOR: a comb's first
// resonance is `sampleRate / delay`, so a six-sample floor is an 8 kHz ceiling at 48 kHz where the
// firmware's two-sample floor reached 24 kHz. It RISES with the sample rate — 16 kHz at 96 kHz.
//
// AND IT IS THE HOUSE RULE RATHER THAN A NEW LIMITATION, which is the difference between "we lost
// something" and "we joined the rest of the codebase". `filter_types.h` ALREADY carries
// `kCombMinDelaySamples = 6.0f` and a `combCeilingHz(sr) = sr / 6` for precisely this reason — our
// own comb FILTER has had this ceiling since it was written, for the same primitive and the same
// twelve-tap window. The automod is now consistent with it.
//
// DECIDED WITH THE MECHANISM SPELLED OUT — that the window reaches past the read point and so
// cannot sit within six samples of the write pointer — and taken knowingly: sinc stays for now.
// The remaining option nobody has costed is oversampling the comb 2x, which is the only way to have
// both the interpolation and the range; it is recorded in the backlog rather than left implicit. The
// hybrid (sinc above six samples, linear below) is closed: a discontinuity in the CHARACTER in the
// middle of a sweep is worse than either endpoint.
//
// NO ALLOCATION. The line is a fixed-size array inside the object, so the whole comb is built when
// the device is, and `prepare` only clears it.

namespace feathers::fx::automod {

/// **THE COMB'S DELAYS ARE AUTHORED AT 48 kHz AND SCALED TO THE HOST'S RATE (backlog IS.5).** Every delay the
/// field and the effect compute — the base, the modulation range, the longest delay — is in REFERENCE
/// samples at this rate, and `Comb` multiplies by `sampleRate / kCombReferenceRate` where it reads the line.
/// Before this the delays were raw samples: a comb is `sampleRate / delay` in pitch, so the same patch rang an
/// octave higher at 96 kHz and 1.4 semitones lower at 44.1 kHz. The one number NOT scaled is the floor below,
/// which is a property of the interpolator rather than a pitch.
inline constexpr double kCombReferenceRate = 48000.0;

/// THE HIGHEST HOST RATE THE LINE IS SIZED FOR. Above it the longest delays clamp to the line, so only the very
/// bottom of the range drifts — see `combMaxDelayFor`.
inline constexpr double kCombMaxSampleRate = 192000.0;

/// **THE LOWEST NOTE THE COMB REACHES WITH THE LFO AT REST: D♭1**, by requirement. The field's longest BASE delay is this pitch's period, and the LFO's
/// sweep reaches below it. MIDI 25, 440 · 2^((25 − 69) / 12).
inline constexpr double kCombRestFloorHz = 34.647828872109;

/// HOW FAR ±1 OF LFO MAY MOVE THE DELAY, in reference samples — the firmware's 400.
inline constexpr float kCombMaxModRangeSamples = 400.0f;

/// THE LINE, sized so the longest delay at `kCombMaxSampleRate` fits with the interpolator's window.
/// NOT SHARED WITH `filter::kCombLineSamples` — that one is sized by a 20 Hz tuning floor at the host's own
/// rate; this one by the rest floor plus the LFO's reach, at the highest rate.
inline constexpr int kCombLineSamples = 8192;

/// THE SHORTEST DELAY, in samples — DERIVED FROM THE INTERPOLATOR RATHER THAN CHOSEN, and the one number here
/// that is NOT scaled with the rate. `SincDelayLine::read` centres its twelve-tap window `kHalfTaps`
/// samples behind the read point, so anything shorter reads slots the write pointer has not reached.
/// Computed from the table's own constant so that a vendored change to the window size moves this with it.
/// The firmware's floor is 2, which is safe for its linear read and is not safe for ours.
inline constexpr float kCombMinDelaySamples =
    static_cast<float>(feathers::dsp::SincTable::kHalfTaps);
static_assert(kCombMinDelaySamples >= 6.0f,
              "the sinc window reaches this far behind the read point; a shorter delay reads unwritten samples");
/// THE LONGEST DELAY, in REFERENCE samples: the rest floor's period plus the LFO's full reach below it.
/// 1785.4 at 48 kHz, where the firmware's was 1534 (a rest floor of E1 at the field's longest base).
inline constexpr float kCombMaxDelaySamples =
    static_cast<float>(kCombReferenceRate / kCombRestFloorHz) + kCombMaxModRangeSamples;
static_assert(static_cast<double>(kCombMaxDelaySamples) * (kCombMaxSampleRate / kCombReferenceRate)
                      + static_cast<double>(feathers::dsp::SincTable::kTaps) + 2.0
                  < static_cast<double>(kCombLineSamples),
              "the longest delay at the highest rate, plus the interpolator's window, must fit inside the line");

/// THE LONGEST DELAY IN THIS RATE'S SAMPLES — the reference scaled, and never past what the line can hold.
[[nodiscard]] inline float combMaxDelayFor(float scale) {
	const float lineLimit = static_cast<float>(kCombLineSamples)
	                        - static_cast<float>(feathers::dsp::SincTable::kTaps) - 2.0f;
	return std::min(kCombMaxDelaySamples * scale, lineLimit);
}

/// HOW HARD THE FEEDBACK MAY BE DRIVEN. Strictly below 1, because a feedback comb at exactly 1 is an
/// oscillator that never decays and above 1 is one that grows without bound. The firmware's feedback
/// comes from a phi triangle with a 0.8 duty, so this is the same ceiling arrived at honestly rather
/// than a new restriction.
inline constexpr float kCombMaxFeedback = 0.95f;

/// WHAT THE EFFECT TELLS THE COMB. Plain numbers in the comb's own units — samples, and 0..1 amounts
/// — rather than zone coordinates, because deriving those from the phi field is the effect's job and
/// a stage that read a coordinate could not be tested without one.
struct CombSettings {
	/// Where the delay sits with no modulation, in samples.
	float baseDelaySamples = 4.0f;
	/// How far ±1 of LFO moves it, in samples.
	float modRangeSamples = 0.0f;
	/// How much of the delayed signal returns to the line. Clamped below 1 — see kCombMaxFeedback.
	float feedback = 0.0f;
	/// How much comb is ADDED to the signal. See the header: this is not a crossfade.
	float mix = 0.0f;
	/// 0 leaves the two channels alone; 1 collapses them to their mean. A width control, applied to
	/// the comb's output only, so the dry signal keeps its stereo image.
	float monoCollapse = 0.0f;

	friend bool operator==(const CombSettings&, const CombSettings&) = default;
};

/// WHERE THE DELAY SITS FOR A GIVEN LFO VALUE — the comb's pitch at an instant.
///
/// A FREE FUNCTION BECAUSE TWO CALLERS NEED IT AND ONLY ONE OF THEM HAS A `Comb`. The DSP asks it
/// through `Comb::delayFor`, which is now one line; automod's STRIP asks it with nothing but a
/// `CombSettings`, to tint the phase star with the pitch the comb is actually at. Restating five
/// lines in the surface would be the shape CLAUDE.md names — two functions that must agree at a
/// point are two functions that stop agreeing — and this one would drift silently, because a tint
/// that is wrong looks like a design choice.
///
/// EVERY TERM GUARDED, because the settings come off a phi field and a surface may ask before the
/// first block has run.
/// `scale` IS `sampleRate / kCombReferenceRate` — the settings are in reference samples, the answer in THIS
/// rate's. A surface asks at 1, which draws the comb's pitch rather than its sample count.
[[nodiscard]] inline float combDelayFor(const CombSettings& s, float lfo, float scale = 1.0f) {
	const float safe = std::isfinite(lfo) ? lfo : 0.0f;
	const float base =
	    std::isfinite(s.baseDelaySamples) ? s.baseDelaySamples : kCombMinDelaySamples;
	const float range = std::isfinite(s.modRangeSamples) ? s.modRangeSamples : 0.0f;
	return std::clamp((base + safe * range) * scale, kCombMinDelaySamples, combMaxDelayFor(scale));
}

class Comb {
public:
	Comb()
	    : lineL_(feathers::fx::filter::kSincTable), lineR_(feathers::fx::filter::kSincTable) {}

	/// Clears the line. The sample rate is not stored because nothing here is a frequency — a delay
	/// is in SAMPLES, and the effect converts if it wants a pitch. Taken as a parameter anyway so the
	/// call site reads like every other device's, and so a rate change still flushes the line.
	void prepare(double sampleRate) {
		scale_ = static_cast<float>((sampleRate > 0.0 ? sampleRate : kCombReferenceRate) / kCombReferenceRate);
		reset();
	}

	/// Empty the line and forget the settings ramp. What activate() and an all-notes-off mean.
	void reset() {
		lineL_.clear();
		lineR_.clear();
	}

	/// Whether either line holds a non-zero sample (`SincDelayLine::holdsSound`, backlog MC).
	[[nodiscard]] bool holdsSound() const noexcept { return lineL_.holdsSound() || lineR_.holdsSound(); }

	void setSettings(const CombSettings& s) { settings_ = s; }
	/// THE BASE DELAY ALONE, per sample — the effect ramps it across a tick (backlog IS).
	void setBaseDelaySamples(float samples) { settings_.baseDelaySamples = samples; }
	[[nodiscard]] const CombSettings& settings() const noexcept { return settings_; }

	/// ONE SAMPLE, IN PLACE. `lfoL` and `lfoR` are the LFO's comb taps, in -1..+1.
	///
	/// PER SAMPLE RATHER THAN PER BLOCK because the delay time moves within a block — the effect
	/// interpolates the LFO across the block and hands a fresh value each sample, which is what makes
	/// a sweep smooth rather than stepped.
	void processSample(float& left, float& right, float lfoL, float lfoR) {
		const float fb = std::clamp(settings_.feedback, -kCombMaxFeedback, kCombMaxFeedback);

		const float delayL = delayFor(lfoL);
		const float delayR = delayFor(lfoR);

		// SINC — see the file header, and note that `kCombMinDelaySamples` is what makes this safe.
		float combL = lineL_.read(delayL);
		float combR = lineR_.read(delayR);

		// MONO COLLAPSE, ON THE COMB'S OUTPUT ONLY. A width control that also narrowed the dry
		// signal would be a width control for the whole device, which is not what the firmware's
		// `combMonoCollapseQ` does and not what a comb's stereo spread is for.
		const float collapse = std::clamp(settings_.monoCollapse, 0.0f, 1.0f);
		if (collapse > 0.0f) {
			const float mono = 0.5f * (combL + combR);
			combL += (mono - combL) * collapse;
			combR += (mono - combR) * collapse;
		}

		// The line takes the input plus what came back, which is what makes it a FEEDBACK comb
		// rather than a single echo.
		lineL_.write(left + combL * fb);
		lineR_.write(right + combR * fb);

		// ADDED, not crossfaded. See the header.
		left += combL * settings_.mix;
		right += combR * settings_.mix;
	}

	/// WHAT DELAY A GIVEN LFO VALUE ASKS FOR, in samples, after clamping. A test-only seam in
	/// CLAUDE.md's sense: the delay is not recoverable from the output, because a comb's output
	/// carries every delay it has ever had. If the claim is "this LFO value means this delay", the
	/// value has to be readable.
	[[nodiscard]] float delayFor(float lfo) const { return combDelayFor(settings_, lfo, scale_); }

private:
	feathers::dsp::SincDelayLine<kCombLineSamples> lineL_;
	feathers::dsp::SincDelayLine<kCombLineSamples> lineR_;
	CombSettings settings_ {};
	/// THIS RATE OVER THE REFERENCE — see `kCombReferenceRate`.
	float scale_ = 1.0f;
};

} // namespace feathers::fx::automod
