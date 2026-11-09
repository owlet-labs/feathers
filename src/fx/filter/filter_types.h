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
#include "fx/fx_tenant.h"

#include <array>
#include <cstddef>

// WHAT THE MULTIMODE FILTER IS, with none of what it does — the types, the parameter descriptors,
// the macro mapping and the response evaluator, in a header that pulls in no DSP and no SIMD.
//
// Split out from multimode_filter.h for one concrete reason and one general one. The concrete one:
// the plugin's parameter table is BUILT from the descriptors below, and params.cpp has no business
// compiling a filter to read a table. The general one: enumerating a device's controls and drawing
// its curve are things a host might want without ever rendering a sample, and a module that makes
// those cheap is a module that is easier to put somewhere else. The sample loop lives next door.
//
// THIS FILE INCLUDES NOTHING FROM src/plugin, and neither does anything else under src/fx. See
// fx/fx_tenant.h for why, and tests/test_fx_filter.cpp for the case that reads these sources and
// proves it.

namespace feathers::fx::filter {


/// The dropdown, in dropdown order. Every entry but the comb is a real SVF mode that changes the
/// sound; a family that may join it later is a lane "· soon" entry and is not in this enum, because
/// this enum is what the DSP switches on.
///
/// ALLPASS IS DELIBERATELY ABSENT although CytomicSVF has it. On a summed bus with no parallel dry
/// path there is nothing for its phase rotation to interfere with, so it would be a menu entry that
/// changes a number and not a sound — the honesty rule, applied to a dropdown. It becomes honest the
/// day this device grows a Mix control, and not before. (That control's id is 0x0309: Slope took the
/// next free slot, 0x0308, because the block hands out ids by position and cannot hold one open.)
///
/// COMB is not a model of anything and is not an SVF mode: it is a feedback delay of one period,
/// with a one-pole damper inside the loop. It is here rather than in a device of its own because a
/// comb IS a filter — it has a corner (the delay's own frequency), a resonance (the loop gain) and a
/// closed-form magnitude response, so it answers every question the card asks a type. See
/// kCombLineSamples for the memory it costs and why that cost is paid unconditionally.
///
/// THE FOUR "CAMP B" MODELS ARE GONE (licence migration). Diode Ladder, the
/// two-pole with a diode-pair resonance, and the two K35s sat between Bell and Comb; they were
/// transcriptions of GPL code, and the decision was to DROP them rather than rewrite them. The enum
/// was renumbered cleanly — patch compatibility is not a constraint yet (CLAUDE.md) — so Comb is 8.
/// A clean-room model, if one ever comes, is new work from published sources, not a restoration.
enum class Type : int {
	Lowpass,
	Highpass,
	Bandpass,
	Notch,
	Peak,
	LowShelf,
	HighShelf,
	Bell,
	Comb,
	/// THE KEYTRACKED BRICKWALL (backlog NL): an order-8 elliptic high-pass, dsp/elliptic_highpass.h — the same
	/// one HARM's High-pass mode runs. CUTOFF NAMES THE FREQUENCY TO REMOVE (f0): it and everything below are at
	/// least 60 dB down and the octave above is within 0.1 dB. So with Keytrack on, a Cutoff at middle C
	/// (`kKeytrackReferenceKey`, 261.6 Hz) removes every note's fundamental — the one setting the backlog asks
	/// for. APPENDED: a Choice is stored by label, so nothing saved moves. Not an SVF mode, like the comb.
	Brickwall,
	kCount,
};

inline constexpr const char* kTypeNames[] = {
	"Lowpass", "Highpass", "Bandpass", "Notch", "Peak", "Low Shelf", "High Shelf", "Bell", "Comb", "Brickwall HP",
};
static_assert(std::size(kTypeNames) == static_cast<size_t>(Type::kCount));

/// The tenant's params, in declaration order.
///
/// THE NAMES QUALIFY THEMSELVES — "Filter Cutoff", not "Cutoff" — because a device does not get to
/// assume what else is in the list it lands in. This one landed in a table that already had four
/// controls called "Macro n", and the plugin requires every name in it to be distinct; a host with a
/// flat parameter list has the same problem and no group column to lean on. The device is the only
/// party that knows its own name, so the device is where the qualifying happens. It matches what the
/// plugin's own table already does with the sound slots: "A Zone", in a module called "Sound A".
/// The plugin's id block is assigned in this order, so
/// APPENDING IS THE ONLY SAFE EDIT: a reordering here silently renumbers every saved patch.
enum class Param : int {
	Enabled,
	Type,
	Cutoff,
	Resonance,
	Drive,
	Keytrack,
	Macro1,
	Macro2,
	Slope,
	Spread,
	/// APPENDED, so no existing control's index moves — a control index is what a saved device row
	/// stores, and the row is index-aligned with this enum.
	Poly,
	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog NR), supplied and applied by the slot: the device
	/// never reads either. Mix is a crossfade (100% the wet), the wet Gain a fader position (unity at 0.5) on the wet
	/// only. Both defaults leave the device as it was, to the bit.
	Mix,
	WetGain,
	kCount,
};

inline constexpr const char* kOffOnNames[] = {"Off", "On"};

/// HOW STEEP THE ROLL-OFF IS, as a count of poles wearing its decibels-per-octave name.
///
/// ALL FOUR ENTRIES SHIP AT ONCE, and that is a decision rather than an ambition.
/// A stepped param stores an INDEX, so a list that grew later by appending would read "12, 24, 6, 18"
/// in every menu forever. The list is therefore written in its final order on the first day, which is
/// what puts a one-pole in scope: the SVF this module is built on is structurally two-pole and cannot
/// make an odd slope by itself.
enum class Slope : int {
	db6,
	db12,
	db18,
	db24,
	kCount,
};

