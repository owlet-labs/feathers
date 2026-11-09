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

#include "backplane/device_host.h"
#include "mod/arp/arp_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"

#include <visage/ui.h>

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

// THE ARP CARD — composed around the steps it plays, not a list of knobs.
//
//     [ Order ▾ ] [ Time Base ▾ ] [ Feel ▾ ] [ Latch ]   — the two lists open as menus; Latch is a two-state chip
//     ┌──────────────────────────────────────────┐
//     │ ▮  ▮▮ ▮   ▮  ▮▮ ▮   ▮  ▮▮ ▮   ▮  ▮▮ ▮▌  │   — the last sixteen steps it played: a bar at each one's
//     └──────────────────────────────────────────┘     pitch, as wide as the Gate, placed as the Feel moves it; the
//       Rate     Gate     Amount   Octaves              newest is the playhead
//
// THE LANE IS WHAT PLAYED, read from the device's newest events (`ModOps::recentEvents`), so it shows the chord
// you are holding in the order you chose. With nothing played yet it says what it waits for. Everything else —
// Velocity, Offset, Nudge, Octave Mode, Run, Seed — is in the window, which holds every control.

namespace feathers::ui {

class Notifier;
class WidgetRegistry;

/// **THE STEP LANE — one frame, on the card and, larger, in the window** (window ⊇ card: the window keeps what the card
/// shows, not only its controls). The newest steps the device played (`ModOps::recentEvents`, through the host), each a
/// bar at its pitch as wide as the Gate, the swung slots late, the newest in the accent: the playhead. Asked again by
/// its owner every frame (`poll`), which repaints it only when the steps, the Gate or the Swing moved.
class ArpLane final : public visage::Frame {
public:
	static constexpr size_t kSteps = device::kRecentEvents;

	ArpLane(backplane::DeviceHost& host, DeviceInstanceId id);
	/// Reads the newest steps and the Gate and Swing; true (and a repaint) when any moved.
	bool poll();
	void draw(visage::Canvas& canvas) override;
	/// THE KEYS IT DRAWS, oldest first — what the device said it played.
	[[nodiscard]] std::vector<int> keysForTest() const;

private:
	[[nodiscard]] float fieldOf(mod::arp::Param p) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	std::array<device::RecentEvent, kSteps> steps_ {};
	size_t count_ = 0;
	uint32_t newest_ = 0;
	float gate_ = -1.0f;
	float feel_ = -1.0f;
	float amount_ = -1.0f;
	float phase_ = 0.0f;
	float wobble_ = 0.0f;
	float timeBase_ = -1.0f;
	float rate_ = -1.0f;
};

class ArpCard final : public FxCard {
public:
	ArpCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr,
	        ModState* mod = nullptr, EditHistory* history = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceControl`'s guard, here too.
	ArpCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr, ModState* = nullptr,
	        EditHistory* = nullptr) = delete;
	~ArpCard() override;

	void resized() override;
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::arp::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kRowGap = 2.0f;
	static constexpr float kLaneHeight = 64.0f;
	static constexpr float kFaceDiameter = Knob::kEngineRailDiameter;
	/// HOW MANY STEPS THE LANE SHOWS — every newest event the device offers.
	static constexpr size_t kLaneSteps = ArpLane::kSteps;

	static constexpr float controlsHeight() { return Knob::columnHeight(kFaceDiameter); }
	static constexpr float bodyHeight() {
		return kGap + ValueBox::kHeight + kGap + kLaneHeight + kGap + controlsHeight() + kGap;
	}
	/// OPEN, IN A VERTICAL LANE — for `ModPanel::bodyHeightOf`.
	static float openHeight(size_t outputRows);

	[[nodiscard]] ValueBox& order() const { return *order_; }
	[[nodiscard]] ValueBox& timeBase() const { return *timeBase_; }
	[[nodiscard]] SwitchChip& latch() const { return *latch_; }
	[[nodiscard]] Knob& rate() const { return *rate_; }
	[[nodiscard]] Knob& gate() const { return *gate_; }
	[[nodiscard]] ModeValueCell& feel() const { return *feel_; }
	[[nodiscard]] ValueBox& wobbleForTest() const { return *wobble_; }
	[[nodiscard]] Knob& octaves() const { return *octaves_; }

	[[nodiscard]] DeviceControl& control(size_t index) const;
	/// Every device control this card puts a widget on (row indices), asked of the widgets' own controls.
	[[nodiscard]] std::vector<size_t> addressedParamsForTest() const;
	/// THE KEYS THE LANE DRAWS, oldest first — what the device said it played.
	[[nodiscard]] std::vector<int> laneKeysForTest() const;

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] visage::Bounds laneBounds() const;
	void layoutControls();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	std::unique_ptr<ValueBox> order_;
	std::unique_ptr<ValueBox> timeBase_;
	/// THE FEEL AS SKLITTER'S GROOVE IS DRAWN (swing/humanize as a zone knob): its names on a
	/// ZoneSelect, its Amount the box above them — two params, one zone-knob look (`ModeValueCell`).
	std::unique_ptr<ModeValueCell> feel_;
	/// **WOBBLE ON THE CARD** (by design): a small box at the Feel cell's right; the window has it too.
	std::unique_ptr<ValueBox> wobble_;
	static constexpr float kWobbleWidth = 50.0f;
	std::unique_ptr<SwitchChip> latch_;
	std::unique_ptr<Knob> rate_;
	std::unique_ptr<Knob> gate_;
	std::unique_ptr<Knob> octaves_;
	std::unique_ptr<ArpLane> lane_;
};

} // namespace feathers::ui
