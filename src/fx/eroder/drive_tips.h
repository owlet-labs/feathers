/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "fx/eroder/eroder_types.h"

#include <cctype>
#include <string>
#include <string_view>

namespace feathers::fx::eroder {

/// **WHAT A DRIVE UNIT'S CONTROLS DO FOR THE TYPE IT IS ON** — the hover text of the Type box, the Amount knob and the Shape box, one row per `DriveMode`,
/// for the Drive and both Etch slots, on the card and in the window. The DECLARED tooltips (`ParamDesc::tooltip`) stay the general text for every Type
/// at once; a surface that knows the unit's Type puts this row's text over them (`DeviceControl::setTooltipOverride`).
///
/// EVERY SENTENCE IS READ FROM THE ENGINE (`drive_stage.h`, `deriveDrive` and `shapeDrive` in `dust.cpp`), not from a Type's name: the spans below are
/// `kDriveLog2Min`..`kDriveLog2Max` (an eighth to sixteen times), `kCrushTopHz` and `kCrushOctaves` (about 2 ms), `kCrushBitsFine` and `kCrushBitsCoarse`,
/// `kSlewTopHz` and `kSlewBottomHz`, `kKeyMultBottom` and `kKeyMultTop`, `kPaunchAmountMax`. A number changed there is changed here.
///
/// `amountKeyed` is the Amount's text with the unit's Key switch on (null for a Type with no Key). `shape` is null where the Shape row is not the Shape: the four
/// Paunch Types show their own Angle there, with a tooltip of its own, which the row then leaves as declared.
struct DriveTip {
	const char* type;
	const char* amount;
	const char* amountKeyed;
	const char* shape;
};

inline constexpr DriveTip kDriveTips[] = {
	// Soft
	{ "A tanh: it rounds the peaks off smoothly, and Shape can starve the quiet parts. Hard sharpens its knee.",
	  "How hard the signal is pushed into the tanh: a gain from an eighth at 0% to sixteen times at 100%, with the level held about where it was. At 0% it is not run.",
	  nullptr,
	  "Starves the quiet parts. At 0% it is a plain tanh; as it rises a dead zone opens round zero, so everything under a threshold (up to 30% of full scale at 100%) is silent and what is over it comes up from there." },
	// Tube
	{ "A tanh whose two halves differ, so even harmonics appear; the offset it leaves is for the DC block to take out. Hard sharpens its knee.",
	  "How hard the signal is pushed into the curve: a gain from an eighth at 0% to sixteen times at 100%, with the level held about where it was. At 0% it is not run.",
	  nullptr,
	  "How unequal the two halves are: the negative half tops out 1.5 times lower than the positive at 0% and 8 times lower at 100%, so the even harmonics grow with it." },
	// Fold
	{ "A sine wavefolder: what goes past its first peak folds back on itself, which makes bright, dense harmonics. Sharp turns its folds toward a triangle's.",
	  "How far the signal is pushed into the folder: a gain from an eighth at 0% to sixteen times at 100%, with the level held about where it was. The higher it goes the more times the wave folds. At 0% it is not run.",
	  nullptr,
	  "How far off centre the folder is read, which brings in the even harmonics. At 0% there are none. The offset it leaves is for the DC block to take out." },
	// Wreck
	{ "A tanh into a sine folder, in series. In Dust the line's feedback is taken after both, so Resonance rings through them. Bias adds even harmonics.",
	  "How hard the signal is pushed into the tanh that feeds the folder: a gain from an eighth at 0% to sixteen times at 100%, with the level held about where it was. At 0% it is not run.",
	  nullptr,
	  "How far the folder is pushed past its first peak: at 0% the tanh just reaches it, at 100% it reaches five times as far, so the wave folds many more times." },
	// Jag
	{ "A tanh into a triangle folder in series, Wreck's sharp counterpart. In Dust the feedback is taken after both. Bias adds even harmonics.",
	  "How hard the signal is pushed into the tanh that feeds the folder: a gain from an eighth at 0% to sixteen times at 100%, with the level held about where it was. At 0% it is not run.",
	  nullptr,
	  "How far the triangle folder is pushed past its first peak: at 0% the tanh just reaches it, at 100% it reaches five times as far, so the wave folds many more times." },
	// Lean (w)
	{ "A tanh read from a bias, the two sides leaning opposite ways: the even harmonics come out in opposite polarity left and right, so they are stereo width, and in mono they cancel and leave the tanh's odd ones.",
	  "How hard both sides are pushed into the tanh: a gain from an eighth at 0% to sixteen times at 100%, with the level held about where it was. At 0% it is not run.",
	  nullptr,
	  "How far each side leans, so how much width: at 0% there is none and both sides are a plain tanh; the more it leans, the more even harmonics, in opposite polarity on the two sides." },
	// Chonk (w)
	{ "A tanh on the left; on the right what the tanh added is turned over by Shape, so the odd harmonics come out in opposite polarity left and right and add stereo width. In mono they cancel.",
	  "How hard both sides are pushed into the tanh: a gain from an eighth at 0% to sixteen times at 100%. At 0% it is not run.",
	  nullptr,
	  "How much of what the tanh adds is turned over on the right side only. At 0% both sides are the same tanh; at 50% the right side is left clean; at 100% what the tanh added to it is reversed, so the odd harmonics sit in opposite polarity left and right and the mono sum is the clean signal." },
	// Paunch-S (w)
	{ "Adds the rectified mid (left and right summed, flipped positive) to one side and takes it from the other: even harmonics that are stereo width and cancel in mono. The dry passes at unity, with no saturation.",
	  "How much of the rectified mid is added, in linear steps from nothing at 0% to four times the mid's level at 100%. The dry is not touched and there is no make-up. At 0% it is not run.",
	  nullptr,
	  nullptr },
	// Paunch-H (w)
	{ "Adds the squared mid to one side and takes it from the other: a pure octave above, as stereo width that cancels in mono. The dry passes at unity, with no saturation.",
	  "How much of the squared mid is added. The octave grows with the square of the Amount, from nothing at 0%. The dry is not touched and there is no make-up. At 0% it is not run.",
	  nullptr,
	  nullptr },
	// Paunch-OS (w)
	{ "Adds a pure third harmonic of the mid, sized to the signal's own level, to one side and takes it from the other: odd harmonics that are stereo width and vanish in mono. It makes no offset.",
	  "The size of the third harmonic, as a share of the signal's own level: it grows in step with the Amount, up to the fundamental's own level at 100%, at any signal level. The dry is not touched and there is no make-up. At 0% it is not run.",
	  nullptr,
	  nullptr },
	// Paunch-OH (w)
	{ "Adds the plain cube of the mid, with its fundamental cancelled, to one side and takes it from the other: odd harmonics that are stereo width and vanish in mono. It grows with level, so quiet passages stay clean and loud ones spread.",
	  "How much of the cubed mid is added: the third harmonic grows with the cube of both the Amount and the signal's level, so it is the Amount and a loud passage together that bring it up. The dry is not touched and there is no make-up. At 0% it is not run.",
	  nullptr,
	  nullptr },
	// Crush
	{ "No shaper: each sample is held for a time and rounded to a coarser step. Not oversampled and not level-matched.",
	  "How long each sample is held: none at 0%, about 2 ms at 100%, whatever the sample rate. At 0% it is not run.",
	  "How often a sample is taken, as a multiple of the played note's frequency, so the hold is tuned to the note: from \xC3\x97" "32 at 0% to \xC3\x97" "0.25 at 100%, shown on the knob.",
	  "How coarsely each held sample is rounded: 10 bits at 0%, down to a bit and a half at 100%." },
	// Decimate
	{ "No shaper: each sample is held for a time and nothing is rounded. Shape turns the held steps into lines. Not oversampled and not level-matched.",
	  "How long each sample is held: none at 0%, about 2 ms at 100%, whatever the sample rate. At 0% it is not run.",
	  "How often a sample is taken, as a multiple of the played note's frequency, so the hold is tuned to the note: from \xC3\x97" "32 at 0% to \xC3\x97" "0.25 at 100%, shown on the knob.",
	  "From the held steps (a plain sample-and-hold) at 0% to a straight line from each held value to the next at 100%, which lags by one hold. In between the two are blended." },
	// Slew
	{ "No shaper: a slew limiter, which lets the signal move only so fast. Not oversampled and not level-matched.",
	  "The limiter's rate, as a frequency: 20 kHz at 0%, 50 Hz at 100%. The lower it is, the slower the signal may move. At 0% it is not run.",
	  "The limiter's rate, as a multiple of the played note's frequency: from \xC3\x97" "32 at 0% to \xC3\x97" "0.25 at 100%, shown on the knob. The lower it is, the slower the signal may move.",
	  "The limiter's curve: a hard limit at 0%, a one-pole lowpass with its cutoff at the rate at 100%, and a blend between." },
	// Lopside
	{ "No shaper: a slew limiter at its hard limit that lets the signal rise and fall at different speeds. Not oversampled and not level-matched.",
	  "The limiter's rate, as a frequency: 20 kHz at 0%, 50 Hz at 100%. The lower it is, the slower the signal may move. At 0% it is not run.",
	  "The limiter's rate, as a multiple of the played note's frequency: from \xC3\x97" "32 at 0% to \xC3\x97" "0.25 at 100%, shown on the knob. The lower it is, the slower the signal may move.",
	  "The balance between rising and falling: at 0% rising steps are a quarter of falling ones, at 50% they are alike, at 100% rising steps are four times falling ones." },
};
static_assert(sizeof kDriveTips / sizeof kDriveTips[0] == static_cast<size_t>(DriveMode::kCount), "a Type needs its tooltip row");

[[nodiscard]] inline const DriveTip& driveTip(DriveMode mode) noexcept {
	return kDriveTips[static_cast<size_t>(mode)];
}

/// THE LEAN'S ROTATION ROW: the Rate and its Track form are shown for Lean as well as the Paunch Types, and what they turn is the lean (`drive_stage.h`), not an angle.
inline constexpr const char* kLeanRateTip = "How fast the lean swaps sides: each side's lean is scaled by the cosine of the turn, so it runs from full left through none to full right and back once a turn, and the mono sum never gains an even harmonic. At the bottom it is off and Shape's lean holds still.";
inline constexpr const char* kLeanRateMultTip = "With Track on, how fast the lean swaps sides as a multiple of the played note's frequency: 0.5 is an octave below it, 0.25 two. Drag lands on 1/16, 1/12, 1/8, 1/6, 1/5, 1/4, 1/3, 1/2 and 1; type any value in. Off (0) holds the lean still. With Track off the rate in hertz applies instead.";

/// **A DECLARED TOOLTIP WITHOUT ITS LEADING "X and Y only: " CLAUSE**, first letter raised. A control the cell shows only for its own Types needs no list of them. Text without
/// such a clause comes back as it is.
[[nodiscard]] inline std::string withoutOnlyClause(std::string_view text) {
	const size_t at = text.find(" only: ");
	if (at == std::string_view::npos || at > 48) {
		return std::string(text);
	}
	std::string out(text.substr(at + 7));
	if (!out.empty()) {
		out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
	}
	return out;
}

} // namespace feathers::fx::eroder