inline constexpr const char* kSlopeNames[] = {"6 dB", "12 dB", "18 dB", "24 dB"};
static_assert(std::size(kSlopeNames) == static_cast<size_t>(Slope::kCount));

/// How many poles a slope names. One per 6 dB per octave, which is what makes the names arithmetic
/// rather than decoration.
constexpr int polesOf(Slope slope) {
	return static_cast<int>(slope) + 1;
}

/// Cutoff is stored as log2(Hz) — the same trick the amp edges play with log2(seconds), and for the
/// same reason: a host's automation lane is linear in the stored value, and a frequency control that
/// is linear in hertz spends nine tenths of its travel above 2 kHz. Spelled out rather than computed
/// so the table stays constexpr.
inline constexpr double kCutoffMinLog2 = 4.321928094887362;   // log2(20)
inline constexpr double kCutoffMaxLog2 = 14.287712379549449;  // log2(20000)
inline constexpr double kCutoffDefaultLog2 = 9.965784284662087; // log2(1000)

/// **THE FILTER DEVICE'S KNOB, WIDENED (Z8, filter cutoff seems to not go all the way to the top)** — 0.5 Hz
/// to 24 kHz. At 20 kHz the knob's top was a real corner: Low Pass was -3 dB (6 dB/oct) to -9 dB (24 dB/oct) at 20 kHz and
/// High Pass the same at 20 Hz. MEASURED at the new ends (tests/test_fx_filter.cpp, "Z8: ..."): Low Pass at 24 kHz is within
/// 0.006 dB at 20 kHz at 44.1 kHz and 0.002 dB at 48 kHz, every slope and resonance, because the vendor clamps the corner to
/// 0.499·sr; High Pass at 0.5 Hz is within 0.009 dB at 20 Hz at 44.1, 48, 96 and 192 kHz. KNOWN LIMIT, accepted: at 96 kHz and
/// above 24 kHz is a real corner, so Low Pass at the top loses at 20 kHz: -2.2 dB at 96 kHz and -2.6 dB at 192 kHz at the
/// defaults (12 dB, resonance 0.2), and at worst (over every slope and resonance 0, 0.2, 1) -6.0 dB and -6.7 dB.
///
/// THE OLD THREE STAY for Waypoint, which borrows them for its own cutoffs and was left alone.
inline constexpr double kCutoffWideMinLog2 = -1.0;                // log2(0.5)
inline constexpr double kCutoffWideMaxLog2 = 14.550746785383243;  // log2(24000)
/// A FULL-DEPTH ROUTE STILL MOVES THE OLD KNOB'S SPAN, 9.97 octaves: the range widened, the meaning of a route's depth did not.
inline constexpr double kCutoffRouteSpan = kCutoffMaxLog2 - kCutoffMinLog2;

/// THOSE THREE ARE THE KNOB'S TRAVEL, NOT THE FILTER'S LIMIT, and the difference is the whole of a
/// bug found: 20 Hz to 20 kHz is where a HAND should be able to put the corner, chosen so that
/// ten octaves fit a knob's arc. What the DSP can run at is bounded by Nyquist, and setCoeff applies
/// that itself — it clamps f/sr to 0.499 whatever it is handed. So the offsets that arrive after the
/// knob — a key offset from the keytracking architecture, a modulation source — are free to carry
/// the corner outside this range, and the sum is bounded only far enough out to keep the arithmetic
/// finite.
///
/// SIXTEEN OCTAVES is past everything that can legitimately ask. The keyboard reaches 5.6 octaves
/// either side of middle C, and a modulation route at full depth reaches the knob's own span, which
/// is ten; the guard exists so that exp2 of a corrupt value cannot become an infinity on its way to
/// a coefficient, not to have an opinion about music.
inline constexpr double kCutoffGuardOctaves = 16.0;
inline constexpr double kCutoffGuardLoLog2 = kCutoffMinLog2 - kCutoffGuardOctaves;
inline constexpr double kCutoffGuardHiLog2 = kCutoffMaxLog2 + kCutoffGuardOctaves;

