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

#include "plugin/ui/rack_window_pane.h"

#include "plugin/ui/chain_column.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {

bool sameLife(const std::weak_ptr<const void>& a, const std::weak_ptr<const void>& b) {
	return !a.owner_before(b) && !b.owner_before(a);
}

} // namespace

RackWindowPane::RackWindowPane() {
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (enableBounds().contains(visage::Point { static_cast<float>(e.position.x), static_cast<float>(e.position.y) })) {
			toggleEnable();
		}
	};
	onDpiChange() += [this] {
		setBounds(bounds());
		resized();
	};
	installTooltipHover(*this, *this);
	setVisible(false);
}

RackWindowPane::~RackWindowPane() {
	release();
}

void RackWindowPane::drop() {
	// OFF THE WATCHER LIST WHILE THE TREE IS ALIVE, then out of the frame tree, then freed (`ChainColumn::~ChainColumn`'s order).
	if (content_ != nullptr) {
		forgetFrameTree(content_.get(), nullptr, nullptr, modulation_);
		removeChild(content_.get());
		content_.reset();
	}
	if (card_ != nullptr) {
		forgetFrameTree(card_.get(), nullptr, nullptr, modulation_);
		removeChild(card_.get());
		card_.reset();
	}
	hosting_ = Hosting::Nothing;
	chainId_ = 0xFFFF;
	life_.reset();
	device_ = kNoInstance;
	desc_ = nullptr;
	modulation_ = nullptr;
	setVisible(false);
}

void RackWindowPane::release() {
	drop();
}

bool RackWindowPane::validate(uint16_t chainId, const std::weak_ptr<const void>& life) {
	if (hosting_ == Hosting::Nothing) {
		return false;
	}
	if (chainId == chainId_ && !life_.expired() && !life.expired() && sameLife(life, life_)) {
		return false;
	}
	drop();
	return true;
}

bool RackWindowPane::follow(ChainColumn* column, uint16_t chainId) {
	const DeviceInstanceId first = column != nullptr && column->deviceCount() > 0 ? column->deviceAt(0) : kNoInstance;
	const device::Descriptor* desc = first != kNoInstance ? column->bridge().descriptorOf(first) : nullptr;
	if (column == nullptr || first == kNoInstance || desc == nullptr) {
		const bool had = hosting_ != Hosting::Nothing;
		drop();
		return had;
	}
	const std::weak_ptr<const void> life = column->bridge().fragmentLife();
	if (hosting_ != Hosting::Nothing && chainId == chainId_ && first == device_ && desc == desc_ && !life_.expired() && sameLife(life, life_)) {
		return false;
	}
	drop();
	chainId_ = chainId;
	life_ = life;
	device_ = first;
	desc_ = desc;
	modulation_ = &column->modulationWatchers();
	++builds_;
	content_ = column->buildWindowContent(first);
	if (content_ != nullptr) {
		hosting_ = Hosting::Window;
		// A PANE NEVER NESTS: a rack hosted here lays itself out as with Cards in front.
		if (auto* rack = dynamic_cast<ParallelRackExpanded*>(content_.get()); rack != nullptr) {
			rack->setHostedInPane(true);
		}
		content_->setPresentedOn(presenter_);
		addChild(content_.get());
	}
	else {
		card_ = column->buildCard(first);
		if (card_ != nullptr) {
			hosting_ = Hosting::Card;
			addChild(card_.get());
		}
	}
	if (hosting_ == Hosting::Nothing) {
		drop();
		return true;
	}
	setVisible(true);
	resized();
	redraw();
	return true;
}

void RackWindowPane::setPresentedOn(SurfaceLayers* host) {
	presenter_ = host;
	if (content_ != nullptr) {
		content_->setPresentedOn(host);
	}
}

size_t RackWindowPane::update() {
	size_t told = 0;
	if (content_ != nullptr && content_->update()) {
		++told;
	}
	if (card_ != nullptr && card_->update()) {
		++told;
	}
	if (hosting_ == Hosting::Window && content_ != nullptr) {
		const bool enabled = content_->deviceEnabled();
		if (enabled != enableShown_) {
			enableShown_ = enabled;
			redraw();
			++told;
		}
	}
	return told;
}

visage::Point RackWindowPane::naturalSize() const {
	switch (hosting_) {
	case Hosting::Window: {
		const visage::Point m = content_->minimumSize();
		return { m.x, kHeaderHeight + m.y };
	}
	case Hosting::Card: {
		const float cross = FxLane::kWidth - 2.0f * ChainColumn::kPadX;
		return { cross + 2.0f * kCardMargin, card_->laneExtent(cross) + 2.0f * kCardMargin };
	}
	default: return { 0.0f, 0.0f };
	}
}

visage::Bounds RackWindowPane::contentBounds() const {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	if (hosting_ == Hosting::Window) {
		// THE CONTENT'S OWN NATURAL SIZE, at the top left under the header: never stretched, so its extent is what its window's is.
		const visage::Point m = content_->minimumSize();
		return { 0.0f, kHeaderHeight, std::min(m.x, w), std::min(m.y, std::max(0.0f, h - kHeaderHeight)) };
	}
	if (hosting_ == Hosting::Card) {
		const float cross = FxLane::kWidth - 2.0f * ChainColumn::kPadX;
		const float tall = card_->laneExtent(cross);
		return { std::round((w - cross) * 0.5f), std::round((h - tall) * 0.5f), cross, tall };
	}
	return {};
}

visage::Bounds RackWindowPane::enableBounds() const {
	if (hosting_ != Hosting::Window || content_ == nullptr || !content_->hasDeviceEnable()) {
		return {};
	}
	return { static_cast<float>(width()) - 10.0f - kEnableSize, std::round((kHeaderHeight - kEnableSize) * 0.5f), kEnableSize, kEnableSize };
}

void RackWindowPane::toggleEnable() {
	if (content_ != nullptr) {
		(void)content_->toggleDeviceEnable();
		enableShown_ = content_->deviceEnabled();
		redraw();
	}
}

void RackWindowPane::resized() {
	// RE-SET UNCONDITIONALLY: a scale change moves no logical rectangle, and `setBounds` de-duplicates on both.
	const visage::Bounds b = contentBounds();
	if (content_ != nullptr) {
		content_->setBounds(b.x(), b.y(), b.width(), b.height());
	}
	if (card_ != nullptr) {
		card_->setBounds(b.x(), b.y(), b.width(), b.height());
	}
}

const char* RackWindowPane::tooltipAt(visage::Point at) const {
	if (enableBounds().contains(at)) {
		return "Turns this device on or off";
	}
	return nullptr;
}

void RackWindowPane::draw(visage::Canvas& canvas) {
	if (hosting_ != Hosting::Window) {
		return;
	}
	const float w = static_cast<float>(width());
	canvas.setColor(kInk);
	drawTracked(canvas, content_->headerTitle(), font(kControlSize), kControlTracking, 10.0f, 0.0f, kHeaderHeight);
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.fill(0.0f, kHeaderHeight - 1.0f, w, 1.0f);
	if (const visage::Bounds sw = enableBounds(); sw.width() > 0.0f) {
		canvas.setColor(enableShown_ ? kAccent : kMuted);
		if (enableShown_) {
			canvas.roundedRectangle(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f);
		}
		else {
			canvas.roundedRectangleBorder(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f, 1.0f);
		}
	}
}

} // namespace feathers::ui
