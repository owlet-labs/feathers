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

// THE SPECTRAL DRAWER'S OPERATORS, as ONE pure function
// every caller shares: the additive engine per partial per control block, and later the Spectral effect
// device per frame. Amplitude operators MULTIPLY, so their order does not matter.
//
// TWO ROADS, ONE MEANING. The single-value functions (gain(), formantRatio(), each operator alone) are the
// REFERENCE: double precision, written as the formulas read, and what the tests restate. prepare() + gains() /
// formantRatios() is the BATCH: everything that depends only on the params and the note is computed once in
// prepare(), and the per-partial work is vector kernels with polynomial log2/exp2/cos (two partials per SSE2 double
// vector), per partial log2(hz) taken once and shared by Filter and Tilt. The tests hold the batch to the reference
// per operator in dB. The voice takes the batch THROUGH GainCache, which keeps each group's factors until something
// they read moves, and is gains() to the bit.
//
// THE IDENTITY IS EXACT: with the drawer disabled, or every operator off, gain() and gains() return 1.0f and
// formantRatio()/formantRatios() return 1.0, bit for bit. A neutral drawer is the engine's own sound, not a
// rounding of it.
//
// BOUNDS. Every control is clamped to its documented range in the reference AND in prepare(), so the two
// roads agree on any setting; hz and f0Hz are floored at kMinHz (a NaN floors too), and Tilt's frequency
// ratio is held to ±kTiltMaxOctaves and its slope to ±kTiltMaxDbPerOctave, so no setting reaches inf; a NaN k
// reads as harmonic 1.

#include "dsp/vocode.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace feathers::spectral {

inline constexpr int kCurveBands = 64;
inline constexpr double kKeyTrackPivotHz = 261.6255653005986; // middle C
inline constexpr double kMaxResonanceGain = 3.0;              // resonance 1 adds 3.0 at the cutoff
inline constexpr double kResonanceWidthOctaves = 0.15;
/// **THE FILTER'S OPEN ENDS (Z7c/Z7d; filter cutoff seems to not go all the way to the top).** A Butterworth corner at
/// 20 kHz is still -3 dB there, so a Low Pass "all the way up" was not open. Over the last `kFilterOpenOctaves` before an end the
/// filter fades out in dB (its mix scaled by 1 - openness) and at the end it is off: exactly transparent. See `filterOpenness`.
inline constexpr double kFilterOpenLowHz = 20.0;
inline constexpr double kFilterOpenHighHz = 20000.0;
inline constexpr double kFilterOpenOctaves = 1.0 / 3.0;
/// Formant Shift's crossover blend: its TOTAL width in octaves, centred on the crossover. Below
/// crossover·2^(−width/2) the low ratio holds exactly, above crossover·2^(+width/2) the high one.
inline constexpr double kFormantBlendWidthOctaves = 2.0 / 3.0;
inline constexpr double kMinHz = 1e-3;          ///< the floor on every hz and f0Hz an entry point is given
inline constexpr double kTiltMaxOctaves = 12.0; ///< Tilt's frequency ratio is clamped to [2^-12, 2^12]
/// Tilt's slope is clamped to ±48 dB/oct, so with the ratio clamp its factor is within 2^±96: the batch's
/// G·T can neither overflow to inf nor underflow to 0 before the compensation scale is applied.
inline constexpr double kTiltMaxDbPerOctave = 48.0;
inline constexpr float kFlushGain = 1e-7f;      ///< −140 dB: a batch gain below this is written as exactly 0

/// THE VOWEL'S SHAPE (the Formant operator, design §3.2). PhiVox gives the vowel set its F1 and F2 (dsp/vox_anchors.h)
/// but no widths or levels, so these are the operator's own. tuned by ear: change them freely, nothing else
/// depends on their values. Each formant is a peak of bandwidth kVowelBandwidthBaseHz + kVowelBandwidthPerHz · F (its −3 dB
/// width in Hz: ~80 Hz at 300 Hz, ~270 Hz at 2.2 kHz), standing kVowelPeakGain · its level above a floor of 1, so with
/// the levels below F1's peak stands at 1 + 12 · 0.62 = 8.44 (+18.5 dB) and F2's PEAK TERM is 0.6 of F1's: 12 · 0.372
/// above the floor, so its peak stands at 1 + 4.46 = 5.46 (+14.7 dB), not at 0.6 of F1's total (PhiVox's levels are
/// 0.62 and 0.62 · balance, here at a balance of 0.6).
inline constexpr double kVowelBandwidthBaseHz = 50.0;
inline constexpr double kVowelBandwidthPerHz = 0.1;
inline constexpr double kVowelF1Level = 0.62;
inline constexpr double kVowelF2Level = 0.62 * 0.6;
inline constexpr double kVowelPeakGain = 12.0;
/// A vowel's centres are held to [1, kVowelMaxHz] on both roads, so no setting makes a peak inf/inf.
inline constexpr double kVowelMaxHz = 100000.0;

