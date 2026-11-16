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

#include "plugin/ui/rack_views.h"

#include "fx/eq/eq_readout.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace feathers::ui {

namespace {

constexpr float kVelocity = 0.8f;

double steadyNowSeconds() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// The seed hue a chain is tinted with (its slot, as the strip's sounds are).
uint32_t hueOfSlot(size_t slot) {
	return kSeedHues[slot % kSeedHues.size()];
}

std::string labelOfChain(const RackChainView& c, size_t slot) {
	return c.name.empty() ? "Chain " + std::to_string(slot + 1) : c.name;
}

} // namespace

//--------------------------------------------------------------------------------------------------//
// THE STRIKE PULSE                                                                                    //
//--------------------------------------------------------------------------------------------------//

RackStrikePulse::RackStrikePulse() : clock_(steadyNowSeconds), reduced_(reducedMotionRequested()) {
	struckAt_.fill(-1.0e9);
}

float RackStrikePulse::level(int key, double nowSeconds) const noexcept {
	if (key < 0 || key >= 128) {
		return 0.0f;
	}
	if (reduced_) {
		return maskHas(steady_, key) ? 1.0f : 0.0f;
	}
	const double age = nowSeconds - struckAt_[static_cast<size_t>(key)];
	if (age < 0.0 || age >= kDecaySeconds) {
		return 0.0f;
	}
	return static_cast<float>(1.0 - age / kDecaySeconds);
}

bool RackStrikePulse::live(double nowSeconds) const noexcept {
	if (reduced_) {
		return maskAny(steady_);
	}
	for (int k = 0; k < 128; ++k) {
		if (level(k, nowSeconds) > 0.0f) {
			return true;
		}
	}
	return false;
}

size_t RackStrikePulse::poll(RackViewModel& model, double nowSeconds) {
	const RackKeyMask mask = model.takeStruck();
	if (reduced_) {
		// ONE STEADY HIGHLIGHT per look: the keys of THIS look, held until the next one, no decay and no animation.
		steady_ = mask;
	}
	else {
		for (int k = 0; k < 128; ++k) {
			if (maskHas(mask, k)) {
				struckAt_[static_cast<size_t>(k)] = nowSeconds;
			}
		}
	}
	const bool liveNow = live(nowSeconds);
	// REPAINT WHILE ANYTHING IS LIT, AND ONCE MORE when the last of it went out (else the final frame stays lit).
	size_t told = 0;
	if (maskAny(mask) || liveNow || wasLive_) {
		told = watchers_.notify();
	}
	wasLive_ = liveNow;
	return told;
}

//--------------------------------------------------------------------------------------------------//
// THE EXPAND ARROW                                                                                    //
//--------------------------------------------------------------------------------------------------//

RackDrawerArrow::RackDrawerArrow(RackFlow flow, std::string tip, std::function<bool()> isOpen, std::function<void(bool)> setOpen)
    : flow_(flow), tip_(std::move(tip)), isOpen_(std::move(isOpen)), setOpen_(std::move(setOpen)) {
	onMouseDown() = [this](const visage::MouseEvent&) { toggle(); };
	onDpiChange() += [this] { setBounds(bounds()); };
	installTooltipHover(*this, *this);
}

void RackDrawerArrow::toggle() {
	if (setOpen_) {
		setOpen_(!(isOpen_ && isOpen_()));
	}
	redraw();
}

