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

#include "backplane/device_host.h"
#include "device/modulatable.h"
#include "device/registry.h"
#include "plugin/ui/card_selection.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/knob.h"
// **FOR `outputChipRowsFor`** — how many rows of chips this card's device needs, which is what the
// card must reserve at the foot. It is beside `SourceArm` because a chip's width is the arm's own
// measurement of the output's name; see that header's note on the three hosts.
#include "plugin/ui/source_arm.h"
#include "plugin/ui/card_meter.h"
#include "plugin/ui/theme.h"
#include "plugin/ui/mod_readout_layer.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

// WHAT THE FX LANE NEEDS FROM A DEVICE'S CARD, WHICH IS ALMOST NOTHING.
//
// THE ONE CARD BASE. It was one of two — see the retired rack_card.h — and the lane places cards,
// shows and hides them, and asks how tall each one is. Everything else a card does is the device's
// own — which is the promise device.h makes and the reason `hasCard` exists.
//
// **THE TWO CHAINS USED TO DIVIDE THE HEAD DIFFERENTLY, AND THEY NO LONGER DO.** This paragraph said
// the modulator strip drew the head row itself and asked a card only for its BODY, which is why there
// were two height questions — `bodyHeight()` beside `laneHeight()`. Every card frames itself now
// (`ui::drawCardFrame`, and `ModPanel::resized`'s *"THE WHOLE RECTANGLE, because the card draws its
// own frame now"*), so the division stopped existing; `bodyHeight()` outlived it as a virtual nobody
// overrode, defined as `laneHeight()`, whose only caller wanted the whole card anyway.
//
// **IT IS GONE, AND DELETING IT IS PART OF A FIX RATHER THAN TIDYING.** A card's height depends on its
// WIDTH — the outputs foot is as many rows as the chips need — so every width-blind way to ask for one
// is a way to be told about a card that has not been placed yet. That cost a seven-output device a
// one-row foot in the modulator strip. One question now, and it takes the width: `laneExtent(cross)`.
//
// SO THE HEIGHT HERE IS THE WHOLE CARD, and a card that folds reports its folded height. The lane
// stacks whatever it is told without knowing what folding is.

namespace feathers::ui {

/// **THE ONE GREYING FOR EVERY CARD — A CHILD ON TOP, NOT A FILL UNDERNEATH.**
///
/// Backlog CY: devices had different semantics for "disabled grey" — some greyed their knobs, some
/// (OWLTT among them) did not. The decision: a disabled device's card greys as a whole.
///
/// **WHAT WAS THERE, MEASURED ON f33e3d3 WITH A REAL POINTER, TEN CARD TYPES.** `FxCard::draw` filled
/// the body with a scrim at its end — "one dimming for every card, drawn last over the body". Last
/// over the body the CARD draws, and visage paints CHILDREN after the parent: so what a card drew
/// itself dimmed (the filter's curve, the shaper's, the drawn pad, the MSEG's points) and every child
/// frame did not — every knob's arc, every value box, OWLTT's whole graph, Automod's strip, the
/// tracker's knobs. Three cards (Filter, Harm, Shaper) then greyed their own knobs by hand with
/// `setActive`, which is the guard asymmetry: a mechanism nobody can forget beside one that three
/// remembered. The sweep table is in the CY entry.
///
/// **A CHILD FRAME `setOnTop(true)` PAINTS OVER EVERY SIBLING WHATEVER ORDER THEY WERE ADDED IN** —
/// `visage::Layer` draws on-top regions in a pass of their own (layer.cpp) — so this is the old scrim
/// moved from under the children to over them, and nothing per widget. It ignores the mouse, so the
/// hand still reaches the controls exactly as it did (the cards' own `setActive` calls already keep
/// a switched-off knob live; nothing here changes that). Its bounds are the body and its visibility
/// is `!headEnabled()`, both refreshed by `FxCard::syncDisabledVeil` from the two roads every card
/// already travels — `update()` per frame and `draw()` — so no card has anything to remember.
class DisabledVeil final : public visage::Frame {
public:
	DisabledVeil() {
		setOnTop(true);
		setIgnoresMouseEvents(true, false);
		setVisible(false);
	}
	void draw(visage::Canvas& canvas) override {
		canvas.setColor(kDisabledVeil);
		canvas.fill(0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()));
	}
};

class FxCard : public visage::Frame, public TooltipHost, public TooltipSource {
public:
	FxCard() {
		// THE VEIL FIRST, so it exists before any card adds a control — and on top regardless, which
		// is the point of it being a child with `setOnTop` rather than a fill in `draw()`.
		addChild(&veil_);
		// **AND THE COURTESY OUTPUT METER** (backlog MK) — in the head, so the veil (the body) never covers it, and in the
		// BASE, so every card in every lane has one and none can forget it. See `syncOutputMeter`.
		addChild(&meter_);
		// **AND THE TOOLTIP LAYER STRAIGHT AFTER IT** — on top too, and added later, so it paints over
		// the veil: a disabled device's controls still say what they are. In the BASE, so no card can
		// be without it; see `ui/tooltip.h`.
		addChild(&tips_);
		tips_.watch(*this);
		// AND THE AS-MODULATED READOUT'S LAYER ABOVE IT (backlog PU) — the box beside a destination whose route amount is
		// being set. In the base for the tooltip's reason: no card can be without it.
		addChild(&modReadout_);
		modReadout_.watch(*this);
		// **SIZED WHENEVER THE CARD IS, WITHOUT ANY CARD CALLING THE BASE.** Every card overrides the
		// virtual `resized()`, so a base override would need ten `FxCard::resized()` calls that ten
		// cards could forget. visage's `onResize()` is a callback LIST whose first entry is the
		// virtual; adding a second runs beside it on every `setBounds`. MEASURED without this: "create()
		// positions every child before anything can draw" went red on the veil at 0x0 — a zero-size
		// child is the shape that paints at the window origin, and `draw()`-time sizing is too late
		// for that case's question.
		// THE METER'S TRACK WITH IT — only the track: the peak is read on `update()`, never from a resize, which can land
		// inside a card's own constructor before the host it would ask is in place.
		onResize() += [this] {
			syncDisabledVeil();
			placeOutputMeter();
		};
		// AND WHENEVER THE SCALE CHANGES, before any beat: the scale is stamped on the veil before this
		// fires, so the bounds it re-sets are converted at the new scale. See `syncDisabledVeil`.
		veil_.onDpiChange() += [this] { syncDisabledVeil(); };
		meter_.onDpiChange() += [this] { placeOutputMeter(); };
		// THE DRAG AND THE RELEASE ARE THE BASE'S, the press is the card's — see noteCardPress.
		onMouseDrag() = [this](const visage::MouseEvent& e) { noteCardMove(e); };
		onMouseUp() = [this](const visage::MouseEvent& e) { noteCardRelease(e); };
		// A HOVER MOVE IS DELIVERED ONLY WITH NO BUTTON HELD, so a card that still thinks a press is live has lost its
		// release (the window was left, a menu took the mouse-up): the gesture is cancelled, never completed.
		onMouseMove() += [this](const visage::MouseEvent&) { cancelCardDrag(); };
		// **SO THAT A PRESS ON ONE OF THIS CARD'S CONTROLS STILL MOVES THE KEY'S TARGET TO THIS CARD.**
		//
		// The bug: deleting a modulator with Backspace did not work, while the menu's remove did at once.
		// Both roads call one function; what differed was the argument.
		// `onCardPressed` — which is what tells a lane which slot a key acts on — fires from
		// `noteCardPress`, and that runs only for a press the CARD frame itself receives.
		// `visage::Frame::propagateMouseEvent` hands a press to an ancestor only if the ancestor asked
		// with `setReceiveChildMouseEvents`, and nothing in this plugin does. **A modulator's card is
		// mostly its value boxes and a filter's is mostly its knobs**, so the commonest press on a card
		// went to a child, the card never heard it, and `focusedSlot_` went on naming the card whose
		// bare body was pressed LAST. MEASURED: press the DAHDSR's first box with the filter focused
		// and the editor reports focus 0, not 1; Backspace then removed the filter and left the
		// modulator still routed at 0.5.
		//
		// **KEYSTROKES ARE ACCEPTED SO THAT FOCUS LANDS HERE, NOT SO THAT KEYS ARE ANSWERED HERE.**
		// `handleMouseDown` sets the keyboard focus to the nearest keystroke-accepting ANCESTOR of the
		// frame under the pointer, so a card that accepts them is the frame a press on its knob
		// focuses. No key is handled at this level: `processKeyPress` on a frame with no handler
		// returns false, and `handleKeyDown` then walks on to the lane, which is where Delete and
		// Backspace live and where they stay.
		//
		// **AND THE HOOK CARRIES NO COORDINATES, WHICH IS WHY IT IS THE RIGHT ONE.** The other way to
		// hear a child's press is `setReceiveChildMouseEvents(true)`, and that delivers the event with
		// the CHILD's `position` — a knob-local point that `noteCardPress` would read as a card-local
		// one and could dispatch as a press on the head's expand mark or its slot menu. A focus change
		// says only that the hand arrived.
		setAcceptsKeystrokes(true);
	}

	/// **FOCUS FOLLOWS THE HAND, INCLUDING THE HAND THAT LANDED ON A CONTROL.** See the constructor.
	///
	/// `was_clicked` GATES IT: this also fires when something moves the focus programmatically, and a
	/// key's target must be a thing the hand chose. Losing focus says nothing — the lane clears its own
	/// target when the device goes, and a press that lands outside every card must not silently retarget
	/// anything.
	void focusChanged(bool is_focused, bool was_clicked) override {
		if (is_focused && was_clicked && onCardPressed) {
			onCardPressed();
		}
	}

	/// **THE CARD'S DRAG.** Public for the same reason `noteCardRelease` is: a card that needs its OWN
	/// `onMouseDrag` must CHAIN to this rather than replace it.
	///
	/// `EnvCard` and `MsegCard` replaced it — each has a curve or a handle to drag — and so the road
	/// that reports `onCardDrag` never ran on those cards. **That is why reorder did nothing in the
	/// modulator strip even after the panel bound the callback**: the callback was correct and nothing
	/// ever called it. Third handler on those two cards found taking the whole seat, after `onMouseUp`
	/// and before anyone thought to look at `onMouseDown`.
	void noteCardMove(const visage::MouseEvent& e) {
			if (!anchor_.has_value()) {
				return;
			}
			// THE ANCHOR IS RE-BASED BY HOWEVER FAR THE CARD ITSELF MOVED.
			//
			// `e.position` is recomputed by the host on every move against the frame's CURRENT place
			// — `window_event_handler.cpp:257` is `window_position - mouse_down_frame_->
			// positionInWindow()`. So anything that moves the card under a live press shifts
			// `e.position` by that distance with NO hand movement. Folding does exactly that: the
			// lane re-places everything below the fold and the scroll re-clamps, which here is up to
			// 226px against a 4px threshold. The card was then picked up and the chain REORDERED by
			// a click that never moved. Found by wp-card sweeping the fold verb, on all four cards.
			//
			// Subtracting the frame's own displacement leaves the HAND's movement, which is what the
			// threshold is about. Done here rather than by anchoring in window coordinates because
			// `e.window_position` is not set by any of this suite's synthesized events — and a fix
			// that only works when the host fills a field the tests do not is a fix no case can hold.
			// **MOTION IS WHAT SEPARATES A DRAG FROM A FOLD — not the click count.**
			//
			// This required `armed_` (a double-click) before a drag could start, on the reasoning that
			// otherwise "the fold and the reorder share one press again". That was TRUE while the fold
			// happened on the PRESS. It stopped being true when the fold moved to the RELEASE: a fold
			// is now `armed && never moved`, so anything that MOVES is already not a fold and the
			// arming gate buys nothing.
			//
			// **AND IT COST THE GESTURE ON REAL HARDWARE.** On a trackpad a click-and-hold needed two taps,
			// where a mouse needs one plain click and hold — and that is the whole bug: the trackpad's tap-tap-hold is synthesised by the
			// DRIVER into an ordinary press-and-hold, so what reaches the plugin is a press whose count
			// is not something we may rely on — and with a mouse it is definitively 1. Requiring 2 made
			// reorder unreachable with a mouse and unreliable with a trackpad.
			//
			// So: any press may become a drag once it moves past the threshold, and the double-click
			// still folds because a fold requires that it did NOT move.
			// **A MAGNITUDE, AND A CORRECTION PER AXIS — BOTH HALVES, BECAUSE BOTH WERE Y-ONLY.**
			//
			// THE THRESHOLD MEASURED Y ALONE, so in a lane that flows in X — the rack's — a perfectly
			// horizontal drag never passed it and reorder simply did not start. The hand gets the bumper
			// because a hand on a trackpad drifts; a mouse on a rail would not, and would have found
			// the gesture dead.
			//
			// **AND THE CORRECTION ABOVE WAS Y-ONLY TOO, WHICH IS THE HALF A MAGNITUDE THRESHOLD
			// ALONE WOULD HAVE LEFT BROKEN.** `carried` subtracts however far the CARD moved under a
			// live press, so a layout change cannot masquerade as hand movement — the fix for a fold
			// that picked the card up and reordered the chain by a click that never moved, worth up
			// to 226px against a 4px threshold. With only a Y term, making the threshold a magnitude
			// would have fixed the hand's axis and left the card's: a card re-placed HORIZONTALLY
			// under a held press would read as horizontal hand movement, and fold-reorders-by-accident
			// would come back sideways in the very lane this change is for.
			//
			// TODAY'S VERTICAL BEHAVIOUR IS A STRICT SUBSET. A purely vertical drag has dx == 0, so
			// `hypot(0, dy)` is `|dy|` — the number this line compared before — and the modulator
			// strip's gesture, which is the one that has been tested by hand, does not move.
			const float carriedX = positionInWindow().x - anchorFrameX_;
			const float carriedY = positionInWindow().y - anchorFrameY_;
			const float movedX = (e.position.x + carriedX) - anchor_->x;
			const float movedY = (e.position.y + carriedY) - anchor_->y;
			if (!dragging_ && std::hypot(movedX, movedY) < kDragThreshold) {
				return;
			}
			const bool starting = !dragging_;
			dragging_ = true;
			if (onCardDrag) {
				if (starting) {
					onCardDrag(DragPhase::Begin, e.position);
				}
				onCardDrag(DragPhase::Move, e.position);
			}
	}

