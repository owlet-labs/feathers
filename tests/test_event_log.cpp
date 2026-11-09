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

// THE EVENT LOG'S OWN CONTRACT (src/plugin/event_log.h): where a line goes, what one looks like, the size
// cap, and that nothing about it can fail loudly. Every case writes to a scratch file of its own; the
// user's file is reached only through `resolvePath`, a pure function, with made-up homes.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "plugin/event_log.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace feathers::eventlog;

struct Scratch {
	fs::path dir;
	Scratch() {
		dir = fs::temp_directory_path() / ("feathers-event-log-test-" + std::to_string(::getpid()));
		fs::remove_all(dir);
		fs::create_directories(dir);
	}
	~Scratch() {
		std::error_code ec;
		fs::remove_all(dir, ec);
		::setenv(kEnvVar, "off", 1);
	}
	std::string file(const char* name) const { return (dir / name).string(); }
};

std::string slurp(const std::string& path) {
	std::ifstream in(path);
	return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("the log goes where the variable says, and nowhere when it says off") {
	CHECK(resolvePath("off", "/x/state/feathers/events.log", true).empty());
	CHECK(resolvePath("/tmp/elsewhere.log", "/x/state/feathers/events.log", false) == fs::path("/tmp/elsewhere.log"));
	CHECK(resolvePath("/tmp/elsewhere.log", "/x/state/feathers/events.log", true) == fs::path("/tmp/elsewhere.log"));
}

TEST_CASE("unset, the default is the platform's — and only once a CLAP entry armed it") {
	CHECK(resolvePath(nullptr, "/x/default.log", true) == fs::path("/x/default.log"));
	CHECK(resolvePath("", "/x/default.log", true) == fs::path("/x/default.log"));
	// NOT ARMED: a test that builds a plugin without the entry never reaches the user's file.
	CHECK(resolvePath(nullptr, "/x/default.log", false).empty());
}

TEST_CASE("the Linux default is XDG_STATE_HOME, else ~/.local/state; a relative XDG value is ignored") {
	CHECK(xdgPath("/x/state", "/home/u") == fs::path("/x/state/feathers/events.log"));
	CHECK(xdgPath(nullptr, "/home/u") == fs::path("/home/u/.local/state/feathers/events.log"));
	CHECK(xdgPath("relative/state", "/home/u") == fs::path("/home/u/.local/state/feathers/events.log"));
	CHECK(xdgPath(nullptr, nullptr).empty());
}

TEST_CASE("the platform's default file is where the platform keeps logs") {
	const fs::path p = platformDefaultPath();
	REQUIRE(!p.empty());
	CHECK(p.filename() == "events.log");
#if defined(__APPLE__)
	// ~/Library/Logs/Feathers — what Console.app shows under Log Reports.
	CHECK(p.parent_path().filename() == "Feathers");
	CHECK(p.parent_path().parent_path().filename() == "Logs");
	CHECK(p.parent_path().parent_path().parent_path().filename() == "Library");
	CHECK(p.string().find('~') == std::string::npos);
#elif defined(_WIN32)
	CHECK(p.parent_path().filename() == "Feathers");
#else
	CHECK(p.parent_path().filename() == "feathers");
#endif
	CHECK(p.is_absolute());
}

TEST_CASE("the format defaults to clap and is what the entry file says") {
	CHECK(std::string(format()) == "clap");
	setFormat("vst3");
	CHECK(std::string(format()) == "vst3");
	setFormat(nullptr);
	CHECK(std::string(format()) == "vst3");
	setFormat("clap");
}

TEST_CASE("this test binary starts with the log off, before any case sets it") {
	// tests/event_log_off.cpp, linked into every test binary — or ctest's ENVIRONMENT, which says the same.
	const char* value = std::getenv(kEnvVar);
	REQUIRE(value != nullptr);
	CHECK(std::string(value) == "off");
	arm();
	CHECK(currentPath().empty());
}

TEST_CASE("one line per event: local time, plugin id, pid.instance, build, event") {
	const Scratch scratch;
	const std::string path = scratch.file("events.log");
	::setenv(kEnvVar, path.c_str(), 1);
	const Instance a("com.owlet.feathers");
	const Instance b("com.owlet.feathers-chain");
	a.say("create");
	b.say("state-load ok bytes=12 version=49");
	const std::string text = slurp(path);
	const std::string pid = std::to_string(::getpid());
	const std::regex line(R"(^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} [+-]\d{4} (\S+) (\d+)\.(\d+) (\S+) (.*)$)");
	std::smatch m;
	const std::string first = text.substr(0, text.find('\n'));
	const std::string second = text.substr(text.find('\n') + 1, text.rfind('\n') - text.find('\n') - 1);
	REQUIRE(std::regex_match(first, m, line));
	CHECK(m[1] == "com.owlet.feathers");
	CHECK(m[2] == pid);
	CHECK(m[4] == std::string(buildSha()));
	CHECK(m[5] == "create");
	const std::string firstInstance = m[3];
	REQUIRE(std::regex_match(second, m, line));
	CHECK(m[1] == "com.owlet.feathers-chain");
	CHECK(m[5] == "state-load ok bytes=12 version=49");
	// TWO INSTANCES, TWO TAGS — the counter, not the pid, tells them apart.
	CHECK(std::stoi(std::string(m[3])) == std::stoi(firstInstance) + 1);
	CHECK(a.tag() != b.tag());
	CHECK(std::string(buildSha()) != "");
}

TEST_CASE("the file is rotated to .1 at the cap, and the line that crosses it starts the new file") {
	const Scratch scratch;
	const std::string path = scratch.file("events.log");
	::setenv(kEnvVar, path.c_str(), 1);
	{
		std::ofstream fill(path, std::ios::binary);
		fill << std::string(kRotateBytes, 'x');
	}
	const Instance a("com.owlet.feathers");
	a.say("after-rotate");
	REQUIRE(fs::exists(path + ".1"));
	CHECK(fs::file_size(path + ".1") == kRotateBytes);
	const std::string text = slurp(path);
	CHECK(text.find("after-rotate") != std::string::npos);
	CHECK(fs::file_size(path) < 1024);
	// And under the cap it appends rather than rotating.
	a.say("second");
	CHECK(slurp(path).find("after-rotate") != std::string::npos);
	CHECK(slurp(path).find("second") != std::string::npos);
}

TEST_CASE("a file that cannot be written costs the line, never the caller") {
	const Scratch scratch;
	// A path whose parent is a FILE: neither the directory nor the log can be made.
	const std::string blocker = scratch.file("blocker");
	{
		std::ofstream(blocker) << "not a directory";
	}
	::setenv(kEnvVar, (blocker + "/sub/events.log").c_str(), 1);
	const Instance a("com.owlet.feathers");
	a.say("lost"); // returns, throws nothing
	CHECK(slurp(blocker) == "not a directory");
	CHECK_FALSE(fs::exists(blocker + "/sub"));
}

TEST_CASE("the directory is created when it does not exist yet") {
	const Scratch scratch;
	const std::string path = scratch.file("deeper/still/events.log");
	::setenv(kEnvVar, path.c_str(), 1);
	const Instance a("com.owlet.feathers");
	a.say("made");
	CHECK(slurp(path).find(" made\n") != std::string::npos);
}
