/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

#include "device/device.h"
#include "device/endpoint.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// THE UNIFIED MODULATION SURFACE, AS THE ONE THING A WIDGET HAS TO IMPLEMENT TO JOIN IT.
//
// Design intent: new dockables define their ui however they want — including new widgets — but
// there is a unified 'knob like' api to unify the modulation surface.
//
// SO THIS IS NOT A WIDGET, AND NOT A BASE CLASS FOR ONE. It draws nothing, it owns no rectangle, it
// knows about no windowing library. It is the set of QUESTIONS a widget must be able to answer in
// order to arm, glow and route like every other control in the window — and the deliberate smallness
// is the feature. A contract that said "inherit our knob" would have forced every modulatable thing
// to be a dial, and the interesting ones are not: an MSEG breakpoint, an XY pad, a curve segment
// bent by the mouse, a draggable time box. Each of those is a thing a hand should be able to point a
// modulator at, and not one of them is a dial.
//
// IT IS DESIGNED AGAINST TWO REAL IMPLEMENTORS RATHER THAN GUESSED AT, which was the stated
// condition for writing it at all. The requirements come from ui::Knob, which has carried the
// editor's whole modulation surface since the panel shipped and whose behaviour is the definition of
// "like every other control"; the first implementation is the DAHDSR card's draggable value box,
// which is a different shape entirely and so is a real test of whether the abstraction is about
// modulation rather than about dials.
//
// WHAT IS DELIBERATELY ABSENT: which source is armed. That is editor state, it is the same for every
// control on screen, and a widget that had to be told it would be a widget that has to be wired to
// the panel. An implementation closes over it and answers `display()` accordingly, so a third-party
// widget asks one question and draws.

namespace feathers::device {

/// EVERYTHING A WIDGET NEEDS IN ORDER TO DRAW ITS MODULATION, in one read.
///
/// One struct rather than six accessors because these are read TOGETHER, once per paint, and a
/// widget that fetched them one at a time could straddle a change and draw a ring from this frame's
/// amount against last frame's base. The knob's own ModRing has been one object for the same reason.
struct ModDisplay {
	/// May a source be routed here at all? False makes every other field meaningless — and a control
	/// that is not a destination must never glow, which is the honesty rule applied to a verb: a lit
	/// control offers a drag that would do nothing.
	bool isDestination = false;

	/// Is a source armed right now, so that a drag on this control would set an AMOUNT rather than a
	/// value? The glow.
	bool armed = false;

	/// How far the armed source reaches this control, in units of the control's own declared range,
	/// signed. Zero means nothing is routed from the armed source.
	float amount = 0.0f;

	/// **DOES THE ARMED SOURCE SWING BOTH WAYS?** A ring draws its reach from the value in one
	/// direction for a unipolar source and in both for a bipolar one, so this is the third fact of the
	/// same kind as `isDestination` and `amount` and belongs beside them.
	///
	/// **IT IS HERE RATHER THAN ON THE MOD STATE, AND THE REASON IS A BUG THAT SHIPPED.** A first
	/// attempt put a polarity probe on `ModState` — which knows the focused source and nothing about
	/// what it is — and had the ring ask that while asking this struct for everything else. Two sources
	/// of truth for one ring is exactly how automod's coordinate-box halves ended up reversed for a
	/// day. A fact about how a control is modulated lives where that control's modulation is described.
	///
	/// FALSE FOR A UNIPOLAR SOURCE AND FOR NO SOURCE AT ALL, which is the same safe default
	/// `SourceView::isBipolarAt` takes: a unipolar rest is at the bottom, and drawing nothing there
	/// beats drawing a centre line for a source that has none.
	bool bipolar = false;

	/// WHERE THE POINTER AND THE PRINTED NUMBER GO. The base — never the modulated result.
	///
	/// This is a rule rather than a preference and it predates this interface: the mock states it
	/// ("the value indicator stays at the base"), and this project has a second reason the mock could
	/// not have known — a readout that chased a modulator would re-rasterise its text every frame,
	/// and text churn is the family the editor's atlas corruption was traced to. A widget that is ALL
	/// text, like a value box, is bound by it harder than a dial is.
	double base = 0.0;

	/// Where the value actually IS this block, once modulation is applied. What the moving part of
	/// the indicator draws — the arc, the bar — while the number stays put.
	double modulated = 0.0;

