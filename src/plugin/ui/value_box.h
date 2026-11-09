/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 *
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

#pragma once

#include "device/format_value.h"
#include "device/modulatable.h"
#include "plugin/ui/drag_acceleration.h"
#include "plugin/ui/frame_watchers.h"
#include "plugin/ui/tooltip.h"
#include "plugin/ui/type_in.h"

#include <functional>
#include <string>
#include <visage/ui.h>

// A DRAGGABLE VALUE BOX — a label, a number, and a modulation bar, in a third of a card's width.
//
// A draggable, modulatable time box saves space against knobs. The value box glows in mod mode and
// shows a bar beside or over it (as Bitwig does) to indicate the modulation amount.
//
// WHY NOT A KNOB. A 52px dial with an 8px caption is most of a card's height for one control, and it
// cannot show a NUMBER — which is exactly what a time wants to show, because "10.9 ms" is the thing
// a hand is aiming for and a dial angle is not. Six of these fit in two rows where six knobs would
// have taken four. The card gets its curve back.
//
// THE PRINTED NUMBER STAYS AT THE BASE, AND THE BAR CARRIES THE MODULATION. That is this editor's
// existing rule for knobs, restated where it binds harder: knob.h records that the mock says the
// value indicator never shows the modulated result, and that this project has a second reason the
// mock could not have known — a readout that chased a modulator would re-rasterise its text every
// frame, and text churn is the family the atlas corruption was traced to. A value box is ALL text.
// So the number is the base, always, and the bar is where the modulation is drawn.
//
// IT KNOWS NOTHING ABOUT WHAT IT EDITS. Everything it needs arrives through device::Modulatable —
// which is the point of that interface existing, and why this widget can sit on a rack device's card
// today and on a plugin parameter tomorrow without changing.

namespace feathers::ui {

class ModState;

/// WHAT A CONTROL'S VALUE READS AS, given only what the control declares about itself.
///
/// A free function rather than a method, because three things print values and only one of them owns
/// a box: the generic fallback card, the Engine rail, and this widget. And because a device's declared
/// UNIT is the only thing needed to print it — which is the whole promise of declaring units at all.
/// A host that knows the vocabulary can render a device it has never seen.
///
/// Handles the LogSeconds convention that the bottom of a range means no time at all, printing
/// "off" — see device.h, where that convention is stated for both ends of the contract.
///
/// **IT IS `device::formatValue`** (`device/format_value.h`), where the formatting now lives so a host
/// slot can print a control's value in the DAW without reaching into the UI (host slots T3). A
/// using-declaration rather than a forwarding function of the same signature, and it has to be: every
/// unqualified call in this namespace passes a `device::ParamDesc`, so argument-dependent lookup finds
/// `device::formatValue` beside a `ui::` one and the call is ambiguous. One declaration, found by
/// both lookups, is the only spelling under which the forty-odd call sites stay as written.
using device::formatValue;

class ValueBox : public visage::Frame, public TooltipSource {
public:
	/// The control's declared tooltip, anywhere on the box — see `ui/tooltip.h`.
	const char* tooltipAt(visage::Point) const override {
		return !enabled_ && disabledReason_ != nullptr ? disabledReason_ : tooltipOf(&control_);
	}
	/// **THE CAPTION AND THE NAME IT STANDS FOR** (`ui::LabelFace`) — the control's declared name; cut when the caption
	/// is wider than the room `draw` gives it (a stacked box's caption row; a compact row's space left of the number).
	[[nodiscard]] std::optional<LabelFace> labelFaceAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>& out) const override { out.push_back({ &control_, Road::Face }); }
	/// THE GLOW (`lit`) AND THE BAR (`barBounds`), for the glow-honesty sweep (`ui::ControlFace::modLookFor`).
	[[nodiscard]] ModLook modLookFor(const device::Modulatable& c) const override;

	/// `control` is borrowed and must outlive the box — the card owns both.
	/// `label` is the caption, which is NOT the control's declared name: the declaration says "Env
	/// Attack" because a device must qualify its own controls in a list it does not own, and a box
	/// sitting under a card headed DAHDSR says "A".
	ValueBox(device::Modulatable& control, std::string label);

	void draw(visage::Canvas& canvas) override;

