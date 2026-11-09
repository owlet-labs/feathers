/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#pragma once

#include "backplane/device_host.h"
#include "device/device.h"
#include "device/registry.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/widget_registry.h"
#include "plugin/ui/zone_coord_cell.h"

#include <memory>
#include <string>
#include <vector>

namespace feathers::ui {

/// The panel's own body, as a rail. Defined below `GenericExpanded`, because its open width IS the
/// grid's width and that constant belongs to the grid.
class BodyRail;

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS A CARD'S FACE DOES NOT CARRY.                                                         //
//--------------------------------------------------------------------------------------------------//
//
// **EVERY CONTROL THE DEVICE DECLARES, DERIVED RATHER THAN LISTED.** The card draws
// `Traits::faceControls` of them; this draws them ALL — the rule since 2026-09-01: an expanded window
// is always a superset of the controls on the card. A panel holding only
// the leftovers is the tidier arithmetic and the wrong instrument, because it sends the player back
// and forth between two partial views.
//
// NEITHER SURFACE HOLDS A LIST, so neither can drift from the declaration and neither can lose a
// control — which is exactly the failure a hand-written pair produced on OWLTT, where fifteen of
// twenty-five ended up with nowhere to be touched.
//
// A DEVICE WHOSE CARD ALREADY SHOWS EVERYTHING HAS NOTHING TO EXPAND INTO, which is what
// `restCountFor` decides; a panel identical to the card would be a button that changes nothing.
class GenericExpanded : public ExpandedView {
public:
	GenericExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                const device::Descriptor& desc, Notifier* notifier = nullptr);
	/// OUT OF LINE because `body_` points at a type this header only forward-declares above.
	~GenericExpanded() override;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	[[nodiscard]] visage::Point minimumSize() const override;
	/// **THE GRID'S NATURAL SIZE PLUS ONE `kPad` EACH WAY, ON BOTH AXES** (backlog EI). The cells run
	/// from the top-left in fixed columns and rows and nothing stretches, so every pixel past the grid
	/// is empty surface. Follows `minimumSize()` — the live cells, the side column, a folded body — so a
	/// mode change that re-lays the grid moves the ceiling with it, on the announcement it already makes.
	[[nodiscard]] visage::Point maximumSize() const override;

	/// **WHETHER THE BODY IS FOLDED AWAY — the third foldable thing on a panel that has rails.**
	///
	/// The main body is collapsible in the window as well. "As well" is the whole specification: the body JOINS the rails rather than replacing anything, so a player
	/// can have the circle alone, the grid alone, or both, and each of the three is one chevron away
	/// from either state.
	///
	/// **THE STATE LIVES IN A `Rail` AND THE LAYOUT LIVES HERE, WHICH IS ONE OBJECT EACH RATHER THAN
	/// TWO AGREEING.** `BodyRail` holds the bool, draws the strip and answers the click; this panel
	/// reads the bool as one more term in the visibility line it already computes per cell
	/// (`resized()`). What it deliberately is NOT is a rail that owns the cells: the grid is laid out
	/// in the PANEL's coordinates by a base class five devices share, and re-parenting every cell to
	/// buy a bool would have moved `drawBodyExtras`, `cellsForTest` and every geometry case with it.
	[[nodiscard]] bool bodyCollapsed() const;

	/// TEST SEAM — null on a panel that never asked to be collapsible. The rail is what a case drives
	/// `setCollapsed` on, which is the road the chevron takes.
	[[nodiscard]] BodyRail* bodyRailForTest() const { return body_.get(); }

