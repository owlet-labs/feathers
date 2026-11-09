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

#include "plugin/ui/chain_column.h"
#include "plugin/ui/generic_expanded.h"
#include "plugin/ui/perform_pad.h"
#include "plugin/ui/device_preset_provider.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/fx_lane.h"    // FxLane::kWidth — THE standard vertical column width (the mock)
#include "plugin/ui/fx_lane_rail.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/sklitter_card.h" // SklitterChunk, and the one decode both surfaces share
#include "plugin/ui/source_arm.h"    // the ONE widget that arms a source, and the chip-row walk
#include "plugin/ui/zone_coord_cell.h"

#include <algorithm>
#include <array>
#include <functional>
#include <memory>

// **THE BAR, AND THE SAME BAR RE-LAID.** The design sketch:
//
//     ┌────────────────────┐
//     │ ▓▒░▓░▒▓▒ buffer    │
//     │ ░▒▓ reordered ▓▒░  │
//     │ ● ● ● ● ● controls │
//     └────────────────────┘
//
// That first drew TWO strips: the captured bar in its own order above (a plain ramp, because position
// in the bar IS the colour) and the same chunks in the order they are PLAYED below.
//
// **THE UPPER STRIP IS GONE (backlog EY)**, to save room, because it was trivial: chunk `i` at position
// `i` coloured `i / count` states nothing the
// colour scale does not, and `sklitterRamp` walks hue through four centres, so a chunk's colour alone
// says where it came from. Its one extra fact — a mark at the chunk the playing step reads FROM — is
// the colour of the chunk under the head, which the PLAYED strip already draws.
//
// So the panel now draws the card's band at panel size. Both decode through `sklitterChunks`, so they
// cannot disagree about what is playing.

namespace feathers::ui {

//--------------------------------------------------------------------------------------------------//
// AND THE PERFORM CIRCLE, WHICH IS WHAT THE MACRO BECAME — design §5, built 2026-09-19.              //
//--------------------------------------------------------------------------------------------------//
//
// Once the eleven controls had landed on the generic grid, the design moved every macro config to the
// circle: each `Cfg` control sits in its own corner, there are no macro knobs (the pad alone controls
// them), and the pad exposes XY handles. So the section below is a disk with the four `Cfg` cells
// around it and a draggable puck in the middle.
//
// **THE CELLS MOVED FROM THE CARDINALS TO THE CORNERS, AND THE CAPTIONS WENT WITH THEM (2026-09-19).**
// The first arrangement was a 92x34 row cell at each of N, E, S and W, each captioned `Cfg N` … `Cfg W`.
// The revision drops the N/E/S/W labels as clutter, makes the phase/dropdown boxes square with two
// rows, and moves them to the circle's corners to stack more efficiently. All three parts of that are
// geometry and the third is what pays: the E and W cells sat
// OUTSIDE the disk on its centre line, so the rail was `2·pad + 2·cell + disk` wide. At the corners the
// disk shares its band with nothing, and the width collapsed to the two squares — 336 → 172.
//
// **AND ON THE SAME DAY IT WENT BACK TO 392, WHICH IS NOT A REVERSAL**: PERFORM is the same width as the
// main panel. The narrowing was the arrangement earning its width honestly; the widening
// is a decision about the COLUMN — the circle and the grid are two stacked-up things that should share an
// edge. What changed with it is the direction the geometry is derived in: the rail's width is now given
// and `kDisk` falls out of it, rather than the disk being given and the rail falling out. The cells
// stayed 72, because a square is sized by the words it prints.
//
// **IT IS THE PLAIN VERSION, AND THAT IS A DECISION RATHER THAN A STOPPING POINT.** The GPU work has
// landed, but richer visuals (flying sparks and the like) are deferred.
// Nothing here front-runs that: `PerformPad` draws a ring, four marks and a puck, and the cool version
// replaces its `draw` without moving a parameter, a weight or a hit test.
//
// **AND IT IS A `Rail`, WHICH IS THE FX LANE'S OWN CLASS RATHER THAN A SIBLING OF IT (2026-09-19).**
// The requirement is that they behave exactly like the fx lane's expansion in Feathers. The first
// attempt — a pair of fold-down sections that stacked vertically down the panel body under a
// full-width text header — did not: inline, vertical rather than horizontal, and no tray, just a text
// label. The class those sections were built on
// (`Tray`) argued at length that it could not be a `Rail` because *"`railWidth()` is what the EDITOR
// sums"*. **THAT ARGUMENT WAS FALSE.** `Rail` knows nothing about the editor: it announces a width
// change through `onRailWidthChanged` and the owner of the layout does the arithmetic. `gui.cpp` is
// one such owner (`railEdge += x->railWidth()`); this panel is another. So the fold-down class is
// deleted and both sections are rails against this panel's RIGHT edge, collapsing to the same 34px
// strip with the same upright label and the same chevron the POST-FX lane has.
//
// **AND THE WINDOW GROWS RATHER THAN THE GRID COMPRESSING** — the decision, chosen over the
// alternative it replaces. The first version made the knobs smaller while a section was open, on the
// earlier allowance that standard controls may compress; with the sections beside the grid rather than above
// it, the room a rail wants is WIDTH, and a panel takes width by being wider. `minimumSize()` includes
// both rails, `ExpandedOverlay::panelSizeOf` reads it on every layout pass, and a rail's own
// announcement is what asks for that pass — see `ExpandedView::onMinimumSizeChanged`, which is the
// mechanism that makes the growth happen rather than merely make it possible. **THE ROAD HAS FOUR
// JOINTS AND ONE OF THEM WAS MISSING**: rail → panel → `MountedFrame` (every panel in either host is
// presented through one, and it was opaque about this while being transparent about `minimumSize()`)
// → host. The case for this drives all four in a real editor.
//
// **WHAT CLOSING A RAIL MUST NOT DO IS HIDE A CONTROL.** With the circle closed the four `Cfg` cells go
// BACK to the grid rather than disappearing — `drawnBySubclass` is asked per-layout, so the same
// predicate that takes them out puts them back. A collapsed section that made four controls unreachable
// would be the honesty rule failing on a feature, which is what `GenericExpanded`'s superset rule exists
// to stop.
//
// **`Perf X`, `Perf Y` AND `Perf Depth` STAY ON THE GRID, and that is a choice worth naming.** The pad
// is the gesture; the three knobs are the numbers — typable, and, more to the point, ROUTABLE, because
// a modulation route is dropped onto a widget and the pad is not one. Reading "the pad exposes XY
// handles" as "the pad replaces them" would have taken the circle's only two modulation
// destinations off the panel.
/// **RAIL 1 — THE PERFORM CIRCLE.** The disk, its four waypoint marks, the puck, and the four `Cfg`
/// cells at the corners around it. It owns them: a rail that only positioned widgets somebody else held
/// would be two objects agreeing about what "open" means, and `Rail::collapsedChanged` hides CHILDREN.
///
/// **IT OPENS BY DEFAULT.** The circle IS the macro on this device now, so a panel that arrived closed
/// would hide the instrument's own gesture behind a chevron.
class CircleRail final : public Rail {
public:
	/// The four cells arrive built, because building one needs a descriptor and a `DeviceControl` and
	/// the panel is what has both. They arrive OWNED, because a rail that positioned widgets somebody
	/// else held would be two objects agreeing about a lifetime as well as about a state.
	CircleRail(device::Modulatable& perfX, device::Modulatable& perfY,
	           std::array<std::unique_ptr<ZoneCoordCell>, fx::sklitter::kCorners> corners);

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	/// **THE ANCHOR BUTTON — backlog HT**, at the right end of the heading row. `enabled` is asked at every draw and
	/// press (the honesty rule: lit iff a press would change something); `press` is the verb, which the panel owns
	/// because it needs the host, the device and the history, none of which a rail holds.
	void setAnchor(std::function<bool()> enabled, std::function<void()> press);
	[[nodiscard]] visage::Bounds anchorBounds() const;
	[[nodiscard]] bool anchorEnabled() const { return anchorEnabled_ && anchorEnabled_(); }
	static constexpr float kAnchorWidth = 52.0f;

