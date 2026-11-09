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

#include "device/device.h"
#include "device/registry.h"
#include "device/state_blocks.h"
#include "mod/arp/arp_types.h"
#include "mod/arp/genes.h"
#include "mod/arp/humanize.h"
#include "mod/arp/pattern.h"
#include "mod/arp/pattern_likes.h"
#include "mod/arp/theory.h"
#include "mod/modulator.h"

#include <array>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <iterator>

// THE PATTERN DEVICE — the house's terrain arp: four patterns, each a full
// control set (its terrain place, pins, Mode, scale boxes, rate), ONE of which plays at a time, chosen by a selector
// the DAW automates (decided 2026-10-30). It shares the classic Arp's clock, held chord, sounding table and saved block
// (`StepEmitter`) and its timing contract: transport-locked, every onset on its sample, deterministic.
//
// THE ROW (design §7.2): the globals, then four blocks of the same per-pattern controls. A patch stores each by its
// KEY, `pN.<control>`, so a control added to the block later is a non-event for every saved patch. What is not in the
// row — each pattern's banked rotate residue, its pin switches and its zoom — is the hand's block `pattern_hand`.

namespace feathers::mod::arp::pat {

using genes::Mode;
using pattern::Keys;

inline constexpr size_t kPatterns = 4;

//-------------------------------------------------------------------------------------------------//
// The controls                                                                                     //
//-------------------------------------------------------------------------------------------------//

/// THE GLOBALS.
enum class Global : int {
	Enabled,
	Selector,
	Keys,
	Latch,
	HumanizeMode,
	HumanizeAmount,
	Seed,
	Offset,
	Nudge,
	Run,
	FixedVelocity,
	Velocity,
	LikesWarp,
	/// **PLAY MODE** (design §7.6):
	/// Selector (the DAW-automated selector picks) or Cycle (each pattern, after its Repeats, takes its Next).
	PlayMode,
	/// **TRANSPOSE**: whole steps of
	/// the playing pattern's scale, then whole octaves — DEVICE-WIDE, so they hold across a selector or Cycle switch.
	TransposeSteps,
	TransposeOctaves,
	/// **FREE'S PHASE** (backlog QA: some groove settings, especially free, should get infinite zone knobs;
	/// design §12-bis): where along Free's walk, unbounded.
	/// Read by the Free feels alone; Humanize Amount is the other feels' depth. (Its Fine is retired: `free_fine`.)
	FreePhase,
	/// **WOBBLE** (zone 2;; design
	/// §12-ter): an unbounded position on orthogonal phi fields, read under every feel; 0 changes nothing.
	Wobble,
	/// **THE NOTE RANGE** (backlog QT; design §7.12): every note Pattern plays, folded by octaves into [Lowest, Highest].
	NoteLow,
	NoteHigh,
	kCount
};
inline constexpr size_t kGlobalCount = static_cast<size_t>(Global::kCount);
/// FREE PHASE'S DECLARED RANGE — not a bound (SKLITTER's `kCoordMax`: a zoned control is one infinite line, and the range is
/// only what a host needs to draw a fader; `offsetsMayLeaveRange` says so).
inline constexpr double kFreePhaseReach = humanize::kFreePhaseReach;
/// TRANSPOSE'S REACH: two octaves of a seven-note scale in steps; four octaves.
inline constexpr double kTransposeStepsReach = 14.0;
/// THE NOTE RANGE'S DEFAULTS (design §7.12): a floor near 30 Hz (MIDI 23, ~30.9 Hz) and no ceiling.
inline constexpr int kNoteLowDefault = 23;
inline constexpr int kNoteHighDefault = 127;
inline constexpr double kTransposeOctavesReach = 4.0;

/// ONE PATTERN'S CONTROLS, in its block's order.
enum class Field : int {
	Enable,
	Solo,
	Mode,
	Root,
	Scale,
	Degree,
	Size,
	Inversion,
	Spread,
	TimeBase,
	Rate,
	Length,
	Gate,
	Accent,
	PitchOnNotes,
	GlideTime,
	Thru, // incoming notes pass to the output
	Trig, // what an incoming note does to the clock
	Zone,
	ZoneOffset,
	Gamma,
	PlaceX,
	PlaceY,
	Rotate, // (Radius and Theta, the orbit, removed 2026-11-05)
	Pin0, // the fifteen pins, genes::Pin's order
	/// DRUM'S EIGHT LANES (design §11-ter): each lane's note, then each lane's on/off — fields, so a DAW automates them.
	DrumNote0 = Pin0 + static_cast<int>(genes::kPinCount),
	DrumOn0 = DrumNote0 + static_cast<int>(pattern::kDrumLanes),
	/// **THE TAB'S CYCLE CELL** (design §7.6), read in Cycle alone: how many times the pattern plays through before its
	/// Next, and what that Next is.
	Repeats = DrumOn0 + static_cast<int>(pattern::kDrumLanes),
	Next,
	/// **THE PATTERN'S OWN TRANSPOSE** (design §7.7): scale steps and octaves that travel with the pattern, ADDED to the device-wide pair.
	TransposeSteps,
	TransposeOctaves,
	/// **SPAN, FILL AND THE PHASE MARKERS**: the tab's slot length in Cycle (Auto, or whole bars),
	/// whether it is a FILL (it merges over the tail of the slot before it instead of taking a slot), where it starts (a
	/// phase on a normal tab, the window's start on a fill), where a fill's window ends, and the fill's trigger.
	Span,
	Fill,
	StartPhase,
	EndPhase,
	FillTrigger,
	/// **MODULATION'S LANES** (design §7.10), read in Modulation alone: Smooth, its Time, Polarity, and Identity (every lane
	/// to 0, the value that moves a destination by nothing).
	ModSmooth,
	ModTime,
	ModPolarity,
	ModNeutral,
	kCount,
};
inline constexpr size_t kFieldCount = static_cast<size_t>(Field::kCount);
inline constexpr size_t kParamCount = kGlobalCount + kPatterns * kFieldCount;
static_assert(kParamCount <= device::kMaxDeviceFields, "the row holds every pattern's controls (design §7.2)");

[[nodiscard]] constexpr size_t at(Global g) { return static_cast<size_t>(g); }
[[nodiscard]] constexpr size_t at(size_t pattern, Field f) {
	return kGlobalCount + pattern * kFieldCount + static_cast<size_t>(f);
}
[[nodiscard]] constexpr size_t pinAt(size_t pattern, genes::Pin p) {
	return at(pattern, Field::Pin0) + static_cast<size_t>(p);
}
[[nodiscard]] constexpr size_t drumNoteAt(size_t pattern, size_t lane) {
	return at(pattern, Field::DrumNote0) + lane;
}
[[nodiscard]] constexpr size_t drumOnAt(size_t pattern, size_t lane) {
	return at(pattern, Field::DrumOn0) + lane;
}

/// THE MODE LIST (decided 2026-10-30; Drum appended 2026-10-31; Progression 2026-11-04; Modulation 2026-11-06), genes::Mode's order.
inline constexpr const char* kModeNames[] = { "Melody", "Melody Mono", "Chords", "TB-Mono", "TB-Glide", "Drum", "Progression",
	                                            "Modulation" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

/// **CLAP'S TUNING EXPRESSION** (`CLAP_NOTE_EXPRESSION_TUNING`: semitones, relative to the note), restated so this
/// portable header need not include clap/events.h; `plugin/noteexpr_clap_check.h` asserts the two agree.
inline constexpr int32_t kTuningExpression = 2;
/// **TB-GLIDE'S SLIDE: A ONE-POLE LAG, EXPONENTIAL IN SEMITONES, τ = 22 ms, THE SAME FOR ANY INTERVAL** (design
/// §11.2-bis: the 303's slide is an RC low-pass on its 1 V/oct pitch CV, ~22 kΩ × 1 µF). By ear on the first build.
inline constexpr double kTb303SlideSeconds = 0.022;
/// **TB-GLIDE'S GLIDE TIME**: the lag's τ, per
/// pattern, as log2(seconds) (`Unit::LogSeconds`), 2 ms to 500 ms; the default is the 303's 22 ms, read out "22 ms (TB)".
inline constexpr double kGlideTimeMin = -8.965784284662087;  // log2(0.002)
inline constexpr double kGlideTimeMax = -1.0;                // log2(0.5)
inline constexpr double kGlideTimeDef = -5.506352666024790;  // log2(0.022)
/// How often the bend writes its tuning, in samples: the synth's control tick.
inline constexpr uint32_t kBendEventSamples = 64;
inline constexpr size_t kModeChoices = std::size(kModeNames);
inline constexpr const char* kRootNames[] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };
inline constexpr const char* kDegreeNames[] = { "I", "II", "III", "IV", "V", "VI", "VII" };
inline constexpr const char* kInversionNames[] = { "Auto", "Root", "1st", "2nd", "3rd" };
/// **THRU AND TRIG** (design §7.5). Auto resolves by Mode: Thru on in Drum; Trig Transport with Thru, Gate
/// without — a field's default cannot follow another field, so the mode-dependent default is a value of its own.
enum class Thru : uint8_t { Auto, Off, On, kCount };
inline constexpr const char* kThruNames[] = { "Auto", "Off", "On" };
inline constexpr size_t kThruCount = static_cast<size_t>(Thru::kCount);
inline constexpr const char* kTrigNames[] = { "Auto", "Transport", "Gate", "Restart" };
inline constexpr size_t kTrigCount = std::size(kTrigNames);
/// **PLAY MODE AND A TAB'S NEXT** (design §7.6). Next, Previous and Random walk the patterns in range (the enabled ones;
/// the soloed ones if any), Next and Previous wrapping in tab order; Random picks one of the OTHERS, derived from Seed and
/// the boundary's index; Stop ends playback (until playback starts again from before it); Loop stays.
enum class PlayMode : uint8_t { Selector, Cycle, kCount };
inline constexpr const char* kPlayModeNames[] = { "Selector", "Cycle" };
enum class NextAction : uint8_t { Next, Previous, Random, Stop, Loop, kCount };
inline constexpr const char* kNextNames[] = { "Next", "Previous", "Random", "Stop", "Loop" };
inline constexpr size_t kNextCount = std::size(kNextNames);
/// **SPAN** (design §7.8): Auto (Repeats × the cycle length, as before), or whole 4/4 bars.
inline constexpr const char* kSpanNames[] = { "Auto", "1 bar", "2 bars", "4 bars", "8 bars" };
inline constexpr size_t kSpanCount = std::size(kSpanNames);
inline constexpr int kSpanBars[] = { 0, 1, 2, 4, 8 };
static_assert(std::size(kSpanBars) == kSpanCount);
/// **MODULATION'S LANES** (design §7.10), per tab: how a lane moves between steps, how fast, its polarity, and the tab's
/// force to identity.
enum class ModSmooth : uint8_t { Hold, Glide, Ramp, kCount };
inline constexpr const char* kModSmoothNames[] = { "Hold", "Glide", "Ramp" };
inline constexpr size_t kModSmoothCount = std::size(kModSmoothNames);
static_assert(kModSmoothCount == static_cast<size_t>(ModSmooth::kCount));
inline constexpr const char* kModPolarityNames[] = { "Unipolar", "Bipolar" };
/// Glide's time constant and Ramp's length, in steps (Ramp reads up to one: it arrives on the next step).
inline constexpr double kModTimeMax = 4.0;
inline constexpr double kModTimeDef = 0.5;
struct ModLanes {
	ModSmooth smooth = ModSmooth::Hold;
	float time = static_cast<float>(kModTimeDef);
	bool bipolar = false;
	bool neutral = false;
	friend bool operator==(const ModLanes&, const ModLanes&) = default;
};
static_assert(kNextCount == static_cast<size_t>(NextAction::kCount));
inline constexpr uint32_t kSelectorMode = 1u << static_cast<unsigned>(PlayMode::Selector);
inline constexpr uint32_t kCycleMode = 1u << static_cast<unsigned>(PlayMode::Cycle);
inline constexpr int kMaxRepeats = 16;

constexpr std::array<const char*, theory::kScaleCount> makeScaleNames() {
	std::array<const char*, theory::kScaleCount> out {};
	for (size_t i = 0; i < theory::kScaleCount; ++i) {
		out[i] = theory::kScales[i].name;
	}
	return out;
}
inline constexpr auto kScaleNames = makeScaleNames();

/// THE GATES (CLAUDE.md's honesty rule), each a mask over the gating control's value.
inline constexpr uint32_t modeBit(Mode m) { return 1u << static_cast<unsigned>(m); }
inline constexpr uint32_t kChordsOnly = modeBit(Mode::Chords);
inline constexpr uint32_t kTbModes = modeBit(Mode::TbMono) | modeBit(Mode::TbGlide);
inline constexpr uint32_t kNotTb = modeBit(Mode::Melody) | modeBit(Mode::MelodyMono) | modeBit(Mode::Chords)
                                   | modeBit(Mode::Drum);
inline constexpr uint32_t kAllModes = kNotTb | kTbModes | modeBit(Mode::Progression);
/// THE MODES THAT VOICE CHORDS — Chords, and Progression's (design §11-quater): Inversion and Spread are theirs.
inline constexpr uint32_t kVoicedModes = kChordsOnly | modeBit(Mode::Progression);
/// THE MODES WHOSE CYCLE IS THE LENGTH BOX — every one but Progression, whose cycle is its progression (§11-quater). Modulation
/// is not in `kAllModes` (the modes that play notes) but reads Length.
inline constexpr uint32_t kLengthModes = (kAllModes & ~modeBit(Mode::Progression)) | modeBit(Mode::Modulation);
/// THE MODES THAT READ A PITCH — every one but Drum, whose keys only gate (design §11-ter).
inline constexpr uint32_t kPitched = kAllModes & ~modeBit(Mode::Drum);
/// THE MODES THAT READ THE DEGREE BOX — the pitched ones but Progression, which walks its own degrees from the tonic.
inline constexpr uint32_t kDegreeModes = kPitched & ~modeBit(Mode::Progression);
inline constexpr uint32_t kPoolKeys = 1u << static_cast<unsigned>(Keys::Pool);
inline constexpr uint32_t kFixedVelocityOn = 1u << 1;

constexpr uint32_t pinModes(genes::Pin p) {
	uint32_t mask = 0;
	for (size_t m = 0; m < static_cast<size_t>(Mode::kCount); ++m) {
		if (genes::pinRead(static_cast<Mode>(m), p)) {
			mask |= 1u << m;
		}
	}
	return mask;
}

/// THE TERRAIN'S DRAG WINDOWS (`shaper_types.h` on Colour and Gamma): Gamma and Rotate are unbounded phases whose
/// declared range is only what a host needs.
inline constexpr double kGammaWindow = 8388608.0;
inline constexpr double kRotateWindow = 8388608.0;
/// **HOW FAR A PLACE REACHES ACROSS THE PLANE, EITHER WAY, in travel units — far enough to be no wall a hand meets.**
/// It was 4: a drag or an edge pan from a place near
/// it stopped dead at ±4 while the map went on drawing past it, and zoomed out (a view ±4 wide) the whole map could be
/// fence. Feathers' travel is unbounded; this is the declared window a host needs, at 2^12 a step's precision is still
/// a millionth of a view (float, 24 bits) — the drag window, as Gamma's and Rotate's are.
inline constexpr double kPlaceReach = 4096.0;
inline constexpr double kPi = 3.14159265358979323846;

// ONE PATTERN'S BLOCK OF DESCRIPTORS, for pattern N (1-based in its key and name). A macro rather than four hand
// copies of thirty-five entries: a key is a string literal, and four copies are four places for one to drift.
#define FEATHERS_PATTERN_BLOCK(N)                                                                                     \
	{ .key = "p" #N ".enable", .name = "P" #N " Enable", .min = 0.0, .max = 1.0, .def = (N) == 1 ? 1.0 : 0.0,          \
	  .stepped = true, .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,                     \
	  .tooltip = "Puts this pattern in the selector's range. A pattern switched off is skipped", .shortName = "On", .retextsWords = true },  \
	{ .key = "p" #N ".solo", .name = "P" #N " Solo", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,              \
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,                                     \
	  .tooltip = "Plays only the soloed patterns, whatever the selector says (two soloed: the selector picks between them)", \
	  .shortName = "Solo", .retextsWords = true },                                                                                             \
	{ .key = "p" #N ".mode", .name = "P" #N " Mode", .min = 0.0, .max = static_cast<double>(kModeChoices) - 1.0,        \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kModeNames, .modulatable = true,             \
	  .tooltip = "What this pattern plays: a melody, a one-voice line, chords, or a 303-style line — TB-Mono overlaps its " \
	             "slides for a Legato synth, TB-Glide bends one held note itself for a synth without Legato",          \
	  .shortName = "Mode", .retextsWords = true },                                                                                             \
	{ .key = "p" #N ".root", .name = "P" #N " Root", .min = 0.0, .max = 11.0, .def = 0.0, .stepped = true,             \
	  .unit = device::Unit::Choice, .names = kRootNames, .modulatable = true,                                           \
	  .tooltip = "The scale's tonic under Pool (Transpose takes the root from the lowest key you hold)",               \
	  .gateControl = static_cast<int16_t>(at(Global::Keys)), .gateMask = kPoolKeys, .shortName = "Root", .retextsWords = true },              \
	{ .key = "p" #N ".scale", .name = "P" #N " Scale", .min = 0.0, .max = static_cast<double>(theory::kScaleCount) - 1.0, \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kScaleNames, .modulatable = true,            \
	  .tooltip = "The scale every note is chosen from",                                                                 \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = kPitched, .shortName = "Scale",        \
	  .retextsWords = true },                                                                                            \
	{ .key = "p" #N ".degree", .name = "P" #N " Degree", .min = 0.0, .max = 6.0, .def = 0.0, .stepped = true,          \
	  .unit = device::Unit::Choice, .names = kDegreeNames, .modulatable = true,                                         \
	  .tooltip = "The chord's degree in the scale; in the line modes, a transposition by scale degrees",                 \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = kDegreeModes, .shortName = "Degree",       \
	  .retextsWords = true },                                                                                            \
	{ .key = "p" #N ".size", .name = "P" #N " Size", .min = 0.0,                                                        \
	  .max = static_cast<double>(theory::ChordSize::kCount) - 1.0, .def = 2.0, .stepped = true,                         \
	  .unit = device::Unit::Choice, .names = theory::kChordSizeNames, .modulatable = true,                             \
	  .tooltip = "How many tones a chord stacks in the scale", .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), \
	  .gateMask = kChordsOnly, .shortName = "Size", .retextsWords = true },                                                                    \
	{ .key = "p" #N ".inversion", .name = "P" #N " Inversion", .min = 0.0, .max = 4.0, .def = 0.0, .stepped = true,   \
	  .unit = device::Unit::Choice, .names = kInversionNames, .modulatable = true,                                      \
	  .tooltip = "Auto leads each chord to the nearest voicing of the next; or a fixed inversion",                      \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = kVoicedModes, .shortName = "Inv", .retextsWords = true },    \
	{ .key = "p" #N ".spread", .name = "P" #N " Spread", .min = 0.0,                                                    \
	  .max = static_cast<double>(theory::Spread::kCount) - 1.0, .def = 0.0, .stepped = true,                           \
	  .unit = device::Unit::Choice, .names = theory::kSpreadNames, .modulatable = true,                                \
	  .tooltip = "How a chord is opened: close, a dropped voice, or spread over two octaves",                          \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = kVoicedModes, .shortName = "Spread" }, \
	{ .key = "p" #N ".time_base", .name = "P" #N " Time Base", .min = 0.0,                                              \
	  .max = static_cast<double>(kTimeBaseCount) - 1.0, .def = static_cast<double>(kDefaultTimeBase), .stepped = true, \
	  .unit = device::Unit::Choice, .names = kTimeBaseNames, .modulatable = true,                                       \
	  .tooltip = "The musical division one step lasts, or Hz to make Rate a step frequency", .shortName = "Base" },     \
	{ .key = "p" #N ".rate", .name = "P" #N " Rate", .min = kTimeLog2SecondsMin, .max = kTimeLog2SecondsMax,             \
	  .def = 0.0, .stepped = false, .unit = device::Unit::LogMultiple, .names = {}, .modulatable = true,                \
	  .tooltip = "How fast this pattern's steps go: a multiple of the division, or steps per second under Hz",          \
	  .shortName = "Rate" },                                                                                             \
	{ .key = "p" #N ".length", .name = "P" #N " Length", .min = 1.0, .max = static_cast<double>(genes::kMaxSteps),     \
	  .def = 16.0, .stepped = true, .unit = device::Unit::Number, .names = {}, .modulatable = true,                    \
	  .tooltip = "How many steps the pattern runs before it repeats (a Progression runs its own)",                      \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = kLengthModes, .shortName = "Length" }, \
	{ .key = "p" #N ".gate", .name = "P" #N " Gate", .min = 0.05, .max = 1.0, .def = 0.5, .stepped = false,             \
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,                                                  \
	  .tooltip = "How much of a step a note lasts before the Length pin varies it (TB lines are always half a step)",   \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = kNotTb, .shortName = "Gate" },        \
	{ .key = "p" #N ".accent", .name = "P" #N " Accent", .min = 0.0, .max = 1.0, .def = 0.25, .stepped = false,       \
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,                                                  \
	  .tooltip = "What an accented step adds to the velocity", .shortName = "Accent" },                                  \
	{ .key = "p" #N ".pitch_on_notes", .name = "P" #N " Pitch on Notes", .min = 0.0, .max = 1.0, .def = 1.0,          \
	  .stepped = true, .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,                    \
	  .tooltip = "As on a 303: the pitches advance only on notes, not on ties or rests, so the two lines drift against " \
	             "each other",                                                                                          \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = kTbModes, .shortName = "Pitch/Notes" }, \
	{ .key = "p" #N ".glide_time", .name = "P" #N " Glide Time", .min = kGlideTimeMin, .max = kGlideTimeMax,              \
	  .def = kGlideTimeDef, .stepped = false, .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,         \
	  .tooltip = "How long a TB-Glide slide takes to settle: the lag's time constant. 22 ms is the 303's own",           \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = modeBit(Mode::TbGlide),                \
	  .shortName = "Glide" },                                                                                            \
	{ .key = "p" #N ".thru", .name = "P" #N " Thru", .min = 0.0, .max = static_cast<double>(kThruCount) - 1.0,        \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kThruNames, .modulatable = false,            \
	  .tooltip = "Incoming notes pass to the output untouched. Auto: on in Drum (the kit's own notes pass, any other "  \
	             "note is a trigger), off otherwise",                                                                   \
	  .shortName = "Thru", .retextsWords = true },                                                                                           \
	{ .key = "p" #N ".trig", .name = "P" #N " Trig", .min = 0.0, .max = static_cast<double>(kTrigCount) - 1.0,        \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kTrigNames, .modulatable = false,            \
	  .tooltip = "What a trigger note does. Transport: nothing, the pattern runs with the transport. Gate: it plays "   \
	             "while one is held. Restart: each one starts the cycle again on its sample. Auto: Transport with Thru, " \
	             "Gate without",                                                                                        \
	  .shortName = "Trig" },                                                                                             \
	{ .key = "p" #N ".zone", .name = "P" #N " Zone", .min = 0.0, .max = 1023.0, .def = 0.0, .stepped = true,           \
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,                                                   \
	  .tooltip = "The coarse place in the pattern terrain", .shortName = "Zone" },                                       \
	{ .key = "p" #N ".zone_offset", .name = "P" #N " Zone Offset", .min = 0.0, .max = 1.0, .def = 0.0,                 \
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,                                 \
	  .tooltip = "The fine place inside the zone", .shortName = "Offset" },                                              \
	{ .key = "p" #N ".gamma", .name = "P" #N " Gamma", .min = 0.0, .max = kGammaWindow, .def = 0.0, .stepped = false, \
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,                                                   \
	  .tooltip = "The coarsest move through the terrain: a new family of patterns each step", .shortName = "Gamma" },   \
	{ .key = "p" #N ".place_x", .name = "P" #N " Place X", .min = -kPlaceReach, .max = kPlaceReach, .def = 0.0,        \
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,                                 \
	  .tooltip = "Where the puck sits on the terrain, across", .centred = true, .shortName = "X" },                      \
	{ .key = "p" #N ".place_y", .name = "P" #N " Place Y", .min = -kPlaceReach, .max = kPlaceReach, .def = 0.0,        \
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,                                 \
	  .tooltip = "Where the puck sits on the terrain, up", .centred = true, .shortName = "Y" },                          \
	{ .key = "p" #N ".rotate", .name = "P" #N " Rotate", .min = -kRotateWindow, .max = kRotateWindow, .def = 0.0,      \
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = false,                                \
	  .tooltip = "Turns the plane the puck travels in, leaving the pattern under the puck where it is",                 \
	  .centred = true, .shortName = "Rotate" },                                                                          \
	FEATHERS_PATTERN_PIN(N, Motion, motion, "how far the melody moves (off: a drone on the root)"),                    \
	FEATHERS_PATTERN_PIN(N, Leap, leap, "how much it leaps rather than steps"),                                         \
	FEATHERS_PATTERN_PIN(N, Range, range, "how wide its range is"),                                                     \
	FEATHERS_PATTERN_PIN(N, Density, density, "how many steps rest (off: none)"),                                       \
	FEATHERS_PATTERN_PIN(N, Length, length, "how much the note lengths vary"),                                          \
	FEATHERS_PATTERN_PIN(N, Hold, hold, "how many steps tie, slur or hold their chord"),                                \
	FEATHERS_PATTERN_PIN(N, Accent, accent, "how many steps are accented"),                                             \
	FEATHERS_PATTERN_PIN(N, Glide, glide, "how many steps slide into the next"),                                        \
	FEATHERS_PATTERN_PIN(N, Octave, octave, "how many steps jump an octave"),                                           \
	FEATHERS_PATTERN_PIN(N, Repeat, repeat, "how much the motif repeats"),                                              \
	FEATHERS_PATTERN_PIN(N, Answer, answer, "how much the second half answers the first"),                              \
	FEATHERS_PATTERN_PIN(N, Detail, detail, "how many steps take a grace note, a ghost or an extension"),               \
	FEATHERS_PATTERN_PIN(N, Ratchet, ratchet, "how many steps ratchet (2, 3 or 4 hits); off by default"),               \
	FEATHERS_PATTERN_PIN_VOICE_LEADING(N),                                                                             \
	FEATHERS_PATTERN_PIN(N, Approach, approach, "how many chord changes are approached by pickup notes; off by default"), \
	FEATHERS_PATTERN_DRUM_NOTE(N, Kick, kick, 36), FEATHERS_PATTERN_DRUM_NOTE(N, Snare, snare, 38),                     \
	FEATHERS_PATTERN_DRUM_NOTE(N, Closed Hat, closed_hat, 42), FEATHERS_PATTERN_DRUM_NOTE(N, Open Hat, open_hat, 46),   \
	FEATHERS_PATTERN_DRUM_NOTE(N, Clap, clap, 39), FEATHERS_PATTERN_DRUM_NOTE(N, Low Tom, low_tom, 45),                \
	FEATHERS_PATTERN_DRUM_NOTE(N, High Tom, high_tom, 50), FEATHERS_PATTERN_DRUM_NOTE(N, Ride, ride, 51),               \
	FEATHERS_PATTERN_DRUM_ON(N, Kick, kick), FEATHERS_PATTERN_DRUM_ON(N, Snare, snare),                                 \
	FEATHERS_PATTERN_DRUM_ON(N, Closed Hat, closed_hat), FEATHERS_PATTERN_DRUM_ON(N, Open Hat, open_hat),               \
	FEATHERS_PATTERN_DRUM_ON(N, Clap, clap), FEATHERS_PATTERN_DRUM_ON(N, Low Tom, low_tom),                             \
	FEATHERS_PATTERN_DRUM_ON(N, High Tom, high_tom), FEATHERS_PATTERN_DRUM_ON(N, Ride, ride),                          \
	{ .key = "p" #N ".repeats", .name = "P" #N " Repeats", .min = 1.0, .max = static_cast<double>(kMaxRepeats),         \
	  .def = 1.0, .stepped = true, .unit = device::Unit::Number, .names = {}, .modulatable = true,                      \
	  .tooltip = "In Cycle: how many times this pattern plays through before its Next",                                 \
	  .gateControl = static_cast<int16_t>(at(Global::PlayMode)), .gateMask = kCycleMode, .shortName = "Repeats" },      \
	{ .key = "p" #N ".next", .name = "P" #N " Next", .min = 0.0, .max = static_cast<double>(kNextCount) - 1.0,          \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kNextNames, .modulatable = true,              \
	  .tooltip = "In Cycle: what plays after this pattern's repeats — the next or previous enabled tab, a random other " \
	             "one (from Seed), stop, or this one again",                                                            \
	  .gateControl = static_cast<int16_t>(at(Global::PlayMode)), .gateMask = kCycleMode, .shortName = "Next" },                     \
	{ .key = "p" #N ".transpose_steps", .name = "P" #N " Transpose Steps", .min = -kTransposeStepsReach,               \
	  .max = kTransposeStepsReach, .def = 0.0, .stepped = true, .unit = device::Unit::Number, .names = {},            \
	  .modulatable = true,                                                                                              \
	  .tooltip = "Moves this pattern's notes this many steps of its scale, added to the device's Transpose (it travels " \
	             "with the pattern: a copy, a Cycle switch)",                                                           \
	  .shortName = "Steps" },                                                                                           \
	{ .key = "p" #N ".transpose_octaves", .name = "P" #N " Transpose Octaves", .min = -kTransposeOctavesReach,         \
	  .max = kTransposeOctavesReach, .def = 0.0, .stepped = true, .unit = device::Unit::Number, .names = {},          \
	  .modulatable = true,                                                                                              \
	  .tooltip = "Moves this pattern's notes this many octaves, added to the device's Transpose Octaves",              \
	  .shortName = "Octave" },                                                                                          \
	{ .key = "p" #N ".span", .name = "P" #N " Span", .min = 0.0, .max = static_cast<double>(kSpanCount) - 1.0,           \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = kSpanNames, .modulatable = true,              \
	  .tooltip = "In Cycle: how long this pattern's slot lasts — Auto (Repeats × its length) or whole bars, the "        \
	             "pattern looping inside it and cut at its end",                                                        \
	  .gateControl = static_cast<int16_t>(at(Global::PlayMode)), .gateMask = kCycleMode, .shortName = "Span" },          \
	{ .key = "p" #N ".fill", .name = "P" #N " Fill", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,               \
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,                                     \
	  .tooltip = "A fill takes no slot of its own: it plays over the end of the pattern before it, from Start to End, " \
	             "then hands back. Its trigger plays it over the end of whatever plays now",                            \
	  .shortName = "Fill" },                                                                                            \
	{ .key = "p" #N ".start", .name = "P" #N " Start", .min = 0.0, .max = static_cast<double>(genes::kMaxSteps) - 1.0,  \
	  .def = 0.0, .stepped = true, .unit = device::Unit::Number, .names = {}, .modulatable = true,                      \
	  .tooltip = "Where the pattern starts: its slot plays from this step and wraps round (a fill: where its window "   \
	             "starts). Drag the marker on the lane",                                                                \
	  .shortName = "Start" },                                                                                           \
	{ .key = "p" #N ".end", .name = "P" #N " End", .min = 1.0, .max = static_cast<double>(genes::kMaxSteps),            \
	  .def = static_cast<double>(genes::kMaxSteps), .stepped = true, .unit = device::Unit::Number, .names = {},         \
	  .modulatable = true,                                                                                              \
	  .tooltip = "A fill's window ends here, and the pattern under it carries on. Drag the marker on the lane",         \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Fill)), .gateMask = 2u, .shortName = "End" },              \
	{ .key = "p" #N ".fill_trigger", .name = "P" #N " Fill Trigger", .min = 0.0, .max = 1.0, .def = 0.0,                \
	  .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,                                \
	  .tooltip = "Fires this fill: it plays over the end of the current pattern's repeat, then that pattern carries on." \
	             " A bipolar source fires when it rises past +0.5.",                                                     \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Fill)), .gateMask = 2u, .shortName = "Go" },               \
	{ .key = "p" #N ".mod_smooth", .name = "P" #N " Smooth", .min = 0.0,                                                \
	  .max = static_cast<double>(kModSmoothCount) - 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,    \
	  .names = kModSmoothNames, .modulatable = true,                                                                    \
	  .tooltip = "How each lane moves from step to step: Hold jumps on the step, Glide slews toward it, Ramp arrives "  \
	             "on the next step in a straight line",                                                                 \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = modeBit(Mode::Modulation),            \
	  .shortName = "Smooth" },                                                                                          \
	{ .key = "p" #N ".mod_time", .name = "P" #N " Smooth Time", .min = 0.0, .max = kModTimeMax, .def = kModTimeDef,    \
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,                                 \
	  .tooltip = "Glide: how many steps the slew takes to get most of the way. Ramp: how much of the step the line "    \
	             "takes to reach the next step's value (up to the whole step)",                                         \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::ModSmooth)), .gateMask = 6u, .shortName = "Time" },        \
	{ .key = "p" #N ".mod_polarity", .name = "P" #N " Polarity", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,   \
	  .unit = device::Unit::Choice, .names = kModPolarityNames, .modulatable = true,                                    \
	  .tooltip = "Unipolar: each lane runs 0 to 1. Bipolar: −1 to 1. A lane that never hits rests at 0 either way",      \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = modeBit(Mode::Modulation),            \
	  .shortName = "Polarity" },                                                                                        \
	{ .key = "p" #N ".mod_neutral", .name = "P" #N " Identity", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,    \
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,                                     \
	  .tooltip = "On: every lane of this pattern rests at 0, so whatever it modulates is left as it is",                 \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = modeBit(Mode::Modulation),            \
	  .shortName = "Identity" }

