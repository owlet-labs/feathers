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

#include "plugin/ui/likes_log.h"

#include "dsp/phase_warp.h"
#include "plugin/event_log.h"
#include "plugin/params.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <string_view>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace feathers::ui::likeslog {

namespace {

std::filesystem::path platformDefault() {
#if defined(__APPLE__)
	const char* home = std::getenv("HOME");
	return home != nullptr && home[0] != '\0'
	           ? std::filesystem::path(home) / "Library" / "Application Support" / "Feathers" / "likes-log"
	           : std::filesystem::path {};
#elif defined(_WIN32)
	const char* local = std::getenv("LOCALAPPDATA");
	return local != nullptr && local[0] != '\0' ? std::filesystem::path(local) / "Feathers" / "likes-log"
	                                            : std::filesystem::path {};
#else
	if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
		return std::filesystem::path(xdg) / "feathers" / "likes-log";
	}
	const char* home = std::getenv("HOME");
	return home != nullptr && home[0] != '\0' ? std::filesystem::path(home) / ".local" / "share" / "feathers" / "likes-log"
	                                          : std::filesystem::path {};
#endif
}

void number(std::string& out, double v) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.17g", v);
	out += buffer;
}
void number(std::string& out, float v) {
	char buffer[32];
	std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(v));
	out += buffer;
}
template <typename T>
void array(std::string& out, const T* values, size_t count) {
	out += '[';
	for (size_t i = 0; i < count; ++i) {
		if (i > 0) {
			out += ',';
		}
		number(out, values[i]);
	}
	out += ']';
}

void likeJson(std::string& out, const TerrainLike& like) {
	out += "{\"x\":";
	number(out, like.x);
	out += ",\"y\":";
	number(out, like.y);
	out += ",\"home\":";
	number(out, like.home);
	out += ",\"engine\":" + std::to_string(like.engine) + ",\"slot\":" + std::to_string(like.slot);
	out += ",\"sound\":\"" + soundId(like) + "\"";
	out += ",\"pointCount\":" + std::to_string(like.pointCount) + ",\"point\":";
	array(out, like.point.data(), std::min<size_t>(like.pointCount, like.point.size()));
	out += ",\"spectrum\":";
	array(out, like.spectrum.data(), like.spectrum.size());
	out += ",\"angles\":";
	array(out, like.angles.data(), like.angles.size());
	out += '}';
}

// A SMALL JSON READER — the subset `line` writes (objects, arrays, numbers, strings without escapes, true/false/null).
struct Value {
	enum class Kind { Null, Number, String, Array, Object } kind = Kind::Null;
	double number = 0.0;
	std::string text;
	std::vector<Value> items;
	std::vector<std::pair<std::string, Value>> fields;
	[[nodiscard]] const Value* field(const char* name) const {
		for (const auto& [key, value] : fields) {
			if (key == name) {
				return &value;
			}
		}
		return nullptr;
	}
};