	/// **THE LANE'S OWN FURNITURE, NOT NEW NUMBERS.** `FxLane`'s padding, heading row and gap, because
	/// this is the same object one rail over and two rails that disagreed about their padding would be
	/// visible side by side on the same panel.
	static constexpr float kPadX = 10.0f;
	static constexpr float kHeadingHeight = 16.0f;
	static constexpr float kGap = 10.0f;

	/// **AS WIDE AS THE GRID IT SITS BESIDE, WHICH IS A SPECIFICATION RATHER THAN AN ARITHMETIC**
	/// (2026-09-19: PERFORM is the same width as the main panel). Two columns meant to line up read
	/// their width from ONE place or they line up until somebody changes a cell, so this IS
	/// `GenericExpanded::kGridWidth` — 2·12 + 4·92 = **392** — rather than a number that agrees with it.
	///
	/// **IT WAS 172, DERIVED FROM THE CONTENT, AND THE DIRECTION OF THE DERIVATION HAS REVERSED.** The
	/// old comment said *"derived from what it holds, not chosen"*: the content was the wider of two
	/// squares side by side and the disk, and the rail was whatever that came to. It is now the other
	/// way round — the WIDTH is given and the disk is derived from it (`kDisk`) — which is the honest
	/// reading of the specification. What has not changed is that every term is a `static constexpr`: `rail.h`
	/// forbids an `expandedWidth()` that measures anything, because `gui.cpp` reads `railWidth()` while
	/// placing the rail, when every frame in the subtree is still zero. MEASURED on the arrangement
	/// before last: a known-bad returning `… + pad_->width()` answered 204 cold against 336 laid out.
	/// **AND IT IS THE ASK, NOT THE SIZE (2026-09-20).** When the window is enlarged, the whole perform
	/// circle scales up with it, as large as the player makes it. That is a statement about the room
	/// this rail is GIVEN, which is a different number from the room it asks for, and only the second
	/// one is forbidden to measure anything. `SklitterExpanded::resized` hands this rail every pixel the
	/// panel has beyond what the whole panel asked for, and the geometry below derives from `width()`
	/// and `height()` at layout time, which is what every frame in this editor does. So `expandedWidth()`
	/// stays this constant — the MINIMUM — and the instrument grows above it.
	static constexpr float kWidth = GenericExpanded::kGridWidth;
	[[nodiscard]] float expandedWidth() const override { return kWidth; }

	/// The band inside the padding — what the disk and the four squares share at the MINIMUM width. 372.
	static constexpr float kInner = kWidth - 2.0f * kPadX;

	/// **THE SQUARE'S SIDE, STATED HERE AND CHECKED IN A CASE — the arrangement `rail.h` forces.**
	///
	/// What the square has to hold is two rows: the coordinate's widest printed value, and the widest
	/// region name between two arrow cells. **Both of those are font measurements**, so neither can be
	/// written as arithmetic in a header, and `expandedWidth()` may not go and ask — `gui.cpp` reads
	/// `railWidth()` while placing the rail, when every frame under it is still zero.
	///
	/// So the requirement is computed where a font exists (`ZoneCoordCell::minimumSquareSide`) and this
	/// constant is asserted against it by *"a corner cell is SQUARE, captionless, and big enough for
	/// both of the rows it keeps"*. **MEASURED there, on this device, 2026-09-19: the requirement is
	/// 66.** Across: 14px of inset, and inside it the wider of the value row (38px for `-8388608` at
	/// the readout size) and the NAME row (52px — two 14px arrow cells with 24px between them for the
	/// widest of the eight weather names), so 14 + 52. Down: 8px of inset, the 15px box, a 2px line
	/// gap and the 13px selector — 38, which the width wins comfortably.
	///
	/// **72 RATHER THAN 66, AND THE SIX PIXELS ARE THE POINT.** At exactly 66 the widest region name
	/// fills its column edge to edge and touches both chevrons; the case would stay green and the cell
	/// would look wrong. Six pixels is three either side of the longest name this device has.
	static constexpr float kCellSide = 72.0f;

	/// **HOW FAR A CELL'S INNER CORNER IS FROM THE CENTRE, ON EACH AXIS — and it is what the extra 220
	/// pixels bought.** The four squares go to the corners of the band, hard against the padding, so
	/// the offset is simply the half-band less one square: 186 − 72 = **114**, against the 4px the old
	/// 172px rail could afford. The cells did NOT grow: a square is sized by the two rows it prints
	/// (`kCellSide`), and giving it room it has nothing to put in would be a bigger box round the same
	/// words.
	///
	/// **AT THE MINIMUM SIZE. The laid-out offsets are `layout()`'s `dx` and `dy`**, over the room the
	/// rail actually got. The cells still do NOT grow with it: a square is sized by the two rows it
	/// prints, so what a bigger room buys is spread, not bigger boxes.
	static constexpr float kCornerOffset = kInner * 0.5f - kCellSide;

