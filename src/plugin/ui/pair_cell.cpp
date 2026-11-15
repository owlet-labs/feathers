/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */


#include "plugin/ui/pair_cell.h"

#include "plugin/ui/knob.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace feathers::ui {

PairCell::PairCell(std::string caption, visage::Frame& first, visage::Frame& second)
    : caption_(std::move(caption)), first_(first), second_(second) {
	addChild(&first_);
	addChild(&second_);
	// THE OUTLINE IS UNDER THE CELLS, and a press between them is nobody's.
	setIgnoresMouseEvents(true, true);
}

void PairCell::setSplit(float firstShare) {
	split_ = std::clamp(firstShare, 0.0f, 1.0f);
	resized();
	redraw();
}

void PairCell::resized() {
	const float w = static_cast<float>(width());
	const float h = std::max(0.0f, static_cast<float>(height()) - kCaptionHeight);
	const float firstW = w * split_;
	// `setBounds` UNCONDITIONALLY: a scale change moves nothing logical.
	first_.setBounds(0.0f, kCaptionHeight, firstW, h);
	second_.setBounds(firstW, kCaptionHeight, w - firstW, h);
}

void PairCell::draw(visage::Canvas& canvas) {
	const float w = static_cast<float>(width());
	const float h = static_cast<float>(height());
	// THE OUTLINE ROUND THE TWO CELLS, the caption over its top edge.
	const float top = kCaptionHeight - 1.0f;
	canvas.setColor(withAlpha(kLine, 0.55f));
	canvas.roundedRectangleBorder(0.5f, top, w - 1.0f, h - top - 0.5f, kRadius, 1.0f);
	const visage::Font& face = font(kKnobLabelSize);
	std::string caption = caption_;
	for (char& c : caption) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	canvas.setColor(kMuted);
	const float textW = trackedWidth(face, caption, kKnobLabelTracking);
	drawTracked(canvas, caption, face, kKnobLabelTracking, (w - textW) * 0.5f, 0.0f, kCaptionHeight - 2.0f);
}

float flowCells(const std::vector<FlowItem>& items, int columns, float x, float y, float columnWidth, float rowGap) {
	const int perRow = std::max(1, columns);
	int column = 0;
	float rowHeight = 0.0f;
	for (const FlowItem& item : items) {
		if (item.frame == nullptr) {
			continue;
		}
		if (!item.shown) {
			item.frame->setVisible(false);
			continue;
		}
		const int span = std::clamp(item.span, 1, perRow);
		if (column + span > perRow) {
			y += rowHeight + rowGap;
			column = 0;
			rowHeight = 0.0f;
		}
		item.frame->setBounds(x + static_cast<float>(column) * columnWidth, y, static_cast<float>(span) * columnWidth,
		                      item.height);
		item.frame->setVisible(true);
		rowHeight = std::max(rowHeight, item.height);
		column += span;
	}
	return y + rowHeight;
}

} // namespace feathers::ui
