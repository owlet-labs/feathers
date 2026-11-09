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

// The out-of-line half of posix_compat.h: the one test-side file that includes <windows.h>.

#include "posix_compat.h"

#include <windows.h>

#include <cstdint>

int feathersWinClockGettime(int clock, struct timespec* out) {
	if (out == nullptr) {
		return -1;
	}
	std::uint64_t hundredNs = 0;
	if (clock == CLOCK_THREAD_CPUTIME_ID) {
		FILETIME created {};
		FILETIME exited {};
		FILETIME kernel {};
		FILETIME user {};
		if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) {
			return -1;
		}
		const auto ticks = [](const FILETIME& t) {
			return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
		};
		hundredNs = ticks(kernel) + ticks(user);
	}
	else if (clock == CLOCK_MONOTONIC) {
		LARGE_INTEGER now {};
		LARGE_INTEGER rate {};
		QueryPerformanceCounter(&now);
		QueryPerformanceFrequency(&rate);
		const auto count = static_cast<std::uint64_t>(now.QuadPart);
		const auto perSecond = static_cast<std::uint64_t>(rate.QuadPart);
		out->tv_sec = static_cast<time_t>(count / perSecond);
		out->tv_nsec = static_cast<long>((count % perSecond) * 1000000000ull / perSecond);
		return 0;
	}
	else {
		return -1;
	}
	out->tv_sec = static_cast<time_t>(hundredNs / 10000000ull);
	out->tv_nsec = static_cast<long>((hundredNs % 10000000ull) * 100ull);
	return 0;
}
