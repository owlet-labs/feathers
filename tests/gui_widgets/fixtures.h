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

// THE WIDGET SUITE'S SHARED FIXTURES. One file held every case and every fixture until 2026-09-03
// (36,148 lines and 554 cases at b9feb1b, one translation unit, 140–220 s per UI edit).
// The cases now live in six binaries under
// tests/gui_widgets/, cut by what people edit together; everything a case in more than one of them
// stands on is here, moved verbatim. A fixture that only one binary uses stays in that binary.
//
// The helpers were in anonymous namespaces; they are in ONE named namespace now with a
// using-directive at the end, so every case body compiles exactly as it did. Struct and class
// bodies stay in-class (implicitly inline); the free functions are declared here and defined once
// in fixtures.cpp.


// The editor's controls, driven by injecting events straight into them.
//
// WHY NOT SYNTHETIC CLICKS. Three sessions were spent trying to verify these paths by pointing
// xdotool at a real window, and all three failed for different reasons — coordinates off by the
// window manager's decoration, a blanked display swallowing input, another window stacked above ours
// taking the click. A test that reports on the display server's mood is not a test of this code.
//
// visage's own event entry points are public, and they are what its hit test calls once it has
// decided which frame a click belongs to. So a widget with bounds and a MouseEvent is the whole
// apparatus: no window, no GPU, no display, and what is exercised is our handler rather than X11's
// willingness to deliver. Hit testing is asserted directly instead, through hitsEngine().
//
// What this cannot cover is visage's own dispatch — that a click at a screen position reaches the
// frame under it — which is visage's to get right and is not ours to re-test.
#include "plugin/bridge_device_host.h"
#include "plugin/ui/slot_edit.h"
#include "doctest.h"

#include "spectrum.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <limits>
#include <set>
#include <typeinfo>

#include "plugin/editor_prefs.h"
#include "plugin/gui.h"
#include "plugin/ui/plugin_window.h"
#include "plugin/gui_bridge.h"
#include "plugin/device_registry.h"
#include "plugin/ui/automod_terrain.h"
#include "plugin/params.h"
#include "plugin/ui/knob_tray.h"
#include "plugin/ui/param_knob.h"
#include "plugin/ui/controls.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/header_bar.h"
#include "plugin/ui/engine_rail.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/env_card.h"
#include "plugin/ui/mseg_card.h"
#include "plugin/ui/mod_panel.h"
#include "plugin/ui/notify_tray.h"
#include "plugin/preset_store.h"
#include "plugin/ui/preset_browser.h"
#include "dsp/slice_build.h"
#include "plugin/ui/orientation_row.h"
#include "plugin/ui/theme.h"
#include <visage/ui.h>
#include "plugin/ui/tuning_row.h"
#include "plugin/ui/param_row.h"
#include "dsp/morph_renderer.h"
#include "dsp/osc_voice.h"
#include "dsp/stair_renderer.h"
#include "dsp/vox_bake.h"
#include "plugin/ui/celestial.h"
#include "plugin/ui/perform_stage.h"
#include "plugin/character_params.h"
#include "plugin/ui/explore_stage.h"
#include "plugin/ui/drop_gap.h"
#include "plugin/ui/filter_card.h"
#include "device_includes.inc" // every registered device's types (backlog NY.1), where this file used to name four
#include "plugin/ui/busutil_card.h"
#include "plugin/ui/comp_card.h"
#include "plugin/ui/comp_expanded.h"
#include "fx/comp/comp_lanes.h"
#include "plugin/ui/disperser_card.h"
#include "plugin/ui/disperser_expanded.h"
#include "plugin/ui/phaser_card.h"
#include "plugin/ui/phaser_expanded.h"
#include "plugin/ui/clip_card.h"
#include "plugin/ui/resonate_card.h"
#include "plugin/ui/resonate_expanded.h"
#include "plugin/ui/chain_card.h"
#include "plugin/ui/parallel_rack_card.h"
#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/macros_card.h"
#include "plugin/ui/math_card.h"
#include "plugin/ui/stereo_tool_card.h"
#include "plugin/ui/rtrig_card.h"
#include "plugin/ui/sample_hold_card.h"
#include "plugin/ui/stepseq_card.h"
#include "plugin/ui/follower_card.h"
#include "plugin/ui/arp_card.h"
#include "mod/arp/pattern_types.h"
#include "plugin/ui/pattern_card.h"
#include "plugin/ui/pattern_expanded.h"
#include "mod/arp/pattern_device.h"
#include "plugin/ui/arp_expanded.h"
#include "plugin/ui/emitters_front.h"
#include "plugin/ui/drawn_card.h"
#include "plugin/ui/drawn_expanded.h"
#include "plugin/ui/shaper_card.h"
#include "plugin/ui/automod_expanded.h"
#include "plugin/ui/shaper_expanded.h"
#include "plugin/ui/waypoint_card.h"
#include "plugin/ui/waypoint_expanded.h"
#include "plugin/ui/harm_card.h"
#include "fx/automod/automod.h"
#include "plugin/ui/automod_card.h"
#include "plugin/ui/trace_marker.h"
#include "plugin/ui/owltt_card.h"
#include "plugin/ui/harm_scope.h"
#include "plugin/ui/expanded_overlay.h"
#include "plugin/ui/window_controls_side.h"
#include "plugin/ui/surface_layers.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/harmonic_row.h"
#include "plugin/ui/owltt_expanded.h"
#include "plugin/ui/zone_coord_cell.h"
#include "plugin/ui/harm_expanded.h"
#include "plugin/ui/sound_stack.h"
#include "plugin/ui/feature_norm.h"
#include "plugin/ui/terrain.h"
#include "plugin/ui/wave_display.h"
#include "plugin/ui/widget_registry.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>
#include <cstdlib>
#include <filesystem>
#include "fx/filter/multimode_filter.h"

#include <fstream>
#include <numbers>

#include "visage_app/application_editor.h"
#include <system_error>
#include <unistd.h>
#include <vector>

using namespace feathers;
using namespace feathers::ui;

