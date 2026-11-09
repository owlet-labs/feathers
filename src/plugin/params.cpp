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

#include "plugin/state_block_order.h"
#include "plugin/params.h"

#include "dsp/vox_anchors.h"
#include "dsp/warp.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

namespace feathers {

namespace {

/// A RELAXED LOAD OF A CELL REACHED THROUGH A `const float*` — `std::atomic_ref<const T>` is C++26 and
/// Apple's libc++ lacks it: macOS CI run 35154217625 stopped here, the second time this class of break
/// shipped (the rack bridge's `relaxedLoad` is the first, run 34093203285). So the read goes through a
/// non-const ref. SOUND because no cell these pointers reach is const-DEFINED: they are this object's
/// own `coreCells_`, written through `atomic_ref<float>`, or the engine's output cells, written the same way.
float relaxedLoadCell(const float& cell) noexcept {
	return std::atomic_ref<float>(const_cast<float&>(cell)).load(std::memory_order_relaxed);
}

/// **IS THIS INPUT'S SOURCE A PAIR NOW** — bit `pairBit` of the engine's `Slot::emitsPair`, read the way `relaxedLoadCell`
/// reads a cell (the word is not const-defined either). False for a source with no pair word (a host or per-voice one).
/// ONE `uint64_t` since 2026-11-07 (a device may declare up to `kMaxDeviceOutputs` = 64 outputs): it was a byte.
bool pairNow(const std::atomic<const uint64_t*>& pairs, const std::atomic<uint8_t>& bit) noexcept {
	const uint64_t* p = pairs.load(std::memory_order_relaxed);
	if (p == nullptr) {
		return false;
	}
	const uint64_t word = std::atomic_ref<uint64_t>(const_cast<uint64_t&>(*p)).load(std::memory_order_relaxed);
	return ((word >> bit.load(std::memory_order_relaxed)) & 1u) != 0;
}

constexpr uint32_t kContinuous = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE;
constexpr uint32_t kStepped = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;

/// log2 of the spectral drawer's Hz bounds and defaults, as literals: `std::log2` is not constexpr in the standard
/// (g++ folds it, clang refuses), and this table is constexpr. test_params pins each against std::log2.
constexpr double kLog2Of20 = 4.321928094887363;
constexpr double kLog2Of100 = 6.643856189774724;
constexpr double kLog2Of1k = 9.965784284662087;
constexpr double kLog2Of500 = 8.965784284662087;
constexpr double kLog2Of2k = 10.965784284662087;
constexpr double kLog2Of8k = 12.965784284662087;
constexpr double kLog2Of20k = 14.287712379549449;
/// Damp's Time bounds are log2 SECONDS: 50 ms here, and 20 s, which is kLog2Of20 above.
constexpr double kLog2Of0p05 = -4.321928094887363;

/// STEPPED, AND THE HOST MAY NOT DRAW A LANE ON IT.
///
/// The engine is not a value that can be swept. It selects which renderer builds the character
/// tables, so moving it is not a bigger version of moving a knob — `CharacterBank::buildOne` branches
/// on it and the bank REBUILDS. That was already true of internal modulation, which never reached
/// here: `kCoreModDestinations` is eight continuous params and stepped controls are excluded by rule,
/// so `modDestinationIndexOf(OscType) < 0` and always has been.
///
/// WHAT WAS STILL OPEN WAS THE OTHER ROAD, and it was open in every DAW: `kStepped` carries
/// `CLAP_PARAM_IS_AUTOMATABLE`, so a host could draw an automation lane on the engine and switch it
/// under a sounding note. This closes it.
///
/// THE DECISION IS ABOUT THE ROAD, NOT ABOUT A CLICK. A mid-note engine switch was measured at 1.951x
/// the waveform's own steepest sample-to-sample step, against 1.000x for a comparable mid-note
/// parameter move — so the step is real, and it is accepted: a click when the engine changes on the dropdown does not matter.
/// A hand that clicks the dropdown mid-chord has chosen to, and the step is
/// not what makes host automation wrong here.
///
/// What IS wrong is a host sweeping a RENDERER SELECTOR as though it were a value — repeatedly,
/// automatically, and without anybody choosing to. That is the road this closes.
///
/// NOT `CLAP_PARAM_IS_READONLY`: the editor writes this parameter, and readonly tells a host the
/// value cannot be changed at all rather than that IT may not change it.
constexpr uint32_t kSteppedNoAutomation = CLAP_PARAM_IS_STEPPED;

// Below this the master gain is silence rather than a very quiet signal, so the fader bottoms out
// where a fader should. Task 11 reads it through decibelsToGain(), which encodes the same rule.
constexpr double kMasterGainMinDb = -60.0;

/// The declared range of a Choice param, from its name list: 0 to one less than the number of
/// entries. Written this way so appending an entry widens the range with it — a name list that had
/// grown past a hard-coded maximum would be a menu entry the host could never select.
constexpr double lastIndexOf(std::span<const char* const> names) {
	return static_cast<double>(names.size() - 1);
}

// unitOf, fxRow and filterRow lived here. All three existed to seat a device's control declaration
// at a clap id, and the swallowing left them with no caller: a docked device's controls are its own
// state now, so nothing in the fx lane becomes a parameter of Feathers.
//
// THEY ARE DELETED RATHER THAN KEPT FOR A FUTURE TENANT, and the reason is that keeping them would
// have been invisible. constexpr implies inline, so -Wunused-function does not fire on any of them —
// dead code here is dead SILENTLY, which is worse than dead loudly. If an fx tenant ever really is a
// parameter of Feathers, this is eleven lines and it is in the history.

/// **ONE SOUND'S SPECTRAL DRAWER ROWS** (2026-10-10, additive design §3.2): its controls (kSpectralControlCount), in
/// SpectralControl's order at `spectralParam(slot, c)`. A macro because the table is a constexpr literal of
/// string-literal names, and the eight sounds differ only in their letter. Every operator's switch is OFF by default, so
/// a fresh sound's drawer is neutral; a control with an amount defaults where switching its operator on is audible
/// (Filter at 2 kHz, Comb at half depth), a bipolar one to its neutral middle. Hz are log2 (LogHertz), so a route's
/// amount is in octaves and a weighted mean of the values is a log-frequency blend. Each S control (§3.2) is `stereo`.
/// NO TILT PIVOT: Tilt is level-compensated, so a pivot only scales every partial by one constant the compensation
/// removes (the clamp at ±12 octaves is outside the audio band) — a Pivot control would change no sound, so the voice
/// keeps ShaperParams' 1 kHz.
#define FEATHERS_SPECTRAL_ROWS(L, l, slot)                                                                                  \
    {spectralParam(slot, SpectralControl::FilterOn), L " Spectral Filter", "Sound " L, 0.0, 1.0, 0.0, kStepped,          \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches this sound's spectral filter in: a magnitude curve over the partials, with no phase and no instability."}, \
    {spectralParam(slot, SpectralControl::FilterMode), L " Spectral Filter Mode", "Sound " L, 0.0,                       \
     lastIndexOf(kSpectralFilterModeNames), 0.0, kStepped, ParamUnit::Choice, kSpectralFilterModeNames, false,           \
     "Which partials the filter keeps: those below the cutoff, above it, around it, or all but those around it; or a "   \
     "Bell, which lifts or cuts those around it by its Gain."},                                                          \
    {spectralParam(slot, SpectralControl::FilterCutoff), L " Spectral Cutoff", "Sound " L, kLog2Of20, kLog2Of20k,         \
     kLog2Of2k, kContinuous, ParamUnit::LogHertz, {}, false, "Where the filter's curve turns.", nullptr, true},           \
    {spectralParam(slot, SpectralControl::FilterResonance), L " Spectral Resonance", "Sound " L, 0.0, 1.0, 0.0,           \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "A peak drawn at the cutoff. Notch has none; in Bell it is the width, from 3 octaves down to a sixth of one.",      \
     nullptr, true},                                                                                                     \
    {spectralParam(slot, SpectralControl::FilterSlope), L " Spectral Slope", "Sound " L, 0.0, 1.0, 0.25, kContinuous,     \
     ParamUnit::Percent, {}, false,                                                                                      \
     "How steeply the filter falls, continuously from gentle (6 dB/oct) to a brick wall (96 dB/oct); in Bell, how "      \
     "steep its sides are: a classic bell at the default, softer below, a flat-topped band above.", nullptr, true},     \
    {spectralParam(slot, SpectralControl::FilterKeyTrack), L " Spectral Key Track", "Sound " L, 0.0, 1.0, 0.0,            \
     kContinuous, ParamUnit::Percent, {}, false, "How far the cutoff follows the note, pivoting at middle C."},          \
    {spectralParam(slot, SpectralControl::CombOn), L " Spectral Comb", "Sound " L, 0.0, 1.0, 0.0, kStepped,              \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches the comb in: notches every few harmonics, which move like a flanger with no delay line."},               \
    {spectralParam(slot, SpectralControl::CombSpacing), L " Spectral Comb Spacing", "Sound " L, 1.0, 16.0, 2.0,           \
     kContinuous, ParamUnit::Number, {}, false, "How many harmonics apart the comb's notches sit.", nullptr, true},      \
    {spectralParam(slot, SpectralControl::CombDepth), L " Spectral Comb Depth", "Sound " L, 0.0, 1.0, 0.5, kContinuous,   \
     ParamUnit::Percent, {}, false, "How deep the comb's notches cut.", nullptr, true},                                  \
    {spectralParam(slot, SpectralControl::CombPhase), L " Spectral Comb Phase", "Sound " L, 0.0, 1.0, 0.0, kContinuous,   \
     ParamUnit::Percent, {}, false, "Slides the notches along the harmonics; sweep it for the flanger's motion.",        \
     nullptr, true},                                                                                                     \
    {spectralParam(slot, SpectralControl::FormantShiftOn), L " Spectral Formant Shift", "Sound " L, 0.0, 1.0, 0.0,        \
     kStepped, ParamUnit::Choice, kEnableNames, false,                                                                   \
     "Switches Formant Shift in: the spectrum's envelope slides in frequency while the partials stay put."},            \
    {spectralParam(slot, SpectralControl::FormantShiftLow), L " Spectral Shift Low", "Sound " L, -36.0, 36.0, 0.0,        \
     kContinuous, ParamUnit::Semitones, {}, false,                                                                       \
     "How far the envelope below the crossover moves. Linked, it moves all of it.", nullptr, true},                     \
    {spectralParam(slot, SpectralControl::FormantShiftHigh), L " Spectral Shift High", "Sound " L, -36.0, 36.0, 0.0,      \
     kContinuous, ParamUnit::Semitones, {}, false, "How far the envelope above the crossover moves. Unlinked only.",     \
     nullptr, true},                                                                                                     \
    {spectralParam(slot, SpectralControl::FormantShiftCrossover), L " Spectral Shift Crossover", "Sound " L,             \
     kLog2Of100, kLog2Of8k, kLog2Of1k, kContinuous, ParamUnit::LogHertz, {}, false,                                      \
     "Where the low shift hands over to the high one. Unlinked only."},                                                  \
    {spectralParam(slot, SpectralControl::FormantShiftLink), L " Spectral Shift Link", "Sound " L, 0.0, 1.0, 1.0,         \
     kStepped, ParamUnit::Choice, kEnableNames, false,                                                                   \
     "Linked, the low shift moves the whole envelope and the high shift and crossover rest."},                          \
    {spectralParam(slot, SpectralControl::TiltOn), L " Spectral Tilt", "Sound " L, 0.0, 1.0, 0.0, kStepped,              \
     ParamUnit::Choice, kEnableNames, false, "Switches Tilt in: brighter or darker, with the loudness held."},           \
    {spectralParam(slot, SpectralControl::TiltAmount), L " Spectral Tilt Amount", "Sound " L, -24.0, 24.0, 0.0,           \
     kContinuous, ParamUnit::Number, {}, false, "The tilt in dB per octave: up is brighter, down is darker.", nullptr,   \
     true},                                                                                                              \
    {spectralParam(slot, SpectralControl::OddEvenOn), L " Spectral Odd/Even", "Sound " L, 0.0, 1.0, 0.0, kStepped,       \
     ParamUnit::Choice, kEnableNames, false, "Switches Odd/Even in: the balance of odd and even harmonics."},            \
    {spectralParam(slot, SpectralControl::OddEvenBalance), L " Spectral Balance", "Sound " L, -1.0, 1.0, 0.0,             \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "Down keeps the odd harmonics (square-ish), up the even ones (hollow); the middle keeps both.", nullptr, true},     \
    {spectralParam(slot, SpectralControl::FormantOn), L " Spectral Formant", "Sound " L, 0.0, 1.0, 0.0, kStepped,        \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches the vowel in: two formant peaks fixed in Hz, laid over the spectrum, with the loudness held."},           \
    {spectralParam(slot, SpectralControl::FormantVowel), L " Spectral Vowel", "Sound " L, 0.0, 7.0, 3.0, kContinuous,    \
     ParamUnit::Vowel, {}, false,                                                                                        \
     "Which vowel: a walk through Breath, Hum, Round, Open, Bright, Nasal, Growl and Rasp, gliding between them.",       \
     nullptr, true},                                                                                                     \
    {spectralParam(slot, SpectralControl::FormantDepth), L " Spectral Vowel Depth", "Sound " L, 0.0, 1.0, 0.7,           \
     kContinuous, ParamUnit::Percent, {}, false, "How strongly the vowel speaks."},                                      \
    {spectralParam(slot, SpectralControl::PartialsOn), L " Spectral Partials", "Sound " L, 0.0, 1.0, 0.0, kStepped,      \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches the partial count in: fewer partials is a simpler, darker timbre and less work for the CPU."},            \
    {spectralParam(slot, SpectralControl::PartialsCount), L " Spectral Partial Count", "Sound " L, 1.0, 320.0, 64.0,     \
     kContinuous, ParamUnit::Number, {}, false,                                                                          \
     "How many partials sound: the lowest N in Harmonics, the loudest N in Peaks. Between two counts the last one "      \
     "fades, so nothing steps."},                                                                                        \
    {spectralParam(slot, SpectralControl::DampOn), L " Spectral Damp", "Sound " L, 0.0, 1.0, 0.0, kStepped,              \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches Damp in: higher partials die away faster over the note, as a plucked string's do."},                      \
    {spectralParam(slot, SpectralControl::DampTime), L " Spectral Damp Time", "Sound " L, kLog2Of0p05, kLog2Of20, 1.0,   \
     kContinuous, ParamUnit::LogSeconds, {}, false, "How long a partial at 1 kHz takes to fall 60 dB."},                 \
    {spectralParam(slot, SpectralControl::DampTilt), L " Spectral Damp Tilt", "Sound " L, 0.0, 3.0, 1.0, kContinuous,    \
     ParamUnit::Number, {}, false,                                                                                       \
     "How much faster the higher partials die: 0 all alike, 1 twice as fast an octave up, 2 four times."},               \
    {spectralParam(slot, SpectralControl::StretchOn), L " Spectral Stretch", "Sound " L, 0.0, 1.0, 0.0, kStepped,        \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches Stretch in: the partials spread sharp as they climb, as a piano's strings do. The same in both ears."},   \
    {spectralParam(slot, SpectralControl::StretchAmount), L " Spectral Stretch Amount", "Sound " L, 0.0, 1.0, 0.3,       \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "How far the partials spread: a piano's inharmonicity around 20-45%, a bell's beyond."},                            \
    {spectralParam(slot, SpectralControl::ShiftOn), L " Spectral Shift", "Sound " L, 0.0, 1.0, 0.0, kStepped,            \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches Shift in: every partial moves by the same Hz, so the sound goes inharmonic. The same in both ears."},     \
    {spectralParam(slot, SpectralControl::ShiftHz), L " Spectral Shift Hz", "Sound " L, -1000.0, 1000.0, 50.0,           \
     kContinuous, ParamUnit::Hertz, {}, false,                                                                           \
     "How far every partial moves, in Hz, up or down. Partials pushed to 0 Hz fade out rather than folding back."},       \
    /* F2 (additive design §3.6): the second id block, KEYED (core rows added since JF declare their key). */             \
    {spectralParam(slot, SpectralControl::FilterGain), L " Spectral Filter Gain", "Sound " L, -24.0, 24.0, 0.0,           \
     kContinuous, ParamUnit::Decibels, {}, false,                                                                        \
     "How far a Bell lifts or cuts the partials around its centre. Bell only; at 0 dB it does nothing.",                 \
     l ".spectral.filter.gain", true},                                                                                   \
    {spectralParam(slot, SpectralControl::Filter2On), L " Spectral Filter 2", "Sound " L, 0.0, 1.0, 0.0, kStepped,        \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches this sound's second spectral filter in, routed with the first in series or in parallel.",                 \
     l ".spectral.filter2.on"},                                                                                          \
    {spectralParam(slot, SpectralControl::Filter2Mode), L " Spectral Filter 2 Mode", "Sound " L, 0.0,                    \
     lastIndexOf(kSpectralFilterModeNames), 0.0, kStepped, ParamUnit::Choice, kSpectralFilterModeNames, false,           \
     "Which partials the second filter keeps: those below the cutoff, above it, around it, or all but those around it; " \
     "or a Bell, which lifts or cuts those around it by its Gain.", l ".spectral.filter2.mode"},                          \
    {spectralParam(slot, SpectralControl::Filter2Cutoff), L " Spectral Filter 2 Cutoff", "Sound " L, kLog2Of20,          \
     kLog2Of20k, kLog2Of500, kContinuous, ParamUnit::LogHertz, {}, false, "Where the second filter's curve turns.",      \
     l ".spectral.filter2.cutoff", true},                                                                                \
    {spectralParam(slot, SpectralControl::Filter2Resonance), L " Spectral Filter 2 Resonance", "Sound " L, 0.0, 1.0,     \
     0.0, kContinuous, ParamUnit::Percent, {}, false,                                                                    \
     "A peak drawn at the second filter's cutoff. Notch has none; in Bell it is the width.",                             \
     l ".spectral.filter2.res", true},                                                                                   \
    {spectralParam(slot, SpectralControl::Filter2Slope), L " Spectral Filter 2 Slope", "Sound " L, 0.0, 1.0, 0.25,       \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "How steeply the second filter falls, from gentle (6 dB/oct) to a brick wall (96 dB/oct); in Bell, how steep its "  \
     "sides are.", l ".spectral.filter2.slope", true},                                                                   \
    {spectralParam(slot, SpectralControl::Filter2KeyTrack), L " Spectral Filter 2 Key Track", "Sound " L, 0.0, 1.0,      \
     0.0, kContinuous, ParamUnit::Percent, {}, false, "How far the second filter's cutoff follows the note, pivoting at " \
     "middle C.", l ".spectral.filter2.keytrack"},                                                                       \
    {spectralParam(slot, SpectralControl::Filter2Gain), L " Spectral Filter 2 Gain", "Sound " L, -24.0, 24.0, 0.0,        \
     kContinuous, ParamUnit::Decibels, {}, false,                                                                        \
     "How far the second filter's Bell lifts or cuts the partials around its centre. Bell only; at 0 dB it does "        \
     "nothing.", l ".spectral.filter2.gain", true},                                                                      \
    {spectralParam(slot, SpectralControl::FilterRouting), L " Spectral Filter Routing", "Sound " L, 0.0, 1.0, 0.0,        \
     kStepped, ParamUnit::Choice, kSpectralRoutingNames, false,                                                          \
     "How the two filters combine: in series one after the other, or in parallel side by side, so two band-passes "      \
     "are two bands. Only while both filters are on.", l ".spectral.filter.routing"},                                    \
    /* OH: the Partials operator's cull scheme, the dropdown of its mode cell; Harmonics, the operator as it was. */     \
    {spectralParam(slot, SpectralControl::PartialsMode), L " Spectral Partials Mode", "Sound " L, 0.0,                   \
     lastIndexOf(kSpectralPartialsModeNames), 1.0, kStepped, ParamUnit::Choice, kSpectralPartialsModeNames, false,       \
     "Which partials the count keeps: the loudest N (Peaks), or the lowest N (Harmonics); or only the fundamental and a few "  \
     "low harmonics (Centers).", l ".spectral.partials.mode"},                                                           \
    /* OI: Centers' set, a coordinate (the Spectral device's runs unbounded, as Spacing's Free; a host param needs a range, */  \
    /* and sixteen is ~24 sets of the generator's pattern), gliding from one handful to the next. */                     \
    {spectralParam(slot, SpectralControl::PartialsSet), L " Spectral Partials Set", "Sound " L, 0.0, 16.0, 0.0,          \
     kContinuous, ParamUnit::Number, {}, false,                                                                          \
     "Which centre set Centers keeps: turn it and the set glides from one handful of low harmonics to the next.",        \
     l ".spectral.partials.set"},                                                                                        \
    /* OF: Partials Depth, the Spectral device's amount: how far the partials the scheme culls are turned down. Default 100% is */  \
    /* the cull as it always was (every sound unchanged); lower keeps some of them. */                                    \
    {spectralParam(slot, SpectralControl::PartialsDepth), L " Spectral Partials Depth", "Sound " L, 0.0, 1.0, 1.0,        \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "How far the partials the cull drops are turned down: all the way at 100%, and none of them at 0%.",               \
     l ".spectral.partials.depth"},                                                                                        \
    /* OE (additive design §9.4): THE VOCODER, the third id block. The analysis it reads is the engine's (Additive Vocode */ \
    /* Resolution .. Key Track); these say what each sound does with it. */                                              \
    {spectralParam(slot, SpectralControl::VocodeOn), L " Spectral Vocode", "Sound " L, 0.0, 1.0, 0.0, kStepped,          \
     ParamUnit::Choice, kEnableNames, false,                                                                             \
     "Switches the vocoder in: every partial takes the sidechain's level at its own frequency, so each note speaks with " \
     "the sidechain's formants at its own pitch. A silent sidechain gives a silent sound.", l ".spectral.vocode.on"},     \
    {spectralParam(slot, SpectralControl::VocodeDepth), L " Spectral Vocode Depth", "Sound " L, 0.0, 2.0, 1.0,            \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "How much of the sidechain's envelope goes in: none leaves the sound as it is, all is the classic vocoder, and past " \
     "that only its loudest peaks come through.", l ".spectral.vocode.depth"},                                           \
    {spectralParam(slot, SpectralControl::VocodeFlatten), L " Spectral Vocode Flatten", "Sound " L, 0.0, 1.0, 1.0,        \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "How much of the sound's own timbre is taken out before the sidechain's goes in. All of it is exact: the drawer's " \
     "filters still shape what comes out.", l ".spectral.vocode.flatten"},                                               \
    {spectralParam(slot, SpectralControl::VocodeGate), L " Spectral Vocode Gate", "Sound " L, -96.0, 0.0, -96.0,          \
     kContinuous, ParamUnit::Decibels, {}, false,                                                                        \
     "Where the sidechain is quieter than this, the partials there close (breaths and noise stay out); at the bottom, "  \
     "off.", l ".spectral.vocode.gate"},                                                                                 \
    {spectralParam(slot, SpectralControl::VocodeLow), L " Spectral Vocode Low", "Sound " L, kLog2Of20, kLog2Of20k,        \
     kLog2Of20, kContinuous, ParamUnit::LogHertz, {}, false,                                                             \
     "The lowest band: below it, the envelope is held at its level here.", l ".spectral.vocode.low"},                    \
    {spectralParam(slot, SpectralControl::VocodeHigh), L " Spectral Vocode High", "Sound " L, kLog2Of20, kLog2Of20k,      \
     kLog2Of20k, kContinuous, ParamUnit::LogHertz, {}, false,                                                            \
     "The highest band: above it, the envelope is held at its level here.", l ".spectral.vocode.high"},                                                                                        \
    /* OE2 (additive design §9.8): THE BANDED ROAD, per sound; the zones themselves are the engine's (Additive Vocode Spacing, */ \
    /* Widths). The Free coordinates are unbounded in the Spectral effect; a host param needs a range, and ±64 is ~50 cycles of */ \
    /* the slowest axis. */                                                                                               \
    {spectralParam(slot, SpectralControl::VocodeBandwidth), L " Spectral Vocode Bandwidth", "Sound " L, 0.0, 1.0, 1.0,    \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "How wide each band is against its neighbours: all the way up the smooth envelope, down to narrow bands that hold " \
     "their own level (a staircase).", l ".spectral.vocode.bandwidth"},                                                  \
    {spectralParam(slot, SpectralControl::VocodeSpacingAmount), L " Spectral Vocode Spacing Amount", "Sound " L, 0.0,    \
     1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,                                                               \
     "How far toward the chosen spacing: none is even per octave, all is the spacing itself. A named Spacing only.",     \
     l ".spectral.vocode.spacing.amount"},                                                                               \
    {spectralParam(slot, SpectralControl::VocodeSpacingFree), L " Spectral Vocode Spacing Free", "Sound " L, -64.0,      \
     64.0, 0.5, kContinuous, ParamUnit::Number, {}, false,                                                               \
     "Where you are among the mixes of the layouts — keep turning, and the mix keeps changing; 0 is even. Free only.",   \
     l ".spectral.vocode.spacing.free"},                                                                                 \
    {spectralParam(slot, SpectralControl::VocodeWidthsAmount), L " Spectral Vocode Widths Amount", "Sound " L, 0.0,      \
     1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,                                                               \
     "How far toward the chosen spread of widths: none is even. A named Widths only.",                                    \
     l ".spectral.vocode.widths.amount"},                                                                                \
    {spectralParam(slot, SpectralControl::VocodeWidthsFree), L " Spectral Vocode Widths Free", "Sound " L, -64.0, 64.0,  \
     0.5, kContinuous, ParamUnit::Number, {}, false,                                                                     \
     "Where you are among the mixes of the widths — keep turning; 0 is even. Free only.",                                \
     l ".spectral.vocode.widths.free"},

/// **ONE DRUM PAD'S ROWS** (2026-11-05, drum proposal §7): kDrumControlCount controls in DrumControl's order at
/// `drumParam(slot, c)`, the defaults from kDrumPadDefaults[slot]. Times are log2 seconds (LogSeconds), so a route's amount
/// is in doublings. The seven field scalers are fieldWithOverride knobs: the middle is the terrain as found.
#define FEATHERS_DRUM_ROWS(L, l, slot)                                                                                      \
    {drumParam(slot, DrumControl::Kind), L " Drum Kind", "Sound " L, 0.0, lastIndexOf(kDrumKindNames),                    \
     static_cast<double>(kDrumPadDefaults[slot].kind), kStepped, ParamUnit::Choice, kDrumKindNames, false,              \
     "Which drum this pad is: a pitch-swept Kick, a ringing Membrane, a Snare, a Metal cymbal or hat, a Clap, or a "     \
     "struck String.",                                                                                                   \
     l ".drum.kind"},                                                                                                    \
    {drumParam(slot, DrumControl::Note), L " Drum Note", "Sound " L, 0.0, 127.0,                                          \
     static_cast<double>(kDrumPadDefaults[slot].note), kStepped, ParamUnit::Integer, {}, false,                         \
     "The note that plays this pad in Kit mode. Two pads on one note sound together.", l ".drum.note"},                 \
    {drumParam(slot, DrumControl::Choke), L " Drum Choke", "Sound " L, 0.0, lastIndexOf(kDrumChokeNames),                 \
     static_cast<double>(kDrumPadDefaults[slot].choke), kStepped, ParamUnit::Choice, kDrumChokeNames, false,            \
     "A hit on this pad silences every other pad in the same group, as a closed hat stops an open one.",                \
     l ".drum.choke"},                                                                                                   \
    {drumParam(slot, DrumControl::Voices), L " Drum Voices", "Sound " L, 0.0, lastIndexOf(kDrumVoicesNames),              \
     static_cast<double>(kDrumPadDefaults[slot].voices), kStepped, ParamUnit::Choice, kDrumVoicesNames, false,          \
     "How many hits of this pad may ring at once; one more fades the oldest out.", l ".drum.voices"},                  \
    {drumParam(slot, DrumControl::Trigger), L " Drum Trigger", "Sound " L, 0.0, lastIndexOf(kDrumTriggerNames), 0.0,      \
     kStepped, ParamUnit::Choice, kDrumTriggerNames, false,                                                              \
     "One-shot plays the whole hit whatever the key does. Gate damps it to Release when the key is let go.",             \
     l ".drum.trigger"},                                                                                                 \
    {drumParam(slot, DrumControl::Level), L " Drum Level", "Sound " L, -60.0, 6.0, 0.0, kContinuous,                      \
     ParamUnit::Decibels, {}, false, "This pad's loudness.", l ".drum.level"},                                           \
    {drumParam(slot, DrumControl::Pan), L " Drum Pan", "Sound " L, -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {},   \
     false, "Where this pad sits between the speakers.", l ".drum.pan"},                                                 \
    {drumParam(slot, DrumControl::Tune), L " Drum Tune", "Sound " L, -48.0, 48.0, kDrumPadDefaults[slot].tune,            \
     kContinuous, ParamUnit::Semitones, {}, false, "The drum's pitch, in semitones from its kind's resting pitch.",      \
     l ".drum.tune"},                                                                                                    \
    {drumParam(slot, DrumControl::Decay), L " Drum Decay", "Sound " L, -6.643856189774724, 3.321928094887362,             \
     kDrumPadDefaults[slot].logDecay, kContinuous, ParamUnit::LogSeconds, {}, false,                                     \
     "How long the body rings: the time it takes to fall 60 dB. The terrain's Length stretches or shortens it.",        \
     l ".drum.decay"},                                                                                                   \
    {drumParam(slot, DrumControl::Release), L " Drum Release", "Sound " L, -6.643856189774724, 1.0,                       \
     -3.321928094887362, kContinuous, ParamUnit::LogSeconds, {}, false,                                                  \
     "With Trigger on Gate, how quickly the hit is damped once the key is let go.", l ".drum.release"},                 \
    {drumParam(slot, DrumControl::SweepTime), L " Drum Sweep Time", "Sound " L, -7.643856189774724, -1.0,                 \
     kDrumPadDefaults[slot].logSweepTime, kContinuous, ParamUnit::LogSeconds, {}, false,                                 \
     "How quickly the pitch falls from the top of the sweep to rest.", l ".drum.sweep_time"},                          \
    {drumParam(slot, DrumControl::Drive), L " Drum Drive", "Sound " L, 0.0, 1.0, kDrumPadDefaults[slot].drive,            \
     kContinuous, ParamUnit::Percent, {}, false, "How hard the hit is pushed into its Drive Curve: rounder and louder.",  \
     l ".drum.drive"},                                                                                                   \
    {drumParam(slot, DrumControl::Count), L " Drum Claps", "Sound " L, 1.0, 8.0,                                          \
     static_cast<double>(kDrumPadDefaults[slot].count), kStepped, ParamUnit::Integer, {}, false,                        \
     "How many hands a Clap is: the bursts before its tail.", l ".drum.count"},                                          \
    {drumParam(slot, DrumControl::Spread), L " Drum Spread", "Sound " L, -7.643856189774724, -3.321928094887362,          \
     -5.058893689053568, kContinuous, ParamUnit::LogSeconds, {}, false,                                                  \
     "How far apart a Clap's hands land: tight is one big clap, wide a ragged crowd.", l ".drum.spread"},               \
    {drumParam(slot, DrumControl::FieldPitch), L " Drum Pitch", "Sound " L, 0.0, 1.0, 0.5, kContinuous,                   \
     ParamUnit::Percent, {}, false, "The terrain's fine pitch: the middle is the terrain as found.", l ".drum.f.pitch"}, \
    {drumParam(slot, DrumControl::FieldLength), L " Drum Length", "Sound " L, 0.0, 1.0, 0.5, kContinuous,                 \
     ParamUnit::Percent, {}, false, "The terrain's Length, which stretches Decay: the middle is the terrain as found.",  \
     l ".drum.f.length"},                                                                                                \
    {drumParam(slot, DrumControl::FieldSweep), L " Drum Sweep", "Sound " L, 0.0, 1.0, kDrumPadDefaults[slot].fieldSweep,  \
     kContinuous, ParamUnit::Percent, {}, false,                                                                         \
     "How far the pitch drops from the strike: the middle is the terrain as found, up asks for more.",                   \
     l ".drum.f.sweep"},                                                                                                 \
    {drumParam(slot, DrumControl::FieldTone), L " Drum Tone", "Sound " L, 0.0, 1.0, 0.5, kContinuous,                     \
     ParamUnit::Percent, {}, false, "Brightness: where it is struck and how high its band sits.", l ".drum.f.tone"},     \
    {drumParam(slot, DrumControl::FieldNoise), L " Drum Noise", "Sound " L, 0.0, 1.0, kDrumPadDefaults[slot].fieldNoise,  \
     kContinuous, ParamUnit::Percent, {}, false, "How much of the hit is noise: a snare's wires, a hat's hiss.",         \
     l ".drum.f.noise"},                                                                                                 \
    {drumParam(slot, DrumControl::FieldSnap), L " Drum Snap", "Sound " L, 0.0, 1.0, 0.5, kContinuous,                     \
     ParamUnit::Percent, {}, false, "How hard the attack is: a soft mallet to a hard stick.", l ".drum.f.snap"},         \
    {drumParam(slot, DrumControl::FieldMetal), L " Drum Metal", "Sound " L, 0.0, 1.0, 0.5, kContinuous,                   \
     ParamUnit::Percent, {}, false, "How inharmonic it rings: a skin toward a bar or a bell.", l ".drum.f.metal"},                                                                                                                  \
    /* THE SNARE'S ENVELOPES AND MOVING BELL (2026-11-06, drum proposal §12): read only on a Snare. */                       \
    {drumParam(slot, DrumControl::WireDecay), L " Drum Wire Decay", "Sound " L, -6.643856189774724, 1.0,                  \
     -2.473931188332412, kContinuous, ParamUnit::LogSeconds, {}, false,                                                  \
     "How long a snare's wires buzz, on their own: the time they take to fall 60 dB.", l ".drum.wire_decay"},           \
    {drumParam(slot, DrumControl::ClickDecay), L " Drum Click", "Sound " L, -9.965784284662087, -4.321928094887363,       \
     -7.380821783940931, kContinuous, ParamUnit::LogSeconds, {}, false,                                                  \
     "How long a snare's stick click lasts.", l ".drum.click_decay"},                                                \
    {drumParam(slot, DrumControl::BellStart), L " Drum Bell Start", "Sound " L, 6.643856189774724, 13.550746785383243,     \
     11.287712379549449, kContinuous, ParamUnit::LogHertz, {}, false,                                                    \
     "Where a snare's moving bell starts: a resonant peak that glides to Bell End.", l ".drum.bell_start"},           \
    {drumParam(slot, DrumControl::BellEnd), L " Drum Bell End", "Sound " L, 6.643856189774724, 13.550746785383243,         \
     9.451211111832329, kContinuous, ParamUnit::LogHertz, {}, false, "Where the moving bell comes to rest.",            \
     l ".drum.bell_end"},                                                                                                \
    {drumParam(slot, DrumControl::BellTime), L " Drum Bell Time", "Sound " L, -7.643856189774724, -1.0,                   \
     -4.643856189774724, kContinuous, ParamUnit::LogSeconds, {}, false, "How long the bell takes to glide to its end.",  \
     l ".drum.bell_time"},                                                                                               \
    /* THE KICK'S SHAPE (2026-11-06, drum proposal §12 items 1-3): read only on a Kick. */                                  \
    {drumParam(slot, DrumControl::KickHold), L " Drum Hold", "Sound " L, 0.0, 200.0, 0.0, kContinuous, ParamUnit::Number,  \
     {}, false, "How long, in ms, a Kick's body stays at full before it decays.", l ".drum.kick_hold"},                  \
    {drumParam(slot, DrumControl::KickDip), L " Drum Dip", "Sound " L, 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent,     \
     {}, false, "The fishtail: how deep a Kick's level dips between its snap and its boom.", l ".drum.kick_dip"},       \
    {drumParam(slot, DrumControl::KickDipTime), L " Drum Dip Time", "Sound " L, 2.0, 80.0, 14.0, kContinuous,              \
     ParamUnit::Number, {}, false, "Where, in ms, the fishtail's dip is deepest.", l ".drum.kick_dip_time"},             \
    {drumParam(slot, DrumControl::ClickSweep), L " Drum Click Sweep", "Sound " L, 0.0, 6.0, 0.0, kContinuous,             \
     ParamUnit::Number, {}, false,                                                                                       \
     "A second, very short pitch drop on a Kick, in octaves: the click of the beater on top of the boom.",               \
     l ".drum.click_sweep"},                                                                                             \
    {drumParam(slot, DrumControl::ClickSweepTime), L " Drum Click Sweep Time", "Sound " L, -10.965784284662087,            \
     -5.643856189774724, -8.380821783940931, kContinuous, ParamUnit::LogSeconds, {}, false,                              \
     "How quickly the click's pitch drop lands.", l ".drum.click_sweep_time"},                                       \
    {drumParam(slot, DrumControl::StartPhase), L " Drum Start Phase", "Sound " L, 0.0, 90.0, 0.0, kContinuous,             \
     ParamUnit::Number, {}, false,                                                                                       \
     "Where, in degrees, a Kick's sine starts: 0 is smooth; toward 90 the first cycle hits harder.",                  \
     l ".drum.start_phase"},                                                                           \
    {drumParam(slot, DrumControl::DriveCurve), L " Drum Drive Curve", "Sound " L, 0.0, lastIndexOf(kDrumDriveCurveNames),  \
     0.0, kStepped, ParamUnit::Choice, kDrumDriveCurveNames, false,                                                      \
     "The shape Drive pushes the hit into: Tanh is round, Tube leans to one side and adds even harmonics, Soft Clip "    \
     "flattens its tops.",                                                                                               \
     l ".drum.drive_curve"},                                                                                             \
    {drumParam(slot, DrumControl::DriveTone), L " Drum Drive Tone", "Sound " L, 0.0, 1.0, 0.5, kContinuous,               \
     ParamUnit::Percent, {}, false,                                                                                      \
     "A tilt before the drive: down feeds it the lows, so they saturate; up feeds it the highs. The middle is flat.",   \
     l ".drum.drive_tone"},                                                                                              \
    {drumParam(slot, DrumControl::ClapQ), L " Drum Clap Q", "Sound " L, 0.5, 8.0, 1.6, kContinuous, ParamUnit::Number,    \
     {}, false, "How narrow a Clap's band is: low is a wide, papery slap; high a ringing, hollow one.",                  \
     l ".drum.clap_q"},                                                                                                  \
    {drumParam(slot, DrumControl::ClapHighPass), L " Drum Clap High Pass", "Sound " L, 4.321928094887363,                \
     11.0, 4.321928094887363, kContinuous, ParamUnit::LogHertz, {}, false,                                              \
     "Takes the lows out of a Clap below this: at its bottom, nothing is taken.", l ".drum.clap_hp"},                                \
    /* THE SNARE'S SECOND BELL (2026-11-06): read only on a Snare. */     \
    {drumParam(slot, DrumControl::Bell2Hz), L " Drum Bell 2 Hz", "Sound " L, 7.965784284662087, 11.287712379549449,       \
     8.965784284662087, kContinuous, ParamUnit::LogHertz, {}, false, "Free: where the second bell sits.",              \
     l ".drum.bell2_hz"},                                                                                                \
    {drumParam(slot, DrumControl::Bell2Time), L " Drum Bell 2 Time", "Sound " L, -8.380821783940931, -3.643856189774724,  \
     -5.058893689053568, kContinuous, ParamUnit::LogSeconds, {}, false,                                                  \
     "How long the second bell rings: its level falls 60 dB in this time.", l ".drum.bell2_time"},                      \
    /* VELOCITY'S OTHER TARGETS (2026-11-06, doc §13 item 4). */                                                          \
    {drumParam(slot, DrumControl::VelocityNoise), L " Drum Velocity Noise", "Sound " L, -1.0, 1.0, 0.0, kContinuous,      \
     ParamUnit::Percent, {}, false,                                                                                      \
     "How far a hit's velocity moves its noise share: up, a harder hit is noisier; down, cleaner.",                     \
     l ".drum.velocity_noise"},                                                                                          \
    /* THE WIRES (2026-11-06, doc §13 items 7 and 9). */                                                                   \
    {drumParam(slot, DrumControl::Width), L " Drum Width", "Sound " L, 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent,    \
     {}, false, "How far a Snare's wires or a Clap's noise differ between the ears: at 0 they are the same.",            \
     l ".drum.width"},                                                                                                   \
    {drumParam(slot, DrumControl::Mute), L " Drum Mute", "Sound " L, 0.0, 1.0, 0.0, kStepped, ParamUnit::Integer, {},      \
     false, "Silences this pad's hits. In Kit mode the sound card's switch is this.", l ".drum.mute"},          \
    /* THE BEND: Kit mode alone; in Pitched mode the voice follows the key and the wheel. */     \
    {drumParam(slot, DrumControl::Bend), L " Drum Bend", "Sound " L, 0.0, 1.0, 1.0, kContinuous, ParamUnit::Percent, {},  \
     false,                                                                                                              \
     "How far the pitch bend moves this pad in a kit, held when it is struck or moved while it rings: at 100% the whole " \
     "Bend range, at 0 the pad ignores the wheel.",                                                                     \
     l ".drum.bend"},                                                                                                  \
    /* THE LAYER SLOT (drum proposal §12; stage (c), 2026-11-07): Kit mode alone. */                                     \
    {drumParam(slot, DrumControl::LayerKind), L " Drum Layer", "Sound " L, 0.0, lastIndexOf(kDrumLayerKindNames), 0.0,     \
     kStepped, ParamUnit::Choice, kDrumLayerKindNames, false,                                                             \
     "A second hit struck with this pad, of its own kind, at its own place on the terrain. Off strikes nothing.",        \
     l ".drum.layer.kind"},                                                                                              \
    {drumParam(slot, DrumControl::LayerLevel), L " Drum Layer Level", "Sound " L, -60.0, 6.0, 0.0, kContinuous,          \
     ParamUnit::Decibels, {}, false, "The layer's loudness, on top of the pad's.", l ".drum.layer.level"},                \
    {drumParam(slot, DrumControl::LayerDelay), L " Drum Layer Delay", "Sound " L, 0.0, 10.0, 0.0, kContinuous,           \
     ParamUnit::Number, {}, false, "How long, in ms, after the pad's hit the layer strikes.", l ".drum.layer.delay"},     \
    {drumParam(slot, DrumControl::LayerHighPass), L " Drum Layer High Pass", "Sound " L, 4.321928094887363,              \
     11.0, 4.321928094887363, kContinuous, ParamUnit::LogHertz, {}, false,                                              \
     "Takes the layer's lows out below this: at its bottom, nothing is taken.", l ".drum.layer.hp"},                    \
    {drumParam(slot, DrumControl::LayerLock), L " Drum Layer Lock", "Sound " L, 0.0, 1.0, 1.0, kStepped,                 \
     ParamUnit::Integer, {}, false,                                                                                      \
     "Locked, the layer keeps its place relative to the pad when the pad is dragged; unlocked, it stays where it is.",   \
     l ".drum.layer.lock"},                                                                                              \
    {drumParam(slot, DrumControl::LayerOffsetX), L " Drum Layer Offset X", "Sound " L, -1.0, 1.0, 0.0, kContinuous,      \
     ParamUnit::Percent, {}, false, "Where the layer stands across from its pad on the terrain.", l ".drum.layer.dx"},     \
    {drumParam(slot, DrumControl::LayerOffsetY), L " Drum Layer Offset Y", "Sound " L, -1.0, 1.0, 0.0, kContinuous,      \
     ParamUnit::Percent, {}, false, "Where the layer stands up or down from its pad on the terrain.", l ".drum.layer.dy"}, \
    /* THE ZONE CONTROLS (drum proposal §14): one coordinate each, a lap of the named zones per 1.0. */     \
    {drumParam(slot, DrumControl::VelocityZone), L " Drum Velocity", "Sound " L, -8.0, 8.0, 2.0 / 3.0, kContinuous,        \
     ParamUnit::Number, {}, false,                                                                                       \
     "What a hit's velocity reaches, through named zones: only Level; and Tone (brightness and snap); and Tension (the "  \
     "glide deepens with velocity squared). Between two zones it moves from one to the next.",                           \
     l ".drum.velocity_zone"},                                                                                           \
    {drumParam(slot, DrumControl::Wires), L " Drum Wires", "Sound " L, -8.0, 8.0, 0.0, kContinuous, ParamUnit::Number,     \
     {}, false,                                                                                                          \
     "A Snare's wires through named zones: Tight, Loose, Buzzy (rattling, following the head) and Gated (cut short, the " \
     "80s snare). Between two zones it moves from one to the next.",                                                     \
     l ".drum.wires"},                                                                                                   \
    {drumParam(slot, DrumControl::Bells), L " Drum Bells", "Sound " L, -8.0, 8.0, 0.0, kContinuous, ParamUnit::Number,     \
     {}, false,                                                                                                          \
     "A Snare's two moving bells through named zones: Off, Body (one peak), Shell (two, linked), Ring (narrow and "       \
     "ringing) and Knock (wide, the second free). How narrow follows the place's Snap; the glide is on the picture.",    \
     l ".drum.bells"},

constexpr ParamDesc kParams[] = {
    // DEFAULT = V5, the flagship (decided 2026-08-18): a fresh instance gets the
    // flagship; a loaded patch carries its stored index and is untouched. The range widens with
    // the name list per lastIndexOf, so the third entry is selectable the moment it is named.
    {ParamId::OscType, "Osc Type", "", 0.0, lastIndexOf(kOscTypeNames),
     static_cast<double>(OscType::V5), kSteppedNoAutomation, ParamUnit::Choice, kOscTypeNames},

    // THE WARP SHAPE — engine-wide, stepped, default Off, so a patch that never touches it is the
    // engine exactly as it was. Ten shapes; dsp/warp.h holds them and says what each computes.
    //
    // `kStepped` AND NOT `kSteppedNoAutomation`, which is the carve-out its neighbour above takes.
    // That carve-out has a stated reason — the engine "selects which renderer builds the character
    // tables", so a host sweeping it is sweeping a RENDERER SELECTOR — and warp is not one: it picks
    // a shape inside the flagship's existing operator tier, on the same bake path, exactly as the
    // five field-modifier knobs do. Extending a decision past the reason it was made for is how a
    // guard turns into a habit, so this takes the ordinary flag and says why.
    {ParamId::WarpMode, "Warp Mode", "", 0.0, lastIndexOf(kWarpModeNames), 0.0, kStepped,
     ParamUnit::Choice, kWarpModeNames},

    // THE RESONATE STRING'S PHYSICS — engine-wide, stepped, default Ring (the firmware's own model).
    // `kSteppedNoAutomation`, OscType's flags and for a kin of its reason (core quality review, 2026-09-25):
    // a model switch RESEATS the string at the new model's rest pose, discarding its motion, and costs
    // 6-30 us at the tick. That is a patch choice, not a gesture: a host sweeping it would throw away the
    // string's motion on every step. The tables still crossfade over one window, so a menu pick does not
    // click.
    {ParamId::ResonateModel, "Model", "", 0.0, lastIndexOf(kResonateModelNames), 0.0, kSteppedNoAutomation,
     ParamUnit::Choice, kResonateModelNames},

    // RESONATE'S SHAPING (2026-09-26): the Explore disk's surround on Resonate. Continuous amounts are
    // automatable AND modulatable (a string shaped by an LFO is the point); the bow mode is a stepped choice.
    // Every default is the laws as authored — x1, the laws' own mode, mono — so a patch that never touches
    // them plays the string exactly as before these existed.
    {ParamId::ResonateWidth, "Resonate Width", "Resonate", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, false,
     "Stereo from the one string: the right channel reads the ring from a second pickup, up to half a turn "
     "away. At 0 both channels read the same place.",
     "resonate.width"},
    {ParamId::ResonateBow, "Resonate Bow", "Resonate", 0.0, 4.0, 1.0, kContinuous, ParamUnit::Factor, {}, false,
     "How hard the string is bowed between notes, times the zone's own bow. At 0 it only moves when a note "
     "plucks it.",
     "resonate.bow"},
    {ParamId::ResonateBowMode, "Resonate Bow Mode", "Resonate", 0.0, lastIndexOf(kResonateBowModeNames), 0.0,
     kStepped, ParamUnit::Choice, kResonateBowModeNames, false,
     "How the bow pushes: the zone's own way (Laws), or forced to a smooth push-pull, a slow drag and snap, "
     "filtered noise, alternating taps or a wandering pressure.",
     "resonate.bow_mode"},
    {ParamId::ResonateShimmer, "Resonate Shimmer", "Resonate", -2.0, 2.0, 0.0, kContinuous, ParamUnit::LogFactor, {},
     false,
     "How fast the scanned shape follows the string's motion. Up lets the string's quicker ripples through as "
     "a shimmer; down keeps only its slow sway.",
     "resonate.shimmer"},
    {ParamId::ResonateStiffness, "Resonate Stiffness", "Resonate", -2.0, 2.0, 0.0, kContinuous, ParamUnit::LogFactor,
     {}, false,
     "How strongly each point is pulled back to the zone's shape. Up is taut: the shape holds and ripples "
     "fast; down is slack: it wanders and drifts.",
     "resonate.stiffness"},
    {ParamId::ResonateCoupling, "Resonate Coupling", "Resonate", -2.0, 2.0, 0.0, kContinuous, ParamUnit::LogFactor, {},
     false,
     "How strongly neighbouring points pull on each other. Up spreads a disturbance quickly round the string "
     "and smooths the shape; down keeps it local and jagged.",
     "resonate.coupling"},
    {ParamId::ResonateDecay, "Resonate Decay", "Resonate", -2.0, 2.0, 0.0, kContinuous, ParamUnit::LogFactor, {},
     false,
     "How long the string keeps moving after it is plucked or bowed: up is a longer ring, down settles "
     "sooner.",
     "resonate.decay"},
    {ParamId::ResonatePluck, "Resonate Pluck", "Resonate", 0.0, 4.0, 1.0, kContinuous, ParamUnit::Factor, {}, false,
     "How hard each note plucks the string, times the zone's own pluck. At 0 notes do not disturb it at all.",
     "resonate.pluck"},
    {ParamId::ResonateTravel, "Resonate Travel", "Resonate", 0.0, 4.0, 1.0, kContinuous, ParamUnit::Factor, {}, false,
     "How fast the string turns under the pickup, times the zone's own drift: a slow chorus-like motion. At 0 "
     "it holds still.",
     "resonate.travel"},
    // THE STRING'S TICK RATE, log2 of a multiple of the firmware's per-128-sample tick. Its
    // DEFAULT IS x2, NOT x1: the hardware ticked once per render window, and its windows run under 128 samples,
    // so the string as originally known moved faster than the port's first clock (WeaveString::setSpeed). x1 is that clock.
    {ParamId::ResonateSpeed, "Resonate Speed", "Resonate", 0.0, 3.0, 1.0, kContinuous, ParamUnit::LogFactor, {}, false,
     "How fast the whole string lives: its ripples, sway, bowing and drift, and how quickly a pluck settles. "
     "x1 is the original, slowest rate; up is livelier.",
     "resonate.speed"},

    // THE BASIC SHAPES MIX (2026-10-03): the Explore stage's right flank on Basic Shapes. Automatable and
    // modulatable, like Resonate's shaping. Default a pure sine (decided 2026-10-03); the mix is normalised by its
    // sum, with a tiny sine bias so all four at zero is still a sine (dsp/basic_shapes.h).
    {ParamId::ShapesSine, "Shapes Sine", "Basic Shapes", 0.0, 1.0, 1.0, kContinuous, ParamUnit::Percent, {}, false,
     "How much sine is in the mix. The four shape knobs are proportions: each is its share of their total.",
     "shapes.sine"},
    {ParamId::ShapesTriangle, "Shapes Triangle", "Basic Shapes", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {},
     false, "How much triangle is in the mix: odd harmonics falling off steeply, soft and hollow.",
     "shapes.triangle"},
    {ParamId::ShapesSaw, "Shapes Saw", "Basic Shapes", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, false,
     "How much saw is in the mix: every harmonic, bright and buzzy.", "shapes.saw"},
    {ParamId::ShapesSquare, "Shapes Square", "Basic Shapes", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {},
     false, "How much square is in the mix: odd harmonics, hollow and woody.", "shapes.square"},

    // THE ADDITIVE SHAPE (2026-10-10, design §4.1) — engine-wide, stepped, default Saw. `kStepped`, WarpMode's flags and
    // for WarpMode's reason: it picks the target each sound's Shape Amount blends toward, on the same build, not a
    // renderer. Live only on Additive.
    {ParamId::AdditiveShape, "Additive Shape", "Additive", 0.0, lastIndexOf(kAdditiveShapeNames), 2.0, kStepped,
     ParamUnit::Choice, kAdditiveShapeNames, false,
     "The exact waveform each sound's Shape Amount pulls its spectrum toward: Sine, Triangle, Saw or Square.",
     "additive.shape"},
    // A-PH (2026-10-18, additive design §8): what the engine does with its phases. `kStepped`, AdditiveShape's flags: a
    // choice made per note-on, never a sweep — a held note keeps the phases it started with whatever this says.
    {ParamId::AdditivePhases, "Additive Phases", "Additive", 0.0, lastIndexOf(kAdditivePhaseNames), 0.0, kStepped,
     ParamUnit::Choice, kAdditivePhaseNames, false,
     "How each note's partials line up. Natural: the engine's own low-crest phases. Headroom: phases optimised so the same "
     "sound peaks lower — louder at the same ceiling. Sub priority: the same, with the fundamental raised by Sub and each "
     "voice kept under Ceiling. Applies from the next note.",
     "additive.phases"},
    // SUB AND CEILING: continuous, so automatable and modulatable (test_params: every continuous control is a destination).
    // Each move rebuilds the table and asks the optimiser again; the request slot keeps only the newest (dsp/phase_profile.h).
    // A-PH3 (2026-10-18): Sub is a LEVEL, the fundamental against the SHAPE's rest, taken upstream of every drawer operator and
    // the Nyquist cull with Damp applied (option (b)). A new key: it was a raise over the sound's own balance.
    {ParamId::AdditiveSub, "Additive Sub", "Additive", -24.0, 12.0, -6.0, kContinuous,
     ParamUnit::Decibels, {}, false,
     "Sub priority: the fundamental's level against the sound's own other partials (0 dB: as much energy as all of them "
     "together), measured before the drawer, so no drawer control and no high note moves it; it falls with Damp as the rest "
     "does. It stays at the note's pitch whatever Shift and Stretch do. The phases are optimised for the lowest peak at it.",
     "additive.sub_level"},
    {ParamId::AdditiveCeiling, "Additive Ceiling", "Additive", -24.0, 0.0, -6.0,
     kContinuous, ParamUnit::Decibels, {}, false,
     "Sub priority: a fixed per-voice turn-down, set at each note-on from the optimised phases' peak. A voice that would peak "
     "over this is turned down whole, its balance kept. Never up, and not a limiter. Voices add, so the mix's final "
     "ceiling is a Limit device's job.",
     "additive.ceiling"},
    // OE (2026-10-26, additive design §9.4): THE ENGINE VOCODER'S ANALYSIS — the sidechain is analysed once for every voice, so
    // these are engine-wide; each sound's Vocode controls say what it does with the envelope. The Spectral effect's ranges and
    // defaults. Resolution, Freeze and Key Track are choices (`kStepped`); Bands, Attack and Release continuous, so destinations.
    {ParamId::AdditiveVocodeResolution, "Additive Vocode Resolution", "Additive", 0.0,
     lastIndexOf(kAdditiveVocodeResolutionNames), 1.0, kStepped, ParamUnit::Choice, kAdditiveVocodeResolutionNames, false,
     "How the sidechain is analysed: Fast follows quickly with coarse low bands, Fine resolves low notes and moves slower.",
     "additive.vocode.resolution"},
    {ParamId::AdditiveVocodeBands, "Additive Vocode Bands", "Additive", 8.0, 128.0, 24.0, kContinuous, ParamUnit::Number, {},
     false,
     "How finely the sidechain's envelope is taken: few coarse bands, as a classic vocoder's, up to a smooth envelope that "
     "follows the formants.",
     "additive.vocode.bands"},
    {ParamId::AdditiveVocodeAttack, "Additive Vocode Attack", "Additive", 1.0, 500.0, 10.0, kContinuous, ParamUnit::Number,
     {}, false, "How quickly the sidechain's envelope rises, in milliseconds.", "additive.vocode.attack"},
    {ParamId::AdditiveVocodeRelease, "Additive Vocode Release", "Additive", 5.0, 2000.0, 80.0, kContinuous,
     ParamUnit::Number, {}, false, "How slowly the sidechain's envelope falls, in milliseconds.", "additive.vocode.release"},
    {ParamId::AdditiveVocodeFreeze, "Additive Vocode Freeze", "Additive", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice,
     kEnableNames, false,
     "Holds the sidechain's envelope as it is now, for as long as Freeze is on. It is saved with the patch.",
     "additive.vocode.freeze"},
    {ParamId::AdditiveVocodeKeyTrack, "Additive Vocode Key Track", "Additive", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice,
     kEnableNames, false,
     "While frozen, each note reads the held envelope moved by its distance from the note that was playing when Freeze "
     "took hold, like a transposed recording.",
     "additive.vocode.keytrack"},
    // OE2 (2026-10-27, additive design §9.8): THE BANDED ROAD'S ZONES AND THE STEREO, engine-wide choices (the Spectral effect's
    // labels and defaults).
    {ParamId::AdditiveVocodeSpacing, "Additive Vocode Spacing", "Additive", 0.0, lastIndexOf(kAdditiveVocodeSpacingNames), 0.0,
     kStepped, ParamUnit::Choice, kAdditiveVocodeSpacingNames, false,
     "Where the vocoder's bands sit: even per octave, along the Bark scale (dense through speech), crowded toward the lows or the "
     "highs, on each note's harmonics or between them — or Free, any mix of them. Each sound says how far.",
     "additive.vocode.spacing"},
    {ParamId::AdditiveVocodeWidths, "Additive Vocode Widths", "Additive", 0.0, lastIndexOf(kAdditiveVocodeWidthsNames), 0.0,
     kStepped, ParamUnit::Choice, kAdditiveVocodeWidthsNames, false,
     "How the bands' widths spread: even, narrow below and wide above or the reverse, narrow on each note's harmonics or between "
     "them — or Free. Each sound says how far.",
     "additive.vocode.widths"},
    {ParamId::AdditiveVocodeStereo, "Additive Vocode Stereo", "Additive", 0.0, lastIndexOf(kAdditiveVocodeStereoNames), 0.0,
     kStepped, ParamUnit::Choice, kAdditiveVocodeStereoNames, false,
     "Mono vocodes both ears by the sidechain's sum; Stereo each ear by its own sidechain channel; Spread sends the bands "
     "alternately left and right.",
     "additive.vocode.stereo"},
    {ParamId::AdditiveVocodeSibilance, "Additive Vocode Sibilance", "Additive", 0.0, 1.0, 0.3, kContinuous, ParamUnit::Percent, {},
     false,
     "Noise mixed in where the sidechain is noisy (its esses and breaths), at its level, while a vocoding note sounds — and "
     "only then.",
     "additive.vocode.sibilance"},

    // THE KARPLUS-STRONG ENGINE'S CONTROLS (2026-10-31): engine-wide, every
    // voice's string reads them each block. Decay and Release in log2 seconds (the house's LogSeconds taper); at the top
    // of either the loop is not shortened by it. Seed is stepped: which plucks, never a sweep. Live only on Pluck.
    {ParamId::KsDecay, "Pluck Decay", "Pluck", -4.321928094887363, 4.906890595608519, 1.584962500721156, kContinuous,
     ParamUnit::LogSeconds, {}, false,
     "How long a plucked string rings: the time a C3's fundamental takes to fall 60 dB. Each sound's Sustain stretches or "
     "shortens it, and Decay Track shortens the high notes. At the top it rings for as long as the key is held.",
     "ks.decay"},
    {ParamId::KsDecayTrack, "Pluck Decay Track", "Pluck", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "How much shorter high notes ring than low ones. At 0 every note rings for Decay; up, an octave higher rings "
     "shorter, as a real string does.",
     "ks.decay_track"},
    {ParamId::KsRelease, "Pluck Release", "Pluck", -5.643856189774724, 3.321928094887362, -2.321928094887362, kContinuous,
     ParamUnit::LogSeconds, {}, false,
     "How quickly a released string is damped, as a finger stops it: the 60 dB time after the key is let go. At the top "
     "a released string rings on.",
     "ks.release"},
    {ParamId::KsHardness, "Pluck Hardness", "Pluck", 0.0, 1.0, 0.6, kContinuous, ParamUnit::Percent, {}, false,
     "How hard the noise in a pluck is: soft is a dull thump, hard a bright scratch. Only the share of the pluck each "
     "sound's Noise gives the noise.",
     "ks.hardness"},
    {ParamId::KsPickup, "Pluck Pickup", "Pluck", 0.02, 0.5, 0.12, kContinuous, ParamUnit::Percent, {}, false,
     "Where along the string it is heard. Near the end is thin and bright; toward the middle is round and hollow.",
     "ks.pickup"},
    {ParamId::KsWidth, "Pluck Width", "Pluck", 0.0, 0.3, 0.1, kContinuous, ParamUnit::Percent, {}, false,
     "Stereo from two pickups: how far apart along the string the left and right ears listen. At 0 both hear the same.",
     "ks.width"},
    {ParamId::KsSeed, "Pluck Seed", "Pluck", 0.0, 999.0, 0.0, kStepped, ParamUnit::Integer, {}, false,
     "Which plucks the noise draws. Every pluck is drawn from the Seed, the key and where in the song it lands, so a "
     "song plays the same plucks on every pass.",
     "ks.seed"},
    {ParamId::KsVelocity, "Pluck Velocity", "Pluck", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "How much a note's velocity moves its pluck's Hardness: harder notes are brighter and scratchier, softer ones duller. "
     "At 0 every note plucks at Hardness alone.",
     "ks.velocity_hardness"},

    // THE DRUM ENGINE'S ENGINE-WIDE CONTROLS (2026-11-05). Live only on
    // Drum; Pitched Kind only in Pitched mode, the Walk only in Kit mode.
    {ParamId::DrumMode, "Drum Mode", "Drum", 0.0, lastIndexOf(kDrumModeNames), 1.0, kStepped, ParamUnit::Choice,
     kDrumModeNames, false,
     "Pitched plays one drum chromatically across the keyboard. Kit plays a different drum for each note: each of sounds "
     "A to H is a pad with its own note, kind and place on the terrain.",
     "drum.mode"},
    {ParamId::DrumPitchedKind, "Drum Kind", "Drum", 0.0, lastIndexOf(kDrumKindNames), 1.0, kStepped, ParamUnit::Choice,
     kDrumKindNames, false, "Which drum Pitched mode plays: every note is this kind, at the note's pitch.",
     "drum.pitched_kind"},
    {ParamId::DrumWalkX, "Drum Walk X", "Drum", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, false,
     "Moves every pad's place on the terrain together, across. At 0 each pad sits where you put it; a slow LFO here "
     "breathes the whole kit.",
     "drum.walk_x"},
    {ParamId::DrumWalkY, "Drum Walk Y", "Drum", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, false,
     "Moves every pad's place on the terrain together, up and down.", "drum.walk_y"},
    {ParamId::DrumSeed, "Drum Seed", "Drum", 0.0, 999.0, 0.0, kStepped, ParamUnit::Integer, {}, false,
     "Which hits the noise draws. Every hit is drawn from the Seed, the pad and where in the song it lands, so a song "
     "plays the same hits on every pass.",
     "drum.seed"},
    // THE KIT-WIDE LAYER (2026-11-06): neutral by default (a kit is bit-identical until it moves); Kit mode only.
    {ParamId::DrumKitGamma, "Drum Kit Gamma", "Drum", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}, false,
     "Moves every pad's Gamma together: the whole kit to another country on the terrain, each pad keeping its place "
     "relative to the others.",
     "drum.kit.gamma"},
    {ParamId::DrumKitZone, "Drum Kit Zone", "Drum", -512.0, 512.0, 0.0, kContinuous, ParamUnit::Zone, {}, false,
     "Moves every pad's Zone together (each held inside the zones that exist).", "drum.kit.zone"},
    {ParamId::DrumKitOffset, "Drum Kit Offset", "Drum", -2.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}, false,
     "Moves every pad's Offset together.", "drum.kit.offset"},
    {ParamId::DrumKitPitch, "Drum Kit Pitch", "Drum", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "Every pad's terrain Pitch, scaled on top of its own: the middle leaves each as it is.", "drum.kit.pitch"},
    {ParamId::DrumKitLength, "Drum Kit Length", "Drum", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "Every pad's terrain Length, scaled on top of its own: the middle leaves each as it is.", "drum.kit.length"},
    {ParamId::DrumKitSweep, "Drum Kit Sweep", "Drum", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "Every pad's terrain Sweep (its Tension), scaled on top of its own: the middle leaves each as it is.",
     "drum.kit.sweep"},
    {ParamId::DrumKitTone, "Drum Kit Tone", "Drum", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "Every pad's terrain Tone, scaled on top of its own: the middle leaves each as it is.", "drum.kit.tone"},
    {ParamId::DrumKitNoise, "Drum Kit Noise", "Drum", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "Every pad's terrain Noise, scaled on top of its own: the middle leaves each as it is.", "drum.kit.noise"},
    {ParamId::DrumKitSnap, "Drum Kit Snap", "Drum", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "Every pad's terrain Snap, scaled on top of its own: the middle leaves each as it is.", "drum.kit.snap"},
    {ParamId::DrumKitMetal, "Drum Kit Metal", "Drum", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}, false,
     "Every pad's terrain Metal, scaled on top of its own: the middle leaves each as it is.", "drum.kit.metal"},

