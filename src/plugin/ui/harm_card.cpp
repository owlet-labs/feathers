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

	// EACH PARTIAL HAS ITS OWN THREE CONTROLS, built over that partial's own parameters and shown only while its tab is open.
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		PartialSet& set = sets_[static_cast<size_t>(k)];
		set.active = std::make_unique<SwitchChip>(control(hrm::partialParam(k, hrm::PartialField::Active)), "Active");
		addChild(set.active.get());
		set.row = std::make_unique<HarmonicRow>(control(hrm::partialParam(k, hrm::PartialField::Harmonic)));
		set.row->onChanged = [this] { redraw(); }; // the tab's ratio reads the same field
		addChild(set.row.get());
		set.level = std::make_unique<FaderPill>(control(hrm::partialParam(k, hrm::PartialField::Level)), "Level");
		addChild(set.level.get());
	}
	applySelection();
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

float HarmCard::openHeight() {
	// head, the tab row, the frame (its top edge is the tab row's bottom edge), then the masters under their caption.
	return kHeadHeight + kRowGap + kTabRowHeight + kFrameHeight + kRowGap + kCaptionBand + kTabGap
	       + Knob::columnHeight(kKnobDiameter) + kPad;
}

bool HarmCard::partialActive(int partial) const {
	return host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(partial, hrm::PartialField::Active))) >= 0.5f;
}

void HarmCard::applySelection() {
	const int selected = selectedPartial();
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const bool shown = !folded_ && k == selected;
		PartialSet& set = sets_[static_cast<size_t>(k)];
		set.active->setVisible(shown);
		set.row->setVisible(shown);
		set.level->setVisible(shown);
	}
}

void HarmCard::foldedChanged() {
	for (auto& knob : knobs_) {
		knob->setVisible(!folded_);
	}
	applySelection();
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


std::vector<hrm::Param> HarmCard::addressedParamsForTest() const {
	std::vector<hrm::Param> out = knobParams_;
	out.push_back(hrm::Param::Enabled); // the square in the head
	// The frame's three controls, for every partial: each is on the card while its tab is open.
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		out.push_back(hrm::partialParam(k, hrm::PartialField::Active));
		out.push_back(hrm::partialParam(k, hrm::PartialField::Harmonic));
		out.push_back(hrm::partialParam(k, hrm::PartialField::Level));
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
		knobs_[i]->setBounds(bodyLeft() + kPad + cell * static_cast<float>(i), top, cell, column);
	}
	// THE FRAME'S TWO ROWS, the same places for every partial's set: Active chip then the harmonic box, then Level.
	const visage::Bounds frame = frameBounds();
	const float x = frame.x() + kFramePad;
	const float w = std::max(0.0f, frame.width() - 2.0f * kFramePad);
	const float rowY = frame.y() + kFramePad;
	const float levelY = rowY + HarmonicRow::kHeight + kFrameRowGap;
	const float rowX = x + kChipWidth + 8.0f;
	for (PartialSet& set : sets_) {
		set.active->setBounds(x, rowY + (HarmonicRow::kHeight - SwitchChip::kHeight) * 0.5f, kChipWidth, SwitchChip::kHeight);
		set.row->setBounds(rowX, rowY, std::max(0.0f, x + w - rowX), HarmonicRow::kHeight);
		set.level->setBounds(x, levelY, w, kFrameLevelHeight);
	}
}