	/// **ALSO WHERE THE SUBSCRIPTION IS TAKEN — see `refreshWatch`.** A layout is the earliest moment
	/// every seated box passes through, and unlike a paint it happens in a HEADLESS fixture, so a case
	/// can hold the fact. Subscribing only on `draw` would have been a repaint fix no suite could see.
	void resized() override;

	/// What the box prints for its current value — the declared unit, applied. Public so a case can
	/// read the text rather than the pixels, which is the only way to assert a readout at all.
	std::string readout() const;

	/// The caption. Rename is allowed for the same reason Knob::setLabel is: a control's meaning can
	/// follow a mode while its identity does not.
	void setLabel(std::string label);

	/// The caption as it currently reads. The other half of setLabel: a caption that follows a mode
	/// is a CLAIM about what the control means, and a claim has to be checkable as text.
	const std::string& label() const { return label_; }

	/// THE CONTROL THIS BOX DRAWS, exactly as `Knob::control()` exposes a knob's. Two widget kinds
	/// implement the same interface over the same four controls, and a case sweeping "every
	/// implementation of `Modulatable`" cannot find half of them through one accessor: `MsegControl`
	/// and `RackControl` are reached only through a box, `FxControl` and `ParamControl` mostly
	/// through a knob. Borrowed, non-owning — the card owns both, as the constructor says.
	device::Modulatable& control() const noexcept { return control_; }

	/// **THE TYPED-VALUE FIELD (backlog LS)** — a triple-click opens it over the reading, on the value from
	/// before the double-click's reset. Null until the first triple-click; see `ui/type_in.h`.
	[[nodiscard]] TextField* typeInField() const noexcept { return typeIn_.field(); }
	/// Where it opens: over the reading's row (the whole box when compact).
	[[nodiscard]] visage::Bounds typeInBounds() const;
	/// Whether the control behind the last double-click refused to be typed into.
	[[nodiscard]] bool typeInRefused() const noexcept { return typeIn_.refused(); }

	/// Whether a drag right now would set an AMOUNT rather than a value — armed, a destination, and
	/// the Alt bypass not held. When Alt is held, a drag edits the value directly without creating
	/// a route, so this answers false. The same bypass `Knob::mappable` implements.
	///
	/// NO DEFAULT ARGUMENT, DELIBERATELY, AND THIS IS THE SECOND TIME THESE TWO SURFACES CONVERGED.
	/// `Knob::mappable` has `= false` because it has a DRAW-path caller (`knob.cpp:503`) that must
	/// not depend on a key held at paint time. This class has no such caller — both its callers are
	/// presses and both hold a real modifier — so a default here would buy nothing and cost the
	/// exact drift that produced the bug: the gate silently answering "no Alt" at a call site that
	/// simply forgot to ask. **Make the next omission a compile error rather than a repeat.**
	bool mappable(bool altDown) const;
	/// **WHETHER THE BOX GLOWS AS A TARGET** — `mappable` without a key (a paint cannot know one): switched on, a
	/// destination, and the armed source may reach it. A press here with no Alt assigns; that is the agreement the
	/// glow promises (a dropdown once glowed and the press opened its menu).
	[[nodiscard]] bool lit() const;
	[[nodiscard]] bool litForTest() const { return lit(); }

	/// WHERE THE MODULATION BAR IS DRAWN, in this box's own coordinates, and empty when there is
	/// nothing to draw. Exposed because "the bar says what the amount says" is a claim a case has to
	/// be able to check against geometry rather than against a screenshot — the project's rule that
	/// rendered geometry is verified rather than believed.
	visage::Bounds barBounds() const;

	/// Told when the value changes, so a card can redraw its curve in the same frame rather than one
	/// behind. The box knows what happened; the card knows what else depends on it.
	void onValueChanged(std::function<void()> callback) { valueChanged_ = std::move(callback); }

