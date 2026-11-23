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

#include "fx/delay/delay_types.h"
#include "plugin/ui/device_control.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

// THE DELAY'S HOVER TEXT THAT FOLLOWS ITS MODE, SYNC AND DUB (TV audit). Shared by the card and the window: each reads the three choices
// and calls `applyTips`, which sets the texts only when one of them changes. An empty text restores the declared one.
// (Feedback keeps its declared text: the loop's soft clip runs in every Mode, and Tape adds its own saturation on top of it.)

namespace feathers::ui::delay_tips {

namespace dl = feathers::fx::delay;

/// Time: a multiplier on the base Sync selects; in Multi-Tap the multiplier sets the pattern's length, in Reverse the chunk's.
inline std::string timeTip(int mode, int sync) {
	const bool freeTime = sync >= 0 && static_cast<size_t>(sync) < dl::kFreeTimeCount;
	const std::string base = freeTime ? std::string("the free time") : std::string("the ") + dl::kSyncNames[static_cast<size_t>(sync)] + " division";
	switch (static_cast<dl::Mode>(mode)) {
	case dl::Mode::MultiTap:
		return "How long the whole tap pattern is, as a multiple of " + base + ": its taps sit inside it";
	case dl::Mode::Reverse:
		return "How long each reversed chunk is, as a multiple of " + base + "; a reversed tap stops at half the line";
	default:
		return "How many of " + base + " one tap is: always a multiplier, whatever the base is";
	}
}

/// Mix: with Dub on it is the send into the line, the dry beside it at full level; off, the declared dry against wet.
inline std::string mixTip(int dub) {
	return dub != 0 ? std::string("How much of the input reaches the line at all, with the dry passing at full level beside it") : std::string();
}

/// Sets Time's and Mix's texts for (Mode, Sync, Dub), once per change: `key` is the caller's last-applied state (-1 before the first).
inline void applyTips(int mode, int sync, int dub, int& key, const std::function<DeviceControl&(dl::Param)>& control) {
	const int m = std::clamp(mode, 0, static_cast<int>(dl::Mode::kCount) - 1);
	const int s = std::clamp(sync, 0, static_cast<int>(dl::kSyncChoiceCount) - 1);
	const int d = dub != 0 ? 1 : 0;
	const int state = (m * static_cast<int>(dl::kSyncChoiceCount) + s) * 2 + d;
	if (state == key) {
		return;
	}
	key = state;
	control(dl::Param::Time).setTooltipOverride(timeTip(m, s));
	control(dl::Param::Mix).setTooltipOverride(mixTip(d));
}

} // namespace feathers::ui::delay_tips
