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

// EQ — the surgical / mastering parametric EQ. Step 2:
// the minimum-phase device; step 3: its card (`plugin/ui/eq_card.h`) and panel (`eq_expanded.h`). The design
// library it runs is `eq_design.h`.
//
// WHAT IS NOT HERE YET, BY THE PLAN AND BY THE HONESTY RULE:
//   - PHASE (Minimum / Linear) arrives with linear phase itself (step 7). A switch whose second position
//     changed nothing would be a control that lies (assumption A3).
//   - The analyser (step 4) and Mid/Side (step 6) read fields of the extra-state block below.

#include "device/device.h"
#include "device/registry.h"
#include "dsp/analyser_bins.h"
#include "fx/eq/eq_design.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace feathers::fx::eq {

inline constexpr int kBandCount = 8;

/// THE GLOBALS, then eight bands of `kBandFields` each. The layout is the DESCRIPTOR's; a patch stores
/// every value by its KEY (JF), so the order is free to change.
enum class Param : int {
	Enabled,
	Offset,
	Amount,
	Tilt,
	TiltPivot,
	Output,
	Phase, ///< Minimum / Linear (step 7). NOT modulatable: it sets the device's latency (JM's L1).
	KeyTrack, ///< Off / On (backlog MQ): every band's Hz times f0 / C3, the played note's.
	/// THE CHAIN FURNITURE'S TWO CONTROLS (backlog NR), supplied and applied by the slot: the device
	/// never reads either, and the card does not draw them (the site's head row does). Mix is a crossfade (100% the
	/// wet), the wet Gain a fader position (unity at 0.5) on the wet only. Both defaults are the EQ as it was, to the bit.
	Mix,
	WetGain,
	kGlobalCount,
};

enum class BandField : int { On, Type, Freq, Gain, Q, Slope, kCount };

inline constexpr size_t kGlobalCount = static_cast<size_t>(Param::kGlobalCount);
inline constexpr size_t kBandFields = static_cast<size_t>(BandField::kCount);
inline constexpr size_t kParamCount = kGlobalCount + kBandCount * kBandFields;

[[nodiscard]] constexpr size_t paramIndex(Param p) noexcept {
	return static_cast<size_t>(p);
}
[[nodiscard]] constexpr size_t bandIndex(int band, BandField f) noexcept {
	return kGlobalCount + static_cast<size_t>(band) * kBandFields + static_cast<size_t>(f);
}

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kPhaseNames[] = { "Minimum", "Linear" };
inline constexpr const char* kTypeNames[] = { "Bell", "Low Shelf", "High Shelf", "Low Cut", "High Cut", "Notch" };
static_assert(std::size(kTypeNames) == static_cast<size_t>(BandType::kCount));
inline constexpr const char* kSlopeNames[] = { "6 dB", "12 dB", "18 dB", "24 dB", "36 dB", "48 dB", "72 dB", "96 dB" };
static_assert(std::size(kSlopeNames) == static_cast<size_t>(kSlopeCount));

[[nodiscard]] constexpr uint32_t typeBit(BandType t) noexcept {
	return 1u << static_cast<uint32_t>(t);
}
/// Gain means something only to a band that HAS a gain; Slope only to a cut (the honesty rule, through
/// `ParamDesc::gateControl`).
inline constexpr uint32_t kGainTypes =
    typeBit(BandType::Bell) | typeBit(BandType::LowShelf) | typeBit(BandType::HighShelf);
inline constexpr uint32_t kCutTypes = typeBit(BandType::LowCut) | typeBit(BandType::HighCut);

