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

#include "plugin/preset_store.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

namespace feathers {

namespace {

/// The extension. Its own rather than a generic one, so that a directory listing says what these are
/// and so that nothing else dropped in the folder is mistaken for a preset.
constexpr const char* kExtension = ".fpreset";

constexpr char kMagic[4] = { 'F', 'P', 'R', 'E' };
constexpr uint8_t kFileVersion = 1;
/// A header claiming more metadata than this is lying, whatever the file's size.
constexpr uint32_t kMaxMetaBytes = 1u << 20;
/// How deep a category may nest before the walk stops looking.
constexpr int kMaxDepth = 8;

bool hidden(const std::filesystem::path& p) {
	const std::string name = p.filename().string();
	return !name.empty() && name[0] == '.';
}

std::vector<uint8_t> readFile(const std::filesystem::path& file) {
	std::ifstream in(file, std::ios::binary);
	if (!in) {
		return {};
	}
	return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// A FILE IS REPLACED, NEVER WRITTEN IN PLACE: the bytes go to a sibling temp file in the same directory (so the
/// rename below stays on one filesystem), are flushed and the stream's state checked, and only then take the target's
/// place. A crash or a full disk mid-write leaves the old preset whole. The temp's extension is not the preset one,
/// so a listing never mistakes it for a preset.
bool writeFile(const std::filesystem::path& file, const std::vector<uint8_t>& bytes) {
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	std::filesystem::path temp = file;
	temp += ".tmp";
	{
		std::ofstream out(temp, std::ios::binary | std::ios::trunc);
		if (!out) {
			return false; // nothing was created, so there is nothing of ours to remove
		}
		out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		out.flush();
		out.close();
		if (!out.good()) {
			std::filesystem::remove(temp, ec);
			return false;
		}
	}
	std::filesystem::rename(temp, file, ec); // atomic over an existing target on POSIX
#ifdef _WIN32
	if (ec) {
		// A platform whose rename will not replace: copy over the target, then drop the temp.
		ec.clear();
		std::filesystem::copy_file(temp, file, std::filesystem::copy_options::overwrite_existing, ec);
		std::error_code ignored;
		std::filesystem::remove(temp, ignored);
	}
#endif
	if (ec) {
		std::error_code ignored;
		std::filesystem::remove(temp, ignored);
		return false;
	}
	return true;
}

/// THE BYTES OF `s[at..]` AS ONE WELL-FORMED UTF-8 SCALAR, or 0: shortest form only, no surrogates, nothing past
/// U+10FFFF, and not a C1 control (U+0080..U+009F), which no file name should carry.
size_t utf8Length(const std::string& s, size_t at) {
	const auto byte = [&](size_t i) { return static_cast<unsigned char>(s[i]); };
	const unsigned char lead = byte(at);
	size_t n = 0;
	uint32_t cp = 0;
	if (lead >= 0xC2 && lead <= 0xDF) {
		n = 2;
		cp = lead & 0x1Fu;
	}
	else if (lead >= 0xE0 && lead <= 0xEF) {
		n = 3;
		cp = lead & 0x0Fu;
	}
	else if (lead >= 0xF0 && lead <= 0xF4) {
		n = 4;
		cp = lead & 0x07u;
	}
	else {
		return 0;
	}
	if (at + n > s.size()) {
		return 0;
	}
	for (size_t i = 1; i < n; ++i) {
		const unsigned char c = byte(at + i);
		if ((c & 0xC0u) != 0x80u) {
			return 0;
		}
		cp = (cp << 6) | (c & 0x3Fu);
	}
	const bool shortest = (n == 2) || (n == 3 && cp >= 0x800) || (n == 4 && cp >= 0x10000);
	const bool surrogate = cp >= 0xD800 && cp <= 0xDFFF;
	const bool c1 = cp >= 0x80 && cp <= 0x9F;
	return shortest && !surrogate && !c1 && cp <= 0x10FFFF ? n : 0;
}

void jsonString(std::string& out, const std::string& s) {
	out.push_back('"');
	for (const char c : s) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				char buf[8];
				std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
				out += buf;
			}
			else {
				out.push_back(c);
			}
		}
	}
	out.push_back('"');
}

