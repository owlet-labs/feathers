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

#include "plugin/ui/route_edit.h"
#include "plugin/ui/fx_control.h"
#include "plugin/bridge_device_host.h"

#include "plugin/ui/edit_history.h"
#include "plugin/ui/value_box.h"

#include <algorithm>
#include <memory>

namespace feathers::ui {

// **THE DESTINATION IS `(device id, control index)` AND NOTHING IS CONVERTED TO GET IT (kill-lanes step
// 2).** It was computed here through a lane, and that computation is where this adapter's worst defect
// lived: it asked `fxRouteLaneAt(slot_)` — "the lane at POSITION slot_" — after `slot_` had become a lane,
// so after the first reorder every fx card's controls computed their destinations through whichever
// device was DRAWN at their lane number: every route to the audio effects (though not to synth params)
// was lost, and new working routes could not be mapped. An id has no
// second meaning to be read by mistake.

void FxControl::beginGesture() {
	// WHAT THE VALUE WAS WHEN THE HAND TOOK IT. Read through `value()` rather than off the row
	// directly, so a control the row does not reach answers with its declared default and the undo
	// step puts back the same number the card was drawing.
	valueBefore_ = value();
	beginHostGesture(); // the host hears the hand take it, if the control has a slot (host slots, T6b)
}

void FxControl::endGesture() {
	endHostGesture(); // first, and whatever undo does: a host gesture is a pair
	if (history_ == nullptr) {
		return;
	}
	const double after = value();
	// ONE STEP PER GESTURE, AND NONE FOR A GESTURE THAT CHANGED NOTHING — the amount road's rule
	// below, and the value box's. `Knob::commit` opens this bracket on the first value that actually
	// MOVES rather than on the press (`knob.cpp:244`) and closes it on mouse-up, so one drag through
	// five intermediate values is one entry and a click that turns nothing is none.
	if (after != valueBefore_) {
		history_->record(
		    std::make_unique<FxValueEdit>(bridge_, device_, control_, valueBefore_, after));
	}
}

void FxControl::beginAmountGesture() {
	amountBefore_ = (mod_ != nullptr && isDestination()) ? bridge_.routeAmount(mod_->source(), destination())
	                                                     : 0.0f;
	bridge_.beginRouteGesture();
}

void FxControl::endAmountGesture() {
	bridge_.endRouteGesture();
	if (history_ == nullptr || mod_ == nullptr || !isDestination()) {
		return;
	}
	const backplane::Destination dest = destination();
	const float after = bridge_.routeAmount(mod_->source(), dest);
	// ONE STEP PER GESTURE, and none for a gesture that changed nothing — the shape RackControl
	// records, and the one that was missing from the value box until an unused `beforeAmount_` gave
	// it away.
	if (after != amountBefore_) {
		history_->record(
		    std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), dest, amountBefore_, after));
	}
}

std::string FxControl::text() const {
	// PRINTED FROM THE DECLARED UNIT, which is the whole reason device::Unit exists — and the reason
	// a swallowed device loses nothing by having no host formatter behind it.
	return formatValue(desc(), value());
}

void FxControl::setAmount(float amount) {
	if (mod_ == nullptr || !isDestination()) {
		return;
	}
	bridge_.setRouteAmount(mod_->source(), destination(), amount);
	// AND THIS ONE WAS THE ODD IMPLEMENTATION OUT. `Modulatable::setAmount` has four of them and the
	// other three end here — ParamControl, RackControl, MsegControl — which is what `Knob::commitRoute`
	// means by "setAmount notifies the panel itself". That comment names the two it checked.
	//
	// IT WAS PARTLY MASKED, WHICH IS WHY NOBODY SAW IT. An fx CARD is on the state channel
	// (`FxLane` is handed `stateWatchers`, gui.cpp), and `setRouteAmount` raises `repaintDirty_`, so
	// the card under the hand did repaint on the next tick. Nothing on THIS list did: the mod panel's
	// route list draws the amounts too, and it is a ModState watcher and not a state one. So dragging
	// an fx control's amount moved the ring and left the list a gesture behind.
	mod_->touched();
}

device::ModDisplay FxControl::display() const {
	device::ModDisplay out;
	const backplane::Destination dest = destination();
	// A control that is not a destination reports nothing else, so a widget drawing from this can
	// never light for a drag that would do nothing — the honesty rule enforced once, here, rather
	// than at every widget.
	out.isDestination = isDestination();
	out.base = value();
	if (!out.isDestination) {
		out.modulated = out.base;
		return out;
	}

	out.armed = mod_ != nullptr && mod_->armed();
	// THE GATE AND THE GLOW, ASKED ONCE, of the same authority the bridge's write gate uses — so a
	// control cannot light for a drag that would then be refused.
	out.routable = out.armed && bridge_.mayRoute(mod_->source(), dest);
	// THE FOCUSED SOURCE'S POLARITY, from the one place that answers it — the CURRENT one, because a
	// ring draws a reach and a reach is about this block. See `sourceIsBipolarNow`.
	out.bipolar = mod_ != nullptr && sourceIsBipolarNow(bridge_, mod_->source());
	device::applyTransferToDisplay(out, mod_ != nullptr ? bridge_.routeTransfer(mod_->source(), dest) : device::Transfer::Lin);
	// ONE ROUTE, ASKED FOR — one read of the record's graph, never a table built to read one cell of it.
	out.amount = bridge_.routeAmount(mod_ != nullptr ? mod_->source() : backplane::Source::core(0), dest);

	// **WHERE MODULATION ACTUALLY LEFT THIS CONTROL — the engine's own arithmetic, not a second copy
	// of it.** `publishModulated` carries the fx lane as well as the rack (BK), so this is one read of
	// that channel by the device's own position, the way `RackControl` has always read the rack's.
	//
	// THIS DREW THE BASE, AND SAID SO — "THE FX LANE HAS NO SUCH CHANNEL YET". The channel arrived and
	// the line did not move, so every fx card's ring sat still however much was routed at it: the
	// defect BE cost an evening of testing to find on the rack side (2026-09-03: the rings were present
	// but did not move), left standing on the other lane by a comment that had stopped
	// being true. A comment that outlives its behaviour is a thing this codebase keeps paying for.
	//
	// THE PRINTED VALUE AND THE POINTER STAY ON THE BASE — only the moving part of an indicator reads
	// this (`device::ModDisplay::modulated`), which is why `text()` and `out.base` are untouched.
	//
	// **A DEVICE THE LANE DOES NOT HOLD ANSWERS THE BASE, NOT ZERO.** `fxModulatedFieldAt` bounds a
	// slot past the lane to 0.0f, and a ring drawn at zero for a device that is simply not there is a
	// plausible picture of a wrong thing — the same reason that road answers the raw field past the
	// published width rather than silence.
	const size_t at = bridge_.fxSlotOf(device_);
	out.modulated = at != kNoPosition ? static_cast<double>(bridge_.fxModulatedFieldAt(at, control_))
	                                   : out.base;

	// AND WHETHER ANYTHING ELSE REACHES HERE, so a control can show it is driven by a source that is
	// not the armed one. Asked rather than cached: nothing survives this call, so it is never a
	// second copy of the routing table living on the far side of a thread boundary. The edges into
	// this destination ARE the sources that reach it, so this is that walk, not sixty lookups.
	// WITH NO PANEL, EVERY SOURCE IS "ELSEWHERE" — so the exception is a host index nothing publishes.
	out.routedElsewhere =
	    bridge_.routedElsewhere(dest, mod_ != nullptr ? mod_->source() : backplane::Source::nobody());
	return out;
}

} // namespace feathers::ui
