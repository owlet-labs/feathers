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

#include "plugin/ui/zone_coord_cell.h"

#include "device/zone_param.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace feathers::ui {

namespace {

/// THE WIDEST STRING THE COORDINATE CAN PRINT, measured at the ENDPOINTS rather than sampled.
///
/// `ZoneCoordBox::widestReadoutWidth` carries the reasoning and it is unchanged: the readout renders
/// whole numbers, so the longest string is at one end of what the control can hold and there is
/// nothing between the ends that is longer than both. **Sampling is what produced three "it fits"
/// verdicts on a lane that did not.**
///
/// **WHAT IS GONE IS THE `named` BRANCH, AND ITS ABSENCE IS THE COLLAPSE.** The old zoned half printed
/// its position INSIDE a region, 0..127, so its widest string was not derivable from its declared
/// range. This box prints the coordinate itself, so the range is the whole answer.
[[nodiscard]] float widestReadoutWidth(const device::Modulatable& control) {
	const device::ParamDesc& desc = control.desc();
	const std::string ends[2] = { std::to_string(static_cast<long>(std::lround(desc.min))),
		                          std::to_string(static_cast<long>(std::lround(desc.max))) };
	float widest = 0.0f;
	for (const std::string& text : ends) {
		widest = std::max(widest, font(kKnobReadoutSize).stringWidth(std::u32string(text.begin(),
		                                                                            text.end())));
	}
	return widest;
}

} // namespace

ZoneCoordCell::ZoneCoordCell(device::Modulatable& coordinate, std::string label,
                             std::vector<ZoneName> names, double perLap)
    : label_(std::move(label)) {
	box_ = std::make_unique<ValueBox>(coordinate, "");
	box_->setCompact(true);
	// **ACCELERATED (backlog QB)** — the grabbed digit's rate when slow, up to `drag_accel::kMaxGain` times it on
	// a flick: a fast top speed and a precise slow speed, with acceleration between them.
	box_->setAccelerated(true);
	// AND A PRESS BESIDE A DIGIT GRABS THAT DIGIT — the first-grab-slow bug (see `ValueBox::pressPlaceAt`).
	box_->setNearestDigitPress(true);
	addChild(box_.get());

	// **WHETHER THERE IS ANYTHING TO NAME**, decided here while the list is still in hand. See the
	// header: a cell whose regions are all unnamed does not reserve a line for a dropdown that would
	// show nothing.
	for (const ZoneName& n : names) {
		if (!n.name.empty()) {
			hasNames_ = true;
			break;
		}
	}
	select_ = std::make_unique<ZoneSelect>(coordinate, std::move(names), perLap);
	addChild(select_.get());
	// **FREE ONLY: NO DROPDOWN, A SIGIL IN ITS ROOM (backlog IW).** A control whose zones are all unnamed has no modes
	// to choose between, so a selector would offer one entry and a row of lap numbers that name nothing. The selector
	// stays (its API is the control's), hidden; the sigil marks the lap.
	if (!hasNames_) {
		select_->setVisible(false);
		sigil_ = std::make_unique<Sigil>(coordinate, perLap);
		addChild(sigil_.get());
	}
	// **THE BOX'S OWN EDITS REPAINT ITS NEIGHBOURS (backlog IW: the glyphs did not update until the puck
	// moved).** A drag or a typed value commits through the box, which repaints ITSELF and nothing else — so the
	// sigil (and the selector) beside it, which read the value only when drawn, kept the old lap until something else
	// repainted the whole panel: moving the puck. Host changes and modulation arrive by the card's notify, which
	// repaints the subtree; this is the one road that did not. Counted, because a repaint cannot be seen headlessly.
	box_->onValueChanged([this]() {
		++neighbourRepaints_;
		select_->redraw();
		if (sigil_ != nullptr) {
			sigil_->redraw();
		}
	});
	// LAST — see `installTooltipHover`.
	installTooltipHover(*this, *this);
}

void ZoneCoordCell::setFaceDiameter(float diameter) {
	faceDiameter_ = diameter;
	resized();
	redraw();
}

void ZoneCoordCell::setLayout(Layout layout) {
	layout_ = layout;
	// A DROPDOWN WITH NOTHING TO SHOW IS NOT DRAWN. See the header: SKLITTER's zones are deliberately
	// unnamed, and reserving their line is most of why the cell looked oversized. **BOTH OF THE
	// HEIGHT-CONSTRAINED ARRANGEMENTS WANT THIS** — the square has exactly two rows and one of them
	// would be blank — while the column has room and has always drawn the line.
	if (layout_ != Layout::Column && !hasNames_) {
		select_->setVisible(false);
	}
	resized();
	redraw();
}

void ZoneCoordCell::setEnabled(bool on) {
	enabled_ = on;
	// **FORWARDED TO BOTH CHILDREN, AND BOTH CAN REFUSE.** This once reached only the selector, because
	// `ValueBox` had no enabled flag; it has one now (a greyed box ignores the hand, 2026-09-27), so a cell
	// drawn as off no longer leaves its box live under it — Resonate's Character at Drone Out 0 was that gap.
	select_->setEnabled(on);
	box_->setEnabled(on);
	if (sigil_ != nullptr) {
		sigil_->setEnabled(on);
	}
	redraw();
}