	/// IS ANYTHING ELSE ROUTED HERE — any source other than the focused one.
	///
	/// `amount` above is deliberately about the FOCUSED source only, because that is what a drag will
	/// edit and what the bar's length has to mean. The cost of that, which went unnoticed until it
	/// cost a whole evening, is that every other route to this control is drawn as NOTHING: make a
	/// route, focus a different source, and the control goes back to looking untouched while the
	/// route is still live and still making the sound.
	///
	/// Chasing a ten-second envelope period through four rounds of measurement turned up an invisible
	/// self route, created by adjusting attack etc. in modulation mode with no glow on the boxes and
	/// no route shown in the Engine rail. Measured: the same
	/// box reported amount 1 with a bar while its envelope was focused, and amount 0 with no bar one
	/// call later, with the route untouched in the bridge throughout.
	///
	/// This is the honesty rule read from the other end. That rule says a control must not offer what
	/// it cannot do; its converse is that a control must not stay silent about what is being done TO
	/// it. A widget draws this differently from `amount` — it is not a quantity to edit, it is a fact
	/// to disclose.
	bool routedElsewhere = false;

	/// MAY THE FOCUSED SOURCE BE ROUTED HERE, RIGHT NOW — the gate AND the glow, which have to be one
	/// answer.
	///
	/// `isDestination` says the control can be a destination at all; this says the source currently
	/// armed is allowed to reach it. They differ, and the difference is the honesty rule's exact
	/// failure case: a weight source may not reach a puck destination, so a widget lit from
	/// `armed && isDestination` alone offers a drag the bridge will then refuse. Knob already asked
	/// the right question (maySourceRouteTo) and the value box did not — this is where the two stop
	/// being able to drift apart, because there is now one answer rather than two computations.
	bool routable = false;

	/// **THE CURVE THE FOCUSED SOURCE'S ROUTE READS IT THROUGH** (the route's transfer chip).
	/// It changes the REACH, not the amount: a rectified bipolar source reaches one way only, so the ring
	/// is drawn from `reachLo`/`reachHi` and `amount` stays the number a drag edits.
	Transfer transfer = Transfer::Lin;

	/// **THIS DESTINATION TAKES A VALUE PER EAR** (`ParamDesc::stereo`, backlog KU) — a capability, advertised by the
	/// potential-destination glow's stereo tint whatever source is armed.
	bool stereo = false;
	/// **A STEREO ROUTE REACHES IT NOW** — the ring keeps its one reach arc (a strength is not stereo) and marks where
	/// each ear is, in the two ear colours.
	bool stereoLive = false;
	/// **WHERE THE RIGHT EAR IS** — equal to `modulated` unless `stereoLive`.
	double modulatedR = 0.0;

