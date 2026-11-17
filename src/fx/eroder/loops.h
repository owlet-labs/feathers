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

#include "fx/eroder/eroder_types.h"
#include "fx/eroder/shell.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace feathers::fx::eroder {

//--------------------------------------------------------------------------------------------------//
// THE SHARED SECTION: EMPHASIS, SMEAR AND TWO FEEDBACK LOOPS, THE SAME FOR EVERY ENGINE.              //
//--------------------------------------------------------------------------------------------------//
//
//   in -> [emphasis in] -> (+) -> [Smear] -> ENGINE -> [Drive] -> [emphasis out] -> [HP Wet] -> wet out
//                           ^                   |          |
//                           |   inner Feedback: tap C      tap D      (C = Clean, D = Driven)
//                           |   outer Loop:                tap D -> outer delay -> limiter
//                           +------- both loops return here
//
// "In Loop" moves the emphasis-in to just after the (+), so each trip round a loop goes through it. Emphasis out is after
// both taps, always. Everything here is derived on the control grid (`set`), the gains glide a sample at a time, and
// at the defaults no stage runs, so an engine's signal is the bits it was.

/// **A STEREO DELAY LINE IN FRAMES, read at a fractional distance**: `read(k)` is what was written `k` writes ago (`k` of
/// one is the latest). Allocated in `prepare`; cleared, only over what was written, when its loop goes idle.
class LoopDelay {
public:
	void prepare(double sampleRate, double seconds) {
		capacity_ = static_cast<size_t>(std::max(sampleRate * seconds, 16.0)) + 8u;
		data_.assign(capacity_ * 2u, 0.0f);
		reset();
	}
	void reset() noexcept {
		if (filled_ > 0) {
			std::fill(data_.begin(), data_.begin() + static_cast<std::ptrdiff_t>(filled_ * 2u), 0.0f);
		}
		filled_ = 0;
		pos_ = 0;
		sinceSound_ = capacity_;
	}
	void write(float left, float right) noexcept {
		if (capacity_ == 0) {
			return;
		}
		data_[pos_ * 2u] = left;
		data_[pos_ * 2u + 1u] = right;
		if (++pos_ >= capacity_) {
			pos_ = 0;
		}
		filled_ = std::min(capacity_, filled_ + 1u);
		if (left != 0.0f || right != 0.0f) {
			sinceSound_ = 0;
		}
		else if (sinceSound_ < capacity_) {
			++sinceSound_;
		}
	}
	/// What was written `k` writes ago, `k` in [1, capacity - 2], linearly interpolated toward the older frame.
	void read(double k, float& left, float& right) const noexcept {
		if (capacity_ < 4) {
			left = right = 0.0f;
			return;
		}
		k = std::clamp(k, 1.0, static_cast<double>(capacity_ - 2u));
		const double whole = std::floor(k);
		const auto frac = static_cast<float>(k - whole);
		const auto n = static_cast<size_t>(whole);
		const size_t i0 = (pos_ + capacity_ - n) % capacity_;
		const size_t i1 = (i0 + capacity_ - 1u) % capacity_;
		left = data_[i0 * 2u];
		right = data_[i0 * 2u + 1u];
		if (frac != 0.0f) {
			left += (data_[i1 * 2u] - left) * frac;
			right += (data_[i1 * 2u + 1u] - right) * frac;
		}
	}
	[[nodiscard]] bool used() const noexcept { return filled_ > 0; }
	[[nodiscard]] bool holdsSound() const noexcept { return filled_ > 0 && sinceSound_ < capacity_; }
	[[nodiscard]] size_t capacity() const noexcept { return capacity_; }

private:
	std::vector<float> data_;
	size_t capacity_ = 0;
	size_t pos_ = 0;
	size_t filled_ = 0;
	size_t sinceSound_ = 0;
};

