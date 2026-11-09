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

#include "plugin/preset_store.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// **PRESET TAGS — spelling, ranking, completion** (backlog FO).
//
// PER TYPE BY CONSTRUCTION: every function here takes the presets it ranks, and the browser hands it
// its OWN bank — SKLITTER's suggestions come from SKLITTER's presets, Feathers' from Feathers'. Nothing
// here knows where a bank is.
//
// **THE RANKING — RECENCY-WEIGHTED FREQUENCY.** A tag's score is the sum, over every preset carrying it,
// of `0.5 ^ (age / 30 days)`, where age is `now - saved` (the metadata's save time, `preset_store.h`; a
// preset with no time counts as old — weight 0.5^12, so it still counts but never outranks). So a tag on
// ten presets from last year and a tag on two presets from this week come out close, and a tag used
// today counts double one used a month ago. Ties: more presets, then the more recent last use, then the
// alphabet — so the order is total and a list does not shuffle between two opens.
//
// **A TAG IS `namespace:value`**, split at
// its FIRST colon. Spelt one way:
//   - THE VALUE: trimmed, inner runs of spaces collapsed, lower case, at most 32 bytes — the rules a whole
//     tag had before namespaces, now applied to the value alone.
//   - THE NAMESPACE: lower case, spaces and tabs dropped, only `a-z 0-9 - _` kept, at most 32 bytes.
//   - A TAG WITH NO NAMESPACE is in the catch-all namespace `tag`, and is SPELT BARE: "dark", never
//     "tag:dark" ("tag:dark" and ":dark" both normalise to "dark"). So every file written before
//     namespaces reads exactly as it did.
//   - `category` IS RESERVED for the folder (`preset_store.h`): a typed "category:x" is dropped, since
//     where a preset lives is said by its folder, and the metadata's copy of it is written by the store.
//   - A comma is the separator in the text field, so it can never be in a tag.
//
// **THE TAG VIEW IS TWO LEVELS: namespace ▸ value (count) ▸ presets.** Values are ordered by how many
// presets carry them (then alphabetically), and show that count; past `kTagViewValues` in one namespace
// the rest go under "More…". `category` never appears there — the folder view is that view.

namespace feathers {

inline constexpr double kTagHalfLifeDays = 30.0;
inline constexpr size_t kTagSuggestions = 5;
inline constexpr size_t kMaxTagBytes = 32;
/// The namespace of a tag typed without one, which is spelt bare.
inline constexpr const char* kCatchAllNamespace = "tag";
/// How many values a namespace shows in the tag view before the rest go under "More…".
inline constexpr size_t kTagViewValues = 12;

/// A normalised tag, in its two halves. `ns` is `kCatchAllNamespace` for a bare tag.
struct TagParts {
	std::string ns;
	std::string value;
};
/// One namespace in its one spelling; empty when nothing is left.
std::string normaliseNamespace(const std::string& raw);
/// The halves of a NORMALISED tag.
TagParts splitTag(const std::string& tag);

/// How many presets carry one value of a namespace.
struct TagValueCount {
	std::string value;
	int count = 0;
};
/// **THE TAG VIEW'S NUMBERS** — every namespace in `metas` (alphabetical), each with its values most
/// carried first (ties alphabetical). A preset carrying one tag twice counts once.
std::vector<std::pair<std::string, std::vector<TagValueCount>>> tagViewCounts(const std::vector<PresetMeta>& metas);

struct TagStat {
	std::string tag;
	double score = 0.0;
	int count = 0;
	int64_t latest = 0;
};

/// One tag in its one spelling; empty when nothing is left.
std::string normaliseTag(const std::string& raw);
/// "Dark, bass ,, Dark" → {"dark", "bass"}: split on commas, normalised, duplicates dropped, order kept.
std::vector<std::string> parseTagList(const std::string& text);
/// The inverse, for putting tags back into a field: "dark, bass".
std::string joinTags(const std::vector<std::string>& tags);

/// Every tag in `metas`, best first (see the file comment).
std::vector<TagStat> rankTags(const std::vector<PresetMeta>& metas, int64_t now);
/// **WHAT THE TAGS FIELD OFFERS FOR `typed`** — the text after its last comma, as typed. NAMESPACE-AWARE:
///   - no colon ("mo"): the namespaces starting with it, as "mood:" (best namespace first, by the summed
///     score of its tags), then the bare tags starting with it;
///   - a colon ("mood:" or "mood:da"): that namespace's values starting with what follows, as "mood:dark",
///     best first.
/// None of `exclude`, and never exactly what is already typed.
std::vector<std::string> tagCompletions(const std::vector<TagStat>& ranked, const std::string& prefix,
                                        const std::vector<std::string>& exclude, size_t limit = kTagSuggestions);
/// The best `limit` known tags, none of `exclude`.
std::vector<std::string> tagSuggestions(const std::vector<TagStat>& ranked, const std::vector<std::string>& exclude,
                                        size_t limit = kTagSuggestions);

} // namespace feathers