	/// **THE CARD'S RELEASE BOOKKEEPING, AND THE FOLD.** Public so a card that needs its OWN
	/// `onMouseUp` can CHAIN to it rather than replace it.
	///
	/// `EnvCard` and `MsegCard` did replace it — `onMouseUp() = [...] { draggingCurve_ = false; }` —
	/// which silently dropped everything below: the drag anchor, `dragging_`, `armed_`, and (once the
	/// press-time road was retired) the fold itself. A handler that is assigned rather than chained
	/// takes the whole seat.
	/// ENDS A PRESS WHOSE RELEASE NEVER ARRIVED, without the drop `noteCardRelease` would make: the host hears `Cancel`
	/// if a drag had begun. Answers whether a gesture was live.
	bool cancelCardDrag() {
		const bool live = anchor_.has_value() || dragging_;
		const bool was = dragging_;
		anchor_.reset();
		dragging_ = false;
		armed_ = false;
		if (was && onCardDrag) {
			onCardDrag(DragPhase::Cancel, visage::Point {});
		}
		return live;
	}

	void noteCardRelease(const visage::MouseEvent& e) {
			const bool was = dragging_;
			const bool armed = armed_;
			const bool pressedHere = anchor_.has_value();
			anchor_.reset();
			dragging_ = false;
			armed_ = false;
			if (was && onCardDrag) {
				onCardDrag(DragPhase::End, e.position);
				return;
			}
			// A CLICK THAT DID NOT DRAG (backlog OA): a plain press inside a selection of several collapses it here,
			// not at the press, so the press could still have picked the whole block up.
			if (pressedHere && !was && onCardClicked) {
				onCardClicked();
			}
			// **ARMED AND NEVER MOVED: THAT IS THE COLLAPSE — AND IT IS NOW THE ONLY ROAD.**
			//
			// A double-click that turned into a drag has already been handled above and must not also
			// fold: the two verbs share an arming gesture and separate themselves on MOTION, which is
			// the whole point of the split and is the design exactly: a double-click expands, a
			// double-click-and-hold drags.
			//
			// **THE PRESS-TIME ROAD IS GONE.** Each card used to call `foldOnHeadPress()` from its own
			// `mouseDown`, which folded before anyone could know whether the gesture would become a
			// drag. Where BOTH roads were live — the rack, which sets `onHeadFold` and keeps this
			// handler — a double-click toggled twice and landed where it started, which is the
			// headers-do-not-collapse bug surviving the first fix. One road cannot disagree with
			// itself.
			//
			// `setFolded` WHEN THE HOST OFFERS NO FOLD OF ITS OWN — a card built outside a host that
			// binds `onHeadFold` (every lane binds it now: Feathers' fx lane since 2026-10-04, when it
			// began holding modulators whose fold the patch keeps), whose own `folded_` is then the state.
			if (armed && !was) {
				if (onHeadFold) {
					onHeadFold();
				}
				else {
					setFolded(!folded());
				}
			}
	}

	/// **HOW TALL THIS CARD IS IN THE LANE, HEAD INCLUDED, IF IT WERE `cardWidth` WIDE.** A card that
	/// can fold answers with its current height, not its open one: the lane stacks what it is told, and
	/// a fold that did not reach this number would leave a hole where the card used to be.
	///
	/// **THE WIDTH IS A PARAMETER AND THE FUNCTION IS PURE, AND BOTH OF THOSE ARE THE FIX.** This was
	/// `laneHeight()` with no argument, and every implementation that needed a width reached for
	/// `width()` — the card's own, which is ZERO until something has placed it. A card's height CAN
	/// depend on its width: the outputs foot is as many rows as the chips need, and the chip-wrap walk
	/// cannot wrap in a strip of zero width, so it reports ONE row however many chips a device
	/// publishes. `ModPanel::resized` asked it of a card `ensureCard` had just constructed and gave a
	/// seven-output device a 146px card where it needed 190.
	///
	/// **AND THE REASON IT IS PURE RATHER THAN DEFAULTED IS THE SECOND HALF OF THAT BUG.** A default
	/// would be width-blind, every card that did not think about the question would silently take it,
	/// and the ones that most need the width are exactly the ones nobody would think about — which is
	/// what `laneExtent`'s own default did (see it). A card whose height does not depend on its width
	/// says so by leaving the parameter UNNAMED, which is a statement a reader can check rather than a
	/// silence they have to interpret.
	///
	/// **A HOST ASKS `laneExtent(cross)`**, never this directly: the lane knows its own cross and a
	/// horizontal lane wants a width back rather than a height.
	virtual float laneHeightAt(float cardWidth) const = 0;

	/// The same question about the card AS IT STANDS — **for draw time and for scrolling, not for a
	/// host laying anything out.** Non-virtual, so there is exactly one place the current width enters
	/// and a card cannot accidentally override the width-blind form instead of the real one.
	float laneHeight() const { return laneHeightAt(static_cast<float>(width())); }

	/// HOW TALL THIS CARD'S CONTENT WANTS TO BE, ignoring what it was given.
	///
	/// **THE UNIT RULE NEEDS TWO NUMBERS AND THERE WAS ONLY ONE.** `laneHeight()` used to be both "how
	/// much room I need" and "how much room I have", which is fine while a lane grants whatever is
	/// asked. It no longer does: a card occupies `kCardUnitHeight`, pads if its content is shorter and
	/// SCROLLS if longer, so the natural height has to survive as its own answer.
	virtual float contentHeight() const { return laneHeight(); }

	/// HOW FAR THE BODY IS SCROLLED, in pixels, and how far it CAN be.
	///
	/// **THE FRAME DOES NOT SCROLL — only the body does.** The head carries the device's name and the
	/// host's marks, the strip along the foot carries the outputs; both must stay put or a card that
	/// scrolled would lose the very affordances that say what it is. `ui::cardFrameOf` already draws
	/// that line, and this is what makes it load-bearing rather than decorative.
	float bodyScroll() const noexcept { return bodyScroll_; }

	/// **THROUGH `ownFrame()`, WHICH IS THE CLAMPED STRIP — NOT `outputStripHeight()`, WHICH IS NOT.**
	///
	/// This subtracted `outputStripHeight()`, the raw `22 * outputRowsAt(width())`, from both terms.
	/// But the strip a card actually GIVES UP is `cardFrameOf`'s, and that one is clamped:
	/// `strip = min(wanted, room)` (theme.cpp), because a card too short to hold its whole foot keeps
	/// the foot inside itself rather than letting it hang. `drawBody` clips to `ownFrame().body` and
	/// `syncDisabledVeil` sizes to it, so the picture uses the clamped number and this used the raw
	/// one. They agree everywhere the clamp does not fire and diverge by exactly the clamped-away
	/// amount where it does — the rack at a short lane — and the card then under-reports how far its
	/// body can scroll, which is content a hand cannot reach.
	///
	/// **`ownFrame()`'S OWN NOTE CALLS ITSELF "one expression, so a card's clip, its veil and its
	/// strip are the same rectangles by construction."** The scroll extent is the fourth thing that
	/// has to agree with them and was the one that did not. Asked of the same rectangle, it cannot.
	float maxBodyScroll() const {
		const float strip = ownFrame().outputs.height();
		const float visible = std::max(0.0f, static_cast<float>(height()) - kCardHeadHeight - strip);
		const float wanted = std::max(0.0f, contentHeight() - kCardHeadHeight - strip);
		return std::max(0.0f, wanted - visible);
	}

	void setBodyScroll(float to) {
		const float clamped = std::clamp(to, 0.0f, maxBodyScroll());
		if (clamped == bodyScroll_) {
			return;
		}
		bodyScroll_ = clamped;
		redraw();
	}

	/// Whether this card has anything to scroll — what the lane asks before drawing a bar.
	bool scrollable() const { return maxBodyScroll() > 0.0f; }

public:
	/// **HOW WIDE THE OUTPUTS STRIP IS ON A CARD THIS WIDE** — the full width under a top head, and
	/// the width less the spine under a left one, which is exactly what `cardFrameOf` gives it.
	/// Stated once here because three answers depend on it and none of them should transpose it again.
	float outputStripWidthAt(float cardWidth) const {
		return headEdge() == CardHeadEdge::Left ? std::max(0.0f, cardWidth - kCardHeadHeight)
		                                        : std::max(0.0f, cardWidth);
	}

	/// **HOW MANY ROWS THIS CARD'S OUTPUT CHIPS NEED IF THE CARD WERE `cardWidth` WIDE.**
	///
	/// **THE WIDTH IS AN ARGUMENT BECAUSE THE LANE ASKS BEFORE THE CARD HAS ONE.** `laneExtent(cross)`
	/// is "how much flow do you need given this much cross", and answering it from `width()` would
	/// read the width the card had LAST layout — zero on the first, which reports one row for seven
	/// chips and under-reserves the card by 22px a row. The same shape as every other "asked before it
	/// was placed" bug in this tree; see `ModPanel::outputChipWidth`.
	size_t outputRowsAt(float cardWidth) const {
		const device::Descriptor* d = descriptor();
		return d != nullptr ? outputChipRowsFor(*d, outputStripWidthAt(cardWidth), outputLabels(), outputsShown()) : 0;
	}

	/// **WHICH OUTPUTS THIS CARD SHOWS** (`DeviceHost::outputsShown`): all, unless its device says otherwise.
	uint64_t outputsShown() const {
		const backplane::DeviceHost* host = deviceHost();
		return host != nullptr && device() != kNoInstance ? host->outputsShown(device()) : ~uint64_t { 0 };
	}

	/// **WHAT EACH OUTPUT IS CALLED ON THIS INSTANCE** — the host's label (`DeviceHost::outputLabel`), in
	/// output order; empty for a card that knows no host or no device, whose chips then measure the
	/// declared names. What the strip reserves and what the host's arms print are then one word.
	std::vector<std::string> outputLabels() const {
		const device::Descriptor* d = descriptor();
		const backplane::DeviceHost* host = deviceHost();
		std::vector<std::string> labels;
		if (d == nullptr || host == nullptr || device() == kNoInstance) {
			return labels;
		}
		labels.reserve(d->outputCount());
		for (size_t i = 0; i < d->outputCount(); ++i) {
			labels.push_back(host->outputLabel(device(), i));
		}
		return labels;
	}
	/// The same question about the card as it stands.
	size_t outputRows() const { return outputRowsAt(static_cast<float>(width())); }

	/// THE STRIP'S HEIGHT FOR THIS CARD — zero for a device that publishes nothing, and ONE ROW PER
	/// ROW OF CHIPS, which is what keeps a seven-output device's chips on the card rather than off
	/// the left edge of it (all of a device's outputs belong on the card).
	float outputStripHeightAt(float cardWidth) const {
		return kCardOutputStripHeight * static_cast<float>(outputRowsAt(cardWidth));
	}
	float outputStripHeight() const { return outputStripHeightAt(static_cast<float>(width())); }

	/// **THIS CARD'S OWN FRAME, IN ITS OWN COORDINATES — head, body and outputs strip.**
	///
	/// Every call in this file spelled the same four arguments out, and each was one edit away from
	/// disagreeing with the others: the row count arrived here as a fourth argument and three of the
	/// four sites would have kept the default. One expression, so a card's clip, its veil and its
	/// strip are the same rectangles by construction.
	CardFrame ownFrame() const {
		const float w = static_cast<float>(width());
		const float h = static_cast<float>(height());
		return cardFrameOf({ 0.0f, 0.0f, w, h },
		                   descriptor() != nullptr ? descriptor()->outputCount() : 0, headEdge(),
		                   outputRows());
	}

	/// **THE HEADER IS THE BASE'S JOB AND THE CONTENT IS THE CARD'S.**
	///
	/// A card MAY override the header, but the API's happy path is clear enough that future
	/// implementations draw the standard one.
	///
	/// So `draw()` is implemented HERE: it paints the shared frame, then calls `drawContent()` inside
	/// `drawBody()`. **A card that does nothing gets the standard header for free and cannot paint
	/// over it** — the first because it does not draw one, the second because the body is clamped.
	/// That is the happy path being the DEFAULT rather than a convention a new card has to be told
	/// about; the eight cards that each restated a surface, a border and a name are what it replaces.
	///
	/// **AND THE OVERRIDE IS STILL THERE, DELIBERATELY.** A device whose head genuinely needs to be
	/// different overrides `draw()` itself and takes the whole rectangle. That is an opt-OUT, visible
	/// at the point it happens, rather than the current arrangement where every card silently opts in
	/// by writing its own.
	void draw(visage::Canvas& canvas) override {
		const device::Descriptor* desc = descriptor();
		const HeadLayout head = headLayout();
		drawCardFrame(canvas, { 0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()) },
		              head.name.c_str(), head.nameBox, desc != nullptr ? desc->outputCount() : 0,
		              frameStyle(), tintedHeadMarks(), headEdge());
		drawHeadCaption(canvas, head);
		// **THE HEAD'S OWN EXTRAS, OUTSIDE THE BODY'S CLAMP.** A card's slot menu and its `+N` expand
		// mark live in the head, and `drawBody` clips to the body — so a card drawing them in
		// `drawContent` would draw them nowhere. They are not `CardHeadMarks` because those are the
		// HOST's affordances (fold, menu) while these are the card's own, and the count in `+N` is a
		// question only the card can answer.
		drawEnableSwitch(canvas);
		drawExpandMark(canvas);
		drawSiteMark(canvas);
		drawHeadExtras(canvas);
		if (folded()) {
			// A FOLDED CARD IS ITS HEAD AND NOTHING ELSE — but the head's extras are still drawn above,
			// which is what keeps a folded card's menu reachable.
			//
			// **AND NOTHING MORE THAN ITS HEAD.** There was a hook here for a folded card to print one
			// more line into its head — `GenericCard` wrote "21 controls" — and it ran under the enable
			// square on every generic card (Delay, Eroder, Pitch, SKLITTER, the tracker). The caption was
			// not needed at all, so it went (2026-09-23), and the hook went with it: the head's text is `headLayout()`'s, and nothing else writes there.
			return;
		}
		drawBody(canvas, [&] { drawContent(canvas); });

