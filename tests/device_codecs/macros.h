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
#include "mod/macros/macros_types.h"
#include "mod/macros/macros.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {


/// THE MACROS DEVICE, BY HAND, for the tracker's reason: `setFields` decodes into members. The four
/// values and four polarities are read and echoed; the four types and the enable are NOT read by the
/// device (a type is how the card's control writes the value; the host switches a modulator), so they
/// are left at zero and exempted below.
inline RowVec tripMacros(const RowVec& in) {
	mod::macros::Macros m;
	m.prepare(48000.0);
	m.setFields(in.data(), in.size());
	RowVec out(static_cast<size_t>(mod::macros::Param::kCount), 0.0f);
	for (size_t n = 0; n < mod::macros::kMacroCount; ++n) {
		out[mod::macros::macroParam(n)] = m.valueForTest(n);
		out[mod::macros::bipolarParam(n)] = m.bipolarForTest(n) ? 1.0f : 0.0f;
	}
	return out;
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kMacrosCodec { mod::macros::kTraits.tag, &tripMacros };

} // namespace feathers::codecs
