/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#pragma once

#include "codec.h"
#include "mod/tracker/tracker_types.h"
#include "mod/tracker/tracker.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {

/// THE TRACKER, BY HAND, because it has no row codec: `setFields` decodes straight into members.
/// Its four decoded controls are read back through the test-only accessors on `Tracker`; see the
/// note there for why `Param::Enabled` is not among them — this device never reads it.
inline RowVec tripTracker(const RowVec& in) {
	mod::tracker::Tracker t;
	t.prepare(48000.0);
	t.setFields(in.data(), in.size());
	RowVec out(static_cast<size_t>(mod::tracker::Param::kCount), 0.0f);
	out[static_cast<size_t>(mod::tracker::Param::Rule)] = static_cast<float>(t.ruleForTest());
	out[static_cast<size_t>(mod::tracker::Param::RootNote)] = t.rootNoteForTest();
	out[static_cast<size_t>(mod::tracker::Param::Spread)] = t.spreadForTest();
	out[static_cast<size_t>(mod::tracker::Param::Glide)] = t.glideForTest();
	// `Param::Enabled` IS LEFT AT ZERO AND NOT ECHOED, deliberately. This device does not read it —
	// the host switches a modulator through `Traits::enableControl` — so echoing the input would be
	// the TEST manufacturing a pass for a decode that does not happen. Leaving it dead makes the
	// sweep report it, and the exemption below is where the reason is written down.
	return out;
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kTrackerCodec { mod::tracker::kTraits.tag, &tripTracker };

} // namespace feathers::codecs