/// DRUM'S LANE FIELDS (design §11-ter): a lane's note (General MIDI's kit by default) and its on/off, shown in Drum alone.
#define FEATHERS_PATTERN_DRUM_NOTE(N, L, key_, note)                                                                   \
	{ .key = "p" #N ".drum." #key_ ".note", .name = "P" #N " " #L " Note", .min = 0.0, .max = 127.0,                  \
	  .def = static_cast<double>(note), .stepped = true, .unit = device::Unit::Number, .names = {}, .modulatable = true, \
	  .tooltip = "The note the " #L " lane sounds in Drum",                                                             \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = modeBit(Mode::Drum), .shortName = #L }
#define FEATHERS_PATTERN_DRUM_ON(N, L, key_)                                                                           \
	{ .key = "p" #N ".drum." #key_ ".on", .name = "P" #N " " #L, .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,  \
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,                                     \
	  .tooltip = "Whether the " #L " lane plays",                                                                       \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = modeBit(Mode::Drum) | modeBit(Mode::Modulation), .shortName = #L }

/// **VOICE LEADING**: a pin like the
/// others but for its default, 1 — the strict least motion every chord had before the pin — and its two-word name.
#define FEATHERS_PATTERN_PIN_VOICE_LEADING(N)                                                                          \
	{ .key = "p" #N ".pin.voice_leading", .name = "P" #N " Voice Leading", .min = 0.0, .max = 1.0, .def = 1.0,       \
	  .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,                               \
	  .tooltip = "How closely each chord follows the one before: 0 block chords, each voiced on its own; 100% the "     \
	             "least motion; 50% exactly what the terrain decides",                                                  \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = pinModes(genes::Pin::VoiceLeading),   \
	  .shortName = "Voice Lead" }

#define FEATHERS_PATTERN_PIN(N, P, key_, what)                                                                         \
	{ .key = "p" #N ".pin." #key_, .name = "P" #N " " #P, .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,      \
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,                                                  \
	  .tooltip = "The terrain decides " what "; at 50% exactly what it decides, lower toward none, higher toward the most", \
	  .gateControl = static_cast<int16_t>(at((N) - 1, Field::Mode)), .gateMask = pinModes(genes::Pin::P),             \
	  .shortName = #P }

inline constexpr device::ParamDesc kParamDescs[] = {
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,
	  .tooltip = "Switches the pattern player on or off. Off, the notes you play pass straight through" },
	{ .key = "selector", .name = "Selector", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Which pattern plays: its range is split evenly between the enabled patterns (the soloed ones, if any). "
	             "A change lands on the next step",
	  .gateControl = static_cast<int16_t>(Global::PlayMode), .gateMask = kSelectorMode },
	{ .key = "keys", .name = "Keys", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = pattern::kKeysNames, .modulatable = true,
	  .tooltip = "Transpose: the lowest key you hold is the root. Pool: the pattern picks from the keys you hold, put in "
	             "the scale",
	  .retextsWords = true },
	{ .key = "latch", .name = "Latch", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,
	  .tooltip = "Keeps playing after you let go. A new chord, played after letting go of every key, replaces it" },
	{ .key = "humanize", .name = "Humanize", .min = 0.0, .max = static_cast<double>(humanize::Mode::kCount) - 1.0,
	  .def = 0.0, .stepped = true, .unit = device::Unit::Choice, .names = humanize::kModeNames, .modulatable = true,
	  .tooltip = "The feel: a swing or groove on the bar, timing and velocity a player would vary, a push or a lag, a strum "
	             "of each chord, or a flam",
	  .choiceAliases = humanize::kFeelAliases },
	{ .key = "humanize_amount", .name = "Humanize Amount", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true, .tooltip = "How much of the feel",
	  .gateControl = static_cast<int16_t>(Global::HumanizeMode), .gateMask = humanize::kDepthModes, // Free: Free Phase
	  .shortName = "Amount" },
	{ .key = "seed", .name = "Seed", .min = 0.0, .max = 999.0, .def = 0.0, .stepped = true, .unit = device::Unit::Number,
	  .names = {}, .modulatable = true,
	  .tooltip = "Rolls the dice: Humanize's feel and Cycle's Random Next, each from its own stream. The same seed at the same "
	             "song position always plays the same" },
	{ .key = "offset", .name = "Offset", .min = -kOffsetReach, .max = kOffsetReach, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "Shifts every pattern against the song's grid by whole steps" },
	{ .key = "nudge", .name = "Nudge", .min = -0.5, .max = 0.5, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Moves every step earlier or later by up to half a step", .centred = true },
	{ .key = "run", .name = "Run", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
	  .names = arp::kRunNames, .modulatable = false, // a run mode is chosen, not swept
	  .tooltip = "When it plays. Always: on the song's grid while the transport plays, and with it stopped on its own clock "
	             "from the first key you press. Transport: only while the transport plays. Note: from step 1 on the first "
	             "trigger key after every key was let go, at the song's tempo, for as long as one is held (legato carries on); "
	             "the cycle, its fills and Span count from that first key",
	  .choiceAliases = arp::kRunAliases },
	{ .key = "fixed_velocity", .name = "Fixed Velocity", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,
	  .tooltip = "Off: the terrain shapes each step's velocity around Velocity. On: every step has Velocity (plus its "
	             "accent)",
	  .shortName = "Fixed Vel" },
	{ .key = "velocity", .name = "Velocity", .min = 0.0, .max = 1.0, .def = 0.8, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "The velocity the steps are played at, before the terrain's shape and the accents" },
	{ .key = "likes_warp", .name = "Likes Warp", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = mod::kOffOnNames, .modulatable = true,
	  .tooltip = "On: each pattern's terrain bends toward the patterns you liked in its slot (fitted in the background; "
	             "until a fit is ready the pattern plays plain)",
	  .shortName = "Warp" },
	{ .key = "play_mode", .name = "Play Mode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kPlayModeNames, .modulatable = true,
	  .tooltip = "Selector: the selector picks the pattern that plays. Cycle: each pattern plays its Repeats, then its Next "
	             "takes over — derived from the song position, so a locate or a loop plays what the run from the start played there",
	  .shortName = "Play" },
	{ .key = "transpose_steps", .name = "Transpose Steps", .min = -kTransposeStepsReach, .max = kTransposeStepsReach,
	  .def = 0.0, .stepped = true, .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "Moves every note of whichever pattern plays this many steps of its scale, so it stays in key (a chord "
	             "moves with its degree). Edits move with it. Drum is not moved",
	  .shortName = "Steps" },
	{ .key = "transpose_octaves", .name = "Transpose Octaves", .min = -kTransposeOctavesReach,
	  .max = kTransposeOctavesReach, .def = 0.0, .stepped = true, .unit = device::Unit::Number, .names = {},
	  .modulatable = true,
	  .tooltip = "Moves every note of whichever pattern plays this many octaves, after the steps. Drum is not moved",
	  .shortName = "Octave" },
	// FREE'S PHASE (design §12-bis): SKLITTER's `free_phase` — unbounded (the range is only what a host draws a fader
	// over), a digit box, one unit of phase a full-depth route.
	{ .key = "free_phase", .name = "Free Phase", .min = -kFreePhaseReach, .max = kFreePhaseReach,
	  .def = 0.0, .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .offsetsMayLeaveRange = true,
	  .tooltip = "Where along Free's walk of phi grooves: 0 is straight, every other position a different groove, and "
	             "no end — it never comes back round. Drag a digit to move that place",
	  .modSpan = 1.0, .gateControl = static_cast<int16_t>(Global::HumanizeMode),
	  .gateMask = humanize::kFreeModes, .shortName = "Phase", .digitBox = true },
	{ .key = "wobble", .name = "Wobble", .min = -kFreePhaseReach, .max = kFreePhaseReach, .def = 0.0, .stepped = false,
	  .unit = device::Unit::Number, .names = {}, .modulatable = true, .offsetsMayLeaveRange = true,
	  .tooltip = "A second groove, a bit whacky: each step pushed or dragged, rushed or dragged in runs, ghosted or rung, "
	             "clipped short. 0 is none, every other position another, and no end. Under every feel",
	  .modSpan = 1.0, .shortName = "Wobble", .digitBox = true },
	// THE NOTE RANGE (design §7.12): MIDI notes, named in the plugin's note names (60 is C3, so the default floor, MIDI 23
	// at ~30.9 Hz, reads B-1). Not modulatable: a range is set, not swept (a moving bound would refold every note).
	{ .key = "note_low", .name = "Lowest Note", .min = 0.0, .max = 127.0, .def = static_cast<double>(kNoteLowDefault),
	  .stepped = true, .unit = device::Unit::Note, .names = {}, .modulatable = false,
	  .tooltip = "No note plays below this: a lower one is moved up by octaves until it is at or above it. Not in Drum or "
	             "Modulation",
	  .shortName = "Lowest" },
	{ .key = "note_high", .name = "Highest Note", .min = 0.0, .max = 127.0, .def = static_cast<double>(kNoteHighDefault),
	  .stepped = true, .unit = device::Unit::Note, .names = {}, .modulatable = false,
	  .tooltip = "No note plays above this: a higher one is moved down by octaves until it is at or below it. Not in Drum "
	             "or Modulation",
	  .shortName = "Highest" },
	FEATHERS_PATTERN_BLOCK(1),
	FEATHERS_PATTERN_BLOCK(2),
	FEATHERS_PATTERN_BLOCK(3),
	FEATHERS_PATTERN_BLOCK(4),
};
#undef FEATHERS_PATTERN_PIN
#undef FEATHERS_PATTERN_PIN_VOICE_LEADING
#undef FEATHERS_PATTERN_BLOCK
static_assert(std::size(kParamDescs) == kParamCount);

//-------------------------------------------------------------------------------------------------//
// The modulation sources                                                                           //
//-------------------------------------------------------------------------------------------------//

/// **SEVEN** (decided 2026-10-30): Step, Pitch, Velocity, Accent, Accent Sweep, Slide, Degree.
/// The design lists three more — Gate, Pattern, Contour. SEVEN WAS THE CAP until 2026-11-07, when outputs became sized per
/// device (`device::kMaxDeviceOutputs` is a bound of 64 now), so more cost
/// only this device's own storage.
///
/// **THEN THE IDENTITY GATES AND THE MODULATION LANES** (by decision): each tab's gate ("Tab N": 1 while it
/// sounds), then each tab's eight lanes ("Tab N Lane L": its Modulation-mode levels, Bipolar by its own Polarity), tab by
/// tab — 43 in all. A route names them by key, so the card may show a tab's alone and a route to another still reads.
enum class Output : size_t { Step, Pitch, Velocity, Accent, AccentSweep, Slide, Degree, Tab0, Lane0 = Tab0 + kPatterns, kCount = Lane0 + kPatterns * pattern::kDrumLanes };
inline constexpr size_t kFirstTabOutput = static_cast<size_t>(Output::Tab0);
inline constexpr size_t kFirstLaneOutput = static_cast<size_t>(Output::Lane0);
[[nodiscard]] constexpr size_t tabOutput(size_t tab) { return kFirstTabOutput + tab; }
[[nodiscard]] constexpr size_t laneOutput(size_t tab, size_t lane) { return kFirstLaneOutput + tab * pattern::kDrumLanes + lane; }
/// **WHAT THE CARD SHOWS** (design §7.10, `DeviceHost::outputsShown`): the seven note sources and the open tab's gate —
/// or, while the tab is in Modulation, its eight lanes in their place (a Modulation tab plays no notes, and a lane of
/// another Mode never moves: the honesty rule). Eight chips either way, which is what the card's foot holds in a chain
/// unit; a Modulation tab's gate is the window's. The window shows every one.
[[nodiscard]] constexpr uint64_t cardOutputsFor(size_t tab, bool modulation) {
	uint64_t m = modulation ? 0u : ((uint64_t { 1 } << kFirstTabOutput) - 1u) | (uint64_t { 1 } << tabOutput(tab));
	for (size_t l = 0; modulation && l < pattern::kDrumLanes; ++l) {
		m |= uint64_t { 1 } << laneOutput(tab, l);
	}
	return m;
}
/// **WHAT THE WINDOW SHOWS**: the seven note sources and every tab's gate, and the window's open tab's eight lanes while
/// it is in Modulation — never another tab's. A route from a hidden one reads on (view, not routing).
[[nodiscard]] constexpr uint64_t windowOutputsFor(size_t tab, bool modulation) {
	uint64_t m = (uint64_t { 1 } << kFirstLaneOutput) - 1u; // the seven and the four gates
	for (size_t l = 0; modulation && l < pattern::kDrumLanes; ++l) {
		m |= uint64_t { 1 } << laneOutput(tab, l);
	}
	return m;
}
#define FEATHERS_PATTERN_LANE_OUT(N, L)                                                                                \
	{ .key = "tab" #N ".lane" #L, .name = "T" #N " Mod " #L, .bipolar = true,                \
	  .bipolarControl = static_cast<int>(at((N) - 1, Field::ModPolarity)), .cardHidden = true }
#define FEATHERS_PATTERN_LANES_OUT(N)                                                                                  \
	FEATHERS_PATTERN_LANE_OUT(N, 1), FEATHERS_PATTERN_LANE_OUT(N, 2), FEATHERS_PATTERN_LANE_OUT(N, 3),                 \
	    FEATHERS_PATTERN_LANE_OUT(N, 4), FEATHERS_PATTERN_LANE_OUT(N, 5), FEATHERS_PATTERN_LANE_OUT(N, 6),             \
	    FEATHERS_PATTERN_LANE_OUT(N, 7), FEATHERS_PATTERN_LANE_OUT(N, 8)
inline constexpr device::SourceDesc kOutputs[] = {
	{ .key = "step", .name = "Step", .bipolar = false },
	{ .key = "pitch", .name = "Pitch", .bipolar = false },
	{ .key = "velocity", .name = "Velocity", .bipolar = false },
	{ .key = "accent", .name = "Accent", .bipolar = false },
	{ .key = "accent_sweep", .name = "Accent Sweep", .bipolar = false },
	{ .key = "slide", .name = "Slide", .bipolar = false },
	{ .key = "degree", .name = "Chord Degree", .bipolar = false },
	{ .key = "tab1.active", .name = "Tab 1", .bipolar = false },
	{ .key = "tab2.active", .name = "Tab 2", .bipolar = false, .cardHidden = true },
	{ .key = "tab3.active", .name = "Tab 3", .bipolar = false, .cardHidden = true },
	{ .key = "tab4.active", .name = "Tab 4", .bipolar = false, .cardHidden = true },
	FEATHERS_PATTERN_LANES_OUT(1),
	FEATHERS_PATTERN_LANES_OUT(2),
	FEATHERS_PATTERN_LANES_OUT(3),
	FEATHERS_PATTERN_LANES_OUT(4),
};
#undef FEATHERS_PATTERN_LANES_OUT
#undef FEATHERS_PATTERN_LANE_OUT
static_assert(std::size(kOutputs) == 43, "7 + 4 identity gates + 32 Modulation lanes (design §7.10)");
/// **THE 303'S ACCENT SWEEP** (design §11.2-bis: consecutive accents stack on a capacitor, so the cutoff climbs on a run
/// of them): each accented onset adds this much, decaying with this time constant. Both by ear; the shape is the firm part.
inline constexpr double kAccentSweepCharge = 0.4;
inline constexpr double kAccentSweepSeconds = 0.3;
/// The newest accents the sweep sums (older ones have decayed to nothing at any sane tempo).
inline constexpr int kAccentSweepDepth = 8;
static_assert(std::size(kOutputs) == static_cast<size_t>(Output::kCount));

inline constexpr const char* kTags[] = { "note", "sequencer", "arpeggiator", "note-effect" };

/// RETIRED — never reuse.
inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "contour", "2026-10-30",
	  "the output cap (kMaxDeviceOutputs = 7): Accent Sweep took its slot; it returns under a new key if the cap grows" },
	{ "pattern.likes.v1", "2026-10-31",
	  "a like now also stores its full point (31 phases, 31 rate offsets) so its mark follows a turn: pattern.likes.v2" },
	{ "pattern.edits.v2", "2026-11-05", "the anchor lost Radius and Theta (removed): v3" },
	{ "p1.radius", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "p1.theta", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "p2.radius", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "p2.theta", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "p3.radius", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "p3.theta", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "p4.radius", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "p4.theta", "2026-11-05", "radius and theta are not needed — the orbit is gone" },
	{ "free_fine", "2026-11-07",
	  "a second zone was meant as orthogonal phi fields for more nuanced control, not finer steps" },
	{ "pattern.edits.v1", "2026-11-04",
	  "an edit's key was absolute and signed by key, so one made over the window's C4 was never heard at another held key: "
	  "v2 keys from the cycle's root, signed by degree and octave" },
	{ "pattern.likes.v2", "2026-10-31",
	  "the point in doubles (two floats each): a float phase loses its fraction far from home, and the warp reads angles: v3" },
	{ "stop_with_transport", "2026-11-07",
	  "the Only w/ Transport switch became Run (Always, Transport, Note) — its Off and On are carried",
	  "run" },
};

inline constexpr device::Traits kTraits {
	.tag = "PAT",
	.name = "Pattern",
	.hasCard = true,
	.hasExpanded = true,
	.enableControl = static_cast<int>(Global::Enabled),
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
};

//-------------------------------------------------------------------------------------------------//
// What is saved                                                                                    //
//-------------------------------------------------------------------------------------------------//

/// **THE HAND'S BLOCK** (design §7.2): per pattern, its banked rotate residue (2N doubles — `SliceResidue`, the house's
/// fold), its fifteen pin switches (one word, a bit each) and its zoom. Written by the editor, never by the audio thread.
inline constexpr uint32_t kResidueWords = static_cast<uint32_t>(2 * genes::kRowCount) * 4;
inline constexpr uint32_t kPatternHandWords = kResidueWords + 1 + 2;
inline constexpr uint32_t kHandWords = kPatternHandWords * static_cast<uint32_t>(kPatterns);

/// **THE HAND'S EDITS** (`pattern.edits.v1`, design §15-bis): per pattern, the BASE PLACE they were made at (Place X/Y,
/// Zone, Zone Offset, Gamma, Radius, Theta — the authored controls, not their modulation) and the Mode, then every step's
/// edit. Rotate is not in the anchor: a turn keeps the pattern under the puck (the fold), so it keeps the edits too.
inline constexpr size_t kAnchorFields = 5;
inline constexpr std::array<Field, kAnchorFields> kAnchorOf { Field::PlaceX, Field::PlaceY, Field::Zone, Field::ZoneOffset,
	                                                          Field::Gamma };
struct EditOverlay {
	std::array<float, kAnchorFields> anchor {};
	Mode mode = Mode::Melody;
	std::array<pattern::StepEdit, genes::kMaxSteps> steps {};
	[[nodiscard]] bool any() const noexcept {
		for (const pattern::StepEdit& e : steps) {
			if (e.on) {
				return true;
			}
		}
		return false;
	}
	friend bool operator==(const EditOverlay&, const EditOverlay&) = default;
};
using EditState = std::array<EditOverlay, kPatterns>;
/// **V4** (2026-11-07): each step's v3 words, then its eight cell levels (16-bit each: a stored level reads back exactly)
/// and which cells a hand set. **V3 IS STILL READ** (`pattern.edits.v3`): it loads into v4 with no cells.
inline constexpr uint32_t kEditStepWordsV3 = 10; ///< velocity and gate as floats (two words each): a round trip is exact
inline constexpr uint32_t kEditStepWords = kEditStepWordsV3 + static_cast<uint32_t>(pattern::kDrumLanes) + 1;
[[nodiscard]] constexpr uint32_t editPatternWords(uint32_t stepWords) {
	return 2 * static_cast<uint32_t>(kAnchorFields) + 1 + stepWords * static_cast<uint32_t>(genes::kMaxSteps);
}
inline constexpr uint32_t kEditPatternWords = editPatternWords(kEditStepWords);
inline constexpr uint32_t kEditsWords = kEditPatternWords * static_cast<uint32_t>(kPatterns);
inline constexpr uint32_t kEditsWordsV3 = editPatternWords(kEditStepWordsV3) * static_cast<uint32_t>(kPatterns);
/// A cell level's 16-bit step: the level a hand sets is held to it (`pat::quantizeLevel`), so it round-trips exactly.
[[nodiscard]] inline float quantizeLevel(float v) {
	return static_cast<float>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f)) / 65535.0f;
}
[[nodiscard]] EditState editStateOf(const float* in, size_t n) noexcept;
void writeEditState(const EditState& state, float* out) noexcept;
/// **ARE PATTERN `p`'S EDITS STALE** — made at another base place or under another Mode than `fieldAt` reads now (any
/// read: the authored controls, which is what a hand or an automation moves; never their modulation).
[[nodiscard]] bool editsStale(const EditOverlay& overlay, size_t p, const std::function<float(size_t)>& fieldAt) noexcept;
/// The anchor `fieldAt` reads now, for pattern `p`.
[[nodiscard]] std::array<float, kAnchorFields> anchorNow(size_t p, const std::function<float(size_t)>& fieldAt) noexcept;