    // THE SPECTRAL DRAWER, PER SOUND (2026-10-10, additive design §3.2; each sound has its own drawer, and the voice
    // runs the sounds' drawers blended by weight). kSpectralControlCount rows per sound (FEATHERS_SPECTRAL_ROWS, above),
    // unkeyed like every per-sound row until JF step 2 keys them — all but F2's, which declare theirs (`a.spectral.filter2.on` …).
    FEATHERS_SPECTRAL_ROWS("A", "a", 0)
    FEATHERS_SPECTRAL_ROWS("B", "b", 1)
    FEATHERS_SPECTRAL_ROWS("C", "c", 2)
    FEATHERS_SPECTRAL_ROWS("D", "d", 3)
    FEATHERS_SPECTRAL_ROWS("E", "e", 4)
    FEATHERS_SPECTRAL_ROWS("F", "f", 5)
    FEATHERS_SPECTRAL_ROWS("G", "g", 6)
    FEATHERS_SPECTRAL_ROWS("H", "h", 7)

    // THE DRUM PADS, PER SOUND (2026-11-05, drum proposal §7): kDrumControlCount rows per sound (FEATHERS_DRUM_ROWS), keyed
    // `a.drum.kind` …, each pad's defaults from kDrumPadDefaults (Pattern Drum's lanes).
    FEATHERS_DRUM_ROWS("A", "a", 0)
    FEATHERS_DRUM_ROWS("B", "b", 1)
    FEATHERS_DRUM_ROWS("C", "c", 2)
    FEATHERS_DRUM_ROWS("D", "d", 3)
    FEATHERS_DRUM_ROWS("E", "e", 4)
    FEATHERS_DRUM_ROWS("F", "f", 5)
    FEATHERS_DRUM_ROWS("G", "g", 6)
    FEATHERS_DRUM_ROWS("H", "h", 7)