namespace gui_widgets_tests {


// `planeAt` STOOD HERE AND HAD NO CALLERS. It came in with the explore work and its last use went
// with a case that was rewritten; the compiler had been saying so on every build.
//
// DELETED RATHER THAN SILENCED, because the argument for killing a harmless warning is not about the
// warning: a build that prints six benign ones is a build where the seventh, dangerous one is
// invisible. That is not hypothetical here — mseg::kTraits shipped a NULL tag, and the
// -Wmissing-field-initializers that would have caught it had to be picked out of exactly that kind of
// noise by somebody who went looking.
//
// If a case needs the builder's own plane again, `terrainFramesAt(pin, home, residue).morph` is the
// one line this was, and calling the engine's accessor rather than restating it was the point of it.

/// A WINDOWLESS CAPTURE THAT PROVES IT IS A PICTURE OF THE FRAME JUST DRAWN (backlog CW, IT).
///
/// A capture off a headless canvas can come back as something nobody drew, and look plausible:
/// zeros, noise, another widget's pixels sheared by a different pitch, or a raw fill byte. MEASURED
/// 2026-09-23: about one whole-shard run in three on lavapipe, because the canvas's RGBA16F
/// gradient atlas starts out as uninitialised memory and some contents stop every shape on the
/// canvas from landing (IT has the reproducer). A case that samples such a frame reports a CONTENT
/// failure ("no ink in the head") or, worse, passes on it.
///
/// So the canvas is made ONE ROW TALLER than the frame under test, and the capture draws a marker
/// row there in a colour unique to this capture. If every pixel of that row reads back as that
/// colour, draws landed and this readback is of this frame. Otherwise the case stops with a
/// message naming the venue. What the case gets back is cropped to `width` x `height`, so none of
/// its own arithmetic sees the marker.
///
/// It proves that THE CANVAS PAINTED, not that the widget did. A widget that draws nothing still
/// passes this guard and still fails its own content assertions, which is the known-bad this was
/// checked against.
void markedWindowless(visage::Canvas& canvas, int width, int height);
visage::Screenshot takeMarkedCapture(visage::Canvas& canvas, int width, int height);

/// NO PLANE AT ALL: every lean zero and every rate exactly one, so the country reduces to the
/// one-scalar reading of home that shipped before the field was opened. Spelled out at the sites that
/// mean it — the stair cases, where the macro comes from the pattern family and no plane is involved.


/// A mouse event at a point inside the widget.
///
/// The two position fields are DELIBERATELY DIFFERENT and must stay that way. visage's names read
/// backwards from what they mean: `position` is the point within the frame — what a widget wants —
/// while `relative_position` is how far the mouse MOVED since the previous event, for
/// relative-mouse mode. This helper used to set both to (x, y), which no real window ever does, and
/// that one convenience hid a bug in every mouse handler we had: all four read the movement, so a
/// press set its parameter from a delta of zero and pinned it to the minimum. The tests asserted
/// the right behaviour and passed anyway, because the fixture had been written to agree with our
/// reading of the platform instead of with the platform.
///
/// The movement below is therefore small, fixed, and obviously not the position. Any handler that
/// goes back to reading `relativePosition()` fails these tests at once, which is the point.
/// HOW MANY FRAMES A STATE NOTIFY SHOULD REACH — every watcher plus everything under it.
///
/// Derived rather than written down, because the literal it replaces (5) was the bug wearing a
/// passing test: a watcher grew a child that draws from the same state, the notify stopped at the
/// parent, and five was still five. Asking the same question the notification answers, by a
/// different route, is what keeps the assertion honest when a surface gains a child.
size_t expectedStateFrames(const feathers::Gui& gui);

visage::MouseEvent at(float x, float y);

/// A SECOND CLICK, WHICH IS WHAT ARMS A CARD'S GESTURE.
///
/// **A SINGLE PRESS NO LONGER DRAGS A CARD**, and that is the point of the split rather than an
/// inconvenience for these cases: it was reported collapse and reorder sharing one press, and
/// `FxCard::noteCardPress` now separates them on the click count. A case that drove a single press and
/// expected a reorder was, after the change, asking for the behaviour the change exists to prevent.
visage::MouseEvent doubleAt(float x, float y);

/// The same event with a modifier held, or as the second click of a double-click. Separate helpers
/// rather than arguments to `at`, so a test reads as the gesture it performs.
visage::MouseEvent fineAt(float x, float y);

visage::MouseEvent doubleClickAt(float x, float y);

/// THE THIRD CLICK OF A TRIPLE (backlog LS) — visage counts them, so a triple arrives as a 2 and then a 3,
/// and a case drives all three presses: `at`, `doubleClickAt`, then this.
visage::MouseEvent tripleClickAt(float x, float y);

/// Opt/Alt held — the MSEG editor's curvature gesture (Option on macOS). visage
/// spells the same bit `kModifierAlt` and reads it back through `isOptionDown()`.
visage::MouseEvent optAt(float x, float y);

/// Sets an environment variable for as long as it is in scope, and puts back exactly what was there
/// — including its absence. Two tests read FEATHERS_REDUCED_MOTION, and a bare setenv/unsetenv pair
/// leaks the variable into every later test in the binary if an assertion between them fails, which
/// is a failure that moves.
class ScopedEnv {
public:
	ScopedEnv(const char* name, const char* value) : name_(name) {
		if (const char* existing = std::getenv(name)) {
			had_ = true;
			previous_ = existing;
		}
		setenv(name, value, 1);
	}
	~ScopedEnv() {
		if (had_) {
			setenv(name_, previous_.c_str(), 1);
		}
		else {
			unsetenv(name_);
		}
	}
	ScopedEnv(const ScopedEnv&) = delete;
	ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
	const char* name_;
	bool had_ = false;
	std::string previous_;
};

/// Everything the editor posted, in order.
std::vector<UiParamEvent> drain(ParamBridge& bridge);

const ParamDesc& descOf(ParamId id);


/// A stage the size the editor gives it, with a clock a test can wind.
struct StageFixture {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	NotifyTray tray;
	int64_t now = 1000;
	PerformStage stage { bridge, registry, &tray, [this] { return now; } };
	ParamSet params;

	StageFixture() {
		stage.setBounds(0.0f, 0.0f, 1072.0f, 560.0f);
		bridge.refresh();
	}

