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

// THE ADDITIVE BUILD BENCHMARK — not a test (plan Task 7 quality round; the number goes into the design §2.5). One
// CharacterBank STEREO build (both ears) of an 8-sound constellation, in µs: Partials with Shape amount 0, Partials
// with a Shape amount on every sound, and the same constellation on V5 (the flagship bake, which runs in the same
// place today). Every sound carries a share, so every sound is built. The cases are ALTERNATED round by round and
// the median over every build is printed, because this host is shared and load swings within a run (CLAUDE.md);
// run `uptime` beside it. Nothing here is optimised yet: this is the measurement Task 10 starts from.

#include "dsp/character_bank.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

using feathers::Character;
using feathers::CharacterBank;
using feathers::OscEngine;

constexpr double kSampleRate = 48000.0;
constexpr int kBuildsPerRound = 20;
constexpr int kRounds = 25;
constexpr double kBlockMicros = 256.0 / kSampleRate * 1e6; // 5333 µs

/// Eight sounds spread over the reachable window, each with an eighth of the blend and a little travel, as a
/// puck in the middle of a full constellation would give them.
Character constellation(OscEngine engine, bool flagship, float shapeAmount) {
	Character c;
	c.engine = engine;
	c.flagship = flagship;
	c.count = 8;
	for (int i = 0; i < 8; ++i) {
		feathers::SoundCoord& s = c.sounds[static_cast<size_t>(i)];
		s.zone = static_cast<uint16_t>(97 * i + 13);
		s.phaseOffset = -6.0f + 2.1f * static_cast<float>(i);
		s.weight = 1.0 / 8.0;
		s.travelX = 0.05 * (i - 3.5);
		s.travelY = -0.03 * (i - 3.5);
		s.shapeAmount = shapeAmount;
	}
	return c;
}

/// µs per stereo build of `c`, one entry per build. Each build moves the two ears' travel a hair, so the bank never
/// drops it as equal-to-live, and the fade guard is run out first, so each request builds on the spot.
void time(CharacterBank& bank, const Character& c, int round, std::vector<double>& out) {
	for (int b = 0; b < kBuildsPerRound; ++b) {
		Character l = c;
		Character r = c;
		const double nudge = 1e-4 * (round * kBuildsPerRound + b + 1);
		for (size_t i = 0; i < 8; ++i) {
			l.sounds[i].travelX -= 0.1 + nudge;
			r.sounds[i].travelX += 0.1 + nudge;
		}
		bank.advance(1 << 20);
		const auto t0 = std::chrono::steady_clock::now();
		bank.setCharacterStereo(l, r);
		const auto t1 = std::chrono::steady_clock::now();
		out.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
	}
}

} // namespace

int main() {
	struct Case {
		const char* name;
		Character c;
		std::vector<double> runs;
	};
	std::vector<Case> cases { { "Partials, Shape amount 0", constellation(OscEngine::Partials, false, 0.0f), {} },
		                      { "Partials, Shape amount 0.5", constellation(OscEngine::Partials, false, 0.5f), {} },
		                      { "V5 (flagship bake)", constellation(OscEngine::Morph, true, 0.0f), {} } };
	CharacterBank bank;
	bank.setSampleRate(kSampleRate);
	for (int r = 0; r < kRounds; ++r) {
		for (Case& c : cases) {
			time(bank, c.c, r, c.runs);
		}
	}
	// WHAT THE V5 CASE BUILT, so its number is read as what it is: the operator grid baked, or the identity corner.
	// Timed into a throwaway, so every case's list holds exactly its alternated builds.
	std::vector<double> diagnostic;
	time(bank, cases[2].c, 0, diagnostic);
	std::printf("V5 last build: operated %d / %d (left / right ear), vowel %d / %d\n", bank.current(0).flagship.operated,
	            bank.current(1).flagship.operated, bank.current(0).vox.audible, bank.current(1).vox.audible);
	std::printf("8-sound STEREO build (both ears), %d builds per case, alternated in %d rounds\n",
	            kRounds * kBuildsPerRound, kRounds);
	for (Case& c : cases) {
		std::sort(c.runs.begin(), c.runs.end());
		const double median = c.runs[c.runs.size() / 2];
		std::printf("%-28s median %8.1f us (min %8.1f, p90 %8.1f)  = %5.2f%% of a 256-frame block at 48 kHz\n",
		            c.name, median, c.runs.front(), c.runs[c.runs.size() * 9 / 10], 100.0 * median / kBlockMicros);
	}
	return 0;
}
