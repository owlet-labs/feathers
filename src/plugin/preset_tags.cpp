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

#include "plugin/preset_tags.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace feathers {

namespace {

bool excluded(const std::vector<std::string>& exclude, const std::string& tag) {
	return std::find(exclude.begin(), exclude.end(), tag) != exclude.end();
}

/// The value half's spelling — what a whole tag's was before namespaces.
std::string normaliseValue(const std::string& raw) {
	std::string out;
	bool space = false;
	for (const char c : raw) {
		if (c == ',' || static_cast<unsigned char>(c) < 0x20) {
			continue;
		}
		if (c == ' ') {
			space = !out.empty();
			continue;
		}
		if (space) {
			out.push_back(' ');
			space = false;
		}
		out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (out.size() > kMaxTagBytes) {
		out.resize(kMaxTagBytes);
		while (!out.empty() && out.back() == ' ') {
			out.pop_back();
		}
	}
	return out;
}

std::string joinTag(const std::string& ns, const std::string& value) {
	return ns == kCatchAllNamespace ? value : ns + ":" + value;
}

} // namespace

std::string normaliseNamespace(const std::string& raw) {
	std::string out;
	for (const char c : raw) {
		const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if ((l >= 'a' && l <= 'z') || (l >= '0' && l <= '9') || l == '-' || l == '_') {
			out.push_back(l);
		}
	}
	if (out.size() > kMaxTagBytes) {
		out.resize(kMaxTagBytes);
	}
	return out;
}

std::string normaliseTag(const std::string& raw) {
	const size_t colon = raw.find(':');
	if (colon == std::string::npos) {
		return normaliseValue(raw);
	}
	const std::string ns = normaliseNamespace(raw.substr(0, colon));
	if (ns == kCategoryNamespace) {
		return {}; // the folder says where a preset lives (the header)
	}
	const std::string value = normaliseValue(raw.substr(colon + 1));
	if (value.empty()) {
		return {};
	}
	return ns.empty() ? value : joinTag(ns, value);
}

TagParts splitTag(const std::string& tag) {
	const size_t colon = tag.find(':');
	if (colon == std::string::npos) {
		return { kCatchAllNamespace, tag };
	}
	return { tag.substr(0, colon), tag.substr(colon + 1) };
}

std::vector<std::pair<std::string, std::vector<TagValueCount>>> tagViewCounts(const std::vector<PresetMeta>& metas) {
	std::map<std::string, std::map<std::string, int>> counts;
	for (const PresetMeta& meta : metas) {
		std::vector<std::string> seen;
		for (const std::string& raw : meta.tags) {
			const std::string tag = normaliseTag(raw);
			if (tag.empty() || excluded(seen, tag)) {
				continue;
			}
			seen.push_back(tag);
			const TagParts parts = splitTag(tag);
			counts[parts.ns][parts.value] += 1;
		}
	}
	std::vector<std::pair<std::string, std::vector<TagValueCount>>> out;
	for (auto& [ns, values] : counts) {
		std::vector<TagValueCount> list;
		for (auto& [value, count] : values) {
			list.push_back({ value, count });
		}
		std::stable_sort(list.begin(), list.end(),
		                 [](const TagValueCount& a, const TagValueCount& b) { return a.count > b.count; });
		out.emplace_back(ns, std::move(list));
	}
	return out;
}

std::vector<std::string> parseTagList(const std::string& text) {
	std::vector<std::string> out;
	size_t start = 0;
	while (start <= text.size()) {
		size_t comma = text.find(',', start);
		if (comma == std::string::npos) {
			comma = text.size();
		}
		const std::string tag = normaliseTag(text.substr(start, comma - start));
		if (!tag.empty() && !excluded(out, tag)) {
			out.push_back(tag);
		}
		start = comma + 1;
	}
	return out;
}

std::string joinTags(const std::vector<std::string>& tags) {
	std::string out;
	for (const std::string& t : tags) {
		if (!out.empty()) {
			out += ", ";
		}
		out += t;
	}
	return out;
}

std::vector<TagStat> rankTags(const std::vector<PresetMeta>& metas, int64_t now) {
	constexpr double kDay = 86400.0;
	std::map<std::string, TagStat> by;
	for (const PresetMeta& meta : metas) {
		// NO SAVE TIME COUNTS AS A YEAR OLD: still a use, never a recent one.
		const double ageDays =
		    meta.saved > 0 ? std::max(0.0, static_cast<double>(now - meta.saved) / kDay) : 12.0 * kTagHalfLifeDays;
		const double weight = std::pow(0.5, ageDays / kTagHalfLifeDays);
		for (const std::string& raw : meta.tags) {
			const std::string tag = normaliseTag(raw);
			if (tag.empty()) {
				continue;
			}
			TagStat& s = by[tag];
			s.tag = tag;
			s.score += weight;
			s.count += 1;
			s.latest = std::max(s.latest, meta.saved);
		}
	}
	std::vector<TagStat> out;
	out.reserve(by.size());
	for (auto& [tag, stat] : by) {
		out.push_back(std::move(stat));
	}
	std::sort(out.begin(), out.end(), [](const TagStat& a, const TagStat& b) {
		if (a.score != b.score) {
			return a.score > b.score;
		}
		if (a.count != b.count) {
			return a.count > b.count;
		}
		if (a.latest != b.latest) {
			return a.latest > b.latest;
		}
		return a.tag < b.tag;
	});
	return out;
}

std::vector<std::string> tagCompletions(const std::vector<TagStat>& ranked, const std::string& prefix,
                                        const std::vector<std::string>& exclude, size_t limit) {
	std::vector<std::string> out;
	const auto starts = [](const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; };
	const size_t colon = prefix.find(':');
	if (colon != std::string::npos) {
		// "mood:" / "mood:da" — THAT NAMESPACE'S VALUES, in the ranking's order.
		std::string ns = normaliseNamespace(prefix.substr(0, colon));
		if (ns.empty()) {
			ns = kCatchAllNamespace;
		}
		if (ns == kCategoryNamespace) {
			return out;
		}
		const std::string v = normaliseValue(prefix.substr(colon + 1));
		for (const TagStat& s : ranked) {
			if (out.size() >= limit) {
				break;
			}
			const TagParts parts = splitTag(s.tag);
			if (parts.ns == ns && starts(parts.value, v) && parts.value != v && !excluded(exclude, s.tag)) {
				out.push_back(s.tag);
			}
		}
		return out;
	}
	const std::string p = normaliseValue(prefix);
	if (p.empty()) {
		return out;
	}
	// "mo" — THE NAMESPACES IT STARTS, best first by the summed score of their tags, offered as "mood:"
	// so the value can be typed next...
	std::vector<std::pair<std::string, double>> spaces;
	for (const TagStat& s : ranked) {
		const TagParts parts = splitTag(s.tag);
		if (parts.ns == kCatchAllNamespace || !starts(parts.ns, p)) {
			continue;
		}
		auto it = std::find_if(spaces.begin(), spaces.end(), [&](const auto& e) { return e.first == parts.ns; });
		if (it == spaces.end()) {
			spaces.emplace_back(parts.ns, s.score);
		}
		else {
			it->second += s.score;
		}
	}
	std::stable_sort(spaces.begin(), spaces.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
	for (const auto& [ns, score] : spaces) {
		if (out.size() >= limit) {
			break;
		}
		out.push_back(ns + ":");
	}
	// ...THEN THE BARE TAGS IT STARTS, as before namespaces.
	for (const TagStat& s : ranked) {
		if (out.size() >= limit) {
			break;
		}
		if (s.tag.find(':') == std::string::npos && starts(s.tag, p) && s.tag != p && !excluded(exclude, s.tag)) {
			out.push_back(s.tag);
		}
	}
	return out;
}

std::vector<std::string> tagSuggestions(const std::vector<TagStat>& ranked, const std::vector<std::string>& exclude,
                                        size_t limit) {
	std::vector<std::string> out;
	for (const TagStat& s : ranked) {
		if (out.size() >= limit) {
			break;
		}
		if (!excluded(exclude, s.tag)) {
			out.push_back(s.tag);
		}
	}
	return out;
}

} // namespace feathers