/// A tag in the reserved `category` namespace — its namespace compared as tags normalise one (trimmed,
/// any case), so a hand-written "Category: Pads" is one too.
bool isCategoryTag(const std::string& tag) {
	const size_t colon = tag.find(':');
	if (colon == std::string::npos) {
		return false;
	}
	std::string ns;
	for (const char c : tag.substr(0, colon)) {
		if (c != ' ' && c != '\t') {
			ns.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
		}
	}
	return ns == kCategoryNamespace;
}

/// **A READER FOR THE ONE SHAPE THE METADATA HAS** — an object of scalars and arrays of strings. It is
/// not a JSON library and does not pretend to be: an unknown member is SKIPPED (any value, nested or
/// not), which is the forward-compatibility the file needs, and anything malformed is a refusal.
struct JsonReader {
	const std::string& s;
	size_t at = 0;

	void space() {
		while (at < s.size() && (s[at] == ' ' || s[at] == '\n' || s[at] == '\r' || s[at] == '\t')) {
			++at;
		}
	}
	bool eat(char c) {
		space();
		if (at < s.size() && s[at] == c) {
			++at;
			return true;
		}
		return false;
	}
	bool string(std::string* out) {
		space();
		if (at >= s.size() || s[at] != '"') {
			return false;
		}
		++at;
		std::string v;
		while (at < s.size() && s[at] != '"') {
			char c = s[at++];
			if (c == '\\') {
				if (at >= s.size()) {
					return false;
				}
				const char e = s[at++];
				switch (e) {
				case 'n':
					c = '\n';
					break;
				case 't':
					c = '\t';
					break;
				case 'r':
					c = '\r';
					break;
				case 'b':
					c = '\b';
					break;
				case 'f':
					c = '\f';
					break;
				case 'u': {
					if (at + 4 > s.size()) {
						return false;
					}
					const unsigned code = static_cast<unsigned>(std::strtoul(s.substr(at, 4).c_str(), nullptr, 16));
					at += 4;
					// UTF-8 for the basic plane; a surrogate pair is kept as two replacement marks.
					if (code < 0x80) {
						v.push_back(static_cast<char>(code));
					}
					else if (code < 0x800) {
						v.push_back(static_cast<char>(0xC0 | (code >> 6)));
						v.push_back(static_cast<char>(0x80 | (code & 0x3F)));
					}
					else {
						v.push_back(static_cast<char>(0xE0 | (code >> 12)));
						v.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
						v.push_back(static_cast<char>(0x80 | (code & 0x3F)));
					}
					continue;
				}
				default:
					c = e; // \" \\ \/
				}
			}
			v.push_back(c);
		}
		if (at >= s.size()) {
			return false;
		}
		++at;
		if (out != nullptr) {
			*out = std::move(v);
		}
		return true;
	}
	bool number(double* out) {
		space();
		const char* begin = s.c_str() + at;
		char* end = nullptr;
		const double v = std::strtod(begin, &end);
		if (end == begin) {
			return false;
		}
		at += static_cast<size_t>(end - begin);
		if (out != nullptr) {
			*out = v;
		}
		return true;
	}
	bool skipValue(int depth = 0) {
		if (depth > 32) {
			return false;
		}
		space();
		if (at >= s.size()) {
			return false;
		}
		const char c = s[at];
		if (c == '"') {
			return string(nullptr);
		}
		if (c == '{' || c == '[') {
			const char close = c == '{' ? '}' : ']';
			++at;
			if (eat(close)) {
				return true;
			}
			do {
				if (c == '{') {
					if (!string(nullptr) || !eat(':')) {
						return false;
					}
				}
				if (!skipValue(depth + 1)) {
					return false;
				}
			} while (eat(','));
			return eat(close);
		}
		for (const char* word : { "true", "false", "null" }) {
			const size_t n = std::strlen(word);
			if (s.compare(at, n, word) == 0) {
				at += n;
				return true;
			}
		}
		return number(nullptr);
	}
};

} // namespace

//------------------------------------------------------------------------------------------------//
// WHERE                                                                                            //
//------------------------------------------------------------------------------------------------//

namespace {

std::mutex& chosenRootMutex() {
	static std::mutex m;
	return m;
}

std::filesystem::path& chosenRootValue() {
	static std::filesystem::path chosen;
	return chosen;
}

} // namespace

bool PresetStore::rootOverridden() {
	const char* over = std::getenv("FEATHERS_PRESET_ROOT");
	return over != nullptr && over[0] != '\0';
}