	/// **THE AIR BETWEEN A CELL'S INNER CORNER AND THE DISK IT IS TUCKED AGAINST**, measured along the
	/// diagonal and against the radius the pad ANSWERS A PRESS FOR rather than the one it draws at.
	///
	/// **THE THING TO CLEAR IS THE GRAB REACH**, `disk().radius + PerformPad::grabSlack()`, which is
	/// half the pad's frame exactly because `rimInset()` and `grabSlack()` are the SAME number — both 6
	/// at 1:1 and both scaled by the same factor once the circle grows (2026-09-20) — a cell inside it
	/// is a control sharing pixels with the gesture behind it, which is worse than one merely drawn
	/// over a line. **AND CLEARING IT EXACTLY IS NOT ENOUGH**, which is `kCellSide`'s own lesson one
	/// line up: at tangency the corner touches, the case stays green, and the picture is wrong. Six
	/// puts the cell's corner 8px clear of the nearest waypoint MARK (the mark sits on the drawn ring,
	/// `kMarkRadius` = 4 across a 6px rim inset), which is the gap that reads.
	///
	/// **IT DOES NOT SCALE, AND THAT FOLLOWS FROM THE CELLS NOT SCALING.** This is air between a
	/// fixed-size square and a growing disk; six pixels beside a 72px box reads the same beside a 500px
	/// circle as beside a 310px one. The MARK does scale, which is why the "8px clear of the nearest
	/// mark" reading above is the 1:1 one — at size the mark grows into part of that eight.
	static constexpr float kCornerClear = 6.0f;

	/// √2 to eight figures. `std::sqrt` is not a constant expression, and this number has to be one:
	/// `expandedWidth()` is read before anything is laid out, so everything downstream of it is a
	/// compile-time sum.
	static constexpr float kRoot2 = 1.41421356f;

	/// **THE DISK, DERIVED FROM THE ROOM RATHER THAN CHOSEN — and this is where the extra width
	/// went.** It was 132 in a 172px rail; PERFORM became the same width as the main
	/// panel, and the circle is this device's main gesture, so the width buys disk.
	///
	/// The four squares are tucked at 45°, which is where their waypoints now are, so what limits the
	/// disk is the DIAGONAL: a cell's inner corner is `kCornerOffset·√2` from the centre and the grab
	/// reach is `kDisk/2`, leaving `kCornerClear` between them. That is **310** — 2.35× the disk this
	/// replaces, and 5.6× its area.
	///
	/// **THE TUCK IS WORTH HAVING NOW AND WAS NOT BEFORE, WHICH THE OLD NOTE PREDICTED**: *"tucking
	/// WOULD pay if the rail were wider — 44 more pixels across buys 10 back down."* It is 220 wider.
	/// At the old spread the corner had 4px of horizontal offset to work with and the nesting was worth
	/// 0.12px; at 114 the disk is 178px bigger than the two rows above and below it would otherwise
	/// allow.
	/// **AND SINCE 2026-09-20 IT IS A FLOOR RATHER THAN THE SIZE**: when the window is enlarged, the
	/// whole perform circle scales up with it. `diskSide()` is what the
	/// pad is laid out at; this is what it is at the smallest panel, and the arithmetic is the same one
	/// (`diskFor(kBlockHeight)`) so the two cannot say different things about the same block.
	static constexpr float kDisk = 2.0f * (kCornerOffset * kRoot2 - kCornerClear);

	/// **THE BLOCK IS A SQUARE, AND THAT IS THE COROLLARY OF PUTTING FOUR SQUARES AT FOUR CORNERS**: it
	/// is `kInner` across by construction and the same down, because the cells are the same distance
	/// out on both axes. 372 either way, with the disk centred in it — and, since the circle grows, a
	/// FLOOR on the room rather than the only answer.
	static constexpr float kBlockHeight = 2.0f * (kCornerOffset + kCellSide);

	//----------------------------------------------------------------------------------------------//
	// THE OUTPUT STRIP ALONG THE BOTTOM — SKLITTER's seven perform outputs, 2026-09-20.              //
	//----------------------------------------------------------------------------------------------//
	//
	// SKLITTER's mod outputs, previously only on the card, are duplicated onto the perform pane, along
	// its bottom edge.
	//
	// **DUPLICATE, NOT MOVE — AND TWO SURFACES CANNOT FIGHT OVER ONE ARM.** The card keeps its chips.
	// Both surfaces build a `SourceArm` over the SAME `device::Source` (`Source::output(device, n)`)
	// against the SAME `ModState` — `gui.cpp` hands `&impl_->mod` to the fx lane, to every expanded
	// view and to everything else — and `ModState` holds ONE focused source and ONE armed flag. So the
	// two chips are two views of one arm by construction: pressing either arms that source and BOTH
	// draw lit, which is what "duplicate" has to mean if the surfaces are not to disagree.
	//
	// **THE CHIPS ARE THE REAL WIDGET, NOT A DRAWING OF ONE.** `SourceArm` is the unified arm widget
	// and `outputChipBoundsFor` is the walk all three existing hosts already use; this rail is the
	// fourth. Painting chips that looked like the card's and armed nothing is precisely what the
	// honesty rule forbids, and `source_arm.h`'s own banner records that Feathers once did it.

	/// **HOW MANY ROWS OF CHIPS THE STRIP RESERVES AT THE RAIL'S DESIGN WIDTH — the FLOOR on the count,
	/// and the one compile-time figure `kOpenMinimumHeight` below is built from.**
	///
	/// **THIS USED TO BE THE WHOLE ANSWER, AND IT WAS TRUE ONLY AT 392.** Until 2026-09-22 the strip
	/// reserved two rows whatever width the rail had. The circle YIELDS width when both fx lanes are
	/// open and the window would clip (`SklitterExpanded::circleWidth`), down to `kYieldFloorWidth`, and
	/// at 266 (220 since backlog IG: four rows there, all seven placed) the same seven chips wrap onto MORE
	/// rows — which `layoutOutputChips` then HID, by its own
	/// card rule (*"an output that cannot be drawn inside its card is hidden, not drawn outside
	/// it"*). The defect: with every panel expanded, some of the mod outputs on PERFORM were truncated
	/// — `Perf Speed` first, then the Weights, a TAIL of the outputs gone.
	///
	/// **SO THE COUNT IS NOW COMPUTED FROM THE WIDTH THE RAIL ACTUALLY HAS** — `outputRowsFor` below
	/// asks the same walk that PLACES the chips (`outputChipRowsFor`, `layoutOutputChips` in counting
	/// mode), so a count and a placement cannot disagree about where a row breaks. The band grows by
	/// whole rows as the rail narrows and the CIRCLE gives up the height, which is the rule already
	/// on `outputStripBounds`: the strip is a fixture of the pane and the circle the thing that scales.
	/// The perform rail is NOT a card — it owns its height — so the card walk's hiding rule is left
	/// exactly where it is and simply never fires here, because the strip is always as tall as the
	/// rows the walk needs.
	///
	/// **WHY A FLOOR RATHER THAN THE BARE COUNT.** How many chips fit on a line is a FONT measurement
	/// (`SourceArm::widthFor` runs text through `trackedWidth`) and `kOpenMinimumHeight` has to be a
	/// compile-time sum, so the design width's count is STATED here and a case asks the font that it is
	/// exactly this (*"the seven perform outputs fit the rows the pane reserves, at its narrowest"*).
	/// Flooring the computed count at it means widening the rail never takes a row away: the picture
	/// at 392 and above is the one auditioned, and this change moves nothing there.
	static constexpr size_t kMinOutputRows = 2;
	/// One row's worth is the card's, so a chip here is the same furniture as a chip there.
	static constexpr float kMinOutputStripHeight =
	    static_cast<float>(kMinOutputRows) * kCardOutputStripHeight;
	/// The strip plus the air above it, at the design width — what `kOpenMinimumHeight` gains. **AT A
	/// NARROWER RAIL THE BAND IS TALLER THAN THIS AND THE DIFFERENCE COMES OUT OF THE CIRCLE'S CENTRING
	/// SLACK, NOT OUT OF THE DISK**: at every width where the count exceeds the floor the block is
	/// bound by the rail's WIDTH, and the height room it leaves is wider than the rows added. That is
	/// a measured claim, not a derived one — the case *"the output strip grows by whole rows as the
	/// circle yields"* sweeps every width from the floor to the design width and asserts it.
	static constexpr float kMinOutputBand = kGap + kMinOutputStripHeight;

