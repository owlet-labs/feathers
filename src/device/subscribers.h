/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

// **A LIST OF LISTENERS, EACH HELD BY A TOKEN THAT ENDS IT** — `ui::Watch`'s shape for a callback
// rather than a frame.
//
// THE PROBLEM IT REPLACES: a bridge that held ONE `std::function` listener. Two parties want to hear a
// macro's rename — the editor, to relabel its control, and the host, to ask the DAW for a rescan (A9)
// — and with one slot each overwrote the other, and the editor clearing its own on close killed the
// host's with it. A list fixes the overwrite; the TOKEN fixes the clearing, because a subscriber can
// only remove itself.
//
// **SAFE IN BOTH DESTRUCTION ORDERS**, as `Watch` is: the token holds a `weak_ptr` to a sentinel the
// list owns, so a token that outlives its list does nothing rather than reaching into a corpse.
//
// Main thread only, like every listener it replaces. A subscriber may drop its own token — or another's
// — from inside `notify`: the walk is over a snapshot, and an entry removed mid-walk is skipped. One
// subscribed mid-walk is not in the snapshot, so it is first called on the NEXT notify.
//
// **A SUBSCRIBER MUST NOT DESTROY THE LIST DURING NOTIFY.** The snapshot keeps the callbacks alive, but
// `notify` still reads `entries_` between calls to ask whether the next one is subscribed — through a
// `this` that would be gone.
//
// **NOTIFY COPIES THE CALLBACKS, AND SO ALLOCATES: MAIN THREAD ONLY, NEVER THE AUDIO THREAD.** The copy
// is the price of the mid-walk rules above, and it is paid on every call.

namespace feathers::device {

class Subscription;

/// What a token reaches through its sentinel. Not a public interface; `Subscribers` is the only one.
class SubscriptionList {
public:
	virtual void unsubscribe(uint64_t id) noexcept = 0;

protected:
	~SubscriptionList() = default;
};

/// **A SUBSCRIPTION THAT ENDS ITSELF.** Move-only; an empty one (default-built, or moved from) ends
/// nothing. `reset()` ends it early.
class Subscription {
public:
	Subscription() = default;
	Subscription(std::weak_ptr<SubscriptionList*> list, uint64_t id) noexcept : list_(std::move(list)), id_(id) {}
	Subscription(const Subscription&) = delete;
	Subscription& operator=(const Subscription&) = delete;
	Subscription(Subscription&& other) noexcept : list_(std::move(other.list_)), id_(std::exchange(other.id_, 0)) {}
	Subscription& operator=(Subscription&& other) noexcept {
		if (this != &other) {
			reset();
			list_ = std::move(other.list_);
			id_ = std::exchange(other.id_, 0);
		}
		return *this;
	}
	~Subscription() { reset(); }

	void reset() noexcept {
		if (id_ == 0) {
			return;
		}
		if (const std::shared_ptr<SubscriptionList*> list = list_.lock()) {
			(*list)->unsubscribe(id_);
		}
		list_.reset();
		id_ = 0;
	}

	[[nodiscard]] bool active() const noexcept { return id_ != 0 && !list_.expired(); }

private:
	std::weak_ptr<SubscriptionList*> list_;
	uint64_t id_ = 0;
};

template <typename... Args>
class Subscribers final : public SubscriptionList {
public:
	using Callback = std::function<void(Args...)>;

	Subscribers() = default;
	Subscribers(const Subscribers&) = delete;
	Subscribers& operator=(const Subscribers&) = delete;
	~Subscribers() = default;

	/// **SUBSCRIBE, AND HOLD WHAT COMES BACK.** An empty callback subscribes nothing and returns an empty
	/// token.
	[[nodiscard]] Subscription subscribe(Callback callback) {
		if (!callback) {
			return {};
		}
		const uint64_t id = ++lastId_;
		entries_.push_back({ id, std::move(callback) });
		return Subscription(alive_, id);
	}

	/// Tell every subscriber, in the order they subscribed.
	void notify(Args... args) const {
		// A SNAPSHOT, because a subscriber may unsubscribe itself or another from inside its call.
		const std::vector<Entry> snapshot = entries_;
		for (const Entry& entry : snapshot) {
			if (subscribed(entry.id)) {
				entry.callback(args...);
			}
		}
	}

	[[nodiscard]] size_t size() const noexcept { return entries_.size(); }

	void unsubscribe(uint64_t id) noexcept override {
		std::erase_if(entries_, [id](const Entry& entry) { return entry.id == id; });
	}

private:
	struct Entry {
		uint64_t id;
		Callback callback;
	};

	[[nodiscard]] bool subscribed(uint64_t id) const noexcept {
		for (const Entry& entry : entries_) {
			if (entry.id == id) {
				return true;
			}
		}
		return false;
	}

	std::vector<Entry> entries_;
	uint64_t lastId_ = 0;
	/// THE SENTINEL TOKENS HOLD WEAKLY. It dies with the list, so a later token finds nothing to end.
	std::shared_ptr<SubscriptionList*> alive_ = std::make_shared<SubscriptionList*>(this);
};

} // namespace feathers::device
