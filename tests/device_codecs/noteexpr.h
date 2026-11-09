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
#include "mod/noteexpr/noteexpr_types.h"
#include "mod/noteexpr/noteexpr.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {


/// THE NOTE-EXPRESSION DEVICE, BY HAND, for the Tracker's reason: `setFields` decodes straight into
/// members and there is no `Row`/`Settings` pair to round-trip. Both of its controls ARE read by the
/// device — unlike the Tracker's `Enabled`, which it ignores — so both are echoed and neither needs
/// an exemption.
inline RowVec tripNoteExpr(const RowVec& in) {
	mod::noteexpr::NoteExpressions d;
	d.prepare(48000.0);
	d.setFields(in.data(), in.size());
	RowVec out(static_cast<size_t>(mod::noteexpr::Param::Count), 0.0f);
	out[static_cast<size_t>(mod::noteexpr::Param::Rule)] = static_cast<float>(d.ruleForTest());
	out[static_cast<size_t>(mod::noteexpr::Param::Enabled)] = d.enabledForTest() ? 1.0f : 0.0f;
	return out;
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kNoteexprCodec { mod::noteexpr::kTraits.tag, &tripNoteExpr };

} // namespace feathers::codecs
