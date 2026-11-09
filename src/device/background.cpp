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

#include "device/background.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace feathers::device {

struct Deriver::State {
	std::mutex lock;
	std::condition_variable idle;
	Work queued;
	uint64_t queuedSerial = 0;
	bool running = false;
	bool closing = false;
	std::atomic<bool> stop { false };
	Deriver* owner = nullptr;
};

namespace {

/// THE ONE WORKER: a FIFO of derivers with work queued, run one job at a time. Started on first use; joined at exit.
class Worker {
public:
	static Worker& instance() {
		static Worker w;
		return w;
	}
	void enqueue(const std::shared_ptr<Deriver::State>& s) {
		{
			const std::lock_guard<std::mutex> guard(lock_);
			ready_.push_back(s);
			if (!thread_.joinable()) {
				thread_ = std::thread([this] { run(); });
			}
		}
		wake_.notify_one();
	}
	~Worker() {
		{
			const std::lock_guard<std::mutex> guard(lock_);
			quit_ = true;
		}
		wake_.notify_one();
		if (thread_.joinable()) {
			thread_.join();
		}
	}

private:
	void run() {
		for (;;) {
			std::shared_ptr<Deriver::State> s;
			{
				std::unique_lock<std::mutex> guard(lock_);
				wake_.wait(guard, [this] { return quit_ || !ready_.empty(); });
				if (quit_) {
					return;
				}
				s = std::move(ready_.front());
				ready_.pop_front();
			}
			Deriver::Work work;
			uint64_t serial = 0;
			{
				const std::lock_guard<std::mutex> guard(s->lock);
				if (s->closing || !s->queued) {
					continue; // superseded into an earlier pass, or the deriver is gone
				}
				work = std::move(s->queued);
				s->queued = nullptr;
				serial = s->queuedSerial;
				s->running = true;
				s->stop.store(false, std::memory_order_release);
			}
			{
				const Deriver::Job job(*s->owner, serial);
				work(job);
			}
			{
				const std::lock_guard<std::mutex> guard(s->lock);
				s->running = false;
			}
			s->idle.notify_all();
		}
	}

	std::mutex lock_;
	std::condition_variable wake_;
	std::deque<std::shared_ptr<Deriver::State>> ready_;
	std::thread thread_;
	bool quit_ = false;
};

} // namespace

bool Deriver::Job::stopped() const noexcept {
	return owner_.state_->stop.load(std::memory_order_acquire) || owner_.latest() != serial_;
}

Deriver::~Deriver() {
	if (!state_) {
		return;
	}
	std::unique_lock<std::mutex> guard(state_->lock);
	state_->closing = true;
	state_->queued = nullptr;
	state_->stop.store(true, std::memory_order_release);
	state_->idle.wait(guard, [this] { return !state_->running; });
}

uint64_t Deriver::request(Work work) {
	if (!state_) {
		state_ = std::make_shared<State>();
		state_->owner = this;
	}
	uint64_t serial = 0;
	bool wasQueued = false;
	{
		const std::lock_guard<std::mutex> guard(state_->lock);
		serial = latest_.load(std::memory_order_relaxed) + 1;
		latest_.store(serial, std::memory_order_release); // a running job now reads stopped()
		wasQueued = static_cast<bool>(state_->queued);
		state_->queued = std::move(work);
		state_->queuedSerial = serial;
	}
	if (!wasQueued) {
		Worker::instance().enqueue(state_);
	}
	return serial;
}

bool Deriver::publish(const Job& job, const std::function<void()>& fn) {
	if (!state_) {
		return false;
	}
	const std::lock_guard<std::mutex> guard(state_->lock);
	if (state_->closing || latest_.load(std::memory_order_acquire) != job.serial()) {
		return false;
	}
	fn();
	return true;
}

void Deriver::waitIdle() {
	if (!state_) {
		return;
	}
	std::unique_lock<std::mutex> guard(state_->lock);
	state_->idle.wait(guard, [this] { return !state_->running && !state_->queued; });
}

} // namespace feathers::device
