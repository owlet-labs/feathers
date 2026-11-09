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

#include "plugin/factory_presets.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// THE USER'S PRESETS, ON DISK — a FOLDER TREE a person can see (backlog FO, phase 2).
//
// WHERE: **~/Documents/Owlet/Feathers/Presets**, with Feathers' own presets and
// `Devices/<Device>/` side by side, and category subfolders under each:
//
//     ~/Documents/Owlet/Feathers/Presets/
//         Feathers/                  <- the plugin's whole-state presets (bank `kPluginBank`)
//             Init.fpreset
//             Pads/Glass.fpreset     <- a category is a folder
//         Devices/
//             SKLITTER/              <- one device's presets (`devicePresetBank`)
//                 Init.fpreset
//                 Stutters/Tape Chop.fpreset
//
// A FIXED PATH, NOT XDG DATA, and that is the point of the move: the old `$XDG_DATA_HOME/feathers` is
// redirected into a Flatpak's own sandbox (`~/.var/app/<id>/data`) and so named a different folder in
// Flatpak Bitwig than in any other host, and it is a folder nobody browses to. Documents is where a
// person looks, and Bitwig's sandbox has the host filesystem. `FEATHERS_PRESET_ROOT` overrides it —
// for the tests, which must never write a developer's real library, and for anybody who wants it
// elsewhere. NO MIGRATION from the old folder: a clean break, by decision — there was nothing in it to keep.
//
// **AND A PERSON MAY CHOOSE ANOTHER**: "Choose preset folder…" in any browser's Manage submenu makes the chosen folder
// the root for the whole process (`setChosenRoot`), kept in the editor's prefs (`presets.root`) so the
// next editor opens on it. The layout under it is the same — `Feathers/`, `Devices/<Device>/` — and the
// factory presets are copied into it when missing, as they are into the default one.
//
// WHY NOT editor_prefs, WHY NOT PATCH STATE: a preset is nothing BUT sound (editor_prefs forbids that),
// and it is a LIBRARY rather than a property of one song.
//
// ONE FILE PER PRESET, named for the preset, so the library is something a person can look at, copy,
// back up and send. Its bytes are a small METADATA block and then the payload, exactly what the owner's
// provider captured (the plugin's state, or a device preset — `device_preset.h`):
//
//     "FPRE"  u8 version  u32 metaBytes (LE)  <meta: one JSON object>  <payload>
//
// THE METADATA IS JSON ON PURPOSE (backlog JF): `{"saved": <unix seconds>, "tags": ["dark", "bass"]}`.
// JF makes every preset one JSON document; this object is what goes in its `"meta"` member verbatim,
// so tags and the save time survive that cutover without a second design. A reader ignores keys it
// does not know. A file WITHOUT the magic is read as a bare payload with no metadata.
//
// **THE CATEGORY TRAVELS IN THE METADATA TOO**, as the reserved tag namespace
// `category`: a preset in `Pads/Glass/` carries `"category:Pads/Glass"` in its tags, so a file sent to
// somebody, or exported, still knows where it belongs — and Import files it there (creating the folder).
// LOCALLY THE FOLDER IS THE TRUTH: a file moved by hand in a file manager is listed under the folder it
// is IN, whatever its metadata says, and the metadata is rewritten to match on its next save (every
// `save` writes the folder it saves into). The metadata is only consulted where there is no folder —
// a file arriving from outside the library.
//
// WHY THE SAVE TIME IS IN THE FILE AND NOT THE MTIME: copying a library, unzipping one, or a sync tool
// all rewrite mtimes, and "most recently used tag" would then mean "most recently copied". The
// metadata's `saved` is set when WE save and survives every copy.
//
// A MISSING, UNREADABLE OR CORRUPT FILE IS NOT AN ERROR: it means that preset is not there.
//
// **THE FACTORY PRESETS SHIP AS FILES IN THE REPO**. `presets/factory/` mirrors the root above; every `.fpreset`
// in it is embedded into BOTH plugins at build time (`cmake/EmbedPresets.cmake` → `factory_presets.h`)
// and written into the root when a browser reads its bank (`installFactoryFiles`), under the same
// copy-when-missing rule as everything else: a copy the user has edited, or deleted and replaced, is
// never touched.
//
// **TO ADD A FACTORY PRESET: drop its file into `presets/factory/<path>` and rebuild.** The path is where
// it installs — `Feathers/Pads/Glass.fpreset` for the synth, `Devices/<TAG>/<Category>/<Name>.fpreset`
// for a device, with `<TAG>` spelled as `devicePresetBank` spells it (the tag, sanitised). The file is
// shipped byte for byte, so save it from the plugin: its tags and category travel in its metadata.
// A new file re-runs CMake by itself (the glob is CONFIGURE_DEPENDS). Hidden files are skipped.
//
// A bank's "Init" is computed when the editor opens (the state's defaults) UNLESS the folder holds a
// `<bank>/Init.fpreset`, which is installed first and so wins. The synth's does (the tagged `init`,
// 2026-09-27); the devices' are computed. `scripts/collect-factory-presets.sh` fills the folder from the
// presets tagged `factory`, and `scripts/hooks/pre-push` refuses a push while it is out of date.

