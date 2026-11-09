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

// THE STACK'S LIKENESS READ, PRICED — not a test (Task 9b quality round). `Synth::updateStackCoherence` reads at most one
// part's playing cycle per voice per chunk, on the audio thread, whenever that part's table moved: 256 points of
// `OscVoice::playingCycle`. A table part (V5) is 256 table evaluations; a Partials part sums up to kPartialsCycleCap
// partials by recurrence. Both measured here, ALTERNATED round by round with the median printed (the host is shared,
// CLAUDE.md), at C2 (the most partials below Nyquist) and C4.

#include "dsp/osc_voice.h"
#include "voice_harness.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

using feathers::Character;
using feathers::OscEngine;
using feathers::test::BankedVoice;

constexpr double kRate = 48000.0;
constexpr int kReadsPerRound = 200;
constexpr int kRounds = 31;

std::unique_ptr<BankedVoice> voiceFor(const Character& c, double hz) {
	auto v = std::make_unique<BankedVoice>();
	v->setSampleRate(kRate);
	v->setCharacter(c);
	v->reset();
	v->setFrequency(hz);
	std::vector<float> warm(512);
	v->render(warm.data(), static_cast<int>(warm.size())); // synced to its table, as a part the synth reads
	return v;
}

/// Median µs per playingCycle(256) read over the round's reads.
double perRead(const BankedVoice& v) {
	std::array<float, 256> out {};
	volatile float sink = 0.0f;
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < kReadsPerRound; ++i) {
		v.voice.playingCycle(out.data(), 256);
		sink = sink + out[static_cast<size_t>(i & 255)];
	}
	(void)sink;
	return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / kReadsPerRound;
}

double median(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

} // namespace

int main() {
	Character v5 = feathers::test::characterAt(OscEngine::Morph, 313, 1.7f);
	v5.flagship = true;
	const Character additive = feathers::test::characterAt(OscEngine::Partials, 313, 1.7f);
	struct Arm {
		const char* name;
		std::unique_ptr<BankedVoice> voice;
		std::vector<double> times;
	};
	std::array<Arm, 4> arms { { { "V5 table, C4", voiceFor(v5, 261.63), {} },
		                        { "V5 table, C2", voiceFor(v5, 65.41), {} },
		                        { "Additive, C4", voiceFor(additive, 261.63), {} },
		                        { "Additive, C2", voiceFor(additive, 65.41), {} } } };
	for (int round = 0; round < kRounds; ++round) {
		for (size_t k = 0; k < arms.size(); ++k) {
			Arm& arm = arms[(k + static_cast<size_t>(round)) % arms.size()];
			arm.times.push_back(perRead(*arm.voice));
		}
	}
	std::printf("one playingCycle(256) read, median of %d rounds x %d reads (cap %d partials):\n", kRounds, kReadsPerRound,
	            feathers::OscVoice::kPartialsCycleCap);
	for (const Arm& arm : arms) {
		std::printf("  %-14s %8.2f us  (partials live %d)\n", arm.name, median(arm.times), arm.voice->voice.partialsLiveForTest());
	}
	std::printf("a 64-sample chunk at 48 kHz is %.0f us\n", 64.0 / kRate * 1e6);
	return 0;
}
