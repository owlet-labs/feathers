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

// **A-PH: THE OPTIMISER'S THREAD** (additive design §8). The character bank builds on the AUDIO thread and the main thread
// ticks only while an editor is open, so there is no build-side thread to borrow: the synth owns this one. It is started
// on the main thread (activate) and joined at deactivate; it SLEEPS until the audio thread posts (an atomic wait the poster
// wakes: no lock, no poll, so an idle or SLEEPING instance costs no wake-ups at all), then optimises the newest request and
// publishes the profile. The audio thread only posts and reads —
// both lock-free, both fixed copies (dsp/phase_profile.h). A request a newer one overwrote is never computed: under a
// modulation that re-bakes every block the thread does one optimisation at a time on the newest spectrum, never a queue.
//
// THE PROFILE IS NOT SAVED with the patch, and that is the persist rule's own exception (CLAUDE.md: derive what can be
// derived): it is a deterministic function of the spectrum the patch builds, recomputed within tens of milliseconds of
// the first block; until then a note-on uses the closed form, as it does before any profile exists.

#include "dsp/phase_profile.h"

#include <atomic>
#include <chrono>
#include <thread>

namespace feathers {

class PhaseService {
public:
	/// How many clip-and-restore rounds one optimisation runs (design §8.4). MEASURED on the default patch at C4 (320 partials):
	/// 2.6424 dB of crest at 100 rounds in 15 ms, 2.6412 at 300 in 44 ms, 2.6408 at 400 in 58 ms — so 100, three times sooner.
	static constexpr int kRounds = 100;

	PhaseService() = default;
	PhaseService(const PhaseService&) = delete;
	PhaseService& operator=(const PhaseService&) = delete;
	~PhaseService() { stop(); }

	/// THE MAIN THREAD: start the thread if it is not running. Allocates (the thread), so never from the audio thread.
	void start() {
		if (running_.exchange(true)) {
			return;
		}
		thread_ = std::thread([this] { run(); });
	}
	/// THE MAIN THREAD: stop and join.
	void stop() {
		if (!running_.exchange(false)) {
			return;
		}
		wake_.fetch_add(1, std::memory_order_release);
		wake_.notify_one();
		if (thread_.joinable()) {
			thread_.join();
		}
	}
	[[nodiscard]] bool running() const noexcept { return running_.load(std::memory_order_relaxed); }

	/// **THE AUDIO THREAD: POST A REQUEST AND WAKE THE THREAD.** The wake is `std::atomic::notify_one` on a 32-bit counter: on
	/// Linux a FUTEX_WAKE when a waiter is parked (no mutex in libstdc++'s path for a futex-sized type), on macOS
	/// __ulock_wake, on Windows WakeByAddressSingle — non-blocking for the poster, like sem_post, and made once per new table.
	void post(uint64_t serial, const float* amp, int count) noexcept {
		requests_.post(serial, amp, count);
		wake_.fetch_add(1, std::memory_order_release);
		wake_.notify_one();
	}
	partials::PhaseRequestSlot& requests() noexcept { return requests_; }
	[[nodiscard]] const partials::PhaseProfileStore& profiles() const noexcept { return profiles_; }

	/// TEST ONLY: optimisations completed, and the longest one took (ms).
	[[nodiscard]] uint64_t computedForTest() const noexcept { return computed_.load(std::memory_order_relaxed); }
	[[nodiscard]] double longestMsForTest() const noexcept { return longestMs_.load(std::memory_order_relaxed); }
	/// TEST ONLY: how many times the thread woke from its wait (an idle instance should not wake at all).
	[[nodiscard]] uint64_t wakesForTest() const noexcept { return wakes_.load(std::memory_order_relaxed); }
	/// TEST ONLY: run one poll on the CALLING thread (a test with no thread running drives the service by hand).
	bool pollOnceForTest() { return pollOnce(); }

private:
	void run() {
		while (running_.load(std::memory_order_relaxed)) {
			// THE COUNTER READ BEFORE THE SLOT IS ASKED: a post landing between the two moves it, so the wait below returns at once
			// rather than sleeping on a request already there.
			const uint32_t seen = wake_.load(std::memory_order_acquire);
			if (pollOnce()) {
				continue;
			}
			if (!running_.load(std::memory_order_relaxed)) {
				break;
			}
			wake_.wait(seen, std::memory_order_acquire); // until a post or stop() moves the counter
			wakes_.fetch_add(1, std::memory_order_relaxed);
		}
	}
	bool pollOnce() {
		if (!requests_.take(request_)) {
			return false;
		}
		const auto t0 = std::chrono::steady_clock::now();
		optimiser_.optimise(request_.amp.data(), request_.count, profile_, kRounds);
		profile_.serial = request_.serial;
		profiles_.publish(profile_);
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		if (ms > longestMs_.load(std::memory_order_relaxed)) {
			longestMs_.store(ms, std::memory_order_relaxed);
		}
		computed_.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	partials::PhaseRequestSlot requests_;
	partials::PhaseProfileStore profiles_;
	partials::PhaseOptimiser optimiser_;
	partials::PhaseRequestSlot::Request request_{};
	partials::PhaseProfile profile_{};
	std::atomic<bool> running_{false};
	std::atomic<uint32_t> wake_{0};   ///< moved by every post and by stop(); the thread waits on it
	std::atomic<uint64_t> wakes_{0};  ///< how many times the thread woke (a test seam)
	std::atomic<uint64_t> computed_{0};
	std::atomic<double> longestMs_{0.0};
	std::thread thread_;
};

} // namespace feathers
