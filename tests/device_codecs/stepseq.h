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
#include "mod/stepseq/stepseq_types.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {


/// THE STEP SEQUENCER, through its codec, whose decode reads a pointer and a count (`settingsOf`) because its row is
/// the controls and then the pattern. Only the controls come back here; the pattern is `test_mod_stepseq`'s.
inline RowVec tripStepSeq(const RowVec& in) {
	const mod::stepseq::Row out = mod::stepseq::rowOf(mod::stepseq::settingsOf(in.data(), in.size()));
	return RowVec(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(mod::stepseq::kParamCount));
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kStepseqCodec { mod::stepseq::kTraits.tag, &tripStepSeq };

} // namespace feathers::codecs
