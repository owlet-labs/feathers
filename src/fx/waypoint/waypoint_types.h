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
#include "fx/filter/filter_types.h"

#include <array>
#include <cstddef>

// THE WAYPOINT FILTER — four filter states in a plane, and one Morph that walks between them.
//
// Everything here that looks like a choice was one; the decisions that shape it are below.
//
// WHAT IT IS NOT: it is not Serum's PZ SVF (a type in the same menu, drawing one filter's response)
// and not its Path LFO (a modulator). Nobody ships "waypoints in a plane, each holding a filter
// state"; the near relatives are u-he FilterScape and Rossum Morpheus.
//
// THE ONE IDEA IT RESTS ON: INTERPOLATE PARAMETERS, NOT OUTPUTS AND NOT COEFFICIENTS.
// Crossfading four running filters is stable and wrong — two lowpasses an octave apart at 50/50 is
// a comb, not a lowpass between them. Interpolating the parameters in the device's own units gives
// a morph that sweeps musically AND keeps the closed-form curve, because `settingsAtMorph` returns
// a `filter::Settings` that draws through `filter::gainAt()` with no new maths.
//
// THE DECISIONS, 2026-08-26, on all four of the roadmap's open questions:
//   - Morph means the individual waypoints interpolate between curves — interpolate, do not blend.
//   - No per-waypoint modulation — `Morph` is the only modulatable control here.
//   - A polyline for now — Morph walks BY INDEX, not by arc length along the drawn line.
//   - The type is constant across all morph points — ONE type for the device.
//
// AND THE LAST OF THOSE IS WHY THIS DEVICE HAS NO DSP RISK. The roadmap worried that the mechanism
// making morph free in the multimode filter — shared g/k, differing output mixes — is the one a
// waypoint filter cannot use, since per-waypoint cutoff is its whole point. True, and it does not
// matter: THIS DEVICE NEVER BLENDS ANYTHING. It interpolates parameters and runs ONE filter at the
// result. A shared type means an identical mix vector with only g and k moving, which is a single
// filter sweeping its corner. `Bell` is therefore an ordinary choice here, not a barred one — the
// carve-out at multimode_filter.cpp:118 is about blending, and there is no blend.

namespace feathers::fx::waypoint {

/// HOW MANY WAYPOINTS, FIXED. Not a variable list: a fixed payload layout, no add/delete gestures,
/// and no "what happens when you delete the state the morph is sitting on".
inline constexpr size_t kWaypoints = 4;

/// THE PLUGIN'S ID BLOCK IS ASSIGNED IN THIS ORDER, so APPENDING IS THE ONLY SAFE EDIT — a
/// reordering silently renumbers every saved device row, which is index-aligned with this enum.
enum class Param : int {
	Enabled,
	/// ONE TYPE FOR THE WHOLE DEVICE — the types are constant for all morph points.
	Type,
	/// WHERE BETWEEN THE WAYPOINTS THE FILTER IS, 0..1. The ONLY modulatable control here.
	Morph,

	// PER WAYPOINT, GROUPED BY WAYPOINT rather than by field, so a row reads as four states rather
	// than as four parallel arrays. Cutoff, Resonance and the two type-following macros: gain lives
	// in a macro for the types that have one (MacroKind::Gain), which is why there is no Gain field
	// of its own — see filter_types.h's MacroKind.
	Cutoff0, Resonance0, Macro1_0, Macro2_0,
	Cutoff1, Resonance1, Macro1_1, Macro2_1,
	Cutoff2, Resonance2, Macro1_2, Macro2_2,
	Cutoff3, Resonance3, Macro1_3, Macro2_3,