/// **THE BELL (F2, additive design §3.6)**: a peaking EQ, gain(hz) = A^b(d) with A = 10^(Gain/20), d the octaves from the
/// centre and b(d) = 1 / (1 + |d/h|^(2n)). Resonance is the bandwidth (its −half-dB width, geometric from kBellWidestOctaves
/// at 0 to kBellNarrowestOctaves at 1; h is half of it), Slope the skirt's shape (n = bellSkirtOrder, 1 at the default Slope), Gain ±kBellMaxDb.
inline constexpr double kBellMaxDb = 24.0;
inline constexpr double kBellWidestOctaves = 3.0;
inline constexpr double kBellNarrowestOctaves = 1.0 / 6.0;
inline constexpr double kBellClassicSlope = 0.25; ///< the filters' default Slope: a Bell's order is 1 there (bellSkirtOrder)
inline constexpr double kBellSkirtOctaves = 4.0;  ///< Slope 1 is order 2^4 = 16

enum class FilterMode : uint8_t { LowPass, HighPass, BandPass, Notch, Bell };
inline constexpr int kFilterModeCount = 5;

struct FilterOp {
	bool on{false};
	FilterMode mode{FilterMode::LowPass};
	float cutoffHz{20000.0f}; ///< >= 1 Hz
	float resonance{0.0f};    ///< 0..1. Ignored by Notch (its control is greyed there: the honesty rule); Bell's bandwidth.
	float slope{0.25f};       ///< 0..1 -> order 1..16, i.e. 6..96 dB/oct, continuous.
	float keyTrack{0.0f};     ///< 0..1: at 1 the cutoff moves with the note, pivoting at middle C.
	/// 0..1: how much of the filter acts — its gain to this power, so its curve fades in dB. 1 is the filter (to the bit);
	/// the per-sound drawer's morph sets it to the summed weight of the sounds whose filter is on (Synth::drawerFor). A mix
	/// of 0 is no filter: the morph switches the filter off rather than ask for that.
	float mix{1.0f};
	float gainDb{0.0f}; ///< Bell only, ±kBellMaxDb: at 0 a Bell is no filter (exactly transparent)
	friend bool operator==(const FilterOp&, const FilterOp&) = default;
};

struct CombOp {
	bool on{false};
	float spacing{2.0f}; ///< in harmonics, >= 1: a notch every `spacing` harmonics
	float depth{0.0f};   ///< 0..1
	float phase{0.0f};   ///< 0..1: slides the notches along the harmonics (any value; it wraps)
	friend bool operator==(const CombOp&, const CombOp&) = default;
};

struct TiltOp {
	bool on{false};
	float dbPerOctave{0.0f}; ///< -48..+48 (kTiltMaxDbPerOctave)
	float pivotHz{1000.0f};  ///< >= 1 Hz
	friend bool operator==(const TiltOp&, const TiltOp&) = default;
};

struct OddEvenOp {
	bool on{false};
	float balance{0.0f}; ///< -1 odd only .. 0 both .. +1 even only
	friend bool operator==(const OddEvenOp&, const OddEvenOp&) = default;
};

struct FormantShiftOp {
	bool on{false};
	float lowSemitones{0.0f};
	float highSemitones{0.0f};
	float crossoverHz{1000.0f}; ///< >= 1 Hz
	bool link{true};            ///< linked: lowSemitones shifts both bands
	friend bool operator==(const FormantShiftOp&, const FormantShiftOp&) = default;
};

/// THE FORMANT OPERATOR: a vowel's two peaks laid on top of the spectrum, FIXED IN Hz (a vowel survives transposition).
/// Its gain is V(hz)^depth, V the floor of 1 plus the two peaks, so Depth fades the vowel in dB and Depth 0 is no vowel.
/// LEVEL-COMPENSATED LIKE TILT in the batch (lead's call, 2026-10-12, revisable): Depth changes the vowel, not the
/// loudness. The vowel is given in Hz: the walk along PhiVox's anchors (vowelFormants) and the per-sound morph are the
/// caller's (Synth::drawerFor).
struct VowelOp {
	bool on{false};
	float f1Hz{300.0f};  ///< >= 1 Hz
	float f2Hz{2200.0f}; ///< >= 1 Hz
	float depth{0.0f};   ///< 0..1
	friend bool operator==(const VowelOp&, const VowelOp&) = default;
};

