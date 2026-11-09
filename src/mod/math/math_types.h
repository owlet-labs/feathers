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

#include "device/registry.h"
#include "device/transfer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// MATH — TWO INPUTS COMBINED INTO ONE MODULATION OUTPUT (macro-modulation phase C).
//
// **THE ARITHMETIC, IN ONE ORDER:** `r = A op B`, clamped to the output's range (−1..1 bipolar, 0..1
// unipolar), then the map bent over `|r|` with the sign kept, then scaled by Amount. Clamping BEFORE the
// map is what keeps the map's input inside 0..1, where both curves are defined and meet Lin at both ends;
// clamping AFTER the op rather than on the inputs is what lets `A + B` saturate at one instead of the two
// inputs being cut separately.

namespace feathers::mod::math {

/// THE OPERATOR — the order of the device's `Operator` menu (a patch stores its LABEL).
///
/// **THE COMPARISONS ARE A GATE** (backlog PY;): 1 where
/// the comparison holds, 0 where it does not — never −1, so they read the same whichever way Bipolar is set,
/// and the card says so by greying Bipolar while one is chosen (`isComparison`).
enum class Op : uint8_t { Multiply, Add, Subtract, Min, Max, Greater, Less, GreaterEq, LessEq, kCount };

[[nodiscard]] constexpr bool isComparison(Op op) noexcept {
	return op == Op::Greater || op == Op::Less || op == Op::GreaterEq || op == Op::LessEq;
}

/// THE MAP — the order of the device's `Map` menu (displayed "Transfer"; a patch stores its LABEL). `Phi` is
/// the route transfer's φ-triangle curve (`device::phiTriangleCurve`), its periods the `Periods` control.
enum class Map : uint8_t { Lin, Log, Exp, Phi, kCount };

/// PROVISIONAL, set by ear later (rule on shape, let the ear set the number). Exp = |r|^k,
/// Log = 1 − (1 − |r|)^k: mirrors, so the pair is symmetric about Lin.
inline constexpr float kMapExponent = 2.0f;

/// **THE TRANSFER CURVE ITSELF** — the combined result `r` (already clamped to −1..1) through `map`, its sign kept.
/// `evaluate` runs every sample through this, and the card's Transfer glyph plots exactly this (backlog PY), so the
/// picture cannot disagree with what the device outputs. A map past the end of its enum answers 0.
[[nodiscard]] inline float mapCurve(Map map, float r, float octaves = device::kPhiOctavesDefault,
                                    float exponent = kMapExponent) noexcept {
	const float magnitude = std::abs(r);
	float bent = 0.0f;
	switch (map) {
	case Map::Lin: bent = magnitude; break;
	case Map::Log:
		bent = 1.0f - std::pow(1.0f - magnitude, std::clamp(exponent, device::kExponentMin, device::kExponentMax));
		break;
	case Map::Exp: bent = std::pow(magnitude, std::clamp(exponent, device::kExponentMin, device::kExponentMax)); break;
	case Map::Phi:
		bent = device::phiTriangleCurve(magnitude,
		                                std::exp2(std::clamp(octaves, 0.0f, device::kPhiOctavesMax)));
		break;
	default: return 0.0f;
	}
	return r < 0.0f ? -bent : bent;
}

/// **ONE OUTPUT SAMPLE FROM TWO INPUTS.** See the note above for the order. An operator or a map past
/// the end of its enum answers 0 — a stored value no menu can produce is silence, not a guess.
/// `octaves` is the Periods control — log2 of the φ map's periods per full swing; only `Map::Phi` reads it.
/// `exponent` is the Exponent control — Log's and Exp's k; only they read it.
[[nodiscard]] inline float evaluate(float a, float b, Op op, Map map, float amount, bool bipolar,
                                    float octaves = device::kPhiOctavesDefault,
                                    float exponent = kMapExponent) noexcept {
	float r = 0.0f;
	switch (op) {
	case Op::Multiply: r = a * b; break;
	case Op::Add: r = a + b; break;
	case Op::Subtract: r = a - b; break;
	case Op::Min: r = std::min(a, b); break;
	case Op::Max: r = std::max(a, b); break;
	case Op::Greater: r = a > b ? 1.0f : 0.0f; break;
	case Op::Less: r = a < b ? 1.0f : 0.0f; break;
	case Op::GreaterEq: r = a >= b ? 1.0f : 0.0f; break;
	case Op::LessEq: r = a <= b ? 1.0f : 0.0f; break;
	default: return 0.0f;
	}
	r = std::clamp(r, bipolar ? -1.0f : 0.0f, 1.0f);
	if (static_cast<size_t>(map) >= static_cast<size_t>(Map::kCount)) {
		return 0.0f;
	}
	return mapCurve(map, r, octaves, exponent) * amount;
}

// ---------------------------------------------------------------------------------------------------
// THE DEVICE.
//
// **A AND B ARE KNOBS THAT REST AT 0 AND TAKE ROUTES** — a route adds to the rest value and the engine's
// resolve clamps it to −1..1 (`device::withRouteOffset`), so "M1 into A at full" makes A the macro. What
// reaches `setFields` is that RESOLVED row; the device does no resolving of its own, as MACROS does not.

/// ORDER IS THE ROW ORDER AND THE DESCRIPTOR ORDER.
enum class Param : size_t {
	A = 0,
	B,
	/// `Op`'s order.
	Operator,
	/// `Map`'s order.
	Map,
	/// **INSIDE `evaluate`, NOT THE HOST'S.** See `kTraits`.
	Amount,
	/// **Off: the output is clamped to 0..1. On: −1..1.** Also what the mod ring reads for the reach of
	/// every route out of this device (`kOutputs`).
	Bipolar,
	/// **THE φ MAP'S PERIODS PER FULL SWING, AS log2** (0..7: 1 to 128) — read only under `Map::Phi`, and gated to
	/// it, so the card draws it only there.
	Periods,
	/// **LOG'S AND EXP'S EXPONENT** (1..8, default 2) — read only under those two maps, and gated to them.
	Exponent,
	/// **THE BYPASS SWITCH** — see `dahdsr::Param::Enabled`. Read by the host, never by this device: a
	/// disabled modulator publishes its rest value (`RackEngine::advanceModulators`).
	Enabled,