	/// **THE ROWS THE STRIP RESERVES FOR A RAIL OF THIS WIDTH** — the placing walk in counting mode
	/// over the strip that width leaves (`railWidth - 2·kPadX`), never below `kMinOutputRows`.
	[[nodiscard]] static size_t outputRowsFor(float railWidth);
	/// Those rows in pixels; `outputStripBounds()` is exactly this tall.
	[[nodiscard]] static float outputStripHeightFor(float railWidth);
	/// The strip plus the air above it, for this width — what `downFor` takes out of the disk's
	/// room and `resized()` out of the room the disk is centred in. ONE function for both, or the
	/// circle sits low against the chips with every size assertion still green.
	[[nodiscard]] static float outputBandFor(float railWidth);

	//----------------------------------------------------------------------------------------------//
	// THE SAME GEOMETRY OVER THE ROOM THE RAIL WAS ACTUALLY GIVEN.                                   //
	//----------------------------------------------------------------------------------------------//
	//
	// **THE DISTINCTION THAT MAKES THIS LEGAL IS ASK vs GIVEN.** `rail.h` forbids `expandedWidth()`
	// measuring a frame, because `gui.cpp` reads `railWidth()` *while placing the rail* and everything
	// under it is still zero. Nothing forbids `resized()` and `draw()` reading `width()` — that is what
	// every frame does, and it is the only thing that can know how big the window was dragged. So the
	// constants above are the ASK and the three functions below are the ANSWER.
	//
	// **THE DISK STAYS ROUND, AND THE ROOM AROUND IT NO LONGER HAS TO BE SQUARE (backlog IR, 2026-09-23).**
	// A disk in a rail that grew only across would either stop growing at the height or stop being a
	// circle; it stops growing — it is never wider than the SHORTER of the two rooms. What changed is
	// the cells. Until IR the rail cut a SQUARE block out of the shorter room, put a cell at each of its
	// corners and inset the disk from it, so a tall or wide rail left room beside the disk that nothing
	// used, room that would have let the circle be larger. So the boxes are laid out according to where
	// the free space is. See `layoutFor` for the rule.

	/// THE ROOM ACROSS — the band inside the padding, never below `kBlockFloor`.
	[[nodiscard]] static float acrossFor(float railWidth) {
		return std::max(kBlockFloor, railWidth - 2.0f * kPadX);
	}

	/// THE ROOM DOWN for a rail of this size — never below `kBlockFloor`, so a panel clipped short by
	/// its layer overflows at the bottom rather than collapsing.
	/// **AND THE OUTPUT STRIP COMES OUT OF `down` BEFORE THE DISK SEES IT (2026-09-20).** The seven
	/// perform outputs sit along the bottom of the pane, and a strip along the bottom is HEIGHT.
	/// Because the block is the SHORTER of the two rooms, a strip taken out of the height would shrink
	/// the disk at every size where height is the binding constraint — which collides with the other
	/// requirement, that the whole perform circle scales as large as the window allows.
	///
	/// **RESOLVED THE WAY THE RAILS ALREADY RESOLVE IT, WHICH IS THE ESTABLISHED PRECEDENT**: the strip ADDS to
	/// `kOpenMinimumHeight` rather than stealing from the disk. `SklitterExpanded::minimumSize` records
	/// the horizontal form of the same rule — *"a rail that opens makes the window wider ... the grid
	/// keeps its knobs at full size throughout"*, chosen over compressing the grid. So the panel gets
	/// taller by exactly the strip and the disk keeps `kBlockHeight` at the floor; subtracting here and
	/// adding there are the SAME number, which is what makes that true rather than nearly true.
	/// **AND THE `kBlockHeight` FLOOR IS ABOUT THE HEIGHT AXIS. ITS WIDTH HALF HAS NEVER RUN — MEASURED
	/// 2026-09-20, WRITTEN DOWN HERE RATHER THAN CHANGED.**
	///
	/// The floor says "when the room is less than designed, OVERFLOW rather than shrink", which
	/// preserves the pre-scaling fixed-size drawing for a panel its layer clipped short. That is a
	/// deliberate choice about a transient clip and it is what the line above is for.
	///
	/// **ON THE WIDTH AXIS IT WOULD BE WRONG, AND IT IS UNREACHABLE.** Pinned at `kBlockHeight` the
	/// block never shrinks, so a narrower rail does not shrink the disk — it slides off the left edge,
	/// because `cx` tracks the rail while the disk does not. MEASURED: rail 292 puts the disk at
	/// [−9.2, 301.2]; rail 266 at [−22.2, 288.2]. **But `across` has never once been below 372**:
	/// `expandedWidth()` answers `kWidth` and `SklitterExpanded::resized` hands this rail
	/// `circleWidth()` = `railWidth() + surplus`, which is `>= kWidth` by construction. Nothing can
	/// currently give it less.
	///
	/// **SO IT IS UNREACHABLE, NOT LATENT, AND THE DISTINCTION IS WHY NOTHING IS CHANGED HERE YET.** A
	/// fix to a branch no caller can enter is a change no case can exercise and no reader can verify.
	/// It becomes reachable the day the circle YIELDS width (shrinking the circle when both lanes are
	/// open, and only when the window would otherwise clip off the screen), and
	/// the floor moves in that commit, where a case can reach it.
	///
	/// **AND IT IS NOW REACHABLE, SO THE FLOOR MOVED — `kBlockFloor`, not `kBlockHeight`.** The circle
	/// YIELDS width when the panel cannot otherwise fit (`SklitterExpanded::circleWidth`), which is the
	/// first thing that ever hands this rail less than `kWidth`. Flooring at the DESIGN size would have
	/// made the yield a no-op that slid the disk off the left edge instead of shrinking it; flooring at
	/// the circle's own honest minimum makes it shrink and stop. Changed in the commit that made it
	/// reachable, with a case that reaches it, rather than shipped blind ahead of one.
	///
	/// **AND THE BAND IT TAKES OUT IS THE ONE THIS WIDTH NEEDS (2026-09-22), NOT THE DESIGN WIDTH'S.**
	/// `outputBandFor` — see `kMinOutputRows` for why it stopped being a constant.
	[[nodiscard]] static float downFor(float railWidth, float railHeight) {
		return std::max(kBlockFloor,
		                railHeight - 2.0f * kPadTop - kHeadingHeight - kGap - outputBandFor(railWidth));
	}