/// THE PARTIALS OPERATOR (design §3.2): how many partials the additive voice renders, a timbre control and the CPU knob.
/// NOT AN AMPLITUDE OPERATOR, so nothing in this file evaluates it: the voice caps its partial loop at ⌈count⌉ and plays
/// the top partial at the fraction count − ⌊count⌋, so a moving count never steps (OscVoice, pitchPartials). Per sound,
/// blended c = Σ w·count with an off sound counting as kMaxCount (lead's call, 2026-10-12, revisable).
/// **ITS CULL SCHEME (backlog OH/OI 2026-10-19: Partials is a mode cell, the dropdown the scheme)**, in the order of
/// the Choice both surfaces share (params.h kSpectralPartialsModeNames, the device's kPartialsModeNames; stored by label, so
/// OI's schemes append): Peaks keeps the N LOUDEST partials of the sound (the Spectral device's Peaks on any audio), Harmonics
/// the LOWEST N (the operator as it always was). Peaks renders every partial below Nyquist and silences the rest, so it is a
/// timbre control and not the CPU knob Harmonics is.
enum class PartialsScheme : uint8_t { Peaks, Harmonics, Centers };
struct PartialsOp {
	static constexpr float kMinCount = 1.0f;   ///< the voice's floor, and the control's (OH: down to 1 on both surfaces)
	static constexpr float kMaxCount = 320.0f; ///< partials::kMaxPartials: every partial, the operator's no-op
	bool on{false};
	float count{kMaxCount};
	/// HOW MUCH OF THE CULL IS PEAKS, 0..1 (P1b, by design: a morph between schemes never steps): 0 is Harmonics alone,
	/// 1 Peaks alone, between them the two keep masks blended by it (pitchPartials). One sound gives exactly 0 or 1.
	float peaks{0.0f};
	/// HOW MUCH IS CENTERS (OI's first scheme), 0..1, blended as Peaks is (Harmonics is what the two shares
	/// leave): the fundamental and a few of 2..7 at `centersSet` through partials::sparseKeep, the terrain sparse field's generator.
	/// The count is not read by Centers.
	float centers{0.0f};
	float centersSet{0.0f};
	/// **HOW FAR THE CULLED PARTIALS ARE TURNED DOWN, 0..1** (the Spectral device's Partials Depth, backlog OF 2026-10-26: "we probably
	/// need an amount knob"): a partial the scheme keeps at weight w plays at 1 − depth·(1 − w), so 1 (the default) is the cull as it always
	/// was, to the bit, and 0 keeps every partial whatever the count. Below 1 the loop runs to Nyquist, as Peaks' does. A NaN depth is 1.
	float depth{1.0f};
	friend bool operator==(const PartialsOp&, const PartialsOp&) = default;
};

/// THE STRETCH OPERATOR (design §3.2, Razor's "Dissonance"): piano-style inharmonicity on top of the terrain's off-grid,
/// partial i (harmonic k = i + 1) at its ratio × √(1 + B·k²). A FREQUENCY operator, so the voice runs it (pitchPartials)
/// and nothing in this file does; THE SAME IN BOTH EARS (design §1.4). Its amplitude travels with the
/// partial (lead's call, 2026-10-12, revisable). The control is an Amount 0..1 onto B = kMaxB · amount³ (a piano's B is
/// ~1e-4..1e-3: Amount 0.2..0.45), and the morph blends B itself by weight × mix, so an off sound pulls toward harmonic.
struct StretchOp {
	static constexpr double kMaxB = 0.01; ///< Amount 1: harmonic 8 sits ~428 cents sharp, harmonic 32 ~2.1 octaves
	static constexpr double bOf(double amount) {
		const double a = amount > 0.0 ? (amount < 1.0 ? amount : 1.0) : 0.0;
		return kMaxB * a * a * a;
	}
	bool on{false};
	float b{0.0f}; ///< 0..kMaxB
	friend bool operator==(const StretchOp&, const StretchOp&) = default;
};

/// THE SHIFT OPERATOR (design §3.2): a frequency shifter, every partial moved by the same Hz, so the spectrum goes
/// inharmonic. A FREQUENCY operator, run by the voice (pitchPartials); THE SAME IN BOTH EARS (design §1.4);
/// amplitudes travel with their partials. A partial shifted toward 0 Hz FADES OUT, linearly in Hz from kLowFadeFullHz
/// down to kLowFadeZeroHz, and is silent at or below 0 Hz: no through-zero fold (a negative frequency would be the same
/// partial reflected, and its phase run backwards). Hz, not harmonics: Shift does not track the keyboard.
struct ShiftOp {
	static constexpr float kMaxHz = 1000.0f;
	static constexpr float kLowFadeFullHz = 20.0f; ///< at and above: full gain
	static constexpr float kLowFadeZeroHz = 5.0f;  ///< at and below: silent
	bool on{false};
	float hz{0.0f}; ///< −kMaxHz..kMaxHz
	friend bool operator==(const ShiftOp&, const ShiftOp&) = default;
};

/// THE DAMP OPERATOR (design §3.2): higher partials decay faster over the note, a pluck's physics. NOT AN AMPLITUDE
/// OPERATOR OF THIS FILE: its gain moves every block with the note's age, so the voice runs it (OscVoice's damp
/// accumulator), AFTER the drawer's gains and their compensation, so the compensation never lifts the decay back up
/// (lead's call, 2026-10-12, revisable). A partial at hz falls 60 dB in 1 / (rate · (hz / kReferenceHz)^tilt) seconds:
/// `rate` is the T60 at 1 kHz inverted, so the morph blends it linearly by weight and 0 is no decay at all.
struct DampOp {
	static constexpr double kReferenceHz = 1000.0;
	static constexpr float kMaxTilt = 3.0f;
	bool on{false};
	float rate{0.5f}; ///< 1 / (T60 at 1 kHz), per second, >= 0
	float tilt{1.0f}; ///< 0..kMaxTilt: the decay rate ∝ (hz / 1 kHz)^tilt; 0 decays every partial alike
	friend bool operator==(const DampOp&, const DampOp&) = default;
};