/// BOTH MACROS ARE BIPOLAR AND NEUTRAL AT ZERO, in every type. That invariant is what lets the type
/// change under them without the sound jumping, and it is pinned by a case per mechanism: a shelf at
/// gain 0 is flat to 1e-6, and a spread or morph weight of zero is bit-identical to the mechanism
/// being absent.
///
/// THE TRAILING `true` IS "A SOURCE MAY BE ROUTED HERE" (fx::ParamDesc::modulatable), and this device
/// says it of its five CONTINUOUS controls and of nothing else. The three it withholds it from are
/// withheld for three different reasons, none of them "it would be hard":
///
///   - ENABLED is a switch. Modulating it would be a source that turns the device on somewhere in the
///     middle of its travel, which is a gate rather than a modulation, and a gate on a filter that
///     hard-bypasses is a click with a schedule.
///   - TYPE is a stepped choice. A ramp across it does not sweep anything; it steps through eight
///     unrelated responses, and each step is a different filter with the same state.
///   - KEYTRACK IS AN ARCHITECTURE, which is the one that matters. Off is one filter on the summed
///     bus; on is one filter per voice. A source crossing that threshold would rebuild the signal
///     topology block by block — not a modulation, a hazard.
// STEREO (backlog KU, design: "stereo = true on all six modulatable controls"): cutoff, res, drive, the two
// macros and spread each take a value per ear when a stereo route reaches them. Those rows are written by field
// name because `stereo` is the LAST member.
inline constexpr fx::ParamDesc kParamDescs[] = {
	// **ON BY DEFAULT — all effects should default to on when they are added.**
	//
	// THIS SAID OFF, and the reason it gave was real for the world it was written in: *"an empty fx
	// chain has to render the same bits it rendered before this device existed"* — the filter was a
	// pinned slot every instance carried whether or not anyone wanted one, so arriving switched on
	// would have changed the sound of a plugin nobody had touched.
	//
	// **THE DECISION IS ABOUT ADDING, AND ADDING IS NOW A GESTURE RATHER THAN A GIVEN.** A device
	// reaches a chain through the browser's (+), and that press IS the hand asking. A device that
	// arrives silent shows a card that is not in the signal — the honesty rule's failure case wearing
	// a different hat, and the rack's own add path had already decided the same way for itself.
	//
	// THE ONE PINNED INSTANCE THAT DID NOT ARRIVE BY A PRESS is the factory patch's filter at slot 0,
	// and `defaultFxChain` (audition.cpp) now switches THAT one off by name rather than by relying on
	// this number. Its default cutoff is 1 kHz, so a fresh instance shipping enabled would be a
	// plugin that low-passes everything before a hand has touched it. The two facts are separable and
	// are now separately stated.
	//
	// The bypass is still HARD rather than a unity-gain pass: process() returns without touching the
	// buffer. That half was never about the default.
	{"filter_enabled", "Filter Enabled", 0.0, 1.0, 1.0, true, fx::ParamUnit::Choice, kOffOnNames, true, false,
	 "Bypass switch — off removes the filter from the chain"},
	{"filter_type", "Filter Type", 0.0, static_cast<double>(Type::kCount) - 1.0, 0.0, true, fx::ParamUnit::Choice,
	 kTypeNames, false, false, "Which filter shape — lowpass, highpass, bandpass, notch and more"},
	// THE ONE ROW WRITTEN BY FIELD NAME, because it is the one row that sets both bools and
	// `true, true` at the end of a positional list says nothing about which is which. The second of
	// them is the statement that this range is the KNOB's: a modulation source, like the key offset,
	// may carry the corner outside it, and Nyquist is what stops it.
	// A NEW KEY (Z8): the range widened, so a stored cutoff means a different knob position; `filter_cutoff` is retired.
	{.key = "filter_cutoff_wide", .name = "Filter Cutoff",
	 .min = kCutoffWideMinLog2,
	 .max = kCutoffWideMaxLog2,
	 .def = kCutoffDefaultLog2,
	 .stepped = false,
	 .unit = fx::ParamUnit::LogHertz,
	 .names = {},
	 .modulatable = true,
	 .offsetsMayLeaveRange = true,
	 .tooltip = "Where in the spectrum the filter acts — a modulation source can push it past the knob's ends",
	 .modSpan = kCutoffRouteSpan,
	 .stereo = true},
	{.key = "filter_res", .name = "Filter Res", .min = 0.0, .max = 1.0, .def = 0.2, .stepped = false, .unit = fx::ParamUnit::Percent,
	 .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
	 .tooltip = "How much the corner resonates — higher is a louder, narrower peak that can self-oscillate",
	 .stereo = true},
	// Drive is an AMOUNT and not a gain, so that zero is exactly the untouched signal: the shaper is
	// mixed in by this number, and mixing in nothing is the identity in floating point as well as in
	// algebra. A drive knob whose "0" still ran a tanh would be a control that lies at rest.
	{.key = "filter_drive", .name = "Filter Drive", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = fx::ParamUnit::Percent,
	 .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
	 .tooltip = "Saturation in front of the filter — 0 is the untouched signal, more is louder harmonics",
	 .stereo = true},
	// KEYTRACK IS TONAL, not an amount and not the architecture (that is Poly, since 2026-08-23): on, the
	// corner follows the note — per voice, the voice's; on the bus, the one note the host picked (2026-10-04).
	// The module takes a per-instance key offset; the owner decides which note each instance hears.
	{"filter_keytrack", "Filter Keytrack", 0.0, 1.0, 0.0, true, fx::ParamUnit::Choice, kOffOnNames, false, false,
	 "On: the corner follows the note — each voice its own with Poly, the host's one held note on the bus"},
	// The macros are the most modulation-worthy controls on the device: one knob that means the right
	// thing for whatever type is selected, which is exactly what a hand wants a macro pointed at.
	{.key = "filter_macro_1", .name = "Filter Macro 1", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = fx::ParamUnit::Percent,
	 .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
	 .tooltip = "Type-following macro — its meaning changes with the filter type",
	 .stereo = true},
	{.key = "filter_macro_2", .name = "Filter Macro 2", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = fx::ParamUnit::Percent,
	 .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
	 .tooltip = "Second type-following macro — its meaning changes with the filter type",
	 .stereo = true},
	// SLOPE IS NOT MODULATABLE, and it inherits Type's own reasoning word for word: a ramp across it
	// does not sweep anything. It steps between filters of different orders, and the stage that
	// appears at the step starts from zero state — a click with a schedule, exactly as a modulated
	// type would be. Its DEFAULT IS 12 dB because that is the filter this device has always been:
	// a patch saved before this control existed omits the id, keeps the default, and renders the
	// samples it always rendered.
	{"filter_slope", "Filter Slope", 0.0, static_cast<double>(Slope::kCount) - 1.0,
	 static_cast<double>(Slope::db12), true, fx::ParamUnit::Choice, kSlopeNames, false, false,
	 "How steep the filter's falloff is — 6, 12, 18 or 24 dB per octave"},
	// SPREAD IS A PARAM AND NOT A MACRO, and the argument is what a macro is FOR: a meaning that
	// changes with the type. Gain's does; Morph's does. Spread's never did — it offsets a second
	// corner by ±kSpreadOctaves identically for all eight types, and the old table half-admitted it
	// by spending macro 1 on Spread for the five types that had nothing better to put there.
	//
	// MODULATABLE, which is the same argument from the other side: a continuous offset that is
	// musical to sweep is a destination, where the architecture switches beside it are not. Its
	// range is NOT a knob's window onto something wider — resolve() clamps the amount to ±1 and the
	// mechanism has nothing past it — so it does not ask for offsetsMayLeaveRange the way the cutoff
	// did. There the DSP had headroom the knob could not reach; here the end of the knob IS the end
	// of the mechanism.
	{.key = "filter_spread", .name = "Filter Spread", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = fx::ParamUnit::Percent,
	 .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
	 .tooltip = "Offsets a second corner by ±octaves — widens the image without changing the filter shape",
	 .stereo = true},
	// POLY IS ARCHITECTURAL AND KEYTRACK IS TONAL, and they were one control until 2026-08-23.
	//
	// the point im making is: poly vs keytrack are two different things. Poly decides HOW
	// MANY filters there are — one per voice, or one on the summed bus. Keytrack decides whether a
	// per-voice filter's corner FOLLOWS that voice's note. Conflating them shipped two of the four
	// combinations, and the missing one is the interesting one.
	//
	// WHY THE MISSING COMBINATION MATTERS, and it is not a tidiness argument: this filter is
	// NONLINEAR — its drive stage is a tanh in front of every type. SATURATION OF A SUM IS NOT THE
	// SUM OF SATURATIONS — six voices
	// through one filter intermodulate and clip against each other; six voices through six filters
	// each saturate alone and then add. Glue against separation, and until now you could only have
	// glue unless you also accepted keytracking.
	{"filter_poly", "Filter Poly", 0.0, 1.0, 0.0, true, fx::ParamUnit::Choice, kOffOnNames, false, false,
	 "One filter per voice, or one on the summed bus — separation against glue"},
	{"filter_mix", "Filter Mix", 0.0, 1.0, 1.0, false, fx::ParamUnit::Percent, {}, true, false,
	 "Dry against filtered, around the whole device"},
	{"filter_wet_gain", "Filter Wet Gain", 0.0, 1.0, 0.5, false, fx::ParamUnit::Fader, {}, true, false,
	 "Level of the filtered signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// WHAT THIS DEVICE IS, for a browser to group it by. See device::Traits::tags.
inline constexpr const char* kTags[] = { "filter", "fx" };

/// RETIRED KEYS (backlog JF §3.3): a key whose meaning changed is never reused.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "filter_cutoff", "2026-10-18",
	  "the knob widened from 20 Hz..20 kHz to 0.5 Hz..24 kHz (Z8), so a stored position means another cutoff: filter_cutoff_wide" },
};