namespace feathers {

/// **WHICH BANK — A FOLDER UNDER THE ROOT.** Spelled by code, never by a person: the NAME and CATEGORY
/// are the parts a text field reaches, so those are sanitised and the bank is trusted.
using PresetBank = std::string;
inline constexpr const char* kPluginBank = "Feathers";
/// The reserved tag namespace the category travels under in a file's metadata.
inline constexpr const char* kCategoryNamespace = "category";

/// What a preset carries besides its sound. See the file comment for the encoding and for JF.
struct PresetMeta {
	std::vector<std::string> tags; ///< never a `category:` tag — that one is `category`, below
	int64_t saved = 0; ///< unix seconds, when WE last wrote it; 0 unknown
	/// **THE CATEGORY AS THE FILE CARRIES IT** — on disk, the tag `category:Pads/Glass` in `tags`; here,
	/// pulled out into its own field by `parseMetaJson` and put back by `metaJson`. `save` writes the
	/// folder it saves into; `loadAll` sets it to the folder the file was FOUND in (the folder wins).
	std::string category {};
	friend bool operator==(const PresetMeta&, const PresetMeta&) = default;
};

/// One preset as it sits on disk.
struct StoredPreset {
	std::string name;     ///< the file's stem
	std::string category; ///< the folders under the bank, '/'-separated; empty at the top
	PresetMeta meta;
	std::vector<uint8_t> bytes; ///< the PAYLOAD — what a provider applies
	std::filesystem::path path;
};

class PresetStore {
public:
	/// **THE ROOT, IN THIS ORDER:** `$FEATHERS_PRESET_ROOT` (the tests, and anybody who pins it); else the
	/// folder a person CHOSE ("Choose preset folder…", `setChosenRoot`); else `defaultRoot()`. Empty with
	/// none of them: presets then live in memory for the session and nothing persists.
	static std::filesystem::path root();
	/// `$HOME/Documents/Owlet/Feathers/Presets` — what "Reset preset folder" returns to. Empty without a home.
	static std::filesystem::path defaultRoot();
	/// **ONE CHOSEN ROOT PER PROCESS**, so every browser — the header's, every device panel's, in every
	/// editor this process has open — reads the same library. Set by the editor from its prefs at startup
	/// and by the browser's verb; this is only the value (`PresetBrowser::setPresetRoot` also re-lists).
	/// Empty: the default. MESSAGE THREAD by use; guarded anyway, being process-wide.
	static void setChosenRoot(const std::filesystem::path& chosen);
	[[nodiscard]] static std::filesystem::path chosenRoot();
	/// `$FEATHERS_PRESET_ROOT` is set, so a chosen root changes nothing — the verbs that choose one say so
	/// by being disabled.
	[[nodiscard]] static bool rootOverridden();
	/// The bank's folder. Empty when there is no root.
	static std::filesystem::path directory(const PresetBank& bank = kPluginBank);

	/// **ONE NAME REDUCED TO WHAT A FILENAME MAY HOLD** — letters, digits, space, dash, underscore;
	/// everything else a dash. "../../.bashrc" is "-------bashrc": a name is not a path. Empty when
	/// nothing usable is left.
	static std::string sanitise(const std::string& name);
	/// **A CATEGORY, SEGMENT BY SEGMENT** — each folder name sanitised as a name is, empty segments
	/// dropped, so no spelling reaches outside the bank.
	static std::string sanitiseCategory(const std::string& category);
	/// **"Pads/Glass" IS A NAME IN A CATEGORY** — what a person types into the save field. The last
	/// segment is the name, the rest the category, both sanitised.
	static std::pair<std::string, std::string> splitTyped(const std::string& typed);

	/// The file one preset lives in. Empty with no root or an unusable name.
	static std::filesystem::path pathFor(const std::string& name, const PresetBank& bank = kPluginBank,
	                                     const std::string& category = {});

	/// Every preset in the bank, every category, sorted by category then name. Hidden files and folders
	/// (a leading dot) are skipped. Silent about anything it could not read.
	static std::vector<StoredPreset> loadAll(const PresetBank& bank = kPluginBank);

	/// Write one preset (metadata + payload), creating folders. False when it could not be written.
	static bool save(const std::string& name, const std::vector<uint8_t>& payload, const PresetBank& bank = kPluginBank,
	                 const std::string& category = {}, const PresetMeta& meta = {});
	/// **WRITE ONLY IF NOTHING IS THERE** — how factory presets are installed: copied when missing, and a
	/// copy the user has edited is never touched. True when it wrote.
	static bool installIfMissing(const std::string& name, const std::vector<uint8_t>& payload, const PresetBank& bank,
	                             const std::string& category = {}, const PresetMeta& meta = {});
	/// **THE EMBEDDED FACTORY FILES FOR ONE BANK, INSTALLED WHEN MISSING** — every file whose path starts
	/// `<bank>/` is written verbatim to `root()/<path>` unless something is already there. A path with an
	/// empty, `.` or `..` segment is refused. Returns how many it wrote. `files` defaults to the set the
	/// plugin carries; the tests pass their own.
	static int installFactoryFiles(const PresetBank& bank,
	                               std::span<const FactoryPresetFile> files = factoryPresetSource());
	/// Move a preset file. Refused (false) when `to` exists — a rename never overwrites.
	static bool rename(const std::filesystem::path& from, const std::filesystem::path& to);
	/// Remove one file. False if it was not there or could not be removed.
	static bool erase(const std::filesystem::path& file);
	static bool erase(const std::string& name, const PresetBank& bank = kPluginBank, const std::string& category = {});

	/// The file bytes for `meta` + `payload`, and back. `decode` of bytes without the magic yields them as
	/// the payload with empty metadata; false only for a header that lies about its own length. Either
	/// out-pointer may be null, and that half is then not written.
	static std::vector<uint8_t> encode(const PresetMeta& meta, const std::vector<uint8_t>& payload);
	static bool decode(const std::vector<uint8_t>& file, PresetMeta* meta, std::vector<uint8_t>* payload);
	/// The metadata as the JSON object the file (and JF's `"meta"`) holds, and back.
	static std::string metaJson(const PresetMeta& meta);
	static bool parseMetaJson(const std::string& json, PresetMeta* meta);
};

} // namespace feathers
