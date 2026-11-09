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

#include "plugin/event_log.h"

#include "feathers_build_stamp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#include <process.h>
#include <shlobj.h>
#include <windows.h>
#else
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <climits>
#include <pwd.h>
#include <sysdir.h>
#endif

namespace feathers::eventlog {

namespace {

std::atomic<bool> gArmed { false };
std::atomic<uint32_t> gInstances { 0 };
std::atomic<const char*> gFormat { "clap" };

long processId() noexcept {
#if defined(_WIN32)
	return static_cast<long>(_getpid());
#else
	return static_cast<long>(::getpid());
#endif
}

/// Local wall-clock time to the millisecond, with its UTC offset — a line has to be placeable against the
/// host's own log and against a memory of the evening, and both are local.
std::string timestamp() {
	const auto now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	const auto millis =
	    std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &seconds);
#else
	localtime_r(&seconds, &local);
#endif
	char date[32];
	char zone[16];
	if (std::strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local) == 0) {
		date[0] = '\0';
	}
	if (std::strftime(zone, sizeof(zone), "%z", &local) == 0) {
		zone[0] = '\0';
	}
	char out[64];
	std::snprintf(out, sizeof(out), "%s.%03d %s", date, static_cast<int>(millis), zone);
	return out;
}

} // namespace

bool armed() noexcept {
	return gArmed.load(std::memory_order_relaxed);
}

void arm() noexcept {
	gArmed.store(true, std::memory_order_relaxed);
}

void setFormat(const char* name) noexcept {
	if (name != nullptr) {
		gFormat.store(name, std::memory_order_relaxed);
	}
}

const char* format() noexcept {
	return gFormat.load(std::memory_order_relaxed);
}

std::filesystem::path resolvePath(const char* override, const std::filesystem::path& platformDefault, bool armed) {
	if (override != nullptr && override[0] != '\0') {
		return std::string(override) == "off" ? std::filesystem::path {} : std::filesystem::path(override);
	}
	return armed ? platformDefault : std::filesystem::path {};
}

std::filesystem::path xdgPath(const char* xdgStateHome, const char* home) {
	// XDG: a relative value is invalid and is ignored, as the spec says it must be.
	if (xdgStateHome != nullptr && xdgStateHome[0] == '/') {
		return std::filesystem::path(xdgStateHome) / "feathers" / "events.log";
	}
	if (home != nullptr && home[0] != '\0') {
		return std::filesystem::path(home) / ".local" / "state" / "feathers" / "events.log";
	}
	return {};
}

std::filesystem::path platformDefaultPath() {
#if defined(__APPLE__)
	// THE USER LIBRARY, AS THE SYSTEM NAMES IT — `sysdir` hands back "~/Library" with the tilde unexpanded, which
	// is expanded from the account's own home (HOME first, the password database if it is unset).
	char library[PATH_MAX] = {};
	sysdir_search_path_enumeration_state state =
	    sysdir_start_search_path_enumeration(SYSDIR_DIRECTORY_LIBRARY, SYSDIR_DOMAIN_MASK_USER);
	if (sysdir_get_next_search_path_enumeration(state, library) == 0 || library[0] == '\0') {
		return {};
	}
	std::string dir(library);
	if (dir[0] == '~') {
		const char* home = std::getenv("HOME");
		if (home == nullptr || home[0] == '\0') {
			const passwd* pw = ::getpwuid(::getuid());
			home = pw != nullptr ? pw->pw_dir : nullptr;
		}
		if (home == nullptr) {
			return {};
		}
		dir = std::string(home) + dir.substr(1);
	}
	return std::filesystem::path(dir) / "Logs" / "Feathers" / "events.log";
#elif defined(_WIN32)
	PWSTR local = nullptr;
	std::filesystem::path out;
	if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local != nullptr) {
		out = std::filesystem::path(local) / "Feathers" / "events.log";
	}
	CoTaskMemFree(local);
	return out;
#else
	return xdgPath(std::getenv("XDG_STATE_HOME"), std::getenv("HOME"));
#endif
}

std::filesystem::path currentPath() {
	const char* override = std::getenv(kEnvVar);
	if (override != nullptr && override[0] != '\0') {
		return resolvePath(override, {}, false);
	}
	return gArmed.load(std::memory_order_relaxed) ? platformDefaultPath() : std::filesystem::path {};
}

std::string initLine(const char* hostName, const char* hostVersion, const char* hostVendor) {
	const auto text = [](const char* s) { return std::string(s != nullptr ? s : "?"); };
	return "init format=" + std::string(format()) + " host=\"" + text(hostName) + "\" " + text(hostVersion) + " vendor=\""
	       + text(hostVendor) + "\"";
}

const char* buildSha() noexcept {
	return FEATHERS_BUILD_SHA;
}

void write(const char* pluginId, const std::string& instance, const std::string& event) noexcept {
	try {
		namespace fs = std::filesystem;
		const fs::path file = currentPath();
		if (file.empty()) {
			return;
		}
		std::error_code ec;
		if (file.has_parent_path()) {
			fs::create_directories(file.parent_path(), ec);
		}
		// ROTATED BEFORE THE LINE, so the line that crosses the cap starts the new file rather than ending
		// the old one. A rename over the previous `.1` is the whole of the history kept: two files, ~2 MB.
		if (const uintmax_t size = fs::file_size(file, ec); !ec && size >= kRotateBytes) {
			fs::path older = file;
			older += ".1";
			fs::rename(file, older, ec);
		}
		const std::string line = timestamp() + " " + (pluginId != nullptr ? pluginId : "?") + " " + instance + " "
		                         + buildSha() + " " + event + "\n";
		// ONE WRITE OF ONE WHOLE LINE, append mode: two processes (a host may run plugins in several) writing
		// at once interleave lines, not characters. A path, not a string, so Windows opens a wide name.
		std::ofstream out(file, std::ios::binary | std::ios::app);
		if (out) {
			out.write(line.data(), static_cast<std::streamsize>(line.size()));
		}
	} catch (...) {
		// SILENT: a log that cannot be written costs the line, never the plugin.
	}
}

Instance::Instance(const char* pluginId) : pluginId_(pluginId) {
	tag_ = std::to_string(processId()) + "." + std::to_string(gInstances.fetch_add(1, std::memory_order_relaxed) + 1);
}

void Instance::say(const std::string& event) const noexcept {
	write(pluginId_, tag_, event);
}

} // namespace feathers::eventlog
