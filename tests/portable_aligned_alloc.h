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

// THE STORAGE BEHIND A REPLACED ALIGNED `operator new`, UNDER THE STRICTEST PLATFORM'S CONTRACT.
//
// FOUND TWICE ON macOS CI, and only there: run 35172215178 (test_lane_rack, four cases) and run
// 37412368692 (test_clap_render, SIGABRT in "KO: editing one control of a lane device ..."). The rack
// allocates a device's state with `new (std::align_val_t { stateAlign }) std::byte[bytes]`, and a
// device whose state is only floats declares `alignof` 4. The explicit form passes that 4 to the
// replaced operator as it stands — any power of two is a valid request there, and the runtime's own
// operator raises it to a pointer's alignment before asking malloc. A replacement that hands the 4
// straight to `std::aligned_alloc` gets null back on macOS (posix_memalign underneath: the alignment
// must be a multiple of `sizeof(void*)`), turns it into `bad_alloc`, and a `noexcept` caller turns that
// into `std::terminate`. glibc accepts the 4, so Linux stays green over the same bug.
//
// SO THIS REFUSES WHAT macOS REFUSES, ON EVERY PLATFORM: a caller that drops the raise to a pointer's
// alignment, or the rounding of the size to a multiple of it (C11's own rule, which macOS enforces
// too), gets null here on Linux as well — and the cases that allocate a float-only device go red where
// we build, rather than only on the runner we cannot see.

#include "test_heap.h"
#include <algorithm>
#include <cstddef>
#include <cstdlib>

namespace feathers::test {

/// `std::aligned_alloc` held to macOS's rules: an alignment below a pointer's, or a size that is not a
/// multiple of the alignment, is refused (null) here rather than only there.
inline void* strictAlignedAlloc(std::size_t align, std::size_t bytes) {
	if (align < sizeof(void*) || (align & (align - 1)) != 0 || bytes % align != 0) {
		return nullptr;
	}
	return feathers::test::heapAlignedAlloc(align, bytes);
}

/// Storage for a replaced `operator new(size_t, std::align_val_t)`: at least a pointer's alignment
/// (the larger satisfies the smaller request), the size rounded up to a whole number of it, never
/// zero bytes. Null only when the heap itself says no.
inline void* portableAlignedAlloc(std::size_t bytes, std::size_t align) {
	const std::size_t a = std::max(align, sizeof(void*));
	const std::size_t rounded = ((bytes == 0 ? 1 : bytes) + a - 1) / a * a;
	return strictAlignedAlloc(a, rounded);
}

} // namespace feathers::test