/// THE SILENT-INPUT RULES (`SharedSection::inputFrame`): how long the input is zero before the gate may close, before the fade
/// starts, the fade's time constant, and the gate's level under the limiter's threshold (-120 dB).
inline constexpr double kLoopGateSeconds = 0.02;
inline constexpr double kLoopFadeStartSeconds = 0.5;
inline constexpr double kLoopFadeSeconds = 0.08;
inline constexpr float kLoopGateRelative = 1.0e-6f;

class SharedSection {
public:
	/// ALLOCATE. Activate-time only. `withInner` gives the section the inner loop's own delay (Rust has no line of its own).
	void prepare(double sampleRate, bool withInner) {
		rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		emphasis_.prepare(rate_);
		smear_.prepare(rate_);
		outer_.prepare(rate_, kLoopLineSeconds);
		if (withInner) {
			inner_.prepare(rate_, kLoopLineSeconds);
		}
		innerLimiter_.prepare(rate_);
		outerLimiter_.prepare(rate_);
		gateFrames_ = static_cast<uint32_t>(kLoopGateSeconds * rate_);
		fadeStartFrames_ = static_cast<uint32_t>(kLoopFadeStartSeconds * rate_);
		fadeStep_ = static_cast<float>(std::exp(-1.0 / (kLoopFadeSeconds * rate_)));
		smoothing_ = static_cast<float>(1.0 - std::exp(-1.0 / (kDustSmoothingSeconds * rate_)));
		reset();
	}

	void reset() noexcept {
		emphasis_.reset();
		smear_.reset();
		outer_.reset();
		inner_.reset();
		innerLimiter_.reset();
		outerLimiter_.reset();
		outerGain_ = 0.0f;
		placed_ = false;
		quietFrames_ = 0;
		loopScale_ = 1.0f;
		gated_ = false;
	}

	/// THE FULL SCALE OF THE DOMAIN THE ENGINE WORKS IN: both limiters hold the fed-back level at or under it.
	void setThreshold(float threshold) noexcept {
		threshold_ = std::max(threshold, 1.0e-6f);
		applyThreshold();
	}

	/// ON THE CONTROL GRID. `centreHz` is the engine's Freq: the emphasis's pivot or centre, Smear's centre, and the period
	/// the tuned delays are made of.
	void set(const Settings& s, const LoopDerived& loops, double centreHz) noexcept {
		EmphasisSettings e;
		e.shape = s.emphBell ? EmphasisShape::Bell : EmphasisShape::Tilt;
		e.amountDb = s.emphAmount;
		e.hz = static_cast<float>(emphasisHz(centreHz, s.emphOffset, rate_));
		e.widthOctaves = static_cast<float>(emphasisWidthOctaves(s.width));
		e.restore = s.emphRestore;
		emphasis_.set(e);
		SmearSettings m;
		m.amount = s.smear;
		m.hz = static_cast<float>(centreHz);
		smear_.set(m);
		loops_ = loops;
		limitDivisor_ = pathGainAfterReturn(s, loops.inLoop);
		applyThreshold();
		// THE TUNED DELAYS ARE AS SET: a period of Freq, and that moved by Interval. The Smear's own delay (it is longer than
		// a period at its centre) simply adds inside the loop, so the first return is not earlier than the tuned delay.
		const double limit = static_cast<double>(outer_.capacity()) - 3.0;
		innerFrames_ = std::clamp(loops.periodSeconds * rate_, 2.0, limit);
		outerFrames_ = std::clamp(loops.outerSeconds * rate_, 2.0, limit);
		outerTarget_ = static_cast<float>(loops.loopGain);
		// THE FIRST BLOCK SNAPS the gain to its control, as the engines' own gains do.
		if (!placed_) {
			outerGain_ = outerTarget_;
			placed_ = true;
		}
	}

	//----------------------------------------------------------------------------------------------//
	// PER FRAME                                                                                      //
	//----------------------------------------------------------------------------------------------//

