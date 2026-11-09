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


#include "fx/featherverb/featherverb_core.h"

#include "character/fixedpoint.h"
#include "character/phi_triangle.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>

namespace feathers::fx::featherverb {

namespace phi = owlet::dsp::phi;

namespace {

// Compile-time diagnostic toggles — the reference's, all off.
constexpr bool kMuteEarly = false;
constexpr bool kMuteCascade = false;
constexpr bool kMuteCascadeFeedback = false;
constexpr bool kBypassFdnToCascade = false;
constexpr bool kDisableVastUndersample = false;

//--------------------------------------------------------------------------------------------------//
// PORT: THE REFERENCE'S TRIANGLE ARITHMETIC, restated here because Feathers' shared phi layer has    //
// moved on from it. `src/character/phi_triangle.hpp` now wraps in double (backlog EH) and folds its  //
// triangle differently. Both are better for an unbounded coordinate, and both round differently at   //
// the last bit. This reverb's coordinates are bounded (0..1023), and parity needs the reference's float //
// adds. These three are the firmware's `triangleSimpleUnipolar`, `wrapPhase` and                     //
// `PhiTriContext::eval` with `freqMult = periodScale = 1`, operation for operation.                  //
//--------------------------------------------------------------------------------------------------//

float fwTriangleSimpleUnipolar(float phase, float duty = 1.0f) {
	phase = phase - floorf(phase);
	float halfDuty = duty * 0.5f;
	float invHalfDuty = 2.0f / duty;
	if (phase < halfDuty) {
		return phase * invHalfDuty;
	}
	else if (phase < duty) {
		return (duty - phase) * invHalfDuty;
	}
	return 0.0f;
}

float fwWrapPhase(double phase) {
	int64_t truncated = static_cast<int64_t>(phase);
	double floored = (phase < 0.0 && phase != static_cast<double>(truncated)) ? static_cast<double>(truncated - 1)
	                                                                          : static_cast<double>(truncated);
	return static_cast<float>(phase - floored);
}

template <size_t N>
std::array<float, N> fwEvalBank(float yNorm, double gammaPhase, const std::array<phi::PhiTriConfig, N>& configs) {
	constexpr float freqMult = 1.0f;
	constexpr float periodScale = 1.0f;
	std::array<float, N> results;
	for (size_t i = 0; i < N; ++i) {
		const phi::PhiTriConfig& c = configs[i];
		float ph = fwWrapPhase(gammaPhase * c.phiFreq);
		float base = static_cast<float>(static_cast<double>(yNorm) * c.phiFreq * freqMult * periodScale);
		float tri = fwTriangleSimpleUnipolar(fwWrapPhase(base + ph + c.phaseOffset), c.duty);
		results[i] = c.bipolar ? (tri * 2.0f - 1.0f) : tri;
	}
	return results;
}

size_t scaledLength(size_t n, double ratio) {
	return ratio == 1.0 ? n : static_cast<size_t>(static_cast<double>(n) * ratio + 0.5);
}

size_t cascadeMaxLength(size_t base) { return static_cast<size_t>(base * Core::kCascadeMaxScale); }

/// The high word of a * b, rounded to nearest (`character/fixedpoint.h`).
int32_t smmulr(int32_t a, int32_t b) { return feathers::character::multiply_accumulate_32x32_rshift32_rounded(0, a, b); }

/// ARM `vcvt.s32.f32`: truncate toward zero, saturate, NaN to zero.
int32_t cvtSat(float x) {
	if (!(x < 2147483648.0f)) {
		return (x != x) ? 0 : std::numeric_limits<int32_t>::max();
	}
	if (x < -2147483648.0f) {
		return std::numeric_limits<int32_t>::min();
	}
	return static_cast<int32_t>(x);
}

int32_t wrapAdd(int32_t a, int32_t b) {
	return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

/// Whether `wrapAdd(a, b)` wrapped: the exact sum, in 64 bits, falls outside int32. The same answer as GCC's
/// `__builtin_add_overflow`, which MSVC does not have.
bool addWraps(int32_t a, int32_t b) {
	const int64_t sum = static_cast<int64_t>(a) + static_cast<int64_t>(b);
	return sum > std::numeric_limits<int32_t>::max() || sum < std::numeric_limits<int32_t>::min();
}

} // namespace

size_t Core::bufferFloatsFor(double sampleRate) noexcept {
	const double ratio = sampleRate > 0.0 ? sampleRate / kReferenceRate : 1.0;
	size_t total = scaledLength(kD0MaxLength, ratio) + scaledLength(kD1MaxLength, ratio)
	               + scaledLength(kD2MaxLength, ratio);
	for (size_t base : {kC0BaseLength, kC1BaseLength, kC2BaseLength, kC3BaseLength}) {
		total += cascadeMaxLength(scaledLength(base, ratio));
	}
	return total + scaledLength(kPredelayMaxLength, ratio);
}

Core::Core(double sampleRate, float* buffer) noexcept {
	// PORT: the rate, and every length and coefficient that follows from it. At 44.1 kHz these are
	// the reference's own constants, untouched (see `scaled` and `rc`).
	ratio_ = sampleRate > 0.0 ? sampleRate / kReferenceRate : 1.0;
	exact_ = ratio_ == 1.0;
	ratioF_ = static_cast<float>(ratio_);
	bufferFrames_ = exact_ ? kReferenceBufferFrames
	                       : std::max(2, static_cast<int>(std::lround(kReferenceBufferFrames * ratio_ / 2.0)) * 2);
	d0Min_ = scaled(kD0MinLength);
	d0Max_ = scaled(kD0MaxLength);
	d1Min_ = scaled(kD1MinLength);
	d1Max_ = scaled(kD1MaxLength);
	d2Min_ = scaled(kD2MinLength);
	d2Max_ = scaled(kD2MaxLength);
	cascadeBase_ = {scaled(kC0BaseLength), scaled(kC1BaseLength), scaled(kC2BaseLength), scaled(kC3BaseLength)};
	for (size_t i = 0; i < kNumCascade; ++i) {
		// PORT: SCALED, AND OF THE SAME PARITY AS THE REFERENCE'S. Vast and Owl write C2/C3 twice per update and
		// read the even slot, so an ODD offset lands its tap in the slot that is never read: at 44.1 kHz
		// (509, 1607) the multi-tap is in effect switched off. Scaled plainly to 48 kHz, 509 became 554, an
		// EVEN offset, so the tap went live and fed Vast's global loop past unity. MEASURED: 24 of 48 Vast
		// settings ran away to the output ceiling at 48 kHz, 36 of 48 at 96 kHz. With the parity kept,
		// none ran away at 48, 88.2, 96 or 192 kHz.
		multiTap_[i] = scaledSameParity(kMultiTapOffsets[i]);
	}
	predelayMax_ = scaled(kPredelayMaxLength);
	tapMod_ = 280.0f * ratioF_;
	silenceThreshold_ = static_cast<uint16_t>(scaled(64));
	dcCoeff_ = rc(0.007f);
	owlDcCoeff_ = rc(0.057f);
	owlCompAttack_ = rc(0.02f); // PORT-STYLE: the measured per-sample coefficients at 44.1 kHz, held in time at any rate
	owlCompRelease_ = rc(0.0005f);
	peakCoeff_ = rc(0.1f);
	aaCoeff_ = rc(kPreCascadeAaCoeff);
	lpMonoCoeff_ = rc(kCascadeLpCoeffMono);
	lpSideCoeff_ = rc(kCascadeLpCoeffSide);
	fdnLengths_ = {d0Max_, d1Max_, d2Max_};
	cascadeLengths_ = cascadeBase_;
	buffer_ = buffer;
	bufferFloats_ = bufferFloatsFor(sampleRate);

	// Compute buffer offsets for contiguous layout
	size_t offset = 0;

	// FDN delays (3 delays)
	for (size_t i = 0; i < kNumFdnDelays; ++i) {
		fdnOffsets_[i] = offset;
		offset += (i == 0) ? d0Max_ : (i == 1) ? d1Max_ : d2Max_;
	}

	// Cascade stages (4 allpass delays) - allocate max size
	for (size_t i = 0; i < kNumCascade; ++i) {
		cascadeOffsets_[i] = offset;
		offset += cascadeMaxLength(cascadeBase_[i]);
	}

	// Predelay
	predelayOffset_ = offset;

	// Initialize defaults
	setRoomSize(0.5f);
	setDamping(0.5f);
	updateMatrix();
	updateSizes();
	updateFeedbackPattern();

	// The reference's `setModel` allocates straight after constructing.
	allocate();
}

void Core::allocate() noexcept {
	if (buffer_ != nullptr) {
		std::memset(buffer_, 0, bufferFloats_ * sizeof(float));
	}

	// Reset state
	fdnWritePos_.fill(0);
	fdnLpState_.fill(0.0f);
	cascadeWritePos_.fill(0);
	cascadeLpState_ = 0.0f;
	cascadeLpStateR_ = 0.0f;
	cascadeLpStateMono_ = 0.0f;
	cascadeLpStateSide_ = 0.0f;
	feedbackEnvelope_ = 0.0f;
	// inputAccum_ persists - holds peak input level for servo ratio
	owlFbEnvScale_ = 1.0f;
	owlSilenceCount_ = 0;
	prevC3Out_ = 0.0f;
	predelayWritePos_ = 0;
	dcBlockState_ = 0.0f;
	hpState_ = 0.0f;
	lpStateL_ = 0.0f;
	lpStateR_ = 0.0f;
	prevOutputMono_ = 0.0f;
	cascadeModDepth_ = 0.0f;
	cascadeAmpMod_ = 0.0f;

	// Reset undersampling
	undersamplePhase_ = false;
	accumIn_ = 0.0f;
	inputAccum_ = 0.0f;
	inputPeakReset_ = true;
	prevOutL_ = prevOutR_ = currOutL_ = currOutR_ = 0.0f;

	// Reset cascade extra undersampling
	cascadeDoubleUndersample_ = false;
	vastChainMode_ = false;
	skyChainMode_ = false;
	featherMode_ = false;
	c0Phase_ = 0;
	c0Accum_ = 0.0f;
	c0Prev_ = 0.0f;
	c1Phase_ = 0;
	c1Accum_ = 0.0f;
	c1Prev_ = 0.0f;
	c2Phase_ = 0;
	c2Accum_ = 0.0f;
	c2Prev_ = 0.0f;
	c3Phase_ = 0;
	c3Accum_ = 0.0f;
	c3Prev_ = 0.0f;
	owlD0Cache_ = 0.0f;
	owlD0ReadAccum_ = 0.0f;
	owlD0WriteAccum_ = 0.0f;
	owlD0WriteVal_ = 0.0f;
	owlD1Cache_ = 0.0f;
	owlD1ReadAccum_ = 0.0f;
	owlD1WriteAccum_ = 0.0f;
	owlD1WriteVal_ = 0.0f;
	owlD2Cache_ = 0.0f;
	owlD2ReadAccum_ = 0.0f;
	owlD2WriteAccum_ = 0.0f;
	owlD2WriteVal_ = 0.0f;
	// PORT: the reference's shared `static` counter, this instance's own.
	silenceCount_ = 0;
	// PORT: Character's pocket LFOs start where a fresh device starts.
	phaserPhase_ = 0.0;
	flangePhase_ = 0.0;
}

void Core::process(const int32_t* inputSamples, int32_t* outL, int32_t* outR, size_t frames) noexcept {
	beginBuffer(frames);
	run(inputSamples, outL, outR, frames);
	endBuffer(frames);
}

//--------------------------------------------------------------------------------------------------//
// PORT: THE REFERENCE'S `process()` IN THREE PIECES, so one firmware buffer can span two host calls.   //
// The reference's buffer-rate prologue does two things at once. It STEPS state (the Owl servo, the   //
// random walk), and it DERIVES a few dozen hoisted constants from the members. The stepping is       //
// `beginBuffer`. The deriving stays at the top of `run`, which may now execute more than once per   //
// buffer, and gives the same numbers each time, because nothing it reads changes inside a buffer:    //
// the device applies settings only between buffers. The epilogue is `endBuffer`. `process` is the    //
// three in a row: the reference, statement for statement, in the same order.                        //
//--------------------------------------------------------------------------------------------------//

//--------------------------------------------------------------------------------------------------//
// PORT: CHARACTER (Feathers, not the firmware — a zone knob with phi triangles,   //
// every mode its own bank, Predelay only a delay). Six bipolar triangles `t`, each mode mapping them  //
// onto the internals that make it itself. EVERY MAPPING IS THE FIRMWARE'S VALUE AT t = 0, EXACTLY:     //
// `x * 2^0`, `x + 0`, and clamps that contain the firmware's own range.                               //
// FEEDBACK IS ONLY EVER TURNED DOWN by Character (`charLoopScale_` <= 1): a knob that could lift a    //
// loop gain past unity would make runaways a place on a dial.                                         //
//--------------------------------------------------------------------------------------------------//

void Core::refreshCharacter() noexcept {
	const std::array<float, kCharacterTriangles>& t = charT_;
	const auto octaves = [](float x) { return x == 0.0f ? 1.0f : std::exp2(x); };

	effCoeffs_ = cascadeCoeffs_;
	effSeriesMix_ = cascadeSeriesMix_;
	effEarlyGain_ = earlyMixGain_;
	effSideGain_ = cascade_side_gain_;
	effBalance_ = skyFbBalance_;
	effLfoAmp_ = skyLfoAmp_;
	effEcho_ = owlEchoGain_;
	charRotAdd_ = 0.0f;
	charLoopScale_ = 1.0f;
	charWobbleMul_ = 1.0f;
	charLfoFreqMul_ = 1.0f;
	charKneeMul_ = 1.0f;
	charServoMul_ = 1.0f;
	charOwlHpK_ = 1.0f;
	charOwlLpK_ = 1.0f;
	envPredelay_ = decoupledPredelay_ ? 0.0f : predelay_;

	// EVERY MODE: stereo width (stereo width should also vary), up to 3/4 octave either
	// way on the tail's side and the global side boost.
	effWidthMul_ = octaves(0.75f * t[6]);
	// EVERY MODE: THE POCKETS (modulated settings with lower duty cycles … stereo phasers, flangers").
	// Two LOW-DUTY triangles, so most of the lap is still and each effect appears in a window and leaves
	// again; deeper into its window, deeper and faster.
	phaserDepth_ = t[7];
	phaserRate_ = 0.1 * std::exp2(3.0 * static_cast<double>(t[7]));
	flangerDepth_ = t[8];
	flangerRate_ = 0.15 * std::exp2(3.0 * static_cast<double>(t[8]));

	if (owlMode_) {
		// OWL — its limiter is its character: how far the tail blooms over what was played, how fast the
		// limiter moves, how fast it swells (what Predelay did), the D2 echo, and its LFO.
		// BLOOM ONLY A THIRD OF AN OCTAVE UP (some owl character settings get a little out of control).
		// MEASURED over 401 coordinates, a 220 Hz pluck: with the full octave up, 12 coordinates at Decay 0
		// and 75 at Decay 50% peaked more than 12 dB over Character 0, nearly all with bloom near +1; with
		// a third, 0 and 23. (Halving the echo, and keeping the LFO depth down-only, changed nothing.)
		charKneeMul_ = octaves(t[0] > 0.0f ? t[0] / 3.0f : t[0]);
		charServoMul_ = octaves(1.5f * t[1]);
		// REGISTER (t9): the in-loop band (the feedback highpass and the cascade's lowpasses) up or down
		// together, ±1 octave (±1.5 measured first; the low end was the loudest of Owl's hot spots, and ±1
		// took the Decay-50% count from 23 to 18); at ±1 the centroid is 653/798/1114 Hz. MEASURED at ±1.5 (Decay 30%, noise burst, 1..3 s): the centroid ran
		// 601 → 794 → 1338 Hz across -1 → 0 → +1, and the strongest peak 387 → 598 → 1248 Hz; a lower
		// register is also peakier and louder (87% vs 63% of the energy in the top 1% of bins).
		// FOCUS (t10): the cascade's diffusion. MEASURED, and the OPPOSITE of the first guess: MORE allpass
		// coefficient is PEAKIER (84% of the energy in the top 1% of bins at +1, 76% at 0, 70% at -1) with
		// the centroid still (781..807 Hz). The first draft narrowed the band for focus; measured, the band
		// moved only the register (77% → 76%). resonate lower or higher, more or less intrinsically
		// peaky. Both the firmware at t = 0.
		charOwlHpK_ = octaves(t[9]);
		charOwlLpK_ = octaves(t[9]);
		if (t[10] != 0.0f) {
			for (size_t i = 0; i < kNumCascade; ++i) {
				effCoeffs_[i] = std::clamp(cascadeCoeffs_[i] + 0.15f * t[10], 0.2f, 0.9f);
			}
		}
		if (decoupledPredelay_) {
			envPredelay_ = std::fabs(t[2]);
		}
		effEcho_ = std::clamp(owlEchoGain_ + 0.3f * t[3], 0.0f, 0.6f);
		charLfoFreqMul_ = octaves(t[4]);
		effLfoAmp_ = std::clamp(skyLfoAmp_ + 0.3f * t[5], 0.0f, 1.0f);
	}
	else if (vastChainMode_) {
		// VAST — the wobble on its long lines, where its density builds, and its LFO; loop gain only down.
		charWobbleMul_ = octaves(t[0]);
		effBalance_ = std::clamp(skyFbBalance_ + 0.35f * t[1], 0.0f, 1.0f);
		charLfoFreqMul_ = octaves(t[2]);
		effLfoAmp_ = std::clamp(skyLfoAmp_ + 0.3f * t[3], 0.0f, 1.0f);
		charLoopScale_ = 1.0f - 0.25f * std::fabs(t[4]);
	}
	else if (skyChainMode_) {
		// SKY — the smeared feedback's balance (early lines against late), its LFO, its loop (only down).
		effBalance_ = std::clamp(skyFbBalance_ + 0.35f * t[0], 0.0f, 1.0f);
		charLfoFreqMul_ = octaves(t[1]);
		effLfoAmp_ = std::clamp(skyLfoAmp_ + 0.3f * t[2], 0.0f, 1.0f);
		charLoopScale_ = 1.0f - 0.25f * std::fabs(t[3]);
		charWobbleMul_ = octaves(t[4]);
	}
	else if (featherMode_) {
		// FEATHER — where its stereo cascade sits in the field, how wide, how diffuse.
		charRotAdd_ = 0.35f * t[0];
		effSideGain_ = cascade_side_gain_ * octaves(t[1]);
		for (size_t i = 0; i < kNumCascade; ++i) {
			effCoeffs_[i] = std::clamp(cascadeCoeffs_[i] + 0.08f * t[2], 0.2f, 0.9f);
		}
	}
	else {
		// THE ROOMS — diffusion (echoey to smooth), density (the cascade's parallel/series blend), and how
		// much of the room is early reflection.
		for (size_t i = 0; i < kNumCascade; ++i) {
			effCoeffs_[i] = std::clamp(cascadeCoeffs_[i] + 0.12f * t[0], 0.2f, 0.9f);
		}
		effSeriesMix_ = std::clamp(cascadeSeriesMix_ + 0.3f * t[1], 0.0f, 1.0f);
		effEarlyGain_ = earlyMixGain_ * octaves(0.5f * t[2]);
	}
}

void Core::beginBuffer(size_t frames) noexcept {
	if (buffer_ == nullptr) {
		return;
	}
	refreshCharacter();
	// THE PHASER POCKET: the cascade's allpasses swept by their own LFO, C0 against C1 (whose difference is
	// the side) and C2 against C3, so the phasing is stereo. Once per buffer: the sweep is 0.1..0.8 Hz.
	if (phaserDepth_ > 0.0f) {
		phaserPhase_ += phaserRate_ * static_cast<double>(frames) / (kReferenceRate * ratio_);
		phaserPhase_ -= std::floor(phaserPhase_);
		const float s = static_cast<float>(std::sin(6.283185307179586 * phaserPhase_));
		const float c = static_cast<float>(std::cos(6.283185307179586 * phaserPhase_));
		const float d = 0.3f * phaserDepth_;
		effCoeffs_[0] = std::clamp(effCoeffs_[0] + d * s, 0.05f, 0.95f);
		effCoeffs_[1] = std::clamp(effCoeffs_[1] - d * s, 0.05f, 0.95f);
		effCoeffs_[2] = std::clamp(effCoeffs_[2] + d * c, 0.05f, 0.95f);
		effCoeffs_[3] = std::clamp(effCoeffs_[3] - d * c, 0.05f, 0.95f);
	}

	// Owl servo: ratio-based feedback limiting (every buffer)
	if (owlMode_) {
		float ratio = feedbackEnvelope_ / std::max(inputAccum_, 1e-6f);
		float knee_ratio = (0.1f + (owlZ3Norm_ * 0.1f)) * charKneeMul_ * owlProbe_.kneeMul; // PORT: Character's bloom
		constexpr float kBandwidth = 0.001f;
		float excess = std::max(0.0f, ratio - knee_ratio - kBandwidth);
		float deficit = std::max(0.0f, knee_ratio - kBandwidth - ratio);
		constexpr float kLimitStrength = 10.0f;
		constexpr float kBoostStrength = 10.0f;
		float denom = 1.0f + excess * kLimitStrength;
		float numer = 1.0f + deficit * kBoostStrength;
		float target_scale = (numer) / (denom);
		target_scale = std::clamp(target_scale, 0.001f, owlProbe_.maxScale);
		// THE TARGET'S CEILING (probe): whatever the knee asks for, never an output above `ceilDb`. The core's output
		// saturates at 1/32, which the device reads as 25.36 (+28 dBFS), so device 0 dBFS is 1/(32 * 25.36) here.
		bool overCeiling = false;
		if (owlProbe_.ceilDb < 99.0f) {
			constexpr float kCoreAt0dBFS = 1.0f / (32.0f * 25.3583f);
			const float ceiling = kCoreAt0dBFS * std::pow(10.0f, owlProbe_.ceilDb / 20.0f);
			if (feedbackEnvelope_ > 0.5f * ceiling) {
				target_scale = std::min(target_scale, std::min(owlFbEnvScale_, 1.0f)); // no boost near the ceiling
			}
			if (feedbackEnvelope_ > ceiling) {
				target_scale = std::min(target_scale, owlFbEnvScale_ * ceiling / feedbackEnvelope_);
				overCeiling = true;
			}
		}
		float servoStep = owlProbe_.servoDown > 0.0f && target_scale < owlFbEnvScale_ ? owlProbe_.servoDown : 0.003f;
		if (overCeiling && target_scale < owlFbEnvScale_) {
			servoStep = owlProbe_.guardStep / charServoMul_;
		}
		else if (owlProbe_.guardAt > 0.0f && ratio > knee_ratio * owlProbe_.guardAt && target_scale < owlFbEnvScale_) {
			servoStep = owlProbe_.guardStep / charServoMul_; // the guard's own rate, whatever Character does to the servo's
			++owlGuardFires_;
		}
		if (!owlProbe_.freezeServo) owlFbEnvScale_ += (target_scale - owlFbEnvScale_) * (servoStep * charServoMul_); // Very slow smoothing to avoid servo oscillations
	}

	// LFO for Sky/Vast/Owl (every buffer - striding causes tonal artifacts)
	if ((skyChainMode_ || vastChainMode_ || owlMode_) && !owlProbe_.freezeLfo) {
		const size_t num_steps = frames / 2;
		skyRandState_ = skyRandState_ * 1664525u + 1013904223u;
		float rand_step = (static_cast<float>(skyRandState_ >> 16) / 32768.0f - 1.0f);
		// PORT: `/ ratioF_` — a buffer is `ratio` times more steps at this rate, and the same time.
		float step_size = 0.001f * (skyLfoFreq_ * charLfoFreqMul_) * static_cast<float>(num_steps) / ratioF_;
		skyRandWalk_ += rand_step * step_size;
		skyRandWalk_ *= 0.97f;
		skyRandWalk_ = std::clamp(skyRandWalk_, -1.0f, 1.0f);
		float smooth_coeff = vastChainMode_ ? 0.012f * (skyLfoFreq_ * charLfoFreqMul_) : 0.025f * (skyLfoFreq_ * charLfoFreqMul_);
		float buffer_smooth = std::min(1.0f, smooth_coeff * static_cast<float>(num_steps) / ratioF_);
		skyRandWalkSmooth_ += buffer_smooth * (skyRandWalk_ - skyRandWalkSmooth_);
	}
	{
		// THE RAMP'S ENDS (probe `rampLfo`): last buffer's value to this one's, over this buffer's undersampled steps.
		lfoFrom_ = lfoTo_;
		lfoTo_ = skyRandWalkSmooth_;
		lfoPos_ = 0;
		lfoLen_ = static_cast<uint32_t>(std::max<size_t>(1, frames / 2));
	}
}

void Core::run(const int32_t* inputSamples, int32_t* outL, int32_t* outR, size_t frames, const int32_t* inject) noexcept {
	const std::span<const int32_t> input(inputSamples, frames);
	if (buffer_ == nullptr) {
		return;
	}

	constexpr float kInputScale = 1.0f / static_cast<float>(std::numeric_limits<int32_t>::max());
	constexpr float kOutputScale =
	    static_cast<float>(std::numeric_limits<int32_t>::max()) * 32.0f; // 2x boost vs original

	// Owl: lower HPF cutoff to let more bass sustain
	// PORT: every one-pole coefficient at this rate (`rc` is the identity at 44.1 kHz) — `hpAlpha`, `outLpCoeff`. Or the
	// input highpass (a trapezoidal one-pole) and the output lowpass in Hz, when the device sets them so (`setHpfHz`, LB).
	const float hp_alpha = hpAlpha();
	const float out_lp = lpA_ > 0.0f ? lpA_ : outLpCoeff();
	const bool hp_hz = hpG_ > 0.0f;
	const float damp = rc(dampCoeff_);
	const float cascade_damp = owlMode_ ? cornerScaled(rc(cascadeDamping_), charOwlLpK_) : rc(cascadeDamping_);
	// PORT: Owl's REGISTER and FOCUS move its in-loop feedback highpass (the firmware's 0.057 DC block).
	const float owl_dc_coeff = cornerScaled(owlDcCoeff_, charOwlHpK_);
	const float tail_feedback = feedback_ * feedback_; // Hoist: tail decays faster than early

	// Owl mode feedback constants (hoisted - don't change per sample)
	const float owl_fb_delta = feedback_ - 0.32f;
	const float owl_room_norm = owl_fb_delta * 8.333f;
	const float owl_fdn_fb = 0.32f + owl_fb_delta * 0.7f;
	const float owl_cascade_fb = 0.32f + owl_fb_delta * 2.0f;
	const float owl_global_fb_base = 0.9f + owl_room_norm * 0.4f;
	const float owl_global_fb_coeff = (owl_global_fb_base + cascadeNestFeedback_) * skyLoopFb_;

	// (The Owl servo's step is in `beginBuffer`; what follows reads its result.)
	// THE SERVO'S GAIN AS APPLIED (probe A: compressed above a knee; the servo's own state is untouched).
	float owlS = owlFbEnvScale_;
	if (owlProbe_.gainKnee > 0.0f && owlS > owlProbe_.gainKnee) {
		owlS = owlProbe_.gainKnee + (owlS - owlProbe_.gainKnee) / owlProbe_.gainRatio;
	}
	float owl_delay_scale = 1.0f;
	// Owl-specific buffer-rate calculations (skip entirely when not in Owl mode)
	float owl_eff_fb = 0.0f;
	float owl_global_fb = 0.0f;
	float owl_read_cache_scale = 0.5f;
	if (owlMode_) {
		owl_delay_scale = owlS * owlS; // Squared for faster choke
		owl_eff_fb = owl_fdn_fb * fdnFeedbackScale_ * 1.8f * owl_delay_scale;
		owl_global_fb = owl_global_fb_coeff * owlS;
		owl_read_cache_scale = owlS * 0.5f;
	}

	// Sky/Vast shared feedback constants (hoisted - don't change per sample)
	float chain_global_fb_base = 0.0f;
	float chain_c2_fb_base = 0.0f;
	float chain_c3_fb_base = 0.0f;
	if (skyChainMode_ || vastChainMode_) {
		const float chain_loop_fb_base = feedback_ * 0.4f * delayRatio_ * skyLoopFb_;
		chain_global_fb_base = cascadeNestFeedback_ * 0.8f;
		chain_c2_fb_base = chain_loop_fb_base * charLoopScale_ * (1.6f - effBalance_ * 1.4f);
		chain_c3_fb_base = chain_loop_fb_base * charLoopScale_ * (0.2f + effBalance_ * 1.4f);
	}

	// Normal mode hoisted constant
	const float cascade_fb_mult = tail_feedback * cascadeFeedbackMult_;

	// LFO for Sky/Vast/Owl (every buffer - striding causes tonal artifacts)
	float lfo_tri_cached = skyRandWalkSmooth_;
	float d0_mod_cached = 0.0f;
	float d1_mod_cached = 0.0f;
	if (skyChainMode_ || vastChainMode_ || owlMode_) {
		// (The random walk's step is in `beginBuffer`; this is its result.)
		lfo_tri_cached = skyRandWalkSmooth_;
		// Compute modulation offsets as floats for interpolation (Sky/Vast only)
		if (!owlMode_) {
			float pitch_scale = 1.0f - skyLfoRouting_;
			// PORT: `* ratioF_` — the wobble is a length in samples.
			d0_mod_cached = std::max(0.0f, lfo_tri_cached * (modDepth_ * charWobbleMul_) * pitch_scale * ratioF_);
			d1_mod_cached = std::max(0.0f, -lfo_tri_cached * (modDepth_ * charWobbleMul_) * pitch_scale * ratioF_);
		}
	}

	// Flag for FDN pitch interpolation (only Sky/Vast use it - Owl and normal don't)
	// PORT: Character's flanger pocket also reads the early lines between samples (off at Character 0).
	const bool flanging = flangerDepth_ > 0.0f;
	const bool needs_pitch_interp = skyChainMode_ || vastChainMode_ || flanging;
	// The flanger's sweep, per FDN tick (a per-buffer step would zipper the delay time).
	const double flangeStep = flangerRate_ / (kReferenceRate * ratio_ * 0.5);
	const float flangeSpan = flangerDepth_ * 0.003f * static_cast<float>(kReferenceRate) * ratioF_;

	// Owl filter coefficients (buffer-rate - depend on cached LFO)
	float owl_mono_coeff = kCascadeLpCoeffMono;
	float owl_side_coeff = kCascadeLpCoeffSide;
	float owl_amp_mod_l = 1.0f;
	float owl_amp_mod_r = 1.0f;
	if (owlMode_) {
		float filter_mod = lfo_tri_cached * effLfoAmp_ * skyLfoRouting_ * 0.5f;
		owl_mono_coeff = std::clamp(kOwlLpCoeffMono - filter_mod, 0.2f, 0.85f);
		owl_side_coeff = std::clamp(kOwlLpCoeffSide + filter_mod * 1.5f, 0.4f, 0.99f);
	}
	// Hoisted values to eliminate in-loop ternaries
	const float owl_c2_scale = owlMode_ ? owlS : 1.0f;
	float cascade_lp_coeff_mono = owlMode_ ? cornerScaled(rc(owl_mono_coeff), charOwlLpK_) : rc(kCascadeLpCoeffMono); // PORT: rc, and Owl's register/focus
	float cascade_lp_coeff_side = owlMode_ ? cornerScaled(rc(owl_side_coeff), charOwlLpK_) : rc(kCascadeLpCoeffSide); // PORT: rc, and Owl's register/focus
	const float cascade_amp_mod_val = owlMode_ ? (effLfoAmp_ * skyLfoRouting_) : cascadeAmpMod_;
	// (Owl's `input_accum_decay` is computed in `endBuffer`, from the whole buffer's length.)

	// Sky/Vast feedback envelope scaling and amplitude modulation (buffer-rate)
	float chain_global_fb = 0.0f;
	float chain_c2_fb = 0.0f;
	float chain_c3_fb = 0.0f;
	float chain_lfo_out = 0.0f;
	float chain_amp_mod_l = 1.0f;
	float chain_amp_mod_r = 1.0f;
	if (skyChainMode_ || vastChainMode_) {
		const float chain_fb_env_scale = 1.0f - std::min(feedbackEnvelope_ * 3.0f, 1.0f);
		chain_global_fb = chain_global_fb_base * chain_fb_env_scale;
		chain_c2_fb = chain_c2_fb_base * chain_fb_env_scale;
		chain_c3_fb = chain_c3_fb_base * chain_fb_env_scale;
		chain_lfo_out = lfo_tri_cached * effLfoAmp_ * skyLfoRouting_;
		chain_amp_mod_l = 1.0f + chain_lfo_out * 0.3f;
		chain_amp_mod_r = 1.0f - chain_lfo_out * 0.3f;
	}

	// Stereo rotation coefficients (buffer-rate, small-angle approx)
	float rot_sin_a = charRotAdd_ == 0.0f ? lfo_tri_cached * 0.5f : std::clamp(lfo_tri_cached * 0.5f + charRotAdd_, -0.5f, 0.5f);
	float rot_cos_a = 1.0f - rot_sin_a * rot_sin_a * 0.5f;

	// Owl: buffer-rate modulation values (skip entirely when not in Owl mode)
	int32_t tap_mod_l = 0;
	int32_t tap_mod_r = 0;
	float owl_fb_lfo_mod = 1.0f;
	float owl_cascade_fb_mod = 1.0f;
	float owl_d2_read_mod = 1.0f;
	float owl_write_scale = 0.0f;
	float owl_h2_scale = 0.0f;
	if (owlMode_) {
		tap_mod_l = static_cast<int32_t>(lfo_tri_cached * tapMod_); // PORT: 280 samples at 44.1 kHz
		tap_mod_r = -tap_mod_l;
		const float abs_lfo_tri = std::abs(lfo_tri_cached);
		owl_fb_lfo_mod = 1.0f - lfo_tri_cached * 0.3f;
		owl_cascade_fb_mod = (1.3f + abs_lfo_tri * 0.3f) * owlS;
		owl_d2_read_mod = 1.0f - abs_lfo_tri * 0.3f;
		owl_write_scale = owl_eff_fb * owl_fb_lfo_mod;
		owl_h2_scale = owl_fdn_fb * 0.95f * owl_delay_scale;
	}

	// Cache matrix
	const auto& m = matrix_;

	for (size_t frame = 0; frame < input.size(); ++frame) {
		// === Full-rate: HPF, envelope, predelay ===
		float in = static_cast<float>(input[frame]) * kInputScale;
		float hp_out;
		if (hp_hz) {
			hp_out = in - tptStep(in, hpState_, hpG_);
		}
		else {
			hp_out = in - hpState_;
			hpState_ += hp_alpha * hp_out; // PORT: `(1.0f - hp_coeff)` at this rate
		}

		// Save input for dry subtraction BEFORE gain boost
		float in_orig = hp_out;

		// +3dB input gain when dry subtraction enabled (linked to DRY- toggle)
		in = dryMinus_ ? hp_out * 1.414f : hp_out;

		// Predelay (single tap)
		if (predelayLength_ > 0) {
			writePredelay(in);
			in = readPredelay(predelayLength_);
		}

		// PORT: FEATHERVERB'S EXCITER — the convolved room, into the loop's own input (backlog JD). Past the HPF and
		// the predelay: the room brings its own low end and its own predelay (the conv's).
		if (inject != nullptr) {
			in += static_cast<float>(inject[frame]) * kInputScale;
		}

		float out_l, out_r;

		// === 2x Undersampling ===
		accumIn_ += in;

		if (undersamplePhase_) {
			float fdn_in = accumIn_ * 0.5f;
			accumIn_ = 0.0f;

			// Peak hold with reset after sustained silence
			// Scale by sqrt(2) to approximate peak-to-RMS ratio
			constexpr float kPeakToRmsScale = 1.414f;
			constexpr float kNoiseFloor = 1e-6f;
			// PORT: the threshold in ticks at this rate, and the counter THIS INSTANCE's (the reference's is a
			// function-local `static`, one for every instance in the process).
			const uint16_t kSilenceThreshold = silenceThreshold_;
			uint16_t& silenceCount = silenceCount_;

			float abs_fdn_in = std::abs(fdn_in) * kPeakToRmsScale;
			if (abs_fdn_in > kNoiseFloor) {
				// Above noise floor - reset silence counter
				silenceCount = 0;
				if (inputPeakReset_) {
					// Dirty flag set - allow new peak even if lower
					// But require significant signal, not just noise (1e-4 ~= -40dB)
					if (abs_fdn_in > 1e-4f) {
						inputAccum_ = abs_fdn_in;
						inputPeakReset_ = false;
					}
				}
				else if (abs_fdn_in > inputAccum_) {
					// New peak - track with smoothing
					inputAccum_ += (abs_fdn_in - inputAccum_) * peakCoeff_; // PORT: 0.1 at 44.1 kHz
				}
				// else: below peak, hold (decay applied below)
			}
			else {
				// Below noise floor - count consecutive samples
				if (silenceCount < kSilenceThreshold) {
					silenceCount++;
				}
				else {
					// Sustained silence - arm reset for next note
					inputPeakReset_ = true;
				}
			}
			// Owl: inputAccum_ decay hoisted to buffer-rate (applied after loop)

			// LFO values cached at buffer rate for Sky/Vast/Owl (random walk evolves slowly)
			float lfo_tri = lfo_tri_cached;
			float d0Mod = d0_mod_cached;
			float d1Mod = d1_mod_cached;
			if ((smoothMod_ || owlProbe_.rampLfo) && (skyChainMode_ || vastChainMode_ || owlMode_)) {
				// THE WALK RAMPED across the buffer (`setSmoothModulation`), and everything derived from it with it.
				const float at = std::min(1.0f, static_cast<float>(++lfoPos_) / static_cast<float>(lfoLen_));
				lfo_tri = lfoFrom_ + (lfoTo_ - lfoFrom_) * at;
				if (!owlMode_) {
					const float k = (modDepth_ * charWobbleMul_) * (1.0f - skyLfoRouting_) * ratioF_;
					d0Mod = std::max(0.0f, lfo_tri * k);
					d1Mod = std::max(0.0f, -lfo_tri * k);
				}
				// The stereo rotation, from the ramped walk (the prologue's formula).
				rot_sin_a = charRotAdd_ == 0.0f ? lfo_tri * 0.5f : std::clamp(lfo_tri * 0.5f + charRotAdd_, -0.5f, 0.5f);
				rot_cos_a = 1.0f - rot_sin_a * rot_sin_a * 0.5f;
				if (skyChainMode_ || vastChainMode_) {
					// Sky/Vast's amplitude wobble, from the ramped walk (the prologue's formula).
					chain_lfo_out = lfo_tri * effLfoAmp_ * skyLfoRouting_;
					chain_amp_mod_l = 1.0f + chain_lfo_out * 0.3f;
					chain_amp_mod_r = 1.0f - chain_lfo_out * 0.3f;
				}
				if (owlMode_) {
					// Owl's taps and gains, from the ramped walk: the taps still whole samples, but a sample at a time.
					tap_mod_l = static_cast<int32_t>(lfo_tri * tapMod_);
					tap_mod_r = -tap_mod_l;
					const float absLfo = std::abs(lfo_tri);
					owl_fb_lfo_mod = 1.0f - lfo_tri * 0.3f;
					owl_cascade_fb_mod = (1.3f + absLfo * 0.3f) * owlS;
					owl_d2_read_mod = 1.0f - absLfo * 0.3f;
					owl_write_scale = owl_eff_fb * owl_fb_lfo_mod;
					// Owl's cascade lowpasses, swept by the walk (the prologue's formula).
					const float fm = lfo_tri * effLfoAmp_ * skyLfoRouting_ * 0.5f;
					cascade_lp_coeff_mono = cornerScaled(rc(std::clamp(kOwlLpCoeffMono - fm, 0.2f, 0.85f)), charOwlLpK_);
					cascade_lp_coeff_side = cornerScaled(rc(std::clamp(kOwlLpCoeffSide + fm * 1.5f, 0.4f, 0.99f)), charOwlLpK_);
				}
			}
			if (flanging) {
				// PORT: THE FLANGER POCKET — D0 and D1 swept in opposite phase, so the early reflections
				// flange across the stereo field. Up to 3 ms, with the wobble on top well inside D0's length.
				flangePhase_ += flangeStep;
				flangePhase_ -= std::floor(flangePhase_);
				const float s = static_cast<float>(std::sin(6.283185307179586 * flangePhase_));
				d0Mod += flangeSpan * (0.5f + 0.5f * s);
				d1Mod += flangeSpan * (0.5f - 0.5f * s);
				lastFlangeD0_ = flangeSpan * (0.5f + 0.5f * s);
			}

			// Read FDN delays - use interpolation only for Sky/Vast pitch mod (avoids overhead in normal/Owl)
			float d0, d1;
			if (needs_pitch_interp) {
				d0 = fdnReadAtInterp(0, d0Mod);
				d1 = fdnReadAtInterp(1, d1Mod);
			}
			else {
				d0 = fdnRead(0);
				d1 = fdnRead(1);
			}
			float d2 = fdnRead(2);
			// Owl: reduce D0/D1 gain, modulate D2 (buffer-rate values)
			if (owlMode_) {
				d0 *= 0.5f;
				d1 *= 0.5f;
				d2 *= owl_d2_read_mod; // 0.7 to 1.0, decorrelated from D0/D1
			}

			// 3x3 matrix multiply
			float h0 = m[0][0] * d0 + m[0][1] * d1 + m[0][2] * d2;
			float h1 = m[1][0] * d0 + m[1][1] * d1 + m[1][2] * d2;
			float h2 = m[2][0] * d0 + m[2][1] * d1 + m[2][2] * d2;

			// Cross-channel bleed: L↔R mixing for stereo complexity
			if (crossBleed_ > 0.0f) {
				float h0Orig = h0;
				h0 += h1 * crossBleed_;
				h1 += h0Orig * crossBleed_;
			}

			float effective_feedback = feedback_ * fdnFeedbackScale_;

			// Damping + feedback
			h0 = onepole(h0, fdnLpState_[0], damp) * effective_feedback * feedbackMult_[0];
			h1 = onepole(h1, fdnLpState_[1], damp) * effective_feedback * feedbackMult_[1];
			h2 = onepole(h2, fdnLpState_[2], damp) * effective_feedback * feedbackMult_[2];

			// DC blocking on FDN
			float dc_sum = (h0 + h1 + h2) * 0.333f;
			dcBlockState_ += dcCoeff_ * (dc_sum - dcBlockState_); // PORT: 0.007 at 44.1 kHz
			h0 -= dcBlockState_;
			h1 -= dcBlockState_;
			h2 -= dcBlockState_;

			// === Cascade: 4-stage with parallel/series blend ===
			// c0→c1→c2 always series, c3 input blends between cascade_in (parallel) and c2 (series)
			// seriesMix=0 → 9 paths (sparse), seriesMix=1 → 16 paths (dense)
			float cascade_in;
			if (kBypassFdnToCascade) {
				cascade_in = fdn_in * 1.4f + prevC3Out_ * cascadeNestFeedback_ * tail_feedback;
			}
			else {
				// 50% dry + 50% FDN to cascade
				cascade_in = fdn_in * 0.7f + (d0 + d1 + d2) * 0.7f + prevC3Out_ * cascadeNestFeedback_ * tail_feedback;
			}

			// c0→c1→c2 series chain with optional 4x undersample for vast rooms
			float c0, c1, c2;
			float cascade_out_l, cascade_out_r;
			float dynamic_width = width_ * widthBreath_ * effWidthMul_; // Z3-controlled width variation (PORT: and Character's)

			if (vastChainMode_) {
				// === VAST CHAIN MODE (Smeared feedback like Sky) ===
				// Topology with smeared feedback loops + global recirculation:
				//   Input → C0 → D0 → C1 → D1 → C2 → D2 → C3 → output
				//                 ↑         ↑         ↑
				//            (C2→C0)    (C3→C1)    (C3)
				// Feedback passes through cascade stages for diffusion before entering delays
				// This reduces comb filter sensitivity compared to direct feedback

				// Pre-decimation AA filter with global C3 feedback (hoisted coeffs)
				float chain_in =
				    onepole(fdn_in * 1.4f + prevC3Out_ * chain_global_fb, cascadeAaState1_, aaCoeff_);

				// C0 with 4x undersample (input → first allpass)
				c0Accum_ += chain_in;
				if (c0Phase_ == 1) {
					float avg_in = c0Accum_ * 0.5f;
					c0Prev_ = processCascadeStage(0, avg_in);
					// Smeared feedback: C2 → C0 → D0 (Z1 controls balance)
					float c2Smeared = processCascadeStage(0, c2Prev_ * chain_c2_fb);
					c0Prev_ += c2Smeared;
					c0Accum_ = 0.0f;
				}
				c0Phase_ = (c0Phase_ + 1) & 1;
				c0 = c0Prev_;

				// D0 delay between C0 and C1 (modulated pitch with interpolation)
				fdnWrite(0, c0);
				fdnWrite(0, c0); // Double write for 4x undersample
				float d0Out = smoothMod_ ? fdnReadAtPairs(0, d0Mod) : fdnReadAtInterp(0, d0Mod);

				// C1 with 4x undersample
				c1Accum_ += d0Out;
				if (c1Phase_ == 1) {
					float avg_in = c1Accum_ * 0.5f;
					c1Prev_ = processCascadeStage(1, avg_in);
					// Smeared feedback: C3 → C1 → D1 (Z1 controls balance)
					float c3Smeared = processCascadeStage(1, c3Prev_ * chain_c3_fb);
					c1Prev_ += c3Smeared;
					c1Accum_ = 0.0f;
				}
				c1Phase_ = (c1Phase_ + 1) & 1;
				c1 = c1Prev_;

				// D1 delay between C1 and C2 (modulated pitch with interpolation)
				fdnWrite(1, c1);
				fdnWrite(1, c1);
				float d1Out = smoothMod_ ? fdnReadAtPairs(1, d1Mod) : fdnReadAtInterp(1, d1Mod);

				// C2 with 4x undersample + pitch modulation
				c2Accum_ += d1Out;
				if (c2Phase_ == 1) {
					float avg_in = c2Accum_ * 0.5f;
					float c2Coeff = effCoeffs_[2];
					size_t orig_write_pos = cascadeWritePos_[2];
					size_t read_pos = orig_write_pos;
					size_t idx = cascadeOffsets_[2] + read_pos;
					float delayed = buffer_[idx];
					float output = -c2Coeff * avg_in + delayed;
					float write_val = avg_in + c2Coeff * output;
					buffer_[cascadeOffsets_[2] + orig_write_pos] = write_val;
					if (++cascadeWritePos_[2] >= cascadeLengths_[2])
						cascadeWritePos_[2] = 0;
					buffer_[cascadeOffsets_[2] + cascadeWritePos_[2]] = write_val;
					if (++cascadeWritePos_[2] >= cascadeLengths_[2])
						cascadeWritePos_[2] = 0;
					if (cascadeDoubleUndersample_) {
						size_t tap_pos = (orig_write_pos + multiTap_[2]) % cascadeLengths_[2];
						buffer_[cascadeOffsets_[2] + tap_pos] += write_val * kMultiTapGain;
					}
					c2Prev_ = output;
					c2Accum_ = 0.0f;
				}
				c2Phase_ = (c2Phase_ + 1) & 1;
				c2 = c2Prev_;

				// D2 delay between C2 and C3 (no direct feedback - using smeared instead)
				fdnWrite(2, c2);
				fdnWrite(2, c2);
				float d2Out = fdnRead(2);

				// C3 with 4x undersample + inverted pitch modulation
				float c3;
				c3Accum_ += d2Out;
				if (c3Phase_ == 1) {
					float avg_in = c3Accum_ * 0.5f;
					float c3Coeff = effCoeffs_[3];
					size_t orig_write_pos = cascadeWritePos_[3];
					size_t read_pos = orig_write_pos;
					size_t idx = cascadeOffsets_[3] + read_pos;
					float delayed = buffer_[idx];
					float output = -c3Coeff * avg_in + delayed;
					float write_val = avg_in + c3Coeff * output;
					buffer_[cascadeOffsets_[3] + orig_write_pos] = write_val;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					buffer_[cascadeOffsets_[3] + cascadeWritePos_[3]] = write_val;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					if (cascadeDoubleUndersample_) {
						size_t tap_pos = (orig_write_pos + multiTap_[3]) % cascadeLengths_[3];
						buffer_[cascadeOffsets_[3] + tap_pos] += write_val * kMultiTapGain;
					}
					c3Prev_ = output;
					c3Accum_ = 0.0f;
				}
				// Cache LFO value when cascade stages update to avoid discontinuities
				if (c3Phase_ == 0) {
					vastLfoCache_ = lfo_tri; // Update cache when c3 finishes updating
				}
				c3Phase_ = (c3Phase_ + 1) & 1;
				c3 = c3Prev_;

				// Track RAW c3 before clipping for servo feedback detection
				prevC3Out_ = c3;

				// Soft clip C3 at ~-2dB below 0dBFS (0.05) with 0.2 ratio
				// Input 0.1 → output 0.06 (0dBFS), smooth transition to hard clip
				constexpr float kC3Limit = 0.05f;
				if (std::abs(c3) > kC3Limit) {
					++clipsForTest_; // PORT: a count for the tests, nothing else reads it
					c3 = std::copysign(kC3Limit + (std::abs(c3) - kC3Limit) * 0.2f, c3);
				}

				// Amplitude modulation for diffusion contour (buffer-rate LFO)
				c2 *= (1.0f + chain_lfo_out);
				c3 *= (1.0f - chain_lfo_out);

				// Mix chain outputs - stereo from well-matched early stages only
				// C0(773) and C1(997) are ~30% different - good for balanced stereo
				// C2(1231) and C3(4001) are 3x different - mono only to avoid L/R imbalance
				float cascade_mono = (c2 + c3) * 0.5f;
				float cascade_side = (c0 - c1) * effSideGain_ * dynamic_width;
				cascade_mono = onepole(cascade_mono, cascadeLpStateMono_, lpMonoCoeff_);
				cascade_side = onepole(cascade_side, cascadeLpStateSide_, lpSideCoeff_);
				cascade_mono = onepole(cascade_mono, cascadeLpState_, cascade_damp);
				cascade_out_l = cascade_mono + cascade_side;
				cascade_out_r = cascade_mono - cascade_side;
				// LFO output modulation: stereo movement (buffer-rate)
				cascade_out_l *= chain_amp_mod_l;
				cascade_out_r *= chain_amp_mod_r;

				// No early reflections in vast chain mode (FDN is repurposed)
				directEarlyL_ = 0.0f;
				directEarlyR_ = 0.0f;
			}
			else if (skyChainMode_) {
				// === SKY MODE (Smeared feedback through cascades at 2x) ===
				// Feedback goes through cascade stages for diffusion BEFORE entering delays.
				// This smears the feedback signal, reducing comb filter sensitivity.
				//
				//   Input → C0 → D0 → C1 → D1 → C2 → D2 → C3 → output
				//                 ↑         ↑
				//            (C2→C0)    (C3→C1)
				//
				// C3 output → smear through C1 → write to D1
				// C2 output → smear through C0 → write to D0

				// Use hoisted constants (all computed at buffer-rate)
				float chain_in = fdn_in * 1.4f + prevC3Out_ * chain_global_fb;

				// C0 at 2x rate
				c0 = processCascadeStage(0, chain_in);

				// D0 delay - feedback from C2 smeared through C0 (interpolated pitch mod)
				// Z1 controls balance: low Z1 = more C2→C0, high Z1 = more C3→C1
				float c2Smeared = processCascadeStage(0, c2Prev_ * chain_c2_fb);
				fdnWrite(0, c0 + c2Smeared);
				float d0Out = fdnReadAtInterp(0, d0Mod);

				// C1 at 2x rate
				c1 = processCascadeStage(1, d0Out);

				// D1 delay - feedback from C3 smeared through C1 (interpolated pitch mod)
				float c3Smeared = processCascadeStage(1, c3Prev_ * chain_c3_fb);
				fdnWrite(1, c1 + c3Smeared);
				float d1Out = fdnReadAtInterp(1, d1Mod);

				// C2 at 2x rate
				c2 = processCascadeStage(2, d1Out);

				// D2 delay
				fdnWrite(2, c2);
				float d2Out = fdnRead(2);

				// C3 final stage
				float c3 = processCascadeStage(3, d2Out);

				// Soft clip C3 at ~-2dB below 0dBFS (0.05) with 0.2 ratio
				constexpr float kC3Limit = 0.05f;
				if (std::abs(c3) > kC3Limit) {
					++clipsForTest_; // PORT: a count for the tests, nothing else reads it
					c3 = std::copysign(kC3Limit + (std::abs(c3) - kC3Limit) * 0.2f, c3);
				}

				// Store for next iteration's smeared feedback
				c2Prev_ = c2;
				c3Prev_ = c3;
				prevC3Out_ = c3;

				// Amplitude modulation for diffusion contour (buffer-rate LFO)
				c2 *= (1.0f + chain_lfo_out);
				c3 *= (1.0f - chain_lfo_out);

				// Ping-pong stereo: C0,C2→L, C1→R, C3→center
				float cascade_mono = c3;
				float cascade_side = (c0 + c2 * 0.7f - c1) * effSideGain_ * dynamic_width;
				cascade_mono = onepole(cascade_mono, cascadeLpState_, cascade_damp);
				cascade_out_l = cascade_mono + cascade_side;
				cascade_out_r = cascade_mono - cascade_side;
				// Stereo rotation following LFO (buffer-rate coefficients)
				float temp_l = cascade_out_l * rot_cos_a - cascade_out_r * rot_sin_a;
				cascade_out_r = cascade_out_l * rot_sin_a + cascade_out_r * rot_cos_a;
				cascade_out_l = temp_l;

				// No early reflections in nested mode (FDN is repurposed as inter-stage delays)
				directEarlyL_ = 0.0f;
				directEarlyR_ = 0.0f;
			}
			else if (featherMode_) {
				// === FEATHER/OWL MODE ===
				// Dual parallel cascades: (C0→C1) for L, (C2→C3) for R
				// Feather (zone 4): 2x undersample, Owl (zone 6): 4x undersample
				// FDN provides shared early reflections, cascades create asymmetric stereo tail
				// Z3 controls cascade recirculation for extended tails

				// Cascade feedback from previous output (Z3 controlled)
				float cascadeFb = prevC3Out_ * cascadeNestFeedback_ * 0.6f;
				float cascade_inWithFb = cascade_in + cascadeFb;

				float c_l1, c_r1;
				if (cascadeDoubleUndersample_) {
					// === OWL: 4x undersample on cascades with multi-tap density ===
					// Pre-decimation AA filter
					cascade_inWithFb = onepole(cascade_inWithFb, cascadeAaState1_, aaCoeff_);

					// Accumulate for L cascade (C0→C1)
					c0Accum_ += cascade_inWithFb;
					if (++c0Phase_ >= 2) {
						c0Phase_ = 0;
						float c_l0 = processCascadeStage(0, c0Accum_ * 0.5f);
						// Cross-channel multi-tap: L writes to R's buffer (C0 → C2)
						size_t prev_pos_0 =
						    (cascadeWritePos_[0] == 0) ? cascadeLengths_[0] - 1 : cascadeWritePos_[0] - 1;
						float write_val0 = buffer_[cascadeOffsets_[0] + prev_pos_0];
						size_t tap_off_0 = static_cast<size_t>(static_cast<int32_t>(multiTap_[0]) + tap_mod_l);
						size_t tap_pos0 = (prev_pos_0 + tap_off_0) % cascadeLengths_[2];
						buffer_[cascadeOffsets_[2] + tap_pos0] += write_val0 * kMultiTapGain;

						c0Accum_ = 0.0f;
						c1Accum_ += c_l0;
						if (++c1Phase_ >= 2) {
							c1Phase_ = 0;
							c_l1 = processCascadeStage(1, c1Accum_ * 0.5f);
							// Cross-channel multi-tap: L writes to R's buffer (C1 → C3)
							size_t prev_pos_1 =
							    (cascadeWritePos_[1] == 0) ? cascadeLengths_[1] - 1 : cascadeWritePos_[1] - 1;
							float write_val1 = buffer_[cascadeOffsets_[1] + prev_pos_1];
							size_t tap_off_1 =
							    static_cast<size_t>(static_cast<int32_t>(multiTap_[1]) + tap_mod_l);
							size_t tap_pos1 = (prev_pos_1 + tap_off_1) % cascadeLengths_[3];
							buffer_[cascadeOffsets_[3] + tap_pos1] += write_val1 * kMultiTapGain;

							c1Accum_ = 0.0f;
							c1Prev_ = c_l1;
						}
						else {
							c_l1 = c1Prev_;
						}
						c0Prev_ = c_l1;
					}
					else {
						c_l1 = c0Prev_;
					}

					// Accumulate for R cascade (C2→C3)
					float cascade_inR = cascade_inWithFb * 0.98f + (d0 - d1) * 0.02f;
					c2Accum_ += cascade_inR;
					if (++c2Phase_ >= 2) {
						c2Phase_ = 0;
						float c_r0 = processCascadeStage(2, c2Accum_ * 0.5f);
						// Cross-channel multi-tap: R writes to L's buffer (C2 → C0)
						size_t prev_pos_2 =
						    (cascadeWritePos_[2] == 0) ? cascadeLengths_[2] - 1 : cascadeWritePos_[2] - 1;
						float write_val2 = buffer_[cascadeOffsets_[2] + prev_pos_2];
						size_t tap_off_2 = static_cast<size_t>(static_cast<int32_t>(multiTap_[2]) + tap_mod_r);
						size_t tap_pos2 = (prev_pos_2 + tap_off_2) % cascadeLengths_[0];
						buffer_[cascadeOffsets_[0] + tap_pos2] += write_val2 * kMultiTapGain;

						c2Accum_ = 0.0f;
						c3Accum_ += c_r0;
						if (++c3Phase_ >= 2) {
							c3Phase_ = 0;
							c_r1 = processCascadeStage(3, c3Accum_ * 0.5f);
							// Cross-channel multi-tap: R writes to L's buffer (C3 → C1)
							size_t prev_pos_3 =
							    (cascadeWritePos_[3] == 0) ? cascadeLengths_[3] - 1 : cascadeWritePos_[3] - 1;
							float write_val3 = buffer_[cascadeOffsets_[3] + prev_pos_3];
							size_t tap_off_3 =
							    static_cast<size_t>(static_cast<int32_t>(multiTap_[3]) + tap_mod_r);
							size_t tap_pos3 = (prev_pos_3 + tap_off_3) % cascadeLengths_[1];
							buffer_[cascadeOffsets_[1] + tap_pos3] += write_val3 * kMultiTapGain;

							c3Accum_ = 0.0f;
							c3Prev_ = c_r1;
						}
						else {
							c_r1 = c3Prev_;
						}
						c2Prev_ = c_r1;
					}
					else {
						c_r1 = c2Prev_;
					}
				}
				else {
					// === FEATHER: Serial cascade with full stereo ===
					float c0 = processCascadeStage(0, cascade_inWithFb);
					float c1 = processCascadeStage(1, c0);
					float c2 = processCascadeStage(2, c1);
					float c3 = processCascadeStage(3, c2);

					// Ping-pong stereo: C0,C2→L, C1→R, C3→center
					float cascade_mono = c3;
					float cascade_side = (c0 + c2 * 0.7f - c1) * effSideGain_ * dynamic_width;
					cascade_mono = onepole(cascade_mono, cascadeLpState_, cascade_damp);
					c_l1 = cascade_mono + cascade_side;
					c_r1 = cascade_mono - cascade_side;
				}

				cascade_out_l = c_l1;
				cascade_out_r = c_r1;
				// Stereo rotation following LFO (buffer-rate coefficients)
				float temp_l = cascade_out_l * rot_cos_a - cascade_out_r * rot_sin_a;
				cascade_out_r = cascade_out_l * rot_sin_a + cascade_out_r * rot_cos_a;
				cascade_out_l = temp_l;
				prevC3Out_ = cascade_out_l + cascade_out_r;

				// Keep early reflections from FDN
				if (!kMuteEarly) {
					float early_mid = (d0 + d1) * effEarlyGain_;
					// Owl mode: narrow early (60%) for focused transients, late cascade provides width
					float early_width_scale = cascadeDoubleUndersample_ ? 0.6f : 1.0f;
					float early_side = (d0 - d1) * effEarlyGain_ * dynamic_width * early_width_scale;
					directEarlyL_ = (early_mid + early_side) * directEarlyGain_;
					directEarlyR_ = (early_mid - early_side) * directEarlyGain_;
				}
				else {
					directEarlyL_ = 0.0f;
					directEarlyR_ = 0.0f;
				}

				// Inject input into FDN (no cascade feedback for cleaner separation)
				h0 += fdn_in;

				// Write FDN (double write for undersampling)
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);
			}
			else if (cascadeDoubleUndersample_) {
				// === LUSH/OWL MODE ===
				// 4x undersample on cascade stages, but keeps FDN + cascade separate
				// Lush: 3-delay FDN for early reflections, cascade for tail
				// Owl: 2-delay FDN (D0/D1 Hadamard) + D2 as nested feedback path

				// Pre-decimation AA filter
				cascade_in = onepole(cascade_in, cascadeAaState1_, aaCoeff_);

				// Owl: feedback envelope controls all feedback paths
				// Owl mode: interleaved D0 → C0 → D1 → C1, all at 4x undersample
				// Input → D0 → C0 → D1 → C1 → C2 → C3 → Output
				//               ↑                        ↓
				//              D2 ←──────────────────────┘
				float fdnFb = feedback_;     // FDN feedback - scaled separately for Owl
				float cascadeFb = feedback_; // Cascade feedback - scaled up for Owl
				float c3ForFb = 0.0f;        // DC-blocked feedback signal for envelope tracking
				if (owlMode_) {
					// Use buffer-rate hoisted constants (envelope applied once per buffer)
					fdnFb = owl_fdn_fb;
					cascadeFb = owl_cascade_fb;

					// Accumulate D0/D1/D2 reads for 4x AA (envelope scale moved to 4x cache averaging)
					owlD0ReadAccum_ += d0;
					owlD1ReadAccum_ += d1;
					// Envelope tracking moved to buffer rate for performance

					// Cheap HPF on feedback to tame LF rumble (~100Hz at 11kHz effective rate)
					c3ForFb = prevC3Out_ - dcBlockState_;
					dcBlockState_ += owl_dc_coeff * c3ForFb; // PORT: 0.057 at 44.1 kHz, and Owl's register/focus
					float globalRet = c3ForFb * owl_global_fb;
					// OWL'S COMPRESSOR (what if we added an additional compressor on the servo / "I suppose I
					// meant on the audio" / "move the compressor to the start of the feedback loop with a threshold that is aware
					// of the current servo setting"): a peak compressor — fast attack, slow release — with the servo untouched.
					// THE DEVICE'S: at the loop's START (the cascade's input — the new note and the tail, before they go round),
					// 4:1 above `kOwlCompThreshold`, the threshold divided by servo^2 while the servo is above 1 (when the
					// servo is cranked way up, lower the threshold). A probe with `compAt` set overrides all four.
					const bool probing = owlProbe_.compAt > 0.0f;
					const bool compOn = owlCompOn_ || probing;
					float compAt = probing ? owlProbe_.compAt : kOwlCompThreshold;
					const float compRatio = probing ? owlProbe_.compRatio : kOwlCompRatio;
					const int compPlace = probing ? owlProbe_.compPlace : 1;
					const float servoPow = probing ? owlProbe_.compServoPow : kOwlCompServoPow;
					if (servoPow > 0.0f && owlFbEnvScale_ > 1.0f) {
						compAt /= std::pow(owlFbEnvScale_, servoPow); // the servo cranked: bite earlier
					}
					const float atkStep = probing && owlProbe_.compAttack > 0.0f ? owlProbe_.compAttack : owlCompAttack_;
					const float relStep = probing && owlProbe_.compRelease > 0.0f ? owlProbe_.compRelease : owlCompRelease_;
					const float gainStep = probing ? owlProbe_.compGainSmooth : 0.0f;
					const auto compress = [&](float x) {
						const float a = std::abs(x);
						owlCompEnv_ += (a > owlCompEnv_ ? atkStep : relStep) * (a - owlCompEnv_);
						const float g = owlCompEnv_ > compAt ? std::pow(compAt / owlCompEnv_, 1.0f - 1.0f / compRatio) : 1.0f;
						if (gainStep > 0.0f) {
							owlCompGain_ += gainStep * (g - owlCompGain_);
							return x * owlCompGain_;
						}
						return x * g;
					};
					if (compOn && compPlace == 0) {
						globalRet = compress(globalRet);
					}
					if (owlProbe_.loopSat > 0.0f) {
						globalRet = owlProbe_.loopSat * std::tanh(globalRet / owlProbe_.loopSat);
					}
					cascade_in = owlD0Cache_ + globalRet;
					if (compOn && compPlace == 1) {
						cascade_in = compress(cascade_in); // THE LOOP'S START: the new note and the tail, before they go round
					}

					// D0 write: input - envelope limited + LFO modulated (pre-combined scale)
					owlD0WriteAccum_ += fdn_in * owl_write_scale;
					// D1 write will be set from C0 output below
					// h2 = C3 for nested feedback (accumulated and written at 4x rate)
				}
				// The reference assigns these two and never reads them; kept, and said so to the compiler.
				(void)fdnFb;
				(void)cascadeFb;

				// C0 with 4x undersample - fed by D0
				c0Accum_ += cascade_in;
				if (c0Phase_ == 1) {
					float avg_in = c0Accum_ * 0.5f;
					c0Prev_ = processCascadeStage(0, avg_in);
					// Double write for 4x undersample
					if (cascadeWritePos_[0] == 0) {
						cascadeWritePos_[0] = cascadeLengths_[0] - 1;
					}
					else {
						--cascadeWritePos_[0];
					}
					processCascadeStage(0, avg_in);
					c0Accum_ = 0.0f;
				}
				c0Phase_ = (c0Phase_ + 1) & 1;
				c0 = c0Prev_;

				// Owl: C0 → D1 → C1 (D1 sits between C0 and C1)
				if (owlMode_) {
					// Accumulate C0 output for D1 write (pre-combined scale)
					owlD1WriteAccum_ += c0 * owl_write_scale;
				}

				// C1 with 4x undersample - fed by D1 (Owl) or C0 (Lush)
				float c1Input = owlMode_ ? owlD1Cache_ : c0;
				c1Accum_ += c1Input;
				if (c1Phase_ == 1) {
					float avg_in = c1Accum_ * 0.5f;
					c1Prev_ = processCascadeStage(1, avg_in);
					if (cascadeWritePos_[1] == 0) {
						cascadeWritePos_[1] = cascadeLengths_[1] - 1;
					}
					else {
						--cascadeWritePos_[1];
					}
					processCascadeStage(1, avg_in);
					c1Accum_ = 0.0f;
				}
				c1Phase_ = (c1Phase_ + 1) & 1;
				c1 = c1Prev_;

				// C2 with 4x undersample + pitch modulation
				c2Accum_ += c1 * owl_c2_scale;
				if (c2Phase_ == 1) {
					float avg_in = c2Accum_ * 0.5f;
					float c2Coeff = effCoeffs_[2];
					size_t orig_write_pos = cascadeWritePos_[2];
					// Pitch mod with subtraction wrap (no modulo - much cheaper)
					size_t c2_mod_offset = static_cast<size_t>(std::max(0.0f, lfo_tri * cascadeModDepth_ * ratioF_));
					size_t read_pos = orig_write_pos + c2_mod_offset;
					if (read_pos >= cascadeLengths_[2])
						read_pos -= cascadeLengths_[2];
					float delayed = buffer_[cascadeOffsets_[2] + read_pos];
					if (owlProbe_.fracCascade) {
						delayed = cascadeReadFrac(2, orig_write_pos, std::max(0.0f, lfo_tri * cascadeModDepth_ * ratioF_));
					}
					float output = -c2Coeff * avg_in + delayed;
					float write_val = avg_in + c2Coeff * output;
					buffer_[cascadeOffsets_[2] + orig_write_pos] = write_val;
					if (++cascadeWritePos_[2] >= cascadeLengths_[2])
						cascadeWritePos_[2] = 0;
					buffer_[cascadeOffsets_[2] + cascadeWritePos_[2]] = write_val;
					if (++cascadeWritePos_[2] >= cascadeLengths_[2])
						cascadeWritePos_[2] = 0;
					// Multi-tap write with subtraction wrap (cheaper than modulo)
					if (cascadeDoubleUndersample_) {
						size_t tap_pos = orig_write_pos + multiTap_[2];
						if (tap_pos >= cascadeLengths_[2])
							tap_pos -= cascadeLengths_[2];
						buffer_[cascadeOffsets_[2] + tap_pos] += write_val * kMultiTapGain;
					}
					c2Prev_ = output;
					c2Accum_ = 0.0f;
				}
				c2Phase_ = (c2Phase_ + 1) & 1;
				c2 = c2Prev_;

				// C3 with 4x undersample + inverted pitch modulation + parallel/series blend
				// Owl: D2 inline between C2→C3 (Vast-like topology)
				float c3In = owlMode_ ? owlD2Cache_ : (cascade_in + (c2 - cascade_in) * effSeriesMix_);
				float c3;
				c3Accum_ += c3In;
				if (c3Phase_ == 1) {
					float avg_in = c3Accum_ * 0.5f;
					float c3Coeff = effCoeffs_[3];
					size_t orig_write_pos = cascadeWritePos_[3];
					size_t c3_mod_offset = static_cast<size_t>(std::max(0.0f, -lfo_tri * cascadeModDepth_ * ratioF_));
					size_t read_pos = (orig_write_pos + c3_mod_offset) % cascadeLengths_[3];
					size_t idx = cascadeOffsets_[3] + read_pos;
					float delayed = buffer_[idx];
					if (owlProbe_.fracCascade) {
						delayed = cascadeReadFrac(3, orig_write_pos, std::max(0.0f, -lfo_tri * cascadeModDepth_ * ratioF_));
					}
					float output = -c3Coeff * avg_in + delayed;
					float write_val = avg_in + c3Coeff * output;
					buffer_[cascadeOffsets_[3] + orig_write_pos] = write_val;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					buffer_[cascadeOffsets_[3] + cascadeWritePos_[3]] = write_val;
					if (++cascadeWritePos_[3] >= cascadeLengths_[3])
						cascadeWritePos_[3] = 0;
					if (cascadeDoubleUndersample_) {
						size_t tap_pos = (orig_write_pos + multiTap_[3]) % cascadeLengths_[3];
						buffer_[cascadeOffsets_[3] + tap_pos] += write_val * kMultiTapGain;
					}
					c3Prev_ = output;
					c3Accum_ = 0.0f;
				}
				// Cache LFO value when cascade stages update to avoid discontinuities
				if (c3Phase_ == 0 && owlMode_) {
					vastLfoCache_ = lfo_tri; // Update cache when c3 finishes updating
				}
				c3Phase_ = (c3Phase_ + 1) & 1;
				c3 = c3Prev_;
				prevC3Out_ = c3;

				// Amplitude modulation for diffusion contour (hoisted cascade_amp_mod_val)
				if (cascade_amp_mod_val != 0.0f) {
					// Owl: use cached LFO (synced to 4x update rate like Vast)
					float lfo_for_amp_mod = owlMode_ ? vastLfoCache_ : lfo_tri;
					c2 *= (1.0f + lfo_for_amp_mod * cascade_amp_mod_val);
					c3 *= (1.0f - lfo_for_amp_mod * cascade_amp_mod_val);
				}

				// Mix cascade outputs - stereo from well-matched early stages only
				// C0(773) and C1(997) are ~30% different - good for balanced stereo
				// C2(1231) and C3(4001) are 3x different - mono only to avoid L/R imbalance
				float cascade_mono = (c2 + c3) * 0.5f;
				float cascade_side = (c0 - c1) * effSideGain_ * dynamic_width;

				// Owl: modulate mid/side filter cutoffs for stereo movement (buffer-rate coeffs)
				cascade_mono = onepole(cascade_mono, cascadeLpStateMono_, cascade_lp_coeff_mono);
				cascade_side = onepole(cascade_side, cascadeLpStateSide_, cascade_lp_coeff_side);
				cascade_mono = onepole(cascade_mono, cascadeLpState_, cascade_damp);
				cascade_out_l = cascade_mono + cascade_side;
				cascade_out_r = cascade_mono - cascade_side;

				// Owl: LFO output modulation for stereo movement (buffer-rate)
				if (owlMode_) {
					cascade_out_l *= owl_amp_mod_l;
					cascade_out_r *= owl_amp_mod_r;

					// Stereo crossfeed to even out channels over time
					float cross_l = cascade_out_l + crossBleed_ * cascade_out_r;
					float cross_r = cascade_out_r + crossBleed_ * cascade_out_l;
					cascade_out_l = cross_l;
					cascade_out_r = cross_r;

					// D2 echo tap - distinct repeat before C3 diffuses it
					float echo_tap = owlD2Cache_ * effEcho_;
					cascade_out_l += echo_tap;
					cascade_out_r += echo_tap;
				}

				// Inject input + cascade feedback into FDN
				if (owlMode_) {
					// Owl: All FDN delays at 4x undersample with proper AA
					// D0/D1/D2 reads accumulated earlier, writes accumulated here
					// D2 read accumulated, envelope scale applied at 4x cache averaging
					owlD2ReadAccum_ += d2;
					// Use squared scale so delays choke faster than cascade
					// fdnFb is less sensitive to Room than cascade feedback
					// C2→D2→C3 envelope limited (pre-combined scale)
					owlD2WriteAccum_ += c2 * owl_h2_scale;
					// AA filter on input before FDN (prevents aliasing at 4x rate)
					float fdn_inAA = onepole(fdn_in, owlInputAaState_, aaCoeff_);
					owlD0WriteAccum_ += fdn_inAA;
					if (c3Phase_ == 0) {
						// c3 just updated - average + envelope scale all accumulated reads/writes
						owlD0Cache_ = owlD0ReadAccum_ * owl_read_cache_scale;
						owlD1Cache_ = owlD1ReadAccum_ * owl_read_cache_scale;
						owlD2Cache_ = owlD2ReadAccum_ * owl_read_cache_scale;
						owlD0WriteVal_ = owlD0WriteAccum_ * 0.5f;
						owlD1WriteVal_ = owlD1WriteAccum_ * 0.5f;
						owlD2WriteVal_ = owlD2WriteAccum_ * 0.5f;
						owlD0ReadAccum_ = 0.0f;
						owlD1ReadAccum_ = 0.0f;
						owlD2ReadAccum_ = 0.0f;
						owlD0WriteAccum_ = 0.0f;
						owlD1WriteAccum_ = 0.0f;
						owlD2WriteAccum_ = 0.0f;
					}
					h0 = owlD0WriteVal_;
					h1 = owlD1WriteVal_;
					h2 = owlD2WriteVal_;
				}
				else if (kMuteCascadeFeedback) {
					h0 += fdn_in;
				}
				else {
					h0 += fdn_in + cascade_mono * cascade_fb_mult * owl_cascade_fb_mod;
				}

				// Write FDN (double write for undersampling)
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);

				// Early reflections from FDN
				if (!kMuteEarly) {
					// Owl: use cached D0/D1 (4x rate) for consistency
					float early_d0 = owlMode_ ? owlD0Cache_ : d0;
					float early_d1 = owlMode_ ? owlD1Cache_ : d1;
					float early_mid = (early_d0 + early_d1) * effEarlyGain_;
					// Owl: wider early reflections (2-delay FDN is more stereo)
					float early_width_mult = owlMode_ ? 1.3f : 1.0f;
					float early_side = (early_d0 - early_d1) * effEarlyGain_ * dynamic_width * early_width_mult;
					directEarlyL_ = (early_mid + early_side) * directEarlyGain_;
					directEarlyR_ = (early_mid - early_side) * directEarlyGain_;
				}
				else {
					directEarlyL_ = 0.0f;
					directEarlyR_ = 0.0f;
				}
			}
			else {
				// === NORMAL FDN + CASCADE MODE ===
				c0 = processCascadeStage(0, cascade_in);
				c1 = processCascadeStage(1, c0);
				c2 = processCascadeStage(2, c1);

				// C3 input: blend parallel (cascade_in) ↔ series (c2)
				float c3In = cascade_in + (c2 - cascade_in) * cascadeSeriesMix_;
				float c3 = processCascadeStage(3, c3In);
				prevC3Out_ = c3;

				// Mix outputs - stereo tail from cascade (mid/side)
				float cascade_mono = (c2 + c3) * 0.5f;
				float cascade_side = (c0 - c1) * effSideGain_ * dynamic_width;
				// Skip extra LPFs for normal mode (only used in Lush/Vast)
				cascade_mono = onepole(cascade_mono, cascadeLpState_, cascade_damp);
				cascade_out_l = cascade_mono + cascade_side;
				cascade_out_r = cascade_mono - cascade_side;

				// Inject input + cascade feedback into FDN
				if (kMuteCascadeFeedback) {
					h0 += fdn_in;
				}
				else {
					h0 += fdn_in + cascade_mono * cascade_fb_mult;
				}

				// Write FDN (double write for undersampling)
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);
				fdnWrite(0, h0);
				fdnWrite(1, h1);
				fdnWrite(2, h2);

				// Early reflections from FDN
				if (!kMuteEarly) {
					float early_mid = (d0 + d1) * effEarlyGain_;
					float early_side = (d0 - d1) * effEarlyGain_ * dynamic_width;
					directEarlyL_ = (early_mid + early_side) * directEarlyGain_;
					directEarlyR_ = (early_mid - early_side) * directEarlyGain_;
				}
				else {
					directEarlyL_ = 0.0f;
					directEarlyR_ = 0.0f;
				}
			}

