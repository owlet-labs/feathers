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

// THE HEAP A TEST'S OWN ALLOCATIONS COME FROM, ONE PAIR PER PLATFORM.
//
// The suites that replace the global `operator new` to count allocations, and the helpers that build a
// device's state by hand, hand out storage from `malloc`, `aligned_alloc` or `posix_memalign` and give
// it all back through ONE `free`. That is correct where those three share a heap — glibc, macOS — and is
// not correct on Windows: the UCRT has no `aligned_alloc` or `posix_memalign` at all, and an aligned
// block (`_aligned_malloc`) may be returned only through `_aligned_free`, never through `free`.
//
// So every allocation those files make goes through here. On POSIX each function is exactly the call it
// replaced, so nothing changes where it already worked. On Windows EVERY block, aligned or not, comes from
// `_aligned_malloc` and goes back through `_aligned_free`, which keeps the files' one-free-for-everything
// shape correct: a replaced `operator delete(void*)` cannot know whether the pointer came from the aligned
// or the plain `operator new`, and here it does not need to.

#include <cstddef>
#include <cstdlib>
#if defined(_WIN32)
#include <malloc.h>
#endif

namespace feathers::test {

#if defined(_WIN32)

/// The UCRT's own `malloc` alignment on x64, so a plain block is aligned as `malloc`'s would be.
inline constexpr std::size_t kHeapPlainAlign = 16;

inline void* heapAlloc(std::size_t bytes) {
	return _aligned_malloc(bytes == 0 ? 1 : bytes, kHeapPlainAlign);
}

inline void* heapAlignedAlloc(std::size_t align, std::size_t bytes) {
	return _aligned_malloc(bytes == 0 ? 1 : bytes, align < kHeapPlainAlign ? kHeapPlainAlign : align);
}

inline int heapPosixMemalign(void** out, std::size_t align, std::size_t bytes) {
	*out = heapAlignedAlloc(align, bytes);
	return *out == nullptr ? 12 /* ENOMEM */ : 0;
}

inline void heapFree(void* p) {
	_aligned_free(p);
}

#else

inline void* heapAlloc(std::size_t bytes) {
	return std::malloc(bytes);
}

inline void* heapAlignedAlloc(std::size_t align, std::size_t bytes) {
	return std::aligned_alloc(align, bytes);
}

inline int heapPosixMemalign(void** out, std::size_t align, std::size_t bytes) {
	return posix_memalign(out, align, bytes);
}

inline void heapFree(void* p) {
	std::free(p);
}

#endif

} // namespace feathers::test
