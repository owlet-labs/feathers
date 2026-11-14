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

#include "backplane/graph.h"
#include "composite/composite_types.h"
#include "device/latency.h"
#include "device/registry.h"
#include "rack/export_table.h"
#include "rack/inner_chain.h"
#include "rack/inner_chain_bundle.h"
#include "fx/owltt/crossover.h"
#include "dsp/analyser_bins.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// **THE RACK: PARALLEL CHAINS, AS A DEVICE** — backlog NO, stage 1 (2026-10-18).
//
// A chain holds devices; a rack holds CHAINS, and a rack is a device. Stage 1 is the trivial parallel:
// every chain is handed the same input and their outputs are summed, each through its own level, mute and solo,
// then the rack's own wet/dry Mix and output fader. The switch/blend/split modes, the canvas mixer and the
// active-chain row are later stages.
//
// **EACH CHAIN IS A `rack::InnerChain`** — the engine, bridge, drain and chunking every holder of an inner chain
// uses (SKLITTER's lanes, Resonate's), so nothing about running a chain is re-derived here. All chains share ONE
// `InstanceCounter` (gap G3), so a device id names one device across the rack and the four route verbs dispatch
// on the id alone.
//
// **NO CAP ON THE CHAIN COUNT, SO THE PER-CHAIN CONTROLS ARE NOT DESCRIPTOR PARAMS** (stop-and-ask, option A):
// a descriptor's controls are a constexpr list. Level, mute and solo live in the rack's BODY, keyed per chain,
// beside each chain's own rack body; the card and the window edit them through this class. They are not DAW
// parameters and not modulation destinations in stage 1. The rack's own Enabled, Mix, Gain and eight macros
// (fanned out to every chain's macros, as the Chain device's are to its one) ARE params, keyed and in the ledger.
// **THEY MUST BECOME MODULATION DESTINATIONS LATER** (main, 2026-10-18): the blend modes (roadmap §3) need a level and
// a position a modulator can move. So each is stored by KEY under the chain's STABLE id (`kLevelKey`, …) — a future
// destination can name (chain id, key) without re-reading a single saved patch.
//
// **A MUTED CHAIN KEEPS RUNNING** — silently, so an unmute resumes mid-tail rather than from cold. The cost is its
// CPU; `holdsTail` counts only the chains that are heard.
//
// **THREADS.** The list of chains is the MESSAGE thread's (`chains_`). The audio thread runs a LINEUP — an
// immutable list of the chains, published by the message thread and adopted at the rack's drain point (the
// host's begin when it has one, else the bus stage), exactly where an inner chain drains its own edits. A chain
// is built, adopted and prepared on the message thread before it is published, so the audio thread allocates
// nothing; a removed chain and a superseded lineup are freed by the message thread only once the audio thread
// has adopted a later lineup (`reap`). CLAUDE.md's ABA rule holds by construction: the audio thread never keeps
// a pointer across a drain.
//
// **LATENCY.** Each chain's output is delayed to the slowest chain's, and the dry by the same; the rack reports
// that maximum. A chain that holds nothing is the dry path, aligned like any other.
//
// **PER VOICE: THE HOST'S VOICES DO NOT PASS THROUGH A RACK** (option A, stop-and-ask #2). A Chain passes them
// through (rack roadmap §8) because one answer per voice fits back in the voice; a rack needs one per CHAIN, and the
// synth applies each voice's level after the per-voice stage, so the chains' per-voice sums cannot be formed outside
// it. A Poly device inside a rack runs once on the bus, as with Poly off. See `Rack::setVoices`.

namespace feathers::rack::parallel {

/// THE RACK'S OWN CONTROLS. The macros sit where the Chain's do (`composite::Param`) — the caption road names a
/// macro by that index (`RackBridge::pushMacroCaptions`) — so the stage-2 controls come after them.
enum class Param : size_t {
	Enabled = 0,
	Mix,
	Gain,
	Macro1,
	Macro2,
	Macro3,
	Macro4,
	Macro5,
	Macro6,
	Macro7,
	Macro8,
	/// **STAGE 2**: what each chain is handed and the weight it is
	/// summed at, and the position Select and Blend read.
	Mode,
	Position,
	/// What a chain the mode gives no role does (§2a): rings out on silence, or stops.
	Unused,
	/// **BANDS** (step 3, §4a): how many, the corners between them, their slope, and where the filtering sits.
	BandCount,
	Cut1,
	Cut2,
	Cut3,
	Cut4,
	Slope,
	Filtering,
	/// **KEY TRACK**: the played note places the corners; each Cut becomes a
	/// semitone OFFSET from it.
	KeyTrack,
	kCount,
};

/// **THE MODES, IN MENU ORDER** — a Choice stored by label, so Bands, Mid/Side and Left/Right append in later steps
/// (§9) without moving a saved patch. Only the modes that do something are offered (the honesty rule).
enum class Mode : uint8_t { Parallel = 0, Select, Blend, MidSide, LeftRight, Bands, kCount };
inline constexpr const char* kModeNames[] = { "Parallel", "Select", "Blend", "Mid/Side", "Left/Right", "Bands" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));
inline constexpr uint32_t kModeBit(Mode m) { return 1u << static_cast<uint32_t>(m); }
/// WHERE THE POSITION MEANS SOMETHING: Select and Blend. In Parallel it is hidden, not greyed.
inline constexpr uint32_t kPositionModes = kModeBit(Mode::Select) | kModeBit(Mode::Blend);
/// **THE MODES THAT GIVE THE FIRST CHAINS FIXED ROLES** (§2a): Mid then Side, Left then Right. The chain list never
/// changes with the mode; a missing role is an empty chain, and a chain past the roles is not routed.
inline constexpr uint32_t kRoleModes = kModeBit(Mode::MidSide) | kModeBit(Mode::LeftRight) | kModeBit(Mode::Bands);
inline constexpr uint32_t kBandsOnly = kModeBit(Mode::Bands);
/// THE BAND COUNTS, as the Choice offers them (decided 2026-10-27, Q4: 2, 3 and 5; no 4).
inline constexpr const char* kBandCountNames[] = { "2", "3", "5" };
inline constexpr int kBandCounts[] = { 2, 3, 5 };
inline constexpr int kMaxBands = 5;
/// The slopes (decided, Q5: the two Linkwitz-Riley pairs only) and where the filtering sits.
inline constexpr const char* kSlopeNames[] = { "LR4 24 dB", "LR2 12 dB" };
enum class Slope : uint8_t { Lr4 = 0, Lr2 };
inline constexpr const char* kFilteringNames[] = { "Before", "After", "Both" };
/// WHAT A SEGMENTED CONTROL'S FACE SAYS, where the stored label is too long for a segment (display only; the Choice
/// still stores `kSlopeNames` / `kUnusedNames`, and the tooltip says the rest).
inline constexpr const char* kSlopeFaceNames[] = { "24 dB", "12 dB" };
inline constexpr const char* kUnusedFaceNames[] = { "Ring", "Stop" };
/// The corners' captions on a face ("Cut 1"…), static so a widget may keep the pointer.
inline constexpr const char* kCutCaptions[] = { "Cut 1", "Cut 2", "Cut 3", "Cut 4" };
enum class Filtering : uint8_t { Before = 0, After, Both };
/// THE CORNERS' RANGE, log2(Hz) (`Unit::LogHertz`): 20 Hz .. 20 kHz.
inline constexpr double kCutMinLog2 = 4.321928094887363;  // log2(20)
inline constexpr double kCutMaxLog2 = 14.287712379549449; // log2(20000)
/// **ORDER IS ENFORCED IN THE ENGINE** (§4a): each corner at least a third of an octave above the one below it, so a
/// modulator pushing cut 2 below cut 1 cannot cross them. PROVISIONAL, to be confirmed by ear.
inline constexpr double kMinCornerOctaves = 1.0 / 3.0;
/// The most roles any mode gives (Bands' five, step 3) — the phantom roles a short rack stands in with.
inline constexpr size_t kMaxRoles = 5;
/// HOW MANY ROLES `mode` GIVES a rack of `chains`: every chain where every chain is used, else the fixed count.
[[nodiscard]] constexpr size_t roleCount(Mode mode, size_t chains, int bands = 2) noexcept {
	if (mode == Mode::Bands) {
		return static_cast<size_t>(bands);
	}
	return (kRoleModes & kModeBit(mode)) != 0 ? size_t { 2 } : chains;
}
inline constexpr const char* kUnusedNames[] = { "Ring out", "Stop" };

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kTags[] = { "utility", "routing" };

