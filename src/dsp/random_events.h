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
#include <cmath>
#include <cstdint>

// THE RANDOM-EVENT GENERATOR — pseudorandom hits in time, with a size each (backlog MZ and NA).
//
// **ONE CORE, TWO DOMAINS** (NA option 3): NOISE's Impulses play these events as Dirac clicks
// in AUDIO, and NA's Random Trigger modulator will play the same events as triggers in the MODULATION domain.
// Same settings and the same seed give the same events in both, which is why this lives in `dsp/` and knows
// nothing about either device: no params, no rows, no allocation, one sample at a time.
//
// **THE DRAW ORDER IS FIXED, SO A SETTING THAT DOES NOT TOUCH TIMING CANNOT MOVE THE TIMING.** Every event takes
// exactly four uniforms from the stream, whatever the distributions: the gap, the cluster branch, the size and
// the sign. Switching Hit Size or Polarity therefore changes each hit's value and leaves every hit where it
// was, and two consumers that differ only in what they do with the size stay in step.

namespace feathers::dsp {

/// WHEN THE EVENTS FALL. The mean rate is `RandomEventSettings::rateHz` in all three.
enum class EventTiming : int {
	/// A GRID, each event displaced from its grid point by up to ±half an interval times `jitter`. At jitter 0 a
	/// metronome; the displacement is per event and never accumulates, so the grid does not drift.
	Regular,
	/// MEMORYLESS: exponential gaps, so the count in a window is Poisson.
	Poisson,
	/// BURSTS: a gap is short (a twentieth of the mean interval) with probability `0.9 x cluster`, otherwise
	/// long enough that the MEAN gap is still the mean interval. At cluster 0 this is Poisson.
	Clustered,
	kCount,
};

/// HOW BIG EACH EVENT IS, before polarity. All three peak at 1.
enum class HitSize : int {
	/// Every hit 1.
	Fixed,
	/// Uniform on (0, 1].
	Uniform,
	/// MOSTLY SMALL, NOW AND THEN BIG: exponential with mean `kExponentialMean`, capped at 1.
	Exponential,
	kCount,
};

struct RandomEventSettings {
	/// MEAN EVENTS PER SECOND. Clamped to (0, the sample rate]: an event is one sample.
	double rateHz = 8.0;
	EventTiming timing = EventTiming::Poisson;
	/// REGULAR ONLY: 0..1, the displacement as a fraction of the interval (±half of it at 1).
	float jitter = 0.0f;
	/// CLUSTERED ONLY: 0..1, how bursty.
	float cluster = 0.5f;
	HitSize size = HitSize::Fixed;
	/// HALF THE HITS NEGATIVE, chosen at random.
	bool randomPolarity = false;
};

class RandomEvents {
public:
	/// THE MEAN OF AN EXPONENTIAL HIT BEFORE ITS CAP — a feel constant. A third keeps most hits well under the
	/// peak and lets roughly one in twenty reach it (P(e >= 1) = e^-3).
	static constexpr double kExponentialMean = 1.0 / 3.0;
	/// CLUSTERED: the probability of a short gap at cluster 1, and how short it is as a fraction of the mean.
	static constexpr double kClusterShortProbability = 0.9;
	static constexpr double kClusterShortFraction = 0.05;

	/// THE STREAM AND THE CLOCK START AGAIN FROM `seed`: what follows is a function of the seed and the
	/// settings alone. The first event waits one drawn gap, so a sparse stream does not open on a hit.
	void seed(uint64_t seed) noexcept {
		state_ = seed * 0x9e3779b97f4a7c15ull + 0x632be59bd9b4e5f5ull;
		if (state_ == 0) {
			state_ = 0x2545f4914f6cdd1dull;
		}
		lastOffset_ = 0.0;
		primed_ = false;
	}

	/// TURN THE STREAM ONTO ANOTHER COURSE WITHOUT RESTARTING IT: the state is folded with `k`, the clock and the wait
	/// already drawn are kept. What a Free consumer uses to make two instances that were born alike diverge.
	void mix(uint64_t k) noexcept {
		state_ ^= k;
		if (state_ == 0) {
			state_ = 0x2545f4914f6cdd1dull;
		}
	}

	void setSampleRate(double sampleRate) noexcept {
		sampleRate_ = std::max(sampleRate, 1.0);
		primed_ = false;
	}