		// **ONE DIMMING FOR EVERY CARD — AND IT IS A CHILD ON TOP, NOT A FILL HERE.**
		//
		// Disable behaved inconsistently: sometimes the card dimmed but the enable light stayed on,
		// sometimes the card did not grey at all. Both halves were real and they had
		// different causes. The light was `headEnabled()` returning a hardcoded `true` — fixed at its
		// source. The greying was per-card: **`FilterCard` was the ONLY card that implemented
		// `showEnablement`**, so one card in ten dimmed and the rest did not.
		//
		// A SCRIM RATHER THAN NINE MORE IMPLEMENTATIONS — and then the scrim was a fill at the end of
		// this function, which dimmed only what THIS function had painted: visage paints children
		// after the parent, so every knob, value box and graph child sat on top of it untouched.
		// MEASURED on ten card types (backlog CY): OWLTT's graph and knobs, Automod's strip and knobs,
		// the tracker's knobs, every card's value boxes — none dimmed. See `ui::DisabledVeil` for
		// the road that cannot be got wrong per widget; this line only keeps it in step.
		//
		// THE HEAD IS NOT DIMMED, deliberately: the switch that turns the device back on lives there,
		// and greying the way back out is how a control becomes hard to find at the moment it matters.
		syncDisabledVeil();
	}

	/// **THE VEIL'S TWO FACTS, REFRESHED FROM ONE PREDICATE.** Bounds = the body rectangle of the
	/// shared frame (never the head); visible iff `!headEnabled()`. Called from `draw()` and from
	/// `update()` — the per-frame road every host already drives — so a toggle from the head, an
	/// undo, a patch load or automation of the enable field all reach it without any card knowing.
	/// Public so a case can drive it the way the frame does and read the value the paint reads.
	void syncDisabledVeil() {
		const visage::Bounds body = ownFrame().body;
		// **UNCONDITIONALLY, AND THE GUARD THAT STOOD HERE WAS THE DEFECT.** It re-set the bounds only
		// when the LOGICAL body moved, and a scale change moves nothing logical: `Frame::setDpiScale`
		// stamps the new scale and leaves a frame's native rectangle where it was, so every frame in the
		// tree is put right by the layout cascade that follows — and this one was not, because its
		// cache said nothing had changed. The veil then painted at the OLD scale's size: 1/1.75 of the
		// card, photographed on the xvfb venue, and seen in Bitwig the moment the canvas
		// resized the editor (2026-09-14: the inactive veils were wrong after a resize).
		// `setBounds` dedups on both the logical and the native rectangle itself, so this costs nothing
		// when nothing moved and is the fix when only the scale did.
		veilBody_ = body;
		veil_.setBounds(body.x(), body.y(), body.width(), body.height());
		const bool want = !headEnabled();
		if (veil_.isVisible() != want) {
			veil_.setVisible(want);
		}
	}

	/// THE VEIL ITSELF, for a case: its visibility is the value `headEnabled()` paints from, its
	/// `isOnTop()` is the whole mechanism, and its bounds are the body.
	const DisabledVeil& disabledVeilForTest() const { return veil_; }

	/// **THE COURTESY OUTPUT METER'S TWO FACTS, REFRESHED FROM THE HOST** (backlog MK) — the veil's shape: its track
	/// (`cardMeterTrack`, the head's inner edge, so a folded card still shows it) and its marks, from the peak the host
	/// answers for this card's device. A host that cannot say (`outputPeakAt` declines: a modulator, a strip slot, a
	/// device that has not run) hides the line rather than drawing a silent one. True when the marks moved — the
	/// meter frame has asked for its own frame by then, so the card under it is not repainted.
	bool syncOutputMeter() {
		const visage::Bounds track = placeOutputMeter();
		float peak = 0.0f;
		const backplane::DeviceHost* host = deviceHost();
		const bool metered = host != nullptr && host->outputPeakAt(device(), peak);
		if (meter_.isVisible() != metered) {
			meter_.setVisible(metered);
		}
		meterPeak_ = metered ? peak : -1.0f;
		const bool vertical = headEdge() == CardHeadEdge::Left;
		return meter_.setMarks(cardMeterMarksFor(vertical ? track.height() : track.width(), vertical,
		                                         cardMeterReadingFor(metered ? peak : 0.0f)));
	}
	/// THE METER'S TRACK, SET ON THE FRAME — UNCONDITIONALLY, for `syncDisabledVeil`'s reason: a scale change moves
	/// nothing logical, and `setBounds` dedups.
	visage::Bounds placeOutputMeter() {
		const visage::Bounds track =
		    cardMeterTrack({ 0.0f, 0.0f, static_cast<float>(width()), static_cast<float>(height()) }, headEdge());
		meter_.setBounds(track.x(), track.y(), track.width(), track.height());
		return track;
	}
	/// THE METER FRAME ITSELF, for a case: its bounds are the track, its visibility whether the host meters this device,
	/// and its marks what it paints.
	const CardOutputMeter& outputMeterForTest() const { return meter_; }
	/// The peak the meter last read, or negative when the host declined.
	[[nodiscard]] float outputMeterPeakForTest() const noexcept { return meterPeak_; }

	/// WHAT THIS CARD DRAWS INSIDE ITS BODY. Measured in the card's own coordinates; the clamp and the
	/// scroll are applied around it.
	virtual void drawContent(visage::Canvas&) {}

	/// **THE WAY INTO THE PANEL, DRAWN ONCE.** The expand-to-window/canvas box was missing from the
	/// vertical headers. Four cards drew their own, all in top-head coordinates, so none of them
	/// appeared on a spine. Geometry and not a glyph — this font has no guaranteed expand character and
	/// a missing glyph is an invisible control, which is the standing rule the copies also followed.
	/// **THE PRESS THAT OPENS THE PANEL, ONCE.** Six cards wrote this branch; the seventh spelling of
	/// it would have been the spine's. Returns whether the press was consumed, so a caller keeps its
	/// existing `else if` chain.
	bool expandAt(visage::Point position) {
		if (!expandBounds().contains(position)) {
			return false;
		}
		if (onExpand) {
			onExpand();
		}
		return true;
	}

	/// GENERIC DRAWS A COUNT INSTEAD (`+106`), which is a different promise from an arrow and must not
	/// be drawn over by one.
	virtual bool drawsOwnExpandMark() const { return false; }

	void drawExpandMark(visage::Canvas& canvas) {
		if (drawsOwnExpandMark()) {
			return;
		}
		// **DRAWN FOLDED TOO**: a folded card goes straight to its window without
		// unfolding first. The press was never gated on the fold — `expandBounds` does not ask — so
		// a folded card had an invisible button; the head's other occupants already stop short of it.
		const visage::Bounds ex = expandBounds();
		if (ex.width() <= 0.0f) {
			return;
		}
		const float inset = 2.0f;
		const float l = ex.x() + inset;
		const float r = ex.right() - inset;
		const float t = ex.y() + inset;
		const float b = ex.bottom() - inset;
		constexpr float kHead = 3.2f;
		canvas.setColor(kMuted);
		// A DIAGONAL WITH AN ARROWHEAD AT EACH END — the same mark HARM, OWLTT and Waypoint each drew,
		// because it is the same verb and a player should not have to learn it twice.
		canvas.segment(l, b, r, t, 1.4f, true);
		canvas.segment(r, t, r - kHead, t, 1.4f, true);
		canvas.segment(r, t, r, t + kHead, 1.4f, true);
		canvas.segment(l, b, l + kHead, b, 1.4f, true);
		canvas.segment(l, b, l, b - kHead, 1.4f, true);
	}

	/// THE CARD'S OWN MARKS IN THE HEAD — drawn unclipped, before the body. Default nothing.
	virtual void drawHeadExtras(visage::Canvas&) {}

	/// IS THIS DEVICE IN THE SIGNAL — what the head's switch shows. A device with no switch is always
	/// in, and answering true keeps the head honest: the mark is drawn only when `hasEnable()`.
	/// **IS THIS DEVICE SWITCHED ON — READ FROM THE FIELD, not assumed.**
	///
	/// This returned `true` unconditionally, so the head's square drew LIT on every card that did not
	/// override it — which was all of them but OWLTT, so a dimmed card could still show its enable
	/// light on. The light was not reporting anything; it was a constant.
	///
	/// ASKED OF THE HOST, so there is ONE answer and it is the same number the engine reads. A card
	/// that cached it would be a second copy to keep in step, which is the shape this file has spent
	/// two days removing.
	///
	/// TRUE WHEN THERE IS NO ENABLE, because a device that declares none is always on — the square is
	/// not drawn at all in that case, so the value only matters to anything that dims by it.
	virtual bool headEnabled() const {
		const backplane::DeviceHost* h = deviceHost();
		if (!hasEnable() || h == nullptr) {
			return true;
		}
		return h->fieldAt(device(), enableControl()) >= 0.5f;
	}

	/// **THE ACTIVE BUTTON, DRAWN ONCE.** The collapsed spine shows the effect name, the active button
	/// and the expand control, and this is the second of those.
	///
	/// It was SEVEN copies — every fx card drew the same filled-or-outlined rounded square from the
	/// same `enableBounds()`. `shaper_card.cpp` named the hazard while adding the fifth: *"a fifth
	/// spelling of a switch is a fifth thing to keep in step."* The base already owned the GEOMETRY
	/// (`hasEnable`, `enableBounds`, `enableControl`); only the drawing was scattered, and only
	/// because reading the control's value needs the card's own host — which `headEnabled()` supplies
	/// in one line instead of a copied paragraph.
	void drawEnableSwitch(visage::Canvas& canvas) {
		if (!hasEnable()) {
			return;
		}
		const visage::Bounds sw = enableBounds();
		const bool on = headEnabled();
		canvas.setColor(on ? kAccent : kMuted);
		if (on) {
			canvas.roundedRectangle(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f);
		}
		else {
			canvas.roundedRectangleBorder(sw.x(), sw.y(), sw.width(), sw.height(), 3.0f, 1.0f);
		}
	}

	/// **WHETHER THIS CARD IS THE ONE A KEY WOULD ACT ON.** Delete or Backspace acts on the focused
	/// effect.
	///
	/// The card holds the FLAG and the host holds the CHOICE — only a lane knows which of its cards is
	/// the focused one, and only a lane can act on a slot.
	///
	/// **THIS IS SELECTION, AND IT IS THE ONLY THING `headFocused` MEANS NOW.** `EnvCard` and
	/// `MsegCard` used to OVERRIDE `headFocused` with the mod panel's notion — "my source is the
	/// focused one" — so in the strip the same border meant a different thing than it meant one host
	/// over, and a selected card and an armed card would have been drawn the same. That notion is
	/// `headSourceFocused` below, a second virtual with a second mark (see `ui::CardFrameStyle`).
	void setFocused(bool on) {
		if (focused_ == on) {
			return;
		}
		focused_ = on;
		redraw();
	}

	bool headFocused() const { return focused_; }
	/// WHETHER THIS CARD'S SOURCE IS THE FOCUSED ONE / THE ARMED ONE — what draws the frame's GLOW.
	/// A card with no outputs answers false to both and gets no glow.
	virtual bool headSourceFocused() const { return false; }
	virtual bool headArmed() const { return false; }

	/// **WHAT THE FRAME WILL BE DRAWN WITH, AS A VALUE.** The three reports above, folded by the one
	/// rule in `ui::cardFrameStyleOf`; `draw()` paints exactly this and a case asserts exactly this.
	CardFrameStyle frameStyle() const {
		return cardFrameStyleOf(headFocused(), headSourceFocused(), headArmed(), tintedHeadMarks().tint);
	}

protected:
	/// **DRAW THE BODY THROUGH THIS — AND THE CLIP IS THE POINT, NOT THE SCROLL.**
	///
	/// In the MSEG the graph drew over the header. That must be impossible by construction — a card
	/// cannot draw over the header, by coordinate constraints — and the version of this that only
	/// offset the canvas made it possible: a card whose geometry
	/// forgot `bodyLeft()` drew straight through its own spine, and the only thing standing between
	/// the head and the content was every card remembering to subtract the same number. That is the
	/// duplication this file has been removing all week, in its most literal form.
	///
	/// `setClampBounds` is checked by every shape the canvas emits, so a card CANNOT paint outside its
	/// body whatever its geometry says. A card that gets its own arithmetic wrong now draws a clipped
	/// picture — visibly wrong, and wrong in its own rectangle — instead of over its neighbour's head.
	///
	/// It also carries the scroll, so a card's geometry stays written against its content and knows
	/// nothing about scrolling.
	template <typename Fn> void drawBody(visage::Canvas& canvas, Fn&& body) {
		const CardFrame frame = ownFrame();
		canvas.saveState();
		canvas.setClampBounds(frame.body.x(), frame.body.y(), frame.body.width(),
		                      frame.body.height());
		canvas.setPosition(0.0f, -bodyScroll_);
		body();
		canvas.restoreState();
	}

	/// A PRESS, IN THE BODY'S OWN COORDINATES. The inverse of what `drawBody` does to the canvas —
	/// without it a scrolled card would draw one thing and be pressed somewhere else.
	visage::Point bodyPoint(visage::Point position) const {
		return { position.x, position.y + bodyScroll_ };
	}

