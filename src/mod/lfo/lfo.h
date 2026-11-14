/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "lfo_types.h"

#include <algorithm>
#include <cmath>
#include <span>
#include <vector>

namespace feathers::mod::lfo {

/// **A SKEW MAPPING — WHERE THE WAVE'S OWN LANDMARKS SIT IN TIME.** Three controls place the three
/// interior boundaries of the cycle; the waveform is then read through the piecewise-linear remap
/// they define, so each quadrant is stretched or squeezed without its SHAPE changing.
///
/// **THE THREE ARE NESTED, NOT PARALLEL, AND THE NAMES ALREADY SAID SO.**
///
///     preCross  places the MID-CROSSING — how much of the whole cycle is the first half
///     prePeak   divides THE FIRST HALF at the peak
///     preTrough divides THE SECOND HALF at the trough
///
/// Read against a sine, which is where the names come from: the peak is at wave phase 0.25, the
/// descending zero crossing at 0.5, the trough at 0.75. So "how much comes before the cross" IS the
/// first half's share, and the other two divide the halves the cross has made. `w3` is what is left
/// of the second half, exactly as `preTrough` leaves it — there is no slack absorber and no
/// normalisation pass, because the weights sum to 1 by construction.
///
/// **THIS REPLACES A PARTITION THAT COULD NOT REACH A SAWTOOTH**. The old build took each knob as one quadrant's weight and
/// gave the FOURTH whatever was left, then normalised. MEASURED off that arithmetic: pushing the
/// central knob to its top gave weights 0.25 / 0.50 / 0.25 / 0, so a triangle's rise reached 0.75 of
/// the cycle and stopped — because the knob only ever spent the implicit quadrant and nothing it did
/// could touch `w2`. A full sawtooth was reachable only by setting all three knobs at once
/// (100 / 100 / 0), which is a recipe rather than a control.
///
/// Nested, `preCross` alone does it: at 1 the first half is the whole cycle, a triangle's rise fills
/// it and the fall has nowhere to happen. At 0, the same in reverse. **And the side skews keep their
/// meaning at every setting**, because they divide whatever half they are given rather than competing
/// with it for the same budget — which is the "re scaling" half of the request.
///
/// EACH CONTROL IS A FRACTION, 0..1, neutral at 0.5. It was 0..100 with `Unit::Percent`, and a
/// Percent in this project STORES A FRACTION — so the boxes read **5000%**. See the descriptors.
struct SkewMap {
	/// Normalized quadrant weights. They sum to 1 by construction; nothing needs to re-check that.
	std::array<float, 4> weight {};

	/// BUILD FROM THE THREE CONTROLS, each a fraction with 0.5 neutral.
	explicit SkewMap(float prePeak = 0.5f, float preCross = 0.5f, float preTrough = 0.5f) {
		const float firstHalf = std::clamp(preCross, 0.0f, 1.0f);
		const float secondHalf = 1.0f - firstHalf;
		const float atPeak = std::clamp(prePeak, 0.0f, 1.0f);
		const float atTrough = std::clamp(preTrough, 0.0f, 1.0f);
		weight = { firstHalf * atPeak, firstHalf * (1.0f - atPeak),
			       secondHalf * atTrough, secondHalf * (1.0f - atTrough) };
	}