	/// **WHAT A SILENT INPUT DOES TO THE LOOPS. Call once a frame, with whether the device's input is exactly zero.** Two things,
	/// both counted in frames, so any host block length gives the same bits.
	///  - THE GATE: the input has been zero for `kLoopGateSeconds` and both loops' limiter envelopes are under -120 dB of the
	///    threshold, so what circulates is nothing: both returns are forced to zero until the input comes back. (Without it a
	///    loop whose gain sits at one, through a curve that is steeper away from zero than at it, can hover just over the
	///    flush floor for ever.)
	///  - THE FADE: with the loops now allowed to total more than one, a limiter-held self-oscillation has no gain to die by.
	///    So once the input has been zero for `kLoopFadeStartSeconds`, both loops' gains are scaled by a factor that falls
	///    exponentially (`kLoopFadeSeconds` per e-fold) and comes back, in a few milliseconds, with the input. Their product
	///    is under one after a fraction of a second, whatever the settings, the loop dies, and the gate then ends it exactly.
	void inputFrame(bool inputIsZero) noexcept {
		if (!inputIsZero) {
			quietFrames_ = 0;
			gated_ = false;
			loopScale_ = std::min(1.0f, loopScale_ + (1.0f - loopScale_) * smoothing_ + 1.0e-6f);
			return;
		}
		if (quietFrames_ < UINT32_MAX) {
			++quietFrames_;
		}
		if (quietFrames_ >= fadeStartFrames_ && loopScale_ > 0.0f) {
			loopScale_ *= fadeStep_;
			if (loopScale_ < 1.0e-9f) {
				loopScale_ = 0.0f;
			}
		}
		else if (quietFrames_ < fadeStartFrames_ && loopScale_ < 1.0f) {
			loopScale_ = std::min(1.0f, loopScale_ + (1.0f - loopScale_) * smoothing_ + 1.0e-6f);
		}
		if (quietFrames_ >= gateFrames_) {
			const float floor = threshold_ * kLoopGateRelative;
			gated_ = innerLimiter_.envelope() <= floor && outerLimiter_.envelope() <= floor;
		}
	}
	/// THE FACTOR BOTH LOOPS' GAINS ARE MULTIPLIED BY (exactly 1.0f unless the input has been silent a while).
	[[nodiscard]] float loopScale() const noexcept { return gated_ ? 0.0f : loopScale_; }

	[[nodiscard]] bool emphOn() const noexcept { return !emphasis_.neutral(); }
	/// WITH THE EMPHASIS ON AND IN THE LOOP: the filter is after the sum.
	[[nodiscard]] bool emphInLoop() const noexcept { return loops_.inLoop && emphOn(); }
	[[nodiscard]] bool smearOn() const noexcept { return smear_.on(); }
	/// THE SMEAR'S GROUP DELAY AT ITS CENTRE, in frames (0 when it is not run): what it adds to a loop's tuned delay.
	[[nodiscard]] double smearDelayFrames() const noexcept { return smear_.on() ? smear_.delayAtCentre() : 0.0; }
	/// WHETHER THE OUTER LOOP IS ASKED FOR OR STILL TRAVELLING DOWN.
	[[nodiscard]] bool outerWanted() const noexcept { return outerTarget_ != 0.0f || outerGain_ != 0.0f; }
	/// WHETHER THE INNER FEEDBACK RUNS THROUGH THE LIMITER (Driven, or the emphasis in the loop).
	[[nodiscard]] bool limitInner() const noexcept { return loops_.limitInner; }

	void emphIn(float& left, float& right) noexcept {
		left = emphasis_.in(0, left);
		right = emphasis_.in(1, right);
	}
	void emphOut(float& left, float& right) noexcept {
		left = emphasis_.out(0, left);
		right = emphasis_.out(1, right);
	}
	void smear(float& left, float& right) noexcept {
		left = smear_.process(0, left);
		right = smear_.process(1, right);
	}