public:

	/// **EVERY CARD FRAMES ITSELF, THROUGH ONE FUNCTION.** There was briefly a `paintsOwnFrame()`
	/// here, because two conventions existed: fx cards painted their own surface and border while the
	/// modulator cards let their host paint theirs. That virtual let a host cope with both — which is
	/// a way of KEEPING a split rather than closing it. The design is one unified frame for both mod
	/// and fx, so there is one: `ui::drawCardFrame`, and `ui::cardFrameOf` says where the
	/// head, the body and the outputs strip are. A host no longer asks; there is nothing to ask.

	/// WHICH WAY THE LANE RUNS. Left to right for the rack, possibly top to bottom for Feathers, so
	/// both are supported: it is a property of the LANE, not of the build.
	enum class LaneAxis : uint8_t {
		Vertical,   ///< Cards stack downward. Each takes the full width and chooses its height.
		Horizontal, ///< Cards run rightward. Each takes the full height and chooses its width.
	};

	/// HOW MUCH OF THE FLOW AXIS THIS CARD NEEDS, given how much CROSS it has been allotted.
	///
	/// THE CROSS ARGUMENT IS WHAT MAKES ONE FUNCTION SERVE BOTH DIRECTIONS. Vertically the cross is the
	/// card's width and the answer is a height; horizontally the cross is its height and the answer is a
	/// width. Nothing else about a lane differs — placement accumulates the flow extent, scroll runs
	/// along it, and the visibility test compares against it.
	///
	/// THE DEFAULT IS THE VERTICAL ANSWER, AND THAT IS AN HONEST STATEMENT ABOUT THE HAND-WRITTEN CARDS
	/// RATHER THAN A STUB. OWLTT, HARM, Shaper, Waypoint, Drawn and Automod lay out plots, pads and
	/// planes against a wide card; none of them can flow sideways without being redesigned, and a card
	/// that answered a plausible width for a layout it cannot draw would be worse than one that does not
	/// override this. **A card supports the horizontal lane exactly when it overrides this.**
	///
	/// IT DOES NOT TAKE THE AXIS. The card is TOLD its axis once, by `setLaneAxis`, and its layout and
	/// its extent then read the same stored value. `waypoint_card.h` records why that matters: *"`layOut`
	/// and `laneHeight` must agree about how many rows"* — passing the axis here as well would be a
	/// second source for the one fact they have to agree on.
	virtual float laneExtent(float cross) const {
		// **IN A HORIZONTAL LANE THE ANSWER IS A WIDTH, AND `laneHeight()` IS NOT ONE.** The note above
		// said a card supports the horizontal lane exactly when it overrides this, and that was right
		// while the alternative was a card drawing a layout it cannot do. The unit rule gives a better
		// default: **one `kCardUnitWidth`**, the same width a modulator card and a generic list column
		// take. A card that wants more overrides.
		//
		// WHAT IT REPLACES WAS SIMPLY WRONG RATHER THAN CONSERVATIVE: answering `laneHeight()` handed
		// the lane a HEIGHT to use as a WIDTH, so HARM came out 146px wide and drew "LEVEL ATTACK
		// RELEASE" on top of itself. A default that is wrong in a specific direction is worse than one
		// that is merely plain.
		// **FOLDED FIRST, because a collapsed card is its spine whatever its open size would be.** The
		// sweep across every card caught this the moment the width default landed: answering a unit
		// wide while folded is exactly the MSEG bug — the state flips and the geometry does not follow
		// — and it was reintroduced from the other side within the hour. Only the horizontal branch
		// needs saying, because the vertical one is the card's own answer at the cross it was handed.
		if (folded()) {
			return kCardHeadHeight;
		}
		if (axis_ == LaneAxis::Horizontal) {
			return kCardUnitWidth;
		}
		// **`laneHeightAt(cross)` AND NOT `laneHeight()`, AND THIS LINE IS WHY THE SIGNATURE CHANGED.**
		// It read `laneHeight()`, so the `cross` this function exists to convey was discarded — the
		// body opened `(void)cross;` and said so. Three call sites pass a width precisely to close the
		// zero-width trap and all three were defeated here: `FxLane::cardBounds` computes
		// `width() - 2 * kPadX`, `RackEditor` passes the 480 it is about to set, and `ModPanel::resized`
		// passes the width in the same call. Each was correct only for `GenericCard`, the one class
		// that overrode this — correctness by which subclass happened to arrive.
		return laneHeightAt(cross);
	}

	/// TELL THIS CARD WHICH WAY ITS LANE RUNS. Set once by the lane when it builds the card.
	void setLaneAxis(LaneAxis axis) {
		if (axis_ == axis) {
			return;
		}
		axis_ = axis;
		resized();
		redraw();
	}

	LaneAxis laneAxis() const noexcept { return axis_; }

	/// OPEN THIS DEVICE'S EXPANDED VIEW. Bound by the LANE, which knows the slot; the card only knows a
	/// hand pressed it. A card whose callback is empty simply has no such verb.
	///
	/// **IT IS ON THE BASE NOW, AND THAT OVERRULES A DELIBERATE DEFERRAL — here is why.** Five cards
	/// declared this identically and `fx_lane.cpp` reached each through its own `dynamic_cast`, with a
	/// note saying a shared hook was "a new road" to be avoided until the head unification was agreed.
	/// Three things changed:
	///
	/// 1. **The cost grew from two casts to five**, all with the same signature and the same body.
	/// 2. **A sixth card needs it and no cast in this lane can reach it.** `GenericCard` is the rack's
	///    only card and lives in another host; a per-type arm in Feathers' lane is not a road to it.
	/// 3. **The deferral has already cost a bug.** `WaypointExpanded` was implemented, correct, its
	///    entry registered and `hasExpanded` true — and unreachable, because its cast arm was missing.
	///    Both halves of the guard were known-badded and went red as designed, and the panel still could
	///    not be opened: there is no third term for "and something calls it". `Shaper` went red the same
	///    way the moment it was registered.
	///
	/// **A HOOK ON THE BASE MAKES THAT OMISSION UNREPRESENTABLE** rather than guarded against, which is
	/// the difference between a rule and a check. The casts were not load-bearing — this repo's rule is
	/// to find out what redundancy buys before deleting it, and what these bought was not touching this
	/// file. Nobody holds it: measured across every worktree, `fx_card.h` is dirty in none.
	std::function<void()> onExpand;

	/// Hand the card the modulation panel's state, so its controls can be routed to. Defaulted
	/// because a device whose controls are not destinations has nothing to do with it — and a card
	/// that must be TOLD to implement this is a card that silently never lights, which is the failure
	/// FilterCard's own knobs shipped with once.
	virtual void setModulation(ModState* mod, EditHistory* history) {
		(void)mod;
		(void)history;
	}

	//---------------------------------------------------------------------------------------------//
	// THE LANE'S VERBS ON THIS SLOT — remove, and move up or down.                                  //
	//---------------------------------------------------------------------------------------------//
	//
	// THEY ARE THE LANE'S OPERATIONS AND NOT THE DEVICE'S, and yet they live in the CARD's head,
	// which is worth explaining because it looks backwards. A card fills the lane's width less its
	// padding, so a target the LANE drew would be underneath the card and unreachable: the child frame
	// gets the press. Ten pixels of lane either side is not a button. The rack does not have this
	// problem because its strip draws the head row itself — the two chains divide the head
	// differently, which this file already says, and this is the first place that difference costs
	// something.
	//
	// SO THE BASE CLASS OWNS THE WHOLE AFFORDANCE and a card spends two lines on it: `drawSlotMenu`
	// in its draw and `slotMenuAt` first in its hit test. Written once here rather than three times,
	// because three cards agreeing by hand is how one of them ends up 4px off and unreachable.
	//
	// AND IT IS A MENU RATHER THAN A BARE REMOVE BUTTON, which is a decision and not a convenience —
	// see FxLane::showSlotMenu for the reasoning about confirmation.

	/// WHICH DEVICE THIS CARD IS THE FACE OF, or nullptr for a card that does not know.
	///
	/// IT EXISTS SO THE HEAD CAN BE DERIVED RATHER THAN HAND-WIRED. A card currently draws its own
	/// head and knows its own device by construction; once the LANE draws one head for every device
	/// (the 2026-08-24 decision) the head has to ASK. Added now, with the enable square as its first
	/// consumer, so the convergence is a move rather than a redesign.
	virtual const device::Descriptor* descriptor() const { return nullptr; }

	/// WHATEVER IS HOSTING THAT DEVICE, or nullptr for a card that does not know.
	///
	/// The same shape as `descriptor()`, and it exists for the same reason: so the base can ASK
	/// rather than know. Its first consumer is `offersExpand`, which used to consult Feathers'
	/// expanded-view registry directly — and that one include put `plugin/params.h` under every card
	/// in both hosts, so the card floor no longer compiled without the synth (backlog X). A card
	/// holding a `backplane::DeviceHost&` answers it in one line; a card without a host has nothing
	/// to ask and draws no way in.
	virtual const backplane::DeviceHost* deviceHost() const { return nullptr; }

	/// **THE CONTROL BEHIND THE ENABLE SQUARE**, so `enablePressAt` can drive it without knowing what
	/// kind of card this is. One line per card: `return &control(enableControl());`
	///
	/// **WHY THIS IS NOT DONE WHOLLY IN THE BASE, since a reader will ask.** `deviceHost()` is const,
	/// so the base cannot write through it — and giving it a mutable host would route the write around
	/// `DeviceControl` and lose the history step again, which is the exact defect this refactor
	/// exists to remove. The per-card hook is what keeps the write on the road that records undo.
	///
	/// A card with no enable, or one whose enable has no control object, returns null and
	/// `enablePressAt` swallows the press rather than letting it become a fold.
	virtual device::Modulatable* enableModulatable() { return nullptr; }

	/// WHICH DEVICE THIS CARD DRAWS — the other half of `deviceHost()`, and useless without it. A host
	/// and a device together are how the base reads a field for itself instead of asking each card to
	/// re-answer the same question.
	///
	/// **ITS ID, NEVER A SLOT OR A LANE (kill-lanes step 2).** This was `deviceSlot()`, and what a slot
	/// meant changed three times under one name — a position, then an fx lane, then a lane in either of
	/// two slot spaces. An id is the device wherever it stands; only the host knows where that is.
	virtual DeviceInstanceId device() const { return kNoInstance; }

	/// WHICH HEAD MARKS THIS CARD'S HOST OFFERS. Set by the host once; the card passes them to
	/// `ui::drawCardFrame`. **A card cannot infer these** — Feathers' strip offers a fold and a slot
	/// menu, the rack's lane offers neither, and it is the same card class in both.
	/// WHICH EDGE THIS CARD'S HEAD SITS ON, derived from the lane it was told it is in.
	///
	/// **NOT STORED SEPARATELY.** The axis is already the card's one fact about its lane, and a second
	/// field for the edge would be a second thing to keep in step with it — the exact shape of every
	/// duplication bug in this file's history.
	/// WHERE THIS CARD'S BODY STARTS, in its own coordinates — the head's thickness on whichever edge
	/// the head is. **Every card's geometry measures from these two** rather than from a constant,
	/// which is what lets one layout serve both lanes: the head moved and the content followed.
	float bodyLeft() const noexcept {
		return headEdge() == CardHeadEdge::Left ? kCardHeadHeight : 0.0f;
	}
	/// HOW WIDE THE BODY IS — the card less whatever the head took. A card measuring against
	/// `width()` would draw its content under its own spine.
	float bodyWidth() const noexcept { return std::max(0.0f, static_cast<float>(width()) - bodyLeft()); }
	float bodyTopEdge() const noexcept {
		return headEdge() == CardHeadEdge::Top ? kCardHeadHeight : 0.0f;
	}

	/// **WHETHER THIS CARD IS COLLAPSED, AND THE ONE PROCEDURE FOR CHANGING IT.**
	///
	/// It was eight copies. Every card declared its own `folded_`, its own `folded()`, and its own
	/// `setFolded` doing the same five things: guard, assign, hide MY children, tell the lane,
	/// repaint. **The two bugs that produced are both on the record**: `OwlttCard::setFolded` wrote
	/// `folded_ = false` instead of `folded_ = folded`, so OWLTT could not be folded at all and every
	/// line after did the right thing with the wrong number; `HarmCard` hid its knobs and not its
	/// harmonic row, leaving a live widget under a 24px head, over whatever card the lane moved up.
	/// Both were found by sweeping every card with one question rather than by reading.
	///
	/// So the PROCEDURE lives here and a card supplies only the part that is genuinely its own — its
	/// children. A card can no longer forget the guard or misspell the assignment, because it no
	/// longer writes them.
	bool folded() const noexcept { return folded_; }

	void setFolded(bool folded) {
		if (folded_ == folded) {
			return;
		}
		folded_ = folded;
		foldedChanged();
		if (onFoldChanged) {
			onFoldChanged();
		}
		redraw();
	}

	/// SHOW OR HIDE WHATEVER THIS CARD OWNS. Read `folded()` for the new state.
	///
	/// **A CHILD LEFT VISIBLE UNDER A FOLDED HEAD IS STILL PRESSABLE**, which is the HARM bug: it is
	/// not a cosmetic omission, it is a live control sitting on top of a neighbouring card.
	virtual void foldedChanged() {}

	/// **TOLD WHEN ITS HOST RETIRES IT** — taken off the tree and parked until the host frees it
	/// (`FxLane::retiredCards_`, `ModPanel::retiredCards_`), which is some frames later. A card that
	/// subscribed to something outliving it ends the subscription here, so a retired card stops hearing
	/// its device while it waits: its device may still exist (a reorder rebuilds the card of a device
	/// that stays), and a retired card refreshing itself on its device's news is a card nobody can see
	/// doing work nobody asked for. Called once, after `removeChild`; the card is still whole.
	virtual void retired() {}

protected:
	/// PROTECTED, because cards READ it while laying out — `folded()` is the public question and this
	/// is the same answer for a subclass that already has it in scope.
	bool folded_ = false;

public:

	CardHeadEdge headEdge() const noexcept {
		return axis_ == LaneAxis::Horizontal ? CardHeadEdge::Left : CardHeadEdge::Top;
	}

	void setHeadMarks(CardHeadMarks marks) { headMarks_ = marks; }
	CardHeadMarks headMarks() const noexcept { return headMarks_; }
	/// THE MARKS AS THE FRAME DRAWS THEM: the host's, plus this INSTANCE's tint — set here rather than
	/// by every lane that builds marks, so no lane can forget it (`CardHeadMarks::tint`).
	CardHeadMarks tintedHeadMarks() const noexcept {
		CardHeadMarks marks = headMarks_;
		marks.tint = instanceTintFor(device(), nest_);
		return marks;
	}

	/// **WHICH CHAIN THIS CARD'S DEVICE LIVES IN** — `DeviceNest`, and the default is the host's own,
	/// which is what every lane that is not inside a panel wants and therefore writes nothing for.
	/// A column inside a device's panel (`ChainColumn`) sets it, and the panel that column opens is
	/// stamped with the SAME value — that is what pairs a card with the panel it opened, rather than
	/// two derivations that have to be kept agreeing.
	void setNest(DeviceNest nest) {
		if (nest == nest_) {
			return;
		}
		nest_ = nest;
		redraw();
	}
	[[nodiscard]] DeviceNest nest() const noexcept { return nest_; }

	/// WHAT TO DO WHEN A HEAD MARK IS PRESSED. Installed by the host, because only the host knows
	/// what folding or opening a menu MEANS for the slot this card sits in.
	std::function<void()> onHeadFold;
	std::function<void()> onHeadMenu;

	/// **A PRESS ON A HEAD MARK, TAKEN BY THE CARD BECAUSE THE CARD IS ON TOP OF IT.**
	///
	/// This is not a convenience, it is the fix for a documented bug reappearing. `EnvCard`'s own
	/// note records a first version whose frame covered the head: *"the card is a child frame, so it
	/// sat on top of the fold and the arrow and swallowed every press aimed at them. The buttons were
	/// drawn, looked right, and did nothing."* The unified frame put the card over the head again —
	/// so the card must now HAND BACK the presses that are not its own, rather than the host trying
	/// to hit-test through it.
	///
	/// Returns true when it consumed the press. A card whose host offers no marks consumes nothing.
	bool headMarkPressed(visage::Point position) {
		// **STATING THE EDGE, WHICH IS THE WHOLE BUG.** This called `cardMenuBounds`/`cardFoldBounds`
		// with no edge and took their `= CardHeadEdge::Top` default, while `drawCardFrame` passed
		// `headEdge()` — so on a spine the marks were painted down the left and pressed across the top.
		// So the (...) and (-) marks did not work. The defaults are gone now, so this cannot be
		// written again without saying which head it means.
		if (headMarks_.menu
		    && cardMenuBounds({ 0.0f, 0.0f, width(), height() }, headEdge()).contains(position)) {
			if (onHeadMenu) {
				onHeadMenu();
			}
			return true;
		}
		return false;
	}

