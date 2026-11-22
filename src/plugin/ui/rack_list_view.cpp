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

#include "plugin/ui/rack_list_view.h"

#include "plugin/ui/card_wheel_gate.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace {
constexpr float kScrollNotch = 24.0f;
constexpr float kDragStart = 4.0f;
constexpr float kDotArea = 12.0f;
} // namespace

RackListView::RackListView(RackViewAdapter& model, backplane::DeviceHost& host, DeviceInstanceId rack, RackChainRename& rename)
    : model_(model), host_(host), rack_(rack), rename_(rename) {
	installTooltipHover(*this, *this);
	// THE GUTTER IS THE LIST'S OWN SURFACE: a press there sets the playing chain (Select), or views the row's chain.
	onMouseDown() = [this](const visage::MouseEvent& e) { pressGutter(e.position); };
	onMouseMove() = [this](const visage::MouseEvent& e) {
		const int now = e.position.x < kGutter ? rowAt(e.position) : -1;
		if (now != hover_) {
			hover_ = now;
			redraw();
		}
	};
	onMouseExit() = [this](const visage::MouseEvent&) {
		if (hover_ != -1) {
			hover_ = -1;
			redraw();
		}
	};
	// A SCALE CHANGE MOVES NOTHING LOGICAL: the rows are placed again, unconditionally.
	onDpiChange() += [this] { layoutRows(); };
}

RackListView::~RackListView() = default;

float RackListView::maxScroll() const {
	const float content = rows_.empty() ? 0.0f : heightFor(rows_.size());
	return std::max(0.0f, content - static_cast<float>(height()));
}

void RackListView::setScroll(float y) {
	const float clamped = std::clamp(y, 0.0f, maxScroll());
	if (clamped == scroll_) {
		return;
	}
	scroll_ = clamped;
	layoutRows();
	redraw();
}

visage::Bounds RackListView::rowBoundsForTest(size_t slot) const {
	const float x = kGutter;
	return { x, static_cast<float>(slot) * kPitch - scroll_, std::max(0.0f, static_cast<float>(width()) - kGutter - kBarWidth), kRowHeight };
}

bool RackListView::rowVisibleForTest(size_t slot) const {
	const visage::Bounds b = rowBoundsForTest(slot);
	return b.y() >= 0.0f && b.bottom() <= static_cast<float>(height());
}

void RackListView::layoutRows() {
	for (size_t i = 0; i < rows_.size(); ++i) {
		const visage::Bounds b = rowBoundsForTest(i);
		ChainStrip& strip = *rows_[i].strip;
		// A ROW WHOLLY OUTSIDE THE FRAME IS HIDDEN, so no press and no tooltip finds it; one cut by an edge is drawn cut (the frame clips).
		strip.setVisible(b.bottom() > 0.0f && b.y() < static_cast<float>(height()));
		strip.setBounds(b); // unconditionally: a scale change moves no logical rectangle
	}
}

void RackListView::resized() {
	scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
	layoutRows();
}

void RackListView::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (Row& r : rows_) {
		r.strip->setModulation(mod, history);
	}
}

void RackListView::rebuild(const std::vector<uint16_t>& ids) {
	for (Row& r : rows_) {
		removeChild(r.strip.get());
	}
	rows_.clear();
	for (const uint16_t id : ids) {
		Row row;
		row.chain = id;
		row.strip = std::make_unique<ChainStrip>(host_, rack_, id, "Level");
		row.strip->setModulation(mod_, history_);
		ChainStrip* const raw = row.strip.get();
		const std::weak_ptr<void> alive = alive_;
		raw->setSurfaceHandler([this, alive, id](ChainStrip::Surface phase, const visage::MouseEvent& e) {
			if (alive.expired()) {
				return;
			}
			if (const size_t slot = model_.slotOfChainId(id); slot != kNoChain) {
				surface(slot, phase, e);
			}
		});
		raw->setContextHandler([this, alive, raw, id](uint16_t, const visage::Bounds& anchor) {
			if (alive.expired()) {
				return;
			}
			if (const size_t slot = model_.slotOfChainId(id); slot != kNoChain) {
				rename_.showMenu(slot, *raw, anchor);
			}
		});
		raw->setRenameHandler([this, alive, raw, id](uint16_t, const visage::Bounds& anchor) {
			if (alive.expired()) {
				return;
			}
			if (const size_t slot = model_.slotOfChainId(id); slot != kNoChain) {
				rename_.begin(slot, *raw, anchor);
			}
		});
		addChild(raw);
		rows_.push_back(std::move(row));
	}
	scroll_ = std::clamp(scroll_, 0.0f, maxScroll());
	layoutRows();
}

bool RackListView::refresh() {
	std::vector<uint16_t> ids;
	for (size_t i = 0; i < model_.chainSlots(); ++i) {
		if (model_.chain(i).exists) {
			ids.push_back(model_.chainIdAt(i));
		}
	}
	bool moved = false;
	if (ids.size() != rows_.size() || !std::equal(ids.begin(), ids.end(), rows_.begin(), [](uint16_t a, const Row& r) { return a == r.chain; })) {
		rebuild(ids);
		moved = true;
	}
	const size_t viewed = model_.viewedChain();
	for (size_t i = 0; i < rows_.size(); ++i) {
		ChainStrip& strip = *rows_[i].strip;
		std::string summary = summaryFn_ ? summaryFn_(i) : model_.chainLabel(i);
		if (summary != strip.summaryForTest()) {
			strip.setSummary(std::move(summary));
			moved = true;
		}
		strip.setSelected(i == viewed);
		moved = strip.refresh() || moved;
	}
	// A CHANGE OF THE VIEWED CHAIN FROM ELSEWHERE brings its row into view; a press on a row already is.
	if (viewed != lastViewed_) {
		lastViewed_ = viewed;
		scrollIntoView(viewed);
		moved = true;
	}
	if (moved) {
		redraw();
	}
	return moved;
}

