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

#include "plugin/bridge_device_host.h"
#include "plugin/ui/route_edit.h"
#include "plugin/ui/rack_control.h"

// For `device::formatValue`, which lives with the device vocabulary (`device/format_value.h`, host slots
// T3): a function of a device::ParamDesc and a number that belongs to no widget. Both roads of
// Modulatable::text() go through it or through the host formatter.
#include "device/format_value.h"

#include <algorithm>

namespace feathers::ui {

// **A DESTINATION IS `(device id, control index)` (kill-lanes step 2)** — see `destination()`. It was the
// n-th modulatable control of a DEVICE SLOT spanning the fx lane first and the rack after it, reached
// through the rack's lane; every one of those conversions was a place for a rack slot and a lane to be
// confused, and the widget now names the device and its own control.

void RackControl::beginAmountGesture() {
	amountBefore_ = (mod_ != nullptr && isDestination()) ? bridge_.routeAmount(mod_->source(), destination())
	                                                     : 0.0f;
	bridge_.beginRouteGesture();
}

void RackControl::endAmountGesture() {
	bridge_.endRouteGesture();
	if (history_ == nullptr || mod_ == nullptr || !isDestination()) {
		return;
	}
	const backplane::Destination dest = destination();
	const float after = bridge_.routeAmount(mod_->source(), dest);
	// ONE STEP PER GESTURE, and none for a gesture that changed nothing. Until this existed a route
	// dragged onto a device control could not be undone at all: the value box captured where the
	// amount started and then dropped it on the floor.
	if (after != amountBefore_) {
		history_->record(
		    std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), dest, amountBefore_, after));
	}
}

std::string RackControl::text() const {
	// The device road: printed from the DECLARED UNIT, which is the whole reason device::Unit exists.
	return device::formatValue(desc(), value());
}

device::ModDisplay RackControl::display() const {
	device::ModDisplay out;
	const backplane::Destination dest = destination();
	// A control that is not a destination reports nothing else, and a widget drawing from this can
	// never light up for a drag that would do nothing — the honesty rule, enforced at the one place
	// every widget reads rather than at every widget.
	out.isDestination = isDestination();
	out.base = value();
	if (!out.isDestination) {
		out.modulated = out.base;
		return out;
	}

	out.armed = mod_ != nullptr && mod_->armed();
	// THE GATE AND THE GLOW, ASKED ONCE. `ParamBridge::mayRoute` is the same authority the bridge's
	// write gate uses, so a control cannot light for a drag that would then be refused — see ModDisplay.
	out.routable = out.armed && bridge_.mayRoute(mod_->source(), dest);
	const backplane::Source focused = mod_ != nullptr ? mod_->source() : backplane::Source::core(0);
	out.amount = bridge_.routeAmount(focused, dest);
	// THE ENGINE'S OWN ARITHMETIC, not ours — published once a block from the settings the envelopes
	// were actually ticked with. Recomputing it here from the routes and the source readings would be
	// a second implementation of ParamSet::rackOffsetAt on the far side of a thread boundary, free to
	// disagree with the one that makes the sound.
	out.modulated = mod::dahdsr::valueOf(bridge_.modulatedEnvelope(bridge_.rackSlotOf(device_)), param_);

	// AND WHETHER ANYTHING ELSE REACHES HERE. Asked rather than cached: this runs when a control
	// draws, and a cache would be a second copy of the routing table living on the wrong side of a
	// thread boundary — which is the mistake the `modulated` line above exists to avoid making twice.
	//
	// **WITH NO PANEL, EVERY SOURCE IS "ELSEWHERE", which is what the mark MEANS** — something other
	// than the armed source reaches this control, and with no panel nothing is armed. Excepting
	// `focused` here excepted MACRO 1 (`Source::core(0)`, the no-panel default), so a control driven by
	// macro 1 alone reported itself undriven — the honesty rule failing quietly, and one of two answers
	// to one question. `DeviceControl` and `FxControl` already except a source nobody publishes; this is
	// that. (Four copies of this constant now: it wants one home, in nobody's file this week.)
	out.routedElsewhere =
	    bridge_.routedElsewhere(dest, mod_ != nullptr ? mod_->source() : backplane::Source::nobody());
	// THE ARMED SOURCE'S CURRENT POLARITY, asked of the same seam as every other seating (backlog CQ).
	// A bipolar macro (backlog EG) or a switched-on envelope routed at a rack control used to draw a
	// one-sided reach here, understating the route by half.
	out.bipolar = mod_ != nullptr && sourceIsBipolarNow(bridge_, mod_->source());
	device::applyTransferToDisplay(out, mod_ != nullptr ? bridge_.routeTransfer(mod_->source(), dest) : device::Transfer::Lin);
	return out;
}

void RackControl::setAmount(float amount) {
	if (!isDestination() || mod_ == nullptr) {
		return;
	}
	const backplane::Destination dest = destination();
	// THE SAME ROAD A KNOB'S RING TAKES, including the rule that an amount dragged back to nothing is
	// GONE rather than a route sitting at zero — otherwise the route list would accumulate entries
	// that reach nowhere and the card would keep drawing a bar of zero width.
	const float clamped = std::clamp(amount, -device::kMaxAmount, device::kMaxAmount);
	bridge_.setRouteAmount(mod_->source(), dest,
	                       std::abs(clamped) < device::kAmountUnroutedBelow ? 0.0f : clamped);
	// The strip, the route list and every other control redraw in the same frame — the amounts live
	// on the bridge, which has no watcher list of its own, so this is the one notification they share.
	mod_->touched();
}

} // namespace feathers::ui
