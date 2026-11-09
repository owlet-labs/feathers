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
#include "mod/dahdsr/dahdsr_types.h"
#include "plugin/gui_bridge.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_state.h"

// ONE CONTROL OF ONE RACK DEVICE, AS SOMETHING A WIDGET CAN EDIT.
//
// This is the plugin's side of device::Modulatable — the adapter that says what "value", "range" and
// "how far is this modulated" MEAN for a control that lives in the modulator rack. A widget holds one
// of these and never learns what a DAHDSR is, which is the whole arrangement: the device declares,
// the host adapts, the widget draws.
//
// THE OTHER IMPLEMENTATION IS A PARAMETER, and it does not exist yet. ui::Knob still addresses its
// value by clap_id through the bridge directly; folding it onto this interface is the generalisation
// this class was designed alongside rather than after, so that the abstraction would have two real
// users before it was fixed. What is here is deliberately the harder of the two — a control with no
// clap_id, no host automation and no ParamDesc of the plugin's own — because an interface that fits
// the awkward case fits the easy one.
//
// IT OWNS NOTHING AND CACHES NOTHING. Every read goes to the bridge, which is the editor's single
// source for what the engine holds; a copy kept here would be one more thing to invalidate when a
// patch loads. The cost is a small struct copy per read, on the UI thread, once per paint.