// Ranges. Frequencies are log2(Hz) (`Unit::LogHertz`), Q is log2(Q) — a Q knob is even in octaves of
// Q, which puts 0.71 and 1 and 2 where a hand can find them.
inline constexpr double kFreqMin = 4.321928094887363;   // log2(20)
inline constexpr double kFreqMax = 14.287712379549449;  // log2(20000)
inline constexpr double kQMin = -3.321928094887362;     // log2(0.1)
inline constexpr double kQMax = 4.169925001442312;      // log2(18)
inline constexpr double kGainRangeDb = 24.0;
inline constexpr double kOffsetRangeOctaves = 10.0;
/// KEY TRACK'S REFERENCE (backlog MQ): C3, MIDI 60 (the house naming, Spectral's `kFixedF0Hz`). A band stored at
/// f plays at f × f0 / C3, so a note an octave up doubles it; with no note heard the device stays at C3, no shift.
inline constexpr double kKeyTrackRefSemitones = 60.0;
[[nodiscard]] constexpr double keyTrackOctaves(double pitchSemitones) noexcept {
	return (pitchSemitones - kKeyTrackRefSemitones) / 12.0;
}
inline constexpr double kAmountRange = 2.0; // ±200% (R9)
inline constexpr double kTiltRangeDbPerOctave = 6.0;
inline constexpr double kPivotMin = 6.643856189774725;  // log2(100)
inline constexpr double kPivotMax = 13.287712379549451; // log2(10000)
inline constexpr double kPivotDef = 9.965784284662087;  // log2(1000)
inline constexpr double kOutputRangeDb = 24.0;

/// THE DEFAULT LAYOUT (the doc's §5.2). Every bell and shelf sits at 0 dB, so a freshly inserted EQ is
/// EXACTLY identity (a 0 dB band is bypassed, not filtered); the two cuts are OFF, because a cut that is
/// on changes the sound.
struct BandDefault {
	BandType type;
	bool on;
	double freqLog2;
	double qLog2;
};
inline constexpr BandDefault kBandDefaults[kBandCount] = {
	{ BandType::LowCut, false, 4.906890595608519, -0.5 },   // 30 Hz
	{ BandType::LowShelf, true, 6.643856189774725, -0.5 },  // 100 Hz
	{ BandType::Bell, true, 7.965784284662087, 0.0 },       // 250 Hz
	{ BandType::Bell, true, 9.643856189774725, 0.0 },       // 800 Hz
	{ BandType::Bell, true, 11.287712379549449, 0.0 },      // 2.5 kHz
	{ BandType::Bell, true, 12.550746785383243, 0.0 },      // 6 kHz
	{ BandType::HighShelf, true, 13.287712379549451, -0.5 }, // 10 kHz
	{ BandType::HighCut, false, 14.135709286104401, -0.5 }, // 18 kHz
};
/// 24 dB/oct, the index of "24 dB" in `kSlopeNames`.
inline constexpr int kDefaultSlope = 3;

/// The device's settings as the DSP reads them: the params decoded, in the params' own units (log2 Hz,
/// log2 Q, dB) and in float, so a row round-trips exactly.
struct Settings {
	bool enabled = true;
	float offsetOctaves = 0.0f;
	float amount = 1.0f;
	float tiltDbPerOctave = 0.0f;
	float tiltPivotLog2 = static_cast<float>(kPivotDef);
	float outputDb = 0.0f;
	bool linearPhase = false;
	bool keyTrack = false;
	struct BandParams {
		BandType type = BandType::Bell;
		bool on = true;
		float freqLog2 = static_cast<float>(kPivotDef);
		float gainDb = 0.0f;
		float qLog2 = 0.0f;
		int slopeIndex = kDefaultSlope;
		friend bool operator==(const BandParams&, const BandParams&) = default;
	};
	std::array<BandParams, kBandCount> bands = [] {
		std::array<BandParams, kBandCount> b {};
		for (int i = 0; i < kBandCount; ++i) {
			b[static_cast<size_t>(i)].type = kBandDefaults[i].type;
			b[static_cast<size_t>(i)].on = kBandDefaults[i].on;
			b[static_cast<size_t>(i)].freqLog2 = static_cast<float>(kBandDefaults[i].freqLog2);
			b[static_cast<size_t>(i)].qLog2 = static_cast<float>(kBandDefaults[i].qLog2);
		}
		return b;
	}();
	friend bool operator==(const Settings&, const Settings&) = default;
};

using Row = std::array<float, kParamCount>;

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// LINEAR PHASE'S SIZES (doc §3.3, R3): an FIR of N = 16384 taps at 44.1/48 kHz, scaled with the rate so the Hz
/// resolution and the latency in milliseconds hold, run as a uniform partitioned convolution in blocks of
/// B = N/16. The latency is N/2 + B: 9216 samples at 48 kHz (0.192 s), 18432 at 96 kHz, under JM's one second
/// at every rate.
[[nodiscard]] constexpr size_t linearFirLength(double sampleRate) noexcept {
	return sampleRate > 100000.0 ? 65536 : sampleRate > 50000.0 ? 32768 : 16384;
}
[[nodiscard]] constexpr size_t linearBlockLength(double sampleRate) noexcept {
	return linearFirLength(sampleRate) / 16;
}
[[nodiscard]] constexpr uint32_t linearLatency(double sampleRate) noexcept {
	return static_cast<uint32_t>(linearFirLength(sampleRate) / 2 + linearBlockLength(sampleRate));
}

