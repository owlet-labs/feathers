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

#pragma once

#include "plugin/ui/value_box.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/zone_select.h"
#include "plugin/ui/sigil.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// **ONE COORDINATE: A NUMBER AND THE NAME OF WHERE IT IS — backlog BJ, piece 3.**
//
// This is what `ZoneCoordBox` becomes once the pair behind it collapses: a zone dropdown and one
// value box.
//
// **THE FOOTPRINT IS UNCHANGED AND THAT IS DELIBERATE.** `ZoneCoordBox` already drew three rows — the
// coordinate, the caption, the region's name — and this draws the same three in the same places. Two
// things change inside them: the coordinate row holds ONE box instead of two boxes and a colon, and
// the name row stops being static text and becomes the selector. So a card's grid does not move, and
// the difference a reader sees is exactly the difference that was made.
//
// **WHAT THE COLON WAS SAYING IS NOW TRUE BY CONSTRUCTION.** Its own comment called it *"the thing
// that says the pair is ONE coordinate rather than two numbers that happen to be adjacent"* — a
// separator arguing that the two things it separates are one thing. There is one number now, so
// nothing has to argue.
//
// **AND THE NAME ROW IS WHERE THE ZONE BECOMES REACHABLE.** It was a caption you could read and not
// touch, over a partition you could only navigate by dragging the coordinate through it. It is now
// the dropdown BJ asked for, in the place the name already was.