	/// THE REACH AS MULTIPLES OF `amount`, the lowest and highest the route can add: the curve's extremes
	/// over what the source can say (−1..1 bipolar, 0..1 unipolar), with the rest value 0 included. Lin
	/// gives the ring every widget has always drawn — (−1, 1) bipolar, (0, 1) unipolar.
	///
	/// **A φ CURVE'S EXTREMES ARE INSIDE THE RANGE, NOT AT ITS ENDS** (backlog PY) — it peaks at 1 somewhere in
	/// every period, so its reach is Lin's whatever it answers at ±1.
	[[nodiscard]] float reachLo() const noexcept {
		if (isPhiTransfer(transfer)) {
			return bipolar ? -1.0f : 0.0f;
		}
		const float a = shapeTransferWith(transfer, bipolar ? -1.0f : 0.0f, NoPhiCurve {});
		const float b = shapeTransferWith(transfer, 1.0f, NoPhiCurve {});
		return std::min({ a, b, 0.0f });
	}
	[[nodiscard]] float reachHi() const noexcept {
		if (isPhiTransfer(transfer)) {
			return 1.0f;
		}
		const float a = shapeTransferWith(transfer, bipolar ? -1.0f : 0.0f, NoPhiCurve {});
		const float b = shapeTransferWith(transfer, 1.0f, NoPhiCurve {});
		return std::max({ a, b, 0.0f });
	}
};

/// THE TWO FACTS A RING DRAWS FROM, FILLED FOR A ROUTE'S CURVE — what every seating does after asking the
/// source's polarity: the curve is recorded, and a curve that leaves only the upper half (Rectify, Pos
/// half) makes the reach one-sided, so a widget that reads `bipolar` alone draws the truth too. Neg half
/// keeps `bipolar`, because its reach is the LOWER half: `reachLo`/`reachHi` say so.
inline void applyTransferToDisplay(ModDisplay& out, Transfer t) noexcept {
	out.transfer = t;
	if (out.bipolar && (t == Transfer::Abs || t == Transfer::Pos)) {
		out.bipolar = false;
		out.transfer = Transfer::Lin; // now a unipolar reach, which Lin over 0..1 draws exactly
	}
}

/// HOW FAR AN AMOUNT MAY REACH, in units of the destination's own range, each way.
///
/// Part of the contract rather than of any host's private table, because a WIDGET needs it: a drag
/// that sets an amount has to know what to clamp to, and a widget that had to be told by whoever
/// constructed it would be a widget that can be told wrong. Two means full-range modulation is
/// available from any base position rather than only from the bottom of the travel.
///
/// The host's own constant is checked against this rather than the two being written twice.
inline constexpr float kMaxAmount = 2.0f;

/// An amount dragged back below this is GONE rather than a route sitting at zero — so the indicator
/// stops being drawn and the route list stops listing it. Also part of the contract, so that every
/// widget's "dragged back to nothing" means the same thing.
inline constexpr float kAmountUnroutedBelow = 0.03f;

/// **WHERE A CONTROL STANDS WITH THE HOST'S SLOTS** (backlog JU, host slots T7) — what a right-click lists
/// and what a knob marks. The pool's own vocabulary (`hostslots::SlotState`) restated at this tier, because a
/// device knows nothing of the pool: `Unavailable` is a host with no pool at all (the standalone rack until
/// T10, a nested rack no host has linked to a pool (a third party's chain), a control that is already a host
/// parameter), and a control there offers no
/// host entry rather than one that would do nothing.
struct HostSlotStatus {
	enum class State : uint8_t {
		Unavailable, ///< No pool behind this control. Nothing to offer.
		Unslotted,   ///< A pool, and no slot names this control yet.
		Default,     ///< On a slot the defaults filled; taken over when a slot is needed.
		Recent,      ///< On a slot a touch filled; recycled only when the pool is full.
		Pinned,      ///< "Expose to host". Only an unpin frees it.
		Anchored,    ///< The host wrote to it, or it was touched while recording. Only a release frees it.
	};
	State state = State::Unavailable;
	/// The slot, 0-based; meaningful only for the four slotted states.
	uint32_t slot = UINT32_MAX;
	/// For `Unslotted`: whether a pin would find a slot — false when every slot is Pinned or Anchored, and
	/// the entry is then shown switched off rather than offered (the honesty rule).
	bool canExpose = false;

	[[nodiscard]] bool slotted() const noexcept {
		return state != State::Unavailable && state != State::Unslotted;
	}
};

/// What any widget implements to become a modulation destination.
class Modulatable {
public:
	virtual ~Modulatable() = default;

	/// What this control IS: name, range, default, unit, whether it may be modulated. The widget
	/// needs the range to turn a drag into a value and the unit to print one; nothing else about it.
	virtual const ParamDesc& desc() const = 0;

	/// The base value, in the control's own units — the number the widget prints.
	virtual double value() const = 0;

	/// Set it. The implementation clamps and rounds by the descriptor, so a widget may hand over
	/// whatever a drag produced without knowing the control's limits.
	virtual void setValue(double value) = 0;

	/// **A MOMENTARY'S RELEASE** — `setValue`, except that a host behind it may keep the press up until a block has read it
	/// (`DeviceHost::releaseField`). `SwitchChip`'s momentary lets go through here.
	virtual void releaseValue(double value) { setValue(value); }

	/// Everything about how this control is being modulated, for the paint. See ModDisplay.
	virtual ModDisplay display() const = 0;

	/// **HOW FAR ONE NAMED SOURCE REACHES THIS CONTROL — the question `display()` deliberately cannot
	/// answer.**
	///
	/// `ModDisplay::amount` is about the FOCUSED source, for a stated reason: that is what a drag will
	/// edit and what a ring's length has to mean. The cost is written down beside it — every OTHER
	/// route is drawn as nothing, and `routedElsewhere` exists because a live
	/// route once went unnoticed, costing an evening, as a route that looked untouched. **That flag discloses THAT something else is routed; this answers
	/// WHAT.** Backlog DI, which is the fix for the problem that flag documents.
	///
	/// **DEFAULTED RATHER THAN PURE, so none of the seven implementors is forced to answer.** A
	/// control that cannot enumerate its routes reports no route from anybody, and a surface built on
	/// this then shows an empty list — which is the honest reading of "I don't know" and not a claim
	/// that nothing is routed. The alternative, a pure virtual, would have every adapter in the
	/// editor implementing a routing query for one menu.
	///
	/// ZERO MEANS NO ROUTE, exactly as `ModDisplay::amount` does: an amount dragged below
	/// `kAmountUnroutedBelow` is removed rather than kept at zero, so there is no live route this can report
	/// as nothing.
	/// **BY ENDPOINT SINCE KILL-LANES STEP 2**: a source is a device's output by the device's id, or one of
	/// the host's own sources — `device::Source`, the one type every surface and host speaks.
	virtual float amountFor(Source source) const {
		(void)source;
		return 0.0f;
	}

