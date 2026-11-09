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

#include "plugin/ui/eq_card.h"

#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

namespace eq = fx::eq;

EqCard::EqCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry;
	controls_adapters_.resize(eq::kParamCount);
	const auto fn = [this](size_t i) -> DeviceControl& { return control(i); };
	graph_ = std::make_unique<EqGraph>(host_, device_, fn, true);
	graph_->onSelected = [this](int) { layOut(); };
	addChild(graph_.get());
	controls_ = std::make_unique<EqControls>(*this, host_, device_, fn, notifier_, kKnobDiameter, false);
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "EQ on" : "EQ off");
			}
		}
	};
}

DeviceControl& EqCard::control(size_t i) const {
	if (controls_adapters_[i] == nullptr) {
		controls_adapters_[i] = std::make_unique<DeviceControl>(host_, device_, eq::kDescriptor, i, mod_, history_);
	}
	return *controls_adapters_[i];
}

device::Modulatable* EqCard::enableModulatable() {
	return &control(eq::paramIndex(eq::Param::Enabled));
}

bool EqCard::headEnabled() const {
	return control(eq::paramIndex(eq::Param::Enabled)).value() >= 0.5;
}

float EqCard::openHeight() {
	const float row = Knob::columnHeight(kKnobDiameter);
	return kSlotHeadHeight + 4.0f + kGraphHeight + kRowGap + EqControls::kSwitchRow + EqControls::kGap + row
	       + kRowGap + row + kPad;
}

float EqCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

float EqCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

visage::Bounds EqCard::meterBounds() const {
	const float top = kSlotHeadHeight + 4.0f;
	return { bodyLeft() + bodyWidth() - kPad - kMeterWidth, top, kMeterWidth, kGraphHeight };
}

void EqCard::resized() {
	layOut();
}

void EqCard::foldedChanged() {
	layOut();
}

void EqCard::layOut() {
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, bodyWidth() - 2.0f * kPad);
	const float top = kSlotHeadHeight + 4.0f;
	graph_->setBounds(left, top, std::max(0.0f, width - kMeterWidth - 4.0f), kGraphHeight);
	graph_->setVisible(open);
	const float row = Knob::columnHeight(kKnobDiameter);
	const float stripY = top + kGraphHeight + kRowGap;
	const visage::Bounds strip { left, stripY, width, EqControls::kSwitchRow + EqControls::kGap + row };
	const visage::Bounds globals { left, stripY + strip.height() + kRowGap, width, row };
	const int sel = graph_->selected();
	controls_->layOut(strip, globals, sel, open);
	laidOutKey_ = controls_->honestyKey(sel);
	redraw();
}

bool EqCard::hasReadings() const {
	return host_.readingCountAt(device_) >= static_cast<size_t>(eq::Reading::kCount);
}

bool EqCard::updateContent() {
	if (!isVisible()) {
		return false;
	}
	bool changed = false;
	if (controls_->honestyKey(graph_->selected()) != laidOutKey_) {
		layOut();
		changed = true;
	}
	changed = graph_->update() || changed;
	const bool has = hasReadings();
	const float peak = has ? host_.readingAt(device_, static_cast<size_t>(eq::Reading::OutPeak)) : 0.0f;
	if (has != drawnHadReadings_ || peak != drawnPeak_) {
		drawnHadReadings_ = has;
		drawnPeak_ = peak;
		redraw();
		changed = true;
	}
	return changed;
}

void EqCard::drawHeadExtras(visage::Canvas& canvas) {
	drawSlotMenu(canvas, kMuted);
}

void EqCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	// THE OUTPUT METER (R14): the shared dynamics bar (`dyn_meter.h`, COMP's and CLIP's), since Bus
	// Utility's meter is a private draw of that card rather than a widget (assumption A10).
	// Too narrow for `drawNoSignal`'s words, so with nothing published it is an empty bar: the same
	// statement, in the column's own width.
	const visage::Bounds box = meterBounds();
	const float barHeight = std::max(0.0f, box.height() - dyn_meter::kCaptionHeight - 2.0f);
	dyn_meter::drawBar(canvas, box.x() + 0.5f * (box.width() - dyn_meter::kBarWidth), box.y(), barHeight,
	                   hasReadings() ? dyn_meter::levelFraction(drawnPeak_) : 0.0f, false, kMeterAccent);
	dyn_meter::drawCaption(canvas, "OUT", box.x() + 0.5f * box.width(), box.y() + barHeight + 2.0f, box.width());
	// AN OFF BAND SAYS SO where its strip would be (the honesty rule, A8).
	const visage::Bounds off = controls_->offCaption();
	if (off.width() > 0.0f) {
		const std::string text = "Band " + std::to_string(graph_->selected() + 1) + " is off";
		canvas.setColor(kMuted);
		canvas.text(text, font(kKnobLabelSize + 1.0f), visage::Font::kLeft, off.x(), off.y(), off.width(), off.height());
	}
}

void EqCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	controls_->setModulation(mod, history);
	for (size_t i = 0; i < eq::kParamCount; ++i) {
		control(i).setModulation(mod, history);
	}
}

std::vector<size_t> EqCard::addressedParamsForTest() const {
	std::vector<size_t> out = controls_->addressedParams();
	out.push_back(eq::paramIndex(eq::Param::TiltPivot)); // the diamond on the plot
	out.push_back(eq::paramIndex(eq::Param::Enabled));   // the head
	return out;
}

} // namespace feathers::ui
