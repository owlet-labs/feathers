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

#include "fx/phaser/phaser_types.h"

namespace feathers::fx::phaser {

/// **WHAT A PHASER CONTROL DOES IN THE MODE AND KEY TRACK IT IS ON** (backlog TV, the audit's Phaser sites). The declared
/// tooltips stay the general text; the card and the window put these over them through `DeviceControl::setTooltipOverride`,
/// refreshed from `applyGates` every tick, so a click, automation, a preset or undo all show the text of the mode they land in.
/// `nullptr` keeps the declared text.
///
/// Center and Delay are the offset from the note only while Key Track is on (`phaserNoteTracked` and `keyCornerLog2Hz`);
/// Mix is the classic phaser's 50% only in Phaser (the flanger's comb has no such point).
inline const char* phaserTip(Param p, Mode mode, bool keyTrack) {
	switch (p) {
	case Param::CenterHz:
		return keyTrack ? "Key Track is on: where the notches sit, as an offset in semitones from the note it tracks. The "
		                  "default puts the first notch on the note"
		                : "Where the notches sit with the sweep at rest. Key Track turns this into an offset from the "
		                  "played note";
	case Param::DelayMs:
		return keyTrack ? "Key Track is on: the flanger's delay, as an offset in semitones from the note it tracks. The "
		                  "default tunes the comb to the note"
		                : "The flanger's delay with the sweep at rest: shorter puts the comb's teeth further apart. Key "
		                  "Track turns this into an offset from the played note";
	case Param::Mix:
		return mode == Mode::Flanger
		           ? "Dry against wet: how much of the swept comb is heard against the untouched input. 50% is an even "
		             "blend; 100% is wet only"
		           : nullptr;
	default:
		return nullptr;
	}
}

/// The prefix an EXPLORE pill's amount takes while its group is dimmed (`applyGates`): what stops it being heard, and how
/// to hear it. Empty when the pill is live, so the declared text stands.
inline const char* dimmedPillPrefix(phase_space::Group g) {
	switch (g) {
	case phase_space::Group::Colour:
		return "Not heard while Damping is open, so the feedback has no damping corner to move. Lower Damping to hear it. ";
	case phase_space::Group::Bias:
		return "Not heard at Drive 0, where the feedback saturator is symmetric. Raise Drive to hear it. ";
	default:
		return "";
	}
}

} // namespace feathers::fx::phaser
