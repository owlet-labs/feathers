/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */


#include "plugin/ui/harm_card.h"

#include "plugin/ui/expanded_views.h"

#include "plugin/gui_bridge.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/tab_chip.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace feathers::ui {

namespace hrm = feathers::fx::harm;

std::shared_ptr<HarmView> harmViewFor(DeviceInstanceId id) {
	// Entries whose surfaces are gone are dropped here, so the map cannot grow past the devices that are still on screen.
	static std::map<DeviceInstanceId, std::weak_ptr<HarmView>> views;
	for (auto it = views.begin(); it != views.end();) {
		if (it->second.expired()) {
			it = views.erase(it);
		}
		else {
			++it;
		}
	}
	if (std::shared_ptr<HarmView> live = views[id].lock()) {
		return live;
	}
	auto fresh = std::make_shared<HarmView>();
	views[id] = fresh;
	return fresh;
}

HarmCard::HarmCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), notifier_(notifier), device_(id) {
	(void)registry;
	view_ = harmViewFor(id);
	for (const CardKnob& knob : kCardKnobs) {
		auto widget = std::make_unique<Knob>(control(knob.param), knob.label, notifier);
		widget->setFaceDiameter(kKnobDiameter);
		addChild(widget.get());
		knobs_.push_back(std::move(widget));
		knobParams_.push_back(knob.param);
	}

	for (const hrm::Param param : kCardChips) {
		auto chip = std::make_unique<SwitchChip>(control(param), param == hrm::Param::Poly ? "Poly" : "Env Depth");
		addChild(chip.get());
		chips_.push_back(std::move(chip));
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (const int tab = tabAt(e.position); tab >= 0) {
			selectPartial(tab);
			return;
		}
		switch (partAt(e.position)) {
		case Part::Fold:
			break;
		case Part::Expand:
			break;
		case Part::Enable:
			toggleEnabled();
			break;
		case Part::SlotMenu:
			pressSlotMenu();
			break;
		case Part::None:
			break;
		}
	};
}

DeviceControl& HarmCard::control(hrm::Param param) const {
	const size_t index = static_cast<size_t>(param);
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, hrm::kDescriptor, index);
	}
	return *controls_[index];
}

namespace {

size_t knobRowCount() {
	return (HarmCard::kCardKnobCount + static_cast<size_t>(HarmCard::kRowCells) - 1)
	       / static_cast<size_t>(HarmCard::kRowCells);
}

} // namespace

float HarmCard::openHeight() {
	const float column = Knob::columnHeight(kKnobDiameter);
	const size_t rows = knobRowCount();
	const float knobRows = static_cast<float>(rows) * column
	                       + (rows > 1 ? static_cast<float>(rows - 1) * kRowGap : 0.0f);
	return kHeadHeight + kRowGap + kCaptionBand + kTabGap + kTabRowHeight + kRowGap + kCaptionBand + kTabGap + knobRows
	       + kActiveRowGap + SwitchChip::kHeight + kPad;
}

void HarmCard::foldedChanged() {
	for (auto& knob : knobs_) {
		knob->setVisible(!folded_);
	}
	for (auto& chip : chips_) {
		chip->setVisible(!folded_);
	}
}

void HarmCard::setModulation(ModState* state, EditHistory* history) {
	for (auto& knob : knobs_) {
		knob->setModulation(state, history);
	}
	for (size_t i = 0; i < controls_.size(); ++i) {
		control(static_cast<hrm::Param>(i)).setModulation(state, history);
	}
}

visage::Bounds HarmCard::headBounds() const {
	return ownFrame().head;
}

visage::Bounds HarmCard::chipBounds(size_t index) const {
	const float top = knobsTop() + static_cast<float>(knobRowCount()) * Knob::columnHeight(kKnobDiameter)
	                  + static_cast<float>(knobRowCount() - 1) * kRowGap + kActiveRowGap;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float each = (inner - kRowGap) / static_cast<float>(kCardChipCount);
	return { bodyLeft() + kPad + (each + kRowGap) * static_cast<float>(index), top, std::max(0.0f, each),
	         SwitchChip::kHeight };
}

