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

#include "plugin/ui/rack_front.h"

#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_bridge.h"

#include <algorithm>

namespace feathers::ui {

namespace pr = rack::parallel;

RackFrontColumn::RackFrontColumn(backplane::DeviceHost& host, DeviceInstanceId rack, WidgetRegistry& registry, Notifier* notifier)
    : host_(host), rack_(rack), registry_(registry), notifier_(notifier) {}

RackFrontColumn::~RackFrontColumn() {
	dropColumn();
}

void RackFrontColumn::bindHistory() {
	history_.forwardTo(bridge_ != nullptr ? outerHistory_ : nullptr, bridge_ != nullptr ? bridge_->fragmentLife() : std::weak_ptr<const void> {});
}

void RackFrontColumn::dropColumn() {
	if (column_ != nullptr) {
		removeChild(column_.get());
		column_.reset();
	}
	bridge_ = nullptr;
	chainId_ = 0xFFFF;
	bindHistory();
}

bool RackFrontColumn::follow(size_t viewed, bool open, bool shown) {
	// THE RACK ASKED AFRESH (its handle is contemporaneous, never remembered), and the chain's identity compared, never dereferenced.
	pr::Rack* rack = pr::rackOf(host_.containerSurfaceOf(rack_));
	rack::InnerChain* chain = rack != nullptr && viewed < rack->chainCount() ? rack->chainAt(viewed) : nullptr;
	rack::RackBridge* bridge = chain != nullptr ? chain->bridge() : nullptr;
	const uint16_t id = bridge != nullptr ? rack->chainIdAt(viewed) : uint16_t { 0xFFFF };
	bool changed = false;
	if (bridge != bridge_ || id != chainId_ || (bridge == nullptr) != (column_ == nullptr)) {
		dropColumn();
		bridge_ = bridge;
		chainId_ = id;
		bindHistory();
		if (bridge_ != nullptr) {
			column_ = std::make_unique<ChainColumn>(*bridge_, registry_, mod_, history_, notifier_);
			// WHAT A REBUILT COLUMN HAS NEVER BEEN TOLD (trap T9's list): where its panels go and which nest it is in.
			column_->setPresentedOn(presenter_);
			column_->setNest(nest_);
			column_->setPadding(0.0f, 0.0f, kGap);
			column_->setHorizontal(horizontal_);
			addChild(column_.get());
		}
		changed = true;
	}
	if (column_ != nullptr) {
		column_->syncCards();
		// THE CLOSED CHAIN IS ITS FIRST CARD; an EMPTY chain keeps the (+) so its first device can be added from here.
		// NOT SHOWN: no card at all (the Pads view's put-away chain); else the first alone (closed) or all of it (open).
		column_->setShownLimit(!shown ? 0 : (open ? static_cast<size_t>(-1) : 1));
		column_->setAddShown(shown && (open || column_->deviceCount() == 0));
		changed = changed || open_ != open || shown_ != shown;
		open_ = open;
		shown_ = shown;
		column_->setBounds(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
	}
	return changed;
}

size_t RackFrontColumn::updateCards() {
	return column_ != nullptr ? column_->updateCards() : 0;
}

void RackFrontColumn::setOuterModulation(ModState* mod, EditHistory* history, std::function<DeviceInstanceId()> holder) {
	outerHistory_ = history;
	bindHistory();
	mod_.setParent(mod);
	// THE HOLDER'S OWN SOURCES ARE NOT SPELLED INWARD HERE (a rack's chains publish no core sources of the holder's): an outer source armed
	// outside is an OUTER source in here, as the rack window's rails take it.
	if (mod == nullptr || !holder) {
		mod_.setOuterSpelling(nullptr);
		return;
	}
	mod_.setOuterSpelling([](device::Source source) { return source.asOuter(); });
}

void RackFrontColumn::setPresentedOn(SurfaceLayers* host) {
	presenter_ = host;
	if (column_ != nullptr) {
		column_->setPresentedOn(host);
	}
}

void RackFrontColumn::setNest(DeviceNest nest) {
	nest_ = nest;
	if (column_ != nullptr) {
		column_->setNest(nest);
	}
}

void RackFrontColumn::setHorizontal(bool horizontal) {
	horizontal_ = horizontal;
	if (column_ != nullptr) {
		column_->setHorizontal(horizontal);
	}
}

float RackFrontColumn::naturalExtent(float cross) const {
	if (column_ == nullptr) {
		return 22.0f; // the line that says there is nothing running to show
	}
	return column_->contentHeightFor(cross);
}

size_t RackFrontColumn::deviceCount() const {
	return column_ != nullptr ? column_->deviceCount() : 0;
}

size_t RackFrontColumn::shownCount() const {
	return column_ != nullptr ? column_->shownCount() : 0;
}

void RackFrontColumn::resized() {
	if (column_ != nullptr) {
		column_->setBounds(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
	}
}

void RackFrontColumn::draw(visage::Canvas& canvas) {
	if (column_ == nullptr) {
		canvas.setColor(kMuted);
		canvas.text("not running here", font(kControlSize), visage::Font::kLeft, 0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
	}
}

} // namespace feathers::ui