constexpr std::array<float, kCurveBands> unityCurve() {
	std::array<float, kCurveBands> a {};
	for (float& g : a) {
		g = 1.0f;
	}
	return a;
}

struct DrawnCurve {
	bool on{false};
	std::array<float, kCurveBands> gain{unityCurve()}; ///< band b is harmonic b + 1; a gain per band, >= 0
	friend bool operator==(const DrawnCurve&, const DrawnCurve&) = default;
};

struct ShaperParams {
	bool enabled{true}; ///< the master switch (design §3.1); stored state, not a host param
	FilterOp filter{};
	/// THE MORPH'S OTHER FILTER MODES (the per-sound drawer, lead's call 2026-10-10): sounds that disagree about the mode
	/// are grouped by it, each group one filter at its own blended settings and mix, and the gains MULTIPLY — so a 50/50
	/// LP/HP morph is both filters at half depth and nothing steps. Off by default; `filter` alone is every older caller.
	std::array<FilterOp, kFilterModeCount - 1> moreFilters{};
	/// **THE SECOND FILTER (F2)** and its own morph groups: off by default, so every older caller is filter 1 alone, to the bit.
	FilterOp filter2{};
	std::array<FilterOp, kFilterModeCount - 1> moreFilters2{};
	/// ROUTING: 0 Series (the gains multiply), 1 Parallel (`parallelGain`); between, the morph's blend in dB, Series^(1−r)·Parallel^r.
	float parallel{0.0f};
	/// Each filter's PRESENCE, 0..1: the morph's share of the blend whose filter is on (1 for a device or one sound). In Parallel a
	/// side leaves the sum by its presence rather than turning into a wire (additive design §3.6). Series does not read it.
	std::array<float, 2> filterPresence{1.0f, 1.0f};
	CombOp comb{};
	TiltOp tilt{};
	OddEvenOp oddEven{};
	FormantShiftOp formantShift{};
	DrawnCurve curve{};
	VowelOp vowel{};
	/// The voice's operators, read by OscVoice and not by prepare()/gains(): they change which partials render, not their
	/// gains. The left (lead) ear's are read for both ears (design §3.2, "the later five").
	PartialsOp partialCount{};
	DampOp damp{};
	StretchOp stretch{};
	ShiftOp shift{};
	/// THE VOCODER (OE, additive design §9): the sidechain's envelope read at each partial, after the gains and before Damp.
	VocodeOp vocode{};
	/// THE AMPLITUDE OPERATORS' PARAMS ALONE: these params with the voice's own operators at rest, which is what GainCache keys
	/// prepare() on, so a modulated Count, Damp, Stretch or Shift never re-prepares the drawer (prepare() does not read them;
	/// Stretch and Shift reach the factors through the frequencies, which GainCache compares on their own).
	ShaperParams shaping() const {
		ShaperParams q = *this;
		q.partialCount = {};
		q.damp = {};
		q.stretch = {};
		q.shift = {};
		q.vocode = {};
		return q;
	}
	friend bool operator==(const ShaperParams&, const ShaperParams&) = default;
};

// THE CONTRACT ON k AND hz. `k` is THE PARTIAL'S HARMONIC INDEX: an integer for every partial the additive
// engine has, INCLUDING an off-grid one (a drifted partial is still "harmonic 7"). `hz` is its ACTUAL frequency.
// The harmonic-indexed operators (Comb, Odd/Even, the drawn curve) read k; the frequency operators (Filter,
// Tilt, Formant Shift) read hz. A caller with no note (the Spectral effect device) passes f0Hz =
// kKeyTrackPivotHz, which makes key track neutral, and k = hz / f0Hz.

/// The amplitude operators' product for a partial at harmonic index k and frequency hz, of a note at f0Hz.
/// Tilt and the vowel here are UNCOMPENSATED: one value cannot see the spectrum's power (the batch gains() compensates).
float gain(const ShaperParams& p, double k, double hz, double f0Hz);

/// Formant Shift: the ratio the envelope is slid by at hz. A partial at hz reads the envelope at hz / ratio.
double formantRatio(const ShaperParams& p, double hz);

/// **HOW OPEN THE FILTER IS, 0..1** (Z7d): 0 away from an open end, rising over the last `kFilterOpenOctaves` to 1 at it, where
/// the filter is off. Low Pass opens at the top and High Pass at the bottom, reached by EITHER road — the control (`log2Control`,
/// the cutoff before Key Track) or the corner Key Track moves (`log2Corner`) — so a knob at its end and a handle at the plot's
/// edge both open it; Notch opens at BOTH ends (Z9b). Band Pass has no open end (0): it passes the band at the edge. Within
/// 1e-5 of 1 it IS 1: a control stored as a float sits a few 1e-7 of an octave from the double bound.
double filterOpenness(FilterMode mode, double log2Control, double log2Corner);
/// THE SAME WITH A BELL'S GAIN (F2): a Bell is open exactly at 0 dB, wherever its centre is; every other mode ignores `gainDb`.
double filterOpenness(FilterMode mode, double log2Control, double log2Corner, double gainDb);
/// The same for an op, its corner tracked to f0Hz.
double filterOpenness(const FilterOp& f, double f0Hz);

