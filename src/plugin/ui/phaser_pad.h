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

#pragma once

#include "plugin/ui/explore_pad.h"

#include "fx/phaser/phaser_phase_space.h"
#include "fx/phaser/phaser_types.h"

#include <array>
#include <chrono>
#include <cstddef>

// THE PHASER/FLANGER'S EXPLORE PAD — the shaper's pad over the phaser's thirty-four phases (phaser plan, Task 9a).
//
// SEATED IN `PhaserExpanded` (Task 9b) beside a GammaBox for Rotate, wired as the shaper's is: the writer sets the
// device's field `kTravelFirst + i`, the reader is `fieldAt`. Its terrain is how far the response model is from the
// textbook one at each point of the plane (`paint`).

namespace feathers::ui {

struct PhaserPadPolicy {
	static constexpr size_t kPhaseCount = fx::phaser::phase_space::kPhaseCount;
	static constexpr size_t kTravelFirst = fx::phaser::kTravelFirst;
	using Phases = fx::phaser::phase_space::Phases;
	using RowReader = std::function<float(size_t field)>;
	/// **WHAT THE MAP IS BUILT FROM** — and, since the Task 9a review, the ONLY thing `paint` reads: the frame's own
	/// inputs, and the response model's settings for the terrain (`terrainSettingsOf`). Those are the stored row with the
	/// MOTION's running taken out — rate, sync, stereo and the follower's shape at defaults, no note, no travel (the
	/// plane supplies it per cell) — but its REACH kept (LFO On, Depth, Env On, Env Amount: round 2 averages over the
	/// sweep's instants, `paint`), so a running LFO or a Rate move never rebuilds the terrain, while Mode, Stages, Center,
	/// Delay, Feedback, Damping, Drive, Mix, Position, Sweep, Through-zero, the reach and every pill each do. A defaulted
	/// `==`: a field added to `Settings` joins the key unasked (and rebuilds too often, the safe direction, until
	/// `terrainSettingsOf` neutralises it).
	struct Key {
		double rotate = -1e9;
		double zoom = -1e9;
		Phases standing {};
		fx::phaser::Settings settings {};
		bool operator==(const Key&) const = default;
	};

	/// THE HOME IS A CONSTANT (`phase_space::homeFor()`: no control moves it), plus the standing travel.
	[[nodiscard]] static SliceBasis<kPhaseCount> basisAt(const RowReader& row, double rotate, const Phases& origin);
	static void wrapForStorage(const Phases& accumulator, std::array<float, kPhaseCount>& stored) {
		fx::phaser::phase_space::wrapForStorage(accumulator, stored);
	}
	/// **THE TERRAIN: HOW FAR FROM THE TEXTBOOK THE RESPONSE IS AT EACH POINT OF THE PLANE.** A coarse `kGrid`² grid of
	/// the RMS dB distance between the response model there and at zero travel, over `kSummaryPoints` log-spaced points
	/// (40 Hz up to 16 kHz or the flanger's trusted band) and `kSweepSamples` instants across the sweep's reach (round 2:
	/// one frozen instant could blank the whole plane), each cell's travel from `plane.travelAt`; drawn against the
	/// build's own range (`kMinSpanDb`); bilinearly interpolated up to the map. Reads only `key`. Why not `contrastDb`:
	/// the .cpp has the measurement (it is flat over the plane).
	static void paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane);
	[[nodiscard]] static Key keyOf(const RowReader& row, double rotate, double zoom, const Phases& standing);
	/// The response model's settings for the terrain — see `Key`.
	[[nodiscard]] static fx::phaser::Settings terrainSettingsOf(const RowReader& row);