	/// Whether the control at this param index is live under its gate — see the .cpp.
	[[nodiscard]] bool relevant(size_t index) const;
	/// How many cells that leaves — the grid's and the side column's together.
	[[nodiscard]] size_t liveCells() const;
	/// **THE TWO HALVES OF THAT COUNT (backlog IG)**: the cells the grid lays out four across, and the ones
	/// `Traits::sideColumn` seats in the column beside it. A case asserting a mode's ROW count divides the
	/// first by `kPerRow` and takes the larger of that and the second.
	[[nodiscard]] size_t liveGridCells() const;
	[[nodiscard]] size_t liveSideCells() const;
	/// Whether this param index is one the side column seats.
	[[nodiscard]] bool inSideColumn(size_t index) const;
	/// **WHETHER THIS CONTROL IS A `modeCell`'S VALUE BOX** — gated on a Choice declared `modeCell`, and so
	/// seated inside that Choice's `ModeValueCell` rather than as a cell of its own (backlog IO addendum).
	[[nodiscard]] bool followsModeCell(size_t index) const;
	/// TEST SEAM: the zone-knob cells `modeCell` Choices became, in param order.
	[[nodiscard]] const std::vector<std::unique_ptr<ModeValueCell>>& modeCellsForTest() const {
		return modeCells_;
	}

	/// **EVERY CELL IN PARAM ORDER, PAIRED INDEX-FOR-INDEX WITH `paramIndicesForTest`.**
	///
	/// The three typed seams below (`knobsForTest`, `zoneCellsForTest`) each hold one KIND, so a claim
	/// about a cell's GEOMETRY — the thing this panel gets wrong when it gets anything wrong — cannot
	/// be made against them without first knowing which kind the param became, which is the very
	/// question at issue. This is the flat list the layout actually walks.
	[[nodiscard]] const std::vector<visage::Frame*>& cellsForTest() const { return cells_; }

	/// WHICH CONTROLS THIS PANEL HOLDS — all of them, per the superset rule. **Test-only and worth the
	/// five lines**: "the expanded view reaches what the card does not" is a claim about a SET, and a
	/// rendered panel cannot answer it. (Public beside its pair: it sat under `private:`, which the
	/// pairing above made unusable from any test.)
	[[nodiscard]] const std::vector<size_t>& paramIndicesForTest() const { return paramIndices_; }

protected:
	/// **HOW MUCH ROOM ABOVE THE CELLS THIS PANEL WANTS**, and what goes in it. The same two hooks
	/// `GenericCard` opens, for the same reason and with the same rule: a panel that needs more than a
	/// band above its grid is a panel that should not be deriving from this one.
	[[nodiscard]] virtual float bodyTopInset() const { return 0.0f; }
	virtual void drawBodyExtras(visage::Canvas&) {}

	/// **HOW MUCH OF THE PANEL'S WIDTH IS NOT THE BODY'S** — what a subclass has put down the right-hand
	/// edge. `SklitterExpanded` returns `railsWidth()`; a plain panel has nothing there.
	///
	/// It exists because the body rail's toggle sits at the body's INBOARD edge, which is where the
	/// body stops rather than where the panel does. The panel already needed this number to keep its
	/// band from running under the rails; before this it was computed twice, once in each place.
	[[nodiscard]] virtual float bodyRightInset() const { return 0.0f; }

	/// **THE COLUMN A COLLAPSIBLE BODY KEEPS CLEAR AT ITS INBOARD EDGE FOR ITS OWN CHEVRON**, and zero
	/// when there is no chevron. It is `Rail::kCollapsedWidth` rather than the toggle's own 20px so
	/// that an open body reserves the same strip it will become — the mark does not move when it is
	/// pressed.
	///
	/// **ONLY THE BAND ABOVE THE GRID PAYS IT.** The cells start at `kPad + bodyTopInset()`, below the
	/// toggle, so nothing in the grid is under it — which is a REQUIREMENT on any panel that turns this
	/// on rather than an observation, and is why `setBodyCollapsible` is a deliberate call and not the
	/// default. It is pinned by *"nothing the body draws sits under the chevron that folds it"*.
	[[nodiscard]] float bodyToggleReserve() const;

