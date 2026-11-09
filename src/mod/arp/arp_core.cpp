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

#include "mod/arp/arp_core.h"

#include <algorithm>
#include <cmath>

namespace feathers::mod::arp {

//------------------------------------------------------------------------------------------------//
// The row                                                                                        //
//------------------------------------------------------------------------------------------------//

Row rowOf(const Settings& s) {
	Row row {};
	row[at(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[at(Param::TimeBase)] = static_cast<float>(s.timeBase);
	row[at(Param::Rate)] = s.rate;
	row[at(Param::Order)] = static_cast<float>(s.order);
	row[at(Param::Octaves)] = static_cast<float>(s.octaves);
	row[at(Param::OctaveMode)] = static_cast<float>(s.octaveMode);
	row[at(Param::Gate)] = s.gate;
	row[at(Param::Feel)] = static_cast<float>(s.feel);
	row[at(Param::FeelAmount)] = s.feelAmount;
	row[at(Param::FreePhase)] = static_cast<float>(s.freePhase);
	row[at(Param::Wobble)] = static_cast<float>(s.wobble);
	row[at(Param::FixedVelocity)] = s.fixedVelocity ? 1.0f : 0.0f;
	row[at(Param::Velocity)] = s.velocity;
	row[at(Param::Latch)] = s.latch ? 1.0f : 0.0f;
	row[at(Param::Offset)] = static_cast<float>(s.offset);
	row[at(Param::Nudge)] = s.nudge;
	row[at(Param::Run)] = static_cast<float>(s.run);
	row[at(Param::Seed)] = static_cast<float>(s.seed);
	return row;
}

Settings settingsOf(const float* fields, size_t fieldsPresent) {
	const auto value = [&](Param p) {
		const size_t i = at(p);
		const device::ParamDesc& d = kParamDescs[i];
		const float v = fields != nullptr && i < fieldsPresent && std::isfinite(fields[i]) ? fields[i]
		                                                                                   : static_cast<float>(d.def);
		return std::clamp(v, static_cast<float>(d.min), static_cast<float>(d.max));
	};
	const auto whole = [&](Param p) { return static_cast<int>(std::lround(value(p))); };
	Settings s;
	s.enabled = value(Param::Enabled) >= 0.5f;
	s.timeBase = static_cast<size_t>(whole(Param::TimeBase));
	s.rate = value(Param::Rate);
	s.order = static_cast<Order>(whole(Param::Order));
	s.octaves = whole(Param::Octaves);
	s.octaveMode = static_cast<OctaveMode>(whole(Param::OctaveMode));
	s.gate = value(Param::Gate);
	s.feel = static_cast<size_t>(whole(Param::Feel));
	s.feelAmount = value(Param::FeelAmount);
	s.freePhase = static_cast<double>(value(Param::FreePhase));
	s.wobble = static_cast<double>(value(Param::Wobble));
	s.fixedVelocity = value(Param::FixedVelocity) >= 0.5f;
	s.velocity = value(Param::Velocity);
	s.latch = value(Param::Latch) >= 0.5f;
	s.offset = whole(Param::Offset);
	s.nudge = value(Param::Nudge);
	s.run = static_cast<RunMode>(std::clamp(whole(Param::Run), 0, 2));
	s.seed = whole(Param::Seed);
	return s;
}

static_assert(Settings {}.enabled == (kParamDescs[at(Param::Enabled)].def >= 0.5));
static_assert(Settings {}.timeBase == static_cast<size_t>(kParamDescs[at(Param::TimeBase)].def));
static_assert(Settings {}.latch == (kParamDescs[at(Param::Latch)].def >= 0.5));
static_assert(static_cast<double>(Settings {}.run) == kParamDescs[at(Param::Run)].def);

//------------------------------------------------------------------------------------------------//
// The chord                                                                                      //
//------------------------------------------------------------------------------------------------//

HeldNote* HeldChord::find(int16_t key) noexcept {
	for (size_t i = 0; i < count_; ++i) {
		if (notes_[i].key == key) {
			return &notes_[i];
		}
	}
	return nullptr;
}

void HeldChord::press(int16_t key, int16_t channel, float velocity, uint32_t order) noexcept {
	(void)release(key); // a second press of a held key moves it to the end of the press order
	if (count_ == kMaxHeld) {
		std::copy(notes_.begin() + 1, notes_.end(), notes_.begin());
		--count_;
	}
	notes_[count_++] = HeldNote { key, channel, velocity, order, false };
}

bool HeldChord::release(int16_t key) noexcept {
	size_t write = 0;
	for (size_t i = 0; i < count_; ++i) {
		if (notes_[i].key != key) {
			notes_[write++] = notes_[i];
		}
	}
	const bool found = write != count_;
	count_ = write;
	return found;
}

//------------------------------------------------------------------------------------------------//
// The pattern                                                                                    //
//------------------------------------------------------------------------------------------------//

size_t buildPattern(const HeldChord& chord, Order order, int octaves, OctaveMode mode, PatternNote* out) noexcept {
	// THE BASE ORDER: by pitch, or as pressed (Played). Down and the Up-Downs are made from Up below.
	std::array<HeldNote, kMaxHeld> base {};
	const size_t n = chord.size();
	for (size_t i = 0; i < n; ++i) {
		base[i] = chord[i];
	}
	if (order == Order::Played) {
		std::stable_sort(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(n),
		                 [](const HeldNote& a, const HeldNote& b) { return a.order < b.order; });
	}
	else {
		std::stable_sort(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(n),
		                 [](const HeldNote& a, const HeldNote& b) { return a.key < b.key; });
	}
	const int o = std::clamp(octaves, 1, kMaxOctaves);
	size_t len = 0;
	const auto put = [&](const HeldNote& h, int octave) {
		const int key = h.key + 12 * octave;
		if (key > 127) {
			return;
		}
		out[len++] = PatternNote { static_cast<int16_t>(key), h.channel, h.velocity };
	};
	if (mode == OctaveMode::EachNote) {
		for (size_t i = 0; i < n; ++i) {
			for (int k = 0; k < o; ++k) {
				put(base[i], k);
			}
		}
	}
	else {
		for (int k = 0; k < o; ++k) {
			for (size_t i = 0; i < n; ++i) {
				put(base[i], k);
			}
		}
	}
	switch (order) {
	case Order::Up:
	case Order::Played:
	case Order::Random:
		break;
	case Order::Down:
		std::reverse(out, out + len);
		break;
	case Order::UpDown:
		// THE ENDS ONCE: up, then back down without the top and the bottom again — 2n − 2 steps.
		if (len > 2) {
			for (size_t i = len - 1; i-- > 1;) {
				out[len + (len - 2 - i)] = out[i];
			}
			len = 2 * len - 2;
		}
		break;
	case Order::UpDownRepeat:
		// THE ENDS TWICE: up, then the same notes down — 2n steps, the top and the bottom each played twice in a row.
		for (size_t i = 0; i < len; ++i) {
			out[len + i] = out[len - 1 - i];
		}
		len *= 2;
		break;
	case Order::kCount:
		break;
	}
	return len;
}

uint64_t stepDice(int seed, int64_t step) noexcept {
	// SPLITMIX64 over the seed and the step: a hash, not a running generator, so a step's draw needs nothing before it.
	uint64_t z = (static_cast<uint64_t>(static_cast<uint32_t>(seed)) << 32) ^ static_cast<uint64_t>(step)
	             ^ 0x9E3779B97F4A7C15ull;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
	return z ^ (z >> 31);
}

size_t entryAt(Order order, int64_t step, size_t length, int seed) noexcept {
	if (order == Order::Random) {
		return static_cast<size_t>(stepDice(seed, step) % length);
	}
	const auto len = static_cast<int64_t>(length);
	return static_cast<size_t>(((step % len) + len) % len);
}

//------------------------------------------------------------------------------------------------//
// The clock                                                                                      //
//------------------------------------------------------------------------------------------------//

double StepGrid::onset(int64_t g) const noexcept {
	const bool even = (g % 2) == 0; // slot 0 is the first step; the SECOND, fourth... are the swung ones
	const double base = static_cast<double>(g) + nudge + (even ? 0.0 : swing * 0.5);
	return shift == nullptr ? base : base + shift(shiftCtx, g);
}

int64_t StepGrid::firstAtOrAfter(double u, double tolerance) const noexcept {
	// Onsets rise with g and never move more than a step from g itself, so the answer is within two slots of u.
	auto g = static_cast<int64_t>(std::floor(u - nudge)) - 2;
	while (onset(g) < u - tolerance) {
		++g;
	}
	return g;
}

double stepBeats(const Settings& s) noexcept {
	const size_t d = std::min(s.timeBase == 0 ? size_t { 0 } : s.timeBase - 1, device::kDivisionCount - 1);
	return device::kDivisionBeats[d] * timeSeconds(s.rate);
}

double stepHz(const Settings& s) noexcept {
	return timeSeconds(s.rate);
}

} // namespace feathers::mod::arp