	/// Set how far the armed source reaches this control. Ignored when nothing is armed or when this
	/// is not a destination, so a widget may offer the drag without first asking whether it is legal.
	virtual void setAmount(float amount) = 0;

	/// GESTURE BRACKETING, and it is not optional decoration. It is what a host's automation
	/// recording latches onto and what the editor's undo groups a whole drag into — a widget that
	/// wrote values without it produces one undo step per mouse-move event.
	virtual void beginGesture() = 0;
	virtual void endGesture() = 0;

	/// THE SAME BRACKETING FOR AN AMOUNT, which is a different gesture on a different thing.
	///
	/// A route amount is not a parameter — this project says so in several places and the whole rack
	/// addressing exists because of it — so it has its own begin/end on the bridge and cannot ride the
	/// pair above. Defaulted to nothing so an implementation whose amounts need no bracketing is not
	/// forced to write two empty functions.
	///
	/// THIS WAS MISSING AND IT WAS A REAL HOLE. Knob brackets its amount drags with
	/// beginRouteGesture/endRouteGesture; RackControl and ValueBox did not, so the same gesture was
	/// bracketed on one surface and not on another. Generalizing Knob onto this interface without
	/// these would have silently dropped that bracketing from every knob in the editor — invisible to
	/// every suite we have, and the kind of thing that only shows up when a DAW records automation.
	/// Found while answering whether the interface could express what Knob already did; it could not.
	virtual void beginAmountGesture() {}
	virtual void endAmountGesture() {}

	/// **THE HOST'S HALF OF `beginGesture`/`endGesture` ALONE — no undo step** (host slots, T6b review). For a
	/// write that is one user action but already records its own undo some other way (a drawn editor's
	/// verb, one step for several fields), so bracketing it with the full pair would change what undo does. A
	/// ValueBox's double-click reset used to be the example and no longer is: no caller of its `edited_`
	/// recorded a step, so the reset could not be undone, and since backlog LS it takes the full pair. A
	/// control that is a host parameter tells the host Begin/End; the default, for one that is not, does nothing.
	virtual void beginHostGesture() {}
	virtual void endHostGesture() {}

	/// THIS CONTROL'S VALUE AS TEXT, printed by whoever knows how.
	///
	/// It cannot be shared arithmetic like normalized() below, because the two roads print
	/// differently ON PURPOSE: a control that is a host parameter prints through the plugin's own
	/// value-to-text, which is what the host shows in its automation lane, and a device control prints
	/// from its declared unit. A widget must not have to know which kind it is holding.
	virtual std::string text() const = 0;

	/// **WHY THIS CONTROL CHANGES NOTHING WHERE IT STANDS, or null when it does** — the honesty rule asked of the
	/// control's PLACE rather than its device: a Poly switch inside a Rack (backlog NO), where the host's voices do
	/// not reach. A widget over a control that answers greys and refuses the hand, and its tooltip is this line.
	[[nodiscard]] virtual const char* unavailable() const { return nullptr; }
	/// **THIS CONTROL'S TOOLTIP WHERE ITS SURFACE SAYS MORE THAN ITS DECLARATION** — a field whose meaning follows a Mode
	/// (Pattern's lane switches: a kit lane in Drum, "Mod N" in Modulation). Null: the declared tooltip.
	[[nodiscard]] virtual const char* tooltipOverride() const { return nullptr; }

	/// **THIS CONTROL AND THE HOST'S SLOTS** (host slots T7): where it stands, and the three verbs a
	/// right-click offers. DEFAULTED TO "NO POOL", for the reason `amountFor` is defaulted: a control whose
	/// host exposes nothing answers `Unavailable` and every verb refuses, so the menu lists no host entry
	/// and no implementor is forced to write four empty functions.
	///
	/// `exposeToHost` gives the control a slot and pins it (Empty, then an untouched Default, then the least
	/// recently used Recent — the last announced, so `displaced`, when not null, is set to what that slot
	/// named before, and left empty otherwise).
	/// `unpinFromHost` returns a Pinned slot to Recent. Each is false when it changed nothing. There is no
	/// release of an anchor: the host's lanes stay on the id whatever the plugin does.
	[[nodiscard]] virtual HostSlotStatus hostSlot() const { return {}; }
	virtual bool exposeToHost(std::string* displaced) {
		(void)displaced;
		return false;
	}
	virtual bool unpinFromHost() { return false; }
	/// Moves whenever `hostSlot()` may have changed its answer (the host's pool generation); constant for a
	/// control with no pool. What a knob caches its mark on.
	[[nodiscard]] virtual uint64_t hostSlotGeneration() const noexcept { return 0; }