inline constexpr fx::TenantTraits kTraits{
	.tag = "flt1",
	.name = "Filter",
	.hasCard = true,
	// I dont think we need an 'expanded' view for this one.
	.hasExpanded = false,
	// POLY IS THE ARCHITECTURE SWITCH. the point im making is: poly vs keytrack are two
	// different things. Keytrack is tonal and stays an ordinary control; this one decides whether
	// the host runs one filter per voice before the sum or one on the summed bus, which is not
	// something the device can act on by itself.
	.perVoiceControl = static_cast<int>(Param::Poly),

	/// THE SQUARE IN THE SHARED HEAD, named by index rather than assumed — every fx device has
	/// exactly one enable and no modulator has any, so the head draws it conditionally and the
	/// condition comes from here. See device.h.
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
	// THE SLOT BLENDS THESE (backlog NR): a plain crossfade around the device, which hands back the wet.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

/// EVERYTHING THIS DEVICE SAYS ABOUT ITSELF.
///
/// `outputs` IS EMPTY, AND THAT IS A DECLARATION RATHER THAN AN OMISSION. A filter publishes no
/// modulation signal — it is a destination and never a source — and saying so explicitly is what
/// lets a host hold effects and modulators in one table without asking which lane a device came
/// from. Empty is the common case for an effect and most will never fill it; the ones that do (an
/// envelope follower, a note effect that also modulates) declare theirs the same way the DAHDSR
/// does. See device::SourceDesc.
/// HOW THE HOST RUNS THIS DEVICE'S AUDIO — defined in multimode_filter.cpp, where the DSP class is.
///
/// DECLARED HERE AND DEFINED THERE so that this header stays free of the DSP: a card, a test or a
/// registry entry that only wants to know WHAT the filter is should not acquire the whole filter to
/// find out. Its address is still a constant expression, so kDescriptor below can point at it.
extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	// AND THE ENTRY POINT, WITHOUT WHICH THE TABLE ABOVE IS UNREACHABLE. It was declared and defined
	// and never pointed at for one commit, which made `audio` null for every device in the registry —
	// so nothing could be seated in the fx lane at all. Found the first time a caller asked the
	// descriptor whether this device can render, rather than by anything the table itself could say.
	.audio = &kAudioOps,
	.category = device::Category::FilterEq,
};

