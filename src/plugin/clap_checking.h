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

// THE CLAP-HELPERS CHECKING LEVEL BOTH PLUGINS ARE BUILT WITH, in one place, so a test can ask which one it is
// testing. Debug builds terminate on a misbehaving host and check everything; release builds check the cheap
// things and carry on. This is the clap-helpers convention and the one the SST plugins use: the Maximal checks
// are assertions about host behaviour that are worth crashing a developer's session over and worth surviving in
// a musician's.
//
// WHY A TEST CARES: at Maximal, clap-helpers' own validation of every value / value_to_text / text_to_value call
// walks every param's info (`getParamInfoForParamId`), so how many `clap_param_info` a call builds is a property
// of the checking level, not only of our code. The sanitizer gate builds without NDEBUG, so it runs Maximal.

#include <clap/helpers/checking-level.hh>
#include <clap/helpers/misbehaviour-handler.hh>

namespace feathers {

#if defined(NDEBUG)
inline constexpr clap::helpers::MisbehaviourHandler kClapMisbehaviour = clap::helpers::MisbehaviourHandler::Ignore;
inline constexpr clap::helpers::CheckingLevel kClapChecking = clap::helpers::CheckingLevel::Minimal;
#else
inline constexpr clap::helpers::MisbehaviourHandler kClapMisbehaviour = clap::helpers::MisbehaviourHandler::Terminate;
inline constexpr clap::helpers::CheckingLevel kClapChecking = clap::helpers::CheckingLevel::Maximal;
#endif

} // namespace feathers