	/// Put Path where the engine would have it, both readings, and let the editor see it.
	void setPath(double base, double modulated) {
		const clap_id id = static_cast<clap_id>(ParamId::Path);
		params.setBase(id, base);
		params.setModOffset(id, modulated - base);
		bridge.publish(params);
		bridge.refresh();
	}
	void setPath(double value) { setPath(value, value); }

	void set(ParamId id, double value) {
		params.setBase(static_cast<clap_id>(id), value);
		bridge.publish(params);
		bridge.refresh();
	}

	/// Where the patch says a sound sits, read the way the stage reads it. Taken from the parameter
	/// table rather than written out, so a moved default moves the test with it instead of past it.
	BlendPoint placeOf(size_t slot) const {
		return { static_cast<float>(bridge.value(
			         static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::PlaceX)))),
			     static_cast<float>(bridge.value(
			         static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::PlaceY)))) };
	}
};


struct WaveFixture {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	WaveDisplay wave { bridge, registry };
	ParamSet params;

	WaveFixture() {
		wave.setBounds(0.0f, 0.0f, WaveDisplay::kWidth, WaveDisplay::kHeight);
		bridge.refresh();
	}

	void set(ParamId id, double value) { params.setBase(static_cast<clap_id>(id), value); }
	void publish() {
		bridge.publish(params);
		bridge.refresh();
	}
};


struct StackFixture {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	NotifyTray tray;
	SoundSelection selection;
	FrameWatchers previewWatchers;
	EditHistory history;
	SoundStack stack { bridge, registry, selection, history, previewWatchers, &tray };
	ParamSet params;

	/// The rail at the height the editor gives it in a 1240x700 window: the header and the knob tray
	/// come off the top and bottom first. Taken from the widgets' own constants rather than written
	/// out, so a taller header moves this with it.
	static constexpr float kRailHeight = 700.0f - HeaderBar::kHeight - KnobTray::kHeight;

	StackFixture() {
		stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, kRailHeight);
		bridge.refresh();
	}

	void set(ParamId id, double value) {
		params.setBase(static_cast<clap_id>(id), value);
		bridge.publish(params);
		bridge.refresh();
	}

	double read(ParamId id) const { return bridge.value(static_cast<clap_id>(id)); }

	/// MAKE A SLOT HOLD A SOUND, the way a hand does: author its identity. The rail draws consumed
	/// slots only now, so a case that wants to press something on slot 3 has to put a sound there
	/// first — which is the state a patch is in by the time anyone is pressing anything.
	///
	/// It deliberately does NOT enable the slot: consumed and enabled are different questions, and
	/// several cases below turn on exactly that difference (a parked sound is in the rail and out of
	/// the constellation).
	void consume(size_t slot, double zone) {
		set(static_cast<ParamId>(soundParam(slot, SoundField::Zone)), zone);
	}

	/// Drain what the editor posted back into the engine, the way the plugin's binding does, so an edit
	/// a verb has just made is visible to the next read.
	void settle() {
		bridge.drain([&](const UiParamEvent& event) {
			if (event.kind == UiParamEvent::Kind::Value) {
				params.setBase(event.id, event.value);
			}
		});
		bridge.publish(params);
		bridge.refresh();
	}

	/// A point in the middle of slot `n`'s card — where a hand aiming at the sound would land, well
	/// clear of the switch on the right.
	/// THE MIDDLE OF EACH CONTROL, ASKED OF THE WIDGET rather than recomputed here. These used to be
	/// arithmetic copied out of the rail's constants, which was fine while every slot was drawn at a
	/// fixed height — and became wrong the moment the rail started drawing consumed slots only, at
	/// heights that depend on how many stances each sound remembers. A test that restates a widget's
	/// geometry is a test that can disagree with it, which is how a case comes to press empty space and
	/// report that a control is broken.
	visage::Point centreOfBounds(const visage::Bounds& b) const {
		return { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	}
	visage::Point cardPoint(size_t slot) const {
		const visage::Bounds card = stack.cardBoundsForTest(slot);
		return { card.x() + 30.0f, card.y() + card.height() * 0.5f };
	}
	visage::Point switchPoint(size_t slot) const {
		return centreOfBounds(stack.switchBoundsForTest(slot));
	}
	visage::Point clonePoint(size_t slot) const {
		return centreOfBounds(stack.cloneBoundsForTest(slot));
	}
	visage::Point previewPoint(size_t slot) const {
		return centreOfBounds(stack.previewBoundsForTest(slot));
	}
	visage::Point deletePoint(size_t slot) const {
		return centreOfBounds(stack.deleteBoundsForTest(slot));
	}
	visage::Point newSoundPoint() const { return centreOfBounds(stack.newRowBoundsForTest()); }
	visage::Point variationPoint(size_t slot, size_t index) const {
		const visage::Bounds row = stack.variationBoundsForTest(slot, index);
		return { row.x() + 30.0f, row.y() + row.height() * 0.5f };
	}
	visage::Point variationStorePoint(size_t slot, size_t index) const {
		return centreOfBounds(stack.variationStoreBoundsForTest(slot, index));
	}
	visage::Point variationForgetPoint(size_t slot, size_t index) const {
		return centreOfBounds(stack.variationForgetBoundsForTest(slot, index));
	}

	/// A GESTURE IN THREE PARTS, the way visage delivers one — press, drag, release. Written out
	/// rather than folded into one call because the cases below turn on the parts: a press that never
	/// travels is still a click, and a release with no target moves nothing.
	void press(visage::Point p) { stack.onMouseDown().callback(at(p.x, p.y)); }
	void dragTo(visage::Point p) { stack.onMouseDrag().callback(at(p.x, p.y)); }
	void release(visage::Point p) { stack.onMouseUp().callback(at(p.x, p.y)); }
	/// The whole reorder gesture: pick a card up and drop it so it ENDS UP at position `to`.
	///
	/// IT AIMS AT A GAP NOW, NOT AT A CARD. A drop target is a bumper between rows, so "drop it on row
	/// 4" is not a thing a hand can express — what it can express is "put it above row 4" or "below
	/// row 4", and those are different outcomes. This helper keeps every caller's INTENT ("the sound
	/// ends up at position `to`") by aiming at the gap that produces it: the gap ABOVE the target when
	/// moving up the list, and the gap BELOW it when moving down, since the dragged row is lifted out
	/// on the way.
	///
	/// AND IT ASKS THE WIDGET WHERE THAT GAP IS rather than restating the geometry. Aiming at a card's
	/// CENTRE — which this did — stopped meaning what it looked like the moment a slot could be taller
	/// than its card: a sound with a stance row is 55px against a 38px card, so the centre of the card
	/// is ABOVE the middle of the row and lands in the gap before it.
	void dragCard(size_t from, size_t to) {
		const size_t gap = to < from ? to : to + 1;
		const visage::Bounds bumper = stack.gapBounds(gap);
		const visage::Point start = cardPoint(from);
		const visage::Point end { start.x, bumper.y() + bumper.height() * 0.5f };
		press(start);
		dragTo(end);
		release(end);
		settle();
	}

	double zoneOf(size_t slot) const {
		return read(static_cast<ParamId>(soundParam(slot, SoundField::Zone)));
	}
	/// EVERY SLOT AT ONCE — the shape an undo claim has to be made in. A record that put back the slot
	/// somebody was looking at and left one of the others shifted would pass any narrower assertion.
	std::array<SlotState, kMaxSounds> everySlot() const {
		std::array<SlotState, kMaxSounds> all;
		for (size_t slot = 0; slot < kMaxSounds; ++slot) {
			all[slot] = readSlotState(bridge, slot);
		}
		return all;
	}
};


