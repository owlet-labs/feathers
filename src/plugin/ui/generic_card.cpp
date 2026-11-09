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

#include "plugin/ui/generic_card.h"

#include "plugin/ui/gamma_box.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/switch_chip.h"

#include "plugin/ui/theme.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

std::vector<ZoneSelect::ZoneName> GenericCard::zoneNamesFor(const device::ParamDesc& p) {
	std::vector<ZoneSelect::ZoneName> names;
	names.reserve(p.zones);
	for (const device::ZoneName& n : p.zoneNames) {
		names.push_back({ n.name == nullptr ? "" : n.name, n.description == nullptr ? "" : n.description });
	}
	names.resize(p.zones);
	return names;
}

GenericCard::GenericCard(backplane::DeviceHost& host, DeviceInstanceId id,
                         const device::Descriptor& desc, Notifier* notifier)
    : host_(host), desc_(desc), device_(id) {
	// **THE LIST SCROLLS UNDER THE WHEEL.** A one-column card of 21 controls cannot fit one unit of
	// cross by any arrangement — it is one column by design and is expected to scroll — and rows clipped at the card's foot are UNREACHABLE until this exists, which is strictly worse
	// than the hiding it replaced.
	//
	// BOTH AXES, SUMMED, as the rack's own lane does: most wheels only go up and down, and a diagonal
	// gesture should do something continuous rather than snap to whichever axis was larger this frame.
	onMouseWheel() = [this](const visage::MouseEvent& e) {
		if (!scrollable()) {
			return false; // NOT CONSUMED, so the lane behind gets the gesture instead.
		}
		// SIGNS AS THE RACK'S LANE USES THEM — see `RackEditor`'s wheel handler for why the two axes
		// carry opposite conventions. A card scrolls DOWN its list, so y drives it and x is the
		// trackpad's sideways gesture reaching the same list.
		const float delta = -e.wheel_delta_y + e.wheel_delta_x;
		setBodyScroll(bodyScroll() + delta * 40.0f);
		layOut();
		return true;
	};

	// ONE KNOB PER DECLARED CONTROL, addressed by its index in the descriptor's param list. The
	// generic DeviceControl constructor takes a descriptor and an index — no device type named, no host
	// identifier. The knob is built over that control, so it arms, glows and routes through the
	// unified modulation surface like every other control.
	// **THE CARD DROPS THE DEVICE'S OWN NAME FROM ITS CONTROLS' CAPTIONS.**
	//
	// `ParamDesc::name` is required to be device-qualified — "Filter Cutoff", not "Cutoff" — and the
	// reason is real: the HOST parameter table needs distinct names across every device, and the
	// first tenant's bare "Macro 1" collided with a global row. That rule is about the table.
	//
	// **ON A CARD IT IS NOISE, because the card is already titled with the device.** The delay draws
	// twelve rows reading DELAY TIME, DELAY FEEDBACK, DELAY TONE, DELAY SPREAD, DELAY MIX … under a
	// head that says `Delay`, which made a poor knob list. Six characters of every
	// caption spent repeating the title, on the narrowest surface in the editor.
	//
	// **STRIPPED HERE RATHER THAN DECLARED SHORT, and that is the difference between this and what
	// SKLITTER did.** That device declares bare `Rate`, `Density`, `Mix` and renders beautifully —
	// by opting out of the qualification rule rather than by satisfying it. This keeps the
	// descriptor's obligation to the host table intact and changes only what is PAINTED, so no
	// device has to choose between a readable card and a legal parameter name.
	//
	// Only a leading `<device name> ` is removed, and only when something is left over: a control
	// genuinely called `Delay` on a device called `Delay` keeps its caption rather than becoming
	// blank.
	const auto captionFor = [&](const device::ParamDesc& p) -> const char* {
		// **A DECLARED SHORT FORM WINS OUTRIGHT**, because it was written for exactly this surface and
		// any stripping rule applied on top of it would be second-guessing the device.
		if (p.shortName != nullptr && p.shortName[0] != '\0') {
			captions_.emplace_back(p.shortName);
			return captions_.back().c_str();
		}
		const std::string_view declared = p.name == nullptr ? std::string_view {} : p.name;
		const std::string_view device = desc.traits.name == nullptr ? std::string_view {}
		                                                           : desc.traits.name;
		std::string_view out = declared;
		if (!device.empty() && declared.size() > device.size() + 1
		    && declared.compare(0, device.size(), device) == 0 && declared[device.size()] == ' ') {
			out = declared.substr(device.size() + 1);
		}
		captions_.emplace_back(out);
		return captions_.back().c_str();
	};

	const auto addControl = [&](size_t index) {
		const device::ParamDesc& p = desc.params[index];
		// **A ZONED COORDINATE IS NOT A KNOB, AND THIS CARD DREW IT AS ONE.**
		//
		// `ParamDesc::zones` says the control is an infinite line with named regions on it — one
		// unit, one quantity, `zone : phase` on its readout. Every hand-written card in this project
		// was migrated onto `ZoneCoordCell` for it; a device reaching THIS card got the pre-migration
		// behaviour purely by not having a card of its own: a bounded-looking dial for an unbounded
		// number, no zone readout, no dropdown, no per-region drag.
		//
		// BUILT THE WAY `owltt_expanded.cpp` BUILDS ITS TWO — the device's OWN names, the widget's
		// own per-lap constant, and the same face diameter the knobs in the row get, which is what
		// makes the two kinds line up row for row rather than two layouts happening to agree.
		if (p.zones > 0) {
			auto coord = std::make_unique<ZoneCoordCell>(control(index), captionFor(p),
			                                            zoneNamesFor(p), ZoneSelect::kCoordinatePerLap);
			coord->setFaceDiameter(kKnobDiameter);
			// **A LIST ROW, NOT A CAPTIONED COLUMN.** This card's rows are `Knob::rowHeight` tall and
			// its knobs are told `LabelPlacement::Right`; a cell laid out for OWLTT's panel column
			// overflowed the row and drew its caption outside it. See `ZoneCoordCell::setRowLayout`.
			// IN A GRID THE CELL IS THE COLUMN, which is the layout this widget was built for.
			coord->setRowLayout(columnsFor(desc) <= 1);
			// THE DEVICE'S HOOK PRINTS THE BOX, as on the panel — see `GenericExpanded` (backlog IG).
			if (desc.text != nullptr && desc.text->format != nullptr) {
				const DeviceControl* adapter = &control(index);
				coord->box().setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
			}
			addChild(coord.get());
			rows_.push_back(coord.get());
			zoneCells_.push_back(std::move(coord));
			paramIndices_.push_back(index);
			return;
		}
		// **A DRAG WINDOW IS NOT A KNOB EITHER, AND THE GUARD THAT SAYS SO MATCHED A NAME.** See
		// `device::isDragWindow`: gamma and rotate declare a range seven orders of magnitude wide
		// because it is a window a drag moves through, not a domain — and `Knob::dragTo` maps one
		// 140px drag across the whole of it, so a knob here is a control with 120,000 units to the
		// pixel. `GammaBox` gives a drag sixteen laps whatever the range.
		//
		// A LABEL, WHICH `GammaBox` DEFAULTS TO EMPTY: its note says an empty one is for a surface
		// whose ROW names its cells, and this card's rows name themselves.
		if (device::isDragWindow(p) && !p.digitBox) { // a digit box is seated below, drag window or not (IO)
			auto box = std::make_unique<GammaBox>(control(index), captionFor(p));
			addChild(box.get());
			rows_.push_back(box.get());
			boxes_.push_back(std::move(box));
			paramIndices_.push_back(index);
			return;
		}
		// **THE DESCRIPTOR DECIDES, NOT THIS SITE.** A stepped Choice with names is a list to pick from
		// and dragging a dial through it is the gesture `40506c9` removed by design; `ValueBox` turns
		// itself into a dropdown from the same three facts, so nothing here has to know which control
		// of which device it is looking at.
		// **A TWO-WAY CHOICE IS A SWITCH, AND A SWITCH IS A CHIP**: an enable is a toggle, not a
		// dropdown. Engage drew as a `ValueBox` reading `Engage / Off` — a bordered
		// rectangle printing a word, identical to `Mode` beside it, which opens a list of eight.
		//
		// **AND IT WAS NEVER A DROPDOWN TO BEGIN WITH.** `ValueBox::choiceMenuAvailable` already declines
		// below three entries, so the box printed a word, refused to open, and dragged instead — it
		// looked like the one thing it would not do. `value_box.h` even names the answer where it
		// explains the floor: *"the switches on these cards are CHIPS anyway."* They were, hand-drawn
		// inside two cards and seatable by nobody; `SwitchChip` is that chip extracted.
		//
		// THE SAME THREE FACTS THE BOX USES, read one entry lower. No construction site opts in.
		if (device::isOffOnChoice(p)) {
			auto chip = std::make_unique<SwitchChip>(control(index), captionFor(p));
			addChild(chip.get());
			rows_.push_back(chip.get());
			chips_.push_back(std::move(chip));
			paramIndices_.push_back(index);
			return;
		}
		// **A DIGIT BOX — `ParamDesc::digitBox`, backlog IO.** A continuous control that asked for the
		// digit-place drag, which only a box has. It prints through the device's hook as a knob would.
		if (p.digitBox || (p.unit == device::Unit::Choice && !p.names.empty())) {
			auto box = std::make_unique<ValueBox>(control(index), captionFor(p));
			if (p.digitBox && desc.text != nullptr && desc.text->format != nullptr) {
				const DeviceControl* adapter = &control(index);
				box->setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
			}
			addChild(box.get());
			rows_.push_back(box.get());
			boxes_.push_back(std::move(box));
			paramIndices_.push_back(index);
			return;
		}
		auto knob = std::make_unique<Knob>(control(index), captionFor(p), notifier);
		knob->setFaceDiameter(kKnobDiameter);
		addChild(knob.get());
		rows_.push_back(knob.get());
		knobs_.push_back(std::move(knob));
		knobParamIndices_.push_back(index);
		paramIndices_.push_back(index);
	};

	// THE FACE, WHICH IS EVERY CONTROL UNLESS THE DEVICE SAYS OTHERWISE. See `Traits::faceControls`:
	// a device with a small playing surface and a menu behind it declares how many come first, and
	// the expanded view draws exactly the rest.
	for (const size_t i : faceIndicesFor(desc)) {
		// **NOT THE ENABLE — THE HEAD ALREADY DRAWS IT.** A generic Filter in the rack listed `FILTER
		// ENABLED / On` as its first row while the active switch sat in its head, so one field had two
		// controls: a player could turn a device off in one place and see it still on in the other,
		// and neither would be lying.
		//
		// ASKED OF THE DESCRIPTOR, not of a name. `enableControl` is the device's own statement about
		// which param is its bypass — the same number `FxCard::hasEnable` reads to decide whether to
		// draw the switch at all — so the two cannot disagree about which field they mean. Matching on
		// "Enabled" in the name would be a second rule, and a device that spelled it differently would
		// get both controls back without anything saying so.
		if (desc.traits.enableControl >= 0
		    && static_cast<size_t>(desc.traits.enableControl) == i) {
			continue;
		}
		addControl(i);
	}

	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
		// THE EXPAND VERB IS TESTED BEFORE THE FOLD, because it is INSIDE the head. `headBounds` is the
		// whole strip, so a press on the button would otherwise fold the card — the head's fold is the
		// fallback for "somewhere in the head that is not a button", and it has to be asked last.
		// (THE EXPAND MARK IS THE BASE'S NOW — `noteCardPress` acts on it, one site for every card.)
		//
		// **THE ENABLE, WHICH THIS CARD HANDLED NOWHERE.** It draws the square through the shared head
		// like every other card and had no arm to press it — so the bypass was dead on every device
		// whose card is the generic grid, which since 2026-09-02 includes the tracker. Enable/disable
		// was broken for every effect — either it disabled and never came back, or it did not disable
		// at all; this is the second half.
		// THE BASE OWNS THE TOGGLE — FxCard::enablePressAt. This card has no notifier, so it has
		// nothing to add and the arm exists only to stop the press becoming a fold.
		else if (enablePressAt(e.position)) {
		}
		else if (headBounds().contains(e.position)) {
			// **THE FOLD HAPPENS ON RELEASE, NOT HERE** — see `FxCard::onMouseUp`. This arm exists so a
			// press on the head is not dispatched as a body press; it has nothing to do.
		}
	};
}