			// Output: early (FDN) + late (cascade)
			// Nested modes (Sky/Vast) have no early (FDN repurposed), normal modes have early
			float early_l = 0.0f, early_r = 0.0f;
			if (!vastChainMode_ && !skyChainMode_ && !kMuteEarly) {
				float early_mid = (d0 + d1) * effEarlyGain_;
				float early_side = (d0 - d1) * effEarlyGain_ * dynamic_width;
				early_l = early_mid + early_side;
				early_r = early_mid - early_side;
			}

			float new_out_l, new_out_r;
			if (kMuteCascade) {
				new_out_l = early_l;
				new_out_r = early_r;
			}
			else {
				new_out_l = early_l + cascade_out_l * tailMixGain_;
				new_out_r = early_r + cascade_out_r * tailMixGain_;
			}

			// Global wet side boost from width knob (mid/side)
			// width=0: normal stereo, width=1: 2x side boost
			float wet_mid = (new_out_l + new_out_r) * 0.5f;
			float wet_side = (new_out_l - new_out_r) * 0.5f * ((1.0f + width_) * effWidthMul_); // PORT: Character's width
			new_out_l = wet_mid + wet_side;
			new_out_r = wet_mid - wet_side;

			prevOutL_ = currOutL_;
			prevOutR_ = currOutR_;
			currOutL_ = new_out_l;
			currOutR_ = new_out_r;

