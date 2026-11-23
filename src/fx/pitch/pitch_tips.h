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

namespace feathers::fx::pitch {

//--------------------------------------------------------------------------------------------------//
// THE SCALE'S HOVER TEXT FOR SHIFT AND FINE (TV audit, 2026-11-22).                                    //
//--------------------------------------------------------------------------------------------------//
//
// Shift snaps to the scale only when a Scale is chosen; Free leaves it unquantised. Fine is applied after the snap
// in either case, so with Free there is no snap for it to be applied after. The card and the window set these as
// the controls' tooltip overrides (`DeviceControl::setTooltipOverride`), refreshed on every tick.

struct ScaleTips {
	const char* shift; ///< `pitch_shift`
	const char* fine;  ///< `pitch_fine`
};

inline constexpr ScaleTips kFreeTips = {
	"The interval, in semitones, played exactly as the knob sets it",
	"Cents on top of the shift, detuning it by hundredths of a semitone",
};

inline constexpr ScaleTips kScaledTips = {
	"The interval, in semitones. With a Scale chosen it snaps to the nearest degree, in whichever octave the knob is in",
	"Cents on top of the shift. Applied after the scale snap, so a locked interval can still be detuned",
};

/// THE ROW FOR A SCALE CHOICE: `scaled` is true for any Scale other than Free.
[[nodiscard]] inline const ScaleTips& tipsFor(bool scaled) noexcept {
	return scaled ? kScaledTips : kFreeTips;
}

} // namespace feathers::fx::pitch
