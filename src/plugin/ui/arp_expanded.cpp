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

#include "plugin/ui/arp_expanded.h"

#include <algorithm>

namespace feathers::ui {

namespace ap = feathers::mod::arp;

ArpExpanded::ArpExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier)
    : GenericExpanded(host, registry, id, ap::kDescriptor, notifier) {
	lane_ = std::make_unique<ArpLane>(host, id);
	addChild(lane_.get());
	layOutBand();
}

ArpExpanded::~ArpExpanded() = default;

float ArpExpanded::bodyTopInset() const {
	return kLaneHeight + kBandGap + kCardOutputStripHeight + kBandGap;
}

void ArpExpanded::modulationBound(ModState* mod, EditHistory* history) {
	GenericExpanded::modulationBound(mod, history);
	// **THE CARD'S OWN HANDLES, AGAINST THE ONE OUTER STATE** — `SklitterExpanded`'s perform-pane chips: the same
	// `ModState` the card's chips arm, so the two are two views of one arm. No state, no handles (the strip's rule).
	if (mod == nullptr) {
		for (std::unique_ptr<SourceArm>& arm : arms_) {
			if (arm != nullptr) {
				removeChild(arm.get());
				arm.reset();
			}
		}
		return;
	}
	for (size_t n = 0; n < arms_.size(); ++n) {
		if (arms_[n] != nullptr) {
			continue;
		}
		arms_[n] = std::make_unique<SourceArm>(*mod, backplane::Source::output(deviceId(), n), ap::kOutputs[n].name);
		addChild(arms_[n].get());
	}
	layOutBand();
	redraw();
}

void ArpExpanded::layOutBand() {
	const float w = std::max(0.0f, static_cast<float>(width()) - 2.0f * kPad - bodyRightInset());
	lane_->setBounds(kPad, kPad, w, kLaneHeight);
	// THE SAME WALK EVERY HOST USES FOR A DEVICE'S CHIPS, so these sit and wrap as the card's do.
	// ONE ROW OF THE CARD'S OWN HEIGHT: the walk drops a row its strip is not tall enough for.
	const visage::Bounds strip { kPad, kPad + kLaneHeight + kBandGap, w, kCardOutputStripHeight };
	const std::vector<visage::Bounds> boxes = outputChipBoundsFor(ap::kDescriptor, strip);
	for (size_t n = 0; n < arms_.size(); ++n) {
		if (arms_[n] != nullptr && n < boxes.size()) {
			arms_[n]->setBounds(boxes[n].x(), boxes[n].y(), boxes[n].width(), boxes[n].height());
		}
	}
}

void ArpExpanded::resized() {
	GenericExpanded::resized();
	layOutBand();
}

bool ArpExpanded::updateContent() {
	const bool grid = GenericExpanded::updateContent();
	return lane_->poll() || grid;
}

} // namespace feathers::ui
