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

// THE DEVICE CARDS — the table shaper's two surfaces, the automodulator's strip and panel, the shaper's two plots, and the structural sweeps. Lines 30294–36148 of the one file, moved verbatim 2026-09-03; three mod-ring cases went to test_gui_widgets_mod_ring.cpp.

// SHARED BY EVERY PIECE OF THE devices SUITE: the includes, and the helpers that more than one piece uses (each piece also holds the helpers only it uses).
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

// THE DEVICE CARDS — the table shaper's two surfaces, the automodulator's strip and panel, the shaper's two plots, and the structural sweeps. Lines 30294–36148 of the one file, moved verbatim 2026-09-03; three mod-ring cases went to test_gui_widgets_mod_ring.cpp.

#include "fixtures.h"
#include "plugin/ui/device_shortcuts.h"
#include "card_head_check.h"
#include "plugin/ui/edge_pan.h"
#include "plugin/ui/mode_value_cell.h"
#include "plugin/ui/route_switch.h"
#include "plugin/ui/gamma_box.h"
#include "mod/twinfield/twinfield_types.h"
#include "plugin/ui/twinfield_card.h"
#include "plugin/ui/drum_device_card.h"
#include "plugin/ui/drum_device_expanded.h"
#include "fx/drum/drum_types.h"
#include "plugin/ui/likes_log.h"
#include "plugin/ui/pattern_likes_log.h"
#include "plugin/ui/inject_card.h"
#include "plugin/ui/owltt_level_meter.h"
#include "plugin/ui/dyn_meter.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/stage_rail.h"
#include "plugin/ui/smooth_card.h"
#include "plugin/ui/spectral_card.h"
#include "dsp/partial_spectrum.h"
#include "plugin/ui/spectral_expanded.h"
#include "plugin/ui/smooth_expanded.h"
#include "fx/smooth/smooth_tips.h"
#include "plugin/ui/phaser_pad.h" // phaser Task 9a: the explore pad over the phaser's policy
#include "fx/phaser/phaser_types.h"
#include "fx/delay/delay.h" // backlog LF: the taps strip is asserted against a running engine
#include "plugin/ui/delay_card.h"
#include "plugin/ui/delay_expanded.h"
#include "plugin/ui/delay_tips.h"
#include "plugin/ui/follower_card.h"
#include "plugin/ui/fader_pill.h"
#include "plugin/ui/noise_card.h"
#include "plugin/ui/macro_polarity.h"
#include "plugin/ui/macro_slot.h"
#include "plugin/device_preset.h"
#include "plugin/ui/surface_layers.h"

#include "backplane/container_bodies.h"
#include "composite/composite_types.h"
#include "fx/eroder/eroder_types.h" // backlog IQ: the Eroder's zone count and Tone
#include "fx/owltt/owltt_types.h"
#include "fx/owltt/owltt.h"
#include "fx/featherverb/featherverb_types.h" // backlog EI's per-panel ceiling table
// `lfo::eval` — the DSP's own waveform function, which the Phase-rotation case below uses as its
// independent restatement. The card's header reaches only `lfo_types.h`, which has the settings and
// not the evaluator.
#include "mod/lfo/lfo.h"
#include "mod/macros/macros.h" // the MACROS card's cases restate the output through the device's own decode
// The shared plot grid — its divisions are both what `drawGrid` puts on screen and what a drag
// sticks to, which is the subject of the last case in this file.
#include "plugin/ui/plot_grid.h"
#include "rack/rack_bridge.h"
#include "fx/featherverb/featherverb.h"
#include "plugin/ui/featherverb_expanded.h"
#include <cstdlib>
#include <new>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <functional>
#include "plugin/ui/ir_view.h"
#include "plugin/ui/knob.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "rack/chain_expanded.h"
// SKLITTER'S OWN POST CHAIN, CONSTRUCTED BARE — the fx-lane cases below hand `&sklitter` to
// `FxLaneRail::follow` because that is EXACTLY the pointer a host files for a container
// (`ContainerBodies::attach` stores the instance's storage, and this device IS that storage). A
// fixture that faked the handle would be testing its own fake.
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h" // kDescriptor — `adopt` fans out to BOTH lanes
#include "rack/inner_chain.h"
#include "fx/busutil/busutil_types.h"
#include "fx/resonate/resonate_lanes.h"
#include "rack/rack_device.h" // kMaxDepth
#include "plugin/ui/chain_column.h"
#include "plugin/ui/source_arm.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/sklitter_card.h"
#include "plugin/ui/device_control.h" // backlog GS: the Rate readout's road
#include "plugin/ui/pill_stack.h"   // backlog IL: the widget, tested before its first card
#include "plugin/ui/value_box.h"      // formatValue, the road every device without a text hook keeps
#include "fx/filter/filter_types.h"
#include "plugin/ui/sklitter_expanded.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/eq_card.h"
#include "plugin/ui/eq_expanded.h"
#include "plugin/ui/lfo_card.h"
#include "plugin/ui/menu_anchor.h"
#include "fx/delay/delay_types.h"
#include "menu_placement_check.h"
#include "hostslots/slot_host.h"          // host slots T7: a slot's name as `params.get_info` answers it
#include "plugin/ui/card_factory.h"       // host slots T7: a card built and destroyed by the case
#include "plugin/ui/mod_route_menu.h"     // host slots T7: the host rows and their ids

#include <algorithm>
#include <deque>
#include <bit>
#include <set>

#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <limits>
#include <random>
#include <thread>

//------------------------------------------------------------------------------------------------//
// FEATHERVERB'S INPUT AND ITS METER (backlog LA): IN, WET and GR beside the Input knob, on the card  //
// and in the window. Values asserted: which fraction a bar is at, where the meter sits, the redraws. //
//------------------------------------------------------------------------------------------------//

#include "plugin/ui/featherverb_card.h"

//------------------------------------------------------------------------------------------------//
// THE DELAY'S WINDOW AND ITS FEEDBACK FX LANE (delay-love S2)                                        //
//------------------------------------------------------------------------------------------------//

#include "fx/delay/delay_lanes.h"
#include "fx/clip/clip_tips.h"
#include "fx/clip/clip_types.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-const-variable"

namespace {
//------------------------------------------------------------------------------------------------//
// THE TABLE SHAPER'S TWO SURFACES.                                                                 //
//                                                                                                  //
// THESE EXIST BECAUSE THE REACHABILITY GUARD CONSTRUCTS THE PANEL AND NEVER LAYS IT OUT.           //
// `makeExpandedView` builds it and asks for its addressed controls; nothing calls `resized()`,     //
// nothing calls `draw()`, and nothing presses anything on it. So a panel whose layout divided by   //
// zero, whose switches overlapped, or whose pad wrote to the wrong control would pass that guard   //
// intact — the guard asks WHETHER a control is reached, not whether the geometry that reaches it   //
// is sane. Every case below asserts a VALUE or a RECTANGLE, never a render.                        //
//------------------------------------------------------------------------------------------------//

/// A lane holding one Shaper, plus its panel built the way the editor builds it.
struct ShaperFixture {
	FilterFixture f;
	size_t slot = 0;
	/// THE SHAPER'S ID — what the card and panel address; `slot` is only where the lane draws it.
	DeviceInstanceId device = kNoInstance;
	ui::ShaperCard* card = nullptr;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::ShaperExpanded* panel = nullptr;

	ShaperFixture() {
		slot = f.bridge.addFxDevice(feathers::fx::shaper::kTraits.tag);
		f.lane->resized();
		card = dynamic_cast<ui::ShaperCard*>(f.lane->cardAt(slot));
		device = f.bridge.fxDeviceAt(slot);
		view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, feathers::fx::shaper::kDescriptor);
		panel = dynamic_cast<ui::ShaperExpanded*>(view.get());
		if (panel != nullptr) {
			// AT ITS OWN MINIMUM, which is the size the overlay actually gives it — the overlay CLAMPS
			// rather than scrolling, so the minimum is the smallest real state and the one where a row
			// that does not fit gets cut off.
			const visage::Point min = panel->minimumSize();
			panel->setBounds(0.0f, 0.0f, min.x, min.y);
			panel->resized();
		}
	}
};
//------------------------------------------------------------------------------------------------//
// THE AUTOMODULATOR'S LFO STRIP.                                                                   //
//                                                                                                  //
// EVERY CASE HERE ASSERTS A VALUE THE STRIP WILL DRAW, NEVER A RENDERED FRAME, and CLAUDE.md says   //
// why in words that fit this widget exactly: "a comparison between two renders is evidence that     //
// SOMETHING changed, never that the right thing did". A waveform is the worst instance of it —      //
// every plausible bug here (the wrong coordinate, a dropped Manual, a picture that ignores Depth)   //
// still draws a perfectly convincing LFO.                                                          //
//------------------------------------------------------------------------------------------------//

