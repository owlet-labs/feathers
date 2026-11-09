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

#include "plugin/ui/featherverb_card.h"

#include <algorithm>

namespace feathers::ui {

namespace fv = fx::featherverb;

FeatherverbCard::FeatherverbCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier)
    : GenericCard(host, id, fv::kDescriptor, notifier) {}

visage::Bounds FeatherverbCard::meterBounds() const {
	if (folded()) {
		return {};
	}
	const auto& indices = paramIndicesForTest();
	const auto& rows = rowsForTest();
	for (size_t i = 0; i < indices.size() && i < rows.size(); ++i) {
		if (indices[i] != static_cast<size_t>(fv::Param::InputGain) || !rows[i]->isVisible()) {
			continue;
		}
		// THE ROW'S OWN FRAME, which already stands where the scroll put it (`GenericCard::drawContent`).
		const visage::Bounds cell = rows[i]->bounds();
		const float left = static_cast<float>(cell.right()) + kPad;
		const float right = static_cast<float>(width()) - kPad;
		const float wide = std::min(right - left, featherverb_meter::kWidth);
		if (wide <= 0.0f) {
			return {};
		}
		// CENTRED IN THE ROOM THE ROW LEFT, as tall as the knob's column.
		const float x = left + 0.5f * (right - left - wide);
		return { x, static_cast<float>(cell.y()), wide, static_cast<float>(cell.height()) };
	}
	return {};
}

void FeatherverbCard::drawBodyExtras(visage::Canvas& canvas) {
	featherverb_meter::draw(canvas, meterBounds(), featherverb_meter::read(ownHost(), deviceId()));
}

bool FeatherverbCard::updateContent() {
	bool moved = GenericCard::updateContent();
	// WHAT THE METER DRAWS, COMPARED RATHER THAN ASSUMED (`CompCard::updateContent`): a frame only when a bar moved,
	// and published-to-unpublished is a change too, so a meter that lost its data says "no signal".
	const featherverb_meter::Fractions now = featherverb_meter::read(ownHost(), deviceId());
	if (!(now == drawn_)) {
		drawn_ = now;
		moved = true;
	}
	// ASK FOR THE FRAME HERE — returning true is the report, not the delivery.
	if (moved) {
		redraw();
	}
	return moved;
}

} // namespace feathers::ui
