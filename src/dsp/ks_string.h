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

#include <array>
#include <cstdint>
#include <vector>

// ONE KARPLUS-STRONG STRING: a delay line whose length is the pitch, excited once and fed back through a loop
// filter. Host rate, one per OscVoice, float in the loop and double
// in the coefficient arithmetic.
//
// THE LOOP, per sample:
//
//     x  = line[w − n]                       the sample written n samples ago
//     a  = Thiran(x)                         first-order allpass, phase delay d at f0 exactly (§1.1)
//     f  = c·(a + a₋₂) + (1 − 2c)·a₋₁         the loop FIR, linear phase, delay exactly 1, |H| ≤ 1 for c ≤ 0.25
//     s  = Disp⁴(f)                          four identical first-order allpasses (coefficient ≤ 0): the stretch
//     v  = g·s + e                           g from the T60 (§1.3), e the excitation being injected
//     line[w] = v
//
// so the loop delay at ω is n + τ_Thiran(ω) + 1 + 4·τ_disp(ω), and at ω0 = 2π f0 / sr it is sr / f0 to the
// rounding of the coefficient solve — the fundamental is exact (§1.2). The dispersion is ALWAYS in the loop, a pure
// four-sample delay at Stretch 0: switching it out would step the loop's length by four samples every time a
// stretch island began, and continuity under a moving puck is worth more than the four samples.
//
// THE INTEGER STEP. While a glide moves the length, the Thiran's fraction d absorbs it until it leaves
// [kFractionLow, kFractionHigh]; then a second read path at the new n is primed (its allpass run over the line's
// last kPrimeSamples, so its state is the steady state, not zero) and the two are crossfaded over kHandoffSamples.
// The hysteresis band keeps vibrato around a boundary from handing off on every swing.
//
// EXACT SILENCE (§1.7): a running mean square of v (a one-pole over about one period) is kept; once it is below
// kSilenceFloor² with nothing left to inject, the line and every state are zeroed and the string is IDLE — it
// renders exact zeros and costs nothing until the next pluck. A non-finite sample also zeroes the string.
//
// THREADING: prepare() allocates and runs on the main thread (activate); everything else is the audio thread's and
// never allocates.

