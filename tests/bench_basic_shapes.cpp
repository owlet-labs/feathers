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

// THE BASIC SHAPES BENCHMARK — not a test. One voice (4x + decimator) rendering a held note, ns per host
// sample, for V5 and for Basic Shapes, each with and without FM. The cases are ALTERNATED round by round and
// the median of each is printed, because this host is shared and load swings within a run (CLAUDE.md).

#include "dsp/basic_shapes.h"
#include "voice_harness.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

namespace {

using feathers::Character;
using feathers::OscEngine;

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 256;
constexpr int kBlocks = 400;
constexpr int kRounds = 15;

Character withFm(Character c, float depth) {
	for (auto& s : c.sounds) {
		s.fmDepth = depth;
	}
	return c;
}

/// A warp at full depth: every sample reads through warpRead, on the duller mip the warp's maximum slope picks.
Character withWarp(Character c, feathers::WarpMode mode) {
	c.warpMode = mode;
	for (auto& s : c.sounds) {
		s.warpDepth = 1.0f;
	}
	return c;
}

/// An operator knob at full on every sound (backlog KV): the mix is baked on V5's grid and played down V5's grid road.
Character withQuantize(Character c) {
	for (auto& s : c.sounds) {
		s.quantizeScale = 1.0f;
	}
	return c;
}

double nsPerSample(const Character& c) {
	feathers::test::BankedVoice v;
	v.setSampleRate(kSampleRate);
	v.setFrequency(220.0);
	v.setCharacter(c);
	std::vector<float> out(kBlock);
	v.render(out.data(), kBlock);
	const auto t0 = std::chrono::steady_clock::now();
	for (int b = 0; b < kBlocks; ++b) {
		v.render(out.data(), kBlock);
	}
	const auto t1 = std::chrono::steady_clock::now();
	return std::chrono::duration<double, std::nano>(t1 - t0).count() / (kBlocks * kBlock);
}

} // namespace

int main() {
	Character v5 = feathers::test::characterAt(OscEngine::Morph, 17);
	v5.flagship = true;
	Character sine = feathers::test::characterAt(OscEngine::Shapes, 17);
	Character four = sine;
	four.shapes = { 1.0f, 1.0f, 1.0f, 1.0f };
	struct Case {
		const char* name;
		Character c;
		std::vector<double> runs;
	};
	std::vector<Case> cases { { "V5 (zone 17)", v5, {} },
		                      { "V5 + FM 100%", withFm(v5, 1.0f), {} },
		                      { "Basic Shapes sine", sine, {} },
		                      { "Basic Shapes all four", four, {} },
		                      { "Basic Shapes all four + FM 100%", withFm(four, 1.0f), {} },
		                      { "Basic Shapes all four + Bend+ 100%", withWarp(four, feathers::WarpMode::BendPlus), {} },
		                      { "Basic Shapes all four + PWM 100%", withWarp(four, feathers::WarpMode::Pwm), {} },
		                      { "Basic Shapes all four + Quantize 100%", withQuantize(four), {} },
		                      { "Basic Shapes all four + Quantize + FM 100%", withFm(withQuantize(four), 1.0f), {} } };
	for (int r = 0; r < kRounds; ++r) {
		for (Case& c : cases) {
			c.runs.push_back(nsPerSample(c.c));
		}
	}
	for (Case& c : cases) {
		std::sort(c.runs.begin(), c.runs.end());
		std::printf("%-44s median %7.1f ns/sample (min %7.1f)\n", c.name, c.runs[c.runs.size() / 2], c.runs.front());
	}
	return 0;
}
