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

#pragma once

#include "backplane/device_host.h"
#include "device/device.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/zone_coord_cell.h"

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// THE GENERIC CARD — the fallback for a device that declines to draw itself.
//
// device.h used to promise that a device declaring `hasCard = false` gets "a generic card built by
// walking this list — a knob per row, labelled by name, addressed by index". That promise was empty:
// the field was documented, every device set it true, and no code anywhere branched on it.
//
// THIS CLASS IS THE FALLBACK, BUT IT IS STILL NOT THE FLAG'S BRANCH — a correction to what this
// comment said before 2026-08-26, because "this class is the branch" reads as though building one of
// these is what `hasCard` finally does. It is not. `FxLane::ensureCard` reaches here when it cannot
// build a SPECIFIC card for the tag, and never consults the flag; `hasCard` is read by nothing to
// this day. device.h now says so at the end that would be edited. The structural test is the better
// one anyway: it also covers the unknown-tag case, which has no descriptor to carry a flag.
//
// IT IS THE FLOOR, NOT THE CEILING. A device that knows it has a response curve to show, an envelope
// to draw, or an MSEG to edit should write its own card — the filter's and HARM's are better than
// this because only the device knows what it has to show. This card exists so a browser can add a
// device nobody wrote a card for and still have every control reachable.
//
// ONE KNOB PER ROW, labelled by the descriptor's own name, addressed by the control's index in the
// descriptor's param list — never by a host identifier. Each knob is built over a DeviceControl that
// takes the generic (descriptor, index) constructor, so the card works for any device that publishes
// a descriptor, without knowing anything about the device type.

