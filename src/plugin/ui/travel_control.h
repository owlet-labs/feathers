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

#include "backplane/endpoint.h"
#include "device/format_value.h"
#include "plugin/bridge_device_host.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/route_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

// ONE HALF OF A SOUND'S PLACE IN ITS OWN PLANE, AS A CONTROL — Radius or Theta, for one slot.
//
// Drift is polar in the plane: Drift became Radius, a Theta knob joined it, and both are modulation
// destinations. The first delivery made them destinations in the table and in nothing a hand could
// reach, so they showed no mod glow — unlike the similar destinations already present (FM Depth,
// Width).
//
// THAT COMPARISON NAMES WHAT WAS MISSING. FM Depth glows because it is a stock `Knob` over a control that
// answers `display()`; the polar corners were custom-drawn and answered nothing. This is that control.
//
// **IT IS `GammaControl`'S SHAPE WITH `display()` FILLED IN.** That class is the precedent for a
// `Modulatable` over patch state that is not a parameter, and its `display()` returns nothing with
// the note *"NEVER A DESTINATION ... a fact about a quantity with no parameter id"*. The fact changed:
// the core has INTERNAL destinations now (params.h), so a quantity with no parameter id can be one.
//
// THE VALUE IS THE SOUND'S OWN POLAR OFFSET AND THE DESTINATION IS WHAT A ROUTE ADDS TO IT — the split
// every device control has (a base a hand sets, a route that adds to it), on the core. A hand turning
// this writes `SoundTravel::orbit`, which is patch state and spends no parameter; a modulator pointed
// at it writes a route to `Destination::core(travelDestinationIndex(slot, axis))`, and the engine adds
// the two (`pulledPlace`). **IT IS NOT THE POLAR FORM OF THE STAR'S PLACE** — the first build made it
// that, which is wrong: the two are independent by design. The place (`SoundTravel::offset`,
// cartesian, moved by dragging the chart) and this offset are two separate facts about a sound.
//
// THE ROUTING HALF MIRRORS `ParamControl` LINE FOR LINE, deliberately: the gate, the glow, the amount
// gestures and the undo record are the same questions asked of the same bridge, and a sixth seating
// that answered them differently is how one gate comes to have two answers.

namespace feathers::ui {

class TravelControl final : public OwnedControl, public RoutedControl, public RecordsEdits {
public:
	/// HOW FAR ONE FULL THETA DRAG SWINGS THE SOUND ROUND — a quarter turn, so four drags go all the
	/// way round: slow enough that a twitch does not fling the sound across the plane. Not
	/// `ExploreStage::kRotateSpan`, which turns the PLANE; this walks one sound round within it.
	static constexpr double kThetaSpan = 3.14159265358979323846 / 2.0;
	static constexpr double kFullTurn = 2.0 * 3.14159265358979323846;

	/// WHAT THE STAGE DOES AROUND A GESTURE — it owns the undo history of a sound's identity, so it is
	/// told when a hand starts and stops turning this. Either may be empty.
	struct Hooks {
		std::function<void()> began;
		std::function<void(TravelAxis)> ended;
	};

	TravelControl(ParamBridge& bridge, size_t slot, TravelAxis axis, Hooks hooks)
	    : bridge_(bridge), slot_(std::min(slot, kMaxSounds - 1)), axis_(axis), hooks_(std::move(hooks)) {}

	void setModulation(ModState* mod, EditHistory* history) override {
		mod_ = mod;
		history_ = history;
	}
	[[nodiscard]] ModState* routingState() const override { return mod_; }

	/// THE ENGINE'S OWN DESCRIPTOR, not a second one written here: the swing the ring draws
	/// (`Knob::faceZoneSpan` reads `modSpan`) and the swing the route applies
	/// (`ParamSet::internalOffset`) must be one number, and the way to guarantee that is one struct.
	const device::ParamDesc& desc() const override {
		return internalDestinationDesc(travelInternalIndex(slot_, axis_));
	}

	/// **THE SOUND'S OWN POLAR OFFSET, AND NOTHING DERIVED FROM WHERE THE STAR IS** (the two are
	/// independent by design). An earlier build answered `hypot`/`atan2` of the star's
	/// place, so dragging the star turned both knobs and a sound at the origin had no angle to turn.
	double value() const override {
		const PolarOffset orbit = this->orbit();
		return axis_ == TravelAxis::Radius ? orbit.radius : orbit.theta;
	}