	/// REMAP A RAW PHASE (0..1) THROUGH THE SKEW. Returns the waveform phase (0..1).
	[[nodiscard]] float remap(float phase) const noexcept {
		const float p = std::clamp(phase, 0.0f, 1.0f);
		float walked = 0.0f;
		for (size_t q = 0; q < 4; ++q) {
			const float span = weight[q];
			// **ZERO-WIDTH QUADRANTS ARE SKIPPED RATHER THAN LANDED IN, which is what makes the
			// extremes behave.** The old walk took the LAST quadrant whenever it fell off the end, so
			// with the tail collapsed a phase of exactly 1 returned 0.75 — the start of a quadrant with
			// no width — and the final sample of a sawtooth jumped to the middle of the wave. Skipping
			// them means the end of the cycle is the end of the cycle.
			if (span > 0.0f && p < walked + span) {
				return (static_cast<float>(q) + std::clamp((p - walked) / span, 0.0f, 1.0f)) / 4.0f;
			}
			walked += span;
		}
		// PAST EVERY QUADRANT: the end of the cycle, which for a periodic wave is also its start.
		return 1.0f;
	}
};
/// WAVEFORM EVALUATION. Given a waveform phase (0..1) and a shape, returns the raw
/// output in 0..1 (unipolar) — the caller applies bipolar rescale.
[[nodiscard]] inline float evalWaveform(Waveform wave, float phase) noexcept {
	const float p = phase - std::floor(phase);
	switch (wave) {
		case Waveform::Sine:
			// Sine: 0 at 0, 1 at 0.25, 0 at 0.5, -1 at 0.75, 1 at 1.0 — remapped to 0..1.
			return 0.5f + 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * p);
		case Waveform::Triangle: {
			// Triangle: 0 at 0, 1 at 0.25, 0 at 0.5, 1 at 0.75, 0 at 1.0 — remapped to 0..1.
			// Standard triangle: rises 0→1 in first half, falls 1→0 in second.
			const float t = 1.0f - std::abs(2.0f * p - 1.0f);
			return t;
		}
		case Waveform::Square:
			// Square: 1 for the first half, 0 for the second.
			return (p < 0.5f) ? 1.0f : 0.0f;
		case Waveform::Random:
			// RANDOM IS NOT A FUNCTION OF PHASE — the engine holds the current step value.
			// This path should not be reached; random is handled via the step index.
			return 0.5f;
		default:
			return 0.0f;
	}
}

/// HASH → [0,1). Deterministic per (seed, step). Used by random walk.
[[nodiscard]] inline float hash01(int seed, int step) noexcept {
	uint32_t h = static_cast<uint32_t>(seed) * 2654435761u;
	h ^= static_cast<uint32_t>(step) * 40503u;
	h ^= h >> 16;
	h *= 0x21f0aaad;
	h ^= h >> 15;
	h *= 0x735a2d97;
	h ^= h >> 15;
	return static_cast<float>(h & 0xffff) / 65536.0f;
}

/// **RANDOM WALK STEP VALUE: the seed picks the sequence, the step picks the place in it.** Nothing
/// else, and that is the whole design — the control that used to sit beside it was
/// just a random seed for the track phase: deterministic random.
///
/// THERE WAS A SECOND CONTROL HERE AND IT WAS A MISREADING. `Gamma` shaped the DISTRIBUTION —
/// `pow(h, gamma)`, skewing the walk's values low or high — which is a different idea from a seed and
/// one nobody asked for. It is deleted rather than repaired: two keys into one deterministic sequence
/// is one more than the idea has, and the name was already taken by the identity axis every other
/// surface in this editor means by "gamma".
///
/// DETERMINISTIC AGAINST THE TRACK, which is what makes it worth seeding at all: when the transport is
/// running the step comes from the song position (`advanceRandom`), so the same bar plays the same
/// values on every pass and a seed is a sequence you can choose rather than a surprise you cannot.
[[nodiscard]] inline float randomWalkValue(int seed, int step) noexcept {
	return hash01(seed, step);
}

/// THE BIPOLAR RESCALE, in ONE place because there are TWO ways out of this engine. Every shaped wave
/// leaves through `eval`; the random walk does not — it has no phase to evaluate — and so it took its
/// own exit and missed the rescale entirely, which made Bipolar a dead switch on exactly one waveform.
/// A second copy of `2v - 1` would have been the same bug waiting: the fix is that neither exit owns it.
[[nodiscard]] inline float applyBipolar(const Settings& s, float unipolar) noexcept {
	return s.bipolar ? (2.0f * unipolar - 1.0f) : unipolar;
}

