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

/// **FLUSH DENORMALS TO ZERO FOR THE SCOPE OF ONE PROCESS CALL, ON THE PLATFORMS THAT HAVE THE BIT.**
///
/// The plugin never set this itself, on any platform (MEASURED 2026-09-07: no FTZ, DAZ, MXCSR or FPCR
/// anywhere in src/). On x86 Linux it lived on the HOST'S setting — Bitwig sets FTZ and DAZ on its
/// audio thread — which is a promise no host makes and the CLAP spec does not require. On arm64 the
/// equivalent is the FZ bit of FPCR, and whether a given macOS host sets it is unknown; a feedback
/// path or a decaying tail that reaches the denormal range then costs tens of times its normal price
/// per sample, which is the classic shape of a DSP thread that falls over only on one machine.
///
/// SCOPED AND RESTORED: the flag is set at the top of process() and put back exactly as found when the
/// call returns, so a host that shares the thread with its own code sees its own state again.
///
/// - x86 / x86_64: MXCSR bit 15 (FTZ — results) and bit 6 (DAZ — operands), through <xmmintrin.h>
///   (GCC, clang, MSVC all ship it; the ARCH check is on the compiler's macros, not the OS's).
/// - aarch64 (GCC and clang): FPCR bit 24 (FZ), read and written with mrs/msr — the register is
///   user-accessible and both compilers accept the asm; MSVC's aarch64 has no such asm and gets the
///   no-op until someone builds there.
/// - anything else: a no-op with the same shape, so the call site never needs a conditional.
///
/// NOT the answer to DF (Bitwig's meter frozen nonzero on the Mac, cleared by a host restart, filed as
/// a sighting): a denormal costs time, not correctness. It is the standard shape every audio plugin
/// carries, and it was missing.

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <xmmintrin.h>
#define FEATHERS_DENORMAL_GUARD_X86 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <cstdint>
#define FEATHERS_DENORMAL_GUARD_AARCH64 1
#endif

namespace feathers {

class DenormalGuard {
public:
	DenormalGuard() noexcept {
#if defined(FEATHERS_DENORMAL_GUARD_X86)
		saved_ = _mm_getcsr();
		_mm_setcsr(saved_ | kFtz | kDaz);
#elif defined(FEATHERS_DENORMAL_GUARD_AARCH64)
		saved_ = readFpcr();
		writeFpcr(saved_ | kFz);
#endif
	}
	~DenormalGuard() noexcept {
#if defined(FEATHERS_DENORMAL_GUARD_X86)
		_mm_setcsr(saved_);
#elif defined(FEATHERS_DENORMAL_GUARD_AARCH64)
		writeFpcr(saved_);
#endif
	}
	DenormalGuard(const DenormalGuard&) = delete;
	DenormalGuard& operator=(const DenormalGuard&) = delete;

	/// Whether THIS build can flush at all — a test asserts the flush only where the answer is yes,
	/// and asserts the restore everywhere.
	static constexpr bool available() noexcept {
#if defined(FEATHERS_DENORMAL_GUARD_X86) || defined(FEATHERS_DENORMAL_GUARD_AARCH64)
		return true;
#else
		return false;
#endif
	}

	/// The control word as the platform reports it right now — for a test to read before, inside and
	/// after a scope. Zero where there is no such word.
	static unsigned long long currentControlWord() noexcept {
#if defined(FEATHERS_DENORMAL_GUARD_X86)
		return _mm_getcsr();
#elif defined(FEATHERS_DENORMAL_GUARD_AARCH64)
		return readFpcr();
#else
		return 0;
#endif
	}

private:
#if defined(FEATHERS_DENORMAL_GUARD_X86)
	static constexpr unsigned int kFtz = 0x8000u;
	static constexpr unsigned int kDaz = 0x0040u;
	unsigned int saved_ = 0;
#elif defined(FEATHERS_DENORMAL_GUARD_AARCH64)
	static constexpr std::uint64_t kFz = std::uint64_t{1} << 24;
	std::uint64_t saved_ = 0;
	static std::uint64_t readFpcr() noexcept {
		std::uint64_t value = 0;
		__asm__ __volatile__("mrs %0, fpcr" : "=r"(value));
		return value;
	}
	static void writeFpcr(std::uint64_t value) noexcept {
		__asm__ __volatile__("msr fpcr, %0" : : "r"(value));
	}
#endif
};

} // namespace feathers
