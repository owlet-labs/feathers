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
#include "fx/owltt/comb_ratio.h"
#include "fx/wet_highpass.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// STEREO BALANCE — a corrective tool: it splits the signal into harmonic classes with OWLTT's comb split (`fx/owltt/comb_split.h`), measures the
// integrated left/right level of each class, and pulls each class's imbalance towards the centre, with an Attack and Release on the measurement and a
// Strength on the correction. The bands sum back, so the stereo field the bands do NOT disagree about is left alone: a left-heavy bass note and a
// right-heavy overtone are each corrected on their own.
//
// **THE LAW.** Per band, the detector keeps the mean square of each side over a 30 ms window, then smooths that with Attack (rising) and Release (falling); the imbalance is
// `d = ln(rmsL / rmsR)`, in nepers of amplitude. With the Strength S in 0..1 the gains are
//
//     gL = exp(-S d / 2),   gR = exp(+S d / 2),   so  gL gR = 1.
//
// gL gR = 1 is what keeps the band's loudness where it was: the geometric mean of its two sides' levels does not move, only their difference
// shrinks, to `(1 - S) d`. At S = 1 the two sides end at one level, at S = 1/2 half-way (in dB), at S = 0 the gains are exactly 1. It is the law of a
// side-weighted band (one that is louder on one side); for a band whose two sides are correlated the power sum moves a little, which is the price
// of a gain-only corrector that cannot invent or remove a width.
//
// **THEN THE PAN** (`panGains`): a balance control after the correction, equal power, `gL = sqrt(2) cos(pi (1 + p) / 4)`, `gR = sqrt(2) sin(pi (1 + p) / 4)`,
// so gL^2 + gR^2 = 2 at every position and p = 0 is exactly 1, 1 (no change). It places the corrected whole, lows included.
//
// **SIGNAL ORDER**: HP Wet / Sub Thru first (the lows bypass everything: no detector hears them), then the comb split, then each band's detector
// and gains, then the sum, then the lows added back, then the Pan.
//
// **RESONANCE IS NOT OFFERED.** A feedback comb rings: it puts the band's energy where the input is not, and a corrector that colours the band it measures
// is no longer measuring what it corrects. The split here is the feedforward comb (and Comb Sharp, which is feedforward too), so the tail is finite and
// exact: after the longest delay of zeros in, zeros out.
//
// **NOTHING IS PERSISTED BUT THE CONTROLS**: the detectors' levels are derived from the audio and rebuild in a release time; nothing random is held.

namespace feathers::fx::balance {

enum class Param : int {
	Enabled,
	/// 2 OR 3 BANDS: one comb split at Spacing (between / on the teeth), or OWLTT's tree of two splits (between / odd / even at the default ratios).
	Bands,
	/// THE FIRST COMB'S TOOTH SPACING, OWLTT's Low Crossover knob: Hz, or with Key Track a ratio to the played note (knob / 200, x1 at 200).
	Spacing,
	/// THE SECOND SPLIT'S, OWLTT's High Crossover knob (a fifth of it in Hz, x1 at 1000; the default 2000 is x2): three bands only.
	Spacing2,
	KeyTrack,
	/// COMB SHARP: each comb run twice, narrower teeth and flat nulls.
	Sharp,
	/// 0..1: how much of each band's imbalance is taken out.
	Strength,
	/// log2(seconds): how fast the measurement follows a side getting louder.
	Attack,
	/// log2(seconds): how fast it follows a side getting quieter.
	Release,
	/// -1 (left) .. 1 (right): the equal-power balance placed after the correction. 0 is untouched.
	Pan,
	/// SHARED WET HIGHPASS (`fx/wet_highpass.h`): log2 Hz, the bottom is no filter. Upstream of the split and of the detectors.
	HpWet,
	SubThru,
	/// THE SLOT'S TWO CONTROLS (backlog NR): applied by the slot, never read by the device.
	Mix,
	WetGain,
	kCount,
};

inline constexpr const char* kBandsNames[] = { "2 bands", "3 bands" };
inline constexpr const char* kOffOnNames[] = { "Off", "On" };

/// THE DETECTOR'S RANGES, log2 seconds.
inline constexpr double kAttackMinLog2 = -6.643856189774724;  // 10 ms
inline constexpr double kAttackMaxLog2 = 2.321928094887362;   // 5 s
inline constexpr double kAttackDefLog2 = -1.321928094887362;  // 0.4 s
inline constexpr double kReleaseMinLog2 = -4.321928094887362; // 50 ms
inline constexpr double kReleaseMaxLog2 = 3.321928094887362;  // 10 s
inline constexpr double kReleaseDefLog2 = 0.584962500721156;  // 1.5 s

struct Settings {
	bool enabled = true;
	/// 2 or 3.
	int bands = 3;
	float spacing = 200.0f;
	float spacing2 = 2000.0f;
	bool keyTrack = false;
	bool sharp = false;
	float strength = 0.5f;
	float attack = static_cast<float>(kAttackDefLog2);
	float release = static_cast<float>(kReleaseDefLog2);
	float pan = 0.0f;
	float hpWet = static_cast<float>(fx::kWetHighpassLog2Min);
	bool subThru = false;