DeviceControl& GenericCard::control(size_t index) {
	if (index >= controls_.size()) {
		controls_.resize(index + 1);
	}
	if (controls_[index] == nullptr) {
		// BUILT WITH THE PANEL STATE IT WILL NEED, because this is lazy and `setModulation` may already
		// have run — a control made afterwards would otherwise be the one knob on the card that never
		// glows, which is the same bug in miniature.
		controls_[index] =
		    std::make_unique<DeviceControl>(host_, device_, desc_, index, mod_, history_);
	}
	return *controls_[index];
}

visage::Bounds GenericCard::headBounds() const {
	return ownFrame().head;
}


float GenericCard::cellWidth() const {
	// **ONE HORIZONTAL UNIT, AND THE NAME RUNS THE WHOLE OF IT**: the name continues, unwrapped, for the
	// full effect width, which defaults to the horizontal unit.
	//
	// This used to MEASURE the longest control name and size the column to fit it, which makes every
	// device a different width — the opposite of the neat rows and columns the unit rule exists for.
	// A unit-wide column, with the text running the full width beside its knob, keeps the grid regular
	// and lets a long name use all the room there is.
	return kCardUnitWidth;
}

GenericCard::Grid GenericCard::gridFor(float cross) const {
	// **THE LIVE ROWS, so the lane reserves the room the card will actually use.** Every measurement
	// on this card flows from here — `laneExtent` asks the lane for space with `grid.along`, and a
	// count that included the rows the current mode hides would leave the reserved height and the
	// drawn height disagreeing. `test_gui_widgets_dahdsr.cpp`'s "EVERY card in the lane folds to its
	// head" caught exactly that: 456 reserved against 180 drawn.
	int cells = 0;
	for (const size_t index : paramIndices_) {
		if (relevant(index)) {
			++cells;
		}
	}

	// **ONE KNOB PER ROW, IN EITHER DIRECTION.** The generic layout puts one knob per row: the knob on
	// the left, and the name and value to its right on a line each, so two lines of text associate with
	// the knob, the name left-justified on top. It was a three-across grid of captioned dials; a list reads a device's controls as a
	// list of names, which is what a generic card is FOR — the custom cards are where a device's
	// controls get an arrangement that means something.
	if (axis_ == LaneAxis::Vertical) {
		// ONE COLUMN THE WIDTH OF THE CARD, growing DOWN — the flow axis is free, the cross is not.
		const float inner = std::max(0.0f, cross - 2.0f * kPad);
		return { 1, std::max(1, cells), inner };
	}

	// HORIZONTAL: **ONE COLUMN TOO, AND IT SCROLLS**, by design.
	//
	// So the two axes differ in which way the LIST runs, not in its shape: a generic card is one
	// column of rows in either lane. It flowed into extra columns here, capped at `kMaxFlowCells`,
	// with the remainder going to the expanded view — which made the same device a different shape
	// depending on the lane it was dropped into, and made "these controls are elsewhere" a thing a
	// player had to discover.
	//
	// **NOTHING IS HIDDEN ANY MORE; THE BODY SCROLLS.** That is what `FxCard::contentHeight` and
	// `drawBody` were built for, and it is the first road that actually needs them: a column of 21
	// rows will not fit one unit of cross by any arrangement, so the last resort is the only resort.
	(void)cross;
	return { 1, std::max(1, cells), cellWidth() };
}

