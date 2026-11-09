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

#include "plugin/stack_index.h"

#include "character/phi_triangle.hpp"

#include <algorithm>
#include <cmath>

namespace feathers {

namespace {

constexpr float kPi = 3.14159265f; // the firmware's own spelling, so Sine agrees with it to the bit

int clampPart(int u, int count) {
	return std::clamp(u, 0, count - 1);
}

} // namespace

float stackHash(int part, uint32_t seed) {
	// The firmware's Random zone, with its seed handed in rather than taken from the knob's whole raw value — that
	// is what made every step of its meta a new, unrelated draw (doc §1.4).
	uint32_t h = static_cast<uint32_t>(part) * 2654435761u + seed * 340573321u;
	h ^= h >> 16;
	h *= 0x45d9f3bu;
	h ^= h >> 16;
	return std::clamp(static_cast<float>(static_cast<int32_t>(h)) / 2147483648.0f, -1.0f, 1.0f);
}

StackValues stackShapeValues(StackShape shape, float amount, double freePhase, uint32_t freshSeed, int count) {
	StackValues raw {};
	if (count <= 1) {
		return raw;
	}
	const float meta = std::clamp(amount, 0.0f, 1.0f);
	for (int u = 0; u < count; ++u) {
		const float t = static_cast<float>(u) / static_cast<float>(count - 1);
		const float bipolar = t * 2.0f - 1.0f;
		float v = bipolar;
		switch (shape) {
		case StackShape::Linear: // the amount biases the fan upward, clamped at +1
			v = std::clamp(bipolar + meta, -1.0f, 1.0f);
			break;
		case StackShape::Power: { // exponent 0.5 → 3
			const float exponent = 0.5f + meta * 2.5f;
			const float sign = bipolar >= 0.0f ? 1.0f : -1.0f;
			v = sign * std::pow(std::abs(bipolar), exponent);
			break;
		}
		case StackShape::SCurve: { // tanh steepness 1 → 10
			const float k = 1.0f + meta * 9.0f;
			v = std::tanh(bipolar * k) / std::tanh(k);
			break;
		}
		case StackShape::Step: { // 2 → 8 levels
			const int levels = 2 + static_cast<int>(meta * 6.99f);
			v = std::round(bipolar * static_cast<float>(levels - 1)) / static_cast<float>(levels - 1);
			break;
		}
		case StackShape::Triangle: { // the peak's place, first part → last
			const float peak = meta;
			const float dist = std::abs(t - peak) / std::max(peak, 1.0f - peak);
			v = 1.0f - 2.0f * std::clamp(dist, 0.0f, 1.0f);
			break;
		}
		case StackShape::Sine: { // 0.5 → 4 cycles
			const float cycles = 0.5f + meta * 3.5f;
			v = std::sin(bipolar * cycles * kPi);
			break;
		}
		case StackShape::Random: // one fixed draw; the amount is its depth
			v = meta * stackHash(u, kStackRandomSeed);
			break;
		case StackShape::RandomFresh: // a draw per note
			v = meta * stackHash(u, freshSeed);
			break;
		case StackShape::Drift: // the base the walk goes around: the plain fan
			v = bipolar;
			break;
		case StackShape::Free: { // the phi field, each part a golden step further on
			static constexpr owlet::dsp::phi::PhiTriConfig kField { 1.0f, 1.0f, 0.0f, true };
			v = owlet::dsp::phi::evalTriangle(
			    freePhase + static_cast<double>(u) * static_cast<double>(owlet::dsp::phi::kPhiN100), 1.0f, kField);
			break;
		}
		case StackShape::kCount:
			break;
		}
		raw[static_cast<size_t>(u)] = v;
	}
	return raw;
}

void stackCurveWarp(float curve, int count, StackValues& values) {
	if (curve == 0.0f) {
		return;
	}
	const float c = std::clamp(curve, -1.0f, 1.0f);
	// The firmware's −50..+50 at 0.06 a step is 3 over the whole side: exponent 4 at the top, ¼ at the bottom.
	const float exponent = c > 0.0f ? 1.0f + 3.0f * c : 1.0f / (1.0f + 3.0f * -c);
	for (int u = 0; u < count; ++u) {
		float& v = values[static_cast<size_t>(u)];
		const float sign = v >= 0.0f ? 1.0f : -1.0f;
		v = sign * std::pow(std::abs(v), exponent);
	}
}

StackValues stackMapValues(StackMap map, float amount, const StackValues& shaped, int count) {
	StackValues out {};
	if (count <= 1) {
		return out;
	}
	const int n = count;
	const float meta = std::clamp(amount, 0.0f, 1.0f);
	std::array<int, kMaxStack> perm {};
	for (int u = 0; u < n; ++u) {
		perm[static_cast<size_t>(u)] = u;
	}
	const auto mag = [&](int u) { return std::abs(shaped[static_cast<size_t>(u)]); };
	switch (map) {
	case StackMap::AsShaped:
		break;
	case StackMap::Symmetric: {
		// Loose alternation at 0; toward 1, the largest magnitudes go to the parts panned furthest out.
		const auto loose = [n](int u) { return (std::min(u, n - 1 - u) & 1) != 0 ? n - 1 - u : u; };
		if (meta < 0.01f) {
			for (int u = 0; u < n; ++u) {
				perm[static_cast<size_t>(u)] = loose(u);
			}
			break;
		}
		std::array<int, kMaxStack> magOrder {};
		std::array<int, kMaxStack> panOrder {};
		for (int u = 0; u < n; ++u) {
			magOrder[static_cast<size_t>(u)] = u;
			panOrder[static_cast<size_t>(u)] = u;
		}
		// Insertion sorts, the firmware's, so equal keys keep their order as its do.
		for (int i = 1; i < n; ++i) {
			const int key = magOrder[static_cast<size_t>(i)];
			int j = i - 1;
			while (j >= 0 && mag(magOrder[static_cast<size_t>(j)]) > mag(key)) {
				magOrder[static_cast<size_t>(j + 1)] = magOrder[static_cast<size_t>(j)];
				--j;
			}
			magOrder[static_cast<size_t>(j + 1)] = key;
		}
		for (int i = 1; i < n; ++i) {
			const int key = panOrder[static_cast<size_t>(i)];
			const int keyDist = std::abs(2 * key - (n - 1));
			int j = i - 1;
			while (j >= 0 && std::abs(2 * panOrder[static_cast<size_t>(j)] - (n - 1)) > keyDist) {
				panOrder[static_cast<size_t>(j + 1)] = panOrder[static_cast<size_t>(j)];
				--j;
			}
			panOrder[static_cast<size_t>(j + 1)] = key;
		}
		for (int i = 0; i < n; ++i) {
			const int at = panOrder[static_cast<size_t>(i)];
			const float blended = static_cast<float>(loose(at)) * (1.0f - meta)
			                      + static_cast<float>(magOrder[static_cast<size_t>(i)]) * meta;
			perm[static_cast<size_t>(at)] = clampPart(static_cast<int>(blended + 0.5f), n);
		}
		break;
	}
	case StackMap::AntiSym:
		if (meta < 0.01f) {
			break; // identity
		}
		if (meta > 0.99f) {
			for (int u = 0; u < n; ++u) {
				perm[static_cast<size_t>(u)] = (std::min(u, n - 1 - u) & 1) != 0 ? u : n - 1 - u;
			}
			break;
		}
		{
			// Swapped from the edges inward, more pairs as the amount grows.
			const int swaps = std::max(1, static_cast<int>(meta * static_cast<float>(n / 2) + 0.5f));
			for (int i = 0; i < swaps && i < n / 2; ++i) {
				perm[static_cast<size_t>(i)] = n - 1 - i;
				perm[static_cast<size_t>(n - 1 - i)] = i;
			}
		}
		break;
	case StackMap::PitchUp:
		if (meta > 0.01f) {
			const float exponent = 1.0f + meta * 3.0f;
			for (int u = 0; u < n; ++u) {
				const float t = static_cast<float>(u) / static_cast<float>(n - 1);
				perm[static_cast<size_t>(u)] =
				    clampPart(static_cast<int>(std::pow(t, exponent) * static_cast<float>(n - 1) + 0.5f), n);
			}
		}
		break;
	case StackMap::PitchDown:
		if (meta < 0.01f) {
			for (int u = 0; u < n; ++u) {
				perm[static_cast<size_t>(u)] = n - 1 - u;
			}
		}
		else {
			const float exponent = 1.0f + meta * 3.0f;
			for (int u = 0; u < n; ++u) {
				const float t = static_cast<float>(n - 1 - u) / static_cast<float>(n - 1);
				perm[static_cast<size_t>(u)] =
				    clampPart(static_cast<int>(std::pow(t, exponent) * static_cast<float>(n - 1) + 0.5f), n);
			}
		}
		break;
	case StackMap::CenterOut: {
		// The focus part takes the top value and the parts furthest from it the bottom one.
		const float focus = meta * static_cast<float>(n - 1);
		for (int u = 0; u < n; ++u) {
			const float dist = std::abs(static_cast<float>(u) - focus);
			const float maxDist = std::max(focus, static_cast<float>(n - 1) - focus);
			const float normalised = maxDist > 0.0f ? dist / maxDist : 0.0f;
			perm[static_cast<size_t>(u)] =
			    clampPart(static_cast<int>((1.0f - normalised) * static_cast<float>(n - 1) + 0.5f), n);
		}
		break;
	}
	case StackMap::Rotate: {
		const int shift = static_cast<int>(meta * static_cast<float>(n - 1));
		for (int u = 0; u < n; ++u) {
			perm[static_cast<size_t>(u)] = (u + shift) % n;
		}
		break;
	}
	case StackMap::Pairs: {
		// THE FIX: each group of 2..4 neighbours takes the MEAN of its members, so the fan stays centred and its
		// top value is heard. The firmware's `(u / g) · g` handed each group its first member's value.
		const int group = 2 + static_cast<int>(meta * 2.99f);
		for (int start = 0; start < n; start += group) {
			const int end = std::min(n, start + group);
			float sum = 0.0f;
			for (int u = start; u < end; ++u) {
				sum += shaped[static_cast<size_t>(u)];
			}
			const float mean = sum / static_cast<float>(end - start);
			for (int u = start; u < end; ++u) {
				out[static_cast<size_t>(u)] = mean;
			}
		}
		return out;
	}
	case StackMap::Shuffle: {
		// SEEDED AS THE FIRMWARE SEEDS IT — the zone's raw value, Shuffle's zone 7 and the amount in 1024 steps — so
		// each step of the amount is its own shuffle, and the same one the firmware deals.
		const auto step = static_cast<uint32_t>(std::lround(meta * 1023.0f));
		uint32_t seed = (7u * 1024u + step) * 2654435761u;
		for (int i = n - 1; i > 0; --i) {
			seed ^= seed >> 16;
			seed *= 0x45d9f3bu;
			seed ^= seed >> 16;
			const int j = static_cast<int>(seed % static_cast<uint32_t>(i + 1));
			std::swap(perm[static_cast<size_t>(i)], perm[static_cast<size_t>(j)]);
		}
		break;
	}
	case StackMap::kCount:
		break;
	}
	for (int u = 0; u < n; ++u) {
		out[static_cast<size_t>(u)] = shaped[static_cast<size_t>(clampPart(perm[static_cast<size_t>(u)], n))];
	}
	return out;
}

StackValues stackIndexSpread(const StackIndexSettings& s) {
	if (s.count <= 1) {
		return {};
	}
	const int count = std::min(s.count, kMaxStack);
	StackValues values = stackShapeValues(s.shape, s.shapeAmount, s.freePhase, s.freshSeed, count);
	stackCurveWarp(s.curve, count, values);
	return stackMapValues(s.map, s.mapAmount, values, count);
}

void stackDriftStep(float amount, int count, int samples, uint32_t& rng, StackValues& offsets) {
	const float meta = std::clamp(amount, 0.0f, 1.0f);
	const double blocks = static_cast<double>(samples) / 128.0;
	// q31 per block, read as fractions of full scale: the noise term is noise · rate >> 32, the decay rate / 8.
	const double rate = (static_cast<double>(meta) * meta * 20000000.0 + 50000.0) / 4294967296.0;
	const double step = rate * std::sqrt(blocks);
	const double decay = rate / 8.0 * blocks;
	for (int u = 0; u < count && u < kMaxStack; ++u) {
		rng ^= rng << 13;
		rng ^= rng >> 17;
		rng ^= rng << 5;
		const double noise = static_cast<double>(static_cast<int32_t>(rng)) / 2147483648.0;
		double o = offsets[static_cast<size_t>(u)];
		o += noise * step;
		o -= o * decay;
		offsets[static_cast<size_t>(u)] = static_cast<float>(o);
	}
}

} // namespace feathers
