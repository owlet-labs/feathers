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

#include "fx/phaser/phaser_response.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx::phaser {

namespace {

using C = std::complex<double>;

/// A TPT one-pole's prewarped g back from the G the engine holds: G/(1 − G).
double gOf(float G) noexcept {
	const double g = static_cast<double>(G);
	return g / (1.0 - g);
}

/// THE TPT ONE-POLE LOWPASS from its allpass (`stageResponseOfG`): ap = 2·lp − x, so lp = (ap + 1)/2.
C lowpassOf(float G, double fOverSr) noexcept { return 0.5 * (stageResponseOfG(gOf(G), fOverSr) + 1.0); }

} // namespace

ResponseInputs responseInputsOf(const float* readings, size_t count, int channel) noexcept {
	ResponseInputs in;
	in.channel = channel == 1 ? 1 : 0;
	if (readings == nullptr) {
		return in;
	}
	const auto at = [&](Reading r, double fallback) {
		const auto i = static_cast<size_t>(r);
		return i < count && std::isfinite(readings[i]) ? static_cast<double>(readings[i]) : fallback;
	};
	in.lfoPhase = at(Reading::LfoPhase, 0.0);
	in.env = at(Reading::Env, 0.0);
	in.barberPhase = at(Reading::BarberPhase, 0.0);
	in.noteHz = at(Reading::NoteHz, 0.0);
	return in;
}

ResponseModel responseModelOf(const Settings& s, const ResponseInputs& in, double sampleRate) noexcept {
	return responseModelOf(s, phase_space::exploreOf(s.travel, pillsOf(s)), in, sampleRate);
}

ResponseModel responseModelOf(const Settings& s, const phase_space::Explore& e, const ResponseInputs& in,
                              double sampleRate) noexcept {
	ResponseModel m;
	const double sr = sampleRate > 0.0 ? sampleRate : 48000.0;
	const int ch = in.channel == 1 ? 1 : 0;
	m.sampleRate = sr;
	m.mode = s.mode;
	m.enabled = s.enabled;
	// `Phaser::tick`, the smoothers landed: every glide at the target `targetsOf` gives the engine (the tracked note is
	// the reading's), and the explore layer at `exploreOf(travel)` — the engine's glides land on both exactly.
	const Targets t = targetsOf(s, in.noteHz, sr);
	m.throughZero = t.throughZero; // the flanger's alone (`targetsOf`)
	m.barberpole = isBarberpole(s.sweep);
	m.stages = t.stages;
	// THE PHASER'S CHANNEL CENTRE: the target, moved by the channel's skew (`channelCentreLog2`).
	const double centre = channelCentreLog2(t.centerLog2, ch, e);

	// THE SWEEP, through the tick's own statement.
	SweepInputs sw;
	sw.flanger = s.mode == Mode::Flanger;
	sw.barberpole = m.barberpole;
	sw.throughZero = m.throughZero;
	sw.position = t.position;
	sw.lfoAmount = t.lfoAmount;
	sw.envTerm = t.envAmount * in.env;
	sw.lfoPhase = channelLfoPhase(in.lfoPhase, t.stereoDegrees, ch);
	sw.psi = m.barberpole ? channelBarberPhase(in.barberPhase, t.stereoDegrees, ch) : 0.0;
	sw.centreLog2 = centre;
	sw.delayLog2 = t.delayLog2;
	sw.squareNorm = squareNormOf(e.bend);
	sw.sampleRate = sr;
	sw.taps = tapOffsetsOf(m.barberpole, ch, e);
	channelSweepOf(sw, e, m.sweep);
	m.voices = m.sweep.count;

	// EACH VOICE'S STAGES, as `aimStages` aims them: the stage's own sweep and spread into its corner, into G.
	for (size_t v = 0; v < static_cast<size_t>(m.voices); ++v) {
		const VoiceSweep& vs = m.sweep.voices[v];
		for (size_t i = 0; i < static_cast<size_t>(kMaxStages); ++i) {
			m.G[v][i] = tptGainOf(stageCornerHz(centre, vs.stageS[i], stageSpreadOf(i, e), sr), sr);
		}
		m.w[v] = m.barberpole ? static_cast<double>(static_cast<float>(vs.w)) : 1.0;
		m.delaySamples[v] = vs.delay;
	}
	m.drySamples = m.throughZero ? static_cast<double>(tzDrySamples(sr)) : 0.0;

	// THE LOOP, as the ramps land: Drive first (the ceiling reads it), the feedback under it, the mix.
	const float drive = loopDriveOf(t.drive);
	// THE SATURATOR LINEARISED AT ZERO: slope 1 symmetric, 1 − tanh²(b) with the explore bias (`saturateWith`).
	const float driveBias = static_cast<float>(e.driveBias);
	const double tb = static_cast<double>(biasTanhOf(drive, driveBias));
	// EACH LOOP'S FEEDBACK, the tilt (index 31) added before the ceiling — the engine's tick, loop for loop.
	const bool perVoice = s.mode == Mode::Phaser && m.barberpole;
	for (size_t v = 0; v < static_cast<size_t>(kBarberVoices); ++v) {
		const double tilt = feedbackTiltOf(perVoice, static_cast<int>(v), ch, e.feedbackTilt);
		m.feedbacks[v] = static_cast<double>(loopFeedbackOf(t.feedback + tilt, drive));
		m.loopGains[v] = m.feedbacks[v] * (1.0 - tb * tb);
	}
	m.feedback = m.feedbacks[0];
	m.loopGain = m.loopGains[0];
	m.dcBlock = tb != 0.0; // the engine's key: the EFFECTIVE bias (none at Drive 0)
	m.dcG = loopDcGainOf(sr);
	m.mix = static_cast<double>(loopMixOf(t.mix));
	m.dampOpen = dampingOpen(t.dampingLog2);
	if (!m.dampOpen) {
		m.dampHz = colouredDampingCornerHz(t.dampingLog2, e.dampingOctaves, sr);
		m.dampG = tptGainOf(m.dampHz, sr);
	}
	return m;
}