/// **IS A CONTROL SHOWN UNDER THIS PATTERN'S MODE, BEYOND ITS DECLARED GATE** — Drum hides the pitch boxes its keys never
/// read (Root, gated on Keys, cannot also be gated on Mode); Modulation hides those and Thru and Trig too, and Smooth Time
/// outside Modulation (it is gated on Smooth). The card and the window ask it with the declared gate.
[[nodiscard]] bool drumHides(size_t control, Mode mode) noexcept;

uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity);
bool restoreBlock(void* storage, size_t index, const float* in, size_t n);
bool applyBlock(void* storage, size_t index, const float* in, size_t n);
inline constexpr device::StateBlockDesc kBlocks[] = {
	{ .key = "arp_play", .floats = kPlayWords },
	{ .key = "pattern_hand", .floats = kHandWords, .handEdited = true },
	/// THE LIKES, PER PATTERN SLOT (by decision): a hand's act, kept with the patch, never read by the audio
	/// thread (the window scores and paints with them).
	{ .key = "pattern.likes.v3", .floats = likes::kBlockWords, .handEdited = true },
	/// THE HAND'S EDITS, PER PATTERN (design §15-bis): the overlay on the cycle decoded at the place it was made, with
	/// that place — captured state, so it saves; cleared when the base place moves (`pat::editsStale`).
	{ .key = "pattern.edits.v3", .floats = kEditsWordsV3 },
	/// **V4: THE SAME, WITH EACH CELL'S OWN LEVEL** (Drum's velocity, Modulation's value). V3 above is read into it on a load
	/// (`PatternDevice::restoreEditsV3`) and never written: a patch saved from here carries v4.
	{ .key = "pattern.edits.v4", .floats = kEditsWords, .handEdited = true },
};
inline constexpr size_t kLikesBlock = 2;
inline constexpr size_t kEditsBlockV3 = 3;
inline constexpr size_t kEditsBlock = 4;
inline constexpr device::StateOps kStateOps {
	.blocks = kBlocks, .read = &readBlock, .restore = &restoreBlock, .apply = &applyBlock
};

