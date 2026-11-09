/*
 * Copyright © 2024-2026 Owlet Records
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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace feathers::fx::featherverb {

/// THE RATE EVERY NUMBER BELOW WAS TUNED AT — the reference's.
inline constexpr double kReferenceRate = 44100.0;
/// THE REFERENCE'S BUFFER. The LFO, the Owl servo and the envelopes step once per buffer, so a buffer is a
/// unit of TIME to this reverb. The device runs the core on a fixed grid of these, scaled to the rate
/// (`Core::bufferFrames`), never on the host's blocks.
inline constexpr int kReferenceBufferFrames = 128;
/// CHARACTER'S TRIANGLES: six a mode maps onto its own internals, one for width, two low-duty pockets.
inline constexpr size_t kCharacterTriangles = 11;

/// **THE REFERENCE'S FEATHERVERB, TRANSCRIBED.** The firmware's `dsp/reverb/featherverb.{hpp,cpp}` at
/// our firmware fork `owlet-firmware-1.3` @ `2567b04c`. It is the reference's own text, with
/// its members, its setters, its order of operations, its double writes and its quirks. Kept on
/// purpose, because the plan is "port first, then improve".
/// Every departure is here and marked `PORT:` in the .cpp:
///
/// 1. **THE SAMPLE RATE.** Every length in samples scales by `ratio = rate / 44.1 kHz` (the multi-tap
///    offsets keeping the reference's PARITY, or Vast runs away — see `multiTap_`): the FDN lines,
///    the cascade stages, the predelay, the multi-tap offsets, the Owl tap wobble, the pitch-wobble
///    depths and the silence count. Every one-pole coefficient `c` becomes `1 - (1-c)^(1/ratio)`. The
///    per-buffer steps (LFO walk, smoothing, decay) are divided by `ratio`, because the buffer they
///    step on is `ratio` times longer. **AT 44.1 kHz EVERY ONE OF THESE IS THE IDENTITY, EXACTLY.** A
///    length times 1.0 is itself; `rc(c)` returns `c` untouched, not `1 - (1 - c)`; `x / 1.0f` is `x`.
///    That is what makes the parity cases in `test_fx_featherverb.cpp` bit-exact.
/// 2. **THE OUTPUT CONVERSION SATURATES**, as the hardware's ARM `vcvt` does. In C++ it is undefined
///    behaviour, and on x86 it wraps to INT32_MIN.
/// 3. **THE SILENCE COUNTER IS THIS INSTANCE'S.** In the firmware it is a function-local `static`,
///    shared by every instance.
/// 4. **THE BUFFER IS THE CALLER'S** (`bufferFloatsFor`), allocated at prepare. Nothing allocates here.
class Core {
public:
	// === The reference's constants, at 44.1 kHz. `Scaled` below holds them at the running rate. ===
	static constexpr size_t kNumFdnDelays = 3;
	static constexpr size_t kD0MinLength = 420;
	static constexpr size_t kD0MaxLength = 660;
	static constexpr size_t kD1MinLength = 696;
	static constexpr size_t kD1MaxLength = 1800;
	static constexpr size_t kD2MinLength = 780;
	static constexpr size_t kD2MaxLength = 2460;
	static constexpr size_t kNumCascade = 4;
	static constexpr size_t kC0BaseLength = 773;
	static constexpr size_t kC1BaseLength = 997;
	static constexpr size_t kC2BaseLength = 1231;
	static constexpr size_t kC3BaseLength = 4001;
	static constexpr float kCascadeMaxScale = 1.8f;
	static constexpr bool kEnableMultiTapWrites = false;
	static constexpr std::array<size_t, kNumCascade> kMultiTapOffsets = {311, 401, 509, 1607};
	static constexpr float kMultiTapGain = 0.22f;
	static constexpr size_t kPredelayMaxLength = 2205;
	static constexpr float kH3Norm = 0.577350269f;

	/// THE ONE BUFFER, in floats, at `sampleRate`. The same layout as the reference: three FDN lines,
	/// four cascade stages at their largest, then the predelay.
	[[nodiscard]] static size_t bufferFloatsFor(double sampleRate) noexcept;

	/// The reference's constructor, at `sampleRate`, over `buffer` (`bufferFloatsFor` floats, owned by
	/// the caller). Then `allocate()`, which clears the buffer — the reference's `setModel` sequence.
	Core(double sampleRate, float* buffer) noexcept;
	Core() noexcept : Core(kReferenceRate, nullptr) {}

	/// The reference's `allocate()`, minus the allocation: clear the buffer, reset the running state.
	/// **LIKE THE REFERENCE, IT RESETS THE MODE FLAGS BUT NOT `owlMode_`** (plan, trap 2).
	void allocate() noexcept;

	/// THE SAMPLE RATE'S ONE READING. At 44.1 kHz it is 1 and every scaling below is the identity.
	[[nodiscard]] double ratio() const noexcept { return ratio_; }
	/// THE REFERENCE BUFFER AT THIS RATE: 128 frames at 44.1 kHz, the same DURATION at any other.
	[[nodiscard]] int bufferFrames() const noexcept { return bufferFrames_; }

	/// ONE FIRMWARE BUFFER. `input` is the mono send, in q31; the result is ADDED into `outL`/`outR` as
	/// the reference adds into the rendering buffer. **Call it with `bufferFrames()` frames**: the
	/// per-buffer dynamics step once per call.
	void process(const int32_t* input, int32_t* outL, int32_t* outR, size_t frames) noexcept;

	/// PORT: `process` IN THREE PIECES, for a firmware buffer that spans host calls: `beginBuffer(n)` once,
	/// then `run` over consecutive pieces adding up to `n`, then `endBuffer(n)`. The samples come out
	/// identical to one `process(n)`, provided no setter is called in between (the device applies
	/// settings only at buffer boundaries). `process` is these three in a row.
	void beginBuffer(size_t frames) noexcept;
	/// `inject`, when not null, is added PAST THE HIGHPASS AND PREDELAY, where every mode's feedback network and
	/// cascade take their input (the undersampler's accumulator) — Featherverb's Exciter (backlog JD). Same scale
	/// as `input`. Null is the firmware, to the bit.
	void run(const int32_t* input, int32_t* outL, int32_t* outR, size_t frames, const int32_t* inject = nullptr) noexcept;
	void endBuffer(size_t frames) noexcept;

	// === The reference's setters, in its own spelling. Order matters; see the device. ===
	void setRoomSize(float value) noexcept;
	void setDamping(float value) noexcept;
	void setWidth(float value) noexcept { width_ = value; }
	void setHPF(float f) noexcept { hpCutoff_ = f; }
	void setLPF(float f) noexcept { lpCutoff_ = f; }
	/// **FEATHERVERB'S OWN FILTERS, IN HERTZ** (backlog LB: the LPF and HPF cover the full audio
	/// range). Still one-poles (6 dB/oct) in the same places, each set by a corner in Hz at THIS
	/// core's rate, in place of `setHPF`/`setLPF`'s 0..1 knob on the firmware's coefficient. Owl's narrower highpass range
	/// goes with the knob: a corner in Hz is one corner in every mode. Zero or less returns to the firmware's one-pole on
	/// its knob, which the parity cases drive.
	/// - **THE HIGHPASS IS A TRAPEZOIDAL ONE-POLE** (`tptStep`, `tptGain`), 3 dB down exactly at its corner at any rate.
	///   The firmware's structure (`in - s`, then `s += a (in - s)`) given a coefficient from Hz drifts off its corner as
	///   the corner rises — MEASURED 8% low at 632 Hz in 44.1 kHz — so "up to 20 kHz" could not have been true of it.
	/// - **THE LOWPASS KEEPS THE FIRMWARE'S ONE-POLE**, its coefficient the pole at the corner (`onePoleAt`): -3 dB within
	///   2% of the corner up to a twentieth of the rate, and above that OPENER than its label, as the firmware's top was.
	///   A trapezoid there (tried first) put a zero at Nyquist: MEASURED 0.3 dB brighter than the firmware's default from
	///   10 to 16 kHz in 44.1 kHz, and it turned two cases red that the firmware's one-pole passes — the click detector
	///   read 4.3 a second on Sky at -1 dBFS against none, and Oversample's image drop on Owl at 7 kHz fell to 17.7 dB.
	void setHpfHz(float hz) noexcept { hpG_ = hz > 0.0f ? tptGain(hz) : -1.0f; }
	void setLpfHz(float hz) noexcept { lpA_ = hz > 0.0f ? onePoleAt(hz) : -1.0f; }
	/// THE TRAPEZOIDAL ONE-POLE'S GAIN FOR A CORNER: `G = g / (1 + g)`, `g = tan(π f / rate)` (the corner prewarped), the
	/// corner held under 0.49 of this rate.
	[[nodiscard]] float tptGain(float hz) const noexcept {
		const double rate = kReferenceRate * ratio_;
		const double g = std::tan(3.14159265358979323846 * std::min(static_cast<double>(hz), 0.49 * rate) / rate);
		return static_cast<float>(g / (1.0 + g));
	}
	/// ONE STEP OF IT: the lowpass of `x`, its state moved on. The highpass is `x` minus this.
	[[gnu::always_inline]] static float tptStep(float x, float& state, float G) noexcept {
		const float v = (x - state) * G;
		const float lp = v + state;
		state = lp + v;
		return lp;
	}
	/// THE ONE-POLE COEFFICIENT WITH ITS POLE AT A CORNER: `1 - e^(-2π f / rate)`. It is what `rc` does to a firmware
	/// coefficient (`1 - (1-c)^(1/ratio)` holds `-ln(1-c) · rate` fixed), so the corner is the one the firmware's
	/// one-poles have always meant, and the readout the firmware's LPF printed.
	[[nodiscard]] float onePoleAt(float hz) const noexcept {
		return static_cast<float>(-std::expm1(-2.0 * 3.14159265358979323846 * static_cast<double>(hz) / (kReferenceRate * ratio_)));
	}
	/// What the input highpass and the output lowpass run on: the trapezoid's gain and the lowpass's coefficient in Hz
	/// (negative when the firmware's knob is in charge), and the firmware's own coefficients on its knobs at this rate.
	[[nodiscard]] float hpGainForTest() const noexcept { return hpG_; }
	[[nodiscard]] float lpCoeffForTest() const noexcept { return lpA_; }
	[[nodiscard]] float firmwareHpAlphaForTest() const noexcept { return hpAlpha(); }
	[[nodiscard]] float firmwareLpCoeffForTest() const noexcept { return outLpCoeff(); }
	void setZone1(int32_t value) noexcept;
	void setZone2(int32_t value) noexcept;
	void setZone3(int32_t value) noexcept;
	void setPredelay(float value) noexcept;
	void setDryMinus(bool value) noexcept { dryMinus_ = value; }
	/// OWL'S COMPRESSOR on its loop's audio (the Owl Comp audition switch); off is the firmware.
	void setOwlCompressor(bool on) noexcept { owlCompOn_ = on; }
	/// **THE MODULATION RAMPED, NOT STEPPED** (micro noise impulse diracs around the attack in Sky,
	/// "in the tail" in Owl). The firmware steps its random walk, and every length and gain derived from it, once per
	/// buffer; ramped across the buffer the same values arrive smoothly. MEASURED: Sky's 6-14 kHz floor 17-19 dB lower,
	/// Owl's ~7 dB at 2x. OFF by default, so the parity cases driving the core still compare against the firmware.
	void setSmoothModulation(bool on) noexcept { smoothMod_ = on; }

	/// **CHARACTER (Feathers, not the firmware).** Six bipolar phi triangles, evaluated by the device from its
	/// Character coordinate; each topology maps them onto its OWN hidden internals (`refreshCharacter`).
	/// ALL ZERO IS THE FIRMWARE, EXACTLY: every mapping is `x * 2^0` or `x + 0`, so the parity cases stand.
	void setCharacter(const std::array<float, kCharacterTriangles>& t) noexcept { charT_ = t; }
	/// **PREDELAY IS ONLY A DELAY** when set. The firmware also feeds `predelay_` into
	/// the level envelope (`prevOutputMono_`) and Owl's servo attack; decoupled, both read Owl's Character
	/// instead, and are zero elsewhere. Off by default so the core stays the reference's.
	void setPredelayDecoupled(bool on) noexcept { decoupledPredelay_ = on; }
	/// The q31 gains the wet return is scaled by, left and right.
	void setReturnLevels(int32_t left, int32_t right) noexcept {
		returnLevelL_ = left;
		returnLevelR_ = right;
	}

	[[nodiscard]] int32_t getZone1() const noexcept { return zone1_; }
	[[nodiscard]] int32_t getZone2() const noexcept { return zone2_; }
	[[nodiscard]] int32_t getZone3() const noexcept { return zone3_; }

	// === TEST SEAMS — a claim about a value cannot be inferred from a rendered buffer. ===
	[[nodiscard]] size_t fdnLengthForTest(size_t line) const noexcept { return fdnLengths_[line % 3]; }
	[[nodiscard]] size_t cascadeLengthForTest(size_t stage) const noexcept { return cascadeLengths_[stage % 4]; }
	[[nodiscard]] size_t predelayLengthForTest() const noexcept { return predelayLength_; }
	[[nodiscard]] float modDepthForTest() const noexcept { return modDepth_; }
	[[nodiscard]] float feedbackForTest() const noexcept { return feedback_; }
	/// 0..3 Normal, 4 Feather, 5 Sky, 6 Owl, 7 Vast — read off the FLAGS, not off `zone2_`, because
	/// the flags are what `process` branches on.
	[[nodiscard]] int topologyForTest() const noexcept {
		return vastChainMode_ ? 7 : owlMode_ ? 6 : skyChainMode_ ? 5 : featherMode_ ? 4 : 0;
	}
	[[nodiscard]] float randWalkForTest() const noexcept { return skyRandWalkSmooth_; }
	/// OWL'S SERVO, as `beginBuffer` last left it: its gain, the output envelope and the held input peak it compares.
	[[nodiscard]] float owlServoScaleForTest() const noexcept { return owlFbEnvScale_; }
	[[nodiscard]] float owlOutputEnvelopeForTest() const noexcept { return feedbackEnvelope_; }
	[[nodiscard]] float owlInputPeakForTest() const noexcept { return inputAccum_; }
	/// **OWL PROBES** (probe owl and propose modifications to make it better behaved). Every
	/// default is the firmware; a case sets one to measure what it changes.
	struct OwlProbe {
		int envelope = 0;       ///< 0 the firmware's (one sample a buffer); 1 the buffer's mean |mono|, same tracker
		float maxScale = 10.0f; ///< the servo's ceiling (the firmware clamps its target at 10)
		float loopSat = 0.0f;   ///< > 0: a soft ceiling, L·tanh(x/L), on the global feedback's return
		float servoDown = 0.0f; ///< > 0: the servo's step when it is CUTTING gain (the firmware moves both ways at 0.003)
		float kneeMul = 1.0f;   ///< the servo's target (the output against the held input peak), scaled
		float guardAt = 0.0f;   ///< > 0: a RUNAWAY GUARD — only while the output is past this many times the target, the
		                        ///  servo cuts at `guardStep`; below it the firmware's servo, untouched
		float guardStep = 0.05f;
		float gainKnee = 0.0f;  ///< > 0: A — the servo's APPLIED gain compressed above this knee by `gainRatio` (its state untouched)
		float gainRatio = 4.0f;
		float compAt = 0.0f;    ///< > 0: B — a compressor on the global feedback's return, threshold (core units), `compRatio`
		float compRatio = 4.0f;
		float ceilDb = 99.0f;   ///< < 99: THE TARGET'S CEILING — the servo never holds the output above this (device dBFS)
		int compPlace = 0;      ///< where the compressor sits: 0 the global return, 1 the START of the loop (the cascade's input)
		float compServoPow = 0.0f; ///< > 0: the threshold divided by servo^this while the servo is above 1 (cranked: bite earlier)
		float compAttack = 0.0f;  ///< > 0: the detector's attack step per sample at 44.1 kHz (the device's is 0.02)
		float compRelease = 0.0f; ///< > 0: its release step (the device's is 0.0005)
		float compGainSmooth = 0.0f; ///< > 0: a one-pole on the APPLIED gain, this step per sample (0: the gain is raw)
		bool freezeLfo = false;   ///< the Sky/Vast/Owl random walk held still (its per-BUFFER steps are a suspect for clicks)
		bool rampLfo = false;     ///< the random walk RAMPED across each buffer instead of stepped at its start
		bool fracCascade = false; ///< the C2/C3 pitch-modulated taps read at a FRACTIONAL offset, not a whole sample
		bool freezeEnv = false;   ///< the output envelope (Sky/Vast's loop-gain scale) held, so it takes no per-buffer steps
		bool freezeServo = false; ///< Owl's servo gain held at 1, so it takes no per-buffer steps
	};
	void setOwlProbeForTest(const OwlProbe& p) noexcept { owlProbe_ = p; }
	[[nodiscard]] uint64_t owlGuardFiresForTest() const noexcept { return owlGuardFires_; }
	/// HOW MANY TIMES Sky/Vast's C3 soft clip has bitten since construction. The clip sits where the
	/// output ceiling nearly does, so a rendered buffer mostly cannot show it; this can.
	[[nodiscard]] uint64_t clipsForTest() const noexcept { return clipsForTest_; }
	/// THE OUTPUT'S LEVEL EVENTS since construction, per channel-sample (backlog LD): the float clamp at
	/// 0.06, the ARM conversion saturating (at 1/32, inside that clamp, so it is the real ceiling), and the
	/// int32 add into the rendering buffer WRAPPING. Nothing else in the core is integer: the send is
	/// read into float at once, and the tank is float to the output.
	struct LevelEvents {
		uint64_t outClamp = 0;
		uint64_t outSat = 0;
		uint64_t outWrap = 0;
	};
	[[nodiscard]] const LevelEvents& levelEventsForTest() const noexcept { return levelEvents_; }
	/// WHAT CHARACTER SET, as of the last `beginBuffer` — the effective values the process body reads.
	struct CharacterReading {
		std::array<float, 4> coeffs;
		std::array<float, 4> firmwareCoeffs;
		float seriesMix, earlyGain, sideGain, balance, lfoAmp, echo, rotAdd, loopScale, wobbleMul, lfoFreqMul,
		    kneeMul, servoMul, envPredelay, widthMul, phaserDepth, flangerDepth;
	};
	/// THE FLANGER'S LAST OFFSET ON D0, in samples — what the read actually moved by.
	[[nodiscard]] float lastFlangeForTest() const noexcept { return lastFlangeD0_; }
	[[nodiscard]] CharacterReading characterForTest() const noexcept {
		return {effCoeffs_,     cascadeCoeffs_,  effSeriesMix_, effEarlyGain_, effSideGain_,
		        effBalance_,    effLfoAmp_,      effEcho_,      charRotAdd_,   charLoopScale_,
		        charWobbleMul_, charLfoFreqMul_, charKneeMul_,  charServoMul_, envPredelay_,
		        effWidthMul_,   phaserDepth_,    flangerDepth_};
	}

private:
	/// PORT: a one-pole coefficient at this rate. The IDENTITY at 44.1 kHz — `c` itself, not a
	/// round trip through `1 - (1 - c)`, which is not always `c` in float.
	/// PORT: a one-pole coefficient with its CORNER moved by `k` (octaves as a ratio): `1 - (1-c)^k`. `c`
	/// itself, exactly, at k = 1.
	[[nodiscard]] static float cornerScaled(float c, float k) noexcept {
		return k == 1.0f ? c : std::clamp(1.0f - static_cast<float>(std::pow(1.0 - static_cast<double>(c), static_cast<double>(k))), 1.0e-5f, 0.999f);
	}
	[[nodiscard]] float rc(float c) const noexcept {
		return exact_ ? c : 1.0f - static_cast<float>(std::pow(1.0 - static_cast<double>(c), 1.0 / ratio_));
	}
	/// PORT: a length at this rate that keeps the reference's PARITY, for an offset into a line that is
	/// written twice per update (see `multiTap_`). The identity at 44.1 kHz.
	[[nodiscard]] size_t scaledSameParity(size_t n) const noexcept {
		const size_t m = scaled(n);
		return (m & 1u) == (n & 1u) ? m : m + 1;
	}
	/// PORT: a length in samples at this rate. The identity at 44.1 kHz.
	[[nodiscard]] size_t scaled(size_t n) const noexcept {
		return exact_ ? n : static_cast<size_t>(static_cast<double>(n) * ratio_ + 0.5);
	}

	double ratio_ = 1.0;
	float ratioF_ = 1.0f;
	bool exact_ = true;
	int bufferFrames_ = kReferenceBufferFrames;

	// PORT: the reference's lengths, at this rate.
	size_t d0Min_ = kD0MinLength;
	size_t d0Max_ = kD0MaxLength;
	size_t d1Min_ = kD1MinLength;
	size_t d1Max_ = kD1MaxLength;
	size_t d2Min_ = kD2MinLength;
	size_t d2Max_ = kD2MaxLength;
	std::array<size_t, kNumCascade> cascadeBase_ {kC0BaseLength, kC1BaseLength, kC2BaseLength, kC3BaseLength};
	std::array<size_t, kNumCascade> multiTap_ = kMultiTapOffsets;
	size_t predelayMax_ = kPredelayMaxLength;
	float tapMod_ = 280.0f;
	uint16_t silenceThreshold_ = 64;
	uint16_t silenceCount_ = 0;
	// PORT: the reference's fixed one-pole coefficients, at this rate.
	float dcCoeff_ = 0.007f;
	float owlDcCoeff_ = 0.057f;
	float peakCoeff_ = 0.1f;
	float aaCoeff_ = 0.30f;
	float lpMonoCoeff_ = 0.45f;
	float lpSideCoeff_ = 0.7f;

	int32_t returnLevelL_ = 0;
	int32_t returnLevelR_ = 0;

	float* buffer_ {nullptr};
	size_t bufferFloats_ = 0;
	uint64_t clipsForTest_ = 0;
	LevelEvents levelEvents_ {};
	OwlProbe owlProbe_ {};
	uint64_t owlGuardFires_ = 0;
	float owlCompEnv_ = 0.0f;
	float owlCompGain_ = 1.0f;
	/// The random walk's ramp across a buffer (`OwlProbe::rampLfo`): from, to, the step into it, and its length.
	float lfoFrom_ = 0.0f;
	float lfoTo_ = 0.0f;
	uint32_t lfoPos_ = 0;
	uint32_t lfoLen_ = 1;
	bool owlCompOn_ = false;
	bool smoothMod_ = false;
	float owlCompAttack_ = 0.02f;
	float owlCompRelease_ = 0.0005f;
	static constexpr float kOwlCompThreshold = 0.005f;
	static constexpr float kOwlCompRatio = 4.0f;
	static constexpr float kOwlCompServoPow = 2.0f;
	double owlSumAbs_ = 0.0;
	uint32_t owlSumCount_ = 0;

	// CHARACTER — the triangles, and the EFFECTIVE values `beginBuffer` derives from the firmware's own
	// members plus them. The process body reads these where the firmware read the members.
	std::array<float, kCharacterTriangles> charT_ {};
	float effWidthMul_ = 1.0f;
	// THE POCKETS: depth 0 is off (and the firmware), and each carries its own LFO.
	float phaserDepth_ = 0.0f;
	double phaserRate_ = 0.0;
	double phaserPhase_ = 0.0;
	float flangerDepth_ = 0.0f;
	double flangerRate_ = 0.0;
	double flangePhase_ = 0.0;
	float lastFlangeD0_ = 0.0f;
	bool decoupledPredelay_ = false;
	std::array<float, 4> effCoeffs_ {0.5f, 0.5f, 0.5f, 0.5f};
	float effSeriesMix_ = 0.6f;
	float effEarlyGain_ = 0.3f;
	float effSideGain_ = 0.2f;
	float effBalance_ = 0.5f;
	float effLfoAmp_ = 0.3f;
	float effEcho_ = 0.0f;
	float charRotAdd_ = 0.0f;
	float charLoopScale_ = 1.0f;
	float charWobbleMul_ = 1.0f;
	float charLfoFreqMul_ = 1.0f;
	float charKneeMul_ = 1.0f;
	float charServoMul_ = 1.0f;
	float charOwlHpK_ = 1.0f;
	float charOwlLpK_ = 1.0f;
	float envPredelay_ = 0.0f;
	void refreshCharacter() noexcept;


	// FDN delay state (3 delays)
	std::array<size_t, kNumFdnDelays> fdnWritePos_{};
	std::array<size_t, kNumFdnDelays> fdnOffsets_{};
	std::array<size_t, kNumFdnDelays> fdnLengths_{kD0MaxLength, kD1MaxLength, kD2MaxLength};
	std::array<float, kNumFdnDelays> fdnLpState_{};

	// Cascade state (4 allpass stages)
	std::array<size_t, kNumCascade> cascadeWritePos_{};
	std::array<size_t, kNumCascade> cascadeOffsets_{};
	std::array<size_t, kNumCascade> cascadeLengths_{kC0BaseLength, kC1BaseLength, kC2BaseLength, kC3BaseLength};
	float cascadeScale_{1.0f};            // Current scale factor from Zone 2
	float earlyMixGain_{0.3f};            // Early reflection gain (scales inverse with Zone 2: smaller = more early)
	float tailMixGain_{0.6f};             // Tail output gain (scales with Zone 2: bigger room = more tail)
	float directEarlyGain_{0.15f};        // Direct early tap (bypasses output LPF for brightness)
	float cascadeLpState_{0.0f};          // LP filter state for cascade output (L channel in Feather)
	float cascadeLpStateR_{0.0f};         // LP filter state for cascade output (R channel in Feather)
	float cascadeSeriesMix_{0.6f};        // 0=parallel (C3 from cascadeIn), 1=series (C3 from c2)
	float cascadeFeedbackMult_{0.7f};     // How much cascade feeds back into FDN (controlled by Zone 3)
	float cascadeNestFeedback_{0.0f};     // Nested feedback: C3 → C0 for extended tails (combined)
	float cascadeNestFeedbackBase_{0.0f}; // Base nest feedback from Zone 3 (before Zone 2 vast boost)
	float prevC3Out_{0.0f};               // Previous C3 output for nested feedback delay
	float delayRatio_{1.0f};              // (D0+D1)/(D0max+D1max) for feedback normalization
	static constexpr float kCascadeCoeffBase = 0.5f; // Base allpass coefficient for cascade (higher = more diffusion)
	std::array<float, kNumCascade> cascadeCoeffs_{kCascadeCoeffBase, kCascadeCoeffBase, kCascadeCoeffBase,
	                                              kCascadeCoeffBase};

	// Predelay state (single tap, 50ms max)
	size_t predelayOffset_{0};
	size_t predelayWritePos_{0};
	size_t predelayLength_{0};
	float prevOutputMono_{0.0f};

	// Parameters
	float roomSize_{0.5f};
	float damping_{0.5f};
	float width_{1.0f};
	float hpCutoff_{0.0f};
	float lpCutoff_{1.0f};
	/// `setHpfHz`'s trapezoidal gain and `setLpfHz`'s coefficient; negative is the firmware's knob.
	float hpG_{-1.0f};
	float lpA_{-1.0f};
	/// THE FIRMWARE'S TWO COEFFICIENTS on its knobs (Owl's highpass its own range), at this rate.
	[[nodiscard]] float hpAlpha() const noexcept {
		return rc(1.0f - (owlMode_ ? (0.997f - hpCutoff_ * 0.05f) : (0.995f - hpCutoff_ * 0.09f)));
	}
	[[nodiscard]] float outLpCoeff() const noexcept { return rc(0.1f + lpCutoff_ * 0.85f); }

	// Zone parameters
	int32_t zone1_{0};
	int32_t zone2_{512};
	int32_t zone3_{0};
	float predelay_{0.0f};

	// Dry subtraction toggle (predelay encoder button)
	bool dryMinus_{false}; // Runtime toggle: true = subtract dry input to remove bleedthrough

	// Derived coefficients
	float feedback_{0.85f};
	float dampCoeff_{0.5f};
	float cascadeDamping_{0.7f}; // Damping in cascade (darker tail)

	// 3x3 matrix for FDN (normalized Hadamard)
	// H3 = 1/sqrt(3) * [[1,1,1], [1,w,w^2], [1,w^2,w]] where w = e^(2πi/3)
	// Using real approximation: sign pattern with 1/sqrt(3) normalization
	std::array<std::array<float, 3>, 3> matrix_{{{kH3Norm, kH3Norm, kH3Norm},
	                                             {kH3Norm, -kH3Norm, 0.0f}, // Simplified orthogonal
	                                             {kH3Norm, 0.0f, -kH3Norm}}};

	// Per-delay feedback multipliers
	std::array<float, kNumFdnDelays> feedbackMult_{1.0f, 1.0f, 1.0f};

	// Filter states
	float hpState_{0.0f};
	float lpStateL_{0.0f};
	float lpStateR_{0.0f};
	float dcBlockState_{0.0f};

	// LFO for modulation (Sky/Vast use random walk, others have no pitch mod)
	float modDepth_{0.0f};          // LFO pitch wobble depth for FDN (controlled by Zone 3)
	float cascadeModDepth_{0.0f};   // LFO pitch wobble depth for C2/C3 in vast mode
	float cascadeAmpMod_{0.0f};     // LFO amplitude modulation depth for C2/C3 diffusion contour
	float widthBreath_{0.0f};       // Width breathing amount (controlled by Zone 3)
	float crossBleed_{0.0f};        // L↔R cross-channel bleed in FDN (controlled by Zone 3)
	float fdnFeedbackScale_{1.0f};  // FDN feedback multiplier (kept at 1.0 - room knob controls decay)
	float cascade_side_gain_{0.2f}; // Stereo side signal gain (higher for Owl mode)

	// Envelope followers for Owl mode feedback servo
	float feedbackEnvelope_{0.0f}; // Tracks output level for ratio-based limiting
	float owlFbEnvScale_{1.0f};    // Servo-controlled feedback scale
	uint8_t owlSilenceCount_{0};   // Buffers below noise floor (reset tracking after threshold)

	// Undersampling state
	bool undersamplePhase_{false};
	float accumIn_{0.0f};
	float inputAccum_{0.0f};    // Peak hold of |fdnIn|
	bool inputPeakReset_{true}; // Reset peak on next above-noise sample
	float prevOutL_{0.0f};
	float prevOutR_{0.0f};
	float currOutL_{0.0f};
	float currOutR_{0.0f};

	// Cascade extra undersampling for vast rooms
	// Normal: 2x, Lush/Vast: all stages at uniform 4x (8x caused ringing)
	bool cascadeDoubleUndersample_{false}; // When true, cascade runs at 4x undersample (Lush or Vast zones)
	bool vastChainMode_{false};            // When true, uses nested topology with 4x undersample (Vast only)
	bool skyChainMode_{false};             // When true, uses nested topology with 2x undersample (Sky only)
	bool owlMode_{false};                  // When true, uses nested topology with smeared feedback at 4x (Owl only)
	float skyFbBalance_{0.5f};             // Z1-controlled balance: 0=C2→C0 only, 1=C3→C1 only
	float skyLfoAmp_{0.3f};                // Z1-controlled LFO amplitude for Sky mode
	float skyLfoFreq_{1.0f};               // Z1-controlled LFO frequency multiplier for Sky mode
	float skyLoopFb_{1.0f};                // Z3-controlled local loop feedback multiplier for Sky mode
	float skyLfoRouting_{0.5f};            // Z3-controlled LFO routing: 0=pitch only, 1=amp only
	float skyRandWalk_{0.0f};              // Random walk position for Sky mode pitch wobble (-1 to 1)
	float skyRandWalkSmooth_{0.0f};        // Smoothed random walk output
	// NOT SAVED WITH THE PATCH, AND WHY (backlog NM): the Sky walk is a slow pitch wobble under a reverb tail, from a
	// constant seed; where it stands is not a decision anyone made, and the tails it colours are audio, not persisted.
	uint32_t skyRandState_{12345};         // LCG state for random walk
	float vastLfoCache_{0.0f};             // Cached LFO value for Vast mode (synced to 4x update rate)
	bool featherMode_{false};              // When true, experimental mode placeholder (zone 4)
	float cascadeAaState1_{0.0f};          // Anti-alias LP filter state (pre-decimation, vast only)
	float owlInputAaState_{0.0f};          // Anti-alias LP filter state for FDN input (Owl mode)
	float cascadeLpStateMono_{0.0f};       // Cascade output LP filter state (mono component)
	float cascadeLpStateSide_{0.0f};       // Cascade output LP filter state (side component)
	static constexpr float kPreCascadeAaCoeff = 0.30f;  // LP coeff ~3kHz for pre-decimation AA (below 4x Nyquist)
	static constexpr float kCascadeLpCoeffMono = 0.45f; // LP coeff ~4kHz for cascade mono (darker tail)
	static constexpr float kCascadeLpCoeffSide = 0.7f;  // LP coeff ~6kHz for cascade side (brighter stereo spread)
	static constexpr float kOwlLpCoeffMono = 0.55f;     // Owl: brighter mono LP for more harmonic content
	static constexpr float kOwlLpCoeffSide = 0.8f;      // Owl: brighter side LP for richer stereo
	uint8_t c0Phase_{0};                                // Phase counter for C0 undersampling
	float c0Accum_{0.0f};                               // Accumulated input for C0
	float c0Prev_{0.0f};                                // Previous C0 output for interpolation
	uint8_t c1Phase_{0};                                // Phase counter for C1 undersampling
	float c1Accum_{0.0f};                               // Accumulated input for C1
	float c1Prev_{0.0f};                                // Previous C1 output for interpolation
	uint8_t c2Phase_{0};                                // Phase counter for C2 undersampling
	float c2Accum_{0.0f};                               // Accumulated input for C2
	float c2Prev_{0.0f};                                // Previous C2 output for interpolation
	uint8_t c3Phase_{0};                                // Phase counter for C3 undersampling
	float c3Accum_{0.0f};                               // Accumulated input for C3
	float c3Prev_{0.0f};                                // Previous C3 output for interpolation
	float owlD0Cache_{0.0f};                            // Cached D0 read for 4x undersample (Owl mode)
	float owlD0ReadAccum_{0.0f};                        // Accumulated D0 reads for AA (Owl mode)
	float owlD0WriteAccum_{0.0f};                       // Accumulated D0 writes for AA (Owl mode)
	float owlD0WriteVal_{0.0f};                         // Held D0 write value for 4x undersample (Owl mode)
	float owlD1Cache_{0.0f};                            // Cached D1 read for 4x undersample (Owl mode)
	float owlD1ReadAccum_{0.0f};                        // Accumulated D1 reads for AA (Owl mode)
	float owlD1WriteAccum_{0.0f};                       // Accumulated D1 writes for AA (Owl mode)
	float owlD1WriteVal_{0.0f};                         // Held D1 write value for 4x undersample (Owl mode)
	float owlD2Cache_{0.0f};                            // Cached D2 read for 4x undersample (Owl mode)
	float owlD2ReadAccum_{0.0f};                        // Accumulated D2 reads for AA (Owl mode)
	float owlD2WriteAccum_{0.0f};                       // Accumulated D2 writes for AA (Owl mode)
	float owlD2WriteVal_{0.0f};                         // Held D2 write value for 4x undersample (Owl mode)
	float owlEchoGain_{0.0f};                           // Z3-controlled D2 echo tap gain (phi triangle)
	float owlEnvRatio_{1.0f};                           // Predelay-controlled attack/release ratio
	float owlZ3Norm_{0.5f};                             // Z3 absolute position for max boost scaling

	// Direct early tap (bypasses output LPF for brightness)
	float directEarlyL_{0.0f};
	float directEarlyR_{0.0f};

	// Update functions
	void updateMatrix() noexcept;
	void updateFeedbackPattern() noexcept;
	void updateSizes() noexcept; // Updates D2 and cascade lengths from Zone 2

	// FDN delay helpers
	[[gnu::always_inline]] float fdnRead(size_t line) const { return buffer_[fdnOffsets_[line] + fdnWritePos_[line]]; }

	// Interpolating delay read for smooth pitch modulation
	/// PROBE `fracCascade`: a cascade stage read `offset` samples AHEAD of its write position (as the modulated taps read),
	/// at a FRACTIONAL offset. The stage is written twice per update (a pair of equal slots), so it interpolates between
	/// PAIRS: `offset / 2` pairs ahead, the fraction between that pair and the next.
	[[nodiscard]] float cascadeReadFrac(size_t stage, size_t writePos, float offset) const {
		const size_t len = cascadeLengths_[stage];
		const float pairs = 0.5f * offset;
		const auto whole = static_cast<size_t>(pairs);
		const float frac = pairs - static_cast<float>(whole);
		const size_t a = (writePos + 2 * whole) % len;
		const size_t b = (a + 2) % len;
		const float va = buffer_[cascadeOffsets_[stage] + a];
		const float vb = buffer_[cascadeOffsets_[stage] + b];
		return va + frac * (vb - va);
	}
	/// **A MODULATED READ OF A DOUBLE-WRITTEN LINE** (Vast writes each FDN line twice per update, so it holds PAIRS of
	/// equal samples): interpolated between PAIRS, `offset / 2` of them ahead, so a moving read slides smoothly over two
	/// samples where `fdnReadAtInterp` tracks the staircase's step at every pair boundary — the crackle heard in
	/// Vast's tail. Used with `setSmoothModulation`; the firmware's reader is kept for the parity cases.
	[[gnu::always_inline]] float fdnReadAtPairs(size_t line, float offset) const {
		const size_t len = fdnLengths_[line];
		const size_t base = fdnOffsets_[line];
		const float pairs = 0.5f * offset;
		const auto whole = static_cast<size_t>(pairs);
		const float frac = pairs - static_cast<float>(whole);
		size_t pos0 = fdnWritePos_[line] + 2 * whole;
		while (pos0 >= len) {
			pos0 -= len;
		}
		size_t pos1 = pos0 + 2;
		while (pos1 >= len) {
			pos1 -= len;
		}
		const float s0 = buffer_[base + pos0];
		const float s1 = buffer_[base + pos1];
		return s0 + frac * (s1 - s0);
	}
	[[gnu::always_inline]] float fdnReadAtInterp(size_t line, float offset) const {
		size_t len = fdnLengths_[line];
		size_t base = fdnOffsets_[line];
		size_t pos0 = fdnWritePos_[line] + static_cast<size_t>(offset);
		if (pos0 >= len) {
			pos0 -= len;
		}
		size_t pos1 = pos0 + 1;
		if (pos1 >= len) {
			pos1 = 0;
		}
		float frac = offset - static_cast<float>(static_cast<size_t>(offset));
		float s0 = buffer_[base + pos0];
		float s1 = buffer_[base + pos1];
		return s0 + frac * (s1 - s0);
	}

	[[gnu::always_inline]] void fdnWrite(size_t line, float value) {
		buffer_[fdnOffsets_[line] + fdnWritePos_[line]] = value;
		if (++fdnWritePos_[line] >= fdnLengths_[line]) {
			fdnWritePos_[line] = 0;
		}
	}

	// Cascade allpass helpers
	[[gnu::always_inline]] float processCascadeStage(size_t stage, float input) {
		size_t idx = cascadeOffsets_[stage] + cascadeWritePos_[stage];
		float delayed = buffer_[idx];
		float coeff = effCoeffs_[stage]; // PORT: Character's (the firmware's at Character 0)
		float output = -coeff * input + delayed;
		float writeVal = input + coeff * output;
		buffer_[idx] = writeVal;
		// (The reference's multi-tap write here is compiled out: `kEnableMultiTapWrites` is false.)
		if (++cascadeWritePos_[stage] >= cascadeLengths_[stage]) {
			cascadeWritePos_[stage] = 0;
		}
		return output;
	}

	// Predelay helpers — PORT: the wrap is the scaled maximum.
	[[gnu::always_inline]] float readPredelay(size_t offset) const {
		if (predelayLength_ == 0)
			return 0.0f;
		size_t pos = (predelayWritePos_ >= offset) ? (predelayWritePos_ - offset)
		                                           : (predelayWritePos_ + predelayMax_ - offset);
		return buffer_[predelayOffset_ + pos];
	}

	[[gnu::always_inline]] void writePredelay(float value) {
		buffer_[predelayOffset_ + predelayWritePos_] = value;
		if (++predelayWritePos_ >= predelayMax_) {
			predelayWritePos_ = 0;
		}
	}

	[[gnu::always_inline]] static float onepole(float input, float& state, float coeff) {
		state += coeff * (input - state);
		return state;
	}
};

} // namespace feathers::fx::featherverb