namespace feathers::ui {

class RackControl : public device::Modulatable, public RecordsEdits {
public:
	/// `mod` may be null — a control that has not been handed the panel's state simply never reports
	/// itself armed, which is the same "absent rather than inactive" shape Knob::setModulation uses.
	/// `history` is optional and is used for ONE thing: the undo step for an amount drag. A rack
	/// control's VALUE edits are recorded by whoever owns the widget — the card, through
	/// ValueBox::onEdited, because an EditRecord has to outlive the card that made it — but an AMOUNT
	/// has no such owner, and until this existed a route dragged onto one of these was not undoable at
	/// all. See endAmountGesture.
	///
	/// **THE DEVICE BY ITS ID (kill-lanes step 2)** — never a rack slot. The strip's mirrors are read
	/// by position, and where the device stands is asked of the bridge (`rackSlotOf`) at each read, so
	/// a card dragged elsewhere in the strip keeps editing the device it was built for.
	RackControl(ParamBridge& bridge, DeviceInstanceId id, mod::dahdsr::Param param, ModState* mod = nullptr,
	            EditHistory* history = nullptr)
	    : bridge_(bridge), device_(id), param_(param), mod_(mod), history_(history) {}
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceInstanceId` is a `uint32_t` and a `size_t` would
	/// narrow into it without a word, so a site still passing its old address fails here instead.
	RackControl(ParamBridge&, size_t, mod::dahdsr::Param, ModState* = nullptr,
	            EditHistory* = nullptr) = delete;

	const device::ParamDesc& desc() const override {
		return mod::dahdsr::kParamDescs[static_cast<size_t>(param_)];
	}

	double value() const override {
		return mod::dahdsr::valueOf(bridge_.modRack().dahdsrAt(bridge_.rackSlotOf(device_)), param_);
	}

	void setValue(double value) override {
		const size_t at = bridge_.rackSlotOf(device_);
		if (at == kNoPosition) {
			return; // the device has left the strip; there is nothing here to write
		}
		mod::dahdsr::Settings settings = bridge_.modRack().dahdsrAt(at);
		// setValueOf clamps and rounds by the control's own declaration, so a widget may hand over
		// whatever its drag produced without knowing the range.
		mod::dahdsr::setValueOf(&settings, param_, static_cast<float>(value));
		bridge_.setEnvelopeSettings(at, settings);
	}

	device::ModDisplay display() const override;

	void setAmount(float amount) override;

	/// A GESTURE ON THIS SIDE IS NOT A HOST GESTURE, and that is not an omission.
	///
	/// ParamBridge::beginGesture posts a CLAP gesture-begin so a host can latch automation onto a
	/// drag. A rack control is not a parameter and no host knows it exists, so there is nothing to
	/// latch and nothing to post. What a gesture still has to do is bracket the EDIT for undo, which
	/// the route road already does through beginRouteGesture; the value road's undo record is the
	/// card's business rather than this adapter's, for the same reason Knob records its own.
	void beginGesture() override {}
	void endGesture() override {}

	/// AN AMOUNT IS BRACKETED EVEN THOUGH A VALUE IS NOT, which looks inconsistent and is not. A rack
	/// control's VALUE is not a host parameter, so there is no host gesture to open around it — hence
	/// the two empty functions above. An AMOUNT is a routing, and routings have their own gesture on
	/// the bridge, which the whole editor shares. This class simply was not calling it: the knob
	/// bracketed its amount drags and the value box did not, for no reason anyone chose.
	void beginAmountGesture() override;
	void endAmountGesture() override;

	std::string text() const override;
	/// See `RecordsEdits` — the history this control's gestures record into (backlog LS's type-in folds into it).
	[[nodiscard]] EditHistory* recordsInto() const override { return history_; }

	DeviceInstanceId device() const noexcept { return device_; }
	mod::dahdsr::Param param() const noexcept { return param_; }

	/// **THIS CONTROL AS A ROUTE'S DESTINATION** — its device and its OWN control index (kill-lanes
	/// step 2). Whether it may be one at all — the switches and the trigger mode may not — is
	/// `isDestination()`.
	backplane::Destination destination() const noexcept {
		return backplane::Destination::control(device_, static_cast<size_t>(param_));
	}
	bool isDestination() const noexcept {
		const device::Descriptor* d = bridge_.descriptorOf(device_);
		const auto control = static_cast<size_t>(param_);
		return d != nullptr && control < d->params.size() && d->params[control].modulatable;
	}

private:
	ParamBridge& bridge_;
	DeviceInstanceId device_ = kNoInstance;
	mod::dahdsr::Param param_ = mod::dahdsr::Param::Attack;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	/// Where the amount was when its gesture opened, so the undo step carries where the drag STARTED
	/// rather than where its last motion event happened to be.
	float amountBefore_ = 0.0f;
};

/// ONE RACK CONTROL'S VALUE CHANGED, for the editor's one history.
///
/// A RECORD OVER COORDINATES, NOT OVER A WIDGET — edit_history.h asks this of every new author, and
/// a device card makes the reason concrete: the rail collapses, the card is torn down, and an undo
/// afterwards has to put the value back rather than reach into a dead frame. So this holds the
/// bridge, the device's id and the control index, and resolves where the device stands afresh each
/// time — an undo after a reorder lands on the device, not on whatever now stands where it stood.
///
/// It lives here rather than in value_box.cpp because it is about a RACK CONTROL. The widget that
/// produced the edit deliberately does not know what it edited — see ValueBox::onEdited — so it
/// could not have built this, and a version of it that could would have been the widget knowing.
class RackValueEdit : public EditRecord {
public:
	RackValueEdit(ParamBridge& bridge, DeviceInstanceId device, mod::dahdsr::Param param, double before,
	              double after)
	    : bridge_(bridge), device_(device), param_(param), before_(before), after_(after) {}
	RackValueEdit(ParamBridge&, size_t, mod::dahdsr::Param, double, double) = delete;

	void undo() const override { write(before_); }
	void redo() const override { write(after_); }
	const char* verb() const override { return "Set"; }

private:
	void write(double value) const {
		const size_t at = bridge_.rackSlotOf(device_);
		if (at == kNoPosition) {
			return;
		}
		mod::dahdsr::Settings settings = bridge_.modRack().dahdsrAt(at);
		mod::dahdsr::setValueOf(&settings, param_, static_cast<float>(value));
		bridge_.setEnvelopeSettings(at, settings);
	}

	ParamBridge& bridge_;
	DeviceInstanceId device_ = kNoInstance;
	mod::dahdsr::Param param_ = mod::dahdsr::Param::Attack;
	double before_ = 0.0;
	double after_ = 0.0;
};

} // namespace feathers::ui