	/// **A SUBCLASS TURNS THE BODY'S CHEVRON ON, IN ITS OWN CONSTRUCTOR, AND THE DEFAULT IS OFF.**
	///
	/// A panel with nothing beside its grid has nothing to gain by folding it: the window would become
	/// a 34px strip holding one word, which is a control that takes something away and gives nothing
	/// back. The affordance is worth its pixels exactly where there is something ELSE to look at, so it
	/// is asked for rather than inherited — and a panel that does not ask is byte-for-byte the panel it
	/// was before this existed.
	///
	/// **THE PANEL MUST HAVE A `bodyTopInset()` OF AT LEAST `Rail::kPadTop * 0.5 + Rail::kToggleSize`**
	/// (26px) before it asks, or the chevron lands on the first row of cells — which, since the cells
	/// are LATER siblings and visage hit-tests topmost first, would leave the toggle drawn and
	/// unclickable. See `bodyToggleReserve`.
	/// **ONE-WAY, AND IT TOOK A GREEN KNOWN-BAD TO GET THERE.** This took a `bool` for an hour, with an
	/// `if (!on) setCollapsed(false)` guard against a panel that stopped being collapsible while folded
	/// and hid its every control behind a chevron it no longer drew. Nothing ever called it with
	/// `false`, so a mutation making it ignore its argument entirely ran all 151 cases GREEN — the
	/// branch was unreachable, and an unreachable guard is a comment that compiles. A call with no
	/// argument cannot be got wrong, and the state it guarded against cannot be reached.
	void setBodyCollapsible();

	/// **A CONTROL THE SUBCLASS SEATS SOMEWHERE OF ITS OWN, so the grid must not seat it as well.**
	///
	/// SKLITTER's four waypoint configurations belong AT their waypoints on the perform circle (since
	/// 2026-09-19, each macro-config control sits in its own corner), and a control drawn
	/// twice is two widgets over one field that are aligned only by both being redrawn often enough.
	///
	/// **IT IS A SECOND PREDICATE RATHER THAN A CASE INSIDE `relevant`, DELIBERATELY.** `relevant` means
	/// "live under its mode gate" — a control a mode does not read genuinely has nothing to show — and a cell
	/// that is elsewhere is not a cell that is off. Two questions in one predicate is how a later reader
	/// ends up believing a control is gated when it has merely moved. The LAYOUT treats them the same
	/// (hidden, no room, the gap closes behind it) and that is the only thing they share.
	///
	/// **IT IS A VIRTUAL CALLED FROM `resized`/`liveCells`/`minimumSize`, NEVER FROM THE CONSTRUCTOR** —
	/// the cells are built in the base constructor, where a virtual would not reach the subclass, so this
	/// deliberately does not decide what gets BUILT. Every control still gets its widget and stays in
	/// `paramIndicesForTest`; what moves is where it is put.
	[[nodiscard]] virtual bool drawnBySubclass(size_t) const { return false; }

	/// **SEAT ONE OF THIS PANEL'S OWN CELLS WHERE THE SUBCLASS WANTS IT** — for a control `drawnBySubclass`
	/// took out of the grid, when the subclass's place for it is just a different spot on this panel
	/// (Featherverb's conv drawer, below the grid). Shown, and sized exactly as the grid sizes it: the SAME
	/// widget, so no second copy over one field. Call it from `resized()` AFTER the base's, which hides every
	/// cell drawn by the subclass. False for a param this panel built no cell for. A mode cell's follower
	/// travels inside its mode cell and is never seated by itself.
	bool seatCell(size_t paramIndex, float x, float y);
	/// Hide it again (a closed drawer).
	void hideCell(size_t paramIndex);
	/// The cell a param became, or null — for a subclass enabling or disabling what it seated.
	[[nodiscard]] visage::Frame* cellOf(size_t paramIndex) const;

	/// The adapter for one control — protected so a subclass can seat a widget of its own over it.
	[[nodiscard]] DeviceControl& controlFor(size_t index) const { return control(index); }