void PresetStore::setChosenRoot(const std::filesystem::path& chosen) {
	const std::lock_guard<std::mutex> lock(chosenRootMutex());
	chosenRootValue() = chosen;
}

std::filesystem::path PresetStore::chosenRoot() {
	const std::lock_guard<std::mutex> lock(chosenRootMutex());
	return chosenRootValue();
}

std::filesystem::path PresetStore::root() {
	if (rootOverridden()) {
		return std::filesystem::path(std::getenv("FEATHERS_PRESET_ROOT"));
	}
	if (std::filesystem::path chosen = chosenRoot(); !chosen.empty()) {
		return chosen;
	}
	return defaultRoot();
}

std::filesystem::path PresetStore::defaultRoot() {
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
		return std::filesystem::path(home) / "Documents" / "Owlet" / "Feathers" / "Presets";
	}
	return {};
}

std::filesystem::path PresetStore::directory(const PresetBank& bank) {
	const std::filesystem::path r = root();
	if (r.empty()) {
		return {};
	}
	return bank.empty() ? r : r / bank;
}

std::string PresetStore::sanitise(const std::string& name) {
	// ASCII: only letters, digits, space, '-' and '_' stay (every other byte, the path separators, the control
	// characters and what Windows forbids in a file name among them, becomes '-'). A BYTE >= 0x80 stays when it is
	// part of well-formed UTF-8, so "Caf\xC3\xA9" and "Caf\xC3\xA8" are two names; a stray byte is a '-'.
	std::string out;
	out.reserve(name.size());
	for (size_t i = 0; i < name.size();) {
		const char c = name[i];
		if (static_cast<unsigned char>(c) >= 0x80) {
			const size_t n = utf8Length(name, i);
			if (n == 0) {
				out.push_back('-');
				++i;
			}
			else {
				out.append(name, i, n);
				i += n;
			}
			continue;
		}
		const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		                  || c == ' ' || c == '-' || c == '_';
		out.push_back(safe ? c : '-');
		++i;
	}
	// Leading and trailing spaces make a filename that is legal and horrible to handle; a name that is
	// nothing but separators is not a name at all.
	const size_t first = out.find_first_not_of(" -_");
	if (first == std::string::npos) {
		return {};
	}
	const size_t last = out.find_last_not_of(' ');
	return out.substr(first, last - first + 1);
}

std::string PresetStore::sanitiseCategory(const std::string& category) {
	std::string out;
	size_t start = 0;
	while (start <= category.size()) {
		size_t slash = category.find_first_of("/\\", start);
		if (slash == std::string::npos) {
			slash = category.size();
		}
		const std::string segment = sanitise(category.substr(start, slash - start));
		if (!segment.empty()) {
			if (!out.empty()) {
				out.push_back('/');
			}
			out += segment;
		}
		start = slash + 1;
	}
	return out;
}

std::pair<std::string, std::string> PresetStore::splitTyped(const std::string& typed) {
	const size_t slash = typed.find_last_of("/\\");
	if (slash == std::string::npos) {
		return { sanitise(typed), {} };
	}
	return { sanitise(typed.substr(slash + 1)), sanitiseCategory(typed.substr(0, slash)) };
}

std::filesystem::path PresetStore::pathFor(const std::string& name, const PresetBank& bank,
                                           const std::string& category) {
	const std::filesystem::path dir = directory(bank);
	const std::string safe = sanitise(name);
	if (dir.empty() || safe.empty()) {
		return {};
	}
	const std::string folder = sanitiseCategory(category);
	// FROM UTF-8, as a path: a narrow string would be read in the Windows ANSI code page and name another file.
	const std::u8string leaf(safe.begin(), safe.end());
	return (folder.empty() ? dir : dir / std::filesystem::path(std::u8string(folder.begin(), folder.end()))) / std::filesystem::path(leaf + u8".fpreset");
}

//------------------------------------------------------------------------------------------------//
// THE FILE                                                                                         //
//------------------------------------------------------------------------------------------------//

std::string PresetStore::metaJson(const PresetMeta& meta) {
	std::string out = "{\"saved\": " + std::to_string(meta.saved) + ", \"tags\": [";
	bool first = true;
	for (const std::string& tag : meta.tags) {
		if (isCategoryTag(tag)) {
			continue; // the field below is the one statement of it
		}
		out += first ? "" : ", ";
		first = false;
		jsonString(out, tag);
	}
	if (const std::string category = sanitiseCategory(meta.category); !category.empty()) {
		out += first ? "" : ", ";
		jsonString(out, std::string(kCategoryNamespace) + ":" + category);
	}
	out += "]}";
	return out;
}