	/// THE OUTER LOOP'S GAIN, glided; TRUE while the loop runs (a target above zero, or a gain still travelling down). When
	/// it stops, its delay and limiter are emptied once.
	[[nodiscard]] bool outerBegin() noexcept {
		if (outerGain_ != outerTarget_) {
			const float next = outerGain_ + (outerTarget_ - outerGain_) * smoothing_;
			outerGain_ = (next == outerGain_ || std::abs(outerTarget_ - next) < 1.0e-9f) ? outerTarget_ : next;
		}
		const bool active = outerTarget_ != 0.0f || outerGain_ != 0.0f;
		if (!active && outer_.used()) {
			outer_.reset();
			outerLimiter_.reset();
		}
		return active;
	}
	/// ADDS what the outer delay returns (limited, scaled) to a sum.
	void outerReturn(float& left, float& right) noexcept {
		float l = 0.0f;
		float r = 0.0f;
		outer_.read(outerFrames_, l, r);
		outerLimiter_.process(l, r);
		lastOuterPeak_ = std::max(std::abs(l), std::abs(r));
		if (outerLimiter_.gain() < 1.0f) {
			++limitedFrames_;
		}
		const float gain = outerGain_ * loopScale();
		left += l * gain;
		right += r * gain;
	}
	void outerWrite(float left, float right) noexcept { outer_.write(flush(left), flush(right)); }

	/// THE INNER LOOP'S OWN DELAY (the engines that have no line). TRUE while it runs; emptied once when it stops.
	[[nodiscard]] bool innerBegin(bool active) noexcept {
		if (!active && inner_.used()) {
			inner_.reset();
			innerLimiter_.reset();
		}
		return active;
	}
	void innerRead(float& left, float& right) noexcept {
		inner_.read(innerFrames_, left, right);
		if (loops_.limitInner) {
			innerLimiter_.process(left, right);
		}
	}
	void innerWrite(float left, float right) noexcept { inner_.write(flush(left), flush(right)); }
	/// The limiter alone, for an engine whose inner loop is its own line.
	void limitInner(float& left, float& right) noexcept { innerLimiter_.process(left, right); }

	[[nodiscard]] bool holdsSound() const noexcept { return outer_.holdsSound() || inner_.holdsSound(); }

	/// **THE GAIN THE PATH AFTER A LOOP'S RETURN APPLIES, at the emphasis's peak** (1 when it applies none): a loop sitting on its
	/// limiter holds the RETURN at the threshold, so the level that reaches the output is that times this. From the topology:
	///  - NOT in the loop, Restore on: the return joins the sum after the emphasis in and meets only the inverse on its way out,
	///    which is the boost of a cut (a tilt either way, a bell below zero): `gi`.
	///  - In the loop, Restore off: the return meets the emphasis in and nothing takes it back: the boost, `gf` (a tilt either
	///    way, a bell above zero).
	///  - In the loop, Restore on: the tap is taken before the inverse, so the loop circulates through the emphasis in and the
	///    engine meets the boost while the inverse only undoes it on the way out: also the boost, `gf`. (Measured: with 1 here a
	///    howling loop under a +18 dB tilt in the loop came out 5 dB over the same loop with no emphasis; with `gf` it is within 2.)
	///  - NOT in the loop, Restore off: the return meets neither: 1.
	/// The limiters' thresholds are divided by it, so a howling loop comes out at about the same level whatever the emphasis.
	[[nodiscard]] static double pathGainAfterReturn(const Settings& s, bool inLoop) noexcept {
		const double db = std::clamp(static_cast<double>(s.emphAmount), -kEmphasisMaxDb, kEmphasisMaxDb);
		if (!std::isfinite(db) || db == 0.0) {
			return 1.0;
		}
		const double a = std::pow(10.0, std::abs(db) / 20.0);
		const bool tilt = !s.emphBell;
		const double forward = (tilt || db > 0.0) ? a : 1.0;
		const double inverse = (tilt || db < 0.0) ? a : 1.0;
		if (inLoop) {
			return forward;
		}
		return s.emphRestore ? inverse : 1.0;
	}