/// **THE LIST'S HEIGHT IS ITS LIVE ROWS, NOT ITS DECLARED ONES.** A card that hid nine controls and
/// went on reserving room for them would scroll through empty space — the hiding would be invisible
/// where it matters most, which is the amount of screen the device asks for.
///
/// `heightFor` stays a function of the DESCRIPTOR because the lane sizes a card before there is an
/// instance to ask, so it is the upper bound; these two are the instance's own answer and they are
/// what the card lays out and scrolls against.
float GenericCard::rowHeightFor(const device::ParamDesc& p) {
	// THE CONSTRUCTOR'S OWN THREE QUESTIONS, IN ITS OWN ORDER. See the header for why this is asked
	// of the descriptor rather than of the widget that was built from it.
	if (p.zones > 0) {
		// A ZONE CELL IN ROW LAYOUT IS SIZED FOR A KNOB ROW — `ZoneCoordCell::setRowLayout`'s note
		// says so in as many words, and its plate, caption and `‹ name ›` selector fill it. Nothing
		// to reclaim here.
		return Knob::rowHeight(kKnobDiameter);
	}
	// **A DRAG WINDOW IS A `GammaBox`, AND A `GammaBox` MAKES ITSELF COMPACT** — one line in its own
	// constructor, invisible from here, which is why `ValueBox::naturalHeight` exists and why this
	// must name the compact height rather than the stacked one.
	if (device::isDragWindow(p) && !p.digitBox) { // a digit box is seated below, drag window or not (IO)
		return ValueBox::kCompactHeight;
	}
	// A TWO-WAY CHOICE IS A CHIP — see the constructor. `SwitchChip::kHeight` is `env_card.h`'s own.
	if (device::isOffOnChoice(p)) {
		return SwitchChip::kHeight;
	}
	// A NAMED CHOICE IS A PLAIN `ValueBox` THAT OPENS A MENU: caption over number, `ValueBox::kHeight`.
	if (p.unit == device::Unit::Choice && !p.names.empty()) {
		return ValueBox::kHeight;
	}
	return Knob::rowHeight(kKnobDiameter);
}

float GenericCard::liveRowsExtent() const {
	std::vector<size_t> liveIndices;
	for (const size_t index : paramIndices_) {
		if (relevant(index)) {
			liveIndices.push_back(index);
		}
	}
	const size_t live = liveIndices.size();
	const float extent = extentOf(desc_, liveIndices);
	// **THE FLOOR IS ONE KNOB ROW, NOT ONE BOX.** A card whose every control its mode has hidden still
	// reserves the taller of the two, because the floor exists so an empty body is not a zero-height
	// card and the conservative answer is the one that cannot clip.
	if (live == 0) {
		return Knob::rowHeight(kKnobDiameter) + kRowGap;
	}
	return extent;
}

float GenericCard::liveHeight(float cardWidth) const {
	// **THE FOOT IS AS MANY ROWS AS THE CHIPS NEED, NOT ONE.** It read `outputCount() > 0 ?
	// kCardOutputStripHeight : 0` — flat, one row — while `cardFrameOf` had already learned to
	// multiply by the row count, so this sum reserved 22px for a foot the frame then took several rows
	// for, and the body lost the difference out of the bottom of the rows it had already laid.
	// MEASURED: SKLITTER's seven chips wrap onto FOUR rows at the fx lane's card width, so 22 reserved
	// against 88 taken. **This is the SECOND defect in this one expression and it has the same first
	// subject**: the strip was missing from `laneExtent`'s restatement of it until that was collapsed
	// into this (see `laneExtent`), and then the strip that was finally here was one row deep.
	//
	// **AND `outputStripHeightAt` IS ASKED RATHER THAN `outputStripHeight()`**, because `laneExtent`
	// reaches here with the width the lane is ABOUT to hand over. See the declaration.
	//
	// NOTHING HERE ASKS THE CARD FOR ITS HEIGHT, which is the trap: `ModPanel::outputRowsOf` blew the
	// stack by asking the panel for a rectangle whose height came back through this very sum. Rows
	// are a question about the WIDTH, so there is no cycle to have.
	return kHeadHeight + kRowGap + bodyTopInset() + liveRowsExtent() + kPad
	       + outputStripHeightAt(cardWidth);
}

float GenericCard::contentHeight() const {
	// WHAT THE LIST WANTS, IGNORING WHAT IT WAS GIVEN — `FxCard::contentHeight`'s contract, and the
	// number `maxBodyScroll` measures the granted height against. A folded card wants only its head.
	if (folded()) {
		return foldedHeight();
	}
	// **THROUGH `heightFor`, WHICH IS THE SAME FORMULA THE LANE RESERVES WITH.** These were two
	// separate expressions for one layout and they disagreed; see `heightFor`.
	// AT THE WIDTH THE CARD HAS — this is what the body is scrolled against, so it is about the card
	// as it stands rather than about one the lane is still sizing. **Reading `width()` is right HERE
	// and is the trap in `laneExtent`**: a scroll extent is only meaningful for a card that has been
	// placed, so a zero here would be describing a card nobody can scroll anyway, while a lane asking
	// how much room to RESERVE is asking before the placement exists.
	return liveHeight(static_cast<float>(width()));
}

size_t GenericCard::faceCountFor(const device::Descriptor& desc) {
	// ZERO MEANS ALL, so a device that has never heard of this field behaves exactly as before, and
	// a count past the end is clamped rather than trusted.
	// A NAMED FACE IS AS LONG AS ITS NAMES THAT EXIST — see `faceIndicesFor`.
	if (!desc.traits.faceOrder.empty()) {
		return faceIndicesFor(desc).size();
	}
	const size_t declared = desc.traits.faceControls;
	return (declared == 0 || declared > desc.params.size()) ? desc.params.size() : declared;
}