/// Wait for a REQUESTED WINDOW to finish, or give up.
///
/// THE WINDOW IS PART OF THE CONDITION, and forgetting that made this harness lie the moment the map
/// started keeping its last good snapshot: a completed old map still reports every row done, so
/// waiting on the row count alone returned instantly and the case that followed compared the OLD
/// terrain against itself. The snapshot carries its window precisely so a reader can tell which map it
/// is holding — so a waiter has to ask.
///
/// THE BOUND IS A TIMEOUT, NOT AN EXPECTATION, AND IT WAS MEASURED (backlog AV, 2026-09-03). Every
/// call across all six widget binaries was traced under the venue, one process per case, on a host
/// under load 10 (mseg-grid at 94b5348): 170 waits per pass at RUN_CASES_JOBS=4, 34 of them in
/// controls+explore also at JOBS 1 and 8; ZERO timeouts. `settle`: n=29 median 32/38/47 ms, p95
/// 303/338/339, max 311/347/359 at JOBS 1/4/8, and 415 ms at most in the other four binaries (the
/// dahdsr content-key case makes dozens of 300-415 ms waits, which is its 42.7 s under ASan);
/// `settleRefined`: n=3, max 284/336/332. So the 20 s and 60 s here are 48x and 178x the worst wait
/// seen. They cost nothing while a settle succeeds -- both return the instant it does -- and only
/// decide how long a BROKEN settle burns before it fails. Lowering them (10x the worst, 5 s and 5 s)
/// would make that failure seconds instead of a minute, but the ASan factor is unmeasured and the
/// gate runs these at JOBS=4 under ASan: measure there before touching either number.
///
/// **THE ASan FACTOR IS NOW MEASURED, and the numbers below changed MEANING rather than value**
/// (2026-09-21, playbook 8cm). Every figure above was taken while these parameters were spent as an
/// ITERATION COUNT — `for (i < milliseconds) { ...; sleep_for(1ms); }` — so the real bound stretched
/// with host load, silently and by an unknown factor. They are wall-clock deadlines now, which is
/// what the name always claimed, and that made the ASan venue the one place the change could bite:
/// it is the slowest, so it is where a bound that used to stretch and now does not would fail first.
///
/// IT DID NOT. A full `sanitize-gate.sh` run — 24 ctest entries at JOBS=4 under ASan, on a host at
/// load average 38 — completed with **every settle succeeding inside the 20 s / 60 s deadlines** and
/// zero sanitizer reports. The one red in that run was `"a rebuild does not blank the chart"` failing
/// its VOID statistics (mean 20.4% against a 10% bound, worst 87.5% against 60%), which is the
/// builder being starved rather than a settle giving up — its own `REQUIRE(settle(...))` passed and
/// the case ran all 120 frames. So the deadlines have real headroom in the slowest venue this repo
/// owns, and the measurement the paragraph above asked for has been made.
///
/// **AND THEN IT DID, AT HIGHER LOAD — SO THE BOUND IS NOW A STALL** (backlog KB, 2026-09-25). The
/// content-key case went red at load 43-73 under ASan. `milliseconds` is how long the builder may do NO
/// work before the settle fails; while it is computing the wait continues, up to ten times that as a
/// backstop against a worker that builds forever and never the wanted window. See fixtures.cpp.
bool settle(TerrainMap& map, const TerrainWindow& wanted, int milliseconds = 20000);

/// The same, but waiting for the REFINED map rather than for the first one to fill every row.
///
/// A wide window is built twice — a one-sample pass so a hand sees something at today's latency, then
/// the schedule's full mean — and `settle` above returns at the earlier of the two. A case about the
/// AVERAGE therefore has to say which of the two maps it means, or it races the builder and compares
/// a refined reference against a rough picture. That is what the snapshot carries `samples` for.
bool settleRefined(TerrainMap& map, const TerrainWindow& wanted, int milliseconds = 60000);

TerrainWindow windowAt(double travelYCentre, double travelYSpan, double travelXSpan);


struct ExploreFixture {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	NotifyTray tray;
	SoundSelection selection;
	EditHistory history;
	/// **THE PANEL STATE A CASE ARMS, OWNED HERE AND DECLARED ABOVE `stage` ON PURPOSE.** The stage's
	/// knobs put themselves on a `ModState`'s watcher list and unwatch in their own destructors, so the
	/// state has to OUTLIVE the stage — and members die in reverse order, so "above" is what says so.
	///
	/// IT IS A MEMBER SO NO CASE CAN GET THE ORDER WRONG. A case that declares its own `ModState` after
	/// the fixture passes every assertion and then unwatches into freed memory at teardown — silent in
	/// `ctest`, where `~FrameWatchers`'s assert is compiled out, and caught only by the sanitizer gate.
	/// THREE instances in two days across two sessions (2026-09-20/21); a peer's rail fixture took this
	/// same shape for the same reason. Not handed to the stage by default — a case that wants
	/// modulation says `stage.setModulation(&mod, &history)`, so every other case is unchanged.
	ui::ModState mod;
	TerrainMap terrain;
	int64_t now = 1000;
	ExploreStage stage { bridge, registry, selection, history, terrain, &tray, [this] { return now; } };
	ParamSet params;