/// WHAT A MACRO DOES, which is one of exactly three mechanisms. The label changes with the type and
/// the id never does, so a host's automation lane keeps pointing at the same knob while the knob
/// changes meaning under it — which is the whole idea of a type-following macro.
enum class MacroKind : uint8_t {
	/// The gain axis of a type that has one: bellShelfAmp, ±24 dB, and exactly 1.0 at macro 0 —
	/// where every one of Bell, Low Shelf and High Shelf reduces to a flat response.
	Gain,
	/// The output mix walked toward a partner mode — free, per this file's header, because the two
	/// modes share the state. Positive and negative have different partners, and both weights reach
	/// zero at the same place, so the sign flip is inaudible.
	Morph,
	/// THE COMB'S DAMPER: a one-pole inside the feedback loop, bipolar around a coefficient of zero.
	/// Positive puts a lowpass in the loop, so each pass round the delay loses its top and the tail
	/// darkens as it decays — a plucked string. Negative puts the same pole at Nyquist instead, so the
	/// tail thins and brightens. AT ZERO THE DAMPER IS THE NUMBER ONE, exactly: the coefficient is
	/// (1 - |a|) / (1 - a z^-1), which at a = 0 is 1 in floating point and not merely in the limit —
	/// the same invariant the other two kinds carry, and the reason a type may change under a macro
	/// without the sound jumping.
	///
	/// IT SPENDS THE SLOT SPREAD LEFT EMPTY, which is what MacroKind::None below says that slot was
	/// being kept for.
	Damp,
	/// NOTHING, DECLARED AS NOTHING — which the card reads as "draw no knob here" rather than "draw
	/// one and disable it". A control that is present and inert is the thing the honesty rule is
	/// against; an absent one tells the truth about a type that has nothing to put there.
	///
	/// Five types have an empty slot since SPREAD BECAME A PARAM OF ITS OWN. A macro is for a meaning
	/// that CHANGES WITH THE TYPE — Gain does, Morph does — and Spread's never did: it offset a
	/// corner identically for all eight. The slot is left empty on purpose rather than filled with
	/// something plausible, because comb and phaser are the devices that will spend it, on damping
	/// and feedback character, and a placeholder now would be a control they had to take away.
	None,
};

struct MacroMap {
	MacroKind kind;
	const char* label;
	Type morphNeg; ///< Morph only
	Type morphPos; ///< Morph only
};

/// WHICH SLOPES A TYPE CAN WEAR, as a set rather than a rule — one bit per Slope entry, so a type
/// SAYS what it supports and every surface reads the same answer instead of re-deriving it. What a
/// card does with the rest is grey them; what the DSP does is fall back (see stagePlanOf).
///
/// The set is not the same for all eight, and the reason is arithmetic rather than taste. A one-pole
/// exists for a lowpass and a highpass and for nothing else — a bandpass and a notch each need a
/// zero on both sides of the corner and cannot be built from a single pole — so 6 and 18 belong
/// exactly to LP and HP. That is the same set Serum offers 6/12/18/24 to, arrived at from the
/// algebra rather than from copying the menu.
using SlopeMask = uint8_t;

constexpr SlopeMask slopeBit(Slope slope) {
	return static_cast<SlopeMask>(1u << static_cast<int>(slope));
}

/// Every entry: a type with a skirt and a one-pole form.
inline constexpr SlopeMask kSlopesAll =
    slopeBit(Slope::db6) | slopeBit(Slope::db12) | slopeBit(Slope::db18) | slopeBit(Slope::db24);
/// The even pair: a skirt to steepen, but no one-pole to make an odd order with.
inline constexpr SlopeMask kSlopesEven = slopeBit(Slope::db12) | slopeBit(Slope::db24);
/// One section and no choice — a GAIN SHAPE, where cascading does not steepen anything. Two bells in
/// series do not roll off twice as fast, they boost twice as many decibels, so a "24 dB" bell would
/// quietly be a +48 dB one.
inline constexpr SlopeMask kSlopesOne = slopeBit(Slope::db12);

struct TypeTraits {
	MacroMap macro1;
	MacroMap macro2;
	SlopeMask slopes;
	/// CAN THIS TYPE'S RESPONSE BE DRAWN IN CLOSED FORM? The card's promise is that the curve is the
	/// filter that plays, and it is pinned against the measured impulse response — but until now the
	/// promise lived in a comment, which is a thing a new model can be added without reading. Every
	/// member of this family satisfies it (one-pole, cascaded SVF, and the comb and phaser that are
	/// coming are all linear with elementary closed forms), so the field costs nothing today; it
	/// exists to be the thing that REFUSES a nonlinear model rather than letting one land and break
	/// the contract quietly.
	bool drawable;

	/// DOES SPREAD DO ANYTHING TO THIS TYPE'S SOUND? The second stage is an SVF section mixed over the
	/// cascade, and a type that does not run the cascade — the comb, today — sets `spreadRunning_ =
	/// false` in solve() and returns. A Spread knob on such a type is a live-looking control with
	/// nothing behind it, which found by turning it on the models that used to sit here. MEASURED
	/// rather than read off the branches: rendering each type at spread 0 and spread 1 gives identical
	/// buffers for the comb and different ones for every SVF type.
	bool spread;