			out_l = currOutL_;
			out_r = currOutR_;
		}
		else {
			// Interpolate
			out_l = (prevOutL_ + currOutL_) * 0.5f;
			out_r = (prevOutR_ + currOutR_) * 0.5f;
		}

		undersamplePhase_ = !undersamplePhase_;

		// prevOutputMono_ = (out_l + out_r) * 0.5f * (1.0f + std::abs(in_orig) * predelay_ * 2.0f);
		prevOutputMono_ = (out_l + out_r) * 0.5f + (std::abs(in_orig) * envPredelay_ * 2.0f);
		if (owlMode_) {
			owlSumAbs_ += std::abs(prevOutputMono_);
			++owlSumCount_;
		}

		// Output LPF
		out_l = onepole(out_l, lpStateL_, out_lp);
		out_r = onepole(out_r, lpStateR_, out_lp);

		// Add direct early brightness tap (bypasses LPF for crisp transients)
		out_l += directEarlyL_;
		out_r += directEarlyR_;

		// Subtract dry input to remove bleedthrough (sparse topology compensation)
		// Toggle via predelay encoder button: DRY- = enabled, DRY+ = disabled
		// in_orig is pre-gain, so subtract at unity
		if (dryMinus_) {
			out_l -= in_orig;
			out_r -= in_orig;
		}

