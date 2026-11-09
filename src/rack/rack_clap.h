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

// THE RACK AS A CLAP PLUGIN. Two functions, so the entry point can be a file of its own the way
// `feathers_entry.cpp` is — every format links the same implementation and exports its own symbol.

namespace feathers::rack {

const clap_plugin_descriptor* descriptor() noexcept;
const clap_plugin* create(const clap_host* host) noexcept;
/// THE NOTE RACK (2026-11-04): the same engine and editor as a CLAP note effect — note in, note out, no audio, and a
/// notes-and-modulators offer. The bundle's second plugin.
const clap_plugin_descriptor* noteRackDescriptor() noexcept;
const clap_plugin* createNoteRack(const clap_host* host) noexcept;

} // namespace feathers::rack