	/// DOES RESONANCE DO ANYTHING TO THIS TYPE'S SOUND? False for the brickwall, whose shape is its design (the
	/// elliptic's Qs are fixed by its ripple and stopband); the card hides the knob, as it hides an unspent macro.
	bool resonance;
};

/// Does this type's Spread control reach the sound? The card asks before drawing the knob, because a
/// control you can turn that changes nothing is the one thing the honesty rule forbids outright.
constexpr bool spreadSupported(const TypeTraits& traits) {
	return traits.spread;
}

/// Does this type's Resonance control reach the sound? Asked by the card before drawing the knob.
constexpr bool resonanceSupported(const TypeTraits& traits) {
	return traits.resonance;
}

/// Does this type offer that slope? For the DSP's fallback and for whatever surface draws the
/// control, both reading the one declaration.
constexpr bool slopeSupported(const TypeTraits& traits, Slope slope) {
	return (traits.slopes & slopeBit(slope)) != 0;
}

/// The resonance value that makes one SVF section a Butterworth — maximally flat, Q = 1/√2 — in this
/// module's 0..1 units, where k = 2 − 2·res and Q = 1/k: Q = 1/√2 means k = √2, so res = 1 − 1/√2.
/// Spelled out rather than leaning on M_SQRT1_2.
///
/// IT IS WHAT MAKES A 24 dB SLOPE PEAK ONCE : the resonance setting goes on ONE
/// section and the rest are Butterworth, so the corner has a single peak of the height the knob
/// asks for. Two sections both wearing the knob's Q would square the peak — a resonance knob that
/// suddenly reaches twice as far in decibels because a slope changed.
inline constexpr float kButterworthRes = 0.2928932188134524f;

/// The type-following mapping table, and it lives here because it is module knowledge: the card
/// reads the labels off it, the DSP switches on the kinds, and nothing outside this module needs to
/// know that a shelf's macro 1 is its gain.
const TypeTraits& traitsOf(Type type);

/// THE CASCADE A SETTINGS ASKS FOR. Resolved in ONE place, like everything else in this module that
/// two readers need to agree about: the sample loop builds this many stages, the closed-form curve
/// multiplies this many responses, and the card asks it whether the control it is drawing does
/// anything at all. A second copy of the mapping would be a second place for a slope to mean
/// something different from what is heard.
struct StagePlan {
	/// SVF sections at the corner: two poles each. The first wears the resonance the knob asks for
	/// and any others are Butterworth — see kButterworthRes.
	int twoPoleStages{1};
	/// A one-pole section at the same corner, which is the only way this SVF family reaches an odd
	/// order at all.
	bool onePole{false};
	bool onePoleHighpass{false};

	friend bool operator==(const StagePlan&, const StagePlan&) = default;
};

/// Does the slope control do anything at all for this type — that is, does the type declare more
/// than one? False for the gain shapes, which offer one section and no choice. A surface asks this
/// to disable the whole control, and slopeSupported() above to grey the individual entries.
bool slopeAppliesTo(Type type);

/// THE SLOPE THIS TYPE WILL ACTUALLY RUN, given the one that was asked for. A card greys what a type
/// does not support, but a saved patch and a host's automation lane can still hold one, so this is a
/// total function: the nearest supported slope by pole count, ties going to the steeper.
///
/// IT EXISTS BECAUSE A GREYED BOX STILL HAS WORDS IN IT. A bell holding a stored 24 dB runs one
/// section — and a box reading "24 dB" over a filter running 12 is the card telling a lie it cannot
/// be caught in, which is the same fault as a curve drawn at a corner the engine is not at. The
/// surface asks this rather than printing what is stored.
Slope effectiveSlope(Type type, Slope slope);

/// The cascade for what effectiveSlope() resolved to. Total for the same reason and by the same
/// route — it calls that function rather than repeating the walk.
StagePlan stagePlanOf(Type type, Slope slope);

/// How far the second stage's corner moves at full spread.
inline constexpr float kSpreadOctaves = 2.0f;

//--------------------------------------------------------------------------------------------//
// THE COMB — its one buffer, and the three bounds that buffer imposes.                          //
//--------------------------------------------------------------------------------------------//

/// HOW LONG THE COMB'S DELAY LINE IS, in samples, and it must be a power of two because
/// dsp::SincDelayLine static_asserts that.
///
/// FOUR THOUSAND AND NINETY-SIX BECAUSE OF A 20 Hz FLOOR ("Comb tuning floor stays
/// at 20 Hz, full range, memory cost accepted"). A period of 20 Hz at 48 kHz is 2400 samples and the
/// next power of two above it is 4096, which floors the comb at 11.7 Hz there — comfortably under
/// the requirement.
///
/// IT IS A 48 kHz NUMBER, AND THAT IS A LIMIT RATHER THAN AN OVERSIGHT. The floor is
/// sampleRate / kCombMaxDelaySamples, so it RISES with the sample rate: 11.8 Hz at 48 kHz, 21.6 Hz at
/// 88.2 kHz, 23.5 Hz at 96 kHz, 47 Hz at 192 kHz. Only the 48 kHz figure is under the requirement. Doubling
/// this constant would carry 96 kHz under it and would double the memory below; that is a decision
/// with a price, so it is declared here rather than silently taken.
inline constexpr int kCombLineSamples = 4096;

/// THE SHORTEST DELAY THE SINC READ CAN BE ASKED FOR. dsp::SincDelayLine::read centres a twelve-tap
/// window six samples behind the read point, so a delay under six reaches for samples that have not
/// been written yet and would come back holding whatever was in that slot a whole line ago. The
/// clamp is the reason the comb has a CEILING — see combCeilingHz.
inline constexpr float kCombMinDelaySamples = 6.0f;