private:
	CardHeadMarks headMarks_ {};
	DeviceNest nest_ {};
	float bodyScroll_ = 0.0f;

public:

protected:
	/// VERTICAL UNTIL TOLD OTHERWISE — Feathers' lane, and what every card here was written against.
	LaneAxis axis_ = LaneAxis::Vertical;

public:

	/// ADVANCE WHATEVER THIS CARD DRAWS FROM A LIVE ENGINE READING, and say whether the screen is out
	/// of date. Default false: a card with nothing live to show asks for nothing, which is every card
	/// but OWLTT today.
	///
	/// THIS EXISTS SO A BLOCK-RATE PUBLISHER DOES NOT HAVE TO USE THE STATE CHANNEL.
	/// `stateWatchers.notify()` redraws the whole subtree of every watcher — measured at 108 frames
	/// for one meter update, 85 of them nothing to do with the device that published — and that is
	/// correct for the gestures and patch loads it was built for and wrong for something arriving
	/// every audio block. `publishModulated` already refuses the state flag for exactly this
	/// reason and says so in `gui_bridge.cpp`: *"a widget that wants to follow a moving modulation
	/// asks for its own redraw."* This is that road.
	///
	/// **NOT VIRTUAL, AND THAT IS DELIBERATE — DO NOT "RESTORE" A VIRTUAL `update()` FOR
	/// COMPATIBILITY.** It was `virtual bool update() { return false; }`, and every card that wanted
	/// to follow a moving reading had to override it and remember to look at `modulatedFieldAt`. Nine
	/// did it against a settings snapshot that cannot see modulation move, and `GenericCard` — which
	/// serves every device without a bespoke card, in BOTH lanes — never overrode it at all, so it
	/// silently answered `false` forever while building `Knob` children, so the modulation dot moved
	/// only when the underlying parameter was edited.
	///
	/// **THAT IS THE ENABLE-PRESS FAILURE VERBATIM**, and this is the same remedy: the base owns the
	/// thing every card needs and each card keeps only what is its own. A convention nine sites must
	/// each remember is a bug with a tenth spelling already written; making `update()` non-virtual
	/// makes the wrong thing IMPOSSIBLE rather than discouraged, and turns the rebase into a list the
	/// compiler produces instead of one somebody remembers.
	bool update() {
		// **THE CARD'S OWN HOVER, INSTALLED ON THE FIRST FRAME RATHER THAN IN THE CONSTRUCTOR.** A card's
		// constructor body runs AFTER this base's, and five of them assign `onMouseDown() = ...`, which
		// would wipe a hook installed here — see `installTooltipHover`. The first `update()` is after
		// every constructor has finished.
		if (!ownHoverInstalled_) {
			installTooltipHover(*this, *this);
			ownHoverInstalled_ = true;
		}
		// THE VEIL BEFORE ANYTHING, for the same reason the walk below is here and not in the cards:
		// an enable field can move from the head, an undo, a patch or automation, and this is the
		// one road every host drives every frame. See `ui::DisabledVeil`.
		syncDisabledVeil();
		// THE MODULATION WALK FIRST, so a card cannot opt out by forgetting — but only if this card
		// actually PAINTS a modulated reading. See `paintsModulation` for why that condition exists and
		// why it is derived rather than declared.
		bool told = paintsModulation() && modulationMoved();
		// THE COURTESY OUTPUT METER (backlog MK), every card's, on the same road.
		told = syncOutputMeter() || told;
		// AND THEN WHATEVER THIS CARD DRAWS OF ITS OWN — a meter, a playhead, a graph.
		told = updateContent() || told;
		// AND THE TOOLTIP'S DELAY, which is a clock like any other thing this road advances.
		told = tips_.advance() || told;
		// AND THE AS-MODULATED READOUT (backlog PU).
		told = modReadout_.advance() || told;
		return told;
	}

	/// THIS CARD'S TOOLTIP LAYER — see `ui/tooltip.h`. Public so a case can read what it shows.
	TooltipLayer& tooltipLayer() override { return tips_; }
	/// THIS CARD'S AS-MODULATED READOUT (backlog PU) — public so a case can read what it shows.
	ModReadoutLayer& modReadoutLayer() { return modReadout_; }

	/// **WHAT THE CARD'S OWN SURFACE MEANS UNDER THE POINTER** — the parts it draws itself rather than
	/// seating a widget for. The base answers for the head's enable square; a card that hand-draws a
	/// control in its body overrides `contentTooltipAt` and chains nothing.
	const char* tooltipAt(visage::Point local) const override {
		if (enableBounds().contains(local)) {
			return controlTooltip(enableControl());
		}
		return contentTooltipAt(local);
	}
	virtual const char* contentTooltipAt(visage::Point) const { return nullptr; }
	/// **THE HEAD'S NAME, WHEN IT WAS CUT** — over the name's box, the whole name (`ui::LabelFace`), so a long name
	/// elided against the head's marks can still be read by hovering it.
	std::optional<LabelFace> labelFaceAt(visage::Point local) const override {
		const HeadLayout layout = headLayout();
		if (!layout.nameBox.contains(local)) {
			return std::nullopt;
		}
		const std::string whole = headName();
		if (whole.empty()) {
			return std::nullopt;
		}
		return LabelFace { .label = whole, .full = whole, .cut = layout.name != whole };
	}
	/// **WHAT THE CARD'S OWN SURFACE OFFERS** (`ControlFace`) — the head's enable square. A card that hand-draws a
	/// control in its body (one that overrides `contentTooltipAt`) overrides this too and adds each one, or the
	/// window-superset guard cannot see the control.
	void offeredControls(std::vector<Offered>& out) const override {
		// `enableModulatable` is non-const for its WRITE road (the comment above it); asking for the pointer writes nothing.
		if (const device::Modulatable* sw = const_cast<FxCard*>(this)->enableModulatable()) {
			out.push_back({ sw, Road::Face });
		}
	}
	/// THE TOOLTIP OF THIS DEVICE'S CONTROL `index`, from its declaration — what a hand-drawn part
	/// answers with. Null past the end, so a bad index says nothing rather than something wrong.
	const char* controlTooltip(size_t index) const {
		const device::Descriptor* d = descriptor();
		if (d == nullptr || index >= d->params.size()) {
			return nullptr;
		}
		return tooltipText(d->params[index].tooltip, d->params[index].name);
	}

	/// WHAT THIS CARD ADVANCES BEYOND ITS CONTROLS' MODULATION. Override this, never `update()`.
	///
	/// Returns whether it asked for a repaint. **A COUNT PRODUCED BY THE NOTIFICATION**, which is what
	/// the repaint rule requires: `FxLane::updateCards` accumulates it into `report.deviceFrames`, and
	/// a counter standing beside the call could not make that promise.
	virtual bool updateContent() { return false; }

	static constexpr size_t kNoEnable = static_cast<size_t>(-1);

	/// WHICH CONTROL SWITCHES THIS DEVICE, or kNoEnable — asked of the descriptor, never assumed.
	///
	/// EVERY FX DEVICE DECLARES ONE AND NO MODULATOR DECLARES ANY, so a shared head can only draw the
	/// square conditionally, and the condition must come from the DEVICE rather than from a branch on
	/// which lane we are in. See device.h's `enableControl`, including why matching the param's NAME
	/// was rejected.
	size_t enableControl() const {
		const device::Descriptor* desc = descriptor();
		if (desc == nullptr || desc->traits.enableControl < 0) {
			return kNoEnable;
		}
		const auto at = static_cast<size_t>(desc->traits.enableControl);
		// A DECLARATION PAST THE END IS REFUSED rather than trusted — an index is only an index while
		// it names something.
		return at < desc->params.size() ? at : kNoEnable;
	}

	bool hasEnable() const { return enableControl() != kNoEnable; }

	/// THE ENABLE SQUARE, in the head, inboard of the slot menu — and EMPTY for a device that declares
	/// no enable. An empty rectangle contains no point, so a card's hit test refuses it without a
	/// second branch.
	///
	/// ON THE BASE RATHER THAN ONCE PER CARD. It was written twice, identically, when each card
	/// hard-wired its own switch; now that the square is derived there is nothing left for a card to
	/// contribute, and two copies of a derivation is two places for it to stop agreeing.
	visage::Bounds enableBounds() const {
		if (!hasEnable()) {
			return {};
		}
		const float size = 12.0f;
		// **ON A SPINE IT RUNS UP THE HEAD LIKE EVERYTHING ELSE IN IT.** Beneath the source dot, above
		// the host's marks at the foot — the same order a top head reads left to right, transposed.
		// A switch left at its horizontal coordinates would sit outside a 24px-wide spine entirely.
		if (headEdge() == CardHeadEdge::Left) {
			// **AT THE TOP, ABOVE THE LAST LETTER OF THE NAME** — the chosen placement. The name reads
			// bottom-to-top and is right-justified, so its last letter is at the spine's top and the
			// switch sits directly above it: the two things a glance wants first, together.
			//
			// (It was at the top ONCE before and covered the name, because the name band started at the
			// same y. The band starts BELOW the switch now — `kSpineNameTop` — so they share the top of
			// the spine instead of competing for one pixel row.)
			return { (kCardHeadHeight - size) * 0.5f, kModCardPadX, size, size };
		}
		// **INBOARD OF WHICHEVER MENU IS ACTUALLY THERE — THERE ARE TWO, AND THIS ANCHORED TO THE ONE
		// THIS HOST DOES NOT DRAW.**
		//
		// The (...) mark still sat over the MSEG and DAHDSR active box after BG's first fix.
		// f4d099f fixed a REAL bug and fixed it properly — but on the other road. A head has
		// two overflow menus and they are different rectangles:
		//
		//   A. `slotMenuBounds()` — the CARD's own, 12px, drawn by `drawHeadExtras` when a host binds
		//      `onSlotMenu`. This is what `spineMark` yields to and what f4d099f repaired.
		//   B. `ui::cardMenuBounds` — the HOST's head mark (18px on a spine; on a top head A's own rectangle
		//      since 2026-10-04, so the two lanes' rows end at one inset), drawn by `ui::drawCardFrame` when
		//      `headMarks().menu`. It has never had a guard of any kind.
		//
		// The modulator strip is the ONLY host that draws B (`mod_panel.cpp` sets `{ true, … }` and
		// binds `onHeadMenu`); it binds no `onSlotMenu`, so A is neither drawn nor pressed there. So
		// this line placed the square against a rectangle that host leaves EMPTY, and B landed on top
		// of it from the left. **The derivation was FALSE rather than unlucky** — wrong in every host,
		// harmless in the two that do not draw B — which is why the fix is here and not a second
		// yielding rule in `theme.cpp`.
		//
		// MEASURED on the rendered strip (DAHDSR, card x∈[178,394), head top 182): square
		// x[354,366) y[188,200), menu x[347,365) y[185,203) — an 11×12 overlap, and two of the three
		// painted dots (351, 355, 359) inside the square. Constant at every card width and height,
		// because both rectangles anchor to the card's RIGHT EDGE and its top: BG's height band is a
		// fact about the spine and says nothing here. Identical to the pixel on MSEG and TRAK.
		//
		// AND IT WAS NOT ONLY PAINT. `headControlAt` counts the square as a head control, so
		// `noteCardPress` reaches `headMarkPressed`, whose rectangle covered 132 of the square's 144
		// px; `EnvCard::onMouseDown` then falls through to `enablePressAt`, which also matched. One
		// click on 11 of the square's 12 columns opened the menu AND flipped the device.
		//
		// **WHY NOT MAKE THE MARK YIELD, WHICH IS WHAT THE SPINE DOES.** The spine can hide a mark
		// because the fold survives beneath it and is the way back out. There is no such fallback
		// here: `SlotVerb::Remove` dispatches only from this menu (`mod_panel.cpp`, and that file has
		// no keyboard road), so hiding it strands a modulator that cannot then be deleted or
		// reordered. A fix that makes a device unremovable is not a fix. Shifting B instead of hiding
		// it needs the square's rectangle inside a free function that today takes only a rect and an
		// edge — a parameter four call sites would have to supply identically, which is the exact
		// drift `headMarkPressed`'s own note records having already paid for once.
		//
		// `std::min` rather than a branch: no host draws both marks today, and that should stay a fact
		// the geometry SURVIVES rather than a fact it depends on.
		float menuLeft = slotMenuBounds().x();
		if (headMarks().menu) {
			menuLeft = std::min(menuLeft, cardMenuBounds({ 0.0f, 0.0f, static_cast<float>(width()),
				                                           static_cast<float>(height()) },
			                                             headEdge())
			                                  .x());
		}
		const float right = menuLeft - 6.0f;
		return { right - size, (kSlotHeadHeight - size) * 0.5f, size, size };
	}

	static constexpr float kSlotMenuSize = kCardMenuMarkSize;
	/// The head every card in this lane wears. The two real cards already declare 24 independently;
	/// this is where they agree, so the button cannot drift off one of them.
	static constexpr float kSlotHeadHeight = 24.0f;
	static constexpr float kSlotMenuPad = kCardMenuMarkInset;


	/// **ONE MARK IN THE SPINE'S FOOT STACK, AND THE RECTANGLE IT REFUSES TO OCCUPY.**
	///
	/// Backlog BG: the (...) menu was rendered on top of the modulator's activate button, so the button
	/// looked checked when it was only a painting mistake. The modulator was OFF.
	///
	/// **THE CAUSE IS THAT THE HEAD IS LAID FROM BOTH ENDS.** On a spine the enable square is placed
	/// from the TOP (`kModCardPadX`, the chosen placement, above the last letter of the name) and the
	/// fold, menu and expand are stacked up from the FOOT. Whether those two ever meet is therefore a
	/// pure function of the card's HEIGHT — and the rack's 336 is the height at which they are
	/// furthest apart, which is why every case we had was green. MEASURED: they overlap for heights
	/// **40 through 60**, and a MINIMISED modulator card is `kCardHeadHeight + 0 +
	/// kCardOutputStripHeight` = **46**. That is the card in the bug's screenshot.
	///
	/// **A MARK THAT WOULD COVER THE SQUARE IS NOT DRAWN AT ALL** — an empty rectangle, the idiom this
	/// file already uses for "there is nothing here", which the hit test refuses without a second
	/// branch. The alternative, clamping it to sit just below the square, only moves the collision:
	/// at 46 there is room for the square and ONE mark, so a clamped menu and a clamped expand would
	/// land on each other instead.
	///
	/// **AND THE ORDER OF THE STACK IS WHY HIDING IS SAFE.** The fold is FIRST from the foot, so it is
	/// the last to run out of room — it survives at every height where anything does. That is the
	/// property that matters, because the fold is the way back OUT of a minimised card: the same
	/// argument `FxCard` makes for never dimming the head, and the one the shaper's Knee makes for
	/// staying lit at Knee 0. What is lost first is a menu you can still reach by unfolding.
	///
	/// **WHY THE SQUARE WINS AND THE MARKS YIELD.** The enable is the only one of the four whose
	/// APPEARANCE CARRIES STATE. A covered menu is a button you cannot find; a covered enable is a
	/// device reporting that it is on when it is off, which is the honesty rule broken by geometry
	/// rather than by a wrong value — and it is the half that cost an evening of confusion.
	visage::Bounds spineMark(int fromFoot) const {
		const float y = static_cast<float>(height()) - kModCardPadX
		                - static_cast<float>(fromFoot) * (kSlotMenuSize + 4.0f);
		// **ADJACENT IS NOT OVERLAPPING**, hence `<` rather than `<=`: at 46 the FOLD sits exactly on
		// the square's bottom edge and is correct there. A `<=` would hide the one mark that must
		// never go.
		if (y < enableBounds().bottom()) {
			return {};
		}
		return { (kCardHeadHeight - kSlotMenuSize) * 0.5f, y, kSlotMenuSize, kSlotMenuSize };
	}
	visage::Bounds slotMenuBounds() const {
		// **IT TRANSPOSES WITH THE HEAD, AND IT DID NOT.** The (...) menu neither worked nor was drawn
		// on the header. Both halves are this one rectangle: every card draws its
		// menu at these coordinates AND hit-tests them, so on a spine it was drawn off the head and
		// pressed somewhere else. One definition, so the mark and the press cannot disagree — the rule
		// this file has been converging on all day.
		if (headEdge() == CardHeadEdge::Left) {
			// SECOND FROM THE FOOT, above the fold. Same order a top head reads right-to-left.
			return spineMark(2);
		}
		return { width() - kSlotMenuPad - kSlotMenuSize, (kSlotHeadHeight - kSlotMenuSize) * 0.5f,
			     kSlotMenuSize, kSlotMenuSize };
	}

	/// **THE WAY INTO THE PANEL — ONE RECTANGLE FOR EVERY CARD.**
	///
	/// The expand-to-window/canvas box was missing from the vertical headers. It was written
	/// per card — OWLTT, Drawn, HARM and Shaper each had their own `expandBounds` — and every copy
	/// placed it in TOP-head coordinates, so none of them appeared on a spine. Hoisted and transposed;
	/// a card that has no panel still answers empty, which is what `hasExpandedView` decides.
	/// **EMPTY WHEN THERE IS NOTHING TO OPEN**, so the button cannot promise a panel this build cannot
	/// construct — six cards each wrote that rule and each wrote it correctly; what none of them could
	/// write was the spine, because all six spelled the geometry in top-head coordinates.
	/// Whether this card opts out of the shared expand mark — asked by the structural audit that keeps
	/// one verb to one glyph. See `test_gui_widgets_devices.cpp`. Placed beside `expandBounds` because
	/// that is where the verb's geometry lives and both are public for the same reason.
	[[nodiscard]] bool drawsOwnExpandMarkForTest() const { return drawsOwnExpandMark(); }

	visage::Bounds expandBounds() const {
		if (!offersExpand()) {
			return {};
		}
		if (headEdge() == CardHeadEdge::Left) {
			// THIRD FROM THE FOOT: fold, menu, expand, reading up — the transpose of a top head's
			// right-to-left order.
			return spineMark(3);
		}
		// **INBOARD OF THE ENABLE SQUARE AND SIZED LIKE IT**, so the head reads as a row of buttons
		// rather than as a square and a mark. This is the geometry all six copies agreed on, and two
		// suites assert it: `ex.right() <= sw.x()`.
		const visage::Bounds sw = enableBounds();
		const visage::Bounds ref = sw.width() > 0.0f ? sw : slotMenuBounds();
		const float size = ref.height();
		return { ref.x() - 6.0f - size, ref.y(), size, size };
	}

	/// **THE WAY INTO THE DEVICE'S PRE AND POST CHAINS** (backlog NR) — a small chip inboard of the expand mark (or of
	/// whatever the expand mark is inboard of, when there is none), FOURTH from the foot on a spine. Empty when the card
	/// offers none: no host verb bound (`onSite`), or a device that offers no site (`offersSite`).
	visage::Bounds siteBounds() const {
		if (!offersSite()) {
			return {};
		}
		if (headEdge() == CardHeadEdge::Left) {
			return spineMark(4);
		}
		const visage::Bounds ex = drawsOwnExpandMark() ? visage::Bounds {} : expandBounds();
		const visage::Bounds sw = enableBounds();
		const visage::Bounds ref = ex.width() > 0.0f ? ex : sw.width() > 0.0f ? sw : slotMenuBounds();
		const float size = ref.height();
		return { ref.x() - 6.0f - size, ref.y(), size, size };
	}
	/// Whether this card offers a site at all — the device runs audio and declares a site (a Rack and a Chain do since
	/// D3 was lifted, 2026-10-29), and the host has bound the verb. The honesty rule: the chip is drawn exactly when
	/// pressing it opens one.
	bool offersSite() const {
		const device::Descriptor* d = descriptor();
		return onSite && d != nullptr && d->audio != nullptr
		       && d->traits.siteLanes != device::Traits::SiteLanes::None && deviceHost() != nullptr;
	}
	/// **THE HOST'S VERB FOR THE CHIP** — give the device its site if it has none, and open the panel.
	std::function<void()> onSite;

	/// THE CHIP: a square split down the middle — PRE | POST, the signal's order — its halves filled while the device
	/// has a site. Geometry, not a glyph, for the expand mark's reason.
	void drawSiteMark(visage::Canvas& canvas) {
		const visage::Bounds box = siteBounds();
		if (box.width() <= 0.0f) {
			return;
		}
		const backplane::DeviceHost* h = deviceHost();
		const bool has = h != nullptr && h->hasSite(device());
		const float inset = 2.0f;
		const float x = box.x() + inset;
		const float y = box.y() + inset;
		const float w = box.width() - 2.0f * inset;
		const float hgt = box.height() - 2.0f * inset;
		canvas.setColor(has ? kAccent : kMuted);
		if (has) {
			canvas.fill(x, y, w * 0.5f - 0.5f, hgt);
			canvas.fill(x + w * 0.5f + 0.5f, y, w * 0.5f - 0.5f, hgt);
		}
		else {
			canvas.rectangleBorder(x, y, w, hgt, 1.0f);
			canvas.fill(x + w * 0.5f - 0.5f, y, 1.0f, hgt);
		}
	}
	/// **THE PRESS THAT OPENS IT**, funnelled where the expand press is (`noteCardPress`).
	bool siteAt(visage::Point position) {
		if (!siteBounds().contains(position)) {
			return false;
		}
		onSite();
		return true;
	}

	/// Whether this card offers a way into a panel at all. A card with nothing behind it draws no mark.
	/// **ASKED OF THE HOST, NOT DECLARED PER CARD AND NOT READ FROM A REGISTRY THE CARD INCLUDES.**
	/// `DeviceHost::hasExpandedViewFor` is the one answer to "can this host open a panel for that
	/// device"; Feathers answers it from `ui::hasExpandedView` — the conjunction of the device's own
	/// `hasExpanded` and an arm existing to build it, and `expanded_views.cpp` records what a SECOND
	/// list cost the last time: a panel that existed, was correct, and was unreachable. This file
	/// once called that registry itself, and the include carried `plugin/params.h` into the base
	/// every card wears — the reach `feathers-device-ui-probe` exists to refuse.
	bool offersExpand() const {
		const device::Descriptor* d = descriptor();
		const backplane::DeviceHost* h = deviceHost();
		return d != nullptr && h != nullptr && h->hasExpandedViewFor(*d);
	}

	/// Did this press land on the slot menu? Cards call it FIRST, before the head's own fold, for the
	/// reason FilterCard's enable square already documents: a button inside the head's rectangle is
	/// unclickable if the head is tested first, and the bug reads as "the button does nothing".
	/// **IS THERE A MENU TO OPEN?** The rack binds no `onSlotMenu` and every card drew its dots
	/// unconditionally, so in the rack the mark was painted and pressed and nothing happened — the
	/// (...) and (-) marks that did not work. A control that is visible and dead is the honesty
	/// rule from its usual side reversed, and the fix belongs here rather than in ten `drawContent`s.
	///
	/// THE RECTANGLE STILL EXISTS when there is no menu, because `enableBounds` lays the head out
	/// from it — what changes is whether anything is drawn in it or reads a press from it.
	bool hasSlotMenu() const { return static_cast<bool>(onSlotMenu); }

	bool slotMenuAt(visage::Point position) const {
		return hasSlotMenu() && slotMenuBounds().contains(position);
	}

	/// Open it. Installed by the lane when it builds the card, because only the lane knows which slot
	/// this is and only the lane can act on the chain. A card whose callback is empty simply has no
	/// such verb — which is what a card outside a lane should have.
	std::function<void()> onSlotMenu;

	void pressSlotMenu() {
		if (onSlotMenu) {
			onSlotMenu();
		}
	}

	/// THE THREE DOTS. Geometry rather than a glyph, the rule every drawn control in this editor
	/// follows: this font has no guarantee of an ellipsis that centres, and a missing glyph is an
	/// invisible control.
	//---------------------------------------------------------------------------------------------//
	// DRAGGING A CARD TO REORDER IT                                                                 //
	//---------------------------------------------------------------------------------------------//
	//
	// Reorder is a drag, not only a "move earlier" menu entry. The menu entries are kept
	// beside it — they cost nothing and they are the only reorder a small window can reach — but the
	// drag is the gesture.
	//
	// THE CARD DETECTS IT AND THE LANE PERFORMS IT, for the same reason the slot menu lives here: a
	// card fills its lane, so the press lands on the CARD and the lane never sees it. What the card
	// knows is that a hand is dragging; only the lane knows what a slot is, where the other cards are,
	// and how to move one.
	//
	// A PRESS IS NOT YET A DRAG. Below the threshold the gesture is still a click, so the head's own
	// verb — fold — keeps working. Same constant and same reason as the sound rail's: the rows are
	// short and the verbs on them are small, so a threshold big enough to feel deliberate would be a
	// threshold that swallows the click.
	enum class DragPhase { Begin, Move, End, Cancel };

	static constexpr float kDragThreshold = 4.0f;

	/// Reported in the CARD's own coordinates; the lane converts, because the lane is the thing that
	/// knows where this card sits in it.
	std::function<void(DragPhase, visage::Point)> onCardDrag;

	/// THE CARD JUST CHANGED HEIGHT — installed by the lane, called by setFolded.
	///
	/// THE BUG: collapsing an audio effect hid its controls but left the card's size unchanged.
	/// That is exactly what happened: a fold hid the controls and called `redraw()`, which marks the
	/// frame stale and repaints it AT THE SIZE IT ALREADY HAD. `laneHeight()` answered the folded
	/// height immediately and nothing ever asked it again, so the card kept its rectangle and the
	/// cards below it did not move.
	///
	/// THE RACK HAD THIS RIGHT ALREADY — `ModPanel::setCardMinimised` calls `resized()`, which
	/// re-places every card and re-clamps the scroll against a content height that just changed. This
	/// is the fx lane being given the same road, rather than a second one.
	std::function<void()> onFoldChanged;

