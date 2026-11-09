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

#include "plugin/ui/stereo_tool_card.h"

#include "plugin/ui/plot_grid.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string>

namespace feathers::ui {

namespace {

namespace st = feathers::mod::stereo;

[[nodiscard]] constexpr size_t at(st::Param p) { return static_cast<size_t>(p); }

/// THE TABS' ENTRIES ARE THE OUTPUTS' OWN NAMES, so the tab and the output chip under the card say the same word.
constexpr const char* const kTabNames[] = { st::kOutputs[0].name, st::kOutputs[1].name };
static_assert(std::size(kTabNames) == static_cast<size_t>(st::Output::kCount));

/// A VIEW CHOICE — not a param: it is stored nowhere, nothing routes to it, and no guard counts it as the device's.
constexpr device::ParamDesc kViewDesc { .key = "view", .name = "Show", .min = 0.0, .max = 1.0, .def = 0.0,
	                                    .stepped = true, .unit = device::Unit::Choice, .names = kTabNames,
	                                    .modulatable = false,
	                                    .tooltip = "Which output the scope shows — its two ears, left and right" };

[[nodiscard]] bool same(device::EarPair a, device::EarPair b) noexcept {
	return a.left == b.left && a.right == b.right;
}

} // namespace

const device::ParamDesc& StereoToolCard::ViewChoice::desc() const {
	return kViewDesc;
}

void StereoToolCard::ViewChoice::setValue(double value) {
	const long n = std::lround(std::clamp(value, 0.0, static_cast<double>(st::Output::kCount) - 1.0));
	card_.setShown(static_cast<st::Output>(n));
}

std::string StereoToolCard::ViewChoice::text() const {
	return kTabNames[static_cast<size_t>(card_.shown_)];
}

StereoToolCard::StereoToolCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
                               Notifier* notifier, ModState* mod, EditHistory* history)
    : host_(host), device_(id), notifier_(notifier), mod_(mod), history_(history) {
	tabs_ = std::make_unique<SegmentedControl>(view_, registry, kTabNames, notifier_);
	amount_ = std::make_unique<Knob>(control(at(st::Param::Amount)), "Amount", notifier_);
	in_ = std::make_unique<Knob>(control(at(st::Param::In)), "In", notifier_);
	width_ = std::make_unique<Knob>(control(at(st::Param::Width)), "Width", notifier_);
	swap_ = std::make_unique<SwitchChip>(control(at(st::Param::Swap)), "Swap");
	for (Knob* knob : { amount_.get(), in_.get(), width_.get() }) {
		knob->setFaceDiameter(kFaceDiameter);
		knob->setModulation(mod_, history_);
	}
	addChild(tabs_.get());
	addChild(amount_.get());
	addChild(in_.get());
	addChild(width_.get());
	addChild(swap_.get());

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		// THE SLOT MENU, WHEN A HOST BINDS ONE — the fx lane holds modulators since lane-mods (2026-10-04), and this
		// card arrived on another branch the same day without the call the other five gained (the head-verb sweep).
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
			return;
		}
		(void)enablePressAt(e.position);
	};
	layoutControls();
}

StereoToolCard::~StereoToolCard() = default;

DeviceControl& StereoToolCard::control(size_t index) const {
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, st::kDescriptor, index, mod_, history_);
	}
	return *controls_[index];
}

device::Modulatable* StereoToolCard::enableModulatable() {
	return &control(at(st::Param::Enabled));
}

void StereoToolCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(i).setModulation(mod, history);
	}
	for (Knob* knob : { amount_.get(), in_.get(), width_.get() }) {
		knob->setModulation(mod, history);
	}
}

void StereoToolCard::show(st::Output out) {
	tabs_->select(static_cast<int>(out));
}

void StereoToolCard::setShown(st::Output out) {
	if (out == shown_) {
		return;
	}
	shown_ = out;
	// **A NEW OUTPUT IS A NEW TRACE** — the old one's history is not this output's past, so the ring starts over.
	head_ = 0;
	count_ = 0;
	still_ = 0;
	layoutControls();
	redraw();
}

