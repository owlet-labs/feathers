/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// **WHERE TWO BRANCHES MEET, THE SHORTER ONE WAITS FOR THE LONGER** — backlog JM. A device that
// delays its audio declares by how much
// (`AudioOps::latencySamples`); a host that mixes that device's output against a signal that did not go
// through it (a rack's dry, a chain's bypass) delays that signal by the same amount here, or the two comb.
//
// IN THE DEVICE VOCABULARY rather than in the rack, because a DEVICE needs it too: a device with an
// internal dry/wet around a latent path (CLIP's oversampler) aligns its own dry with the same object.
namespace feathers::device {

/// **THE MOST A COMPENSATION POINT CAN ALIGN: ONE SECOND at the running rate** (assumption L4). The EQ's
/// linear phase is about 0.19 s, so five fit in series. Past it the delay clamps; the report does not.
inline constexpr double kMaxLatencySeconds = 1.0;

/// **THE MOST LATENCY A HOST COMPENSATES ON ITS PER-VOICE ROAD, IN ALL** (`Traits::latentPerVoice`): 80 ms, two of Spectral's Fine
/// stop in series. Feathers' lane admits latent per-voice devices up to it (`RackEngine::setLatentPerVoiceCap`) and the Synth's
/// gain rings are sized for it; a device that would take the sum past it runs on the bus, so the delay is never clamped.
inline constexpr double kMaxPerVoiceLatencySeconds = 0.08;

/// That bound in samples at `sampleRate`, rounded up, plus one for safety at an odd rate.
[[nodiscard]] inline uint32_t perVoiceLatencyBound(double sampleRate) noexcept {
	return static_cast<uint32_t>(std::ceil(kMaxPerVoiceLatencySeconds * std::max(sampleRate, 1.0))) + 1u;
}

/// The ring's length at `sampleRate`: the next power of two at or above one second of it.
[[nodiscard]] inline size_t latencyCapacityFor(double sampleRate) noexcept {
	const double wanted = std::max(1.0, kMaxLatencySeconds * sampleRate);
	size_t capacity = 1;
	while (static_cast<double>(capacity) < wanted) {
		capacity <<= 1;
	}
	return capacity;
}

/// **A STEREO DELAY WHOSE LENGTH IS HANDED PER CALL, IN PLACE.** Sized at `prepare` — MESSAGE THREAD — and
/// never again, so `process` allocates nothing.
///
/// **AT ZERO IT DOES NOTHING AT ALL** — not even a history write — which is what keeps every road that
/// holds no latent device bit-identical to the road before this existed. The price: history written only
/// while delaying, so a delay coming UP from zero starts from a CLEARED ring rather than from whatever it
/// held the last time it delayed (that would replay stale audio). Between two non-zero lengths the ring is
/// continuous and only the read point jumps. Either change is a latency change, which restarts the host.
class CompensationDelay {
public:
	/// MESSAGE THREAD. Allocates the ring for `sampleRate` and clears it.
	void prepare(double sampleRate) {
		const size_t capacity = latencyCapacityFor(sampleRate);
		left_.assign(capacity, 0.0f);
		right_.assign(capacity, 0.0f);
		mask_ = capacity - 1;
		write_ = 0;
		delay_ = 0;
	}

	/// **SIZED TO WHAT THE BRANCH DECLARES NOW, NOT TO A SECOND** (backlog NR's second cheap win) — MESSAGE THREAD.
	/// The ring reaches `declared` samples, or `kSizedFloorSeconds`, whichever is longer, and never past the second
	/// `prepare(sampleRate)` would give. For a holder of a chain that is usually empty: a second of ring is
	/// ~0.4-0.5 MB per chain at 48 kHz, and this is 64 KB.
	///
	/// **THE PRICE: a branch whose latency grows past the ring after this call clamps until the next `prepare`** —
	/// which the host restart a latency change already causes provides (`b5d148e9`), exactly as a branch past one
	/// second clamps today. The floor covers every latency a device in this repo declares but the EQ's linear phase.
	void prepare(double sampleRate, uint32_t declared) {
		const double floor = kSizedFloorSeconds * std::max(sampleRate, 1.0);
		const double wanted = std::min(std::max(floor, static_cast<double>(declared) + 1.0),
		                               static_cast<double>(latencyCapacityFor(sampleRate)));
		size_t capacity = 1;
		while (static_cast<double>(capacity) < wanted) {
			capacity <<= 1;
		}
		left_.assign(capacity, 0.0f);
		right_.assign(capacity, 0.0f);
		left_.shrink_to_fit();
		right_.shrink_to_fit();
		mask_ = capacity - 1;
		write_ = 0;
		delay_ = 0;
	}
	/// The shortest ring the sized `prepare` gives: 0.1 s, past every per-voice bound (`kMaxPerVoiceLatencySeconds`).
	static constexpr double kSizedFloorSeconds = 0.1;

	/// FORGET THE HISTORY, keep the size. Allocates nothing, and costs nothing now: the ring is cleared by
	/// the next call that delays, exactly as when coming up from zero.
	void reset() noexcept { delay_ = 0; }

	/// The most this can delay: one less than the ring (0 before `prepare`).
	[[nodiscard]] uint32_t capacity() const noexcept {
		return left_.empty() ? 0 : static_cast<uint32_t>(mask_);
	}

	/// DELAY `left`/`right` IN PLACE BY `samples`, clamped to `capacity()`. AUDIO THREAD.
	void process(float* left, float* right, int frames, uint32_t samples) noexcept {
		const uint32_t d = std::min(samples, capacity());
		if (d == 0) {
			delay_ = 0;
			return;
		}
		if (delay_ == 0) {
			// COMING UP FROM ZERO (or a reset): the ring holds audio from the last time it delayed.
			std::fill(left_.begin(), left_.end(), 0.0f);
			std::fill(right_.begin(), right_.end(), 0.0f);
			write_ = 0;
		}
		delay_ = d;
		for (int i = 0; i < frames; ++i) {
			left_[write_] = left[i];
			right_[write_] = right[i];
			const size_t read = (write_ - d) & mask_;
			left[i] = left_[read];
			right[i] = right_[read];
			write_ = (write_ + 1) & mask_;
		}
	}

	/// TEST ONLY — the length the last call delayed by.
	[[nodiscard]] uint32_t delayForTest() const noexcept { return delay_; }

private:
	std::vector<float> left_;
	std::vector<float> right_;
	size_t mask_ = 0;
	size_t write_ = 0;
	uint32_t delay_ = 0;
};

} // namespace feathers::device
