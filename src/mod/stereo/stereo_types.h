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

#pragma once

#include "device/ears.h"
#include "device/registry.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
// THE STEREO TOOL — the sources stereo modulation needs that no modulator is (backlog KU, design §3). Global, not
// per voice, like Math. Every output is an EAR PAIR; its mono reading (`outputAt`) is its left ear, because a mono
// destination reads the left ear.
//
// TWO OUTPUTS SINCE 2026-10-05: Identity and Pan are gone, replaced by SPLIT. Pan was an
// equal-power gain pair — both ears ≥ 0 — so a bipolar LFO routed into it gave both ears POSITIVE offsets, which
// read as broken. Split takes In (any source routed into it) and sends it to the left ear as it is and to the right
// ear inverted by Amount: "at 100% it is full inversion", "at very low, near zero amount, the signal should
// basically be the input signal in mono", and "to get it to do identity, you just turn the knobs to 100%".

namespace feathers::mod::stereo {

/// ORDER IS THE ROW ORDER AND THE DESCRIPTOR ORDER.
enum class Param : size_t { In = 0, Amount, Width, Swap, Enabled, kCount };
/// ORDER IS THE OUTPUT ORDER.
enum class Output : size_t { Split = 0, Width, kCount };

/// **ONE OUTPUT FROM THE CONTROLS, AS RESOLVED** — the device's arithmetic and the card's scope's, one function.
/// Split, with i = In's LEFT ear and a = Amount (0..2): a ≤ 1 → (i, i·(1 − 2a)); a > 1 → (i·a, −i·a). The left ear
/// is the reference, so a mono destination sees the input; 0% is mono, 50% silences the right, 100% inverts it,
/// and past 100% both ears grow (continuous at 100%: (i, −i) from either side). NOT clamped — 200% is ±2i.
/// Width: In's pair re-imaged as mid ± width·side, side = (R − L)/2, clamped to ±1, then swapped if asked.
[[nodiscard]] inline device::EarPair evaluate(Output out, float inL, float inR, float amount, float width, bool swap) noexcept {
	switch (out) {
	case Output::Split: {
		const float a = std::clamp(amount, 0.0f, 2.0f);
		if (a <= 1.0f) {
			return { inL, inL * (1.0f - 2.0f * a) };
		}
		return { inL * a, -inL * a };
	}
	case Output::Width: {
		const float mid = 0.5f * (inL + inR);
		const float side = 0.5f * (inR - inL);
		const float l = std::clamp(mid - width * side, -1.0f, 1.0f);
		const float r = std::clamp(mid + width * side, -1.0f, 1.0f);
		return swap ? device::EarPair { r, l } : device::EarPair { l, r };
	}
	default: return {};
	}
}

inline constexpr const char* const kOffOnNames[] = { "Off", "On" };

inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "in", .name = "In", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "What Split and Width work on — rests at 0; route a source into it",
	  .centred = true, .stereo = true },
	{ .key = "amount", .name = "Amount", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true,
	  .tooltip = "How far Split turns In's right ear against its left — 0 is mono, 100% fully inverted, 200% both ears doubled" },
	{ .key = "width", .name = "Width", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "How far In's ears are pushed apart — 0 is mono, 100% as they came, 200% twice as wide" },
	{ .key = "swap", .name = "Swap", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = false, .tooltip = "Exchange Width's left and right ears" },
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = true, .tooltip = "Bypass switch — off: every output rests at 0" },
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "split", .name = "Split", .bipolar = true, .pairAlways = true },
	{ .key = "width", .name = "Width", .bipolar = true, .pairFrom = 1u << static_cast<unsigned>(Param::In) },
};
static_assert(std::size(kOutputs) == static_cast<size_t>(Output::kCount));

/// RETIRED 2026-10-05 with Identity and Pan — never reuse these.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "identity", "2026-10-05", "the Identity output (−1, +1) is gone: Split with In and Amount at 100% gives (+1, −1)" },
	{ "pan", "2026-10-05",
	  "the Pan control and the Pan output (an equal-power gain pair, both ears ≥ 0) are gone; Split replaces them" },
};

inline constexpr const char* kTags[] = { "stereo", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "STEREO",
	.name = "Stereo Tool",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
};

extern const device::ModOps kModOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::stereo
