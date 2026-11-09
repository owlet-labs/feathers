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

// THE LIKE-SET LOG (backlog ON, 2026-10-28): what a listener likes and where they linger, appended as JSON lines while testing
// other things, so a hyperparameter sweep of the likes' warp can run offline over real sessions
// (tests/study_likes_replay.cpp replays it).
//
// DELTAS, NOT SETS (the lead, 2026-10-28: a whole set on every line grows as n², ~200 KB a line at 50 likes):
//   - `snapshot` — the patch's WHOLE like set (`likes`), written when the editor first draws and whenever the set changed
//     by any road the log did not describe (a patch load, the host's own undo of a state).
//   - `like` / `unlike` — ONE like added at, or removed from, `index` in the patch's set (`like`: its full point and every
//     site's angle as heard). `cause` says which edit made it: like, unlike, clear, undo, redo. A clear or an undo is
//     written as the deltas it amounts to (removals by descending index, then additions by ascending), so a replay of the
//     snapshot plus the deltas IS the live set, order included.
//   - `walk` — a stance (`like`: the place as a like would store it) the selected sound SETTLED at: written when the stance
//     differs from the last one logged AND it has been held still `kWalkDwellSeconds`, or a like is made there — so a drag writes nothing until it settles, and a still stance writes nothing more. `held` is how long it
//     had been still when written; `dwell` how long the PREVIOUS logged stance was held in all (absent on the first).
//     These are the walk's implicit negatives, and lingering is carried without repeating a line.
//
// EVERY LINE: {"v":2,"t":<unix s>,"session":"<pid>.<start>","event":…,"cause":…,"engine":"V5","engineId":2,"slot":0,
//   "scheme":"P-MOBIUS","hp":{"k0":3,"kMax":4,"knn":2,"iters":40,"lut":129,"widthFloor":0.005,"pinWidth":0.02},"strength":1,
//   then per event "index", "sound", "like":{x, y, home, engine, slot, sound, pointCount, point[], spectrum[48],
//   angles[385]}, "likes":[…], "held", "dwell"}. Doubles to 17 digits and floats to 9, so a replay refits exactly what the
//   editor fitted. The sound id is an FNV-1a of the engine, the sound and the full point.
//
// WHERE: `$FEATHERS_LIKES_LOG_DIR/<local date>.jsonl` when the variable is set — an EMPTY value (or `off`, which Windows
// needs: it cannot hold an empty variable) turns logging off — else, once a CLAP entry has armed the event log
// (eventlog::arm), `$XDG_DATA_HOME/feathers/likes-log/`, else `~/.local/share/feathers/likes-log/` (a flatpak host sets
// XDG_DATA_HOME to its own sandbox, as the event log's banner says of XDG_STATE_HOME). Every test binary sets the variable
// empty (tests/event_log_off.cpp), and a test that constructs the editor directly never arms the default.
//
// MESSAGE THREAD ONLY: it opens a file and allocates. SILENT: a file that cannot be written costs the line, never the editor.
// Nothing is written while the Explore stage is not being drawn (the editor closed, another page showing).

#include "plugin/terrain_likes.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace feathers::ui::likeslog {

inline constexpr const char* kEnvVar = "FEATHERS_LIKES_LOG_DIR";
inline constexpr const char* kScheme = "P-MOBIUS";
/// How long a stance must be held still before the walk logs it (about a second).
inline constexpr double kWalkDwellSeconds = 1.0;

/// The directory lines go to now (empty: logging is off).
std::filesystem::path directory();
/// The day's file in `dir`: `<YYYY-MM-DD>.jsonl`, by the local date at `unixSeconds`.
std::filesystem::path fileFor(const std::filesystem::path& dir, double unixSeconds);

/// One place's id: an FNV-1a over its engine, its sound and its full point, as 16 hex digits.
std::string soundId(const TerrainLike& like);

struct Record {
	double time = 0.0;
	std::string session;
	/// snapshot | like | unlike | walk.
	std::string event;
	/// The edit a like/unlike line came from: like, unlike, clear, undo, redo.
	std::string cause;
	uint16_t engine = 0;
	uint16_t slot = 0;
	double strength = 1.0;
	/// like / unlike: the position in the patch's set.
	int index = -1;
	/// like / unlike / walk: the like (or the stance as one).
	std::optional<TerrainLike> like;
	/// snapshot: the patch's whole set.
	std::optional<std::vector<TerrainLike>> likes;
	/// walk: how long this stance had been held when it was written, and how long the previous logged one was held in all.
	std::optional<double> held;
	std::optional<double> dwell;
};

/// The record as one JSON line (no newline).
std::string line(const Record& record);
/// Appends the record to the day's file in `directory()`; false if logging is off or the write failed.
bool append(const Record& record) noexcept;
/// This process's session tag, `<pid>.<unix start>`.
const std::string& session();

/// Every record a file holds, in order; a line that does not parse is skipped and counted in `*skipped`.
std::vector<Record> read(const std::filesystem::path& file, size_t* skipped = nullptr);

/// ONE EDIT AS DELTAS: the like/unlike lines (event, index, like) that take `before` to `after` — removals by descending
/// index in `before`, then additions by ascending index in `after`. Exact when one set is a subsequence of the other (every
/// edit the stage makes: a like, an unlike, a clear and their undos); otherwise every like out, then every like in.
std::vector<Record> deltas(const TerrainLikes& before, const TerrainLikes& after);

/// THE REPLAY: the patch's like set rebuilt from the snapshots and the deltas, line by line.
struct Replay {
	TerrainLikes likes;
	size_t records = 0;
	size_t snapshots = 0;
	/// Deltas that did not fit the rebuilt set (an index out of range, or an unlike naming a different like).
	size_t mismatches = 0;
	/// Applies one line (walks are ignored).
	void apply(const Record& record);
	/// One terrain's likes, in the set's order.
	[[nodiscard]] std::vector<TerrainLike> likesOf(uint16_t engine, uint16_t slot) const;
};
Replay replay(const std::vector<Record>& records);

} // namespace feathers::ui::likeslog
