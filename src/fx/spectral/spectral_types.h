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
#include "dsp/spectral_shaper.h"
#include "fx/spectral/spectral_stft.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// THE SPECTRAL DEVICE'S CONTROLS. The additive drawer's operators,
// by the same shared function (`shaper::gains()`), on any audio. The ranges are the drawer's (plugin/params.cpp), so
// an operator reads the same in both.
//
// THE FUNDAMENTAL, AND EVERY OPERATOR FOLLOWS IT (/13: Offset shows even when Key Track is off, and Offset moves
// every keytracked control). f0 is C3 (MIDI 60, the house naming) raised
// by Key Offset with Key Track off, the played note raised by it with Key Track on (on the bus the last note the host hands every
// device; per voice the voice's own). The filter's cutoff is held as a harmonic of f0 (the drawer's Key track at 1, pivoting at
// C3); the comb, Odd/Even and the curve are in harmonics of f0; the vowel's formants are scaled by f0 / C3; Tilt has no frequency
// to follow. With Key Track off and Offset 0, every operator is exactly where its controls say. The comb's old Hz spacing
// (`spectral_comb_hz`) is retired: f0 × Spacing covers it.
namespace feathers::fx::spectral {

namespace shaper = ::feathers::spectral;

enum class Param : int {
	Enabled,
	Resolution,
	Mix,
	KeyTrack,
	KeyOffset,
	Poly,
	FilterOn,
	FilterMode,
	FilterCutoff,
	FilterResonance,
	FilterSlope,
	FilterGain,
	Filter2On,
	Filter2Mode,
	Filter2Cutoff,
	Filter2Resonance,
	Filter2Slope,
	Filter2Gain,
	FilterRouting,
	CombOn,
	CombSpacing,
	CombDepth,
	CombPhase,
	VowelOn,
	Vowel,
	VowelDepth,
	TiltOn,
	TiltAmount,
	OddEvenOn,
	OddEvenBalance,
	VocodeOn,
	VocodeBands,
	VocodeAttack,
	VocodeRelease,
	VocodeFlatten,
	VocodeSibilance,
	VocodeCarrier,
	VocodeDepth,
	VocodeGate,
	VocodeFreeze,
	VocodeLow,
	VocodeHigh,
	VocodeStereo,
	VocodeBandwidth,
	VocodeZone,
	VocodeZoneAmount,
	VocodeZoneFree,
	VocodeWidthZone,
	VocodeWidthAmount,
	VocodeWidthFree,
	FShiftOn,
	FShiftLow,
	FShiftHigh,
	FShiftCrossover,
	FShiftLink,
	PartialsOn,
	PartialsMode,
	PartialsCount,
	PartialsSet,
	PartialsDepth,
	CurveOn,
	kCount,
};
inline constexpr size_t kParamCount = static_cast<size_t>(Param::kCount);

inline constexpr const char* kOffOnNames[] = { "Off", "On" };
inline constexpr const char* kResolutionNames[] = { "Fast", "Balanced", "Fine" };
static_assert(std::size(kResolutionNames) == static_cast<size_t>(kResolutionCount));
/// THE LOWEST NOTE EACH STOP RESOLVES, MEASURED (test_fx_spectral, "the resolution tooltip names the measured lowest notes"):
/// a sawtooth through Odd/Even at +1 loses its fundamental by 20 dB and keeps its second harmonic within 1 dB, for that
/// note and every note an octave above it, at 44.1 and 48 kHz (the higher of the two). Fast G#4, Balanced F#3, Fine E1.
inline constexpr int kResolvedMidi[] = { 68, 54, 28 };

inline constexpr const char* kFilterModeNames[] = { "Low Pass", "High Pass", "Band Pass", "Notch", "Bell" };
static_assert(std::size(kFilterModeNames) == static_cast<size_t>(shaper::kFilterModeCount));
/// F2's routing of the two filters (additive design §3.6): Series, the gains multiplied, or Parallel, their held sum.
inline constexpr const char* kRoutingNames[] = { "Series", "Parallel" };
inline constexpr double kLog2Of500 = 8.965784284662087;
/// THE VOCODER'S CARRIER : what is vocoded. Input is the device's own input (the synth, or the rack's main in).
inline constexpr const char* kCarrierNames[] = { "Input", "Square", "White", "Pink" };
enum class Carrier : int { Input, Square, White, Pink };
/// THE VOCODER'S STEREO: one envelope for both, each channel by its own sidechain channel, or the bands alternating.
inline constexpr const char* kVocodeStereoNames[] = { "Mono", "Stereo", "Spread" };
/// THE ZONE (design §10.7.2): where the bands sit between Range Low and High — Harmonic and Inharmonic on the carrier's harmonics
/// of the operator f0 or between them (Z3, two entries:); and Free, the amount a coordinate through their mixes (Z4; the
/// list grows; a Choice is stored by label, so a saved patch keeps its Zone).
inline constexpr const char* kZoneNames[] = { "Even", "Speech", "Low", "High", "Harmonic", "Inharmonic", "Free" };
/// THE BANDWIDTH ZONE (design §10.7.2, Z5): how the widths spread across the layout, each by its amount from Even; Free its own
/// coordinate (separate from the Zone's, by decision).
inline constexpr const char* kWidthZoneNames[] = { "Even", "Narrow Low", "Narrow High", "Harmonic", "Inharmonic", "Free" };
/// **WHAT EACH MODE DOES, READ FROM `Stft::rebuildBands` AND `Stft::widthFactor`** — the mode cells' dropdown lines (Z7: Zone and
/// Widths are the house's mode cells, as OWLTT's Character and Vibe; backlog IW).
inline constexpr const char* kZoneDescriptions[] = {
	"Bands evenly spaced per octave between Low and High",
	"Bands along the Bark scale, crowded through the speech range",
	"Bands crowded toward Low, sparse above",
	"Bands crowded toward High, sparse below",
	"Bands on the carrier's harmonics of the note, grouped higher up",
	"Bands in the gaps between the note's harmonics",
	"One coordinate through mixes of Speech, Low, High and the harmonics — just keep turning",
};
inline constexpr const char* kWidthZoneDescriptions[] = {
	"Every band the same width against its spacing",
	"Narrow bands below, wide above",
	"Wide bands below, narrow above",
	"Narrow on the note's harmonics, wide between",
	"Wide on the note's harmonics, narrow between",
	"One coordinate through mixes of the tilt and the harmonic bias — just keep turning",
};
static_assert(std::size(kZoneDescriptions) == std::size(kZoneNames) && std::size(kWidthZoneDescriptions) == std::size(kWidthZoneNames));
/// FREE IS LAST IN BOTH (the house rule: Free is infinite), and the mode each Free coordinate is gated on.
inline constexpr uint32_t kZoneFree = std::size(kZoneNames) - 1;
inline constexpr uint32_t kWidthZoneFree = std::size(kWidthZoneNames) - 1;
/// A Free coordinate's window: unbounded in practice (OWLTT's and the Disperser's 2^23), a drag window (`device::isDragWindow`).
inline constexpr double kFreeCoordMax = 8388608.0;
/// GENERALISED PARTIALS : keep the N strongest PEAKS of each frame (any audio), or HARMONICS 1..N of f0.
/// **A MODE CELL (backlog OH/OI)**: the dropdown is the cull SCHEME, the box the count N; the drawer's Partials
/// cell offers the same list (params.h kSpectralPartialsModeNames). Stored by label, so OI's schemes append.
/// CENTERS (OI's first scheme): only the harmonics AT a centre set — the fundamental and a few of 2..7, the
/// generator the terrain's sparse country uses (partials::sparseKeep) — its box the set, a coordinate that glides between sets.
inline constexpr const char* kPartialsModeNames[] = { "Peaks", "Harmonics", "Centers" };
inline constexpr const char* kPartialsModeDescriptions[] = {
	"The N strongest peaks of the sound, whatever their pitch",
	"Harmonics 1 to N of the note (Key Track and Offset)",
	"Only the fundamental and a few low harmonics — keep turning for another set",
};
static_assert(std::size(kPartialsModeDescriptions) == std::size(kPartialsModeNames));

/// Key Track off, f0 is this raised by Key Offset: C3, MIDI 60 (the house naming).
inline constexpr double kFixedF0Hz = 261.6255653005986;
inline constexpr int kFixedF0Midi = 60;
inline constexpr double kLog2Of20 = 4.321928094887363;
inline constexpr double kLog2Of200 = 7.643856189774724;
inline constexpr double kLog2Of100 = 6.643856189774724;
inline constexpr double kLog2Of1k = 9.965784284662087;
inline constexpr double kLog2Of8k = 12.965784284662087;
inline constexpr double kLog2Of2k = 10.965784284662087;
inline constexpr double kLog2Of20k = 14.287712379549449;
/// The Bell's Gain range, the shaper's.
inline constexpr double kBellDb = shaper::kBellMaxDb;
/// Tilt pivots where the drawer's does (TiltOp's default).
inline constexpr float kTiltPivotHz = 1000.0f;
/// THE VOCODER'S GATE at its bottom is off: nothing is gated.
inline constexpr double kVocodeGateOffDb = -96.0;
/// The comb's spacing in Hz on the mix: 20 Hz to 2 kHz.

struct Settings {
	bool enabled = true;
	Resolution resolution = Resolution::Balanced;
	float mix = 1.0f;
	bool keyTrack = false;
	float keyOffset = 0.0f; ///< semitones added to the played note
	bool poly = false; ///< the house Poly switch: per voice, each voice tracking its own note (plan T7)
	bool filterOn = false;
	int filterMode = 0;
	float filterCutoff = static_cast<float>(kLog2Of2k); ///< log2 Hz
	float filterResonance = 0.0f;
	float filterSlope = 0.25f;
	float filterGain = 0.0f; ///< dB, Bell only
	// THE SECOND FILTER (F2): off, Low Pass at 500 Hz, so switching it on does not stack its handle on filter 1's.
	bool filter2On = false;
	int filter2Mode = 0;
	float filter2Cutoff = static_cast<float>(kLog2Of500); ///< log2 Hz
	float filter2Resonance = 0.0f;
	float filter2Slope = 0.25f;
	float filter2Gain = 0.0f;
	bool filterParallel = false; ///< the routing: Series (false) or Parallel
	bool combOn = false;
	float combSpacing = 2.0f; ///< harmonics of f0
	float combDepth = 0.5f;
	float combPhase = 0.0f;
	bool vowelOn = false;
	float vowel = 3.0f;
	float vowelDepth = 0.7f;
	bool tiltOn = false;
	float tiltAmount = 0.0f;
	bool curveOn = false;
	/// THE DRAWN CURVE, the hand's block `spectral_curve_db` (NOT the row): band b is harmonic b + 1 of f0. STORED IN dB, so the zeroed block
	/// (`defaultRowFor` leaves it at 0) is a flat curve: kCurveFloorDb..kCurveMaxDb, the floor exact silence (`curveGainOf`).
	std::array<float, shaper::kCurveBands> curveDb {};
	bool oddEvenOn = false;
	float oddEvenBalance = 0.0f;
	// THE VOCODER (design §10.3): on the bus only; under Poly it is hidden and hears no sidechain.
	bool vocodeOn = false;
	float vocodeBands = 24.0f;
	float vocodeAttackMs = 10.0f;
	float vocodeReleaseMs = 80.0f;
	float vocodeFlatten = 1.0f;
	float vocodeSibilance = 0.3f;
	int vocodeCarrier = 0; ///< Carrier: Input, Square (at f0), White or Pink noise
	float vocodeDepth = 1.0f; ///< 0 the carrier unchanged, 1 the classic vocoder, 2 only the envelope's peaks
	float vocodeGateDb = -96.0f; ///< dBFS; at the bottom, off
	bool vocodeFreeze = false;
	float vocodeLow = static_cast<float>(kLog2Of20); ///< log2 Hz: below, the envelope is held at its value here
	float vocodeHigh = static_cast<float>(kLog2Of20k); ///< log2 Hz: above, held likewise
	int vocodeStereo = 0; ///< Mono, Stereo or Spread
	float vocodeBandwidth = 1.0f; ///< 1 the smooth envelope; below, discrete bands, narrower toward 0
	int vocodeZone = 0; ///< Even, Speech, Low, High, Harmonic, Inharmonic, Free
	float vocodeZoneAmount = 0.5f; ///< in a named Zone: 0 is Even; rising, more of the Zone's warp (fenced 0..1)
	float vocodeZoneFree = 0.5f;   ///< in Free: the unbounded coordinate (Z7)
	int vocodeWidthZone = 0; ///< Even, Narrow Low, Narrow High, Harmonic, Inharmonic, Free
	float vocodeWidthAmount = 0.5f; ///< in a named Widths mode, fenced 0..1
	float vocodeWidthFree = 0.5f;   ///< in Free: the unbounded coordinate (Z7)
	// FORMANT SHIFT (design §10.4, the TIMBRE operator): the envelope moved, the partials kept. The drawer's controls and ranges.
	bool fshiftOn = false;
	float fshiftLow = 0.0f;  ///< semitones; linked, the whole envelope
	float fshiftHigh = 0.0f; ///< semitones above the crossover, unlinked only
	float fshiftCrossover = static_cast<float>(kLog2Of1k);
	bool fshiftLink = true;
	// GENERALISED PARTIALS (design §11): the input simplified to N partials, the rest attenuated by Depth.
	bool partialsOn = false;
	int partialsMode = 0; ///< 0 Peaks, 1 Harmonics
	float partialsCount = 8.0f; ///< continuous: the top partial is the fraction
	float partialsSet = 0.0f;   ///< Centers' set: an unbounded coordinate (partials::sparseKeep)
	float partialsDepth = 1.0f;
	friend bool operator==(const Settings&, const Settings&) = default;
};

/// THE DRAWN CURVE'S RANGE in dB: up to +12 (the drawer's kSpectralCurveGainMax is 4, +12.04 dB), and at or below the floor
/// EXACT SILENCE, so a band can be drawn out entirely (-36 dB is not silence: the drawer's rule).
inline constexpr float kCurveMaxDb = 12.0f;
inline constexpr float kCurveFloorDb = -36.0f;
/// A stored band's gain: 0 at or below the floor, else its amplitude.
[[nodiscard]] inline float curveGainOf(float db) {
	return db <= kCurveFloorDb ? 0.0f : static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
}
/// The curve as the shaper reads it: a gain per band.
[[nodiscard]] inline std::array<float, shaper::kCurveBands> curveGainsOf(const std::array<float, shaper::kCurveBands>& db) {
	std::array<float, shaper::kCurveBands> g {};
	for (size_t b = 0; b < g.size(); ++b) {
		g[b] = curveGainOf(db[b]);
	}
	return g;
}
/// THE ROW: the controls and nothing else. The drawn curve left it (row headroom, 2026-10-27) for the hand's block
/// `spectral_curve_db`, so the row holds `kParamCount` of the 128 fields.
inline constexpr size_t kRowFloats = kParamCount;
using Row = std::array<float, kRowFloats>;

/// THE READINGS: the input spectrum, `kSpectrumBands` bands even in log Hz from 20 Hz to 20 kHz, in dBFS (spectral_stft.h); then
/// f0 in Hz (what every operator follows: shaperF0), and 1 while it is a played note (Key Track on and a note heard), 0 while it
/// is C3's reference.
inline constexpr size_t kReadingF0 = static_cast<size_t>(kSpectrumBands);
inline constexpr size_t kReadingPlaying = kReadingF0 + 1;
/// THE VOCODER'S (design §10.5): 1 while a sidechain is connected, then its envelope at the input bands' centres, in dBFS.
inline constexpr size_t kReadingSideSounding = kReadingPlaying + 1;
inline constexpr size_t kReadingSideEnvelope = kReadingSideSounding + 1;
inline constexpr size_t kReadingCount = kReadingSideEnvelope + static_cast<size_t>(kSpectrumBands);
/// Input band b's centre, Hz: even in log frequency from kBandLowHz to kBandHighHz (spectral_stft.h).
[[nodiscard]] inline double bandCentreHz(int b) {
	return kBandLowHz * std::exp2(std::log2(kBandHighHz / kBandLowHz) * (static_cast<double>(b) + 0.5) / kSpectrumBands);
}

Settings stateOf(const Row& row, size_t fieldsPresent);
Row rowOf(const Settings& settings);

/// THE SHAPER'S PARAMS AT f0 (`shaperF0`): Key track at 1, the vowel scaled by f0 / C3, the comb, Odd/Even and the curve in
/// harmonics of f0.
[[nodiscard]] shaper::ShaperParams shaperParamsOf(const Settings& s, double f0);
/// f0: the tracked note, or kFixedF0Hz, raised by `keyOffset` semitones.
[[nodiscard]] double shaperF0(const Settings& s, bool tracked, double noteHz);

/// THE GAIN PER BIN, design §3.3: hz_i = i·sr/N (floored at shaper::kMinHz), k_i = hz_i / f0, base_i =
/// 1/sqrt(max(hz_i, 20)) (pink, static), gain_i = gains(prepare(params, f0), k, hz, base)_i. Writes N/2 + 1 values.
/// Returns whether the result is the identity (every value exactly 1). Scratch arrays are the caller's, of N/2 + 1.
struct BinScratch {
	float* k;
	float* hz;
	float* base;
};
bool binGains(const Settings& s, bool tracked, double noteHz, double sampleRate, int fftSize, BinScratch scratch,
              float* out) noexcept;

inline constexpr device::RetiredKey kRetiredKeys[] = {
	{ "spectral_curve", "2026-10-27",
	  "the drawn curve left the row's extra state (128-float cap) for the hand's block spectral_curve_db, which saves it whole" },
	{ "spectral_comb_hz", "2026-10-12",
	  "the comb is always in harmonics of f0 now (C3 raised by Key Offset with Key Track off), so f0 × Spacing covers a spacing in Hz" },
};

inline constexpr const char* kTags[] = { "spectral", "filter", "fx" };
inline constexpr device::Traits kTraits {
	.tag = "spectral",
	.name = "Spectral",
	.hasCard = true,
	.hasExpanded = true,
	.perVoiceControl = static_cast<int>(Param::Poly),
	.enableControl = static_cast<int>(Param::Enabled),
	.amountControl = static_cast<int>(Param::Mix),
	.tags = kTags,
	.retiredKeys = kRetiredKeys,
	.latentPerVoice = true,
	// ITS MIX IS ITS OWN (backlog NR, D2): named for the frame, made by the device, so the slot does not blend and a
	// POST chain is a series insert after it.
	.mixControl = static_cast<int>(Param::Mix),
	.mixInDevice = true,
};

inline constexpr int16_t kFilterModeControl = static_cast<int16_t>(Param::FilterMode);
inline constexpr int16_t kFilter2ModeControl = static_cast<int16_t>(Param::Filter2Mode);
/// Resonance acts in every mode but Notch (in Bell it is the width).
inline constexpr uint32_t kResonanceModes = 0b10111u;
/// Gain acts in Bell alone.
inline constexpr uint32_t kGainModes = 0b10000u;
/// **ZONE AND WIDTHS ARE MODE CELLS (Z7)**: each named mode reads a FENCED amount 0..1 from Even, Free reads its own unbounded
/// coordinate, and Even reads neither (it is where every amount starts).
inline constexpr int16_t kZoneControl = static_cast<int16_t>(Param::VocodeZone);
inline constexpr uint32_t kZoneNamedModes = ((1u << kZoneFree) - 1u) & ~1u;
inline constexpr uint32_t kZoneFreeMode = 1u << kZoneFree;
inline constexpr int16_t kWidthZoneControl = static_cast<int16_t>(Param::VocodeWidthZone);
inline constexpr uint32_t kWidthZoneNamedModes = ((1u << kWidthZoneFree) - 1u) & ~1u;
inline constexpr uint32_t kWidthZoneFreeMode = 1u << kWidthZoneFree;
/// THE PARTIALS MODE CELL (OH): its count is read by Peaks and Harmonics, its set by Centers.
inline constexpr int16_t kPartialsModeControl = static_cast<int16_t>(Param::PartialsMode);
inline constexpr uint32_t kPartialsCountModes = 0b011u;
inline constexpr uint32_t kPartialsCentersMode = 0b100u;

inline constexpr device::ParamDesc kParamDescs[] = {
    {.key = "spectral_enabled", .name = "Spectral Enabled", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames, .modulatable = true, .tooltip = "Whether the spectral shaper is in the chain"},
    {.key = "spectral_resolution", .name = "Spectral Resolution", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kResolutionNames,
     .tooltip = "How finely the spectrum is resolved, against how late it sounds. Fast is 5 ms late and separates the "
                "harmonics of notes from G#4 up; Balanced is 10 ms and resolves from F#3; Fine is 40 ms and resolves from E1"},
    {.key = "spectral_mix", .name = "Spectral Mix", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "The shaped sound against the dry, which is delayed to line up with it"},
    {.key = "spectral_keytrack", .name = "Spectral Key Track", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Measures the comb, Odd/Even and the curve in the played note's harmonics, raised by Offset; off, in "
                "harmonics of C3 raised by Offset. It follows the last note, which suits a monophonic line"},
    {.key = "spectral_key_offset", .name = "Spectral Key Offset", .min = -36.0, .max = 36.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "Moves the fundamental the comb, Odd/Even and the curve are measured in, in semitones: from C3 with Key "
                "Track off (so C0 to C6), from the played note with it on"},
    {.key = "spectral_poly", .name = "Spectral Poly", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Runs one Spectral per voice, each following its own note, instead of one on the mix following the last. Each "
                "voice is delayed by the Resolution's latency, and its envelope with it"},
    {.key = "spectral_filter_on", .name = "Spectral Filter", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches the filter in: a magnitude curve with no phase shift and no instability"},
    {.key = "spectral_filter_mode", .name = "Spectral Filter Mode", .min = 0.0, .max = 4.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kFilterModeNames,
     .tooltip = "What the filter keeps: below the cutoff, above it, around it, or all but around it; or a Bell, which lifts or "
                "cuts around it by its Gain"},
    {.key = "spectral_filter_cutoff", .name = "Spectral Cutoff", .min = kLog2Of20, .max = kLog2Of20k, .def = kLog2Of2k,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "Where the filter's curve turns"},
    {.key = "spectral_filter_res", .name = "Spectral Resonance", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "A peak drawn at the cutoff; in Bell, the width, from 3 octaves down to a sixth of one",
     .gateControl = kFilterModeControl, .gateMask = kResonanceModes},
    {.key = "spectral_filter_slope", .name = "Spectral Slope", .min = 0.0, .max = 1.0, .def = 0.25, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How steeply the filter falls, from gentle (6 dB/oct) to a brick wall (96 dB/oct); in Bell, how steep its sides "
                "are: a classic bell at the default, softer below, a flat-topped band above"},
    // F2 (additive design §3.6): the Bell's Gain, the second filter (the first's controls; its cutoff follows f0 as the first's
    // does) and the routing. All modulation destinations: the per-device control cap is gone (mainline d1543cb3, unbounded routes and
    // controls), so filter 2's amounts, held back as plain controls while Spectral sat at the 32-control stride, are destinations now.
    {.key = "spectral_filter_gain", .name = "Spectral Filter Gain", .min = -kBellDb, .max = kBellDb, .def = 0.0, .stepped = false,
     .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "How far the Bell lifts or cuts around its centre; at 0 dB it does nothing",
     .gateControl = kFilterModeControl, .gateMask = kGainModes},
    {.key = "spectral_filter2_on", .name = "Spectral Filter 2", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches the second filter in, in series or in parallel with the first"},
    {.key = "spectral_filter2_mode", .name = "Spectral Filter 2 Mode", .min = 0.0, .max = 4.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kFilterModeNames,
     .tooltip = "What the second filter keeps: below the cutoff, above it, around it, or all but around it; or a Bell"},
    {.key = "spectral_filter2_cutoff", .name = "Spectral Filter 2 Cutoff", .min = kLog2Of20, .max = kLog2Of20k, .def = kLog2Of500,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "Where the second filter's curve turns"},
    {.key = "spectral_filter2_res", .name = "Spectral Filter 2 Resonance", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "A peak drawn at the second filter's cutoff; in Bell, the width",
     .gateControl = kFilter2ModeControl, .gateMask = kResonanceModes},
    {.key = "spectral_filter2_slope", .name = "Spectral Filter 2 Slope", .min = 0.0, .max = 1.0, .def = 0.25, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How steeply the second filter falls, from 6 to 96 dB/oct; in Bell, how steep its sides are"},
    {.key = "spectral_filter2_gain", .name = "Spectral Filter 2 Gain", .min = -kBellDb, .max = kBellDb, .def = 0.0,
     .stepped = false, .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "How far the second filter's Bell lifts or cuts around its centre; at 0 dB it does nothing",
     .gateControl = kFilter2ModeControl, .gateMask = kGainModes},
    {.key = "spectral_filter_routing", .name = "Spectral Filter Routing", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kRoutingNames,
     .tooltip = "How the two filters combine: in series one after the other, or in parallel side by side, so two band-passes are "
                "two bands. Only while both are on"},
    {.key = "spectral_comb_on", .name = "Spectral Comb", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches the comb in: evenly spaced notches, which move like a flanger with no delay line"},
    {.key = "spectral_comb_spacing", .name = "Spectral Comb Spacing", .min = 1.0, .max = 16.0, .def = 2.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How many harmonics of the note apart the comb's notches sit"},
    {.key = "spectral_comb_depth", .name = "Spectral Comb Depth", .min = 0.0, .max = 1.0, .def = 0.5, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .tooltip = "How deep the comb's notches cut"},
    {.key = "spectral_comb_phase", .name = "Spectral Comb Phase", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Slides the notches along; sweep it for the flanger's motion"},
    {.key = "spectral_vowel_on", .name = "Spectral Formant", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches the vowel in: two formant peaks fixed in Hz, with the loudness held"},
    {.key = "spectral_vowel", .name = "Spectral Vowel", .min = 0.0, .max = 7.0, .def = 3.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "Which vowel: a walk through Breath, Hum, Round, Open, Bright, Nasal, Growl and Rasp, gliding between "
                "them"},
    {.key = "spectral_vowel_depth", .name = "Spectral Vowel Depth", .min = 0.0, .max = 1.0, .def = 0.7, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true, .tooltip = "How strongly the vowel speaks"},
    {.key = "spectral_tilt_on", .name = "Spectral Tilt", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches Tilt in: brighter or darker, with the loudness held"},
    {.key = "spectral_tilt", .name = "Spectral Tilt Amount", .min = -24.0, .max = 24.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "The tilt in dB per octave about 1 kHz: up is brighter, down is darker"},
    {.key = "spectral_oddeven_on", .name = "Spectral Odd/Even", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches Odd/Even in: the balance of the note's odd and even harmonics", },
    {.key = "spectral_oddeven", .name = "Spectral Balance", .min = -1.0, .max = 1.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Down keeps the odd harmonics, up the even ones; the middle keeps both", .centred = true},
    {.key = "spectral_vocode_on", .name = "Spectral Vocode", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches the vocoder in: the sound flattened by its own envelope and given the sidechain's. A silent sidechain "
                "gives a silent output, as a real vocoder's does"},
    {.key = "spectral_vocode_bands", .name = "Spectral Vocode Bands", .min = 8.0, .max = kMaxBands, .def = 24.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How finely the envelopes are taken: few coarse bands, as a classic vocoder's, up to a smooth envelope that "
                "follows the formants"},
    {.key = "spectral_vocode_attack", .name = "Spectral Vocode Attack", .min = 1.0, .max = 500.0, .def = 10.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How quickly the sidechain's envelope rises, in milliseconds"},
    {.key = "spectral_vocode_release", .name = "Spectral Vocode Release", .min = 5.0, .max = 2000.0, .def = 80.0,
     .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How slowly the sidechain's envelope falls, in milliseconds"},
    {.key = "spectral_vocode_flatten", .name = "Spectral Vocode Flatten", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How much of the sound's own envelope is taken out before the sidechain's goes in"},
    {.key = "spectral_vocode_sibilance", .name = "Spectral Vocode Sibilance", .min = 0.0, .max = 1.0, .def = 0.3,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "Noise mixed in where the sidechain is noisy (its esses and breaths), at its level"},
    {.key = "spectral_vocode_carrier", .name = "Spectral Vocode Carrier", .min = 0.0, .max = 3.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kCarrierNames,
     .tooltip = "What is vocoded: the input, or a square wave at the note (the played note with Key Track, C3 without; Offset does "
                "not move it), or white or pink noise"},
    {.key = "spectral_vocode_depth", .name = "Spectral Vocode Depth", .min = 0.0, .max = 2.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How much of the sidechain's envelope goes in: none leaves the sound as it is, all is the classic vocoder, "
                "and past that only its loudest peaks come through"},
    {.key = "spectral_vocode_gate", .name = "Spectral Vocode Gate", .min = kVocodeGateOffDb, .max = 0.0, .def = kVocodeGateOffDb,
     .stepped = false, .unit = device::Unit::Decibel, .names = {}, .modulatable = true,
     .tooltip = "Where the sidechain is quieter than this, its bands close (breaths and noise stay out); at the bottom, off"},
    {.key = "spectral_vocode_freeze", .name = "Spectral Vocode Freeze", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Holds the sidechain's envelope as it is now, for as long as Freeze is on; it follows the note when Key Track is "
                "on, like a transposed recording"},
    {.key = "spectral_vocode_low", .name = "Spectral Vocode Low", .min = kLog2Of20, .max = kLog2Of20k, .def = kLog2Of20,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "The lowest band: below it, the envelope is held at its level here"},
    {.key = "spectral_vocode_high", .name = "Spectral Vocode High", .min = kLog2Of20, .max = kLog2Of20k, .def = kLog2Of20k,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "The highest band: above it, the envelope is held at its level here"},
    {.key = "spectral_vocode_stereo", .name = "Spectral Vocode Stereo", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kVocodeStereoNames,
     .tooltip = "Mono vocodes both channels by the sidechain's sum; Stereo each by its own sidechain channel; Spread sends the "
                "bands alternately left and right"},
    {.key = "spectral_vocode_bandwidth", .name = "Spectral Vocode Bandwidth", .min = 0.0, .max = 1.0, .def = 1.0,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How wide each band is against its neighbours: all the way up a smooth envelope, down to narrow bands that "
                "hold their own level (a staircase)"},
    // **SPACING: THE HOUSE'S MODE CELL (Z7; the zone knobs aren't zone knob widgets)** — the mode, then the value
    // it reads: a named spacing's fenced amount, or Free's unbounded coordinate. Not a destination: a layout switching under a route
    // is a different filterbank arriving mid-note. NAMED SPACING ON THE FACE (Z7b; calling a zone knob 'zone' is a miss);
    // its key and the code's identifiers keep "zone" — a rename keeps its key.
    {.key = "spectral_vocode_zone", .name = "Spectral Vocode Spacing", .min = 0.0, .max = 6.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kZoneNames,
     .tooltip = "Where the bands sit: even per octave, along the Bark scale (dense through speech), crowded toward the lows or "
                "the highs, on the note's harmonics or between them — or Free, any mix of them",
     .shortName = "Spacing", .modeCell = true},
    // FENCED: each named Zone is a monotonic warp from Even, and nothing lies past all of it — a lap would either jump back to
    // Even (a wrap) or retrace the warp (a fold), neither of them new ground. Its meaning in the named Zones is unchanged, so its
    // key is too; Free no longer reads it (its coordinate is the next control's).
    {.key = "spectral_vocode_zone_amount", .name = "Spectral Vocode Spacing Amount", .min = 0.0, .max = 1.0, .def = 0.5,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How far toward the chosen spacing: none is even per octave, all is the spacing itself",
     .gateControl = kZoneControl, .gateMask = kZoneNamedModes},
    // **FREE'S COORDINATE, UNBOUNDED (Z7)** — just keep turning. Its four axes are phi
    // triangles at incommensurate rates, so every turn is somewhere new and 0 is Even. A NEW KEY: it was Zone Amount's 0..1 in Z4.
    // A full-depth route moves it one unit: four-fifths of its slowest axis's cycle (1/0.809).
    {.key = "spectral_vocode_zone_free", .name = "Spectral Vocode Spacing Free", .min = -kFreeCoordMax, .max = kFreeCoordMax,
     .def = 0.5, .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "Where you are among the mixes of the layouts — keep turning, and the mix keeps changing; 0 is even",
     .zones = 0, .modSpan = 1.0, .zoneNames = {}, .gateControl = kZoneControl, .gateMask = kZoneFreeMode},
    {.key = "spectral_vocode_width_zone", .name = "Spectral Vocode Widths", .min = 0.0, .max = 5.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kWidthZoneNames,
     .tooltip = "How the bands' widths spread: even, narrow below and wide above or the reverse, narrow on the note's "
                "harmonics and wide between or the reverse — or Free, any mix of them",
     .shortName = "Widths", .modeCell = true},
    {.key = "spectral_vocode_width_amount", .name = "Spectral Vocode Widths Amount", .min = 0.0, .max = 1.0, .def = 0.5,
     .stepped = false, .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How far the widths spread: none is even",
     .gateControl = kWidthZoneControl, .gateMask = kWidthZoneNamedModes},
    {.key = "spectral_vocode_width_free", .name = "Spectral Vocode Widths Free", .min = -kFreeCoordMax, .max = kFreeCoordMax,
     .def = 0.5, .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "Where you are among the mixes of the width spreads — keep turning, and the mix keeps changing; 0 is even",
     .zones = 0, .modSpan = 1.0, .zoneNames = {}, .gateControl = kWidthZoneControl, .gateMask = kWidthZoneFreeMode},
    {.key = "spectral_fshift_on", .name = "Spectral Formant Shift", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches Formant Shift in: the envelope slides in frequency while the partials stay put — the sidechain's while "
                "vocoding, the sound's own otherwise"},
    {.key = "spectral_fshift_low", .name = "Spectral Shift Low", .min = -36.0, .max = 36.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How far the envelope below the crossover moves, in semitones. Linked, it moves all of it"},
    {.key = "spectral_fshift_high", .name = "Spectral Shift High", .min = -36.0, .max = 36.0, .def = 0.0, .stepped = false,
     .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How far the envelope above the crossover moves, in semitones. Unlinked only"},
    {.key = "spectral_fshift_crossover", .name = "Spectral Shift Crossover", .min = kLog2Of100, .max = kLog2Of8k, .def = kLog2Of1k,
     .stepped = false, .unit = device::Unit::LogHertz, .names = {}, .modulatable = true,
     .tooltip = "Where the low shift hands over to the high one. Unlinked only"},
    {.key = "spectral_fshift_link", .name = "Spectral Shift Link", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Linked, the low shift moves the whole envelope and the high shift and crossover rest"},
    {.key = "spectral_partials_on", .name = "Spectral Partials", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches Partials in: the sound simplified to its strongest partials, the rest turned down by Depth"},
    {.key = "spectral_partials_mode", .name = "Spectral Partials Mode", .min = 0.0, .max = 2.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kPartialsModeNames,
     .tooltip = "Which partials are kept: Peaks the strongest peaks of any sound, Harmonics the note's harmonics (Key Track and "
                "Offset), Centers only the fundamental and a few low harmonics",
     .shortName = "Partials", .modeCell = true},
    // THE MODE CELL'S BOXES (OH, OI): the count in Peaks and Harmonics, the set in Centers (the cell finds its followers by the gate).
    // The count's key and meaning are unchanged; its floor was already 1 here (the drawer's was 8).
    {.key = "spectral_partials_count", .name = "Spectral Partials Count", .min = kPartialsMin, .max = kPartialsMax, .def = 8.0,
     .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "How many partials are kept; between whole numbers the last one fades in",
     .gateControl = kPartialsModeControl, .gateMask = kPartialsCountModes},
    // CENTERS' SET (OI, 2026-10-19): a coordinate as Spacing's Free is — unbounded, "just keep turning", each set gliding into the
    // next. A modulation destination (the per-device control cap is gone, mainline d1543cb3); a full-depth route moves it one unit.
    {.key = "spectral_partials_set", .name = "Spectral Partials Set", .min = -kFreeCoordMax, .max = kFreeCoordMax, .def = 0.0,
     .stepped = false, .unit = device::Unit::Number, .names = {}, .modulatable = true,
     .tooltip = "Which centre set Centers keeps — keep turning, and the set glides from one handful of low harmonics to the next",
     .zones = 0, .modSpan = 1.0, .zoneNames = {}, .gateControl = kPartialsModeControl, .gateMask = kPartialsCentersMode},
    {.key = "spectral_partials_depth", .name = "Spectral Partials Depth", .min = 0.0, .max = 1.0, .def = 1.0, .stepped = false,
     .unit = device::Unit::Percent, .names = {}, .modulatable = true,
     .tooltip = "How far everything but the kept partials is turned down: all the way at 100%"},
    {.key = "spectral_curve_on", .name = "Spectral Curve", .min = 0.0, .max = 1.0, .def = 0.0, .stepped = true,
     .unit = device::Unit::Choice, .names = kOffOnNames,
     .tooltip = "Switches the drawn curve in: a gain drawn across the spectrum, from 20 Hz to 20 kHz"},
};
static_assert(std::size(kParamDescs) == kParamCount);

extern const device::AudioOps kAudioOps;
/// KEY OFFSET PRINTS THE PITCH IT SETS while Key Track is off ("C3 +0", "G3 +7"), and semitones while it is on ("+7 st").
extern const device::TextOps kTextOps;
/// The note name of MIDI `midi` in the house naming (60 is C3).
void noteName(int midi, char* out, size_t size);

/// **THE FROZEN ENVELOPE, SAVED WITH THE PATCH** (Z10b, backlog NM; CLAUDE.md "RANDOM AND CAPTURED STATE PERSISTS WITH THE
/// PATCH"): Freeze captures the sidechain's envelope at a moment, which nothing can derive again. Per sidechain channel (the
/// second only under Stereo): whether it holds, its kEnvelopePoints grid IN dBFS (the FFT's own scale moves with Resolution,
/// so a patch loads into any stop the same envelope), and its flatness above 2 kHz (Sibilance reads it); then Key Track's
/// note as Freeze took hold. Floats by their bits (`device::BlockWriter`). **THE LOAD DECISIONS** (2026-10-18): a
/// patch loaded with Freeze ON primes the envelope and is frozen at once; with Freeze OFF the stored shape is ignored.
/// **WHAT IS NOT SAVED, AND WHY**: a Poly instance's envelope (each voice's ends with its note), and the envelope while
/// Freeze is off (it is the live sidechain's, not a capture).
inline constexpr size_t kFreezeSideWords = 1 + 2 * static_cast<size_t>(kEnvelopePoints) + 2;
inline constexpr uint32_t kFreezeWords = static_cast<uint32_t>(2 * kFreezeSideWords + 1 + 4);
/// THE DRAWN CURVE'S BLOCK: one float per band, in dB.
inline constexpr uint32_t kCurveBlockFloats = static_cast<uint32_t>(shaper::kCurveBands);
uint64_t readBlock(const void* storage, size_t index, float* out, size_t capacity);
bool restoreBlock(void* storage, size_t index, const float* in, size_t n);
bool applyBlock(void* storage, size_t index, const float* in, size_t n);
inline constexpr device::StateBlockDesc kBlocks[] = {
	{ .key = "spectral_freeze", .floats = kFreezeWords },
	// THE DRAWN CURVE (a HAND'S block, `device::HandBlock`): band b in dB, harmonic b + 1 of f0, written by the editor through
	// `DeviceHost::setBlock` and pulled by the audio thread. It was the row's extra state `spectral_curve` until the row ran out.
	{ .key = "spectral_curve_db", .floats = kCurveBlockFloats, .handEdited = true },
};
inline constexpr device::StateOps kStateOps { .blocks = kBlocks, .read = &readBlock, .restore = &restoreBlock, .apply = &applyBlock };

inline constexpr device::Descriptor kDescriptor {
	.traits = kTraits,
	.params = kParamDescs,
	.outputs = {},
	.audio = &kAudioOps,
	.state = &kStateOps,
	.text = &kTextOps,
	.category = device::Category::FilterEq,
};

} // namespace feathers::fx::spectral
