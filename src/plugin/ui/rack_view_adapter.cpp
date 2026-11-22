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

#include "plugin/ui/rack_view_adapter.h"

#include "fx/drum/drum_types.h"
#include "plugin/device_registry.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/parallel_rack_card.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace feathers::ui {

namespace pr = rack::parallel;

RackViewAdapter::RackViewAdapter(backplane::DeviceHost& host, DeviceInstanceId rack, Controls controls)
    : host_(host), id_(rack), controls_(controls) {}

pr::Rack* RackViewAdapter::rack() const {
	return pr::rackOf(host_.containerSurfaceOf(id_));
}

int RackViewAdapter::fieldInt(pr::Param p) const {
	return static_cast<int>(std::lround(host_.fieldAt(id_, static_cast<size_t>(p))));
}

void RackViewAdapter::writeField(pr::Param p, int value) {
	if (fieldInt(p) != value) {
		host_.setField(id_, static_cast<size_t>(p), static_cast<float>(value));
	}
}

RackViewKind RackViewAdapter::view() const {
	switch (std::clamp(fieldInt(pr::Param::View), 0, 3)) {
	case 1: return RackViewKind::Tabs;
	case 3: return RackViewKind::List;
	case 2: return RackViewKind::Pads;
	default: return RackViewKind::Strips;
	}
}

RackRoutingKind RackViewAdapter::routing() const {
	const auto mode = static_cast<pr::Mode>(std::clamp(fieldInt(pr::Param::Mode), 0, static_cast<int>(pr::Mode::kCount) - 1));
	if (mode == pr::Mode::Select) {
		return RackRoutingKind::Select;
	}
	if (mode == pr::Mode::Keyed) {
		return RackRoutingKind::Keyed;
	}
	return RackRoutingKind::Parallel;
}

size_t RackViewAdapter::chainCount() const {
	const pr::Rack* r = rack();
	return r != nullptr ? r->chainCount() : 0;
}

size_t RackViewAdapter::chainSlots() const {
	return chainCount();
}

std::string RackViewAdapter::chainTitle(size_t slot) const {
	const pr::Rack* r = rack();
	if (r == nullptr || slot >= r->chainCount()) {
		return {};
	}
	const rack::InnerChain* chain = r->chainAt(slot);
	const rack::RackBridge* bridge = chain != nullptr ? chain->bridge() : nullptr;
	if (bridge == nullptr) {
		return {};
	}
	for (size_t p = 0; p < bridge->chainLength(); ++p) {
		if (const device::Descriptor* d = bridge->descriptorOf(bridge->deviceAtPosition(p)); d != nullptr && d->traits.name != nullptr) {
			return backplane::instanceBaseName(*bridge, bridge->deviceAtPosition(p), d->traits.name);
		}
	}
	return {};
}

std::vector<std::string> RackViewAdapter::deviceNames(size_t slot) const {
	std::vector<std::string> out;
	const pr::Rack* r = rack();
	if (r == nullptr || slot >= r->chainCount()) {
		return out;
	}
	const rack::InnerChain* chain = r->chainAt(slot);
	const rack::RackBridge* bridge = chain != nullptr ? chain->bridge() : nullptr;
	for (size_t p = 0; bridge != nullptr && p < bridge->chainLength(); ++p) {
		if (const device::Descriptor* d = bridge->descriptorOf(bridge->deviceAtPosition(p)); d != nullptr && d->traits.name != nullptr) {
			out.push_back(backplane::instanceBaseName(*bridge, bridge->deviceAtPosition(p), d->traits.name));
		}
	}
	return out;
}

RackChainView RackViewAdapter::chain(size_t slot) const {
	const pr::Rack* r = rack();
	if (r == nullptr || slot >= r->chainCount()) {
		return {};
	}
	// ONE LABEL, the rack's (`Rack::chainLabel`): the user's name, else the first device's, else "Chain N" — what every reader of a chain shows.
	return { true, r->chainLabel(slot), r->chainKey(slot) == pr::kAnyKey ? kChainAnyKey : r->chainKey(slot), r->chainChoke(slot) };
}

std::string RackViewAdapter::chainLabel(size_t slot) const {
	const pr::Rack* r = rack();
	return r != nullptr ? r->chainLabel(slot) : std::string {};
}

uint16_t RackViewAdapter::chainIdAt(size_t slot) const {
	const pr::Rack* r = rack();
	return r != nullptr ? r->chainIdAt(slot) : uint16_t { 0xFFFF };
}

size_t RackViewAdapter::slotOfChainId(uint16_t id) const {
	const pr::Rack* r = rack();
	const size_t at = r != nullptr ? r->indexOfChain(id) : SIZE_MAX;
	return at == SIZE_MAX ? kNoChain : at;
}

std::string RackViewAdapter::chainUserName(size_t slot) const {
	const pr::Rack* r = rack();
	return r != nullptr ? std::string(r->chainName(slot)) : std::string {};
}

