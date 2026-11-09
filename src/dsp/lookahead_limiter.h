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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// **A LOOKAHEAD PEAK LIMITER** (backlog LD, where Featherverb's wet ceiling needed it; backlog LE, where it becomes a
// device of its own). Its signal is delayed by `lookahead` samples, and in that time the gain has ramped down to what
// the loudest sample coming needs — so no sample leaves above the ceiling, and none is squared off on the way.
//
// WHY NOT AN INSTANT ATTACK (what Featherverb had, MEASURED 2026-10-04): an envelope that jumps to each new peak makes
// that very sample leave at EXACTLY the ceiling, so the rising part of every new peak is flattened — a clip with a
// release, heard as tiny glitches on loud input.
//
// THE MECHANISM, per sample:
// 1. the peak entering (|x|, or max(|l|, |r|) when the channels are LINKED) is HELD as a running maximum over
//    `lookahead + 1` samples (or the caller's own detector levels: `processSidechain`);
// 2. an envelope follows the held peak — up at once, down with time constant `releaseSeconds` (in level, or in dB:
//    `Params::releaseInDecibels`) — and the wanted gain is
//    ceiling / envelope, or EXACTLY 1 while the envelope is at or under the ceiling;
// 3. that gain is smoothed by two box filters in cascade, of `boxA + boxB - 1 = lookahead + 1` samples together.
// The hold spans every sample the two boxes average, so the gain applied to a sample is never above that sample's own
// ceiling / peak: NO SAMPLE ABOVE THE CEILING (a last clamp catches the ulp a product can carry). And the gain is a
// RAMP WITH A CONTINUOUS SLOPE, by construction:
// - its step per sample is at most `1 / boxA`;
// - its change of slope per sample is at most `2 / (boxA · boxB)`.
// (At 72 samples: two boxes of 37, a step of at most 0.027, a slope change of at most 0.00146.)
//
// UNDER THE CEILING IT IS A PURE DELAY, TO THE BIT: a box of exact ones returns exactly 1, and only the running maximum
// is kept while nothing in the window is over the ceiling and the envelope is under it.
//
// O(1) PER SAMPLE: the running maximum is a monotone queue and the boxes are running sums, so the cost does not grow
// with the lookahead. (A first draft scanned the window and summed both boxes every sample: MEASURED at 194 ns a frame
// while limiting, 142% of the whole Featherverb it sits in.)
//
// THE LOOKAHEAD IS LATENCY. A host must be told it if the limiter's output is mixed against anything undelayed; a
// caller whose undelayed partner is uncorrelated (a reverb's wet against its dry) may treat it as predelay instead.
//
// ALLOCATES ONLY IN `prepare`. `setParams` and `process` allocate nothing.
namespace feathers::dsp {

class LookaheadLimiter {
public:
	struct Params {
		float ceiling = 1.0f;        ///< linear; no sample leaves above it
		size_t lookahead = 64;       ///< samples, 1 .. the `maxLookahead` given to `prepare`
		double releaseSeconds = 0.1; ///< the envelope's time constant on the way down
		bool link = true;            ///< one gain for both channels, from the louder of the two
		/// THE RELEASE'S DOMAIN. False (the block's own, and Featherverb's): the envelope falls toward the held peak
		/// as a one-pole on its LEVEL, so the gain recovers LINEARLY in dB, quickly after a deep reduction. True: a
		/// one-pole on its level IN dB, toward the louder of the held peak and the ceiling, so the reduction in dB
		/// decays EXPONENTIALLY — the textbook smoother's release, and COMP's (backlog LE), whose Release control
		/// meant that curve before it had a lookahead.
		bool releaseInDecibels = false;
	};

	/// MESSAGE THREAD. Every ring sized for up to `maxLookahead` samples, and the release's coefficient for this rate.
	void prepare(double sampleRate, size_t maxLookahead) {
		sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
		maxLookahead_ = std::max<size_t>(1, maxLookahead);
		for (Chain& c : chains_) {
			c.delay.assign(maxLookahead_, 0.0f);
			c.qPeak.assign(maxLookahead_ + 1, 0.0f);
			c.qTime.assign(maxLookahead_ + 1, 0);
			c.boxA.assign(maxLookahead_ / 2 + 1, 1.0f);
			c.boxB.assign(maxLookahead_ / 2 + 2, 1.0f);
		}
		apply(params_);
		reset();
	}

