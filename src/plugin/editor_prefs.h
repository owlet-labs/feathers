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

#include <filesystem>
#include <map>
#include <string>
#include <string_view>

// HOW THE EDITOR IS ARRANGED, remembered between runs — and NOTHING ELSE.
//
// WHAT BELONGS HERE is a fact about somebody's desk: which rails they keep closed, which cards they
// keep folded. It is the state the editor already keeps in memory across an open and close (the
// editor object outlives its window), promoted to outliving the process too, because persisting state
// was asked for.
//
// WHAT DOES NOT BELONG HERE is anything the sound depends on. None of this may ever reach patch
// state: a preset that carried a collapsed rail would rearrange the editor of whoever loaded it, and
// two people sharing a patch do not share a monitor. That is a rule about direction as much as
// storage — nothing in this file is ever read by the engine, and nothing the engine knows is ever
// written to it.
//
// WHERE IT LIVES is the XDG base directory: $XDG_CONFIG_HOME/feathers/editor.conf, falling back to
// ~/.config/feathers/editor.conf. Not beside the .clap, which is a build artefact that gets deleted
// and reinstalled, and not in the plugin's own directory, which may not be writable at all.
//
// Honouring XDG_CONFIG_HOME is also what makes this testable without touching the developer's real
// desk: the test binary points that variable at a temporary directory before any case runs. A store
// that could only ever write to one absolute path would have made every case that opens an editor a
// case that edits the machine it runs on.
//
// THE FORMAT IS FLAT `key=value`, one per line, `#` starts a comment. Hand-readable and
// hand-editable on purpose — this is a preferences file, and the first thing anybody does with a
// broken one is open it. Unknown keys are kept and written back untouched, so a file written by a
// newer build survives a run of an older one instead of being silently trimmed.
//
// A MISSING OR BROKEN FILE IS NOT AN ERROR. It means "no preferences", the editor opens the way it
// always did, and nothing is said to anybody: preferences are never worth interrupting a musician
// for. There is no state in which this file's contents can stop the editor opening.

namespace feathers {

class EditorPrefs {
public:
	/// Where the file is, per XDG. Empty when there is no home to speak of, which is the one case
	/// where preferences simply do not persist.
	static std::filesystem::path path();

	/// Read what is there, if anything. Silent about everything: a missing file, an unreadable one and
	/// a corrupt one are the same answer — no preferences.
	void load();

	/// What was stored under this key, or `fallback` when it was not stored, was not a flag, or the
	/// file was never there. The fallback is the caller's default, so a key nobody has ever written
	/// behaves exactly as the editor did before it existed.
	bool flag(std::string_view key, bool fallback = false) const;

	/// Remember this, now. Writes the whole file ATOMICALLY — a temporary beside it, then a rename —
	/// so a crash or a pulled plug leaves either the old file or the new one, never half of either.
	/// Writing on every change rather than at shutdown is deliberate: destroy() is not guaranteed to
	/// run, and a preference that survives only a polite exit is not a preference that survives.
	void setFlag(std::string_view key, bool value);
	/// A NUMBER, for the one preference that is not a switch: the UI scale (backlog EJ). Unparsable or
	/// non-finite text answers the fallback, the same posture `flag` takes toward a word it does not know.
	double number(std::string_view key, double fallback) const;
	void setNumber(std::string_view key, double value);
	/// **TEXT, for the one preference that is a path: the preset folder.** Stored ESCAPED — `%`, `#`,
	/// space and every control byte as `%XX` — because a line is cut at `#`, trimmed of spaces at its
	/// ends and ended by a newline, and a folder may hold any of those. Everything else is written as is,
	/// so the file still reads. An EMPTY value removes the key: the default comes back, which is what an
	/// empty setting means.
	std::string text(std::string_view key, const std::string& fallback = {}) const;
	void setText(std::string_view key, const std::string& value);

	/// Everything currently held, for tests and for the sake of not having to read a file to know what
	/// was written.
	const std::map<std::string, std::string, std::less<>>& values() const noexcept { return values_; }

private:
	void save() const;

	std::map<std::string, std::string, std::less<>> values_;
	/// Whether load() has run. A store that has never looked at the disk must not overwrite it: the
	/// first setFlag would otherwise stamp a one-key file over everything else somebody had.
	bool loaded_ = false;
};

} // namespace feathers
