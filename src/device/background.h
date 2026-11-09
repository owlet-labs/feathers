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

// **BACKGROUND DERIVATION — A DEVICE-API ADDITION**.
//
// Some state a device plays from is DERIVED from what it stores, and deriving it is too slow for either thread a device
// otherwise runs on: the audio thread may not allocate or wait, and the UI must never stall (the Pattern's likes warp is a
// 1.7 s fit). A `Deriver` is a device's handle on ONE process-wide worker thread:
//
//   - `request(work)` (any non-audio thread; cheap: it queues and returns) asks for `work` to run on the worker. A newer
//     request SUPERSEDES the older: one still queued is dropped, and one running sees `Job::stopped()` turn true and should
//     return early. Each request gets a SERIAL, and `Job::current()` is true only while no newer request has been made.
//   - `publish(job, fn)` (from inside the work) runs `fn` under the deriver's lock IFF the job is still current — so a
//     superseded job can never publish, however its timing falls. What `fn` publishes to is the device's business; the
//     audio thread must read it lock-free (the Pattern double-buffers it, keyed by the serial).
//   - THE DESTRUCTOR CANCELS AND WAITS: a queued job is dropped and a running one is stopped and waited for, so the
//     work's captures (the device itself) outlive it. Declare the Deriver AFTER everything its work touches, so it is
//     destroyed first.
//
// NEVER CALLED FROM THE AUDIO THREAD: `request` takes a mutex and may allocate.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace feathers::device {

class Deriver {
public:
	/// What a running piece of work can ask.
	class Job {
	public:
		/// True once a newer request has been made, or the deriver is being destroyed: return as soon as convenient.
		[[nodiscard]] bool stopped() const noexcept;
		[[nodiscard]] uint64_t serial() const noexcept { return serial_; }
		/// Made by the worker (and by a test that runs work synchronously).
		Job(const Deriver& owner, uint64_t serial) : owner_(owner), serial_(serial) {}

	private:
		const Deriver& owner_;
		uint64_t serial_;
	};
	using Work = std::function<void(const Job&)>;

	/// Allocates nothing: the shared state is made by the first `request`, so a device may be built on any thread.
	Deriver() = default;
	~Deriver();
	Deriver(const Deriver&) = delete;
	Deriver& operator=(const Deriver&) = delete;

	/// Queues `work`, superseding any earlier request; returns its serial.
	uint64_t request(Work work);
	/// Runs `fn` under the lock iff `job` is still the newest request (and the deriver not closing); true if it ran.
	bool publish(const Job& job, const std::function<void()>& fn);
	/// TESTS AND TEARDOWN ONLY: blocks until nothing of this deriver's is queued or running.
	void waitIdle();
	/// The newest request's serial (0: none).
	[[nodiscard]] uint64_t latest() const noexcept { return latest_.load(std::memory_order_acquire); }

	struct State; ///< shared with the worker: the work queued, the work running, the lock

private:
	std::shared_ptr<State> state_;
	std::atomic<uint64_t> latest_ { 0 };
};

} // namespace feathers::device
