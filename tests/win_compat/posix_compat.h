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

// THE POSIX CALLS THE TEST SUITES MAKE, FOR WINDOWS — force-included into every test TU there (/FI, tests/CMakeLists.txt),
// and never seen by any other build. The suites set environment variables, name scratch files after their process id,
// sleep for a few milliseconds and read their own thread's CPU time; all four have a Windows spelling, and this is the
// one place that spells them, so not one test file grows a `_WIN32` branch for them. Each shim keeps the POSIX
// contract the callers rely on (setenv's `overwrite`, an empty value unsetting) and nothing more.
//
// `getpid` needs nothing here: the UCRT's <process.h> declares it under its POSIX name.

#if defined(_WIN32)

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <process.h>
#include <thread>

// <windows.h> FIRST AND ONCE, LEAN, then minus its two K&R relics: minwindef.h defines `near` and `far` as empty macros,
// and doctest pulls <windows.h> into every suite, which turned dozens of locals called `near` and `far` into syntax errors.
// Its include guard keeps a later include from defining them again.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#define FEATHERS_UNDEF_LEAN
#endif
#include <windows.h>
#ifdef FEATHERS_UNDEF_LEAN
#undef WIN32_LEAN_AND_MEAN
#undef FEATHERS_UNDEF_LEAN
#endif
#undef near
#undef far

inline int setenv(const char* name, const char* value, int overwrite) {
	if (overwrite == 0 && std::getenv(name) != nullptr) {
		return 0;
	}
	return _putenv_s(name, value) == 0 ? 0 : -1;
}

/// The suites pass `path.c_str()`, which is wide on Windows; the narrowing is the path's own.
inline int setenv(const char* name, const wchar_t* value, int overwrite) {
	return setenv(name, std::filesystem::path(value).string().c_str(), overwrite);
}

/// `_putenv_s` with an empty value REMOVES the variable, which is unsetenv's contract.
inline int unsetenv(const char* name) {
	return _putenv_s(name, "") == 0 ? 0 : -1;
}

inline int usleep(unsigned int microseconds) {
	std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
	return 0;
}

#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#ifndef CLOCK_THREAD_CPUTIME_ID
#define CLOCK_THREAD_CPUTIME_ID 3
#endif

/// clock_gettime for the two clocks the suites read: CLOCK_THREAD_CPUTIME_ID (GetThreadTimes, kernel + user) and
/// CLOCK_MONOTONIC (QueryPerformanceCounter). Defined in win_compat.cpp out of line.
int feathersWinClockGettime(int clock, struct timespec* out);
#define clock_gettime feathersWinClockGettime

#endif // _WIN32