/// THE ANALYSER'S LOG BINS (doc §4.1): 96 from 20 Hz to 20 kHz, ~9.6 an octave, each a power DENSITY
/// (the mean over the FFT bins it covers, or the interpolated power at its centre where it is narrower
/// than one), so pink noise reads -3 dB/oct as on any per-bin analyser.
/// LIFTED to `dsp/analyser_bins.h` (rack stage 2, §7a) so the Rack's strip reads the same bins; named here as before.
using dsp::kAnalyserBins;
using dsp::kAnalyserLowHz;
using dsp::kAnalyserHighHz;
using dsp::kAnalyserFloorDb;
using dsp::analyserBinHz;

/// WHAT THE DEVICE PUBLISHES FOR ITS CARD, in this order:
///   - the post-trim output peak (R14's meter), held so a 60 Hz reader cannot miss a transient;
///   - THE ANALYSER FRAME: a counter, the VIEW it was computed in (`Channel`, step 6), 96 PRE bins, 96 POST
///     bins (dB, power), and the counter again. The readings store has no seqlock, so a reader that finds
///     the two counters different has read a frame half-written and drops it (doc §4.1);
///   - KEY TRACK'S SHIFT (backlog MQ): the octaves every band is moved by for the played note, log2(f0 / C3), and 0
///     with Key Track off — the plot draws the heard curve with the device's own number.
///
/// ONE VIEW PER FRAME, NOT ALL THREE (assumption A20): the device computes only the view the extra-state
/// block asks for, so the readings stay at 197 and no shared cap moves. A reader drops a frame whose view is
/// not the one it is showing — at most one hop (~43 ms) after the view changes.
enum class Reading : size_t {
	OutPeak = 0,
	FrameA = 1,
	View = 2,
	PreFirst = 3,
	PostFirst = 3 + kAnalyserBins,
	FrameB = 3 + 2 * kAnalyserBins,
	KeyOctaves = 4 + 2 * kAnalyserBins,
	kCount,
};
inline constexpr size_t kReadingCount = static_cast<size_t>(Reading::kCount);

/// THE EXTRA-STATE BLOCK (doc §5.5, R1 + R4): per-instance state saved with the patch that is NOT a
/// param — no host automation, no modulation slot. One key, `eq_state`, over the whole run; positions
/// within it are this device's contract, new fields APPEND.
///
/// EVERY FIELD'S ZERO IS ITS DEFAULT, because a block nobody has written reads as zeros. So the encodings
/// are chosen to make 0 the right thing: DbRange 0 is ±24 dB, Selected 0 is band 1. (The doc's first
/// table gave some of these other defaults; this is the one the code keeps.)
enum class Extra : size_t {
	Version = 0,    ///< 0 or 1: layout 1
	Channel1 = 1,   ///< .. Channel8 = 8: the band's `Channel`, 0 Stereo, 1 Mid, 2 Side (step 6)
	Pre = 9,        ///< the analyser's PRE tap: 0 on, 1 off (read by the audio thread: an off tap costs no FFT)
	Post = 10,      ///< the POST tap, likewise
	AnalyserChannel = 11, ///< the VIEW, a `Channel`: the analyser's spectrum and the curve drawn (step 6)
	AnalyserTilt = 12,    ///< slope compensation: 0 4.5, 1 3, 2 0 dB/oct (`kAnalyserTiltDb`)
	AnalyserSpeed = 13,   ///< release: 0 Medium, 1 Fast, 2 Slow (`kAnalyserSpeedSeconds`)
	AnalyserPeak = 14,    ///< peak hold: 0 Off, 1 2 s, 2 Hold
	DbRange = 15,   ///< the panel's zoom: 0 ±24, 1 ±12, 2 ±6, 3 ±3 dB
	Selected = 16,  ///< the band the strip edits, 0..7
	kCount = 17,
};
inline constexpr size_t kExtraState = static_cast<size_t>(Extra::kCount);
inline constexpr float kDbRanges[] = { 24.0f, 12.0f, 6.0f, 3.0f };
/// THE ANALYSER'S VIEW, index by index as stored (zero is the default in each).
inline constexpr float kAnalyserTiltDb[] = { 4.5f, 3.0f, 0.0f };
inline constexpr const char* kAnalyserTiltNames[] = { "4.5 dB/oct", "3 dB/oct", "0 dB/oct" };
inline constexpr float kAnalyserSpeedSeconds[] = { 0.25f, 0.06f, 1.0f };
inline constexpr const char* kAnalyserSpeedNames[] = { "Medium", "Fast", "Slow" };
inline constexpr float kAnalyserPeakSeconds[] = { 0.0f, 2.0f, -1.0f }; // 0 off, -1 held until cleared
inline constexpr const char* kAnalyserPeakNames[] = { "Off", "2 s", "Hold" };