	ExploreFixture() {
		stage.setBounds(0.0f, 0.0f, 866.0f, 534.0f);
		bridge.refresh();
	}

	void set(ParamId id, double value) {
		params.setBase(static_cast<clap_id>(id), value);
		bridge.publish(params);
		bridge.refresh();
	}

	/// Drain what the editor posted back into the engine, the way the plugin's own binding does, so a
	/// gesture that commits is visible to the next read.
	void settleGesture() {
		bridge.drain([&](const UiParamEvent& event) {
			if (event.kind == UiParamEvent::Kind::Value) {
				params.setBase(event.id, event.value);
			}
		});
		bridge.publish(params);
		bridge.refresh();
	}

	/// LET A RELEASED DRAG COME TO A STOP. Since the drag-rate cap (2026-09-17) the star is still
	/// travelling when the button comes up, and the gesture's own consequences — the crumb, the undo
	/// entry, the recentre — are planted when it ARRIVES. A case that asserts any of those has to give
	/// the world the frames it needs, which a fixture with a frozen clock does not do by itself.
	///
	/// Returns how many frames it took, so a caller can tell "settled immediately" (a click, which is
	/// not capped) from "coasted" — and so a bound that was never reached shows up as the cap rather
	/// than being silently absorbed.
	/// **AND SINCE backlog GE IT ALSO WAITS FOR THE VIEW.** The recentre named above is no longer
	/// instant: a click and a jog both PAN the view onto the sound at `kViewsPerSecond` rather than
	/// teleporting onto it, so "the gesture has finished" now means the star has arrived AND the
	/// window has caught up with it. A case that waited only for the star would read the view part way
	/// through its journey and call it a wrong destination.
	int settleScrub(int limit = 400) {
		int frames = 0;
		while ((stage.settlingScrubForTest() || stage.viewGlidingForTest()) && frames < limit) {
			now += 16;
			stage.update();
			settleGesture();
			++frames;
		}
		return frames;
	}

	double read(ParamId id) const { return bridge.value(static_cast<clap_id>(id)); }
};


/// A host that answers the editor and records nothing else. The routing case below does not care what
/// the host does with a flush; it cares where a click lands.
struct QuietGuiHost : GuiHost {
	int flushes = 0;
	int resizes = 0;
	void guiRequestFlush() noexcept override { ++flushes; }
	void guiRequestResize(uint32_t, uint32_t) noexcept override { ++resizes; }
	// No plugin state behind the test host, so the preset browser's capture/load are stubs.
	void guiCaptureState(clap_ostream_t*) noexcept override {}
	bool guiLoadState(clap_istream_t*) noexcept override { return false; }
};

/// A PRESET LIBRARY OF ONE CASE'S OWN, for the whole of that case.
///
/// PRESETS ARE THE FIRST THING IN THIS EDITOR THAT PERSISTS ACROSS AN EDITOR, which makes them the
/// first thing that can leak from one case into the next. main() points XDG_DATA_HOME at one
/// temporary directory for the run, and that is right for isolating the suite from the developer's
/// real library and wrong for isolating the cases from each other: the moment a case saved a preset,
/// every later case that opened an editor inherited it. It cost a REQUIRE in the case above, which
/// asserted a bank of exactly one and got two — and it would have cost a much stranger failure the
/// first time the order changed.
///
/// So a case that touches the bank takes its own directory and puts the old one back.
/// **SAVE THE WAY A HAND DOES** — the menu's "Save as…", which opens the name prompt, then Enter on
/// a NEW name ("User N" — the prompt itself pre-fills the preset last saved or loaded, backlog PP).
/// The one road every case that wants "a saved preset" takes, so a save that skipped the prompt would not be a road any case could reach.
inline void saveThroughPrompt(feathers::ui::PresetBrowser& browser) {
	browser.chooseFromMenu(feathers::ui::PresetBrowser::kMenuSave);
	REQUIRE(browser.prompt() == feathers::ui::PresetBrowser::Prompt::Naming);
	browser.typeForTest(browser.suggestedName());
	browser.typeTagsForTest({});
	browser.submitPrompt();
	REQUIRE(browser.prompt() == feathers::ui::PresetBrowser::Prompt::Closed);
}