/// And the longest, four short of the line so the twelve-tap window and the write-side duplication
/// both stay inside the buffer.
inline constexpr float kCombMaxDelaySamples = static_cast<float>(kCombLineSamples) - 16.0f;

/// THE LOOP GAIN AT FULL RESONANCE. The same 0.98 the SVF path clamps its resonance to, and for the
/// same reason: it is the last value at which the thing still decays. At 0.98 a comb tuned to 1 kHz
/// takes about a third of a second to fall 60 dB, which is a pluck.
inline constexpr float kCombMaxFeedback = 0.98f;

/// The damper's pole at full macro travel. Short of 1 because the damper's own gain is (1 - |a|):
/// at |a| = 1 it would pass nothing at all, and the knob's last degree would be a mute.
inline constexpr float kCombMaxDamp = 0.9f;

/// THE DELAY, IN SAMPLES, THAT A COMB AT THIS CORNER ACTUALLY RUNS — the one place the conversion and
/// both clamps live, so the sample loop and the drawn curve cannot disagree about which comb is
/// playing. Fractional: the sinc read interpolates.
double combDelaySamples(double cutoffHz, double sampleRate);

/// THE HIGHEST CORNER A COMB CAN REACH at this sample rate, which is 8 kHz at 48 kHz. The knob goes
/// to 20 kHz and this does not, so the corner STOPS rather than continuing — declared here so a
/// surface can shade the band it cannot reach, per the roadmap's rule that a model capping below its
/// knob must say so. NOTHING SHADES IT YET.
inline constexpr double combCeilingHz(double sampleRate) {
	return sampleRate / static_cast<double>(kCombMinDelaySamples);
}

/// And the lowest, for the same reason from the other end.
inline constexpr double combFloorHz(double sampleRate) {
	return sampleRate / static_cast<double>(kCombMaxDelaySamples);
}

/// The gain range of a Gain macro, in decibels at full travel.
inline constexpr float kMacroGainDb = 24.0f;

/// **THE SIX CONTROLS THAT TAKE A RIGHT EAR** (backlog KU) — the modulatable continuous ones. Bit n of
/// `Settings::rightLive` is `Ear` n.
enum class Ear : uint8_t { Cutoff, Resonance, Drive, Macro1, Macro2, Spread, kCount };
[[nodiscard]] constexpr uint8_t earBit(Ear e) noexcept { return static_cast<uint8_t>(1u << static_cast<unsigned>(e)); }

/// Each `Ear`'s control, in `Ear` order.
inline constexpr Param kEarParams[] = { Param::Cutoff, Param::Resonance, Param::Drive,
	                                    Param::Macro1, Param::Macro2,    Param::Spread };
static_assert(std::size(kEarParams) == static_cast<size_t>(Ear::kCount));

/// Everything the DSP needs, in the tenant's own units. Assembled by the owner from the params (or
/// from anywhere else — a preview, a test, another host).
struct Settings {
	/// ON, BECAUSE THE DESCRIPTOR SAYS ON — see `kParamDescs`. The static_assert below is what makes
	/// this a restatement rather than a second declaration: `stateOf` fills an omitted field from
	/// `ParamDesc::def` while `Settings {}` gives a caller this initialiser, so a device whose two
	/// numbers disagree has a fresh state that depends on which road it arrived by. HARM paid for
	/// that lesson with five red cases and its own static_asserts; this is the same guard.
	bool enabled{true};
	Type type{Type::Lowpass};
	float cutoffLog2Hz{static_cast<float>(kCutoffDefaultLog2)};
	float resonance{0.2f};
	float drive{0.0f};
	float macro1{0.0f};
	float macro2{0.0f};
	/// How many poles the roll-off has. The default is the two-pole this device shipped with, so a
	/// Settings assembled anywhere — a preview, a test, a patch that predates the control — is the
	/// filter that was always there.
	Slope slope{Slope::db12};
	/// The second corner's offset, ±kSpreadOctaves at the ends, blended in by |spread|. Zero is one
	/// stage and is skipped entirely, which is what makes a resting Spread bit-identical to a device
	/// that has none. Its own field since it became a param: it used to arrive through macro1 for
	/// five of the eight types.
	float spread{0.0f};
	/// Semitones added to the cutoff, in the keytracking architecture: zero on the summed bus, and
	/// the voice's own distance from kKeytrackReferenceKey when there is one filter per voice.
	float keyOffsetSemitones{0.0f};