std::vector<size_t> GenericCard::faceIndicesFor(const device::Descriptor& desc) {
	std::vector<size_t> out;
	if (!desc.traits.faceOrder.empty()) {
		for (const uint8_t index : desc.traits.faceOrder) {
			if (index < desc.params.size()) {
				out.push_back(index);
			}
		}
		return out;
	}
	const size_t declared = desc.traits.faceControls;
	const size_t face = (declared == 0 || declared > desc.params.size()) ? desc.params.size() : declared;
	for (size_t i = 0; i < face; ++i) {
		out.push_back(i);
	}
	return out;
}

int GenericCard::columnsFor(const device::Descriptor& desc) {
	return std::max(1, static_cast<int>(desc.traits.faceColumns));
}

float GenericCard::cellHeightFor(const device::ParamDesc& p) {
	// `rowHeightFor`'s questions, in its order, answered for a CAPTIONED COLUMN rather than a row.
	if (p.zones > 0) {
		return Knob::columnHeight(kKnobDiameter);
	}
	if (device::isDragWindow(p) && !p.digitBox) { // a digit box is seated below, drag window or not (IO)
		return ValueBox::kCompactHeight;
	}
	// **A STACKED CONTROL KEEPS ITS OWN COMPACT HEIGHT**, whatever it is, because the point of
	// stacking is that TWO of them fit in one knob cell. A switch given the pad treatment below
	// would make the pair half as tall again as the cell it is supposed to share.
	if (p.stacked) {
		return device::isOffOnChoice(p) ? SwitchChip::kHeight : ValueBox::kHeight;
	}
	// **A SWITCH IS A PAD THE SIZE OF A KNOB CELL**, so the gesture is something to hit rather than a
	// chip to find.
	if (device::isOffOnChoice(p)) {
		return Knob::columnHeight(kKnobDiameter);
	}
	if (p.unit == device::Unit::Choice && !p.names.empty()) {
		return ValueBox::kHeight;
	}
	return Knob::columnHeight(kKnobDiameter);
}

bool GenericCard::isPad(const device::Descriptor& desc, size_t index) {
	if (columnsFor(desc) <= 1 || index >= desc.params.size()) {
		return false;
	}
	const device::ParamDesc& p = desc.params[index];
	if (!device::isOffOnChoice(p)) {
		return false;
	}
	// A STACKED SWITCH IS NOT A PAD — it is sharing a cell, not filling one. See `cellHeightFor`.
	if (p.stacked) {
		return false;
	}
	const std::vector<size_t> face = faceIndicesFor(desc);
	return std::find(face.begin(), face.end(), index) != face.end();
}

std::vector<GenericCard::FaceRow> GenericCard::faceRowsOf(const device::Descriptor& desc,
                                                          const std::vector<size_t>& indices) {
	const auto across = static_cast<size_t>(std::max(1, columnsFor(desc)));
	const auto headingAt = [&desc](size_t paramIndex) -> const char* {
		// Asked by PARAM INDEX rather than by position in the face, because `faceOrder` can move a
		// control and its index cannot.
		for (const device::FaceSection& sec : desc.traits.faceSections) {
			if (static_cast<size_t>(sec.firstControl) == paramIndex && sec.label != nullptr
			    && sec.label[0] != '\0') {
				return sec.label;
			}
		}
		return nullptr;
	};

	// **A STACKED CONTROL IS NOT A CELL** — it shares the cell of the control before it, so it must
	// not consume one of the row's `across` slots or the row would break a column early. First in
	// the list has nothing to stack onto, which `ParamDesc::stacked` documents.
	const auto sharesPreviousCell = [&desc, &indices](size_t at) {
		return at > 0 && desc.params[indices[at]].stacked;
	};

	std::vector<FaceRow> rows;
	size_t i = 0;
	while (i < indices.size()) {
		// **A SECTION ALWAYS STARTS A NEW ROW**, so a heading never lands mid-row with two of the
		// previous group's controls beside it. In a one-column face that costs nothing; in a grid it
		// is what makes the heading mean the cells under it rather than the cells around it.
		FaceRow row {};
		row.heading = headingAt(indices[i]);
		row.first = i;
		row.count = 1;
		size_t cells = sharesPreviousCell(i) ? 0u : 1u;
		++i;
		while (i < indices.size() && headingAt(indices[i]) == nullptr) {
			const size_t next = cells + (sharesPreviousCell(i) ? 0u : 1u);
			if (next > across) {
				break;
			}
			cells = next;
			++row.count;
			++i;
		}
		rows.push_back(row);
	}
	return rows;
}

float GenericCard::extentOf(const device::Descriptor& desc, const std::vector<size_t>& indices) {
	const bool grid = columnsFor(desc) > 1;
	float extent = 0.0f;
	// **MEASURED OFF `faceRowsOf`, WHICH IS WHAT `layOut` PLACES.** `indices` is the LIVE set, so a
	// section whose every control is gated away never reaches this loop and costs no heading —
	// which is the same reason it draws none.
	for (const FaceRow& row : faceRowsOf(desc, indices)) {
		if (row.heading != nullptr) {
			extent += kSectionHeight;
		}
		// **A STACKED PAIR IS ONE CELL AS TALL AS BOTH OF THEM**, which is the number `layOut` places
		// with. Measuring them as two competing cells would reserve the taller of the two and clip
		// whichever sat underneath.
		float tallest = 0.0f;
		float cell = 0.0f;
		for (size_t k = row.first; k < row.first + row.count; ++k) {
			const device::ParamDesc& p = desc.params[indices[k]];
			const float h = grid ? cellHeightFor(p) : rowHeightFor(p);
			if (grid && k > 0 && p.stacked) {
				cell += kStackGap + h;
			}
			else {
				cell = h;
			}
			tallest = std::max(tallest, cell);
		}
		extent += tallest + kRowGap;
	}
	return extent;
}

size_t GenericCard::knobCountFor(const device::Descriptor& desc) {
	// THE CONSTRUCTOR'S OWN RULE, ASKED OF THE DESCRIPTOR ALONE — params less the enable, which the
	// head draws as its square. See the constructor: a device listing its enable as a knob had one
	// field with two controls, and a player could turn it off in one place and see it on in the other.
	const std::vector<size_t> face = faceIndicesFor(desc);
	size_t knobs = face.size();
	if (desc.traits.enableControl >= 0
	    && std::find(face.begin(), face.end(), static_cast<size_t>(desc.traits.enableControl)) != face.end()) {
		--knobs;
	}
	return knobs;
}