/// **THE RATE IN Hz, FROM THE TWO CONTROLS AND WHATEVER THE HOST HAS SAID ABOUT TEMPO.**
///
/// **ONE RULE, BECAUSE A CARD AND AN ENGINE THAT DISAGREE ABOUT THE PERIOD DRAW AND PLAY DIFFERENT
/// WAVES.** `Shape::tick` owned this arithmetic and the card had no way to ask for it; the card now
/// needs it — what a one-pole does to a periodic wave depends on `tau / period` — so it moved here
/// rather than being restated, which is `modulator.h`'s standing argument applied within one device.
///
/// **A TEMPO OF ZERO MEANS THE HOST HAS NOT SAID, and the fallback is the ENGINE'S OWN, not a guess.**
/// A synced LFO with no transport runs at the scale read as Hz — that is what `tick` has always done
/// — so this is well defined at every setting and there is no "cannot know the period" case for a
/// caller to handle. `DeviceHost::secondsPerBeat` answers zero for exactly this situation.
[[nodiscard]] inline double rateHz(const Settings& s, double secondsPerBeat) noexcept {
	if (s.synced() && secondsPerBeat > 0.0 && std::isfinite(secondsPerBeat)) {
		const size_t divIndex = (s.timeBase > 0) ? s.timeBase - 1 : 0;
		// QUALIFIED: the `using device::kDivisionBeats` that lets `lfo.cpp` say it bare is in the .cpp,
		// and this is a header.
		const double beats = device::kDivisionBeats[std::min(divIndex, device::kDivisionCount - 1)]
		                     * timeSeconds(s.timeScale);
		const double seconds = beats * secondsPerBeat;
		return (seconds > 0.0) ? 1.0 / seconds : 0.0;
	}
	// FREE-RUNNING, or synced with nothing to sync to: the scale read as Hz.
	return timeSeconds(s.timeScale);
}

/// **WHAT A ONE-POLE DOES TO A PERIODIC WAVE, AS A STATIC CURVE — no oscilloscope required.**
///
/// It can be, and exactly rather than approximately, because **a linear filter's
/// response to a periodic input is itself periodic with the same period.** There is a steady state
/// and it is a closed form; nothing has to be watched settling.
///
/// `raw` is one period sampled at `M` even steps; the answer overwrites it. `period` and `tau` are in
/// the same unit and only their RATIO matters, which is what makes this drawable by a card that knows
/// the rate but not the sample rate.
///
/// **THE ARITHMETIC.** With `r = exp(-dt/tau)` per drawn sample and `a = 1 - r`, the recursion
/// `y[n] = r·y[n-1] + a·x[n]` has the periodic solution
///
///     y[n] = a/(1 - r^M) · Σ_{j=0..M-1} r^j · x[n-j]
///
/// which is two O(M) passes rather than the O(M²) that sum suggests: run the recursion once from
/// zero to get `y₀[M-1]`, divide by `1 - r^M` to get the true value at the wrap, then run it once
/// more from there. `r^M` is computed as `exp(-period/tau)` directly rather than as `pow(r, M)` —
/// the same number, without M roundings.
///
/// **AND IT IS HONEST AT ANY RESOLUTION, which is the property that makes drawing it legitimate.**
/// The exact pole's retained fraction over a time T is `exp(-T/tau)` whatever step it is taken in, so
/// sampling the period at M points is the CONTINUOUS filter at M points rather than a coarser
/// filter — see `mod::smoothingCoeff`, where that exactness was the point. The engine differs only
/// in holding its input for a tick, which is a statement about the engine's input, not about this.
///
/// **THE tau ≫ period LIMIT IS A REAL SETTING, NOT AN EDGE CASE**, and it is 0/0 in the formula:
/// `y₀[M-1] → 0` because `a → 0`, and `1 - r^M → 0` with it. The limit is the MEAN of the input — a
/// filter far slower than the wave passes its average — so that is returned directly rather than
/// dividing two vanishing numbers and drawing the noise.
inline void smoothPeriodic(std::span<float> raw, double period, double tau) {
	const size_t m = raw.size();
	if (m == 0 || !(period > 0.0) || !(tau > 0.0) || !std::isfinite(period) || !std::isfinite(tau)) {
		return; // no smoothing to apply, or no period to apply it over: the wave stands as drawn
	}
	const double decayPeriod = std::exp(-period / tau); // r^M, exactly
	if (1.0 - decayPeriod < 1e-9) {
		double sum = 0.0;
		for (const float v : raw) {
			sum += static_cast<double>(v);
		}
		const auto mean = static_cast<float>(sum / static_cast<double>(m));
		for (float& v : raw) {
			v = mean;
		}
		return;
	}
	const double r = std::exp(-(period / static_cast<double>(m)) / tau);
	const double a = 1.0 - r;

	double y = 0.0;
	for (const float v : raw) {
		y = r * y + a * static_cast<double>(v);
	}
	y /= (1.0 - decayPeriod); // where the filter actually stands at the wrap
	for (float& v : raw) {
		y = r * y + a * static_cast<double>(v);
		v = static_cast<float>(y);
	}
}

