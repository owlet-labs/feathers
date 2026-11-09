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
#include "mod/mseg/mseg_types.h"
#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_state.h"

// ONE CONTROL OF AN MSEG IN A RACK SLOT, as a device::Modulatable — the MSEG's RackControl.
//
// THIS IS THE SECOND INSTANCE AND IT IS ALMOST A COPY, WHICH IS WORTH WRITING DOWN RATHER THAN
// APOLOGISING FOR. Against `RackControl` it differs in exactly four places:
//
//   * the descriptor table it indexes         (mseg::kParamDescs vs dahdsr::kParamDescs)
//   * how it reads a value                    (modRack().msegAt vs .dahdsrAt)
//   * how it writes one                       (setMsegSettings vs setEnvelopeSettings)
//   * the type of `param_`                    (mseg::Param vs dahdsr::Param)
//
// Everything else — display(), setAmount(), the gesture pair, text(), and destination() — is
// identical, because a destination is `(device id, control index)` and takes a control index rather
// than a device's own enum.
//
// SO WHY NOT A TEMPLATE. Two callers is where the question becomes answerable, not where it must be
// answered — and generalising `RackControl` means rewriting a class every card in this editor
// already depends on, at the same moment as adding a card. Two risky changes at once is how the
// second one gets blamed for the first. The four differences are listed above so a THIRD device can
// make that call on evidence: if it needs the same four and nothing else, the template is proven and
// the copy stops being defensible.

namespace feathers::ui {

class MsegControl : public device::Modulatable, public RecordsEdits {
public:
	/// THE DEVICE BY ITS ID (kill-lanes step 2), as `RackControl` — where it stands is asked at each read.
	MsegControl(ParamBridge& bridge, DeviceInstanceId id, mod::mseg::Param param, ModState* mod = nullptr,
	            EditHistory* history = nullptr)
	    : bridge_(bridge), device_(id), param_(param), mod_(mod), history_(history) {}
	/// **A LANE OR A SLOT IS NOT AN ID** — see `RackControl`'s deleted twin.
	MsegControl(ParamBridge&, size_t, mod::mseg::Param, ModState* = nullptr, EditHistory* = nullptr) = delete;

	const device::ParamDesc& desc() const override {
		return mod::mseg::kParamDescs[static_cast<size_t>(param_)];
	}

	double value() const override {
		return mod::mseg::valueOf(bridge_.modRack().msegAt(bridge_.rackSlotOf(device_)), param_);
	}

	void setValue(double value) override {
		const size_t at = bridge_.rackSlotOf(device_);
		if (at == kNoPosition) {
			return; // the device has left the strip; there is nothing here to write
		}
		mod::mseg::Settings settings = bridge_.modRack().msegAt(at);
		// setValueOf clamps and rounds by the control's own declaration, and sanitises the
		// relationships a per-field clamp cannot state — the loop markers staying inside the live
		// segments, which is a constraint the DAHDSR has no equivalent of.
		mod::mseg::setValueOf(&settings, param_, static_cast<float>(value));
		bridge_.setMsegSettings(at, settings);
	}

	device::ModDisplay display() const override;

	void setAmount(float amount) override;

	void beginGesture() override {}
	void endGesture() override {}

	void beginAmountGesture() override;
	void endAmountGesture() override;

	std::string text() const override;
	/// See `RecordsEdits` — the history this control's gestures record into (backlog LS's type-in folds into it).
	[[nodiscard]] EditHistory* recordsInto() const override { return history_; }

	DeviceInstanceId device() const noexcept { return device_; }
	mod::mseg::Param param() const noexcept { return param_; }

	/// THIS CONTROL AS A ROUTE'S DESTINATION — its device and its OWN control index; whether it may be
	/// one at all is `isDestination()`, asked of the device actually under the id.
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
	mod::mseg::Param param_ = mod::mseg::Param::TimeScale;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	float amountBefore_ = 0.0f;
};

} // namespace feathers::ui