void RackDrawerArrow::draw(visage::Canvas& canvas) {
	const bool open = isOpen_ && isOpen_();
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const float cx = w * 0.5f;
	const float cy = h * 0.5f;
	const float r = std::min(w, h) * 0.28f;
	canvas.setColor(open ? kAccent : kMuted);
	// POINTING DOWN (top-down) or RIGHT (left-right) while closed, back the other way while open.
	if (flow_ == RackFlow::TopDown) {
		if (!open) {
			canvas.triangle(cx - r, cy - r * 0.6f, cx + r, cy - r * 0.6f, cx, cy + r * 0.8f);
		}
		else {
			canvas.triangle(cx - r, cy + r * 0.6f, cx + r, cy + r * 0.6f, cx, cy - r * 0.8f);
		}
	}
	else {
		if (!open) {
			canvas.triangle(cx - r * 0.6f, cy - r, cx - r * 0.6f, cy + r, cx + r * 0.8f, cy);
		}
		else {
			canvas.triangle(cx + r * 0.6f, cy - r, cx + r * 0.6f, cy + r, cx - r * 0.8f, cy);
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE TAB BAR                                                                                         //
//--------------------------------------------------------------------------------------------------//

RackTabBar::RackTabBar(RackViewModel& model, RackFlow flow) : model_(model), flow_(flow) {
	onMouseDown() = [this](const visage::MouseEvent& e) { press(e.position); };
	// A SCALE CHANGE MOVES NOTHING LOGICAL: re-set the bounds, so the native extents are cut again.
	onDpiChange() += [this] {
		setBounds(bounds());
		redraw();
	};
	installTooltipHover(*this, *this);
}

float RackTabBar::scale() const {
	return std::max(0.01f, dpiScale());
}

int RackTabBar::nativeLength() const {
	return horizontal() ? nativeWidth() : nativeHeight();
}

std::pair<int, int> RackTabBar::tabNativeExtent(size_t k) const {
	const size_t n = tabbedChains(model_).size();
	return { evenEdge(nativeLength(), n, k), evenEdge(nativeLength(), n, k + 1) };
}

int RackTabBar::tabAtNative(float along, size_t count) const {
	if (count == 0) {
		return -1;
	}
	for (size_t k = 0; k < count; ++k) {
		if (along < static_cast<float>(evenEdge(nativeLength(), count, k + 1))) {
			return static_cast<int>(k);
		}
	}
	return static_cast<int>(count) - 1;
}

visage::Point RackTabBar::tabCentre(size_t k) const {
	const auto [a, b] = tabNativeExtent(k);
	const float mid = 0.5f * static_cast<float>(a + b) / scale();
	return horizontal() ? visage::Point { mid, static_cast<float>(height()) * 0.5f } : visage::Point { static_cast<float>(width()) * 0.5f, mid };
}

visage::Point RackTabBar::dotCentre(size_t k) const {
	const auto [a, b] = tabNativeExtent(k);
	const float half = kDotArea * 0.5f;
	if (horizontal()) {
		return { static_cast<float>(b) / scale() - half, half };
	}
	return { half, static_cast<float>(a) / scale() + half };
}

bool RackTabBar::inDotArea(visage::Point at, size_t k) const {
	if (model_.routing() != RackRoutingKind::Select) {
		return false;
	}
	const visage::Point c = dotCentre(k);
	return std::fabs(at.x - c.x) <= kDotArea * 0.5f && std::fabs(at.y - c.y) <= kDotArea * 0.5f;
}

bool RackTabBar::dotShown(size_t k) const {
	const std::vector<size_t> tabs = tabbedChains(model_);
	return model_.routing() == RackRoutingKind::Select && k < tabs.size() && model_.playingChain() == tabs[k];
}

bool RackTabBar::raised(size_t k) const {
	const std::vector<size_t> tabs = tabbedChains(model_);
	return k < tabs.size() && model_.viewedChain() == tabs[k];
}

void RackTabBar::press(visage::Point at) {
	const std::vector<size_t> tabs = tabbedChains(model_);
	const int k = tabAtNative((horizontal() ? at.x : at.y) * scale(), tabs.size());
	if (k < 0) {
		return;
	}
	if (inDotArea(at, static_cast<size_t>(k))) {
		// THE PLAYING CHAIN, through its parameter; the viewed tab does not move.
		model_.setPlayingChain(tabs[static_cast<size_t>(k)]);
	}
	else {
		model_.viewChain(tabs[static_cast<size_t>(k)]);
	}
	redraw();
}

const char* RackTabBar::tooltipAt(visage::Point at) const {
	const std::vector<size_t> tabs = tabbedChains(model_);
	const int k = tabAtNative((horizontal() ? at.x : at.y) * scale(), tabs.size());
	if (k >= 0 && inDotArea(at, static_cast<size_t>(k))) {
		return "Make this the playing chain: the one the notes reach. The tab in front stays where it is";
	}
	return "A chain. Press to bring it to the front";
}

void RackTabBar::offeredControls(std::vector<Offered>& out) const {
	if (device::Modulatable* playing = model_.playingControl(); playing != nullptr) {
		out.push_back({ playing, Road::Gesture });
	}
}

void RackTabBar::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	const std::vector<size_t> tabs = tabbedChains(model_);
	canvas.setColor(kSurface);
	canvas.fill(0.0f, 0.0f, w, h);
	const float s = scale();
	for (size_t k = 0; k < tabs.size(); ++k) {
		const auto [a, b] = tabNativeExtent(k);
		const float from = static_cast<float>(a) / s;
		const float to = static_cast<float>(b) / s;
		const bool front = model_.viewedChain() == tabs[k];
		const float x = horizontal() ? from : 0.0f;
		const float y = horizontal() ? 0.0f : from;
		const float tw = horizontal() ? to - from : w;
		const float th = horizontal() ? h : to - from;
		// THE FRONT TAB IS RAISED (the card's own surface) with the accent along the edge that meets the card; the others sit in the bar.
		if (front) {
			canvas.setColor(kSurfaceRaised);
			canvas.fill(x, y, tw, th);
			canvas.setColor(kAccent);
			if (horizontal()) {
				canvas.fill(x, y + th - 2.0f, tw, 2.0f);
			}
			else {
				canvas.fill(x + tw - 2.0f, y, 2.0f, th);
			}
		}
		if (k > 0) {
			canvas.setColor(kLine);
			if (horizontal()) {
				canvas.fill(x, y, 1.0f, th);
			}
			else {
				canvas.fill(x, y, tw, 1.0f);
			}
		}
		// THE NAME, ALONE: shrunk toward the floor size and then cut, so eight tabs stay legible and never overflow into a neighbour.
		const std::string name = labelOfChain(model_.chain(tabs[k]), tabs[k]);
		const float room = (horizontal() ? tw : th) - 8.0f;
		const FittedName fit = fitName(name, std::max(0.0f, room), 9.0f, kNameFitFloorSize, 0.0f);
		canvas.setColor(front ? kPointer : kMuted);
		if (horizontal()) {
			canvas.text(fit.text, font(fit.size), visage::Font::kCenter, x, y, tw, th);
		}
		else {
			canvas.text(fit.text, font(fit.size), visage::Font::kCenter, x, y, tw, th, visage::Direction::Left);
		}
		// THE PLAYING DOT (Select): lit on the playing chain, a faint ring elsewhere so the place that sets it can be found.
		if (model_.routing() == RackRoutingKind::Select) {
			const visage::Point c = dotCentre(k);
			if (model_.playingChain() == tabs[k]) {
				canvas.setColor(kAccent);
				canvas.circle(c.x - 3.0f, c.y - 3.0f, 6.0f);
			}
			else {
				canvas.setColor(withAlpha(kLine, 0.9f));
				canvas.circle(c.x - 2.5f, c.y - 2.5f, 5.0f);
			}
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE PAD GRID                                                                                        //
//--------------------------------------------------------------------------------------------------//

RackPadGrid::RackPadGrid(RackViewModel& model, RackStrikePulse& pulse) : model_(model), pulse_(pulse) {
	watch_ = pulse_.watchers().watch(this);
	onMouseDown() = [this](const visage::MouseEvent& e) { press(e.position); };
	onMouseUp() = [this](const visage::MouseEvent&) { release(); };
	onDpiChange() += [this] {
		setBounds(bounds());
		redraw();
	};
	installTooltipHover(*this, *this);
}

float RackPadGrid::scale() const {
	return std::max(0.01f, dpiScale());
}

std::pair<int, int> RackPadGrid::columnNativeExtent(int column) const {
	const auto c = static_cast<size_t>(std::clamp(column, 0, 3));
	return { evenEdge(nativeWidth(), 4, c), evenEdge(nativeWidth(), 4, c + 1) };
}

std::pair<int, int> RackPadGrid::rowNativeExtent(int rowFromTop) const {
	const auto r = static_cast<size_t>(std::clamp(rowFromTop, 0, 3));
	return { evenEdge(nativeHeight(), 4, r), evenEdge(nativeHeight(), 4, r + 1) };
}

int RackPadGrid::padAt(visage::Point at) const {
	if (at.x < 0.0f || at.y < 0.0f || at.x >= static_cast<float>(width()) || at.y >= static_cast<float>(height())) {
		return -1;
	}
	const float nx = at.x * scale();
	const float ny = at.y * scale();
	int column = 3;
	int rowTop = 3;
	for (int c = 0; c < 4; ++c) {
		if (nx < static_cast<float>(columnNativeExtent(c).second)) {
			column = c;
			break;
		}
	}
	for (int r = 0; r < 4; ++r) {
		if (ny < static_cast<float>(rowNativeExtent(r).second)) {
			rowTop = r;
			break;
		}
	}
	// LEFT TO RIGHT, THEN BOTTOM TO TOP: the bottom row holds indices 0..3.
	return (3 - rowTop) * 4 + column;
}

visage::Point RackPadGrid::padCentre(int index) const {
	const auto [xa, xb] = columnNativeExtent(padColumn(index));
	const auto [ya, yb] = rowNativeExtent(3 - padRowFromBottom(index));
	return { 0.5f * static_cast<float>(xa + xb) / scale(), 0.5f * static_cast<float>(ya + yb) / scale() };
}

bool RackPadGrid::usedForTest(int index) const {
	return chainForKey(model_, padKey(model_.page(), index)) != kNoChain;
}

std::string RackPadGrid::labelForTest(int index) const {
	const int key = padKey(model_.page(), index);
	const size_t slot = chainForKey(model_, key);
	return slot == kNoChain ? fx::eq::noteName(key) : labelOfChain(model_.chain(slot), slot);
}

float RackPadGrid::flashForTest(int index) const {
	return pulse_.level(padKey(model_.page(), index), pulse_.now());
}

bool RackPadGrid::selectedForTest(int index) const {
	const size_t slot = chainForKey(model_, padKey(model_.page(), index));
	return slot != kNoChain && model_.viewedChain() == slot;
}

void RackPadGrid::press(visage::Point at) {
	const int index = padAt(at);
	if (index < 0) {
		return;
	}
	const int key = padKey(model_.page(), index);
	// PLAY THE KEY (the note road, where the host has one) AND VIEW ITS CHAIN, if there is one.
	if (model_.canTrigger() && model_.trigger(key, true, kVelocity)) {
		held_ = key;
	}
	if (const size_t slot = chainForKey(model_, key); slot != kNoChain) {
		model_.viewChain(slot);
	}
	redraw();
}

void RackPadGrid::release() {
	if (held_ >= 0) {
		(void)model_.trigger(held_, false, 0.0f);
		held_ = -1;
	}
}

const char* RackPadGrid::tooltipAt(visage::Point at) const {
	const int index = padAt(at);
	if (index >= 0 && usedForTest(index)) {
		return "A chain on this key. Press to play the key and bring its chain forward";
	}
	return "A key with no chain. Press to play it";
}

void RackPadGrid::draw(visage::Canvas& canvas) {
	const float s = scale();
	const double now = pulse_.now();
	for (int index = 0; index < kPadsPerPage; ++index) {
		const int key = padKey(model_.page(), index);
		const auto [xa, xb] = columnNativeExtent(padColumn(index));
		const auto [ya, yb] = rowNativeExtent(3 - padRowFromBottom(index));
		const float x = static_cast<float>(xa) / s + kInset;
		const float y = static_cast<float>(ya) / s + kInset;
		const float w = static_cast<float>(xb - xa) / s - 2.0f * kInset;
		const float h = static_cast<float>(yb - ya) / s - 2.0f * kInset;
		const size_t slot = chainForKey(model_, key);
		const bool used = slot != kNoChain;
		const float flash = pulse_.level(key, now);
		uint32_t fill = used ? blendColour(kSurfaceRaised, hueOfSlot(slot), 0.28f) : withAlpha(kSurface, 0.0f);
		uint32_t border = used ? blendColour(kLine, hueOfSlot(slot), 0.7f) : withAlpha(kLine, 0.45f);
		if (flash > 0.0f) {
			fill = blendColour(used ? fill : kSurfaceRaised, kPointer, 0.55f * flash);
			border = blendColour(border, kPointer, flash);
		}
		canvas.setColor(fill);
		canvas.roundedRectangle(x, y, w, h, 7.0f);
		const bool selected = used && model_.viewedChain() == slot;
		if (selected) {
			canvas.setColor(kAccent);
			canvas.roundedRectangleBorder(x - 1.0f, y - 1.0f, w + 2.0f, h + 2.0f, 8.0f, 1.5f);
		}
		canvas.setColor(border);
		canvas.roundedRectangleBorder(x, y, w, h, 7.0f, 1.0f);
		// THE NAME OF A USED PAD, the note of an empty one (dimmed).
		const std::string label = used ? labelOfChain(model_.chain(slot), slot) : fx::eq::noteName(key);
		const FittedName fit = fitName(label, std::max(0.0f, w - 6.0f), used ? 9.0f : 8.0f, kNameFitFloorSize, 0.0f);
		canvas.setColor(used ? kPointer : withAlpha(kMuted, 0.6f));
		canvas.text(fit.text, font(fit.size), visage::Font::kCenter, x, y, w, h);
	}
}

//--------------------------------------------------------------------------------------------------//
// THE PAGE STRIP                                                                                      //
//--------------------------------------------------------------------------------------------------//

RackPadPages::RackPadPages(RackViewModel& model, RackStrikePulse& pulse, RackFlow flow) : model_(model), pulse_(pulse), flow_(flow) {
	watch_ = pulse_.watchers().watch(this);
	onMouseDown() = [this](const visage::MouseEvent& e) { press(e.position); };
	onDpiChange() += [this] {
		setBounds(bounds());
		redraw();
	};
	installTooltipHover(*this, *this);
}

float RackPadPages::scale() const {
	return std::max(0.01f, dpiScale());
}

int RackPadPages::nativeLength() const {
	return horizontal() ? nativeWidth() : nativeHeight();
}

std::pair<int, int> RackPadPages::pageNativeExtent(int page) const {
	const auto p = static_cast<size_t>(std::clamp(page, 0, kPadPages - 1));
	return { evenEdge(nativeLength(), kPadPages, p), evenEdge(nativeLength(), kPadPages, p + 1) };
}

int RackPadPages::pageAt(visage::Point at) const {
	if (at.x < 0.0f || at.y < 0.0f || at.x >= static_cast<float>(width()) || at.y >= static_cast<float>(height())) {
		return -1;
	}
	const float along = (horizontal() ? at.x : at.y) * scale();
	for (int p = 0; p < kPadPages; ++p) {
		if (along < static_cast<float>(pageNativeExtent(p).second)) {
			return p;
		}
	}
	return kPadPages - 1;
}

visage::Point RackPadPages::pageCentre(int page) const {
	const auto [a, b] = pageNativeExtent(page);
	const float mid = 0.5f * static_cast<float>(a + b) / scale();
	return horizontal() ? visage::Point { mid, static_cast<float>(height()) * 0.5f } : visage::Point { static_cast<float>(width()) * 0.5f, mid };
}

bool RackPadPages::cellUsedForTest(int page, int index) const {
	return chainForKey(model_, padKey(page, index)) != kNoChain;
}

float RackPadPages::cellFlashForTest(int page, int index) const {
	return pulse_.level(padKey(page, index), pulse_.now());
}

bool RackPadPages::currentForTest(int page) const {
	return model_.page() == page;
}

void RackPadPages::press(visage::Point at) {
	if (const int page = pageAt(at); page >= 0) {
		model_.setPage(page);
		redraw();
	}
}

const char* RackPadPages::tooltipAt(visage::Point) const {
	return "A page of sixteen keys. Press to show it; a lit cell has a chain on its key";
}

void RackPadPages::draw(visage::Canvas& canvas) {
	const float s = scale();
	const double now = pulse_.now();
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	for (int page = 0; page < kPadPages; ++page) {
		const auto [a, b] = pageNativeExtent(page);
		const float from = static_cast<float>(a) / s;
		const float to = static_cast<float>(b) / s;
		const float x = (horizontal() ? from : 0.0f) + 1.0f;
		const float y = (horizontal() ? 0.0f : from) + 1.0f;
		const float tw = (horizontal() ? to - from : w) - 2.0f;
		const float th = (horizontal() ? h : to - from) - 2.0f;
		const bool current = model_.page() == page;
		canvas.setColor(current ? kAccent : kLine);
		canvas.roundedRectangleBorder(x, y, tw, th, 3.0f, current ? 1.5f : 1.0f);
		// SIXTEEN CELLS IN THE PADS' ORDER (lowest bottom-left) inside the frame.
		const float pad = 3.0f;
		const float cw = (tw - 2.0f * pad) / 4.0f;
		const float ch = (th - 2.0f * pad) / 4.0f;
		for (int index = 0; index < kPadsPerPage; ++index) {
			const int key = padKey(page, index);
			const float cx = x + pad + static_cast<float>(padColumn(index)) * cw;
			const float cy = y + pad + static_cast<float>(3 - padRowFromBottom(index)) * ch;
			uint32_t colour = chainForKey(model_, key) != kNoChain ? withAlpha(kAccent, 0.9f) : withAlpha(kLine, 0.5f);
			if (const float flash = pulse_.level(key, now); flash > 0.0f) {
				colour = blendColour(colour, kPointer, flash);
			}
			canvas.setColor(colour);
			canvas.fill(cx + 0.5f, cy + 0.5f, std::max(1.0f, cw - 1.0f), std::max(1.0f, ch - 1.0f));
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// THE PADS PANEL                                                                                      //
//--------------------------------------------------------------------------------------------------//

RackPadsPanel::RackPadsPanel(RackViewModel& model, RackStrikePulse& pulse, RackFlow flow)
    : model_(model), flow_(flow), grid_(model, pulse), pages_(model, pulse, flow) {
	addChild(&grid_);
	addChild(&pages_);
	if (device::Modulatable* level = model.kitLevel(); level != nullptr) {
		level_ = std::make_unique<FaderPill>(*level, "Level");
		addChild(level_.get());
	}
	onDpiChange() += [this] { resized(); };
}

visage::Point RackPadsPanel::naturalSize() const {
	const float level = level_ != nullptr ? kGap * 2.0f + kLevelHeight : 0.0f;
	if (flow_ == RackFlow::TopDown) {
		return { 2.0f * kMargin + gridSide(), 2.0f * kMargin + gridSide() + kGap * 2.0f + RackPadPages::kThickness + level };
	}
	return { 2.0f * kMargin + gridSide() + kGap * 2.0f + RackPadPages::kThickness, 2.0f * kMargin + gridSide() + level };
}

void RackPadsPanel::resized() {
	// UNCONDITIONAL, so a scale change (which moves nothing logical) still cuts the native cells again.
	const float side = gridSide();
	grid_.setBounds(kMargin, kMargin, side, side);
	if (flow_ == RackFlow::TopDown) {
		pages_.setBounds(kMargin, kMargin + side + 2.0f * kGap, side, RackPadPages::kThickness);
		if (level_ != nullptr) {
			level_->setBounds(kMargin, kMargin + side + 4.0f * kGap + RackPadPages::kThickness, side, kLevelHeight);
		}
	}
	else {
		pages_.setBounds(kMargin + side + 2.0f * kGap, kMargin, RackPadPages::kThickness, side);
		if (level_ != nullptr) {
			level_->setBounds(kMargin, kMargin + side + 2.0f * kGap, side, kLevelHeight);
		}
	}
}

void RackPadsPanel::refresh() {
	grid_.refresh();
	pages_.refresh();
}

} // namespace feathers::ui