	friend bool operator==(const Settings&, const Settings&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// WHAT THE METER PUBLISHES, IN THIS ORDER: for each of three bands, the imbalance before the correction (dB, positive = left louder), after it, and the
/// band's level (linear rms of both sides); then the two spacings in Hz as the engine runs them, and the tracked note (0: none).
enum class Reading : size_t {
	Before0, After0, Level0,
	Before1, After1, Level1,
	Before2, After2, Level2,
	Spacing1Hz,
	Spacing2Hz,
	NoteHz,
	kCount,
};
inline constexpr size_t kBandReadings = 3;
[[nodiscard]] constexpr Reading bandReading(size_t band, size_t what) { return static_cast<Reading>(band * kBandReadings + what); }

/// THE CORRECTION'S GAINS for an imbalance `d` (nepers, ln(rmsL / rmsR)) and a strength: see the law above.
struct SideGains {
	float left = 1.0f;
	float right = 1.0f;
};
[[nodiscard]] inline SideGains correctionGains(float d, float strength) {
	const float h = 0.5f * strength * d;
	return { std::exp(-h), std::exp(h) };
}

/// THE EQUAL-POWER BALANCE: p in -1..1. Exactly 1, 1 at 0.
[[nodiscard]] inline SideGains panGains(float p) {
	if (p == 0.0f) {
		return {};
	}
	constexpr float kQuarterPi = 0.7853981633974483f;
	constexpr float kSqrt2 = 1.4142135623730951f;
	const float a = kQuarterPi * (1.0f + std::clamp(p, -1.0f, 1.0f));
	return { kSqrt2 * std::cos(a), kSqrt2 * std::sin(a) };
}

inline constexpr const char* kTags[] = { "dynamics", "fx" };

inline constexpr device::Traits kTraits {
	.tag = "balance",
	.name = "Stereo Balance",
	.hasCard = true,
	.hasExpanded = true,
	.perVoiceControl = -1,
	.enableControl = static_cast<int>(Param::Enabled),
	.tags = kTags,
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

inline constexpr int16_t kBandsControl = static_cast<int16_t>(Param::Bands);

inline constexpr device::ParamDesc kParamDescs[] = {
    {.key = "balance_enabled", .name = "Balance Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the balancer is in the chain"},
    {.key = "balance_bands", .name = "Balance Bands", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kBandsNames,
     .tooltip = "How many comb bands are balanced on their own. 2: one comb, what falls between its teeth and what sits on them. 3: two combs, "
                "between, odd and even at the default spacings"},
    {.key = "balance_spacing", .name = "Balance Spacing", .min = 20.0, .max = 2000.0, .def = 200.0, .stepped = false, .unit = device::Unit::Number,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "The comb's tooth spacing: the bands are harmonic classes of it, not frequency ranges. In Hz",
     .wholeReadout = true, .suffix = "Hz"},
    {.key = "balance_spacing2", .name = "Balance Spacing 2", .min = 200.0, .max = 20000.0, .def = 2000.0, .stepped = false, .unit = device::Unit::Number,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "The second comb's tooth spacing: it cuts the on-teeth band in two. Read in Hz, a fifth of this knob; its default is twice the first",
     .gateControl = kBandsControl, .gateMask = 0b10u, .wholeReadout = true, .suffix = "Hz"},
    {.key = "balance_key_track", .name = "Balance Key Track", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kOffOnNames,
     .tooltip = "The spacings follow the last note played: the knobs read as ratios to it (x1 = the note's own harmonics, x2 = every other one). "
                "Until a note is heard, a plain frequency"},
    {.key = "balance_sharp", .name = "Balance Comb Sharp", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kOffOnNames,
     .tooltip = "Each comb run twice: narrower teeth and flat nulls, so the bands keep to their harmonics more tightly. The bands still sum to the input"},
    {.key = "balance_strength", .name = "Balance Strength", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true,
     .tooltip = "How much of each band's left/right imbalance is taken out: 100% brings both sides to one level, 50% half-way (in dB), 0% leaves the bands as they are. "
                "The gains are a pair that multiply to one, so a band's overall level does not move"},
    {.key = "balance_attack", .name = "Balance Attack", .min = kAttackMinLog2, .max = kAttackMaxLog2, .def = kAttackDefLog2, .stepped = false,
     .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "How fast the measurement follows a side getting louder. Longer reads a steadier, more integrated balance"},
    {.key = "balance_release", .name = "Balance Release", .min = kReleaseMinLog2, .max = kReleaseMaxLog2, .def = kReleaseDefLog2, .stepped = false,
     .unit = device::Unit::LogSeconds, .names = {}, .modulatable = true,
     .tooltip = "How fast the measurement follows a side getting quieter, and so how long a correction is held once the sound that asked for it has gone"},
    {.key = "balance_pan", .name = "Balance Pan", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true,
     .tooltip = "Places the corrected whole, lows included: an equal-power balance, so the two sides' powers always add to the same total. "
                "Centre is untouched, at any Strength"},
    {.key = "balance_hp_wet", .name = "Balance HP Wet", .min = fx::kWetHighpassLog2Min, .max = fx::kWetHighpassLog2Max, .def = fx::kWetHighpassLog2Min,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "A highpass in front of the comb split and the detectors: what sits below it is never measured and never corrected. "
                "With Sub Thru on, it passes untouched; with it off, it is taken out. At the bottom there is no filter",
     .shortName = "HP Wet"},
    {.key = "balance_sub_thru", .name = "Balance Sub Thru", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kOffOnNames, .modulatable = false,
     .tooltip = "Gives back what HP Wet took out: the lows pass untouched, past the comb and the detectors, at the same level and phase as the dry"},
    {.key = "balance_mix", .name = "Balance Mix", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true, .tooltip = "Dry against balanced, around the whole device"},
    {.key = "balance_wet_gain", .name = "Balance Wet Gain", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false, .unit = device::Unit::Fader,
     .names = {}, .modulatable = true,
     .tooltip = "Level of the balanced signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched"},
};

static_assert(std::size(kParamDescs) == static_cast<size_t>(Param::kCount));

/// EVERY DEFAULT PINNED TO THE DESCRIPTOR.
namespace detail {
[[nodiscard]] constexpr float defOf(Param p) {
	return static_cast<float>(kParamDescs[static_cast<size_t>(p)].def);
}
} // namespace detail
static_assert(Settings {}.enabled == (detail::defOf(Param::Enabled) >= 0.5f));
static_assert(Settings {}.bands == 2 + static_cast<int>(detail::defOf(Param::Bands)));
static_assert(Settings {}.spacing == detail::defOf(Param::Spacing));
static_assert(Settings {}.spacing2 == detail::defOf(Param::Spacing2));
static_assert(Settings {}.strength == detail::defOf(Param::Strength));
static_assert(Settings {}.attack == detail::defOf(Param::Attack));
static_assert(Settings {}.release == detail::defOf(Param::Release));
static_assert(Settings {}.pan == detail::defOf(Param::Pan));
static_assert(Settings {}.hpWet == detail::defOf(Param::HpWet));

extern const device::AudioOps kAudioOps;
/// WHILE A NOTE IS TRACKED the two Spacing knobs print as ratios to it ("x2"), and read one back.
extern const device::TextOps kTextOps;

/// TEST ONLY — the detector's imbalance (nepers, ln(rmsL / rmsR)) and the applied gains of a band, as of the last block; and the engine's own counters.
[[nodiscard]] float imbalanceForTest(const void* storage, size_t band);
[[nodiscard]] SideGains gainsForTest(const void* storage, size_t band);
[[nodiscard]] float strengthNowForTest(const void* storage);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.text = &kTextOps,
	.category = device::Category::Dynamics,
};

} // namespace feathers::fx::balance
