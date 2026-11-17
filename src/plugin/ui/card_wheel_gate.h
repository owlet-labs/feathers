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

#include "plugin/ui/fx_card.h"

#include <visage/ui.h>

namespace feathers::ui {

// **ON A CARD, THE WHEEL IS THE LANE'S UNLESS THE CARD IS FOCUSED.** Scrolling across a lane must not be taken over
// by whatever picture or list happens to pass under the pointer. A widget that takes the wheel asks this first and,
// on false, answers false so visage hands the event up to the lane.
//   - A frame with NO card above it (an expanded window, a stage) is its own focused surface: true.
//   - Otherwise true when ANY card above it (itself, or the parent rack card of a card inside a rack) is the
//     selected one (`FxCard::headFocused`), or when the main modifier (Ctrl, Cmd on macOS) is held.
[[nodiscard]] inline bool cardTakesWheel(const visage::Frame& frame, const visage::MouseEvent& e) {
	bool onCard = false;
	for (const visage::Frame* at = &frame; at != nullptr; at = at->parent()) {
		if (const auto* card = dynamic_cast<const FxCard*>(at); card != nullptr) {
			onCard = true;
			if (card->headFocused()) {
				return true;
			}
		}
	}
	return !onCard || e.isMainModifier();
}

} // namespace feathers::ui
