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
#include "composite/composite_types.h"
#include "rack/rack_engine.h"
#include "rack/rack_device.h"
#include "rack/parallel_rack.h"

#include <algorithm>
#include <cstdlib>

namespace feathers::codecs {


/// THE RACK, BY HAND, for the tracker's reason: `setFields` decodes straight into members — the
/// engine's macros and the device's own output stage — and there is no `Row`/`Settings` pair. ALL
/// ELEVEN CONTROLS ARE READ: the enable, mix and gain aim the output ramp, and each macro is set on the
/// engine, which routes it by its own edges. So all eleven are echoed and none needs an exemption.
///
/// UNADOPTED, deliberately: a row codec is about the row, and `setFields` touches nothing a catalogue
/// provides.
inline RowVec tripRack(const RowVec& in) {
	const device::AudioOps& ops = rack::kDeviceAudioOps;
	const size_t align = std::max<size_t>(ops.stateAlign, alignof(std::max_align_t));
	void* mem = feathers::test::heapAlignedAlloc(align, ((ops.stateBytes + align - 1) / align) * align);
	ops.construct(mem);
	ops.prepare(mem, 48000.0, 4096);
	ops.setFields(mem, in.data(), in.size());
	RowVec out(static_cast<size_t>(composite::Param::kCount), 0.0f);
	const rack::Reading r = rack::readingForTest(mem);
	out[static_cast<size_t>(composite::Param::Enabled)] = r.enabled ? 1.0f : 0.0f;
	out[static_cast<size_t>(composite::Param::Mix)] = r.mix;
	out[static_cast<size_t>(composite::Param::Gain)] = r.gain;
	for (size_t n = 0; n < composite::kMacroCount; ++n) {
		out[static_cast<size_t>(composite::Param::Macro1) + n] = rack::engineForTest(mem).macro(n);
	}
	ops.destruct(mem);
	feathers::test::heapFree(mem);
	return out;
}

/// This device's row codec for tests/test_device_ranges.cpp, registered from its own file (backlog NY.1).
inline constexpr Codec kChainCodec { composite::kTraits.tag, &tripRack };

} // namespace feathers::codecs