bool PresetStore::parseMetaJson(const std::string& json, PresetMeta* meta) {
	PresetMeta got;
	JsonReader r { json };
	if (!r.eat('{')) {
		return false;
	}
	if (!r.eat('}')) {
		do {
			std::string key;
			if (!r.string(&key) || !r.eat(':')) {
				return false;
			}
			if (key == "saved") {
				double v = 0.0;
				if (!r.number(&v)) {
					return false;
				}
				got.saved = static_cast<int64_t>(v);
			}
			else if (key == "tags") {
				if (!r.eat('[')) {
					return false;
				}
				if (!r.eat(']')) {
					do {
						std::string tag;
						if (!r.string(&tag)) {
							return false;
						}
						if (isCategoryTag(tag)) {
							// THE CATEGORY, OUT OF THE TAGS INTO ITS FIELD — sanitised like one typed.
							got.category = sanitiseCategory(tag.substr(tag.find(':') + 1));
						}
						else {
							got.tags.push_back(std::move(tag));
						}
					} while (r.eat(','));
					if (!r.eat(']')) {
						return false;
					}
				}
			}
			else if (!r.skipValue()) {
				return false; // a member this build does not know, skipped whole
			}
		} while (r.eat(','));
		if (!r.eat('}')) {
			return false;
		}
	}
	if (meta != nullptr) {
		*meta = std::move(got);
	}
	return true;
}

std::vector<uint8_t> PresetStore::encode(const PresetMeta& meta, const std::vector<uint8_t>& payload) {
	const std::string json = metaJson(meta);
	std::vector<uint8_t> out;
	out.reserve(9 + json.size() + payload.size());
	out.insert(out.end(), kMagic, kMagic + 4);
	out.push_back(kFileVersion);
	const auto n = static_cast<uint32_t>(json.size());
	for (int i = 0; i < 4; ++i) {
		out.push_back(static_cast<uint8_t>((n >> (8 * i)) & 0xFFu));
	}
	out.insert(out.end(), json.begin(), json.end());
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

bool PresetStore::decode(const std::vector<uint8_t>& file, PresetMeta* meta, std::vector<uint8_t>* payload) {
	// EITHER OUT-POINTER MAY BE NULL: a caller that wants only the tags passes no payload (backlog KA).
	if (file.size() < 9 || std::memcmp(file.data(), kMagic, 4) != 0) {
		// NO HEADER: a bare payload, from before metadata existed or written by something else.
		if (meta != nullptr) {
			*meta = PresetMeta {};
		}
		if (payload != nullptr) {
			*payload = file;
		}
		return true;
	}
	uint32_t n = 0;
	for (int i = 0; i < 4; ++i) {
		n |= static_cast<uint32_t>(file[5 + static_cast<size_t>(i)]) << (8 * i);
	}
	if (n > kMaxMetaBytes || 9u + n > file.size()) {
		return false;
	}
	const std::string json(reinterpret_cast<const char*>(file.data()) + 9, n);
	// UNREADABLE METADATA COSTS THE TAGS, NOT THE SOUND: the payload is still there and still loads.
	if (!parseMetaJson(json, meta) && meta != nullptr) {
		*meta = PresetMeta {};
	}
	if (payload != nullptr) {
		payload->assign(file.begin() + 9 + n, file.end());
	}
	return true;
}

//------------------------------------------------------------------------------------------------//
// THE BANK                                                                                         //
//------------------------------------------------------------------------------------------------//

std::vector<StoredPreset> PresetStore::loadAll(const PresetBank& bank) {
	std::vector<StoredPreset> out;
	const std::filesystem::path dir = directory(bank);
	if (dir.empty()) {
		return out;
	}
	// EVERY FAILURE HERE IS "NO PRESETS", never an exception out of an editor opening.
	std::error_code ec;
	if (!std::filesystem::is_directory(dir, ec)) {
		return out;
	}
	std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied, ec);
	for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
		const std::filesystem::directory_entry& entry = *it;
		if (hidden(entry.path())) {
			if (entry.is_directory(ec)) {
				it.disable_recursion_pending();
			}
			continue;
		}
		if (entry.is_directory(ec)) {
			if (it.depth() >= kMaxDepth) {
				it.disable_recursion_pending();
			}
			continue;
		}
		if (!entry.is_regular_file(ec) || entry.path().extension() != kExtension) {
			continue;
		}
		const std::vector<uint8_t> file = readFile(entry.path());
		StoredPreset preset;
		if (file.empty() || !decode(file, &preset.meta, &preset.bytes) || preset.bytes.empty()) {
			continue; // a row that loads nothing when clicked would be a lie
		}
		{
			const std::u8string stem = entry.path().stem().u8string();
			preset.name.assign(stem.begin(), stem.end());
		}
		const std::filesystem::path rel = entry.path().parent_path().lexically_relative(dir);
		{
			const std::u8string cat = rel.generic_u8string();
			preset.category = rel == "." ? std::string {} : std::string(cat.begin(), cat.end());
		}
		// THE FOLDER WINS LOCALLY: whatever the metadata says, the preset is where it IS, and its next save
		// writes that (see the header).
		preset.meta.category = preset.category;
		preset.path = entry.path();
		out.push_back(std::move(preset));
	}
	// SORTED, because directory order is whatever the filesystem feels like and a browser whose list
	// reshuffles between runs is one a player cannot learn.
	std::sort(out.begin(), out.end(), [](const StoredPreset& a, const StoredPreset& b) {
		return a.category != b.category ? a.category < b.category : a.name < b.name;
	});
	return out;
}

