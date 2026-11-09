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
 */
#pragma once

// THE SPIKE RECORDER'S THRESHOLD, AS BOTH PLUGINS READ IT (backlog SR): `editor.conf`'s `spike_threshold` line, then
// `$FEATHERS_SPIKE_THRESHOLD` over it. One function, because feathers-chain read only the variable while feathers.clap
// read both. Header-only and outside `feathers-spike-log`, which links no
// `EditorPrefs` (that is feathers-ui's); each plugin TU that includes this links feathers-ui already. MAIN THREAD.

#include "plugin/editor_prefs.h"
#include "plugin/spike_log.h"

namespace feathers::spike {

[[nodiscard]] inline double thresholdFromPrefsAndEnvironment() noexcept {
	double fromPrefs = kDefaultThreshold;
	try {
		EditorPrefs prefs;
		prefs.load();
		fromPrefs = prefs.number("spike_threshold", kDefaultThreshold);
	}
	catch (...) {
	}
	try {
		return thresholdFromEnvironment(fromPrefs);
	}
	catch (...) {
		return fromPrefs;
	}
}

} // namespace feathers::spike