	/// **THE RIGHT EAR OF THE SIX** (backlog KU) — read only for the bits `rightLive` sets; every other control's
	/// right ear is its left. Zero `rightLive` is today's filter, solved once (the zero-cost promise).
	struct Right {
		float cutoffLog2Hz{static_cast<float>(kCutoffDefaultLog2)};
		float resonance{0.2f};
		float drive{0.0f};
		float macro1{0.0f};
		float macro2{0.0f};
		float spread{0.0f};
		friend bool operator==(const Right&, const Right&) = default;
	};
	Right right{};
	uint8_t rightLive{0};

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// THE RIGHT EAR AS A WHOLE `Settings` — each live control from `right`, everything else (type, slope, key offset,
/// the controls with no right ear of their own) the left's; `rightLive` is cleared in the result. One function, so
/// the DSP and the card build the same ear.
[[nodiscard]] Settings rightEarOf(const Settings& s);

/// THE TWO DECLARATIONS OF ONE FACT, PINNED TOGETHER. See `Settings::enabled` and HARM's identical
/// block for why a device whose descriptor and struct initialiser disagree has a fresh state that
/// depends on which road it arrived by.
static_assert(Settings {}.enabled
              == (kParamDescs[static_cast<size_t>(Param::Enabled)].def >= 0.5));

//--------------------------------------------------------------------------------------------//
// THE DEVICE'S OWN INDEXED VIEW OF ITSELF — what a host needs to carry this filter's controls   //
// without knowing what a filter is.                                                            //
//--------------------------------------------------------------------------------------------//

// WHY THIS EXISTS NOW AND DID NOT BEFORE. Every control of this device used to be a clap param, so
// the PLUGIN held its values and assembled Settings from its own table. That road is closed by
// design — docked effects' CLAP params are not on Feathers' surface; they are swallowed and
// reached by internal modulation — so the values move into the device's own state, and
// the host needs the same indexed vocabulary the DAHDSR already publishes: a row it can flatten into
// a channel, and a value it can read and write by control index.
//
// THE DAHDSR HAD ALL OF THIS AND THE FILTER HAD NONE OF IT, which is not an oversight in either: the
// envelope was built for the swallowed model from its first day and the filter was built for the
// parameter model. This is the filter catching up, and it is phase 2's actual prerequisite.

/// WHAT A PATCH STORES ABOUT THIS FILTER — every control it declares, which is NOT the same as what
/// its DSP takes.
///
/// `Settings` is the DSP's input and deliberately omits Keytrack, because keytracking decides how
/// many filters there are and where they sit — the owner's architecture rather than a number the
/// sample loop reads. But Keytrack IS a declared control and IS patch state, so a device asked to
/// carry its own state has to carry it.
///
/// The distinction generalises past this device: any device may declare controls that shape how the
/// host runs it rather than feeding its DSP directly, and a host storing "the device's state" must
/// store all of them while a DSP takes only its own.
struct State {
	Settings settings {};

	/// DOES THE CORNER FOLLOW THE NOTE. Tonal: per voice the voice's note, on the bus the one note the
	/// host picked (`OpsFilter::refresh`). It used to mean nothing without `poly`.
	bool keytrack { false };

	/// ONE FILTER PER VOICE, OR ONE ON THE SUMMED BUS. Architectural, and independent of keytrack:
	/// per-voice WITHOUT keytracking is the combination that did not exist before 2026-08-23, and it
	/// is the one that gives separation instead of glue on a nonlinear filter.
	bool poly { false };

	/// THE CHAIN FURNITURE (backlog NR): the slot applies these around the device, which never reads
	/// them, and a host storing the state stores them like Poly. Defaults are the descriptor's: 100% wet, wet Gain at unity.
	float mix { 1.0f };
	float wetGain { 0.5f };

	friend bool operator==(const State&, const State&) = default;
};

/// The controls as an indexed row, aligned with `Param` and therefore with `kParamDescs`.
using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

/// THE FIELD ORDER LIVES HERE AND NOWHERE ELSE, which is mod_rack.h's rule for the envelope applied
/// to this device: a host that wrote the order down separately would be a host to edit every time
/// the device grows a knob.
Row rowOf(const State& state);

/// `fieldsPresent` is how many of the row's entries the caller actually has — a payload written by
/// an older build is shorter, and the fields it omits keep their declared defaults. Every value is
/// clamped and rounded by its own descriptor on the way in, because a file is editable by hand and
/// outlives the build that wrote it.
State stateOf(const Row& row, size_t fieldsPresent);

/// One control's value, in the units its descriptor declares. Read off the row rather than off a
/// second switch over the same fields — the row IS this device's indexed view of itself.
float valueOf(const State& state, Param param);

/// Set one control, clamped and rounded by its own declaration. This is the door a card writes
/// through, and a card should not have to know a control's range to move it.
void setValueOf(State* state, Param param, float value);


/// Clamp every field to what its descriptor declares. Applied on the way out of stateOf for the
/// reason that function gives.
void sanitize(State* state);

/// The note at which a keytracking filter's cutoff is exactly what the knob says. Middle C, so that
/// switching keytrack on with a C4 held changes nothing.
inline constexpr int kKeytrackReferenceKey = 60;

/// THE CORNER THE FILTER ACTUALLY RUNS AT, in hertz: the stored cutoff, the key offset folded in,
/// and whatever bound the module puts on the sum. One line of code shared with the sample loop and
/// with gainAt(), so a caller asking where the corner ended up cannot be told a different number
/// from the one it hears.
double effectiveCutoffHz(const Settings& s);

/// THE MAGNITUDE RESPONSE, in closed form, for the card's curve — module-owned because the curve has
/// to show what the macros did, and the macros are this module's invention. It is the response of
/// the LINEAR part: drive is a waveshaper and has no magnitude response to draw.
///
/// Returns |H(f)| as a linear amplitude ratio.
double gainAt(const Settings& s, double sampleRate, double freqHz);

/// `n` log-spaced points from kProfileLoHz to kProfileHiHz. Writes into caller storage — this is
/// called from the editor, but it allocates nothing, so it is safe anywhere.
inline constexpr double kProfileLoHz = 20.0;
inline constexpr double kProfileHiHz = 20000.0;
void gainProfile(const Settings& s, double sampleRate, float* freqOut, float* gainOut, size_t n);


} // namespace feathers::fx::filter