bool RackViewAdapter::renameChain(size_t slot, std::string_view name, EditHistory* history) {
	pr::Rack* r = rack();
	if (r == nullptr || slot >= r->chainCount()) {
		return false;
	}
	const std::string before(r->chainName(slot));
	const uint16_t id = r->chainIdAt(slot);
	if (!r->setChainName(slot, name)) {
		return false;
	}
	if (history != nullptr) {
		history->record(std::make_unique<ChainNameEdit>(host_, id_, id, before, std::string(r->chainName(slot))));
	}
	return true;
}

size_t RackViewAdapter::viewedChain() const {
	const size_t n = chainCount();
	return n == 0 ? 0 : std::min(static_cast<size_t>(std::max(0, fieldInt(pr::Param::ViewedChain))), n - 1);
}

void RackViewAdapter::viewChain(size_t slot) {
	writeField(pr::Param::ViewedChain, static_cast<int>(slot));
}

size_t RackViewAdapter::playingChain() const {
	const size_t n = chainCount();
	if (n == 0) {
		return 0;
	}
	const double position = controls_.position != nullptr ? controls_.position->value() : 0.0;
	return pr::playingChain(position, n, fieldInt(pr::Param::Playing));
}

void RackViewAdapter::setPlayingChain(size_t slot) {
	if (controls_.playing != nullptr) {
		controls_.playing->setValue(static_cast<double>(slot + 1));
	}
}

int RackViewAdapter::page() const {
	return std::clamp(fieldInt(pr::Param::PadsPage), 0, kPadPages - 1);
}

void RackViewAdapter::setPage(int page) {
	writeField(pr::Param::PadsPage, std::clamp(page, 0, kPadPages - 1));
}

bool RackViewAdapter::expanded(size_t slot) const {
	const pr::Rack* r = rack();
	return r != nullptr && slot < r->chainCount() && r->open(slot);
}

void RackViewAdapter::setExpanded(size_t slot, bool expanded) {
	if (pr::Rack* r = rack(); r != nullptr && slot < r->chainCount()) {
		r->setOpen(slot, expanded);
	}
}

RackStrikeKey RackViewAdapter::strikeKey() const {
	const pr::Rack* r = rack();
	return { r != nullptr ? r->serial() : 0 };
}

bool RackViewAdapter::followPlayed() const {
	return fieldInt(pr::Param::FollowPlayed) >= 1;
}

RackKeyMask RackViewAdapter::takeStruck() {
	RackKeyMask out {};
	if (pr::Rack* r = rack(); r != nullptr) {
		uint64_t words[2] = { 0, 0 };
		r->takeStrikeMask(words);
		out = { words[0], words[1] };
	}
	return out;
}

size_t RackViewAdapter::addKeyedChain(int key, EditHistory* history) {
	return addKeyedChain(key, fx::drumdevice::kTraits.tag, history);
}

std::vector<const device::Descriptor*> RackViewAdapter::addableDevices() const {
	std::vector<const device::Descriptor*> out;
	const pr::Rack* r = rack();
	if (r == nullptr || !r->adopted()) {
		return out;
	}
	for (const device::Descriptor& d : kDevices) {
		if (r->catalogue()(d.traits.tag) != nullptr) {
			out.push_back(&d);
		}
	}
	return out;
}

void RackViewAdapter::setFirstDeviceField(size_t slot, size_t control, float value) {
	pr::Rack* r = rack();
	rack::InnerChain* chain = r != nullptr && slot < r->chainCount() ? r->chainAt(slot) : nullptr;
	rack::RackBridge* bridge = chain != nullptr ? chain->bridge() : nullptr;
	if (bridge != nullptr && bridge->chainLength() > 0) {
		bridge->setField(bridge->deviceAtPosition(0), control, value);
	}
}

size_t RackViewAdapter::addKeyedChain(int key, const char* tag, EditHistory* history) {
	pr::Rack* r = rack();
	if (r == nullptr || key < 0 || key > 127 || tag == nullptr) {
		return kNoChain;
	}
	const size_t at = r->addChain();
	if (at == SIZE_MAX) {
		return kNoChain;
	}
	r->setChainKey(at, key);
	// A CHAIN MADE FROM A PAD STARTS OPEN (its rail whole in the window, its cards whole in front of the card): the hand that made it is about to
	// use it, and a fold hides the device it just chose. (A chain added by "+ Chain" keeps the rack's own default, folded.)
	r->setOpen(at, true);
	rack::InnerChain* chain = r->chainAt(at);
	rack::RackBridge* bridge = chain != nullptr ? chain->bridge() : nullptr;
	const DeviceInstanceId made = bridge != nullptr ? bridge->addDevice(tag) : kNoInstance;
	if (made == kNoInstance) {
		// THE CHAIN REFUSED THE DEVICE: nothing was asked for but the pair, so neither stays.
		(void)r->removeChain(at);
		return kNoChain;
	}
	// A DRUM IS SET TO ANY KEY (so the chain's own key filter does the answering).
	if (std::strcmp(tag, fx::drumdevice::kTraits.tag) == 0) {
		bridge->setField(made, static_cast<size_t>(fx::drumdevice::Param::Keys), 1.0f);
	}
	if (history != nullptr) {
		history->record(std::make_unique<ChainPresenceEdit>(host_, id_, at, r->snapshotChain(at), true));
	}
	return at;
}

} // namespace feathers::ui