protected:
	/// **THIS CARD'S HEIGHT CHANGED FOR A REASON ITS HOST CANNOT SEE** — a chain arriving through the running body, say.
	/// The host asks a card its extent only when the host lays out, and a fold already had a road to make it do that;
	/// this is the same road (`onFoldChanged` re-lays every host that binds it), named for what it now carries. A card
	/// calls it once per change, not per frame: the host's re-layout is a whole lane.
	void extentChanged() {
		if (onFoldChanged) {
			onFoldChanged();
		}
	}

public:

	/// WHERE A FOLDED CARD MAY WRITE ITS ONE LINE — the head, less whatever the head's buttons
	/// occupy. Right-aligned text is drawn INTO this, so a card that used the full width would put
	/// its label under them.
	///
	/// ON THE COLLAPSED FILTER, the mode label ("Lowpass" and the rest) moved into the header and overlapped
	/// the active button. It did — the label's box ran to `width() - kPad - 20`, and the enable square starts
	/// at `slotMenuBounds().x() - 6 - 12`, which is further left than that. Both cards had it, and
	/// HARM's was worse because its label ran to the full padding and crossed the dots as well.
	///
	/// DERIVED FROM THE LEFTMOST BUTTON rather than from a guessed constant, so a fourth occupant in
	/// the head moves the text instead of hiding under it.
	visage::Bounds foldedLabelBounds(float leftmostButtonX) const {
		const float right = leftmostButtonX - 6.0f;
		return { 0.0f, 0.0f, std::max(right, 0.0f), kSlotHeadHeight };
	}

	/// ONE OCCUPANT OF THE HEAD, named so a case that finds two overlapping can say which two.
	struct HeadPart {
		const char* what;
		visage::Bounds box;
	};

	/// **THE HEAD, LAID OUT ONCE — WHAT `draw` PAINTS THERE, AS RECTANGLES.**
	///
	/// Text and widgets collided in the headers. Every occupant
	/// placed itself: the buttons from the right edge inwards, the name from the left with the card's
	/// whole width to run in, and a caption right-aligned into a box that began at x = 0 and ended at
	/// the slot menu — so it ran under the enable square and the expand mark, and a long enough one
	/// would have run under the name. No one place knew what else was in the head.
	///
	/// This is that place. The MARKS are the rectangles the base and the host draw (tint bar, source
	/// dot, expand, enable, slot menu, host menu); the NAME gets the room between the dot and the
	/// nearest mark, fitted to it with `fitText`; the CAPTION, if the card has one, gets what the name
	/// leaves. `draw` paints from this and the head sweep asserts on this, so the picture and the
	/// assertion read one layout.
	struct HeadLayout {
		visage::Bounds head;            ///< the head's own rectangle
		std::vector<HeadPart> marks;    ///< everything drawn in it that is not text
		visage::Bounds nameBox;         ///< where the name may be written
		std::string name;               ///< the name as drawn — fitted to `nameBox`, maybe elided
		float nameSize = kControlSize;  ///< the font size it is measured and drawn at
		visage::Bounds captionBox;      ///< where the caption may be written (right-aligned); empty if none
		std::string caption;            ///< the caption as drawn — fitted, maybe empty
		static constexpr float kCaptionSize = kKnobReadoutSize;
	};

	HeadLayout headLayout() const {
		const auto w = static_cast<float>(width());
		const auto h = static_cast<float>(height());
		const visage::Bounds card { 0.0f, 0.0f, w, h };
		const bool spine = headEdge() == CardHeadEdge::Left;
		const device::Descriptor* desc = descriptor();
		const size_t outputs = desc != nullptr ? desc->outputCount() : 0;
		const CardHeadMarks host = tintedHeadMarks();

		HeadLayout out;
		out.head = spine ? visage::Bounds { 0.0f, 0.0f, kCardHeadHeight, h }
		                 : visage::Bounds { 0.0f, 0.0f, w, kCardHeadHeight };
		const auto add = [&out](const char* what, visage::Bounds box) {
			if (box.width() > 0.0f && box.height() > 0.0f) {
				out.marks.push_back({ what, box });
			}
		};
		if (host.tint != 0) {
			add("tint", spine ? visage::Bounds { kTintBarInset, 1.0f, kCardHeadHeight - 2.0f * kTintBarInset,
				                                  kTintBarWidth }
			                  : visage::Bounds { 1.0f, kTintBarInset, kTintBarWidth,
				                                  kCardHeadHeight - 2.0f * kTintBarInset });
		}
		if (outputs > 0) {
			add("source dot", cardSourceDotBounds(card, headEdge()));
		}
		if (!drawsOwnExpandMark()) {
			add("expand", expandBounds());
		}
		add("site", siteBounds());
		add("enable", enableBounds());
		if (hasSlotMenu()) {
			add("slot menu", slotMenuBounds());
		}
		if (host.menu) {
			add("host menu", cardMenuBounds(card, headEdge()));
		}

		// THE NAME STOPS AT THE NEAREST MARK PAST IT: the leftmost on a top head, the topmost of the
		// foot stack on a spine (the tint, the switch and the dot sit ABOVE the band and bound it by
		// `kSpineNameTop`, not by this).
		float stopAt = spine ? h - kModCardPadX : w - kModCardPadX;
		for (const HeadPart& m : out.marks) {
			if (spine && m.box.y() >= kSpineNameTop) {
				stopAt = std::min(stopAt, m.box.y());
			}
			else if (!spine && m.box.x() > kModCardPadX) {
				stopAt = std::min(stopAt, m.box.x());
			}
		}
		out.nameBox = cardNameBounds(card, outputs, headEdge(), stopAt);
		out.nameSize = spine ? kSpineNameSize : kControlSize;
		out.name = fitText(headName(), font(out.nameSize), spine ? out.nameBox.height() : out.nameBox.width());

		// THE CAPTION GETS WHAT THE NAME LEAVES, AND ONLY ON A TOP HEAD — a spine's band is the name's.
		// A gap either side, and nothing at all if what is left cannot hold even an elided word.
		const std::string caption = headCaption();
		if (!spine && !caption.empty()) {
			constexpr float kGap = 8.0f;
			const float nameEnd = out.nameBox.x() + textWidth(out.name, font(out.nameSize));
			const float left = nameEnd + kGap;
			const float right = out.nameBox.right();
			if (right > left) {
				out.captionBox = { left, 0.0f, right - left, kCardHeadHeight };
				out.caption = fitText(caption, font(HeadLayout::kCaptionSize), out.captionBox.width());
			}
		}
		return out;
	}

	/// THE NAME THE HEAD WEARS — the descriptor's. A card with no descriptor overrides it.
	virtual std::string headName() const {
		const device::Descriptor* desc = descriptor();
		return desc != nullptr && desc->traits.name != nullptr ? desc->traits.name : "";
	}

	/// **A SHORT LINE THE HEAD MAY CARRY AFTER THE NAME** — the chain's summary is the one there is.
	/// Default none. It is laid out by `headLayout`, after the name and before the marks, and dropped
	/// before it can crowd either.
	virtual std::string headCaption() const { return {}; }

	void drawHeadCaption(visage::Canvas& canvas, const HeadLayout& head) const {
		if (head.caption.empty()) {
			return;
		}
		canvas.setColor(kDisabled);
		canvas.text(head.caption.c_str(), font(HeadLayout::kCaptionSize), visage::Font::kRight,
		            head.captionBox.x(), head.captionBox.y(), head.captionBox.width(),
		            head.captionBox.height());
	}

	/// EACH CARD'S OWN MOUSE-DOWN CALLS THIS, one line, the way it already calls `slotMenuAt`. The
	/// base cannot install its own press handler because every card replaces `onMouseDown` wholesale
	/// to route its own parts — but drag and release are untouched, so those the base does own.
	///
	/// **THE CLICK COUNT IS WHAT SEPARATES COLLAPSE FROM REORDER.** A double-click expands and a
	/// double-click-and-hold drags; Feathers had conflated collapse and drag.
	///
	/// They were one gesture: ANY press that moved 4px picked the card up, and the fold sits inside
	/// that region. The note twenty lines above records where that led — a fold re-places every card
	/// below it, `e.position` is recomputed against the frame's new place, and **the chain was
	/// reordered by a click that never moved**. That was fixed by subtracting the card's own
	/// displacement, which treats the symptom: the two verbs still shared one press, so any future
	/// motion under a live press would be a reorder again.
	///
	/// **NOW A SINGLE PRESS DOES NEITHER.** A double-click arms the pair; releasing without moving
	/// collapses or expands, holding and moving reorders. One press can no longer mean two things,
	/// which is a stronger guarantee than making one of them hard to trigger by accident.
	/// **NO DEFAULT ON THE CLICK COUNT, DELIBERATELY.** It had one, and every one of the eight call
	/// sites compiled unchanged and went on passing 1 — so the gesture split was in the base, correct,
	/// and reached by nothing. A defaulted parameter is how a behaviour change becomes a no-op that
	/// still builds; requiring the argument makes each caller say what it means.
	/// **DID THIS PRESS LAND ON A HEAD CONTROL?** If it did, the header's own gesture must not also see
	/// it.
	///
	/// Presses were double-counted between the header and its active/minimize boxes: a control's hit
	/// box must block the header's minimize double-click gesture. The head is one
	/// rectangle with several buttons inside it, so a press on a button is also a press on the head —
	/// and the fold gesture happily armed underneath a press meant for the menu. `slotMenuAt`'s own
	/// note states the rule from the other side: *"a button inside the head's rectangle is unclickable
	/// if the head is tested first."* Same rule, applied to the gesture as well as the hit test.
	/// **THE ENABLE SQUARE'S PRESS, ONCE, FOR EVERY CARD (BM).** Each card's mouse-down calls this in
	/// one line, exactly as it already calls `slotMenuAt`: `if (enablePressAt(e.position)) return;`
	///
	/// TEN COPIES OF THIS EXISTED AND THEY WERE NOT EVEN THE SAME COPY, which is the whole argument.
	/// The fx cards toggled through a `DeviceControl` — `beginGesture` / `setValue` / `endGesture` —
	/// and the two MODULATOR cards wrote straight through the host with `setField`. `DeviceControl` is
	/// what records the undo step, so **turning a modulator off was not undoable while turning an
	/// audio effect off was.** Those two host-direct arms were added on 2026-09-02 while fixing the
	/// dead enable presses: the surrounding style was matched without asking what the fx cards' extra
	/// three lines were buying. CLAUDE.md's rule about load-bearing redundancy, met from the other
	/// side — the bracketing read as ceremony and it was the history.
	///
	/// THE THREE BUGS THIS SEAM HAS PRODUCED ARE NOW STRUCTURAL RATHER THAN PER-CARD: a missing
	/// `redraw()` so the light never followed (owltt); a gate on `y < bodyTopEdge()`, correct for
	/// Feathers' top head and dead in the rack's LEFT one, caught by the registry sweep on its first
	/// run; and the missing gesture above. One implementation cannot disagree with itself.
	///
	/// **ASKED OF `enableBounds()` DIRECTLY and never of an edge**, which is what makes it independent
	/// of which side the head is on — one card class serves two hosts with two head geometries.
	/// **RETURNS THE NEW STATE WHEN IT HANDLED THE PRESS, `nullopt` when the press was not on the
	/// square.** It is not a bare bool because half these cards ANNOUNCE the toggle — *"Shaper off"*,
	/// *"Waypoint on"* — and the base holds no notifier. A helper that swallowed the press and
	/// returned nothing would have deleted five announcements silently, which is the sort of quiet
	/// regression a refactor is supposed to be incapable of. The card writes one more line and keeps
	/// its own voice:
	///
	///     if (const auto nowOn = enablePressAt(e.position)) {
	///         if (notifier_ != nullptr) notifier_->say(*nowOn ? "Shaper on" : "Shaper off");
	///         return;
	///     }
	[[nodiscard]] std::optional<bool> enablePressAt(visage::Point position) {
		if (!hasEnable() || !enableBounds().contains(position)) {
			return std::nullopt;
		}
		return toggleEnable();
	}

	/// **THE TOGGLE WITHOUT THE HIT-TEST**, for the cards that route the press through a `Part` enum
	/// and so have already decided it landed on the square — `HarmCard` and `FilterCard` both reach
	/// their switch from a `hitTest` that returned `Part::Enable`, and have no position left to hand
	/// back. Same body, same undo bracketing; `enablePressAt` is this plus the bounds check.
	///
	/// Returns the NEW state, or `nullopt` when there is no enable at all.
	[[nodiscard]] std::optional<bool> toggleEnable() {
		if (!hasEnable()) {
			return std::nullopt;
		}
		device::Modulatable* sw = enableModulatable();
		if (sw == nullptr) {
			// A CARD THAT DECLARES AN ENABLE AND HANDS BACK NO CONTROL SWALLOWS THE PRESS, rather than
			// letting it fall through to the body — the square is drawn, so a press on it must not
			// become a fold. Reporting the state as unchanged is the honest answer: nothing moved. The
			// registry sweep is what stops this being reached.
			return headEnabled();
		}
		const bool on = sw->value() >= 0.5;
		// BRACKETED, WHICH IS THE UNDO STEP — see the note above. `DeviceControl::beginGesture` is
		// where the history entry is recorded; a bare `setValue` writes the field and forgets it.
		sw->beginGesture();
		sw->setValue(on ? 0.0 : 1.0);
		sw->endGesture();
		redraw();
		return !on;
	}

	bool headControlAt(visage::Point position) const {
		return slotMenuAt(position) || enableBounds().contains(position) || siteBounds().contains(position)
		       || expandBounds().contains(position)
		       || (headMarks().menu && cardMenuBounds({ 0.0f, 0.0f, static_cast<float>(width()),
			                                            static_cast<float>(height()) },
		                                              headEdge())
		                                   .contains(position));
	}

	// **`foldOnHeadPress()` IS GONE.** It was the press-time fold road; the verb moved to the RELEASE
	// (see `onMouseUp`) when the fold and the drag had to be told apart, and the function was left as
	// `{ return false; }` with ten card chains still calling it.
	//
	// A NO-OP UNDER A NAME THAT SAYS IT FOLDS IS WORSE THAN NO FUNCTION. Every call discarded the
	// return, so the calls did nothing whatever — but a reader of `case Part::Fold: foldOnHeadPress();`
	// has every reason to believe the press folds the card, and would go looking for the double-toggle
	// bug in the wrong place. That is the same failure step 2 spent an arc on with `slotAt` and
	// `routeLaneOf`, sitting in my own earlier work: a name that outlived its behaviour.
	//
	// THE SWITCH ARMS STAY, because they still have a job — a press on the head must not fall through
	// and be dispatched as a body press. They say so in a comment instead of in a call.


	/// Told by the host so it can move focus to this slot. Fired for ANY press the card sees, head or
	/// body — focus follows the hand, which is what every host this plugin runs in does.
	std::function<void()> onCardPressed;
	/// **THE CARD'S OWN PRESS, WITH THE MODIFIER THE HAND HELD** (backlog OA, D1): plain, Shift (a range from the
	/// anchor) or Ctrl/Cmd (toggle). Fired after `onCardPressed`, only for a press on the card's frame — never a
	/// knob's, which arrives modifier-free through the focus road — and never for a head control (the dots, the
	/// expand mark, the enable, the site chip), which is not a selection gesture.
	std::function<void(CardSelection::Press)> onCardSelectPress;
	/// A RELEASE OF THAT PRESS WHICH DID NOT DRAG.
	std::function<void()> onCardClicked;

	/// **THE PRESS, WITH WHAT THE HAND HELD** — every card's `onMouseDown` funnels here (it was `noteCardPress(e.position,
	/// e.repeat_click_count)` at 31 sites, which dropped the modifiers on the floor).
	void noteCardPress(const visage::MouseEvent& e) {
		noteCardPress(e.position, e.repeat_click_count);
		if (pressConsumed_ || !onCardSelectPress) {
			return;
		}
		const CardSelection::Press kind = e.isShiftDown() ? CardSelection::Press::Range
		                                  : e.isMainModifier() ? CardSelection::Press::Toggle
		                                                       : CardSelection::Press::Plain;
		onCardSelectPress(kind);
	}
	/// **A PRESS ON THE HEAD ITSELF — not on one of its controls, and not the second click of a
	/// double.** Clicking an effect's header focuses its window if one is open. The
	/// host decides what that means (it alone knows whether a window is up); a card with no window
	/// behind it hears this and nothing happens, which keeps the head's existing behaviour.
	std::function<void()> onHeadPressed;

	void noteCardPress(visage::Point position, int repeatClickCount) {
		pressConsumed_ = false;
		pressClicks_ = repeatClickCount;
		if (onCardPressed) {
			onCardPressed();
		}
		// **A PRESS ON A CONTROL IS NOT A PRESS ON THE HEAD.** Without this the double-click that
		// collapses a card also fires when a hand double-clicks the menu or the active switch, which is
		// the double counting that was reported.
		if (headControlAt(position)) {
			// **AND THE EXPAND MARK IS ACTED ON HERE**, because this is the one site every card funnels
			// its head presses through. Six cards each wrote an `else if` for it and all six wrote it in
			// top-head coordinates, so none of them fired on a spine; a branch per card is also a branch
			// per card to forget. `expandAt` is a no-op when the press is elsewhere or there is no panel.
			expandAt(position);
			(void)siteAt(position); // the site chip, by the same funnel (backlog NR)
			// **AND THE HOST'S MARK TOO.** Only `EnvCard` and `MsegCard` ever called
			// `headMarkPressed`; the other eight drew the dots and dispatched nothing, so the mark was
			// dead on those cards no matter which head it was on. Same fix as the expand press: the
			// verb lives where every card already funnels.
			headMarkPressed(position);
			pressConsumed_ = true;
			anchor_.reset();
			dragging_ = false;
			armed_ = false;
			return;
		}
		anchor_ = position;
		// WHERE THE CARD WAS WHEN THE HAND WENT DOWN. The drag subtracts the card's displacement
		// since this instant, so a layout change under a live press cannot masquerade as movement.
		// BOTH AXES, BECAUSE A LANE MAY FLOW EITHER WAY. The rack's lane flows in X and the modulator
		// strip's in Y, and a displacement correction that only watches one of them is blind exactly
		// where its own lane moves things.
		anchorFrameX_ = positionInWindow().x;
		anchorFrameY_ = positionInWindow().y;
		dragging_ = false;
		// **THE SECOND CLICK OF A DOUBLE, OR ANY BEYOND IT, ON THE HEAD.** A triple-click is still a
		// hold-and-drag rather than a third verb — there is no third thing for a card head to do.
		//
		// **HEAD ONLY — the 2026-09-05 decision.** This read
		// `repeatClickCount >= 2` and nothing else, so a double-click ANYWHERE on a card folded it:
		// `noteCardRelease` collapses on "armed and never moved", and the body arms exactly as the head
		// does. MEASURED on a modulator card, three-quarters of the way down its body, before the
		// decision: it folded, 190 px to 24.
		//
		// **THAT IS THE WHOLE OF WHAT WAS REPORTED AS A DELETION:** double-clicking anywhere on a
		// modulator seemed to make the card disappear — the device was never removed and its routes
		// were never cleared because there was nothing to clear, so the modulator went on modulating
		// and read as a ghost. A gesture meant for one of a card's own controls must not put the whole
		// device away.
		//
		// **THROUGH `bodyTopEdge`/`bodyLeft`, WHICH ARE THE CARD'S OWN ANSWER, not a second one.** They
		// are what every card's layout already measures from, and they follow `headEdge()` — so a
		// horizontal lane's spine down the left is tested as the head there without this line knowing
		// which lane it is in. Writing `position.y < kCardHeadHeight` here would be a second spelling
		// of "the head" to keep in step with the first, which is the shape this file's own history
		// names as the source of its duplication bugs.
		//
		// THE DRAG IS UNAFFECTED, and that is checked rather than hoped: `noteCardMove` gates on
		// `anchor_`, which is still set for a body press, and its comment says so explicitly —
		// requiring `armed_` before a drag was removed because a trackpad's tap-tap-hold made reorder
		// unreachable with a mouse. A card is still picked up anywhere; it is only FOLDED at its head.
		armed_ = repeatClickCount >= 2 && pressOnHead(position);
		if (repeatClickCount == 1 && pressOnHead(position) && onHeadPressed) {
			onHeadPressed();
		}
	}

	/// **IS THIS PRESS ON THE CARD'S HEAD?** In the card's own coordinates, against the card's own
	/// body origin — see `bodyLeft` and `bodyTopEdge`, which are where the head's thickness is already
	/// stated once for both lane axes.
	bool pressOnHead(visage::Point position) const noexcept {
		return headEdge() == CardHeadEdge::Left ? position.x < bodyLeft() : position.y < bodyTopEdge();
	}

	/// Whether the press in progress may become a drag or a fold. False for a single click.
	bool cardGestureArmed() const noexcept { return armed_; }

	bool cardDragging() const noexcept { return dragging_; }

