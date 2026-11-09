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

#include "fx/phaser/phaser_types.h"

#include <array>
#include <complex>
#include <cstddef>
#include <span>

// **WHAT THE PHASER/FLANGER'S PICTURE IS — ONE CHANNEL'S FREQUENCY RESPONSE AT ONE INSTANT, A PURE FUNCTION OF THE
// SETTINGS AND OF THE FEW FACTS ONLY THE RUNNING ENGINE HAS.**
//
// The disperser's arrangement (`disperser_response.h`): the model is built from the SAME pure functions the engine's
// tick runs — `channelSweepOf` for every voice's sweep, per-stage sweeps and flanger read, `stageCornerHz` and
// `tptGainOf` for each stage's G, `effectiveFeedback`, `colouredDampingCornerHz`, `exploreOf` — so the plot cannot
// drift from the sound. What the settings cannot say (where the LFO is, what the follower reads, where the barberpole's
// ramp stands, which note is tracked) comes in as `ResponseInputs`, taken from the published readings.
//
// WHAT IT SHOWS:
//   - THE PHASER: (1 − Mix) + Mix·wet, wet = M / (1 − L·z⁻¹·M) — M the product of the stages' allpasses
//     (`stageResponseOfG`, each at the engine's own float G), L the loop: the effective feedback (the Drive-dependent
//     ceiling included), the damping lowpass D (the coloured corner, or 1 when open), and with an explore bias the
//     saturator's small-signal slope 1 − tanh²(b) and the 10 Hz DC blocker. Under the barberpole the three voices'
//     wets summed by their windows, each with its own loop.
//   - THE FLANGER: wet = T / (1 − L·T), T = Σ w_k·z^(−d_k) (Normal: one tap, w = 1), d fractional, z^(−d) = e^(−jωd)
//     — the taps summed BEFORE the loop, as the engine's one line does. Under through-zero the wet is INVERTED and the
//     dry delayed by the fixed dry delay: (1 − Mix)·z^(−dry) − Mix·wet.
//   - THE SMOOTHERS LANDED: every glide at its target (the engine's glides land on them exactly, `kGlideSnap`).
// WHAT IT DOES NOT:
//   - DRIVE'S NONLINEARITY. The loop is linearised at zero signal: the symmetric saturator's slope there is 1, so
//     Drive enters only through the feedback ceiling it opens. A loud loop driven hard is quieter and duller than drawn.
//   - THE FLANGER'S KERNEL. The engine reads its line through the B = 1 sinc kernel (`kFlangerBandwidth`): a delta at
//     a whole-sample delay, but at a fractional one a windowed sinc whose response departs from the ideal toward
//     Nyquist. The model's e^(−jωd) is the ideal fractional delay. TRUSTED BAND: up to `kFlangerTrustedFraction` of
//     the sample rate — see it for the measurement; above it the drawn comb is the ideal one, not the engine's.
//   - Motion inside a tick: the model is the tick's corners and reads, as the readings are.