float ZoneCoordCell::coordinateRowTop() const {
	return columnRowsFor(faceDiameter_).value;
}

float ZoneCoordCell::nameRowTop() const {
	return columnRowsFor(faceDiameter_).middle;
}

visage::Bounds ZoneCoordCell::plateBounds() const {
	// **A SQUARE PLATE IS THE WHOLE CELL, INSET NOWHERE.** The boxes are square by design, and the
	// plate is the box a reader sees; a rectangle inset 3px across and flush down is 66 by 72, which
	// is not square however square the frame around it is. The 3px was buying separation between
	// plates seated side by side in a list — the corner layout buys that with `CircleRail::kCellGap`
	// instead, so nothing is lost by dropping it here.
	if (layout_ == Layout::Square) {
		return { 0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()) };
	}
	// **THE ROW'S OWN HEIGHT, not the column's.** `Knob::columnHeight` is face + caption + readout
	// stacked; on a list row the cell is given `Knob::rowHeight`, and a plate drawn at the column's
	// height simply ran out through the bottom of it.
	const float tall = layout_ == Layout::Row ? static_cast<float>(height())
	                                          : Knob::columnHeight(faceDiameter_);
	return { kPlateInsetX, 0.0f, std::max(0.0f, static_cast<float>(width()) - kPlateInsetX * 2.0f),
		     tall };
}

float ZoneCoordCell::minimumWidth() const {
	return 2.0f * (kPlateInsetX + kPlateInset) + widestReadoutWidth(box_->control());
}

float ZoneCoordCell::minimumSquareSide() const {
	// **ACROSS: the wider of the two rows, inside the inset `resized` seats them at.** The value row
	// prints the coordinate at its declared ends; the name row prints the widest region name between
	// two arrow cells. A square sized for only one of them crops the other, and which one it crops
	// depends on the device rather than on anything visible here.
	const float across = 2.0f * (kPlateInsetX + kPlateInset)
	                     + std::max(widestReadoutWidth(box_->control()),
	                                hasNames_ ? select_->minimumWidth() : Sigil::kSide);
	// **DOWN: the two rows stacked, with a plate inset above and below.** Never the binding term on
	// any device seen so far — 38 against an `across` in the sixties — but it is the term that would
	// bind if a device's names and readout were both short, and a square that assumed otherwise would
	// clip the row it stopped measuring.
	const float down = 2.0f * kPlateInset + ValueBox::kCompactHeight
	                   + Knob::kLineGap + Knob::kReadoutHeight; // the selector's row, or the sigil's
	return std::max(across, down);
}

void ZoneCoordCell::resized() {
	const float inset = kPlateInsetX + kPlateInset;
	const float usable = std::max(0.0f, static_cast<float>(width()) - inset * 2.0f);

	if (layout_ == Layout::Square) {
		// **TWO ROWS, THE WHOLE WIDTH EACH, CENTRED DOWN THE SQUARE — AND NO CAPTION LINE.** The row
		// layout gives its text column half the cell because a caption has to fit beside the number;
		// with the caption gone there is nothing to share with, so both rows get everything.
		const float h = static_cast<float>(height());
		const float block = ValueBox::kCompactHeight
		                    + Knob::kLineGap + Knob::kReadoutHeight;
		const float top = std::max(kPlateInset, (h - block) * 0.5f);
		box_->setBounds(inset, top, usable, ValueBox::kCompactHeight);
		if (hasNames_) {
			select_->setBounds(inset, top + ValueBox::kCompactHeight + Knob::kLineGap, usable,
			                   Knob::kReadoutHeight);
		}
		else if (sigil_ != nullptr) {
			sigil_->setBounds(inset, top + ValueBox::kCompactHeight + Knob::kLineGap, usable, Knob::kReadoutHeight);
		}
		return;
	}

	if (layout_ == Layout::Row) {
		// **THE VALUE BOX WHERE A KNOB'S FACE GOES**, and the two text lines beside it — `Knob`'s
		// `LabelPlacement::Right` arithmetic, so a row of these and a row of dials line up.
		const float h = static_cast<float>(height());
		const float boxWide = std::max(0.0f, std::min(usable * 0.5f, faceDiameter_ * 2.0f));
		box_->setBounds(inset, (h - ValueBox::kCompactHeight) * 0.5f, boxWide,
		                ValueBox::kCompactHeight);
		if (!hasNames_) {
			// NO SECOND LINE — the caption centres itself in `draw`; the sigil sits at the row's far end (backlog IW).
			if (sigil_ != nullptr) {
				sigil_->setBounds(static_cast<float>(width()) - inset - Sigil::kSide, (h - Sigil::kSide) * 0.5f,
				                  Sigil::kSide, Sigil::kSide);
			}
			return;
		}
		const float textX = inset + boxWide + Knob::kGap;
		const float textWide = std::max(0.0f, static_cast<float>(width()) - textX - inset);
		const float block = Knob::kLabelHeight + Knob::kLineGap + Knob::kReadoutHeight;
		const float top = (h - block) * 0.5f;
		select_->setBounds(textX, top + Knob::kLabelHeight + Knob::kLineGap, textWide,
		                   Knob::kReadoutHeight);
		return;
	}

	// **THE WHOLE ROW, WHERE TWO BOXES AND A COLON USED TO SPLIT IT.** The weighting `ZoneCoordBox`
	// computes — each half by its own widest string, the slack shared — has nothing to weigh once
	// there is one box, and the digits get the space the colon and the second inset were spending.
	box_->setBounds(inset, coordinateRowTop(), usable, ValueBox::kCompactHeight);
	select_->setBounds(inset, nameRowTop(), usable, Knob::kReadoutHeight);
	if (sigil_ != nullptr) {
		sigil_->setBounds(inset, nameRowTop(), usable, Knob::kReadoutHeight);
	}
}