	/// TOLD WHEN A WHOLE EDIT FINISHES — one call per gesture, with where it started and where it
	/// ended, for the caller to record as one undo step.
	///
	/// THE BOX DOES NOT WRITE HISTORY ITSELF, and that is the interface's promise being kept rather
	/// than an omission. An EditRecord has to be able to put a value back after the widget that
	/// changed it has been torn down, so it must hold the ADDRESS of the thing — a bridge, a slot, a
	/// control index — and the widget is the one party here that deliberately does not know any of
	/// that. A first draft had this method dynamic_cast to the concrete control to build the record,
	/// which is exactly the coupling device::Modulatable exists to prevent: it would have compiled,
	/// worked, and quietly made the widget unusable for the next kind of control.
	/// OPEN A MENU INSTEAD OF DRAGGING, for a control whose values are a LIST rather than a range.
	///
	/// The MSEG's time base is a dropdown, and the reason reaches beyond that
	/// control — a stepped `Unit::Choice` box is a list of names, and dragging through 27 of them to
	/// find "1/8 dotted" is a gesture that gets slower the more useful the list becomes. A range wants
	/// a drag; a vocabulary wants a menu.
	///
	/// A HOOK RATHER THAN A MODE, so `ValueBox` gains no branch and no second personality: when this
	/// is set the press opens whatever the owner opens and the drag never arms. The box still draws
	/// itself, still prints through the descriptor's names, and still resets on double-click.
	void onPicker(std::function<void()> callback) { picker_ = std::move(callback); }
	[[nodiscard]] bool hasPicker() const { return static_cast<bool>(picker_); }

	/// **AND A BOX WHOSE CONTROL IS A LIST OPENS ONE WITHOUT BEING ASKED.**
	///
	/// Every value box that is a multivalue selector is a dropdown, not a drag-over-the-box selector —
	/// automod's time base was the case in point. The hook above was added for the MSEG and wired at
	/// exactly ONE site, while the paragraph beside it already said the reason reached beyond that
	/// control — so the generalisation was written down and then
	/// not made, and automod's four choice cells stayed draggable. `automod_card.cpp` even answered
	/// the original request with *"there is no separate dropdown widget in this editor"*, which was a
	/// reason to build the road rather than a reason not to.
	///
	/// **SO THE DEFAULT IS THE MENU, NOT THE OPT-IN.** Any box whose control is a stepped
	/// `Unit::Choice` with names gets it, from the DESCRIPTOR alone — no construction site opts in,
	/// none can forget, and a device that declares a choice tomorrow is a dropdown the day it is
	/// declared. `onPicker` still wins where it is set, because an owner that wants a different list
	/// (a submenu, a filtered set) must be able to say so.
	///
	/// **A TWO-WAY SWITCH IS NOT A LIST.** Below three entries this declines and the box keeps its
	/// drag, because a menu to choose between "Off" and "On" is two clicks for what a nudge does —
	/// and the switches on these cards are CHIPS anyway. Three is the smallest count at which reading
	/// beats guessing.
	[[nodiscard]] bool choiceMenuAvailable() const;

	/// Open it. False when this control is not a list — see `choiceMenuAvailable`.
	bool showChoiceMenu();

	/// **SWITCHED OFF: DRAWN DIMMED, AND THE HAND REFUSED** — `Knob`'s idiom, for a control whose value means
	/// nothing in the current arrangement (Resonate's drone Phase and Chord at Drone Out 0). A disabled box takes no
	/// press at all — no menu, no drag, no reset, no route — and answers no mapping. Enabled by default.
	void setEnabled(bool enabled);
	bool enabled() const noexcept { return enabled_; }
	/// WHY IT IS SWITCHED OFF, shown as its tooltip while it is (one sentence, no inner full stop: the brief tooltip cuts at the first). Null keeps the control's own.
	void setDisabledReason(const char* reason) noexcept { disabledReason_ = reason; }
	[[nodiscard]] const char* disabledReason() const noexcept { return disabledReason_; }

	/// **THE MENU `showChoiceMenu` OPENS, BUILT AND NOT SHOWN — so a case can read it and pick from it.**
	/// One row per declared entry, and its `onSelection` is the whole pick: `PopupMenuFrame` answers a
	/// click by calling exactly that with the row's id, so a case that does the same is on the road a
	/// hand takes rather than beside it (backlog GG, where the claim is "a pick applies one value").
	/// Only meaningful where `choiceMenuAvailable()` is true.
	[[nodiscard]] visage::PopupMenu choiceMenu();

