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

#include "fx/shaper/shaper_types.h"
#include "fx/shaper/shaper_phase_space.h"

#include <array>
#include <cstddef>

// THE COLOUR PAD — the second zone control, expanded into the plane it was always half of.
//
// WHAT IT IS FOR. `shaper_types.h` records the measurement that decides this: Knee is a SCALE (its
// eight zones are eight ranges of one monotone quantity) and Colour is a FIELD (its zones are a
// navigation grid over something that keeps moving inside them). A scale gains nothing from a second
// dimension. A field has as many as you give it — and Gamma is the one the firmware had and could not
// show, because there it was a push-and-twist modifier on a hardware encoder, which is a GESTURE and
// not a coordinate.
//
// MEASURED, and this is the number that justifies the whole widget: at fixed colour, the six-weight
// vector travels 13.9 across gamma 0..1 — more than six of the eight colour zones do across their own
// width. It was a full dimension of the sound, reachable only by holding a button.
//
// Y IS COLOUR AND X IS GAMMA, and that assignment is not arbitrary. Colour keeps its zone structure,
// its names and its knob, so it must be the axis the zone bands are drawn along — bands run
// horizontally, which means the quantity they divide runs vertically. Gamma has no zones and no
// names, so it gets the axis with nothing to say.
//
// THE BOTTOM EDGE USED TO BE THE CARD: `deriveCurve` gated an inflator band on `gamma == 0`, the state a card with no Gamma
// control is always in. Since backlog IW the inflator is Colour's Inflate MODE, applied wherever the pad is, so no line of
// the plane is special any more — the terrain reads the mode (`View::inflate`) and paints the curve it builds.
//
// THE MAP BEHIND THE PUCK IS CACHED, AND REBUILT ONLY WHEN WHAT IT WAS BUILT FROM MOVES. This said it was
// "computed once at construction", which stopped being true when the terrain became the PLANE: it is a
// function of the standing travel, the basis (Rotate), the zoom, the pad's size, and the terrain inputs
// `Key` names (colour, gamma, knee, the extras, Colour's mode). It is rebuilt when one of those moves, at
// rest, and when a pan leaves the margin it was built with — never per repaint.
//
// **THE COST ROSE 53x WHEN THE TERRAIN STARTED READING THE CURVE, and the old figure is left here so
// the trade is visible.** Summing `deriveCurve`'s six weights was 1536 samples at 0.13 us each, 200 us
// total. Walking the TRANSFER at 48 points per sample — which is what makes the picture say what the
// curve DOES rather than what is present in it — MEASURED **6.89 us each, 10.6 ms total**
// (`BI PROBE 16`). Once, on a panel that opens, so it is affordable; recomputing it per repaint never
// was and now is emphatically not.
//
// **IT IS ALSO THE OBVIOUS PLACE FOR THE SHARED RENDERING WORK** — rendering optimisations taken from
// Feathers and extracted for reuse. Feathers' own terrain
// already has the machinery this wants — mip levels, baking, and a SIMD triangle evaluator
// (`evalTriangle4Neon`) — and 10.6 ms of scalar curve-walking is exactly the workload it was built
// for. Extracting it would serve this pad, automod's, and any device that grows one.

//
// **THE WIDGET ITSELF IS NOW `ExplorePad<ShaperPadPolicy>`** (phaser plan, Task 9a): the puck, the pan, the
// zoom and the cached map are shared with the phaser's pad, and what is the shaper's — the eighteen phases,
// where its travel sits in the row, the home Colour and Gamma move, and the curve's terrain — is the policy
// below. `ShaperPad` stays a class of its own name, so the construction site, the tests and the tooltip
// sweep's allow-list (which matches the demangled name) all read exactly as before.

namespace feathers::ui {

struct ShaperPadPolicy {
	static constexpr size_t kPhaseCount = fx::shaper::phase_space::kPhaseCount;
	static constexpr size_t kTravelFirst = fx::shaper::kTravelFirst;
	using Phases = fx::shaper::phase_space::Phases;
	using RowReader = std::function<float(size_t field)>;

