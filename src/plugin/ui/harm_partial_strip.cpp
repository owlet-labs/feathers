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

#include "plugin/ui/harm_partial_strip.h"

#include "device/format_value.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <string>

namespace feathers::ui {

namespace hrm = feathers::fx::harm;

HarmPartialStrip::HarmPartialStrip(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier)
    : host_(host), device_(id) {
	(void)notifier;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const auto at = static_cast<size_t>(k);
		// A CHIP FOR THE SWITCH AND A PILL FOR THE LEVEL — two states and a quantity, each the shape its job asks for.
		chips_[at] = std::make_unique<SwitchChip>(control(hrm::partialParam(k, hrm::PartialField::Active)), "Active");
		addChild(chips_[at].get());
		pills_[at] = std::make_unique<FaderPill>(control(hrm::partialParam(k, hrm::PartialField::Level)), "Level");
		addChild(pills_[at].get());
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		for (int k = 0; k < hrm::kPartialCount; ++k) {
			if (headerBounds(k).contains(e.position)) {
				if (onSelect) {
					onSelect(k);
				}
				return;
			}
		}
	};
	refresh();
}

DeviceControl& HarmPartialStrip::control(hrm::Param param) const {
	const size_t index = static_cast<size_t>(param);
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, hrm::kDescriptor, index);
	}
	return *controls_[index];
}

void HarmPartialStrip::setModulation(ModState* mod, EditHistory* history) {
	for (auto& c : controls_) {
		if (c != nullptr) {
			c->setModulation(mod, history);
		}
	}
}

void HarmPartialStrip::setSelected(int partial) {
	partial = std::clamp(partial, 0, hrm::kPartialCount - 1);
	if (partial == selected_) {
		return;
	}
	selected_ = partial;
	redraw();
}

visage::Bounds HarmPartialStrip::columnBounds(int partial) const {
	const float inner = std::max(0.0f, width() - 2.0f * kPadX);
	const float each = (inner - kColumnGap * static_cast<float>(hrm::kPartialCount - 1)) / static_cast<float>(hrm::kPartialCount);
	return { kPadX + (each + kColumnGap) * static_cast<float>(partial), 0.0f, std::max(0.0f, each), height() };
}

visage::Bounds HarmPartialStrip::headerBounds(int partial) const {
	const visage::Bounds c = columnBounds(partial);
	return { c.x(), kPadY, c.width(), kHeaderHeight };
}

visage::Bounds HarmPartialStrip::chipBounds(int partial) const {
	const visage::Bounds c = columnBounds(partial);
	const float y = kPadY + kHeaderHeight + kGap;
	return { c.x() + 2.0f, y + (kRowHeight - SwitchChip::kHeight) * 0.5f, kChipWidth, SwitchChip::kHeight };
}

visage::Bounds HarmPartialStrip::pillBounds(int partial) const {
	const visage::Bounds c = columnBounds(partial);
	const visage::Bounds chip = chipBounds(partial);
	const float x = chip.right() + 6.0f;
	return { x, kPadY + kHeaderHeight + kGap, std::max(0.0f, c.right() - 2.0f - x), kRowHeight };
}

size_t HarmPartialStrip::harmonicIndex(int partial) const {
	const double v = host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(partial, hrm::PartialField::Harmonic)));
	return static_cast<size_t>(std::clamp(v, 0.0, static_cast<double>(hrm::kHarmonicCount - 1)));
}

bool HarmPartialStrip::partialActive(int partial) const {
	return host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(partial, hrm::PartialField::Active))) >= 0.5f;
}

std::string HarmPartialStrip::headerText(int partial) const {
	const std::string number = std::to_string(partial + 1);
	const std::string label = hrm::harmonicLabel(harmonicIndex(partial));
	// THE RATIO AND ITS INTERVAL NAME WHERE THEY FIT, THE RATIO ALONE WHERE THEY DO NOT — a name cut short is worse than none.
	const float room = std::max(0.0f, headerBounds(partial).width() - 20.0f);
	if (trackedWidth(font(kKnobLabelSize), label, kKnobLabelTracking) <= room) {
		return number + "    " + label;
	}
	const size_t dot = label.find("  ");
	return number + "    " + (dot == std::string::npos ? label : label.substr(0, dot));
}

bool HarmPartialStrip::refresh() {
	bool changed = false;
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const auto at = static_cast<size_t>(k);
		const std::string header = headerText(k);
		const device::ParamDesc& levelDesc = hrm::kParamDescs[static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Level))];
		const std::string readout =
		    device::formatValue(levelDesc, host_.fieldAt(device_, static_cast<size_t>(hrm::partialParam(k, hrm::PartialField::Level))));
		const int active = partialActive(k) ? 1 : 0;
		if (header != drawnHeader_[at] || readout != drawnReadout_[at] || active != drawnActive_[at]) {
			drawnHeader_[at] = header;
			drawnReadout_[at] = readout;
			drawnActive_[at] = active;
			if (pills_[at] != nullptr) {
				pills_[at]->setReadout(readout);
				// A PARTIAL THAT IS OFF DRAWS ITS LEVEL DIM: the value is kept, and says it is not sounding.
				pills_[at]->setActive(active != 0);
			}
			changed = true;
		}
	}
	if (changed) {
		redraw();
	}
	return changed;
}

void HarmPartialStrip::resized() {
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const auto at = static_cast<size_t>(k);
		const visage::Bounds chip = chipBounds(k);
		chips_[at]->setBounds(chip.x(), chip.y(), chip.width(), chip.height());
		const visage::Bounds pill = pillBounds(k);
		pills_[at]->setBounds(pill.x(), pill.y(), pill.width(), pill.height());
	}
	refresh();
}

void HarmPartialStrip::draw(visage::Canvas& canvas) {
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		const visage::Bounds c = columnBounds(k);
		const bool selected = k == selected_;
		const bool active = partialActive(k);
		// THE SELECTION OUTLINE round the whole column — the tab, in the strip.
		canvas.setColor(selected ? kAccent : withAlpha(kLine, 0.35f));
		canvas.roundedRectangleBorder(c.x() - 2.0f, c.y() + 1.0f, c.width() + 4.0f, c.height() - 2.0f, 4.0f, selected ? 1.5f : 1.0f);
		const visage::Bounds header = headerBounds(k);
		canvas.setColor(selected ? kInk : (active ? kInk : kMuted));
		canvas.text(headerText(k), font(kKnobLabelSize), visage::Font::kLeft, header.x() + 4.0f, header.y(),
		            header.width() - 4.0f, header.height());
	}
}

} // namespace feathers::ui
