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
#include <cstdio>
#include <string>

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
	/// FOCUS, 0..1 (default 0): how much of each band's own deviation from the whole's imbalance is corrected, over the global part every band gets alike.
	/// 0 balances the whole and leaves the bands' relative image; 1 brings every band to the centre. APPENDED.
	Focus,
	/// TOLERANCE, dB (default 0, off): a dead zone on the deviation part, so a band within it of the whole's imbalance is not pulled further.
	Tolerance,
	/// WIDEN, 0..1 (default 0): complementary comb bands sent to opposite sides, mono-safe and level-balanced.
	Widen,
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
	float focus = 0.0f;
	float toleranceDb = 0.0f;
	float widen = 0.0f;

	friend bool operator==(const Settings&, const Settings&) = default;
};

using Row = std::array<float, static_cast<size_t>(Param::kCount)>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// WHAT THE METER PUBLISHES, IN THIS ORDER: for each of three bands, the imbalance before the correction (dB, positive = left louder), after it, and the
/// band's level (linear rms of both sides); then the two spacings in Hz as the engine runs them, and the tracked note (0: none).
/// THE GONIOMETER'S POINTS: each of four channels (the three bands' corrected, widened outputs, then the whole INPUT before HP Wet) keeps its newest `kRingPairs` L/R pairs,
/// the newest consecutive samples, published oldest first from index `kPointsBase`. A fixed ring on the audio thread, copied out with the other readings.
inline constexpr size_t kPointChannels = 4;
inline constexpr size_t kRingPairs = 24;
inline constexpr size_t kPointsBase = 17;
inline constexpr size_t kInputChannel = 3;
[[nodiscard]] constexpr size_t pointIndex(size_t channel, size_t pair, size_t side) { return kPointsBase + (channel * kRingPairs + pair) * 2 + side; }

enum class Reading : size_t {
	Before0, After0, Level0,
	Before1, After1, Level1,
	Before2, After2, Level2,
	Spacing1Hz,
	Spacing2Hz,
	NoteHz,
	/// THE CORRELATION OF EACH BAND'S OUTPUT SIDES (after the correction and the Widen), -1..1, and of the whole output (0 for a band with nothing in it).
	Corr0,
	Corr1,
	Corr2,
	CorrAll,
	/// HOW MANY POINT PAIRS THE DEVICE HAS WRITTEN, modulo 65536 (exact in a float): the picture's way to know how many of the ring's newest are new.
	PointSeq,
	kCount = 17 + kPointChannels * kRingPairs * 2,
};
inline constexpr size_t kBandReadings = 3;
static_assert(static_cast<size_t>(Reading::PointSeq) + 1 == kPointsBase);
static_assert(static_cast<size_t>(Reading::kCount) <= 224, "the readings store holds 224 values per device");
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


/// **THE CORRECTION LAW, IN ONE PLACE.** `d` is a band's imbalance and `dTotal` the whole signal's (both ln(rmsL / rmsR), the whole's from the SUMMED band
/// energies), `focus` 0..1, `toleranceNepers` >= 0. The band's deviation from the whole is `d - dTotal`; the tolerance is a dead zone on it (only the excess
/// counts, with its sign); the imbalance the band is corrected by is `dTotal + focus * excess`. Focus 0 gives every band the whole's correction (the bands'
/// relative image is kept), Focus 1 with no tolerance gives `d`: each band to the centre. The gains are then `correctionGains` of that at `strength`.
[[nodiscard]] inline float correctedImbalance(float d, float dTotal, float focus, float toleranceNepers) {
	const float dev = d - dTotal;
	const float excess = std::max(0.0f, std::fabs(dev) - toleranceNepers);
	return dTotal + focus * (dev < 0.0f ? -excess : excess);
}
[[nodiscard]] inline SideGains bandCorrection(float d, float dTotal, float strength, float focus, float toleranceNepers) {
	return correctionGains(correctedImbalance(d, dTotal, focus, toleranceNepers), strength);
}

