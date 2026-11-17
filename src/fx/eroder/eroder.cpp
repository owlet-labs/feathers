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
#include <limits>
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
/// samples do (some 19,000 a second), while the default coordinate wraps a handful of times a second. Those
/// filter wraps are caught by the saturation after them and move no head; the wraps that are heard are the
/// triangle's and the filter's at a high corner. A float port without
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

/// ONE POLE TOWARD A TARGET, LANDING ON IT: a float one-pole stalls a little short of where it is going (the step
/// rounds away), and the last of the way is taken at once.
float glideTo(float value, float target, float coefficient) noexcept {
	const float next = value + (target - value) * coefficient;
	return next == value ? target : next;
}

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
	row[static_cast<size_t>(Param::Mode)] = static_cast<float>(s.mode);
	row[static_cast<size_t>(Param::Amount)] = s.amount;
	row[static_cast<size_t>(Param::Freq)] = s.freq;
	row[static_cast<size_t>(Param::Width)] = s.width;
	row[static_cast<size_t>(Param::Source)] = s.source;
	row[static_cast<size_t>(Param::Grit)] = s.grit;
	row[static_cast<size_t>(Param::Stereo)] = s.stereo;
	row[static_cast<size_t>(Param::Resonance)] = s.resonance;
	row[static_cast<size_t>(Param::Drive)] = s.drive;
	row[static_cast<size_t>(Param::Track)] = s.track ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::DriveMode)] = static_cast<float>(s.driveMode);
	row[static_cast<size_t>(Param::DriveShape)] = s.driveShape;
	row[static_cast<size_t>(Param::HpWet)] = s.hpWet;
	row[static_cast<size_t>(Param::SubThru)] = s.subThru ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Follow)] = s.follow;
	row[static_cast<size_t>(Param::RustBias)] = s.rustBias;
	row[static_cast<size_t>(Param::RustRectify)] = s.rustRectify;
	row[static_cast<size_t>(Param::Step)] = s.step;
	row[static_cast<size_t>(Param::Detune)] = s.detune;
	row[static_cast<size_t>(Param::ModFold)] = s.modFold;
	row[static_cast<size_t>(Param::Heat)] = s.heat;
	row[static_cast<size_t>(Param::Blend)] = s.blend ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::EmphAmount)] = s.emphAmount;
	row[static_cast<size_t>(Param::EmphShape)] = s.emphBell ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::EmphRestore)] = s.emphRestore ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::EmphInLoop)] = s.emphInLoop ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Smear)] = s.smear;
	row[static_cast<size_t>(Param::FbTap)] = s.tapDriven ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Loop)] = s.loop;
	row[static_cast<size_t>(Param::LoopInterval)] = s.loopInterval;
	row[static_cast<size_t>(Param::EmphOffset)] = s.emphOffset;
	row[static_cast<size_t>(Param::SlotAAmount)] = s.slotA.amount;
	row[static_cast<size_t>(Param::SlotAType)] = static_cast<float>(s.slotA.type);
	row[static_cast<size_t>(Param::SlotAShape)] = s.slotA.shape;
	row[static_cast<size_t>(Param::SlotAMix)] = s.slotA.mix;
	row[static_cast<size_t>(Param::SlotBAmount)] = s.slotB.amount;
	row[static_cast<size_t>(Param::SlotBType)] = static_cast<float>(s.slotB.type);
	row[static_cast<size_t>(Param::SlotBShape)] = s.slotB.shape;
	row[static_cast<size_t>(Param::SlotBMix)] = s.slotB.mix;
	row[static_cast<size_t>(Param::SlotRoute)] = s.slotParallel ? 1.0f : 0.0f;
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
	s.mode = static_cast<Mode>(
	    std::clamp(static_cast<int>(std::lround(read(Param::Mode))), 0, static_cast<int>(Mode::kCount) - 1));
	s.amount = read(Param::Amount);
	s.freq = read(Param::Freq);
	s.width = read(Param::Width);
	s.source = read(Param::Source);
	s.grit = read(Param::Grit);
	s.stereo = read(Param::Stereo);
	s.resonance = read(Param::Resonance);
	s.drive = read(Param::Drive);
	s.track = read(Param::Track) >= 0.5f;
	s.driveMode = static_cast<DriveMode>(std::clamp(static_cast<int>(std::lround(read(Param::DriveMode))), 0,
	                                                static_cast<int>(DriveMode::kCount) - 1));
	s.driveShape = read(Param::DriveShape);
	s.hpWet = read(Param::HpWet);
	s.subThru = read(Param::SubThru) >= 0.5f;
	s.follow = read(Param::Follow);
	s.rustBias = read(Param::RustBias);
	s.rustRectify = read(Param::RustRectify);
	s.step = read(Param::Step);
	s.detune = read(Param::Detune);
	s.modFold = read(Param::ModFold);
	s.heat = read(Param::Heat);
	s.blend = read(Param::Blend) >= 0.5f;
	s.emphAmount = read(Param::EmphAmount);
	s.emphBell = read(Param::EmphShape) >= 0.5f;
	s.emphRestore = read(Param::EmphRestore) >= 0.5f;
	s.emphInLoop = read(Param::EmphInLoop) >= 0.5f;
	s.smear = read(Param::Smear);
	s.tapDriven = read(Param::FbTap) >= 0.5f;
	s.loop = read(Param::Loop);
	s.loopInterval = read(Param::LoopInterval);
	s.emphOffset = read(Param::EmphOffset);
	s.slotA.amount = read(Param::SlotAAmount);
	s.slotA.type = static_cast<DriveMode>(
	    std::clamp(static_cast<int>(std::lround(read(Param::SlotAType))), 0, static_cast<int>(DriveMode::kCount) - 1));
	s.slotA.shape = read(Param::SlotAShape);
	s.slotA.mix = read(Param::SlotAMix);
	s.slotB.amount = read(Param::SlotBAmount);
	s.slotB.type = static_cast<DriveMode>(
	    std::clamp(static_cast<int>(std::lround(read(Param::SlotBType))), 0, static_cast<int>(DriveMode::kCount) - 1));
	s.slotB.shape = read(Param::SlotBShape);
	s.slotB.mix = read(Param::SlotBMix);
	s.slotParallel = read(Param::SlotRoute) >= 0.5f;
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

	d.levelMakeUp = std::sqrt((1.0 + d.feedback) / (1.0 - d.feedback));
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
	dust_.prepare(sampleRate_);
	analyser_.prepare(sampleRate_);
	combShaper_.prepare(sampleRate_);
	combTapShaper_.prepare(sampleRate_);
	shared_.prepare(sampleRate_, false);
	gainSmoothing_ = static_cast<float>(1.0 - std::exp(-1.0 / (kSmoothingSeconds * sampleRate_)));
	followAttack_ = static_cast<float>(1.0 - std::exp(-1.0 / (kFollowAttackSeconds * sampleRate_)));
	followRelease_ = static_cast<float>(1.0 - std::exp(-1.0 / (kFollowReleaseSeconds * sampleRate_)));
	reset();
}