	/// ANY THREAD THE CALLER SERIALISES WITH `process`; allocates nothing. A change of lookahead or link RESETS (the
	/// first is a change of latency, the second of which gain runs where); ceiling and release change in place.
	void setParams(const Params& p) noexcept {
		const size_t was = lookahead_;
		const bool linked = params_.link;
		apply(p);
		if (lookahead_ != was || params_.link != linked) {
			reset();
		}
	}

	/// AS IF JUST SWITCHED ON: nothing in flight, the gain at 1. Allocates nothing.
	void reset() noexcept {
		for (Chain& c : chains_) {
			std::fill(c.delay.begin(), c.delay.end(), 0.0f);
			std::fill(c.boxA.begin(), c.boxA.end(), 1.0f);
			std::fill(c.boxB.begin(), c.boxB.end(), 1.0f);
			c.delayPos = c.aPos = c.bPos = 0;
			c.aUnder = c.bUnder = 0;
			c.aSum = static_cast<double>(boxA_);
			c.bSum = static_cast<double>(boxB_);
			c.qHead = c.qSize = 0;
			c.now = 0;
			c.env = 0.0f;
			c.gain = 1.0f;
		}
	}

	/// THE SIGNAL, IN PLACE: delayed by `lookahead()` and held under the ceiling. `limit` false keeps the delay and
	/// applies no gain (a caller's way of hearing the signal unlimited on the same timing). `left == right` is a mono
	/// road and is safe. `gains`, if given, receives each frame's applied gain (the lower of the two), for a meter.
	/// AUDIO THREAD; allocates nothing.
	void process(float* left, float* right, int frames, bool limit = true, float* gains = nullptr) noexcept {
		run(left, right, frames, gains, [&](int k, float inL, float inR) {
			(void)k;
			return limit ? std::array<float, 2> { std::fabs(inL), std::fabs(inR) } : std::array<float, 2> { 0.0f, 0.0f };
		});
	}

	/// THE SAME, WITH THE DETECTOR SUPPLIED BY THE CALLER — A SIDECHAIN (backlog LE, COMP's Limit, whose detector is
	/// high-passed, driven and stereo-blended). Frame `k`'s gains are computed from `peakL[k]` and `peakR[k]` (linked:
	/// the louder) in place of the signal's own levels, in the ceiling's units, and the signal is delayed and gained
	/// exactly as `process` does it. **THE NO-OVERSHOOT PROMISE IS THEN ABOUT THE PEAKS GIVEN**: a sample louder than
	/// its own peak (a detector that cannot hear bass the audio carries) can leave above the ceiling while the gain
	/// is 1, so a caller whose detector is not the audio clamps after. AUDIO THREAD; allocates nothing.
	void processSidechain(float* left, float* right, const float* peakL, const float* peakR, int frames,
	                      float* gains = nullptr) noexcept {
		run(left, right, frames, gains, [&](int k, float inL, float inR) {
			(void)inL;
			(void)inR;
			return std::array<float, 2> { peakL[k], peakR[k] };
		});
	}