extern const device::ModOps kModOps;
extern const device::NoteOps kNoteOps;
/// **THE DAW'S WORDS** (design §16): the selector names the pattern it picks ("2 · D Dorian", "3 · C Minor, iv, 1st
/// inv", "(solo)", "No pattern"), and Root, Scale, Degree (a numeral cased by the chord's quality), Inversion and Mode
/// print and parse as themselves. `pattern_text.cpp`.
extern const device::TextOps kTextOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = kOutputs,
	.mod = &kModOps,
	.notes = &kNoteOps,
	.state = &kStateOps,
	.text = &kTextOps,
	.category = device::Category::Notes,
};

//-------------------------------------------------------------------------------------------------//
// The decoded row                                                                                  //
//-------------------------------------------------------------------------------------------------//

/// ONE PATTERN'S CONTROLS, decoded.
struct PatternSettings {
	bool enable = false;
	bool solo = false;
	Mode mode = Mode::Melody;
	int root = 0;
	size_t scale = 0;
	int degree = 0;
	theory::ChordSize size = theory::ChordSize::Triad;
	int inversion = theory::kAuto;
	theory::Spread spread = theory::Spread::Close;
	size_t timeBase = kDefaultTimeBase;
	float rate = 0.0f;
	size_t length = 16;
	float gate = 0.5f;
	float accent = 0.25f;
	bool pitchOnNotes = true;
	/// DRUM'S LANE MAP (its fields).
	pattern::DrumLanes drum {};
	/// TB-Glide's lag τ as log2(seconds) (`Field::GlideTime`).
	float glideTime = static_cast<float>(kGlideTimeDef);
	/// THRU AND TRIG (`Field::Thru`, `Field::Trig`): as stored, Auto included — `thruOn` and `trigOf` resolve them.
	Thru thru = Thru::Auto;
	uint8_t trig = 0;
	int zone = 0;
	float zoneOffset = 0.0f;
	double gamma = 0.0;
	float placeX = 0.0f;
	float placeY = 0.0f;
	float rotate = 0.0f;
	std::array<float, genes::kPinCount> pins = genes::kPinAmountByDefault;