std::complex<double> responseAt(const ResponseModel& m, double hz) noexcept {
	const double f = hz / m.sampleRate;
	const double omega = 2.0 * std::numbers::pi * f;
	if (!m.enabled) {
		// OFF IS THE DRY — late by the latency under through-zero.
		return std::polar(1.0, -omega * m.drySamples);
	}
	// THE LOOP'S LINEAR PART: the damping lowpass and the DC blocker's highpass (1 − its lowpass), times each loop's own
	// feedback × slope below (`loopGains`: the tilt makes it per loop).
	C filters { 1.0, 0.0 };
	if (!m.dampOpen) {
		filters *= lowpassOf(m.dampG, f);
	}
	if (m.dcBlock) {
		filters *= 1.0 - lowpassOf(m.dcG, f);
	}
	const double dry = 1.0 - m.mix;
	if (m.mode == Mode::Phaser) {
		// e^(jω) ONCE, and every stage's allpass from it (`stageResponseOfG`'s expression with z in hand) — a cascade with
		// travel has up to twelve distinct stages, and the terrain asks this tens of thousands of times a build.
		const C z = std::polar(1.0, omega);
		const C zi = std::conj(z);
		C wet {};
		for (size_t v = 0; v < static_cast<size_t>(m.voices); ++v) {
			// THE CASCADE: each stage's allpass at its own G — a stage at the G before it reuses that response (at home,
			// every stage of a voice).
			C cascade { 1.0, 0.0 };
			C stage { 1.0, 0.0 };
			float last = -1.0f;
			for (size_t i = 0; i < static_cast<size_t>(m.stages); ++i) {
				const float G = m.G[v][i];
				if (G != last) {
					const double g = gOf(G);
					stage = ((g - 1.0) * z + (g + 1.0)) / ((g + 1.0) * z + (g - 1.0));
					last = G;
				}
				cascade *= stage;
			}
			const C loop = m.loopGains[v] * filters;
			wet += m.w[v] * cascade / (1.0 - loop * zi * cascade);
		}
		return dry + m.mix * wet;
	}
	// THE FLANGER: the taps summed before the loop, one line, one loop (entry 0's feedback).
	C taps {};
	for (size_t v = 0; v < static_cast<size_t>(m.voices); ++v) {
		taps += m.w[v] * std::polar(1.0, -omega * m.delaySamples[v]);
	}
	const C loop = m.loopGains[0] * filters;
	const C wet = taps / (1.0 - loop * taps);
	if (m.throughZero) {
		// THROUGH-ZERO: the dry held back, the wet inverted.
		return dry * std::polar(1.0, -omega * m.drySamples) - m.mix * wet;
	}
	return dry + m.mix * wet;
}

float magnitudeDb(const ResponseModel& m, double hz) noexcept {
	const double mag = std::abs(responseAt(m, hz));
	return static_cast<float>(20.0 * std::log10(std::max(mag, 1.0e-6)));
}

ResponseSummary responseSummaryOf(const ResponseModel& m, std::span<const double> hz) noexcept {
	ResponseSummary out;
	if (hz.empty()) {
		return out;
	}
	// ONE PASS: each point's dB (floored at `kSummaryFloorDb`) against its octave, accumulated for the minimum, the
	// least-squares slope and the spread.
	const auto n = static_cast<double>(hz.size());
	double deepest = 0.0;
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
	bool first = true;
	for (const double f : hz) {
		const double y = std::max(static_cast<double>(magnitudeDb(m, f)), kSummaryFloorDb);
		const double x = std::log2(std::max(f, 1.0e-3));
		deepest = first ? y : std::min(deepest, y);
		first = false;
		sx += x;
		sy += y;
		sxx += x * x;
		sxy += x * y;
		syy += y * y;
	}
	out.deepestDb = deepest;
	const double varX = sxx / n - (sx / n) * (sx / n);
	out.tiltDbPerOct = varX > 0.0 ? (sxy / n - (sx / n) * (sy / n)) / varX : 0.0;
	const double varY = syy / n - (sy / n) * (sy / n);
	out.contrastDb = std::sqrt(std::max(varY, 0.0));
	return out;
}

} // namespace feathers::fx::phaser
