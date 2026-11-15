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
#include "plugin/ui/drum_device_pad.h"
#include "plugin/ui/drum_device_ui.h"
#include "plugin/ui/drum_hit_picture.h"
#include "plugin/ui/drum_pitch_readout.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"

#include <array>
#include <memory>
#include <string>
#include <vector>

// THE DRUM DEVICE'S WINDOW: the hit picture with its handles, the strike controls, the terrain (the explore pad over Travel X and Y, which are
// unbounded, with Zone, Offset, Gamma and Rotate beside it), the Terrain Pitch mode and the settled-pitch readout, the seven scalers, and every
// control of the Kind. A control the Kind does not read is hidden, not greyed. Every control of the device is here, bound by index.

namespace feathers::ui {

class DrumDeviceExpanded final : public ExpandedView {
public:
	DrumDeviceExpanded(backplane::DeviceHost& host, WidgetRegistry& registry, DeviceInstanceId id, Notifier* notifier = nullptr);
	DrumDeviceExpanded(backplane::DeviceHost&, WidgetRegistry&, size_t, Notifier* = nullptr) = delete;

	void draw(visage::Canvas& canvas) override;
	void resized() override;
	visage::Point minimumSize() const override { return { kWidth, kMinimumHeight }; }
	visage::Point maximumSize() const override { return { kWidth, kNoCap }; }
	std::string title() const override { return "Drum"; }
	std::string headerTitle() const override;
	bool updateContent() override;
	void modulationBound(ModState* mod, EditHistory* history) override;

	static constexpr float kWidth = 640.0f;
	static constexpr float kMinimumHeight = 690.0f;
	static constexpr float kPad = 12.0f;
	static constexpr float kPictureHeight = 210.0f;
	static constexpr float kTerrainSize = 148.0f;

	/// For the cases.
	[[nodiscard]] DrumDeviceModel& modelForTest() { return model_; }
	[[nodiscard]] HitPicture& pictureForTest() { return picture_; }
	/// The widget bound to control `p`, null where the control has none of its own (the picture's handles apart).
	[[nodiscard]] visage::Frame* widgetForTest(dd::Param p) const { return widgets_[static_cast<size_t>(p)].get(); }
	[[nodiscard]] DrumDevicePad& padForTest() const { return *pad_; }
	[[nodiscard]] PitchReadout& readoutForTest() const { return *readout_; }
	[[nodiscard]] DrumPlayButton* playForTest() const { return play_.get(); }

private:
	enum class Kind : uint8_t { Box, Segmented, Pill, Zone, Chip };
	void layOut();
	void follow();

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	DrumDeviceModel model_;
	HitPicture picture_;
	std::array<std::unique_ptr<visage::Frame>, static_cast<size_t>(dd::Param::kCount)> widgets_ {};
	std::unique_ptr<DrumDevicePad> pad_;
	std::unique_ptr<SettledPitchControl> settledControl_;
	std::unique_ptr<PitchReadout> readout_;
	std::unique_ptr<DrumPlayButton> play_;
	dd::Settings padContext_ {};
	EditHistory* padGroup_ = nullptr;
	feathers::drum::Kind shownKind_ = feathers::drum::Kind::kCount;
	bool shownGate_ = false;
	int shownKeys_ = 0;
	bool shownSettled_ = false;
	float contentBottom_ = 0.0f;
};

} // namespace feathers::ui
