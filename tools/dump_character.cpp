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

// Regenerates tests/golden/character.txt. Run only when a submodule bump is a deliberate,
// reviewed change to the character layer:
//   build/dump_character > tests/golden/character.txt

#include "character_dump.h"
#include <cstdio>

int main() {
	for (const std::string& line : feathers::character_dump::collectLines()) {
		std::printf("%s\n", line.c_str());
	}
	return 0;
}
