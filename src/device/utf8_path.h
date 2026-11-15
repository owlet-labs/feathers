/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <filesystem>
#include <string>
#include <string_view>

// **UTF-8 AT THE EDGE OF THE FILE SYSTEM.** Every string this program stores or shows is UTF-8: preset names, IR paths in a
// patch, the folder a user chose. On Windows a narrow `std::filesystem::path` is read in the ANSI code page, so a UTF-8
// name builds a path that names another file (or none), and `path::string()` on a name the code page cannot hold THROWS
// ("No mapping for the Unicode character exists in the target multi-byte code page"). These two functions are the only
// crossing: text goes in through `pathFromUtf8`, and comes out through `utf8Of`. Elsewhere they are the identity on bytes.
namespace feathers {

[[nodiscard]] inline std::filesystem::path pathFromUtf8(std::string_view text) {
	return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

[[nodiscard]] inline std::string utf8Of(const std::filesystem::path& path) {
	const std::u8string text = path.u8string();
	return std::string(text.begin(), text.end());
}

} // namespace feathers
