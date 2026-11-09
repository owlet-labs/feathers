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

// HOW MANY QUADS A REAL FRAME ASKS FOR, so the transient arena can be sized from measurement.
//
// WHY THIS EXISTS. bgfx's transient vertex buffer is a fixed per-frame pool — 6 MB by default, which
// visage never enlarges — and a batch that cannot be served is dropped in its entirety and silently.
// On this build that pool holds about 14043 quads for the WHOLE frame, shared across every batch.
// Raising it is the fix, and the size wants to come from what a frame actually asks for rather than
// from a round number somebody liked.
//
// THE RECIPE IS THE CHAOS RECIPE, the same one tests/test_gui_widgets.cpp turned into a fixture: Weather
// up, three trail departures, then Path/PosX/PosY driven by incommensurate LFOs the way a host's
// automation drives them, swept across zoom levels. That is the condition under which the surface was
// seen to fail, so it is the condition the arena has to survive.
//
// THE TERRAIN'S OWN CONTRIBUTION IS REPORTED SEPARATELY, and it is now 1. It used to be one quad per
// texel and by far the largest consumer — 12892 of a 13067-quad worst frame — which is what made the
// arena bind at all. Since it became a single textured quad the same recipe measures 230, and the
// separate line is kept because it is the fastest way to see that the swap is still in place: if that
// number ever climbs back into the thousands, something has gone back to drawing texels.
//
// WHAT THIS DOES NOT MEASURE, and it matters more now than it did. This drives the Explore surface
// alone, not the assembled editor. While the terrain dominated, everything else was rounding error;
// at 230 quads it is not. So the number to reason about when sizing is the MARGIN, not the 230.
//
// A REAL SUBMIT, not a logical snapshot. The widget suite draws into a canvas without submitting,
// which never reaches the batcher; the count only exists once `setupQuads` runs. So this needs a
// graphics context, and it uses the windowless one for the same reason the other controls do.
//
//   cmake -B build -DFEATHERS_BUILD_TOOLS=ON && ninja -C build quad-census && build/quad-census

#include "plugin/gui_bridge.h"
#include "plugin/params.h"
#include "plugin/ui/explore_stage.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/ui/terrain.h"
#include "plugin/ui/ui_state.h"

#include "visage_graphics/canvas.h"
#include "visage_graphics/graphics_utils.h"
#include "visage_graphics/shape_batcher.h"
#include "visage_graphics/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979;

// The editor's size, matching the widget suite's fixture so the two are talking about the same
// surface.
constexpr float kWidth = 866.0f;
constexpr float kHeight = 534.0f;

struct Census {
	uint64_t worst = 0;
	uint64_t worstTerrain = 0;
	uint64_t total = 0;
	int frames = 0;

	void add(uint64_t quads, uint64_t terrain) {
		if (quads > worst) {
			worst = quads;
			worstTerrain = terrain;
		}
		total += quads;
		frames++;
	}
};

} // namespace

int main() {
	visage::Canvas canvas;
	canvas.setWindowless(static_cast<int>(kWidth), static_cast<int>(kHeight));
	if (!visage::Renderer::instance().supported()) {
		std::printf("SKIP  no renderer: %s\n", visage::Renderer::instance().errorMessage().c_str());
		return 77;
	}

	feathers::ParamBridge bridge;
	feathers::ui::WidgetRegistry registry;
	feathers::ui::NotifyTray tray;
	feathers::ui::SoundSelection selection;
	feathers::ui::EditHistory history;
	feathers::ui::TerrainMap terrain;
	int64_t now = 1000;
	feathers::ui::ExploreStage stage { bridge,  registry, selection, history,
		                               terrain, &tray,    [&now] { return now; } };
	feathers::ParamSet params;

	auto set = [&](feathers::ParamId id, double value) {
		params.setBase(static_cast<clap_id>(id), value);
		bridge.publish(params);
		bridge.refresh();
	};

	stage.setBounds(0.0f, 0.0f, kWidth, kHeight);
	bridge.refresh();
	selection.select(1);
	set(feathers::ParamId::Weather, 0.6);

	// Let the terrain finish, or the census measures an empty map rather than a full one — the whole
	// point is the worst case.
	for (int settle = 0; settle < 400; ++settle) {
		now += 16;
		stage.update();
		canvas.clearDrawnShapes();
		stage.draw(canvas);
		canvas.submit();
	}

	// TWO CENSUSES, because the per-zoom line and the summary want different things. `overall` runs
	// across every zoom and is what the arena is sized from; `perZoom` is emptied at the top of each
	// level so its line describes THAT level. Sharing one made every line after the first quote the
	// running worst — three zoom levels printing the same number, which reads as "zoom does not
	// matter" when what it really says is "the first level was the worst".
	Census overall;
	for (double zoom : { 1.0, 10.0, 40.0 }) {
		Census perZoom;
		stage.setZoom(zoom);
		for (int frame = 0; frame < 240; ++frame) {
			const double t = static_cast<double>(frame) / 240.0;
			set(feathers::ParamId::Path, 0.5 + 0.5 * std::sin(t * 2.0 * kPi * 3.0));
			set(feathers::ParamId::PosX, std::sin(t * 2.0 * kPi * 5.0));
			set(feathers::ParamId::PosY, std::cos(t * 2.0 * kPi * 8.0));

			now += 16;
			stage.update();
			// CLEARED AND INVALIDATED EACH FRAME, which is both necessary and a caveat. Necessary
			// because a canvas with no invalid rect submits nothing at all and the census reads zero;
			// a caveat because it makes every frame here a FULL repaint — one damage rectangle, so
			// the shape-per-rect multiplier is 1. A real frame repainting several areas multiplies
			// these counts, which is part of what the headroom below is for.
			canvas.clearDrawnShapes();
			stage.draw(canvas);
			canvas.submit();
			const uint64_t quads = visage::batchQuadsThisFrame();
			const uint64_t terrain = static_cast<uint64_t>(stage.terrainQuadCount());
			perZoom.add(quads, terrain);
			overall.add(quads, terrain);
		}
		std::printf("zoom x%-4.0f worst frame %6llu quads (terrain %6llu)\n", zoom,
		            static_cast<unsigned long long>(perZoom.worst),
		            static_cast<unsigned long long>(perZoom.worstTerrain));
	}

	std::printf("\n");
	std::printf("frames measured            %d\n", overall.frames);
	std::printf("mean frame                 %llu quads\n",
	            static_cast<unsigned long long>(overall.total / std::max(1, overall.frames)));
	std::printf("WORST FRAME, today         %llu quads\n",
	            static_cast<unsigned long long>(overall.worst));
	std::printf("  of which terrain         %llu\n",
	            static_cast<unsigned long long>(overall.worstTerrain));
	std::printf("\n");
	std::printf("arena, this build          %d quads\n", visage::transientQuadCapacity());
	std::printf("margin                     %.0fx the worst frame\n",
	            static_cast<double>(visage::transientQuadCapacity()) /
	                static_cast<double>(std::max<uint64_t>(1, overall.worst)));
	return 0;
}