	[[nodiscard]] const device::Descriptor& ownDescriptor() const noexcept { return desc_; }
	[[nodiscard]] DeviceInstanceId deviceId() const noexcept { return device_; }
	[[nodiscard]] backplane::DeviceHost& ownHost() const noexcept { return host_; }

protected:
	/// **PROTECTED SO A SUBCLASS CAN CHAIN TO IT**, the same rule — and the same wording — as
	/// `updateContent` below. A panel that holds a surface of its own with its own `ModState` (the
	/// chain in `SklitterExpanded`'s fx rail) must parent that state to the outer one AND still give
	/// this base's grid the outer state, or half its controls stop following an armed source. It was
	/// private, so an override could not call it, and the only way to add the first half was to lose
	/// the second.
	void modulationBound(ModState* mod, EditHistory* history) override;

private:
	[[nodiscard]] std::string title() const override;

	/// Every zoned coordinate the panel built a cell for, in param order.
	[[nodiscard]] const std::vector<std::unique_ptr<ZoneCoordCell>>& zoneCellsForTest() const {
		return zoneCells_;
	}

	/// Every knob the panel built — a zoned coordinate must not be among them.
	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

	/// HOW MANY CONTROLS FALL BEHIND THE FACE for a device. Zero means the card carries everything
	/// and no panel is needed.
	[[nodiscard]] static size_t restCountFor(const device::Descriptor& desc);

	// **PROTECTED, NOT PRIVATE: a subclass drawing a band above the grid measures against the same
	// padding the grid uses, or the two are aligned only by coincidence.**
protected:
	static constexpr float kPad = 12.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kCellWidth = 92.0f;
	/// **A CELL IS AS TALL AS THE KNOB IT HOLDS — DERIVED, BECAUSE THE LITERAL WAS 7px SHORT.**
	///
	/// It was `74.0f` against a knob column of `Knob::columnHeight(Knob::kDiameter)` = 81: the face is
	/// 52 here, not the card's 38, because nothing calls `setFaceDiameter` on a panel. So every knob
	/// on every generic panel had SEVEN PIXELS TAKEN OFF ITS 13px READOUT and printed a row of
	/// half-height glyphs — `RATE`'s "50%" cut through the middle, PHOTOGRAPHED on SKLITTER's panel.
	/// The symptom (some effect values and readouts clipped) surfaced during the UI round and was read
	/// as a card problem.
	///
	/// **THE FIX IS THE DERIVATION, NOT THE NUMBER 81.** A literal that happens to match today is the
	/// same bug waiting for the next change to `kDiameter`, `kGap`, `kLineGap` or `kReadoutHeight` —
	/// any of which moves the column and none of which would move a hand-written constant. The clip
	/// was silent because `Knob::draw` clamps to its own bounds: the widget did exactly what it was
	/// told, and being told the wrong size looks identical to having nothing to say.
	static constexpr float kCellHeight = Knob::columnHeight(Knob::kDiameter);

	/// The cell's height, which is the knob column's — `kCellHeight`, through the one derivation rather
	/// than a second number that agrees with it today.
	///
	/// **IT WAS A PAIR OF VIRTUALS FOR ONE DAY AND IS NOT ANY MORE (2026-09-19).** `cellFaceDiameter()`
	/// existed so SKLITTER's panel could COMPRESS its grid — shrink every knob's face — while one of its
	/// fold-down sections was open, on the premise that standard controls may also compress. Those
	/// sections are rails against the panel's right edge now, and a rail takes WIDTH: the decision is that
	/// **the window grows** instead (`sklitter_expanded.h`). Nothing else ever overrode the seam, so what is left is
	/// the constant every generic panel always drew at.
	[[nodiscard]] float cellHeight() const { return kCellHeight; }

public:
	/// **THE SAME ANSWER, PUBLIC, BECAUSE A CASE ASSERTING A CELL'S GEOMETRY MUST ASK RATHER THAN
	/// ASSUME.** Added 2026-09-19 with the panel that compressed: a case pinning a chip's height
	/// against `Knob::columnHeight(Knob::kDiameter)` went red reading 67 against 81 — correctly at the
	/// time, because the grid was giving up height for an open section. The compression is gone and the
	/// numbers agree again, and the seam is kept anyway: the case was right about the RULE (a pad-shaped
	/// chip is a full cell tall) and wrong to spell the number, which is `kCellHeight`'s own lesson
	/// arriving a third time. Ask the panel.
	[[nodiscard]] float cellHeightForTest() const { return cellHeight(); }

public:
	/// PUBLIC BESIDE `kGridWidth`, WHICH IS BUILT FROM IT: a case that asserts how many ROWS a mode lays
	/// out restates `ceil(live / kPerRow)` and must take the divisor from here rather than spell a 4.
	static constexpr size_t kPerRow = 4;

