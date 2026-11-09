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
#include "mod/arp/arp_types.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {

// THE ARP'S ROW: decoded the way the device decodes it, then written back.
inline RowVec tripArp(const RowVec& in) {
	const mod::arp::Row out = mod::arp::rowOf(mod::arp::settingsOf(in.data(), in.size()));
	return RowVec(out.begin(), out.end());
}

inline constexpr Codec kArpCodec { mod::arp::kTraits.tag, &tripArp };

} // namespace feathers::codecs
