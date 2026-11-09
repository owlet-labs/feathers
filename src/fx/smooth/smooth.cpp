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

#include "fx/smooth/smooth.h"
#include "character/fixedpoint.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace feathers::fx::smooth {

namespace {

constexpr float kOneQ31F = static_cast<float>(kOneQ31);

/// 1.0 → the reference's 0 dBFS, saturating, NaN to silence: the q31 the firmware's buffer would hold.
int32_t toQ31(float x) noexcept {
	return saturatingQ31(x * kQ31PerUnit);
}
float fromQ31(int32_t q) noexcept {
	return static_cast<float>(q) / kQ31PerUnit;
}

int32_t wadd(int32_t a, int32_t b) noexcept {
	return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
int32_t wsub(int32_t a, int32_t b) noexcept {
	return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}
int32_t mulhi(int32_t a, int32_t b) noexcept {
	return character::multiply_32x32_rshift32(a, b);
}
int32_t addSaturate(int32_t a, int32_t b) noexcept {
	return character::add_saturate(a, b);
}
/// `signed_saturate<22>`: the feedback into the input can be at most an eighth of the reference's 0 dBFS.
int32_t saturate22(int32_t v) noexcept {
	constexpr int32_t hi = (1 << 21) - 1;
	constexpr int32_t lo = -hi - 1;
	return v > hi ? hi : (v < lo ? lo : v);
}
/// `x << 1` on a q31, modular as the ARM's (and as C++20's).
int32_t twice(int32_t v) noexcept {
	return static_cast<int32_t>(static_cast<uint32_t>(v) << 1);
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE RATE, AND RESET.                                                                               //
//--------------------------------------------------------------------------------------------------//

void SmoothShaper::prepare(double sampleRate) noexcept {
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	// EACH CONSTANT IS ITS 44.1 kHz VALUE TIMES `scale`, which is exactly 1.0 there — so every one below is
	// the firmware's own number to the bit at the rate it was written for (a case asserts it), and at any
	// other rate the TIME it stands for is the same.
	const double scale = kReferenceRate / sampleRate_;
	perSampleAlpha_ = static_cast<float>(std::min(kPerSampleAlphaAtReference * scale, 1.0));
	// `kStridedAlpha = kPerSampleAlpha * kSmoothingStride`, in float as the firmware computes it.
	stridedAlpha_ = perSampleAlpha_ * static_cast<float>(kSmoothingStride);
	hpfAlphaQ_ = static_cast<int32_t>(std::min(kOutputHpfAlphaAtReference * scale, 0.5) * static_cast<double>(kOneQ31));
	// THE FEEDBACK TAP'S POLE IS 0.5 PER FRAME AT 44.1 kHz — a pole, so it moves as a power, not linearly.
	const double step = 1.0 - std::pow(kFeedbackPoleAtReference, scale);
	fbAlphaQ_ = static_cast<int32_t>(std::clamp(std::llround(step * 2147483648.0), 1LL, static_cast<long long>(kOneQ31)));
	// THE DEAD BAND OF THE WET HIGHPASS: where `mulhi(-dc, alpha)` is 0 the state stops moving. One past it.
	deadBand_ = static_cast<int32_t>(std::ceil(4294967296.0 / static_cast<double>(std::max(hpfAlphaQ_, 1)))) + 1;
	restFrames_ = std::max(1, static_cast<int>(std::lround(kRestSilentSeconds * sampleRate_)));
	inPeak_.prepare(sampleRate_);
	reset();
}

void SmoothShaper::reset() noexcept {
	block_ = Block {};
	coefficientsPlaced_ = false;
	smoothedDriveQ_ = INT32_MIN;
	driveGain_ = 1.0f;
	driveGainPlaced_ = false;
	lfoPhase_ = 0.0f;
	lfoLock_.release();
	dcState_[0] = dcState_[1] = 0;
	fbState_[0] = fbState_[1] = 0;
	probe_[0] = probe_[1] = ShapeProbe {};
	silentRun_ = 0;
	inPeak_.reset();
}

SmoothShaper::CoefficientSet SmoothShaper::coefficientsForTest() const noexcept {
	return smoothed_;
}

void SmoothShaper::adoptCoefficientsForTest(const CoefficientSet& c) noexcept {
	smoothed_ = c;
	coefficientsPlaced_ = true;
}

//--------------------------------------------------------------------------------------------------//
// THE PER-BUFFER SETUP — `sineShapeBuffer` down to its sample loop.                                   //
//--------------------------------------------------------------------------------------------------//

void SmoothShaper::beginBlock() noexcept {
	const FirmwareControls fc = firmwareControlsOf(target_);
	Block& b = block_;
	b.framesLeft = kBlockFrames;
	b.stride = 0;
	// "if mix is 0, do nothing": the buffer passes untouched and no state moves, the smoothers included.
	b.bypass = !(fc.mix > 0);
	if (b.bypass) {
		return;
	}

	const Zone zone = zoneOf(fc.harmonic);
	b.zone = zone.index;
	b.posInZone = zone.position;
	b.twist = twistOf(fc);
	const TwistParams& twist = b.twist;
	b.mix = fc.mix;
	b.invMix = kOneQ31 - fc.mix;

	// `prepareSmoothing(*smoothedDrive, drive)`, snapping on first use. Its `current` is read once, below.
	const int32_t driveCurrentQ = smoothedDriveQ_ == INT32_MIN ? fc.drive : smoothedDriveQ_;

	// POSITION STEREO: while Width is up and the LFO is POSITIVE, the two sides read the blend at two
	// positions. Read from the LFO phase as it stands, BEFORE a new voice's seeding below — the firmware's
	// order, so a new note's first block is never position-stereo.
	float stereoMod = 0.0f;
	const float stereoLfoValue = stereoLfoValueOf(lfoPhase_);
	if (twist.stereoWidth > 0.0f && stereoLfoValue > 0.0f) {
		stereoMod = stereoLfoValue;
	}

	// **THE TARGETS, FRESH EVERY BLOCK — THE FIRMWARE'S CACHE IS NOT REPRODUCED (fix it).**
	// The firmware reuses its last targets whenever position stereo is off and nothing in its key moved, and the
	// key omits the LFO's offset: after a positive half-cycle the two sides stayed a little apart, frozen, until
	// a control moved. Computed afresh, the offset is exactly what the LFO says this block, so the sides come back
	// together when it goes negative. The reference still has the cache, so the comparisons are exact everywhere
	// but the stereo passages with Width live, where the difference is bounded (`test_fx_smooth.cpp`).
	b.targets = targetsOf(b.zone, b.posInZone, twist, stereoMod);

	// ONE SMOOTHER PER INSTANCE, PLACED AT ITS TARGETS ON ITS FIRST BLOCK — the departure the header states.
	if (!coefficientsPlaced_) {
		smoothed_ = b.targets;
		coefficientsPlaced_ = true;
	}
	b.fmZone = b.zone >= 2 && b.zone <= 6;
	weights_[0] = { smoothed_[0], smoothed_[1], smoothed_[2], smoothed_[3], smoothed_[4] };
	weights_[1] = { smoothed_[0], smoothed_[5], smoothed_[6], smoothed_[7], smoothed_[8] };
	if (b.fmZone) {
		weights_[0].updateQ31();
		weights_[1].updateQ31();
	}
	// CONVERGED FOR THE WHOLE BLOCK, OR NOT: decided here, as the firmware decides it per buffer.
	b.converged = true;
	for (size_t k = 0; k < smoothed_.size(); ++k) {
		b.converged = b.converged && std::abs(smoothed_[k] - b.targets[k]) < kConvergenceEpsilon;
	}

	// THE STEREO LFO, at buffer rate: its value for the block, then the phase moved on by a block.
	float lfoPhase = lfoPhase_;
	// A NEW VOICE STARTS ITS LFO AT A PHASE ITS TWIST POSITION CHOOSES — "per-note variation".
	const bool isFirstUse = !driveGainPlaced_;
	if (isFirstUse && twist.stereoWidth > 0.0f) {
		lfoPhase = stereoLfoSeedOf(twist.phaseHarmonic);
	}
	float lfoPhaseIncrement = 0.0f;
	float driveStereoScale = 0.0f;
	if (twist.stereoWidth > 0.0f) {
		float lfoHz = kMinLfoHz + twist.stereoLfoRate * (kMaxLfoHz - kMinLfoHz);
		lfoHz *= twist.stereoFreqMult;
		// THE RATE: the firmware's `kSampleRate = 44100.0f`, here the host's.
		lfoPhaseIncrement = lfoHz / static_cast<float>(sampleRate_);
		driveStereoScale = twist.stereoWidth * kStereoDriveScale;
		// **THE BUS INSTANCE ON A PLAYING TRANSPORT FOLLOWS THE SONG** (backlog NM 2026-10-18: determinism
		// first): its phase is the song's seconds at the LFO's rate, so a reload or a play mid-song lands on it. A voice
		// is note-anchored instead (`setVoice`'s reason), and a stopped transport free-runs as the firmware does.
		if (!voice_ && clock_.playing && clock_.havePositionSeconds() && lfoHz > 0.0f) {
			const double at = clock_.songPositionSeconds + static_cast<double>(blockOffset_) / sampleRate_;
			const double turns = lfoLock_.follow(at, 1.0 / static_cast<double>(lfoHz),
			                                     static_cast<double>(kBlockFrames) / sampleRate_, 2);
			lfoPhase = static_cast<float>(turns - std::floor(turns));
		}
		else {
			lfoLock_.release();
		}
	}
	else {
		lfoLock_.release();
	}
	const float bufferLfoValue = stereoLfoValueOf(lfoPhase);
	const float driveStereoReduction = (bufferLfoValue < 0.0f) ? (-bufferLfoValue) * driveStereoScale : 0.0f;
	b.driveRScale = 1.0f - driveStereoReduction;
	lfoPhase += lfoPhaseIncrement * static_cast<float>(kBlockFrames);
	lfoPhase -= std::floor(lfoPhase);
	lfoPhase_ = lfoPhase;

	// THE FEEDBACK AMOUNT: Twist's, a quarter at most; in Poly less the further into the zone, and -3 dB;
	// and up to 10% less as the drive rises.
	constexpr float kFeedbackScale = 0.25f;
	float fbScale = kFeedbackScale;
	if (b.zone == kZones - 1) {
		fbScale *= (1.0f - b.posInZone);
		fbScale *= 0.707f;
	}
	const float driveNorm = static_cast<float>(fc.drive) / kOneQ31F;
	fbScale *= (1.0f - 0.1f * driveNorm);
	b.fbAmount = saturatingQ31(twist.feedbackAmount * fbScale * kOneQ31F);

	// THE DRIVE'S GLIDE: toward this block's target, per frame; a new voice starts where the last block's
	// target was (`computeDriveGain(driveCtx.current)`), which is this block's target on the very first.
	// **AND THE STACK TRIM** (law (b)): a stacked voice enters the shaper at a single voice's
	// loudness. Per voice only — the bus sums whole chords, where "a stacked voice" means nothing — and exactly 1 at
	// a stack of one, so every comparison with the firmware is untouched. In the TARGET, so it glides. ρ starts from the
	// parts' own 0-cent coherence, which the synth measures from the tables they play (`NoteContext::stackCoherence`).
	b.stackTrim = target_.poly ? stackTrimOf(note_.stackCount, note_.stackDetuneCents, note_.pitchSemitones, note_.stackCoherence)
	                            : 1.0f;
	b.targetDriveGain = driveGainOf(fc.drive) * b.stackTrim;
	if (!driveGainPlaced_) {
		driveGain_ = driveGainOf(driveCurrentQ) * b.stackTrim;
		driveGainPlaced_ = true;
	}
	smoothedDriveQ_ = fc.drive;
}

void SmoothShaper::advanceCoefficients() noexcept {
	// "Strided update (alpha already scaled 4x)": `current += (target - current) * alpha`, the scalar form
	// and the NEON `vmlaq` form being the same unfused arithmetic.
	for (size_t k = 0; k < smoothed_.size(); ++k) {
		smoothed_[k] += (block_.targets[k] - smoothed_[k]) * stridedAlpha_;
	}
	weights_[0] = { smoothed_[0], smoothed_[1], smoothed_[2], smoothed_[3], smoothed_[4] };
	weights_[1] = { smoothed_[0], smoothed_[5], smoothed_[6], smoothed_[7], smoothed_[8] };
	if (block_.fmZone) {
		weights_[0].updateQ31();
		weights_[1].updateQ31();
	}
}

//--------------------------------------------------------------------------------------------------//
// THE SAMPLE LOOP.                                                                                   //
//--------------------------------------------------------------------------------------------------//

void SmoothShaper::renderFrames(float* left, float* right, int frames) noexcept {
	// `left` AND `right` MAY BE THE SAME BUFFER — the mono road (`processMono`) hands one channel as both.
	// Both inputs are read before either output is written, and the LEFT is written last, so what survives
	// is the stereo road's left channel.
	Block& b = block_;
	const TwistParams& twist = b.twist;
	const bool fbHalf = fbAlphaQ_ == (1 << 30);
	float* const io[2] = { left, right };

	for (int i = 0; i < frames; ++i) {
		const float driveGains[2] = { driveGain_, driveGain_ * b.driveRScale };
		int32_t shaped[2] = {};
		int32_t dry[2] = {};
		for (int ch = 0; ch < 2; ++ch) {
			dry[ch] = toQ31(io[ch][i]);
		}
		// THE READING IS SANITISED LIKE THE SIGNAL: a NaN or an infinity counts as nothing, never a held peak.
		const float peakIn = std::max(std::fabs(io[0][i]), std::fabs(io[1][i]));
		inPeak_.push(std::isfinite(peakIn) ? peakIn : 0.0f);
		for (int ch = 0; ch < 2; ++ch) {
			int32_t input = dry[ch];
			if (b.fbAmount > 0) {
				// FEEDBACK INTO THE INPUT, before shaping; the tap is high-passed so the fundamental does not
				// build, and saturated at an eighth of full scale.
				const int32_t fb = saturate22(twice(mulhi(fbState_[ch], b.fbAmount)));
				input = addSaturate(input, fb);
				input = std::clamp(input, -kOneQ31, kOneQ31);
			}
			shaped[ch] = shapeSample(input, driveGains[ch], b.zone, weights_[ch], twist.evenAmount, twist.evenDryBlend,
			                         twist.rectAmount, &probe_[ch]);
		}

		driveGain_ += (b.targetDriveGain - driveGain_) * perSampleAlpha_;
		if (!b.converged) {
			if (++b.stride >= kSmoothingStride) {
				b.stride = 0;
				advanceCoefficients();
			}
		}

		int32_t out[2] = {};
		for (int ch = 0; ch < 2; ++ch) {
			// THE 100 Hz HIGHPASS ON THE WET ONLY: a lowpass, subtracted.
			dcState_[ch] = wadd(dcState_[ch], twice(mulhi(wsub(shaped[ch], dcState_[ch]), hpfAlphaQ_)));
			const int32_t hpfWet = wsub(shaped[ch], dcState_[ch]);
			// THE FEEDBACK TAP'S ONE-POLE: at 44.1 kHz the firmware's own `(fb + wet) >> 1`, wrap and all;
			// elsewhere the same pole restated for the rate.
			if (fbHalf) {
				fbState_[ch] = wadd(fbState_[ch], hpfWet) >> 1;
			}
			else {
				const int64_t delta = static_cast<int64_t>(hpfWet) - fbState_[ch];
				fbState_[ch] = static_cast<int32_t>(fbState_[ch] + ((delta * fbAlphaQ_) >> 31));
			}
			// WET AND DRY, IN q31 — the dry too, so a Mix short of full passes the dry through the same
			// quantisation the hardware's does.
			out[ch] = addSaturate(twice(mulhi(dry[ch], b.invMix)), twice(mulhi(hpfWet, b.mix)));
		}

		// **THE REST — NOT THE FIRMWARE'S, AND NEEDED BECAUSE THE PLUGIN SLEEPS ONLY ON EXACT SILENCE.** The wet
		// highpass is a q31 one-pole whose update `mulhi(shaped - dc, alpha)` FLOORS: with silence in, a
		// positive state walks down to 0 or -1, but a NEGATIVE one inside the dead band (|dc| < 2^32 / alpha,
		// ~141 steps at 44.1 kHz) updates by exactly 0 and stays, and the wet is then `0 - dc` — a constant of up
		// to ~140 q31 steps, about -100 dB, for ever, with the feedback tap settled on it too. On the reference a
		// voice's state goes with the voice; here the bus instance, and a per-voice one between notes, would
		// keep the plugin awake for ever. CLIP's rest (`clip.cpp`, `kRestSilentSeconds`), gated for THIS
		// arithmetic: after `kRestSilentSeconds` of EXACTLY-zero input, and with both integer states inside
		// the dead band, they are cleared and the output is exact zero until the input moves.
		//
		// **GATED ON THE STATES, NOT ON |y| < 1e-9 AS CLIP IS**, because the output here is quantised to q31
		// steps of 2^-24 ≈ 6e-8: "under 1e-9" would mean "already zero", and the stuck residue is by
		// construction at least one step. Inside the dead band |y| ≤ deadBand · 2^-24 (8.4e-6 at 44.1 kHz).
		// It cannot cut a signal: any non-zero input — 1e-6 is 17 q31 steps — restarts the count, and 50 ms
		// after the last one every linear tail here (the 100 Hz highpass, a 0.5 pole) is some e^-30 down, so
		// what the rest removes is the residue and nothing else. A self-oscillating feedback loop (Fold and
		// Mod, Twist in its feedback zone) holds its states far outside the band, so it keeps sounding and
		// keeps the plugin awake — which is correct, and by design.
		if (dry[0] == 0 && dry[1] == 0) {
			silentRun_ = std::min(silentRun_ + 1, restFrames_);
		}
		else {
			silentRun_ = 0;
		}
		if (silentRun_ >= restFrames_) {
			bool inBand = true;
			for (int ch = 0; ch < 2; ++ch) {
				inBand = inBand && std::abs(static_cast<int64_t>(dcState_[ch])) <= deadBand_
				         && std::abs(static_cast<int64_t>(fbState_[ch])) <= deadBand_;
			}
			if (inBand) {
				dcState_[0] = dcState_[1] = 0;
				fbState_[0] = fbState_[1] = 0;
				out[0] = out[1] = 0;
			}
		}

		// RIGHT, THEN LEFT: see the top of this function.
		io[1][i] = fromQ31(out[1]);
		io[0][i] = fromQ31(out[0]);
	}
}

void SmoothShaper::processStereo(float* left, float* right, int frames) noexcept {
	if (left == nullptr || right == nullptr || frames <= 0 || !target_.enabled) {
		return;
	}
	// **THE SETUP RUNS ON AN ABSOLUTE GRID, CARRIED ACROSS CALLS** — the eroder's lesson: a setup once per
	// CALL, as the firmware's is once per buffer, makes where the host cuts its blocks decide how far each
	// glide got and how long each target held.
	int done = 0;
	while (done < frames) {
		if (block_.framesLeft <= 0) {
			blockOffset_ = done;
			beginBlock();
		}
		const int take = std::min(frames - done, block_.framesLeft);
		if (!block_.bypass) {
			renderFrames(left + done, right + done, take);
		}
		block_.framesLeft -= take;
		done += take;
	}
	// THE READINGS, once per call — only the bus instance has a sink (see `Reading`).
	if (sink_.publish != nullptr) {
		const float values[static_cast<size_t>(Reading::kCount)] = { inPeak_.reading() };
		sink_.publish(sink_.context, values, static_cast<size_t>(Reading::kCount));
	}
}

void SmoothShaper::processMono(float* mono, int frames) noexcept {
	// **THE STEREO ROAD'S LEFT CHANNEL, NOT THE FIRMWARE'S MONO OVERLOAD — by design.** The house
	// contract (`test_fx_devices.cpp`: a mono road is the stereo road's left, sample for sample) is what keeps a
	// note's timbre from changing when the character bank's stereo window opens or shuts mid-note. The
	// firmware's mono `sineShapeBuffer` differs in the Width and meta zones — no stereo LFO, so no position
	// offset on the left — and is not reproduced here.
	processStereo(mono, mono, frames);
}

//--------------------------------------------------------------------------------------------------//
// THE OPS TABLE.                                                                                     //
//--------------------------------------------------------------------------------------------------//

namespace {
SmoothShaper& asShaper(void* storage) {
	return *static_cast<SmoothShaper*>(storage);
}
} // namespace

const device::AudioOps kAudioOps {
    .stateBytes = sizeof(SmoothShaper),
    .stateAlign = alignof(SmoothShaper),
    .construct = [](void* storage) { new (storage) SmoothShaper(); },
    .destruct = [](void* storage) { asShaper(storage).~SmoothShaper(); },
    .prepare = [](void* storage, double sampleRate, int) { asShaper(storage).prepare(sampleRate); },
    .reset = [](void* storage) { asShaper(storage).reset(); },
    .setFields =
        [](void* storage, const float* fields, size_t fieldsPresent) {
	        Row row {};
	        const size_t take = std::min(fieldsPresent, row.size());
	        for (size_t i = 0; i < take; ++i) {
		        row[i] = fields[i];
	        }
	        asShaper(storage).setTarget(stateOf(row, take).settings);
        },
    .setNoteContext = [](void* storage, const device::NoteContext& note) { asShaper(storage).setNoteContext(note); },
    .setTransportClock = [](void* storage,
                            const device::TransportClock& clock) { asShaper(storage).setTransportClock(clock); },
    .processStereo =
        [](void* storage, float* left, float* right, int frames) {
	        asShaper(storage).processStereo(left, right, frames);
        },
    // THE MONO ROAD, DECLARED: a voice renders into one buffer whenever the character bank's stereo window is
    // shut. It is the stereo road's left channel (by design, see `processMono`), NOT the firmware's mono overload.
    .processMono = [](void* storage, float* mono, int frames) { asShaper(storage).processMono(mono, frames); },
    .setReadingsSink = [](void* storage, device::ReadingsSink sink) { asShaper(storage).setReadingsSink(sink); },
    .setVoice = [](void* storage, size_t) { asShaper(storage).setVoice(); },
};

} // namespace feathers::fx::smooth