inline constexpr device::Traits kTraits {
	.tag = "RACK",
	.name = "Rack",
	.hasCard = true,
	.hasExpanded = true,
	/// NOT POLY — its devices are, each for itself (the pass-through model, rack roadmap §8).
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	/// **THE SITE'S FURNITURE IS THIS DEVICE'S OWN MIX AND GAIN** (D1, 2026-10-29, lifting D3 for the Rack and the
	/// Chain): the device mixes, so a POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::Gain),
	.mixInDevice = true,
};

inline constexpr device::ParamDesc kParamDescs[] = {
    {"rack_enabled", "Rack Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, kOffOnNames, true, false,
     "Whether the rack is in the signal path"},
    // FULLY WET BY DEFAULT: a rack added to a lane sounds like its chains, as a Chain sounds like its devices.
    {"rack_mix", "Rack Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Dry/wet across the whole rack — 100% is the chains' sum alone, 0% passes the input"},
    {"rack_gain", "Rack Gain", 0.0, 1.0, composite::kFaderUnity, false, device::Unit::Fader, {}, true, false,
     "Output fader for the rack — unity at twelve o'clock"},
    {"macro_1", "Macro 1", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    {"macro_2", "Macro 2", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    {"macro_3", "Macro 3", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    {"macro_4", "Macro 4", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    {"macro_5", "Macro 5", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    {"macro_6", "Macro 6", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    {"macro_7", "Macro 7", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    {"macro_8", "Macro 8", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "A control of the rack's own, the same macro in every chain inside it"},
    // STRUCTURAL: a change alters what every chain hears, so no DAW lane and no modulator may move it (§2), and it
    // fades the rack's sum to silence and back rather than stepping.
    {.key = "rack_mode", .name = "Rack Mode", .min = 0.0, .max = static_cast<double>(Mode::kCount) - 1.0, .def = 0.0,
     .stepped = true, .unit = device::Unit::Choice, .names = kModeNames, .modulatable = false,
     .tooltip = "Parallel sums every chain. Select hears the chain at the Position, Blend the two either side of it. "
                "Mid/Side hands chain 1 the mid and chain 2 the side, Left/Right chain 1 the left and chain 2 the right",
     .hostSlot = false},
    {.key = "rack_position", .name = "Rack Position", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Where across the chains Select and Blend listen: the first chain at 0%, the last at 100%",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kPositionModes},
    // STRUCTURAL TOO, but only what the rack does with a chain nobody hears: a SwitchChip, two states (§2a, Q1).
    {.key = "rack_unused", .name = "Rack Unused", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kUnusedNames, .modulatable = false,
     .tooltip = "A chain this mode gives no role: Ring out runs it on silence until its tail ends, Stop halts it at once",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kRoleModes, .hostSlot = false},
    // BANDS (§4a): the count, the slope and the filtering are structural (faded, not automatable); the corners are
    // automatable and modulatable, ordered in the engine. Only the first count−1 corners are drawn.
    {.key = "rack_bands", .name = "Rack Bands", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kBandCountNames, .modulatable = false,
     .tooltip = "How many bands the input is split into, one per chain from the first",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBandsOnly, .hostSlot = false},
    {.key = "rack_cut_1", .name = "Rack Cut 1", .min = kCutMinLog2, .max = kCutMaxLog2, .def = 6.906890595608519,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "The corner between bands 1 and 2", .gateControl = static_cast<int16_t>(Param::Mode),
     .gateMask = kBandsOnly},
    {.key = "rack_cut_2", .name = "Rack Cut 2", .min = kCutMinLog2, .max = kCutMaxLog2, .def = 8.965784284662087,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "The corner between bands 2 and 3, kept at least a third of an octave above cut 1",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBandsOnly},
    {.key = "rack_cut_3", .name = "Rack Cut 3", .min = kCutMinLog2, .max = kCutMaxLog2, .def = 10.965784284662087,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "The corner between bands 3 and 4, kept at least a third of an octave above cut 2",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBandsOnly},
    {.key = "rack_cut_4", .name = "Rack Cut 4", .min = kCutMinLog2, .max = kCutMaxLog2, .def = 12.550746785383243,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "The corner between bands 4 and 5, kept at least a third of an octave above cut 3",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBandsOnly},
    {.key = "rack_slope", .name = "Rack Slope", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kSlopeNames, .modulatable = false,
     .tooltip = "The crossover's steepness: Linkwitz-Riley 24 or 12 dB per octave. Either sums flat",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBandsOnly, .hostSlot = false},
    {.key = "rack_filtering", .name = "Rack Filtering", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kFilteringNames, .modulatable = false,
     .tooltip = "Before splits the input so each chain hears its band. After filters each chain's output to its band. "
                "Both splits each band's filtering in half, one half before its chain and the other after, so empty chains sum flat",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBandsOnly, .hostSlot = false},
    {.key = "rack_key_track", .name = "Rack Key Track", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = false,
     .tooltip = "The last note played places the corners, and each Cut becomes its offset from the note: Cut 1 at its "
                "default sits on the note, the others keep the default layout above it",
     .gateControl = static_cast<int16_t>(Param::Mode), .gateMask = kBandsOnly, .shortName = "Key"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));
static_assert(static_cast<size_t>(Param::Macro1) == static_cast<size_t>(composite::Param::Macro1));

//------------------------------------------------------------------------------------------------//
// A CHAIN'S LEVEL, MUTE AND SOLO AS DESTINATIONS (backlog NO;)          //
//------------------------------------------------------------------------------------------------//
//
// REAL PARAMS, so the graph, mayRoute, the key codec, the glow and the save all work as they do for any control: a
// block of three per CHAIN SLOT after the rack's own controls. A chain is given a slot when it is made and keeps it for
// its life — reordering or removing OTHER chains never moves it — and the chain id → slot map is saved in the body, so
// a route (stored by the slot's key) follows its chain. A freed slot is not reused until the host has pruned every
// route into it (`ContainerOps::releasedControls`), so a new chain never inherits another's routes.
//
// THE BODY IS THE TRUTH and each slot param's base MIRRORS it (written through the gate's `setOwnField` on every hand
// edit, undo and load), so the ring and the glow draw over the real value; the audio reads the RESOLVED param — the
// base plus its routes. Level is continuous; mute and solo are switches that threshold at half, and every change goes
// through the chain's existing 5 ms fade. A routed solo is a solo like a hand's.
//
// **64 SLOTS** (decided 2026-10-31, to be confirmed; 32 until `device::kMaxDeviceFields` went 160 -> 256):
// 192 params beside the rack's own, leaving room under the cap for the rack
// to grow its own. A chain past the 64th has no slot: its controls are not destinations and their tooltip says why (the
// honesty rule). No CLAP param per chain: a host slot carries one to the DAW on demand (`exposeToHost`), and the body
// adopts what the DAW writes (`adoptOwnFields`).
inline constexpr size_t kChainSlots = 64;
inline constexpr uint8_t kNoChainSlot = 0xFF;
/// THE MOST CHAINS A BODY MAY DECLARE. Chains past `kChainSlots` are legal (they have no slot), so this is not that;
/// it is the bound a stored count is held to before any chain is built, since each is a whole inner engine.
inline constexpr uint32_t kMaxChains = 1024;
enum class SlotControl : size_t { Level = 0, Mute, Solo, kCount };
inline constexpr size_t kSlotControls = static_cast<size_t>(SlotControl::kCount);
inline constexpr size_t kSlotParamBase = static_cast<size_t>(Param::kCount);
inline constexpr size_t kAllParamCount = kSlotParamBase + kChainSlots * kSlotControls;
/// The control index of slot `slot`'s `c`.
[[nodiscard]] constexpr size_t slotParam(size_t slot, SlotControl c) noexcept {
	return kSlotParamBase + slot * kSlotControls + static_cast<size_t>(c);
}
/// What slot, and which of its three, control `control` is — false for one of the rack's own.
[[nodiscard]] constexpr bool slotOfParam(size_t control, size_t* slot, SlotControl* c) noexcept {
	if (control < kSlotParamBase || control >= kAllParamCount) {
		return false;
	}
	*slot = (control - kSlotParamBase) / kSlotControls;
	*c = static_cast<SlotControl>((control - kSlotParamBase) % kSlotControls);
	return true;
}
/// THE TOOLTIP A CHAIN PAST THE LAST SLOT SHOWS ON ITS LEVEL, MUTE AND SOLO.
inline constexpr const char* kNoSlotWhy =
    "A Rack modulates the level, mute and solo of its first 64 chains; this one is past them, so nothing can be routed here";

namespace detail {

/// THE SLOT PARAMS' KEYS AND NAMES, built once as text a `ParamDesc` can point into.
struct SlotText {
	std::array<std::array<char, 28>, kChainSlots * kSlotControls> keys {};
	std::array<std::array<char, 28>, kChainSlots * kSlotControls> names {};
};

constexpr void writeText(std::array<char, 28>& out, const char* a, size_t number, const char* b) {
	size_t n = 0;
	for (const char* p = a; *p != '\0'; ++p) {
		out[n++] = *p;
	}
	if (number >= 10) {
		out[n++] = static_cast<char>('0' + number / 10);
	}
	out[n++] = static_cast<char>('0' + number % 10);
	for (const char* p = b; *p != '\0'; ++p) {
		out[n++] = *p;
	}
	out[n] = '\0';
}

constexpr SlotText makeSlotText() {
	SlotText t;
	const char* keyTail[] = { "_level", "_mute", "_solo" };
	const char* nameTail[] = { " Level", " Mute", " Solo" };
	for (size_t s = 0; s < kChainSlots; ++s) {
		for (size_t c = 0; c < kSlotControls; ++c) {
			writeText(t.keys[s * kSlotControls + c], "rack_chain_slot_", s + 1, keyTail[c]);
			writeText(t.names[s * kSlotControls + c], "Chain Slot ", s + 1, nameTail[c]);
		}
	}
	return t;
}
inline constexpr SlotText kSlotText = makeSlotText();

constexpr std::array<device::ParamDesc, kAllParamCount> makeAllParams() {
	std::array<device::ParamDesc, kAllParamCount> all {};
	for (size_t i = 0; i < kSlotParamBase; ++i) {
		all[i] = kParamDescs[i];
	}
	for (size_t s = 0; s < kChainSlots; ++s) {
		for (size_t c = 0; c < kSlotControls; ++c) {
			const size_t i = s * kSlotControls + c;
			const bool level = c == 0;
			all[kSlotParamBase + i] = device::ParamDesc {
				.key = kSlotText.keys[i].data(),
				.name = kSlotText.names[i].data(),
				.min = 0.0,
				.max = 1.0,
				.def = level ? composite::kFaderUnity : 0.0,
				.stepped = !level,
				.unit = level ? device::Unit::Fader : device::Unit::Choice,
				.names = level ? std::span<const char* const> {} : std::span<const char* const>(kOffOnNames),
				.modulatable = true,
				.tooltip = level ? "A chain's level into the rack's sum — routed, it moves around the chain's own fader"
				           : c == 1 ? "A chain's mute — routed, the chain is muted while the route holds it above half"
				                    : "A chain's solo — routed, the chain counts as soloed while the route holds it above half",
				.hostSlot = true, // a host slot carries it to the DAW (backlog NO item 2 follow-up); the body adopts the DAW's writes
			};
		}
	}
	return all;
}

} // namespace detail

/// THE RACK'S WHOLE CONTROL LIST: its own controls, then the 32 chain slots' three each.
inline constexpr std::array<device::ParamDesc, kAllParamCount> kAllParamDescs = detail::makeAllParams();
static_assert(kAllParamCount <= device::kMaxDeviceFields);
static_assert(static_cast<size_t>(Param::Macro8) + 1 == static_cast<size_t>(Param::Mode));

/// **THE CORNERS THE ENGINE RUNS AT**, in Hz, from the four cuts (log2 Hz): each at least `kMinCornerOctaves` above
/// the one below (§4a). One function for the engine and the strip that draws them, so the lines on the plot are the
/// corners the filters use, not the cuts a modulator pushed past each other.
inline void orderCorners(const double targetLog2[4], double outHz[4]) noexcept {
	const double ratio = std::exp2(kMinCornerOctaves);
	for (size_t k = 0; k < 4; ++k) {
		const double wanted = std::exp2(targetLog2[k]);
		outHz[k] = k == 0 ? wanted : (wanted > outHz[k - 1] * ratio ? wanted : outHz[k - 1] * ratio);
	}
}

//--------------------------------------------------------------------------------------------------//
// KEY TRACK — the Phaser's contract (`fx::phaser`, `phaser.h`'s class comment): the note    //
// LATCHED while a key is held, held after the release, forgotten at a reset; with no note yet the knobs rule.   //
//--------------------------------------------------------------------------------------------------//

/// **THE NOTE'S OWN FREQUENCY**, A4 = 69, the pitch clamped to [−60, 200] semitones so it is always finite and
/// positive — `fx::phaser::keyHzOf`, restated (the rack does not include a device's types).
[[nodiscard]] inline double keyHzOf(double pitchSemitones) noexcept {
	const double p = std::isfinite(pitchSemitones) ? (pitchSemitones < -60.0 ? -60.0 : (pitchSemitones > 200.0 ? 200.0 : pitchSemitones))
	                                               : 69.0;
	return 440.0 * std::exp2((p - 69.0) / 12.0);
}

/// **THE CUTS' ONE REFERENCE**: cut 1's default AS A ROW STORES IT (the Phaser's rule: so "+0" is exactly the note).
/// WHERE THE PHASER'S CONTRACT DID NOT FIT: it has one knob and measures it from ITS OWN default; four corners each
/// measured from their own default would all land on the note at their defaults. Measured from ONE reference, the
/// 5-band layout transposes with the note — cut 1 on it, the others the default layout's octaves above.
inline constexpr double kCutKeyRefLog2 = static_cast<double>(static_cast<float>(6.906890595608519)); // log2(120)

/// A CUT AS AN OFFSET FROM THE NOTE, semitones.
[[nodiscard]] inline double cutKeyOffsetSemitones(double cutLog2) noexcept { return 12.0 * (cutLog2 - kCutKeyRefLog2); }

/// **EACH CORNER'S TARGET, log2(Hz), BEFORE THE ORDER**: the cut clamped to 20 Hz..20 kHz, or — tracking (Key Track on
/// and a note heard) — the note moved by the cut's offset, clamped to the same audible range.
inline void cornerTargetsLog2(const float cutLog2[4], bool tracking, double noteHz, double out[4]) noexcept {
	for (size_t k = 0; k < 4; ++k) {
		const double c = static_cast<double>(cutLog2[k]);
		double t = c;
		if (tracking) {
			t = std::log2(noteHz) + cutKeyOffsetSemitones(c) / 12.0;
		}
		out[k] = t < kCutMinLog2 ? kCutMinLog2 : (t > kCutMaxLog2 ? kCutMaxLog2 : t);
	}
}

/// THE CORNERS, tracked or not, then ordered — what the engine settles on and the strip draws.
inline void trackedCorners(const float cutLog2[4], bool tracking, double noteHz, double outHz[4]) noexcept {
	double targets[4];
	cornerTargetsLog2(cutLog2, tracking, noteHz, targets);
	orderCorners(targets, outHz);
}
inline void orderedCorners(const float cutLog2[4], double outHz[4]) noexcept { trackedCorners(cutLog2, false, 0.0, outHz); }

/// **WHERE THE SPECTRUM STRIP IS DRAWN** (§7a): Bands, Mid/Side and Left/Right. Select, Blend and Parallel have
/// nothing on a frequency axis to edit, and a plot that is only decoration is not what the strip is for.
inline constexpr uint32_t kSpectrumModes = kModeBit(Mode::Bands) | kModeBit(Mode::MidSide) | kModeBit(Mode::LeftRight);

/// THE READINGS (`AudioOps::setReadingsSink`): the analyser's last frame — PRE is the rack's input, POST its output —
/// counter first and last, so a reader can tell a whole frame from one half-written.
enum class Reading : size_t {
	FrameA = 0,
	PreFirst = 1,
	PostFirst = 1 + dsp::kAnalyserBins,
	FrameB = 1 + 2 * dsp::kAnalyserBins,
	/// The note key tracking places the corners on, Hz — 0 while it is not tracking (the Phaser's `NoteHz`).
	NoteHz,
	kCount,
};
inline constexpr size_t kReadingCount = static_cast<size_t>(Reading::kCount);

/// **IS CONTROL `p` LIVE IN MODE `mode`?** Its declared gate on the Mode — what the card and the window HIDE by (the
/// engine-settings rule: a control only a mode could enable is not drawn, not greyed).
[[nodiscard]] inline int bandCountOf(double choice) noexcept {
	const long i = std::lround(choice);
	return kBandCounts[i < 0 ? 0 : (i > 2 ? 2 : i)];
}
/// AND A CORNER ONLY BELOW THE BAND COUNT: cut k is drawn while there are more than k bands (Cut 3 and 4 only at 5).
[[nodiscard]] inline bool controlShown(Param p, double mode, double bandChoice = 2.0) noexcept {
	if (!device::relevantFor(kParamDescs[static_cast<size_t>(p)], mode)) {
		return false;
	}
	if (p >= Param::Cut1 && p <= Param::Cut4) {
		return static_cast<int>(p) - static_cast<int>(Param::Cut1) + 1 < bandCountOf(bandChoice);
	}
	return true;
}

/// **THE RACK'S OWN WORDS FOR ITS POSITION** (§3, "the chain in the DAW"): the chain the value lands on, by its 1-based
/// number, in Select and Blend; every other control, and the position in Parallel, prints by its declared unit.
extern const device::TextOps kTextOps;

/// **THE CHAIN SELECT HEARS** at `position` across `chains`: the nearest, chain k (0-based) sitting at k/(N−1); one
/// chain sits everywhere. The DSP and the text both ask this, so the DAW cannot name a chain the rack is not playing.
[[nodiscard]] inline size_t selectedChain(double position, size_t chains) noexcept {
	if (chains <= 1) {
		return 0;
	}
	const double p = position < 0.0 ? 0.0 : (position > 1.0 ? 1.0 : position);
	const auto k = static_cast<size_t>(std::lround(p * static_cast<double>(chains - 1)));
	return k < chains ? k : chains - 1;
}

/// **SELECT'S AND BLEND'S WEIGHT ON CHAIN k** of `chains` at `position` (§3): Select the nearest at 1, Blend the
/// linear triangle of the two either side (summing to one); 1 in any other mode. The engine and the position bar both
/// read it, so the bar's fill is the weight the sum uses.
[[nodiscard]] inline float positionWeight(Mode mode, double position, size_t k, size_t chains) noexcept {
	if (mode == Mode::Select) {
		return k == selectedChain(position, chains) ? 1.0f : 0.0f;
	}
	if (mode != Mode::Blend || chains <= 1) {
		return 1.0f;
	}
	const double p = position < 0.0 ? 0.0 : (position > 1.0 ? 1.0 : position);
	const double x = p * static_cast<double>(chains - 1);
	const size_t lower = static_cast<size_t>(x) < chains - 1 ? static_cast<size_t>(x) : chains - 1;
	const auto frac = static_cast<float>(x - static_cast<double>(lower));
	if (k == lower) {
		return 1.0f - frac;
	}
	return k == lower + 1 ? frac : 0.0f;
}

/// THE TEXT ITSELF, for a rack of `chains` chains in `mode` — `Chain 2`, or `Chain 2 → 3, 40%` in Blend. False where
/// the unit should print (Parallel, or no chains). Pure; what `kTextOps` and the cases share.
bool formatPosition(Mode mode, size_t chains, double position, char* out, size_t size) noexcept;
/// Its inverse: `Chain 2` (or a bare `2`) is that chain's centre; `Chain 2 → 3, 40%` is 40% of the way from 2 to 3.
/// False for anything else, a chain the rack does not hold, or Parallel — the unit then reads it.
bool parsePosition(Mode mode, size_t chains, const char* text, double* position) noexcept;

/// A CUT WHILE A NOTE IS TRACKED: its offset from the note, "+7.0 st" (`phaserOffsetText`'s form), and its inverse
/// ("7", "+7 st", "-3.5 st" to the cut's stored log2).
bool formatCutOffset(double cutLog2, char* out, size_t size) noexcept;
bool parseCutOffset(const char* text, double* cutLog2) noexcept;
/// THE KEY TRACK GLIDE AND ITS LANDING — the Phaser's (`kKeySmoothSeconds`, `kGlideSnap`).
inline constexpr double kKeySmoothSeconds = 0.005;
inline constexpr double kGlideSnap = 1.0e-7;

extern const device::AudioOps kAudioOps;
extern const device::ContainerOps kContainerOps;
extern const device::NoteOps kNoteOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kAllParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.mod = nullptr,
	.notes = &kNoteOps, // backlog RN: notes through the chains — `Rack::processNotes`
	.container = &kContainerOps,
	.text = &kTextOps,
	.category = device::Category::Containers,
};

/// THE BODY'S FRAME: 'R','K','P','1' little-endian, then a version.
inline constexpr uint32_t kBodyMagic = 0x31504B52u;
/// 2 since backlog NZ (b): the export table follows the chains (count u16, then row u16 · id u32 · index u16 each).
/// 3 since NZ any depth (2026-10-31): the index is u32, whole, so a row may name an inner holder's export. 2 still reads.
inline constexpr uint16_t kBodyVersion = 3;
/// THE LARGEST BODY A READER WILL WALK — a bound on BYTES, not on chains: four times one chain's ceiling, which
/// is already 32 MiB and far past anything a hand builds. A real chain's body is a few hundred bytes.
inline constexpr size_t kMaxRackBodyBytes = 4 * kMaxBodyBytes;

/// THE KEYS A CHAIN'S OWN CONTROLS ARE STORED UNDER in the body (the ledger's rule, applied inside the body):
/// a key names a meaning, an unknown key is skipped, a missing one is the default.
inline constexpr const char* kLevelKey = "level";
inline constexpr const char* kMuteKey = "mute";
inline constexpr const char* kSoloKey = "solo";
/// A CHAIN'S WINDOW STATE: open (1) or closed (0, and what a body without the key reads as).
inline constexpr const char* kOpenKey = "open";
/// A CHAIN'S SLOT (backlog NO): which block of slot params is its, −1 for none.
inline constexpr const char* kSlotKey = "slot";
/// **WHAT A POLY SWITCH SAYS INSIDE A RACK** (backlog NO, option A): no host voice reaches a rack's chains, so the
/// switch changes nothing and is greyed with this line (`device::Modulatable::unavailable`).
inline constexpr const char* kPolyInsideRack =
    "Poly is unavailable here: inside a Rack this device runs once, on the bus — no voice reaches a Rack's chains";

/// A FADER POSITION TO A LINEAR GAIN — `fx::busutil`'s curve, the Chain's Gain: unity at 12 o'clock.
constexpr float faderGain(float position) {
	const float p = position < 0.0f ? 0.0f : (position > 1.0f ? 1.0f : position);
	return (2.0f * p) * (2.0f * p);
}
static_assert(faderGain(static_cast<float>(composite::kFaderUnity)) == 1.0f);

/// THE RACK, WHICH IS THE DEVICE'S STORAGE. The host files a pointer to it as the container's surface
/// (`DeviceHost::containerSurfaceOf`); the card and the window cast it with `rackOf`.
class Rack final {
public:
	Rack();
	~Rack();
	Rack(const Rack&) = delete;
	Rack& operator=(const Rack&) = delete;

	//--------------------------------------------------------------------------------------------//
	// MESSAGE THREAD — the chains, as a hand edits them                                            //
	//--------------------------------------------------------------------------------------------//

	/// How many chains the rack holds.
	[[nodiscard]] size_t chainCount() const noexcept { return chains_.size(); }
	/// The chain at `index` (display order), or null.
	[[nodiscard]] InnerChain* chainAt(size_t index) noexcept;
	[[nodiscard]] const InnerChain* chainAt(size_t index) const noexcept;
	/// Where the chain with stable id `id` stands now, or `SIZE_MAX`.
	[[nodiscard]] size_t indexOfChain(uint16_t id) const noexcept;
	/// A stable id for the chain at `index` (what the body stores it under), or 0xFFFF.
	[[nodiscard]] uint16_t chainIdAt(size_t index) const noexcept;

	/// ADD AN EMPTY CHAIN at the end — built, adopted and prepared here, handed to the audio thread at its next
	/// drain. Returns its index, or `SIZE_MAX` before a catalogue has been adopted (nothing could go in it).
	size_t addChain();
	/// REMOVE the chain at `index`; it stops sounding at the audio thread's next drain and is freed after.
	bool removeChain(size_t index);
	/// **ONE CHAIN, WHOLE** — its id, body and controls: what an undo puts back.
	struct ChainSnapshot {
		uint16_t id = 0xFFFF;
		std::vector<uint8_t> body;
		float level = static_cast<float>(composite::kFaderUnity);
		bool mute = false;
		bool solo = false;
		bool open = false;
		/// The slot it held — taken again on a put-back when still free, else the lowest free one.
		uint8_t slot = kNoChainSlot;
	};
	[[nodiscard]] ChainSnapshot snapshotChain(size_t index) const;
	/// PUT A CHAIN BACK at `index`, under its own id — refused (`SIZE_MAX`) for an id the rack holds, a body the
	/// chain will not read, or before a catalogue. The undo of a removal, and the redo of an add.
	size_t insertChain(size_t index, const ChainSnapshot& chain);

	/// THE CHAIN'S OWN CONTROLS. Level is a fader position (0..1, unity at `composite::kFaderUnity`).
	[[nodiscard]] float level(size_t index) const noexcept;
	[[nodiscard]] bool muted(size_t index) const noexcept;
	[[nodiscard]] bool soloed(size_t index) const noexcept;
	void setLevel(size_t index, float position) noexcept;
	void setMuted(size_t index, bool muted) noexcept;
	void setSoloed(size_t index, bool soloed) noexcept;
	/// **THE CHAIN'S SLOT** (backlog NO) — which block of `kAllParamDescs` its level, mute and solo are, for its life; or
	/// `kNoChainSlot` past the last slot (`kChainSlots`).
	[[nodiscard]] uint8_t slotOf(size_t index) const noexcept;
	/// **SLOTS FREED, AWAITING THE HOST'S PRUNE** (`ContainerOps::releasedControls`): the control indices of every slot a
	/// removed chain gave up, which no new chain may take until `controlsReleased` says the host dropped their routes.
	[[nodiscard]] size_t releasedControls(size_t* out, size_t capacity) const noexcept;
	void controlsReleased() noexcept;
	/// **EVERY SLOT PARAM'S BASE PUT BACK TO ITS CHAIN'S VALUE** through the gate.
	void remirror() const;
	/// **THE MIRROR A LOAD OR A NEW HOLDER OWES, WRITTEN** (`ContainerOps::serviceMirror`) — on the host's service beat,
	/// never inside its provisioning. True when it wrote. Until then the body's values are heard, not the slot params.
	bool serviceMirror();
	/// **A BASE THE HOST MOVED, INTO THE BODY** (`ContainerOps::adoptOwnFields`): the holder's record row, read on its
	/// service beat. Skipped while a mirror is owed. True when a chain's level, mute or solo moved.
	bool adoptOwnFields(const float* fields, size_t present);
	/// Whether the slot params are what is heard (the holder mirrors and owes nothing) — for tests.
	[[nodiscard]] bool mirroredForTest() const noexcept { return mirrored_.load(std::memory_order_relaxed); }
	/// The slot params as the last `setFields` resolved them (slot × 3 + control), heard or not — for tests.
	[[nodiscard]] float slotFieldForTest(size_t i) const noexcept { return i < slotFields_.size() ? slotFields_[i] : 0.0f; }
	/// **THE WINDOW'S OPEN/CLOSED STATE FOR THIS CHAIN** (backlog OC) — view state saved with the patch under the chain's id.
	/// Closed by default. A change marks the holder dirty like a card's fold; it is not an undo step.
	[[nodiscard]] bool open(size_t index) const noexcept;
	void setOpen(size_t index, bool open) noexcept;

	/// **THE CHAIN'S COURTESY METER** (backlog OC): the held, falling linear peak of what the chain at `index` puts into
	/// the sum (after its level, mute and solo), published by the audio thread as one atomic. False — `peak` untouched —
	/// for no such chain or one that has not run yet. Any thread; no allocation.
	[[nodiscard]] bool chainPeak(size_t index, float& peak) const noexcept;

	/// **A VIEW SHOWING THE SPECTRUM ASKS FOR IT** (§7a's cost rule, the "readings wanted" bit EQ A17 never built): the
	/// analyser runs for the next half second of audio and stops when no view has asked since. A lease rather than a
	/// count, so a view torn down without saying so cannot leave it running. Any thread; no allocation.
	void wantReadings() noexcept { readingsLease_.store(leaseSamples_, std::memory_order_relaxed); }
	void setReadingsSink(device::ReadingsSink sink) noexcept { sink_ = sink; }

	/// A COUNT THAT MOVES WHENEVER THE CHAINS OR THEIR CONTROLS DO — what a card re-reads on.
	[[nodiscard]] uint64_t revision() const noexcept { return revision_; }

	/// FREE what the audio thread has moved past (superseded lineups, removed chains).
	void reap();

	//--------------------------------------------------------------------------------------------//
	// THE DEVICE'S OPS (the tables in parallel_rack.cpp forward here)                              //
	//--------------------------------------------------------------------------------------------//

	void adopt(const device::Catalogue& catalogue);
	void prepare(double sampleRate, int maxFrames);
	void reset();
	void setVoices(size_t voices);
	void setOuterGate(const device::OuterGate& gate);
	void setHostSlots(const device::HostSlotLink& link);
	device::RouteDrop service();
	bool saveState(const device::ByteSink& sink) const;
	bool loadState(const uint8_t* data, size_t bytes);
	/// `ContainerOps::applyState` — **A BODY INTO A RACK THAT MAY BE RUNNING** (backlog OU, a Rack preset). `loadState`'s
	/// all-or-nothing read, without its provisioning: the old chains retire and the new lineup is published, adopted by
	/// the audio thread at its drain, as `insertChain` hands one over. EMPTY BYTES are a fresh rack: one empty chain.
	bool applyState(const uint8_t* data, size_t bytes);
	/// The chain that holds inner device `id`, or null — the route verbs dispatch on it (gap G3).
	[[nodiscard]] InnerChain* holding(DeviceInstanceId id) const noexcept;
	/// **WHAT THE RACK EXPORTS TO ITS HOST** (backlog NZ (b)): a modulator in any of its chains, or one of its macros,
	/// read by the devices after the rack as `Source::exported(rack, row)` — `ContainerOps::exportRow` and its pair.
	size_t exportRow(backplane::Source inner);
	[[nodiscard]] const ExportTable& exports() const noexcept { return exports_; }
	/// MESSAGE THREAD: the table, for `ContainerOps::sweepExports` (release of rows no route reads).
	[[nodiscard]] ExportTable& exportsMutable() noexcept { return exports_; }

	void setFields(const float* fields, size_t present);
	void setNoteContext(const device::NoteContext& note);
	void setTransportClock(const device::TransportClock& clock);
	void setOuterSources(const device::OuterSources& outer);
	void setSidechain(const float* left, const float* right, bool sounding);
	bool beginPerVoiceBlock();
	void processVoice(size_t voice, float* left, float* right, int frames, const device::NoteContext& note);
	void processStereo(float* left, float* right, int frames);
	[[nodiscard]] uint32_t latencySamples(double sampleRate) const noexcept;
	[[nodiscard]] bool holdsTail() const noexcept;

	/// **NOTES THROUGH THE RACK** (backlog RN). The rack's incoming notes go to every chain's note pass, and what
	/// leaves is the DRY (the incoming notes, at 1 − Mix) and each chain's notes (at Mix × the chain's weight: 1 in
	/// Parallel, Mid/Side, Left/Right and Bands; Select's 0/1; Blend's triangle; 0 for a muted or un-soloed chain),
	/// merged in time order. A note-ON's velocity is scaled by what it is heard at and never rounds below 1/127 (zero reads
	/// as an off to some hosts); a source at 0 starts no NEW note. NO CHOKE: a note that sounded ends with its own off,
	/// whatever the weights do after. The same event from several sources (a chain that passes its notes through, beside
	/// the dry) is ONE note, at the sum of what they hear it at, capped at its own velocity — so a rack whose chains hold
	/// no note device is exactly transparent, as it always was. Two sources' different notes on one key are two notes, each
	/// ended by its own off. AUDIO THREAD; allocates nothing.
	size_t processNotes(const device::NoteEvent* in, size_t count, device::NoteEvent* out, size_t capacity) noexcept;
	/// What the chains' self-clocked devices (an Arp, a Pattern) emit now, through the same mix; `*until` the soonest any
	/// wants its next call. A chain whose last emitter leaves has the notes it invented ended.
	size_t emitNotes(const device::TransportClock& clock, uint64_t sampleNow, device::NoteEvent* out, size_t capacity,
	                 uint32_t* until) noexcept;
	/// TEST SEAM: how many notes the rack is tracking as sounding (or swallowed) now.
	[[nodiscard]] size_t noteRecordsForTest() const noexcept { return recordCount_; }

	//--------------------------------------------------------------------------------------------//
	// TEST ONLY                                                                                    //
	//--------------------------------------------------------------------------------------------//

	/// The rack's own control `p` as `setFields` decoded it (`test_device_ranges`' row-codec sweep).
	[[nodiscard]] float fieldForTest(Param p) const noexcept;
	/// How many chains the AUDIO thread runs now (the adopted lineup).
	[[nodiscard]] size_t liveChainsForTest() const noexcept;
	/// The linear gain the chain at live position `index` was last summed at.
	[[nodiscard]] float chainGainForTest(size_t index) const noexcept;
	/// The MODE'S weight on the chain at live position `index` (Select's 0/1 and its crossfade, Blend's triangle; 1 in
	/// Parallel), which multiplies `chainGainForTest`'s.
	[[nodiscard]] float chainWeightForTest(size_t index) const noexcept;
	/// The mode the audio thread runs now — the wanted one only once the fade has reached silence.
	[[nodiscard]] Mode modeForTest() const noexcept { return mode_; }
	/// The mode change's fade on the chains' sum: 1 settled, 0 at the switch.
	[[nodiscard]] float modeGainForTest() const noexcept { return modeGain_; }
	/// **HOW MANY CALLS THE CHAIN AT LIVE POSITION `index` HAS RUN** (§2a's run-count seam): an unrouted chain under Ring
	/// out runs on silence until its tail is exactly zero and then stops; under Stop it never runs again.
	[[nodiscard]] uint64_t runsForTest(size_t index) const noexcept;
	/// **THE CORNER THE ENGINE RUNS AT**, in Hz (k = 0..3), after the order is enforced — the value a modulated cut
	/// actually reached (CLAUDE.md: assert the value, not a render).
	[[nodiscard]] double cornerHzForTest(size_t k) const noexcept { return k < 4 ? cornerHz_[k] : 0.0; }
	/// KEY TRACK: whether a held note has arrived since the reset, and the note it latched (semitones, A4 = 69).
	[[nodiscard]] bool heardNoteForTest() const noexcept { return heardNote_; }
	[[nodiscard]] double noteSemitonesForTest() const noexcept { return noteSemitones_; }
	/// The band count, slope and filtering the audio thread runs now.
	[[nodiscard]] int bandsForTest() const noexcept { return bands_; }
	[[nodiscard]] Filtering filteringForTest() const noexcept { return filtering_; }
	/// How many corners' coefficients were recomputed since construction (once per sub-block, only those that moved).
	[[nodiscard]] uint64_t coefficientUpdatesForTest() const noexcept { return coefficientUpdates_; }
	/// How many FFTs the analyser has run — the cost the "readings wanted" bit exists to keep at zero.
	[[nodiscard]] int transformsForTest() const noexcept;
	/// The samples of analysis a view's last ask has left (0: nobody is looking).
	[[nodiscard]] int64_t leaseForTest() const noexcept { return readingsLease_.load(std::memory_order_relaxed); }
	/// The readings the rack publishes, as published.
	[[nodiscard]] const std::array<float, kReadingCount>& readingsForTest() const noexcept { return readings_; }
	/// The master wet and dry coefficients the last sample used.
	[[nodiscard]] float wetForTest() const noexcept { return wetNow_; }
	[[nodiscard]] float dryForTest() const noexcept { return dryNow_; }
	/// How many lineups and removed chains are waiting to be freed.
	[[nodiscard]] size_t pendingFreesForTest() const noexcept { return lineups_.size() + retired_.size(); }

private:
	struct Branch;
	struct Lineup;

	std::unique_ptr<Branch> makeBranch(uint16_t id);
	void prepareBranch(Branch& branch);
	/// `loadState` and `applyState`: the body read whole, then the chains replaced; `provision` adopts the lineup now.
	bool readBody(const uint8_t* data, size_t bytes, bool provision);
	void publish();
	void adoptPending() noexcept;
	void installLive(Lineup* lineup) noexcept;
	void aim() noexcept;
	/// **THE MODE'S WEIGHTS** for the lineup, from `mode_` and `position_` — snapped while unprimed or at a mode's
	/// switch (`snap`), else ramped: Select's chain change over `selectSamples_`, equal-gain, every chain on one count
	/// so the weights keep summing to one; Blend's over the rack's 5 ms.
	void aimWeights(const Lineup* live, bool snap) noexcept;
	/// **WHAT ROLE `role` IS HANDED** (§1's split): the input, or its mid (M, M), side (S, −S), left (L, 0) or right
	/// (0, R), into `workL_`/`workR_` for `n` samples from `inL_`/`inR_`.
	void roleInput(size_t role, int n) noexcept;
	/// **BANDS** (step 3): the corners from the cuts, ordered (§4a), and their coefficients recomputed only where a
	/// corner moved; the split of the input (Before, Both), a role's own band path on its output (After, Both), and
	/// the dry through Π AP (for every filtering, Both included since backlog OS) so a Mix does not cancel at the corners (§4d).
	void aimCorners(int frames) noexcept;
	void splitBands(int n) noexcept;
	void bandOfOutput(size_t role, int n) noexcept;
	void alignDryToBands(int n) noexcept;
	/// WHAT THE DRY PATH IS in a structure: the raw input (0), through Π AP (1), through Π AP² (2).
	[[nodiscard]] int dryKind(Mode mode, Filtering filtering) const noexcept {
		(void)filtering; // Both's pair sums to the same Π AP as Before and After (backlog OS), so the dry's path is one
		return mode != Mode::Bands ? 0 : 1;
	}
	/// WHETHER THE WANTED STRUCTURE IS THE RUNNING ONE — the mode, and in Bands its count, slope and filtering.
	[[nodiscard]] bool structureIsWanted() const noexcept;
	/// The wanted structure becomes the running one (at a fade's silence, or at once where nothing is heard).
	void takeWantedStructure() noexcept;
	/// THE SLOWEST CHAIN THE MODE ROUTES — what every routed chain, every phantom role and the dry wait for.
	[[nodiscard]] uint32_t routedLatency(const Lineup* live, double sampleRate) const noexcept;
	/// **THE HOST IS OWED NEW TEXT FOR THE POSITION** (§3): the same value names another chain once the count moved.
	/// Through the link's `retext`, which marks this rack's slots in the owner's pool (`SlotHost::rescanOwed`). Asked at
	/// every publish, the one road every add, removal, put-back and load takes.
	void retextIfCountMoved();
	/// Fold each live chain's gathered peak into its held, falling one and publish it (see `chainPeak`).
	void advanceMeters(const Lineup* live, int frames) noexcept;
	[[nodiscard]] device::HostSlotLink linkFor(const Branch& branch) const;
	/// The gate every chain is handed: the holder's, saying that no host voice reaches in here (`kPolyInsideRack`).
	[[nodiscard]] device::OuterGate chainGate() const;
	/// **A HAND'S EDIT OF THE CHAINS IS AN EDIT OF THE HOLDER'S PATCH** (the body is saved in it): raise its dirty flag.
	void markDirty() const;
	/// THE SLOT A NEW OR RETURNING CHAIN TAKES: `wanted` if no chain holds it and no prune awaits it, else the lowest such;
	/// `kNoChainSlot` when all 32 are taken.
	[[nodiscard]] uint8_t freeSlot(uint8_t wanted) const noexcept;
	/// The chain's three slot params' bases set to its values (through the gate), when it has a slot.
	void mirror(const Branch& branch) const;
	/// A load or a new holder: the whole mirror is owed to the next `serviceMirror`, and the body is heard until then.
	void markMirrorDue();
	bool mirrorDue_ = false;
	/// Slots a removed chain gave up, until the host has pruned their routes (`releasedControls`). Message thread.
	std::vector<uint8_t> releasing_;
	/// AUDIO THREAD: the slot params as `setFields` resolved them, and whether the holder mirrors (a gate with
	/// `setOwnField`) — where it does not (a bare rack in a test), the body's values are the ones heard.
	std::array<float, kChainSlots * kSlotControls> slotFields_ {};
	bool slotFieldsLive_ = false;
	std::atomic<bool> mirrored_ { false };

	/// ONE COUNTER FOR EVERY CHAIN (gap G3). The rack's storage is never moved (a device lives where it was
	/// constructed), so the chains may borrow a plain member.
	backplane::InstanceCounter ids_ {};
	/// The export table (NZ (b)), saved after the chains in the body (version 2) and published after every block.
	ExportTable exports_;
	void publishExports(const Lineup* live) noexcept;
	device::Catalogue catalogue_ {};
	bool adopted_ = false;
	double sampleRate_ = 0.0;
	int maxFrames_ = device::kDefaultMaxFrames;
	size_t voices_ = 0;
	device::OuterGate outerGate_ {};
	device::HostSlotLink slotLink_ {};
	uint16_t nextChainId_ = 0;
	uint64_t revision_ = 0;
	/// The chain count the host last had text for (`retextIfCountMoved`).
	size_t textedCount_ = 0;

	// MESSAGE THREAD: the chains in display order, the lineups published and not yet freed, the chains removed.
	std::vector<std::unique_ptr<Branch>> chains_;
	struct Retired {
		uint64_t serial = 0; ///< freed once the audio thread has adopted this lineup serial or later
		std::unique_ptr<Branch> branch;
	};
	std::vector<Retired> retired_;
	std::vector<std::unique_ptr<Lineup>> lineups_;
	uint64_t nextSerial_ = 1;

	// THE CROSSING.
	std::atomic<Lineup*> pending_ { nullptr };
	std::atomic<Lineup*> live_ { nullptr };
	std::atomic<uint64_t> adoptedSerial_ { 0 };

	// THE NOTE ROAD (backlog RN) — AUDIO THREAD, sized at `prepare`.
	/// ONE NOTE A SOURCE STARTED, until its off: the source (a chain's id, or `kDrySource`), and whether it was HEARD —
	/// a source at weight 0 starts a record that is not, so its off is swallowed with its on.
	struct NoteRecord {
		uint16_t source = 0;
		int16_t port = 0;
		int16_t channel = 0;
		int16_t key = 0;
		int32_t noteId = -1;
		bool counts = false;
		bool invented = false; ///< from a self-clocked device: ended for it if the last one leaves its chain
		bool release = false;  ///< end it now (its chain left, its emitter left, or the rack was reset)
	};
	struct StagedNote {
		device::NoteEvent e {};
		uint16_t source = 0;
		float factor = 0.0f;
		bool invented = false;
		bool done = false;
	};
	static constexpr uint16_t kDrySource = 0xFFFF;
	static constexpr size_t kMaxNoteRecords = 256;
	static constexpr size_t kMaxStagedNotes = 1024;
	std::vector<NoteRecord> noteRecords_;
	size_t recordCount_ = 0;
	std::vector<StagedNote> staged_;
	size_t stagedCount_ = 0;
	std::vector<device::NoteEvent> chainNotes_;
	std::vector<device::NoteEvent> owedOffs_; ///< offs a full output could not take, delivered first next call
	size_t owedCount_ = 0;
	bool releaseAll_ = false;
	bool chainMuted(const Branch* b) const noexcept;
	bool chainSoloed(const Branch* b) const noexcept;
	/// What a chain at live position `k` hears its notes at (Mix × its weight), and the dry.
	float noteFactor(const Lineup* live, size_t k, bool anySolo) const noexcept;
	void stageNote(const device::NoteEvent& e, uint16_t source, float factor, bool invented) noexcept;
	/// The staged notes, merged and in time order, into `out` — and every note owed an off.
	size_t deliverNotes(const Lineup* live, device::NoteEvent* out, size_t capacity) noexcept;

	// AUDIO THREAD.
	bool hosted_ = false;
	bool enabled_ = true;
	float mix_ = 1.0f;
	float gain_ = static_cast<float>(composite::kFaderUnity);
	float macros_[composite::kMacroCount] {};
	/// WHAT THE FIELDS ASK FOR, and what runs: the two part only through the fade (`modeGain_` down to 0, then up).
	Mode modeWanted_ = Mode::Parallel;
	Mode mode_ = Mode::Parallel;
	float position_ = 0.0f;
	/// `rack_unused`: an unrouted chain stops at once (true) or rings out on silence (false).
	bool unusedStop_ = false;
	/// **A ROLE NO CHAIN FILLS** (§2a: fewer chains than roles) is an EMPTY chain: its share of the split passes,
	/// aligned to the slowest chain like any empty chain, at unity, silenced only by another chain's solo.
	struct Phantom {
		device::CompensationDelay align;
		float gainNow = 1.0f, gainTarget = 1.0f, gainStep = 0.0f;
		int remaining = 0;
		bool primed = false;
		uint32_t quiet = UINT32_MAX;
	};
	Phantom phantoms_[kMaxRoles];

	// BANDS (step 3). Wanted from the fields; running once a fade has reached silence.
	int bandsWanted_ = 3;
	int bands_ = 3;
	Slope slopeWanted_ = Slope::Lr4;
	Slope slope_ = Slope::Lr4;
	Filtering filteringWanted_ = Filtering::Before;
	Filtering filtering_ = Filtering::Before;
	/// Whether this fade also fades the DRY: the dry's path changes (into or out of Bands, or to or from Both).
	bool dryFade_ = false;
	float cutLog2_[4] { 6.906890595608519f, 8.965784284662087f, 10.965784284662087f, 12.550746785383243f };
	/// KEY TRACK: the switch, the latched note, and the corners' glide toward the tracked targets (log2 Hz).
	bool keyTrack_ = false;
	bool heardNote_ = false;
	double noteSemitones_ = 60.0;
	double cornerLog2Now_[4] {};
	bool cornersPrimed_ = false;
	double cornerHz_[4] {};
	double coeffHz_[4] {};
	Slope coeffSlope_ = Slope::Lr4;
	bool coeffsValid_ = false;
	uint64_t coefficientUpdates_ = 0;
	fx::owltt::Lr4Split::Coeffs lr4_[4] {};
	fx::owltt::Lr2Split::Coeffs lr2_[4] {};
	/// ONE SLOPE'S FILTERS, per channel: the split (Before), each role's band path (After), the HALF split and HALF band
	/// paths (Both: one Butterworth section each side, so the pair multiplies to the LR band and empty chains sum flat,
	/// backlog OS), and the dry's allpass chain.
	template <class Split> struct BandsEngine {
		using Halves = fx::owltt::Halves<Split>;
		fx::owltt::MultiSplit<Split, kMaxBands> split[2];
		fx::owltt::BandPath<Split, kMaxBands> path[kMaxBands][2];
		/// BOTH'S PAIR: the input's half split carries no compensation; the band paths after carry it, once.
		fx::owltt::MultiSplit<typename Halves::Before, kMaxBands, false> halfSplit[2];
		fx::owltt::BandPath<typename Halves::After, kMaxBands> halfPath[kMaxBands][2];
		fx::owltt::AllpassChain<Split, kMaxBands> dry[2];
		void reset() {
			for (int c = 0; c < 2; ++c) {
				split[c].reset();
				halfSplit[c].reset();
				dry[c].reset();
				for (auto& p : path) {
					p[c].reset();
				}
				for (auto& p : halfPath) {
					p[c].reset();
				}
			}
		}
		void flush() {
			for (int c = 0; c < 2; ++c) {
				split[c].flush();
				halfSplit[c].flush();
				dry[c].flush();
				for (auto& p : path) {
					p[c].flush();
				}
				for (auto& p : halfPath) {
					p[c].flush();
				}
			}
		}
		[[nodiscard]] bool settled() const {
			for (int c = 0; c < 2; ++c) {
				if (!split[c].settled() || !halfSplit[c].settled() || !dry[c].settled()) {
					return false;
				}
				for (const auto& p : path) {
					if (!p[c].settled()) {
						return false;
					}
				}
				for (const auto& p : halfPath) {
					if (!p[c].settled()) {
						return false;
					}
				}
			}
			return true;
		}
	};
	BandsEngine<fx::owltt::Lr4Split> lr4Engine_;
	BandsEngine<fx::owltt::Lr2Split> lr2Engine_;
	std::vector<float> bandL_[kMaxBands], bandR_[kMaxBands];

	// THE SPECTRUM (§7a): the EQ's analyser, lifted (`dsp/analyser.h`), at the EQ's 32-sample grid — behind a pointer
	// so this header does not carry pffft to every includer. Built in `prepare`, never on the audio thread.
	struct Spectrum;
	std::unique_ptr<Spectrum> spectrum_;
	int gridPhase_ = 0;
	std::atomic<int64_t> readingsLease_ { 0 };
	int64_t leaseSamples_ = 24000;
	std::vector<float> tapL_, tapR_; ///< the PRE tap: the input as the rack received it
	device::ReadingsSink sink_ {};
	std::array<float, kReadingCount> readings_ {};
	void analyse(const float* left, const float* right, int n) noexcept;
	float modeGain_ = 1.0f;
	float modeStep_ = 0.0f;
	int modeRemaining_ = 0;
	/// 10 ms, counted in samples: Select's change of chain (§3, Q3).
	int selectSamples_ = 1;
	device::TransportClock clock_ {};
	device::NoteContext note_ {};
	device::OuterSources outer_ {};
	bool haveOuter_ = false;
	const float* sideL_ = nullptr;
	const float* sideR_ = nullptr;
	bool sideSounding_ = false;

	std::vector<float> inL_, inR_;     ///< the input, kept for every chain and for the dry
	std::vector<float> workL_, workR_; ///< one chain's pass
	std::vector<float> sumL_, sumR_;   ///< the chains' sum
	device::CompensationDelay dryDelay_;

	float wetNow_ = 1.0f, dryNow_ = 0.0f, wetTarget_ = 1.0f, dryTarget_ = 0.0f, wetStep_ = 0.0f, dryStep_ = 0.0f;
	int remaining_ = 0;
	int rampSamples_ = 1;
	bool primed_ = false;
};

/// THE RACK BEHIND A HOST'S HANDLE (`DeviceHost::containerSurfaceOf`), or null. A contemporaneous handle, never
/// an identity: re-ask every tick.
[[nodiscard]] inline Rack* rackOf(void* surface) noexcept { return static_cast<Rack*>(surface); }

/// **A RACK'S BODY HOLDING ONE CHAIN** (backlog OA, Group into Rack: D2) — `chain` is that chain's body as
/// `InnerChain::saveState` writes it; the chain at unity, neither muted nor soloed, and open.
/// `exports` is the rack's export table (NZ (b)), naming ids inside the chain.
[[nodiscard]] std::vector<uint8_t> bodyOfOneChain(std::span<const uint8_t> chain,
                                                 const std::vector<RackPatch::Export>& exports = {});

/// **A RACK'S BODY WITH EVERY CHAIN'S BYTES THROUGH `map`** (backlog OA) — ids, order and each chain's own controls
/// kept. False for bytes that are not a whole Rack body, or a chain `map` refuses. `droppedExports`, when given, writes the rack's
/// export table EMPTY and adds the rows it held — a preset's capture (backlog OU: no route to a device outside).
bool mapRackBody(std::span<const uint8_t> bytes, LaneMap map, void* ctx, std::vector<uint8_t>* out,
                 size_t* droppedExports = nullptr);

} // namespace feathers::rack::parallel
