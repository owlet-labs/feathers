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

#pragma once

#include "codec.h"
#include "mod/arp/pattern_types.h"

namespace feathers::codecs {

// THE PATTERN DEVICE'S ROW: decoded the way the device decodes it, then written back.
inline RowVec tripPattern(const RowVec& in) {
	const mod::arp::pat::Row out = mod::arp::pat::rowOf(mod::arp::pat::settingsOf(in.data(), in.size()));
	return RowVec(out.begin(), out.end());
}

inline constexpr Codec kPatternCodec { mod::arp::pat::kTraits.tag, &tripPattern };

} // namespace feathers::codecs