	[[nodiscard]] bool synced() const noexcept { return timeBase > 0; }
	friend bool operator==(const PatternSettings&, const PatternSettings&) = default;
};

/// **THRU, RESOLVED**: On, or Auto in Drum.
[[nodiscard]] constexpr bool thruOn(const PatternSettings& q) noexcept {
	// MODULATION plays no notes, so every incoming note passes and the transport runs it (design §7.10).
	return genes::isModulation(q.mode) || q.thru == Thru::On || (q.thru == Thru::Auto && q.mode == Mode::Drum);
}
/// **TRIG, RESOLVED**: as chosen, or Auto — Transport with Thru (nothing else would start it), Gate without (as ever).
[[nodiscard]] constexpr TrigMode trigOf(const PatternSettings& q) noexcept {
	if (genes::isModulation(q.mode)) {
		return TrigMode::Transport;
	}
	switch (q.trig) {
	case 1: return TrigMode::Transport;
	case 2: return TrigMode::Gate;
	case 3: return TrigMode::Restart;
	default: return thruOn(q) ? TrigMode::Transport : TrigMode::Gate;
	}
}

/// ONE TAB'S CYCLE CELL, decoded.
struct CycleCell {
	int repeats = 1;
	NextAction next = NextAction::Next;
	friend bool operator==(const CycleCell&, const CycleCell&) = default;
};

