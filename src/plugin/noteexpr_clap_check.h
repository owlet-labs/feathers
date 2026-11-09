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

#include "mod/arp/pattern_types.h"
#include "mod/noteexpr/noteexpr_types.h"

#include <clap/events.h>

namespace feathers {

/// **THE NOTE-EXPRESSION DEVICE'S OUTPUTS ARE CLAP'S EXPRESSIONS — checked where both are visible.**
///
/// `mod/noteexpr/` is in the portable layer and does not include `clap/events.h`; it must not, or
/// every device after it would. This header sees both worlds, so this is where the two numbers are
/// made to agree. The device's output INDEX is the CLAP expression id — that is the whole reason it
/// needs seven and needs them in CLAP's order — so a mismatch here is a device publishing pressure on
/// the lane a patch has routed brightness from. (It lived in device_registry.h until backlog NY.1;
/// the device's manifest now includes it, so the registry names no device.)
static_assert(mod::noteexpr::kOutputCount == CLAP_NOTE_EXPRESSION_PRESSURE + 1,
              "the note-expression device declares one output per CLAP expression, in CLAP's order; "
              "if clap/events.h has grown an eighth, the device needs an eighth output");

/// **AND THE PATTERN DEVICE'S TB-GLIDE BENDS BY CLAP'S TUNING EXPRESSION**, whose id its portable header restates.
static_assert(mod::arp::pat::kTuningExpression == CLAP_NOTE_EXPRESSION_TUNING,
              "TB-Glide's bend is a CLAP TUNING expression (terrain-arp design §11.3)");

} // namespace feathers
