/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/rack_strike_hub.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace feathers::ui {

struct RackStrikeRegistry {
	std::vector<std::unique_ptr<RackStrikeHub>> hubs;
	static RackStrikeRegistry& get() {
		static RackStrikeRegistry registry;
		return registry;
	}
};

RackStrikeHub* RackStrikeHub::attach(const RackStrikeKey& key, RackStrikePulse* pulse) {
	if (!key.valid() || pulse == nullptr) {
		return nullptr;
	}
	auto& hubs = RackStrikeRegistry::get().hubs;
	RackStrikeHub* hub = nullptr;
	for (auto& h : hubs) {
		if (h->key_ == key) {
			hub = h.get();
		}
	}
	if (hub == nullptr) {
		hubs.push_back(std::make_unique<RackStrikeHub>());
		hub = hubs.back().get();
		hub->key_ = key;
	}
	for (size_t i = 0; i < hub->count_; ++i) {
		if (hub->subs_[i] == pulse) {
			return hub;
		}
	}
	if (hub->count_ >= kMaxSubscribers) {
		return nullptr; // past the room: that pulse takes the mask on its own, as it did before there was a hub
	}
	hub->subs_[hub->count_++] = pulse;
	return hub;
}

void RackStrikeHub::detach(const RackStrikeKey& key, RackStrikePulse* pulse) {
	auto& hubs = RackStrikeRegistry::get().hubs;
	for (auto it = hubs.begin(); it != hubs.end(); ++it) {
		RackStrikeHub& hub = **it;
		if (!(hub.key_ == key)) {
			continue;
		}
		for (size_t i = 0; i < hub.count_; ++i) {
			if (hub.subs_[i] == pulse) {
				if (hub.lastTaker_ == pulse) {
					hub.lastTaker_ = nullptr;
				}
				hub.subs_[i] = hub.subs_[--hub.count_];
				hub.subs_[hub.count_] = nullptr;
				break;
			}
		}
		if (hub.count_ == 0) {
			hubs.erase(it);
		}
		return;
	}
}

size_t RackStrikeHub::hubCountForTest() noexcept {
	return RackStrikeRegistry::get().hubs.size();
}

size_t RackStrikeHub::subscribersForTest(const RackStrikeKey& key) noexcept {
	for (auto& h : RackStrikeRegistry::get().hubs) {
		if (h->key_ == key) {
			return h->count_;
		}
	}
	return 0;
}

RackStrikePulse* const* RackStrikeHub::begin() const noexcept {
	return subs_;
}
RackStrikePulse* const* RackStrikeHub::end() const noexcept {
	return subs_ + count_;
}

} // namespace feathers::ui