	kCount,
};

/// U+00D7 and U+2212, not `x` and `-`: the UI face (Lato) carries both glyphs. **THE BARE SIGN, NOT
/// "A × B"** — the card seats the operator BETWEEN the A and B knobs, so the operands
/// are already on either side of it, and a one-to-three-character entry is what lets the box be narrow
/// enough to sit there. `min`/`max` lower-case, as functions are written.
/// The comparisons in ASCII, `>=` not `≥`: the face is known to carry `×` and `−`, and a two-character entry
/// fits the operator box as `max` does.
inline constexpr const char* const kOpNames[] = { "×", "+", "−", "min", "max", ">", "<", ">=", "<=" };
inline constexpr const char* const kMapNames[] = { "Lin", "Log", "Exp", "Phi" };
inline constexpr const char* const kOffOnNames[] = { "Off", "On" };
static_assert(std::size(kOpNames) == static_cast<size_t>(Op::kCount));

/// **THE OPERATOR'S LABELS BEFORE THE COMPACT CARD**. A patch stores a Choice by its label, so a MATH saved before the rename says `A × B`; each
/// line here tells the load what that label means now.
inline constexpr device::ChoiceAlias kOpAliases[] = {
	{ "A × B", "×" }, { "A + B", "+" }, { "A − B", "−" }, { "Min", "min" }, { "Max", "max" },
};
static_assert(std::size(kMapNames) == static_cast<size_t>(Map::kCount));

/// **A AND B ARE `centred`** — drawing only: their arcs run from the notch at 0, so a sign reads as a
/// direction. **A, B AND AMOUNT ARE THE MODULATABLE CONTROLS.** An operator, a map or a polarity that moved under a
/// route would change what every downstream route MEANS mid-note — the same reason MACROS' polarities
/// are fixed.
inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "a", .name = "A", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "The first input — rests at 0; route a source into it",
	  .centred = true, .stereo = true },
	{ .key = "b", .name = "B", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "The second input — rests at 0; route a source into it",
	  .centred = true, .stereo = true },
	{ .key = "operator", .name = "Operator", .min = 0.0, .max = static_cast<double>(Op::kCount) - 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOpNames, .modulatable = false, .tooltip = "How A and B combine", .choiceAliases = kOpAliases },
	/// **DISPLAYED AS "Transfer"** — the curve the result is passed through. The
	/// enum, the `Param` and the stored KEY keep the name `map`: only what a hand reads moved.
	{ .key = "map", .name = "Transfer", .min = 0.0, .max = static_cast<double>(Map::kCount) - 1.0, .def = 0.0,
	  .stepped = true, .unit = device::Unit::Choice, .names = kMapNames, .modulatable = false,
	  .tooltip = "The curve over the result — Log rises fast then settles, Exp starts slow, Phi is a continuous "
	             "random-looking lookup; the sign is kept" },
	{ .key = "amount", .name = "Amount", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
	  .names = {}, .modulatable = true, .tooltip = "Scales the output", .stereo = true },
	{ .key = "bipolar", .name = "Bipolar", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = false,
	  .tooltip = "On: the output swings -1 to +1. Off: negative results rest at 0" },
	/// NOT MODULATABLE, as the map it shapes is not: a route moving it would re-draw the curve under every
	/// downstream route mid-note.
	{ .key = "phi_periods", .name = "Periods", .min = 0.0, .max = static_cast<double>(device::kPhiOctavesMax),
	  .def = static_cast<double>(device::kPhiOctavesDefault), .stepped = false, .unit = device::Unit::LogMultiple,
	  .names = {}, .modulatable = false,
	  .tooltip = "How many times the Phi curve rises and falls over a full swing — a few is a shape, many is a "
	             "random-looking lookup",
	  .gateControl = static_cast<int16_t>(Param::Map), .gateMask = 1u << static_cast<unsigned>(Map::Phi) },
	/// THE SAME RANGE AS A ROUTE'S Log/Exp exponent (`device::kExponentMin..Max`), so one number bends both alike.
	{ .key = "exponent", .name = "Exponent", .min = static_cast<double>(device::kExponentMin),
	  .max = static_cast<double>(device::kExponentMax), .def = static_cast<double>(kMapExponent), .stepped = false,
	  .unit = device::Unit::Number, .names = {}, .modulatable = false,
	  .tooltip = "How hard Log and Exp bend — 1 is a straight line, higher bends further",
	  .gateControl = static_cast<int16_t>(Param::Map),
	  .gateMask = (1u << static_cast<unsigned>(Map::Log)) | (1u << static_cast<unsigned>(Map::Exp)) },
	/// ON BY DEFAULT — a modulator added to a chain should modulate.
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = kOffOnNames, .modulatable = true, .tooltip = "Bypass switch — off: the output rests at 0" },
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// **ONE OUTPUT, NAMING ITS POLARITY SWITCH** — so the mod ring draws each route's reach from the switch
/// that actually moves it, the way MACROS' and the LFO's do.
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "math", .name = "Math", .bipolar = true, .bipolarControl = static_cast<int>(Param::Bipolar),
	  // A PAIR WHENEVER A, B OR AMOUNT HEARS ONE (backlog KU): the evaluation runs per ear.
	  .pairFrom = (1u << static_cast<unsigned>(Param::A)) | (1u << static_cast<unsigned>(Param::B))
	              | (1u << static_cast<unsigned>(Param::Amount)) },
};

inline constexpr const char* kTags[] = { "math", "modulator" };

inline constexpr device::Traits kTraits {
	.tag = "MATH",
	.name = "Math",
	.hasCard = true,
	.hasExpanded = false,
	.enableControl = static_cast<int>(Param::Enabled),
	/// **NO `amountControl`, AND AMOUNT IS STILL A CONTROL.** `evaluate` applies it — after the map, which
	/// is where the specified order puts it. Naming it here as well would have the HOST scale the output by it a
	/// second time at the publish (backlog DH), so a half Amount would publish a quarter.
	.tags = kTags,
};

extern const device::ModOps kModOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.category = device::Category::Modulators,
};

} // namespace feathers::mod::math
