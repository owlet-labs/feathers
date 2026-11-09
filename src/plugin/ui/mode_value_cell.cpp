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

#include "plugin/ui/mode_value_cell.h"

#include "plugin/ui/theme.h"
#include "plugin/ui/zone_coord_cell.h"

#include <algorithm>
#include <cctype>

namespace feathers::ui {

ModeValueCell::ModeValueCell(device::Modulatable& mode, std::string caption,
                             std::vector<ZoneSelect::ZoneName> names, std::function<bool(size_t)> relevant)
    : caption_(std::move(caption)), relevant_(std::move(relevant)) {
	// ONE ZONE PER NAME: a lap is the whole list, so a step of the selector is one step of the Choice.
	const auto count = static_cast<double>(names.size());
	select_ = std::make_unique<ZoneSelect>(mode, std::move(names), count > 0.0 ? count : 1.0);
	addChild(select_.get());
	// LAST — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

ValueBox& ModeValueCell::addFollower(size_t param, std::unique_ptr<ValueBox> box) {
	// COMPACT, as `ZoneCoordCell`'s box is: the value row of a zone knob, not a captioned box.
	box->setCompact(true);
	// A ZONED FOLLOWER DRAGS AS `ZoneCoordCell`'s BOX DOES — accelerated (backlog QB).
	if (box->control().desc().zones > 0) {
		box->setAccelerated(true);
		box->setNearestDigitPress(true);
	}
	box->setVisible(false); // `refresh` decides
	addChild(box.get());
	params_.push_back(param);
	boxes_.push_back(std::move(box));
	return *boxes_.back();
}

ValueBox* ModeValueCell::boxFor(size_t param) const {
	for (size_t i = 0; i < params_.size(); ++i) {
		if (params_[i] == param) {
			return boxes_[i].get();
		}
	}
	return nullptr;
}

bool ModeValueCell::refresh() {
	long live = -1;
	for (size_t i = 0; i < params_.size(); ++i) {
		const bool on = live < 0 && relevant_ && relevant_(params_[i]);
		boxes_[i]->setVisible(on);
		if (on) {
			live = static_cast<long>(params_[i]);
		}
	}
	if (live == active_) {
		return false;
	}
	active_ = live;
	redraw();
	return true;
}

void ModeValueCell::setFaceDiameter(float diameter) {
	faceDiameter_ = diameter;
	resized();
	redraw();
}

// THE SAME ROWS `ZoneCoordCell`'s Column layout uses, so a Groove cell and Zone A's line up row for row.
float ModeValueCell::coordinateRowTop() const {
	return ZoneCoordCell::columnRowsFor(faceDiameter_).value;
}

float ModeValueCell::nameRowTop() const {
	return ZoneCoordCell::columnRowsFor(faceDiameter_).middle;
}

void ModeValueCell::resized() {
	const float inset = ZoneCoordCell::kPlateInsetX + ZoneCoordCell::kPlateInset;
	const float usable = std::max(0.0f, static_cast<float>(width()) - inset * 2.0f);
	for (const std::unique_ptr<ValueBox>& box : boxes_) {
		box->setBounds(inset, coordinateRowTop(), usable, ValueBox::kCompactHeight);
	}
	select_->setBounds(inset, nameRowTop(), usable, Knob::kReadoutHeight);
}

void ModeValueCell::draw(visage::Canvas& canvas) {
	const float plateX = ZoneCoordCell::kPlateInsetX;
	const float plateW = std::max(0.0f, static_cast<float>(width()) - plateX * 2.0f);
	const float plateH = Knob::columnHeight(faceDiameter_);
	canvas.setColor(withAlpha(kLine, enabled_ ? 0.12f : 0.06f));
	canvas.roundedRectangle(plateX, 0.0f, plateW, plateH, ZoneCoordCell::kPlateRadius);
	canvas.setColor(withAlpha(kLine, enabled_ ? 0.7f : 0.3f));
	canvas.roundedRectangleBorder(plateX, 0.0f, plateW, plateH, ZoneCoordCell::kPlateRadius, 1.0f);

	// NO FOLLOWER LIVE (Straight): the first follower's own print for this mode, muted — not a box, since
	// a drag there would change nothing (the honesty rule).
	if (active_ < 0 && !boxes_.empty()) {
		const std::string idle = boxes_.front()->readout();
		const float inset = ZoneCoordCell::kPlateInsetX + ZoneCoordCell::kPlateInset;
		canvas.setColor(kDisabled);
		canvas.text(idle, font(kKnobReadoutSize), visage::Font::kCenter, inset, coordinateRowTop(),
		            std::max(0.0f, static_cast<float>(width()) - inset * 2.0f), ValueBox::kCompactHeight);
	}

	const visage::Font& labelFace = font(kKnobLabelSize);
	canvas.setColor(enabled_ ? kMuted : kDisabled);
	std::string caption = caption_;
	for (char& c : caption) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	const float captionWidth = trackedWidth(labelFace, caption, kKnobLabelTracking);
	drawTracked(canvas, caption, labelFace, kKnobLabelTracking,
	            (static_cast<float>(width()) - captionWidth) * 0.5f, ZoneCoordCell::columnRowsFor(faceDiameter_).caption,
	            Knob::kLabelHeight);
}

} // namespace feathers::ui

namespace feathers::ui {

float ModeValueCell::minimumWidth() const {
	return 2.0f * (ZoneCoordCell::kPlateInsetX + ZoneCoordCell::kPlateInset) + select_->minimumWidth();
}

} // namespace feathers::ui

namespace feathers::ui {

void ModeValueCell::setEnabled(bool on) {
	enabled_ = on;
	select_->setEnabled(on);
	redraw();
}

} // namespace feathers::ui