bool PresetStore::save(const std::string& name, const std::vector<uint8_t>& payload, const PresetBank& bank,
                       const std::string& category, const PresetMeta& meta) {
	const std::filesystem::path file = pathFor(name, bank, category);
	if (file.empty() || payload.empty()) {
		return false;
	}
	// THE FOLDER IT GOES INTO IS THE CATEGORY IT CARRIES, whatever `meta` said.
	PresetMeta written = meta;
	written.category = sanitiseCategory(category);
	return writeFile(file, encode(written, payload));
}

bool PresetStore::installIfMissing(const std::string& name, const std::vector<uint8_t>& payload,
                                   const PresetBank& bank, const std::string& category, const PresetMeta& meta) {
	const std::filesystem::path file = pathFor(name, bank, category);
	std::error_code ec;
	if (file.empty() || payload.empty() || std::filesystem::exists(file, ec)) {
		return false; // an edited copy is the user's: never touched
	}
	PresetMeta written = meta;
	written.category = sanitiseCategory(category);
	return writeFile(file, encode(written, payload));
}

int PresetStore::installFactoryFiles(const PresetBank& bank, std::span<const FactoryPresetFile> files) {
	const std::filesystem::path r = root();
	if (r.empty() || bank.empty()) {
		return 0;
	}
	const std::string prefix = bank + "/";
	int wrote = 0;
	for (const FactoryPresetFile& f : files) {
		if (!f.path.starts_with(prefix) || f.bytes.empty()) {
			continue;
		}
		std::filesystem::path file = r;
		bool usable = true;
		for (const std::filesystem::path& part : std::filesystem::path(std::string(f.path))) {
			const std::string s = part.string();
			usable = usable && !s.empty() && s != "." && s != ".." && s != "/";
			file /= part;
		}
		std::error_code ec;
		if (!usable || std::filesystem::exists(file, ec)) {
			continue; // an edited copy is the user's: never touched
		}
		if (writeFile(file, std::vector<uint8_t>(f.bytes.begin(), f.bytes.end()))) {
			++wrote;
		}
	}
	return wrote;
}

bool PresetStore::rename(const std::filesystem::path& from, const std::filesystem::path& to) {
	std::error_code ec;
	if (from.empty() || to.empty() || std::filesystem::exists(to, ec) || !std::filesystem::exists(from, ec)) {
		return false;
	}
	std::filesystem::create_directories(to.parent_path(), ec);
	std::filesystem::rename(from, to, ec);
	return !ec;
}

bool PresetStore::erase(const std::filesystem::path& file) {
	std::error_code ec;
	return !file.empty() && std::filesystem::remove(file, ec);
}

bool PresetStore::erase(const std::string& name, const PresetBank& bank, const std::string& category) {
	return erase(pathFor(name, bank, category));
}

} // namespace feathers