/// **WIDEN'S AMOUNTS.** The first band of the pair goes left, the second right. Each band's mid is moved between the channels (`L += a m`, `R -= a m` for the
/// left band, the reverse for the right one, m = (L + R) / 2), so L + R is unchanged for ANY input: mono-safe by construction. A centred band then ends at
/// (1 + a, 1 - a). The two amounts are `w` for the weaker band and less for the stronger, `w min(1, E_other / E_this)`, because the sides' energy difference is
/// `4 (a1 E1 - a2 E2)` for orthogonal bands and that is zero exactly then: the widened result stays level from side to side.
struct WidenAmounts {
	float first = 0.0f;
	float second = 0.0f;
};
[[nodiscard]] inline WidenAmounts widenAmounts(float widen, float energyFirst, float energySecond) {
	constexpr float kFloor = 1.0e-12f;
	const float a = energyFirst + kFloor;
	const float b = energySecond + kFloor;
	return { widen * std::min(1.0f, b / a), widen * std::min(1.0f, a / b) };
}

/// **THE BAND NAMES (`owltt::combBandNames`)**: harmonic classes only while a note is tracked and the first spacing is x1 of it; otherwise Gaps / Teeth (A, B).
[[nodiscard]] inline std::array<std::string, 3> bandNames(const Settings& s, bool noteHeard) {
	const bool harmonic = s.keyTrack && noteHeard;
	if (s.bands < 3) {
		const auto names = owltt::combBandNames2(owltt::combRatioOfKnob(s.spacing, false), harmonic);
		return { names[0], names[1], "" };
	}
	return owltt::combBandNames(owltt::combRatioOfKnob(s.spacing, false), owltt::combRatioOfKnob(s.spacing2, true), harmonic);
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
     .tooltip = "How many comb bands are balanced on their own. 2: one comb, Gaps (between its teeth) and Teeth (on them). 3: two combs, Gaps, Teeth A (on the first comb's teeth, off the second's) "
                "and Teeth B (on both). They are harmonic classes (Between, Odd, Even) only while Key Track follows a note at x1"},
    {.key = "balance_spacing", .name = "Balance Spacing", .min = 20.0, .max = 2000.0, .def = 200.0, .stepped = false, .unit = device::Unit::Number,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "The comb's tooth spacing: the bands are classes of teeth, not frequency ranges. In Hz. With Key Track off the comb is fixed in Hz, so its teeth meet an input's harmonics only when the pitch matches: it then works as a broadband balancer or widener",
     .wholeReadout = true, .suffix = "Hz"},
    {.key = "balance_spacing2", .name = "Balance Spacing 2", .min = 200.0, .max = 20000.0, .def = 2000.0, .stepped = false, .unit = device::Unit::Number,
     .names = {}, .modulatable = true, .offsetsMayLeaveRange = false,
     .tooltip = "The second comb's tooth spacing: it cuts the on-teeth band in two. Read in Hz, a fifth of this knob; its default is twice the first",
     .gateControl = kBandsControl, .gateMask = 0b10u, .wholeReadout = true, .suffix = "Hz"},
    {.key = "balance_key_track", .name = "Balance Key Track", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true, .unit = device::Unit::Choice,
     .names = kOffOnNames,
     .tooltip = "The spacings follow the last note played: the knobs read as ratios to it (x1 = the note's own harmonics, x2 = every other one). "
                "Until a note is heard, a plain frequency, and the bands are named by their teeth, not as harmonics"},
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
    {.key = "balance_focus", .name = "Balance Focus", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true,
     .tooltip = "How much each band is corrected on its own. 0% balances the whole signal and keeps the bands' relative image (a band that leans right stays right of the rest); "
                "100% brings every band to the centre. Strength scales the whole correction"},
    {.key = "balance_tolerance", .name = "Balance Tolerance", .min = 0.0, .max = 12.0, .def = 0.0, .stepped = false, .unit = device::Unit::Number,
     .names = {}, .modulatable = true,
     .tooltip = "A dead zone, in dB, on how far a band may lean from the whole before it is corrected on its own: only the excess is pulled in. 0 is off. Works with Focus",
     .suffix = "dB"},
    {.key = "balance_widen", .name = "Balance Widen", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false, .unit = device::Unit::Percent,
     .names = {}, .modulatable = true,
     .tooltip = "Sends complementary comb bands to opposite sides (odd to the left, even to the right; with two bands the first to the left, the second to the right), "
                "so the sound widens without the sum changing: left plus right is exactly what it was. The amounts are weighted by the bands' levels so the two sides stay level. "
                "0 is no change. With Key Track off the comb is fixed in Hz and widens broadband material rather than harmonics"},
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
static_assert(Settings {}.focus == detail::defOf(Param::Focus));
static_assert(Settings {}.toleranceDb == detail::defOf(Param::Tolerance));
static_assert(Settings {}.widen == detail::defOf(Param::Widen));

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
