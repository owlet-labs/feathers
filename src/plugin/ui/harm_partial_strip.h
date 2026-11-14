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
#include "fx/harm/harm_types.h"
#include "plugin/ui/device_control.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/switch_chip.h"

#include <visage/ui.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

// HARM'S PARTIALS STRIP — four columns, always visible, whichever partial the window has open.
//
// Each column is one partial: its number and ratio (a press on them OPENS that partial in the window — selecting is view
// state, not a parameter), an Active switch, and its Level. So every partial is switched on and off and set to a level from any
// tab, and the selected column wears the selection outline. The strip owns no tab state: the window tells it which is
// selected and is told when a column asks to be.

namespace feathers::ui {

class ModState;
class Notifier;

class HarmPartialStrip : public visage::Frame {
public:
	HarmPartialStrip(backplane::DeviceHost& host, DeviceInstanceId id, Notifier* notifier = nullptr);
	HarmPartialStrip(backplane::DeviceHost&, size_t, Notifier* = nullptr) = delete; // a lane or a slot is not an id

	static constexpr float kHeaderHeight = 16.0f;
	static constexpr float kRowHeight = 26.0f;
	static constexpr float kGap = 4.0f;
	static constexpr float kColumnGap = 10.0f;
	static constexpr float kPadX = 6.0f;
	static constexpr float kPadY = 4.0f;
	static constexpr float kChipWidth = 52.0f;
	static constexpr float kHeight = kPadY + kHeaderHeight + kGap + kRowHeight + kPadY;

	/// Which partial the window has open, 0..3.
	void setSelected(int partial);
	int selected() const noexcept { return selected_; }
	/// Told when a column's header is pressed.
	std::function<void(int)> onSelect;

	/// WHERE THINGS ARE, in this frame's own coordinates — drawn, laid out and hit-tested from these.
	visage::Bounds columnBounds(int partial) const;
	visage::Bounds headerBounds(int partial) const;
	visage::Bounds chipBounds(int partial) const;
	visage::Bounds pillBounds(int partial) const;

	/// WHAT THE HEADER SAYS: the partial's number and its ratio, with the interval's name where the column is wide enough.
	std::string headerText(int partial) const;

	/// RE-READ WHAT THE DEVICE HOLDS (ratios, levels, which are on) and say whether anything the strip shows moved.
	bool refresh();

	void draw(visage::Canvas& canvas) override;
	void resized() override;

	void setModulation(ModState* mod, EditHistory* history);

	SwitchChip* activeChip(int partial) const { return chips_[static_cast<size_t>(partial)].get(); }
	FaderPill* levelPill(int partial) const { return pills_[static_cast<size_t>(partial)].get(); }

private:
	DeviceControl& control(fx::harm::Param param) const;
	size_t harmonicIndex(int partial) const;
	bool partialActive(int partial) const;

	backplane::DeviceHost& host_;
	DeviceInstanceId device_ = kNoInstance;
	int selected_ = 0;
	mutable std::array<std::unique_ptr<DeviceControl>, static_cast<size_t>(fx::harm::Param::kCount)> controls_ {};
	std::array<std::unique_ptr<SwitchChip>, fx::harm::kPartialCount> chips_;
	std::array<std::unique_ptr<FaderPill>, fx::harm::kPartialCount> pills_;
	std::array<std::string, fx::harm::kPartialCount> drawnHeader_;
	std::array<std::string, fx::harm::kPartialCount> drawnReadout_;
	std::array<int, fx::harm::kPartialCount> drawnActive_ { -1, -1, -1, -1 };
};

} // namespace feathers::ui
