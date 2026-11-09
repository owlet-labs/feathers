/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#include "plugin/ui/generic_expanded.h"

#include "plugin/ui/gamma_box.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/theme.h"

#include <algorithm>

namespace feathers::ui {

namespace {
/// **THE CARD'S CAPTION RULE, ON THE PANEL TOO** — the declared `shortName` when there is one, else the
/// host's name. The panel used `name` everywhere but its mode cells, so a device whose host names carry
/// its own name ("Featherverb Room", for the DAW's automation list) printed that prefix on every knob
/// and the captions ran into each other. Nothing else declared a `shortName` outside a mode cell, so no
/// other panel moves.
const char* captionOf(const device::ParamDesc& p) {
	if (p.shortName != nullptr && p.shortName[0] != '\0') {
		return p.shortName;
	}
	return p.name == nullptr ? "" : p.name;
}
} // namespace

//--------------------------------------------------------------------------------------------------//
// THE BODY, AS A RAIL.                                                                               //
//--------------------------------------------------------------------------------------------------//

BodyRail::BodyRail() noexcept : Rail("CONTROLS", Side::Left) {
	// **`Side::Left`, WHICH IS WHERE THE BODY IS AND WHAT DECIDES WHERE THE BUTTON GOES.** `Rail` puts
	// a left-hand rail's toggle at its far end — against the stage rather than against the window edge
	// — so the body's chevron sits at the body's right, next to PERFORM's, and the two mark the seam
	// between the two columns instead of being at opposite ends of the panel.
	//
	// **AND IT OPENS BY DEFAULT**, for the reason the circle does and the placeholder does not: the
	// grid is what the panel IS. A device panel that arrived folded would be an expand button that
	// expands into a strip.
	//
	// **THE TOGGLE IS THE WHOLE OF ITS VERB.** Everything else inside the body is a cell that answers
	// for itself, and this rail is under all of them (see `GenericExpanded`'s constructor), so a press
	// anywhere else reaches it only because nothing else wanted it.
	onMouseDown() = [this](const visage::MouseEvent& e) {
		if (toggleHit(e.position)) {
			toggleCollapsed();
		}
	};
	// **NO `addChild` HERE, EVER.** The grid belongs to the panel — see the note above this class for
	// why that is the design rather than a shortcut — so `Rail::collapsedChanged`, which hides a rail's
	// children, has nothing to hide and the panel's own visibility line does the work.
}

void BodyRail::draw(visage::Canvas& canvas) {
	if (!collapsed()) {
		// **OPEN, IT IS A PANE OF GLASS OVER THE BODY WITH ONE MARK ON IT.** No ground and no hairline:
		// this frame covers the whole body and is drawn BEFORE the cells, so a ground would be a second
		// surface under the panel's own and a hairline would be a rule down the middle of nothing.
		drawRailChrome(canvas);
		return;
	}
	// CLOSED IS A WHOLE DRAWING — the same one `CircleRail` and `FxLanesRail` make, with the hairline on
	// the INBOARD edge, which for a left-hand rail is the right one. The rule separates the strip from
	// what it sits beside, not from the window.
	canvas.setColor(kSurfaceRaised);
	canvas.rectangle(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
	canvas.setColor(kLine);
	canvas.fill(static_cast<float>(width()) - 1.0f, 0.0f, 1.0f, static_cast<float>(height()));
	drawRailChrome(canvas);
}

size_t GenericExpanded::restCountFor(const device::Descriptor& desc) {
	// HOW MANY THE CARD'S FACE LEAVES OFF — which is not what this panel DRAWS (it draws everything,
	// per the superset rule) but is what decides whether a panel is worth offering at all. A card
	// already showing every control has nothing to expand into.
	const size_t face = GenericCard::faceCountFor(desc);
	return desc.params.size() > face ? desc.params.size() - face : 0;
}

DeviceControl& GenericExpanded::control(size_t index) const {
	if (index >= controls_.size()) {
		controls_.resize(index + 1);
	}
	if (controls_[index] == nullptr) {
		controls_[index] = std::make_unique<DeviceControl>(host_, device_, desc_, index, mod_, history_);
	}
	return *controls_[index];
}

GenericExpanded::GenericExpanded(backplane::DeviceHost& host, WidgetRegistry& registry,
                                 DeviceInstanceId id, const device::Descriptor& desc,
                                 Notifier* notifier)
    : host_(host), desc_(desc), device_(id) {
	(void)registry;
	// **THE BODY'S RAIL FIRST, BEFORE ONE CELL EXISTS.** Child order is fixed here and cannot be
	// changed later — visage has no reorder — and the order is what keeps a knob's press its own: the
	// rail is the bottom-most child, so `Frame::frameAtPoint`, walking in reverse, reaches it only
	// where nothing else is. Hidden until a subclass asks for it (`setBodyCollapsible`), which a
	// virtual could not be asked here because the subclass does not exist yet.
	body_ = std::make_unique<BodyRail>();
	body_->setVisible(false);
	addChild(body_.get());
	// **TOLD, NOT ASSUMED — `rail.h`'s own rule, and the same three lines `SklitterExpanded` gives its
	// two rails.** The body's width is one term in an arithmetic that belongs to whoever owns the
	// columns; here that is this panel, and the HOST owns the rectangle the panel is drawn in, so both
	// have to be told. Without the last line a body that folded would be re-laid-out inside the old
	// window and the room would never come back.
	body_->onRailWidthChanged([this]() {
		resized();
		redraw();
		minimumSizeChanged();
	});
	// **EVERY CONTROL, NOT THE COMPLEMENT — decided 2026-09-01:** an expanded window is always a
	// superset of the controls on the card. A first draft drew only what
	// the face leaves off, which is the tidier arithmetic and the wrong instrument: a window that
	// hides what the card already shows makes the player go back and forth between two partial views.
	// `test_gui_widgets_devices.cpp`'s "an expanded window is a SUPERSET of its card's controls"
	// enforces it across every device that has both surfaces, and caught this.
	for (size_t i = 0; i < desc.params.size(); ++i) {
		// THE ENABLE NEVER APPEARS HERE EITHER. The head draws it as its square, and a second control
		// for one field is how a player turns a device off in one place and sees it on in the other.
		if (desc.traits.enableControl >= 0 && static_cast<size_t>(desc.traits.enableControl) == i) {
			continue;
		}
		const device::ParamDesc& p = desc.params[i];
		// **A CONTROL GATED ON A `modeCell` CHOICE IS THAT CELL'S VALUE BOX (backlog IO addendum)** — seated
		// below with its mode, not here.
		if (followsModeCell(i)) {
			continue;
		}
		// **A `modeCell` CHOICE IS ONE ZONE KNOB: its selector, and a box for each control gated on it** — so
		// Groove looks like a normal zone knob again. Each follower keeps its own `ValueBox` over
		// its own control — the digit drag, the device's print and the modulation display are that control's
		// — and is listed in `cells_`/`paramIndices_` like any cell, so every set-based claim about this
		// panel (superset, reachability) still sees it; the layout leaves its visibility to the cell.
		if (p.modeCell) {
			std::vector<ZoneSelect::ZoneName> names;
			for (const char* n : p.names) {
				names.push_back({ n == nullptr ? "" : n, {} });
			}
			const char* caption = p.shortName != nullptr ? p.shortName : p.name;
			auto cell = std::make_unique<ModeValueCell>(control(i), caption == nullptr ? "" : caption,
			                                            std::move(names),
			                                            [this](size_t index) { return relevant(index); });
			addChild(cell.get());
			cells_.push_back(cell.get());
			paramIndices_.push_back(i);
			for (size_t j = 0; j < desc.params.size(); ++j) {
				if (!followsModeCell(j) || static_cast<size_t>(desc.params[j].gateControl) != i) {
					continue;
				}
				auto box = std::make_unique<ValueBox>(control(j), "");
				if (desc.text != nullptr && desc.text->format != nullptr) {
					const DeviceControl* adapter = &control(j);
					box->setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
				}
				ValueBox& seated = cell->addFollower(j, std::move(box));
				cells_.push_back(&seated);
				paramIndices_.push_back(j);
			}
			modeCells_.push_back(std::move(cell));
			continue;
		}
		// **THE SAME CELL THE CARD BUILDS, FOR THE SAME REASON.** A zoned coordinate is one unbounded
		// number with named regions on it, and the panel that drew it as a dial was the pre-migration
		// widget surviving in the one place nobody had a hand-written card to migrate. The superset
		// rule is about WHICH controls appear; it says nothing about drawing one of them differently
		// here, and a player who expands a card to reach a control should not find a different
		// instrument under the same name.
		if (p.zones > 0) {
			auto coord = std::make_unique<ZoneCoordCell>(control(i), captionOf(p),
			                                            GenericCard::zoneNamesFor(p),
			                                            ZoneSelect::kCoordinatePerLap);
			// **THE BOX PRINTS THROUGH THE DEVICE'S HOOK WHEN IT HAS ONE (backlog IG).** A `ValueBox`
			// prints the declared unit, which for a zoned coordinate is the raw number — right for an
			// unbounded lap counter, and meaningless for SKLITTER's Groove, whose number is "which zone
			// and how far into it". A hook that declines a control hands back the unit's own print, so
			// every coordinate that has nothing to say reads exactly what it read.
			if (desc.text != nullptr && desc.text->format != nullptr) {
				const DeviceControl* adapter = &control(i);
				coord->box().setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
			}
			addChild(coord.get());
			cells_.push_back(coord.get());
			zoneCells_.push_back(std::move(coord));
			paramIndices_.push_back(i);
			continue;
		}
		// THE SAME SEATING THE CARD GIVES IT — see `device::isDragWindow`.
		// A DIGIT BOX IS SEATED BELOW EVEN WHEN ITS RANGE IS A DRAG WINDOW (backlog IO, Free Phase): a
		// `GammaBox`'s fixed sixteen-lap span would outrank the grabbed digit.
		if (device::isDragWindow(p) && !p.digitBox) {
			auto box = std::make_unique<GammaBox>(control(i), captionOf(p));
			addChild(box.get());
			cells_.push_back(box.get());
			boxes_.push_back(std::move(box));
			paramIndices_.push_back(i);
			continue;
		}
		// **A TWO-WAY CHOICE IS A CHIP HERE TOO — and the panel is the second road, which this device has
		// now taught me to check every time.** `GenericCard` makes the same call for the same reason; a
		// fix landing on one of the two surfaces is half a fix by default, and SKLITTER has produced that
		// shape four times (density, the hard cut, the repaint, and the mode re-arm in backlog EQ).
		if (p.unit == device::Unit::Choice && p.names.size() == 2u) {
			auto chip = std::make_unique<SwitchChip>(control(i), captionOf(p));
			addChild(chip.get());
			cells_.push_back(chip.get());
			chips_.push_back(std::move(chip));
		}
		// **A DIGIT BOX — `ParamDesc::digitBox`, backlog IO** — seated as the card seats it, and printing
		// through the device's hook as a knob would.
		else if (p.digitBox || (p.unit == device::Unit::Choice && !p.names.empty())) {
			auto box = std::make_unique<ValueBox>(control(i), captionOf(p));
			if (p.digitBox && desc.text != nullptr && desc.text->format != nullptr) {
				const DeviceControl* adapter = &control(i);
				box->setReadoutFormatter([adapter](double at) { return adapter->textAt(at); });
			}
			addChild(box.get());
			cells_.push_back(box.get());
			boxes_.push_back(std::move(box));
		}
		else {
			auto knob = std::make_unique<Knob>(control(i), captionOf(p), notifier);
			addChild(knob.get());
			cells_.push_back(knob.get());
			knobs_.push_back(std::move(knob));
		}
		paramIndices_.push_back(i);
	}
}

GenericExpanded::~GenericExpanded() = default;

bool GenericExpanded::bodyCollapsed() const {
	return body_ != nullptr && body_->isVisible() && body_->collapsed();
}

float GenericExpanded::bodyToggleReserve() const {
	// NOTHING TO RESERVE WHEN THE BODY IS THE WHOLE STRIP — closed, the rail IS that column.
	return body_ != nullptr && body_->isVisible() && !body_->collapsed() ? Rail::kCollapsedWidth : 0.0f;
}

void GenericExpanded::setBodyCollapsible() {
	if (body_ == nullptr) {
		return;
	}
	// **VISIBILITY IS THE WHOLE OF THE OPT-IN.** visage skips an invisible child in `frameAtPoint` and
	// in the draw walk, so a rail nobody turned on is neither drawn nor pressable — and `bodyCollapsed()`
	// asks for BOTH, so a stray `setCollapsed(true)` on an untouched rail folds nothing.
	body_->setVisible(true);
}

visage::Point GenericExpanded::minimumSize() const {
	// **FOLDED, THE BODY ASKS FOR ITS STRIP AND NOTHING ELSE**, which is what hands the room back: the
	// panel is sized from this on every layout pass (`ExpandedOverlay::panelSizeOf`), so the grid's
	// four columns leave the arithmetic the instant they leave the screen.
	//
	// **AND THE HEIGHT IS THE STRIP'S OWN FLOOR, BECAUSE NOTHING ELSE MAY BE ASKING FOR ANY.** With the
	// body folded AND every rail folded — a legal state, and the one this line exists for — the tallest
	// thing on the panel is an upright word. A panel that went on claiming the hidden grid's height
	// would be asserting a need it does not have; one that claimed nothing would clip all three labels.
	if (bodyCollapsed()) {
		return { Rail::kCollapsedWidth, Rail::kCollapsedMinimumHeight };
	}
	// THE LIVE ONES, so a panel in Burst is a panel the size of Burst's controls.
	//
	// **AND THE SIDE COLUMN IS ONE MORE COLUMN, AS TALL AS ITS OWN LIVE CELLS** (backlog IG). It sits at
	// the grid's full width whatever the grid holds — a column that moved left when the grid thinned
	// would be the row-major shuffle it exists to avoid — and a column with nothing live takes no room.
	const auto cells = static_cast<float>(liveGridCells());
	const auto side = static_cast<float>(liveSideCells());
	const float across = side > 0.0f ? static_cast<float>(kPerRow) + 1.0f
	                                 : std::min(cells, static_cast<float>(kPerRow));
	const float rows = std::max(std::ceil(cells / static_cast<float>(kPerRow)), side);
	return { kPad * 2.0f + std::max(1.0f, across) * kCellWidth,
		     kPad * 2.0f + bodyTopInset() + std::max(1.0f, rows) * (cellHeight() + kRowGap) };
}

visage::Point GenericExpanded::maximumSize() const {
	const visage::Point natural = minimumSize();
	return { natural.x + kPad, natural.y + kPad };
}

std::string GenericExpanded::title() const {
	return desc_.traits.name != nullptr ? std::string(desc_.traits.name) : std::string("Device");
}

/// **THE SAME GATE THE CARD USES** — `ParamDesc::gateControl`, read against the instance's own value.
/// A panel that went on showing what the card hides would be the two surfaces disagreeing about what
/// the device does, which is worse than either answer alone.
bool GenericExpanded::relevant(size_t index) const {
	// **A CONTROL GATED ON A HIDDEN CONTROL IS HIDDEN (backlog IO)** — the gate is followed up its chain.
	// SKLITTER's Groove Amount is live only where Groove Mode is not Straight, and Groove Mode only in the
	// slice modes; without the walk, Amount would show in Burst under a Mode that is itself hidden.
	// Bounded by the control count, so a cyclic declaration ends rather than recursing.
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

bool GenericExpanded::followsModeCell(size_t index) const {
	if (index >= desc_.params.size() || desc_.params[index].gateControl < 0 || desc_.params[index].ownCell) {
		return false;
	}
	const auto gate = static_cast<size_t>(desc_.params[index].gateControl);
	return gate < desc_.params.size() && desc_.params[gate].modeCell
	       && !(desc_.traits.enableControl >= 0 && static_cast<size_t>(desc_.traits.enableControl) == gate);
}

size_t GenericExpanded::liveCells() const {
	return liveGridCells() + liveSideCells();
}

bool GenericExpanded::inSideColumn(size_t index) const {
	for (const uint8_t p : desc_.traits.sideColumn) {
		if (p == index) {
			return true;
		}
	}
	return false;
}

size_t GenericExpanded::liveGridCells() const {
	size_t live = 0;
	for (const size_t index : paramIndices_) {
		// A CELL THE SUBCLASS SEATS ELSEWHERE IS NOT A CELL IN THIS GRID — see `drawnBySubclass`, which
		// is why the two conditions are separate and both are asked here, in `resized` and in
		// `minimumSize` (through this) so the three cannot disagree about how many cells there are.
		// A MODE CELL'S FOLLOWER IS INSIDE THAT CELL, so it takes no slot of its own.
		if (relevant(index) && !drawnBySubclass(index) && !inSideColumn(index) && !followsModeCell(index)) {
			++live;
		}
	}
	return live;
}

size_t GenericExpanded::liveSideCells() const {
	size_t live = 0;
	for (const size_t index : paramIndices_) {
		if (relevant(index) && !drawnBySubclass(index) && inSideColumn(index) && !followsModeCell(index)) {
			++live;
		}
	}
	return live;
}

void GenericExpanded::resized() {
	// **THE BODY'S RAIL, OVER THE WHOLE BODY OR OVER ITS STRIP.** Open, it spans what the subclass's
	// rails have not taken — so its chevron sits at the body's INBOARD edge and travels with it when
	// the window is dragged wider, rather than at `expandedWidth()`, which is only what the panel asks
	// for. Closed, it is the 34px column, and the grid below is not laid out at all.
	if (body_ != nullptr) {
		const float wide = body_->collapsed()
		                       ? Rail::kCollapsedWidth
		                       : std::max(0.0f, static_cast<float>(width()) - bodyRightInset());
		body_->setBounds(0.0f, 0.0f, wide, static_cast<float>(height()));
	}

	// **THE WIDGETS ARE TOLD THE FACE BEFORE THE CELLS ARE MEASURED AGAINST IT**, and it is one face for
	// every panel since the grid stopped compressing (see `cellHeight()`). Told rather than left at the
	// widget's own default, because `kCellHeight` is computed FROM this number: a cell measured against
	// a face its knob was never given is the clipped-readout defect that note records, arriving from the
	// other direction. A `ValueBox` and a `SwitchChip` have no face and are centred in the cell, below.
	const float face = Knob::kDiameter;
	for (const std::unique_ptr<Knob>& knob : knobs_) {
		knob->setFaceDiameter(face);
	}
	for (const std::unique_ptr<ZoneCoordCell>& zone : zoneCells_) {
		zone->setFaceDiameter(face);
	}
	// A MODE CELL SHOWS THE BOX OF WHICHEVER FOLLOWER IS LIVE — asked here, where every other cell's
	// visibility is decided, and reached on a mode change because the follower's relevance is in the live
	// set `updateContent` compares.
	for (const std::unique_ptr<ModeValueCell>& mode : modeCells_) {
		mode->setFaceDiameter(face);
		(void)mode->refresh();
	}

	// **A FOLDED BODY IS A BODY WITH NO LIVE CELLS, AND THAT IS ONE TERM RATHER THAN A SECOND ROAD.**
	// The visibility line below already decides what is on screen, per cell, from two questions; this
	// is a third, asked once. A separate early return that hid the cells would be a second place
	// deciding what is visible, and the two would disagree the first time either question changed.
	const bool folded = bodyCollapsed();

	float x = kPad;
	float y = kPad + bodyTopInset();
	size_t inRow = 0;
	for (size_t n = 0; n < cells_.size(); ++n) {
		visage::Frame* cell = cells_[n];
		// A MODE CELL'S FOLLOWER IS PLACED AND SHOWN BY THAT CELL (`ModeValueCell::refresh`).
		if (n < paramIndices_.size() && followsModeCell(paramIndices_[n])) {
			continue;
		}
		// A CELL ITS MODE DOES NOT USE IS HIDDEN AND TAKES NO ROOM — the gap closes behind it.
		const bool live = !folded
		                  && (n >= paramIndices_.size()
		                      || (relevant(paramIndices_[n]) && !drawnBySubclass(paramIndices_[n])));
		cell->setVisible(live);
		// THE SIDE COLUMN IS PLACED BELOW, IN ITS OWN ORDER; the grid's flow skips it (backlog IG).
		if (!live || (n < paramIndices_.size() && inSideColumn(paramIndices_[n]))) {
			continue;
		}
		placeCell(n, x, y);
		++inRow;
		if (inRow >= kPerRow) {
			inRow = 0;
			x = kPad;
			y += cellHeight() + kRowGap;
		}
		else {
			x += kCellWidth;
		}
	}

	// **THE SIDE COLUMN: top to bottom in the declared order, at the grid's full width** (backlog IG). A
	// gated cell closes its gap here too, but only within the column — nothing moves between the two.
	if (folded) {
		return;
	}
	float sideY = kPad + bodyTopInset();
	const float sideX = kPad + static_cast<float>(kPerRow) * kCellWidth;
	for (const uint8_t p : desc_.traits.sideColumn) {
		for (size_t n = 0; n < paramIndices_.size() && n < cells_.size(); ++n) {
			if (paramIndices_[n] != p || !cells_[n]->isVisible()) {
				continue;
			}
			placeCell(n, sideX, sideY);
			sideY += cellHeight() + kRowGap;
		}
	}
}

visage::Frame* GenericExpanded::cellOf(size_t paramIndex) const {
	for (size_t n = 0; n < paramIndices_.size() && n < cells_.size(); ++n) {
		if (paramIndices_[n] == paramIndex) {
			return cells_[n];
		}
	}
	return nullptr;
}

bool GenericExpanded::seatCell(size_t paramIndex, float x, float y) {
	for (size_t n = 0; n < paramIndices_.size() && n < cells_.size(); ++n) {
		if (paramIndices_[n] == paramIndex) {
			cells_[n]->setVisible(true);
			placeCell(n, x, y);
			return true;
		}
	}
	return false;
}

void GenericExpanded::hideCell(size_t paramIndex) {
	if (visage::Frame* cell = cellOf(paramIndex); cell != nullptr) {
		cell->setVisible(false);
	}
}

void GenericExpanded::placeCell(size_t n, float x, float y) {
	visage::Frame* cell = cells_[n];
	{
		// **A BOX IS NOT A KNOB-SHAPED HOLE** (the dropdowns were mis-sized). A
		// `ValueBox` draws a caption over a number in `ValueBox::kHeight` and was handed the whole
		// 81px cell, so SKLITTER's Engage and Mode were bordered rectangles with their content in the
		// top third and fifty-odd pixels of empty box under it — they read as two blank slabs rather
		// than as two pickers.
		//
		// **CENTRED IN THE CELL RATHER THAN SHRUNK TO THE TOP OF IT**, which is `GammaBox::cellBounds`
		// making exactly this call one surface over: a box seated beside knobs belongs on their line,
		// not hanging from the cell's ceiling. The GRID keeps its regular cell — a ragged row in a
		// four-column grid is a worse picture than a tall one — and only the widget inside it stops
		// pretending to be a dial.
		//
		// **AND THE CARD MAKES THE SAME CALL BY A DIFFERENT ROAD** (`GenericCard::rowHeightFor`),
		// because there the list is ONE column and a short row genuinely closes the gap. Two surfaces,
		// one decision, two correct arithmetics — which is the distinction this device has now paid
		// for three times by fixing only one of them.
		//
		// **AND THE HEIGHT IS THE BOX'S OWN ANSWER, NOT `kHeight`** — `GammaBox` makes itself compact
		// in its constructor, so gamma wants 15 where a choice wants 28, and a site that assumed one
		// of the two would have drawn the other in a box half full of ground. See
		// `ValueBox::naturalHeight`.
		if (auto* box = dynamic_cast<ValueBox*>(cell); box != nullptr) {
			const float h = std::min(box->naturalHeight(), cellHeight());
			cell->setBounds(x, y + (cellHeight() - h) * 0.5f, kCellWidth, h);
		}
		else if (dynamic_cast<SwitchChip*>(cell) != nullptr && n < paramIndices_.size()
		         && GenericCard::isPad(desc_, paramIndices_[n])) {
			// **THE CARD'S PAD IS A PAD HERE TOO** — the whole cell, as a knob is. See `GenericCard::isPad`.
			cell->setBounds(x + 2.0f, y, kCellWidth - 4.0f, cellHeight());
		}
		else if (dynamic_cast<SwitchChip*>(cell) != nullptr) {
			// A CHIP IS SHORTER STILL, and on the knobs' line for the same reason a box is.
			const float h = std::min(SwitchChip::kHeight, cellHeight());
			cell->setBounds(x, y + (cellHeight() - h) * 0.5f, kCellWidth, h);
		}
		else {
			cell->setBounds(x, y, kCellWidth, cellHeight());
		}
	}
}

void GenericExpanded::draw(visage::Canvas& canvas) {
	// **FOLDED, THE BODY DRAWS NOTHING OF ITS OWN EITHER.** Hiding the cells is not enough: whatever a
	// subclass paints above them — SKLITTER's chunk band — is drawn by the PANEL, which paints before
	// any child, so it would be a full-width picture under a 34px strip and out the other side of it.
	if (!bodyCollapsed()) {
		drawBodyExtras(canvas);
	}
	// NOTHING OF ITS OWN TO DRAW. The cells are children and paint themselves; a panel that filled a
	// background here would be painting over the overlay's, which already owns the surface.
	(void)canvas;
}

void GenericExpanded::modulationBound(ModState* mod, EditHistory* history) {
	mod_ = mod;
	history_ = history;

	// **THE WIDGETS, NOT ONLY THE ADAPTERS, AND FORGETTING THEM IS A PANEL THAT CANNOT BE ARMED.**
	// A first draft forwarded to `controls_` alone. Every knob in the panel was then a modulation
	// DESTINATION that nothing watched, so none of them would glow when a source was armed and none
	// could be dropped on — twelve of them, on SKLITTER. `test_gui_widgets_devices.cpp`'s
	// "every destination knob is lit and watched" caught it; `owltt_expanded.cpp` had already written
	// the same two loops for the same reason.
	for (auto& knob : knobs_) {
		knob->setModulation(mod, history);
	}
	// A `ValueBox` has no `setModulation`: a Choice is not a modulation destination, so there is
	// nothing to arm on one and nothing to watch.

	// AND EVERY ADAPTER, INCLUDING ANY NO WIDGET HOLDS: `DeviceControl::setAmount` and `display` both
	// read the adapter's own `mod_`, so one built before this ran would be the control that never
	// glows.
	for (auto& c : controls_) {
		if (c != nullptr) {
			c->setModulation(mod, history);
		}
	}
}

bool GenericExpanded::liveSetMoved() const {
	// IN PLACE, so a tick allocates nothing once the first has sized the record.
	if (lastLiveSet_.size() != paramIndices_.size()) {
		lastLiveSet_.assign(paramIndices_.size(), 2u); // neither live nor dead: the first tick always moves
	}
	bool moved = false;
	for (size_t n = 0; n < paramIndices_.size(); ++n) {
		const uint8_t now = relevant(paramIndices_[n]) ? 1u : 0u;
		if (now != lastLiveSet_[n]) {
			lastLiveSet_[n] = now;
			moved = true;
		}
	}
	return moved;
}

bool GenericExpanded::updateContent() {
	// **THE GATE IS POLLED HERE FOR THE SAME REASON THE CARD POLLS IT.** Changing Mode is an ordinary
	// write to one field and nothing tells this panel. `updateContent` is the per-tick hook the
	// overlay already calls, and reporting `true` is the honest answer when the layout DID change —
	// the note below is still right that a cell needs no refresh, and a re-layout is not a refresh.
	const size_t live = liveCells();
	double gate = 0.0;
	for (const size_t index : paramIndices_) {
		if (index < desc_.params.size() && desc_.params[index].gateControl >= 0) {
			gate = host_.fieldAt(device_, static_cast<size_t>(desc_.params[index].gateControl));
			break;
		}
	}
	// **AND THE LIVE SET, NOT ONLY ITS COUNT (backlog IO addendum).** The defect: sometimes the switch from Groove
	// Amount to Free did not happen — Free mode, but the Groove value shown. SKLITTER's Swing -> Free
	// hides Groove Amount and shows Free Phase: the count does not move, and the gate polled above is the
	// FIRST one found (Mode), not Groove Mode — so neither trigger fired, and the Amount cell stayed on screen
	// and under the hand, clamped at 1. Every cell's relevance is compared, which is the question the layout
	// answers; the count stays the trigger for re-fitting below, since only it moves `minimumSize()`.
	const bool setMoved = liveSetMoved();
	if (gate != lastGate_ || live != lastLive_ || setMoved) {
		const bool grewOrShrank = live != lastLive_;
		lastGate_ = gate;
		lastLive_ = live;
		resized();
		// **THE DELIVERY, NOT JUST THE REPORT.** `ExpandedOverlay` counts what this returns; it
		// repaints nothing. `busutil_card.cpp` has the long version of this lesson and the card learnt
		// it an hour ago — the panel was still reporting a re-layout and asking for nothing.
		redraw();
		// **AND THE HOST IS TOLD WHEN THE COUNT MOVED, BECAUSE THE COUNT IS THE WHOLE OF
		// `minimumSize()`.** The defect: some knobs were truncated in 'controls' at the default window
		// size (backlog HY). Five of SKLITTER's controls are mode-gated, so Burst lays out a few rows
		// and Grain six — and the panel is sized by its host from `minimumSize()` on the HOST's layout
		// pass, which nothing here asked for. The rails and the body fold each announce through
		// `minimumSizeChanged()` on their own road (`onRailWidthChanged`); the count changed the same
		// number and announced nothing, so a panel opened in a sparse mode kept that mode's height and the rows
		// a fuller mode added were laid out below its foot. MEASURED before this line, in the case that
		// carries the backlog letter: three knob cells with feet at 645 and 734 against a panel bottom
		// of 631. Announced on the COUNT and not on the gate alone, because a gate change that keeps
		// the count (Shuffle → Repeat) moves no dimension, and a host told to re-fit would re-fit to
		// the same rectangle.
		if (grewOrShrank) {
			minimumSizeChanged();
		}
		return true;
	}

	// **NOTHING TO PULL.** Every cell here is a `Knob`, a `ValueBox` or a `ZoneCoordCell` over a
	// `DeviceControl`, and those read the host when they draw rather than holding a copy — so there is
	// no cached picture to refresh, and reporting `true` would claim a repaint this panel did not
	// cause.
	return false;
}

} // namespace feathers::ui