class ScopedPresetLibrary {
public:
	explicit ScopedPresetLibrary(const char* label) {
		const char* existing = std::getenv("FEATHERS_PRESET_ROOT");
		previous_ = existing != nullptr ? existing : "";
		path_ = std::filesystem::temp_directory_path()
		        / ("feathers-case-" + std::string(label) + "-"
		           + std::to_string(static_cast<long>(::getpid())));
		std::error_code code;
		std::filesystem::remove_all(path_, code);
		std::filesystem::create_directories(path_, code);
		// THE ROOT, NOT XDG DATA — the library moved to ~/Documents/Owlet/Feathers/Presets (backlog FO
		// phase 2) and this variable is what redirects it.
		::setenv("FEATHERS_PRESET_ROOT", path_.c_str(), 1);
	}
	~ScopedPresetLibrary() {
		// NEVER UNSET: with no root variable the store falls back to the real ~/Documents. main() always
		// sets one, so `previous_` is empty only for a binary that forgot — which gets a scratch path.
		const std::string back = previous_.empty() ? (std::filesystem::temp_directory_path() / "feathers-case-fallback").string()
		                                           : previous_;
		::setenv("FEATHERS_PRESET_ROOT", back.c_str(), 1);
		std::error_code code;
		std::filesystem::remove_all(path_, code);
	}
	ScopedPresetLibrary(const ScopedPresetLibrary&) = delete;
	ScopedPresetLibrary& operator=(const ScopedPresetLibrary&) = delete;
	[[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
	std::filesystem::path path_;
	std::string previous_;
};

/// A HOST WITH REAL STATE BEHIND IT, which QuietGuiHost above is not — its capture and load are empty
/// bodies, and that is exactly why the preset browser's whole reason for existing was never once
/// exercised. A stub host cannot fail the way a real one fails, and this feature shipped unable to
/// save or recall anything.
///
/// It carries a ParamSet and walks it the way the CLAP does: parameters, in the one order both
/// directions read. Loading also PUBLISHES, because that is what the real plugin's load does by
/// asking the host to rescan and flush — without it the editor would still be showing the patch that
/// was replaced, and a case would be pinning serialization while claiming to pin recall.
struct StatefulGuiHost : GuiHost {
	feathers::ParamSet params;
	feathers::ParamBridge* bridge = nullptr;
	int flushes = 0;
	/// Made to refuse, so a case can ask what the editor does when a preset will not load.
	bool refuseLoads = false;
	/// **A TAGGED BLOCK AFTER THE PARAMETERS, OPT-IN** — the stand-in for the CLAP's state blocks (the
	/// chains, the rack, the sounds), which ride the same stream after the parameters. A case that claims
	/// "the WHOLE patch" sets this, so the claim is about more than the parameter table. Length-prefixed.
	bool withBlock = false;
	std::vector<uint8_t> block;

	void guiRequestFlush() noexcept override { ++flushes; }
	void guiRequestResize(uint32_t, uint32_t) noexcept override {}

	void guiCaptureState(clap_ostream_t* stream) noexcept override {
		if (stream != nullptr) {
			params.save(stream);
			if (withBlock) {
				const auto size = static_cast<uint32_t>(block.size());
				stream->write(stream, &size, sizeof(size));
				if (size > 0) {
					stream->write(stream, block.data(), size);
				}
			}
		}
	}
	bool guiLoadState(clap_istream_t* stream) noexcept override {
		if (refuseLoads) {
			return false;
		}
		if (stream == nullptr || !params.load(stream)) {
			return false;
		}
		if (withBlock) {
			uint32_t size = 0;
			if (stream->read(stream, &size, sizeof(size)) != static_cast<int64_t>(sizeof(size))) {
				return false;
			}
			std::vector<uint8_t> read(size);
			if (size > 0 && stream->read(stream, read.data(), size) != static_cast<int64_t>(size)) {
				return false;
			}
			block = std::move(read);
		}
		if (bridge != nullptr) {
			bridge->publish(params);
		}
		return true;
	}
	/// What the engine would do with a value the editor moved, and then hand back.
	void adopt(feathers::ParamId id, double value) {
		params.setBase(static_cast<clap_id>(id), value);
		if (bridge != nullptr) {
			bridge->publish(params);
		}
	}
};

/// The centre of a frame, in the coordinates the top frame receives events in.
visage::Point centreInRoot(visage::Frame* frame);

/// Whether `frame` is `ancestor` or lives inside it — a click may legitimately land on a child of the
/// control that watches the parameter, which is still the control being reached.
bool isWithin(visage::Frame* frame, visage::Frame* ancestor);


/// The first VISIBLE frame anywhere under `root` that is not entirely inside it — a control the
/// window has no pixels for. Visible only, because a panel that has yielded still has bounds and a
/// frame nobody draws is not a cropped control; and the first rather than a count, because the name
/// of one is what a failure needs to be actionable.
///
/// A whole pixel of slack: a physical-to-logical round trip at a fractional scale lands a hair off,
/// and this is a question about controls being off the canvas, not about rounding.
visage::Frame* firstFrameOutside(visage::Frame* root, visage::Frame* frame);


/// A press, a drag and a release delivered the way visage's own window delivers them: the down-frame
/// is found by hit test, the event position is the window position minus that frame's position in the
/// window, and the SAME frame receives the drag and the release even if the pointer has moved off it
/// (WindowEventHandler::handleMouseDown/handleMouseMove/handleMouseUp). Replicated rather than
/// approximated — a venue that routes events its own way tests its own way of routing them.
/// **A DOUBLE-CLICK ON A CARD HEAD, WHICH IS THE FOLD GESTURE.**
///
/// Two presses, count 1 then count 2 — what a real double-click delivers. `FxCard::foldOnHeadPress`
/// acts only on the SECOND, so a case driving ONE press asserts the ABSENCE of a fold and a case
/// driving this asserts its presence.
///
/// **CALLING `dragThroughRoot` TWICE IS NOT THIS.** That sends two count-1 presses, which folds
/// nothing — a different gesture wearing the same shape.
///
/// THE TARGET IS RE-RESOLVED FOR THE SECOND PRESS, because the first may have folded the card and a
/// folded card is a different size: `frameAtPoint` can answer something else by then.
void doubleClickHead(visage::Frame* root, visage::Point where);

void dragThroughRoot(visage::Frame* root, visage::Point from, visage::Point to);


/// WHERE THE KEYBOARD FOCUS LANDS WHEN THE HAND PRESSES AT `where` — the walk
/// `WindowEventHandler::handleMouseDown` makes, replicated for the same reason `dragThroughRoot`
/// replicates the mouse road: a venue that routes events its own way tests its own way of routing
/// them. The hit frame, then up through PARENTS to the first that `acceptsKeystrokes()`, and
/// `nullptr` if there is none — which is not a detail, it is the whole answer to "does a key reach
/// anything at all after this press".
visage::Frame* keyboardFocusAfterPress(visage::Frame* root, visage::Point where);

/// THE KEY, FROM THE FOCUSED FRAME UPWARD — `WindowEventHandler::handleKeyDown`, replicated. The
/// focused frame is offered it whether or not it accepts keystrokes; after that only ancestors that
/// do are visited, and the first `true` stops the walk. Returns whether anything consumed it.
///
/// A SIBLING IS NEVER VISITED, and that is the fact these two helpers exist to make testable. A
/// handler installed on a frame that is neither the focus nor one of its ancestors is a handler that
/// cannot fire, and calling its callback directly proves the handler works while proving nothing
/// about whether a keystroke can ever reach it.
bool keyFromFocus(visage::Frame* focused, const visage::KeyEvent& e);
/// The RELEASE, by the same walk — `WindowEventHandler::handleKeyUp`, replicated. visage's X11 layer
/// sends a release to the host exactly when this is false, as it does a press.
bool keyUpFromFocus(visage::Frame* focused, const visage::KeyEvent& e);

/// **THE KEYBOARD CLAIM, COUNTED** — installed with `ui::ScopedKeyboardClaimBackend`, it stands in for
/// the X focus grab so a case can assert every take is paired with a give-back on every exit. Tokens
/// are never 0, so every take is one the claim must give back.
struct ClaimCounter final : ui::KeyboardClaimBackend {
	int takes = 0;
	int gives = 0;
	uint64_t take(const visage::Frame&) override { return static_cast<uint64_t>(++takes); }
	void giveBack(uint64_t) override { ++gives; }
	[[nodiscard]] int outstanding() const { return takes - gives; }
};


/// The first frame of type T anywhere under `root`. The panel is not on Gui's public surface — it has
/// no reason to be — so a case that wants the REAL one goes and finds it, the way the windowed probe
/// does.
/// THE PANEL A LAYER HOLDS, AS ITS OWN TYPE. Since 2026-09-13 every present goes through
/// `SurfaceLayers`, which mounts the view in a `MountedFrame` (the tick and the scroll live there), so
/// the layer's content is the mount and the device's panel is one step inside it. A case that wants
/// to read HARM's scope or the Shaper's plots asks here rather than casting the mount.
template <typename T>
T* presentedAs(const ui::ExpandedOverlay* overlay) {
	if (overlay == nullptr) {
		return nullptr;
	}
	auto* content = const_cast<ui::ExpandedView*>(overlay->contentForTest());
	if (auto* mounted = dynamic_cast<ui::MountedFrame*>(content); mounted != nullptr) {
		return dynamic_cast<T*>(mounted->contentForTest());
	}
	return dynamic_cast<T*>(content);
}

template <typename T>
T* findInTree(visage::Frame* root) {
	if (auto* found = dynamic_cast<T*>(root)) {
		return found;
	}
	for (visage::Frame* child : root->children()) {
		if (T* found = findInTree<T>(child)) {
			return found;
		}
	}
	return nullptr;
}

/// Every knob in the assembled editor, wherever it was placed.
void collectKnobs(visage::Frame* root, std::vector<ui::Knob*>* out);

/// A DESCRIPTOR'S NAME FOR A CAPTURE, WITH THE NULL GUARDED — `device::SourceDesc::name` is a
/// `const char*` with NO default initializer, and production guards it in three places
/// (`ModPanel::outputName`, `FxLane::ensureOutputArm`, `chipWidthsOf`). A case that walks the whole
/// registry to report a failure should not itself be the thing that crashes on a device someone
/// added without one — the run would die inside the assertion that was about to name the defect.
inline std::string nameOrUnnamed(const char* name) {
	return name != nullptr ? std::string(name) : std::string("(unnamed)");
}

struct ModFixture {
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	/// ONE HOST FOR BOTH CHAINS — the strip's cards and the lane's address their devices by id, so there
	/// is no second slot space for a second host to keep apart (kill-lanes step 2).
	feathers::BridgeDeviceHost deviceHost { bridge };
	Gui gui { host, bridge };
	visage::Frame* root = nullptr;
	ui::ModPanel* panel = nullptr;
	/// PHYSICAL PIXELS PER LOGICAL ONE on whatever display this is running against. setSize is told
	/// physical pixels and the layout is written in logical ones, so a case that wants a canvas of a
	/// stated size has to multiply — and a case that forgets is asking about a different editor than it
	/// thinks. Measured from the editor rather than assumed to be 1.
	float scale = 1.0f;

	ModFixture() {
		// A FIXTURE THAT ROUTES NOTHING. A fresh bridge carries the default patch's one route — the
		// strip's envelope into Level (backlog EF) — and every case on this fixture reasons from "no
		// route" (`routeList().count() == 0`, every end a core end) about the route ITS hand makes. Taking
		// the default route away here keeps those claims about the gesture rather than about the patch;
		// the bridge suite is where the default route itself is pinned. Amount zero IS removal.
		bridge.setRouteAmount(backplane::Source::output(feathers::kDefaultRackInstance, 0),
		                      backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::VoiceLevel))),
		                      0.0f);
		(void)bridge.takeStateDirty(); // the removal is the fixture's, not a hand's
		bridge.refresh();
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		// EXPANDED VIEWS STAY IN THE EDITOR FOR THESE CASES. The popout preference defaults ON, and an
		// expanded view opened in its own OS window inside this 475-case process takes the whole
		// process down with it — MEASURED: 392 of 475 ran, 83 never executed, and ctest reported one
		// failure rather than eighty-four. The windowed road has its own binary (`gui-windowed`),
		// which is where it is asked about; here the question is always about the in-editor overlay.
		gui.setPopoutWindowForTest(false);
		REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));
		root = gui.root();
		REQUIRE(root != nullptr);
		REQUIRE(root->width() > 0.0f);
		scale = static_cast<float>(Gui::kLogicalWidth) / root->width();
		panel = findInTree<ui::ModPanel>(root);
		REQUIRE(panel != nullptr);
		// Every case below wants the canvas the layout was written for.
		REQUIRE(gui.setSize(static_cast<uint32_t>(Gui::kLogicalWidth * scale),
		                    static_cast<uint32_t>(Gui::kLogicalHeight * scale)));
	}

	/// WHERE THE ARROW IS, in the coordinates the top frame receives events in. Exposed so a case can
	/// ask what a press THERE does to the keyboard focus — the same point `arm` presses, from one
	/// place, because two spellings of it would drift and the case would be about a point nobody
	/// clicks.
	/// A SOURCE IS AN ENDPOINT (kill-lanes step 2): a macro is `Source::core(i)`, a modulator's output
	/// `Source::output(bridge.rackDeviceAt(slot), out)`.
	/// THE HOST GRANTS THE ROOM THE EDITOR ASKED FOR. Since backlog EJ the window's size does not feed
	/// the scale, so a canvas opened in a fixture window sized for the folded editor sits BELOW the
	/// window's bottom rather than shrinking everything to fit — which is what a real host sees until
	/// it answers `request_resize`. A case whose gesture lands on the canvas calls this after opening it,
	/// exactly as a host that honours the ask would have resized.
	void grantPreferredSize() {
		// ON A SCREEN THAT HOLDS IT: the floor is held to the usable screen since backlog LX (2026-10-10), and the
		// private display's (972 rows) is shorter than the editor with its canvas open. What these cases need is the
		// room granted, so the screen is stubbed for the grant and put back.
		ui::PluginWindow::setUsableScreenForTest(8192, 8192);
		uint32_t w = 1;
		uint32_t h = 1;
		REQUIRE(gui.adjustSize(&w, &h));
		REQUIRE(gui.setSize(w, h));
		ui::PluginWindow::setUsableScreenForTest(0, 0);
	}

	visage::Point armPoint(backplane::Source source) const {
		return panel->positionInWindow()
		       + visage::Point { panel->arrowBounds(source).x() + 8.0f,
			                     panel->arrowBounds(source).y() + 8.0f };
	}

	/// Click the real arrow of a real card, through the real tree.
	void arm(backplane::Source source) {
		const visage::Point at = armPoint(source);
		dragThroughRoot(root, at, at);
	}

	ui::Knob* knobFor(ParamId id) {
		std::vector<ui::Knob*> knobs;
		collectKnobs(root, &knobs);
		for (ui::Knob* knob : knobs) {
			const std::span<visage::Frame* const> watchers =
			    gui.widgets()->watchers(static_cast<clap_id>(id));
			for (visage::Frame* watcher : watchers) {
				if (watcher == knob && knob->width() > 0.0f) {
					return knob;
				}
			}
		}
		return nullptr;
	}

	/// The centre of a knob's FACE in window coordinates — the face, not the column, because the label
	/// and readout under it are part of the frame and a drag from the middle of the column would start
	/// below the dial.
	visage::Point faceCentre(ui::Knob* knob) const {
		return knob->positionInWindow()
		       + visage::Point { knob->width() * 0.5f, knob->faceDiameter() * 0.5f };
	}
};