	[[nodiscard]] size_t lookahead() const noexcept { return lookahead_; }
	[[nodiscard]] size_t maxLookahead() const noexcept { return maxLookahead_; }
	/// The two boxes' lengths: the step bound is `1 / boxA()`, the slope-change bound `2 / (boxA() · boxB())`.
	[[nodiscard]] size_t boxA() const noexcept { return boxA_; }
	[[nodiscard]] size_t boxB() const noexcept { return boxB_; }
	[[nodiscard]] const Params& params() const noexcept { return params_; }
	/// **WHETHER A NON-ZERO SAMPLE IS STILL IN THE DELAY** (backlog MC) — what the lookahead has taken in and not yet put
	/// out. A host deciding a device is done asks this, not the output. Scanned: the delay is milliseconds long.
	[[nodiscard]] bool holdsSound() const noexcept {
		for (const Chain& c : chains_) {
			for (const float v : c.delay) {
				if (v != 0.0f) {
					return true;
				}
			}
		}
		return false;
	}
	/// TEST ONLY — the gain applied to channel `ch`'s last sample out, and its envelope now.
	[[nodiscard]] float gainForTest(int ch) const noexcept { return chains_[ch & 1].gain; }
	[[nodiscard]] float envelopeForTest(int ch) const noexcept { return chains_[ch & 1].env; }
	/// TEST ONLY — the running maximum of channel `ch`'s peaks over the window, as the queue holds it.
	[[nodiscard]] float heldForTest(int ch) const noexcept {
		const Chain& c = chains_[ch & 1];
		return c.qSize == 0 ? 0.0f : c.qPeak[c.qHead];
	}
	/// FRAMES LIMITED (any gain under 1, either channel) and ATTACKS (the envelope jumping up to a new held peak).
	struct Events {
		uint64_t limited = 0;
		uint64_t attack = 0;
	};
	[[nodiscard]] const Events& eventsForTest() const noexcept { return events_; }

private:
	/// THE ONE LOOP BOTH ROADS RUN: `peaksOf(k, inL, inR)` is what frame `k`'s gains are computed from.
	template <typename PeaksOf>
	void run(float* left, float* right, int frames, float* gains, PeaksOf&& peaksOf) noexcept {
		if (chains_[0].delay.empty()) {
			return;
		}
		Chain& cl = chains_[0];
		Chain& cr = chains_[1];
		for (int k = 0; k < frames; ++k) {
			const float inL = left[k];
			const float inR = right[k];
			const std::array<float, 2> peak = peaksOf(k, inL, inR);
			const float outL = cl.delayed(inL, lookahead_);
			const float outR = cr.delayed(inR, lookahead_);
			float gL = 1.0f;
			float gR = 1.0f;
			if (params_.link) {
				gL = gR = cl.gainFor(std::max(peak[0], peak[1]), *this, events_);
				cr.gain = gL;
			}
			else {
				gL = cl.gainFor(peak[0], *this, events_);
				gR = cr.gainFor(peak[1], *this, events_);
			}
			events_.limited += gL < 1.0f || gR < 1.0f ? 1u : 0u;
			if (gains != nullptr) {
				gains[k] = std::min(gL, gR);
			}
			const float c = params_.ceiling;
			// RIGHT BEFORE LEFT, so a mono road (both pointers the same) keeps the left channel.
			right[k] = gR < 1.0f ? std::clamp(outR * gR, -c, c) : outR;
			left[k] = gL < 1.0f ? std::clamp(outL * gL, -c, c) : outL;
		}
	}

	struct Chain {
		std::vector<float> delay;
		std::vector<float> qPeak;
		std::vector<uint64_t> qTime;
		std::vector<float> boxA;
		std::vector<float> boxB;
		size_t delayPos = 0;
		size_t qHead = 0;
		size_t qSize = 0;
		uint64_t now = 0;
		size_t aPos = 0;
		size_t bPos = 0;
		size_t aUnder = 0;
		size_t bUnder = 0;
		double aSum = 0.0;
		double bSum = 0.0;
		float env = 0.0f;
		/// THE ENVELOPE AS ln(level), carried in double — read and written only by the dB-domain release.
		double logEnv = 0.0;
		float gain = 1.0f;

		float delayed(float in, size_t n) noexcept {
			const float out = delay[delayPos];
			delay[delayPos] = in;
			delayPos = delayPos + 1 == n ? 0 : delayPos + 1;
			return out;
		}

