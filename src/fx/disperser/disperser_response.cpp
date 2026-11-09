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

#include "fx/disperser/disperser_response.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace feathers::fx::disperser {

namespace {

using C = std::complex<double>;

/// ONE STAGE AT `z^-1 = e^{-jω}`: the DF2T allpass `(a2 + a1 z^-1 + z^-2) / (1 + a1 z^-1 + a2 z^-2)`, times
/// its emphasis gain — what `Disperser::runStage` computes, without the saturations it never reaches.
C stage(const AllpassCoeffs& c, double gain, C zi) {
	const double a1 = 2.0 * c.a1Half;
	const double a2 = c.a2;
	const C zi2 = zi * zi;
	return gain * (a2 + a1 * zi + zi2) / (1.0 + a1 * zi + a2 * zi2);
}

} // namespace

ResponseModel responseModelOf(const Settings& s, bool haveNote, double noteSemitones, double sampleRate) noexcept {
	ResponseModel m;
	m.sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
	// `Disperser::tick`, at rest: the coordinates where the controls are, the smoothers landed.
	const double gammaTerm = kGammaScale * static_cast<double>(s.gamma);
	const Coordinate topoC = splitCoordinate(static_cast<double>(s.topo));
	const Coordinate twistC = splitCoordinate(static_cast<double>(s.twist));
	m.twist = computeTwistParams(twistC.knob, twistC.lap + gammaTerm);
	m.topo = computeTopoParams(topoC.knob, topoC.lap + static_cast<double>(s.topoLap) + gammaTerm, m.twist.phaseOffset);
	applyPills(s, m.topo, m.twist);
	m.topo.q = qWithOffset(m.topo.q, static_cast<double>(s.qOffsetOctaves));
	m.routing = routingOf(m.topo, m.twist);
	m.topology = static_cast<Topology>(m.topo.zone);
	const double freqIndex = freqIndexOf(s, haveNote, noteSemitones);
	m.tracking = s.keyTrack && haveNote;
	const float centerHz = centerHzOf(freqIndex);
	m.centerHz = centerHz;
	m.markerHz = centerHz;

	const int stages = std::clamp(s.stages, kMinStages, kMaxStages);
	m.layout = layoutStages(centerHz, static_cast<float>(m.routing.spread * kSpreadOctaves),
	                        restingHalfSpread(m.routing.lrOffset), static_cast<float>(m.topo.q), stages,
	                        static_cast<float>(m.twist.spreadCurve), static_cast<float>(m.twist.qTilt),
	                        static_cast<float>(m.routing.bimodalSeparation), static_cast<float>(m.topo.detuning),
	                        static_cast<float>(m.topo.emphasis));
	for (int i = 0; i < m.layout.count; ++i) {
		const auto k = static_cast<size_t>(i);
		m.coeffsL[k] = allpassCoeffs(m.layout.hzL[k], m.layout.q[k], m.sampleRate);
		m.coeffsR[k] = allpassCoeffs(m.layout.hzR[k], m.layout.q[k], m.sampleRate);
		m.ladderCross[k] = 0.05 + 0.20 * static_cast<double>(i) / static_cast<double>(m.layout.count);
		m.gains[k] = m.layout.gainTarget[k];
	}
	m.crossGain = 0.4 + m.routing.crossMix * 0.5;

	// THE ECHO: the centre stage's delay doubled and folded, as `processDisperser` places it.
	const auto centre = static_cast<size_t>(stages / 2);
	const double raw = static_cast<double>(static_cast<float>(m.sampleRate) / m.layout.hzBase[centre]);
	const double offset = std::floor(foldedDelaySamples(raw, m.sampleRate));
	m.echoMs = foldedDelaySamples(offset * 2.0, m.sampleRate) / m.sampleRate * 1000.0;
	return m;
}

void useStageGains(ResponseModel& m, const float* gains) noexcept {
	if (gains == nullptr) {
		return;
	}
	for (size_t i = 0; i < static_cast<size_t>(kMaxStages); ++i) {
		m.gains[i] = gains[i];
	}
}

