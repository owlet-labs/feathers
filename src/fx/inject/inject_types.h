/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "device/device.h"
#include "device/registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// SIDECHAIN INJECT (backlog NW 2026-10-18: a device with mix and gain knobs that materialises the sidechain
// anywhere in a chain) — the plugin's Sidechain input, brought into the chain AT THIS DEVICE'S
// POSITION. `out = (1 - Mix) * passing + Mix * Gain * sidechain`: at Mix 100% the sidechain replaces what was passing
// (the default, so a fresh Inject is the sidechain, not a no-op), at 0% the passing signal is untouched to the bit.
//
// WHAT FOLLOWS IT SEES THE SIDECHAIN: a Follower in Audio mode after it follows the sidechain's level (this replaced the
// Follower's own Sidechain source), a vocoder after it has a carrier-and-modulator mix to work on, a compressor ducks
// by it. It reads the sidechain the way Spectral's Vocode does — `AudioOps::setSidechain`, BUS ROAD ONLY, so inside a
// Chain it works and a per-voice instance never hears one — and says "sidechain silent" when nothing sounds there
// (`device::SidechainPresence`, the honesty rule). NO LATENCY, NO TAIL: it is a mix of two buffers the host already
// gave this block, so whether the plugin may sleep is the plugin's own sidechain rule (it stays awake while the
// sidechain sounds, feathers_clap.cpp).

namespace feathers::fx::inject {

/// THE PLUGIN'S ID BLOCK IS ASSIGNED IN THIS ORDER, so a device row is index-aligned with this enum.
enum class Param : int {
	Enabled,
	/// PASSING SIGNAL AGAINST SIDECHAIN, 0..1: 0 is exactly what arrived, 1 is exactly the sidechain times Gain. Crossfaded
	/// in the two-ended form `(1-m)*x + m*s`, exact at both ends (COMP's Mix has the reasoning).
	Mix,
	/// THE SIDECHAIN'S LEVEL INTO THE MIX, in dB; the passing signal is not scaled by it.
	Gain,
	kCount,
};

inline constexpr double kGainMinDb = -24.0;
inline constexpr double kGainMaxDb = 24.0;

struct Settings {
	bool enabled = true;
	float mix = 1.0f;
	float gainDb = 0.0f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// WHAT THE METER PUBLISHES, IN THIS ORDER — the contract between `processStereo` and the card.
enum class Reading : size_t {
	/// LINEAR sample peak of what the sidechain adds (Gain and Mix included), held 50 ms.
	Injected,
	/// 1 WHILE SOMETHING SOUNDS ON THE PLUGIN'S SIDECHAIN (`device::SidechainPresence`), else 0. The card prints
	/// "sidechain silent" from it.
	SidechainSounding,
	kCount,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kTags[] = { "sidechain", "utility", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "inject",
	.name = "Sidechain Inject",
	.hasCard = true,
	.hasExpanded = false,
	// BUS ONLY: the per-voice road hears no sidechain (`AudioOps::setSidechain` is the bus road's).
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// ITS MIX IS ITS OWN (backlog NR D2): a plain crossfade, but of the passing signal against ANOTHER signal, which the
	// slot's blend (a mix against what entered) could not make: it would blend the sidechain-replaced signal with itself.
	.mixControl = static_cast<int>(Param::Mix),
	.mixInDevice = true,
};

inline constexpr device::ParamDesc kParamDescs[] = {
    {.key = "inject_enabled", .name = "Inject Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the sidechain is brought into the chain here"},
    {.key = "inject_mix", .name = "Inject Mix", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "The passing signal against the sidechain — 0% is exactly what arrives, 100% is the sidechain alone"},
    {.key = "inject_gain", .name = "Inject Gain", .min = kGainMinDb, .max = kGainMaxDb, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "How loud the sidechain comes in — the passing signal is not turned by it"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

namespace detail {
[[nodiscard]] constexpr float defOf(Param p) {
	return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
}
} // namespace detail
static_assert(Settings {}.enabled == (detail::defOf(Param::Enabled) >= 0.5f));
static_assert(Settings {}.mix == detail::defOf(Param::Mix));
static_assert(Settings {}.gainDb == detail::defOf(Param::Gain));

/// A FIELD, CLAMPED TO ITS RANGE; an absent one is its DECLARED default, never zero, and a NaN is the default too.
[[nodiscard]] inline float fieldOf(const Row& row, size_t present, Param p) {
	const size_t i = static_cast<size_t>(p);
	const auto& d = kParamDescs[i];
	const float v = i < present && !std::isnan(row[i]) ? row[i] : static_cast<float>(d.def);
	return std::clamp(v, static_cast<float>(d.min), static_cast<float>(d.max));
}

[[nodiscard]] inline Settings stateOf(const Row& row, size_t fieldsPresent) {
	Settings s;
	s.enabled = fieldOf(row, fieldsPresent, Param::Enabled) >= 0.5f;
	s.mix = fieldOf(row, fieldsPresent, Param::Mix);
	s.gainDb = fieldOf(row, fieldsPresent, Param::Gain);
	return s;
}

[[nodiscard]] inline Row rowOf(const Settings& s) {
	Row row {};
	row[static_cast<size_t>(Param::Enabled)] = s.enabled ? 1.0f : 0.0f;
	row[static_cast<size_t>(Param::Mix)] = s.mix;
	row[static_cast<size_t>(Param::Gain)] = s.gainDb;
	return row;
}

extern const device::AudioOps kAudioOps;

/// TEST ONLY — the linear gain the DSP is applying to the sidechain now (Gain, ramped). The level of a buffer cannot say
/// which of Gain and the sidechain's own level it came from.
[[nodiscard]] float gainLinForTest(const void* storage);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// NO OUTPUTS: an effect, not a modulator. Spelled because every field of this struct is a decision.
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::Utility,
};

} // namespace feathers::fx::inject
