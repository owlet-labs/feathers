/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE VOICE-STACKING BENCHMARK (backlog KE, step 0) — NOT A TEST: it asserts nothing and ctest never runs it.
// It measures the four unit costs the stack's CPU guard is built from, on the real oscillator and the real
// bank, and prints the guard's budget B derived from them:
//
//   c_osc_still   one oscillator, one 256-sample block, its table standing still
//   c_osc_moving  the same while its table is rebuilt every 128 samples (the fade guard's pace), so the
//                 oscillator spends most of its time mixing two tables — what a moving puck costs a part
//   c_build       one V5 table build (`CharacterBank::setCharacter`, an operated position)
//   c_bake        one vowel bake, from a gliding vowel voice against a gliding vowel-free one, divided by
//                 the bakes COUNTED (`voxBakesForTest`), not assumed
//
// and then the worst block the guard must fit:  B × c_osc_moving + Nb × c_build + Nk × c_bake,  where Nb and
// Nk are the per-block build and bake budgets. B is printed for two targets (50% and 70% of one core).
//
// **THE HOST IS SHARED.** CLAUDE.md's benchmarking rule: every rep runs every case in turn (alternated, not
// batched), the default is 9 reps, and the figure reported is the MEDIAN of the reps' medians. The load
// average at start and end is printed with the result, because a number without it says nothing.
//
// The positions (zone 640 at offsets 0.0 and 1.3563) are the vox tests' own: the second is the first offset
// `findVoxOffset` returns with a vowel present above 0.6, the first an operated vowel-free V5 stand.

#include "dsp/character_bank.h"
#include "dsp/osc_voice.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using feathers::Character;
using feathers::CharacterBank;
using feathers::OscEngine;
using feathers::OscVoice;

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
constexpr double kBlockMicros = 1e6 * kBlock / kRate;
constexpr float kPlainOffset = 0.0f;
constexpr float kVowelOffset = 1.3563f;

Character stand(float offset) {
	Character c;
	c.engine = OscEngine::Morph;
	c.flagship = true;
	c.count = 1;
	c.sounds[0].zone = 640;
	c.sounds[0].phaseOffset = offset;
	c.sounds[0].weight = 1.0;
	return c;
}

double median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v.empty() ? 0.0 : v[v.size() / 2];
}

double loadAverage() {
	double load = -1.0;
	if (FILE* f = std::fopen("/proc/loadavg", "r")) {
		if (std::fscanf(f, "%lf", &load) != 1) {
			load = -1.0;
		}
		std::fclose(f);
	}
	return load;
}

/// N oscillators on one bank for `blocks` blocks; the bank rebuilt every 128 samples when `moving`. Returns the
/// median block in µs PER OSCILLATOR, with the builds' own cost left in (the caller subtracts it).
double oscillators(int n, bool moving, int blocks, float offset, bool glide, uint32_t* bakes) {
	auto bank = std::make_unique<CharacterBank>();
	bank->setSampleRate(kRate);
	bank->setCharacter(stand(offset));
	std::vector<std::unique_ptr<OscVoice>> parts;
	for (int i = 0; i < n; ++i) {
		auto o = std::make_unique<OscVoice>();
		o->setCharacterBank(bank.get());
		o->setSampleRate(kRate);
		o->setNoiseSeed(static_cast<uint32_t>(i + 1));
		o->setPitch(57.0 + 0.05 * i);
		if (glide) {
			o->setGlideSeconds(0.5);
		}
		parts.push_back(std::move(o));
	}
	std::vector<float> out(32);
	std::vector<double> us;
	int flip = 0;
	int since = 0;
	uint32_t bakes0 = 0;
	for (auto& p : parts) {
		bakes0 += p->voxBakesForTest();
	}
	for (int b = 0; b < blocks; ++b) {
		if (glide && b % 40 == 0) {
			for (auto& p : parts) {
				p->glideToPitch(b % 80 == 0 ? 69.0 : 45.0);
			}
		}
		const auto t0 = std::chrono::steady_clock::now();
		int done = 0;
		while (done < kBlock) {
			if (moving && since >= 128) {
				flip ^= 1;
				bank->setCharacter(stand(offset + (flip != 0 ? 1e-5f : 0.0f)));
				since = 0;
			}
			const int m = std::max(1, std::min({ 32, kBlock - done, bank->samplesUntilPendingBuild() }));
			for (auto& p : parts) {
				p->render(out.data(), m);
			}
			bank->advance(m);
			since += m;
			done += m;
		}
		const auto t1 = std::chrono::steady_clock::now();
		if (b >= blocks / 8) {
			us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count() / n);
		}
	}
	if (bakes != nullptr) {
		uint32_t total = 0;
		for (auto& p : parts) {
			total += p->voxBakesForTest();
		}
		*bakes = total - bakes0;
	}
	return median(us);
}

