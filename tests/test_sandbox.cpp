/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// EVERY TEST EXECUTABLE STARTS ON A DESK OF ITS OWN (backlog Q). Linked into each one by `tests/CMakeLists.txt`
// (as an OBJECT library, so the linker cannot drop it), this runs before `main` and before any case:
//
//   - it records the home the process started in as FEATHERS_TEST_REAL_HOME — not overwriting one a parent test
//     process already recorded — and `EditorPrefs::path()` ABORTS if it ever resolves to that home's editor.conf.
//     Reaching the user's real preferences from a test is an error, not a hazard;
//   - it points XDG_CONFIG_HOME, XDG_DATA_HOME and XDG_STATE_HOME at a per-process scratch directory when they are
//     unset or name the real home's own, so a binary with no main of its own (most of them) and a binary run BARE
//     (`run-cases.sh`, a debugger) are covered, not only one run through ctest or the venue.
//
// WHY BOTH: the redirect is what keeps a test off the real store; the abort is what makes a test that gets past the
// redirect loud. The section's history is a redirect in one binary's main and none in its neighbours.
// A value already pointing somewhere else (venue-xvfb's scratch desk, a test's own) is left alone.

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#if !defined(_WIN32) // XDG is a POSIX desk; Windows keeps its preferences elsewhere and has no setenv
#include <unistd.h>

namespace {

struct TestSandbox {
	std::filesystem::path desk;

	TestSandbox() {
		const char* home = std::getenv("HOME");
		if (home == nullptr || home[0] == '\0') {
			return; // no home, nothing real to reach — EditorPrefs persists nothing
		}
		::setenv("FEATHERS_TEST_REAL_HOME", home, /*overwrite=*/0);
		const std::filesystem::path real(std::getenv("FEATHERS_TEST_REAL_HOME"));
		desk = std::filesystem::temp_directory_path()
		       / ("feathers-test-sandbox-" + std::to_string(static_cast<long>(::getpid())));
		redirect("XDG_CONFIG_HOME", real / ".config", desk / "config");
		redirect("XDG_DATA_HOME", real / ".local" / "share", desk / "data");
		redirect("XDG_STATE_HOME", real / ".local" / "state", desk / "state");
	}

	~TestSandbox() {
		if (!desk.empty()) {
			std::error_code code;
			std::filesystem::remove_all(desk, code);
		}
	}

	static void redirect(const char* name, const std::filesystem::path& realDefault, const std::filesystem::path& to) {
		const char* now = std::getenv(name);
		if (now != nullptr && now[0] != '\0') {
			std::error_code code;
			const bool isReal = std::filesystem::path(now).lexically_normal() == realDefault.lexically_normal()
			                    || std::filesystem::weakly_canonical(now, code)
			                           == std::filesystem::weakly_canonical(realDefault, code);
			if (!isReal) {
				return;
			}
		}
		::setenv(name, to.c_str(), 1);
	}
};

const TestSandbox sandbox;

} // namespace

#endif