float GenericCard::heightFor(const device::Descriptor& desc, float cardWidth) {
	// **THIS WAS THE OLD THREE-ACROSS GRID'S FORMULA AND THE LAYOUT STOPPED BEING A GRID.** It read
	// `ceil(knobs / kRowCells)` rows of `Knob::columnHeight` — a captioned dial in one of three
	// columns — while `layOut` has placed ONE KNOB PER ROW at `Knob::rowHeight` since the decision
	// to use one knob per row. So a lane reserved a third of the rows it then drew.
	//
	// **NOTHING CAUGHT IT FOR AS LONG AS NO FX DEVICE USED THIS CARD.** The only generic card in the
	// build was the tracker's, and a modulator sits in the rack rather than the fx lane, so the case
	// that measures a card against the height it claims — *"EVERY card in the lane folds to its head
	// when the head is pressed"* — never had a subject here. Bus Utility is the first, and it failed
	// that case immediately: `height() == 364` against a claimed `laneHeight() == 267`.
	//
	// **THE FIX IS ONE FORMULA RATHER THAN A CORRECTED SECOND ONE.** `contentHeight` was already
	// right — it is what `layOut` and `maxBodyScroll` agree with — so the two have been collapsed
	// into this, and a future divergence has nowhere to hide. The output strip comes from the
	// descriptor for the same reason it did there: a device that publishes gets a strip, and a static
	// answer that omitted it would be a third disagreement waiting.
	// **THE SAME SUM AS `liveRowsExtent`, OVER THE DECLARED ROWS RATHER THAN THE LIVE ONES** — the
	// upper bound, because a lane sizes a card before there is an instance whose gate can be read.
	// It walks the face the constructor walks, skipping the enable for the same reason the
	// constructor does, so the row it counts and the row it measures are the same row.
	std::vector<size_t> rows;
	for (const size_t i : faceIndicesFor(desc)) {
		if (desc.traits.enableControl >= 0 && static_cast<size_t>(desc.traits.enableControl) == i) {
			continue;
		}
		rows.push_back(i);
	}
	float extent = extentOf(desc, rows);
	if (rows.empty()) {
		extent = Knob::rowHeight(kKnobDiameter) + kRowGap;
	}
	// **ONE ROW PER ROW OF CHIPS, AS `liveHeight` RESERVES IT.** A static answer that assumed one row
	// would be the third disagreement this function's own note warns about — and it would be wrong for
	// exactly the device that needs it, since a device with one output needs one row anyway.
	//
	// **`cardWidth` IS PASSED AS THE STRIP WIDTH WITH NO TRANSPOSE, AND THAT IS A PRECONDITION RATHER
	// THAN AN OVERSIGHT.** `FxCard::outputStripWidthAt` exists because a LEFT head takes the spine out
	// of the strip's width; this function is static, has no instance to ask which edge it is on, and
	// answers a HEIGHT — which only a vertical lane asks for. Its one caller is Feathers' modulator
	// strip, whose head is on top and whose strip therefore spans the card. A horizontal lane must not
	// call this: it wants a width, and `laneExtent`'s own branch is what answers that.
	return kHeadHeight + kRowGap + extent + kPad
	       + kCardOutputStripHeight * static_cast<float>(outputChipRowsFor(desc, cardWidth));
}

float GenericCard::openHeight() const {
	// THE VERTICAL ANSWER, **AT THE WIDTH THIS CARD CURRENTLY HAS — AND IT DOES DEPEND ON THAT
	// WIDTH.** This comment used to say the opposite: *"it does not depend on the width: three columns
	// whatever the card is given, so the row count is a property of the knob count alone."* That was
	// true of the rows and stopped being true of the whole card the day the outputs foot arrived — the
	// chips wrap, so the foot is as many rows as they need AT THIS WIDTH (`liveHeight`, through
	// `outputStripHeightAt`). Believing the old sentence is how `ModPanel::resized` came to read this
	// off a card `ensureCard` had just built, at a width of zero, and give a seven-output device a
	// one-row foot.
	//
	// **SO THIS IS ONLY A VALID ANSWER FOR A CARD THAT HAS BEEN PLACED.** A host sizing a card asks
	// `laneExtent(cross)`, which takes the width it is about to hand over; a host reserving space
	// before any card exists asks the static `heightFor(desc, cardWidth)`. Both are width-taking, and
	// this is the third spelling for the one case where the card's own width is the right width.
	//
	// THROUGH `heightFor`, so a lane reserving space and a card drawing in it cannot disagree.
	return liveHeight(static_cast<float>(width()));
}

size_t GenericCard::hiddenCount() const {
	// ASKED WITH THE CROSS THIS CARD ACTUALLY HAS, so the answer is about what is on screen rather
	// than about what would be at some other size. Same road `layOut` takes, for the same reason.
	//
	// **IT COUNTS HIDDEN CONTROLS AND NOT HIDDEN OUTPUTS, AND THAT IS A KNOWN GAP RATHER THAN AN
	// OVERSIGHT.** Since the 2026-09-19 decision an output that cannot be drawn inside its card is
	// hidden (`ui::layoutOutputChips`), so a card can now be showing fewer outputs than its device
	// publishes with nothing in the picture saying so — the `+N` mark this count feeds says nothing
	// about the foot. **It is deliberately NOT built**, and there are TWO ways a chip can be hidden,
	// so the reason has to cover both:
	//
	//   DOWN — its row lies past the strip's own height. Unreachable by a player today: the rack
	//          clips rather than re-lays a short window, so the foot is below the sill either way.
	//          This is the case the note originally gave, and it was the whole of the note.
	//   ACROSS — the chip is wider than the strip's usable width, so no row could hold it. This is
	//          NOT a property of the layout at all: it is a property of the DEVICE'S OUTPUT NAME
	//          against the narrowest card that name can appear on, so it holds by a margin rather
	//          than by construction. **MEASURED, and the margin is wide rather than nervous:** the
	//          widest name in the registry is SKLITTER's `"Perf Speed"` at 87.84px against 198px
	//          usable — 110.16 to spare, room for a name about twice as long. It is still **pinned
	//          by a case rather than by this sentence**, because a margin nothing checks is a claim:
	//          see *"every declared output chip fits the narrowest strip it can be laid into"* in
	//          `test_gui_widgets_dahdsr.cpp`, which measures every name in the registry and reports
	//          the slack. A new output called something long goes red there instead of quietly
	//          vanishing from a card.
	//
	// A signal for a state nobody can reach is furniture. **Backlog FT** carries it with the design
	// question about which should give way first, the body or the foot.
	const bool vertical = axis_ == LaneAxis::Vertical;
	const Grid grid = gridFor(vertical ? static_cast<float>(width()) : static_cast<float>(height()));
	const size_t shown = static_cast<size_t>(grid.across) * static_cast<size_t>(grid.along);
	return rows_.size() > shown ? rows_.size() - shown : 0;
}

float GenericCard::laneExtent(float cross) const {
	if (folded()) {
		// A FOLDED CARD IS ITS HEAD IN EITHER DIRECTION, and the head's extent along the flow axis is
		// what the lane needs. Vertically that is the head's height; horizontally a folded card is a
		// narrow spine, which is `kCardHeadHeight` turned on its side — the same number, meaning the
		// same thing, which is the point of the transpose: in h-mode the collapse is
		// vertical, like the fx lane in Feathers.
		return kCardHeadHeight;
	}
	const Grid grid = gridFor(cross);
	if (axis_ == LaneAxis::Vertical) {
		// THE FLOW IS THE HEIGHT, and a vertical lane scrolls the whole card, so it may ask for all of
		// it.
		//
		// **THROUGH `liveHeight()`, WHICH IS THE ONE EXPRESSION — it was this sum restated, MINUS THE
		// OUTPUT STRIP, and the omission was a real 22px under-reservation waiting for its first
		// subject.** `liveRowsExtent` was already shared (a count times one height stopped being the
		// list's extent the moment the rows stopped agreeing about their height); the strip was not,
		// because no `GenericCard`-family device in the fx lane had ever declared an output — every
		// `src/fx/*` descriptor said `.outputs = {}`. SKLITTER, the registry's first device that is both
		// an audio device and a modulation source, is that subject: the lane reserved 224 for a card
		// that answers 246, and `test_gui_widgets_dahdsr.cpp`'s "EVERY card in the lane folds to its
		// head" caught it — the same case that caught 456 reserved against 180 drawn.
		//
		// `grid` above is not consulted on this branch and was not before: a vertical `GenericCard` is
		// one column whatever the cross is, so its extent is a property of the rows alone.
		//
		// **EXCEPT THE FOOT, WHICH IS THE ONE PART THAT DOES DEPEND ON THE CROSS**: how many rows of
		// output chips fit is a question about the width, so the CROSS is what `liveHeight` is asked
		// about here — not `width()`, which on a card the lane has not placed yet is zero.
		return liveHeight(cross);
	}
	// **ONE UNIT WIDE, WHATEVER THE LIST'S LENGTH.** The flow here is the WIDTH and a one-column list
	// does not consume it — the rows go down and the body scrolls. Answering the list's height would
	// hand the lane a width taken from the wrong axis, which is the transpose bug in miniature.
	//
	// **PLUS THE HEAD**, exactly as the vertical branch above adds `kHeadHeight` — and this is the half
	// of the transpose that was missing. The unit is what the BODY gets; the head is an addition in
	// both directions or the same list is 24px narrower on a spine than in a column. It went unseen
	// because the knobs were placed from the CARD's origin rather than the body's, so the column
	// overhung the head by precisely the amount the card was short: two errors that cancelled in the
	// picture and did not cancel under the header.
	return kCardHeadHeight + grid.cell + 2.0f * kPad;
}