	/// **WHAT THE MAP WAS BUILT FOR.** The terrain is the PLANE, so it is a function of where the field
	/// is, which way the basis points and how far in the zoom is — not a constant of the device. Kept so
	/// a repaint can tell a stale picture from a current one without rebuilding to find out.
	struct Key {
		double colour = -1e9;
		double gamma = -1e9;
		double rotate = -1e9;
		double zoom = -1e9;
		/// **KNEE AND HARMONICS ARE TERRAIN INPUTS AND WERE MISSING FROM THIS.** `paint` passes
		/// both to `deriveCurve`, so both can change the picture. Left out of the staleness check they
		/// changed the terrain's INPUT without ever asking for a rebuild, so the pad went on showing
		/// the field for a knee the device had left.
		///
		/// **THEY REACH IT BY DIFFERENT ROADS, MEASURED RATHER THAN ASSUMED.** Knee reaches `drive`
		/// and `preExpandAmount`, which `transferAt` reads, so it moves the variation term directly.
		/// Harmonics does NOT touch the transfer at all — MEASURED, 0.10 against 0.95 gives a transfer
		/// maxdiff of exactly 0.000000 — it moves `slewIntensity` alone (0.244 -> 0.527 at the same
		/// point), which reaches the picture only through the extras term and only while Lowpass or
		/// Integrator is on. A first draft of this comment claimed harmonics moved the six weights;
		/// the measurement says it moves none of them.
		///
		/// **THE CLASS OF BUG, NOT THE INSTANCE: a cache key must name every input its value reads.**
		/// This one was written when the terrain read colour and gamma alone, and grew two inputs
		/// without growing its key — which is why the key now sits directly beside the painter that
		/// reads them (`paint`, below) rather than anywhere else.
		double knee = -1e9;
		double harmonics = -1e9;
		/// THE FIVE EXTRAS' AMOUNTS — Sub, Feedback, Mangle, Lowpass, Integrator, in that order, each its amount
		/// while its switch is on and 0 while off (`extrasOf`). Not a bitset: since the extras became field scalers
		/// (backlog II) the picture moves with the amount, not only with the switch. They change the picture through
		/// `extrasActivityOf`, so by the same rule as knee and harmonics above they belong in the key.
		std::array<float, 5> extras {}; ///< see `extrasKeyOf`
		Phases standing {};
		/// **COLOUR'S MODE, AS THE TERRAIN SEES IT (backlog IW)** — -1 in Free, the Inflate amount in Inflate. A terrain input
		/// (`applyInflate` moves the six weights), so by this struct's own rule it belongs in the key.
		double inflate = -1e9;
		bool operator==(const Key&) const = default;
	};

	/// THE HOME FROM THE ROW — `homeFor(colour, gamma)` plus the standing travel — and the plane taken there.
	[[nodiscard]] static SliceBasis<kPhaseCount> basisAt(const RowReader& row, double rotate, const Phases& origin);
	static void wrapForStorage(const Phases& accumulator, std::array<float, kPhaseCount>& stored) {
		fx::shaper::phase_space::wrapForStorage(accumulator, stored);
	}
	/// THE TERRAIN: the transfer's total variation at each cell, plus the turned-up extras' activity.
	/// Reads ONLY `key` (`ExplorePad`'s contract: the key is taken first, and the map is a picture of it).
	static void paint(ExploreMap& map, const Key& key, const ExplorePlane<kPhaseCount>& plane);
	[[nodiscard]] static Key keyOf(const RowReader& row, double rotate, double zoom, const Phases& standing);
};

extern template class ExplorePad<ShaperPadPolicy>;

class ShaperPad final : public ExplorePad<ShaperPadPolicy> {
public:
	/// **A JOYSTICK, NOT A MAP — and the reason is that this device has no second axis to map.**
	/// The pad it replaces wrote x to Gamma and y to Colour. MEASURED (`shaper_phase_space.h`), those
	/// two point the same way: the angle between them is 0 degrees at colour 0 and 5.5 degrees at
	/// colour 1. It was a LINE drawn as a square. A second dimension cannot be had by picking different
	/// parameters, because the parameters do not contain one — it has to come from travel through a
	/// basis, which is the only thing that moves the eight phases independently.
	using ExplorePad::ExplorePad;

	/// WHAT THE TERRAIN SAYS AT A POINT — the transfer's TOTAL VARIATION, normalised. See the
	/// definition: it reads the curve rather than counting its ingredients.
	[[nodiscard]] static float activityAt(float colour, float gamma);
};

} // namespace feathers::ui