/// WHERE A BAND ACTS (doc §7, R8): on left and right alike, or on the mid (L+R)/2 or side (L-R)/2 alone. No
/// Left / Right. Stored in the extra-state block rather than as a param (R1), so it is not automatable.
enum class Channel : int { Stereo = 0, Mid = 1, Side = 2, kCount };
inline constexpr const char* kChannelNames[] = { "Stereo", "Mid", "Side" };
static_assert(std::size(kChannelNames) == static_cast<size_t>(Channel::kCount));

/// A stored channel field, rounded and clamped — a block from nowhere still names a channel.
[[nodiscard]] inline Channel channelOf(float stored) noexcept {
	const float r = std::isfinite(stored) ? std::round(stored) : 0.0f;
	return static_cast<Channel>(std::clamp(static_cast<int>(r), 0, static_cast<int>(Channel::kCount) - 1));
}

[[nodiscard]] constexpr Extra channelField(int band) noexcept {
	return static_cast<Extra>(static_cast<size_t>(Extra::Channel1) + static_cast<size_t>(band));
}

[[nodiscard]] constexpr size_t extraIndex(Extra e) noexcept {
	return kParamCount + static_cast<size_t>(e);
}

inline constexpr const char* kTags[] = { "eq", "fx" };

inline constexpr int16_t typeControl(int band) {
	return static_cast<int16_t>(bandIndex(band, BandField::Type));
}

// ONE ROW OF THE FACE PER BAND, then the globals — `GenericCard`'s grid, six across, fills row by row in
// this order. The enable is the head's, not the grid's.
inline constexpr uint8_t kFaceOrder[] = {
#define FEATHERS_EQ_FACE_BAND(b)                                                                             \
	static_cast<uint8_t>(bandIndex(b, BandField::On)), static_cast<uint8_t>(bandIndex(b, BandField::Type)),  \
	    static_cast<uint8_t>(bandIndex(b, BandField::Freq)), static_cast<uint8_t>(bandIndex(b, BandField::Gain)), \
	    static_cast<uint8_t>(bandIndex(b, BandField::Q)), static_cast<uint8_t>(bandIndex(b, BandField::Slope))
	FEATHERS_EQ_FACE_BAND(0), FEATHERS_EQ_FACE_BAND(1), FEATHERS_EQ_FACE_BAND(2), FEATHERS_EQ_FACE_BAND(3),
	FEATHERS_EQ_FACE_BAND(4), FEATHERS_EQ_FACE_BAND(5), FEATHERS_EQ_FACE_BAND(6), FEATHERS_EQ_FACE_BAND(7),
#undef FEATHERS_EQ_FACE_BAND
	static_cast<uint8_t>(Param::Offset), static_cast<uint8_t>(Param::Amount), static_cast<uint8_t>(Param::Tilt),
	static_cast<uint8_t>(Param::TiltPivot), static_cast<uint8_t>(Param::Output), static_cast<uint8_t>(Param::Phase),
	static_cast<uint8_t>(Param::KeyTrack),
};
static_assert(std::size(kFaceOrder) == kParamCount - 3, "every control but the enable and the slot's furniture is on the face");