    // A sound's identity: where on the zone axis it sits, how far the phase offset walks it, and its
    // γ coordinate. Zone is declared continuous over the uint16 domain the character builders take —
    // the quantisation to an integer zone happens when a table is built, not here, so automation and
    // modulation of zone stay smooth instead of stepping at the param.
    {ParamId::AZone, "A Zone", "Sound A", 0.0, 1023.0, 0.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::AOffset, "A Offset", "Sound A", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::AGamma, "A Gamma", "Sound A", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},

    // B defaults to the middle of the zone range purely so the shipped patch has somewhere to morph
    // to: with both sounds at zone 0, Path would be silent as a control.
    {ParamId::BZone, "B Zone", "Sound B", 0.0, 1023.0, 512.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::BOffset, "B Offset", "Sound B", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::BGamma, "B Gamma", "Sound B", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},

    // Path tours the constellation; with two sounds that tour is the segment A→B, which is exactly
    // the firmware's crossfade. Pos X/Y are bipolar deviations from the tour, centre-detented at
    // zero — declared now so their ids and centres are fixed, dormant until the blend takes more
    // than two sounds.
    {ParamId::Path, "Path", "Blend", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::PosX, "Pos X", "Blend", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::PosY, "Pos Y", "Blend", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},


    // SOUNDS C..H — identity only; their placement is in the block below with everyone else's. Zone
    // defaults are spread across the range rather than stacked at zero, so a constellation that is
    // switched on has somewhere to go rather than eight copies of the same character.
    {ParamId::CZone, "C Zone", "Sound C", 0.0, 1023.0, 320.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::COffset, "C Offset", "Sound C", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::CGamma, "C Gamma", "Sound C", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::DZone, "D Zone", "Sound D", 0.0, 1023.0, 448.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::DOffset, "D Offset", "Sound D", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::DGamma, "D Gamma", "Sound D", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::EZone, "E Zone", "Sound E", 0.0, 1023.0, 576.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::EOffset, "E Offset", "Sound E", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::EGamma, "E Gamma", "Sound E", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::FZone, "F Zone", "Sound F", 0.0, 1023.0, 704.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::FOffset, "F Offset", "Sound F", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::FGamma, "F Gamma", "Sound F", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::GZone, "G Zone", "Sound G", 0.0, 1023.0, 832.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::GOffset, "G Offset", "Sound G", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::GGamma, "G Gamma", "Sound G", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::HZone, "H Zone", "Sound H", 0.0, 1023.0, 960.0, kContinuous, ParamUnit::Zone, {}},
    {ParamId::HOffset, "H Offset", "Sound H", 0.0, 2.0, 0.0, kContinuous, ParamUnit::Number, {}},
    {ParamId::HGamma, "H Gamma", "Sound H", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},

