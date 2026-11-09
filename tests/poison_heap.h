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

// THE POISONED HEAP (2026-09-24, the SKLITTER crush). Recording SILENCE into a FRESH instance still
// played a softer version of the broken signal, and only a Bitwig restart cleared it — as if corrupted
// memory were being reused across instances. A host allocator hands a new
// instance the pages an old one freed, still holding its audio; a buffer read before it is written
// plays them. This host has no MSan (no clang), so a binary including this does the job the blunt
// way: EVERY C++ allocation it makes arrives full of `kPoison`. A device fed silence must then answer
// with exact zeros; any sample that is not is a read of memory nobody wrote.
//
// **INCLUDE FROM EXACTLY ONE TRANSLATION UNIT OF A BINARY** — it defines the replaceable global
// allocation functions, which a program may define once.

#include "test_heap.h"
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace feathers::test {

/// 0.5f in every float slot. Loud, finite, and a value no silent render produces by accident.
inline constexpr float kPoison = 0.5f;

inline void* poisonedAlloc(std::size_t n, std::size_t align) {
	const std::size_t a = std::max(align, alignof(std::max_align_t));
	const std::size_t bytes = ((n == 0 ? 1 : n) + a - 1) / a * a;
	void* p = feathers::test::heapAlignedAlloc(a, bytes);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	auto* f = static_cast<float*>(p);
	for (std::size_t i = 0; i < bytes / sizeof(float); ++i) {
		f[i] = kPoison;
	}
	return p;
}

} // namespace feathers::test

// Every form a C++ allocation takes, including the aligned ones the rack uses for device storage
// (`rack_engine.cpp`: `new (std::align_val_t{align}) std::byte[]`).
void* operator new(std::size_t n) { return feathers::test::poisonedAlloc(n, alignof(std::max_align_t)); }
void* operator new[](std::size_t n) { return feathers::test::poisonedAlloc(n, alignof(std::max_align_t)); }
void* operator new(std::size_t n, std::align_val_t a) {
	return feathers::test::poisonedAlloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
	return feathers::test::poisonedAlloc(n, static_cast<std::size_t>(a));
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
	try {
		return feathers::test::poisonedAlloc(n, alignof(std::max_align_t));
	} catch (...) {
		return nullptr;
	}
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
	try {
		return feathers::test::poisonedAlloc(n, alignof(std::max_align_t));
	} catch (...) {
		return nullptr;
	}
}
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