void Eroder::reset() noexcept {
	dust_.reset();
	analyser_.reset();
	untilAnalyserStep_ = 0;
	analyserLive_ = true;
	envelope_ = 0.0f;
	combFollow_ = 1.0f;
	combShaper_.reset();
	combTapShaper_.reset();
	tapStageRan_ = false;
	shared_.reset();
	combHighpass_.reset();
	gainsPlaced_ = false;
	outGain_ = outGainTarget_ = 0.0f;
	dryGain_ = dryGainTarget_ = 1.0f;
	heatGain_ = 1.0f;
	smoothedCutoff_ = 0.0;
	smoothedGamma_ = 0.0;
	readings_.fill(0.0f);
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
		dust_.setPitch(pitchSemitones_);
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
	if (!s.enabled) {
		return;
	}
	// A CHANGE OF ENGINE STARTS THE NEW ONE AS IF JUST SWITCHED ON: neither line means anything to the other.
	if (s.mode != running_) {
		reset();
		running_ = s.mode;
	}

	// **THE ENGINE RUNS IN RUNS OF AT MOST A CONTROL BLOCK, WITH THE SPECTRUM TAP LISTENING EITHER SIDE.** The cuts
	// are the tap's own grid, carried across calls; each engine keeps its own grid inside and comes out the same
	// however it is cut (the partition cases), so the sound is what it was before anything listened.
	// THE SPECTRUM IS COMPUTED ONLY WHILE A VIEW IS ASKING (`ReadingsSink::wanted`; a host that never learned the
	// question says always), and never with nobody to publish to. A picture that was idle starts from nothing rather
	// than from a window of old audio.
	const bool analysing = sink_.publish != nullptr && (sink_.wanted == nullptr || sink_.wanted(sink_.context, frames));
	if (analysing && !analyserLive_) {
		analyser_.reset();
	}
	analyserLive_ = analysing;
	float nearFrames[2] = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max() };
	float farFrames[2] = { 0.0f, 0.0f };
	int done = 0;
	while (done < frames) {
		if (untilAnalyserStep_ <= 0) {
			if (analysing) {
				analyser_.step();
			}
			untilAnalyserStep_ = kControlFrames;
			// THE LEVEL THE NEXT CONTROL BLOCK'S SHAKE RIDES IS THE FOLLOWER AS IT STOOD WHEN THE BLOCK BEGAN: taken
			// on the grid, so however the host cuts its blocks the engines are told the same thing at the same frame.
			dust_.setEnvelope(envelope_);
			combFollow_ = followGain(s.follow, envelope_);
		}
		const int take = std::min(frames - done, untilAnalyserStep_);
		float dryL[kControlFrames];
		float dryR[kControlFrames];
		if (analysing) {
			for (int i = 0; i < take; ++i) {
				dryL[i] = left[done + i];
				dryR[i] = right[done + i];
			}
		}
		// THE FOLLOWER: the input's peak, fast up and slow down (`Param::Follow`). Always kept, so turning Follow up
		// finds it current.
		for (int i = 0; i < take; ++i) {
			const float l = std::isfinite(left[done + i]) ? std::abs(left[done + i]) : 0.0f;
			const float r = std::isfinite(right[done + i]) ? std::abs(right[done + i]) : 0.0f;
			const float peak = std::max(l, r);
			envelope_ += (peak - envelope_) * (peak > envelope_ ? followAttack_ : followRelease_);
			if (envelope_ < kFollowFloor) {
				envelope_ = 0.0f;
			}
		}
		runEngine(s, left + done, right + done, take);
		if (analysing) {
			for (int i = 0; i < take; ++i) {
				analyser_.push(dryL[i], dryR[i], left[done + i], right[done + i]);
			}
		}
		for (int c = 0; c < 2; ++c) {
			const auto at = static_cast<float>(s.mode == Mode::Comb ? lastDelayFrames_[c] : dust_.lastDelayFramesForTest(c));
			nearFrames[c] = std::min(nearFrames[c], at);
			farFrames[c] = std::max(farFrames[c], at);
		}
		untilAnalyserStep_ -= take;
		done += take;
	}
	const auto perFrame = static_cast<float>(1.0 / sampleRate_);
	readings_[static_cast<size_t>(Reading::HeadNearL)] = nearFrames[0] * perFrame;
	readings_[static_cast<size_t>(Reading::HeadFarL)] = farFrames[0] * perFrame;
	readings_[static_cast<size_t>(Reading::HeadNearR)] = nearFrames[1] * perFrame;
	readings_[static_cast<size_t>(Reading::HeadFarR)] = farFrames[1] * perFrame;
	publishReadings();
}

