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

// PIECE 2 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

TEST_CASE("the envelope card marks which stage is running, and declines the looping picture") {
	// **A DRAWN POSITION IS NOT OBSERVABLE HEADLESSLY**, so the marker's coordinates are exposed and
	// asserted against the card's OWN stage table — `stageEndX`, which is what the drag and the hit
	// test already resolve a press with. A marker placed from anything else could disagree with the
	// region a press lands in, which is this surface's classic defect shape.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace dsr = feathers::mod::dahdsr;
	namespace md = feathers::mod;

	feathers::ModRack rack = f.bridge.modRack();
	dsr::Settings env;
	// FOUR STAGES OF DIFFERENT WIDTHS. Equal ones would let an off-by-one in the stage land somewhere
	// plausible and the case would agree with it.
	env.delay = static_cast<float>(std::log2(0.02));
	env.attack = static_cast<float>(std::log2(0.08));
	env.hold = static_cast<float>(std::log2(0.03));
	env.decay = static_cast<float>(std::log2(0.12));
	env.sustain = 0.5f;
	env.release = static_cast<float>(std::log2(0.06));
	env.mode = md::Mode::Gate;
	rack.count = std::max<size_t>(rack.count, 1);
	rack.setDahdsrAt(0, env);
	f.bridge.restoreModRack(rack);

	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	SUBCASE("an envelope that is not running is not marked") {
		f.bridge.publishRackPlayhead(0, md::Playhead {});
		CHECK_FALSE(card.marker().drawn);
	}

	SUBCASE("each stage's mark lands inside that stage's own span") {
		// **THE INDEPENDENT RESTATEMENT**: a stage begins where its predecessor ended, and the card
		// says where that is. Asserted for every stage rather than one, because the mapping walks an
		// order and an order is exactly what an off-by-one hides in.
		const std::array<dsr::Stage, 6> order { dsr::Stage::Delay,   dsr::Stage::Attack,
			                                    dsr::Stage::Hold,    dsr::Stage::Decay,
			                                    dsr::Stage::Sustain, dsr::Stage::Release };
		const visage::Bounds box = card.curveBounds();
		for (size_t i = 0; i < order.size(); ++i) {
			CAPTURE(i);
			const float from = (i == 0) ? 0.0f : card.stageEndX(order[i - 1]);
			const float to = card.stageEndX(order[i]);
			if (!(to > from)) {
				continue; // a stage with no width of its own — nothing to land in
			}
			f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(order[i]), 0.5f });
			const ui::EnvCard::Marker mark = card.marker();
			REQUIRE(mark.drawn);
			const float midUnit = from + (to - from) * 0.5f;
			CHECK(mark.at.x == doctest::Approx(box.x() + midUnit * box.width()).epsilon(1e-4));
		}
	}

	SUBCASE("inside a stage the mark is ON THE CHORD the card stroked") {
		f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(dsr::Stage::Decay),
		                                              0.4f });
		const ui::EnvCard::Marker mark = card.marker();
		REQUIRE(mark.drawn);

		const float from = card.stageEndX(dsr::Stage::Hold);
		const float to = card.stageEndX(dsr::Stage::Decay);
		const float unitX = from + (to - from) * 0.4f;
		const std::vector<visage::Point> pts = card.curvePoints();
		REQUIRE(pts.size() > 8);
		size_t hi = 1;
		while (hi + 1 < pts.size() && pts[hi].x < unitX) {
			++hi;
		}
		const float span = pts[hi].x - pts[hi - 1].x;
		const float frac = span > 0.0f ? (unitX - pts[hi - 1].x) / span : 0.0f;
		const visage::Bounds box = card.curveBounds();
		const auto plot = [&](const visage::Point& p) {
			return visage::Point { box.x() + std::clamp(p.x, 0.0f, 1.0f) * box.width(),
				                   box.bottom()
				                       - ui::EnvCard::plotUnit(p.y, false) * box.height() };
		};
		const visage::Point a = plot(pts[hi - 1]);
		const visage::Point b = plot(pts[hi]);
		CHECK(mark.at.x == doctest::Approx(a.x + (b.x - a.x) * frac).epsilon(1e-4));
		CHECK(mark.at.y == doctest::Approx(a.y + (b.y - a.y) * frac).epsilon(1e-4));
	}

	SUBCASE("a release from below the sustain is marked at the level the envelope PUBLISHES, not on the drawn release (it \"jumps all the way to the sustain level before releasing\")") {
		// The drawn release starts at the sustain (0.5); an envelope let go a quarter of the way up its attack
		// releases from 0.25. MEASURED before: the star stood at the drawn line's 0.5, then slid down from there.
		const visage::Bounds box = card.curveBounds();
		const auto yOf = [&](float level) {
			return box.bottom() - ui::EnvCard::plotUnit(level, false) * box.height();
		};
		for (const float phase : { 0.0f, 0.5f }) {
			CAPTURE(phase);
			md::Playhead where { static_cast<uint32_t>(dsr::Stage::Release), phase };
			where.level = 0.25f * (1.0f - phase); // what a linear release from 0.25 stands at
			where.haveLevel = true;
			f.bridge.publishRackPlayhead(0, where);
			const ui::EnvCard::Marker mark = card.marker();
			REQUIRE(mark.drawn);
			const float from = card.stageEndX(dsr::Stage::Sustain);
			const float to = card.stageEndX(dsr::Stage::Release);
			CHECK(mark.at.x == doctest::Approx(box.x() + (from + (to - from) * phase) * box.width()).epsilon(1e-4));
			CHECK(mark.at.y == doctest::Approx(yOf(where.level)).epsilon(1e-4));
		}
		// AND A RE-ATTACK FROM WHERE IT STOOD — the drawn attack starts at zero.
		md::Playhead attack { static_cast<uint32_t>(dsr::Stage::Attack), 0.1f };
		attack.level = 0.6f;
		attack.haveLevel = true;
		f.bridge.publishRackPlayhead(0, attack);
		CHECK(card.marker().at.y == doctest::Approx(yOf(0.6f)).epsilon(1e-4));
		// NOT VACUOUS: with no level published the star is on the drawn release, which starts at the sustain.
		f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(dsr::Stage::Release), 0.0f });
		CHECK(card.marker().at.y == doctest::Approx(yOf(0.5f)).epsilon(1e-3));
	}

	SUBCASE("it MOVES across the box, and it is the playhead that moves it") {
		std::vector<float> xs;
		for (const dsr::Stage st : { dsr::Stage::Delay, dsr::Stage::Attack, dsr::Stage::Decay,
		                             dsr::Stage::Release }) {
			f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(st), 0.5f });
			const ui::EnvCard::Marker mark = card.marker();
			REQUIRE(mark.drawn);
			xs.push_back(mark.at.x);
		}
		for (size_t i = 1; i < xs.size(); ++i) {
			CAPTURE(i);
			CHECK(xs[i] > xs[i - 1]);
		}
		// AND THE Y IS NOT CONSTANT — a mark sliding along a horizontal line would satisfy every x
		// check above and be wrong about the envelope.
		f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(dsr::Stage::Attack),
		                                              0.05f });
		const float lowY = card.marker().at.y;
		f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(dsr::Stage::Hold),
		                                              0.5f });
		const float highY = card.marker().at.y;
		CHECK(lowY != doctest::Approx(highY));
	}

	SUBCASE("a LOOPING envelope is not marked at all, and that is a decline") {
		// The picture is TWO cycles, so one position has two candidate x's. `stageEndX_` records the
		// second, so a naive marker would silently always sit on the second cycle — defensible, and
		// chosen by nobody. Until that choice is made deliberately this draws nothing rather than
		// something arbitrary, and the decline is pinned so it cannot be lost by accident.
		feathers::ModRack looping = f.bridge.modRack();
		dsr::Settings withLoop = env;
		withLoop.mode = md::Mode::Loop;
		looping.setDahdsrAt(0, withLoop);
		f.bridge.restoreModRack(looping);
		f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(dsr::Stage::Decay),
		                                              0.5f });
		CHECK_FALSE(card.marker().drawn);

		// AND IT COMES BACK when the loop goes away — a decline, not a dead branch.
		looping.setDahdsrAt(0, env);
		f.bridge.restoreModRack(looping);
		CHECK(card.marker().drawn);
	}

	SUBCASE("it asks for a frame only when the playhead moved") {
		f.bridge.publishRackPlayhead(0, md::Playhead { 3, 0.25f });
		(void)card.update(); // settle
		CHECK_FALSE(card.update());
		f.bridge.publishRackPlayhead(0, md::Playhead { 3, 0.75f });
		CHECK(card.update());
		CHECK_FALSE(card.update());
		// THE ORDINAL IS PART OF THE COMPARISON: same phase, different stage, must still ask.
		f.bridge.publishRackPlayhead(0, md::Playhead { 4, 0.75f });
		CHECK(card.update());
		CHECK_FALSE(card.update());
	}
}
TEST_CASE("the MSEG card marks where the shape is, on the line it actually drew") {
	// **A DRAWN POSITION IS NOT OBSERVABLE HEADLESSLY**, so the marker's coordinates are exposed and
	// asserted against an independent restatement of the geometry the card draws from — not against a
	// second render, which passes whenever anything differs.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;
	namespace md = feathers::mod;

	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	// THREE SEGMENTS OF DIFFERENT LENGTHS AND DIFFERENT LEVELS. Equal ones would let an off-by-one in
	// the ordinal land somewhere plausible, and a flat shape would make every y agree.
	shape.segment[0].share = feathers::mod::timeValueFor(0.1);
	shape.segment[0].level = 1.0f;
	shape.segment[1].share = feathers::mod::timeValueFor(0.4);
	shape.segment[1].level = 0.2f;
	shape.segment[2].share = feathers::mod::timeValueFor(0.2);
	shape.segment[2].level = 0.8f;
	// **AND THE SEGMENTS ARE BENT, WHICH IS LOAD-BEARING RATHER THAN DECORATION.** With `shape` at
	// its default of 0 every segment is a straight line, the polyline's chords lie exactly on the
	// curve, and "the mark is on the CHORD" agrees with "the mark is on the curve" — so the subcase
	// below would pass against either and could not tell them apart. A bent segment separates them.
	// The fixture-bypasses-the-device shape, met where a DEFAULT rather than a device is the thing
	// that is doing nothing.
	shape.segment[0].shape = 0.8f;
	shape.segment[1].shape = -0.7f;
	shape.segment[2].shape = 0.6f;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);
	REQUIRE(f.bridge.modRack().holds(0, mseg::kTraits.tag));

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	const mseg::Layout laid = card.layoutNow();
	REQUIRE(laid.segmentEnd.size() == 3);

	SUBCASE("a shape that is not running is not marked at all") {
		// The honesty rule as ink: a finished shape is NOWHERE, not parked at its end, and a marker
		// drawn there would be saying something the device is not doing.
		f.bridge.publishRackPlayhead(0, md::Playhead {});
		CHECK_FALSE(card.marker().drawn);
	}

	SUBCASE("at a segment's start the mark is at that segment's left edge") {
		// **THE INDEPENDENT RESTATEMENT**, from the geometry the card draws its own segment bands and
		// loop markers from: segment 0 starts at the left edge; every later one starts where its
		// predecessor ended.
		for (size_t seg = 0; seg < 3; ++seg) {
			CAPTURE(seg);
			f.bridge.publishRackPlayhead(0, md::Playhead { static_cast<uint32_t>(seg), 0.0f });
			const ui::MsegCard::Marker mark = card.marker();
			REQUIRE(mark.drawn);
			const float unitX = (seg == 0) ? 0.0f : laid.segmentEnd[seg - 1];
			CHECK(mark.at.x == doctest::Approx(card.fromUnitForTest(unitX, 0.0f).x).epsilon(1e-4));
		}
	}

	SUBCASE("inside a segment it is ON THE CHORD the card stroked") {
		// **WHY THE MARKER INTERPOLATES IN SCREEN SPACE.** The shape is drawn as straight chords
		// between polyline points; evaluating the segment afresh would put the mark off the drawn line
		// by however much the sampling rounded a bend — worst exactly where the shape bends, which is
		// where a reader is looking. So the expectation is built from the SAME polyline the card
		// strokes, and it is a point on one of its chords.
		f.bridge.publishRackPlayhead(0, md::Playhead { 1, 0.5f });
		const ui::MsegCard::Marker mark = card.marker();
		REQUIRE(mark.drawn);

		const float unitX = laid.segmentEnd[0] + (laid.segmentEnd[1] - laid.segmentEnd[0]) * 0.5f;
		const std::vector<std::pair<float, float>> pts = card.curve();
		REQUIRE(pts.size() > 8);
		size_t hi = 1;
		while (hi + 1 < pts.size() && pts[hi].first < unitX) {
			++hi;
		}
		const float span = pts[hi].first - pts[hi - 1].first;
		const float frac = span > 0.0f ? (unitX - pts[hi - 1].first) / span : 0.0f;
		const visage::Point a = card.fromUnitForTest(pts[hi - 1].first, pts[hi - 1].second);
		const visage::Point b = card.fromUnitForTest(pts[hi].first, pts[hi].second);
		CHECK(mark.at.x == doctest::Approx(a.x + (b.x - a.x) * frac).epsilon(1e-4));
		CHECK(mark.at.y == doctest::Approx(a.y + (b.y - a.y) * frac).epsilon(1e-4));
	}

	SUBCASE("it MOVES, left to right, and it is the playhead that moves it") {
		// NON-VACUITY IN THE DIRECTION THAT MATTERS: a mark pinned at one x satisfies every check
		// above at whichever position happens to be published.
		std::vector<float> xs;
		for (const md::Playhead& at : { md::Playhead { 0, 0.0f }, md::Playhead { 0, 0.5f },
		                                md::Playhead { 1, 0.5f }, md::Playhead { 2, 0.9f } }) {
			f.bridge.publishRackPlayhead(0, at);
			const ui::MsegCard::Marker mark = card.marker();
			REQUIRE(mark.drawn);
			xs.push_back(mark.at.x);
		}
		for (size_t i = 1; i < xs.size(); ++i) {
			CAPTURE(i);
			CHECK(xs[i] > xs[i - 1]);
		}
		// AND THE Y IS NOT CONSTANT EITHER — a mark that slid along a horizontal line would satisfy
		// the x checks and be wrong about the shape.
		f.bridge.publishRackPlayhead(0, md::Playhead { 0, 0.9f });
		const float highY = card.marker().at.y;
		f.bridge.publishRackPlayhead(0, md::Playhead { 1, 0.9f });
		const float lowY = card.marker().at.y;
		CHECK(highY != doctest::Approx(lowY));
	}

	SUBCASE("the panel's own walk reaches it, which is the road out of the editor's tick") {
		// **NAME THE CONSUMER.** A card's `update()` that nothing calls is a capability with no
		// caller, and the mod rack had no such walk at all until this work — so the walk is asserted
		// through the REAL panel in the real tree rather than by calling the card directly.
		ModFixture panelFixture;
		REQUIRE(panelFixture.panel != nullptr);
		feathers::ModRack pr = panelFixture.bridge.modRack();
		pr.count = std::max<size_t>(pr.count, 1);
		pr.setMsegAt(0, shape);
		panelFixture.bridge.restoreModRack(pr);
		// **`resized()` IS WHAT REBUILDS THE CARD, and leaving it out is how this subcase first ran
		// against an `EnvCard` while claiming to be about the MSEG's.** Restoring a rack changes what
		// the slot HOLDS; it does not rebuild the frame that draws it.
		panelFixture.panel->resized();

		// AND THE CARD IS THE ONE THIS SUBCASE IS ABOUT, asserted rather than assumed. Without this
		// the walk below would go green the day `EnvCard` gains an `update()` of its own — reporting
		// a road it never travelled.
		REQUIRE(dynamic_cast<const ui::MsegCard*>(panelFixture.panel->cardFrameForTest(0)) != nullptr);

		panelFixture.bridge.publishRackPlayhead(0, md::Playhead { 1, 0.2f });
		(void)panelFixture.panel->updateCards(); // settle
		CHECK(panelFixture.panel->updateCards() == 0);

		panelFixture.bridge.publishRackPlayhead(0, md::Playhead { 1, 0.6f });
		CHECK(panelFixture.panel->updateCards() >= 1);
		// AND ONCE, not once per ask — the count is what `Gui::Impl::tick()` adds to `deviceFrames`.
		CHECK(panelFixture.panel->updateCards() == 0);
	}

	SUBCASE("it asks for a frame only when the playhead moved") {
		// CONTENT-GATED, which is what makes a live card cost nothing while nothing is playing.
		f.bridge.publishRackPlayhead(0, md::Playhead { 1, 0.25f });
		(void)card.update(); // settle
		CHECK_FALSE(card.update());
		CHECK_FALSE(card.update());

		f.bridge.publishRackPlayhead(0, md::Playhead { 1, 0.50f });
		CHECK(card.update());
		CHECK_FALSE(card.update());

		// **THE ORDINAL IS PART OF THE COMPARISON, and this is the subcase that proves it.** A shape
		// stepping from one segment to the next can land on a phase it has just had; a gate that
		// compared only the phase would sit on a stale segment for as long as the numbers agreed.
		f.bridge.publishRackPlayhead(0, md::Playhead { 2, 0.50f });
		CHECK(card.update());
		CHECK_FALSE(card.update());
	}
}
TEST_CASE("the MSEG card's editor drags a breakpoint into the settings the engine reads") {
	// THE CARD'S WHOLE JOB. An MSEG's curve is not a readout — it is the control — so the claim worth
	// asserting is that a drag on the picture arrives as a NUMBER in the slot, not that the picture
	// changed. A comparison of two renders would pass on any drag that moved any pixel.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	// Put an MSEG in the slot first — the rack holds a device now, and it defaults to an envelope.
	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	shape.segment[0].share = feathers::mod::timeValueFor(0.2);
	shape.segment[0].level = 1.0f;
	shape.segment[1].share = feathers::mod::timeValueFor(0.2);
	shape.segment[1].level = 0.4f;
	shape.segment[2].share = feathers::mod::timeValueFor(0.2);
	shape.segment[2].level = 0.0f;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);
	REQUIRE(f.bridge.modRack().holds(0, mseg::kTraits.tag));

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	// A handle per segment END, plus one at the start.
	const mseg::Layout laid = card.layoutNow();
	REQUIRE(laid.handle.size() == 4);

	// GRAB THE ONE AT THE TOP OF THE FIRST SEGMENT — the peak — and pull it DOWN.
	const mseg::Handle& peak = laid.handle[1];
	CHECK(peak.level == doctest::Approx(1.0f));

	const float beforeLevel = f.bridge.modRack().msegAt(0).segment[0].level;
	const visage::Bounds box = card.editorBounds();
	const visage::Point grab { box.x() + peak.x * box.width(),
	                           box.bottom() - peak.level * box.height() };

	// The hit test the mouse uses, driven the way a hand drives it.
	CHECK(card.handleAtCardPoint(grab) == 1);

	card.onMouseDown().callback(at(grab.x, grab.y));
	card.onMouseDrag().callback(at(grab.x, box.bottom() - 0.5f * box.height()));
	card.onMouseUp().callback(at(grab.x, box.bottom() - 0.5f * box.height()));

	// THE VALUE, IN THE SLOT. Halfway up the box is level 0.5 for a unipolar shape.
	const float afterLevel = f.bridge.modRack().msegAt(0).segment[0].level;
	CAPTURE(beforeLevel);
	CAPTURE(afterLevel);
	CHECK(beforeLevel == doctest::Approx(1.0f));
	CHECK(afterLevel == doctest::Approx(0.5f).epsilon(0.05));

	SUBCASE("and the gesture is ONE undo step, which puts the whole shape back") {
		history.undo();
		CHECK(f.bridge.modRack().msegAt(0).segment[0].level == doctest::Approx(1.0f));
		history.redo();
		CHECK(f.bridge.modRack().msegAt(0).segment[0].level == doctest::Approx(0.5f).epsilon(0.05));
	}

	SUBCASE("and the slot still holds an MSEG afterwards") {
		// The write road stamps the device's own tag from its own declarations. A card that assembled
		// the row itself could stamp the wrong one onto the right numbers, and the slot would reload
		// as the other device — which is why setMsegSettings takes Settings rather than a DeviceRow.
		CHECK(f.bridge.modRack().holds(0, mseg::kTraits.tag));
	}
}
TEST_CASE("dragging the sustain marker moves the breakpoint the gate holds at") {
	// The sustain marker must be draggable to a different breakpoint. The marker was drawn and
	// nothing moved it — the reachability guard's own known gap, closed in the same commit as this.
	//
	// THE VALUE IN THE SLOT, for the reason every gesture case on this card gives: the picture carries
	// every index at once and cannot say which number the drag wrote.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;
	namespace md = feathers::mod;

	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
	}
	// EVERY BREAKPOINT HIGH IN THE BOX, ON PURPOSE. A marker is a full-height line and a handle is a
	// 13px dot that wins the mouse-down, so a case about the marker has to aim somewhere no handle
	// is. Keeping every level at or above 0.8 leaves the bottom twentieth of the box free — the
	// nearest handle is then 4.1 grab-radii away vertically, which is what makes the grabs below
	// about the marker rather than about which control happened to be nearer.
	shape.startLevel = 1.0f;
	shape.segment[0].level = 0.8f;
	shape.segment[1].level = 1.0f;
	shape.segment[2].level = 0.9f;
	shape.mode = md::Mode::Gate;
	shape.sustainSegment = 1;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	const mseg::Layout laid = card.layoutNow();
	REQUIRE(laid.sustainVisible);
	REQUIRE(laid.sustainActive);
	const visage::Bounds box = card.editorBounds();
	// AIMED AWAY FROM THE BREAKPOINTS' OWN HEIGHTS. The marker is a full-height line and the handles
	// are 13px dots that win the mouse-down; picking a y between the levels is how a hand reaches the
	// line, and is what the card's comment about 13px against 72px is describing.
	const float lowY = box.bottom() - box.height() * 0.05f;

	// Grab it where it stands — the end of segment 1 — and drag left to the end of segment 0.
	const float from = box.x() + laid.sustainX * box.width();
	card.onMouseDown().callback(at(from, lowY));
	card.onMouseDrag().callback(at(box.x() + 0.30f * box.width(), lowY));
	card.onMouseUp().callback(at(box.x() + 0.30f * box.width(), lowY));

	const mseg::Settings after = f.bridge.modRack().msegAt(0);
	CAPTURE(after.sustainSegment);
	CHECK(after.sustainSegment == 0);

	// AND THE SHAPE ITSELF DID NOT MOVE. One gesture, one edit — the same claim the loop-marker case
	// above makes, and the reason a marker drag is not allowed to be a handle drag that missed.
	CHECK(after.startLevel == doctest::Approx(1.0f));
	CHECK(after.segment[0].level == doctest::Approx(0.8f));
	CHECK(after.segment[1].level == doctest::Approx(1.0f));
	CHECK(after.segment[2].level == doctest::Approx(0.9f));
	CHECK(after.segment[0].share == doctest::Approx(shape.segment[0].share));
	CHECK(after.count == 3);

	SUBCASE("dragged to the left wall it clears, and is still there to be picked up") {
		const mseg::Layout now = card.layoutNow();
		const float here = box.x() + now.sustainX * box.width();
		card.onMouseDown().callback(at(here, lowY));
		card.onMouseDrag().callback(at(box.x(), lowY));
		card.onMouseUp().callback(at(box.x(), lowY));

		const mseg::Settings cleared = f.bridge.modRack().msegAt(0);
		CHECK(cleared.sustainSegment == mseg::kNoSustain);

		// PARKED, NOT GONE — and the way back is the same gesture, which is the whole reason "none"
		// was given a position instead of being a disappearance.
		const mseg::Layout parked = card.layoutNow();
		CHECK(parked.sustainVisible);
		CHECK_FALSE(parked.sustainActive);
		CHECK(parked.sustainX == doctest::Approx(0.0f));

		card.onMouseDown().callback(at(box.x(), lowY));
		card.onMouseDrag().callback(at(box.x() + 0.65f * box.width(), lowY));
		card.onMouseUp().callback(at(box.x() + 0.65f * box.width(), lowY));
		CHECK(f.bridge.modRack().msegAt(0).sustainSegment == 1);
	}

	SUBCASE("and the gesture is one undo step") {
		history.undo();
		CHECK(f.bridge.modRack().msegAt(0).sustainSegment == 1);
		history.redo();
		CHECK(f.bridge.modRack().msegAt(0).sustainSegment == 0);
	}

	SUBCASE("a looping shape offers no sustain marker to grab at all") {
		// THE HONESTY RULE ON THE SURFACE THE HAND TOUCHES. The engine overrules a sustain point while
		// looping, so the card must not hand out a grip for one — a gesture that appeared to work and
		// changed nothing audible is exactly what the rule forbids.
		mseg::Settings looping = f.bridge.modRack().msegAt(0);
		looping.mode = md::Mode::Loop;
		looping.loopStart = 0;
		looping.loopEnd = 2;
		looping.sustainSegment = 1;
		f.bridge.setMsegSettings(0, looping);

		const mseg::Layout laidLooping = card.layoutNow();
		CHECK_FALSE(laidLooping.sustainVisible);

		// The same drag that just worked now moves nothing — and the loop markers are what the grab
		// finds instead, which is the picture the shape is actually in.
		const float onSustain = box.x() + 0.65f * box.width();
		card.onMouseDown().callback(at(onSustain, lowY));
		card.onMouseDrag().callback(at(box.x() + 0.30f * box.width(), lowY));
		card.onMouseUp().callback(at(box.x() + 0.30f * box.width(), lowY));
		CHECK(f.bridge.modRack().msegAt(0).sustainSegment == 1);
	}
}
TEST_CASE("dragging a loop marker snaps it to a boundary and leaves the shape alone") {
	// The markers are the thing a plain looping flag cannot express, so the card has to be able to
	// move them — and moving one must not disturb the breakpoints, which are a different edit.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	namespace md = feathers::mod;
	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
	}
	shape.segment[0].level = 1.0f;
	shape.segment[1].level = 0.4f;
	shape.segment[2].level = 0.9f;
	shape.mode = md::Mode::Loop;
	shape.loopStart = 0;
	shape.loopEnd = 2;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	const mseg::Layout laid = card.layoutNow();
	REQUIRE(laid.loopVisible);
	const visage::Bounds box = card.editorBounds();

	// Grab the OPENING marker at x = 0 and drag it toward the middle. Nearest boundary is the start
	// of segment 1, a third across.
	const float startX = box.x() + laid.loopStartX * box.width();
	const float mid = box.y() + box.height() * 0.5f;
	card.onMouseDown().callback(at(startX, mid));
	card.onMouseDrag().callback(at(box.x() + 0.4f * box.width(), mid));
	card.onMouseUp().callback(at(box.x() + 0.4f * box.width(), mid));

	const mseg::Settings after = f.bridge.modRack().msegAt(0);
	CAPTURE(after.loopStart);
	CHECK(after.loopStart == 1);

	// AND THE BREAKPOINTS DID NOT MOVE. The value, not "roughly": a marker drag that nudged a level
	// would be one gesture editing two things, which is how an editor becomes unpredictable.
	CHECK(after.segment[0].level == doctest::Approx(1.0f));
	CHECK(after.segment[1].level == doctest::Approx(0.4f));
	CHECK(after.segment[2].level == doctest::Approx(0.9f));
	CHECK(after.segment[0].share == doctest::Approx(shape.segment[0].share));
}
TEST_CASE("an Opt-drag bends the segment it caught, and lights that segment alone") {
	// THE INSTALLED BUILD HAD no per-segment curvature, though `Segment::shape` has existed
	// since the device was written — declared as SegNShape, carried in the row, read by the engine,
	// and reachable by nothing. The design: an Opt-drag (Alt off macOS) on a waypoint applies inverse
	// curvature on either side of it, an Opt-drag on a segment bends that segment, and the affected
	// segments are highlighted while editing.
	//
	// THE VALUE IN THE SLOT, NOT THE PICTURE. A bent curve carries every segment's shape at once and
	// cannot say which number moved; the engine reads `segment[i].shape`, so that is what is asserted.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
		shape.segment[i].shape = 0.0f;
	}
	shape.segment[0].level = 1.0f;
	shape.segment[1].level = 0.2f;
	shape.segment[2].level = 0.8f;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);
	REQUIRE(f.bridge.modRack().holds(0, mseg::kTraits.tag));

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	const mseg::Layout laid = card.layoutNow();
	const visage::Bounds box = card.editorBounds();
	REQUIRE(laid.segmentEnd.size() == 3);

	// THE MIDDLE OF SEGMENT 1, AND DELIBERATELY OFF THE CURVE. Segment 1 falls from 1.0 to 0.2, so at
	// its midpoint the line is at 0.6 — the press is at 0.15, well below it and well away from both
	// breakpoints, so what is caught is the SEGMENT and could be nothing else.
	const float midX = box.x() + 0.5f * (laid.segmentEnd[0] + laid.segmentEnd[1]) * box.width();
	const float lowY = box.bottom() - 0.15f * box.height();

	const mseg::CurveTarget caught = card.curveTargetAtCardPoint({ midX, lowY });
	REQUIRE(caught.kind == mseg::CurveTarget::Kind::Segment);
	CHECK(caught.index == 1);
	CHECK(caught.first == 1);
	CHECK(caught.count == 1);

	// A QUARTER OF THE BOX UPWARD. Full bend is half the box, so a quarter is exactly +0.5 — a
	// number, chosen so the assertion below is a value rather than "more than it was".
	const float travel = 0.25f * box.height();
	card.onMouseDown().callback(optAt(midX, lowY));
	CHECK(card.highlightForTest().kind == mseg::CurveTarget::Kind::Segment);
	CHECK(card.highlightForTest().first == 1);
	CHECK(card.highlightForTest().count == 1);

	card.onMouseDrag().callback(optAt(midX, lowY - travel));

	const mseg::Settings mid = f.bridge.modRack().msegAt(0);
	CHECK(mid.segment[1].shape == doctest::Approx(0.5f).epsilon(0.02));
	// ITS NEIGHBOURS ARE UNTOUCHED, which is the half that separates a segment target from a
	// waypoint's.
	CHECK(mid.segment[0].shape == doctest::Approx(0.0f));
	CHECK(mid.segment[2].shape == doctest::Approx(0.0f));

	card.onMouseUp().callback(optAt(midX, lowY - travel));

	// AND THE HIGHLIGHT GOES OUT WITH THE GESTURE — the "while editing", the second half.
	CHECK(card.highlightForTest().kind == mseg::CurveTarget::Kind::None);

	SUBCASE("and the drag is absolute from its anchor rather than an accumulation of moves") {
		// THE MUTATION THIS EXISTS FOR: `withCurvature` ADDS, so a card that applied each move to the
		// CURRENT settings would bend further for a drag that reached the same pixel by a longer
		// route. Same start, same end, four moves instead of one — the answer must be the same number.
		mseg::Settings again = shape;
		feathers::ModRack reset = f.bridge.modRack();
		reset.setMsegAt(0, again);
		f.bridge.restoreModRack(reset);

		card.onMouseDown().callback(optAt(midX, lowY));
		for (int step = 1; step <= 4; ++step) {
			const float y = lowY - travel * static_cast<float>(step) / 4.0f;
			card.onMouseDrag().callback(optAt(midX, y));
		}
		card.onMouseUp().callback(optAt(midX, lowY - travel));
		CHECK(f.bridge.modRack().msegAt(0).segment[1].shape == doctest::Approx(0.5f).epsilon(0.02));
	}

	SUBCASE("and the whole bend is ONE undo step") {
		history.undo();
		CHECK(f.bridge.modRack().msegAt(0).segment[1].shape == doctest::Approx(0.0f));
		history.redo();
		CHECK(f.bridge.modRack().msegAt(0).segment[1].shape == doctest::Approx(0.5f).epsilon(0.02));
	}

	SUBCASE("and without Opt the same press moves the shape instead of bending it") {
		// THE MODIFIER IS LOAD-BEARING, stated so a card that ignored it and bent on every drag would
		// fail here rather than pass everything above. A plain press at the same point catches no
		// handle and no marker, so it does nothing at all — and in particular it bends nothing.
		mseg::Settings again = shape;
		feathers::ModRack reset = f.bridge.modRack();
		reset.setMsegAt(0, again);
		f.bridge.restoreModRack(reset);

		card.onMouseDown().callback(at(midX, lowY));
		card.onMouseDrag().callback(at(midX, lowY - travel));
		card.onMouseUp().callback(at(midX, lowY - travel));
		CHECK(f.bridge.modRack().msegAt(0).segment[1].shape == doctest::Approx(0.0f));
		CHECK(card.highlightForTest().kind == mseg::CurveTarget::Kind::None);
	}
}
TEST_CASE("an Opt-drag on a waypoint bends both its sides in opposite ways, and lights both") {
	// The other half of the design: a waypoint drag applies inverse curvature on either side of the
	// waypoint. One drag, two segments, opposite signs — which is
	// what makes a waypoint drag draw an S through the breakpoint rather than a corner.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
		shape.segment[i].shape = 0.0f;
	}
	shape.segment[0].level = 1.0f;
	shape.segment[1].level = 0.2f;
	shape.segment[2].level = 0.8f;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	const mseg::Layout laid = card.layoutNow();
	const visage::Bounds box = card.editorBounds();
	REQUIRE(laid.handle.size() == 4);

	// HANDLE 1 — the peak, between segment 0 and segment 1.
	const mseg::Handle& peak = laid.handle[1];
	const visage::Point on { box.x() + peak.x * box.width(),
		                     box.bottom() - peak.level * box.height() };

	const mseg::CurveTarget caught = card.curveTargetAtCardPoint(on);
	REQUIRE(caught.kind == mseg::CurveTarget::Kind::Waypoint);
	CHECK(caught.index == 1);
	CHECK(caught.first == 0);
	CHECK(caught.count == 2);

	const float travel = 0.25f * box.height(); // +0.5, as above
	card.onMouseDown().callback(optAt(on.x, on.y));
	CHECK(card.highlightForTest().first == 0);
	CHECK(card.highlightForTest().count == 2);

	card.onMouseDrag().callback(optAt(on.x, on.y - travel));
	card.onMouseUp().callback(optAt(on.x, on.y - travel));

	const mseg::Settings after = f.bridge.modRack().msegAt(0);
	// THE TWO NUMBERS, AND THEN THE RELATION BETWEEN THEM. Either alone would pass for a build that
	// wrote the same sign to both, or for one that wrote nothing to the second.
	CHECK(after.segment[0].shape == doctest::Approx(0.5f).epsilon(0.02));
	CHECK(after.segment[1].shape == doctest::Approx(-0.5f).epsilon(0.02));
	CHECK(after.segment[0].shape == doctest::Approx(-after.segment[1].shape));
	CHECK(after.segment[2].shape == doctest::Approx(0.0f));

	SUBCASE("and a waypoint at the shape's end has one neighbour, and bends it") {
		// A CHOSEN BEHAVIOUR RATHER THAN A SPECIFIED ONE — the spec says nothing about the ends,
		// and the alternative was to refuse them the gesture, which would make two of the handles
		// silently different from every other. See the commit message.
		feathers::ModRack reset = f.bridge.modRack();
		reset.setMsegAt(0, shape);
		f.bridge.restoreModRack(reset);

		const mseg::Handle last = card.layoutNow().handle.back();
		const visage::Point tip { box.x() + last.x * box.width(),
			                      box.bottom() - last.level * box.height() };
		const mseg::CurveTarget end = card.curveTargetAtCardPoint(tip);
		REQUIRE(end.kind == mseg::CurveTarget::Kind::Waypoint);
		CHECK(end.index == 3);
		CHECK(end.first == 2);
		CHECK(end.count == 1);

		card.onMouseDown().callback(optAt(tip.x, tip.y));
		card.onMouseDrag().callback(optAt(tip.x, tip.y - travel));
		card.onMouseUp().callback(optAt(tip.x, tip.y - travel));

		const mseg::Settings bent = f.bridge.modRack().msegAt(0);
		CHECK(bent.segment[2].shape == doctest::Approx(0.5f).epsilon(0.02));
		CHECK(bent.segment[0].shape == doctest::Approx(0.0f));
		CHECK(bent.segment[1].shape == doctest::Approx(0.0f));
	}
}
TEST_CASE("an Opt-hover lights what the hand would catch before it commits to a drag") {
	// THIS IS THE FIX FOR WAYPOINTS BEING HARD TO GRAB, and the tolerance is only its other half.
	// A hand aiming between a breakpoint and the segment beside it cannot tell which it caught until
	// the shape moves — by which time it has already bent the wrong thing. The affected segments are
	// highlighted while editing; showing the same highlight under the pointer
	// makes the hit test legible BEFORE the button goes down.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
	}
	shape.segment[0].level = 1.0f;
	shape.segment[1].level = 0.2f;
	shape.segment[2].level = 0.8f;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	const mseg::Layout laid = card.layoutNow();
	const visage::Bounds box = card.editorBounds();
	const mseg::Handle& peak = laid.handle[1];
	const visage::Point on { box.x() + peak.x * box.width(),
		                     box.bottom() - peak.level * box.height() };
	const float midX = box.x() + 0.5f * (laid.segmentEnd[0] + laid.segmentEnd[1]) * box.width();
	const float lowY = box.bottom() - 0.15f * box.height();

	// NOTHING IS LIT UNTIL OPT IS DOWN. A highlight that appeared on a plain hover would be telling a
	// hand about a gesture it is not making.
	card.onMouseMove().callback(at(on.x, on.y));
	CHECK(card.highlightForTest().kind == mseg::CurveTarget::Kind::None);

	// OVER THE BREAKPOINT: two segments.
	card.onMouseMove().callback(optAt(on.x, on.y));
	CHECK(card.highlightForTest().kind == mseg::CurveTarget::Kind::Waypoint);
	CHECK(card.highlightForTest().first == 0);
	CHECK(card.highlightForTest().count == 2);

	// MOVE OFF IT ONTO THE SEGMENT: one segment, and a DIFFERENT one — which is the disambiguation
	// working rather than the highlight merely existing.
	card.onMouseMove().callback(optAt(midX, lowY));
	CHECK(card.highlightForTest().kind == mseg::CurveTarget::Kind::Segment);
	CHECK(card.highlightForTest().first == 1);
	CHECK(card.highlightForTest().count == 1);

	// AND THE CARD DRAWS WITH A HIGHLIGHT UP. The picture itself is not asserted — there is no
	// headless way to read a canvas here — but the highlight walks segment indices, and a walk that
	// ran off the end of `segmentEnd` would take the editor down rather than draw wrongly.
	{
		visage::Canvas canvas;
		card.draw(canvas);
		card.onMouseMove().callback(optAt(on.x, on.y)); // the waypoint, two segments
		card.draw(canvas);
	}

	// AND IT GOES OUT WHEN THE POINTER LEAVES.
	card.onMouseExit().callback(at(-5.0f, -5.0f));
	CHECK(card.highlightForTest().kind == mseg::CurveTarget::Kind::None);

	SUBCASE("and the hover neither writes to the slot nor records an undo step") {
		// A HOVER IS NOT AN EDIT. Stated because the highlight is computed by the same call the drag
		// uses, and a card that took the shortcut of applying a zero-delta bend would leave a
		// no-op undo step behind every time the pointer crossed the editor.
		const mseg::Settings before = f.bridge.modRack().msegAt(0);
		card.onMouseMove().callback(optAt(on.x, on.y));
		card.onMouseMove().callback(optAt(midX, lowY));
		CHECK(f.bridge.modRack().msegAt(0) == before);
		CHECK_FALSE(history.canUndo());
	}
}
TEST_CASE("a double-click adds a waypoint where the hand pointed, and one on a waypoint removes it") {
	// The defect: new waypoints could not be added. There was no affordance at all —
	// the device declares eight segments and the editor could only ever drag the ones a patch already
	// had.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 2;
	shape.segment[0].share = feathers::mod::timeValueFor(0.2);
	shape.segment[0].level = 1.0f;
	shape.segment[1].share = feathers::mod::timeValueFor(0.2);
	shape.segment[1].level = 0.0f;
	shape.sustainSegment = mseg::kNoSustain;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));
	const visage::Bounds box = card.editorBounds();
	REQUIRE(card.layoutNow().handle.size() == 3);

	// A THIRD ACROSS AND DELIBERATELY NOT A MIDPOINT. Two equal segments make 0.25 segment 0's exact
	// middle, so a build that ignored the pointer and always halved would pass there.
	const float x = box.x() + 0.35f * box.width();
	const float y = box.y() + 0.5f * box.height();
	card.onMouseDown().callback(doubleClickAt(x, y));
	card.onMouseUp().callback(doubleClickAt(x, y));

	const mseg::Settings added = f.bridge.modRack().msegAt(0);
	CHECK(added.count == 3);
	// WHERE THE HAND POINTED, as a value: the new boundary sits at 0.35 of the width.
	const mseg::Layout now = card.layoutNow();
	REQUIRE(now.segmentEnd.size() == 3);
	CHECK(now.segmentEnd[0] == doctest::Approx(0.35f).epsilon(0.06));
	// AND THE SHAPE DID NOT MOVE: the new breakpoint takes the level the curve already had at 0.35 of
	// segment 0's span, which on a straight rise from 0 to 1 over the first half is 0.70.
	CHECK(added.segment[0].level == doctest::Approx(0.70f).epsilon(0.03));
	CHECK(added.segment[1].level == doctest::Approx(1.0f));
	CHECK(added.segment[2].level == doctest::Approx(0.0f));

	SUBCASE("and the same click on the breakpoint takes it away again") {
		const mseg::Handle fresh = card.layoutNow().handle[1];
		const visage::Point on { box.x() + fresh.x * box.width(),
			                     box.bottom() - fresh.level * box.height() };
		REQUIRE(card.handleAtCardPoint(on) == 1);
		card.onMouseDown().callback(doubleClickAt(on.x, on.y));
		card.onMouseUp().callback(doubleClickAt(on.x, on.y));

		const mseg::Settings back = f.bridge.modRack().msegAt(0);
		CHECK(back.count == 2);
		CHECK(back.segment[0].level == doctest::Approx(1.0f));
		CHECK(back.segment[1].level == doctest::Approx(0.0f));
		CHECK(feathers::mod::timeSeconds(back.segment[0].share)
		      == doctest::Approx(0.2).epsilon(0.03));
	}

	SUBCASE("and each is ONE undo step") {
		history.undo();
		CHECK(f.bridge.modRack().msegAt(0).count == 2);
		history.redo();
		CHECK(f.bridge.modRack().msegAt(0).count == 3);
	}

	SUBCASE("and a SINGLE click adds nothing, or every drag would leave a breakpoint behind") {
		const size_t was = f.bridge.modRack().msegAt(0).count;
		card.onMouseDown().callback(at(box.x() + 0.6f * box.width(), y));
		card.onMouseUp().callback(at(box.x() + 0.6f * box.width(), y));
		CHECK(f.bridge.modRack().msegAt(0).count == was);
	}
}
TEST_CASE("a breakpoint sitting on a loop boundary can still be grabbed") {
	// THE SAME DEFECT AS "HARD TO GRAB", IN ITS TOTAL FORM. A loop marker sits on a segment
	// boundary and so does a handle, and the card used to give the marker first refusal within its
	// tolerance AT ANY HEIGHT — so every breakpoint on a loop boundary was ungrabbable, not merely
	// fiddly. `handleAt`'s own note says a handle that cannot be reached at all is the thing to
	// prevent; this is that case arriving from the card's side rather than the geometry's.
	//
	// The marker stays reachable, and more honestly: it is drawn as a full-height line, so aiming at
	// it anywhere other than the 13px dot still catches it. The subcase below is that half.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	namespace md = feathers::mod;
	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
	}
	shape.segment[0].level = 1.0f;
	shape.segment[1].level = 0.4f;
	shape.segment[2].level = 0.9f;
	shape.mode = md::Mode::Loop;
	shape.loopStart = 1; // opens at the end of segment 0 — exactly where handle 1 sits
	shape.loopEnd = 2;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	const mseg::Layout laid = card.layoutNow();
	const visage::Bounds box = card.editorBounds();
	REQUIRE(laid.loopVisible);
	// THE MARKER AND THE BREAKPOINT REALLY DO SHARE AN X, or this case proves nothing.
	REQUIRE(laid.loopStartX == doctest::Approx(laid.handle[1].x));

	const visage::Point on { box.x() + laid.handle[1].x * box.width(),
		                     box.bottom() - laid.handle[1].level * box.height() };
	card.onMouseDown().callback(at(on.x, on.y));
	card.onMouseDrag().callback(at(on.x, box.bottom() - 0.5f * box.height()));
	card.onMouseUp().callback(at(on.x, box.bottom() - 0.5f * box.height()));

	const mseg::Settings after = f.bridge.modRack().msegAt(0);
	// THE HANDLE MOVED — the value, halfway up the box for a unipolar shape.
	CHECK(after.segment[0].level == doctest::Approx(0.5f).epsilon(0.05));
	// AND THE MARKER DID NOT, which is the half that says the press did not land on it instead.
	CHECK(after.loopStart == 1);
	CHECK(after.loopEnd == 2);

	SUBCASE("and the marker is still reachable, away from the breakpoint's dot") {
		feathers::ModRack reset = f.bridge.modRack();
		reset.setMsegAt(0, shape);
		f.bridge.restoreModRack(reset);

		// Halfway down the box, which on this shape is 36px below the peak — outside the 13px dot and
		// on the marker's line.
		const float away = box.bottom() - 0.5f * box.height();
		REQUIRE(card.handleAtCardPoint({ on.x, away }) == -1);
		card.onMouseDown().callback(at(on.x, away));
		card.onMouseDrag().callback(at(box.x() + 0.9f * box.width(), away));
		card.onMouseUp().callback(at(box.x() + 0.9f * box.width(), away));

		const mseg::Settings moved = f.bridge.modRack().msegAt(0);
		CHECK(moved.loopStart == 2);
		// AND NO BREAKPOINT MOVED WITH IT.
		CHECK(moved.segment[0].level == doctest::Approx(1.0f));
		CHECK(moved.segment[1].level == doctest::Approx(0.4f));
	}
}
TEST_CASE("the MSEG card's boxes hold the controls that are NOT the shape") {
	// A deliberate divergence from the envelope card, recorded as a case so the choice is visible.
	// Thirty-seven declared controls; three boxes. The other thirty-four are the breakpoints and the
	// switches, and breakpoints are dragged because a list of twenty-four numbers is not an editor.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));

	// The window is `Base x Rate`, so the two sit together — reading either alone says nothing about how long the shape
	// takes. DEPTH IS GONE (backlog FH): Amount is the one depth, as on the DAHDSR. Phase is the start position.
	const auto& boxes = ui::MsegCard::layout();
	REQUIRE(boxes.size() == 6);
	CHECK(boxes[0] == mseg::Param::TimeBase);
	CHECK(boxes[1] == mseg::Param::TimeScale);
	CHECK(boxes[2] == mseg::Param::Smoothing);
	CHECK(boxes[3] == mseg::Param::Amount);
	CHECK(boxes[4] == mseg::Param::Phase);
	CHECK(boxes[5] == mseg::Param::StereoPhase);
	CHECK(mseg::kParamDescs[static_cast<size_t>(mseg::Param::Amount)].modulatable == true);

	// AND THE BASE IS A DESTINATION SINCE 2026-10-30 (time base as a modulation destination, applied generally;
	// backlog PF). It was not, on the argument that a route would move which
	// SHAPE OF TIME the device is in; a routed base lands on whole divisions and the window follows through the
	// transport lock, proved by value in test_mod_timebase.
	CHECK(mseg::kParamDescs[static_cast<size_t>(mseg::Param::TimeBase)].modulatable == true);

	// AND THE OTHERS THAT ARE DESTINATIONS ARE THE ONES THE DEVICE DECLARES AS SUCH. The card does not
	// get to decide this — it reads what mseg_types.h declared, which is what the rack's destination
	// space was sized from.
	CHECK(mseg::kParamDescs[static_cast<size_t>(mseg::Param::TimeScale)].modulatable == true);

	SUBCASE("and every box follows the armed flag itself") {
		// Repainting a card does not repaint the boxes inside it — visage marks one frame. The
		// envelope card learned this from a glow that never arrived; this one is built with it.
		CHECK(mod.watches(&card));
		for (size_t i = 0; i < ui::MsegCard::kBoxCount; ++i) {
			CAPTURE(i);
			CAPTURE(card.boxCaption(i));
			REQUIRE(card.boxFrame(i) != nullptr);
			CHECK(mod.watches(card.boxFrame(i)));
		}
	}
}
TEST_CASE("the strip draws the card the slot's device asks for, and swaps it when the device does") {
	// THE VISIBLE HALF OF THE UNPINNING, AND THE LAST PLACE IT WAS STILL PINNED. The rack learned to
	// hold a device rather than an envelope; the strip went on holding
	// `std::array<std::unique_ptr<EnvCard>, N>` and asking `EnvCard::height()`. A rack that can hold
	// an MSEG and a strip that can only draw envelopes is half-unpinned, and the leftover half is the
	// one a hand can see.
	//
	// ASSERTED THROUGH THE HEIGHT, which is the one thing the two cards genuinely disagree about and
	// the one the strip has to get from the card rather than from a constant. An MSEG's editor is
	// taller than an envelope's curve because a breakpoint needs room to be grabbed in.
	ModFixture f;
	namespace mseg = feathers::mod::mseg;
	namespace dsr = feathers::mod::dahdsr;

	REQUIRE(f.panel != nullptr);
	REQUIRE(ui::MsegCard::height() != ui::EnvCard::height()); // or the assertions below prove nothing

	// A fresh rack holds envelopes.
	REQUIRE(f.bridge.modRack().holds(0, dsr::kTraits.tag));
	f.panel->resized();
	CHECK(f.panel->cardHeightForTest(0) == doctest::Approx(ui::kCardHeadHeight + ui::EnvCard::height() + ui::kCardOutputStripHeight));

	// Put an MSEG in slot 0.
	feathers::ModRack rack = f.bridge.modRack();
	rack.setMsegAt(0, mseg::Settings {});
	f.bridge.restoreModRack(rack);
	REQUIRE(f.bridge.modRack().holds(0, mseg::kTraits.tag));

	// THE STRIP FOLLOWS IT. Not on being told — nothing tells it — but by asking the rack what the
	// slot holds before it lays the card out. A patch load changes a slot's device under the panel
	// today, and the browser will do it deliberately.
	f.panel->resized();
	CHECK(f.panel->cardHeightForTest(0) == doctest::Approx(ui::kCardHeadHeight + ui::MsegCard::height() + ui::kCardOutputStripHeight));

	SUBCASE("and back again, because a swap is not one-way") {
		feathers::ModRack back = f.bridge.modRack();
		back.setDahdsrAt(0, dsr::Settings {});
		f.bridge.restoreModRack(back);
		f.panel->resized();
		CHECK(f.panel->cardHeightForTest(0) == doctest::Approx(ui::kCardHeadHeight + ui::EnvCard::height() + ui::kCardOutputStripHeight));
	}

	SUBCASE("and a slot that did not change keeps the card it had") {
		// The rebuild is keyed on the TAG, not on any change to the rack — otherwise every patch load
		// would throw away every card and with it every drag in progress.
		const void* before = f.panel->cardFrameForTest(1);
		f.panel->resized();
		CHECK(f.panel->cardFrameForTest(1) == before);
	}
}
TEST_CASE("every control the filter declares is reachable from its card") {
	// THE GAP NOTHING LOOKED FOR: A PARAM WITH NO CONTROL.
	//
	// "every knob in the editor is wired to the modulation state" walks the tree FROM THE WIDGETS
	// OUTWARD, so it can only ever check controls that exist. A parameter the card never built a
	// control for is invisible to it — and that is exactly what happened when ParamId::FilterPoly was
	// added: the engine read it, the param table declared it, and NO HAND COULD SET IT. The whole
	// point of the poly/keytrack split was to make one combination selectable, and for one build it
	// was selectable by nothing.
	//
	// feathers-cc found it by grepping the UI for the new name. This walks it the other way — from
	// the DECLARATIONS inward — which is the direction that catches the next one.
	FilterFixture f;
	visage::Canvas canvas;
	f.card->draw(canvas);

	// WHAT THE CARD ADDRESSES: every knob's control, plus the two switches'.
	//
	// ASKED AND ANSWERED IN THE DEVICE'S OWN VOCABULARY, which is the change that came with the
	// swallow. This walk used to run over ParamIds and reach the descriptor list by ADDING AN INDEX TO
	// ParamId::FilterEnabled — an arithmetic bridge between two numbering schemes that was correct
	// only while the filter had a contiguous block of host ids. It has none now, and the walk is
	// better for it: `fx::filter::Param` is the device's own index into its own declarations, which is
	// the thing the card addresses and the thing the row stores.
	namespace flt = feathers::fx::filter;
	std::set<size_t> reachable;
	for (const flt::Param param : f.card->addressedParamsForTest()) {
		reachable.insert(static_cast<size_t>(param));
	}

	// AND THE ONES DRAWN AS BESPOKE PARTS RATHER THAN AS CONTROLS, each named with its reason. An
	// allow-list rather than a blanket exemption, so adding a control cannot join it by accident — a
	// new declaration that nothing draws fails until someone writes down which part draws it.
	const std::map<flt::Param, const char*> drawnAsParts{
	    {flt::Param::Enabled, "the square in the head — Part::Enable"},
	    {flt::Param::Type, "the dropdown and its arrows — Part::TypeMenu/PrevType/NextType"},
	    {flt::Param::Slope, "the slope dropdown — Part::SlopeMenu"},
	    // THE CHAIN FURNITURE (backlog NR,): the card is left alone, so the slot's Mix and wet Gain are the
	    // site panel's head row (test_gui_widgets_nest, "NR: the seven custom-card devices ...").
	    {flt::Param::Mix, "the site panel's head row — the slot's furniture, not the card's"},
	    {flt::Param::WetGain, "the site panel's head row — the slot's furniture, not the card's"},
	};

	size_t checked = 0;
	for (size_t i = 0; i < std::size(flt::kParamDescs); ++i) {
		const auto param = static_cast<flt::Param>(i);
		CAPTURE(std::string(flt::kParamDescs[i].name));

		const bool hasControl = reachable.count(i) > 0;
		const auto part = drawnAsParts.find(param);
		if (part != drawnAsParts.end()) {
			CAPTURE(std::string(part->second));
			// A control may be drawn as a part INSTEAD of as a control, never as neither and never as
			// both — two roads to one value is the duplication the honesty rule refuses.
			CHECK_FALSE(hasControl);
		}
		else {
			CHECK(hasControl);
		}
		++checked;
	}

	// NOT VACUOUS. A declaration list that had lost its entries would pass the loop by never running
	// it, and an allow-list that had swallowed everything would pass it by exempting everything.
	CHECK(checked == std::size(flt::kParamDescs));
	CHECK(reachable.size() >= checked - drawnAsParts.size());
}
TEST_CASE("every declared parameter a hand should be able to reach has a control that reaches it") {
	// THE DIRECTION NOBODY WAS COVERING, AND IT HAS ALREADY SHIPPED TWICE.
	//
	// "every knob in the editor is wired to the modulation state" walks the frame tree OUTWARD from
	// the widgets. It can see a widget with no wiring; it cannot see a PARAM WITH NO WIDGET, because
	// there is no widget to start from. `ParamId::FilterPoly` shipped read by synth.cpp and set by
	// nothing — the whole point of the commit, unreachable by a hand, with every suite green.
	//
	// So this walks the other way: from the DECLARED PARAMS to the widgets, and asks which ones
	// nothing can reach.
	ModFixture f;

	// THE EXCEPTION IS ONE PARAMETER, AND IT IS HAND-KEPT BECAUSE NOTHING DERIVES IT — measured
	// rather than assumed. Master Gain carries `kContinuous`, exactly the flags Level and every other
	// continuous control carry, so there is no flag, unit or module to separate "deliberately has no
	// widget" from "somebody forgot one". A derived exception list would be better and this is not
	// one; it is a list of one with a reason, which is the honest form when the reason is a design
	// decision rather than a property.
	//
	// WHY MASTER GAIN HAS NO CONTROL: it is the plugin's output trim, and the host already gives every
	// track a fader an inch away. A second one inside the editor is two controls for one job, which is
	// the objection this project raises to every other doubled control. It stays a declared parameter
	// because a HOST should be able to automate it.
	// THE SPECTRUM PANEL IS ONE PRESS AWAY (its tab on Explore, 2026-10-12: a floating window, not a drawer that stood in
	// the tree): on Additive, the engine it opens on, and opened as a hand opens it, its controls are counted with every
	// other surface's.
	ParamSet additive;
	additive.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Additive));
	f.bridge.publish(additive);
	f.bridge.refresh();
	if (auto* tab = findInTree<ui::SpectrumTab>(f.root); tab != nullptr) {
		tab->clickLabelForTest();
	}
	REQUIRE(f.gui.spectrumPanelForTest() != nullptr);
	const std::array<ParamId, 1> kNoWidgetByDesign { ParamId::MasterGain };
	// WHAT THE EDITOR REACHES WITH THE SPECTRUM PANEL OPEN, kept: the drum kit panel below is opened on Drum, and opening it
	// retires the spectrum panel (one surface presented at a time), so each panel's controls are counted while it is open.
	std::vector<bool> reached(paramDescs().size(), false);
	for (size_t i = 0; i < paramDescs().size(); ++i) {
		reached[i] = !f.gui.widgets()->watchers(static_cast<clap_id>(paramDescs()[i].id)).empty();
	}
	std::vector<std::string> unreachable;
	size_t declared = 0;
	for (size_t i = 0; i < paramDescs().size(); ++i) {
		const ParamDesc& desc = paramDescs()[i];
		++declared;
		const auto id = static_cast<clap_id>(desc.id);
		if (!reached[i] && f.gui.widgets()->watchers(id).empty()) {
			unreachable.push_back(std::string(desc.name));
		}
	}
	// The editor really is fully built, or "nothing is unreachable" would be a claim about an empty
	// registry. 108 params on the day this was written.
	CHECK(declared > 90);

	// WHAT THE EXCEPTIONS ARE CALLED, so the comparison below reads as names rather than as ids.
	std::vector<std::string> expected;
	for (const ParamId id : kNoWidgetByDesign) {
		const ParamDesc* desc = findParam(static_cast<clap_id>(id));
		REQUIRE(desc != nullptr);
		expected.push_back(std::string(desc->name));
	}
	// THE DRUM PAD'S TWO LAYERS (kit stance redesign) HAVE NO ALLOWANCE LEFT: every control a layer owns (its Lock, its vector, its pitch
	// mode, layer two's copies of everything a hit reads, its Kind, Level, Delay and High Pass) is on the pad editor's tabs, so the
	// declared-but-unreachable set is the one parameter above and nothing of the drum's.
	std::sort(unreachable.begin(), unreachable.end());
	std::sort(expected.begin(), expected.end());

	// NAMED, NOT COUNTED. A count says "one more than expected" and leaves the reader to work out
	// which — and not noticing which is how FilterPoly shipped.
	std::string extra;
	for (const std::string& name : unreachable) {
		if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
			extra += name + "; ";
		}
	}
	CAPTURE(extra);
	CHECK(extra.empty());

	SUBCASE("and an exception that GAINS a control is a failure too") {
		// PINNED BOTH WAYS ON PURPOSE. A one-way check — "nothing unexpected is unreachable" — lets the
		// exception list rot: give Master Gain a widget and the list still says it has none, and the
		// next reader believes a stale sentence. The list has to be exactly the set, so removing an
		// entry is as deliberate as adding one.
		std::string stale;
		for (const std::string& name : expected) {
			if (std::find(unreachable.begin(), unreachable.end(), name) == unreachable.end()) {
				stale += name + "; ";
			}
		}
		CAPTURE(stale);
		CHECK(stale.empty());
	}

	SUBCASE("and the check can actually see an unreachable parameter") {
		// THE NON-VACUITY HALF, and it is not decoration here: `watchers()` returning empty for a param
		// nothing declares would make every id look reachable and this whole case a formality. So ask
		// about an id that certainly has no widget — one past the end of the declared range.
		const auto absurd = static_cast<clap_id>(0xFFFFu);
		CHECK(findParam(absurd) == nullptr);
		CHECK(f.gui.widgets()->watchers(absurd).empty());

		// And a param that certainly HAS one comes back non-empty, or `watchers` is answering empty to
		// everything and the case above passes for the wrong reason.
		//
		// THIS NAMED FilterPoly AND CANNOT ANY MORE, which is worth a sentence because the swap is not
		// cosmetic. Poly was chosen precisely BECAUSE it had once shipped with no control at all; it
		// still has one, but that control is a device's rather than a parameter's, so it registers
		// with no clap_id and `watchers` correctly answers empty for it. Asserting non-empty here
		// would now be asserting that a swallowed control is still reachable as a host parameter,
		// which is the thing the swallow removed on purpose. Path is a plain host parameter with a
		// plain knob, which is all this check ever needed.
		CHECK_FALSE(f.gui.widgets()->watchers(static_cast<clap_id>(ParamId::Path)).empty());
	}
}
TEST_CASE("the start handle sets StartLevel, and the closing marker sets LoopEnd") {
	// TWO ROADS THE REACHABILITY GUARD CLAIMS AND NOTHING ELSE IN THIS FILE EXERCISED. The guard
	// takes `MsegCard::addressedParamsForTest` at its word about which controls a gesture reaches,
	// and a claim nobody drives is a wish — so each entry in that list needs a case that asserts the
	// VALUE it writes. The other five entries have one; these two did not.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	namespace md = feathers::mod;
	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
	}
	shape.segment[0].level = 1.0f;
	shape.segment[1].level = 0.4f;
	shape.segment[2].level = 0.9f;
	shape.mode = md::Mode::Loop;
	shape.loopStart = 0;
	shape.loopEnd = 2;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));
	const visage::Bounds box = card.editorBounds();

	// HANDLE 0 IS THE SHAPE'S START. It has no duration before it, so only its level moves — dragged
	// to three quarters of the way up an 0..1 box, which is 0.75 and not "higher than it was".
	const mseg::Layout laid = card.layoutNow();
	REQUIRE(laid.handle.front().segment == feathers::mod::mseg::kStartHandle); // the start handle wears the sentinel
	const visage::Point start { box.x() + laid.handle.front().x * box.width(),
		                        box.bottom() - laid.handle.front().level * box.height() };
	REQUIRE(card.handleAtCardPoint(start) == 0);

	card.onMouseDown().callback(at(start.x, start.y));
	card.onMouseDrag().callback(at(start.x, box.bottom() - 0.75f * box.height()));
	card.onMouseUp().callback(at(start.x, box.bottom() - 0.75f * box.height()));

	const mseg::Settings afterStart = f.bridge.modRack().msegAt(0);
	CHECK(afterStart.startLevel == doctest::Approx(0.75f).epsilon(0.03));
	// AND THE FIRST BREAKPOINT DID NOT COME WITH IT, which is what separates "the start moved" from
	// "the whole left edge moved".
	CHECK(afterStart.segment[0].level == doctest::Approx(1.0f));

	SUBCASE("and the closing marker snaps to the boundary it was dropped nearest") {
		// The opening marker has a case of its own; this is the other one, and it is a different
		// field with a different asymmetry — loopEnd sits on a segment's RIGHT edge where loopStart
		// sits on its left.
		const mseg::Layout now = card.layoutNow();
		REQUIRE(now.loopVisible);
		const float endX = box.x() + now.loopEndX * box.width();
		// Halfway down, clear of every breakpoint's 13px dot.
		const float away = box.bottom() - 0.5f * box.height();
		REQUIRE(card.handleAtCardPoint({ endX, away }) == -1);

		card.onMouseDown().callback(at(endX, away));
		// Drop it a third across, which is the right edge of segment 0.
		card.onMouseDrag().callback(at(box.x() + 0.34f * box.width(), away));
		card.onMouseUp().callback(at(box.x() + 0.34f * box.width(), away));

		const mseg::Settings moved = f.bridge.modRack().msegAt(0);
		CHECK(moved.loopEnd == 0);
		CHECK(moved.loopStart == 0);
		// AND NO BREAKPOINT MOVED.
		CHECK(moved.segment[1].level == doctest::Approx(0.4f));
		CHECK(moved.segment[2].level == doctest::Approx(0.9f));
	}
}
TEST_CASE("a breakpoint at either extreme is as selectable as one in the middle") {
	// The suspicion: waypoints at the far right extreme seem to become ungrabbable.
	//
	// THE HYPOTHESIS THIS CASE WAS WRITTEN TO TEST — and it turned out to be WRONG, which is why the
	// case says what it says. The guess was that a handle at x == 1 can only be approached from one
	// side, because the pointer's x is clamped to the editor rect, leaving half an ellipse. That
	// would be about 6.5px of reachable target and would feel exactly as reported.
	//
	// It is not what happens, and the reason is `editorGrabBounds`: a press up to the grab radius
	// OUTSIDE the drawn box is still the editor's, and `toUnit` clamps it to x = 1 — so the half of
	// the ellipse that falls outside the rect is not lost, it is folded onto the edge. The extremes
	// measure within 13% of the middle and the two of them measure the SAME as each other.
	//
	// So this case exists to hold that true, and to make the next person's version of the same guess
	// cheap to check. The real defect behind the symptom is in the DRAG, not the hit test — see
	// "a breakpoint lands where the pointer is, in one event" in the mseg suite.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace mseg = feathers::mod::mseg;

	feathers::ModRack rack = f.bridge.modRack();
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	for (size_t i = 0; i < 3; ++i) {
		shape.segment[i].share = feathers::mod::timeValueFor(0.2);
	}
	shape.segment[0].level = 1.0f;
	shape.segment[1].level = 0.5f;
	shape.segment[2].level = 0.0f;
	shape.sustainSegment = mseg::kNoSustain;
	rack.setMsegAt(0, shape);
	f.bridge.restoreModRack(rack);

	ui::MsegCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::MsegCard::height()));
	REQUIRE(card.layoutNow().handle.size() == 4);

	// THE SET OF POINTER POSITIONS THAT SELECT A HANDLE, counted through the card's OWN hit test —
	// which is the thing a hand actually meets. Swept over the whole card, because a press outside
	// the drawn editor can still be the editor's and that is exactly the mechanism under test.
	const auto selectableArea = [&card](int handle) {
		constexpr float kStep = 0.25f;
		int hits = 0;
		// THE CARD'S OWN WIDTH, NOT 2px PAST IT ON EACH SIDE (backlog MV): a press outside the card never reaches it, and
		// since the Trig pad's column took the left margin the first handle's reach is bounded by the card's rule
		// (`MsegCard::leftOfEditorReach`, mirroring the right margin) rather than by the card's edge, so overscan counted
		// on the right only.
		for (float px = 0.0f; px < ui::ModPanel::kCardWidth; px += kStep) {
			// **THE WINDOW FOLLOWS THE BODY.** It was written as -20..120 when a card's body started at
			// its top edge; the card frames itself now and its body starts at `bodyTop()`, so a fixed
			// window would frame a different slice of the editor and the areas would not be comparable
			// with each other, let alone with the figures recorded below. Same window, moved with the
			// thing it measures.
			// ASKED OF THE CARD, not of the type: `bodyTop()` depends on which EDGE the head is on, so
			// it stopped being a constant when the head learned to move with the lane.
			for (float py = card.bodyTop() - 20.0f; py < card.bodyTop() + 120.0f; py += kStep) {
				if (card.handleAtCardPoint({ px, py }) == handle) {
					++hits;
				}
			}
		}
		return static_cast<double>(hits) * static_cast<double>(kStep) * static_cast<double>(kStep);
	};

	const double leftmost = selectableArea(0);
	const double middle = selectableArea(1);
	const double rightmost = selectableArea(3);
	CAPTURE(leftmost);
	CAPTURE(middle);
	CAPTURE(rightmost);

	// NOT VACUOUS: every one of them is a real target rather than a few stray pixels.
	REQUIRE(middle > 100.0);

	// THE TWO EXTREMES ARE THE SAME AS EACH OTHER. This is the assertion that answers the report, and
	// it is an EQUALITY rather than a floor: if the right edge were special — clamped where the left
	// is not, or clipped by a neighbouring frame — these two would differ and the difference would be
	// the bug. Measured 536.6 and 530.2 square pixels.
	CHECK(rightmost == doctest::Approx(leftmost).epsilon(0.05));

	// AND NEITHER EXTREME IS MUCH SMALLER THAN THE MIDDLE. They ARE slightly smaller, and the reason
	// is not the hit test: the card is 216px wide, so the part of the grab region that would fall
	// beyond the card is claimed by whatever frame sits there instead. That costs about four pixels
	// of width at each end, symmetrically. Stated as a proportion with its cause rather than as a
	// bare bound, because the cause is what tells the next reader whether a change made it worse.
	CHECK(leftmost / middle > 0.85);
	CHECK(rightmost / middle > 0.85);

	SUBCASE("and the far handle is reachable from OUTSIDE the drawn editor, which is what saves it") {
		// The mechanism, asserted directly. A press to the right of the drawn box still selects the
		// last handle — remove editorGrabBounds' inflation and this is the assertion that fails,
		// along with the equality above.
		const visage::Bounds box = card.editorBounds();
		const float beyond = box.right() + 4.0f;
		REQUIRE(beyond > box.right());
		REQUIRE(beyond < ui::ModPanel::kCardWidth);
		const float atLevel = box.bottom() - card.layoutNow().handle[3].level * box.height();
		CHECK(card.handleAtCardPoint({ beyond, atLevel }) == 3);
	}
}
TEST_CASE("Off/On is a switch and any other named choice is a picker, however few entries it has") {
	// **THE COUNT WAS STANDING IN FOR THE QUESTION, and it was right until a control disagreed with
	// it.** Every card surface branched on `names.size() == 2`: a chip rather than a box, the
	// parameter name rather than the value, and no dropdown below three entries. Each was correct for
	// `Off`/`On`, where lit MEANS on and a menu is two clicks for what a nudge does — and each was
	// wrong for the delay's `Clean`/`Granular`, where a lit chip captioned "Delay Mode" never says
	// which of two words it is set to. So Mode is a dropdown, which also leaves room for further
	// modes such as analog and tape.
	//
	// **WHICH WIDGET A CONTROL BECAME IS NOT RECOVERABLE FROM THE RENDERED CARD** — a chip and a box
	// are both a bordered rectangle with a word in it, which is exactly why this went unnoticed. So
	// it is asked of the card's own widget lists rather than of its pixels.
	FilterFixture f;
	namespace dly = feathers::fx::delay;
	feathers::BridgeDeviceHost host { f.bridge };
	ui::GenericCard card { host, kNoInstance, dly::kDescriptor, nullptr };

	const auto boxOver = [&](dly::Param p) -> ui::ValueBox* {
		for (const auto& b : card.boxesForTest()) {
			if (&b->control().desc() == &dly::kParamDescs[static_cast<size_t>(p)]) {
				return b.get();
			}
		}
		return nullptr;
	};
	const auto chipOver = [&](dly::Param p) -> ui::SwitchChip* {
		for (const auto& c : card.chipsForTest()) {
			if (&c->control().desc() == &dly::kParamDescs[static_cast<size_t>(p)]) {
				return c.get();
			}
		}
		return nullptr;
	};

	SUBCASE("Mode is a picker, and it opens a menu at two entries") {
		ui::ValueBox* mode = boxOver(dly::Param::Mode);
		REQUIRE(mode != nullptr);
		CHECK(chipOver(dly::Param::Mode) == nullptr);
		// **THE MENU IS THE POINT, not the box.** A `ValueBox` that declined to open would look like a
		// picker and behave like a drag — the affordance lying, which is the objection that made the
		// chip exist in the first place. The floor of three entries is really a floor of "not Off/On".
		CHECK(mode->choiceMenuAvailable());
	}

	SUBCASE("Ping-Pong is a picker since it grew Classic, and the Off/On chips are untouched") {
		// PING-PONG WAS THE OFF/ON SWITCH HERE UNTIL 2026-10-11, when it became Off / Stereo / Classic: three named
		// states are a picker with every choice in its menu, never a chip that cycles (CLAUDE.md, the multivalue
		// toggle rule).
		ui::ValueBox* pingPong = boxOver(dly::Param::PingPong);
		REQUIRE(pingPong != nullptr);
		CHECK(chipOver(dly::Param::PingPong) == nullptr);
		CHECK(pingPong->choiceMenuAvailable());
		// THE OTHER HALF, and it is the half that says the change was narrow: Dub, still Off/On, is still a chip.
		ui::SwitchChip* dub = chipOver(dly::Param::Dub);
		REQUIRE(dub != nullptr);
		CHECK(boxOver(dly::Param::Dub) == nullptr);
		// A SWITCH PRINTS WHAT IT IS, because lit carries the value.
		CHECK(dub->caption() == dub->label());
	}
}
TEST_CASE("a stacked control shares its neighbour's cell and costs the row no column") {
	// Mode and the Dub button stack into one knob height. `ParamDesc::stacked` is that, and the
	// delay's `Dub` is its first user.
	//
	// **THE CLAIM IS GEOMETRY, AND GEOMETRY IS THE ONE THING A SCREENSHOT IS BAD AT.** Two controls
	// in one cell and two controls in adjacent cells both look like "two controls near each other"
	// at 60 pixels wide; what distinguishes them is that the x is IDENTICAL and the row did not
	// spend a column. So it is asked of the bounds.
	FilterFixture f;
	namespace dly = feathers::fx::delay;
	feathers::BridgeDeviceHost host { f.bridge };

	const size_t slot = f.bridge.addFxDevice(dly::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	ui::GenericCard card { host, f.bridge.fxDeviceAt(slot), dly::kDescriptor, nullptr };
	card.setBounds(0, 0, 256, 900);
	card.resized();

	const auto frameOver = [&](dly::Param p) -> visage::Frame* {
		for (const auto& b : card.boxesForTest()) {
			if (&b->control().desc() == &dly::kParamDescs[static_cast<size_t>(p)]) {
				return b.get();
			}
		}
		for (const auto& c : card.chipsForTest()) {
			if (&c->control().desc() == &dly::kParamDescs[static_cast<size_t>(p)]) {
				return c.get();
			}
		}
		for (const auto& k : card.knobsForTest()) {
			if (k->control() != nullptr && &k->control()->desc() == &dly::kParamDescs[static_cast<size_t>(p)]) {
				return k.get();
			}
		}
		return nullptr;
	};

	visage::Frame* mode = frameOver(dly::Param::Mode);
	visage::Frame* dub = frameOver(dly::Param::Dub);
	visage::Frame* time = frameOver(dly::Param::Time);
	visage::Frame* mix = frameOver(dly::Param::Mix);
	REQUIRE(mode != nullptr);
	REQUIRE(dub != nullptr);
	REQUIRE(time != nullptr);
	REQUIRE(mix != nullptr);

	SUBCASE("the pair shares one column, and Dub is under Mode") {
		CHECK(dub->x() == mode->x());
		CHECK(dub->width() == mode->width());
		CHECK(dub->y() > mode->y());
		// TOUCHING, NOT MERELY BELOW — a gap the size of a row would be two cells again.
		CHECK(dub->y() >= mode->y() + mode->height());
		CHECK(dub->y() < mode->y() + mode->height() + ui::GenericCard::kRowGap);
	}

	SUBCASE("and it costs the row no column — Mix is still on the top row") {
		// **THE HALF THAT MATTERS TO THE LAYOUT.** If `Dub` consumed a cell, the four-across row
		// would break after it and `Mix` would drop to a second row. Same y as its neighbours is
		// what says it did not.
		CHECK(mix->y() == time->y());
		CHECK(mix->x() > mode->x());
		// AND THE COLUMNS ARE EVENLY SPACED, so "same row" is not being satisfied by everything
		// piling into one place.
		CHECK(mode->x() > time->x());
	}

	SUBCASE("the pair fits inside one knob cell, which is the whole ask") {
		// Both controls plus the air between them, against the height a single knob cell gets.
		const float pair = static_cast<float>(dub->y() + dub->height() - mode->y());
		const float oneKnobCell = static_cast<float>(time->height());
		CAPTURE(pair);
		CAPTURE(oneKnobCell);
		CHECK(pair <= oneKnobCell);
		// NON-VACUITY: the pair is a real stack and not two zero-height frames.
		CHECK(pair > 0.0f);
		CHECK(dub->height() > 0);
		CHECK(mode->height() > 0);
	}
}
TEST_CASE("a section heading appears only when something under it is live") {
	// **THE HONESTY RULE, APPLIED TO A HEADING.** A title over four hidden rows is the same lie as a
	// control that does nothing — and the delay in `Clean` mode is exactly that case, since its four
	// grain controls are gated away by `relevantFor`.
	//
	// **AND IT IS ASSERTED ON THE HEADING LIST RATHER THAN ON THE PIXELS**, because a heading drawn
	// in the wrong place and a heading not drawn at all look identical in a screenshot of a card
	// that scrolls.
	FilterFixture f;
	namespace dly = feathers::fx::delay;
	feathers::BridgeDeviceHost host { f.bridge };

	const size_t slot = f.bridge.addFxDevice(dly::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	ui::GenericCard card { host, f.bridge.fxDeviceAt(slot), dly::kDescriptor, nullptr };
	card.setBounds(0, 0, 260, 900);

	const auto headings = [&]() {
		std::vector<std::string> out;
		for (const auto& h : card.headingsForTest()) {
			out.emplace_back(h.label == nullptr ? "" : h.label);
		}
		return out;
	};
	const auto has = [&](const char* label) {
		const std::vector<std::string> all = headings();
		return std::find(all.begin(), all.end(), std::string(label)) != all.end();
	};

	SUBCASE("in Clean mode the GRAIN section is ABSENT, not an empty heading") {
		host.setField(f.bridge.fxDeviceAt(slot), static_cast<size_t>(dly::Param::Mode), 0.0f);
		card.resized();
		// **THE HONESTY RULE APPLIED TO A HEADING.** Every control under GRAIN is gated on the Mode,
		// so in Clean there is nothing beneath it and it does not draw — a title over four hidden
		// rows is the same lie as a control that does nothing.
		//
		// **THIS SUBCASE USED TO ASSERT THE OPPOSITE**, when the section opened at `Mode` and so
		// always had its picker under it. `Mode` moved to the top row (one row: sync, time, mode,
		// mix), and the section now comes and goes as a block. Recorded rather than
		// quietly rewritten, because the old sentence was true of the old face.
		CHECK_FALSE(has("GRAIN"));
		CHECK(has("FEEDBACK"));
	}

	SUBCASE("every heading is above the rows it names, and OUT is last") {
		host.setField(f.bridge.fxDeviceAt(slot), static_cast<size_t>(dly::Param::Mode), 1.0f);
		card.resized();
		const std::vector<std::string> all = headings();
		// ORDER IS THE FACE ORDER, not the declaration order of the sections. The top row — sync,
		// time, mode, mix — deliberately carries no heading, so the first one seen is FEEDBACK.
		// DUCK / FREEZE IS GONE FROM THE CARD (its controls live in the window only), and its heading with it.
		REQUIRE(all.size() == 2u);
		CHECK(all[0] == "FEEDBACK");
		CHECK(all[1] == "GRAIN");
		// AND THEY GO DOWN THE CARD — a heading list in the right order with the wrong positions
		// would pass the check above and draw nonsense.
		for (size_t i = 1; i < card.headingsForTest().size(); ++i) {
			CHECK(card.headingsForTest()[i].y > card.headingsForTest()[i - 1].y);
		}
	}

	SUBCASE("the height the lane reserves covers the layout that is actually placed") {
		// **THE ONE CHECK THAT CATCHES THE TWO READINGS DRIFTING APART.** `contentHeight()` measures
		// the face through `extentOf`; `layOut` places it through the same `faceRowsOf` grouping.
		// Sharing the grouping is what SHOULD make them agree — but a known-bad that deleted the
		// heading height from `extentOf` alone left every other case in this file GREEN, because a
		// card that reserves 68 pixels too little still has all its headings, in order, going down
		// the card. The only visible symptom is the last row clipped off the bottom.
		//
		// Checked in BOTH modes, because the mode is what changes how many sections are live and the
		// shortfall is one heading per section.
		for (const float mode : { 0.0f, 1.0f }) {
			host.setField(f.bridge.fxDeviceAt(slot), static_cast<size_t>(dly::Param::Mode), mode);
			card.resized();
			CAPTURE(mode);
			CAPTURE(card.headingsForTest().size());
			CHECK(card.laidOutBottomForTest() > 0.0f); // non-vacuity: something was placed
			CHECK(card.contentHeight() >= card.laidOutBottomForTest());
		}
	}

	SUBCASE("the card is taller in Granular, by the rows the grain controls need") {
		// **THE LANE RESERVES FROM `heightFor`, WHICH IS STATIC AND DESCRIPTOR-ONLY** — the reason
		// sections had to live on `Traits` at all. This is the claim that the reserved height follows
		// the live set rather than the declared one.
		host.setField(f.bridge.fxDeviceAt(slot), static_cast<size_t>(dly::Param::Mode), 0.0f);
		card.resized();
		const float clean = card.contentHeight();
		host.setField(f.bridge.fxDeviceAt(slot), static_cast<size_t>(dly::Param::Mode), 1.0f);
		card.resized();
		CHECK(card.contentHeight() > clean);
	}
}