	void setValue(double value) override {
		if (!std::isfinite(value)) {
			return;
		}
		PolarOffset orbit = this->orbit();
		if (axis_ == TravelAxis::Radius) {
			// A MAGNITUDE HAS A FLOOR AND THE FLOOR IS REAL: a negative radius is the same point half a
			// turn round, so the knob parks on the sound's place rather than reappearing beyond it.
			orbit.radius = std::clamp(value, 0.0, kTravelRadiusMax);
		}
		else {
			// STORED INSIDE ONE TURN, so the readout and the patch stay legible however far the knob
			// is wound. `remainder` is symmetric about zero; only cos/sin ever read it, so the fold is
			// invisible to the sound. (A MODULATOR's angle is never folded — see `pulledPlace`.)
			orbit.theta = std::remainder(value, kFullTurn);
		}
		bridge_.setSoundOrbit(slot_, orbit);
	}

	/// **THE FACE** (`OwnedControl::faceAt`). Radius is EXPONENTIAL (`radiusFace`, sound_travel.h — the
	/// engine's own, since routes move along it) and bounded; it replaced a
	/// vernier that wrapped once per base travel and followed the zoom; a radius that does not react
	/// to zoom is the deliberate trade. Theta is a COMPASS, zero at the middle
	/// of the sweep, and wraps: `x - floor(x)`, never `fmod`, which keeps its argument's sign and would
	/// pin the negative half to the bottom of the dial.
	std::optional<float> faceAt(double value) const override {
		if (!std::isfinite(value)) {
			return 0.0f;
		}
		if (axis_ == TravelAxis::Radius) {
			return static_cast<float>(radiusFace(value));
		}
		const double turns = value / kFullTurn + 0.5;
		return static_cast<float>(turns - std::floor(turns));
	}

	/// The Radius face's inverse (`OwnedControl::valueAtFace`): the drag runs along the face, so the
	/// curve is under the hand. Theta has none — a compass wraps — and keeps `dragSpan`.
	std::optional<double> valueAtFace(float face) const override {
		if (axis_ != TravelAxis::Radius) {
			return std::nullopt;
		}
		return radiusAtFace(static_cast<double>(face));
	}

	/// **WHERE A ROUTE'S OFFSET PUTS THE FACE** (`OwnedControl::routeFace`), Radius only: a route into Radius
	/// moves ALONG THE FACE, through `playedRadiusFace` — the function the engine's `playedRadius` is built on —
	/// so the ring's edges are where the engine plays the sound at those source values, not a second rule.
	std::optional<float> routeFace(double offset) const override {
		if (axis_ != TravelAxis::Radius) {
			return std::nullopt;
		}
		return static_cast<float>(playedRadiusFace(value(), offset));
	}

	/// **HOW MUCH VALUE ONE SWEEP OF THE FACE IS** (`OwnedControl::faceSweep`) — what Theta's
	/// modulation ring is measured against: one whole turn. Radius's ring is drawn between its end
	/// values instead (a curved face has no one answer), which `valueAtFace` is what asks for. Either
	/// way NOT the declared range: a ring measured against that once drew a full-amount Radius route
	/// as 1/2046 of the face — assigned, working, and invisible.
	std::optional<double> faceSweep() const override {
		return axis_ == TravelAxis::Theta ? std::optional<double>(kFullTurn) : std::nullopt;
	}

	/// **THE GEARING** (`OwnedControl::dragSpan`), Theta only: a quarter turn per full drag, unclamped.
	std::optional<double> dragSpan() const override {
		return axis_ == TravelAxis::Theta ? std::optional<double>(kThetaSpan) : std::nullopt;
	}

	device::ModDisplay display() const override {
		device::ModDisplay out;
		out.base = value();
		// THE LIVE READING IS THE ENGINE'S OWN PULL, added the way the engine adds it (`pulledPlace`):
		// the radius floored at the centre, the angle left unwrapped — `faceAt` folds it onto the
		// compass, so a ramp carries the mark round and round rather than snapping it back.
		const TravelPulls pulls = bridge_.travelPulls();
		out.modulated = axis_ == TravelAxis::Radius ? playedRadius(out.base, pulls.radius[slot_])
		                                            : out.base + pulls.theta[slot_];
		out.isDestination = true;
		const backplane::Destination dest = destination();
		out.armed = mod_ != nullptr && mod_->armed();
		const backplane::Source focused = mod_ != nullptr ? mod_->source() : backplane::Source::core(0);
		out.routable = out.armed && bridge_.mayRoute(focused, dest);
		out.amount = mod_ != nullptr ? bridge_.routeAmount(focused, dest) : 0.0f;
		out.bipolar = mod_ != nullptr && sourceIsBipolarNow(bridge_, focused);
		device::applyTransferToDisplay(out, mod_ != nullptr ? bridge_.routeTransfer(focused, dest) : device::Transfer::Lin);
		out.routedElsewhere = bridge_.routedElsewhere(dest, focused);
		return out;
	}