	/// THE COARSE GRID'S SIDE, and the frequencies each cell is summarised over. 17 by 40, not the first painter's 32 by
	/// 48: each cell is now five instants of two channels, and the build has a 30 ms budget (the widget case reports it).
	/// MEASURED 2026-10-04, the widget case on this host: 23–25 ms Phaser (6 stages), 10–18 ms Flanger, load ~2.5 — the
	/// first painter measured 33–35 / 25–28 ms the same night at load ~9 (a shared host: the two are not a strict A/B).
	/// The response's own inner loop got cheaper with it (`responseAt` takes e^(jω) once, not once a stage).
	/// ODD ON PURPOSE: a cell centre lands exactly on the frame's origin — the sound as it stands — so the cell under the
	/// puck is the true distance there (0 at home) rather than an average of four cells a twelfth of a view away, where
	/// the averaged distance has already climbed most of its plateau. The map is bilinear over it either way.
	static constexpr size_t kGrid = 17;
	static constexpr size_t kSummaryPoints = 40;
	/// THE INSTANTS OF THE SWEEP EACH CELL IS AVERAGED OVER (`paint`) — one with the LFO off or at Depth 0.
	static constexpr size_t kSweepSamples = 5;
	static constexpr double kSummaryLowHz = 40.0;
	static constexpr double kSummaryHighHz = 16000.0;
	/// THE TERRAIN IS SUMMARISED AT A FIXED RATE — the pad has no host to ask. The summary stops at 16 kHz, inside
	/// every rate's Nyquist; only the flanger's trusted band (0.3·sr: 14.4 kHz here) and the bilinear prewarp move
	/// with it, which shifts the picture a little at 44.1 or 96 kHz and changes none of its shape.
	static constexpr double kTerrainSampleRate = 48000.0;
	/// **THE PICTURE'S SCALE: 0 dB IS DARK, ALWAYS — DARK MEANS "TEXTBOOK"** (decided in the Task 11 review). The
	/// full scale is the farthest cell in view (`kMinSpanDb` at least, so a plane whose whole reach is under 2 dB draws
	/// dim rather than amplifying a fraction of a dB into full contrast): brightness = (d / span)^`kTerrainGamma`. A
	/// floor at the build's own minimum (the first round-2 draft) would have drawn a far-from-home view's nearest cell
	/// dark, which says "textbook" about a place that is not.
	static constexpr double kMinSpanDb = 2.0;
	/// **THE CURVE ON THAT 0-BASED SCALE** — a DISPLAY choice from a measurement: averaged over the sweep's instants with
	/// all thirty-four phases live, the distance PLATEAUS (the default view: 2.1 dB beside home, 6.3 at the 10th
	/// percentile, 8.1 at the median, 9.2 at the most), so a linear map puts nine tenths of the plane in its top third.
	/// A power above one spreads that top without moving 0: LOOKED AT on the private display at 1, 3 and 5 (2026-10-04) —
	/// at 1 the default view is one pale field, at 3 its valleys and ridges read, at 5 the mid-distances sink too dark.
	static constexpr double kTerrainGamma = 3.0;
	/// **THE TERRAIN IS DRAWN AT A FIXED MIX OF 50%, WHATEVER THE KNOB SAYS** (backlog LT: choosing 50%
	/// over 100% once shown what 100% draws). Mix only scales how much of the effect is heard; the notches and peaks the
	/// terrain maps are the effect's own, and at a low Mix they flattened out of sight (deepest point −4 dB at 20%
	/// against −19 to −39 dB at 50%, default settings). NOT WET-ONLY: the phaser's wet is an allpass cascade and the
	/// flanger's (Normal) one delay tap, both flat in magnitude, so at Feedback 0 every cell is exactly 0 dB from home
	/// and the map would be black; their notches exist only as dry + wet, deepest at equal parts. The engine and the
	/// response plot (`PhaserPlot`, the real output) keep the knob's Mix.
	static constexpr float kTerrainMix = 0.5f;
	/// **A MOVING KEY REBUILDS AT MOST THIS OFTEN** (`ExplorePad`'s throttle, the final review). A build is ~25 ms
	/// (Phaser, 6 stages; ~10–18 ms Flanger — `kGrid`'s measurement), and the key holds Center, Delay, Feedback,
	/// Position, Damping, Drive, Stages, Sweep, LFO On and Depth, Env On and Amount and every pill, so a knob drag or
	/// automation on any of them would rebuild every frame; at 100 ms a build is at most a quarter of the UI thread while
	/// the key keeps moving, the terrain trails the hand by at most a tenth of a second, and it lands on the final key
	/// once the knob stops.
	static constexpr std::chrono::milliseconds kKeyRebuildInterval { 100 };
	/// **THE VISIBLE ZOOM** (`ExplorePad::zoomControlShown`): "− 100% +" in the pad's top-left corner, on Rotate's row. The
	/// phaser's alone — the shaper's pad names no such member and is unchanged, pixel for pixel.
	static constexpr bool kZoomControl = true;
};

extern template class ExplorePad<PhaserPadPolicy>;

class PhaserPad final : public ExplorePad<PhaserPadPolicy> {
public:
	using ExplorePad::ExplorePad;
};

} // namespace feathers::ui