    // PLACEMENT. Bipolar and centred, the same units Pos X/Y already use, because they are the same
    // plane: a sound's placement and the puck's position are measured against each other and a second
    // scale would be a bug waiting for its moment.
    //
    // The defaults put A and B on a horizontal segment through the origin — which is exactly the A→B
    // path v1 had, restated spatially — and the other six around a circle so that switching one on
    // gives it somewhere to be. Enable defaults on for A and B, off for the rest: a patch that says
    // nothing is the two-sound instrument v1 was.
    //
    // WIDTH IS THE FOURTH FIELD IN EACH BLOCK (V13) and the sound's first RIDE-ALONG (V18): how far
    // apart the two ears' evaluations of THIS sound's anchor stand — a setting of the sound's
    // character that follows the sound through clone and stance, where the placements stay with the
    // slot. Effective separation is global Width (0x0043) × this — the global is the master scale.
    // Defaults to zero: that sound is mono, and a patch that says nothing renders today's mono path
    // to the bit.
    {ParamId::APlaceX, "A Place X", "Sound A", -1.0, 1.0, -0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::APlaceY, "A Place Y", "Sound A", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::AEnable, "A Enable", "Sound A", 0.0, 1.0, 1.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::AWidth, "A Width", "Sound A", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BPlaceX, "B Place X", "Sound B", -1.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BPlaceY, "B Place Y", "Sound B", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BEnable, "B Enable", "Sound B", 0.0, 1.0, 1.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::BWidth, "B Width", "Sound B", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CPlaceX, "C Place X", "Sound C", -1.0, 1.0, 0.65, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CPlaceY, "C Place Y", "Sound C", -1.0, 1.0, 0.375, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CEnable, "C Enable", "Sound C", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::CWidth, "C Width", "Sound C", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DPlaceX, "D Place X", "Sound D", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DPlaceY, "D Place Y", "Sound D", -1.0, 1.0, 0.75, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DEnable, "D Enable", "Sound D", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::DWidth, "D Width", "Sound D", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EPlaceX, "E Place X", "Sound E", -1.0, 1.0, -0.65, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EPlaceY, "E Place Y", "Sound E", -1.0, 1.0, 0.375, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EEnable, "E Enable", "Sound E", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::EWidth, "E Width", "Sound E", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FPlaceX, "F Place X", "Sound F", -1.0, 1.0, -0.65, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FPlaceY, "F Place Y", "Sound F", -1.0, 1.0, -0.375, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FEnable, "F Enable", "Sound F", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::FWidth, "F Width", "Sound F", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GPlaceX, "G Place X", "Sound G", -1.0, 1.0, -0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GPlaceY, "G Place Y", "Sound G", -1.0, 1.0, -0.75, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GEnable, "G Enable", "Sound G", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::GWidth, "G Width", "Sound G", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HPlaceX, "H Place X", "Sound H", -1.0, 1.0, 0.65, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HPlaceY, "H Place Y", "Sound H", -1.0, 1.0, -0.375, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HEnable, "H Enable", "Sound H", 0.0, 1.0, 0.0, kStepped, ParamUnit::Choice, kEnableNames},
    {ParamId::HWidth, "H Width", "Sound H", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},

