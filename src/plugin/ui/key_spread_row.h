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

#include "plugin/ui/fader_pill.h"
#include "plugin/ui/switch_chip.h"

#include <string>
#include <visage/ui.h>

namespace feathers::ui {

/// **A SWITCH CHIP AND A FADER, SIDE BY SIDE AND SEPARATE** (a Type cell's Key and Spread row). The chip is the switch alone; the fader is an
/// amount-only pill, lit whatever the switch says, because the amount works with the switch off. They are two controls on one row, not one
/// control with a power: the fader never reads as "how much Key". Neither writes the other's parameter; each is a plain child, so a walk of
/// the tree finds both (`ControlFace`).
class KeySpreadRow final : public visage::Frame {
public:
	/// The chip's width: "Key" at the label size with a pixel of air either side.
	static constexpr float kChipWidth = 24.0f;
	static constexpr float kGap = 2.0f;

	/// @param key     the switch (borrowed, as the chip's control is)
	/// @param spread  the amount the fader writes
	/// @param keyLabel   the chip's caption
	/// @param spreadLabel the fader's caption
	KeySpreadRow(device::Modulatable& key, device::Modulatable& spread, std::string keyLabel, std::string spreadLabel)
	    : chip_(key, std::move(keyLabel)), fader_(spread, std::move(spreadLabel)) {
		addChild(&chip_);
		addChild(&fader_);
	}
	[[nodiscard]] SwitchChip& chip() noexcept { return chip_; }
	[[nodiscard]] FaderPill& fader() noexcept { return fader_; }
	[[nodiscard]] const SwitchChip& chip() const noexcept { return chip_; }
	[[nodiscard]] const FaderPill& fader() const noexcept { return fader_; }
	void setReadout(std::string readout) { fader_.setReadout(std::move(readout)); }
	void resized() override {
		const float w = static_cast<float>(width());
		const float h = static_cast<float>(height());
		const float chipW = std::min(kChipWidth, w);
		const float chipH = std::min(SwitchChip::kHeight, h);
		chip_.setBounds(0.0f, (h - chipH) * 0.5f, chipW, chipH);
		fader_.setBounds(chipW + kGap, 0.0f, std::max(0.0f, w - chipW - kGap), h);
	}

private:
	SwitchChip chip_;
	FaderPill fader_;
};

} // namespace feathers::ui
