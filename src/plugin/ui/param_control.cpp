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

#include "plugin/bridge_device_host.h"
#include "plugin/device_registry.h"
#include "plugin/ui/param_edit.h"
#include "plugin/ui/route_edit.h"
#include "plugin/ui/param_control.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace feathers::ui {

namespace {

/// THE UNIT, AS FAR AS IT TRAVELS — and it does not travel far, which is fine here and would not be
/// fine anywhere else.
///
/// The two vocabularies are genuinely different. The plugin's ParamUnit has fourteen members because
/// it prints for a HOST — cents, semitones, decibels, a bend-range index — and device::Unit has six
/// because it describes what a control IS to a device that has never heard of this plugin. There is
/// no total mapping and there should not be one.
///
/// It costs nothing here because ParamControl overrides text() and prints through the plugin's own
/// valueToText, which is the string the DAW shows in its automation lane. Nothing on this road reads
/// the converted unit to print with. It is carried so that desc() answers something honest about the
/// SHAPE of the control — a percentage is a percentage on either road — and anything that cannot be
/// said in the device vocabulary says Number rather than guessing.
constexpr device::Unit convertUnit(ParamUnit unit) {
	switch (unit) {
	case ParamUnit::Percent:
		return device::Unit::Percent;
	case ParamUnit::LogHertz:
		return device::Unit::LogHertz;
	case ParamUnit::LogSeconds:
		return device::Unit::LogSeconds;
	case ParamUnit::Choice:
	case ParamUnit::BendChoice:
		return device::Unit::Choice;
	default:
		return device::Unit::Number;
	}
}

} // namespace

ParamControl::ParamControl(ParamBridge& bridge, const ParamDesc* desc, ModState* mod,
                           EditHistory* history)
    : bridge_(bridge), desc_(desc), mod_(mod), history_(history) {
	if (desc_ == nullptr) {
		return;
	}
	converted_.name = desc_->name;
	converted_.min = desc_->min;
	converted_.max = desc_->max;
	converted_.def = desc_->def;
	converted_.stepped = (desc_->flags & CLAP_PARAM_IS_STEPPED) != 0;
	converted_.unit = convertUnit(desc_->unit);
	converted_.names = desc_->names;
	converted_.stereo = desc_->stereo; // a stereo destination (backlog KU): Phase
	// AND ITS TOOLTIP, which a core row declares as a device control does (voice stacking step 5 found it dropped here:
	// the Engine rail's knobs and boxes answered with the bare name).
	converted_.tooltip = desc_->tooltip;
	// MODULATABLE MEANS "THE ROUTING TABLE HAS A COLUMN FOR IT", asked of the one authority rather
	// than restated: a parameter the host can automate is not necessarily one an internal source may
	// reach, and the mock's six are the list that decides.
	converted_.modulatable = modDestinationIndexOf(desc_->id) >= 0;
}

double ParamControl::value() const {
	return desc_ == nullptr ? 0.0 : bridge_.value(id());
}

void ParamControl::setValue(double value) {
	if (desc_ == nullptr) {
		return;
	}
	bridge_.setValue(id(), std::clamp(value, desc_->min, desc_->max));
}

std::string ParamControl::text() const {
	if (desc_ == nullptr) {
		return {};
	}
	char printed[64] = {};
	if (!valueToText(id(), bridge_.value(id()), printed, sizeof(printed))) {
		return {};
	}
	return printed;
}

std::optional<std::string> ParamControl::editText() const {
	const std::string shown = text();
	if (shown.empty()) {
		return std::nullopt;
	}
	return shown;
}

std::optional<double> ParamControl::parseText(std::string_view text, double) const {
	if (desc_ == nullptr) {
		return std::nullopt;
	}
	double parsed = 0.0;
	const std::string owned(text);
	if (!textToValue(id(), owned.c_str(), &parsed)) {
		return std::nullopt;
	}
	return std::clamp(parsed, desc_->min, desc_->max);
}

int ParamControl::destinationIndex() const {
	return desc_ == nullptr ? -1 : modDestinationIndexOf(desc_->id);
}

std::optional<backplane::Destination> ParamControl::destination() const {
	// **ONE OF FEATHERS' OWN PARAMETERS IS A HOST END** — `Destination::core`, at the index the axes give
	// it, which no device can move (kill-lanes step 2).
	const int dest = destinationIndex();
	if (dest < 0) {
		return std::nullopt;
	}
	return backplane::Destination::core(static_cast<size_t>(dest));
}

