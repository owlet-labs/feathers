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
#include "mod/follower/follower_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/follower_plot.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/switch_chip.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

// THE FOLLOWER CARD (backlog MY, OP) — composed around the envelope it publishes.
//
//     [ Control | Audio ]              [ Poly ]   — what it follows; Poly greyed in Control (it follows voices' audio)
//     [ Peak | RMS | Signed ]                       — the detector (Audio: [ Peak | RMS ])
//     follows its Input: route any source into it   — ONE LINE saying what it hears
//     ┌─(In)─┬──────────────────────────────┐
//     │ knob │  the envelope, the threshold  │      — Input's knob at the plot's left, greyed in Audio
//     └──────┴──────────────────────────────┘
//       Attack   Release   Gain   Floor             — the detector's time, then its level (Floor greyed under Signed)
//
// **THE THRESHOLD HAS NO KNOB: IT IS THE LINE ON THE PLOT** (`FollowerPlot`), dragged where it acts. The choices are
// `SegmentedControl`s (every entry on the face) and Poly a two-state `SwitchChip` (CLAUDE.md: no multivalue toggle).
// Under Signed the plot is bipolar: zero is its middle.
//
// **THE MODE IS ONE EDIT WITH WHAT IT COSTS** (`FollowerModeControl`): a flip deletes the routes the new mode forbids
// (`DeviceHost::takePrunedRoutes`), the tray names them, and one undo puts the mode and every one of them back. A flip
// to Audio from Signed takes Peak in the same step (Signed is Control's).
//
// NO WINDOW: the card is the whole device.

namespace feathers::ui {

class Notifier;
class WidgetRegistry;

/// **THE MODE'S ROAD** — `DeviceControl` for Mode, whose gesture is recorded as one step with the Detector it moved and
/// the routes the flip deleted, and said in the tray with those routes named.
class FollowerModeControl final : public DeviceControl {
public:
	FollowerModeControl(backplane::DeviceHost& host, DeviceInstanceId device, ModState* mod, EditHistory* history,
	                    Notifier* notifier);
	void setValue(double value) override;
	void beginGesture() override;
	void endGesture() override;
	/// THE TRAY LINE THE LAST FLIP SAID — for a test, and for a surface with no notifier.
	[[nodiscard]] const std::string& lastSaidForTest() const noexcept { return lastSaid_; }

private:
	backplane::DeviceHost& host_;
	Notifier* notifier_ = nullptr;
	float modeBefore_ = 0.0f;
	float detectorBefore_ = 0.0f;
	std::vector<backplane::Route> pruned_;
	std::string lastSaid_;
};

class FollowerCard final : public FxCard {
public:
	FollowerCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr,
	             ModState* mod = nullptr, EditHistory* history = nullptr);
	/// **A LANE OR A SLOT IS NOT AN ID** — `DeviceControl`'s guard, here too.
	FollowerCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr, ModState* = nullptr,
	             EditHistory* = nullptr) = delete;
	~FollowerCard() override;

	void resized() override;
	float laneHeightAt(float cardWidth) const override;
	void setModulation(ModState* mod, EditHistory* history) override;
	void foldedChanged() override;
	bool updateContent() override;
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override { drawSlotMenu(canvas, kMuted); }
	bool headSourceFocused() const override;
	bool headArmed() const override;

	const device::Descriptor* descriptor() const override { return &mod::follower::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }

	static constexpr float kPadX = 9.0f;
	static constexpr float kGap = 6.0f;
	/// THE KNOB ROW'S GAP — narrower than `kGap`, so the labels get the room.
	static constexpr float kRowGap = 2.0f;
	static constexpr float kTabsHeight = 18.0f;
	static constexpr float kHearsHeight = 12.0f;
	static constexpr float kPlotHeight = 78.0f;
	static constexpr float kInputColumnWidth = 46.0f;
	static constexpr float kFaceDiameter = Knob::kEngineRailDiameter;
	/// THE MODE'S SHARE OF ITS ROW; Poly takes the rest at its own width.
	static constexpr float kModeShare = 0.62f;

	static constexpr float controlsHeight() { return Knob::columnHeight(kFaceDiameter); }
	static constexpr float bodyHeight() {
		return kGap + kTabsHeight + kGap + kTabsHeight + kGap + kHearsHeight + kGap + kPlotHeight + kGap
		       + controlsHeight() + kGap;
	}
	/// OPEN, IN A VERTICAL LANE — `MathCard::openHeight`'s shape, for `ModPanel::bodyHeightOf`.
	static float openHeight(size_t outputRows);

	[[nodiscard]] SegmentedControl& mode() const { return *mode_; }
	[[nodiscard]] SwitchChip& poly() const { return *poly_; }
	[[nodiscard]] Knob& input() const { return *input_; }
	/// THE DETECTOR SHOWING — Control's three entries or Audio's two.
	[[nodiscard]] SegmentedControl& detector() const { return audioLaid_ ? *detectorAudio_ : *detectorControl_; }
	[[nodiscard]] Knob& attack() const { return *attack_; }
	[[nodiscard]] Knob& release() const { return *release_; }
	[[nodiscard]] Knob& gain() const { return *gain_; }
	[[nodiscard]] Knob& floor() const { return *floor_; }
	[[nodiscard]] FollowerPlot& plot() const { return *plot_; }

	[[nodiscard]] DeviceControl& control(size_t index) const;
	[[nodiscard]] FollowerModeControl& modeControl() const { return *modeControl_; }
	/// Every device control this card puts a widget on (row indices), asked of the widgets' own controls.
	[[nodiscard]] std::vector<size_t> addressedParamsForTest() const;
	/// THE LINE SAYING WHAT IT HEARS, as drawn.
	[[nodiscard]] std::string hearsForTest() const { return hears(); }

private:
	device::Modulatable* enableModulatable() override;
	[[nodiscard]] float fieldOf(mod::follower::Param p) const;
	[[nodiscard]] bool underAudio() const;
	[[nodiscard]] bool underPoly() const;
	[[nodiscard]] bool underSigned() const;
	[[nodiscard]] std::string hears() const;
	void layoutControls();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	Notifier* notifier_ = nullptr;
	ModState* mod_ = nullptr;
	EditHistory* history_ = nullptr;
	mutable std::vector<std::unique_ptr<DeviceControl>> controls_;
	/// OWNED BY `controls_` at Mode's index.
	FollowerModeControl* modeControl_ = nullptr;
	std::unique_ptr<SegmentedControl> mode_;
	std::unique_ptr<SwitchChip> poly_;
	std::unique_ptr<SegmentedControl> detectorControl_;
	std::unique_ptr<SegmentedControl> detectorAudio_;
	std::unique_ptr<Knob> input_;
	std::unique_ptr<Knob> attack_;
	std::unique_ptr<Knob> release_;
	std::unique_ptr<Knob> gain_;
	std::unique_ptr<Knob> floor_;
	std::unique_ptr<FollowerPlot> plot_;
	/// WHAT THE LAYOUT WAS LAST DONE FOR — a flip of any re-lays the card and re-draws the line.
	bool audioLaid_ = false;
	bool polyLaid_ = false;
	bool signedLaid_ = false;
};

} // namespace feathers::ui