	/// **THE NARROWEST THIS RAIL MAY BE YIELDED TO — DERIVED FROM THE ROOM THE DEFAULT EDITOR LEAVES IT
	/// WITH EVERY PANEL OPEN (backlog IG, option (A), 2026-09-22).**
	///
	/// It WAS 266, chosen so the circle could give back *"everything the scaling work gave it, and not one
	/// pixel more"* (disk 132.25, the pre-scaling `kDisk`). Then SKLITTER's grid gained its side column
	/// (Groove, `Traits::sideColumn`) and grew 92px, and at 266 the all-open panel clipped the default
	/// editor by 46px. Between that clip and a smaller floor, the floor was chosen. So the
	/// number is no longer a statement about the disk — it is the width left over:
	///
	///     1240 (the editor)  − 2·12 (the overlay's panel pad)  − 484 (grid + side column)
	///                        − 2·256 (both fx lanes open)      = 220
	///
	/// The disk that leaves is `diskFor(kBlockFloor)`, 67.2px, and every other tenant of the rail was
	/// checked to still fit at it — the case *"backlog IG: at the yield floor every PERFORM tenant fits"*.
	///
	/// **A FLOOR RATHER THAN A PROMISE THAT IT NEVER BINDS.** When the deficit exceeds what the circle
	/// can yield, the circle stops here and the remainder falls through to the body exactly as it did
	/// before — the panel clips. At the default editor with every panel open it now binds exactly.
	static constexpr float kDesignEditorWidth = 1240.0f; ///< `Gui::kLogicalWidth`; a case pins the two equal
	static constexpr float kYieldFloorWidth =
	    kDesignEditorWidth - 2.0f * ExpandedOverlay::kPanelPad
	    - (GenericExpanded::kGridWidth + GenericExpanded::kSideColumnWidth) - 2.0f * FxLane::kWidth;
	static_assert(kYieldFloorWidth == 220.0f, "the derivation above is spelled out in the comment");
	/// The block that width leaves, and what `acrossFor` and `downFor` floor at. Derived, so the two cannot drift.
	static constexpr float kBlockFloor = kYieldFloorWidth - 2.0f * kPadX;
	/// A cell's inner corner from the centre, on each axis, for a SQUARE room of that side. This and
	/// `diskFor` below are the square case of `layoutFor`, kept because `kDisk` has to be a constant.
	[[nodiscard]] static constexpr float cornerOffsetFor(float blockSide) {
		return blockSide * 0.5f - kCellSide;
	}
	/// The disk that fits it — **the SMALLER of two bounds, and the second one was a real defect.**
	///
	/// The DIAGONAL bound is `kDisk`'s own arithmetic: a cell's inner corner is `offset·√2` from the
	/// centre and the grab reach is half the pad's frame, leaving `kCornerClear` between them.
	///
	/// **THE BLOCK BOUND IS THE ONE THE COMPILE-TIME VERSION NEVER NEEDED.** `offset·√2` grows FASTER
	/// than `offset`, so past `block ≈ 521` the diagonal answer is bigger than the block itself and the
	/// circle spills out of the band at its four cardinal points — which, since the block is what the
	/// rail's height allows, means a circle clipped top and bottom. MEASURED on the private display at
	/// a 1240px-wide panel: block 610, diagonal answer 647. It cannot happen at the floor (310 against
	/// 372), which is exactly why deriving a constant into a function needs a second look at the
	/// inequality that constant satisfied by luck.
	///
	/// Past that crossover the cells are simply further from the circle than `kCornerClear`, which is
	/// the honest outcome: four fixed squares at the corners of a growing square have more and more room
	/// beside a disk that has stopped at the square's own side.
	///
	/// **AND THE GENERAL LESSON, WHICH IS WHY THE CROSSOVER HAS A CASE OF ITS OWN: THE TWO BOUNDS CROSS,
	/// SO NO SINGLE SIZE CAN SUPPORT THE CLAIM THAT THE DISK IS BOUNDED CORRECTLY.** Below `b ≈ 520.6`
	/// only the diagonal is ever the answer and the block bound is unreachable; above it, the reverse. A
	/// suite laid out at one size is therefore blind to half of this function BY CONSTRUCTION, not by
	/// oversight — which is exactly how the defect got in: every case was green at 392 and the spill was
	/// found by rendering a THIRD size. *"Render geometry before believing it"* (CLAUDE.md) earned its
	/// place again, and what found it was one more picture rather than one more assertion. The case
	/// *"the disk's two bounds CROSS"* sweeps both sides and pins where they swap.
	[[nodiscard]] static constexpr float diskFor(float blockSide) {
		const float diagonal = 2.0f * (cornerOffsetFor(blockSide) * kRoot2 - kCornerClear);
		return std::max(0.0f, std::min(blockSide, diagonal));
	}

	/// **WHERE THE DISK AND THE FOUR CELLS GO, FOR A RAIL OF THIS SIZE (backlog IR).** All offsets are
	/// from the disk's centre, which is the centre of the room (`across` × `down`).
	struct Layout {
		float across = 0.0f; ///< the room across, as `acrossFor` gives it
		float down = 0.0f;   ///< the room down, as `downFor` gives it
		float disk = 0.0f;   ///< the pad's side
		float dx = 0.0f;     ///< a cell's inner corner from the centre, across
		float dy = 0.0f;     ///< and down
	};

	/// **THE LARGEST DISK THE ROOM HOLDS, THEN THE CELLS AS CLOSE TO IT AS `kCornerClear` ALLOWS.**
	///
	/// **THE DISK.** Each cell stays in its own quadrant, since it sits at its own waypoint's corner. So
	/// the furthest out a cell can go is the corner of the room, with its inner corner
	/// `(across/2 − kCellSide, down/2 − kCellSide)` from the centre. The disk is then the smallest of
	/// three bounds: the room's width, its height, and twice that corner's distance less `kCornerClear`.
	/// Centring is optimal: the four corners are symmetric, so any shift brings one of them closer. In a
	/// SQUARE room this is exactly `diskFor(side)`, the rule until IR. In a tall or wide room the
	/// diagonal bound grows with the long side, so the disk grows until the short side stops it.
	///
	/// **THE CELLS.** Each sits on its waypoint's 45° ray at `disk/2 + kCornerClear` from the centre,
	/// clamped to the room. When one axis is too short for that, the cell stays at that edge and slides
	/// along the other axis until it clears the rim by exactly `kCornerClear`. That is "where the free
	/// space is": in a tall rail the cells move up and down, in a wide one sideways, and the disk
	/// grows into the middle. When the disk is bound by the diagonal, both clamps hold and the cells are
	/// at the room's corners, as before. A cell never leaves its quadrant, because both offsets are ≥ 0.
	[[nodiscard]] static Layout layoutFor(float railWidth, float railHeight);

