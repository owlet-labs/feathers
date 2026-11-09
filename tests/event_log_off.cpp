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

// **EVERY TEST BINARY AND TOOL HOST STARTS WITH THE EVENT LOG OFF** (plugin/event_log.h). The tool hosts (guihost,
// bitwighost, cyclehost) compile this file in (the root CMakeLists). For the tests it is linked into every executable in
// this directory (`link_libraries` at the top of tests/CMakeLists.txt). ctest sets the same variable, but a
// binary run by hand (`scripts/run-cases.sh`) does not go through ctest, and a test that inits the CLAP entry
// arms the default file: without this, an agent's run by hand would write lines into the user's real log.
// Overwrite 0, so a case that wants a log sets its own path and a caller's explicit value is kept.

#include <cstdlib>

namespace {

[[maybe_unused]] const bool kEventLogOff = [] {
#if defined(_WIN32)
	if (std::getenv("FEATHERS_EVENT_LOG") == nullptr) {
		(void)_putenv_s("FEATHERS_EVENT_LOG", "off");
	}
#else
	(void)setenv("FEATHERS_EVENT_LOG", "off", 0);
#endif
	return true;
}();

// AND THE LIKES LOG (plugin/ui/likes_log.h), for the same reason: a case that likes a place must never append to the user's
// dataset. Empty is off; Windows cannot hold an empty variable, so there it is `off`.
[[maybe_unused]] const bool kLikesLogOff = [] {
#if defined(_WIN32)
	if (std::getenv("FEATHERS_LIKES_LOG_DIR") == nullptr) {
		(void)_putenv_s("FEATHERS_LIKES_LOG_DIR", "off");
	}
#else
	(void)setenv("FEATHERS_LIKES_LOG_DIR", "", 0);
#endif
	return true;
}();

[[maybe_unused]] const bool kSpikeLogOff = [] {
#if defined(_WIN32)
	if (std::getenv("FEATHERS_SPIKE_LOG_DIR") == nullptr) {
		(void)_putenv_s("FEATHERS_SPIKE_LOG_DIR", "off");
	}
#else
	(void)setenv("FEATHERS_SPIKE_LOG_DIR", "", 0);
#endif
	return true;
}();

} // namespace