inline constexpr device::Traits kTraits {
	.tag = "eq",
	.name = "EQ",
	.hasCard = true,
	.hasExpanded = true,
	.perVoiceControl = -1,
	.extraState = kExtraState,
	.extraStateKey = "eq_state",
	.enableControl = static_cast<int>(Param::Enabled),
	.faceOrder = kFaceOrder,
	.faceColumns = static_cast<uint8_t>(kBandFields),
	.tags = kTags,
	// THE SLOT BLENDS THESE (backlog NR): a plain crossfade around the device, which hands back the wet.
	// A Linear-phase EQ's latency is declared, so the slot delays the dry by it.
	.mixControl = static_cast<int>(Param::Mix),
	.wetGainControl = static_cast<int>(Param::WetGain),
};

// Every band's six controls, keyed b<n>_<field>. A key names a MEANING (JF): freq is log2(Hz), q is
// log2(Q), gain is dB — change one of those and it needs a new key.
#define FEATHERS_EQ_BAND(n, i)                                                                                 \
	{ .key = "b" n "_on", .name = "Band " n, .min = 0.0, .max = 1.0,                                           \
	  .def = kBandDefaults[i].on ? 1.0 : 0.0, .stepped = true, .unit = device::Unit::Choice,                   \
	  .names = kOffOnNames, .tooltip = "Whether band " n " is in the curve. An off band costs nothing" },     \
	    { .key = "b" n "_type", .name = "Band " n " Type", .min = 0.0,                                         \
	      .max = static_cast<double>(BandType::kCount) - 1.0,                                                  \
	      .def = static_cast<double>(kBandDefaults[i].type), .stepped = true, .unit = device::Unit::Choice,    \
	      .names = kTypeNames,                                                                                 \
	      .tooltip = "What band " n " does. Bell lifts or cuts around its frequency; the shelves lift or "     \
	                 "cut everything below or above it; the cuts remove everything below or above it; Notch " \
	                 "removes a narrow slice" },                                                               \
	    { .key = "b" n "_freq", .name = "Band " n " Freq", .min = kFreqMin, .max = kFreqMax,                   \
	      .def = kBandDefaults[i].freqLog2, .stepped = false, .unit = device::Unit::LogHertz, .names = {},     \
	      .modulatable = true,                                                                                 \
	      .tooltip = "Where band " n " acts. Offset moves it further, and past Nyquist a bell fades out" },    \
	    { .key = "b" n "_gain", .name = "Band " n " Gain", .min = -kGainRangeDb, .max = kGainRangeDb,           \
	      .def = 0.0, .stepped = false, .unit = device::Unit::Decibel, .names = {}, .modulatable = true,       \
	      .tooltip = "How far band " n " lifts or cuts. At exactly 0 dB the band is bypassed. Amount scales it", \
	      .gateControl = typeControl(i), .gateMask = kGainTypes },                                             \
	    { .key = "b" n "_q", .name = "Band " n " Q", .min = kQMin, .max = kQMax, .def = kBandDefaults[i].qLog2, \
	      .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,                    \
	      .tooltip = "How narrow band " n " is. On a cut it is resonance: 0.71 is flat, higher peaks at the "   \
	                 "corner" },                                                                               \
	    { .key = "b" n "_slope", .name = "Band " n " Slope", .min = 0.0,                                       \
	      .max = static_cast<double>(kSlopeCount) - 1.0, .def = static_cast<double>(kDefaultSlope),            \
	      .stepped = true, .unit = device::Unit::Choice, .names = kSlopeNames,                                 \
	      .tooltip = "How steep band " n "'s cut is, in dB per octave",                                        \
	      .gateControl = typeControl(i), .gateMask = kCutTypes }

