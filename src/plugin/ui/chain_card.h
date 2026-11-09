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

#include "backplane/device_host.h"
#include "composite/composite_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/widget_registry.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// THE CHAIN DEVICE'S CARD — D6, DRAWN: the summary as the head, the contents as the body, plus mix and
// gain.
//
// A chain (`composite::kTraits`, "cmps") is a device that HOLDS devices: the rack engine wrapped in the
// ops every device declares, with eleven controls of its own — enable, mix, gain and eight macros. Until
// this card it fell to `GenericCard`, which drew the eleven on a grid and knew nothing about what was
// inside — a shell of a UI.
//
// **THE HEAD IS THE SUMMARY.** Beside the name, the devices the chain holds by name in chain order —
// "Filter › Harm › OWLTT" — read from the BODY the host carries for this device (`DeviceHost::
// containerBodyOf`), which is the inner rack's own file, parsed with `rack::parseRack`. No reach into the
// running instance, which lives on the audio thread; re-parsed only when the host's body generation
// moves. A folded chain is its head, so a folded chain still says what it holds.
//
// **THE BODY IS THE CHAIN'S OWN CONTROLS**, which are the point of a chain device: the eight macros in
// two rows of four, then Mix and Gain — a `DeviceControl` and a `Knob` each, as a filter's macro knobs.
// Enable is the head's square. That is all eleven, so the reachability guard is satisfied outright.
//
// THE EXPAND MARK lights when the host offers an editor for a chain (`hasExpandedViewFor`), which is the
// inner rack's own editor over a bridge inside the device — roadmap §6 Stage B. Until then the card is
// what there is, and it says so by offering nothing.

namespace feathers::ui {

class ChainCard final : public FxCard {
public:
	ChainCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id,
	          Notifier* notifier = nullptr);
	ChainCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	std::string headCaption() const override;
	bool headEnabled() const override;
	void resized() override;
	float laneHeightAt(float) const override;
	float laneExtent(float cross) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	/// Re-reads the body when the host's generation moved — the summary changes, so the card redraws.
	bool updateContent() override;

	const device::Descriptor* descriptor() const override { return &rackDescriptor(); }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPad = 10.0f;
	static constexpr float kRowGap = 8.0f;
	static constexpr float kKnobDiameter = Knob::kEngineRailDiameter;
	static constexpr size_t kKnobsPerRow = 4;
	/// Eight macros on two rows, then Mix and Gain on a third — ten knobs, twelve cells.
	static constexpr size_t kKnobCount = composite::kMacroCount + 2;

	[[nodiscard]] static float openHeight();
	[[nodiscard]] static constexpr float foldedHeight() { return kSlotHeadHeight; }
	[[nodiscard]] visage::Bounds headBounds() const;

	/// THE SUMMARY AS DRAWN — "Filter › Harm › OWLTT", or "empty" — and how many devices the body names.
	[[nodiscard]] const std::string& summaryForTest() const { return summary_; }
	[[nodiscard]] size_t heldCountForTest() const { return heldCount_; }
	/// EVERY CONTROL A KNOB ON THIS CARD ADDRESSES — the reachability guard's question. The head's enable
	/// is the guard's own to add, as for every card whose head draws it.
	[[nodiscard]] std::vector<composite::Param> addressedParamsForTest() const;
	[[nodiscard]] const std::vector<std::unique_ptr<Knob>>& knobsForTest() const { return knobs_; }

private:
	/// The chain device's descriptor, which lives in the rack module; fetched by tag so this header does
	/// not include the rack.
	static const device::Descriptor& rackDescriptor();
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] DeviceControl& control(composite::Param p) const;
	[[nodiscard]] visage::Bounds cellBounds(size_t index) const;
	void layOut();
	/// Parse the host's body for this device into `summary_`/`heldCount_`. Returns whether it changed.
	bool refreshSummary();
	/// **RE-READ THE MACROS' NAMES FROM THE RUNNING CHAIN** (backlog OX) — the names its window renames, through
	/// `ContainerOps::macroLabel`, not from the stored body, which moves only on a host save. A renamed macro's knob
	/// prints the name; the others print "M1".."M8". Returns whether any caption changed (so the card repaints).
	bool refreshMacroNames();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	/// THE CAPTIONS THE MACRO KNOBS POINT INTO — declared before `knobs_`, which dies first.
	std::array<std::string, composite::kMacroCount> macroCaption_;
	std::vector<std::unique_ptr<Knob>> knobs_;
	std::string summary_;
	size_t heldCount_ = 0;
	uint64_t summaryGeneration_ = 0;
	bool summaryRead_ = false;
};

} // namespace feathers::ui
