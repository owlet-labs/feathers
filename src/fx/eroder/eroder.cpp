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

#include "fx/eroder/eroder.h"

#include "character/phi_triangle.hpp"
#include "fx/ring_lerp.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace phi = owlet::dsp::phi;

namespace feathers::fx::eroder {

namespace {

//--------------------------------------------------------------------------------------------------//
// THE TWO BANKS — `eroder.cpp`'s `kEroderToneBank` and `kEroderCharBank`, VERBATIM.                  //
//--------------------------------------------------------------------------------------------------//
//
// {phi frequency, duty, phase offset, bipolar}. These twenty-two numbers ARE the device: they decide
// which of the eleven quantities are up together and which never are. `test_fx_eroder.cpp` pins them
// against the reference's table by value, because a transposed digit here is a different instrument
// that still makes a plausible noise.

constexpr std::array<phi::PhiTriConfig, 6> kToneBank = {{
    {phi::kPhi150, 0.85f, 0.0f, false},  // [0] delay time
    {phi::kPhi125, 0.8f, 0.4f, false},   // [1] feedback
    {phi::kPhi175, 0.7f, 0.6f, false},   // [2] soft clip intensity
    {phi::kPhi100, 0.65f, 0.15f, false}, // [3] fold intensity
    {phi::kPhi200, 0.75f, 0.35f, false}, // [4] highpass cutoff
    {phi::kPhi125, 0.5f, 0.45f, false},  // [5] pitched mod depth
}};

constexpr std::array<phi::PhiTriConfig, 5> kCharacterBank = {{
    {phi::kPhi150, 1.0f, 0.25f, false}, // [0] depth
    {phi::kPhi175, 0.8f, 0.3f, false},  // [1] SVF resonance
    {phi::kPhi100, 0.75f, 0.7f, false}, // [2] S&H blend
    {phi::kPhi075, 0.8f, 0.2f, false},  // [3] stereo width
    {phi::kPhi075, 0.6f, 0.1f, true},   // [4] pitched mod offset — BIPOLAR, the only one
}};

const device::ParamDesc* descOf(Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= std::size(kParamDescs)) {
		return nullptr;
	}
	return &kParamDescs[index];
}

float coerce(const device::ParamDesc& desc, float value) {
	if (!std::isfinite(value)) {
		return static_cast<float>(desc.def);
	}
	const float clamped = device::coerceControl(desc, value);
	return desc.stepped ? static_cast<float>(std::lround(clamped)) : clamped;
}

/// `add_saturate`, where the firmware has one INSIDE the wet path. See `eroder_types.h` for the one
/// that is deliberately not reproduced.
float sat(float x) noexcept { return std::clamp(x, -1.0f, 1.0f); }

/// **TWO'S-COMPLEMENT WRAP, WHERE THE FIRMWARE HAS A PLAIN `+` AND NOT `add_saturate`.** The q31
/// range [-2^31, 2^31) is [-1, 1) here; past either end the sum comes back in at the other.
///
/// **THIS IS THE ERODER'S LOUDEST BEHAVIOUR AND THE FIRST RELEASE OF THE PORT DID NOT HAVE IT.** The
/// SVF's `band` and `low` accumulate with `+=`, and the firmware's own fix (`3e8ce78a`, "saturate
/// SVF high computation to prevent overflow at high resonance") saturated `high` only — so at high
/// resonance and depth the filter still overflows, wraps to the opposite extreme, and throws the
/// read head half a base delay in one sample. MEASURED against a q31 transcription of the loop
/// (`test_fx_eroder.cpp`): a third of a coordinate grid wraps; at one Character setting 43% of all
/// samples do, and the default Character wraps thousands of times a second. A float port without
/// this is a smooth noise-modulated comb; the reference's is not. The triangle's `+=` and the wet
/// highpass's plain adds wrap too, and are reproduced for the same reason.
float wrap(float x) noexcept {
	if (x >= -1.0f && x < 1.0f) {
		return x;
	}
	return x - 2.0f * std::floor((x + 1.0f) * 0.5f);
}

/// **NOT THE FIRMWARE'S, AND NEEDED BECAUSE FLOAT IS NOT Q31.** An integer comb decays to exactly
/// zero on its own — a q31 times 0.9 eventually rounds to nothing. A float one decays INTO the
/// denormals and stays there for thousands of laps, which is the defect `test_fx_devices.cpp`'s
/// silence sweep exists to find. Below this the feedback tap is flushed.
constexpr float kSilenceFloor = 1.0e-7f / static_cast<float>(kHeadroom);

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE CODEC.                                                                                         //
//--------------------------------------------------------------------------------------------------//

Row rowOf(const State& state) {
	Row row {};
	const Settings& s = state.settings;
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Tone)] = s.tone;
	row[static_cast<size_t>(Param::Character)] = s.character;
	row[static_cast<size_t>(Param::Gamma)] = s.gamma;
	row[static_cast<size_t>(Param::Cutoff)] = s.cutoffOctaves;
	row[static_cast<size_t>(Param::Mix)] = s.mix;
	return row;
}