private:
	/// **DOES THIS CARD PAINT A MODULATED READING AT ALL? — and if not, it must not repaint for one.**
	///
	/// **WHY THE CONDITION EXISTS, with the measurement named.** An unconditional walk returns true
	/// whenever any modulated value moved, on EVERY card — including cards that paint no modulated
	/// widget. `EnvCard` and `MsegCard` are exactly that: surfaces verified that `ValueBox::draw` reads
	/// only ROUTING facts and prints the BASE by explicit rule (value_box.cpp), and that both cards
	/// build their rows from `host_.fieldAt`. They are also MODULATOR cards, whose controls are
	/// precisely what people route to — so an unconditional walk would repaint them AT BLOCK RATE to
	/// show a number neither of them draws.
	///
	/// That is the cost `publishModulated` refuses the state flag to avoid, quoted twenty lines above:
	/// the channel measured at 108 frames for one meter update, "right for the gestures and patch loads
	/// it was built for and wrong at block rate". A smaller blast radius, the same mistake in kind.
	///
	/// **DERIVED, NOT DECLARED, AND THAT IS THE POINT.** A `virtual bool paintsModulation()` defaulting
	/// to false would be an opt-in a card can forget — which is this bug's own shape, and how
	/// `GenericCard` came to have no `update()` at all. Asking the WIDGETS instead means the answer
	/// cannot drift from what is on screen: a `Knob` is what draws a control's moving ring, so a card
	/// that owns one paints modulation and a card that owns none does not. `GenericCard` builds Knobs,
	/// so it becomes correct the moment it builds its first one, with nobody remembering anything.
	///
	/// **THE LIMIT, SAID PLAINLY: this knows about `Knob` and nothing else.** A future widget that
	/// paints a moving modulated value and is not a `Knob` will not be seen here, and the card will go
	/// quiet in exactly the way this commit is fixing. There is no compiler help for that; if you add
	/// such a widget, add it here, and the case to catch it is a card built over one asserting that
	/// `update()` reports.
	bool paintsModulation() const {
		return hasKnobIn(*this);
	}

	static bool hasKnobIn(const visage::Frame& frame) {
		for (const visage::Frame* child : frame.children()) {
			if (child == nullptr) {
				continue;
			}
			if (dynamic_cast<const Knob*>(child) != nullptr || hasKnobIn(*child)) {
				return true;
			}
		}
		return false;
	}

	/// **DID ANY OF THIS DEVICE'S CONTROLS MOVE UNDER MODULATION SINCE THE LAST ASK?**
	///
	/// Walks the DECLARED modulatable controls and compares `modulatedFieldAt` against what was last
	/// seen. Asked of the descriptor and the host, so it needs to know nothing about which card this
	/// is or what widgets it built — which is exactly why it can live here and the nine per-card
	/// versions could not.
	///
	/// **`redrawAll()`, NOT `redraw()`, AND THAT IS THE SECOND HALF OF THE BUG.** A card's knobs are
	/// separate child frames with no `update()` of their own (the hazard `frame_watchers.h` documents),
	/// so marking this one frame leaves them holding last block's ring.
	///
	/// **AND THE DIFFERENCE IS INVISIBLE ON A SHIP BUILD**: `FEATHERS_FULL_REDRAW` invalidates every
	/// layer once any frame is submitted, so `redraw()` looks identical there. A case built or run
	/// with that flag passes either way and proves nothing — the reduced-motion rule's family, a venue
	/// that cannot express the failure reporting success. Anything asserting this belongs in a DEFAULT
	/// build, and a known-bad putting `redraw()` back must go red there.
	bool modulationMoved() {
		const device::Descriptor* desc = descriptor();
		const backplane::DeviceHost* host = deviceHost();
		if (desc == nullptr || host == nullptr) {
			return false;
		}
		const size_t count = desc->modulatableCount();
		// THE FIRST ASK ALWAYS REPORTS, because nothing has been drawn against these readings yet —
		// the same rule the drive meter's case states for itself.
		bool moved = lastModulated_.size() != count;
		lastModulated_.resize(count, 0.0f);
		for (size_t ordinal = 0; ordinal < count; ++ordinal) {
			const size_t control = desc->controlOfNthModulatable(ordinal);
			const float now = host->modulatedFieldAt(device(), control);
			if (now != lastModulated_[ordinal]) {
				lastModulated_[ordinal] = now;
				moved = true;
			}
		}
		if (moved) {
			redrawAll();
		}
		return moved;
	}

	/// WHERE MODULATION LEFT EACH CONTROL WHEN THIS CARD LAST LOOKED. Indexed by ORDINAL, not by
	/// control, because that is what the walk above iterates and a mismatch between the two would be
	/// a card watching one control and drawing another.
	std::vector<float> lastModulated_;

	/// THE DISABLED VEIL — see `ui::DisabledVeil`. A plain member: `visage::Frame`'s destructor
	/// detaches a frame from its parent, so it may die before this card's base does.
	DisabledVeil veil_;
	/// THE TOOLTIP LAYER — see `ui/tooltip.h`. A plain member for the veil's reason.
	TooltipLayer tips_;
	ModReadoutLayer modReadout_;
	/// THE COURTESY OUTPUT METER (backlog MK) — see `syncOutputMeter`. A plain member for the veil's reason.
	CardOutputMeter meter_;
	float meterPeak_ = -1.0f;
	bool ownHoverInstalled_ = false;
	/// The body the veil was last sized to, so `syncDisabledVeil` writes bounds only when they move.
	visage::Bounds veilBody_ {};

	/// SET BY THE LAST `noteCardPress`: did a head control already answer it? See `foldOnHeadPress`.
	bool focused_ = false;
	bool pressConsumed_ = false;
	/// THE CLICK COUNT OF THE LAST PRESS — see `foldOnHeadPress`, which folds only on the second.
	int pressClicks_ = 0;
	std::optional<visage::Point> anchor_;
	float anchorFrameX_ = 0.0f;
	float anchorFrameY_ = 0.0f;
	bool dragging_ = false;
	bool armed_ = false;

