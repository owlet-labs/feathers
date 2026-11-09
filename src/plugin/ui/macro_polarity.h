/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not,
 * see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/macro_type.h"
#include "device/modulatable.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/mod_route_menu.h"
#include "plugin/ui/type_in.h"

#include <functional>
#include <string>

namespace feathers::ui {

/// **A STRUCTURAL MACRO, SEEN THROUGH ITS POLARITY (backlog EG).** The main structural macros (4 on
/// Feathers, 8 on the rack) have a context menu that turns them into bipolar knobs defaulting to 0.
///
/// THE MACRO'S STORED VALUE NEVER CHANGES SHAPE. Both hosts hold a macro as a 0..1 parameter the DAW
/// automates by that name, and a mode that re-ranged the parameter would re-point every automation
/// lane under a hand's switch. So bipolar is a VIEW: the knob, the readout and the published source
/// read `2v - 1`, the rest — the default a double-click returns to, the notch the arc grows from — is
/// the middle, and a hand turning the knob writes `(v + 1) / 2` back. Unipolar is the identity, and a
/// macro in that mode is indistinguishable from one built without this class.
///
/// ONE ADAPTER FOR BOTH HOSTS, over whatever control a host builds its macro knob on — Feathers'
/// `ParamControl`, the rack's `MacroControl` — with the mode read from the host each call rather than
/// copied: the mode lives with the patch (`ParamBridge::macroBipolar`, `RackBridge::macroBipolar`)
/// and a copy here would be one more place for it to be stale. Every road that is not the value
/// forwards untouched, including `RoutedControl` for the knob's route menu, which a macro never has.
class BipolarMacro final : public device::Modulatable, public RoutedControl, public RecordsEdits {
public:
	BipolarMacro(device::Modulatable& inner, std::function<bool()> bipolar);

	const device::ParamDesc& desc() const override;
	double value() const override;
	void setValue(double value) override;
	device::ModDisplay display() const override;
	float amountFor(device::Source source) const override { return inner_.amountFor(source); }
	void setAmount(float amount) override { inner_.setAmount(amount); }
	void beginGesture() override { inner_.beginGesture(); }
	void endGesture() override { inner_.endGesture(); }
	void beginAmountGesture() override { inner_.beginAmountGesture(); }
	void endAmountGesture() override { inner_.endAmountGesture(); }
	std::string text() const override;
	// **THE HOST SLOT IS THE INNER CONTROL'S** (host slots T11: a nested rack's macro is its device's control,
	// exposable like any other). Without these the view answered `Unavailable` for every macro it stood in
	// front of, and the knob's menu had no host rows. A header macro's inner is already a host parameter and
	// answers `Unavailable` itself.
	device::HostSlotStatus hostSlot() const override { return inner_.hostSlot(); }
	bool exposeToHost(std::string* displaced) override { return inner_.exposeToHost(displaced); }
	bool unpinFromHost() override { return inner_.unpinFromHost(); }
	uint64_t hostSlotGeneration() const noexcept override { return inner_.hostSlotGeneration(); }

	ModState* routingState() const override;

	/// **TYPE-IN (backlog LS) IS THE VIEW'S WHILE IT IS BIPOLAR** — it prints a signed percent from its own
	/// declaration then — and the inner's otherwise, whose text the knob shows unchanged. The history is
	/// always the inner's: the view records nothing of its own.
	[[nodiscard]] std::optional<std::string> editText() const override {
		return bipolar() ? Modulatable::editText() : inner_.editText();
	}
	[[nodiscard]] std::optional<double> parseText(std::string_view text, double shown) const override {
		return bipolar() ? Modulatable::parseText(text, shown) : inner_.parseText(text, shown);
	}
	[[nodiscard]] EditHistory* recordsInto() const override { return historyOf(inner_); }

	/// Whether the view is bipolar RIGHT NOW — what the host says, asked afresh.
	[[nodiscard]] bool bipolar() const { return bipolar_ && bipolar_(); }

	/// THE CONTROL UNDERNEATH — for a case that wants to ask the parameter road (a `DeviceControl`, a
	/// `ParamControl`) what it would have asked before the view stood in front of it.
	[[nodiscard]] device::Modulatable& inner() const noexcept { return inner_; }

	/// THE ARITHMETIC, on its own so a case can restate it: a stored 0..1 seen bipolar, and back. It is
	/// `device::macroToBipolar`/`macroToStored` (macro_type.h), which the MACROS device's outputs use too.
	[[nodiscard]] static constexpr double toBipolar(double stored) noexcept { return device::macroToBipolar(stored); }
	[[nodiscard]] static constexpr double toStored(double bipolar) noexcept { return device::macroToStored(bipolar); }

private:
	device::Modulatable& inner_;
	std::function<bool()> bipolar_;
	/// THE BIPOLAR DECLARATION, rebuilt from the inner's on each `desc()` so a renamed or re-ranged inner
	/// cannot leave a stale copy here; a reference has to be returned, which is why it is a member.
	mutable device::ParamDesc bipolarDesc_ {};
};

} // namespace feathers::ui