State stateOf(const Row& row, size_t fieldsPresent) {
	State out {};
	const auto read = [&](Param p) {
		const device::ParamDesc* d = descOf(p);
		if (d == nullptr) {
			return 0.0f;
		}
		return static_cast<size_t>(p) < fieldsPresent ? coerce(*d, row[static_cast<size_t>(p)])
		                                              : static_cast<float>(d->def);
	};
	Settings& s = out.settings;
	s.enabled = read(Param::Enabled) >= 0.5f;
	s.tone = read(Param::Tone);
	s.character = read(Param::Character);
	s.gamma = read(Param::Gamma);
	s.cutoffOctaves = read(Param::Cutoff);
	s.mix = read(Param::Mix);
	return out;
}

float valueOf(const State& state, Param param) {
	const auto index = static_cast<size_t>(param);
	if (index >= static_cast<size_t>(Param::kCount)) {
		return 0.0f;
	}
	return rowOf(state)[index];
}

void setValueOf(State* state, Param param, float value) {
	const auto index = static_cast<size_t>(param);
	if (state == nullptr || index >= static_cast<size_t>(Param::kCount)) {
		return;
	}
	const device::ParamDesc* desc = descOf(param);
	if (desc == nullptr || !std::isfinite(value)) {
		return;
	}
	Row row = rowOf(*state);
	row[index] = coerce(*desc, value);
	*state = stateOf(row, static_cast<size_t>(Param::kCount));
}

//--------------------------------------------------------------------------------------------------//
// THE MAPPING.                                                                                       //
//--------------------------------------------------------------------------------------------------//

Phases phasesOf(double tone, double character, double gamma) noexcept {
	// `freqNorm + effectiveFreq()`, where `effectiveFreq()` is `freqPhaseOffset + 1024 * gammaPhase`
	// and the first two terms are one coordinate here. IN DOUBLE, as the firmware has it and for
	// `phi_triangle.hpp`'s reason: 1024 * gamma leaves a float's fraction behind almost at once.
	return { tone + kGammaScale * gamma, character + kGammaScale * gamma };
}

ToneBank evalToneBank(double phase) noexcept { return phi::evalTriangleBank<6>(phase, 1.0f, kToneBank); }

CharacterBank evalCharacterBank(double phase) noexcept {
	return phi::evalTriangleBank<5>(phase, 1.0f, kCharacterBank);
}

