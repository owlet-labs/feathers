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

#include "fx/clip/clip_types.h"
#include "fx/eroder/drive_tips.h"

namespace feathers::fx::clip {

/// **WHAT THE MODE BOX, THE DRIVE KNOB AND THE SHAPE KNOB DO FOR THE MODE THEY ARE ON**, one row per `Mode`. The declared tooltips (`ParamDesc::tooltip`) stay the general text for every
/// mode at once; the card puts this row's text over them (`DeviceControl::setTooltipOverride`).
///
/// EVERY SENTENCE IS READ FROM CLIP'S OWN ENGINE (`clip.cpp`), NOT FROM THE ERODER'S TABLE. Clip's Drive is a gain of -24 to +24 dB into a curve that is scaled to the Ceiling, with no make-up
/// (the Eroder's Amount is an exponential gain with its level held); in the two slews it is the rate (`aimSlew`), in the four Paunch modes the linear amount `4 (dB + 24) / 48` (`paunchA`).
/// Clip runs the curve oversampled when Oversample is on, clamps every Eroder mode to the Ceiling, and has its own Soft, Texture, Grit, Fold and Bits. Where an Eroder row is true of Clip as it
/// stands, the row below points at it (`eroder::kDriveTips[...]`) instead of repeating it; the Shape rows of Tube, Jag, Lean, Slew and Lopside are those, because Clip calls the same
/// `shapeDrive` and `deriveDrive` with the same Shape. The rest are Clip's own.
///
/// `shape` is null where the Shape knob is not shown (the modes that read no Shape, and the four Paunch modes, whose knob is their own Angle).
struct ClipTip {
	const char* type;
	const char* amount;
	const char* shape;
};

namespace tips_detail {
constexpr const char* kCurveAmount =
    "Gain into the curve, from -24 dB to +24 dB. The Ceiling is the top it is scaled to, and nothing makes up the level: the harder it is pushed, the more of the wave reaches the top.";
constexpr const char* kSlewAmount =
    "The limiter's rate rather than a gain: 20 kHz at -24 dB down to 50 Hz at +24 dB, as the frequency of a triangle wave at half the Ceiling. The lower it is, the slower the signal may move.";
} // namespace tips_detail

inline constexpr ClipTip kModeTips[] = {
	// Soft
	{ "A soft shoulder: the signal is untouched up to the Knee's start, then a tanh bends it into the Ceiling. At 0% Knee it is a hard clamp.",
	  tips_detail::kCurveAmount, nullptr },
	// Texture
	{ "The soft shoulder, then the detail clipping removed (above Freq) is carved back out of the top by Foldback. It only ever takes from the top, so the Ceiling still holds.",
	  tips_detail::kCurveAmount, nullptr },
	// Grit
	{ "The soft shoulder, then noise is carved out of the top, deeper the further the signal goes past the Knee's start. Nothing below it is touched.",
	  tips_detail::kCurveAmount, nullptr },
	// Fold
	{ "What goes past the Ceiling folds back as a triangle instead of flattening, as far as Depth says (0% is a clamp). Below the Ceiling it is untouched.",
	  "Gain into the folder, from -24 dB to +24 dB. The more of the wave that goes past the Ceiling, the more times it folds back.", nullptr },
	// Bits
	{ "The part of the wave above the Knee's start is cut into one or two bits' worth of steps, a stepped top. At 0% Knee there is no such part and it is a hard clamp.",
	  tips_detail::kCurveAmount, nullptr },
	// Tube: the Eroder's row is true (same curve, same Hard; Clip's DC Block switch is the DC block the text names)
	{ eroder::kDriveTips[static_cast<size_t>(eroder::DriveMode::Tube)].type, tips_detail::kCurveAmount, eroder::kDriveTips[static_cast<size_t>(eroder::DriveMode::Tube)].shape },
	// Jag
	{ "A tanh into a triangle folder in series: past the folder's first peak the wave folds back. Bias adds even harmonics, and the offset it makes is taken out below 10 Hz.",
	  tips_detail::kCurveAmount, eroder::kDriveTips[static_cast<size_t>(eroder::DriveMode::Jag)].shape },
	// Lean (w): the Eroder's row is true (the same curve on both sides)
	{ eroder::kDriveTips[static_cast<size_t>(eroder::DriveMode::Lean)].type, tips_detail::kCurveAmount, eroder::kDriveTips[static_cast<size_t>(eroder::DriveMode::Lean)].shape },
	// Chonk (w)
	{ "A tanh on the left; on the right what the tanh added is turned over by Shape, so the odd harmonics come out in opposite polarity left and right and add stereo width. Bal keeps the two sides' levels alike, and the Ceiling holds the right side.",
	  tips_detail::kCurveAmount,
	  "How much of what the tanh adds is turned over on the right side only. At 0% both sides are the same tanh; at 50% the right side is the clean signal; at 100% what the tanh added to it is reversed, so the odd harmonics sit in opposite polarity left and right." },
	// Paunch-S (w)
	{ "Adds the rectified mid (left and right summed, flipped positive) to one side and takes it from the other: even harmonics that are stereo width, and at Angle 0% cancel in mono. The dry passes at unity; the Ceiling clips the sum, and DC Block takes out the offset.",
	  "How much of the rectified mid is added, in linear steps across the knob's travel: nothing at -24 dB, four times the mid's level at +24 dB. The dry is not touched and nothing makes up the level; the Ceiling holds the sum.",
	  nullptr },
	// Paunch-H (w)
	{ "Adds the squared mid to one side and takes it from the other: a pure octave above, as stereo width that at Angle 0% cancels in mono. The dry passes at unity; the Ceiling clips the sum, and DC Block takes out the offset.",
	  "How much of the squared mid is added across the knob's travel. The octave grows with the square of the amount: nothing at -24 dB, the most at +24 dB. The dry is not touched; the Ceiling holds the sum.",
	  nullptr },
	// Paunch-OS (w)
	{ "Adds a pure third harmonic of the mid, sized to the signal's own level, to one side and takes it from the other: odd harmonics that are stereo width. It makes no offset, and the Ceiling clips the sum.",
	  "The size of the third harmonic, as a share of the signal's own level: it grows in step with the knob's travel, from nothing at -24 dB to the fundamental's own level at +24 dB, at any signal level. The dry is not touched; the Ceiling holds the sum.",
	  nullptr },
	// Paunch-OH (w)
	{ "Adds the plain cube of the mid, with its fundamental cancelled, to one side and takes it from the other: odd harmonics that are stereo width. It grows with level, so quiet passages stay clean and loud ones spread; the Ceiling clips the sum.",
	  "How much of the cubed mid is added: the third harmonic grows with the cube of both the knob's travel and the signal's level, so it is the knob and a loud passage together that bring it up. The dry is not touched; the Ceiling holds the sum.",
	  nullptr },
	// Slew
	{ "A slew limiter, which lets the signal move only so fast; what comes out is held to the Ceiling. It runs oversampled when Oversample is on.",
	  tips_detail::kSlewAmount, eroder::kDriveTips[static_cast<size_t>(eroder::DriveMode::Slew)].shape },
	// Lopside
	{ "A slew limiter at its hard limit that lets the signal rise and fall at different speeds; what comes out is held to the Ceiling. It runs oversampled when Oversample is on.",
	  tips_detail::kSlewAmount, eroder::kDriveTips[static_cast<size_t>(eroder::DriveMode::Lopside)].shape },
};
static_assert(sizeof kModeTips / sizeof kModeTips[0] == static_cast<size_t>(Mode::kCount), "a mode needs its tooltip row");

[[nodiscard]] inline const ClipTip& modeTip(int mode) noexcept {
	return kModeTips[static_cast<size_t>(mode) < static_cast<size_t>(Mode::kCount) ? static_cast<size_t>(mode) : 0];
}

/// THE ROTATION RATE'S TEXT FOR THE PAUNCH MODES (Lean's is `eroder::kLeanRateTip`, true of Clip: the same `leanRotated` turns by the same phase). The declared one names both.
inline constexpr const char* kPaunchRateTip =
    "How fast the angle of the addition turns, so no side leans on average. At the bottom it is off and Angle holds still.";

} // namespace feathers::fx::clip