	/// APPENDED, NOT INSERTED BESIDE `Type` WHERE THEY BELONG BY MEANING. This enum's own rule is
	/// that appending is the only safe edit, because a row is index-aligned with it; the device is
	/// unshipped so an insert would in fact be harmless today, and following the rule anyway is
	/// cheaper than reasoning about the exception every time somebody reads it. `firstFieldOf` is
	/// computed, so the sixteen waypoint fields stay contiguous either way.
	///
	/// KEYTRACK — supported as an option that tracks the offset, and it is free: `MultimodeFilter` adds `keyOffsetSemitones / 12` directly to
	/// `cutoffLog2Hz` (multimode_filter.cpp:704), which is the same log domain the waypoints are
	/// stored in. So the note shifts the INTERPOLATED corner and **the whole morph path transposes
	/// together** — every waypoint moves by the same number of semitones, and the shape a hand drew
	/// in the plane is preserved under transposition rather than distorted by it.
	Keytrack,
	/// ONE FILTER PER VOICE, OR ONE ON THE SUMMED BUS. It was here because keytrack required it; it no
	/// longer does (2026-10-04): on the bus, keytrack follows the one note the host picked — the
	/// filter's rule (multimode_filter.cpp's OpsFilter::refresh), restated in waypoint.cpp because it is
	/// a property of what keytrack MEANS rather than of either device. Poly stays for separation.
	Poly,

	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog NR), supplied and applied by the slot: the device
	/// never reads either. Mix is a crossfade (100% the wet), the wet Gain a fader position (unity at 0.5) on the wet
	/// only. Both defaults leave the device as it was, to the bit.
	Mix,
	WetGain,

	kCount,
};

/// ONE WAYPOINT: everything that differs between the four. The TYPE is not here, by design.
struct Waypoint {
	/// LOG2 HERTZ, NOT HERTZ, and this is load-bearing rather than a storage convenience: linear
	/// interpolation of a log quantity IS musical octave interpolation, so the morph sweeps evenly
	/// in octaves for free. Lerping hertz would crowd every intermediate corner toward the low end.
	float cutoffLog2Hz = static_cast<float>(fx::filter::kCutoffDefaultLog2);
	float resonance = 0.2f;
	float macro1 = 0.0f;
	float macro2 = 0.0f;

	friend bool operator==(const Waypoint&, const Waypoint&) = default;
};

struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — see `kParamDescs`, and the static_assert below that pins
	/// the two together so a fresh state cannot depend on which road it arrived by.
	bool enabled = true;
	bool keytrack = false;
	/// DEFAULT OFF, and that is a correctness default rather than a compatibility one: a bus filter is
	/// what an effect in a chain normally is, and per-voice is the deliberate choice.
	bool poly = false;
	/// SHARED BY ALL FOUR. See the header note.
	fx::filter::Type type = fx::filter::Type::Lowpass;
	float morph = 0.0f;
	std::array<Waypoint, kWaypoints> point {};

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// Row: the serialized form of Settings, index-aligned with `Param`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// THE FILTER STATE AT A MORPH POSITION — the whole device in one function.
///
/// IT RETURNS THE FILTER'S OWN `Settings` ON PURPOSE. That is what makes `filter::gainAt()` and
/// `MultimodeFilter` reusable without a line of new DSP: the card draws the interpolated state
/// through the same evaluator it draws the four waypoints with, and the audio path hands the result
/// to a filter that already exists.
///
/// BY INDEX, PIECEWISE-LINEAR, IN EQUAL THIRDS — NOT BY ARC LENGTH along the drawn line. The choice was
/// a polyline, and the dots' positions set each waypoint's cutoff and resonance, NOT how long the
/// morph spends reaching them. The two coincide only when the dots happen to be evenly spaced, which
/// is precisely the coincidence that would let an arc-length implementation pass a test and then
/// drift the moment a dot is dragged.
[[nodiscard]] fx::filter::Settings settingsAtMorph(const Settings& settings, double morph);

