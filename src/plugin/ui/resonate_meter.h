/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "backplane/device_host.h"
#include "device/modulatable.h"
#include "plugin/ui/tooltip.h"

#include <visage/ui.h>

// RESONATE'S INPUT METER — the level bar (the input as Drive hears it), and beside it the PLUCK STRIP, which lights for a
// quarter second when the main string is plucked: by a played note or the Cycle (v19: the input's onset detector, whose
// value and threshold the strip used to draw, is gone, by design). Its own frame, so it speaks through the hover road
// like any other widget — with Pluck's declaration, the control that sets how hard those plucks are.

namespace feathers::ui {

class ResonateMeter final : public visage::Frame, public TooltipSource {
public:
	ResonateMeter(const backplane::DeviceHost& host, DeviceInstanceId id, device::Modulatable& pluck);
	ResonateMeter(const backplane::DeviceHost&, size_t, device::Modulatable&) = delete;

	void draw(visage::Canvas& canvas) override;
	[[nodiscard]] const char* tooltipAt(visage::Point) const override;
	/// A meter takes no press: it speaks for Pluck but offers no way to change it.
	void offeredControls(std::vector<Offered>&) const override {}

	/// READ THE DEVICE AND REDRAW IF THE METER WOULD CHANGE. True when it redrew.
	bool refresh();

	static constexpr float kStripWidth = 4.0f;
	/// HOW MANY TICKS A PLUCK LIGHTS THE STRIP — the ring view's flash, the same quarter second.
	static constexpr float kFlashTicks = 96.0f;

	struct Fractions {
		float level = 0.0f;
		bool flashing = false;
		bool operator==(const Fractions&) const = default;
	};
	[[nodiscard]] Fractions fractions() const;
	[[nodiscard]] bool hasReadings() const;

private:
	const backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	device::Modulatable& pluck_;
	Fractions drawn_ {};
	bool drawnHad_ = false;
	bool drawnOnce_ = false;
};

} // namespace feathers::ui
