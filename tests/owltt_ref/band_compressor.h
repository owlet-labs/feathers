/*
 * Copyright © 2024-2025 Owlet Records
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
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "owltt_ref/fast_math.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

// BandCompressor — single-band upward+downward compressor (OTT-style), ported from the firmware's
// multiband.h.
//
// THE PORT: the firmware version stores knobs as q31_t and takes q31_t audio buffers for level
// detection. This version uses float throughout (knobs 0..1, audio float). The gain calculation is
// IDENTICAL — same thresholds, same envelope follower, same soft-knee math. Only the arithmetic is
// restated.
//
// The compressor has two independent paths:
//   - Downward compression: attenuates signals above thresholdDown (conventional compression)
//   - Upward compression: amplifies signals below thresholdUp (OTT-style)
// The skew control balances between them (-1 = upward only, +1 = downward only).
//
// Per-band values in the full OWLTT are OFFSETS from linked globals (see the brief). This class
// handles the per-band DSP; the offset application happens in the MultibandCompressor that owns it.

namespace feathers::fx::owltt_ref {

class BandCompressor {
public:
	BandCompressor() = default;

	/// **THE LOOKAHEAD WINDOW (backlog ND, option 3)**: with the audio held back `samples`, the detector's rise
	/// and the envelope's attack are capped to a time constant of `samples / kLookaheadSettle`, so the gain is down
	/// before the onset reaches it, as a lookahead limiter's is. Never slower than the Attack and the detector already
	/// are: the cap only speeds them. 0 removes it, and with it this function's whole effect, to the bit.
	static constexpr float kLookaheadSettle = 6.0f;
	/// HOW FAR ABOVE THE LEVEL A SAMPLE MUST BE FOR THE FAST RISE — an ONSET, not the ripple of a held sound: a sine's peak
	/// is 1.57x its mean, under this, so a steady signal is detected exactly as without a lookahead.
	static constexpr float kLookaheadRiseRatio = 2.0f;
	void setLookaheadWindow(float samples) {
		riseTau_ = samples > 0.0f ? samples / kLookaheadSettle : 0.0f;
		attackCap_ = riseTau_ > 0.0f ? -1.0f / riseTau_ : 0.0f;
	}

	void setAttack(float attack) {
		attackKnob_ = attack;
		attackMS_ = 0.5f + (fastExp(2.0f * attack) - 1.0f) * 15.0f;
		attack_ = (-1000.0f / sampleRate_) / attackMS_;
	}

	void setRelease(float release) {
		releaseKnob_ = release;
		releaseMS_ = 5.0f + (fastExp(2.0f * release) - 1.0f) * 75.0f;
		release_ = (-1000.0f / sampleRate_) / releaseMS_;
	}

	/// HOW FAR DOWN FROM FULL SCALE THE THRESHOLD KNOB REACHES, in the detector's units.
	///
	/// `rms_` is `log(level * 2^31)` -- a NATURAL log of amplitude, so one unit is one neper and
	/// 8.6859 dB. 60 dB is the window a multiband compressor is actually used across; the 149 dB this
	/// replaces was not a choice, it was what a fraction-of-full-scale mapping happened to produce.
	static constexpr float kNepersPerDb = 1.0f / 8.6859f;
	static constexpr float kThresholdSpanNepers = 60.0f * kNepersPerDb;

	void setThresholdDown(float t) {
		thresholdDownKnob_ = t;
		thresholdDown_ = 0.2f + 0.8f * t;
		// AND THE UP THRESHOLD FOLLOWS IT, because it is defined RELATIVE to this one. It used to be
		// left where the last `setBandwidth` put it, so moving the threshold slid the gap out from
		// under the bandwidth that set it -- two numbers that had to agree, kept in step by call
		// order. `Owltt::update` happens to call them in the right sequence; nothing made it.
		setBandwidth(bandwidthKnob_);
	}

	// `setThresholdUp` IS GONE. Nothing called it: the up threshold is DEFINED by `setBandwidth` relative
	// to the down one, and the knob field the setter wrote was read only by the `getThresholdUp` seam —
	// which therefore reported 0 forever while the band compared against `thresholdUp_` (OWLTT parity
	// sweep, 2026-09-16; backlog EH shape 9, a field only a dead road wrote).

	void setRatioDown(float r) {
		ratioDownKnob_ = r;
		fractionDown_ = r;
	}

	void setRatioUp(float r) {
		ratioUpKnob_ = r;
		fractionUp_ = r * r * r; // x^3 curve for balanced feel
	}

	void setOutputLevel(float g) {
		outputLevelKnob_ = g;
		if (g <= 0.5f) {
			outputLevel_ = g * 2.0f;
		}
		else {
			outputLevel_ = 1.0f + (g - 0.5f) * 2.0f * 9.0f;
		}
	}

	void setBandwidth(float bw) {
		bandwidthKnob_ = bw;
		const float bandwidthFraction = 0.6f * bw;
		// BELOW THE DOWN THRESHOLD, NOT ABOVE IT — and this is a behaviour change, not a sign fix.
		//
		// It was `min(1, thresholdDown + fraction)`, which puts the UP threshold ABOVE the DOWN one.
		// Both `over` and `under` are then positive between them, so the band between the two bars is
		// where BOTH paths engage and partially cancel, and outside it exactly one acts. That is
		// backwards for this kind of device: an OTT-style compressor puts the upward threshold BELOW,
		// so the region between is TRANSPARENT.
		//
		// AND THE CARD HAS BEEN DRAWING IT AS TRANSPARENT ALL ALONG — *"the dead zone (region between
		// bars)"*, tinted, in `OwlttGraph::draw`. So the picture described the intent and the DSP did
		// the opposite. Asked rather than assumed: is overlapping upward and downward compression normal
		// for this kind of device? It is not, and this is what it should have been.
		//
		// MEASURED CONSEQUENCE OF THE OLD ORDER: bandwidth moved the sound by 0.1 dB across its whole
		// 0..1 range, because widening a region where two opposed gains cancel changes almost
		// nothing. The control that the entire two-bar picture is about did essentially nothing.
		thresholdUp_ = std::max(0.2f, thresholdDown_ - bandwidthFraction);
	}

	[[nodiscard]] float getAttack() const { return attackKnob_; }
	[[nodiscard]] float getRelease() const { return releaseKnob_; }
	[[nodiscard]] float getAttackMS() const { return attackMS_; }
	[[nodiscard]] float getReleaseMS() const { return releaseMS_; }
	[[nodiscard]] float getThresholdDown() const { return thresholdDownKnob_; }
	/// THE UP THRESHOLD THE BAND COMPARES AGAINST, in the same knob units as `getThresholdDown` —
	/// inverted from the internal `0.2 + 0.8 * t` form so the two seams read on one scale. Read off
	/// the member `calculateGain` uses, not a knob nothing sets.
	[[nodiscard]] float getThresholdUp() const { return (thresholdUp_ - 0.2f) / 0.8f; }
	[[nodiscard]] float getRatioDown() const { return ratioDownKnob_; }
	[[nodiscard]] float getRatioUp() const { return ratioUpKnob_; }
	[[nodiscard]] float getOutputLevel() const { return outputLevelKnob_; }
	[[nodiscard]] float getBandwidth() const { return bandwidthKnob_; }
	[[nodiscard]] float getOutputLevelLinear() const { return outputLevel_; }

	[[nodiscard]] float getBandwidthForDisplay() const {
		float bandwidthFraction = 0.6f * bandwidthKnob_;
		return bandwidthFraction * 60.0f;
	}

	[[nodiscard]] float getThresholdForDisplay() const {
		constexpr float minDB = -60.0f;
		constexpr float maxDB = -12.0f;
		float normalized = (thresholdDown_ - 0.2f) / 0.8f;
		return minDB + normalized * (maxDB - minDB);
	}

	[[nodiscard]] float getRatioForDisplay() const {
		if (fractionDown_ >= 0.99f) {
			return 100.0f;
		}
		return 1.0f / (1.0f - fractionDown_);
	}

	void reset() {
		envelope_ = 0.0f;
		rms_ = 0.0f;
		level_ = 0.0f;
		lastFrameCount_ = 0;
	}

	/// Calculate gain adjustment for the band based on current RMS level.
	[[nodiscard]] float calculateGain(float numSamples, float songVolumedB, float knee = 0.0f, float skew = 0.0f,
	                                  uint32_t frameCount = 0) {
		uint32_t previousFrameCount = lastFrameCount_;
		lastFrameCount_ = frameCount;
		bool gapDetected = (frameCount > 0) && (previousFrameCount > 0) && (frameCount > previousFrameCount + 1);

		// THE THRESHOLD SPANS A USEFUL WINDOW, rather than 149 dB of which 100 are unreachable.
		//
		// This was `songVolumedB * thresholdDown_`. In the firmware `songVolumedB` is the SONG's
		// current volume, so that product is a threshold RELATIVE to how loud the material is. We
		// pass `kThresholdRefdB` -- a constant 21.49, which is log(2^31), i.e. full scale -- so the
		// product became a fraction of FULL SCALE instead, and `thresholdDown_` running 0.2..1.0
		// mapped the knob across 0.2*21.49 to 21.49 nepers. In dB that is -149 dBFS to 0 dBFS.
		//
		// MEASURED, WHICH IS HOW THE SIZE OF IT BECAME CLEAR: the bottom HALF of the threshold knob
		// did nothing at all -- 0.0 and 0.25 and 0.5 all produced -20.1 dB of reduction, because every
		// one of them sits so far below the signal that everything is over the threshold. The default
		// of 0.5 lands at -74.7 dBFS. A knob whose usable travel is its top third is a knob that is
		// mostly decoration.
		//
		// SO THE KNOB SPANS 60 dB DOWN FROM FULL SCALE, measured in the detector's own units. That
		// keeps `bandwidth` in the same units -- it is a distance in this space, and 0.6 of it is
		// about 36 dB of gap at the extreme -- and it keeps the threshold ABSOLUTE, which is what a
		// plugin wants where the firmware could ask the song.
		const float threshDowndB = songVolumedB - (1.0f - thresholdDown_) * kThresholdSpanNepers;
		const float threshUpdB = songVolumedB - (1.0f - thresholdUp_) * kThresholdSpanNepers;

		float diffDown = rms_ - threshDowndB;
		float diffUp = threshUpdB - rms_;

		float kneeWidthdB = knee * 12.0f;
		float halfKnee = kneeWidthdB * 0.5f;
		float invTwoKnee = (kneeWidthdB > 0.1f) ? (0.5f / kneeWidthdB) : 0.0f;

		float over;
		if (kneeWidthdB < 0.1f || diffDown > halfKnee) {
			over = std::max(0.0f, diffDown);
		}
		else if (diffDown > -halfKnee) {
			float x = diffDown + halfKnee;
			over = x * x * invTwoKnee;
		}
		else {
			over = 0.0f;
		}

		float under;
		if (kneeWidthdB < 0.1f || diffUp > halfKnee) {
			under = std::max(0.0f, diffUp);
		}
		else if (diffUp > -halfKnee) {
			float x = diffUp + halfKnee;
			under = x * x * invTwoKnee;
		}
		else {
			under = 0.0f;
		}

		float upwardFactor = std::clamp(1.0f - skew, 0.0f, 1.0f);
		float downwardFactor = std::clamp(1.0f + skew, 0.0f, 1.0f);

		float target = -over * fractionDown_ * downwardFactor + under * fractionUp_ * upwardFactor;

		if (gapDetected) {
			uint32_t gapFrames = frameCount - previousFrameCount - 1;
			float gapSamples = static_cast<float>(gapFrames) * numSamples;
			envelope_ = envelope_ * fastExp(release_ * gapSamples);
			level_ = 0.0f;
			rms_ = 0.0f;
		}
		else {
			envelope_ = runEnvelope(envelope_, target, numSamples);
		}

		float gain = fastExp(envelope_);
		return std::clamp(gain, 0.1f, 31.6f);
	}

	/// Update the level from float band buffers (ported from NEON q31 peak detection).
	///
	/// **THE DETECTOR ADVANCES PER SAMPLE, NOT ONCE PER CALL — "re: the ott window
	/// and attack: fix them. firmware isnt sacred on that."**
	///
	/// IT USED TO TAKE THE PEAK OVER THE WHOLE BLOCK AND THEN RUN ONE IIR STEP. Two things followed
	/// and both were defects rather than character. The gain could not change more than once a block,
	/// so the detector's resolution WAS the host's buffer length — and a peak taken over a longer
	/// window is a bigger peak, so the device's whole response tracked a number the player cannot
	/// see. MEASURED through the plugin before the change: the same four-event script differed by
	/// 0.0314169 worst against a 4.90306 peak across host blocks 256/64/100/441, and replacing the
	/// window with per-sample detection collapsed that to 0.00370073 — 0.07%.
	///
	/// **SAME DETECTOR, SAMPLED FINER.** The coefficient is the one the caller passes, rescaled from
	/// its reference block to the stride, so `Character` still means what it meant; `kPeakStride`
	/// still means what it meant (bass looks every fourth sample); and a call of N samples now costs
	/// N/stride IIR steps instead of one. What is gone is the block boundary, and nothing else.
	void updateLevel(const float* bufferL, const float* bufferR, size_t numSamples, float alpha, float oneMinusAlpha,
	                 bool useAvg, int32_t stride = 1) {
		(void)oneMinusAlpha; // the per-call coefficient's partner; this advances per step instead
		const auto hop = static_cast<size_t>(stride < 1 ? 1 : stride);
		// THE PER-STEP COEFFICIENT. `alpha` arrives scaled to `numSamples` by the caller — see
		// `Owltt::processStereoSlice` — so undoing that and rescaling to one hop is what keeps the
		// detector's time constant a duration rather than a count of calls.
		const double perCall = std::clamp(static_cast<double>(alpha), 0.0, 1.0);
		const double steps = static_cast<double>(numSamples) / static_cast<double>(hop);
		const float perStep =
		    steps > 0.0 ? static_cast<float>(1.0 - std::pow(1.0 - perCall, 1.0 / steps)) : 0.0f;
		const float keep = 1.0f - perStep;

		if (riseTau_ > 0.0f) {
			// THE LOOKAHEAD'S DETECTOR (`setLookaheadWindow`): an ONSET (a sample over `kLookaheadRiseRatio` x the level) is
			// followed at least as fast as the window
			// allows; anything else exactly as without it, so a held sound is not read louder.
			const float rise = std::max(perStep, 1.0f - std::exp(-static_cast<float>(hop) / riseTau_));
			for (size_t i = 0; i < numSamples; i += hop) {
				const float L = std::abs(bufferL[i]);
				const float R = std::abs(bufferR[i]);
				const float s = useAvg ? ((L + R) * 0.5f) : std::max(L, R);
				level_ = s > kLookaheadRiseRatio * level_ ? level_ + (s - level_) * rise : level_ * keep + s * perStep;
			}
		}
		else {
			for (size_t i = 0; i < numSamples; i += hop) {
				const float L = std::abs(bufferL[i]);
				const float R = std::abs(bufferR[i]);
				const float s = useAvg ? ((L + R) * 0.5f) : std::max(L, R);
				level_ = level_ * keep + s * perStep;
			}
		}

		// SCALE TO Q31 RANGE before taking the log. The firmware's peak is q31_t (0..2^31), so its
		// log(level_) is in 0..21.5. Our float peak is 0..1, so we scale by 2^31 to match.
		constexpr float kQ31Scale = 2147483648.0f; // 2^31
		rms_ = fastLog(std::max(level_ * kQ31Scale, 1.0f));
	}

	/// Get current gain reduction in dB (for metering). Test-only accessor: the OTT display renders this.
	[[nodiscard]] float getGainReductionDB() const { return envelope_; }

	/// Get current input level in log domain (for metering).
	[[nodiscard]] float getInputLevelLog() const { return rms_; }

	/// **REPLACE THE LEVEL THE NEXT `calculateGain` READS** (backlog LQ) — the stereo link's road in. A
	/// channel's compressor detects its own channel with `updateLevel`, and OWLTT then blends that level
	/// toward the band's shared one and hands the blend back here. Only the log level is replaced: the
	/// smoothed linear level `updateLevel` carries is the channel's own and keeps running.
	void setInputLevelLog(float level) { rms_ = level; }

	/// Get post-level peak (for band level metering). Firmware tracks `bandPeakThisBuffer`.
	[[nodiscard]] float getBandOutputPeak() const { return bandOutputPeak_; }

	/// Update post-gain peak tracking (called from the render loop after gain application).
	void updatePostGainPeak(float peak) { bandOutputPeakThisBuffer_ = std::max(bandOutputPeakThisBuffer_, peak); }

	/// **DECAY THE PER-BAND OUTPUT PEAK, AT A RATE IN SAMPLES RATHER THAN IN CALLS.**
	///
	/// The firmware writes `bandOutputPeak_ = max(bandPeakThisBuffer, bandOutputPeak_ * 0.95)` once per
	/// buffer, and on hardware with ONE block size known at design time that is a rate. On desktop it
	/// is not: the host chooses the block, so a meter falling 0.95 per call has a half-life of **9 ms
	/// at a 32-sample block and 148 ms at 512** — sixteen times slower for the same signal, decided by
	/// a number in the user's audio settings.
	///
	/// It is a METER and not audio, which is why it is a smaller finding than the detector next door
	/// and not a smaller defect: the OTT display's ballistics are what a player reads the compressor's
	/// behaviour off, and they should not change when they change their buffer.
	///
	/// `kDecayReferenceFrames` KEEPS TODAY'S BEHAVIOUR AT 256 SAMPLES, the same reference the gain
	/// smoother and the detector use, so all three of this device's ported per-block rates agree about
	/// what "today" means.
	void decayBandOutputPeak(size_t numSamples) {
		static constexpr double kDecayPerReference = 0.95;
		static constexpr double kDecayReferenceFrames = 256.0;
		const double keep = std::pow(kDecayPerReference,
		                             static_cast<double>(numSamples) / kDecayReferenceFrames);
		bandOutputPeak_ =
		    std::max(bandOutputPeakThisBuffer_, bandOutputPeak_ * static_cast<float>(keep));
		bandOutputPeakThisBuffer_ = 0.0f;
	}

	void prepare(double sampleRate) { sampleRate_ = sampleRate; }

private:
	/// **ATTACK IS THE GAIN GOING DOWN. RELEASE IS THE GAIN COMING BACK.**
	///
	/// THIS USED TO ASK `|target| > |current|` — "is the gain moving away from unity" — and that is a
	/// different question with the same answer most of the time, which is why it survived. It differs
	/// in exactly one place, and that place is the first note after a rest.
	///
	/// **THE UPWARD EXPANDER SITS AT +18 dB ON SILENCE** (measured at the shipped threshold 0.5 and
	/// ratio 0.5; +30 dB at ratio 0.8). So when a note arrives, the gain has to travel DOWN through
	/// unity to reach its compressed value — and every step of that descent is "toward unity", which
	/// the old test classified as a RELEASE. The attack knob was not slow; it was not connected.
	///
	/// MEASURED, time for band 0's gain to reach half its eventual reduction after silence, at the
	/// shipped defaults and a 32-sample block:
	///
	///                       before            after
	///     attack knob 0     17.3 ms           2.0 ms
	///     attack knob 0.5   18.0 ms          10.7 ms
	///     attack knob 1.0   20.7 ms          30.0 ms
	///     release knob 0     4.0 ms           2.0 ms
	///     release knob 0.5  38.0 ms           2.0 ms
	///     release knob 1.0 122.7 ms           4.0 ms
	///
	/// **The RELEASE knob moved the attack time by 31x and the ATTACK knob by 1.2x. They have swapped
	/// back.** Even at minimum attack OWLTT felt slow on attack — 17.3 ms against a control reading
	/// 0.5 ms, and no setting of that control
	/// could fix it.
	///
	/// WHAT IS NOT FIXED HERE, and it is a separate question: the +18 dB itself. A device that lifts
	/// silence by eighteen decibels has to claw it back on every entry, and whether the upward
	/// expander should reach that far with nothing to expand is a design call rather than a defect.
	/// The attack is now the attack whatever that number is.
	[[nodiscard]] float runEnvelope(float current, float target, float numSamples) const {
		const bool reducing = target < current;
		// UNDER A LOOKAHEAD the attack is no slower than the window allows (`attackCap_` is 0 without one, and the
		// attack is always negative, so `min` is then the attack itself).
		float timeConstant = reducing ? std::min(attack_, attackCap_) : release_;
		return target + fastExp(timeConstant * numSamples) * (current - target);
	}

	double sampleRate_ = 48000.0;

	float attack_ = -1000.0f / 48000.0f;
	float release_ = -1000.0f / 48000.0f;
	float attackMS_ = 1.0f;
	/// BACKLOG ND: the lookahead's caps — see `setLookaheadWindow`. 0 is none.
	float riseTau_ = 0.0f;
	float attackCap_ = 0.0f;
	float releaseMS_ = 100.0f;
	float thresholdDown_ = 0.8f;
	float thresholdUp_ = 0.5f;
	float fractionDown_ = 0.5f;
	float fractionUp_ = 0.5f;
	float envelope_ = 0.0f;
	float rms_ = 0.0f;
	float level_ = 0.0f;
	float bandOutputPeak_ = 0.0f; ///< Post-level peak, decayed per buffer (firmware: bandOutputPeak_)
	float bandOutputPeakThisBuffer_ = 0.0f; ///< Peak within current buffer (firmware: bandPeakThisBuffer)
	uint32_t lastFrameCount_ = 0;

	float attackKnob_ = 0.25f;
	float releaseKnob_ = 0.25f;
	float thresholdDownKnob_ = 0.0f;
	float ratioDownKnob_ = 0.0f;
	float ratioUpKnob_ = 0.0f;
	float outputLevelKnob_ = 0.5f;
	float bandwidthKnob_ = 0.5f;
	float outputLevel_ = 1.0f;
};

} // namespace feathers::fx::owltt_ref
