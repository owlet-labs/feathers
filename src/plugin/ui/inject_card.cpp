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

#include "plugin/ui/inject_card.h"

#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

namespace inj = fx::inject;

InjectCard::InjectCard(backplane::DeviceHost& host, WidgetRegistry&, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id), notifier_(notifier) {
	controls_.resize(static_cast<size_t>(inj::Param::kCount));
	mix_ = std::make_unique<Knob>(control(inj::Param::Mix), "Mix", notifier_);
	gain_ = std::make_unique<Knob>(control(inj::Param::Gain), "Gain", notifier_);
	for (Knob* knob : { mix_.get(), gain_.get() }) {
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob);
	}
	// THE HEAD'S OCCUPANTS — `CompCard`'s constructor: the press list only, the base's reorder lives on drag and release.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		else if (const auto nowOn = enablePressAt(e.position)) {
			if (notifier_ != nullptr) {
				notifier_->say(*nowOn ? "Sidechain Inject on" : "Sidechain Inject off");
			}
		}
	};
}

DeviceControl& InjectCard::control(inj::Param p) const {
	const size_t i = static_cast<size_t>(p);
	if (controls_[i] == nullptr) {
		controls_[i] = std::make_unique<DeviceControl>(host_, device_, inj::kDescriptor, i, mod_, history_);
	}
	return *controls_[i];
}

device::Modulatable* InjectCard::enableModulatable() {
	return &control(inj::Param::Enabled);
}

bool InjectCard::headEnabled() const {
	return control(inj::Param::Enabled).value() >= 0.5;
}

void InjectCard::setModulation(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;
	for (Knob* knob : { mix_.get(), gain_.get() }) {
		knob->setModulation(mod, history);
	}
	for (size_t i = 0; i < static_cast<size_t>(inj::Param::kCount); ++i) {
		control(static_cast<inj::Param>(i)).setModulation(mod, history);
	}
}

float InjectCard::openHeight() {
	return kSlotHeadHeight + 4.0f + kCellHeight + kRowGap + kLineHeight + kPad;
}

float InjectCard::laneHeightAt(float) const {
	return folded() ? foldedHeight() : openHeight();
}

float InjectCard::laneExtent(float cross) const {
	if (folded()) {
		return kCardHeadHeight;
	}
	return FxCard::laneExtent(cross);
}

float InjectCard::contentRight() const {
	return bodyLeft() + bodyWidth() - kPad - kMeterWidth - kMeterGap;
}

visage::Bounds InjectCard::meterBounds() const {
	const float top = kSlotHeadHeight + 4.0f;
	return { contentRight() + kMeterGap, top, kMeterWidth, kCellHeight };
}

void InjectCard::resized() { layOut(); }

void InjectCard::layOut() {
	const bool open = !folded();
	const float left = bodyLeft() + kPad;
	const float width = std::max(0.0f, contentRight() - left);
	const float y = kSlotHeadHeight + 4.0f;
	// `setBounds` UNCONDITIONALLY, so a scale change reaches every knob (CLAUDE.md's DPI rule).
	const float cell = width * 0.5f;
	mix_->setBounds(left, y, cell, kCellHeight);
	gain_->setBounds(left + cell, y, cell, kCellHeight);
	mix_->setVisible(open);
	gain_->setVisible(open);
	line_ = { left, y + kCellHeight + kRowGap, std::max(0.0f, bodyWidth() - 2.0f * kPad), kLineHeight };
}

void InjectCard::foldedChanged() { layOut(); }

bool InjectCard::hasReadings() const {
	return host_.readingCountAt(device_) >= static_cast<size_t>(inj::Reading::kCount);
}

float InjectCard::reading(inj::Reading r) const {
	return hasReadings() ? host_.readingAt(device_, static_cast<size_t>(r)) : 0.0f;
}

bool InjectCard::sidechainSilent() const {
	return hasReadings() && reading(inj::Reading::SidechainSounding) < 0.5f;
}

float InjectCard::barFraction() const {
	return hasReadings() ? dyn_meter::levelFraction(reading(inj::Reading::Injected)) : 0.0f;
}

void InjectCard::drawHeadExtras(visage::Canvas& canvas) {
	drawSlotMenu(canvas, kMuted);
}

bool InjectCard::updateContent() {
	bool changed = false;
	const bool has = hasReadings();
	if (has != drawnHadReadings_) {
		drawnHadReadings_ = has;
		changed = true;
	}
	const float bar = barFraction();
	if (bar != drawnBar_) {
		drawnBar_ = bar;
		changed = true;
	}
	if (const bool silent = sidechainSilent(); silent != drawnSilent_) {
		drawnSilent_ = silent;
		changed = true;
	}
	// **ASK FOR THE FRAME HERE — returning true is the report, not the delivery** (`CompCard::updateContent`).
	if (changed) {
		redraw();
	}
	return changed;
}

void InjectCard::drawMeter(visage::Canvas& canvas) {
	const visage::Bounds box = meterBounds();
	if (!hasReadings()) {
		dyn_meter::drawNoSignal(canvas, box);
		return;
	}
	const float barHeight = std::max(0.0f, box.height() - dyn_meter::kCaptionHeight - 2.0f);
	dyn_meter::drawBar(canvas, box.x() + box.width() * 0.5f - dyn_meter::kBarWidth * 0.5f, box.y(), barHeight, barFraction(),
	                   false, kMeterAccent);
	dyn_meter::drawCaption(canvas, "SC", box.x() + box.width() * 0.5f, box.y() + barHeight + 2.0f, box.width());
}

void InjectCard::drawContent(visage::Canvas& canvas) {
	if (folded()) {
		return;
	}
	drawMeter(canvas);
	if (sidechainSilent()) {
		// THE HONESTY LINE: with nothing on the sidechain, Mix toward it gives silence, and the card says why.
		canvas.setColor(kMuted);
		canvas.text("sidechain silent", font(kKnobLabelSize), visage::Font::kLeft, line_.x(), line_.y(), line_.width(),
		            line_.height());
	}
}

std::vector<inj::Param> InjectCard::addressedParamsForTest() const {
	return { inj::Param::Mix, inj::Param::Gain, inj::Param::Enabled };
}

Knob* InjectCard::knobForTest(inj::Param p) const {
	switch (p) {
	case inj::Param::Mix: return mix_.get();
	case inj::Param::Gain: return gain_.get();
	default: return nullptr;
	}
}

} // namespace feathers::ui
