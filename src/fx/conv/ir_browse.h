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

#include <string>
#include <vector>

namespace feathers::fx::conv {

/// **THE IMPULSE BROWSER'S ARITHMETIC** (the browser and its forward/back arrows): the IRs beside a file, in order, and a step through them. The drawer's arrows and its list both ask
/// here, so what the arrows walk and what the list shows cannot disagree. Reads the disk: message thread.
///
/// - **WHICH FILES:** those `loadIrFile` reads — `.wav`, `.aif`, `.aiff`, `.flac`, any case — in the file's own
///   folder, not below it.
/// - **WHICH ORDER:** by name, case-insensitively, digits compared as numbers ("Hall 2" before "Hall 10"), so the
///   arrows walk a folder the way a file browser lists it.
[[nodiscard]] std::vector<std::string> irFilesBeside(const std::string& path);

/// The file `step` places from `path` among `irFilesBeside(path)`, WRAPPING at either end; `path` itself when it
/// is the only one; empty when the folder cannot be read. A `path` no longer in its folder steps from where it
/// would sort.
[[nodiscard]] std::string steppedIr(const std::string& path, int step);

/// Whether a name is one the loader reads.
[[nodiscard]] bool isIrFileName(const std::string& name);

/// **THE FIXED FOLDER** (lets make a fixed one): where the browser looks while a generated room
/// runs — `~/Documents/Owlet/Feathers/IRs`, beside the presets (`%USERPROFILE%\\Documents\\Owlet\\Feathers\\IRs` on Windows),
/// or `FEATHERS_IR_ROOT` when set (the tests' scratch). Created on first use so there is somewhere
/// to put files; empty if there is no home to put it in.
[[nodiscard]] std::string defaultIrFolder(bool create);

/// **ONE ENTRY IN THE BROWSER**: a generated room (`generated` >= 0) or a file (`path`).
struct IrChoice {
	int generated = -1;
	std::string path;
	bool operator==(const IrChoice&) const = default;
};
/// **WHAT THE BROWSER WALKS**: the generated rooms first, then the IRs in `folder` in natural order.
[[nodiscard]] std::vector<IrChoice> irChoices(int generatedRooms, const std::string& folder);
/// The entry `step` places from `here` among `choices`, wrapping; `here` itself if it is not listed.
[[nodiscard]] IrChoice steppedChoice(const std::vector<IrChoice>& choices, const IrChoice& here, int step);

/// **CHOOSING A FOLDER** (add a \choose folder\" item to the browser"*). There is no native folder
/// dialog here (visage has none, and a portal would mean a D-Bus client in a statically linked plugin), so the folder
/// is chosen from a menu that walks the disk one level at a time: this folder's own entry, its parent, its
/// subfolders, and two places to jump to. Reads the disk: message thread.
///
/// The IRs in one folder, natural order.
[[nodiscard]] std::vector<std::string> irFilesIn(const std::string& folder);
/// The folders inside one, natural order, hidden ones (a leading dot) left out.
[[nodiscard]] std::vector<std::string> subfoldersIn(const std::string& folder);

struct FolderRow {
	enum class Kind {
		Header, ///< where the menu is, not a choice
		Use,    ///< browse this folder (disabled when it holds no IRs)
		Enter,  ///< open the menu again at `path`
		Break,  ///< a line between groups
	};
	Kind kind = Kind::Header;
	std::string label;
	std::string path;
	bool enabled = true;
	bool operator==(const FolderRow&) const = default;
};
/// THE MENU AT `folder`: a header naming it; "Use" with its IR count; the parent; each subfolder; then the fixed IR
/// folder and the home folder (`irRoot`, `home`; either left out when empty or when it is `folder` itself).
[[nodiscard]] std::vector<FolderRow> folderRows(const std::string& folder, const std::string& irRoot, const std::string& home);

} // namespace feathers::fx::conv