/// A CARD IN A REAL LANE, at a coordinate **CHOSEN BY SWEEP AND NOT BY GUESSING**.
///
/// THE FIRST VERSION OF THIS FIXTURE USED (0.23, 0.61, 0.37) AND TWO CASES FAILED FOR THE FIXTURE
/// RATHER THAN FOR THE CODE: at flavour 0.61 `filterModDepth` is EXACTLY 0.0000, so the corner does
/// not move however far Depth or Manual is driven, and at mod 0.37 `stereoPhaseOffset` is exactly
/// 0.0000 too. That is CLAUDE.md's own recorded trap — *"(0.4,0.6,0.3) had `filterModDepth` and
/// `envDepthInfluence` both exactly 0" — arriving a third time, and it is a trap because the field
/// has genuine deadzones by construction (a φ triangle with a duty below 1 is zero across a band, on
/// purpose).
///
/// SO THE REPLACEMENT WAS MEASURED. A 91³ sweep scored each coordinate by its WEAKEST live field —
/// a coordinate is only as good as its deadest number — under a constraint that each axis sit at
/// least 0.15 from its declared default, so a card that ignored the row and drew the defaults would
/// FAIL rather than nearly pass. (0.76, 0.29, 0.53) won at 0.206, with `filterModDepth` 0.273,
/// `filterCutoffBase` 0.408 (at neither end of its range), `stereoPhaseOffset` 0.260,
/// `envDepthInfluence` -0.719, `svfFeedback` 0.209 and `lfoStartPhase` 0.470.
///
/// AND THE FIXTURE ASSERTS THAT RATHER THAN RECORDING IT, so retuning a bank makes the fixture say
/// so instead of quietly making six cases vacuous.
struct AutomodStripFixture : FilterFixture {
	static constexpr float kType = 0.76f;
	static constexpr float kFlavor = 0.29f;
	static constexpr float kMod = 0.53f;

	size_t slot = 0;
	DeviceInstanceId device = kNoInstance;
	ui::AutomodCard* card = nullptr;

	AutomodStripFixture() {
		namespace amd = feathers::fx::automod;
		slot = bridge.addFxDevice(amd::kTraits.tag);
		REQUIRE(slot != static_cast<size_t>(-1));
		device = bridge.fxDeviceAt(slot);
		lane->resized();
		card = dynamic_cast<ui::AutomodCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);
		set(amd::Param::Enabled, 1.0f);
		set(amd::Param::Type, kType);
		set(amd::Param::Flavor, kFlavor);
		set(amd::Param::Mod, kMod);

		// THE FIXTURE'S PREMISE, CHECKED. `REQUIRE` rather than `CHECK`: every case below is about
		// something moving, so a dead field here does not weaken a case, it empties it.
		const amd::Settings s = card->settings();
		const amd::AutomodDerived d = amd::deriveAutomod(amd::coordsOf(s, amd::nominalRateHz(s)));
		REQUIRE(d.filterModDepth > 0.1f);
		REQUIRE(d.filterCutoffBase > 0.05f);
		REQUIRE(d.filterCutoffBase < 0.95f);
		REQUIRE(std::abs(d.stereoPhaseOffset) > 0.1f);
		REQUIRE(d.lfoStartPhase > 0.05f);
		REQUIRE(d.lfoStartPhase < 0.95f);
	}

	void set(feathers::fx::automod::Param p, float v) {
		bridge.setDeviceControl(device, static_cast<size_t>(p), v);
	}
};
/// Every zoned-coordinate cell in a surface, wherever it was placed.
void collectZoneCells(visage::Frame* root, std::vector<ui::ZoneCoordCell*>* out) {
	if (auto* cell = dynamic_cast<ui::ZoneCoordCell*>(root)) {
		out->push_back(cell);
	}
	for (visage::Frame* child : root->children()) {
		collectZoneCells(child, out);
	}
}
//--------------------------------------------------------------------------------------------------//
// THE PERFORM CIRCLE ON THE PANEL — design §5, built 2026-09-19.                                     //
//--------------------------------------------------------------------------------------------------//
//
// **EVERY CLAIM HERE IS A VALUE, and the one render this arc makes is a separate, later look.**
// CLAUDE.md: a comparison between two renders is evidence that SOMETHING changed, never that the right
// thing did. What a pad can get wrong is arithmetic — which axis a pixel means, whether both axes are
// written, whether the puck drawn is the one the parameters hold — and none of it is recoverable from a
// picture.

/// The panel for a fresh SKLITTER, laid out at its own minimum. Returned with the fixture because the
/// panel does not own the host it reads.
struct PerformPanel {
	/// **THE OUTER MODULATION STATE, DECLARED FIRST SO IT IS DESTROYED LAST — and it is a fixture
	/// member because getting it wrong is a use-after-free that `ctest` cannot see.**
	///
	/// `SklitterExpanded::setModulation` reaches the GRID's controls and the perform pane's output
	/// chips, and each of those WATCHES the `ModState` and the `EditHistory` it is handed
	/// (`SourceArm` holds a `ModState&` and subscribes in its constructor). A case that declared its
	/// own pair AFTER the panel therefore let them die with watchers still on them.
	///
	/// **TWICE IN ONE SESSION, WHICH IS WHY IT MOVED HERE.** `FrameWatchers::~FrameWatchers` asserts on
	/// it and that assert is compiled out of the ordinary build, so both instances were GREEN through
	/// every assertion and then aborted in a destructor under the sanitizer gate. Two cases getting the
	/// same declaration order wrong is not two mistakes, it is a fixture that let them — members are
	/// destroyed in reverse declaration order, so putting the pair at the top makes the ordering
	/// structural and a case cannot reintroduce it.
	ui::ModState outer;
	ui::EditHistory outerHistory;

	ModFixture f;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::SklitterExpanded* panel = nullptr;
	feathers::DeviceInstanceId device = feathers::kNoInstance;

	/// Hand the panel the outer state this fixture owns — the road `gui.cpp` takes with `&impl_->mod`,
	/// and the only one a case should use.
	void armFrom() { panel->setModulation(&outer, &outerHistory); }

	PerformPanel() {
		namespace sc = feathers::fx::sklitter;
		const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
		device = f.bridge.fxDeviceAt(slot);
		view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, sc::kDescriptor);
		panel = dynamic_cast<ui::SklitterExpanded*>(view.get());
		if (panel != nullptr) {
			const visage::Point want = panel->minimumSize();
			panel->setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
			panel->resized();
		}
	}
};
/// Put both rails in a stated state and re-lay the panel out at the size that state wants. **The
/// re-layout is not a convenience and it is not the mechanism either**: in the editor a rail announces
/// its new width, the panel re-derives and tells its HOST that its minimum moved, and the host resizes
/// it — see the window-grows case, which drives that whole road through a real overlay. This fixture
/// holds the panel bare, with no host to tell, so the size is given to it here.
/// **`fxOpen` MEANS BOTH FX RAILS, which is what it meant when there was one.** Every existing caller
/// is asking "with the fx lane showing", and the answer that keeps their claims true is "every fx
/// lane". `setLanes` below is for the cases that are about the two rails DIFFERING.
void setRails(PerformPanel& p, bool circleOpen, bool fxOpen) {
	p.panel->circleRailForTest().setCollapsed(!circleOpen);
	p.panel->grainRailForTest().setCollapsed(!fxOpen);
	p.panel->postRailForTest().setCollapsed(!fxOpen);
	const visage::Point want = p.panel->minimumSize();
	p.panel->setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	p.panel->resized();
}
/// The three rails independently — the state space `setRails` deliberately cannot express, and the one
/// the split into two fx rails created. Same re-layout, same reason.
void setLanes(PerformPanel& p, bool circleOpen, bool grainOpen, bool postOpen) {
	p.panel->circleRailForTest().setCollapsed(!circleOpen);
	p.panel->grainRailForTest().setCollapsed(!grainOpen);
	p.panel->postRailForTest().setCollapsed(!postOpen);
	const visage::Point want = p.panel->minimumSize();
	p.panel->setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	p.panel->resized();
}
/// The same, with the BODY as the third thing that folds — eight states rather than four. Kept beside
/// `setRails` rather than replacing it: the cases above are about the two RAILS and say so, and a
/// helper that silently also decided the body's state would have made every one of them a claim about
/// three things.
void setFolds(PerformPanel& p, bool bodyOpen, bool circleOpen, bool fxOpen) {
	REQUIRE(p.panel->bodyRailForTest() != nullptr);
	p.panel->bodyRailForTest()->setCollapsed(!bodyOpen);
	setRails(p, circleOpen, fxOpen);
}
/// **A LIVE SKLITTER AND ITS ADOPTED POST CHAIN** — the far end of `containerSurfaceOf`, built the way
/// a host builds it: construct, then `adopt` a catalogue, which is *"the only moment a container may
/// build its record" (bolt-on-chains trap T5 — a bridge built at construct holds an empty catalogue
/// and offers nothing, with no error).
struct HeldChain {
	feathers::fx::sklitter::Sklitter device;
	/// **ADOPTED THROUGH THE OPS TABLE, WHICH IS WHAT FANS OUT TO BOTH LANES.** This said
	/// `device.postChain()->adopt(...)` while the rail had one column, and a fixture that adopted one
	/// lane would leave the grain lane with no bridge — so the rail's second column would never build
	/// and every case below would be asserting about a state the plugin cannot be in (trap T5, in the
	/// fixture). `ContainerOps::adopt` is the call a host makes.
	HeldChain() {
		REQUIRE(feathers::fx::sklitter::kDescriptor.container != nullptr);
		feathers::fx::sklitter::kDescriptor.container->adopt(&device, feathers::hostCatalogue());
	}
	/// The pointer a host would file for this instance, and the one every op table is handed.
	void* surface() { return &device; }
	feathers::rack::RackBridge& bridge() { return *device.postChain()->bridge(); }
	feathers::rack::RackBridge& grainBridge() { return *device.grainChain()->bridge(); }
};
/// **THE ROOM THE DISK AND ITS CELLS SHARE, RESTATED (backlog IR)** — under the heading, above the
/// output band, inside the padding, each axis floored at the yield floor's block. Restated from the
/// rail's furniture rather than read off `acrossFor`/`downFor`, which are under test. The band is
/// `outputBandFor`'s, which HX's own case pins.
std::pair<float, float> expectedRoomFor(float railWidth, float railHeight) {
	const float across = std::max(ui::CircleRail::kBlockFloor, railWidth - 2.0f * ui::CircleRail::kPadX);
	const float down = std::max(ui::CircleRail::kBlockFloor,
	                            railHeight - 2.0f * ui::Rail::kPadTop - ui::CircleRail::kHeadingHeight
	                                - ui::CircleRail::kGap - ui::CircleRail::outputBandFor(railWidth));
	return { across, down };
}
/// **THE LARGEST DISK THAT ROOM CAN HOLD WITH A CELL IN EACH OF ITS QUADRANTS (backlog IR).** A cell
/// is furthest from the centre at the room's corner, so its inner corner is at best
/// `(across/2 − kCellSide, down/2 − kCellSide)` out; the disk's grab reach (half its frame) must leave
/// `kCornerClear` to that corner, and the disk must fit the room on both axes. Centring is optimal
/// because the four corners are symmetric. `std::hypot`, not the production arithmetic.
float expectedOptimumDisk(float railWidth, float railHeight) {
	const auto [across, down] = expectedRoomFor(railWidth, railHeight);
	const float cornerX = across * 0.5f - ui::CircleRail::kCellSide;
	const float cornerY = down * 0.5f - ui::CircleRail::kCellSide;
	return std::min({ across, down, 2.0f * (std::hypot(cornerX, cornerY) - ui::CircleRail::kCornerClear) });
}
/// **THE SPARK RIG — a pad, a clock a case owns, and the two gestures that drive it.** Backlog HR.
///
/// Every claim below is about a VALUE (an emitted count, a velocity), never about a rendered buffer,
/// because a picture carries every quantity at once and a difference check passes whenever anything
/// differs — CLAUDE.md's rule, and the reason `sparksEmittedForTest` and `puckVelocityForTest` exist.
struct SparkRig {
	PerformPanel p;
	ui::PerformPad* pad = nullptr;
	feathers::ui::perform_pad::Disk disk;
	int64_t nowMs = 100000;