	/// **THE GRID AT ITS FULL WIDTH: four columns and the padding either side.** 2·12 + 4·92 = 392.
	///
	/// **IT IS PUBLIC BECAUSE IT IS NOW A SHARED SPECIFICATION RATHER THAN THIS CLASS'S ARITHMETIC.**
	/// Perform is the same width as the main panel. Two columns that are meant
	/// to line up must read their width from ONE place — `CircleRail::kWidth` and `BodyRail::kWidth`
	/// are both this — or they line up only until somebody changes a cell.
	///
	/// **AND IT IS THE GRID'S FULL WIDTH, NOT THE LIVE ONE.** `minimumSize()` below asks for
	/// `min(liveCells, kPerRow)` columns, because a panel in a mode with two controls should not
	/// reserve four; this constant is the four-column case, which is what a rail beside the grid is
	/// built to match. Every constant in it is a `static constexpr`, so nothing here asks a frame how
	/// wide it is — `rail.h` forbids that in an `expandedWidth()` and this is one.
	static constexpr float kGridWidth = 2.0f * kPad + static_cast<float>(kPerRow) * kCellWidth;

	/// **WHAT `Traits::sideColumn` ADDS TO THAT, WHEN A DEVICE DECLARES ONE — one cell's width (backlog
	/// IG).** Public beside `kGridWidth` because the perform rail's yield floor is derived from it: the room
	/// a SKLITTER panel with every rail open must leave is the grid AND its side column.
	static constexpr float kSideColumnWidth = kCellWidth;

private:
	backplane::DeviceHost& host_;
	const device::Descriptor& desc_;
	DeviceInstanceId device_ = kNoInstance;

	std::vector<std::unique_ptr<Knob>> knobs_;
	std::vector<std::unique_ptr<ValueBox>> boxes_;
	/// THE ZONED COORDINATES — see `GenericCard::zoneNamesFor`. They take no `setModulation`: the
	/// cell holds no mod state of its own and the adapter loop below reaches the control it wraps.
	/// The two-way switches — chips rather than boxes. See the constructor.
	std::vector<std::unique_ptr<SwitchChip>> chips_;
	std::vector<std::unique_ptr<ZoneCoordCell>> zoneCells_;
	std::vector<std::unique_ptr<ModeValueCell>> modeCells_;
	std::vector<visage::Frame*> cells_;
	std::vector<size_t> paramIndices_;

	/// **THE BODY'S RAIL, BUILT FIRST AND THEREFORE THE BOTTOM-MOST CHILD.** visage walks children in
	/// reverse to find what a press landed on (`Frame::frameAtPoint`), so every cell added after this
	/// is ABOVE it: a knob keeps its own clicks and the rail answers only where nothing else is, which
	/// is the band its chevron sits in. Built even when the panel is not collapsible — invisible, and
	/// therefore neither drawn nor hit-tested — because the order of `addChild` is fixed at
	/// construction and a virtual asked in a constructor does not reach the subclass.
	std::unique_ptr<BodyRail> body_;

