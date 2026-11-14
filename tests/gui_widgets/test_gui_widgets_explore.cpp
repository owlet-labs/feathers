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

// THE EXPLORE STAGE. Lines 5820–11732 of the one file, moved verbatim 2026-09-03.

#include "fixtures.h"

#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include "dsp/ks_picture.h"
#include "plugin/ui/expanded_canvas.h"
#include "plugin/ui/likes_log.h"
#include "plugin/ui/param_control.h"
#include "plugin/ui/mode_value_cell.h"
#include "dsp/drum_picture.h"
#include "plugin/character_params.h"
#include "plugin/ui/drum_panel.h"
#include "plugin/device_registry.h"
#include "plugin/synth.h"
#include "rack/rack_bridge.h"
#include "plugin/ui/spectrum_panel.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/travel_control.h"
#include "../param_routes.h"

//---------------//
// Explore stage //
//---------------//

// The surface is the terrain plus the verbs, and the verbs are what is tested here: what a scrub
// writes, what it must NOT write, what undo walks, what a breadcrumb records, and what the two
// unbuilt corners answer. The picture is checked by photographing it, which no assertion below
// pretends to do.


TEST_CASE("a scrub moves the selected sound's Y travel to where the pointer is") {
	// **THE VERTICAL AXIS IS THE DECODED PLANE'S Y** (backlog GY). It was the sound's HOME, and the
	// whole of this case's change is the probe following the axis: what a press means has not moved by
	// a pixel, and the round trip through the bubble that it really tests is untouched.
	ExploreFixture f;
	// Sound B, which is born mid-axis rather than at the floor of the space.
	f.selection.select(1);
	const float centreY = f.stage.disk().centre().y;
	const double before = f.stage.selectedDriftY();

	const float target = centreY - 60.0f;
	f.stage.onMouseDown().callback(at(f.stage.disk().centre().x, target));
	f.settleGesture();

	// The Y travel follows the pointer EXACTLY. The old tolerance was half a zone step, which a
	// rounded zone needed and a continuous coordinate does not — so this got STRICTER rather than
	// looser, which is the direction a rewritten expectation should move if the rewrite is honest.
	//
	// THROUGH THE BUBBLE, AND THE REFERENCE IS RESTATED RATHER THAN ASKED FOR. Since the chart is drawn
	// as a sphere's near face, the place under a pixel is not `travelYAtScreenY` of that pixel — it is `travelYAtScreenY`
	// of the SHEET point the pixel is looking at. The flattening is done here by `Bubble` directly, not
	// by the stage's own `flatAt`, so this is the law restated beside the code rather than the code
	// asked whether it agrees with itself.
	const visage::Point flat =
	    Bubble::toFlat(f.stage.disk(), { f.stage.disk().centre().x, target });
	CHECK(f.stage.selectedDriftY() == doctest::Approx(f.stage.travelYAtScreenY(flat.y)));
	// AND IT ACTUALLY MOVED, so the equality above is not two untouched zeros agreeing — which is
	// exactly how this case failed when the axis changed under it.
	CHECK(f.stage.selectedDriftY() != doctest::Approx(before));
}

TEST_CASE("a vertical scrub moves the plane's Y, spends no parameter, and leaves gamma alone") {
	// RESTATED, NOT RELAXED, under the one-value design — and every defect the original guarded is
	// still guarded here. What it was for: a scrub writes IDENTITY, it must not reach the performance
	// half, and whatever gesture it opens it must close. What changed underneath it is which ROAD
	// identity travels on. The chart used to decompose the pointer's home into Zone and Offset and
	// commit both; it now moves the unbounded gamma base and leaves those two as bounded host dials
	// summing on top, the way the reference does it (voice.cpp:1688).
	//
	// So the two parameters the old case watched being written are now asserted to HOLD ABSOLUTELY
	// STILL, which is the design itself stated as a test, and the honesty clause about Pos X / Path /
	// Weather is carried over word for word. This case is now the vertical TWIN of the horizontal one
	// below: both axes of this chart are patch state, both bracket their own writes, and neither spends
	// a parameter.
	//
	// **AND THE LIST OF THINGS HELD STILL GREW BY ONE** (backlog GY). The vertical axis was the gamma
	// base; it is now a displacement in the decoded plane, gamma having become something the decode
	// CONSUMES rather than an axis you can point at. So the base joins Zone, Offset and the vernier in
	// the must-not-move column, and the moving number is the sound's Y travel. Every defect the
	// original guarded is still guarded; one more is guarded now.
	ExploreFixture f;
	f.selection.select(1);
	const float x = f.stage.disk().centre().x;
	const double zoneBefore = f.read(ParamId::BZone);
	const double offsetBefore = f.read(ParamId::BOffset);
	const double gammaBefore = f.read(ParamId::BGamma);
	const double baseBefore = f.stage.selectedGammaBase();
	const double yBefore = f.stage.selectedDriftY();

	f.stage.onMouseDown().callback(at(x, f.stage.disk().centre().y - 40.0f));
	f.stage.onMouseDrag().callback(at(x, f.stage.disk().centre().y - 80.0f));
	f.stage.onMouseUp().callback(at(x, f.stage.disk().centre().y - 80.0f));

	// THE DRAG DID SOMETHING, or everything below is vacuous.
	INFO("Y travel " << yBefore << " -> " << f.stage.selectedDriftY());
	REQUIRE(f.stage.selectedDriftY() != doctest::Approx(yBefore));
	// AND IT WAS NOT THE BASE. The number this case used to watch a drag write is now the number it
	// watches a drag leave alone — the design, stated where it can fail.
	CHECK(f.stage.selectedGammaBase() == doctest::Approx(baseBefore));

	// AND IT SPENT NO PARAMETER AT ALL. The base is state, like drift, so the parameter queue stays
	// empty — including the two dials that used to carry this gesture, which a host may now automate
	// without the chart ever fighting it for them.
	CHECK(drain(f.bridge).empty());
	CHECK(f.read(ParamId::BZone) == doctest::Approx(zoneBefore));
	CHECK(f.read(ParamId::BOffset) == doctest::Approx(offsetBefore));
	CHECK(f.read(ParamId::BGamma) == doctest::Approx(gammaBefore));
	// The honesty clause, unchanged: the performance half belongs to Perform, and a second way to write
	// it from here would be two controls for one value.
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::PosX)) == doctest::Approx(0.0));
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::PosY)) == doctest::Approx(0.0));
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::Path)) == doctest::Approx(0.0));
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::Weather)) == doctest::Approx(0.0));

	// And the edit is announced, or a moved sound is a sound that silently fails to save. This is the
	// bracket's only observable consequence on this side of the bridge — the same honest limit the
	// horizontal case records about its own.
	CHECK(f.bridge.takeStateDirty());
}

TEST_CASE("a horizontal drag drifts the sound, and spends no parameter doing it") {
	ExploreFixture f;
	f.selection.select(1);
	const visage::Point centre = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	drain(f.bridge);
	REQUIRE(f.bridge.soundTravel().offset[1].x == doctest::Approx(0.0));

	// Straight across, on the same row: home does not move, so the only thing that may change is the
	// sound's own drift.
	//
	// THE CLOCK MOVES BECAUSE THE HAND DOES. The drag rate is capped per SECOND (`kViewsPerSecond`),
	// so a pointer that teleports 120 px while the fixture's clock stands still is a hand travelling
	// at infinite speed, and the cap correctly refuses to follow it. A quarter-second is the cap's own
	// window, which is enough room for 120 px of drift and is what a real hand would have spent.
	f.now += 250;
	f.stage.onMouseDrag().callback(at(centre.x + 120.0f, centre.y));
	// The sheet point under the pointer, restated through the bubble's own law — see the scrub case at
	// the top of this file for why the reference is computed here rather than asked of the stage.
	const visage::Point flat = Bubble::toFlat(f.stage.disk(), { centre.x + 120.0f, centre.y });
	CHECK(f.bridge.soundTravel().offset[1].x == doctest::Approx(f.stage.travelXAtScreenX(flat.x)));
	// DRIFT IS STATE, NOT A PARAMETER — sound_travel.h — so nothing reaches the parameter queue, and in
	// particular Pos X and Pos Y are untouched: they say where you are STANDING, and a scrub must not
	// quietly move the listener to reposition the sound.
	CHECK(drain(f.bridge).empty());
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::PosX)) == doctest::Approx(0.0));

	// The drag makes the patch dirty, so the host knows to ask for the state again.
	//
	// NOT A CONTROL FOR THE BRACKET, and it was written as one before the known-bad build said
	// otherwise: `setSoundTravel` already sets the flag, so deleting `endSoundTravelGesture` entirely
	// leaves this green. The bracket's own job — telling the host a run of writes is one edit — has
	// nothing on this side of the bridge to observe it, so this asserts only what it can see.
	f.stage.onMouseUp().callback(at(centre.x + 120.0f, centre.y));
	CHECK(f.bridge.takeStateDirty());
}

TEST_CASE("a scrub commits exactly the drift the chart is showing, at any Weather") {
	// THE DECISION: Weather does not move anything on the Explore chart. The chart is the
	// IDENTITY surface, so the star sits at the sound's own drift and a horizontal drag is 1:1 with
	// what is drawn — there is no performance term to subtract out, and therefore no arithmetic by
	// which one could reach the patch.
	ExploreFixture f;
	f.selection.select(1);
	// Stand well off the anchor. Under the shipped placement this term slid the star sideways and the
	// drag had to compensate for it; it must now change neither.
	f.set(ParamId::Weather, 1.0);
	f.set(ParamId::PosX, 0.3);
	const double performed = f.stage.selectedTravel() - f.stage.selectedDrift();
	REQUIRE(performed != doctest::Approx(0.0));

	const visage::Point centre = f.stage.disk().centre();
	const float target = centre.x + 40.0f;
	f.stage.onMouseDown().callback(at(target, centre.y));
	// The stored drift IS the pointer's travel, with nothing taken off it, and the star is drawn back
	// under the pointer that asked for it.
	//
	// AND THE ROUND TRIP IS THE SECOND HALF OF THE CLAIM, which the bubble makes worth more than it
	// was: the pointer is flattened onto the sheet, the drift is stored in the sheet's own units, and
	// the star is drawn by projecting that back — so "the star is under the pointer that asked for it"
	// now passes only if the two directions of the map are actually inverse.
	const visage::Point flat = Bubble::toFlat(f.stage.disk(), { target, centre.y });
	CHECK(f.stage.selectedDrift() == doctest::Approx(f.stage.travelXAtScreenX(flat.x)));
	CHECK(f.stage.glassAt(f.stage.selectedDriftY(), f.stage.selectedDrift()).x
	      == doctest::Approx(target));
	// And the performance half is exactly where the performer left it — the property the old control
	// asserted, kept, because it is the one a drag could still break.
	CHECK(f.stage.selectedTravel() - f.stage.selectedDrift() == doctest::Approx(performed));
}

TEST_CASE("Weather and the puck move the engine's travel and not the star") {
	// THE HEADLINE OF THE DECISION, asserted where it can actually be seen: the ink the star leaves.
	// `selectedTravel()` still reports where the ENGINE has this sound — drift plus
	// Weather x (puck - anchor) — and the surface still reads it. What changed is that the chart no
	// longer draws with it, because where you are standing while you listen belongs to Perform.
	ExploreFixture f;
	f.selection.select(1);
	f.stage.driftTo(0.004);
	f.settleGesture();
	REQUIRE(f.stage.selectedDrift() == doctest::Approx(0.004));

	visage::Canvas canvas;
	f.stage.draw(canvas);
	const ExploreStage::InkBox before = f.stage.overlayInk();
	REQUIRE(before.any);
	const double travelBefore = f.stage.selectedTravel();

	f.set(ParamId::Weather, 1.0);
	f.set(ParamId::PosX, 0.4);
	// The engine really did move the sound, or this case would pass against a surface that had simply
	// stopped reading anything.
	REQUIRE(f.stage.selectedTravel() != doctest::Approx(travelBefore));

	f.stage.draw(canvas);
	const ExploreStage::InkBox after = f.stage.overlayInk();
	CHECK(after.left == doctest::Approx(before.left));
	CHECK(after.right == doctest::Approx(before.right));
	CHECK(after.top == doctest::Approx(before.top));
	CHECK(after.bottom == doctest::Approx(before.bottom));
	// And the sound's identity is untouched by all of it.
	CHECK(f.stage.selectedDrift() == doctest::Approx(0.004));
}

TEST_CASE("Explore does not even repaint for the performance controls") {
	// THE MECHANICAL HALF of the same decision. Nothing this surface draws is a function of Weather,
	// Path, Pos X/Y or anybody's placement any more, so a host automating them must not reach it at
	// all: a redraw per automation frame that produces an identical picture is exactly the text churn
	// this project has already traced corruption to, and it was the shape of the chaotic behaviour seen.
	ExploreFixture f;
	const auto watchedByStage = [&](clap_id id) {
		for (const visage::Frame* frame : f.registry.watchers(id)) {
			if (frame == &f.stage) {
				return true;
			}
		}
		return false;
	};
	// What it does watch, so the check below is a statement about this surface rather than about an
	// empty registry.
	CHECK(watchedByStage(static_cast<clap_id>(ParamId::OscType)));
	CHECK(watchedByStage(static_cast<clap_id>(ParamId::GlobalGamma)));
	CHECK(watchedByStage(static_cast<clap_id>(soundParam(1, SoundField::Zone))));
	// And what it must not.
	CHECK_FALSE(watchedByStage(static_cast<clap_id>(ParamId::Weather)));
	CHECK_FALSE(watchedByStage(static_cast<clap_id>(ParamId::Path)));
	CHECK_FALSE(watchedByStage(static_cast<clap_id>(ParamId::PosX)));
	CHECK_FALSE(watchedByStage(static_cast<clap_id>(ParamId::PosY)));
	CHECK_FALSE(watchedByStage(static_cast<clap_id>(soundParam(1, SoundField::PlaceX))));
	CHECK_FALSE(watchedByStage(static_cast<clap_id>(soundParam(1, SoundField::PlaceY))));
}

TEST_CASE("a scrub stops at the end of the space rather than posting past it") {
	ExploreFixture f;
	// Sound A is born at zone 0 with no offset, which IS the floor of home.
	f.selection.select(0);
	const visage::Point centre = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(centre.x, centre.y + 400.0f));
	f.settleGesture();
	CHECK(f.read(ParamId::AZone) >= 0.0);
	CHECK(f.read(ParamId::AOffset) >= 0.0);
	CHECK(f.stage.selectedHome() >= homeFloor(0.0) - 1e-9);
}

TEST_CASE("undo and redo walk the identity edits this surface made") {
	ExploreFixture f;
	f.selection.select(1);
	const SoundIdentity start = f.stage.identity();
	CHECK_FALSE(f.stage.canUndo());

	const visage::Point centre = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(centre.x + 55.0f, centre.y - 70.0f));
	f.stage.onMouseUp().callback(at(centre.x + 55.0f, centre.y - 70.0f));
	f.settleGesture();
	// AND A TURN, so the plane really differs across the edit. Without one the plane half of this case
	// compares an unpinned identity against itself and passes on a build where undo ignores planes
	// entirely — which is exactly what the known-bad build showed.
	const visage::Bounds rotateBox = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point rotateAt { rotateBox.x() + rotateBox.width() * 0.5f,
		                           rotateBox.y() + rotateBox.height() * 0.5f };
	f.stage.onMouseDown().callback(at(rotateAt.x, rotateAt.y));
	f.stage.onMouseDrag().callback(at(rotateAt.x, rotateAt.y - 50.0f));
	f.stage.onMouseUp().callback(at(rotateAt.x, rotateAt.y - 50.0f));
	f.settleGesture();
	const SoundIdentity moved = f.stage.identity();
	REQUIRE(moved.driftX != doctest::Approx(start.driftX));
	REQUIRE_FALSE(moved.plane == start.plane);
	REQUIRE_FALSE(moved == start);
	REQUIRE(f.stage.canUndo());

	// Two edits now, so two undos to get back.
	f.stage.undo();
	f.settleGesture();
	f.stage.undo();
	f.settleGesture();
	CHECK(f.stage.identity() == start);
	// Including the drift half: an undo that put a sound's home back and left it out in the plane
	// would be restoring half a sound.
	CHECK(f.bridge.soundTravel().offset[1].x == doctest::Approx(start.driftX));
	// And the plane, restored as a stored number rather than as the turns subtracted back off.
	CHECK(f.bridge.soundOrientations().pin[1] == start.plane);
	REQUIRE(f.stage.canRedo());

	f.stage.redo();
	f.settleGesture();
	f.stage.redo();
	f.settleGesture();
	CHECK(f.stage.identity() == moved);
}

TEST_CASE("a breadcrumb records the identity you left, and the trail is capped") {
	ExploreFixture f;
	f.selection.select(1);
	const SoundIdentity start = f.stage.identity();
	const visage::Point centre = f.stage.disk().centre();

	f.stage.onMouseDown().callback(at(centre.x, centre.y - 50.0f));
	f.stage.onMouseUp().callback(at(centre.x, centre.y - 50.0f));
	f.settleGesture();
	std::vector<SoundIdentity> trail = f.stage.crumbs(1);
	REQUIRE(trail.size() == 1);
	// THE DEPARTURE POINT, not the arrival: the trail is the places you have been, so the last crumb
	// must not be a duplicate of where you now are.
	CHECK(trail.front() == start);
	CHECK_FALSE(trail.front() == f.stage.identity());

	for (int i = 0; i < 20; ++i) {
		f.stage.onMouseDown().callback(at(centre.x, centre.y - 50.0f - static_cast<float>(i)));
		f.stage.onMouseUp().callback(at(centre.x, centre.y - 50.0f - static_cast<float>(i)));
		f.settleGesture();
	}
	CHECK(f.stage.crumbs(1).size() == ExploreStage::kMaxCrumbs);
	// And it is the SELECTED sound's trail: another sound's history is not this one's.
	CHECK(f.stage.crumbs(0).empty());
}

TEST_CASE("zoom is the view scale and the mutation scale at once") {
	ExploreFixture f;
	f.selection.select(1);
	const double wideSpan = f.stage.homeSpanZones();

	f.stage.setZoom(16.0);
	CHECK(f.stage.homeSpanZones() == doctest::Approx(wideSpan / 16.0));
	// The travel axis is the same span in the same units, so it scales with it — a chart that zoomed
	// one axis and not the other would draw a step right and a step up as different distances.
	CHECK(f.stage.travelSpan() == doctest::Approx(f.stage.homeSpanZones() / 1023.0));

	// And a dice roll shrinks with it: the design has one control doing both jobs, so a closer look
	// really has to mean a smaller jump.
	const auto reach = [&](double zoom) {
		f.stage.setZoom(zoom);
		double most = 0.0;
		for (int i = 0; i < 40; ++i) {
			// **MEASURED ON THE PLANE, NOT ON HOME** (backlog GY). `mutate()` walks the sound around the
			// decoded plane, and since the one-value design it moves neither zone, offset nor gamma — so the old
			// probe read a number the dice no longer touches and every roll measured zero. The claim is
			// unchanged and is the design's: one control is both the view scale and the jump size.
			const double beforeX = f.stage.selectedDrift();
			const double beforeY = f.stage.selectedDriftY();
			f.stage.mutate();
			f.settleGesture();
			most = std::max(most, std::hypot(f.stage.selectedDrift() - beforeX,
			                                 f.stage.selectedDriftY() - beforeY));
		}
		return most;
	};
	const double wide = reach(1.0);
	const double close = reach(32.0);
	// NOT VACUOUS: the dice has to actually roll, or "close < wide" is two zeros and the case passes
	// against a mutate() that does nothing at all. This is what the old probe silently became.
	INFO("dice reach: " << wide << " at x1 against " << close << " at x32");
	REQUIRE(wide > 0.0);
	CHECK(close < wide * 0.5);
}

TEST_CASE("zoom stops at both ends of its ladder") {
	ExploreFixture f;
	f.stage.setZoom(1000.0);
	CHECK(f.stage.zoom() == doctest::Approx(ExploreStage::kMaxZoom));
	f.stage.setZoom(0.001);
	CHECK(f.stage.zoom() == doctest::Approx(ExploreStage::minZoom()));
}

TEST_CASE("undoing an explore edit leaves the slot's placement and width standing") {
	// THE REGRESSION THAT MADE SOMEBODY READ A WARNING. ExploreStage::record used to assemble its
	// SlotStates as `{ identity, on }` and let the rest default — so undoing a scrub or a mutate
	// wrote the DEFAULT placement and an empty stance list over whatever the slot had authored: the
	// sound snapped to the origin and forgot its places. -Wmissing-field-initializers had pointed at
	// the line since `place` joined the struct; the per-sound width joining it (V14a) is when the
	// warning got read. This pins the fix: an Explore undo restores the IDENTITY and leaves every
	// placement-family value exactly where the hand put it.
	ExploreFixture f;
	f.selection.select(0);
	f.set(ParamId::APlaceX, 0.31);
	f.set(ParamId::APlaceY, -0.22);
	f.set(soundParam(0, SoundField::Width), 0.4);
	const SlotState before = readSlotState(f.bridge, 0);

	f.stage.mutate();
	f.settleGesture();
	// The mutate really moved the identity, or the undo below walks nothing.
	REQUIRE_FALSE(readSlotState(f.bridge, 0).identity == before.identity);

	f.stage.undo();
	f.settleGesture();
	// The WHOLE state is back — identity restored, and the placement, width and stances never
	// touched. On the pre-fix record this failed with PlaceX at 0 and the width at its default.
	CHECK(readSlotState(f.bridge, 0) == before);
}

TEST_CASE("the explore width knob edits the sound under your hands, and only that one") {
	// V15: "in explore, we should only have the width knob for the sound we are exploring." One
	// visible control, rebound by selection — spelled as eight slot-bound Knobs and a visibility
	// flip, so the binding itself can never point where the label does not say. The claims: the
	// selected slot's knob is the visible one, a drag lands on THAT slot's Width parameter, and
	// moving the selection moves both the visibility and where the next drag lands.
	ExploreFixture f;
	f.stage.update();

	Knob& forA = f.stage.widthKnobForTest(0);
	REQUIRE(f.selection.slot() == 0);
	CHECK(forA.isVisible());
	CHECK_FALSE(f.stage.widthKnobForTest(1).isVisible());
	CHECK(f.stage.widthKnobBounds().width() > 0.0f);

	forA.processMouseDown(at(26.0f, 60.0f));
	forA.processMouseDrag(at(26.0f, 60.0f - Knob::kDragRange * 0.5f));
	forA.processMouseUp(at(26.0f, 60.0f - Knob::kDragRange * 0.5f));
	f.settleGesture();
	const auto widthOf = [&](size_t slot) {
		return f.bridge.value(static_cast<clap_id>(soundParam(slot, SoundField::Width)));
	};
	CHECK(widthOf(0) == doctest::Approx(0.5));
	CHECK(widthOf(1) == 0.0);

	f.selection.select(1);
	f.stage.update();
	CHECK_FALSE(forA.isVisible());
	Knob& forB = f.stage.widthKnobForTest(1);
	CHECK(forB.isVisible());

	forB.processMouseDown(at(26.0f, 60.0f));
	forB.processMouseDrag(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
	forB.processMouseUp(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
	f.settleGesture();
	CHECK(widthOf(1) == doctest::Approx(0.25));
	// And A's stayed exactly where the first drag left it — the selection moved, not the binding.
	CHECK(widthOf(0) == doctest::Approx(0.5));
}

TEST_CASE("a selection change mid-drag cannot smear the gesture onto the new sound") {
	// THE REBIND CASE NAMED AT REVIEW: a drag is in flight on the selected sound's width when the
	// selection moves. The bracketing rule says the gesture must close on the OLD slot's parameter —
	// and in this spelling that is structural rather than careful, because a Knob's binding is a
	// constructor argument: the bracket can only ever close on the id it opened on. What this pins
	// is the whole observable consequence: one properly bracketed gesture, every event of it on the
	// OLD slot's param, and the new slot's param never touched. (In the running editor the case is
	// barely reachable — every road that moves the selection needs the mouse the drag is holding —
	// but automation and future verbs get no chance to make it reachable and wrong.)
	ExploreFixture f;
	f.stage.update();
	Knob& forA = f.stage.widthKnobForTest(0);
	REQUIRE(forA.isVisible());

	forA.processMouseDown(at(26.0f, 60.0f));
	forA.processMouseDrag(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));

	// The selection moves mid-drag, and the stage syncs: the visible knob is B's now.
	f.selection.select(1);
	f.stage.update();
	CHECK_FALSE(forA.isVisible());
	CHECK(f.stage.widthKnobForTest(1).isVisible());

	// The drag finishes on the knob it started on.
	forA.processMouseDrag(at(26.0f, 60.0f - Knob::kDragRange * 0.5f));
	forA.processMouseUp(at(26.0f, 60.0f - Knob::kDragRange * 0.5f));

	const auto widthId = [](size_t slot) {
		return static_cast<clap_id>(soundParam(slot, SoundField::Width));
	};
	const std::vector<UiParamEvent> posted = drain(f.bridge);
	REQUIRE_FALSE(posted.empty());
	// Bracketed, and every event on A's param — the id the gesture opened on.
	CHECK(posted.front().kind == UiParamEvent::Kind::GestureBegin);
	CHECK(posted.back().kind == UiParamEvent::Kind::GestureEnd);
	for (const UiParamEvent& event : posted) {
		CHECK(event.id == widthId(0));
	}
}

TEST_CASE("the explore width knob tells the truth about doing nothing") {
	// The honesty rule's two width cases (V11/V12c): a frozen-engine patch, where the parameter
	// changes no sound at any value, and a PARKED selected sound, which is not in the character at
	// all. They no longer look alike ("prefer not drawn over greyed for explore engine
	// settings since theres no way to ungrey them without fully switching engine"): off V5 only the engine
	// could wake the knob, so it is NOT DRAWN and a pointer at its centre does not reach it; parked, the
	// sound's own switch on this face would, so it is drawn GREYED — its value shown, the mouse refused.
	//
	// THE KNOWN-BAD IS A CONTROL BUILD, not a control assertion: the same knob with enablement
	// forced back on — exactly what a build that ignored the parked gate would be — answers the
	// same drag, so the refusals above could not have passed on a knob that simply answers nothing.
	// And the honest build's own sync then takes the forced enablement back, which is the guard
	// running the right direction.
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Morph));
	f.stage.update();

	Knob& knob = f.stage.widthKnobForTest(f.selection.slot());
	CHECK_FALSE(knob.isVisible());
	CHECK_FALSE(knob.enabled());
	const visage::Bounds b = f.stage.widthKnobBounds();
	const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	CHECK(f.stage.frameAtPoint(centre) != &knob);

	// The parked case, on the flagship: the sound is out of the constellation, so its width buys
	// nothing until the switch comes back — and the knob says so, on the face.
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	f.set(ParamId::AEnable, 0.0);
	f.stage.update();
	REQUIRE(knob.isVisible());
	CHECK_FALSE(knob.enabled());
	knob.processMouseDown(at(26.0f, 60.0f));
	knob.processMouseDrag(at(26.0f, 20.0f));
	knob.processMouseUp(at(26.0f, 20.0f));
	CHECK_FALSE(f.bridge.hasPendingWork());

	f.set(ParamId::AEnable, 1.0);
	f.stage.update();
	CHECK(knob.enabled());

	// THE CONTROL BUILD. Parked again, force the enablement a bad build would never have cleared: the
	// very drags refused above now post, which is what proves the refusals were enablement doing its job.
	f.set(ParamId::AEnable, 0.0);
	f.stage.update();
	REQUIRE_FALSE(knob.enabled());
	knob.setEnabled(true);
	knob.processMouseDown(at(26.0f, 60.0f));
	knob.processMouseDrag(at(26.0f, 20.0f));
	knob.processMouseUp(at(26.0f, 20.0f));
	CHECK(f.bridge.hasPendingWork());
	f.settleGesture();
	// And the honest build corrects the forced state on the next frame.
	f.stage.update();
	CHECK_FALSE(knob.enabled());
}

TEST_CASE("the chart yields: the width knob is on stage at every size") {
	// V16: hiding the width knob on a narrow window was a defect, not a layout choice. The dependency
	// is INVERTED — the layout reserves the width column before
	// sizing the chart square, so the knob is always present and the chart gets what remains. THE
	// KNOWN-BAD IS THE OLD HIDING BEHAVIOUR THESE ASSERTIONS REPLACE: on the pre-V16 code the
	// narrow sizes below came back with an empty rectangle and every knob invisible — this case's
	// own first version asserted exactly that, so the inversion is measured against a behaviour
	// that demonstrably existed, not a strawman.
	ExploreFixture f;
	struct Size {
		float width;
		float height;
	};
	// The shipped stage, the narrow window's league (where the old code hid the knob), and two
	// harsher ones down to the rails-squeezed minimum the gui notes record.
	for (const Size size : { Size { 866.0f, 534.0f }, Size { 560.0f, 534.0f },
	                         Size { 360.0f, 400.0f }, Size { 200.0f, 300.0f } }) {
		CAPTURE(size.width);
		f.stage.setBounds(0.0f, 0.0f, size.width, size.height);
		f.stage.update();

		// A real rectangle, entirely on stage.
		const visage::Bounds knob = f.stage.widthKnobBounds();
		CHECK(knob.width() > 0.0f);
		CHECK(knob.x() >= 0.0f);
		CHECK(knob.x() + knob.width() <= size.width);
		CHECK(f.stage.widthKnobForTest(f.selection.slot()).isVisible());

		// And the chart square never overlaps the column: the square's left edge sits at least the
		// flank gap to the right of the knob — which also keeps the corners clear, since they live
		// INSIDE the square.
		const StageDisk d = f.stage.disk();
		const float squareLeft = d.centre().x - (d.radius() + StageDisk::kRimInset);
		CHECK(squareLeft >= knob.x() + knob.width());

		// The general affordance (V16 settled): both flank strips are real, on stage, and mirror
		// images about the disk's centre — the banked right one included, geometry only.
		const visage::Bounds left = f.stage.flankColumn(ExploreStage::Flank::Left);
		const visage::Bounds right = f.stage.flankColumn(ExploreStage::Flank::Right);
		CHECK(left.x() == knob.x());
		CHECK(right.x() + right.width() <= size.width);
		CHECK(right.x() >= d.centre().x + d.radius() + StageDisk::kRimInset);
		CHECK(right.x() - (d.centre().x + d.radius() + StageDisk::kRimInset)
		      == doctest::Approx(squareLeft - (left.x() + left.width())));
	}

	// And at the generous size the reserve is free: the disk is the full min-side one it always
	// was, so V16 changed nothing where the room was never in question.
	f.stage.setBounds(0.0f, 0.0f, 866.0f, 534.0f);
	CHECK(f.stage.disk().radius() == StageDisk(866.0f, 534.0f).radius());
}

TEST_CASE("a variant switch re-settles the chart on the sound") {
	// The terrain must re-centre when the variant switches. A
	// same-slot switch changes no selection, so the selection-change recentre never fires, and no
	// other road asked the view to re-settle — the gap this pins shut. The switch's record now asks
	// to be followed (EditRecord::followsOnRecord), through the SAME centering road an undo already
	// walks. The KNOWN-BAD is the displacement REQUIREd before the switch: without the
	// follow-on-record wiring the view stays exactly there, and the equality below cannot pass.
	ExploreFixture f;
	FrameWatchers previewWatchers;
	SoundStack stack { f.bridge, f.registry, f.selection, f.history, previewWatchers, &f.tray };
	stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, 400.0f);
	f.selection.select(0);

	stack.storeVariationAt(0); // two records; the copy is active
	f.settleGesture();
	// Settle the view once, through the wiring under test's own road (a first switch), so the view
	// is genuinely HELD rather than derived — an unsettled viewCentre() answers "where it would
	// settle" and could never disagree with the home below.
	stack.activateVariation(0, 0);
	f.settleGesture();
	REQUIRE(f.stage.viewCentre() == doctest::Approx(f.stage.selectedDriftY()));

	// Walk the sound's home away from the settled view down the one road that holds the view while
	// home moves: the HOST's (A29 — the view follows automated identity only when the star exits;
	// a small move leaves it held). The stage's own gestures all recentre at mouse-up, so this is
	// also the state a player is actually in when the gap bites: automation moved the sound, the
	// chart stayed, and a variant switch should bring the two back together.
	f.set(ParamId::AZone, 40.0);
	REQUIRE(f.stage.viewCentre() != doctest::Approx(f.stage.selectedHome()));

	// The switch. Under V20 it MOVES THE SOUND — the incoming record restores the exploration it
	// stored (zone 0-era home, not the automated 40) — and its record asks to be followed, so the
	// chart lands on the RECALLED home: the requirement made literal, the chart shows the recalled
	// exploration.
	const double automatedHome = f.stage.selectedHome();
	stack.activateVariation(0, 1);
	f.settleGesture();
	REQUIRE(f.stage.selectedHome() != doctest::Approx(automatedHome)); // the switch moved the sound
	CHECK(f.stage.viewCentre() == doctest::Approx(f.stage.selectedDriftY()));
}

TEST_CASE("undo walks a variant switch back, chart and record both") {
	// The switch records a StanceEdit, so the undo expectation holds by the same machinery every
	// other verb uses — pinned here through the real history, on the ride-along axis (width) so the
	// claim survives whatever future decisions redraw the stance's contents. One undo: the act
	// pointer returns, the parameters return, the outgoing record's bank is released back to
	// dead-while-active (its stored copy is the live view again), and the chart re-settles via the
	// same editApplied road it has always walked.
	ExploreFixture f;
	FrameWatchers previewWatchers;
	SoundStack stack { f.bridge, f.registry, f.selection, f.history, previewWatchers, &f.tray };
	stack.setBounds(0.0f, 0.0f, SoundStack::kWidth, 400.0f);
	f.selection.select(0);
	const auto width0 = [&] {
		return f.bridge.value(static_cast<clap_id>(soundParam(0, SoundField::Width)));
	};

	f.set(soundParam(0, SoundField::Width), 0.6);
	f.set(ParamId::AZone, 200.0);
	stack.storeVariationAt(0); // banks {0.6, zone 200}; the copy is active
	f.set(soundParam(0, SoundField::Width), 0.2); // the active record wears both edits
	f.set(ParamId::AZone, 800.0);
	f.settleGesture();

	stack.activateVariation(0, 0); // banks {0.2, 800} into row 1, restores {0.6, 200}
	f.settleGesture();
	REQUIRE(width0() == doctest::Approx(0.6));
	REQUIRE(f.bridge.value(static_cast<clap_id>(ParamId::AZone)) == doctest::Approx(200.0));
	REQUIRE(stack.activeStance(0, 0));
	REQUIRE(stack.variationValuesAt(0, 1).width == doctest::Approx(0.2));
	REQUIRE(stack.variationValuesAt(0, 1).zone == doctest::Approx(800.0));

	REQUIRE(f.history.canUndo());
	f.history.undo();
	f.settleGesture();
	// The act pointer and the parameters are back, and row 1 — active again — mirrors the live 0.2
	// (its stored copy is dead-while-active once more).
	CHECK(stack.activeStance(0, 1));
	CHECK(width0() == doctest::Approx(0.2));
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::AZone)) == doctest::Approx(800.0));
	CHECK(stack.variationValuesAt(0, 1).width == doctest::Approx(0.2));
	CHECK(stack.variationValuesAt(0, 0).width == doctest::Approx(0.6));
	// And the chart followed the undo onto the restored exploration — the editApplied half of the
	// one road, landing where the sound actually is.
	CHECK(f.stage.viewCentre() == doctest::Approx(f.stage.selectedDriftY()));

	// Redo banks again and lands back on the original record, chart included.
	f.history.redo();
	f.settleGesture();
	CHECK(stack.activeStance(0, 0));
	CHECK(width0() == doctest::Approx(0.6));
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::AZone)) == doctest::Approx(200.0));
	CHECK(f.stage.viewCentre() == doctest::Approx(f.stage.selectedDriftY()));
}

TEST_CASE("the corners win the press they land on, and two of them answer nothing") {
	ExploreFixture f;
	const auto centreOf = [&](ExploreStage::Part part) {
		const visage::Bounds b = f.stage.partBounds(part);
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};
	CHECK(f.stage.hitAt(centreOf(ExploreStage::Part::Gamma)) == ExploreStage::Part::Gamma);
	CHECK(f.stage.hitAt(centreOf(ExploreStage::Part::Zoom)) == ExploreStage::Part::Zoom);
	CHECK(f.stage.hitAt(centreOf(ExploreStage::Part::Mutate)) == ExploreStage::Part::Mutate);
	CHECK(f.stage.hitAt(centreOf(ExploreStage::Part::Undo)) == ExploreStage::Part::Undo);
	CHECK(f.stage.hitAt(centreOf(ExploreStage::Part::Redo)) == ExploreStage::Part::Redo);
	CHECK(f.stage.hitAt(f.stage.disk().centre()) == ExploreStage::Part::Chart);

	CHECK(f.stage.hitAt(centreOf(ExploreStage::Part::Rotate)) == ExploreStage::Part::Rotate);
	CHECK(f.stage.hitAt(centreOf(ExploreStage::Part::Radius)) == ExploreStage::Part::Radius);
}

TEST_CASE("the rotate corner writes the plane channel and nothing else") {
	// THE TRIPWIRE THIS REPLACES watched `drain()` and the travel channel, and would have stayed green
	// through the natural early wiring — which goes through the orientation atomics and the dirty flag
	// and never touches the parameter queue. Now that the corner is live, the same lesson decides what
	// its control watches: the channels rotate ACTUALLY uses.
	ExploreFixture f;
	f.selection.select(2);
	f.bridge.takeStateDirty();
	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };

	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 40.0f));
	f.stage.onMouseUp().callback(at(centre.x, centre.y - 40.0f));

	// The sound it names, and no other.
	CHECK(f.bridge.soundOrientations().pin[2].pinned);
	CHECK_FALSE(f.bridge.soundOrientations().pin[1].pinned);
	// Patch state, so the host has to be told the project changed.
	//
	// NOT A CONTROL FOR THE BRACKET, and the known-bad build said so: `setSoundOrientation` sets the
	// flag itself, so deleting `endSoundOrientationGesture` entirely leaves this green. Same shape as
	// the travel bracket, and named here for the same reason — a verdict that cannot fail should say
	// what it can and cannot see rather than borrow credit from the line above it.
	CHECK(f.bridge.takeStateDirty());
	// And nothing else moved: not a parameter, not the drift channel, not the global audition pin.
	CHECK(drain(f.bridge).empty());
	CHECK(f.bridge.soundTravel().offset[2].x == doctest::Approx(0.0));
	CHECK(f.bridge.orientation() == SliceOrientationPin {});
}

TEST_CASE("the rotate corner does not guess which plane the sound is on") {
	// G3's own hazard, at the surface. The three-argument `rotateSoundBy` takes the plane to turn FROM,
	// and a surface that has not worked it out passes a default-constructed generator — the DEGENERATE
	// plane, aperture zero — so the sound jumps to the line the engine was before any of this, silently,
	// on the first click. The deriving overload works it out instead, and this is what says we called it.
	// RESTATED WHEN THE APERTURE STOPPED BEING READ, and the premise is what changed rather than the
	// standard. The old assertion was that a one-pixel drag leaves the APERTURE where it was, because a
	// guessed `from` would have read zero and zero aperture was the degenerate plane. Under M there is
	// no aperture in the decode and a default-constructed generator is rotate 0 — the very plane an
	// unpinned sound plays — so that assertion now passes on the bug it was written to catch as well as
	// on correct code. It is replaced by the claim it was always standing in for: ONE PIXEL OF DRAG
	// MOVES THE SOUND ONE PIXEL'S WORTH, in the plane the sound is actually on.
	ExploreFixture f;
	f.selection.select(2);
	// Unpinned to start with, so the plane comes from the sound's HOME — the case a guess destroys.
	REQUIRE_FALSE(f.bridge.soundOrientations().pin[2].pinned);
	const double rotateBefore = f.stage.selectedRotation();
	const double homeOf = sliceHome(static_cast<uint16_t>(std::lround(f.stage.identity().zone)),
	                                static_cast<float>(f.stage.identity().offset
	                                                   + f.stage.identity().gamma),
	                                0.0);
	const MorphBasis planeBefore = sliceBasisFor<kMorphTriangles>(rotateBefore, homeOf);

	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	// One pixel: the smallest turn there is, where a jump would be most obvious.
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 1.0f));

	// It turned, and it turned from where the sound already was: one pixel of the encoder's travel and
	// no more. A surface that guessed its `from` — or read the corner off a different sound — would
	// land somewhere unrelated, and the plane would move by a great deal more than one pixel's worth.
	const double rotateAfter = f.stage.selectedRotation();
	CHECK(rotateAfter != doctest::Approx(rotateBefore));
	// One pixel of the encoder, in radians, taken from the surface's own two constants rather than
	// written out: the corner spreads a full kRotateSpan across Knob::kDragRange pixels of drag.
	const double perPixel = ExploreStage::kRotateSpan / static_cast<double>(Knob::kDragRange);
	CHECK(std::abs(rotateAfter - rotateBefore) <= doctest::Approx(perPixel).epsilon(0.001));

	const MorphBasis planeAfter = sliceBasisFor<kMorphTriangles>(rotateAfter, homeOf);
	const double climb = 4.0 * detail::kPlaneTurnRates[0] * detail::kRateRowTurn
	                     * std::max(detail::kPhaseSpread, detail::kRateSpread) * perPixel;
	for (size_t i = 0; i < 2 * kMorphTriangles; ++i) {
		INFO("coordinate " << i);
		CHECK(std::abs(planeAfter.x[i] - planeBefore.x[i]) <= climb * 1.000001);
	}
	f.stage.onMouseUp().callback(at(centre.x, centre.y - 1.0f));
}

TEST_CASE("the rotate dial and its degree text can never disagree") {
	// R1: the dial used `std::fmod`, which KEEPS ITS ARGUMENT'S SIGN — so every turn past −180° came
	// back negative and clamped the dial to the bottom of its travel while the text went on counting.
	// Measured on the shipped build: −183°, −229° and −372° all drew at 0.0000. Positive angles wrapped
	// correctly, which is exactly why it survived being looked at.
	//
	// Reachable in one drag by anybody: an untouched sound's plane already sits anywhere in ±π (the
	// review measured −135.84° at zone 880), so a single turn downward crosses the boundary.
	// **ITS OWN PI, NOT THE CORNER'S GEARING.** This read `ExploreStage::kRotateSpan`, which was
	// numerically pi and is a GEARING — how far one encoder travel turns the plane — not the
	// mathematical constant. The two were equal by coincidence, and the moment the corner's
	// sensitivity is tuned (backlog GF) this conversion would quietly start expressing degrees in
	// quarter-radians: the sweep below would claim to walk +-900 degrees while walking a quarter of
	// that, and it would keep passing, because `rotateDialPosition` wraps correctly at any angle.
	// A constant borrowed for its VALUE rather than its MEANING is a test that stops testing what its
	// name says the day somebody tunes the thing it borrowed from.
	constexpr double kPi = 3.14159265358979323846;
	const auto atDegrees = [](double degrees) {
		return ExploreStage::rotateDialPosition(degrees * kPi / 180.0);
	};

	// IN RANGE EVERYWHERE, which is the property the clamp was standing in for.
	for (double degrees = -900.0; degrees <= 900.0; degrees += 7.0) {
		const float dial = atDegrees(degrees);
		CHECK(dial >= 0.0f);
		CHECK(dial < 1.0f);
	}

	// THE NEGATIVE CASES THE OLD ONE FLATTENED, each distinct rather than all zero.
	CHECK(atDegrees(-183.0) == doctest::Approx(0.9917).epsilon(0.001));
	CHECK(atDegrees(-229.0) == doctest::Approx(0.8639).epsilon(0.001));
	CHECK(atDegrees(-372.0) == doctest::Approx(0.4667).epsilon(0.001));
	CHECK(atDegrees(-183.0) != doctest::Approx(atDegrees(-229.0)));
	CHECK(atDegrees(-229.0) != doctest::Approx(atDegrees(-372.0)));

	// AND THE DIAL IS A COMPASS: a whole turn either way is the same place, so the wrap is a wrap
	// rather than a fold. Both directions, because the bug was that only one of them worked.
	for (const double degrees : { -300.0, -135.84, 0.0, 90.0, 300.0 }) {
		CHECK(atDegrees(degrees + 360.0) == doctest::Approx(atDegrees(degrees)).epsilon(0.0001));
		CHECK(atDegrees(degrees - 360.0) == doctest::Approx(atDegrees(degrees)).epsilon(0.0001));
	}

	// The positive half, unchanged — a fix that broke what already worked would be a worse bug.
	CHECK(atDegrees(0.0) == doctest::Approx(0.5));
	CHECK(atDegrees(90.0) == doctest::Approx(0.75));
	CHECK(atDegrees(-90.0) == doctest::Approx(0.25));
	CHECK(atDegrees(183.0) == doctest::Approx(0.0083).epsilon(0.001));
}

TEST_CASE("a turn accumulates in the generator rather than being set from the drag") {
	ExploreFixture f;
	f.selection.select(2);
	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };

	// MEASURED AGAINST THE PLANE IT STARTED ON, and that is the whole of this case. The first version
	// derived the starting point from its own two measurements — `start = half - (whole - half)` — which
	// is satisfied by ANY sequence, including the broken one, and it duly passed the known-bad build.
	// A fixture that solves for its own baseline is asserting arithmetic, not behaviour.
	const double before = f.stage.selectedRotation();
	const auto turnOf = [](float pixels) {
		return static_cast<double>(pixels) / Knob::kDragRange * ExploreStage::kRotateSpan;
	};

	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 20.0f));
	CHECK(f.stage.selectedRotation() - before == doctest::Approx(turnOf(20.0f)).epsilon(0.01));
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 40.0f));
	// Twice the travel is twice the turn, from where the plane started. Sending the whole drag every
	// frame instead of the increment turns it by the SUM of every frame's reading, which grows with the
	// frame rate — a bug that only shows on a fast machine, and one no self-referential baseline sees.
	CHECK(f.stage.selectedRotation() - before == doctest::Approx(turnOf(40.0f)).epsilon(0.01));
	f.stage.onMouseUp().callback(at(centre.x, centre.y - 40.0f));
}

namespace {

/// The table the engine would build for this fixture's patch, through every channel it reads.
owlet::dsp::PhiMorphWavetable tableOf(ExploreFixture& fix) {
	const auto read = [&](ParamId id) { return fix.bridge.modulated(static_cast<clap_id>(id)); };
	return buildMorphFor(characterFromParams(read, oscTypeFromParams(read), fix.bridge.tuning(),
	                                         fix.bridge.puckVelocity(), fix.bridge.soundTravel(),
	                                         fix.bridge.soundOrientations(),
	                                         fix.bridge.soundResidue(), SoundGamma {}, Preview {}, EarSide::Head));
}

void turnRotateCorner(ExploreFixture& fix, float pixels) {
	const visage::Bounds box = fix.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point c { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	fix.stage.onMouseDown().callback(at(c.x, c.y));
	fix.stage.onMouseDrag().callback(at(c.x, c.y - pixels));
	fix.stage.onMouseUp().callback(at(c.x, c.y - pixels));
}

void driftSlotZeroTo(ExploreFixture& fix, double x) {
	fix.bridge.beginSoundTravelGesture();
	fix.bridge.setSoundTravel(0, { x, 0.0 });
	fix.bridge.endSoundTravelGesture();
}

} // namespace

TEST_CASE("rotating leaves the sound you are standing on, and re-aims where travel leads") {
	// SUPERSEDES A10 (landed engine-side at e7f1ac9): a rotation must preserve the STANDING point
	// at any drift, not only at home. The reasoning is this surface's own — on a 2-D chart every drag
	// that is not exactly vertical commits drift, so under A10 rotate preserved the sound almost never,
	// which is the opposite of what a re-aiming verb is for. A rotation now banks what it would have
	// moved into a stored residue, and the decode adds it back.
	//
	// THE DRIFTED ASSERTION USED TO READ `!= 0` — A10 faithfully encoded, and now a rejected
	// behaviour. Flipping it is the whole of what changed. The case is kept rather than deleted because
	// the verb still has to DO something, and the last third is what says so.
	//
	// A FRESH FIXTURE PER CLAIM, on purpose: chaining them through one fixture makes each case depend
	// on the rotations before it, and the first draft did exactly that and reported a failure that was
	// really its own setup.
	SUBCASE("at home, untouched") {
		ExploreFixture f;
		f.selection.select(0);
		// THE FIXTURE CAN NOTICE: move the zone and the table must change, or the memcmps below are
		// comparing something this patch does not depend on.
		const owlet::dsp::PhiMorphWavetable atHome = tableOf(f);
		f.set(ParamId::AZone, 300.0);
		const owlet::dsp::PhiMorphWavetable elsewhere = tableOf(f);
		REQUIRE(std::memcmp(&atHome, &elsewhere, sizeof(atHome)) != 0);
		f.set(ParamId::AZone, 0.0);

		turnRotateCorner(f, 70.0f);
		REQUIRE(f.bridge.soundOrientations().pin[0].pinned);
		const owlet::dsp::PhiMorphWavetable turned = tableOf(f);
		CHECK(std::memcmp(&atHome, &turned, sizeof(atHome)) == 0);
	}

	SUBCASE("and drifted, also untouched — the overrule") {
		ExploreFixture f;
		f.selection.select(0);
		driftSlotZeroTo(f, 0.35);
		const owlet::dsp::PhiMorphWavetable drifted = tableOf(f);
		turnRotateCorner(f, 70.0f);
		REQUIRE(f.bridge.soundOrientations().pin[0].pinned);
		const owlet::dsp::PhiMorphWavetable turned = tableOf(f);
		CHECK(std::memcmp(&drifted, &turned, sizeof(drifted)) == 0);
	}

	SUBCASE("but it is not a no-op: it re-aims where further travel leads") {
		// (1) and (2) alone are satisfied by a verb that does nothing at all. What rotation changes is
		// where FURTHER travel LEADS — so two histories differing only by a rotation, then drifting by
		// the same amount, must part company.
		ExploreFixture turnedFirst;
		ExploreFixture plain;
		turnedFirst.selection.select(0);
		plain.selection.select(0);
		driftSlotZeroTo(turnedFirst, 0.35);
		driftSlotZeroTo(plain, 0.35);
		const owlet::dsp::PhiMorphWavetable before = tableOf(plain);
		const owlet::dsp::PhiMorphWavetable beforeTurned = tableOf(turnedFirst);
		REQUIRE(std::memcmp(&before, &beforeTurned, sizeof(before)) == 0);

		turnRotateCorner(turnedFirst, 70.0f);
		driftSlotZeroTo(turnedFirst, 0.55);
		driftSlotZeroTo(plain, 0.55);
		const owlet::dsp::PhiMorphWavetable wentFurtherTurned = tableOf(turnedFirst);
		const owlet::dsp::PhiMorphWavetable wentFurtherPlain = tableOf(plain);
		CHECK(std::memcmp(&wentFurtherPlain, &wentFurtherTurned, sizeof(wentFurtherPlain)) != 0);
	}
}

//---------------------------------------------------------------------------------------------//
// THE POLAR CORNER ("I want to move drift to be polar in the plane and add a  //
// theta knob and rename drift radius")                                                          //
//                                                                                               //
// The state of record does NOT become polar — `SoundTravel::offset` stays the cartesian pair the //
// engine decodes, and these two corners are a VIEW onto it. That is what every case here is      //
// really asserting: a polar gesture, a cartesian patch, and no drift between the two.            //
//---------------------------------------------------------------------------------------------//

namespace {

/// Turn a polar knob by `dy` pixels (positive is up, which is more), gesture closed.
///
/// THE KNOB, NOT THE STAGE. Radius and Theta were custom-drawn corners driven through the stage's own
/// mouse handlers; they are child `Knob`s now, which is what lets them glow and take a source, and in
/// the shipped editor a child frame receives the press. Calling the stage's callback at the corner
/// would be driving a road no hand can reach any more.
Knob& polarKnob(ExploreFixture& f, ExploreStage::Part part) {
	return part == ExploreStage::Part::Theta ? f.stage.thetaKnobForTest(f.selection.slot())
	                                         : f.stage.radiusKnobForTest(f.selection.slot());
}

void dragCorner(ExploreFixture& f, ExploreStage::Part part, float dy) {
	f.stage.update(); // the visibility flip and the bounds — a knob for the selected slot, sized
	Knob& knob = polarKnob(f, part);
	const float x = knob.width() * 0.5f;
	const float y = knob.height() * 0.5f;
	knob.processMouseDown(at(x, y));
	knob.processMouseDrag(at(x, y - dy));
	knob.processMouseUp(at(x, y - dy));
	f.settleGesture();
}

} // namespace

TEST_CASE("a hand can point a source at Radius and Theta: they glow, take the drag, and the route is real") {
	// **THE CASE WHOSE ABSENCE SHIPPED A DEAD FEATURE.** The first delivery of "radius and theta as mod
	// destinations" put them in the engine's table and ran `ctest` 112/112 — and nothing in the editor
	// could point a source at them, because the corners were custom-drawn and answered no `display()`.
	// The symptom was no mod glow on the polar corners, while similar destinations (FM depth, width)
	// already glowed — so the gap was in the editor, not the engine.
	//
	// explore_stage.cpp had said so all along, about the field knobs: a knob with no ModState cannot be
	// pointed at: that loop is what makes them destinations in practice rather than only in
	// kModDestinations. No case asked the question a hand asks. This one does, in a hand's order:
	// arm a source, see the glow, drag, and find the route where the ENGINE will read it.
	//
	// KNOWN-BAD: delete the two polar lines from `ExploreStage::setModulation` and the glow checks die.
	// THE FIXTURE OWNS THE PANEL STATE, above the stage, so it outlives the knobs that watch it — see
	// `ExploreFixture::mod` for the use-after-free a case-local `ModState` was here.
	ExploreFixture f;
	ModState& mod = f.mod;
	f.selection.select(2);
	f.stage.setModulation(&mod, nullptr);
	f.stage.update();

	for (const TravelAxis axis : { TravelAxis::Radius, TravelAxis::Theta }) {
		CAPTURE(static_cast<int>(axis));
		Knob& knob = axis == TravelAxis::Radius ? f.stage.radiusKnobForTest(2) : f.stage.thetaKnobForTest(2);
		const backplane::Destination destination =
		    backplane::Destination::core(travelDestinationIndex(2, axis));
		REQUIRE(knob.isVisible());

		// (1) AT REST IT IS AN ORDINARY KNOB: not lit, and a drag moves the SOUND.
		CHECK_FALSE(knob.haloLit());

		// (2) ARMED, IT GLOWS — the thing that was missing.
		mod.focus(macroSourceAt(0));
		mod.arm(true);
		CHECK(knob.mappable());
		CHECK(knob.haloLit());

		// (3) AND THE DRAG ASSIGNS rather than moving the sound: the place is untouched and a route
		// exists from the armed source to THIS destination, in the internal region of the core table.
		const AnchorPoint placeBefore = f.bridge.soundTravel().offset[2];
		const float x = knob.width() * 0.5f;
		const float y = knob.height() * 0.5f;
		knob.processMouseDown(at(x, y));
		knob.processMouseDrag(at(x, y - Knob::kDragRange * 0.5f));
		knob.processMouseUp(at(x, y - Knob::kDragRange * 0.5f));
		const float amount = f.bridge.routeAmount(macroSourceAt(0), destination);
		INFO("assigned amount " << amount);
		CHECK(amount > 0.0f);
		CHECK(f.bridge.soundTravel().offset[2].x == placeBefore.x);
		CHECK(f.bridge.soundTravel().offset[2].y == placeBefore.y);
		// THE RING IS DRAWN FROM IT, so the face says what was assigned — AND IT HAS A LENGTH A HAND
		// CAN SEE. `drawn` alone was true of a Radius ring 1/2046 of the face long, so Radius looked as if
		// it had no mod assignments: the route was real and the arc was
		// measured against the corner's 0..64 range. Stated from the amount: Radius's full swing is
		// half a sweep of its vernier, Theta's is one whole turn of its compass.
		CHECK(knob.modRingForTest().drawn);
		CHECK(f.bridge.soundTravel().orbit[2] == PolarOffset {}); // nor did the drag turn the corner
		const float length = knob.modRingForTest().hi - knob.modRingForTest().lo;
		// Theta: a full amount is one whole turn of the compass. Radius: a route moves ALONG THE FACE
		// (radius-mod-jump,), so from the value (zero here) the arc is the amount itself, held at
		// the face's top — the same as Theta's arithmetic, the curve no longer in the way.
		const float wanted = std::min(1.0f - knob.normalized(), amount);
		INFO("ring length " << length << ", wanted " << wanted);
		CHECK(length == doctest::Approx(wanted));
		CHECK(length > 0.1f);

		// (4) ONLY THIS SOUND'S: the same axis on a neighbour has no route.
		CHECK(f.bridge.routeAmount(macroSourceAt(0),
		                           backplane::Destination::core(travelDestinationIndex(3, axis)))
		      == 0.0f);

		// (5) AND THE ROUTES LIST CAN NAME IT, sound included — eight rows all called "Radius" would
		// make the sound the one thing a reader could not tell.
		CHECK(f.bridge.destinationName(destination)
		      == std::string(axis == TravelAxis::Radius ? "C Radius" : "C Theta"));
		mod.arm(false);
	}
}

TEST_CASE("a modulated sound's star orbits: drawn where the engine plays it, leaving no crumb, moving no view, writing nothing") {
	// DESIGN: a sound with a modulator on Radius/Theta visibly moves on the chart, but leaves no
	// breadcrumbs and does not re-centre the view.
	//
	// THE ENGINE IS STOOD IN FOR BY ITS OWN PUBLISH. `publishTravelPulls` is the one road the pull
	// reaches the editor by, so publishing on it is the platform's contract rather than a guess at it;
	// that the engine really publishes what it plays is test_synth's half.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(2);
	// A 3-4-5 place, small enough that a quarter turn round the origin stays on the chart at x1.
	// PUT at (0.003, 0.004) and PLAYED 0.005 from there along +x: the offset is the sound's own, and
	// what a modulator turns is THAT, about the sound's place.
	f.bridge.setSoundTravel(2, { 0.003, 0.004 });
	f.bridge.setSoundOrbit(2, { 0.005, 0.0 });
	f.stage.draw(canvas);
	f.settleScrub();
	// Let the surface finish with the fixture's own move, until a frame asks for nothing.
	bool quiet = false;
	for (int frame = 0; frame < 64 && !quiet; ++frame) {
		f.now += 16;
		quiet = !f.stage.update();
	}
	REQUIRE(quiet);
	f.stage.draw(canvas);
	// TWO STARS (a second, displaced puck, so the primary location stays visible): the
	// main one on the place, the second where the sound is played — the place plus its offset.
	REQUIRE(f.stage.starDrawnOnChart());
	REQUIRE(f.stage.playedMarkDrawn());
	const visage::Point primary = f.stage.starDrawnAt();
	const visage::Point placeOnGlass = f.stage.glassAt(0.004, 0.003);
	CHECK(primary.x == doctest::Approx(placeOnGlass.x));
	CHECK(primary.y == doctest::Approx(placeOnGlass.y));
	const visage::Point rest = f.stage.playedMarkAt();
	const visage::Point playedOnGlass = f.stage.glassAt(0.004, 0.008);
	CHECK(rest.x == doctest::Approx(playedOnGlass.x));
	CHECK(rest.y == doctest::Approx(playedOnGlass.y));
	// AND THE VECTOR BETWEEN THEM (pointing from the centre to the offset puck):
	// from the main star, along the line to the second one, stopping short of it.
	REQUIRE(f.stage.vectorDrawn());
	CHECK(f.stage.vectorFrom().x == doctest::Approx(primary.x));
	CHECK(f.stage.vectorFrom().y == doctest::Approx(primary.y));
	{
		const visage::Point tip = f.stage.vectorTip();
		const float whole = std::hypot(rest.x - primary.x, rest.y - primary.y);
		const float shaft = std::hypot(tip.x - primary.x, tip.y - primary.y);
		const float cross = (tip.x - primary.x) * (rest.y - primary.y) - (tip.y - primary.y) * (rest.x - primary.x);
		CHECK(std::abs(cross) / whole < 0.01f); // on the line
		CHECK(shaft > whole * 0.5f);            // most of the way
		CHECK(shaft < whole);                   // and short of the glyph
	}

	const double viewBefore = f.stage.viewCentre();
	const float xOfFixedTravel = f.stage.xAtTravelX(0.0);
	const bool undoBefore = f.stage.canUndo();
	REQUIRE(f.stage.crumbs(2).empty());

	// THE ENGINE REPORTS A PULL: a quarter turn, and a fifth more radius.
	TravelPulls pulls;
	pulls.theta[2] = std::numbers::pi * 0.5;
	// A RADIUS PULL IS A FACE DISTANCE (radius-mod-jump), so "a fifth more" is the face's step from 0.005 to 0.006 —
	// restated: face(r) = ln(1 + r / 2 (e^8.4 - 1)) / 8.4.
	const auto faceOf = [](double r) { return std::log1p(r / 2.0 * std::expm1(8.4)) / 8.4; };
	pulls.radius[2] = faceOf(0.006) - faceOf(0.005);
	f.bridge.publishTravelPulls(pulls);
	f.now += 16;
	CHECK(f.stage.update()); // the star moved, so the chart asks to be painted
	f.stage.draw(canvas);

	// BY AN INDEPENDENT RESTATEMENT, not by calling `pulledPlace` back: the offset grows 5 -> 6 and
	// turns a quarter from +x to +y, ABOUT THE PLACE — so (0.003, 0.004 + 0.006). Read off the DRAWING.
	REQUIRE(f.stage.playedMarkDrawn());
	const visage::Point mark = f.stage.playedMarkAt();
	const visage::Point expected = f.stage.glassAt(0.010, 0.003);
	// AND THE PRIMARY DID NOT MOVE AT ALL — that is the point of having two.
	CHECK(f.stage.starDrawnAt().x == primary.x);
	CHECK(f.stage.starDrawnAt().y == primary.y);
	INFO("rest " << rest.x << "," << rest.y << "; drawn " << mark.x << "," << mark.y << "; expected "
	             << expected.x << "," << expected.y);
	CHECK(mark.x == doctest::Approx(expected.x));
	CHECK(mark.y == doctest::Approx(expected.y));
	// NOT VACUOUS: the expected point is somewhere else on the glass.
	CHECK(std::hypot(mark.x - rest.x, mark.y - rest.y) > 5.0f);

	// NOTHING WAS WRITTEN AND NOTHING FOLLOWED. The authored place to the bit, no crumb, the view
	// where it was on both axes, and no undo record — a modulator is not an edit.
	CHECK(f.bridge.soundTravel().offset[2].x == 0.003);
	CHECK(f.bridge.soundTravel().offset[2].y == 0.004);
	CHECK(f.stage.crumbs(2).empty());
	CHECK(f.stage.viewCentre() == doctest::Approx(viewBefore));
	CHECK(f.stage.xAtTravelX(0.0) == doctest::Approx(xOfFixedTravel));
	CHECK(f.stage.canUndo() == undoBefore);
	CHECK_FALSE(f.stage.viewGlidingForTest());

	// THE KNOBS' MARKS RIDE THE SAME NUMBERS.
	CHECK(f.stage.radiusKnobForTest(2).control()->display().modulated == doctest::Approx(0.006));
	CHECK(f.stage.thetaKnobForTest(2).control()->display().modulated
	      == doctest::Approx(std::numbers::pi * 0.5));
	// AND THE OFFSET ITSELF WAS NOT WRITTEN: a modulator is not an edit.
	CHECK(f.bridge.soundTravel().orbit[2] == PolarOffset { 0.005, 0.0 });

	// A PULL THAT HOLDS STILL IS QUIET, and so is a pull on a sound that is not the one on the chart:
	// the engine publishes every block, and Explore must not repaint for it.
	f.now += 16;
	CHECK_FALSE(f.stage.update());
	pulls.theta[5] = 1.0;
	f.bridge.publishTravelPulls(pulls);
	f.now += 16;
	CHECK_FALSE(f.stage.update());

	// AND A RAMP'S RESET DOES NOT JUMP THE STAR: a whole turn more is the same place on the glass.
	pulls.theta[2] += 2.0 * std::numbers::pi;
	f.bridge.publishTravelPulls(pulls);
	f.now += 16;
	f.stage.update();
	f.stage.draw(canvas);
	CHECK(f.stage.playedMarkAt().x == doctest::Approx(expected.x));
	CHECK(f.stage.playedMarkAt().y == doctest::Approx(expected.y));
}

TEST_CASE("the polar corners leave no crumb and move no view, but are still undoable") {
	// Radius/Theta changes leave no breadcrumbs and do not re-centre the view.
	//
	// BOTH HALVES HAVE ONE REASON. Radius and Theta are the only corners that move the sound
	// CONTINUOUSLY across the plane, so a crumb per gesture is a crumb per knob twiddle — the trail
	// fills with places nobody chose to visit and the ones that meant something fall out of
	// `kMaxCrumbs`. And the view holding still is what makes the knob READABLE: the point of turning
	// Radius is watching the puck travel, which a view that pans to follow it hides completely.
	//
	// THE CONTRAST IS THE CASE. Gamma turns under exactly the same machinery and DOES crumb and DOES
	// re-centre, so asserting the polar corners alone would pass against a build where crumbing had
	// broken generally. Both are driven here, from one fixture, in one case.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(2);
	f.stage.draw(canvas);
	// Somewhere off the origin, so Theta has a radius to swing and the moves are real.
	f.bridge.setSoundTravel(2, { 0.03, 0.04 });
	f.stage.draw(canvas);
	f.settleScrub();

	const auto turn = [&](ExploreStage::Part part, float dy) {
		if (part == ExploreStage::Part::Radius || part == ExploreStage::Part::Theta) {
			dragCorner(f, part, dy); // a child Knob — the press goes to it, not to the stage
			f.settleScrub();
			return;
		}
		const visage::Bounds box = f.stage.partBounds(part);
		const float bx = box.x() + box.width() * 0.5f;
		const float by = box.y() + box.height() * 0.5f;
		f.stage.onMouseDown().callback(at(bx, by));
		f.stage.onMouseDrag().callback(at(bx, by - dy));
		f.stage.onMouseUp().callback(at(bx, by - dy));
		f.settleGesture();
		f.settleScrub();
	};

	// LET THE VIEW FINISH WITH THE FIXTURE'S OWN MOVE FIRST. `setSoundTravel` above put the sound
	// outside the view from OUTSIDE a gesture, and `update()` rightly follows a move like that (the
	// "a move from outside brings the star back" rule). That follow is not the subject here, and left
	// pending it lands during the first knob turn and reads as the knob having re-centred.
	for (int frame = 0; frame < 8; ++frame) {
		f.now += 16;
		f.stage.update();
	}
	f.settleScrub();

	REQUIRE(f.stage.crumbs(2).empty());
	const double viewBefore = f.stage.viewCentre();
	// THE HORIZONTAL HALF OF THE VIEW, read through the projection rather than the private centre:
	// where a fixed travel is DRAWN cannot move unless the view did.
	const float xOfFixedTravel = f.stage.xAtTravelX(0.0);

	// (1) RADIUS: the sound moves, the trail does not grow, the view does not follow.
	turn(ExploreStage::Part::Radius, 60.0f);
	INFO("after Radius: crumbs " << f.stage.crumbs(2).size() << ", view " << f.stage.viewCentre()
	                             << " (was " << viewBefore << ")");
	// NOT VACUOUS: the gesture has to have actually moved the sound, or "no crumb" is a statement
	// about a knob that did nothing.
	REQUIRE(f.stage.selectedRadius() > 0.01);
	CHECK(f.stage.crumbs(2).empty());
	CHECK(f.stage.viewCentre() == doctest::Approx(viewBefore));
	CHECK(f.stage.xAtTravelX(0.0) == doctest::Approx(xOfFixedTravel));

	// (2) THETA: the same, on the other corner.
	const double thetaBefore = f.stage.selectedTheta();
	turn(ExploreStage::Part::Theta, 60.0f);
	REQUIRE(f.stage.selectedTheta() != doctest::Approx(thetaBefore));
	CHECK(f.stage.crumbs(2).empty());
	CHECK(f.stage.viewCentre() == doctest::Approx(viewBefore));
	CHECK(f.stage.xAtTravelX(0.0) == doctest::Approx(xOfFixedTravel));

	// (3) BUT THE HISTORY IS THERE. The requirement covers the trail and the centring, not undo — a knob whose
	// move cannot be walked back would be the worse surprise. Two gestures, two steps back.
	REQUIRE(f.stage.canUndo());
	const double radiusAfter = f.stage.selectedRadius();
	f.stage.undo();
	f.settleGesture();
	CHECK(f.stage.selectedTheta() == doctest::Approx(thetaBefore));
	f.stage.undo();
	f.settleGesture();
	INFO("after two undos: radius " << f.stage.selectedRadius() << " (peaked at " << radiusAfter << ")");
	CHECK(f.stage.selectedRadius() == 0.0); // where it started: nobody had given it a radius

	// (4) THE POSITIVE CONTROL, and it is what stops every check above being green on a broken build:
	// the GAMMA corner, through the same mouse road, still crumbs and still re-centres.
	f.stage.redo();
	f.stage.redo();
	f.settleGesture();
	f.settleScrub();
	const size_t crumbsBeforeGamma = f.stage.crumbs(2).size();
	turn(ExploreStage::Part::Gamma, 60.0f);
	INFO("gamma left " << f.stage.crumbs(2).size() << " crumb(s), was " << crumbsBeforeGamma);
	CHECK(f.stage.crumbs(2).size() > crumbsBeforeGamma);
}

TEST_CASE("the Radius corner sets how far from its place the sound is played, and moves neither the place nor the angle") {
	// **INDEPENDENT, BY DESIGN** (a build where these corners were the polar FORM of the star's
	// place made Radius and Theta follow the puck position; they are meant to be independent of it).
	// The star is where the sound was PUT; Radius is how far from there it is
	// PLAYED. So this corner writes one number and the star's place is not it.
	ExploreFixture f;
	f.selection.select(2);
	f.bridge.setSoundTravel(2, { 0.03, 0.04 });
	f.bridge.setSoundOrbit(2, { 0.0, 1.0 });
	REQUIRE(f.stage.selectedRadius() == 0.0);

	dragCorner(f, ExploreStage::Part::Radius, 70.0f);

	// THE VALUE, INDEPENDENTLY RESTATED. The face is EXPONENTIAL (for finer control at small radii)
	// and the drag runs along it, so half a full travel from the
	// bottom is the MIDDLE of the face: (e^4.2 - 1) / (e^8.4 - 1) of the plane's reach (2), which is 0.0296 —
	// about one disc radius of the chart at x1. A LINEAR knob over the same range would read 1.0 here.
	CHECK(f.stage.selectedRadius()
	      == doctest::Approx(kTravelRadiusMax * (std::exp(4.2) - 1.0) / (std::exp(8.4) - 1.0)));
	CHECK(f.stage.selectedRadius() < 0.03);
	// THE PLACE DID NOT MOVE — exactly, because nothing wrote it.
	CHECK(f.bridge.soundTravel().offset[2].x == 0.03);
	CHECK(f.bridge.soundTravel().offset[2].y == 0.04);
	// NOR DID THE ANGLE, and nobody else's anything.
	CHECK(f.stage.selectedTheta() == 1.0);
	CHECK(f.bridge.soundTravel().orbit[1] == PolarOffset {});
	CHECK(f.bridge.soundTravel().offset[1].x == 0.0);
}

TEST_CASE("the Theta corner turns the sound about ITS OWN PLACE, and changes neither the place nor the radius") {
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(2);
	f.bridge.setSoundTravel(2, { 0.003, 0.004 });
	f.bridge.setSoundOrbit(2, { 0.005, 0.0 });
	f.stage.draw(canvas);
	const visage::Point primary = f.stage.starDrawnAt();
	REQUIRE(f.stage.playedMarkDrawn());
	const visage::Point before = f.stage.playedMarkAt();

	dragCorner(f, ExploreStage::Part::Theta, 70.0f);

	// Half a travel is half of kThetaSpan, and up is anticlockwise — restated from the gearing.
	CHECK(f.stage.selectedTheta() == doctest::Approx(0.5 * ExploreStage::kThetaSpan));
	CHECK(f.stage.selectedRadius() == 0.005);
	CHECK(f.bridge.soundTravel().offset[2].x == 0.003);
	CHECK(f.bridge.soundTravel().offset[2].y == 0.004);

	// AND THE SECOND STAR IS DRAWN ON THE CIRCLE ABOUT THE PLACE — not about the plane's origin, which is what
	// the first build turned it round. Restated independently of `pulledPlace`.
	f.stage.draw(canvas);
	REQUIRE(f.stage.playedMarkDrawn());
	const double theta = 0.5 * ExploreStage::kThetaSpan;
	const visage::Point expected =
	    f.stage.glassAt(0.004 + 0.005 * std::sin(theta), 0.003 + 0.005 * std::cos(theta));
	CHECK(f.stage.playedMarkAt().x == doctest::Approx(expected.x));
	CHECK(f.stage.playedMarkAt().y == doctest::Approx(expected.y));
	CHECK(std::hypot(f.stage.playedMarkAt().x - before.x, f.stage.playedMarkAt().y - before.y) > 5.0f);
	// THE MAIN STAR STAYED ON THE PLACE.
	CHECK(f.stage.starDrawnAt().x == primary.x);
	CHECK(f.stage.starDrawnAt().y == primary.y);
}

TEST_CASE("a Radius drag through zero parks on the sound's place and comes back out along the same angle") {
	// THE ANGLE IS ITS OWN STORED NUMBER NOW, so a radius of zero no longer destroys it — the first
	// build derived both from one point and needed a held angle to survive the origin. What is left to
	// assert is the FLOOR (a negative radius is the same place half a turn round, so the knob parks)
	// and that the angle is simply still there.
	ExploreFixture f;
	f.selection.select(2);
	f.bridge.setSoundOrbit(2, { 0.03, -2.2 });

	f.stage.update();
	Knob& knob = f.stage.radiusKnobForTest(2);
	const float bx = knob.width() * 0.5f;
	const float by = knob.height() * 0.5f;
	knob.processMouseDown(at(bx, by));
	knob.processMouseDrag(at(bx, by + 140.0f)); // through the floor
	CHECK(f.stage.selectedRadius() == 0.0);
	knob.processMouseDrag(at(bx, by - 35.0f)); // and back out, same gesture
	knob.processMouseUp(at(bx, by - 35.0f));
	f.settleGesture();

	CHECK(f.stage.selectedRadius() > 0.0);
	CHECK(f.stage.selectedTheta() == -2.2);
}

TEST_CASE("with no Radius the Theta corner moves nothing on the chart, and the angle it set is there when Radius arrives") {
	// A SOUND PLAYED ON ITS OWN PLACE HAS NOTHING FOR AN ANGLE TO TURN — true, and not a dead knob: the
	// angle is stored, and the moment there is a radius it is the direction the sound leaves in.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(2);
	f.stage.draw(canvas);
	const visage::Point before = f.stage.starDrawnAt();
	REQUIRE(f.stage.selectedRadius() == 0.0);

	dragCorner(f, ExploreStage::Part::Theta, 70.0f);
	f.stage.draw(canvas);

	CHECK(f.stage.selectedTheta() == doctest::Approx(0.5 * ExploreStage::kThetaSpan));
	CHECK(f.stage.starDrawnAt().x == before.x);
	CHECK(f.stage.starDrawnAt().y == before.y);
	// AND NO SECOND STAR: the sound is played where it was put, so there is one thing to draw.
	CHECK_FALSE(f.stage.playedMarkDrawn());
	CHECK_FALSE(f.stage.vectorDrawn());

	// THE POSITIVE CONTROL: give it a radius and it leaves along the angle the corner set — a quarter
	// of a turn is half of `kThetaSpan`'s pi/2... so restate it: straight along (cos, sin) of it.
	f.bridge.setSoundOrbit(2, { 0.005, f.stage.selectedTheta() });
	f.stage.draw(canvas);
	const double theta = 0.5 * ExploreStage::kThetaSpan;
	const visage::Point expected = f.stage.glassAt(0.005 * std::sin(theta), 0.005 * std::cos(theta));
	// NOW THERE ARE TWO THINGS: the second star out along that angle, the main one where it was.
	REQUIRE(f.stage.playedMarkDrawn());
	CHECK(f.stage.playedMarkAt().x == doctest::Approx(expected.x));
	CHECK(f.stage.playedMarkAt().y == doctest::Approx(expected.y));
	CHECK(f.stage.starDrawnAt().x == before.x);
	CHECK(f.stage.starDrawnAt().y == before.y);
}

TEST_CASE("dragging the star turns neither polar corner: the place and the offset are independent") {
	// THE DEFECT, AS A CASE: Radius and Theta must not follow the puck position. In the
	// first build a chart drag changed both knobs, because both were computed from the star.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(2);
	f.bridge.setSoundOrbit(2, { 0.004, 0.7 });
	f.stage.draw(canvas);
	const float radiusFace = f.stage.radiusKnobForTest(2).normalized();
	const float thetaFace = f.stage.thetaKnobForTest(2).normalized();
	const AnchorPoint placeBefore = f.bridge.soundTravel().offset[2];

	// A real chart drag, off both axes.
	const visage::Point c = f.stage.disk().centre();
	const float reach = f.stage.disk().radius() * 0.4f;
	f.stage.onMouseDown().callback(at(c.x, c.y));
	f.stage.onMouseDrag().callback(at(c.x + reach, c.y - reach * 0.5f));
	f.settleGesture();
	f.stage.onMouseUp().callback(at(c.x + reach, c.y - reach * 0.5f));
	f.settleGesture();
	f.settleScrub();

	// NOT VACUOUS: the star's place really moved.
	const AnchorPoint placeAfter = f.bridge.soundTravel().offset[2];
	REQUIRE(std::hypot(placeAfter.x - placeBefore.x, placeAfter.y - placeBefore.y) > 0.001);
	// AND THE CORNERS DID NOT: the stored numbers exactly, and the faces a hand would see.
	CHECK(f.bridge.soundTravel().orbit[2] == PolarOffset { 0.004, 0.7 });
	f.stage.draw(canvas);
	CHECK(f.stage.radiusKnobForTest(2).normalized() == radiusFace);
	CHECK(f.stage.thetaKnobForTest(2).normalized() == thetaFace);
}

TEST_CASE("the two polar corners stack in the bottom-right without overlapping each other or Zoom") {
	// TWO KNOBS IN ONE CORNER'S WIDTH (smaller controls are acceptable here).
	// Geometry rather than eyes, because the failure mode is a hit test that answers for the wrong
	// knob — a hand aiming at Theta swinging the sound out instead.
	ExploreFixture f;
	const visage::Bounds radius = f.stage.partBounds(ExploreStage::Part::Radius);
	const visage::Bounds theta = f.stage.partBounds(ExploreStage::Part::Theta);
	const visage::Bounds zoom = f.stage.partBounds(ExploreStage::Part::Zoom);

	// Stacked: Radius above Theta, and Theta keeps the bottom-right seat Drift had.
	CHECK(radius.y() + radius.height() <= theta.y());
	CHECK(radius.x() == doctest::Approx(theta.x()));
	CHECK(radius.width() == doctest::Approx(theta.width()));
	// Clear of Zoom, which owns the top-right of the same column.
	INFO("zoom bottom " << zoom.y() + zoom.height() << ", radius top " << radius.y());
	CHECK(zoom.y() + zoom.height() < radius.y());

	// AND THE HIT TEST AGREES WITH THE DRAWING, which is the property the geometry is for.
	const auto centreOf = [](const visage::Bounds& b) {
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};
	CHECK(f.stage.hitAt(centreOf(radius)) == ExploreStage::Part::Radius);
	CHECK(f.stage.hitAt(centreOf(theta)) == ExploreStage::Part::Theta);
	CHECK(f.stage.hitAt(centreOf(zoom)) == ExploreStage::Part::Zoom);
}

TEST_CASE("backlog LH: no two corner controls or buttons overlap at any stage size or UI scale, in native pixels") {
	// At some scales Rotate and Theta overlapped Mutate and the undo/redo buttons. The stage's size is
	// what moves: the rails beside it (all four open leave the design canvas a 376-wide stage), the window a host
	// gave, and the UI scale, which divides a window into fewer logical pixels. So every stage size in the sweep, at
	// every rung of the UI-scale ladder (the development display is 1.75), and the claim is made on the NATIVE rectangles.
	//
	// THE DOMAIN, restated from the widgets' own sizes rather than read from the stage: from the square's top to the
	// stage's foot there must be room for Zoom, the polar pair (Radius over Theta) and the button row stacked, and the
	// square must be two corners wide. Below that nothing can be laid out without dropping a control; those sizes are
	// counted, not asserted. The design canvas with every rail open (a 374x539 stage) is inside it.
	ExploreFixture f;
	using P = ExploreStage::Part;
	const float corner = Knob::columnHeight(ExploreStage::kCornerDiameter);
	const float polar = 2.0f * Knob::columnHeight(ExploreStage::kPolarDiameter) + ExploreStage::kPolarGap;
	const float button = ExploreStage::kButtonHeight + ExploreStage::kButtonGap;
	const P parts[] = { P::Gamma, P::Zoom, P::Rotate, P::Radius, P::Theta, P::Mutate, P::Undo, P::Redo };
	const char* names[] = { "Gamma", "Zoom", "Rotate", "Radius", "Theta", "Mutate", "Undo", "Redo" };
	// A GRID, PLUS THE STAGE THE OVERLAP WAS MOST LIKELY SEEN ON: the design canvas with every rail open, 374x539.
	std::vector<float> widths { 374.0f };
	std::vector<float> heights { 539.0f };
	for (float w = 300.0f; w <= 1300.0f; w += 25.0f) {
		widths.push_back(w);
	}
	for (float h = 200.0f; h <= 800.0f; h += 25.0f) {
		heights.push_back(h);
	}
	size_t checked = 0;
	size_t outside = 0;
	bool railsOpenChecked = false;
	for (const double ui : { 1.0, 1.125, 1.25, 1.375, 1.5, 1.75, 2.0 }) {
		const auto scale = static_cast<float>(ui);
		for (const float w : widths) {
			for (const float h : heights) {
				// THE LAYOUT CASCADE'S ORDER: the scale is stamped, then the bounds are set again (`Frame::setDpiScale`
				// leaves every native rectangle where it was; the re-set is what converts it).
				f.stage.setDpiScale(scale);
				f.stage.setBounds(0.0f, 0.0f, w, h);
				const float side = 2.0f * (f.stage.disk().radius() + StageDisk::kRimInset);
				// From the square's top to the stage's foot: Zoom, a gap, the polar pair, a gap and the row. And the
				// square two corners wide.
				const float room = h - (h - side) * 0.5f;
				const bool fits = room >= corner + ExploreStage::kButtonGap + polar + button
				                  && side >= 2.0f * ExploreStage::kCornerWidth + ExploreStage::kButtonGap;
				if (!fits) {
					++outside;
					continue;
				}
				++checked;
				railsOpenChecked = railsOpenChecked || (w == 374.0f && h == 539.0f);
				visage::IBounds native[std::size(parts)];
				for (size_t i = 0; i < std::size(parts); ++i) {
					native[i] = (f.stage.partBounds(parts[i]) * scale).round();
				}
				for (size_t i = 0; i < std::size(parts); ++i) {
					// INSIDE THE STAGE: a control pushed off the frame is not a control. Logical, because rounding x and
					// width separately can put a native edge one pixel past a native frame whose edge it meets exactly.
					if (!f.stage.localBounds().contains(f.stage.partBounds(parts[i]))) {
						const std::string why = std::string(names[i]) + " leaves the stage at " + std::to_string(w) + "x"
						                        + std::to_string(h) + " ui " + std::to_string(ui);
						FAIL_CHECK(why);
					}
					for (size_t j = i + 1; j < std::size(parts); ++j) {
						if (native[i].overlaps(native[j])) {
							const std::string why = std::string(names[i]) + " overlaps " + names[j] + " at stage "
							                        + std::to_string(w) + "x" + std::to_string(h) + " ui " + std::to_string(ui);
							FAIL_CHECK(why);
						}
					}
				}
				// THE POLAR KNOBS ARE FRAMES: their native rectangles must be the parts' at THIS scale, not a stale one.
				const size_t slot = f.selection.slot();
				const visage::Bounds r = f.stage.partBounds(P::Radius) * scale;
				const visage::Bounds t = f.stage.partBounds(P::Theta) * scale;
				CHECK(f.stage.radiusKnobForTest(slot).nativeWidth() == r.round().width());
				CHECK(f.stage.thetaKnobForTest(slot).nativeY() == t.round().y());
			}
		}
	}
	MESSAGE("stage sizes checked " << checked << ", below the domain " << outside);
	CHECK(checked > 1000);
	CHECK(railsOpenChecked);
	f.stage.setDpiScale(1.0f);
}

TEST_CASE("the Radius dial is exponential: a real zero, the plane's reach at the top, and the small radii spread out") {
	// Exponential for finer control at small radii — replacing a vernier that wrapped once per base
	// travel. What is asserted is the intended property: small offsets, where an orbit lives, get most
	// of the face.
	ExploreFixture f;
	f.selection.select(2);
	visage::Canvas canvas;
	const auto faceAt = [&](double radius) {
		f.bridge.setSoundOrbit(2, { radius, 0.0 });
		f.stage.draw(canvas);
		return f.stage.radiusKnobForTest(2).normalized();
	};

	CHECK(faceAt(0.0) == doctest::Approx(0.0)); // a magnitude has a floor and the face shows it
	CHECK(faceAt(kTravelRadiusMax) == doctest::Approx(1.0));
	// HALF THE FACE IS SPENT BELOW ABOUT ONE DISC RADIUS OF THE CHART AT x1 (0.031) — restated from the
	// curve: ln(1 + r / 2 (e^8.4 - 1)) / 8.4 (the top is the whole plane, 2, since radius-mod-jump).
	CHECK(faceAt(0.0296) == doctest::Approx(0.5).epsilon(0.01));
	// AND 0.001 IS ALREADY A TENTH OF THE FACE. Linear, it would be 0.0005.
	CHECK(faceAt(0.001) == doctest::Approx(std::log(1.0 + 0.001 / 2.0 * (std::exp(8.4) - 1.0)) / 8.4));
	CHECK(faceAt(0.001) > 0.1f);
	// Monotone all the way, so no two radii share a pointer.
	CHECK(faceAt(0.2) < faceAt(0.6));
	CHECK(faceAt(0.6) < faceAt(0.9));
}

TEST_CASE("the radius corner writes the selected sound's offset, through state and not through a parameter") {
	ExploreFixture f;
	f.selection.select(3);
	dragCorner(f, ExploreStage::Part::Radius, 35.0f);

	// Upward is more, on the sound the selection names and no other.
	CHECK(f.bridge.soundTravel().orbit[3].radius > 0.0);
	CHECK(f.bridge.soundTravel().orbit[2].radius == 0.0);
	CHECK(f.stage.selectedRadius() == f.bridge.soundTravel().orbit[3].radius);
	// State rather than an id — the road drift takes — so nothing reaches the parameter queue.
	CHECK(drain(f.bridge).empty());
}

TEST_CASE("the gamma corner carries the coarse half and leaves the vernier to the host") {
	// THE VERNIER ARRANGEMENT, from this surface's side. The corner used to drive the per-sound Gamma
	// PARAMETER, which meant the one control for "take this sound further along its own line" stopped
	// at ±4 — and that parameter has an id, a range a host draws, and an automation lane. So the
	// parameter stays put as the fine control and the corner accumulates into the unbounded base
	// beside it (sound_gamma.h). The split is the point: a corner that still wrote the parameter would
	// be fighting the host over one number.
	ExploreFixture f;
	f.selection.select(2);
	const double vernierBefore = f.read(static_cast<ParamId>(soundParam(2, SoundField::Gamma)));
	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Gamma);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 35.0f));

	// Upward is more, on the sound the selection names and no other.
	CHECK(f.bridge.soundGamma().base[2] > 0.0);
	CHECK(f.bridge.soundGamma().base[1] == doctest::Approx(0.0));
	CHECK(f.stage.selectedGammaBase() == doctest::Approx(f.bridge.soundGamma().base[2]));

	f.stage.onMouseUp().callback(at(centre.x, centre.y - 35.0f));
	// NOTHING REACHED THE PARAMETER QUEUE. The base is state rather than an id — the same road drift
	// takes — so a host automating Gamma is untouched by any of this.
	CHECK(drain(f.bridge).empty());
	CHECK(f.read(static_cast<ParamId>(soundParam(2, SoundField::Gamma)))
	      == doctest::Approx(vernierBefore));
	// The patch is dirty, though, or a carried sound would silently fail to save.
	CHECK(f.bridge.takeStateDirty());
}

TEST_CASE("the gamma corner's readout and face are the whole gamma, not half of it") {
	// What the number has to answer is "where is this sound along its line", and that is base plus
	// vernier. A readout of the base alone would sit still while a host automated the vernier under
	// it; a readout of the vernier alone is the one that used to stop at 4 while the sound went on
	// moving.
	ExploreFixture f;
	f.selection.select(2);
	f.stage.nudgeSelectedGamma(6.0);
	CHECK(f.stage.selectedGammaBase() == doctest::Approx(6.0));
	// PAST THE VERNIER'S WHOLE RANGE, which is the thing the old corner could not reach at all.
	CHECK(f.stage.selectedGammaBase() > 4.0);

	// The face is a compass, so it wraps rather than pinning at an end — an unbounded travel has no
	// end to point at. One full turn is the vernier's range.
	CHECK(ExploreStage::gammaDialPosition(0.0) == doctest::Approx(0.5));
	CHECK(ExploreStage::gammaDialPosition(ExploreStage::kGammaTurn)
	      == doctest::Approx(ExploreStage::gammaDialPosition(0.0)));
	// And it wraps in BOTH directions, which `std::fmod` does not — the lesson the rotate dial paid
	// for, applied here before it could be paid for twice.
	for (const double gamma : { -3.0, -100.0, 250.0 }) {
		CHECK(ExploreStage::gammaDialPosition(gamma) >= 0.0f);
		CHECK(ExploreStage::gammaDialPosition(gamma) <= 1.0f);
	}
}

TEST_CASE("the view holds still under a scrub and re-centres when it ends") {
	ExploreFixture f;
	f.selection.select(1);
	const visage::Point centre = f.stage.disk().centre();
	const double before = f.stage.viewCentre();

	f.stage.onMouseDown().callback(at(centre.x, centre.y - 30.0f));
	f.settleGesture();
	// THE GROUND DOES NOT MOVE UNDER THE HAND. A view that followed the star frame by frame would ask
	// for a new terrain every frame, and at 31 ms a build the map would never finish.
	CHECK(f.stage.viewCentre() == doctest::Approx(before));

	f.stage.onMouseUp().callback(at(centre.x, centre.y - 30.0f));
	f.settleGesture();
	// backlog GE: the close re-centres by PANNING now, so let the view land before reading it.
	f.settleScrub();
	CHECK(f.stage.viewCentre() != doctest::Approx(before));
}

TEST_CASE("how far an automated follow moves the view is one constant, and centring is a value of it") {
	// A29's policy, as arithmetic. The view follows automated IDENTITY and never performance (A32), and
	// the only question the decision left open was HOW FAR each follow moves — because a single automated
	// sweep fires this repeatedly (ui-10 counted seven in one test gesture) and every move uncovers
	// ground that has to be built before it can be shown.
	//
	// THE IDENTITY THAT MAKES THE DECISION ONE CONSTANT: a star that has JUST left the edge is half a view
	// from the centre, so a follow to margin `m` moves the view by exactly `m * span` — half a view at
	// 0.5, which is centring and what ships, and 15% at 0.15, which is the alternative. That is where
	// ui-10's measured 38% morph / 51% stair strip comes from, and what a flip would buy.
	constexpr double kSpan = 0.0626; // 64 zones, the x1 view
	for (const double margin : { 0.5, 0.15, 0.0 }) {
		for (const bool throughTop : { true, false }) {
			const double view = 3.25;
			// The moment the follow fires: the star is exactly on the edge it is leaving.
			const double star = throughTop ? view + kSpan * 0.5 : view - kSpan * 0.5;
			const double settled = ExploreStage::followCentreFor(star, kSpan, throughTop, margin);
			CAPTURE(margin);
			CAPTURE(throughTop);
			CHECK(std::abs(settled - view) == doctest::Approx(kSpan * margin));
			// And the star ends up `margin` of a view inside the edge it left — the property the number
			// is chosen for, rather than a restatement of the line above.
			const double edge = throughTop ? settled + kSpan * 0.5 : settled - kSpan * 0.5;
			CHECK(std::abs(edge - star) == doctest::Approx(kSpan * margin));
		}
	}

	// CENTRING IS margin 0.5 EXACTLY, not approximately: the inset is `span * (0.5 - margin)`, a hard
	// zero there, so the follow returns the star's own home for every span and both edges. That is what
	// lets the shipped behaviour be a VALUE of the new mechanism instead of a preserved second branch.
	for (const double span : { 0.0626, 3.0, 0.001 }) {
		for (const bool throughTop : { true, false }) {
			CHECK(ExploreStage::followCentreFor(1.75, span, throughTop, 0.5) == 1.75);
		}
	}

	// THE SHIPPED DECISION, PINNED. The view is CENTRED on the active point by design, so centring keeps
	// the default and the strip is its price. Flipping kFollowMargin to 0.15 is the A/B, and it has to
	// come here to say so — which is the whole point of pinning it.
	CHECK(ExploreStage::kFollowMargin == 0.5);
}

TEST_CASE("a move from outside brings the star back, and gamma no longer moves the view at all") {
	// THE ROAD WITH NO GESTURE TO END. Something moving the sound from outside this surface is not an
	// Explore gesture, so `grabbed_` stays None and `update()`'s star-left-the-view rule is the only
	// thing keeping the chart pointed at the sound. Driven through the real road rather than by
	// calling the follow directly, so the branch under test is the one that actually runs.
	//
	// **THE DRIVER CHANGED BECAUSE THE OLD ONE STOPPED MOVING THE STAR** (backlog GY). It set Zone,
	// and under the one-value design Zone is consumed by the DECODE rather than being an axis — it selects which
	// plane is drawn, not where on the plane the sound stands. So a host sweeping Zone now redraws the
	// map with the puck untouched, and cannot trigger this rule at all. The remaining road that moves
	// a sound without a gesture is a write to its travel, which is what an undo, a crumb retrace or
	// another surface does, so that is what drives it.
	ExploreFixture f;
	f.selection.select(0);
	f.set(ParamId::AZone, 0.0);
	// SETTLE THE VIEW FIRST, or there is nothing for the star to leave: until a view has been taken,
	// viewCentre() derives itself from the selected sound every time it is asked, so the star is
	// permanently centred and the trigger's condition can never be met. A draw is what settles it in
	// the shipped editor, so a draw is what settles it here.
	visage::Canvas canvas;
	f.stage.draw(canvas);
	const double before = f.stage.viewCentre();
	const float centreY = f.stage.disk().centre().y;

	// Far enough that the star is off the chart, which is the trigger's own condition.
	f.bridge.setSoundTravel(0, { 0.0, f.stage.travelSpan() * 8.0 });
	REQUIRE((f.stage.yAtTravelY(f.stage.selectedDriftY()) < 0.0f
	         || f.stage.yAtTravelY(f.stage.selectedDriftY()) > f.stage.height()));
	REQUIRE(f.stage.update());
	CHECK(f.stage.viewCentre() != doctest::Approx(before));

	// AT THE SHIPPED MARGIN THE STAR COMES TO REST CENTRED, asserted on where it is DRAWN rather than on
	// the arithmetic that put it there — the lesson from the placement defect, which measured a full
	// buffer while the picture sat off the widget.
	CHECK(f.stage.yAtTravelY(f.stage.selectedDriftY()) == doctest::Approx(centreY).epsilon(0.02));

	// **AND GAMMA NO LONGER MOVES THE VIEW — THE INVERSION, AND IT RETIRES A WHOLE CLASS OF BUG.**
	// This asserted `settled + 1.25`: the view followed gamma one for one, because the reachable band
	// was `[gammaSum, gammaSum + 3]` and a view that held still while gamma moved ended up outside the
	// world with nothing to draw (ui-10's "gamma blank"). Every compensation that followed — the
	// baseline recorded at the settle, the two counters cancelling a hand's own motion — existed to
	// keep that follow honest.
	//
	// Under the one-value design the chart's axes are displacements in the decoded plane and gamma is not on
	// either of them, so there is no band to fall off and nothing to follow. The view holds ABSOLUTELY
	// still through a global gamma move, and the blank it used to chase is unreachable rather than
	// compensated for.
	const double settled = f.stage.viewCentre();
	f.set(ParamId::GlobalGamma, 1.25);
	INFO("view " << settled << " -> " << f.stage.viewCentre() << " across a 1.25 global gamma move");
	CHECK(f.stage.viewCentre() == doctest::Approx(settled));
	// AND THE STAR DID NOT MOVE EITHER, so "the view held still" is not the view and the sound having
	// drifted together — which is the shape the old follow had, and is what this must not become.
	CHECK(f.stage.yAtTravelY(f.stage.selectedDriftY()) == doctest::Approx(centreY).epsilon(0.02));
}

TEST_CASE("every deliberate move centres the star, on BOTH axes, at the pixel it is drawn") {
	// Two symptoms: centring on a manual move did not work, and a drag could hit the edge of the
	// plane, which should be impossible. ONE CAUSE: the chart had a vertical view centre and
	// NO HORIZONTAL ONE — `xAtTravelX` was pinned to drift 0 — so recentre() centred on the point's HOME,
	// which is one of the point's two coordinates.
	//
	// WHY THE OLD CASES MISSED IT, which is the reason this one is written the way it is: every
	// assertion in them was on the y axis, and the #18 audit checked that recentre() is CALLED on each
	// move road rather than whether it centres on the POINT. Verifying the call is not verifying the
	// claim. So this drives the REAL widget down every deliberate road and asserts the DRAWN pixel on
	// both axes — the only statement that could not have been satisfied by the broken build.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point centre = f.stage.disk().centre();

	const auto starIsCentred = [&](const std::string& road) {
		f.stage.draw(canvas);
		const float x = f.stage.xAtTravelX(f.stage.selectedDrift());
		const float y = f.stage.yAtTravelY(f.stage.selectedDriftY());
		INFO("after " << road << ": star drawn at " << x << "," << y << " chart centre " << centre.x
		               << "," << centre.y);
		CHECK(x == doctest::Approx(centre.x).epsilon(0.02));
		CHECK(y == doctest::Approx(centre.y).epsilon(0.02));
	};

	// A CHART SCRUB THAT MOVES BOTH COORDINATES AT ONCE — diagonal on purpose, because a scrub writes
	// both travel coordinates together and a case that only ever moved one of them is how this was
	// missed. Under backlog GY the two are now the SAME KIND of number (both displacements in the
	// decoded plane), which makes "centres on the POINT rather than on one of its coordinates" a
	// stronger claim than it was when the axes were different animals.
	f.stage.onMouseDown().callback(at(centre.x + 90.0f, centre.y - 70.0f));
	f.stage.onMouseDrag().callback(at(centre.x + 140.0f, centre.y - 110.0f));
	f.stage.onMouseUp().callback(at(centre.x + 140.0f, centre.y - 110.0f));
	f.settleGesture();
	// backlog GE: the close re-centres by PANNING now, so let the view land before reading it.
	f.settleScrub();
	// THE RECENTRE IS PART OF THE GESTURE'S CLOSE, and since the rate cap a released drag is still
	// coasting when the button comes up — so the close, and the recentre with it, happens when the star
	// ARRIVES. Waiting for that is waiting for the thing this case is about to have happened.
	f.settleScrub();
	starIsCentred("a diagonal scrub");

	// **THE POLAR CORNERS ARE NO LONGER ON THIS LIST** (they deliberately do not re-centre the
	// view). They were, as "the road that strands a star", and the centring is what used to stop a
	// Radius drag putting the sound where no gesture could reach it.
	//
	// WHAT REPLACES THE GUARANTEE is that the corner is its own way back: turning Radius down walks
	// the sound home along the same ray it left on, so a sound off the chart is off it because you
	// sent it there and one gesture returns it. The case below this one asserts exactly that, which
	// is why removing the corner here does not remove the protection — it moves it to where it is
	// now true.
	//
	// Gamma still centres and is still driven here, so "every deliberate move centres the star" keeps
	// a road under test rather than becoming a claim about one gesture.
	const auto turnCorner = [&](ExploreStage::Part part, float pixels) {
		const visage::Bounds box = f.stage.partBounds(part);
		const float x = box.x() + box.width() * 0.5f;
		const float y = box.y() + box.height() * 0.5f;
		f.stage.onMouseDown().callback(at(x, y));
		f.stage.onMouseDrag().callback(at(x, y - pixels));
		f.stage.onMouseUp().callback(at(x, y - pixels));
		f.settleGesture();
		// backlog GE: the close re-centres by PANNING now, so let the view land before reading it.
		f.settleScrub();
	};
	turnCorner(ExploreStage::Part::Gamma, 40.0f);
	starIsCentred("a gamma-corner drag");

	// AND THE HISTORY VERBS, which #18 asked for by name.
	f.stage.undo();
	f.settleGesture();
	starIsCentred("undo");
	f.stage.redo();
	f.settleGesture();
	starIsCentred("redo");
}

TEST_CASE("a drag keeps going past the old glass, in both directions, without losing the star") {
	// The symptom: a drag could still pan to the edge of the plane — dragging the star, the
	// terrain stopped and the point could not move any further.
	//
	// THE MECHANISM, MEASURED, AND NOT THE ONE I FIRST WROTE DOWN: the VALUE was never walled.
	// travelXAtScreenX extrapolates past the window, and one long drag on the shipped build wrote drift out to
	// 0.109 — three and a half visible widths. What stopped was the PICTURE: the view is held for the
	// whole grab, so past the edge the star was drawn at x=1333 of a 166..700 chart with the terrain
	// gone from under it, and the surface stopped answering the hand. Release re-centred and let you
	// drag again, which is a ratchet rather than a chart.
	//
	// So the claim is BOTH halves at once, and neither alone would have caught it: the point keeps
	// moving AND it stays on the chart, within one continuous gesture.
	for (const float direction : { 1.0f, -1.0f }) {
		ExploreFixture f;
		visage::Canvas canvas;
		f.selection.select(1);
		f.stage.draw(canvas);
		const visage::Point c = f.stage.disk().centre();
		const double spanAtStart = f.stage.travelSpan();
		const double driftAtStart = f.stage.selectedDrift();

		f.stage.onMouseDown().callback(at(c.x, c.y));
		for (int px = 20; px <= 600; px += 20) {
			f.stage.onMouseDrag().callback(at(c.x + direction * static_cast<float>(px), c.y));
			f.settleGesture();
			// A DRAG IS THROTTLED BY TIME SINCE 2026-09-17 (`kViewsPerSecond`), so the hand in a case has to take some:
			// with the fixture's clock held still, a drag asks for travel in no time at all and rightly gets none.
			f.now += 60;
			f.stage.update();
		}

		const double moved = std::abs(f.stage.selectedDrift() - driftAtStart);
		const ExploreStage::InkBox chart = f.stage.chartBox();
		const float starX = f.stage.xAtTravelX(f.stage.selectedDrift());
		INFO("direction " << direction << ": drift moved " << moved << " (one whole view is "
		                  << spanAtStart << "), star at x=" << starX << " chart " << chart.left << ".."
		                  << chart.right);
		// It went somewhere no single drag could have SHOWN before: further than one whole view.
		CHECK(moved > spanAtStart);
		// And it is still on the chart, mid-gesture, with the hand still down.
		CHECK(starX > chart.left);
		CHECK(starX < chart.right);
		f.stage.onMouseUp().callback(at(c.x + direction * 600.0f, c.y));
		f.settleGesture();
	}
}

TEST_CASE("dragging up does not stop — not at the glass, and no longer at a world's edge either") {
	// RESTATED, AND ITS PREMISE IS WHAT THE DESIGN CHANGE DELETED. The original said the vertical axis is
	// GENUINELY BOUNDED — home is `zone/1023 + offset + gamma` with zone in [0,1023] and offset in
	// [0,2], so the band is 3 wide and a scrub ran out of world — and it pinned WHICH edge a player
	// meets: the world's, drawn, rather than the picture jamming against the glass. That was the right
	// claim while the chart wrote Zone and Offset. But the edge was reachable at the widest zoom, and the
	// world is meant to be infinite, so the edge was removed — one value, the base absorbs it, the way
	// gamma sums into the wave index on the hardware.
	//
	// So the case keeps its subject (where does a long upward drag END UP) and inverts its answer, and
	// the defect it still guards is the ORIGINAL one: a drag whose picture jams against the glass while
	// the value goes somewhere the player cannot see. Both halves are asserted after a drag far longer
	// than the old band.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(0);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	const double startY = f.stage.selectedDriftY();
	// **WHAT THE OLD BOUNDED AXIS COULD ADDRESS**, kept as the yardstick even though the axis it
	// bounded is gone. Under backlog GY the vertical axis is a displacement in the decoded plane and
	// was never bounded at all — so the claim "a drag does not stop at the old edge of the world" is
	// now true for a second, better reason, and this number is here to prove the drag went genuinely
	// far rather than to mark an edge anybody could still hit.
	const double oldCeiling = homeCeiling(0.0);
	// ZOOMED OUT FIRST, SINCE THE DRAG IS THROTTLED (`kViewsPerSecond`, 2026-09-17): a drag carries the world at a
	// bounded number of VIEWS per second, so reaching the far country is a question of how big a view is. At the widest
	// zoom one view is the whole of the old band, and the seconds below are enough to cross it several times over. The
	// claim is unchanged — a drag does not stop at the old edge of the world — and it is now made at a speed a hand and
	// a terrain builder can both keep up with.
	f.stage.setZoom(ExploreStage::minZoom());
	f.stage.draw(canvas);

	f.stage.onMouseDown().callback(at(c.x, c.y));
	for (int px = 20; px <= 1200; px += 20) {
		f.stage.onMouseDrag().callback(at(c.x, c.y - static_cast<float>(px)));
		f.settleGesture();
		// THE VIEW FOLLOWS AT A BOUNDED SPEED SINCE 2026-09-17 (`kViewsPerSecond`), so a drag that outruns it arrives
		// over the frames after it rather than inside the event. Time passes here, as it does under a hand.
		f.now += 40;
		f.stage.update();
	}

	// IT WENT PAST THE OLD END OF THE WORLD AND KEPT GOING. The old ceiling is now just a number the
	// drag passed through: nothing about it is visible in where the sound ended up.
	INFO("Y travel " << startY << " -> " << f.stage.selectedDriftY() << ", the old band's ceiling "
	                 << oldCeiling);
	CHECK(f.stage.selectedDriftY() > oldCeiling);
	// **AND IT SPENT NO GAMMA AND NO DIALS GETTING THERE — the design, inverted.** The old case said
	// the whole distance landed on the gamma base; under GY a vertical drag does not touch gamma at
	// all, so the base holding STILL across a 1200px drag is the assertion that says the axis really
	// did change rather than merely being read through a different accessor.
	CHECK(f.stage.selectedGammaBase() == doctest::Approx(0.0));
	CHECK(f.read(ParamId::AZone) == doctest::Approx(0.0));
	CHECK(f.read(ParamId::AOffset) == doctest::Approx(0.0));

	// AND THE STAR IS STILL ON THE CHART, mid-gesture, with the hand still down — the half of the
	// original claim that survives the design change word for word. A value that travels while the picture
	// jams against the glass is the defect this has always been about.
	const float top = c.y - f.stage.disk().radius();
	const float starY = f.stage.yAtTravelY(f.stage.selectedDriftY());
	INFO("star drawn at y=" << starY << ", chart top at " << top);
	CHECK(starY > top);
	CHECK(starY < c.y + f.stage.disk().radius());
	f.stage.onMouseUp().callback(at(c.x, c.y - 1200.0f));
	f.settleGesture();
}

TEST_CASE("a drag out past the old world and back leaves the sound exactly as it was") {
	// THE PROPERTY THAT CHOSE THE DESIGN, and it is worth stating why it was ever in doubt. The
	// rejected spill design let a drag move zone and offset inside the band and hand only the OVERSHOOT
	// to gamma, which accumulates on the way out and does not unwind on the way back: home returned but
	// the SPLIT did not, and PhiStair's family is `min(7, zone >> 7)` — a function of the split — so a
	// round trip came back a different sound. Traced before it was built: zone 511 (family 3) out and
	// back as zone 0 (family 0). Under the one-value design the split cannot drift because the split
	// never moves: zone and offset are untouched and the base absorbs everything.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	const SoundIdentity start = f.stage.identity();
	const double startY = f.stage.selectedDriftY();

	// ZOOMED OUT AND WITH TIME PASSING, since a drag carries the world at a bounded number of VIEWS per second
	// (`kViewsPerSecond`, 2026-09-17). The round trip is the claim; how long a hand takes over it is not.
	f.stage.setZoom(ExploreStage::minZoom());
	f.stage.draw(canvas);

	f.stage.onMouseDown().callback(at(c.x, c.y));
	for (int px = 20; px <= 1600; px += 20) {
		f.stage.onMouseDrag().callback(at(c.x, c.y - static_cast<float>(px)));
		f.settleGesture();
		f.now += 40;
		f.stage.update();
	}
	// It really did leave the old world, or the return trip proves nothing.
	INFO("dragged out to Y " << f.stage.selectedDriftY() << ", the old band's ceiling "
	                         << homeCeiling(0.0));
	REQUIRE(f.stage.selectedDriftY() > homeCeiling(0.0));

	// BACK TO THE PIXEL THAT NOW ADDRESSES WHERE IT STARTED — which is a DIFFERENT pixel, because the
	// edge-follow panned the view under the hand on the way out. Saying that plainly is the honest form
	// of the claim: what returns is the sound, not the pointer. A case that dragged back to the
	// starting pixel would be asserting that the view never moved, which is a different (and false)
	// AND BACK, AT THE SAME BOUNDED SPEED: the return is as many steps as the way out, so the case gives it as much
	// time. A single event cannot carry it home any more, which is the throttle doing its job.
	for (int step = 0; step < 120; ++step) {
		f.stage.onMouseDrag().callback(at(c.x, f.stage.glassAt(startY, f.stage.selectedDrift()).y));
		f.settleGesture();
		f.now += 40;
		f.stage.update();
		if (f.stage.selectedDriftY() == doctest::Approx(startY).epsilon(0.0001)) {
			break;
		}
	}
	f.stage.onMouseUp().callback(at(c.x, f.stage.glassAt(startY, f.stage.selectedDrift()).y));
	f.settleGesture();
	f.settleGesture();

	INFO("Y travel " << startY << " -> " << f.stage.selectedDriftY());
	CHECK(f.stage.selectedDriftY() == doctest::Approx(startY).epsilon(0.001));
	// AND THE SPLIT NEVER MOVED AT ALL — the half the spill design could not promise. Everything that
	// decides which of the eight stair families this sound is, is exactly where it started.
	CHECK(f.stage.identity().zone == doctest::Approx(start.zone));
	CHECK(f.stage.identity().offset == doctest::Approx(start.offset));
	CHECK(f.stage.identity().gamma == doctest::Approx(start.gamma));

	// AND THE ARITHMETIC IS EXACT, which pixels cannot show: a pointer round trip goes through a float
	// pixel and comes back within a rounding of it. Asked of the verb directly, the same coordinate in
	// gives the identical place out.
	//
	// **THIS USED TO ASK `scrubTo`, AND THAT VERB IS NOW GONE** (backlog GY). It edited HOME by moving
	// the gamma base, which is what the vertical axis used to do; when the axis became a displacement
	// in the decoded plane, `scrubTo` lost its last caller in `src/` and this line was the only thing
	// left referring to it — a dead verb kept alive by a stale case, which is how dead code survives
	// a refactor. The travel write is exact for the same reason the old one was: it SOLVES rather
	// than accumulates, so asking twice for the same place writes the same place.
	f.stage.driftToXY(f.stage.selectedDrift(), startY);
	f.settleGesture();
	CHECK(f.stage.selectedDriftY() == doctest::Approx(startY));
	// AND THE IDENTITY IS STILL UNTOUCHED, which under the one-value design is now true by construction rather
	// than by arithmetic — the chart writes no identity field at all. Kept because "by construction"
	// is a claim about today's code, and this is what would notice it stopping being true.
	CHECK(f.stage.identity() == start);
}

TEST_CASE("the star goes where the finger goes, and the world holds still under it") {
	// THE HALF OF THE DESIGN THAT IS ABOUT FEEL. The model of this gesture is panning to the edge of
	// the plane — a point that travels across a world that stays where it is — and the opposite feel
	// is a view that chases the sound at exactly the speed the sound is moving, which pins the star to
	// the glass and slides the terrain past underneath it. That is the corner encoder's feel, and it is
	// not this one.
	//
	// **THE MECHANISM THIS GUARDED IS GONE, AND THE CLAIM SURVIVED IT** (backlog GY). The danger used
	// to be a drag writing gamma while `viewCentre()` followed gamma one for one — one hand counted
	// twice — and the guard was three lines of compensation in `scrubTo`. Under the one-value design the chart's
	// vertical axis is a displacement in the decoded plane, `viewCentre()` follows nothing, and
	// `scrubTo` does not exist. So the case now asserts the same feel against a design in which the
	// old failure is unreachable — which is worth keeping precisely because "unreachable" is a
	// statement about today's code.
	//
	// So: IN THE MIDDLE OF THE CHART, where the edge-follow has nothing to say, the star is drawn under
	// the pointer, frame by frame. KNOWN-BAD: make `viewCentre()` follow the selected sound's Y and the
	// star stays pinned at the press point while the finger walks away from it.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	const double centreBefore = f.stage.viewCentre();

	f.stage.onMouseDown().callback(at(c.x, c.y));
	float worst = 0.0f;
	for (int px = 15; px <= 150; px += 15) {
		const float pointerY = c.y - static_cast<float>(px);
		// AND THE HAND MOVES AT A HAND'S SPEED. Since the rate cap (`kViewsPerSecond`), "the star is
		// under the pointer" is a claim about a hand the terrain can answer for — 15 px per frame at
		// this pace is well inside the cap, so the star still lands exactly on the finger, which is
		// what this case is about. A pointer that teleports with the clock stopped is not a slower
		// version of the same gesture; it is an infinitely fast one, and the case above this one is
		// the one that says what happens then.
		f.now += 100;
		f.stage.onMouseDrag().callback(at(c.x, pointerY));
		f.settleGesture();
		f.stage.draw(canvas);
		// WHERE THE STAR IS DRAWN, which since the bubble is not `yAtTravelY` — that answers about the
		// sheet, and the sheet is not what you are looking at. This is the projected position, and
		// asserting it against the pointer is now a claim about BOTH directions of the map at once: the
		// press flattened the pointer, the draw projects it back, and only a true inverse lands.
		const float starY = f.stage.glassAt(f.stage.selectedDriftY(), f.stage.selectedDrift()).y;
		worst = std::max(worst, std::abs(starY - pointerY));
		INFO("at " << px << "px the star is drawn at y=" << starY << " and the pointer is at "
		           << pointerY);
		CHECK(starY == doctest::Approx(pointerY).epsilon(0.002));
	}
	INFO("worst star-to-pointer gap over the drag: " << worst << "px");
	CHECK(worst < 1.0f);

	// AND THE GROUND DID NOT MOVE. This is the same claim from the other side: the view is untouched by
	// a drag that stays clear of the edges, so what the eye sees travelling is the star and not the
	// world. A15's rule, and it outlived the axis it was written about: the scrub writes the plane's
	// Y now rather than gamma, and the claim is word for word the same.
	CHECK(f.stage.viewCentre() == doctest::Approx(centreBefore));
	f.stage.onMouseUp().callback(at(c.x, c.y - 150.0f));
	f.settleGesture();
}

TEST_CASE("the chart draws the sound's own neighbourhood, and holds still under a drag") {
	// THE WIDGET-LEVEL HALF OF THE MAP'S NEW DECOMPOSITION, and it is about the WIRING that the
	// map-level cases cannot see: a builder that decomposes correctly still draws a stranger's country
	// if the surface hands it the wrong dials. That failure has cost this project an install three
	// times — the per-sound pin, the residue, the travel centre — always with the same shape, a window
	// keyed or filled without something the picture depends on, and always looking like a dead verb.
	//
	// AND THE SECOND HALF IS ABOUT FLICKER. A drag moves the star across the plane without changing
	// which plane it is — so a whole drag asks for the map already on screen. It must therefore build
	// NOTHING: the terrain under a scrub is meant to hold perfectly still while the star travels
	// across it, and a rebuild per drag event would put a carry-shift and a snap-back into exactly the
	// gesture that needs stillness.
	//
	// (It said "now that a vertical drag writes gamma". Backlog GY made that false — gamma is a
	// content input the decode consumes and no drag writes it — and the stillness claim is unchanged
	// and now holds for a plainer reason than the one it was written for.)
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	// PhiStair, because it is the engine where drawing the wrong zone is AUDIBLE rather than invisible:
	// the family comes from the raw dial and nothing else.
	f.set(ParamId::OscType, 1.0);
	f.set(ParamId::BZone, 384.0); // family 3
	f.set(ParamId::BOffset, 0.0);
	REQUIRE(stairFamilyAt(384) == 3);
	f.stage.draw(canvas);

	const auto settled = [&]() -> std::shared_ptr<const TerrainSnapshot> {
		// **THIS BOUND ASSERTS NOTHING, AND THAT IS THE WHOLE POINT OF ITS SIZE.** Nothing in this case
		// claims the terrain settles within any particular time; the claims below are about what the
		// map CONTAINS. The loop exists only to turn "hang forever" into "fail", so the number wants to
		// be large enough for the slowest venue this case runs in and no cleverer than that.
		//
		// IT USED TO BE 8000, MEASURED IN THE ORDINARY BUILD AND THEN SPENT UNDER ASAN — the exact
		// mistake its own comment warned about. That comment recorded 232/471 ms at JOBS=1 and
		// 356/809 at JOBS=8, called 8 s "~10x the worst seen in the ordinary build", and added that
		// under ASan at JOBS=4 it would be "the first to watch". It was right, and it still went red
		// there on 2026-09-20: `REQUIRE( after )` reading 0, inside `sanitize-gui-widgets-explore` at
		// jobs=4, while the same case was green 6/6 in an ordinary build, 4/4 under ASan alone, and
		// 4/4 on the branch under ASan alone. Only the combination tripped it.
		//
		// A NUMBER MEASURED IN ONE VENUE AND SPENT IN ANOTHER IS NOT A MEASUREMENT. ASan is several
		// times slower by construction and jobs=4 adds contention on top; a headroom computed without
		// either is headroom against the wrong world. So the bound is now sized for the slow venue,
		// and it costs nothing in the fast one: the loop returns the instant the condition holds, which
		// is about 1.5 s for the whole case at jobs=1.
		//
		// EXCLUDED BEFORE IT WAS RAISED, because "make the timeout bigger" is what someone does when
		// they have not looked: the release's re-centre became a PAN rather than a teleport that day
		// (backlog GE), which would have been a fine explanation. Measured with the pan on and off over
		// three runs each: 1547/1584/1606 ms against 1588/1636/1588. Identical within noise. The pan is
		// not what made this tight, and the case is not waiting on the view.
		constexpr int kGiveUpMs = 60000;
		std::shared_ptr<const TerrainSnapshot> last;
		for (int waited = 0; waited < kGiveUpMs; ++waited) {
			const std::shared_ptr<const TerrainSnapshot> s = f.terrain.snapshot();
			if (s->rows == TerrainMap::kResolution
			    && s->window.travelYCentre == doctest::Approx(f.stage.viewCentre())
			    && s->window.engine == OscType::Stair) {
				return s;
			}
			last = s;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		// AND IT SAYS WHAT IT GAVE UP ON. The previous version failed as `REQUIRE( 0 )`, which names
		// neither the condition that was not met nor how close it got — so the next person to see it
		// starts from nothing, as I did.
		MESSAGE("settled() gave up after " << kGiveUpMs << " ms: rows "
		                                   << (last ? last->rows : -1) << " of "
		                                   << TerrainMap::kResolution << ", window home "
		                                   << (last ? last->window.travelYCentre : 0.0) << " against view "
		                                   << f.stage.viewCentre() << ", engine "
		                                   << (last && last->window.engine == OscType::Stair ? "Stair"
		                                                                                     : "not Stair"));
		return {};
	};

	const std::shared_ptr<const TerrainSnapshot> before = settled();
	REQUIRE(before);
	// The surface handed over the sound's own dials...
	CHECK(before->window.zoneBaseline == doctest::Approx(384.0));
	CHECK(before->window.offsetBaseline == doctest::Approx(0.0));
	// ...and the picture under the star is the sound the star IS, family included. Compared at the
	// row's own home, because a row is where the map can put a place; the star sits inside half a row
	// of it.
	const auto textureRow = [&](const std::shared_ptr<const TerrainSnapshot>& s) {
		// **THE MAP'S VERTICAL AXIS IS THE PLANE'S Y** (backlog GY), so the row under the star is found
		// from the star's Y TRAVEL. Fed a home, `rowAtTravelY` answered -505 of 128 rows — a row index
		// hundreds outside the map, which is what a coordinate mix-up looks like when the two quantities
		// no longer share units.
		const long row = std::lround(TerrainMap::rowAtTravelY(s->window, f.stage.selectedDriftY()));
		REQUIRE(row >= 0);
		REQUIRE(row < TerrainMap::kResolution);
		// THE ROW'S OWN Y AND THE COLUMN'S OWN X — both travels, which is the whole of the design: the
		// two axes are the same kind of number and the reference restates the decode with both of them
		// in the places the builder puts them. The row used to be turned back into a gamma base by
		// subtracting the dials off it; there is nothing to subtract now, because the axis was never a
		// home in the first place.
		const double rowY = TerrainMap::travelYAtRow(s->window, static_cast<int>(row));
		const double travel = TerrainMap::travelXAtColumn(s->window, 64);
		const auto zone = static_cast<uint16_t>(std::lround(s->window.zoneBaseline));
		const TerrainFeatures features =
		    terrainFeaturesAt(OscType::Stair, s->window.pin, zone, s->window.offsetBaseline, travel,
		                      rowY, kNoPlaneResidue, s->window.gammaBase);
		INFO("row " << row << " at Y travel " << rowY << " built at zone " << zone);
		CHECK(s->texel(64, static_cast<int>(row))
		      == terrainColour(features, travel, rowY, 3, colourMapFor(OscType::Stair)));
	};
	textureRow(before);

	// NOW DRAG, and count what the terrain does about it.
	//
	// INSIDE THE EDGE-FOLLOW MARGIN, which is where the "nothing rebuilds" claim belongs and the only
	// place it is true: past kScrubEdgeMargin the view moves with the point, and a moved view is a
	// different picture that MUST be rebuilt. The margin is 0.35 of a span either side of centre, which
	// on this widget is about 187px, so this stops at 150.
	const visage::Point c = f.stage.disk().centre();
	const uint64_t builtBefore = f.terrain.builds();
	const double heldCentre = f.stage.viewCentre();
	const visage::Bounds placedBefore = f.stage.terrainPlacement();
	f.stage.onMouseDown().callback(at(c.x, c.y));
	for (int px = 15; px <= 150; px += 15) {
		f.stage.onMouseDrag().callback(at(c.x, c.y - static_cast<float>(px)));
		f.settleGesture();
		f.stage.draw(canvas);
	}
	// THE TERRAIN HELD STILL. Ten drag frames, ten windows asked for, nothing rebuilt — because the map
	// does not depend on the number the drag is writing. (And the view really did hold, or this would
	// be measuring a drag that never reached the terrain at all.)
	INFO("builds during a ten-frame vertical drag: " << f.terrain.builds() - builtBefore);
	CHECK(f.stage.viewCentre() == doctest::Approx(heldCentre));
	CHECK(f.terrain.builds() == builtBefore);
	// AND IT IS STILL DRAWN IN THE SAME PLACE. "Nothing was rebuilt" is a fact about the buffer, and
	// this project has been caught twice believing that meant the picture was right — the map can be
	// 100% full and drawn entirely off the widget (defect 4). So the claim is asserted where the DRAW
	// reads it: the rectangle the terrain is painted into has not moved by so much as a pixel.
	INFO("terrain drawn at y=" << f.stage.terrainPlacement().y() << " (was " << placedBefore.y()
	                           << ")");
	CHECK(f.stage.terrainPlacement().y() == doctest::Approx(placedBefore.y()));
	CHECK(f.stage.terrainPlacement().height() == doctest::Approx(placedBefore.height()));

	// AND ON PAST THE OLD END OF THE WORLD, where the view does follow and the map is rebuilt as it
	// should be — the far country is drawn, and it is drawn at this sound's dials like everywhere else.
	// ZOOMED OUT FOR THE LONG HAUL, since a drag carries the world at a bounded number of VIEWS per second
	// (`kViewsPerSecond`, 2026-09-17): the far country is fifty views away at this zoom and one view away at the widest,
	// and what this case is about is that the drag does not STOP there, not how long a hand would take.
	f.stage.setZoom(ExploreStage::minZoom());
	f.stage.draw(canvas);
	for (int px = 170; px <= 1600; px += 20) {
		f.stage.onMouseDrag().callback(at(c.x, c.y - static_cast<float>(px)));
		f.settleGesture();
		f.now += 40; // the view follows at a bounded speed — see the case above
		f.stage.update();
	}
	f.stage.onMouseUp().callback(at(c.x, c.y - 1600.0f));
	f.settleGesture();
	f.stage.draw(canvas);
	INFO("dragged out to Y travel " << f.stage.selectedDriftY() << ", the old band's ceiling "
	                                << homeCeiling(0.0));
	REQUIRE(f.stage.selectedDriftY() > homeCeiling(0.0));

	// AND AFTER THE RELEASE — which re-centres, so the view really has moved and a map really is due —
	// the sound is still standing on its own country. The dials never moved, so the family did not
	// either, which is the design drawn.
	const std::shared_ptr<const TerrainSnapshot> after = settled();
	REQUIRE(after);
	CHECK(after->window.zoneBaseline == doctest::Approx(384.0));
	CHECK(f.read(ParamId::BZone) == doctest::Approx(384.0));
	textureRow(after);
}

TEST_CASE("a whole carrying drag is one crumb and one undo, and the undo brings the PLACE back") {
	// #26 SAID PROVE THIS RATHER THAN READ IT. `SoundIdentity` carries the coordinate the chart writes,
	// so undo, redo, breadcrumbs and clone "should" follow the drag onto its new road for free — and
	// "should, by inspection" is how this project has shipped half-verbs twice. The thing that makes
	// it worth a case is GRANULARITY: a drag is forty-odd separate writes, and a history that recorded
	// each one would make undo a frame-by-frame rewind of a gesture nobody thinks of as forty edits.
	//
	// **THE COORDINATE MOVED, THE CLAIM DID NOT** (backlog GY). It was `gammaBase`; the chart is now a
	// window on the decoded plane, so a vertical drag writes `driftY`. Both are fields of the same
	// identity and both were always carried by it — which is exactly what #26 asked to have proved,
	// and the proof survives the axis change without being weakened.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	const SoundIdentity start = f.stage.identity();
	REQUIRE(f.stage.crumbs(1).empty());
	REQUIRE_FALSE(f.stage.canUndo());

	f.stage.onMouseDown().callback(at(c.x, c.y));
	for (int px = 20; px <= 800; px += 20) {
		// A frame's worth of time per 20 px, so the rate cap has something to pace against; with the
		// clock frozen the whole 800 px is an instantaneous hand and the cap holds the world still.
		f.now += 16;
		f.stage.onMouseDrag().callback(at(c.x, c.y - static_cast<float>(px)));
		f.settleGesture();
	}
	f.stage.onMouseUp().callback(at(c.x, c.y - 800.0f));
	f.settleGesture();
	// AND THE CRUMB IS PLANTED WHEN THE WORLD STOPS, not when the button comes up: a capped drag is
	// still travelling at the release, and it is one gesture until it arrives. The frame count is
	// captured so the case says which of the two things it is testing — a drag that had further to go
	// (what this is) rather than one that had already finished (which would prove nothing about the
	// deferred close).
	const int coasted = f.settleScrub();
	INFO("the released drag coasted for " << coasted << " frames");
	CHECK(coasted > 0);

	const SoundIdentity arrived = f.stage.identity();
	INFO("Y travel " << start.driftY << " -> " << arrived.driftY << " over 40 drag events");
	REQUIRE(arrived.driftY != doctest::Approx(start.driftY));
	// AND NO GAMMA WENT ANYWHERE, which is the design and is what says the drag really did change
	// roads rather than the case merely changing which field it reads.
	CHECK(arrived.gammaBase == doctest::Approx(start.gammaBase));

	// ONE CRUMB, planted where the gesture LEFT, not forty crumbs down the path it took.
	REQUIRE(f.stage.crumbs(1).size() == 1);
	CHECK(f.stage.crumbs(1)[0].driftY == doctest::Approx(start.driftY));

	// AND ONE UNDO STEP, which puts the whole gesture back — the place included, exactly, in one go.
	REQUIRE(f.stage.canUndo());
	f.stage.undo();
	f.settleGesture();
	CHECK(f.stage.identity() == start);
	CHECK_FALSE(f.stage.canUndo());
	// Redo returns the whole of it just as completely: the base is a stored number restored, never a
	// sum of increments replayed.
	f.stage.redo();
	f.settleGesture();
	CHECK(f.stage.identity() == arrived);
}

TEST_CASE("the editor's one history puts a slot's switch back, not only its sound") {
	// WHY AN ENTRY CARRIES THE SWITCH. The history moved out of this surface because the CARDS have a
	// verb that needs recording, and a clone does two things at once: it writes an identity into a
	// slot and it switches that slot on. An entry that carried only the identity would undo half of
	// that — the slot would keep the sound it was given and stay in the patch — which is the same
	// half-a-sound failure SoundIdentity itself has had twice (A5's drift, A26's residue), arriving
	// one level up.
	ExploreFixture f;
	const auto enableB = static_cast<clap_id>(soundParam(1, SoundField::Enable));
	f.params.setBase(enableB, 0.0);
	f.bridge.publish(f.params);
	f.bridge.refresh();
	REQUIRE(f.bridge.value(enableB) == 0.0);

	const SlotState parked = readSlotState(f.bridge, 1);
	REQUIRE_FALSE(parked.enabled);

	SlotState arrived = parked;
	arrived.enabled = true;
	arrived.identity.zone = parked.identity.zone + 100.0;
	recordSlot(f.history, f.bridge, 1, parked, arrived, "clone");
	writeSlotState(f.bridge, 1, arrived);
	f.settleGesture();
	REQUIRE(f.bridge.value(enableB) == 1.0);
	REQUIRE(f.bridge.value(static_cast<clap_id>(soundParam(1, SoundField::Zone)))
	        == doctest::Approx(arrived.identity.zone));

	// ONE STEP BACK TAKES BOTH HALVES: the sound it had, and out of the patch again.
	REQUIRE(f.stage.canUndo());
	f.stage.undo();
	f.settleGesture();
	CHECK(f.bridge.value(enableB) == 0.0);
	CHECK(readSlotState(f.bridge, 1) == parked);

	// And forward again, both halves together.
	f.stage.redo();
	f.settleGesture();
	CHECK(f.bridge.value(enableB) == 1.0);
	CHECK(readSlotState(f.bridge, 1) == arrived);
}

TEST_CASE("the stage walks entries it did not record, and points itself at them") {
	// THE SHARING CLAIM, and it is the whole reason the stack left this widget. Something else — the
	// cards, a future panel — records into the editor's history; the Explore corners must light for it
	// and walking it must bring the view to the sound that moved, exactly as it does for a scrub. The
	// record puts the VALUES back on its own (it holds the bridge, not a widget); this asserts the
	// other half, which is this surface's: the slot is selected and the chart re-centres on it.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(0);
	f.stage.draw(canvas);
	REQUIRE_FALSE(f.stage.canUndo());

	const SlotState before = readSlotState(f.bridge, 3);
	SlotState after = before;
	after.identity.zone = before.identity.zone + 250.0;
	recordSlot(f.history, f.bridge, 3, before, after, "clone");
	writeSlotState(f.bridge, 3, after);
	f.settleGesture();

	// The corner is live for somebody else's edit.
	CHECK(f.stage.canUndo());
	CHECK(f.stage.historyLength() == 1);

	f.stage.undo();
	f.settleGesture();
	// The values came back...
	CHECK(readSlotState(f.bridge, 3) == before);
	// ...and this surface pointed itself at the sound that moved, which is what it is for.
	CHECK(f.selection.slot() == 3);
}

TEST_CASE("a chart drag moves one sound's PLACE, no gamma at all, and never the frame") {
	// A24, AT THE NEW ROAD, and the road moved again. The claim is unchanged and is about SCOPE: a
	// gesture that is about ONE sound must reach exactly one slot and nothing else, because the frame
	// over all of them is one keystroke away in the same bridge.
	//
	// **WHAT CHANGED IS WHICH NUMBER A DRAG WRITES** (backlog GY). It was the selected slot's gamma
	// base; the chart is now a window on the decoded plane, so a drag writes that slot's TRAVEL and no
	// gamma anywhere. So the case asserts the scope claim on travel AND adds the inverse of its old
	// subject — every base, including the selected one, holding perfectly still — which is strictly
	// stronger than what it used to say and is what would catch the old road being re-opened.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	const double globalBefore = f.read(ParamId::GlobalGamma);
	std::array<double, kMaxSounds> basesBefore = f.bridge.soundGamma().base;
	const std::array<AnchorPoint, kMaxSounds> travelBefore = f.bridge.soundTravel().offset;
	// ZOOMED OUT, since a drag is throttled in VIEWS per second (`kViewsPerSecond`, 2026-09-17) and the distance this
	// case is about is a fifth of the whole old band.
	f.stage.setZoom(ExploreStage::minZoom());
	f.stage.draw(canvas);

	f.stage.onMouseDown().callback(at(c.x, c.y));
	for (int px = 40; px <= 800; px += 40) {
		f.stage.onMouseDrag().callback(at(c.x, c.y - static_cast<float>(px)));
		f.settleGesture();
		f.now += 60; // the drag is throttled by time — see the cases above
		f.stage.update();
	}
	f.stage.onMouseUp().callback(at(c.x, c.y - 800.0f));
	f.settleGesture();

	// The selected slot moved, by far more than a rounding — the drag is 800px at the widest zoom.
	INFO("slot 1 travel Y " << travelBefore[1].y << " -> "
	                        << f.bridge.soundTravel().offset[1].y);
	REQUIRE(std::abs(f.bridge.soundTravel().offset[1].y - travelBefore[1].y) > 0.5 * 64.0 / 1023.0);
	// ...the frame did not move at all...
	CHECK(f.read(ParamId::GlobalGamma) == doctest::Approx(globalBefore));
	// ...NO sound's gamma base moved, INCLUDING THE SELECTED ONE. This is the inverted half: the
	// number this case used to watch a drag write is now the number it watches a drag leave alone.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		INFO("gamma base, slot " << slot);
		CHECK(f.bridge.soundGamma().base[slot] == doctest::Approx(basesBefore[slot]));
	}
	// ...and no other sound went anywhere in the plane either.
	for (size_t slot = 0; slot < kMaxSounds; ++slot) {
		if (slot == 1) {
			continue;
		}
		INFO("travel, slot " << slot);
		CHECK(f.bridge.soundTravel().offset[slot].x == doctest::Approx(travelBefore[slot].x));
		CHECK(f.bridge.soundTravel().offset[slot].y == doctest::Approx(travelBefore[slot].y));
	}
}

TEST_CASE("the gamma corner HOLDS STILL while the chart is dragged, and is still telling the truth") {
	// **A DECISION OVERTURNED, AND THEN RESTORED — the record is worth keeping because the original
	// worry turned out to be right.** `homeToCoord` used to hold gamma still under a scrub, because
	// a scrub that quietly moved it would make that encoder's readout a lie. The one-value design
	// then made every vertical drag move exactly that number, and this case was rewritten to assert
	// the opposite: that the corner is SEEN to move, since a verb that works and looks dead is this
	// project's own recurring bug.
	//
	// **Backlog GY puts it back.** The chart is a window on the decoded plane, gamma is consumed by
	// the decode rather than being an axis, and a chart drag writes no gamma at all — so the corner
	// holding still is once again the correct behaviour, for the reason first given.
	//
	// TWO CLAIMS, AND THE SECOND IS WHY THIS IS NOT A WEAKER CASE THAN THE ONE IT REPLACES:
	// 1. the reading and its compass do not move on ANY frame of a chart drag;
	// 2. what they say is STILL the sound's own number, checked every frame — so "it held still"
	//    cannot be satisfied by a readout that has simply gone dead.
	// And a positive control closes the last gap: the same readout, driven from the GAMMA corner,
	// must move. A case that only ever asserts stillness passes against a deleted control.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	std::string previous = f.stage.gammaReadingDrawn();
	const std::string first = previous;
	float previousDial = f.stage.gammaDialDrawn();
	REQUIRE_FALSE(previous.empty());
	const double totalBefore = f.read(ParamId::BGamma) + f.stage.selectedGammaBase();

	f.stage.onMouseDown().callback(at(c.x, c.y));
	int moved = 0;
	int dialMoved = 0;
	for (int px = 30; px <= 300; px += 30) {
		// A frame of time per frame of drag, so the rate cap lets the whole 30 px through: the claim
		// here is that the corner is LIVE, and a hand the cap is holding back would make it look dead
		// for reasons that have nothing to do with the readout.
		f.now += 150;
		f.stage.onMouseDrag().callback(at(c.x, c.y - static_cast<float>(px)));
		f.settleGesture();
		f.stage.draw(canvas);
		const std::string now = f.stage.gammaReadingDrawn();
		if (now != previous) {
			++moved;
		}
		if (f.stage.gammaDialDrawn() != previousDial) {
			++dialMoved;
		}
		INFO("at " << px << "px the corner reads " << now);
		previous = now;
		previousDial = f.stage.gammaDialDrawn();

		// AND IT IS THIS SOUND'S NUMBER. The corner reads the base plus the vernier — "where is this
		// sound along its line" — so the string on the glass is checked against that sum every frame,
		// which is what stops a readout that moves beautifully and means nothing.
		char expected[32] = {};
		std::snprintf(expected, sizeof(expected), "%+.2f",
		              f.read(ParamId::BGamma) + f.stage.selectedGammaBase());
		CHECK(std::string(expected) == now);
	}
	f.stage.onMouseUp().callback(at(c.x, c.y - 300.0f));
	f.settleGesture();

	// **NEITHER MOVES, ON ANY FRAME.** The dial is drawn from a float and has no resolution to hide
	// behind, so this is exact rather than approximate: if a drag had touched gamma by any amount at
	// all, the face would have moved.
	INFO("readout changed on " << moved << " of 10 drag frames, dial on " << dialMoved << "; "
	                           << first << " -> " << previous);
	CHECK(dialMoved == 0);
	CHECK(moved == 0);
	CHECK(previous == first);

	// AND THE PRINTED NUMBER MOVES AS OFTEN AS TWO DECIMALS CAN SHOW, which is a smaller number than
	// ten and it would be dishonest to assert otherwise: the reading is "%+.2f", so it changes when the
	// sound crosses a hundredth and not before. The bound is DERIVED from how far this drag actually
	// went rather than picked — a run covering N whole hundredths must change the string at least N-1
	// times, the −1 being the part-hundredth it starts inside. Measured here: 6 hundredths, 5 changes.
	// What makes the readout TRUE rather than merely lively is the per-frame equality against the
	// sound's own number inside the loop; this is the liveness half alone.
	// AND THE NUMBER ITSELF DID NOT MOVE, which is the claim the pixels above are evidence for. Read
	// from the bridge rather than from the glass, so the two roads have to agree.
	const double totalAfter = f.read(ParamId::BGamma) + f.stage.selectedGammaBase();
	INFO("gamma total " << totalBefore << " -> " << totalAfter);
	CHECK(totalAfter == doctest::Approx(totalBefore));

	// **THE POSITIVE CONTROL.** Everything above says a readout did not change; none of it can tell
	// the difference between "held still" and "stopped working". The Gamma CORNER is the control that
	// still owns this number, so driving it must move both the string and the face — and if this half
	// ever goes red, the stillness above means nothing.
	const visage::Bounds corner = f.stage.partBounds(ExploreStage::Part::Gamma);
	const float gx = corner.x() + corner.width() * 0.5f;
	const float gy = corner.y() + corner.height() * 0.5f;
	f.stage.onMouseDown().callback(at(gx, gy));
	f.stage.onMouseDrag().callback(at(gx, gy - 60.0f));
	f.stage.onMouseUp().callback(at(gx, gy - 60.0f));
	f.settleGesture();
	f.stage.draw(canvas);
	INFO("after a gamma-corner drag the reading is " << f.stage.gammaReadingDrawn());
	CHECK(std::string(f.stage.gammaReadingDrawn()) != first);
	CHECK(f.stage.gammaDialDrawn() != previousDial);
}

TEST_CASE("a Radius drag cannot strand the sound: the corner is its own way back") {
	// **THE ORIGINAL REPORT, AND THE GUARANTEE MOVED RATHER THAN LEFT.** Measured on the shipped
	// build before the first fix: one full drift-corner drag left the star at x=1151 with the chart
	// spanning 166..700 — 451px outside the right edge — and NOTHING could bring it back, because
	// `driftTo(travelXAtScreenX(x))` can only write a drift the view can SEE. The sound was stranded
	// off its own chart. That is what "hitting the edge of the plane" was: not the world ending, but
	// the window refusing to follow.
	//
	// THE OLD FIX WAS TO RE-CENTRE ON THE CORNER, and the polar corners deliberately do not
	// re-centre the view. Turning Radius is meant to be watched, and a view that pans to follow the
	// puck puts it back in the middle and shows you nothing moving.
	//
	// **SO THE SOUND CAN NOW LEAVE THE CHART, AND THAT IS NOT THE BUG.** The bug was being unable to
	// get it back. The corner is its own inverse — Radius walks the sound out along a ray and down
	// the same ray — so what this case asserts is the property that actually failed: a sound sent far
	// away comes home, by the gesture that sent it. What it no longer asserts is that the star stays
	// drawn inside the disc, because it was asked for the behaviour that lets it leave.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);

	const auto turn = [&](float dy) {
		dragCorner(f, ExploreStage::Part::Radius, dy);
		f.settleScrub();
	};

	const double home = f.stage.selectedRadius();
	// FAR OUT — two full travels, which is two disc radii at this zoom and well past the rim.
	turn(280.0f);
	const double far = f.stage.selectedRadius();
	INFO("radius " << home << " -> " << far);
	REQUIRE(far > 0.5); // the top of the exponential face is the plane's whole reach

	// THE PLAYED PLACE REALLY DID LEAVE THE CHART — no second star is drawn out there — WHILE THE MAIN
	// STAR STAYED PUT, because Radius is an offset from the place and not the place (2026-09-21).
	f.stage.draw(canvas);
	CHECK_FALSE(f.stage.playedMarkDrawn());
	CHECK(f.stage.starDrawnOnChart());

	// **AND ONE GESTURE BRINGS IT HOME.** The same corner, the same distance, the other way — which is
	// the whole of the anti-stranding guarantee now: the way out and the way back are one control.
	turn(-280.0f);
	INFO("after turning back: radius " << f.stage.selectedRadius() << ", started at " << home);
	CHECK(f.stage.selectedRadius() == doctest::Approx(home));
	f.stage.draw(canvas);
	CHECK(f.stage.starDrawnOnChart());
	CHECK_FALSE(f.stage.playedMarkDrawn()); // radius back at zero: one sound, drawn once

	// AND THE DIRECTION SURVIVED THE ROUND TRIP, so "it came back" is the sound returning rather than
	// collapsing to the origin and being redrawn somewhere plausible.
	CHECK(f.stage.selectedTheta() == doctest::Approx(0.0));
}

TEST_CASE("the header's switch chooses a surface and stores nothing in the patch") {
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	StageMode mode;
	NotifyTray tray;
	HeaderBar header(bridge, registry, mode, &tray);
	header.setBounds(0.0f, 0.0f, 1240.0f, HeaderBar::kHeight);
	bridge.refresh();
	REQUIRE(mode.surface() == StageMode::Surface::Perform);

	const visage::Bounds box { 1240.0f - 20.0f - 152.0f,
		                       (HeaderBar::kHeight - 26.0f) * 0.5f, 152.0f, 26.0f };
	const visage::Point left { box.x() + box.width() * 0.25f, box.y() + box.height() * 0.5f };
	REQUIRE(header.hitsMode(left));
	header.onMouseDown().callback(at(left.x, left.y));
	CHECK(mode.exploring());
	// Not a parameter, and it must never become one: which view you are looking at is not part of a
	// sound, and a permanent id spent on it would show up in somebody's automation lane.
	CHECK(drain(bridge).empty());

	const visage::Point right { box.x() + box.width() * 0.75f, box.y() + box.height() * 0.5f };
	header.onMouseDown().callback(at(right.x, right.y));
	CHECK_FALSE(mode.exploring());
	CHECK(drain(bridge).empty());
}

TEST_CASE("a surface change swaps the stage without waiting for a tick") {
	// The header lights from a watcher on the click. The circle used to wait for tick() to
	// notice perform->isVisible() == mode.exploring() and call showSurface. On macOS a host
	// that offers no timer never ticks, so the switch lit and the circle stayed. The swap
	// is now the mode's own reaction, not a frame later.
	StageMode mode;
	int swaps = 0;
	mode.onChanged([&] { ++swaps; });
	REQUIRE_FALSE(mode.exploring());
	mode.show(StageMode::Surface::Explore);
	CHECK(mode.exploring());
	CHECK(swaps == 1);
	mode.show(StageMode::Surface::Explore);
	CHECK(swaps == 1);
	mode.show(StageMode::Surface::Perform);
	CHECK_FALSE(mode.exploring());
	CHECK(swaps == 2);
}

TEST_CASE("stair's pattern families are where its colour families change") {
	// §4a rules the literal families may anchor the colour, because they are real discrete regions:
	// `min(7, zone >> 7)` is a step change in the SOUND, so a step change in the colour is the honest
	// drawing of it rather than a visualisation convenience.
	CHECK(stairFamilyAt(0) == 0);
	CHECK(stairFamilyAt(127) == 0);
	CHECK(stairFamilyAt(128) == 1);
	CHECK(stairFamilyAt(1023) == 7);

	SliceOrientationPin pin;
	const auto colourAt = [&](uint16_t zone) {
		const double home = static_cast<double>(zone) / 1023.0;
		const TerrainFeatures f = terrainFeaturesAt(OscType::Stair, pin, zone, 0.0f, 0.0, 0.0);
		return terrainColour(f, home, 0.0, stairFamilyAt(zone));
	};
	// HOW FAR APART TWO DRAWN COLOURS ARE, which is what this case can honestly ask now that the map is
	// two-dimensional and swappable. It used to measure redness — red over blue — reasoning that the
	// ramp's two ends differed most in red. That reasoning belonged to a ONE-DIMENSIONAL ramp between
	// two fixed colours: under a map chosen as a vibe, no channel is promised to be the one that moves,
	// and a monotone climb in red would be a demand on the palette rather than on the anchor. What must
	// be true of ANY map is that the eight families land in eight distinguishable places.
	// MEASURED IN CHROMATICITY rather than in raw channel distance, and that correction is the old
	// case's insight recovered. It measured red OVER blue — a RATIO — which is invariant to how bright
	// the sound is, and my first replacement used a plain RGB distance, which is not: the drawn colour
	// is multiplied by `glow`, so the same family step reads large under a loud sound and small under a
	// quiet one, and a threshold in that space is a threshold on the fixture's loudness. Normalising by
	// the channel sum measures the COLOUR and lets the brightness alone.
	const auto apart = [](uint32_t a, uint32_t b) {
		const auto chroma = [](uint32_t c) {
			const float r = static_cast<float>((c >> 16) & 0xFF);
			const float g = static_cast<float>((c >> 8) & 0xFF);
			const float b = static_cast<float>(c & 0xFF);
			const float sum = std::max(r + g + b, 1.0f);
			return std::array<float, 3> { r / sum, g / sum, b / sum };
		};
		const auto x = chroma(a);
		const auto y = chroma(b);
		return std::sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1])
		                 + (x[2] - y[2]) * (x[2] - y[2]));
	};

	// THE ANCHOR ON ITS OWN, with the sound held still: one set of features coloured under each of the
	// eight families. Anything measured at real zones mixes in how the sound itself changed, and the
	// ember term moves red as well — so this is the only way to see the family term by itself.
	const TerrainFeatures held = terrainFeaturesAt(OscType::Stair, pin, 400, 0.0f, 0.0, 0.0);
	std::array<uint32_t, kStairFamilies> byFamily {};
	for (int family = 0; family < kStairFamilies; ++family) {
		byFamily[static_cast<size_t>(family)] = terrainColour(held, 0.4, 0.0, family);
	}
	// EVERY FAMILY IS ITS OWN COLOUR — all twenty-eight pairs distinct, not merely consecutive ones.
	// This replaced a per-step minimum distance, and the reason is the same one that retired the
	// redness ratio: how far one step MOVES a drawn colour depends on the map's local gradient, which
	// is a property of whichever vibe was picked, so a fixed per-step threshold is a demand on the
	// palette wearing the anchor's clothes. Distinctness is the requirement itself and survives any map.
	for (int a = 0; a < kStairFamilies; ++a) {
		for (int b = a + 1; b < kStairFamilies; ++b) {
			INFO("families " << a << " and " << b);
			CHECK(byFamily[static_cast<size_t>(a)] != byFamily[static_cast<size_t>(b)]);
		}
	}
	// AND THE TERM ITSELF CLIMBS, which is the mechanism under the picture: the family tint is monotone
	// and spends its whole range, so the eight are spread rather than crowded whatever the map does
	// with them afterwards.
	for (int family = 1; family < kStairFamilies; ++family) {
		CHECK(terrainFamilyTint(family) > terrainFamilyTint(family - 1));
	}
	CHECK(terrainFamilyTint(0) == 0.0f);
	CHECK(terrainFamilyTint(kStairFamilies - 1) == 1.0f);
	// AND THE ENDS ARE FAR APART, which is what makes the anchor legible rather than a gradient nobody
	// can name.
	CHECK(apart(byFamily.front(), byFamily.back()) > 0.02f);

	// AND IT HOLDS FOR EVERY VIBE MAP, which is the point of restating this case: the anchor is a
	// property of the decode, not of the palette somebody picked this week. A map that crowded the
	// families would be a map that cannot carry stair, and this is where that shows up.
	for (const ColourMap map : { ColourMap::Nebula, ColourMap::Ember, ColourMap::Tide }) {
		CAPTURE(static_cast<int>(map));
		const uint32_t low = terrainColour(held, 0.4, 0.0, 0, map);
		const uint32_t high = terrainColour(held, 0.4, 0.0, kStairFamilies - 1, map);
		CHECK(apart(low, high) > 0.02f);
	}

	// And at a real boundary the drawn colour really does change, which is the claim a user can see.
	CHECK(colourAt(127) != colourAt(128));
}

TEST_CASE("the chart does not run off the end of the world when gamma moves") {
	// AFTER THE SHIPPED CARRYING FIX: rotation was fixed, and sound gamma still made the terrain
	// disappear entirely and stay gone until the adjustment stopped. Two defects wore one symptom. This
	// is the second one, and it is arithmetic rather than a race.
	//
	// The reachable band is `[gammaSum, gammaSum + 3]` — `homeFloor` IS the gamma sum — so gamma drags
	// the whole world along with it instead of moving a sound through it. Hold the view still while
	// gamma moves and the band slides out from under the chart; every row is then a place that does not
	// exist, which the builder correctly paints as nothing. The corner moves kGammaTurn = 8.0 of home
	// per encoder travel against a band 3.0 wide, so a couple of pixels of hand empties the screen.
	//
	// WHY THE INSTRUMENT MISSED IT, which is the lesson worth more than the fix: my gesture model moved
	// the view WITH the gamma, because I wrote it from update()'s star-left-the-view rule. The real
	// corner drag holds `grabbed_ == Part::Gamma`, and update() only re-centres when nothing is
	// grabbed. So the model reproduced a gesture nobody makes and measured 1.2% void where a real hand
	// gets 100%. A gesture driven through the WIDGET cannot make that mistake, which is why this case
	// presses the corner instead of composing a window.
	//
	// KNOWN-BAD: make viewCentre() return the raw viewCentre_ again and this case blanks at 100%.
	ExploreFixture f;
	// the sound A exactly: zone 0, offset 0, gamma 0 — so home EQUALS gamma and the view sits on the
	// band's floor, which is the worst case and also the default a new patch hands you.
	f.set(ParamId::AZone, 0.0);
	f.set(ParamId::AOffset, 0.0);
	f.set(ParamId::AGamma, 0.0);
	f.selection.select(0);
	visage::Canvas canvas;

	const size_t all = static_cast<size_t>(TerrainMap::kResolution) * TerrainMap::kResolution;
	// Draw until the map for whatever window the stage is asking for has finished, so what is measured
	// is the WINDOW's honesty and not the progressive fill the carrying fix already covers.
	const auto settledVoid = [&]() -> double {
		for (int spin = 0; spin < 4000; ++spin) {
			f.stage.draw(canvas);
			const uint64_t builds = f.terrain.builds();
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
			const std::shared_ptr<const TerrainSnapshot> snapshot = f.terrain.snapshot();
			if (snapshot->rows >= TerrainMap::kResolution && f.terrain.builds() == builds) {
				size_t transparent = 0;
				for (const uint32_t texel : snapshot->texels) {
					transparent += (texel >> 24) == 0u ? 1 : 0;
				}
				return static_cast<double>(transparent) / static_cast<double>(all);
			}
		}
		return -1.0;
	};

	const double atRest = settledVoid();
	REQUIRE(atRest >= 0.0);
	// AND THE CHART IS FULL AGAIN, by a different road than the one that used to make it full. This line
	// has now been three things, which is worth keeping because each was true of its own build: `< 0.02`
	// when the view was CLAMPED inside the band (full because it could not look out); `~0.5` when the
	// clamp came off and a sound on the floor was drawn centred with void beneath it; and `< 0.02` again
	// now that the world is infinite and there IS no void — the rows below the floor carry the true
	// character at their home, because the overflow goes into the gamma base instead of being clamped
	// away. Same number, opposite reason: not "the view cannot leave the world" but "there is nowhere the
	// view can go that is not the world".
	MESSAGE("void at rest, standing on the band floor: " << (100.0 * atRest) << "%");
	CHECK(atRest < 0.02);

	// The corner, pressed and dragged UPWARD — the direction that raises gamma and so raises the floor
	// out through the top of a view sitting on it.
	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Gamma);
	const float x = box.x() + box.width() * 0.5f;
	const float y = box.y() + box.height() * 0.5f;
	f.stage.onMouseDown().callback(at(x, y));

	double worst = 0.0;
	for (const float pixels : { 2.0f, 5.0f, 12.0f, 30.0f, 60.0f }) {
		f.stage.onMouseDrag().callback(at(x, y - pixels));
		f.settleGesture();
		const double fraction = settledVoid();
		REQUIRE(fraction >= 0.0);
		worst = std::max(worst, fraction);
		MESSAGE("  dragged " << pixels << "px: void " << (100.0 * fraction) << "%");
	}
	f.stage.onMouseUp().callback(at(x, y - 60.0f));

	// The gesture has to have actually MOVED gamma, or the case is measuring a hand that never touched
	// anything — and a corner whose hit box moved would do exactly that silently.
	f.settleGesture();
	// Read off the WINDOW the builder was given, which needs no new accessor and is the number that
	// actually decides where the band is — ONCE A BUILD HAS CARRIED IT THERE. The snapshot is the last map that
	// LANDED, so reading it the instant the gesture ends asks the terrain a question it has not answered yet: the
	// case used to get away with that because the spins above happened to leave a build behind, and it went red the
	// day the worker had something else to do first (the prefetch, 2026-09-17). Draw and wait for the answer.
	// READ OFF `gammaBase`, WHICH IS WHERE THE WINDOW CARRIES GAMMA NOW (backlog GY). It was
	// `viewGamma`, a frame for re-registering a stale map against a view that FOLLOWED gamma. Nothing
	// follows gamma; the base is instead a real content input — it completes the home the whole plane
	// is taken at — so it is in the key and a build must land before it reads back.
	double gammaAfter = 0.0;
	for (int spin = 0; spin < 4000 && std::abs(gammaAfter) <= 0.1; ++spin) {
		f.stage.draw(canvas);
		gammaAfter = f.terrain.snapshot()->window.gammaBase;
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	MESSAGE("gamma the terrain was built at, after the drag: " << gammaAfter);
	CHECK(std::abs(gammaAfter) > 0.1);

	// THE CLAIM, RESTATED FOR THE CENTRING DECISION BUT GUARDING THE SAME DEFECT: the void must not GROW
	// as gamma moves. The band travels with gamma, so a view that follows it sees exactly as much of the
	// world after the drag as before — whatever the sound's place in the band makes that. Defect 2 was
	// the view holding still while the band slid out from under it, and that took the void to 100%; this
	// fires on that exactly as the old `< 0.02` did, without also asserting the clamp that was retired.
	MESSAGE("worst void during the sweep: " << (100.0 * worst) << "% (at rest " << (100.0 * atRest)
	                                        << "%)");
	CHECK(worst <= atRest + 0.02);
}

TEST_CASE("the map is drawn ON the chart while gamma sweeps, not somewhere off it") {
	// After the first fix the terrain still flickered during a gamma sweep, as rotate used
	// to. The buffer was already 100% full on every published map — measured, 0.0% void mean AND worst
	// at every sweep rate — so the flicker was never about content. It was about WHERE the picture went.
	//
	// A snapshot is placed by the window it was BUILT for, which is right for a zoom and right for a pan
	// because those share a gamma. But the view follows gamma now, so during a sweep the view runs away
	// from the map at up to one and a half viewports PER FRAME. Placed by absolute home, a map one
	// gamma-tick old lands off the widget entirely. Measured through this very drag before the fix: the
	// picture sat at y 812..1330 of a 0..534 widget — 0% overlap — on 56 frames of 60, snapping back to
	// 100% only on the frames where a build had just published with the current window. Appear, vanish,
	// appear: exactly "flickers like rotate used to".
	//
	// THE FIX IS AN AGREEMENT, not a nudge. Carrying matches on `home - gammaSum` because that is the
	// chart's addressable coordinate; placement has to use the same coordinate, or one indexes by place
	// and the other by absolute home and the disagreement is the flicker.
	//
	// THIS CASE READS terrainPlacement(), WHICH drawTerrain ITSELF CALLS — the published map being
	// complete says nothing about whether any of it is on screen, and that gap is what let a "0.0% void"
	// measurement coexist with a visible flicker. Measuring the buffer was measuring the wrong noun.
	//
	// KNOWN-BAD: drop the `intoNow` translation in ExploreStage::terrainPlacement and this fails with the
	// map off the widget on almost every frame.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.set(ParamId::AGamma, 0.0);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	for (int spin = 0; spin < 500 && f.terrain.snapshot()->rows < TerrainMap::kResolution; ++spin) {
		f.stage.draw(canvas);
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	REQUIRE(f.terrain.snapshot()->rows == TerrainMap::kResolution);

	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Gamma);
	const float x = box.x() + box.width() * 0.5f;
	const float y = box.y() + box.height() * 0.5f;
	f.stage.onMouseDown().callback(at(x, y));

	float leastOverlap = 1.0f;
	int frames = 0;
	// 1.7px a frame is a hand at about 100px/s on a 140px encoder — the speed that made the view outrun
	// the builder in the first place. A case written at a crawl would pass while a real hand flickered.
	for (int frame = 1; frame <= 60; ++frame) {
		f.stage.onMouseDrag().callback(at(x, y - 1.7f * static_cast<float>(frame)));
		f.settleGesture();
		f.stage.draw(canvas);
		const visage::Bounds where = f.stage.terrainPlacement();
		const float overlapX =
		    std::max(0.0f, std::min(where.right(), f.stage.width()) - std::max(where.x(), 0.0f));
		const float overlapY =
		    std::max(0.0f, std::min(where.bottom(), f.stage.height()) - std::max(where.y(), 0.0f));
		const float covered =
		    (overlapX * overlapY) / std::max(1.0f, where.width() * where.height());
		leastOverlap = std::min(leastOverlap, covered);
		++frames;
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	f.stage.onMouseUp().callback(at(x, y - 102.0f));
	REQUIRE(frames == 60);

	// The drag has to have actually swept gamma, or this is a case about a hand that never moved.
	f.settleGesture();
	// **ASKED OF THE SOUND, NOT OF THE SNAPSHOT.** It read `snapshot()->window.gammaBase`, which is
	// now DELIBERATELY STILL for the length of a Gamma-corner gesture (`heldGammaBase_`): the map is
	// pinned to the base the grab started at so it cannot flicker through a rebuild per frame. So the
	// window reads zero all the way through a sweep that really happened, and the probe has to ask
	// the thing that moved. The hold is the subject of its own case; here it is a confound.
	const double swept = f.stage.selectedGammaBase();
	MESSAGE("swept gamma to " << swept << "; least overlap of the map with the widget "
	                          << (100.0f * leastOverlap) << "%");
	CHECK(std::abs(swept) > 1.0);
	// Measured 100% on every frame of 60. The bound leaves room for the half-texel margins and nothing
	// like the 0% the absolute-home placement reached.
	CHECK(leastOverlap > 0.75f);
}

TEST_CASE("a held gesture rebuilds the terrain live, and coalesces rather than queues") {
	// THE DEFECT: rotate only redrew the terrain when the knob was LET GO. The surface used to gate
	// requests on no gesture being in flight — a workaround for an older fault, where a request per
	// frame ABANDONED the build in progress so the map never finished. The builder coalesces now:
	// each map runs to completion, then goes again at wherever the view is BY THEN.
	//
	// **THE BUILDER IS HELD MID-BUILD, SO THE CASE COUNTS WORK INSTEAD OF TIMING IT** (backlog PL,
	// 2026-10-30). This used to time a build, time the drag, and bound the builds by their ratio, with
	// a 30% margin for the variance between the two. At a host load of 20-50 the two timings come from
	// different moments of a machine whose speed is moving, and the case went red with the mechanism
	// healthy: the ratio was measuring the host. Now the publish observer (which runs ON the terrain
	// thread, mid-build) parks the builder at the first publish of the build the gesture starts. All the
	// rest of the gesture's requests arrive while that build is provably in flight, then it is released.
	// What each design must do from there is a COUNT, the same on any machine at any load:
	//   coalescing — the held build completes, then ONE more, at the gesture's last window: 2 builds;
	//   a queue — one build per request: ~40;
	//   the old abandoning builder — the held build never completes a map.
	std::atomic<bool> armed { false };
	std::atomic<bool> holding { false };
	std::atomic<bool> released { false };
	std::mutex seenMutex;
	std::vector<std::pair<TerrainWindow, int>> seen; // every publish after the hold: its window and rows
	TerrainWindow heldWindow;
	ExploreFixture f;
	// WHATEVER HAPPENS BELOW, THE BUILDER IS LET GO before the fixture joins it — a REQUIRE that fails
	// while it is parked would otherwise leave `~TerrainMap` waiting on it for ever.
	struct Release {
		std::atomic<bool>& flag;
		~Release() { flag.store(true); }
	} releaseOnExit { released };
	REQUIRE_FALSE(f.terrain.workerStarted()); // the observer must be in place before the first request
	f.terrain.observePublishesForTest([&](const std::shared_ptr<const TerrainSnapshot>& s) {
		if (armed.exchange(false)) {
			heldWindow = s->window;
			holding.store(true);
			while (!released.load()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		if (holding.load()) {
			const std::lock_guard<std::mutex> lock(seenMutex);
			seen.emplace_back(s->window, s->rows);
		}
	});
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);

	// HANG PROTECTION ONLY, never a verdict: a wait that runs out says nothing about speed, so each one
	// is followed by a REQUIRE on the condition itself.
	const auto waitFor = [](const auto& condition) {
		for (int spin = 0; spin < 60000 && !condition(); ++spin) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return condition();
	};
	// QUIESCENT FIRST: the opening map built and published whole, and nothing more starting.
	const auto quiesce = [&] {
		for (int spin = 0; spin < 2000; ++spin) {
			const uint64_t before = f.terrain.builds();
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			if (f.terrain.builds() == before && f.terrain.snapshot()->rows >= TerrainMap::kResolution) {
				return;
			}
		}
	};
	quiesce();
	REQUIRE(f.terrain.snapshot()->rows >= TerrainMap::kResolution);

	const uint64_t requestsBefore = f.terrain.requests();
	const uint64_t buildsBefore = f.terrain.builds();

	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	// THE FIRST STEP STARTS A BUILD, and the builder parks inside it.
	armed.store(true);
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 1.0f));
	f.stage.draw(canvas);
	REQUIRE(waitFor([&] { return holding.load(); }));
	// LIVE: the map is being rebuilt while the knob is still held, which is the whole ask.
	CHECK(f.terrain.builds() - buildsBefore == 1);

	// THE REST OF THE GESTURE, every request arriving while that build is in flight.
	for (int i = 2; i <= 40; ++i) {
		f.stage.onMouseDrag().callback(at(centre.x, centre.y - static_cast<float>(i)));
		f.stage.draw(canvas);
	}
	const uint64_t requestsDuring = f.terrain.requests() - requestsBefore;
	CHECK(requestsDuring >= 20);
	const auto finalPin = f.bridge.soundOrientations().pin[0];
	released.store(true);
	// STILL HELD: the builder goes again on its own, at the latest window, without waiting for the hand.
	REQUIRE(waitFor([&] { return f.terrain.builds() - buildsBefore >= 2; }));
	f.stage.onMouseUp().callback(at(centre.x, centre.y - 40.0f));

	// And it settles: the builder catches up to exactly where the gesture ended, then stops asking.
	REQUIRE(waitFor([&] {
		const auto snap = f.terrain.snapshot();
		return snap->window.pin == finalPin && snap->rows >= TerrainMap::kResolution;
	}));
	const uint64_t settled = f.terrain.builds();
	std::this_thread::sleep_for(std::chrono::milliseconds(120));
	CHECK(f.terrain.builds() == settled);

	// COALESCED: thirty-nine requests landed on one build in flight, and the builder answered them with ONE.
	const uint64_t buildsDuring = settled - buildsBefore;
	INFO(buildsDuring << " builds over " << requestsDuring << " requests");
	CHECK(buildsDuring == 2);
	CHECK(buildsDuring < requestsDuring);
	// RAN TO COMPLETION: the held build finished its map rather than being abandoned for the newer
	// window, and the second one is the gesture's last window.
	const std::lock_guard<std::mutex> lock(seenMutex);
	const auto fullAt = [&](const auto& matches) {
		return std::any_of(seen.begin(), seen.end(), [&](const auto& p) {
			return p.second >= TerrainMap::kResolution && matches(p.first);
		});
	};
	CHECK(fullAt([&](const TerrainWindow& w) { return w == heldWindow; }));
	CHECK(fullAt([&](const TerrainWindow& w) { return w.pin == finalPin; }));
	CHECK_FALSE(heldWindow.pin == finalPin); // not vacuous: the two builds were for different windows
}

TEST_CASE("a map nobody asks for starts no thread") {
	// An editor that never shows Explore should not be paying for a worker. The surface is what asks
	// first, so the thread is born in request() rather than in the constructor.
	TerrainMap map;
	CHECK_FALSE(map.workerStarted());
	CHECK(map.snapshot()->rows == 0);
	// And a window with no span is not a request: it is the state before the surface has been laid out.
	map.request(TerrainWindow {});
	CHECK_FALSE(map.workerStarted());

	const TerrainWindow window = windowAt(0.5, 64.0 / 1023.0, 2.0);
	map.request(window);
	CHECK(map.workerStarted());
	REQUIRE(settle(map, window));
}

TEST_CASE("automation during a drag cannot reach the sound's stored drift") {
	// THE HAZARD THIS CASE WAS BORN FOR, and it is worth keeping the history: the surface used to
	// subtract a live `travel - drift` from every drag frame, so a host moving Weather, Path or Pos X
	// mid-drag shifted that term under the hand and each frame wrote the INVERSE of the automation's
	// movement into the patch. The fix was to freeze the term at the press.
	//
	// Since the one-value design there is no term to freeze: the chart is identity, the drag is 1:1, and the
	// leak is unreachable by construction rather than by a captured value. So this now asserts the
	// stronger property — the drift is what the POINTER asked for, and the automation appears nowhere
	// in the arithmetic — which is also what fails on the shipped placement.
	ExploreFixture f;
	f.selection.select(1);
	f.set(ParamId::Weather, 1.0);
	f.set(ParamId::PosX, 0.20);

	const visage::Point centre = f.stage.disk().centre();
	const float from = centre.x + 20.0f;
	const float to = centre.x + 60.0f;
	const double performedAtPress = f.stage.selectedTravel() - f.stage.selectedDrift();
	REQUIRE(performedAtPress != doctest::Approx(0.0));

	f.stage.onMouseDown().callback(at(from, centre.y));
	// The host moves the puck while the hand is down.
	f.set(ParamId::PosX, -0.35);
	// Time passes between press and drag, or the rate cap answers a hand of infinite speed and the
	// pointer's 40 px never arrive — which would be a true fact about the cap and no test of what
	// automation can reach.
	f.now += 250;
	f.stage.onMouseDrag().callback(at(to, centre.y));

	const visage::Point flatTo = Bubble::toFlat(f.stage.disk(), { to, centre.y });
	CHECK(f.bridge.soundTravel().offset[1].x == doctest::Approx(f.stage.travelXAtScreenX(flatTo.x)));
	// Read while the hand is still down, because the release re-centres the view and `travelXAtScreenX` is
	// relative to the view: asking it the same question afterwards asks it in a different frame.
	const double committedUnderTheHand = f.stage.selectedDrift();
	f.stage.onMouseUp().callback(at(to, centre.y));
	// backlog GE: the close re-centres by PANNING now, so let the view land before reading it.
	f.settleScrub();

	// The committed drift is exactly what the pointer asked for, and the release does not revise it. This
	// used to be spelled "the star is still drawn at `to`", which held only because the chart had no
	// horizontal view centre — now the release re-centres, so the star's PIXEL moves while the sound does
	// not, and asserting the pixel would assert the missing axis. The value is the claim; the pixel below
	// is where the axis now puts it.
	CHECK(f.stage.selectedDrift() == doctest::Approx(committedUnderTheHand));
	CHECK(f.stage.xAtTravelX(f.stage.selectedDrift())
	      == doctest::Approx(f.stage.disk().centre().x).epsilon(0.01));
	// And the performance term moved — under the hand, as the host intended — without any of it
	// landing in the sound.
	CHECK(f.stage.selectedTravel() - f.stage.selectedDrift() != doctest::Approx(performedAtPress));
}

TEST_CASE("the scope draws the sound the engine is playing, drift and planes included") {
	// G1: `characterFromParams` takes drift and per-sound planes with DEFAULTS, and defaults are what
	// this call site was getting — so the scope drew a sound with no drift on nobody's plane while the
	// engine played a drifted sound on its own. A picture of the instrument that is a picture of a
	// DIFFERENT instrument is worse than no picture, because it looks authoritative.
	//
	// The control is the comparison the bug is: build the character the way the ENGINE builds it, from
	// the same channels, and require the scope's to be identical.
	WaveFixture f;
	f.set(ParamId::Weather, 1.0);
	f.set(ParamId::PosX, 0.3);
	f.publish();

	// Drift one sound out into its plane, and pin another's plane by hand — the two channels the call
	// site was dropping, moved through the bridge exactly as a gesture moves them.
	f.bridge.beginSoundTravelGesture();
	f.bridge.setSoundTravel(0, { 0.42, -0.19 });
	f.bridge.endSoundTravelGesture();
	SoundOrientations planes;
	planes.pin[1].pinned = true;
	planes.pin[1].generator.rotate = 0.7f;
	f.bridge.setSoundOrientations(planes);
	// AND THE THIRD CHANNEL, added with the gamma vernier: the unbounded base, which arrived at this
	// call site defaulted for exactly the same reason drift and planes once did. A sound carried a
	// long way along its own line is somewhere else, and a scope that drew it at base zero would be
	// G1 for the third time — the same authoritative picture of a different instrument.
	// ON A LIVE SLOT, and that is not incidental: the character packs the ENABLED sounds, so a base set
	// on a switched-off slot would be a number nothing reads and this case would pass against a scope
	// that ignored the channel entirely. Slot 0 is A, which the table is born with.
	f.bridge.beginSoundGammaGesture();
	f.bridge.setSoundGamma(0, 137.91);
	f.bridge.endSoundGammaGesture();

	REQUIRE(f.wave.refresh());
	const auto read = [&](ParamId id) { return f.bridge.modulated(static_cast<clap_id>(id)); };
	const Character engine =
	    characterFromParams(read, oscTypeFromParams(read), f.bridge.tuning(), f.bridge.puckVelocity(),
	                        f.bridge.soundTravel(), f.bridge.soundOrientations(),
	                        f.bridge.soundResidue(), f.bridge.soundGamma(), Preview {}, EarSide::Head);

	// The drift half, which has been wrong since the drift channel landed.
	CHECK(f.wave.character().sounds[0].travelX == doctest::Approx(engine.sounds[0].travelX));
	CHECK(f.wave.character().sounds[0].travelY == doctest::Approx(engine.sounds[0].travelY));
	CHECK(engine.sounds[0].travelX != doctest::Approx(0.0));
	// And the plane half, which is independent of it: two defects at one call site. A sound's plane
	// lands on the sound, resolved against the global audition pin — so a dropped `planes` argument
	// silently falls back to that pin, which is a DIFFERENT plane rather than a missing one.
	CHECK(f.wave.character().sounds[1].orientation.pinned == engine.sounds[1].orientation.pinned);
	CHECK(f.wave.character().sounds[1].orientation.generator.rotate
	      == doctest::Approx(engine.sounds[1].orientation.generator.rotate));
	CHECK(engine.sounds[1].orientation.pinned);
	CHECK(engine.sounds[1].orientation.generator.rotate == doctest::Approx(0.7f));

	// And the base half. Compared against the ENGINE's own reading rather than against the number
	// typed above, so a scope that folded the base in somewhere subtly different still fails.
	CHECK(f.wave.character().sounds[0].gammaBase == doctest::Approx(engine.sounds[0].gammaBase));
	CHECK(engine.sounds[0].gammaBase == doctest::Approx(137.91));
	// Load-bearing: at base zero this assertion would pass against a scope that dropped the argument.
	CHECK(f.wave.character().sounds[0].gammaBase != doctest::Approx(0.0));
}

TEST_CASE("no snapshot ever mixes one map's coordinates with another's colours") {
	// THE REVIEWER'S STALE-FRAME HUNT, kept as a regression probe. Its original form polled for the
	// symptom — a NEW window published while OLD texels were still counted ready — over 250 attempts,
	// and never caught it even though the race was real and TSan-proven. That is worth remembering
	// about symptom hunts, and it is why this asks the stronger question instead.
	//
	// The class is removed BY CONSTRUCTION now: a snapshot carries its own window, so a map whose
	// texels belong to a different window than its coordinates is unrepresentable. This probe
	// documents what "unrepresentable" means by checking it while the view moves underneath — every
	// snapshot handed out, mid-build or finished, must be self-consistent.
	//
	// IT WAITS FOR THE INTERESTING MOMENT RATHER THAN HOPING TO LAND ON IT. The first version polled a
	// fixed few times just after each request and asserted it had caught a partial map; that window is
	// a few milliseconds against a 31 ms build, and under ctest's parallel load it caught NOTHING and
	// went red on a perfectly good build. A probe whose verdict depends on machine timing is a flake,
	// and a flake is worse than no probe because it teaches you to re-run reds.
	TerrainMap map;
	int checked = 0;
	int partial = 0;
	for (int attempt = 0; attempt < 12; ++attempt) {
		TerrainWindow w = windowAt(0.30 + 0.02 * attempt, 64.0 / 1023.0, 64.0 / 1023.0);
		// A plane the TEST chose, so the reference below is not simply echoing the window back.
		w.pin.pinned = true;
		w.pin.generator.rotate = 0.3f + 0.01f * static_cast<float>(attempt);
		map.request(w);
		// Poll until this window is finished, checking whatever is on offer on the way — so the
		// mid-build states are visited because the loop cannot leave until the build is done, not
		// because the timing happened to be kind.
		for (int poll = 0; poll < 4000; ++poll) {
			const std::shared_ptr<const TerrainSnapshot> s = map.snapshot();
			if (s->rows > 0) {
				// One texel of a finished row, against a bare call at that texel's own coordinates under
				// the snapshot's OWN window. A mixture fails this immediately.
				const int row = s->rows - 1;
				// **THE TEXEL'S OWN TWO TRAVELS** (backlog GY). This read the row as a HOME and split it
				// into the dials plus a per-row base, which is what the builder did while the vertical
				// axis was home. The map has one home now — the window's — and the row is travel-Y.
				const double travelY = TerrainMap::travelYAtRow(s->window, row);
				const double travelX = TerrainMap::travelXAtColumn(s->window, 7);
				const auto zone = static_cast<uint16_t>(std::lround(s->window.zoneBaseline));
				const auto phaseOffset = static_cast<float>(s->window.offsetBaseline);
				// The snapshot's own window IS the right reference here — the claim is that a map's
				// texels belong to the window it carries, whichever window that is — but the pin it
				// carries has to be one the test asked for rather than a default nobody chose.
				CHECK(s->window.pin.pinned);
				const TerrainFeatures features =
				    terrainFeaturesAt(s->window.engine, s->window.pin, zone, phaseOffset, travelX,
				                      travelY, kNoPlaneResidue, s->window.gammaBase);
				const int family = s->window.engine == OscType::Stair ? stairFamilyAt(zone) : -1;
				CHECK(s->texel(7, row)
				      == terrainColour(features, travelX, travelY, family, colourMapFor(s->window.engine)));
				++checked;
				if (s->rows < TerrainMap::kResolution) {
					++partial;
				}
			}
			if (s->window == w && s->rows >= TerrainMap::kResolution) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
	}
	// The probe has to have actually looked at something, or it proves nothing. The loop cannot exit an
	// attempt without seeing the finished map, so this is a floor the structure guarantees rather than
	// a hope about timing.
	CHECK(checked >= 12);
	MESSAGE("snapshots checked: " << checked << " (" << partial << " mid-build)");
}

TEST_CASE("a sound's own plane reaches the terrain, and reaching it is not the same as changing it") {
	// THE BUG: rotate appeared not to work at all, or at least did not redraw the background.
	// Reproduced in pixels first — the terrain interior was BYTE-IDENTICAL between no rotation and a
	// hard turn while the corner's dial moved. Two faults, one root:
	// the window carried only the GLOBAL audition pin, so the map could not respond to a per-sound turn
	// and could not even be asked to, because nothing in its key had changed.
	//
	// Compounded by the guarantee that makes rotate honest: at home with no drift it is AUDIBLY inert
	// by construction. With the map frozen too, the verb had no feedback anywhere but its own dial.
	//
	// TWO ASSERTIONS BECAUSE THEY ARE TWO FAULTS. Carrying the pin in the key without consuming it
	// would rebuild diligently and draw the same map every time.
	SliceOrientationPin turned;
	turned.pinned = true;
	turned.generator.rotate = 2.5f;

	// (a) THE PIN IS LOAD-BEARING in the evaluation: same position, different plane, different sound.
	//     Away from zero travel, because at zero travel every plane gives the same character BY
	//     CONSTRUCTION and this would pass on a build that ignored the pin entirely.
	const TerrainFeatures derived = terrainFeaturesAt(OscType::Morph, SliceOrientationPin {}, 512, 0.0f, 0.03, 0.0);
	const TerrainFeatures pinned = terrainFeaturesAt(OscType::Morph, turned, 512, 0.0f, 0.03, 0.0);
	const float moved = std::abs(derived.energy - pinned.energy)
	                    + std::abs(derived.centroid - pinned.centroid)
	                    + std::abs(derived.roughness - pinned.roughness);
	CHECK(moved > 0.001f);
	// And at zero travel it must NOT move — the guarantee, and the reason the check above is off-axis.
	const TerrainFeatures homeDerived = terrainFeaturesAt(OscType::Morph, SliceOrientationPin {}, 512, 0.0f, 0.0, 0.0);
	const TerrainFeatures homePinned = terrainFeaturesAt(OscType::Morph, turned, 512, 0.0f, 0.0, 0.0);
	CHECK(homeDerived.energy == doctest::Approx(homePinned.energy));

	// (b) AND THE MAP IS ASKED TO REBUILD, because the pin is part of the window's identity.
	TerrainMap map;
	TerrainWindow plain = windowAt(0.5, 64.0 / 1023.0, 64.0 / 1023.0);
	map.request(plain);
	REQUIRE(settle(map, plain));
	const uint64_t askedOnce = map.requests();
	TerrainWindow rotated = plain;
	rotated.pin = turned;
	CHECK_FALSE(plain == rotated);
	map.request(rotated);
	CHECK(map.requests() > askedOnce);
	REQUIRE(settle(map, rotated));
	// The two maps differ somewhere off the travel-zero column.
	CHECK(map.snapshot()->texel(100, 64) != 0u);
}

TEST_CASE("the rotate corner puts the sound's plane into the terrain it asks for") {
	// The surface half of the same bug: the window it builds has to carry the plane the sound is on,
	// or (a) and (b) above are true of a code path nobody reaches.
	ExploreFixture f;
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	REQUIRE_FALSE(f.terrain.snapshot()->window.pin.pinned);
	const uint64_t before = f.terrain.requests();

	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 60.0f));
	f.stage.onMouseUp().callback(at(centre.x, centre.y - 60.0f));
	f.stage.draw(canvas);

	// A turn asks for a new map...
	CHECK(f.terrain.requests() > before);
	// ...and the map it asks for is on the sound's new plane. WAITED FOR, not read immediately: the
	// last COMPLETED snapshot is deliberately what a reader gets while the next one builds, so checking
	// straight after the request reads the old window and calls a working build broken.
	bool arrived = false;
	for (int waited = 0; waited < 20000 && !arrived; ++waited) {
		arrived = f.terrain.snapshot()->window.pin.pinned;
		if (!arrived) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	CHECK(arrived);
	CHECK(f.terrain.snapshot()->window.pin == f.bridge.soundOrientations().pin[0]);
}

TEST_CASE("every breadcrumb is placed from its own identity, not from the live sound") {
	// The symptom: moving the explore point translated the entire history constellation to the
	// coordinate of the destination. Reproduced in pixels first — five crumbs planted at five
	// DIFFERENT drifts drew as one perfectly vertical line down the middle of the disk.
	//
	// The storage was never wrong: a crumb is a full identity, drift included. The PROJECTION threw the
	// horizontal half away and substituted the live sound's travel, one number shared by every crumb —
	// so the trail collapsed into a column and slid sideways with whatever the sound was doing now.
	ExploreFixture f;
	f.selection.select(1);
	const visage::Point centre = f.stage.disk().centre();

	// Three gestures at three different places, each planting a crumb where it LEFT.
	const float xs[3] = { centre.x - 60.0f, centre.x + 10.0f, centre.x + 70.0f };
	const float ys[3] = { centre.y - 40.0f, centre.y + 20.0f, centre.y - 10.0f };
	for (int i = 0; i < 3; ++i) {
		f.stage.onMouseDown().callback(at(xs[i], ys[i]));
		f.stage.onMouseUp().callback(at(xs[i], ys[i]));
		f.settleGesture();
	}
	const std::vector<SoundIdentity> trail = f.stage.crumbs(1);
	REQUIRE(trail.size() == 3);
	// The identities really do differ on BOTH axes, or the projection check below proves nothing.
	REQUIRE(trail[0].driftX != doctest::Approx(trail[1].driftX));
	REQUIRE(trail[1].driftX != doctest::Approx(trail[2].driftX));
	// The VERTICAL difference is now the BASE rather than the zone — a scrub moves the sound's unbounded
	// gamma base and leaves the dials alone (the one-value design) — so this is the same requirement
	// as before, asked of the coordinate that actually carries it. Getting this wrong would leave the
	// projection check below comparing three crumbs that are all at the same height.
	// **THE CRUMBS DIFFER IN THEIR PLACE, not in their gamma** (backlog GY): the chart writes the
	// decoded plane now, so two presses at different heights leave two crumbs at different Y travels
	// and identical gammas. This is the non-vacuity guard for everything below, so it has to name the
	// coordinate that actually differs or the case proves nothing.
	INFO("crumb Y travels: " << trail[0].driftY << " and " << trail[1].driftY);
	REQUIRE(trail[0].driftY != doctest::Approx(trail[1].driftY));

	// EACH CRUMB AT ITS OWN PLACE. Distinct horizontally — which is exactly what collapsed — and each
	// one where its own stored drift puts it, with nothing of the live sound in it.
	//
	// **THE REFERENCE IS THE CRUMB'S OWN TWO TRAVELS** (backlog GY). It used to reconstruct the
	// crumb's HOME — zone, offset, vernier, base and global gamma summed — because the vertical axis
	// was home. Both axes are travel now, and `SoundIdentity` has always stored both components; the
	// projection simply threw the second away. So the reference is the stored place itself, and the
	// elaborate home sum it replaces was only ever a way of asking where that place was drawn.
	for (size_t i = 0; i < trail.size(); ++i) {
		CHECK(f.stage.crumbAt(i).x == doctest::Approx(f.stage.xAtTravelX(trail[i].driftX)));
		CHECK(f.stage.crumbAt(i).y == doctest::Approx(f.stage.yAtTravelY(trail[i].driftY)));
	}
	CHECK(f.stage.crumbAt(0).x != doctest::Approx(f.stage.crumbAt(1).x));
	CHECK(f.stage.crumbAt(1).x != doctest::Approx(f.stage.crumbAt(2).x));

	// AND THE TRAIL STAYS PUT WHEN THE SOUND MOVES ON. Drifting the sound moves the star, not the
	// history — the whole point of a trail.
	const double wasDrift = f.stage.crumbs(1)[0].driftX;
	f.stage.onMouseDown().callback(at(centre.x + 100.0f, centre.y));
	f.stage.onMouseDrag().callback(at(centre.x + 120.0f, centre.y));
	f.stage.onMouseUp().callback(at(centre.x + 120.0f, centre.y));
	f.settleGesture();
	// THE CRUMB DID NOT MOVE, said in the coordinate it is stored in. This used to compare PIXELS across
	// the gesture, which held only while the chart had no horizontal view centre — the release now
	// re-centres on the sound's new drift, so every crumb's pixel legitimately slides while no crumb
	// moves. Its stored drift is untouched, and it is still drawn exactly where that drift puts it.
	CHECK(f.stage.crumbs(1)[0].driftX == doctest::Approx(wasDrift));
	CHECK(f.stage.crumbAt(0).x == doctest::Approx(f.stage.xAtTravelX(wasDrift)));

	// AND WEATHER DOES NOT SLIDE THE TRAIL EITHER. The projection used to add the LIVE performance
	// term to every crumb — deliberately, so the trail shared the star's frame — which meant turning
	// Weather up or automating the puck translated the whole history sideways. The design makes the
	// chart identity-only, so a crumb is where that sound WAS and where you are standing has nothing
	// to say about it.
	const float heldAt = f.stage.crumbAt(0).x;
	f.set(ParamId::Weather, 1.0);
	f.set(ParamId::PosX, 0.45);
	CHECK(f.stage.crumbAt(0).x == doctest::Approx(heldAt));
	CHECK(f.stage.crumbAt(2).x != doctest::Approx(f.stage.crumbAt(0).x));
}

TEST_CASE("Explore zooms on the wheel") {
	// The symptom: touchpad gesture zoom did nothing on Explore. The first fact is blunter than the gesture
	// question — the surface answered NO scroll event at all. Zoom was a corner-knob drag and nothing
	// else, and the only wheel handler in the whole editor was the sound stack's.
	//
	// The SYNTHESIS CAVEAT, stated where it applies: this exercises the handler, not the delivery.
	// visage's own event entry point is an ordinary function call, so what a wheel event DOES is
	// testable here; whether a touchpad produces one is a question about X11 and libinput that no
	// in-process test can answer. See the header, and FEATHERS_TRACE_WHEEL.
	ExploreFixture f;
	const visage::Point centre = f.stage.disk().centre();
	const auto wheel = [&](float dy) {
		visage::MouseEvent e = at(centre.x, centre.y);
		e.wheel_delta_y = dy;
		e.precise_wheel_delta_y = dy;
		return f.stage.mouseWheel(e);
	};

	REQUIRE(f.stage.zoom() == doctest::Approx(1.0));
	// Up is in, by one notch — a quarter of the mock's ×1.6.
	CHECK(wheel(1.0f));
	CHECK(f.stage.zoom() == doctest::Approx(ExploreStage::kZoomStep));
	// GEOMETRIC, not additive: the second notch multiplies again, so a notch means the same thing at
	// x1 and at x32. An additive step would spend most of its travel at the top of the range.
	CHECK(wheel(1.0f));
	CHECK(f.stage.zoom() == doctest::Approx(ExploreStage::kZoomStep * ExploreStage::kZoomStep));
	// And back down the same ladder, to exactly where it started.
	CHECK(wheel(-1.0f));
	CHECK(wheel(-1.0f));
	CHECK(f.stage.zoom() == doctest::Approx(1.0));

	// It stops at the ends rather than running away, and SAYS SO. Scrolling past an end used to return
	// true — claiming the event — while `setZoom` returned early without repainting, so the surface
	// painted nothing while the hand was actively scrolling over it. Anything that had corrupted the
	// region then had nothing to repaint it, which is the shape of the zoom-out-past-maximum recipe.
	for (int i = 0; i < 160; ++i) { // four notches make the old one
		wheel(1.0f);
	}
	CHECK(f.stage.zoom() == doctest::Approx(ExploreStage::kMaxZoom));
	CHECK_FALSE(wheel(1.0f));
	for (int i = 0; i < 160; ++i) {
		wheel(-1.0f);
	}
	CHECK(f.stage.zoom() == doctest::Approx(ExploreStage::minZoom()));
	CHECK_FALSE(wheel(-1.0f));
	CHECK_FALSE(wheel(0.0f));

	// A zoom is a view change, so it must not touch the patch — no parameter, no drift, no plane.
	CHECK(drain(f.bridge).empty());
	CHECK(f.bridge.soundTravel().offset[0].x == doctest::Approx(0.0));
	CHECK_FALSE(f.bridge.soundOrientations().pin[0].pinned);
}

namespace {
double projectionMiss(const TerrainWindow& window, std::span<const double> point, double x, double y);
} // namespace

TEST_CASE("local gamma is identity and global gamma is the frame") {
	// Local gamma is recorded with undo; global gamma is not. Local gamma is
	// a component of HOME — turning it IS identity motion — so it belongs to the sound and to its
	// history. Global gamma is a macro over every sound, so it slides the whole frame and belongs to
	// the ordinary parameter history the host already keeps.
	//
	// THREE HALVES, VERIFIED SEPARATELY, because "true in storage, false in replay" is the shape that
	// hides here.
	//
	// SINCE THE VERNIER LANDED the corner carries gamma's unbounded BASE rather than its ±4 parameter,
	// so this reads `gammaBase` where it used to read `gamma`. Every claim it makes is unchanged and
	// every reason for them is unchanged: the base is local gamma's coarse half, it is identity, and
	// it is what the corner turns.
	ExploreFixture f;
	f.selection.select(1);
	const visage::Point centre = f.stage.disk().centre();

	// Leave a crumb, then turn the LOCAL gamma corner.
	f.stage.onMouseDown().callback(at(centre.x + 30.0f, centre.y - 30.0f));
	f.stage.onMouseUp().callback(at(centre.x + 30.0f, centre.y - 30.0f));
	f.settleGesture();
	REQUIRE(f.stage.crumbs(1).size() == 1);
	// IN IDENTITY SPACE, NOT SCREEN SPACE. The view re-centres on the sound when a gesture ends, so
	// screen position says who the camera is following, not what moved — a first draft of this case
	// asserted screen invariance and failed for that reason rather than for a real one. The claim the
	// design makes is about the identities: a crumb keeps the gamma it was left at, so the DISTANCE
	// between where you are and where you have been grows by exactly the turn.
	const double crumbHomeBefore =
	    homeOf(f.stage.crumbs(1)[0].zone, f.stage.crumbs(1)[0].offset,
	           f.stage.crumbs(1)[0].gamma + f.stage.crumbs(1)[0].gammaBase);
	const double starHomeBefore = f.stage.selectedHome();

	// WHAT THE BASE WAS BEFORE THE CORNER MOVED IT, because the crumb-planting press above is a SCRUB
	// and a scrub now writes the base itself (the one-value design). This case used to be able to
	// say "the base is zero until the corner turns"; it no longer is, and every claim below is about
	// what the CORNER did, so each one is now stated as a difference from here. Written as a captured
	// baseline rather than a literal so it cannot quietly become true again for the wrong reason.
	const double baseBeforeTurn = f.stage.identity().gammaBase;

	const visage::Bounds gammaBox = f.stage.partBounds(ExploreStage::Part::Gamma);
	const visage::Point gammaAt { gammaBox.x() + gammaBox.width() * 0.5f,
		                          gammaBox.y() + gammaBox.height() * 0.5f };
	f.stage.onMouseDown().callback(at(gammaAt.x, gammaAt.y));
	f.stage.onMouseDrag().callback(at(gammaAt.x, gammaAt.y - 30.0f));
	f.stage.onMouseUp().callback(at(gammaAt.x, gammaAt.y - 30.0f));
	f.settleGesture();
	const double turned = f.stage.identity().gammaBase - baseBeforeTurn;
	REQUIRE(turned != doctest::Approx(0.0));

	// (1) THE TRAIL IS INVARIANT UNDER LOCAL GAMMA, and the star is not. Each crumb carries the gamma
	//     it was left at, so turning the corner moves where you ARE and leaves where you have BEEN.
	const double crumbHomeAfter =
	    homeOf(f.stage.crumbs(1)[0].zone, f.stage.crumbs(1)[0].offset,
	           f.stage.crumbs(1)[0].gamma + f.stage.crumbs(1)[0].gammaBase);
	CHECK(crumbHomeAfter == doctest::Approx(crumbHomeBefore));
	CHECK(f.stage.selectedHome() != doctest::Approx(starHomeBefore));
	// And they part company by exactly the turn.
	CHECK(f.stage.selectedHome() - crumbHomeAfter
	      == doctest::Approx((starHomeBefore - crumbHomeBefore) + turned));

	// AND THE PROJECTION USES THAT STORED GAMMA, which is a different claim from the three above and
	// needs saying separately: they are all about what is STORED, and a build that stored the crumb's
	// own gamma and then drew it with the LIVE one passes every one of them. (It did — the known-bad
	// for exactly that stayed green until this assertion existed. The lead warned about
	// true-in-storage-false-in-replay and I wrote the mirror image of it.)
	// **A CRUMB IS DRAWN AT ITS OWN POINT'S PROJECTION** (overriding backlog GY's "gamma does not enter
	// the projection"): the crumb's identity decodes to a point in the full space, and the chart draws where that point
	// lands on the plane being drawn — which a gamma move pans it across by the plane's alignment with the home direction.
	// Still drawn from what the crumb STORED, never from the live sound: by value, against the projection itself.
	const PlanePlace crumbLands = f.stage.crumbPlace(0);
	CHECK_FALSE(crumbLands.exact); // the gamma move changed the plane, so it was projected
	CHECK(f.stage.crumbAt(0).x == f.stage.sheetAtWorld(crumbLands.x, crumbLands.y).x);
	CHECK(f.stage.crumbAt(0).y == f.stage.sheetAtWorld(crumbLands.x, crumbLands.y).y);
	CHECK(std::isfinite(f.stage.crumbAt(0).x));
	CHECK(std::isfinite(f.stage.crumbAt(0).y));
	{
		// THE PROJECTION BY ITS PROPERTY (projectionMiss, the PLACES section): the crumb's point, decoded on its own plane.
		const SoundIdentity c = f.stage.crumbs(1)[0];
		TerrainWindow own = f.stage.terrainWindowNow();
		own.pin = c.plane.pinned ? c.plane : f.bridge.orientation();
		own.residue = c.residue;
		own.gammaBase = c.gammaBase;
		own.zoneBaseline = c.zone;
		own.offsetBaseline = c.offset;
		std::array<double, kTerrainLikePoint> point {};
		const size_t rows = terrainPlacePoint(own, c.driftX, c.driftY, point);
		REQUIRE(rows > 0);
		CHECK(projectionMiss(f.stage.terrainWindowNow(), std::span(point).first(rows), crumbLands.x, crumbLands.y) < 1e-9);
	}
	// Which is NOT where the LIVE sound sits, or the assertion above is satisfied by accident — the
	// non-vacuity half, restated on the coordinate that now carries the claim.
	CHECK(f.stage.selectedDriftY() != doctest::Approx(f.stage.crumbs(1)[0].driftY));

	// (2) AND IT LOCKS IN WITH UNDO — the actual ask. The gamma corner's edit is in this surface's
	//     history, and retracing restores the gamma with the rest of the identity.
	const SoundIdentity afterTurn = f.stage.identity();
	REQUIRE(f.stage.canUndo());
	f.stage.undo();
	f.settleGesture();
	// Back to what the SCRUB left, not to zero — undoing the corner's edit must not also undo the
	// gesture before it.
	CHECK(f.stage.identity().gammaBase == doctest::Approx(baseBeforeTurn));
	f.stage.redo();
	f.settleGesture();
	CHECK(f.stage.identity().gammaBase == doctest::Approx(afterTurn.gammaBase));

	// (3) GLOBAL GAMMA IS A FRAME SLIDE: star and trail move TOGETHER, and it is not stored per crumb.
	//
	// RESTATED IN HOME RATHER THAN IN PIXELS, and the reason is a real behaviour change rather than a
	// fixture repair. This used to assert that the star SLID ON SCREEN, which was true and was also the
	// shape of a defect: the reachable band is `[gammaSum, gammaSum + 3]`, so any gamma — global
	// included — drags the whole world along with it, and a view that stayed put while it moved ended up
	// outside the world entirely with nothing to draw. `viewCentre()` now follows gamma, so the frame
	// slides WITH the picture and neither the star nor the trail changes pixel. The claim this case was
	// making is intact and is about togetherness, so it is made where the movement now lives: both homes
	// shift by exactly the global delta, and both screen positions hold. Asserting the pixel shift was
	// never the point; it was the available proxy.
	const visage::Point crumbAtGlobalZero = f.stage.crumbAt(0);
	const double starAtGlobalZero = f.stage.yAtTravelY(f.stage.selectedDriftY());
	// Every crumb's LOCAL gammas as they stand before the global frame moves — the reference for the
	// not-captured-into-identity claim below.
	std::vector<std::pair<double, double>> localGammasBefore;
	for (const SoundIdentity& crumb : f.stage.crumbs(1)) {
		localGammasBefore.emplace_back(crumb.gamma, crumb.gammaBase);
	}
	const double globalCrumbHomeBefore = homeOf(f.stage.crumbs(1)[0].zone, f.stage.crumbs(1)[0].offset,
	                                      f.stage.crumbs(1)[0].gamma
	                                          + f.stage.crumbs(1)[0].gammaBase
	                                          + f.bridge.value(static_cast<clap_id>(
	                                              ParamId::GlobalGamma)));
	const double globalStarHomeBefore = f.stage.selectedHome();
	f.set(ParamId::GlobalGamma, 0.25);
	const double globalCrumbHomeAfter = homeOf(f.stage.crumbs(1)[0].zone, f.stage.crumbs(1)[0].offset,
	                                     f.stage.crumbs(1)[0].gamma
	                                         + f.stage.crumbs(1)[0].gammaBase
	                                         + f.bridge.value(static_cast<clap_id>(
	                                             ParamId::GlobalGamma)));
	const double crumbHomeShift = globalCrumbHomeAfter - globalCrumbHomeBefore;
	const double starHomeShift = f.stage.selectedHome() - globalStarHomeBefore;
	// The frame really moved, by the amount asked for — so what follows is not satisfied by nothing
	// having happened.
	CHECK(crumbHomeShift == doctest::Approx(0.25));
	CHECK(crumbHomeShift == doctest::Approx(starHomeShift));
	// And it moved as ONE FRAME: the view came with it, so nothing slid across the chart.
	const double crumbShift = f.stage.crumbAt(0).y - crumbAtGlobalZero.y;
	const double starShift = f.stage.yAtTravelY(f.stage.selectedDriftY()) - starAtGlobalZero;
	CHECK(crumbShift == doctest::Approx(0.0));
	CHECK(starShift == doctest::Approx(0.0));
	CHECK(crumbShift == doctest::Approx(starShift));
	// NOT CAPTURED INTO THE IDENTITY: a crumb records the sound, and global gamma is not the sound's.
	// Asserted as "unchanged by the global move" rather than as "zero", because a crumb's base is no
	// longer zero — a scrub writes it. That is also the stronger reading of the claim: what matters is
	// that moving the GLOBAL frame leaves every LOCAL number exactly where it was, and a literal zero
	// could go on passing here for reasons that have nothing to do with global gamma.
	for (size_t i = 0; i < f.stage.crumbs(1).size(); ++i) {
		CHECK(f.stage.crumbs(1)[i].gamma == doctest::Approx(localGammasBefore[i].first));
		CHECK(f.stage.crumbs(1)[i].gammaBase == doctest::Approx(localGammasBefore[i].second));
	}
	// And this surface's undo does not reach for it — it belongs to the host's parameter history.
	const double globalNow = f.bridge.value(static_cast<clap_id>(ParamId::GlobalGamma));
	f.stage.undo();
	f.settleGesture();
	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::GlobalGamma)) == doctest::Approx(globalNow));
}

TEST_CASE("a wheel zoom rebuilds the terrain with no gesture to end") {
	// THE SYMPTOM, at x10: the whole disk a horizontally-stretched low-res smear — the old 128x128 snapshot
	// drawn over a window it does not belong to, with the re-evaluated map never arriving.
	//
	// A WHEEL HAS NO RELEASE. Every other terrain-affecting input on this surface ends with a mouse-up,
	// and the rebuild used to be gated on no gesture being in flight — which meant it fired on that
	// mouse-up. Scroll zoom just STOPS ARRIVING, so on the build that showed the smear the rebuild had no
	// event to hang off and the stretched interim was the final state, indefinitely.
	//
	// The gate is gone, so this asserts the general policy rather than a wheel special case: the window
	// changed, therefore a build is asked for, with nothing pressed and nothing released.
	ExploreFixture f;
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	const uint64_t requestsBefore = f.terrain.requests();
	const uint64_t buildsBefore = f.terrain.builds();

	const visage::Point centre = f.stage.disk().centre();
	visage::MouseEvent wheel = at(centre.x, centre.y);
	wheel.wheel_delta_y = 1.0f;
	wheel.precise_wheel_delta_y = 1.0f;
	for (int i = 0; i < 20; ++i) { // five of the old ×1.6 notches
		REQUIRE(f.stage.mouseWheel(wheel));
	}
	REQUIRE(f.stage.zoom() > 5.0);
	// NO onMouseDown, NO onMouseUp anywhere in this case. Only a draw, which is what a frame does.
	f.stage.draw(canvas);

	CHECK(f.terrain.requests() > requestsBefore);
	bool built = false;
	for (int waited = 0; waited < 4000 && !built; ++waited) {
		built = f.terrain.builds() > buildsBefore;
		if (!built) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	CHECK(built);

	// And it settles on the zoom that was actually reached, not on one the wheel passed through.
	bool arrived = false;
	for (int waited = 0; waited < 8000 && !arrived; ++waited) {
		const std::shared_ptr<const TerrainSnapshot> now = f.terrain.snapshot();
		arrived = now->rows >= TerrainMap::kResolution
		          && now->window.travelYSpan == doctest::Approx(ExploreStage::builtSpan(f.stage.homeSpanZones() / 1023.0));
		if (!arrived) {
			f.stage.draw(canvas);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	CHECK(arrived);
}

TEST_CASE("the widest stop is as far out as the picture still says something") {
	// The zoom control needs range further out than one neighbourhood.
	// THAT DEFECT HAS NEVER CHANGED and neither has what this case is for: a ladder that
	// stops at one neighbourhood is a keyhole. What has changed three times is WHAT DECIDES where it
	// stops, and the chain is worth having in one place:
	//
	//   1. ×1 — 64 zones. No reasoning, just where the ladder happened to end. Too narrow; replaced.
	//   2. THE BAND — 3069 zones, "and not a step wider", because the view clamp held the chart inside
	//      what Zone and Offset could address and a wider stop would have spent the extra on nowhere.
	//   3. the infinite world deleted "nowhere": every home draws. That left the stop anchored to a
	//      quantity that no longer meant anything, and the honest replacement was legibility — how far
	//      out the picture still says something. Measured, the answer was humbling: the map was
	//      ALREADY past its limit at the band stop, and stair's glyph pattern had a Nyquist span of
	//      2208 zones against the 3069 we shipped.
	//   4. SPAN AVERAGING moved the limit rather than merely finding it. A texel reports the mean of
	//      its own cell, so a feature finer than a texel is summarised instead of sampled, and what
	//      bounds the view is now the slow orientation field the wide picture is made of — measured
	//      legible out past this stop with a twofold margin.
	//
	// So the claim is no longer "exactly the band" but "sixteen times it, and the reason is written
	// down and measured". The original defect is still guarded: the widest stop is far wider than the
	// view this surface opens at, and the ladder still has an end.
	ExploreFixture f;
	const double band = homeCeiling(0.0) - homeFloor(0.0);
	REQUIRE(band > 0.0);

	f.stage.setZoom(ExploreStage::minZoom());
	CHECK(f.stage.zoom() < 1.0); // the whole point: further OUT than the old floor
	INFO("widest span " << f.stage.homeSpanZones() << " zones against a band of " << band * 1023.0);
	CHECK(f.stage.homeSpanZones() == doctest::Approx(ExploreStage::kWidestSpanZones));
	// Sixteen times the band it used to end at — the ladder genuinely reaches somewhere new.
	CHECK(f.stage.homeSpanZones() > band * 1023.0 * 8.0);

	// AND WHAT IS OUT THERE IS A TEXEL THAT CANNOT RESOLVE ITS OWN CELL, which is the honest statement
	// of the stop after the decision. Averaging is what made the wide view defensible, and the
	// unsmoothed view was preferred over it — so the wide
	// stop ships raw, and this asserts the fact rather than the remedy: at this span a cell is far past
	// what either engine's finest feature can be sampled at, and the schedule says so while the cap
	// declines to act on it.
	const double cell = f.stage.homeSpanZones() / static_cast<double>(TerrainMap::kResolution);
	INFO("one texel covers " << cell << " zones, against a finest feature of 169 (morph) and 34.5 (stair)");
	CHECK(cell > 169.0 * 0.5);
	CHECK(cell > 34.5 * 0.5);
	CHECK(terrainSamplesFor(OscType::Morph, cell) == 1);
	CHECK(terrainSamplesFor(OscType::Stair, cell) == 1);

	// The view really does span what it says, top to bottom — the arithmetic behind every reading on
	// this chart, and the half of the old case that was never about the band.
	const StageDisk d = f.stage.disk();
	const float top = d.centre().y - d.radius();
	const float bottom = d.centre().y + d.radius();
	CHECK((f.stage.travelYAtScreenY(top) - f.stage.travelYAtScreenY(bottom)) * 1023.0
	      == doctest::Approx(ExploreStage::kWidestSpanZones).epsilon(0.01));

	// A wheel notch out from there changes nothing — the ladder has an end, and setZoom says so.
	const double atFloor = f.stage.zoom();
	f.stage.setZoom(atFloor / 4.0);
	CHECK(f.stage.zoom() == doctest::Approx(atFloor));
}

TEST_CASE("the gridlines stay countable at the new stop, and unmoved at every old one") {
	// THE LADDER HAD TO GROW WITH THE VIEW. A 64-zone step draws about 48 lines over the 3069-zone stop
	// this surface used to end at, which is the density already judged right by eye; the same step
	// at the new stop would draw 768 of them — not a grid but a grey wash, and a few hundred more
	// strings for the font atlas, which is the environment every wedge sighting correlates with.
	//
	// AND NOTHING BELOW THE OLD STOP MOVED, which is the half worth testing hardest: the new rungs
	// start above 3072 zones, so every view anyone has ever seen draws exactly the lines it drew.
	for (const double span : { 1.0, 5.0, 6.0, 24.0, 96.0, 500.0, 3069.0, 3072.0 }) {
		INFO("span " << span);
		const double was = span <= 6.0 ? 1.0 : (span <= 24.0 ? 4.0 : (span <= 96.0 ? 16.0 : 64.0));
		CHECK(ExploreStage::gridLineStep(span) == doctest::Approx(was));
	}
	// Above it, the count stays where it was approved rather than the step staying where it was.
	for (const double span : { 3069.0, 6000.0, 12288.0, 24576.0, ExploreStage::kWidestSpanZones }) {
		const double lines = span / ExploreStage::gridLineStep(span);
		INFO("span " << span << ": step " << ExploreStage::gridLineStep(span) << ", " << lines
		             << " lines");
		CHECK(lines <= 48.0);
		CHECK(lines >= 8.0);
	}
	// And labels remain a handful of ticks landing ON lines, at the new spans as at the old ones — the
	// label ladder is derived from this one, so widening the view must not paper the chart again.
	for (double span = 1.0; span <= ExploreStage::kWidestSpanZones; span *= 1.7) {
		const double lines = ExploreStage::gridLineStep(span);
		const double labels = ExploreStage::gridLabelStep(span);
		INFO("span " << span << ": line step " << lines << ", label step " << labels);
		CHECK(labels >= lines);
		CHECK(std::abs(std::remainder(labels, lines)) < 1e-9);
		CHECK(span / labels <= ExploreStage::kWantedGridLabels + 1.0);
	}
}

TEST_CASE("the zone labels are axis ticks at every zoom and every base") {
	// THE SCREENSHOT, 19:47: base +120.74 at x1/48, and the chart papered in labels — z125736,
	// z125760, z125784 … one per gridline, stacked ten pixels apart, unreadable. Two bugs in one:
	// nobody can read it, AND at that base every string is seven digits nobody has drawn before, which
	// is a few hundred novel glyphs per view. That is the heaviest font-atlas churn this plugin can
	// produce, and atlas churn is the environment every wedge sighting has been correlated with.
	ExploreFixture f;
	f.selection.select(0);
	visage::Canvas canvas;

	// The screenshot's own state, as near as a fixture can stand in it.
	f.stage.nudgeSelectedGamma(120.74);
	f.settleGesture();
	f.stage.setZoom(ExploreStage::minZoom());
	f.stage.draw(canvas);
	INFO("at x1/48 and base +120.74: " << f.stage.gridLabelCount() << " labels, line step "
	                                   << ExploreStage::gridLineStep(f.stage.homeSpanZones())
	                                   << ", label step "
	                                   << ExploreStage::gridLabelStep(f.stage.homeSpanZones()));
	// A HANDFUL. The old build drew one per line, which is about forty-eight here.
	CHECK(f.stage.gridLabelCount() <= 8);
	// And not none: a chart with no numbers on it is a different bug.
	CHECK(f.stage.gridLabelCount() >= 2);

	// AND THEY DO NOT VANISH WHERE THEY ARE USEFUL. At a tight zoom the span is a zone or two and the
	// labels are the only thing saying where you are.
	f.stage.setZoom(ExploreStage::kMaxZoom);
	f.stage.draw(canvas);
	INFO("at x64: " << f.stage.gridLabelCount() << " labels");
	CHECK(f.stage.gridLabelCount() >= 1);
	CHECK(f.stage.gridLabelCount() <= 8);

	// EVERY RUNG, not just the two ends — the bound is the claim, so it is checked across the ladder.
	for (double zoom = ExploreStage::minZoom(); zoom <= ExploreStage::kMaxZoom; zoom *= 1.6) {
		f.stage.setZoom(zoom);
		f.stage.draw(canvas);
		INFO("at zoom " << zoom << ": " << f.stage.gridLabelCount() << " labels");
		CHECK(f.stage.gridLabelCount() <= 8);
	}

	// A LABEL ALWAYS LANDS ON A LINE, by construction rather than by luck: the label step is a whole
	// multiple of the line step at every span this surface can reach.
	for (double span = 1.0; span <= 4096.0; span *= 1.37) {
		const double lines = ExploreStage::gridLineStep(span);
		const double labels = ExploreStage::gridLabelStep(span);
		INFO("span " << span << ": line step " << lines << ", label step " << labels);
		CHECK(labels >= lines);
		CHECK(std::abs(std::remainder(labels, lines)) < 1e-9);
		// And the count that step implies is the handful the surface promises.
		CHECK(span / labels <= ExploreStage::kWantedGridLabels + 1.0);
	}
}

TEST_CASE("a dice roll is a quarter of the visible view at every zoom, zoomed out included") {
	// NO CAP (supersedes 8560e420). A roll used to stop growing at x1, so zoomed out
	// it shrank to a speck of the view — 0.03% of it at the widest stop — and the moves sounded too
	// small, as if not zoom-scaled. Now the step is a quarter of what is on screen whatever the zoom,
	// and the old protection against one roll becoming a different sound is deliberately dropped.
	//
	// MEASURED ON THE ROLLS, not only on the readout: `mutate()` moves the plane's travel and nothing
	// else, so each axis's largest delta over many rolls is read against travelSpan() at each stop. The
	// dice is a seeded xorshift, so the rolls are the same every run; 200 uniform draws per axis put the
	// largest within 2% of the bound, and the 0.9 floor is what stops a roll capped at any smaller size
	// passing as "inside a quarter".
	ExploreFixture f;
	f.selection.select(1);
	for (const double zoom : { ExploreStage::minZoom(), 1.0 / 8.0, 1.0, 32.0 }) {
		f.stage.setZoom(zoom);
		const double quarter = f.stage.travelSpan() / 4.0;
		INFO("zoom " << f.stage.zoom() << ": the view spans " << f.stage.travelSpan() << " travel ("
		             << f.stage.homeSpanZones() << " zones)");
		CHECK(f.stage.mutationTravel() == doctest::Approx(quarter).epsilon(1e-12));
		CHECK(f.stage.mutationZones() == doctest::Approx(f.stage.homeSpanZones() / 4.0).epsilon(1e-12));

		double mostX = 0.0;
		double mostY = 0.0;
		for (int i = 0; i < 200; ++i) {
			const double beforeX = f.stage.selectedDrift();
			const double beforeY = f.stage.selectedDriftY();
			f.stage.mutate();
			f.settleGesture();
			mostX = std::max(mostX, std::abs(f.stage.selectedDrift() - beforeX));
			mostY = std::max(mostY, std::abs(f.stage.selectedDriftY() - beforeY));
		}
		INFO("largest roll: " << mostX / quarter << " of a quarter across, " << mostY / quarter << " up");
		// Never past a quarter of the view — the rounding allowance is for the stored double sum.
		CHECK(mostX <= quarter * (1.0 + 1e-9));
		CHECK(mostY <= quarter * (1.0 + 1e-9));
		// And it really reaches a quarter of THIS view, at this zoom.
		CHECK(mostX >= quarter * 0.9);
		CHECK(mostY >= quarter * 0.9);
	}
}

TEST_CASE("MEASURE: how fine the terrain's own features are, per axis and per engine"
          * doctest::skip()) {
	// THE INSTRUMENT BEHIND THE WIDEST STOP, run by hand and reported in the record. The stop used to be
	// anchored to the band, and the band stopped meaning anything when the world went infinite — so what
	// bounds it now is the only thing that ever really did: how fine the picture's own features are
	// against how much home one ROW of the map covers. The chart is 128 rows whatever the span, so the
	// span is a sampling rate and the question is Nyquist's.
	//
	// A SPECTRUM RATHER THAN A BANK PERIOD, deliberately. The record's ~180-zone figure was read off the
	// engine's declared banks, which is a statement about one input to the character rather than about
	// the character. What the map samples is `terrainFeaturesAt` — a built wavetable reduced to three
	// numbers — and its finest real structure is an empirical question with an empirical answer.
	const auto finestPeriod = [](const std::vector<float>& signal, double step, const char* what) {
		const feathers::test::Window window { static_cast<int>(signal.size()) };
		const std::vector<double> spectrum = feathers::test::amplitudeSpectrum(signal, window);
		double peak = 0.0;
		for (size_t bin = 1; bin < spectrum.size(); ++bin) {
			peak = std::max(peak, spectrum[bin]);
		}
		// The finest structure that is really THERE: the highest bin still holding a hundredth of the
		// strongest one. A threshold rather than the very last non-zero bin, because float noise reaches
		// every bin and would report the sampling step back at us.
		size_t highest = 1;
		for (size_t bin = 1; bin < spectrum.size(); ++bin) {
			if (spectrum[bin] >= peak * 0.01) {
				highest = bin;
			}
		}
		const double period = static_cast<double>(signal.size()) * step / static_cast<double>(highest);
		MESSAGE(what << ": finest period " << period << " (bin " << highest << " of "
		             << spectrum.size() << ")");
		return period;
	};

	constexpr int kSamples = 4096;
	SliceOrientationPin unpinned;
	for (const OscType engine : { OscType::Morph, OscType::Stair }) {
		const char* name = engine == OscType::Morph ? "morph" : "stair";
		// ALONG HOME, in zones, at a quarter-zone step. The window is 1024 zones — a third of the old
		// band — which is long enough to hold several periods of anything that matters here.
		const double homeStep = 0.25;
		std::vector<float> energy(kSamples);
		std::vector<float> centroid(kSamples);
		std::vector<float> rough(kSamples);
		for (int i = 0; i < kSamples; ++i) {
			const double zone = 300.0 + static_cast<double>(i) * homeStep;
			const TerrainFeatures features =
			    terrainFeaturesAt(engine, unpinned, static_cast<uint16_t>(std::lround(zone)), 0.0f, 0.0, 0.0,
			                      kNoPlaneResidue, zone / 1023.0 - std::lround(zone) / 1023.0);
			energy[static_cast<size_t>(i)] = features.energy;
			centroid[static_cast<size_t>(i)] = features.centroid;
			rough[static_cast<size_t>(i)] = features.roughness;
		}
		const double home = std::min({ finestPeriod(energy, homeStep, "home/energy"),
		                               finestPeriod(centroid, homeStep, "home/centroid"),
		                               finestPeriod(rough, homeStep, "home/roughness") });

		// ALONG TRAVEL, in travel units, over one kBaseTravel — the width the chart spans at x1. Both
		// axes divide by the same zoom (A13), so whichever is finer against its own span is the one that
		// decides the stop.
		const double travelStep = ExploreStage::kBaseTravel / static_cast<double>(kSamples);
		for (int i = 0; i < kSamples; ++i) {
			const double travel = static_cast<double>(i) * travelStep - ExploreStage::kBaseTravel * 0.5;
			const TerrainFeatures features =
			    terrainFeaturesAt(engine, unpinned, 512, 0.0f, travel, 0.0, kNoPlaneResidue, 0.0);
			energy[static_cast<size_t>(i)] = features.energy;
			centroid[static_cast<size_t>(i)] = features.centroid;
			rough[static_cast<size_t>(i)] = features.roughness;
		}
		const double travel = std::min({ finestPeriod(energy, travelStep, "travel/energy"),
		                                 finestPeriod(centroid, travelStep, "travel/centroid"),
		                                 finestPeriod(rough, travelStep, "travel/roughness") });

		// What that means for the ladder: how wide a span may be before one row is half a period.
		const double rowsPerMap = static_cast<double>(TerrainMap::kResolution);
		MESSAGE(name << ": finest home period " << home << " zones, finest travel period " << travel
		             << " travel units");
		MESSAGE(name << ": Nyquist-limited span = " << home * 0.5 * rowsPerMap << " zones (home), "
		             << travel * 0.5 * rowsPerMap << " travel units (travel = "
		             << travel * 0.5 * rowsPerMap / ExploreStage::kBaseTravel << " x kBaseTravel)");
		MESSAGE(name << ": at the shipped widest (3069 zones) one row is " << 3069.0 / rowsPerMap
		             << " zones = " << home / (3069.0 / rowsPerMap) << " samples per finest period");
	}
}

TEST_CASE("MEASURE: at what span the drawn map stops being a picture and becomes noise"
          * doctest::skip()) {
	// THE INSTRUMENT THAT ACTUALLY DECIDES THE STOP, and it exists because the one above answers a
	// question this map cannot use. A spectrum assumes a band-limited signal and this field is not one:
	// the character has genuine STEP DISCONTINUITIES in it — the morph's active-waypoint count changes
	// as accumulated phase passes its maximum, the stair's family changes at every 128th zone — and a
	// step has energy at every frequency, so "the finest period present" walks down towards the
	// sampling step and says nothing about how wide a view may be. NO zoom stop can avoid aliasing an
	// edge; only averaging over the span can draw one honestly, and that is deliberately step 2.
	//
	// SO ASK WHAT THE EYE ASKS: is a row still like the row above it? Terrain reads as terrain while
	// neighbours are related, and reads as noise once a row is no more like its neighbour than like a
	// place half a view away. That ratio needs no assumption about the signal, and it is what "features
	// thinner than a row skipping instead of blending" means once written down.
	//
	// Measured on the DRAWN COLOUR rather than on the features, because the colour is what is looked at
	// — grain and all. The grain is a deliberate fixed-frequency texture (`grainAt`) whose fastest
	// octave has a period near a sixth of a zone, so it is undersampled at EVERY rung including x1, by
	// design. It contributes a constant floor to this ratio rather than something a wider stop worsens.
	SliceOrientationPin unpinned;
	const auto luminance = [](uint32_t argb) {
		return (static_cast<double>((argb >> 16) & 0xFF) + static_cast<double>((argb >> 8) & 0xFF)
		        + static_cast<double>(argb & 0xFF))
		       / 3.0;
	};
	const auto colourAt = [&](OscType engine, double homeZones, double travel) {
		const auto zone = static_cast<uint16_t>(std::lround(homeZones));
		const double home = homeZones / 1023.0;
		const TerrainFeatures features =
		    terrainFeaturesAt(engine, unpinned, zone, 0.0f, travel, 0.0, kNoPlaneResidue,
		                      home - static_cast<double>(zone) / 1023.0);
		return terrainColour(features, home, travel,
		                     engine == OscType::Stair ? stairFamilyAt(zone) : -1);
	};
	const auto ratio = [](const std::vector<double>& v) {
		double adjacent = 0.0;
		for (size_t i = 1; i < v.size(); ++i) {
			adjacent += std::abs(v[i] - v[i - 1]);
		}
		adjacent /= static_cast<double>(v.size() - 1);
		// THE BASELINE IS THE SIGNAL'S OWN SPREAD, and the first draft of this instrument got that wrong
		// in a way worth keeping written down. It compared against pairs half a map apart — a fixed LAG
		// — and the moment half a map happened to be a whole number of periods of a periodic field,
		// those pairs sat at the same phase, the denominator collapsed, and the ratio read 10.4 where
		// the picture was perfectly smooth. I nearly reported that as aliasing. Mean absolute deviation
		// has no lag to be unlucky with: a smooth run takes small steps against a wide spread, and noise
		// takes steps the size of the spread itself.
		double mean = 0.0;
		for (const double x : v) {
			mean += x;
		}
		mean /= static_cast<double>(v.size());
		double spread = 0.0;
		for (const double x : v) {
			spread += std::abs(x - mean);
		}
		spread /= static_cast<double>(v.size());
		return spread > 0.0 ? adjacent / spread : 0.0;
	};

	for (const OscType engine : { OscType::Morph, OscType::Stair }) {
		const char* name = engine == OscType::Morph ? "morph" : "stair";
		for (const double spanZones : { 64.0, 1024.0, 3069.0, 12288.0, 24576.0, 49152.0, 98304.0, 196608.0 }) {
			const int rows = TerrainMap::kResolution;
			const double step = spanZones / static_cast<double>(rows);
			std::vector<double> down(static_cast<size_t>(rows));
			std::vector<double> across(static_cast<size_t>(rows));
			// Both axes, because both spans divide by the same zoom (A13) — whichever decorrelates first
			// is the one that decides the stop.
			const double travelSpan = ExploreStage::kBaseTravel * spanZones / 64.0;
			for (int i = 0; i < rows; ++i) {
				down[static_cast<size_t>(i)] =
				    luminance(colourAt(engine, 400.0 + static_cast<double>(i) * step, 0.0));
				across[static_cast<size_t>(i)] = luminance(
				    colourAt(engine, 400.0,
				             (static_cast<double>(i) / static_cast<double>(rows) - 0.5) * travelSpan));
			}
			// AND THE LOCAL LAYER SEPARATELY — the character's own channels, which is where the fine
			// structure lives. (When this was first measured the picture was a sum of two layers and the
			// MACRO one — the φ-derived orientation field for morph, the pattern family for stair — was
			// measured beside it. That field is deleted and the macro tint is gone from the draw, so the
			// drawn colour IS the local layer now, plus the grain.)
			std::vector<double> local(static_cast<size_t>(rows));
			for (int i = 0; i < rows; ++i) {
				const double homeZones = 400.0 + static_cast<double>(i) * step;
				const double home = homeZones / 1023.0;
				const auto zone = static_cast<uint16_t>(std::lround(homeZones));
				local[static_cast<size_t>(i)] =
				    terrainFeaturesAt(engine, unpinned, zone, 0.0f, 0.0, 0.0, kNoPlaneResidue,
				                      home - static_cast<double>(zone) / 1023.0)
				        .energy;
			}
			MESSAGE(name << " span " << spanZones << " zones (" << step
			             << " zones/row): drawn down " << ratio(down) << ", across " << ratio(across)
			             << " | local " << ratio(local));
		}
	}
}

TEST_CASE("MEASURE: how much averaging a row needs before the map reads as a picture again"
          * doctest::skip()) {
	// THE INSTRUMENT FOR THE "just do span averaging instead". The two cases above say what the
	// problem is: past a few zones a row, the drawn map's local layer is decorrelated — a row is no
	// more like its neighbour than like a place half a view away — and no choice of zoom stop fixes
	// that, because the character genuinely contains step discontinuities. Averaging is the fix: a row
	// that reports the MEAN of what it covers is honest blur instead of a lucky point sample.
	//
	// What this measures is the two numbers the design needs. HOW MUCH averaging (K samples a row)
	// buys back how much continuity, at the spans that matter; and therefore what it COSTS, since every
	// sample is a built wavetable and a spectrum — the same call the 31ms build already makes 16,384
	// times.
	//
	// AVERAGING THE FEATURES, NOT THE PARAMETERS AND NOT THE PIXELS. Averaging the engine's inputs
	// would be plainly wrong — the character is nonlinear in them, and the mean of two zones is not the
	// sound between them. Averaging the three FEATURE channels and colouring once at the end is the
	// perceptual mean the task asks for, and it leaves the two layers that are already honest alone:
	// the macro field (smooth at every span measured) and the grain (a deliberate fixed-frequency
	// texture that averaging would quietly erase).
	SliceOrientationPin unpinned;
	const auto luminance = [](uint32_t argb) {
		return (static_cast<double>((argb >> 16) & 0xFF) + static_cast<double>((argb >> 8) & 0xFF)
		        + static_cast<double>(argb & 0xFF))
		       / 3.0;
	};
	const auto ratio = [](const std::vector<double>& v) {
		double adjacent = 0.0;
		for (size_t i = 1; i < v.size(); ++i) {
			adjacent += std::abs(v[i] - v[i - 1]);
		}
		adjacent /= static_cast<double>(v.size() - 1);
		double mean = 0.0;
		for (const double x : v) {
			mean += x;
		}
		mean /= static_cast<double>(v.size());
		double spread = 0.0;
		for (const double x : v) {
			spread += std::abs(x - mean);
		}
		spread /= static_cast<double>(v.size());
		return spread > 0.0 ? adjacent / spread : 0.0;
	};

	for (const OscType engine : { OscType::Morph, OscType::Stair }) {
		const char* name = engine == OscType::Morph ? "morph" : "stair";
		for (const double spanZones : { 64.0, 3069.0, 12288.0, 49152.0 }) {
			const int rows = TerrainMap::kResolution;
			const double step = spanZones / static_cast<double>(rows);
			for (const int k : { 1, 4, 16, 64 }) {
				std::vector<double> down(static_cast<size_t>(rows));
				for (int i = 0; i < rows; ++i) {
					const double centre = 400.0 + static_cast<double>(i) * step;
					// The row's own span, sampled at K evenly spaced points inside it — a box filter,
					// which is what "the average over what this row covers" means before anyone argues
					// about window shapes.
					TerrainFeatures sum {};
					for (int s = 0; s < k; ++s) {
						const double at =
						    centre
						    + ((static_cast<double>(s) + 0.5) / static_cast<double>(k) - 0.5) * step;
						const auto zone = static_cast<uint16_t>(std::lround(at));
						const TerrainFeatures one =
						    terrainFeaturesAt(engine, unpinned, zone, 0.0f, 0.0, 0.0, kNoPlaneResidue,
						                      at / 1023.0 - static_cast<double>(zone) / 1023.0);
						sum.energy += one.energy;
						sum.centroid += one.centroid;
						sum.roughness += one.roughness;
					}
					const float scale = 1.0f / static_cast<float>(k);
					sum.energy *= scale;
					sum.centroid *= scale;
					sum.roughness *= scale;
					// Coloured ONCE, at the row's centre, so the macro field and the grain are exactly
					// what they are today and only the local layer has been summarised.
					const auto centreZone = static_cast<uint16_t>(std::lround(centre));
					down[static_cast<size_t>(i)] =
					    luminance(terrainColour(sum, centre / 1023.0, 0.0,
					                            engine == OscType::Stair ? stairFamilyAt(centreZone) : -1));
				}
				// ABSOLUTE, NOT NORMALISED, and the first draft of this instrument reported only the
				// ratio above — which cannot see the thing averaging actually does. Adjacent rows cover
				// DISJOINT intervals, so their means stay independent draws however large K is: the
				// shape stays white and the ratio barely moves (it even rises, since the denominator
				// shrinks too). What falls is the AMPLITUDE. Averaging does not make neighbours alike;
				// it lowers the noise floor until the slow field underneath shows through, which is a
				// different and better claim than the one the ratio was asked for.
				double adjacent = 0.0;
				for (size_t i = 1; i < down.size(); ++i) {
					adjacent += std::abs(down[i] - down[i - 1]);
				}
				adjacent /= static_cast<double>(down.size() - 1);
				double mean = 0.0;
				for (const double x : down) {
					mean += x;
				}
				mean /= static_cast<double>(down.size());
				double spread = 0.0;
				for (const double x : down) {
					spread += std::abs(x - mean);
				}
				spread /= static_cast<double>(down.size());
				MESSAGE(name << " span " << spanZones << " (" << step << " zones/row) K=" << k
				             << ": ratio " << ratio(down) << " | row-to-row step " << adjacent
				             << " of 255, spread " << spread);
			}
		}
	}
}

TEST_CASE("what the widest stop can and cannot show, measured from the engine's own fields") {
	// The spec: the widest zoom should show several periods of the lowest-wavelength phi
	// oscillations. This is that spec turned into numbers, taken from the engine rather than from the
	// plan's approximations — and it is a case rather than a note because it is the measurement that
	// says which half of the spec is a view question and which half is not.
	//
	// MEASURED, NOT PREDICTED. The obvious version of this case computes 1/rate from phi and asserts
	// the field repeats a period along — and it fails, because the engine's rates are FLOATS
	// (`1.0f / kPhi175`, times `kPhi100`) and a double's idea of the period lands a hair off theirs.
	// A triangle's slope is about four per unit of phase, so a hair is visible. So the period is found
	// the way you would find it on an oscilloscope: walk the function and measure peak to peak.
	const double band = homeCeiling(0.0) - homeFloor(0.0);
	// STRICT ON BOTH SIDES, AND ONLY REAL PEAKS — which this got wrong first and is worth keeping. A
	// phi triangle has a DEAD ZONE: outside its duty it returns exactly zero. The bipolar ones enter
	// that zone from below, rising −1 → 0, so `here >= next` reads the first flat sample as a maximum
	// and the "period" measured was the 0.675 of one between the true peak and the dead zone —
	// 19.6 home instead of 29.0, a plausible wrong number rather than an obvious one. Requiring a
	// strict maximum ABOVE half scale keeps only the tips of the triangles.
	const auto periodOf = [](const std::function<double(double)>& field, double upTo) {
		constexpr double kStep = 0.0002;
		std::vector<double> peaks;
		double previous = field(-kStep);
		double here = field(0.0);
		for (double at = kStep; at < upTo && peaks.size() < 3; at += kStep) {
			const double next = field(at);
			if (here > previous && here > next && here > 0.5) {
				peaks.push_back(at - kStep);
			}
			previous = here;
			here = next;
		}
		REQUIRE(peaks.size() >= 2);
		return peaks[1] - peaks[0];
	};

	// The blue-shift overshoot field, phi^-0.75: the fastest of the long fields.
	const double overshootPeriod =
	    periodOf([](double at) { return static_cast<double>(blueShiftFieldsAt(at).overshoot); }, 6.0);
	INFO("overshoot field period " << overshootPeriod << " home = " << overshootPeriod * 1023.0
	                               << " zones; the band holds " << band / overshootPeriod);
	// The plan quotes ~1470 zones for it, and the measurement has to agree with the plan or one of
	// them is describing a different engine.
	CHECK(overshootPeriod * 1023.0 == doctest::Approx(1468.0).epsilon(0.01));
	// SEVERAL PERIODS OF THE FASTEST LONG FIELD IS REACHABLE, which is the half that ships: two of
	// them fit inside the widest stop.
	CHECK(band / overshootPeriod > 2.0);

	// (The slowest component this case used to measure — the orientation ladder's last angle, φ⁻⁷,
	// twelve times slower than the fields the spec named — is deleted along with the φ-derived
	// orientation field. Its conclusion stands in the record: several periods of a field that slow was
	// never a view question. The slowest fields still shipping are the two blue-shift ones above.)
}

TEST_CASE("every rung of the zoom ladder is a window of its own") {
	// The carrying-without-consuming shape in reverse: if some clamp collapsed high-zoom windows onto
	// equal keys, `request()` would see no change and skip the rebuild — and the symptom would be
	// exactly the stretched map, with the surface believing it had asked.
	ExploreFixture f;
	std::vector<TerrainWindow> seen;
	visage::Canvas canvas;
	for (double zoom = ExploreStage::minZoom(); zoom <= ExploreStage::kMaxZoom; zoom *= 1.6) {
		f.stage.setZoom(zoom);
		f.stage.draw(canvas);
		TerrainWindow w;
		w.travelYCentre = f.stage.viewCentre();
		w.travelYSpan = f.stage.homeSpanZones() / 1023.0;
		w.travelXSpan = f.stage.travelSpan();
		// Every span is a real span — a zero would make request() drop it on the floor.
		CHECK(w.travelYSpan > 0.0);
		CHECK(w.travelXSpan > 0.0);
		for (const TerrainWindow& earlier : seen) {
			CHECK_FALSE(earlier == w);
		}
		seen.push_back(w);
	}
	CHECK(seen.size() >= 9);
}

TEST_CASE("retracing a crumb restores the sound, not just the orientation") {
	// LOAD-BEARING since the overrule. A rotation preserves the standing point by BANKING the
	// difference it would have made, so home + drift + plane no longer determine the sound on their
	// own. A crumb that restored those three and not the residue would land on the right ORIENTATION
	// with the WRONG SOUND standing on it — worse than landing somewhere obviously else, because it
	// looks like arriving.
	//
	// THE ORDER HERE IS THE WHOLE TEST. A first draft left the crumb BEFORE any rotation, so its
	// residue was zero whether or not the field existed, and a build that dropped the field entirely
	// passed every assertion. The crumb has to be planted AFTER a turn, holding a residue that is not
	// the default and not the live one either.
	ExploreFixture f;
	f.selection.select(0);
	const auto table = [&](ExploreFixture& fix) {
		const auto read = [&](ParamId id) { return fix.bridge.modulated(static_cast<clap_id>(id)); };
		return buildMorphFor(characterFromParams(read, oscTypeFromParams(read), fix.bridge.tuning(),
		                                         fix.bridge.puckVelocity(), fix.bridge.soundTravel(),
		                                         fix.bridge.soundOrientations(),
		                                         fix.bridge.soundResidue(), SoundGamma {}, Preview {}, EarSide::Head));
	};
	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point rotateAt { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	const auto rotate = [&](float pixels) {
		f.stage.onMouseDown().callback(at(rotateAt.x, rotateAt.y));
		f.stage.onMouseDrag().callback(at(rotateAt.x, rotateAt.y - pixels));
		f.stage.onMouseUp().callback(at(rotateAt.x, rotateAt.y - pixels));
		f.settleGesture();
	};

	// Stand out in the plane — rotation only banks anything for a sound that has travelled.
	f.bridge.beginSoundTravelGesture();
	f.bridge.setSoundTravel(0, { 0.40, -0.15 });
	f.bridge.endSoundTravelGesture();
	rotate(45.0f);
	const PlaneResidue bankedWhenLeft = f.bridge.soundResidue().banked[0];
	REQUIRE_FALSE(bankedWhenLeft == PlaneResidue {});

	// Leave the crumb HERE, with that residue standing.
	const visage::Point centre = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(centre.x + 25.0f, centre.y - 25.0f));
	f.stage.onMouseUp().callback(at(centre.x + 25.0f, centre.y - 25.0f));
	f.settleGesture();
	// The rotation left one of its own — every committed gesture does — so the crumb this case is
	// about is the LAST one, not the first.
	REQUIRE(f.stage.crumbs(0).size() >= 2);
	const SoundIdentity crumb = f.stage.crumbs(0).back();
	// THE CRUMB CAPTURED WHAT WAS ACTUALLY BANKED, checked against a reading taken independently of
	// the crumb — a crumb that stored a default would otherwise agree with itself.
	CHECK(crumb.residue == bankedWhenLeft);

	// Turn again and wander off, so both the residue and the identity have moved on.
	rotate(70.0f);
	REQUIRE_FALSE(f.bridge.soundResidue().banked[0] == bankedWhenLeft);
	f.stage.onMouseDown().callback(at(centre.x - 40.0f, centre.y + 30.0f));
	f.stage.onMouseUp().callback(at(centre.x - 40.0f, centre.y + 30.0f));
	f.settleGesture();
	const owlet::dsp::PhiMorphWavetable wandered = table(f);

	// RETRACE, and require the SOUND rather than the coordinates.
	while (f.stage.canUndo() && !(f.stage.identity() == crumb)) {
		f.stage.undo();
		f.settleGesture();
	}
	REQUIRE(f.stage.identity() == crumb);
	CHECK(f.bridge.soundResidue().banked[0] == bankedWhenLeft);
	const owlet::dsp::PhiMorphWavetable retraced = table(f);
	CHECK(std::memcmp(&wandered, &retraced, sizeof(wandered)) != 0);

	// And it is the sound that was standing there: rebuilt from the reading taken at the time, in a
	// fixture that has never rotated anything.
	ExploreFixture reference;
	reference.selection.select(0);
	reference.set(ParamId::AZone, crumb.zone);
	reference.set(ParamId::AOffset, crumb.offset);
	reference.set(ParamId::AGamma, crumb.gamma);
	reference.bridge.beginSoundTravelGesture();
	reference.bridge.setSoundTravel(0, { crumb.driftX, crumb.driftY });
	reference.bridge.endSoundTravelGesture();
	reference.bridge.setSoundOrientation(0, crumb.plane);
	reference.bridge.setSoundResidue(0, bankedWhenLeft);
	const owlet::dsp::PhiMorphWavetable fromCrumb = table(reference);
	CHECK(std::memcmp(&fromCrumb, &retraced, sizeof(fromCrumb)) == 0);
}

TEST_CASE("the terrain is one quad, whatever the map and the view disagree about") {
	// WHAT THIS CASE USED TO BE, kept because the history is the reason the claim is worth asserting:
	// the map was drawn as a quad PER TEXEL, 12,892 of them at any zoom, and the recipe below — zoom
	// in so the builder makes a narrow map, then jump the view back out before it catches up — packed
	// all 16,384 inside the disk. That was over visage's uint16 index ceiling, and far over the
	// transient arena's ~14,041 quads a frame, which is what actually killed batches: past it the
	// whole batch silently vanishes.
	//
	// The map is a picture and is drawn as one now, so the recipe cannot produce a count at all. What
	// is left to assert is that it stays one under exactly the conditions that used to break it — a
	// map from a window the view has left, at every zoom, mid-build and finished.
	ExploreFixture f;
	f.selection.select(0);
	visage::Canvas canvas;

	// **A WALL-CLOCK DEADLINE, NOT A COUNT OF SLEEPS** (2026-09-21). Both settle loops here counted
	// 8000 iterations of `sleep_for(1ms)` and called that eight seconds. It is not: a 1 ms sleep is a
	// FLOOR, and on a loaded host it lands at five or ten, so the "8 s" bound became a minute or more
	// — seven times over, because the loop below runs once per zoom. MEASURED: this case was SIGTERMed
	// by the runner's 120 s watchdog at load average 30 while passing in seconds on an idle host.
	//
	// The bound asserts nothing either way — it exists only to turn "hang forever" into "fail" — so
	// what matters is that it means the same thing in every venue. A deadline does; an iteration count
	// silently scales with whatever else is running. Same lesson the neighbourhood case wrote down
	// after being spent under ASan: a number measured in one venue and spent in another is not a
	// measurement.
	using Clock = std::chrono::steady_clock;
	// **FIFTEEN SECONDS EACH, AND THE NUMBER IS SIZED AGAINST THE RUNNER'S WATCHDOG.** There are seven
	// settle loops here (one per zoom, plus the first), so the worst case is 105 s and `run-cases.sh`
	// gives a case 120 s — a genuine hang is still caught, with room to spare, while a merely SLOW
	// venue is not converted into a false red.
	//
	// THE DEADLINE MUST NOT BE TIGHT, which is the trap in replacing a count with a clock: the old
	// count gave MORE time under load (its "1 ms" sleeps stretched), so a mechanically-equal deadline
	// would have turned passes into failures on exactly the busy hosts the change is for. MEASURED on
	// this box: the whole case takes 57 s at load 12 and 84 s at load 25, which is ~8-12 s per loop of
	// real terrain building — so 8 s would have been INSIDE the working range and 15 is outside it.
	constexpr auto kSettleBound = std::chrono::seconds(15);
	const auto settleUntil = [&](const auto& done) {
		const auto giveUp = Clock::now() + kSettleBound;
		while (Clock::now() < giveUp && !done()) {
			f.stage.draw(canvas);
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	};

	f.stage.setZoom(ExploreStage::minZoom());
	f.stage.draw(canvas);
	settleUntil([&] { return f.terrain.snapshot()->rows >= TerrainMap::kResolution; });
	REQUIRE(f.terrain.snapshot()->rows == TerrainMap::kResolution);
	CHECK(f.stage.terrainQuadCount() == 1);

	// **THREE RUNGS, NOT SIX, AND THE REASON IS THE CLAIM RATHER THAN THE CLOCK.** The recipe is: zoom
	// in so the builder makes a NARROW map, then jump the view back out before it catches up. The
	// failure it guards — the old per-texel quad count crossing visage's uint16 index ceiling and the
	// transient arena's ~14,041 quads a frame — is MONOTONE IN NARROWNESS: the more the stale map is
	// magnified, the more quads the old drawing needed. So the extreme rung is the one that matters,
	// and the middle ones are the same question asked more quietly.
	//
	// THE LADDER ITSELF IS COVERED ELSEWHERE AND CHEAPLY, by "every rung of the zoom ladder is a window
	// of its own" — so dropping three rungs here loses no statement about the ladder, only three
	// repetitions of a claim whose worst case is still tested.
	//
	// AND THE COST WAS REAL: seven full terrain settles put this case at 2m00s against `run-cases.sh`'s
	// 120 s watchdog at load average 33, so it went red in every busy `--each` run while passing alone.
	// A case that reds under load teaches the next reader to discount reds, which is worse than a
	// slightly narrower case honestly described.
	for (double zoom : { 2.0, 8.0, 64.0 }) {
		f.stage.setZoom(zoom);
		f.stage.draw(canvas);
		settleUntil([&] {
			const std::shared_ptr<const TerrainSnapshot> now = f.terrain.snapshot();
			return now->rows >= TerrainMap::kResolution
			       && now->window.travelYSpan
			              == doctest::Approx(ExploreStage::builtSpan(f.stage.homeSpanZones() / 1023.0));
		});
		// The view snaps back out while that narrow map is still the one in hand — the old recipe.
		f.stage.setZoom(ExploreStage::minZoom());
		INFO("stale map from zoom x" << zoom << " drawn at x1");
		CHECK(f.stage.terrainQuadCount() == 1);
		f.stage.draw(canvas);
	}
}

namespace {

/// What the overlay is allowed to mark: the chart, plus the bleed a glyph drawn AT a point has.
void checkOverlayStaysOnTheChart(const ExploreStage& stage) {
	const ExploreStage::InkBox ink = stage.overlayInk();
	const ExploreStage::InkBox chart = stage.chartBox();
	INFO("overlay ink " << ink.left << "," << ink.top << " .. " << ink.right << "," << ink.bottom
	                    << "   chart " << chart.left << "," << chart.top << " .. " << chart.right
	                    << "," << chart.bottom);
	CHECK(ink.within(chart, ExploreStage::kGlyphBleed));
}

} // namespace

TEST_CASE("a crumb outside the view does not draw a line across the whole window") {
	// THE WHITE DOTTED DIAGONAL, running from near the window's top-left across the header, the
	// Engine rail and the sound cards, and surviving a Voice-Mode toggle.
	//
	// The trail is the only thing this surface draws as a line between two computed points, and its
	// endpoints are `yAtTravelY` of each crumb's own home. At the zoom (x10) the view spans 6.4 zones,
	// so a crumb left at any other home projects THOUSANDS of pixels away — and a segment drawn from
	// there to the star crosses the entire window on its way in. Measured below rather than argued.
	ExploreFixture f;
	f.selection.select(1);
	const visage::Point centre = f.stage.disk().centre();

	// Scrub away from where the sound was: the crumb marks the DEPARTURE, so this puts real distance
	// between the trail and the star. Then zoom in hard — at x64 the view spans one zone, and a
	// separation that was sixty pixels becomes thousands.
	f.stage.onMouseDown().callback(at(centre.x, centre.y + 60.0f));
	f.stage.onMouseUp().callback(at(centre.x, centre.y + 60.0f));
	f.settleGesture();
	// backlog GE: the close re-centres by PANNING now, so let the view land before reading it.
	f.settleScrub();
	REQUIRE(f.stage.crumbs(1).size() == 1);
	f.stage.setZoom(64.0);

	// THE PROJECTION REALLY IS ENORMOUS — this is the fact the drawing has to cope with, and it is why
	// a segment to it sweeps the window.
	const float y = f.stage.crumbAt(0).y;
	INFO("crumb projects to y=" << y << " in a stage 534 tall");
	CHECK(std::abs(y) > 2000.0f);

	// Its own mark is not drawn: a crumb off the chart has nothing to say about where you are.
	CHECK_FALSE(f.stage.crumbIsVisible(0));
	CHECK(f.stage.visibleCrumbCount() == 0);

	// AND THE LEG TO IT LEAVES NO INK OFF THE CHART. This is the half a cull cannot do, because the
	// leg has to keep being drawn where it crosses what you are looking at.
	visage::Canvas canvas;
	f.stage.draw(canvas);
	checkOverlayStaysOnTheChart(f.stage);

	// Back out to where it belongs and the crumb returns — this is about the view, not the crumb.
	f.stage.setZoom(1.0);
	CHECK(f.stage.crumbIsVisible(0));
	CHECK(f.stage.visibleCrumbCount() == 1);
	f.stage.draw(canvas);
	checkOverlayStaysOnTheChart(f.stage);
}

TEST_CASE("a sound that is off the chart is marked at the rim, not out over the sound cards") {
	// THE x40 PHOTOGRAPH: the selected star and its label drawn at the far left of the stage,
	// outside the disk entirely, with a trail leg running from it across the whole window.
	//
	// The star sits at the sound's own DRIFT, which is identity — the view's horizontal centre is the
	// identity rail and stays there — so a drifted sound leaves the chart as soon as the zoom is tight
	// enough, and this surface used to draw it wherever the arithmetic put it. The one-value design narrowed
	// which sounds can be off the chart (a scrubbed one, not an automated one); it did not remove the
	// case, and this is the photograph it came from.
	ExploreFixture f;
	f.selection.select(1);
	visage::Canvas canvas;

	// WHICH SOUNDS CAN STILL BE OFF THE CHART, now that the view has a horizontal centre. A finished
	// gesture re-centres on both axes, so a hand can no longer strand its own star — that was the bug
	// that showed as hitting the edge of the plane. What remains, and what this case now stands on, is an
	// identity moved by a road with no gesture to end: a direct write, a host automation lane. The view
	// stays where it was settled, the sound goes somewhere else, and the mark has to find the rim.
	f.stage.setZoom(40.0);
	f.stage.draw(canvas); // settle the view where the sound is now
	// A drift well outside that view: at x40 the chart spans 64/1023/40 of travel, so a hundredth is
	// many chart-widths away.
	f.stage.driftTo(0.01);
	f.settleGesture();
	REQUIRE(f.stage.selectedDrift() == doctest::Approx(0.01));

	const ExploreStage::InkBox chart = f.stage.chartBox();
	const float starX = f.stage.xAtTravelX(f.stage.selectedDrift());
	INFO("star projects to x=" << starX << ", chart " << chart.left << ".." << chart.right);
	REQUIRE(starX > chart.right);

	f.stage.draw(canvas);
	// SOMETHING IS DRAWN — the sound this surface is editing does not silently disappear...
	CHECK(f.stage.overlayInk().any);
	// ...and all of it is on the chart.
	checkOverlayStaysOnTheChart(f.stage);
}

TEST_CASE("the dash walker refuses coordinates it cannot walk") {
	// THE FAN sent somebody looking at this function, and what the looking found was an asymmetry
	// rather than the fan: of the two straight-line primitives on these surfaces, `clipSegmentToDisk`
	// has always checked its endpoints and `dashedLine` never did.
	//
	// THE THREE FAILURES MISBEHAVE DIFFERENTLY, and the difference is the point — measured with the
	// loop in hand rather than assumed:
	visage::Canvas canvas;
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();

	// A real line draws real dashes, or the refusals below prove nothing.
	CHECK(dashedLine(canvas, { 10.0f, 10.0f }, { 110.0f, 10.0f }, 2.0f, 5.0f, 1.0f) == 15);

	// (1) A NaN endpoint ALREADY drew nothing, before this guard existed: every comparison against a
	//     NaN is false, so the loop ends before its first dash. Recorded because it is the half that
	//     says a NaN cannot be the fan — the guard makes the refusal explicit, it does not change what
	//     reaches the screen.
	CHECK(dashedLine(canvas, { nan, nan }, { 110.0f, 10.0f }, 2.0f, 5.0f, 1.0f) == 0);
	CHECK(dashedLine(canvas, { 10.0f, 10.0f }, { 110.0f, nan }, 2.0f, 5.0f, 1.0f) == 0);

	// (2) An INFINITE endpoint is the one this guard is really for, and it is not a misdrawn line but
	//     a HANG: the length is infinite so the loop never reaches its end, and the unit vector is
	//     inf/inf, so every one of those endless iterations hands the renderer a NaN — on the one
	//     thread the editor has. The known-bad for this is not run in the suite for the obvious
	//     reason; it was measured in a standalone probe of the same loop, which passed a hundred
	//     thousand iterations and was still going.
	CHECK(dashedLine(canvas, { 10.0f, 10.0f }, { inf, 10.0f }, 2.0f, 5.0f, 1.0f) == 0);
	CHECK(dashedLine(canvas, { -inf, 10.0f }, { 110.0f, 10.0f }, 2.0f, 5.0f, 1.0f) == 0);

	// (3) And the shape a FAN would actually need: finite coordinates in the wrong place. A zeroed
	//     `from` draws a perfectly well-formed dashed line from the origin — fifty-two dashes of it —
	//     and no finite check can refuse that, which is why this case documents it rather than
	//     guarding it. If a fan anchored at the origin is photographed again, its source is a caller
	//     handing over a zeroed point, and the search belongs there.
	CHECK(dashedLine(canvas, { 0.0f, 0.0f }, { 300.0f, 200.0f }, 2.0f, 5.0f, 1.0f) == 52);

	// The older refusals still refuse.
	CHECK(dashedLine(canvas, { 10.0f, 10.0f }, { 110.0f, 10.0f }, 0.0f, 5.0f, 1.0f) == 0);
	CHECK(dashedLine(canvas, { 10.0f, 10.0f }, { 10.0f, 10.0f }, 2.0f, 5.0f, 1.0f) == 0);
}

TEST_CASE("a line between two places off the chart still draws where it crosses the chart") {
	// The first attempt at this broke the trail wherever a crumb was culled, which loses the one thing
	// the clip is for: two distant places can be distant in opposite directions, and the line between
	// them passes straight through what you are looking at.
	const StageDisk disk { 866.0f, 534.0f };
	const visage::Point centre = disk.centre();

	const std::optional<std::pair<visage::Point, visage::Point>> across =
	    clipSegmentToDisk(disk, { centre.x - 40000.0f, centre.y }, { centre.x + 40000.0f, centre.y });
	REQUIRE(across.has_value());
	CHECK(across->first.x == doctest::Approx(centre.x - disk.radius()).epsilon(0.01));
	CHECK(across->second.x == doctest::Approx(centre.x + disk.radius()).epsilon(0.01));

	// A line that passes the disk without entering it draws nothing at all.
	CHECK_FALSE(clipSegmentToDisk(disk, { centre.x - 40000.0f, centre.y - disk.radius() - 2.0f },
	                              { centre.x + 40000.0f, centre.y - disk.radius() - 2.0f })
	                .has_value());
	// Neither does a line whose coordinates are not numbers, which is the one input the renderer has
	// no defined answer for.
	const float notANumber = std::numeric_limits<float>::quiet_NaN();
	CHECK_FALSE(clipSegmentToDisk(disk, centre, { notANumber, notANumber }).has_value());
}

TEST_CASE("automation at Weather churns the engine and moves nothing on the chart") {
	// The symptom: with Weather enabled and position automated, the Explore view was chaos —
	// breadcrumbs leaving the circle, line and polygon glitches.
	//
	// WHAT THIS CASE USED TO ASSERT, and why it changed. With the shipped placement the star sat at
	// drift + Weather x (puck - anchor), so the host streamed it and every trail leg across the view
	// and out the other side at whatever rate its LFO ran — measured here at 227 of 240 frames off the
	// chart at x1 and 240 of 240 at x10 and x40. This case pinned the CLIP that kept that mess inside
	// the disk, and the clip is still pinned below because a scrubbed sound can still be off the chart.
	//
	// the decision removed the movement itself: Weather does not move anything on the chart. So the
	// headline is now the opposite claim — the automation churns as hard as ever, and NOTHING the
	// chart draws moves — which is checked frame by frame, because a surface that holds still for 239
	// frames and jumps on the 240th is the bug this fixture exists to catch.
	ExploreFixture f;
	f.selection.select(1);
	visage::Canvas canvas;

	// Weather up, which is what turns a performance control into a term of the star's position.
	f.set(ParamId::Weather, 0.6);

	// A trail to fling around: three departures at three different places, so the legs are real legs
	// between different identities rather than one point drawn twice.
	const visage::Point centre = f.stage.disk().centre();
	for (int gesture = 0; gesture < 3; ++gesture) {
		const float y = centre.y - 40.0f + 30.0f * static_cast<float>(gesture);
		f.stage.onMouseDown().callback(at(centre.x + 20.0f * static_cast<float>(gesture), y));
		f.stage.onMouseUp().callback(at(centre.x + 20.0f * static_cast<float>(gesture), y));
		f.settleGesture();
	}
	REQUIRE(f.stage.crumbs(1).size() == 3);

	for (double zoom : { 1.0, 10.0, 40.0 }) {
		f.stage.setZoom(zoom);
		int offView = 0;
		int worst = 0;
		int shifted = 0;
		ExploreStage::InkBox firstInk;
		double travelLow = 0.0;
		double travelHigh = 0.0;
		for (int frame = 0; frame < 240; ++frame) {
			// An LFO on all three, at incommensurate rates so the puck sweeps the whole square rather
			// than retracing one diagonal — the same reason the character layer uses phi ratios.
			const double t = static_cast<double>(frame) / 240.0;
			f.set(ParamId::Path, 0.5 + 0.5 * std::sin(t * 2.0 * 3.14159265358979 * 3.0));
			f.set(ParamId::PosX, std::sin(t * 2.0 * 3.14159265358979 * 5.0));
			f.set(ParamId::PosY, std::cos(t * 2.0 * 3.14159265358979 * 8.0));

			f.stage.draw(canvas);

			const float starX = f.stage.xAtTravelX(f.stage.selectedDrift());
			const float starY = f.stage.yAtTravelY(f.stage.selectedDriftY());
			// WHERE THE ENGINE HAS THE SOUND, which is what has to be seen moving: if this stood still
			// the whole case would be a quiet frame repeated 240 times and would pass against anything.
			const double engineTravel = f.stage.selectedTravel();
			if (frame == 0) {
				travelLow = engineTravel;
				travelHigh = engineTravel;
			}
			travelLow = std::min(travelLow, engineTravel);
			travelHigh = std::max(travelHigh, engineTravel);
			if (!f.stage.disk().contains({ starX, starY })) {
				++offView;
			}

			const ExploreStage::InkBox ink = f.stage.overlayInk();
			// WHAT WAS ACTUALLY DRAWN, not where the arithmetic says the star is. Asking the stage for
			// a coordinate would be asking the same expression twice: the ink is the mark the canvas
			// received, and it is the only thing here that can tell a stationary chart from a stationary
			// formula. Exact equality, not a tolerance — nothing in this loop touches a number the star,
			// the trail or the label are placed from, so a single differing bit is the performance half
			// leaking back in.
			if (frame == 0) {
				firstInk = ink;
			}
			else if (ink.any != firstInk.any || ink.left != firstInk.left || ink.top != firstInk.top
			         || ink.right != firstInk.right || ink.bottom != firstInk.bottom) {
				++shifted;
			}
			if (!ink.within(f.stage.chartBox(), ExploreStage::kGlyphBleed)) {
				INFO("frame " << frame << " at x" << zoom << ": ink " << ink.left << "," << ink.top
				              << " .. " << ink.right << "," << ink.bottom << " star at " << starX << ","
				              << starY);
				++worst;
			}
		}
		INFO("at x" << zoom << ": " << shifted << " of 240 frames moved the ink, " << offView
		            << " had the star off the chart, " << worst << " left ink off it; the engine's "
		            << "travel swept " << (travelHigh - travelLow));
		// The automation is real and the surface is watching it...
		CHECK(travelHigh - travelLow > 0.0);
		// ...and every mark the chart made held perfectly still through all 240 frames of it.
		CHECK(shifted == 0);
		// The clip still holds, which is the assertion this case was built for.
		CHECK(worst == 0);
	}
	// AND THE OFF-CHART CASE COLLAPSES AT THE USUAL WORKING ZOOM. It was 227 of 240 frames
	// at x1 — A27's rim mark was what this surface mostly showed. The star is now where the last
	// gesture left it, which at x1 is on the chart, every frame. (At a tight zoom that same drift is
	// still many chart-widths away, which is why the rim mark stays.)
	f.stage.setZoom(1.0);
	f.stage.draw(canvas);
	CHECK(f.stage.disk().contains({ f.stage.xAtTravelX(f.stage.selectedDrift()),
	                                f.stage.yAtTravelY(f.stage.selectedDriftY()) }));
}

TEST_CASE("what a rotation banked reaches the terrain, so the chart stops drawing the neighbour") {
	// §4a: THE MAP IS TRUTH. The engine builds every character through sliceFrameFor(rotate, home,
	// residue) — the plane says which way travel leads, the residue says where it leads FROM — and the
	// preview took the plane alone. After A26, a sound rotated at a drift stands on its residue, so the
	// chart was drawing a NEIGHBOUR of the sound being heard, underneath a star claiming to be it.
	//
	// THE SAME TWO FAULTS AS THE PIN, and so the same two assertions: consuming it without keying on
	// it draws the right map once and never again; keying on it without consuming it rebuilds
	// diligently and draws the same map every time.
	const SliceGenerator before;
	SliceGenerator after = before;
	after.rotate += 0.6f;
	PlaneResidue banked;
	// The fold takes the place the turn happened at, because M is a function of it — the same home the
	// window below is keyed on, so the residue under test is the one that sound really banked.
	const double turnedAt = sliceHome(512, 0.0f, 0.0);
	banked.morph = foldResidue<kMorphTriangles>(SliceResidue<kMorphTriangles> {}, sliceRotateOf(before),
	                                            sliceRotateOf(after), 0.02, 0.0, turnedAt);
	banked.stair = foldResidue<kStairTriangles>(SliceResidue<kStairTriangles> {}, sliceRotateOf(before),
	                                            sliceRotateOf(after), 0.02, 0.0, turnedAt);
	REQUIRE_FALSE(banked.morph == SliceResidue<kMorphTriangles> {});

	// (a) THE RESIDUE IS LOAD-BEARING in the evaluation: same place, same plane, a sound that has been
	//     rotated at a drift is a different sound from one that has not. AT ZERO TRAVEL TOO, unlike the
	//     plane — that is the whole point of a residue, which is an offset of the origin rather than a
	//     direction of departure.
	const SliceOrientationPin pin;
	const TerrainFeatures fresh = terrainFeaturesAt(OscType::Morph, pin, 512, 0.0f, 0.0, 0.0);
	const TerrainFeatures folded = terrainFeaturesAt(OscType::Morph, pin, 512, 0.0f, 0.0, 0.0, banked);
	const float moved = std::abs(fresh.energy - folded.energy)
	                    + std::abs(fresh.centroid - folded.centroid)
	                    + std::abs(fresh.roughness - folded.roughness);
	INFO("features moved by " << moved);
	CHECK(moved > 0.0001f);

	// And an EMPTY residue is the map exactly as it was — the migration guarantee, in the picture as
	// well as in the sound: a patch that has never rotated a drifted sound draws what it always drew.
	const TerrainFeatures empty = terrainFeaturesAt(OscType::Morph, pin, 512, 0.0f, 0.03, 0.0, PlaneResidue {});
	const TerrainFeatures none = terrainFeaturesAt(OscType::Morph, pin, 512, 0.0f, 0.03, 0.0);
	CHECK(empty.energy == none.energy);
	CHECK(empty.centroid == none.centroid);
	CHECK(empty.roughness == none.roughness);

	// (b) AND THE MAP IS ASKED TO REBUILD, because the residue is part of the window's identity.
	TerrainMap map;
	TerrainWindow plain = windowAt(0.5, 64.0 / 1023.0, 64.0 / 1023.0);
	map.request(plain);
	REQUIRE(settle(map, plain));
	const uint32_t wasDrawn = map.snapshot()->texel(64, 64);
	const uint64_t askedOnce = map.requests();

	TerrainWindow rotated = plain;
	rotated.residue = banked;
	CHECK_FALSE(plain == rotated);
	map.request(rotated);
	CHECK(map.requests() > askedOnce);
	REQUIRE(settle(map, rotated));
	// The picture itself is different, which is the half a key alone cannot promise.
	CHECK(map.snapshot()->texel(64, 64) != wasDrawn);
}

TEST_CASE("gamma's coarse half reaches the chart, the band and the map it asks for") {
	// THE VERNIER, from the chart's side. Home is vernier plus global plus BASE, so the base moves the
	// band a scrub can reach, the floor and ceiling it clamps to, the star's own home, and the
	// coordinates every texel is built at. One case for all of it, because they are one number —
	// `gammaSum` — and the failure of getting it wrong is a chart drawing a neighbourhood the sound
	// has left.
	ExploreFixture f;
	f.selection.select(1);
	const double homeBefore = f.stage.selectedHome();
	const double floorBefore = homeFloor(f.stage.identity().gamma);

	f.stage.nudgeSelectedGamma(12.5);
	f.settleGesture();
	REQUIRE(f.stage.selectedGammaBase() == doctest::Approx(12.5));

	// The sound is 12.5 further along its own line...
	CHECK(f.stage.selectedHome() - homeBefore == doctest::Approx(12.5));
	// ...and the reachable band went with it, which is what stops a scrub clamping to a floor the
	// sound left behind. Taken from homeFloor rather than restated, so the two cannot drift apart.
	CHECK(homeFloor(f.stage.identity().gamma + f.stage.selectedGammaBase()) - floorBefore
	      == doctest::Approx(12.5));
	CHECK(f.stage.selectedHome() >= homeFloor(f.stage.identity().gamma + f.stage.selectedGammaBase()));

	// AND THE MAP IS ASKED FOR IN THOSE COORDINATES. The window used to carry the base as a field of its
	// own, so this waited for that field; the builder now derives each row's base from the row's own
	// home and the sound's dials, so there is no such field to wait for and the claim is made where it
	// is still visible — the view went to the sound, and the frame that lines an old map up against a
	// new view went with it. Both by exactly the turn.
	//
	// THIS IS THE CORNER'S ROAD, which is the one viewGamma follows: gamma moved the world under a still
	// hand. A scrub is the other road and is deliberately absent from that number — see viewGamma().
	visage::Canvas canvas;
	f.stage.draw(canvas);
	bool arrived = false;
	for (int waited = 0; waited < 5000 && !arrived; ++waited) {
		const TerrainWindow window = f.terrain.snapshot()->window;
		// **THE CONTRACT INVERTED BY backlog GY.** This used to require the window's gamma frame to
		// track the turn AND the vertical centre to have followed to the sound's home — the view
		// chasing gamma. Under the one-value design the corner moves the PLANE and moves nothing within it, so
		// what must arrive is the base in the content key, and the vertical centre must NOT have gone
		// anywhere. The second half is the stronger claim and it is the one that was wrong before.
		arrived = window.gammaBase == doctest::Approx(f.stage.identity().gamma + 12.5);
		if (!arrived) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	CHECK(arrived);
}

TEST_CASE("a texel is built with the base beside its offset, not inside it") {
	// THE SPLIT, measured where it matters, and at BOTH levels — which the first draft of this case
	// did not do. It exercised `terrainFeaturesAt` directly and passed against a builder that folded
	// the base into the float, because the fold is at the CALL SITE and calling the function myself
	// walked around it. A test near the thing is not a test of the thing.
	//
	// The number that makes it sharp: a float's step at a base of 1e5 is about 0.0078, so a base
	// nudged by 0.001 is a DIFFERENT sound that a fold cannot tell apart from the old one.
	// A float's own step at 1e7 is a whole unit of home — 1023 zones — so a fine detail folded into it
	// is not merely blurred, it is gone. 1e5 was the first choice and it made a weak case: 1e5 is
	// exactly representable in float, so the two roads agreed exactly wherever the offset was zero.
	const SliceOrientationPin unpinned;
	const double large = 1.0e7;
	const double fine = 0.001;

	// (a) The function honours the base as a double.
	const TerrainFeatures here =
	    terrainFeaturesAt(OscType::Morph, unpinned, 512, 0.25f, 0.0, 0.0, kNoPlaneResidue, large);
	const TerrainFeatures nudged =
	    terrainFeaturesAt(OscType::Morph, unpinned, 512, 0.25f, 0.0, 0.0, kNoPlaneResidue, large + fine);
	const auto apart = [](const TerrainFeatures& a, const TerrainFeatures& b) {
		return std::abs(a.energy - b.energy) + std::abs(a.centroid - b.centroid)
		       + std::abs(a.roughness - b.roughness);
	};
	INFO("features moved by " << apart(here, nudged) << " for a " << fine << " step at " << large);
	CHECK(apart(here, nudged) > 1.0e-5f);
	// A zero base is EXACTLY the old call — the migration guarantee, in the picture.
	const TerrainFeatures grounded =
	    terrainFeaturesAt(OscType::Morph, unpinned, 512, 0.25f, 0.0, 0.0, kNoPlaneResidue, 0.0);
	const TerrainFeatures old =
	    terrainFeaturesAt(OscType::Morph, unpinned, 512, 0.25f, 0.0, 0.0, kNoPlaneResidue);
	CHECK(old.energy == grounded.energy);
	CHECK(old.centroid == grounded.centroid);
	CHECK(old.roughness == grounded.roughness);
	CHECK(apart(grounded, here) > 1.0e-5f);

	// (b) AND THE BUILDER'S OWN SPLIT KEEPS IT. This half took three drafts to state without a hole and
	// has now been restated a fourth time, because the one-value design took the choice of split
	// away from the caller: a window no longer says how much of home is base and how much is offset.
	// The builder decides, always the same way — the sound's two bounded dials in the float, the whole
	// of the rest in the double base — so the old fixture, TWO WINDOWS AT THE SAME GAMMA SPLIT
	// DIFFERENTLY, can no longer be written. Its two dead ends are kept because they are about
	// isolation and will outlive this arrangement too: stepping the base ALONE used to move every
	// texel's decomposition, so the map changed for a reason that was not precision; stepping the whole
	// frame together still passed against a fold, because `terrainColour` reads `home` directly for the
	// family hue and a moved home recolours the map whatever the character did.
	//
	// WHAT ISOLATES IT NOW, with no second window at all: at a home of 1e7 a float's own step is about
	// 1.2 units of HOME — some twelve hundred zones — so a builder that folded the base into the float
	// would hand EVERY ROW of a 64-zone view the identical number and draw a chart with no vertical
	// variation whatsoever. Carried as a double, half a zone between neighbouring rows survives. The
	// assertion is therefore simply that the far-away chart still has a picture on it.
	TerrainMap map;
	TerrainWindow far = windowAt(large + 1.0, 64.0 / 1023.0, 64.0 / 1023.0);
	far.zoneBaseline = 512.0;
	far.offsetBaseline = 0.25;
	map.request(far);
	REQUIRE(settle(map, far));
	const std::shared_ptr<const TerrainSnapshot> built = map.snapshot();
	REQUIRE(built->rows == TerrainMap::kResolution);

	std::set<uint32_t> downTheColumn;
	for (int row = 0; row < TerrainMap::kResolution; ++row) {
		downTheColumn.insert(built->texel(64, row));
	}
	INFO("distinct texels down the middle of a chart at home " << large << ": "
	                                                           << downTheColumn.size());
	// Ends of the view are 64 zones apart — a third of a cycle of the fastest character bank — so they
	// cannot legitimately be the same colour.
	CHECK(built->texel(64, 0) != built->texel(64, TerrainMap::kResolution - 1));
	// And it is a landscape rather than two flat halves.
	CHECK(downTheColumn.size() > 4);
}

TEST_CASE("the surface puts the sound's banked rotations into the terrain it asks for") {
	// The surface half, mirroring the rotate corner's own case: (a) and (b) above are true of a code
	// path nobody reaches unless the window this stage builds carries the residue.
	//
	// DRIFT FIRST, and that is not incidental: a fold at zero drift banks exactly +/-0.0 by
	// construction (A26), so a rotation with the sound standing at home would leave the residue empty
	// and this case would pass against a stage that never read one.
	ExploreFixture f;
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	REQUIRE(f.terrain.snapshot()->window.residue == PlaneResidue {});

	f.stage.driftTo(0.02);
	f.settleGesture();
	REQUIRE(f.stage.selectedDrift() == doctest::Approx(0.02));

	const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	f.stage.onMouseDown().callback(at(centre.x, centre.y));
	f.stage.onMouseDrag().callback(at(centre.x, centre.y - 60.0f));
	f.stage.onMouseUp().callback(at(centre.x, centre.y - 60.0f));
	f.settleGesture();
	const PlaneResidue expected = f.bridge.soundResidue().banked[0];
	REQUIRE_FALSE(expected == PlaneResidue {});

	f.stage.draw(canvas);
	// WAITED FOR, not read immediately: a reader gets the last COMPLETED snapshot while the next one
	// builds, so checking straight after the request reads the old window and calls a working build
	// broken.
	// The request has already been made by the draw above, so this only waits — redrawing in here
	// would make a failing run pay for a frame per millisecond to learn what it already knows.
	bool arrived = false;
	for (int waited = 0; waited < 5000 && !arrived; ++waited) {
		arrived = f.terrain.snapshot()->window.residue == expected;
		if (!arrived) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	CHECK(arrived);
}

TEST_CASE("PERF: what an Explore frame costs, at rest and under a held rotate" * doctest::skip()) {
	// Redraws felt slow. Numbers rather than impressions, and skipped by default —
	// a timing case in the suite is a red that gets learned as noise. Run it by name:
	//
	//   build/tests/test_gui_widgets -ns -tc="PERF*"
	//
	// WHAT THIS MEASURES AND WHAT IT DOES NOT. The editor thread's own cost of a frame: working out
	// the shapes and handing them to a canvas, plus whatever the terrain worker makes this thread wait
	// for. Not the GPU, and there is no window here — so a number from this is a LOWER BOUND on a
	// frame. That is enough to convict or clear the suspect that matters, because the bulk of the work
	// is quad submission and it happens on this side of the line.
	ExploreFixture f;
	f.selection.select(0);
	visage::Canvas canvas;

	const auto report = [&](const std::string& what, std::vector<double> spent, uint64_t requests,
	                        uint64_t builds) {
		std::sort(spent.begin(), spent.end());
		const std::string line =
		    what + ": median " + std::to_string(spent[spent.size() / 2]) + " ms, p90 "
		    + std::to_string(spent[spent.size() * 9 / 10]) + " ms, worst "
		    + std::to_string(spent.back()) + " ms over " + std::to_string(spent.size())
		    + " frames — " + std::to_string(f.stage.terrainQuadCount()) + " terrain quads, "
		    + std::to_string(requests) + " requests, " + std::to_string(builds) + " builds";
		MESSAGE(line);
	};

	// BEFORE THE FIRST MAP ARRIVES, which is every part of a frame EXCEPT the terrain: the grid, the
	// sky, the trail, the star, the readouts, four corner encoders and three buttons. The subtraction
	// against the next measurement is what says where the time goes.
	{
		// Only the frames drawn BEFORE the worker delivers its first row count — the map arrives on its
		// own schedule, so the sample ends when it does rather than after a fixed number of frames.
		std::vector<double> spent;
		while (f.stage.terrainQuadCount() == 0 && spent.size() < 400) {
			const auto start = std::chrono::steady_clock::now();
			f.stage.draw(canvas);
			spent.push_back(
			    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
			        .count());
		}
		REQUIRE(spent.size() >= 3);
		report("  everything but the terrain", spent, 0, 0);
	}

	const auto settleMap = [&] {
		for (int waited = 0; waited < 8000; ++waited) {
			f.stage.draw(canvas);
			const std::shared_ptr<const TerrainSnapshot> now = f.terrain.snapshot();
			if (now->rows >= TerrainMap::kResolution
			    && now->window.travelYSpan == doctest::Approx(ExploreStage::builtSpan(f.stage.homeSpanZones() / 1023.0))) {
				return;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	};

	const auto measure = [&](const std::string& what, int frames,
	                         const std::function<void(int)>& before) {
		std::vector<double> spent;
		spent.reserve(frames);
		const uint64_t requestsBefore = f.terrain.requests();
		const uint64_t buildsBefore = f.terrain.builds();
		for (int frame = 0; frame < frames; ++frame) {
			before(frame);
			const auto start = std::chrono::steady_clock::now();
			f.stage.draw(canvas);
			spent.push_back(
			    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
			        .count());
		}
		report(what, std::move(spent), f.terrain.requests() - requestsBefore,
		       f.terrain.builds() - buildsBefore);
	};

	const int frames = 300;
	for (double zoom : { 1.0, 10.0, 40.0 }) {
		f.stage.setZoom(zoom);
		settleMap();
		const std::string heading = "--- at x" + std::to_string(zoom) + ", view spans "
		                            + std::to_string(f.stage.homeSpanZones()) + " zones ---";
		MESSAGE(heading);
		measure("  at rest", frames, [](int) {});

		// A HELD ROTATE, frame by frame, which is the gesture that felt slow. Each drag moves the encoder,
		// which moves the terrain window, which asks the builder for a new map — so this is the path
		// where the worker thread and the editor thread meet.
		const visage::Bounds box = f.stage.partBounds(ExploreStage::Part::Rotate);
		const visage::Point corner { box.x() + box.width() * 0.5f,
			                         box.y() + ExploreStage::kCornerDiameter * 0.5f };
		f.stage.onMouseDown().callback(at(corner.x, corner.y));
		measure("  held rotate", frames, [&](int frame) {
			f.stage.onMouseDrag().callback(at(corner.x, corner.y - 0.25f * static_cast<float>(frame)));
		});
		f.stage.onMouseUp().callback(at(corner.x, corner.y - 0.25f * static_cast<float>(frames)));
		f.settleGesture();
	}
}

TEST_CASE("PERF: what a coloured quad costs, which is the terrain's unit of work" * doctest::skip()) {
	// THE SUSPECT, ISOLATED. The terrain draws one quad per texel and sets a colour before each one,
	// because each texel IS a colour — and visage's setColor is not a state write: every call packs a
	// gradient into the atlas and heap-allocates a PackedBrush the region owns until the frame ends
	// (canvas.h setBrush -> region.h addBrush -> make_unique). This measures both halves separately, so
	// "the terrain submits 13k quads" can be replaced by a number that says which half to attack.
	visage::Canvas canvas;
	visage::Frame frame;
	frame.setBounds(0, 0, 866, 534);
	const int quads = 12892; // what a settled map submits inside the disk at any zoom

	const auto timed = [](const std::string& what, const std::function<void()>& work) {
		std::vector<double> spent;
		for (int run = 0; run < 30; ++run) {
			const auto start = std::chrono::steady_clock::now();
			work();
			spent.push_back(
			    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
			        .count());
		}
		std::sort(spent.begin(), spent.end());
		const std::string line = what + ": median " + std::to_string(spent[spent.size() / 2]) + " ms";
		MESSAGE(line);
	};

	timed("  " + std::to_string(quads) + " quads, a colour set before each", [&] {
		canvas.beginRegion(frame.region());
		for (int i = 0; i < quads; ++i) {
			canvas.setColor(0x80402060u + static_cast<unsigned>(i));
			canvas.fill(static_cast<float>(i % 800), static_cast<float>(i % 500), 4.0f, 4.0f);
		}
		canvas.endRegion();
	});

	timed("  " + std::to_string(quads) + " quads, ONE colour for all of them", [&] {
		canvas.beginRegion(frame.region());
		canvas.setColor(0x80402060u);
		for (int i = 0; i < quads; ++i) {
			canvas.fill(static_cast<float>(i % 800), static_cast<float>(i % 500), 4.0f, 4.0f);
		}
		canvas.endRegion();
	});
}


TEST_CASE("closing the editor takes a latched preview with it") {
	// NO ORPHANED SOLO. Preview is latched and its only control is a button on a card, so an editor
	// that closed while it was on would leave the plugin playing one sound with nothing on screen that
	// could say why or turn it off — and nothing in a patch to explain it either, since preview is
	// never saved. The clear lives in Gui::destroy so that every road out of the editor takes it.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	bridge.setPreview(5);
	REQUIRE(bridge.preview().active());

	gui.destroy();
	CHECK_FALSE(bridge.preview().active());
	CHECK(bridge.preview().slot == Preview::kNone);
}

TEST_CASE("a press where a tray knob is reaches that knob, at every size the editor can be given") {
	// The symptom: Pos and Weather could no longer be edited — with the
	// transport stopped, so it is not automation overwriting the drag.
	//
	// THE GAP THIS CLOSES. Every control here is tested in isolation and passes; the embed protocol is
	// tested by bitwighost and passes; and NOTHING tested the step between them — that a click at a
	// coordinate reaches the widget that owns it through the tree gui.cpp assembles. A knob can be
	// perfect and unreachable: covered by a panel, outside its parent's bounds, or behind a frame that
	// answers the mouse first. This asks the tree the same question a window asks it.
	//
	// SWEPT ACROSS SIZES, because the editor is resizable and the layout is a function of the size —
	// so "works at 1240x700" is not the claim worth making. The sizes below bracket the window
	// and the extremes a host can ask for.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	const ui::WidgetRegistry* widgets = gui.widgets();
	REQUIRE(widgets != nullptr);
	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);

	struct Size {
		uint32_t width;
		uint32_t height;
	};
	const Size sizes[] = { { Gui::kLogicalWidth, Gui::kLogicalHeight }, { 1400, 820 },
		                   { 1113, 700 },                              { 1600, 900 },
		                   { 1920, 1080 } };

	// The parameters that failed, and one that was not seen to, so a failure says whether it is these two or the
	// whole tray.
	const ParamId watched[] = { ParamId::PosX, ParamId::PosY, ParamId::Weather, ParamId::Path };

	for (const Size& size : sizes) {
		REQUIRE(gui.setSize(size.width, size.height));

		for (const ParamId id : watched) {
			const std::span<visage::Frame* const> watchers =
			    widgets->watchers(static_cast<clap_id>(id));
			INFO("size " << size.width << "x" << size.height << ", param " << static_cast<int>(id));
			REQUIRE_FALSE(watchers.empty());

			// The one that answers the mouse, found by asking the registry rather than by restating
			// where the layout put it — the whole point is that these two must be answered separately.
			visage::Frame* knob = nullptr;
			for (visage::Frame* watcher : watchers) {
				if (watcher->width() > 0.0f && watcher->height() > 0.0f) {
					knob = watcher;
					break;
				}
			}
			REQUIRE(knob != nullptr);

			const visage::Point at = centreInRoot(knob);
			visage::Frame* hit = root->frameAtPoint(at);
			INFO("centre " << at.x << "," << at.y << " in a " << size.width << "x" << size.height
			               << " editor");
			REQUIRE(hit != nullptr);
			// THE CLAIM: what a window would deliver to is the control, or something inside it.
			CHECK(isWithin(hit, knob));
		}
	}
}


TEST_CASE("the smallest window a host can ask for is a whole editor AT ITS SCALE — the floor grows with the "
          "scale and the content is never shrunk to fit (backlog EJ)") {
	// THE PROMISE syncContentScale states in its own comment — whatever size the window is given, at
	// least kLogicalWidth x kLogicalHeight logical pixels fit inside it — asked at the one size where
	// it is hardest to keep: the floor, on a display whose scale is bigger than 1.
	//
	// IT DID NOT HOLD. The scale was resolved only when there was a window to push it onto, so an
	// editor that had been built but not yet embedded — which is where a host asks its size, and
	// where every case in this file lives — was laid out at whatever visage stamped on the frame for
	// itself. On this display that is 1.75, and the floor window measured 708.571x400 logical against
	// a design canvas of 1240x700: the modulator strip yielded, the stage disk fell from 259 to 109,
	// and the Engine rail's Glide and Fine knobs were laid out at y=360..518 in a canvas 400 tall.
	//
	// **THE OTHER HALF WAS REVERSED BY BACKLOG EJ** (the window-size control enlarged the
	// window while the widgets stayed the same size). This case used to hold the floor at the design
	// canvas in DEVICE pixels whatever the scale, and bring the CONTENT down to meet a small window —
	// on the argument that "logical x scale" at 1.75 asks for 2170x1225 and the measured desktop has
	// 1920x1048, so the minimum window would be the whole screen. That is now the accepted consequence:
	// the widgets' size is the user's setting, the floor is the design AT that scale, and a scale the
	// screen cannot hold is the user's to lower (Ctrl -). What the case still guards is the first half —
	// the floor window shows a WHOLE editor, no control outside it — plus the new promise: the scale a
	// host set is the scale the content is drawn at, whatever the window.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);

	// ON A SCREEN THAT HOLDS EVERY ONE OF THESE (backlog LX, 2026-10-10: the floor is held to the usable screen, and the
	// private display's is 1728x972). Where the screen is smaller, the floor is the screen: `test_clap_render`'s
	// "the synth opened in Bitwig's order (no set_scale) ..." is that case.
	struct ScreenStub {
		ScreenStub() { ui::PluginWindow::setUsableScreenForTest(8192, 8192); }
		~ScreenStub() { ui::PluginWindow::setUsableScreenForTest(0, 0); }
	} screen;
	// Through set_scale, which is the source a host speaks with and the one that wins outright, so
	// the case does not depend on what the display under it happens to derive.
	const double scales[] = { 1.0, 1.25, 1.5, 1.75, 2.0, 3.0 };
	for (const double scale : scales) {
		INFO("display scale " << scale);
		REQUIRE(gui.setScale(scale));

		// THE FLOOR IS THE DESIGN AT THIS SCALE, in device pixels, rounded up (backlog EJ) — plus the
		// canvas band under the tray, open by default and room the editor asks for (2026-09-13).
		uint32_t width = 1;
		uint32_t height = 1;
		REQUIRE(gui.adjustSize(&width, &height));
		const double logicalH = Gui::kLogicalHeight + static_cast<double>(gui.canvasForTest()->extent());
		CHECK(width == static_cast<uint32_t>(std::ceil(Gui::kLogicalWidth * scale)));
		CHECK(height == static_cast<uint32_t>(std::ceil(logicalH * scale)));

		REQUIRE(gui.setSize(width, height));
		// THE PROMISE: the content is drawn at the scale the host set, and the floor window holds the
		// whole design in logical pixels — not more than a rounding pixel over, which is what "the widgets
		// stay the same size" means in numbers.
		CHECK(gui.contentScaleForTest() == doctest::Approx(scale));
		CHECK(root->width() >= static_cast<float>(Gui::kLogicalWidth) - 1.0f);
		CHECK(root->width() <= static_cast<float>(Gui::kLogicalWidth) + 2.0f);
		CHECK(root->height() >= static_cast<float>(Gui::kLogicalHeight) - 1.0f);

		// AND WHAT THE PROMISE IS FOR, read off the assembled editor rather than trusted to follow
		// from the arithmetic: no control is anywhere the window cannot draw.
		visage::Frame* outside = firstFrameOutside(root, root);
		if (outside != nullptr) {
			INFO("a " << typeid(*outside).name() << " at " << outside->positionInWindow().x << ","
			          << outside->positionInWindow().y << " sized " << outside->width() << "x"
			          << outside->height() << " in a canvas " << root->width() << "x"
			          << root->height());
		}
		CHECK(outside == nullptr);
	}

	SUBCASE("and a scale stamped on the frame from outside does not survive the next sizing") {
		// THE SEQUENCE A REAL WINDOW ARRIVES IN, which no case here can open but every session
		// performs: visage pushes the WINDOW's own scale down the whole tree the moment the editor is
		// attached (ApplicationEditor::addToWindow), overwriting whatever the editor had settled on
		// while it was windowless. If the editor then decides its scale has not changed and skips the
		// push, visage's derivation is what the layout runs at and the promise above is void — the
		// same 708.571x400 canvas, arrived at from the other direction.
		// AT SCALE 1, THE WINDOW THE FLOOR ASKS FOR: the design plus the open canvas band. (Before EJ a
		// shorter window was answered with a smaller scale; now it would be clipped, so the case gives
		// the editor the room it asked for and tests the stamp alone.)
		REQUIRE(gui.setScale(1.0));
		const uint32_t fullH = Gui::kLogicalHeight + static_cast<uint32_t>(gui.canvasForTest()->extent());
		REQUIRE(gui.setSize(Gui::kLogicalWidth, fullH));
		REQUIRE(root->width() >= static_cast<float>(Gui::kLogicalWidth) - 1.0f);
		root->setDpiScale(1.75f);

		REQUIRE(gui.setSize(Gui::kLogicalWidth, fullH));
		CHECK(gui.contentScaleForTest() == doctest::Approx(1.0));
		CHECK(root->width() >= static_cast<float>(Gui::kLogicalWidth) - 1.0f);
		CHECK(root->height() >= static_cast<float>(Gui::kLogicalHeight) - 1.0f);
		CHECK(firstFrameOutside(root, root) == nullptr);
	}
}


TEST_CASE("create() positions every child before anything can draw") {
	// THE OTHER HALF OF THE FAN SUSPECT, and the half that had a real window in it. A visage frame is
	// zero-initialized and drawable, so a child added but not yet positioned would paint at the window
	// ORIGIN — which is where the fan and the polygon are anchored. The notify tray is the named
	// candidate: created and added LAST, and given bounds only inside the layout pass.
	//
	// The window is closed by ORDERING, and this pins it: `create()` ends with a layout, and it does
	// that before the window exists — `show()` happens later, in `setParent`. So there is no moment
	// between "the tray is a child" and "the tray has bounds" in which anything can draw at all.
	//
	// Two further reasons the tray in particular could not have done it, kept because a suspect should
	// be cleared with its whole alibi: its draw returns immediately while its opacity is zero, and its
	// opacity is zero until something has been said — and nothing says anything before a hand touches
	// a control, by which time the layout above has run several times over. The recurring path is
	// closed the same way: every layout assigns its bounds unconditionally, so no later resize or
	// surface switch can leave it unpositioned.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	// NOTHING BUT create() — no setSize, no setParent, no show. This is the state the editor is in the
	// instant the host has built it, and the claim is that even here nobody is sitting at the origin.
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));

	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);

	int counted = 0;
	int atOrigin = 0;
	const auto walk = [&](visage::Frame* frame, auto&& self) -> void {
		if (frame != root) {
			++counted;
			const visage::Point at = frame->positionInWindow();
			INFO("child " << counted << " at " << at.x << "," << at.y << " sized " << frame->width()
			              << "x" << frame->height());
			// Positioned and real: a child with no size is the shape that paints at the origin.
			CHECK(frame->width() > 0.0f);
			CHECK(frame->height() > 0.0f);
			if (at.x == 0.0f && at.y == 0.0f) {
				++atOrigin;
			}
		}
		for (visage::Frame* child : frame->children()) {
			self(child, self);
		}
	};
	walk(root, walk);

	INFO(counted << " children after create(), " << atOrigin << " of them at the window origin");
	REQUIRE(counted > 10);
	// The header legitimately starts at (0,0) and its own children may too, so the claim is not "none
	// at the origin" — it is that nobody is there by ACCIDENT, i.e. everything there has a real size
	// (asserted above) rather than being an unplaced default.
	CHECK(atOrigin < counted);
}

TEST_CASE("a knob gesture creates, shows, moves and resizes nothing") {
	// THE NAMED SUSPECT IN THE FAN HUNT, asked of our own tree. The hypothesis: some widget is
	// created or shown when a knob gesture starts, draws once before its bounds are assigned, and so
	// paints at the window origin — which is where the fan and the polygon are anchored. It fits the
	// evidence from outside (per-touch retrigger, filled shape plus strokes, and parameter-API storms
	// that never drive the gesture path and were always clean).
	//
	// It is answerable directly rather than by inference. If a gesture leaves the frame tree exactly
	// as it found it — same frames, same positions, same sizes, same visibility — then nothing is
	// shown or laid out per gesture here, and the suspect is dead in this codebase whatever is true of
	// visage's own drawing order. That is what this asks, with REAL events routed the way the window
	// routes them.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(Gui::isAvailable());
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));

	visage::Frame* root = gui.root();
	const ui::WidgetRegistry* widgets = gui.widgets();
	REQUIRE(root != nullptr);
	REQUIRE(widgets != nullptr);

	struct Placed {
		const visage::Frame* frame = nullptr;
		float x = 0.0f;
		float y = 0.0f;
		float width = 0.0f;
		float height = 0.0f;
		bool visible = false;
	};
	// A local class cannot have a defaulted friend comparison, so the comparison is a lambda below.
	// Every frame in the tree, in a fixed order, with where it sits IN THE WINDOW — which is the
	// coordinate a stray mark would be drawn at.
	const auto census = [](visage::Frame* from) {
		std::vector<Placed> seen;
		const auto walk = [&seen](visage::Frame* frame, auto&& self) -> void {
			const visage::Point at = frame->positionInWindow();
			seen.push_back({ frame, at.x, at.y, frame->width(), frame->height(), frame->isVisible() });
			for (visage::Frame* child : frame->children()) {
				self(child, self);
			}
		};
		walk(from, walk);
		return seen;
	};

	const std::vector<Placed> before = census(root);
	INFO("the assembled editor has " << before.size() << " frames");
	REQUIRE(before.size() > 10);
	// Nothing is visible at a degenerate size to begin with: a frame that is drawable and unpositioned
	// is the shape of the whole hypothesis, so it is worth stating that none exists at rest.
	for (const Placed& p : before) {
		INFO("frame at " << p.x << "," << p.y << " sized " << p.width << "x" << p.height);
		if (p.visible && p.frame != root) {
			CHECK(p.width > 0.0f);
			CHECK(p.height > 0.0f);
		}
	}

	// TOUCH PATH, AND IT USED TO BE GAMMA — which is what it was reported the fan on. Gamma stopped
	// being a knob on 2026-08-29 (`zone_coord_box.h`), and the claim here is about a KNOB gesture, so
	// the vehicle moved to the tray's neighbouring dial rather than the assertion being loosened. The
	// hypothesis and the evidence are unchanged; only which circle the hand lands on.
	const std::span<visage::Frame* const> watchers =
	    widgets->watchers(static_cast<clap_id>(ParamId::Path));
	REQUIRE_FALSE(watchers.empty());
	visage::Frame* gammaKnob = nullptr;
	for (visage::Frame* watcher : watchers) {
		if (watcher->width() > 0.0f && watcher->height() > 0.0f) {
			gammaKnob = watcher;
			break;
		}
	}
	REQUIRE(gammaKnob != nullptr);
	const visage::Point centre = centreInRoot(gammaKnob);

	// Mid-gesture is the moment that matters — a widget shown on press and hidden on release would be
	// invisible to a before/after comparison — so the tree is counted with the button still down.
	visage::Frame* down = root->frameAtPoint(centre);
	REQUIRE(down != nullptr);
	visage::MouseEvent press;
	press.window_position = centre;
	press.position = centre - down->positionInWindow();
	press.event_frame = down;
	press.button_id = visage::kMouseButtonLeft;
	press.button_state = visage::kMouseButtonLeft;
	press.is_down = true;
	press.repeat_click_count = 1;
	down->processMouseDown(press);
	const std::vector<Placed> pressed = census(root);

	visage::MouseEvent moved = press;
	moved.window_position = { centre.x, centre.y - 40.0f };
	moved.position = moved.window_position - down->positionInWindow();
	down->processMouseDrag(moved);
	const std::vector<Placed> dragging = census(root);

	visage::MouseEvent up = moved;
	up.is_down = false;
	up.button_state = visage::kMouseButtonNone;
	down->processMouseUp(up);
	const std::vector<Placed> released = census(root);

	// THE CLAIM: the tree is the same tree, throughout. Same frames in the same order — so nothing was
	// created — at the same places and sizes, with the same visibility — so nothing was shown, moved
	// or laid out. A gesture changes VALUES, not the furniture.
	INFO("frames: " << before.size() << " at rest, " << pressed.size() << " pressed, "
	                << dragging.size() << " dragging, " << released.size() << " released");
	CHECK(pressed.size() == before.size());
	CHECK(dragging.size() == before.size());
	CHECK(released.size() == before.size());
	for (size_t i = 0; i < before.size() && i < dragging.size(); ++i) {
		INFO("frame " << i << " was at " << before[i].x << "," << before[i].y << " sized "
		              << before[i].width << "x" << before[i].height << ", now at " << dragging[i].x
		              << "," << dragging[i].y << " sized " << dragging[i].width << "x"
		              << dragging[i].height);
		const auto same = [](const Placed& a, const Placed& b) {
			return a.frame == b.frame && a.x == b.x && a.y == b.y && a.width == b.width
			       && a.height == b.height && a.visible == b.visible;
		};
		CHECK(same(pressed[i], before[i]));
		CHECK(same(dragging[i], before[i]));
		CHECK(same(released[i], before[i]));
	}

	// And the drag really happened, or the case above is a photograph of a tree nobody touched.
	bool wrote = false;
	bridge.drain([&](const UiParamEvent& event) {
		wrote = wrote
		        || (event.id == static_cast<clap_id>(ParamId::Path)
		            && event.kind == UiParamEvent::Kind::Value);
	});
	CHECK(wrote);
}

TEST_CASE("every live tray knob writes its parameter, bracketed, through the assembled editor") {
	// THE OTHER HALF OF THE SYMPTOM, and the half a hit test cannot answer: the click reaches the
	// knob (the case above), but does the EDIT reach the engine? "Will not move" has two very
	// different causes — an event that never arrives, or a value that arrives and is not drawn. This
	// asks the bridge, not the pixels.
	//
	// WHAT IT CAUGHT: Pos X and Pos Y posted NOTHING. They were switched off in v1, when they were
	// real ids with no constellation behind them — a knob that moved nothing would have broken the
	// honesty rule — and phase 2 built the constellation without switching them back on. Verified
	// against the engine rather than by grepping ids, which is what the rule asks: character_params.h
	// puts the puck at `onTour + (Pos X, Pos Y)` and hands it to blendWeights.
	QuietGuiHost host;
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	bridge.refresh();
	Gui gui(host, bridge);
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	REQUIRE(gui.setSize(Gui::kLogicalWidth, Gui::kLogicalHeight));

	visage::Frame* root = gui.root();
	const ui::WidgetRegistry* widgets = gui.widgets();
	REQUIRE(root != nullptr);
	REQUIRE(widgets != nullptr);

	const auto dragAndCollect = [&](ParamId id) {
		const std::span<visage::Frame* const> watchers = widgets->watchers(static_cast<clap_id>(id));
		REQUIRE_FALSE(watchers.empty());
		visage::Frame* knob = watchers.front();
		const visage::Point centre = centreInRoot(knob);
		const double before = bridge.value(static_cast<clap_id>(id));

		// AWAY FROM WHICHEVER END IT IS ALREADY ON. Upward is more, and Path ships at 1.0 — so a drag
		// up from its default is a drag into the clamp, which posts nothing and would have been read
		// here as a dead control. The fixture has to move the value it is asking about.
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		REQUIRE(desc != nullptr);
		const double span = desc->max - desc->min;
		const bool nearTop = span > 0.0 && (before - desc->min) / span > 0.5;
		const float travel = nearTop ? 40.0f : -40.0f;
		dragThroughRoot(root, centre, { centre.x, centre.y + travel });

		struct Seen {
			bool begun = false;
			bool moved = false;
			bool ended = false;
		} seen;
		bridge.drain([&](const UiParamEvent& event) {
			if (event.id != static_cast<clap_id>(id)) {
				return;
			}
			switch (event.kind) {
			case UiParamEvent::Kind::GestureBegin:
				seen.begun = true;
				break;
			case UiParamEvent::Kind::GestureEnd:
				seen.ended = true;
				break;
			case UiParamEvent::Kind::Value:
				seen.moved = seen.moved || event.value != before;
				break;
			}
		});
		return seen;
	};

	// A host needs all three from a drag: the touch, the value, and the release that closes the undo
	// grouping. Pos and Weather were the ones that failed; Path and Level are here so a failure says whether it is
	// those two or the whole tray. Width is the tray's TENTH knob (V17 — the GLOBAL Width (master
	// scale) joins the TRAY, replacing the mock's fixed nine by design), live here because the
	// default patch is flagship; its non-V5 refusal has its own case beside the dormant knob's.
	for (const ParamId id : { ParamId::Path, ParamId::PosX, ParamId::PosY, ParamId::Weather,
		                      ParamId::Width, ParamId::VoiceLevel }) {
		INFO("param " << static_cast<int>(id));
		const auto seen = dragAndCollect(id);
		CHECK(seen.begun);
		CHECK(seen.moved);
		CHECK(seen.ended);
	}

	// AND THE ONE THAT IS STILL HONESTLY DEAD. Relief has an id and no engine behind it — nothing
	// outside params.cpp reads it — so its knob is drawn, labelled and inert, and this is what stops
	// the case above from being "switch everything on until the test passes".
	const auto relief = dragAndCollect(ParamId::Relief);
	CHECK_FALSE(relief.begun);
	CHECK_FALSE(relief.moved);
}

// THE FONT-ATLAS REPACK LEVER, proven to lever.
//
// A windowed probe needs to put a repack INSIDE a gesture rather than wait for one to occur by
// luck, and forceFontAtlasRepack() is that lever. A lever nobody has watched work is worth nothing:
// if it quietly stops forcing anything, every run that uses it comes back clean and the cleanliness
// gets read as evidence about the defect rather than about the tool. So this asserts the mechanism,
// not the call.
TEST_CASE("forcing a font atlas repack actually repacks, and keeps working") {
	const uint64_t before = visage::fontAtlasRepacks();
	const int caused = feathers::ui::forceFontAtlasRepack();

	CHECK(caused > 0);
	CHECK(visage::fontAtlasRepacks() > before);
	// The counter's own delta and the lever's report have to agree, or one of them is guessing.
	CHECK(visage::fontAtlasRepacks() - before == static_cast<uint64_t>(caused));

	// AND AGAIN — the half that matters for a probe calling this once per gesture tick. An atlas that
	// has grown has room, so a lever reusing the same size and characters would repack the first time
	// and silently do nothing afterwards. Each call has to advance to a size and a character block it
	// has never used.
	const uint64_t between = visage::fontAtlasRepacks();
	const int againCaused = feathers::ui::forceFontAtlasRepack();
	CHECK(againCaused > 0);
	CHECK(visage::fontAtlasRepacks() > between);

	const uint64_t third = visage::fontAtlasRepacks();
	CHECK(feathers::ui::forceFontAtlasRepack() > 0);
	CHECK(visage::fontAtlasRepacks() > third);
}

// THE ESTABLISHED-ATLAS LEVER, which stages a different event from the birth lever above.
//
// A birth repacks an atlas nothing is drawing from — one glyph, then two. That event has already
// been staged inside a drag and came back clean, which is unsurprising once you know how small it
// is. This one repacks an atlas the editor is ALREADY drawing from, so every resident glyph moves
// while real text quads are in flight. The two are not interchangeable and the case says so by
// asserting the size stays the one the editor uses.
TEST_CASE("forcing an established atlas to repack moves an atlas already in use") {
	// A size the editor really draws with, taken from the same accessor the widgets use — so the
	// PackedFont this repacks is the very one their text is made of.
	constexpr float kInUse = 15.0f;
	const visage::Font& before = feathers::ui::font(kInUse);
	const int atlasBefore = before.atlasWidth();

	const uint64_t repacksBefore = visage::fontAtlasRepacks();
	const int caused = feathers::ui::forceEstablishedAtlasRepack(kInUse);

	CHECK(caused > 0);
	CHECK(visage::fontAtlasRepacks() - repacksBefore == static_cast<uint64_t>(caused));

	// THE SAME FONT, NOT A NEW ONE — the whole distinction from the birth lever. If this returned a
	// different PackedFont the lever would be staging the acquitted event under a new name.
	const visage::Font& after = feathers::ui::font(kInUse);
	CHECK(after.packedFont() == before.packedFont());
	// And the atlas it repacked really did grow, which is what makes resident glyphs move.
	CHECK(after.atlasWidth() > atlasBefore);

	// Still works on a second call, for a probe firing it once per gesture tick.
	const uint64_t between = visage::fontAtlasRepacks();
	CHECK(feathers::ui::forceEstablishedAtlasRepack(kInUse) > 0);
	CHECK(visage::fontAtlasRepacks() > between);
}

TEST_CASE("no gamma setting sends a drawn coordinate somewhere it cannot be") {
	// The symptom: the line glitch came back when moving Path, only
	// over the chrome cards and only at CERTAIN GAMMA SETTINGS. A live draw landing outside the chart
	// would do that, and the view now follows an UNBOUNDED gamma base, so the suspicion that some gamma
	// value sends a coordinate somewhere wild is a reasonable one and this is the sweep that tests it.
	//
	// WHAT IT SWEPT, recorded so the negative result is worth something: 206 settings — the whole range
	// -24..+24 in quarter steps, both sides of every kGammaTurn (8.0) wrap, integer crossings of the
	// sum, sign changes either side of zero, and bases out to 1e7 — reached through the REAL corner
	// drag, so the accumulation path a hand takes is the one exercised. 3230 coordinates, zero wild.
	// So a wild coordinate out of THIS surface is not the mechanism, and the hunt belongs elsewhere.
	//
	// Kept as a guard rather than deleted with its answer: an unbounded base is permanent, so "every
	// drawn coordinate survives every gamma" is worth holding true, and it is the standing cover for
	// the band-cast guard in drawSky.
	ExploreFixture f;
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	int wild = 0;
	int checked = 0;
	// The lead's suspect boundaries: kGammaTurn=8 wraps, integer crossings of the sum, sign changes,
	// large bases from the vernier era, and the band edges.
	std::vector<double> gammas;
	for (double g = -24.0; g <= 24.0; g += 0.25) {
		gammas.push_back(g);
	}
	for (const double extra : { 7.999, 8.0, 8.001, 15.999, 16.0, 16.001, -8.0, -0.001, 0.001,
	                            1000.0, 100000.0, 1e6, 1e7 }) {
		gammas.push_back(extra);
	}
	for (const double g : gammas) {
		f.set(ParamId::GlobalGamma, 0.0);
		f.set(ParamId::AGamma, std::clamp(g, -4.0, 4.0));
		// The unbounded half, which is the one that can run away.
		// Reached through the REAL corner, so the sweep exercises the path a hand takes: kGammaTurn 8.0
		// of home per kEncoderTravel, and the corner accumulates into the unbounded base.
		const double want = g - std::clamp(g, -4.0, 4.0);
		const visage::Bounds corner = f.stage.partBounds(ExploreStage::Part::Gamma);
		const float cx = corner.x() + corner.width() * 0.5f;
		const float cy = corner.y() + corner.height() * 0.5f;
		f.stage.onMouseDown().callback(at(cx, cy));
		f.stage.onMouseDrag().callback(
		    at(cx, cy - static_cast<float>(want / 8.0 * 140.0)));
		f.stage.onMouseUp().callback(at(cx, cy - static_cast<float>(want / 8.0 * 140.0)));
		f.settleGesture();
		f.stage.draw(canvas);
		const visage::Bounds where = f.stage.terrainPlacement();
		const float coords[4] = { where.x(), where.y(), where.width(), where.height() };
		for (const float c : coords) {
			++checked;
			// A draw more than ten widget-widths away, or non-finite, is not a position.
			if (!std::isfinite(c) || std::abs(c) > 10.0f * f.stage.width()) {
				++wild;
				MESSAGE("  WILD at gamma " << g << ": placement " << where.x() << "," << where.y()
				                           << " " << where.width() << "x" << where.height());
				break;
			}
		}
		// A CRUMB IS DRAWN AT ITS POINT'S PROJECTION, so a gamma move may pan it far off the chart — which
		// is a legitimate place, clipped. What it may never be is a number that is not a position: non-finite, or past
		// anything the pixel arithmetic can carry. And off the chart it is not drawn.
		for (size_t crumb = 0; crumb < f.stage.crumbs(0).size(); ++crumb) {
			const visage::Point p = f.stage.crumbAt(crumb);
			++checked;
			if (!std::isfinite(p.x) || !std::isfinite(p.y) || std::abs(p.x) > 1e30f || std::abs(p.y) > 1e30f) {
				++wild;
				MESSAGE("  WILD crumb at gamma " << g << ": " << p.x << "," << p.y);
			}
			if (f.stage.crumbIsVisible(crumb) != f.stage.disk().contains(p)) {
				++wild;
				MESSAGE("  crumb drawn off the chart at gamma " << g << ": " << p.x << "," << p.y);
			}
		}
	}
	MESSAGE("swept " << gammas.size() << " gamma settings, " << checked << " coordinates, " << wild
	                 << " wild");
	// The sweep has to have looked at a real number of coordinates, or a zero is a zero of nothing.
	CHECK(checked > 800);
	CHECK(wild == 0);
}

TEST_CASE("MEASURE: dump the rough and the settled map at the widest stop, as pictures"
          * doctest::skip()) {
	// THE EYE-CHECK'S INSTRUMENT, and it exists because the windowed probe cannot do this job by
	// construction: the probe storms gestures, every gesture invalidates the window, and the refining
	// pass yields to a waiting window on purpose — so a map never settles in front of it. That is the
	// design working, not a venue fault, and it means the before/after has to be dumped from the
	// builder rather than photographed from a chart.
	//
	//   FEATHERS_DUMP_DIR=/some/dir ./tests/test_gui_widgets -tc="MEASURE: dump the rough*" --no-skip
	const char* out = std::getenv("FEATHERS_DUMP_DIR");
	if (out == nullptr) {
		MESSAGE("set FEATHERS_DUMP_DIR to write the two maps");
		return;
	}
	// TWO SPANS, because they answer different questions and one of them alone misleads: a BROWSING
	// zoom, where a texel is half a zone and the character is genuinely resolved, and the WIDEST stop,
	// where a texel is 384 zones and everything fine has already folded.
	for (const double spanZones : { 64.0, ExploreStage::kWidestSpanZones }) {
	const double span = spanZones / 1023.0;
	for (const OscType engine : { OscType::Morph, OscType::Stair }) {
		const std::string label = std::string(engine == OscType::Morph ? "morph" : "stair") + "-span"
		                          + std::to_string(static_cast<int>(spanZones));
		const char* name = label.c_str();
		TerrainWindow wide = windowAt(6.0, span, span);
		wide.engine = engine;
		wide.zoneBaseline = 512.0;
		TerrainMap map;
		map.request(wide);
		REQUIRE(settle(map, wide));
		const TerrainSnapshot rough = *map.snapshot();
		REQUIRE(settleRefined(map, wide));
		const TerrainSnapshot settled = *map.snapshot();

		const auto write = [&](const TerrainSnapshot& snapshot, const char* which) {
			char path[512];
			std::snprintf(path, sizeof(path), "%s/%s-%s.ppm", out, name, which);
			std::FILE* file = std::fopen(path, "wb");
			REQUIRE(file != nullptr);
			std::fprintf(file, "P6\n%d %d\n255\n", TerrainMap::kResolution, TerrainMap::kResolution);
			for (int row = TerrainMap::kResolution - 1; row >= 0; --row) {
				for (int column = 0; column < TerrainMap::kResolution; ++column) {
					const uint32_t texel = snapshot.texel(column, row);
					const unsigned char rgb[3] = { static_cast<unsigned char>((texel >> 16) & 0xFF),
						                           static_cast<unsigned char>((texel >> 8) & 0xFF),
						                           static_cast<unsigned char>(texel & 0xFF) };
					std::fwrite(rgb, 1, 3, file);
				}
			}
			std::fclose(file);
		};
		write(rough, "rough");
		write(settled, "settled");

		// And the number the pictures illustrate: how much quieter the settled one is, row to row.
		const auto step = [&](const TerrainSnapshot& snapshot) {
			double sum = 0.0;
			for (int row = 1; row < TerrainMap::kResolution; ++row) {
				const uint32_t a = snapshot.texel(64, row);
				const uint32_t b = snapshot.texel(64, row - 1);
				sum += std::abs(static_cast<double>((a >> 8) & 0xFF)
				                - static_cast<double>((b >> 8) & 0xFF));
			}
			return sum / static_cast<double>(TerrainMap::kResolution - 1);
		};
		MESSAGE(name << ": row-to-row step " << step(rough) << " rough, " << step(settled)
		             << " settled (" << rough.samples << " -> " << settled.samples
		             << " samples a texel)");
	}
	}
}

TEST_CASE("MEASURE: where the settle's wall-clock actually goes" * doctest::skip()) {
	// The averaging visibly happened slowly after controls settled — too slow as it was.
	// Before making anything faster, find out what the time IS.
	// Three candidates and they want different fixes: raw sampling cost, dead time between passes, or a
	// republish cadence that makes a fast thing LOOK like a crawl.
	// TWO SPANS, because the decomposition is a SCALING question: if the refinement's wall-clock rises
	// with K it is sampling-bound and the lever is parallelism; if a fixed lump stays behind at both, it
	// is scheduling and the lever is the pass shape. A residual attributed by subtraction is a label,
	// which is how the first draft of this case called 196ms "scheduling and republishing" before the
	// per-sample timing showed the macro it blamed was 2% of a sample.
	for (const double spanZones : { 6000.0, ExploreStage::kWidestSpanZones }) {
	const double span = spanZones / 1023.0;
	TerrainWindow wide = windowAt(6.0, span, span);
	wide.zoneBaseline = 512.0;
	const double cell = wide.travelYSpan * 1023.0 / static_cast<double>(TerrainMap::kResolution);
	const int target = terrainSamplesFor(wide.engine, cell);

	TerrainMap map;
	const auto start = std::chrono::steady_clock::now();
	map.request(wide);
	REQUIRE(settle(map, wide));
	const auto roughAt = std::chrono::steady_clock::now();
	REQUIRE(settleRefined(map, wide));
	const auto settledAt = std::chrono::steady_clock::now();

	const auto ms = [](auto a, auto b) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count();
	};

	// AND THE SAMPLING ON ITS OWN, timed here rather than inferred: one texel's worth of work, times
	// the map, times the schedule's K. Anything the settle costs beyond this is not sampling.
	const auto zone = static_cast<uint16_t>(std::lround(wide.zoneBaseline));
	const auto cpuStart = std::chrono::steady_clock::now();
	double sink = 0.0;
	for (int i = 0; i < TerrainMap::kResolution * target; ++i) {
		const TerrainFeatures one =
		    terrainFeaturesAt(OscType::Morph, wide.pin, zone, 0.0f, 0.0, 0.0, kNoPlaneResidue,
		                      static_cast<double>(i) * 1e-4);
		sink += one.energy;
	}
	const auto cpuEnd = std::chrono::steady_clock::now();
	REQUIRE(sink > 0.0);
	const double perSample =
	    static_cast<double>(
	        std::chrono::duration_cast<std::chrono::microseconds>(cpuEnd - cpuStart).count())
	    / static_cast<double>(TerrainMap::kResolution * target);
	const double sampling = perSample * static_cast<double>(TerrainMap::kResolution)
	                        * static_cast<double>(TerrainMap::kResolution)
	                        * static_cast<double>(target) / 1000.0;

	MESSAGE("at the widest stop, K=" << target << ": rough in " << ms(start, roughAt)
	                                 << "ms, settled " << ms(roughAt, settledAt)
	                                 << "ms after that (" << ms(start, settledAt) << "ms total)");
	MESSAGE("sampling alone accounts for " << sampling << "ms of the refinement; the rest is "
	                                       << static_cast<double>(ms(roughAt, settledAt)) - sampling
	                                       << "ms of scheduling and republishing");
	MESSAGE("one sample costs " << perSample << "us; a refined map is "
	                            << TerrainMap::kResolution * TerrainMap::kResolution * target
	                            << " of them");
	}
}

TEST_CASE("MEASURE: what a sample is actually made of" * doctest::skip()) {
	// The residual in the case above was 196ms and "scheduling and republishing" was a LABEL rather
	// than a measurement — the publishes are eight 64KB copies, which cannot be a fifth of a second.
	// So: time the two halves of a sample separately.
	SliceOrientationPin unpinned;
	constexpr int kRuns = 60000;
	const auto time = [](const char* what, auto&& fn) {
		const auto start = std::chrono::steady_clock::now();
		double sink = 0.0;
		for (int i = 0; i < kRuns; ++i) {
			sink += fn(i);
		}
		const auto end = std::chrono::steady_clock::now();
		REQUIRE(sink != 12345.6789);
		const double us =
		    static_cast<double>(
		        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count())
		    / static_cast<double>(kRuns);
		MESSAGE(what << ": " << us << "us each");
		return us;
	};
	const double features = time("terrainFeaturesAt (a whole wavetable and its spectrum)", [&](int i) {
		return terrainFeaturesAt(OscType::Morph, unpinned, 512, 0.0f, 0.0, 0.0, kNoPlaneResidue,
		                         static_cast<double>(i) * 1e-4)
		    .energy;
	});
	// THE FRAME IS HOISTED OUT OF THE TIMING because it is hoisted out of the builder: the plane is
	// derived once per ROW and the macro is read once per TEXEL, so timing the derivation inside this
	// lambda prices a row's work against one texel's. My first draft did exactly that and reported the
	// macro at 30% of a sample when it is a fortieth of it — the same shape of error as timing a
	// constructor inside a loop that does not construct.
	// THE MACRO IS NOT PART OF A SAMPLE ANY MORE — the country left the draw by design, so a
	// texel is a wavetable build, a 32-point DFT and the colour arithmetic, full stop. What used to be
	// timed here was a layer that no longer exists; the number it produced (a fortieth of a sample) is
	// in the history beside the reason it went.
	MESSAGE("a sample is " << features << "us of character, and nothing else");
}

TEST_CASE("MEASURE: the diagonal's angle in the field, against the angle the basis predicts"
          * doctest::skip()) {
	// Most views showed a strong diagonal overlay pattern, where the phi FM would suggest something
	// more irregular. The basis-side measurement (slice-build suite) says the triangles' banding
	// slopes CLUSTER — morph's mean is about −0.85 with the extreme pinned at exactly −1 — so the
	// prediction is a coherent band near −40°, and near −45° for whatever sits at the extreme.
	//
	// THIS IS THE OTHER END OF THE SAME CLAIM: measure the angle in the field itself and see whether it
	// matches. If it does, the mechanism is the basis and neither the grain nor the sampling lattice
	// needs to be blamed. Two discriminators come free with it:
	//   - TWO CELL SIZES over the same world window. A moiré angle is a function of the sampling
	//     lattice and moves with it; world structure holds its angle in world coordinates.
	//   - WITH AND WITHOUT THE GRAIN, by measuring the ENERGY field (which has no grain in it at all)
	//     beside the drawn colour (which does).
	// Measured from VALUES throughout — no image, no write-out path. An orientation read off a picture
	// has already cost this arc one wrong answer.
	SliceOrientationPin unpinned;
	const auto orientation = [](const std::vector<double>& field, int side) {
		// The structure tensor: the dominant GRADIENT direction, from which the band direction is a
		// right angle away. Central differences, so the answer is the field's own and not an edge's.
		double jxx = 0.0;
		double jyy = 0.0;
		double jxy = 0.0;
		for (int y = 1; y + 1 < side; ++y) {
			for (int x = 1; x + 1 < side; ++x) {
				const double gx = field[static_cast<size_t>(y) * side + x + 1]
				                  - field[static_cast<size_t>(y) * side + x - 1];
				const double gy = field[static_cast<size_t>(y + 1) * side + x]
				                  - field[static_cast<size_t>(y - 1) * side + x];
				jxx += gx * gx;
				jyy += gy * gy;
				jxy += gx * gy;
			}
		}
		const double gradient = 0.5 * std::atan2(2.0 * jxy, jxx - jyy);
		const double band = gradient + 3.14159265358979 / 2.0;
		const double coherence =
		    std::sqrt((jxx - jyy) * (jxx - jyy) + 4.0 * jxy * jxy) / std::max(1e-12, jxx + jyy);
		// As a slope dhome/dtravel, which is what the basis predicts directly.
		return std::pair<double, double> { std::tan(band), coherence };
	};

	constexpr int kSide = 96;
	for (const double spanZones : { 49152.0, 3069.0, 64.0 }) {
		const double span = spanZones / 1023.0;
		std::vector<double> energy(static_cast<size_t>(kSide) * kSide);
		std::vector<double> drawn(static_cast<size_t>(kSide) * kSide);
		for (int y = 0; y < kSide; ++y) {
			for (int x = 0; x < kSide; ++x) {
				// Equal steps on both axes, which is what the chart draws (A13) — and under backlog GY
				// both axes are TRAVEL, walked around a home that does not move. The home below is the
				// one place the plane is taken at; y walks travel-Y as x walks travel-X.
				const double home = 6.0;
				const double travelY = (static_cast<double>(y) / kSide - 0.5) * span;
				const double travelX = (static_cast<double>(x) / kSide - 0.5) * span;
				const TerrainFeatures f =
				    terrainFeaturesAt(OscType::Morph, unpinned, 512, 0.0f, travelX, travelY,
				                      kNoPlaneResidue, home - 512.0 / 1023.0, {}, feathers::WarpMode::Off);
				energy[static_cast<size_t>(y) * kSide + x] = f.energy;
				const uint32_t c =
				    terrainColour(f, travelX, travelY, -1);
				drawn[static_cast<size_t>(y) * kSide + x] =
				    static_cast<double>((c >> 16) & 0xFF) + static_cast<double>((c >> 8) & 0xFF)
				    + static_cast<double>(c & 0xFF);
			}
		}
		// AND THE SAME FIELD AVERAGED OVER EACH CELL, which is the third discriminator and the own:
		// the prediction was that it is not aliasing. Averaging is a low-pass — it attenuates what folded
		// and leaves what is genuinely there — so if the diagonal is aliasing its coherence should fall
		// when a texel becomes the mean of its cell, and if it is real structure it should not. Computed
		// here rather than read off a build so the answer holds whatever the shipped cap happens to be.
		std::vector<double> meaned(static_cast<size_t>(kSide) * kSide);
		const double cell = span / kSide;
		for (int y = 0; y < kSide; ++y) {
			for (int x = 0; x < kSide; ++x) {
				const double home = 6.0 + (static_cast<double>(y) / kSide - 0.5) * span;
				const double travel = (static_cast<double>(x) / kSide - 0.5) * span;
				double sum = 0.0;
				for (int i = 0; i < 2; ++i) {
					const TerrainSampleOffset at = terrainSampleOffset(i, 2);
					sum += terrainFeaturesAt(OscType::Morph, unpinned, 512, 0.0f,
					                         travel + at.travel * cell, 0.0, kNoPlaneResidue,
					                         home + at.home * cell - 512.0 / 1023.0)
					           .energy;
				}
				meaned[static_cast<size_t>(y) * kSide + x] = sum * 0.5;
			}
		}
		const auto e = orientation(energy, kSide);
		const auto d = orientation(drawn, kSide);
		const auto m = orientation(meaned, kSide);
		MESSAGE("span " << spanZones << " zones: energy band slope " << e.first << " (coherence "
		                << e.second << "), drawn band slope " << d.first << " (coherence " << d.second
		                << "), energy MEANED over the cell " << m.first << " (coherence " << m.second
		                << ")");
	}
}

TEST_CASE("browsing does not re-roll the world: the same place keeps its texel across a recentre") {
	// The symptom: dropping the star in a dark region, then letting the display resettle after the
	// recentre, the destination appeared to be in different topography.
	//
	// THE MECHANISM, read out of the code rather than guessed: `travelYAtRow` is
	// `homeCentre + (0.5 - t) * homeSpan` (terrain.cpp) — every texel's sample point is measured FROM
	// THE VIEW'S CENTRE. So the sampling lattice rides the window: move the view by a third of a texel
	// and all sixteen thousand sample points move with it, and wherever the character is finer than a
	// texel — which past the trigger is everywhere — each one lands on a different piece of world and
	// comes back a different colour. The world did not change; the questions we asked it did.
	//
	// It is a BROWSING-INTEGRITY defect rather than a look one: a map you cannot navigate without it
	// re-rolling is a map you cannot learn, which is the rule this whole surface is built on (the file's
	// own "a colour that meant one thing before you zoomed and another after is a map that cannot be
	// learned"). Averaging hid it; sampling once exposed it.
	//
	// KNOWN-BAD IS THE SHIPPED BUILD: with the lattice riding the view, the loop below finds the same
	// world position wearing two different colours.
	const double span = ExploreStage::kWidestSpanZones / 1023.0;
	const double cell = span / static_cast<double>(TerrainMap::kResolution);

	TerrainWindow before = windowAt(6.0, span, span);
	before.zoneBaseline = 512.0;
	// A move that is NOT a whole number of texels — which is what any recentre onto a star is.
	TerrainWindow after = before;
	after.travelYCentre += cell * 0.37;
	after.travelXCentre += cell * 0.21;

	TerrainMap first;
	first.request(before);
	REQUIRE(settle(first, before));
	const TerrainSnapshot a = *first.snapshot();
	TerrainMap second;
	second.request(after);
	REQUIRE(settle(second, after));
	const TerrainSnapshot b = *second.snapshot();
	REQUIRE(a.rows == TerrainMap::kResolution);
	REQUIRE(b.rows == TerrainMap::kResolution);

	// THE SAME WORLD POSITIONS, asked of both maps. Every texel of the first map whose place also lies
	// in the second must be the same colour there — the map is a pure function of position, so anything
	// else is the lattice talking rather than the world.
	size_t compared = 0;
	size_t agreed = 0;
	for (int row = 8; row < TerrainMap::kResolution - 8; ++row) {
		for (int column = 8; column < TerrainMap::kResolution - 8; ++column) {
			const double home = TerrainMap::travelYAtRow(a.window, row);
			const double travel = TerrainMap::travelXAtColumn(a.window, column);
			// Where that same world place sits in the second map — the nearest texel to it.
			const long otherRow = std::lround(TerrainMap::rowAtTravelY(b.window, home));
			const long otherColumn = std::lround(TerrainMap::columnAtTravelX(b.window, travel));
			if (otherRow < 0 || otherRow >= TerrainMap::kResolution || otherColumn < 0
			    || otherColumn >= TerrainMap::kResolution) {
				continue;
			}
			++compared;
			agreed += a.texel(column, row)
			                  == b.texel(static_cast<int>(otherColumn), static_cast<int>(otherRow))
			              ? 1
			              : 0;
		}
	}
	REQUIRE(compared > 1000);
	INFO(agreed << " of " << compared << " world positions kept their colour across the recentre");
	CHECK(agreed == compared);
}

TEST_CASE("dropping the star somewhere and letting the view resettle leaves the world where it was") {
	// THE GESTURE, as the acceptance case: aim at a place at a wide zoom, put the sound there,
	// let the release re-centre — and the destination must be the country you were looking at, not a
	// re-rolled one. "the destination looks like its in different topography."
	//
	// The map-level case above pins the MECHANISM (a lattice that rides the view); this one pins the
	// EXPERIENCE, through the assembled widget, with a real press and release and the re-centre they
	// trigger. Both are needed: the mechanism case would still pass if the surface stopped asking for
	// maps at all, and this one would still pass if it asked for the right map by luck.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.setZoom(ExploreStage::minZoom());
	f.now += ExploreStage::kZoomDwellMs; // the map follows a zoom once it has rested
	f.stage.draw(canvas);

	const auto settled = [&]() -> std::shared_ptr<const TerrainSnapshot> {
		for (int waited = 0; waited < 8000; ++waited) {
			const std::shared_ptr<const TerrainSnapshot> s = f.terrain.snapshot();
			if (s->rows == TerrainMap::kResolution
			    && s->window.travelYCentre == doctest::Approx(f.stage.viewCentre())) {
				return s;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		return {};
	};

	const std::shared_ptr<const TerrainSnapshot> before = settled();
	REQUIRE(before);

	// The place being aimed at: somewhere off-centre, so the re-centre really moves the view.
	const visage::Point c = f.stage.disk().centre();
	const float aimX = c.x + 70.0f;
	const float aimY = c.y - 90.0f;
	const double aimedHome = f.stage.travelYAtScreenY(aimY);
	const double aimedTravel = f.stage.travelXAtScreenX(aimX);
	// What the world looks like there, BEFORE the jump — read out of the map on screen.
	const long fromRow = std::lround(TerrainMap::rowAtTravelY(before->window, aimedHome));
	const long fromColumn = std::lround(TerrainMap::columnAtTravelX(before->window, aimedTravel));
	REQUIRE(fromRow >= 0);
	REQUIRE(fromRow < TerrainMap::kResolution);
	REQUIRE(fromColumn >= 0);
	REQUIRE(fromColumn < TerrainMap::kResolution);

	// Drop the star there. The release re-centres, which is the move that used to re-roll the world.
	f.stage.onMouseDown().callback(at(aimX, aimY));
	f.stage.onMouseUp().callback(at(aimX, aimY));
	f.settleGesture();
	// backlog GE: the close re-centres by PANNING now, so let the view land before reading it.
	f.settleScrub();
	f.stage.draw(canvas);
	const std::shared_ptr<const TerrainSnapshot> after = settled();
	REQUIRE(after);
	// The view really did move, or this case is about nothing.
	REQUIRE(after->window.travelYCentre != doctest::Approx(before->window.travelYCentre));

	// AND THE DESTINATION IS THE SAME COUNTRY. Not similar — the same texels, because the sampling
	// lattice belongs to the world and the world did not move.
	size_t compared = 0;
	size_t agreed = 0;
	for (int row = 4; row < TerrainMap::kResolution - 4; ++row) {
		for (int column = 4; column < TerrainMap::kResolution - 4; ++column) {
			const double home = TerrainMap::travelYAtRow(after->window, row);
			const double travel = TerrainMap::travelXAtColumn(after->window, column);
			const long oldRow = std::lround(TerrainMap::rowAtTravelY(before->window, home));
			const long oldColumn = std::lround(TerrainMap::columnAtTravelX(before->window, travel));
			if (oldRow < 0 || oldRow >= TerrainMap::kResolution || oldColumn < 0
			    || oldColumn >= TerrainMap::kResolution) {
				continue;
			}
			++compared;
			agreed += after->texel(column, row)
			                  == before->texel(static_cast<int>(oldColumn), static_cast<int>(oldRow))
			              ? 1
			              : 0;
		}
	}
	REQUIRE(compared > 500);
	INFO(agreed << " of " << compared << " overlapping places kept their colour across the drop");
	CHECK(agreed == compared);
}

TEST_CASE("MEASURE: prototype an OPENED macro field, against the banded one that ships"
          * doctest::skip()) {
	// THE DIAGNOSIS, and it is the one that explains every variant tried so far: "adjusting all of
	// the frequencies at different amounts" cannot make the landscape two-dimensional, because
	// `phiOrientationFor` is a function of ONE SCALAR — and the level sets of a one-scalar field are
	// LINES however that scalar is parameterised. Leaning the coordinate turns the lines. It cannot
	// make them into shapes. The macro never got the decode the character got.
	//
	// SO THIS PROTOTYPES THE DECODE FOR IT: several φ components, each read along its OWN direction in
	// (home, travel), with the directions taken from M so they turn when the plane turns. Two families
	// of bands at different angles interfere into a lattice; four make a country.
	//
	// UI-SIDE ONLY, AND HERE THAT IS FREE RATHER THAN CAREFUL: nothing in the engine reads a φ-derived
	// orientation field any more — the field is deleted and the born plane is M(0, home) — so opening
	// the drawn field cannot reach a sound.
	constexpr int kSide = 64;
	constexpr int kComponents = 4;
	// φ-distinct rates, the same long ladder the orientation bank uses: nothing here may be fast enough
	// to flicker between neighbouring zones, or the country stops being a country.
	constexpr double kRates[kComponents] = { 0.1458980, 0.0901699, 0.0557281, 0.0344419 };
	constexpr double kWeights[kComponents] = { 1.0, 0.618, 0.382, 0.236 };
	// AND THE SAME SHAPE AT RATES COMPARABLE TO THE VIEW, which is the hypothesis the first run forced:
	// a component whose period is many times the window is LOCALLY LINEAR over it, and a sum of linear
	// functions is linear, so its level sets are straight parallel lines however many components are
	// added and however each one is turned. If that is the mechanism, only rates near the view's own
	// scale can produce a form — and those are character-layer rates, which is a different thing from a
	// country.
	constexpr double kFastRates[kComponents] = { 2.618, 1.618, 1.0, 0.618 };
	// AND A MIXED LADDER, which is the shape the first three runs argue for: the slowest component
	// carries the country's IDENTITY at the long period §7.1b settled on, and the faster ones carry
	// visible form, with the weights falling so the long one still decides what neighbourhood you are
	// in. Still φ-power-distinct, still the formula.
	constexpr double kMixedRates[kComponents] = { 0.1458980, 0.3819660, 1.0, 1.6180340 };
	// SLOW-DOMINANT: the same view-scale ladder, but the WEIGHT falling as the rate rises rather than
	// with it, so the slowest component decides what country you are in and the faster ones carve the
	// shapes inside it. Which way the weights run is the whole difference between "the country has
	// texture" and "the texture has replaced the country".
	constexpr double kSlowFirst[kComponents] = { 0.6180340, 1.0, 1.6180340, 2.6180340 };

	const auto opened = [&](double rotate, double span, bool fast, bool opened2d, bool mixed,
	                        bool slowFirst, std::array<float, kSide * kSide>& out) {
		SliceOrientationPin pin;
		if (rotate != 0.0) {
			pin.pinned = true;
			pin.generator.rotate = static_cast<float>(rotate);
		}
		for (int y = 0; y < kSide; ++y) {
			const double home = 6.0 + (static_cast<double>(y) / kSide - 0.5) * span;
			const TerrainFrames frames = terrainFramesAt(pin, home, kNoPlaneResidue);
			for (int x = 0; x < kSide; ++x) {
				const double travel = (static_cast<double>(x) / kSide - 0.5) * span;
				double sum = 0.0;
				double weight = 0.0;
				for (int k = 0; k < kComponents; ++k) {
					// EACH COMPONENT GETS THE CHARACTER'S OWN DECODE, which is the part every earlier
					// variant left out: the phi^n rotation and FM apply to every offset AND every
					// frequency. A component is read at a phase AND at a RATE, both fed from M — phase
					// leans along coordinate k's phase coefficient, rate is scaled by coordinate k's RATE
					// coefficient. That second half is what makes the field two-dimensional rather than
					// tilted: with the rate itself a function of travel, the triangle's argument is
					// bilinear in (home, travel), so its level sets are HYPERBOLAE rather than lines. No
					// amount of tilting a linear argument can do that, which is why every variant so far
					// measured 0.97 and up.
					const size_t slot = static_cast<size_t>(k) * 7;
					const double lean = static_cast<double>(frames.morph.x[slot]);
					const double rate = static_cast<double>(frames.morph.rateFor(slot, travel, 0.0));
					const double ladder = slowFirst ? kSlowFirst[k]
					                                : (mixed ? kMixedRates[k]
					                                         : (fast ? kFastRates[k] : kRates[k]));
					const double at = (home + lean * travel) * ladder * (opened2d ? rate : 1.0);
					const double w = at - std::floor(at);
					const double tri = w < 0.5 ? 4.0 * w - 1.0 : 3.0 - 4.0 * w;
					sum += kWeights[k] * tri;
					weight += kWeights[k];
				}
				out[static_cast<size_t>(y) * kSide + x] =
				    static_cast<float>(0.5 + 0.5 * sum / weight);
			}
		}
	};

	const auto tensor = [&](const std::array<float, kSide * kSide>& f) {
		double gxx = 0.0;
		double gyy = 0.0;
		double gxy = 0.0;
		for (int y = 1; y < kSide - 1; ++y) {
			for (int x = 1; x < kSide - 1; ++x) {
				const double gx = f[static_cast<size_t>(y) * kSide + x + 1]
				                  - f[static_cast<size_t>(y) * kSide + x - 1];
				const double gy = f[static_cast<size_t>(y + 1) * kSide + x]
				                  - f[static_cast<size_t>(y - 1) * kSide + x];
				gxx += gx * gx;
				gyy += gy * gy;
				gxy += gx * gy;
			}
		}
		const double trace = gxx + gyy;
		const double diff = std::sqrt((gxx - gyy) * (gxx - gyy) + 4.0 * gxy * gxy);
		return trace > 0.0 ? diff / trace : 0.0;
	};

	for (const double span : { 0.25, 3.0 }) {
		for (const int variant : { 0, 1, 2, 3, 4, 5 }) {
			const bool fast = variant == 1 || variant == 3;
			const bool opened2d = variant >= 2;
			const bool mixed = variant == 4;
			const bool slowFirst = variant == 5;
			std::array<float, kSide * kSide> rest {};
			std::array<float, kSide * kSide> turned {};
			opened(0.0, span, fast, opened2d, mixed, slowFirst, rest);
			opened(2.1, span, fast, opened2d, mixed, slowFirst, turned);
			double moved = 0.0;
			for (size_t i = 0; i < rest.size(); ++i) {
				moved += std::abs(static_cast<double>(rest[i]) - static_cast<double>(turned[i]));
			}
			MESSAGE("macro at span "
			        << span
			        << (variant == 0 ? " [phase-only, country rates]"
			                         : (variant == 1 ? " [phase-only, view-scale rates]"
			                                         : (variant == 2 ? " [PHASE+RATE, country rates]"
			                                                         : (variant == 3
			                                                                ? " [PHASE+RATE, view-scale rates]"
			                                                                : (variant == 4
			                                                                       ? " [PHASE+RATE, MIXED ladder]"
			                                                                       : " [PHASE+RATE, SLOW-DOMINANT]")))))
			        << ": coherence " << tensor(rest) << " -> " << tensor(turned) << ", moved "
			        << moved / rest.size() << "/texel");
		}
	}
}


TEST_CASE("MEASURE: the waveform-owned picture, rest and turned, at both spans"
          * doctest::skip()) {
	// DUMP AND LOOK BEFORE CASING — the standing order, and this is the change it exists for: the
	// country left the draw, so what a map looks like without it is a thing to SEE before anything is
	// asserted about it. Rest and turned at the same place, at a browsing zoom and at the widest stop.
	//
	//   FEATHERS_DUMP_DIR=/some/dir ./tests/test_gui_widgets -tc="MEASURE: the waveform-owned*" --no-skip
	const char* out = std::getenv("FEATHERS_DUMP_DIR");
	if (out == nullptr) {
		MESSAGE("set FEATHERS_DUMP_DIR to write the pair");
		return;
	}
	constexpr int kSide = TerrainMap::kResolution;
	for (const double spanZones : { 1023.0, ExploreStage::kWidestSpanZones }) {
		const double span = spanZones / 1023.0;
		for (const bool turned : { false, true }) {
			SliceOrientationPin pin;
			if (turned) {
				pin.pinned = true;
				pin.generator.rotate = 2.1f;
			}
			TerrainWindow window = windowAt(6.0, span, span);
			window.engine = OscType::Morph;
			window.zoneBaseline = 512.0;
			window.pin = pin;
			std::vector<unsigned char> rgb(static_cast<size_t>(kSide) * kSide * 3);
			for (int row = 0; row < kSide; ++row) {
				// **BOTH AXES ARE TRAVEL** (backlog GY). This instrument walked rows as HOME and derived a
				// plane per row from it — an independent restatement of the builder, and it restated the
				// defect too. Home is now the one place the whole plane is taken at; the row is travel-Y.
				const double travelY = TerrainMap::travelYAtRow(window, row);
				const double home = window.gammaBase;
				const auto frames = terrainFramesAt(pin, home, kNoPlaneResidue);
				const auto zone =
				    static_cast<uint16_t>(std::clamp(std::lround(home * 1023.0), 0L, 1023L));
				const double base = home - static_cast<double>(zone) / 1023.0;
				for (int column = 0; column < kSide; ++column) {
					const double travelX = TerrainMap::travelXAtColumn(window, column);
					const TerrainFeatures f =
					    terrainFeaturesOn(OscType::Morph, frames, zone, 0.0f, travelX, travelY, base);
					const uint32_t c = terrainColour(f, travelX, travelY, -1);
					const size_t at = (static_cast<size_t>(row) * kSide + column) * 3;
					rgb[at] = static_cast<unsigned char>((c >> 16) & 0xFF);
					rgb[at + 1] = static_cast<unsigned char>((c >> 8) & 0xFF);
					rgb[at + 2] = static_cast<unsigned char>(c & 0xFF);
				}
			}
			const std::string path = std::string(out) + "/waveform" + std::to_string(static_cast<int>(spanZones))
			                         + (turned ? "-turned.ppm" : "-rest.ppm");
			std::FILE* file = std::fopen(path.c_str(), "wb");
			REQUIRE(file != nullptr);
			std::fprintf(file, "P6\n%d %d\n255\n", kSide, kSide);
			std::fwrite(rgb.data(), 1, rgb.size(), file);
			std::fclose(file);
		}
	}
	// `std::string`, NOT the bare `const char*`. MEASURED against libs/doctest 2.5.0: a
	// `const char*` streamed into INFO/MESSAGE prints as **1** — it converts to bool before it
	// reaches a string overload — so this line said "written to 1" and named nothing. A literal
	// and a std::string both print properly; only a pointer variable is eaten.
	MESSAGE("written to " << std::string(out));
}

TEST_CASE("the waveform owns the picture: nothing but the character colours a morph map") {
	// The waveform owns the picture (the decoding scheme can be revisited later if needed).
	// The φ macro field that used to tint every morph texel is gone from the draw. This is that decision
	// as a property rather than a deletion nobody can check.
	//
	// THE SHAPE OF THE CLAIM, and it took a moment to find one that is exact rather than nearly true:
	// the colour still takes `home` and `travel`, because the GRAIN does — that is a deliberate fixed
	// frequency texture, kept deliberately, and it is a function of position by design. But the grain's depth is
	// proportional to the place's own ROUGHNESS, so at roughness zero it drops out exactly, and what is
	// left must be a function of the character alone. If any position-dependent tint survived anywhere
	// in the mix, two distant places with identical features would still draw differently, and they
	// must not.
	TerrainFeatures f;
	f.energy = 0.42f;
	f.centroid = 0.23f;
	f.roughness = 0.0f; // so the grain's contribution is exactly zero, not merely small

	const uint32_t here = terrainColour(f, 0.37, 0.0, -1);
	for (const double travelY : { 0.0, 1.9, 6.0, 31.0, 137.5 }) {
		for (const double travelX : { -1.5, -0.2, 0.0, 0.4, 2.0 }) {
			CAPTURE(travelY);
			CAPTURE(travelX);
			CHECK(terrainColour(f, travelX, travelY, -1) == here);
		}
	}

	// AND IT IS NOT A CONSTANT FUNCTION, or the case above passes on a build that draws one colour
	// everywhere: the character still decides, and a different character is a different pixel.
	TerrainFeatures other = f;
	other.centroid = 0.61f;
	CHECK(terrainColour(other, 0.37, 0.0, -1) != here);
	other = f;
	other.energy = 0.72f;
	CHECK(terrainColour(other, 0.37, 0.0, -1) != here);

	SUBCASE("and a stair map keeps the one region term that was never a field") {
		// The pattern family is `zone >> 7` — eight discrete regions of the world, which §4a allows to
		// anchor colour honestly, and a part already judged right by eye. It is not the
		// orientation field and it did not leave with it.
		const uint32_t familyZero = terrainColour(f, 0.37, 0.0, 0);
		bool anyDiffers = false;
		for (int family = 1; family < kStairFamilies; ++family) {
			anyDiffers = anyDiffers || terrainColour(f, 0.37, 0.0, family) != familyZero;
		}
		CHECK(anyDiffers);
		// ...and it is still a function of the family alone, not of where in the family you stand.
		for (const double home : { 0.0, 6.0, 31.0 }) {
			CHECK(terrainColour(f, home, 0.8, 3) == terrainColour(f, 0.37, 0.0, 3));
		}
	}
}


TEST_CASE("resonate: the map colours by Resonate's own knots, spread like Stair's, and no other engine's colour moved") {
	// THE DECISION: bake Resonate its own normalisation knots. Before it, a Resonate map
	// stood its colour axes through the Morph/Stair table: its rest poses are dark and smooth, so the
	// first axis (the standardised centroid) sat in one corner and the map drew in one hue.
	//
	// MEASURED over the calibration case's sample (every 7th zone x 17 travels, sound A's plane): the
	// median and interquartile spread of the two colour axes the map draws — `tilt`, the standardised
	// centroid, and `second`, the parity residual — for Stair through the shared table (the control),
	// Resonate through the shared table (before), and Resonate through its own (after).
	struct Spread {
		float median;
		float iqr;
	};
	const auto spreadOf = [](std::vector<float> v) {
		std::sort(v.begin(), v.end());
		const auto at = [&](double p) { return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))]; };
		return Spread { at(0.5), at(0.75) - at(0.25) };
	};
	const auto axes = [&](OscType engine, const FeatureNorm& norm) {
		SliceOrientationPin pin;
		std::vector<float> tilt;
		std::vector<float> second;
		for (int zone = 0; zone < 1024; zone += 7) {
			for (int step = -8; step <= 8; ++step) {
				const TerrainFeatures f =
				    terrainFeaturesAt(engine, pin, static_cast<uint16_t>(zone), 0.0f, step * 0.005, 0.0);
				tilt.push_back(standardize(norm.centroid, f.centroid));
				second.push_back(residualParity(norm, f.centroid, f.parity));
			}
		}
		return std::make_pair(spreadOf(tilt), spreadOf(second));
	};
	const auto [stairTilt, stairSecond] = axes(OscType::Stair, kBakedFeatureNorm);
	const auto [beforeTilt, beforeSecond] = axes(OscType::Resonate, kBakedFeatureNorm);
	const auto [afterTilt, afterSecond] = axes(OscType::Resonate, featureNormFor(OscType::Resonate));
	MESSAGE("tilt   median/IQR — Stair " << stairTilt.median << "/" << stairTilt.iqr << ", Resonate before "
	                                      << beforeTilt.median << "/" << beforeTilt.iqr << ", after "
	                                      << afterTilt.median << "/" << afterTilt.iqr);
	MESSAGE("second median/IQR — Stair " << stairSecond.median << "/" << stairSecond.iqr
	                                      << ", Resonate before " << beforeSecond.median << "/"
	                                      << beforeSecond.iqr << ", after " << afterSecond.median << "/"
	                                      << afterSecond.iqr);
	// Comparable to Stair's: at least half its spread on both axes, and a first axis off the floor.
	CHECK(afterTilt.iqr >= 0.5f * stairTilt.iqr);
	CHECK(afterSecond.iqr >= 0.5f * stairSecond.iqr);
	CHECK(afterTilt.median > 0.2f);
	CHECK(afterTilt.median < 0.8f);

	// AND THE MAP ACTUALLY TAKES IT: a Resonate texel's colour through the engine argument is the colour
	// its own table gives, not the shared one's (where the two tables disagree, which is most places).
	const TerrainFeatures sample = terrainFeaturesAt(OscType::Resonate, SliceOrientationPin {}, 300, 0.0f, 0.02, 0.0);
	CHECK(terrainColour(sample, 0.02, 0.0, -1, kShippedMap, kShippedSecondAxis, OscType::Resonate)
	      != terrainColour(sample, 0.02, 0.0, -1, kShippedMap, kShippedSecondAxis, OscType::Morph));

	// AND THE MAP BUILDER PASSES ITS WINDOW'S ENGINE: a finished Resonate map's texel is the colour its
	// own table gives at that texel, recomputed here from the window the snapshot carries.
	{
		TerrainWindow w = windowAt(0.3, 64.0 / 1023.0, 64.0 / 1023.0);
		w.engine = OscType::Resonate;
		TerrainMap map;
		map.request(w);
		REQUIRE(settle(map, w));
		const std::shared_ptr<const TerrainSnapshot> s = map.snapshot();
		const int row = 40;
		const double travelY = TerrainMap::travelYAtRow(s->window, row);
		const double travelX = TerrainMap::travelXAtColumn(s->window, 7);
		const TerrainFeatures features = terrainFeaturesAt(
		    OscType::Resonate, s->window.pin, static_cast<uint16_t>(std::lround(s->window.zoneBaseline)),
		    static_cast<float>(s->window.offsetBaseline), travelX, travelY, kNoPlaneResidue, s->window.gammaBase,
		    s->window.scales(), s->window.warpMode, s->window.weaveModel);
		CHECK(s->texel(7, row)
		      == terrainColour(features, travelX, travelY, -1, colourMapFor(OscType::Resonate), kShippedSecondAxis, OscType::Resonate));
	}

	// NO OTHER ENGINE'S COLOUR MOVED. Morph, Stair and V5 read the shared table — the very object, which
	// the staleness case pins to its own bytes — and through the engine argument every texel of theirs
	// is what the defaulted call (every caller before Resonate) draws.
	for (const OscType engine : { OscType::Morph, OscType::Stair, OscType::V5 }) {
		CAPTURE(static_cast<int>(engine));
		CHECK(&featureNormFor(engine) == &kBakedFeatureNorm);
		for (int zone = 0; zone < 1024; zone += 31) {
			const TerrainFeatures f = terrainFeaturesAt(engine, SliceOrientationPin {}, static_cast<uint16_t>(zone),
			                                            0.0f, 0.01, 0.0);
			const int family = engine == OscType::Stair ? stairFamilyAt(static_cast<uint16_t>(zone)) : -1;
			CHECK(terrainColour(f, 0.01, 0.0, family, kShippedMap, kShippedSecondAxis, engine)
			      == terrainColour(f, 0.01, 0.0, family));
		}
	}
}

TEST_CASE("resonate: the map's grain and glow are Resonate's own, spread like Stair's, and no other engine's moved") {
	// Resonate's Explore topography read dark and dull. The grain, glow and ember channels are
	// fixed gains on raw energy and roughness, calibrated on the shared Morph/Stair world; Resonate's rest
	// poses are dark and smooth, so through them its map had almost no grain. Its channels now read its
	// features quantile-matched onto the shared world (channelFeaturesFor). MEASURED over the calibration
	// sample, the median and interquartile spread of luminance and grain for Stair (the control), Resonate
	// before (as measured) and Resonate after.
	struct Spread {
		float median;
		float iqr;
	};
	const auto spreadOf = [](std::vector<float> v) {
		std::sort(v.begin(), v.end());
		const auto at = [&](double p) { return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))]; };
		return Spread { at(0.5), at(0.75) - at(0.25) };
	};
	const auto channelsOf = [&](OscType featuresOf, OscType channelsAs) {
		SliceOrientationPin pin;
		std::vector<float> lum;
		std::vector<float> grain;
		for (int zone = 0; zone < 1024; zone += 7) {
			for (int step = -8; step <= 8; ++step) {
				const TerrainChannels c = terrainChannels(
				    terrainFeaturesAt(featuresOf, pin, static_cast<uint16_t>(zone), 0.0f, step * 0.005, 0.0), channelsAs);
				lum.push_back(c.luminance);
				grain.push_back(c.grain);
			}
		}
		return std::make_pair(spreadOf(lum), spreadOf(grain));
	};
	const auto [stairLum, stairGrain] = channelsOf(OscType::Stair, OscType::Stair);
	const auto [beforeLum, beforeGrain] = channelsOf(OscType::Resonate, OscType::Morph); // as measured
	const auto [afterLum, afterGrain] = channelsOf(OscType::Resonate, OscType::Resonate);
	MESSAGE("luminance median/IQR — Stair " << stairLum.median << "/" << stairLum.iqr << ", Resonate before "
	                                         << beforeLum.median << "/" << beforeLum.iqr << ", after "
	                                         << afterLum.median << "/" << afterLum.iqr);
	MESSAGE("grain     median/IQR — Stair " << stairGrain.median << "/" << stairGrain.iqr << ", Resonate before "
	                                         << beforeGrain.median << "/" << beforeGrain.iqr << ", after "
	                                         << afterGrain.median << "/" << afterGrain.iqr);
	CHECK(afterGrain.iqr >= 0.5f * stairGrain.iqr);
	CHECK(afterGrain.median >= 0.5f * stairGrain.median);
	CHECK(afterLum.iqr >= 0.5f * stairLum.iqr);
	CHECK(afterGrain.iqr > 2.0f * beforeGrain.iqr);

	// AND THE MAP TAKES IT: a Resonate texel's colour is computed from the matched channels.
	const TerrainFeatures sample = terrainFeaturesAt(OscType::Resonate, SliceOrientationPin {}, 300, 0.0f, 0.02, 0.0);
	CHECK(terrainChannels(sample, OscType::Resonate).grain != terrainChannels(sample).grain);

	// NO OTHER ENGINE MOVED: their channels are computed from their features as measured, to the bit, and
	// every sampled texel's colour through the engine argument is the defaulted call's.
	for (const OscType engine : { OscType::Morph, OscType::Stair, OscType::V5 }) {
		CAPTURE(static_cast<int>(engine));
		for (int zone = 0; zone < 1024; zone += 31) {
			const TerrainFeatures f = terrainFeaturesAt(engine, SliceOrientationPin {}, static_cast<uint16_t>(zone),
			                                            0.0f, 0.01, 0.0);
			const TerrainChannels a = terrainChannels(f, engine);
			const TerrainChannels b = terrainChannels(f);
			CHECK(a.luminance == b.luminance);
			CHECK(a.hue == b.hue);
			CHECK(a.grain == b.grain);
			CHECK(a.ember == b.ember);
			const int family = engine == OscType::Stair ? stairFamilyAt(static_cast<uint16_t>(zone)) : -1;
			CHECK(terrainColour(f, 0.01, 0.0, family, kShippedMap, kShippedSecondAxis, engine)
			      == terrainColour(f, 0.01, 0.0, family));
		}
	}
}
TEST_CASE("the baked feature standardisation is not stale") {
	// The standardisation is an offline pass in artifact generation, so nobody has to remember to
	// rerun it by hand. This is the "nobody has to remember" half, and it is the whole mechanism: the
	// table is checked in beside its generator, and
	// this case recomputes it from the world and compares. Change a character bank, a slice constant or
	// the sampling frame and the suite goes red with a line telling you to re-bake — nobody has to
	// remember, and CI never enters into it.
	//
	// IT REGENERATES AT FULL FIDELITY rather than checking a hash, because it can: the shared frame is 8,192
	// character builds and Resonate's own 20,480 (its five models), and the whole case MEASURED 3.3-4.7 s on a
	// host at load ~37 (2026-09-26; the shared frame alone was about ninety milliseconds before Resonate's
	// rest-pose solves joined it), which is still cheap enough to do honestly. A
	// hash of the generator's inputs would have been the fallback if this were slow, and it would have
	// been weaker — it would notice a changed input but not a changed OUTPUT, which is the thing that
	// actually re-colours every map.
	const FeatureNorm fresh = measureFeatureNorm();
	INFO("re-bake with: ninja -C build bake-feature-norm && build/bake-feature-norm > "
	     "src/plugin/ui/feature_norm_baked.cpp");
	CHECK(fresh == kBakedFeatureNorm);
	// AND RESONATE'S OWN TABLE, over its own frame, by the same rule.
	CHECK(measureResonateFeatureNorm() == kBakedResonateFeatureNorm);
	CHECK(kBakedResonateFeatureNorm.centroid.front() < kBakedResonateFeatureNorm.centroid.back());
	CHECK(kBakedResonateFeatureNorm.parity.front() < kBakedResonateFeatureNorm.parity.back());
	// AND ADDITIVE'S (backlog LY, 2026-10-26): its own frame, its own table.
	CHECK(measureAdditiveFeatureNorm() == kBakedAdditiveFeatureNorm);
	CHECK(kBakedAdditiveFeatureNorm.energy.front() < kBakedAdditiveFeatureNorm.energy.back());
	CHECK(kBakedAdditiveFeatureNorm.centroid.front() < kBakedAdditiveFeatureNorm.centroid.back());
	// AND PLUCK'S (2026-10-31): its own frame through the string as heard, its own table, every channel spread.
	CHECK(measurePluckFeatureNorm() == kBakedPluckFeatureNorm);
	CHECK(kBakedPluckFeatureNorm.energy.front() < kBakedPluckFeatureNorm.energy.back());
	CHECK(kBakedPluckFeatureNorm.centroid.front() < kBakedPluckFeatureNorm.centroid.back());
	CHECK(kBakedPluckFeatureNorm.roughness.front() < kBakedPluckFeatureNorm.roughness.back());
	// AND THE DRUM'S (2026-11-05): its own frame through each kind as heard, pooled, its own table, every channel spread.
	CHECK(measureDrumFeatureNorm() == kBakedDrumFeatureNorm);
	CHECK(kBakedDrumFeatureNorm.energy.front() < kBakedDrumFeatureNorm.energy.back());
	CHECK(kBakedDrumFeatureNorm.centroid.front() < kBakedDrumFeatureNorm.centroid.back());
	CHECK(kBakedDrumFeatureNorm.roughness.front() < kBakedDrumFeatureNorm.roughness.back());

	// AND THE TABLE HAS TO BE A USABLE ONE, or "not stale" could be satisfied by two matching piles of
	// zeros — which is exactly what the placeholder committed before the first bake was.
	CHECK(kBakedFeatureNorm.energy.front() < kBakedFeatureNorm.energy.back());
	CHECK(kBakedFeatureNorm.centroid.front() < kBakedFeatureNorm.centroid.back());
	CHECK(kBakedFeatureNorm.parity.front() < kBakedFeatureNorm.parity.back());
	CHECK(kBakedFeatureNorm.fundamental.front() < kBakedFeatureNorm.fundamental.back());

	SUBCASE("and standardising actually flattens the distribution it was measured from") {
		// THE POINT OF THE PASS, as a property rather than a promise: the raw features are lumpy — half
		// the world's energy sits inside a band 0.007 wide — and a colour map fed that would spend its
		// range on values nothing takes. After standardising, each feature should be near-uniform on
		// [0,1], so every decile of the map is somewhere the world actually goes.
		std::array<int, 10> deciles {};
		int n = 0;
		forEachFrameSample([&](const TerrainFeatures& f) {
			const float u = standardize(kBakedFeatureNorm.energy, f.energy);
			deciles[static_cast<size_t>(std::clamp(static_cast<int>(u * 10.0f), 0, 9))]++;
			++n;
		});
		const int expected = n / 10;
		for (size_t d = 0; d < deciles.size(); ++d) {
			INFO("decile " << d << " holds " << deciles[d] << " of an expected " << expected);
			// Generous bounds on purpose: flat runs in a lumpy feature genuinely pile into one decile,
			// and demanding perfection here would be demanding the world be smooth. Half to double is
			// the difference between "the map is reachable everywhere" and "the map has dead regions".
			CHECK(deciles[d] > expected / 2);
			CHECK(deciles[d] < expected * 2);
		}
	}
}

TEST_CASE("MEASURE: the second-axis and vibe-map candidates, rest and turned" * doctest::skip()) {
	// THE PICTURE ROUND. Two things are chosen by eye from these: WHICH MEASUREMENT feeds the map's second
	// axis, and WHICH MAP reads the pair — the map being the vibe prior. Every tile is
	// rendered through the shipped standardisation, at the span the country can show itself in, rest
	// and turned by the same 2.1 radians.
	//
	//   FEATHERS_DUMP_DIR=/some/dir ./tests/test_gui_widgets -tc="MEASURE: the second-axis*" --no-skip
	const char* out = std::getenv("FEATHERS_DUMP_DIR");
	if (out == nullptr) {
		MESSAGE("set FEATHERS_DUMP_DIR to write the candidates");
		return;
	}
	struct Rung {
		const char* name;
		ColourMap map;
		SecondAxis axis;
	};
	const Rung rungs[] = {
		{ "axis-parity", ColourMap::Nebula, SecondAxis::Parity },
		{ "axis-fundamental", ColourMap::Nebula, SecondAxis::Fundamental },
		{ "axis-roughness", ColourMap::Nebula, SecondAxis::Roughness },
		{ "map-ember", ColourMap::Ember, SecondAxis::Parity },
		{ "map-tide", ColourMap::Tide, SecondAxis::Parity },
	};
	constexpr int kSide = TerrainMap::kResolution;
	const double span = 1023.0 / 1023.0;
	for (const Rung& rung : rungs) {
		for (const bool turned : { false, true }) {
			SliceOrientationPin pin;
			if (turned) {
				pin.pinned = true;
				pin.generator.rotate = 2.1f;
			}
			TerrainWindow window = windowAt(6.0, span, span);
			window.engine = OscType::Morph;
			window.zoneBaseline = 512.0;
			window.pin = pin;
			std::vector<unsigned char> rgb(static_cast<size_t>(kSide) * kSide * 3);
			for (int row = 0; row < kSide; ++row) {
				// **BOTH AXES ARE TRAVEL** (backlog GY). This instrument walked rows as HOME and derived a
				// plane per row from it — an independent restatement of the builder, and it restated the
				// defect too. Home is now the one place the whole plane is taken at; the row is travel-Y.
				const double travelY = TerrainMap::travelYAtRow(window, row);
				const double home = window.gammaBase;
				const auto frames = terrainFramesAt(pin, home, kNoPlaneResidue);
				const auto zone =
				    static_cast<uint16_t>(std::clamp(std::lround(home * 1023.0), 0L, 1023L));
				const double base = home - static_cast<double>(zone) / 1023.0;
				for (int column = 0; column < kSide; ++column) {
					const double travelX = TerrainMap::travelXAtColumn(window, column);
					const TerrainFeatures f =
					    terrainFeaturesOn(OscType::Morph, frames, zone, 0.0f, travelX, travelY, base);
					const uint32_t c = terrainColour(f, travelX, travelY, -1, rung.map, rung.axis);
					const size_t at = (static_cast<size_t>(row) * kSide + column) * 3;
					rgb[at] = static_cast<unsigned char>((c >> 16) & 0xFF);
					rgb[at + 1] = static_cast<unsigned char>((c >> 8) & 0xFF);
					rgb[at + 2] = static_cast<unsigned char>(c & 0xFF);
				}
			}
			const std::string path = std::string(out) + "/cand-" + rung.name
			                         + (turned ? "-turned.ppm" : "-rest.ppm");
			std::FILE* file = std::fopen(path.c_str(), "wb");
			REQUIRE(file != nullptr);
			std::fprintf(file, "P6\n%d %d\n255\n", kSide, kSide);
			std::fwrite(rgb.data(), 1, rgb.size(), file);
			std::fclose(file);
		}
	}
	// AND THE DIAGNOSTIC TILE the question earned: the joint distribution drawn ON the map square,
	// raw against residualised. It is the picture of why the sheet above looked one-dimensional — the
	// world walks a diagonal band through the raw pair and fills the square once the first axis's share
	// is subtracted. Drawn as the map itself, dimmed where nothing lands, so what you see is which
	// colours the world can actually reach.
	for (const bool residual : { false, true }) {
		constexpr int kGrid = 128;
		std::array<int, kGrid * kGrid> hits {};
		int worst = 1;
		forEachFrameSample([&](const TerrainFeatures& f) {
			const float x = standardize(kBakedFeatureNorm.centroid, f.centroid);
			const float y = residual ? residualParity(kBakedFeatureNorm, f.centroid, f.parity)
			                         : standardize(kBakedFeatureNorm.parity, f.parity);
			const int cx = std::clamp(static_cast<int>(x * kGrid), 0, kGrid - 1);
			const int cy = std::clamp(static_cast<int>(y * kGrid), 0, kGrid - 1);
			const size_t at = static_cast<size_t>(kGrid - 1 - cy) * kGrid + cx;
			hits[at]++;
			worst = std::max(worst, hits[at]);
		});
		std::vector<unsigned char> rgb(static_cast<size_t>(kGrid) * kGrid * 3);
		for (int y = 0; y < kGrid; ++y) {
			for (int x = 0; x < kGrid; ++x) {
				const size_t at = static_cast<size_t>(y) * kGrid + x;
				const MapRgb c = colourMapAt(ColourMap::Nebula, static_cast<float>(x) / kGrid,
				                             1.0f - static_cast<float>(y) / kGrid);
				// Unvisited cells go nearly black; visited ones show the map's own colour there.
				const float lit = hits[at] > 0 ? 0.35f + 0.65f * std::min(1.0f, static_cast<float>(hits[at])
				                                                                    / (0.25f * worst))
				                               : 0.06f;
				rgb[at * 3] = static_cast<unsigned char>(std::clamp(c.r * lit, 0.0f, 255.0f));
				rgb[at * 3 + 1] = static_cast<unsigned char>(std::clamp(c.g * lit, 0.0f, 255.0f));
				rgb[at * 3 + 2] = static_cast<unsigned char>(std::clamp(c.b * lit, 0.0f, 255.0f));
			}
		}
		const std::string path =
		    std::string(out) + (residual ? "/scatter-residual.ppm" : "/scatter-raw.ppm");
		std::FILE* file = std::fopen(path.c_str(), "wb");
		REQUIRE(file != nullptr);
		std::fprintf(file, "P6\n%d %d\n255\n", kGrid, kGrid);
		std::fwrite(rgb.data(), 1, rgb.size(), file);
		std::fclose(file);
	}
	MESSAGE("candidates written to " << std::string(out));
}

TEST_CASE("the second colour axis is independent of the first, and still means what it says") {
	// The map read as a 1-D colour map although it was
	// genuinely two-dimensional in code. The eye was reading the JOINT distribution: parity and
	// centroid correlate at −0.72, so the pair walked a diagonal band and two thirds of the square was
	// never visited. Standardising each axis fixes each MARGIN and does nothing about the pair.
	//
	// SO THE SECOND AXIS SHIPS AS A RESIDUAL, and this case is the pair of claims that buys: the square
	// gets filled, AND the axis still says what its name says.
	std::vector<std::pair<float, float>> raw;
	std::vector<std::pair<float, float>> shipped;
	forEachFrameSample([&](const TerrainFeatures& f) {
		const float tilt = standardize(kBakedFeatureNorm.centroid, f.centroid);
		raw.emplace_back(tilt, standardize(kBakedFeatureNorm.parity, f.parity));
		shipped.emplace_back(tilt, residualParity(kBakedFeatureNorm, f.centroid, f.parity));
	});

	const auto correlation = [](const std::vector<std::pair<float, float>>& pairs) {
		double mx = 0.0;
		double my = 0.0;
		for (const auto& [x, y] : pairs) {
			mx += x;
			my += y;
		}
		mx /= static_cast<double>(pairs.size());
		my /= static_cast<double>(pairs.size());
		double cov = 0.0;
		double vx = 0.0;
		double vy = 0.0;
		for (const auto& [x, y] : pairs) {
			cov += (x - mx) * (y - my);
			vx += (x - mx) * (x - mx);
			vy += (y - my) * (y - my);
		}
		return cov / std::sqrt(vx * vy);
	};

	// ONE: THE AXES COME APART. Raw, the pair is strongly correlated; residualised, an OLS residual is
	// uncorrelated with its regressor by construction, so this is really a check that the projection
	// was fitted on the population it is applied to — a stale coefficient would show up here.
	//
	// THE BAR IS 0.10 RATHER THAN ZERO, and the gap is explained rather than padded: an OLS residual is
	// exactly uncorrelated with its regressor, but the residual then goes through its OWN quantile
	// standardisation so it fills [0,1], and that step is monotone but not linear — which puts a little
	// correlation back. Measured 0.058. The bar catches a projection fitted to the wrong population
	// (which would read like the raw −0.72) without pretending the quantile pass is free.
	INFO("raw r = " << correlation(raw) << ", shipped r = " << correlation(shipped));
	CHECK(std::abs(correlation(raw)) > 0.5);
	CHECK(std::abs(correlation(shipped)) < 0.10);

	// TWO: THE SQUARE GETS FILLED, which is what the eye actually responds to — a low
	// correlation with an unvisited corner would still draw as a band. Count how many cells of an 8x8
	// grid over the map the world ever reaches.
	// A CELL COUNTS WHEN THE WORLD REALLY LIVES THERE, not when one sample of eight thousand grazes it.
	// A first version counted any-sample-at-all and reported 61 of 64 for the RAW pair — a band plainly
	// visible by eye scored 95% on that metric, which makes it a metric of nothing. A cell needs
	// at least a quarter of an even share before it is somewhere the map actually spends its colours.
	const auto occupancy = [](const std::vector<std::pair<float, float>>& pairs) {
		std::array<int, 64> hits {};
		for (const auto& [x, y] : pairs) {
			const int cx = std::clamp(static_cast<int>(x * 8.0f), 0, 7);
			const int cy = std::clamp(static_cast<int>(y * 8.0f), 0, 7);
			hits[static_cast<size_t>(cy) * 8 + cx]++;
		}
		const int share = static_cast<int>(pairs.size()) / 64 / 4;
		return std::count_if(hits.begin(), hits.end(), [&](int h) { return h >= share; });
	};
	const auto before = occupancy(raw);
	const auto after = occupancy(shipped);
	INFO("map cells the world really occupies: " << before << " of 64 raw, " << after
	                                             << " of 64 shipped");
	CHECK(after > before);

	// THREE: THE SENTENCE SURVIVES THE PROJECTION, which is the thing a residual can quietly cost. The
	// axis no longer says "odd-heavy" outright — it says ODD-HEAVY FOR ITS TILT — so within a band of
	// fixed tilt it must still track the raw odd/even balance, and strongly. Measured in centroid bins
	// because that is what "at fixed tilt" means.
	// EIGHT BINS RATHER THAN FOUR, because "at fixed tilt" should mean nearly fixed: a quarter-of-the-
	// range bin still lets tilt vary enough inside it for the projection to remove some of the very
	// signal being looked for, which showed up as 0.86 where the claim wants 0.9. Narrowing the bin
	// sharpens the question instead of lowering the bar for it.
	for (int bin = 0; bin < 8; ++bin) {
		std::vector<std::pair<float, float>> within;
		forEachFrameSample([&](const TerrainFeatures& f) {
			const float tilt = standardize(kBakedFeatureNorm.centroid, f.centroid);
			if (static_cast<int>(tilt * 8.0f) == bin) {
				within.emplace_back(f.parity, residualParity(kBakedFeatureNorm, f.centroid, f.parity));
			}
		});
		REQUIRE(within.size() > 100);
		INFO("tilt bin " << bin << ": r against raw parity = " << correlation(within) << " over "
		                 << within.size() << " samples");
		CHECK(correlation(within) > 0.9);
	}
}

TEST_CASE("MEASURE: the widest stop with its travel axis narrowed" * doctest::skip()) {
	// A PROBE, not a proposal: at the widest stop the chart sweeps travel as far as it sweeps home
	// (±24 of each), and travel MULTIPLIES every triangle's rate — so the sides of that map are the
	// character changing ten to a hundred times faster than a texel can show. This renders the same
	// home sweep with the travel axis held narrow, to see whether the graininess is the home axis
	// being undersampled or the travel axis destroying it.
	const char* out = std::getenv("FEATHERS_DUMP_DIR");
	if (out == nullptr) {
		return;
	}
	constexpr int kSide = TerrainMap::kResolution;
	for (const double travelSpan : { 48.0, 3.0, 0.25 }) {
		TerrainWindow window = windowAt(6.0, ExploreStage::kWidestSpanZones / 1023.0, travelSpan);
		window.engine = OscType::Morph;
		window.zoneBaseline = 512.0;
		std::vector<unsigned char> rgb(static_cast<size_t>(kSide) * kSide * 3);
		for (int row = 0; row < kSide; ++row) {
			const double home = TerrainMap::travelYAtRow(window, row);
			const double travelY = TerrainMap::travelYAtRow(window, row);
			const auto frames = terrainFramesAt(window.pin, home, kNoPlaneResidue);
			const auto zone = static_cast<uint16_t>(std::clamp(std::lround(home * 1023.0), 0L, 1023L));
			const double base = home - static_cast<double>(zone) / 1023.0;
			for (int column = 0; column < kSide; ++column) {
				const double travelX = TerrainMap::travelXAtColumn(window, column);
				const TerrainFeatures f =
				    terrainFeaturesOn(OscType::Morph, frames, zone, 0.0f, travelX, travelY, base);
				const uint32_t c = terrainColour(f, travelX, travelY, -1);
				const size_t at = (static_cast<size_t>(row) * kSide + column) * 3;
				rgb[at] = static_cast<unsigned char>((c >> 16) & 0xFF);
				rgb[at + 1] = static_cast<unsigned char>((c >> 8) & 0xFF);
				rgb[at + 2] = static_cast<unsigned char>(c & 0xFF);
			}
		}
		char name[128];
		std::snprintf(name, sizeof(name), "%s/widestop-travel%g.ppm", out, travelSpan);
		std::FILE* file = std::fopen(name, "wb");
		REQUIRE(file != nullptr);
		std::fprintf(file, "P6\n%d %d\n255\n", kSide, kSide);
		std::fwrite(rgb.data(), 1, rgb.size(), file);
		std::fclose(file);
	}
	MESSAGE("written to " << std::string(out));
}

TEST_CASE("MEASURE: how hard V5 leans on the interim morph knots — the V10c saturation report") {
	// Decision V10c: the flagship's terrain features normalise through the EXISTING knots this cut,
	// and if operated territory saturates the map that is evidence FOR the scale-split round —
	// reported here, never refit away. standardize() clamps at the end knots, so the measurable
	// symptom is features landing exactly at 0.0 or 1.0; the fractions below put a number on it,
	// beside Morph's own fractions from the same positions so the delta is the flagship's and not
	// the frame's.
	const SliceOrientationPin unpinned {};
	int v5Clamped = 0;
	int morphClamped = 0;
	int operated = 0;
	int total = 0;
	auto clampedEnds = [](const NormKnots& knots, float raw) {
		const float standard = standardize(knots, raw);
		return standard == 0.0f || standard == 1.0f;
	};
	for (int i = 0; i < 400; ++i) {
		const auto zone = static_cast<uint16_t>((i * 61) % 1024);
		const float offset = static_cast<float>(i) * 0.173f;
		const double travel = (i % 5) * 0.2 - 0.4;
		const TerrainFeatures v5 =
		    terrainFeaturesAt(OscType::V5, unpinned, zone, offset, travel, 0.0);
		const TerrainFeatures morph =
		    terrainFeaturesAt(OscType::Morph, unpinned, zone, offset, travel, 0.0);
		const int v5Hit = clampedEnds(kBakedFeatureNorm.energy, v5.energy)
		                  + clampedEnds(kBakedFeatureNorm.centroid, v5.centroid)
		                  + clampedEnds(kBakedFeatureNorm.roughness, v5.roughness)
		                  + clampedEnds(kBakedFeatureNorm.parity, v5.parity);
		const int morphHit = clampedEnds(kBakedFeatureNorm.energy, morph.energy)
		                     + clampedEnds(kBakedFeatureNorm.centroid, morph.centroid)
		                     + clampedEnds(kBakedFeatureNorm.roughness, morph.roughness)
		                     + clampedEnds(kBakedFeatureNorm.parity, morph.parity);
		v5Clamped += v5Hit;
		morphClamped += morphHit;
		operated += std::memcmp(&v5, &morph, sizeof(TerrainFeatures)) != 0;
		total += 4;
	}
	std::printf("[flagship knots] V5 end-clamped %.1f%% vs Morph %.1f%% over %d feature reads; "
	            "%d/400 positions operated\n",
	            100.0 * v5Clamped / total, 100.0 * morphClamped / total, total, operated);
	// Sanity only: the flagship must actually be operating somewhere in the sample, or the numbers
	// above are a comparison of identical pictures.
	CHECK(operated > 100);
}

TEST_CASE("the terrain follows a zoom once the zoom rests, or once it has moved far — a notch is a quarter of the old step") {
	// The request (2026-09-17): "lets just add scaling 1/4 of the steps, redrawing only on dwell", and "also redraw if
	// the zoom goes some minimum amount of displacement even on not dwell". Each notch used to re-sample a new grid and
	// pop. Asserted on the zoom the map is BUILT at (`terrainZoom`) and on the window the builder was asked for, with the
	// fixture's clock wound by hand.
	ExploreFixture f;
	visage::Canvas canvas;
	const visage::Point centre = f.stage.disk().centre();
	const auto wheel = [&](float dy) {
		visage::MouseEvent e = at(centre.x, centre.y);
		e.wheel_delta_y = dy;
		e.precise_wheel_delta_y = dy;
		return f.stage.mouseWheel(e);
	};
	f.stage.draw(canvas);
	REQUIRE(f.stage.terrainZoom() == doctest::Approx(1.0));
	CHECK(ExploreStage::kZoomStep * ExploreStage::kZoomStep * ExploreStage::kZoomStep * ExploreStage::kZoomStep
	      == doctest::Approx(1.6));

	SUBCASE("a notch scales the view at once and the map only when the zoom has rested") {
		REQUIRE(wheel(1.0f));
		f.stage.draw(canvas);
		CHECK(f.stage.zoom() == doctest::Approx(ExploreStage::kZoomStep));
		CHECK(f.stage.terrainZoom() == doctest::Approx(1.0)); // not yet
		f.now += ExploreStage::kZoomDwellMs - 1;
		f.stage.draw(canvas);
		CHECK(f.stage.terrainZoom() == doctest::Approx(1.0)); // still moving, by the clock
		f.now += 1;
		f.stage.draw(canvas);
		CHECK(f.stage.terrainZoom() == doctest::Approx(ExploreStage::kZoomStep)); // rested: the map follows
	}
	SUBCASE("a run of notches that goes far enough refreshes the map without resting") {
		int notches = 0;
		while (f.stage.terrainZoom() == doctest::Approx(1.0) && notches < 20) {
			REQUIRE(wheel(1.0f));
			f.stage.draw(canvas); // no time passes: a scroll with no rest in it
			++notches;
		}
		CAPTURE(notches);
		CHECK(f.stage.zoom() >= ExploreStage::kZoomRebuildRatio);
		CHECK(f.stage.terrainZoom() == doctest::Approx(f.stage.zoom()));
		CHECK(notches == 3); // √2 at 1.6^(1/4) a notch: the third notch crosses it
	}
}

TEST_CASE("backlog FE: the terrain is lit as a planet — the satellite is the selected star, in disk radii, and its light is the sound's colour") {
	// The design: the terrain looks like a planet, the puck a very bright satellite in low orbit at 5% of the
	// disk's radius, white but slightly tinted by the sound's colour, over a fairly dim ambience. WHAT THE SHADER DOES
	// WITH THESE IS A PHOTOGRAPH (a capture through guihost, FEATHERS_PLANET_STAR to place the star); what this pins is
	// the half a case can read: the numbers the draw hands it, restated here from the disk and the star.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas); // the view centres on the star
	// THEN THE STAR MOVES AWAY FROM IT, or its x is zero and a draw that lost it would look right.
	f.bridge.setSoundTravel(1, { 0.35, 0.0 });
	f.bridge.refresh();
	f.stage.draw(canvas);
	// THE LIGHT IS PART OF DRAWING THE GROUND, so there has to be some: no map, no planet.
	for (int waited = 0; waited < 8000 && f.terrain.snapshot()->rows < TerrainMap::kResolution; ++waited) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(f.terrain.snapshot()->rows == TerrainMap::kResolution);
	f.stage.draw(canvas);

	const auto& light = f.stage.planetShadeForTest().uniforms();
	REQUIRE(light.count("u_planet_light") == 1u);
	REQUIRE(light.count("u_planet_tint") == 1u);
	const auto& shade = light.at("u_planet_light");
	const auto d = f.stage.disk();
	const float radius = std::max(1.0f, d.radius());
	// WHERE THE SATELLITE IS DRAWN. The shader lights a sphere in screen coordinates, so what it must be
	// handed is the star's PROJECTED position — restated here through `Bubble` rather than read off the
	// stage. It matters most exactly where the sheet reaches furthest: this star stands 11 disc radii
	// out on the flat plane and is seen on the limb, and a light placed at eleven radii is a light that
	// is nowhere.
	const visage::Point drawn =
	    Bubble::toGlass(d, { f.stage.xAtTravelX(f.stage.selectedDrift()),
		                     f.stage.yAtTravelY(f.stage.selectedDriftY()) });
	const float starX = (drawn.x - d.centre().x) / radius;
	const float starY = (drawn.y - d.centre().y) / radius;
	CHECK(shade[0] == doctest::Approx(starX));
	CHECK(shade[1] == doctest::Approx(starY));
	REQUIRE(std::abs(starX) > 0.05f); // not vacuous: a star at the centre has nothing to lose
	// THE TERMINATOR'S SOFTNESS, which is what the third number became when the lamp turned into a distant light (the
	// star is the point through which a ray normal to the planet surface towards the true light
	// passes). Pinned as a softness rather than a value: a hard zero cuts the terminator as a line, and past one there
	// is no terminator left.
	CHECK(shade[2] > 0.0f);
	CHECK(shade[2] < 1.0f);
	CHECK(shade[3] > 0.0f);                   // an ambience, and a dim one
	CHECK(shade[3] < 0.5f);

	// THE LIGHT IS WHITE, LEANING TOWARDS THE SOUND'S OWN COLOUR — restated from the hue the star is drawn in.
	const visage::Color hue(ui::kSeedHues[feathers::hueIndexOf(f.bridge.soundHues(), 1)]);
	const auto& tint = light.at("u_planet_tint");
	const auto leans = [](float channel, float toward) {
		return channel == doctest::Approx(1.0f + 0.25f * (toward - 1.0f)).epsilon(0.001);
	};
	CHECK(leans(tint[0], hue.red()));
	CHECK(leans(tint[1], hue.green()));
	CHECK(leans(tint[2], hue.blue()));
	CHECK(tint[0] != doctest::Approx(tint[2])); // not vacuous: the hue is not grey
	// AND THE ATMOSPHERE IS HANDED THE SAME LIGHT, so the glow cannot lean one way while the ground leans another.
	const auto& air = f.stage.planetAirForTest().uniforms();
	REQUIRE(air.count("u_planet_air") == 1u);
	CHECK(air.at("u_planet_light")[0] == doctest::Approx(shade[0]));
	CHECK(air.at("u_planet_air")[2] > 1.0f); // the glow's quad reaches past the limb
}

TEST_CASE("backlog FE: a pan lands on ground the terrain built before it was asked — the prefetched map is carried under the new window") {
	// A low-priority pre-emptive render makes panning smoother. A pan asks
	// for a window the last map only partly covers, and the rest used to arrive row by row — the edge that wipes in.
	// When nothing is waiting, the terrain builds the same country WIDER (`kPrefetchSpread`) and keeps it; the next
	// window carries from it wherever the map being replaced had nothing.
	//
	// ASSERTED ON THE FIRST SNAPSHOT AFTER THE PAN, before a row of the new build can have landed: how much of the
	// newly exposed strip is already ground. Against the same pan with the prefetch never consulted, which is what the
	// known-bad does.
	TerrainMap terrain;
	TerrainWindow window;
	window.engine = feathers::OscType::Morph;
	window.travelYCentre = 0.4;
	window.travelYSpan = 0.02;
	window.travelXCentre = 0.0;
	window.travelXSpan = 0.02;
	terrain.request(window);
	for (int waited = 0; waited < 20000 && terrain.prefetches() == 0; ++waited) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(terrain.prefetches() >= 1u); // the wider map exists, which is what the pan below leans on

	// A PAN OF NINE TENTHS OF THE VIEW: only a tenth of it is country the last window covered, so all but that tenth
	// is the prefetch's to answer for. A smaller pan proves nothing — the ordinary carry covers it, which is what a
	// first draft of this case measured and why it stayed green with the prefetch switched off.
	TerrainWindow panned = window;
	panned.travelYCentre = window.travelYCentre + window.travelYSpan * 0.9;
	terrain.request(panned);
	std::shared_ptr<const TerrainSnapshot> first;
	for (int waited = 0; waited < 20000; ++waited) {
		first = terrain.snapshot();
		if (first->window.travelYCentre == doctest::Approx(panned.travelYCentre)) {
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	REQUIRE(first);
	REQUIRE(first->window.travelYCentre == doctest::Approx(panned.travelYCentre));

	// WHAT THAT FIRST SNAPSHOT HOLDS, over the rows the new build has NOT computed yet: ground, or void. This is about
	// what was carried into it, not about how fast the builder is.
	int ground = 0;
	int counted = 0;
	const int newRows = std::max(0, first->rows);
	for (int row = newRows; row < TerrainMap::kResolution; ++row) {
		for (int column = 0; column < TerrainMap::kResolution; ++column) {
			++counted;
			ground += first->texel(column, row) != 0u ? 1 : 0;
		}
	}
	CAPTURE(newRows);
	CAPTURE(counted);
	CAPTURE(ground);
	REQUIRE(counted > 0);
	// A NINE-TENTHS PAN WALKS HALF WAY OUT of a map built 1.8 spans wide, so about half of the new window is ground the
	// prefetch answers for and the old map's own overlap is a tenth. The bar sits between the two.
	CHECK(ground * 10 > counted * 3);
}

TEST_CASE("a fast drag cannot carry the world faster than the terrain can answer for — the star follows at a bounded speed") {
	// The pan rate is throttled so unrendered terrain cannot come into the viewport. THE CLAIM IS A SPEED,
	// so the case measures one: how far the world travelled, in views,
	// divided by how long the hand took, against `kViewsPerSecond`. The fixture's clock is wound by hand, so the drag's
	// duration is a number this case chooses rather than a race.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(0);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	const double span = f.stage.homeSpanZones() / 1023.0;
	const double startY = f.stage.selectedDriftY();

	// A FLING: the pointer crosses most of the chart in four frames, which is what a hand does when it throws the view.
	f.stage.onMouseDown().callback(at(c.x, c.y));
	constexpr int kFrames = 4;
	constexpr int kMsPerFrame = 16;
	for (int i = 1; i <= kFrames; ++i) {
		f.now += kMsPerFrame;
		f.stage.onMouseDrag().callback(at(c.x, c.y - 60.0f * static_cast<float>(i)));
		f.stage.update();
		f.stage.draw(canvas);
	}
	const double travelled = std::abs(f.stage.selectedDriftY() - startY) / span; // in views
	const double seconds = static_cast<double>(kFrames * kMsPerFrame) / 1000.0;
	CAPTURE(travelled);
	CAPTURE(seconds);
	// THE CAP, WITH THE SLACK THE EVENT AND THE FRAME EACH GET: a drag event and the frame after it may each take one
	// step, so the bound is two steps' worth per frame rather than one.
	CHECK(travelled <= 2.0 * ExploreStage::kViewsPerSecond * seconds + 1e-9);
	// AND IT DID MOVE: a throttle that stopped the chart would pass the line above and be useless.
	CHECK(travelled > 0.02);

	// AND A SLOW DRAG IS NOT THROTTLED AT ALL — the star lands exactly where the pointer is, which is what pressing on
	// the chart means. Same distance, over long enough that the cap cannot bite.
	ExploreFixture slow;
	slow.selection.select(0);
	slow.stage.draw(canvas);
	const visage::Point sc = slow.stage.disk().centre();
	slow.stage.onMouseDown().callback(at(sc.x, sc.y));
	slow.now += 600;
	slow.stage.onMouseDrag().callback(at(sc.x, sc.y - 60.0f));
	const double wanted =
	    slow.stage.travelYAtScreenY(Bubble::toFlat(slow.stage.disk(), { sc.x, sc.y - 60.0f }).y);
	CHECK(slow.stage.selectedDriftY() == doctest::Approx(wanted));
}

//---------------------------------------------------------------------------------------------//
// THE BUBBLE (backlog FE): the chart as the near face of a sphere                              //
//---------------------------------------------------------------------------------------------//

TEST_CASE("the bubble's radial law is a sphere seen from far away, and its two directions are inverse") {
	// THE LAW ITSELF, ASSERTED AS ARITHMETIC RATHER THAN THROUGH A WIDGET, because everything else in
	// this section is this function wearing a costume — the star's position, the pointer's meaning, the
	// grid's bow and the shader's sampling are one map applied at four call sites. Get it wrong here
	// and every case below is wrong in the same direction, which is exactly the shape that makes a
	// suite agree with itself and with nothing else.
	//
	// The reference is restated from the geometry rather than from `Bubble`: a surface point an angle θ
	// from the pole is SEEN at radius sin θ and IS θ of arc from the pole, both normalised to the limb
	// at θ = π/2.
	for (int i = 0; i <= 100; ++i) {
		const double world = static_cast<double>(i) / 100.0;
		const double theta = world * (M_PI / 2.0);
		CAPTURE(world);
		CHECK(Bubble::glassRadius(world) == doctest::Approx(std::sin(theta)));
		// And back, to the place it started: the map is a bijection on [0,1] and the code must not have
		// approximated one direction of it.
		CHECK(Bubble::worldRadius(Bubble::glassRadius(world)) == doctest::Approx(world));
	}

	// THE MIDDLE IS MAGNIFIED AND THE LIMB IS CRUSHED, which is what foreshortening IS and is the cost
	// accepted with this projection, together with a slower drag rate.
	// Asserted as the gain at each end: π/2 at the pole, nothing at all at the limb.
	const double tiny = 1e-4;
	CHECK(Bubble::glassRadius(tiny) / tiny == doctest::Approx(M_PI / 2.0).epsilon(1e-6));
	const double atLimb = (Bubble::glassRadius(1.0) - Bubble::glassRadius(1.0 - tiny)) / tiny;
	INFO("gain at the limb: " << atLimb);
	CHECK(atLimb < 0.01);

	// AND PAST THE RIM THE SHEET IS FLAT, which is the model taken literally: a bubble IN a sheet, so
	// the identity outside it, meeting the lens at the rim where sin(π/2) = 1. It does NOT fold back —
	// `sin` continued past a quarter turn would bring the next country onto the near face, mirrored —
	// and it does not saturate either.
	//
	// SATURATION IS WHAT THIS FIRST SHIPPED AS, and it is the case's reason for existing. Clamping is
	// right for the GROUND, and the ground keeps it: the shader samples only inside the disc. But the
	// same map answers what a POINTER means, and a drag outside the disc is how you cross a lot of
	// country — so the clamp quietly capped how far a pan could reach, which was noticeable at
	// once (click-to-pan did not go far enough).
	CHECK(Bubble::glassRadius(1.5) == doctest::Approx(1.5));
	CHECK(Bubble::glassRadius(40.0) == doctest::Approx(40.0));
	CHECK(Bubble::worldRadius(1.5) == doctest::Approx(1.5));
	// Continuous where the two halves meet, or the hand feels a step exactly at the rim.
	CHECK(Bubble::glassRadius(1.0 - 1e-9) == doctest::Approx(1.0));
	CHECK(Bubble::worldRadius(1.0 + 1e-9) == doctest::Approx(1.0));
	// And still a round trip out there, which the clamp could not be.
	CHECK(Bubble::worldRadius(Bubble::glassRadius(3.0)) == doctest::Approx(3.0));
	// The middle is its own image, in both directions, and is the one point with no bearing to keep.
	CHECK(Bubble::glassRadius(0.0) == doctest::Approx(0.0));
	CHECK(Bubble::worldRadius(0.0) == doctest::Approx(0.0));
}

TEST_CASE("the bubble is radial, so a diagonal press means a place neither axis could have named") {
	// WHY THE FLAT PAIR COULD NOT SIMPLY BE BENT, and the reason `travelYAtScreenY`/`travelXAtScreenX` stayed flat with
	// one point-shaped pair added beside them: a radial map is NOT separable. Where a pixel's height
	// lands depends on how far across the chart it is, so an axis-at-a-time answer is wrong everywhere
	// except on the two centre lines — and wrong in a way that looks plausible, because it is right in
	// the two places a test is most likely to press.
	//
	// KNOWN-BAD: read the pointer's home as `travelYAtScreenY(y)` instead of flattening the point first and this
	// case fails while a straight-up press still passes.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const StageDisk d = f.stage.disk();
	const visage::Point c = d.centre();

	// Well off both centre lines, and well out towards the rim where the map has the most to say.
	const float dx = d.radius() * 0.5f;
	const float dy = d.radius() * 0.5f;
	const visage::Point press { c.x + dx, c.y - dy };
	f.stage.onMouseDown().callback(at(press.x, press.y));
	f.settleGesture();

	const visage::Point flat = Bubble::toFlat(d, press);
	CHECK(f.stage.selectedDriftY() == doctest::Approx(f.stage.travelYAtScreenY(flat.y)).epsilon(0.002));
	CHECK(f.stage.selectedDrift() == doctest::Approx(f.stage.travelXAtScreenX(flat.x)).epsilon(0.002));

	// NOT VACUOUS, and this is the assertion that makes the case worth its lines: the separable answer
	// is a DIFFERENT number here, by more than the tolerance above, so the two readings can be told
	// apart. On the centre lines they agree exactly, which is why a press up the middle proves nothing.
	const double separable = f.stage.travelYAtScreenY(press.y);
	INFO("radial " << f.stage.selectedHome() << " against separable " << separable);
	CHECK(std::abs(f.stage.selectedHome() - separable) > 0.004);

	// AND THE STAR IS DRAWN BACK UNDER THE FINGER. Both directions of the map, at a point where both
	// coordinates are in play.
	const visage::Point drawn = f.stage.glassAt(f.stage.selectedDriftY(), f.stage.selectedDrift());
	CHECK(drawn.x == doctest::Approx(press.x).epsilon(0.005));
	CHECK(drawn.y == doctest::Approx(press.y).epsilon(0.005));
}

TEST_CASE("a zone line is drawn as a curve that bows away from the middle, and ends on the rim") {
	// THE STRONGEST CUE THE SURFACE HAS. A lit circle with straight lines across it is a lit circle; the
	// same circle with its latitudes bowed is a ball. This asserts the bow as geometry — the curve's
	// own middle against the chord between its ends — rather than by looking at it.
	ExploreFixture f;
	const StageDisk d = f.stage.disk();
	const visage::Point c = d.centre();
	const float radius = d.radius();

	// A line half way up the chart, drawn the way `drawGrid` draws one: from the sheet's own chord at
	// that height, both ends on the world's edge.
	const float dy = -radius * 0.5f;
	const float half = std::sqrt(radius * radius - dy * dy);
	const visage::Point left = Bubble::toGlass(d, { c.x - half, c.y + dy });
	const visage::Point right = Bubble::toGlass(d, { c.x + half, c.y + dy });
	const visage::Point middle = Bubble::toGlass(d, { c.x, c.y + dy });

	// BOTH ENDS ARE ON THE RIM, because the chord's ends are on the world's edge and that is where the
	// limb is. So the curve spans the disc exactly as the straight line did — the bow is not the line
	// getting shorter.
	CHECK(std::hypot(left.x - c.x, left.y - c.y) == doctest::Approx(radius).epsilon(0.001));
	CHECK(std::hypot(right.x - c.x, right.y - c.y) == doctest::Approx(radius).epsilon(0.001));

	// AND IT BOWS AWAY FROM THE MIDDLE OF THE DISC — upward here, since this line is above centre. The
	// chord's midpoint against the curve's own: a straight line has zero sag and this must not.
	const float chordY = (left.y + right.y) * 0.5f;
	const float sag = chordY - middle.y;
	INFO("curve middle at y=" << middle.y << " against the chord's " << chordY);
	CHECK(sag > 2.0f);
	// The sign is the claim, not just the size: bowing the other way would draw a sphere seen from
	// inside, which is a different (and wrong) picture that a magnitude-only check would accept.
	CHECK(middle.y < chordY);

	// AND THE LINE THROUGH THE MIDDLE STAYS STRAIGHT, which is the same law saying something else: a
	// diameter is a great circle seen edge-on, so it is the one latitude with no bow at all.
	const visage::Point mid = Bubble::toGlass(d, { c.x, c.y });
	CHECK(mid.x == doctest::Approx(c.x));
	CHECK(mid.y == doctest::Approx(c.y));

	// AND THE GRID ACTUALLY USES IT. Everything above is the law; this is the surface obeying it, and
	// they are separate claims — a `drawGrid` that went on drawing straight fills would pass every
	// assertion in this case up to here. `gridBowForTest` is the deepest bow the last draw put on the
	// glass, which is zero for a straight grid and cannot be faked by anything else moving.
	//
	// KNOWN-BAD: draw the zone lines as the flat fills they used to be and this reads 0.
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	INFO("deepest zone-line bow: " << f.stage.gridBowForTest() << "px");
	CHECK(f.stage.gridBowForTest() > 2.0f);
}

TEST_CASE("the ground is a frame of its own, and it is the only thing the bubble bends") {
	// THE WIRING, WHICH IS WHERE THIS DESIGN CAN GO WRONG SILENTLY. The warp is a post effect, and a
	// post effect applies to everything its frame paints — so the terrain has to be alone on a surface
	// nobody else draws on. Two halves, and both are assertions about the OBJECTS rather than about
	// pixels, because a headless canvas has no shader to run:
	//
	// 1. the ground frame carries the bubble effect;
	// 2. a stage that has been given a ground does not paint the picture itself, or the flat copy would
	//    sit under the warped one and show through wherever the warp moved it.
	TerrainGround ground;
	CHECK(ground.postEffect() != nullptr);
	CHECK(std::string(ground.bubbleForTest().fragmentShader().name)
	      == std::string(resources::shaders::fs_bubble.name));

	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	// Before: no ground, so the stage paints it — which is what every other case in this file relies
	// on, and what a venue with no frame tree gets.
	const int before = f.stage.groundPaintsForTest();
	f.stage.draw(canvas);
	CHECK(f.stage.groundPaintsForTest() == before + 1);

	// After: the stage has a ground, and hands the job over completely.
	ground.setStage(&f.stage);
	f.stage.setGround(&ground);
	const int handedOver = f.stage.groundPaintsForTest();
	f.stage.draw(canvas);
	CHECK(f.stage.groundPaintsForTest() == handedOver);

	// AND THE GROUND DOES THE JOB. Half a handover is a chart with no terrain on it at all, which is
	// the failure this half catches and the one above cannot.
	ground.setBounds(0.0f, 0.0f, f.stage.width(), f.stage.height());
	ground.onDraw().callback(canvas);
	CHECK(f.stage.groundPaintsForTest() == handedOver + 1);

	// AND THE WARP IS TOLD WHERE THE CHART IS. The frame is the whole stage rectangle and the chart is
	// a circle inside it, so the shader has no way to find the disc on its own; handed the wrong share
	// it bends the ground about an ellipse the size of the stage, and the rim mask covers that so
	// completely that no capture can see the difference. The reference is restated from the geometry.
	const float expected = f.stage.disk().radius() / f.stage.width();
	INFO("disc radius " << f.stage.disk().radius() << " of width " << f.stage.width());
	CHECK(f.stage.bubbleRadiusShareForTest() == doctest::Approx(expected));
	// NOT VACUOUS: the share is a real fraction of the frame, not zero and not the half that a square
	// frame would give — the stage is wider than it is tall, so the two readings differ.
	CHECK(f.stage.bubbleRadiusShareForTest() > 0.05f);
	CHECK(f.stage.bubbleRadiusShareForTest() < 0.45f);
	// And it reached the warp's own frame, not just the stage's arithmetic.
	CHECK(ground.shareForTest() == doctest::Approx(expected));
	// And the two are the same rectangle, or they do not share coordinates and every place on the
	// chart is drawn in one frame and addressed in the other.
	CHECK(ground.width() == f.stage.width());
	CHECK(ground.height() == f.stage.height());
}

TEST_CASE("a drag out past the rim keeps reaching, because outside the bubble the sheet is flat") {
	// THE REACH THE FIRST BUBBLE TOOK AWAY, noticeable within minutes: click-to-pan did not go
	// far enough. Dragging the pointer outside the disc is how you cross a lot of
	// country — the flat map extrapolates past the window on purpose — and a radial map that SATURATES
	// at the limb turns every such drag into a request for the rim and nothing further.
	//
	// Two identical gestures, differing only in how far outside the circle the hand goes. The one that
	// leaves the disc must ask for substantially more world; under the clamp the two are the same
	// number, which is the known-bad this case exists for.
	const auto reachedAfterDragTo = [](float pastCentre) {
		ExploreFixture f;
		visage::Canvas canvas;
		f.selection.select(1);
		f.stage.draw(canvas);
		const visage::Point c = f.stage.disk().centre();
		f.stage.onMouseDown().callback(at(c.x, c.y));
		f.now += 16;
		f.stage.onMouseDrag().callback(at(c.x + pastCentre, c.y));
		f.settleGesture();
		f.stage.onMouseUp().callback(at(c.x + pastCentre, c.y));
		f.settleGesture();
		f.settleScrub();
		return f.stage.selectedDrift();
	};

	ExploreFixture measure;
	const float radius = measure.stage.disk().radius();
	const double justInside = reachedAfterDragTo(radius * 0.97f);
	const double wellOutside = reachedAfterDragTo(radius * 3.0f);
	INFO("just inside the rim: " << justInside << ", three radii out: " << wellOutside);

	// The rim is the world's edge, so a drag that stops just short of it has nearly all of the near
	// face — and going three times further must be worth a great deal more than "nearly all of it".
	CHECK(wellOutside > justInside * 2.0);
	// NOT VACUOUS FROM THE OTHER END: the inside drag did travel, so the comparison is between two
	// real reaches rather than against a chart that never moved.
	CHECK(justInside > 0.0);
}

TEST_CASE("a released drag arrives where the hand asked, rather than stopping where the clock left it") {
	// THE OTHER HALF OF "NOT FAR ENOUGH", and it is the rate cap's own doing: a quick drag is still
	// travelling when the button comes up, and closing the gesture at the release threw away whatever
	// had not yet been handed over. The sound stopped short of the pointer, and the crumb and the undo
	// entry recorded the place it stopped at.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();
	const visage::Point to { c.x, c.y - f.stage.disk().radius() * 0.6f };

	f.stage.onMouseDown().callback(at(c.x, c.y));
	// One frame's worth of time for most of a view: far more than `kViewsPerSecond` allows, which is
	// what makes this a capped drag rather than an ordinary one.
	f.now += 16;
	f.stage.onMouseDrag().callback(at(to.x, to.y));
	f.settleGesture();
	const double wanted = f.stage.travelYAtScreenY(Bubble::toFlat(f.stage.disk(), to).y);

	// NOT VACUOUS: the cap really is holding it back at the moment of release, so there is something
	// left to arrive. Without this the case would pass on a build with no cap at all.
	INFO("at the release the sound is at " << f.stage.selectedHome() << " of " << wanted);
	REQUIRE(f.stage.selectedHome() != doctest::Approx(wanted));

	f.stage.onMouseUp().callback(at(to.x, to.y));
	f.settleGesture();
	// The release does not end the gesture; the arrival does.
	CHECK(f.stage.settlingScrubForTest());
	const int coasted = f.settleScrub();
	INFO("the released drag coasted for " << coasted << " frames");
	CHECK(coasted > 1);
	CHECK_FALSE(f.stage.settlingScrubForTest());
	CHECK(f.stage.selectedDriftY() == doctest::Approx(wanted).epsilon(0.002));

	// AND IT IS STILL ONE EDIT. The coast is the same gesture continuing, so it leaves one crumb and
	// one undo step — not a second one for the part that arrived after the button came up.
	CHECK(f.stage.crumbs(1).size() == 1);
	CHECK(f.stage.canUndo());
}

TEST_CASE("a press and hold leaves the sound where it was pressed, whatever the last drag wanted") {
	// THE THIRD SHAPE OF THE SAME REPORT, and the one that moves a chart under a STILL HAND. `update()`
	// walks the star toward `dragWanted*` on every frame the chart is held; a press that never set them
	// inherits the last gesture's target and quietly drags the sound back toward it.
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.stage.draw(canvas);
	const visage::Point c = f.stage.disk().centre();

	// A first gesture that ends somewhere definite, and well away from where the second press lands.
	const visage::Point far { c.x, c.y - f.stage.disk().radius() * 0.6f };
	f.stage.onMouseDown().callback(at(c.x, c.y));
	f.now += 16;
	f.stage.onMouseDrag().callback(at(far.x, far.y));
	f.settleGesture();
	f.stage.onMouseUp().callback(at(far.x, far.y));
	f.settleGesture();
	f.settleScrub();
	// **THE PROBE IS THE Y TRAVEL, NOT THE HOME** (backlog GY). It was `selectedHome()`, and when the
	// vertical axis stopped being gamma this case's own NON-VACUITY GUARD is what caught it: home no
	// longer moves for either gesture, so "there IS a stale target somewhere else" stopped being true
	// and the REQUIRE below fired with both sides reading the same untouched home. The guard did the
	// job it was written for — this is the probe following the axis, not the expectation following the
	// code.
	const double firstEnded = f.stage.selectedDriftY();

	// Now press somewhere else and HOLD, without moving the pointer at all.
	const visage::Point again { c.x + 40.0f, c.y + 30.0f };
	f.stage.onMouseDown().callback(at(again.x, again.y));
	f.settleGesture();
	const double atPress = f.stage.selectedDriftY();
	const double driftAtPress = f.stage.selectedDrift();
	// NOT VACUOUS: there IS a stale target to be pulled toward, and it is somewhere else. On a build
	// where the two happened to coincide this case could not fail.
	INFO("the previous gesture ended at " << firstEnded << ", this press is at " << atPress);
	REQUIRE(firstEnded != doctest::Approx(atPress));

	for (int frame = 0; frame < 20; ++frame) {
		f.now += 16;
		f.stage.update();
		f.settleGesture();
	}
	// Twenty frames is a third of a second, and the cap would have carried it a long way in that time.
	CHECK(f.stage.selectedDriftY() == doctest::Approx(atPress));
	CHECK(f.stage.selectedDrift() == doctest::Approx(driftAtPress));
}

TEST_CASE("MEASURE: how much of the chart has no map on it during a pan drag"
          * doctest::skip()) {
	// The symptom: undrawn canvas still entered the viewport during a drag.
	//
	// THE CASE ABOVE THIS ONE MEASURES THE WRONG NOUN, and that is why this exists. "the map is drawn ON
	// the chart while gamma sweeps" divides the overlap by the PICTURE's own area, so a picture wholly on
	// screen scores 100% however little of the WIDGET it covers. What a player sees is the complement:
	// the fraction of the chart with nothing drawn on it.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	for (int spin = 0; spin < 500 && f.terrain.snapshot()->rows < TerrainMap::kResolution; ++spin) {
		f.now += 4;
		f.stage.draw(canvas);
		std::this_thread::sleep_for(std::chrono::milliseconds(4));
	}
	REQUIRE(f.terrain.snapshot()->rows == TerrainMap::kResolution);

	// THE DISK IS THE CHART, and `partBounds(Part::Chart)` does not know that: Chart falls through to
	// `default` and returns an EMPTY rectangle, so a press placed by it lands at (0,0), hits nothing,
	// and every number downstream is about a gesture that never happened. `hitAt` says as much —
	// the corners and buttons are rectangles, and the chart is `hitsDisk`.
	const StageDisk chartDisk = f.stage.disk();
	const visage::Bounds box { chartDisk.centre().x - chartDisk.radius(),
		                       chartDisk.centre().y - chartDisk.radius(), 2.0f * chartDisk.radius(),
		                       2.0f * chartDisk.radius() };
	const float x = chartDisk.centre().x;
	const float y = chartDisk.centre().y;
	REQUIRE(f.stage.hitAt({ x, y }) == ExploreStage::Part::Chart);

	// TWO WAYS THE CHART CAN HAVE NOTHING ON IT, and they are different facts. The picture can be placed
	// off the widget (what the case above measures, by the wrong denominator) — or it can be placed
	// exactly right and be TRANSPARENT where the build has not reached and no carry covered. A new
	// window is published the instant its build starts, seeded by carry, so placement snaps forward at
	// once and only the second kind survives a pan. Measure both so the one that is zero says so.
	// AGAINST THE CHART, NOT THE STAGE. Measured against the whole widget this reads 42% bare at rest
	// and never moves — that is the square map not filling a 866x534 stage, which is geometry and not a
	// defect, and it drowns the thing being looked for.
	const auto offWidget = [&] {
		const visage::Bounds chart = box;
		const visage::Bounds where = f.stage.terrainPlacement();
		const float ox = std::max(0.0f, std::min(where.right(), chart.right()) - std::max(where.x(), chart.x()));
		const float oy = std::max(0.0f, std::min(where.bottom(), chart.bottom()) - std::max(where.y(), chart.y()));
		return 1.0f - (ox * oy) / (chart.width() * chart.height());
	};
	// **ONLY THE TEXELS THAT LAND ON THE CHART.** Since `kTerrainMargin` the map is built WIDER than the
	// view, and the ring outside the view is speculative ground nobody is looking at — a build that has
	// not filled it yet is the margin doing its job, not a hole. Counting the whole snapshot scored a
	// transparent margin as void and read 16% where the chart was whole, which is the same mistake as
	// the case that measures the picture's own area instead of the chart's, one file up.
	const auto transparent = [&] {
		const std::shared_ptr<const TerrainSnapshot> t = f.terrain.snapshot();
		const double viewFraction = 1.0 / ExploreStage::terrainMargin();
		const double half = 0.5 * viewFraction * TerrainMap::kResolution;
		const double mid = 0.5 * TerrainMap::kResolution;
		const int from = std::max(0, static_cast<int>(std::ceil(mid - half)));
		const int to = std::min(TerrainMap::kResolution, static_cast<int>(std::floor(mid + half)));
		int empty = 0;
		int looked = 0;
		for (int row = from; row < to; ++row) {
			for (int col = from; col < to; ++col) {
				++looked;
				if ((t->texel(col, row) >> 24) == 0u) {
					++empty;
				}
			}
		}
		return looked > 0 ? static_cast<float>(empty) / static_cast<float>(looked) : 0.0f;
	};
	const auto bare = [&] { return std::max(offWidget(), transparent()); };

	const double startCentre = f.stage.viewCentre();
	const double startHome = f.stage.selectedHome();
	const double startDrift = f.stage.selectedDrift();
	const double startWindow = f.terrain.snapshot()->window.travelYCentre;
	const double startWindowTravel = f.terrain.snapshot()->window.travelXCentre;
	
	const float atRest = bare();
	f.stage.onMouseDown().callback(at(x, y));
	float worst = 0.0f;
	float worstOff = 0.0f;
	float worstClear = 0.0f;
	double total = 0.0;
	int frames = 0;
	// A FLING, not a crawl. `keepInsideView` lets the star wander 0.35 of a span from centre before the
	// view moves at all, so a drag of a hundred pixels from the middle pans nothing and would measure a
	// chart that never moved. 25px a frame at 60fps is about 1500px/s, comfortably past the
	// `kViewsPerSecond` cap — which is the case the cap exists for and the one that showed the symptom.
	for (int frame = 1; frame <= 60; ++frame) {
		f.now += 16;
		f.stage.onMouseDrag().callback(at(x - 25.0f * static_cast<float>(frame), y - 25.0f * static_cast<float>(frame)));
		f.stage.update();
		f.settleGesture();
		f.stage.draw(canvas);
		const float b = bare();
		worst = std::max(worst, b);
		worstOff = std::max(worstOff, offWidget());
		worstClear = std::max(worstClear, transparent());
		total += b;
		++frames;
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
	}
	f.stage.onMouseUp().callback(at(x - 1500.0f, y - 1500.0f));
	// The view has to have actually panned, or this is a case about a chart that never moved.
	MESSAGE("view travelled " << (f.stage.viewCentre() - startCentre) << " in home; star travelled "
	                          << (f.stage.selectedHome() - startHome) << " home, "
	                          << (f.stage.selectedDrift() - startDrift) << " drift; window centre "
	                          << startWindow << " -> " << f.terrain.snapshot()->window.travelYCentre
	                          << ", travel " << startWindowTravel << " -> "
	                          << f.terrain.snapshot()->window.travelXCentre);
	CHECK(std::abs(f.stage.viewCentre() - startCentre) > 0.0);
	MESSAGE("at rest " << (100.0f * atRest) << "% bare; during the drag worst " << (100.0f * worst)
	                   << "%, mean " << (100.0 * total / frames) << "% over " << frames << " frames");
	MESSAGE("  split: worst off-widget " << (100.0f * worstOff) << "%, worst transparent "
	                                     << (100.0f * worstClear) << "%");
	CHECK(frames == 60);
	// AT REST THE CHART IS WHOLE, and that is the half of this a REGRESSION could take away without
	// anyone noticing — the drag numbers are a measurement to be read, but a chart with a hole in it
	// standing still is simply broken.
	CHECK(atRest == 0.0f);
}

TEST_CASE("the terrain is asked for WIDER than the view, so a pan has ground to walk onto") {
	// **THE SPECULATIVE MARGIN, AS A STRUCTURAL FACT** (`ExploreStage::kTerrainMargin`: undrawn
	// canvas still entered the viewport during a drag).
	//
	// THE MEASUREMENT THAT MOTIVATED IT IS A `MEASURE:` CASE AND IS SKIPPED, because it drags for a
	// real second and its numbers move with the host's load — on a box at load 20 a build takes 300ms
	// and on a quiet one 80ms, and a bound tight enough to be worth having would go red for reasons
	// that are nothing to do with this code. So the measurement stays an instrument to be read, and
	// what guards the margin in the suite is this: the ARITHMETIC, which no load can move.
	//
	// It is worth having because the margin is one multiplication and looks exactly like a mistake.
	// A reader tidying `askForTerrain` would remove it in good faith, every existing case would stay
	// green, and the only thing that would notice is a hand on a chart.
	//
	// WHAT THE MEASUREMENT SAID, for whoever reads this next: with the margin at 1.0 the drag leaves
	// worst 25.3% of the chart with nothing drawn on it and mean 9.8%; at 1.35 both are 0.0%. The
	// control was run — a metric that reads zero has to be shown capable of reading non-zero first.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);

	const TerrainWindow asked = f.stage.askForTerrainForTest();
	// THE VIEW'S OWN SPANS, read from the surface rather than restated, so a change to the zoom ladder
	// cannot make this case agree with a bug.
	const double viewHome = f.stage.homeSpanZones() / 1023.0;
	const double viewTravel = f.stage.travelSpan();
	REQUIRE(viewHome > 0.0);
	REQUIRE(viewTravel > 0.0);

	CHECK(asked.travelYSpan == doctest::Approx(viewHome * ExploreStage::terrainMargin()));
	CHECK(asked.travelXSpan == doctest::Approx(viewTravel * ExploreStage::terrainMargin()));
	// AND STRICTLY WIDER, asserted separately from the ratio: if kTerrainMargin were ever set to 1 the
	// two checks above would both pass and say nothing at all, which is how a guard empties itself.
	CHECK(asked.travelYSpan > viewHome);
	CHECK(asked.travelXSpan > viewTravel);

	// THE CENTRE IS STILL THE VIEW'S. Only the span grows here; biasing the margin along the drag is a
	// later step, and a centre that had quietly moved would put the star off the middle of its own map.
	// Home only — `travelCentre()` is private, and widening a class's surface so a test can read one
	// number it can get another way is how the seam becomes the design.
	CHECK(asked.travelYCentre == doctest::Approx(f.stage.viewCentre()));

	// AND THE ZOOM LADDER DOES NOT ESCAPE IT — the margin is applied where the span is computed, so it
	// has to hold at every stop rather than only at the default one.
	for (const double zoom : { 0.5, 2.0, 8.0, 64.0 }) {
		f.stage.setZoom(zoom);
		f.stage.draw(canvas);
		const TerrainWindow at = f.stage.askForTerrainForTest();
		CAPTURE(zoom);
		CHECK(at.travelYSpan
		      == doctest::Approx((f.stage.homeSpanZones() / 1023.0) * ExploreStage::terrainMargin()));
		CHECK(at.travelXSpan == doctest::Approx(f.stage.travelSpan() * ExploreStage::terrainMargin()));
	}
}

TEST_CASE("MEASURE: a DIAGONAL drag outruns the margin because the cap is per-axis" * doctest::skip()) {
	// The symptom: a little void remained, especially when dragging
	// diagonally.
	//
	// **THE THROTTLE CAPS EACH AXIS SEPARATELY**, and that is the whole of it. `dragStarTo` applies the
	// same `kViewsPerSecond * span * elapsed` step to home and to travel INDEPENDENTLY, so a drag along
	// one axis moves at the cap and a drag along the diagonal moves at `sqrt(2)` times it — about 41%
	// faster than the number that was chosen, and the speculative margin is sized against that number.
	// Void is drag rate x build time, so a diagonal fling eats 41% more ground per build than the
	// margin was bought to cover. The cap belongs on the VECTOR, not on its components.
	//
	// This case measures the three directions side by side so the asymmetry is a number rather than an
	// argument. Each gets a fresh fixture, because a drag leaves the view somewhere else.
	struct Leg {
		const char* name;
		float dx;
		float dy;
	};
	const Leg legs[] = { { "horizontal", -25.0f, 0.0f },
		                 { "vertical", 0.0f, -25.0f },
		                 { "diagonal", -25.0f, -25.0f } };

	for (const Leg& leg : legs) {
		ExploreFixture f;
		f.set(ParamId::AZone, 512.0);
		f.set(ParamId::AOffset, 0.5);
		f.selection.select(0);
		visage::Canvas canvas;
		f.stage.draw(canvas);
		for (int spin = 0; spin < 500 && f.terrain.snapshot()->rows < TerrainMap::kResolution; ++spin) {
			f.now += 4;
			f.stage.draw(canvas);
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		REQUIRE(f.terrain.snapshot()->rows == TerrainMap::kResolution);

		const StageDisk d = f.stage.disk();
		const visage::Bounds box { d.centre().x - d.radius(), d.centre().y - d.radius(),
			                       2.0f * d.radius(), 2.0f * d.radius() };
		const float x = d.centre().x;
		const float y = d.centre().y;
		REQUIRE(f.stage.hitAt({ x, y }) == ExploreStage::Part::Chart);

		// Only the texels that land ON the chart — the margin ring outside the view is speculative
		// ground nobody is looking at, and counting it scores the margin working as a hole.
		const auto bare = [&] {
			const std::shared_ptr<const TerrainSnapshot> t = f.terrain.snapshot();
			const double viewFraction = 1.0 / ExploreStage::terrainMargin();
			const double half = 0.5 * viewFraction * TerrainMap::kResolution;
			const double mid = 0.5 * TerrainMap::kResolution;
			const int from = std::max(0, static_cast<int>(std::ceil(mid - half)));
			const int to = std::min(TerrainMap::kResolution, static_cast<int>(std::floor(mid + half)));
			int empty = 0;
			int looked = 0;
			for (int row = from; row < to; ++row) {
				for (int col = from; col < to; ++col) {
					++looked;
					if ((t->texel(col, row) >> 24) == 0u) {
						++empty;
					}
				}
			}
			const visage::Bounds where = f.stage.terrainPlacement();
			const float ox = std::max(0.0f, std::min(where.right(), box.right()) - std::max(where.x(), box.x()));
			const float oy = std::max(0.0f, std::min(where.bottom(), box.bottom()) - std::max(where.y(), box.y()));
			const float off = 1.0f - (ox * oy) / (box.width() * box.height());
			const float clear = looked > 0 ? static_cast<float>(empty) / static_cast<float>(looked) : 0.0f;
			return std::max(off, clear);
		};

		const double startHome = f.stage.selectedHome();
		const double startDrift = f.stage.selectedDrift();
		f.stage.onMouseDown().callback(at(x, y));
		float worst = 0.0f;
		double total = 0.0;
		for (int frame = 1; frame <= 60; ++frame) {
			f.now += 16;
			f.stage.onMouseDrag().callback(
			    at(x + leg.dx * static_cast<float>(frame), y + leg.dy * static_cast<float>(frame)));
			f.stage.update();
			f.settleGesture();
			f.stage.draw(canvas);
			const float b = bare();
			worst = std::max(worst, b);
			total += b;
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
		f.stage.onMouseUp().callback(at(x + leg.dx * 60.0f, y + leg.dy * 60.0f));

		// HOW FAR THE STAR ACTUALLY WENT, in views, per axis and as a VECTOR. The vector length is the
		// number the cap is supposed to bound and the one it does not: if the diagonal's length is about
		// sqrt(2) times an axis leg's, the cap is on the components.
		const double dHome = std::abs(f.stage.selectedHome() - startHome) / (f.stage.homeSpanZones() / 1023.0);
		const double dTravel = std::abs(f.stage.selectedDrift() - startDrift) / f.stage.travelSpan();
		MESSAGE(std::string(leg.name) << ": worst " << (100.0f * worst) << "% bare, mean "
		                              << (100.0 * total / 60.0) << "%; star moved " << dHome
		                              << " home + " << dTravel << " travel = "
		                              << std::hypot(dHome, dTravel) << " views of vector");
	}
	CHECK(true);
}

TEST_CASE("an UNTOUCHED chart asks for the same map every frame, so nothing rebuilds under a still hand") {
	// **THE AT-REST HALF OF THE FLICKER** (an overall flicker on the whole circle, which panning
	// the already drawn pixels should have removed).
	//
	// THERE ARE TWO MECHANISMS THAT COULD PRODUCE A WHOLE-CIRCLE FLICKER AND THEY ARE TOLD APART BY
	// STILLNESS. The bubble's resampling — a warp fixed in screen space over a sliding map — can only
	// shimmer while something MOVES, because it is the tap weights changing that does it. A window key
	// that jitters rebuilds the map forever and flickers WITH THE HAND OFF THE MOUSE. One of those is
	// answerable by an eye and the other is answerable here, so it is answered here.
	//
	// AND THE SHARP ASSERTION IS THE KEY ITSELF, not the counter. `builds()` not climbing is a fact
	// about a worker that may simply have been busy or coalescing; the WINDOW being equal frame to
	// frame is the thing that would have to be false for a rest-state rebuild to exist at all. The
	// counter stays as the second assertion, proving the first is not vacuous — this file's own rule.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);

	// SETTLE FIRST. A map still in flight is not the rest state, and counting through one is how a
	// case reports a spurious rebuild that never happened (see the `quiesce` note above).
	for (int spin = 0; spin < 2000; ++spin) {
		const uint64_t before = f.terrain.builds();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (f.terrain.builds() == before && f.terrain.snapshot()->rows >= TerrainMap::kResolution) {
			break;
		}
	}

	const TerrainWindow settled = f.stage.askForTerrainForTest();
	const uint64_t buildsBefore = f.terrain.builds();
	const uint64_t requestsBefore = f.terrain.requests();
	const visage::Bounds placedBefore = f.stage.terrainPlacement();

	// THIRTY FRAMES AND NOT ONE EVENT. No press, no drag, no dial, no zoom — the editor doing what it
	// does when the player is reading the chart rather than using it.
	for (int frame = 0; frame < 30; ++frame) {
		f.stage.draw(canvas);
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
		CAPTURE(frame);
		CHECK(f.stage.askForTerrainForTest() == settled);
	}

	// NOTHING WAS EVEN ASKED FOR, which is a stronger result than the one this case was written
	// expecting and is worth stating rather than smoothing over. `requests()` is the map's generation
	// and it advances only when the window DIFFERS — so a still hand does not reach the worker at all,
	// and the coalescing happens before the request rather than inside it.
	INFO("requests over 30 still frames: " << f.terrain.requests() - requestsBefore);
	CHECK(f.terrain.requests() == requestsBefore);
	// AND NOTHING WAS BUILT.
	INFO("builds over 30 still frames: " << f.terrain.builds() - buildsBefore);
	CHECK(f.terrain.builds() == buildsBefore);
	// AND THE PICTURE DID NOT MOVE. "Nothing rebuilt" is a fact about the buffer and this project has
	// twice believed that meant the picture was right — so the claim is asserted where the DRAW reads
	// it, the rectangle the map is painted into.
	CHECK(f.stage.terrainPlacement().x() == doctest::Approx(placedBefore.x()));
	CHECK(f.stage.terrainPlacement().y() == doctest::Approx(placedBefore.y()));
	CHECK(f.stage.terrainPlacement().width() == doctest::Approx(placedBefore.width()));

	// **THE POSITIVE CONTROL, AND WITHOUT IT EVERYTHING ABOVE IS A CASE THAT PASSES BY READING
	// NOTHING.** Thirty equal keys and a counter that did not move is exactly the picture a broken
	// `askForTerrainForTest()` would paint — one that returns a default-constructed window, or a stage
	// that never got as far as computing one. So the instrument is shown capable of saying "changed"
	// before its silence is allowed to mean "unchanged", which is this repo's venue rule wearing a
	// different hat.
	//
	// A ZOOM RATHER THAN A DRAG, because the zoom ladder writes the span directly and a drag inside
	// the edge-follow margin deliberately does NOT move the map — the one motion this file already
	// proves leaves the terrain alone would have been the worst possible control.
	f.stage.setZoom(4.0);
	f.stage.draw(canvas);
	const TerrainWindow moved = f.stage.askForTerrainForTest();
	CHECK_FALSE(moved == settled);
	INFO("requests after the zoom: " << f.terrain.requests() - requestsBefore);
	CHECK(f.terrain.requests() > requestsBefore);
}

TEST_CASE("Explore asks for NO repaint at rest — the twinkle that used to demand one is gone") {
	// **THE STANDING COST, NOW REMOVED** (the sky layer was dropped).
	//
	// THIS CASE USED TO ASSERT THE OPPOSITE AND THAT IS WHY IT IS STILL HERE. It read ten repaints
	// over ten still frames and called it the finding — `shimmer_` advanced on every tick and marked
	// the stage dirty unconditionally, so the whole surface re-rasterised forever with no hand on
	// anything, and the only thing it bought was the starfield's twinkle at a ~3.1 s period. That was
	// measured (backlog GJ), judged not worth its cost, and the sky went.
	//
	// SO THE ASSERTION IS INVERTED RATHER THAN DELETED. A case that merely stopped testing the old
	// behaviour would let the next animation quietly reintroduce a per-frame repaint at rest, which is
	// exactly how this one arrived — nothing was watching the cost, so nobody paid attention to it.
	// Now something is.
	//
	// `update()` RETURNING THE DIRTY FLAG IS WHAT MAKES THIS TESTABLE AT ALL — the same reason
	// `notify()` returns a count (CLAUDE.md): a repaint cannot be observed headlessly, so the line
	// that asks for one has to report that it did.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	// SETTLED FIRST. The first tick legitimately has work — the width knob syncs, and a click's pan
	// may still be gliding — so the claim is about a surface at REST, not about the frame after a
	// change. Anything still owed is paid here.
	for (int frame = 0; frame < 40; ++frame) {
		f.now += 16;
		f.stage.update();
	}

	int dirtyFrames = 0;
	for (int frame = 0; frame < 20; ++frame) {
		f.now += 16;
		if (f.stage.update()) {
			++dirtyFrames;
		}
	}
	INFO("repaints asked for over twenty still frames: " << dirtyFrames);
	CHECK(dirtyFrames == 0);

	// NOT VACUOUS: the surface must still ask for a repaint when something actually moves, or this
	// passes just as well against an `update()` that has stopped answering at all. A click is the
	// motion to use — it starts the view's glide (backlog GE), which is a walk `update()` itself
	// drives frame by frame, so its dirty flag is the thing being exercised. A zoom would NOT do: it
	// calls `repaint()` directly and never touches this return, so a control built on one would be
	// testing a road that does not run through the claim.
	const visage::Point c = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(c.x, c.y - f.stage.disk().radius() * 0.6f));
	f.stage.onMouseUp().callback(at(c.x, c.y - f.stage.disk().radius() * 0.6f));
	f.now += 16;
	CHECK(f.stage.update());
}
TEST_CASE("the ground needs its layer in BOTH bubble states, so the debug switch cannot restructure it") {
	// **THE BLACK STRIP** (on the first build with a runtime bubble switch: a strip of canvas was
	// never rendered and sat black in the viewport, perpendicular to the pan direction at the time
	// the switch was triggered, with only SKY lit — so the bubble was OFF).
	//
	// `Frame::requiresLayer()` is `post_effect_ || backdrop_effect_ || cached_ || masked_ || alpha != 1`
	// and `drawToRegion` re-asks it on EVERY draw. With the post effect as the only thing holding the
	// layer up, `setBubbleEnabled(false)` ran `setNeedsLayer(false)` mid-flight: the packed layer
	// dropped, the intermediate region destroyed. A strip that needed paint at that instant never got
	// it, and nothing returns to a region restructured underneath it.
	//
	// THE CONSTRUCTOR ALREADY WARNED ABOUT THIS and the warning was about the other direction — "a post
	// effect arriving later is a frame that has already decided it does not need a layer". Removing one
	// is the same hazard read backwards, and the switch was written straight past it.
	//
	// SO THE INVARIANT IS THE THING TO PIN, not the symptom: the ground needs a layer because of WHAT
	// IT IS, not because of which effect is currently on it. A case that only checked the bubble came
	// back would have stayed green through the whole defect.
	// DRIVEN THROUGH `drawToRegion`, WHICH IS THE LINE THAT BROKE IT. `requiresLayer()` is private and
	// a test cannot ask it; what a test CAN do is make the framework ask it, the same way a real frame
	// does — `drawToRegion` calls `region_.setNeedsLayer(requiresLayer())` every draw, and that call is
	// the one that tore the layer down. Asserting `needsLayer()` after it is asserting the outcome of
	// the real mechanism rather than a restatement of the condition.
	ExploreFixture f;
	visage::Canvas canvas;
	TerrainGround ground;
	ground.setStage(&f.stage);
	ground.setBounds(0.0f, 0.0f, f.stage.width(), f.stage.height());
	ground.region()->setCanvas(&canvas);
	// A DETACHED FRAME IS NEITHER VISIBLE NOR DRAWING, and `redraw()` is a no-op for one —
	// `drawToRegion` then returns before it ever asks `requiresLayer()`, which is the only line this
	// case is about. The first draft left these out and the REQUIRE below caught it reading false with
	// the bubble ON, which is the venue refusing to express the outcome rather than a finding.
	ground.setVisible(true);
	ground.setDrawing(true);
	// AND `redraw()` ALSO NEEDS SOMEBODY TO ASK. `requestRedraw()` returns false with no event handler,
	// so `redrawing_` never latches and `drawToRegion` returns at its first line. In the editor the
	// window supplies this; here the case supplies the smallest thing that satisfies it, because the
	// alternative is a venue that cannot run the line under test.
	visage::FrameEventHandler handler;
	handler.request_redraw = [](visage::Frame*) {};
	ground.setEventHandler(&handler);

	const auto drawOnce = [&] {
		ground.redraw();
		ground.drawToRegion(canvas);
	};

	// NOT VACUOUS, ASSERTED FIRST: a reading of "needs a layer" with the bubble ON proves the question
	// can be answered at all in this venue. If this cannot be made true headlessly the case is worth
	// nothing, and saying so with a REQUIRE is how it fails loudly instead of passing quietly.
	drawOnce();
	REQUIRE(ground.postEffect() != nullptr);
	REQUIRE(ground.region()->needsLayer());

	// THE WHOLE POINT. Take the effect off, draw again so `requiresLayer()` is re-asked, and the layer
	// must still be there — `setCached(true)` in the constructor holds it up independently of which
	// effect is on the frame. Before that line this read false, and that false is the black strip.
	//
	// THROUGH VISAGE'S OWN `removePostEffect()` SINCE THE DEBUG SWITCHES WERE REMOVED (2026-09-20).
	// The switch that first exposed this is gone, so nothing in OUR code can take the effect off any
	// more — which is exactly why the guard is worth keeping rather than deleting with it. The hazard
	// is a property of the framework: `Frame::drawToRegion` re-asks `requiresLayer()` on EVERY draw,
	// so the day anyone adds a post-effect toggle back, or a mask, or an alpha, this frame's layer
	// starts depending on it again. `setCached(true)` is what makes that day harmless, and this is the
	// only thing that can say so.
	ground.removePostEffect();
	drawOnce();
	CHECK(ground.postEffect() == nullptr);
	CHECK(ground.region()->needsLayer());
}

TEST_CASE("MEASURE: does a fixed place on the world jump on screen when the map rebuilds mid-pan?"
          * doctest::skip()) {
	// **THE FLICKER, NARROWED BY A HAND** (it flickered only while moving, and then
	// only on drag-pan, after a screencast showed the chart BYTE-IDENTICAL for 3.58 seconds at
	// rest).
	//
	// A DRAG PAN IS THE ONLY THING THAT MAKES THE MAP REBUILD. At rest the window key is identical
	// frame to frame and is never even requested — asserted elsewhere in this file — so everything that
	// only happens on a rebuild has been invisible to every instrument pointed at this so far,
	// including the probe's rest phase, which touches nothing BY DESIGN.
	//
	// AND A REBUILD MOVES THE LATTICE. `anchoredTravelY` snaps the window's centre to a whole number of
	// cells before any sample is taken, so consecutive windows are not offset by the hand's motion —
	// they are offset by that motion ROUNDED. The picture is meant to absorb this: `terrainPlacement`
	// draws each snapshot at its OWN window's coordinates, so a snapped map drawn in the right place
	// looks identical to an unsnapped one. If it does not absorb it exactly, every rebuild nudges the
	// whole picture, and a pan rebuilds several times a second.
	//
	// SO THE QUESTION IS CONTINUITY, and it is asked of a FIXED POINT IN THE WORLD rather than of the
	// placement rectangle: the rectangle is SUPPOSED to move. A place on the ground should walk down
	// the screen at exactly the rate the view walks up, and should not care that the map under it was
	// rebuilt. Any step bigger than the frame's own motion is a jump an eye would see.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	for (int spin = 0; spin < 2000; ++spin) {
		const uint64_t before = f.terrain.builds();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (f.terrain.builds() == before && f.terrain.snapshot()->rows >= TerrainMap::kResolution) {
			break;
		}
	}

	// A PLACE ON THE GROUND, fixed for the whole run, chosen at the view's centre so it stays on the
	// chart while the pan carries it.
	const double fixedHome = f.stage.viewCentre();

	// WHERE THE CURRENT SNAPSHOT DRAWS IT. Rows map linearly into the placement rectangle, so this is
	// the placement's own arithmetic run backwards for one world coordinate.
	const auto drawnY = [&]() -> double {
		const std::shared_ptr<const TerrainSnapshot> s = f.terrain.snapshot();
		if (s == nullptr || s->rows <= 0) {
			return std::numeric_limits<double>::quiet_NaN();
		}
		const double row = TerrainMap::rowAtTravelY(s->window, fixedHome);
		const visage::Bounds p = f.stage.terrainPlacement();
		return p.y() + (row + 0.5) / static_cast<double>(TerrainMap::kResolution) * p.height();
	};

	// AND WHAT COLOUR THE GROUND IS THERE. This is the other half, and the code's own history says it
	// is the likelier half: before `anchoredTravelY`, every texel's sample point was measured from the
	// window's own centre, so a rebuild moved sixteen thousand sample points and — MEASURED on the
	// shipped build, recorded in terrain.cpp — of 12,544 world positions visible in both maps, ONE kept
	// its colour. The snap is what fixed that. Whether it fixes it EXACTLY is what this asks: a place
	// on the ground must keep its colour across a rebuild, or the whole picture re-textures itself
	// several times a second while a hand drags, which is what an eye would call a flicker.
	const double fixedTravel = f.stage.selectedDrift();
	const auto colourAt = [&]() -> uint32_t {
		const std::shared_ptr<const TerrainSnapshot> s = f.terrain.snapshot();
		// NO `rows == kResolution` GUARD, AND ITS ABSENCE IS THE POINT. The first draft had one, and it
		// meant this never sampled during a pan at all — 19 readings, every one of them from the
		// settled map, reported as "changed 0 times" about the condition under test. A carried row is
		// a real drawn pixel; refusing to look at it is refusing to look at 100% of the pan.
		if (s == nullptr || (s->rows <= 0 && !s->carried)) {
			return 0u;
		}
		const long row = std::lround(TerrainMap::rowAtTravelY(s->window, fixedHome));
		const long col = std::lround(TerrainMap::columnAtTravelX(s->window, fixedTravel));
		if (row < 0 || row >= TerrainMap::kResolution || col < 0 || col >= TerrainMap::kResolution) {
			return 0u;
		}
		return s->texel(static_cast<int>(col), static_cast<int>(row));
	};

	const visage::Point c = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(c.x, c.y));

	// **THE WHOLE-MAP MEASUREMENT, WHICH IS THE ONE THIS FILE'S HISTORY SAYS TO MAKE.** One world place
	// keeping its colour is n=1 and proves nothing about a picture of 16,384 of them; terrain.cpp
	// records the original defect as "of 12,544 world positions visible in both maps, ONE kept its
	// colour", so the population is the claim. Every eighth texel, mapped through ITS OWN window to a
	// world coordinate and looked up in the previous snapshot's grid — which is the only way to ask
	// "is this the same PLACE" across two maps that are anchored differently.
	int panFrames = 0;
	int worstChannelDelta = 0;
	long deltaSum = 0;
	double worstRowFrac = 0.0;
	double worstColFrac = 0.0;
	std::shared_ptr<const TerrainSnapshot> lastSnap;
	long overlapTotal = 0;
	long overlapDiffer = 0;
	double worstFrameShare = 0.0;
	int partialFrames = 0;
	int previousRows = -1;
	int rowChanges = 0;
	std::string rowTrace;
	uint32_t previousColour = colourAt();
	int colourChangesOnRebuild = 0;
	int colourChangesBetween = 0;
	int colourSamples = 0;
	double previous = drawnY();
	uint64_t previousBuilds = f.terrain.builds();
	double worstStepOnRebuild = 0.0;
	double worstStepBetween = 0.0;
	int rebuilds = 0;
	int frames = 0;

	for (int frame = 1; frame <= 120; ++frame) {
		f.now += 16;
		f.stage.onMouseDrag().callback(at(c.x, c.y - 22.0f * static_cast<float>(frame)));
		f.stage.update();
		f.settleGesture();
		f.stage.draw(canvas);
		std::this_thread::sleep_for(std::chrono::milliseconds(16));

		const double now = drawnY();
		const uint64_t builds = f.terrain.builds();
		const uint32_t colour = colourAt();
		if (colour != 0u && previousColour != 0u) {
			++colourSamples;
			if (colour != previousColour) {
				if (builds != previousBuilds) {
					++colourChangesOnRebuild;
				}
				else {
					++colourChangesBetween;
				}
			}
		}
		previousColour = colour;
		++panFrames;
		{
			const std::shared_ptr<const TerrainSnapshot> snap = f.terrain.snapshot();
			if (snap != nullptr && lastSnap != nullptr && snap != lastSnap
			    && (snap->rows > 0 || snap->carried) && (lastSnap->rows > 0 || lastSnap->carried)) {
				long seen = 0;
				long moved = 0;
				for (int row = 0; row < TerrainMap::kResolution; row += 8) {
					for (int col = 0; col < TerrainMap::kResolution; col += 8) {
						const double home = TerrainMap::travelYAtRow(snap->window, row);
						const double travel = TerrainMap::travelXAtColumn(snap->window, col);
						// **HOW FAR THE LOOKUP HAD TO ROUND, recorded before it is trusted.** If two
						// consecutive windows sit on the same lattice these land on integers and the
						// comparison is exact; if they are offset by half a texel this compares a place
						// with its NEIGHBOUR and manufactures a difference out of nothing. The share
						// below is only meaningful if this stays near zero.
						const double wasRowExact = TerrainMap::rowAtTravelY(lastSnap->window, home);
						const double wasColExact = TerrainMap::columnAtTravelX(lastSnap->window, travel);
						worstRowFrac = std::max(worstRowFrac,
						                        std::abs(wasRowExact - std::round(wasRowExact)));
						worstColFrac = std::max(worstColFrac,
						                        std::abs(wasColExact - std::round(wasColExact)));
						const long wasRow = std::lround(wasRowExact);
						const long wasCol = std::lround(wasColExact);
						if (wasRow < 0 || wasRow >= TerrainMap::kResolution || wasCol < 0
						    || wasCol >= TerrainMap::kResolution) {
							continue;
						}
						const uint32_t nowPx = snap->texel(col, row);
						const uint32_t wasPx = lastSnap->texel(static_cast<int>(wasCol),
						                                       static_cast<int>(wasRow));
						if ((nowPx >> 24) == 0u || (wasPx >> 24) == 0u) {
							continue;
						}
						++seen;
						if (nowPx != wasPx) {
							++moved;
							// HOW FAR IT MOVED, not just that it did. A one-level shift is invisible and
							// a fifty-level one is a sparkle; a count alone cannot tell those apart, and
							// the difference decides whether this is the flicker or a rounding artefact.
							int worst = 0;
							for (int shift = 0; shift <= 16; shift += 8) {
								const int a = static_cast<int>((nowPx >> shift) & 0xffu);
								const int b = static_cast<int>((wasPx >> shift) & 0xffu);
								worst = std::max(worst, std::abs(a - b));
							}
							worstChannelDelta = std::max(worstChannelDelta, worst);
							deltaSum += worst;
						}
					}
				}
				overlapTotal += seen;
				overlapDiffer += moved;
				if (seen > 0) {
					worstFrameShare = std::max(worstFrameShare,
					                           static_cast<double>(moved) / static_cast<double>(seen));
				}
			}
			if (snap != nullptr) {
				lastSnap = snap;
			}
		}
		{
			const std::shared_ptr<const TerrainSnapshot> snap = f.terrain.snapshot();
			const int rows = snap ? snap->rows : -1;
			if (rows < TerrainMap::kResolution) {
				++partialFrames;
			}
			if (rows != previousRows) {
				++rowChanges;
			}
			previousRows = rows;
			rowTrace += std::to_string(rows) + " ";
		}
		if (std::isfinite(now) && std::isfinite(previous)) {
			const double step = std::abs(now - previous);
			if (builds != previousBuilds) {
				++rebuilds;
				worstStepOnRebuild = std::max(worstStepOnRebuild, step);
			}
			else {
				worstStepBetween = std::max(worstStepBetween, step);
			}
			++frames;
		}
		previous = now;
		previousBuilds = builds;
	}
	f.stage.onMouseUp().callback(at(c.x, c.y - 400.0f));

	MESSAGE("MAGNITUDE of those changes: worst single channel " << worstChannelDelta
	        << " of 255; mean over the changed ones "
	        << (overlapDiffer > 0 ? static_cast<double>(deltaSum) / static_cast<double>(overlapDiffer) : 0.0));
	MESSAGE("lookup rounding (0 = the two lattices agree, 0.5 = comparing neighbours): worst row "
	        << worstRowFrac << ", worst column " << worstColFrac);
	MESSAGE("SAME WORLD PLACE, CONSECUTIVE SNAPSHOTS: "
	        << overlapDiffer << " of " << overlapTotal << " sampled positions changed colour ("
	        << (overlapTotal > 0 ? 100.0 * static_cast<double>(overlapDiffer)
	                                   / static_cast<double>(overlapTotal)
	                             : 0.0)
	        << "%); worst single frame " << (100.0 * worstFrameShare) << "%");
	MESSAGE("snapshot rows per frame (kResolution=" << TerrainMap::kResolution << "): " << rowTrace);
	MESSAGE("frames drawing a PARTIAL map: " << partialFrames << " of " << panFrames << "; the row count changed "
	                                         << rowChanges << " times");
	MESSAGE("colour of ONE fixed world place over " << colourSamples << " samples: changed "
	                                               << colourChangesOnRebuild << " times on a rebuild frame, "
	                                               << colourChangesBetween << " times with no rebuild");
	MESSAGE("over " << frames << " frames with " << rebuilds
	                << " rebuilds: worst screen step for a FIXED world place was "
	                << worstStepOnRebuild << "px on a rebuild frame, " << worstStepBetween
	                << "px on a frame with no rebuild");
	// THE CONTROL: the pan has to have actually rebuilt, or this measured a map that never changed.
	// THE CONTROLS: the pan has to have actually rebuilt, and the lookup has to have been exact, or
	// every number above is about something else.
	CHECK(rebuilds > 0);
	CHECK(frames > 0);
	CHECK(overlapTotal > 0);
	CHECK(worstRowFrac < 1e-6);
	CHECK(worstColFrac < 1e-6);
	// AND THE FINDING, PINNED AS A FACT RATHER THAN A BOUND: during a drag the map never completes.
	// If this ever goes red because the builder started finishing, that is the fix landing and this
	// case should be rewritten, not relaxed.
	CHECK(partialFrames > 0);
}

TEST_CASE("a click PANS the view to its new centre instead of snapping there, and so does a jog's release") {
	// **backlog GE** (a click used to snap the view to centre; it should pan there instead, and
	// the pan after a click is distinct from the one after a jog).
	//
	// THE DESTINATION WAS NEVER WRONG — `recentre()` has always centred on the selected sound. What
	// was missing is the JOURNEY, and that is exactly what a case can get wrong: asserting where the
	// view ends up passes against the teleport this replaces, because the teleport ends up in the same
	// place. So the claim under test is that the view is SOMEWHERE IN BETWEEN on the frames after the
	// release, and that it has not arrived yet.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);

	const double before = f.stage.viewCentre();
	const visage::Point c = f.stage.disk().centre();

	// A CLICK WELL AWAY FROM THE MIDDLE, because a press near the centre has almost nowhere to travel
	// and would leave the difference between a pan and a jump inside the rounding.
	const float reach = f.stage.disk().radius() * 0.6f;
	f.stage.onMouseDown().callback(at(c.x, c.y - reach));
	// The star goes to the pressed point AT ONCE — that half was never in question and is what makes
	// the chart feel direct. Only the view is asked to take its time.
	// THE VIEW'S AXIS IS THE DECODED PLANE'S Y (backlog GY), so the place it is panning TO is the
	// sound's Y travel. This read `selectedHome()`, which the view stopped following — and the miss was
	// large rather than a rounding (0.013 against 1.000), which is what says "wrong quantity" rather
	// than "wrong by a bit".
	const double star = f.stage.selectedDriftY();
	CHECK(star != doctest::Approx(before));
	// AND THE VIEW HAS NOT MOVED YET. If it had, the press itself would be doing the centring and
	// everything below would be measuring the tail of a jump.
	CHECK(f.stage.viewCentre() == doctest::Approx(before));

	f.stage.onMouseUp().callback(at(c.x, c.y - reach));
	REQUIRE(f.stage.viewGlidingForTest());

	// ONE FRAME IN: MOVED, BUT NOT ARRIVED. This is the whole of the change — the teleport would have
	// satisfied "moved" on this very frame and failed "not arrived".
	f.now += 16;
	f.stage.update();
	const double afterOneFrame = f.stage.viewCentre();
	INFO("view " << before << " -> " << afterOneFrame << " after one frame, target " << star);
	CHECK(afterOneFrame != doctest::Approx(before));
	CHECK(afterOneFrame != doctest::Approx(star));
	// And it went the RIGHT WAY — between where it was and where it is going, rather than merely
	// different from both.
	CHECK(((afterOneFrame > before && afterOneFrame < star)
	       || (afterOneFrame < before && afterOneFrame > star)));

	// AND IT GETS THERE. A pan that never lands is worse than a snap; the walk is bounded but it is
	// not asymptotic, and `stepViewGlide` lands EXACTLY on the target rather than within a step of it.
	for (int frame = 0; frame < 400 && f.stage.viewGlidingForTest(); ++frame) {
		f.now += 16;
		f.stage.update();
	}
	CHECK_FALSE(f.stage.viewGlidingForTest());
	CHECK(f.stage.viewCentre() == doctest::Approx(star));

	// **AND A SELECTION CHANGE STILL SNAPS**, which is the half that keeps this from being a blanket
	// easing. A new sound is a new chart: easing into it would animate a view from a place it was
	// never at, and the first frame of the editor would slide into position from nowhere. `settleOn`
	// cancels any glide in flight for the same reason — an instant centring is a statement about where
	// the view IS, and it must win over a stale target.
	f.stage.onMouseDown().callback(at(c.x, c.y - reach));
	f.stage.onMouseUp().callback(at(c.x, c.y - reach));
	REQUIRE(f.stage.viewGlidingForTest());
	f.selection.select(1);
	f.stage.draw(canvas);
	CHECK_FALSE(f.stage.viewGlidingForTest());
	CHECK(f.stage.viewCentre() == doctest::Approx(f.stage.selectedDriftY()));
}

TEST_CASE("the Gamma corner holds the map still while it turns, and asks for the true one on release") {
	// **backlog GY, option ii of three.** This is where the guarantee that used to
	// live in the controls suite as "a gamma sweep at a hand's speed keeps a live map" now lives. It
	// moved because its old mechanism went: that case drove `TerrainMap` directly and turned on
	// `carry()` matching band-relatively (`home - gammaSum`), which existed only because the view
	// followed gamma one for one. Nothing follows gamma now.
	//
	// WHAT REPLACED THE PROBLEM. Gamma sets the one home the whole plane is taken at, so it is in the
	// content key and every step of a turn is a genuinely different map — and none of them can reuse
	// the last, because `TerrainSnapshot::computed` needs the windows to agree on every field but
	// their centres and this is the field that differs. MEASURED before the hold: 22 builds over one
	// sweep with the chart 43% blank on average, which is the "scroll into nothing" by a new road.
	//
	// SO THE MAP HOLDS THE BASE THE GESTURE STARTED AT. Stale while a hand is turning, true the moment
	// it stops — the same bargain the drag cap makes with the star.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	for (int spin = 0; spin < 2000; ++spin) {
		const uint64_t before = f.terrain.builds();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (f.terrain.builds() == before && f.terrain.snapshot()->rows >= TerrainMap::kResolution) {
			break;
		}
	}

	const visage::Bounds corner = f.stage.partBounds(ExploreStage::Part::Gamma);
	REQUIRE(corner.width() > 0.0f);
	const visage::Point at0 { corner.x() + corner.width() * 0.5f,
		                      corner.y() + corner.height() * 0.5f };
	const double baseBefore = f.stage.askForTerrainForTest().gammaBase;

	// A REAL TURN, twenty events — enough that a rebuild per event would be unmissable.
	//
	// THE BASELINE IS TAKEN AFTER THE PRESS, and that is not a convenience. `onMouseDown` re-centres a
	// chart that has never been centred on this slot before touching the switch below, and that
	// re-centre legitimately asks for a map. Counting from before the press would charge the hold for
	// a build the press made, which is the kind of off-by-one that gets "fixed" by loosening a bound.
	f.stage.onMouseDown().callback(at(at0.x, at0.y));
	f.settleGesture();
	const uint64_t builtBefore = f.terrain.builds();
	for (int step = 1; step <= 20; ++step) {
		f.now += 16;
		f.stage.onMouseDrag().callback(at(at0.x, at0.y - 3.0f * static_cast<float>(step)));
		f.settleGesture();
		f.stage.draw(canvas);
	}

	// THE GAMMA REALLY MOVED, or the rest of this is a case about a hand that never turned anything.
	INFO("gamma base after the turn: " << f.stage.selectedGammaBase() << " (was " << baseBefore
	                                   << ")");
	REQUIRE(f.stage.selectedGammaBase() != doctest::Approx(baseBefore));
	// AND THE WINDOW DID NOT: the map is still being asked for at the base the gesture opened at, so
	// the key has not moved and there is nothing to rebuild.
	CHECK(f.stage.askForTerrainForTest().gammaBase == doctest::Approx(baseBefore));
	INFO("builds during the turn: " << (f.terrain.builds() - builtBefore));
	CHECK(f.terrain.builds() == builtBefore);

	// AND THE RELEASE ASKS FOR THE TRUE ONE. The hold is cleared before the mouse-up's own
	// `askForTerrain`, so the last word of the gesture is where the sound actually is — a hold that
	// outlived its gesture would freeze the chart at a gamma nobody is on, and it would fail silently.
	f.stage.onMouseUp().callback(at(at0.x, at0.y - 60.0f));
	f.settleGesture();
	CHECK(f.stage.askForTerrainForTest().gammaBase
	      == doctest::Approx(f.stage.selectedGammaBase()));
	CHECK(f.stage.askForTerrainForTest().gammaBase != doctest::Approx(baseBefore));
}

TEST_CASE("where you click is where you go: the sound a press commits is the sound the terrain drew there") {
	// **THE ACCEPTANCE CRITERION FOR backlog GY:** clicking etc. in the terrain keeps the same
	// semantics, and the terrain encode is honest — where you click is where you go.
	//
	// THIS IS THE ONE CASE THE WHOLE REFACTOR HAS TO PASS. Everything else — the axes, the basis, the
	// key, the carry — is machinery in service of a single promise: the picture under the pointer is a
	// picture OF the sound the pointer would commit. Break that and the chart becomes decoration that
	// lies, which is worse than a chart with a bias in it.
	//
	// IT IS ASSERTED IN BOTH DIRECTIONS, because each alone is satisfiable by a broken surface:
	//   * a press writes the coordinates the SCREEN POINT means — catches a write path that drifted
	//     from the projection;
	//   * and the texel drawn AT that screen point was built at those same coordinates — catches a map
	//     drawn in a frame the pointer does not share, which is exactly the class of defect
	//     `terrainPlacement` exists to prevent and which cost this project three installs.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	for (int spin = 0; spin < 2000; ++spin) {
		const uint64_t before = f.terrain.builds();
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
		if (f.terrain.builds() == before && f.terrain.snapshot()->rows >= TerrainMap::kResolution) {
			break;
		}
	}

	const visage::Point c = f.stage.disk().centre();
	const float reach = f.stage.disk().radius() * 0.45f;
	// OFF BOTH AXES, so a surface that got one of them right by luck cannot pass. A point on a
	// centre-line would agree under a transposed or half-broken projection.
	const visage::Point press { c.x + reach * 0.6f, c.y - reach * 0.7f };

	// WHAT THAT PIXEL MEANS, read through the bubble exactly as the press path reads it.
	const visage::Point flat = f.stage.flatAt(press);
	const double meantX = f.stage.travelXAtScreenX(flat.x);
	const double meantY = f.stage.travelYAtScreenY(flat.y);

	f.stage.onMouseDown().callback(at(press.x, press.y));
	f.settleGesture();

	// **ONE: THE PRESS COMMITTED WHAT THE PIXEL MEANT.** Not "moved in the right direction" — the
	// value. A press is not capped (the cap is for a drag that outruns the builder), so this lands
	// exactly.
	INFO("pressed at " << press.x << "," << press.y << " meaning travel " << meantX << "," << meantY);
	CHECK(f.stage.selectedDrift() == doctest::Approx(meantX));
	CHECK(f.stage.selectedDriftY() == doctest::Approx(meantY));

	// NOT VACUOUS: the press has to have MOVED the sound, or two zeroes would satisfy the above.
	CHECK(meantX != doctest::Approx(0.0));
	CHECK(meantY != doctest::Approx(0.0));

	// **TWO: THE TERRAIN DRAWN THERE IS OF THAT PLACE.** Find the texel the pointer is over, by the
	// map's own index arithmetic, and ask what travel it was built at. It must be the travel the press
	// committed — within half a texel, which is the most a grid can promise about a point inside a
	// cell.
	f.stage.draw(canvas);
	const std::shared_ptr<const TerrainSnapshot> map = f.terrain.snapshot();
	REQUIRE(map->rows >= TerrainMap::kResolution);
	const long col = std::lround(TerrainMap::columnAtTravelX(map->window, meantX));
	const long row = std::lround(TerrainMap::rowAtTravelY(map->window, meantY));
	REQUIRE(col >= 0);
	REQUIRE(col < TerrainMap::kResolution);
	REQUIRE(row >= 0);
	REQUIRE(row < TerrainMap::kResolution);
	const double drawnX = TerrainMap::travelXAtColumn(map->window, static_cast<double>(col));
	const double drawnY = TerrainMap::travelYAtRow(map->window, static_cast<double>(row));
	const double halfCellX = 0.5 * map->window.travelXSpan / TerrainMap::kResolution;
	const double halfCellY = 0.5 * map->window.travelYSpan / TerrainMap::kResolution;
	INFO("texel " << col << "," << row << " was built at " << drawnX << "," << drawnY
	              << "; the press committed " << meantX << "," << meantY);
	CHECK(std::abs(drawnX - meantX) <= halfCellX * 1.001);
	CHECK(std::abs(drawnY - meantY) <= halfCellY * 1.001);

	// **AND THE MAP IS OF THE SOUND'S OWN HOME**, which is the half that makes the other two mean
	// something: a picture at the right travel but somebody else's home is a picture of a different
	// country. Gamma is what moves this, and the chart never writes it.
	CHECK(map->window.gammaBase == doctest::Approx(f.stage.selectedGammaBase()));
}

TEST_CASE("a vertical drag moves the puck in Y, finishes, and the view comes to rest on it") {
	// **THE BUG:** puck navigation was broken — the puck seemed locked in Y and could not be
	// dragged off it, and sometimes stayed panned without recentring. Two symptoms, one cause, and the cause is worth keeping because it
	// is the shape a half-finished refactor makes.
	//
	// THE WRITE PATH HAD MOVED AND THE VIEW PATH HAD NOT. A chart drag writes `drift.y` now, but four
	// places still asked `selectedHome()` where they meant "where is the star vertically" — the drag's
	// view nudge, the released-drag arrival test, `update()`'s settle loop, and `viewCentre()`'s
	// before-first-frame answer. So the arrival test compared a HOME against a travel target, which
	// can never be equal: the settle never finished, which is "stays panned without recentering", and
	// the view was nudged by a coordinate that no longer moves, which reads as "locked in y".
	//
	// IT COMPILED AND 79 OF 108 CASES PASSED. Nothing about `selectedHome()` is wrong on its own — it
	// is still the sound's home, still correct, still used by the gamma road. It was simply the wrong
	// QUESTION at four call sites, and no type could say so.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);

	const visage::Point c = f.stage.disk().centre();
	const double yBefore = f.stage.selectedDriftY();
	const double xBefore = f.stage.selectedDrift();

	// STRAIGHT UP, far enough to be unambiguous and past the edge-follow deadzone.
	f.stage.onMouseDown().callback(at(c.x, c.y));
	for (int step = 1; step <= 12; ++step) {
		f.now += 16;
		f.stage.onMouseDrag().callback(at(c.x, c.y - 12.0f * static_cast<float>(step)));
		f.stage.update();
		f.settleGesture();
		f.stage.draw(canvas);
	}
	f.stage.onMouseUp().callback(at(c.x, c.y - 144.0f));
	f.settleGesture();

	// **ONE: THE PUCK MOVED IN Y.** This is the "locked" half, and it is the assertion that a write
	// path pointed at the wrong coordinate cannot satisfy.
	INFO("drift y " << yBefore << " -> " << f.stage.selectedDriftY());
	CHECK(f.stage.selectedDriftY() != doctest::Approx(yBefore));
	// AND NOT SIDEWAYS, because a drag straight up that moved x would mean the two axes are crossed.
	CHECK(f.stage.selectedDrift() == doctest::Approx(xBefore));
	// AND THE SOUND'S HOME DID NOT MOVE: a chart drag is travel, and gamma has one writer.
	CHECK(f.stage.selectedGammaBase() == doctest::Approx(0.0));

	// **TWO: THE GESTURE FINISHES.** The released drag coasts at the capped rate and then closes; if
	// its arrival test can never be satisfied it coasts forever, which is the "stays panned" half.
	const int frames = f.settleScrub();
	INFO("frames for the released drag to arrive and the view to land: " << frames);
	CHECK(frames < 400);
	CHECK_FALSE(f.stage.settlingScrubForTest());
	CHECK_FALSE(f.stage.viewGlidingForTest());

	// **THREE: AND THE VIEW IS ON THE PUCK.** The close re-centres by panning (backlog GE), so once
	// everything has landed the centre is the star's own place — both halves of it.
	// The vertical half only — `travelCentre()` is private, and widening a class's surface so a case
	// can read a number it can already infer is how the seam becomes the design. The horizontal half
	// is covered by the centring cases that have always asserted it.
	CHECK(f.stage.viewCentre() == doctest::Approx(f.stage.selectedDriftY()));
}

TEST_CASE("the puck is DRAWN where the press put it — the mark and the sound are the same place") {
	// The symptom: the puck got stuck on the
	// bottom of the disc. Four draw sites still placed the star from `selectedHome()` — `drawStar`
	// twice, the planet light's satellite position, and one more — while the view centred on the
	// sound's travel. At a home of about 0.5 against a vertical span of 0.063, that is eight
	// view-heights off centre: the mark pins to the rim and stays there however you drag.
	//
	// AND IT IS A DIFFERENT DEFECT FROM THE ONE BEFORE IT, which is why this case exists beside the
	// other. That one was the view asking the wrong question, so the puck would not MOVE. This one is
	// the picture asking it, so the puck moves and is drawn somewhere else — and the previous case
	// passes throughout, because it only ever read values.
	//
	// SO THE CLAIM IS ABOUT PIXELS, not about the number behind them: press a point, and the mark must
	// appear AT that point. It is the visual half of "where you click is where you go", and nothing
	// that reads state alone can stand in for it.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);

	const visage::Point c = f.stage.disk().centre();
	const float reach = f.stage.disk().radius() * 0.5f;
	// OFF BOTH AXES AND BELOW THE MIDDLE, because "stuck at the bottom" is satisfied by a mark that
	// happens to sit low; a point that is neither centred nor at an extreme cannot be reached by
	// accident.
	const visage::Point press { c.x - reach * 0.5f, c.y + reach * 0.6f };
	f.stage.onMouseDown().callback(at(press.x, press.y));
	f.settleGesture();
	f.stage.onMouseUp().callback(at(press.x, press.y));
	f.settleGesture();
	f.settleScrub();
	f.stage.draw(canvas);

	// **WHERE THE SURFACE ACTUALLY DREW THE STAR, taken from the draw itself.**
	//
	// THIS LINE USED TO RECOMPUTE IT — `glassAt(selectedDriftY(), selectedDrift())` — which is the same
	// arithmetic the draw does, asked a second time. So the case asserted that the recomputation agrees
	// with the disc centre and said NOTHING about the picture, in direct contradiction of its own
	// heading three lines up. MEASURED 2026-09-21: a known-bad that moved a draw site to
	// `selectedHome()` left this case GREEN.
	//
	// That is CLAUDE.md's rule arriving from an unexpected side: "a claim about a value cannot be
	// inferred from a rendered buffer" is usually a warning against reading pixels, and this is the
	// mirror — a claim about the PICTURE cannot be made by re-deriving the number behind it.
	REQUIRE(f.stage.starDrawnOnChart());
	const visage::Point mark = f.stage.starDrawnAt();
	// AND THE RECOMPUTATION IS KEPT AS AN INDEPENDENT REFERENCE, not as the subject: the drawn point
	// must equal the point the projection says, which is what ties the picture to the state. Two
	// assertions, because either one alone is satisfiable by a bug the other catches.
	const visage::Point projected = f.stage.glassAt(f.stage.selectedDriftY(), f.stage.selectedDrift());
	INFO("drawn at " << mark.x << "," << mark.y << "; the projection says " << projected.x << ","
	                 << projected.y);
	CHECK(mark.x == doctest::Approx(projected.x));
	CHECK(mark.y == doctest::Approx(projected.y));

	// AFTER THE CLOSE THE VIEW HAS PANNED ONTO THE SOUND (backlog GE), so the mark belongs at the
	// middle of the disc — which is also the strongest available statement of "not pinned to a rim".
	INFO("mark at " << mark.x << "," << mark.y << "; disc centre " << c.x << "," << c.y
	                << "; radius " << f.stage.disk().radius());
	CHECK(mark.x == doctest::Approx(c.x).epsilon(0.02));
	CHECK(mark.y == doctest::Approx(c.y).epsilon(0.02));

	// NOT VACUOUS, AND THIS IS THE HALF THAT CATCHES THE BUG: before the fix the mark sat about eight
	// view-heights out, so "is it on the disc at all" is the question that was failing. A generous
	// bound, because the claim is "on the chart" rather than any particular pixel.
	const float fromCentre = std::hypot(mark.x - c.x, mark.y - c.y);
	CHECK(fromCentre < f.stage.disk().radius());

	// AND THE PRESS REALLY WAS OFF-CENTRE, or a surface that ignored the press entirely would pass
	// every line above by drawing at the middle and never moving.
	CHECK(f.stage.selectedDrift() != doctest::Approx(0.0));
	CHECK(f.stage.selectedDriftY() != doctest::Approx(0.0));
}

namespace {

/// One chart drag from the disc's centre along `(dx, dy)` screen pixels, at a hand's pace, released and
/// settled. Returns the star's travel before and after and the view centre before and after.
struct DragReading {
	double x0, y0, x1, y1;
	double viewY0, viewY1;
	visage::Point star0, star1;
};

DragReading dragChart(ExploreFixture& f, float dx, float dy) {
	visage::Canvas canvas;
	const visage::Point c = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(c.x, c.y));
	f.settleGesture();
	f.stage.draw(canvas);
	DragReading r {};
	r.x0 = f.stage.selectedDrift();
	r.y0 = f.stage.selectedDriftY();
	r.viewY0 = f.stage.viewCentre();
	r.star0 = f.stage.starDrawnAt();
	for (int step = 1; step <= 8; ++step) {
		f.now += 16;
		const float t = static_cast<float>(step) / 8.0f;
		f.stage.onMouseDrag().callback(at(c.x + dx * t, c.y + dy * t));
		f.stage.update();
		f.settleGesture();
	}
	f.stage.draw(canvas);
	r.star1 = f.stage.starDrawnAt();
	f.stage.onMouseUp().callback(at(c.x + dx, c.y + dy));
	f.settleGesture();
	f.settleScrub();
	r.x1 = f.stage.selectedDrift();
	r.y1 = f.stage.selectedDriftY();
	r.viewY1 = f.stage.viewCentre();
	return r;
}

} // namespace

TEST_CASE("a chart drag and the rotate corner go the hand's way at every turn, zoom, orbit and pull") {
	// The symptom: panning and rotate in Explore sometimes went the wrong way.
	// The claim is a SIGN, so this asserts signs: a drag right moves the sound's place right, a drag up
	// moves it up (travel Y grows upward, `travelYAtScreenY`), the star is drawn moving with the hand, and
	// the view that follows on release moves the same way (so the ground slides against the hand). The
	// rotate corner turns forward on an upward drag and back on a downward one. Swept across the
	// conditions "sometimes" could mean: the plane already turned, the zoom, a Radius/Theta orbit, and an
	// engine pull on the orbit (a modulated star).
	constexpr double kPi = 3.14159265358979323846;
	for (const double degrees : { 0.0, 45.0, 90.0, 135.0, 180.0, 270.0 }) {
		for (const double zoom : { 1.0, 4.0, ExploreStage::minZoom() }) {
			for (const int orbitCase : { 0, 1, 2 }) {
				ExploreFixture f;
				f.set(ParamId::AZone, 512.0);
				f.set(ParamId::AOffset, 0.5);
				f.selection.select(0);
				visage::Canvas canvas;
				f.stage.draw(canvas);
				f.stage.setZoom(zoom);
				if (degrees != 0.0) {
					turnRotateCorner(f, static_cast<float>(degrees * kPi / 180.0 / ExploreStage::kRotateSpan
					                                       * Knob::kDragRange));
					f.settleGesture();
				}
				if (orbitCase >= 1) {
					f.bridge.setSoundOrbit(0, PolarOffset { 0.05 / zoom, 2.0 });
				}
				if (orbitCase == 2) {
					TravelPulls pulls;
					pulls.radius[0] = 0.02 / zoom;
					pulls.theta[0] = -1.3;
					f.bridge.publishTravelPulls(pulls);
				}
				f.stage.update();
				f.stage.draw(canvas);
				INFO("turned " << degrees << " deg (reads " << f.stage.selectedRotation() << " rad), zoom "
				               << f.stage.zoom() << ", orbit case " << orbitCase);
				CHECK(f.stage.selectedRotation() == doctest::Approx(degrees * kPi / 180.0).epsilon(0.01));

				struct Way {
					float dx, dy;
					const char* name;
				};
				for (const Way way : { Way { 90.0f, 0.0f, "right" }, Way { -90.0f, 0.0f, "left" },
				                       Way { 0.0f, -90.0f, "up" }, Way { 0.0f, 90.0f, "down" } }) {
					const DragReading r = dragChart(f, way.dx, way.dy);
					INFO("drag " << way.name << ": place x " << r.x0 << " -> " << r.x1 << ", y " << r.y0
					             << " -> " << r.y1 << "; view y " << r.viewY0 << " -> " << r.viewY1
					             << "; star drawn " << r.star0.x << "," << r.star0.y << " -> " << r.star1.x
					             << "," << r.star1.y);
					if (way.dx != 0.0f) {
						CHECK((r.x1 - r.x0) * way.dx > 0.0);
						CHECK(r.y1 == doctest::Approx(r.y0));
						CHECK((r.star1.x - r.star0.x) * way.dx > 0.0f);
					}
					else {
						// Screen y grows DOWN and travel Y grows UP.
						CHECK((r.y1 - r.y0) * -way.dy > 0.0);
						CHECK(r.x1 == doctest::Approx(r.x0));
						CHECK((r.star1.y - r.star0.y) * way.dy > 0.0f);
						CHECK((r.viewY1 - r.viewY0) * -way.dy > 0.0);
					}
				}

				// JOGS: a press that lands while the previous release's glide is still in flight, and a hand
				// that keeps going the same way. Every frame the star is drawn, it must never have moved
				// against the hand since the frame before.
				for (const float dir : { 1.0f, -1.0f }) {
					for (const bool vertical : { false, true }) {
						const visage::Point c = f.stage.disk().centre();
						float lastStar = 0.0f;
						bool haveLast = false;
						for (int jog = 0; jog < 4; ++jog) {
							f.stage.onMouseDown().callback(at(c.x, c.y));
							f.settleGesture();
							for (int step = 0; step <= 6; ++step) {
								const float travel = dir * 8.0f * static_cast<float>(step);
								const float px = vertical ? c.x : c.x + travel;
								const float py = vertical ? c.y + travel : c.y;
								if (step > 0) {
									f.stage.onMouseDrag().callback(at(px, py));
								}
								f.now += 16;
								f.stage.update();
								f.settleGesture();
								f.stage.draw(canvas);
								const visage::Point s = f.stage.starDrawnAt();
								const float along = vertical ? s.y : s.x;
								INFO("jog " << jog << " step " << step << (vertical ? " vertical" : " horizontal")
								            << " dir " << dir << ": star " << lastStar << " -> " << along);
								if (haveLast && step > 0) {
									CHECK((along - lastStar) * dir >= -0.5f);
								}
								lastStar = along;
								haveLast = true;
							}
							f.stage.onMouseUp().callback(at(vertical ? c.x : c.x + dir * 48.0f,
							                                vertical ? c.y + dir * 48.0f : c.y));
							f.settleGesture();
							// A FEW FRAMES ONLY, so the next press lands mid-glide.
							for (int frame = 0; frame < 3; ++frame) {
								f.now += 16;
								f.stage.update();
								f.settleGesture();
							}
							f.stage.draw(canvas);
							haveLast = false;
						}
						f.settleScrub();
					}
				}

				for (const float pixels : { 20.0f, -20.0f }) {
					const double before = f.stage.selectedRotation();
					turnRotateCorner(f, pixels);
					f.settleGesture();
					const double after = f.stage.selectedRotation();
					INFO("rotate corner by " << pixels << " px: " << before << " -> " << after);
					CHECK((after - before) * pixels > 0.0);
					CHECK(after - before
					      == doctest::Approx(pixels / Knob::kDragRange * ExploreStage::kRotateSpan).epsilon(0.01));
				}
			}
		}
	}
}

TEST_CASE("Shift taken up or down mid-drag never turns the Rotate or Gamma corner backward") {
	// THE CORNERS SEND INCREMENTS against a running total (`moved - grabValue_`), and `moved` was recomputed
	// from the press with whichever gearing the modifier asked for NOW. So Shift pressed after 100 px up
	// re-read those 100 px at a fifth of their worth, and the difference went out as one turn BACKWARD while
	// the hand was still moving forward; letting go of Shift sent it forward again. `Knob::dragTo` re-anchors
	// on a modifier change for exactly this reason; the corners did not.
	for (const ExploreStage::Part part : { ExploreStage::Part::Rotate, ExploreStage::Part::Gamma }) {
		ExploreFixture f;
		f.selection.select(0);
		const visage::Bounds box = f.stage.partBounds(part);
		const visage::Point c { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
		const auto reading = [&] {
			return part == ExploreStage::Part::Rotate ? f.stage.selectedRotation() : f.stage.selectedGammaBase();
		};
		const auto shifted = [](visage::MouseEvent e) {
			e.modifiers = visage::kModifierShift;
			return e;
		};
		const double start = reading();
		f.stage.onMouseDown().callback(at(c.x, c.y));
		f.stage.onMouseDrag().callback(at(c.x, c.y - 100.0f));
		const double coarse = reading();
		REQUIRE(coarse > start);

		// Shift goes down and the hand keeps going UP.
		f.stage.onMouseDrag().callback(shifted(at(c.x, c.y - 110.0f)));
		const double fine = reading();
		// Shift comes up and the hand keeps going up.
		f.stage.onMouseDrag().callback(at(c.x, c.y - 120.0f));
		const double coarseAgain = reading();
		f.stage.onMouseUp().callback(at(c.x, c.y - 120.0f));

		INFO((part == ExploreStage::Part::Rotate ? "rotate" : "gamma") << ": " << start << " -> " << coarse
		                                                                 << " -> (shift) " << fine << " -> "
		                                                                 << coarseAgain);
		// NEVER BACKWARD, and each step worth what its own gearing says: 10 px fine is a fifth of 10 px coarse.
		CHECK(fine > coarse);
		CHECK(coarseAgain > fine);
		const double perPixel = (coarse - start) / 100.0;
		CHECK(fine - coarse == doctest::Approx(10.0 * perPixel * Knob::kFineFactor).epsilon(0.02));
		CHECK(coarseAgain - fine == doctest::Approx(10.0 * perPixel).epsilon(0.02));
	}
}

TEST_CASE("MEASURE: which way the terrain appears to turn under a steady Rotate, across the dial"
          * doctest::skip()) {
	// A least-squares fit of the picture's change under a small turn to a rigid motion about the view's
	// centre: brightness constancy says f_t = -(f_x u + f_y v), and with u = u0 - w y, v = v0 + w x the
	// three unknowns (u0, v0, w) are linear. w's SIGN is the sense a hand sees the ground turn; R^2 says how
	// much of the change a turn explains at all. Screen axes: x right, y UP (travel Y grows upward).
	constexpr double kPi = 3.14159265358979323846;
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	const TerrainWindow window = f.stage.askForTerrainForTest();
	constexpr int kGrid = 40;
	const double spanX = window.travelXSpan / ExploreStage::terrainMargin();
	const double spanY = window.travelYSpan / ExploreStage::terrainMargin();
	for (const OscType engine : { OscType::Morph, OscType::Stair, OscType::V5 }) {
		for (const int zone : { 100, 512, 900 }) {
			int positive = 0;
			int negative = 0;
			std::string line;
			for (int k = 0; k < 24; ++k) {
				const double rotate = 2.0 * kPi * k / 24.0;
				const auto field = [&](double r) {
					SliceOrientationPin pin;
					pin.pinned = true;
					pin.generator.rotate = static_cast<float>(r);
					std::vector<double> out(static_cast<size_t>(kGrid * kGrid));
					for (int j = 0; j < kGrid; ++j) {
						for (int i = 0; i < kGrid; ++i) {
							const double tx = ((i + 0.5) / kGrid - 0.5) * spanX;
							const double ty = ((j + 0.5) / kGrid - 0.5) * spanY;
							out[static_cast<size_t>(j * kGrid + i)] =
							    terrainFeaturesAt(engine, pin, static_cast<uint16_t>(zone), 0.5f, tx, ty).centroid;
						}
					}
					return out;
				};
				const double delta = 0.01;
				const std::vector<double> a = field(rotate);
				const std::vector<double> b = field(rotate + delta);
				// Normal equations for (u0, v0, w) in grid units.
				double m[3][3] = {};
				double rhs[3] = {};
				double total = 0.0;
				for (int j = 1; j < kGrid - 1; ++j) {
					for (int i = 1; i < kGrid - 1; ++i) {
						const auto at2 = [&](const std::vector<double>& g, int ii, int jj) {
							return g[static_cast<size_t>(jj * kGrid + ii)];
						};
						const double fx = (at2(a, i + 1, j) - at2(a, i - 1, j)) * 0.5;
						const double fy = (at2(a, i, j + 1) - at2(a, i, j - 1)) * 0.5;
						const double ft = (at2(b, i, j) - at2(a, i, j)) / delta;
						const double x = i - (kGrid - 1) * 0.5;
						const double y = j - (kGrid - 1) * 0.5;
						const double g[3] = { fx, fy, x * fy - y * fx };
						for (int p = 0; p < 3; ++p) {
							for (int q = 0; q < 3; ++q) {
								m[p][q] += g[p] * g[q];
							}
							rhs[p] += g[p] * -ft;
						}
						total += ft * ft;
					}
				}
				// Solve by Cramer's rule.
				const auto det3 = [](double s[3][3]) {
					return s[0][0] * (s[1][1] * s[2][2] - s[1][2] * s[2][1])
					       - s[0][1] * (s[1][0] * s[2][2] - s[1][2] * s[2][0])
					       + s[0][2] * (s[1][0] * s[2][1] - s[1][1] * s[2][0]);
				};
				const double d = det3(m);
				double sw[3][3];
				for (int p = 0; p < 3; ++p) {
					for (int q = 0; q < 3; ++q) {
						sw[p][q] = q == 2 ? rhs[p] : m[p][q];
					}
				}
				const double w = d != 0.0 ? det3(sw) / d : 0.0;
				// R^2 of the fit: explained / total.
				double sx[3][3], sy[3][3];
				for (int p = 0; p < 3; ++p) {
					for (int q = 0; q < 3; ++q) {
						sx[p][q] = q == 0 ? rhs[p] : m[p][q];
						sy[p][q] = q == 1 ? rhs[p] : m[p][q];
					}
				}
				const double u0 = d != 0.0 ? det3(sx) / d : 0.0;
				const double v0 = d != 0.0 ? det3(sy) / d : 0.0;
				const double explained = u0 * rhs[0] + v0 * rhs[1] + w * rhs[2];
				const double r2 = total > 0.0 ? explained / total : 0.0;
				(w > 0.0 ? positive : negative)++;
				char cell[64];
				std::snprintf(cell, sizeof(cell), " %3.0f:%+.3f(R2 %.2f)", rotate * 180.0 / kPi, w, r2);
				line += cell;
			}
			MESSAGE("engine " << static_cast<int>(engine) << " zone " << zone << ": CCW " << positive << " / CW "
			                  << negative << " of 24 turns;" << line);
		}
	}
}

// THE SPECTRUM TAB AND PANEL (additive design §4.2). On Additive only; per sound: the row shows and
// edits the SELECTED sound's settings; every control binds its own sound's param; a control is greyed exactly when its
// edits the SELECTED sound's drawer; every control binds its own sound's param; a control is greyed exactly when its
// operator or the master switch is off (or an operator setting makes it inert).
namespace {

using feathers::SpectralControl;

ParamId drawerParam(size_t slot, SpectralControl c) {
	return feathers::spectralParam(slot, c);
}

/// The drawer's control for (slot, control): a knob's, a chip's, or the mode's `device::Modulatable`.
feathers::device::Modulatable* drawerControl(ui::SpectrumPanel& d, size_t slot, SpectralControl c) {
	if (c == SpectralControl::FilterMode) {
		return &d.modeBoxForTest(slot, 0)->control();
	}
	if (c == SpectralControl::Filter2Mode) {
		return &d.modeBoxForTest(slot, 1)->control();
	}
	if (c == SpectralControl::FilterRouting) {
		return &d.routingForTest(slot, 0).control();
	}
	// OH: the Partials mode cell — its dropdown the scheme, its box the count.
	if (c == SpectralControl::PartialsMode) {
		return const_cast<feathers::device::Modulatable*>(&d.partialsCellForTest(slot)->select().control());
	}
	if (c == SpectralControl::PartialsCount || c == SpectralControl::PartialsSet) {
		return &d.partialsCellForTest(slot)->boxFor(static_cast<size_t>(c))->control();
	}
	// OE2: the Vocode zone cells' boxes (Spacing's and Widths' amount and Free coordinate).
	if (c == SpectralControl::VocodeSpacingAmount || c == SpectralControl::VocodeSpacingFree) {
		return &d.vocodeSpacingForTest(slot)->boxFor(static_cast<size_t>(c))->control();
	}
	if (c == SpectralControl::VocodeWidthsAmount || c == SpectralControl::VocodeWidthsFree) {
		return &d.vocodeWidthsForTest(slot)->boxFor(static_cast<size_t>(c))->control();
	}
	if (ui::SwitchChip* chip = d.switchForTest(slot, c); chip != nullptr) {
		return &chip->control();
	}
	if (ui::FaderPill* pill = d.pillForTest(slot, c); pill != nullptr) {
		return c == SpectralControl::TiltOn || c == SpectralControl::OddEvenOn ? pill->powerForTest() : &pill->amountForTest();
	}
	if (ui::Knob* knob = d.knobForTest(slot, c); knob != nullptr) {
		return knob->control();
	}
	return nullptr;
}

/// EXPLORE AND THE SPECTRUM PANEL beside it, on one bridge — the panel built as the editor builds it (Gui's
/// toggleSpectrumPanel), at its minimum size, and ticked with the stage: `update()` is one editor frame for both. The
/// open and close ROADS (the tab, Esc, ✕, an engine change, the editor closing) need the editor and have their own cases.
struct SpectrumFixture : ExploreFixture {
	ui::SpectrumPanel panel { bridge, registry, selection, &tray };
	SpectrumFixture() {
		panel.setBounds(0.0f, 0.0f, ui::SpectrumPanel::kMinimumWidth, ui::SpectrumPanel::kMinimumHeight);
	}
	void update() {
		stage.update();
		(void)panel.updateContent();
	}
};

} // namespace

TEST_CASE("spectrum panel: the tab is on the face on Additive only, and every panel control is dead elsewhere") {
	SpectrumFixture f;
	for (size_t e = 0; e < static_cast<size_t>(OscType::kCount); ++e) {
		const auto engine = static_cast<OscType>(e);
		CAPTURE(e);
		f.set(ParamId::OscType, static_cast<double>(engine));
		f.set(drawerParam(f.selection.slot(), SpectralControl::FilterOn), 1.0);
		f.update();
		const bool additive = engine == OscType::Additive;
		ui::SpectrumPanel& d = f.panel;
		CHECK(f.stage.spectrumTabForTest().isVisible() == additive);
		const size_t selected = f.selection.slot();
		for (visage::Frame* frame : d.controlsForTest(selected)) {
			if (auto* knob = dynamic_cast<ui::Knob*>(frame); knob != nullptr) {
				CHECK(knob->enabled() == additive);
			}
			if (auto* chip = dynamic_cast<ui::SwitchChip*>(frame); chip != nullptr) {
				// Link greys with its operator, which a fresh sound has off (the greying case pins that rule).
				const bool link = chip == d.switchForTest(selected, SpectralControl::FormantShiftLink);
				// THE ROUTING greys unless both filters are on (F2), which a fresh sound has not.
				const bool routing = chip == &d.routingForTest(selected, 0) || chip == &d.routingForTest(selected, 1);
				CHECK(chip->enabled() == (additive && !link && !routing));
			}
		}
		// THE FILTER MODE BOX greys with the engine, and with its own filter (it changes nothing while the filter is off): on here.
		CHECK(d.modeBoxForTest(selected)->enabled() == additive);
		CHECK(f.stage.spectrumTabForTest().masterForTest().enabled() == additive);
		CHECK(f.stage.spectrumTabForTest().isVisible() == additive);
	}
}

TEST_CASE("spectrum panel: every panel control binds its own sound's param, and the registry knows it") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.update();
	ui::SpectrumPanel& d = f.panel;
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		for (size_t c = 0; c < feathers::kSpectralControlCount; ++c) {
			const auto control = static_cast<SpectralControl>(c);
			const ParamId id = drawerParam(slot, control);
			const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(id));
			REQUIRE(desc != nullptr);
			INFO(desc->name);
			auto* bound = dynamic_cast<ui::ParamControl*>(drawerControl(d, slot, control));
			REQUIRE(bound != nullptr);
			CHECK(bound->id() == static_cast<clap_id>(id));
			const auto watchers = f.registry.watchers(static_cast<clap_id>(id));
			CHECK(std::find(watchers.begin(), watchers.end(), d.frameForTest(slot, control)) != watchers.end());
			// THE STEREO CAPABILITY (design §4.3): an S control offers the stereo glow, every other one does not.
			CHECK(bound->display().stereo == feathers::spectralStereo(control));
		}
	}
}

namespace {
bool inside(const visage::Bounds& inner, const visage::Bounds& outer); // defined with the geometry cases below
bool overlap(const visage::Bounds& a, const visage::Bounds& b);
} // namespace

TEST_CASE("OF drawer: the drawer is the Spectral window's dense panel — amounts are pills, Filter keeps knobs, Mode is a menu, Tone is a cell, the curve is on the plot") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.update();
	ui::SpectrumPanel& d = f.panel;
	using C = SpectralControl;
	// THE FILTER SECTION KEEPS FULL KNOBS (backlog OG, the preference): all five on each filter, no pill.
	for (const C c : { C::FilterCutoff, C::FilterResonance, C::FilterSlope, C::FilterKeyTrack, C::FilterGain, C::Filter2Cutoff,
	                   C::Filter2Resonance, C::Filter2Slope, C::Filter2KeyTrack, C::Filter2Gain }) {
		CAPTURE(static_cast<int>(c));
		CHECK(d.knobForTest(0, c) != nullptr);
		CHECK(d.pillForTest(0, c) == nullptr);
	}
	// EVERY OTHER AMOUNT IS AN AMOUNT-ONLY PILL (the section's chip keeps the power), no knob.
	for (const C c : { C::CombSpacing, C::CombDepth, C::CombPhase, C::FormantShiftLow, C::FormantShiftHigh, C::FormantShiftCrossover,
	                   C::FormantVowel, C::FormantDepth, C::DampTime, C::DampTilt }) {
		CAPTURE(static_cast<int>(c));
		CHECK(d.knobForTest(0, c) == nullptr);
		ui::FaderPill* pill = d.pillForTest(0, c);
		REQUIRE(pill != nullptr);
		CHECK(pill->amountOnly());
	}
	for (const C sw : { C::CombOn, C::FormantShiftOn, C::FormantOn, C::DampOn, C::StretchOn, C::ShiftOn, C::PartialsOn }) {
		CAPTURE(static_cast<int>(sw));
		CHECK(d.switchForTest(0, sw) != nullptr); // the section chip keeps the power
	}
	// A LONE AMOUNT STAYS A KNOB (as the window's does); the Partials Depth is a knob beside the mode cell.
	CHECK(d.knobForTest(0, C::StretchAmount) != nullptr);
	CHECK(d.knobForTest(0, C::ShiftHz) != nullptr);
	CHECK(d.knobForTest(0, C::PartialsDepth) != nullptr);
	CHECK(d.partialsCellForTest(0) != nullptr);
	// THE CHOICES OF 3+ ARE MENUS, all on demand: both filters' Mode, never a segmented row (no multivalue toggle).
	for (const int filter : { 0, 1 }) {
		ui::ValueBox* mode = d.modeBoxForTest(0, filter);
		REQUIRE(mode != nullptr);
		CHECK(mode->choiceMenuAvailable());
		CHECK(d.filterModeForTest(0, filter) == nullptr);
	}
	// TONE: Tilt and Odd/Even are one cell of two pills, each its own switch; no chip of their own.
	for (const C sw : { C::TiltOn, C::OddEvenOn }) {
		CHECK(d.switchForTest(0, sw) == nullptr);
		ui::FaderPill* pill = d.pillForTest(0, sw);
		REQUIRE(pill != nullptr);
		CHECK_FALSE(pill->amountOnly());
	}
	// THE CURVE IS ON THE PLOT: its switch, Flatten and Draw inside the plot's bounds, and no Curve panel in the row.
	const std::vector<visage::Frame*> overlay = d.overlayForTest(0);
	REQUIRE(overlay.size() == 3);
	for (visage::Frame* frame : overlay) {
		REQUIRE(frame != nullptr);
		CHECK(inside(frame->bounds(), d.plotBounds()));
	}
	// THE ROW IS THE DENSE ROW (a pill stack's cell under the head), not the knob row.
	CHECK(d.rowHeightNow() == doctest::Approx(ui::SpectrumPanel::kHeadHeight + 4.0f + ui::PillStack::kHeight + 2.0f * ui::SpectrumPanel::kPanelPad));
	// AND THE ENGINE-WIDE STRIP (A-PH) stays above the plot.
	CHECK(d.phasesBoxForTest() != nullptr);
}

TEST_CASE("OF drawer: Partials Depth is a knob bound to its own sound's param, greyed with the Partials operator and the sound, live under every scheme, and an edit lands on that sound alone") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.update();
	ui::SpectrumPanel& d = f.panel;
	using C = SpectralControl;
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		ui::Knob* knob = d.knobForTest(slot, C::PartialsDepth);
		REQUIRE(knob != nullptr);
		auto* bound = dynamic_cast<ui::ParamControl*>(knob->control());
		REQUIRE(bound != nullptr);
		CHECK(bound->id() == static_cast<clap_id>(drawerParam(slot, C::PartialsDepth)));
		CHECK(std::string(knob->label()) == "Depth");
	}
	const size_t s = f.selection.slot();
	// GREYED WITH ITS OPERATOR: Partials off, the knob does not act; on, it does — under EVERY scheme (the engine reads it for all).
	f.set(drawerParam(s, C::PartialsOn), 0.0);
	f.update();
	CHECK_FALSE(d.liveForTest(s, C::PartialsDepth));
	f.set(drawerParam(s, C::PartialsOn), 1.0);
	for (const double scheme : { 0.0, 1.0, 2.0 }) {
		CAPTURE(scheme);
		f.set(drawerParam(s, C::PartialsMode), scheme);
		f.update();
		CHECK(d.liveForTest(s, C::PartialsDepth));
	}
	f.bridge.setSpectralOn(false);
	f.update();
	CHECK_FALSE(d.liveForTest(s, C::PartialsDepth)); // the master greys everything
	f.bridge.setSpectralOn(true);
	f.update();
	// AN EDIT THROUGH THE KNOB moves that sound's depth and nobody else's; the default is 100%.
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		CHECK(f.bridge.value(static_cast<clap_id>(drawerParam(slot, C::PartialsDepth))) == 1.0);
	}
	d.knobForTest(s, C::PartialsDepth)->control()->setValue(0.35);
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		CHECK(f.bridge.value(static_cast<clap_id>(drawerParam(slot, C::PartialsDepth))) == doctest::Approx(slot == s ? 0.35 : 1.0));
	}
	// ITS KNOB LIES INSIDE THE PANEL, in the Partials panel, clear of the plot and the cell.
	const visage::Bounds knobBox = d.knobForTest(s, C::PartialsDepth)->bounds();
	CHECK(inside(knobBox, visage::Bounds { 0.0f, 0.0f, d.width(), d.height() }));
	CHECK_FALSE(overlap(knobBox, d.plotBounds()));
	CHECK_FALSE(overlap(knobBox, d.partialsCellForTest(s)->bounds()));
}

TEST_CASE("spectrum panel: the row shows the selected sound's settings, and an edit through it lands on that sound alone") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::CEnable, 1.0);
	ui::SpectrumPanel& d = f.panel;
	for (const size_t selected : { size_t { 0 }, size_t { 2 }, size_t { 1 } }) {
		CAPTURE(selected);
		f.selection.select(selected);
		f.update();
		CHECK(d.selected() == selected);
		// EXACTLY ONE SET ON THE FACE: the selected sound's.
		for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
			CHECK(d.knobForTest(slot, SpectralControl::FilterCutoff)->isVisible() == (slot == selected));
			CHECK(d.modeBoxForTest(slot)->isVisible() == (slot == selected));
			CHECK(d.curveSwitchForTest(slot).isVisible() == (slot == selected));
		}
		// AN EDIT THROUGH THE SHOWN KNOB moves that sound's cutoff and nobody else's.
		std::array<double, feathers::kMaxSounds> before {};
		for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
			before[slot] = f.bridge.value(static_cast<clap_id>(drawerParam(slot, SpectralControl::FilterCutoff)));
		}
		const double to = std::log2(300.0 + 100.0 * static_cast<double>(selected));
		d.knobForTest(selected, SpectralControl::FilterCutoff)->control()->setValue(to);
		for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
			const double now = f.bridge.value(static_cast<clap_id>(drawerParam(slot, SpectralControl::FilterCutoff)));
			CHECK(now == doctest::Approx(slot == selected ? to : before[slot]));
		}
		// AND THE CURVE'S SWITCH AND FLATTEN act on the selected sound's curve alone.
		d.curveSwitchForTest(selected).toggleForTest();
		f.bridge.setSpectralCurveBand(selected, 7, 0.3f);
		for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
			CHECK(f.bridge.spectralCurve(slot).on == (slot == selected));
		}
		d.flattenForTest();
		CHECK(f.bridge.spectralCurve(selected).gain == feathers::spectral::unityCurve());
		d.curveSwitchForTest(selected).toggleForTest(); // back off, for the next selection
	}
}

TEST_CASE("A-PH: the drawer's strip offers Phases, Sub and Ceiling — Phases live on Additive, Sub and Ceiling only under Sub priority, each bound to its param") {
	SpectrumFixture f;
	const auto publish = [&f] {
		f.bridge.publish(f.params);
		f.bridge.refresh();
	};
	ui::SpectrumPanel& d = f.panel;
	REQUIRE(d.phasesBoxForTest() != nullptr);
	REQUIRE(d.subBoxForTest() != nullptr);
	REQUIRE(d.ceilingBoxForTest() != nullptr);
	// EACH BOX IS ITS PARAM'S (the honesty rule asks the engine, so the binding is asserted by its declaration).
	CHECK(std::string(d.phasesBoxForTest()->control().desc().name) == "Additive Phases");
	CHECK(std::string(d.subBoxForTest()->control().desc().name) == "Additive Sub");
	CHECK(std::string(d.ceilingBoxForTest()->control().desc().name) == "Additive Ceiling");
	struct At {
		OscType osc;
		double mode;
		bool phases, rest;
	};
	for (const At a : { At { OscType::Additive, 0.0, true, false }, At { OscType::Additive, 1.0, true, false },
	                    At { OscType::Additive, 2.0, true, true }, At { OscType::V5, 2.0, false, false } }) {
		CAPTURE(static_cast<int>(a.osc));
		CAPTURE(a.mode);
		f.set(ParamId::OscType, static_cast<double>(a.osc));
		f.set(ParamId::AdditivePhases, a.mode);
		publish();
		f.update();
		CHECK(d.phasesBoxForTest()->enabled() == a.phases);
		CHECK(d.subBoxForTest()->enabled() == a.rest);
		CHECK(d.ceilingBoxForTest()->enabled() == a.rest);
	}
}

TEST_CASE("spectrum panel: a control is greyed exactly when its operator or the master switch is off, or a setting makes it inert") {
	SpectrumFixture f;
	const auto publish = [&f] {
		f.bridge.publish(f.params);
		f.bridge.refresh();
	};
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	ui::SpectrumPanel& d = f.panel;
	const size_t s = f.selection.slot();
	for (const bool master : { true, false }) {
		f.bridge.setSpectralOn(master);
		for (const SpectralControl lit : feathers::kSpectralSwitches) {
			CAPTURE(master);
			CAPTURE(static_cast<int>(lit));
			// ONE OPERATOR ON, the rest off.
			for (const SpectralControl sw : feathers::kSpectralSwitches) {
				f.params.setBase(static_cast<clap_id>(drawerParam(s, sw)), sw == lit ? 1.0 : 0.0);
			}
			f.params.setBase(static_cast<clap_id>(drawerParam(s, SpectralControl::FormantShiftLink)), 0.0);
			publish();
			f.update();
			for (size_t c = 0; c < feathers::kSpectralControlCount; ++c) {
				const auto control = static_cast<SpectralControl>(c);
				CAPTURE(feathers::findParam(static_cast<clap_id>(drawerParam(s, control)))->name);
				const bool isSwitch = std::find(feathers::kSpectralSwitches.begin(), feathers::kSpectralSwitches.end(),
				                                control)
				                      != feathers::kSpectralSwitches.end();
				// A SWITCH is greyed only by the master; every other control by the master and its operator. F2: a Gain acts only
				// in Bell (the default mode is Low-pass), and the routing only with BOTH filters on (one operator is lit here) —
				// their own case below pins both.
				const bool settingInert = control == SpectralControl::FilterGain || control == SpectralControl::Filter2Gain
				                          || control == SpectralControl::FilterRouting;
				const bool want = master && !settingInert && (isSwitch || feathers::spectralOperatorOf(control) == lit);
				CHECK(d.liveForTest(s, control) == want);
			}
			CHECK(d.curveSwitchForTest(s).enabled() == master);
		}
	}
	f.bridge.setSpectralOn(true);
	for (const SpectralControl sw : feathers::kSpectralSwitches) {
		f.params.setBase(static_cast<clap_id>(drawerParam(s, sw)), 1.0);
	}
	SUBCASE("Notch greys Resonance, and no other mode does") {
		f.params.setBase(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterGain)), 6.0); // a Bell at 0 dB is open (F2)
		for (size_t m = 0; m < feathers::kSpectralFilterModeNames.size(); ++m) {
			f.params.setBase(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterMode)), static_cast<double>(m));
			publish();
			f.update();
			CHECK(d.liveForTest(s, SpectralControl::FilterResonance)
			      == (m != static_cast<size_t>(feathers::spectral::FilterMode::Notch)));
			CHECK(d.liveForTest(s, SpectralControl::FilterCutoff));
		}
	}
	SUBCASE("Link greys the high shift and the crossover, and leaves the low shift") {
		for (const bool link : { false, true }) {
			f.params.setBase(static_cast<clap_id>(drawerParam(s, SpectralControl::FormantShiftLink)), link ? 1.0 : 0.0);
			publish();
			f.update();
			CHECK(d.liveForTest(s, SpectralControl::FormantShiftHigh) == !link);
			CHECK(d.liveForTest(s, SpectralControl::FormantShiftCrossover) == !link);
			CHECK(d.liveForTest(s, SpectralControl::FormantShiftLow));
		}
	}
	SUBCASE("Depth 0 greys the Vowel (it is not heard there, and the morph leaves its position out), and Depth above 0 does not") {
		for (const double depth : { 0.0, 0.4 }) {
			f.params.setBase(static_cast<clap_id>(drawerParam(s, SpectralControl::FormantDepth)), depth);
			publish();
			f.update();
			CHECK(d.liveForTest(s, SpectralControl::FormantVowel) == (depth > 0.0));
			CHECK(d.liveForTest(s, SpectralControl::FormantDepth));
		}
	}
	SUBCASE("Z7d: the filter at its open end greys Resonance, Slope and Key Track (it is off there), and nowhere else does — Notch at both ends (Z9b)") {
		struct At {
			size_t mode;
			double cutoff;
			bool open;
		};
		for (const At a : { At { 0, std::log2(20000.0), true }, At { 0, std::log2(2000.0), false }, At { 1, std::log2(20.0), true },
		                    At { 1, std::log2(2000.0), false }, At { 2, std::log2(20000.0), false }, At { 3, std::log2(20.0), true },
		                    At { 3, std::log2(20000.0), true }, At { 3, std::log2(2000.0), false } }) {
			CAPTURE(a.mode);
			CAPTURE(a.cutoff);
			f.params.setBase(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterMode)), static_cast<double>(a.mode));
			f.params.setBase(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterCutoff)), a.cutoff);
			publish();
			f.update();
			CHECK(d.liveForTest(s, SpectralControl::FilterSlope) == !a.open);
			CHECK(d.liveForTest(s, SpectralControl::FilterKeyTrack) == !a.open);
			CHECK(d.liveForTest(s, SpectralControl::FilterResonance) == (!a.open && a.mode != 3));
			CHECK(d.liveForTest(s, SpectralControl::FilterCutoff)); // the way back out
		}
	}
	SUBCASE("a parked selected sound's drawer is dead, as its flank knobs are") {
		f.set(soundParam(s, SoundField::Enable), 0.0);
		f.update();
		CHECK_FALSE(d.liveForTest(s, SpectralControl::FilterCutoff));
		CHECK_FALSE(d.liveForTest(s, SpectralControl::FilterOn));
	}
	SUBCASE("the drawn curve's Flatten follows the selected sound's curve switch, and puts every band back to unity") {
		f.bridge.setSpectralCurveBand(s, 5, 0.25f);
		f.bridge.setSpectralCurveOn(s, false);
		// THE EDIT IS PINNED TO THE LOADER'S BOUND (kSpectralCurveGainMax), so a drawing cannot hold what a reload would change.
		f.bridge.setSpectralCurveBand(s, 6, 9.0f);
		CHECK(f.bridge.spectralCurve(s).gain[6] == feathers::kSpectralCurveGainMax);
		f.bridge.setSpectralCurveBand(s, 6, -2.0f);
		CHECK(f.bridge.spectralCurve(s).gain[6] == 0.0f);
		f.update();
		CHECK_FALSE(d.flattenEnabledForTest());
		f.bridge.setSpectralCurveOn(s, true);
		f.update();
		CHECK(d.flattenEnabledForTest());
		d.flattenForTest();
		CHECK(f.bridge.spectralCurve(s).gain == feathers::spectral::unityCurve());
	}
}

TEST_CASE("spectrum panel: the tab's label asks the editor for the panel, its caret lights while one is open, and its chip is the master switch, keeping every setting") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	const size_t s = f.selection.slot();
	f.set(drawerParam(s, SpectralControl::FilterOn), 1.0);
	f.set(drawerParam(s, SpectralControl::FilterCutoff), std::log2(700.0));
	f.update();
	ui::SpectrumTab& tab = f.stage.spectrumTabForTest();
	CHECK(tab.bounds().bottom() == doctest::Approx(f.stage.height()));
	// THE LABEL ASKS THE EDITOR (Explore holds no panel): one press, one ask.
	int asks = 0;
	f.stage.onSpectrumTab = [&asks] { ++asks; };
	tab.clickLabelForTest();
	CHECK(asks == 1);
	// THE CARET IS TOLD, and lights only while the editor says a panel is open.
	REQUIRE_FALSE(tab.caretLitForTest());
	f.stage.setSpectrumOpen(true);
	f.update();
	CHECK(tab.caretLitForTest());
	f.stage.setSpectrumOpen(false);
	f.update();
	CHECK_FALSE(tab.caretLitForTest());

	// THE CHIP: the ONE master switch (lead's call, 2026-10-10), stored state, the label lit while it is on.
	CHECK(tab.litForTest());
	CHECK(f.bridge.spectralOn());
	tab.masterForTest().toggleForTest();
	f.update();
	CHECK_FALSE(f.bridge.spectralOn());
	CHECK_FALSE(tab.litForTest());
	// SETTINGS KEPT: the filter is still on at 700 Hz, only greyed.
	CHECK(f.bridge.value(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterOn))) == 1.0);
	CHECK(f.bridge.value(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterCutoff)))
	      == doctest::Approx(std::log2(700.0)));
	CHECK_FALSE(f.panel.liveForTest(s, SpectralControl::FilterCutoff));
	tab.masterForTest().toggleForTest();
	f.update();
	CHECK(f.bridge.spectralOn());
	CHECK(tab.litForTest());
	CHECK(f.panel.liveForTest(s, SpectralControl::FilterCutoff));
}

// ---- THE SPECTRUM DRAWER'S PLOT (D2) ----------------------------------------------------------------------------------
namespace {

using Handle = ui::SpectrumPlot::Handle;

/// A frame as the engine would publish one: `count` harmonic partials at `f0`, the terrain falling as 1/k and the result
/// half of it (the right ear a quarter when `stereo`).
feathers::SpectralPlotFrame plotFrameAt(double f0, int count = 40, bool stereo = false) {
	feathers::SpectralPlotFrame fr;
	fr.f0 = static_cast<float>(f0);
	fr.count = count;
	fr.playing = 1;
	fr.stereo = stereo ? 1 : 0;
	for (int i = 0; i < count; ++i) {
		const auto u = static_cast<size_t>(i);
		fr.ratio[u] = static_cast<float>(i + 1);
		fr.terrain[u] = 1.0f / static_cast<float>(i + 1);
		fr.left[u] = 0.5f * fr.terrain[u];
		fr.right[u] = stereo ? 0.25f * fr.terrain[u] : fr.left[u];
	}
	return fr;
}

/// Additive, the drawer open on sound A, a frame at `f0` published and taken.
ui::SpectrumPlot& openPlot(SpectrumFixture& f, double f0 = 220.0) {
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	f.selection.select(0);
	(void)f.bridge.publishPlot(plotFrameAt(f0));
	f.update();
	return f.panel.plotForTest();
}

double drawerValue(SpectrumFixture& f, SpectralControl c, size_t slot = 0) {
	return f.bridge.value(static_cast<clap_id>(drawerParam(slot, c)));
}

/// A drag of a handle to `to`, as a hand makes it: press on the handle, move, release.
void dragHandle(SpectrumFixture& f, ui::SpectrumPlot& plot, Handle h, visage::Point to) {
	const auto at = plot.handleForTest(h);
	REQUIRE(at.has_value());
	plot.pressForTest(*at);
	plot.dragForTest(to);
	plot.releaseForTest();
	f.update();
}

} // namespace

TEST_CASE("spectrum plot: the axes map Hz and dB both ways, and the bottom of the curve's range writes an exact zero") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f);
	REQUIRE(plot.width() > 200.0f);
	for (const double hz : { 20.0, 110.0, 1000.0, 7300.0, 20000.0 }) {
		CHECK(plot.xToHz(plot.hzToX(hz)) == doctest::Approx(hz).epsilon(1e-4));
	}
	for (const double db : { 12.0, 0.0, -20.0, -48.0 }) {
		CHECK(plot.yToDb(plot.dbToY(db)) == doctest::Approx(db).epsilon(1e-4));
	}
	CHECK(plot.hzToX(20.0) == doctest::Approx(ui::SpectrumPlot::kPadLeft));
	CHECK(plot.hzToX(20000.0) == doctest::Approx(plot.width() - ui::SpectrumPlot::kPadRight));
	// THE FLOOR IS SILENCE, EXACTLY (lead's condition): at −36 dB and everywhere below it the gain written is 0.0f, not a
	// small number — −36 dB is not silence.
	CHECK(plot.curveGainAtY(plot.dbToY(ui::SpectrumPlot::kCurveFloorDb)) == 0.0f);
	CHECK(plot.curveGainAtY(plot.dbToY(-40.0)) == 0.0f);
	CHECK(plot.curveGainAtY(plot.height()) == 0.0f);
	// Above it, the level's amplitude; at the top, the stored maximum.
	CHECK(static_cast<double>(plot.curveGainAtY(plot.dbToY(-30.0))) == doctest::Approx(std::pow(10.0, -1.5)).epsilon(1e-3));
	CHECK(static_cast<double>(plot.curveGainAtY(plot.dbToY(0.0))) == doctest::Approx(1.0).epsilon(1e-3));
	CHECK(plot.curveGainAtY(0.0f) == feathers::kSpectralCurveGainMax);
}

TEST_CASE("spectrum plot: it draws the engine's frame, taken by its serial, and repaints only when the frame or the selected sound's settings move") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f);
	CHECK(plot.frameForTest().sameAs(plotFrameAt(220.0)));
	CHECK(plot.serialForTest() == f.bridge.plotSerial());
	CHECK(plot.f0() == doctest::Approx(220.0));
	const int settled = plot.redrawsForTest();
	for (int k = 0; k < 4; ++k) {
		f.update();
	}
	CHECK(plot.redrawsForTest() == settled); // nothing new: no repaint
	(void)f.bridge.publishPlot(plotFrameAt(330.0, 30, true));
	f.update();
	CHECK(plot.redrawsForTest() == settled + 1);
	CHECK(plot.frameForTest().stereo == 1);
	CHECK(plot.f0() == doctest::Approx(330.0));
	f.set(drawerParam(0, SpectralControl::CombDepth), 0.8); // a handle setting of the selected sound
	f.update();
	CHECK(plot.redrawsForTest() == settled + 2);
	f.set(drawerParam(1, SpectralControl::CombDepth), 0.8); // another sound's: not drawn, no repaint
	f.update();
	CHECK(plot.redrawsForTest() == settled + 2);
}

TEST_CASE("spectrum plot: a handle is drawn exactly while its operator, the master switch and the sound are live") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f);
	const std::array<std::pair<SpectralControl, Handle>, 4> ops { {
	    { SpectralControl::FilterOn, Handle::Filter },
	    { SpectralControl::CombOn, Handle::Comb },
	    { SpectralControl::FormantShiftOn, Handle::ShiftLow },
	    { SpectralControl::TiltOn, Handle::Tilt },
	} };
	for (const auto& [sw, h] : ops) {
		CAPTURE(static_cast<int>(h));
		CHECK_FALSE(plot.handleForTest(h).has_value()); // a fresh sound: every operator off
		f.set(drawerParam(0, sw), 1.0);
		f.update();
		CHECK(plot.handleForTest(h).has_value());
	}
	// LINKED (the default), Formant Shift's high arrow and crossover are inert, so they are not drawn.
	CHECK_FALSE(plot.handleForTest(Handle::ShiftHigh).has_value());
	CHECK_FALSE(plot.handleForTest(Handle::Crossover).has_value());
	f.set(drawerParam(0, SpectralControl::FormantShiftLink), 0.0);
	f.update();
	CHECK(plot.handleForTest(Handle::ShiftHigh).has_value());
	CHECK(plot.handleForTest(Handle::Crossover).has_value());
	// THE MASTER SWITCH OFF: none.
	f.bridge.setSpectralOn(false);
	f.update();
	for (size_t i = 0; i < static_cast<size_t>(Handle::kCount); ++i) {
		CHECK_FALSE(plot.handleForTest(static_cast<Handle>(i)).has_value());
	}
	f.bridge.setSpectralOn(true);
	// A PARKED SOUND: its drawer does nothing, so neither does its plot's handles.
	f.set(ParamId::AEnable, 0.0);
	f.update();
	CHECK_FALSE(plot.handleForTest(Handle::Filter).has_value());
	// AND A PRESS WHERE A HANDLE WOULD BE MOVES NOTHING.
	const double cutoff = drawerValue(f, SpectralControl::FilterCutoff);
	plot.pressForTest({ plot.hzToX(std::exp2(cutoff)), plot.dbToY(-6.0) });
	plot.dragForTest({ plot.hzToX(300.0), plot.dbToY(0.0) });
	plot.releaseForTest();
	CHECK(drawerValue(f, SpectralControl::FilterCutoff) == cutoff);
}

TEST_CASE("spectrum plot: each handle sits at its control's value, and a drag writes the control back in the units it tracks") {
	SpectrumFixture f;
	constexpr double kF0 = 220.0;
	ui::SpectrumPlot& plot = openPlot(f, kF0);
	SUBCASE("the filter corner: Key Track off, Hz") {
		f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
		f.set(drawerParam(0, SpectralControl::FilterCutoff), std::log2(1500.0));
		f.update();
		const auto at = plot.handleForTest(Handle::Filter);
		REQUIRE(at.has_value());
		CHECK(at->x == doctest::Approx(plot.hzToX(1500.0)));
		CHECK(at->y == doctest::Approx(plot.dbToY(ui::SpectrumPlot::kFilterBaseDb)));
		CHECK(plot.labelForTest(Handle::Filter) == "LP 1.5 kHz");
		dragHandle(f, plot, Handle::Filter, { plot.hzToX(800.0), plot.dbToY(-6.0 + 0.5 * 14.0) });
		CHECK(drawerValue(f, SpectralControl::FilterCutoff) == doctest::Approx(std::log2(800.0)).epsilon(1e-4));
		CHECK(drawerValue(f, SpectralControl::FilterResonance) == doctest::Approx(0.5).epsilon(1e-4));
	}
	SUBCASE("the filter corner: Key Track on, harmonics, and the pointer is the TRACKED corner") {
		f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
		f.set(drawerParam(0, SpectralControl::FilterKeyTrack), 1.0);
		f.set(drawerParam(0, SpectralControl::FilterCutoff), std::log2(1000.0));
		f.update();
		const double tracked = 1000.0 * kF0 / feathers::spectral::kKeyTrackPivotHz;
		const auto at = plot.handleForTest(Handle::Filter);
		REQUIRE(at.has_value());
		CHECK(at->x == doctest::Approx(plot.hzToX(tracked)));
		char want[32];
		std::snprintf(want, sizeof(want), "LP \xc3\x97%.1f", tracked / kF0);
		CHECK(plot.labelForTest(Handle::Filter) == want);
		dragHandle(f, plot, Handle::Filter, { plot.hzToX(kF0 * 8.0), at->y });
		// the corner lands at harmonic 8: the param is that, before Key Track moves it
		CHECK(drawerValue(f, SpectralControl::FilterCutoff)
		      == doctest::Approx(std::log2(kF0 * 8.0) - std::log2(kF0 / feathers::spectral::kKeyTrackPivotHz)).epsilon(1e-4));
	}
	SUBCASE("the comb tooth: harmonics across, depth down") {
		f.set(drawerParam(0, SpectralControl::CombOn), 1.0);
		f.update();
		const auto at = plot.handleForTest(Handle::Comb);
		REQUIRE(at.has_value());
		CHECK(at->x == doctest::Approx(plot.hzToX(kF0 * drawerValue(f, SpectralControl::CombSpacing))));
		dragHandle(f, plot, Handle::Comb, { plot.hzToX(kF0 * 4.0), plot.dbToY(-15.0) });
		CHECK(drawerValue(f, SpectralControl::CombSpacing) == doctest::Approx(4.0).epsilon(1e-4));
		CHECK(drawerValue(f, SpectralControl::CombDepth) == doctest::Approx(0.5).epsilon(1e-4));
		CHECK(plot.labelForTest(Handle::Comb) == "comb \xc3\x97" "4.0");
	}
	SUBCASE("Formant Shift: the arrows' distance from their anchors in semitones, the crossover in Hz") {
		f.set(drawerParam(0, SpectralControl::FormantShiftOn), 1.0);
		f.update();
		dragHandle(f, plot, Handle::ShiftLow,
		           { plot.hzToX(ui::SpectrumPlot::kShiftAnchorHz * std::exp2(5.0 / 12.0)), plot.dbToY(4.0) });
		CHECK(drawerValue(f, SpectralControl::FormantShiftLow) == doctest::Approx(5.0).epsilon(1e-3));
		f.settleGesture();
		f.set(drawerParam(0, SpectralControl::FormantShiftLink), 0.0);
		f.update();
		dragHandle(f, plot, Handle::Crossover, { plot.hzToX(2000.0), plot.dbToY(-42.0) });
		CHECK(drawerValue(f, SpectralControl::FormantShiftCrossover) == doctest::Approx(std::log2(2000.0)).epsilon(1e-4));
		dragHandle(f, plot, Handle::ShiftHigh, { plot.hzToX(4000.0 * std::exp2(-7.0 / 12.0)), plot.dbToY(4.0) });
		CHECK(drawerValue(f, SpectralControl::FormantShiftHigh) == doctest::Approx(-7.0).epsilon(1e-3));
		CHECK(drawerValue(f, SpectralControl::FormantShiftLow) == doctest::Approx(5.0).epsilon(1e-3)); // untouched
	}
	SUBCASE("Tilt: a grip on its compensated line's end, its height the amount, and no pivot") {
		f.set(drawerParam(0, SpectralControl::TiltOn), 1.0);
		f.set(drawerParam(0, SpectralControl::TiltAmount), 2.0);
		f.update();
		const auto target = plot.handleForTest(Handle::Tilt); // where +2 dB/oct puts the grip (still on the plot)
		REQUIRE(target.has_value());
		f.set(drawerParam(0, SpectralControl::TiltAmount), 0.0);
		f.update();
		const auto rest = plot.handleForTest(Handle::Tilt);
		REQUIRE(rest.has_value());
		CHECK(rest->x == doctest::Approx(target->x)); // the line's right end, whatever the amount
		CHECK(rest->y > target->y + 5.0f);            // not vacuous: +2 dB/oct lifts the end
		dragHandle(f, plot, Handle::Tilt, { rest->x, target->y });
		CHECK(drawerValue(f, SpectralControl::TiltAmount) == doctest::Approx(2.0).epsilon(1e-2));
		CHECK(plot.labelForTest(Handle::Tilt) == "+2.0 dB/oct");
		// AND A SECOND PIN, below zero, so the bisection is held at two points: the grip where −3 dB/oct puts it, reached
		// by a drag from +2.
		f.settleGesture();
		f.set(drawerParam(0, SpectralControl::TiltAmount), -3.0);
		f.update();
		const auto low = plot.handleForTest(Handle::Tilt);
		REQUIRE(low.has_value());
		REQUIRE(low->y > rest->y + 5.0f);
		f.set(drawerParam(0, SpectralControl::TiltAmount), 2.0);
		f.update();
		dragHandle(f, plot, Handle::Tilt, { rest->x, low->y });
		CHECK(drawerValue(f, SpectralControl::TiltAmount) == doctest::Approx(-3.0).epsilon(1e-2));
	}
}

TEST_CASE("spectrum plot: the vowel, Shift and Partials handles — each drawn while its operator is on, at its value, and a drag writes it back") {
	// SPECTRAL-OPS, 2026-10-12: the vowel's handle walks PhiVox's anchors evenly across the plot (across = Vowel) at a height
	// set by Depth (up = Depth); Shift's marker is the fundamental moved by the shift, in Hz; the Partials cut sits between
	// the last partial kept and the first cut, in harmonics.
	SpectrumFixture f;
	constexpr double kF0 = 220.0;
	ui::SpectrumPlot& plot = openPlot(f, kF0);
	for (const Handle h : { Handle::Vowel, Handle::ShiftMarker, Handle::PartialsCut }) {
		CHECK_FALSE(plot.handleForTest(h).has_value()); // every operator off on a fresh sound
	}
	SUBCASE("the vowel: across walks the anchors, up is Depth, and at Depth 0 across moves nothing") {
		f.set(drawerParam(0, SpectralControl::FormantOn), 1.0);
		f.set(drawerParam(0, SpectralControl::FormantVowel), 3.0);
		f.set(drawerParam(0, SpectralControl::FormantDepth), 0.5);
		f.update();
		const auto at = plot.handleForTest(Handle::Vowel);
		REQUIRE(at.has_value());
		CHECK(at->x == doctest::Approx(plot.vowelToX(3.0)));
		CHECK(at->y == doctest::Approx(plot.dbToY(ui::SpectrumPlot::kVowelBaseDb + 0.5 * ui::SpectrumPlot::kVowelSpanDb)));
		CHECK(plot.labelForTest(Handle::Vowel) == "Open 3.0");
		CHECK(plot.verticalForTest(Handle::Vowel));
		dragHandle(f, plot, Handle::Vowel, { plot.vowelToX(5.0), plot.dbToY(ui::SpectrumPlot::kVowelBaseDb + 0.25 * ui::SpectrumPlot::kVowelSpanDb) });
		CHECK(drawerValue(f, SpectralControl::FormantVowel) == doctest::Approx(5.0).epsilon(1e-3));
		CHECK(drawerValue(f, SpectralControl::FormantDepth) == doctest::Approx(0.25).epsilon(1e-3));
		CHECK(plot.labelForTest(Handle::Vowel) == "Nasal 5.0");
		f.settleGesture();
		f.set(drawerParam(0, SpectralControl::FormantDepth), 0.0);
		f.update();
		// Across, held below the Depth-0 line (Depth stays 0): the vowel is inert there and does not move.
		dragHandle(f, plot, Handle::Vowel, { plot.vowelToX(1.0), plot.dbToY(ui::SpectrumPlot::kVowelBaseDb - 5.0) });
		CHECK(drawerValue(f, SpectralControl::FormantVowel) == doctest::Approx(5.0).epsilon(1e-6));
		CHECK(drawerValue(f, SpectralControl::FormantDepth) == 0.0);
		f.settleGesture();
		// Across AND up from Depth 0: the drag lifts Depth first, which brings the vowel's axis alive within the same move
		// (inertness is decided on every move, not at the press).
		dragHandle(f, plot, Handle::Vowel, { plot.vowelToX(1.0), plot.dbToY(ui::SpectrumPlot::kVowelBaseDb + 0.5 * ui::SpectrumPlot::kVowelSpanDb) });
		CHECK(drawerValue(f, SpectralControl::FormantVowel) == doctest::Approx(1.0).epsilon(1e-3));
		CHECK(drawerValue(f, SpectralControl::FormantDepth) == doctest::Approx(0.5).epsilon(1e-3));
	}
	SUBCASE("Shift's marker: the fundamental moved, in Hz") {
		f.set(drawerParam(0, SpectralControl::ShiftOn), 1.0);
		f.set(drawerParam(0, SpectralControl::ShiftHz), 50.0);
		f.update();
		const auto at = plot.handleForTest(Handle::ShiftMarker);
		REQUIRE(at.has_value());
		CHECK(at->x == doctest::Approx(plot.hzToX(kF0 + 50.0)));
		CHECK(plot.labelForTest(Handle::ShiftMarker) == "+50.0 Hz");
		CHECK_FALSE(plot.verticalForTest(Handle::ShiftMarker));
		dragHandle(f, plot, Handle::ShiftMarker, { plot.hzToX(kF0 - 120.0), at->y });
		CHECK(drawerValue(f, SpectralControl::ShiftHz) == doctest::Approx(-120.0).epsilon(1e-3));
		CHECK(plot.labelForTest(Handle::ShiftMarker) == "-120.0 Hz");
	}
	SUBCASE("the Partials cut: between the last kept and the first cut, in harmonics") {
		f.set(drawerParam(0, SpectralControl::PartialsOn), 1.0);
		f.set(drawerParam(0, SpectralControl::PartialsCount), 20.0);
		f.update();
		const auto at = plot.handleForTest(Handle::PartialsCut);
		REQUIRE(at.has_value());
		CHECK(at->x == doctest::Approx(plot.hzToX(kF0 * 20.5)));
		CHECK(plot.labelForTest(Handle::PartialsCut) == "20.0 partials");
		dragHandle(f, plot, Handle::PartialsCut, { plot.hzToX(kF0 * 10.5), at->y });
		CHECK(drawerValue(f, SpectralControl::PartialsCount) == doctest::Approx(10.0).epsilon(1e-3));
		// BETWEEN TWO WHOLE COUNTS THE DRAG IS CONTINUOUS: the log-Hz midpoint of the cuts for 10 and 11 writes 10.5, and the
		// line is drawn back there.
		f.settleGesture();
		const double mid = kF0 * std::sqrt(10.5 * 11.5);
		const auto now = plot.handleForTest(Handle::PartialsCut);
		REQUIRE(now.has_value());
		dragHandle(f, plot, Handle::PartialsCut, { plot.hzToX(mid), now->y });
		CHECK(drawerValue(f, SpectralControl::PartialsCount) == doctest::Approx(10.5).epsilon(1e-3));
		const auto back = plot.handleForTest(Handle::PartialsCut);
		REQUIRE(back.has_value());
		CHECK(back->x == doctest::Approx(plot.hzToX(mid)).epsilon(1e-3));
	}
	SUBCASE("the Partials cut under Shift +300 Hz: between the MOVED partials, and a drag inverts it there") {
		f.set(drawerParam(0, SpectralControl::PartialsOn), 1.0);
		f.set(drawerParam(0, SpectralControl::PartialsCount), 20.0);
		f.set(drawerParam(0, SpectralControl::ShiftOn), 1.0);
		f.set(drawerParam(0, SpectralControl::ShiftHz), 300.0);
		f.update();
		const auto at = plot.handleForTest(Handle::PartialsCut);
		REQUIRE(at.has_value());
		const double off = static_cast<float>(300.0 / kF0); // the shift on the ratio, as the voice computes it
		CHECK(at->x == doctest::Approx(plot.hzToX(kF0 * 0.5 * ((20.0 + off) + (21.0 + off)))).epsilon(1e-4));
		dragHandle(f, plot, Handle::PartialsCut, { plot.hzToX(kF0 * (10.5 + off)), at->y });
		CHECK(drawerValue(f, SpectralControl::PartialsCount) == doctest::Approx(10.0).epsilon(1e-6));
	}
	SUBCASE("the Partials cut under Stretch: between the stretched partials") {
		f.set(drawerParam(0, SpectralControl::PartialsOn), 1.0);
		f.set(drawerParam(0, SpectralControl::PartialsCount), 20.0);
		f.set(drawerParam(0, SpectralControl::StretchOn), 1.0);
		f.set(drawerParam(0, SpectralControl::StretchAmount), 0.5);
		f.update();
		const auto at = plot.handleForTest(Handle::PartialsCut);
		REQUIRE(at.has_value());
		const float b = static_cast<float>(feathers::spectral::StretchOp::bOf(0.5));
		const auto stretched = [b](float k) { return static_cast<double>(k * std::sqrt(1.0f + b * k * k)); };
		CHECK(at->x == doctest::Approx(plot.hzToX(kF0 * 0.5 * (stretched(20.0f) + stretched(21.0f)))).epsilon(1e-4));
		CHECK(std::abs(at->x - plot.hzToX(kF0 * 20.5)) > 10.0f); // not vacuous: the harmonic place is elsewhere
		dragHandle(f, plot, Handle::PartialsCut, { plot.hzToX(kF0 * 0.5 * (stretched(12.0f) + stretched(13.0f))), at->y });
		CHECK(drawerValue(f, SpectralControl::PartialsCount) == doctest::Approx(12.0).epsilon(1e-6));
	}
}

TEST_CASE("spectrum plot: under Notch the filter corner moves across and not up — Resonance changes nothing there") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f);
	f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
	f.set(drawerParam(0, SpectralControl::FilterResonance), 0.3);
	f.set(drawerParam(0, SpectralControl::FilterMode), 3.0); // Notch
	f.update();
	CHECK_FALSE(plot.verticalForTest(Handle::Filter));
	const auto at = plot.handleForTest(Handle::Filter);
	REQUIRE(at.has_value());
	CHECK(at->y == doctest::Approx(plot.dbToY(ui::SpectrumPlot::kFilterBaseDb))); // its height says nothing
	dragHandle(f, plot, Handle::Filter, { plot.hzToX(600.0), plot.dbToY(10.0) });
	CHECK(drawerValue(f, SpectralControl::FilterCutoff) == doctest::Approx(std::log2(600.0)).epsilon(1e-4));
	CHECK(drawerValue(f, SpectralControl::FilterResonance) == 0.3);
	// NOT VACUOUS: in a mode with a resonance, the same drag moves it.
	f.settleGesture();
	f.set(drawerParam(0, SpectralControl::FilterMode), 0.0);
	f.update();
	CHECK(plot.verticalForTest(Handle::Filter));
	dragHandle(f, plot, Handle::Filter, { plot.hzToX(600.0), plot.dbToY(-6.0 + 14.0) });
	CHECK(drawerValue(f, SpectralControl::FilterResonance) == doctest::Approx(1.0).epsilon(1e-4));
}

TEST_CASE("spectrum plot: a stroke paints every band it crosses, the floor silences a band exactly, and a stroke is one undo step") {
	SpectrumFixture f;
	f.stage.setModulation(&f.mod, &f.history);
	f.panel.setModulation(&f.mod, &f.history);
	constexpr double kF0 = 200.0;
	ui::SpectrumPlot& plot = openPlot(f, kF0);
	ui::SpectrumPanel& d = f.panel;
	// The curve on and the Draw tool chosen (the off-curve stroke has its own case).
	d.curveSwitchForTest(0).toggleForTest();
	d.drawToolForTest().toggleForTest();
	f.update();
	REQUIRE(d.drawToolOnForTest());
	const size_t stepsBefore = f.history.length();
	const feathers::spectral::DrawnCurve before = f.bridge.spectralCurve(0);

	// ONE STROKE from harmonic 3 at −12 dB to harmonic 11 at the floor, in a single move: every band between is painted
	// at the height the line crosses it, and the last is EXACTLY zero.
	const visage::Point a { plot.hzToX(kF0 * 3.0), plot.dbToY(-12.0) };
	const visage::Point b { plot.hzToX(kF0 * 11.0), plot.dbToY(-44.0) };
	plot.pressForTest(a);
	plot.dragForTest(b);
	plot.releaseForTest();
	const feathers::spectral::DrawnCurve after = f.bridge.spectralCurve(0);
	for (size_t band = 2; band <= 10; ++band) {
		CAPTURE(band);
		const float x = plot.hzToX(kF0 * static_cast<double>(band + 1));
		// THE LINE AT THE BAND, in the order the stroke computes it (the fraction along the segment, then the height), so the
		// float matches to the bit.
		const float t = std::clamp((x - a.x) / (b.x - a.x), 0.0f, 1.0f);
		const float y = a.y + (b.y - a.y) * t;
		CHECK(after.gain[band] == plot.curveGainAtY(y));
	}
	CHECK(after.gain[10] == 0.0f); // drawn out: silence
	CHECK(after.gain[2] == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(1e-3));
	CHECK(after.gain[1] == 1.0f); // outside the stroke, untouched
	CHECK(after.gain[11] == 1.0f);
	int zeros = 0;
	for (size_t band = 2; band <= 10; ++band) {
		zeros += after.gain[band] == 0.0f ? 1 : 0;
	}
	CHECK(zeros >= 1);
	CHECK(zeros < 9); // not vacuous: the line descends through audible bands to the floor

	// ONE STEP: one undo restores the curve before the stroke, one redo the stroke.
	CHECK(f.history.length() == stepsBefore + 1);
	f.history.undo();
	CHECK(f.bridge.spectralCurve(0) == before);
	f.history.redo();
	CHECK(f.bridge.spectralCurve(0) == after);
}

TEST_CASE("spectrum plot: a stroke on an off curve switches it on, and one undo takes back the bands and the switch together") {
	SpectrumFixture f;
	f.stage.setModulation(&f.mod, &f.history);
	f.panel.setModulation(&f.mod, &f.history);
	constexpr double kF0 = 200.0;
	ui::SpectrumPlot& plot = openPlot(f, kF0);
	ui::SpectrumPanel& d = f.panel;
	REQUIRE_FALSE(f.bridge.spectralCurve(0).on);
	// THE DRAW TOOL IS LIVE WITH THE CURVE OFF (live sound, master on), as the sketch's is.
	CHECK(d.drawToolForTest().enabled());
	d.drawToolForTest().toggleForTest();
	f.update();
	const feathers::spectral::DrawnCurve before = f.bridge.spectralCurve(0);
	const size_t steps = f.history.length();
	plot.pressForTest({ plot.hzToX(kF0 * 4.0), plot.dbToY(-18.0) });
	plot.dragForTest({ plot.hzToX(kF0 * 6.0), plot.dbToY(-18.0) });
	plot.releaseForTest();
	const feathers::spectral::DrawnCurve after = f.bridge.spectralCurve(0);
	CHECK(after.on);
	for (size_t band = 3; band <= 5; ++band) {
		CAPTURE(band);
		CHECK(after.gain[band] == plot.curveGainAtY(plot.dbToY(-18.0)));
	}
	CHECK(after.gain[1] == 1.0f); // outside the stroke, untouched
	CHECK(f.history.length() == steps + 1); // the switch and the bands: ONE step
	f.history.undo();
	CHECK(f.bridge.spectralCurve(0) == before); // off again, and flat
	CHECK_FALSE(f.bridge.spectralCurve(0).on);
	f.history.redo();
	CHECK(f.bridge.spectralCurve(0) == after);
}

TEST_CASE("spectrum plot: Tilt's line is the shaper's compensated tilt over the frame's terrain, and its grip rides the line's right end") {
	namespace sp = feathers::spectral;
	SpectrumFixture f;
	constexpr double kF0 = 220.0;
	ui::SpectrumPlot& plot = openPlot(f, kF0);
	f.set(drawerParam(0, SpectralControl::TiltOn), 1.0);
	f.set(drawerParam(0, SpectralControl::TiltAmount), -6.0);
	f.update();
	const feathers::SpectralPlotFrame& fr = plot.frameForTest();
	REQUIRE(fr.count == 40);
	// THE REFERENCE IS THE ENGINE'S OWN CALL, made here over the same terrain: what a voice's drawer applies for Tilt alone.
	sp::ShaperParams p;
	p.tilt.on = true;
	p.tilt.dbPerOctave = -6.0f;
	std::array<float, feathers::SpectralPlotFrame::kPartials> k {}, hz {}, want {}, line {};
	for (int i = 0; i < fr.count; ++i) {
		k[static_cast<size_t>(i)] = static_cast<float>(i + 1);
		hz[static_cast<size_t>(i)] = static_cast<float>(kF0 * fr.ratio[static_cast<size_t>(i)]);
	}
	sp::gains(sp::prepare(p, kF0), k.data(), hz.data(), fr.terrain.data(), fr.count, want.data());
	REQUIRE(plot.tiltLine(-6.0, line.data()) == fr.count);
	double before = 0.0;
	double afterPower = 0.0;
	int compensated = 0;
	for (int i = 0; i < fr.count; ++i) {
		const auto u = static_cast<size_t>(i);
		CAPTURE(i);
		CHECK(line[u] == want[u]);
		before += double(fr.terrain[u]) * fr.terrain[u];
		afterPower += double(fr.terrain[u] * line[u]) * (fr.terrain[u] * line[u]);
		compensated += std::abs(line[u] - sp::tiltGain(p.tilt, hz[u])) > 1e-3f * line[u] ? 1 : 0;
	}
	// NOT VACUOUS: the level is held (the compensation), so the line is not Tilt's bare shape.
	CHECK(afterPower == doctest::Approx(before).epsilon(1e-4));
	CHECK(compensated == fr.count);
	// THE GRIP: the right end of that line — the highest partial, harmonic 40 at 8.8 kHz.
	const auto grip = plot.handleForTest(Handle::Tilt);
	REQUIRE(grip.has_value());
	CHECK(grip->x == doctest::Approx(plot.hzToX(kF0 * 40.0)));
	CHECK(grip->y == doctest::Approx(plot.dbToY(20.0 * std::log10(double(line[39])))));
}

TEST_CASE("spectrum plot: the plot says which note it shows — the note's name while one plays, the reference's otherwise") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.update();
	ui::SpectrumPlot& plot = f.panel.plotForTest();
	CHECK(plot.noteLabel().empty()); // no frame yet
	(void)f.bridge.publishPlot(plotFrameAt(220.0)); // playing, MIDI 57
	f.update();
	CHECK(plot.noteLabel() == "A2"); // the reference's naming: 60 is C3
	feathers::SpectralPlotFrame reference = plotFrameAt(440.0 * std::exp2((48.0 - 69.0) / 12.0));
	reference.playing = 0;
	(void)f.bridge.publishPlot(reference);
	f.update();
	CHECK(plot.noteLabel() == "reference \xc2\xb7 C2");
}

TEST_CASE("spectrum plot: a handle drag opens exactly the gestures of the controls it moves — none on a bare click — and closes them all on release") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f);
	std::map<clap_id, int> begins;
	std::map<clap_id, int> ends;
	const auto take = [&] {
		begins.clear();
		ends.clear();
		f.bridge.drain([&](const UiParamEvent& e) {
			if (e.kind == UiParamEvent::Kind::GestureBegin) {
				++begins[e.id];
			}
			if (e.kind == UiParamEvent::Kind::GestureEnd) {
				++ends[e.id];
			}
		});
	};
	const auto id = [](SpectralControl c) { return static_cast<clap_id>(drawerParam(0, c)); };
	f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
	f.set(drawerParam(0, SpectralControl::CombOn), 1.0);
	f.update();
	take();
	SUBCASE("a bare click opens nothing") {
		const auto at = plot.handleForTest(Handle::Filter);
		REQUIRE(at.has_value());
		plot.pressForTest(*at);
		plot.releaseForTest();
		take();
		CHECK(begins.empty());
		CHECK(ends.empty());
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("the filter corner: cutoff and resonance, once each") {
		const auto at = plot.handleForTest(Handle::Filter);
		REQUIRE(at.has_value());
		plot.pressForTest(*at);
		plot.dragForTest({ at->x - 40.0f, at->y - 20.0f });
		plot.dragForTest({ at->x - 60.0f, at->y - 30.0f });
		CHECK(f.bridge.openGestureCountForTest() == 2);
		plot.releaseForTest();
		take();
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::FilterCutoff), 1 }, { id(SpectralControl::FilterResonance), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("a drag that moves only the cutoff opens only the cutoff") {
		// Resonance at 0 and the pointer dragged BELOW the corner: the height asks for less than 0, which is 0 — the value
		// it has — so its gesture never opens, as a knob that has not moved opens none.
		REQUIRE(f.bridge.value(id(SpectralControl::FilterResonance)) == 0.0);
		const auto at = plot.handleForTest(Handle::Filter);
		REQUIRE(at.has_value());
		plot.pressForTest(*at);
		plot.dragForTest({ at->x - 50.0f, at->y + 40.0f });
		plot.releaseForTest();
		take();
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::FilterCutoff), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("the filter corner under Notch: cutoff alone") {
		f.set(drawerParam(0, SpectralControl::FilterMode), 3.0);
		f.update();
		take();
		const auto at = plot.handleForTest(Handle::Filter);
		REQUIRE(at.has_value());
		plot.pressForTest(*at);
		plot.dragForTest({ at->x + 40.0f, at->y - 30.0f });
		plot.releaseForTest();
		take();
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::FilterCutoff), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("the comb tooth: spacing and depth") {
		const auto at = plot.handleForTest(Handle::Comb);
		REQUIRE(at.has_value());
		plot.pressForTest(*at);
		plot.dragForTest({ at->x + 30.0f, at->y + 25.0f });
		plot.releaseForTest();
		take();
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::CombSpacing), 1 }, { id(SpectralControl::CombDepth), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	// THE LATER FIVE'S HANDLES (spectral-ops, 2026-10-12). Each case switches the others' operators off, so no other handle
	// is in reach of the press.
	const auto only = [&](SpectralControl sw) {
		f.set(drawerParam(0, SpectralControl::FilterOn), 0.0);
		f.set(drawerParam(0, SpectralControl::CombOn), 0.0);
		f.set(drawerParam(0, sw), 1.0);
		f.update();
		take();
	};
	const auto drag = [&](Handle h, float dx, float dy) {
		const auto at = plot.handleForTest(h);
		REQUIRE(at.has_value());
		plot.pressForTest(*at);
		plot.dragForTest({ at->x + dx, at->y + dy });
		plot.releaseForTest();
		take();
	};
	SUBCASE("the vowel: Vowel and Depth") {
		only(SpectralControl::FormantOn);
		drag(Handle::Vowel, 60.0f, -20.0f);
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::FormantVowel), 1 }, { id(SpectralControl::FormantDepth), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("the vowel at Depth 0, dragged across below its line: nothing (the vowel is inert, Depth stays 0)") {
		f.set(drawerParam(0, SpectralControl::FormantDepth), 0.0);
		only(SpectralControl::FormantOn);
		drag(Handle::Vowel, 80.0f, 30.0f);
		CHECK(begins.empty());
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("the vowel at Depth 0, dragged straight up: only Depth") {
		f.set(drawerParam(0, SpectralControl::FormantDepth), 0.0);
		only(SpectralControl::FormantOn);
		drag(Handle::Vowel, 0.0f, -40.0f);
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::FormantDepth), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("Shift's marker: Shift Hz alone") {
		only(SpectralControl::ShiftOn);
		drag(Handle::ShiftMarker, 40.0f, -30.0f);
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::ShiftHz), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
	SUBCASE("the Partials cut: the count alone") {
		only(SpectralControl::PartialsOn);
		drag(Handle::PartialsCut, -60.0f, -30.0f);
		CHECK(begins == std::map<clap_id, int> { { id(SpectralControl::PartialsCount), 1 } });
		CHECK(ends == begins);
		CHECK(f.bridge.openGestureCountForTest() == 0);
	}
}

TEST_CASE("spectrum plot: a handle drag repaints the plot even when nothing heard changes and no frame is published") {
	// THE CROSSOVER WITH BOTH SHIFTS AT ZERO changes nothing heard, so the engine would publish nothing new: the plot must
	// repaint for the drag itself, or the handle and its label freeze where the press found them.
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f);
	f.set(drawerParam(0, SpectralControl::FormantShiftOn), 1.0);
	f.set(drawerParam(0, SpectralControl::FormantShiftLink), 0.0);
	f.update();
	const auto at = plot.handleForTest(Handle::Crossover);
	REQUIRE(at.has_value());
	const uint32_t serial = f.bridge.plotSerial();
	const int before = plot.redrawsForTest();
	plot.pressForTest(*at);
	plot.dragForTest({ at->x + 30.0f, at->y });
	CHECK(plot.redrawsForTest() == before + 1);
	plot.dragForTest({ at->x + 60.0f, at->y });
	CHECK(plot.redrawsForTest() == before + 2);
	plot.releaseForTest();
	CHECK(f.bridge.plotSerial() == serial); // no frame was published: the repaints were the drag's own
	const auto moved = plot.handleForTest(Handle::Crossover);
	REQUIRE(moved.has_value());
	CHECK(moved->x == doctest::Approx(at->x + 60.0f).epsilon(1e-4)); // and the handle is where the hand took it
}

TEST_CASE("spectrum plot: an off-centre press moves the handle by the drag, and never jumps it to the pointer") {
	SpectrumFixture f;
	constexpr double kF0 = 220.0;
	ui::SpectrumPlot& plot = openPlot(f, kF0);
	f.set(drawerParam(0, SpectralControl::CombOn), 1.0);
	f.update();
	const auto at = plot.handleForTest(Handle::Comb);
	REQUIRE(at.has_value());
	const visage::Point press { at->x + 6.0f, at->y - 4.0f }; // inside the grab, off the centre
	plot.pressForTest(press);
	plot.dragForTest({ press.x + 40.0f, press.y + 20.0f });
	plot.releaseForTest();
	// THE HANDLE MOVED BY (40, 20) FROM WHERE IT WAS, not to the pointer.
	CHECK(drawerValue(f, SpectralControl::CombSpacing) == doctest::Approx(plot.xToHz(at->x + 40.0f) / kF0).epsilon(1e-4));
	CHECK(drawerValue(f, SpectralControl::CombDepth) == doctest::Approx(-plot.yToDb(at->y + 20.0f) / 30.0).epsilon(1e-4));
}

TEST_CASE("spectrum plot: with fewer than two partials on the axis Tilt has no grip, since its line has no slope to set") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	f.selection.select(0);
	f.set(drawerParam(0, SpectralControl::TiltOn), 1.0);
	(void)f.bridge.publishPlot(plotFrameAt(220.0, 1));
	f.update();
	ui::SpectrumPlot& plot = f.panel.plotForTest();
	REQUIRE(plot.frameForTest().count == 1);
	CHECK_FALSE(plot.handleForTest(Handle::Tilt).has_value());
	// A press where a grip could be moves nothing.
	const double amount = drawerValue(f, SpectralControl::TiltAmount);
	plot.pressForTest({ plot.hzToX(220.0), plot.dbToY(0.0) });
	plot.dragForTest({ plot.hzToX(220.0), plot.dbToY(10.0) });
	plot.releaseForTest();
	CHECK(drawerValue(f, SpectralControl::TiltAmount) == amount);
	// NOT VACUOUS: two partials, and the grip is there.
	(void)f.bridge.publishPlot(plotFrameAt(220.0, 2));
	f.update();
	CHECK(plot.handleForTest(Handle::Tilt).has_value());
}

TEST_CASE("spectrum plot: a reopened panel draws no frame published before it opened, only the engine's next one") {
	// EVERY OPEN IS A FRESH PANEL (the editor builds one per open), and the bridge still holds the frame the last one saw —
	// a note that may have ended long ago. So the new panel does not take it, and its opening asks for the flush that
	// publishes a fresh one; that one, though equal to the old, goes out under a new serial and is drawn.
	SpectrumFixture f;
	ui::SpectrumPlot& first = openPlot(f);
	REQUIRE(first.noteLabel() == "A2");
	REQUIRE(f.bridge.plotWanted());
	auto closed = std::make_unique<ui::SpectrumPanel>(f.bridge, f.registry, f.selection, &f.tray);
	(void)closed->updateContent();
	closed.reset(); // a panel closed: the plot is not wanted
	CHECK_FALSE(f.bridge.plotWanted());
	f.bridge.clearSpectralMoved(); // the flush the first panel asked for has run
	auto reopened = std::make_unique<ui::SpectrumPanel>(f.bridge, f.registry, f.selection, &f.tray);
	reopened->setBounds(0.0f, 0.0f, ui::SpectrumPanel::kMinimumWidth, ui::SpectrumPanel::kMinimumHeight);
	(void)reopened->updateContent();
	ui::SpectrumPlot& plot = reopened->plotForTest();
	CHECK(f.bridge.plotWanted());
	CHECK(plot.frameForTest().count == 0); // the bridge's frame is older than this panel
	CHECK(plot.noteLabel().empty());
	CHECK(f.bridge.hasPendingWork()); // the opening asked for the flush that publishes a fresh one
	CHECK(f.bridge.publishPlot(plotFrameAt(220.0))); // the fresh one, equal to the old, republished
	(void)reopened->updateContent();
	CHECK(plot.frameForTest().count == 40);
	CHECK(plot.noteLabel() == "A2");
}

TEST_CASE("spectrum panel: a change of selected sound moves the panel at once — its header and its operator set — and closes a gesture open on the plot first") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f);
	f.set(ParamId::BEnable, 1.0);
	f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
	f.update();
	REQUIRE(f.panel.headerTitle() == "Spectrum \xe2\x80\x94 Sound A");
	// A HAND ON SOUND A'S FILTER CORNER, mid-drag: the host has its cutoff gesture open.
	const auto at = plot.handleForTest(Handle::Filter);
	REQUIRE(at.has_value());
	plot.pressForTest(*at);
	plot.dragForTest({ at->x - 40.0f, at->y });
	REQUIRE(f.bridge.openGestureCountForTest() > 0);
	f.selection.select(1);
	f.update();
	// CLOSED FIRST: no gesture left open on a sound the panel no longer shows, and a further drag moves nothing.
	CHECK(f.bridge.openGestureCountForTest() == 0);
	const double cutoffB = drawerValue(f, SpectralControl::FilterCutoff, 1);
	plot.dragForTest({ at->x - 80.0f, at->y });
	CHECK(drawerValue(f, SpectralControl::FilterCutoff, 1) == cutoffB);
	// AND THE PANEL IS SOUND B'S AT ONCE: the header names it, and B's set is the one on the face.
	CHECK(f.panel.selected() == 1u);
	CHECK(f.panel.headerTitle() == "Spectrum \xe2\x80\x94 Sound B");
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		CHECK(f.panel.knobForTest(slot, SpectralControl::FilterCutoff)->isVisible() == (slot == 1));
	}
}

TEST_CASE("spectrum panel: Flatten, a curve's switch and the master switch are each one undo step") {
	SpectrumFixture f;
	f.stage.setModulation(&f.mod, &f.history);
	f.panel.setModulation(&f.mod, &f.history);
	openPlot(f);
	ui::SpectrumPanel& d = f.panel;
	size_t steps = f.history.length();
	d.curveSwitchForTest(0).toggleForTest();
	CHECK(f.history.length() == ++steps);
	f.bridge.setSpectralCurveBand(0, 4, 0.25f);
	f.update();
	const feathers::spectral::DrawnCurve drawn = f.bridge.spectralCurve(0);
	d.flattenForTest();
	CHECK(f.history.length() == ++steps);
	CHECK(f.bridge.spectralCurve(0).gain == feathers::spectral::unityCurve());
	f.history.undo();
	CHECK(f.bridge.spectralCurve(0) == drawn);
	f.history.undo();
	CHECK_FALSE(f.bridge.spectralCurve(0).on);
	f.history.redo();
	CHECK(f.bridge.spectralCurve(0).on);
	f.stage.spectrumTabForTest().masterForTest().toggleForTest();
	CHECK_FALSE(f.bridge.spectralOn());
	f.history.undo();
	CHECK(f.bridge.spectralOn());
	// THE DRAW TOOL IS THE VIEWER'S, NOT THE PATCH'S: it records nothing.
	const size_t before = f.history.length();
	d.drawToolForTest().toggleForTest();
	CHECK(f.history.length() == before);
}

namespace {

/// THE WHOLE EDITOR, on Additive, its canvas folded so a panel lands on the overlay — the roads the panel opens and closes
/// by are the editor's (Gui::Impl::toggleSpectrumPanel), so they are asked of a real one.
struct SpectrumEditor : ModFixture {
	ui::SpectrumTab* tab = nullptr;
	ui::ExpandedOverlay* overlay = nullptr;
	SpectrumEditor() {
		engine(OscType::Additive);
		(void)gui.canvasForTest()->setCollapsed(true);
		tab = findInTree<ui::SpectrumTab>(root);
		overlay = gui.expandedForTest();
		REQUIRE(tab != nullptr);
		REQUIRE(overlay != nullptr);
		tick();
	}
	void engine(OscType type) {
		ParamSet params;
		params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(type));
		bridge.publish(params);
		bridge.refresh();
	}
	/// Additive and `values`, as a host would publish them.
	void publish(std::initializer_list<std::pair<ParamId, double>> values) {
		ParamSet params;
		params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Additive));
		for (const auto& [id, value] : values) {
			params.setBase(static_cast<clap_id>(id), value);
		}
		bridge.publish(params);
		bridge.refresh();
	}
	void tick() { (void)gui.tickForTest(); }
	[[nodiscard]] ui::SpectrumPanel* panel() { return gui.spectrumPanelForTest(); }
};

} // namespace

TEST_CASE("spectrum panel: the tab opens it on the editor's overlay, and Esc, the ✕ and the tab again each close it, un-lighting the caret — a press outside does not") {
	SpectrumEditor f;
	REQUIRE(f.panel() == nullptr);
	REQUIRE_FALSE(f.tab->caretLitForTest());
	f.tab->clickLabelForTest();
	f.tick();
	REQUIRE(f.panel() != nullptr);
	REQUIRE(f.overlay->open());
	ui::ExpandedPanel* window = f.overlay->panelForTest();
	REQUIRE(window != nullptr);
	CHECK(window->title() == "Spectrum \xe2\x80\x94 Sound A");
	// THE HEADER FOLLOWS THE SELECTED SOUND, through the overlay's own title strip (ExpandedView::headerTitle, re-read
	// every tick), not only the panel's answer.
	f.publish({ { ParamId::BEnable, 1.0 } });
	f.gui.selectionForTest()->select(1);
	f.tick();
	CHECK(window->title() == "Spectrum \xe2\x80\x94 Sound B");
	CHECK(f.tab->caretLitForTest());
	// ITS SIZE: at least its minimum, and inside the editor's canvas.
	CHECK(f.overlay->panelBounds().width() >= ui::SpectrumPanel::kMinimumWidth);
	CHECK(f.overlay->panelBounds().right() <= f.overlay->width() + 0.5f);
	SUBCASE("the ✕") {
		const visage::Bounds close = window->closeBounds();
		window->onMouseDown().callback(at(close.x() + close.width() * 0.5f, close.y() + close.height() * 0.5f));
		CHECK(f.overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::CloseButton);
	}
	SUBCASE("Escape") {
		const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
		CHECK(f.overlay->onKeyPress().callback(escape));
		CHECK(f.overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::Escape);
	}
	SUBCASE("the tab again") {
		f.tab->clickLabelForTest();
	}
	SUBCASE("a press outside is NOT a close: it reaches the editor beneath (the house rule for every window)") {
		const visage::Bounds b = f.overlay->panelBounds();
		const visage::Point outside { b.x() > 20.0f ? 10.0f : b.right() + 10.0f, f.root->height() - 10.0f };
		f.root->processMouseDown(at(outside.x, outside.y));
		f.root->processMouseUp(at(outside.x, outside.y));
		f.tick();
		CHECK(f.panel() != nullptr);
		CHECK(f.overlay->open());
		CHECK(f.tab->caretLitForTest());
		return;
	}
	f.tick();
	CHECK(f.panel() == nullptr);
	CHECK_FALSE(f.overlay->open());
	CHECK_FALSE(f.tab->caretLitForTest()); // the caret un-lights, whichever road closed it
}

TEST_CASE("spectrum panel: off Additive the tab's press opens nothing, at once — not a panel closed a tick later") {
	SpectrumEditor f;
	f.engine(OscType::V5);
	f.tick();
	f.tab->clickLabelForTest(); // the tab is hidden off Additive; a host or a case can still ask
	CHECK(f.panel() == nullptr); // before any tick
	CHECK_FALSE(f.overlay->open());
	CHECK_FALSE(f.bridge.plotWanted());
	// NOT VACUOUS: on Additive the same press opens it.
	f.engine(OscType::Additive);
	f.tab->clickLabelForTest();
	CHECK(f.panel() != nullptr);
}

TEST_CASE("spectrum panel: closing it mid-drag ends the host's gestures, whichever road closed it") {
	SpectrumEditor f;
	f.publish({ { ParamId::AEnable, 1.0 }, { spectralParam(0, SpectralControl::FilterOn), 1.0 } });
	f.gui.selectionForTest()->select(0);
	f.tab->clickLabelForTest();
	f.tick();
	ui::SpectrumPanel* panel = f.panel();
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, ui::SpectrumPanel::kMinimumWidth, ui::SpectrumPanel::kMinimumHeight);
	f.tick();
	ui::SpectrumPlot& plot = panel->plotForTest();
	const auto at = plot.handleForTest(Handle::Filter);
	REQUIRE(at.has_value());
	plot.pressForTest(*at);
	plot.dragForTest({ at->x - 40.0f, at->y - 20.0f });
	REQUIRE(f.bridge.openGestureCountForTest() > 0); // a hand on the corner: the host holds its gestures
	SUBCASE("Escape (the host's road)") {
		const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
		CHECK(f.overlay->onKeyPress().callback(escape));
	}
	SUBCASE("the tab (the editor's road)") {
		f.tab->clickLabelForTest();
	}
	SUBCASE("leaving Additive") {
		f.engine(OscType::V5);
		f.tick();
	}
	SUBCASE("the editor closing") {
		f.gui.destroy();
	}
	CHECK(f.panel() == nullptr);
	CHECK(f.bridge.openGestureCountForTest() == 0); // no gesture left with nobody to end it
}

TEST_CASE("spectrum panel: the window header's enable chip is the master switch, one undo step a flip") {
	SpectrumEditor f;
	f.tab->clickLabelForTest();
	f.tick();
	ui::SpectrumPanel* panel = f.panel();
	REQUIRE(panel != nullptr);
	REQUIRE(panel->hasDeviceEnable());
	CHECK(panel->deviceEnabled() == f.bridge.spectralOn());
	const bool before = f.bridge.spectralOn();
	const size_t steps = f.gui.history()->length();
	CHECK(panel->toggleDeviceEnable() == std::optional<bool> { !before });
	CHECK(f.bridge.spectralOn() == !before);
	CHECK(f.gui.history()->length() == steps + 1);
	f.gui.history()->undo();
	CHECK(f.bridge.spectralOn() == before);
	f.tick();
	CHECK(panel->deviceEnabled() == before); // the chip follows the field
}

TEST_CASE("spectrum panel: the engine's plot is wanted exactly while the panel is open — closing it, leaving Additive or closing the editor clears it") {
	SUBCASE("closed") {
		SpectrumEditor f;
		CHECK_FALSE(f.bridge.plotWanted());
		f.tab->clickLabelForTest();
		f.tick();
		CHECK(f.bridge.plotWanted());
		f.tab->clickLabelForTest();
		f.tick();
		CHECK_FALSE(f.bridge.plotWanted());
	}
	SUBCASE("leaving Additive closes the panel, by its host's road") {
		SpectrumEditor f;
		f.tab->clickLabelForTest();
		f.tick();
		REQUIRE(f.bridge.plotWanted());
		f.engine(OscType::V5);
		f.tick();
		CHECK(f.panel() == nullptr);
		CHECK_FALSE(f.overlay->open());
		CHECK_FALSE(f.bridge.plotWanted());
		CHECK_FALSE(f.tab->caretLitForTest());
	}
	SUBCASE("the editor closing") {
		SpectrumEditor f;
		f.tab->clickLabelForTest();
		f.tick();
		REQUIRE(f.bridge.plotWanted());
		f.gui.destroy();
		CHECK_FALSE(f.bridge.plotWanted());
	}
}

TEST_CASE("SHOT the spectrum panel on Additive: at its minimum, larger, narrow, and with the master switch off") {
	// NOT A CASE — A CAMERA, gated on FEATHERS_SHOT_DIR like the other SHOT cases. Run under `scripts/venue-xvfb.sh`
	// (VISAGE_FULL_REDRAW=1). The panel's CONTENT on the editor's 1240x700 canvas (the overlay's title strip and frame are
	// the overlay's, photographed with every device window). Writes spectrum_panel.png (its minimum), spectrum_panel_large.png,
	// spectrum_panel_narrow.png (a narrow resize: the row wraps) and spectrum_panel_off.png.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	SpectrumFixture f;
	constexpr int w = 1240;
	constexpr int h = 700;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	editor.addChild(&f.panel);
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	// A stereo frame, every handle on (Formant Shift unlinked), and a curve with a band drawn out.
	(void)f.bridge.publishPlot(plotFrameAt(130.0, 120, true));
	f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
	f.set(drawerParam(0, SpectralControl::TiltOn), 1.0);
	f.set(drawerParam(0, SpectralControl::CombOn), 1.0);
	f.set(drawerParam(0, SpectralControl::FormantShiftOn), 1.0);
	f.set(drawerParam(0, SpectralControl::FormantShiftLink), 0.0);
	f.set(drawerParam(0, SpectralControl::FormantShiftLow), 5.0);
	f.set(drawerParam(0, SpectralControl::FilterCutoff), std::log2(1800.0));
	f.set(drawerParam(0, SpectralControl::FilterResonance), 0.6);
	// AND THE LATER FIVE (spectral-ops, 2026-10-12), so their panels are lit and their handles drawn.
	f.set(drawerParam(0, SpectralControl::FormantOn), 1.0);
	f.set(drawerParam(0, SpectralControl::PartialsOn), 1.0);
	f.set(drawerParam(0, SpectralControl::PartialsCount), 40.0);
	f.set(drawerParam(0, SpectralControl::DampOn), 1.0);
	f.set(drawerParam(0, SpectralControl::StretchOn), 1.0);
	f.set(drawerParam(0, SpectralControl::ShiftOn), 1.0);
	f.set(drawerParam(0, SpectralControl::ShiftHz), 80.0);
	f.bridge.setSpectralCurveOn(0, true);
	for (size_t b = 4; b < 12; ++b) {
		f.bridge.setSpectralCurveBand(0, b, b == 8 ? 0.0f : 0.35f);
	}
	const auto shoot = [&](const char* name, float pw, float ph) {
		f.panel.setBounds((static_cast<float>(w) - pw) * 0.5f, (static_cast<float>(h) - ph) * 0.5f, pw, ph);
		for (int i = 0; i < 20; ++i) {
			f.update();
			editor.redrawAll();
			editor.drawWindow();
		}
		const visage::Screenshot& shot = editor.takeScreenshot();
		REQUIRE(shot.width() == w);
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		MESSAGE("wrote " << path << " (" << f.panel.rowsForTest() << " row(s))");
	};
	shoot("spectrum_panel", ui::SpectrumPanel::kMinimumWidth, ui::SpectrumPanel::kMinimumHeight);
	shoot("spectrum_panel_large", 1200.0f, 640.0f);
	shoot("spectrum_panel_narrow", 560.0f, ui::SpectrumPanel::kMinimumHeight);
	f.bridge.setSpectralOn(false);
	shoot("spectrum_panel_off", ui::SpectrumPanel::kMinimumWidth, ui::SpectrumPanel::kMinimumHeight);
	editor.removeChild(&f.panel);
}

TEST_CASE("OE: the drawer's Vocode section — the sound's controls by its own switch, the engine's analysis live while any sound vocodes, Key Track only while frozen") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	const size_t selected = f.selection.slot();
	const size_t other = (selected + 1) % feathers::kMaxSounds;
	f.set(soundParam(selected, SoundField::Enable), 1.0);
	f.update();
	ui::VocodeAnalysisColumn* column = f.panel.vocodeColumnForTest(selected);
	ui::PillStack* times = f.panel.vocodeTimesForTest(selected);
	REQUIRE(column != nullptr);
	REQUIRE(times != nullptr);
	ui::FaderPill* depth = f.panel.pillForTest(selected, SpectralControl::VocodeDepth);
	REQUIRE(depth != nullptr);
	const auto state = [&] {
		f.update();
		return std::array<bool, 5> { column->resolution().enabled(), column->freeze().enabled(), column->keyTrack().enabled(),
			                         times->pill(0).active(), depth->active() };
	};
	// NO SOUND VOCODES: the analysis reads nothing, so every analysis control is greyed, and so is the sound's Depth.
	CHECK(state() == std::array<bool, 5> { false, false, false, false, false });
	// ANOTHER SOUND VOCODES: the one analysis is live, whichever sound is shown; this sound's own Depth still follows its switch.
	f.set(drawerParam(other, SpectralControl::VocodeOn), 1.0);
	CHECK(state() == std::array<bool, 5> { true, true, false, true, false });
	// FROZEN: Key Track moves the held envelope, so it lights.
	f.set(ParamId::AdditiveVocodeFreeze, 1.0);
	CHECK(state() == std::array<bool, 5> { true, true, true, true, false });
	// THIS SOUND VOCODES TOO: its Depth lights.
	f.set(drawerParam(selected, SpectralControl::VocodeOn), 1.0);
	CHECK(state() == std::array<bool, 5> { true, true, true, true, true });
	// THE MASTER OFF: nothing of the drawer acts, the analysis included.
	f.bridge.setSpectralOn(false);
	CHECK(state() == std::array<bool, 5> { false, false, false, false, false });
	f.bridge.setSpectralOn(true);
	// OFF ADDITIVE: no engine reads it.
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	CHECK(state() == std::array<bool, 5> { false, false, false, false, false });
}

TEST_CASE("OE2: the Vocode section's Spacing and Widths cells — live with this sound's Vocode, and showing the box their zone reads") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	const size_t selected = f.selection.slot();
	f.set(soundParam(selected, SoundField::Enable), 1.0);
	f.update();
	ui::ModeValueCell* spacing = f.panel.vocodeSpacingForTest(selected);
	ui::ModeValueCell* widths = f.panel.vocodeWidthsForTest(selected);
	REQUIRE(spacing != nullptr);
	REQUIRE(widths != nullptr);
	CHECK_FALSE(spacing->enabled()); // this sound does not vocode
	f.set(drawerParam(selected, SpectralControl::VocodeOn), 1.0);
	f.update();
	CHECK(spacing->enabled());
	CHECK(widths->enabled());
	// EVEN READS NOTHING; A NAMED ZONE ITS AMOUNT; FREE ITS COORDINATE (the relevance the engine's VocodeOp::zoneDrive reads).
	CHECK(spacing->activeParam() == -1);
	f.set(ParamId::AdditiveVocodeSpacing, 1.0);
	f.update();
	CHECK(spacing->activeParam() == static_cast<long>(SpectralControl::VocodeSpacingAmount));
	f.set(ParamId::AdditiveVocodeSpacing, 6.0);
	f.update();
	CHECK(spacing->activeParam() == static_cast<long>(SpectralControl::VocodeSpacingFree));
	f.set(ParamId::AdditiveVocodeWidths, 5.0);
	f.update();
	CHECK(widths->activeParam() == static_cast<long>(SpectralControl::VocodeWidthsFree));
	f.set(ParamId::AdditiveVocodeWidths, 3.0);
	f.update();
	CHECK(widths->activeParam() == static_cast<long>(SpectralControl::VocodeWidthsAmount));
}

TEST_CASE("SHOT the spectrum panel's Vocode section: on, frozen, with Key Track") {
	// NOT A CASE — A CAMERA (the SHOT case above's road), gated on FEATHERS_SHOT_DIR. Writes spectrum_panel_vocode.png at the panel's
	// minimum and spectrum_panel_vocode_large.png.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	SpectrumFixture f;
	constexpr int w = 1240;
	constexpr int h = 700;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	editor.addChild(&f.panel);
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	(void)f.bridge.publishPlot(plotFrameAt(130.0, 120, true));
	f.set(drawerParam(0, SpectralControl::VocodeOn), 1.0);
	f.set(drawerParam(0, SpectralControl::VocodeFlatten), 0.8);
	f.set(drawerParam(0, SpectralControl::VocodeGate), -60.0);
	f.set(drawerParam(0, SpectralControl::VocodeLow), std::log2(120.0));
	f.set(ParamId::AdditiveVocodeFreeze, 1.0);
	f.set(ParamId::AdditiveVocodeKeyTrack, 1.0);
	f.set(ParamId::AdditiveVocodeBands, 48.0);
	// OE2: the banded road on, Harmonic spacing, Spread.
	f.set(drawerParam(0, SpectralControl::VocodeBandwidth), 0.3);
	f.set(ParamId::AdditiveVocodeSpacing, 4.0);
	f.set(drawerParam(0, SpectralControl::VocodeSpacingAmount), 0.8);
	f.set(ParamId::AdditiveVocodeStereo, 2.0);
	const auto shoot = [&](const char* name, float pw, float ph) {
		f.panel.setBounds((static_cast<float>(w) - pw) * 0.5f, (static_cast<float>(h) - ph) * 0.5f, pw, ph);
		for (int i = 0; i < 20; ++i) {
			f.update();
			editor.redrawAll();
			editor.drawWindow();
		}
		const visage::Screenshot& shot = editor.takeScreenshot();
		REQUIRE(shot.width() == w);
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		MESSAGE("wrote " << path << " (" << f.panel.rowsForTest() << " row(s))");
	};
	shoot("spectrum_panel_vocode", ui::SpectrumPanel::kMinimumWidth, ui::SpectrumPanel::kMinimumHeight);
	shoot("spectrum_panel_vocode_large", 1200.0f, 640.0f);
	editor.removeChild(&f.panel);
}

// ---- THE OPERATOR ROW AT EVERY WIDTH THE PANEL CAN BE GIVEN. It opens at its minimum, where the row is one line; a host
// window smaller than the editor's canvas, or a resize, can make it narrower, and then the row wraps (2026-10-11:
// "controls are clipping off the right side" was the drawer's; wrap, never scroll, and never cut a caption).
namespace {

// THE NARROWEST IS 480 (it was 374): the Vocode panel now carries its own Spacing and Widths cells under its chip, which
// makes it one ~460 px section that cannot be cut; the panel itself never opens below kMinimumWidth (940).
constexpr std::array<float, 8> kPanelWidths { 1240.0f, ui::SpectrumPanel::kMinimumWidth, 866.0f, 798.0f, 710.0f, 576.0f,
	                                          508.0f, 480.0f };

/// Additive, on sound A, the panel `width` wide at its minimum height.
ui::SpectrumPanel& panelAt(SpectrumFixture& f, float width) {
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	f.selection.select(0);
	f.panel.setBounds(0.0f, 0.0f, width, ui::SpectrumPanel::kMinimumHeight);
	f.update();
	return f.panel;
}

bool inside(const visage::Bounds& inner, const visage::Bounds& outer) {
	return inner.x() >= outer.x() - 0.01f && inner.y() >= outer.y() - 0.01f && inner.right() <= outer.right() + 0.01f
	       && inner.bottom() <= outer.bottom() + 0.01f;
}

bool overlap(const visage::Bounds& a, const visage::Bounds& b) {
	return a.x() < b.right() - 0.01f && b.x() < a.right() - 0.01f && a.y() < b.bottom() - 0.01f && b.y() < a.bottom() - 0.01f;
}

} // namespace

TEST_CASE("spectrum panel: at every width the panel can be given, every control lies inside it and none overlaps another or the plot") {
	for (const float width : kPanelWidths) {
		CAPTURE(width);
		SpectrumFixture f;
		ui::SpectrumPanel& d = panelAt(f, width);
		REQUIRE(d.width() == doctest::Approx(width));
		const visage::Bounds whole { 0.0f, 0.0f, d.width(), d.height() };
		// EACH FILTER TAB IN TURN (F2): the row lays out the selected filter's panel, and the other's widgets are out of sight.
		for (const int tab : { 0, 1 }) {
			CAPTURE(tab);
			d.filterTabsForTest(0, 0).select(tab);
			f.update();
			std::vector<std::pair<std::string, visage::Bounds>> boxes;
			size_t n = 0;
			for (visage::Frame* frame : d.controlsForTest(0)) {
				if (frame->isVisible()) {
					boxes.emplace_back("control " + std::to_string(n), frame->bounds());
				}
				++n;
			}
			// THE CURVE'S OVERLAY (its switch, Flatten, Draw) lies ON the plot by design (backlog OF): inside it, clear of every other control.
			std::vector<std::pair<std::string, visage::Bounds>> overlay;
			for (visage::Frame* frame : d.overlayForTest(0)) {
				overlay.emplace_back("overlay " + std::to_string(overlay.size()), frame->bounds());
			}
			for (const auto& [name, box] : overlay) {
				CAPTURE(name);
				CHECK(box.width() > 0.0f);
				CHECK(inside(box, d.plotBounds()));
				for (const auto& [other, otherBox] : overlay) {
					CHECK((name == other) == overlap(box, otherBox));
				}
			}
			for (const auto& [name, box] : boxes) {
				CAPTURE(name);
				CHECK(box.width() > 0.0f);
				CHECK(inside(box, whole));
				CHECK_FALSE(overlap(box, d.plotBounds()));
			}
			for (size_t i = 0; i < boxes.size(); ++i) {
				for (size_t j = i + 1; j < boxes.size(); ++j) {
					CAPTURE(boxes[i].first);
					CAPTURE(boxes[j].first);
					CHECK_FALSE(overlap(boxes[i].second, boxes[j].second));
				}
			}
			// NO CAPTION IS CUT (by design: a truncated label is a defect): each knob's caption, drawn in its face
			// and tracking, fits its column with the margins either side, and the text the knob draws is the whole word.
			for (const SpectralControl c : feathers::kSpectralContinuousControls) {
				const ui::Knob* knob = d.knobForTest(0, c);
				// OE2: AND THE VOCODE ZONE CELLS' BOXES (Spacing, Widths: an amount and a Free coordinate each), not knobs either.
				const bool zoneBox = c == SpectralControl::VocodeSpacingAmount || c == SpectralControl::VocodeSpacingFree
				                     || c == SpectralControl::VocodeWidthsAmount || c == SpectralControl::VocodeWidthsFree;
				if (c == SpectralControl::PartialsCount || c == SpectralControl::PartialsSet || zoneBox) {
					CHECK(knob == nullptr); // the Partials mode cell's box (OH), not a knob
					continue;
				}
				// DENSE (backlog OF): every amount but the filters' and the Partials Depth is a pill in a stack (its caption is the pill's own).
				if (knob == nullptr) {
					CHECK(d.pillForTest(0, c) != nullptr);
					continue;
				}
				if (!knob->isVisible()) {
					continue; // the other filter's: checked with its own tab
				}
				CAPTURE(std::string(knob->label()));
				CHECK(knob->captionWidth() + 2.0f * ui::Knob::kCaptionMargin <= knob->width() + 0.01f);
				CHECK(knob->drawnCaption().find("...") == std::string::npos);
			}
			// AND THE ROUTING PAD'S (F2), a column of the shown filter's knob row.
			// (A dense head carries the routing as a chip, which prints its whole word: only a Pad truncates a caption.)
			CHECK(d.routingForTest(0, tab).width() >= 40.0f);
			CHECK(d.fitsForTest());
			// THE PLOT GAVE UP THE HEIGHT, and is still a plot.
			CHECK(inside(d.plotBounds(), whole));
			// At its minimum height the plot keeps over 80 px at every width, the narrowest included, where five rows leave it the
			// least (kMinimumHeight was raised for exactly that; the MEASUREMENT case prints each).
			CHECK(d.plotBounds().height() > 80.0f);
		}
	}
}

/// Every visible descendant of `frame` lies inside it (its own coordinates), and no two visible siblings overlap — the cells' inner
/// boxes, pills and chips, which a section-level sweep cannot see.
static void checkDescendants(const visage::Frame& frame, const std::string& path) {
	const visage::Bounds local { 0.0f, 0.0f, frame.width(), frame.height() };
	std::vector<const visage::Frame*> kids;
	for (const visage::Frame* child : frame.children()) {
		if (child != nullptr && child->isVisible() && child->width() > 0.0f) {
			kids.push_back(child);
		}
	}
	for (size_t i = 0; i < kids.size(); ++i) {
		CAPTURE(path);
		CHECK(inside(kids[i]->bounds(), local));
		for (size_t j = i + 1; j < kids.size(); ++j) {
			CHECK_FALSE(overlap(kids[i]->bounds(), kids[j]->bounds()));
		}
		checkDescendants(*kids[i], path + "/" + std::to_string(i));
	}
}

TEST_CASE("spectrum panel: at every width, with Vocode off and on, every section holds its own cells and its own chip, and no two sections or cells overlap") {
	// FROM THE NARROWEST WIDTH THE ROWS CAN STILL STACK INSIDE THE PANEL'S MINIMUM HEIGHT (the panel's own floor is 940: minimumSize), and the
	// widths between, where a wrap changes. Below ~420 the panels need more rows than 640 px hold, with or without Vocode.
	std::vector<float> widths;
	for (const float w : kPanelWidths) {
		if (w >= 420.0f) {
			widths.push_back(w);
		}
	}
	for (float w = 423.0f; w <= 1240.0f; w += 23.0f) {
		widths.push_back(w);
	}
	for (const float width : widths) {
		for (const int vocode : { 0, 1, 2 }) { // off; on in the shown sound; on in ANOTHER sound only (the analysis shows for either)
			for (const int tab : { 0, 1 }) {
				CAPTURE(width);
				CAPTURE(vocode);
				CAPTURE(tab);
				SpectrumFixture f;
				ui::SpectrumPanel& d = panelAt(f, width);
				f.panel.setDpiScale(1.75f); // the development display
				f.panel.setBounds(0.0f, 0.0f, width, ui::SpectrumPanel::kMinimumHeight);
				f.set(drawerParam(0, SpectralControl::VocodeOn), vocode == 1 ? 1.0 : 0.0);
				f.set(drawerParam(1, SpectralControl::VocodeOn), vocode == 2 ? 1.0 : 0.0);
				// THE CELLS' OWN STATES: each Spacing/Widths zone and Partials scheme shows a different box in the cell.
				f.set(ParamId::AdditiveVocodeSpacing, static_cast<double>((static_cast<int>(width) / 23) % 7));
				f.set(ParamId::AdditiveVocodeWidths, static_cast<double>((static_cast<int>(width) / 23) % 6));
				f.set(drawerParam(0, SpectralControl::PartialsMode), static_cast<double>((static_cast<int>(width) / 23) % 3));
				f.update();
				d.filterTabsForTest(0, 0).select(tab);
				f.update();
				f.update();
				const auto sections = d.sectionsForTest(0);
				const visage::Bounds whole { 0.0f, 0.0f, d.width(), d.height() };
				bool analysis = false;
				for (const auto& s : sections) {
					analysis = analysis || s.name == "Analysis";
				}
				CHECK(analysis == (vocode != 0)); // the engine's analysis panel is in the row only while some sound vocodes
				// THE VOCODE CHIP HEADS THE WHOLE PER-SOUND SECTION: Spacing and Widths (this sound's values, greyed with its Vocode) are in the
				// section the chip heads, and the engine-wide analysis is in a chipless panel of its own, never under the chip.
				for (const auto& s : sections) {
					const auto holds = [&s](const visage::Frame* frame) { return std::find(s.frames.begin(), s.frames.end(), frame) != s.frames.end(); };
					if (s.name == "Vocode") {
						CHECK(s.chip == d.switchForTest(0, SpectralControl::VocodeOn));
						CHECK(holds(d.vocodeSpacingForTest(0)));
						CHECK(holds(d.vocodeWidthsForTest(0)));
						CHECK_FALSE(holds(d.vocodeColumnForTest(0)));
					}
					if (s.name == "Analysis") {
						CHECK(s.chip == nullptr);
						CHECK(holds(d.vocodeColumnForTest(0)));
						CHECK_FALSE(holds(d.vocodeSpacingForTest(0)));
					}
				}
				for (size_t i = 0; i < sections.size(); ++i) {
					const auto& s = sections[i];
					CAPTURE(s.name);
					CHECK(inside(s.bounds, whole));
					CHECK_FALSE(overlap(s.bounds, d.plotBounds()));
					// A SECTION'S CHIP HEADS ITS OWN SECTION: inside its bounds, in its head line, and its frame is the section's own.
					if (s.chip != nullptr) {
						CHECK(inside(s.chip->bounds(), s.bounds));
						CHECK(s.chip->y() < s.bounds.y() + ui::SpectrumPanel::kPanelPad + ui::SpectrumPanel::kHeadHeight);
					}
					for (size_t a = 0; a < s.frames.size(); ++a) {
						if (!s.frames[a]->isVisible()) {
							continue;
						}
						CHECK(inside(s.frames[a]->bounds(), s.bounds));
						checkDescendants(*s.frames[a], s.name + ":" + std::to_string(a));
						for (size_t b = a + 1; b < s.frames.size(); ++b) {
							if (s.frames[b]->isVisible()) {
								CHECK_FALSE(overlap(s.frames[a]->bounds(), s.frames[b]->bounds()));
							}
						}
					}
					for (size_t j = i + 1; j < sections.size(); ++j) {
						CAPTURE(sections[j].name);
						CHECK_FALSE(overlap(s.bounds, sections[j].bounds));
					}
				}
			}
		}
	}
}

TEST_CASE("spectrum panel: the panels wrap onto more rows exactly when one row does not fit at the floor column, and the plot follows") {
	SpectrumFixture f;
	ui::SpectrumPanel& d = panelAt(f, 900.0f);
	const float threshold = d.oneRowWidthForTest(d.minColumn()) + 2.0f * ui::SpectrumPanel::kPad;
	std::vector<float> widths(kPanelWidths.begin(), kPanelWidths.end());
	widths.push_back(std::ceil(threshold));
	widths.push_back(std::floor(threshold) - 1.0f);
	for (const float width : widths) {
		CAPTURE(width);
		panelAt(f, width);
		const bool wraps = width < threshold;
		CHECK((d.rowsForTest() == 1u) == !wraps);
		CHECK(d.oneRowFitsForTest() == !wraps);
		const auto rows = static_cast<float>(d.rowsForTest());
		CHECK(d.rowsHeight() == doctest::Approx(rows * d.rowHeightNow() + (rows - 1.0f) * ui::SpectrumPanel::kPanelGap));
		// THE PLOT FOLLOWS ITS BOUNDS: its own frame is the reduced area, and its dB axis spans that height (the D2 geometry
		// is a function of the plot's frame, so it holds at either height).
		ui::SpectrumPlot& plot = d.plotForTest();
		CHECK(plot.bounds().height() == doctest::Approx(d.plotBounds().height()));
		CHECK(plot.dbToY(ui::SpectrumPlot::kBottomDb) == doctest::Approx(plot.height() - ui::SpectrumPlot::kPadBottom));
		CHECK(plot.dbToY(ui::SpectrumPlot::kTopDb) == doctest::Approx(ui::SpectrumPlot::kPadTop));
	}
}

TEST_CASE("MEASUREMENT spectrum panel: the operator row's width, each caption's, and the rows and the plot at every width") {
	// NOT AN ASSERTION ABOUT A NUMBER — THE NUMBERS THE PANEL'S MINIMUM AND ITS WRAP ARE SIZED AGAINST. Printed; the
	// geometry cases above sweep these widths.
	SpectrumFixture f;
	ui::SpectrumPanel& d = panelAt(f, ui::SpectrumPanel::kMinimumWidth);
	MESSAGE("operator row at knob column " << ui::SpectrumPanel::kKnobColumn << ": "
	        << d.oneRowWidthForTest(ui::SpectrumPanel::kKnobColumn) << " px; at the floor " << d.minColumn()
	        << " (the widest caption and its margins): " << d.oneRowWidthForTest(d.minColumn()) << " px; plus "
	        << 2.0f * ui::SpectrumPanel::kPad << " of pad");
	// EACH CAPTION'S DRAWN WIDTH, in the face and tracking the knob draws it with (Knob::captionWidth), which sets the floor
	// column: a caption never truncates (by design).
	for (const SpectralControl c : feathers::kSpectralContinuousControls) {
		const ui::Knob* knob = d.knobForTest(0, c);
		if (knob == nullptr) {
			continue; // the Partials count: its mode cell's box (OH)
		}
		MESSAGE("caption " << std::string(knob->label()) << ": " << knob->captionWidth() << " px");
	}
	for (const float width : kPanelWidths) {
		panelAt(f, width);
		MESSAGE("panel " << width << " x " << ui::SpectrumPanel::kMinimumHeight << ": " << d.rowsForTest()
		                 << " row(s), the plot " << d.plotBounds().height() << " px tall");
	}
	std::string widths;
	for (const float w : d.panelWidthsForTest(d.minColumn())) {
		widths += std::to_string(static_cast<int>(w)) + " ";
	}
	MESSAGE("each panel at the floor column, in row order: " << widths);
}

// ---------------------------------------------------------------------------------------------
// BACKLOG LI — THE FOOTER'S SEPARATOR UNDER THE EXPLORE CIRCLE. A 1 px error in the frame directly
// below the explore circle (between Explore and the tray): the line took the terrain's colour.
// PHOTOGRAPHED on xvfb at UI scale 1.5 with the rails folded: the separator's first
// native row read the terrain across the stage (84 of 691 pixels kLine) and the separator everywhere else.
// visage gives a frame `round(y*s)` and `round(h*s)` SEPARATELY, so a stage whose logical foot is the
// separator's top can end half a native row INTO it, and the stage is composited over the ground.
// ---------------------------------------------------------------------------------------------

TEST_CASE("backlog LI: the separator under the Explore stage is the separator's colour at every UI scale, "
          "and the stage reaches it with no gap") {
	// ON A SCREEN THAT HOLDS EVERY RUNG — the floor is held to the usable screen (backlog LX).
	struct ScreenStub {
		ScreenStub() { ui::PluginWindow::setUsableScreenForTest(8192, 8192); }
		~ScreenStub() { ui::PluginWindow::setUsableScreenForTest(0, 0); }
	} screen;
	QuietGuiHost host;
	ParamBridge bridge;
	Gui gui { host, bridge };
	bridge.refresh();
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	gui.setPopoutWindowForTest(false);
	visage::Frame* root = gui.root();
	REQUIRE(root != nullptr);
	auto* stage = findInTree<ExploreStage>(root);
	REQUIRE(stage != nullptr);
	// THE LADDER BY ITS OWN ROAD, from the bottom rung: Ctrl+0, then Ctrl+= up to the top. Each step writes the
	// preference — into this binary's scratch desk, and put back to 100 % at the end.
	const auto key = [&](visage::KeyCode code) {
		REQUIRE(root->onKeyPress().callback(visage::KeyEvent { code, visage::kModifierRegCtrl, true }));
	};
	key(visage::KeyCode::Number0);

	// ANYTHING THE STAGE COULD PAINT: its whole native rectangle in a colour nothing else uses. The terrain is a
	// bake on a worker and may not have landed; the rectangle is what visage clips the stage's drawing to, so it
	// is the bound on every pixel the stage can put on the seam, terrain included.
	constexpr uint32_t kSentinel = 0xffff00ffu;
	const visage::Color line(ui::kLine);
	const visage::Color sentinel(kSentinel);
	const auto same = [](const visage::Color& a, const visage::Color& b) {
		const auto near = [](int x, int y) { return std::abs(x - y) <= 2; };
		return near(a.hexRed(), b.hexRed()) && near(a.hexGreen(), b.hexGreen()) && near(a.hexBlue(), b.hexBlue());
	};

	ExpandedCanvas* band = gui.canvasForTest();
	REQUIRE(band != nullptr);
	int configurations = 0;
	int rungs = 0;
	for (;;) {
		const double ui = gui.uiScaleForTest();
		// THE FOOT'S PHASE IS SET BY THE SCALE AND BY WHAT SITS UNDER THE TRAY, not by the window: the
		// separator is `h - canvas - tray` logical, so in native rows it is the integer window height less
		// `(canvas + tray) * s`. So both canvas states, each at the size the host would grant — and a row
		// taller, which moves nothing and proves it.
		for (int config = 0; config < 4; ++config) {
			const bool open = config >= 2;
			if (band->collapsed() == open) {
				REQUIRE(band->toggleCollapsed());
			}
			REQUIRE(band->collapsed() == !open);
			uint32_t w = 1;
			uint32_t h = 1;
			REQUIRE(gui.adjustSize(&w, &h)); // the floor the host is told: the design at this scale
			REQUIRE(gui.setSize(w, h + static_cast<uint32_t>(config % 2)));
			CAPTURE(open);
			const int nativeW = root->nativeWidth();
			const int nativeH = root->nativeHeight();
			REQUIRE(nativeW > 0);
			REQUIRE(nativeH > 0);
			REQUIRE(stage->nativeHeight() > 0);
			const float scale = stage->dpiScale();
			CAPTURE(ui);
			CAPTURE(scale);
			CAPTURE(nativeH);

			const auto compose = [&](bool withStage) {
				visage::Canvas canvas;
				markedWindowless(canvas, nativeW, nativeH);
				canvas.setDpiScale(scale);
				visage::Region ground;
				ground.setBounds(0, 0, nativeW, nativeH);
				canvas.addRegion(&ground);
				canvas.beginRegion(&ground);
				gui.drawBackgroundForTest(canvas);
				canvas.endRegion();
				visage::Region over;
				over.setBounds(stage->nativeX(), stage->nativeY(), stage->nativeWidth(), stage->nativeHeight());
				if (withStage) {
					canvas.addRegion(&over);
					canvas.beginRegion(&over);
					canvas.setNativePixelScale();
					canvas.setColor(kSentinel);
					canvas.fill(0.0f, 0.0f, static_cast<float>(stage->nativeWidth()),
					            static_cast<float>(stage->nativeHeight()));
					canvas.endRegion();
				}
				return takeMarkedCapture(canvas, nativeW, nativeH);
			};
			const visage::Screenshot groundOnly = compose(false);
			const visage::Screenshot composite = compose(true);

			// THE SEPARATOR'S ROWS, read off the ground alone at the left edge — outside the stage, where nothing
			// can cover it. Searched a few rows either side of the stage's foot.
			const visage::Color voidColour = groundOnly.sample(1, stage->nativeBottom() - 8);
			std::vector<int> rows;
			for (int y = std::max(0, stage->nativeBottom() - 4); y < std::min(nativeH, stage->nativeBottom() + 4); ++y) {
				if (!same(groundOnly.sample(1, y), voidColour)) {
					rows.push_back(y);
				}
			}
			REQUIRE_MESSAGE(!rows.empty(), "no separator near the stage's foot: the instrument cannot see the seam");
			CAPTURE(rows.front());
			CAPTURE(stage->nativeBottom());

			int wrong = 0;
			int gap = 0;
			for (int x = stage->nativeX(); x < stage->nativeRight(); ++x) {
				for (int y : rows) {
					// BY VALUE: the separator's token, under the stage exactly as it is beside it.
					wrong += same(composite.sample(x, y), line) ? 0 : 1;
				}
				// AND THE STAGE REACHES IT: the row above the separator is the stage's.
				gap += same(composite.sample(x, rows.front() - 1), sentinel) ? 0 : 1;
			}
			CHECK(wrong == 0);
			CHECK(gap == 0);
			++configurations;
		}
		++rungs;
		if (PluginWindow::stepUiScale(ui, +1) <= ui) {
			break; // the top rung holds
		}
		key(visage::KeyCode::Equals);
		REQUIRE(gui.uiScaleForTest() > ui);
	}
	// EVERY RUNG THE LADDER HAS, counted by walking it independently of the keys.
	int ladder = 1;
	for (double at = 1.0; PluginWindow::stepUiScale(at, +1) > at; at = PluginWindow::stepUiScale(at, +1)) {
		++ladder;
	}
	CHECK(rungs == ladder);
	CHECK(configurations == ladder * 4);
	if (band->collapsed() == false) {
		REQUIRE(band->toggleCollapsed());
	}
	key(visage::KeyCode::Number0);
}

TEST_CASE("Explore's Theta reads in degrees, types in degrees, and a typed angle past half a turn folds "
          "rather than clamping") {
	// THE ROTATE ENTRY ("Rotate reads 3.14"): Theta is a real angle — the sound's place is cos/sin of it — so
	// its descriptor is `Unit::Degrees`. Its knob printed degrees by hand before and REFUSED a type-in,
	// because `Modulatable::editText` opens only on the descriptor's own reading.
	constexpr double kPi = 3.14159265358979323846;
	ParamBridge bridge;
	bridge.refresh();
	TravelControl theta(bridge, 0, TravelAxis::Theta, {});
	REQUIRE(theta.desc().unit == device::Unit::Degrees);

	const auto stored = [&] { return bridge.soundTravel().orbit[0].theta; };
	theta.setValue(0.0);
	CHECK(theta.text() == "+0 deg");
	theta.setValue(kPi / 2.0);
	CHECK(theta.text() == "+90 deg");
	theta.setValue(kPi);
	CHECK(theta.text() == "+180 deg");
	// WOUND PAST A TURN, IT IS STORED FOLDED — and reads the fold: 2.5 pi is a quarter turn.
	theta.setValue(2.5 * kPi);
	CHECK(std::abs(stored() - kPi / 2.0) < 1e-9);
	CHECK(theta.text() == "+90 deg");

	// THE TYPE-IN: open, read back, and land.
	const std::optional<std::string> opened = theta.editText();
	REQUIRE(opened.has_value());
	CAPTURE(*opened);
	const std::optional<double> same = theta.parseText(*opened, theta.value());
	REQUIRE(same.has_value());
	CHECK(static_cast<float>(*same) == static_cast<float>(theta.value()));

	// "270" IS THREE QUARTERS ROUND, which is a quarter turn back — not the half turn a clamp would type.
	const std::optional<double> typed = theta.parseText("270", theta.value());
	REQUIRE(typed.has_value());
	CHECK(std::abs(*typed - 1.5 * kPi) < 1e-9);
	theta.setValue(*typed);
	CHECK(std::abs(stored() + kPi / 2.0) < 1e-9);
	CHECK(theta.text() == "-90 deg");
	CHECK_FALSE(theta.parseText("1.57 rad", theta.value()).has_value());

	// RADIUS IS NOT AN ANGLE AND KEEPS ITS OWN READING.
	TravelControl radius(bridge, 0, TravelAxis::Radius, {});
	CHECK(radius.desc().unit == device::Unit::Number);
	CHECK(radius.text().find("deg") == std::string::npos);
}

TEST_CASE("Explore's Rotate corner prints the turn in degrees of the stored turn — a quarter reads 90, not 360") {
	// THE CORNER PRINTED `radians * 180 / kRotateSpan`, degrees only while the gearing was pi. Backlog GF cut
	// the gearing to pi/4 and the text read four times the turn; the dial beside it had been corrected.
	constexpr double kPi = 3.14159265358979323846;
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	visage::Canvas canvas;
	f.stage.draw(canvas);
	CHECK(f.stage.rotateReadout() == "+0 deg");

	turnRotateCorner(f, static_cast<float>(0.5 * kPi / ExploreStage::kRotateSpan * Knob::kDragRange));
	f.settleGesture();
	f.stage.update();
	const double turned = f.stage.selectedRotation();
	CAPTURE(turned);
	REQUIRE(std::abs(turned - 0.5 * kPi) < 0.02);
	// BY VALUE, from the stored turn restated here: whole degrees of it.
	const long whole = std::lround(turned * 180.0 / kPi);
	char want[32] = {};
	std::snprintf(want, sizeof(want), "%+ld deg", whole);
	CHECK(f.stage.rotateReadout() == std::string(want));
	CHECK(std::abs(whole - 90) <= 1);
}

// ---- F2 (additive design §3.6): the filter tabs, the second filter's handle, the Bell, the routing ----------------------

TEST_CASE("F2 spectrum panel: the Filter 1 / Filter 2 tabs show one filter's panel at a time, and every control of both stays reachable") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.update();
	ui::SpectrumPanel& d = f.panel;
	const size_t s = f.selection.slot();
	const SpectralControl first[] = { SpectralControl::FilterOn, SpectralControl::FilterMode, SpectralControl::FilterCutoff,
		                              SpectralControl::FilterResonance, SpectralControl::FilterSlope,
		                              SpectralControl::FilterKeyTrack, SpectralControl::FilterGain };
	const SpectralControl second[] = { SpectralControl::Filter2On, SpectralControl::Filter2Mode, SpectralControl::Filter2Cutoff,
		                               SpectralControl::Filter2Resonance, SpectralControl::Filter2Slope,
		                               SpectralControl::Filter2KeyTrack, SpectralControl::Filter2Gain };
	const auto shown = [&](SpectralControl c) {
		visage::Frame* frame = d.frameForTest(s, c);
		REQUIRE(frame != nullptr);
		return frame->isVisible();
	};
	// FILTER 1 BY DEFAULT: its every control on the face, filter 2's none; the routing chip in filter 1's head.
	CHECK(d.filterTabForTest() == 0);
	for (const SpectralControl c : first) {
		CAPTURE(static_cast<int>(c));
		CHECK(shown(c));
	}
	for (const SpectralControl c : second) {
		CAPTURE(static_cast<int>(c));
		CHECK_FALSE(shown(c));
	}
	CHECK(d.filterTabsForTest(s, 0).isVisible());
	CHECK_FALSE(d.filterTabsForTest(s, 1).isVisible());
	CHECK(d.routingForTest(s, 0).isVisible());
	CHECK_FALSE(d.routingForTest(s, 1).isVisible());
	// THE TAB: Filter 2's panel in the row, filter 1's out of it — and nothing written to any param (view state).
	const double routingBefore = f.bridge.value(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterRouting)));
	d.filterTabsForTest(s, 0).select(1);
	f.update();
	CHECK(d.filterTabForTest() == 1);
	for (const SpectralControl c : second) {
		CAPTURE(static_cast<int>(c));
		CHECK(shown(c));
	}
	for (const SpectralControl c : first) {
		CAPTURE(static_cast<int>(c));
		CHECK_FALSE(shown(c));
	}
	CHECK(d.filterTabsForTest(s, 1).isVisible());
	CHECK(d.routingForTest(s, 1).isVisible());
	CHECK(f.bridge.value(static_cast<clap_id>(drawerParam(s, SpectralControl::FilterRouting))) == routingBefore);
	// EVERY SHOWN CONTROL LIES INSIDE THE PANEL, filter 2's head included (the tabs, the switch, the Mode, the routing).
	for (visage::Frame* frame : d.controlsForTest(s)) {
		if (frame->isVisible()) {
			CHECK(frame->bounds().right() <= d.width() + 0.5f);
			CHECK(frame->bounds().bottom() <= d.height() + 0.5f);
		}
	}
	// AND BACK.
	d.filterTabsForTest(s, 1).select(0);
	f.update();
	CHECK(d.filterTabForTest() == 0);
	CHECK(shown(SpectralControl::FilterCutoff));
	CHECK_FALSE(shown(SpectralControl::Filter2Cutoff));
}

TEST_CASE("F2 spectrum panel: Gain is live only in Bell, a 0 dB Bell greys its Res, Slope and Key, and the routing is live only with both filters on") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	ui::SpectrumPanel& d = f.panel;
	const size_t s = f.selection.slot();
	const auto at = [&](SpectralControl c, double v) { f.set(drawerParam(s, c), v); };
	for (const int filter : { 0, 1 }) {
		CAPTURE(filter);
		const auto ctl = [filter](SpectralControl c) { return feathers::spectralFilterControl(filter, c); };
		at(ctl(SpectralControl::FilterOn), 1.0);
		for (size_t m = 0; m < feathers::kSpectralFilterModeNames.size(); ++m) {
			CAPTURE(m);
			at(ctl(SpectralControl::FilterMode), static_cast<double>(m));
			at(ctl(SpectralControl::FilterGain), 6.0);
			f.update();
			const bool isBell = m == static_cast<size_t>(feathers::spectral::FilterMode::Bell);
			CHECK(d.liveForTest(s, ctl(SpectralControl::FilterGain)) == isBell);
			CHECK(d.liveForTest(s, ctl(SpectralControl::FilterResonance)) == (m != 3));
		}
		// A BELL AT 0 dB IS OPEN: Res, Slope and Key change nothing; Gain (the way back) and Cutoff stay live.
		at(ctl(SpectralControl::FilterMode), static_cast<double>(feathers::spectral::FilterMode::Bell));
		at(ctl(SpectralControl::FilterGain), 0.0);
		f.update();
		CHECK_FALSE(d.liveForTest(s, ctl(SpectralControl::FilterResonance)));
		CHECK_FALSE(d.liveForTest(s, ctl(SpectralControl::FilterSlope)));
		CHECK_FALSE(d.liveForTest(s, ctl(SpectralControl::FilterKeyTrack)));
		CHECK(d.liveForTest(s, ctl(SpectralControl::FilterGain)));
		CHECK(d.liveForTest(s, ctl(SpectralControl::FilterCutoff)));
		at(ctl(SpectralControl::FilterGain), 6.0);
	}
	// THE ROUTING: live with both on, greyed with either off — whichever tab shows its chip.
	for (const bool one : { false, true }) {
		for (const bool two : { false, true }) {
			CAPTURE(one);
			CAPTURE(two);
			at(SpectralControl::FilterOn, one ? 1.0 : 0.0);
			at(SpectralControl::Filter2On, two ? 1.0 : 0.0);
			f.update();
			CHECK(d.liveForTest(s, SpectralControl::FilterRouting) == (one && two));
			CHECK(d.routingForTest(s, 1).enabled() == (one && two));
		}
	}
}

TEST_CASE("F2 spectrum plot: both filters' handles are drawn, the selected grabbed first; grabbing filter 2's selects its tab; a Bell's height is its Gain") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f, 220.0);
	f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
	f.set(drawerParam(0, SpectralControl::FilterCutoff), std::log2(1500.0));
	f.update();
	CHECK(plot.handleForTest(Handle::Filter).has_value());
	CHECK_FALSE(plot.handleForTest(Handle::Filter2).has_value()); // filter 2 off: no handle
	f.set(drawerParam(0, SpectralControl::Filter2On), 1.0);
	f.set(drawerParam(0, SpectralControl::Filter2Cutoff), std::log2(1500.0)); // ON TOP OF filter 1's corner
	f.update();
	const auto one = plot.handleForTest(Handle::Filter);
	const auto two = plot.handleForTest(Handle::Filter2);
	REQUIRE(one.has_value());
	REQUIRE(two.has_value());
	CHECK(one->x == doctest::Approx(two->x));
	CHECK(one->y == doctest::Approx(two->y));
	CHECK(plot.labelForTest(Handle::Filter2) == "2: LP 1.5 kHz");
	// THE SELECTED FILTER IS GRABBED FIRST where the two overlap: filter 1's tab, so a drag moves filter 1 alone.
	dragHandle(f, plot, Handle::Filter, { plot.hzToX(800.0), one->y });
	CHECK(drawerValue(f, SpectralControl::FilterCutoff) == doctest::Approx(std::log2(800.0)).epsilon(1e-4));
	CHECK(drawerValue(f, SpectralControl::Filter2Cutoff) == doctest::Approx(std::log2(1500.0)).epsilon(1e-6));
	CHECK(f.panel.filterTabForTest() == 0);
	// GRABBING FILTER 2'S HANDLE (now alone where it sits) SELECTS ITS TAB, and its drag writes filter 2.
	dragHandle(f, plot, Handle::Filter2, { plot.hzToX(3000.0), two->y });
	CHECK(drawerValue(f, SpectralControl::Filter2Cutoff) == doctest::Approx(std::log2(3000.0)).epsilon(1e-4));
	CHECK(f.panel.filterTabForTest() == 1);
	// WITH FILTER 2 SELECTED IT IS THE ONE GRABBED ON TOP of filter 1.
	f.set(drawerParam(0, SpectralControl::FilterCutoff), std::log2(3000.0));
	f.update();
	const auto over = plot.handleForTest(Handle::Filter2);
	REQUIRE(over.has_value());
	dragHandle(f, plot, Handle::Filter2, { plot.hzToX(5000.0), over->y });
	CHECK(drawerValue(f, SpectralControl::Filter2Cutoff) == doctest::Approx(std::log2(5000.0)).epsilon(1e-4));
	CHECK(drawerValue(f, SpectralControl::FilterCutoff) == doctest::Approx(std::log2(3000.0)).epsilon(1e-6));
	// A BELL: its handle's height is its Gain (0 dB at the corner's resting height, kBellDbScale per dB), and up writes Gain.
	f.set(drawerParam(0, SpectralControl::Filter2Mode), static_cast<double>(feathers::spectral::FilterMode::Bell));
	f.set(drawerParam(0, SpectralControl::Filter2Gain), 8.0);
	f.update();
	const auto bell = plot.handleForTest(Handle::Filter2);
	REQUIRE(bell.has_value());
	CHECK(bell->y == doctest::Approx(plot.dbToY(ui::SpectrumPlot::kFilterBaseDb + 8.0 * ui::SpectrumPlot::kBellDbScale)));
	CHECK(plot.verticalForTest(Handle::Filter2));
	CHECK(plot.labelForTest(Handle::Filter2) == "2: Bell 5.0 kHz +8.0 dB");
	const double resBefore = drawerValue(f, SpectralControl::Filter2Resonance);
	dragHandle(f, plot, Handle::Filter2,
	           { bell->x, plot.dbToY(ui::SpectrumPlot::kFilterBaseDb - 4.0 * ui::SpectrumPlot::kBellDbScale) });
	CHECK(drawerValue(f, SpectralControl::Filter2Gain) == doctest::Approx(-4.0).epsilon(1e-3));
	CHECK(drawerValue(f, SpectralControl::Filter2Resonance) == resBefore); // up is Gain in Bell, never Resonance
}

TEST_CASE("SHOT F2: the drawer panel with both filters on, each tab in turn (FEATHERS_SHOT_DIR)") {
	// NOT A CASE — A CAMERA (the SHOT case above's road): spectrum_panel_f2_tab1.png and spectrum_panel_f2_tab2.png, filter 1 a
	// Band Pass, filter 2 a Bell at +9 dB, in Parallel, under each tab. Run under `scripts/venue-xvfb.sh` with
	// FEATHERS_REDUCED_MOTION=1.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	SpectrumFixture f;
	constexpr int w = 1240;
	constexpr int h = 700;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	editor.addChild(&f.panel);
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	(void)f.bridge.publishPlot(plotFrameAt(130.0, 120, false));
	f.set(drawerParam(0, SpectralControl::FilterOn), 1.0);
	f.set(drawerParam(0, SpectralControl::FilterMode), static_cast<double>(feathers::spectral::FilterMode::BandPass));
	f.set(drawerParam(0, SpectralControl::FilterCutoff), std::log2(500.0));
	f.set(drawerParam(0, SpectralControl::Filter2On), 1.0);
	f.set(drawerParam(0, SpectralControl::Filter2Mode), static_cast<double>(feathers::spectral::FilterMode::Bell));
	f.set(drawerParam(0, SpectralControl::Filter2Cutoff), std::log2(3500.0));
	f.set(drawerParam(0, SpectralControl::Filter2Gain), 9.0);
	f.set(drawerParam(0, SpectralControl::Filter2Resonance), 0.5);
	f.set(drawerParam(0, SpectralControl::FilterRouting), 1.0);
	f.set(drawerParam(0, SpectralControl::TiltOn), 1.0);
	const auto shoot = [&](const char* name) {
		f.panel.setBounds((static_cast<float>(w) - ui::SpectrumPanel::kMinimumWidth) * 0.5f,
		                  (static_cast<float>(h) - ui::SpectrumPanel::kMinimumHeight) * 0.5f, ui::SpectrumPanel::kMinimumWidth,
		                  ui::SpectrumPanel::kMinimumHeight);
		for (int i = 0; i < 20; ++i) {
			f.update();
			editor.redrawAll();
			editor.drawWindow();
		}
		const visage::Screenshot& shot = editor.takeScreenshot();
		REQUIRE(shot.width() == w);
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		MESSAGE("wrote " << path << " (tab " << f.panel.filterTabForTest() + 1 << ")");
	};
	shoot("spectrum_panel_f2_tab1");
	f.panel.filterTabsForTest(0, 0).select(1);
	shoot("spectrum_panel_f2_tab2");
	editor.removeChild(&f.panel);
}

TEST_CASE("OH spectrum panel: Partials is a mode cell — the scheme its dropdown, the count its box — greyed with its switch, and in Peaks the plot draws no cut line") {
	SpectrumFixture f;
	ui::SpectrumPlot& plot = openPlot(f, 220.0);
	ui::SpectrumPanel& d = f.panel;
	ui::ModeValueCell* cell = d.partialsCellForTest(0);
	REQUIRE(cell != nullptr);
	CHECK(d.frameForTest(0, SpectralControl::PartialsMode) == cell);
	CHECK(d.frameForTest(0, SpectralControl::PartialsCount) == cell->boxFor(static_cast<size_t>(SpectralControl::PartialsCount)));
	CHECK(d.knobForTest(0, SpectralControl::PartialsCount) == nullptr); // no knob: the count is the cell's box
	CHECK(cell->activeParam() == static_cast<long>(SpectralControl::PartialsCount));
	CHECK_FALSE(d.liveForTest(0, SpectralControl::PartialsMode)); // Partials off: greyed
	f.set(drawerParam(0, SpectralControl::PartialsOn), 1.0);
	f.set(drawerParam(0, SpectralControl::PartialsCount), 12.0);
	f.update();
	CHECK(d.liveForTest(0, SpectralControl::PartialsMode));
	CHECK(plot.handleForTest(Handle::PartialsCut).has_value()); // Harmonics: the cut between the 12th and 13th
	// THE CELL'S DROPDOWN WRITES THE SCHEME (the param road), and in Peaks there is no one frequency to cut at.
	cell->select().selectZone(static_cast<size_t>(feathers::spectral::PartialsScheme::Peaks)); // the dropdown's pick
	f.update();
	CHECK(drawerValue(f, SpectralControl::PartialsMode) == static_cast<double>(feathers::spectral::PartialsScheme::Peaks));
	CHECK_FALSE(plot.handleForTest(Handle::PartialsCut).has_value());
}

TEST_CASE("SHOT OH: the drawer's plot and the Spectral card's with Partials culling, each surface (FEATHERS_SHOT_DIR)") {
	// NOT A CASE — A CAMERA: spectrum_panel_oh_peaks.png, the drawer with Partials in Peaks at 6 over a frame whose result is the
	// engine's (the plot draws what the engine publishes). Run under `scripts/venue-xvfb.sh` with FEATHERS_REDUCED_MOTION=1.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	SpectrumFixture f;
	constexpr int w = 1240;
	constexpr int h = 700;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	editor.addChild(&f.panel);
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	f.set(drawerParam(0, SpectralControl::PartialsOn), 1.0);
	f.set(drawerParam(0, SpectralControl::PartialsMode), static_cast<double>(feathers::spectral::PartialsScheme::Peaks));
	f.set(drawerParam(0, SpectralControl::PartialsCount), 6.0);
	// THE ENGINE'S FRAME, AS A PEAKS CULL PUBLISHES IT: the six loudest partials' result kept, the rest silenced (terrain only).
	feathers::SpectralPlotFrame frame = plotFrameAt(130.0, 120, false);
	std::vector<std::pair<float, int>> loud;
	for (int i = 0; i < frame.count; ++i) {
		loud.emplace_back(frame.terrain[static_cast<size_t>(i)], i);
	}
	std::sort(loud.begin(), loud.end(), [](auto a, auto b) { return a.first > b.first; });
	for (size_t r = 6; r < loud.size(); ++r) {
		frame.left[static_cast<size_t>(loud[r].second)] = 0.0f;
		frame.right[static_cast<size_t>(loud[r].second)] = 0.0f;
	}
	(void)f.bridge.publishPlot(frame);
	f.panel.setBounds((static_cast<float>(w) - ui::SpectrumPanel::kMinimumWidth) * 0.5f,
	                  (static_cast<float>(h) - ui::SpectrumPanel::kMinimumHeight) * 0.5f, ui::SpectrumPanel::kMinimumWidth,
	                  ui::SpectrumPanel::kMinimumHeight);
	for (int i = 0; i < 20; ++i) {
		f.update();
		editor.redrawAll();
		editor.drawWindow();
	}
	const visage::Screenshot& shot = editor.takeScreenshot();
	REQUIRE(shot.width() == w);
	const std::string path = std::string(dir) + "/spectrum_panel_oh_peaks.png";
	shot.save(path);
	MESSAGE("wrote " << path);
	editor.removeChild(&f.panel);
}
TEST_CASE("terrain palette: every engine selects its own lifted map, and the Additive default is CRT") {
	// The terrain read dim overall. Additive gets retro-futuristic purples, blues and greens (CRT), and
	// every other engine a brighter identity. THE SELECTION, BY VALUE:
	CHECK(colourMapFor(OscType::Morph) == ColourMap::NebulaLit);
	CHECK(colourMapFor(OscType::Stair) == ColourMap::Orchid);
	CHECK(colourMapFor(OscType::V5) == ColourMap::Nebula); // V5 keeps its original look, by value:
	CHECK(colourMapFor(OscType::V5) == kShippedMap);
	CHECK(terrainGlowFor(colourMapFor(OscType::V5)).floor == doctest::Approx(0.10f).epsilon(1e-6));
	CHECK(terrainGlowFor(colourMapFor(OscType::V5)).range == doctest::Approx(1.05f).epsilon(1e-6));
	CHECK(colourMapFor(OscType::Resonate) == ColourMap::Gilt);
	CHECK(colourMapFor(OscType::BasicShapes) == ColourMap::Spectrum);
	CHECK(colourMapFor(OscType::Additive) == ColourMap::Crt);
	CHECK(kAdditiveMap == ColourMap::Crt);
	CHECK(colourMapFor(OscType::Pluck) == ColourMap::Grove); // 2026-10-31, wood and moss: provisional
	CHECK(kPluckMap == ColourMap::Grove);
	// EACH ENGINE'S MAP IS ITS OWN, and the same features read through them give seven different colours.
	const TerrainFeatures sampleF = terrainFeaturesAt(OscType::Morph, SliceOrientationPin {}, 300, 0.0f, 0.02, 0.0);
	std::array<uint32_t, 7> colours {};
	for (size_t e = 0; e < 7; ++e) {
		colours[e] = terrainColour(sampleF, 0.02, 0.0, -1, colourMapFor(static_cast<OscType>(e)), kShippedSecondAxis, OscType::Morph);
		for (size_t other = 0; other < e; ++other) {
			CAPTURE(e);
			CAPTURE(other);
			CHECK(colours[e] != colours[other]);
		}
		// AND LIFTED against the dim look the other engines had (V5 never did, and is the original, above).
		if (static_cast<OscType>(e) != OscType::V5) {
			CHECK(terrainGlowFor(colourMapFor(static_cast<OscType>(e))).floor > terrainGlowFor(kShippedMap).floor + 0.1f);
		}
	}

	// THE CANDIDATES' CORNERS, restated: the settled-and-full corner (tilt 0, second 0) and the busy-and-full one (tilt 1, second 0).
	const auto corner = [](ColourMap map, float tilt, float second) {
		const MapRgb c = colourMapAt(map, tilt, second);
		return std::array<float, 3> { c.r, c.g, c.b };
	};
	CHECK(corner(ColourMap::Synthwave, 0.0f, 0.0f) == std::array<float, 3> { 84.0f, 56.0f, 220.0f });
	CHECK(corner(ColourMap::Synthwave, 1.0f, 0.0f) == std::array<float, 3> { 40.0f, 225.0f, 255.0f });
	CHECK(corner(ColourMap::Aurora, 1.0f, 1.0f) == std::array<float, 3> { 150.0f, 255.0f, 130.0f });
	CHECK(corner(ColourMap::Crt, 1.0f, 1.0f) == std::array<float, 3> { 90.0f, 255.0f, 110.0f });
	// RESONATE (yellows, reds and golds): deep red low, bright yellow high. BASIC SHAPES: the RGB sweep, a full circle: red at 0, yellow-green at 0.25, cyan at 0.5 of the tilt.
	CHECK(corner(ColourMap::Gilt, 0.0f, 0.0f) == std::array<float, 3> { 205.0f, 35.0f, 40.0f });
	CHECK(corner(ColourMap::Gilt, 1.0f, 0.0f) == std::array<float, 3> { 255.0f, 238.0f, 80.0f });
	// PLUCK: walnut settled and full, leaf green busy, pale birch busy and hollow.
	CHECK(corner(ColourMap::Grove, 0.0f, 0.0f) == std::array<float, 3> { 165.0f, 92.0f, 45.0f });
	CHECK(corner(ColourMap::Grove, 1.0f, 0.0f) == std::array<float, 3> { 150.0f, 235.0f, 75.0f });
	CHECK(corner(ColourMap::Grove, 1.0f, 1.0f) == std::array<float, 3> { 250.0f, 228.0f, 165.0f });
	CHECK(corner(ColourMap::Spectrum, 0.0f, 0.0f) == std::array<float, 3> { 255.0f, 0.0f, 0.0f });
	CHECK(corner(ColourMap::Spectrum, 0.25f, 0.0f) == std::array<float, 3> { 127.5f, 255.0f, 0.0f });
	CHECK(corner(ColourMap::Spectrum, 0.5f, 0.0f) == std::array<float, 3> { 0.0f, 255.0f, 255.0f });
	// AND THE ORIGINALS DID NOT MOVE (the three that ship for every other engine).
	CHECK(corner(ColourMap::Nebula, 0.0f, 0.0f) == std::array<float, 3> { 96.0f, 120.0f, 200.0f });
	CHECK(terrainGlowFor(ColourMap::Nebula).floor == doctest::Approx(0.10f).epsilon(1e-6));
	CHECK(terrainGlowFor(ColourMap::Nebula).range == doctest::Approx(1.05f).epsilon(1e-6));

	// THE BUILDER PASSES THE ENGINE'S MAP: a finished map's texel is the colour that engine's palette gives there.
	for (const OscType engine : { OscType::Morph, OscType::Stair, OscType::V5, OscType::Resonate, OscType::BasicShapes, OscType::Additive }) {
		TerrainWindow w = windowAt(0.3, 64.0 / 1023.0, 64.0 / 1023.0);
		w.engine = engine;
		TerrainMap map;
		map.request(w);
		REQUIRE(settle(map, w));
		const std::shared_ptr<const TerrainSnapshot> s = map.snapshot();
		const int row = 40;
		const double travelY = TerrainMap::travelYAtRow(s->window, row);
		const double travelX = TerrainMap::travelXAtColumn(s->window, 7);
		const auto zone = static_cast<uint16_t>(std::lround(s->window.zoneBaseline));
		const TerrainFeatures here = terrainFeaturesAt(
		    engine, s->window.pin, zone, static_cast<float>(s->window.offsetBaseline), travelX, travelY, kNoPlaneResidue,
		    s->window.gammaBase, s->window.scales(), s->window.warpMode, s->window.weaveModel);
		const int family = engine == OscType::Stair ? stairFamilyAt(zone) : -1;
		CAPTURE(static_cast<int>(engine));
		CHECK(s->texel(7, row)
		      == terrainColour(here, travelX, travelY, family, colourMapFor(engine), kShippedSecondAxis, engine));
		if (engine == OscType::BasicShapes) {
			// ITS TERRAIN IS ONE CYCLE AT FM DEPTH 0, so without the position-keyed hue the whole map would be one tint: it is a rainbow.
			std::set<uint32_t> distinct;
			for (int x = 0; x < TerrainMap::kResolution; x += 4) {
				distinct.insert(s->texel(x, row));
			}
			CHECK(distinct.size() > 8);
		}
	}
}

TEST_CASE("SHOT terrain palettes: every engine before and after, and the Additive alternates, one position and zoom") {
	// NOT A CASE — A CAMERA, gated on FEATHERS_DUMP_DIR: writes terrain-<engine>-before/after and the Additive alternates as .ppm
	// of the same window per engine.
	const char* out = std::getenv("FEATHERS_DUMP_DIR");
	if (out == nullptr) {
		return;
	}
	constexpr int kSide = TerrainMap::kResolution;
	struct Look {
		const char* name;
		OscType engine;
		ColourMap map;
		OscType colourAs = OscType::kCount; // kCount: the engine itself; Morph reproduces the shared-knots colouring Additive had before backlog LY
	};
	// BEFORE (the Nebula map at its own glow, what every engine drew) AND AFTER (the engine's own), per engine; plus the two
	// Additive alternates.
	const Look looks[] = {
		{ "morph-before", OscType::Morph, ColourMap::Nebula }, { "morph-after", OscType::Morph, colourMapFor(OscType::Morph) },
		{ "stair-before", OscType::Stair, ColourMap::Nebula }, { "stair-after", OscType::Stair, colourMapFor(OscType::Stair) },
		{ "v5-before", OscType::V5, ColourMap::Nebula }, { "v5-after", OscType::V5, colourMapFor(OscType::V5) },
		{ "resonate-before", OscType::Resonate, ColourMap::Nebula }, { "resonate-after", OscType::Resonate, colourMapFor(OscType::Resonate) },
		{ "basic-before", OscType::BasicShapes, ColourMap::Nebula }, { "basic-after", OscType::BasicShapes, colourMapFor(OscType::BasicShapes) },
		{ "additive-before", OscType::Additive, ColourMap::Nebula }, { "additive-crt", OscType::Additive, ColourMap::Crt },
		{ "additive-crt-oldnorm", OscType::Additive, ColourMap::Crt, OscType::Morph },
		{ "additive-synthwave", OscType::Additive, ColourMap::Synthwave }, { "additive-aurora", OscType::Additive, ColourMap::Aurora },
	};
	for (const Look& look : looks) {
		TerrainWindow window = windowAt(0.3, 1.0, 1.0);
		window.engine = look.engine;
		window.zoneBaseline = 512.0;
		std::vector<unsigned char> rgb(static_cast<size_t>(kSide) * kSide * 3);
		for (int row = 0; row < kSide; ++row) {
			const double travelY = TerrainMap::travelYAtRow(window, row);
			const double home = window.gammaBase;
			const auto frames = terrainFramesAt(window.pin, home, kNoPlaneResidue);
			const auto zone = static_cast<uint16_t>(std::clamp(std::lround(home * 1023.0), 0L, 1023L));
			const double base = home - static_cast<double>(zone) / 1023.0;
			for (int column = 0; column < kSide; ++column) {
				const double travelX = TerrainMap::travelXAtColumn(window, column);
				const TerrainFeatures f = terrainFeaturesOn(look.engine, frames, zone, 0.0f, travelX, travelY, base);
				const uint32_t c = terrainColour(f, travelX, travelY, look.engine == OscType::Stair ? stairFamilyAt(zone) : -1, look.map, kShippedSecondAxis, look.colourAs == OscType::kCount ? look.engine : look.colourAs);
				const size_t at = (static_cast<size_t>(row) * kSide + column) * 3;
				rgb[at] = static_cast<unsigned char>((c >> 16) & 0xFF);
				rgb[at + 1] = static_cast<unsigned char>((c >> 8) & 0xFF);
				rgb[at + 2] = static_cast<unsigned char>(c & 0xFF);
			}
		}
		const std::string path = std::string(out) + "/terrain-" + look.name + ".ppm";
		std::FILE* file = std::fopen(path.c_str(), "wb");
		REQUIRE(file != nullptr);
		std::fprintf(file, "P6\n%d %d\n255\n", kSide, kSide);
		std::fwrite(rgb.data(), 1, rgb.size(), file);
		std::fclose(file);
	}
	// AND THE SPECTRUM MAP ITSELF, tilt across and the second axis down (Basic Shapes' own terrain is nearly flat, so it shows one hue).
	for (const ColourMap map : { ColourMap::Spectrum, ColourMap::Gilt }) {
		constexpr int kGrid = 128;
		std::vector<unsigned char> rgb(static_cast<size_t>(kGrid) * kGrid * 3);
		for (int y = 0; y < kGrid; ++y) {
			for (int x = 0; x < kGrid; ++x) {
				const MapRgb c = colourMapAt(map, static_cast<float>(x) / (kGrid - 1), static_cast<float>(y) / (kGrid - 1));
				const size_t at = (static_cast<size_t>(y) * kGrid + x) * 3;
				rgb[at] = static_cast<unsigned char>(c.r);
				rgb[at + 1] = static_cast<unsigned char>(c.g);
				rgb[at + 2] = static_cast<unsigned char>(c.b);
			}
		}
		const std::string path = std::string(out) + (map == ColourMap::Spectrum ? "/map-spectrum.ppm" : "/map-gilt.ppm");
		std::FILE* file = std::fopen(path.c_str(), "wb");
		REQUIRE(file != nullptr);
		std::fprintf(file, "P6\n%d %d\n255\n", kGrid, kGrid);
		std::fwrite(rgb.data(), 1, rgb.size(), file);
		std::fclose(file);
	}
	MESSAGE("wrote the terrain palettes to " << out);
}

TEST_CASE("OE: the Vocode analysis cell — a compact Resolution box, the controls above one bottom caption, the follower stack titled apart") {
	// FOUND AUDITIONING OE: the Resolution dropdown was too large and the "Analysis" tag sat under the pills only. The box is now a
	// COMPACT one-row ValueBox, and "Analysis" is the column's own bottom caption (the house pattern: "Partials", "Tone").
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	const size_t selected = f.selection.slot();
	f.set(soundParam(selected, SoundField::Enable), 1.0);
	// OE2: THE ANALYSIS PANEL IS IN THE ROW ONLY WHILE SOME SOUND VOCODES.
	f.set(drawerParam(selected, SpectralControl::VocodeOn), 1.0);
	f.update();
	ui::VocodeAnalysisColumn* column = f.panel.vocodeColumnForTest(selected);
	ui::PillStack* times = f.panel.vocodeTimesForTest(selected);
	REQUIRE(column != nullptr);
	REQUIRE(times != nullptr);
	CHECK(column->resolution().isCompact());
	CHECK(column->resolution().height() == doctest::Approx(ui::ValueBox::kCompactHeight));
	CHECK(column->resolution().y() == doctest::Approx(0.0f));
	// THE CHIPS SIT UNDER THE BOX, and both end above the caption's line, so nothing overlaps "Analysis".
	const float captionTop = column->height() - ui::PillStack::kReadoutHeight;
	CHECK(column->freeze().y() >= column->resolution().y() + column->resolution().height());
	CHECK(column->keyTrack().y() == doctest::Approx(column->freeze().y()));
	CHECK(column->freeze().y() + column->freeze().height() <= captionTop);
	CHECK(std::string(ui::VocodeAnalysisColumn::kCaption) == "Analysis");
	// THE STACK BESIDE IT NO LONGER CLAIMS THE WORD.
	CHECK(times->title() == "Follower");
	// NO TWO CAPTIONS READ ALIKE: the Depth/Flatten/Gate stack and the Low/High stack were both titled "Vocode".
	const auto* depthStack = dynamic_cast<ui::PillStack*>(f.panel.pillForTest(selected, SpectralControl::VocodeDepth)->parent());
	const auto* rangeStack = dynamic_cast<ui::PillStack*>(f.panel.pillForTest(selected, SpectralControl::VocodeLow)->parent());
	REQUIRE(depthStack != nullptr);
	REQUIRE(rangeStack != nullptr);
	CHECK(depthStack->title() == "Vocode");
	CHECK(rangeStack->title() == "Range");
}

//-------//
// Likes //
//-------//

namespace {
/// Wait until a map holds the whole of a window built through REFINED likes — defined with the shaped-lens cases below.
bool settleRefinedLikes(TerrainMap& map, const TerrainWindow& window);
} // namespace

namespace {

/// Put the selected sound somewhere that is not the origin, by the dice, and let the gesture land.
void walkOffOrigin(ExploreFixture& f, int rolls) {
	for (int i = 0; i < rolls; ++i) {
		f.stage.mutate();
		f.settleGesture();
	}
}

} // namespace

TEST_CASE("ON: the heart likes the star's place with the envelope measured there, a second press removes it, each one undo step, and the patch is modified") {
	ExploreFixture f;
	f.selection.select(1);
	walkOffOrigin(f, 3);
	const double x = f.stage.selectedDrift();
	const double y = f.stage.selectedDriftY();
	REQUIRE((x != 0.0 || y != 0.0));
	(void)f.bridge.takeStateDirty();
	const size_t entries = f.stage.historyLength();

	// THE BUTTON IS WHERE IT IS DRAWN and is the heart: a press there is a like, not a scrub of the chart under it.
	const visage::Bounds heart = f.stage.partBounds(ExploreStage::Part::Like);
	REQUIRE(heart.width() > 0.0f);
	CHECK(f.stage.hitAt({ heart.x() + heart.width() * 0.5f, heart.y() + heart.height() * 0.5f })
	      == ExploreStage::Part::Like);
	// AND IT SITS ON MUTATE'S LEFT, clear of it.
	const visage::Bounds mutate = f.stage.partBounds(ExploreStage::Part::Mutate);
	CHECK(heart.right() < mutate.x());
	CHECK(heart.y() == mutate.y());
	CHECK_FALSE(f.stage.likeUnderStar().has_value());

	f.stage.toggleLike();
	const std::vector<feathers::TerrainLike>& likes = f.bridge.terrainLikes().likes;
	REQUIRE(likes.size() == 1);
	// THE PLACE, IN REAL COORDINATES, AND THE PLANE'S HOME.
	CHECK(likes[0].x == x);
	CHECK(likes[0].y == y);
	const TerrainWindow window = f.stage.terrainWindowNow();
	CHECK(likes[0].home == terrainPlaneHome(window));
	// THE SPECTRUM IS THE FINE CYCLE BUILT THERE: the builder's road at kLikeFineCycle points, and — restated by Parseval,
	// independently of the binning — its bins hold the cycle's AC power (twice, the negative frequencies) bar the Nyquist bin.
	CHECK(likes[0].spectrum == terrainFineSpectrumAt(window, x, y));
	{
		std::array<float, kLikeFineCycle> cycle {};
		terrainCycleOn(window.engine, terrainFramesAt(window.pin, terrainPlaneHome(window), window.residue),
		               static_cast<uint16_t>(std::lround(window.zoneBaseline)), static_cast<float>(window.offsetBaseline), x, y,
		               window.gammaBase, window.scales(), window.warpMode, window.weaveModel, window.resonateShaping,
		               window.shapesMix, window.additive, cycle);
		double mean = 0.0;
		for (const float s : cycle) {
			mean += s / static_cast<double>(cycle.size());
		}
		double ac = 0.0;
		for (const float s : cycle) {
			ac += (s - mean) * (s - mean) / static_cast<double>(cycle.size());
		}
		double binned = 0.0;
		for (const float p : likes[0].spectrum) {
			binned += 2.0 * p;
		}
		CHECK(ac > 0.0);
		CHECK(binned == doctest::Approx(ac).epsilon(0.02));
	}
	// AN EDIT: the host is told the patch moved, and it is one undo step.
	CHECK(f.bridge.takeStateDirty());
	CHECK(f.stage.historyLength() == entries + 1);
	CHECK(f.stage.likeUnderStar() == std::optional<size_t> { 0 });
	const feathers::TerrainLike made = likes[0];

	// A SECOND PRESS ON THE SAME PLACE REMOVES IT.
	f.stage.toggleLike();
	CHECK(f.bridge.terrainLikes().likes.empty());
	CHECK(f.bridge.takeStateDirty());
	CHECK(f.stage.historyLength() == entries + 2);

	// UNDO PUTS IT BACK VALUE FOR VALUE, envelope included; undo again takes it away; redo returns it.
	f.stage.undo();
	REQUIRE(f.bridge.terrainLikes().likes.size() == 1);
	CHECK(f.bridge.terrainLikes().likes[0] == made);
	f.stage.undo();
	CHECK(f.bridge.terrainLikes().likes.empty());
	f.stage.redo();
	REQUIRE(f.bridge.terrainLikes().likes.size() == 1);
	CHECK(f.bridge.terrainLikes().likes[0] == made);
}

TEST_CASE("ON: a like's mark is drawn on the plane it was made on and not on another, and a second like elsewhere is a second mark") {
	ExploreFixture f;
	f.selection.select(1);
	visage::Canvas canvas;
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	walkOffOrigin(f, 1);
	REQUIRE_FALSE(f.stage.likeUnderStar().has_value());
	f.stage.toggleLike();
	REQUIRE(f.bridge.terrainLikes().likes.size() == 2);
	f.stage.draw(canvas);
	CHECK(f.stage.likeMarksDrawn() == 2);
	// ANOTHER PLANE: the gamma corner moves the home every texel is taken at, so the marks are of somewhere else.
	f.stage.nudgeSelectedGamma(1.0);
	f.settleGesture();
	f.stage.draw(canvas);
	CHECK(f.stage.likeMarksDrawn() == 0);
	CHECK_FALSE(f.stage.likeUnderStar().has_value());
	// AND BACK: the corner turned back lands on the stored number, and both marks return.
	f.stage.nudgeSelectedGamma(-1.0);
	f.settleGesture();
	f.stage.draw(canvas);
	CHECK(f.stage.likeMarksDrawn() == 2);
}

//-------------------//
// Likes: the score  //
//-------------------//

namespace {

/// A FAMILY OF PARTIAL ENVELOPES with two shape knobs — a roll-off and a ripple — and a level: enough variety that a map
/// of them has a covariance worth whitening, and a closed form, so a case can name the envelope a place has.
LikeSpectrum envelopeOf(double rolloff, double ripple, double level) {
	// THE FAMILY AS A FINE SPECTRUM: harmonic k's power in its sixth-octave bin, the likes' own grid.
	LikeSpectrum s {};
	for (int k = 1; k < 256; ++k) {
		const double n = static_cast<double>(k);
		const double m = level * std::pow(n, -rolloff) * (1.0 + 0.8 * ripple * std::sin(1.7 * n));
		s[static_cast<size_t>(std::floor(6.0 * std::log2(n) + 1e-9))] += static_cast<float>(m * m);
	}
	return s;
}

/// THE MAP: an 8x8 grid of the family over its two shape knobs and a slow level drift — a stand-in for the pre-pass.
std::vector<LikeVector> familyMap() {
	std::vector<LikeVector> map;
	for (int j = 0; j < kLikePrepass; ++j) {
		for (int i = 0; i < kLikePrepass; ++i) {
			map.push_back(likeFingerprint(envelopeOf(0.4 + 0.25 * i, -0.9 + 0.25 * j, 0.3 + 0.02 * (i + j))));
		}
	}
	return map;
}

/// A deterministic direction per index — a hash, not an RNG, so a case reads the same every run.
LikeVector scatteredDirection(size_t index) {
	LikeVector d {};
	uint32_t h = 2166136261u ^ static_cast<uint32_t>(index * 2654435761u);
	for (double& x : d) {
		h ^= h << 13;
		h ^= h >> 17;
		h ^= h << 5;
		x = static_cast<double>(h) / 4294967296.0 - 0.5;
	}
	return d;
}

LikeVector plus(const LikeVector& a, const LikeVector& b, double t = 1.0) {
	LikeVector out = a;
	for (size_t i = 0; i < out.size(); ++i) {
		out[i] += t * b[i];
	}
	return out;
}

} // namespace

TEST_CASE("ON score: a like's own envelope scores the maximum, the same partial shape found anywhere scores high, and a different shape scores low") {
	// A LIKE IS ABOUT THE SOUND, NOT THE PLACE: the model sees envelopes and nothing else, so "distant" is whatever map
	// position carries the same shape — here the like's own envelope at a 4% different level, as a far texel would.
	LikeModelInputs inputs;
	inputs.map = familyMap();
	const LikeSpectrum liked = envelopeOf(0.9, 0.35, 0.4);
	inputs.likes.push_back(likeFingerprint(liked));
	const LikeModel model = buildLikeModel(inputs);
	REQUIRE_FALSE(model.empty());
	CHECK(model.removed == 0);
	CHECK(model.axes == kLikeDims);

	const double own = model.score(likeFingerprint(liked));
	CHECK(own == doctest::Approx(1.0).epsilon(1e-12));
	for (const LikeVector& f : inputs.map) {
		CHECK(model.score(f) <= own);
	}
	const double sameShape = model.score(likeFingerprint(envelopeOf(0.9, 0.35, 0.4 * 1.04)));
	const double otherShape = model.score(likeFingerprint(envelopeOf(1.9, -0.8, 0.4)));
	INFO("same shape, 4% louder: " << sameShape << "; another shape at the like's level: " << otherShape);
	CHECK(sameShape > 0.8);
	CHECK(otherShape < 0.1);
}

TEST_CASE("ON score: a control that moves every place the same way is projected out and the score cannot see it; one whose effect varies stays; one that does nothing is ignored") {
	LikeModelInputs inputs;
	inputs.map = familyMap();
	inputs.likes.push_back(likeFingerprint(envelopeOf(0.9, 0.35, 0.4)));
	inputs.likes.push_back(likeFingerprint(envelopeOf(1.1, 0.1, 0.42)));
	// A: A LOUDNESS-AND-TILT KNOB, the same direction at every place (the knob's job, not the place's).
	LikeVector global {};
	for (size_t k = 0; k < static_cast<size_t>(kLikeDims); ++k) {
		global[k] = -0.03 * static_cast<double>(k);
	}
	global[kLikeDims - 1] = 0.2;
	// B: A KNOB WHOSE EFFECT DEPENDS ON WHERE YOU ARE — a different direction at every place.
	// C: A KNOB THIS ENGINE DOES NOT READ.
	std::vector<LikeVector> a;
	std::vector<LikeVector> b;
	for (size_t p = 0; p < inputs.map.size(); ++p) {
		a.push_back(plus(inputs.map[p], global));
		b.push_back(plus(inputs.map[p], scatteredDirection(p), 0.1));
	}
	inputs.nudged = { a, b, inputs.map };
	const LikeModel model = buildLikeModel(inputs);
	INFO("consistency A " << model.consistency[0] << ", B " << model.consistency[1] << ", C " << model.consistency[2]);
	CHECK(model.consistency[0] == doctest::Approx(1.0).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	CHECK(model.projected[0]);
	CHECK(model.consistency[1] < kLikeConsistentAt);
	CHECK_FALSE(model.projected[1]);
	CHECK(model.consistency[2] == -1.0);
	CHECK_FALSE(model.projected[2]);
	CHECK(model.removed == 1);
	CHECK(model.axes == kLikeDims - 1);
	// THE SCORE IS BLIND TO THE GLOBAL DIRECTION — turning that knob any distance changes nothing it reads — and is not
	// blind to the other (so the first claim is not vacuous).
	const LikeVector probe = likeFingerprint(envelopeOf(1.0, 0.2, 0.41));
	const double at = model.score(probe);
	for (const double t : { -3.0, 0.5, 2.0 }) {
		CHECK(model.score(plus(probe, global, t)) == doctest::Approx(at).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	}
	CHECK(std::abs(model.score(plus(probe, scatteredDirection(3), 0.5)) - at) > 1e-3);
}

TEST_CASE("ON score: the projection is capped at half the dimensions, most consistent first, and the residual is reported") {
	LikeModelInputs inputs;
	inputs.map = familyMap();
	inputs.likes.push_back(likeFingerprint(envelopeOf(0.9, 0.35, 0.4)));
	// THREE MORE GLOBAL KNOBS THAN THE CAP, each the same everywhere but one, which is slightly less consistent than the rest.
	for (size_t c = 0; c < static_cast<size_t>(kLikeMaxRemoved) + 3; ++c) {
		std::vector<LikeVector> moved;
		for (size_t p = 0; p < inputs.map.size(); ++p) {
			LikeVector step = scatteredDirection(100 + c);
			if (c == 4) {
				step = plus(step, scatteredDirection(1000 + p), 0.6); // part global, part local
			}
			moved.push_back(plus(inputs.map[p], step, 0.05));
		}
		inputs.nudged.push_back(moved);
	}
	const LikeModel model = buildLikeModel(inputs);
	CHECK(model.removed == kLikeMaxRemoved);
	CHECK(model.axes == kLikeDims - kLikeMaxRemoved);
	// THE LESS CONSISTENT ONE GOES LAST, so with three more candidates than places it is not among them.
	CHECK(model.consistency[4] < model.consistency[0]);
	CHECK_FALSE(model.projected[4]);
	CHECK(model.effectiveDims > 0.0);
	CHECK(model.effectiveDims <= static_cast<double>(model.axes) + 1e-9);
}

TEST_CASE("ON score: what the likes agree on weighs more than what they scatter on") {
	// TWO SETS OF THREE LIKES THAT BOTH HOLD (roll-off 1.1, ripple 0.3): one agrees on the ripple and scatters on the
	// roll-off, the other the reverse. For each candidate the nearest like is that shared one in both sets, so without the
	// weights both sets would measure it at exactly the same distance; with them, a step off the shared ripple is FARTHER
	// under the set that agrees on ripple, and a step off the roll-off is nearer — the likes have said which matters.
	const auto modelOf = [](std::initializer_list<std::pair<double, double>> likes) {
		LikeModelInputs inputs;
		inputs.map = familyMap();
		for (const auto& [rolloff, ripple] : likes) {
			inputs.likes.push_back(likeFingerprint(envelopeOf(rolloff, ripple, 0.4)));
		}
		return buildLikeModel(inputs);
	};
	const LikeModel rippleAgreed = modelOf({ { 0.6, 0.3 }, { 1.1, 0.3 }, { 1.6, 0.3 } });
	const LikeModel rolloffAgreed = modelOf({ { 1.1, -0.2 }, { 1.1, 0.3 }, { 1.1, 0.8 } });
	const LikeVector offRipple = likeFingerprint(envelopeOf(1.1, 0.4, 0.4));
	const LikeVector offRolloff = likeFingerprint(envelopeOf(1.2, 0.3, 0.4));
	INFO("off the ripple: " << rippleAgreed.nearestSquared(offRipple) << " vs " << rolloffAgreed.nearestSquared(offRipple)
	                        << "; off the roll-off: " << rippleAgreed.nearestSquared(offRolloff) << " vs "
	                        << rolloffAgreed.nearestSquared(offRolloff) << "; effective dims " << rippleAgreed.effectiveDims
	                        << " of " << rippleAgreed.axes);
	CHECK(rippleAgreed.nearestSquared(offRipple) > 1.5 * rolloffAgreed.nearestSquared(offRipple));
	CHECK(rippleAgreed.nearestSquared(offRolloff) * 1.5 < rolloffAgreed.nearestSquared(offRolloff));
	CHECK(rippleAgreed.effectiveDims < static_cast<double>(rippleAgreed.axes) - 1e-3);
}

TEST_CASE("ON score: inside the bake, every texel's score is read off the warp's coarse grid at the real place drawn there, the like's own vertex carries room, and a map with no likes carries none" * doctest::skip(!kLikesMoveThePlane)) {
	TerrainWindow window = windowAt(0.004, ExploreStage::kBaseTravel / 8.0, ExploreStage::kBaseTravel / 8.0);
	window.engine = OscType::V5;
	const double x = TerrainMap::travelXAtColumn(window, 40);
	const double y = TerrainMap::travelYAtRow(window, 70);
	feathers::TerrainLike like;
	like.x = x;
	like.y = y;
	like.home = terrainPlaneHome(window);
	like.spectrum = terrainFineSpectrumAt(window, x, y);

	// WITHOUT LIKES: no model, no scores.
	TerrainMap plain;
	plain.request(window);
	REQUIRE(settleRefined(plain, window));
	CHECK(plain.snapshot()->likeState == nullptr);
	CHECK(plain.snapshot()->score.empty());

	window.likes = std::make_shared<const std::vector<feathers::TerrainLike>>(std::vector { like });
	// THE VIEW IS CENTRED WHERE THE WARP DRAWS THE LIKE (it moves the drawn place by up to a few cells), as the stage centres
	// on the star — the likes and the world, not the view, decide the warp, so this changes nothing else.
	{
		const WarpPoint shown = deriveTerrainLikes(window)->warp.viewAt({ x, y });
		window.travelXCentre = shown.x;
		window.travelYCentre = shown.y;
	}
	TerrainMap map;
	map.request(window);
	REQUIRE(settleRefined(map, window));
	const std::shared_ptr<const TerrainSnapshot> with = map.snapshot();
	REQUIRE(with->likeState != nullptr);
	REQUIRE(with->score.size() == with->texels.size());
	REQUIRE(with->samples == 1); // one sample a texel, so the texel's envelope IS the one built at its centre
	const LikeModel& model = with->likeState->model;
	const TerrainWarp& warp = with->likeState->warp;
	INFO("derivation " << with->likeState->millis << " ms over " << with->likeState->evaluations << " cycles; removed "
	                   << model.removed << ", axes " << model.axes << ", effective " << model.effectiveDims);
	CHECK(model.score(likeFingerprint(like.spectrum)) == doctest::Approx(1.0).epsilon(1e-12));
	// EVERY TEXEL'S SCORE IS READ OFF THE COARSE GRID (the fine fingerprint is never taken per texel): restated here as the
	// bilinear of the derivation's lattice at the real place the warp draws at the texel's centre.
	const LikeDerivation& d = *with->likeState;
	const double step = 2.0 * d.region.half / kWarpCells;
	const auto lattice = [&](int i, int j) {
		return d.latticeScore[static_cast<size_t>(j) * (kWarpCells + 1) + static_cast<size_t>(i)];
	};
	int checked = 0;
	for (const int r : { 3, 64, 70, 127 }) {
		for (const int c : { 0, 40, 77, 126 }) {
			const WarpPoint real =
			    warp.worldAt({ TerrainMap::travelXAtColumn(window, c), TerrainMap::travelYAtRow(window, r) });
			const double u = (real.x - (d.region.centreX - d.region.half)) / step;
			const double v = (real.y - (d.region.centreY - d.region.half)) / step;
			double expected = 0.0;
			if (u >= 0.0 && v >= 0.0 && u <= kWarpCells && v <= kWarpCells) {
				const int i = std::min(static_cast<int>(u), kWarpCells - 1);
				const int j = std::min(static_cast<int>(v), kWarpCells - 1);
				const double s = u - i;
				const double t = v - j;
				expected = (1 - s) * (1 - t) * lattice(i, j) + s * (1 - t) * lattice(i + 1, j) + s * t * lattice(i + 1, j + 1)
				           + (1 - s) * t * lattice(i, j + 1);
				++checked;
			}
			CHECK(with->score[static_cast<size_t>(r) * TerrainMap::kResolution + c] == doctest::Approx(expected).epsilon(1e-6));
		}
	}
	CHECK(checked > 0);
	// THE LATTICE VERTEX NEAREST THE LIKE carries its room: within half a cell of the like, so at least the bump's there.
	const int li = static_cast<int>(std::lround((x - (d.region.centreX - d.region.half)) / step));
	const int lj = static_cast<int>(std::lround((y - (d.region.centreY - d.region.half)) / step));
	CHECK(lattice(li, lj) >= std::exp(-0.5 / (1.5 * 1.5)) - 1e-9);
}

TEST_CASE("MEASURE ON: the bake's cost with likes against without, paired and interleaved" * doctest::skip()) {
	// RUN BY HAND: build/tests/test_gui_widgets_explore --no-skip -tc="MEASURE ON*". The shared host's load swings, so
	// the two arms alternate and the medians are compared.
	for (const OscType engine : { OscType::V5, OscType::Morph }) {
		const double span = ExploreStage::kBaseTravel * (std::getenv("FEATHERS_MEASURE_SPAN") != nullptr ? std::atof(std::getenv("FEATHERS_MEASURE_SPAN")) : 1.0);
		TerrainWindow base = windowAt(0.004, span, span);
		base.engine = engine;
		feathers::TerrainLike like;
		like.home = terrainPlaneHome(base);
		like.spectrum = terrainFineSpectrumAt(base, 0.003, -0.002);
		like.x = 0.003;
		like.y = -0.002;
		std::vector<double> plainMs;
		std::vector<double> likedMs;
		std::vector<double> deriveMs;
		for (int pair = 0; pair < 6; ++pair) {
			for (const bool liked : { false, true }) {
				TerrainWindow window = base;
				window.travelXCentre = 0.0001 * pair; // a fresh map each time, nothing carried
				if (liked) {
					window.likes = std::make_shared<const std::vector<feathers::TerrainLike>>(std::vector { like });
				}
				TerrainMap map;
				const auto started = std::chrono::steady_clock::now();
				map.request(window);
				REQUIRE(settleRefined(map, window));
				const double ms =
				    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
				(liked ? likedMs : plainMs).push_back(ms);
				if (liked) {
					deriveMs.push_back(map.snapshot()->likeState->millis);
					const LikeModel& m = map.snapshot()->likeState->model;
					if (pair == 0) {
						std::string projected;
						for (size_t c = 0; c < m.projected.size(); ++c) {
							if (m.consistency[c] >= 0.0) {
								projected += std::string(" ") + terrainLikeControls()[c] + "=" + std::to_string(m.consistency[c]).substr(0, 5);
							}
							if (m.projected[c]) {
								projected += std::string(" ") + terrainLikeControls()[c];
							}
						}
						MESSAGE("engine " << static_cast<int>(engine) << ": removed " << m.removed << " (" << projected
						                  << " ) [consistency of each control that moved anything; projected ones repeat], axes " << m.axes << ", effective " << m.effectiveDims);
					}
				}
			}
		}
		const auto median = [](std::vector<double> v) {
			std::sort(v.begin(), v.end());
			return v[v.size() / 2];
		};
		MESSAGE("engine " << static_cast<int>(engine) << ": without " << median(plainMs) << " ms, with one like "
		                  << median(likedMs) << " ms (of which the derivation " << median(deriveMs) << " ms)");
	}
}

//------------------//
// Likes: the warp  //
//------------------//

namespace {

/// A SCORE FIELD with one liked hill, on the region's lattice — the shape a single like gives the solver.
std::vector<double> hillScore(const LikeRegion& region, int n, double atX, double atY, double width) {
	std::vector<double> score;
	const double step = 2.0 * region.half / n;
	for (int j = 0; j <= n; ++j) {
		for (int i = 0; i <= n; ++i) {
			const double x = region.centreX - region.half + i * step;
			const double y = region.centreY - region.half + j * step;
			score.push_back(std::exp(-((x - atX) * (x - atX) + (y - atY) * (y - atY)) / (width * width)));
		}
	}
	return score;
}

/// The warp's Jacobian determinant at a view point, by central differences.
double jacobianAt(const TerrainWarp& warp, double x, double y, double h) {
	const WarpPoint px = warp.worldAt({ x + h, y });
	const WarpPoint mx = warp.worldAt({ x - h, y });
	const WarpPoint py = warp.worldAt({ x, y + h });
	const WarpPoint my = warp.worldAt({ x, y - h });
	return ((px.x - mx.x) * (py.y - my.y) - (px.y - mx.y) * (py.x - my.x)) / (4.0 * h * h);
}

/// THE ROOM A PLACE IS GIVEN, as an area rather than at a point: the world area the view's square of side `side` centred
/// on `view` covers, over the square's own area (< 1 is magnified). Pointwise Jacobians jump across cell edges.
double worldPerViewAround(const TerrainWarp& warp, WarpPoint view, double side) {
	constexpr int k = 16;
	std::vector<WarpPoint> ring;
	for (int s = 0; s < 4 * k; ++s) {
		const int edge = s / k;
		const double t = static_cast<double>(s % k) / k;
		const double h = side * 0.5;
		const WarpPoint p = edge == 0 ? WarpPoint { view.x - h + t * side, view.y - h }
		                    : edge == 1 ? WarpPoint { view.x + h, view.y - h + t * side }
		                    : edge == 2 ? WarpPoint { view.x + h - t * side, view.y + h }
		                                : WarpPoint { view.x - h, view.y + h - t * side };
		ring.push_back(warp.worldAt(p));
	}
	double area = 0.0;
	for (size_t i = 0; i < ring.size(); ++i) {
		const WarpPoint& a = ring[i];
		const WarpPoint& b = ring[(i + 1) % ring.size()];
		area += a.x * b.y - b.x * a.y;
	}
	return 0.5 * area / (side * side);
}

} // namespace

TEST_CASE("ON warp: a uniform score is the identity to the bit, and a map with no likes is the map without the feature, to the bit") {
	const LikeRegion region { 0.01, -0.02, kLikeRegionHalfMin };
	const TerrainWarp flat = solveTerrainWarp(region, kWarpCells, std::vector<double>((kWarpCells + 1) * (kWarpCells + 1), 0.4));
	const double step = 2.0 * region.half / kWarpCells;
	for (int j = 0; j <= kWarpCells; ++j) {
		for (int i = 0; i <= kWarpCells; ++i) {
			const WarpPoint v = flat.world[static_cast<size_t>(j) * (kWarpCells + 1) + i];
			CHECK(v.x == region.centreX - region.half + i * step);
			CHECK(v.y == region.centreY - region.half + j * step);
		}
	}
	// NO LIKES: a window whose likes are null and one whose likes are an empty list build the same bytes, carry no model,
	// and are the same window (the content key compares likes by content).
	TerrainWindow window = windowAt(0.004, ExploreStage::kBaseTravel / 4.0, ExploreStage::kBaseTravel / 4.0);
	window.engine = OscType::V5;
	TerrainMap none;
	none.request(window);
	REQUIRE(settleRefined(none, window));
	TerrainWindow empty = window;
	empty.likes = std::make_shared<const std::vector<feathers::TerrainLike>>();
	CHECK(empty == window);
	TerrainMap emptied;
	emptied.request(empty);
	REQUIRE(settleRefined(emptied, empty));
	CHECK(emptied.snapshot()->likeState == nullptr);
	CHECK(emptied.snapshot()->texels == none.snapshot()->texels);
}

TEST_CASE("ON warp: one liked hill is given room — the cells over it cover less world — and no cell folds or dents anywhere, nor does the map between them") {
	const LikeRegion region { 0.0, 0.0, kLikeRegionHalfMin };
	const double hillX = 0.03;
	const double hillY = -0.02;
	const TerrainWarp warp =
	    solveTerrainWarp(region, kWarpCells, hillScore(region, kWarpCells, hillX, hillY, region.half * 0.25));
	// NEVER FOLDS: every cell strictly convex and positively oriented — the solver's own invariant — and the map's
	// Jacobian positive at every point of a fine sweep, cell interiors and edges alike.
	CHECK(warp.minimumShape() >= kWarpShape * (1.0 - 1e-9));
	const double cell = 2.0 * region.half / kWarpCells;
	double least = std::numeric_limits<double>::infinity();
	double most = 0.0;
	for (int j = 1; j < 4 * kWarpCells; ++j) {
		for (int i = 1; i < 4 * kWarpCells; ++i) {
			const double x = -region.half + i * cell / 4.0;
			const double y = -region.half + j * cell / 4.0;
			const double det = jacobianAt(warp, x, y, cell * 1e-3);
			least = std::min(least, det);
			most = std::max(most, det);
		}
	}
	INFO("Jacobian over the region: " << least << " .. " << most);
	CHECK(least > 0.0);
	// ROOM WHERE IT IS LIKED: around the hill a unit of the view covers less world (Jacobian < 1, drawn magnified); far
	// from it, more; and the boundary is fixed, so outside the region the map is the identity.
	const WarpPoint hillView = warp.viewAt({ hillX, hillY });
	CHECK(jacobianAt(warp, hillView.x, hillView.y, cell * 1e-3) < 0.6);
	CHECK(jacobianAt(warp, -region.half + cell, region.half - cell, cell * 1e-3) > 1.0);
	CHECK(warp.worldAt({ region.half + 1e-6, 0.0 }) == WarpPoint { region.half + 1e-6, 0.0 });
	// THE ROOM FLOOR holds: no corner below its share of an unwarped cell.
	CHECK(warp.maximumSqueeze() <= (1.0 + 1e-9) / (kWarpFloorLinear * kWarpFloorLinear));
}

TEST_CASE("ON warp: a real place round-trips through the warp, both ways, everywhere in the region") {
	const LikeRegion region { 0.005, 0.002, kLikeRegionHalfMin };
	const TerrainWarp warp = solveTerrainWarp(
	    region, kWarpCells, hillScore(region, kWarpCells, region.centreX - 0.04, region.centreY + 0.03, region.half * 0.2));
	double worst = 0.0;
	for (int j = 0; j < 37; ++j) {
		for (int i = 0; i < 37; ++i) {
			const WarpPoint p { region.centreX - region.half + (i + 0.37) * region.half / 18.5,
				                region.centreY - region.half + (j + 0.61) * region.half / 18.5 };
			const WarpPoint back = warp.worldAt(warp.viewAt(p));
			const WarpPoint fore = warp.viewAt(warp.worldAt(p));
			worst = std::max({ worst, std::hypot(back.x - p.x, back.y - p.y), std::hypot(fore.x - p.x, fore.y - p.y) });
		}
	}
	INFO("worst round trip " << worst << " of a region " << 2.0 * region.half << " wide");
	CHECK(worst < 1e-12);
}

TEST_CASE("ON warp: the bake samples the warped world, the stage draws the star where its real place lands, a press on it means that real place, and the place itself is never moved by drawing" * doctest::skip(!kLikesMoveThePlane)) {
	ExploreFixture f;
	f.selection.select(1);
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	walkOffOrigin(f, 1);
	f.stage.toggleLike();
	const double x = f.stage.selectedDrift();
	const double y = f.stage.selectedDriftY();
	// THE MAP THE STAGE ASKS FOR, built, with the likes' warp in it.
	const TerrainWindow window = f.stage.terrainWindowNow();
	f.terrain.request(window);
	REQUIRE(settleRefinedLikes(f.terrain, window));
	const std::shared_ptr<const TerrainSnapshot> map = f.terrain.snapshot();
	REQUIRE(map->likeState != nullptr);
	const TerrainWarp& warp = map->likeState->warp;
	REQUIRE(warp.n == kWarpCells);
	// THE WARP IS NOT THE IDENTITY HERE — otherwise every claim below would be vacuous.
	double moved = 0.0;
	const double cell = 2.0 * warp.half / warp.n;
	for (int j = 0; j <= warp.n; ++j) {
		for (int i = 0; i <= warp.n; ++i) {
			const WarpPoint v = warp.world[static_cast<size_t>(j) * (warp.n + 1) + i];
			moved = std::max(moved, std::hypot(v.x - (warp.centreX - warp.half + i * cell),
			                                   v.y - (warp.centreY - warp.half + j * cell)));
		}
	}
	INFO("largest lattice displacement " << moved / cell << " cells");
	REQUIRE(moved > 0.25 * cell);
	// THE BAKE: a texel at a view place is the character of the real place the warp draws there.
	REQUIRE(map->samples == 1);
	for (const auto& [column, row] : { std::pair { 30, 40 }, std::pair { 64, 64 }, std::pair { 101, 17 } }) {
		const double u = TerrainMap::travelXAtColumn(window, column);
		const double v = TerrainMap::travelYAtRow(window, row);
		const WarpPoint real = warp.worldAt({ u, v });
		const TerrainFeatures expected = terrainFeaturesAt(
		    window.engine, window.pin, static_cast<uint16_t>(std::lround(window.zoneBaseline)),
		    static_cast<float>(window.offsetBaseline), real.x, real.y, window.residue, window.gammaBase, window.scales(),
		    window.warpMode, window.weaveModel, window.resonateShaping, window.shapesMix, window.additive);
		{
			CHECK(map->featuresAt(column, row).centroid == doctest::Approx(expected.centroid).epsilon(1e-6));
			CHECK(map->featuresAt(column, row).energy == doctest::Approx(expected.energy).epsilon(1e-6));
		}
	}
	// THE STAR: drawn at its place's view position, and the view position carries back to the place.
	const WarpPoint drawn = f.stage.viewOf(x, y);
	CHECK(f.stage.viewDrift() == drawn.x);
	CHECK(f.stage.viewDriftY() == drawn.y);
	const WarpPoint back = f.stage.worldOf(drawn.x, drawn.y);
	CHECK(back.x == doctest::Approx(x).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	CHECK(back.y == doctest::Approx(y).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	// DRAWING MOVES NOTHING: a frame drawn through the warp leaves the stored place exactly where it was.
	visage::Canvas canvas;
	f.stage.draw(canvas);
	f.stage.update();
	CHECK(f.stage.selectedDrift() == x);
	CHECK(f.stage.selectedDriftY() == y);
	// A PRESS ON THE STAR means the star's own real place (to the precision of a pixel), not the unwarped point under it.
	const visage::Point star = f.stage.glassAtWorld(y, x);
	f.stage.onMouseDown().callback(at(star.x, star.y));
	f.stage.onMouseUp().callback(at(star.x, star.y));
	f.settleGesture();
	f.settleScrub();
	const double pixel = f.stage.travelSpan() / (2.0 * f.stage.disk().radius());
	CHECK(std::abs(f.stage.selectedDrift() - x) < 2.0 * pixel);
	CHECK(std::abs(f.stage.selectedDriftY() - y) < 2.0 * pixel);
	// AND THE HEART STILL FINDS THE LIKE UNDER THE STAR, through the warp.
	CHECK(f.stage.likeUnderStar().has_value());
}

//------------------------------//
// Likes: Mutate on the warp    //
//------------------------------//

TEST_CASE("ON mutate: with no likes a roll is today's — the seeded dice's draw added to the place, to the bit") {
	ExploreFixture f;
	f.selection.select(1);
	const double x = f.stage.selectedDrift();
	const double y = f.stage.selectedDriftY();
	// THE DICE, RESTATED: the stage's xorshift from its fixed seed, the vertical draw then the sideways one.
	uint32_t dice = 0x9E3779B9u;
	const auto roll = [&dice] {
		dice ^= dice << 13;
		dice ^= dice >> 17;
		dice ^= dice << 5;
		return static_cast<double>(dice) / 4294967296.0 * 2.0 - 1.0;
	};
	const double unit = roll();
	const double sideways = roll();
	f.stage.mutate();
	f.settleGesture();
	CHECK(f.stage.selectedDrift() == x + sideways * f.stage.mutationTravel());
	CHECK(f.stage.selectedDriftY() == y + unit * f.stage.mutationTravel());
}

TEST_CASE("ON mutate: under the likes' warp the roll is the same draw on the warped plane, carried back through the inverse warp" * doctest::skip(!kLikesMoveThePlane)) {
	// TWO STAGES ON THE SAME DICE: one with likes and their map built, one with none. Their rolls so far are the same
	// (the warp only exists once a map is built), so they stand at the same place with the same dice.
	ExploreFixture warped;
	ExploreFixture plain;
	warped.selection.select(1);
	plain.selection.select(1);
	walkOffOrigin(warped, 2);
	warped.stage.toggleLike();
	walkOffOrigin(warped, 1);
	warped.stage.toggleLike();
	walkOffOrigin(plain, 3);
	REQUIRE(warped.stage.selectedDrift() == plain.stage.selectedDrift());
	REQUIRE(warped.stage.selectedDriftY() == plain.stage.selectedDriftY());
	const TerrainWindow window = warped.stage.terrainWindowNow();
	warped.terrain.request(window);
	REQUIRE(settleRefinedLikes(warped.terrain, window));
	REQUIRE(warped.stage.likesDrawn() != nullptr);

	const double x = plain.stage.selectedDrift();
	const double y = plain.stage.selectedDriftY();
	const WarpPoint from = warped.stage.viewOf(x, y);
	plain.stage.mutate();
	plain.settleGesture();
	warped.stage.mutate();
	warped.settleGesture();
	const double drawX = plain.stage.selectedDrift() - x;
	const double drawY = plain.stage.selectedDriftY() - y;
	// THE SAME DRAW, IN WARPED SPACE: where the warped stage's star is now drawn, less where it was, is today's roll.
	const WarpPoint to = warped.stage.viewOf(warped.stage.selectedDrift(), warped.stage.selectedDriftY());
	CHECK(to.x - from.x == doctest::Approx(drawX).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	CHECK(to.y - from.y == doctest::Approx(drawY).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	// AND THE PLACE STORED IS THAT POINT CARRIED BACK THROUGH THE INVERSE WARP, exactly.
	const WarpPoint real = warped.stage.worldOf(from.x + drawX, from.y + drawY);
	CHECK(warped.stage.selectedDrift() == real.x);
	CHECK(warped.stage.selectedDriftY() == real.y);
	// NOT VACUOUS: in real coordinates the warped roll is not today's.
	const double apart = std::hypot(warped.stage.selectedDrift() - plain.stage.selectedDrift(),
	                                warped.stage.selectedDriftY() - plain.stage.selectedDriftY());
	INFO("the warped roll lands " << apart / std::hypot(drawX, drawY) << " of a roll from today's");
	CHECK(apart > 1e-3 * std::hypot(drawX, drawY));
}

//------------------------------//
// Likes: any surface           //
//------------------------------//

namespace {

/// A SURFACE THAT IS NOT THE CHARACTER TERRAIN: the envelope family over a plane, with two controls — a tilt that
/// steepens every place alike, and a twist that turns the plane's ripple by an amount that depends on where you are.
class ToySurface final : public LikeSurface {
public:
	void fingerprintsAt(std::span<const WarpPoint> points, std::vector<LikeVector>& out) const override {
		at(points, out, 0.0, 0.0);
	}
	size_t controlCount() const override { return 2; }
	const char* controlName(size_t control) const override { return control == 0 ? "tilt" : "twist"; }
	void nudgedFingerprintsAt(size_t control, std::span<const WarpPoint> points,
	                          std::vector<LikeVector>& out) const override {
		at(points, out, control == 0 ? 0.1 : 0.0, control == 1 ? 0.1 : 0.0);
	}

private:
	static void at(std::span<const WarpPoint> points, std::vector<LikeVector>& out, double gain, double twist) {
		for (const WarpPoint& p : points) {
			// THE TILT ACTS ON THE SHAPE ITSELF, the same step at every place.
			LikeVector f = likeFingerprint(envelopeOf(1.0 + 6.0 * p.x, 6.0 * p.y * (1.0 + twist * 8.0 * p.x), 0.4));
			for (int k = 0; k < kLikeDims; ++k) {
				f[k] -= gain * 3.0 * std::log(1.0 + k);
			}
			out.push_back(f);
		}
	}
};

} // namespace

TEST_CASE("ON surface: the score and the warp work on any surface — a global control is projected out, a like's place scores 1, and the warp gives it room") {
	const ToySurface surface;
	const std::vector<WarpPoint> places { { 0.02, -0.03 } };
	std::vector<LikeVector> prints;
	surface.fingerprintsAt(places, prints);
	const LikeDerivation d = deriveLikes(surface, places, prints);
	CHECK(d.region.centreX == places[0].x);
	CHECK(d.region.centreY == places[0].y);
	CHECK(d.evaluations == kLikePrepass * kLikePrepass * 3 + kSupportScan * kSupportScan + (kWarpCells + 1) * (kWarpCells + 1));
	REQUIRE(d.model.projected.size() == 2);
	INFO("consistency: tilt " << d.model.consistency[0] << ", twist " << d.model.consistency[1]);
	CHECK(d.model.projected[0]);       // the tilt: the same everywhere
	CHECK_FALSE(d.model.projected[1]); // the twist: depends on the place
	CHECK(d.model.score(prints[0]) == doctest::Approx(1.0).epsilon(1e-12));
	CHECK(d.warp.minimumShape() >= kWarpShape * (1.0 - 1e-9));
	CHECK(d.warp.maximumSqueeze() <= (1.0 + 1e-9) / (kWarpFloorLinear * kWarpFloorLinear));
	const WarpPoint drawn = d.warp.viewAt(places[0]);
	const double cell = 2.0 * d.warp.half / d.warp.n;
	// ROOM AT THE LIKE (each unit of the view covers less world there) and less of it far away.
	const double atLike = worldPerViewAround(d.warp, drawn, 4.0 * cell);
	const double far = worldPerViewAround(d.warp, { d.warp.centreX + d.warp.half - 2.5 * cell, d.warp.centreY + d.warp.half - 2.5 * cell }, 4.0 * cell);
	double jLow = 1e300;
	WarpPoint lowAt {};
	for (int j = 1; j < 4 * d.warp.n; ++j) {
		for (int i = 1; i < 4 * d.warp.n; ++i) {
			const WarpPoint p { d.warp.centreX - d.warp.half + i * cell / 4.0, d.warp.centreY - d.warp.half + j * cell / 4.0 };
			const double J = jacobianAt(d.warp, p.x, p.y, cell * 1e-3);
			if (J < jLow) {
				jLow = J;
				lowAt = p;
			}
		}
	}
	INFO("Jacobian at the like " << atLike << " (drawn at " << drawn.x << "," << drawn.y << "), far corner " << far << ", least " << jLow << " at " << lowAt.x << "," << lowAt.y << "; score at the hot spot " << [&] { std::vector<LikeVector> o; surface.fingerprintsAt(std::vector { d.warp.worldAt(lowAt) }, o); return d.model.score(o[0]); }() << "; worldAt(drawn) " << d.warp.worldAt(drawn).x << "," << d.warp.worldAt(drawn).y << "; worldAt(least) " << d.warp.worldAt(lowAt).x << "," << d.warp.worldAt(lowAt).y << " in a region centred " << d.warp.centreX << "," << d.warp.centreY << " half " << d.warp.half);
	CHECK(atLike < 0.9);
	CHECK(far > atLike);
}

TEST_CASE("MEASURE: the distribution each engine's terrain colours by (luminance channel, standardised tilt, glow)" * doctest::skip()) {
	// the question, 2026-10-26: is Additive's dimness a NORMALISATION issue (its field sits compressed low against what the colour mapping
	// expects) that the lifted glow merely masks? Same window and pooled zones for each engine; prints min / p5 / median / p95 / max and the
	// share of texels in the bottom 20% of [0,1]. Run with --no-skip.
	constexpr int kSide = TerrainMap::kResolution;
	const auto report = [](const std::string& what, std::vector<float> v, float scale) {
		std::sort(v.begin(), v.end());
		const auto at = [&](double q) { return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))] * scale; };
		size_t low = 0;
		for (const float x : v) {
			low += x * scale < 0.2f ? 1 : 0;
		}
		MESSAGE("  " << what << ": min " << at(0) << " p5 " << at(0.05) << " med " << at(0.5) << " p95 " << at(0.95) << " max "
		               << at(1) << " | bottom-20%: " << 100.0 * static_cast<double>(low) / static_cast<double>(v.size()) << "%");
	};
	for (const OscType engine : { OscType::V5, OscType::Morph, OscType::Additive, OscType::Resonate }) {
		std::vector<float> raw, lum, tilt, glow, grain, second;
		for (const double zoneBase : { 200.0, 512.0, 800.0 }) {
			TerrainWindow window = windowAt(0.3, 1.0, 1.0);
			window.engine = engine;
			window.zoneBaseline = zoneBase;
			for (int row = 0; row < kSide; row += 2) {
				const double travelY = TerrainMap::travelYAtRow(window, row);
				const double home = window.gammaBase;
				const auto frames = terrainFramesAt(window.pin, home, kNoPlaneResidue);
				const auto zone = static_cast<uint16_t>(std::clamp(std::lround(home * 1023.0), 0L, 1023L));
				const double base = home - static_cast<double>(zone) / 1023.0;
				for (int column = 0; column < kSide; column += 2) {
					const double travelX = TerrainMap::travelXAtColumn(window, column);
					const TerrainFeatures f = terrainFeaturesOn(engine, frames, zone, 0.0f, travelX, travelY, base);
					const TerrainChannels ch = terrainChannels(f, engine);
					raw.push_back(f.energy);
					lum.push_back(ch.luminance);
					glow.push_back(0.10f + 1.05f * ch.luminance);
					grain.push_back(ch.grain);
					second.push_back(residualParity(featureNormFor(engine), f.centroid, f.parity));
					tilt.push_back(standardize(featureNormFor(engine).centroid, f.centroid));
				}
			}
		}
		MESSAGE("engine " << static_cast<int>(engine) << " (" << kOscTypeNames[static_cast<size_t>(engine)] << "), " << raw.size() << " texels");
		report("raw energy (x1.1 = luminance gain)", raw, 1.0f);
		report("luminance channel clamp(energy*1.1)", lum, 1.0f);
		report("old glow 0.10+1.05*lum (of ~1.15)", glow, 1.0f);
		report("tilt standardised through its table", tilt, 1.0f);
		report("grain channel clamp(roughness*2.6)", grain, 1.0f);
		report("second axis (residual parity, standardised)", second, 1.0f);
	}
}

TEST_CASE("Additive's terrain colours through its own normalisation: luminance is read from the centroid, the other channels from their own features (backlog LY)") {
	// Additive's terrain read dim because its ENERGY is degenerate (a normalised partial sum, about 0.25 for 40% of the
	// world) while the shared knots expect up to 0.9. Its luminance is now read from the spectral CENTROID, through Additive's own baked knots
	// onto the shared energy knots (channelFeaturesFor); roughness (the grain) and the second axis keep their own features. By value:
	CHECK(&featureNormFor(OscType::Additive) == &kBakedAdditiveFeatureNorm);
	CHECK(&featureNormFor(OscType::V5) == &kBakedFeatureNorm);
	// THE LUMINANCE IS AN INDEPENDENT RESTATEMENT of that road, at the same energy for every centroid (so only the centroid can move it):
	TerrainFeatures at {};
	at.energy = 0.2475f;
	at.roughness = 0.1f;
	float previous = -1.0f;
	for (float u = 0.05f; u < 1.0f; u += 0.1f) {
		at.centroid = destandardize(kBakedAdditiveFeatureNorm.centroid, u);
		const float expected =
		    std::min(1.0f, destandardize(kBakedFeatureNorm.energy, standardize(kBakedAdditiveFeatureNorm.centroid, at.centroid)) * 1.1f);
		CAPTURE(u);
		CHECK(terrainChannels(at, OscType::Additive).luminance == doctest::Approx(expected).epsilon(1e-5));
		CHECK(terrainChannels(at, OscType::Additive).luminance >= previous); // a brighter-sounding place never glows less
		previous = terrainChannels(at, OscType::Additive).luminance;
	}
	// AND THE GRAIN IS STILL ROUGHNESS'S: the roughness channel matches through its own knots, not the centroid.
	TerrainFeatures rough = at;
	rough.roughness = destandardize(kBakedAdditiveFeatureNorm.roughness, 0.9f);
	TerrainFeatures smooth = at;
	smooth.roughness = destandardize(kBakedAdditiveFeatureNorm.roughness, 0.1f);
	CHECK(terrainChannels(rough, OscType::Additive).grain > terrainChannels(smooth, OscType::Additive).grain);
	// OVER ADDITIVE'S OWN FRAME the luminance now spans (it was pinned at p95 0.27): a wide spread, a bright end near V5's 0.66.
	std::vector<float> lum;
	std::vector<float> tilt;
	forEachAdditiveFrameSample([&](const TerrainFeatures& f) {
		lum.push_back(terrainChannels(f, OscType::Additive).luminance);
		tilt.push_back(standardize(featureNormFor(OscType::Additive).centroid, f.centroid));
	});
	REQUIRE(lum.size() > 100);
	const auto quantile = [](std::vector<float> v, double q) {
		std::sort(v.begin(), v.end());
		return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))];
	};
	CHECK(quantile(lum, 0.95) > 0.5);
	CHECK(quantile(lum, 0.05) < 0.2);
	CHECK(quantile(tilt, 0.05) < 0.15);
	CHECK(quantile(tilt, 0.95) > 0.85);
}

TEST_CASE("MEASURE ON warp: the linear magnification the warp achieves at the likes and the support's peaks, and the most squeezed place" * doctest::skip()) {
	// RUN BY HAND: --no-skip -tc="MEASURE ON warp*" (FEATHERS_WARP_STRENGTH to try others). V5 at defaults; one like, then
	// three far apart. Linear magnification = 1/sqrt(J), J the view-to-world Jacobian, at each lens centre; the most
	// squeezed is over a sweep of the cartogram's region and every lens disk.
	TerrainWindow base = windowAt(0.0, ExploreStage::kBaseTravel, ExploreStage::kBaseTravel);
	base.engine = OscType::V5;
	const auto likeAt = [&](double x, double y) {
		feathers::TerrainLike like;
		like.x = x;
		like.y = y;
		like.home = terrainPlaneHome(base);
		like.spectrum = terrainFineSpectrumAt(base, x, y);
		return like;
	};
	const double u = ExploreStage::kBaseTravel;
	for (const std::vector<feathers::TerrainLike>& likes :
	     { std::vector { likeAt(0.003, -0.002) }, std::vector { likeAt(-0.9 * u, 0.4 * u), likeAt(0.6 * u, 0.8 * u), likeAt(0.2 * u, -1.1 * u) } }) {
		TerrainWindow window = base;
		window.likes = std::make_shared<const std::vector<feathers::TerrainLike>>(likes);
		const std::shared_ptr<const TerrainLikeState> round = deriveTerrainLikes(window);
		const std::shared_ptr<const TerrainLikeState> d = std::getenv("FEATHERS_MEASURE_ROUND") != nullptr ? round : refineTerrainLikes(window, *round, [] { return false; });
		const TerrainWarp& warp = d->warp;
		const double h = 1e-7;
		std::string at;
		for (const WarpLens& lens : warp.lenses) {
			at += " " + std::to_string(1.0 / std::sqrt(jacobianAt(warp, lens.centre.x, lens.centre.y, h))).substr(0, 5);
		}
		double jMax = 0.0;
		const auto sweep = [&](WarpPoint c, double half, int n) {
			for (int j = 0; j < n; ++j) {
				for (int i = 0; i < n; ++i) {
					jMax = std::max(jMax, jacobianAt(warp, c.x - half + (i + 0.5) * 2.0 * half / n,
					                                 c.y - half + (j + 0.5) * 2.0 * half / n, h));
				}
			}
		};
		sweep({ warp.centreX, warp.centreY }, warp.half, 128);
		for (const WarpLens& lens : warp.lenses) {
			sweep(lens.centre, lens.radius, 64);
		}
		MESSAGE(likes.size() << " like(s): " << warp.lenses.size() << " lens(es), linear magnification at their centres" << at
		                     << " (strength " << kWarpStrength << "); most squeezed " << 1.0 / std::sqrt(jMax)
		                     << "x; support places " << d->supportPlaces << "; derivation " << d->millis << " ms, refine " << d->refineMillis << " ms (cartogram solve "
		                     << d->solveMillis << ")");
	}
}

//------------------------------//
// Likes: lenses and support    //
//------------------------------//

TEST_CASE("ON lens: a lens draws its centre its magnification larger, never below its rim slope either way, is the identity outside its disk, and inverts exactly") {
	const WarpLens lens = makeLens({ 0.01, -0.02 }, 0.05, 25.0);
	// g(1) = 1, so the rim lands on itself, and g'(0) is the magnification.
	CHECK(lens.profile(1.0) == doctest::Approx(1.0).epsilon(1e-12));
	CHECK(lens.slope(0.0) == doctest::Approx(25.0).epsilon(1e-12));
	double leastRadial = 1e300;
	double leastTangential = 1e300;
	for (int i = 1; i < 2000; ++i) {
		const double x = i / 2000.0;
		leastRadial = std::min(leastRadial, lens.slope(x));
		leastTangential = std::min(leastTangential, lens.profile(x) / x);
	}
	INFO("least radial " << leastRadial << ", least tangential " << leastTangential);
	CHECK(leastRadial >= kLensRim * (1.0 - 1e-12));
	CHECK(leastTangential >= kLensRim * (1.0 - 1e-12));
	// THE CENTRE, MEASURED THROUGH THE MAP: a small step from the centre lands 25 times farther in the view.
	const WarpPoint step = lens.forward({ lens.centre.x + 1e-9, lens.centre.y });
	CHECK((step.x - lens.centre.x) / 1e-9 == doctest::Approx(25.0).epsilon(1e-4));
	// OUTSIDE ITS DISK, the identity to the bit; inside, the inverse is exact.
	CHECK(lens.forward({ 0.07, -0.02 }) == WarpPoint { 0.07, -0.02 });
	double worst = 0.0;
	for (int j = 0; j < 41; ++j) {
		for (int i = 0; i < 41; ++i) {
			const WarpPoint p { lens.centre.x - 0.05 + i * 0.0025 + 1e-5, lens.centre.y - 0.05 + j * 0.0025 + 3e-6 };
			const WarpPoint back = lens.inverse(lens.forward(p));
			worst = std::max(worst, std::hypot(back.x - p.x, back.y - p.y));
		}
	}
	CHECK(worst < 1e-14);
}

TEST_CASE("ON warp: every like is drawn at the full strength, a like-alike place away from every like gets room too, nothing is drawn below the floor, and the whole warp round-trips") {
	TerrainWindow window = windowAt(0.0, ExploreStage::kBaseTravel, ExploreStage::kBaseTravel);
	window.engine = OscType::V5;
	const double u = ExploreStage::kBaseTravel;
	std::vector<feathers::TerrainLike> likes;
	for (const auto& [x, y] : { std::pair { -0.9 * u, 0.4 * u }, std::pair { 0.6 * u, 0.8 * u }, std::pair { 0.2 * u, -1.1 * u } }) {
		feathers::TerrainLike like;
		like.x = x;
		like.y = y;
		like.home = terrainPlaneHome(window);
		like.spectrum = terrainFineSpectrumAt(window, x, y);
		likes.push_back(like);
	}
	window.likes = std::make_shared<const std::vector<feathers::TerrainLike>>(likes);
	const std::shared_ptr<const TerrainLikeState> d = deriveTerrainLikes(window);
	const TerrainWarp& warp = d->warp;
	REQUIRE(warp.lenses.size() > likes.size());
	const double h = 1e-7;
	for (const feathers::TerrainLike& like : likes) {
		const WarpPoint drawn = warp.viewAt({ like.x, like.y });
		CHECK(1.0 / std::sqrt(jacobianAt(warp, drawn.x, drawn.y, h)) == doctest::Approx(kWarpStrength).epsilon(1e-3));
	}
	// THE SUPPORT: a lens whose centre is no like's, at a place that scores (it was found by the scan).
	size_t elsewhere = 0;
	for (const WarpLens& lens : warp.lenses) {
		bool atLike = false;
		for (const feathers::TerrainLike& like : likes) {
			const WarpPoint drawn = warp.viewAt({ like.x, like.y });
			atLike = atLike || std::hypot(drawn.x - lens.centre.x, drawn.y - lens.centre.y) < 1e-9;
		}
		if (!atLike) {
			++elsewhere;
			CHECK(lens.magnification > 1.0);
		}
	}
	CHECK(elsewhere >= 1);
	CHECK(d->supportPlaces >= 1);
	// THE FLOOR: the whole warp — cartogram and lenses — draws nothing below a quarter, linearly.
	double jMax = 0.0;
	for (int j = 0; j < 96; ++j) {
		for (int i = 0; i < 96; ++i) {
			const double x = warp.centreX - 2.0 * warp.half + (i + 0.5) * 4.0 * warp.half / 96;
			const double y = warp.centreY - 2.0 * warp.half + (j + 0.5) * 4.0 * warp.half / 96;
			jMax = std::max(jMax, jacobianAt(warp, x, y, h));
		}
	}
	for (const WarpLens& lens : warp.lenses) {
		for (int j = 0; j < 48; ++j) {
			for (int i = 0; i < 48; ++i) {
				jMax = std::max(jMax, jacobianAt(warp, lens.centre.x - lens.radius + (i + 0.5) * lens.radius / 24,
				                                 lens.centre.y - lens.radius + (j + 0.5) * lens.radius / 24, h));
			}
		}
	}
	INFO("most squeezed " << 1.0 / std::sqrt(jMax) << "x linear");
	CHECK(1.0 / std::sqrt(jMax) >= kWarpFloorLinear * kLensRim * (1.0 - 1e-6));
	// THE BIJECTION, lenses and all.
	double worst = 0.0;
	for (const feathers::TerrainLike& like : likes) {
		for (int k = 0; k < 64; ++k) {
			const WarpPoint p { like.x + 0.01 * u * std::cos(k * 0.7) * (k % 5), like.y + 0.01 * u * std::sin(k * 0.7) * (k % 7) };
			const WarpPoint back = warp.worldAt(warp.viewAt(p));
			worst = std::max(worst, std::hypot(back.x - p.x, back.y - p.y));
		}
	}
	CHECK(worst < 1e-12);
}

TEST_CASE("ON bake: under the warp each texel is meaned from the count its own world span asks for, not the zoom's" * doctest::skip(!kLikesMoveThePlane)) {
	ExploreFixture f;
	f.selection.select(1);
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	const TerrainWindow window = f.stage.terrainWindowNow();
	f.terrain.request(window);
	REQUIRE(settleRefinedLikes(f.terrain, window));
	std::shared_ptr<const TerrainSnapshot> map = f.terrain.snapshot();
	REQUIRE(map->likeState != nullptr);
	// WHILE THE SCHEDULE IS CAPPED AT ONE (by design, kSampleCap) there is nothing to follow and no count is kept.
	if (terrainSamplesFor(window.engine, 1e9) == 1) {
		CHECK(map->texelSamples.empty());
		return;
	}
	REQUIRE(map->texelSamples.size() == map->texels.size());
	const TerrainWarp& warp = map->likeState->warp;
	const double cell = window.travelXSpan / TerrainMap::kResolution;

	int mismatched = 0;
	for (int row = 0; row < TerrainMap::kResolution; ++row) {
		for (int column = 0; column < TerrainMap::kResolution; ++column) {
			const double x = TerrainMap::travelXAtColumn(window, column);
			const double y = TerrainMap::travelYAtRow(window, row);
			const double span = std::sqrt(worldPerViewAround(warp, { x, y }, cell)) * cell;
			const int expected = terrainSamplesFor(window.engine, span * 1023.0);
			mismatched += map->texelSamples[static_cast<size_t>(row) * TerrainMap::kResolution + column] != expected ? 1 : 0;

		}
	}
	// AND THE TEXEL DRAWN OVER THE LIKE, magnified, takes one.
	const feathers::TerrainLike& like = f.bridge.terrainLikes().likes[0];
	const WarpPoint drawn = warp.viewAt({ like.x, like.y });
	const long c = std::lround(TerrainMap::columnAtTravelX(window, drawn.x));
	const long r = std::lround(TerrainMap::rowAtTravelY(window, drawn.y));
	REQUIRE((c >= 0 && c < TerrainMap::kResolution && r >= 0 && r < TerrainMap::kResolution));
	CHECK(map->texelSamples[static_cast<size_t>(r) * TerrainMap::kResolution + static_cast<size_t>(c)] == 1);
	CHECK(mismatched == 0);
}

TEST_CASE("ON: the heart's context menu clears every like in one undo step, marks the patch modified, returns the warp to the identity, and is greyed with no likes") {
	ExploreFixture f;
	f.selection.select(1);
	// GREYED WITH NONE.
	{
		const visage::PopupMenu menu = f.stage.likesMenu();
		REQUIRE(menu.options().size() >= 1); // the clear first; the likes' strength choices follow
		CHECK(menu.options()[0].id() == ExploreStage::kClearLikes);
		CHECK_FALSE(menu.options()[0].enabled());
	}
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	walkOffOrigin(f, 1);
	f.stage.toggleLike();
	const feathers::TerrainLikes made = f.bridge.terrainLikes();
	REQUIRE(made.likes.size() == 2);
	const visage::PopupMenu menu = f.stage.likesMenu();
	CHECK(menu.options()[0].enabled());
	(void)f.bridge.takeStateDirty();
	const size_t entries = f.stage.historyLength();
	// A RIGHT PRESS ON THE HEART IS NOT A LIKE: it opens the menu and leaves the likes alone.
	const visage::Bounds heart = f.stage.partBounds(ExploreStage::Part::Like);
	visage::MouseEvent right = at(heart.x() + heart.width() * 0.5f, heart.y() + heart.height() * 0.5f);
	right.button_id = visage::kMouseButtonRight;
	right.button_state = visage::kMouseButtonRight;
	f.stage.onMouseDown().callback(right);
	CHECK(f.bridge.terrainLikes() == made);
	// THE PICK.
	menu.onSelection().callback(ExploreStage::kClearLikes);
	CHECK(f.bridge.terrainLikes().likes.empty());
	CHECK(f.bridge.takeStateDirty());
	CHECK(f.stage.historyLength() == entries + 1);
	// THE WARP IS THE IDENTITY AGAIN: the window carries no likes, so the map it builds carries no model or lenses.
	const TerrainWindow window = f.stage.terrainWindowNow();
	CHECK((window.likes == nullptr || window.likes->empty()));
	f.terrain.request(window);
	REQUIRE(settle(f.terrain, window));
	CHECK(f.terrain.snapshot()->likeState == nullptr);
	CHECK(f.stage.viewOf(0.0123, -0.0045) == WarpPoint { 0.0123, -0.0045 });
	// ONE UNDO PUTS THEM ALL BACK, value for value.
	f.stage.undo();
	CHECK(f.bridge.terrainLikes() == made);
}

TEST_CASE("ON score: loudness is not in the fingerprint — one envelope at two levels is one fingerprint and scores the same") {
	const LikeSpectrum quiet = envelopeOf(1.1, 0.3, 0.2);
	LikeSpectrum loud = quiet;
	for (float& m : loud) {
		m *= 7.0f;
	}
	const LikeVector a = likeFingerprint(quiet);
	const LikeVector b = likeFingerprint(loud);
	for (int k = 0; k < kLikeDims; ++k) {
		CHECK(a[k] == doctest::Approx(b[k]).epsilon(1e-6)); // the levels are floats
	}
	CHECK(kLikeDims == kLikeBands);
}

TEST_CASE("ON: a like belongs to its engine and sound — another engine's likes neither score, warp nor draw, switching back restores them, and Clear clears only the terrain on screen") {
	ExploreFixture f;
	visage::Canvas canvas;
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	REQUIRE(f.bridge.terrainLikes().likes.size() == 1);
	CHECK(f.bridge.terrainLikes().likes[0].engine == static_cast<uint16_t>(OscType::V5));
	CHECK(f.bridge.terrainLikes().likes[0].slot == 1);
	// ANOTHER ENGINE: its window carries none of V5's likes, none is drawn, and the heart is not lit by one.
	f.set(ParamId::OscType, static_cast<double>(OscType::Morph));
	{
		const TerrainWindow window = f.stage.terrainWindowNow();
		CHECK((window.likes == nullptr || window.likes->empty()));
		f.stage.draw(canvas);
		CHECK(f.stage.likeMarksDrawn() == 0);
		CHECK_FALSE(f.stage.likeUnderStar().has_value());
		CHECK_FALSE(f.stage.likesMenu().options()[0].enabled());
	}
	// A LIKE MADE HERE IS MORPH'S; V5'S IS KEPT.
	walkOffOrigin(f, 1);
	f.stage.toggleLike();
	REQUIRE(f.bridge.terrainLikes().likes.size() == 2);
	CHECK(f.bridge.terrainLikes().likes[1].engine == static_cast<uint16_t>(OscType::Morph));
	REQUIRE(f.stage.terrainWindowNow().likes->size() == 1);
	CHECK(f.stage.terrainWindowNow().likes->front() == f.bridge.terrainLikes().likes[1]);
	// ANOTHER SOUND ON THE SAME ENGINE sees none of them either.
	f.selection.select(0);
	CHECK((f.stage.terrainWindowNow().likes == nullptr || f.stage.terrainWindowNow().likes->empty()));
	f.selection.select(1);
	// BACK ON V5: only V5's like.
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	REQUIRE(f.stage.terrainWindowNow().likes->size() == 1);
	CHECK(f.stage.terrainWindowNow().likes->front() == f.bridge.terrainLikes().likes[0]);
	// CLEAR, ON V5, CLEARS V5'S AND SAYS SO; MORPH'S STAYS.
	const visage::PopupMenu menu = f.stage.likesMenu();
	CHECK(std::string(menu.options()[0].name().toUtf8()) == "Clear all likes on V5, sound B");
	menu.onSelection().callback(ExploreStage::kClearLikes);
	REQUIRE(f.bridge.terrainLikes().likes.size() == 1);
	CHECK(f.bridge.terrainLikes().likes[0].engine == static_cast<uint16_t>(OscType::Morph));
}

TEST_CASE("MEASURE ON fingerprint: leave-one-out over the stored likes — log-12 against Hellinger with fixed bands and with adaptive bands" * doctest::skip()) {
	// RUN BY HAND ONCE THERE ARE REAL LIKES: FEATHERS_LOO_PRESET=<a saved Feathers preset or state file>
	//   build/tests/test_gui_widgets_explore --no-skip -tc="MEASURE ON fingerprint*"
	// Without one it runs on five demo likes. FOR EACH LIKE, the model is built from the OTHERS and the held-out like is
	// scored against an 8x8 pre-pass of the likes' region: its PERCENTILE among those places (1 = it outscores every one)
	// says whether what the other likes share predicts it. Higher is better; the mean over the likes is the verdict.
	// LIMITS, said here: the map is read at V5's defaults (the knobs at the time of the like are not in a patch's likes), the
	// likes of the most common engine are used, and no control is nudged (the comparison is of fingerprints, not of the
	// projection).
	TerrainWindow window = windowAt(0.0, ExploreStage::kBaseTravel, ExploreStage::kBaseTravel);
	window.engine = OscType::V5;
	std::vector<feathers::TerrainLike> likes;
	if (const char* path = std::getenv("FEATHERS_LOO_PRESET")) {
		std::ifstream file(path, std::ios::binary);
		const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		const uint8_t tag[4] = { 'F', 'S', 'L', 'K' };
		const auto at = std::search(bytes.begin(), bytes.end(), tag, tag + 4);
		REQUIRE(at != bytes.end());
		struct Reader {
			const std::vector<uint8_t>* bytes;
			size_t at;
		} reader { &bytes, static_cast<size_t>(at - bytes.begin()) };
		clap_istream in {};
		in.ctx = &reader;
		in.read = [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
			auto* r = static_cast<Reader*>(s->ctx);
			const size_t n = std::min<size_t>(size, r->bytes->size() - r->at);
			std::memcpy(buffer, r->bytes->data() + r->at, n);
			r->at += n;
			return static_cast<int64_t>(n);
		};
		feathers::TerrainLikes read;
		bool present = false;
		REQUIRE(feathers::loadTerrainLikes(&in, &read, &present));
		std::array<int, 8> counts {};
		for (const auto& like : read.likes) {
			++counts[std::min<size_t>(like.engine, 7)];
		}
		const auto engine = static_cast<uint16_t>(std::max_element(counts.begin(), counts.end()) - counts.begin());
		for (const auto& like : read.likes) {
			if (like.engine == engine) {
				likes.push_back(like);
			}
		}
		window.engine = static_cast<OscType>(engine);
	}
	else {
		const double u = ExploreStage::kBaseTravel;
		for (const auto& [x, y] : { std::pair { -0.7 * u, 0.3 * u }, std::pair { 0.5 * u, 0.6 * u }, std::pair { 0.1 * u, -0.9 * u },
		                            std::pair { 0.8 * u, -0.2 * u }, std::pair { -0.3 * u, -0.4 * u } }) {
			feathers::TerrainLike like;
			like.x = x;
			like.y = y;
			like.spectrum = terrainFineSpectrumAt(window, x, y);
			likes.push_back(like);
		}
	}
	REQUIRE(likes.size() >= 3);
	window.likes = std::make_shared<const std::vector<feathers::TerrainLike>>(likes);
	std::vector<WarpPoint> places;
	for (const auto& like : likes) {
		places.push_back({ like.x, like.y });
	}
	const LikeRegion region = likeRegionFor(places);
	std::vector<WarpPoint> grid;
	for (int j = 0; j < kLikePrepass; ++j) {
		for (int i = 0; i < kLikePrepass; ++i) {
			grid.push_back({ region.centreX - region.half + (i + 0.5) * 2.0 * region.half / kLikePrepass,
			                 region.centreY - region.half + (j + 0.5) * 2.0 * region.half / kLikePrepass });
		}
	}
	std::vector<LikeSpectrum> gridSpectra;
	for (const WarpPoint& p : grid) {
		gridSpectra.push_back(terrainFineSpectrumAt(window, p.x, p.y));
	}
	const auto log12 = [&](double x, double y) {
		const auto p = terrainPartialsAt(window, x, y);
		return logShapeFingerprint(p);
	};
	enum Contender { Log12, Fixed, Adaptive };
	for (const Contender c : { Log12, Fixed, Adaptive }) {
		double percentile = 0.0;
		double held = 0.0;
		for (size_t out = 0; out < likes.size(); ++out) {
			std::vector<LikeSpectrum> others;
			for (size_t i = 0; i < likes.size(); ++i) {
				if (i != out) {
					others.push_back(likes[i].spectrum);
				}
			}
			const LikeBandEdges edges = c == Adaptive ? adaptiveLikeBands(others) : fixedLikeBands();
			const auto print = [&](size_t i, const LikeSpectrum& s, double x, double y) {
				(void)i;
				return c == Log12 ? log12(x, y) : likeFingerprint(s, edges);
			};
			LikeModelInputs inputs;
			for (size_t g = 0; g < grid.size(); ++g) {
				inputs.map.push_back(print(g, gridSpectra[g], grid[g].x, grid[g].y));
			}
			for (size_t i = 0; i < likes.size(); ++i) {
				if (i != out) {
					inputs.likes.push_back(print(i, likes[i].spectrum, likes[i].x, likes[i].y));
				}
			}
			const LikeModel model = buildLikeModel(inputs);
			const double s = model.score(print(out, likes[out].spectrum, likes[out].x, likes[out].y));
			int below = 0;
			for (const LikeVector& f : inputs.map) {
				below += model.score(f) < s ? 1 : 0;
			}
			percentile += static_cast<double>(below) / inputs.map.size() / likes.size();
			held += s / likes.size();
		}
		const std::string name = c == Log12 ? "log-12" : c == Fixed ? "Hellinger, fixed bands" : "Hellinger, adaptive bands";
		MESSAGE(name
		        << ": held-out likes score " << held << " on average, outscoring " << 100.0 * percentile
		        << "% of the region's places (" << likes.size() << " likes)");
	}
}

TEST_CASE("ON fingerprint: Hellinger over bands — a point on the unit sphere, banded third-octave by default, and adaptive edges are strictly increasing, cover the grid, and follow where the likes' power is") {
	const LikeSpectrum s = envelopeOf(1.0, 0.3, 0.5);
	const LikeVector f = likeFingerprint(s);
	double norm = 0.0;
	for (const double v : f) {
		norm += v * v;
	}
	CHECK(norm == doctest::Approx(1.0).epsilon(1e-12));
	// BAND b IS FINE BINS 2b AND 2b+1: the square root of their share of the whole.
	double total = 0.0;
	for (const float p : s) {
		total += p;
	}
	CHECK(f[3] == doctest::Approx(std::sqrt((s[6] + s[7]) / total)).epsilon(1e-6));
	const LikeBandEdges fixed = fixedLikeBands();
	CHECK(fixed[0] == 0);
	CHECK(fixed[kLikeBands] == kLikeFineBins);
	// ADAPTIVE: likes with all their power low pull the edges together there.
	std::vector<LikeSpectrum> low;
	for (const double r : { 2.5, 3.0, 3.5 }) {
		low.push_back(envelopeOf(r, 0.0, 1.0));
	}
	const LikeBandEdges adaptive = adaptiveLikeBands(low);
	CHECK(adaptive[0] == 0);
	CHECK(adaptive[kLikeBands] == kLikeFineBins);
	for (int b = 0; b < kLikeBands; ++b) {
		CHECK(adaptive[static_cast<size_t>(b + 1)] > adaptive[static_cast<size_t>(b)]);
	}
	CHECK(adaptive[kLikeBands / 2] < fixed[kLikeBands / 2]);
}

TEST_CASE("MEASURE ON: what one fine fingerprint costs per texel, per engine" * doctest::skip()) {
	for (const OscType engine : { OscType::V5, OscType::Morph, OscType::Stair, OscType::Additive }) {
		TerrainWindow window = windowAt(0.0, ExploreStage::kBaseTravel, ExploreStage::kBaseTravel);
		window.engine = engine;
		const auto started = std::chrono::steady_clock::now();
		double sink = 0.0;
		for (int i = 0; i < 2048; ++i) {
			sink += terrainFineSpectrumAt(window, 1e-5 * i, -2e-5 * i)[0];
		}
		const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count() / 2048;
		MESSAGE("engine " << static_cast<int>(engine) << ": " << us << " us a fine fingerprint (" << kLikeFineCycle
		                  << " samples); a 128x128 view " << us * 16384 / 1e6 << " s, a 64x64 patch " << us * 4096 / 1e3
		                  << " ms (" << sink << ")");
	}
}

//------------------------------//
// Likes: shaped lenses         //
//------------------------------//

namespace {

/// Wait until `map` holds the whole of `window` built through REFINED likes (the worker's idle refine), up to a minute.
bool settleRefinedLikes(TerrainMap& map, const TerrainWindow& window) {
	for (int i = 0; i < 6000; ++i) {
		const std::shared_ptr<const TerrainSnapshot> now = map.snapshot();
		if (now->window == window && now->rows >= TerrainMap::kResolution && (kLikesMoveThePlane ? now->likeState != nullptr && now->likeState->refined : true)) {
			return true;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	return false;
}

TerrainWindow threeLikedWindow() {
	TerrainWindow window = windowAt(0.0, ExploreStage::kBaseTravel, ExploreStage::kBaseTravel);
	window.engine = OscType::V5;
	const double u = ExploreStage::kBaseTravel;
	std::vector<feathers::TerrainLike> likes;
	for (const auto& [x, y] : { std::pair { -0.9 * u, 0.4 * u }, std::pair { 0.6 * u, 0.8 * u }, std::pair { 0.2 * u, -1.1 * u } }) {
		feathers::TerrainLike like;
		like.x = x;
		like.y = y;
		like.home = terrainPlaneHome(window);
		like.engine = static_cast<uint16_t>(OscType::V5);
		like.spectrum = terrainFineSpectrumAt(window, x, y);
		likes.push_back(like);
	}
	window.likes = std::make_shared<const std::vector<feathers::TerrainLike>>(likes);
	return window;
}

} // namespace

TEST_CASE("ON lens: a score-shaped lens is a star-shaped bijection — each ray monotone, never below the rim slope, the identity outside — whose extent reaches past where the score stays high along each ray") {
	const TerrainWindow window = threeLikedWindow();
	const std::shared_ptr<const TerrainLikeState> base = deriveTerrainLikes(window);
	REQUIRE(base != nullptr);
	CHECK_FALSE(base->refined);
	const std::shared_ptr<const TerrainLikeState> d = refineTerrainLikes(window, *base, [] { return false; });
	REQUIRE(d != nullptr);
	CHECK(d->refined);
	REQUIRE_FALSE(d->warp.lenses.empty());
	const std::vector<LikeVector> unused;
	bool followed = false;
	for (const WarpLens& lens : d->warp.lenses) {
		REQUIRE(lens.starShaped());
		REQUIRE(lens.extent.size() == static_cast<size_t>(kLensRays));
		for (int ray = 0; ray < kLensRays; ++ray) {
			CHECK(lens.extent[static_cast<size_t>(ray)] <= lens.radius * (1.0 + 1e-12));
			const double* g = &lens.shape[static_cast<size_t>(ray * (kLensProfilePoints + 1))];
			CHECK(g[0] == 0.0);
			CHECK(g[kLensProfilePoints] == 1.0);
			for (int k = 0; k < kLensProfilePoints; ++k) {
				CHECK(g[k + 1] - g[k] >= kLensRim / kLensProfilePoints * (1.0 - 1e-9));
			}
		}
		// THE IDENTITY OUTSIDE, AND AN EXACT INVERSE INSIDE.
		CHECK(lens.forward({ lens.centre.x + 1.01 * lens.radius, lens.centre.y }) == WarpPoint { lens.centre.x + 1.01 * lens.radius, lens.centre.y });
		double worst = 0.0;
		for (int k = 0; k < 200; ++k) {
			const double r = lens.radius * (k % 20 + 0.5) / 21.0;
			const double a = 0.37 * k;
			const WarpPoint p { lens.centre.x + r * std::cos(a), lens.centre.y + r * std::sin(a) };
			const WarpPoint back = lens.inverse(lens.forward(p));
			worst = std::max(worst, std::hypot(back.x - p.x, back.y - p.y));
		}
		CHECK(worst < 1e-12 * std::max(1.0, lens.radius * 1e3));
		const auto [low, high] = std::minmax_element(lens.extent.begin(), lens.extent.end());
		followed = followed || *high > *low * 1.05;
	}
	CHECK(followed);
	// AND THE MARGIN BUYS THE STRENGTH: the strongest like's lens draws its centre at least ten times larger (the room given
	// back by the dissimilar ground past the core; without it a core can only be drawn at about its own size).
	double strongest = 0.0;
	for (const WarpLens& lens : d->warp.lenses) {
		const WarpPoint step = lens.forward({ lens.centre.x + 1e-9, lens.centre.y });
		strongest = std::max(strongest, (step.x - lens.centre.x) / 1e-9);
	}
	INFO("strongest centre " << strongest << "x");
	CHECK(strongest >= 10.0);
	// THE WHOLE WARP STILL ROUND-TRIPS, lenses and all.
	double worst = 0.0;
	for (const auto& like : *window.likes) {
		for (int k = 0; k < 64; ++k) {
			const WarpPoint p { like.x + 0.002 * std::cos(k * 0.7) * (k % 5), like.y + 0.002 * std::sin(k * 0.7) * (k % 7) };
			const WarpPoint back = d->warp.worldAt(d->warp.viewAt(p));
			worst = std::max(worst, std::hypot(back.x - p.x, back.y - p.y));
		}
	}
	CHECK(worst < 1e-11);
}

TEST_CASE("ON refine: distant centres are searched coarse to fine — each refined centre scores at least its candidate — and duplicates merge") {
	const TerrainWindow window = threeLikedWindow();
	const std::shared_ptr<const TerrainLikeState> base = deriveTerrainLikes(window);
	const std::shared_ptr<const TerrainLikeState> d = refineTerrainLikes(window, *base, [] { return false; });
	REQUIRE(d != nullptr);
	const double span = 2.0 * kSupportReach / kSupportScan;
	int distant = 0;
	for (const LikePeak& p : d->lensPeaks) {
		if (p.score >= 1.0) {
			continue; // a like
		}
		++distant;
		CHECK(p.score >= kSupportAt);
		// IT SCORES AT LEAST THE CANDIDATE IT GREW FROM (the nearest within a scan cell).
		double near = 0.0;
		for (const LikePeak& c : base->candidates) {
			if (std::hypot(c.at.x - p.at.x, c.at.y - p.at.y) <= span * std::sqrt(2.0)) {
				near = std::max(near, c.score);
			}
		}
		CHECK(p.score >= near - 1e-12);
	}
	CHECK(distant >= 1);
	for (size_t i = 0; i < d->lensPeaks.size(); ++i) {
		for (size_t j = i + 1; j < d->lensPeaks.size(); ++j) {
			CHECK(std::hypot(d->lensPeaks[i].at.x - d->lensPeaks[j].at.x, d->lensPeaks[i].at.y - d->lensPeaks[j].at.y) > 0.0);
		}
	}
	// AND IT GIVES UP WHEN ASKED, leaving nothing.
	CHECK(refineTerrainLikes(window, *base, [] { return true; }) == nullptr);
}

TEST_CASE("ON refine: the first map after the likes change is drawn with round lenses at once, the refined one follows when nothing waits, and a pan reuses it" * doctest::skip(!kLikesMoveThePlane)) {
	TerrainMap map;
	std::mutex lock;
	std::vector<bool> refinedWhenWhole;
	map.observePublishesForTest([&](const std::shared_ptr<const TerrainSnapshot>& s) {
		if (s->rows >= TerrainMap::kResolution && s->likeState != nullptr) {
			std::lock_guard<std::mutex> g(lock);
			refinedWhenWhole.push_back(s->likeState->refined);
		}
	});
	const TerrainWindow window = threeLikedWindow();
	map.request(window);
	REQUIRE(settleRefinedLikes(map, window));
	{
		std::lock_guard<std::mutex> g(lock);
		REQUIRE(refinedWhenWhole.size() >= 2);
		CHECK_FALSE(refinedWhenWhole.front());
		CHECK(refinedWhenWhole.back());
	}
	const std::shared_ptr<const TerrainLikeState> refined = map.snapshot()->likeState;
	// A PAN: the same likes and world, so the same refined derivation, not a new round one.
	TerrainWindow panned = window;
	panned.travelXCentre += window.travelXSpan / 8.0;
	map.request(panned);
	REQUIRE(settle(map, panned));
	CHECK(map.snapshot()->likeState == refined);
}

TEST_CASE("MEASURE ON: a pan's cost with no likes, round lenses and shaped lenses" * doctest::skip()) {
	// RUN BY HAND, and once more with FEATHERS_LIKES_NO_REFINE=1 for the round lenses. A pan reuses the derivation, so what
	// is left is the bake through the warp.
	for (const bool liked : { false, true }) {
		TerrainWindow window = threeLikedWindow();
		if (!liked) {
			window.likes = nullptr;
		}
		TerrainMap map;
		map.request(window);
		REQUIRE((liked && std::getenv("FEATHERS_LIKES_NO_REFINE") == nullptr ? settleRefinedLikes(map, window) : settle(map, window)));
		std::vector<double> ms;
		for (int pan = 1; pan <= 6; ++pan) {
			TerrainWindow panned = window;
			panned.travelXCentre += pan * window.travelXSpan; // a whole view each time: nothing carried exactly
			const auto started = std::chrono::steady_clock::now();
			map.request(panned);
			REQUIRE(settle(map, panned));
			ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
		}
		std::sort(ms.begin(), ms.end());
		const std::string kind = liked ? (std::getenv("FEATHERS_LIKES_NO_REFINE") != nullptr ? "round lenses" : "shaped lenses") : "no likes";
		MESSAGE(kind
		        << ": a fresh-ground pan " << ms[ms.size() / 2] << " ms (median of 6)");
	}
}

//------------------------------//
// Stored places follow the plane //
//------------------------------//

namespace {

/// THE PROJECTION, RESTATED BY ITS PROPERTY: `place` is the nearest point of `window`'s plane to `point` iff what is left
/// over is orthogonal to both travel directions. Directions taken by differencing the plane's own decode, so nothing of
/// terrainPlaceOn's arithmetic is reused. Returns the worse of the two normalised dot products.
double projectionMiss(const TerrainWindow& window, std::span<const double> point, double x, double y) {
	std::array<double, kTerrainLikePoint> at {};
	std::array<double, kTerrainLikePoint> ax {};
	std::array<double, kTerrainLikePoint> ay {};
	const size_t n = terrainPlacePoint(window, x, y, at);
	(void)terrainPlacePoint(window, x + 1.0, y, ax);
	(void)terrainPlacePoint(window, x, y + 1.0, ay);
	double mx = 0.0;
	double my = 0.0;
	double mm = 0.0;
	double dx = 0.0;
	double dy = 0.0;
	for (size_t i = 0; i < n; ++i) {
		const double miss = point[i] - at[i];
		mx += miss * (ax[i] - at[i]);
		my += miss * (ay[i] - at[i]);
		mm += miss * miss;
		dx += (ax[i] - at[i]) * (ax[i] - at[i]);
		dy += (ay[i] - at[i]) * (ay[i] - at[i]);
	}
	const double scale = std::sqrt(mm) + 1e-30;
	return std::max(std::abs(mx) / (scale * std::sqrt(dx) + 1e-30), std::abs(my) / (scale * std::sqrt(dy) + 1e-30));
}

constexpr double kPlacesPi = 3.14159265358979323846;

void turnQuarter(ExploreFixture& f, double sign) {
	turnRotateCorner(f, static_cast<float>(sign * 0.25 * kPlacesPi / ExploreStage::kRotateSpan * Knob::kDragRange));
	f.settleGesture();
	f.stage.update();
}

} // namespace

TEST_CASE("PLACES: a like follows a rotate — what it stores is unchanged, its mark and the warp's place are its point's projection onto the turned plane, undo returns it to the bit, and a counter-turn to 1e-12") {
	ExploreFixture f;
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	walkOffOrigin(f, 1); // stand elsewhere, so the turn is not about the like
	REQUIRE(f.bridge.terrainLikes().likes.size() == 1);
	const TerrainLike like = f.bridge.terrainLikes().likes[0];
	REQUIRE(like.pointCount == 2 * (kMorphTriangles + kStairTriangles + kVoxTriangles));
	const PlanePlace made = f.stage.likePlace(0);
	CHECK(made.exact);
	CHECK(made.x == like.x);
	CHECK(made.y == like.y);

	turnQuarter(f, 1.0);
	const TerrainWindow turned = f.stage.terrainWindowNow();
	// WHAT IS STORED IS NOT REWRITTEN BY A TURN: the point, the spectrum, the place it was made at.
	CHECK(f.bridge.terrainLikes().likes[0] == like);
	const PlanePlace there = f.stage.likePlace(0);
	CHECK_FALSE(there.exact);
	INFO("moved to " << there.x << "," << there.y << " from " << like.x << "," << like.y << ", off the plane " << there.offPlane);
	CHECK(std::hypot(there.x - like.x, there.y - like.y) > 1e-6); // not vacuous: the turn moved where it lands
	CHECK(projectionMiss(turned, std::span(like.point).first(like.pointCount), there.x, there.y) < 1e-9);
	// THE WARP'S PLACE IS THE SAME PROJECTION.
	const auto derived = deriveTerrainLikes(turned);
	REQUIRE(derived != nullptr);
	REQUIRE(derived->likePlaces.size() == 1);
	CHECK(derived->likePlaces[0].x == there.x);
	CHECK(derived->likePlaces[0].y == there.y);

	// UNDO RESTORES THE PLANE, AND THE PLACE TO THE BIT.
	f.stage.undo();
	f.settleGesture();
	const PlanePlace back = f.stage.likePlace(0);
	CHECK(back.exact);
	CHECK(back.x == like.x);
	CHECK(back.y == like.y);
	// A COUNTER-TURN re-folds the residue (one rounding a turn), so it lands to 1e-12, not to the bit.
	turnQuarter(f, 1.0);
	turnQuarter(f, -1.0);
	const PlanePlace counter = f.stage.likePlace(0);
	CHECK(std::abs(counter.x - like.x) < 1e-12);
	CHECK(std::abs(counter.y - like.y) < 1e-12);
}

TEST_CASE("PLACES: a history crumb follows a rotate — drawn at its own point's projection onto the turned plane, back to the bit on undo") {
	ExploreFixture f;
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	walkOffOrigin(f, 2);
	const visage::Point centre = f.stage.disk().centre();
	f.stage.onMouseDown().callback(at(centre.x + 40.0f, centre.y - 50.0f));
	f.stage.onMouseUp().callback(at(centre.x + 40.0f, centre.y - 50.0f));
	f.settleGesture();
	REQUIRE(f.stage.crumbs(1).size() >= 1);
	const SoundIdentity crumb = f.stage.crumbs(1).back();
	const size_t last = f.stage.crumbs(1).size() - 1;
	const PlanePlace made = f.stage.crumbPlace(last);
	CHECK(made.exact);
	CHECK(made.x == crumb.driftX);
	CHECK(made.y == crumb.driftY);

	turnQuarter(f, 1.0);
	REQUIRE(f.stage.crumbs(1)[last] == crumb);
	const PlanePlace there = f.stage.crumbPlace(last);
	CHECK_FALSE(there.exact);
	CHECK(std::hypot(there.x - crumb.driftX, there.y - crumb.driftY) > 1e-6);
	// ITS POINT, decoded on the crumb's own plane, projected by the property.
	TerrainWindow own = f.stage.terrainWindowNow();
	own.pin = crumb.plane.pinned ? crumb.plane : f.bridge.orientation();
	own.residue = crumb.residue;
	own.gammaBase = crumb.gammaBase;
	own.zoneBaseline = crumb.zone;
	own.offsetBaseline = crumb.offset;
	std::array<double, kTerrainLikePoint> point {};
	const size_t n = terrainPlacePoint(own, crumb.driftX, crumb.driftY, point);
	REQUIRE(n > 0);
	CHECK(projectionMiss(f.stage.terrainWindowNow(), std::span(point).first(n), there.x, there.y) < 1e-9);
	// AND THE MARK IS DRAWN THERE.
	const visage::Point drawn = f.stage.crumbAt(last);
	const visage::Point want = f.stage.sheetAtWorld(there.x, there.y);
	CHECK(drawn.x == want.x);
	CHECK(drawn.y == want.y);

	f.stage.undo();
	f.settleGesture();
	const PlanePlace back = f.stage.crumbPlace(last);
	CHECK(back.exact);
	CHECK(back.x == crumb.driftX);
	CHECK(back.y == crumb.driftY);
}


TEST_CASE("PLACES: a like made at another home draws at its point's projection, panned off by the gamma move, by value — no fade; off the chart it is not drawn, and the numbers stay finite") {
	ExploreFixture f;
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	const TerrainLike like = f.bridge.terrainLikes().likes[0];
	visage::Canvas canvas;
	f.stage.draw(canvas);
	REQUIRE(f.stage.likeMarksDrawn() == 1);
	f.set(ParamId::BZone, f.bridge.value(static_cast<clap_id>(ParamId::BZone)) + 3.0); // another home, three zones up
	f.stage.update();
	const TerrainWindow now = f.stage.terrainWindowNow();
	const PlanePlace there = f.stage.likePlace(0);
	CHECK_FALSE(there.exact);
	CHECK(std::isfinite(there.x));
	CHECK(std::isfinite(there.y));
	// BY VALUE: the nearest place of the new plane, by the projection's property.
	CHECK(projectionMiss(now, std::span(like.point).first(like.pointCount), there.x, there.y) < 1e-9);
	// AND IT MOVED, which is the gamma move's in-plane share (not vacuous).
	CHECK(std::hypot(there.x - like.x, there.y - like.y) > 1e-6);
	// DRAWN IFF ON THE CHART: no fade, only the clip.
	const visage::Point flat = f.stage.sheetAtWorld(there.x, there.y);
	REQUIRE(std::isfinite(flat.x));
	REQUIRE(std::isfinite(flat.y));
	f.stage.draw(canvas);
	CHECK(f.stage.likeMarksDrawn() == (f.stage.disk().contains(flat) ? 1 : 0));
	// FAR OFF: a big gamma move pans it off the chart — finite, and not drawn.
	f.set(ParamId::BZone, 900.0);
	f.stage.update();
	const PlanePlace far = f.stage.likePlace(0);
	const visage::Point farFlat = f.stage.sheetAtWorld(far.x, far.y);
	CHECK(std::isfinite(farFlat.x));
	CHECK(std::isfinite(farFlat.y));
	f.stage.draw(canvas);
	CHECK(f.stage.likeMarksDrawn() == (f.stage.disk().contains(farFlat) ? 1 : 0));
}

//------------------------------//
// Likes: P-MÖBIUS, the warp    //
//------------------------------//

namespace {

/// The voice's φ of the selected sound now: the character the scope builds (the synth's road, every sound's warp attached
/// as the synth attaches it), through the bank's own flagship build.
feathers::FlagshipRecipe voiceRecipeNow(ExploreFixture& f) {
	WaveDisplay wave { f.bridge, f.registry };
	return feathers::buildFlagshipFor(wave.characterNowForTest());
}

/// Every field a recipe carries, for an exact comparison.
std::vector<float> recipeFields(const feathers::FlagshipRecipe& r) {
	std::vector<float> out;
	for (int i = 0; i < owlet::dsp::kPhiMorphMovableWaypoints; ++i) {
		out.push_back(r.morph.phases[i]);
		out.push_back(r.morph.amplitudes[i]);
	}
	for (int i = 0; i < owlet::dsp::kPhiMorphMaxSegments; ++i) {
		out.push_back(r.morph.curvatures[i]);
	}
	for (const float v : { r.morph.gain, r.morph.endpointAmp, r.morph.morphAmpOvershoot, r.morph.morphCurvBoost, r.morph.morphPhaseDistort,
	                       r.morph.phaseJitter, r.morph.ampNoise, r.quantize, r.syncRatio, r.feedback, r.stepCount, r.riserSlope,
	                       r.smoothness, r.pointCount, r.fmRatio, r.fmDenominator, r.fmStereoWidth }) {
		out.push_back(v);
	}
	return out;
}

} // namespace

TEST_CASE("ON P-MÖBIUS: every sound's warp is derived from its likes and published; the heart's menu offers strength ×1/×2/×4, the chosen one ticked, and a pick re-derives and publishes it") {
	ExploreFixture f;
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	walkOffOrigin(f, 1);
	f.stage.toggleLike();
	f.stage.refreshLikeWarps();
	const auto ticked = [&f] {
		std::vector<int> out;
		const visage::PopupMenu menu = f.stage.likesMenu();
		for (const auto& option : menu.options()) {
			if (option.id() > ExploreStage::kLikeStrength && option.selected()) {
				out.push_back(option.id() - ExploreStage::kLikeStrength);
			}
		}
		return out;
	};
	CHECK(ticked() == std::vector<int> { 1 });
	const TerrainWindow one = f.stage.terrainWindowNow();
	CHECK(one.likeStrength == 1.0);
	REQUIRE(one.likeWarp.warp != nullptr);
	CHECK(one.likeWarp.warp->likes == 2);
	// BY VALUE: the warp the stage published is the fit of this terrain's likes at this strength, restated independently.
	CHECK(deriveLikeWarp(one) == one.likeWarp);
	CHECK(f.bridge.likeWarpOf(1) == one.likeWarp);
	CHECK(f.bridge.likeEngineOf(1) == static_cast<uint8_t>(OscType::V5));
	// PICK ×4: re-derived at that strength, and published.
	f.stage.likesMenu().onSelection().callback(ExploreStage::kLikeStrength + 4);
	f.stage.refreshLikeWarps();
	CHECK(ticked() == std::vector<int> { 4 });
	const TerrainWindow four = f.stage.terrainWindowNow();
	CHECK(four.likeStrength == 4.0);
	REQUIRE(four.likeWarp.warp != nullptr);
	CHECK(four.likeWarp != one.likeWarp);
	CHECK(deriveLikeWarp(four) == four.likeWarp);
	CHECK(f.bridge.likeWarpOf(1) == four.likeWarp);
	// AND BACK: the same warp, by content.
	f.stage.likesMenu().onSelection().callback(ExploreStage::kLikeStrength + 1);
	f.stage.refreshLikeWarps();
	CHECK(f.stage.terrainWindowNow().likeWarp == one.likeWarp);
	CHECK(f.bridge.likeWarpOf(1) == one.likeWarp);
}

TEST_CASE("ON P-MÖBIUS: no likes is plain — no warp derived or published on V5 or Additive, the bake unmoved, to the bit") {
	ExploreFixture f;
	f.selection.select(1);
	for (const OscType engine : { OscType::V5, OscType::Additive, OscType::Stair, OscType::Resonate, OscType::BasicShapes }) {
		CAPTURE(static_cast<int>(engine));
		f.set(ParamId::OscType, static_cast<double>(engine));
		walkOffOrigin(f, 2);
		f.stage.refreshLikeWarps();
		const TerrainWindow window = f.stage.terrainWindowNow();
		CHECK(window.likeWarp.identity());
		CHECK(f.bridge.likeWarpOf(1).identity());
		TerrainWindow bare = window;
		bare.likeWarp = {};
		CHECK(terrainFineSpectrumAt(window, f.stage.selectedDrift(), f.stage.selectedDriftY())
		      == terrainFineSpectrumAt(bare, f.stage.selectedDrift(), f.stage.selectedDriftY()));
	}
}

TEST_CASE("ON P-MÖBIUS: the stance just liked keeps its sound to the bit across the like — V5's voice and bake, the bake on Additive, Stair, Resonate and Basic Shapes — while the field moves elsewhere") {
	for (const OscType engine : { OscType::V5, OscType::Additive, OscType::Stair, OscType::Resonate, OscType::BasicShapes }) {
		CAPTURE(static_cast<int>(engine));
		ExploreFixture f;
		f.selection.select(1);
		f.set(ParamId::OscType, static_cast<double>(engine));
		walkOffOrigin(f, 2);
		f.stage.toggleLike();
		walkOffOrigin(f, 1);
		const double x = f.stage.selectedDrift();
		const double y = f.stage.selectedDriftY();
		f.stage.refreshLikeWarps();
		const TerrainWindow before = f.stage.terrainWindowNow();
		REQUIRE(before.likeWarp.warp != nullptr);
		const LikeSpectrum drawnBefore = terrainFineSpectrumAt(before, x, y);
		const std::vector<float> heardBefore = engine == OscType::V5 || engine == OscType::BasicShapes ? recipeFields(voiceRecipeNow(f)) : std::vector<float> {};
		const double fx = x + 1.5 * ExploreStage::kBaseTravel;
		const LikeSpectrum farBefore = terrainFineSpectrumAt(before, fx, y);
		f.stage.toggleLike();
		REQUIRE(f.bridge.terrainLikes().likes.size() == 2);
		f.stage.refreshLikeWarps();
		const TerrainWindow after = f.stage.terrainWindowNow();
		REQUIRE(after.likeWarp.warp != nullptr);
		REQUIRE(after.likeWarp != before.likeWarp);
		CHECK(terrainFineSpectrumAt(after, x, y) == drawnBefore);
		if (engine == OscType::V5 || engine == OscType::BasicShapes) {
			CHECK(recipeFields(voiceRecipeNow(f)) == heardBefore);
		}
		// THE FIELD MOVES ELSEWHERE — where this engine's sound depends on the place at all. Basic Shapes at its default knobs
		// does not (it plays the shape ladders; the recipe its 45 sites move reaches the sound only through FM, Grit and Warp).
		if (drawnBefore == farBefore) {
			CHECK(engine == OscType::BasicShapes);
		}
		else {
			CHECK_FALSE(terrainFineSpectrumAt(after, fx, y) == farBefore);
		}
	}
}

//------------------------------//
// Likes: the like-set log      //
//------------------------------//

namespace {

/// A fresh, empty directory for one case's log, removed when the case ends; the log variable points at it meanwhile.
struct LikesLogDir {
	std::filesystem::path path;
	std::unique_ptr<ScopedEnv> env;
	LikesLogDir() {
		static int made = 0;
		path = std::filesystem::temp_directory_path()
		       / ("feathers-likes-log-" + std::to_string(static_cast<long>(::getpid())) + "-" + std::to_string(++made));
		std::filesystem::remove_all(path);
		env = std::make_unique<ScopedEnv>(likeslog::kEnvVar, path.c_str());
	}
	~LikesLogDir() {
		env.reset();
		std::error_code ec;
		std::filesystem::remove_all(path, ec);
	}
	/// Every record of every day's file, in order.
	[[nodiscard]] std::vector<likeslog::Record> records() const {
		std::vector<std::filesystem::path> files;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
			files.push_back(entry.path());
		}
		std::sort(files.begin(), files.end());
		std::vector<likeslog::Record> out;
		for (const auto& file : files) {
			size_t skipped = 0;
			for (likeslog::Record& r : likeslog::read(file, &skipped)) {
				out.push_back(std::move(r));
			}
			CHECK(skipped == 0);
		}
		return out;
	}
	/// The records of one event.
	[[nodiscard]] std::vector<likeslog::Record> of(const char* event) const {
		std::vector<likeslog::Record> out = records();
		std::erase_if(out, [event](const likeslog::Record& r) { return r.event != event; });
		return out;
	}
	/// The like and unlike deltas.
	[[nodiscard]] std::vector<likeslog::Record> deltas() const {
		std::vector<likeslog::Record> out = records();
		std::erase_if(out, [](const likeslog::Record& r) { return r.event != "like" && r.event != "unlike"; });
		return out;
	}
};

/// A clock a case moves by hand, for the log (ExploreStage::setLogClockForTest).
struct LogClock {
	double now = 2.0e9;
	std::function<double()> fn() {
		return [this] { return now; };
	}
};

} // namespace

TEST_CASE("LIKES LOG: a like writes one delta — op, id, index and only that like, its angles equal to the stored like's — and an unlike one more; the set is written once, as a snapshot") {
	ExploreFixture f;
	const LikesLogDir log;
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	walkOffOrigin(f, 2);
	f.stage.toggleLike();
	REQUIRE(f.bridge.terrainLikes().likes.size() == 1);
	const TerrainLike& stored = f.bridge.terrainLikes().likes[0];
	std::vector<likeslog::Record> deltas = log.deltas();
	REQUIRE(deltas.size() == 1);
	const likeslog::Record& liked = deltas[0];
	CHECK(liked.event == "like");
	CHECK(liked.cause == "like");
	CHECK(liked.index == 0);
	CHECK(liked.engine == static_cast<uint16_t>(OscType::Additive));
	CHECK(liked.slot == 1);
	CHECK(liked.strength == f.stage.likeStrength());
	CHECK(liked.session == likeslog::session());
	REQUIRE(liked.like.has_value());
	// BY VALUE, every site's angle as heard and the full point, against what the patch stores.
	CHECK(liked.like->angles == stored.angles);
	CHECK(liked.like->point == stored.point);
	CHECK(*liked.like == stored);
	CHECK(std::count_if(stored.angles.begin(), stored.angles.end(), [](float a) { return a != 0.0f; }) == 340);
	// ONLY THAT LIKE: no set on a delta, so a line does not grow with the set.
	CHECK_FALSE(liked.likes.has_value());
	const std::string text = likeslog::line(liked);
	CHECK(text.find("\"likes\"") == std::string::npos);
	CHECK(text.find("\"sound\":\"" + likeslog::soundId(stored) + "\"") != std::string::npos);
	CHECK(text.find("\"scheme\":\"P-MOBIUS\"") != std::string::npos);
	CHECK(text.find("\"hp\":{\"k0\":3,\"kMax\":4,\"knn\":2,\"iters\":40,\"lut\":129,\"widthFloor\":0.005") != std::string::npos); // 17 digits
	CHECK(text.find(",\"pinWidth\":0.02}") != std::string::npos);
	// THE SET, ONCE: one snapshot (the set as it stood before the first edit: empty).
	const std::vector<likeslog::Record> snapshots = log.of("snapshot");
	REQUIRE(snapshots.size() == 1);
	REQUIRE(snapshots[0].likes.has_value());
	CHECK(snapshots[0].likes->empty());
	// THE UNLIKE: one more delta, naming the like it removed and where it stood.
	f.stage.toggleLike();
	REQUIRE(f.bridge.terrainLikes().likes.empty());
	deltas = log.deltas();
	REQUIRE(deltas.size() == 2);
	CHECK(deltas[1].event == "unlike");
	CHECK(deltas[1].index == 0);
	CHECK(deltas[1].like == deltas[0].like);
	CHECK(log.of("snapshot").size() == 1);
}

TEST_CASE("LIKES LOG: an empty FEATHERS_LIKES_LOG_DIR turns logging off — a like, an unlike and a settled walk write nothing, and every test binary starts that way") {
	// EVERY TEST BINARY STARTS OFF (tests/event_log_off.cpp), so no case ever appends to the dataset.
	REQUIRE(std::getenv(likeslog::kEnvVar) != nullptr);
	CHECK(likeslog::directory().empty());
	ExploreFixture f;
	LogClock clock;
	f.stage.setLogClockForTest(clock.fn());
	const LikesLogDir log; // a real directory, to show the same steps DO write there
	{
		const ScopedEnv off(likeslog::kEnvVar, "");
		CHECK(likeslog::directory().empty());
		f.selection.select(1);
		walkOffOrigin(f, 2);
		f.stage.toggleLike();
		f.stage.logTick();
		clock.now += 5.0;
		f.stage.logTick();
		walkOffOrigin(f, 1);
		f.stage.toggleLike();
		REQUIRE(f.bridge.terrainLikes().likes.size() == 2);
		CHECK_FALSE(std::filesystem::exists(log.path));
	}
	// NOT VACUOUS: with the directory named again, the same press writes.
	walkOffOrigin(f, 1);
	f.stage.toggleLike();
	CHECK(log.deltas().size() == 1);
}

TEST_CASE("LIKES LOG: replaying the snapshot plus the deltas reproduces the live like set exactly — likes, an unlike, a clear, an undo and a redo, and a patch load between them") {
	ExploreFixture f;
	const LikesLogDir log;
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	for (int i = 0; i < 3; ++i) {
		walkOffOrigin(f, 1);
		f.stage.toggleLike();
	}
	f.stage.toggleLike(); // stands on the newest: an unlike
	// A PATCH LOAD the log did not see: two likes of the first three, in another order, and one of another sound.
	TerrainLikes loaded = f.bridge.terrainLikes();
	REQUIRE(loaded.likes.size() == 2);
	std::swap(loaded.likes[0], loaded.likes[1]);
	TerrainLike other = loaded.likes[0];
	other.slot = 0;
	loaded.likes.insert(loaded.likes.begin() + 1, other);
	f.bridge.restoreTerrainLikes(loaded);
	f.stage.logTick();
	// THE LOAD IS WRITTEN AT THE NEXT FRAME, before any edit: a snapshot of the loaded set.
	REQUIRE(log.of("snapshot").size() == 2);
	CHECK(log.of("snapshot")[1].likes == loaded.likes);
	walkOffOrigin(f, 1);
	f.stage.toggleLike();
	walkOffOrigin(f, 1);
	f.stage.toggleLike();
	const TerrainLikes kept = f.bridge.terrainLikes();
	REQUIRE(kept.likes.size() == 5);
	f.stage.clearLikes(); // sound B's four, around sound A's one
	REQUIRE(f.bridge.terrainLikes().likes.size() == 1);
	f.stage.undo();
	f.stage.redo();
	f.stage.undo();
	REQUIRE(f.bridge.terrainLikes() == kept);
	const std::vector<likeslog::Record> records = log.records();
	CHECK(log.of("snapshot").size() == 2); // the first edit's, and the load's
	const likeslog::Replay replayed = likeslog::replay(records);
	CHECK(replayed.mismatches == 0);
	CHECK(replayed.snapshots == 2);
	// EXACTLY, order and all.
	CHECK(replayed.likes == f.bridge.terrainLikes());
	// AND THE WARP THE REPLAYED SET DERIVES is the editor's, by content.
	TerrainWindow window = f.stage.terrainWindowNow();
	const LikeWarp editor = window.likeWarp;
	REQUIRE(editor.warp != nullptr);
	window.likes = std::make_shared<const std::vector<TerrainLike>>(replayed.likesOf(static_cast<uint16_t>(OscType::V5), 1));
	CHECK(deriveLikeWarp(window) == editor);
	// NOT VACUOUS: a replay missing one delta rebuilds a different set.
	std::vector<likeslog::Record> lossy = records;
	// THE REDO'S UNLIKE: nothing after it rewrites the set whole, so the undo that follows lands on the wrong set.
	const auto unlike = std::find_if(lossy.rbegin(), lossy.rend(), [](const likeslog::Record& r) { return r.event == "unlike"; });
	REQUIRE(unlike != lossy.rend());
	lossy.erase(std::next(unlike).base());
	const likeslog::Replay broken = likeslog::replay(lossy);
	CHECK((broken.mismatches > 0 || !(broken.likes == f.bridge.terrainLikes())));
}

TEST_CASE("LIKES LOG: the walk — a still stance writes nothing more, a drag writes nothing until it settles, the settle writes one line with the previous stance's dwell") {
	ExploreFixture f;
	const LikesLogDir log;
	LogClock clock;
	f.stage.setLogClockForTest(clock.fn());
	f.selection.select(1);
	f.set(ParamId::OscType, static_cast<double>(OscType::V5));
	walkOffOrigin(f, 1);
	// THE FIRST STANCE SETTLES: one line once it has been still kWalkDwellSeconds, not before.
	const double arrived = clock.now;
	f.stage.logTick();
	clock.now += 0.5;
	f.stage.logTick();
	CHECK(log.of("walk").empty());
	clock.now = arrived + likeslog::kWalkDwellSeconds;
	f.stage.logTick();
	const TerrainLike first = f.stage.stanceNow();
	REQUIRE(log.of("walk").size() == 1);
	// STILL, FOR A LONG TIME: nothing more.
	for (int i = 0; i < 200; ++i) {
		clock.now += 0.1;
		f.stage.logTick();
	}
	CHECK(log.of("walk").size() == 1);
	const double left = clock.now;
	// A DRAG through many places, a tick at each and never still for a second: nothing.
	for (int i = 0; i < 8; ++i) {
		walkOffOrigin(f, 1);
		clock.now += 0.25;
		f.stage.logTick();
	}
	CHECK(log.of("walk").size() == 1);
	// IT SETTLES: one line, the stance's full point, how long it was held, and how long the first stance was held in all.
	const double settled = clock.now;
	clock.now += 1.2;
	f.stage.logTick();
	const std::vector<likeslog::Record> walks = log.of("walk");
	REQUIRE(walks.size() == 2);
	CHECK(walks[0].like == first);
	CHECK_FALSE(walks[0].dwell.has_value());
	CHECK(walks[1].like == f.stage.stanceNow());
	CHECK_FALSE(walks[1].like == first);
	REQUIRE(walks[1].held.has_value());
	CHECK(*walks[1].held == doctest::Approx(clock.now - settled).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	REQUIRE(walks[1].dwell.has_value());
	// THE FIRST STANCE'S DWELL: from its arrival to the first move away (the first tick of the drag).
	CHECK(*walks[1].dwell == doctest::Approx(left + 0.25 - arrived).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	CHECK(std::count_if(walks[1].like->angles.begin(), walks[1].like->angles.end(), [](float a) { return a != 0.0f; }) == 57); // V5: the table, the authors and the vowel
}

TEST_CASE("LIKES LOG: a like on a stance the walk has not logged writes its walk line too, before the like, settled or not") {
	ExploreFixture f;
	const LikesLogDir log;
	LogClock clock;
	f.stage.setLogClockForTest(clock.fn());
	f.selection.select(1);
	walkOffOrigin(f, 1);
	f.stage.logTick();
	clock.now += 0.3; // not yet settled
	f.stage.toggleLike();
	const std::vector<likeslog::Record> records = log.records();
	std::vector<std::string> events;
	for (const likeslog::Record& r : records) {
		events.push_back(r.event);
	}
	CHECK(events == std::vector<std::string> { "snapshot", "walk", "like" });
	const likeslog::Record& walk = records[1];
	REQUIRE(walk.held.has_value());
	CHECK(*walk.held == doctest::Approx(0.3).epsilon(1e-5)); // the clock is near 2e9 s, so a difference of it carries ~1e-7 s
	CHECK(walk.like == f.bridge.terrainLikes().likes[0]);
	// AND NOT TWICE: the stance is logged now, so settling there writes nothing more.
	clock.now += 5.0;
	f.stage.logTick();
	CHECK(log.of("walk").size() == 1);
}

TEST_CASE("pluck: the terrain pictures the string as heard — its features are ks::heardSpectrum's, and the material moves them") {
	// THE VALUE, restated independently of terrainCycleOn's Pluck branch: the place's Additive spectrum (buildPartialSound, the bank's
	// road on Pluck: neutral bias) and its five material fields through a picture's scalers, handed to ks::heardSpectrum, summed by
	// partials::cycleOf over the lens's partials, reduced by terrainFeaturesOfCycle. The map's features must be that, to the bit.
	using namespace feathers;
	const uint16_t zone = 310;
	const float offset = 0.37f;
	const double tx = 0.21;
	const double ty = -0.13;
	const double home = sliceHome(zone, offset, 0.0);
	const ui::TerrainFrames frames = ui::terrainFramesAt(SliceOrientationPin {}, home, kNoPlaneResidue);
	const auto restated = [&](const StringBias& bias) {
		partials::PartialSpectrum tone;
		buildPartialSound(frames.partials, home, tx, ty, tone, kFieldNeutral, {});
		const StringFields material = stringFieldsWithOverrides(readStringFields(frames.partials, home, tx, ty), bias);
		partials::PartialSpectrum heard;
		ks::heardSpectrum(tone, material, ui::kAdditiveLensPartials, heard, {}, ui::kTerrainCycleSamples / 2.0 - 0.75);
		std::array<float, ui::kTerrainCycleSamples> cycle {};
		partials::cycleOf(heard, ui::kAdditiveLensPartials, ui::kAdditiveLensPartials, nullptr, ui::kTerrainCycleSamples,
		                  static_cast<double>(ui::kTerrainCycleSamples), cycle.data());
		return ui::terrainFeaturesOfCycle(cycle, nullptr);
	};
	const auto drawn = [&](const StringBias& bias) {
		ui::AdditivePicture picture;
		picture.string = bias;
		return ui::terrainFeaturesOn(OscType::Pluck, frames, zone, offset, tx, ty, 0.0, {}, WarpMode::Off, WeaveModel::Ring, {}, {},
		                             picture);
	};
	for (const StringBias bias : { StringBias {}, StringBias { 1.0f, 0.0f, 0.5f, 0.0f, 1.0f } }) {
		const ui::TerrainFeatures a = drawn(bias);
		const ui::TerrainFeatures b = restated(bias);
		CHECK(a.energy == b.energy);
		CHECK(a.centroid == b.centroid);
		CHECK(a.roughness == b.roughness);
	}
	// THE MATERIAL MOVES THE PICTURE, each in the direction it sounds (the second assertion, that the first is not vacuous):
	// dark material reads darker (lower centroid) than bright at the same place; a short string reads dimmer than a long one;
	// and a stretched string's grain is not the harmonic one's (its partials do not close on the cycle: which WAY the grain
	// moves depends on the place, so only that it moves is asserted).
	const auto with = [&](float brightness, float sustain, float stretch) {
		return drawn(StringBias { brightness, sustain, kFieldNeutral, 0.0f, stretch });
	};
	CHECK(with(0.0f, 0.5f, 0.0f).centroid < with(1.0f, 0.5f, 0.0f).centroid);
	CHECK(with(0.5f, 0.0f, 0.0f).energy < with(0.5f, 1.0f, 0.0f).energy);
	CHECK(with(0.5f, 0.5f, 1.0f).roughness != with(0.5f, 0.5f, 0.0f).roughness);
	// AND IT IS NOT ADDITIVE'S PICTURE OF THE SAME PLACE: the string is its own map.
	const ui::TerrainFeatures additive = ui::terrainFeaturesOn(OscType::Additive, frames, zone, offset, tx, ty, 0.0);
	CHECK(additive.centroid != drawn({}).centroid);
}

// THE DRUM KIT PANEL AND THE DRUM SURROUND: only what the selected pad's
// kind reads is drawn (hidden, not greyed), the pads and the kit's structure only in Kit mode, and the hit picture read from
// the engine's own mapping.

TEST_CASE("drum kit panel: the selected pad's set alone is drawn, and only what its kind reads") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	CHECK(panel.padsShownForTest());
	// PAD A, a Kick (One-shot): Drive and Sweep Time drawn, a Clap's Claps and Spread not, Release not (no Gate).
	CHECK(panel.drawnForTest(0, DrumControl::Drive));
	CHECK(panel.drawnForTest(0, DrumControl::SweepTime));
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Count));
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Spread));
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Release));
	CHECK(panel.drawnForTest(0, DrumControl::Kind));
	CHECK(panel.drawnForTest(0, DrumControl::Note));
	// NO OTHER PAD'S SET IS DRAWN.
	for (size_t pad = 1; pad < 8; ++pad) {
		CAPTURE(pad);
		CHECK_FALSE(panel.drawnForTest(pad, DrumControl::Level));
		CHECK_FALSE(panel.drawnForTest(pad, DrumControl::Kind));
	}
	// PAD E, a Clap, by the hand: Claps, Spread and Drive drawn, Sweep Time not; and A's set goes.
	panel.clickPadForTest(4);
	(void)panel.update();
	CHECK(f.selection.slot() == 4);
	CHECK(panel.drawnForTest(4, DrumControl::Count));
	CHECK(panel.drawnForTest(4, DrumControl::Spread));
	CHECK(panel.drawnForTest(4, DrumControl::Drive)); // drive on every kind (2026-11-06)
	CHECK_FALSE(panel.drawnForTest(4, DrumControl::SweepTime));
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Level));
	// A GATE PAD draws its Release.
	f.set(feathers::drumParam(4, DrumControl::Trigger), 1.0);
	(void)panel.update();
	CHECK(panel.drawnForTest(4, DrumControl::Release));
}

TEST_CASE("drum kit panel: Pitched hides the pads and the kit's structure, and pad A's block is the one drum's") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 0.0);
	f.selection.select(3);
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	CHECK_FALSE(panel.padsShownForTest());
	for (const DrumControl c : { DrumControl::Kind, DrumControl::Note, DrumControl::Choke, DrumControl::Voices }) {
		CAPTURE(static_cast<int>(c));
		CHECK_FALSE(panel.drawnForTest(0, c));
	}
	CHECK(panel.drawnForTest(0, DrumControl::Level));
	CHECK(panel.drawnForTest(0, DrumControl::Trigger));
	CHECK_FALSE(panel.drawnForTest(3, DrumControl::Level)); // the selection is not a pad here
	CHECK(panel.pictureForTest().kindForTest() == feathers::drum::Kind::Membrane); // the engine's Kind's default
}

TEST_CASE("drum kit panel: the hit picture is the pad at its REAL place (and the Walk), by the bank's road and the voice's mapping") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(feathers::drumParam(5, DrumControl::Tune), -5.0);
	f.set(feathers::drumParam(5, DrumControl::Decay), std::log2(0.8));
	f.set(ParamId::FPlaceX, 0.3);
	f.selection.select(5);
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	// Restated: the character the synth builds from these params, the bank's pads, the voice's mapping at velocity 0.8.
	const auto expected = [&f] {
		const auto read = [&f](ParamId id) { return f.params.base(id); };
		const Character c = feathers::characterFromParams(read, OscType::Drum, feathers::AuditionTuning {}, feathers::AnchorPoint {},
		                                                  feathers::SoundTravel {}, feathers::SoundOrientations {},
		                                                  feathers::SoundResidue {}, feathers::SoundGamma {}, feathers::Preview {},
		                                                  feathers::EarSide::Head);
		feathers::drum::Controls k;
		k.decaySeconds = std::exp2(read(feathers::drumParam(5, DrumControl::Decay)));
		k.sweepSeconds = std::exp2(read(feathers::drumParam(5, DrumControl::SweepTime)));
		k.drive = read(feathers::drumParam(5, DrumControl::Drive));
		k.count = static_cast<int>(std::lround(read(feathers::drumParam(5, DrumControl::Count))));
		k.spreadSeconds = std::exp2(read(feathers::drumParam(5, DrumControl::Spread)));
		k.pitchRangeSemitones = 3.0;
		k.wireDecaySeconds = std::exp2(read(feathers::drumParam(5, DrumControl::WireDecay)));
		k.clickDecaySeconds = std::exp2(read(feathers::drumParam(5, DrumControl::ClickDecay)));
		k.bellStartHz = std::exp2(read(feathers::drumParam(5, DrumControl::BellStart)));
		k.bellEndHz = std::exp2(read(feathers::drumParam(5, DrumControl::BellEnd)));
		k.bellSeconds = std::exp2(read(feathers::drumParam(5, DrumControl::BellTime)));
		k.kickHoldSeconds = read(feathers::drumParam(5, DrumControl::KickHold)) / 1000.0;
		k.kickDip = read(feathers::drumParam(5, DrumControl::KickDip));
		k.kickDipSeconds = read(feathers::drumParam(5, DrumControl::KickDipTime)) / 1000.0;
		k.clickSweepOctaves = read(feathers::drumParam(5, DrumControl::ClickSweep));
		k.clickSweepSeconds = std::exp2(read(feathers::drumParam(5, DrumControl::ClickSweepTime)));
		k.startPhaseDegrees = read(feathers::drumParam(5, DrumControl::StartPhase));
		k.driveCurve = static_cast<int>(std::lround(read(feathers::drumParam(5, DrumControl::DriveCurve))));
		k.driveTone = read(feathers::drumParam(5, DrumControl::DriveTone));
		k.clapQ = read(feathers::drumParam(5, DrumControl::ClapQ));
		k.clapHighPassHz = std::exp2(read(feathers::drumParam(5, DrumControl::ClapHighPass)));
		k.bell2Hz = std::exp2(read(feathers::drumParam(5, DrumControl::Bell2Hz)));
		k.bell2Seconds = std::exp2(read(feathers::drumParam(5, DrumControl::Bell2Time)));
		k.velocityNoise = read(feathers::drumParam(5, DrumControl::VelocityNoise));
		k.velocityZone = read(feathers::drumParam(5, DrumControl::VelocityZone));
		k.wires = read(feathers::drumParam(5, DrumControl::Wires));
		k.bells = read(feathers::drumParam(5, DrumControl::Bells));
		k.width = read(feathers::drumParam(5, DrumControl::Width));
		return feathers::drum::lawsFor(feathers::drum::Kind::Membrane, feathers::buildDrumFor(c).pads[5], k,
		                               110.0 * std::pow(2.0, -5.0 / 12.0), 0.8);
	};
	(void)panel.update();
	const feathers::drum::Laws here = panel.pictureForTest().lawsForTest();
	CHECK(panel.pictureForTest().kindForTest() == feathers::drum::Kind::Membrane);
	CHECK(here == expected());
	// AND THE WALK MOVES IT: the same restatement at the walked place, which is not the unwalked one.
	f.set(ParamId::DrumWalkX, 0.6);
	f.set(ParamId::DrumWalkY, -0.4);
	(void)panel.update();
	CHECK(panel.pictureForTest().lawsForTest() == expected());
	CHECK(!(panel.pictureForTest().lawsForTest() == here));
}

TEST_CASE("drum kit panel: the handles on the hit picture set Tune, Decay, Level and a Kick's Sweep Time and Drive, by value") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	// A KICK: every handle is drawn.
	for (const H h : { H::Tune, H::SweepTime, H::Decay, H::Level, H::Drive }) {
		CAPTURE(static_cast<int>(h));
		CHECK(p.handleShown(h));
	}
	// TUNE: up by one octave's height on the plot's log axis is +12 semitones.
	const float octave = (p.height() * 0.58f - 8.0f) / static_cast<float>(std::log2(1000.0));
	p.dragForTest(H::Tune, 0.0f, -octave);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::Tune)) - 12.0) < 1.0e-3);
	(void)panel.update();
	// DECAY: moved to where 1.2 times the T60 is drawn, Decay grows by 1.2 (Length's multiplier unchanged).
	const double decay0 = f.params.base(feathers::drumParam(0, DrumControl::Decay));
	const double t60 = p.lawsForTest().t60;
	// (the plot's own time axis: square-root time, so the target is read from it, not assumed linear)
	p.dragForTest(H::Decay, p.xOf(1.2 * t60) - p.handleAt(H::Decay).x, 0.0f); // inside the view (it spans 1.35 T60)
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::Decay)) - (decay0 + std::log2(1.2))) < 1.0e-3);
	(void)panel.update();
	// LEVEL: down by a sixth of the envelope's height is −11 dB (the plot spans −60 to +6).
	const float env = p.height() - 8.0f - p.height() * 0.64f;
	p.dragForTest(H::Level, 0.0f, env / 6.0f);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::Level)) - (-11.0)) < 1.0e-3);
	// DRIVE: its travel is the envelope's upper three quarters (the handle's floor sits a quarter up), so up by a quarter of
	// the envelope adds a third.
	const double drive0 = f.params.base(feathers::drumParam(0, DrumControl::Drive));
	(void)panel.update();
	p.dragForTest(H::Drive, 0.0f, -0.25f * env);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::Drive)) - (drive0 + 0.25 / 0.75)) < 1.0e-3);
	// A CLAP READS NO SWEEP TIME (no handle), but Drive, as every kind does since 2026-11-06.
	panel.clickPadForTest(4);
	(void)panel.update();
	CHECK_FALSE(p.handleShown(H::SweepTime));
	CHECK(p.handleShown(H::Drive)); // drive on every kind (2026-11-06)
	CHECK(p.handleShown(H::Decay));
}

TEST_CASE("drum surround: Metal is drawn on a pad that reads it and not on a Kick; the Walk only in Kit mode") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.stage.update();
	using DF = ExploreStage::DrumField;
	CHECK_FALSE(f.stage.drumFieldKnobForTest(DF::Metal, 0).isVisible()); // A, a Kick
	CHECK(f.stage.drumFieldKnobForTest(DF::Sweep, 0).isVisible());
	CHECK(f.stage.drumWalkKnobForTest(0).isVisible());
	f.selection.select(2); // C, Metal
	f.stage.update();
	CHECK(f.stage.drumFieldKnobForTest(DF::Metal, 2).isVisible());
	CHECK(f.stage.drumFieldKnobForTest(DF::Metal, 2).enabled());
	CHECK_FALSE(f.stage.drumFieldKnobForTest(DF::Sweep, 2).isVisible());
	// A KIT PAD NEVER GREYS FOR ITS SWITCH: C is switched off in the default patch, and is a pad all the same.
	f.set(ParamId::CEnable, 0.0);
	f.stage.update();
	CHECK(f.stage.drumFieldKnobForTest(DF::Metal, 2).enabled());
	// PITCHED: the Walk goes, and the scalers are the engine's Kind's (Membrane reads Metal).
	f.set(ParamId::DrumMode, 0.0);
	f.stage.update();
	CHECK_FALSE(f.stage.drumWalkKnobForTest(0).isVisible());
	CHECK_FALSE(f.stage.drumWalkKnobForTest(1).isVisible());
}

TEST_CASE("drum terrain: each place draws the selected sound's kind as heard there, through its scalers, by the bank's road") {
	// Restated: the place's seven fields (readDrumFields on the Additive frame, the scalers over them), the kind's heard
	// spectrum (drum::heardSpectrum), summed into the lens's cycle exactly as Additive's lens sums.
	SliceOrientationPin pin;
	const double home = 0.37;
	const TerrainFrames frames = terrainFramesAt(pin, home, kNoPlaneResidue);
	const auto zone = static_cast<uint16_t>(std::clamp(std::lround(home * 1023.0), 0L, 1023L));
	const double base = home - static_cast<double>(zone) / 1023.0;
	for (const auto kind : { feathers::drum::Kind::Membrane, feathers::drum::Kind::Metal, feathers::drum::Kind::Kick }) {
		CAPTURE(static_cast<int>(kind));
		AdditivePicture picture;
		picture.drumKind = kind;
		picture.drum.tone = 0.8f;
		std::array<float, kTerrainCycleSamples> got {};
		terrainCycleOn(OscType::Drum, frames, zone, 0.0f, 0.21, -0.4, base, {}, WarpMode::Off, WeaveModel::Ring, {}, {},
		               picture, got);
		const double placeHome = sliceHome(zone, 0.0f, base);
		const feathers::drum::Fields fields = drumFieldsWithOverrides(
		    readDrumFields(frames.partials, placeHome, 0.21, -0.4), picture.drum);
		partials::PartialSpectrum heard;
		feathers::drum::heardSpectrum(kind, fields, kAdditiveLensPartials, heard, kTerrainCycleSamples / 2.0 - 0.75);
		std::array<float, kTerrainCycleSamples> want {};
		partials::cycleOf(heard, kAdditiveLensPartials, kAdditiveLensPartials, nullptr, kTerrainCycleSamples,
		                  static_cast<double>(kTerrainCycleSamples), want.data());
		CHECK(got == want);
		// Not vacuous: the kind is in the picture (Membrane's cycle is not Metal's at the same place).
		if (kind == feathers::drum::Kind::Metal) {
			AdditivePicture skin = picture;
			skin.drumKind = feathers::drum::Kind::Membrane;
			std::array<float, kTerrainCycleSamples> membrane {};
			terrainCycleOn(OscType::Drum, frames, zone, 0.0f, 0.21, -0.4, base, {}, WarpMode::Off, WeaveModel::Ring, {}, {},
			               skin, membrane);
			CHECK(membrane != got);
		}
	}
}

TEST_CASE("drum terrain: the window asked for is the selected PAD's kind and scalers in Kit mode, the engine's Kind in Pitched") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(feathers::drumParam(2, DrumControl::FieldTone), 0.9);
	f.selection.select(2); // C, Metal by default
	TerrainWindow w = f.stage.terrainWindowNow();
	CHECK(w.engine == OscType::Drum);
	CHECK(w.additive.drumKind == feathers::drum::Kind::Metal);
	CHECK(w.additive.drum.tone == 0.9f);
	f.selection.select(0); // A, a Kick
	w = f.stage.terrainWindowNow();
	CHECK(w.additive.drumKind == feathers::drum::Kind::Kick);
	f.set(ParamId::DrumMode, 0.0);
	f.set(ParamId::DrumPitchedKind, 5.0); // String
	w = f.stage.terrainWindowNow();
	CHECK(w.additive.drumKind == feathers::drum::Kind::String);
}

TEST_CASE("drum likes: a pad's like is of the kind it was made on, so a pad switched to another kind is not drawn or warped by it") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.stage.update();
	// THE STANCE A LIKE STORES CARRIES THE PAD'S KIND (A, a Kick).
	const TerrainLike kick = f.stage.stanceNow();
	CHECK(kick.kind == static_cast<uint16_t>(feathers::drum::Kind::Kick));
	TerrainLike made = kick;
	made.engine = static_cast<uint16_t>(OscType::Drum);
	made.slot = 0;
	made.kind = static_cast<uint16_t>(feathers::drum::Kind::Kick);
	CHECK(f.stage.likeApplies(made));
	TerrainLike metal = made;
	metal.kind = static_cast<uint16_t>(feathers::drum::Kind::Metal);
	CHECK_FALSE(f.stage.likeApplies(metal));
	// SWITCH PAD A TO METAL: now the Metal like is its, the Kick like is not.
	f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(feathers::drum::Kind::Metal));
	CHECK(f.stage.likeApplies(metal));
	CHECK_FALSE(f.stage.likeApplies(made));
	// AND THE WINDOW THE WARP IS DERIVED FROM HOLDS ONLY THIS KIND'S LIKES.
	TerrainLikes both;
	both.likes = { made, metal };
	f.bridge.setTerrainLikes(both);
	const TerrainWindow w = f.stage.terrainWindowNow();
	REQUIRE(w.likes != nullptr);
	REQUIRE(w.likes->size() == 1);
	CHECK(w.likes->front().kind == static_cast<uint16_t>(feathers::drum::Kind::Metal));
}

TEST_CASE("drum kit panel: a snare's wires, click and moving bell are handles on its picture, its Wires and Bells zones beside") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.selection.select(1); // B, a Snare
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	for (const H h : { H::WireDecay, H::ClickDecay, H::BellStart, H::BellEnd }) {
		CAPTURE(static_cast<int>(h));
		CHECK(p.handleShown(h));
	}
	for (const DrumControl c : { DrumControl::Wires, DrumControl::Bells, DrumControl::VelocityZone }) { // the zones (§14)
		CAPTURE(static_cast<int>(c));
		CHECK(panel.drawnForTest(1, c));
	}
	// THE BELL'S START: up by one octave's height is an octave higher (log2 Hz +1).
	const double start0 = f.params.base(feathers::drumParam(1, DrumControl::BellStart));
	const float octave = (p.height() * 0.58f - 8.0f) / static_cast<float>(std::log2(1000.0));
	p.dragForTest(H::BellStart, 0.0f, -octave);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(1, DrumControl::BellStart)) - (start0 + 1.0)) < 1.0e-3);
	(void)panel.update();
	// THE BELL'S END: across to where 1.2× its time is drawn sets Bell Time ×1.2; down an octave sets Bell End an octave lower.
	const double time0 = f.params.base(feathers::drumParam(1, DrumControl::BellTime));
	const double end0 = f.params.base(feathers::drumParam(1, DrumControl::BellEnd));
	const double t = 3.0 * p.lawsForTest().bellTau;
	p.dragForTest(H::BellEnd, p.xOf(1.2 * t) - p.handleAt(H::BellEnd).x, octave);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(1, DrumControl::BellTime)) - (time0 + std::log2(1.2))) < 1.0e-3);
	CHECK(std::abs(f.params.base(feathers::drumParam(1, DrumControl::BellEnd)) - (end0 - 1.0)) < 1.0e-3);
	(void)panel.update();
	// THE WIRES: across to 1.2× their T60, Wire Decay ×1.2 (Length's multiplier unchanged).
	const double wire0 = f.params.base(feathers::drumParam(1, DrumControl::WireDecay));
	p.dragForTest(H::WireDecay, p.xOf(1.2 * p.lawsForTest().wireT60) - p.handleAt(H::WireDecay).x, 0.0f);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(1, DrumControl::WireDecay)) - (wire0 + std::log2(1.2))) < 1.0e-3);
	// A KICK HAS NONE OF THEM.
	panel.clickPadForTest(0);
	(void)panel.update();
	for (const H h : { H::WireDecay, H::ClickDecay, H::BellStart, H::BellEnd }) {
		CHECK_FALSE(p.handleShown(h));
	}
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Bells));
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Wires));
}

TEST_CASE("hardening: a drag on a drum handle ends on the control it began on when the pad's Kind changes mid-drag") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.selection.select(1); // B, a Snare: it has the Wire Decay handle
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y);
	(void)panel.update();
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	REQUIRE(p.handleShown(H::WireDecay));

	const auto wire = static_cast<clap_id>(feathers::drumParam(1, DrumControl::WireDecay));
	int begins = 0;
	int ends = 0;
	const auto count = [&] {
		f.bridge.drain([&](const UiParamEvent& event) {
			if (event.kind == UiParamEvent::Kind::Value) {
				f.params.setBase(event.id, event.value); // as `settleGesture` does: the drain is what lands a value
			}
			if (event.id != wire) {
				return;
			}
			if (event.kind == UiParamEvent::Kind::GestureBegin) {
				++begins;
			}
			else if (event.kind == UiParamEvent::Kind::GestureEnd) {
				++ends;
			}
		});
	};
	count(); // whatever was queued before the press is not this drag's
	begins = 0;
	ends = 0;

	const double wire0 = f.params.base(feathers::drumParam(1, DrumControl::WireDecay));
	const visage::Point at = p.handleAt(H::WireDecay);
	visage::MouseEvent down;
	down.position = at;
	p.onMouseDown().callback(down);
	count();
	REQUIRE(begins == 1);

	// THE PAD BECOMES A KICK UNDER THE HAND: the next tick's `show` takes the Wire Decay handle away.
	f.set(feathers::drumParam(1, DrumControl::Kind), static_cast<double>(feathers::drum::Kind::Kick));
	(void)panel.update();
	REQUIRE_FALSE(p.handleShown(H::WireDecay));

	visage::MouseEvent move;
	move.position = { at.x + 30.0f, at.y };
	p.onMouseDrag().callback(move);
	visage::MouseEvent up;
	up.position = move.position;
	p.onMouseUp().callback(up);
	count();
	CHECK(begins == 1);
	CHECK(ends == 1); // the gesture the press began is the one that ends, once
	// AND THE DRAG KEPT WRITING THE CONTROL IT HELD: its value moved off where it started.
	f.bridge.publish(f.params);
	CHECK(f.params.base(feathers::drumParam(1, DrumControl::WireDecay)) != wire0);
}

TEST_CASE("drum kit panel: the title names the selected pad's kind now, and the handles' labels never overlap or leave the plot") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	CHECK(panel.headerTitle() == "Drum Kit · A · Kick");
	f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(feathers::drum::Kind::Snare));
	(void)panel.update();
	CHECK(panel.headerTitle() == "Drum Kit · A · Snare");
	// THE LABELS, on the busiest picture there is (a Snare with every handle) and on a Kick: pairwise clear, inside the plot.
	for (const auto kind : { feathers::drum::Kind::Snare, feathers::drum::Kind::Kick }) {
		CAPTURE(static_cast<int>(kind));
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(kind));
		(void)panel.update();
		const ui::HitPicture& p = panel.pictureForTest();
		const auto rects = p.labelRects();
		std::vector<visage::Bounds> shown;
		for (size_t i = 0; i < ui::HitPicture::kHandleCount; ++i) {
			if (p.handleShown(static_cast<ui::HitPicture::Handle>(i))) {
				const visage::Bounds b = rects[i];
				CHECK(b.x() >= 0.0f);
				CHECK(b.y() >= 0.0f);
				CHECK(b.right() <= p.width());
				CHECK(b.bottom() <= p.height());
				shown.push_back(b);
			}
		}
		CHECK(shown.size() >= 4);
		for (size_t a = 0; a < shown.size(); ++a) {
			for (size_t b = a + 1; b < shown.size(); ++b) {
				CAPTURE(a);
				CAPTURE(b);
				const bool overlap = shown[a].x() < shown[b].right() && shown[b].x() < shown[a].right()
				                     && shown[a].y() < shown[b].bottom() && shown[b].y() < shown[a].bottom();
				CHECK_FALSE(overlap);
			}
		}
	}
}

TEST_CASE("drum kit panel: a kick's Hold, fishtail and click sweep are handles on its picture, its start phase a pill") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(feathers::drumParam(0, DrumControl::KickHold), 10.0);
	f.set(feathers::drumParam(0, DrumControl::KickDip), 0.3);
	f.set(feathers::drumParam(0, DrumControl::ClickSweep), 1.0);
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	for (const H h : { H::Hold, H::Dip, H::ClickSweep }) {
		CAPTURE(static_cast<int>(h));
		CHECK(p.handleShown(h));
	}
	CHECK(panel.drawnForTest(0, DrumControl::StartPhase));
	// HOLD: across to where 2× Hold is drawn makes it 20 ms.
	p.dragForTest(H::Hold, p.xOf(0.02) - p.handleAt(H::Hold).x, 0.0f);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::KickHold)) - 20.0) < 1.0e-3);
	(void)panel.update();
	// THE FISHTAIL: across sets its time (to 30 ms); down to −12 dB below the peak sets a depth of 1 − 10^(−12/20).
	const float env = p.height() - 8.0f - p.height() * 0.64f;
	const visage::Point dip = p.handleAt(H::Dip);
	const float peakY = p.handleAt(H::Level).y;
	const float wantY = peakY + static_cast<float>(12.0 / 66.0) * env;
	p.dragForTest(H::Dip, p.xOf(0.03) - dip.x, wantY - dip.y);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::KickDipTime)) - 30.0) < 1.0e-3);
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::KickDip)) - (1.0 - std::pow(10.0, -12.0 / 20.0))) < 1.0e-3);
	(void)panel.update();
	// THE CLICK SWEEP: up by an octave's height adds an octave.
	const double click0 = f.params.base(feathers::drumParam(0, DrumControl::ClickSweep));
	const float octave = (p.height() * 0.58f - 8.0f) / static_cast<float>(std::log2(1000.0));
	p.dragForTest(H::ClickSweep, 0.0f, -octave);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::ClickSweep)) - (click0 + 1.0)) < 1.0e-3);
	// A SNARE HAS NONE OF THEM.
	f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(feathers::drum::Kind::Snare));
	(void)panel.update();
	for (const H h : { H::Hold, H::Dip, H::ClickSweep }) {
		CHECK_FALSE(p.handleShown(h));
	}
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::StartPhase));
}

TEST_CASE("drum kit panel: every kind shows Drive on its picture and its curve and tone; a Clap its Q and High Pass; all inside the panel") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(feathers::drumParam(0, DrumControl::Trigger), 1.0); // a Gate pad: Release is drawn too, the fullest row of pills
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	using H = ui::HitPicture::Handle;
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		const auto kind = static_cast<feathers::drum::Kind>(k);
		CAPTURE(k);
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		(void)panel.update();
		CHECK(panel.pictureForTest().handleShown(H::Drive));
		CHECK(panel.drawnForTest(0, DrumControl::Drive));
		CHECK(panel.drawnForTest(0, DrumControl::DriveCurve));
		CHECK(panel.drawnForTest(0, DrumControl::DriveTone));
		const bool clap = kind == feathers::drum::Kind::Clap;
		CHECK(panel.drawnForTest(0, DrumControl::ClapQ) == clap);
		CHECK(panel.drawnForTest(0, DrumControl::ClapHighPass) == clap);
		// EVERY DRAWN BOX, SEGMENT AND PILL SITS INSIDE THE PANEL at its minimum size, however many this kind has.
		for (size_t c = 0; c < kDrumControlCount; ++c) {
			const visage::Frame* frame = panel.controlForTest(0, static_cast<DrumControl>(c));
			if (frame == nullptr || !frame->isVisible()) {
				continue;
			}
			CAPTURE(c);
			CHECK(frame->x() >= 0.0f);
			CHECK(frame->right() <= panel.width() + 1.0e-3f);
			CHECK(frame->bottom() <= panel.height() + 1.0e-3f);
		}
		// AND NO TWO OF THEM OVERLAP.
		for (size_t a = 0; a < kDrumControlCount; ++a) {
			const visage::Frame* fa = panel.controlForTest(0, static_cast<DrumControl>(a));
			for (size_t b = a + 1; b < kDrumControlCount; ++b) {
				const visage::Frame* fb = panel.controlForTest(0, static_cast<DrumControl>(b));
				if (fa == nullptr || fb == nullptr || !fa->isVisible() || !fb->isVisible()) {
					continue;
				}
				CAPTURE(a);
				CAPTURE(b);
				CHECK_FALSE((fa->x() < fb->right() && fb->x() < fa->right() && fa->y() < fb->bottom() && fb->y() < fa->bottom()));
			}
		}
	}
}

TEST_CASE("drum kit panel: a snare's second bell is a handle (across, its ring; up and down, its Hz while the Bells zone has it Free)") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.selection.select(1); // B, a Snare
	f.set(feathers::drumParam(1, DrumControl::Bells), 2.0 / 5.0); // Shell: two bells, linked
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	REQUIRE(p.handleShown(H::Bell2));
	CHECK(panel.drawnForTest(1, DrumControl::Bells));
	CHECK(panel.drawnForTest(1, DrumControl::Bell2Time));
	// LINKED: the handle sits at Metal's ratio × the first bell's end, and up and down writes nothing (the place sets it).
	CHECK_FALSE(panel.drawnForTest(1, DrumControl::Bell2Hz));
	const feathers::drum::Laws& l = p.lawsForTest();
	CHECK(l.bell2Linked);
	CHECK(std::abs(p.handleAt(H::Bell2).y - p.yOfHz(l.bellEndHz * l.bell2Ratio)) < 0.5f);
	const double hzBefore = f.params.base(feathers::drumParam(1, DrumControl::Bell2Hz));
	const float octave = (p.height() * 0.58f - 8.0f) / static_cast<float>(std::log2(1000.0));
	p.dragForTest(H::Bell2, 0.0f, -octave);
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(1, DrumControl::Bell2Hz)) == hzBefore);
	(void)panel.update();
	// ACROSS: to where 1.5× its ring is drawn sets Bell 2 Time ×1.5.
	const double time0 = f.params.base(feathers::drumParam(1, DrumControl::Bell2Time));
	p.dragForTest(H::Bell2, p.xOf(1.5 * p.lawsForTest().bell2T60) - p.handleAt(H::Bell2).x, 0.0f);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(1, DrumControl::Bell2Time)) - (time0 + std::log2(1.5))) < 1.0e-3);
	// FREE (the Knock zone): the handle sits at its own Hz, and up an octave adds an octave to it.
	f.set(feathers::drumParam(1, DrumControl::Bells), 4.0 / 5.0);
	(void)panel.update();
	REQUIRE_FALSE(p.lawsForTest().bell2Linked);
	CHECK(panel.drawnForTest(1, DrumControl::Bell2Hz));
	const double hz0 = f.params.base(feathers::drumParam(1, DrumControl::Bell2Hz));
	CHECK(std::abs(p.handleAt(H::Bell2).y - p.yOfHz(std::exp2(hz0))) < 0.5f);
	p.dragForTest(H::Bell2, 0.0f, -octave);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(1, DrumControl::Bell2Hz)) - (hz0 + 1.0)) < 1.0e-3);
	// A KICK HAS NONE OF IT.
	panel.clickPadForTest(0);
	(void)panel.update();
	CHECK_FALSE(p.handleShown(H::Bell2));
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Bells));
}

TEST_CASE("drum kit panel: the Velocity zone on every kind, Velocity Noise where Noise is read; no Sweep Time on a Membrane or Snare") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		const auto kind = static_cast<feathers::drum::Kind>(k);
		CAPTURE(k);
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		(void)panel.update();
		const bool tension = kind == feathers::drum::Kind::Kick || kind == feathers::drum::Kind::Membrane
		                     || kind == feathers::drum::Kind::Snare;
		(void)tension;
		CHECK(panel.drawnForTest(0, DrumControl::VelocityZone)); // Level, Tone, Tension: every kind reads velocity
		CHECK(panel.drawnForTest(0, DrumControl::VelocityNoise) == feathers::drum::kindReads(kind).noise);
		const bool sweepTime = kind == feathers::drum::Kind::Kick || kind == feathers::drum::Kind::String;
		CHECK(panel.drawnForTest(0, DrumControl::SweepTime) == sweepTime);
	}
}

TEST_CASE("drum kit panel: a Snare's Wires and Bells zones; Width on a Snare and a Clap") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		const auto kind = static_cast<feathers::drum::Kind>(k);
		CAPTURE(k);
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		(void)panel.update();
		const bool snare = kind == feathers::drum::Kind::Snare;
		CHECK(panel.drawnForTest(0, DrumControl::Wires) == snare);
		CHECK(panel.drawnForTest(0, DrumControl::Bells) == snare);
		CHECK(panel.drawnForTest(0, DrumControl::Width) == (snare || kind == feathers::drum::Kind::Clap));
	}
}

namespace {

/// A Drum Kit on the chart, pad A selected and settled, and pad `slot` put on A's own plane (its zone, offset and gamma) at
/// `place`, so its projection onto A's chart is exact: it must be drawn at glassAt(place.y, place.x) to the bit.
void kitWithPadOnAsPlane(ExploreFixture& f, visage::Canvas& canvas, size_t slot, AnchorPoint place) {
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	f.selection.select(0);
	for (const SoundField field : { SoundField::Zone, SoundField::Offset, SoundField::Gamma }) {
		f.set(soundParam(slot, field), f.read(soundParam(0, field)));
	}
	f.bridge.setSoundTravel(slot, place);
	f.stage.draw(canvas);
	f.settleScrub();
	for (int frame = 0; frame < 64 && f.stage.update(); ++frame) {
		f.now += 16;
	}
	f.stage.draw(canvas);
}

std::optional<ui::ExploreStage::PadMark> markOf(const ExploreFixture& f, size_t slot) {
	for (const ui::ExploreStage::PadMark& m : f.stage.padMarksForTest()) {
		if (m.slot == slot) {
			return m;
		}
	}
	return std::nullopt;
}

} // namespace

TEST_CASE("drum kit chart: another pad is drawn at its place on the selected pad's plane, HOLLOW when its kind differs, with the Walk's arrow") {
	ExploreFixture f;
	visage::Canvas canvas;
	kitWithPadOnAsPlane(f, canvas, 2, { 0.003, 0.004 });
	// C is a Metal and A a Kick (the default kit): a hollow mark, exactly where its place is on A's plane.
	REQUIRE(markOf(f, 2).has_value());
	const visage::Point want = f.stage.glassAt(0.004, 0.003);
	CHECK(markOf(f, 2)->at.x == doctest::Approx(want.x).epsilon(1.0e-5));
	CHECK(markOf(f, 2)->at.y == doctest::Approx(want.y).epsilon(1.0e-5));
	CHECK(markOf(f, 2)->hollow);
	CHECK_FALSE(markOf(f, 2)->arrow); // no Walk, no arrow
	CHECK_FALSE(markOf(f, 0).has_value()); // the selected pad is the star, not a mark
	// C MADE A KICK: the same mark, filled.
	f.set(feathers::drumParam(2, DrumControl::Kind), static_cast<double>(feathers::drum::Kind::Kick));
	f.stage.draw(canvas);
	REQUIRE(markOf(f, 2).has_value());
	CHECK_FALSE(markOf(f, 2)->hollow);
	// THE WALK: every pad played (0.002, 0.001) from its place. C's arrow ends where it plays; A's played mark is the Walk's.
	f.set(ParamId::DrumWalkX, 0.002);
	f.set(ParamId::DrumWalkY, 0.001);
	f.stage.draw(canvas);
	REQUIRE(markOf(f, 2).has_value());
	REQUIRE(markOf(f, 2)->arrow);
	const visage::Point tip = f.stage.glassAt(0.005, 0.005);
	CHECK(markOf(f, 2)->tip.x == doctest::Approx(tip.x).epsilon(1.0e-5));
	CHECK(markOf(f, 2)->tip.y == doctest::Approx(tip.y).epsilon(1.0e-5));
	REQUIRE(f.stage.playedMarkDrawn());
	const visage::Point played = f.stage.glassAt(0.001, 0.002);
	CHECK(f.stage.playedMarkAt().x == doctest::Approx(played.x).epsilon(1.0e-5));
	CHECK(f.stage.playedMarkAt().y == doctest::Approx(played.y).epsilon(1.0e-5));
	// NOT ON A DRUM KIT: no marks at all (Pitched mode, the puck and sounds as on every engine).
	f.set(ParamId::DrumMode, 0.0);
	f.stage.draw(canvas);
	CHECK(f.stage.padMarksForTest().empty());
}

TEST_CASE("drum kit chart: selecting a pad in view moves nothing; one off the chart or near its rim makes the view JUMP onto it, unanimated") {
	ExploreFixture f;
	visage::Canvas canvas;
	kitWithPadOnAsPlane(f, canvas, 1, { 0.0, 0.0 });
	const double vy = f.stage.storedViewCentreForTest();
	const double vx = f.stage.storedTravelCentreForTest();
	const double sx = f.stage.travelSpanForTest();
	const double sy = f.stage.homeSpanForTest();
	// B IN VIEW, a fifth of the radius out: the view to the bit.
	f.bridge.setSoundTravel(1, { vx + 0.1 * sx, vy + 0.0 });
	f.selection.select(1);
	f.stage.draw(canvas);
	CHECK(f.stage.storedViewCentreForTest() == vy);
	CHECK(f.stage.storedTravelCentreForTest() == vx);
	CHECK_FALSE(f.stage.viewGlidingForTest());
	CHECK(f.stage.viewCentre() == vy); // and it is the view for B now, not one waiting to recentre
	// D NEAR THE RIM, nine tenths of the radius out: the view jumps onto it, at once.
	f.bridge.setSoundTravel(3, { vx, vy + 0.45 * sy });
	f.selection.select(3);
	f.stage.draw(canvas);
	CHECK(f.stage.storedViewCentreForTest() == vy + 0.45 * sy);
	CHECK(f.stage.storedTravelCentreForTest() == vx);
	CHECK_FALSE(f.stage.viewGlidingForTest());
	// E OFF THE CHART altogether: the same jump.
	f.bridge.setSoundTravel(4, { vx + 3.0 * sx, vy });
	f.selection.select(4);
	f.stage.draw(canvas);
	CHECK(f.stage.storedTravelCentreForTest() == vx + 3.0 * sx);
	CHECK_FALSE(f.stage.viewGlidingForTest());
	// ON ANOTHER ENGINE a selection still centres, in view or not (only a Kit keeps its view).
	f.set(ParamId::DrumMode, 0.0);
	const double before = f.stage.storedTravelCentreForTest();
	f.bridge.setSoundTravel(5, { before + 0.1 * sx, f.stage.storedViewCentreForTest() });
	f.selection.select(5);
	f.stage.draw(canvas);
	CHECK(f.stage.storedTravelCentreForTest() == before + 0.1 * sx);
}

TEST_CASE("drum kit chart: a pad on ANOTHER plane is drawn at its place's projection onto the selected pad's, not at its raw travel") {
	ExploreFixture f;
	visage::Canvas canvas;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	f.selection.select(0);
	// G at A's zone but a nudged Offset (a neighbouring home, so another plane that is close by), a little way out.
	f.set(soundParam(6, SoundField::Zone), f.read(soundParam(0, SoundField::Zone)));
	f.set(soundParam(6, SoundField::Offset), f.read(soundParam(0, SoundField::Offset)) + 0.02);
	f.bridge.setSoundTravel(6, { 0.002, -0.001 });
	f.stage.draw(canvas);
	f.settleScrub();
	for (int frame = 0; frame < 64 && f.stage.update(); ++frame) {
		f.now += 16;
	}
	f.stage.draw(canvas);
	// THE RESTATEMENT: G's place decoded on G's own window, projected onto A's (terrain.h's public road, as a like's is).
	const TerrainWindow g = f.stage.terrainWindowFor(6);
	const TerrainWindow a = f.stage.terrainWindowNow();
	std::array<double, kTerrainLikePoint> point {};
	const size_t n = terrainPlacePoint(g, 0.002, -0.001, point);
	const PlanePlace place = terrainPlaceOn(a, std::span<const double>(point.data(), n), 0.002, -0.001);
	REQUIRE_FALSE(place.exact); // another plane, or the case says nothing the first did not
	const auto mark = markOf(f, 6);
	const visage::Point want = f.stage.glassAtWorld(place.y, place.x);
	const visage::Point raw = f.stage.glassAt(-0.001, 0.002);
	REQUIRE(mark.has_value());
	CHECK(mark->at.x == doctest::Approx(want.x).epsilon(1.0e-5));
	CHECK(mark->at.y == doctest::Approx(want.y).epsilon(1.0e-5));
	MESSAGE("projected " << place.x << ", " << place.y << " from raw 0.002, -0.001");
	CHECK(std::hypot(want.x - raw.x, want.y - raw.y) > 0.5f); // and that is not where its raw travel would put it
}

TEST_CASE("kit layer on the pictures: Kit Zone, Offset and Gamma move every pad's terrain window, the trims ride it, and the hit picture reads them") {
	ExploreFixture f;
	visage::Canvas canvas;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	const TerrainWindow before = f.stage.terrainWindowFor(2);
	CHECK(before.additive.drumKit == feathers::DrumBias {}); // neutral by default
	f.set(ParamId::DrumKitZone, 20.0);
	f.set(ParamId::DrumKitOffset, 0.1);
	f.set(ParamId::DrumKitGamma, 0.2);
	f.set(ParamId::DrumKitTone, 0.8);
	for (const size_t slot : { size_t { 0 }, size_t { 2 }, size_t { 7 } }) {
		CAPTURE(slot);
		const TerrainWindow w = f.stage.terrainWindowFor(slot);
		CHECK(w.zoneBaseline == std::clamp(f.read(soundParam(slot, SoundField::Zone)) + 20.0, 0.0, 1023.0));
		CHECK(w.offsetBaseline == doctest::Approx(f.read(soundParam(slot, SoundField::Offset)) + 0.3).epsilon(1.0e-12));
		CHECK(w.additive.drumKit.tone == 0.8f);
		CHECK(w.additive.drumKit.pitch == feathers::kFieldNeutral);
	}
	// PITCHED: no kit layer reaches the picture (it is a Kit's).
	f.set(ParamId::DrumMode, 0.0);
	CHECK(f.stage.terrainWindowFor(0).additive.drumKit == feathers::DrumBias {});
	CHECK(f.stage.terrainWindowFor(0).zoneBaseline == f.read(soundParam(0, SoundField::Zone)));
	// THE HIT PICTURE reads the pad through the bank's road, so a Kit Tone moves it.
	f.set(ParamId::DrumMode, 1.0);
	f.set(ParamId::DrumKitTone, 0.5);
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	const float neutralTone = panel.pictureForTest().lawsForTest().tone;
	f.set(ParamId::DrumKitTone, 0.95);
	(void)panel.update();
	CHECK(std::abs(panel.pictureForTest().lawsForTest().tone - neutralTone) > 1.0e-3);
}

TEST_CASE("kit layer on the surround: Kit mode offers three pages (Pad, Kit Fields, Kit Place); Pitched offers no page and no kit knob") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	using C = ui::ExploreStage::SurroundControl;
	const auto shown = [&f](C c) { return f.stage.seatedNowForTest(c); };
	CHECK(shown(C::DrumPage));
	CHECK(shown(C::DrumPitch));
	CHECK_FALSE(shown(C::DrumKitPitch));
	f.stage.chooseDrumPage(ui::ExploreStage::DrumPage::KitFields);
	for (const C c : { C::DrumKitPitch, C::DrumKitLength, C::DrumKitSweep, C::DrumKitTone, C::DrumKitNoise, C::DrumKitSnap,
	                   C::DrumKitMetal, C::DrumWalkX, C::DrumWalkY }) {
		CAPTURE(static_cast<int>(c));
		CHECK(shown(c));
	}
	CHECK_FALSE(shown(C::DrumPitch));
	f.stage.chooseDrumPage(ui::ExploreStage::DrumPage::KitPlace);
	for (const C c : { C::DrumKitGamma, C::DrumKitZone, C::DrumKitOffset, C::DrumWalkX, C::DrumWalkY }) {
		CAPTURE(static_cast<int>(c));
		CHECK(shown(c));
	}
	CHECK_FALSE(shown(C::DrumKitTone));
	// PITCHED: the Pad page whatever was chosen, no page box, no kit knob, no Walk.
	f.set(ParamId::DrumMode, 0.0);
	CHECK_FALSE(shown(C::DrumPage));
	CHECK_FALSE(shown(C::DrumKitGamma));
	CHECK_FALSE(shown(C::DrumWalkX));
	// EACH KIT KNOB DRIVES ITS PARAM: the tenth is Kit Metal.
	CHECK(f.stage.drumKitKnobForTest(9).control()->display().base == doctest::Approx(0.5));
}

TEST_CASE("kit layer on the terrain: a kit trim over a neutral pad scaler draws exactly what that scaler alone draws, and not the neutral map") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	TerrainWindow kitTrim = f.stage.terrainWindowFor(1);
	TerrainWindow padTrim = kitTrim;
	TerrainWindow neutral = kitTrim;
	kitTrim.additive.drum = {};
	kitTrim.additive.drumKit.tone = 0.2f;
	kitTrim.additive.drumKit.noise = 0.9f;
	padTrim.additive.drum = {};
	padTrim.additive.drum.tone = 0.2f;
	padTrim.additive.drum.noise = 0.9f;
	padTrim.additive.drumKit = {};
	neutral.additive.drum = {};
	neutral.additive.drumKit = {};
	bool differs = false;
	for (const double x : { -0.004, 0.0, 0.003 }) {
		for (const double y : { -0.002, 0.0015 }) {
			const auto a = terrainPartialsAt(kitTrim, x, y);
			const auto b = terrainPartialsAt(padTrim, x, y);
			const auto n = terrainPartialsAt(neutral, x, y);
			CHECK(a == b); // the law at neutral is the field to the bit, so these compose to the same fields exactly
			differs = differs || a != n;
		}
	}
	CHECK(differs); // and the trim did reach the picture
}

TEST_CASE("drum kit panel (audition bug b): the Decay and Drive handles are never confusable, so a Drive drag never writes Decay") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(feathers::drumParam(0, DrumControl::FieldSweep), 1.0);             // a real sweep, so the axis can be the sweep's
	f.set(feathers::drumParam(0, DrumControl::SweepTime), -1.0);             // the longest Sweep Time: the axis is long
	f.set(feathers::drumParam(0, DrumControl::Drive), 0.0);                  // Drive at its floor
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	const ParamDesc* decay = findParam(static_cast<clap_id>(feathers::drumParam(0, DrumControl::Decay)));
	int confusable = 0;
	for (int i = 0; i <= 200; ++i) {
		const double d = decay->min + (decay->max - decay->min) * static_cast<double>(i) / 200.0;
		f.set(feathers::drumParam(0, DrumControl::Decay), d);
		(void)panel.update();
		// A press on each dot takes that dot's own handle.
		confusable += p.handleUnder(p.handleAt(H::Decay)) != H::Decay ? 1 : 0;
		confusable += p.handleUnder(p.handleAt(H::Drive)) != H::Drive ? 1 : 0;
		confusable += std::hypot(p.handleAt(H::Decay).x - p.handleAt(H::Drive).x, p.handleAt(H::Decay).y - p.handleAt(H::Drive).y)
		                      <= 11.0f
		                  ? 1
		                  : 0;
	}
	CHECK(confusable == 0);
	// AND THE DRAG ITSELF: Drive up by a quarter of the envelope writes Drive and leaves Decay where it was, to the bit.
	f.set(feathers::drumParam(0, DrumControl::Decay), std::log2(0.04));
	(void)panel.update();
	const double decay0 = f.params.base(feathers::drumParam(0, DrumControl::Decay));
	const float env = p.height() - 8.0f - p.height() * 0.64f;
	p.dragForTest(H::Drive, 0.0f, -0.25f * env);
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Decay)) == decay0);
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Drive)) > 0.1);
}

TEST_CASE("drum kit panel (audition bugs a and c): the Decay handle reaches its whole range: past the plot's edge it keeps lengthening, and a long Decay still moves") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	const ParamDesc* decay = findParam(static_cast<clap_id>(feathers::drumParam(0, DrumControl::Decay)));
	// (c) FROM A SHORT DECAY, ONE LONG DRAG RIGHT (well past the plot) REACHES THE TOP OF ITS RANGE.
	f.set(feathers::drumParam(0, DrumControl::Decay), std::log2(0.08));
	(void)panel.update();
	p.dragForTest(H::Decay, 1200.0f, 0.0f);
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Decay)) == doctest::Approx(decay->max));
	// (a) A LONG DECAY IS NOT STUCK: its handle is inside the plot (not pinned to the edge), and a small drag right lengthens
	// it, a small drag left shortens it.
	f.set(feathers::drumParam(0, DrumControl::Decay), decay->max - 1.0);
	(void)panel.update();
	CHECK(p.handleAt(H::Decay).x < p.width() - 12.0f);
	const double before = f.params.base(feathers::drumParam(0, DrumControl::Decay));
	p.dragForTest(H::Decay, 20.0f, 0.0f);
	f.settleGesture();
	const double longer = f.params.base(feathers::drumParam(0, DrumControl::Decay));
	CHECK(longer > before);
	(void)panel.update();
	p.dragForTest(H::Decay, -20.0f, 0.0f);
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Decay)) < longer);
}

TEST_CASE("drum kit panel (audition): Level and Tune have explicit pills beside their handles, on every kind, writing the same params") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	using H = ui::HitPicture::Handle;
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		CAPTURE(k);
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		(void)panel.update();
		CHECK(panel.drawnForTest(0, DrumControl::Level));
		CHECK(panel.drawnForTest(0, DrumControl::Tune));
		CHECK(panel.pictureForTest().handleShown(H::Level)); // and the handles stay
		CHECK(panel.pictureForTest().handleShown(H::Tune));
	}
	// THE PILL WRITES THE PAD'S OWN PARAM, read back from the engine's side.
	auto* level = dynamic_cast<ui::FaderPill*>(panel.controlForTest(0, DrumControl::Level));
	auto* tune = dynamic_cast<ui::FaderPill*>(panel.controlForTest(0, DrumControl::Tune));
	REQUIRE(level != nullptr);
	REQUIRE(tune != nullptr);
	level->amountForTest().beginGesture();
	level->amountForTest().setValue(-9.0);
	level->amountForTest().endGesture();
	tune->amountForTest().beginGesture();
	tune->amountForTest().setValue(5.0);
	tune->amountForTest().endGesture();
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Level)) == doctest::Approx(-9.0));
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Tune)) == doctest::Approx(5.0));
}

TEST_CASE("drum kit panel (audition): a pad's hit pulses that pad alone at its velocity; each pad wears its sound's colour") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.updatePulses(0);
	feathers::PadHits hits;
	hits.count[5] = 2;
	hits.velocity[5] = 0.5f;
	f.bridge.publishPadHits(hits);
	CHECK(panel.updatePulses(10));
	CHECK(panel.pulseOf(5) == doctest::Approx(0.5f));
	CHECK(panel.pulseOf(4) == 0.0f);
	(void)panel.updatePulses(170);
	CHECK(panel.pulseOf(5) == 0.0f);
	// THE PAD'S COLOUR IS ITS SOUND'S, the hue the card's dot and the star wear (sound_hues: by the sound, not the row).
	for (size_t pad = 0; pad < feathers::kMaxSounds; ++pad) {
		CAPTURE(pad);
		CHECK(panel.padHue(pad) == ui::kSeedHues[feathers::hueIndexOf(f.bridge.soundHues(), pad)]);
	}
	// AND THE TITLE NAMES THE PAD'S LETTER AND KIND.
	f.selection.select(1);
	(void)panel.update();
	CHECK(panel.headerTitle() == std::string("Drum Kit \xc2\xb7 B \xc2\xb7 ") + feathers::kDrumKindNames[static_cast<size_t>(
	                                 steppedIndex(f.read(feathers::drumParam(1, DrumControl::Kind)), feathers::kDrumKindNames.size()))]);
}

TEST_CASE("drum kit panel (audition bug b, every kind): at the default pad no two handles a press could take stand within the hit slop") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		(void)panel.update();
		for (size_t a = 0; a < ui::HitPicture::kHandleCount; ++a) {
			const auto ha = static_cast<H>(a);
			if (!p.handleShown(ha)) {
				continue;
			}
			CAPTURE(k);
			CAPTURE(a);
			CHECK(p.handleUnder(p.handleAt(ha)) == ha); // a press on each dot takes that dot's own handle
			for (size_t b = a + 1; b < ui::HitPicture::kHandleCount; ++b) {
				const auto hb = static_cast<H>(b);
				if (!p.handleShown(hb)) {
					continue;
				}
				CAPTURE(b);
				const visage::Point pa = p.handleAt(ha);
				const visage::Point pb = p.handleAt(hb);
				CHECK(std::hypot(pa.x - pb.x, pa.y - pb.y) > 11.0f);
			}
		}
	}
}

TEST_CASE("drum kit panel (consolidation §14): Drive's across is its Tone, a Kick's Start Phase and a Clap's Spread, High Pass and Q are handles, by value") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	using H = ui::HitPicture::Handle;
	ui::HitPicture& p = panel.pictureForTest();
	const float plotW = p.width() - 20.0f;
	const float env = p.height() - 8.0f - p.height() * 0.64f;
	// DRIVE: across by a quarter of the plot moves its Tone by half (half the plot is the tone's whole travel).
	const double tone0 = f.params.base(feathers::drumParam(0, DrumControl::DriveTone));
	p.dragForTest(H::Drive, 0.25f * plotW, 0.0f);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::DriveTone)) - std::min(1.0, tone0 + 0.5)) < 1.0e-3);
	(void)panel.update();
	// A KICK'S START PHASE: up by a third of the envelope is 30 degrees.
	REQUIRE(p.handleShown(H::StartPhase));
	const double phase0 = f.params.base(feathers::drumParam(0, DrumControl::StartPhase));
	p.dragForTest(H::StartPhase, 0.0f, -env / 3.0f);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::StartPhase)) - (phase0 + 30.0)) < 1.0e-2);
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::ClapQ));
	// A CLAP: Spread across to where 1.5× it is drawn; the band's handle up an octave doubles its High Pass corner; across
	// 80 px doubles its Q.
	f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(feathers::drum::Kind::Clap));
	f.set(feathers::drumParam(0, DrumControl::ClapHighPass), std::log2(200.0));
	(void)panel.update();
	REQUIRE(p.handleShown(H::Spread));
	REQUIRE(p.handleShown(H::ClapBand));
	CHECK_FALSE(p.handleShown(H::StartPhase));
	const double spread0 = f.params.base(feathers::drumParam(0, DrumControl::Spread));
	p.dragForTest(H::Spread, p.xOf(1.5 * p.lawsForTest().spreadSeconds) - p.handleAt(H::Spread).x, 0.0f);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::Spread)) - (spread0 + std::log2(1.5))) < 1.0e-3);
	(void)panel.update();
	const float octave = (p.height() * 0.58f - 8.0f) / static_cast<float>(std::log2(1000.0));
	const double q0 = f.params.base(feathers::drumParam(0, DrumControl::ClapQ));
	p.dragForTest(H::ClapBand, 80.0f, -octave);
	f.settleGesture();
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::ClapHighPass)) - (std::log2(200.0) + 1.0)) < 1.0e-3);
	CHECK(std::abs(f.params.base(feathers::drumParam(0, DrumControl::ClapQ)) - std::min(8.0, 2.0 * q0)) < 1.0e-3);
}

TEST_CASE("drum kit panel (consolidation §14): the zone cells write their coordinates; Velocity Noise stays its own pill; the absorbed pills are gone") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(feathers::drum::Kind::Snare));
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	for (const DrumControl c : { DrumControl::VelocityZone, DrumControl::Wires, DrumControl::Bells, DrumControl::VelocityNoise }) {
		CAPTURE(static_cast<int>(c));
		CHECK(panel.drawnForTest(0, c));
	}
	auto* wires = dynamic_cast<ui::ZoneCoordCell*>(panel.controlForTest(0, DrumControl::Wires));
	REQUIRE(wires != nullptr);
	wires->box().control().beginGesture();
	wires->box().control().setValue(0.75); // Gated
	wires->box().control().endGesture();
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Wires)) == doctest::Approx(0.75));
	(void)panel.update();
	CHECK(panel.pictureForTest().lawsForTest().wireGate > 0.0); // and the picture reads the zone
}

TEST_CASE("drum kit panel (decision 8): a kit pad's Bend pill writes its own Bend, on every kind; Pitched mode does not draw it") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0); // Kit
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		CAPTURE(k);
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		(void)panel.update();
		CHECK(panel.drawnForTest(0, DrumControl::Bend));
	}
	auto* bend = dynamic_cast<ui::FaderPill*>(panel.controlForTest(0, DrumControl::Bend));
	REQUIRE(bend != nullptr);
	bend->amountForTest().beginGesture();
	bend->amountForTest().setValue(0.25);
	bend->amountForTest().endGesture();
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::Bend)) == doctest::Approx(0.25));
	// PITCHED MODE: the voice follows the key and the wheel whole, so a pad's share is not drawn.
	f.set(ParamId::DrumMode, 0.0);
	(void)panel.update();
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::Bend));
	CHECK(panel.drawnForTest(0, DrumControl::Pan)); // the row is still there
}

TEST_CASE("drum layer slot (stage (c)): dragging a pad carries its LOCKED layer with no write; UNLOCKED, the layer stays where it "
          "stands, the vector taking the drag back exactly") {
	for (const bool locked : { true, false }) {
		CAPTURE(locked);
		ExploreFixture f;
		f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
		f.set(ParamId::DrumMode, 1.0); // Kit
		const auto id = [](DrumControl c) { return static_cast<clap_id>(feathers::drumParam(1, c)); };
		f.set(static_cast<ParamId>(id(DrumControl::LayerKind)), 2.0);
		f.set(static_cast<ParamId>(id(DrumControl::LayerLock)), locked ? 1.0 : 0.0);
		f.set(static_cast<ParamId>(id(DrumControl::LayerOffsetX)), 0.2);
		f.set(static_cast<ParamId>(id(DrumControl::LayerOffsetY)), 0.1);
		f.selection.select(1);
		const AnchorPoint before = f.bridge.soundTravel().offset[1];
		const visage::Point centre = f.stage.disk().centre();
		f.stage.onMouseDown().callback(at(centre.x, centre.y));
		drain(f.bridge);
		f.now += 250;
		f.stage.onMouseDrag().callback(at(centre.x + 120.0f, centre.y));
		const AnchorPoint after = f.bridge.soundTravel().offset[1];
		const double dx = after.x - before.x;
		REQUIRE(std::abs(dx) > 1.0e-3); // the pad moved, so the claim is about a real drag
		const double ox = f.bridge.value(id(DrumControl::LayerOffsetX));
		const double oy = f.bridge.value(id(DrumControl::LayerOffsetY));
		if (locked) {
			CHECK(ox == 0.2); // no write: the layer's place is the pad's plus the same vector
			CHECK(oy == 0.1);
		}
		else {
			CHECK(std::abs((after.x + ox) - (before.x + 0.2)) < 1.0e-9); // where it stood
			CHECK(std::abs((after.y + oy) - (before.y + 0.1)) < 1.0e-9);
		}
	}
}

TEST_CASE("drum kit panel (layer slot, stage (c)): Kit mode draws the Layer box; its Level, Delay, High Pass, Lock and offset "
          "only while it is on; Pitched mode draws none; every one inside the panel") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	constexpr std::array<DrumControl, 6> kOn { DrumControl::LayerLevel, DrumControl::LayerDelay, DrumControl::LayerHighPass,
		                                       DrumControl::LayerLock, DrumControl::LayerOffsetX, DrumControl::LayerOffsetY };
	(void)panel.update();
	CHECK(panel.drawnForTest(0, DrumControl::LayerKind));
	for (const DrumControl c : kOn) {
		CAPTURE(static_cast<int>(c));
		CHECK_FALSE(panel.drawnForTest(0, c));
	}
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		CAPTURE(k);
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		f.set(feathers::drumParam(0, DrumControl::Trigger), 1.0); // Gate: the most pills
		f.set(feathers::drumParam(0, DrumControl::LayerKind), 3.0);
		(void)panel.update();
		for (const DrumControl c : kOn) {
			CAPTURE(static_cast<int>(c));
			CHECK(panel.drawnForTest(0, c));
			const visage::Frame* frame = panel.controlForTest(0, c);
			REQUIRE(frame != nullptr);
			CHECK(frame->bottom() <= panel.height() + 1.0e-3f);
			CHECK(frame->right() <= panel.width() + 1.0e-3f);
		}
	}
	// THE BOX WRITES THE KIND.
	auto* box = dynamic_cast<ui::ValueBox*>(panel.controlForTest(0, DrumControl::LayerKind));
	REQUIRE(box != nullptr);
	box->control().beginGesture();
	box->control().setValue(1.0);
	box->control().endGesture();
	f.settleGesture();
	CHECK(f.params.base(feathers::drumParam(0, DrumControl::LayerKind)) == doctest::Approx(1.0));
	f.set(ParamId::DrumMode, 0.0);
	(void)panel.update();
	CHECK_FALSE(panel.drawnForTest(0, DrumControl::LayerKind));
	for (const DrumControl c : kOn) {
		CHECK_FALSE(panel.drawnForTest(0, c));
	}
}

TEST_CASE("drum kit panel (layout): the pads are ONE row of eight, A to H; the panel is exactly that "
          "row wide and no wider; every pad control sits under the row") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y); // the pad row plus the pad fx rail
	(void)panel.update();
	const visage::Bounds first = panel.padBoundsForTest(0);
	for (size_t p = 1; p < feathers::kMaxSounds; ++p) {
		CAPTURE(p);
		const visage::Bounds b = panel.padBoundsForTest(p);
		CHECK(b.y() == first.y()); // one row
		CHECK(b.x() > panel.padBoundsForTest(p - 1).right()); // A to H, left to right
	}
	// THE WIDTH IS THE ROW'S: the same gutter either side, and the cap is the minimum.
	CHECK(panel.padBoundsForTest(feathers::kMaxSounds - 1).right() + first.x() == doctest::Approx(ui::DrumPanel::kMinimumWidth));
	CHECK(panel.maximumSize().x == ui::DrumPanel::kMinimumWidth + panel.laneRailForTest().railWidth()); // and the pad fx rail
	CHECK(panel.minimumSize().x == panel.maximumSize().x);
	// EVERY CONTROL UNDER THE ROW, on every kind, with a layer on (the most drawn).
	f.set(feathers::drumParam(0, DrumControl::LayerKind), 3.0);
	f.set(feathers::drumParam(0, DrumControl::Trigger), 1.0);
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		CAPTURE(k);
		f.set(feathers::drumParam(0, DrumControl::Kind), static_cast<double>(k));
		(void)panel.update();
		for (size_t c = 0; c < feathers::kDrumControlCount; ++c) {
			const visage::Frame* frame = panel.controlForTest(0, static_cast<DrumControl>(c));
			if (frame == nullptr || !frame->isVisible()) {
				continue;
			}
			CAPTURE(c);
			CHECK(frame->y() >= first.bottom());
			CHECK(frame->x() >= first.x() - 1.0e-3f);
			CHECK(frame->right() <= ui::DrumPanel::kMinimumWidth - first.x() + 1.0e-3f);
		}
	}
}

TEST_CASE("drum kit panel (pad fx rail, §17 P3): the rail hangs off the right of the pad row and follows the FOCUSED pad's lane; "
          "selecting another pad swaps it; a pad with none offers the phantom (+), whose press builds that pad's lane; a pad with "
          "a device wears the FX mark; Pitched mode has no rail") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Drum));
	f.set(ParamId::DrumMode, 1.0);
	feathers::ParamSet synthParams;
	auto synth = std::make_unique<feathers::Synth>(synthParams);
	synth->setCatalogue(feathers::hostCatalogue());
	synth->activate(48000.0, 512);
	f.bridge.setPadLaneSource([&synth](size_t pad, bool make) { return make ? synth->ensurePadLane(pad) : synth->padLane(pad); });
	feathers::rack::InnerChain* lane5 = synth->ensurePadLane(5);
	REQUIRE(lane5 != nullptr);
	f.selection.select(5);
	ui::DrumPanel panel(f.bridge, f.registry, f.selection, &f.tray);
	panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y);
	(void)panel.update();
	ui::FxLaneRail& rail = panel.laneRailForTest();
	REQUIRE(rail.isVisible());
	CHECK(rail.x() >= panel.padBoundsForTest(feathers::kMaxSounds - 1).right()); // off the right of the row
	CHECK(rail.right() == doctest::Approx(panel.width()));
	CHECK(rail.surfaceForTest() == lane5); // the focused pad's own chain
	CHECK_FALSE(rail.phantom());
	// ANOTHER PAD, WITH NO LANE: the rail swaps to it — nothing to follow, so the phantom.
	f.selection.select(2);
	(void)panel.update();
	CHECK(rail.surfaceForTest() == nullptr);
	CHECK(rail.phantom());
	CHECK(synth->padLane(2) == nullptr);
	// THE PHANTOM'S (+), on the open rail, builds pad 2's lane and the rail follows it (empty, so no mark).
	(void)rail.setCollapsed(false);
	(void)panel.update();
	const visage::Bounds plus = rail.phantomAddBounds();
	rail.onMouseDown().callback(at(plus.x() + 0.5f * plus.width(), plus.y() + 0.5f * plus.height()));
	(void)panel.update();
	REQUIRE(synth->padLane(2) != nullptr);
	CHECK(rail.surfaceForTest() == synth->padLane(2));
	// THE MARK: a device in pad 5's lane, and its tile says so; pad 2's does not.
	REQUIRE(lane5->bridge()->addDevice("busu") != 0u);
	CHECK(panel.padShowsFxForTest(5));
	CHECK_FALSE(panel.padShowsFxForTest(2));
	// BACK TO PAD 5: the rail follows its chain again.
	f.selection.select(5);
	(void)panel.update();
	CHECK(rail.surfaceForTest() == lane5);
	// PITCHED MODE: no rail, and the panel is the pad row's width alone.
	f.set(ParamId::DrumMode, 0.0);
	(void)panel.update();
	CHECK_FALSE(rail.isVisible());
	CHECK(panel.minimumSize().x == ui::DrumPanel::kMinimumWidth);
}

TEST_CASE("radius-mod-jump: a route into Radius moves along the knob's face — the engine, the mark and the ring agree by value") {
	// The symptom: modulating Radius on the main terrain showed a strange minimum delta, as if the mod jumped
	// right to 50%. It was not a step: the engine added depth x source x 2 in RADIUS units, and the knob's face is
	// exponential, so a 1% route from zero sat at 45% of the throw. The decision (option A): a route moves ALONG THE FACE.
	// So, BY VALUE, at several bases, depths and source values, unipolar and bipolar:
	//   the ENGINE (ParamSet's pull through `playedRadius`) plays radiusAt(clamp(face(base) + depth x source, 0, 1)),
	//   restated here independently — face(r) = ln(1 + r / 2 (e^8.4 - 1)) / 8.4;
	//   the knob's MARK (`display().modulated`, from the engine's published pull) is that radius, on that face;
	//   the knob's RING edges are the faces the engine plays at the source's two ends.
	// 1% depth is 1% of the face; a bipolar source at its bottom moves the face down by as much as its top moves it up.
	//
	// KNOWN-BAD (engine linear again): `playedRadius` returning `orbitRadius + facePull` — the engine checks go red.
	// KNOWN-BAD (UI and engine disagree): `TravelControl::routeFace` returning `faceAt(value() + offset)` — the ring
	// checks go red while the engine's stay green.
	const auto faceOf = [](double r) { return r > 0.0 ? std::log1p(r / 2.0 * std::expm1(8.4)) / 8.4 : 0.0; };
	const auto radiusAt = [](double p) { return 2.0 * std::expm1(8.4 * std::clamp(p, 0.0, 1.0)) / std::expm1(8.4); };
	const auto near = [](double a, double b, double tol) { return std::abs(a - b) <= tol; };

	ExploreFixture f;
	ModState& mod = f.mod;
	f.selection.select(2);
	f.stage.setModulation(&mod, nullptr);
	f.stage.update();
	Knob& knob = f.stage.radiusKnobForTest(2);
	const backplane::Destination destination = backplane::Destination::core(travelDestinationIndex(2, TravelAxis::Radius));
	const size_t index = travelInternalIndex(2, TravelAxis::Radius);
	mod.focus(macroSourceAt(0));
	mod.arm(true);

	for (const bool bipolar : { false, true }) {
		CAPTURE(bipolar);
		f.bridge.setMacroBipolar(0, bipolar);
		for (const double base : { 0.0, 0.004, 0.0296, 0.3, 1.9 }) {
			CAPTURE(base);
			f.bridge.setSoundOrbit(2, { base, 0.0 });
			for (const float depth : { 0.01f, 0.05f, 0.3f, -0.2f }) {
				CAPTURE(depth);
				ParamSet params;
				test::Routes routes;
				routes.set(test::macro(0), destination, depth);
				REQUIRE(routes.publish(params));
				f.bridge.setRouteAmount(macroSourceAt(0), destination, depth);
				const double baseFace = faceOf(base);
				for (const double v : { 0.0, 1e-4, 0.01, 0.25, 0.5, 0.75, 1.0 }) {
					CAPTURE(v);
					params.setBase(static_cast<clap_id>(macroParam(0)), v);
					const double source = bipolar ? 2.0 * v - 1.0 : v;
					// THE PULL: ParamSet's own for a unipolar macro. FOR A BIPOLAR ONE IT IS STATED, NOT READ: a core
					// route from a macro reads `macroReading` (0..1) and not the 2v - 1 cell, so ParamSet plays a
					// bipolar macro unipolar on EVERY core destination (reported separately; not this branch's to
					// change). What is under test here is the mapping a signed pull goes through.
					const double pull = bipolar ? static_cast<double>(depth) * source : params.internalOffset(index);
					const double wantFace = std::clamp(baseFace + static_cast<double>(depth) * source, 0.0, 1.0);
					// THE ENGINE, BY VALUE.
					const double played = playedRadius(base, pull);
					INFO("pull " << pull << " want " << static_cast<double>(depth) * source);
					CHECK(near(pull, static_cast<double>(depth) * source, 1e-6));
					CHECK(near(played, radiusAt(wantFace), 1e-9 + 1e-6 * radiusAt(wantFace)));
					// THE MARK: the engine's own pull, published as it is every block, read back through the knob.
					TravelPulls pulls;
					pulls.radius[2] = pull;
					f.bridge.publishTravelPulls(pulls);
					const double mark = knob.control()->display().modulated;
					CHECK(mark == played); // the SAME function on the SAME inputs: exact
					// (the knob shows no mark within 0.001 of its pointer — its own threshold for "moved")
					const Knob::ModRing shown = knob.modRingForTest();
					INFO("mark " << shown.mark << " shown " << shown.hasMark << ", want " << wantFace);
					CHECK(shown.hasMark == (std::abs(wantFace - baseFace) > 0.001 + 1e-6 || shown.hasMark));
					CHECK(near(shown.hasMark ? shown.mark : baseFace, wantFace, shown.hasMark ? 1e-5 : 0.001 + 1e-5));
				}
				// THE RING: its edges are where the engine plays the source's two ends — 0 and 1 unipolar, -1 and +1
				// bipolar — so a bipolar ring reaches down by as much as up (until the face's end holds it).
				const Knob::ModRing ring = knob.modRingForTest();
				REQUIRE(ring.drawn);
				const double upFace = std::clamp(baseFace + std::abs(static_cast<double>(depth)), 0.0, 1.0);
				const double downFace = std::clamp(baseFace - std::abs(static_cast<double>(depth)), 0.0, 1.0);
				const double hi = bipolar || depth > 0.0f ? upFace : baseFace;
				const double lo = bipolar || depth < 0.0f ? downFace : baseFace;
				CHECK(near(ring.hi, hi, 1e-5));
				CHECK(near(ring.lo, lo, 1e-5));
			}
		}
	}

	// 1% DEPTH IS 1% OF THE FACE, from the knob's zero — the case it was heard as a jump to 50%.
	f.bridge.setMacroBipolar(0, false);
	f.bridge.setSoundOrbit(2, { 0.0, 0.0 });
	f.bridge.setRouteAmount(macroSourceAt(0), destination, 0.01f);
	TravelPulls pulls;
	pulls.radius[2] = 0.01;
	f.bridge.publishTravelPulls(pulls);
	CHECK(near(knob.modRingForTest().mark, 0.01, 1e-6));
	CHECK(near(knob.modRingForTest().hi - knob.modRingForTest().lo, 0.01, 1e-6));
	// AND THE SOUND REALLY IS ONLY A LITTLE WAY OUT: 1% of the face is radiusAt(0.01), 0.00004 travel units — where the
	// radius-units route put it at 0.02, a third of a base travel.
	CHECK(playedRadius(0.0, 0.01) < 1e-4);
}

//--------------------------------------------------------------------------------------------------//
// A SWITCH'S RELAYOUT IS THE RELAYOUT A RESIZE DOES                              //
//--------------------------------------------------------------------------------------------------//
//
// Layout was wrong in the Additive engine (at least) after enabling Vocode, until the window was resized. The first
// Vocode on any sound puts the VOCODE ANALYSIS panel in the row; `sync` re-laid the row by calling `resized()` directly. The
// cells moved (they are frames, and `setBounds` repaints them) but the panel frames, which `SpectrumPanel::draw` paints at the
// `p.bounds` `layoutRow` computed, stayed where the last paint left them — until a resize, whose `setBounds` repaints the
// panel itself. The layout was right; the picture of it was not, so the instrument is the panel's own repaint request
// (headless: visage asks the frame's event handler), plus every child's bounds against a forced resize.

namespace {

struct PanelLaidOut {
	const visage::Frame* frame;
	visage::Bounds bounds;
	bool visible;
};

std::vector<PanelLaidOut> panelLayoutOf(const visage::Frame& root) {
	std::vector<PanelLaidOut> out;
	std::function<void(const visage::Frame&)> walk = [&](const visage::Frame& frame) {
		for (const visage::Frame* child : frame.children()) {
			out.push_back({ child, child->bounds(), child->isVisible() });
			walk(*child);
		}
	};
	walk(root);
	return out;
}

/// Where the switch's layout differs from a resize's at the same size: the shown frames' bounds and every frame's visibility.
std::string panelLayoutDiff(const std::vector<PanelLaidOut>& a, const std::vector<PanelLaidOut>& b) {
	if (a.size() != b.size()) {
		return " the tree changed size";
	}
	std::string out;
	for (size_t i = 0; i < a.size(); ++i) {
		if (a[i].visible != b[i].visible || (a[i].visible && !(a[i].bounds == b[i].bounds))) {
			char line[160];
			const auto& frameRef = *a[i].frame;
			std::snprintf(line, sizeof line, " [%s %.1f,%.1f %.1f wide -> %.1f,%.1f %.1f wide]", typeid(frameRef).name(), a[i].bounds.x(),
			              a[i].bounds.y(), a[i].bounds.width(), b[i].bounds.x(), b[i].bounds.y(), b[i].bounds.width());
			out += line;
		}
	}
	return out;
}

/// A shown direct child of the panel that moved: the panel paints its frames around those, so it must repaint itself.
bool panelChildMoved(const visage::Frame& panel, const std::vector<PanelLaidOut>& a, const std::vector<PanelLaidOut>& b) {
	for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
		if (a[i].frame->parent() == &panel && a[i].visible && b[i].visible && !(a[i].bounds == b[i].bounds)) {
			return true;
		}
	}
	return a.size() != b.size();
}

/// COUNTS THE REDRAWS THE PANEL ITSELF ASKS FOR (children share the handler and are not counted); the previous handler is put
/// back on destruction.
struct PanelRedraws {
	visage::Frame* target;
	visage::FrameEventHandler* previous;
	visage::FrameEventHandler handler;
	int count = 0;
	explicit PanelRedraws(visage::Frame* frame) : target(frame), previous(frame->eventHandler()) {
		handler.request_redraw = [this](visage::Frame* asked) {
			if (asked == target) {
				++count;
			}
		};
		target->setEventHandler(&handler);
	}
	~PanelRedraws() { target->setEventHandler(previous); }
	PanelRedraws(const PanelRedraws&) = delete;
	PanelRedraws& operator=(const PanelRedraws&) = delete;
};

void forcePanelResize(visage::Frame& frame) {
	const visage::Bounds b = frame.bounds();
	frame.setBounds(b.x(), b.y(), b.width() + 1.0f, b.height());
	frame.setBounds(b);
}

} // namespace

TEST_CASE("spectrum panel: Vocode on and off lays the row out as a resize does, and the panel repaints its own frames") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	const size_t selected = f.selection.slot();
	f.set(soundParam(selected, SoundField::Enable), 1.0);
	f.update();
	const size_t rowsOff = f.panel.rowsForTest();
	for (const double v : { 1.0, 0.0, 1.0, 0.0 }) {
		CAPTURE(v);
		const std::vector<PanelLaidOut> before = panelLayoutOf(f.panel);
		int redraws = 0;
		{
			PanelRedraws probe(&f.panel);
			f.set(drawerParam(selected, SpectralControl::VocodeOn), v);
			f.update();
			redraws = probe.count;
		}
		const std::vector<PanelLaidOut> toggled = panelLayoutOf(f.panel);
		// NOT VACUOUS: the analysis panel really comes and goes, and its cells with it.
		ui::VocodeAnalysisColumn* column = f.panel.vocodeColumnForTest(selected);
		REQUIRE(column != nullptr);
		CHECK(column->isVisible() == (v != 0.0));
		CHECK(panelChildMoved(f.panel, before, toggled));
		// THE PANEL ASKED FOR ITS OWN REPAINT (the frames it draws behind the cells moved with them).
		CHECK(redraws > 0);
		// AND WHAT IT LAID OUT IS WHAT A RESIZE LAYS OUT.
		forcePanelResize(f.panel);
		CHECK(panelLayoutDiff(toggled, panelLayoutOf(f.panel)) == "");
	}
	CHECK(f.panel.rowsForTest() == rowsOff);
}

TEST_CASE("spectrum panel: every switch, stepped control and filter tab, at three widths, lays out as a resize does and repaints the panel when it moves the row") {
	SpectrumFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	const size_t selected = f.selection.slot();
	const size_t other = (selected + 1) % feathers::kMaxSounds;
	f.set(soundParam(selected, SoundField::Enable), 1.0);
	f.update();
	struct Move {
		std::string name;
		std::function<void(double)> apply;
		std::vector<double> values;
	};
	std::vector<Move> moves;
	const auto stepped = [&](ParamId id, const std::string& name) {
		const ParamDesc* d = findParam(static_cast<clap_id>(id));
		REQUIRE(d != nullptr);
		std::vector<double> values;
		for (double x = d->min; x <= d->max; x += 1.0) {
			values.push_back(x);
		}
		values.push_back(d->def);
		moves.push_back({ name, [&f, id](double x) { f.set(id, x); }, values });
	};
	for (const SpectralControl c : kSpectralSteppedControls) {
		stepped(drawerParam(selected, c), "this sound's control " + std::to_string(static_cast<int>(c)));
	}
	// ANOTHER SOUND'S VOCODE: the engine's analysis panel comes for any sound's switch.
	stepped(drawerParam(other, SpectralControl::VocodeOn), "another sound's Vocode");
	stepped(ParamId::AdditivePhases, "Phases");
	// THE FILTER TABS (UI state): the row closes over the other filter's panel.
	moves.push_back({ "the filter tab", [&f, selected](double x) { f.panel.filterTabsForTest(selected, f.panel.filterTabForTest()).select(static_cast<int>(x)); }, { 1.0, 0.0 } });
	size_t moved = 0;
	size_t checked = 0;
	for (const float width : { ui::SpectrumPanel::kMinimumWidth, 1200.0f, 560.0f }) {
		f.panel.setBounds(0.0f, 0.0f, width, ui::SpectrumPanel::kMinimumHeight);
		f.update();
		for (const Move& move : moves) {
			for (const double value : move.values) {
				CAPTURE(width);
				CAPTURE(move.name);
				CAPTURE(value);
				const std::vector<PanelLaidOut> before = panelLayoutOf(f.panel);
				int redraws = 0;
				{
					PanelRedraws probe(&f.panel);
					move.apply(value);
					f.update();
					f.update();
					redraws = probe.count;
				}
				const std::vector<PanelLaidOut> after = panelLayoutOf(f.panel);
				if (panelChildMoved(f.panel, before, after)) {
					++moved;
					CHECK(redraws > 0);
				}
				forcePanelResize(f.panel);
				CHECK(panelLayoutDiff(after, panelLayoutOf(f.panel)) == "");
				++checked;
			}
		}
	}
	MESSAGE("moves: " << checked << ", of which re-laid the row: " << moved);
	// NOT VACUOUS: Vocode (this sound's and another's) and the tab re-lay the row at every width.
	CHECK(moved >= 9);
}

TEST_CASE("SHOT the spectrum panel after Vocode is switched on and off, against the same after a resize") {
	// NOT A CASE — A CAMERA, gated on FEATHERS_SHOT_DIR. Run under `scripts/venue-xvfb.sh` with FEATHERS_REDUCED_MOTION=1. No
	// `redrawAll` after the first frame, so only what the panel itself asks to repaint is repainted, as in the editor. Writes
	// vocode_0_before, vocode_1_on, vocode_2_on_resized, vocode_3_off, vocode_4_off_resized; a fixed build has 1 == 2 and 3 == 4
	// == 0 byte for byte.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	SpectrumFixture f;
	constexpr int w = 1240;
	constexpr int h = 700;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	editor.addChild(&f.panel);
	f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
	f.set(ParamId::AEnable, 1.0);
	f.panel.setBounds(20.0f, 30.0f, 1200.0f, 640.0f);
	const auto frames = [&] {
		for (int i = 0; i < 20; ++i) {
			f.update();
			editor.drawWindow();
		}
	};
	const auto shoot = [&](const char* name) {
		const std::string path = std::string(dir) + "/" + name + ".png";
		editor.takeScreenshot().save(path);
		MESSAGE("wrote " << path);
	};
	editor.redrawAll();
	frames();
	shoot("vocode_0_before");
	f.set(drawerParam(f.selection.slot(), SpectralControl::VocodeOn), 1.0);
	frames();
	shoot("vocode_1_on");
	forcePanelResize(f.panel);
	frames();
	shoot("vocode_2_on_resized");
	f.set(drawerParam(f.selection.slot(), SpectralControl::VocodeOn), 0.0);
	frames();
	shoot("vocode_3_off");
	forcePanelResize(f.panel);
	frames();
	shoot("vocode_4_off_resized");
	editor.removeChild(&f.panel);
}

namespace {

/// A 16x16 BGRA ramp: red = 16 x column, green = 16 x row, blue 0, opaque — so the colour at a pixel says, by value, which
/// texel coordinate was sampled there, on each axis separately.
std::vector<uint32_t> rampTexels() {
	std::vector<uint32_t> ramp(16 * 16);
	for (uint32_t row = 0; row < 16; ++row) {
		for (uint32_t col = 0; col < 16; ++col) {
			ramp[row * 16 + col] = 0xff000000u | ((col * 16u) << 16) | ((row * 16u) << 8);
		}
	}
	return ramp;
}

/// Bilinear of a BGRA texel buffer at texel coordinate (u, v), texel CENTRES at integer + 0.5 — one channel, by shift.
double bilinear(const uint32_t* texels, int size, double u, double v, int shift) {
	const double x = std::clamp(u - 0.5, 0.0, static_cast<double>(size - 1));
	const double y = std::clamp(v - 0.5, 0.0, static_cast<double>(size - 1));
	const int x0 = static_cast<int>(std::floor(x));
	const int y0 = static_cast<int>(std::floor(y));
	const int x1 = std::min(x0 + 1, size - 1);
	const int y1 = std::min(y0 + 1, size - 1);
	const double fx = x - x0;
	const double fy = y - y0;
	const auto at = [&](int cx, int cy) { return static_cast<double>((texels[cy * size + cx] >> shift) & 0xffu); };
	return (at(x0, y0) * (1.0 - fx) + at(x1, y0) * fx) * (1.0 - fy) + (at(x0, y1) * (1.0 - fx) + at(x1, y1) * fx) * fy;
}

} // namespace

TEST_CASE("terrain-render-bugs: a stretched raw image that is clipped samples its own texels at its own scale, per axis, "
          "and never a neighbour in the atlas") {
	// THE TERRAIN IS A 128-TEXEL PICTURE STRETCHED ACROSS A STAGE (`Canvas::imageRaw`), and on a big or maximised window,
	// with rails collapsed, or while a zoom is in flight, it overhangs the stage and is CLIPPED. visage's image vertex
	// shader moved the texture coordinate by the PIXELS the clip removed — right only for an image drawn at its own size
	// — so the visible part showed the whole picture squashed, and read past the image's atlas sub-rect into its
	// neighbours (the header's mark, tiled into the terrain on a Mac). The fork now shifts by texels per pixel per axis
	// and clamps to the sub-rect (owlet-labs/visage 585b1e0b, vs_image/fs_image).
	//
	// BY VALUE: a 16x16 ramp (red = 16 x column, green = 16 x row) drawn at several per-axis scales and clipped by
	// several amounts off the canvas's top-left; at every visible pixel the colour is the bilinear restatement of the
	// texel under it, and blue — a solid-blue image in the same atlas, drawn off the canvas — is never seen.
	// KNOWN-BAD: vs_image's `delta * a_texcoord0.zw` back to `delta` (the old shift in pixels) — the ramp checks go red.
	const std::vector<uint32_t> ramp = rampTexels();
	const std::vector<uint32_t> blue(16, 0xff0000ffu);
	constexpr int kW = 160;
	constexpr int kH = 120;
	struct Case {
		float clipX, clipY, scaleX, scaleY;
	};
	for (const Case c : { Case { 0, 0, 13, 13 }, Case { 40, 0, 13, 13 }, Case { 0, 40, 13, 13 }, Case { 57, 93, 13, 13 },
	                      Case { 57, 93, 20, 9 }, Case { 31, 17, 9, 20 }, Case { 120, 5, 17.5f, 11 } }) {
		CAPTURE(c.clipX);
		CAPTURE(c.clipY);
		CAPTURE(c.scaleX);
		CAPTURE(c.scaleY);
		visage::Canvas canvas;
		markedWindowless(canvas, kW, kH);
		visage::Region region;
		region.setBounds(0, 0, kW, kH);
		canvas.addRegion(&region);
		canvas.beginRegion(&region);
		canvas.setColor(0xff000000u);
		canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
		canvas.setColor(0xffffffffu);
		canvas.imageRaw(reinterpret_cast<const unsigned char*>(blue.data()), 4, 4, visage::ImageFormat::BGRA8, -60.0f,
		                -60.0f, 4.0f, 4.0f);
		canvas.imageRaw(reinterpret_cast<const unsigned char*>(ramp.data()), 16, 16, visage::ImageFormat::BGRA8, -c.clipX,
		                -c.clipY, 16.0f * c.scaleX, 16.0f * c.scaleY);
		canvas.endRegion();
		const visage::Screenshot shot = takeMarkedCapture(canvas, kW, kH);
		int checked = 0;
		int wrong = 0;
		int blueSeen = 0;
		for (int py = 0; py < kH; py += 3) {
			for (int px = 0; px < kW; px += 3) {
				const double x = px + 0.5 + c.clipX;
				const double y = py + 0.5 + c.clipY;
				if (x >= 16.0 * c.scaleX - 1.0 || y >= 16.0 * c.scaleY - 1.0) {
					continue; // past the picture's far edge, or on it
				}
				const visage::Color got = shot.sample(px, py);
				const double wantR = bilinear(ramp.data(), 16, x / c.scaleX, y / c.scaleY, 16);
				const double wantG = bilinear(ramp.data(), 16, x / c.scaleX, y / c.scaleY, 8);
				++checked;
				if (std::abs(got.hexRed() - wantR) > 4.0 || std::abs(got.hexGreen() - wantG) > 4.0) {
					if (wrong++ < 3) {
						MESSAGE("at " << px << "," << py << ": got " << got.hexRed() << "," << got.hexGreen() << " want "
						              << wantR << "," << wantG);
					}
				}
				blueSeen += got.hexBlue() > 4 ? 1 : 0;
			}
		}
		CHECK(checked > 100); // the picture covers most of the canvas: not vacuous
		CHECK(wrong == 0);
		CHECK(blueSeen == 0);
	}
}

TEST_CASE("terrain-render-bugs: the terrain is drawn at its logical shape at every stage size — the rail-collapse and "
          "one-jump-resize sequences, at dpi 1 and 2") {
	// THE SIZES THE STAGE TAKES, measured with `widget-shot --collapse-rails` and `--resize-to` on xvfb: the default
	// editor's stage with 0, 1, 2 and 3 rails collapsed (it overhangs from the second), a 3440x1440 and a 5120x2880 editor
	// (one jump or forty steps land at the same stage), and at dpi 2 the Retina shapes of the same. Wherever the
	// picture overhangs the stage it is clipped, which is where it used to squash and wrap into the atlas.
	//
	// BY VALUE: the ground (`drawGround`, the terrain picture alone, no planet light) captured at native size, and at a
	// grid of pixels whose four nearest texels are opaque, the colour is the bilinear restatement of the snapshot's own
	// texels at the place `terrainPlacement` puts them — width and height each by their own cell. The placement is
	// square (the logical shape), so a picture drawn squashed fails at every point off its centre row.
	// KNOWN-BAD: vs_image's clip shift back in pixels — red at every overhanging size, green at the two that fit.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.set(ParamId::AOffset, 0.5);
	f.selection.select(0);
	struct Size {
		float w, h, scale;
	};
	for (const Size s : { Size { 374, 539, 1 }, Size { 508, 539, 1 }, Size { 710, 539, 1 }, Size { 932, 539, 1 },
	                      Size { 2574, 1245, 1 }, Size { 4254, 2685, 1 }, Size { 710, 539, 2 }, Size { 1287, 622, 2 },
	                      Size { 2127, 1342, 2 } }) {
		CAPTURE(s.w);
		CAPTURE(s.h);
		CAPTURE(s.scale);
		f.stage.setBounds(0.0f, 0.0f, s.w, s.h);
		visage::Canvas scratch;
		for (int spin = 0; spin < 800; ++spin) {
			f.stage.draw(scratch);
			const auto snap = f.terrain.snapshot();
			if (snap->rows == TerrainMap::kResolution && snap->window == f.stage.askForTerrainForTest()) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		const std::shared_ptr<const TerrainSnapshot> snap = f.terrain.snapshot();
		REQUIRE(snap->rows == TerrainMap::kResolution);
		const visage::Bounds where = f.stage.terrainPlacement();
		CHECK(where.width() == doctest::Approx(where.height()).epsilon(1e-4)); // the logical shape: square
		const int nw = static_cast<int>(std::lround(s.w * s.scale));
		const int nh = static_cast<int>(std::lround(s.h * s.scale));
		visage::Canvas canvas;
		markedWindowless(canvas, nw, nh);
		canvas.setDpiScale(s.scale);
		visage::Region region;
		region.setBounds(0, 0, nw, nh);
		canvas.addRegion(&region);
		canvas.beginRegion(&region);
		f.stage.drawGround(canvas);
		canvas.endRegion();
		const visage::Screenshot shot = takeMarkedCapture(canvas, nw, nh);
		const bool overhangs = where.x() < 0.0f || where.y() < 0.0f || where.right() > s.w || where.bottom() > s.h;
		CAPTURE(overhangs);
		const double cellX = where.width() / TerrainMap::kResolution;
		const double cellY = where.height() / TerrainMap::kResolution;
		const uint32_t* texels = snap->texels.data();
		int checked = 0;
		int wrong = 0;
		const int step = std::max(3, nw / 60);
		for (int py = step / 2; py < nh; py += step) {
			for (int px = step / 2; px < nw; px += step) {
				const double lx = (px + 0.5) / s.scale;
				const double ly = (py + 0.5) / s.scale;
				const double u = (lx - where.x()) / cellX;
				const double v = (ly - where.y()) / cellY;
				if (u < 1.0 || v < 1.0 || u > TerrainMap::kResolution - 1.0 || v > TerrainMap::kResolution - 1.0) {
					continue;
				}
				const int x0 = static_cast<int>(std::floor(u - 0.5));
				const int y0 = static_cast<int>(std::floor(v - 0.5));
				bool opaque = true;
				for (int dy = 0; dy < 2; ++dy) {
					for (int dx = 0; dx < 2; ++dx) {
						opaque = opaque && (texels[(y0 + dy) * TerrainMap::kResolution + x0 + dx] >> 24) == 0xffu;
					}
				}
				if (!opaque) {
					continue;
				}
				const visage::Color got = shot.sample(px, py);
				const double r = bilinear(texels, TerrainMap::kResolution, u, v, 16);
				const double g = bilinear(texels, TerrainMap::kResolution, u, v, 8);
				const double b = bilinear(texels, TerrainMap::kResolution, u, v, 0);
				++checked;
				if (std::abs(got.hexRed() - r) > 6.0 || std::abs(got.hexGreen() - g) > 6.0 || std::abs(got.hexBlue() - b) > 6.0) {
					if (wrong++ < 3) {
						MESSAGE("at " << px << "," << py << ": got " << got.hexRed() << "," << got.hexGreen() << ","
						              << got.hexBlue() << " want " << r << "," << g << "," << b);
					}
				}
			}
		}
		CHECK(checked > 200);
		CHECK(wrong == 0);
	}
}

TEST_CASE("terrain-render-bugs: the terrain is uploaded to the atlas once per new picture, not once per frame") {
	// `Canvas::imageRaw` re-uploaded (and swizzled) the whole buffer on every draw. The stage now stamps a serial where
	// it copies a snapshot in (`freshPixelSerial`), and the atlas uploads only when the serial moves.
	// BY VALUE: ten frames of a settled terrain add no uploads; a new map (a zone move) adds exactly one.
	// KNOWN-BAD: drop the serial from the `imageRaw` call in `drawTerrainPicture` (serial 0, upload every draw) — the
	// first check reads 10, not 0.
	ExploreFixture f;
	f.set(ParamId::AZone, 512.0);
	f.selection.select(0);
	visage::Canvas scratch;
	const auto settle = [&](const std::shared_ptr<const TerrainSnapshot>& old) {
		for (int spin = 0; spin < 800; ++spin) {
			f.stage.draw(scratch);
			const auto snap = f.terrain.snapshot();
			if (snap != old && snap->rows == TerrainMap::kResolution && snap->window == f.stage.askForTerrainForTest()) {
				return snap;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(4));
		}
		return f.terrain.snapshot();
	};
	std::shared_ptr<const TerrainSnapshot> settled = settle(nullptr);
	REQUIRE(settled->rows == TerrainMap::kResolution);
	visage::Canvas canvas;
	canvas.setWindowless(866, 534);
	visage::Region region;
	region.setBounds(0, 0, 866, 534);
	canvas.addRegion(&region);
	const auto frame = [&] {
		canvas.beginRegion(&region);
		f.stage.drawGround(canvas);
		canvas.endRegion();
		canvas.submit();
	};
	frame();
	frame(); // the atlas's texture exists from the first submit; the second draw is its first upload road
	const uint64_t before = canvas.imageAtlas()->rawUploads();
	for (int i = 0; i < 10; ++i) {
		frame();
	}
	CHECK(canvas.imageAtlas()->rawUploads() - before == 0);

	// A NEW MAP: the zone moves, the terrain rebuilds, the stage copies the new picture in once.
	f.set(ParamId::AZone, 700.0);
	const std::shared_ptr<const TerrainSnapshot> rebuilt = settle(settled);
	REQUIRE(rebuilt != settled);
	REQUIRE(rebuilt->rows == TerrainMap::kResolution);
	const uint64_t beforeRebuild = canvas.imageAtlas()->rawUploads();
	for (int i = 0; i < 5; ++i) {
		frame();
	}
	CHECK(canvas.imageAtlas()->rawUploads() - beforeRebuild == 1);
}