		// Clamp and output
		constexpr float kMaxFloat = 0.06f;
		// PORT: THE LEVEL EVENTS (`levelEventsForTest`), counted only where they happen: a compare against a value
		// already in a register, the increment on the rare side.
		levelEvents_.outClamp += (std::abs(out_l) > kMaxFloat ? 1u : 0u) + (std::abs(out_r) > kMaxFloat ? 1u : 0u);
		out_l = std::clamp(out_l, -kMaxFloat, kMaxFloat);
		out_r = std::clamp(out_r, -kMaxFloat, kMaxFloat);
		// PORT: SATURATING, as the hardware's ARM `vcvt` is. The reference's `static_cast` is undefined past
		// 2^31, and the clamp above (0.06 x 32 > 1) does not keep it inside.
		constexpr float kSatAt = 2147483648.0f / kOutputScale;
		levelEvents_.outSat += (std::abs(out_l) >= kSatAt ? 1u : 0u) + (std::abs(out_r) >= kSatAt ? 1u : 0u);
		int32_t out_lq31 = cvtSat(out_l * kOutputScale);
		int32_t out_rq31 = cvtSat(out_r * kOutputScale);

		// The reference's plain int32 add into the rendering buffer: it WRAPS, stated without the UB.
		const int32_t addL = smmulr(out_lq31, returnLevelL_);
		const int32_t addR = smmulr(out_rq31, returnLevelR_);
		levelEvents_.outWrap += (addWraps(outL[frame], addL) ? 1u : 0u) + (addWraps(outR[frame], addR) ? 1u : 0u);
		outL[frame] = wrapAdd(outL[frame], addL);
		outR[frame] = wrapAdd(outR[frame], addR);
	}

}