/// **THE PARALLEL SUM OF TWO MAGNITUDE MASKS** (F2, additive design §3.6): the sum, held to unity or to the louder path's own
/// boost — min(g1 + g2, max(1, g1, g2)). Flat at an LP/HP crossover, two band-passes two bands at unity.
inline double parallelGain(double g1, double g2) {
	return std::min(g1 + g2, std::max({ 1.0, g1, g2 }));
}
/// THE ROUTING'S COMBINATION of the two filters' faded products s1, s2 (each its filters' gains to their mixes), at presences
/// p1, p2 and routing r (ShaperParams::parallel): Series^(1−r) · Parallel^r, Parallel = s1^(1−p2)·s2^(1−p1)·P(G1, G2)^(p1·p2) with
/// G = s^(1/p) each side's gain at full presence. At r = 0 it is s1·s2; at p1 = p2 = 1 Parallel is P(s1, s2).
double routedFilterGain(double s1, double s2, double p1, double p2, double r);
/// Bell's half bandwidth in octaves for a Resonance (clamped to 0..1).
double bellHalfWidthOctaves(double resonance);
/// **BELL'S SKIRT ORDER n FOR A SLOPE** (decided 2026-10-19: a fresh filter switched to Bell is a standard peaking EQ):
/// n = 2^(kBellSkirtOctaves · (slope − kBellClassicSlope) / (1 − kBellClassicSlope)), so the DEFAULT Slope gives n = 1, the classic
/// bell; below it the skirts soften (n ≈ 0.4 at Slope 0), above it the top flattens (n = 16 at Slope 1, every mode's steepest).
double bellSkirtOrder(double slope);

/// Each operator alone, so a case can hold one at a time against its own restatement.
float filterGain(const FilterOp& f, double hz, double f0Hz);
/// (The SHAPE alone: the open end is applied where the mix is, by `gain` and `prepare`.)
float combGain(const CombOp& c, double k);
float tiltGain(const TiltOp& t, double hz); ///< uncompensated: the shape of Tilt, not its level
float oddEvenGain(const OddEvenOp& o, double k);
float curveGain(const DrawnCurve& c, double k);
float vowelGain(const VowelOp& v, double hz); ///< uncompensated: the shape of the vowel, not its level

/// Everything that depends only on the params and the note, computed ONCE per voice per control block (and only when
/// the params or f0 moved, on the voice's road: GainCache), clamps applied. A prepared shaper that is the identity is
/// flagged, so the batch writes exactly 1.0f.
///
/// GROUPED BY WHAT EACH FACTOR READS, because the groups are GainCache's keys: the harmonic group's factor reads k
/// alone, the filter's and Tilt's read hz. AN OPERATOR THAT IS OFF KEEPS ITS GROUP'S DEFAULTS, whatever its settings
/// say, so moving a control of a switched-off operator changes no key and costs nothing.
struct PreparedShaper {
	bool identity{true};        ///< no amplitude operator acts (or the drawer is disabled)
	bool formantIdentity{true}; ///< Formant Shift is off (or the drawer is disabled)

	struct Filter {
		bool on{false};
		FilterMode mode{FilterMode::LowPass};
		double log2Cutoff{0.0};   ///< log2 of the key-tracked cutoff
		double twoOrder{2.0};     ///< 2·order: the exponent's slope per octave
		double resonanceAmp{0.0}; ///< kMaxResonanceGain · resonance; 0 for Notch and Bell
		double mix{1.0};          ///< the gain's exponent (FilterOp::mix, clamped to [0, 1])
		double bellLog2Gain{0.0}; ///< Bell: log2 A (A its peak gain)
		double bellInvHalf{1.0};  ///< Bell: 1 / its half bandwidth in octaves
		friend bool operator==(const Filter&, const Filter&) = default;
	} filter;
	/// The morph's further modes, packed: [0, moreFilterCount) are on. Their gains multiply into the filter group's.
	std::array<Filter, 2 * kFilterModeCount - 1> moreFilters{};
	int moreFilterCount{0};
	/// **THE SECOND SIDE (F2)**: moreFilters[moreFilterCount, moreFilterCount + side2Count) are filter 2's, routed against the
	/// first side's by `parallel`. 0 whenever only one filter acts — prepare() then packs whichever side acts into the first, so
	/// one acting filter is the old road wherever it came from.
	int side2Count{0};
	double parallel{0.0};               ///< ShaperParams::parallel, clamped to [0, 1]; read only while side2Count > 0
	double presence1{1.0}, presence2{1.0}; ///< ShaperParams::filterPresence, held to (0, 1]; read only in Parallel
	/// The one filter alone, with no further mode and no second side: the road that shares its log2(hz) with Tilt.
	bool singleFilter() const { return filter.on && moreFilterCount == 0 && side2Count == 0; }
	/// The filter group's key: the primary, every further mode and the routing, as GainCache compares them.
	bool sameFilterGroup(const PreparedShaper& o) const {
		if (!(filter == o.filter) || moreFilterCount != o.moreFilterCount || side2Count != o.side2Count) {
			return false;
		}
		if (side2Count > 0 && (parallel != o.parallel || presence1 != o.presence1 || presence2 != o.presence2)) {
			return false;
		}
		for (int i = 0; i < moreFilterCount + side2Count; ++i) {
			if (!(moreFilters[static_cast<std::size_t>(i)] == o.moreFilters[static_cast<std::size_t>(i)])) {
				return false;
			}
		}
		return true;
	}

