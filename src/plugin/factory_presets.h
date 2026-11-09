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

#include <cstdint>
#include <span>
#include <string_view>

// THE FACTORY PRESETS THE PLUGIN CARRIES — every `.fpreset` under the repo's `presets/factory/`,
// embedded at build time by `cmake/EmbedPresets.cmake`. The folder mirrors the preset root
// (`preset_store.h`), so a file's path IS where it installs: `Feathers/Pads/Glass.fpreset`,
// `Devices/SKLITTER/Stutters/Tape Chop.fpreset`. The bytes are the file verbatim, metadata and all.

namespace feathers {

struct FactoryPresetFile {
	std::string_view path; ///< relative to the preset root, '/'-separated
	std::span<const uint8_t> bytes;
};

/// Every embedded factory preset, sorted by path. Generated; empty when the folder holds none.
std::span<const FactoryPresetFile> factoryPresetFiles();

/// THE SET THE PLUGIN INSTALLS FROM: `factoryPresetFiles` in the plugin. The widget test binaries point it
/// at an empty set (tests/gui_widgets/main.cpp), so their preset counts and tag lists do not move every
/// time a preset is tagged `factory`; the factory cases pass their own set explicitly.
inline std::span<const FactoryPresetFile> (*factoryPresetSource)() = &factoryPresetFiles;

} // namespace feathers