void GenericCard::foldedChanged() {
	// EVERY ROW, whichever widget it became — a box left visible on a folded card would draw over the
	// head exactly as a knob would.
	//
	// **AN UNFOLD IS A LAYOUT, NOT A `setVisible(true)` ON EVERY ROW (backlog GM).** Showing them all
	// brought back the rows the device's MODE gates away (SKLITTER's Density and P Write in Burst), at
	// whatever bounds they last had, over the rows that had closed up behind them — and nothing re-laid
	// them, because the gate had not moved, until a resize did. `layOut` is the one place that asks the
	// gate, so the unfold goes through it.
	if (!folded_) {
		layOut();
		return;
	}
	for (visage::Frame* row : rows_) {
		row->setVisible(false);
	}
}

void GenericCard::setModulation(ModState* state, EditHistory* history) {
	// THE CONTROL NEEDS THIS, NOT JUST THE KNOB — and only the knob was getting it.
	//
	// `Knob::mappable` does not decide whether it may be routed to; it ASKS its control
	// (`control_->display().routable`), and `DeviceControl::display` computes `armed` from its OWN
	// `mod_`. A knob that had been handed the panel state while its control had not would know it was
	// armed and be told, by the thing it asks, that nothing was. So no knob ever glowed and no route
	// could be drawn by hand: "when i click arrows, knobs dont glow".
	//
	// ONE FACT, TWO OBJECTS, AND ONLY ONE OF THEM TOLD. `ModDisplay`'s whole purpose is that the gate
	// and the glow are one answer; that only holds if the thing computing it has been given what it
	// needs to compute it.
	mod_ = state;
	history_ = history;
	for (auto& control : controls_) {
		if (control != nullptr) {
			control->setModulation(state, history);
		}
	}
	for (auto& knob : knobs_) {
		knob->setModulation(state, history);
	}
}

/// **THE GATE IS POLLED, BECAUSE NOTHING TELLS A CARD THAT ANOTHER CONTROL MOVED.** Changing Mode is
/// an ordinary write to one field; the card that draws nine other controls has no subscription to it.
/// Without this the hiding would be correct and invisible — right on the next resize, and wrong for
/// every frame until one. Asked once per paint, against one float.
bool GenericCard::updateContent() {
	// **THE PER-FRAME HOOK, WHICH IS NOT INSIDE A PAINT.** The gate poll lived in `drawContent` and
	// therefore MOVED CHILD BOUNDS WHILE THE CANVAS WAS BEING DRAWN. `FxCard::update` is the road
	// every host drives every frame for exactly this — a meter, a playhead, a graph — and its return
	// value is "did I ask for a repaint", which a re-layout genuinely does.
	//
	// **SUSPECTED RATHER THAN PROVEN, AND MOVED ANYWAY.** `gui-widgets-lane`'s pixel-capture case
	// began failing intermittently with assertion errors — `lit > 0` reading zero, `widestChroma`
	// reading 191 — in the runs after the poll went into `drawContent`. MEASURED: the case alone
	// passes 5/5 and the shard passes 3/3, so the rate is too low to attribute at a cost worth
	// paying. Laying out during a paint is a hazard whatever that case is doing, the correct hook
	// already existed, and moving it costs nothing.
	return relayoutIfGateMoved();
}

bool GenericCard::relayoutIfGateMoved() {
	double now = 0.0;
	for (const size_t index : paramIndices_) {
		if (index < desc_.params.size() && desc_.params[index].gateControl >= 0) {
			now = host_.fieldAt(device_, static_cast<size_t>(desc_.params[index].gateControl));
			break;
		}
	}
	// **AND ANY ROW'S RELEVANCE, NOT ONLY THE FIRST GATE (backlog IO addendum).** The gate above is the first
	// one found; a row gated on ANOTHER control (SKLITTER's Groove Amount and Free Phase are gated on Groove
	// Mode, not Mode) moved nothing here, and the panel had the same gap and showed Amount in Free. Compared
	// every tick, in place, so it allocates nothing after the first.
	if (lastLiveSet_.size() != paramIndices_.size()) {
		lastLiveSet_.assign(paramIndices_.size(), 2u); // neither live nor dead: the first tick always moves
	}
	bool setMoved = false;
	for (size_t n = 0; n < paramIndices_.size(); ++n) {
		const uint8_t live = relevant(paramIndices_[n]) ? 1u : 0u;
		if (live != lastLiveSet_[n]) {
			lastLiveSet_[n] = live;
			setMoved = true;
		}
	}
	if (now != lastGate_ || setMoved) {
		lastGate_ = now;
		layOut();
		// **AND THE HOST IS TOLD IF THE CARD'S OWN HEIGHT MOVED, which is a different event from
		// the rows moving inside it.**
		//
		// The card resizes larger and smaller with the grain state rather than showing a scroll
		// window. Without this the card keeps the rectangle the
		// lane gave it and the newly-relevant rows go behind a scrollbar.
		//
		// **IT IS THE FOLD DEFECT IN A SECOND DOORWAY.** `onFoldChanged` exists because of an
		// earlier defect — collapsing an audio effect hid its UI but left the card's size unchanged —
		// where a fold hid the controls and repainted the card at the size it
		// already had. A gated control appearing is the same event, and `FxLane::resized`'s own
		// comment lists what reaches it (*"a card folding, a device being removed, or the lane
		// itself being resized"*) without this among them.
		//
		// **THE HEIGHT IS THE TRIGGER, NOT THE GATE MOVE**, and that is what keeps it cheap. A gate
		// that hides one control and reveals another of the same height has not changed what the
		// lane must do, and `update()` already returns true every frame for a meter — so re-placing
		// on the repaint rather than on the extent would have the lane re-laying itself out
		// continuously.
		const float wanted = openHeight();
		if (wanted != lastExtent_) {
			lastExtent_ = wanted;
			if (onFoldChanged) {
				onFoldChanged();
			}
		}
		return true; // the rows moved, so the card genuinely needs repainting
	}
	return false;
}