namespace feathers::ui {

class ZoneCoordCell final : public visage::Frame, public TooltipSource {
public:
	/// THE CELL'S OWN SURFACE — its caption row — is the coordinate's, which is the one control the cell
	/// edits. See `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override { return tooltipOf(&box_->control()); }
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &box_->control(), Road::Face }); }
	/// WHAT ITS TWO FACES LIGHT (backlog QK): the box and the zone selector both draw the coordinate's modulation, so the
	/// cell answers with either — `ModeValueCell`'s rule, which asks its selector.
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override;

	using ZoneName = ZoneSelect::ZoneName;

	/// @param coordinate the single collapsed control — the whole quantity, unbounded
	/// @param label      the coordinate's name, drawn where a knob's caption goes
	/// @param names      the named regions, from the device beside its declaration
	/// @param perLap     how much coordinate one full pass through `names` is worth
	ZoneCoordCell(device::Modulatable& coordinate, std::string label, std::vector<ZoneName> names,
	              double perLap = ZoneSelect::kCoordinatePerLap);

	/// **THERE IS NO TWO-QUANTITY FORM ANY MORE.** One existed for OWLTT, whose coordinate and phase
	/// were genuinely different inputs while the coordinate was bounded 0..1 — it seated two stacked
	/// boxes. Unbounding the coordinate made the phase reachable through it (the whole part IS the
	/// lap), the phase params were deleted, and with them the dual-gang arrangement. Every zoned coordinate in the project is one control and one box.

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// THE SIZE IS THE CALLER'S, spelled the way `Knob` and `ZoneCoordBox` both spell it — a row of
	/// dials and a row of these line up because they are told the same number, not because two
	/// layouts happen to agree.
	void setFaceDiameter(float diameter);

	/// **THE THREE ARRANGEMENTS ONE CELL CAN TAKE, AS ONE VALUE RATHER THAN AS TWO BOOLS.** A cell that
	/// carried a `rowLayout_` and a `squareLayout_` would have a fourth state meaning "both", which is
	/// not an arrangement — and whichever branch happened to be tested first would silently win it.
	///
	/// - `Column` — the original and still the default: the value box where a knob's face goes, the
	///   caption under it, the zone name under that. OWLTT's panel, the shaper's, automod's.
	/// - `Row` — the list row; see `setRowLayout` below for why it exists.
	/// - `Square` — **THE PERFORM CIRCLE'S CORNER CELL, AND IT HAS NO CAPTION AT ALL.** The four
	///   `Cfg N/E/S/W` captions on that rail were clutter, so they are gone and the phase/dropdown
	///   boxes are square with two rows. The two
	///   rows are the VALUE and the ZONE NAME, stacked and centred in a square plate, and which
	///   waypoint a cell belongs to is said by where the rail puts it rather than by a word inside it.
	///   **That is a weaker claim than the caption made and `CircleRail` says so where it places them.**
	enum class Layout : uint8_t { Column, Row, Square };

	void setLayout(Layout layout);
	[[nodiscard]] Layout layout() const noexcept { return layout_; }

	/// **LAY THIS OUT AS A LIST ROW RATHER THAN AS A CAPTIONED COLUMN.**
	///
	/// On SKLITTER's card these cells were too large and some values and readouts were clipped.
	/// Both are one cause. This cell was built for OWLTT's PANEL, where it sits in a column
	/// the height of `Knob::columnHeight` — face, caption, readout stacked — and everything is placed
	/// from `faceDiameter_` on that assumption. A generic card's list gives it a ROW of
	/// `Knob::rowHeight` instead, which is half as tall: the plate overflowed it and the caption, drawn
	/// at the column's `faceDiameter + kGap`, landed outside the cell and was clipped away. Three
	/// unlabelled oversized plates, which is exactly what the capture showed.
	///
	/// **THE ROW IS `Knob`'S OWN, not a second arrangement.** `LabelPlacement::Right` puts the face at
	/// the left and two lines of text beside it; this puts the value box where the face goes and the
	/// caption and the zone name on those two lines. A row of knobs and a row of these then line up
	/// because they are the same arithmetic, which is the reason `setFaceDiameter` exists at all.
	///
	/// **IT IS A SPELLING OF `setLayout` AND NOT A SECOND FLAG.** One caller is left — `GenericCard`,
	/// `coord->setRowLayout(columnsFor(desc) <= 1)` — and "row or column" is the whole of the question
	/// it is asking, so it keeps the predicate it has rather than choosing between three things.
	void setRowLayout(bool on) { setLayout(on ? Layout::Row : Layout::Column); }
	[[nodiscard]] bool rowLayout() const noexcept { return layout_ == Layout::Row; }

	/// **WHETHER ANY REGION HAS A NAME.** SKLITTER declares eight EMPTY names for each of Zone A and
	/// Zone B — deliberately, because the zone names mean nothing in most of its modes — and
	/// a dropdown with nothing to show is a row of blank space in a cell that is already too tall. A
	/// cell with no names to offer does not reserve the line.
	[[nodiscard]] bool hasNames() const noexcept { return hasNames_; }
	[[nodiscard]] float faceDiameter() const noexcept { return faceDiameter_; }

	/// GREYED WITH ITS CARD, forwarded to both children — a cell that dimmed its own chrome while its
	/// box stayed live would be a control that looks off and is not.
	void setEnabled(bool on);
	[[nodiscard]] bool enabled() const noexcept { return enabled_; }

	/// **NO `setModulation` HERE, DELIBERATELY** — `ZoneCoordBox` carried that note and the reason is
	/// unchanged. Every surface holding one of these already sweeps every adapter it owns, which is
	/// the only road that covers a control no knob holds; a second road here could fall out of step
	/// with the first.

	[[nodiscard]] ValueBox& box() const noexcept { return *box_; }
	[[nodiscard]] ZoneSelect& select() const noexcept { return *select_; }
	/// The sigil a Free-only control shows where the selector was, or null — see `sigil_`.
	[[nodiscard]] const Sigil* sigil() const noexcept { return sigil_.get(); }
	/// **HOW MANY REPAINTS THE BOX'S EDITS DELIVERED TO ITS NEIGHBOURS** — the repaint itself is unobservable headlessly.
	[[nodiscard]] size_t neighbourRepaintsForTest() const noexcept { return neighbourRepaints_; }

	/// WHAT THE CELL NEEDS TO PRINT ITS NUMBER WITHOUT CROPPING IT. Asked by the layouts that have to
	/// decide whether a row fits — the same question `ZoneCoordBox::minimumWidth` answered, minus the
	/// colon and the second box that are no longer there.
	[[nodiscard]] float minimumWidth() const;

	/// **WHAT A `Square` CELL MUST BE, MEASURED — and the reason it is a method rather than a constant.**
	///
	/// The square holds two rows: the value box, whose widest string comes from the control's declared
	/// ends, and the zone selector, whose widest string comes from the device's own name list. Neither
	/// is knowable without a font, so neither can be written into a header — and `CircleRail::kCellSide`
	/// HAS to be a constant, because `rail.h` forbids `expandedWidth()` measuring anything.
	///
	/// So the two are joined the only honest way left: the rail states the side, and a case asserts
	/// that the side it states is at least this. The number stays checked without ever being measured
	/// on the layout path.
	[[nodiscard]] float minimumSquareSide() const;

	static constexpr float kPlateInset = 4.0f;
	static constexpr float kPlateInsetX = 3.0f;
	static constexpr float kPlateRadius = 6.0f;

	/// **THE COLUMN LAYOUT'S THREE ROWS — ONE ARITHMETIC FOR THIS CELL AND `ModeValueCell`** (centring the caption
	/// vertically left a large gap under the value and pushed the glyph off the bottom; the caption belongs at the bottom
	/// with the glyph centred between it and the value, in the plain zone boxes as much as in the ones with glyphs).
	/// The VALUE stays centred in the band a knob's face occupies, so its number lines up with the dials beside it; the
	/// CAPTION is pinned to the plate's bottom; the MIDDLE row — a sigil or a selector — is centred in the gap between.
	struct ColumnRows {
		float value;   ///< the value box's top
		float middle;  ///< the sigil's or selector's top
		float caption; ///< the caption's top
	};
	[[nodiscard]] static ColumnRows columnRowsFor(float faceDiameter);

private:
	[[nodiscard]] float coordinateRowTop() const;
	[[nodiscard]] float nameRowTop() const;
	[[nodiscard]] visage::Bounds plateBounds() const;

	std::string label_;
	std::unique_ptr<ValueBox> box_;
	std::unique_ptr<ZoneSelect> select_;
	/// **A FREE-ONLY CONTROL'S SIGIL (backlog IW)** — made when no zone has a name, in place of the selector: its
	/// dropdown would list one mode, so the room goes to a mark for the lap instead. Null when the control has modes.
	std::unique_ptr<Sigil> sigil_;
	/// HOW MANY TIMES THE BOX'S OWN EDITS REPAINTED THE SIGIL AND SELECTOR — produced by the hook that does it.
	size_t neighbourRepaints_ = 0;
	float faceDiameter_ = Knob::kEngineRailDiameter;
	Layout layout_ = Layout::Column;
	bool hasNames_ = false;
	bool enabled_ = true;
};

} // namespace feathers::ui