/// **IS THIS WAVE A WALK?** Both randoms leave the engine by the sequence road rather than through
/// `eval`, and every test of "which road" has to name both or `RandomGlide` silently takes the phase
/// road and evaluates a shape it does not have. One predicate, so a third walk has one place to go.
[[nodiscard]] inline constexpr bool isRandom(Waveform w) noexcept {
	return w == Waveform::Random || w == Waveform::RandomGlide;
}

/// HOW MANY STEPS THE SCOPE SHOWS. A window, like a scope's time base: enough of them that the walk
/// reads as a walk, few enough that one step is a visible division.
inline constexpr int kScopeSteps = 8;

/// **THE RANDOM WALK'S ACTUAL OUTPUT**, over the `kScopeSteps` steps ending at `currentStep` — by
/// design the Random card is an oscilloscope of the actual output. `x` runs 0..1 across the window,
/// so the newest step is the rightmost
/// division and `x = 1` is now.
///
/// IT CAN SHOW THE PAST BECAUSE THE WALK HAS NO SECRETS: every value is `randomWalkValue(seed, step)`,
/// so given the step the engine is on, every step before it is recoverable exactly. Nothing
/// is sampled, buffered or plumbed back from the audio thread, and the drawn values are not "like"
/// the output — they are the output, from the same function that produced it.
///
/// WHAT THIS REPLACES, AND WHY IT WAS WORTH REPLACING: the first version drew eight steps regardless
/// of the rate (so the x axis meant nothing and the Rate knob moved no pixel), started them at step
/// zero rather than at the step being played (so it was never this walk's actual position), and
/// smoothed them with a coefficient of its own invention — `1/(1 + tau*100)`, against the engine's
/// per-tick one-pole — under a comment reading "close enough for display". Three ways for a picture
/// to disagree with the sound it claims to be.
///
/// **THE SMOOTHING IS THE ENGINE'S OWN LAW, AND SINCE 2026-09-19 THAT IS EXACT RATHER THAN NEARLY.**
///
/// It used to be an approximation, stated as one: the engine ran `s += alpha*(target - s)` per
/// 64-sample tick with `alpha = dt/(dt + tau)`, so after time T it reached
/// `target + (start - target)*(tau/(dt + tau))^(T/dt)`, and this drew the dt → 0 limit of that —
/// `exp(-T/tau)` — because the card cannot see the sample rate (its host seam carries fields and a
/// playhead, nothing else). The gap was second order in dt/tau, about 3% of the time constant at
/// what was then the default 20 ms, and it was listed as open in the modulators' handoff.
///
/// **IT CLOSED WITHOUT BEING AIMED AT.** `mod::smoothingCoeff` put all three modulators on the exact
/// pole `1 - exp(-dt/tau)`, whose per-tick RETAINED fraction is `exp(-dt/tau)` — so T seconds of it
/// is `exp(-T/tau)` on the nose, for any dt, which is the expression already written here. The card
/// still cannot see the sample rate and no longer needs to: the law it draws has stopped depending
/// on one. A picture that was 3% out is now the same arithmetic as the sound.
[[nodiscard]] inline std::vector<std::pair<float, float>> randomTrace(const Settings& s,
                                                                     int currentStep,
                                                                     float intoStep = 0.0f,
                                                                     size_t samples = 128) {
	std::vector<std::pair<float, float>> c;
	if (samples < 2) {
		return c;
	}
	c.reserve(samples);

	// **THE WINDOW ENDS AT NOW, AND "NOW" IS FRACTIONAL — which is what stops this scrolling in
	// lurches.**
	//
	// It used to draw `kScopeSteps` WHOLE steps ending at the end of `currentStep`, so the window was
	// quantised to step boundaries: it showed the step in progress ALREADY COMPLETE — including the
	// part that had not been played — and then jumped a whole step sideways when the ordinal ticked.
	// At a slow rate that is one lurch per step and nothing in between, which is exactly what a
	// scrolling scope must not do.
	//
	// The fraction was already being published: `Playhead::phase` is the step's own fraction for a
	// walk, packed with the ordinal into ONE atomic so a reader cannot get halves of two publishes,
	// and stored once a block. The scope simply was not reading it — it went only to the star.
	//
	// So the window is now the half-open interval `(now - kScopeSteps, now]` in step units, and it
	// slides continuously. Nothing beyond `now` is drawn, which also retires a smaller dishonesty: the
	// old right-hand edge showed the remainder of a step the engine had not reached.
	const double now = static_cast<double>(currentStep)
	                   + (std::isfinite(intoStep) ? std::clamp(intoStep, 0.0f, 1.0f) : 0.0f);
	const double windowStart = now - static_cast<double>(kScopeSteps);
	const int firstStep = static_cast<int>(std::floor(windowStart));
	// The step's length in seconds, which is what the smoothing is measured against. Free-running
	// reads the scale as Hz; a synced walk steps on the division, and the card has no tempo, so the
	// window is drawn in steps either way and only the smoothing wants the seconds.
	const double hz = timeSeconds(s.timeScale);
	const double stepSeconds = (hz > 0.0) ? (1.0 / hz) : 0.0;
	// OFF IS THE BOTTOM OF THE TRAVEL, the same test `mod::smoothingCoeff` makes — not a `smoothingOn`
	// flag (deleted) and not a `-10.0f` literal an octave above the declared minimum (a bug: the
	// engine ignored that octave while this picture drew it).
	const bool smoothing = std::isfinite(s.smoothing) && s.smoothing > kSmoothingLog2SecondsMin;
	const double tau = smoothing ? std::exp2(std::min(static_cast<double>(s.smoothing),
	                                                  static_cast<double>(kSmoothingLog2SecondsMax)))
	                             : 0.0;

	// **THE POLE ON WHATEVER THE WAVE FEEDS IT, IN CLOSED FORM, AND THERE ARE TWO INPUTS NOW.**
	//
	// `Random` holds its step value, so the input is piecewise CONSTANT and the pole's answer is the
	// familiar `target + (start - target)·exp(-t/tau)`. `RandomGlide` ramps between step values, so
	// the input is piecewise LINEAR — and a pole driven by `a + b·t` from `y0` is
	// `a + b·t - b·tau + (y0 - a + b·tau)·exp(-t/tau)`, which is as exact as the other one and no
	// longer to write. **Marching a filter numerically across the drawn samples would have been an
	// approximation whose error depends on how many pixels wide the card happens to be**, which is not
	// a quantity anything else here is allowed to depend on.
	//
	// With no smoothing both collapse to the input itself: the held value, or the ramp.
	const bool glide = (s.wave == Waveform::RandomGlide);
	const auto valueAt = [&](int step) { return randomWalkValue(s.seed, step); };
	/// The drawn value `into` of the way through `step`, entering the step holding `y0`. NOT named
	/// `within`: the sample loop below already has an int by that name, and the shadow compiles to
	/// "cannot be used as a function" rather than to anything a reader would recognise.
	const auto poleAt = [&](int step, float y0, double into) -> float {
		const float a = valueAt(step);
		const double b = glide ? (static_cast<double>(valueAt(step + 1)) - a) : 0.0; // per STEP, not per second
		if (!smoothing || !(stepSeconds > 0.0)) {
			return static_cast<float>(a + b * into);
		}
		// tau in STEPS, so `into` and it are in one unit and the ramp's slope needs no rescaling.
		const double tauSteps = tau / stepSeconds;
		const double decay = std::exp(-into / tauSteps);
		return static_cast<float>(a + b * into - b * tauSteps
		                          + (static_cast<double>(y0) - a + b * tauSteps) * decay);
	};

	// WHERE THE FILTER STANDS WHEN THE WINDOW OPENS. Run it over a few steps BEFORE the window so the
	// left edge is where the filter actually was, rather than a fresh start that would draw a settling
	// transient the engine is not producing. Eight steps of lead is far past any audible tau.
	float held = valueAt(firstStep - kScopeSteps);
	for (int step = firstStep - kScopeSteps; step < firstStep; ++step) {
		held = poleAt(step, held, 1.0);
	}

	for (size_t i = 0; i < samples; ++i) {
		const float x = static_cast<float>(i) / static_cast<float>(samples - 1);
		// Where in the walk this sample sits, as a fractional step position, and which step that is.
		// `floor` rather than a truncating cast: the window's start is negative for the first few steps
		// of a song and a cast toward zero would fold two different steps onto one.
		const double at = windowStart + static_cast<double>(x) * kScopeSteps;
		const int step = static_cast<int>(std::floor(at));
		const double into = at - std::floor(at);
		// The value the filter was holding when THIS step began: every earlier step in the window,
		// walked forward. Recomputed per sample rather than carried, because the samples are not in
		// step order once `samples` and `kScopeSteps` disagree.
		float start = held;
		for (int earlier = firstStep; earlier < step; ++earlier) {
			start = poleAt(earlier, start, 1.0);
		}
		c.emplace_back(x, applyBipolar(s, poleAt(step, start, into)));
	}
	return c;
}

