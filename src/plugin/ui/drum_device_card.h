/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/device_host.h"
#include "fx/drum/drum_types.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/drum_device_ui.h"
#include "plugin/ui/drum_hit_picture.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/fx_card.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include "plugin/ui/widget_registry.h"

#include <memory>
#include <string>

// THE DRUM DEVICE'S CARD: composed around the hit picture (the standing rule: a visualiser, the controls on the plot), not a list of knobs.
// Under it: Kind, the Keys chip, Note (One Key only: hidden, not greyed, when Pitched), a play button where the host has a note road, and Level and Tune. Everything else is in the window.

namespace feathers::ui {

class DrumDeviceCard final : public FxCard {
public:
	DrumDeviceCard(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	DrumDeviceCard(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	const device::Descriptor* descriptor() const override { return &dd::kDescriptor; }
	const backplane::DeviceHost* deviceHost() const override { return &host_; }
	DeviceInstanceId device() const override { return device_; }
	void drawContent(visage::Canvas& canvas) override;
	void drawHeadExtras(visage::Canvas& canvas) override;
	bool updateContent() override;
	void resized() override;
	void foldedChanged() override;
	void setModulation(ModState* state, EditHistory* history) override;
	float laneHeightAt(float) const override { return folded() ? kHeadHeight : openHeight(); }

	static constexpr float kHeadHeight = 24.0f;
	static constexpr float kPad = 10.0f;
	static constexpr float kGap = 6.0f;
	static constexpr float kPictureHeight = 120.0f;
	static constexpr float kPillHeight = 22.0f;
	static float openHeight();

	/// For the cases: the widgets, by what they are bound to.
	[[nodiscard]] HitPicture& pictureForTest() { return picture_; }
	[[nodiscard]] ValueBox& kindForTest() const { return *kind_; }
	[[nodiscard]] ValueBox& noteForTest() const { return *note_; }
	[[nodiscard]] SwitchChip& keysForTest() const { return *keys_; }
	[[nodiscard]] FaderPill& levelForTest() const { return *level_; }
	[[nodiscard]] FaderPill& tuneForTest() const { return *tune_; }
	/// Null where the host has no note road (the button is not built there).
	[[nodiscard]] DrumPlayButton* playForTest() const { return play_.get(); }
	[[nodiscard]] DrumDeviceModel& modelForTest() { return model_; }

private:
	device::Modulatable* enableModulatable() override { return &model_.control(dd::Param::Enabled); }
	void toggleEnabled();

	backplane::DeviceHost& host_;
	Notifier* notifier_ = nullptr;
	DeviceInstanceId device_ = kNoInstance;
	DrumDeviceModel model_;
	HitPicture picture_;
	std::unique_ptr<ValueBox> kind_, note_;
	std::unique_ptr<SwitchChip> keys_;
	std::unique_ptr<FaderPill> level_, tune_;
	std::unique_ptr<DrumPlayButton> play_;
	bool noteShown_ = true; ///< Note's last laid-out visibility (it follows Keys)
};

} // namespace feathers::ui
