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

#include <cstddef>

// FEATHERS RACK: THE PARALLEL RACK AS A PLUGIN OF ITS OWN (a fourth mode of the rack plugin, rack_clap.cpp). The root is ONE Rack
// device, in the mode a new one is made in, and the window is that device's own view, so every mode (Parallel, Select, Blend,
// Mid/Side, Left/Right, Bands) is chosen on the plugin's face, and its chains are edited there.
//
// THE HOST'S PARAMETERS ARE THE RACK'S OWN CONTROLS, NAMED, THEN THE POOL. They ride the host slot machinery the plugin already
// has: the controls below are pinned at construction in THE ORDER BELOW, so a control's CLAP id is `hostslots::kFirstSlotId`
// plus its place in this list, and every slot after them is the ordinary pool, taken by the first touch of a control inside a
// chain (least recently used recycled; a pin never is). THE LIST IS APPEND-ONLY: a host keeps its automation lanes on an id
// for ever, so a control that joins the rack takes the next place and nothing is reordered or removed (`test_rack_plugin`).
//
// MODE, UNUSED, BANDS, SLOPE AND FILTERING ARE NOT ON THE LIST, and are not host parameters at all: they are structural (the rack
// fades its sum out and back across a change), and the device declares that no lane and no modulator may move them
// (`ParamDesc::hostSlot` false). They are reached from the plugin's own window.

namespace feathers::rack::rackplugin {

inline constexpr const char* kPluginId = "com.owlet.feathers.rack";
inline constexpr const char* kPluginName = "Feathers Rack";

inline constexpr const char* kHostKeys[] = {
	"rack_enabled", "rack_mix", "rack_gain",
	"macro_1", "macro_2", "macro_3", "macro_4",
	"macro_5", "macro_6", "macro_7", "macro_8",
	"rack_position", "rack_cut_1", "rack_cut_2", "rack_cut_3",
	"rack_cut_4", "rack_key_track",
};
inline constexpr size_t kHostParamCount = sizeof(kHostKeys) / sizeof(kHostKeys[0]);

} // namespace feathers::rack::rackplugin