void Eroder::publishReadings() noexcept {
	readings_[static_cast<size_t>(Reading::FrameA)] = analyser_.frame();
	for (size_t k = 0; k < dsp::kAnalyserBins; ++k) {
		readings_[static_cast<size_t>(Reading::PreFirst) + k] = analyser_.pre()[k];
		readings_[static_cast<size_t>(Reading::PostFirst) + k] = analyser_.post()[k];
	}
	readings_[static_cast<size_t>(Reading::FrameB)] = analyser_.frame();
	readings_[static_cast<size_t>(Reading::PitchSemitones)] = static_cast<float>(pitchSemitones_);
	if (sink_.publish != nullptr) {
		sink_.publish(sink_.context, readings_.data(), readings_.size());
	}
}

void Eroder::runEngine(const Settings& s, float* left, float* right, int frames) noexcept {
	if (s.mode != Mode::Comb) {
		// DUST AND RUST RUN AT ANY MIX, zero included: the line keeps filling, so bringing Mix back up reads the present.
		dust_.process(s, left, right, frames);
		return;
	}
	// THE COMB RUNS AT ANY MIX TOO. The reference does not run at all at zero, state included, so a Mix brought up
	// from nothing read a line that had stood still since it went down.
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
	// CUTOFF AND GAMMA GLIDE WITH THEM: a route on either was a staircase of control blocks.
	const auto cutoffTarget = static_cast<double>(s.cutoffOctaves);
	const auto gammaTarget = static_cast<double>(s.gamma);
	if (!placed_) {
		smoothedTone_ = toneTarget;
		smoothedCharacter_ = characterTarget;
		smoothedCutoff_ = cutoffTarget;
		smoothedGamma_ = gammaTarget;
		placed_ = true;
	}
	else {
		const double alpha =
		    1.0 - std::exp(-static_cast<double>(kControlFrames) / (kSmoothingSeconds * sampleRate_));
		// LANDING ON THE TARGET, so a control at rest derives exactly what it is set to.
		const auto toward = [alpha](double value, double target) {
			const double gap = target - value;
			return std::abs(gap) < 1.0e-9 ? target : value + gap * alpha;
		};
		smoothedTone_ += (toneTarget - smoothedTone_) * alpha;
		smoothedCharacter_ += (characterTarget - smoothedCharacter_) * alpha;
		smoothedCutoff_ = toward(smoothedCutoff_, cutoffTarget);
		smoothedGamma_ = toward(smoothedGamma_, gammaTarget);
	}

	derived_ = derive(phasesOf(smoothedTone_, smoothedCharacter_, smoothedGamma_), smoothedCutoff_,
	                  static_cast<double>(s.mix), pitchSemitones_, sampleRate_);
	// HEAT: the wet path's units moved up toward full scale, so its clip and fold are reached. None is the
	// reference's level exactly.
	heatGain_ = static_cast<float>(std::exp2(kHeatOctaves * std::clamp(static_cast<double>(s.heat), 0.0, 1.0)));
	// THE TWO GAINS THE SAMPLE LOOP GLIDES TO: the wet's, with the level law's make-up; the dry's, whole unless Blend.
	// THE COMB'S LOOPS (`loops.h`), for the pitch of its own line. The level law's make-up assumes a clean loop, so a Driven tap goes without.
	const DriveSettings driveBefore = combShaper_.settings();
	const DriveSettings drive = deriveDrive(s.drive, s.driveMode, s.driveShape, sampleRate_, &driveBefore);
	combLoops_ = deriveLoops(s, 1.0 / derived_.baseDelaySeconds, driveLoopGain(drive), derived_.feedback);
	shared_.setThreshold(heatGain_ / static_cast<float>(kHeadroom));
	shared_.set(s, combLoops_, 1.0 / derived_.baseDelaySeconds);
	outGainTarget_ = static_cast<float>(derived_.wetGain * (combLoops_.tapDriven ? 1.0 : derived_.levelMakeUp)) *
	                 static_cast<float>(kHeadroom) / heatGain_;
	dryGainTarget_ = s.blend ? 1.0f - std::clamp(s.mix, 0.0f, 1.0f) : 1.0f;
	subThru_ = s.subThru;
	combHighpass_.set(deriveHighpass(s.hpWet, sampleRate_));
	if (!gainsPlaced_) {
		outGain_ = outGainTarget_;
		dryGain_ = dryGainTarget_;
		gainsPlaced_ = true;
	}
	// THE DRIVE STAGE, on the wet as it is about to be added to the dry.
	combShaper_.set(drive);
	combTapShaper_.set(drive);
}

