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

#include "fx/conv/test_ir.h"

#include <algorithm>
#include <cmath>

namespace feathers::fx::conv {

namespace {

/// xorshift32 → [-1, 1). Deterministic and portable: the IR is the same bits on every platform.
struct Noise {
	uint32_t state;
	float next() {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
		return static_cast<float>(static_cast<int32_t>(state) >> 8) * (1.0f / 8388608.0f);
	}
};

/// -60 dB over `rt60` seconds, as an amplitude at `t` seconds.
double decay(double t, double rt60) { return std::pow(10.0, -3.0 * t / rt60); }

void fill(std::vector<float>& out, double sr, uint32_t seed, const RoomSpec& spec) {
	Noise n {seed != 0 ? seed : 1u};
	const size_t frames = out.size();

	// THE DIFFUSE TAIL: two INDEPENDENT noises, one through four cascaded one-pole lowpasses at
	// `kBandSplitHz` and one through four cascaded one-pole highpasses there (-24 dB/octave each), each on
	// its own decay. MEASURED: a single one-pole split let the slow low band leak 9 dB into 8 kHz, which
	// then dominated once the highs had died; the highs measured 1.7 s against a designed 0.9.
	Noise m {(seed != 0 ? seed : 1u) ^ 0x5bd1e995u};
	const double a = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * spec.splitHz / sr);
	double lo[4] = {}, hi[4] = {};
	const auto tailStart = static_cast<size_t>(spec.tailStart * sr);
	const double fade = spec.tailFade * sr;
	for (size_t i = tailStart; i < frames; ++i) {
		double low = n.next();
		for (double& s : lo) {
			s += a * (low - s);
			low = s;
		}
		double high = m.next();
		for (double& s : hi) {
			s += a * (high - s);
			high -= s;
		}
		const double t = static_cast<double>(i) / sr;
		const double ramp = std::min(1.0, static_cast<double>(i - tailStart) / fade);
		out[i] = static_cast<float>(ramp * (low * decay(t, spec.rt60Low) + high * decay(t, spec.rt60High)));
	}

	// THE EARLY REFLECTIONS: sparse, loud, on top of the tail's first 60 ms.
	for (int k = 0; k < spec.earlyTaps; ++k) {
		const double u = 0.5 * (n.next() + 1.0f);
		const double t = spec.earlyFrom + spec.earlySpan * u;
		const auto at = static_cast<size_t>(t * sr);
		if (at >= frames) {
			continue;
		}
		const double sign = n.next() < 0.0f ? -1.0 : 1.0;
		const double gain = (0.4 + 0.6 * 0.5 * (n.next() + 1.0f)) * decay(t, spec.earlyDecay);
		out[at] += static_cast<float>(spec.earlyGain * sign * gain);
	}

	// SILENCE BEFORE THE PREDELAY (the earliest tap is past it, and the tail starts after it).
	const auto pre = std::min(frames, static_cast<size_t>(spec.predelay * sr));
	std::fill(out.begin(), out.begin() + static_cast<long>(pre), 0.0f);

	// UNIT ENERGY.
	double energy = 0.0;
	for (const float v : out) {
		energy += static_cast<double>(v) * static_cast<double>(v);
	}
	if (energy > 0.0) {
		const double g = 1.0 / std::sqrt(energy);
		for (float& v : out) {
			v = static_cast<float>(static_cast<double>(v) * g);
		}
	}
}


GeneratedIr generate(double sampleRate, double seconds, uint32_t seed, const RoomSpec& spec) {
	GeneratedIr ir;
	ir.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
	const auto frames = static_cast<size_t>(std::max(0.0, seconds) * ir.sampleRate);
	ir.left.assign(frames, 0.0f);
	ir.right.assign(frames, 0.0f);
	// DECORRELATED: the two channels are two different rooms of the same shape.
	fill(ir.left, ir.sampleRate, seed, spec);
	fill(ir.right, ir.sampleRate, seed * 2654435761u + 0x9e3779b9u, spec);
	return ir;
}

} // namespace

const RoomSpec& roomSpec(int which) noexcept {
	switch (which) {
	case kGeneratedPlate: return kPlateSpec;
	case kGeneratedHall: return kHallSpec;
	case kGeneratedAmbience: return kAmbienceSpec;
	default: return kRoomSpec;
	}
}

const char* generatedRoomName(int which) noexcept {
	switch (which) {
	case kGeneratedPlate: return "Plate";
	case kGeneratedHall: return "Hall";
	case kGeneratedAmbience: return "Ambience";
	default: return "Room";
	}
}

GeneratedIr generateTestIr(double sampleRate, double seconds, uint32_t seed) {
	return generate(sampleRate, seconds, seed, kRoomSpec);
}

GeneratedIr generateRoom(int which, double sampleRate, uint32_t seed) {
	const RoomSpec& spec = roomSpec(which);
	return generate(sampleRate, spec.seconds, seed, spec);
}

} // namespace feathers::fx::conv