	/// THE LAYOUT OVER THIS RAIL'S LAID-OUT BOUNDS. `resized()` uses exactly this, so a case reading
	/// it is reading what the layout used and not a second copy of it.
	[[nodiscard]] Layout layout() const {
		return layoutFor(static_cast<float>(width()), static_cast<float>(height()));
	}
	[[nodiscard]] float diskSide() const { return layout().disk; }

	/// **THE SHORTEST THE PANEL MAY BE WHILE THIS RAIL IS OPEN.** A rail is as tall as the panel, so
	/// unlike its width this is a floor rather than a size — `SklitterExpanded::minimumSize` maxes the
	/// grid's own answer against it, because a grid with few live cells is shorter than the disk.
	///
	/// **338 → 422, AND THE PANEL STILL DOES NOT GET TALLER.** SKLITTER's grid asks for 445 with the
	/// circle open (four rows of 89 over a 65px band inside 24 of padding), so the floor is still under
	/// it — DERIVED, and the case for it reads the panel rather than this comment. What the growth does
	/// cost is a device in a mode with three rows or fewer, which would now meet the floor instead of
	/// the grid; that is the honest price of a disk this size and it is a floor rather than a surprise.
	/// **AND 422 → 454 ON 2026-09-20, WHICH IS THE OUTPUT STRIP AND ITS GAP AND NOTHING ELSE.** It is
	/// the exact term `downFor` takes OUT of the disk's room, so the disk is `kBlockHeight` at the
	/// floor just as it was — the panel grew and the circle did not shrink, which is the rule.
	///
	/// **AND IT HANDS THE FX RAIL'S CASE MORE MARGIN, WHICH IS WORTH SAYING BECAUSE THE PAIR HAS BEEN
	/// WRONG TWICE TODAY.** `FxLaneRail::kOpenMinimumHeight <= CircleRail::kOpenMinimumHeight` was
	/// passing at EXACT equality (422 = 422, two unrelated sums) while both fx lanes were stacked in
	/// one rail. The split into two rails already dropped the fx floor to 231; this raises the circle's
	/// to 454, so the gap is 223 and the case asserts a strict `<`.
	///
	/// **BUILT FROM THE DESIGN WIDTH'S BAND, AND THAT IS STILL THE RIGHT FLOOR NOW THE BAND VARIES
	/// (2026-09-22).** At the design width the block is `kBlockHeight` and this is exactly the height
	/// that holds it over two rows. At a NARROWER rail the band has more rows, but the block is then
	/// bound by the width — `kBlockFloor` at 220 is 172 short of `kBlockHeight` — so the rows added
	/// fit inside the height the smaller block no longer uses. The panel does not need to get taller
	/// for them; the sweep case measures that at every width down to the floor.
	static constexpr float kOpenMinimumHeight =
	    2.0f * kPadTop + kHeadingHeight + kGap + kBlockHeight + kMinOutputBand;

	/// Where the heading is written while the rail is open — public for the reason `FxLane`'s is: on a
	/// rail against the RIGHT edge the toggle shares the heading's end of the row, so "these two do not
	/// overlap" is a real question with a wrong answer available.
	[[nodiscard]] visage::Bounds headingBounds() const;

	/// **THE GROUND BEHIND THE CIRCLE — `--void-perform`, WHICH IS WHAT FEATHERS' PERFORM CIRCLE SITS
	/// ON.** The background behind the circle matches Feathers' own perform background.
	/// `PerformStage` paints no ground of its own, so that background is whatever
	/// the editor paints behind it, which is `Gui::Impl::drawBackground`'s `kVoid`. Named here so a case
	/// can assert the VALUE rather than read a colour off a picture.
	static inline const uint32_t& kGround = kVoid; // live: the chrome hue turns it (backlog LG)

	/// **WHAT THE GROUND WAS LAST PAINTED WITH, PRODUCED BY THE PAINT RATHER THAN RECORDED BESIDE IT** —
	/// `draw` assigns this from `fillGround`'s return, and `fillGround` is the call that paints. Zero
	/// until the rail has drawn once. **ITS LIMIT, STATED**: it cannot see a mutation INSIDE
	/// `fillGround` that changes the colour handed to the canvas and returns the other one. That is a
	/// pixel question and it was answered on the private display — inside and outside the ring both
	/// sample `#010204`, against `#0E1420` outside it before this change.
	[[nodiscard]] uint32_t groundForTest() const noexcept { return ground_; }

	[[nodiscard]] PerformPad& pad() { return *pad_; }
	[[nodiscard]] const std::array<ZoneCoordCell*, fx::sklitter::kCorners>& corners() const {
		return corners_;
	}

	/// **WHERE THE OUTPUT CHIPS GO — the strip along the bottom, full width inside the padding, and as
	/// many rows tall as this rail's width needs.** `outputBandFor(width())` is what `downFor`
	/// took out of the disk's room, so this rectangle and the disk's cannot overlap by construction
	/// rather than by a check.
	[[nodiscard]] visage::Bounds outputStripBounds() const;

	/// **BUILD THE SEVEN ARMS, OR DROP THEM WHEN THERE IS NO STATE TO ARM AGAINST.**
	///
	/// **NO ARMS WITHOUT A `ModState`** — `FxLane::layOutOutputArms` states the same rule and it is not
	/// caution: `SourceArm` holds a `ModState&`, and a chip over nothing is a control that cannot do
	/// what its arrow says. So the panel calls this when it is told, and a rail that has not been told
	/// draws no strip at all.
	///
	/// `device` is asked for rather than passed, for the reason `FxLane::sourceOfOutput` gives: a
	/// source names a DEVICE, and the way to ask is what survives anything that re-points it.
	void setOutputSources(ModState* mod, std::function<DeviceInstanceId()> device);

	/// THE ARMS, IN DECLARED OUTPUT ORDER; null entries where none was built. A case reads the rail's
	/// own widgets rather than a rectangle it restated.
	[[nodiscard]] const std::array<std::unique_ptr<SourceArm>, fx::sklitter::kPerformOutputs>&
	outputArmsForTest() const {
		return outputArms_;
	}

private:
	/// Paints this rail's ground and RETURNS the colour it used — see `groundForTest`.
	static uint32_t fillGround(visage::Canvas& canvas, float wide, float tall, uint32_t colour);
	/// Place the arms into `outputStripBounds()`, through the same walk every other host uses.
	void layOutOutputArms();