	/// ONE SAMPLE: the hit's value (signed, |v| <= 1) if an event lands on it, else exactly 0. At most one event a
	/// sample. A changed rate rescales the wait already drawn, so a sparse stream turned dense answers at once
	/// rather than after the long gap it was in — exact for Poisson, whose waits are memoryless.
	[[nodiscard]] float next(const RandomEventSettings& s) noexcept {
		const double interval = intervalSamples(s.rateHz);
		if (!primed_) {
			interval_ = interval;
			untilEvent_ = drawGap(s, interval);
			primed_ = true;
		}
		else if (interval != interval_) {
			untilEvent_ *= interval / interval_;
			interval_ = interval;
		}
		untilEvent_ -= 1.0;
		if (untilEvent_ > 0.0) {
			return 0.0f;
		}
		const float value = drawHit(s);
		// THE FRACTION CARRIES: an event due at 0.3 of a sample past this one makes the next gap 0.3 shorter, so
		// the mean rate is exact however the gaps round.
		untilEvent_ += std::max(drawGap(s, interval), 0.0);
		// AND NEVER TWO IN ONE SAMPLE: a gap under a sample lands on the next one.
		untilEvent_ = std::max(untilEvent_, 1e-9);
		return value;
	}

	/// TEST AND DISPLAY: samples until the next event would land at the current settings.
	[[nodiscard]] double untilEventForTest() const noexcept { return untilEvent_; }

	/// **WHERE THE STREAM STANDS** (backlog NM — a patch saves it, so a reload goes on from it): everything but the sample
	/// rate, which is the host's.
	struct Position {
		uint64_t state = 0;
		double interval = 0.0;
		double untilEvent = 0.0;
		double lastOffset = 0.0;
		bool primed = false;
	};
	[[nodiscard]] Position position() const noexcept { return { state_, interval_, untilEvent_, lastOffset_, primed_ }; }
	void setPosition(const Position& p) noexcept {
		state_ = p.state != 0 ? p.state : 0x2545f4914f6cdd1dull;
		interval_ = p.interval;
		untilEvent_ = p.untilEvent;
		lastOffset_ = p.lastOffset;
		primed_ = p.primed;
	}

private:
	[[nodiscard]] double intervalSamples(double rateHz) const noexcept {
		const double rate = std::clamp(std::isfinite(rateHz) ? rateHz : 1.0, 1e-6, sampleRate_);
		return sampleRate_ / rate;
	}

	/// xorshift64* — the full 64-bit state, the top 53 bits as a double in [0, 1).
	[[nodiscard]] double uniform() noexcept {
		state_ ^= state_ >> 12;
		state_ ^= state_ << 25;
		state_ ^= state_ >> 27;
		const uint64_t r = state_ * 0x2545f4914f6cdd1dull;
		return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0);
	}

	/// THE NEXT GAP, in samples — two uniforms, always both drawn.
	[[nodiscard]] double drawGap(const RandomEventSettings& s, double interval) noexcept {
		const double uGap = uniform();
		const double uBranch = uniform();
		// (0, 1], so the log is finite.
		const double expUnit = -std::log(1.0 - uGap);
		switch (s.timing) {
		case EventTiming::Regular: {
			// THE NEXT GRID POINT, AND THIS EVENT'S OWN DISPLACEMENT FROM IT: gap = interval + new - old, never
			// negative because each displacement is within half an interval.
			const double j = std::clamp(static_cast<double>(s.jitter), 0.0, 1.0);
			const double offset = (uGap - 0.5) * j * interval;
			const double gap = interval + offset - lastOffset_;
			lastOffset_ = offset;
			return gap;
		}
		case EventTiming::Clustered: {
			lastOffset_ = 0.0;
			const double p = kClusterShortProbability * std::clamp(static_cast<double>(s.cluster), 0.0, 1.0);
			const double shortMean = kClusterShortFraction * interval;
			// p x short + (1 - p) x long = interval, so the mean rate is the rate asked for.
			const double longMean = (interval - p * shortMean) / (1.0 - p);
			return expUnit * (uBranch < p ? shortMean : longMean);
		}
		case EventTiming::Poisson:
		case EventTiming::kCount:
			break;
		}
		lastOffset_ = 0.0;
		return expUnit * interval;
	}

	/// THE HIT'S VALUE — two uniforms, always both drawn.
	[[nodiscard]] float drawHit(const RandomEventSettings& s) noexcept {
		const double uSize = uniform();
		const double uSign = uniform();
		double size = 1.0;
		switch (s.size) {
		case HitSize::Uniform: size = 1.0 - uSize; break; // (0, 1]
		case HitSize::Exponential: size = std::min(1.0, -std::log(1.0 - uSize) * kExponentialMean); break;
		case HitSize::Fixed:
		case HitSize::kCount: break;
		}
		const bool negative = s.randomPolarity && uSign < 0.5;
		return static_cast<float>(negative ? -size : size);
	}

	uint64_t state_ = 0x2545f4914f6cdd1dull;
	double sampleRate_ = 48000.0;
	double interval_ = 6000.0;
	double untilEvent_ = 0.0;
	double lastOffset_ = 0.0;
	bool primed_ = false;
};

} // namespace feathers::dsp