void ZoneCoordCell::draw(visage::Canvas& canvas) {
	// THE PLATE — the same signature the zone assemblies wear, so a zoned control still announces
	// itself at a glance now that it no longer has a knob's silhouette to do it.
	const visage::Bounds plate = plateBounds();
	canvas.setColor(withAlpha(kLine, enabled_ ? 0.12f : 0.06f));
	canvas.roundedRectangle(plate.x(), plate.y(), plate.width(), plate.height(), kPlateRadius);
	canvas.setColor(withAlpha(kLine, enabled_ ? 0.7f : 0.3f));
	canvas.roundedRectangleBorder(plate.x(), plate.y(), plate.width(), plate.height(), kPlateRadius,
	                              1.0f);

	// **A SQUARE CELL DRAWS ITS PLATE AND STOPS.** The N/E/S/W cfg labels were clutter and are gone.
	// There is no caption line in the square's arithmetic either, so this is not a
	// drawing that hides a reserved row — the row does not exist, which is what makes the cell small.
	if (layout_ == Layout::Square) {
		return;
	}

	const visage::Font& labelFace = font(kKnobLabelSize);
	canvas.setColor(enabled_ ? kMuted : kDisabled);
	std::string caption = label_;
	for (char& c : caption) {
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	}
	if (layout_ == Layout::Row) {
		// **BESIDE THE BOX, ON THE LINE A KNOB PUTS ITS NAME.** Drawn at the column's
		// `faceDiameter + kGap` this fell outside a list row entirely and was clipped — the cell
		// looked like an unlabelled plate, and three of them in a column were unidentifiable.
		const float inset = kPlateInsetX + kPlateInset;
		const float usable = std::max(0.0f, static_cast<float>(width()) - inset * 2.0f);
		const float boxWide = std::max(0.0f, std::min(usable * 0.5f, faceDiameter_ * 2.0f));
		const float textX = inset + boxWide + Knob::kGap;
		const float block = Knob::kLabelHeight + Knob::kLineGap + Knob::kReadoutHeight;
		// CENTRED WHEN THERE IS NO SECOND LINE, so an unnamed cell does not sit high against its own
		// box with a hole under it.
		const float top = hasNames_ ? (static_cast<float>(height()) - block) * 0.5f
		                            : (static_cast<float>(height()) - Knob::kLabelHeight) * 0.5f;
		drawTracked(canvas, caption, labelFace, kKnobLabelTracking, textX, top, Knob::kLabelHeight);
		return;
	}

	const float captionWidth = trackedWidth(labelFace, caption, kKnobLabelTracking);
	// EXACTLY WHERE `Knob::draw` PUTS ITS CAPTION — `faceDiameter + kGap`, the knob's own arithmetic.
	drawTracked(canvas, caption, labelFace, kKnobLabelTracking,
	            (static_cast<float>(width()) - captionWidth) * 0.5f, columnRowsFor(faceDiameter_).caption,
	            Knob::kLabelHeight);
}

} // namespace feathers::ui

namespace feathers::ui {

ZoneCoordCell::ColumnRows ZoneCoordCell::columnRowsFor(float faceDiameter) {
	ColumnRows rows {};
	rows.value = std::max(kPlateInset, (faceDiameter - ValueBox::kCompactHeight) * 0.5f);
	rows.caption = Knob::columnHeight(faceDiameter) - kPlateInset - Knob::kLabelHeight;
	const float gapTop = rows.value + ValueBox::kCompactHeight;
	rows.middle = gapTop + std::max(0.0f, (rows.caption - gapTop - Knob::kReadoutHeight) * 0.5f);
	return rows;
}

} // namespace feathers::ui

namespace feathers::ui {

ControlFace::ModLook ZoneCoordCell::modLookFor(const device::Modulatable& c) const {
	const ModLook box = box_ != nullptr ? box_->modLookFor(c) : ModLook {};
	const ModLook zone = select_ != nullptr && select_->isVisible() ? select_->modLookFor(c) : ModLook {};
	return { box.drawn || zone.drawn, box.armedGlow || zone.armedGlow, box.routedMark || zone.routedMark };
}

} // namespace feathers::ui