float ParamControl::amountFor(device::Source source) const {
	// **A CONTROL THAT IS NOT A DESTINATION REPORTS NO ROUTE FROM ANYBODY**, which is the same
	// refusal `display()` makes and the honest answer rather than a zero that could be mistaken for
	// "routed, at nothing".
	const std::optional<backplane::Destination> dest = destination();
	return dest.has_value() ? bridge_.routeAmount(source, *dest) : 0.0f;
}

device::ModDisplay ParamControl::display() const {
	device::ModDisplay out;
	if (desc_ == nullptr) {
		return out;
	}
	out.base = value();
	// THE LIVE READING IS A FAIR QUESTION ABOUT ANY PARAMETER, destination or not — a macro is a
	// SOURCE and not a destination, and a host modulating one really does move it. Knob learned this
	// the hard way (with a modulated signal routed into an internal macro, the value arc on the macro
	// knob did not move) and the lesson belongs here now that this class owns
	// the reading.
	out.modulated = bridge_.modulated(id());
	// **A STEREO DESTINATION** (backlog KU, 2026-10-05) — the row's capability, which lights the glow's stereo tint, and the
	// right ear as the engine resolved it while a pair reaches it — DeviceControl's three fields, filled the same way.
	out.stereo = desc_->stereo && (!stereoWhen_ || stereoWhen_());
	double right = out.modulated;
	out.stereoLive = out.stereo && bridge_.modulatedRight(id(), right);
	out.modulatedR = out.stereoLive ? right : out.modulated;

	const std::optional<backplane::Destination> destination = this->destination();
	out.isDestination = destination.has_value();
	if (!out.isDestination) {
		return out;
	}
	const backplane::Destination dest = *destination;

	out.armed = mod_ != nullptr && mod_->armed();
	// WITH NO PANEL, NO SOURCE IS FOCUSED — the first host source is what `ModState` starts on, and it
	// is only ever read here to report an amount of zero from a state that does not exist.
	const backplane::Source focused = mod_ != nullptr ? mod_->source() : backplane::Source::core(0);
	// THE GATE AND THE GLOW, ASKED ONCE — see ModDisplay::routable. A weight source may not reach a
	// puck destination, and a knob lit for a drag the bridge would refuse is the honesty rule's exact
	// failure case. The bridge's `mayRoute` is the same authority its `setRouteAmount` asks.
	out.routable = out.armed && bridge_.mayRoute(focused, dest);

	out.amount = mod_ != nullptr ? bridge_.routeAmount(focused, dest) : 0.0f;
	// **AND WHETHER THE ARMED SOURCE SWINGS BOTH WAYS** (backlog CQ). This seating never asked, and it
	// was right by accident for as long as every source a synth knob could see was unipolar: a rack
	// modulator with its polarity switch ON, or a macro in bipolar mode, routed at a synth parameter
	// drew a one-sided reach — half the route. The same question the device seatings ask, of the
	// same seam, so the five seatings that fill a `ModDisplay` cannot disagree.
	out.bipolar = mod_ != nullptr && sourceIsBipolarNow(bridge_, focused);
	device::applyTransferToDisplay(out, mod_ != nullptr ? bridge_.routeTransfer(focused, dest) : device::Transfer::Lin);
	out.routedElsewhere = bridge_.routedElsewhere(dest, focused);
	return out;
}

void ParamControl::setAmount(float amount) {
	const std::optional<backplane::Destination> destination = this->destination();
	if (mod_ == nullptr || !destination.has_value()) {
		return;
	}
	// THE MOCK'S OWN RULE, and the same one RackControl follows: an amount dragged back to nothing is
	// not a route at 0 %, it is GONE. A route that stayed would keep drawing a ring and keep a row in
	// the list for a connection the hand has just taken back.
	const float clamped = std::clamp(amount, -device::kMaxAmount, device::kMaxAmount);
	bridge_.setRouteAmount(mod_->source(), *destination,
	                       std::abs(clamped) < device::kAmountUnroutedBelow ? 0.0f : clamped);
	// The list and the card follow in the same frame — see ModState::touched.
	mod_->touched();
	LiveAmount::touch(&bridge_, mod_->source(), *destination); // the as-modulated readout (backlog PU)
}