/// FULL EVALUATION: skew remap → waveform → bipolar. Returns the final output.
[[nodiscard]] inline float eval(const Settings& s, float phase) noexcept {
	SkewMap skew(s.skewPrePeak, s.skewPreCross, s.skewPreTrough);
	const float wavePhase = skew.remap(phase);
	return applyBipolar(s, evalWaveform(s.wave, wavePhase));
}

//------------------------------------------------------------------------------------------------//
// Shape — the DSP                                                                                //
//------------------------------------------------------------------------------------------------//

/// THE LFO DSP. Advances a phase, evaluates the skewed waveform, applies smoothing.
class Shape {
public:
	void prepare(double sampleRate);
	void reset();

	/// **SMOOTH THROUGH RETRIGGER** (backlog MM): this fresh shape's SMOOTHER starts where `other`'s output stands, so
	/// a new note glides from there instead of from zero. The smoother only — the wave itself starts as it would.
	/// `mod::PerNoteTable::processNotes` is the one caller.
	void carrySmoothingFrom(const Shape& other) noexcept {
		smoothed_ = other.smoothed_;
		value_ = other.value_;
		smoothedR_ = other.smoothedR_;
		valueR_ = other.valueR_;
	}

	/// **A TRIG FIRED (backlog MV): the next tick sends the wave back to its start phase** — latched like
	/// `retrigger`. It restarts whenever the mode lets the wave run: always under Once, Free and LFO Retrig, and under
	/// Gate and Loop only while the gate is up (they rest at the key). On a Free wave the song is holding, the restart
	/// shifts the lock rather than being undone by it (`TransportLock::restartAt`).
	void fire() noexcept { trigPending_ = true; }

