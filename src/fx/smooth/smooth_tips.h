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

#include "fx/smooth/smooth_types.h"

#include <cstddef>
#include <iterator>

namespace feathers::fx::smooth {

//--------------------------------------------------------------------------------------------------//
// THE MODE'S HOVER TEXT — ONE TABLE, ONE ROW PER HARMONIC MODE (TV audit, 2026-11-22).                  //
//--------------------------------------------------------------------------------------------------//
//
// Three controls say something different in each mode: Feedback (only Fold and Mod can sustain), H Phase (in Poly
// it is the self-multiplication blend) and Harmonic (what it moves: four harmonics, four operators, or the Poly
// cascade). The card and the window set these as their controls' tooltip overrides, and refresh them on every
// editor tick, so a mode set by the host, a preset, or an undo reaches them the same way a stored value does.
//
// The text is plain musician language: no firmware or hardware names, no names of people.

struct ModeTips {
	const char* feedback; ///< `smooth_fdbk`: the Feedback pill's amount
	const char* phase;    ///< `smooth_harmonic_phase`: H Phase, in the window
	const char* harmonic; ///< `smooth_harmonic_coord`: Harmonic, on the card and in the window
};

inline constexpr size_t kHarmonicModeTipCount = std::size(kHarmonicModeNames);

inline constexpr const char* kFeedbackTip =
	"The wet fed back into the input, up to a quarter, reshaping the curve on each pass. Heard while its power is on";
inline constexpr const char* kFeedbackSustainTip =
	"The wet fed back into the input, up to a quarter. In Fold and Mod it can sustain itself after the input stops. "
	"Heard while its power is on";
inline constexpr const char* kFeedbackPolyTip =
	"The wet fed back into the input, up to a quarter: less the further Harmonic goes into the zone, and -3 dB on top. "
	"Heard while its power is on";
inline constexpr const char* kPhaseTip =
	"Shifts where inside the mode you stand against its blend, an eighth per step, and wraps. Eight steps are one lap "
	"of Harmonic";
inline constexpr const char* kPhasePolyTip =
	"Shifts where inside the mode you stand against its blend, an eighth per step, and wraps. Eight steps are one lap "
	"of Harmonic. In Poly it sets the self-multiplication blend";
inline constexpr const char* kHarmonicHarmonicsTip =
	"Where inside the waveshaper: moves a blend of its four harmonics, each on its own slow triangle. It keeps turning; "
	"each whole number is another lap of the same mode. Grab a digit to drag by that place";
inline constexpr const char* kHarmonicOperatorsTip =
	"Where inside the mode: moves a blend of its four operators, each on its own slow triangle. It keeps turning; each "
	"whole number is another lap of the same mode. Grab a digit to drag by that place";
inline constexpr const char* kHarmonicPolyTip =
	"Where inside the polynomial: moves how far the cascade is run a second time, blending one pass into two. It keeps "
	"turning; each whole number is another lap of the same mode. Grab a digit to drag by that place";

/// THE TABLE. Index = `Settings::harmonicMode`, in `kHarmonicModeNames` order: 3579, 3579wm, FM, Fold, Ring, Add, Mod, Poly.
inline constexpr ModeTips kModeTips[] = {
	/* 3579  */ { kFeedbackTip,         kPhaseTip,     kHarmonicHarmonicsTip },
	/* 3579wm*/ { kFeedbackTip,         kPhaseTip,     kHarmonicHarmonicsTip },
	/* FM    */ { kFeedbackTip,         kPhaseTip,     kHarmonicOperatorsTip },
	/* Fold  */ { kFeedbackSustainTip,  kPhaseTip,     kHarmonicOperatorsTip },
	/* Ring  */ { kFeedbackTip,         kPhaseTip,     kHarmonicOperatorsTip },
	/* Add   */ { kFeedbackTip,         kPhaseTip,     kHarmonicOperatorsTip },
	/* Mod   */ { kFeedbackSustainTip,  kPhaseTip,     kHarmonicOperatorsTip },
	/* Poly  */ { kFeedbackPolyTip,     kPhasePolyTip, kHarmonicPolyTip },
};
static_assert(std::size(kModeTips) == kHarmonicModeTipCount, "one row per harmonic mode");

/// THE ROW FOR A MODE, clamped to the table: a stored mode outside the menu reads as the nearest end.
[[nodiscard]] inline const ModeTips& modeTipsFor(int harmonicMode) noexcept {
	const int last = static_cast<int>(kHarmonicModeTipCount) - 1;
	const int clamped = harmonicMode < 0 ? 0 : (harmonicMode > last ? last : harmonicMode);
	return kModeTips[static_cast<size_t>(clamped)];
}

} // namespace feathers::fx::smooth