void GenericCard::drawHeadExtras(visage::Canvas& canvas) {
	// **NO SURFACE, NO BORDER, NO NAME — `FxCard::draw` painted those.** What is left here is this
	// card's OWN head marks: the slot menu and the `+N`, which are the card's rather than the host's
	// and which `drawContent` could not draw because the body is clamped.
	drawSlotMenu(canvas, kMuted);

}

void GenericCard::drawContent(visage::Canvas& canvas) {
	drawBodyExtras(canvas);
	// **THE SECTION HEADINGS, AT THE POSITIONS `layOut` PUT THEM.** Not recomputed here: a second
	// walk would be a second reading of the layout, which is the failure `faceRowsOf` exists to
	// prevent — and this one would be silent, drawing titles a few pixels off the rows they name.
	//
	// The rows are child frames and place themselves against the scroll; this text is drawn through
	// the card's own canvas, which `drawBody` has already offset, so the stored y needs no further
	// adjustment and gets none.
	for (const Heading& h : headings_) {
		canvas.setColor(kMuted);
		canvas.text(h.label, font(kKnobLabelSize), visage::Font::kLeft, bodyLeft() + kPad, h.y,
		            cellWidth(), kSectionHeight);
		// A HAIRLINE UNDER IT, so the group reads as a group rather than as a word that happens to
		// be above some knobs. Faint: it is a separator, not a border.
		canvas.setColor(withAlpha(kLine, 0.5f));
		canvas.rectangle(bodyLeft() + kPad, h.y + kSectionHeight - 5.0f, cellWidth(), 1.0f);
	}
	// THE BAR, OVER THE ROWS IT DESCRIBES — and only when there is something to scroll. The knobs
	// themselves are child frames and draw on their own; this card's body is the bar and nothing else.
	if (scrollable()) {
		// THE CARD'S OWN FRAME — the same rectangles `drawBody` clipped to, so the bar cannot end up
		// beside a body of a different size.
		drawBodyScrollBar(canvas, ownFrame().body, bodyScroll(), contentHeight() - kCardHeadHeight);
	}
}

void GenericCard::resized() {
	layOut();
}

void GenericCard::layOut() {
	const bool vertical = axis_ == LaneAxis::Vertical;
	const Grid grid = gridFor(vertical ? static_cast<float>(width()) : static_cast<float>(height()));

	// **ONE COLUMN IN EITHER LANE, AND NOTHING IS HIDDEN — THE BODY SCROLLS.**
	//
	// This used to compute what FITS and make the rest invisible, because a card had to hold its
	// controls within the room it was given. It no longer does: `contentHeight` asks for the whole
	// list and `FxCard::drawBody` offsets it, so every knob is placed and the ones past the bottom
	// edge are simply off-screen — which is what a scrolled list IS.
	//
	// A KNOB IS STILL HIDDEN WHEN THE CARD IS FOLDED, and that is a different question: a folded card
	// draws no body at all, and `Frame` keeps the bounds it was last given, so a knob left visible
	// would go on drawing over the head.
	// **A CONTROL ITS MODE DOES NOT USE IS NOT DRAWN AT ALL**: many value boxes are mode-specific and
	// are hidden where they are unused. SKLITTER in Burst uses a handful of
	// its controls; the rest were sitting there doing nothing, which is the honesty rule failing in
	// the direction of clutter rather than of lies. `ParamDesc::gateControl` is the device's own
	// statement about it and this card does not know what a mode is.
	//
	// **THE GAP CLOSES BEHIND THEM**, so the list is the live controls and not a colander — `placed`
	// counts what is actually laid out rather than using the loop index.
	//
	// **AND THE GAP THAT CLOSES IS NOW A RUNNING SUM RATHER THAN A COUNT TIMES A HEIGHT**, because a
	// box row is `ValueBox::kHeight` and a knob row is `Knob::rowHeight`. See `rowHeightFor`.
	size_t placed = 0;
	float top = kHeadHeight + kRowGap + bodyTopInset() - bodyScroll();

	// **A GRID WHEN THE DEVICE ASKS FOR ONE** (`Traits::faceColumns`). The same live walk, the gap
	// closing behind hidden cells in the same way, filled row by row; each row as tall as its tallest
	// cell, which is `extentOf`'s arithmetic, so what the lane reserved is what is drawn.
	if (const int columns = columnsFor(desc_); columns > 1) {
		std::vector<size_t> live;
		for (size_t i = 0; i < rows_.size(); ++i) {
			const bool on = relevant(paramIndices_[i]);
			rows_[i]->setVisible(!folded_ && on);
			if (on) {
				live.push_back(i);
			}
		}
		// **THE SAME GROUPING `extentOf` MEASURED.** `faceRowsOf` wants param indices; `live` holds
		// row indices, so it is translated once here rather than the grouping learning about rows.
		std::vector<size_t> liveParams;
		liveParams.reserve(live.size());
		for (const size_t i : live) {
			liveParams.push_back(paramIndices_[i]);
		}
		const float cell = grid.cell / static_cast<float>(columns);
		headings_.clear();
		for (const FaceRow& fr : faceRowsOf(desc_, liveParams)) {
			if (fr.heading != nullptr) {
				headings_.push_back({ fr.heading, top });
				top += kSectionHeight;
			}
			// **THE PAIR'S HEIGHT, AND THE SAME ARITHMETIC `extentOf` MEASURED WITH.** A stacked
			// control adds to the cell it shares rather than competing with it for the row.
			float rowHeight = 0.0f;
			float running = 0.0f;
			for (size_t k = fr.first; k < fr.first + fr.count; ++k) {
				const device::ParamDesc& p = desc_.params[liveParams[k]];
				const float h = cellHeightFor(p);
				running = (k > 0 && p.stacked) ? running + kStackGap + h : h;
				rowHeight = std::max(rowHeight, running);
			}

			size_t column = 0;
			float cellTop = top;
			float cellHeight = 0.0f;
			for (size_t k = fr.first; k < fr.first + fr.count; ++k) {
				const size_t i = live[k];
				const device::ParamDesc& p = desc_.params[liveParams[k]];
				const float h = cellHeightFor(p);
				const bool stacked = k > fr.first && p.stacked;
				if (auto* knob = dynamic_cast<Knob*>(rows_[i]); knob != nullptr) {
					knob->setLabelPlacement(Knob::LabelPlacement::Below);
				}
				if (!stacked) {
					// A NEW CELL. Work out how tall the whole cell is — this control plus anything
					// stacking onto it — so the pair can be centred as one thing rather than the
					// first of them being centred and the second hanging below the row.
					cellHeight = h;
					for (size_t j = k + 1; j < fr.first + fr.count; ++j) {
						const device::ParamDesc& q = desc_.params[liveParams[j]];
						if (!q.stacked) {
							break;
						}
						cellHeight += kStackGap + cellHeightFor(q);
					}
					// A CELL SHORTER THAN ITS ROW SITS IN THE MIDDLE OF IT — a picker beside two
					// dials.
					cellTop = top + 0.5f * (rowHeight - cellHeight);
					++column;
				}
				// Inset by a hair so neighbouring pads and boxes do not touch.
				const float x = bodyLeft() + kPad + static_cast<float>(column - 1) * cell;
				rows_[i]->setBounds(x + 2.0f, cellTop, std::max(0.0f, cell - 4.0f), h);
				cellTop += h + kStackGap;
			}
			top += rowHeight + kRowGap;
			placed += fr.count;
		}
		liveRows_ = placed;
		laidOutBottom_ = top + bodyScroll();
		return;
	}

	headings_.clear();
	const auto headingAt = [this](size_t paramIndex) -> const char* {
		for (const device::FaceSection& sec : desc_.traits.faceSections) {
			if (static_cast<size_t>(sec.firstControl) == paramIndex && sec.label != nullptr
			    && sec.label[0] != '\0') {
				return sec.label;
			}
		}
		return nullptr;
	};
	for (size_t i = 0; i < rows_.size(); ++i) {
		const bool live = relevant(paramIndices_[i]);
		rows_[i]->setVisible(!folded_ && live);
		if (!live) {
			continue;
		}
		// **THE HEADING ONLY REACHES A LIVE CONTROL**, which is what makes a fully gated-away section
		// disappear rather than leaving a title over nothing. `extentOf` measured it under the same
		// condition, from the same live set.
		if (const char* heading = headingAt(paramIndices_[i]); heading != nullptr) {
			headings_.push_back({ heading, top });
			top += kSectionHeight;
		}
		const float row = rowHeightFor(desc_.params[paramIndices_[i]]);
		// THE FACE AT THE LEFT WITH ITS NAME OVER ITS READING — `Knob::LabelPlacement::Right`. Told to
		// the knob rather than drawn around it, so the widget stays the only thing that knows what a
		// knob looks like.
		// A KNOB IS TOLD WHERE ITS LABEL GOES; a box carries its own caption and needs no telling.
		if (auto* knob = dynamic_cast<Knob*>(rows_[i]); knob != nullptr) {
			knob->setLabelPlacement(Knob::LabelPlacement::Right);
		}
		// **PLACED AGAINST THE SCROLL**, so a knob's frame is where its picture is. `drawBody` offsets
		// the CANVAS for the card's own drawing; a child frame is not drawn through that, so it has to
		// be positioned with the offset applied — otherwise the list would scroll and its knobs would
		// not.
		rows_[i]->setBounds(bodyLeft() + kPad, top, grid.cell, row);
		top += row + kRowGap;
		++placed;
	}
	liveRows_ = placed;
	// **WHERE THE PLACEMENT ACTUALLY ENDED**, with the scroll taken back out so it is a property of
	// the CONTENT rather than of where the body happens to be scrolled to. `contentHeight()` claims
	// to cover this; nothing checked that it did, and a known-bad that dropped the heading height
	// from `extentOf` alone stayed green.
	laidOutBottom_ = top + bodyScroll();
}