	/// **WHETHER A PRESS ON THIS BOX OPENS A MENU** — an owner's picker, or a named Choice list. The one
	/// question the caret below is drawn from, so a box cannot promise a menu it does not open.
	[[nodiscard]] bool opensMenu() const { return hasPicker() || choiceMenuAvailable(); }

	/// **THE DROPDOWN CARET — on every box that opens a menu, by design.** A small `v` at the
	/// right end of the CAPTION row — a compact box's one row — drawn in strokes (the glyph atlas lacks
	/// the arrows) in the caption's colour. Empty for a box that opens nothing. The caption's rectangle
	/// (`captionBox`; a compact box's readout, `readoutBox`) gives up exactly this much on that side, so
	/// the caret never sits on text, and a stacked box's VALUE keeps its whole width.
	[[nodiscard]] visage::Bounds caretBounds() const;
	static constexpr float kCaretWidth = 5.0f;
	static constexpr float kCaretHeight = 3.0f;
	/// Between the text's room and the caret.
	static constexpr float kCaretGap = 2.0f;

	/// HOW MANY ENTRIES THE CONTROL DECLARES, from its RANGE — `max - min + 1`, inclusive.
	[[nodiscard]] size_t entryCount() const;

	/// **THE TEXT OF ONE ROW, INCLUDING THE MARK — and it is a function so the mark can be ASSERTED.**
	///
	/// A `visage::PopupMenu` is built and shown in one breath; nothing hands it back for reading, so a
	/// case that wanted to know whether the X is on the right row could only photograph a menu. This
	/// codebase has a rule about that (*"a claim about a value cannot be inferred from a rendered
	/// buffer"*), and the remedy it names is a seam that exposes the value — five lines, against no
	/// amount of cleverness with pixels.
	[[nodiscard]] std::string choiceLabelAt(size_t index) const;

	void onEdited(std::function<void(double before, double after)> callback) {
		edited_ = std::move(callback);
	}

	//-----------//
	// Geometry  //
	//-----------//

	/// The mock's caption and readout sizes, stacked. A box is a label row over a value row, which is
	/// what lets three sit in 198px with the value legible rather than four in a row of tiny ones.
	static constexpr float kLabelHeight = 10.0f;
	static constexpr float kValueHeight = 14.0f;
	static constexpr float kPadY = 2.0f;
	static constexpr float kHeight = kPadY + kLabelHeight + kValueHeight + kPadY;

	/// COMPACT: caption and number on ONE row, and no border of its own.
	///
	/// WHY IT EXISTS. The full second-row phase box under a zone knob was too large and looked
	/// disjointed (2026-08-29). At `kHeight` under a 38px Engine rail knob the box
	/// is 28 of the assembly's 104 pixels — a quarter of the widget for a secondary quantity, drawn
	/// as a second card sitting beneath the first.
	///
	/// A ROW, NOT A SHRUNK CARD. The two-row layout exists because a box on a card is standing in for
	/// a KNOB and must carry a caption with the weight a knob's caption has. A box that is already
	/// inside a labelled assembly is not naming itself to a stranger — the knob above it says what it
	/// belongs to — so the caption can sit beside the number instead of over it.
	///
	/// AND NO BORDER UNLESS IT IS THE MAPPING SIGNAL. The ordinary border is what makes the box read
	/// as its own object, which is exactly the "disjointed" being complained about; the ARMED border
	/// is not decoration at all — it is the same periwinkle that says "this control could take the
	/// route", and dropping it would break the affordance the honesty rule is about. So the frame
	/// goes and the signal stays.
	static constexpr float kCompactHeight = 15.0f;

	/// Horizontal padding inside a compact box, between its edge and the text.
	static constexpr float kCompactPadX = 4.0f;

	void setCompact(bool compact) { compact_ = compact; }
	[[nodiscard]] bool isCompact() const noexcept { return compact_; }

	/// **HOW TALL THIS BOX WANTS TO BE — the one number a surface seating it should ask for.**
	///
	/// Both heights above are public and both are correct, which is exactly the problem: a caller has
	/// to know whether the box it is holding was made compact, and `GammaBox` makes itself compact in
	/// its own constructor where no seating site can see it. A generic surface holding a
	/// `ValueBox*` cannot answer that question and should not have to — it stretched the box to
	/// whatever cell it had instead, which is how SKLITTER's Engage became a 74px slab with its
	/// content in the top third.
	///
	/// THE WIDGET IS THE ONLY THING THAT KNOWS HOW IT DRAWS ITSELF, which is this file's standing
	/// argument for why a card does not position a knob's caption. Same argument, one size up.
	[[nodiscard]] float naturalHeight() const noexcept { return compact_ ? kCompactHeight : kHeight; }