std::optional<ModReadout> ParamControl::routeReadout(device::Source source) {
	const std::optional<backplane::Destination> destination = this->destination();
	if (desc_ == nullptr || !destination.has_value()) {
		return std::nullopt;
	}
	// NO ROUTE, NO READOUT.
	const float amount = bridge_.routeAmount(source, *destination);
	if (amount == 0.0f) {
		return std::nullopt;
	}
	// WHAT `ParamSet` READS AROUND IT — named now, answered by the plugin's next block (`Synth::answerRouteProbe`).
	backplane::RouteProbe probe;
	bridge_.routeProbe().watch(source, *destination);
	if (!bridge_.routeProbe().read(source, *destination, probe)) {
		return std::nullopt;
	}
	const ParamDesc& desc = *desc_;
	const clap_id param = id();
	return coreReadoutFor(
	    value(), probe, amount, bridge_.routeTransfer(source, *destination), sourceIsBipolarNow(bridge_, source),
	    &coreRouteTerm, [&desc](double sum) { return landCore(desc, sum); },
	    [param](double v) {
		    char printed[64] = {};
		    return valueToText(param, v, printed, sizeof(printed)) ? std::string(printed) : std::string();
	    });
}

void ParamControl::beginGesture() {
	if (desc_ != nullptr) {
		// WHAT THE VALUE WAS WHEN THE HAND TOOK IT — the bridge shows a move at once, so this is the reading
		// the undo step puts back (DeviceControl's rule).
		valueBefore_ = bridge_.value(id());
		bridge_.beginGesture(id());
	}
}

void ParamControl::endGesture() {
	if (desc_ == nullptr) {
		return;
	}
	bridge_.endGesture(id());
	// **ONE STEP PER GESTURE, AND NONE FOR A GESTURE THAT CHANGED NOTHING** (backlog JB). This used to leave
	// values to "the host's undo" — while the editor's Ctrl+Z walked its own history, so a Ctrl+Z after a
	// knob drag took back an older edit and skipped the drag.
	const double after = bridge_.value(id());
	if (EditHistory* history = recordsInto(); history != nullptr && after != valueBefore_) {
		history->record(std::make_unique<ParamValueEdit>(bridge_, id(), valueBefore_, after));
	}
}

EditHistory* ParamControl::recordsInto() const {
	return history_ != nullptr ? history_ : bridge_.editHistory();
}

void ParamControl::beginAmountGesture() {
	const std::optional<backplane::Destination> destination = this->destination();
	amountBefore_ = (mod_ != nullptr && destination.has_value())
	                    ? bridge_.routeAmount(mod_->source(), *destination)
	                    : 0.0f;
	bridge_.beginRouteGesture();
	if (mod_ != nullptr && destination.has_value()) {
		LiveAmount::begin(&bridge_, mod_->source(), *destination);
	}
}

void ParamControl::endAmountGesture() {
	bridge_.endRouteGesture();
	const std::optional<backplane::Destination> destination = this->destination();
	if (mod_ != nullptr && destination.has_value()) {
		LiveAmount::end(&bridge_, mod_->source(), *destination);
	}
	if (history_ == nullptr || mod_ == nullptr || !destination.has_value()) {
		return;
	}
	const backplane::Destination dest = *destination;
	const float after = bridge_.routeAmount(mod_->source(), dest);
	// ONE STEP PER GESTURE, and none at all for a gesture that changed nothing — the same rule the
	// value road follows, where the host gesture opens on the first value that actually moves.
	if (after != amountBefore_) {
		history_->record(
		    std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), dest, amountBefore_, after));
	}
}

void ParamControl::unroute() {
	const std::optional<backplane::Destination> destination = this->destination();
	if (mod_ == nullptr || !destination.has_value()) {
		return;
	}
	const backplane::Destination dest = *destination;
	const float before = bridge_.routeAmount(mod_->source(), dest);
	if (before == 0.0f) {
		return;
	}
	bridge_.beginRouteGesture();
	bridge_.setRouteAmount(mod_->source(), dest, 0.0f);
	bridge_.endRouteGesture();
	mod_->touched();
	if (history_ != nullptr) {
		history_->record(
		    std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), dest, before, 0.0f));
	}
}

} // namespace feathers::ui
