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
#include "mod/macros/macros_core.h"
#include "mod/macro1/macro1_types.h"

namespace feathers::codecs {

/// THIS DEVICE'S ROW CODEC, for MACROS' reason (`macros.h`): `setFields` decodes into members, the types and
/// the enable are not read by the device, so they are left at zero and exempted.
inline RowVec tripMacro1(const RowVec& in) {
	mod::macros::MacrosCore<mod::macro1::kMacroCount> m;
	m.prepare(48000.0);
	m.setFields(in.data(), in.size());
	RowVec out(mod::macro1::kParamDescs.size(), 0.0f);
	for (size_t n = 0; n < mod::macro1::kMacroCount; ++n) {
		out[mod::macros::macroField(n)] = m.valueForTest(n);
		out[mod::macros::bipolarField(mod::macro1::kMacroCount, n)] = m.bipolarForTest(n) ? 1.0f : 0.0f;
	}
	return out;
}

inline constexpr Codec kMacro1Codec { mod::macro1::kTraits.tag, &tripMacro1 };

} // namespace feathers::codecs