void RackListView::scrollIntoView(size_t slot) {
	if (slot >= rows_.size()) {
		return;
	}
	const float top = static_cast<float>(slot) * kPitch;
	const float bottom = top + kRowHeight;
	if (bottom - scroll_ > static_cast<float>(height())) {
		setScroll(bottom - static_cast<float>(height()));
	}
	if (top - scroll_ < 0.0f) {
		setScroll(top);
	}
}

bool RackListView::mouseWheel(const visage::MouseEvent& e) {
	if (maxScroll() <= 0.0f) {
		return false; // nothing to scroll: the wheel goes on to whatever holds the list
	}
	const float before = scroll_;
	setScroll(scroll_ - e.precise_wheel_delta_y * kScrollNotch);
	return scroll_ != before;
}

int RackListView::rowAt(visage::Point at) const {
	if (at.y < 0.0f || at.y >= static_cast<float>(height())) {
		return -1;
	}
	const float y = at.y + scroll_;
	const int slot = static_cast<int>(std::floor(y / kPitch));
	if (slot < 0 || static_cast<size_t>(slot) >= rows_.size() || y - static_cast<float>(slot) * kPitch > kRowHeight) {
		return -1;
	}
	return slot;
}

visage::Point RackListView::dotCentreForTest(size_t slot) const {
	return { kGutter * 0.5f, static_cast<float>(slot) * kPitch - scroll_ + kRowHeight * 0.5f };
}

bool RackListView::dotLitForTest(size_t slot) const {
	return model_.routing() == RackRoutingKind::Select && model_.playingChain() == slot;
}

bool RackListView::inDot(visage::Point at, size_t slot) const {
	if (model_.routing() != RackRoutingKind::Select) {
		return false;
	}
	const visage::Point c = dotCentreForTest(slot);
	return std::fabs(at.x - c.x) <= kDotArea * 0.5f && std::fabs(at.y - c.y) <= kDotArea * 0.5f;
}

void RackListView::pressGutter(visage::Point at) {
	const int slot = rowAt(at);
	if (slot < 0 || at.x >= kGutter) {
		return;
	}
	if (inDot(at, static_cast<size_t>(slot))) {
		model_.setPlayingChain(static_cast<size_t>(slot)); // the playing chain, through its parameter; the viewed row does not move
	}
	else {
		model_.viewChain(static_cast<size_t>(slot));
	}
	redraw();
}

void RackListView::surface(size_t slot, ChainStrip::Surface phase, const visage::MouseEvent& e) {
	switch (phase) {
	case ChainStrip::Surface::Down:
		downY_ = e.window_position.y;
		downScroll_ = scroll_;
		dragging_ = false;
		surfaceDown_ = true;
		break;
	case ChainStrip::Surface::Drag:
		if (!surfaceDown_) {
			break;
		}
		if (!dragging_ && std::fabs(e.window_position.y - downY_) > kDragStart) {
			dragging_ = true;
		}
		if (dragging_) {
			setScroll(downScroll_ - (e.window_position.y - downY_)); // the content follows the hand
		}
		break;
	case ChainStrip::Surface::Up:
		// A PRESS THAT DID NOT DRAG VIEWS THE ROW (as a tab's press does); one that dragged only scrolled.
		if (surfaceDown_ && !dragging_) {
			model_.viewChain(slot);
			redraw();
		}
		surfaceDown_ = false;
		dragging_ = false;
		break;
	}
}

const char* RackListView::tooltipAt(visage::Point at) const {
	const int slot = rowAt(at);
	if (slot >= 0 && at.x < kGutter && inDot(at, static_cast<size_t>(slot))) {
		return "Make this the playing chain: the one the notes reach. The chain in front stays where it is";
	}
	return nullptr;
}

void RackListView::offeredControls(std::vector<Offered>& out) const {
	if (device::Modulatable* playing = model_.playingControl(); playing != nullptr) {
		out.push_back({ playing, Road::Gesture });
	}
}

void RackListView::draw(visage::Canvas& canvas) {
	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
	// THE PLAYING DOT (Select), as the tab's: lit on the playing chain alone, faint under the pointer.
	if (model_.routing() == RackRoutingKind::Select) {
		for (size_t i = 0; i < rows_.size(); ++i) {
			const visage::Point c = dotCentreForTest(i);
			if (c.y < 0.0f || c.y > static_cast<float>(height())) {
				continue;
			}
			if (model_.playingChain() == i) {
				canvas.setColor(kAccent);
				canvas.circle(c.x - 3.0f, c.y - 3.0f, 6.0f);
			}
			else if (hover_ == static_cast<int>(i)) {
				canvas.setColor(withAlpha(kAccent, 0.45f));
				canvas.circle(c.x - 3.0f, c.y - 3.0f, 6.0f);
			}
		}
	}
	// THE SCROLL'S EXTENT, a slim bar at the right edge only while there is something to scroll to.
	const float room = maxScroll();
	if (room > 0.0f) {
		const float h = static_cast<float>(height());
		const float content = heightFor(rows_.size());
		const float thumb = std::max(12.0f, h * h / content);
		const float at = (h - thumb) * (scroll_ / room);
		canvas.setColor(withAlpha(kMuted, 0.5f));
		canvas.fill(static_cast<float>(width()) - kBarWidth + 1.0f, at, kBarWidth - 2.0f, thumb);
	}
}

} // namespace feathers::ui
