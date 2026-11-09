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

#include "plugin/ui/eq_expanded.h"

#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

namespace eq = fx::eq;

EqExpanded::EqExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	(void)registry;
	adapters_.resize(eq::kParamCount);
	const auto fn = [this](size_t i) -> DeviceControl& { return control(i); };
	graph_ = std::make_unique<EqGraph>(host_, device_, fn, false);
	graph_->onSelected = [this](int) { layOut(); };
	addChild(graph_.get());
	controls_ = std::make_unique<EqControls>(*this, host_, device_, fn, notifier_, kKnobDiameter, true);
}

DeviceControl& EqExpanded::control(size_t i) const {
	if (adapters_[i] == nullptr) {
		adapters_[i] = std::make_unique<DeviceControl>(host_, device_, eq::kDescriptor, i, mod_, history_);
	}
	return *adapters_[i];
}

std::string EqExpanded::title() const {
	return std::string(eq::kTraits.name) + " \xE2\x80\x94 Band " + std::to_string(graph_->selected() + 1);
}

float EqExpanded::bottomHeight() const {
	return EqControls::kSwitchRow + EqControls::kGap + Knob::columnHeight(kKnobDiameter);
}

visage::Point EqExpanded::minimumSize() const {
	const float w = kPad + kStripWidth + kRowGap + kGlobalsWidth + kPad;
	const float h = kPad + kMinPlotHeight + kRowGap + bottomHeight() + kPad;
	return { std::max(w, kPad + 400.0f + kRowGap + kMeterWidth + kPad), h };
}

visage::Bounds EqExpanded::meterBounds() const {
	const float plotH = std::max(0.0f, height() - kPad - kRowGap - bottomHeight() - kPad);
	return { width() - kPad - kMeterWidth, kPad, kMeterWidth, plotH };
}

void EqExpanded::resized() {
	layOut();
}

void EqExpanded::layOut() {
	const float plotW = std::max(0.0f, width() - 2.0f * kPad - kMeterWidth - kRowGap);
	const float plotH = std::max(0.0f, height() - kPad - kRowGap - bottomHeight() - kPad);
	graph_->setBounds(kPad, kPad, plotW, plotH);
	const float y = kPad + plotH + kRowGap;
	// THE STRIP ON THE LEFT, THE GLOBALS FILLING THE REST OF THE ROW — one row under the plot, however
	// wide the window is.
	const visage::Bounds strip { kPad, y, kStripWidth, bottomHeight() };
	const float gx = kPad + kStripWidth + kRowGap;
	const float gw = std::max(0.0f, width() - gx - kPad);
	const visage::Bounds globals { gx, y + EqControls::kSwitchRow + EqControls::kGap,
		                           std::min(gw, kGlobalsWidth * 1.5f), Knob::columnHeight(kKnobDiameter) };
	const int sel = graph_->selected();
	controls_->layOut(strip, globals, sel, true);
	laidOutKey_ = controls_->honestyKey(sel);
	redraw();
}

bool EqExpanded::updateContent() {
	if (!isVisible()) {
		return false;
	}
	bool changed = false;
	const int sel = graph_->selected();
	if (controls_->honestyKey(sel) != laidOutKey_ || sel != drawnSelected_) {
		drawnSelected_ = sel;
		layOut();
		changed = true;
	}
	changed = graph_->update() || changed;
	const bool has = host_.readingCountAt(device_) >= static_cast<size_t>(eq::Reading::kCount);
	const float peak = has ? host_.readingAt(device_, static_cast<size_t>(eq::Reading::OutPeak)) : 0.0f;
	if (has != drawnHadReadings_ || peak != drawnPeak_) {
		drawnHadReadings_ = has;
		drawnPeak_ = peak;
		redraw();
		changed = true;
	}
	return changed;
}

void EqExpanded::draw(visage::Canvas& canvas) {
	// An empty bar with nothing published, as on the card: `drawNoSignal`'s words do not fit the column.
	const visage::Bounds box = meterBounds();
	const float barHeight = std::max(0.0f, box.height() - dyn_meter::kCaptionHeight - 2.0f);
	dyn_meter::drawBar(canvas, box.x() + 0.5f * (box.width() - dyn_meter::kBarWidth), box.y(), barHeight,
	                   drawnHadReadings_ ? dyn_meter::levelFraction(drawnPeak_) : 0.0f, false, kMeterAccent);
	dyn_meter::drawCaption(canvas, "OUT", box.x() + 0.5f * box.width(), box.y() + barHeight + 2.0f, box.width());
	const visage::Bounds off = controls_->offCaption();
	if (off.width() > 0.0f) {
		const std::string text = "Band " + std::to_string(graph_->selected() + 1) + " is off";
		canvas.setColor(kMuted);
		canvas.text(text, font(kKnobLabelSize + 1.0f), visage::Font::kLeft, off.x(), off.y(), off.width(), off.height());
	}
}

void EqExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	controls_->setModulation(mod, history);
	for (size_t i = 0; i < eq::kParamCount; ++i) {
		control(i).setModulation(mod, history);
	}
}

std::vector<size_t> EqExpanded::addressedParamsForTest() const {
	return controls_->addressedParams();
}

} // namespace feathers::ui