SwitchChip* HarmCard::chipForTest(hrm::Param param) const {
	for (size_t i = 0; i < kCardChipCount; ++i) {
		if (kCardChips[i] == param) {
			return chips_[i].get();
		}
	}
	return nullptr;
}

std::vector<hrm::Param> HarmCard::addressedParamsForTest() const {
	std::vector<hrm::Param> out = knobParams_;
	out.push_back(hrm::Param::Enabled); // the square in the head
	for (const hrm::Param p : kCardChips) {
		out.push_back(p);
	}
	return out;
}

device::Modulatable* HarmCard::enableModulatable() {
	return hasEnable() ? &control(static_cast<hrm::Param>(enableControl())) : nullptr;
}

void HarmCard::toggleEnabled() {
	if (const auto nowOn = toggleEnable()) {
		if (notifier_ != nullptr) {
			notifier_->say(*nowOn ? "Harm on" : "Harm off");
		}
	}
}

HarmCard::Part HarmCard::partAt(visage::Point position) const {
	if (slotMenuAt(position)) {
		return Part::SlotMenu;
	}
	if (enableBounds().contains(position)) {
		return Part::Enable;
	}
	if (expandBounds().contains(position)) {
		return Part::Expand;
	}
	if (headBounds().contains(position)) {
		return Part::Fold;
	}
	return Part::None;
}

void HarmCard::resized() {
	layOut();
}

void HarmCard::layOut() {
	const float column = Knob::columnHeight(kKnobDiameter);
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float cell = inner / static_cast<float>(kRowCells);
	const float top = knobsTop();
	for (size_t i = 0; i < knobs_.size(); ++i) {
		const size_t line = i / static_cast<size_t>(kRowCells);
		const size_t at = i % static_cast<size_t>(kRowCells);
		knobs_[i]->setBounds(bodyLeft() + kPad + cell * static_cast<float>(at),
		                     top + static_cast<float>(line) * (column + kRowGap), cell, column);
	}
	for (size_t i = 0; i < chips_.size(); ++i) {
		const visage::Bounds b = chipBounds(i);
		chips_[i]->setBounds(b.x(), b.y(), b.width(), b.height());
	}
}

bool HarmCard::updateContent() {
	if (!isVisible() || folded()) {
		return false; // `redraw()` is refused for a frame that is not drawn; a true would be a lie.
	}
	// READ THE FIELD, DO NOT ASK FOR A CONTROL: `control()` constructs a widget, and a tick is the wrong moment for that.
	const bool enabled = host_.fieldAt(device_, static_cast<size_t>(hrm::Param::Enabled)) >= 0.5f;
	// THE SELECTED PARTIAL is shared with the window, so a change made there arrives here as a redraw of the tabs.
	const int partial = selectedPartial();
	if (haveDrawnEnabled_ && enabled == drawnEnabled_ && partial == drawnPartial_) {
		return false;
	}
	drawnEnabled_ = enabled;
	haveDrawnEnabled_ = true;
	drawnPartial_ = partial;
	redraw();
	return true;
}

void HarmCard::drawHeadExtras(visage::Canvas& canvas) {
	// The head's marks are drawn here, outside the body's clamp, so the slot menu is not clipped in a top-head lane.
	drawSlotMenu(canvas, kMuted);
}

void HarmCard::drawContent(visage::Canvas& canvas) {
	const bool on = control(hrm::Param::Enabled).value() >= 0.5;
	for (auto& knob : knobs_) {
		knob->setActive(on);
	}

	// Env Depth scales the notch's depth, so it has something to act on only while a partial runs a notch. The window
	// makes the same decision from the same fields.
	bool anyNotch = false;
	bool anyActive = false;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const bool highpass = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Mode))) >= 0.5f;
		const bool active = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Active))) >= 0.5f;
		if (active) {
			anyActive = true;
			anyNotch = anyNotch || !highpass;
		}
	}
	for (size_t i = 0; i < kCardChipCount; ++i) {
		if (kCardChips[i] == hrm::Param::Poly) {
			// Live or dead by the device's own declaration, as the window decides it.
			chips_[i]->setEnabled(hrm::kDescriptor.traits.perVoiceControl >= 0);
		}
		else {
			chips_[i]->setEnabled(!anyActive || anyNotch);
		}
	}

	drawPartialTabs(canvas, on);

	// The enable square is the one thing that never dims: it is the only way back.
	const visage::Bounds sw = enableBounds();
	canvas.setColor(on ? kAccent : kMuted);
	if (on) {
		canvas.roundedRectangle(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f);
	}
	else {
		canvas.roundedRectangleBorder(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f, 1.0f);
	}
}