    // THE PER-SOUND FLAGSHIP CONTROLS (params.h doc §11). Default 1.0 = full field authority, so every
    // patch that predates them renders exactly as it did; turning a knob down is a deliberate act.
    // FmDepth scales that sound's FM contribution (0 = off). Smoothness scales the smoothness field
    // (1 = current verified behavior, 0 = plain morph). Both modulatable — see kContinuous.
    {ParamId::AFmDepth, "A FM Depth", "Sound A", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::ASmoothness, "A Smoothness", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BFmDepth, "B FM Depth", "Sound B", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BSmoothness, "B Smoothness", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CFmDepth, "C FM Depth", "Sound C", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CSmoothness, "C Smoothness", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DFmDepth, "D FM Depth", "Sound D", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DSmoothness, "D Smoothness", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EFmDepth, "E FM Depth", "Sound E", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::ESmoothness, "E Smoothness", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FFmDepth, "F FM Depth", "Sound F", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FSmoothness, "F Smoothness", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GFmDepth, "G FM Depth", "Sound G", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GSmoothness, "G Smoothness", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HFmDepth, "H FM Depth", "Sound H", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HSmoothness, "H Smoothness", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    // THE FIVE FIELD MODIFIER KNOBS (2026-08-25).
    // Each scales the terrain field that authors its quantity through fieldWithOverride, the curve the
    // Smoothness knob above already uses.
    //
    // DEFAULT 0.5 IS THE FIXED POINT OF THAT CURVE, kFieldNeutral — the field exactly as the terrain
    // authored it. It is not a compatibility concession (there are no patches to protect): it is the
    // RIGHT default, because these fields ARE the flagship's character and a modifier over them should
    // rest where the character is. A default anywhere else would mean every new patch arrives with the
    // terrain already argued with.
    //
    // ALL FIVE ARE kContinuous, so they are modulation destinations by the same rule that admitted
    // Width, FM Depth and Smoothness: every continuous performance parameter is modulatable.
    {ParamId::AQuantize, "A Quantize", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::ASync, "A Sync", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::AFeedback, "A Feedback", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::APoints, "A Points", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::AGrit, "A Grit", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BQuantize, "B Quantize", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BSync, "B Sync", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BFeedback, "B Feedback", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BPoints, "B Points", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BGrit, "B Grit", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CQuantize, "C Quantize", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CSync, "C Sync", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CFeedback, "C Feedback", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CPoints, "C Points", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CGrit, "C Grit", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DQuantize, "D Quantize", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DSync, "D Sync", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DFeedback, "D Feedback", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DPoints, "D Points", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DGrit, "D Grit", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EQuantize, "E Quantize", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::ESync, "E Sync", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EFeedback, "E Feedback", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EPoints, "E Points", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EGrit, "E Grit", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FQuantize, "F Quantize", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FSync, "F Sync", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FFeedback, "F Feedback", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FPoints, "F Points", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FGrit, "F Grit", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GQuantize, "G Quantize", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GSync, "G Sync", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GFeedback, "G Feedback", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GPoints, "G Points", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GGrit, "G Grit", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HQuantize, "H Quantize", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HSync, "H Sync", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HFeedback, "H Feedback", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HPoints, "H Points", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HGrit, "H Grit", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    // THE WARP DEPTH KNOBS — **DEFAULT 0, NOT 0.5**, and the difference from the five rows above is
    // the whole design. Those bias a phi field, where 0.5 means "as the terrain authored it"; this is
    // an AMOUNT with no field behind it, where 0 means none. `flagship.h` records what it cost to get
    // that distinction wrong once already.
    {ParamId::AWarp, "A Warp", "Sound A", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BWarp, "B Warp", "Sound B", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CWarp, "C Warp", "Sound C", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DWarp, "D Warp", "Sound D", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EWarp, "E Warp", "Sound E", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FWarp, "F Warp", "Sound F", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GWarp, "G Warp", "Sound G", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HWarp, "H Warp", "Sound H", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},

    // THE ADDITIVE SHAPE AMOUNT KNOBS (2026-10-10) — default 0 for Warp's reason: an amount, not a bias. Unkeyed, like
    // every per-sound row (JF step 2 keys them together).
    {ParamId::AShapeAmount, "A Shape Amount", "Sound A", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BShapeAmount, "B Shape Amount", "Sound B", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CShapeAmount, "C Shape Amount", "Sound C", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DShapeAmount, "D Shape Amount", "Sound D", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EShapeAmount, "E Shape Amount", "Sound E", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FShapeAmount, "F Shape Amount", "Sound F", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GShapeAmount, "G Shape Amount", "Sound G", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HShapeAmount, "H Shape Amount", "Sound H", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},

    // THE ADDITIVE BIAS KNOBS (2026-10-10, design §4.1) — default 0.5 (kFieldNeutral) for the five field modifiers'
    // reason: a bias over a terrain field, where the middle is the field as authored. Unkeyed, like every per-sound row.
    {ParamId::ARolloff, "A Rolloff", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BRolloff, "B Rolloff", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CRolloff, "C Rolloff", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DRolloff, "D Rolloff", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::ERolloff, "E Rolloff", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FRolloff, "F Rolloff", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GRolloff, "G Rolloff", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HRolloff, "H Rolloff", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ParamId::AFormant, "A Formant", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BFormant, "B Formant", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CFormant, "C Formant", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DFormant, "D Formant", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EFormant, "E Formant", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FFormant, "F Formant", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GFormant, "G Formant", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HFormant, "H Formant", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ParamId::AOddEven, "A Odd/Even", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BOddEven, "B Odd/Even", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::COddEven, "C Odd/Even", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DOddEven, "D Odd/Even", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EOddEven, "E Odd/Even", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FOddEven, "F Odd/Even", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GOddEven, "G Odd/Even", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HOddEven, "H Odd/Even", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ParamId::AComb, "A Comb", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BComb, "B Comb", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CComb, "C Comb", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DComb, "D Comb", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EComb, "E Comb", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FComb, "F Comb", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GComb, "G Comb", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HComb, "H Comb", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ParamId::ADetail, "A Detail", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BDetail, "B Detail", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::CDetail, "C Detail", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DDetail, "D Detail", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EDetail, "E Detail", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FDetail, "F Detail", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GDetail, "G Detail", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HDetail, "H Detail", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ParamId::AOffGrid, "A Off-grid", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::BOffGrid, "B Off-grid", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::COffGrid, "C Off-grid", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::DOffGrid, "D Off-grid", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::EOffGrid, "E Off-grid", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::FOffGrid, "F Off-grid", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GOffGrid, "G Off-grid", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ParamId::HOffGrid, "H Off-grid", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    // THE STRING'S FIELD SCALERS (2026-10-31, KS design §4.1) — default 0.5 (kFieldNeutral): a bias over a terrain field,
    // the middle the field as authored. Unkeyed, like every per-sound row.
    {ksBrightnessParam(0), "A Pluck Brightness", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksBrightnessParam(1), "B Pluck Brightness", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksBrightnessParam(2), "C Pluck Brightness", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksBrightnessParam(3), "D Pluck Brightness", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksBrightnessParam(4), "E Pluck Brightness", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksBrightnessParam(5), "F Pluck Brightness", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksBrightnessParam(6), "G Pluck Brightness", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksBrightnessParam(7), "H Pluck Brightness", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ksSustainParam(0), "A Pluck Sustain", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksSustainParam(1), "B Pluck Sustain", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksSustainParam(2), "C Pluck Sustain", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksSustainParam(3), "D Pluck Sustain", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksSustainParam(4), "E Pluck Sustain", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksSustainParam(5), "F Pluck Sustain", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksSustainParam(6), "G Pluck Sustain", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksSustainParam(7), "H Pluck Sustain", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ksPickParam(0), "A Pluck Pick", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksPickParam(1), "B Pluck Pick", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksPickParam(2), "C Pluck Pick", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksPickParam(3), "D Pluck Pick", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksPickParam(4), "E Pluck Pick", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksPickParam(5), "F Pluck Pick", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksPickParam(6), "G Pluck Pick", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksPickParam(7), "H Pluck Pick", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ksNoiseParam(0), "A Pluck Noise", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksNoiseParam(1), "B Pluck Noise", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksNoiseParam(2), "C Pluck Noise", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksNoiseParam(3), "D Pluck Noise", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksNoiseParam(4), "E Pluck Noise", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksNoiseParam(5), "F Pluck Noise", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksNoiseParam(6), "G Pluck Noise", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksNoiseParam(7), "H Pluck Noise", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},

    {ksStretchParam(0), "A Pluck Stretch", "Sound A", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksStretchParam(1), "B Pluck Stretch", "Sound B", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksStretchParam(2), "C Pluck Stretch", "Sound C", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksStretchParam(3), "D Pluck Stretch", "Sound D", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksStretchParam(4), "E Pluck Stretch", "Sound E", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksStretchParam(5), "F Pluck Stretch", "Sound F", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksStretchParam(6), "G Pluck Stretch", "Sound G", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},
    {ksStretchParam(7), "H Pluck Stretch", "Sound H", 0.0, 1.0, 0.5, kContinuous, ParamUnit::Percent, {}},


    // Relief is complexity-field depth (0 = even world, 1 = dramatic) — an amount, hence bounded.
    // Dormant in v1. Global Gamma is the macro offset over both sounds' γ coordinates: per-sound γ is
    // the note, this is the bend wheel.
    {ParamId::Relief, "Relief", "Character", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::GlobalGamma, "Global Gamma", "Character", -4.0, 4.0, 0.0, kContinuous, ParamUnit::Number, {}},

    // WEATHER IS HOW MUCH OF THE TERRAIN YOU GET. At zero the blend is purely relative — sounds
    // crossfade and each one is exactly itself, which is what the engine has always done. Turned up,
    // where you are STANDING starts to colour what you hear, and at full you are walking the space
    // rather than mixing points in it..
    //
    // Defaults to zero, so every patch that predates it sounds exactly as it did and turning it up is
    // a deliberate act. Bounded because it is an amount rather than a coordinate.
    {ParamId::Weather, "Weather", "Character", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},

    // WIDTH IS THE MASTER SCALE over the per-sound widths — terrain stereo, recast by V13: the
    // effective ear separation for sound k is THIS × w_k (the sound's own 0x02k3 parameter), each
    // ear seeing that sound's anchor displaced ±(this·w_k)/2 along the disk's X, so decorrelation
    // comes from the terrain itself and scales with the local rate-spread. Full separation stands
    // the ears half the disk apart (decision V12b — the range errs small until listening asks for
    // more; widening later is the legitimate deliberate act, narrowing breaks patches).
    //
    // THE DEFAULT IS 1, MOVED DELIBERATELY FROM 0 (V13b — a fingerprint change, quoted in
    // tests/test_params.cpp): the default patch stays mono because the per-sound widths default to
    // 0, and a scale that rested at zero would make every new width knob a knob that does nothing
    // until a second knob is found. A patch from the brief global-width era (this parameter > 0,
    // no per-sound values) comes back mono — accepted on the record, pre-release patches of a single user.
    // V5 ONLY (by design): the frozen species stay strictly mono at every width, so on a non-V5 patch
    // this parameter changes nothing and any control bound to it must say so — the honesty rule's
    // V11 consequence.
    {ParamId::Width, "Width", "Character", 0.0, 1.0, 1.0, kContinuous, ParamUnit::Percent, {}},

    // Per-note-id modulation as well as the global lane: this is the one amplitude a host can address
    // per voice, which is what makes MPE-style expression reach the synth at all.
    // **ZERO BY DEFAULT, BECAUSE THE DEFAULT PATCH'S ENVELOPE CARRIES IT** (backlog EF). Routes ADD, and at a base of 1 a route
    // clamps to nothing; at 0 the strip's envelope, routed here at full amount, IS the level, per note. The
    // knob reads as a floor a hand can put under the envelope; with no route, 0 is silence and anything above
    // it is a gate. Was "Level" until 2026-10-03, same id.
    {ParamId::VoiceLevel, "Voice Level", "Amp", 0.0, 1.0, 0.0,
     kContinuous | CLAP_PARAM_IS_MODULATABLE_PER_NOTE_ID, ParamUnit::Gain, {}, false,
     "Each note's own level. The envelope drives it in the default patch; a note sounds while this is above zero.",
     "voice.level"},
    {ParamId::MasterGain, "Master Gain", "Amp", kMasterGainMinDb, 12.0, 0.0, kContinuous,
     ParamUnit::Decibels, {}},
    // **A PLAIN FADER**, unity by default, on the
    // voices' sum before the fx lane — the place the old Level stood, so the lane is driven as it always was.
    {ParamId::Level, "Level", "Amp", 0.0, 1.0, 1.0, kContinuous, ParamUnit::Gain, {}, false,
     "The instrument's output level into the effects.", "output.level"},

    {ParamId::VoiceMode, "Voice Mode", "Voice", 0.0, lastIndexOf(kVoiceModeNames), 0.0, kStepped,
     ParamUnit::Choice, kVoiceModeNames},
    {ParamId::VoiceCount, "Voice Count", "Voice", 1.0, static_cast<double>(kMaxVoices), 8.0, kStepped,
     ParamUnit::Integer, {}, false,
     "How many notes can sound at once. A stack plays fewer, because a CPU guard caps the oscillators in all; \"(plays N)\" "
     "shows how many notes the current stack actually gets."},
    {ParamId::NotePriority, "Note Priority", "Voice", 0.0, lastIndexOf(kNotePriorityNames), 0.0, kStepped,
     ParamUnit::Choice, kNotePriorityNames},
    {ParamId::GlideTime, "Glide Time", "Voice", 0.0, 1.0, 0.0, kContinuous, ParamUnit::GlideSeconds, {}},
    // WHAT A NOTE-ON DOES TO THE RUNNING PHASE (V24): Digi restarts it, Analog never touches
    // it, Phase Catchup bends it home over ~24 cycles — §5.8's HARM port, and the DEFAULT because
    // de-clicked retriggers are the point (V21d). Beside Note Priority, because it answers the same
    // question about the same event. The parity-minded reading now has a name of its own rather than
    // being an off position: with Catchup selected, mono/legato retriggers and steals RENDER
    // differently from hardware (goldens are builder-level and unaffected).
    {ParamId::OscMode, "Osc Mode", "Voice", 0.0, lastIndexOf(kOscModeNames),
     static_cast<double>(OscMode::Catchup), kStepped, ParamUnit::Choice, kOscModeNames},
    // VOICE STACKING (backlog KE). Stack is how many oscillators a note plays — stepped and not a modulation
    // destination, because it changes what the voice IS and what it costs; a change reaches held notes live,
    // with the parts fading in and out and the fan re-spacing (synth.h, `StackPart`). Default 1: one
    // oscillator per note is the synth it has always been, bit for bit.
    {ParamId::StackCount, "Stack", "Voice", 1.0, static_cast<double>(kMaxStack), 1.0, kStepped, ParamUnit::Integer,
     {}, false, "How many oscillators each note plays, fanned in pitch by Stack Detune. More notes are stolen "
                "sooner as the stack grows, so the whole stays within the CPU budget.",
     "stack.count"},
    // The total fan, lowest part to highest, in cents. 10 cents is the
    // firmware's own default synth. Continuous and resolved per voice.
    {ParamId::StackDetune, "Stack Detune", "Voice", 0.0, kStackDetuneMaxCents, 10.0, kContinuous,
     ParamUnit::Cents, {}, false,
     "How far apart in pitch the stacked oscillators are, from the lowest to the highest.", "stack.detune"},
    // How far apart in the stereo field the parts sit (step 2): 0 is every part centred and the voice mono, 1 is
    // the outermost parts hard left and right. The firmware's alternating fan, so pitch neighbours sit apart.
    {ParamId::StackSpread, "Stack Spread", "Voice", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, false,
     "How wide the stacked oscillators are spread across the stereo field.", "stack.spread"},
    // THE STACK INDEX (step 3): a value per part, −1..+1, that the Stack Index source carries into Fine Tune and
    // Level. Shape gives each part its value, Mapping decides which part gets which, Curve warps them all. The two
    // modes are stepped choices; the amounts, Free's phase and the Curve are played and resolved per voice. Linear
    // with nothing warped is the firmware's plain fan, part k at k/(N−1) doubled to −1..+1 (voice-stacking doc §3.2).
    {ParamId::StackShape, "Stack Shape", "Voice", 0.0, lastIndexOf(kStackShapeNames),
     static_cast<double>(StackShape::Linear), kStepped, ParamUnit::Choice, kStackShapeNames, false,
     "How the Stack Index is shaped across the stacked oscillators, lowest to highest.", "stack.shape"},
    {ParamId::StackShapeAmount, "Stack Shape Amount", "Voice", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {},
     false, "The chosen shape's own amount: its bias, power, steepness, steps, peak, cycles, depth or drift rate.",
     "stack.shape_amount"},
    {ParamId::StackFreePhase, "Stack Free Phase", "Voice", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Number, {}, false,
     "Where the Free shape reads the golden-ratio field; each oscillator reads a golden step further on.",
     "stack.free_phase"},
    {ParamId::StackMap, "Stack Mapping", "Voice", 0.0, lastIndexOf(kStackMapNames),
     static_cast<double>(StackMap::AsShaped), kStepped, ParamUnit::Choice, kStackMapNames, false,
     "Which stacked oscillator is given which of the shape's values.", "stack.map"},
    {ParamId::StackMapAmount, "Stack Mapping Amount", "Voice", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {},
     false, "The chosen mapping's own amount: how tight, how far, which focus, how many to a group, which shuffle.",
     "stack.map_amount"},
    {ParamId::StackCurve, "Stack Curve", "Voice", -1.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, false,
     "Warps every Stack Index value: positive pulls them toward the centre, negative pushes them to the edges.",
     "stack.curve"},
    // **THE OSCILLATOR'S READ PHASE, IN CYCLES** (2026-10-03). The knob's travel is one cycle; a route may carry it
    // past either end (`offsetsMayLeaveRange`), because phase WRAPS — a clamp would pin a modulated phase at an end,
    // which is a hard nonlinearity rather than phase modulation.
    {ParamId::OscPhase, "Phase", "Voice", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, true,
     "Where in its cycle the oscillator reads. Modulate it slowly for drift, quickly for phase modulation.",
     "osc.phase", /* stereo: a pair routed here reaches the ears apart (backlog KU, 2026-10-05) */ true},
    {ParamId::OscPhaseWidth, "Phase Width", "Voice", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}, false,
     "Mirrors Phase's modulation into the right ear: at full width the two ears drift opposite ways.",
     "osc.phase_width"},

    {ParamId::TransposeOct, "Transpose Oct", "Pitch", -4.0, 4.0, 0.0, kStepped, ParamUnit::Octaves, {}},
    {ParamId::TransposeSemi, "Transpose Semi", "Pitch", -12.0, 12.0, 0.0, kStepped, ParamUnit::Semitones, {}},
    {ParamId::FineTune, "Fine Tune", "Pitch", -kFineTuneMaxCents, kFineTuneMaxCents, 0.0, kContinuous, ParamUnit::Cents,
     {}},
    {ParamId::Coarse, "Coarse", "Pitch", -kCoarseMaxSemitones, kCoarseMaxSemitones, 0.0, kContinuous,
     ParamUnit::Semitones, {}, false,
     "A continuous pitch offset in semitones. Route a modulator to it for wide, smooth pitch sweeps.", "osc.coarse"},
    // Index into kBendRangeSemitones; default index 3 is ±12 semitones, an octave either way (2026-10-03:
    // the right default, not a compatibility choice).
    {ParamId::BendRange, "Bend Range", "Pitch", 0.0, static_cast<double>(kBendRangeSemitones.size() - 1), 3.0,
     kStepped, ParamUnit::BendChoice, {}},

    // THE MACROS. Unipolar and resting at zero, so a patch that has routed one and left the knob down
    // sounds exactly like a patch that has not — the amount says how far the source can reach, and a
    // source at zero reaches nowhere. Automatable AND modulatable, the second deliberately: a host
    // modulator driving a macro that fans out across the whole destination list is the whole point of a
    // macro, and it terminates because a macro is never a destination itself.
    {ParamId::Macro1, "Macro 1", "Macros", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::Macro2, "Macro 2", "Macros", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::Macro3, "Macro 3", "Macros", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},
    {ParamId::Macro4, "Macro 4", "Macros", 0.0, 1.0, 0.0, kContinuous, ParamUnit::Percent, {}},

    // THE FX LANE'S FIRST TENANT HAS LEFT THIS TABLE. Its eleven rows sat here until the filter was
    // swallowed; a docked device's controls are its own state now, carried in its chain row and
    // reached by the route table rather than by a clap id. See ParamId for what that costs.
};

#undef FEATHERS_SPECTRAL_ROWS
#undef FEATHERS_DRUM_ROWS
static_assert(std::size(kParams) == kParamCount);

/// EVERY CORE KEY IS ONE A PATCH MAY STORE A VALUE UNDER, AND NO TWO ROWS SHARE ONE — the device registry's
/// compile-time rule (JF §3.3), applied to the core rows that declare a key. A row without one is still stored
/// by CLAP id, and is not checked here until JF step 2 gives it one.
constexpr bool coreKeysSound() {
	// Sorted by hash, not compared pairwise (device::keysDistinct): the O(n^2) walk outgrew clang's constexpr step limit.
	std::vector<const char*> keys;
	for (size_t i = 0; i < std::size(kParams); ++i) {
		if (kParams[i].key == nullptr) {
			continue;
		}
		if (!device::validStateKey(kParams[i].key)) {
			return false;
		}
		keys.push_back(kParams[i].key);
	}
	return device::keysDistinct(keys);
}
static_assert(coreKeysSound(), "a core parameter's key is malformed or used twice");

// THE DRIFT ASSERTION WENT WITH THE BLOCK. It guarded a real hazard — a control appended in src/fx
// with no id to sit at would have vanished silently — and there is no longer an id block for it to
// drift from. The device's row is built from its own descriptor list by position, so a control
// appended there is carried by construction rather than by a matching edit here.

/// Bring a value from outside — a host event, a text field, a state file — into the range and shape
/// this param actually accepts. Returns false if there is no such value, in which case the caller
/// must leave the param alone rather than substitute something.
///
/// The NaN check is the load-bearing part and cannot be left to the clamp. std::clamp compares, and
/// every comparison against NaN is false, so clamp(NaN, min, max) returns NaN unharmed. A NaN that
/// reaches base_ is not a transient glitch either: it prints as "nan", multiplies every sample it
/// touches into silence, and gets written to the patch on the next save, so a single bad event
/// poisons the file.
bool sanitizeValue(const ParamDesc& desc, double value, double* out) {
	if (!std::isfinite(value)) {
		return false;
	}
	// Stepped params are declared to the host as taking whole values, so a fractional one is not a
	// setting they have — CLAP's contract, and a "3.7 voices" that survived would make params.value
	// disagree with everything downstream that rounds it.
	const double shaped = (desc.flags & CLAP_PARAM_IS_STEPPED) != 0 ? std::round(value) : value;
	*out = std::clamp(shaped, desc.min, desc.max);
	return true;
}

/// Format seconds the way a musician reads them: milliseconds until a second, then seconds.
void formatSeconds(double seconds, char* out, uint32_t capacity) {
	if (seconds < 1.0) {
		std::snprintf(out, capacity, "%.2f ms", seconds * 1000.0);
	}
	else {
		std::snprintf(out, capacity, "%.3f s", seconds);
	}
}

/// Parse the leading number of a host-supplied string. Returns false if there isn't one, so a typo
/// leaves the param where it was rather than snapping it to zero.
///
/// Non-finite parses are refused for the same reason: strtod happily reads "nan" and "inf", and
/// there is no param here for which either is a setting. The one display that legitimately reads
/// "-inf" is handled by name in textToValue before this is reached.
///
/// That also means a number too large to represent — "-1e400", which strtod overflows to -infinity —
/// is refused rather than clamped to the minimum. Deliberate: someone typing it has not expressed a
/// setting, and leaving the param where it was is the same answer typing "banana" gets.
bool parseLeadingDouble(const char* text, double* out) {
	char* end = nullptr;
	const double v = std::strtod(text, &end);
	if (end == text || !std::isfinite(v)) {
		return false;
	}
	*out = v;
	return true;
}

/// True if `text` begins with `prefix`, ignoring case.
bool equalsIgnoreCasePrefix(const char* text, const char* prefix) {
	for (; *prefix != '\0'; ++prefix, ++text) {
		if (std::tolower(static_cast<unsigned char>(*text)) != std::tolower(static_cast<unsigned char>(*prefix))) {
			return false;
		}
	}
	return true;
}

bool equalsIgnoreCase(const char* a, const char* b) {
	while (*a != '\0' && *b != '\0') {
		if (std::tolower(static_cast<unsigned char>(*a)) != std::tolower(static_cast<unsigned char>(*b))) {
			return false;
		}
		++a;
		++b;
	}
	return *a == *b;
}

/// True if the text is the "-inf" a silent fader prints, in either the plugin's own spelling or the
/// one a user is likely to type back.
bool mentionsNegativeInfinity(const char* text) {
	while (*text == ' ') {
		++text;
	}
	if (*text != '-') {
		return false;
	}
	++text;
	return equalsIgnoreCasePrefix(text, "inf");
}

/// True if `text` carries a unit suffix mentioning seconds rather than milliseconds. "12 s" is
/// twelve seconds; "12" and "12 ms" are both twelve milliseconds, because that is what the plugin
/// printed.
/// Whether a typed frequency is in kilohertz. The same shape as mentionsWholeSeconds below and for
/// the same reason: "2 kHz" and "2 Hz" are three orders of magnitude apart, so the suffix decides the
/// number rather than decorating it. A bare number is hertz, which is what the display prints for
/// everything under a kilohertz.
bool mentionsKilohertz(const char* text) {
	for (const char* s = std::strchr(text, 'k'); s != nullptr; s = std::strchr(s + 1, 'k')) {
		if (std::tolower(static_cast<unsigned char>(s[1])) == 'h') {
			return true;
		}
	}
	for (const char* s = std::strchr(text, 'K'); s != nullptr; s = std::strchr(s + 1, 'K')) {
		if (std::tolower(static_cast<unsigned char>(s[1])) == 'h') {
			return true;
		}
	}
	return false;
}

bool mentionsWholeSeconds(const char* text) {
	const char* s = std::strchr(text, 's');
	while (s != nullptr) {
		if (s == text || std::tolower(static_cast<unsigned char>(s[-1])) != 'm') {
			return true;
		}
		s = std::strchr(s + 1, 's');
	}
	return false;
}

} // namespace

size_t steppedIndex(double value, size_t count) {
	const long last = static_cast<long>(count) - 1;
	return static_cast<size_t>(std::clamp(std::lround(value), 0L, last));
}

std::span<const ParamDesc> paramDescs() {
	return {kParams, kParamCount};
}

namespace {

/// WHERE EACH ID SITS IN `kParams`, -1 where none does — built once, at compile time (backlog FI: the scan this replaced
/// ran on every `resolved()`, and the tick reads dozens of parameters).
constexpr std::array<int16_t, kParamIdSpan> kParamIndexOfId = [] {
	std::array<int16_t, kParamIdSpan> table {};
	table.fill(-1);
	for (size_t i = 0; i < kParamCount; ++i) {
		const auto at = static_cast<size_t>(kParams[i].id);
		if (table.at(at) == -1) { // the FIRST, as the scan answered
			table.at(at) = static_cast<int16_t>(i);
		}
	}
	return table;
}();

} // namespace

int paramIndexOf(clap_id id) {
	return id < kParamIdSpan ? kParamIndexOfId[id] : -1;
}

const ParamDesc* findParam(clap_id id) {
	const int index = paramIndexOf(id);
	return index < 0 ? nullptr : &kParams[index];
}

namespace {

struct Fnv1a {
	uint64_t hash{0xcbf29ce484222325ULL};