Derived derive(const Phases& phases, double cutoffOctaves, double mix, double pitchSemitones,
               double sampleRate) noexcept {
	const ToneBank tone = evalToneBank(phases.tone);
	const CharacterBank ch = evalCharacterBank(phases.character);
	const double rate = sampleRate > 0.0 ? sampleRate : kReferenceRate;

	// "unclamped — SVF/HPF/triangle need full range". The DELAY is what clamps, below.
	const double pitchRatio = std::exp2((pitchSemitones - kReferenceNote) / 12.0);
	const double knobRatio = std::clamp(std::exp2(cutoffOctaves), kCutoffRatioMin, kCutoffRatioMax);

	Derived d;

	// THE COMB. Longer for a higher bank value, SHORTER for a higher note — "comb fundamental tracks
	// pitch" — and clamped back into the line afterwards, so above a certain note it stops tracking.
	const double span = kMaxBaseDelaySeconds - kMinBaseDelaySeconds;
	const double untracked = kMinBaseDelaySeconds + std::max(kBankFloor, static_cast<double>(tone[0])) * span;
	d.baseDelaySeconds = std::clamp(untracked / pitchRatio, kMinBaseDelaySeconds, kMaxBaseDelaySeconds);
	d.feedback = std::max(kBankFloor, static_cast<double>(tone[1])) * kFeedbackCeiling;

	d.clipIntensity = static_cast<double>(tone[2]);
	d.foldIntensity = static_cast<double>(tone[3]);
	d.clipDrive = 1.0 + d.clipIntensity * kClipDriveSpan;
	d.foldDrive = 1.0 + d.foldIntensity * kFoldDriveSpan;

	d.hpfHz = kHpfBaseHz * std::exp2(kHpfOctaves * static_cast<double>(tone[4])) * pitchRatio * knobRatio;
	// THE WET HIGHPASS'S COEFFICIENT, STATED AT 44.1 kHz AND CARRIED TO THE RUNNING RATE BY ITS POLE (NX-D1).
	// `2 pi f / rate` is the firmware's small-angle form, and with its 0.5 ceiling it is a different corner at every
	// rate — the clamp engages at 3.5 kHz at 44.1 and never at 96. The pole `1 - alpha` per 44.1 kHz frame is the
	// filter the firmware HAS, so the running rate takes the same pole per second: `(1 - alpha)^(44100 / rate)`.
	const double referenceAlpha = std::min(2.0 * std::numbers::pi * d.hpfHz / kReferenceRate, kHpfAlphaMax);
	d.hpfAlpha = rate == kReferenceRate ? referenceAlpha : 1.0 - std::pow(1.0 - referenceAlpha, kReferenceRate / rate);

	d.triangleDepth = static_cast<double>(tone[5]);
	d.triangleHz =
	    kTriangleBaseHz * pitchRatio * knobRatio * std::exp2(static_cast<double>(ch[4]) * kTriangleOffsetOctaves);

	d.depth = std::max(kDepthFloor, static_cast<double>(ch[0]));
	d.resonance = static_cast<double>(ch[1]) * kResonanceCeiling;
	d.shBlend = static_cast<double>(ch[2]);
	d.width = static_cast<double>(ch[3]);

	// THE SVF'S CORNER: clamped AS A 44.1 kHz COEFFICIENT, because that is the domain the firmware's
	// clamps are stated in; then to hertz; then back to a coefficient AT 44.1 kHz, AT EVERY HOST RATE — the SVF
	// runs on the modulation clock (`Eroder::tickPhase_`, NX-D1), so its coefficient is the firmware's always.
	const double referenceCoefficient =
	    std::clamp(kSvfReferenceCoefficient * pitchRatio * knobRatio, kSvfCoefficientMin, kSvfCoefficientMax);
	d.svfHz = std::asin(referenceCoefficient * 0.5) / std::numbers::pi * kReferenceRate;
	const double angle = std::min(std::numbers::pi * d.svfHz / kReferenceRate, std::numbers::pi * 0.5);
	d.svfCoefficient = std::min(2.0 * std::sin(angle), kSvfCoefficientMax);

	// "Compensation: (1-feedback)/drive tames comb resonance + waveshaper amplification."
	double compensation = 1.0 - d.feedback;
	if (d.clipIntensity > 0.0) {
		compensation /= d.clipDrive;
	}
	if (d.foldIntensity > 0.0) {
		compensation /= d.foldDrive;
	}
	d.wetGain = mix * compensation;
	return d;
}