void Core::endBuffer(size_t frames) noexcept {
	if (buffer_ == nullptr) {
		return;
	}
	// Owl: aggregate decay for inputAccum_ (~12s at 22kHz, applied once per buffer)
	// Linear approx: (1-x)^n ≈ 1 - n*x for small x (avoids std::pow)
	// PORT: `/ ratioF_`, as the LFO's steps.
	const float input_accum_decay = 1.0f - (owlMode_ ? 5e-6f * static_cast<float>(frames) / ratioF_ : 0.0f);

	// Owl: apply aggregate inputAccum_ decay (hoisted from per-sample)
	if (owlMode_) {
		inputAccum_ *= input_accum_decay;
	}

	// Buffer-end envelope update: track amplitude (not squared)
	float out_amp = std::abs(prevOutputMono_);
	if (owlMode_ && owlProbe_.envelope == 1 && owlSumCount_ > 0) {
		out_amp = static_cast<float>(owlSumAbs_ / owlSumCount_);
	}
	owlSumAbs_ = 0.0;
	owlSumCount_ = 0;
	float release_coeff = 0.003f;
	if (owlMode_) {
		// Feedback envelope: track output level (every buffer)
		float attack_coeff = 0.008f + envPredelay_ * 0.019f;
		constexpr float kReleaseCoeff = 0.05f;
		float fb_coeff = (out_amp > feedbackEnvelope_) ? attack_coeff : kReleaseCoeff;
		if (!owlProbe_.freezeEnv) feedbackEnvelope_ += fb_coeff * (out_amp - feedbackEnvelope_);
	}
	else {
		// Non-Owl modes: original per-buffer tracking
		float attack_coeff = 0.02f;
		float env_coeff = (out_amp > feedbackEnvelope_) ? attack_coeff : release_coeff;
		if (!owlProbe_.freezeEnv) feedbackEnvelope_ += env_coeff * (out_amp - feedbackEnvelope_);
	}
}