struct Reader {
	const std::string& s;
	size_t at = 0;
	void space() {
		while (at < s.size() && (s[at] == ' ' || s[at] == '\t' || s[at] == '\r' || s[at] == '\n')) {
			++at;
		}
	}
	bool literal(const char* word) {
		const size_t n = std::char_traits<char>::length(word);
		if (s.compare(at, n, word) != 0) {
			return false;
		}
		at += n;
		return true;
	}
	bool string(std::string& out) {
		if (at >= s.size() || s[at] != '"') {
			return false;
		}
		const size_t end = s.find('"', at + 1);
		if (end == std::string::npos) {
			return false;
		}
		out = s.substr(at + 1, end - at - 1);
		at = end + 1;
		return true;
	}
	bool value(Value& out, int depth) {
		if (depth > 8) {
			return false;
		}
		space();
		if (at >= s.size()) {
			return false;
		}
		const char c = s[at];
		if (c == '{') {
			out.kind = Value::Kind::Object;
			++at;
			space();
			if (at < s.size() && s[at] == '}') {
				++at;
				return true;
			}
			for (;;) {
				space();
				std::string key;
				if (!string(key)) {
					return false;
				}
				space();
				if (at >= s.size() || s[at] != ':') {
					return false;
				}
				++at;
				Value v;
				if (!value(v, depth + 1)) {
					return false;
				}
				out.fields.emplace_back(std::move(key), std::move(v));
				space();
				if (at < s.size() && s[at] == ',') {
					++at;
					continue;
				}
				if (at < s.size() && s[at] == '}') {
					++at;
					return true;
				}
				return false;
			}
		}
		if (c == '[') {
			out.kind = Value::Kind::Array;
			++at;
			space();
			if (at < s.size() && s[at] == ']') {
				++at;
				return true;
			}
			for (;;) {
				Value v;
				if (!value(v, depth + 1)) {
					return false;
				}
				out.items.push_back(std::move(v));
				space();
				if (at < s.size() && s[at] == ',') {
					++at;
					continue;
				}
				if (at < s.size() && s[at] == ']') {
					++at;
					return true;
				}
				return false;
			}
		}
		if (c == '"') {
			out.kind = Value::Kind::String;
			return string(out.text);
		}
		if (literal("true")) {
			out.kind = Value::Kind::Number;
			out.number = 1.0;
			return true;
		}
		if (literal("false")) {
			out.kind = Value::Kind::Number;
			return true;
		}
		if (literal("null")) {
			return true;
		}
		char* end = nullptr;
		out.number = std::strtod(s.c_str() + at, &end);
		if (end == s.c_str() + at) {
			return false;
		}
		out.kind = Value::Kind::Number;
		at = static_cast<size_t>(end - s.c_str());
		return true;
	}
};

double numberOf(const Value& object, const char* name, double fallback = 0.0) {
	const Value* v = object.field(name);
	return v != nullptr && v->kind == Value::Kind::Number ? v->number : fallback;
}

template <typename T, size_t N>
bool fill(const Value& object, const char* name, std::array<T, N>& out, size_t* count = nullptr) {
	const Value* v = object.field(name);
	if (v == nullptr || v->kind != Value::Kind::Array || v->items.size() > N) {
		return false;
	}
	out.fill(T {});
	for (size_t i = 0; i < v->items.size(); ++i) {
		out[i] = static_cast<T>(v->items[i].number);
	}
	if (count != nullptr) {
		*count = v->items.size();
	}
	return true;
}

bool likeOf(const Value& v, TerrainLike& like) {
	if (v.kind != Value::Kind::Object) {
		return false;
	}
	like = {};
	like.x = numberOf(v, "x");
	like.y = numberOf(v, "y");
	like.home = numberOf(v, "home");
	like.engine = static_cast<uint16_t>(numberOf(v, "engine"));
	like.slot = static_cast<uint16_t>(numberOf(v, "slot"));
	like.pointCount = static_cast<uint16_t>(numberOf(v, "pointCount"));
	size_t points = 0;
	return fill(v, "point", like.point, &points) && points == like.pointCount && fill(v, "spectrum", like.spectrum)
	       && fill(v, "angles", like.angles);
}

bool sameTerrain(const TerrainLike& like, uint16_t engine, uint16_t slot) {
	return like.engine == engine && like.slot == slot;
}

} // namespace

std::filesystem::path directory() {
	if (const char* over = std::getenv(kEnvVar); over != nullptr) {
		// EMPTY TURNS IT OFF — and so does "off", because Windows cannot hold an empty variable (`_putenv_s` with "" deletes it).
		return over[0] == '\0' || std::string_view(over) == "off" ? std::filesystem::path {} : std::filesystem::path(over);
	}
	return eventlog::armed() ? platformDefault() : std::filesystem::path {};
}

std::filesystem::path fileFor(const std::filesystem::path& dir, double unixSeconds) {
	const auto t = static_cast<std::time_t>(unixSeconds);
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &t);
#else
	localtime_r(&t, &local);
#endif
	char name[32];
	std::strftime(name, sizeof(name), "%Y-%m-%d.jsonl", &local);
	return dir / name;
}

std::string soundId(const TerrainLike& like) {
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](const void* p, size_t bytes) {
		const auto* b = static_cast<const unsigned char*>(p);
		for (size_t i = 0; i < bytes; ++i) {
			h = (h ^ b[i]) * 1099511628211ull;
		}
	};
	mix(&like.engine, sizeof(like.engine));
	mix(&like.slot, sizeof(like.slot));
	mix(&like.pointCount, sizeof(like.pointCount));
	mix(like.point.data(), sizeof(double) * std::min<size_t>(like.pointCount, like.point.size()));
	char text[20];
	std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(h));
	return text;
}

