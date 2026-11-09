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

#include "device/modulatable.h"
#include "plugin/gui_bridge.h"
#include "plugin/sound_gamma.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/route_edit.h"

#include <cstddef>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>

// THE SELECTED SOUND'S GAMMA, AS ONE CONTROL — reads the whole quantity, writes the half that can
// absorb it.
//
// **IT WAS HALF OF A COARSE:FINE PAIR FOR ABOUT AN HOUR, AND THE CODE IS WHAT SHOWED THAT WRONG.**
// The pair was a deliberate design, approved on screen; what changed is the decomposition
// underneath. `slice_build.h:239`:
//
//     sliceHome(zone, phaseOffset, gammaBase) = zone/1023.0 + phaseOffset + gammaBase
//
// **THREE terms on ONE axis** — phase, phase offset, and gamma. Gamma is a THIRD term, not the coarse
// half of anything. The pair had put `gammaBase` beside `GlobalGamma` and called them coarse and
// fine, and they are not: they are two different quantities sharing a display. The decision: device
// gamma for Explore (not for effects) is one number in one value box, added to the phase offset
// (the coarse) and the phase (the fine).
//
// So: ONE box. The zoned devices keep their `ZoneCoordBox` — this does not apply to effects — because
// there the two halves really are one line at two granularities.
//
// WHAT THIS IS FOR. `sound_gamma.h` splits gamma into a bounded VERNIER that keeps the parameter id
// and an unbounded BASE that deliberately has none: *"no host would know how to draw an unbounded
// range and no automation lane could hold it."* The base has been reachable only through
// `ExploreStage::nudgeSelectedGamma` off the corner encoder — **a quantity a hand can move and cannot
// read**, which is exactly the failure a value box exists to fix.
//
// **READS THE SUM, WRITES THE BASE — and this split was an implementation call rather than a reviewed
// decision, so it is the first thing to overturn at review if it reads wrong.** Put on record here because a
// decision nobody can find is a decision nobody can reverse.
//
//   READ  `per-sound Gamma + GlobalGamma + base` (`explore_stage.cpp:604`) — the number that means
//         something musically: where this sound's gamma actually IS. A box showing only the base
//         would read zero on a sound whose gamma is plainly not zero.
//   WRITE the base, and only the base. It is the sole term that can absorb any value, being
//         unbounded and id-less; the other two are bounded parameters with automation lanes, and a
//         typed number that silently rewrote one of those would be a widget editing a host's
//         automation target behind its back.
//
// So a typed `4.0` moves the base by whatever it takes to make the SUM read 4.0 — which is what
// "type where you want to be" means on a quantity assembled from three terms.
//
// **THE CONSEQUENCE, ACCEPTED RATHER THAN OVERLOOKED: `GlobalGamma` NO LONGER HAS A HAND-REACHABLE
// WIDGET.** It keeps its id, its ±4 range, its automation and its modulation routing, and it still
// moves this readout because it is in the sum — but no dial or box on any surface writes it any
// more. That was raised explicitly before this was built and the decision was to ship it and
// review at install. It is a known trade, not an oversight, and it is the second thing to overturn
// if the review goes the other way.
//
// NOT A PARAMETER, AND THAT IS THE WHOLE POINT OF THE SPLIT. `desc()` reports no id and
// `modulatable = false`, so every surface that asks "is this a destination?" gets the right answer by
// construction rather than by a special case: `ZoneCoordBox` draws no modulation bar on it, no route
// can be pointed at it, and the arming affordance never appears.
//
// `double` END TO END. `sound_gamma.h` is explicit that a base folded into a float *"would quantise:
// at a base of 1e5 a float ULP is about eight zones, so the vernier's whole range would vanish
// exactly where 'unbounded' starts to mean something."* `device::Modulatable` is a double interface,
// so nothing here narrows it.
//
// PER SOUND, AND THE SLOT IS ASKED FOR RATHER THAN REMEMBERED. The base is `base[slot]` and the
// selection moves under the widget; a slot captured at construction would quietly edit whichever
// sound happened to be selected when the tray was built.

namespace feathers::ui {

class GammaControl final : public device::Modulatable, public RoutedControl {
public:
	/// @param slotOf where the selection is NOW — read on every access, never cached. See above.
	GammaControl(ParamBridge& bridge, std::function<size_t()> slotOf)
	    : bridge_(bridge), slotOf_(std::move(slotOf)) {}

	const device::ParamDesc& desc() const override {
		// A WINDOW, NOT A LIMIT. The declared range is what a drag and a box can express by hand; the
		// quantity itself is unbounded and the engine never clamps it. Stated wide enough that no
		// ordinary session reaches an end, and narrow enough that a full drag is not absurd.
		static const device::ParamDesc kDesc { "gamma", "Gamma",     -1024.0, 1024.0, 0.0, false,
			                                   device::Unit::Number, {} };
		return kDesc;
	}