namespace feathers::ks {

/// C0 (MIDI 12, chosen over the draft's MIDI 0: half the memory, about 5 MB at 48 kHz
/// across 160 strings). The line is sized for it at prepare(); a lower pitch is clamped HERE (and `clamped()` says so), and
/// the voice plays a note below it as silence, as it does a note above the top (OscVoice::controlString).
inline constexpr double kLowestHz = 16.351597831287414;
/// THE HIGHEST PITCH IS sr / this: the loop needs n ≥ 1, a Thiran fraction ≥ 0.5, the FIR's one sample and the
/// dispersion's four at Stretch 0, so a period under 6.5 samples cannot be built. 7 leaves a margin. A voice asked for a
/// note above it plays nothing (OscVoice: honest silence, as Additive culls what it cannot render), never a wrong pitch.
inline constexpr double kShortestPeriod = 7.0;
inline constexpr int kDispersionStages = 4;
/// −160 dB, as the Additive bank's floor: below it the string is silent and is made exactly so.
inline constexpr float kSilenceFloor = 1.0e-8f;
inline constexpr int kHandoffSamples = 32;
inline constexpr int kPrimeSamples = 24;
/// The Thiran's fraction is kept inside this band; a fraction that leaves it starts a handoff to the integer delay
/// that puts it back near the middle, [0.5, 1.5).
inline constexpr double kFractionLow = 0.25;
inline constexpr double kFractionHigh = 2.0;
/// The most a loop can gain per round trip at any frequency: a T60 of "ring" is this at the fundamental.
inline constexpr double kMaxLoopGain = 1.0 - 1.0e-7;
/// The dispersion may take at most this share of the period (§1.4: the delay budget), so a high note is fed less
/// stretch than it asked for, and `tuning().disp` reports what it got.
inline constexpr double kDispersionBudget = 0.5;
/// THE EXCITATION'S RMS: V5's default patch measures 0.261 (Additive design §5.5), and the pickup's output keeps the
/// loop's RMS on average (its 1/√2 against the comb's mean power of 2), so a fresh pluck at velocity 1 starts at
/// V5's level.
inline constexpr float kExcitationRms = 0.26f;

/// Phase delay, in samples, of the first-order allpass (a + z⁻¹) / (1 + a·z⁻¹) at ω (radians per sample).
double allpassPhaseDelay(double a, double omega);
/// THE THIRAN COEFFICIENT whose phase delay at ω is exactly d: η = sin((1−d)ω/2) / sin((1+d)ω/2), which tends to the
/// textbook (1−d)/(1+d) as ω → 0. The textbook form is what goes sharp at high notes.
double thiranCoefficient(double d, double omega);
/// |H| of the loop FIR (c, 1 − 2c, c) at ω.
double loopFirMagnitude(double c, double omega);
/// The loop gain g that gives the fundamental the T60 `t60Seconds` (≤ 0 or infinite: ring, kMaxLoopGain), capped so
/// the loop never gains at any frequency.
double loopGainFor(double sampleRate, double f0, double t60Seconds, double c);
/// The most negative dispersion coefficient a period of `period` samples can afford (0 when it can afford none).
double dispersionFloor(double period);

/// One pitch's loop coefficients.
struct Tuning {
	int n = 1;          ///< the integer read delay, ≥ 1
	double d = 1.0;     ///< the Thiran's phase delay at f0
	double eta = 0.0;   ///< the Thiran's coefficient
	double disp = 0.0;  ///< each dispersion stage's coefficient, ≤ 0
};

/// THE LOOP'S PHASE DELAY AT ω for a tuning: n + τ_Thiran(ω) + 1 + 4·τ_disp(ω). At ω0 this is the period.
double loopPhaseDelay(const Tuning& t, double omega);
/// The frequency, in Hz, of partial k (1-based) of a loop with this tuning: the root of ω·loopPhaseDelay(ω) = 2πk,
/// found by bisection. Harmonic (k·f0) at Stretch 0 up to the Thiran's own small dispersion.
double partialHz(const Tuning& t, double sampleRate, int k);
/// The tuning that puts the fundamental exactly on `f0` with the dispersion as asked (clamped by the budget), choosing
/// n so the fraction sits in [0.5, 1.5).
Tuning solveTuning(double sampleRate, double f0, double dispRequested);

/// WHAT A PLUCK IS MADE OF (§1.5): one period of (1 − noise)·tone + noise·W through the circular pick comb, at
/// kExcitationRms × level.
struct Excitation {
	const float* tone = nullptr; ///< one cycle of the exciter's tone, `tonePoints` long (null: no tone)
	int tonePoints = 0;
	float noise = 1.0f;          ///< 0 the tone alone, 1 the noise alone
	float hardness = 0.6f;       ///< the noise's one-pole: 0 soft, 1 white
	double pick = 0.13;          ///< the pick position, a fraction of the string
	uint64_t seed = 1;           ///< the noise's seed (§6: derived by the caller, never incidental)
	float level = 1.0f;          ///< velocity's gain
};

/// THE NOISE: xorshift64*, seeded through splitmix64, uniform on [−1, 1). Pinned by a test as a value.
struct PluckNoise {
	explicit PluckNoise(uint64_t seed);
	float next();
	uint64_t state;
};

/// Fill `out` with one period of the excitation for a loop of `period` samples, at most `capacity` samples; answers
/// how many it wrote (round(period), at least 2). `scratch` is `capacity` floats the caller owns (no allocation).
int buildExcitation(const Excitation& e, double period, float* out, float* scratch, int capacity);

/// THE PER-PLUCK VARIATION (by design): each pluck's pick position within ±5% of the material's and
/// its level within ±0.25 dB, drawn from the pluck's own seed. Subtle by design: a phrase breathes, a note does not wander.
inline constexpr double kPluckPickVariation = 0.05;
inline constexpr double kPluckLevelVariationDb = 0.25;

/// Decay and Release at or above these are "ring": the loop is not shortened by them (design §4.1).
inline constexpr double kDecayRing = 30.0;
inline constexpr double kReleaseRing = 10.0;
/// Decay is the T60 of a C3's fundamental; Decay Track bends it by (f0 / C3)^−κ.
inline constexpr double kDecayReferenceHz = 130.8127826502993;

/// THE ENGINE-WIDE STRING CONTROLS (design §4.1), one set for every voice.
struct StringControls {
	double decaySeconds = 3.0;
	double decayTrack = 0.5;
	double releaseSeconds = 0.2;
	float hardness = 0.6f;
	double pickup = 0.12;
	double width = 0.1;
	/// How far a note's velocity moves its Hardness: hardness + velocity · (v − 0.5), clamped to [0, 1] (velocity 0..1).
	double velocity = 0.5;
	friend bool operator==(const StringControls&, const StringControls&) = default;
};

/// ONE STRING'S LOOP AT ONE PITCH, from its material (each in [0, 1], the bank's StringFields) and the controls.
struct StringLaws {
	double damping = 0.0;   ///< the loop FIR's c, 0.25·(1 − brightness)²
	double t60 = 0.0;       ///< seconds; 0 is ring
	double dispersion = 0.0;///< −0.9·stretch
	double pick = 0.13;     ///< 0.04 + 0.36·pick
	float noise = 0.0f;
	friend bool operator==(const StringLaws&, const StringLaws&) = default;
};

/// The mapping (design §2.1), in one place: the voice and the pictures read it, so the sound and the map agree.
/// T60 = Decay · (f0 / C3)^−Track · 4^(2·sustain − 1) held; released, the shorter of that and Release.
StringLaws lawsFor(float brightness, float sustain, float pick, float noise, float stretch, const StringControls& c,
                   double f0, bool released);

class KsString;

/// THE PLUCK'S TONE CYCLE (design §1.5, §2.2): harmonics k = 1..count at amplitudes A_k and FIXED Newman phases
/// π·k²/kToneReferencePartials — fixed so a key's attack does not change phase as its band limit moves — summed by an
/// inverse FFT into kToneCyclePoints points. Band-limited per note: only the harmonics below 0.475 of the loop's
/// sample rate are summed, so a pluck read from it at the loop's length never aliases.
inline constexpr int kToneCyclePoints = 1024;
inline constexpr int kToneReferencePartials = 320;

/// THE PLUCK BUILDER: its working buffers, sized at prepare() (allocates: activate), shared by every voice of a synth
/// (they render one after another on the audio thread), so the per-voice memory is the string alone.
class PluckBuilder {
public:
	void prepare(double sampleRate);
	/// The tone cycle for a loop of `period` samples from `count` harmonic amplitudes (amplitude[0] is harmonic 1).
	/// Null when no harmonic is below the band limit or every one is silent.
	const float* toneCycle(const float* amplitude, int count, double period);
	/// How many harmonics the last toneCycle summed.
	int toneHarmonics() const { return toneHarmonics_; }
	/// Build `e` for `s`'s current period and pluck it.
	void pluck(KsString& s, const Excitation& e);

private:
	std::vector<float> out_;
	std::vector<float> scratch_;
	std::vector<float> re_;
	std::vector<float> im_;
	std::vector<float> cosT_;
	std::vector<float> sinT_;
	std::vector<float> cycle_;
	int toneHarmonics_ = 0;
};

class KsString {
public:
	/// Size the line for kLowestHz at `sampleRate` and clear. ALLOCATES: main thread, at activate.
	void prepare(double sampleRate);
	double sampleRate() const { return sr_; }
	/// The line's length in samples (a power of two).
	int capacity() const { return static_cast<int>(line_.size()); }

