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

#pragma once

#include "device/modulatable.h"
#include "fx/filter/filter_types.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_state.h"

#include <cstddef>
#include <string>

// ONE CONTROL OF AN FX DEVICE, addressed the way a swallowed device has to be addressed.
//
// RackControl for the fx lane, and the symmetry is the point: an effect and a modulator are the same
// kind of thing to this layer, so their control adapters are the same shape. Both read and write
// their device's settings THROUGH THE BRIDGE, which is what makes their widgets modulation
// destinations rather than knobs that happen to draw.
//
// IT EXISTS BECAUSE THE FILTER'S CONTROLS STOPPED BEING CLAP PARAMS. By design, docked effects' CLAP
// params are not on Feathers' surface; they are swallowed and reached by internal modulation.
// A ParamControl addresses a clap_id; a swallowed device has none, so its
// card cannot be built from ParamControls and its knobs address (device id, control index) instead.
//
// AND A CONTROL IS ADDRESSED BY ITS INDEX IN THE DEVICE'S OWN DECLARATIONS, never by a host
// identifier — device.h's permanent rule, and the one place the filter card used to fall short of it.

namespace feathers::ui {

class FxControl final : public device::Modulatable, public RecordsEdits {
public:
	/// `mod` may be null — a control that has not been handed the panel's state never reports itself
	/// armed. `history` is optional and is used for one thing: the undo step for an amount drag, for
	/// the reason RackControl's constructor states.
	/// THE GENERIC FORM: a device, a DESCRIPTOR, and a control index into that descriptor.
	///
	/// IT WAS FILTER-SHAPED UNTIL HARM NEEDED A CARD, and the name was aspirational for a commit: it
	/// took `fx::filter::Param` and wrote through setFilterSettings, so the lane's second device could
	/// not be edited through it at all. That is the same pin this whole arc keeps pulling — a fact
	/// about one device baked into something that claims to be about devices.
	///
	/// **THE DEVICE BY ITS ID (kill-lanes step 2)** — never a slot or a lane. What `slot_` meant here
	/// changed twice (a position, then a lane) and each change left one line behind still reading the
	/// old meaning; an id is the device wherever it is drawn, and the bridge finds it inside.
	FxControl(ParamBridge& bridge, DeviceInstanceId id, const device::Descriptor& desc, size_t control,
	          ModState* mod = nullptr, EditHistory* history = nullptr)
	    : bridge_(bridge), device_(id), desc_(&desc), control_(control), mod_(mod),
	      history_(history) {}