	/// Comb, Odd/Even and the drawn curve: the operators that read the harmonic index only.
	struct Harmonic {
		bool combOn{false};
		double combInvSpacing{0.5};
		double combPhase{0.0}; ///< the control's phase reduced to [0, 1]: the comb is periodic in it
		double combDepth{0.0};
		bool oddEvenOn{false};
		double oddGain{1.0};
		double evenGain{1.0};
		bool curveOn{false};
		std::array<float, kCurveBands> curve{unityCurve()};
		bool any() const { return combOn || oddEvenOn || curveOn; }
		friend bool operator==(const Harmonic&, const Harmonic&) = default;
	} harmonic;

	struct Tilt {
		bool on{false};
		double exponent{0.0}; ///< dbPerOctave / 20·log10(2): the gain's exponent per octave
		double log2Pivot{0.0};
		friend bool operator==(const Tilt&, const Tilt&) = default;
	} tilt;

	/// The vowel (Formant): reads hz, and is compensated with Tilt. Off when its operator is, or at Depth 0, so a vowel
	/// at no depth is no group work and no compensation (the identity holds structurally, not by a pow landing on 1).
	struct Vowel {
		bool on{false};
		double f1{0.0}, f2{0.0};       ///< centres, Hz, held to [1, kVowelMaxHz]
		double inv1{0.0}, inv2{0.0};   ///< 1 / bandwidth, per Hz
		double depth{0.0};             ///< the exponent, (0, 1]
		friend bool operator==(const Vowel&, const Vowel&) = default;
	} vowel;
	/// Whether the batch compensates the spectrum's level: Tilt or the vowel is on.
	bool compensated() const { return tilt.on || vowel.on; }

	bool formantConstant{true}; ///< linked, or both bands equal: one ratio everywhere
	float formantConstantRatio{1.0f};
	double formantLowSemitones{0.0};
	double formantHighSemitones{0.0};
	double formantLog2Crossover{0.0};
};

PreparedShaper prepare(const ShaperParams& p, double f0Hz);

/// out[i] = the amplitude operators' gain for a partial at harmonic index k[i], frequency hz[i], whose amplitude
/// BEFORE the drawer is base[i]. A gain below kFlushGain is written as exactly 0.0f (after Tilt's compensation),
/// so the bank can skip that partial.
///
/// TILT IS LEVEL-COMPENSATED HERE : it changes colour, not loudness. With G_i every other
/// operator's gain and T_i Tilt's factor, out_i = G_i·T_i·c where c = sqrt(Σ(base_i·G_i)² / Σ(base_i·G_i·T_i)²),
/// so the spectrum's power after Tilt equals its power before it (c = 1 when that ratio is not finite). This is
/// why the batch needs `base` and the whole spectrum in one call; with Tilt off, base is not read.
/// THE VOWEL IS COMPENSATED WITH IT (lead's call, 2026-10-12, revisable): T_i is Tilt's factor times the vowel's, so
/// Depth changes the vowel, not the loudness. With neither on, base is not read.
///
/// THE CONTRACT ON base AND count. base[i] is the amplitude the bank WILL RENDER for partial i before the drawer,
/// already including the Nyquist fade and Formant Shift's envelope re-read (so formantRatios() runs BEFORE base is
/// built), and count covers only the partials that will render. Otherwise a positive Tilt pushes power into
/// partials culled at Nyquist and c quietens the audible spectrum to pay for them.
///
/// Safe in place: out may alias base, k or hz (each is read at i before out[i] is written).
void gains(const PreparedShaper& s, const float* k, const float* hz, const float* base, int count, float* out);

/// out[i] = formantRatio at hz[i]; exactly 1.0f everywhere when Formant Shift is off or the drawer disabled.
/// Safe in place: out may alias hz.
void formantRatios(const PreparedShaper& s, const float* hz, int count, float* out);

// ---- The kernels gains() and GainCache share ---------------------------------------------------------------
//
// gains() IS THESE, IN THIS ORDER: one factor per group per partial, then their product, compensated for Tilt and
// flushed. Two partials per vector (SSE2 doubles), log2, exp2 and cos by polynomials whose truncation is ~1e-10
// relative or below (spectral_shaper.cpp states each), so each factor is the reference to ~1e-9; the tests hold it to
// the reference per operator in dB. Each kernel is ELEMENTWISE IN VALUE: partial i's factor reads only k[i] or hz[i].
// The one decision taken per PAIR is the filter's resonance bump, skipped when both lanes are >= 2 octaves from the
// cutoff; a skipped bump is below 2^−94 of that lane's gain, under half an ulp of a double, so adding it could not
// change the result and the pairing cannot show. That is what lets GainCache keep a group's factors while another
// group's are recomputed, or apply a shorter count than it computed, and still equal gains() to the bit.