	/// **TYPE-IN (backlog LS): THE TEXT A TYPED-VALUE FIELD OPENS ON, OR `nullopt` — THIS CONTROL REFUSES.**
	///
	/// A triple-click on a knob or a value box opens a field on the value; this is what it says, and it must
	/// read back through `parseText` to the value it was opened on. Asked of the control rather than worked
	/// out by the widget for `text()`'s reason: the roads print differently, so they parse differently.
	///
	/// **A REFUSAL IS NEVER A SILENTLY WRONG VALUE.** The default types in the declared unit
	/// (`device::editText`) and refuses whenever `text()` is not that unit's own reading — a control whose
	/// adapter prints some other way would otherwise take a number in a unit it is not showing. An adapter
	/// that prints its own way and can read its own text back overrides both.
	[[nodiscard]] virtual std::optional<std::string> editText() const;
	/// **WHAT WAS TYPED, AS A VALUE** in this control's units, clamped to its range — or `nullopt`, no value.
	/// `shown` is the value the field was opened on: a bare number is read in the unit the control was
	/// SHOWING, and for a duration that depends on the value (ms or s).
	/// Never called on a control whose `editText` refused.
	[[nodiscard]] virtual std::optional<double> parseText(std::string_view text, double shown) const;

	//-------------------------------------------------------------------//
	// Provided, not required — the shared arithmetic every widget wants. //
	//-------------------------------------------------------------------//

	/// The base as a 0..1 position along the control's own range, which is what a widget actually
	/// draws with. Non-virtual: there is one right way to compute it and no reason for two.
	double normalized() const;

	/// Where modulation has put it, on the same 0..1 sweep.
	double normalizedModulated() const;

	/// Where modulation has put the RIGHT EAR (backlog KU) — equal to `normalizedModulated()` unless a stereo route
	/// reaches this control (`ModDisplay::stereoLive`).
	double normalizedModulatedR() const;

	/// **WHERE THE VALUE ARC STARTS** on that sweep: the middle for a control whose declaration says
	/// its rest is its centre (`ParamDesc::centred` — a bipolar macro, backlog EG), the bottom for
	/// every other. A widget draws its arc between this and `normalized()`, whichever way round.
	double restNormalized() const;

	/// The span of the ring or bar an armed control should draw: `lo`..`hi` on the 0..1 sweep, from
	/// the base toward where a full-strength source would carry it. Clamped to the sweep, and
	/// ordered so `lo <= hi` whatever the sign of the amount.
	struct Span {
		float lo = 0.0f;
		float hi = 0.0f;
	};
	Span amountSpan() const;

	/// THE TWO GESTURES A DOUBLE-CLICK MAKES, expressed once in terms of the interface above.
	///
	/// Both were `Knob` members, and `Knob` is a dial. Everything else a hand can drag in this editor
	/// — a OWLTT threshold bar, a band's level strip, an MSEG breakpoint — reimplements the drag and
	/// then simply does not have these, so a double-click on it does nothing. For the OWLTT bars a
	/// double-click did not reset the setting, because there was
	/// nowhere for it to be except inside a widget that is not the one under the pointer.
	///
	/// They belong HERE for the same reason `normalized()` does: they are expressible entirely in the
	/// contract's own terms — `desc().def`, `setValue`, `setAmount` and the two bracketing pairs — so
	/// there is one right way to do each and no reason for two. A widget spends one line per gesture
	/// and gets the same undo bracketing every knob gets.
	///
	/// Each returns whether it CHANGED anything, so a caller can skip its announcement and its repaint
	/// on a no-op rather than testing the same condition a second time and drifting from this one.

	/// Put the value back to the descriptor's default, bracketed as one undoable step.
	bool resetToDefault();

	/// Drop the focused source's route to nothing, bracketed as one amount gesture.
	///
	/// NOT NAMED `unroute`, deliberately. `ui::ParamControl::unroute()` already exists, does more —
	/// it goes through the bridge's route table and records an undo step that knows how the route is
	/// addressed — and is NOT an override of anything, because this is non-virtual. A base method
	/// sharing that name would be silently shadowed, so a `Modulatable&` and a `ParamControl&` would
	/// mean different things by the same call. That is precisely the two-places-must-agree shape this
	/// method exists to close, and it would have been reintroduced by the name alone.
	bool dropRoute();
};

} // namespace feathers::device