	//-------------------------------------------------------------------------------------------//
	// A BOX ON AN INFINITE LINE.                                                                  //
	//-------------------------------------------------------------------------------------------//
	//
	// A correction to the premise the zone widgets were built on: most of the firmware's zone knobs
	// allow "just keep turning" past the 8th zone, which keeps driving the phase, and a zone name is
	// just a name assigned to a region of an otherwise infinite space.
	//
	// SO A ZONED CONTROL WAS NEVER BOUNDED. There is ONE infinite line, in ONE unit, with two
	// GRANULARITIES and a name lookup over part of it — not a bounded quantity beside an unbounded
	// one. A widget whose ends mean something was expressing fine motion along that line and
	// pretending the ends were real.
	//
	// **AND THE WRAP-AND-CARRY THAT USED TO EXPRESS THAT IS GONE (BJ piece 4).** A drag off the top
	// came back at the bottom and reported a lap for a second control to absorb — which made the
	// bound load-bearing, since leaving the range was implemented by not leaving it. It was settled
	// the other way on 2026-09-09: none of these is bounded. A coordinate continues, and a
	// device wanting a lap count derives it from the coordinate's own whole part.
	/// HOW MUCH VALUE A FULL `kDragRange` OF TRAVEL IS WORTH. Zero (the default) means the control's
	/// whole declared range, which is what every ordinary box wants.
	///
	/// **WHY THE FINE HALF OF A COORDINATE NEEDS ANOTHER NUMBER.** Its declared range is the WHOLE
	/// named span — eight regions — so at the default a single drag crosses all eight and lands back
	/// where it started one carry later. That is arithmetically correct and useless: nothing about it
	/// is fine, and the carry it reports is unreachable by any gesture smaller than the entire line.
	/// A case caught it by asking where the value ended up after a full drag and getting the value it
	/// started at.
	///
	/// So a coordinate's fine half asks for ONE REGION per drag — the multi-turn feel the zone knob
	/// had, where crossing a boundary is a gesture rather than a whole sweep.
	void setDragSpan(double span) noexcept { dragSpan_ = span; }

	/// WHAT A FULL DRAG IS WORTH, so a case can assert it. **A span is not recoverable from anything
	/// else the widget exposes** — it changes how far a gesture travels, and a gesture is not
	/// observable headlessly — so a caller that forgot to set one and a caller that set the right one
	/// are indistinguishable without this. Gamma's is the case that needed it.
	[[nodiscard]] double dragSpanForTest() const noexcept { return dragSpan_; }

	/// **POINTER ACCELERATION (backlog QB)** — a value drag's per-pixel rate rises with the hand's speed, by the curve
	/// in `drag_acceleration.h`. A slow drag keeps the grabbed digit's own rate exactly; a flick reaches `kMaxGain`
	/// times it. Off by default, so no box's drag moves unless its seating asks: `ZoneCoordCell` does (2026-11-06:
	/// fast at top speed, precise when slow, hence acceleration). Amount drags
	/// (mapping mode) and the Shift-fine drag never accelerate — fine is the precise override.
	void setAccelerated(bool on) noexcept { accelerated_ = on; }
	[[nodiscard]] bool accelerated() const noexcept { return accelerated_; }

	/// **A PRESS GRABS THE NEAREST DIGIT, not only the one strictly under it — see `pressPlaceAt` (backlog QB).** Off
	/// by default: a box that has declared what a press on its padding means keeps it (SKLITTER's Groove Amount drags
	/// its whole range from there, and a case says so). `ZoneCoordCell` turns it on, where the old fallback was the
	/// FINEST column and made a near miss of the big digit 100× slower.
	void setNearestDigitPress(bool on) noexcept { nearestDigitPress_ = on; }
	[[nodiscard]] bool nearestDigitPress() const noexcept { return nearestDigitPress_; }