/// Comb × Odd/Even × the drawn curve at harmonic index k[i]. Call only when h.any().
void harmonicFactors(const PreparedShaper::Harmonic& h, const float* k, int count, float* out);
/// The same at k = i + 1, the voice's contract (partial i is harmonic i + 1), without a k array.
void harmonicFactorsAtIndex(const PreparedShaper::Harmonic& h, int count, float* out);
/// Filter's gain at hz[i], resonance included, floored at 0, to its mix. DOUBLE, because a steep filter's gain falls far below a
/// float's range while Tilt's compensation may still lift the product into hearing. Call only when f.on.
void filterFactors(const PreparedShaper::Filter& f, const float* hz, int count, double* out);
/// THE FILTER GROUP'S WHOLE FACTOR: `s.filter`'s and every further mode's (`moreFilters`), multiplied; with a second side,
/// the two sides' products routed (routedFilterGain). Call only when s.filter.on.
void filterGroupFactors(const PreparedShaper& s, const float* hz, int count, double* out);
/// Tilt's UNCOMPENSATED factor at hz[i] (within 2^±96: a float holds it). Call only when t.on.
void tiltFactors(const PreparedShaper::Tilt& t, const float* hz, int count, float* out);
/// Both of the above in one pass, sharing each partial's log2(hz): the same values, to the bit, for half the logs.
void filterTiltFactors(const PreparedShaper::Filter& f, const PreparedShaper::Tilt& t, const float* hz, int count,
                       double* filterOut, float* tiltOut);
/// The vowel's UNCOMPENSATED factor V(hz[i])^depth (>= 1, at most ~13). Call only when v.on.
void vowelFactors(const PreparedShaper::Vowel& v, const float* hz, int count, float* out);
/// The product G·T of the factors (a null array is a group that is off, factor 1), where the compensated factor T is
/// Tilt's t times the vowel's v. With both null, out[i] is the final flushed gain and base is not read; otherwise out[i]
/// is the UNCOMPENSATED float(G·T), and the two powers Σ(base·G)² and Σ(base·G·T)² are ADDED to powerBefore and
/// powerAfter, for compensate() once the whole spectrum is in.
void factorProduct(const float* h, const double* f, const float* t, const float* v, const float* base, int count,
                   float* out, double& powerBefore, double& powerAfter);
/// Tilt's level compensation: out[i] ·= c = sqrt(powerBefore / powerAfter) (1 when not finite), then flushed.
void compensate(float* out, int count, double powerBefore, double powerAfter);
/// out[i] = 2^(scale·q[i]), the exponent held to [−1020, 1020], by the kernels' vector exp2 (~1e-10 relative): Damp's per-block
/// factors, which move with the pitch every control block under vibrato (spectral-ops Damp review, 2026-10-12).
void exp2Scaled(const float* q, double scale, int count, float* out);

// ---- The voice's road, with memory -------------------------------------------------------------------------

