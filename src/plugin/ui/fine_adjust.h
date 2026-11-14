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

namespace feathers::ui {

/// **WHAT HOLDING SHIFT MULTIPLIES A DRAG'S TRAVEL BY, ON EVERY DRAGGABLE CONTROL.** Shift is the modifier every
/// DAW's own controls use for fine adjustment, so a hand already knows it. One number for the knob, the value box, the
/// fader pill and a route's amount drag, so the four cannot come to feel different.
///
/// **AND ONE RULE FOR THE MODIFIER CHANGING MID-DRAG: RE-ANCHOR.** Pressing or releasing Shift while the button is down
/// restarts the drag from where the value is NOW (`Knob::dragTo`'s original rule). Rescaling the travel already covered
/// instead would make the value jump the instant the key moved, because everything before it would suddenly be worth a
/// fifth of what it was.
inline constexpr float kFineAdjust = 0.2f;

} // namespace feathers::ui