	SparkRig() {
		REQUIRE(p.panel != nullptr);
		setFolds(p, true, true, false);
		pad = &p.panel->padForTest();
		disk = pad->disk();
		REQUIRE(disk.valid());
		// **THE SEED BEFORE THE CLOCK, AND BOTH BEFORE ANY TICK.** A Poisson process is random, so a
		// case that cannot reproduce its own emissions cannot assert anything about them; this pins the
		// stream, and `setClockForTest` drops the previous puck sample with the clock that took it.
		pad->setSparkSeedForTest(ui::PerformPad::kSparkSeed);
		pad->setClockForTest([this] { return nowMs; });
	}

	/// MOVE THE PUCK THE WAY A HAND DOES — through the pad's own gesture, so this writes `Perf X` and
	/// `Perf Y` and the pad reads them back exactly as it does in the editor.
	void puckAt(float ax, float ay) {
		visage::MouseEvent e;
		e.position = { disk.cx + ax * disk.radius, disk.cy + ay * disk.radius };
		pad->onMouseDown().callback(e);
		pad->onMouseUp().callback(e);
	}

	/// ONE TICK OF THE PANEL'S HEARTBEAT: the clock moves, then the pad is asked. Answers what the pad
	/// answered — whether it requested a frame. `grain` defaults to zero so a case about the
	/// straight-through emitter says so by not mentioning the lane.
	bool tick(int64_t ms, float level, float grain = 0.0f) {
		nowMs += ms;
		return pad->updateSparks(level, grain);
	}

	/// THE SAME TICK WITH THE DEVICE'S GRAIN ONSET COUNTERS (backlog HR round 2), as `SklitterLevels`
	/// hands them to the pad in the editor.
	bool tickOnsets(int64_t ms, float level, float grain, const ui::PerformPad::GrainOnsetCounts& counts) {
		ui::PerformPad::SparkEvents events;
		events.onsets = counts;
		return tickEvents(ms, level, grain, events);
	}
	/// AND WITH REPEAT'S COUNTER AND PERIOD TOO.
	bool tickEvents(int64_t ms, float level, float grain, const ui::PerformPad::SparkEvents& events) {
		nowMs += ms;
		return pad->updateSparks(level, grain, &events);
	}

	/// **DRAIN TO EMPTY, so a following claim is about sparks this case emitted and not about leftovers.**
	/// Ticking at silence emits nothing and retires what is alive; `kSparkLife` at its longest is under
	/// a second, so 90 frames is comfortably past it.
	void drain() {
		for (int i = 0; i < 90; ++i) {
			tick(16, 0.0f);
		}
		REQUIRE(pad->sparksForTest().empty());
	}
};
/// THE PANEL'S CELL FOR ONE OF SKLITTER'S CONTROLS, as the widget it was seated as.
template <typename Widget>
Widget* sklitterCellFor(ui::GenericExpanded& panel, feathers::fx::sklitter::Param param) {
	const auto& indices = panel.paramIndicesForTest();
	const auto where = std::find(indices.begin(), indices.end(), static_cast<size_t>(param));
	if (where == indices.end()) {
		return nullptr;
	}
	return dynamic_cast<Widget*>(panel.cellsForTest()[static_cast<size_t>(where - indices.begin())]);
}
/// **ONE CARD, PHOTOGRAPHED ON ITS OWN** — the card's own paint (frame, head, body), which is where
/// every head occupant is drawn; its child frames are not, and none of them sits in the head.
void photographCard(ui::FxCard& card, const std::string& path) {
	const int width = static_cast<int>(std::ceil(card.width()));
	const int height = static_cast<int>(std::ceil(card.height()));
	REQUIRE(width > 0);
	REQUIRE(height > 0);
	visage::Canvas canvas;
	markedWindowless(canvas, width, height);
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
	visage::Region region;
	region.setBounds(0, 0, width, height);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	card.draw(canvas);
	canvas.endRegion();
	takeMarkedCapture(canvas, width, height).save(path.c_str());
}
//------------------------------------------------------------------------------------------------//
// THE MACROS CARD (macro-modulation B3)                                                            //
//------------------------------------------------------------------------------------------------//