/// THE DRAWER ON A HELD NOTE COSTS ~NOTHING PER BLOCK (backlog LY: the drawer cost 2.46× the drawer-off voice when every
/// control block recomputed every factor). One per oscillator, for partials 0..N−1 at harmonic index i + 1.
///
/// THE KEYS ARE VALUES, NEVER ADDRESSES (CLAUDE.md, the ABA rule): the ShaperParams and f0 that prepare() read (by
/// value), each group's PREPARED values (so a group recomputes exactly when something it reads moved, and a group that
/// is off has nothing to move), the hz of every partial (bitwise: a factor is a deterministic function of those bits),
/// and the count each group covers. A moving cutoff recomputes the filter's factors only; a glide recomputes the
/// filter's and Tilt's (they read hz) and keeps Comb/Odd/Even/curve; a static drawer recomputes nothing and pays only
/// the product, which Tilt's compensation needs anyway because it reads each side's base.
///
/// A PREPARED SHAPER THAT IS THE IDENTITY DOES NO GROUP WORK AT ALL, and the voice does not call apply() for it: a
/// neutral drawer stays the engine's own sound to the bit and costs one params compare per block.
template <int N>
class GainCache {
public:
	/// Once per control block, before apply(): prepares when p or f0Hz changed, then (unless the result is the identity)
	/// brings each group's factors up to date for partials [0, count) at frequencies hz.
	void update(const ShaperParams& p, double f0Hz, const float* hz, int count) {
		if (count < 0) {
			count = 0;
		}
		if (count > N) {
			count = N;
		}
		// A NaN param (or f0) never compares equal, so it re-prepares every block, and a group whose PREPARED value is NaN
		// (a NaN Comb phase) recomputes every block too. That costs time, never correctness: the output is what gains()
		// gives for those inputs (the non-finite case in test_spectral_shaper), and the clamps keep the other groups' keys
		// finite, so they stay cached.
		const ShaperParams key = p.shaping(); // the voice's own operators move nothing here
		if (!primed_ || !(key == params_) || !(f0Hz == f0_)) {
			const PreparedShaper next = prepare(key, f0Hz);
			++work_.prepares;
			if (!(next.harmonic == prepared_.harmonic)) {
				harmonicCount_ = 0;
			}
			if (!next.sameFilterGroup(prepared_)) {
				filterCount_ = 0;
			}
			if (!(next.tilt == prepared_.tilt)) {
				tiltCount_ = 0;
			}
			if (!(next.vowel == prepared_.vowel)) {
				vowelCount_ = 0;
			}
			prepared_ = next;
			params_ = key;
			f0_ = f0Hz;
			primed_ = true;
		}
		count_ = count;
		if (prepared_.identity) {
			return;
		}
		const bool readsHz = prepared_.filter.on || prepared_.tilt.on || prepared_.vowel.on;
		if (readsHz && !(count <= hzCount_ && sameBits(hz, hz_, count))) {
			for (int i = 0; i < count; ++i) {
				hz_[i] = hz[i];
			}
			hzCount_ = count;
			filterCount_ = 0;
			tiltCount_ = 0;
			vowelCount_ = 0;
		}
		if (prepared_.harmonic.any() && count > harmonicCount_) {
			harmonicFactorsAtIndex(prepared_.harmonic, count, h_);
			harmonicCount_ = count;
			++work_.harmonic;
		}
		if (prepared_.singleFilter() && count > filterCount_ && prepared_.tilt.on
		    && count > tiltCount_) {
			filterTiltFactors(prepared_.filter, prepared_.tilt, hz_, count, f_, t_);
			filterCount_ = count;
			tiltCount_ = count;
			++work_.filter;
			++work_.tilt;
		}
		if (prepared_.filter.on && count > filterCount_) {
			filterGroupFactors(prepared_, hz_, count, f_);
			filterCount_ = count;
			++work_.filter;
		}
		if (prepared_.tilt.on && count > tiltCount_) {
			tiltFactors(prepared_.tilt, hz_, count, t_);
			tiltCount_ = count;
			++work_.tilt;
		}
		if (prepared_.vowel.on && count > vowelCount_) {
			vowelFactors(prepared_.vowel, hz_, count, v_);
			vowelCount_ = count;
			++work_.vowel;
		}
	}

	const PreparedShaper& prepared() const { return prepared_; }

	/// out[i] = gains(prepared(), {1, 2, ...}, hz, base, count, out)[i] for the last update's hz, TO THE BIT. count is at
	/// most the last update's. Not for an identity shaper (its gain is exactly 1, and the voice does not ask).
	void apply(const float* base, int count, float* out) const {
		// A count past the last update's has no factors: those partials are written as exactly 1 (no drawer), so out is
		// never left partly unwritten; within the bound, the contract above.
		assert(count <= count_);
		if (count > count_) {
			std::fill(out + count_, out + count, 1.0f);
			count = count_;
		}
		++work_.applies;
		double powerBefore = 0.0;
		double powerAfter = 0.0;
		factorProduct(prepared_.harmonic.any() ? h_ : nullptr, prepared_.filter.on ? f_ : nullptr,
		              prepared_.tilt.on ? t_ : nullptr, prepared_.vowel.on ? v_ : nullptr, base, count, out, powerBefore,
		              powerAfter);
		if (prepared_.compensated()) {
			compensate(out, count, powerBefore, powerAfter);
		}
	}

	/// TEST SEAM: how many times prepare() ran, each group was recomputed and apply() was called, since construction.
	struct Work {
		uint32_t prepares{0};
		uint32_t harmonic{0};
		uint32_t filter{0};
		uint32_t tilt{0};
		uint32_t vowel{0};
		uint32_t applies{0}; ///< apply() calls: a side the voice took the product for
	};
	Work workForTest() const { return work_; }

private:
	static bool sameBits(const float* a, const float* b, int n) {
		return std::memcmp(a, b, sizeof(float) * static_cast<std::size_t>(n)) == 0;
	}

	bool primed_{false};
	ShaperParams params_{};
	double f0_{0.0};
	PreparedShaper prepared_{};
	int count_{0};        ///< the last update's count
	int hzCount_{0};      ///< hz_ holds the frequencies of partials [0, hzCount_)
	int harmonicCount_{0}; ///< h_ holds prepared_.harmonic's factors for [0, harmonicCount_); 0: none
	int filterCount_{0};   ///< f_ holds prepared_.filter's at hz_ for [0, filterCount_)
	int tiltCount_{0};     ///< t_ holds prepared_.tilt's at hz_ for [0, tiltCount_)
	int vowelCount_{0};    ///< v_ holds prepared_.vowel's at hz_ for [0, vowelCount_)
	alignas(16) float hz_[N] {};
	alignas(16) float h_[N] {};
	alignas(16) double f_[N] {};
	alignas(16) float t_[N] {};
	alignas(16) float v_[N] {};
	mutable Work work_{}; ///< mutable for apply()'s count: a test seam, one voice's, never shared across threads
};

} // namespace feathers::spectral