void Eroder::renderChunk(float* left, float* right, int frames) noexcept {
	// EVERYTHING BELOW IS A FUNCTION OF `derived_` AND THE RUNNING STATE, so recomputing it at a
	// chunk boundary the host chose gives the numbers the previous chunk had.
	const Derived& d = derived_;

	const auto baseDelay = static_cast<float>(d.baseDelaySeconds * sampleRate_);
	const auto minDelay = static_cast<float>(kMinDelaySeconds * sampleRate_);
	const auto maxDelay = static_cast<float>(kMaxBaseDelaySeconds * sampleRate_);
	const float modScale = baseDelay * static_cast<float>(kModExcursion);
	// THE INNER LOOP'S GAIN: the bank's, less the Driven tap's and the emphasis's divisions (none, and so the bits it was, at neither).
	const auto feedback = static_cast<float>(combLoops_.innerFeedback);
	lastFeedbackGain_ = feedback;
	const bool tapDriven = combLoops_.tapDriven;
	const bool emph = shared_.emphOn();
	const bool emphInLoop = shared_.emphInLoop();
	const bool smearOn = shared_.smearOn();
	const bool limitInner = shared_.limitInner();
	// THE LINE'S CENTRE IS AS SET: the Smear's own delay adds inside the loop.
	const float centreDelay = baseDelay;
	const bool tapStage = combShaper_.settings().driven && (tapDriven || shared_.outerWanted());
	if (tapStage && !tapStageRan_) {
		combTapShaper_.reset();
	}
	tapStageRan_ = tapStage;

	// DEPTH IS BAKED INTO THE BLEND WEIGHTS, as the firmware bakes it, and the right channel's white
	// noise is `(1 - width)` of the left's plus `width` of its own.
	// FOLLOW RIDES THE DEPTH (a gain of exactly one at no Follow, which changes no bit).
	const float depth = static_cast<float>(d.depth) * combFollow_;
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
	const float kFromFullScale = heatGain_ / static_cast<float>(kHeadroom);
	const float toFullScale = static_cast<float>(kHeadroom) / heatGain_;

	for (int i = 0; i < frames; ++i) {
		// A NON-FINITE INPUT SAMPLE IS TREATED AS 0 (as Comp and EQ do): `sat()` is a clamp, which
		// passes a NaN, and the feedback floor test below keeps it, so one NaN circulated for ever.
		left[i] = std::isfinite(left[i]) ? left[i] : 0.0f;
		right[i] = std::isfinite(right[i]) ? right[i] : 0.0f;
		const float inL = left[i];
		const float inR = right[i];

		// "Write input + feedback to delay (IIR comb filter topology)."
		// INTO THE FIRMWARE'S UNITS: a full-scale input is 1/128 of the ceiling every clamp below sits at.
		// THE SUM: the input (through the emphasis unless that is in the loop), the inner loop and the outer's return; then Smear.
		float aL = inL;
		float aR = inR;
		if (emph && !emphInLoop) {
			shared_.emphIn(aL, aR);
		}
		float fbL = feedbackL_;
		float fbR = feedbackR_;
		if (limitInner) {
			shared_.limitInner(fbL, fbR);
		}
		lastFedBack_ = fbL;
		float xL = aL * kFromFullScale + fbL * feedback;
		float xR = aR * kFromFullScale + fbR * feedback;
		const bool outer = shared_.outerBegin();
		if (outer) {
			shared_.outerReturn(xL, xR);
		}
		if (emph && emphInLoop) {
			shared_.emphIn(xL, xR);
		}
		if (smearOn) {
			shared_.smear(xL, xR);
		}
		line_.write(sat(xL), sat(xR));
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
		const float delayL = std::clamp(centreDelay + modL_ * modScale, minDelay, maxDelay);
		const float delayR = std::clamp(centreDelay + modR_ * modScale, minDelay, maxDelay);
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
		const float readL = wetL;
		const float readR = wetR;

		// TAP D: the read, through the Drive stage again in the full-scale domain. IT IS TAKEN BEFORE THE COMB'S OWN CLIP AND FOLD,
		// which carry a gain of up to twenty that only the output's level law takes back out, so a loop through them would not be
		// the Drive's gain divided out. The outer loop is always taken here; the inner loop only when Tap says Driven. At the
		// defaults none of this runs.
		if (tapStage || outer || tapDriven) {
			float tapL = readL;
			float tapR = readR;
			if (tapStage) {
				tapL *= toFullScale;
				tapR *= toFullScale;
				combTapShaper_.process(tapL, tapR);
				tapL *= kFromFullScale;
				tapR *= kFromFullScale;
			}
			if (tapDriven) {
				feedbackL_ = std::abs(tapL) < kSilenceFloor ? 0.0f : tapL;
				feedbackR_ = std::abs(tapR) < kSilenceFloor ? 0.0f : tapR;
			}
			if (outer) {
				shared_.outerWrite(tapL, tapR);
			}
		}

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
		// MIX ARRIVES BY A TIME, a sample at a time: it was a staircase of control blocks.
		outGain_ = glideTo(outGain_, outGainTarget_, gainSmoothing_);
		dryGain_ = glideTo(dryGain_, dryGainTarget_, gainSmoothing_);
		float outL = wetL * outGain_;
		float outR = wetR * outGain_;
		lastCombWet_ = outL;
		combShaper_.process(outL, outR);
		lastCombShaped_ = outL;
		// EMPHASIS OUT: after both taps and the Drive.
		if (emph) {
			shared_.emphOut(outL, outR);
		}
		// HP WET, after the shaper. Sub Thru gives back what Blend's crossfade took of the dry's low end; with the
		// dry whole there is nothing to give back, and it adds nothing.
		if (combHighpass_.on()) {
			outL = combHighpass_.wet(0, outL);
			outR = combHighpass_.wet(1, outR);
			if (subThru_) {
				outL += (1.0f - dryGain_) * combHighpass_.taken(0, inL);
				outR += (1.0f - dryGain_) * combHighpass_.taken(1, inR);
			}
		}
		right[i] = inR * dryGain_ + outR;
		left[i] = inL * dryGain_ + outL;
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
    .setReadingsSink =
        [](void* storage, device::ReadingsSink sink) { asEroder(storage).setReadingsSink(sink); },
    .holdsTail = [](const void* storage) { return static_cast<const Eroder*>(storage)->holdsTail(); },
};

} // namespace feathers::fx::eroder
