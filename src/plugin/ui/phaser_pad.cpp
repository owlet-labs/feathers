/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#include "plugin/ui/phaser_pad.h"
#include "plugin/ui/explore_pad_impl.h"

#include "fx/phaser/phaser_response.h"

#include <algorithm>
#include <cmath>

namespace feathers::ui {

SliceBasis<PhaserPadPolicy::kPhaseCount> PhaserPadPolicy::basisAt(const RowReader&, double rotate,
                                                                  const Phases& origin) {
	namespace ps = fx::phaser::phase_space;
	// THE SHAPER'S ARRANGEMENT (`ShaperPadPolicy::basisAt`): the plane is anchored at the home plus the standing
	// travel, reduced to one scalar by `planeHomeOf`. Only the home differs — a constant here.
	ps::Phases home = ps::homeFor();
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		home[i] += origin[i];
	}
	return ps::basisFor(rotate, ps::planeHomeOf(home));
}

fx::phaser::Settings PhaserPadPolicy::terrainSettingsOf(const RowReader& row) {
	namespace ph = fx::phaser;
	ph::Row r {};
	for (size_t i = 0; i < r.size(); ++i) {
		r[i] = row(i);
	}
	ph::Settings s = ph::stateOf(r, r.size()).settings;
	// THE MOTION'S PHASE IS NOT AN INPUT, ITS REACH IS (round 2's terrain fix): `paint` averages the distance over a few
	// instants spread across the sweep's reach — Position ± Depth, and Position + Env Amount·[0, 1] with the follower on
	// — so LFO On, Depth, Env On and Env Amount stay in the key (they set where those instants fall), while the LFO's
	// RUNNING (its phase, the readings), its speed (Rate, Sync, Stereo's phase offset) and the follower's level and shape
	// (Attack, Release, Sens: how the input moves it, not how far) never rebuild it.
	const ph::Settings defaults {};
	s.sync = defaults.sync;
	s.rateLog2Hz = defaults.rateLog2Hz;
	s.stereoDegrees = defaults.stereoDegrees;
	s.envAttackLog2s = defaults.envAttackLog2s;
	s.envReleaseLog2s = defaults.envReleaseLog2s;
	s.envSensDb = defaults.envSensDb;
	// NO NOTE: `ResponseInputs{}` carries none, so Key Track changes nothing in the model.
	s.keyTrack = false;
	// THE MIX IS FIXED AT 50% (`kTerrainMix`, backlog LT): the terrain maps the effect's own notches, not how much of it
	// is heard, so the knob neither reshapes it nor rebuilds it.
	s.mix = kTerrainMix;
	// ROTATE TURNS THE PLANE (`Key::rotate`), not the sound; the travel is the plane's, cell by cell.
	s.rotate = 0.0f;
	s.travel = {};
	return s;
}

