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

#include "backplane/device_host.h"
#include "device/subscribers.h"
#include "mod/macros/macros_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/macro_polarity.h"
#include "plugin/ui/macro_slot.h"

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

// THE MACROS CARD (macro-modulation B3) — FOUR MACRO SLOTS IN A ROW, ONE PER OUTPUT.
//
// **THE SAME `MacroSlot` THE HEADER ROWS SEAT**, over this device's controls instead of a host's macro
// parameters: each slot's control is `Macro n` seen through its `Bipolar n` (`BipolarMacro`), its menu's
// type and polarity are `Type n` and `Bipolar n`, and its name is output n's label on this instance
// (`DeviceHost::outputLabel`). So a knob turned here, a latch pressed here and a name typed here are the
// device's patch state, saved with the device, and the route lists read the name.
//
// **THIS CARD IS THE ONLY READER OF `Type n`** — the device never reads it (`macros_types.h`), so the
// honesty rule for those four params rests on this card existing wherever the device does. It is the
// factory's answer for the tag (`card_factory.cpp`) and `ModPanel`'s, so every host that draws the
// device draws this.
//
// **UNLIKE A HEADER MACRO, EACH KNOB IS A DESTINATION**: `Macro n` is modulatable, so the knob takes a
// route when a source is armed and draws the ring of whatever rides it.
//
// **IT CAN DIE UNDER ITS OWN POPUP.** A slot's menu is parented to the root frame and keeps a copy of
// the hooks until a pick, and the rename field's commit reaches the naming functions; a nested chain's
// editor, and with it this card, can be rebuilt on a tick while either is open. Every hook and naming
// function asks `alive_` first, so a late pick writes nothing and a late read answers the default.

namespace feathers::ui {

class Notifier;

class MacrosCard final : public FxCard {
public:
	/// See `ControlFace` — what this surface offers, so the window-superset guard can see it.
	void offeredControls(std::vector<Offered>& out) const override;
	/// `mod` and `history` may be null — a host with no modulation state gets knobs that take no route,
	/// and writes that record no undo step; `setModulation` hands them over later either way.
	MacrosCard(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr,
	           ModState* mod = nullptr, EditHistory* history = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceControl`'s guard, here too.
	MacrosCard(backplane::DeviceHost&, size_t, Notifier* = nullptr, ModState* = nullptr,
	           EditHistory* = nullptr) = delete;
	/// **SILENCES EVERYTHING THAT MAY OUTLIVE IT FIRST** — the token, then the label subscription.
	~MacrosCard() override;

	void resized() override;
	/// THE SLOT MENU'S DOTS, WHEN A HOST BINDS ONE (a lane: Feathers' fx lane holds modulators since 2026-10-04) —
	/// `drawSlotMenu` draws nothing where none is bound, so the strip, which draws its own head mark, is unchanged.
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	/// **RETIRED, IT STOPS HEARING RELABELS** — the label subscription ends here rather than at the free.
	/// The token `alive_` is NOT ended here: it guards the FREE (a menu still open on the root frame
	/// picking into a card that is gone), and what a pick into a retired-but-whole card should do is a
	/// separate question this does not answer.
	void retired() override { labelSubscription_.reset(); }
	bool updateContent() override;
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::macros::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr size_t kSlotCount = mod::macros::kMacroCount;
	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	/// THE HEADER ROW'S FACE (`ModPanel::kMacroDiameter`), so a macro reads as one size wherever it is.
	static constexpr float kFaceDiameter = 36.0f;

	/// THE BODY: a gap, one row of slots, a gap.
	static constexpr float bodyHeight() { return kGap + MacroSlot::columnHeight(kFaceDiameter) + kGap; }
	/// OPEN, IN A VERTICAL LANE — head, body and `outputRows` rows of output chips. Static so a host can
	/// reserve the card before it has built one (`ModPanel::bodyHeightOf`).
	static float openHeight(size_t outputRows);

	/// **THE SHORT CAPTION OF SLOT n** — "M1".."M4", printed while output n carries no label of its own.
	[[nodiscard]] static std::string shortCaption(size_t n);

	[[nodiscard]] MacroSlot& slot(size_t n) const { return *slots_[n]; }
	/// THE VIEW SLOT n's CONTROLS ARE BUILT ON — `Macro n` through `Bipolar n`.
	[[nodiscard]] BipolarMacro& view(size_t n) const { return *views_[n]; }
	/// The parameter road under a control of this device, by its index in the descriptor.
	[[nodiscard]] DeviceControl& control(size_t index) const;
	/// Every control this card puts a hand on — the twelve per-macro ones; the enable is the head's.
	[[nodiscard]] std::vector<mod::macros::Param> addressedParamsForTest() const;

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] MacroMenuHooks hooksFor(size_t n);
	[[nodiscard]] MacroSlot::Naming namingFor(size_t n);
	/// ONE CHOICE WRITTEN AS A HAND WRITES IT — begin, set, end — so the edit is one undo step.
	void writeAsGesture(size_t control, double value);
	[[nodiscard]] visage::Bounds slotBounds(size_t n) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	/// **THE CARD, FOR ANYTHING THAT MAY OUTLIVE IT** — the menu's copy of the hooks and the naming
	/// functions capture it weakly and do nothing once it is gone (`RackEditor::alive_`'s shape).
	std::shared_ptr<void> alive_ = std::make_shared<char>(0);
	/// THE PARAMETER ROAD, ONE PER DECLARED CONTROL, built on first ask. Declared before the views and
	/// the slots so it outlives both: a held momentary lets go through them as its chip dies.
	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(mod::macros::Param::kCount)> controls_;
	std::array<std::unique_ptr<BipolarMacro>, kSlotCount> views_;
	std::array<std::unique_ptr<MacroSlot>, kSlotCount> slots_;
	/// WHAT EACH SLOT LAST DREW — the base value and the polarity — so `updateContent` repaints a slot
	/// only when an undo, a load or the host moved one under it.
	std::array<float, kSlotCount> drawnValue_ {};
	std::array<float, kSlotCount> drawnPolarity_ {};
	/// THE EAR ON OUTPUT RELABELS — it refreshes `slots_`, so it is declared after them and dies first.
	device::Subscription labelSubscription_;
};

} // namespace feathers::ui