std::array<C, 2> routingResponse(const ResponseModel& m, double hz) noexcept {
	const double w = 2.0 * std::numbers::pi * hz / m.sampleRate;
	const C zi = std::polar(1.0, -w);
	const auto n = static_cast<size_t>(std::max(m.layout.count, 1));
	const auto gain = [&](size_t i) { return static_cast<double>(m.gains[i]); };
	const auto run = [&](size_t i, C& l, C& r) {
		l = stage(m.coeffsL[i], gain(i), zi) * l;
		r = stage(m.coeffsR[i], gain(i), zi) * r;
	};
	C l { 1.0, 0.0 };
	C r { 1.0, 0.0 };
	switch (m.topology) {
	case Topology::Ladder:
		for (size_t i = 0; i < n; ++i) {
			const double cross = m.ladderCross[i];
			const C bl = l * (1.0 - cross) + r * cross;
			const C br = r * (1.0 - cross) + l * cross;
			l = bl;
			r = br;
			run(i, l, r);
		}
		break;
	case Topology::Cross:
		for (size_t i = 0; i < n; ++i) {
			run(i, l, r);
			if ((i & 3u) == 3u) {
				const C nl = l * (1.0 - m.crossGain) + r * m.crossGain;
				const C nr = r * (1.0 - m.crossGain) + l * m.crossGain;
				l = nl;
				r = nr;
			}
		}
		break;
	case Topology::Parallel: {
		const size_t half = std::max<size_t>(n / 2, 1);
		C al = l, ar = r, bl = l, br = r;
		for (size_t i = 0; i < half; ++i) {
			run(i, al, ar);
		}
		for (size_t i = half; i < n; ++i) {
			run(i, bl, br);
		}
		l = 0.5 * al + 0.5 * bl;
		r = 0.5 * ar + 0.5 * br;
		break;
	}
	case Topology::Nested: {
		const size_t half = std::max<size_t>(n / 2, 1);
		C il = l, ir = r;
		for (size_t i = 0; i < half; ++i) {
			run(i, il, ir);
		}
		C ol = 0.5 * l + 0.5 * il;
		C orr = 0.5 * r + 0.5 * ir;
		for (size_t i = half; i < n; ++i) {
			run(i, ol, orr);
		}
		l = ol * static_cast<double>(kNested80) + orr * static_cast<double>(kNested20);
		r = orr * static_cast<double>(kNested80) + ol * static_cast<double>(kNested20);
		break;
	}
	case Topology::Spring: {
		size_t tap1 = std::max<size_t>(n / 3, 1);
		size_t tap2 = (n * 2) / 3;
		if (tap2 <= tap1) {
			tap2 = tap1 + 1;
		}
		C t1l {}, t1r {}, t2l {}, t2r {};
		for (size_t i = 0; i < n; ++i) {
			run(i, l, r);
			if (i == tap1) {
				t1l = l;
				t1r = r;
			}
			else if (i == tap2) {
				t2l = l;
				t2r = r;
			}
		}
		const double p70 = kSpring70;
		const double p15 = kSpring15;
		const C bl = l * p70 + t1l * p15 + t2l * p15;
		const C br = r * p70 + t1r * p15 + t2r * p15;
		l = bl * static_cast<double>(kSpring875) + br * static_cast<double>(kSpring125);
		r = br * static_cast<double>(kSpring875) + bl * static_cast<double>(kSpring125);
		break;
	}
	default:
		for (size_t i = 0; i < n; ++i) {
			run(i, l, r);
		}
		break;
	}
	return { l, r };
}

std::array<double, 2> groupDelayMs(const ResponseModel& m, double hz) noexcept {
	// A CENTRAL DIFFERENCE OF THE PHASE, taken as the angle of a RATIO so no unwrapping is needed: the
	// two responses are a hair apart in frequency, so their phase difference is small.
	const double dHz = std::max(hz * 1.0e-4, 1.0e-3);
	const auto up = routingResponse(m, hz + dHz);
	const auto down = routingResponse(m, std::max(hz - dHz, 1.0e-3));
	const double dw = 2.0 * std::numbers::pi * ((hz + dHz) - std::max(hz - dHz, 1.0e-3)) / m.sampleRate;
	std::array<double, 2> out {};
	for (size_t c = 0; c < 2; ++c) {
		const double dphi = std::arg(up[c] / down[c]);
		out[c] = -dphi / dw / m.sampleRate * 1000.0;
	}
	return out;
}

} // namespace feathers::fx::disperser