void PhaserPadPolicy::paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane) {
	namespace ph = fx::phaser;
	// THE SUMMARY'S FREQUENCIES, log-spaced, stopping under the flanger's trusted band (`kFlangerTrustedFraction`).
	const double top = key.settings.mode == ph::Mode::Flanger
	                       ? std::min(kSummaryHighHz, ph::kFlangerTrustedFraction * kTerrainSampleRate)
	                       : kSummaryHighHz;
	std::array<double, kSummaryPoints> hz {};
	for (size_t i = 0; i < kSummaryPoints; ++i) {
		const double t = static_cast<double>(i) / static_cast<double>(kSummaryPoints - 1);
		hz[i] = kSummaryLowHz * std::pow(top / kSummaryLowHz, t);
	}
	// THE TEXTBOOK RESPONSE — zero travel, the device as its literal controls say — each point floored as the summary
	// floors it (`kSummaryFloorDb`), so one exact null cannot dominate.
	const auto dbAt = [&](const ph::ResponseModel& m, size_t i) {
		return std::max(static_cast<double>(ph::magnitudeDb(m, hz[i])), ph::kSummaryFloorDb);
	};
	// **BOTH CHANNELS** (the Task 9b review): three of the explore dimensions act on the RIGHT only — the stereo skew
	// on R's corner or delay, and the flanger's L/R tap detune (indices 1 and 13) — so a left-only terrain stayed dark
	// along exactly those directions while the sound moved. Each cell is the RMS over L's points and R's together.
	// It doubled the build (MEASURED then, 32² cells, one instant: 17.1 ms Phaser / 11.6 ms Flanger against 9.2 / 6.5 for L
	// alone). Paid only on a rebuild, never per repaint; the round-2 build's cost is `kGrid`'s note.
	//
	// **AND OVER SEVERAL INSTANTS OF THE SWEEP, NOT ONE** (round 2, after a capture showed a BLANK terrain): the first painter
	// took the sound at ONE frozen instant, s = Position. In the Flanger under through-zero at Position 0 that instant is
	// the crossing — every tap's relative delay is 0 whatever the travel does to its span — so the whole plane was one
	// null and the map drew nothing, while the sweep a hand actually hears passes through it and away. So each cell is
	// the RMS over `kSweepSamples` instants spread across the sweep's reach as well: the LFO at the phases where its sine
	// is −1, −½, 0, ½, 1 (s = Position + Depth·{−1, −½, 0, ½, 1} for an unbent shape, clamped by the model's own
	// `sweepOf`), and under the barberpole ψ at five points over a third of a turn (the three voices tile the rest). With
	// the LFO off, or at Depth 0, every instant is Position, and one is taken. No single instant can blank it now, and
	// the explore groups that act only through the LFO (Stagger, Shape) are visible in it.
	//
	// **THE FOLLOWER MOVES THE SWEEP TOO** (the review): with Env on, the instants also walk its reading over 0..1, so the
	// sweep visits Position + Env Amount·{0, ¼, ½, ¾, 1} — with the LFO off that alone keeps the Flanger's through-zero
	// crossing at Position 0 from blanking the map. Both sources on: instant k takes the LFO's k-th phase and the
	// follower's k-th level together.
	const bool lfoMoves = key.settings.lfoOn && key.settings.lfoDepth != 0.0f;
	const bool envMoves = key.settings.envOn && key.settings.envAmount != 0.0f;
	const bool moving = lfoMoves || envMoves;
	const size_t instants = moving ? kSweepSamples : 1;
	constexpr double kSineAt[kSweepSamples] = { 0.75, 11.0 / 12.0, 0.0, 1.0 / 12.0, 0.25 }; // sin(2πφ) = −1 … 1
	std::array<ph::ResponseInputs, 2 * kSweepSamples> inputs {};
	for (size_t k = 0; k < instants; ++k) {
		for (size_t c = 0; c < 2; ++c) {
			ph::ResponseInputs& in = inputs[k * 2 + c];
			in.channel = static_cast<int>(c);
			in.lfoPhase = lfoMoves ? kSineAt[k] : 0.0;
			in.barberPhase = lfoMoves ? static_cast<double>(k) / (3.0 * static_cast<double>(kSweepSamples)) : 0.0;
			in.env = envMoves ? static_cast<double>(k) / static_cast<double>(kSweepSamples - 1) : 0.0;
		}
	}
	const size_t perCell = instants * 2 * kSummaryPoints;
	ph::Settings s = key.settings;
	s.travel = {};
	// THE REFERENCE IS THE TEXTBOOK DEVICE — NO EXPLORE LAYER AT ALL — not zero travel through the pills: a pill away
	// from 0.5 moves the sound at home too (0 and 1 force its fields wherever the puck is), so its group lights the
	// whole plane, the puck's cell included, which is what the ear hears. At every pill 0.5 the two are the same thing
	// (zero travel derives +0.0 everywhere), so a default device draws exactly what it drew before the house law.
	std::array<double, 2 * kSweepSamples * kSummaryPoints> home {};
	for (size_t k = 0; k < instants * 2; ++k) {
		const ph::ResponseModel m = ph::responseModelOf(s, ph::phase_space::Explore {}, inputs[k], kTerrainSampleRate);
		for (size_t i = 0; i < kSummaryPoints; ++i) {
			home[k * kSummaryPoints + i] = dbAt(m, i);
		}
	}
	const ph::phase_space::Pills pills = ph::pillsOf(s);
	// THE COARSE GRID, ON THE STACK (the review's ask: no allocation in a build). Each cell: HOW FAR THE RESPONSE A
	// PRESS THERE WOULD GIVE IS FROM THE TEXTBOOK ONE — the RMS of the dB difference over the band, the instants and both
	// channels.
	// Dark at home with the pills at 0.5 (exactly 0 there, by the explore layer's own invariant) and wherever the travel
	// lands back on it; bright where the character layer has moved the notches, the loop's colour or the stereo skew
	// furthest.
	//
	// **NOT `contrastDb`, WHICH THE TASK NAMED, BECAUSE IT IS FLAT.** MEASURED (scratch probe, default settings, a
	// 13x13 grid of ±1.5 plane units, the default view's reach): the Phaser's contrast runs 6.14..6.29 dB over the whole
	// plane at 48 points and 6.17..6.29 at 256, the Flanger's 5.42..6.86 — the explore layer moves WHERE the notches are
	// far more than how deep the response swings, so a contrast terrain is one even grey (the first build of this
	// painter measured a 0.009 brightness spread over the map). `deepestDb` (−20.1..−18.4) and `tiltDbPerOct`
	// (−0.8..+0.5) are no better. The distance from home over the same grid: 0.0..5.8 dB (Phaser), 0.0..10.3 (Flanger).
	// One hue — the pad's accent, the shared painter's: a hue axis would need a colour hook in `ExplorePad::draw`, and
	// the shaper has none.
	std::array<float, kGrid * kGrid> coarse {};
	for (size_t gy = 0; gy < kGrid; ++gy) {
		for (size_t gx = 0; gx < kGrid; ++gx) {
			const float ux = (static_cast<float>(gx) + 0.5f) / static_cast<float>(kGrid);
			const float uy = (static_cast<float>(gy) + 0.5f) / static_cast<float>(kGrid);
			s.travel = plane.travelAt(ux, uy);
			// ONE DERIVE A CELL (the pills included — the terrain shows what they allow), then every instant and channel.
			const ph::phase_space::Explore e = ph::phase_space::exploreOf(s.travel, pills);
			double acc = 0.0;
			for (size_t k = 0; k < instants * 2; ++k) {
				const ph::ResponseModel m = ph::responseModelOf(s, e, inputs[k], kTerrainSampleRate);
				for (size_t i = 0; i < kSummaryPoints; ++i) {
					const double d = dbAt(m, i) - home[k * kSummaryPoints + i];
					acc += d * d;
				}
			}
			coarse[gy * kGrid + gx] = static_cast<float>(std::sqrt(acc / static_cast<double>(perCell)));
		}
	}
	// FROM 0 dB — DARK MEANS TEXTBOOK (by design) — to the farthest cell in view (`kMinSpanDb` at least), then
	// `kTerrainGamma` (see it: the averaged distance plateaus, and a linear map draws the plateau as one bright field).
	const float hi = *std::max_element(coarse.begin(), coarse.end());
	const float span = std::max(std::isfinite(hi) ? hi : 0.0f, static_cast<float>(kMinSpanDb));
	for (float& c : coarse) {
		const float x = std::isfinite(c) ? std::clamp(c / span, 0.0f, 1.0f) : 0.0f;
		c = std::pow(x, static_cast<float>(kTerrainGamma));
	}
	// BILINEAR UP TO THE MAP — both grids sampled at their cell centres over the same unit square.
	map.build([&](float ux, float uy) {
		const float fx = std::clamp(ux * static_cast<float>(kGrid) - 0.5f, 0.0f, static_cast<float>(kGrid - 1));
		const float fy = std::clamp(uy * static_cast<float>(kGrid) - 0.5f, 0.0f, static_cast<float>(kGrid - 1));
		const auto x0 = static_cast<size_t>(fx);
		const auto y0 = static_cast<size_t>(fy);
		const size_t x1 = std::min(x0 + 1, kGrid - 1);
		const size_t y1 = std::min(y0 + 1, kGrid - 1);
		const float tx = fx - static_cast<float>(x0);
		const float ty = fy - static_cast<float>(y0);
		const float a = coarse[y0 * kGrid + x0] + (coarse[y0 * kGrid + x1] - coarse[y0 * kGrid + x0]) * tx;
		const float b = coarse[y1 * kGrid + x0] + (coarse[y1 * kGrid + x1] - coarse[y1 * kGrid + x0]) * tx;
		return a + (b - a) * ty;
	});
}

PhaserPadPolicy::Key PhaserPadPolicy::keyOf(const RowReader& row, double rotate, double zoom, const Phases& standing) {
	return Key { rotate, zoom, standing, terrainSettingsOf(row) };
}

template class ExplorePad<PhaserPadPolicy>;

} // namespace feathers::ui