namespace feathers::ui {

// **NOT `final` ANY MORE, AND THE ONE SUBCLASS IS THE REASON.** SKLITTER needs a picture above its
// controls and nothing else about the list changes — the same scrolling, the same mode-aware hiding.
// (Its face became a GRID on 2026-09-16, and that is a DEVICE trait — `Traits::faceColumns` — read by
// this class, not something the subclass does, so it stays inside the two hooks below.) Deriving keeps ONE implementation of all of that; a hand-written card would
// be a second copy of the list machinery whose first divergence nobody would notice. The two hooks a
// subclass may use are `bodyTopInset` and `drawBodyExtras`, and they are the whole of the extension
// surface on purpose: a card that needs more than a band above its rows is a card that should not be
// deriving from this one.
class GenericCard : public FxCard {
public:
	/// Build a generic card for the device `id`, described by `desc` — addressed by its id, never a slot.
	GenericCard(backplane::DeviceHost& host, DeviceInstanceId id,
	            const device::Descriptor& desc, Notifier* notifier = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceInstanceId` is a `uint32_t` and a `size_t` would narrow
	/// into it without a word, so a site still passing its old address fails here instead.
	GenericCard(backplane::DeviceHost&, size_t, const device::Descriptor&, Notifier* = nullptr) = delete;

	/// THE BODY AND THE HEAD'S OWN MARKS — the frame, border and name belong to `FxCard::draw`. See
	/// its note on why the standardized header is the default rather than a convention each card is
	/// trusted to follow. (A folded generic card no longer prints "N controls": see `FxCard::draw`.)
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	bool updateContent() override;

	/// **THE COUNT IS THIS CARD'S EXPAND MARK.** `+106` says how much is behind the panel; the base's
	/// diagonal arrow says only that there is one, and two marks in one square is a smudge.
	// **THE SHARED MARK, LIKE EVERY OTHER CARD.** The expand-to-window button must use the same glyph
	// as every other card's. This was the ONLY card that opted out — `drawsOwnExpandMark` returned true and it drew `+N` in
	// accent text where the other six draw `FxCard::drawExpandMark`'s diagonal with an arrowhead at
	// each end, in muted grey.
	//
	// **AND THE `+N` IT EXISTED FOR CANNOT HAPPEN ANY MORE.** The count answered "this card is showing
	// 21 of 127 controls" back when the grid hid the overflow; the body scrolls now and `hiddenCount()`
	// is zero for every device — `test_rack_editor.cpp` asserts exactly that — so the branch always
	// fell through to a bare `>`. A different glyph, in a different colour, for the same verb.

	void resized() override;

	float laneHeightAt(float) const override { return folded() ? foldedHeight() : openHeight(); }

	/// **HOW TALL THIS CARD WILL BE FOR A DEVICE, BEFORE ONE IS BUILT.**
	///
	/// A host that reserves space in a lane has to know the extent before it constructs anything —
	/// `ModPanel::resized` clamps its scroll, which asks for a body height, before `ensureCard` has
	/// run. Every other card in the strip answers that with a static `height()`; a generic card's
	/// height depends on the DEVICE, so the descriptor is the argument.
	///
	/// **`openHeight()` IS DEFINED IN TERMS OF THIS RATHER THAN BESIDE IT.** Two spellings of one
	/// height is how a lane comes to reserve a different number from the one the card then draws at,
	/// and that gap is invisible until something overlaps.
	///
	/// **AND THE WIDTH IS AN ARGUMENT FOR THE SAME REASON `liveHeight` TAKES ONE**: the outputs foot is
	/// as many rows as the chips need at that width, and a device publishing seven of them needs more
	/// than one. The default is the modulator strip's card width, which is the only caller.
	static float heightFor(const device::Descriptor& desc, float cardWidth = kModCardWidth);

	/// How many knobs a generic card puts on that device: its params, less the one its head already
	/// draws as the enable square. Public because `heightFor` is, and because a caller sizing a lane
	/// may reasonably want the count rather than the pixels.
	static size_t knobCountFor(const device::Descriptor& desc);

	/// **HOW TALL THE ROW FOR ONE PARAM IS — AND IT IS NOT ONE NUMBER FOR EVERY ROW.**
	///
	/// The dropdowns were mis-sized. Every row on this card was `Knob::rowHeight`,
	/// which is the 38px a dial needs; a `ValueBox` needs 28 and drew its caption and its number in
	/// the top 26 of the 38 it was given. The remaining 10px was bordered empty space, which is why
	/// SKLITTER's Engage and Mode read as two blank slabs rather than as two pickers — PHOTOGRAPHED,
	/// not inferred: the dead band is visible under the word "Off".
	///
	/// **ASKED OF THE DESCRIPTOR, WITH THE SAME QUESTIONS THE CONSTRUCTOR ASKS TO CHOOSE THE WIDGET.**
	/// That is the whole reason this is one function and not a `dynamic_cast` in `layOut`: the
	/// constructor already decides zone-cell / box / knob from `zones`, `isDragWindow` and
	/// `Unit::Choice`, and a height computed from a different set of questions is a second opinion
	/// about what the row contains. It also has to work with NO INSTANCE, because `heightFor` sizes a
	/// lane before a card exists — a `dynamic_cast` road could not have answered there at all, and
	/// the lane would have reserved the old height for the new layout. That disagreement is the exact
	/// bug `heightFor`'s own note records (`height() == 364` against a claimed 267).
	static float rowHeightFor(const device::ParamDesc& p);

	/// HOW MANY CONTROLS THIS CARD PUTS ON ITS FACE — `Traits::faceControls`, with 0 meaning all and
	/// an over-large count clamped. Public because the expanded view draws the COMPLEMENT and the two
	/// must agree about where the line falls; one spelling, read twice.
	static size_t faceCountFor(const device::Descriptor& desc);

	/// **WHICH CONTROLS ARE ON THE FACE, IN THE ORDER THE CARD LAYS THEM OUT** — `Traits::faceOrder`
	/// when the device names them, else the first `faceCountFor` in declaration order. The enable is
	/// NOT removed here; the constructor and `heightFor` skip it, each saying why.
	static std::vector<size_t> faceIndicesFor(const device::Descriptor& desc);

	/// HOW MANY CELLS ACROSS — `Traits::faceColumns`, with 0 read as 1. One is the list.
	static int columnsFor(const device::Descriptor& desc);

	/// **HOW TALL ONE GRID CELL IS FOR ONE PARAM** — the grid's `rowHeightFor`, asked with the same
	/// questions for the same reason. A knob is a captioned column; a two-way switch is a knob-sized
	/// PAD, the whole cell (an enable is a full knob-sized piece); a picker
	/// keeps its own height and is centred in the row.
	static float cellHeightFor(const device::ParamDesc& p);

	/// **WHETHER THIS CONTROL IS DRAWN AS A KNOB-SIZED PAD** — a two-way switch on the FACE of a device
	/// whose face is a grid. Asked by BOTH surfaces: the card's grid sizes it through `cellHeightFor`,
	/// and the expanded panel asks this so the same control is the same widget there (Engage is
	/// enlarged in the window too). A switch the face does not carry — SKLITTER's
	/// Quantized — stays a chip on the panel.
	static bool isPad(const device::Descriptor& desc, size_t index);

	/// **THE BODY EXTENT OF THESE ROWS, LIST OR GRID, GAPS INCLUDED** — the one sum `heightFor` (over
	/// the declared face) and `liveRowsExtent` (over the live one) both read, so the lane's reservation
	/// and the card's drawing are one arithmetic.
	static float extentOf(const device::Descriptor& desc, const std::vector<size_t>& indices);

	/// **ONE ROW OF THE FACE**, as both the height arithmetic and the placement see it. `first` and
	/// `count` index into the LIVE list handed to `faceRowsOf`, not into the descriptor.
	struct FaceRow {
		const char* heading = nullptr; ///< a section opens above this row, or nullptr
		size_t first = 0;
		size_t count = 0;
	};

	/// **THE FACE, GROUPED INTO ROWS — the ONE place that decides where rows break and where headings
	/// go.** `extentOf` measures this and `layOut` places it, so the lane's reserved height and the
	/// drawn card cannot disagree. They already did once: `heightFor` reserved `ceil(knobs/3)` rows
	/// while `layOut` drew one knob per row, and a lane reserved a third of what it then drew. Two
	/// readings of one layout is how that happens, so there is only one reading.
	static std::vector<FaceRow> faceRowsOf(const device::Descriptor& desc,
	                                       const std::vector<size_t>& indices);

	/// The height a section heading costs. A row of small caps plus the air under it.
	static constexpr float kSectionHeight = 17.0f;

	/// The air between two controls sharing one cell — see `ParamDesc::stacked`. Smaller than
	/// `kRowGap`, because the pair is one thing and should read as one.
	static constexpr float kStackGap = 3.0f;

	/// THE ONE CARD THAT WORKS BOTH WAYS, because it is the only card whose layout is a GRID rather
	/// than a drawing. Every hand-written card places a plot or a plane at coordinates chosen for a wide
	/// card; this one just packs knob cells, and a grid transposes.
	float laneExtent(float cross) const override;

	/// HOW THE CELLS PACK, for a given cross extent — **the single source both `layOut` and
	/// `laneExtent` read.** `waypoint_card.h` names the failure this avoids: a card whose layout and
	/// whose reported extent disagree about the cell count draws outside its own bounds.
	struct Grid {
		int across = 1; ///< cells along the CROSS axis (columns when vertical, rows when horizontal)
		int along = 1;  ///< cells along the FLOW axis
		float cell = 0.0f; ///< the cross-axis size of one cell
	};
	Grid gridFor(float cross) const;

	/// HOW WIDE ONE LIST ROW HAS TO BE FOR THIS DEVICE.
	///
	/// **MEASURED FROM THE LONGEST NAME, NOT ASSUMED.** A fixed 92px was right for a captioned column
	/// and wrong the moment the caption moved BESIDE the face: Filter's controls are named "Filter
	/// Enabled", "Filter Cutoff", "Filter Keytrack", and every one of them was clipped to "FILTER
	/// EN...". A generic card exists to name a device's controls, so a width that cannot show a name
	/// defeats it.
	float cellWidth() const;

	/// HOW TALL THE WHOLE LIST WANTS TO BE — see `FxCard::contentHeight`. A one-column list of 21
	/// controls will not fit one unit of cross by any arrangement, so this is what the body scrolls
	/// against.
	float contentHeight() const override;

	/// HOW MANY CONTROLS THIS CARD IS NOT SHOWING — 0 when it shows them all.
	///
	/// THE CARD MUST SAY SO. A card silently drawing 21 of a device's 127 controls is the honesty rule's
	/// failure case: the surface would look complete and be missing 106 things, with nothing on screen
	/// admitting it. This is what the head's "+N" is drawn from and what makes the expand verb mean
	/// something specific rather than "there might be more".
	size_t hiddenCount() const;
	void setModulation(ModState* state, EditHistory* history) override;

	/// WHICH DEVICE THIS CARD IS THE FACE OF — the one card that can answer without naming a type.
	///
	/// FilterCard and HarmCard return a specific `kDescriptor` because each is written for one
	/// device. This card is written for ALL of them and already holds the descriptor it was built
	/// over, so the answer is the member rather than a constant.
	///
	/// BEHAVIOUR-NEUTRAL TODAY AND DELIBERATELY LANDED EARLY. Nothing this card draws asks: the only
	/// reader of `descriptor()` is `FxCard::enableControl`, and this card never calls it. It matters
	/// AFTER the head unification, when the LANE draws one head for every device and derives the
	/// name and the enable square by asking. **Without this line every device using the generic card
	/// — OWLTT among them — would silently lose both**, and it would look like a bug in the shared
	/// head rather than a missing override here.
	///
	/// Flagged by `harm-card` while scoping stage 4, and landed by the integrator rather than left in
	/// a plan: it is the only part of that stage payable in advance, precisely because it changes
	/// nothing until the rest arrives.
	const device::Descriptor* descriptor() const override { return &desc_; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	//--------//
	// Layout //
	//--------//

	static constexpr float kHeadHeight = 24.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kPad = 10.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	/// THREE ACROSS WHEN VERTICAL — and this constant IS the vertical assumption, which is worth saying
	/// out loud: a card that fills its width with exactly three columns is a card designed for a lane
	/// that gives it the whole width. Horizontally the count is not chosen, it is DERIVED from how many
	/// rows fit the height it was given.
	static constexpr int kRowCells = 3;

	/// A CELL'S WIDTH WHEN THE CARD IS CHOOSING ITS OWN — the horizontal case only.
	///
	/// VERTICALLY THERE IS NO SUCH CONSTANT because the cell width is DERIVED: `inner / kRowCells`,
	/// three columns sharing whatever width the lane gave. Horizontally the width is the answer rather
	/// than the question, so the cell has to have a size of its own. This is the asymmetry between the
	/// two directions, and it is the only one.
	/// THE NARROWEST A LIST ROW MAY BE. The real width is measured from the device's own longest
	/// control name — see `cellWidth()` — and this is the floor for a device whose names are short.
	static constexpr float kCellWidth = 92.0f;

	/// THE MOST CELLS THIS CARD PLACES ALONG THE FLOW AXIS. Everything past it goes to the expanded view.
	///
	/// MEASURED, AND THE REASON IS NOT CLIPPING. A horizontal card cannot clip — it grows sideways, so
	/// every knob always lands inside it. What it does instead, at a 720x560 window: 28 knobs makes a
	/// 664px card, and 127 knobs makes a 2964px one, which is 4.1x the whole window for ONE device.
	/// **A card with no error state can still be unusable**, and no assertion about geometry will ever
	/// say so, which is why this is a chosen number rather than a derived one.
	///
	/// THREE IS CHOSEN SO THE COMMON CASE IS UNTOUCHED. Three columns is 296px, which is exactly what
	/// the 11- and 12-knob devices already measured; with a 430px lane that is 7 rows, so 21 controls
	/// are shown. Every device in the catalogue up to 21 knobs is unaffected, and only the two large
	/// ones truncate. A cap that changed every card would be a redesign wearing a bound's clothes.
	///
	/// VERTICAL IS NOT CAPPED, deliberately: that lane scrolls, a tall card is reachable, and this is
	/// Feathers' existing behaviour. The cap exists because a horizontal card competes for the window's
	/// WIDTH with every other card in the chain — a scroll does not make that better, it hides it.
	static constexpr int kMaxFlowCells = 3;

	/// Show or hide this card's own children — see `FxCard::foldedChanged`.
	void foldedChanged() override;

	/// Where the head rectangle is, for hit-testing the fold gesture.
	visage::Bounds headBounds() const;

	/// THE EXPAND AFFORDANCE IN THE HEAD, or an empty rectangle when there is nothing to expand.
	///
	/// EMPTY IS A REAL ANSWER AND IT IS THE HONESTY RULE: a control is enabled iff it does something.
	/// A card showing all of its device's controls has no more to show, so it draws no button — the
	/// same shape `HarmCard::expandBounds` uses, and the reason `hasExpandedView` is a conjunction
	/// rather than a flag.

	/// **THE NAMED REGIONS OF A ZONED COORDINATE, IN THE WIDGET'S OWN TYPE.**
	///
	/// Declared beside the CONTROL in the device and carried on the descriptor, so a standalone
	/// device keeps its names and no UI-side map has to agree with it. Static and public because
	/// `GenericExpanded` builds the same cells over the same descriptor: two copies of this
	/// conversion is two places for a device to be drawn with somebody else's names.
	///
	/// A DEVICE MAY DECLARE FEWER NAMES THAN IT HAS ZONES, OR NONE AT ALL. The unnamed regions still
	/// exist and still count — SKLITTER declares eight of each, all empty, because the zone names
	/// mean nothing in most of its modes — so the list is grown to
	/// `zones` rather than trusted to be that long.
	static std::vector<ZoneSelect::ZoneName> zoneNamesFor(const device::ParamDesc& p);

	/// How tall the card is with all its knobs showing.
	float openHeight() const;
	static constexpr float foldedHeight() { return kHeadHeight; }

	//-----------//
	// For tests //
	//-----------//

	/// Every knob on the card, in param order — the card's own answer to "what can a hand reach".
	/// **WHERE `layOut` PUT EACH SECTION HEADING**, for `drawContent` to paint. Computed once by the
	/// layout and read by the paint, rather than derived twice — see `faceRowsOf`.
	struct Heading {
		const char* label = nullptr;
		float y = 0.0f;
	};

	const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

	/// TEST SEAMS for the two non-knob row kinds. **Which WIDGET a control became is the claim** —
	/// `Off`/`On` is a chip and any other named choice is a picker — and it is not recoverable from
	/// the rendered card: a chip and a box are both a bordered rectangle with a word in it, which is
	/// the whole reason the old shape went unnoticed.
	const std::vector<std::unique_ptr<ValueBox>>& boxesForTest() const { return boxes_; }
	const std::vector<std::unique_ptr<SwitchChip>>& chipsForTest() const { return chips_; }

	/// THE SECTION HEADINGS THIS CARD WILL PAINT, at the positions `layOut` computed. Empty when the
	/// device declares none, and a section whose controls are all gated away is ABSENT rather than
	/// present-and-empty — which is the claim, and one no screenshot can make without reading text.
	const std::vector<Heading>& headingsForTest() const { return headings_; }

	/// **WHERE `layOut` ACTUALLY FINISHED PLACING**, scroll removed. The lane reserves from
	/// `heightFor`/`contentHeight`, which measure the same layout through `extentOf` — two readings
	/// of one thing, and they have disagreed before: `heightFor` once reserved `ceil(knobs/3)` rows
	/// while `layOut` drew one per row. Exposing the placed bottom is what lets a case assert the
	/// two agree instead of trusting that they must.
	[[nodiscard]] float laidOutBottomForTest() const { return laidOutBottom_; }

	/// One control, for a venue that wants to ask what a knob would ask.
	const DeviceControl& controlForTest(size_t index) { return control(index); }

	/// The param index each knob corresponds to, in the same order as knobsForTest.
	const std::vector<size_t>& paramIndicesForTest() const { return paramIndices_; }

	/// The param index of each KNOB — see `knobParamIndices_` for why this is not the list above.
	const std::vector<size_t>& knobParamIndicesForTest() const { return knobParamIndices_; }

	/// **EVERY ROW, WHATEVER KIND OF WIDGET IT IS.** A case counting knobs alone sees too few of
	/// SKLITTER's rows in Burst, because most of them are `ValueBox` choices and one is a coordinate
	/// cell — which reads exactly like a card that hid too much. This is the list the card lays out.
	const std::vector<visage::Frame*>& rowsForTest() const { return rows_; }

	/// Every zoned coordinate the card built a cell for, in param order.
	const std::vector<std::unique_ptr<ZoneCoordCell>>& zoneCellsForTest() const { return zoneCells_; }

private:
protected:
	/// **HOW MUCH ROOM ABOVE THE ROWS THIS CARD WANTS**, in the body. Zero for a generic card; a
	/// subclass drawing a band returns its height and the list starts below it.
	[[nodiscard]] virtual float bodyTopInset() const { return 0.0f; }

	/// DRAW WHATEVER SITS IN THAT ROOM. Called from `drawContent`, in body coordinates, with the
	/// scroll already applied by `drawBody` exactly as it is for the rows.
	virtual void drawBodyExtras(visage::Canvas&) {}

	/// **THE INSTANCE A SUBCLASS DRAWS FROM.** `descriptor()` and `deviceHost()` already exist on this
	/// class returning POINTERS, so these two are spelled differently rather than overloaded — an
	/// accessor pair that differs only in returning a reference is a coin toss at every call site.
	[[nodiscard]] const device::Descriptor& ownDescriptor() const noexcept { return desc_; }
	[[nodiscard]] DeviceInstanceId deviceId() const noexcept { return device_; }
	[[nodiscard]] backplane::DeviceHost& ownHost() const noexcept { return host_; }

private:
	void layOut();
	/// Whether the control at this param index is live under its gate's current value.
	[[nodiscard]] bool relevant(size_t index) const;
	/// This instance's own height: the rows its mode actually uses, plus the foot its device's outputs
	/// need. See the .cpp.
	///
	/// **`cardWidth` IS WHAT THE FOOT IS MEASURED AGAINST**, because how many ROWS of output chips fit
	/// is a question about the width — and a lane asking `laneExtent(cross)` is asking about a width
	/// the card has not been given yet. Answering from `width()` would read the last layout's number,
	/// which is zero on the first.
	[[nodiscard]] float liveHeight(float cardWidth) const;
	/// **THE LIVE ROWS PLUS THEIR GAPS, AS ONE SUM** — every measurement of this list's body flows
	/// through here, so `liveHeight`, `laneExtent` and `layOut` cannot arrive at different totals.
	/// They already could not while all rows were one height; with per-row heights the arithmetic is
	/// no longer a multiplication anyone can retype correctly by eye.
	[[nodiscard]] float liveRowsExtent() const;
	/// Re-lay out when the gate control has moved since the last paint. See the .cpp.
	[[nodiscard]] bool relayoutIfGateMoved();
	DeviceControl& control(size_t index);

	/// THE ENABLE SQUARE'S CONTROL, for FxCard::enablePressAt.
	device::Modulatable* enableModulatable() override {
		return hasEnable() ? &control(enableControl()) : nullptr;
	}


	backplane::DeviceHost& host_;
	const device::Descriptor& desc_;
	// NO STORED NOTIFIER. Every sibling card keeps one to announce its enable toggle; this card has
	// no toggle to announce — `enableControl()` is never consulted here — so the pointer was
	// constructed, stored and never read. Clang says so with -Wunused-private-field and macOS builds
	// -Werror. The constructor still TAKES one, so every card is built the same way; re-add the
	// member the day this card grows something worth saying.
	DeviceInstanceId device_ = kNoInstance;

	std::vector<std::unique_ptr<DeviceControl>> controls_;

	/// HELD SO A CONTROL BUILT LATER GETS IT TOO. `control()` is lazy — a knob's control is made the
	/// first time it is asked for, which can be after `setModulation` has already run.
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	/// **THE CAPTIONS THIS CARD DRAWS, WHICH ARE NOT THE NAMES THE DESCRIPTOR DECLARES** — see
	/// `captionFor`. Owned here because `Knob` stores a bare `const char*` and does not copy, so a
	/// caption computed into a temporary would dangle the moment the row was built.
	///
	/// **A `deque` AND NOT A `vector`, and that is the whole reason this member is commented.** A
	/// vector reallocates as it grows and every `const char*` handed out before the growth becomes a
	/// dangling pointer into freed storage — which does not fault, reads as the right string for a
	/// while, and is exactly the class `scripts/sanitize-gate.sh` exists to catch. `deque` guarantees
	/// element addresses survive `push_back`.
	std::deque<std::string> captions_;

	std::vector<Heading> headings_;
	float laidOutBottom_ = 0.0f;

	std::vector<std::unique_ptr<Knob>> knobs_;
	/// **A CHOICE CONTROL IS A DROPDOWN, NOT A DIAL** — the collapse rule among them. `40506c9` had
	/// already settled the general form: a value box that selects among several values is a dropdown,
	/// not "drag over the box to select". `ValueBox` decides from the DESCRIPTOR alone, so this card opts no
	/// control in and can forget none.
	std::vector<std::unique_ptr<ValueBox>> boxes_;
	/// THE ZONED COORDINATES, which are neither knobs nor plain value boxes: an infinite line with
	/// named regions, drawn by the widget every hand-written card in this project was migrated onto.
	/// The two-way switches — see the constructor. Held apart from `boxes_` because they are a
	/// different widget, not a configured one.
	std::vector<std::unique_ptr<SwitchChip>> chips_;
	std::vector<std::unique_ptr<ZoneCoordCell>> zoneCells_;
	/// **EVERY CONTROL IN DECLARATION ORDER, whichever widget it became** — the list the layout walks.
	/// Two vectors laid out independently would be two orders that must agree, which this file has no
	/// reason to invent.
	std::vector<visage::Frame*> rows_;
	std::vector<size_t> paramIndices_;

	/// HOW MANY ROWS WERE ACTUALLY PLACED at the last layout — the list's real length once the
	/// controls their mode does not use are left out. `contentHeight` and `openHeight` measure this,
	/// or the card would reserve room for rows it does not draw.
	mutable size_t liveRows_ = 0;
	/// The gate's value at the last layout — the only thing that can change which rows exist.
	double lastGate_ = -1.0e30;
	/// Each row's relevance at the last layout, paired with `paramIndices_` — a row gated on a control other
	/// than the first gate moves this and not `lastGate_` (see `relayoutIfGateMoved`).
	std::vector<uint8_t> lastLiveSet_;
	/// THE HEIGHT THIS CARD LAST ASKED FOR. Compared rather than recomputed either side of a gate
	/// move, because `openHeight()` reads the LIVE relevance and so already answers the new height
	/// by the time the move is noticed — there is no "before" left to take.
	float lastExtent_ = -1.0f;
	/// **THE PARAM INDEX OF EACH KNOB, which is NOT `paramIndices_` indexed the same way.** That list is
	/// every control in declaration order; `knobs_` skips the ones that became dropdowns, so the two
	/// stopped lining up the moment a Choice control got a `ValueBox`. Keeping a knob-only list is
	/// cheaper than making every reader remember the difference — and a reader who forgot would label a
	/// knob with its neighbour's name.
	std::vector<size_t> knobParamIndices_;
};

/// **A DEVICE THIS BUILD DOES NOT HAVE — "device missing".** By design, a patch naming a device
/// the catalogue cannot find (a newer build's, or one that comes as its own file and is not on this
/// machine) KEEPS it, as a placeholder carrying its settings, with a "device missing" message. The
/// record keeps it and saves it back out; the engine runs nothing there; this is its face.
///
/// ONE CARD FOR EVERY HOST — the fx lane, the modulator strip and the standalone rack — because a
/// missing device is the same fact in each, and three placeholders (there were: "Unknown device — no
/// editor yet", a "Device" head over "no editor yet", and nothing at all) said three different things.
///
/// THE HEAD NAMES IT BY ITS TAG, which is the only name a missing device has: its display name lives in
/// the device that is not here. The slot menu works, because the one thing a hand most needs to do with
/// a missing device is take it out.
class MissingDeviceCard final : public FxCard {
public:
	/// `tag` is the record's four characters, NUL-padded or not.
	explicit MissingDeviceCard(std::string_view tag);

	void draw(visage::Canvas& canvas) override;
	float laneHeightAt(float) const override { return folded() ? kCardHeadHeight : height(); }
	/// OPEN, HEAD AND BODY — what a host reserves before the card exists (the strip's `bodyHeightOf`).
	static constexpr float height() { return kCardHeadHeight + kBodyHeight; }

	/// WHAT THE HEAD SAYS — the tag — and what the body says, for a case.
	[[nodiscard]] const std::string& name() const noexcept { return name_; }
	/// THE TAG IS THE NAME — there is no descriptor to ask — laid out like every other card's.
	std::string headName() const override { return name_; }
	static constexpr const char* kMessage = "device missing";
	static constexpr float kBodyHeight = 28.0f;

private:
	std::string name_;
};

} // namespace feathers::ui