/// **WHETHER THE CONTROL AT `index` IS LIVE RIGHT NOW**, asked of the descriptor and of the gate's
/// current value. Reading the gate needs an instance, which is why this cannot be a free function on
/// the descriptor alone — `device::relevantFor` does the deciding and this supplies the number.
bool GenericCard::relevant(size_t index) const {
	// **FOLLOWED UP ITS CHAIN (backlog IO)** — a control gated on a hidden control is hidden. The panel's
	// `GenericExpanded::relevant` walks it the same way; the two surfaces must not disagree.
	size_t at = index;
	for (size_t hops = 0; hops < desc_.params.size() && at < desc_.params.size(); ++hops) {
		const device::ParamDesc& p = desc_.params[at];
		if (p.gateControl < 0) {
			return true;
		}
		const auto gate = static_cast<size_t>(p.gateControl);
		if (gate >= desc_.params.size()) {
			return true;
		}
		if (!device::relevantFor(p, host_.fieldAt(device_, gate))) {
			return false;
		}
		at = gate;
	}
	return true;
}

MissingDeviceCard::MissingDeviceCard(std::string_view tag) {
	// THE TAG'S OWN CHARACTERS, up to the first NUL — a record pads a short tag.
	for (const char c : tag.substr(0, 4)) {
		if (c == '\0') {
			break;
		}
		name_.push_back(c);
	}
	if (name_.empty()) {
		name_ = "?";
	}
	// THE PRESS IS THE CARD'S (see `FxCard`'s constructor): the head's fold and expand through
	// `noteCardPress`, and the slot menu — a missing device must be removable.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		noteCardPress(e);
		if (slotMenuAt(e.position)) {
			pressSlotMenu();
		}
	};
}

void MissingDeviceCard::draw(visage::Canvas& canvas) {
	// THE SHARED FRAME, WITH THE TAG WHERE A DESCRIPTOR'S NAME WOULD GO — the base's `draw` names the
	// card from its descriptor, and a missing device has none. Nothing else of the base's head applies:
	// no enable (nothing runs), no expand (nothing to show), no outputs.
	//
	// **`localBounds()` AND NOT `{0, 0, width(), height()}`, BECAUSE `height()` IS NOT THE FRAME'S
	// HERE.** This class declares `static constexpr float height()` — the height a host reserves
	// before the card exists — which SHADOWS `visage::Frame::height()`. So the copy of `FxCard::draw`
	// this line began as painted the card's border and head over `{0, 0, live width, 52}` whatever
	// rectangle the host had actually granted, while `drawBody` below clips through `ownFrame()`,
	// which is written in `FxCard`'s scope and therefore sees the real one. Two heights for one card.
	//
	// **AND THE HOST THAT MAKES IT VISIBLE IS THE RACK**, which grants `kCardUnitHeight` rather than
	// asking the card: a 216x336 frame painting a 52px spine down a 336px card, with 284px unframed.
	// The two strip hosts happen to grant exactly `laneHeight()`, which for this card IS the static —
	// so the bug was invisible in both of them, and the render case sizes the card at
	// `MissingDeviceCard::height()` and so could not see it either.
	//
	// `localBounds()` is declared in `Frame` and resolves there, which is the whole of the fix.
	const HeadLayout head = headLayout();
	drawCardFrame(canvas, localBounds(), head.name.c_str(), head.nameBox, 0, frameStyle(),
	              tintedHeadMarks(), headEdge());
	// THE MENU THE PRESS ABOVE ANSWERS — it was pressable and never drawn, so the one verb this card
	// exists to offer ("take it out") was an invisible button. The head sweep lists it as an occupant.
	drawSlotMenu(canvas, kMuted);
	if (folded()) {
		return;
	}
	drawBody(canvas, [&] {
		// MUTED, NOT DISABLED: this is a message a hand must read — secondary text — not a control that is
		// switched off. (Not a measured contrast fix: the one reading taken in `kDisabled` was of a build
		// whose text was clipped away, so it measured the border.)
		canvas.setColor(kMuted);
		// IN THE CARD'S OWN COORDINATES, FROM THE BODY'S ORIGIN — `drawBody` clips to the body and does
		// not move the origin, so text at y = 0 lands under the head and is clipped away entirely. The
		// first draft did exactly that and said nothing; the render case below the three hosts' caught it.
		canvas.text(kMessage, font(kKnobReadoutSize), visage::Font::kLeft, bodyLeft() + kModCardPadX,
		            bodyTopEdge(), std::max(0.0f, bodyWidth() - 2.0f * kModCardPadX), kBodyHeight);
	});
}

} // namespace feathers::ui
