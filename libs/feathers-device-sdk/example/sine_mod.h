/*
 * Copyright © 2026 Owlet Records
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the Feathers Device SDK, which is MIT-licensed (see LICENSE in the SDK's
 * root) so that devices of any licence, open or closed, may build against it. Feathers itself,
 * which vendors this SDK, is GPL-3.0-or-later.
 */

#pragma once

// **A THIRD-PARTY DEVICE IN MINIATURE: A SINE MODULATOR, BUILT AGAINST THE SDK ALONE.**
//
// Everything a host needs to run it is here: its controls (`ParamDesc`, keyed for the patch), its identity
// (`Traits`), its one modulation output (`SourceDesc`), and how to advance it (`ModOps`), gathered into the
// `Descriptor` a host's catalogue hands out. Nothing on its include path but the SDK and the standard library.
//
// THE CONTRACT IT KEEPS, as every modulator does: its state lives in bytes the HOST owns (`stateBytes`,
// `stateAlign`) and is built and destroyed in place; it allocates nothing; `setFields` takes the row raw, with the
// count that is really there, and gives an absent field its declared default; `tick` advances one control tick of
// `blockSamples` and `outputAt` answers what the tick published.

#include "device/device.h"
#include "device/registry.h"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <new>
#include <numbers>

namespace sine_mod {

using feathers::device::ParamDesc;
using feathers::device::Unit;

enum Param : size_t { kRate, kDepth, kCount };

/// THE KEY FIRST, as the vocabulary requires: it is what a patch stores the value under.
inline constexpr ParamDesc kParams[] = {
	{ "rate", "Rate", -4.0, 5.0, 1.0, false, Unit::LogHertz, {}, true }, // log2(Hz): 1/16 Hz .. 32 Hz, default 2 Hz
	{ "depth", "Depth", 0.0, 1.0, 1.0, false, Unit::Percent, {}, true },
};
static_assert(std::size(kParams) == kCount);
static_assert(feathers::device::validStateKey(kParams[kRate].key));
static_assert(feathers::device::validStateKey(kParams[kDepth].key));
static_assert(!feathers::device::sameKey(kParams[kRate].key, kParams[kDepth].key));

/// ONE OUTPUT, BIPOLAR: −depth .. depth.
inline constexpr feathers::device::SourceDesc kOutputs[] = {
	{ .key = "sine", .name = "Sine", .bipolar = true },
};

inline constexpr feathers::device::Traits kTraits {
	.tag = "sinemod",
	.name = "Sine Mod",
	.hasCard = false, // the host's generic card draws it: one knob per row, from the declarations
	.hasExpanded = false,
};

/// THE SAMPLES ONE TICK ADVANCES — the host calls `tick` once per this many.
inline constexpr size_t kBlockSamples = 64;

struct State {
	double sampleRate = 48000.0;
	double phase = 0.0; ///< 0..1, where the next tick reads
	float out = 0.0f;   ///< what the last tick published
	float fields[kCount] { static_cast<float>(kParams[kRate].def), static_cast<float>(kParams[kDepth].def) };
};

inline void construct(void* storage) { new (storage) State {}; }
inline void destruct(void* storage) { static_cast<State*>(storage)->~State(); }
inline void prepare(void* storage, double sampleRate) { static_cast<State*>(storage)->sampleRate = sampleRate; }
inline void reset(void* storage) {
	auto& s = *static_cast<State*>(storage);
	s.phase = 0.0;
	s.out = 0.0f;
}

inline void setFields(void* storage, const float* fields, size_t fieldsPresent) {
	auto& s = *static_cast<State*>(storage);
	for (size_t i = 0; i < kCount; ++i) {
		s.fields[i] = i < fieldsPresent ? fields[i] : static_cast<float>(kParams[i].def);
	}
}

/// THE VALUE AT `phase` — the device's whole sound, and what a test restates independently.
[[nodiscard]] inline float valueAt(double phase, float depth) noexcept {
	return depth * static_cast<float>(std::sin(2.0 * std::numbers::pi * phase));
}

/// ONE CONTROL TICK: publish the value where the phase stands, then advance it by one tick at the tick's rate.
inline void tick(void* storage, const feathers::device::ModTickContext& context) {
	auto& s = *static_cast<State*>(storage);
	const double hz = std::exp2(static_cast<double>(s.fields[kRate])) * static_cast<double>(context.rateMul);
	s.out = valueAt(s.phase, s.fields[kDepth]);
	s.phase = std::fmod(s.phase + hz * static_cast<double>(kBlockSamples) / s.sampleRate, 1.0);
}

[[nodiscard]] inline float outputAt(const void* storage, size_t output) {
	return output == 0 ? static_cast<const State*>(storage)->out : 0.0f;
}

inline constexpr feathers::device::ModOps kModOps {
	.stateBytes = sizeof(State),
	.stateAlign = alignof(State),
	.construct = construct,
	.destruct = destruct,
	.prepare = prepare,
	.reset = reset,
	.setFields = setFields,
	.blockSamples = kBlockSamples,
	.tick = tick,
	.outputAt = outputAt,
	.playhead = nullptr,
};

/// **WHAT A HOST'S CATALOGUE HANDS OUT FOR "sinemod"** — the device, entire.
inline constexpr feathers::device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParams,
	.outputs = kOutputs,
	.mod = &kModOps,
};

} // namespace sine_mod
