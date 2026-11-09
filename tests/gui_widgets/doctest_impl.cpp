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

// DOCTEST'S IMPLEMENTATION, COMPILED ONCE FOR ALL SIX WIDGET BINARIES. Until 2026-09-03 each
// binary's main.cpp carried DOCTEST_CONFIG_IMPLEMENT, so the framework's ~7,000 lines were compiled
// six times per build. MEASURED SERIALLY, one object at a time with ccache disabled, on a quiet
// host: under ASan this file's main.cpp cost 10.6 and 11.8 s WITH the implementation and 0.92 and
// 0.94 s without it, and the single TU here costs 12.0 s — so the six binaries were paying about
// 63 s of compile for one framework, and now pay 12. (An earlier note in scripts/sanitize-gate.sh
// said 67–93 s per binary; that came from a PARALLEL ninja log and was contention, not cost. A
// per-object time read off a `-j16` build is not a measurement of that object.)
// Here it is one object in
// feathers-gui-widget-fixtures; every main() references doctest::Context, so the archive member is
// pulled into each link. main() itself stays a SOURCE of each executable (gui_widgets/main.cpp) —
// the implementation and the entry point are different things, and only the entry point needs to
// be where no linker search order can drop it. test_gui_windowed keeps its own implementation TU
// and links this library too: nothing this member defines is undefined in that link, so it is not
// pulled — a claim the link verifies (a pulled member would be a duplicate-definition error).
#define DOCTEST_CONFIG_IMPLEMENT
#include "doctest.h"