/// The filter card in the assembled editor, at the place the layout actually puts it — the same
/// reason ModFixture exists rather than a bare widget: a card can pass every test it has and still
/// be unreachable where the lane docks it.
struct FilterFixture : ModFixture {
	ui::FxLane* lane = nullptr;
	ui::FilterCard* card = nullptr;

	FilterFixture() {
		lane = findInTree<ui::FxLane>(root);
		REQUIRE(lane != nullptr);
		card = lane->filterCard();
		REQUIRE(card != nullptr);
	}

	void press(visage::Point p) { card->onMouseDown().callback(at(p.x, p.y)); }

	/// **THE FOLD GESTURE: count 1 then count 2.** `FxCard::foldOnHeadPress` acts only on the SECOND,
	/// so one press on a head asserts the ABSENCE of a fold — which is what stops a double-click
	/// toggling twice and landing where it started.
	void foldPress(visage::Point p) {
		card->onMouseDown().callback(at(p.x, p.y));
		card->onMouseDown().callback(doubleAt(p.x, p.y));
		// **AND THE RELEASE, WHICH IS WHERE THE FOLD NOW HAPPENS.** Armed and never moved is the
		// collapse; a helper that sent presses alone exercised the arming and never the verb.
		card->onMouseUp().callback(doubleAt(p.x, p.y));
	}
	visage::Point centreOf(visage::Bounds b) const {
		return {b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f};
	}
	/// THE FILTER'S SETTINGS WHERE THEY NOW LIVE — its row in the fx chain, not the host's parameter
	/// table. Every assertion in this file about what the card did reads through here.
	fx::filter::State filter() const { return fxFilterAt(bridge.fxChain(), 0); }