	/// PRINT IT SOME OTHER WAY — a formatter that replaces `formatValue`'s declared-unit rendering.
	///
	/// WHY A BOX EVER NEEDS ONE. `formatValue` renders a control the way its DECLARATION says, which
	/// is the right default and the whole promise of declaring units: a host that knows the
	/// vocabulary can draw a device it has never seen. But a zoned control declares itself a Percent
	/// because that is what it IS to the engine, and "0%" is not a coordinate. The display follows the
	/// firmware's, and the firmware counts: `0 : 42`, not `0.00 : 0%`.
	///
	/// SO THE DECLARATION STILL DESCRIBES THE CONTROL and this describes the VIEW. Nothing about the
	/// value changes — a host, the Engine rail and the generic card all still print the declared unit,
	/// because they ask the descriptor rather than this box.
	///
	/// **`sameNumber`: THE READING IS THE CONTROL'S OWN NUMBER UNDER ANOTHER NAME**, so a typed number means what it
	/// means in the declared unit and the box stays typeable. The LFO's Rate on the Hz base prints "2Hz" where the
	/// declaration prints "2X" — exp2 of the same value either way (backlog NJ) — so typing 2 lands on 2 Hz. A
	/// reading in a DIFFERENT number (the Phaser's semitones from a tracked note) leaves this false and refuses.
	/// AND THE FACE'S OWN UNIT NAME TYPES BACK: on such a box "2hz" or "2 Hz" is read as 2 while the face prints Hz,
	/// case-insensitive (`faceUnitStripped`); the declared unit's spellings ("2x") still read as they always did.
	void setReadoutFormatter(std::function<std::string(double)> formatter, bool sameNumber = false);
	/// `typed` with the unit the face prints right now taken off its end, or as typed when it ends in another.
	[[nodiscard]] std::string faceUnitStripped(const std::string& typed) const;

	/// The bar sits along the box's bottom edge, inside its border.
	static constexpr float kBarHeight = 2.0f;
	static constexpr float kCornerRadius = 3.0f;

	/// Pixels of vertical travel for the control's whole range. The knob's own number, so a hand that
	/// has learned one has learned the other.
	static constexpr float kDragRange = 140.0f;

	//--------------------------------------------------------------------------------------------//
	// DIGIT-PLACE DRAGGING — backlog BJ                                                            //
	//--------------------------------------------------------------------------------------------//

	/// **HOW MANY OF A DIGIT'S OWN UNITS A FULL DRAG IS WORTH WHEN THE PRESS LANDED ON IT.** Ten: a
	/// full drag rolls the grabbed digit once through its own range and carries, which is the whole
	/// intuition of grabbing a digit. Larger and the digit you aimed at is not the one that moves;
	/// smaller and a full drag cannot even complete it.
	static constexpr double kUnitsPerPlaceDrag = 10.0;

	/// **THE PLACE VALUE OF THE DIGIT UNDER `x`, or 0 where the press is not on one.**
	///
	/// THE COARSE:FINE PAIR IS GONE (BJ, 2026-09-08): it collapses to one box, and the speed of a mouse
	/// edit depends on where the box is grabbed. Grab the hundreds column
	/// and a drag moves hundreds; grab the units and it moves units. **This is what replaces the pair
	/// of controls** — as many granularities as the number has digits, in one control, rather than
	/// exactly two fixed at declaration time across two params and a carry road.
	///
	/// ZERO IS THE REFUSAL and the caller falls back to `dragSpan_`: a press on a sign, a decimal
	/// point, a unit suffix, or past the end of the text is not a press on a place.
	///
	/// **AND IT REFUSES WHOLESALE WHERE THE READOUT IS NOT THE VALUE** — see `readoutTracksValue`. A
	/// Percent box prints 42 for 0.42 and a Choice box prints a word; a digit column in either of
	/// those names nothing the drag could move by. Only a box that renders its own number gets this.
	[[nodiscard]] double placeAt(float x) const;