// INLINE CONSTEXPR IN THE HEADER, not a definition in the .cpp: `device_registry.h`'s width guards walk
// every registered device at compile time (drawn_types.h says why).
inline constexpr device::ParamDesc kParamDescs[] = {
	// ON BY DEFAULT — all effects should default to on when they are added. The
	// default layout is identity, so on-by-default costs nothing audible.
	{ .key = "enabled", .name = "Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the EQ is in the chain" },
	{ .key = "offset", .name = "Offset", .min = -kOffsetRangeOctaves, .max = kOffsetRangeOctaves, .def = 0.0,
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "Moves every band by the same number of octaves, far enough to sweep the whole EQ like a "
	             "filter. A bell pushed past Nyquist fades out" },
	{ .key = "amount", .name = "Amount", .min = -kAmountRange, .max = kAmountRange, .def = 1.0,
	  .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Scales every bell, shelf and the tilt. Negative mirrors the curve; cuts and notches are "
	             "untouched" },
	{ .key = "tilt", .name = "Tilt", .min = -kTiltRangeDbPerOctave, .max = kTiltRangeDbPerOctave, .def = 0.0,
	  .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
	  .tooltip = "Tips the whole spectrum around the pivot, in dB per octave: brighter above it and darker "
	             "below, or the reverse" },
	{ .key = "tilt_pivot", .name = "Tilt Pivot", .min = kPivotMin, .max = kPivotMax, .def = kPivotDef,
	  .stepped = false, .unit = device::Unit::LogHertz, .names = {},
	  .tooltip = "The frequency the tilt turns around, which it leaves at unity" },
	{ .key = "output", .name = "Output", .min = -kOutputRangeDb, .max = kOutputRangeDb, .def = 0.0,
	  .stepped = false, .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
	  .tooltip = "Level after the EQ" },
	// NOT MODULATABLE (JM's L1): it sets the latency the host compensates, which the host reads from the STORED
	// row. A toggle costs the host one restart.
	{ .key = "phase", .name = "Phase", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kPhaseNames,
	  .tooltip = "Linear keeps every frequency in time, at the cost of about 0.2 s of latency and some "
	             "pre-ringing before sharp sounds. In Linear, moving controls update in steps, about 45 times a second" },
	// KEY TRACK (backlog MQ). Off by default, which is today's EQ to the bit. NOT MODULATABLE: a switch.
	{ .key = "key_track", .name = "Key Track", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
	  .unit = device::Unit::Choice, .names = kOffOnNames,
	  .tooltip = "Moves every band with the played note: an octave up doubles every frequency. The bands are set "
	             "for C3. It follows the last note, which suits a monophonic line; with no note it stays at C3" },
	{ .key = "eq_mix", .name = "EQ Mix", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
	  .unit = device::Unit::Percent, .names = {}, .modulatable = true,
	  .tooltip = "Dry against equalised, around the whole device" },
	{ .key = "eq_wet_gain", .name = "EQ Wet Gain", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
	  .unit = device::Unit::Fader, .names = {}, .modulatable = true,
	  .tooltip = "Level of the equalised signal alone, unity at twelve o'clock; the dry in a partial Mix is untouched" },
	FEATHERS_EQ_BAND("1", 0),
	FEATHERS_EQ_BAND("2", 1),
	FEATHERS_EQ_BAND("3", 2),
	FEATHERS_EQ_BAND("4", 3),
	FEATHERS_EQ_BAND("5", 4),
	FEATHERS_EQ_BAND("6", 5),
	FEATHERS_EQ_BAND("7", 6),
	FEATHERS_EQ_BAND("8", 7),
};

#undef FEATHERS_EQ_BAND

static_assert(std::size(kParamDescs) == kParamCount);

extern const device::AudioOps kAudioOps;
extern const device::TextOps kTextOps;

/// TEST SEAMS — the values the DSP is running, read directly (CLAUDE.md: assert the value, never infer
/// it from a rendered buffer). `effectiveForTest` is band `band`'s smoothed, resolved settings; the
/// tilt's is the smoothed effective slope in dB/oct.
[[nodiscard]] EffectiveBand effectiveForTest(const void* storage, int band);
[[nodiscard]] double tiltForTest(const void* storage);
[[nodiscard]] bool bandBypassedForTest(const void* storage, int band);
/// How many analyser transforms have run since the last reset (a tap switched off costs none).
[[nodiscard]] int analyserTransformsForTest(const void* storage);
/// The channel band `band` is RUNNING in (a change waits for its fade out), and whether the last sample
/// went through the mid/side conversion at all (it must not while no Mid or Side band is acting).
[[nodiscard]] Channel bandChannelForTest(const void* storage, int band);
[[nodiscard]] bool midSideActiveForTest(const void* storage);
/// Linear phase: kernels built since the last reset, and swaps made (each one crossfaded over a block).
[[nodiscard]] int linearBuildsForTest(const void* storage);
[[nodiscard]] int linearSwapsForTest(const void* storage);

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.text = &kTextOps,
	.category = device::Category::FilterEq,
};

} // namespace feathers::fx::eq