/// WHICH SEGMENT A MORPH POSITION FALLS IN, and how far along it — the index arithmetic on its own.
///
/// IT IS SEPARATE BECAUSE THE FENCEPOST CANNOT BE TESTED THROUGH `settingsAtMorph`. At morph == 1.0
/// the fraction is 0, so an unclamped segment reads `point[4]` — out of bounds — and then multiplies
/// it by zero. The returned Settings is correct, the read is undefined behaviour, and a value
/// assertion on the output CANNOT SEE IT: a known-bad that deleted the clamp came back GREEN.
///
/// So the claim "the last segment is 2, never 3" is made about the number itself. Same reason this
/// repo grew `voicePhaseForTest` and its neighbours: if the claim is about a value, assert the value
/// rather than something computed from it.
struct MorphAt {
	size_t segment = 0; ///< 0..kWaypoints-2
	double fraction = 0.0; ///< 0..1 within that segment
};
[[nodiscard]] MorphAt morphAt(double morph);

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "filter", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "wayp",
	.name = "Waypoint",
	.hasCard = true,
	/// DECLARED, AND THE ARM EXISTS IN THE SAME COMMIT. `hasExpandedView` is
	/// `traits.hasExpanded && entryFor(desc) != nullptr` — both halves or neither. OWLTT shipped the
	/// declaration without the table entry once and the result was *"THERE IS NO EXPANDED VIEW AVAILABLE."*
	.hasExpanded = true,
	/// WHICH CONTROL DECIDES HOW MANY OF THIS DEVICE THERE ARE. Architecture, not a setting — see
	/// device.h. Declared because `Poly` exists, and `Poly` exists because keytrack requires it.
	.perVoiceControl = static_cast<int>(Param::Poly),
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	// THE SLOT BLENDS THESE (backlog NR): a plain crossfade around the device, which hands back the wet.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

/// ONE WAYPOINT'S FOUR DESCRIPTORS, so the table below cannot describe waypoint 2 with waypoint 1's
/// ranges. The name is qualified because a host's parameter list is flat and four controls called
/// "Cutoff" would be four rows nobody can tell apart — filter_types.h's own rule, applied here for
/// the same reason.
#define FEATHERS_WAYPOINT_FIELDS(n)                                                                \
	{"cutoff_" n, "Cutoff " n, fx::filter::kCutoffMinLog2, fx::filter::kCutoffMaxLog2, fx::filter::kCutoffDefaultLog2, false,         \
	 device::Unit::LogHertz, {}, false, false,                                                     \
	 "Corner frequency of waypoint " n " — the morph sweeps between these"},                       \
	{"resonance_" n, "Resonance " n, 0.0, 1.0, 0.2, false, device::Unit::Percent, {}, false, false,                 \
	 "Resonance of waypoint " n},                                                                  \
	{"macro_1_" n, "Macro 1 " n, 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, false, false,                   \
	 "First type-following macro of waypoint " n " — gain on the types that have one"},            \
	{"macro_2_" n, "Macro 2 " n, 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, false, false,                   \
	 "Second type-following macro of waypoint " n }

// NONE OF THE PER-WAYPOINT CONTROLS IS MODULATABLE, and that is a decision rather than an
// oversight: no per-waypoint modulation. `Morph` alone declares true. It is also what makes the
// storage question disappear — sixteen non-destination fields need no host ids, and the roadmap's
// "does not fit a 16-id block" worry belonged to a world where a filter's controls were clap params.
/// How many of the filter's types a waypoint offers — see the Type row.
inline constexpr int kTypeCount = static_cast<int>(fx::filter::Type::Brickwall);