/// ONE TAB'S SPAN, FILL AND PHASE MARKERS, decoded (design §7.8). `end` is as stored (1..64); a reader clamps it to the
/// cycle's length and keeps it past `start`.
struct TabPhase {
	int spanBars = 0; ///< 0: Auto
	bool fill = false;
	int start = 0;
	int end = static_cast<int>(genes::kMaxSteps);
	friend bool operator==(const TabPhase&, const TabPhase&) = default;
};

/// EVERYTHING THE DEVICE READS, decoded from the row.
struct Settings {
	bool enabled = true;
	float selector = 0.0f;
	Keys keys = Keys::Transpose;
	bool latch = false;
	humanize::Feel feel { humanize::Mode::Straight, 0.5, 0, 0.0 };
	/// FREE'S PHASE AS STORED (backlog QA): unbounded; `feel.freePhase` reads it.
	double freePhase = 0.0;
	int offset = 0;
	float nudge = 0.0f;
	/// TRANSPORT BY DEFAULT, now one of three (`arp::RunMode`).
	arp::RunMode run = arp::RunMode::Transport;
	/// THE NOTE RANGE (design §7.12), MIDI notes.
	int noteLow = kNoteLowDefault;
	int noteHigh = kNoteHighDefault;
	bool fixedVelocity = false;
	float velocity = 0.8f;
	/// THE LIKES' WARP: Off by default.
	bool likesWarp = false;
	/// PLAY MODE AND EACH TAB'S CYCLE CELL (design §7.6). Kept out of `PatternSettings`, so a Repeats or Next edit never
	/// rebuilds a cycle.
	PlayMode playMode = PlayMode::Selector;
	std::array<CycleCell, kPatterns> cycle {};
	/// TRANSPOSE (design §7.7): scale steps, then octaves; applied at play time to whatever plays, never to a decode.
	int transposeSteps = 0;
	int transposeOctaves = 0;
	/// EACH PATTERN'S OWN TRANSPOSE (`Field::TransposeSteps`, `Field::TransposeOctaves`), ADDED to the pair above. Kept out of
	/// `PatternSettings`, as the cycle cells are, so a transpose never rebuilds a cycle.
	std::array<std::array<int, 2>, kPatterns> patternTranspose {};
	/// EACH TAB'S SPAN, FILL AND PHASE MARKERS (design §7.8), and its Fill Trigger's reading (edge-detected by the device).
	std::array<TabPhase, kPatterns> phase {};
	std::array<float, kPatterns> fillTrigger {};
	/// EACH TAB'S MODULATION LANES (design §7.10): read at play time, so kept out of `PatternSettings` (no rebuild).
	std::array<ModLanes, kPatterns> modLanes {};
	/// Pattern 1 is enabled by default (its descriptor says so), the others not: a fresh device plays pattern 1.
	std::array<PatternSettings, kPatterns> patterns = [] {
		std::array<PatternSettings, kPatterns> p {};
		p[0].enable = true;
		return p;
	}();