void Core::setRoomSize(float value) noexcept {
	roomSize_ = value;
	feedback_ = 0.32f + value * 0.12f;
}

void Core::setDamping(float value) noexcept {
	damping_ = value;
	dampCoeff_ = 0.1f + (1.0f - value) * 0.85f;
	updateSizes();
}

void Core::setPredelay(float value) noexcept {
	predelay_ = value;
	// PORT: the scaled maximum. At 44.1 kHz it is `kPredelayMaxLength` itself.
	predelayLength_ = static_cast<size_t>(value * predelayMax_);
	// Update Owl mode attack rate from predelay
	if (owlMode_) {
		owlEnvRatio_ = 0.5f + predelay_ * 1.5f; // Range: 0.5 to 2.0
	}
}

// === Zone 1: Matrix morphing ===
// Simplified 3x3 matrix with phi triangle modulation

static constexpr std::array<phi::PhiTriConfig, 9> kMatrix3TriBank = {{
    {phi::kPhi025, 0.7f, 0.000f, true},
    {phi::kPhi050, 0.7f, 0.111f, true},
    {phi::kPhi075, 0.7f, 0.222f, true},
    {phi::kPhi100, 0.7f, 0.333f, true},
    {phi::kPhi125, 0.7f, 0.444f, true},
    {phi::kPhi150, 0.7f, 0.555f, true},
    {phi::kPhi175, 0.7f, 0.666f, true},
    {phi::kPhi200, 0.7f, 0.777f, true},
    {phi::kPhi033, 0.7f, 0.888f, true},
}};

