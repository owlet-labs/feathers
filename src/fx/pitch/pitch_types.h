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
#include "fx/sklitter/sklitter_types.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

//--------------------------------------------------------------------------------------------------//
// THE PITCH SHIFTER.                                                                                 //
//--------------------------------------------------------------------------------------------------//
//
// A pitch shifter with an option to lock to a scale (the same options as SKLITTER, plus Free), with formant
// control and/or a grain size and mode.
//
// **THIS IS THE GRANULAR PASS, AND FORMANT CONTROL IS DELIBERATELY NOT IN IT.** That is a fork in the
// design, and this is the branch taken. Independent formant control needs either the pitch PERIOD
// (pitch-synchronous grains) or a SPECTRUM (an envelope to hold still while the partials move), and
// the repo has neither: the tracker follows MIDI notes rather than audio, and there is no FFT in the
// DSP code or the vendored libs. A granular shifter needs neither, works on chords and on anything
// else, costs almost nothing and has no latency to speak of — and its formants move WITH the pitch.
// `Grain Size` and `Mode` are this pass's character controls; formant is pass two, by one of:
//
//   - a phase vocoder with the spectral envelope held (any source; an FFT to write, 20-40 ms of
//     latency, the phasey smear on transients), or
//   - note-aware pitch-synchronous grains on Feathers' own voices, where `setNoteContext` hands the
//     device the period for free — clean and cheap, and only where a note exists.
//
// **THE ENGINE IS THE CLASSIC TWO-HEAD DELAY-LINE SHIFTER**: two read heads half a cycle apart, each
// sweeping its delay so that it reads the line faster or slower than it was written, each silenced
// by its window at the instant its delay jumps back. What is particular to this one is below, where
// each decision is made.