/// One V5 table build, landing immediately (the previous build's fade drained first).
double build(int count) {
	auto bank = std::make_unique<CharacterBank>();
	bank->setSampleRate(kRate);
	std::vector<double> us;
	for (int i = 0; i < count; ++i) {
		int left = bank->fadeLengthSamples() + kBlock;
		while (left > 0) {
			const int m = std::max(1, std::min({ 32, left, bank->samplesUntilPendingBuild() }));
			bank->advance(m);
			left -= m;
		}
		const uint32_t g = bank->generation();
		const Character c = stand(kPlainOffset + 1e-5f * static_cast<float>(i % 2));
		const auto t0 = std::chrono::steady_clock::now();
		bank->setCharacter(c);
		const auto t1 = std::chrono::steady_clock::now();
		if (bank->generation() != g && i >= count / 8) {
			us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
		}
	}
	return median(us);
}

} // namespace

int main(int argc, char** argv) {
	int reps = 9;
	int nb = 8;
	int nk = 8;
	for (int i = 1; i + 1 < argc; i += 2) {
		const int v = std::atoi(argv[i + 1]);
		if (std::strcmp(argv[i], "--reps") == 0) {
			reps = std::max(1, v);
		}
		else if (std::strcmp(argv[i], "--build-budget") == 0) {
			nb = v;
		}
		else if (std::strcmp(argv[i], "--bake-budget") == 0) {
			nk = v;
		}
	}
	const double loadBefore = loadAverage();
	std::vector<double> still, moving, builds, bakes;
	for (int r = 0; r < reps; ++r) {
		still.push_back(oscillators(8, false, 800, kPlainOffset, false, nullptr));
		const double b = build(200);
		builds.push_back(b);
		// Two builds land per block while moving; their cost is the bank's, not the oscillators'.
		moving.push_back(oscillators(8, true, 800, kPlainOffset, false, nullptr) - 2.0 * b / 8.0);
		uint32_t vowelBakes = 0;
		const double vowelGlide = oscillators(1, false, 800, kVowelOffset, true, &vowelBakes);
		const double plainGlide = oscillators(1, false, 800, kPlainOffset, true, nullptr);
		const double perBlock = static_cast<double>(vowelBakes) / 800.0;
		bakes.push_back(perBlock > 0.0 ? (vowelGlide - plainGlide) / perBlock : 0.0);
	}
	const double loadAfter = loadAverage();

	const double cStill = median(still);
	const double cMoving = median(moving);
	const double cBuild = median(builds);
	const double cBake = median(bakes);
	const double budgets = nb * cBuild + nk * cBake;
	std::printf("bench_stack: %d reps, load average %.1f -> %.1f, 48 kHz, %d-sample blocks (%.0f us)\n", reps,
	            loadBefore, loadAfter, kBlock, kBlockMicros);
	std::printf("  c_osc_still   %7.1f us per oscillator per block\n", cStill);
	std::printf("  c_osc_moving  %7.1f us per oscillator per block (table rebuilt every 128 samples)\n", cMoving);
	std::printf("  c_build       %7.1f us per V5 table\n", cBuild);
	std::printf("  c_bake        %7.1f us per vowel bake (counted)\n", cBake);
	std::printf("  budgets       Nb=%d Nk=%d -> %.0f us per block\n", nb, nk, budgets);
	for (const double target : { 0.5, 0.7 }) {
		const double room = target * kBlockMicros - budgets;
		const int bMoving = room > 0.0 ? static_cast<int>(std::floor(room / cMoving)) : 0;
		const int bStill = room > 0.0 ? static_cast<int>(std::floor(room / cStill)) : 0;
		std::printf("  B for %2.0f%% of a core: %d oscillators moving, %d still\n", 100.0 * target, bMoving,
		            bStill);
	}
	std::printf("  reps (c_osc_moving):");
	for (const double m : moving) {
		std::printf(" %.1f", m);
	}
	std::printf("\n");
	return 0;
}