	uint32_t ground_ = 0;
	/// **ONE ARM PER DECLARED OUTPUT, OWNED BY THIS RAIL AND BUILT ONCE.** Unlike `FxLane`'s, these
	/// need no keying by slot: a panel is about ONE device for its whole life, so there is no swap
	/// underneath them to survive. The SOURCE is still asked for at every press and paint.
	std::array<std::unique_ptr<SourceArm>, fx::sklitter::kPerformOutputs> outputArms_ {};
	std::function<DeviceInstanceId()> outputDevice_;
	std::unique_ptr<PerformPad> pad_;
	std::function<bool()> anchorEnabled_;
	std::function<void()> anchorPress_;
	/// The four, owned; `corners_` is the same four as raw pointers because every reader wants them
	/// that way and `Corner`-indexed. Two arrays over one set of objects, which is a lifetime and an
	/// index rather than two copies of a value.
	std::array<std::unique_ptr<ZoneCoordCell>, fx::sklitter::kCorners> owned_ {};
	std::array<ZoneCoordCell*, fx::sklitter::kCorners> corners_ {};
};

/// **RAILS 2 AND 3 — `GRAIN FX` and `POST FX` — are `FxLaneRail`s** (`fx_lane_rail.h`, lifted out 2026-10-05 so
/// Resonate's lanes are the same widget). Its furniture must stay `CircleRail`'s, side by side on one panel.
static_assert(FxLaneRail::kPadX == CircleRail::kPadX && FxLaneRail::kHeadingHeight == CircleRail::kHeadingHeight
              && FxLaneRail::kGap == CircleRail::kGap);

class SklitterExpanded final : public GenericExpanded {
public:
	SklitterExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	                 const device::Descriptor& desc, Notifier* notifier = nullptr);

	/// The strip, and the gap between it and the controls below.
	static constexpr float kStripHeight = 30.0f;
	static constexpr float kBandGap = 12.0f;
	/// The whole band: one strip with a line of text over it.
	static constexpr float kLabelHeight = 11.0f;
	static constexpr float kBandHeight = kStripHeight + kLabelHeight + kBandGap;

	/// TEST SEAM — whether the panel would ask for a frame right now. The REPORT; the delivery is the
	/// `redraw()` inside, which `visage::Frame` gives no way to observe.
	bool updateContentForTest() { return updateContent(); }
	/// TEST SEAM — backlog MJ: whether the record light this surface draws is lit, off its own readings.
	[[nodiscard]] bool recordLightForTest() const { return sklitterRecording(ownHost(), deviceId()); }

	/// THE THREE RAILS — and, from `GenericExpanded`, the BODY, which is a fourth thing that folds and
	/// is reached through `bodyRailForTest()`. **All four open and close INDEPENDENTLY**, which needs no
	/// new concept: `collapsed_` is per-`Rail` and `railsWidth()` sums `railWidth()`, so a player who
	/// wants only the grain lane pays 34px for each of the other two. **SIXTEEN legal states**, up from
	/// eight, including the one where the panel is four strips side by side.
	[[nodiscard]] CircleRail& circleRailForTest() { return *circle_; }
	[[nodiscard]] FxLaneRail& grainRailForTest() { return *grainFx_; }
	[[nodiscard]] FxLaneRail& postRailForTest() { return *postFx_; }

	/// THE ANCHOR VERB (backlog HT), what the rail's button calls: one undo step over Perf X, Perf Y and the anchored push.
	void anchor();
	/// TEST SEAMS for the circle's parts — the pad and the four corner cells in `Corner` order.
	[[nodiscard]] PerformPad& padForTest();
	[[nodiscard]] const std::array<ZoneCoordCell*, fx::sklitter::kCorners>& cornerCellsForTest() const;
	/// ANY OF THE PANEL'S CONTROLS BY ITS PARAM INDEX — the one `ModDisplay` a cell of any KIND draws
	/// from (a knob, a zone cell, the pad), so a claim about what glows is made once for all of them
	/// (backlog HS) rather than per widget type through `knobsForTest`/`zoneCellsForTest`.
	[[nodiscard]] DeviceControl& controlForTest(size_t index) const { return controlFor(index); }

	/// **WHAT THE RAILS COST ACROSS, WHICH IS THE ONE TERM THE GRID IS NOT ALLOWED TO USE.** The editor
	/// sums `railWidth()` the same way (`gui.cpp`: `railEdge += x->railWidth()`); this is that sum for
	/// this panel. Public because a case asserting "the window grew by exactly what the rail wanted"
	/// must ask rather than restate it.
	[[nodiscard]] float railsWidth() const;

	/// **WHERE THE ROOM GOES WHEN THE PANEL IS DRAGGED WIDER: ALL OF IT TO THE CIRCLE (2026-09-20).**
	/// When the window is enlarged, the whole perform circle scales up with it.
	///
	/// **AND THE CIRCLE IS THE ONLY COLUMN THAT CAN SPEND IT, which is why "all" rather than "shared".**
	/// The panel is three columns. The GRID is fixed cells four across by design — `GenericExpanded`
	/// runs them from `kPad` at `kCellWidth` and stops — so width past its own answer is dead air behind
	/// the knobs. The FX LANES rail is a column of cards, each as wide as the rail, and `kWidth` there is
	/// *"chosen rather than derived ... not load-bearing"*: stretching it stretches every card for no
	/// gain. The circle is the one thing here whose whole substance is area. So the surplus is handed to
	/// it, and the other two keep exactly what they asked for at every window size.
	///
	/// Zero while the circle is CLOSED — a 34px strip cannot spend it either, so the panel behaves
	/// exactly as it did before this existed — and zero when the panel is clipped below its own minimum.
	[[nodiscard]] float surplusWidth() const;

	/// **HOW MUCH LESS ROOM THE PANEL WAS GIVEN THAN ITS OWN MINIMUM — the twin of `surplusWidth()`,
	/// and only one of the two is ever non-zero.** A MONOTONE CLAMP rather than a predicate: it is
	/// computed from the rails' BASE widths, so it cannot depend on the yield it causes and cannot
	/// oscillate. See the definition for why there is no `if (clipping)` anywhere.
	[[nodiscard]] float widthDeficit() const;