bool HarmCard::updateContent() {
	if (!isVisible() || folded()) {
		return false; // `redraw()` is refused for a frame that is not drawn; a true would be a lie.
	}
	// READ THE FIELD, DO NOT ASK FOR A CONTROL: `control()` constructs a widget, and a tick is the wrong moment for that.
	const bool enabled = host_.fieldAt(device_, static_cast<size_t>(hrm::Param::Enabled)) >= 0.5f;
	// THE SELECTED PARTIAL is shared with the window, so a change made there arrives here as a redraw of the tabs and a new set.
	const int partial = selectedPartial();
	bool activeMoved = false;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const int now = partialActive(k) ? 1 : 0;
		activeMoved = activeMoved || now != drawnActive_[static_cast<size_t>(k)];
		drawnActive_[static_cast<size_t>(k)] = now;
	}
	if (haveDrawnEnabled_ && enabled == drawnEnabled_ && partial == drawnPartial_ && !activeMoved) {
		return false;
	}
	drawnEnabled_ = enabled;
	haveDrawnEnabled_ = true;
	if (partial != drawnPartial_) {
		applySelection();
	}
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
	// A partial that is switched off sounds nothing, so its harmonic and Level recede with it, and everything recedes with the device.
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		PartialSet& set = sets_[static_cast<size_t>(k)];
		const bool live = on && partialActive(k);
		set.row->setActive(live);
		set.level->setActive(live);
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

visage::Bounds HarmCard::frameBounds() const {
	const float top = bodyTopEdge() + kRowGap + kTabRowHeight;
	return { bodyLeft() + kPad, top, std::max(0.0f, bodyWidth() - 2.0f * kPad), kFrameHeight };
}

float HarmCard::knobsTop() const {
	return frameBounds().bottom() + kRowGap + kCaptionBand + kTabGap;
}

visage::Bounds HarmCard::tabBounds(int partial) const {
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float each = (inner - kTabGap * static_cast<float>(hrm::kPartialCount - 1)) / static_cast<float>(hrm::kPartialCount);
	const float top = bodyTopEdge() + kRowGap;
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
	drawnPartial_ = next;
	applySelection();
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
	return tabLineOf(partial, harmonic) + (partialActive(partial) ? " on" : " off");
}

void HarmCard::drawPartialTabs(visage::Canvas& canvas, bool deviceOn) {
	const int selected = selectedPartial();
	const visage::Bounds frame = frameBounds();

	// THE FRAME FIRST, in the selected tab's own colour, so the tab and the panel read as one object. Its top edge runs under the
	// selected tab, which is then drawn solid down through that edge.
	canvas.setColor(withAlpha(kAccent, 0.75f));
	canvas.roundedRectangleBorder(frame.x(), frame.y(), frame.width(), frame.height(), 3.0f, 1.0f);

	for (int k = 0; k < hrm::kPartialCount; ++k) {
		visage::Bounds tab = tabBounds(k);
		const bool isSelected = k == selected;
		if (isSelected) {
			// The selected tab runs 2px into the frame, over the border's top edge, so the line is broken under it.
			tab = { tab.x(), tab.y(), tab.width(), tab.height() + 2.0f };
		}
		drawTabChip(canvas, tab, std::string {}, isSelected);
		const bool active = partialActive(k);
		const double harmonic = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Harmonic)));
		const float half = kTabRowHeight * 0.5f;
		canvas.setColor(isSelected ? kSurface : (deviceOn && active ? kMuted : kDisabled));
		drawTracked(canvas, tabLineOf(k, harmonic), font(kKnobLabelSize), kKnobLabelTracking, tab.x() + 6.0f, tab.y(), half);
		drawTracked(canvas, active ? "ON" : "OFF", font(kKnobLabelSize), kKnobLabelTracking, tab.x() + 6.0f, tab.y() + half,
		            half);
	}

	// THE MASTERS' CAPTION, under the frame and outside it.
	const float globalY = frame.bottom() + kRowGap;
	const float inner = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	canvas.setColor(withAlpha(kMuted, 0.75f));
	drawTracked(canvas, "GLOBAL · EVERY PARTIAL", font(kKnobLabelSize), kKnobLabelTracking, bodyLeft() + kPad, globalY,
	            kCaptionBand);
	canvas.setColor(withAlpha(kLine, 0.55f));
	canvas.fill(bodyLeft() + kPad, globalY + kCaptionBand, inner, 1.0f);
}

} // namespace feathers::ui