namespace feathers::fx::pitch {

//--------------------------------------------------------------------------------------------------//
// THE SCALE LOCK.                                                                                    //
//--------------------------------------------------------------------------------------------------//

/// **SKLITTER'S OPTIONS, PLUS FREE — SO THE MENU IS BUILT FROM SKLITTER'S OWN TABLE.** Its
/// `kPitchScaleNames` opens with twelve scales and continues with thirteen fixed shifts (`+1`…`+13`);
/// the twelve are taken and the thirteen are not, because a fixed shift is what this device's Shift
/// knob already IS. Built rather than retyped, for `kSyncNames`' reason: "same options" is then a
/// property of the code rather than of two lists somebody keeps in step.
inline constexpr size_t kSklitterScaleCount = 12;
inline constexpr size_t kScaleChoiceCount = 1 + kSklitterScaleCount;

constexpr std::array<const char*, kScaleChoiceCount> makeScaleNames() {
	std::array<const char*, kScaleChoiceCount> out {};
	out[0] = "Free";
	for (size_t i = 0; i < kSklitterScaleCount; ++i) {
		out[1 + i] = fx::sklitter::kPitchScaleNames[i];
	}
	return out;
}
inline constexpr auto kScaleNames = makeScaleNames();

// THE TWELVE ARE PINNED BY NAME, because the masks below are index-aligned with them and a reorder
// of SKLITTER's table would otherwise hand `Dorian` the blues scale with nothing red anywhere.
static_assert(std::string_view(kScaleNames[1]) == "Chromatic");
static_assert(std::string_view(kScaleNames[2]) == "Major");
static_assert(std::string_view(kScaleNames[3]) == "Minor");
static_assert(std::string_view(kScaleNames[4]) == "MajPent");
static_assert(std::string_view(kScaleNames[5]) == "MinPent");
static_assert(std::string_view(kScaleNames[6]) == "Blues");
static_assert(std::string_view(kScaleNames[7]) == "Dorian");
static_assert(std::string_view(kScaleNames[8]) == "Mixolyd");
static_assert(std::string_view(kScaleNames[9]) == "MajTri");
static_assert(std::string_view(kScaleNames[10]) == "MinTri");
static_assert(std::string_view(kScaleNames[11]) == "Sus4");
static_assert(std::string_view(kScaleNames[12]) == "Dim");

/// A pitch-class set: bit n set means the interval n semitones above the root belongs.
constexpr uint16_t pcs(std::initializer_list<int> degrees) {
	uint16_t mask = 0;
	for (const int d : degrees) {
		mask = static_cast<uint16_t>(mask | (1u << static_cast<unsigned>(d)));
	}
	return mask;
}

/// **FULL SCALE DEGREES, NOT SKLITTER'S TABLES — THE NAMES ARE SHARED AND THE CONTENTS ARE NOT, by
/// decision.** SKLITTER's `kScaleSemitones` are weighted RANDOM picks built to stay consonant:
/// its `Major` is tonic, third, fifth and octave, and its `Chromatic` is 0/3/5/7/12. That is right
/// for a machine drawing a slice's pitch at random, and wrong for a knob a hand turns to choose an
/// interval — most of the scale would be unreachable. So `Major` here is the major scale.
///
/// The three triads and `Dim` are chord sets because that is what their names say; `Dim` is the
/// diminished TRIAD, matching what SKLITTER's own entry contains.
inline constexpr uint16_t kScaleMasks[kScaleChoiceCount] = {
	0x0FFF,                          // Free — never consulted; every class, so a stray read is harmless
	0x0FFF,                          // Chromatic
	pcs({ 0, 2, 4, 5, 7, 9, 11 }),   // Major
	pcs({ 0, 2, 3, 5, 7, 8, 10 }),   // Minor
	pcs({ 0, 2, 4, 7, 9 }),          // MajPent
	pcs({ 0, 3, 5, 7, 10 }),         // MinPent
	pcs({ 0, 3, 5, 6, 7, 10 }),      // Blues
	pcs({ 0, 2, 3, 5, 7, 9, 10 }),   // Dorian
	pcs({ 0, 2, 4, 5, 7, 9, 10 }),   // Mixolyd
	pcs({ 0, 4, 7 }),                // MajTri
	pcs({ 0, 3, 7 }),                // MinTri
	pcs({ 0, 5, 7 }),                // Sus4
	pcs({ 0, 3, 6 }),                // Dim
};

/// **WHAT THE SHIFT KNOB ACTUALLY PLAYS.** `Free` passes the knob through. Anything else snaps it to
/// the nearest interval whose pitch class belongs to the scale, in whichever octave the knob is in —
/// a shift of -5 in `Major` is a fourth DOWN, because -5 is pitch class 7 and the fifth belongs.
///
/// **A TIE GOES TO THE SMALLER SHIFT.** Half-way between two degrees the knob could honestly mean
/// either; the one nearer to "no shift" is the one a hand sweeping up from zero has not reached yet,
/// so it is the one that does not jump ahead of the hand.
[[nodiscard]] inline double snapSemitones(double shift, size_t scale) noexcept {
	if (scale == 0 || scale >= kScaleChoiceCount) {
		return shift;
	}
	const uint16_t mask = kScaleMasks[scale];
	const auto centre = static_cast<int>(std::lround(shift));
	double best = shift;
	double bestDistance = 1.0e9;
	for (int n = centre - 12; n <= centre + 12; ++n) {
		const int pc = ((n % 12) + 12) % 12;
		if ((mask & (1u << static_cast<unsigned>(pc))) == 0) {
			continue;
		}
		const double distance = std::abs(static_cast<double>(n) - shift);
		const bool nearer = distance < bestDistance - 1.0e-9;
		const bool tieButSmaller =
		    std::abs(distance - bestDistance) <= 1.0e-9 && std::abs(n) < std::abs(static_cast<int>(best));
		if (nearer || tieButSmaller) {
			best = static_cast<double>(n);
			bestDistance = distance;
		}
	}
	return best;
}

//--------------------------------------------------------------------------------------------------//
// THE RANGES.                                                                                        //
//--------------------------------------------------------------------------------------------------//

inline constexpr double kShiftSemitonesMin = -24.0;
inline constexpr double kShiftSemitonesMax = 24.0;
inline constexpr double kFineCentsMax = 100.0;

/// GRAIN SIZE, IN MILLISECONDS, and stated in the unit a hand reads rather than as `LogSeconds` —
/// whose minimum prints "off" by this project's convention, and a pitch shifter's grain cannot be
/// off. Short grains track transients and buzz; long grains are smooth and smear.
inline constexpr double kSizeMsMin = 5.0;
inline constexpr double kSizeMsMax = 250.0;
inline constexpr double kSizeMsDefault = 60.0;

/// HOW FAR APART `Width` DETUNES THE TWO CHANNELS AT FULL, in cents — each channel moves half of it,
/// in opposite directions, so the centre of the image stays on the pitch the controls name.
inline constexpr double kWidthCentsMax = 50.0;

/// **THE ONE-KNOB TRIP: THE SHIFTED OUTPUT, BACK INTO THE LINE.** Every pass round the loop is
/// shifted again, so a fifth becomes a ninth becomes a thirteenth — the staircase that "shimmer" and
/// "crystals" effects are built on, and with `Reverse` the steps play backwards.
///
/// **BELOW ONE, AND THE LOOP CANNOT GROW.** The two heads' shares sum to exactly one, so a pass
/// through the shifter is at most unity and the loop gain is at most this. `softClip` stands behind
/// it anyway, because two heads landing on correlated material can briefly sum to more than either.
///
/// **WHILE IT IS UP, EVERY GRAIN IS SEATED ONE GRAIN FURTHER BACK.** A head at zero delay feeding
/// the line it reads is not an echo, it is a one-pole lowpass with a DC gain of `1 / (1 - feedback)`
/// — twenty times, at this maximum. One grain of delay in the loop is what makes it a staircase
/// instead, and is why Size is the step time. At zero Feedback the seat is where it always was, so
/// a shift of zero is still the input exactly.
inline constexpr double kFeedbackMax = 0.95;

/// BELOW THIS THE LOOP IS FLUSHED TO EXACT ZERO — the delay's floor, for the delay's reason: a
/// feedback loop otherwise decays INTO the denormals and parks there, which is the defect
/// `test_fx_devices.cpp`'s silence sweep was written to find.
inline constexpr float kSilenceFloor = 1.0e-7f;

/// **HOW MUCH LINE THE HEADS CAN NEED.** A reversed grain is the worst case: its head falls back
/// through the line at the pitch ratio WHILE the line advances, so it sweeps `(1 + ratio)` grain
/// lengths of delay — five of them at +24 semitones — plus a grain of jitter and a grain of
/// Feedback's loop delay on top. Seven grains of 250 ms is 1.75 s.
inline constexpr double kLineSeconds = 2.0;

//--------------------------------------------------------------------------------------------------//
// THE CONTROLS.                                                                                      //
//--------------------------------------------------------------------------------------------------//

/// ORDER IS THE STREAM ORDER AND THE DESCRIPTOR ORDER AND THE ROW ORDER, all three.
enum class Param : int {
	Enabled, ///< bypass switch
	Scale,   ///< Free, or which scale the Shift knob snaps to
	Shift,   ///< the interval, in semitones
	Fine,    ///< cents on top of it — applied AFTER the snap, so a locked shift can still be detuned
	Mix,     ///< dry against wet
	Mode,    ///< how a grain is cut: Smooth, Tight, or Reverse
	Size,    ///< grain length, in milliseconds
	Jitter,  ///< how far each grain's start is thrown, which breaks up the comb a fixed grain rings at
	Width,    ///< detunes the two channels apart
	Feedback, ///< the shifted output back into the line, so every repeat is shifted AGAIN