	/// The gate's value and the live-cell count at the last layout — see `updateContent`.
	mutable double lastGate_ = -1.0e30;
	mutable size_t lastLive_ = 0;
	/// Each cell's relevance at the last layout, paired with `paramIndices_` — see `liveSetMoved`.
	mutable std::vector<uint8_t> lastLiveSet_;
	/// Whether any cell's relevance moved since the last call, recording the new set as it goes. A swap
	/// that keeps the count (SKLITTER's Swing -> Free) moves this and not `liveCells()`.
	[[nodiscard]] bool liveSetMoved() const;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;

	[[nodiscard]] DeviceControl& control(size_t index) const;
	/// One cell into its slot at (x, y) — the grid's flow and the side column share it, so a box, a chip
	/// and a knob are seated the same way in both.
	void placeCell(size_t n, float x, float y);

protected:
	/// **PROTECTED SO A SUBCLASS CAN CHAIN TO IT.** One that advances a picture of its own still owes
	/// the grid's answer: a mode change re-lays the cells out, and that is a repaint this panel asked
	/// for. `SklitterExpanded::updateContent` calls it first for exactly that reason.
	[[nodiscard]] bool updateContent() override;
};

//--------------------------------------------------------------------------------------------------//
// THE BODY, AS A RAIL.                                                                               //
//--------------------------------------------------------------------------------------------------//
//
// **IT IS A `Rail` AND IT HOLDS NOTHING**, which is the one shape decision in this change and is worth
// saying why rather than leaving to be read out of a class body.
//
// The three foldable things on SKLITTER's panel now are the body, the perform circle and the fx lanes,
// and `rail.h` already argues the case for a base over a bool: *"written per-panel, the collapsed form
// is the same forty lines three times, and the third copy is where they start to disagree about how
// wide a closed rail is."* This IS the third copy. Everything a closed body needs — the 34px strip, the
// upright label, the chevron that points the way the panel will travel, the announcement that the
// width moved — is `Rail`'s, already written and already photographed.
//
// **WHAT IT DOES NOT DO IS OWN THE GRID, AND THAT IS DELIBERATE.** A rail hides its CHILDREN when it
// closes, so the obvious reading of "make the body a rail" is to re-parent every cell into it. The
// cells are built by `GenericExpanded`'s constructor and laid out in the PANEL's coordinates by a base
// five devices share; moving them would have taken `drawBodyExtras`, `cellsForTest()` and every
// geometry case in `test_gui_widgets_devices` with it, to buy a bool the panel can simply read. So the
// division is: the RAIL owns the state, the chrome and the click; the PANEL owns the layout and asks
// the rail whether to lay anything out at all. That is one authority each, not two objects agreeing
// about what "open" means — the panel never decides it is open, and the rail never decides where a
// knob goes.
//
// **AND IT DRAWS NOTHING WHILE IT IS OPEN.** It covers the whole body, under everything; a ground or a
// hairline would be painted over the panel's own band and cells. Open, the only mark it makes is its
// chevron.
class BodyRail final : public Rail {
public:
	BodyRail() noexcept;

	void draw(visage::Canvas& canvas) override;

	/// **THE GRID'S OWN WIDTH, WHICH IS ALSO `CircleRail::kWidth`.** A constant, so `rail.h`'s rule is
	/// satisfied by construction: it is the same number before this rail has been laid out and after.
	///
	/// **NOBODY SUMS IT, AND THAT IS THE DIFFERENCE BETWEEN THIS RAIL AND THE OTHER FIVE.** The panel
	/// does not add `railWidth()` up to find its body — the body is what is LEFT once the subclass's
	/// rails have taken theirs (`bodyRightInset()`), so an open body is as wide as the window allows
	/// and this number is only what the panel asks for when nothing else has an opinion. It is stated
	/// anyway because `expandedWidth()` is the interface, and because a body narrower than this is a
	/// grid with a column missing.
	static constexpr float kWidth = GenericExpanded::kGridWidth;
	[[nodiscard]] float expandedWidth() const override { return kWidth; }
};

} // namespace feathers::ui