void Core::setZone1(int32_t value) noexcept {
	zone1_ = value;
	updateMatrix();
}

// Simple Gram-Schmidt for 3x3
static bool gram_schmidt_3x3(std::array<std::array<float, 3>, 3>& m) {
	for (int col = 0; col < 3; ++col) {
		for (int prev = 0; prev < col; ++prev) {
			float dot = 0.0f;
			for (int row = 0; row < 3; ++row) {
				dot += m[row][col] * m[row][prev];
			}
			for (int row = 0; row < 3; ++row) {
				m[row][col] -= dot * m[row][prev];
			}
		}
		float norm = 0.0f;
		for (int row = 0; row < 3; ++row) {
			norm += m[row][col] * m[row][col];
		}
		norm = std::sqrt(norm);
		if (norm < 0.0001f)
			return false;
		for (int row = 0; row < 3; ++row) {
			m[row][col] /= norm;
		}
	}
	return true;
}

void Core::updateMatrix() noexcept {


	const float y_norm = static_cast<float>(zone1_) / 1023.0f;
	const int32_t zone = zone1_ >> 7;
	const double gamma_phase = zone * 0.125;

	// Vast mode: offset y_norm slightly to avoid phi triangle null at Z1 max
	float y_norm_adj = (zone == 7) ? std::min(y_norm, 0.97f) : y_norm;
	// PORT: the reference's float arithmetic, restated (`fwEvalBank`).
	std::array<float, 9> vals = fwEvalBank(y_norm_adj, gamma_phase, kMatrix3TriBank);

	// Base 3x3 Hadamard-like matrix
	static constexpr std::array<std::array<float, 3>, 3> kH3Base = {
	    {{1.0f, 1.0f, 1.0f}, {1.0f, -1.0f, 0.0f}, {1.0f, 0.0f, -1.0f}}};

	static constexpr std::array<float, 8> kZoneBlend = {0.0f, 0.15f, 0.3f, 0.45f, 0.6f, 0.75f, 0.85f, 0.95f};
	const float blend = kZoneBlend[zone];

	for (int row = 0; row < 3; ++row) {
		for (int col = 0; col < 3; ++col) {
			float base = kH3Base[row][col];
			float mod = vals[row * 3 + col];
			matrix_[row][col] = base + blend * mod * 0.5f;
		}
	}

	if (!gram_schmidt_3x3(matrix_)) {
		// Fallback
		for (int row = 0; row < 3; ++row) {
			for (int col = 0; col < 3; ++col) {
				matrix_[row][col] = kH3Base[row][col] * kH3Norm;
			}
		}
	}

	// Pitch wobble depth: Lush/Vast use blend, Sky uses dedicated control
	if (cascadeDoubleUndersample_) {
		modDepth_ = blend * 25.0f;
	}
	else {
		modDepth_ = 0.0f;
	}

	// Update D0/D1 lengths from phi triangles
	float d0_tri = (vals[0] + 1.0f) * 0.5f;
	float d1_tri = (vals[3] + 1.0f) * 0.5f;

	// PORT: the scaled lengths (the reference's at 44.1 kHz).
	fdnLengths_[0] = d0Min_ + static_cast<size_t>(d0_tri * (d0Max_ - d0Min_));
	fdnLengths_[1] = d1Min_ + static_cast<size_t>(d1_tri * (d1Max_ - d1Min_));

	// Precompute delay ratio for feedback normalization (avoid division in hot path)
	delayRatio_ = static_cast<float>(fdnLengths_[0] + fdnLengths_[1]) / static_cast<float>(d0Max_ + d1Max_);

	// Sky/Vast/Owl mode: Z1 controls various parameters using phi triangles
	if (skyChainMode_ || vastChainMode_ || owlMode_) {
		// Balance between C2→C0 and C3→C1 feedback paths
		float balance_base = y_norm;                          // 0 = favor C2→C0, 1 = favor C3→C1
		float balance_mod = (vals[1] + 1.0f) * 0.15f - 0.15f; // ±0.15 texture
		skyFbBalance_ = std::clamp(balance_base + balance_mod, 0.0f, 1.0f);

		// LFO amplitude and frequency from phi triangles
		float amp_tri = (vals[2] + 1.0f) * 0.5f;   // 0 to 1
		float freq_tri = (vals[4] + 1.0f) * 0.5f;  // 0 to 1
		float pitch_tri = (vals[5] + 1.0f) * 0.5f; // 0 to 1
		skyLfoAmp_ = 0.15f + amp_tri * 0.85f;      // 0.15 → 1.0
		skyLfoFreq_ = 0.5f + freq_tri * 2.0f;      // 0.5x → 2.5x
		// Owl: lower pitch modulation to avoid artifacts at 4x undersample
		float max_pitch = owlMode_ ? 80.0f : 160.0f;
		modDepth_ = 20.0f + pitch_tri * max_pitch;
	}

	if (fdnWritePos_[0] >= fdnLengths_[0])
		fdnWritePos_[0] = 0;
	if (fdnWritePos_[1] >= fdnLengths_[1])
		fdnWritePos_[1] = 0;
}

// === Zone 2: Size (D2 + cascade scaling) ===

void Core::setZone2(int32_t value) noexcept {
	zone2_ = value;
	updateSizes();
	updateFeedbackPattern(); // Recalc cascade coefficients (depend on cascadeDoubleUndersample_)
	// Resync z1-controlled params (skyFbBalance_, LFO, etc.) to avoid feedback imbalance
	if (skyChainMode_ || vastChainMode_ || owlMode_) {
		updateMatrix();
	}
}

