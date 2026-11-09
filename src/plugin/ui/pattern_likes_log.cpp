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

#include "plugin/ui/pattern_likes_log.h"

#include "plugin/ui/likes_log.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <set>
#include <utility>

namespace feathers::ui::patternlog {

namespace likes = feathers::mod::arp::likes;

namespace {

void number(std::string& out, float v) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.9g", static_cast<double>(v));
	out += text;
}
void number(std::string& out, double v) {
	char text[40];
	std::snprintf(text, sizeof(text), "%.17g", v);
	out += text;
}

template <typename Range>
void array(std::string& out, const Range& values) {
	out += '[';
	bool first = true;
	for (const auto v : values) {
		if (!first) {
			out += ',';
		}
		first = false;
		number(out, v);
	}
	out += ']';
}

std::string head(uint64_t instance, size_t slot, const char* event, double now) {
	std::string out = "{\"v\":1,\"t\":";
	char text[32];
	std::snprintf(text, sizeof(text), "%.3f", now);
	out += text;
	out += ",\"session\":\"" + likeslog::session() + "\",\"device\":\"PAT\",\"instance\":" + std::to_string(instance)
	       + ",\"slot\":" + std::to_string(slot) + ",\"event\":\"" + event + "\"";
	return out;
}

bool append(const std::string& text, double now) noexcept {
	try {
		const std::filesystem::path dir = likeslog::directory();
		if (dir.empty()) {
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		// ONE WRITE OF ONE WHOLE LINE, append mode, as Feathers' log writes.
		std::ofstream out(fileFor(dir, now), std::ios::binary | std::ios::app);
		if (!out) {
			return false;
		}
		const std::string line = text + "\n";
		out.write(line.data(), static_cast<std::streamsize>(line.size()));
		return static_cast<bool>(out);
	}
	catch (...) {
		return false;
	}
}

} // namespace

std::filesystem::path fileFor(const std::filesystem::path& dir, double unixSeconds) {
	std::filesystem::path day = likeslog::fileFor(dir, unixSeconds);
	day.replace_extension(".pattern.jsonl");
	return day;
}

std::string likeJson(const likes::Like& l) {
	std::string out = "{";
	const std::pair<const char*, float> fields[] = { { "zone", l.zone },     { "zoneOffset", l.zoneOffset }, { "gamma", l.gamma },
		                                             { "placeX", l.placeX }, { "placeY", l.placeY },         { "radius", l.radius },
		                                             { "theta", l.theta },   { "rotate", l.rotate } };
	for (const auto& [name, value] : fields) {
		out += '"';
		out += name;
		out += "\":";
		number(out, value);
		out += ',';
	}
	out += "\"heard\":";
	array(out, l.heard);
	out += ",\"point\":";
	array(out, l.point);
	out += '}';
	return out;
}

bool logLike(uint64_t instance, size_t slot, const likes::SlotLikes& before, const likes::SlotLikes& after) {
	if (likeslog::directory().empty() || after.count == 0) {
		return false;
	}
	const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	// THE FIRST LINE ABOUT THIS SLOT IN THIS PROCESS IS ITS WHOLE SET, so the deltas after it replay to the live set.
	static std::set<std::pair<uint64_t, size_t>> snapshotted;
	bool ok = true;
	if (snapshotted.insert({ instance, slot }).second) {
		std::string line = head(instance, slot, "snapshot", now) + ",\"likes\":[";
		for (size_t i = 0; i < before.count; ++i) {
			line += (i > 0 ? "," : "") + likeJson(before.like[i]);
		}
		ok = append(line + "]}", now);
	}
	const bool dropped = before.count == likes::kMaxLikes;
	std::string line = head(instance, slot, "like", now) + ",\"index\":" + std::to_string(after.count - 1)
	                   + ",\"dropped\":" + (dropped ? "true" : "false") + ",\"like\":" + likeJson(after.like[after.count - 1]) + "}";
	return append(line, now) && ok;
}

} // namespace feathers::ui::patternlog