std::vector<st::Param> StereoToolCard::addressedParamsForTest() const {
	std::vector<st::Param> out;
	const auto add = [&](const device::Modulatable* c) {
		if (const auto* road = dynamic_cast<const DeviceControl*>(c); road != nullptr && road->device() == device_) {
			out.push_back(static_cast<st::Param>(road->control()));
		}
	};
	add(amount_->control());
	add(in_->control());
	add(width_->control());
	add(&swap_->control());
	return out;
}

float StereoToolCard::openHeight(size_t outputRows) {
	return kCardHeadHeight + bodyHeight() + kCardOutputStripHeight * static_cast<float>(outputRows);
}

float StereoToolCard::laneHeightAt(float cardWidth) const {
	return folded() ? kCardHeadHeight : kCardHeadHeight + bodyHeight() + outputStripHeightAt(cardWidth);
}

visage::Bounds StereoToolCard::scopeBounds() const {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const float top = bodyTopEdge() + kGap + kTabsHeight + kGap;
	return { left, top, inner, kScopeHeight };
}

void StereoToolCard::resized() {
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	tabs_->setBounds(left, bodyTopEdge() + kGap, inner, kTabsHeight);
	layoutControls();
}

void StereoToolCard::layoutControls() {
	// THE CONTROLS OF THE SHOWN OUTPUT, in the row under the scope. In is under both; Split adds Amount; Width's three take
	// thirds, the chip level with the knobs' faces (`MathCard`'s second row).
	const float left = bodyLeft() + kPadX;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPadX);
	const float top = scopeBounds().bottom() + kGap;
	const float third = std::max(0.0f, (inner - 2.0f * kGap) / 3.0f);
	const bool open = !folded();
	const bool split = shown_ == st::Output::Split;
	amount_->setVisible(open && split);
	in_->setVisible(open);
	width_->setVisible(open && !split);
	swap_->setVisible(open && !split);
	tabs_->setVisible(open);
	// Split's two sit centred, a half-cell in; Width's three fill the row.
	const float splitLeft = left + 0.5f * (third + kGap);
	amount_->setBounds(splitLeft + third + kGap, top, third, controlsHeight());
	in_->setBounds(split ? splitLeft : left, top, third, controlsHeight());
	width_->setBounds(left + third + kGap, top, third, controlsHeight());
	swap_->setBounds(left + 2.0f * (third + kGap), top + (kFaceDiameter - SwitchChip::kHeight) * 0.5f, third,
	                 SwitchChip::kHeight);
}

void StereoToolCard::foldedChanged() {
	layoutControls();
}

device::EarPair StereoToolCard::evaluateNow() const {
	if (control(at(st::Param::Enabled)).value() < 0.5) {
		return {}; // the bypass: every output rests at 0
	}
	// THE CONTROLS AS THE ENGINE LEFT THEM — the device's own clamps, then its own arithmetic.
	const auto modulated = [this](st::Param p) { return host_.modulatedFieldAt(device_, at(p)); };
	const float amount = std::clamp(modulated(st::Param::Amount), 0.0f, 2.0f);
	const float inL = std::clamp(modulated(st::Param::In), -1.0f, 1.0f);
	float inR = inL;
	if (float right = 0.0f; host_.modulatedFieldRAt(device_, at(st::Param::In), right)) {
		inR = std::clamp(right, -1.0f, 1.0f);
	}
	const float width = std::clamp(modulated(st::Param::Width), 0.0f, 2.0f);
	const bool swap = host_.fieldAt(device_, at(st::Param::Swap)) >= 0.5f;
	return st::evaluate(shown_, inL, inR, amount, width, swap);
}

void StereoToolCard::push(device::EarPair pair) noexcept {
	still_ = count_ > 0 && same(pair, scopeLastForTest()) ? still_ + 1 : 1;
	ring_[head_] = pair;
	head_ = (head_ + 1) % kScopeCapacity;
	count_ = std::min(count_ + 1, kScopeCapacity);
}