	/// Silence, every state zeroed, nothing pending: the string is idle.
	void reset();

	/// Play `hz` (clamped to [kLowestHz, sr/4]) from the next sample. Cheap enough to call per sample in a glide.
	void setFrequency(double hz);
	/// Each dispersion stage's coefficient, asked for (−0.95 .. 0); the budget may grant less. Applied at once.
	void setDispersion(double a);
	/// The loop FIR's c, 0 (bright) .. 0.25 (dark).
	void setDamping(double c);
	/// The fundamental's T60 in seconds (≤ 0 or infinite: ring). With `rampSamples` the loop gain moves to its new
	/// value in a straight line over that many samples (a release is a 5 ms ramp, so it never clicks).
	void setDecay(double t60Seconds, int rampSamples = 0);
	/// The pickups, each a fraction of the period behind the write head (§1.6). Equal: one pickup, both ears.
	void setPickups(double left, double right);

	/// ADD an excitation to be injected at the loop's input over its length, from the next sample: a string already
	/// ringing is re-plucked, not replaced. A pluck longer than the line is truncated to it.
	void pluck(const float* e, int length);

	/// Render `n` samples into `outL` (and `outR`, which may be null for one ear).
	void render(float* outL, float* outR, int n);

	bool idle() const { return idle_; }
	/// TEST SEAMS.
	const Tuning& tuning() const { return a_.tuning; }
	double loopGain() const { return g_; }
	double loopGainTarget() const { return gTarget_; }
	double frequencyHz() const { return f0_; }
	bool clamped() const { return clamped_; }
	bool handingOff() const { return handoffLeft_ > 0; }
	double damping() const { return c_; }
	/// How many samples of excitation are still to be injected.
	int pendingExcitation() const { return excLength_ - excRead_; }

private:
	struct ReadPath {
		Tuning tuning {};
		float eta = 0.0f;
		float x1 = 0.0f;
		float y1 = 0.0f;
	};

	void retune();
	void clearStates();
	float readLine(double delay) const;

	double sr_ = 48000.0;
	std::vector<float> line_;
	std::vector<float> exc_;
	uint32_t mask_ = 0;
	uint32_t w_ = 0;

	double f0_ = 440.0;
	double period_ = 48000.0 / 440.0;
	double dispAsked_ = 0.0;
	double c_ = 0.125;
	double t60_ = 2.0;
	bool clamped_ = false;

	ReadPath a_ {};
	ReadPath b_ {};
	int handoffLeft_ = 0;

	float h1_ = 0.0f;
	float h2_ = 0.0f;
	std::array<float, kDispersionStages> dx1_ {};
	std::array<float, kDispersionStages> dy1_ {};
	float dispF_ = 0.0f;

	double g_ = 0.0;
	double gTarget_ = 0.0;
	double gStep_ = 0.0;
	int gRampLeft_ = 0;

	double pickL_ = 0.12;
	double pickR_ = 0.12;

	int excRead_ = 0;
	int excLength_ = 0;

	double energy_ = 0.0;
	bool idle_ = true;
};

} // namespace feathers::ks
