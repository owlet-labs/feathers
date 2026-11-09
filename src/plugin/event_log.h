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

// THE PLUGIN'S EVENT LOG — a plain text file of what the host did to each instance and what the instance
// did about it: create, init, activate, every state save and load with its size and outcome, the preset
// browser's loads, the editor opening and closing. In EVERY build, not only a diagnostic one.
//
// WHY IT EXISTS: "the plugin seems to spontaneously forget its state and unexpectedly
// re-initialize. super frustrating and rare. I wish I had more context about when it does this." A rare
// event in a host is only ever seen after the fact, so the record has to have been running already. A load
// that failed says WHICH stage failed and why; a process that died says so by the absence of its `destroy`
// line and a new pid on the next `create`.
//
// WHERE, BY PLATFORM — each directory found by the platform's own call, never a spelled-out home:
// - macOS: `~/Library/Logs/Feathers/events.log` (the user Library from `sysdir`), which Console.app shows
//   under Log Reports. The first sighting was on a Mac.
// - Windows: `%LOCALAPPDATA%\Feathers\events.log` (`SHGetKnownFolderPath(FOLDERID_LocalAppData)`).
// - Linux: `$XDG_STATE_HOME/feathers/events.log`, else `~/.local/state/feathers/events.log`. **A FLATPAK HOST
//   (Bitwig) SETS `XDG_STATE_HOME` TO ITS OWN SANDBOX**, so there the file is
//   `~/.var/app/<app id>/.local/state/feathers/events.log`.
// Rotated to `events.log.1` at `kRotateBytes`.
//
// THE RULES IT KEEPS:
// - MAIN THREAD ONLY. It opens a file and allocates; nothing on the audio thread may call it.
// - SILENT. A file that cannot be opened or written costs the line, never the plugin.
// - TESTS AND DEV TOOLS NEVER WRITE THE USER'S FILE. `FEATHERS_EVENT_LOG=off` turns it off and
//   `FEATHERS_EVENT_LOG=<path>` sends it elsewhere; every test binary and every tool host gets `off`
//   (tests/event_log_off.cpp, linked into each — and the ctest ENVIRONMENT too). And the DEFAULT file is
//   written only once a CLAP entry's `init` has armed it — a test that constructs a plugin directly never does.

#include <cstdint>
#include <filesystem>
#include <string>

namespace feathers::eventlog {

/// The size at which the file is moved to `events.log.1` (replacing the previous one) and a new one started.
inline constexpr uint64_t kRotateBytes = 1024 * 1024;

/// The environment variable: `off`, a path, or unset for the default file.
inline constexpr const char* kEnvVar = "FEATHERS_EVENT_LOG";

/// Called by each CLAP entry's `init`: from here on, an unset `FEATHERS_EVENT_LOG` means the default file.
void arm() noexcept;
/// Whether `arm` has been called: other per-user files (the likes log) take their default only once it has.
bool armed() noexcept;

/// **WHICH FORMAT IS HOSTING THIS BINARY** — "clap", "vst3" (clap-wrapper's VST3), "standalone" — set once by
/// the per-format entry file (`feathers_entry.cpp`, `rack_entry.cpp`), which is the one TU compiled per format.
void setFormat(const char* format) noexcept;
const char* format() noexcept;

/// WHERE A LINE GOES, as a pure function of the inputs (empty: nowhere). `override` is the variable's value
/// (null when unset); `platformDefault` is where this platform keeps logs (empty if it could not be found).
std::filesystem::path resolvePath(const char* override, const std::filesystem::path& platformDefault, bool armed);

/// THE LINUX RULE, pure: `$XDG_STATE_HOME/feathers/events.log`, else `<home>/.local/state/feathers/events.log`.
std::filesystem::path xdgPath(const char* xdgStateHome, const char* home);

/// This platform's default file, from the platform's own directory lookup (see the banner).
std::filesystem::path platformDefaultPath();

/// The destination now, from the real environment.
std::filesystem::path currentPath();

/// The build's commit (`git describe --always --dirty`, or CI's GITHUB_SHA) as stamped at build time.
const char* buildSha() noexcept;

/// THE `init` LINE: `init format=<format> host="<name>" <version> vendor="<vendor>"`. A host wrapped by clap-wrapper
/// (the VST3) reports the DAW's name and the vendor "The CLAP Wrapper Team", so the two say the same thing twice.
std::string initLine(const char* hostName, const char* hostVersion, const char* hostVendor);

/// One line for one instance: `<local time> <pluginId> <instance> <build> <event>`. Main thread only.
void write(const char* pluginId, const std::string& instance, const std::string& event) noexcept;

/// **ONE INSTANCE'S VOICE IN THE LOG** — the plugin id and an instance tag (`<pid>.<n>`, `n` counting the
/// instances this process has made) fixed at construction, so every line from one instance carries the same
/// two words and lines from two instances, or two processes, cannot be confused.
class Instance {
public:
	explicit Instance(const char* pluginId);
	void say(const std::string& event) const noexcept;
	const std::string& tag() const noexcept { return tag_; }

private:
	const char* pluginId_;
	std::string tag_;
};

} // namespace feathers::eventlog