	/// The filter's spelling, kept so its card reads in its own vocabulary rather than in indices.
	/// Forwards to the generic form; there is one implementation underneath.
	FxControl(ParamBridge& bridge, DeviceInstanceId id, fx::filter::Param param, ModState* mod = nullptr,
	          EditHistory* history = nullptr)
	    : FxControl(bridge, id, fx::filter::kDescriptor, static_cast<size_t>(param), mod,
	                history) {}

	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceInstanceId` is a `uint32_t` and a `size_t` would
	/// narrow into it without a word, so a site still passing its old address fails here instead.
	FxControl(ParamBridge&, size_t, const device::Descriptor&, size_t, ModState* = nullptr,
	          EditHistory* = nullptr) = delete;
	FxControl(ParamBridge&, size_t, fx::filter::Param, ModState* = nullptr, EditHistory* = nullptr) = delete;

	void setModulation(ModState* mod, EditHistory* history) {
		mod_ = mod;
		history_ = history;
	}

	const device::ParamDesc& desc() const override { return desc_->params[control_]; }

	double value() const override {
		// READ OFF THE ROW, which is the device's own indexed view of itself in its own declared
		// units. A field the row does not reach is ABSENT, not zero, and answers with the control's
		// declared default — device_row.h's rule, arriving here because a card may be looking at a
		// patch written by a build that declared fewer controls.
		//
		// **ONE LOAD PER READ, ASKED BY THE DEVICE'S ID** — the bridge finds where the device is drawn
		// (`fxSlotOf`) and answers the field from its mirror, rather than this copying the whole chain
		// per paint to look the row up itself.
		const size_t at = bridge_.fxSlotOf(device_);
		if (at == kNoPosition || control_ >= desc_->params.size()) {
			return 0.0;
		}
		return control_ < bridge_.fxFieldsPresentAt(at) ? static_cast<double>(bridge_.fxFieldAt(at, control_))
		                                                : desc_->params[control_].def;
	}

	void setValue(double value) override {
		// ONE FIELD OF THE ROW, coerced by the bridge against the control's own declaration — so a
		// widget may hand over whatever its drag produced without knowing the range, and no device
		// type is named on the way.
		bridge_.setDeviceControl(device_, control_, static_cast<float>(value));
	}

	device::ModDisplay display() const override;
	void setAmount(float amount) override;
	std::string text() const override;
	/// See `RecordsEdits` — the history this control's gestures record into (backlog LS's type-in folds into it).
	[[nodiscard]] EditHistory* recordsInto() const override { return history_; }

	/// A GESTURE ON THIS SIDE IS NOT A HOST GESTURE, and that is not an omission: a swallowed
	/// device's control is not a parameter and no host knows it exists, so there is nothing to latch
	/// automation onto.
	///
	/// WHICH IS EXACTLY WHY THIS ROAD HAS TO RECORD THE UNDO STEP ITSELF, and the reason these two
	/// were empty for as long as they were. `ParamControl`'s pair (`param_control.cpp:155`) records
	/// no history either and is right not to: a host parameter has a DAW behind it that owns undo.
	/// A swallowed control has NOBODY. So this is not the parameter road's shape — it is the AMOUNT
	/// road's, four lines further down this file: capture before, record one entry iff it moved.
	///
	/// AN EARLIER COMMENT HERE SAID "the value road's owner does for itself". It does not, on this
	/// road. `env_card` and `mseg_card` do record their own value edits, which is what that sentence
	/// was describing; `filter_card`, `harm_card`, `owltt_card`, `owltt_expanded` and `harm_expanded`
	/// record NONE, and `Knob::history_` is assigned and never read. The sentence was true of a
	/// different arrangement and generalised to this one wrongly, which is what hid the gap.
	/// A CONTROL DESTROYED MID-DRAG STILL CLOSES ITS HOST GESTURE — `DeviceControl`'s rule (host slots, T6b).
	~FxControl() override { endHostGesture(); }
	FxControl(const FxControl&) = delete;
	FxControl& operator=(const FxControl&) = delete;

	void beginGesture() override;
	void endGesture() override;
	/// THE HOST'S BEGIN/END ON THE CONTROL'S SLOT, IF IT HAS ONE (host slots, T6b review) — the filter card's
	/// knobs are these, not `DeviceControl`s, so without this a drag of a slotted filter control reached the
	/// host as bare values that record nothing. At most once each; `beginGesture`/`endGesture` call these.
	void beginHostGesture() override {
		if (!hostGesture_) {
			hostGesture_ = true;
			bridge_.touchControl(device_, control_); // a hand: the recent ring first (host slots, T9)
			bridge_.beginControlGesture(device_, control_);
		}
	}
	void endHostGesture() override {
		if (hostGesture_) {
			hostGesture_ = false;
			bridge_.endControlGesture(device_, control_);
		}
	}

	void beginAmountGesture() override;
	void endAmountGesture() override;

	DeviceInstanceId device() const noexcept { return device_; }
	size_t control() const noexcept { return control_; }

	/// **THIS CONTROL AS A ROUTE'S DESTINATION** — its device and its OWN control index, which is all a
	/// destination is (kill-lanes step 2). Whether it may be one at all is `isDestination()`.
	backplane::Destination destination() const noexcept {
		return backplane::Destination::control(device_, control_);
	}

	/// WHETHER THIS CONTROL IS A MODULATION DESTINATION — the declaration of the device actually under
	/// this id says, asked of the bridge rather than of the descriptor this adapter was built with.
	bool isDestination() const noexcept {
		const device::Descriptor* d = bridge_.descriptorOf(device_);
		return d != nullptr && control_ < d->params.size() && d->params[control_].modulatable;
	}

private:
	ParamBridge& bridge_;
	DeviceInstanceId device_ = kNoInstance;
	const device::Descriptor* desc_ = nullptr;
	size_t control_ = 0;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	float amountBefore_ = 0.0f;
	double valueBefore_ = 0.0;
	bool hostGesture_ = false;
};

/// ONE FX DEVICE CONTROL'S VALUE, BEFORE AND AFTER — the undo step for a drag on a swallowed
/// device's knob. `RackValueEdit` (`rack_control.h:131`) is the same record on the modulator chain
/// and this is deliberately its twin: an effect and a modulator are the same kind of thing here.
///
/// IT WRITES THROUGH THE BRIDGE AND HOLDS NOTHING OF THE EDITOR. A record outlives the widget that
/// made it — a card can be retired while its step is still in the history — so it addresses the
/// control the way every other road does, by (device id, control index), and never by a pointer to
/// the adapter that recorded it. BY ID, so an undo after the device was dragged elsewhere still lands.
class FxValueEdit : public EditRecord {
public:
	FxValueEdit(ParamBridge& bridge, DeviceInstanceId device, size_t control, double before, double after)
	    : bridge_(bridge), device_(device), control_(control), before_(before), after_(after) {}

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Set"; }

private:
	void write(double value) const {
		// THE SAME ROAD `FxControl::setValue` TAKES, so the bridge coerces an undone value against the
		// control's own declaration exactly as it coerced the value that got recorded.
		bridge_.setDeviceControl(device_, control_, static_cast<float>(value));
	}

	ParamBridge& bridge_;
	DeviceInstanceId device_ = kNoInstance;
	size_t control_ = 0;
	double before_ = 0.0;
	double after_ = 0.0;
};

} // namespace feathers::ui