float softClip(float x, float drive) noexcept {
	// `q31SoftClip`: linear to the knee, a quarter slope above it, and a hard ceiling at one.
	float driven = x * drive;
	if (driven > kClipKnee) {
		driven = kClipKnee + (driven - kClipKnee) * kClipSlopeAboveKnee;
	}
	else if (driven < -kClipKnee) {
		driven = -kClipKnee + (driven + kClipKnee) * kClipSlopeAboveKnee;
	}
	return std::clamp(driven, -1.0f, 1.0f);
}

float triFold(float x, float drive) noexcept {
	// `q31TriFold`: a period-4 triangle in the driven signal. The firmware takes the modulo with a
	// bitmask, which for a negative two's-complement value is a FLOOR modulo — so this is too.
	const float driven = x * drive;
	float phase = std::fmod(driven + 1.0f, 4.0f);
	if (phase < 0.0f) {
		phase += 4.0f;
	}
	const float folded = phase < 2.0f ? phase - 1.0f : 3.0f - phase;
	return std::min(folded, 1.0f);
}

float shapeWet(float wet, const Derived& derived) noexcept {
	// "Waveshaping: soft clip → fold."
	if (derived.clipIntensity > 0.0) {
		wet = softClip(wet, static_cast<float>(derived.clipDrive));
	}
	if (derived.foldIntensity > 0.0) {
		wet = triFold(wet, static_cast<float>(derived.foldDrive));
	}
	return wet;
}

//--------------------------------------------------------------------------------------------------//
// THE DEVICE.                                                                                        //
//--------------------------------------------------------------------------------------------------//

void Eroder::prepare(double sampleRate) {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// THE ONE ALLOCATION. The firmware's 256 samples as a duration, plus a few frames so that the
	// longest read and its interpolation neighbour are both inside the line at any rate.
	line_.prepare(sampleRate_, kLineSeconds + 8.0 / sampleRate_);
	tickStep_ = kReferenceRate / sampleRate_;
	reset();
}

void Eroder::reset() noexcept {
	line_.reset();
	writeCounter_ = 0;
	placed_ = false;
	untilTick_ = 0;
	smoothedTone_ = 0.0;
	smoothedCharacter_ = 0.0;
	svfLowL_ = svfLowR_ = svfBandL_ = svfBandR_ = 0.0f;
	held_ = 0.0f;
	prevInputL_ = 0.0f;
	prevFilteredL_ = 0.0f;
	feedbackL_ = feedbackR_ = 0.0f;
	wetHpfL_ = wetHpfR_ = 0.0f;
	triValue_ = 0.0f;
	triStep_ = 0.0f;
	rng_ = kRngSeed;
	// "A TICK IS DUE": the first frame runs one, at any rate.
	tickPhase_ = 1.0;
	pendingInputCross_ = false;
	modL_ = modR_ = 0.0f;
	modTicks_ = 0;
	lastDelayFrames_[0] = lastDelayFrames_[1] = 0.0;
	holds_ = 0;
}

void Eroder::setTarget(const Settings& settings) noexcept { target_ = settings; }

void Eroder::setNoteContext(const device::NoteContext& note) noexcept {
	if (std::isfinite(note.pitchSemitones)) {
		pitchSemitones_ = static_cast<double>(note.pitchSemitones);
	}
}

float Eroder::nextNoise() noexcept {
	rng_ ^= rng_ << 13;
	rng_ ^= rng_ >> 17;
	rng_ ^= rng_ << 5;
	// THE TOP 24 BITS, signed — exact in a float, and in [-1, 1).
	return static_cast<float>(static_cast<int32_t>(rng_) >> 8) * (1.0f / 8388608.0f);
}