	/// What the circle rail is actually LAID OUT at: its ask, plus the surplus, LESS the deficit, never
	/// below `CircleRail::kYieldFloorWidth`. `bodyRightInset()` and `resized()` both read this, so the
	/// body's right edge and the rail's left edge are one number in both directions.
	[[nodiscard]] float circleWidth() const;
	/// The three rails as LAID OUT rather than as asked — `railsWidth()` with the circle's yielded
	/// width in place of its ask. **This is what stops the BODY being the residual** (backlog HB): the
	/// body's inset follows what the rails actually took, so a deficit spent on the circle is a deficit
	/// the grid never sees.
	[[nodiscard]] float railsLaidOutWidth() const;

	void resized() override;
	visage::Point minimumSize() const override;
	/// **THE CEILING FOLLOWS WHAT IS OPEN** (backlog EI). The open circle takes every spare pixel in both
	/// directions (`surplusWidth`, and it centres a square in the rail's height), so while it is open
	/// there is no cap at all. Closed, the width is the grid's and the rails' fixed asks and is capped
	/// like any grid; the height stays uncapped while an fx rail is open, because its chain column
	/// fills the rail's height, and is capped otherwise. The circle's chevron re-lays the rails and
	/// announces through `minimumSizeChanged()`, so a window left wide by an open circle closes to the
	/// cap on that pass and gets its width back when the circle reopens.
	visage::Point maximumSize() const override;

	/// **THE GRID'S CONTROLS GET THE OUTER STATE; THE CHAIN GETS IT AS A PARENT.** Two different
	/// answers to one call, which is why this is overridden rather than inherited: an inner control in
	/// the fx lane answers the chain's own `ModState`, and that state answers the outer one through
	/// `setParent`. `ChainExpanded` splits the same call the same way.
	void modulationBound(ModState* mod, EditHistory* history) override;
	/// The editor's history, kept for the anchor verb (GenericExpanded's own copy is private).
	EditHistory* anchorHistory_ = nullptr;

	/// **THE CHAIN'S PANELS ESCALATE PAST THIS ONE.** `SurfaceLayers` tells every view it mounts which
	/// host it is on; a view that holds a chain hands that on, so a device inside the chain presents
	/// where THIS panel was presented rather than inside it — nesting escalates to the outermost host,
	/// the same line `ChainExpanded::setPresentedOn` has held for the rack device.
	void setPresentedOn(SurfaceLayers* host) override;

	/// **THE PRESET BROWSER, IN THE WINDOW'S TITLE ROW** (backlog FO: SKLITTER gets a preset browser in
	/// its window header, reusing Feathers' own). It IS Feathers'
	/// `PresetBrowser` — the class the editor's header holds — over this device's own bank
	/// (`devicePresetBank`: `devices/SKLITTER`) at the title row's size. The bytes behind it are
	/// `device_preset.h`'s: this device's row and both lanes, never the captured audio.
	[[nodiscard]] visage::Frame* titleAccessory() const override { return presets_.get(); }
	[[nodiscard]] PresetBrowser& presetBrowserForTest() const { return *presets_; }
	/// What a load leaves as it was beyond the bypass the codec always keeps: `Engage`, the gesture.
	static constexpr std::array<uint8_t, 1> kPresetKeeps { static_cast<uint8_t>(fx::sklitter::Param::Engage) };

protected:
	/// ONE LEVEL DEEPER FOR THE CHAIN — `innerNest()`, which mixes THIS panel's device into its own
	/// nest. Stamped on the column's cards and on whatever they open.
	void onInnerNestChanged() override;

	/// **THE BAND, AND ONLY THE BAND.** The rails are beside the grid rather than above it, so they
	/// cost the body no height at all — which is the whole difference between this arrangement and the
	/// fold-down one it replaces.
	float bodyTopInset() const override;

	/// **WHAT IS NOT THE BODY ACROSS: the two rails.** The base needs it to place the body's own rail,
	/// and `drawBodyExtras` needed it already — this is the one answer both now read.
	float bodyRightInset() const override;

	void drawBodyExtras(visage::Canvas& canvas) override;

	/// THE FOUR WAYPOINT CONFIGURATIONS, WHILE THE RAIL IS OPEN. Closed, they go back to the grid —
	/// see the note above this class for why that is not a detail.
	bool drawnBySubclass(size_t index) const override;

	/// **THE PANEL ASKS FOR ITS OWN FRAMES TOO — and it did not**, so the expanded window still redrew
	/// wrongly. The CARD was given this an hour earlier and the panel was not: one
	/// surface of two, which is the third time in this device that a fix reached one road and stopped
	/// (density reached the slice modes and not Grain; the hard cut reached the looper renderer and
	/// not the grain renderer). **When a device has two surfaces over one picture, a fix to one of
	/// them is half a fix by default.**
	bool updateContent() override;

private:
	/// What was last drawn, so `updateContent` can ask whether the picture actually moved.
	std::vector<SklitterChunk> drawn_;
	int drawnStep_ = -2;
	float drawnPhase_ = -1.0f;
	float drawnFill_ = -1.0f;
	int drawnSeqBars_ = -1;
	/// backlog IG — the grid can move while the chunks do not; `kCount` so the first tick always draws.
	fx::sklitter::GrooveFeel drawnGroove_ { fx::sklitter::GrooveMode::kCount, -1.0 };
	/// `Reading::RepeatLength` as last drawn — Repeat's bottom row can change while nothing else does.
	float drawnRepeat_ = -1.0f;
	/// backlog MJ — the record light as last drawn (1 lit, 0 dark); -1 so the first tick draws it.
	int drawnRecording_ = -1;

	std::unique_ptr<CircleRail> circle_;
	/// **THE INSTANCE BOTH FX RAILS' COLUMNS ARE BUILT OVER, HELD AGAINST THE HOST'S REAP** (backlog JX) —
	/// the same hole `rack::ChainExpanded` had: the rails re-ask once a beat, and Feathers' reap runs after
	/// the beat, so a column could be drawn or turned over a freed chain until the next one. Declared
	/// BEFORE the rails, so it is let go only after their columns are gone.
	backplane::DeviceHost::SurfacePin chainPin_;
	/// **IN LAYOUT ORDER, LEFT TO RIGHT: circle, grain, post** — grain-fx sits to the left of post-fx.
	/// `resized()` walks them from the RIGHT edge inward and so reads this backwards, which
	/// is the one place the order is written as code rather than as a comment.
	std::unique_ptr<FxLaneRail> grainFx_;
	std::unique_ptr<FxLaneRail> postFx_;
	/// The four `Param::Cfg*` indices in `Corner` order, resolved once from the descriptor.
	std::array<size_t, fx::sklitter::kCorners> cornerParams_ {};
	/// What the browser's entries mean for this device. Declared BEFORE the browser so it outlives it.
	std::unique_ptr<DevicePresetProvider> presetProvider_;
	/// Owned here; PARENTED by the panel that presents this view (`ExpandedPanel::setAccessory`).
	std::unique_ptr<PresetBrowser> presets_;
};

} // namespace feathers::ui