namespace mac = feathers::mod::macros;
/// A MACROS DEVICE IN THE MODULATOR STRIP AND THE CARD THE STRIP BUILT FOR IT — asked of the strip, so
/// every case below is about the card a hand actually gets, and the cast is the claim that it is this one.
struct MacrosSeat {
	size_t slot = 0;
	DeviceInstanceId device = feathers::kNoInstance;
	ui::MacrosCard* card = nullptr;
};
MacrosSeat seatMacros(ModFixture& f) {
	MacrosSeat seat;
	seat.slot = f.bridge.addRackDevice(mac::kTraits.tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	seat.device = f.bridge.rackDeviceAt(seat.slot);
	REQUIRE(seat.device != feathers::kNoInstance);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::MacrosCard*>(dynamic_cast<const ui::MacrosCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
//------------------------------------------------------------------------------------------------//
// MATH (macro-modulation C) — its compact card ("make the MATH card much smaller").
//------------------------------------------------------------------------------------------------//

namespace mth = feathers::mod::math;
/// A MATH DEVICE IN THE MODULATOR STRIP AND THE CARD THE STRIP BUILT FOR IT — `seatMacros`' shape: the
/// cast is the claim that the strip builds `MathCard` for the tag rather than the generic grid.
struct MathSeat {
	size_t slot = 0;
	DeviceInstanceId device = feathers::kNoInstance;
	ui::MathCard* card = nullptr;
};
MathSeat seatMath(ModFixture& f) {
	MathSeat seat;
	seat.slot = f.bridge.addRackDevice(mth::kTraits.tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	seat.device = f.bridge.rackDeviceAt(seat.slot);
	REQUIRE(seat.device != feathers::kNoInstance);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::MathCard*>(dynamic_cast<const ui::MathCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
/// THE DESCRIPTOR INDEX A WIDGET'S CONTROL ADDRESSES, on this device — asked of the parameter road.
size_t addressOn(DeviceInstanceId device, const feathers::device::Modulatable* c) {
	const auto* road = dynamic_cast<const ui::DeviceControl*>(c);
	REQUIRE(road != nullptr);
	CHECK(road->device() == device);
	return road->control();
}
/// A frame's rectangle in its parent's coordinates.
struct Rect {
	float x, y, w, h;
	float right() const { return x + w; }
	float bottom() const { return y + h; }
	float midY() const { return y + h * 0.5f; }
};
//------------------------------------------------------------------------------------------------//
// FEATHERVERB'S CONV DRAWER (backlog JD): the conv pane is in the expanded window only, not the card   //
//------------------------------------------------------------------------------------------------//

namespace fvx = feathers::fx::featherverb;
/// A real Featherverb behind a real host that files its surface — the road the plugins take.
/// **THE OS'S DIALOGS, FAKED** (`ui::FileDialog`): no case ever shows a real one. `canShow` false is a desktop with no
/// portal and no zenity/kdialog, where Choose folder... falls back to the menu and Load IR... is greyed. Declare it
/// BEFORE the rig, so it outlives the panel whose open dialog it would be asked to cancel.
class FakeIrDialogs final : public ui::FileDialogBackend {
public:
	class FakeSession final : public Session {
	public:
		explicit FakeSession(FakeIrDialogs& owner) : owner_(owner) {}
		void cancel() override { ++owner_.cancels; }

	private:
		FakeIrDialogs& owner_;
	};
	std::unique_ptr<Session> start(const ui::FileDialogOptions& options,
	                               std::shared_ptr<ui::FileDialogChannel> channel) override {
		++starts;
		last = options;
		this->channel = std::move(channel);
		return std::make_unique<FakeSession>(*this);
	}
	bool available() override { return canShow; }
	/// The person answered; the panel hears of it when its dialog is polled, as the timer would.
	void answer(ui::FeatherverbExpanded& panel, ui::FileDialogResult result) {
		REQUIRE(channel != nullptr);
		channel->complete(std::move(result));
		CHECK(panel.fileDialogForTest().poll());
	}

	int starts = 0;
	int cancels = 0;
	bool canShow = true;
	ui::FileDialogOptions last;
	std::shared_ptr<ui::FileDialogChannel> channel;
};
struct VerbRig {
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	std::unique_ptr<feathers::rack::RackBridge> bridge;
	DeviceInstanceId id = kNoInstance;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::FeatherverbExpanded> panel;

	VerbRig() {
		engine.prepare(48000.0, 4096);
		bridge = std::make_unique<feathers::rack::RackBridge>(engine);
		bridge->prepare(48000.0, 4096);
		id = bridge->addDevice(fvx::kTraits.tag);
		REQUIRE(id != kNoInstance);
		bridge->drain();
		bridge->publish(engine);
		panel = std::make_unique<ui::FeatherverbExpanded>(*bridge, registry, id, fvx::kDescriptor);
		const visage::Point min = panel->minimumSize();
		panel->setBounds(0.0f, 0.0f, min.x, min.y);
	}
	fvx::Featherverb& verb() const { return *static_cast<fvx::Featherverb*>(bridge->containerSurfaceOf(id)); }
	visage::Frame* cell(fvx::Param p) const {
		const auto& params = panel->paramIndicesForTest();
		const auto& cells = panel->cellsForTest();
		for (size_t n = 0; n < params.size(); ++n) {
			if (params[n] == static_cast<size_t>(p)) {
				return cells[n];
			}
		}
		return nullptr;
	}
};
std::vector<std::string> headingsOf(const visage::PopupMenu& menu) {
	std::vector<std::string> out;
	for (const visage::PopupMenu& o : menu.options()) {
		if (o.hasOptions()) {
			out.push_back(o.name().toUtf8());
		}
	}
	return out;
}
//------------------------------------------------------------------------------------------------//
// THE DYNAMICS CARDS — COMP and CLIP.                                                               //
//                                                                                                  //
// VISIBILITY, METER NUMBERS AND DELIVERY, each asserted as a value: which widget is shown, which  //
// fraction a bar is drawn at, how many redraws the frame asked its handler for. Never a render.    //
//------------------------------------------------------------------------------------------------//

/// ONE DYNAMICS DEVICE IN THE LANE, ITS CARD, AND THE TWO ROADS A CASE DRIVES IT BY.
template <typename CardT>
struct DynCardFixture {
	ModFixture f;
	ui::FxLane* lane = nullptr;
	size_t slot = 0;
	DeviceInstanceId device = kNoInstance;
	CardT* card = nullptr;

	explicit DynCardFixture(const char* tag) {
		lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		slot = f.bridge.addFxDevice(tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		card = dynamic_cast<CardT*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);
		device = f.bridge.fxDeviceAt(slot);
	}

	void set(size_t control, float value) {
		f.bridge.setDeviceControl(device, control, value);
		f.bridge.refresh();
	}

	template <size_t N>
	void publish(const std::array<float, N>& values) {
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
	}
};
/// **COUNTS THE REDRAWS A FRAME ACTUALLY ASKS FOR** — the delivery, not the report. `Frame::redraw()`
/// asks its event handler only when the frame is visible, drawing and not already waiting for a frame;
/// this handler counts the asks made BY `target` (children share the handler and are not counted).
/// `rearm()` re-installs it, which clears visage's pending latch the way a drawn frame would.
/// **THE PREVIOUS HANDLER IS PUT BACK ON DESTRUCTION** — the frame outlives this probe, and a frame
/// left pointing at a dead handler reads it when it is torn down.
struct RedrawProbe {
	visage::Frame* target = nullptr;
	visage::FrameEventHandler* previous = nullptr;
	visage::FrameEventHandler handler;
	int count = 0;

	explicit RedrawProbe(visage::Frame* frame) : target(frame), previous(frame->eventHandler()) {
		handler.request_redraw = [this](visage::Frame* asked) {
			if (asked == target) {
				++count;
			}
		};
		target->setEventHandler(&handler);
	}
	~RedrawProbe() { target->setEventHandler(previous); }
	RedrawProbe(const RedrawProbe&) = delete;
	RedrawProbe& operator=(const RedrawProbe&) = delete;

	void rearm() { target->setEventHandler(&handler); }
};
/// A LINEAR PEAK AS THE BAR FRACTION THE PLAN SPECIFIES, restated independently of `dyn_meter`:
/// −60..0 dBFS, linear in dB.
float expectedLevel(float linear) {
	const float db = 20.0f * std::log10(linear);
	return std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
}
//--------------------------------------------------------------------------------------------------//
// BACKLOG GG — THE FOUR MULTIVALUE TOGGLES, AND WHAT REPLACED THEM                                   //
//--------------------------------------------------------------------------------------------------//
//
// The LFO's Waveform and Mode, the MSEG's Mode and the envelope's Mode were chips that printed their
// value and advanced on a press — so reaching Once from Loop APPLIED Free on the way (CLAUDE.md's
// "A MULTIVALUE TOGGLE IS AN ANTIPATTERN").
// Each is a compact `ValueBox` in the same cell now, whose press opens the declared list. These cases
// assert the WIDGET, the LIST and the WRITES as values, because none of the three is recoverable from a
// picture: a chip and a box are both a bordered word.

/// **EVERY FIELD WRITE, WRITTEN DOWN, ON ITS WAY THROUGH.** A pick's claim is "one value, and no value
/// in between" — a claim about the SEQUENCE of writes, which the final state cannot show: a cycle that
/// walked Loop -> Free -> LFO Retrig -> Gate -> Once ends exactly where a pick of Once does. So the
/// card is handed a host that records each `setField` and forwards it, and the case reads the record.
/// `BridgeDeviceHost` is `final`, hence a forwarder rather than a subclass.
class RecordingDeviceHost final : public backplane::DeviceHost {
public:
	explicit RecordingDeviceHost(backplane::DeviceHost& inner) : inner_(inner) {}

	struct Write {
		DeviceInstanceId device;
		size_t control;
		float value;
	};
	std::vector<Write> writes;

	const backplane::Axes& axes() const noexcept override { return inner_.axes(); }
	backplane::DeviceView devices() const noexcept override { return inner_.devices(); }
	float fieldAt(DeviceInstanceId device, size_t control) const override {
		return inner_.fieldAt(device, control);
	}
	void setField(DeviceInstanceId device, size_t control, float value) override {
		writes.push_back({ device, control, value });
		inner_.setField(device, control, value);
	}
	size_t fieldsPresentAt(DeviceInstanceId device) const override { return inner_.fieldsPresentAt(device); }
	float modulatedFieldAt(DeviceInstanceId device, size_t control) const override {
		return inner_.modulatedFieldAt(device, control);
	}
	double sampleRate() const noexcept override { return inner_.sampleRate(); }
	double secondsPerBeat() const noexcept override { return inner_.secondsPerBeat(); }
	size_t readingCountAt(DeviceInstanceId device) const noexcept override {
		return inner_.readingCountAt(device);
	}
	float readingAt(DeviceInstanceId device, size_t index) const noexcept override {
		return inner_.readingAt(device, index);
	}
	bool playheadAt(DeviceInstanceId device, uint32_t& ordinal, float& phase) const override {
		return inner_.playheadAt(device, ordinal, phase);
	}
	bool sourceIsBipolarNow(backplane::Source source) const override {
		return inner_.sourceIsBipolarNow(source);
	}
	float routeAmount(backplane::Source source, backplane::Destination destination) const override {
		return inner_.routeAmount(source, destination);
	}
	void setRouteAmount(backplane::Source source, backplane::Destination destination, float amount) override {
		inner_.setRouteAmount(source, destination, amount);
	}
	bool routedElsewhere(backplane::Destination destination, backplane::Source except) const override {
		return inner_.routedElsewhere(destination, except);
	}
	void beginRouteGesture() override { inner_.beginRouteGesture(); }
	void endRouteGesture() override { inner_.endRouteGesture(); }
	void forEachRouteFrom(backplane::Source source, void* ctx,
	                      void (*f)(void* ctx, backplane::Destination d)) const override {
		inner_.forEachRouteFrom(source, ctx, f);
	}
	std::string destinationName(backplane::Destination destination) const override {
		return inner_.destinationName(destination);
	}
	backplane::CorePolicy corePolicy() const noexcept override { return inner_.corePolicy(); }
	/// THE HAND'S HOST ROADS, recorded too (the phaser plot's lazy gestures): a touch, a Begin, an End — by control.
	struct Hand {
		char what; ///< 't' touch, 'b' begin, 'e' end
		size_t control;
	};
	std::vector<Hand> hands;
	void beginControlGesture(DeviceInstanceId device, size_t control) override {
		hands.push_back({ 'b', control });
		inner_.beginControlGesture(device, control);
	}
	void endControlGesture(DeviceInstanceId device, size_t control) override {
		hands.push_back({ 'e', control });
		inner_.endControlGesture(device, control);
	}
	void touchControl(DeviceInstanceId device, size_t control) override {
		hands.push_back({ 't', control });
		inner_.touchControl(device, control);
	}
	[[nodiscard]] size_t handsOn(size_t control, char what) const {
		return static_cast<size_t>(std::count_if(hands.begin(), hands.end(),
		                                         [&](const Hand& h) { return h.control == control && h.what == what; }));
	}

private:
	backplane::DeviceHost& inner_;
};
//------------------------------------------------------------------------------------------------//
// RESONATE'S CARD AND ITS RING VIEW.                                                               //
//                                                                                                  //
// Values from the view's own geometry seam and the widgets' visibility — never a render. The        //
// readings go in through the bridge's real road (`publishDeviceReadings`), packed by the device's   //
// own `packReading`, so the card reads exactly what a running device would hand it.                 //
//------------------------------------------------------------------------------------------------//

namespace rsn = feathers::fx::resonate;
/// A SYNTHETIC READING: model `m`, node i displaced by `amp * sin(2 pi i / 32)`, rest at half that, one
/// sounding head (note 60), the travel phase at `travel`, a pluck `age` ticks old at 0.25.
rsn::Reading syntheticReading(rsn::Model m, float amp = 0.5f, float travel = 0.0f, float age = 1024.0f) {
	rsn::Reading r;
	r.model = static_cast<float>(m);
	for (size_t i = 0; i < rsn::kPictureNodes; ++i) {
		const float s = std::sin(2.0f * 3.14159265f * static_cast<float>(i % rsn::kChainNodes) / 32.0f);
		r.nodes[i] = amp * s;
		r.rest[i] = 0.5f * amp * s;
	}
	r.travel = travel;
	r.headGains[0] = 1.0f;
	r.headNotes[0] = 60.0f;
	r.pluckPos = 0.25f;
	r.pluckAge = age;
	return r;
}
template <typename Fixture>
void publishReading(Fixture& d, const rsn::Reading& r) {
	std::array<float, rsn::kReadingCount> flat {};
	rsn::packReading(r, flat);
	d.publish(flat);
}
using ResonateFixture = DynCardFixture<ui::ResonateCard>;
bool overlaps(const visage::Frame* a, const visage::Frame* b) {
	return !(a->x() + a->width() <= b->x() + 0.01f || b->x() + b->width() <= a->x() + 0.01f
	         || a->y() + a->height() <= b->y() + 0.01f || b->y() + b->height() <= a->y() + 0.01f);
}
/// THE CURRENT ROW IS MARKED AND FLAGGED, AND NO OTHER ROW IS.
void checkMarkedOnly(const visage::PopupList& list, int current) {
	for (int i = 0; i < list.numOptions(); ++i) {
		CAPTURE(i);
		const std::string name = list.option(i).name().toUtf8();
		CHECK(list.option(i).selected() == (i == current));
		CHECK(name.rfind(i == current ? "X  " : "     ", 0) == 0);
	}
}
/// THE CURRENT ROW, AS DRAWN, SITS IN THE MIDDLE OF THE LIST'S ROOM — within half a row, or flush with the
/// end the list cannot scroll past. Computed from the list's own row geometry and scroll position.
void checkCentred(const visage::PopupList& list, int current) {
	const float full = list.renderHeight();
	const float h = list.height();
	const float rowTop = static_cast<float>(list.yForIndex(current));
	const float rowH = static_cast<float>(list.yForIndex(current + 1)) - rowTop;
	const float scroll = list.yPosition();
	const float drawnCentre = rowTop + 0.5f * rowH - scroll;
	CAPTURE(full);
	CAPTURE(h);
	CAPTURE(rowTop);
	CAPTURE(rowH);
	CAPTURE(scroll);
	CAPTURE(drawnCentre);
	REQUIRE(full > h + rowH); // the list scrolls: otherwise there is nothing for this to prove
	// IN VIEW, whole.
	CHECK(rowTop - scroll >= -0.5f);
	CHECK(rowTop + rowH - scroll <= h + 0.5f);
	const bool atTop = scroll <= 0.5f;
	const bool atFoot = scroll >= full - h - 0.5f;
	const bool middle = std::abs(drawnCentre - 0.5f * h) <= 0.5f * rowH + 0.5f;
	CHECK((middle || (atTop && drawnCentre < 0.5f * h) || (atFoot && drawnCentre > 0.5f * h)));
	// THE FIRST VISIBLE ROW, by the same geometry: as many rows above the current one as fit in half the room.
	const float pad = static_cast<float>(list.yForIndex(0));
	const auto firstVisible = static_cast<int>(std::floor((scroll - pad) / rowH + 0.5f));
	const auto halfRoom = static_cast<int>(std::floor(0.5f * h / rowH));
	if (middle) {
		CHECK(std::abs((current - firstVisible) - halfRoom) <= 1);
	}
}
} // namespace

namespace cascade {
// **"ADD BEFORE…" IS A CASCADE** — "Add before" in the fx lanes had regressed. The slot menu's row
// opens the add menu FROM INSIDE ITS OWN SELECTION CALLBACK, from the same box: `showMenuAt` read that as the re-click toggle (the slot menu is still open while its pick is being
// delivered) and closed instead of opening. Driven as a hand does it — the dots pressed through the root,
// the rows picked through `PopupMenuFrame::optionSelected` — because the `*ForTest` verbs never open a
// menu at all. One case per surface, so one surface's red cannot hide another's.

using menu_check::newestPopup;
inline void press(visage::Frame* root, visage::Frame& frame, const visage::Bounds& local) {
	const visage::Point at =
	    frame.positionInWindow() + visage::Point { local.x() + local.width() * 0.5f, local.y() + local.height() * 0.5f };
	dragThroughRoot(root, at, at);
}
} // namespace cascade

namespace {
//------------------------------------------------------------------------------------------------//
// HOST SLOTS T7: "EXPOSE TO HOST" ON A DEVICE CONTROL'S RIGHT-CLICK (backlog JU).                  //
// Driven through the menu a right-click builds and its own                                         //
// pick (`Knob::contextMenu`), never a synthesised click (banned on the development display). What the host  //
// sees is read through `hostslots::SlotHost`, the object the plugin answers `params.get_info` with //
// — and the visit is `serviceSlots`' own two calls, in its order.                                  //
//------------------------------------------------------------------------------------------------//

namespace hs = feathers::hostslots;
/// " · " in UTF-8, spelled once — the separator the host's slot names use.
const std::string kSlotDot = "\xc2\xb7";
/// One row of a popup as a hand sees it: its label, whether it can be picked, and the id a pick sends.
struct SeenRow {
	std::string label;
	bool enabled = true;
	int id = -1;
};
std::vector<SeenRow> rowsOf(const visage::PopupMenu& menu) {
	std::vector<SeenRow> rows;
	for (const visage::PopupMenu& o : menu.options()) {
		rows.push_back({ o.isBreak() ? std::string() : o.name().toUtf8(), o.enabled(), o.id() });
	}
	return rows;
}
/// **THE PLUGIN'S LOOKUP, RESTATED** (`FeathersPlugin::BridgeControlLookup`): the bridge resolves a target.
struct BridgeLookup final : hs::ControlLookup {
	explicit BridgeLookup(const ParamBridge& b) : bridge(b) {}
	bool lookup(const hs::Target& target, const feathers::device::ParamDesc** desc, double* value) const override {
		return bridge.resolveSlotTarget(target, desc, value);
	}
	const ParamBridge& bridge;
};
/// **ONE MAIN-THREAD VISIT, AS `serviceSlots` MAKES IT**: reconcile, the write roads, then what the host is
/// owed — returned rather than sent, so a case can count it.
clap_param_rescan_flags visit(ParamBridge& bridge, hs::SlotHost& host) {
	(void)bridge.reconcileSlotsIfMoved();
	(void)bridge.serviceSlotRoads();
	return host.rescanOwed();
}
/// **A KNOWN POOL: EMPTY.** A fresh instance may be given defaults (T8 fills the filter's cutoff first), and
/// every assertion below about which slot a pin takes is about the pool's own order from empty.
void emptyPool(ParamBridge& bridge) { REQUIRE(bridge.slotPool().restore(hs::PoolSnapshot {})); }
//------------------------------------------------------------------------------------------------//
// THE DISPERSER — its card and its window (from the approved sketch, 2026-09-25)                      //
//------------------------------------------------------------------------------------------------//

namespace dspr = feathers::fx::disperser;
bool overlaps(const visage::Bounds& a, const visage::Bounds& b) {
	return !(a.right() <= b.x() || b.right() <= a.x() || a.bottom() <= b.y() || b.bottom() <= a.y());
}
//------------------------------------------------------------------------------------------------//
// THE CARD AND ITS WINDOW AS ONE DEVICE: the selected card's outline wears its    //
// window's outline colour, a press on the card's head brings an open window to the front, and a     //
// folded card keeps its way into the window. Values, through the real editor (`ModFixture`).        //
//------------------------------------------------------------------------------------------------//

/// A POINT ON THE CARD'S HEAD THAT IS THE HEAD ITSELF — not the enable, the expand, a menu — found by
/// asking the card's own two answers, so it holds for a top head and a spine alike.
std::optional<visage::Point> bareHeadPoint(const ui::FxCard& card) {
	for (float y = 1.0f; y < static_cast<float>(card.height()); y += 2.0f) {
		for (float x = 1.0f; x < static_cast<float>(card.width()); x += 2.0f) {
			const visage::Point p { x, y };
			if (card.pressOnHead(p) && !card.headControlAt(p)) {
				return p;
			}
		}
	}
	return std::nullopt;
}
/// ONE PRESS AND RELEASE THROUGH THE CARD'S OWN MOUSE ROAD, in the card's coordinates — the road every
/// card funnels its head presses through (`FxCard::noteCardPress`). Not through the root: the open
/// windows float over the lane, and the question here is what the CARD does with a press, not which
/// frame a point resolves to.
void pressCard(ui::FxCard& card, visage::Point p) {
	visage::MouseEvent e;
	e.position = p;
	e.window_position = card.positionInWindow() + p;
	e.event_frame = &card;
	e.button_id = visage::kMouseButtonLeft;
	e.button_state = visage::kMouseButtonLeft;
	e.is_down = true;
	e.repeat_click_count = 1;
	card.processMouseDown(e);
	e.is_down = false;
	e.button_state = visage::kMouseButtonNone;
	card.processMouseUp(e);
}
/// The device the front panel is a window onto.
feathers::DeviceInstanceId frontDevice(ui::ExpandedOverlay& overlay) {
	const ui::ExpandedView* content = overlay.contentForTest();
	return content != nullptr ? content->watchedDevice() : feathers::kNoInstance;
}
//------------------------------------------------------------------------------------------------//
// THE DELAY'S TAPS STRIP (backlog LF).                                                           //
//                                                                                                //
// The strip is asserted against a RUNNING ENGINE: a `Delay` is driven here, its readings handed to //
// the lane the way the plugin's sink hands them, and the card's ticks compared with the heads and  //
// gains the engine reports through its own seams — two roads to one number.                      //
//------------------------------------------------------------------------------------------------//

namespace dl = feathers::fx::delay;
//--------------------------------------------------------------------------------------------------//
// SPECTRAL — the Additive drawer's plot and panels over the device (2026-10-13; the lead's Option A).  //
//--------------------------------------------------------------------------------------------------//
namespace spx = feathers::fx::spectral;
// THE STEP SEQUENCER (backlog MX) — the card is the step grid, drawn and edited on its face (its header comes through
// fixtures.h; the merge with NA/MW put its helpers in the one anonymous namespace above).

namespace ssq = feathers::mod::stepseq;
struct StepSeqSeat {
	size_t slot = 0;
	DeviceInstanceId device = feathers::kNoInstance;
	ui::StepSeqCard* card = nullptr;
};
StepSeqSeat seatStepSeq(ModFixture& f) {
	StepSeqSeat seat;
	seat.slot = f.bridge.addRackDevice(ssq::kTraits.tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	seat.device = f.bridge.rackDeviceAt(seat.slot);
	REQUIRE(seat.device != feathers::kNoInstance);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::StepSeqCard*>(dynamic_cast<const ui::StepSeqCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
namespace snh = feathers::mod::samplehold;
struct SampleHoldSeat {
	size_t slot = 0;
	ui::SampleHoldCard* card = nullptr;
};
/// A SAMPLE & HOLD IN THE STRIP AND THE CARD THE STRIP BUILT — the cast is the claim that the strip builds the device's
/// own card for the tag rather than the generic grid.
SampleHoldSeat seatSampleHold(ModFixture& f) {
	SampleHoldSeat seat;
	seat.slot = f.bridge.addRackDevice(snh::kTraits.tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::SampleHoldCard*>(
	    dynamic_cast<const ui::SampleHoldCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
/// A RACK INSTANCE, running where a rack engine runs it, filed in a fixture's lane as the host files it.
struct RackOnLane {
	feathers::rack::RackEngine bodies { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	void* body = nullptr;
	feathers::DeviceInstanceId id = feathers::kNoInstance;
	size_t slot = 0;
	explicit RackOnLane(ModFixture& f) {
		bodies.prepare(48000.0, 4096);
		const size_t at = bodies.addDevice(feathers::rack::parallel::kTraits.tag);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		body = bodies.slotAtLane(at).storage.get();
		REQUIRE(body != nullptr);
		slot = f.bridge.addFxDevice(feathers::rack::parallel::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		id = f.bridge.fxDeviceAt(slot);
		f.bridge.fxBodies().attach(id, body);
	}
	feathers::rack::parallel::Rack& rack() const { return *feathers::rack::parallel::rackOf(body); }
};
/// EVERY VISIBLE CONTROL ON A CARD, its rectangle in the card's own coordinates (the card's direct children: its widgets).
std::vector<std::pair<const visage::Frame*, visage::Bounds>> visibleControlsOf(const ui::FxCard& card) {
	std::vector<std::pair<const visage::Frame*, visage::Bounds>> out;
	for (const visage::Frame* c : card.children()) {
		if (c->isVisible() && c->width() > 0.0f && c->height() > 0.0f) {
			out.emplace_back(c, c->bounds());
		}
	}
	return out;
}
/// THE GEOMETRY A HORIZONTAL CHAIN NEEDS OF A CARD, AT ONE STATE: its content fits the unit, no two controls overlap (a tab
/// strip and the E/S chips that sit ON it by design excepted), and every control lies in the card's body, above its
/// outputs strip.
void checkFitsChainUnit(ui::FxCard& card) {
	card.setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	card.setBounds(0.0f, 0.0f, card.laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	card.resized();
	(void)card.update();
	CHECK(card.contentHeight() <= ui::kCardUnitHeight);
	CHECK_FALSE(card.scrollable());
	const ui::CardFrame frame = card.ownFrame();
	const auto controls = visibleControlsOf(card);
	REQUIRE(controls.size() > 3);
	const auto inside = [](const visage::Bounds& in, const visage::Bounds& b) {
		return b.x() >= in.x() - 0.5f && b.y() >= in.y() - 0.5f && b.right() <= in.right() + 0.5f && b.bottom() <= in.bottom() + 0.5f;
	};
	for (size_t i = 0; i < controls.size(); ++i) {
		CAPTURE(i);
		CHECK(inside(frame.body, controls[i].second));
		for (size_t j = i + 1; j < controls.size(); ++j) {
			const bool tabsPair = dynamic_cast<const ui::PatternTabs*>(controls[i].first) != nullptr
			                      || dynamic_cast<const ui::PatternTabs*>(controls[j].first) != nullptr;
			if (tabsPair) {
				continue;
			}
			const visage::Bounds a = controls[i].second;
			const visage::Bounds b = controls[j].second;
			const bool overlap = a.x() < b.right() - 0.5f && b.x() < a.right() - 0.5f && a.y() < b.bottom() - 0.5f
			                     && b.y() < a.bottom() - 0.5f;
			CAPTURE(j);
			CHECK_FALSE(overlap);
		}
	}
	// **AND EVERY WORD FITS ITS RECT** (the lead, 2026-11-05: "LEN…", "RA…", "Melod"): a knob's caption is printed whole,
	// and a box's caption and reading both fit — side by side in a compact box, each on its row in a stacked one. Walked
	// through the whole tree, so a box inside a cell (the Feel's Amount) is asked too.
	std::vector<const visage::Frame*> stack { &card };
	while (!stack.empty()) {
		const visage::Frame* fr = stack.back();
		stack.pop_back();
		if (fr != &card && (!fr->isVisible() || fr->width() <= 0.0f)) {
			continue;
		}
		if (const auto* knob = dynamic_cast<const ui::Knob*>(fr)) {
			CAPTURE(std::string(knob->label()));
			CHECK(knob->drawnCaption().find("...") == std::string::npos);
		}
		if (const auto* box = dynamic_cast<const ui::ValueBox*>(fr)) {
			const std::string caption = box->label();
			const std::string reading = box->readout();
			CAPTURE(caption);
			CAPTURE(reading);
			const float cw = caption.empty() ? 0.0f : ui::textWidth(caption, ui::font(ui::kKnobLabelSize));
			const float rw = ui::textWidth(reading, ui::font(ui::kKnobReadoutSize));
			if (box->isCompact()) {
				CHECK(cw + (caption.empty() ? 0.0f : 3.0f) + rw <= box->readoutBox().width + 0.5f);
			}
			else {
				CHECK(cw <= box->captionBox().width + 0.5f);
				CHECK(rw <= box->readoutBox().width + 0.5f);
			}
		}
		for (const visage::Frame* c : fr->children()) {
			stack.push_back(c);
		}
	}
	// **AND EVERY SOURCE CHIP IS WHOLE AND IN THE CARD** — placed where the host will place it (`outputChipBoundsFor` over
	// the card's own strip); a chip that does not fit comes back empty and would be hidden, an output lost.
	const feathers::device::Descriptor* desc = card.descriptor();
	REQUIRE(desc != nullptr);
	const std::vector<visage::Bounds> chips = ui::outputChipBoundsFor(*desc, frame.outputs, card.outputLabels(), card.outputsShown());
	CHECK(chips.size() == desc->outputCount());
	const visage::Bounds whole { 0.0f, 0.0f, static_cast<float>(card.width()), static_cast<float>(card.height()) };
	for (size_t i = 0; i < chips.size(); ++i) {
		CAPTURE(i);
		if (((card.outputsShown() >> i) & 1u) == 0u) {
			CHECK(chips[i].width() == 0.0f); // NOT ON THIS CARD (design §7.10): another tab's, or a lane of no Modulation tab
			continue;
		}
		CHECK(chips[i].width() > 0.0f);
		CHECK(inside(whole, chips[i]));
	}
}
/// A RACK ON FEATHERS' LANE WHOSE HOLDER MIRRORS (`ParamBridge::outerGateOver`), its owed mirror written, with the
/// window built over it — the venue for NO item 2's follow-ups.
struct MirroredRack {
	// THE STATE AND THE HISTORY FIRST (backlog PM): the panel's and the lane's widgets watch them, so they outlive them.
	ui::ModState mod;
	ui::EditHistory history;
	ModFixture f;
	RackOnLane r { f };
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::ParallelRackExpanded* panel = nullptr;
	MirroredRack() {
		r.rack().setOuterGate(ParamBridge::outerGateOver(&f.bridge, r.id));
		(void)f.bridge.serviceOwnControls();
		REQUIRE(r.rack().mirroredForTest());
		view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, feathers::rack::parallel::kDescriptor);
		panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->setBounds(0.0f, 0.0f, 900.0f, 600.0f);
		panel->modulationBound(&mod, &history);
		(void)panel->update();
	}
	~MirroredRack() {
		view.reset();
		f.bridge.fxBodies().detach(r.id, r.body);
		(void)f.bridge.removeFxDevice(r.slot);
	}
	backplane::Destination dest(uint8_t slot, feathers::rack::parallel::SlotControl c) const {
		return backplane::Destination::control(r.id, feathers::rack::parallel::slotParam(slot, c));
	}
};
} // namespace

namespace {
namespace lane_check {
namespace pt = feathers::mod::arp::pat;
namespace pn = feathers::mod::arp::pattern;
/// A PATTERN WINDOW over its own device in a rack, with an undo history: the window, its lane and its Engine rail.
struct Window {
	ModFixture f;
	size_t slot = 0;
	DeviceInstanceId id = feathers::kNoInstance;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::PatternExpanded* window = nullptr;
	ui::EditHistory history;
	explicit Window(float extra = 0.0f) {
		slot = f.bridge.addRackDevice(pt::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		id = f.bridge.rackDeviceAt(slot);
		view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
		window = dynamic_cast<ui::PatternExpanded*>(view.get());
		REQUIRE(window != nullptr);
		window->setModulation(nullptr, &history);
		const visage::Point min = window->minimumSize();
		window->setBounds(0.0f, 0.0f, min.x, min.y + extra);
		// EVERY STEP A PLAIN NOTE OR A REST: no ratchets or grace notes (Detail), so a step is one strike.
		f.deviceHost.setField(id, pt::pinAt(0, feathers::mod::arp::genes::Pin::Detail), 0.0f);
		f.deviceHost.setField(id, pt::at(0, pt::Field::Zone), 420.0f);
		(void)window->updateContent();
	}
	~Window() { (void)f.bridge.removeRackDevice(slot); }
	ui::PatternLane& lane() const { return window->lane(); }
	std::vector<float> edits() const {
		std::vector<float> block(pt::kEditsWords);
		if (!f.deviceHost.blockAt(id, pt::kEditsBlock, block)) {
			pt::writeEditState(pt::EditState {}, block.data());
		}
		return block;
	}
	size_t notes() const {
		size_t n = 0;
		for (size_t k = 0; k < lane().cycleForTest().length; ++k) {
			n += lane().cycleForTest().steps[k].kind == pn::PatternStep::Kind::Note ? 1u : 0u;
		}
		return n;
	}
};
/// ONE STRIKE THE DEVICE SOUNDED: its key, velocity, and where its off landed (samples after its on; -1 none).
struct Strike {
	int key = -1;
	double velocity = 0.0;
	int64_t length = -1;
};
/// **WHAT THE DEVICE PLAYS FROM THIS ROW AND THESE EDITS** — a fresh `PatternDevice` handed the host's row and the
/// edits block (the road a patch takes into the plugin), C4 held, the transport at 120 bpm: each step's strikes, by
/// step (a sixteenth = 6000 samples at 48 kHz), over one 16-step cycle. Its built cycle is returned too.
inline std::map<int64_t, std::vector<Strike>> play(const backplane::DeviceHost& host, DeviceInstanceId id,
                                            pn::PatternCycle* cycle = nullptr) {
	constexpr double kRate = 48000.0;
	constexpr int64_t kStep = 6000;
	pt::PatternDevice d;
	d.prepare(kRate);
	std::vector<float> row(pt::kParamCount);
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = host.fieldAt(id, i);
	}
	d.setFields(row.data(), row.size());
	std::vector<float> block(pt::kEditsWords);
	if (host.blockAt(id, pt::kEditsBlock, block)) {
		REQUIRE(d.writeEdits(block.data(), block.size()));
	}
	feathers::device::NoteEvent on {};
	on.kind = feathers::device::NoteEvent::Kind::On;
	on.noteId = 3;
	on.key = 60;
	on.velocity = 0.9;
	std::array<feathers::device::NoteEvent, 8> passed {};
	size_t n = 0;
	d.processNotes(&on, 1, passed.data(), passed.size(), &n);
	std::map<int64_t, std::vector<Strike>> out;
	std::map<int, std::pair<int64_t, size_t>> sounding; // key -> (on sample, index in its step's list)
	std::vector<feathers::device::NoteEvent> events(257);
	int64_t now = 0;
	const int64_t until = 17 * kStep;
	while (now < until) {
		feathers::device::TransportClock c = feathers::device::TransportClock::fromSecondsPerBeat(0.5);
		c.songPositionBeats = static_cast<double>(now) / kRate / 0.5;
		c.songPositionSeconds = static_cast<double>(now) / kRate;
		c.playing = true;
		uint32_t next = 0;
		const size_t got = d.emit(c, static_cast<uint64_t>(now), events.data(), 256, &next);
		for (size_t i = 0; i < got; ++i) {
			const auto& e = events[i];
			if (e.kind == feathers::device::NoteEvent::Kind::On && now < 16 * kStep) {
				auto& list = out[now / kStep];
				list.push_back({ e.key, e.velocity, -1 });
				sounding[e.key] = { now, list.size() - 1 };
			}
			else if (e.kind == feathers::device::NoteEvent::Kind::Off && sounding.count(e.key) != 0) {
				const auto [at, index] = sounding[e.key];
				out[at / kStep][index].length = now - at;
				sounding.erase(e.key);
			}
		}
		now += std::min<int64_t>({ static_cast<int64_t>(std::max<uint32_t>(next, 1)), 32, until - now });
	}
	if (cycle != nullptr) {
		*cycle = d.cycleForTest(0);
	}
	return out;
}
/// A CLICK AS THE PLATFORM SENDS ONE: the first of a run counts 1 (`visage::Window::handleMouseDown`), not the fixture's 0.
inline visage::MouseEvent once(visage::MouseEvent e) {
	e.repeat_click_count = 1;
	return e;
}
} // namespace lane_check
} // namespace

namespace {
/// A DELAY IN A RACK, with the editor's road to it — what the window and the card are built over.
struct DelayRig {
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	feathers::rack::RackBridge bridge { engine };
	DeviceInstanceId id = kNoInstance;
	DelayRig() {
		engine.prepare(48000.0, 4096);
		bridge.prepare(48000.0, 4096);
		id = bridge.addDevice(feathers::fx::delay::kTraits.tag);
		REQUIRE(id != kNoInstance);
		bridge.drain();
		bridge.publish(engine);
	}
	void* surface() const { return bridge.containerSurfaceOf(id); }
};
/// RUN THE RIG'S ENGINE (noise at -6 dBFS) and hand the editor what it published.
void runDelayRig(DelayRig& rig, int frames) {
	std::vector<float> l(256), r(256);
	uint32_t seed = 3;
	for (int done = 0; done < frames; done += 256) {
		const int n = std::min(256, frames - done);
		for (int i = 0; i < n; ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[static_cast<size_t>(i)] = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		rig.bridge.drain();
		rig.engine.processStereo(l.data(), r.data(), n, feathers::backplane::unknownBlock());
		rig.bridge.publish(rig.engine);
	}
}
} // namespace

//------------------------------------------------------------------------------------------------//
// HARDENING: UI POLLS AT REST, THE ROUTE LIST'S ROWS, AND A CARD DRAG KEYED ON ITS DEVICE            //
//------------------------------------------------------------------------------------------------//

// THE COUNTING OPERATOR NEW EXISTS ONLY WHERE NOTHING ELSE OWNS THE ALLOCATOR: a sanitiser supplies its own and a
// replacement beside it defeats its tracking. Detected for GCC (`__SANITIZE_*__`) and clang (`__has_feature`).
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define FEATHERS_TEST_SANITISED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)
#define FEATHERS_TEST_SANITISED 1
#endif
#endif
//--------------------------------------------------------------------------------------------------//
// THE ERODER'S CARD, PLOT AND WINDOW.                                                                //
//--------------------------------------------------------------------------------------------------//

namespace ero = feathers::fx::eroder;

namespace {
constexpr size_t eroAt(ero::Param p) { return static_cast<size_t>(p); }
float eroField(DynCardFixture<ui::EroderCard>& d, ero::Param p) { return d.f.deviceHost.fieldAt(d.device, eroAt(p)); }
/// A WHOLE FRAME OF READINGS: `pre` and `post` flat at the levels given, the counters as given.
std::array<float, ero::kReadingCount> eroFrame(float frameA, float frameB, float preDb, float postDb) {
	std::array<float, ero::kReadingCount> v {};
	v[static_cast<size_t>(ero::Reading::FrameA)] = frameA;
	for (size_t k = 0; k < feathers::dsp::kAnalyserBins; ++k) {
		v[static_cast<size_t>(ero::Reading::PreFirst) + k] = preDb - static_cast<float>(k);
		v[static_cast<size_t>(ero::Reading::PostFirst) + k] = postDb - static_cast<float>(k);
	}
	v[static_cast<size_t>(ero::Reading::FrameB)] = frameB;
	v[static_cast<size_t>(ero::Reading::PitchSemitones)] = 60.0f;
	// THE LIMITERS LET EVERYTHING BY unless a case says otherwise.
	v[static_cast<size_t>(ero::Reading::LimiterInnerGain)] = 1.0f;
	v[static_cast<size_t>(ero::Reading::LimiterOuterGain)] = 1.0f;
	return v;
}
//------------------------------------------------------------------------------------------------------------------------------//
// THE ERODER'S SHARED SECTION ON ITS FACES: Emphasis, Smear, Tap, Restore, In Loop, Loop, Interval.                            //
//------------------------------------------------------------------------------------------------------------------------------//

/// THE VISIBLE CHILDREN OF A FACE, but the ones in `skip`.
std::vector<visage::Frame*> eroVisible(visage::Frame* face, std::vector<visage::Frame*> skip) {
	std::vector<visage::Frame*> out;
	for (visage::Frame* child : face->children()) {
		if (child->isVisible() && std::find(skip.begin(), skip.end(), child) == skip.end()) {
			out.push_back(child);
		}
	}
	return out;
}
bool eroSameSet(std::vector<visage::Frame*> a, std::vector<visage::Frame*> b) {
	std::sort(a.begin(), a.end());
	std::sort(b.begin(), b.end());
	return a == b;
}
/// NOTHING OVERLAPS, EVERYTHING INSIDE.
void eroApart(const std::vector<visage::Frame*>& shown, float width, float height) {
	for (size_t i = 0; i < shown.size(); ++i) {
		CAPTURE(i);
		CHECK(shown[i]->x() >= 0.0f);
		CHECK(shown[i]->y() >= 0.0f);
		CHECK(shown[i]->right() <= width);
		CHECK(shown[i]->bottom() <= height);
		for (size_t j = i + 1; j < shown.size(); ++j) {
			CAPTURE(j);
			const bool apart = shown[i]->right() <= shown[j]->x() || shown[j]->right() <= shown[i]->x()
			                   || shown[i]->bottom() <= shown[j]->y() || shown[j]->bottom() <= shown[i]->y();
			// HOISTED: typeid of an expression with a call in it is evaluated, which Apple clang refuses under -Werror.
			const auto& frameI = *shown[i];
			const auto& frameJ = *shown[j];
			CAPTURE(typeid(frameI).name());
			CAPTURE(typeid(frameJ).name());
			CAPTURE(shown[i]->x());
			CAPTURE(shown[i]->y());
			CAPTURE(shown[i]->right());
			CAPTURE(shown[i]->bottom());
			CAPTURE(shown[j]->x());
			CAPTURE(shown[j]->y());
			CAPTURE(shown[j]->right());
			CAPTURE(shown[j]->bottom());
			CHECK(apart);
		}
	}
}
struct EroFaces {
	DynCardFixture<ui::EroderCard> d { ero::kTraits.tag };
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::EroderExpanded* panel = nullptr;
	EroFaces() {
		view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
		panel = dynamic_cast<ui::EroderExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
		refresh();
	}
	void set(ero::Param p, float v) {
		d.set(eroAt(p), v);
		refresh();
	}
	void mode(ero::Mode m) { set(ero::Param::Mode, static_cast<float>(m)); }
	void refresh() {
		(void)d.card->updateContent();
		(void)panel->updateContent();
	}
};
//--------------------------------------------------------------------------------------------------//
// THE SLOT ROW: a pair of pairs with a route switch between, and the three-row mode cell and the
// route switch it is made of. Stand-ins are the Eroder's own Drive, Type, Shape, Mix and Track.
//--------------------------------------------------------------------------------------------------//

struct SlotNotes final : ui::Notifier {
	std::vector<std::string> said;
	void say(std::string_view text) override { said.emplace_back(text); }
};
/// THE WHOLE SLOT ROW at the Eroder card's row width: `PairCell("Slots", PairCell("", Knob, ModeValueCell), PairCell("",
/// Knob, ModeValueCell))` with a `RouteSwitch` between them.
struct SlotRow {
	DynCardFixture<ui::EroderCard> d;
	SlotNotes notes;
	std::vector<std::unique_ptr<ui::DeviceControl>> controls;
	std::unique_ptr<ui::Knob> knobA, knobB;
	std::unique_ptr<ui::ModeValueCell> cellA, cellB;
	std::unique_ptr<ui::PairCell> innerA, innerB, outer;
	std::unique_ptr<ui::RouteSwitch> route;
	float cell = 0.0f; ///< one column of the card's four-across row
	float rowW = 0.0f;
	float rowH = 0.0f;

	SlotRow() : d(ero::kTraits.tag) {
		cell = static_cast<float>(d.card->drivePairForTest()->width()) * 0.5f;
		rowW = cell * 4.0f;
		rowH = ui::PairCell::heightFor(ui::Knob::columnHeight(ui::EroderCard::kKnobDiameter));
		const auto control = [this](ero::Param p) -> ui::DeviceControl& {
			controls.push_back(std::make_unique<ui::DeviceControl>(d.f.deviceHost, d.device, ero::kDescriptor, eroAt(p)));
			return *controls.back();
		};
		const auto slot = [&](std::unique_ptr<ui::Knob>& knob, std::unique_ptr<ui::ModeValueCell>& mode,
		                      std::unique_ptr<ui::PairCell>& pair, ero::Param amount, const char* name) {
			// THE KNOB IS CAPTIONED WITH ITS SLOT'S NAME, one letter: a word does not fit half a quarter-row.
			knob = std::make_unique<ui::Knob>(control(amount), name, &notes);
			knob->setFaceDiameter(ui::EroderCard::kKnobDiameter);
			std::vector<ui::ZoneSelect::ZoneName> types = ui::driveTypeZoneNames();
			mode = std::make_unique<ui::ModeValueCell>(control(ero::Param::DriveMode), "Type", std::move(types),
			                                           [](size_t) { return true; });
			mode->setFaceDiameter(ui::EroderCard::kKnobDiameter);
			(void)mode->addFollower(eroAt(ero::Param::DriveShape),
			                        std::make_unique<ui::ValueBox>(control(ero::Param::DriveShape), ""), "S");
			(void)mode->addFollower(eroAt(ero::Param::Mix), std::make_unique<ui::ValueBox>(control(ero::Param::Mix), ""), "M");
			mode->setStacked(true);
			(void)mode->refresh();
			pair = std::make_unique<ui::PairCell>("", *knob, *mode);
		};
		slot(knobA, cellA, innerA, ero::Param::Drive, "A");
		slot(knobB, cellB, innerB, ero::Param::Amount, "B");
		route = std::make_unique<ui::RouteSwitch>(control(ero::Param::Track), &notes);
		outer = std::make_unique<ui::PairCell>("Slots", *innerA, *innerB);
		outer->setMiddle(*route, ui::RouteSwitch::kWidth);
		outer->setBounds(0.0f, 0.0f, rowW, rowH);
	}

	/// A frame's rectangle in the outer pair's own coordinates.
	[[nodiscard]] visage::Bounds inOuter(const visage::Frame& f) const {
		float x = 0.0f;
		float y = 0.0f;
		for (const visage::Frame* p = &f; p != outer.get(); p = p->parent()) {
			x += static_cast<float>(p->x());
			y += static_cast<float>(p->y());
		}
		return { x, y, static_cast<float>(f.width()), static_cast<float>(f.height()) };
	}
};
} // namespace


#pragma GCC diagnostic pop