const std::string& session() {
	static const std::string tag = [] {
#if defined(_WIN32)
		const long pid = static_cast<long>(_getpid());
#else
		const long pid = static_cast<long>(::getpid());
#endif
		const auto start = std::chrono::duration_cast<std::chrono::seconds>(
		                       std::chrono::system_clock::now().time_since_epoch())
		                       .count();
		return std::to_string(pid) + "." + std::to_string(static_cast<long long>(start));
	}();
	return tag;
}

std::string line(const Record& record) {
	std::string out;
	out.reserve(record.likes.has_value() ? 8192 * (record.likes->size() + 1) : 8192);
	out += "{\"v\":2,\"t\":";
	char t[32];
	std::snprintf(t, sizeof(t), "%.3f", record.time);
	out += t;
	out += ",\"session\":\"" + record.session + "\",\"event\":\"" + record.event + "\"";
	if (!record.cause.empty()) {
		out += ",\"cause\":\"" + record.cause + "\"";
	}
	const size_t engine = record.engine;
	out += ",\"engine\":\"" + std::string(engine < kOscTypeNames.size() ? kOscTypeNames[engine] : "?") + "\"";
	out += ",\"engineId\":" + std::to_string(record.engine) + ",\"slot\":" + std::to_string(record.slot);
	out += ",\"scheme\":\"" + std::string(kScheme) + "\",\"hp\":{\"k0\":";
	number(out, kPhaseWarpPrior);
	out += ",\"kMax\":" + std::to_string(kPhaseWarpMaxComponents) + ",\"knn\":" + std::to_string(kPhaseWarpNeighbours)
	       + ",\"iters\":" + std::to_string(kPhaseWarpIterations) + ",\"lut\":" + std::to_string(kPhaseWarpLut)
	       + ",\"widthFloor\":";
	number(out, kPhaseWarpWidthFloor);
	out += ",\"pinWidth\":";
	number(out, kPhaseWarpPinWidth);
	out += "},\"strength\":";
	number(out, record.strength);
	if (record.index >= 0) {
		out += ",\"index\":" + std::to_string(record.index);
	}
	if (record.like.has_value()) {
		out += ",\"sound\":\"" + soundId(*record.like) + "\",\"like\":";
		likeJson(out, *record.like);
	}
	if (record.likes.has_value()) {
		out += ",\"likes\":[";
		for (size_t i = 0; i < record.likes->size(); ++i) {
			if (i > 0) {
				out += ',';
			}
			likeJson(out, (*record.likes)[i]);
		}
		out += ']';
	}
	if (record.held.has_value()) {
		out += ",\"held\":";
		number(out, *record.held);
	}
	if (record.dwell.has_value()) {
		out += ",\"dwell\":";
		number(out, *record.dwell);
	}
	out += '}';
	return out;
}