	/// **THE PLACE A PRESS AT `x` GRABS — the digit under it, or else the NEAREST digit (backlog QB).** `placeAt` is
	/// the strict hit test; this is what a `setNearestDigitPress` box (a zoned one) latches. A digit is ~5 px wide and MEASURED 66% of a zone box's
	/// width is padding or punctuation, and a press there used to fall back to the FINEST column — 100× slower than
	/// the units digit beside it. So the same grab aimed at the big digit was fast or crawling depending on one pixel
	/// (too slow even when grabbing the large digits, then fast on a second grab). Left of the
	/// number grabs its leading digit, right of it the last, a point or sign its nearer neighbour (the finer on a tie).
	/// Zero only where the readout has no place at all (not the value, or no digit), as `placeAt`.
	[[nodiscard]] double pressPlaceAt(float x) const;

	/// WHETHER THIS BOX'S READOUT IS ITS VALUE, rather than a scaled or named translation of it —
	/// measured by asking the formatter, at the current value and one `place` above it, so no
	/// construction site has to declare anything and a unit added tomorrow is classified correctly
	/// without being listed anywhere. `tolerance` is the readout's own rounding.
	[[nodiscard]] bool readoutTracksValue(double place, double tolerance) const;

	/// **HOW MUCH VALUE ONE UNIT OF THE READOUT IS WORTH, FOR A `digitBox` CONTROL, or 0 (backlog IO).**
	///
	/// The one exception to the refusal above, and it is narrow on purpose: only a control that DECLARES
	/// itself a digit box (`device::ParamDesc::digitBox`) gets it, so no existing box's drag moves. Its
	/// readout only has to be a straight line across the declared range — SKLITTER's Groove Amount prints
	/// a swing % that runs 50→83.3 over 0..1. Measured from the formatter at the range's ends and its
	/// middle, so a readout that is a word or a curve gets 0 and no digit road. `placeAt` multiplies the
	/// grabbed place by this, which makes grabbing the tenths of "62.5%" move a tenth of a PERCENT.
	[[nodiscard]] double scaledReadoutScale(double tolerance) const;

	/// THE SMALLEST DIGIT THIS READOUT PRINTS, or 0 where digit dragging does not apply — the drag's
	/// last resort before the declared range. See the .cpp: on an unbounded coordinate the range is a
	/// lethal fallback, and the finest column is the conservative one a reader can actually see.
	[[nodiscard]] double finestPlace() const;

	/// WHERE THE NUMBER IS DRAWN — the rectangle and the justification, asked by `draw` AND by
	/// `placeAt`. **One source of truth**, because a hit test computed separately from the drawing is
	/// a hit test that disagrees with the picture the moment either moves, and this box draws its
	/// readout three different ways depending on whether it is compact and whether it has a label.
	struct ReadoutBox {
		float x = 0.0f;
		float width = 0.0f;
		bool centred = true; ///< false means right-aligned
	};
	[[nodiscard]] ReadoutBox readoutBox() const;
	/// WHERE THE CAPTION OF A STACKED BOX IS DRAWN — the full width, less the caret's room when the box
	/// opens a menu (the caret sits on the caption row there, so the value keeps its width).
	[[nodiscard]] ReadoutBox captionBox() const;

	[[nodiscard]] double pressPlaceForTest() const noexcept { return pressPlace_; }
	/// Shift-drag, from Knob::kFineFactor.
	static constexpr float kFineFactor = 0.2f;
	/// **HOW BRIGHT A SWITCHED-OFF BOX'S MAPPING BORDER IS** against the live one's full colour (QK, by design).
	static constexpr float kGreyedSignalAlpha = 0.5f;
	/// An amount has twice the range and half the travel — Knob::kRouteDragRange.
	static constexpr float kRouteDragRange = 70.0f;
	/// What a press on an unrouted destination creates before the drag has moved, so the bar appears
	/// under the hand rather than after the first few pixels. Knob::kRouteSeedAmount.
	static constexpr float kRouteSeedAmount = 0.4f;

private:
	/// **PUT THIS BOX ON THE PANEL STATE'S WATCHER LIST, so arming a source REPAINTS it.**
	///
	/// THE BUG: value boxes did not glow; in assign mode they got a border, but only after a
	/// click. The box read the right state all along — `draw` computes
	/// `isDestination && armed` exactly as `Knob` does — and was never TOLD when that state moved, so
	/// it kept painting the last frame until some other edit repainted it. Clicking is such an edit.
	///
	/// **THE SAME REPORT, ONE WIDGET OVER.** `frame_watchers.h` records the knob version of this in
	/// detail — the effect assign knob glow not lighting right away, 2026-09-05, where a
	/// `Watch` move-assignment unsubscribed the six knobs a hand was about to point at. That was fixed
	/// and this was never wired at all: `Knob::setModulation` ends in `watch_ = mod_->watch(this)` and
	/// `ValueBox` has no `setModulation` and no watch.
	///
	/// **AND IT MUST NOT GAIN ONE.** `mod_route_menu.h` was written to keep exactly this out of the
	/// seven construction sites — a `setModulation` per site is the shape that shipped Bus Utility
	/// with no arm glow. So the box asks its CONTROL for the state and subscribes itself.
	///
	/// **RESOLVED LAZILY AND RE-RESOLVED, because the state arrives AFTER construction.** A surface
	/// sweeps `control(i).setModulation(...)` once it has one, which is later than the box's
	/// constructor; and a surface may hand over a different state. Comparing against what is currently
	/// watched makes both cases one road, and re-watching the same state is a no-op the `Watch`
	/// assignment now handles correctly (it did not, which is what the 2026-09-05 fix was).
	void refreshWatch();