	/// WHERE THE EMPHASIS IS TUNED: Freq moved by `offsetSemitones`, held to 20 Hz .. min(20 kHz, 0.45 of the rate). An
	/// offset of exactly zero is Freq as it was, untouched, so the emphasis is the bits it was.
	[[nodiscard]] static double emphasisHz(double centreHz, double offsetSemitones, double rate) noexcept {
		if (!(offsetSemitones != 0.0)) {
			return centreHz;
		}
		const double offset = std::clamp(offsetSemitones, -kEmphOffsetSemitones, kEmphOffsetSemitones);
		return std::clamp(centreHz * std::exp2(offset / 12.0), 20.0, std::min(20000.0, 0.45 * rate));
	}

	/// THE BELL'S WIDTH IN OCTAVES FROM THE WIDTH CONTROL: 0 is a quarter of an octave, 1 is three, geometrically between.
	[[nodiscard]] static double emphasisWidthOctaves(float width) noexcept {
		const double w = std::clamp(static_cast<double>(width), 0.0, 1.0);
		return kEmphasisWidthOctavesMin * std::pow(kEmphasisWidthOctavesMax / kEmphasisWidthOctavesMin, w);
	}

	//----------------------------------------------------------------------------------------------//
	// TEST SEAMS                                                                                     //
	//----------------------------------------------------------------------------------------------//

	[[nodiscard]] const EmphasisPair& emphasisForTest() const noexcept { return emphasis_; }
	[[nodiscard]] const Smear& smearForTest() const noexcept { return smear_; }
	[[nodiscard]] const LoopDerived& loopsForTest() const noexcept { return loops_; }
	[[nodiscard]] double innerFramesForTest() const noexcept { return innerFrames_; }
	[[nodiscard]] double outerFramesForTest() const noexcept { return outerFrames_; }
	[[nodiscard]] float outerGainForTest() const noexcept { return outerGain_; }
	[[nodiscard]] float loopScaleForTest() const noexcept { return loopScale_; }
	[[nodiscard]] bool gatedForTest() const noexcept { return gated_; }
	[[nodiscard]] const LoopLimiter& innerLimiterForTest() const noexcept { return innerLimiter_; }
	[[nodiscard]] float outerLimiterGainForTest() const noexcept { return outerLimiter_.gain(); }
	/// THE LARGEST SAMPLE THE LAST OUTER RETURN CARRIED, after the limiter and before the gain.
	[[nodiscard]] float lastOuterPeakForTest() const noexcept { return lastOuterPeak_; }
	/// HOW MANY FRAMES THE OUTER LIMITER HAS ACTED ON, since the section was built.
	[[nodiscard]] uint64_t limitedFramesForTest() const noexcept { return limitedFrames_; }
	[[nodiscard]] const LoopLimiter& outerLimiterForTest() const noexcept { return outerLimiter_; }

private:
	void applyThreshold() noexcept {
		const auto t = static_cast<float>(static_cast<double>(threshold_) / limitDivisor_);
		innerLimiter_.setThreshold(t);
		outerLimiter_.setThreshold(t);
	}

	/// A tap below this is flushed, so a float loop reaches exact zero instead of circling in the denormals.
	[[nodiscard]] static float flush(float x) noexcept { return std::abs(x) < 1.0e-9f ? 0.0f : x; }

	double rate_ = 48000.0;
	float smoothing_ = 1.0f;
	EmphasisPair emphasis_ {};
	Smear smear_ {};
	LoopDelay outer_ {};
	LoopDelay inner_ {};
	LoopLimiter innerLimiter_ {};
	LoopLimiter outerLimiter_ {};
	LoopDerived loops_ {};
	double innerFrames_ = 2.0;
	double outerFrames_ = 2.0;
	float outerGain_ = 0.0f;
	float outerTarget_ = 0.0f;
	float lastOuterPeak_ = 0.0f;
	uint64_t limitedFrames_ = 0;
	bool placed_ = false;
	float threshold_ = 1.0f;
	double limitDivisor_ = 1.0;
	uint32_t quietFrames_ = 0;
	uint32_t gateFrames_ = 0;
	uint32_t fadeStartFrames_ = 0;
	float fadeStep_ = 1.0f;
	float loopScale_ = 1.0f;
	bool gated_ = false;
};

} // namespace feathers::fx::eroder