	/// Advance one block. Returns the output value.
	float tick(const Settings& settings, bool gate, bool retrigger, TransportClock clock,
	           float rateMul = 1.0f);

	/// WHERE THE WAVEFORM IS, for a playhead/card to draw a star.
	[[nodiscard]] Playhead playhead() const noexcept;

	/// THE LAST OUTPUT.
	[[nodiscard]] float value() const noexcept { return value_; }
	/// **THE RIGHT EAR'S LAST OUTPUT** (backlog PA): the same wave read `Settings::stereoLead` further on, through its own
	/// smoother. `value()` exactly while Stereo Phase is 0.
	[[nodiscard]] float valueRight() const noexcept { return valueR_; }

	/// **NOTHING LEFT TO PLAY FOR A NOTE** — what `mod::PerNoteTable` marks a released note done by. A one-shot
	/// has finished and its smoothed output has arrived (the MSEG's rule: done is decided by the output). A
	/// CYCLING wave has no end to arrive at, so it is idle the moment it is asked — once its key is up it is done,
	/// and goes on cycling for whatever voice is still sounding it; done only decides which slot is reclaimed first.
	/// **EXCEPT GATE AND LOOP, which stop at the key** (2026-10-07): idle once at rest and the smoothing has arrived.
	[[nodiscard]] bool idle() const noexcept {
		return (cycles_ && !stopsAtKey_) || (position_.finished && smoothed_ == static_cast<double>(raw_));
	}

