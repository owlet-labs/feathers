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

#include "dsp/constellation.h"
#include "dsp/slice_basis.h"

// EVERY HANDLE THE AUDITION HAS, AND NOT ONE OF THEM IS A PARAMETER.
//
// One decision covers all of these: they are not CLAP params, and they may stay in the UI to pin values
// experimentally. They are environment variables and debug sliders, they spend no id,
// and they go away when the question they exist to ask has been answered. Gathered into one struct
// rather than passed one by one because they travel together everywhere — the engine reads them once
// a block, the editor reads them to draw the same sound, and a signature that grows an argument per
// question is a signature every call site has to be edited for every question.
//
// WHAT DECIDES WHETHER SOMETHING BELONGS HERE: it changes the sound, it is being judged by ear, and
// nobody has committed to keeping it. A setting that survives the audition earns a parameter id and
// leaves; a setting that loses it leaves with nothing.

namespace feathers {

/// The steps the audition strip offers for either scale. 1 is the DEFAULT and the decided shipping
/// value — the field at its own magnitude, which is what "φ authors the variation" means. Off is the
/// reference to compare against; 2 is there because an effect judged too subtle is judged wrong for
/// the wrong reason.
inline constexpr double kBlueShiftScaleOff = 0.0;
inline constexpr double kBlueShiftScaleOn = 1.0;
inline constexpr double kBlueShiftScaleMore = 2.0;

/// The handles at their shipping values, which is what a test, a host without the environment set,
/// and the instrument itself all get. Two of them are the engine as it always was; the blue-shift
/// scales are live, and are the one place a default here changes what a moving patch sounds like.
struct AuditionTuning {
	/// Normally unpinned, in which case every sound's plane comes from its own home.
	SliceOrientationPin orientation{};

	/// How sharply the nearest sound dominates the blend — §7.1d's A/B.
	///
	/// Power 1 is the default and the presumptive winner, and it is also the only value that leaves a two-sound patch alone. The question
	/// the exponent really asks — how local is the support when there are SEVERAL sounds — does not
	/// exist below three of them, which is why this ships with the N-sound build and not before it.
	double falloffPower = kBlendFalloffPower;

	/// THE BLUE SHIFT'S TWO SCALES, and they are scales rather than depths on purpose.
	///
	/// §2b.1: *"The audition toggles become GLOBAL SCALES multiplying the fields, rather than flat
	/// depths. An ear tunes the magnitude; φ authors the spatial variation. That is the right division
	/// of labour and it is worth stating: a flat depth would have thrown away the reason for making it
	/// a field."*
	///
	/// BOTH DEFAULT TO 1 — blue shift is LIVE. Decided, and the deciding argument was §2c's: the uniform
	/// rule that dissolves the two-sound crossfade is gated on blue shift precisely so there is
	/// directional character to give back, and scales at zero would have made that step a net loss.
	///
	/// It costs no existing patch anything AT REST, and that is arithmetic rather than a promise: every
	/// term is multiplied by a velocity that is exactly zero when the puck is parked. A patch changes
	/// only while somebody is moving it, which is when they are asking it to.
	double blueOvershootScale = kBlueShiftScaleOn;
	double blueDopplerScale = kBlueShiftScaleOn;

	/// THE REFERENCE-PARITY REFERENCE, and it is not a user feature — §2c.
	///
	/// The verbatim v1 two-sound crossfade, reachable so the bit-identity tests keep running against it
	/// rather than bit-rotting, and so there is a way back if the character is ever redeployed to
	/// hardware. Environment and tests only: no CLAP parameter, and no control in the strip either,
	/// because a control is how something becomes a feature.
	///
	/// The real redeployment path is still the firmware submodule and the character goldens, which pin
	/// what the hardware actually does. This pins what OUR engine did before the rule became uniform.
	bool v1Crossfade = false;

	/// Whether anything here can change a sound. False for the shipping default, which is what lets
	/// the blend take its old path exactly rather than a path that agrees with it to fifteen places.
	bool blueShiftActive() const { return blueOvershootScale != 0.0 || blueDopplerScale != 0.0; }

	friend bool operator==(const AuditionTuning&, const AuditionTuning&) = default;
};



/// The A side and the B side of §7.1d, named so that the toggle and the tests cannot disagree about
/// which two values are being compared.
inline constexpr double kFalloffPowerA = kBlendFalloffPower;
inline constexpr double kFalloffPowerB = 2.0;

} // namespace feathers