public:
	void drawSlotMenu(visage::Canvas& canvas, uint32_t colour) const {
		// NOT DRAWN WHEN NOTHING WOULD OPEN — see `hasSlotMenu`. Ten cards call this and none of them
		// knows whether its host bound the verb, which is exactly why the question is answered here.
		if (!hasSlotMenu()) {
			return;
		}
		const visage::Bounds box = slotMenuBounds();
		canvas.setColor(colour);
		constexpr float kDot = 2.0f;
		constexpr float kStep = 4.0f;
		const float y = box.y() + (box.height() - kDot) * 0.5f;
		const float x = box.x() + (box.width() - (2.0f * kStep + kDot)) * 0.5f;
		for (int i = 0; i < 3; ++i) {
			canvas.fill(x + static_cast<float>(i) * kStep, y, kDot, kDot);
		}
	}
};

//------------------------------------------------------------------------------------------------//
// ONE CARD'S OUTPUT ARMS, PLACED — the half of the road that is the same in every host.            //
//------------------------------------------------------------------------------------------------//
//
// **THERE WERE THREE COPIES OF THIS AND A FOURTH HOST WITH NONE, WHICH IS HOW THE DEFECT ARRIVED.**
// `FxLane::layOutOutputArms`, `RackEditor::resized` and `ModPanel` each spelled the same six steps —
// the card's own frame moved into the host's coordinates, `outputChipBoundsFor`, the fold test, the
// visibility test, the does-it-fit test, `setBounds` — and `ChainColumn`, extracted out of
// `RackEditor`'s card half during the nested-popup work, was given none of them. So an LFO put in a
// SKLITTER's grain chain drew no chip, and with no chip there was no arm, and with no arm its output
// could not be routed from the surface it was sitting on at all (an LFO in the grain fx had no
// modulation output on its card). The honesty rule failing by
// omission: the device is there, it modulates, and the surface offers no way to reach it.
//
// **WHAT IS SHARED IS THE GEOMETRY AND THE THREE HONESTY TESTS; WHAT IS NOT IS OWNERSHIP.** A host
// still decides how arms are keyed, when they are built and when they are dropped — `FxLane` keys by
// slot and prunes on a label change, `RackEditor` and `ChainColumn` key by device and rebuild
// wholesale — because those genuinely differ and a callback per difference would be a worse seam than
// four lines of loop. What must NOT differ is where a chip goes and whether it is shown, and that is
// all of what lives here.
//
// `arms[i]` is the arm for the device's output `i`, or null where a host has none; every entry is
// told, so a host cannot leave a stale arm visible by skipping it.
inline void layOutCardOutputArms(const FxCard& card, std::span<SourceArm* const> arms) {
	const device::Descriptor* desc = card.descriptor();
	// **THE CARD'S OWN FRAME, MOVED INTO THE HOST'S COORDINATES.** The card reserved this strip itself
	// (`FxCard::outputStripHeightAt`, through `laneExtent`), so asking it is what makes the room and
	// the chips one decision rather than two that have to agree. An arm is the HOST's child and the
	// frame is in the card's coordinates, which is the offset.
	const visage::Bounds box = card.bounds();
	const CardFrame own = card.ownFrame();
	const visage::Bounds strip { own.outputs.x() + box.x(), own.outputs.y() + box.y(),
		                         own.outputs.width(), own.outputs.height() };
	// MEASURED BY THE LABELS THE ARMS PRINT, so a relabelled chip gets its label's width.
	const std::vector<visage::Bounds> chips =
	    desc != nullptr ? outputChipBoundsFor(*desc, strip, card.outputLabels(), card.outputsShown()) : std::vector<visage::Bounds> {};
	// **A FOLDED CARD IS ITS HEAD AND NOTHING ELSE, AND AN INVISIBLE CARD SHOWS NOTHING AT ALL.** The
	// strip is body, so it folds away with the device's surface — `ModPanel`'s rule, and its reason: an
	// arrow that survived a fold would be the head carrying an output again by the back door. The
	// visibility half is the lanes' own: a card scrolled out of view is hidden, and an arm left shown
	// would draw a chip where no card is. (The rack never hides a card, so that half is inert there
	// rather than a change of behaviour.)
	const bool showsBody = card.isVisible() && !card.folded();
	for (size_t output = 0; output < arms.size(); ++output) {
		SourceArm* arm = arms[output];
		if (arm == nullptr) {
			continue;
		}
		// **AND AN EMPTY CHIP IS AN OUTPUT THAT DOES NOT FIT INSIDE THIS CARD** — hidden rather than
		// drawn outside it (the 2026-09-19 decision; `layoutOutputChips` carries it and has
		// the two ways a chip can fail to fit).
		const bool shown = showsBody && output < chips.size() && chips[output].width() > 0.0f;
		arm->setVisible(shown);
		if (!shown) {
			continue;
		}
		const visage::Bounds chip = chips[output];
		arm->setBounds(chip.x(), chip.y(), chip.width(), chip.height());
	}
}

} // namespace feathers::ui
