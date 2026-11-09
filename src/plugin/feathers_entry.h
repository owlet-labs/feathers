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

#include "clap/clap.h"

// The three functions a clap_plugin_entry is made of, with external linkage so the entry structure
// can be built in a separate translation unit. That separation is what clap-wrapper's "clap first"
// flow needs: the plugin is a static library, and each format (CLAP, VST3, standalone) recompiles a
// tiny entry file of its own against it. See feathers_entry.cpp and the make_clapfirst_plugins call
// in the root CMakeLists.
namespace feathers {

bool CLAP_ABI entryInit(const char* pluginPath);
void CLAP_ABI entryDeinit();
const void* CLAP_ABI entryGetFactory(const char* factoryId);

} // namespace feathers
