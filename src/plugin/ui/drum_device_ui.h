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
#include "dsp/drum_voice.h"
#include "fx/drum/drum.h"
#include "fx/drum/drum_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/drum_hit_picture.h"
#include "plugin/ui/tooltip.h"

#include <array>
#include <functional>
#include <memory>
#include <vector>
#include <visage/ui.h>

// THE DRUM DEVICE'S UI, SHARED BY ITS CARD AND ITS WINDOW: the model (a device's row read as the drum engine's own laws, controls bound by
// device control INDEX through `DeviceControl`) and the play button.

namespace feathers::ui {

namespace dd = feathers::fx::drumdevice;

/// ONE DRUM DEVICE AS THE UI READS IT. Every value comes through the host by control index; nothing here names a parameter id.
class DrumDeviceModel {
public:
	DrumDeviceModel(backplane::DeviceHost& host, DeviceInstanceId id) : host_(host), id_(id) {}
	[[nodiscard]] backplane::DeviceHost& host() const { return host_; }
	[[nodiscard]] DeviceInstanceId id() const { return id_; }
	/// The control at `p`, made once.
	[[nodiscard]] DeviceControl& control(dd::Param p) const;
	/// The row as the engine plays it now (modulation in), cleaned into the declared ranges.
	[[nodiscard]] dd::Settings settings() const;
	[[nodiscard]] feathers::drum::Kind kind() const;
	/// Whether `p` is read by the current Kind (a control with no gate always is): the honesty rule's question.
	[[nodiscard]] bool relevant(dd::Param p) const;
	/// Whether the drum is a Drone (a kind that can be held, set to Drone): it sounds with no key, so the Trig button has nothing to do and is hidden.
	[[nodiscard]] bool droneOn() const;
	/// The hit as the drum engine plays it at velocity 0.8 (the panel's), and the pitch it settles on.
	[[nodiscard]] feathers::drum::Laws laws() const;
	[[nodiscard]] feathers::drum::SettledPitch settled() const;
	[[nodiscard]] double decaySeconds() const;
	[[nodiscard]] double levelDb() const;
	[[nodiscard]] int noteKey() const;
	/// The key the picture and the play button use when the drum is Pitched (middle C).
	static constexpr int kPictureKey = 60;
	/// The picture's handles for the current Kind, each bound to its device control (the kind's own only).
	[[nodiscard]] HitPicture::Controls handles() const;
	void setModulation(ModState* mod, EditHistory* history);

private:
	backplane::DeviceHost& host_;
	DeviceInstanceId id_;
	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(dd::Param::kCount)> controls_ {};
};

/// **A DRUM'S TRIG BUTTON** (the house trigger jack, `TrigPad::drawJack`): a press strikes the drum's Note through the host's note road (`DeviceHost::postNote`), the release
/// lets it go; velocity 100/127. Offered only where the host has such a road (`canPostNote`): a host with none draws none.
class DrumPlayButton : public visage::Frame, public TooltipSource {
public:
	static constexpr float kVelocity = 100.0f / 127.0f;
	DrumPlayButton(backplane::DeviceHost& host, std::function<int()> key);
	~DrumPlayButton() override;
	void draw(visage::Canvas& canvas) override;
	const char* tooltipAt(visage::Point) const override;
	void offeredControls(std::vector<Offered>&) const override {}
	void press();
	void release();
	[[nodiscard]] bool held() const noexcept { return held_; }

private:
	backplane::DeviceHost& host_;
	std::function<int()> key_;
	bool held_ = false;
	int heldKey_ = 0;
};

} // namespace feathers::ui
