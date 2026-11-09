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

// THE NOTHROW FORMS, FOR A BINARY THAT REPLACES `operator new` TO COUNT ALLOCATIONS.
//
// **INCLUDE AFTER THE BINARY'S OWN REPLACEMENTS, FROM EXACTLY ONE TRANSLATION UNIT** — it defines
// replaceable global allocation functions, which a program may define once.
//
// FOUND BY ASan, 2026-10-03 (`test_fx_eq`: alloc-dealloc-mismatch, operator new vs free). A probe that
// replaces the throwing forms and frees with `std::free` leaves the NOTHROW forms to the runtime. In a
// plain build that is harmless — libstdc++'s nothrow new calls the replaced throwing one — but ASan
// intercepts the nothrow form itself, so the block comes from ASan's `new` and goes back through the
// probe's `free`. `std::stable_sort` is the everyday caller: its `_Temporary_buffer` is nothrow new.
// Forwarding to whatever throwing form is in effect keeps every pair matched, counts the allocation
// like any other, and is what the standard says the default nothrow forms do anyway.

#include <cstddef>
#include <new>

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
	try {
		return ::operator new(n);
	} catch (...) {
		return nullptr;
	}
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
	try {
		return ::operator new[](n);
	} catch (...) {
		return nullptr;
	}
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
	try {
		return ::operator new(n, a);
	} catch (...) {
		return nullptr;
	}
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
	try {
		return ::operator new[](n, a);
	} catch (...) {
		return nullptr;
	}
}
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { ::operator delete[](p); }
void operator delete(void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete(p, a); }
void operator delete[](void* p, std::align_val_t a, const std::nothrow_t&) noexcept { ::operator delete[](p, a); }
