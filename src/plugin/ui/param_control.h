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
#include "plugin/ui/knob.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_readout.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/mod_state.h"

#include <functional>
#include <optional>

// A HOST PARAMETER, AS A device::Modulatable — the other implementation of the interface RackControl
// already implements, and the one that lets a widget stop caring which kind of control it holds.
//
// WHY THIS EXISTS. Knob addressed its value through a clap_id on the ParamBridge in twenty places:
// value(id), setValue(id), modulated(id), beginGesture(id), and the route table by the index
// modDestinationIndexOf(id) returns. That is exactly right for the ten knobs in the footer and the
// filter card's six — they ARE host parameters — and it is exactly wrong for a control inside a
// dockable device, which has no clap_id at all and never will. A swallowed filter's cutoff is
// addressed by (slot, control), and it cannot be a Knob as Knob was written.
//
// device.h says a device must be able to define its own interface and still join the shared
// modulation surface; that promise is empty while the editor's most-used control can only be built
// over a host parameter. So the road became one road, and this class is the old one behind it.
//
// THE POINT OF THE SPLIT IS THAT NOTHING ELSE MOVED. Knob keeps the constructor its seventeen call
// sites and fourteen fixtures use, and builds one of these internally. A refactor that rewrote those
// seventeen sites to gain one is the version of this that goes wrong, and the rack plan's own warning
// about this file — "it is a shared file — coordinate through main rather than carrying it
// half-finished in this worktree" — was written about precisely that risk.
//
// A NULL DESCRIPTOR IS A REAL STATE, kept from Knob: the footer's Weather knob is bound to nothing
// and draws a bare track. It reads as "no value" rather than as zero, which is a difference the face
// draws — see drawKnobFace's `hasValue`.

namespace feathers::ui {

class ParamControl final : public OwnedControl, public RoutedControl, public RecordsEdits, public RouteReadoutSubject {
public:
	/// `desc` may be null — an unbound control, which answers "not a destination", prints nothing and
	/// refuses every write.
	ParamControl(ParamBridge& bridge, const ParamDesc* desc, ModState* mod = nullptr,
	             EditHistory* history = nullptr);

	/// The panel's state, handed over after construction for the reason Knob::setModulation states:
	/// most knobs are built by a tray that has no business knowing about modulation.
	void setModulation(ModState* mod, EditHistory* history) override {
		mod_ = mod;
		history_ = history;
	}

	/// Whether there is a parameter behind this at all.
	/// AND IT IS THE CONTRACT'S, not just this class's — see OwnedControl::bound. The concept and
	/// the spelling already existed here; all that was missing was the virtual above it.
	bool bound() const noexcept override { return desc_ != nullptr; }

	/// The host identifier, for the two places that legitimately still need one — the announce line's
	/// formatter and the destination lookup. Not part of the interface, and deliberately so: a widget
	/// that reached for this would be back where this class started.
	clap_id id() const noexcept { return static_cast<clap_id>(desc_->id); }

	const device::ParamDesc& desc() const override { return converted_; }
	double value() const override;
	void setValue(double value) override;
	device::ModDisplay display() const override;
	void setAmount(float amount) override;

	void beginGesture() override;
	void endGesture() override;
	/// THE UNDO RECORD FOR A ROUTE LIVES HERE, not in the widget.
	///
	/// Knob used to open the bridge's route gesture, drag, close it, and write the RouteEdit itself —
	/// which meant reaching past its control to the routing table for a source and a destination
	/// index. A device control has neither of those, so a widget that did this could never be given
	/// one. The gesture pair is the seam: begin remembers where the amount started, end writes the
	/// step if it moved.
	void beginAmountGesture() override;
	void endAmountGesture() override;

	/// THROUGH THE PLUGIN'S OWN FORMATTER, not through the declared unit — see Modulatable::text on
	/// why the two roads print differently on purpose. This is the string the host shows in its
	/// automation lane, and a knob that printed something else would be disagreeing with the DAW.
	std::string text() const override;
	/// **TYPE-IN THROUGH THE HOST'S OWN TABLE** (backlog LS): this knob prints what the DAW prints
	/// (`valueToText`), so it opens on that and reads back through `textToValue` — the same pair a host's own
	/// type-in uses — rather than through a device unit it does not have.
	[[nodiscard]] std::optional<std::string> editText() const override;
	[[nodiscard]] std::optional<double> parseText(std::string_view text, double shown) const override;
	/// See `RecordsEdits`. A value gesture, an unroute and a typed amount each record a step here (backlog JB:
	/// value gestures used to be left to the host's undo). The history this control was handed, else the
	/// editor's own through the bridge — so a control built with none still records its values.
	[[nodiscard]] EditHistory* recordsInto() const override;

	/// Which of the routing table's destinations this is, or -1. Kept public because the route list
	/// and the undo record are addressed by index and neither is a widget.
	int destinationIndex() const;

	/// **THIS CONTROL AS A ROUTE'S DESTINATION** — a host end, `Destination::core` at `destinationIndex()`
	/// (kill-lanes step 2) — or nothing for a parameter no source may reach.
	std::optional<backplane::Destination> destination() const;

	/// See `device::Modulatable::amountFor`. The plugin's own road, by endpoint.
	float amountFor(device::Source source) const override;

	/// See `ui::RoutedControl`.
	ModState* routingState() const override { return mod_; }

	/// Drop the route from the focused source entirely, recording it. The mock's double-click in
	/// mapping mode: the route is GONE, not zeroed.
	void unroute() override;

	/// **THE AS-MODULATED READOUT** (backlog PU; `mod_readout.h`) — through `ParamSet`'s own resolve
	/// (`coreRouteTerm`, `landCore`), answered by `ParamSet::probeRoute` on the plugin's block road.
	[[nodiscard]] const void* readoutIdentity() const noexcept override { return &bridge_; }
	[[nodiscard]] device::Destination readoutDestination() const noexcept override {
		return destination().value_or(device::Destination::core(~uint32_t { 0 }));
	}
	[[nodiscard]] std::optional<ModReadout> routeReadout(device::Source source) override;
	void readoutEnded() override { bridge_.routeProbe().unwatch(); }

	/// **A STEREO DESTINATION ONLY WHILE `when` SAYS SO** (2026-10-10). The row's `stereo` is the parameter's capability
	/// in general; an engine may be unable to honour it — Phase on Additive, whose two ears share one rotator per
	/// partial (design §1.4, backlog LY) — and then the pink glow and the L/R marks would advertise a split nobody
	/// hears. Unset (the default) is the row's flag alone.
	void setStereoWhen(std::function<bool()> when) { stereoWhen_ = std::move(when); }

private:
	std::function<bool()> stereoWhen_;
	ParamBridge& bridge_;
	const ParamDesc* desc_;
	/// THE DECLARATION IN THE DEVICE VOCABULARY, built once at construction rather than on every call.
	/// desc() returns a reference, so it has to outlive the call; and the two ParamDesc types are
	/// genuinely different things — the plugin's carries a host id and the device's carries a unit —
	/// which is why this is a conversion and not a cast.
	device::ParamDesc converted_ {};
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	/// What the amount was when the amount gesture opened, so the undo record carries where the drag
	/// STARTED rather than where its last motion event happened to be.
	float amountBefore_ = 0.0f;
	/// What the value was when the value gesture opened, for the same reason.
	double valueBefore_ = 0.0;
};

} // namespace feathers::ui
