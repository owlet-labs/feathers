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

#include "plugin/ui/route_edit.h"
#include "plugin/ui/mseg_control.h"
#include "plugin/bridge_device_host.h"

#include "plugin/ui/value_box.h" // formatValue

#include <algorithm>
#include <cmath>

namespace feathers::ui {

// **THE DESTINATION IS `(device id, control index)`** — see `destination()` — and whether the control
// may be one is the declaration of the device actually under the id. That is the generic form the old
// comment here waited for: it once took the DAHDSR's ordinal for an MSEG's control 5 (a compile-time
// `isFxDeviceSlot(slot) ? filter : dahdsr` that never consulted the rack) and aimed a route at an MSEG's
// Rate into the column reserved for an envelope's Release. No ordinal is computed now, anywhere.

device::ModDisplay MsegControl::display() const {
	device::ModDisplay out;
	const backplane::Destination dest = destination();
	out.isDestination = isDestination();
	out.base = value();

	// THE ENGINE DOES NOT TICK MSEGs YET, SO THERE IS NO MODULATED READING TO HAVE.
	//
	// Synth's rack ticks DAHDSRs; nothing in it knows what an MSEG is. The modulated channel is wide
	// enough to carry one — envModulated_ is kMaxDeviceFields across since the heterogeneity landed —
	// but nothing writes an MSEG's row into it, so decoding that row as MSEG settings would hand this
	// card a DAHDSR's fifteen numbers reinterpreted as an MSEG's thirty-seven. That is not a modulated
	// reading, it is noise wearing the right type.
	//
	// So `modulated` is the base, and this is a DECLARED LIMIT rather than a value. When the engine
	// learns to tick an MSEG it will publish into that channel and this line becomes
	// `mod::mseg::valueOf(bridge_.modulatedMseg(bridge_.rackSlotOf(device_)), param_)` — one line, at
	// the one place, and the card starts moving without anything else changing.
	out.modulated = out.base;

	if (!out.isDestination) {
		return out;
	}

	out.armed = mod_ != nullptr && mod_->armed();
	const backplane::Source focused = mod_ != nullptr ? mod_->source() : backplane::Source::core(0);
	out.routable = out.armed && bridge_.mayRoute(focused, dest);
	out.amount = bridge_.routeAmount(focused, dest);
	// THE FOCUSED SOURCE'S POLARITY, beside its amount, from the one place that answers it — and it is
	// the CURRENT one rather than the declared widest, because this fills a ring. See
	// `sourceIsBipolarNow`; `sourceIsBipolar` is the routing question and is not this one.
	out.bipolar = mod_ != nullptr && sourceIsBipolarNow(bridge_, mod_->source());
	device::applyTransferToDisplay(out, mod_ != nullptr ? bridge_.routeTransfer(mod_->source(), dest) : device::Transfer::Lin);

	// **WITH NO PANEL, EVERY SOURCE IS "ELSEWHERE"** — the mark means "something other than the armed
	// source reaches this", and with no panel nothing is armed. Excepting `focused` excepted MACRO 1
	// (its no-panel default), so a control driven by macro 1 alone reported itself undriven. See
	// `RackControl::display` for the same line and the same reason.
	out.routedElsewhere =
	    bridge_.routedElsewhere(dest, mod_ != nullptr ? mod_->source() : backplane::Source::nobody());
	return out;
}

void MsegControl::setAmount(float amount) {
	if (!isDestination() || mod_ == nullptr) {
		return;
	}
	const float clamped = std::clamp(amount, -device::kMaxAmount, device::kMaxAmount);
	bridge_.setRouteAmount(mod_->source(), destination(),
	                       std::abs(clamped) < device::kAmountUnroutedBelow ? 0.0f : clamped);
	mod_->touched();
}

void MsegControl::beginAmountGesture() {
	amountBefore_ = (mod_ != nullptr && isDestination()) ? bridge_.routeAmount(mod_->source(), destination())
	                                                     : 0.0f;
	bridge_.beginRouteGesture();
}

void MsegControl::endAmountGesture() {
	bridge_.endRouteGesture();
	if (history_ == nullptr || mod_ == nullptr || !isDestination()) {
		return;
	}
	const backplane::Destination dest = destination();
	const float after = bridge_.routeAmount(mod_->source(), dest);
	if (after != amountBefore_) {
		history_->record(
		    std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), dest, amountBefore_, after));
	}
}

std::string MsegControl::text() const {
	return formatValue(desc(), value());
}

} // namespace feathers::ui