void Core::updateSizes() noexcept {
	const float t = static_cast<float>(zone2_) / 1023.0f;
	const int32_t zone = zone2_ >> 7; // Zone ID 0-7

	// D2 scales from min to max
	fdnLengths_[2] = d2Min_ + static_cast<size_t>(t * (d2Max_ - d2Min_)); // PORT: scaled
	if (fdnWritePos_[2] >= fdnLengths_[2])
		fdnWritePos_[2] = 0;

	// Cascade scales uniformly from 1x to 1.5x
	cascadeScale_ = 1.0f + t * (kCascadeMaxScale - 1.0f);

	// Early/tail balance: inverse relationship for room character
	// Tiny rooms: punchy early reflections, minimal tail (0.4 early, 0.25 tail)
	// Vast rooms: spacious tails, subtle early (0.15 early, 1.3 tail)
	earlyMixGain_ = 0.4f - t * 0.25f;   // 0.4 → 0.15
	tailMixGain_ = 0.25f + t * 1.05f;   // 0.25 → 1.3
	directEarlyGain_ = 0.2f - t * 0.1f; // 0.2 → 0.1 (more direct brightness at small, less at vast)

	// Zone layout (compressed small rooms to make room for Feather):
	// Zones 0-3: Compressed small rooms (Smol, Chamber, Hall, Church)
	// Zone 4: Feather - experimental mode placeholder
	// Zone 5: Sky - nested topology at 2x undersample (responsive, extended)
	// Zone 6: Lush - FDN + Cascade with 4x undersample
	// Zone 7: Vast - nested topology at 4x undersample (maximum tail length)
	bool prevFeatherMode = featherMode_;
	bool prevOwlMode = owlMode_;
	bool prevSkyMode = skyChainMode_;
	bool prevVastMode = vastChainMode_;

	cascadeDoubleUndersample_ = !kDisableVastUndersample && (zone >= 6);
	featherMode_ = (zone == 4);   // Feather: dual parallel cascades at 2x
	skyChainMode_ = (zone == 5);  // Sky: nested at 2x
	owlMode_ = (zone == 6);       // Owl: nested with smeared feedback at 4x
	vastChainMode_ = (zone == 7); // Vast: nested at 4x

	// Reset shared filter state when mode changes to prevent stereo imbalance
	// cascadeLpStateMono_/Side_ are reused differently across modes:
	// - Feather: cascadeLpStateR_ = R channel filter (separate L/R)
	// - Owl/Vast: cascadeLpStateMono_/Side_ = M/S processing
	bool modeChanged = (featherMode_ != prevFeatherMode) || (owlMode_ != prevOwlMode) || (skyChainMode_ != prevSkyMode)
	                   || (vastChainMode_ != prevVastMode);
	if (modeChanged) {
		cascadeLpStateR_ = 0.0f;
		cascadeLpStateMono_ = 0.0f;
		cascadeLpStateSide_ = 0.0f;
		feedbackEnvelope_ = 0.0f;
		// Reset cascade phase counters to avoid partial updates
		c0Phase_ = 0;
		c1Phase_ = 0;
		c2Phase_ = 0;
		c3Phase_ = 0;
		c0Accum_ = 0.0f;
		c1Accum_ = 0.0f;
		c2Accum_ = 0.0f;
		c3Accum_ = 0.0f;
		// Initialize Owl mode envelope tracking from current parameters
		if (owlMode_) {
			// inputAccum_ persists - don't reset
			owlFbEnvScale_ = 1.0f;
			owlSilenceCount_ = 0;
			owlEnvRatio_ = 0.5f + predelay_ * 1.5f;
			owlZ3Norm_ = static_cast<float>(zone3_) / 1023.0f;
		}
	}

	// Mode-specific enhancements
	// Recompute cascade damping from base damping_ value to avoid compounding
	float baseCascadeDamping = 0.05f + (1.0f - damping_) * 0.6f;
	if (zone == 7) {
		// Vast: Nested topology with 4x undersample, self-limiting feedback
		cascadeDamping_ = baseCascadeDamping * 0.5f;
		cascadeModDepth_ = 14.0f;
		cascadeAmpMod_ = 0.25f;
		cascadeNestFeedback_ = std::clamp(cascadeNestFeedbackBase_ + 0.25f, 0.0f, 0.52f); // Slightly more fb headroom
		cascade_side_gain_ = 0.23f; // Slightly wider for spacious modes
	}
	else if (zone == 6) {
		// Owl: FDN + Cascade with 4x undersample, wider stereo tail
		cascadeDamping_ = baseCascadeDamping * 0.6f;
		cascadeModDepth_ = 0.0f; // Disabled - pitch mod causes clicks at 4x undersample
		cascadeAmpMod_ = 0.15f;
		cascadeNestFeedback_ = std::clamp(cascadeNestFeedbackBase_ + 0.25f, 0.0f, 0.55f);
		cascade_side_gain_ = 0.25f; // Wider stereo spread for Owl
	}
	else if (zone == 5) {
		// Sky: Nested topology at 2x undersample (faster response than Vast)
		cascadeDamping_ = baseCascadeDamping * 0.65f;
		cascadeModDepth_ = 8.0f;
		cascadeAmpMod_ = 0.12f;
		cascadeNestFeedback_ = std::clamp(cascadeNestFeedbackBase_ + 0.15f, 0.0f, 0.40f);
		cascade_side_gain_ = 0.23f; // Slightly wider for spacious modes
		modDepth_ = 100.0f;         // D0/D1 pitch wobble default (~4.5ms), Z1 modulates 20-180
	}
	else if (zone == 4) {
		// Feather: Experimental mode - start with normal FDN+cascade, tweak from here
		cascadeDamping_ = baseCascadeDamping * 0.7f;
		cascadeModDepth_ = 4.0f;
		cascadeAmpMod_ = 0.08f;
		cascadeNestFeedback_ = std::clamp(cascadeNestFeedbackBase_ + 0.1f, 0.0f, 0.35f);
		cascade_side_gain_ = 0.2f;
	}
	else {
		cascadeDamping_ = baseCascadeDamping;
		cascadeModDepth_ = 0.0f;
		cascadeAmpMod_ = 0.0f;
		cascadeNestFeedback_ = std::clamp(cascadeNestFeedbackBase_, 0.0f, 0.55f);
		cascade_side_gain_ = 0.2f; // Default stereo side gain
	}

	cascadeLengths_[0] = static_cast<size_t>(cascadeBase_[0] * cascadeScale_); // PORT: scaled
	cascadeLengths_[1] = static_cast<size_t>(cascadeBase_[1] * cascadeScale_); // PORT: scaled
	cascadeLengths_[2] = static_cast<size_t>(cascadeBase_[2] * cascadeScale_); // PORT: scaled
	cascadeLengths_[3] = static_cast<size_t>(cascadeBase_[3] * cascadeScale_); // PORT: scaled

	// Clamp write positions
	for (size_t i = 0; i < kNumCascade; ++i) {
		if (cascadeWritePos_[i] >= cascadeLengths_[i]) {
			cascadeWritePos_[i] = 0;
		}
	}
}

// === Zone 3: Feedback pattern ===

static constexpr std::array<phi::PhiTriConfig, 3> kFeedback3TriBank = {{
    {phi::kPhi033, 0.6f, 0.00f, true},
    {phi::kPhi067, 0.6f, 0.33f, true},
    {phi::kPhi100, 0.6f, 0.66f, true},
}};

void Core::setZone3(int32_t value) noexcept {
	zone3_ = value;
	updateFeedbackPattern();
	// Resync z1-controlled params (skyFbBalance_, LFO, etc.) to avoid feedback imbalance
	if (skyChainMode_ || vastChainMode_ || owlMode_) {
		updateMatrix();
	}
}

void Core::updateFeedbackPattern() noexcept {


	const float y_norm = static_cast<float>(zone3_) / 1023.0f;
	const int32_t zone = zone3_ >> 7;
	const double gamma_phase = zone * 0.125;

	// Widened bias for more tonal variation across z3 zones
	static constexpr std::array<std::array<float, 3>, 8> kZoneBias = {{{1.00f, 1.00f, 1.00f},
	                                                                   {1.25f, 0.90f, 0.75f},
	                                                                   {0.70f, 1.00f, 1.30f},
	                                                                   {1.20f, 0.70f, 1.15f},
	                                                                   {0.65f, 1.35f, 0.70f},
	                                                                   {1.30f, 0.75f, 1.00f},
	                                                                   {0.60f, 1.00f, 1.40f},
	                                                                   {1.35f, 0.85f, 0.60f}}};

	// PORT: the reference's float arithmetic, restated (`fwEvalBank`).
	std::array<float, 3> mods = fwEvalBank(y_norm, gamma_phase, kFeedback3TriBank);

	for (size_t i = 0; i < 3; ++i) {
		// Wide range (±45%) for dramatic tonal variation
		feedbackMult_[i] = std::clamp(kZoneBias[zone][i] + mods[i] * 0.2f, 0.55f, 1.45f);
	}

	// Cascade series mix - 10 periods for fine density control
	// seriesMix=0 → C3 parallel (9 paths, sparse), seriesMix=1 → C3 series (16 paths, dense)
	float tri10 = fwTriangleSimpleUnipolar(y_norm * 10.0f) * 2.0f - 1.0f; // Bipolar -1..1
	cascadeSeriesMix_ = 0.6f + tri10 * 0.25f;                                // Map to 0.35..0.85

	// Cascade feedback - 7 periods, clockwise = more feedback
	float tri7 = fwTriangleSimpleUnipolar(y_norm * 7.0f) * 2.0f - 1.0f;
	float base_feedback = 0.03f + y_norm * 0.12f;
	cascadeFeedbackMult_ = std::clamp(base_feedback + tri7 * 0.02f, 0.02f, 0.2f);

	// Nested cascade feedback (C3→C0) - 5 periods
	float tri5 = fwTriangleSimpleUnipolar(y_norm * 5.0f) * 2.0f - 1.0f;
	float base_nest = std::max(0.0f, (y_norm - 0.35f) * 0.5f);
	cascadeNestFeedbackBase_ = std::clamp(base_nest + tri5 * 0.08f, 0.0f, 0.4f);
	// Recompute combined value (Zone 2 adds vast boost)
	updateSizes();

	// LFO pitch wobble depth - 13 periods (fast), adds subtle chorus/shimmer
	if (skyChainMode_ || vastChainMode_ || owlMode_) {
		// Sky/Vast/Owl: Z3 controls pitch wobble (lower max at 4x to avoid artifacts)
		// Sky max capped at 180 to match z1's modDepth range and avoid aliasing
		float tri13 = fwTriangleSimpleUnipolar(y_norm * 13.0f) * 2.0f - 1.0f;
		float max_wobble = (vastChainMode_ || owlMode_) ? 80.0f : 140.0f;
		float wobble_texture = (vastChainMode_ || owlMode_) ? 15.0f : 40.0f;
		modDepth_ = std::max(0.0f, y_norm * max_wobble + tri13 * wobble_texture);

		// Z3 also controls local loop feedback (smeared path density)
		skyLoopFb_ = std::max(0.4f, 0.4f + y_norm * 0.9f + tri13 * 0.2f);

		// Z3 controls LFO routing - 7 periods for variety
		float tri7_route = fwTriangleSimpleUnipolar(y_norm * 7.0f) * 2.0f - 1.0f;
		skyLfoRouting_ = std::clamp(y_norm + tri7_route * 0.15f, 0.0f, 1.0f);

		// Owl mode: D2 echo tap - 10 periods, 50% duty cycle
		owlEchoGain_ = owlMode_ ? y_norm * 0.5f * fwTriangleSimpleUnipolar(y_norm * 10.0f, 0.5f) : 0.0f;

		// Owl mode: Z3 controls max boost for ratio-based feedback
		// Low Z3: no boost (unity only), High Z3: more boost allowed
		// Envelope attack rate controlled by predelay knob
		if (owlMode_) {
			owlZ3Norm_ = y_norm;                    // Store Z3 position for ratio control max boost
			owlEnvRatio_ = 0.5f + predelay_ * 1.5f; // Range: 0.5 to 2.0
		}
	}
	else if (cascadeDoubleUndersample_) {
		float tri13 = fwTriangleSimpleUnipolar(y_norm * 13.0f) * 2.0f - 1.0f;
		modDepth_ = std::clamp(0.3f + tri13 * 0.25f, 0.0f, 0.6f);
	}
	else {
		modDepth_ = 0.0f;
	}

	// Width breathing - 11 periods (fast), z3 increases stereo width
	float tri11 = fwTriangleSimpleUnipolar(y_norm * 11.0f) * 2.0f - 1.0f;
	widthBreath_ = std::clamp(0.6f + y_norm * 0.5f + tri11 * 0.35f, 0.3f, 1.4f);

	// Cross-channel bleed - 9 periods, L↔R mixing in FDN feedback
	float tri9 = fwTriangleSimpleUnipolar(y_norm * 9.0f) * 2.0f - 1.0f;
	float base_bleed = y_norm * 0.3f; // Increased for better L/R balance
	crossBleed_ = std::clamp(base_bleed + tri9 * 0.1f, 0.0f, 0.4f);

	// FDN feedback scale - keep at unity (room knob already controls overall feedback)
	// Z3 only affects tonal balance via feedbackMult_, not overall decay
	fdnFeedbackScale_ = 1.0f;

	// Per-stage cascade allpass coefficients - higher coeffs = more diffusion (less slapback)
	// Each stage uses different triangle period for variety
	// Extended modes (Feather/Sky/Lush/Vast) need higher coefficients for proper diffusion
	bool extended_mode = cascadeDoubleUndersample_ || skyChainMode_ || featherMode_;
	float coeff_base = extended_mode ? (0.5f + y_norm * 0.12f)  // Extended modes: 0.5-0.62
	                                 : (0.32f + y_norm * 0.2f); // Normal: 0.32-0.52

	// C0: 8 periods - fastest stage, moderate variation
	float tri8 = fwTriangleSimpleUnipolar(y_norm * 8.0f) * 2.0f - 1.0f;
	float c0_min = extended_mode ? 0.45f : 0.28f;
	cascadeCoeffs_[0] = std::clamp(coeff_base + tri8 * 0.08f, c0_min, 0.75f);

	// C1: 6 periods - slightly more variation
	float tri6 = fwTriangleSimpleUnipolar(y_norm * 6.0f) * 2.0f - 1.0f;
	float c1_min = extended_mode ? 0.45f : 0.28f;
	cascadeCoeffs_[1] = std::clamp(coeff_base + tri6 * 0.1f, c1_min, 0.75f);

	// C2: 4 periods - slower variation, slightly higher base for density
	float tri4 = fwTriangleSimpleUnipolar(y_norm * 4.0f) * 2.0f - 1.0f;
	float c2_min = extended_mode ? 0.48f : 0.30f;
	cascadeCoeffs_[2] = std::clamp(coeff_base + 0.05f + tri4 * 0.1f, c2_min, 0.78f);

	// C3: 3 periods - longest stage, highest base for maximum diffusion
	float tri3 = fwTriangleSimpleUnipolar(y_norm * 3.0f) * 2.0f - 1.0f;
	float c3_min = extended_mode ? 0.50f : 0.32f;
	cascadeCoeffs_[3] = std::clamp(coeff_base + 0.08f + tri3 * 0.12f, c3_min, 0.82f);
}

} // namespace feathers::fx::featherverb