bool append(const Record& record) noexcept {
	try {
		const std::filesystem::path dir = directory();
		if (dir.empty()) {
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		// ONE WRITE OF ONE WHOLE LINE, append mode, as the event log writes: two instances interleave lines, not characters.
		std::ofstream out(fileFor(dir, record.time), std::ios::binary | std::ios::app);
		if (!out) {
			return false;
		}
		const std::string text = line(record) + "\n";
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
		return static_cast<bool>(out);
	}
	catch (...) {
		return false;
	}
}

std::vector<Record> read(const std::filesystem::path& file, size_t* skipped) {
	std::vector<Record> out;
	size_t bad = 0;
	std::ifstream in(file, std::ios::binary);
	std::string text;
	while (std::getline(in, text)) {
		if (text.empty()) {
			continue;
		}
		Reader reader { text };
		Value v;
		Record r;
		if (!reader.value(v, 0) || v.kind != Value::Kind::Object) {
			++bad;
			continue;
		}
		r.time = numberOf(v, "t");
		if (const Value* s = v.field("session"); s != nullptr) {
			r.session = s->text;
		}
		if (const Value* e = v.field("event"); e != nullptr) {
			r.event = e->text;
		}
		if (const Value* c = v.field("cause"); c != nullptr) {
			r.cause = c->text;
		}
		r.engine = static_cast<uint16_t>(numberOf(v, "engineId"));
		r.slot = static_cast<uint16_t>(numberOf(v, "slot"));
		r.strength = numberOf(v, "strength", 1.0);
		r.index = static_cast<int>(numberOf(v, "index", -1.0));
		bool ok = !r.event.empty();
		if (const Value* like = v.field("like"); like != nullptr) {
			TerrainLike l;
			ok = ok && likeOf(*like, l);
			r.like = l;
		}
		if (const Value* set = v.field("likes"); set != nullptr) {
			r.likes.emplace();
			for (const Value& item : set->items) {
				TerrainLike l;
				ok = ok && likeOf(item, l);
				r.likes->push_back(l);
			}
		}
		if (const Value* h = v.field("held"); h != nullptr) {
			r.held = h->number;
		}
		if (const Value* d = v.field("dwell"); d != nullptr) {
			r.dwell = d->number;
		}
		if (!ok) {
			++bad;
			continue;
		}
		out.push_back(std::move(r));
	}
	if (skipped != nullptr) {
		*skipped = bad;
	}
	return out;
}

std::vector<Record> deltas(const TerrainLikes& before, const TerrainLikes& after) {
	const std::vector<TerrainLike>& a = before.likes;
	const std::vector<TerrainLike>& b = after.likes;
	// WHICH IS THE SUBSEQUENCE: `kept[i]` marks the entries of the longer set the shorter one walks through, in order.
	const auto subsequence = [](const std::vector<TerrainLike>& longer, const std::vector<TerrainLike>& shorter,
	                            std::vector<bool>& kept) {
		kept.assign(longer.size(), false);
		size_t j = 0;
		for (size_t i = 0; i < longer.size() && j < shorter.size(); ++i) {
			if (longer[i] == shorter[j]) {
				kept[i] = true;
				++j;
			}
		}
		return j == shorter.size();
	};
	std::vector<bool> keptA(a.size(), false);
	std::vector<bool> keptB(b.size(), false);
	std::vector<bool> kept;
	if (b.size() <= a.size() && subsequence(a, b, kept)) {
		keptA = kept;
		keptB.assign(b.size(), true);
	}
	else if (a.size() < b.size() && subsequence(b, a, kept)) {
		keptB = kept;
		keptA.assign(a.size(), true);
	}
	std::vector<Record> out;
	for (size_t i = a.size(); i-- > 0;) {
		if (!keptA[i]) {
			Record r;
			r.event = "unlike";
			r.index = static_cast<int>(i);
			r.like = a[i];
			r.engine = a[i].engine;
			r.slot = a[i].slot;
			out.push_back(std::move(r));
		}
	}
	for (size_t i = 0; i < b.size(); ++i) {
		if (!keptB[i]) {
			Record r;
			r.event = "like";
			r.index = static_cast<int>(i);
			r.like = b[i];
			r.engine = b[i].engine;
			r.slot = b[i].slot;
			out.push_back(std::move(r));
		}
	}
	return out;
}

void Replay::apply(const Record& r) {
	if (r.event == "walk") {
		return;
	}
	++records;
	std::vector<TerrainLike>& set = likes.likes;
	if (r.event == "snapshot") {
		++snapshots;
		set = r.likes.value_or(std::vector<TerrainLike> {});
		return;
	}
	const bool fits = r.like.has_value() && r.index >= 0;
	if (r.event == "like") {
		if (!fits || static_cast<size_t>(r.index) > set.size()) {
			++mismatches;
			return;
		}
		set.insert(set.begin() + r.index, *r.like);
	}
	else if (r.event == "unlike") {
		if (!fits || static_cast<size_t>(r.index) >= set.size() || !(set[static_cast<size_t>(r.index)] == *r.like)) {
			++mismatches;
			return;
		}
		set.erase(set.begin() + r.index);
	}
}

std::vector<TerrainLike> Replay::likesOf(uint16_t engine, uint16_t slot) const {
	std::vector<TerrainLike> out;
	for (const TerrainLike& l : likes.likes) {
		if (sameTerrain(l, engine, slot)) {
			out.push_back(l);
		}
	}
	return out;
}

Replay replay(const std::vector<Record>& records) {
	Replay out;
	for (const Record& r : records) {
		out.apply(r);
	}
	return out;
}

} // namespace feathers::ui::likeslog