inline constexpr device::ParamDesc kParamDescs[] = {
    // ON BY DEFAULT — all effects should default to on when they are added.
    // See `fx::filter::kParamDescs` for the decision and what it replaced.
    {"enabled", "Enabled", 0.0, 1.0, 1.0, true, device::Unit::Choice, fx::filter::kOffOnNames, true, false,
     "Whether the waypoint filter is in the chain"},
    // THE FILTER'S TYPES UP TO THE COMB, NOT ITS BRICKWALL HP (backlog NL): a waypoint is a point on a cutoff x
    // RESONANCE plane, and the brickwall has no resonance, so its dots would drag an axis that does nothing.
    {"type", "Type", 0.0, static_cast<double>(kTypeCount - 1), 0.0, true,
     device::Unit::Choice, std::span<const char* const>(fx::filter::kTypeNames, kTypeCount), false, false,
     "Filter type — SHARED by all four waypoints, so the morph never changes type mid-sweep"},
    {"morph", "Morph", 0.0, 1.0, 0.0, false, device::Unit::Percent, {}, true, false,
     "Where between the four waypoints the filter sits — 0 is waypoint 1, 1 is waypoint 4"},

    FEATHERS_WAYPOINT_FIELDS("1"),
    FEATHERS_WAYPOINT_FIELDS("2"),
    FEATHERS_WAYPOINT_FIELDS("3"),
    FEATHERS_WAYPOINT_FIELDS("4"),

    {"keytrack", "Keytrack", 0.0, 1.0, 0.0, true, device::Unit::Choice, fx::filter::kOffOnNames, false, false,
     "Does the corner follow the note — shifts every waypoint by the same interval, so the morph "
     "path transposes rather than distorting"},
    {"poly", "Poly", 0.0, 1.0, 0.0, true, device::Unit::Choice, fx::filter::kOffOnNames, false, false,
     "One filter per voice, or one on the summed bus — separation against glue"},
    {"waypoint_mix", "Waypoint Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true, false,
     "Dry against filtered, around the whole device"},
    {"waypoint_wet_gain", "Waypoint Wet Gain", 0.0, 1.0, 0.5, false, device::Unit::Fader, {}, true, false,
     "Level of the filtered signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},
};

#undef FEATHERS_WAYPOINT_FIELDS

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// THE ENABLE'S TWO DECLARATIONS, PINNED TOGETHER — see `fx::owltt` for the full statement of why a
/// descriptor default and a struct initialiser that disagree give a device two different fresh
/// states.
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

// INLINE CONSTEXPR IN THE HEADER, NOT A DEFINITION IN THE .cpp, and this is a requirement rather
// than a style match. `device_registry.h`'s `widestDeviceControls()` walks every registered device
// AT COMPILE TIME to size the modulation matrix stride, so a descriptor table that is merely `extern
// const` is "not usable in a constant expression" and the static_assert cannot run.
//
// IT ALSO ONLY BREAKS THE WHOLE BUILD. `ninja feathers-fx` and the device's own test target both
// compiled happily with the table in the .cpp — nothing in either instantiates that assert. The
// scoped build hid it exactly as the playbook says a scoped build does.

/// THE RENDER PATH. Phase 2 of the plan: one `MultimodeFilter` driven at the interpolated state.
///
/// THERE IS NO NEW DSP IN THIS DEVICE AND THAT IS THE DESIGN. `settingsAtMorph` returns the FILTER's
/// own `Settings`, so the ops hand an existing filter an existing settings type; the whole device is
/// the interpolation plus the plumbing below.
extern const device::AudioOps kAudioOps;

/// TEST ONLY — THE SETTINGS THE DSP WAS ACTUALLY HANDED, read out of an ops instance.
///
/// IT EXISTS BECAUSE A FILTER'S CORNER IS NOT RECOVERABLE FROM ITS OUTPUT. This repo found that
/// independently twice in one week and grew `Synth::voiceFilterTargetForTest` for it; the same claim
/// arrives here one layer down. A case that asked "did the morph move the corner" by comparing two
/// renders would pass whenever ANYTHING differed, which is the partial-effect blindness the rule is
/// about. If the claim is about a value, read the value.
///
/// A FUNCTION RATHER THAN A PUBLISHED STRUCT: the ops state is an implementation detail living in an
/// anonymous namespace, and a test reaching it through a reinterpret_cast would be asserting against
/// a layout nobody promised.
[[nodiscard]] const fx::filter::Settings& targetForTest(const void* storage);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	// NO OUTPUTS: this is an effect, not a modulator — it has nothing to offer the rack as a source.
	// Spelled rather than defaulted because `-Werror=missing-field-initializers` makes every field of
	// this struct a DECISION, which is the same guard test_mod_mseg.cpp relies on for `hasExpanded`.
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::FilterEq,
};

} // namespace feathers::fx::waypoint