	friend bool operator==(const Settings&, const Settings&) = default;
};

using Row = std::array<float, kParamCount>;
[[nodiscard]] Row rowOf(const Settings& s);
/// A short row reads its missing controls at their declared defaults; anything out of range is clamped home.
[[nodiscard]] Settings settingsOf(const float* fields, size_t fieldsPresent);

/// **THE PATTERNS IN THE SELECTOR'S RANGE** (design §7.3): the soloed ones if any is soloed (a solo counts even on a
/// pattern switched off — it is an explicit ask), else the enabled ones, in tab order. Returns how many, into `out`.
size_t inRange(const Settings& s, std::array<size_t, kPatterns>& out) noexcept;
/// **THE PATTERN THE SELECTOR PICKS**: its range cut into equal bands over the patterns in range; `kPatterns` for none.
[[nodiscard]] size_t selected(const Settings& s) noexcept;
/// **TRANSPOSE ONE KEY** (design §7.7): `steps` scale degrees within `scale` spelled from `rootKey` (a key off the scale — a
/// chromatic pickup — keeps its offset from the degree at or below it), then `octaves` × 12, clamped to a MIDI key. Zero
/// and zero is the key itself.
[[nodiscard]] int transposeKey(const theory::Scale& scale, int rootKey, int key, int steps, int octaves) noexcept;
/// **ARE THE TRANSPOSE CONTROLS SHOWN** (the honesty rule): not when every pattern in range is a Drum — lanes are fixed
/// note numbers, so then they move nothing. `fieldAt` reads the row.
[[nodiscard]] bool transposeLive(const std::function<float(size_t)>& fieldAt) noexcept;

/// **A NOTE FOLDED INTO THE RANGE** (design §7.12): below `low` it moves up by octaves until it is at or above it, above `high`
/// down until it is at or below it. A range narrower than an octave may hold no octave of the note: then it takes the nearer
/// bound. `high` below `low` is read as `low` (Lowest wins). The result is a MIDI note, 0..127.
[[nodiscard]] int foldToRange(int key, int low, int high) noexcept;
/// THE NOTE RANGE SHOWS while a pattern in range plays notes from a pitched Mode — never Drum (fixed kit notes) or
/// Modulation (no notes), the honesty rule, as `transposeLive` reads it.
[[nodiscard]] bool noteRangeLive(const std::function<float(size_t)>& fieldAt) noexcept;

} // namespace feathers::mod::arp::pat