	void byte(uint8_t b) {
		hash ^= b;
		hash *= 0x100000001b3ULL;
	}
	void u32(uint32_t v) {
		for (int shift = 0; shift < 32; shift += 8) {
			byte(static_cast<uint8_t>((v >> shift) & 0xffU));
		}
	}
	/// Doubles go in as their bit pattern, so a range that moved by one representable step is as
	/// visible as one that moved by half its span.
	void real(double v) {
		uint64_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		for (int shift = 0; shift < 64; shift += 8) {
			byte(static_cast<uint8_t>((bits >> shift) & 0xffU));
		}
	}
	void string(const char* s) {
		for (const char* c = s; *c != '\0'; ++c) {
			byte(static_cast<uint8_t>(*c));
		}
		byte(0);
	}
};

} // namespace

uint64_t paramIdFingerprint() {
	Fnv1a fnv;
	for (const ParamDesc& desc : kParams) {
		fnv.u32(static_cast<uint32_t>(desc.id));
		fnv.string(desc.name);
	}
	return fnv.hash;
}

uint64_t paramRangeFingerprint() {
	Fnv1a fnv;
	for (const ParamDesc& desc : kParams) {
		fnv.u32(static_cast<uint32_t>(desc.id));
		fnv.real(desc.min);
		fnv.real(desc.max);
		fnv.real(desc.def);
	}
	return fnv.hash;
}

double attackReleaseSeconds(double logSeconds) {
	return std::exp2(logSeconds);
}

double glideSeconds(double taper01) {
	const double x = std::clamp(taper01, 0.0, 1.0);
	return kGlideMaxSeconds * x * x * x;
}

double decibelsToGain(double db) {
	if (db <= kMasterGainMinDb) {
		return 0.0;
	}
	return std::pow(10.0, db / 20.0);
}

int bendRangeSemitonesAt(double index) {
	return kBendRangeSemitones[steppedIndex(index, kBendRangeSemitones.size())];
}

ParamSet::ParamSet() {
	static_assert(std::atomic<double>::is_always_lock_free,
	              "base values are read from the main thread while the audio thread writes them; a "
	              "std::atomic that took a lock would take it inside process()");
	reset();
}

void ParamSet::reset() {
	for (size_t i = 0; i < kParamCount; ++i) {
		base_[i].store(kParams[i].def, std::memory_order_relaxed);
		mod_[i] = 0.0;
	}
	// NO ROUTES: the snapshot emptied, every range zeroed, both flags down — and the publish count moved,
	// so a synth comparing it knows to rebuild the engine's inputs from the empty list too.
	stagedCount_ = 0;
	for (CoreRange& range : coreRanges_) {
		range.count.store(0, std::memory_order_relaxed);
		range.varies.store(false, std::memory_order_relaxed);
		range.first.store(0, std::memory_order_relaxed);
	}
	anyRoute_.store(false, std::memory_order_relaxed);
	anyDeviceSourceRoute_.store(false, std::memory_order_relaxed);
	++routePublishes_;
	// **AND THE GENERATION GOES BACK TO "NOTHING APPLIED", or this reset is a one-way door.**
	//
	// `publishEditorLanes` skips the publish when the engine's generation matches the bridge's. A
	// reset that emptied the routes WITHOUT clearing the generation would leave the engine agreeing
	// that it is already up to date — so the routes would stay gone until a hand next touched a ring,
	// while the editor went on drawing every one of them. Zero is below every generation the bridge
	// can hand out (it starts at 1), so the next publish always lands.
	//
	// **THIS FUNCTION HAS NO CALLERS TODAY**, which is exactly why the line is here rather than in a
	// note: the trap is for whoever adds the first one, and it would present as routes that silently
	// stopped working after a reset rather than as anything pointing here.
	routesGeneration_.store(0, std::memory_order_relaxed);
}

void ParamSet::reserveRoutes(size_t routes) {
	// IN PLACE, ON THE MESSAGE THREAD — chunks added, nothing moved, so the drain writing an input and the editor
	// reading one are both undisturbed (backlog OJ). Before the bridge publishes the count that needs it.
	staged_.grow(routes);
	coreInputs_.grow(routes);
}

bool ParamSet::adoptRoutes(const backplane::RouteList& published) {
	// **NOTHING MOVED — every block on which no hand touched a ring.**
	const uint32_t offered = published.sequence();
	if (offered == routesGeneration_.load(std::memory_order_relaxed)) {
		return false;
	}
	// **THE SNAPSHOT, INTO `staged_`, AND NOTHING OF OURS IS TOUCHED UNLESS IT WAS WHOLE.** A copy that
	// straddled the editor's publish would lose routes nobody moved; a torn one keeps the list already
	// staged and asks again next block.
	size_t count = 0;
	uint32_t seen = 0;
	backplane::RouteList::Refused why = backplane::RouteList::Refused::None;
	if (!published.snapshot(staged_, &count, &seen, &why)) {
		// NO ROOM IS COUNTED, NOT TRUNCATED: the stores were never grown for this list (an unwired host).
		routeRoomRefusals_ += why == backplane::RouteList::Refused::NoRoom ? 1 : 0;
		return false;
	}
	stagedCount_ = count;
	routesGeneration_.store(seen, std::memory_order_relaxed);
	// THE COUNT THAT MOVES ON EVERY PUBLISH THAT ACTUALLY RAN — what `Synth::compileRoutes` compares.
	++routePublishes_;
	// THE ENVELOPE QUESTION, ASKED OF WHAT WAS ADOPTED: does any route leave a device at all? The engine
	// asks it 750 times a second, so it is a flag set here rather than a walk there.
	bool deviceSourceRouted = false;
	for (size_t i = 0; i < count; ++i) {
		deviceSourceRouted = deviceSourceRouted || staged_[i].source.isDevice();
	}
	anyRoute_.store(count > 0, std::memory_order_relaxed);
	anyDeviceSourceRoute_.store(deviceSourceRouted, std::memory_order_relaxed);
	return true;
}

void ParamSet::rebuildCoreInputs(const DeviceCells& cells) {
	// **THIS NODE'S INPUTS FROM THE STAGED ROUTES — the whole plan for the synth's parameters, rebuilt at
	// the drain (kill-lanes §2.4b).** The same two passes `RackEngine::rebuildInputs` makes over the
	// same routes, for the destinations that are the HOST's controls rather than a device's.
	perVoiceCells_ = cells.voices;

	// WHERE A ROUTE'S SOURCE IS, OR NOWHERE. A host source is one of this object's own cells; a device
	// source is wherever the synth says the engine keeps that device's output.
	const auto placeSource = [&](const backplane::Source& s, const float** value, const float** perVoice) {
		*perVoice = nullptr;
		if (s.isDevice()) {
			*value = cells.cellOf != nullptr ? cells.cellOf(cells.host, s.device, s.index) : nullptr;
			if (*value != nullptr && cells.perVoiceCellOf != nullptr) {
				*perVoice = cells.perVoiceCellOf(cells.host, s.device, s.index);
			}
			return *value != nullptr;
		}
		*value = s.index < coreCells_.size() ? &coreCells_[s.index] : nullptr;
		return *value != nullptr;
	};
	// BOTH REGIONS OF THE CORE INDEX SPACE ARE THIS NODE'S — the parameters and, since 2026-09-21, the
	// internal destinations above them (params.h). What is refused is a device's control and an index
	// past the whole table; `kCoreDestinationCount` is the "not ours" answer and every user tests it.
	// AND AN EDIT-ONLY DESTINATION (a sound's identity, backlog PQ) IS NOT COMPILED AT ALL: a route there is a Relative
	// macro's, whose whole effect is the base it edits on the message thread, so the engine adds nothing for it —
	// and an Absolute route that reached one (a macro switched back from Relative) stays inert rather than
	// modulating the identity Explore draws from its base (A32).
	const auto destinationOf = [](const backplane::Destination& d) -> size_t {
		return d.isDevice() || d.index >= kCoreDestinationCount || isEditOnlyDestination(d.index) ? kCoreDestinationCount
		                                                                                         : d.index;
	};

	// EVERY RANGE TO ZERO FIRST — a reader mid-rebuild sees no modulation on a parameter rather than a
	// range into inputs being rewritten under it.
	for (CoreRange& range : coreRanges_) {
		range.count.store(0, std::memory_order_relaxed);
		range.varies.store(false, std::memory_order_relaxed);
	}
	// PASS ONE: COUNT.
	std::array<uint32_t, kCoreDestinationCount> counts {};
	for (size_t i = 0; i < stagedCount_; ++i) {
		const backplane::Route& r = staged_[i];
		const float* value = nullptr;
		const float* perVoice = nullptr;
		const size_t d = destinationOf(r.destination);
		if (d < kCoreDestinationCount && placeSource(r.source, &value, &perVoice)) {
			++counts[d];
		}
	}
	// LAY OUT, then the counts become fill cursors.
	std::array<uint32_t, kCoreDestinationCount> firsts {};
	uint32_t next = 0;
	for (size_t d = 0; d < kCoreDestinationCount; ++d) {
		firsts[d] = next;
		next += counts[d];
		counts[d] = 0;
	}
	// PASS TWO: FILL — the same tests as pass one, so the two agree on every route.
	for (size_t i = 0; i < stagedCount_; ++i) {
		const backplane::Route& r = staged_[i];
		const float* value = nullptr;
		const float* perVoice = nullptr;
		const size_t d = destinationOf(r.destination);
		if (d >= kCoreDestinationCount || !placeSource(r.source, &value, &perVoice)) {
			continue;
		}
		const size_t at = static_cast<size_t>(firsts[d]) + counts[d];
		if (at >= coreInputs_.size()) {
			continue;
		}
		CoreInput& input = coreInputs_[at];
		input.value.store(value, std::memory_order_relaxed);
		input.perVoice.store(perVoice, std::memory_order_relaxed);
		// THE WINDOW RUN beside the per-voice cell run.
		input.window.store(
		    r.source.isDevice() && perVoice == nullptr && cells.windowOf != nullptr
		        ? cells.windowOf(cells.host, r.source.device, r.source.index)
		        : nullptr,
		    std::memory_order_relaxed);
		input.perVoiceWindow.store(
		    perVoice != nullptr && cells.perVoiceWindowOf != nullptr
		        ? cells.perVoiceWindowOf(cells.host, r.source.device, r.source.index)
		        : nullptr,
		    std::memory_order_relaxed);
		// AND ITS RIGHT EAR (backlog KU): a device output's right cell, right window and pair byte — and, for a per-voice one
		// (backlog PA), its per-voice right runs. Whether it IS a pair is read at each plan (`buildWindowPlan`), not here —
		// the byte moves with the engine's fixed point.
		const bool fromDevice = r.source.isDevice();
		const bool blockDevice = fromDevice && perVoice == nullptr;
		input.valueR.store(fromDevice && cells.cellROf != nullptr ? cells.cellROf(cells.host, r.source.device, r.source.index)
		                                                                : nullptr,
		                             std::memory_order_relaxed);
		input.windowR.store(
		    blockDevice && cells.windowROf != nullptr ? cells.windowROf(cells.host, r.source.device, r.source.index) : nullptr,
		    std::memory_order_relaxed);
		input.perVoiceWindowR.store(
		    perVoice != nullptr && cells.perVoiceWindowROf != nullptr
		        ? cells.perVoiceWindowROf(cells.host, r.source.device, r.source.index)
		        : nullptr,
		    std::memory_order_relaxed);
		input.pairs.store(
		    fromDevice && cells.pairsOf != nullptr ? cells.pairsOf(cells.host, r.source.device, r.source.index) : nullptr,
		    std::memory_order_relaxed);
		input.pairBit.store(static_cast<uint8_t>(r.source.index & 63u), std::memory_order_relaxed);
		if (perVoice != nullptr) {
			coreRanges_[d].varies.store(true, std::memory_order_relaxed);
		}
		input.amount.store(r.amount, std::memory_order_relaxed);
		input.transfer.store(static_cast<uint16_t>(r.transfer), std::memory_order_relaxed);
		// A MACRO SOURCE IS RESOLVED LIVE — see `CoreInput::macro`. Its cell still exists, for the engine.
		input.macro.store(!r.source.isDevice() && r.source.index < kMacroCount
		                                ? static_cast<int>(r.source.index)
		                                : -1,
		                            std::memory_order_relaxed);
		input.source.store((uint64_t { r.source.device } << 32) | r.source.index, std::memory_order_relaxed);
		++counts[d];
	}
	// THE RANGES LAST, RELEASE — a reader that sees a count has been able to see the inputs it counts.
	for (size_t d = 0; d < kCoreDestinationCount; ++d) {
		coreRanges_[d].first.store(firsts[d], std::memory_order_relaxed);
		coreRanges_[d].count.store(counts[d], std::memory_order_release);
	}
}
void ParamSet::buildWindowPlan(ParamId id, WindowPlan& plan) const {
	// `sourceOffsetAt`'s walk, once, sorted into what is constant for the tick (the scalar), what
	// moves the same way for every voice (block-rate windows), and what varies by voice (per-voice windows, and
	// per-voice cells a source publishes without one).
	plan = WindowPlan {};
	const int index = paramIndexOf(static_cast<clap_id>(id));
	if (index < 0) {
		return;
	}
	const ParamDesc& desc = kParams[static_cast<size_t>(index)];
	plan.lo = desc.min;
	plan.hi = desc.max;
	plan.clamp = !desc.offsetsMayLeaveRange;
	plan.voices = perVoiceCells_;
	plan.scalar = base_[static_cast<size_t>(index)].load(std::memory_order_relaxed) + mod_[static_cast<size_t>(index)];
	const int destination = modDestinationIndexOf(id);
	if (!anyRoute_.load(std::memory_order_relaxed) || destination < 0
	    || static_cast<size_t>(destination) >= kParamDestinationCount) {
		return;
	}
	const double span = desc.max - desc.min;
	const CoreRange& range = coreRanges_[static_cast<size_t>(destination)];
	const uint32_t count = range.count.load(std::memory_order_acquire);
	const uint32_t first = range.first.load(std::memory_order_relaxed);
	for (size_t k = first; k < static_cast<size_t>(first) + count && k < coreInputs_.size(); ++k) {
		const CoreInput& in = coreInputs_[k];
		const double amount = in.amount.load(std::memory_order_relaxed);
		const int macro = in.macro.load(std::memory_order_relaxed);
		const auto transfer = static_cast<device::Transfer>(in.transfer.load(std::memory_order_relaxed));
		if (macro >= 0) {
			plan.scalar += amount * throughTransfer(transfer, macroReading(static_cast<size_t>(macro))) * span;
			continue;
		}
		const float* win = in.perVoiceWindow.load(std::memory_order_relaxed);
		if (win != nullptr && plan.windows < WindowPlan::kMaxInputs) {
			plan.windowAmount[plan.windows] = static_cast<float>(amount * span);
			plan.windowTransfer[plan.windows] = transfer;
			plan.window[plan.windows++] = win;
			// AND ITS EARS' DIFFERENCE PER VOICE, for a stereo destination, while the source is a pair (backlog PA).
			if (desc.stereo && plan.voicePairs < WindowPlan::kMaxInputs && pairNow(in.pairs, in.pairBit)) {
				if (const float* right = in.perVoiceWindowR.load(std::memory_order_relaxed); right != nullptr) {
					plan.voicePairAmount[plan.voicePairs] = static_cast<float>(amount * span);
					plan.voicePairTransfer[plan.voicePairs] = transfer;
					plan.voicePairLeft[plan.voicePairs] = win;
					plan.voicePairRight[plan.voicePairs++] = right;
				}
			}
			continue;
		}
		const float* perVoice = in.perVoice.load(std::memory_order_relaxed);
		if (perVoice != nullptr && plan.cells < WindowPlan::kMaxInputs) {
			plan.cellAmount[plan.cells] = static_cast<float>(amount * span);
			plan.cellTransfer[plan.cells] = transfer;
			plan.cell[plan.cells++] = perVoice;
			continue;
		}
		const float* block = in.window.load(std::memory_order_relaxed);
		if (block != nullptr && plan.blockWindows < WindowPlan::kMaxInputs) {
			plan.blockAmount[plan.blockWindows] = static_cast<float>(amount * span);
			plan.blockTransfer[plan.blockWindows] = transfer;
			plan.blockWindow[plan.blockWindows++] = block;
			// AND, FOR A STEREO DESTINATION, ITS EARS' DIFFERENCE when the source is a pair now (backlog KU). The left ear
			// above is untouched, so with no pair here this plan is the one it always was.
			if (desc.stereo && plan.pairs < WindowPlan::kMaxInputs && pairNow(in.pairs, in.pairBit)) {
				if (const float* right = in.windowR.load(std::memory_order_relaxed); right != nullptr) {
					plan.pairAmount[plan.pairs] = static_cast<float>(amount * span);
					plan.pairTransfer[plan.pairs] = transfer;
					plan.pairLeft[plan.pairs] = block;
					plan.pairRight[plan.pairs++] = right;
				}
			}
			continue;
		}
		const float* cell = in.value.load(std::memory_order_relaxed);
		if (cell != nullptr) {
			plan.scalar += amount * static_cast<double>(device::shapeTransfer(transfer, relaxedLoadCell(*cell))) * span;
		}
	}
}

bool ParamSet::resolveWindowPlanned(const WindowPlan& plan, int voice, int phase, int n, double extra,
                                    float* out) noexcept {
	// FALSE UNLESS A WINDOW IS MOVING: a flat window is its value, and the caller's scalar road already
	// has it. `voice` -1 asks for the voice-independent part alone, and is refused when a per-voice input exists.
	if ((plan.windows == 0 && plan.blockWindows == 0) || phase < 0 || phase + n > device::kTickWindow) {
		return false;
	}
	const bool anyPerVoice = plan.windows > 0 || plan.cells > 0;
	if (anyPerVoice && (voice < 0 || static_cast<size_t>(voice) >= plan.voices)) {
		return false;
	}
	const auto stride = static_cast<size_t>(device::kTickWindowStride);
	double scalar = plan.scalar + extra;
	for (size_t c = 0; c < plan.cells; ++c) {
		scalar += static_cast<double>(plan.cellAmount[c] * device::shapeTransfer(plan.cellTransfer[c], plan.cell[c][voice]));
	}
	std::array<float, WindowPlan::kMaxInputs * 2> movingAmount {};
	std::array<const float*, WindowPlan::kMaxInputs * 2> movingRun {};
	std::array<device::Transfer, WindowPlan::kMaxInputs * 2> movingTransfer {};
	size_t movingCount = 0;
	for (size_t w = 0; w < plan.windows; ++w) {
		const float* run = plan.window[w] + static_cast<size_t>(voice) * stride;
		if (device::tickWindowFlat(run)) {
			scalar += static_cast<double>(plan.windowAmount[w] * device::shapeTransfer(plan.windowTransfer[w], run[0]));
		}
		else {
			movingAmount[movingCount] = plan.windowAmount[w];
			movingTransfer[movingCount] = plan.windowTransfer[w];
			movingRun[movingCount++] = run + phase;
		}
	}
	for (size_t b = 0; b < plan.blockWindows; ++b) {
		const float* run = plan.blockWindow[b];
		if (device::tickWindowFlat(run)) {
			scalar += static_cast<double>(plan.blockAmount[b] * device::shapeTransfer(plan.blockTransfer[b], run[0]));
		}
		else {
			movingAmount[movingCount] = plan.blockAmount[b];
			movingTransfer[movingCount] = plan.blockTransfer[b];
			movingRun[movingCount++] = run + phase;
		}
	}
	if (movingCount == 0) {
		return false;
	}
	const auto s = static_cast<float>(scalar);
	for (int i = 0; i < n; ++i) {
		out[i] = s;
	}
	for (size_t m = 0; m < movingCount; ++m) {
		const float a = movingAmount[m];
		const float* run = movingRun[m];
		// THE BRANCH IS PER INPUT, NOT PER SAMPLE: a Lin route runs the loop it always ran.
		if (movingTransfer[m] == device::Transfer::Lin) {
			for (int i = 0; i < n; ++i) {
				out[i] += a * run[i];
			}
		}
		else {
			const device::Transfer t = movingTransfer[m];
			for (int i = 0; i < n; ++i) {
				out[i] += a * device::shapeTransfer(t, run[i]);
			}
		}
	}
	if (plan.clamp) {
		const auto lo = static_cast<float>(plan.lo);
		const auto hi = static_cast<float>(plan.hi);
		for (int i = 0; i < n; ++i) {
			out[i] = std::clamp(out[i], lo, hi);
		}
	}
	return true;
}
bool ParamSet::resolveEarDifferencePlanned(const WindowPlan& plan, int voice, int phase, int n, float* out) noexcept {
	// Σ amount × (R − L) over the pair inputs, per sample of the tick — each ear through the route's curve first, so the
	// right ear is exactly what the left ear's sum would have been with every pair's right window in place of its left.
	if (!plan.stereoLive() || phase < 0 || phase + n > device::kTickWindow) {
		return false;
	}
	if (plan.voicePairs > 0 && (voice < 0 || static_cast<size_t>(voice) >= plan.voices)) {
		return false;
	}
	for (int i = 0; i < n; ++i) {
		out[i] = 0.0f;
	}
	for (size_t p = 0; p < plan.pairs; ++p) {
		const float a = plan.pairAmount[p];
		const device::Transfer t = plan.pairTransfer[p];
		const float* left = plan.pairLeft[p] + phase;
		const float* right = plan.pairRight[p] + phase;
		for (int i = 0; i < n; ++i) {
			out[i] += a * (device::shapeTransfer(t, right[i]) - device::shapeTransfer(t, left[i]));
		}
	}
	// THE PER-VOICE PAIRS (backlog PA): this voice's run in each ear, `resolveWindowPlanned`'s stride.
	const size_t at = static_cast<size_t>(std::max(voice, 0)) * static_cast<size_t>(device::kTickWindowStride)
	                  + static_cast<size_t>(phase);
	for (size_t p = 0; p < plan.voicePairs; ++p) {
		const float a = plan.voicePairAmount[p];
		const device::Transfer t = plan.voicePairTransfer[p];
		const float* left = plan.voicePairLeft[p] + at;
		const float* right = plan.voicePairRight[p] + at;
		for (int i = 0; i < n; ++i) {
			out[i] += a * (device::shapeTransfer(t, right[i]) - device::shapeTransfer(t, left[i]));
		}
	}
	return true;
}

bool ParamSet::stereoLive(ParamId id) const {
	const int index = paramIndexOf(static_cast<clap_id>(id));
	const int destination = modDestinationIndexOf(id);
	if (index < 0 || !kParams[static_cast<size_t>(index)].stereo || !anyRoute_.load(std::memory_order_relaxed)
	    || destination < 0 || static_cast<size_t>(destination) >= kParamDestinationCount) {
		return false;
	}
	const CoreRange& range = coreRanges_[static_cast<size_t>(destination)];
	const uint32_t count = range.count.load(std::memory_order_acquire);
	const uint32_t first = range.first.load(std::memory_order_relaxed);
	for (size_t k = first; k < static_cast<size_t>(first) + count && k < coreInputs_.size(); ++k) {
		if (pairNow(coreInputs_[k].pairs, coreInputs_[k].pairBit)) {
			return true;
		}
	}
	return false;
}

double ParamSet::resolvedRight(ParamId id) const {
	// THE LEFT EAR'S READING, plus each pair input's (R − L) from its cells — the same terms `resolveEarDifferencePlanned`
	// sums per sample, at the cells' instant. Unclamped like that difference (see the declaration).
	double value = resolved(id);
	const int index = paramIndexOf(static_cast<clap_id>(id));
	const int destination = modDestinationIndexOf(id);
	if (index < 0 || !kParams[static_cast<size_t>(index)].stereo || !anyRoute_.load(std::memory_order_relaxed)
	    || destination < 0 || static_cast<size_t>(destination) >= kParamDestinationCount) {
		return value;
	}
	const ParamDesc& desc = kParams[static_cast<size_t>(index)];
	const double span = desc.max - desc.min;
	const CoreRange& range = coreRanges_[static_cast<size_t>(destination)];
	const uint32_t count = range.count.load(std::memory_order_acquire);
	const uint32_t first = range.first.load(std::memory_order_relaxed);
	for (size_t k = first; k < static_cast<size_t>(first) + count && k < coreInputs_.size(); ++k) {
		const CoreInput& in = coreInputs_[k];
		const float* left = in.value.load(std::memory_order_relaxed);
		const float* right = in.valueR.load(std::memory_order_relaxed);
		if (left == nullptr || right == nullptr || !pairNow(in.pairs, in.pairBit)) {
			continue;
		}
		const auto t = static_cast<device::Transfer>(in.transfer.load(std::memory_order_relaxed));
		value += static_cast<double>(in.amount.load(std::memory_order_relaxed))
		         * static_cast<double>(device::shapeTransfer(t, relaxedLoadCell(*right)) - device::shapeTransfer(t, relaxedLoadCell(*left)))
		         * span;
	}
	return value;
}

bool ParamSet::resolveWindow(ParamId id, int voice, int phase, int n, float* out) const {
	WindowPlan plan;
	buildWindowPlan(id, plan);
	return resolveWindowPlanned(plan, voice, phase, n, 0.0, out);
}

double ParamSet::perVoiceOffset(ParamId id, int voice) const {
	// `sourceOffsetAt`'s walk, kept to the inputs that carry a per-voice run — and to that run's cell for
	// this voice. A macro or a block source is not this voice's to hold on to.
	const int index = paramIndexOf(static_cast<clap_id>(id));
	if (index < 0 || voice < 0 || !anyRoute_.load(std::memory_order_relaxed)) {
		return 0.0;
	}
	const int destination = modDestinationIndexOf(id);
	if (destination < 0 || static_cast<size_t>(destination) >= kParamDestinationCount) {
		return 0.0;
	}
	const ParamDesc& desc = kParams[static_cast<size_t>(index)];
	const double span = desc.max - desc.min;
	const CoreRange& range = coreRanges_[static_cast<size_t>(destination)];
	const uint32_t count = range.count.load(std::memory_order_acquire);
	const uint32_t first = range.first.load(std::memory_order_relaxed);
	double offset = 0.0;
	for (size_t i = first; i < static_cast<size_t>(first) + count && i < coreInputs_.size(); ++i) {
		const CoreInput& in = coreInputs_[i];
		const float* perVoice = in.perVoice.load(std::memory_order_relaxed);
		if (perVoice == nullptr || static_cast<size_t>(voice) >= perVoiceCells_) {
			continue;
		}
		offset += static_cast<double>(in.amount.load(std::memory_order_relaxed))
		          * static_cast<double>(device::shapeTransfer(static_cast<device::Transfer>(in.transfer.load(std::memory_order_relaxed)),
		                                                      relaxedLoadCell(perVoice[voice])))
		          * span;
	}
	return offset;
}

bool ParamSet::variesPerVoice(ParamId id) const noexcept {
	const int destination = modDestinationIndexOf(id);
	if (destination < 0 || static_cast<size_t>(destination) >= kParamDestinationCount) {
		return false;
	}
	return coreRanges_[static_cast<size_t>(destination)].varies.load(std::memory_order_acquire);
}

bool ParamSet::isRouted(ParamId id) const noexcept {
	const int destination = modDestinationIndexOf(id);
	if (destination < 0 || static_cast<size_t>(destination) >= kParamDestinationCount
	    || !anyRoute_.load(std::memory_order_relaxed)) {
		return false;
	}
	return coreRanges_[static_cast<size_t>(destination)].count.load(std::memory_order_acquire) > 0;
}


void ParamSet::publishCoreSources() {
	// **THE HOST NODE PUBLISHES ITS OUTPUTS** — a macro's own reading, which is base plus whatever the host
	// is modulating IT by (`resolved`, not `base`), so a host modulating a macro reaches everything that
	// macro feeds. Written with `atomic_ref` because an input on another node reads the cell.
	// **READ THROUGH ITS POLARITY (backlog EG):** a macro switched bipolar publishes `2v - 1` of the
	// same 0..1 parameter, so a route from it reaches both ways from the notch and the DAW's automation
	// lane is untouched. The mask is the message thread's word (`setMacroBipolarMask`), one relaxed load.
	const uint32_t bipolar = macroBipolar_.load(std::memory_order_relaxed);
	const uint32_t relative = macroRelative_.load(std::memory_order_relaxed);
	for (size_t i = 0; i < kMacroCount; ++i) {
		// A RELATIVE MACRO RESTS AT 0, whatever its polarity says (backlog PQ): it has no value to publish.
		if ((relative >> i) & 1u) {
			std::atomic_ref<float>(coreCells_[i]).store(0.0f, std::memory_order_relaxed);
			continue;
		}
		const double v = resolved(macroParam(i));
		std::atomic_ref<float>(coreCells_[i])
		    .store(static_cast<float>((bipolar >> i) & 1u ? 2.0 * v - 1.0 : v), std::memory_order_relaxed);
	}
}

void ParamSet::setMacroBipolarMask(uint32_t mask) noexcept {
	macroBipolar_.store(mask, std::memory_order_relaxed);
}

uint32_t ParamSet::macroBipolarMask() const noexcept {
	return macroBipolar_.load(std::memory_order_relaxed);
}

void ParamSet::setMacroRelativeMask(uint32_t mask) noexcept {
	macroRelative_.store(mask, std::memory_order_relaxed);
}

uint32_t ParamSet::macroRelativeMask() const noexcept {
	return macroRelative_.load(std::memory_order_relaxed);
}

double ParamSet::macroReading(size_t i) const {
	if (((macroRelative_.load(std::memory_order_relaxed) >> i) & 1u) != 0) {
		return 0.0;
	}
	// THROUGH ITS POLARITY, AS ITS CELL IS (`publishCoreSources`, backlog EG): a bipolar macro reads 2v - 1 on every core
	// road too (bipolar-macro-core). It read the raw 0..1 parameter here, so a bipolar macro
	// played unipolar on every core destination while its ring, and the rack road reading the cell, went both ways.
	const double v = resolved(macroParam(i));
	return ((macroBipolar_.load(std::memory_order_relaxed) >> i) & 1u) != 0 ? 2.0 * v - 1.0 : v;
}

void ParamSet::setWeightSources(const SoundWeights& weights) {
	for (size_t slot = 0; slot < kWeightSourceCount; ++slot) {
		// ABSENT IS ZERO HERE, and only here. A meter has to tell "not in the blend" from "in the blend
		// at nothing" — see sound_weights.h — but a modulation source has nothing to say with the
		// difference: a source that is not there pushes its destination nowhere, which is exactly what
		// `at` already returns for a slot outside the mask.
		std::atomic_ref<float>(coreCells_[kMacroCount + slot])
		    .store(static_cast<float>(weights.at(slot)), std::memory_order_relaxed);
	}
}

double ParamSet::internalOffset(size_t internalIndex, int voice) const {
	if (!anyRoute_.load(std::memory_order_relaxed) || internalIndex >= kInternalDestinationCount) {
		return 0.0;
	}
	// `device::routeSpanOf`, THE DEVICE ROAD'S OWN RULE — `modSpan` where a control declares one, its
	// range otherwise. An internal destination is described by `device::ParamDesc` precisely so that
	// this question has ONE answer in the codebase rather than a second one written here.
	return pullAt(kParamDestinationCount + internalIndex,
	              device::routeSpanOf(internalDestinationDesc(internalIndex)), voice);
}

double ParamSet::sourceOffsetAt(int index, int voice) const {
	if (!anyRoute_.load(std::memory_order_relaxed)) {
		return 0.0;
	}
	const int destination = modDestinationIndexOf(kParams[static_cast<size_t>(index)].id);
	if (destination < 0 || static_cast<size_t>(destination) >= kParamDestinationCount) {
		return 0.0;
	}
	// The destination's own range is the unit an amount is stated in, so one amount means the same
	// distance on a knob that runs 0..1 and on one that runs -100..100 — which is what lets the ring on
	// the face be drawn from the amount without asking what the parameter is.
	const ParamDesc& desc = kParams[static_cast<size_t>(index)];
	return pullAt(static_cast<size_t>(destination), desc.max - desc.min, voice);
}

double ParamSet::pullAt(size_t destination, double span, int voice) const {
	// **THE PULL.** This parameter's inputs, each pointing at the cell its source owns — a device node's
	// published output, or a macro's — and for a voice, the source's per-voice cell where it has one. Each term is
	// `coreRouteTerm`, the one spelling the editor's as-modulated readout calls too (backlog PU).
	const CoreRange& range = coreRanges_[destination];
	const uint32_t count = range.count.load(std::memory_order_acquire);
	const uint32_t first = range.first.load(std::memory_order_relaxed);
	double offset = 0.0;
	for (size_t i = first; i < static_cast<size_t>(first) + count && i < coreInputs_.size(); ++i) {
		const CoreInput& in = coreInputs_[i];
		const float* cell = in.value.load(std::memory_order_relaxed);
		const float* perVoice = in.perVoice.load(std::memory_order_relaxed);
		const int macro = in.macro.load(std::memory_order_relaxed);
		const float amount = in.amount.load(std::memory_order_relaxed);
		const auto transfer = static_cast<device::Transfer>(in.transfer.load(std::memory_order_relaxed));
		if (macro >= 0) {
			// THE HOST NODE'S OWN OUTPUT, READ LIVE — a macro's reading is `resolved()`, which is base plus
			// whatever the host is modulating IT by; so a host modulating a macro reaches everything the
			// macro feeds, and a value asked for between two blocks is the macro as it is now.
			offset += coreRouteTerm(amount, transfer, macroReading(static_cast<size_t>(macro)), true, span);
			continue;
		}
		if (cell == nullptr) {
			continue;
		}
		if (voice >= 0 && perVoice != nullptr && static_cast<size_t>(voice) < perVoiceCells_) {
			cell = perVoice + voice;
		}
		offset += coreRouteTerm(amount, transfer, static_cast<double>(relaxedLoadCell(*cell)), false, span);
	}
	return offset;
}

bool ParamSet::probeRoute(backplane::Destination destination, backplane::Source source,
                          backplane::RouteProbe& out) const {
	// `pullAt`'s walk at no voice, with the watched input set aside (backlog PU).
	if (destination.isDevice() || destination.index >= kParamDestinationCount) {
		return false;
	}
	const size_t d = destination.index;
	const int index = paramIndexOf(static_cast<clap_id>(kModDestinations[d]));
	if (index < 0) {
		return false;
	}
	const ParamDesc& desc = kParams[static_cast<size_t>(index)];
	const double span = desc.max - desc.min;
	const uint64_t key = (uint64_t { source.device } << 32) | source.index;
	backplane::RouteProbe p;
	p.span = span;
	p.baseOffset = mod_[static_cast<size_t>(index)];
	bool found = false;
	bool readable = false;
	if (!source.isDevice() && source.index < kMacroCount) {
		p.macro = true;
		p.source = macroReading(source.index);
		readable = true;
	}
	const CoreRange& range = coreRanges_[d];
	const uint32_t count = range.count.load(std::memory_order_acquire);
	const uint32_t first = range.first.load(std::memory_order_relaxed);
	for (size_t i = first; i < static_cast<size_t>(first) + count && i < coreInputs_.size(); ++i) {
		const CoreInput& in = coreInputs_[i];
		const float* cell = in.value.load(std::memory_order_relaxed);
		const int macro = in.macro.load(std::memory_order_relaxed);
		const float amount = in.amount.load(std::memory_order_relaxed);
		const auto transfer = static_cast<device::Transfer>(in.transfer.load(std::memory_order_relaxed));
		if (in.source.load(std::memory_order_relaxed) == key) {
			found = true;
			if (macro < 0) {
				if (cell == nullptr) {
					return false;
				}
				p.source = static_cast<double>(relaxedLoadCell(*cell));
				readable = true;
			}
			continue;
		}
		double term = 0.0;
		if (macro >= 0) {
			term = coreRouteTerm(amount, transfer, macroReading(static_cast<size_t>(macro)), true, span);
		}
		else if (cell != nullptr) {
			term = coreRouteTerm(amount, transfer, static_cast<double>(relaxedLoadCell(*cell)), false, span);
		}
		(found ? p.after : p.before) += term;
	}
	if (!readable) {
		return false; // a device source no input holds yet: its cell is the drain's to find
	}
	out = p;
	return true;
}

double ParamSet::base(clap_id id) const {
	const int index = paramIndexOf(id);
	return index < 0 ? 0.0 : base_[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void ParamSet::setBase(clap_id id, double value) {
	const int index = paramIndexOf(id);
	if (index < 0) {
		return;
	}
	// One door for every value that comes from outside — host param events and state files both
	// arrive here — so nothing has to remember to validate separately.
	double sanitized = 0.0;
	if (!sanitizeValue(kParams[static_cast<size_t>(index)], value, &sanitized)) {
		return;
	}
	base_[static_cast<size_t>(index)].store(sanitized, std::memory_order_relaxed);
}

void ParamSet::setModOffset(clap_id id, double offset) {
	const int index = paramIndexOf(id);
	if (index < 0) {
		return;
	}
	// Finite only, and deliberately NOT the full sanitizeValue the base gets. An offset is a distance,
	// not a setting: clamping it to [min, max] would reject a legitimate -0.5 on a param whose minimum
	// is 0 (resolved() clamps the sum, which is where clamping belongs), and rounding it for a stepped
	// param would quantise the wrong quantity.
	//
	// A NaN, though, has to stop here. It never reaches a file, but resolved() is the per-block
	// reading the voice layer runs on, and a NaN in it propagates through attackSeconds, masterGain
	// and the rest to silence a voice with nothing to show for it.
	if (!std::isfinite(offset)) {
		return;
	}
	// A param the host was never told it could modulate does not get modulated by a stray event. The
	// stepped params are the whole of that set today, and a mod offset on one of them would be
	// meaningless anyway — resolved() would round a voice count mid-block.
	if ((kParams[static_cast<size_t>(index)].flags & CLAP_PARAM_IS_MODULATABLE) == 0) {
		return;
	}
	mod_[static_cast<size_t>(index)] = offset;
}

void ParamSet::clearModulation() {
	mod_.fill(0.0);
}

double ParamSet::resolved(clap_id id) const {
	// THE COLLAPSED READING IS THE PER-VOICE ONE WITH NO VOICE, so there is one implementation.
	return resolved(id, -1);
}

double ParamSet::resolved(clap_id id, int voice) const {
	const int index = paramIndexOf(id);
	if (index < 0) {
		return 0.0;
	}
	const ParamDesc& desc = kParams[static_cast<size_t>(index)];
	const double sum = base_[static_cast<size_t>(index)].load(std::memory_order_relaxed)
	                   + mod_[static_cast<size_t>(index)] + sourceOffsetAt(index, voice);
	// THE RANGE IS THE LIMIT UNLESS THE PARAM SAYS IT IS ONLY A KNOB. For everything but the filter's
	// cutoff the clamp is the whole point of resolving here — a base is sanitised on the way in but an
	// offset is a distance and is not, so this is where a host's modulation stops being able to ask
	// for a resonance of 3. The cutoff declares otherwise because its range is ten octaves of knob
	// travel and the corner it names is bounded by Nyquist instead: clamping the sum here is what left
	// a keytracked low note two octaves shy of open with no control able to move it. Both offsets are
	// finite by construction — setModOffset refuses a NaN, and a route's reach is an amount times a
	// source times a declared span — so the raw sum is a number, and the device it reaches guards it.
	return landCore(desc, sum); // `landCore`: the one landing the as-modulated readout calls too (backlog PU)
}

OscType ParamSet::oscType() const {
	return oscTypeFromValue(resolved(ParamId::OscType));
}

// ParamSet::filterSettings lived here. It assembled the device's settings from this table so the
// engine and the card could not disagree about what a knob meant; both now read the device's row,
// which is the same guarantee made by there being one copy rather than by two agreeing.

VoiceMode ParamSet::voiceMode() const {
	return static_cast<VoiceMode>(steppedIndex(resolved(ParamId::VoiceMode), kVoiceModeNames.size()));
}

NotePriority ParamSet::notePriority() const {
	return static_cast<NotePriority>(
	    steppedIndex(resolved(ParamId::NotePriority), kNotePriorityNames.size()));
}

int ParamSet::voiceCount() const {
	// Clamped to the pool as well as rounded. The declared maximum is the pool size, so this can only
	// bite if the two ever disagree — which is exactly when a silent overrun would be worst.
	return static_cast<int>(std::clamp(std::lround(resolved(ParamId::VoiceCount)), 1L,
	                                   static_cast<long>(kMaxVoices)));
}

bool ParamSet::drumKit() const {
	return oscType() == OscType::Drum
	       && steppedIndex(resolved(ParamId::DrumMode), kDrumModeNames.size()) == static_cast<size_t>(DrumMode::Kit);
}

int ParamSet::stackCount() const {
	// A DRUM KIT STACKS NOTHING (drum proposal §4.5): a pad's pitch is its Tune, so unison hits would only phase. Stack is not
	// drawn in Kit mode, and a Stack left up before the switch is not obeyed.
	if (drumKit()) {
		return 1;
	}
	// voiceCount()'s rule for the same reason: the declared maximum is the part array's size.
	return static_cast<int>(std::clamp(std::lround(resolved(ParamId::StackCount)), 1L,
	                                   static_cast<long>(kMaxStack)));
}

StackShape ParamSet::stackShape() const {
	return static_cast<StackShape>(steppedIndex(resolved(ParamId::StackShape), kStackShapeNames.size()));
}

StackMap ParamSet::stackMap() const {
	return static_cast<StackMap>(steppedIndex(resolved(ParamId::StackMap), kStackMapNames.size()));
}

double ParamSet::stackIndexAmount(ParamId id) const {
	if (!anyRoute_.load(std::memory_order_relaxed)) {
		return 0.0;
	}
	const int destination = modDestinationIndexOf(id);
	if (destination < 0 || static_cast<size_t>(destination) >= kParamDestinationCount) {
		return 0.0;
	}
	const float* const cell = &coreCells_[kStackIndexSource];
	const CoreRange& range = coreRanges_[static_cast<size_t>(destination)];
	const uint32_t count = range.count.load(std::memory_order_acquire);
	const uint32_t first = range.first.load(std::memory_order_relaxed);
	double amount = 0.0;
	for (size_t i = first; i < static_cast<size_t>(first) + count && i < coreInputs_.size(); ++i) {
		const CoreInput& in = coreInputs_[i];
		if (in.value.load(std::memory_order_relaxed) == cell) {
			amount += static_cast<double>(in.amount.load(std::memory_order_relaxed));
		}
	}
	return amount;
}

bool ParamSet::valueToText(clap_id id, double value, char* out, uint32_t capacity) const {
	return feathers::valueToText(id, value, out, capacity);
}

bool valueToText(clap_id id, double value, char* out, uint32_t capacity) {
	const ParamDesc* desc = findParam(id);
	if (desc == nullptr || out == nullptr || capacity == 0) {
		return false;
	}

	switch (desc->unit) {
	case ParamUnit::Number:
		std::snprintf(out, capacity, "%.2f", value);
		return true;
	case ParamUnit::Integer:
		std::snprintf(out, capacity, "%ld", std::lround(value));
		return true;
	case ParamUnit::Zone:
		std::snprintf(out, capacity, "%.1f", value);
		return true;
	case ParamUnit::Percent:
		std::snprintf(out, capacity, "%.1f %%", value * 100.0);
		return true;
	case ParamUnit::Cents:
		std::snprintf(out, capacity, "%+.1f cents", value);
		return true;
	case ParamUnit::Semitones:
		// Whole steps for Transpose Semi; a continuous control (Coarse) shows where between them it sits.
		if ((desc->flags & CLAP_PARAM_IS_STEPPED) != 0) {
			std::snprintf(out, capacity, "%+ld st", std::lround(value));
		}
		else {
			std::snprintf(out, capacity, "%+.2f st", value);
		}
		return true;
	case ParamUnit::Octaves:
		std::snprintf(out, capacity, "%+ld oct", std::lround(value));
		return true;
	case ParamUnit::Decibels:
		// "-inf" ONLY FOR A FADER'S SILENT FLOOR (Master Gain's -60): a range like A-PH's Ceiling (-24..0) or Sub (0..24) has a
		// real bottom, which prints as its number.
		if (value <= desc->min && desc->min <= kMasterGainMinDb) {
			std::snprintf(out, capacity, "-inf dB");
		}
		else {
			std::snprintf(out, capacity, "%+.1f dB", value);
		}
		return true;
	case ParamUnit::Gain:
		if (value <= 0.0) {
			std::snprintf(out, capacity, "-inf dB");
		}
		else {
			std::snprintf(out, capacity, "%+.1f dB", 20.0 * std::log10(value));
		}
		return true;
	case ParamUnit::LogHertz: {
		// The value IS log2(hertz) — the taper is the value, per this enum's own doc — so the display
		// is where it becomes a frequency again. Three bands rather than one format, because "0.02
		// kHz" and "12000.0 Hz" are both the wrong way to say a number a musician reads at a glance.
		const double hz = std::exp2(value);
		if (hz >= 1000.0) {
			std::snprintf(out, capacity, "%.2f kHz", hz / 1000.0);
		}
		else if (hz >= 100.0) {
			std::snprintf(out, capacity, "%.0f Hz", hz);
		}
		else {
			std::snprintf(out, capacity, "%.1f Hz", hz);
		}
		return true;
	}
	case ParamUnit::Hertz:
		// SIGNED AND LINEAR (the spectral Shift): a shift down is as much a shift as one up, and says so.
		std::snprintf(out, capacity, "%+.1f Hz", value);
		return true;
	case ParamUnit::Vowel: {
		// THE NEAREST VOWEL'S NAME, then where between the anchors it stands (the plot's handle says the same).
		const double position = std::clamp(value, 0.0, static_cast<double>(kVoxAnchorCount - 1));
		std::snprintf(out, capacity, "%s %.2f", kVoxAnchorNames[static_cast<size_t>(std::lround(position))], position);
		return true;
	}
	case ParamUnit::LogSeconds:
		formatSeconds(attackReleaseSeconds(value), out, capacity);
		return true;
	case ParamUnit::Factor:
		std::snprintf(out, capacity, "x%.2f", value);
		return true;
	case ParamUnit::LogFactor:
		std::snprintf(out, capacity, "x%.2f", std::exp2(value));
		return true;
	case ParamUnit::GlideSeconds:
		if (value <= 0.0) {
			std::snprintf(out, capacity, "Off");
		}
		else {
			formatSeconds(glideSeconds(value), out, capacity);
		}
		return true;
	case ParamUnit::BendChoice:
		std::snprintf(out, capacity, "%d st", bendRangeSemitonesAt(value));
		return true;
	case ParamUnit::Choice:
		std::snprintf(out, capacity, "%s", desc->names[steppedIndex(value, desc->names.size())]);
		return true;
	}
	return false;
}

bool ParamSet::textToValue(clap_id id, const char* text, double* out) const {
	return feathers::textToValue(id, text, out);
}

bool textToValue(clap_id id, const char* text, double* out) {
	const ParamDesc* desc = findParam(id);
	if (desc == nullptr || text == nullptr || out == nullptr) {
		return false;
	}

	// Enum entries answer to their own name first, so a host that round-trips the display string gets
	// back the entry it showed. A bare index still works for hosts that type numbers.
	if (desc->unit == ParamUnit::Choice) {
		for (size_t i = 0; i < desc->names.size(); ++i) {
			if (equalsIgnoreCase(text, desc->names[i])) {
				*out = static_cast<double>(i);
				return true;
			}
		}
	}

	// Same rule for the non-numeric displays a continuous param has. "Off" is the whole of what a
	// glide time of zero prints, and "-inf dB" is what the bottom of a fader prints — both have to
	// parse back to the setting that produced them, and neither survives a numeric parse now that
	// non-finite results are refused.
	if (desc->unit == ParamUnit::GlideSeconds && equalsIgnoreCase(text, "Off")) {
		*out = 0.0;
		return true;
	}
	if ((desc->unit == ParamUnit::Decibels || desc->unit == ParamUnit::Gain) && mentionsNegativeInfinity(text)) {
		*out = desc->min; // Gain's minimum is 0.0, Decibels' is the silent bottom of its range
		return true;
	}

	// A VOWEL PRINTS ITS NEAREST ANCHOR'S NAME FIRST ("Open 3.00"): the name is read past, and a name alone is that anchor.
	if (desc->unit == ParamUnit::Vowel) {
		for (size_t i = 0; i < kVoxAnchorCount; ++i) {
			if (equalsIgnoreCasePrefix(text, kVoxAnchorNames[i])) {
				text += std::strlen(kVoxAnchorNames[i]);
				while (*text == ' ') {
					++text;
				}
				if (*text == '\0') {
					*out = static_cast<double>(i);
					return true;
				}
				break;
			}
		}
	}
	double parsed = 0.0;
	// A multiplier prints as "x1.00", so a leading x (either case) is the unit, not a number to refuse.
	if ((desc->unit == ParamUnit::Factor || desc->unit == ParamUnit::LogFactor) && (*text == 'x' || *text == 'X')) {
		++text;
	}
	if (!parseLeadingDouble(text, &parsed)) {
		return false;
	}

	double value = parsed;
	switch (desc->unit) {
	case ParamUnit::Percent:
		value = parsed / 100.0;
		break;
	case ParamUnit::Gain:
		value = std::pow(10.0, parsed / 20.0);
		break;
	case ParamUnit::LogHertz: {
		const double hz = mentionsKilohertz(text) ? parsed * 1000.0 : parsed;
		if (hz <= 0.0) {
			return false; // log2 of nothing is not a cutoff, and sanitizeValue refuses non-finite anyway
		}
		value = std::log2(hz);
		break;
	}
	case ParamUnit::Hertz:
		value = mentionsKilohertz(text) ? parsed * 1000.0 : parsed;
		break;
	case ParamUnit::Vowel:
		value = parsed;
		break;
	case ParamUnit::LogSeconds: {
		const double seconds = mentionsWholeSeconds(text) ? parsed : parsed * 0.001;
		if (seconds <= 0.0) {
			return false;
		}
		value = std::log2(seconds);
		break;
	}
	case ParamUnit::GlideSeconds: {
		const double seconds = mentionsWholeSeconds(text) ? parsed : parsed * 0.001;
		value = std::cbrt(std::max(seconds, 0.0) / kGlideMaxSeconds);
		break;
	}
	case ParamUnit::LogFactor:
		if (parsed <= 0.0) {
			return false; // log2 of nothing is not a multiplier
		}
		value = std::log2(parsed);
		break;
	case ParamUnit::BendChoice: {
		// The display is a semitone count, so that is what comes back — mapped to the nearest choice
		// rather than used as an index, which would read "12" as 48 semitones.
		size_t best = 0;
		for (size_t i = 1; i < kBendRangeSemitones.size(); ++i) {
			if (std::abs(kBendRangeSemitones[i] - parsed) < std::abs(kBendRangeSemitones[best] - parsed)) {
				best = i;
			}
		}
		value = static_cast<double>(best);
		break;
	}
	case ParamUnit::Number:
	case ParamUnit::Integer:
	case ParamUnit::Zone:
	case ParamUnit::Cents:
	case ParamUnit::Semitones:
	case ParamUnit::Octaves:
	case ParamUnit::Decibels:
	case ParamUnit::Choice:
	case ParamUnit::Factor: // the value IS the multiplier
		break;
	}

	// Same shaping the event and state doors get: a stepped param typed as "3.7" is not a setting the
	// host was offered, and returning it would hand back a value CLAP says cannot exist.
	return sanitizeValue(*desc, value, out);
}

namespace {

// State is a version byte, a param count, then id→value pairs. Everything little-endian and doubles
// as their exact bit pattern, so a patch written on one machine loads bit-identically on another
// rather than picking up a text-conversion rounding difference.
// VERSION 2 SINCE 2026-08-22: the routings block moved to the end of the sequence (it resolves
// device instance ids against the chains, so it must read after them) and its payload became a
// sparse list rather than a dense matrix. Neither change is readable by a version-1 reader and
// neither is detectable from the bytes, so the byte moves and an older patch refuses HERE — at the
// first byte of the stream, before any block is walked — instead of misreading one block as another.
//
// **VERSION 3 SINCE 2026-09-10, AND THE REASON IS THE RULE THIS BYTE HAD BEEN MISSING — backlog DR.**
//
// On
// 2026-08-26 `kDivisionBeats`/`kDivisionNames` grew from NINE entries to TWENTY-SEVEN **in place**, by
// inserting a dotted and a triplet after each base. `mseg::Param::TimeBase` stores an INDEX into that
// table. Nothing remapped the stored indices, so every shape saved before that day came back on a
// different division — 6 meant "1 bar" and now means "1/16" — and most landed on a dotted or a
// triplet, which is why it read as random rather than as a shift.
//
// **THE BYTE COULD NOT SEE IT AND THAT IS THE FINDING.** It had gone to 2 on 2026-08-22, four days
// EARLIER, and did not move again. Every version-2 reader accepted every version-2 file, because the
// stream's BLOCK ORDER was unchanged — and a device row is a tag, a width and a run of floats, with no
// layout version of its own. So the file was structurally valid and semantically wrong, which is the
// exact failure this byte exists to convert into a clean refusal.
//
// **THE RULE, WRITTEN DOWN BECAUSE IT WAS THE MISSING HALF OF THE POLICY.** CLAUDE.md's *"Patch
// compatibility is NOT a constraint yet"* says to break the format and take the clean design — a decision
// and the right one. Breaking it SILENTLY is a different act from breaking it CLEANLY:
//
//     **BUMP THIS BYTE WHENEVER A STORED NUMBER CHANGES MEANING**, not only when the stream's shape
//     does. A param inserted, removed or reordered inside a device's row; a `Choice` table that grows,
//     shrinks or reorders; any index whose table is written to a file. The stream can be
//     byte-compatible and the patch still wrong.
//
//     **SUPERSEDED FOR DEVICES BY BACKLOG JF (2026-09-25):** a device's values are stored by key and a
//     Choice by its label, so the device half of that rule no longer moves this byte —
//     `tests/state_schema.ledger` guards it (see `state_block_order.h`). It still holds for this file's
//     own stream: the core parameters and blocks, until they are keyed too (JF step 2).
//
// **AND IT WAS ENFORCED RATHER THAN REMEMBERED** by `kStoredMeaningFingerprint`, a digest of every
// device's ordered control names and every Choice table, pinned in `tests/test_device_ranges.cpp` —
// deleted with JF step 1d in favour of the schema ledger, which also sees a rescale. A comment alone
// would have been the third thing in this file to say something true and be forgotten.
//
// WHAT VERSION 3 COSTS: every version-2 patch refuses. Correct ones too — the information that would
// separate a pre-2026-08-26 file from a post- one was never written down, so they cannot be told
// apart and a remap is impossible rather than merely expensive. Refusing all of them is the honest
// outcome, and the standing policy that no saved projects or presets need protecting yet is what
// makes it affordable.
//
// VERSION 4 (2026-09-11): the fx lane, the strip and the routings — three blocks, one device record of
// their own — became ONE `chains` block in the rack's format (chain-as-a-device item 6). A shape change,
// so a version-3 patch refuses at the first byte rather than reading `fxchain`'s bytes as `chains`'.
// THE NUMBER ITSELF IS IN `state_block_order.h`, beside the block table and the fingerprint — one
// place for the three things that together decide whether a patch may be read. `using` rather than a
// second definition, so there is nothing here to drift.
using feathers::kStateVersion;


bool writeAll(const clap_ostream* stream, const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	size_t written = 0;
	while (written < size) {
		const int64_t n = stream->write(stream, bytes + written, size - written);
		if (n <= 0) {
			return false;
		}
		written += static_cast<size_t>(n);
	}
	return true;
}

bool readAll(const clap_istream* stream, void* data, size_t size) {
	auto* bytes = static_cast<uint8_t*>(data);
	size_t read = 0;
	while (read < size) {
		const int64_t n = stream->read(stream, bytes + read, size - read);
		if (n <= 0) { // 0 is end of file, and a truncated record is not a state we can trust.
			return false;
		}
		read += static_cast<size_t>(n);
	}
	return true;
}

bool writeU32(const clap_ostream* stream, uint32_t value) {
	const uint8_t bytes[4] = {
	    static_cast<uint8_t>(value & 0xffU),
	    static_cast<uint8_t>((value >> 8) & 0xffU),
	    static_cast<uint8_t>((value >> 16) & 0xffU),
	    static_cast<uint8_t>((value >> 24) & 0xffU),
	};
	return writeAll(stream, bytes, sizeof(bytes));
}

bool readU32(const clap_istream* stream, uint32_t* value) {
	uint8_t bytes[4];
	if (!readAll(stream, bytes, sizeof(bytes))) {
		return false;
	}
	*value = static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8)
	         | (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
	return true;
}

bool writeDouble(const clap_ostream* stream, double value) {
	uint64_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	uint8_t bytes[8];
	for (int i = 0; i < 8; ++i) {
		bytes[i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xffU);
	}
	return writeAll(stream, bytes, sizeof(bytes));
}

bool readDouble(const clap_istream* stream, double* value) {
	uint8_t bytes[8];
	if (!readAll(stream, bytes, sizeof(bytes))) {
		return false;
	}
	uint64_t bits = 0;
	for (int i = 0; i < 8; ++i) {
		bits |= static_cast<uint64_t>(bytes[i]) << (i * 8);
	}
	std::memcpy(value, &bits, sizeof(*value));
	return true;
}

} // namespace

bool ParamSet::save(const clap_ostream* stream) const {
	if (stream == nullptr) {
		return false;
	}
	if (!writeAll(stream, &kStateVersion, sizeof(kStateVersion))) {
		return false;
	}
	if (!writeU32(stream, static_cast<uint32_t>(kParamCount))) {
		return false;
	}
	for (size_t i = 0; i < kParamCount; ++i) {
		if (!writeU32(stream, static_cast<uint32_t>(kParams[i].id))) {
			return false;
		}
		if (!writeDouble(stream, base_[i].load(std::memory_order_relaxed))) {
			return false;
		}
	}
	return true;
}

bool ParamSet::load(const clap_istream* stream) {
	Staged staged;
	if (!read(stream, &staged)) {
		return false; // nothing has been committed, so the live patch is untouched
	}
	commit(staged);
	return true;
}

bool ParamSet::read(const clap_istream* stream, Staged* staged, ReadFailure* why, uint8_t* versionFound) const {
	const auto refuse = [why](ReadFailure failure) {
		if (why != nullptr) {
			*why = failure;
		}
		return false;
	};
	if (stream == nullptr || staged == nullptr) {
		return refuse(ReadFailure::NoStream);
	}
	uint8_t version = 0;
	if (!readAll(stream, &version, sizeof(version))) {
		return refuse(ReadFailure::Empty);
	}
	if (versionFound != nullptr) {
		*versionFound = version;
	}
	// A newer major version can have reordered the record layout itself, which no amount of id
	// skipping recovers from — refusing is honest, where guessing would load a wrong patch silently. 49, 50
	// and 51 share this record exactly (50 added the trailer, 51 a block after the last), so all are read here;
	// the caller holds a 49 stream to its own rule (`FeathersPlugin::loadWalk`).
	if (!feathers::readableStateVersion(version)) {
		return refuse(ReadFailure::Version);
	}

	uint32_t count = 0;
	if (!readU32(stream, &count)) {
		return refuse(ReadFailure::ShortRead);
	}

	// Read into a local first and commit only once the stream has ended cleanly. A load that fails
	// half way is the case that matters: a truncated or corrupt project file must cost the user
	// nothing, and resetting before reading would have handed them a patch of defaults in exchange
	// for the one they had.
	for (size_t i = 0; i < kParamCount; ++i) {
		staged->base[i] = kParams[i].def; // ids the stream omits keep their default
	}

	for (uint32_t i = 0; i < count; ++i) {
		uint32_t id = 0;
		double value = 0.0;
		if (!readU32(stream, &id) || !readDouble(stream, &value)) {
			return refuse(ReadFailure::ShortRead);
		}
		const int index = paramIndexOf(static_cast<clap_id>(id));
		if (index < 0) {
			continue; // a param from some later version
		}
		double sanitized = 0.0;
		if (sanitizeValue(kParams[static_cast<size_t>(index)], value, &sanitized)) {
			staged->base[static_cast<size_t>(index)] = sanitized;
		}
		// An unusable value (a NaN written by a buggy build, say) leaves that one param at its
		// default rather than failing the whole load — the rest of the patch is still good.
	}
	if (why != nullptr) {
		*why = ReadFailure::None;
	}
	return true;
}

void ParamSet::commit(const Staged& staged) {
	for (size_t i = 0; i < kParamCount; ++i) {
		base_[i].store(staged.base[i], std::memory_order_relaxed);
	}
	// The modulation lane is deliberately left alone, and not only because clearing it from the main
	// thread would race the audio thread that owns it. base_ is the plugin's state and mod_ is the
	// host's: the host re-sends modulation continuously from its own modulators, so wiping it on a
	// preset load would have been claiming ownership of something we do not own — a momentary drop
	// to the unmodulated value until the next block re-established what was already true.
}

} // namespace feathers
