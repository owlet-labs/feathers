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

#include "plugin/editor_prefs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace feathers {

namespace {

/// The one place a key/value line is understood, so reading and writing cannot disagree about what a
/// line is. Everything after the first `=` is the value, including further `=` signs: keys are ours
/// and contain none, values may one day.
bool splitLine(const std::string& line, std::string* key, std::string* value) {
	const std::string::size_type hash = line.find('#');
	const std::string trimmed = hash == std::string::npos ? line : line.substr(0, hash);
	const std::string::size_type equals = trimmed.find('=');
	if (equals == std::string::npos) {
		return false;
	}
	const auto strip = [](std::string text) {
		const std::string::size_type first = text.find_first_not_of(" \t\r\n");
		if (first == std::string::npos) {
			return std::string {};
		}
		const std::string::size_type last = text.find_last_not_of(" \t\r\n");
		return text.substr(first, last - first + 1);
	};
	*key = strip(trimmed.substr(0, equals));
	*value = strip(trimmed.substr(equals + 1));
	return !key->empty();
}

} // namespace

namespace {

/// **A TEST BINARY THAT REACHES THE REAL STORE IS STOPPED, NOT TRUSTED** (backlog Q). Every test executable links
/// `tests/test_sandbox.cpp`, which records the home it started in as `FEATHERS_TEST_REAL_HOME` and points XDG at a
/// scratch desk before any case runs. If a path resolves to that home's editor.conf anyway — a binary whose own
/// main put XDG back, a case that unset it — the process ends here, before the read or the write, naming the file.
/// A plugin in a host never has the variable, so this costs it one getenv.
std::filesystem::path refuseRealStoreUnderTest(std::filesystem::path file) {
	const char* real = std::getenv("FEATHERS_TEST_REAL_HOME");
	if (real == nullptr || real[0] == '\0' || file.empty()) {
		return file;
	}
	const std::filesystem::path forbidden = std::filesystem::path(real) / ".config" / "feathers" / "editor.conf";
	std::error_code code;
	const bool same = file.lexically_normal() == forbidden.lexically_normal()
	                  || std::filesystem::weakly_canonical(file, code) == std::filesystem::weakly_canonical(forbidden, code);
	if (same) {
		std::fprintf(stderr,
		             "FEATHERS TEST SANDBOX: a test reached the user's REAL preferences store %s — "
		             "point XDG_CONFIG_HOME at a scratch desk (backlog Q)\n",
		             file.string().c_str());
		std::fflush(stderr);
		std::abort();
	}
	return file;
}

} // namespace

std::filesystem::path EditorPrefs::path() {
	// XDG first, and it is believed as given — a caller who sets it has said where their configuration
	// lives, which is exactly what the variable is for.
	if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') {
		return refuseRealStoreUnderTest(std::filesystem::path(xdg) / "feathers" / "editor.conf");
	}
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
		return refuseRealStoreUnderTest(std::filesystem::path(home) / ".config" / "feathers" / "editor.conf");
	}
	// No home, no preferences. The editor works exactly as it did before this file existed.
	return {};
}

void EditorPrefs::load() {
	loaded_ = true;
	values_.clear();
	const std::filesystem::path file = path();
	if (file.empty()) {
		return;
	}
	std::ifstream in(file);
	if (!in) {
		return;
	}
	std::string line;
	while (std::getline(in, line)) {
		std::string key;
		std::string value;
		// A line that is not a pair is skipped rather than failing the read: half a readable file is
		// worth more than none, and the alternative is losing every preference to one bad line.
		if (splitLine(line, &key, &value)) {
			values_[key] = value;
		}
	}
}

bool EditorPrefs::flag(std::string_view key, bool fallback) const {
	const auto found = values_.find(key);
	if (found == values_.end()) {
		return fallback;
	}
	// Written as 1/0 and read a little more generously, because this file is meant to be edited by
	// hand and somebody typing `true` means it.
	const std::string& value = found->second;
	if (value == "1" || value == "true" || value == "yes") {
		return true;
	}
	if (value == "0" || value == "false" || value == "no") {
		return false;
	}
	return fallback;
}

void EditorPrefs::setFlag(std::string_view key, bool value) {
	if (!loaded_) {
		// Never write over a file we have not read. Otherwise the first toggle of a session would
		// replace everything stored with the single key it happened to be about.
		load();
	}
	const std::string text = value ? "1" : "0";
	const auto found = values_.find(key);
	if (found != values_.end() && found->second == text) {
		return;
	}
	values_[std::string(key)] = text;
	save();
}

