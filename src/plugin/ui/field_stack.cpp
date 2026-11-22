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

#include "plugin/ui/field_stack.h"

#include "plugin/ui/zone_coord_cell.h"

#include <algorithm>
#include <cctype>

namespace feathers::ui {

FieldStack::FieldStack(std::string caption) : caption_(std::move(caption)) {
	// LAST — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

const char* FieldStack::tooltipAt(visage::Point) const {
	if (!boxes_.empty()) {
		return tooltipOf(&boxes_.front()->control());
	}
	return select_ != nullptr ? tooltipOf(&select_->control()) : nullptr;
}

void FieldStack::offeredControls(std::vector<Offered>& out) const {
	// EVERY ROW AND THE SELECTOR: each is its own control, drag-editable and a modulation target.
	for (const std::unique_ptr<ValueBox>& box : boxes_) {
		out.push_back({ &box->control(), Road::Face });
	}
	if (select_ != nullptr) {
		out.push_back({ &select_->control(), Road::Face });
	}
}

ControlFace::ModLook FieldStack::modLookFor(const device::Modulatable& control) const {
	for (const std::unique_ptr<ValueBox>& box : boxes_) {
		if (&box->control() == &control) {
			return box->modLookFor(control);
		}
	}
	if (select_ != nullptr && &select_->control() == &control) {
		return select_->modLookFor(control);
	}
	return {};
}

ValueBox& FieldStack::addBox(std::unique_ptr<ValueBox> box, std::string label) {
	box->setLabel(std::move(label));
	box->setCompact(true);
	addChild(box.get());
	boxes_.push_back(std::move(box));
	resized();
	return *boxes_.back();
}

ZoneSelect& FieldStack::setSelector(device::Modulatable& choice, std::vector<ZoneSelect::ZoneName> names) {
	const auto count = static_cast<double>(names.size());
	select_ = std::make_unique<ZoneSelect>(choice, std::move(names), count > 0.0 ? count : 1.0);
	select_->setTightFace(true);
	addChild(select_.get());
	resized();
	return *select_;
}

void FieldStack::setFaceDiameter(float diameter) {
	faceDiameter_ = diameter;
	resized();
	redraw();
}

void FieldStack::setBoxEnabled(size_t i, bool on) {
	if (i < boxes_.size() && boxes_[i]->enabled() != on) {
		boxes_[i]->setEnabled(on);
		boxes_[i]->redraw();
	}
}

void FieldStack::setSelectorEnabled(bool on) {
	if (select_ != nullptr) {
		select_->setEnabled(on);
	}
}

void FieldStack::setBoxShown(size_t i, bool shown) {
	if (i < boxes_.size() && boxes_[i]->isVisible() != shown) {
		boxes_[i]->setVisible(shown);
		resized();
		redraw();
	}
}

void FieldStack::resized() {
	// THE ROWS TAKE THE PLATE'S WIDTH LESS ONE PIXEL, as a stacked `ModeValueCell`'s do.
	const float inset = ZoneCoordCell::kPlateInsetX + 1.0f;
	const float usable = std::max(0.0f, static_cast<float>(width()) - inset * 2.0f);
	constexpr float gap = 2.0f;
	float y = ZoneCoordCell::kPlateInset;
	for (const std::unique_ptr<ValueBox>& box : boxes_) {
		box->setBounds(inset, y, usable, ValueBox::kCompactHeight);
		// A ROW NOT SHOWN TAKES NO ROOM: the rows under it move up.
		y += box->isVisible() ? ValueBox::kCompactHeight + gap : 0.0f;
	}
	if (select_ != nullptr) {
		select_->setBounds(inset, y, usable, Knob::kReadoutHeight);
	}
}

void FieldStack::draw(visage::Canvas& canvas) {
	const float plateX = ZoneCoordCell::kPlateInsetX;
	const float plateW = std::max(0.0f, static_cast<float>(width()) - plateX * 2.0f);
	const float plateH = Knob::columnHeight(faceDiameter_);
	canvas.setColor(withAlpha(kLine, 0.12f));
	canvas.roundedRectangle(plateX, 0.0f, plateW, plateH, ZoneCoordCell::kPlateRadius);
	canvas.setColor(withAlpha(kLine, 0.7f));
	canvas.roundedRectangleBorder(plateX, 0.0f, plateW, plateH, ZoneCoordCell::kPlateRadius, 1.0f);
	const visage::Font& labelFace = font(kKnobLabelSize);
	canvas.setColor(kMuted);
	std::string caption = caption_;
	for (char& c : caption) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	const float captionWidth = trackedWidth(labelFace, caption, kKnobLabelTracking);
	drawTracked(canvas, caption, labelFace, kKnobLabelTracking, (static_cast<float>(width()) - captionWidth) * 0.5f,
	            ZoneCoordCell::columnRowsFor(faceDiameter_).caption, Knob::kLabelHeight);
}

} // namespace feathers::ui