	void commit(double value);
	/// Stroke the caret into `caretBounds()`, if there is one.
	void drawCaret(visage::Canvas& canvas) const;
	/// THE READOUT AT `at`, AS A NUMBER, or NaN where it prints no number — the parse both digit checks share.
	[[nodiscard]] double parseReadout(double at) const;
	void beginDrag(const visage::MouseEvent& e);
	void dragTo(const visage::MouseEvent& e);
	void endDrag();

	/// **THE MODULATION SUBSCRIPTION, HELD** — see `refreshWatch`. Held rather than taken and dropped
	/// because the token IS the subscription: releasing it unwatches.
	Watch watch_;
	/// WHAT `watch_` IS A SUBSCRIPTION TO, so a state that changes is noticed and a state that has not
	/// costs nothing. Never dereferenced — identity only.
	const ModState* watched_ = nullptr;

	device::Modulatable& control_;
	std::string label_;
	std::function<std::string(double)> formatter_;
	/// See `setReadoutFormatter`.
	bool formatterIsSameNumber_ = false;
	bool compact_ = false;
	double dragSpan_ = 0.0;
	/// THE PLACE THE CURRENT DRAG GRABBED, latched at press. Zero when the press was not on a digit.
	double pressPlace_ = 0.0;
	std::function<void()> valueChanged_;
	std::function<void(double, double)> edited_;
	std::function<void()> picker_;

	bool dragging_ = false;
	bool enabled_ = true;
	const char* disabledReason_ = nullptr;
	bool draggingAmount_ = false;
	bool fine_ = false;
	float anchorY_ = 0.0f;
	double anchorValue_ = 0.0;
	float anchorAmount_ = 0.0f;
	double before_ = 0.0;
	float beforeAmount_ = 0.0f;
	/// ACCELERATION'S PER-GESTURE STATE — every field reset at the press (`beginDrag`), so nothing a gesture
	/// learned reaches the next one. `extraTravel_` is the travel the gain ADDED beyond the pointer's own, kept
	/// apart so a drag that never accelerates computes exactly `anchorY_ - y`, as an unaccelerated box does.
	bool accelerated_ = false;
	bool nearestDigitPress_ = false;
	drag_accel::Tracker tracker_;
	float lastY_ = 0.0f;
	double extraTravel_ = 0.0;

	/// THE TYPED-VALUE FIELD (backlog LS). LAST, so it — and the field it may own as a child — goes first.
	TypeIn typeIn_ { *this };
};

/// **A CHOICE SEATED IN A ROW OF CHIPS — backlog GG.** The modulator cards' Mode (and the LFO's
/// Waveform) were chips that advanced on a press; they are compact, uncaptioned boxes now, in the
/// same cells, so a press opens the list instead. The card outlines the cell as an unlit chip and the
/// box sits one pixel inside it — a compact box draws no resting border of its own (see
/// `kCompactHeight`), and without the outline the row would read as three chips and a loose word.
/// Two functions so the three cards cannot seat or draw it three different ways.
void seatInChipCell(ValueBox& box, const visage::Bounds& cell);
void drawChoiceCell(visage::Canvas& canvas, const visage::Bounds& cell);

} // namespace feathers::ui