void Eroder::processStereo(float* left, float* right, int frames) noexcept {
	if (left == nullptr || right == nullptr || frames <= 0) {
		return;
	}
	const Settings s = target_;
	// `isEnabled()` is `mix > 0`: at zero the firmware does not run at all, state included.
	if (!s.enabled || !(s.mix > 0.0f)) {
		return;
	}
	if (line_.length() < 8) {
		return;
	}

	// **THE CONTROL PATH RUNS ON AN ABSOLUTE GRID, CARRIED ACROSS CALLS — `untilTick_`, the filter's
	// idiom.** The first draft ran it once per call, as the firmware does once per buffer, and
	// `test_fx_devices.cpp`'s partition sweep caught it: with the smoother TRAVELLING, where the
	// host cut the block decided how far the glide had got and how long each derived value was
	// held, so the same passage came out differently at a different buffer size. That is the
	// firmware's behaviour and it is a defect there too; nothing a user can turn gets the other
	// sound back. My own partition case could not see it — it never moved a control.
	int done = 0;
	while (done < frames) {
		if (untilTick_ <= 0) {
			tick(s);
			untilTick_ = kControlFrames;
		}
		const int take = std::min(frames - done, untilTick_);
		renderChunk(left + done, right + done, take);
		untilTick_ -= take;
		done += take;
	}
}

void Eroder::tick(const Settings& s) noexcept {
	// SMOOTH THE TWO COORDINATES — per control block, by a TIME.
	const auto toneTarget = static_cast<double>(s.tone);
	const auto characterTarget = static_cast<double>(s.character);
	if (!placed_) {
		smoothedTone_ = toneTarget;
		smoothedCharacter_ = characterTarget;
		placed_ = true;
	}
	else {
		const double alpha =
		    1.0 - std::exp(-static_cast<double>(kControlFrames) / (kSmoothingSeconds * sampleRate_));
		smoothedTone_ += (toneTarget - smoothedTone_) * alpha;
		smoothedCharacter_ += (characterTarget - smoothedCharacter_) * alpha;
	}

	derived_ = derive(phasesOf(smoothedTone_, smoothedCharacter_, static_cast<double>(s.gamma)),
	                  static_cast<double>(s.cutoffOctaves), static_cast<double>(s.mix), pitchSemitones_,
	                  sampleRate_);
}

