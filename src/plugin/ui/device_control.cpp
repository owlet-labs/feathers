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

#include "plugin/ui/device_control.h"
#include "backplane/sources.h"

#include "plugin/ui/value_box.h"

#include <cmath>
#include <memory>

namespace feathers::ui {

void DeviceControl::beginGesture() {
	// WHAT THE VALUE WAS WHEN THE HAND TOOK IT — read through `value()` rather than off the host, so a
	// control the row does not reach answers with its declared default and the undo step puts back the
	// same number the card was drawing.
	valueBefore_ = value();
	// AND THE HOST HEARS THE HAND TAKE IT, if the control is one of its parameters (host slots, T6b): the
	// `setValue`s between here and `endGesture` then record into its lane as one gesture.
	beginHostGesture();
}

void DeviceControl::beginHostGesture() {
	if (!hostGesture_) {
		hostGesture_ = true;
		// A HAND, so the recent ring hears it FIRST (host slots, T9): an unslotted control gets its slot here,
		// and the Begin below then finds the control the pool now names.
		host_.touchControl(device_, control_);
		host_.beginControlGesture(device_, control_);
	}
}

void DeviceControl::endHostGesture() {
	if (hostGesture_) {
		hostGesture_ = false;
		host_.endControlGesture(device_, control_);
	}
}

void DeviceControl::endGesture() {
	// FIRST, and whether or not there is a history: the host's gesture is a pair whatever undo does.
	endHostGesture();
	if (history_ == nullptr) {
		return;
	}
	const double after = value();
	// ONE STEP PER GESTURE, AND NONE FOR A GESTURE THAT CHANGED NOTHING.
	if (after != valueBefore_) {
		history_->record(
		    std::make_unique<DeviceValueEdit>(host_, device_, control_, valueBefore_, after));
	}
}

void DeviceControl::beginAmountGesture() {
	amountBefore_ = (mod_ != nullptr && isDestination()) ? host_.routeAmount(mod_->source(), destination()) : 0.0f;
	if (mod_ != nullptr && isDestination()) {
		LiveAmount::begin(host_.routeIdentity(), mod_->source(), destination()); // the as-modulated readout (backlog PU)
	}
	host_.beginRouteGesture();
}

void DeviceControl::endAmountGesture() {
	host_.endRouteGesture();
	if (mod_ != nullptr && isDestination()) {
		LiveAmount::end(host_.routeIdentity(), mod_->source(), destination());
	}
	if (history_ == nullptr || mod_ == nullptr || !isDestination()) {
		return;
	}
	const float after = host_.routeAmount(mod_->source(), destination());
	if (after != amountBefore_) {
		history_->record(std::make_unique<DeviceRouteEdit>(host_, *mod_, mod_->source(), destination(),
		                                                  amountBefore_, after));
	}
}

void DeviceControl::setAmount(float amount) {
	if (mod_ == nullptr || !isDestination()) {
		return;
	}
	host_.setRouteAmount(mod_->source(), destination(), amount);
	// THE PANEL IS TOLD BY THE CONTROL, not by the widget. `Knob::commitRoute` relies on this — the
	// route list is a ModState watcher and not a state one, so a control that wrote an amount without
	// saying so left the list a gesture behind. That was a real defect on this road once.
	mod_->touched();
	// AND THE AS-MODULATED READOUT (backlog PU) — every face's amount road ends here, a gesture round it or not.
	LiveAmount::touch(host_.routeIdentity(), mod_->source(), destination());
}

std::optional<ModReadout> DeviceControl::routeReadout(device::Source source) {
	if (!isDestination() || control_ >= desc_->params.size()) {
		return std::nullopt;
	}
	const backplane::Destination d = destination();
	// NO ROUTE, NO READOUT: an amount dragged back to nothing is a route that is gone.
	const float amount = host_.routeAmount(source, d);
	if (amount == 0.0f) {
		return std::nullopt;
	}
	// WHAT THE ENGINE READS AROUND IT — or nothing, from a host that cannot say (`DeviceHost::probeRoute`).
	backplane::RouteProbe probe;
	if (!host_.probeRoute(source, d, probe)) {
		return std::nullopt;
	}
	return readoutFor(desc(), value(), probe, amount, host_.routeTransfer(source, d), host_.sourceIsBipolarNow(source),
	                  [this](double v) { return textAt(v); });
}

std::string DeviceControl::text() const {
	return textAt(value());
}

std::string DeviceControl::textAt(double at) const {
	// **THE DEVICE FIRST, IF IT HAS SOMETHING TO SAY** (backlog GS) — a control whose meaning depends on
	// the device's state, which no unit can print. A device with no hook, or a hook that declines this
	// control, falls through to the unit exactly as before.
	if (desc_->text != nullptr && desc_->text->format != nullptr) {
		char out[32] = {};
		const device::TextView view { this, &viewField, &viewReadingCount, &viewReading, host_.containerSurfaceOf(device_) };
		if (desc_->text->format(control_, at, view, out, sizeof(out))) {
			return out;
		}
	}
	// PRINTED FROM THE DECLARED UNIT, which is the whole reason `device::Unit` exists — and the reason
	// a swallowed device loses nothing by having no host formatter behind it.
	return formatValue(desc(), at);
}

std::optional<std::string> DeviceControl::editText() const {
	if (!typeable()) {
		return std::nullopt;
	}
	// A DEVICE THAT READS ITS OWN WORDS BACK (`TextOps::parse`) is typed into in them, where it says any: the field opens
	// on what it shows. Where its hook declines, the unit prints, and the unit's own edit text is the one that reads back.
	if (desc_->text != nullptr && desc_->text->format != nullptr && desc_->text->parse != nullptr) {
		char out[32] = {};
		const device::TextView view { this, &viewField, &viewReadingCount, &viewReading, host_.containerSurfaceOf(device_) };
		if (desc_->text->format(control_, value(), view, out, sizeof(out))) {
			return std::string(out);
		}
	}
	return device::editText(desc(), value());
}

std::optional<double> DeviceControl::parseText(std::string_view typed, double shown) const {
	if (desc_->text != nullptr && desc_->text->parse != nullptr) {
		const std::string text(typed);
		const device::TextView view { this, &viewField, &viewReadingCount, &viewReading, host_.containerSurfaceOf(device_) };
		double value = 0.0;
		if (desc_->text->parse(control_, text.c_str(), view, &value)) {
			return std::clamp(value, desc().min, desc().max);
		}
	}
	return Modulatable::parseText(typed, shown);
}

bool DeviceControl::typeable() const {
	if (desc_->text == nullptr || desc_->text->format == nullptr) {
		return true; // printed by the declared unit, which `device::parseValue` inverts
	}
	if (desc_->text->parse != nullptr) {
		return true; // the device reads back what it prints, and hands the rest to the unit
	}
	// THE DEVICE'S OWN WORDS MUST READ BACK TO THEMSELVES, wherever it says any: every reading the hook
	// claims, parsed in the declared unit, must print the same reading again. "1/16" does not parse at all;
	// a swing printed as a ratio of something else parses to a value that prints differently. Either way
	// the hand would be typing in a unit the control is not showing.
	const device::ParamDesc& p = desc();
	const double span = p.max - p.min;
	if (!(span > 0.0)) {
		return false;
	}
	const device::TextView view { this, &viewField, &viewReadingCount, &viewReading, host_.containerSurfaceOf(device_) };
	constexpr int kSamples = 64;
	for (int k = 0; k <= kSamples + 1; ++k) {
		double v = k > kSamples ? p.def : p.min + span * static_cast<double>(k) / kSamples;
		if (p.stepped) {
			v = std::round(v);
		}
		char out[32] = {};
		if (!desc_->text->format(control_, v, view, out, sizeof(out))) {
			continue; // the hook declined here: the unit prints this reading
		}
		const std::optional<double> back = device::parseValue(p, out, v);
		if (!back || textAt(*back) != out) {
			return false;
		}
	}
	return true;
}

float DeviceControl::viewField(const void* host, size_t control) {
	const auto& self = *static_cast<const DeviceControl*>(host);
	if (control >= self.desc_->params.size()) {
		return 0.0f;
	}
	// ABSENT IS THE DECLARED DEFAULT, NEVER ZERO — the same rule `value()` applies to its own field.
	return control < self.host_.fieldsPresentAt(self.device_)
	           ? self.host_.fieldAt(self.device_, control)
	           : static_cast<float>(self.desc_->params[control].def);
}

size_t DeviceControl::viewReadingCount(const void* host) {
	const auto& self = *static_cast<const DeviceControl*>(host);
	return self.host_.readingCountAt(self.device_);
}

float DeviceControl::viewReading(const void* host, size_t index) {
	const auto& self = *static_cast<const DeviceControl*>(host);
	return self.host_.readingAt(self.device_, index);
}

device::ModDisplay DeviceControl::display() const {
	device::ModDisplay out;
	const backplane::Destination dest = destination();
	// A control that is not a destination reports nothing else, so a widget drawing from this can
	// never light for a drag that would do nothing — the honesty rule enforced once, here, rather than
	// at every widget.
	out.isDestination = isDestination();
	out.base = value();
	// A CAPABILITY OF THE DECLARATION (backlog KU), so it is known before anything is asked of the host.
	out.stereo = desc().stereo;
	if (!out.isDestination) {
		out.modulated = out.base;
		out.modulatedR = out.modulated;
		return out;
	}

	out.armed = mod_ != nullptr && mod_->armed();
	// THIS SEATING HAS A HOST, so it asks the host rather than Feathers' own tables — it is the seam a
	// rack device is drawn through and must not assume whose editor it is in.
	//
	// **AND IT ASKS WHAT THE SOURCE IS DOING NOW, NOT WHAT IT DECLARES.** This read
	// `SourceView::isBipolarAt`, which answers the WIDEST signal the output can publish — right for
	// routing, wrong for a ring. The DAHDSR declares bipolar with its switch defaulting OFF, so an
	// envelope publishing 0..1 drew a symmetric reach: `[base − a, base + a]` for an amount that can
	// only pull one way, which made a negative amount indistinguishable from a positive one: positive
	// modulation was applied negatively and pulled below the knob's notch. See
	// `DeviceHost::sourceIsBipolarNow`, whose default is still the declaration for a host that cannot
	// read a switch.
	out.bipolar = mod_ != nullptr && host_.sourceIsBipolarNow(mod_->source());
	device::applyTransferToDisplay(out, mod_ != nullptr ? host_.routeTransfer(mod_->source(), dest) : device::Transfer::Lin);
	// THE GATE AND THE GLOW, ASKED ONCE, of the same authority that gates the write — so a control
	// cannot light for a drag that would then be refused. `DeviceHost::mayRoute` is non-virtual for
	// exactly this reason.
	out.routable = out.armed && host_.mayRoute(mod_->source(), dest);
	out.amount = host_.routeAmount(mod_ != nullptr ? mod_->source() : backplane::Source::core(0), dest);

	// **ASKED OF THE HOST, WHICH IS THE ROAD THAT USED TO BE MISSING.**
	//
	// This read `out.modulated = out.base`, with the reason stated here: a host that publishes where
	// modulation actually left each control lets the editor draw the ENGINE'S arithmetic, and
	// recomputing it here from the routes and the source readings would be a second implementation of
	// it across a thread boundary. That reasoning stands; what changed is that a host now answers.
	//
	// **AND A HOST THAT DOES NOT STILL ANSWERS THE BASE**, because `DeviceHost::modulatedFieldAt`
	// defaults to `fieldAt`. So Feathers is unchanged until `Synth` grows the same road, and the arc
	// there sits where it always did rather than a surface inventing a number.
	//
	// **THE GRADE OF THIS READ, so nobody later mistakes it for the transport road:** one relaxed
	// atomic load per control, with no epoch around the set. Two controls painted in the same frame
	// may therefore have come from adjacent blocks, and a control read twice in one frame may differ.
	// That is fine for an arc — it is a picture of a moving quantity, a frame late at worst — and it is
	// NOT good enough for anything that must be consistent ACROSS controls. Nothing draws that today;
	// if something needs it, it needs a block epoch and this is not it.
	out.modulated = static_cast<double>(host_.modulatedFieldAt(device_, control_));

	// THE RIGHT EAR, asked of the host by the same road as the left (backlog KU). False — mono — for a host with no
	// right-ear channel, which is `DeviceHost`'s default: the ring then draws as it always has.
	float right = 0.0f;
	out.stereoLive = out.stereo && host_.modulatedFieldRAt(device_, control_, right);
	out.modulatedR = out.stereoLive ? static_cast<double>(right) : out.modulated;

	// AND WHETHER ANYTHING ELSE REACHES HERE, so a control can disclose that it is driven by a source
	// which is not the armed one. Asked of the host, which holds the routes — never cached, because a
	// cache would be a second copy of the routing on the far side of a thread boundary. (It was a walk
	// of every flat source index here; there is no flat index now, and the host answers in one pass.)
	// WITH NO PANEL, EVERY SOURCE IS "ELSEWHERE" — so the exception is a host index nothing publishes.
	out.routedElsewhere =
	    host_.routedElsewhere(dest, mod_ != nullptr ? mod_->source() : backplane::Source::nobody());
	return out;
}

} // namespace feathers::ui
