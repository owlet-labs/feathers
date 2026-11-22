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

// FEATHERS_PINNED_PLATFORM: the platform a bit-exact golden was taken on (Linux x86-64). A test that asserts a
// golden hash or a bit-identical render guards the assertion with `#if FEATHERS_PINNED_PLATFORM`, because libm and
// code generation differ elsewhere (macOS arm64); the value tests beside it hold the behaviour everywhere.
//
// WHY A MACRO AND NOT `defined(__linux__) && defined(__x86_64__)` WRITTEN OUT: scripts/clang-preflight.sh compiles
// the tests a second time with -DFEATHERS_FORCE_UNPINNED, which makes this 0 on Linux, so the OTHER branch (the one
// macOS CI compiles) is checked under -Werror here: a helper or counter used only inside the pinned branch shows up
// as unused. `-U__linux__` cannot do that, since the system headers need it. The preflight also refuses the
// written-out form in tests/.
#if !defined(FEATHERS_FORCE_UNPINNED) && defined(__linux__) && defined(__x86_64__)
#define FEATHERS_PINNED_PLATFORM 1
#else
#define FEATHERS_PINNED_PLATFORM 0
#endif
