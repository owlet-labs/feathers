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

#include "fx/comp/comp_types.h"

// COMP'S HOVER TEXT FOLLOWS MODE. Three controls change meaning with Mode, so their tooltip says what the
// control does in the mode the device is in. The card and the window both hand these strings to
// `DeviceControl::setTooltipOverride` from `layOut`, the road every Mode change takes.
namespace feathers::fx::comp {

// The per-mode text for a control whose meaning turns with Mode, or "" for a control whose descriptor
// tooltip already holds. Mode is the stored Choice index: 0 Compress, 1 Limit, 2 Compander.
inline const char* modeTip(Param p, int mode) {
	switch (p) {
	case Param::Threshold:
		// Limit has no threshold (the control is gated out there), so it takes Compress's sentence.
		return mode == static_cast<int>(Mode::Compander)
		           ? "The level above which the signal is turned down. Here it is Ref, the level left alone: "
		             "louder is turned down, quieter is turned up"
		           : "The level above which the signal is turned down";
	case Param::Release:
		return mode == static_cast<int>(Mode::Compander)
		           ? "How fast the level follower falls — off at the bottom is instant"
		           : "How fast gain reduction lets go — off at the bottom is instant";
	case Param::Drive:
		switch (static_cast<Mode>(mode)) {
		case Mode::Limit:
			return "Gain in front of the limiter — how hard the signal is pushed into it; negative backs it off. "
			       "The dry is never driven";
		case Mode::Compander:
			return "Gain in front of the compander — how hard the signal is pushed into it; negative backs it off. "
			       "The dry is never driven";
		default:
			return "Gain in front of the compressor — how hard the signal is pushed into it; negative backs it off. "
			       "The dry is never driven";
		}
	default:
		return "";
	}
}

} // namespace feathers::fx::comp