namespace feathers::fx::phaser {

/// **WHERE THE MODEL OF THE FLANGER IS TRUSTED**, as a fraction of the sample rate — 14.4 kHz at 48 kHz. MEASURED
/// (2026-10-03, engine against model, steady-state sine gain, 200 frequencies, wherever the model is above −15 dB;
/// Delay 1 and 5 ms, fractional reads of .4, .5 and .8 of a sample, Feedback 0 and ±0.7, through-zero off and on):
/// worst 0.08 dB up to 0.30·sr; up to 0.73 dB by 0.35·sr; up to 13 dB by 0.45·sr, where the B = 1 kernel's
/// fractional reads stop being the ideal delay and the feedback multiplies the difference. A whole-sample read is
/// exact everywhere (the kernel is a delta there). `test_fx_phaser`'s flanger agreement case tests inside this band.
inline constexpr double kFlangerTrustedFraction = 0.30;

/// **WHAT ONLY THE ENGINE KNOWS AT AN INSTANT** — each one a published reading (`Reading`).
struct ResponseInputs {
	double lfoPhase = 0.0;    ///< `Reading::LfoPhase`, the LEFT channel's φ (the right's is derived, `channelLfoPhase`)
	double env = 0.0;         ///< `Reading::Env`, the follower's 0..1 reading the sweep used
	double barberPhase = 0.0; ///< `Reading::BarberPhase`, the LEFT channel's ψ (unread with Sweep = Normal)
	double noteHz = 0.0;      ///< `Reading::NoteHz`: the tracked note, or 0 for none
	int channel = 0;          ///< 0 left, 1 right
};

/// THE READINGS INTO INPUTS for `channel` — `count` of them (a short array leaves the rest at their defaults).
[[nodiscard]] ResponseInputs responseInputsOf(const float* readings, size_t count, int channel) noexcept;

struct ResponseModel {
	Mode mode = Mode::Phaser;
	bool enabled = true;
	bool barberpole = false;
	bool throughZero = false;
	double sampleRate = 48000.0;
	int stages = 6;
	int voices = 1; ///< 1 in Normal, `kBarberVoices` under the barberpole
	/// EACH VOICE'S SWEEP AS THE TICK COMPUTES IT (`channelSweepOf`), and what the readings would report of them.
	ChannelSweep sweep {};
	/// EACH VOICE'S STAGES' TPT GAIN G = g/(1 + g) — the very float `aimStages` aims the engine's ramps at.
	std::array<std::array<float, kMaxStages>, kBarberVoices> G {};
	/// EACH VOICE'S WINDOW as the engine's float ramp holds it (Normal: voice 0 is 1).
	std::array<double, kBarberVoices> w {};
	/// THE FLANGER: each tap's wet read in samples, and the through-zero dry delay (0 without it).
	std::array<double, kBarberVoices> delaySamples {};
	double drySamples = 0.0;
	/// THE LOOP: the effective feedback (as the engine's float ramp holds it), times the saturator's small-signal slope —
	/// ONE A LOOP, because the explore layer's feedback tilt (index 31) gives each its own (`feedbackTiltOf`): the
	/// phaser's barberpole voices step it, and otherwise the channel's share is in every entry (the flanger's one loop
	/// is entry 0). `responseAt` reads `loopGains`; `feedback` and `loopGain` are voice 0's, for a reader that wants one.
	std::array<double, kBarberVoices> feedbacks {};
	std::array<double, kBarberVoices> loopGains {};
	double feedback = 0.0;
	double loopGain = 0.0;
	double mix = 0.5;
	bool dampOpen = true;
	double dampHz = 0.0;
	float dampG = 1.0f;
	bool dcBlock = false; ///< the explore bias put the 10 Hz DC blocker in the loop
	float dcG = 0.0f;
};

/// **THE MODEL FOR ONE CHANNEL**, built through the engine's own pure functions. The explore layer is
/// `exploreOf(s.travel, pillsOf(s))` — the pills included (their `fieldWithOverride` in front of each field).
///
/// **THE RATE SKEW (index 30) IS NOT IN IT, by construction**: the model is one instant, and a rate is not visible at an
/// instant. The right channel's φ is taken as `channelLfoPhase(in.lfoPhase, Stereo)` — the left's plus Stereo/360 —
/// which is the engine's R exactly at zero skew and drifts from it as a skewed R runs at its own rate (the readings
/// publish only L's phase). Everything else round 2 added — the feedback tilt, the notch pairing, the sweep curve — is.
[[nodiscard]] ResponseModel responseModelOf(const Settings& s, const ResponseInputs& in, double sampleRate) noexcept;
/// THE SAME, WITH THE EXPLORE LAYER ALREADY DERIVED — for a caller building many models of one travel (the pad's
/// terrain: one derive a cell, many instants and both channels). `e` must be what `responseModelOf` would derive.
[[nodiscard]] ResponseModel responseModelOf(const Settings& s, const phase_space::Explore& e, const ResponseInputs& in,
                                            double sampleRate) noexcept;

/// THE OUTPUT OVER THE INPUT at `hz`, complex.
[[nodiscard]] std::complex<double> responseAt(const ResponseModel& m, double hz) noexcept;
/// Its magnitude in dB, floored at −120.
[[nodiscard]] float magnitudeDb(const ResponseModel& m, double hz) noexcept;

/// **THREE NUMBERS FOR A WHOLE RESPONSE** — what the explore pad's terrain (Task 9) paints, sampled over `hz`. Every
/// point's dB is first FLOORED AT `kSummaryFloorDb` (−60), so one exact null cannot dominate the statistics:
///   - `deepestDb`: the lowest (floored) dB — how deep the deepest notch the grid sees is;
///   - `tiltDbPerOct`: the least-squares slope of dB against log2(hz) — the spectrum's tilt, dB per octave;
///   - `contrastDb`: the standard deviation (over the points, population) of the dB — how far the response swings.
/// The points weigh equally, so a log-spaced grid gives every octave equal say. Empty `hz`: all zero.
inline constexpr double kSummaryFloorDb = -60.0;
struct ResponseSummary {
	double deepestDb = 0.0;
	double tiltDbPerOct = 0.0;
	double contrastDb = 0.0;
};
[[nodiscard]] ResponseSummary responseSummaryOf(const ResponseModel& m, std::span<const double> hz) noexcept;

} // namespace feathers::fx::phaser