device::EarPair StereoToolCard::scopeAtForTest(size_t back) const noexcept {
	if (back >= count_) {
		return {};
	}
	return ring_[(head_ + kScopeCapacity - 1 - back) % kScopeCapacity];
}

device::EarPair StereoToolCard::scopeLastForTest() const noexcept {
	return scopeAtForTest(0);
}

bool StereoToolCard::updateContent() {
	// ONE PAIR A FRAME. A repaint only while the picture moves: once every pair the plot shows is the newest one,
	// the traces are two still lines and another frame of them would draw the same pixels.
	push(evaluateNow());
	const auto shownWidth = static_cast<size_t>(std::max(0.0f, scopeBounds().width()));
	if (still_ > std::min(shownWidth, count_) + 1) {
		return false;
	}
	redraw();
	return true;
}

void StereoToolCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	const visage::Bounds box = scopeBounds();
	canvas.setColor(kVoid);
	canvas.roundedRectangle(box.x(), box.y(), box.width(), box.height(), 4.0f);
	canvas.setColor(withAlpha(kLine, 0.3f));
	canvas.roundedRectangleBorder(box.x(), box.y(), box.width(), box.height(), 4.0f, 1.0f);
	plot::drawGrid(canvas, box);

	// THE TRACES — ±1 a few pixels inside the box, so a pair resting on a rail still reads as a line.
	constexpr float kInset = 6.0f;
	const float mid = box.y() + box.height() * 0.5f;
	const float half = std::max(0.0f, box.height() * 0.5f - kInset);
	const auto yOf = [&](float v) { return mid - std::clamp(v, -1.0f, 1.0f) * half; };
	const size_t shown = std::min(count_, static_cast<size_t>(std::max(0.0f, box.width() - 4.0f)));
	const float newestX = box.right() - 3.0f;
	for (const bool right : { false, true }) {
		canvas.setColor(right ? kModulationR : kModulationL);
		const auto ear = [&](size_t back) {
			const device::EarPair p = scopeAtForTest(back);
			return right ? p.right : p.left;
		};
		for (size_t back = 1; back < shown; ++back) {
			const float x0 = newestX - static_cast<float>(back);
			canvas.segment(x0, yOf(ear(back)), x0 + 1.0f, yOf(ear(back - 1)), 1.5f, true);
		}
		if (shown > 0) {
			canvas.circle(newestX - 2.5f, yOf(ear(0)) - 2.5f, 5.0f);
		}
	}

	// THE LEGEND — which colour is which ear, in the corner the newest point is not, on a patch of the plot's own
	// ground: a pair on the +1 rail (Split at In 100%) runs straight through this corner, and PHOTOGRAPHED without
	// the patch the letters were struck through by it. It covers only the OLDEST few pixels of the trace.
	const visage::Font& legend = font(kKnobLabelSize);
	canvas.setColor(withAlpha(kVoid, 0.85f));
	canvas.roundedRectangle(box.x() + 2.0f, box.y() + 2.0f, 22.0f, 12.0f, 2.0f);
	canvas.setColor(kModulationL);
	canvas.text("L", legend, visage::Font::kLeft, box.x() + 5.0f, box.y() + 2.0f, 10.0f, 12.0f);
	canvas.setColor(kModulationR);
	canvas.text("R", legend, visage::Font::kLeft, box.x() + 14.0f, box.y() + 2.0f, 10.0f, 12.0f);
}

bool StereoToolCard::headSourceFocused() const {
	if (mod_ == nullptr) {
		return false;
	}
	for (size_t out = 0; out < static_cast<size_t>(st::Output::kCount); ++out) {
		if (mod_->source() == backplane::Source::output(device_, out)) {
			return true;
		}
	}
	return false;
}

bool StereoToolCard::headArmed() const {
	return headSourceFocused() && mod_->armed();
}

} // namespace feathers::ui
