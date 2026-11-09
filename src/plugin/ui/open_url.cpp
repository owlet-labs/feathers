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

#include "plugin/ui/open_url.h"

#include <string_view>

#if defined(_WIN32)
#include <windows.h>

#include <shellapi.h>
#else
#include <thread>

#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace feathers::ui {

bool openInBrowser(const char* url) {
	if (url == nullptr || std::string_view(url) != kPlugUrl) {
		return false;
	}
#if defined(_WIN32)
	(void)ShellExecuteW(nullptr, L"open", L"https://soundcloud.com/greatowlmusic", nullptr, nullptr, SW_SHOWNORMAL);
#else
#if defined(__APPLE__)
	const char* opener = "/usr/bin/open";
#else
	const char* opener = "xdg-open";
#endif
	char* const argv[] = { const_cast<char*>(opener), const_cast<char*>(url), nullptr };
	pid_t pid = 0;
	// posix_spawnp: PATH lookup for xdg-open, no shell, fixed argv.
	if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) == 0) {
		// Reaped off the main thread, so no zombie outlives the opener and nothing here ever blocks.
		std::thread([pid] {
			int status = 0;
			(void)waitpid(pid, &status, 0);
		}).detach();
	}
#endif
	return true;
}

} // namespace feathers::ui