	kCount,
};

inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// **HOW A GRAIN IS CUT.** Three, so it is a picker (`device::isOffOnChoice` — any named choice is).
///
///   Smooth   long raised-cosine fades, half a grain each — the least amplitude ripple, the most smear
///   Tight    short fades and a flat top — transients survive better, the splice is more audible
///   Reverse  each grain plays BACKWARDS at the shifted pitch
enum class Mode : int { Smooth, Tight, Reverse, kCount };
inline constexpr const char* kModeNames[] = { "Smooth", "Tight", "Reverse" };
static_assert(std::size(kModeNames) == static_cast<size_t>(Mode::kCount));

/// The fraction of a head's cycle spent fading, per mode. Half is the most a two-head scheme allows
/// (each head is up for half the cycle plus one fade). The fades are raised-cosine and complementary,
/// so the two heads sum to exactly one at every instant whatever this is — asserted with a DC input.
inline constexpr double kSmoothFade = 0.5;
inline constexpr double kTightFade = 0.12;

/// `Reverse` fades like `Smooth`: a reversed grain's edges are its loudest surprise, and the long
/// fade is what keeps them from reading as clicks.
[[nodiscard]] constexpr double fadeFor(Mode mode) noexcept {
	return mode == Mode::Tight ? kTightFade : kSmoothFade;
}

/// **HEAD 0'S SHARE OF THE OUTPUT AT THIS PHASE; HEAD 1 HAS THE REST.** One number rather than two
/// windows, so the pair sums to one by construction instead of by two cosines agreeing in float.
///
///   [0, f)          head 0 fades in      (head 1, half a cycle on, is fading out)
///   [f, 0.5)        head 0 alone
///   [0.5, 0.5 + f)  head 0 fades out     (head 1 has just wrapped and is fading in)
///   [0.5 + f, 1)    head 1 alone
///
/// A head is re-seated at ITS wrap — phase 0 for head 0, phase 0.5 for head 1 — and this is zero for
/// head 0 at the first and one (so zero for head 1) at the second. HERE RATHER THAN IN THE `.cpp` so
/// that those two facts are assertions on a function instead of a reading of a loop.
[[nodiscard]] inline double headZeroShare(double phase, double fade) noexcept {
	const auto rise = [](double u) { return 0.5 - 0.5 * std::cos(3.14159265358979323846 * u); };
	if (phase < fade) {
		return rise(phase / fade);
	}
	if (phase < 0.5) {
		return 1.0;
	}
	if (phase < 0.5 + fade) {
		return 1.0 - rise((phase - 0.5) / fade);
	}
	return 0.0;
}

inline constexpr const char* kTags[] = { "pitch", "fx" };

/// **THE FACE: TWO ROWS OF FOUR, NINE CONTROLS.** Added mid-build: a mix knob, a width, and anything else
/// that makes a trippy character from about one knob — which is Width and Feedback, and with them the card would have gone to a third row for one control. `Mode` sits
/// UNDER `Scale` instead, the delay's Mode-over-Dub idiom: two pickers are each far shorter than the
/// knob their cell is sized for.
///
///   Scale/Mode   Shift   Fine    Mix
///   Size         Jitter  Width   Feedback
///
/// NO SECTION HEADINGS. The delay's headings name a MODE's four controls; this second row is two
/// grain controls and two that are not, and a "GRAIN" over all four would be a label that is half
/// wrong.
inline constexpr uint8_t kFaceOrder[] = {
	static_cast<uint8_t>(Param::Scale),  static_cast<uint8_t>(Param::Mode),
	static_cast<uint8_t>(Param::Shift),  static_cast<uint8_t>(Param::Fine),
	static_cast<uint8_t>(Param::Mix),    static_cast<uint8_t>(Param::Size),
	static_cast<uint8_t>(Param::Jitter), static_cast<uint8_t>(Param::Width),
	static_cast<uint8_t>(Param::Feedback),
};

inline constexpr fx::TenantTraits kTraits {
	.tag = "PITCH",
	.name = "Pitch",
	.hasCard = true,
	.hasExpanded = false,
	// ONE SHIFTER ON THE BUS. The note-aware per-voice road is pass two's question, not this one's.
	.perVoiceControl = -1,
	.extraState = 0,
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = -1,
	.faceControls = 0,
	.faceOrder = kFaceOrder,
	.faceColumns = 4,
	.tags = kTags,
	// ITS MIX IS THE SLOT'S (backlog NR, D1): a plain crossfade, so the engine blends it around the device and its
	// site, and the device hands back the wet. The key is unchanged: the number means what it meant.
	.mixControl = static_cast<int>(Param::Mix),
};

/// EVERY ROW BY FIELD NAME — device.h records the positional-init hazard twice over.
inline constexpr fx::ParamDesc kParamDescs[] = {
    {.key = "pitch_enabled", .name = "Pitch Enabled",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kOffOnNames,
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Bypass switch — off removes the pitch shifter from the chain"},
    {.key = "pitch_scale", .name = "Pitch Scale",
     .min = 0.0,
     .max = static_cast<double>(kScaleChoiceCount) - 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kScaleNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "Free leaves Shift unquantised. Any scale snaps it to the nearest interval that "
                "belongs — so a sweep, or a modulator, plays the scale instead of sliding through it"},
    {.key = "pitch_shift", .name = "Pitch Shift",
     .min = kShiftSemitonesMin,
     .max = kShiftSemitonesMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "The interval, in semitones. With a Scale chosen it snaps to the nearest degree, in "
                "whichever octave the knob is in",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true},
    {.key = "pitch_fine", .name = "Pitch Fine",
     .min = -kFineCentsMax,
     .max = kFineCentsMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Cents on top of the shift. Applied after the scale snap, so a locked interval can "
                "still be detuned",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = true},
    {.key = "pitch_mix", .name = "Pitch Mix",
     .min = 0.0,
     .max = 1.0,
     .def = 1.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Dry against shifted. Halfway is the original plus a harmony"},
    {.key = "pitch_mode", .name = "Pitch Mode",
     .min = 0.0,
     .max = static_cast<double>(Mode::kCount) - 1.0,
     .def = 0.0,
     .stepped = true,
     .unit = fx::ParamUnit::Choice,
     .names = kModeNames,
     .modulatable = false,
     .offsetsMayLeaveRange = false,
     .tooltip = "How a grain is cut. Smooth fades long and smears; Tight fades short, so transients "
                "survive and the splice shows; Reverse plays each grain backwards at the shifted pitch",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     .shortName = nullptr,
     // SHARES `Scale`'S CELL, SITTING UNDER IT — see `kFaceOrder`.
     .stacked = true},
    {.key = "pitch_size", .name = "Pitch Size",
     .min = kSizeMsMin,
     .max = kSizeMsMax,
     .def = kSizeMsDefault,
     .stepped = false,
     .unit = fx::ParamUnit::Number,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Grain length in milliseconds. Short tracks transients and buzzes; long is smooth "
                "and smears",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     // THE UNIT IS ON THE READOUT (`ParamDesc::suffix`), where it used to be in the caption ("Size ms" over a bare "60.00").
     .shortName = "Size",
     .wholeReadout = true,
     .suffix = "ms"},
    {.key = "pitch_jitter", .name = "Pitch Jitter",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Throws each grain's start by a random amount, which breaks up the metallic ring a "
                "fixed grain length has. At zero, a shift of zero is exactly the input"},
    {.key = "pitch_width", .name = "Pitch Width",
     .min = 0.0,
     .max = 1.0,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Detunes the two channels apart, half each way, so the image widens around the pitch "
                "the controls name"},
    {.key = "pitch_feedback", .name = "Pitch Feedback",
     .min = 0.0,
     .max = kFeedbackMax,
     .def = 0.0,
     .stepped = false,
     .unit = fx::ParamUnit::Percent,
     .names = {},
     .modulatable = true,
     .offsetsMayLeaveRange = false,
     .tooltip = "Sends the shifted sound back in, so every repeat is shifted again — a staircase "
                "climbing or falling by the interval, one step per grain. Size sets the step time",
     .zones = 0,
     .modSpan = 0.0,
     .zoneNames = {},
     .gateControl = -1,
     .gateMask = 0,
     .centred = false,
     // "FEEDBACK" IS EIGHT LETTERS AND THE CELL HOLDS SEVEN — photographed clipped to "FEEDBACI".
     // `Regen` is the pitch-shifter trade's own word for exactly this control.
     .shortName = "Regen"},
};
static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

//--------------------------------------------------------------------------------------------------//
// WHAT A PATCH STORES.                                                                               //
//--------------------------------------------------------------------------------------------------//

struct Settings {
	bool enabled = true;
	size_t scale = 0;
	float shiftSemitones = 0.0f;
	float fineCents = 0.0f;
	float mix = 1.0f;
	Mode mode = Mode::Smooth;
	float sizeMs = static_cast<float>(kSizeMsDefault);
	float jitter = 0.0f;
	float width = 0.0f;
	float feedback = 0.0f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

/// As the delay's: nothing but `Settings`, kept because the codec vocabulary is stated in terms of it.
struct State {
	Settings settings {};

	friend bool operator==(const State&, const State&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Row rowOf(const State& state);
State stateOf(const Row& row, size_t fieldsPresent);
float valueOf(const State& state, Param param);
void setValueOf(State* state, Param param, float value);

/// **THE RATIO EACH CHANNEL IS SHIFTED BY — the one place Scale, Shift, Fine and Width meet.**
/// A pure function, so the claim "Shift 8 in Major plays a fifth" is an equality on a number rather
/// than something inferred from a rendered buffer. `right` false is the left channel.
[[nodiscard]] double ratioFor(const Settings& s, bool right) noexcept;

extern const device::AudioOps kAudioOps;

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.category = device::Category::Pitch,
};

} // namespace feathers::fx::pitch