void Eroder::renderChunk(float* left, float* right, int frames) noexcept {
	// EVERYTHING BELOW IS A FUNCTION OF `derived_` AND THE RUNNING STATE, so recomputing it at a
	// chunk boundary the host chose gives the numbers the previous chunk had.
	const Derived& d = derived_;

	const auto baseDelay = static_cast<float>(d.baseDelaySeconds * sampleRate_);
	const auto minDelay = static_cast<float>(kMinDelaySeconds * sampleRate_);
	const auto maxDelay = static_cast<float>(kMaxBaseDelaySeconds * sampleRate_);
	const float modScale = baseDelay * static_cast<float>(kModExcursion);
	const auto feedback = static_cast<float>(d.feedback);

	// DEPTH IS BAKED INTO THE BLEND WEIGHTS, as the firmware bakes it, and the right channel's white
	// noise is `(1 - width)` of the left's plus `width` of its own.
	const auto depth = static_cast<float>(d.depth);
	const auto shBlend = static_cast<float>(d.shBlend);
	const auto width = static_cast<float>(d.width);
	const float whiteDepth = (1.0f - shBlend) * depth;
	const float shDepth = shBlend * depth;
	const float monoWhiteDepth = (1.0f - width) * whiteDepth;
	const float widthWhiteDepth = width * whiteDepth;

	const auto svfF = static_cast<float>(d.svfCoefficient);
	const auto svfQ = static_cast<float>(1.0 - d.resonance);

	// THE PITCHED TRIANGLE. Amplitude is baked into the step; direction persists between blocks.
	const bool usePitchedMod = d.triangleDepth > 0.0;
	const auto triPeak = static_cast<float>(d.triangleDepth);
	if (usePitchedMod) {
		// PER MODULATION TICK, so per 1/44100 s — the ceiling and the wrap are the firmware's at every rate.
		const double step = std::min(4.0 * d.triangleDepth * d.triangleHz / kReferenceRate, kTriangleStepMax);
		const float stepMag = std::min(static_cast<float>(step), triPeak);
		triStep_ = triStep_ >= 0.0f ? stepMag : -stepMag;
		// "Clamp value in case peak shrank since last buffer."
		triValue_ = std::clamp(triValue_, -triPeak, triPeak);
	}

	const auto hpfAlpha = static_cast<float>(d.hpfAlpha);
	// **THE WET PATH RUNS IN THE FIRMWARE'S UNITS AND COMES BACK OUT BY THE SAME FACTOR.** See
	// `kHeadroom`: in, the signal is scaled down so that every clamp, the soft clip's knee and the
	// fold's period sit as far above it as they do on the hardware; out, the wet is scaled back up.
	// Linear in between, so the two factors cancel exactly wherever nothing is clipping — which on
	// the reference is almost everywhere.
	const float kFromFullScale = 1.0f / static_cast<float>(kHeadroom);
	const float outGain = static_cast<float>(d.wetGain) * static_cast<float>(kHeadroom);

	for (int i = 0; i < frames; ++i) {
		// A NON-FINITE INPUT SAMPLE IS TREATED AS 0 (as Comp and EQ do): `sat()` is a clamp, which
		// passes a NaN, and the feedback floor test below keeps it, so one NaN circulated for ever.
		left[i] = std::isfinite(left[i]) ? left[i] : 0.0f;
		right[i] = std::isfinite(right[i]) ? right[i] : 0.0f;
		const float inL = left[i];
		const float inR = right[i];

		// "Write input + feedback to delay (IIR comb filter topology)."
		// INTO THE FIRMWARE'S UNITS: a full-scale input is 1/128 of the ceiling every clamp below sits at.
		line_.write(sat(inL * kFromFullScale + feedbackL_ * feedback), sat(inR * kFromFullScale + feedbackR_ * feedback));
		++writeCounter_;
		const auto newest = static_cast<double>(writeCounter_ - 1u);

		// THE HOLD RE-DRAWS ON A ZERO CROSSING of the dry left input OR of the SVF's left lowpass.
		// `(a ^ b) < 0` is "the sign bits differ", with zero counting as positive. The input's is seen
		// here, at the host rate, and fires at the next modulation tick; at 44.1 kHz that is this frame's.
		pendingInputCross_ = pendingInputCross_ || ((inL < 0.0f) != (prevInputL_ < 0.0f));
		prevInputL_ = inL;

		// THE MODULATION CLOCK — `tickPhase_` in `eroder.h`. Between ticks the heads hold.
		while (tickPhase_ >= 1.0) {
			tickPhase_ -= 1.0;
			++modTicks_;

			// TWO DRAWS A TICK, ALWAYS, and a third when the hold fires — the firmware's order.
			const float whiteL = nextNoise();
			const float whiteRIndependent = nextNoise();

			const bool filterCross = (svfLowL_ < 0.0f) != (prevFilteredL_ < 0.0f);
			if (pendingInputCross_ || filterCross) {
				held_ = nextNoise() * 0.5f; // `getNoise() >> 1`
				++holds_;
			}
			pendingInputCross_ = false;
			prevFilteredL_ = svfLowL_;

			const float shTerm = held_ * shDepth;
			const float noiseL = sat(whiteL * whiteDepth + shTerm);
			const float noiseR = sat(sat(whiteL * monoWhiteDepth + whiteRIndependent * widthWhiteDepth) + shTerm);

			// THE SVF, Chamberlin form, `high` saturated "to prevent runaway at high resonance".
			const float highL = sat(wrap(noiseL - svfLowL_) - svfBandL_ * svfQ);
			svfBandL_ = wrap(svfBandL_ + highL * svfF);
			svfLowL_ = wrap(svfLowL_ + svfBandL_ * svfF);
			const float highR = sat(wrap(noiseR - svfLowR_) - svfBandR_ * svfQ);
			svfBandR_ = wrap(svfBandR_ + highR * svfF);
			svfLowR_ = wrap(svfLowR_ + svfBandR_ * svfF);

			// BANDPASS ONLY, plus the triangle.
			modL_ = svfBandL_;
			modR_ = svfBandR_;
			if (usePitchedMod) {
				triValue_ = wrap(triValue_ + triStep_);
				if (triValue_ > triPeak) {
					triValue_ = triPeak - (triValue_ - triPeak);
					triStep_ = -triStep_;
				}
				else if (triValue_ < -triPeak) {
					triValue_ = -triPeak - (triValue_ + triPeak);
					triStep_ = -triStep_;
				}
				modL_ = sat(modL_ + triValue_);
				modR_ = sat(modR_ + triValue_);
			}
		}
		tickPhase_ += tickStep_;

		// THE READ HEADS, each channel shaken by its own noise.
		const float delayL = std::clamp(baseDelay + modL_ * modScale, minDelay, maxDelay);
		const float delayR = std::clamp(baseDelay + modR_ * modScale, minDelay, maxDelay);
		lastDelayFrames_[0] = static_cast<double>(delayL);
		lastDelayFrames_[1] = static_cast<double>(delayR);

		float wetL = 0.0f;
		float wetR = 0.0f;
		float unused = 0.0f;
		fx::readLerp(line_, newest - static_cast<double>(delayL), &wetL, &unused);
		fx::readLerp(line_, newest - static_cast<double>(delayR), &unused, &wetR);

		// "Feedback tap BEFORE waveshaping."
		feedbackL_ = std::abs(wetL) < kSilenceFloor ? 0.0f : wetL;
		feedbackR_ = std::abs(wetR) < kSilenceFloor ? 0.0f : wetR;

		// SOFT CLIP, THEN FOLD.
		wetL = shapeWet(wetL, d);
		wetR = shapeWet(wetR, d);

		// ONE-POLE HIGHPASS ON THE WET: a lowpass, subtracted.
		wetHpfL_ = wrap(wetHpfL_ + wrap(wetL - wetHpfL_) * hpfAlpha);
		wetL = wrap(wetL - wetHpfL_);
		wetHpfR_ = wrap(wetHpfR_ + wrap(wetR - wetHpfR_) * hpfAlpha);
		wetR = wrap(wetR - wetHpfR_);
		if (std::abs(wetHpfL_) < 1.0e-20f) {
			wetHpfL_ = 0.0f;
		}
		if (std::abs(wetHpfR_) < 1.0e-20f) {
			wetHpfR_ = 0.0f;
		}

		// ADDITIVE, the dry at unity. RIGHT THEN LEFT — `processMono` runs this over one buffer and
		// the survivor must be the left. NOT SATURATED: see `eroder_types.h`.
		right[i] = inR + wetR * outGain;
		left[i] = inL + wetL * outGain;
	}
}

void Eroder::processMono(float* mono, int frames) noexcept {
	if (mono == nullptr || frames <= 0) {
		return;
	}
	processStereo(mono, mono, frames);
}

//--------------------------------------------------------------------------------------------------//
// THE OPS TABLE.                                                                                     //
//--------------------------------------------------------------------------------------------------//

namespace {
Eroder& asEroder(void* storage) { return *static_cast<Eroder*>(storage); }
} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(Eroder),
    .stateAlign = alignof(Eroder),
    .construct = [](void* storage) { new (storage) Eroder(); },
    .destruct = [](void* storage) { asEroder(storage).~Eroder(); },
    .prepare = [](void* storage, double sampleRate, int) { asEroder(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asEroder(storage).reset(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asEroder(storage).setTarget(stateOf(row, take).settings);
        },
    .setNoteContext =
        [](void* storage, const device::NoteContext& note) { asEroder(storage).setNoteContext(note); },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asEroder(storage).processStereo(left, right, frames);
        },
    .processMono =
        [](void* storage, float* mono, int frames) { asEroder(storage).processMono(mono, frames); },
    .holdsTail = [](const void* storage) { return static_cast<const Eroder*>(storage)->holdsTail(); },
};

} // namespace feathers::fx::eroder
