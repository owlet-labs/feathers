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
#include "mod/twinfield/twinfield_types.h"

namespace feathers::codecs {

/// TWIN FIELD'S ROW, decoded and re-encoded through the device's own `settingsOf` / `rowOf`.
inline RowVec tripTwinField(const RowVec& in) {
	const auto out = mod::twinfield::rowOf(mod::twinfield::settingsOf(in.data(), in.size()));
	return RowVec(out.begin(), out.end());
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kTwinFieldCodec { mod::twinfield::kTraits.tag, &tripTwinField };

} // namespace feathers::codecs