void EditorPrefs::save() const {
	const std::filesystem::path file = path();
	if (file.empty()) {
		return;
	}
	std::error_code code;
	std::filesystem::create_directories(file.parent_path(), code);
	if (code) {
		return;
	}
	// ATOMIC OR NOT AT ALL. The temporary carries this process's id so two editors saving at once
	// cannot write through each other's file, and the rename is what a reader sees — a rename within
	// a directory either happened or did not, so nobody can read a half-written preferences file.
	std::filesystem::path temporary = file;
	#if defined(_WIN32)
	temporary += ".tmp-" + std::to_string(static_cast<long>(_getpid()));
#else
	temporary += ".tmp-" + std::to_string(static_cast<long>(::getpid()));
#endif
	{
		std::ofstream out(temporary, std::ios::trunc);
		if (!out) {
			return;
		}
		out << "# Feathers editor preferences. How your editor is arranged, and nothing about sound.\n";
		out << "# Written by the plugin; safe to edit by hand or to delete, which restores the\n";
		out << "# defaults. Keys this build does not know are kept as they are.\n";
		for (const auto& [key, value] : values_) {
			out << key << '=' << value << '\n';
		}
		if (!out) {
			out.close();
			std::filesystem::remove(temporary, code);
			return;
		}
	}
	std::filesystem::rename(temporary, file, code);
	if (code) {
		// Nothing to say and nobody to say it to — see the header. The temporary goes rather than
		// being left beside the real file for somebody to find later and wonder about.
		std::filesystem::remove(temporary, code);
	}
}

double EditorPrefs::number(std::string_view key, double fallback) const {
	const auto found = values_.find(key);
	if (found == values_.end()) {
		return fallback;
	}
	// `strtod` on the stored text, whole-string: a trailing character that is not part of a number
	// means the line is not one, and the fallback answers — a preference is never a guess.
	const std::string& text = found->second;
	char* end = nullptr;
	const double value = std::strtod(text.c_str(), &end);
	if (end == text.c_str() || end == nullptr || *end != '\0' || !std::isfinite(value)) {
		return fallback;
	}
	return value;
}

void EditorPrefs::setNumber(std::string_view key, double value) {
	if (!loaded_) {
		load();
	}
	if (!std::isfinite(value)) {
		return;
	}
	// Six significant digits: enough for any scale a hand would choose, short enough to read in the file.
	char text[32];
	std::snprintf(text, sizeof(text), "%.6g", value);
	const auto found = values_.find(key);
	if (found != values_.end() && found->second == text) {
		return;
	}
	values_[std::string(key)] = text;
	save();
}

namespace {

/// `%XX` for the bytes a line cannot carry as they are — see `EditorPrefs::text`.
std::string escapeText(const std::string& value) {
	static constexpr char kHex[] = "0123456789ABCDEF";
	std::string out;
	out.reserve(value.size());
	for (const char c : value) {
		const auto byte = static_cast<unsigned char>(c);
		if (byte <= 0x20 || byte == 0x7f || c == '%' || c == '#') {
			out.push_back('%');
			out.push_back(kHex[byte >> 4]);
			out.push_back(kHex[byte & 0x0f]);
		}
		else {
			out.push_back(c);
		}
	}
	return out;
}

int hexDigit(char c) {
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

/// The inverse. A `%` not followed by two hex digits is kept as it is: a hand-edited line is read, not refused.
std::string unescapeText(const std::string& text) {
	std::string out;
	out.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == '%' && i + 2 < text.size()) {
			const int hi = hexDigit(text[i + 1]);
			const int lo = hexDigit(text[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(text[i]);
	}
	return out;
}

} // namespace

std::string EditorPrefs::text(std::string_view key, const std::string& fallback) const {
	const auto found = values_.find(key);
	if (found == values_.end() || found->second.empty()) {
		return fallback;
	}
	return unescapeText(found->second);
}

void EditorPrefs::setText(std::string_view key, const std::string& value) {
	if (!loaded_) {
		load();
	}
	const auto found = values_.find(key);
	if (value.empty()) {
		if (found == values_.end()) {
			return;
		}
		values_.erase(found);
		save();
		return;
	}
	const std::string stored = escapeText(value);
	if (found != values_.end() && found->second == stored) {
		return;
	}
	values_[std::string(key)] = stored;
	save();
}

} // namespace feathers
