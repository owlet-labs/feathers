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
#include "mod/stereo/stereo_types.h"
#include "mod/stereo/stereo.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {


/// THE STEREO TOOL, BY HAND, for Math's reason: `setFields` decodes into members. Every control but the enable is
/// read and echoed; the enable is the host's switch and is exempted below.
inline RowVec tripStereo(const RowVec& in) {
	mod::stereo::StereoTool t;
	t.prepare(48000.0);
	t.setFields(in.data(), in.size());
	RowVec out(static_cast<size_t>(mod::stereo::Param::kCount), 0.0f);
	const auto at = [&](mod::stereo::Param p) -> float& { return out[static_cast<size_t>(p)]; };
	at(mod::stereo::Param::Amount) = t.amountForTest();
	at(mod::stereo::Param::In) = t.inForTest();
	at(mod::stereo::Param::Width) = t.widthForTest();
	at(mod::stereo::Param::Swap) = t.swapForTest() ? 1.0f : 0.0f;
	return out;
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kStereoCodec { mod::stereo::kTraits.tag, &tripStereo };

} // namespace feathers::codecs