	float amountFor(device::Source source) const override {
		return bridge_.routeAmount(source, destination());
	}

	void setAmount(float amount) override {
		if (mod_ == nullptr) {
			return;
		}
		// An amount dragged back to nothing is not a route at 0 %, it is GONE — `ParamControl`'s rule.
		const float clamped = std::clamp(amount, -device::kMaxAmount, device::kMaxAmount);
		bridge_.setRouteAmount(mod_->source(), destination(),
		                       std::abs(clamped) < device::kAmountUnroutedBelow ? 0.0f : clamped);
		mod_->touched();
	}

	/// See `RecordsEdits` — the history this control's gestures record into (backlog LS's type-in folds into it).
	[[nodiscard]] EditHistory* recordsInto() const override { return history_; }
	std::string text() const override {
		if (axis_ == TravelAxis::Theta) {
			// THE DESCRIPTOR'S OWN READING (`Unit::Degrees`), so the type-in opens on what the knob shows
			// (`Modulatable::editText` refuses any other text).
			return device::formatValue(desc(), value());
		}
		char printed[24] = {};
		std::snprintf(printed, sizeof(printed), "%.3f", value());
		return printed;
	}

	/// **THETA TYPES IN DEGREES AND DOES NOT STOP AT HALF A TURN.** The descriptor's -pi..pi is where the
	/// stored angle is FOLDED to (`setValue`), not a wall: "270" is a quarter turn back, and clamping it to
	/// 180 would type a different angle than the one asked for. So the parse is the descriptor's, unclamped,
	/// and `setValue` folds.
	[[nodiscard]] std::optional<double> parseText(std::string_view typed, double shown) const override {
		if (axis_ != TravelAxis::Theta) {
			return Modulatable::parseText(typed, shown);
		}
		device::ParamDesc unwound = desc();
		unwound.min = -std::numeric_limits<double>::max();
		unwound.max = std::numeric_limits<double>::max();
		return device::parseValue(unwound, typed, shown);
	}

	void beginGesture() override {
		if (hooks_.began) {
			hooks_.began();
		}
		bridge_.beginSoundTravelGesture();
	}

	void endGesture() override {
		bridge_.endSoundTravelGesture();
		if (hooks_.ended) {
			hooks_.ended(axis_);
		}
	}

	void beginAmountGesture() override {
		amountBefore_ = mod_ != nullptr ? bridge_.routeAmount(mod_->source(), destination()) : 0.0f;
		bridge_.beginRouteGesture();
	}

	void endAmountGesture() override {
		bridge_.endRouteGesture();
		if (history_ == nullptr || mod_ == nullptr) {
			return;
		}
		const float after = bridge_.routeAmount(mod_->source(), destination());
		if (after != amountBefore_) {
			history_->record(std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), destination(),
			                                             amountBefore_, after));
		}
	}

	void unroute() override {
		if (mod_ == nullptr) {
			return;
		}
		const float before = bridge_.routeAmount(mod_->source(), destination());
		if (before == 0.0f) {
			return;
		}
		bridge_.beginRouteGesture();
		bridge_.setRouteAmount(mod_->source(), destination(), 0.0f);
		bridge_.endRouteGesture();
		mod_->touched();
		if (history_ != nullptr) {
			history_->record(
			    std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), destination(), before, 0.0f));
		}
	}

	/// **THIS CONTROL AS A ROUTE'S END** — a core destination in the INTERNAL region of the index space.
	[[nodiscard]] backplane::Destination destination() const {
		return backplane::Destination::core(travelDestinationIndex(slot_, axis_));
	}

private:
	[[nodiscard]] PolarOffset orbit() const { return bridge_.soundTravel().orbit[slot_]; }

	ParamBridge& bridge_;
	size_t slot_;
	TravelAxis axis_;
	Hooks hooks_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	float amountBefore_ = 0.0f;
};

} // namespace feathers::ui