	/// THE WHOLE QUANTITY — the same sum `ExploreStage::gammaSum()` builds, and deliberately the same
	/// three terms rather than a second opinion about what gamma is.
	double value() const override {
		return bridge_.value(static_cast<clap_id>(soundParam(slot(), SoundField::Gamma)))
		       + bridge_.value(static_cast<clap_id>(ParamId::GlobalGamma))
		       + bridge_.soundGamma().base[slot()];
	}

	void setValue(double value) override {
		// THE DIFFERENCE GOES INTO THE BASE. `value` is a target for the SUM, so the base takes
		// whatever the other two do not account for — which is what makes typing a number land you at
		// that gamma rather than at that gamma plus whatever the global happened to be.
		//
		// THROUGH THE STATE OF RECORD, exactly as `nudgeSelectedGamma` does: the stored array is
		// rewritten from itself rather than accumulated into, so a breadcrumb restores the number
		// instead of replaying the increments that produced it.
		const double others =
		    bridge_.value(static_cast<clap_id>(soundParam(slot(), SoundField::Gamma)))
		    + bridge_.value(static_cast<clap_id>(ParamId::GlobalGamma));
		SoundGamma gamma = bridge_.soundGamma();
		gamma.base[slot()] = value - others;
		bridge_.setSoundGamma(slot(), gamma.base[slot()]);
	}

	/// THE BASE ALONE, for the cases that need to see which term actually moved. Not part of the
	/// interface: a widget reading this would be reading past the abstraction the class exists to
	/// present.
	[[nodiscard]] double baseForTest() const { return bridge_.soundGamma().base[slot()]; }

	/// **A RELATIVE MACRO'S DESTINATION, AND ONLY ITS (backlog PQ).** The sound's Gamma is an edit-only destination
	/// (`isEditOnlyDestination`): a Relative macro's route there carries each turn into the unbounded half this box
	/// reads, and nothing else may route here (`ParamBridge::mayReachEditOnly`). So the box is a destination — it
	/// glows, and a press while a Relative macro is armed sets the route's depth — but `routable` is false for any
	/// other armed source, so it never glows for a drag the bridge would refuse.
	device::ModDisplay display() const override {
		device::ModDisplay out;
		out.base = value();
		out.modulated = out.base; // nothing modulates an identity per block; the turn edits the base itself
		out.isDestination = true;
		const backplane::Destination dest = destination();
		out.armed = mod_ != nullptr && mod_->armed();
		const backplane::Source focused = mod_ != nullptr ? mod_->source() : backplane::Source::nobody();
		out.routable = out.armed && bridge_.mayRoute(focused, dest);
		out.amount = mod_ != nullptr ? bridge_.routeAmount(focused, dest) : 0.0f;
		out.routedElsewhere = bridge_.routedElsewhere(dest, focused);
		return out;
	}

	float amountFor(device::Source source) const override { return bridge_.routeAmount(source, destination()); }

	void setAmount(float amount) override {
		if (mod_ == nullptr) {
			return;
		}
		const float clamped = std::clamp(amount, -device::kMaxAmount, device::kMaxAmount);
		bridge_.setRouteAmount(mod_->source(), destination(),
		                       std::abs(clamped) < device::kAmountUnroutedBelow ? 0.0f : clamped);
		mod_->touched();
	}

	/// The panel state and history — `KnobTray::setModulation`.
	void setModulation(ModState* state, EditHistory* history) {
		mod_ = state;
		history_ = history;
	}
	[[nodiscard]] ModState* routingState() const override { return mod_; }

	/// The selected sound's Gamma, as a core destination (edit-only).
	[[nodiscard]] backplane::Destination destination() const {
		return backplane::Destination::core(
		    static_cast<size_t>(modDestinationIndexOf(soundParam(slot(), SoundField::Gamma))));
	}

	std::string text() const override {
		// A COUNT, NOT A MEASUREMENT — the same reading `ZoneCoordBox` gives its coarse box. Whole
		// steps along the sound's own line, which is the unit `sound_gamma.h` declares.
		return std::to_string(static_cast<long>(std::llround(value())));
	}

	void beginGesture() override { bridge_.beginSoundGammaGesture(); }
	void endGesture() override { bridge_.endSoundGammaGesture(); }
	void beginAmountGesture() override {
		amountBefore_ = mod_ != nullptr ? bridge_.routeAmount(mod_->source(), destination()) : 0.0f;
		amountDestination_ = destination();
		bridge_.beginRouteGesture();
	}
	void endAmountGesture() override {
		bridge_.endRouteGesture();
		if (history_ == nullptr || mod_ == nullptr) {
			return;
		}
		const float after = bridge_.routeAmount(mod_->source(), amountDestination_);
		if (after != amountBefore_) {
			history_->record(std::make_unique<RouteEdit>(bridge_, *mod_, mod_->source(), amountDestination_,
			                                             amountBefore_, after));
		}
	}

private:
	[[nodiscard]] size_t slot() const {
		return slotOf_ ? std::min(slotOf_(), kMaxSounds - 1) : 0;
	}

	ParamBridge& bridge_;
	std::function<size_t()> slotOf_;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	float amountBefore_ = 0.0f;
	backplane::Destination amountDestination_ {};
};

} // namespace feathers::ui