		float gainFor(float peak, const LookaheadLimiter& lim, Events& events) noexcept {
			const float ceiling = lim.params_.ceiling;
			const uint64_t span = lim.lookahead_ + 1;
			// THE RUNNING MAXIMUM over the last `span` peaks, in O(1) amortised: a queue of (peak, time) whose peaks
			// FALL from front to back — a new peak retires every one at the back it is not under (none of them can be the
			// maximum again while it is in the window), and the front leaves once it is older than the window.
			// THE FRONT LEAVES FIRST, so the queue holds only times in (now - span, now] — at most `span` entries, a ring
			// of `maxLookahead + 1`. (Retiring it after the push needs one slot more: MEASURED, under ASan, a falling
			// sine filled the ring and the push overwrote the front.)
			const size_t cap = qPeak.size();
			while (qSize > 0 && qTime[qHead] + span <= now) {
				qHead = qHead + 1 == cap ? 0 : qHead + 1;
				--qSize;
			}
			size_t tail = qHead + qSize; // one past the back, unwrapped (< 2 cap)
			while (qSize > 0) {
				const size_t back = tail - 1 >= cap ? tail - 1 - cap : tail - 1;
				if (qPeak[back] > peak) {
					break;
				}
				--qSize;
				--tail;
			}
			const size_t slot = tail >= cap ? tail - cap : tail;
			qPeak[slot] = peak;
			qTime[slot] = now;
			++qSize;
			++now;
			const float held = qPeak[qHead];
			float want = 1.0f;
			if (held > ceiling || env > ceiling) {
				if (held > env) {
					env = held;
					logEnv = lim.params_.releaseInDecibels ? std::log(static_cast<double>(held)) : 0.0;
					++events.attack;
				}
				else if (lim.params_.releaseInDecibels) {
					// IN dB, IN DOUBLE: toward the ceiling it is an asymptote, and in float the step would round to nothing
					// one ulp above the ceiling and stay there. In double the float `env` reaches the ceiling, and the
					// branch below puts the chain at rest.
					logEnv += static_cast<double>(lim.release_) *
					          (std::log(static_cast<double>(std::max(held, ceiling))) - logEnv);
					env = static_cast<float>(std::exp(logEnv));
				}
				else {
					env += lim.release_ * (held - env);
				}
				want = env > ceiling ? ceiling / env : 1.0f;
			}
			else {
				env = 0.0f;
			}
			// AT REST — wanting 1 with both boxes full of ones — the boxes are left as they are: a ring of ones is the same
			// ring wherever its write position stands.
			if (want == 1.0f && aUnder == 0 && bUnder == 0) {
				gain = 1.0f;
				return gain;
			}
			const float a = push(boxA, aPos, aUnder, aSum, lim.boxA_, want);
			gain = push(boxB, bPos, bUnder, bSum, lim.boxB_, a);
			return gain;
		}

		/// One box of length `n`: `v` in, the mean out, by a RUNNING SUM in double. EXACTLY 1 while every entry is
		/// exactly 1 — and the sum is re-seated to exactly `n` then, so its rounding cannot outlive a limiting episode.
		static float push(std::vector<float>& ring, size_t& at, size_t& under, double& sum, size_t n, float v) noexcept {
			under += (v < 1.0f ? 1 : 0) - (ring[at] < 1.0f ? 1 : 0);
			sum += static_cast<double>(v) - static_cast<double>(ring[at]);
			ring[at] = v;
			at = at + 1 == n ? 0 : at + 1;
			if (under == 0) {
				sum = static_cast<double>(n);
				return 1.0f;
			}
			return std::min(1.0f, static_cast<float>(sum / static_cast<double>(n)));
		}
	};

	void apply(const Params& p) noexcept {
		params_ = p;
		params_.ceiling = p.ceiling > 0.0f && std::isfinite(p.ceiling) ? p.ceiling : 1.0f;
		// THE REQUEST IS KEPT in `params_`, the clamp is `lookahead_`: params set before `prepare` survive it.
		lookahead_ = std::clamp<size_t>(p.lookahead, 1, maxLookahead_);
		boxA_ = (lookahead_ + 2) / 2;
		boxB_ = lookahead_ + 2 - boxA_;
		const double release = p.releaseSeconds > 0.0 ? p.releaseSeconds : 0.1;
		release_ = static_cast<float>(1.0 - std::exp(-1.0 / (release * sampleRate_)));
	}

	std::array<Chain, 2> chains_ {};
	Params params_ {};
	double sampleRate_ = 48000.0;
	size_t maxLookahead_ = 1;
	size_t lookahead_ = 1;
	size_t boxA_ = 1;
	size_t boxB_ = 1;
	float release_ = 0.0f;
	Events events_ {};
};

} // namespace feathers::dsp