	/// ONE CONTROL'S VALUE, ASKED OF THE DEVICE. There is nothing else left to ask: the filter's clap
	/// ids are gone, so `fx::filter::Param` is the only name a control has.
	///
	/// The shim that stood here mapped a ParamId back to a control index while both existed, and it
	/// said it would die with the ids. It has.
	double param(fx::filter::Param p) const { return fx::filter::valueOf(filter(), p); }

	/// Put one control where a case wants it, the way the card itself would — one write, to the row
	/// the card reads and the engine now reads too.
	void set(fx::filter::Param p, double value) {
		fx::filter::State state = filter();
		fx::filter::setValueOf(&state, p, static_cast<float>(value));
		bridge.setFilterSettings(0, state);
	}

	/// A click delivered the way a WINDOW delivers one — through the root, at the real place the part
	/// sits. `press` above invokes the handler directly, which says nothing about whether a hand could
	/// reach the control; see the routed case at the end of this file for what that gap cost once.
	void click(visage::Bounds part) { clickAt(card->positionInWindow() + centreOf(part)); }
	void clickAt(visage::Point window) { dragThroughRoot(root, window, window); }

	/// Drain the editor's ring into a patch and publish it, which is what the host does between frames.
	/// A value read back without this is the one from BEFORE the click, which is exactly the reading
	/// that made a working card look dead in a venue that never drained.
	void settle() {
		bridge.drain([&](const UiParamEvent& event) {
			if (event.kind == UiParamEvent::Kind::Value) {
				params.setBase(event.id, event.value);
			}
		});
		bridge.publish(params);
		bridge.refresh();
	}

	ParamSet params;
};


struct AutomodPanelFixture {
	FilterFixture f;
	size_t slot = 0;
	/// THE AUTOMOD'S ID — what the panel is built over; `slot` is only where the lane draws it.
	DeviceInstanceId device = kNoInstance;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::AutomodExpanded* panel = nullptr;

	AutomodPanelFixture() {
		slot = f.bridge.addFxDevice(feathers::fx::automod::kTraits.tag);
		device = f.bridge.fxDeviceAt(slot);
		f.lane->resized();
		view = ui::makeExpandedView(
		    f.deviceHost, &f.bridge, registry, device, feathers::fx::automod::kDescriptor);
		panel = dynamic_cast<ui::AutomodExpanded*>(view.get());
		if (panel != nullptr) {
			const visage::Point min = panel->minimumSize();
			panel->setBounds(0.0f, 0.0f, min.x, min.y);
			panel->resized();
		}
	}
};


} // namespace gui_widgets_tests

using namespace gui_widgets_tests;
