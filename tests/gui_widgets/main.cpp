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

// THE ONE main() FOR EVERY WIDGET BINARY — listed as a source of each of the six rather than
// archived in the fixture library, so that no linker search order decides whether it is present.

// OUR OWN main(), for one reason: THIS BINARY MUST NOT TOUCH THE DEVELOPER'S REAL DESK. The editor
// remembers how it is arranged in $XDG_CONFIG_HOME/feathers/editor.conf (see editor_prefs.h), and
// every case here builds a real editor. Left alone, a test run would read the user's preferences — so a
// rail they had closed would decide what these cases measure — and the cases that toggle one would
// write to them. Pointing XDG_CONFIG_HOME at a temporary directory before any case runs makes the
// whole binary answer out of a desk of its own.
// DOCTEST_CONFIG_IMPLEMENT IS NOT HERE ANY MORE: the implementation is gui_widgets/doctest_impl.cpp,
// compiled once in the fixture library. Measured serially with ccache off, under ASan, this file
// costs 0.92 s without it and 10.6–11.8 s with it — six binaries paid ~63 s of compile for one
// framework. This file is the ENTRY POINT and the clean-desk reporter, and it stays a source of
// each executable so that no linker search order decides whether a binary has a main().
// NOT fixtures.h: that header is the 88-include floor of the suite (MEASURED 22–24 s per TU on
// this host, 2026-09-03), and this file needs none of it -- only doctest, the filesystem, and
// where the editor keeps its preferences. Paid once per binary otherwise, six times per header edit.
#include "doctest.h"
#include "plugin/editor_prefs.h"
#include "plugin/factory_presets.h"
#include "plugin/ui/drag_acceleration.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

/// EVERY CASE STARTS AT A CLEAN DESK. The editor now remembers how it was arranged, which is the
/// point of it — and inside one test binary that memory is shared by every case that follows.
/// Without this, a case that closes a rail silently rearranges the editor of every case after it,
/// which is exactly what happened the first time this ran: nine failures in surfaces that had never
/// heard of rails.
///
/// Deleting the file rather than resetting the objects, because the file IS the state a fresh editor
/// reads — see editor_prefs.h. One listener rather than a line in each fixture, so a case written
/// next year gets the clean desk without knowing to ask for one.
struct CleanDesk : doctest::IReporter {
	explicit CleanDesk(const doctest::ContextOptions&) {}

	void test_case_start(const doctest::TestCaseData&) override { clear(); }

	/// AND ON EVERY SUBCASE PASS, because doctest re-runs the whole body once per subcase and each of
	/// those passes is a fresh editor reading the same file. Without this, a case that folds a card
	/// before its SUBCASE line hands the next pass an editor that starts folded — which is exactly
	/// what happened: the fold case's own REQUIRE_FALSE tripped on state its first pass had written.
	void test_case_reenter(const doctest::TestCaseData&) override { clear(); }

	/// AND A STOPPED DRAG CLOCK (backlog QB). A value-box drag accelerates with the hand's speed measured against a
	/// clock, and a case's synthetic press and drag are microseconds apart — or, on a loaded host, a preempted 10 ms
	/// apart, which reads as a flick. A stopped clock reads every synthetic gesture as slow, so the cases written
	/// before acceleration keep asserting the digit's own rate deterministically; the acceleration cases set their own.
	static void clear() {
		feathers::ui::drag_accel::setClockForTest([] { return 0.0; });
		std::error_code code;
		std::filesystem::remove(feathers::EditorPrefs::path(), code);
	}

	void report_query(const doctest::QueryData&) override {}
	void test_run_start() override {}
	void test_run_end(const doctest::TestRunStats&) override {}
	void test_case_end(const doctest::CurrentTestCaseStats&) override {}
	void test_case_exception(const doctest::TestCaseException&) override {}
	void subcase_start(const doctest::SubcaseSignature&) override {}
	void subcase_end() override {}
	void log_assert(const doctest::AssertData&) override {}
	void log_message(const doctest::MessageData&) override {}
	void test_case_skipped(const doctest::TestCaseData&) override {}
};

DOCTEST_REGISTER_REPORTER("clean-desk", 0, CleanDesk);

} // namespace
int main(int argc, char** argv) {
	// Before any case, because a static initialiser or a fixture could otherwise get there first.
	const std::filesystem::path desk =
	    std::filesystem::temp_directory_path()
	    / ("feathers-test-config-" + std::to_string(static_cast<long>(::getpid())));
	std::error_code code;
	std::filesystem::create_directories(desk, code);
	::setenv("XDG_CONFIG_HOME", desk.c_str(), 1);
	// AND THE PRESET LIBRARY, for the same reason and to the same temporary place. presets are XDG
	// DATA rather than config — see preset_store.h — so redirecting the config variable alone would
	// have left every case that saves a preset writing into the developer's real library.
	const std::filesystem::path library =
	    std::filesystem::temp_directory_path()
	    / ("feathers-test-data-" + std::to_string(static_cast<long>(::getpid())));
	std::filesystem::create_directories(library, code);
	::setenv("XDG_DATA_HOME", library.c_str(), 1);
	// AND THE PRESET ROOT ITSELF, which since backlog FO phase 2 is NOT under XDG data but
	// ~/Documents/Owlet/Feathers/Presets — so the redirect above no longer covers it, and without this
	// line every case that saves would write into the developer's real Documents folder.
	::setenv("FEATHERS_PRESET_ROOT", (library / "presets").c_str(), 1);
	// AND THE IMPULSE FOLDER (backlog JD): Featherverb's panel makes ~/Documents/Owlet/Feathers/IRs when its browser
	// first reads it, and a case that opens the panel without its own override made one in a real home (2026-09-24).
	::setenv("FEATHERS_IR_ROOT", (library / "irs").c_str(), 1);
	// AND NO SHIPPED FACTORY PRESETS: presets/factory changes whenever a preset is tagged `factory`, and a
	// case counting a bank's entries must not change with it. The factory cases pass their own set.
	feathers::factoryPresetSource = [] { return std::span<const feathers::FactoryPresetFile> {}; };

	doctest::Context context;
	context.applyCommandLine(argc, argv);
	// The listener above is a reporter in doctest's vocabulary, and reporters are opt-in by name. It
	// reports nothing; it is registered for its per-case hook.
	context.addFilter("reporters", "console,clean-desk");
	const int result = context.run();

	std::filesystem::remove_all(desk, code);
	return result;
}
