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
#include "../test_heap.h"
#include "doctest.h"

#pragma once

#include "codec.h"
#include "rack/parallel_rack.h"
#include "rack/rack_engine.h"
#include "rack/rack_device.h"
#include "rack/parallel_rack.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {


/// THE RACK (backlog NO) — the same shape as the Chain's: decoded into members, read back by value.
inline RowVec tripParallelRack(const RowVec& in) {
	const device::AudioOps& ops = rack::parallel::kAudioOps;
	const size_t align = std::max<size_t>(ops.stateAlign, alignof(std::max_align_t));
	void* mem = feathers::test::heapAlignedAlloc(align, ((ops.stateBytes + align - 1) / align) * align);
	ops.construct(mem);
	ops.prepare(mem, 48000.0, 4096);
	ops.setFields(mem, in.data(), in.size());
	RowVec out(rack::parallel::kAllParamCount, 0.0f);
	for (size_t i = 0; i < rack::parallel::kSlotParamBase; ++i) {
		out[i] = rack::parallel::rackOf(mem)->fieldForTest(static_cast<rack::parallel::Param>(i));
	}
	// THE CHAIN SLOTS (backlog NO), as the rack resolved them.
	for (size_t i = rack::parallel::kSlotParamBase; i < out.size(); ++i) {
		out[i] = rack::parallel::rackOf(mem)->slotFieldForTest(i - rack::parallel::kSlotParamBase);
	}
	ops.destruct(mem);
	feathers::test::heapFree(mem);
	return out;
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kParallelRackCodec { rack::parallel::kTraits.tag, &tripParallelRack };

} // namespace feathers::codecs