	/// HOW MANY TIMES THE SHAPE HAS BEEN SENT BACK TO ITS START since `reset`. A test seam: a restart
	/// is not recoverable from the output (a looping wave passes its start every cycle anyway).
	[[nodiscard]] uint32_t restartsForTest() const noexcept { return restarts_; }
	/// TEST-ONLY: how many times the smoothing coefficient was designed (once per knob value, not per tick).
	[[nodiscard]] size_t smoothingDesignsForTest() const noexcept { return smoothingDesigns_; }

private:
	void advance(const Settings& s, float hz, float rateMul);
	void advanceRandom(const Settings& s, float hz, float rateMul);
	void restart(const Settings& s);
	/// The right ear's raw value, the wave read `lead` cycles past where the left ear's raw value was read.
	[[nodiscard]] float rawRightAt(const Settings& s, float lead) const noexcept;

	struct Position {
		float phase = 0.0f;
		bool finished = true;
	};

	Position position_ {};
	double smoothed_ = 0.0;
	/// THE ONE-POLE'S STEP, from `mod::smoothingCoeff`. 1 means pass-through, which is what the bottom
	/// of the Smoothing travel now carries — see `tick`.
	double smoothingCoeff_ = 1.0;
	float smoothingKnobWas_ = 0.0f; ///< the knob `smoothingCoeff_` was made for
	bool smoothingCoeffValid_ = false;
	size_t smoothingDesigns_ = 0;
	float raw_ = 0.0f;
	float value_ = 0.0f;
	/// THE RIGHT EAR (backlog PA) — its own smoother, the left's coefficient. Held equal to the left's while Stereo Phase
	/// is 0, so turning it up starts the right ear from where the left stands rather than from rest.
	double smoothedR_ = 0.0;
	float valueR_ = 0.0f;
	bool gateWas_ = false;
	/// Whether the last tick ran a mode that cycles (anything but Once) — `idle` reads it.
	bool cycles_ = false;
	/// Whether the last tick ran Gate or Loop, which stop at the key — `idle` reads it.
	bool stopsAtKey_ = false;
	uint32_t restarts_ = 0;
	/// A Trig fired since the last tick (`fire`), and whether THIS tick restarted for one — the locks read it.
	bool trigPending_ = false;
	bool trigRestart_ = false;
	/// **GATE AND LOOP STOP AT THE KEY**. `resting_`: stopped, the
	/// output at NEUTRAL (0, the bipolar centre) through the smoothing, the phase still. `finishing_`: a Loop whose
	/// key is up, running out its current cycle; `cycleTravel_` is how far into that cycle, counted from the start
	/// phase, so the cycle ends where it began. Cleared by every restart and by `reset` — a wave never struck stands
	/// still where it starts, as it always did; only a key going up sends it to neutral.
	bool resting_ = false;
	bool finishing_ = false;
	float cycleTravel_ = 0.0f;
	/// Stop: the output to neutral and the phase still. See `resting_`.
	void rest() noexcept;
	/// FREE + SYNCED ON A PLAYING TRANSPORT follows the song through this — see `mod::TransportLock`.
	TransportLock lock_ {};
	/// The synced random walk's own, for the same reason — see `advanceRandom`.
	TransportLock randomLock_ {};
	TransportClock clock_ {};
	double sampleRate_ = 48000.0;
	double tickSeconds_ = static_cast<double>(kBlockSamples) / 48000.0;

	// RANDOM WALK STATE. The current stepped value and which step we're on.
	// NOT SAVED WITH THE PATCH, AND WHY (backlog NM): it is DERIVED. The value is `randomWalkValue(Seed, step)`, and on a
	// playing transport the step is the song's (synced: beats; Hz: seconds, `followsSongSeconds`), so a reload lands on
	// it by construction. Stopped, the walk free-runs from step 0, as every free clock does with no song to follow.
	int randomStep_ = 0;
	float randomValue_ = 0.0f;
	/// THE WALK'S OWN PHASE, because `position_.phase` is wrapped before the walk gets to read it and
	/// the wrap is the event the walk is waiting for. Free-running only; the synced branch takes its
	/// step straight from the song position.
	float randomPhase_ = 0.0f;
	/// HOW FAR INTO THE CURRENT STEP, 0..1 — what `RandomGlide` interpolates on. Both roads through
	/// `advanceRandom` already computed this; it used to be discarded.
	float randomFrac_ = 0.0f;
	/// Has the walk drawn its first value? Without this the output holds at zero until the FIRST step
	/// lands — a whole second of silence at 1 Hz — because "the step did not change" is true on tick
	/// one for the same reason it is true on tick two.
	bool randomPrimed_ = false;
};

} // namespace feathers::mod::lfo
