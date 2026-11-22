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

#include <array>
#include <cstddef>

// FEATHERS DRUM: THE DRUM DEVICE AS A CLAP INSTRUMENT (a third mode of the rack plugin, rack_clap.cpp).
//
// THE HOST'S PARAMETERS ARE THE DEVICE'S CONTROLS, NAMED AND WITH IDS THAT NEVER MOVE. They ride the host slot machinery the
// rack already has (pinned slots: a host write, a hand's edit, undo, the outbox to the host's lanes, recording), pinned at
// construction in THE ORDER BELOW, so a control's CLAP id is `hostslots::kFirstSlotId` plus its place in this list. THE LIST
// IS APPEND-ONLY: a host keeps its automation lanes on an id for ever, so a control that joins the device takes the next
// place and nothing is ever reordered or removed (`test_drum_plugin` holds every control of the device to exactly one place).
// The device's own key is the stored name, never the enum's position, so the device's face may be reordered freely.

namespace feathers::rack::drumplugin {

inline constexpr const char* kPluginId = "com.owlet.feathers.drum";
inline constexpr const char* kPluginName = "Feathers Drum";

inline constexpr const char* kHostKeys[] = {
	"drum_enabled", "drum_kind", "drum_note", "drum_level",
	"drum_pan", "drum_tune", "drum_decay", "drum_f_tone",
	"drum_f_noise", "drum_drive", "drum_trigger", "drum_voices",
	"drum_release", "drum_bend", "drum_delay", "drum_hp",
	"drum_seed", "drum_zone", "drum_zone_offset", "drum_gamma",
	"drum_rotate", "drum_travel_x", "drum_travel_y", "drum_pitch_mode",
	"drum_f_pitch", "drum_f_length", "drum_f_sweep", "drum_f_snap",
	"drum_f_metal", "drum_sweep_time", "drum_drive_curve", "drum_drive_tone",
	"drum_velocity_noise", "drum_width", "drum_claps",
	"drum_spread", "drum_clap_q", "drum_clap_hp", "drum_wire_decay",
	"drum_click_decay", "drum_bell_start",
	"drum_bell_end", "drum_bell_time", "drum_bell2_hz", "drum_bell2_time",
	"drum_kick_hold", "drum_kick_dip", "drum_kick_dip_time", "drum_click_sweep",
	"drum_click_sweep_time", "drum_start_phase", "drum_keys",
	// THE VELOCITY ROW AND THE SNARE'S AMOUNTS, appended. (The velocity, wires and bells coordinates left the device before any host held
	// a lane on them, so their places were closed up rather than kept; from here a removed control keeps its place, inert.)
	"drum_vel_level", "drum_vel_tone", "drum_vel_snap", "drum_vel_tension",
	"drum_wire_buzz", "drum_wire_gate", "drum_body_gate", "drum_bell1_level",
	"drum_bell2_level",
	"drum_density", "drum_grain",
};
inline constexpr size_t kHostParamCount = sizeof(kHostKeys) / sizeof(kHostKeys[0]);

} // namespace feathers::rack::drumplugin
