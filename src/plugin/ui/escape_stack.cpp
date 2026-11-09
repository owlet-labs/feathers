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

#include "plugin/ui/escape_stack.h"

#include "plugin/ui/mod_state.h"

#include <algorithm>
#include <utility>

namespace feathers::ui {

struct EscapeTicket::Core {
	struct Entry {
		EscapeId id = kNoEscape;
		std::function<bool()> close;
	};
	/// BACK IS TOP: the most recently opened thing.
	std::vector<Entry> entries;

	std::vector<Entry>::iterator find(EscapeId id) {
		return std::find_if(entries.begin(), entries.end(), [id](const Entry& e) { return e.id == id; });
	}

	bool closeTop() {
		// THE TOP IS COPIED BEFORE IT IS CALLED: a close releases tickets (its own, and whatever closing it
		// took down with it) and may open something, so the vector is not to be trusted across the call.
		while (!entries.empty()) {
			const Entry top = entries.back();
			if (top.close && top.close()) {
				return true;
			}
			// ALREADY GONE — its owner went without saying so. Dropped, and the next one down is asked: a
			// stale entry must never be the reason the key is swallowed.
			if (const auto it = find(top.id); it != entries.end()) {
				entries.erase(it);
			}
		}
		return false;
	}
};

namespace {

/// ONE COUNTER FOR EVERY STACK, so an id names one entry in the process and a ticket can never land on
/// another stack's entry.
EscapeId nextEscapeId() {
	static EscapeId next = kNoEscape;
	return ++next;
}

} // namespace

EscapeTicket::EscapeTicket(EscapeTicket&& other) noexcept
    : core_(std::move(other.core_)), id_(std::exchange(other.id_, kNoEscape)) {}

EscapeTicket& EscapeTicket::operator=(EscapeTicket&& other) noexcept {
	if (this != &other) {
		release();
		core_ = std::move(other.core_);
		id_ = std::exchange(other.id_, kNoEscape);
	}
	return *this;
}

void EscapeTicket::release() {
	if (const std::shared_ptr<Core> core = core_.lock(); core != nullptr && id_ != kNoEscape) {
		if (const auto it = core->find(id_); it != core->entries.end()) {
			core->entries.erase(it);
		}
	}
	core_.reset();
	id_ = kNoEscape;
}

void EscapeTicket::raise() {
	const std::shared_ptr<Core> core = core_.lock();
	if (core == nullptr) {
		return;
	}
	if (const auto it = core->find(id_); it != core->entries.end()) {
		Core::Entry taken = std::move(*it);
		core->entries.erase(it);
		core->entries.push_back(std::move(taken));
	}
}

bool EscapeTicket::held() const {
	const std::shared_ptr<Core> core = core_.lock();
	return core != nullptr && core->find(id_) != core->entries.end();
}

EscapeStack::EscapeStack() : core_(std::make_shared<EscapeTicket::Core>()) {}

EscapeStack::~EscapeStack() = default;

EscapeTicket EscapeStack::push(std::function<bool()> close) {
	const EscapeId id = nextEscapeId();
	core_->entries.push_back({ id, std::move(close) });
	return EscapeTicket { core_, id };
}

bool EscapeStack::closeTop() {
	const std::shared_ptr<EscapeTicket::Core> core = core_; // a close may take this stack with it
	return core->closeTop();
}

bool EscapeTicket::closeTop() {
	// HELD BY A LOCAL for the whole call: a close may destroy the last owner of the stack.
	const std::shared_ptr<Core> core = core_.lock();
	return core != nullptr && core->closeTop();
}

size_t EscapeStack::size() const noexcept {
	return core_->entries.size();
}

EscapeId EscapeStack::topForTest() const noexcept {
	return core_->entries.empty() ? kNoEscape : core_->entries.back().id;
}

void ArmEscape::attach(ModState& root, EscapeStack& stack) {
	detach();
	mod_ = &root;
	stack_ = &stack;
	const auto open = [this] {
		if (ticket_.held()) {
			ticket_.raise();
			return;
		}
		// ESC DISARMS THE WHOLE TREE — and the disarm is what takes this entry off, through the observer.
		ticket_ = stack_->push([this] { return mod_->disarmTree() > 0; });
	};
	root.setTreeArmObserver([this, open](bool byHand) {
		if (byHand) {
			open();
		}
		else if (!mod_->armedInTree()) {
			ticket_.release();
		}
	});
	if (root.armedInTree()) {
		open();
	}
}

void ArmEscape::detach() {
	if (mod_ != nullptr) {
		mod_->setTreeArmObserver({});
	}
	ticket_.release();
	mod_ = nullptr;
	stack_ = nullptr;
}

} // namespace feathers::ui
