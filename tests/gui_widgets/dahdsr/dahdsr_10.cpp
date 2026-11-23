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

// PIECE 10 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

// BOTH SIDES APPENDED AT THE END OF THIS FILE AND BOTH ARE KEPT. The OWLTT strip cases
// below and the Waypoint cases after them are unrelated work that landed the same
// evening; the conflict is adjacency, not disagreement.
// =================================================================================================
// THE RATIO STRIP'S DEAD HALF (2026-08-26)
// =================================================================================================

TEST_CASE("a OWLTT strip puts its value where the pointer is, downward as well as up") {
	// The defect: the per-band ratio bars in the pop-out graph did not respond.
	//
	// THEY RESPOND UPWARD AND NOT DOWNWARD, which is why every case already here passed. `Handle`'s
	// four rings all sit ON their value, so every one of these cases grabbed a ring and pulled it UP
	// -- the ring test, the level-strip test, the panel test, the real-tree test. MEASURED before the
	// fix: an upward drag on the ratio strip landed on 0.750 against 0.750 wanted, and the downward
	// drag on the SAME strip in the SAME run landed on 0.500 against 0.250 wanted, having written
	// nothing at all. A control that works in one direction is indistinguishable from a working one
	// to a check that only pulls that way.
	//
	// WHY RATIO AND NOT THE OTHER THREE, which is the whole mechanism. `owltt.cpp` uses
	// `effectiveBandOf`, and a band's stored value is clamped to 0..1 after the global's shift is
	// added -- so the reachable effective range is `[max(0, shift), min(1, 1 + shift)]` and the strip
	// is drawn over the whole of 0..1. Threshold and bandwidth shift by `global - 0.5`, which is ZERO
	// at their declared defaults. Ratio shifted by the global ITSELF, whose declared default is 0.5.
	// So at the state the editor opens in, ratio was the one handle with half its travel unreachable,
	// and the half it lost was the bottom one -- the direction a hand goes to take compression OFF.
	//
	// ASSERTED AS A VALUE, NOT AS A DIFFERENCE. `wanted` is computed from the plot rectangle by an
	// independent restatement of what a strip means -- the fraction of `barArea()` the pointer stands
	// at -- rather than by asking the drag what it did. A `!=` check passes for a press that landed on
	// the neighbouring strip, and the two are fifteen pixels apart.
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);

	// THE STATE THE EDITOR OPENS IN, asserted rather than assumed: this whole case is about what the
	// SHIPPED default of the global does to the strip beside it, so a default that had quietly moved
	// would make it test nothing.
	REQUIRE(f.bridge.fxChain().device[1].row.field[static_cast<size_t>(fx::owltt::Param::Ratio)]
	        == doctest::Approx(0.5f));

	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();

	auto* lane = f.lane;
	lane->resized();
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);

	// BOTH SURFACES, because the report named the panel and the mechanism is in the shared class.
	// MEASURED: the card's ratio strip was dead in exactly the same way and by exactly the same
	// number, so "the panel's is broken" was a true sentence about a bug that is not the panel's.
	struct Surface { ui::OwlttGraph* graph; const char* name; };
	const std::array<Surface, 2> surfaces { {
	    { panel.graph(), "panel" },
	    { card->graph(), "card" },
	} };

	using Handle = ui::OwlttGraph::Handle;
	struct Strip { Handle handle; std::array<fx::owltt::Param, 3> param; const char* name; };
	// THE TWO STRIPS TOGETHER, so the ratio's claim is made beside a control that already worked --
	// which is what makes a failure attributable to the shift rather than to the venue.
	const std::array<Strip, 2> strips { {
	    { Handle::RatioStrip,
	      { fx::owltt::Param::LowRatio, fx::owltt::Param::MidRatio, fx::owltt::Param::HighRatio },
	      "ratio" },
	    { Handle::LevelStrip,
	      { fx::owltt::Param::LowLevel, fx::owltt::Param::MidLevel, fx::owltt::Param::HighLevel },
	      "level" },
	} };

	for (const Surface& surface : surfaces) {
		CAPTURE(std::string(surface.name));
		ui::OwlttGraph* graph = surface.graph;
		REQUIRE(graph != nullptr);
		REQUIRE(graph->width() > 0.0f);

		for (const Strip& strip : strips) {
			CAPTURE(std::string(strip.name));
			for (size_t band = 0; band < 3; ++band) {
				CAPTURE(band);
				// THE OTHER THREE CONTROLS OF THIS BAND, READ BEFORE THE PRESS. The four handles are
				// close together and the two strips share a column, so "something moved" is satisfied
				// by a press that landed one strip over.
				const auto other = [&](fx::owltt::Param p) {
					return f.bridge.fxChain().device[1].row.field[static_cast<size_t>(p)];
				};
				static constexpr fx::owltt::Param kNeighbours[3][2] = {
					{ fx::owltt::Param::LowThreshold, fx::owltt::Param::LowBandwidth },
					{ fx::owltt::Param::MidThreshold, fx::owltt::Param::MidBandwidth },
					{ fx::owltt::Param::HighThreshold, fx::owltt::Param::HighBandwidth },
				};
				const float neighbourA = other(kNeighbours[band][0]);
				const float neighbourB = other(kNeighbours[band][1]);
				// THE OTHER STRIP IN THIS BAND — the one fifteen pixels away that a press aimed
				// at this one could land on instead. The strips share a column and the hit test
				// splits it down the middle, so this is the assertion that separates "the ratio
				// moved" from "something in the ratio's column moved".
				static constexpr fx::owltt::Param kRatioOf[3] = { fx::owltt::Param::LowRatio,
					                                             fx::owltt::Param::MidRatio,
					                                             fx::owltt::Param::HighRatio };
				static constexpr fx::owltt::Param kLevelOf[3] = { fx::owltt::Param::LowLevel,
					                                             fx::owltt::Param::MidLevel,
					                                             fx::owltt::Param::HighLevel };
				const size_t sibling = static_cast<size_t>(
				    strip.handle == Handle::RatioStrip ? kLevelOf[band] : kRatioOf[band]);
				const float siblingBefore = f.bridge.fxChain().device[1].row.field[sibling];

				// BOTH ENDS OF THE DRAWN TRAVEL, AS ABSOLUTE TARGETS, and that is not a detail.
				// The first draft of this case pulled a QUARTER OF THE PLOT from wherever the ring
				// happened to sit — and a known-bad walked straight through it: uncentring the shift
				// again moved the ring to the TOP of the strip, where a quarter down is still inside
				// the reachable range, so the drag round-tripped and the case reported SUCCESS for a
				// strip with half its travel missing. A relative nudge asks "does this control move",
				// which is exactly the question that is satisfied everywhere except where it matters.
				// The claim is that the WHOLE drawn strip answers, so the case aims at its floor and
				// at its ceiling and does not care where the handle started.
				const visage::Bounds bars = graph->barArea();
				const visage::Point ring = graph->ringCentre(band, strip.handle);
				// THE PRESS RESOLVES TO THIS STRIP AND NOT ITS NEIGHBOUR — asked of the hit test
				// before anything is dragged, so a geometry drift fails here and says so.
				REQUIRE(graph->hitTestThreshold(ring) != ui::OwlttGraph::ThresholdHit::kNone);

				// THE FLOOR FIRST, which is the direction the bug was in: down is where a hand goes
				// to take compression off, and down is what wrote nothing.
				const float floorY = bars.bottom() - ui::OwlttGraph::kRingRadius;
				const float ceilY = bars.y() + ui::OwlttGraph::kRingRadius;
				for (const float toY : { floorY, ceilY }) {
					CAPTURE(toY);
					// AN INDEPENDENT RESTATEMENT of what a strip means: the fraction of the bar area
					// the pointer stands at, measured from its floor. Not the drag's own arithmetic.
					const float wanted =
					    std::clamp((bars.bottom() - toY) / bars.height(), 0.0f, 1.0f);
					const visage::Point grab = graph->ringCentre(band, strip.handle);
					graph->onMouseDown().callback(at(grab.x, grab.y));
					graph->onMouseDrag().callback(at(grab.x, toY));
					graph->onMouseUp().callback(at(grab.x, toY));

					const fx::owltt::EffectiveBand eff = graph->effective(band);
					const float landed = strip.handle == Handle::RatioStrip ? eff.ratio : eff.level;
					CAPTURE(landed);
					CAPTURE(wanted);
					CHECK(landed == doctest::Approx(wanted).epsilon(0.02));

					// AND THE HANDLE FOLLOWED, which is the half a value assertion alone does not
					// make: a strip whose ring stays where it was is unusable however right the
					// number is.
					CHECK(graph->ringCentre(band, strip.handle).y
					      == doctest::Approx(toY).epsilon(0.05));
				}

				// AND NOTHING ELSE IN THE BAND MOVED. A press fifteen pixels to the right lands on
				// the other strip and passes every check above except this one.
				CHECK(f.bridge.fxChain().device[1].row.field[sibling]
				      == doctest::Approx(siblingBefore));
				CHECK(other(kNeighbours[band][0]) == doctest::Approx(neighbourA));
				CHECK(other(kNeighbours[band][1]) == doctest::Approx(neighbourB));
			}
		}
	}
}
TEST_CASE("the panel's ratio strip answers a press that travelled the real tree") {
	// THE OTHER HALF OF THE CASE ABOVE, and the half this file has been caught missing twice.
	// `graph->onMouseDown()` calls the callback; it does not ask the window which frame a press at
	// that pixel would REACH. The defect was in the POPOUT specifically, so the routing claim
	// is made on the popout, at the strip's own drawn position, through `frameAtPoint` from the root.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(1);

	auto* panel = findInTree<ui::OwlttExpanded>(f.root);
	REQUIRE(panel != nullptr);
	auto* graph = panel->graph();
	REQUIRE(graph != nullptr);
	REQUIRE(graph->width() > 0.0f);

	const visage::Bounds bars = graph->barArea();
	const visage::Point ring = graph->ringCentre(0, ui::OwlttGraph::Handle::RatioStrip);
	const visage::Point inWindow = graph->positionInWindow() + ring;
	CHECK(f.root->frameAtPoint(inWindow - f.root->positionInWindow())
	      == static_cast<visage::Frame*>(graph));

	// DOWN TO THE STRIP'S FLOOR, which is the direction that wrote nothing — and as an ABSOLUTE
	// target rather than a nudge, for the reason the case above records: a relative pull lands wherever
	// the handle already was, and a handle whose reachable range has been halved is still free to move
	// within what is left of it.
	const float floorY = bars.bottom() - ui::OwlttGraph::kRingRadius;
	const float wanted = std::clamp((bars.bottom() - floorY) / bars.height(), 0.0f, 1.0f);
	dragThroughRoot(f.root, inWindow, inWindow + visage::Point { 0.0f, floorY - ring.y });
	CAPTURE(wanted);
	CHECK(graph->effective(0).ratio == doctest::Approx(wanted).epsilon(0.02));

	// AND THE PICTURE WAS TOLD. A repaint is unobservable headlessly, so what is asserted is the
	// count the notification itself reports — and the panel's content is on the state channel in its
	// own right (`ExpandedOverlay::show`), so the number has to be non-zero for a drag on a surface
	// that is not a child of anything else already watched.
	const Gui::TickReport report = f.gui.tickForTest();
	CAPTURE(report.stateFrames);
	CHECK(report.stateFrames > 0);
	// AND NOT MERELY BECAUSE THE BIT IS ALWAYS SET: an idle frame tells nobody, which is what makes
	// the count above attributable to the drag rather than to the tick.
	CHECK(f.gui.tickForTest().stateFrames == 0);
}
TEST_CASE("a bar stops where its band's own range runs out, and that is what a displaced global costs") {
	// CHARACTERISED RATHER THAN FIXED — the residue of "other bars sometimes not redrawing"
	// (2026-08-26), written down so the next reader does not have to rediscover the mechanism.
	//
	// `effectiveBandOf` clamps `band + shift` to 0..1, so a band declared 0..1 can reach only
	// `[max(0, shift), min(1, 1 + shift)]` of the effective value the compressor uses. The graph
	// draws each bar over the WHOLE sweep, so once a global is off neutral part of the drawn sweep is
	// unreachable and the handle silently stops following the pointer there. That is ordinary
	// end-of-travel for an offset control, and it is the same arithmetic the ratio fix above is
	// about — the difference is that ratio paid it AT THE DEFAULT and these three do not.
	//
	// MEASURED, so the numbers are the instrument's and not a model's: with the global threshold at
	// 1.0 the low band's lower bar sits at the top of its sweep, and a drag aimed at the floor of the
	// plot leaves it at the middle. Pinned here because a change to the clamp — in either direction —
	// should have to argue with a case rather than pass quietly.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	auto* graph = card->graph();
	REQUIRE(graph != nullptr);

	const visage::Bounds bars = graph->barArea();
	const size_t idx = static_cast<size_t>(fx::owltt::Param::LowThreshold);

	SUBCASE("at the default global the whole sweep answers") {
		// THE CONTROL ARM. Without it the subcase below is a claim about OWLTT and not about the
		// shift: a bar that never followed the pointer would satisfy it just as well.
		REQUIRE(f.bridge.fxChain().device[1].row.field[static_cast<size_t>(fx::owltt::Param::Threshold)]
		        == doctest::Approx(0.5f));
		const visage::Point ring = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
		// AIMED AT 30% UP: the top bar now stops at the up threshold (0.125 at the defaults), which the floor of the plot is below, so the
		// sweep it answers over is the part above that.
		const float toY = bars.bottom() - 0.3f * bars.height();
		// THE PLOT READS THE THRESHOLD PARAMETER'S OWN 0..1 over the bar area (the compressor's `0.2 + 0.8 t` is its internal
		// scale for the same range), so the pointer's height IS the effective threshold; this restated `0.2 + 0.8 t`, the
		// mapping the plot used before it was made to match the sound.
		const float wanted = std::clamp((bars.bottom() - toY) / bars.height(), 0.0f, 1.0f);
		graph->onMouseDown().callback(at(ring.x, ring.y));
		graph->onMouseDrag().callback(at(ring.x, toY));
		graph->onMouseUp().callback(at(ring.x, toY));
		CAPTURE(wanted);
		CHECK(graph->effective(0).threshold == doctest::Approx(wanted).epsilon(0.02));
		CHECK(graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y
		      == doctest::Approx(toY).epsilon(0.05));
	}

	SUBCASE("with the global at its ceiling the bottom of the sweep is out of reach") {
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Threshold), 1.0f);
		// THE SHIFT IS HALF THE RANGE, so the band's stored 0..1 reaches effective 0.5..1.0.
		REQUIRE(graph->effective(0).threshold == doctest::Approx(1.0f));

		const visage::Point ring = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
		const float toY = bars.bottom() - ui::OwlttGraph::kRingRadius;
		graph->onMouseDown().callback(at(ring.x, ring.y));
		graph->onMouseDrag().callback(at(ring.x, toY));
		graph->onMouseUp().callback(at(ring.x, toY));

		// IT MOVED — as far as it could — which is why "the bar did not respond" is the wrong
		// description and "it stopped early" is the right one.
		// (SINCE THE TOP BAR HOLDS THE UP THRESHOLD STILL, the bar's floor is the up threshold: with the global threshold at its
		// ceiling and the bandwidth at its default that is 1.0 - 0.75 x 0.5 = 0.625 effective, so the stored value is 0.625 - 0.5. The
		// bar stops THERE - at the lower bar - before it reaches what the band's own range could express.)
		CHECK(f.bridge.fxChain().device[1].row.field[idx] == doctest::Approx(0.125f));
		// AND STOPPED AT THE FLOOR OF WHAT THE BAND CAN EXPRESS, not at the pointer. Y GROWS
		// DOWNWARD, so a bar that stopped short of the floor sits at a SMALLER y than the target —
		// the first draft of this line had the comparison the other way up and failed for the
		// direction rather than for the claim.
		CHECK(graph->effective(0).threshold == doctest::Approx(0.625f));
		CHECK(graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y < toY - 10.0f);

		// AND A SECOND DRAG THE SAME WAY DOES NOTHING AT ALL, which is the half a hand reports as
		// "not responding".
		const visage::Point stalled = graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar);
		graph->onMouseDown().callback(at(stalled.x, stalled.y));
		graph->onMouseDrag().callback(at(stalled.x, toY));
		graph->onMouseUp().callback(at(stalled.x, toY));
		CHECK(graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y
		      == doctest::Approx(stalled.y));
	}
}
TEST_CASE("the waypoint card's dots are where they are drawn, and each one writes its own waypoint") {
	// THE CARD'S CLAIM IN `addressedParamsForTest` IS THAT DRAGGING DOT n WRITES ITS CUTOFF AND ITS
	// RESONANCE. This is the case that stops the claim being a wish — it asserts the VALUE each
	// gesture writes, and it asks `frameAtPoint` where the hand would actually land.
	//
	// THIS LANE HAS SHIPPED FOUR "DRAWN HERE, GRABBABLE THERE" BUGS. A plane with four draggable dots
	// over a knob row is where the fifth would live, so the hit test is asserted before the values.
	namespace wp = feathers::fx::waypoint;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(wp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::WaypointCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	card->setBounds(0.0f, 0.0f, 240.0f, card->laneHeight());
	card->resized();

	// FOUR DISTINCT WAYPOINTS, so no assertion below can pass because two dots agree.
	const auto write = [&](wp::Param p, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, static_cast<size_t>(p) };
		c.setValue(v);
		f.bridge.refresh();
	};
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		const size_t base = static_cast<size_t>(wp::Param::Cutoff0) + i * 4;
		write(static_cast<wp::Param>(base), std::log2(250.0 * std::pow(2.0, static_cast<double>(i))));
		write(static_cast<wp::Param>(base + 1), 0.1 * static_cast<double>(i + 1));
	}

	SUBCASE("every dot answers its own centre, and the nearest one wins a tie") {
		for (size_t i = 0; i < wp::kWaypoints; ++i) {
			CAPTURE(i);
			const visage::Point c = card->dotCentre(i);
			// INSIDE THE PLANE, or the dot is drawn somewhere a hand cannot reach at all.
			CHECK(card->planeBounds().contains(c));
			// AND THE HIT TEST AGREES WITH THE DRAWING, which is the whole of this lane's recurring bug.
			CHECK(card->dotAt(c) == i);
		}
		// A POINT FAR FROM EVERY DOT IS NOT A DOT — or `dotAt` answers yes to everything and the
		// assertions above say nothing.
		CHECK(card->dotAt({ card->planeBounds().right() - 1.0f, card->planeBounds().y() + 1.0f })
		      >= wp::kWaypoints);
	}

	SUBCASE("two dots close together resolve to the NEARER one, not to the lower index") {
		// ADDED BECAUSE ITS KNOWN-BAD CAME BACK GREEN. The subcase above spreads the four waypoints an
		// octave apart, so no two overlap and "first wins" is indistinguishable from "nearest wins" —
		// a mutation making `dotAt` return the first match within the grab radius passed every
		// assertion in this case.
		//
		// THE BUG THAT HIDES THERE is waypoint 0 becoming UNREACHABLE whenever another dot sits on it:
		// first-wins would hand every press to the lower index, and this lane has shipped four
		// "drawn here, grabbable there" bugs already. So the fixture has to put two dots close
		// together, which the natural one never does.
		const size_t base3 = static_cast<size_t>(wp::Param::Cutoff0) + 3 * 4;
		const size_t base0 = static_cast<size_t>(wp::Param::Cutoff0);
		ui::FxControl c3 { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, base3 };
		ui::FxControl r3 { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, base3 + 1 };
		ui::FxControl c0 { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, base0 };
		ui::FxControl r0 { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, base0 + 1 };
		// WAYPOINT 3 PUT ALMOST ON TOP OF WAYPOINT 0 — a few pixels apart, well inside one grab radius.
		c0.setValue(std::log2(1000.0));
		r0.setValue(0.5);
		c3.setValue(std::log2(1080.0));
		r3.setValue(0.52);
		f.bridge.refresh();

		const visage::Point p0 = card->dotCentre(0);
		const visage::Point p3 = card->dotCentre(3);
		// THE FIXTURE IS DOING WHAT IT CLAIMS: the two really are within one grab radius of each other,
		// or this subcase is the spread-out one again under a different name.
		const float apart = std::hypot(p3.x - p0.x, p3.y - p0.y);
		CAPTURE(apart);
		REQUIRE(apart > 0.5f);
		REQUIRE(apart < 11.0f);

		// ASKED NEARER THREE, ANSWERS THREE. First-wins would answer 0, because 0 is inside the radius
		// too and comes first.
		CHECK(card->dotAt(p3) == 3);
		// AND ASKED NEARER ZERO, ANSWERS ZERO — so the assertion above is about DISTANCE and not about
		// a tie-break that simply prefers the higher index.
		CHECK(card->dotAt(p0) == 0);
	}

	SUBCASE("dragging a dot writes THAT waypoint's cutoff and resonance, and no other's") {
		const size_t target = 2;
		const visage::Point from = card->dotCentre(target);
		const auto valueOf = [&](wp::Param p) {
			ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, static_cast<size_t>(p) };
			return c.value();
		};
		const size_t base = static_cast<size_t>(wp::Param::Cutoff0) + target * 4;
		const double otherCutoffBefore = valueOf(static_cast<wp::Param>(base - 4));

		card->onMouseDown().callback(at(from.x, from.y));
		const visage::Bounds box = card->planeBounds();
		const visage::Point to { box.x() + box.width() * 0.75f, box.y() + box.height() * 0.25f };
		card->onMouseDrag().callback(at(to.x, to.y));
		card->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();

		// THE VALUES, COMPUTED FROM THE AXES RATHER THAN COMPARED TO THEMSELVES. X is log frequency
		// across the plane's width; Y is resonance, inverted so up is more.
		namespace flt = feathers::fx::filter;
		const double span = flt::kCutoffMaxLog2 - flt::kCutoffMinLog2;
		CHECK(valueOf(static_cast<wp::Param>(base))
		      == doctest::Approx(flt::kCutoffMinLog2 + 0.75 * span).epsilon(0.02));
		CHECK(valueOf(static_cast<wp::Param>(base + 1)) == doctest::Approx(0.75).epsilon(0.02));
		// AND IT WROTE ONLY THAT WAYPOINT. A drag that moved all four would satisfy both assertions
		// above and be a different device.
		CHECK(valueOf(static_cast<wp::Param>(base - 4)) == doctest::Approx(otherCutoffBefore));
	}

	SUBCASE("the press selects, so the knob row is pointed at the dot the hand took") {
		REQUIRE(card->selected() == 0);
		const visage::Point c = card->dotCentre(3);
		card->onMouseDown().callback(at(c.x, c.y));
		card->onMouseUp().callback(at(c.x, c.y));
		CHECK(card->selected() == 3);

		// AND THE SELECTED WAYPOINT'S MACRO KNOBS ARE THE VISIBLE ONES. Every waypoint has its own
		// pair — `Knob` has no retargeting and adding some to serve one card would mutate the most
		// shared widget in the editor — so selection is a VISIBILITY question and this asserts it as
		// one.
		// **BY POSITION, NOT BY THE LABEL'S DIGIT.** This used to read the `4` out of `Macro 1 4` to
		// prove the visible pair belonged to waypoint 4 — and that stopped working the moment the card
		// dropped the index from its captions, because on a card that shows one waypoint at a time the
		// digit distinguished nothing a player could see.
		//
		// THE PROXY WAS THE WEAKER CLAIM ANYWAY. The macro knobs are built two per waypoint in order,
		// so the pair for waypoint N is at offsets 2N and 2N+1 of that sublist — an identity, where the
		// label was a string that happened to contain a character.
		std::vector<size_t> macroPositions;
		for (size_t i = 0; i < card->knobsForTest().size(); ++i) {
			const ui::Knob* knob = card->knobsForTest()[i].get();
			if (knob->label() != nullptr && std::string(knob->label()).rfind("Macro", 0) == 0) {
				macroPositions.push_back(i);
			}
		}
		REQUIRE(macroPositions.size() == 2 * wp::kWaypoints);

		size_t visibleMacros = 0;
		for (size_t m = 0; m < macroPositions.size(); ++m) {
			const bool visible = card->knobsForTest()[macroPositions[m]]->isVisible();
			const bool belongsToSelected = m / 2 == 3;
			CAPTURE(m);
			CHECK(visible == belongsToSelected);
			visibleMacros += visible ? 1 : 0;
		}
		CHECK(visibleMacros == 2);
	}
}
TEST_CASE("the drawn card's puck is where it is drawn, and it writes both morph axes") {
	// THE CARD'S CLAIM IN `addressedParamsForTest` IS THAT DRAGGING THE PUCK WRITES MorphX AND MorphY.
	// This is the case that stops the claim being a wish: it asserts the VALUES the gesture writes and
	// it asks the drawing where the puck is, rather than trusting that the two agree.
	//
	// THIS LANE HAS SHIPPED FOUR "DRAWN HERE, GRABBABLE THERE" BUGS, which is why the round trip
	// between `puckCentre()` and `dragPuck` is asserted in both directions rather than once.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::DrawnCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	card->setBounds(0.0f, 0.0f, 240.0f, card->laneHeight());
	card->resized();

	const auto valueOf = [&](dr::Param p) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, static_cast<size_t>(p) };
		return c.value();
	};

	SUBCASE("the puck is inside the plane, and the plane is inside the card") {
		CHECK(card->planeBounds().contains(card->puckCentre()));
		CHECK(card->planeBounds().bottom() <= card->height());
		// **THEY ARE NOW ONE RECTANGLE.** As on AutoMod, the filter curve is overlaid on the XY pad, and
		// it fits without scrolling. This assertion used to read
		// `curveBounds().bottom() <= planeBounds().y()` and its comment gave the objection: the
		// strip's X is log frequency and the plane's X is a morph weight, so overlaying them asserts a
		// relationship between two axes that share nothing.
		//
		// THE OBJECTION SURVIVES AND IS ANSWERED RATHER THAN DROPPED. The curve now draws its own
		// decade marks, in its own colour, on plates — automod's convention on the strip where it
		// already overlays a response on a waveform for the same reason. Two axes on one box is fine
		// once each says which one it is; what was forbidden was two SILENT axes.
		CHECK(card->curveBounds().x() == doctest::Approx(card->planeBounds().x()));
		CHECK(card->curveBounds().width() == doctest::Approx(card->planeBounds().width()));
		CHECK(card->curveBounds().bottom() == doctest::Approx(card->planeBounds().bottom()));
		CHECK(card->curveBounds().y() >= ui::DrawnCard::kSlotHeadHeight);

		// AND THE WHOLE CARD FITS A LANE, which is the entire reason this device has a card of its own
		// rather than the generic one. `GenericCard` draws every declared control, and 103 of them
		// MEASURED 2842 pixels tall with an enable square no press could reach. The bound is loose on
		// purpose — it is not a pin on the layout, it is the difference between a card and a wall.
		CAPTURE(card->laneHeight());
		CHECK(card->laneHeight() < 600.0f);
		CHECK(card->laneHeight() > ui::DrawnCard::kSlotHeadHeight);
	}

	SUBCASE("dragging the puck writes both axes, and up is more") {
		const visage::Bounds box = card->planeBounds();
		const visage::Point to { box.x() + box.width() * 0.75f, box.y() + box.height() * 0.25f };
		card->onMouseDown().callback(at(box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f));
		card->onMouseDrag().callback(at(to.x, to.y));
		card->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();

		// THE VALUES, COMPUTED FROM THE AXES RATHER THAN COMPARED TO THEMSELVES. X runs left to right;
		// Y is INVERTED, so a quarter of the way down the box is three quarters of the way up the
		// control. A card that dropped the inversion would put the puck on the wrong frame pair and
		// every other assertion here would still pass.
		CHECK(valueOf(dr::Param::MorphX) == doctest::Approx(0.75).epsilon(0.02));
		CHECK(valueOf(dr::Param::MorphY) == doctest::Approx(0.75).epsilon(0.02));

		// AND THE DRAWING AGREES WITH WHAT WAS WRITTEN — the other half of the round trip, which is
		// the half this lane keeps getting wrong.
		const visage::Point drawn = card->puckCentre();
		CHECK(drawn.x == doctest::Approx(to.x).epsilon(0.01));
		CHECK(drawn.y == doctest::Approx(to.y).epsilon(0.01));
	}

	SUBCASE("the puck's corners are the frames the BLEND calls corners") {
		// THE ONE ASSERTION THAT TIES THE GEOMETRY TO THE ENGINE. A Y-flip, or a card that named the
		// corners in the blend's other order, would be invisible to every check above — the puck would
		// move, the values would be in range, and the device would simply morph toward the wrong
		// frame. So each corner of the plane is dragged to, and the REALISED cascade there is required
		// to equal that corner's own frame realised on its own.
		//
		// Four distinct frames first, or "it reached the right corner" passes because all four agree.
		const auto write = [&](size_t index, double v) {
			ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
			c.setValue(v);
		};
		for (size_t frame = 0; frame < dr::kFrames; ++frame) {
			for (size_t band = 0; band < dr::kBands; ++band) {
				write(dr::bandFieldIndex(frame, band, dr::PoleRadius),
				      0.2 + 0.15 * static_cast<double>(frame));
				write(dr::bandFieldIndex(frame, band, dr::PoleAngle),
				      -10.0 + static_cast<double>(band) + 0.7 * static_cast<double>(frame));
			}
		}
		f.bridge.refresh();

		const visage::Bounds box = card->planeBounds();
		struct Corner {
			dr::Corner which;
			float x;
			float y;
			const char* name;
		};
		// BOTTOM-LEFT IS (0,0) BECAUSE Y IS DRAWN UP.
		const std::array<Corner, 4> corners { Corner { dr::X0Y0, box.x(), box.bottom(), "x0y0" },
			                                  Corner { dr::X1Y0, box.right(), box.bottom(), "x1y0" },
			                                  Corner { dr::X0Y1, box.x(), box.y(), "x0y1" },
			                                  Corner { dr::X1Y1, box.right(), box.y(), "x1y1" } };
		for (const Corner& c : corners) {
			CAPTURE(c.name);
			card->onMouseDown().callback(at(box.x() + box.width() * 0.5f,
			                                box.y() + box.height() * 0.5f));
			card->onMouseDrag().callback(at(c.x, c.y));
			card->onMouseUp().callback(at(c.x, c.y));
			f.bridge.refresh();

			// THE DEVICE'S OWN STATE, decoded from the row the card just wrote, and the corner frame
			// realised on its own as the independent restatement.
			const FxChain chain = f.bridge.fxChain();
			dr::Row row {};
			const DeviceRow& deviceRow = fxDeviceAtPosition(chain, slot).row;
			const size_t present = std::min(deviceRow.fieldsPresent, row.size());
			for (size_t i = 0; i < present; ++i) {
				row[i] = deviceRow.field[i];
			}
			const dr::Settings s = dr::stateOf(row, present);
			CHECK(dr::realisedAt(s)
			      == dr::realise(dr::withGlobals(s.frame[c.which], s.cutoff, s.res)));
		}
	}
}
TEST_CASE("the drawn card names its four frames, and the puck cannot hide the one you are dragging toward") {
	// **THE PLANE HAD FOUR CORNERS AND SAID NOTHING ABOUT WHICH WAS WHICH.** Four dim dots announced
	// that frames existed; the one question a hand on the puck is actually asking — what am I morphing
	// between — had its answer only behind the expand button. So Drawn floats each waypoint's id near the
	// waypoint, making it clear what morphs into what.
	//
	// **WHAT THIS CASE CAN AND CANNOT SEE, SAID PLAINLY.** A label's INK cannot be read back
	// headlessly — there is no way to ask a canvas what glyphs landed where — so nothing here asserts
	// that the text was drawn. What is assertable is the geometry the drawing is placed by, the string
	// the drawing is handed, and the correspondence between a corner's label and the frame the device
	// actually morphs to there. The third is the one worth having: "there are four labels" is a claim
	// any four rectangles satisfy, and a label that named the wrong corner would be worse than none.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::DrawnCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	card->setBounds(0.0f, 0.0f, 240.0f, card->laneHeight());
	card->resized();

	const std::array<dr::Corner, dr::kFrames> order { dr::X0Y0, dr::X1Y0, dr::X0Y1, dr::X1Y1 };
	// BOTTOM-LEFT IS (0,0) BECAUSE Y IS DRAWN UP, the same reading `puckCentre` and `dragPuck` share.
	const auto cornerPoint = [&](dr::Corner c) {
		const visage::Bounds box = card->planeBounds();
		const bool right = c == dr::X1Y0 || c == dr::X1Y1;
		const bool top = c == dr::X0Y1 || c == dr::X1Y1;
		return visage::Point { right ? box.right() : box.x(), top ? box.y() : box.bottom() };
	};
	// The closest point of a rectangle to a point, so "does the puck's ink touch this label" is a
	// distance rather than four inequalities written out four times.
	const auto gapTo = [](const visage::Bounds& r, visage::Point p) {
		const float dx = std::max({ r.x() - p.x, 0.0f, p.x - r.right() });
		const float dy = std::max({ r.y() - p.y, 0.0f, p.y - r.bottom() });
		return std::hypot(dx, dy);
	};

	SUBCASE("every label is inside the plane, inboard of its own corner and no other") {
		const visage::Bounds box = card->planeBounds();
		for (const dr::Corner c : order) {
			CAPTURE(static_cast<size_t>(c));
			const visage::Bounds at = card->frameLabelBounds(c);
			CAPTURE(at.x());
			CAPTURE(at.y());
			// A LABEL THAT LEAVES THE PLANE IS DRAWN OVER WHATEVER IS NEXT TO IT — the knob row below
			// or the response strip above — so this is the containment claim, both edges of both axes.
			CHECK(at.width() > 0.0f);
			CHECK(at.height() > 0.0f);
			CHECK(at.x() >= box.x());
			CHECK(at.y() >= box.y());
			CHECK(at.right() <= box.right());
			CHECK(at.bottom() <= box.bottom());

			// AND IT IS NEAREST THE CORNER IT NAMES. Distance to its own corner against the distance
			// to each of the other three: a table read in the wrong order, or a Y that was not
			// flipped, puts every label inside the plane and every one of them on the wrong corner.
			const float own = gapTo(at, cornerPoint(c));
			for (const dr::Corner other : order) {
				if (other == c) {
					continue;
				}
				CAPTURE(static_cast<size_t>(other));
				CHECK(own < gapTo(at, cornerPoint(other)));
			}
		}
	}

	SUBCASE("the puck at a corner does not cover that corner's label, or any other") {
		// **THE REASON THE INSET IS 8 AND NOT 2.** The puck's centre reaches the plane's corner
		// exactly, so at each extreme there is a disc of accent ink sitting ON a corner — and a label
		// hidden at precisely the moment you are looking at that corner is worse than no label at all.
		//
		// DRIVEN BY THE REAL GESTURE rather than by writing the two controls, because it is the drawn
		// puck that would do the covering and `puckCentre` is what draws it.
		const visage::Bounds box = card->planeBounds();
		for (const dr::Corner c : order) {
			CAPTURE(static_cast<size_t>(c));
			const visage::Point to = cornerPoint(c);
			card->onMouseDown().callback(at(box.x() + box.width() * 0.5f,
			                                box.y() + box.height() * 0.5f));
			card->onMouseDrag().callback(at(to.x, to.y));
			card->onMouseUp().callback(at(to.x, to.y));
			f.bridge.refresh();

			const visage::Point puck = card->puckCentre();
			// NOT VACUOUS: the puck really is at this corner, or "it does not cover the label" is a
			// statement about a puck that never moved.
			REQUIRE(std::hypot(puck.x - to.x, puck.y - to.y) < 0.5f);
			for (const dr::Corner any : order) {
				CAPTURE(static_cast<size_t>(any));
				const float gap = gapTo(card->frameLabelBounds(any), puck);
				CAPTURE(gap);
				CHECK(gap > ui::DrawnCard::kPuckRadius);
			}
		}
	}

	SUBCASE("the label at a corner names the frame the device actually morphs to there") {
		// **THE ASSERTION THE OTHER TWO ARE SCAFFOLDING FOR.** A label is a claim about what the
		// device does at that corner, and geometry cannot tell you whether the claim is true — so the
		// puck is driven to each extreme, the REALISED cascade there is required to equal one
		// particular frame realised on its own, and that frame's numeral is required to be the one the
		// label at that corner carries.
		//
		// FOUR DISTINCT FRAMES FIRST, or every corner realises the same cascade and "it names the
		// right one" passes for all 24 wrong orderings as readily as for the right one.
		const auto write = [&](size_t index, double v) {
			ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
			c.setValue(v);
		};
		for (size_t frame = 0; frame < dr::kFrames; ++frame) {
			for (size_t band = 0; band < dr::kBands; ++band) {
				write(dr::bandFieldIndex(frame, band, dr::PoleRadius),
				      0.2 + 0.15 * static_cast<double>(frame));
				write(dr::bandFieldIndex(frame, band, dr::PoleAngle),
				      -10.0 + static_cast<double>(band) + 0.7 * static_cast<double>(frame));
			}
		}
		f.bridge.refresh();

		const visage::Bounds box = card->planeBounds();
		for (const dr::Corner c : order) {
			CAPTURE(static_cast<size_t>(c));
			const visage::Point to = cornerPoint(c);
			card->onMouseDown().callback(at(box.x() + box.width() * 0.5f,
			                                box.y() + box.height() * 0.5f));
			card->onMouseDrag().callback(at(to.x, to.y));
			card->onMouseUp().callback(at(to.x, to.y));
			f.bridge.refresh();

			const FxChain chain = f.bridge.fxChain();
			dr::Row row {};
			const DeviceRow& deviceRow = fxDeviceAtPosition(chain, slot).row;
			const size_t present = std::min(deviceRow.fieldsPresent, row.size());
			for (size_t i = 0; i < present; ++i) {
				row[i] = deviceRow.field[i];
			}
			const dr::Settings s = dr::stateOf(row, present);
			// WHICH FRAME THIS CORNER ACTUALLY IS, decided by the engine rather than by this test's
			// idea of the corner order.
			size_t realisedFrame = dr::kFrames;
			for (size_t k = 0; k < dr::kFrames; ++k) {
				if (dr::realisedAt(s) == dr::realise(dr::withGlobals(s.frame[k], s.cutoff, s.res))) {
					realisedFrame = k;
				}
			}
			REQUIRE(realisedFrame < dr::kFrames);
			// AND THE LABEL FLOATING HERE CARRIES THAT FRAME'S NUMERAL.
			const std::string label = ui::DrawnCard::frameLabelText(c);
			CAPTURE(label);
			CAPTURE(realisedFrame);
			CHECK(label.find(dr::kFrameNumerals[realisedFrame]) != std::string::npos);
		}
	}

	SUBCASE("the card and the expanded editor call each frame by ONE name") {
		// **TWO NAMES FOR ONE THING IS THE BUG THIS REPO KEEPS PAYING FOR**, and a corner marker that
		// disagreed with the tab you open to edit that corner would be a fresh instance of it. Both
		// surfaces compose from `dr::kFrameNumerals`; this asserts the composition rather than the
		// table, because a second table added later would still satisfy a check on the first.
		for (size_t i = 0; i < dr::kFrames; ++i) {
			CAPTURE(i);
			const std::string numeral = dr::kFrameNumerals[i];
			const std::string onCard = ui::DrawnCard::frameLabelText(static_cast<dr::Corner>(i));
			const std::string onPanel = ui::DrawnExpanded::frameTabText(i);
			CAPTURE(onCard);
			CAPTURE(onPanel);
			CHECK(onCard.find(numeral) != std::string::npos);
			CHECK(onPanel.find(numeral) != std::string::npos);
			// AND THE NUMERALS ARE FOUR DIFFERENT STRINGS, or "they agree" is satisfied by four
			// identical labels that name nothing.
			for (size_t j = i + 1; j < dr::kFrames; ++j) {
				CHECK(numeral != std::string(dr::kFrameNumerals[j]));
			}
		}
		// A BARE DIGIT WOULD READ AS AN AXIS TICK on a plane whose axes both run 0 to 1, so the card's
		// form carries a letter as well. Asserted because it is the reason the two forms differ at
		// all — without it the obvious "simplification" is to draw the numeral alone.
		CHECK(ui::DrawnCard::frameLabelText(dr::X0Y0).size() > std::string(dr::kFrameNumerals[0]).size());
	}
}
TEST_CASE("every knob on the drawn card answers the hand, Wake included") {
	// THIS CASE USED TO ASSERT THE OPPOSITE FOR ONE KNOB. `Smooth` was declared and inert, so its knob
	// was drawn and DISABLED — `Knob::setEnabled(false)`, whose own comment documents that shape:
	// "MOVING the knob would be a lie". Design §8 gave the control a job (the zeros lag the poles
	// while the puck moves), so the lie is gone and so is the exception.
	//
	// INVERTED RATHER THAN DELETED, deliberately: if `Wake` ever loses its implementation the knob
	// goes back to refusing the hand, and this line says so instead of the file quietly losing the
	// only case that ever mentioned it. The reachability entry asserts the same fact from the other
	// side — the card claims `Wake`, and the `drwn` gap list no longer holds it.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	f.lane->resized();
	auto* card = dynamic_cast<ui::DrawnCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);

	size_t enabled = 0;
	bool sawWake = false;
	for (const auto& knob : card->knobsForTest()) {
		REQUIRE(knob->label() != nullptr);
		CAPTURE(std::string(knob->label()));
		CHECK(knob->enabled());
		++enabled;
		if (std::string(knob->label()).find("Wake") != std::string::npos) {
			sawWake = true;
		}
	}
	CHECK(enabled == ui::DrawnCard::kKnobCells);
	// AND THE KNOB IN QUESTION IS ACTUALLY THERE, or "every knob answers" is a claim about five knobs.
	CHECK(sawWake);

	// AND THE CARD CLAIMS IT, which is what the reachability guard reads.
	const auto reached = card->addressedParamsForTest();
	CHECK(std::find(reached.begin(), reached.end(), dr::Param::Wake) != reached.end());
	CHECK(std::find(reached.begin(), reached.end(), dr::Param::MorphX) != reached.end());
}
TEST_CASE("the drawn card's knobs write the controls their labels name") {
	// THE OTHER HALF OF `addressedParamsForTest`. The puck's case above covers the two morph axes; the
	// four globals are reached by knobs, and this asserts that each knob writes the control it is
	// labelled with rather than the one next to it — an off-by-one in the layout table would move
	// every gesture one control along and change nothing visible.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	f.lane->resized();
	auto* card = dynamic_cast<ui::DrawnCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);

	for (const auto& knob : card->knobsForTest()) {
		REQUIRE(knob->label() != nullptr);
		const std::string label(knob->label());
		// EVERY LABEL IS A DECLARED NAME, asked of the descriptor rather than spelled here — a card
		// that invented a caption would be describing a control the host does not have.
		size_t matches = 0;
		size_t index = static_cast<size_t>(dr::Param::kCount);
		for (size_t i = 0; i < static_cast<size_t>(dr::Param::kCount); ++i) {
			if (label == dr::kParamDescs[i].name) {
				++matches;
				index = i;
			}
		}
		CAPTURE(label);
		CHECK(matches == 1);
		CHECK(index < static_cast<size_t>(dr::Param::kCount));
	}

	// AND THE SIX ARE THE SIX THE LAYOUT MEANS TO SHOW, in cell order.
	// UNPREFIXED. 2026-08-27: the controls said "Drawn XYZ", which "takes up space and is
	// redundant since the knob is on Drawn". The card already names the device in its head, so the
	// prefix was the device saying its own name twice. OWLTT and Waypoint were already unprefixed;
	// filter and harm still are not, which is a tidy-up for whoever owns those files.
	//
	// AND THE LAST ONE IS `Wake` RATHER THAN `Smooth` — design §8 gave the inert control a job, so
	// the knob answers the hand now. Two changes crossed on this line: the unprefixing and the
	// rename. Both are here.
	const std::array<const char*, 6> want { "Morph X", "Morph Y", "Cutoff",
		                                    "Res",     "Drive",   "Wake" };
	REQUIRE(card->knobsForTest().size() == want.size());
	for (size_t i = 0; i < want.size(); ++i) {
		CAPTURE(i);
		CHECK(std::string(card->knobsForTest()[i]->label()) == want[i]);
	}
}
TEST_CASE("the drawn editor's radius axis is a log axis in distance from the unit circle") {
	// The defect: the pop-out editor was twitchy. The vertical axis was
	// LINEAR IN RADIUS, and `Q ~ 1/(2(1 - r))`, so it was a reciprocal Q axis: one pixel of a 300px
	// plane moved Q from 50 to 150, and the whole of Q 50 -> 500 occupied 1.35 pixels.
	//
	// WHAT IS ASSERTED HERE IS THE NUMBER, NOT THE SHAPE. "It feels better" is not a claim a case can
	// hold, and the whole point of the change is a quantity — pixels per octave of Q — so that is
	// what is measured, on the DRAWING road (`poleCentre`), with Q derived from the stored radius by
	// the resonator relation rather than read back out of the axis being tested. An axis that had
	// been warped the wrong way, or not at all, cannot make these pass.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();
	panel->selectFrame(0);

	using Axis = ui::DrawnExpanded;
	const visage::Bounds box = panel->planeBounds();
	REQUIRE(box.height() == doctest::Approx(Axis::kPlaneHeight));

	// ONE BAND MOVED OUT OF THE WAY OF THE OTHERS and used as the probe.
	//
	// **ITS ZERO IS PUT SOMEWHERE OF ITS OWN, BY THIS CASE, rather than found there.** The probe needs
	// a zero that is NOT sitting on its pole: several subcases press the pole and drag it, and a
	// coincident zero makes that the Bypass gesture — the zero follows — while one of them presses the
	// ZERO and requires the press to take it, which a coincident pole wins by the deliberate tie rule
	// in `hitAt`.
	//
	// IT USED TO BE FOUND THERE, and that is why the two lines exist. The default frames were four
	// audibly different filters when this case was written, band 0 of the lowpass frame had its zero
	// at Nyquist, and the case simply borrowed it — with a comment saying so, which is the only reason
	// it was easy to repair. The default is flat again now, every band is `Bypass`, and the borrowed
	// fixture evaporated: the zero subcase went red on its REQUIRE. **A test that leans on a default
	// is a test that changes meaning when the default does**, and this file has now paid that twice.
	const size_t probe = 0;
	{
		ui::FxControl radius { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
			                   dr::bandFieldIndex(0, probe, dr::ZeroRadius) };
		ui::FxControl angle { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
			                  dr::bandFieldIndex(0, probe, dr::ZeroAngle) };
		// FREE FIRST — see the sibling case: a bound zero is not grabbable, and every type but Free
		// binds it. This is the fixture line that used to be implicit.
		ui::FxControl type { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
			                 dr::bandFieldIndex(0, probe, dr::TypeIndex) };
		type.setValue(static_cast<double>(static_cast<int>(dr::Type::Free)));
		radius.setValue(1.0);
		angle.setValue(static_cast<double>(dr::kMaxAngleLog2));
		f.bridge.refresh();
		// THE FIXTURE IS DOING WHAT IT CLAIMS, or every subcase below is about a coincident pair
		// again and nothing says so.
		REQUIRE(panel->hitAt(panel->zeroCentre(probe)).what == ui::DrawnExpanded::Grab::Zero);
		REQUIRE(panel->hitAt(panel->poleCentre(probe)).what == ui::DrawnExpanded::Grab::Pole);
	}
	const auto placePole = [&](double radius) {
		ui::FxControl r { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, dr::bandFieldIndex(0, probe, dr::PoleRadius) };
		r.setValue(radius);
		f.bridge.refresh();
		return panel->poleCentre(probe).y;
	};
	const auto placeZero = [&](double radius) {
		ui::FxControl r { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, dr::bandFieldIndex(0, probe, dr::ZeroRadius) };
		r.setValue(radius);
		f.bridge.refresh();
		return panel->zeroCentre(probe).y;
	};

	SUBCASE("the axis and its inverse are exact inverses, on both sides of the circle") {
		// **THE PROPERTY A HAND NOTICES**, and the one most likely to break in a change like this: a
		// handle must sit under the pointer that dragged it. Both directions, because the drag uses
		// one and the drawing uses the other and it is their COMPOSITION that puts ink under a finger.
		float worstRadius = 0.0f;
		for (const float r : { 0.0f, 1.0e-4f, 0.25f, 0.5f, 0.62f, 0.8f, 0.9f, 0.99f, 0.999f,
			                   dr::kMaxPoleRadius, 1.0f, 1.0001f, 1.01f, 1.1f, 1.5f,
			                   Axis::kRadiusCeiling }) {
			CAPTURE(r);
			const float t = Axis::heightForRadius(r);
			CHECK(t >= 0.0f);
			CHECK(t <= 1.0f);
			worstRadius = std::max(worstRadius, std::abs(Axis::radiusAtHeight(t) - r));
		}
		CAPTURE(worstRadius);
		CHECK(worstRadius < 2.0e-5f);

		float worstHeight = 0.0f;
		for (int i = 0; i <= 600; ++i) {
			const float t = static_cast<float>(i) / 600.0f;
			worstHeight = std::max(worstHeight, std::abs(Axis::heightForRadius(Axis::radiusAtHeight(t)) - t));
		}
		CAPTURE(worstHeight);
		CHECK(worstHeight < 1.0e-5f);

		// AND IT IS STRICTLY INCREASING, which is what makes "inverse" mean something: a function
		// with a flat spot has a right inverse on a grid and still loses the handle between samples.
		float previous = -1.0f;
		for (int i = 0; i <= 600; ++i) {
			const float r = Axis::radiusAtHeight(static_cast<float>(i) / 600.0f);
			CHECK(r > previous);
			previous = r;
		}

		// A RADIUS OFF THE ENDS HAS NO POSITION, said out loud. A NEGATIVE radius is the split-real
		// pair and is drawn by `splitZeroCentres`; an allpass mirror past the ceiling (a pole at
		// 0.125 wants a zero at 8) is pinned to the top rather than drawn off the plane.
		CHECK(Axis::heightForRadius(-1.0f) == doctest::Approx(0.0));
		CHECK(Axis::heightForRadius(8.0f) == doctest::Approx(1.0));
	}

	SUBCASE("equal pixels are equal RATIO of Q — 19.1 px per octave, flat to 2 percent") {
		// THE MEASUREMENT THE WHOLE CHANGE IS FOR. Q is computed from the radius by the resonator
		// relation, the pixel comes from `poleCentre`, and neither knows about `asinh`.
		//
		// THE ANGLE DOES NOT ENTER IT and that is worth saying: Q is `theta / (2(1 - r))`, so at any
		// fixed frequency `d(log Q) = -d(log(1 - r))` exactly. This is not a linearisation around one
		// corner frequency — the spread below holds at every frequency the plane draws.
		const double qs[] = { 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0 };
		double lowest = 1e9;
		double highest = 0.0;
		double previousY = 0.0;
		for (size_t i = 0; i < std::size(qs); ++i) {
			const double radius = 1.0 - 1.0 / (2.0 * qs[i]);
			const double y = static_cast<double>(placePole(radius));
			if (i > 0) {
				const double perOctave = previousY - y;
				CAPTURE(qs[i - 1]);
				CAPTURE(qs[i]);
				CAPTURE(perOctave);
				// UP THE PLANE IS MORE Q, so the pixel step is positive; a sign error here is an axis
				// drawn upside down, which every "it changed" assertion would have missed.
				CHECK(perOctave > 0.0);
				lowest = std::min(lowest, perOctave);
				highest = std::max(highest, perOctave);
			}
			previousY = y;
		}
		CAPTURE(lowest);
		CAPTURE(highest);
		// **THE BOUND, STATED AS A NUMBER.** Measured 18.821 .. 19.150 px per octave of Q over
		// Q in [0.5, 128] at `kCircleSoftness = 1e-3` on a 300px plane. The window is wide enough
		// that turning the shaping constant by ear does not have to come here first, and narrow
		// enough that a linear axis — which spans 0.66 px/octave at Q = 128 and 100 px/octave at
		// Q = 0.5 — cannot enter it.
		CHECK(lowest > 18.0);
		CHECK(highest < 20.0);
		CHECK(highest / lowest < 1.02);

		// AND THE HEADLINE, WHICH IS THE BUG ITSELF: the stretch of the axis a hand could not aim in.
		const double atFifty = static_cast<double>(placePole(1.0 - 1.0 / 100.0));
		const double atFiveHundred = static_cast<double>(placePole(1.0 - 1.0 / 1000.0));
		CAPTURE(atFifty - atFiveHundred);
		// 1.35 px before this change; 58.5 px measured after it.
		CHECK(atFifty - atFiveHundred > 50.0);
	}

	SUBCASE("the circle, the floor and the ceiling land where the constants say") {
		// THE THREE FIXED POINTS OF THE AXIS, asserted against the box and the constants rather than
		// against the axis's own arithmetic — this is what would catch a `kCircleHeight` that was read
		// from the wrong end, which every ratio assertion above would survive.
		CHECK(placeZero(1.0) == doctest::Approx(box.bottom() - box.height() * Axis::kCircleHeight));
		CHECK(placeZero(0.0) == doctest::Approx(box.bottom()));
		CHECK(placeZero(Axis::kRadiusCeiling) == doctest::Approx(box.y()));

		// AND THE POLE CEILING IS BELOW THE CIRCLE, by a small but non-zero amount: `kMaxPoleRadius`
		// is 1e-4 from the circle and `kCircleSoftness` is 1e-3, so the top of the pole range sits
		// inside the axis's linear knee — 2.76px below the line — rather than on it.
		const float ceilingY = placePole(static_cast<double>(dr::kMaxPoleRadius));
		const float circleY = box.bottom() - box.height() * Axis::kCircleHeight;
		CAPTURE(ceilingY - circleY);
		CHECK(ceilingY > circleY);
		CHECK(ceilingY - circleY < 6.0f);
	}

	SUBCASE("a drag puts the handle under the pointer, at every height a pole can reach") {
		// REQUIREMENT 1 AND REQUIREMENT 3 COMPOSED, on the real gesture road rather than on the two
		// pure functions: press the pole, drag to a y, and ask where it is now drawn.
		//
		// STOPPING SHORT OF THE POLE CEILING IS THE POINT OF THE LOOP'S TOP END, not an omission —
		// above it the drag clamps and the handle deliberately STOPS following the pointer, which the
		// next case asserts from the other side.
		const float circleY = box.bottom() - box.height() * Axis::kCircleHeight;
		for (const float fraction : { 0.02f, 0.1f, 0.25f, 0.4f, 0.55f, 0.65f }) {
			CAPTURE(fraction);
			const float wantY = box.bottom() - box.height() * fraction;
			REQUIRE(wantY > circleY); // or the drag would be clamped and this would prove nothing
			const visage::Point from = panel->poleCentre(probe);
			panel->onMouseDown().callback(at(from.x, from.y));
			panel->onMouseDrag().callback(at(box.x() + box.width() * 0.5f, wantY));
			panel->onMouseUp().callback(at(box.x() + box.width() * 0.5f, wantY));
			f.bridge.refresh();
			const visage::Point now = panel->poleCentre(probe);
			CAPTURE(now.y);
			CAPTURE(wantY);
			// A QUARTER OF A PIXEL. The stored radius is a float that has been through the parameter
			// layer's normalise/denormalise as well as the axis, so this is not asserting the axis's
			// own precision — it is asserting what a hand sees.
			CHECK(std::abs(now.y - wantY) < 0.25f);
			CHECK(std::abs(now.x - (box.x() + box.width() * 0.5f)) < 0.25f);
		}
	}

	SUBCASE("a zero dragged to the floor is a radius of zero, not a Bandpass") {
		// **THE LOWER CLAMP IN `radiusAtHeight`, WHICH IS THE ONE PIECE OF THIS AXIS THAT IS NOT ABOUT
		// AIMING.** `circleUnwarp(-warpSpan())` is `1 - sinh(asinh(1/softness)) * softness`, which is
		// zero only to within a couple of ulp — and a NEGATIVE zero radius is not a small error, it is
		// the split-real branch: the band silently becomes a `Bandpass` whose zeros are at DC and
		// Nyquist, drawn as two marks on the border instead of as the handle the hand was holding.
		//
		// The old linear axis could not do this — `t * ceiling` with `t` clamped at 0 is 0 exactly —
		// so this hazard arrives WITH the warp and the case arrives with it.
		//
		// **AND THIS CASE IS CURRENTLY GUARDING A CONDITION THAT DOES NOT ARISE, WHICH IS SAID OUT
		// LOUD RATHER THAN LEFT FOR SOMEONE TO DISCOVER.** Deleting the clamp at the shipped
		// constants leaves this GREEN: the unclamped floor measures +5.36e-7, on the safe side of
		// zero by luck of the rounding. It was shown able to fire — `kCircleHeight` at 0.8, where the
		// floor is -3.58e-7, reddens the two assertions below with the clamp removed and passes with
		// it kept — and 175 of 304 swept (softness, height) pairs land on the wrong side. So this is
		// a case about the SHAPING CONSTANTS being tunable, and it will start doing real work the
		// first time one of them moves.
		const visage::Point from = panel->zeroCentre(probe);
		REQUIRE(panel->hitAt(from).what == ui::DrawnExpanded::Grab::Zero);
		REQUIRE(panel->hitAt(from).band == probe);
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(box.x() + box.width() * 0.5f, box.bottom() + 50.0f));
		panel->onMouseUp().callback(at(box.x() + box.width() * 0.5f, box.bottom() + 50.0f));
		f.bridge.refresh();
		ui::FxControl r { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
			              dr::bandFieldIndex(0, probe, dr::ZeroRadius) };
		CAPTURE(r.value());
		CHECK(r.value() == doctest::Approx(0.0));
		CHECK(r.value() >= 0.0);
		CHECK(panel->zeroIsSplitReal(probe) == false);
	}

	SUBCASE("the pole ceiling is still unreachable, at the top of the new axis") {
		// THE CEILING MOVED — it used to be a hair under the halfway line and is now 2.76px under the
		// seven-tenths line — so the case that it cannot be crossed has to be re-aimed at where the
		// line now is. Four heights: one pixel BELOW the circle (which is inside the 2.76px band the
		// clamp now owns, and would have been reachable before), on the line, one above it, and off
		// the top of the plane entirely.
		const float circleY = box.bottom() - box.height() * Axis::kCircleHeight;
		for (const float target : { circleY - 1.0f, circleY, circleY + 1.0f, box.y() - 200.0f }) {
			CAPTURE(target);
			const visage::Point from = panel->poleCentre(probe);
			panel->onMouseDown().callback(at(from.x, from.y));
			panel->onMouseDrag().callback(at(box.x() + box.width() * 0.5f, target));
			panel->onMouseUp().callback(at(box.x() + box.width() * 0.5f, target));
			f.bridge.refresh();
			ui::FxControl r { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
				              dr::bandFieldIndex(0, probe, dr::PoleRadius) };
			CAPTURE(r.value());
			CHECK(r.value() <= static_cast<double>(dr::kMaxPoleRadius));
			// AND THE DRAWING AGREES — the handle is at or below the pole-ceiling row, which is below
			// the unit circle. A clamp that held the VALUE while the ink went to the pointer would be
			// the same bug wearing the assertion above as cover.
			CHECK(panel->poleCentre(probe).y > circleY);
		}
	}
}
TEST_CASE("the drawn editor's handles are where they are drawn, and each writes its own band") {
	// THE PANEL'S CLAIM IN `addressedParamsForTest` IS THAT DRAGGING REACHES ALL NINETY-SIX. This is
	// what stops the claim being a wish: it asserts the VALUES each gesture writes, and it asks the
	// hit test whether it agrees with the drawing before it believes either.
	//
	// THIS LANE HAS SHIPPED FOUR "DRAWN HERE, GRABBABLE THERE" BUGS, and this surface has twelve
	// handles on one plane, which is where the fifth would live.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();

	const auto valueAt = [&](size_t index) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		return c.value();
	};

	// THE RADIUS THE VERTICAL AXIS OWES AT A HEIGHT FRACTION, restated here from the constants rather
	// than asked of the panel — `DrawnExpanded::radiusAtHeight` is a thing under test, so a subcase
	// that called it would be comparing the editor with itself. See the axis case above for what the
	// shape is and why. Each use also pins the literal it produces, so a change to `kCircleSoftness`
	// or `kCircleHeight` shows up here as a number rather than as a silently-retracking expectation.
	const auto radiusAtFraction = [](double fraction) {
		const double softness = static_cast<double>(ui::DrawnExpanded::kCircleSoftness);
		const double circle = static_cast<double>(ui::DrawnExpanded::kCircleHeight);
		const double span = fraction >= circle ? 1.0 - circle : circle;
		const double whole = std::asinh((ui::DrawnExpanded::kRadiusCeiling - 1.0) / softness);
		return 1.0 + softness * std::sinh((fraction - circle) * whole / span);
	};

	SUBCASE("every handle is inside the plane, and the hit test agrees with the drawing") {
		for (size_t frame = 0; frame < dr::kFrames; ++frame) {
			panel->selectFrame(frame);
			CAPTURE(frame);
			for (size_t b = 0; b < dr::kBands; ++b) {
				CAPTURE(b);
				const visage::Point pole = panel->poleCentre(b);
				CHECK(panel->planeBounds().contains(pole));
				const auto hit = panel->hitAt(pole);
				// THE POLE'S OWN CENTRE ANSWERS THE POLE, which is the whole of the recurring bug.
				CHECK(hit.what == ui::DrawnExpanded::Grab::Pole);
				CHECK(hit.band == b);
			}
		}
		// A POINT FAR FROM EVERY HANDLE IS NOT A HANDLE — or `hitAt` says yes to everything and the
		// assertions above say nothing.
		const visage::Bounds box = panel->planeBounds();
		CHECK(panel->hitAt({ box.right() - 1.0f, box.y() + 1.0f }).what
		      == ui::DrawnExpanded::Grab::None);
	}

	SUBCASE("no two handles in a default frame are drawn on top of each other") {
		// **THE SHIPPED DEFAULT HAD TWO BANDS WITH IDENTICAL NUMBERS**, in the lowpass frame and again
		// in the highpass one, so they drew at exactly the same point and one of the six was
		// unreachable by any press — the subcase above found it by asking a band's own centre and
		// getting its neighbour's index.
		//
		// A HAND CAN STILL STACK TWO HANDLES BY DRAGGING, and this does not fix that; what it fixes is
		// the device ARRIVING that way, which is the case nobody would have thought to check. The two
		// sections now share a corner and differ in Q, which is how a four-pole filter is built
		// anyway.
		for (size_t frame = 0; frame < dr::kFrames; ++frame) {
			panel->selectFrame(frame);
			CAPTURE(frame);
			for (size_t a = 0; a < dr::kBands; ++a) {
				for (size_t b = a + 1; b < dr::kBands; ++b) {
					CAPTURE(a);
					CAPTURE(b);
					const visage::Point pa = panel->poleCentre(a);
					const visage::Point pb = panel->poleCentre(b);
					// FURTHER APART THAN ONE GRAB RADIUS, which is the distance at which they stop
					// competing for the same press rather than merely being distinguishable by eye.
					CHECK(std::hypot(pa.x - pb.x, pa.y - pb.y) > ui::DrawnExpanded::kGrabRadius);
				}
			}
		}
	}

	SUBCASE("dragging a pole writes THAT band's pole in THAT frame, and no other") {
		panel->selectFrame(2);
		const size_t band = 3;
		const size_t otherBandField = dr::bandFieldIndex(2, 1, dr::PoleAngle);
		const size_t otherFrameField = dr::bandFieldIndex(1, band, dr::PoleAngle);
		const double otherBandBefore = valueAt(otherBandField);
		const double otherFrameBefore = valueAt(otherFrameField);

		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->poleCentre(band);
		const visage::Point to { box.x() + box.width() * 0.75f, box.bottom() - box.height() * 0.25f };
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(to.x, to.y));
		panel->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();

		// THE VALUES, COMPUTED FROM THE AXES rather than compared to themselves. X is the stored
		// `angleLog2` across the width; Y is radius, inverted so up is more — and a quarter of the way
		// up is **no longer a quarter of the ceiling**: the axis is logarithmic in distance from the
		// unit circle now, so a quarter of the height is r = 0.93377, which is Q = 7.5.
		const double wantAngle = dr::kMinAngleLog2
		                         + 0.75 * (dr::kMaxAngleLog2 - dr::kMinAngleLog2);
		CHECK(valueAt(dr::bandFieldIndex(2, band, dr::PoleAngle))
		      == doctest::Approx(wantAngle).epsilon(0.02));
		REQUIRE(radiusAtFraction(0.25) == doctest::Approx(0.9337729).epsilon(1e-5));
		CHECK(valueAt(dr::bandFieldIndex(2, band, dr::PoleRadius))
		      == doctest::Approx(radiusAtFraction(0.25)).epsilon(1e-4));
		// AND IT WROTE ONLY THAT ONE. A drag that moved every band, or wrote into the frame that
		// happened to be first, would satisfy both assertions above and be a different editor.
		CHECK(valueAt(otherBandField) == doctest::Approx(otherBandBefore));
		CHECK(valueAt(otherFrameField) == doctest::Approx(otherFrameBefore));
	}

	SUBCASE("dragging a zero writes the zero and leaves the pole where it is") {
		// THE OTHER HALF OF THE NINETY-SIX. The pole subcase above covers two fields per band; the
		// zero's two are reached by this gesture and by the type menu, and **this case did not exist
		// until the known-bad plan asked which mutation would catch a `dragZero` that wrote the pole.**
		// Nothing would have. A claimed road with no case behind it is what the panel's
		// `addressedParamsForTest` list is worth without one.
		panel->selectFrame(1);
		const size_t band = 0;
		// THE ZERO PUT SOMEWHERE OF ITS OWN FIRST, so it is not sitting on the pole — that is the
		// Bypass gesture and it has its own case; here the two must move independently.
		{
			ui::FxControl radius { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
				                   dr::bandFieldIndex(1, band, dr::ZeroRadius) };
			ui::FxControl angle { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
				                  dr::bandFieldIndex(1, band, dr::ZeroAngle) };
			// AND THE BAND MADE `Free` FIRST, which is what makes the two independent since
			// 2026-08-29. Every other type BINDS the zero to the pole — that is what "types stick"
			// means — and a bound zero is not grabbable at all, so a case about dragging the zero is
			// a case about a Free band whether it says so or not. It used to be implicit because
			// nothing was stored; now it is a fixture line, and the case is honest about needing it.
			ui::FxControl type { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
				                 dr::bandFieldIndex(1, band, dr::TypeIndex) };
			type.setValue(static_cast<double>(static_cast<int>(dr::Type::Free)));
			radius.setValue(1.0);
			angle.setValue(-8.0);
			f.bridge.refresh();
		}
		const double poleAngleBefore = valueAt(dr::bandFieldIndex(1, band, dr::PoleAngle));
		const double poleRadiusBefore = valueAt(dr::bandFieldIndex(1, band, dr::PoleRadius));

		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->zeroCentre(band);
		// THE PRESS HAS TO TAKE THE ZERO, or this is a case about dragging a pole.
		REQUIRE(panel->hitAt(from).what == ui::DrawnExpanded::Grab::Zero);
		REQUIRE(panel->hitAt(from).band == band);

		// **DRAGGED ABOVE THE UNIT CIRCLE, WHICH IS NOW AT 0.7 OF THE HEIGHT.** This used to read 0.6
		// and 0.6 used to be above a circle drawn halfway up; on the log axis 0.6 of the height is
		// r = 0.9987, still below the line, and the last assertion in this subcase — that a zero may
		// go above the circle — would have failed for a reason that had nothing to do with zeros.
		const visage::Point to { box.x() + box.width() * 0.4f, box.bottom() - box.height() * 0.85f };
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(to.x, to.y));
		panel->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();

		const double wantAngle = dr::kMinAngleLog2 + 0.4 * (dr::kMaxAngleLog2 - dr::kMinAngleLog2);
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle))
		      == doctest::Approx(wantAngle).epsilon(0.02));
		REQUIRE(radiusAtFraction(0.85) == doctest::Approx(1.0223495).epsilon(1e-5));
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius))
		      == doctest::Approx(radiusAtFraction(0.85)).epsilon(1e-4));
		// AND THE POLE DID NOT MOVE. A `dragZero` that wrote the pole's fields would satisfy every
		// assertion above if they were phrased about "something changed".
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::PoleAngle)) == doctest::Approx(poleAngleBefore));
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::PoleRadius)) == doctest::Approx(poleRadiusBefore));

		// A ZERO MAY GO ABOVE THE UNIT CIRCLE, which is the asymmetry the pole subcase asserts from
		// the other side: an allpass mirror sits at 1/r and nothing constrains it.
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius)) > 1.0);
	}

	SUBCASE("a pole cannot be dragged above the unit circle, however far up the hand goes") {
		// §3b's ASYMMETRY, ENFORCED WHERE THE GESTURE IS. The plane's ceiling is 2 so an allpass
		// mirror has somewhere to sit, which means the top half of it is a place a pole must not go.
		//
		// **TWO GUARDS HOLD THIS AND NEITHER CAN BE CAUGHT ALONE — MEASURED, not assumed.** `dragPole`
		// clamps to `kMaxPoleRadius`, and the `Pole Q` descriptor declares that as its range, so the
		// parameter layer clamps too. Removing EITHER leaves this case green; removing BOTH turns it
		// red. That is genuine defence in depth rather than dead code, and it is written here because
		// it is invisible from reading: a future simplification will look at one of them, see a bound
		// that cannot be exceeded, and delete it as free. CLAUDE.md's rule — where this codebase looks
		// redundant, find out what the redundancy is buying before removing it — and this is the
		// answer for this one.
		panel->selectFrame(0);
		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->poleCentre(0);
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(box.x() + box.width() * 0.5f, box.y() - 200.0f));
		panel->onMouseUp().callback(at(box.x() + box.width() * 0.5f, box.y() - 200.0f));
		f.bridge.refresh();
		const double radius = valueAt(dr::bandFieldIndex(0, 0, dr::PoleRadius));
		CAPTURE(radius);
		CHECK(radius <= static_cast<double>(dr::kMaxPoleRadius));
		CHECK(radius > 0.9);
		// AND THE DRAWING AGREES: the handle sits at or below the unit-circle line rather than off the
		// top of the plane.
		CHECK(panel->poleCentre(0).y >= box.y());
	}
}
TEST_CASE("a bypassed band stays bypassed when you move it, and its pole stays grabbable") {
	// TWO THINGS IN EXACTLY THE SAME PLACE, which is what `Bypass` is: the zero sits ON the pole.
	//
	// THE FIRST HALF IS A HIT-TESTING CLAIM. A point you cannot grab because something else is
	// precisely on top of it is the kind of thing that ships — the waypoint card's own case exists
	// because its first known-bad came back green — so the tie is resolved deliberately and asserted:
	// the POLE wins, because it is the handle that moves the band.
	//
	// THE SECOND IS A SOUND CLAIM. Dragging a band that is doing NOTHING must leave it doing nothing.
	// Without the zero following, moving a bypassed band leaves its zero behind and the band starts
	// filtering — an audible change from a gesture that should have been neutral.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();
	panel->selectFrame(0);

	const auto valueAt = [&](size_t index) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		return c.value();
	};
	const auto write = [&](size_t index, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		c.setValue(v);
	};

	// BAND 2 MADE EXACTLY BYPASSED — the zero on the pole, bit for bit.
	const size_t band = 2;
	write(dr::bandFieldIndex(0, band, dr::PoleRadius), 0.5);
	write(dr::bandFieldIndex(0, band, dr::PoleAngle), -6.0);
	write(dr::bandFieldIndex(0, band, dr::ZeroRadius), 0.5);
	write(dr::bandFieldIndex(0, band, dr::ZeroAngle), -6.0);
	f.bridge.refresh();

	SUBCASE("the two are drawn in the same place, and the pole wins the press") {
		const visage::Point pole = panel->poleCentre(band);
		const visage::Point zero = panel->zeroCentre(band);
		// THE FIXTURE IS DOING WHAT IT CLAIMS: they really are coincident, or this is a case about two
		// handles that happen to be far apart.
		REQUIRE(std::hypot(zero.x - pole.x, zero.y - pole.y) < 0.001f);
		const auto hit = panel->hitAt(pole);
		CHECK(hit.what == ui::DrawnExpanded::Grab::Pole);
		CHECK(hit.band == band);
	}

	SUBCASE("dragging it keeps the zero on the pole, so a band doing nothing goes on doing nothing") {
		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->poleCentre(band);
		const visage::Point to { box.x() + box.width() * 0.3f, box.bottom() - box.height() * 0.4f };
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(to.x, to.y));
		panel->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();

		// THE POLE MOVED...
		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::PoleAngle)) != doctest::Approx(-6.0));
		// ...AND THE ZERO IS STILL ON IT, which is the claim. Asserted as equality of the two stored
		// pairs rather than as "the response is flat", because flatness is what this BUYS and the
		// thing that must hold is the geometry.
		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::ZeroAngle))
		      == doctest::Approx(valueAt(dr::bandFieldIndex(0, band, dr::PoleAngle))));
		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::ZeroRadius))
		      == doctest::Approx(valueAt(dr::bandFieldIndex(0, band, dr::PoleRadius))));
	}

	SUBCASE("and a FREE band leaves the zero alone") {
		// THE CONTROL FOR THE SUBCASE ABOVE. Without it, "the zero followed" is indistinguishable from
		// "the editor writes the zero on every pole drag", which would make every named type collapse
		// into Bypass the moment a hand touched it.
		//
		// **WHAT MAKES A BAND LEAVE ITS ZERO ALONE IS ITS STORED TYPE NOW, NOT WHERE THE ZERO SITS.**
		// This subcase used to move the zero off the pole and rely on the coincidence latch; since
		// 2026-08-29 the type is state, so it sets `Free` — the one type that binds nothing. Moving
		// the zero is kept as well, because the control still wants the two points apart: a Free band
		// whose zero happened to sit on its pole would pass this by accident.
		//
		// AND THE OLD PREMISE IS NOW A DIFFERENT CLAIM ENTIRELY. A band left as `Bypass` with its zero
		// written elsewhere is an inconsistent state that only a direct parameter write can reach —
		// the editor will not produce it, because a bound zero is not draggable — and the next pole
		// drag REASSERTS the type, which is what "types stick" means. That is asserted on purpose in
		// "a stored type carries the zero, and Free lets it go".
		write(dr::bandFieldIndex(0, band, dr::TypeIndex),
		      static_cast<double>(static_cast<int>(dr::Type::Free)));
		write(dr::bandFieldIndex(0, band, dr::ZeroRadius), 1.0);
		write(dr::bandFieldIndex(0, band, dr::ZeroAngle), 0.0);
		f.bridge.refresh();
		const double zeroAngleBefore = valueAt(dr::bandFieldIndex(0, band, dr::ZeroAngle));
		const double zeroRadiusBefore = valueAt(dr::bandFieldIndex(0, band, dr::ZeroRadius));

		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->poleCentre(band);
		const visage::Point to { box.x() + box.width() * 0.6f, box.bottom() - box.height() * 0.2f };
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(to.x, to.y));
		panel->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();

		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::ZeroAngle)) == doctest::Approx(zeroAngleBefore));
		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::ZeroRadius)) == doctest::Approx(zeroRadiusBefore));
	}
}
TEST_CASE("the type menu writes the zero placement the type means, and Bandpass draws as two marks") {
	// THE RIGHT-CLICK MENU'S WHOLE JOB is to write a zero, so what is asserted is the VALUE it writes,
	// against `zeroPlacementFor` — the device's own answer — rather than against numbers typed here.
	// `applyType` is what the menu's selection handler calls, so this drives the same road a hand does
	// without needing a popup to be open.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();
	panel->selectFrame(1);

	const auto valueAt = [&](size_t index) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		return c.value();
	};
	const auto write = [&](size_t index, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		c.setValue(v);
	};

	const size_t band = 4;
	write(dr::bandFieldIndex(1, band, dr::PoleRadius), 0.8);
	write(dr::bandFieldIndex(1, band, dr::PoleAngle), -5.0);
	f.bridge.refresh();

	SUBCASE("every named type writes exactly what zeroPlacementFor says, and stores itself") {
		const dr::Point pole { 0.8f, -5.0f };
		for (size_t i = 0; i < std::size(dr::kTypeNames); ++i) {
			const auto type = static_cast<dr::Type>(i);
			CAPTURE(dr::kTypeNames[i]);
			panel->applyType(band, type);
			f.bridge.refresh();
			// **THE TYPE IS STORED, WHICHEVER IT IS.** Since 2026-08-29 this is the field the whole
			// feature rests on, and it is written for `Free` as well — which is the one that writes
			// no placement, so without this assertion nothing would say the menu had done anything
			// at all when a hand picked it.
			CHECK(valueAt(dr::bandFieldIndex(1, band, dr::TypeIndex))
			      == doctest::Approx(static_cast<double>(i)));
			const auto want = dr::zeroPlacementFor(type, pole);
			if (!want.has_value()) {
				// `Free` PLACES NOTHING AND MUST MOVE NOTHING. The zero stays wherever the previous
				// type left it — that is what releasing a constraint means, as against resetting it.
				CHECK(type == dr::Type::Free);
				continue;
			}
			CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius))
			      == doctest::Approx(static_cast<double>(want->radius)).epsilon(1e-5));
			CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle))
			      == doctest::Approx(static_cast<double>(want->angleLog2)).epsilon(1e-5));
			// AND THE POLE IS UNTOUCHED — a type places a ZERO, and one that moved the pole would be
			// re-deriving the band rather than typing it.
			CHECK(valueAt(dr::bandFieldIndex(1, band, dr::PoleRadius)) == doctest::Approx(0.8));
		}
	}

	SUBCASE("the menu offers EVERY type, Free included, because Free is the only way back out") {
		// **THIS SUBCASE USED TO ASSERT THE OPPOSITE** — `std::size(kTypeNames) == Type::Free`, on the
		// reasoning that *"a menu entry for it would change nothing at all". That was true while
		// nothing was stored: a band was then free exactly when its zero sat somewhere no preset would
		// have put it, so Free had nothing to write.
		//
		// IT IS FALSE THE MOMENT THE TYPE STICKS. Every other entry now BINDS the zero to the pole, so
		// Free is the only entry that unbinds it — Free releases the constraint — and a menu
		// that could enter six constraints and leave none of them would be the control that lies,
		// which is the objection the old comment raised, pointing the other way.
		CHECK(std::size(dr::kTypeNames) == static_cast<size_t>(dr::Type::kCount));
		// AND FREE STILL PLACES NOTHING, which is why it needed the stored field to mean anything.
		CHECK(dr::zeroPlacementFor(dr::Type::Free, dr::Point { 0.8f, -5.0f }).has_value() == false);
	}

	SUBCASE("a Bandpass draws as two marks at the ends of the spectrum, not as one point below zero") {
		// THE SPLIT-REAL BRANCH MADE VISIBLE. Its zero radius is NEGATIVE, which on a radius axis has
		// no position at all — a naive polar-to-screen would put it off the bottom of the plane, or
		// worse, somewhere plausible. There is no single point to draw because there is no single
		// zero: a bandpass's nulls are at DC and at Nyquist, which are the two ends of this axis.
		panel->applyType(band, dr::Type::Bandpass);
		f.bridge.refresh();
		REQUIRE(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius)) < 0.0);
		CHECK(panel->zeroIsSplitReal(band));

		const auto [left, right] = panel->splitZeroCentres(band);
		const visage::Bounds box = panel->planeBounds();
		CHECK(left.x == doctest::Approx(box.x()));
		CHECK(right.x == doctest::Approx(box.right()));
		// BOTH ON THE UNIT CIRCLE, which is where a bandpass's zeros are.
		CHECK(left.y == doctest::Approx(right.y));

		// AND IT IS NOT GRABBABLE AS ONE POINT, which is what stops a hand dragging a pair that has no
		// single position. The type menu is how a band gets out of this branch.
		CHECK(panel->hitAt(left).what != ui::DrawnExpanded::Grab::Zero);
		CHECK(panel->hitAt(right).what != ui::DrawnExpanded::Grab::Zero);
	}
}
TEST_CASE("the drawn editor's frame tabs choose which of the four curves the handles edit") {
	// THE PANEL EDITS ONE FRAME AT A TIME, so the tabs are the only way to reach the other three —
	// seventy-two of the ninety-six controls are behind them.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();

	const auto write = [&](size_t index, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		c.setValue(v);
	};
	// A DIFFERENT POLE ANGLE PER FRAME, so "the handles followed the tab" cannot pass by the four
	// frames agreeing.
	for (size_t frame = 0; frame < dr::kFrames; ++frame) {
		write(dr::bandFieldIndex(frame, 0, dr::PoleAngle), -10.0 + 2.0 * static_cast<double>(frame));
		write(dr::bandFieldIndex(frame, 0, dr::PoleRadius), 0.5);
	}
	f.bridge.refresh();

	CHECK(panel->selectedFrame() == 0);
	float previousX = panel->poleCentre(0).x;
	for (size_t frame = 1; frame < dr::kFrames; ++frame) {
		CAPTURE(frame);
		const visage::Bounds tab = panel->tabBounds(frame);
		// PRESSED AT THE TAB'S OWN RECTANGLE, so the hit test is asserted rather than the setter.
		panel->onMouseDown().callback(at(tab.x() + tab.width() * 0.5f, tab.y() + tab.height() * 0.5f));
		panel->onMouseUp().callback(at(tab.x() + tab.width() * 0.5f, tab.y() + tab.height() * 0.5f));
		CHECK(panel->selectedFrame() == frame);
		// AND THE HANDLES MOVED WITH IT — a selector that changed a number and not the drawing would
		// pass the line above and edit the wrong frame.
		const float x = panel->poleCentre(0).x;
		CHECK(x > previousX);
		previousX = x;
	}

	// THE TABS DO NOT OVERLAP, or a press lands on two of them and the one that answers is whichever
	// the loop happens to test first.
	for (size_t a = 0; a + 1 < dr::kFrames; ++a) {
		CHECK(panel->tabBounds(a).right() <= panel->tabBounds(a + 1).x());
	}
}
TEST_CASE("the waypoint filter's expanded view exists in BOTH halves, and shows every waypoint") {
	// THE FAILURE THIS GUARDS IS OWLTT'S, EXACTLY: no expanded view was available.
	// OWLTT declared `hasExpanded`, an arm existed, and no button appeared — because the answer came
	// from a SECOND list that named only HARM. `hasExpandedView` is
	// `traits.hasExpanded && entryFor(desc) != nullptr`, so this asserts the conjunction rather than
	// either half: a device that declares without an arm, or grows an arm without declaring, fails
	// here instead of failing in the user's hands.
	namespace wp = feathers::fx::waypoint;
	FilterFixture f;
	ui::WidgetRegistry registry;

	CHECK(wp::kTraits.hasExpanded);
	CHECK(ui::hasExpandedView(wp::kDescriptor));

	// AND THE ARM REALLY BUILDS ONE — `hasExpandedView` answering true while `makeExpandedView`
	// returns null is the same disagreement one layer down.
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(1), wp::kDescriptor,
	                                 nullptr);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::WaypointExpanded*>(view.get());
	REQUIRE(panel != nullptr);

	SUBCASE("it names itself from the descriptor, not from a string typed here") {
		CHECK(panel->title() == std::string(wp::kTraits.name));
	}

	SUBCASE("it fits the editor, and asks for no canvas it does not fill") {
		// THE OVERLAY CLAMPS RATHER THAN SCROLLING, so a panel asking for more than it fills leaves
		// blank canvas — the "lots of blank space in the pop-out canvas" defect — and one asking for less
		// has its bottom row silently cut off.
		const visage::Point want = panel->minimumSize();
		CAPTURE(want.x);
		CAPTURE(want.y);
		CHECK(want.x <= static_cast<float>(Gui::kLogicalWidth));
		CHECK(want.y <= static_cast<float>(Gui::kLogicalHeight));

		panel->setBounds(0.0f, 0.0f, want.x, want.y);
		panel->resized();
		// EVERY KNOB INSIDE THE EXTENT IT ASKED FOR, which the height alone does not imply: a budget
		// computed right and a layout that walked wrong would still clip.
		const auto& knobs = panel->knobsForTest();
		REQUIRE(!knobs.empty());
		float lowest = 0.0f;
		for (const auto& knob : knobs) {
			CHECK(knob->bounds().bottom() <= want.y);
			CHECK(knob->bounds().right() <= want.x + 0.01f);
			lowest = std::max(lowest, knob->bounds().bottom());
		}
		// AND IT FILLS WHAT IT ASKED FOR — the bottom row reaches the bottom, or the panel is
		// reserving canvas nobody uses. This is the half that catches over-asking rather than
		// clipping.
		CHECK(lowest > want.y - 40.0f);
	}

	SUBCASE("EVERY per-waypoint control is on it at once, which is why it is worth opening") {
		// THE CARD SHOWS ONE WAYPOINT'S MACROS because a knob row has three cells and there are four
		// waypoints. THE PANEL SHOWS ALL SIXTEEN — a different job rather than a bigger version of the
		// same one, which is the test device.h sets for declaring hasExpanded at all.
		const std::vector<wp::Param> reached = panel->addressedParamsForTest();
		const std::set<size_t> seen = [&] {
			std::set<size_t> out;
			for (const wp::Param p : reached) {
				out.insert(static_cast<size_t>(p));
			}
			return out;
		}();
		for (size_t w = 0; w < wp::kWaypoints; ++w) {
			for (size_t field = 0; field < 4; ++field) {
				const size_t i = static_cast<size_t>(wp::Param::Cutoff0) + w * 4 + field;
				CAPTURE(w);
				CAPTURE(field);
				CHECK(seen.count(i) == 1);
			}
		}
		// THE DEVICE-WIDE FOUR ARE HERE TOO.
		CHECK(seen.count(static_cast<size_t>(wp::Param::Morph)) == 1);
		CHECK(seen.count(static_cast<size_t>(wp::Param::Type)) == 1);
		// AND IT IS NOT ANSWERING YES TO EVERYTHING: `Enabled` is the card's head square and is NOT on
		// this panel, so a `reached` set that contained it would be a list nobody checked.
		CHECK(seen.count(static_cast<size_t>(wp::Param::Enabled)) == 0);
	}
}
TEST_CASE("two filters in one chain are two devices, not one device drawn twice") {
	// THE BUG THIS PINS: two filters running at once mirrored each other's parameters instead of
	// having their own. FilterCard was the ONE card whose constructor never
	// took its slot -- HarmCard, OwlttCard, WaypointCard and GenericCard all take it, and HARM's
	// call site in FxLane::ensureCard even carries the comment naming this exact failure -- so its
	// `slot_` sat at its declared 0 and was assigned nowhere. Its two readers are precisely the
	// write path (the FxControl it builds) and the read path (the row it draws), so both cards
	// wrote row 0 and both drew row 0.
	//
	// AND THE SECOND FILTER IS NOT THE ONLY VICTIM: a single filter anywhere but slot 0 was editing
	// slot 0 too. The subcases ask both.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(lane != nullptr);
	REQUIRE(lane->deviceCount() == 1);

	const size_t cutoff = static_cast<size_t>(fx::filter::Param::Cutoff);

	SUBCASE("each card is the face of its own slot") {
		const size_t second = f.bridge.addFxDevice(fx::filter::kTraits.tag);
		REQUIRE(second == 1);
		lane->resized();

		auto* c0 = dynamic_cast<ui::FilterCard*>(lane->cardAt(0));
		auto* c1 = dynamic_cast<ui::FilterCard*>(lane->cardAt(second));
		REQUIRE(c0 != nullptr);
		REQUIRE(c1 != nullptr);
		// TWO CARDS, not one shown twice -- the cheap half, and it passed even when broken.
		CHECK(c0 != c1);
		// THE HALF THAT WAS FALSE: the identity each card carries.
		CHECK(c0->deviceForTest() == f.bridge.fxDeviceAt(0));
		CHECK(c1->deviceForTest() == f.bridge.fxDeviceAt(second));
		CHECK(c0->deviceForTest() != c1->deviceForTest());
	}

	SUBCASE("moving one filter's cutoff leaves the other's alone") {
		const size_t second = f.bridge.addFxDevice(fx::filter::kTraits.tag);
		REQUIRE(second == 1);
		lane->resized();
		auto* c1 = dynamic_cast<ui::FilterCard*>(lane->cardAt(second));
		REQUIRE(c1 != nullptr);

		const float before0 = f.bridge.fxChain().device[0].row.field[cutoff];
		const float before1 = f.bridge.fxChain().device[second].row.field[cutoff];
		// They start equal, which is exactly why a difference check alone would be worthless here.
		REQUIRE(before0 == doctest::Approx(before1));

		c1->setControlForTest(fx::filter::Param::Cutoff, 7.0);

		const float after0 = f.bridge.fxChain().device[0].row.field[cutoff];
		const float after1 = f.bridge.fxChain().device[second].row.field[cutoff];
		// THE WRITE LANDED ON THE SECOND DEVICE...
		CHECK(after1 != doctest::Approx(before1));
		// ...AND NOWHERE ELSE. This is the assertion that was failing: before the fix the two rows
		// moved together, because both cards addressed row 0.
		CHECK(after0 == doctest::Approx(before0));
	}

	SUBCASE("a card draws the row it is the face of") {
		const size_t second = f.bridge.addFxDevice(fx::filter::kTraits.tag);
		REQUIRE(second == 1);
		lane->resized();
		auto* c0 = dynamic_cast<ui::FilterCard*>(lane->cardAt(0));
		auto* c1 = dynamic_cast<ui::FilterCard*>(lane->cardAt(second));
		REQUIRE(c0 != nullptr);
		REQUIRE(c1 != nullptr);

		// Drive the two devices APART through the bridge, so neither card can be right by accident.
		// BY DEVICE, not by the place it stands: a card is the face of a DEVICE, which is exactly the
		// claim below, so the write that sets it up names the same thing the assertion does.
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(0), cutoff, 6.0f);
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(second), cutoff, 10.0f);

		// THE VALUE, not a render comparison: each card reports the cutoff of ITS OWN row.
		CHECK(c0->settingsForTest().cutoffLog2Hz
		      == doctest::Approx(fxFilterAt(f.bridge.fxChain(), 0).settings.cutoffLog2Hz));
		CHECK(c1->settingsForTest().cutoffLog2Hz
		      == doctest::Approx(fxFilterAt(f.bridge.fxChain(), second).settings.cutoffLog2Hz));
		// And the two cards genuinely disagree, which proves the assertions above are not vacuous.
		CHECK(c0->settingsForTest().cutoffLog2Hz != doctest::Approx(c1->settingsForTest().cutoffLog2Hz));
	}

	SUBCASE("a lone filter that is not in slot 0 still edits itself") {
		// THE WIDER HALF OF THE SAME BUG. Nothing about it needs two filters -- it needs a filter
		// whose slot is not the one the card assumed.
		REQUIRE(f.lane->removeDeviceAtLane(0));
		const size_t harm = f.bridge.addFxDevice(fx::harm::kTraits.tag);
		const size_t only = f.bridge.addFxDevice(fx::filter::kTraits.tag);
		REQUIRE(harm == 0);
		REQUIRE(only == 1);
		lane->resized();

		auto* card = dynamic_cast<ui::FilterCard*>(lane->cardAt(only));
		REQUIRE(card != nullptr);
		CHECK(card->deviceForTest() == f.bridge.fxDeviceAt(only));
		const float before = f.bridge.fxChain().device[only].row.field[cutoff];
		card->setControlForTest(fx::filter::Param::Cutoff, 7.0);
		CHECK(f.bridge.fxChain().device[only].row.field[cutoff] != doctest::Approx(before));
	}
}
TEST_CASE("a SECOND card in the lane answers a real click, and it reaches its OWN device") {
	// The symptom: only the first effect in the chain seemed to process audio. The engine was
	// cleared first, by measurement rather than by reading: `provision` builds every slot, `sync`
	// hands each device its own row (slot 0 cutoff 6.0 and slot 1 cutoff 11.0 arrive distinct at
	// `setFields`), `processStereo` visits every slot, and a mixed chain runs slot 0 on the bus
	// runner and slot 1 on the voice runner. None of that is the bug.
	//
	// WHAT WAS NEVER ASKED is whether a card past the first can be CLICKED where the layout puts
	// it. Every card case in this file drives slot 0. That is exactly the gap this file's own
	// header warns about — "a knob can pass every test it has and still be unreachable where the
	// layout puts it" — and from a player's chair an unreachable enable switch and a device that
	// does not process are the same complaint.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);

	const size_t second = f.bridge.addFxDevice(fx::filter::kTraits.tag);
	REQUIRE(second == 1);
	lane->resized();
	lane->scrollCardIntoView(second);
	lane->resized();

	auto* card = dynamic_cast<ui::FilterCard*>(lane->cardAt(second));
	REQUIRE(card != nullptr);
	REQUIRE(card->deviceForTest() == f.bridge.fxDeviceAt(second));
	REQUIRE(card->isVisible());

	const auto settle = [&](ParamSet* params) {
		f.bridge.drain([&](const UiParamEvent& event) {
			if (event.kind == UiParamEvent::Kind::Value) {
				params->setBase(event.id, event.value);
			}
		});
		f.bridge.publish(*params);
		f.bridge.refresh();
	};
	ParamSet params;
	const auto enabledAt = [&](size_t slot) {
		return fxFilterAt(f.bridge.fxChain(), slot).settings.enabled;
	};

	const visage::Bounds b = card->enableBounds();
	const visage::Point enableAt =
	    card->positionInWindow() + visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };

	// THE CLICK LANDS ON THE SECOND CARD, not through it and not on its neighbour. This is the half
	// that a slot-0-only suite cannot ask at all.
	REQUIRE(f.root->frameAtPoint(enableAt) == card);

	// **THE CLAIM IS THAT THE CLICK FLIPS THIS CARD'S SWITCH, WHICHEVER WAY IT WAS POINTING.** This
	// read `REQUIRE_FALSE(enabledAt(second))` and then checked for true, which pinned the case to the
	// device's arrival state as well as to its behaviour — and that state moved on 2026-09-04, when
	// every effect became on by default. Reading the before-value and asserting the flip is the
	// same claim with one fewer thing to go stale, and it keeps the second card's reachability — the
	// actual subject — testable from either side.
	const bool firstBefore = enabledAt(0);
	const bool secondBefore = enabledAt(second);
	dragThroughRoot(f.root, enableAt, enableAt);
	settle(&params);

	// IT REACHED ITS OWN DEVICE...
	CHECK(enabledAt(second) == !secondBefore);
	// ...AND NOBODY ELSE'S. The two rows start identical, so without this the case would pass on a
	// card that wrote row 0 — which is precisely the bug that shipped in this file's neighbour.
	CHECK(enabledAt(0) == firstBefore);
}