float HarmCard::knobsTop() const {
	// The partial tabs and the GLOBAL caption sit between the head and the knobs; see `drawContent`.
	return bodyTopEdge() + kRowGap + kCaptionBand + kTabGap + kTabRowHeight + kRowGap + kCaptionBand + kTabGap;
}

visage::Bounds HarmCard::tabBounds(int partial) const {
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float each = (inner - kTabGap * static_cast<float>(hrm::kPartialCount - 1)) / static_cast<float>(hrm::kPartialCount);
	const float top = bodyTopEdge() + kRowGap + kCaptionBand + kTabGap;
	return { bodyLeft() + kPad + (each + kTabGap) * static_cast<float>(partial), top, std::max(0.0f, each), kTabRowHeight };
}

int HarmCard::tabAt(visage::Point position) const {
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		if (tabBounds(k).contains(position)) {
			return k;
		}
	}
	return -1;
}

void HarmCard::selectPartial(int partial) {
	const int next = std::clamp(partial, 0, hrm::kPartialCount - 1);
	if (next == view_->partial) {
		return;
	}
	view_->partial = next;
	redraw();
}

namespace {

/// What a partial's tab says: its number, and the ratio its harmonic plays — the table's own words, as the window says them.
std::string tabLineOf(int partial, double harmonic) {
	const size_t at = std::min(static_cast<size_t>(std::max(0.0, harmonic)), hrm::kHarmonicCount - 1);
	return std::to_string(partial + 1) + "  " + std::to_string(static_cast<int>(hrm::kHarmonics[at].num)) + ":"
	       + std::to_string(static_cast<int>(hrm::kHarmonics[at].den));
}

} // namespace

std::string HarmCard::tabTextForTest(int partial) const {
	const double harmonic = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(partial, hrm::PartialField::Harmonic)));
	const bool active = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(partial, hrm::PartialField::Active))) >= 0.5f;
	return tabLineOf(partial, harmonic) + (active ? " on" : " off");
}

void HarmCard::drawPartialTabs(visage::Canvas& canvas, bool deviceOn) {
	// THE CAPTIONS: the tabs are the partials, and the knobs below are the device's global controls, outside any tab's scope.
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	canvas.setColor(withAlpha(kMuted, 0.75f));
	drawTracked(canvas, "PARTIAL", font(kKnobLabelSize), kKnobLabelTracking, bodyLeft() + kPad,
	            bodyTopEdge() + kRowGap, kCaptionBand);
	const float globalY = bodyTopEdge() + kRowGap + kCaptionBand + kTabGap + kTabRowHeight + kRowGap;
	drawTracked(canvas, "GLOBAL · EVERY PARTIAL", font(kKnobLabelSize), kKnobLabelTracking, bodyLeft() + kPad, globalY,
	            kCaptionBand);
	canvas.setColor(withAlpha(kLine, 0.55f));
	canvas.fill(bodyLeft() + kPad, globalY + kCaptionBand, inner, 1.0f);

	const int selected = selectedPartial();
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const visage::Bounds tab = tabBounds(k);
		const bool isSelected = k == selected;
		drawTabChip(canvas, tab, std::string {}, isSelected);
		const double harmonic = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Harmonic)));
		const bool active = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Active))) >= 0.5f;
		const float half = tab.height() * 0.5f;
		canvas.setColor(isSelected ? kSurface : (deviceOn && active ? kMuted : kDisabled));
		drawTracked(canvas, tabLineOf(k, harmonic), font(kKnobLabelSize), kKnobLabelTracking, tab.x() + 6.0f, tab.y(), half);
		drawTracked(canvas, active ? "ON" : "OFF", font(kKnobLabelSize), kKnobLabelTracking, tab.x() + 6.0f, tab.y() + half,
		            half);
	}
}

} // namespace feathers::ui
