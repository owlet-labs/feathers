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

// ONE KARPLUS-STRONG STRING'S COST (design §5): µs per 256-frame block at 48 kHz, ringing (never idle), one ear and
// two, median of five runs; and one pluck's excitation build at the lowest note. Prints a checksum so the work is used.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

#include "dsp/ks_string.h"

using namespace feathers::ks;
using Clock = std::chrono::steady_clock;

int main() {
	const double sr = 48000.0;
	double sink = 0.0;
	for (const bool stereo : { false, true }) {
		std::vector<double> runs;
		for (int run = 0; run < 5; ++run) {
			KsString s;
			s.prepare(sr);
			s.setDamping(0.05);
			s.setDecay(0.0);
			s.setDispersion(-0.3);
			s.setFrequency(220.0);
			s.setPickups(0.1, stereo ? 0.2 : 0.1);
			std::vector<float> e(static_cast<size_t>(s.capacity()));
			std::vector<float> scratch(e.size());
			const int n = buildExcitation(Excitation {}, sr / 220.0, e.data(), scratch.data(), s.capacity());
			s.pluck(e.data(), n);
			std::vector<float> l(256);
			std::vector<float> r(256);
			const int blocks = 4000;
			const auto t0 = Clock::now();
			for (int b = 0; b < blocks; ++b) {
				s.render(l.data(), stereo ? r.data() : nullptr, 256);
				sink += l[17];
			}
			runs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / blocks);
		}
		std::sort(runs.begin(), runs.end());
		std::printf("ks string %s: %.2f us per 256-frame block (median of 5; range %.2f-%.2f)\n", stereo ? "stereo" : "mono",
		            runs[2], runs[0], runs[4]);
	}
	{
		std::vector<double> runs;
		std::vector<float> e(8192);
		std::vector<float> scratch(8192);
		for (int run = 0; run < 5; ++run) {
			const auto t0 = Clock::now();
			for (int i = 0; i < 100; ++i) {
				Excitation ex;
				ex.noise = 0.5f;
				std::vector<float>& tone = scratch;
				(void)tone;
				ex.seed = static_cast<uint64_t>(i + 1);
				sink += buildExcitation(ex, sr / kLowestHz, e.data(), scratch.data(), 8192);
			}
			runs.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count() / 100);
		}
		std::sort(runs.begin(), runs.end());
		std::printf("excitation at C0 (2936 samples): %.2f us (median of 5)\n", runs[2]);
	}
	std::printf("checksum %g\n", sink);
	return 0;
}
