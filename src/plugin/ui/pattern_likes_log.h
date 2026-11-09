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

// THE PATTERN'S LIKE LOG (design §14-bis: the likes surface's log, given a fingerprint road): what is liked on the Pattern
// terrain, appended as JSON lines beside Feathers' own like-set log (`likes_log.h`) — same directory, same switch
// (`FEATHERS_LIKES_LOG_DIR`, empty or `off` silences it; tests set it empty), same session tag — so a sweep of the
// pattern likes' warp can run offline over real sessions, as the terrain's does.
//
// ITS OWN FILE, `<local date>.pattern.jsonl` (lead's call, to be confirmed): Feathers' replay reads every line of its day
// file as one of its own and would count these as unparsed, and a pattern like is not a TerrainLike (no engine, no
// spectrum; a fingerprint of 48 and a point of 62 instead).
//
// EVENTS: `snapshot` — the slot's whole like set (`likes`), written the first time this instance logs that slot (so a
// replay starts from what the patch already held); `like` — one like appended at `index` (the oldest gives way past 16:
// `dropped` says one did). EVERY LINE: {"v":1,"t":<unix s>,"session":"<pid>.<start>","device":"PAT","instance":<id>,
// "slot":<0..3>,"event":…, then "index","dropped","like":{zone, zoneOffset, gamma, placeX, placeY, radius, theta, rotate,
// heard[48], point[62]} or "likes":[…]}. Floats to 9 digits and the point's doubles to 17, so a replay refits exactly
// what the window scored.
//
// MESSAGE THREAD ONLY (it opens a file). SILENT: a file that cannot be written costs the line, never the window.

#include "mod/arp/pattern_likes.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace feathers::ui::patternlog {

/// The day's file in `dir`: `<YYYY-MM-DD>.pattern.jsonl`.
std::filesystem::path fileFor(const std::filesystem::path& dir, double unixSeconds);
/// One like as its JSON object.
std::string likeJson(const mod::arp::likes::Like& like);
/// Logs a like just made: a snapshot of the slot `before` it (the first time `instance`'s `slot` is logged by this process),
/// then the like at `index`. False when logging is off or a write failed.
bool logLike(uint64_t instance, size_t slot, const mod::arp::likes::SlotLikes& before, const mod::arp::likes::SlotLikes& after);

} // namespace feathers::ui::patternlog
