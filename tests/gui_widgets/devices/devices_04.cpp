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

// PIECE 4 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("a click away from the centre moves the field, and the puck with it") {
	// Clicking somewhere moves the puck too. It did not, and the cause was that the
	// press ANCHORED the gesture where it landed: `anchor_` was set to the click position, so the
	// distance between the centre and where you pressed became nobody's delta. The first drag event
	// measured from the click, and the click's own displacement was dropped on the floor.
	//
	// The press now anchors at the CENTRE, which makes a click the first move of the gesture rather
	// than a preamble to it — and costs nothing afterwards, because the commit re-anchors to the
	// pointer, so a drag continuing from the same press still commits incremental deltas.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);

	const auto committed = [&] {
		const FxChain chain = f.f.bridge.fxChain();
		double magnitude = 0.0;
		for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
			magnitude += std::fabs(fxDeviceAtPosition(chain, f.slot).row.field[shp::kTravelFirst + i]);
		}
		return magnitude;
	};

	REQUIRE(committed() == doctest::Approx(0.0));

	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
	const float off = pad->width() * 0.25f;

	// **A PRESS AND RELEASE WITH NO DRAG AT ALL** — the gesture in question. No `onMouseDrag`
	// between them, so anything committed here is the click's own displacement and nothing else.
	pad->onMouseDown().callback(at(centre.x + off, centre.y));
	// THE PUCK IS UNDER THE POINTER WHILE THE BUTTON IS DOWN.
	CHECK(pad->puck().x == doctest::Approx(centre.x + off));
	CHECK(pad->puck().y == doctest::Approx(centre.y));
	pad->onMouseUp().callback(at(centre.x + off, centre.y));
	pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)

	CAPTURE(committed());
	CHECK(committed() > 1e-6);
	// AND HOME AGAIN ON RELEASE: the travel is committed, so the chart's origin is the new position.
	CHECK(pad->puck().x == doctest::Approx(centre.x));

	// **A CLICK EXACTLY ON THE CENTRE COMMITS NOTHING**, which is the honest answer rather than a
	// special case — the displacement is nil. Without this the case above would pass on a pad that
	// commits some fixed lurch on every press regardless of where it lands.
	ShaperFixture g;
	REQUIRE(g.panel != nullptr);
	auto* pad2 = const_cast<ui::ShaperPad*>(g.panel->padForTest());
	REQUIRE(pad2 != nullptr);
	const visage::Point c2 { pad2->width() * 0.5f, pad2->height() * 0.5f };
	pad2->onMouseDown().callback(at(c2.x, c2.y));
	pad2->onMouseUp().callback(at(c2.x, c2.y));
	pad2->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)
	const FxChain chain = g.f.bridge.fxChain();
	double still = 0.0;
	for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
		still += std::fabs(fxDeviceAtPosition(chain, g.slot).row.field[shp::kTravelFirst + i]);
	}
	CHECK(still == doctest::Approx(0.0));
}
TEST_CASE("the plane's turn is unbounded, and past 1 is real ground") {
	// Rotate worked but was stuck in a finite 0-100 range; Explore's unbounded rotate is the model.
	//
	// **THE WALL WAS REAL, NOT COSMETIC, AND THE MEASUREMENT IS WHY.** `planeTurnRate` spaces the
	// coordinates on {0.618, 0.382, 0.236, 0.146, 0.090}, so across the WHOLE old 0..1 declaration the
	// fastest coordinate advanced 62% of one triangle period and the slowest 9% — not one full turn on
	// any of them. The angular EXTENT saturates early (24.2 degrees by rotate 1, 28.6 by rotate 100),
	// which is why turning further does not aim further away; what keeps growing is the COUNT of
	// distinct planes: **11 distinct x-directions at 5-degree resolution over 0..1, 121 over 0..11,
	// 960 over 0..100.** That is Explore's own reading of its rotate — *"a re-aiming rather than an
	// amount, so it has no ends" — and the old declaration cut it off after eleven.
	//
	// **THE BRIDGE CLAMPS TO THE DECLARED RANGE**, so this is the assertion that matters: before the
	// change, writing 5.0 stored 1.0. Asserted on the stored VALUE rather than on a knob's appearance,
	// because the clamp is what the player was hitting.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);

	const device::ParamDesc& d = shp::kParamDescs[static_cast<size_t>(shp::Param::Rotate)];
	// SYMMETRIC, BECAUSE A TURN HAS TWO DIRECTIONS AND THE DEFAULT SITS AT ZERO. Declared 0..N the way
	// gamma is, the first backwards drag would wall at the place every patch starts.
	CHECK(d.min < -1000.0);
	CHECK(d.max > 1000.0);
	CHECK(d.def == doctest::Approx(0.0));

	const auto stored = [&] {
		const FxChain chain = f.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, f.slot).row.field[static_cast<size_t>(shp::Param::Rotate)];
	};

	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Rotate), 5.0f);
	CHECK(stored() == doctest::Approx(5.0f));
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Rotate), -37.5f);
	CHECK(stored() == doctest::Approx(-37.5f));

	// **AND THE GROUND PAST 1 IS DIFFERENT GROUND, not a repeat of what 0..1 already covered.** Without
	// this the range change would be a bigger number with nothing behind it: the claim is that the
	// mechanism keeps yielding new planes, so it is asked of the mechanism.
	const auto direction = [](double rotate) {
		const feathers::SliceBasis<shp::phase_space::kPhaseCount> b
		    = feathers::sliceBasisFor<shp::phase_space::kPhaseCount>(rotate, 0.37);
		return b.x;
	};
	const auto angleBetween = [](const auto& u, const auto& v) {
		double dot = 0.0;
		double nu = 0.0;
		double nv = 0.0;
		for (size_t i = 0; i < u.size(); ++i) {
			dot += static_cast<double>(u[i]) * static_cast<double>(v[i]);
			nu += static_cast<double>(u[i]) * static_cast<double>(u[i]);
			nv += static_cast<double>(v[i]) * static_cast<double>(v[i]);
		}
		return std::acos(std::clamp(dot / (std::sqrt(nu) * std::sqrt(nv)), -1.0, 1.0)) * 180.0 / 3.14159265358979;
	};
	const auto atOne = direction(1.0);
	const auto atFive = direction(5.0);
	const auto atForty = direction(40.0);
	// Each is a genuinely different aim, not the same plane reached again.
	CHECK(angleBetween(atOne, atFive) > 2.0);
	CHECK(angleBetween(atOne, atForty) > 2.0);
	CHECK(angleBetween(atFive, atForty) > 2.0);
}
TEST_CASE("the shaper's gamma is unbounded, and it is a box already") {
	// Gamma was stuck at 0-100%: already a value box, but bound to 100%. That is the whole shape of
	// this fix: the SEATING was right and the DECLARATION was wrong. `ShaperExpanded` has built a
	// `GammaBox` for this control since the nonconforming-knob sweep; what walled it was `0..1` with
	// `Unit::Percent`, which is a drag window mistaken for a domain.
	//
	// **THE STANDING DECISION IS THAT THE SHAPER'S GAMMA CONFORMS, AND `gamma_box.h` WAS WRITTEN
	// EXPECTING THIS EDIT** — the field is infinite too — with a span of `min(sixteen laps, the
	// declared range)` precisely so that widening the range here needs no change there. So this case
	// asserts the declaration, and the drag sensitivity follows from a rule that was already correct.
	//
	// **AND IT IS THE SAME MISREADING THAT ONCE COST A WHOLE PROPOSAL.** An XY design was built on
	// gamma's `0..1` read as a bound, and gamma is never bounded.
	// A declared range in this codebase is a window unless something says otherwise.
	namespace shp = feathers::fx::shaper;
	const device::ParamDesc& d = shp::kParamDescs[static_cast<size_t>(shp::Param::Gamma)];
	CHECK(d.min < -1000.0);
	CHECK(d.max > 1000.0);
	CHECK(d.def == doctest::Approx(0.0));
	// NOT A PERCENTAGE: a percentage of nothing is what the old unit was claiming.
	CHECK(d.unit == device::Unit::Number);

	// **AND THE VALUE SURVIVES THE ROAD**, which is the part the player was hitting — the bridge
	// clamps to the declared range, so before this change writing 4.0 stored 1.0.
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Gamma), 4.0f);
	const FxChain chain = f.f.bridge.fxChain();
	CHECK(fxDeviceAtPosition(chain, f.slot).row.field[static_cast<size_t>(shp::Param::Gamma)]
	      == doctest::Approx(4.0f));
}
TEST_CASE("widening a range must not change how fast a drag moves it") {
	// **THE WALL AND THE SENSITIVITY WERE THE SAME NUMBER, AND ONLY THE WALL WAS MEANT TO GO.**
	//
	// With gamma driven hard, a tiny bump on fine Colour seemed barely to change the transfer, as if
	// gamma were ignored. The field was fine. `ValueBox` with no explicit span drags its WHOLE declared range across one
	// 140-pixel travel, and `GammaBox` asks for `min(sixteen laps, the range)` — so while these were
	// declared 0..1 the clamp held them to ONE unit per drag, and widening the declaration silently
	// released them to SIXTEEN. The same gesture began covering sixteen times the ground.
	//
	// **MEASURED, AND THE FIELD SATURATES SO THE EXTRA TRAVEL BOUGHT ALMOST NOTHING.** Counting curves
	// more than 0.15 apart at colour 0.38: gamma 0..1 holds 351 distinct curves, 0..4 holds 679, 0..16
	// holds 758, 0..64 holds 763. Sixteen units per drag re-treads most of its travel while making
	// every pixel jump about five distinct curves.
	//
	// This case exists because the coupling is INVISIBLE at the edit site: nothing in a descriptor
	// says a number there is also a sensitivity, and the next person to remove a wall will not be
	// told either.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);

	const auto spanOf = [&](shp::Param p) -> double {
		for (visage::Frame* cell : f.panel->cellsForTest()) {
			auto* box = dynamic_cast<ui::ValueBox*>(cell);
			if (box == nullptr) {
				continue;
			}
			if (box->label() == shp::shortLabelOf(p)) {
				return box->dragSpanForTest();
			}
		}
		return -1.0;
	};

	// ONE UNIT PER DRAG FOR BOTH — the sensitivity each had while it was walled at 1.0.
	CHECK(spanOf(shp::Param::Gamma) == doctest::Approx(1.0));
	// ROTATE LIVES IN THE TERRAIN BOX SINCE backlog IJ, not among the row cells — asked of its own seam.
	REQUIRE(f.panel->rotateForTest() != nullptr);
	CHECK(f.panel->rotateForTest()->dragSpanForTest() == doctest::Approx(1.0));

	// **AND THE RANGES ARE STILL UNBOUNDED**, which is what makes the two assertions above a statement
	// about DECOUPLING rather than a restatement of the old declaration.
	CHECK(shp::kParamDescs[static_cast<size_t>(shp::Param::Gamma)].max > 1000.0);
	CHECK(shp::kParamDescs[static_cast<size_t>(shp::Param::Rotate)].max > 1000.0);
}
TEST_CASE("the terrain answers to knee and to each extra") {
	// The terrain looked rendered before the extras, since changing an extra did not change it — and
	// separately knee did not move it either. Two causes, only one a bug.
	//
	// **KNEE WAS A CACHE-KEY BUG.** `rebuildMap` has always passed it to `deriveCurve`, and it reaches
	// `drive` and `preExpandAmount`, which `transferAt` reads — but `View`, the staleness key, named
	// only colour, gamma, rotate, zoom and the standing travel. So it changed the terrain's INPUT
	// without ever asking for a rebuild. **A cache key must name every input its value reads**; this
	// one was written when the terrain read colour and gamma alone and grew inputs without growing.
	//
	// **THE EXTRAS WERE A GAP NO KEY COULD HAVE CLOSED.** The terrain is the total variation of
	// `transferAt`, and `transferAt` reads only the six weights, asymmetry, drive and preExpand —
	// never `subIntensity`, `feedback`, `rotation` or `slewIntensity`, which are applied per-sample
	// AFTER the table read. MEASURED across a 33x33 field grid those amounts span nearly their whole
	// range (sub 0.000-0.993, feedback 0.000-0.800, slew 0.000-0.999, rotation 0-30 bits), so with a
	// switch on the picture was hiding most of what varies across the plane.
	//
	// **THE POSITION IS CHOSEN, NOT DEFAULT, AND THAT IS THE POINT OF THIS COMMENT.** A first draft
	// ran at the fixture's default and failed on Sub — correctly, because MEASURED there
	// `subIntensity` is exactly 0.000 and an extra whose amount is zero has nothing to show. Colour
	// 0.875, gamma 0.675 is the position where all four amounts are jointly live (sub 0.790, feedback
	// 0.625, slew 0.736, rotation 27 bits). **A terrain can only draw what the field has, so a case
	// about the terrain has to stand somewhere the field is speaking.**
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	visage::Canvas canvas;
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Colour), 0.875f);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Gamma), 0.675f);

	const size_t cells = pad->mapForTest().size();
	const auto cellsMovedAfter = [&](shp::Param p, float value) {
		pad->draw(canvas);
		const auto before = pad->mapForTest();
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(p), value);
		pad->draw(canvas);
		const auto& after = pad->mapForTest();
		size_t moved = 0;
		for (size_t i = 0; i < before.size(); ++i) {
			if (before[i] != after[i]) {
				++moved;
			}
		}
		return moved;
	};

	// A GOOD FRACTION OF THE FIELD, not merely non-zero: one cell moving would satisfy "it rebuilt"
	// while still describing a picture that barely answers.
	CHECK(cellsMovedAfter(shp::Param::Knee, 0.9f) > cells / 8);

	// **EACH EXTRA ON ITS OWN**, because they are averaged over the ENABLED terms — a bug dropping one
	// of the four would be invisible if they were all switched together. EACH BY ITS POWER, at the
	// amount's default of 50% — the field's own, what the old switch's On gave (backlog II, IK). At an
	// amount of 100% the scaler lifts every cell to the full amount and the field's variation is gone.
	CHECK(cellsMovedAfter(shp::Param::SubOn, 1.0f) > cells / 8);
	CHECK(cellsMovedAfter(shp::Param::FeedbackOn, 1.0f) > cells / 8);
	CHECK(cellsMovedAfter(shp::Param::MangleOn, 1.0f) > cells / 8);
	CHECK(cellsMovedAfter(shp::Param::LowpassOn, 1.0f) > cells / 8);

	// **AND EMPHASIS DOES NOT MOVE THE TERRAIN, deliberately.** Its slot was `Harmonics`, which fed the
	// slew duty and so reached this picture through a slew switch. It became Emphasis on 2026-09-22 —
	// a frequency tilt around the curve, not a term of the curve — and the slew duty is now fixed at
	// `kSlewHarmonics`. A position map cannot show a frequency tilt, so the honest terrain is unmoved.
	CHECK(cellsMovedAfter(shp::Param::Emphasis, 7.0f) == 0);
}
TEST_CASE("with every extra off the terrain is exactly what it always was") {
	// **THE OTHER HALF OF THE EXTRAS TERM, and the reason it is safe to add.** `extrasActivityOf`
	// returns 0 when no switch is on, so a device with its switches off draws the terrain it drew
	// before the term existed. Without this the change would be a recalibration of a picture that has
	// already been read for hours, dressed up as an addition.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	visage::Canvas canvas;
	// THE SAME LIVE POSITION the case above uses — at the default, Sub's amount is 0.000 and the round
	// trip below would be trivially true for the wrong reason.
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Colour), 0.875f);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Gamma), 0.675f);
	pad->draw(canvas);
	const auto plain = pad->mapForTest();

	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::SubOn), 1.0f);
	pad->draw(canvas);
	size_t movedOn = 0;
	for (size_t i = 0; i < plain.size(); ++i) {
		if (plain[i] != pad->mapForTest()[i]) {
			++movedOn;
		}
	}
	// NON-VACUITY: the switch did something, so the return below is a real round trip.
	CHECK(movedOn > 0);

	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Sub), 0.0f);
	pad->draw(canvas);
	// BITWISE, because the term is added and then not added — not scaled down.
	size_t differing = 0;
	for (size_t i = 0; i < plain.size(); ++i) {
		if (pad->mapForTest()[i] != plain[i]) {
			++differing;
		}
	}
	CHECK(differing == 0);
}
TEST_CASE("the second row sits beneath the controls it belongs to, and the card still fits its lane") {
	// It sits beneath Mix — a second knob row on both surfaces, under the control
	// that decides where the crossfade starts.
	//
	// **AND THE CARD IS NOW 8px FROM ITS LIMIT, WHICH IS WHY THIS CASE ASSERTS THE GEOMETRY RATHER
	// THAN THE ARITHMETIC.** MEASURED: the card summed to 253px of a 336px lane unit before this, and
	// a knob column plus a gap takes it to 328. `shaper_card.cpp` already records the decision against
	// exactly this 8px for Hysteresis — "inside the slop between the derived figure and a capture" —
	// so a height that is merely *computed* to fit is not evidence here. Every cell is asked where it
	// actually landed.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.card != nullptr);
	REQUIRE(f.panel != nullptr);

	SUBCASE("on the card, beneath Mix and inside the card") {
		const auto& cells = f.card->cellsForTest();
		REQUIRE(cells.size() == ui::ShaperCard::kKnobCells);
		// **ADDRESSED BY DECLARATION ORDER, NOT BY "THE LAST CELL".** This read `kKnobCells - 1` and
		// meant "Aggression", which stopped being true the moment Trim joined the second row behind
		// it — a positional alias that survives one addition and silently names the wrong widget on
		// the next. The row's tenants are `kTopRowCells + 0` and `+ 1`, in `kCardKnobs` order.
		visage::Frame* mix = cells[ui::ShaperCard::kAggressionColumn];
		visage::Frame* drive = cells[ui::ShaperCard::kTrimColumn];
		visage::Frame* aggr = cells[ui::ShaperCard::kTopRowCells];
		visage::Frame* trim = cells[ui::ShaperCard::kTopRowCells + 1];
		REQUIRE(mix != nullptr);
		REQUIRE(drive != nullptr);
		REQUIRE(aggr != nullptr);
		REQUIRE(trim != nullptr);

		// BENEATH: each takes the left edge and width of the control it sits under, and is strictly
		// lower. Trim under Drive because both decide what reaches the curve; Aggression under Mix
		// because both decide where and how abruptly it takes over.
		CHECK(aggr->x() == doctest::Approx(mix->x()));
		CHECK(aggr->width() == doctest::Approx(mix->width()));
		CHECK(aggr->y() > mix->bounds().bottom() - 1.0f);
		CHECK(trim->x() == doctest::Approx(drive->x()));
		CHECK(trim->width() == doctest::Approx(drive->width()));
		CHECK(trim->y() > drive->bounds().bottom() - 1.0f);
		// AND THEY DO NOT OVERLAP EACH OTHER, which two cells sharing a row can do if both are
		// pinned to the same column by a copy-paste.
		CHECK(trim->bounds().right() <= aggr->x() + 0.5f);

		// **AND INSIDE THE CARD**, which is the assertion the 8px demands. The extras pills moved down
		// with it, so they are checked too — a row drawn off the bottom is the failure this file has
		// already seen once, when the fifth switch was clipped in half and a screenshot caught it.
		const float bottom = ui::ShaperCard::openHeight();
		CAPTURE(bottom);
		CAPTURE(aggr->bounds().bottom());
		CHECK(aggr->bounds().bottom() <= bottom + 0.5f);
		CHECK(trim->bounds().bottom() <= bottom + 0.5f);
		for (size_t i = 0; i < shp::kExtraSwitchCount; ++i) {
			const visage::Bounds pill = f.card->extraBounds(i);
			CAPTURE(i);
			CHECK(pill.bottom() <= bottom + 0.5f);
			// AND BELOW THE NEW ROW rather than overlapping it — the two share a derived top, so a
			// height that grew in one place and not the other would land them on each other.
			CHECK(pill.y() >= aggr->bounds().bottom() - 0.5f);
		}
	}

	SUBCASE("on the panel, beneath Mix and inside the size it asks for") {
		const auto& cells = f.panel->cellsForTest();
		REQUIRE(cells.size() == ui::ShaperExpanded::kKnobCells);
		visage::Frame* mix = cells[ui::ShaperExpanded::kAggressionColumn];
		visage::Frame* drive = cells[ui::ShaperExpanded::kTrimColumn];
		visage::Frame* aggr = cells[ui::ShaperExpanded::kTopRowCells];
		visage::Frame* trim = cells[ui::ShaperExpanded::kTopRowCells + 1];
		REQUIRE(mix != nullptr);
		REQUIRE(drive != nullptr);
		REQUIRE(aggr != nullptr);
		REQUIRE(trim != nullptr);
		CHECK(aggr->x() == doctest::Approx(mix->x()));
		CHECK(aggr->y() > mix->bounds().bottom() - 1.0f);
		CHECK(trim->x() == doctest::Approx(drive->x()));
		CHECK(trim->y() > drive->bounds().bottom() - 1.0f);
		CHECK(trim->bounds().right() <= aggr->x() + 0.5f);
		// The panel clamps rather than scrolling, so its own request has to cover both rows.
		CHECK(aggr->bounds().bottom() <= f.panel->minimumSize().y + 0.5f);

		// **AND NOTHING SITS ON TOP OF THE EXTRAS ROW.** In the window the aggression knob once lay on
		// top of the extras buttons. It did — `extraBounds`
		// derived its top as "below the knob row" and there were two knob rows, so the switches were
		// laid at exactly the second row's y. The card's copy of that derivation had been moved down
		// when the row was added and the panel's had not.
		//
		// **THE EXISTING LAYOUT ASSERTION COULD NOT SEE IT**, which is the lesson worth keeping: it
		// checked every cell against the panel's declared SIZE, and both widgets were comfortably
		// inside it — they were inside it in the same place. A containment check says nothing about
		// two things containing each other, so the overlap is asserted directly.
		for (size_t i = 0; i < shp::kExtraSwitchCount; ++i) {
			const visage::Bounds pill = f.panel->extraBounds(i);
			CAPTURE(i);
			CHECK(pill.y() >= aggr->bounds().bottom() - 0.5f);
			CHECK(pill.bottom() <= f.panel->minimumSize().y + 0.5f);
		}
	}
}
TEST_CASE("head only - a double-click on an fx card's BODY does not fold it, and on its head does") {
	// **THE OTHER HOST'S HALF OF ONE RULE.** "Head only" (2026-09-05) was decided for the
	// modulator strip, where the behaviour was met; the road is `FxCard::noteCardRelease` and `FxCard`
	// is BOTH lanes', so the fx lane changes with it. That is the right outcome rather than a side
	// effect — a device collapsing to a sliver from anywhere on its face is the same defect wherever
	// it lives — and this case is here so the fx lane's half is asserted rather than inherited.
	//
	// The mod strip's twin is `test_gui_widgets_dahdsr.cpp`, "a double-click on a modulator card
	// MINIMISES it"; the rack's head road is `test_rack_editor.cpp`, "a double-click folds a rack card
	// ONCE", whose head is a SPINE down the left because its lane flows in x.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	lane->resized();
	lane->scrollCardIntoView(1);
	lane->resized();

	ui::FxCard* card = lane->cardAt(1);
	REQUIRE(card != nullptr);
	// THE CARD'S OWN ANSWER ABOUT WHERE ITS HEAD IS — this lane flows in y, so the head is the top
	// row. Asked rather than written out, which is the mistake the rack's case carried for weeks.
	REQUIRE(card->headEdge() == ui::CardHeadEdge::Top);
	REQUIRE_FALSE(card->folded());

	const auto doubleClickAt = [&](visage::Point local) {
		for (int click = 1; click <= 2; ++click) {
			visage::MouseEvent e {};
			e.position = local;
			e.event_frame = card;
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = visage::kMouseButtonLeft;
			e.is_down = true;
			e.repeat_click_count = click;
			card->onMouseDown().callback(e);
			e.is_down = false;
			e.button_state = visage::kMouseButtonNone;
			card->onMouseUp().callback(e);
		}
	};

	SUBCASE("the body does nothing") {
		// WELL CLEAR OF THE HEAD, and of the card's own bottom furniture: three-quarters down, which
		// is the point that folded a modulator before the rule.
		doubleClickAt({ static_cast<float>(card->width()) * 0.5f,
		                static_cast<float>(card->height()) * 0.75f });
		CHECK_FALSE(card->folded());
	}

	SUBCASE("and the head still folds, which is the half that passes on the broken build") {
		// **KEPT BESIDE THE OTHER ON PURPOSE.** Asserting only this one is satisfied by a card that
		// folds from anywhere — which is exactly what was there — and asserting only the first is
		// satisfied by a card that has stopped folding at all.
		doubleClickAt({ static_cast<float>(card->width()) * 0.5f, ui::kCardHeadHeight * 0.5f });
		CHECK(card->folded());
	}
}
TEST_CASE("clicking a KNOB on an fx card selects that card - by design, anywhere on the card") {
	// **Clicking anywhere on the card, a knob included, selects it.** The rack's twin is in `test_rack_editor.cpp`.
	//
	// **BY TYPE, WHICH IS WHAT MAKES THIS DIFFERENT FROM THE CASE ABOVE.** "the fx lane's Backspace
	// takes the card under the hand" presses the first visible child of whatever card it uses; the claim
	// names the KNOB, so a `dynamic_cast` picks one and a `REQUIRE` fails rather than quietly falling
	// back to some other child if a card stops having knobs.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 1);
	lane->resized();
	lane->scrollCardIntoView(0);
	lane->resized();

	ui::FxCard* card = lane->cardAt(0);
	REQUIRE(card != nullptr);
	ui::Knob* knob = nullptr;
	for (visage::Frame* child : card->children()) {
		if (auto* k = dynamic_cast<ui::Knob*>(child); k != nullptr && k->isVisible() && k->width() > 2.0f) {
			knob = k;
			break;
		}
	}
	REQUIRE(knob != nullptr);

	// SELECT THE OTHER CARD FIRST, or a stale index would happen to be right.
	lane->focusSlot(1);
	REQUIRE(lane->focusedSlotForTest() == 1);

	const visage::Point where =
	    knob->positionInWindow() + visage::Point { knob->width() * 0.5f, knob->faceDiameter() * 0.5f };
	visage::Frame* target = f.root->frameAtPoint(where);
	REQUIRE(target != nullptr);
	// THE PRESS REALLY LANDS ON THE KNOB, or this is the ordinary body press covered elsewhere.
	CHECK(dynamic_cast<ui::Knob*>(target) != nullptr);
	visage::Frame* focus = keyboardFocusAfterPress(f.root, where);
	REQUIRE(focus != nullptr);
	CHECK(focus == static_cast<visage::Frame*>(card));
	focus->processFocusChanged(true, true);

	// **THE RULE, AS A VALUE.**
	CHECK(lane->focusedSlotForTest() == 0);
}
TEST_CASE("Feathers' host is addressed by DEVICE ID, on chains where the record's slot and the position differ") {
	// **THE CLAIM `routeLaneOf` USED TO CARRY, ASSERTED WITHOUT IT (BX step 4), THEN WITHOUT THE LANE
	// (kill-lanes step 2), AND NOW WITHOUT A CHAIN THAT HAS ONE TO EDIT (step 3b).**
	//
	// This case read `CHECK(f.deviceHost.routeLaneOf(lane) == lane)` while the virtual existed, and then
	// "a host asked at a lane reads THE DEVICE AT THAT LANE". The seam names a device by its id now, so
	// the claim is: **a host asked for an id reads THAT DEVICE**, checked by value on a chain where the
	// two numbers this seam has ever been addressed by — the record's own pool slot and the device's
	// position — are different, so neither old address could answer by coincidence.
	//
	// AND IT ASKS BOTH CHAINS, because the third implementation is what held the virtual up.
	// `ModRackDeviceHost` was addressed by MOD RACK SLOT and `device_host.h` never knew it existed —
	// its retirement note said "both implementations" while there were three. A case that checked only
	// the fx host would go green on exactly the arrangement that blocked this for two commits.
	//
	// **ON A DIVERGED CHAIN, OR IT PROVES NOTHING.** On a chain that has only ever been appended to, the
	// two are the same number and an addressing bug returns the identity by coincidence.
	//
	// **THE DIVERGENCE IS MADE IN THE RECORD NOW, NOT IN A HAND-BUILT CHAIN VALUE (step 3b).** It used to
	// edit an `FxChain` value until its lanes and positions disagreed and restore that; a restore carries
	// only the ORDER since 3b, so a value's lanes never reach the record and the arrangement never
	// arrived. The record's own verbs make it instead — delete the head, then add, so the new device
	// takes the FREED slot and stands LAST — which is the same arrangement, made where the two numbers
	// actually live.
	//
	// **AND THE PARKING IS ASKED OF THE GRAPH DIRECTLY, WHICH IS THE ONE PLACE A CASE MAY STILL DO IT.**
	// Every other case wants the SURFACE's number, which is the POSITION, and asks for it by position —
	// the coordinate vocabulary a shim used to cross into is gone (kill-lanes step 5), and `laneOf` is a
	// pool index that never leaves its pool. This case is the exception because its whole subject is the
	// gap between the two numbers — the road under test is `fieldAt(id)` → `fxSlotOf(id)` →
	// `fxFieldAt(position)`, and `graph().laneOf(id)` is the other plausible-looking "where is this
	// device" that road could have been written with. A case about which index a seam uses has to be able
	// to name both indices.
	StageFixture f;
	REQUIRE(f.bridge.addFxDevice(feathers::fx::shaper::kTraits.tag) < feathers::kMaxFxSlots);
	REQUIRE(f.bridge.addFxDevice(feathers::fx::filter::kTraits.tag) < feathers::kMaxFxSlots);
	// DELETE THE HEAD, THEN ADD: the new device goes to the END of the chain holding the FREED slot.
	REQUIRE(f.bridge.removeFxDevice(0));
	REQUIRE(f.bridge.addFxDevice(feathers::fx::shaper::kTraits.tag) < feathers::kMaxFxSlots);
	REQUIRE(f.bridge.fxDeviceCount() == 3);

	// **A DISTINCT VALUE PER POSITION, SO A WRONG ROW IS CAUGHT BY THE NUMBER AND NOT BY A SHAPE.** The
	// rule this file works under: a claim about a value cannot be inferred from anything but the value.
	// Field 0 exists on every device in this chain.
	//
	// WRITTEN THROUGH A RESTORE rather than through `setDeviceControl`, because a control write coerces
	// against the declaration and three values inside one control's range is not something a descriptor
	// owes anybody — a clamp that collapsed them would leave this case comparing a number with itself.
	// A restore of the same ids in the same order is a no-op structurally, so nothing moves.
	//
	// **AND THE ROW IS REACHED BY THE POSITION DIRECTLY (kill-lanes step 3c).** This went through
	// `fxLaneAtPosition(chain, at)` because the value's array was keyed by the chain's own lane; the
	// array is dense in `[0, count)` by POSITION now, so `device[at]` IS the device standing at `at`.
	// Note this is the CHAIN VALUE's numbering and says nothing about the record's parking — the two
	// disagreeing is the whole subject below, and `graph().laneOf` is the only thing that answers that.
	FxChain chain = f.bridge.fxChain();
	for (size_t at = 0; at < feathers::fxChainLength(chain); ++at) {
		chain.device[at].row.field[0] = 0.125f * static_cast<float>(at + 1);
	}
	f.bridge.restoreFxChain(chain);

	// **THE PAIR THAT DISAGREES, FOUND RATHER THAN ASSUMED.** A position whose record slot is a different
	// number AND is itself a live position — so both addressings name a real device and the two answers
	// can be compared as values rather than as a presence.
	const auto parkedAt = [&](size_t position) {
		return f.bridge.chainRecordForTest().graph().laneOf(f.bridge.fxDeviceAt(position));
	};
	size_t divergedAt = feathers::kMaxFxDevices;
	for (size_t at = 0; at < f.bridge.fxDeviceCount(); ++at) {
		const size_t lane = parkedAt(at);
		if (lane != at && lane < f.bridge.fxDeviceCount()) {
			divergedAt = at;
		}
	}
	REQUIRE(divergedAt < feathers::kMaxFxDevices);

	SUBCASE("a device in the lane: the host reads THE DEVICE, not the one standing at its slot's index") {
		const size_t lane = parkedAt(divergedAt);
		const float atPosition = f.bridge.fxFieldAt(divergedAt, 0);
		const DeviceInstanceId id = f.bridge.fxDeviceAt(divergedAt);
		CAPTURE(divergedAt);
		CAPTURE(lane);
		CAPTURE(id);
		REQUIRE(id != kNoInstance);
		CHECK(f.deviceHost.fieldAt(id, 0) == doctest::Approx(atPosition));

		// **THE CONTROL, COMPUTED BY AN INDEPENDENT RESTATEMENT OF THE WRONG PATH.** A host that took
		// this device's RECORD SLOT for a position answers the device STANDING at that index, and the two
		// are required to be different numbers before the inequality below means anything — otherwise it
		// passes on a chain where the two addressings agree.
		const float bySlotAsPosition = f.bridge.fxFieldAt(lane, 0);
		REQUIRE(bySlotAsPosition != doctest::Approx(atPosition));
		CHECK(f.deviceHost.fieldAt(id, 0) != doctest::Approx(bySlotAsPosition));
	}

	SUBCASE("and a device in the strip — the third implementation, which held the virtual up") {
		// **THE STRIP WAS A SECOND SLOT SPACE WITH A HOST OF ITS OWN** (`ModRackDeviceHost`), and
		// handing a modulator card the fx host read an unrelated device. One host serves both chains
		// now, because an id has one space: the same host, asked for the strip's device, must answer
		// from the strip's row.
		REQUIRE(f.bridge.modRack().count >= 1);
		const DeviceInstanceId id = f.bridge.rackDeviceAt(0);
		REQUIRE(id != kNoInstance);
		REQUIRE(f.bridge.fxSlotOf(id) >= feathers::kMaxFxDevices); // not an fx device by another name

		// THE DEVICE'S OWN WIDTH, which only the right row can answer.
		REQUIRE(f.bridge.rackFieldsPresentAt(0) > 0);
		CHECK(f.deviceHost.fieldsPresentAt(id) == f.bridge.rackFieldsPresentAt(0));
		// AND AN ID NOTHING WEARS: nothing. The assertion that the host answers for a DEVICE rather than
		// for a number — a host still keyed by slot would read row 0 for "no device" and fail this.
		CHECK(f.deviceHost.fieldsPresentAt(kNoInstance) == 0);
	}
}
// A CASE STOOD HERE — "the backplane's mayRoute and Feathers' maySourceRouteTo now run ONE routing
// rule" — and it retired with the fold that made it tautological. It compared the two predicates'
// answers; `maySourceRouteTo` IS a call into `mayRoute` now, so the comparison could not fail for any
// reason but the compiler's. What replaced it as the instrument is the mutation the fold's commit
// records: break `mayRoute`'s clause and Feathers' own routing cases go red THROUGH
// `maySourceRouteTo`, where before the fold they stayed green.

TEST_CASE("a switched-off device's card greys as a whole - the veil over every child, per card type") {
	// Devices had different semantics for "disabled grey": some greyed their knobs, some (OWLTT among
	// them) did not. DECISION: a disabled device's card greys as a whole.
	//
	// **THE SWEEP THAT PRECEDED THIS, MEASURED ON f33e3d3 WITH A REAL POINTER, TEN CARD TYPES:** the
	// old scrim was a fill at the end of `FxCard::draw`, under every child frame, so what a card drew
	// itself dimmed and no knob, value box or graph child did — OWLTT and Automod (graph + knobs
	// untouched), Drawn/Shaper/Waypoint (curve dims, knobs stay orange), the tracker's generic card
	// (knobs untouched), the DAHDSR (nothing at all). See `ui::DisabledVeil` for the road.
	//
	// **ASSERTED AS THE VALUES THE PAINT READS, ONE CARD TYPE AT A TIME.** The veil's visibility is
	// `!headEnabled()` — the same predicate that draws the head's square — its `isOnTop()` is the
	// whole mechanism (a child under the knobs is the old scrim wearing a frame), its bounds are the
	// body and never the head, and it ignores the mouse so the hand reaches the controls through it.
	// The toggle is the real press on the real square through the real tree, both ways.
	struct Subject {
		const char* tag;
		bool rack;
	};
	const Subject subjects[] = {
		{ fx::filter::kTraits.tag, false }, { fx::harm::kTraits.tag, false },
		{ fx::owltt::kTraits.tag, false },   { fx::waypoint::kTraits.tag, false },
		{ fx::drawn::kTraits.tag, false },  { fx::shaper::kTraits.tag, false },
		{ fx::automod::kTraits.tag, false }, { feathers::mod::dahdsr::kTraits.tag, true },
		{ feathers::mod::mseg::kTraits.tag, true }, { feathers::mod::tracker::kTraits.tag, true },
	};
	for (const Subject& subject : subjects) {
		const std::string who = subject.tag;
		CAPTURE(who);
		// A FIXTURE PER CARD — a lane's layout is a function of what is in it.
		ModFixture f;
		ui::FxCard* card = nullptr;
		if (subject.rack) {
			auto* panel = f.panel;
			REQUIRE(panel != nullptr);
			// THE DEFAULT RACK HOLDS A DAHDSR AT 0; the others are added below it.
			size_t slot = 0;
			if (!feathers::device::sameTag(subject.tag, feathers::mod::dahdsr::kTraits.tag)) {
				slot = f.bridge.addRackDevice(subject.tag);
				REQUIRE(slot < feathers::kMaxRackSlots);
			}
			panel->resized();
			// INTO VIEW, or the press below lands on the rail's edge rather than the square.
			const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(slot), 0);
			const visage::Bounds box = panel->sourceBoundsForTest(source);
			panel->setScroll(panel->scroll() + box.y() - ui::ModPanel::kPadTop);
			card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(panel->cardFrameForTest(slot)));
		}
		else {
			auto* lane = findInTree<ui::FxLane>(f.root);
			REQUIRE(lane != nullptr);
			// THE DEFAULT CHAIN HOLDS THE FILTER AT 0; the others are added after it.
			size_t slot = 0;
			if (!feathers::device::sameTag(subject.tag, fx::filter::kTraits.tag)) {
				slot = f.bridge.addFxDevice(subject.tag);
				REQUIRE(slot < feathers::kMaxFxSlots);
			}
			lane->resized();
			lane->scrollCardIntoView(slot);
			lane->resized();
			card = lane->cardAt(slot);
		}
		REQUIRE(card != nullptr);
		REQUIRE(card->hasEnable());
		REQUIRE(card->isVisible());

		// THE FRAME'S OWN REFRESH, headless: `update()` is the per-frame road every host drives.
		card->update();
		const ui::DisabledVeil& veil = card->disabledVeilForTest();
		const visage::Bounds body =
		    ui::cardFrameOf({ 0.0f, 0.0f, static_cast<float>(card->width()),
		                      static_cast<float>(card->height()) },
		                    card->descriptor() != nullptr ? card->descriptor()->outputCount() : 0,
		                    card->headEdge())
		        .body;
		REQUIRE(body.width() > 0.0f);
		REQUIRE(body.height() > 0.0f);

		// THE MECHANISM, AS VALUES: on top (so it paints over every sibling), mouse-transparent, and
		// exactly the body — a veil over the head would grey the way back out.
		CHECK(veil.isOnTop());
		CHECK(veil.ignoresMouseEvents());
		CHECK(veil.parent() == static_cast<const visage::Frame*>(card));
		CHECK(static_cast<float>(veil.x()) == doctest::Approx(body.x()));
		CHECK(static_cast<float>(veil.y()) == doctest::Approx(body.y()));
		CHECK(static_cast<float>(veil.width()) == doctest::Approx(body.width()));
		CHECK(static_cast<float>(veil.height()) == doctest::Approx(body.height()));
		CHECK_FALSE(visage::Bounds { static_cast<float>(veil.x()), static_cast<float>(veil.y()),
		                             static_cast<float>(veil.width()),
		                             static_cast<float>(veil.height()) }
		                .contains(visage::Point { card->enableBounds().x() + 1.0f,
			                                      card->enableBounds().y() + 1.0f }));

		// THE VALUE, BOTH WAYS, THROUGH THE REAL PRESS. The filter starts OFF and the rest start ON,
		// which is what makes "both ways" read the same for every subject.
		const bool on0 = card->headEnabled();
		CHECK(veil.isVisible() == !on0);
		const visage::Bounds sq = card->enableBounds();
		const visage::Point at = card->positionInWindow()
		                         + visage::Point { sq.x() + sq.width() * 0.5f, sq.y() + sq.height() * 0.5f };
		REQUIRE(f.root->frameAtPoint(at) == static_cast<visage::Frame*>(card));
		dragThroughRoot(f.root, at, at);
		card->update();
		REQUIRE(card->headEnabled() == !on0);
		CHECK(veil.isVisible() == on0);

		// AND WHILE VEILED, A PRESS INTO THE BODY STILL FINDS A CONTROL OR THE CARD — NEVER THE VEIL.
		if (veil.isVisible()) {
			const visage::Point inBody = card->positionInWindow()
			                             + visage::Point { body.x() + body.width() * 0.5f,
				                                           body.y() + body.height() * 0.5f };
			visage::Frame* under = f.root->frameAtPoint(inBody);
			REQUIRE(under != nullptr);
			CHECK(dynamic_cast<ui::DisabledVeil*>(under) == nullptr);

			// **HOVER AND DRAG GO THROUGH THE VEIL TO THE KNOB, MEASURED — not assumed from the flag.**
			// visage's `handleMouseMove` and `handleMouseDown` both begin with `frameAtPoint`, and
			// `Frame::frameAtPoint` walks on-top children first and returns a frame only if it does
			// not ignore mouse events (frame.cpp): so the frame under the pointer while veiled is the
			// same knob it is unveiled, and the same drag moves its value by the same amount. Asked
			// of every subject that has a knob on its body (the DAHDSR and MSEG have boxes).
			ui::Knob* knob = nullptr;
			for (visage::Frame* child : card->children()) {
				if (auto* k = dynamic_cast<ui::Knob*>(child);
				    k != nullptr && k->isVisible() && k->width() > 2.0f && k->control() != nullptr) {
					knob = k;
					break;
				}
			}
			if (knob != nullptr) {
				const visage::Point centre =
				    knob->positionInWindow()
				    + visage::Point { knob->width() * 0.5f, knob->faceDiameter() * 0.5f };
				// THE VEIL COVERS THIS POINT — the claim is about a pointer INSIDE the veil.
				REQUIRE(visage::Bounds { static_cast<float>(veil.x()), static_cast<float>(veil.y()),
				                         static_cast<float>(veil.width()),
				                         static_cast<float>(veil.height()) }
				            .contains(centre - card->positionInWindow()));
				CHECK(f.root->frameAtPoint(centre) == static_cast<visage::Frame*>(knob));
				const double before = knob->control()->value();
				// TOWARDS THE ROOM THE VALUE HAS — Harm's first knob is LEVEL at 100%, and a drag up
				// from the top of a range moves nothing on any build.
				const float dy = before > 0.5 ? 35.0f : -35.0f;
				dragThroughRoot(f.root, centre, { centre.x, centre.y + dy });
				const double veiledDelta = knob->control()->value() - before;
				CHECK(veiledDelta != 0.0);
				// AND THE SAME DRAG UNVEILED MOVES IT BY THE SAME AMOUNT, from the same start.
				dragThroughRoot(f.root, at, at);
				card->update();
				REQUIRE_FALSE(veil.isVisible());
				knob->control()->setValue(before);
				CHECK(f.root->frameAtPoint(centre) == static_cast<visage::Frame*>(knob));
				dragThroughRoot(f.root, centre, { centre.x, centre.y + dy });
				CHECK(knob->control()->value() - before == doctest::Approx(veiledDelta));
				knob->control()->setValue(before);
				dragThroughRoot(f.root, at, at);
				card->update();
				REQUIRE(veil.isVisible());
			}
		}

		dragThroughRoot(f.root, at, at);
		card->update();
		REQUIRE(card->headEnabled() == on0);
		CHECK(veil.isVisible() == !on0);
	}
}
// ---------------------------------------------------------------------------------------------------
// DD — 2026-09-07, ON THE FIRST MAC BUILD: the separate OS window popped under the DAW and stayed
// under it after setAlwaysOnTop. DECISION: back to the window inside the plugin window, but floating
// rather than at a fixed location, and without dimming the synth behind it.
// ---------------------------------------------------------------------------------------------------
TEST_CASE("DD, on the first Mac build: the expanded panel FLOATS — dragged by its head through the real tree, clamped inside the editor, remembered per device while the editor lives, and the synth behind is neither dimmed nor deaf") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	// A DEVICE WITH A NARROW WINDOW stands for "the first panel": the second (the shaper's) is 808 wide in a 1240 layer, so the
	// first has to leave room beside it, and HARM's window — four partials and an envelope — no longer does.
	const size_t harm = f.bridge.addFxDevice(feathers::mod::arp::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(harm);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	const auto insideOverlay = [&](visage::Frame* frame) {
		for (visage::Frame* at = frame; at != nullptr; at = at->parent()) {
			if (at == static_cast<visage::Frame*>(overlay)) {
				return true;
			}
		}
		return false;
	};
	const auto open = [&](size_t slot) {
		(void)f.gui.canvasForTest()->setCollapsed(true); // the floating panel is the overlay's
		lane->onExpandDevice(slot);
		REQUIRE(overlay->open());
		REQUIRE(overlay->panelForTest() != nullptr);
		REQUIRE(overlay->panelForTest()->width() > 0.0f);
	};
	// THE HEAD, IN WINDOW PIXELS: the middle of the title row, well clear of the ✕ at its right end.
	const auto head = [&] {
		ui::ExpandedPanel* panel = overlay->panelForTest();
		return panel->positionInWindow()
		       + visage::Point { panel->width() * 0.5f, ui::ExpandedOverlay::kTitleHeight * 0.5f };
	};
	const auto sameRect = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() == b.x() && a.y() == b.y() && a.width() == b.width() && a.height() == b.height();
	};

	open(harm);
	const visage::Bounds centred = overlay->panelBounds();
	// PLACED — every panel is, by the dodge, since 2026-09-14 — and the first one at the CENTRE of the
	// editor's overlay (the band starts its cards full left instead; a corner panel here would cover
	// the rails).
	CHECK(overlay->placedForTest());
	CHECK(centred.x() == doctest::Approx(std::round((overlay->width() - centred.width()) * 0.5f)));
	CHECK(centred.y() == doctest::Approx(std::round((overlay->height() - centred.height()) * 0.5f)));
	// THE SYNTH BEHIND IS NOT DEAF: a press beside the panel resolves to something that is NOT this
	// layer — the editor beneath, in its own right. (That it is not DIMMED is the render's to show:
	// the layer draws nothing; see ExpandedOverlay::draw.)
	{
		const visage::Point beside = overlay->positionInWindow() + visage::Point { 2.0f, 2.0f };
		REQUIRE(overlay->outside(beside - overlay->positionInWindow()));
		CHECK_FALSE(insideOverlay(f.root->frameAtPoint(beside)));
	}

	// A DRAG BY THE HEAD MOVES THE PANEL BY EXACTLY THE DRAG — through the real tree, the way a hand
	// would: press on the head, move, release.
	dragThroughRoot(f.root, head(), head() + visage::Point { 40.0f, 30.0f });
	const visage::Bounds moved = overlay->panelBounds();
	CHECK(moved.x() == doctest::Approx(centred.x() + 40.0f));
	CHECK(moved.y() == doctest::Approx(centred.y() + 30.0f));
	CHECK(moved.width() == centred.width());
	CHECK(overlay->placedForTest());
	// AND THE FRAME ITSELF IS THERE, not only the number: the panel's own bounds followed.
	CHECK(overlay->panelForTest()->x() == doctest::Approx(moved.x()));
	CHECK(overlay->panelForTest()->y() == doctest::Approx(moved.y()));

	// CLAMPED: dragged far past the bottom-right, the panel stops flush with the layer's edge — it
	// cannot be lost, which was the whole of the complaint about the window.
	dragThroughRoot(f.root, head(), head() + visage::Point { 5000.0f, 5000.0f });
	const visage::Bounds lowRight = overlay->panelBounds();
	CHECK(lowRight.right() == doctest::Approx(overlay->width()));
	CHECK(lowRight.bottom() == doctest::Approx(overlay->height()));
	CHECK(overlay->panelForTest()->x() == doctest::Approx(lowRight.x()));
	// ...and past the top-left, flush with the origin.
	dragThroughRoot(f.root, head(), head() - visage::Point { 5000.0f, 5000.0f });
	const visage::Bounds origin = overlay->panelBounds();
	CHECK(origin.x() == 0.0f);
	CHECK(origin.y() == 0.0f);
	CHECK(origin.width() == centred.width());

	// REMEMBERED PER DEVICE, FOR THE EDITOR'S LIFETIME: close with Escape, open the same device, and
	// it comes back where it was left. A DIFFERENT device opens centred — its own place, not harm's.
	{
		const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
		REQUIRE(overlay->onKeyPress().callback(escape));
		REQUIRE_FALSE(overlay->open());
	}
	open(harm);
	CHECK(overlay->placedForTest());
	CHECK(sameRect(overlay->panelBounds(), origin));
	const ui::ExpandedOverlay::PanelId harmPanel = overlay->frontPanelIdForTest();
	// A device that HAS an expanded view (the filter declines one, so opening it would leave harm's
	// panel where it was and the assertions below would read harm's place).
	const size_t shaper = f.bridge.addFxDevice(fx::shaper::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(shaper);
	lane->resized();
	// A SECOND DEVICE OPENS BESIDE THE FIRST (2026-09-14: several at once), DODGING IT: harm's panel sits
	// at the origin, so the shaper's is placed clear of it rather than centred over it.
	open(shaper);
	CHECK(overlay->panelCount() == 2);
	CHECK(overlay->placedForTest());
	{
		const visage::Bounds p = overlay->panelBounds();
		CHECK_FALSE(p.overlaps(overlay->panelBounds(harmPanel)));
		CHECK(p.right() <= overlay->width());
		CHECK(p.bottom() <= overlay->height());
	}
	// And back to harm: its mark CLOSES its own panel and the shaper's stays; the
	// next press reopens it where it was left.
	open(harm);
	CHECK(overlay->panelCount() == 1);
	CHECK(overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::ExpandToggle);
	open(harm);
	CHECK(overlay->panelCount() == 2);
	CHECK(sameRect(overlay->panelBounds(), origin));
	// THE ✕ STILL CLOSES — its own panel and no other.
	{
		ui::ExpandedPanel* panel = overlay->panelForTest();
		const visage::Bounds close = panel->closeBounds();
		panel->onMouseDown().callback(
		    at(close.x() + close.width() * 0.5f, close.y() + close.height() * 0.5f));
		CHECK(overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::CloseButton);
		CHECK(overlay->open());
		CHECK(overlay->panelCount() == 1);
		overlay->dismiss();
		CHECK_FALSE(overlay->open());
	}
}
TEST_CASE("PE (\"the popout editor is ... twitchy\"): a floating panel dragged by its head at 1.75x follows the hand to the device pixel on EVERY frame, wherever on the head it was grabbed") {
	// THE MEASUREMENT IS FRAME BY FRAME, because a twitch is a property of the path and not of where a
	// drag ends: the DD case above asserts the end of one long move, and every end it reads is a whole
	// logical pixel, which is exactly where a rounding term cannot show. Here the hand moves ONE DEVICE
	// PIXEL per event — what an X server delivers on the 1.75x display — and after every event the
	// panel's native place in the overlay is asserted against the place the hand has asked for.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t harm = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(harm);
	lane->resized();
	(void)f.gui.canvasForTest()->setCollapsed(true); // the floating panel is the overlay's
	lane->onExpandDevice(harm);
	REQUIRE(overlay->open());
	ui::ExpandedPanel* panel = overlay->panelForTest();
	REQUIRE(panel != nullptr);
	REQUIRE(panel->width() > 0.0f);

	constexpr float kScale = 1.75f; // the development display
	f.root->setDpiScale(kScale);
	overlay->resized();
	REQUIRE(panel->dpiScale() == kScale);

	// THREE GRABS, because a grab offset is the term that bit the drawn editor (b8829ee2): the left of the
	// head, its middle, and near its right end clear of the buttons. Each starts from the same place.
	for (const float across : { 0.2f, 0.5f, 0.6f }) {
		CAPTURE(across);
		const visage::Point grab = panel->positionInWindow()
		                           + visage::Point { panel->width() * across, ui::ExpandedOverlay::kTitleHeight * 0.5f };
		// THE POINTER LIVES ON THE DEVICE GRID, as visage's `convertToLogical` hands it over.
		const visage::IPoint pressNative { static_cast<int>(std::lround(grab.x * kScale)),
			                               static_cast<int>(std::lround(grab.y * kScale)) };
		const auto windowAt = [&](int nx, int ny) {
			return visage::Point { static_cast<float>(nx) / kScale, static_cast<float>(ny) / kScale };
		};
		visage::Frame* down = f.root->frameAtPoint(windowAt(pressNative.x, pressNative.y));
		REQUIRE(down != nullptr);
		// THE REAL ROAD'S EVENT: `position` is recomputed against the down frame EVERY move
		// (`WindowEventHandler::handleMouseMove`), and that frame is moving with the panel.
		const auto eventAt = [&](visage::Point window, bool isDown) {
			visage::MouseEvent e;
			e.window_position = window;
			e.position = window - down->positionInWindow();
			e.event_frame = down;
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
			e.is_down = isDown;
			e.repeat_click_count = 1;
			return e;
		};
		const int anchorX = panel->nativeX();
		const int anchorY = panel->nativeY();
		down->processMouseDown(eventAt(windowAt(pressNative.x, pressNative.y), true));
		// A PRESS MOVES NOTHING.
		CHECK(panel->nativeX() == anchorX);
		CHECK(panel->nativeY() == anchorY);
		int worst = 0;
		int stalls = 0;
		int leaps = 0;
		int lastX = anchorX;
		for (int k = 1; k <= 40; ++k) {
			// One device pixel right every event, and one down every other, so both axes are walked.
			const int dx = k;
			const int dy = k / 2;
			down->processMouseDrag(eventAt(windowAt(pressNative.x + dx, pressNative.y + dy), true));
			const int errX = panel->nativeX() - (anchorX + dx);
			const int errY = panel->nativeY() - (anchorY + dy);
			worst = std::max({ worst, std::abs(errX), std::abs(errY) });
			stalls += panel->nativeX() == lastX ? 1 : 0;
			leaps += panel->nativeX() - lastX > 1 ? 1 : 0;
			lastX = panel->nativeX();
		}
		down->processMouseUp(eventAt(windowAt(pressNative.x + 40, pressNative.y + 20), false));
		MESSAGE("grab at " << across << " of the head: worst lag " << worst << " device px, " << stalls
		                   << " frames the panel stood still and " << leaps << " it leapt, of 40");
		// THE HAND'S DEVICE PIXEL IS THE PANEL'S DEVICE PIXEL, every frame, on both axes...
		CHECK(worst == 0);
		// ...so a hand moving one pixel a frame moves the panel one pixel a frame: no frame stands still
		// and no frame catches up two at once, which is the stutter a whole-LOGICAL-pixel grid draws at
		// 1.75x (four steps in seven frames, two of them doubled).
		CHECK(stalls == 0);
		CHECK(leaps == 0);
		// Back where it started, for the next grab: a drag of exactly the inverse, from the head again.
		const visage::Point head = panel->positionInWindow()
		                           + visage::Point { panel->width() * 0.5f, ui::ExpandedOverlay::kTitleHeight * 0.5f };
		dragThroughRoot(f.root, head, head - visage::Point { 40.0f / kScale, 20.0f / kScale });
		REQUIRE(std::abs(panel->nativeX() - anchorX) <= 1);
		REQUIRE(std::abs(panel->nativeY() - anchorY) <= 1);
	}

	// THE GRIP IS THE SAME ROAD AT THE FAR CORNER: one device pixel a frame, and the panel's native width
	// follows by exactly that. Room is made first by dragging the panel to the top left.
	{
		const visage::Point head = panel->positionInWindow()
		                           + visage::Point { panel->width() * 0.5f, ui::ExpandedOverlay::kTitleHeight * 0.5f };
		dragThroughRoot(f.root, head, head - visage::Point { 5000.0f, 5000.0f });
		const visage::Bounds grip = panel->gripBounds();
		REQUIRE(grip.width() > 0.0f);
		const visage::Point at = panel->positionInWindow() + visage::Point { grip.x() + grip.width() * 0.5f,
			                                                                 grip.y() + grip.height() * 0.5f };
		const int px = static_cast<int>(std::lround(at.x * kScale));
		const int py = static_cast<int>(std::lround(at.y * kScale));
		const auto windowAt = [&](int nx, int ny) {
			return visage::Point { static_cast<float>(nx) / kScale, static_cast<float>(ny) / kScale };
		};
		visage::Frame* down = f.root->frameAtPoint(windowAt(px, py));
		REQUIRE(down == static_cast<visage::Frame*>(panel));
		const auto eventAt = [&](visage::Point window, bool isDown) {
			visage::MouseEvent e;
			e.window_position = window;
			e.position = window - down->positionInWindow();
			e.event_frame = down;
			e.button_id = visage::kMouseButtonLeft;
			e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
			e.is_down = isDown;
			e.repeat_click_count = 1;
			return e;
		};
		const int startWidth = panel->nativeWidth();
		down->processMouseDown(eventAt(windowAt(px, py), true));
		int worst = 0;
		for (int k = 1; k <= 20; ++k) {
			down->processMouseDrag(eventAt(windowAt(px + k, py), true));
			worst = std::max(worst, std::abs(panel->nativeWidth() - (startWidth + k)));
		}
		down->processMouseUp(eventAt(windowAt(px + 20, py), false));
		MESSAGE("grip: worst lag " << worst << " device px of 20 one-pixel frames");
		CHECK(worst == 0);
	}
	f.root->setDpiScale(1.0f);
}
TEST_CASE("two instances of ONE kind open as TWO panels, each wearing its own tint — the factory stamps the device, the panel does not have to remember to") {
	// The canvas windows showed no tint, and a second instance would not open into the canvas until
	// the first was closed. One cause: no real panel answered `watchedDevice()`, so the
	// overlay keyed every instance of a kind alike (title#0) — the second REPLACED the first in the
	// first's place under the same title, and wore the no-device tint. The DD case above never met it
	// because it opens two different KINDS. This opens two of the same.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	// TWO OF A DEVICE WITH A NARROW WINDOW, so the two panels fit either side of the middle of the layer, where the first one
	// opens (two HARM windows, at 660 wide, do not: a centred first leaves 290 a side).
	const size_t first = f.bridge.addFxDevice(fx::automod::kTraits.tag);
	const size_t second = f.bridge.addFxDevice(fx::automod::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true);
	const DeviceInstanceId firstDevice = f.bridge.fxDeviceAt(first);
	const DeviceInstanceId secondDevice = f.bridge.fxDeviceAt(second);
	REQUIRE(firstDevice != secondDevice);

	lane->scrollCardIntoView(first);
	lane->resized();
	lane->onExpandDevice(first);
	REQUIRE(overlay->panelCount() == 1);
	const ui::ExpandedOverlay::PanelId one = overlay->frontPanelIdForTest();
	// THE PANEL KNOWS ITS INSTANCE — the value, not a rendering of it.
	CHECK(overlay->panelForTest(one)->tintForTest() == ui::instanceTintFor(firstDevice));
	CHECK(overlay->panelForTest(one)->tintForTest() != 0);

	lane->scrollCardIntoView(second);
	lane->resized();
	lane->onExpandDevice(second);
	// TWO PANELS, NOT ONE REPLACED: the first is still there, and the second is beside it.
	CHECK(overlay->panelCount() == 2);
	const ui::ExpandedOverlay::PanelId two = overlay->frontPanelIdForTest();
	REQUIRE(two != one);
	REQUIRE(overlay->panelForTest(one) != nullptr);
	CHECK(overlay->panelForTest(two)->tintForTest() == ui::instanceTintFor(secondDevice));
	CHECK(overlay->panelForTest(two)->tintForTest() != overlay->panelForTest(one)->tintForTest());
	CHECK_FALSE(overlay->panelBounds(two).overlaps(overlay->panelBounds(one)));
	// AND THE FIRST'S MARK AGAIN CLOSES ITS OWN PANEL, not the second's.
	lane->onExpandDevice(first);
	CHECK(overlay->panelCount() == 1);
	CHECK(overlay->panelForTest(one) == nullptr);
	CHECK(overlay->panelForTest(two) != nullptr);
	overlay->dismiss();
}
TEST_CASE("arming a source lights the knobs on an OPEN expanded panel — floating over the editor and on the canvas alike") {
	// The modulation glow stopped reaching the expanded panel's controls, in the floating window and
	// on the canvas alike. The halo reads `Knob::haloLit()`; every knob on a panel must be on the
	// modulation list (so the arm repaints it) and must answer lit once a source is armed.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	REQUIRE(canvas != nullptr);
	REQUIRE(mod != nullptr);
	const size_t harm = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(harm);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);

	const auto litKnobsOf = [&](visage::Frame* panel, size_t* total) {
		std::vector<ui::Knob*> knobs;
		collectKnobs(panel, &knobs);
		*total = knobs.size();
		size_t lit = 0;
		for (ui::Knob* knob : knobs) {
			CHECK(mod->watches(knob));
			if (knob->haloLit()) {
				++lit;
			}
		}
		return lit;
	};

	SUBCASE("floating over the editor") {
		(void)canvas->setCollapsed(true);
		lane->onExpandDevice(harm);
		REQUIRE(overlay->open());
		size_t total = 0;
		CHECK(litKnobsOf(overlay->panelForTest(), &total) == 0); // not armed: nothing lit
		REQUIRE(total > 0);
		f.arm(macroSourceAt(0));
		REQUIRE(mod->armed());
		const size_t lit = litKnobsOf(overlay->panelForTest(), &total);
		CHECK(lit > 0);
		CHECK(lit == total);
	}
	SUBCASE("on the canvas") {
		REQUIRE(canvas->setCollapsed(false)); f.grantPreferredSize(); // the host grants the room (backlog EJ)
		lane->onExpandDevice(harm);
		REQUIRE(overlay->open());
		size_t total = 0;
		f.arm(macroSourceAt(0));
		REQUIRE(mod->armed());
		const size_t lit = litKnobsOf(overlay->panelForTest(), &total);
		REQUIRE(total > 0);
		CHECK(lit > 0);
		CHECK(lit == total);
	}
	SUBCASE("every kind of panel this host can build, floating") {
		// NOT ONLY HARM: the panels are seven different classes and each hands the state to its own
		// controls by its own `setModulation`, so one kind green says nothing about the rest.
		(void)canvas->setCollapsed(true);
		f.arm(macroSourceAt(0));
		REQUIRE(mod->armed());
		size_t kinds = 0;
		for (const device::Descriptor& d : kDevices) {
			if (!ui::hasExpandedView(d) || d.traits.tag == std::string(feathers::composite::kTraits.tag)) {
				continue;
			}
			const size_t at = f.bridge.addFxDevice(d.traits.tag);
			if (at >= feathers::kMaxFxSlots) {
				continue;
			}
			f.bridge.refresh();
			lane->resized();
			lane->scrollCardIntoView(at);
			lane->resized();
			lane->onExpandDevice(at);
			REQUIRE(overlay->open());
			// EVERY DESTINATION KNOB LIT AND WATCHED; a knob that is no destination never glows (the
			// honesty rule) and is not counted against the panel.
			std::vector<ui::Knob*> knobs;
			collectKnobs(overlay->panelForTest(), &knobs);
			const std::string kind = d.traits.name;
			CAPTURE(kind);
			size_t destinations = 0;
			size_t lit = 0;
			size_t unwatched = 0;
			for (ui::Knob* knob : knobs) {
				if (!knob->isModDestination()) {
					continue;
				}
				++destinations;
				lit += knob->haloLit() ? 1 : 0;
				unwatched += mod->watches(knob) ? 0 : 1;
			}
			// A WINDOW WITH NO KNOBS (OWLTT's is boxes, faders and plot handles) is judged on the assignable-control
			// interface instead: every ControlFace frame that DRAWS modulation for a control it offers must glow for the armed
			// source and be watched. The knob walk above is the same question asked of the one widget class.
			size_t faces = 0;
			if (knobs.empty()) {
				std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
					if (const auto* face = dynamic_cast<const ui::ControlFace*>(frame)) {
						std::vector<ui::ControlFace::Offered> offered;
						face->offeredControls(offered);
						for (const auto& o : offered) {
							const ui::ControlFace::ModLook look = face->modLookFor(*o.control);
							if (!look.drawn || !frame->isVisible()) {
								continue;
							}
							++faces;
							++destinations;
							lit += look.armedGlow ? 1 : 0;
							unwatched += mod->watches(frame) ? 0 : 1;
						}
					}
					for (visage::Frame* child : frame->children()) {
						walk(child);
					}
				};
				walk(overlay->panelForTest());
				CAPTURE(faces);
			}
			CHECK((!knobs.empty() || faces > 0));
			CHECK(destinations > 0);
			CHECK(lit == destinations);
			CHECK(unwatched == 0);
			++kinds;
			overlay->dismiss();
		}
		CHECK(kinds >= 5);
	}
	SUBCASE("a DEVICE source arms them too, and a route made on a panel knob draws its ring — floating and on the canvas") {
		// A macro is not the only source a hand arms: a modulator in the strip publishes an output, and
		// `mayRoute` answers differently for a device source than for a core one. And the glow is not
		// the only thing modulation shows — once a route exists the knob draws its RING, read from the
		// host through the same `DeviceControl` as the halo.
		size_t modulatorSlot = feathers::kMaxRackSlots;
		for (const device::Descriptor& d : kDevices) {
			if (feathers::maySitInModRack(d) && d.outputCount() > 0) {
				modulatorSlot = f.bridge.addRackDevice(d.traits.tag);
				break;
			}
		}
		REQUIRE(modulatorSlot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		const backplane::Source deviceSource = backplane::Source::output(f.bridge.rackDeviceAt(modulatorSlot), 0);

		const auto checkHost = [&](bool onCanvas) {
			(void)canvas->setCollapsed(!onCanvas);
			lane->onExpandDevice(harm);
			ui::ExpandedPanel* panel = overlay->panelForTest(); // one overlay, band open or folded
			REQUIRE(panel != nullptr);
			std::vector<ui::Knob*> knobs;
			collectKnobs(panel, &knobs);
			ui::Knob* target = nullptr;
			for (ui::Knob* knob : knobs) {
				if (knob->isModDestination() && dynamic_cast<ui::DeviceControl*>(knob->control()) != nullptr) {
					target = knob;
					break;
				}
			}
			REQUIRE(target != nullptr);
			(void)mod->arm(true);
			(void)mod->focus(deviceSource);
			REQUIRE(mod->armed());
			CHECK(target->haloLit());
			// THE ROUTE, MADE BY THE HAND ON THE PANEL: a drag on an armed knob edits the amount.
			const auto* control = dynamic_cast<ui::DeviceControl*>(target->control());
			const backplane::Destination dest = control->destination();
			CHECK(f.bridge.routeAmount(deviceSource, dest) == 0.0f);
			const visage::Point centre = f.faceCentre(target);
			dragThroughRoot(f.root, centre, centre - visage::Point { 0.0f, 60.0f });
			CHECK(f.bridge.routeAmount(deviceSource, dest) != 0.0f);
			// AND THE RING DRAWS IT.
			const ui::Knob::ModRing ring = target->modRingForTest();
			CHECK(ring.drawn);
			f.bridge.setRouteAmount(deviceSource, dest, 0.0f);
			overlay->dismiss();
		};
		checkHost(false);
		checkHost(true);
	}
}
TEST_CASE("an open panel's knobs FOLLOW live modulation — the base walk runs for every factory-built panel and asks for a frame exactly when a watched control moved") {
	// The modulation glow did not reach the expanded panel's controls. The base `ExpandedView::update`
	// walks the panel's device's modulatable controls and marks the panel dirty when a modulated
	// reading moved — but only for a panel that names its host, descriptor and device, and NO real
	// panel ever did (git -S finds no override). So a panel's rings moved only while something else
	// happened to repaint the window. The factory stamps all three now.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t slot = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	(void)f.gui.canvasForTest()->setCollapsed(true);
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	auto* mounted = const_cast<ui::ExpandedView*>(overlay->contentForTest());
	REQUIRE(mounted != nullptr);
	const auto* inner = dynamic_cast<const ui::MountedFrame*>(mounted);
	REQUIRE(inner != nullptr);
	auto* view = dynamic_cast<ui::ExpandedView*>(inner->contentForTest());
	REQUIRE(view != nullptr);
	// A KNOB'S CONTROL: the first partial's Fine. (Level is a fader pill in the strip now, and the enable is the header chip.)
	const size_t control = static_cast<size_t>(fx::harm::Param::Fine);
	REQUIRE(fx::harm::kDescriptor.params[control].modulatable);

	// SETTLED: nothing moved, nothing asked for.
	(void)mounted->update();
	(void)mounted->update();
	CHECK_FALSE(mounted->update());

	// A MODULATED READING MOVES (the audio side publishes a different value on the modulated channel
	// while the base stays), and the panel asks for a frame — ONCE.
	feathers::FxChain movedFx = f.bridge.fxChain();
	const float base = movedFx.device[slot].row.field[control];
	movedFx.device[slot].row.field[control] = base > 0.5f ? base - 0.3f : base + 0.3f;
	f.bridge.publishModulated(f.bridge.modRack(), movedFx, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
	CHECK(f.deviceHost.modulatedFieldAt(f.bridge.fxDeviceAt(slot), control) != doctest::Approx(base));
	CHECK(mounted->update());
	CHECK_FALSE(mounted->update());
	// AND THE PANEL'S OWN KNOB READS THE MOVED VALUE.
	std::vector<ui::Knob*> knobs;
	collectKnobs(overlay->panelForTest(), &knobs);
	bool found = false;
	for (ui::Knob* knob : knobs) {
		const auto* dc = dynamic_cast<const ui::DeviceControl*>(knob->control());
		if (dc == nullptr || dc->control() != control) {
			continue;
		}
		found = true;
		const device::ModDisplay shown = dc->display();
		CHECK(shown.modulated == doctest::Approx(movedFx.device[slot].row.field[control]));
		CHECK(shown.base == doctest::Approx(base));
	}
	CHECK(found);
	// THE THREE FACTS, STAMPED — asserted last, so a factory that stamps fewer fails on the walk
	// above, which is the claim, before it fails here.
	CHECK(view->watchedDevice() == f.bridge.fxDeviceAt(slot));
	REQUIRE(view->watchedDescriptor() != nullptr);
	CHECK(std::string(view->watchedDescriptor()->traits.tag) == fx::harm::kTraits.tag);
	CHECK(view->watchedHost() != nullptr);
	overlay->dismiss();
}
// ---------------------------------------------------------------------------------------------------
// DG — once moved, OWLTT stopped updating its analyzer bars, and some modulation appeared
// corrupted. The card widgets hold a LANE and ask the
// bridge host for readings by it; the host converted it to a position and then asked the reading
// COUNT with that position, which the count accessor converted AGAIN as if it were a lane.
// ---------------------------------------------------------------------------------------------------
TEST_CASE("the expanded panel RESIZES by its corner grip — through the real tree, never below its content's minimum, never past the editor, remembered per device") {
	// The floating window is resizable. The panel floated (DD above)
	// at exactly its content's minimum; a chain's editor at 720×560 wants more when the editor has it. A
	// grip in the panel's bottom-right corner, in the pad where the content does not reach, so a press
	// there is the panel's and not a card's. Asserted on the PANEL'S BOUNDS and the content's, by numbers.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t harm = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(harm);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	const auto open = [&] {
		(void)f.gui.canvasForTest()->setCollapsed(true); // the grip is the overlay panel's
		lane->onExpandDevice(harm);
		REQUIRE(overlay->open());
		REQUIRE(overlay->panelForTest() != nullptr);
	};
	const auto grip = [&] {
		ui::ExpandedPanel* panel = overlay->panelForTest();
		const visage::Bounds g = panel->gripBounds();
		return panel->positionInWindow() + visage::Point { g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f };
	};
	open();
	const visage::Bounds initial = overlay->panelBounds();
	const visage::Point minimum = overlay->contentForTest()->minimumSize();
	CHECK_FALSE(overlay->sizedForTest());
	// THE PRESS LANDS ON THE PANEL, not on its content — the grip is in the pad.
	REQUIRE(f.root->frameAtPoint(grip()) == static_cast<visage::Frame*>(overlay->panelForTest()));

	SUBCASE("a drag from the grip grows the panel by the drag, and the content grows with it") {
		dragThroughRoot(f.root, grip(), grip() + visage::Point { 120.0f, 80.0f });
		const visage::Bounds bigger = overlay->panelBounds();
		// THE DRAG, OR THE EDITOR'S EDGE, WHICHEVER COMES FIRST — the fixture's editor is not tall. AND,
		// SINCE BACKLOG EI, HARM'S CEILING: its scope takes width without limit, and its rows are fixed
		// heights, so its height stops a pad past its natural size.
		const visage::Point ceiling = overlay->contentForTest()->maximumSize();
		REQUIRE(ceiling.x == ui::ExpandedView::kNoCap);
		REQUIRE(ceiling.y < ui::ExpandedView::kNoCap);
		CHECK(bigger.width() == doctest::Approx(std::min(initial.width() + 120.0f, overlay->width() - initial.x())));
		CHECK(bigger.height()
		      == doctest::Approx(std::min({ initial.height() + 80.0f, overlay->height() - initial.y(),
		                                    initial.height() + (ceiling.y - minimum.y) })));
		CHECK(bigger.x() == doctest::Approx(initial.x())); // the near corner stayed
		CHECK(bigger.y() == doctest::Approx(initial.y()));
		CHECK(overlay->sizedForTest());
		CHECK(overlay->contentForTest()->width() == doctest::Approx(bigger.width() - 2.0f * ui::ExpandedOverlay::kPanelPad));
		// ONE PAD BELOW THE CONTENT (`contentBounds`), two in the panel's wanted height: the content is a pad
		// taller than its minimum at the floor, and that is the panel's geometry as it was, not this change's.
		CHECK(overlay->contentForTest()->height()
		      == doctest::Approx(bigger.height() - ui::ExpandedOverlay::kTitleHeight - ui::ExpandedOverlay::kPanelPad));
		// AND THE PANEL IS PINNED WHERE IT WAS, not re-centred at its new size: a resize is a gesture on the far
		// corner, so the near one becomes the placement.
		CHECK(overlay->placedForTest());
	}
	SUBCASE("it cannot shrink below the content's minimum") {
		dragThroughRoot(f.root, grip(), grip() + visage::Point { -1000.0f, -1000.0f });
		const visage::Bounds floor = overlay->panelBounds();
		CHECK(overlay->contentForTest()->width() == doctest::Approx(minimum.x));
		CHECK(overlay->contentForTest()->height() == doctest::Approx(minimum.y + ui::ExpandedOverlay::kPanelPad));
		CHECK(floor.width() == doctest::Approx(initial.width()));
	}
	SUBCASE("it cannot grow past the editor's edge") {
		dragThroughRoot(f.root, grip(), grip() + visage::Point { 5000.0f, 5000.0f });
		const visage::Bounds ceiling = overlay->panelBounds();
		CHECK(ceiling.right() <= overlay->width() + 0.5f);
		CHECK(ceiling.bottom() <= overlay->height() + 0.5f);
		CHECK(ceiling.width() >= initial.width());
	}
	SUBCASE("the size is remembered for the device across a close and reopen, as the placement is") {
		dragThroughRoot(f.root, grip(), grip() + visage::Point { 60.0f, 40.0f });
		const visage::Bounds chosen = overlay->panelBounds();
		overlay->dismiss();
		REQUIRE_FALSE(overlay->open());
		open();
		CHECK(overlay->sizedForTest());
		CHECK(overlay->panelBounds().width() == doctest::Approx(chosen.width()));
		CHECK(overlay->panelBounds().height() == doctest::Approx(chosen.height()));
	}
}

namespace {
// ---------------------------------------------------------------------------------------------------
// EI — some device windows could be enlarged past what made sense for their design, so window
// size is capped. A panel states a CEILING per axis the way it
// states a floor, and the overlay's grip stops at it. Asserted on the frames' bounds, logical and
// native, never on a picture.
// ---------------------------------------------------------------------------------------------------

/// ONE DEVICE'S PANEL, OPEN ON THE EDITOR'S FLOATING OVERLAY THROUGH THE LANE'S EXPAND ROAD, and pushed
/// into the top-left corner so the editor's edge is as far from the grip as it can be.
struct EiPanel {
	ui::ExpandedOverlay* overlay = nullptr;
	const ui::MountedFrame* mounted = nullptr;
	visage::Frame* inner = nullptr;
	size_t slot = 0;
};
EiPanel openPanelForEi(ModFixture& f, const char* tag) {
	EiPanel p;
	auto* lane = findInTree<ui::FxLane>(f.root);
	p.overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(p.overlay != nullptr);
	p.slot = f.bridge.addFxDevice(tag);
	REQUIRE(p.slot < feathers::kMaxFxSlots);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(p.slot);
	lane->resized();
	(void)f.gui.canvasForTest()->setCollapsed(true); // the floating panel, whose size the overlay sets
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(p.slot);
	REQUIRE(p.overlay->open());
	p.overlay->beginPanelDrag();
	p.overlay->dragPanelBy({ -5000.0f, -5000.0f });
	// THROUGH THE MOUNT, which is what the overlay holds and what answers it — so its forward of the
	// ceiling is under test with everything else.
	p.mounted = dynamic_cast<const ui::MountedFrame*>(p.overlay->contentForTest());
	REQUIRE(p.mounted != nullptr);
	p.inner = p.mounted->contentForTest();
	REQUIRE(p.inner != nullptr);
	return p;
}
/// The content size plus the overlay's chrome — what the panel frame is for a content of `s`.
visage::Point eiChrome(visage::Point s) {
	return { s.x + 2.0f * ui::ExpandedOverlay::kPanelPad,
		     s.y + ui::ExpandedOverlay::kTitleHeight + 2.0f * ui::ExpandedOverlay::kPanelPad };
}
visage::Point eiGrip(ui::ExpandedOverlay* overlay) {
	ui::ExpandedPanel* panel = overlay->panelForTest();
	const visage::Bounds g = panel->gripBounds();
	return panel->positionInWindow() + visage::Point { g.x() + g.width() * 0.5f, g.y() + g.height() * 0.5f };
}
} // namespace

TEST_CASE("backlog EI: every panel's CEILING per axis — capped where the content is fixed cells and open where it stretches something into the room") {
	// THE DECISION, PER PANEL, PINNED. Each row was decided by reading what the panel's `resized()` does
	// with slack (see each `maximumSize()`): a plane, a scope or a graph that takes the width leaves that
	// axis open; an axis that is all fixed cells is capped. A knob row spreading its cells apart does not
	// count as using the room.
	struct Row {
		const char* tag;
		bool capX;
		bool capY;
	};
	const Row rows[] = {
		// THE GRID IS FIXED CELLS, BUT THE CONV DRAWER UNDER IT DRAWS THE IMPULSE ACROSS THE WIDTH (backlog JD), so
		// the width is open and the height follows the grid plus the drawer.
		{ fx::featherverb::kTraits.tag, false, true },
		{ fx::shaper::kTraits.tag, true, true },      // pad, plot and extras fixed
		{ fx::automod::kTraits.tag, true, true },     // the pad is a fixed square
		{ fx::harm::kTraits.tag, false, true },       // the harmonic row and the scope take the width
		{ fx::owltt::kTraits.tag, false, true },      // the graph takes the width
		{ fx::drawn::kTraits.tag, false, true },      // the plane's frequency axis takes the width
		{ fx::waypoint::kTraits.tag, false, true },   // the plane takes the width
		{ fx::sklitter::kTraits.tag, false, false },  // the open circle takes both
	};
	for (const Row& row : rows) {
		CAPTURE(row.tag);
		ModFixture f;
		const EiPanel p = openPanelForEi(f, row.tag);
		const visage::Point minimum = p.mounted->minimumSize();
		const visage::Point ceiling = p.mounted->maximumSize();
		CHECK((ceiling.x < ui::ExpandedView::kNoCap) == row.capX);
		CHECK((ceiling.y < ui::ExpandedView::kNoCap) == row.capY);
		// THE MOUNT FORWARDS; it does not decide.
		const auto* inner = dynamic_cast<const ui::ExpandedView*>(p.inner);
		REQUIRE(inner != nullptr);
		CHECK(inner->maximumSize().x == ceiling.x);
		CHECK(inner->maximumSize().y == ceiling.y);
		// A SMALL MARGIN, from the panel's own padding: above the natural size and not far above it.
		if (row.capX) {
			CHECK(ceiling.x > minimum.x);
			CHECK(ceiling.x - minimum.x <= 16.0f);
		}
		if (row.capY) {
			CHECK(ceiling.y > minimum.y);
			CHECK(ceiling.y - minimum.y <= 16.0f);
		}
	}
}
TEST_CASE("backlog EI: a fixed-grid panel's grip STOPS AT ITS CEILING — smoothly up to it and not a pixel past, in logical and native pixels at two scales") {
	// FEATHERVERB LEFT THIS LOOP WITH ITS CONV DRAWER (backlog JD): its width is open, so the grip has no
	// ceiling to stop at on that axis. The Shaper is the fixed-grid panel here.
	for (const char* tag : { fx::shaper::kTraits.tag }) {
		for (const float factor : { 1.0f, 1.75f }) {
			CAPTURE(tag);
			CAPTURE(factor);
			ModFixture f;
			// THE EDITOR'S SCALE — the display's times the user's (EJ) — is the root's dpi scale.
			const float scale = f.root->dpiScale() * factor;
			f.root->setDpiScale(scale);
			const EiPanel p = openPanelForEi(f, tag);
			ui::ExpandedOverlay* overlay = p.overlay;
			ui::ExpandedPanel* panel = overlay->panelForTest();
			REQUIRE(panel != nullptr);
			CHECK(panel->dpiScale() == doctest::Approx(scale));
			const visage::Point minimum = p.mounted->minimumSize();
			const visage::Point ceiling = p.mounted->maximumSize();
			REQUIRE(ceiling.x < ui::ExpandedView::kNoCap);
			REQUIRE(ceiling.y < ui::ExpandedView::kNoCap);
			// NOT VACUOUS: from where the panel sits the editor has room well past the ceiling on both
			// axes, so whatever stops the grip below is the ceiling and not the edge.
			const visage::Bounds at = overlay->panelBounds();
			REQUIRE(overlay->width() - at.x() - eiChrome(ceiling).x > 40.0f);
			REQUIRE(overlay->height() - at.y() - eiChrome(ceiling).y > 40.0f);

			// SMOOTHLY: a gesture that stays under the ceiling follows the hand exactly, pixel for pixel.
			const visage::Point part { std::floor((ceiling.x - minimum.x) * 0.5f), std::floor((ceiling.y - minimum.y) * 0.5f) };
			REQUIRE(part.x >= 1.0f);
			REQUIRE(part.y >= 1.0f);
			// "EXACTLY" IS TO HALF A DEVICE PIXEL: the far corner lands on the DEVICE grid (backlog PE, so a
			// grip moved one device pixel a frame moves the corner one device pixel a frame), and a whole
			// logical `part` is not on that grid at 1.75x. A hand never moves by one anyway.
			overlay->beginPanelResize();
			overlay->resizePanelBy(part);
			CHECK(std::abs(panel->width() - (eiChrome(minimum).x + part.x)) <= 0.5f / scale);
			CHECK(std::abs(panel->height() - (eiChrome(minimum).y + part.y)) <= 0.5f / scale);
			// THE SAME GESTURE CARRIED ON PAST IT: the panel stops at the ceiling and stays there.
			overlay->resizePanelBy(part + visage::Point { 900.0f, 900.0f });
			CHECK(static_cast<float>(panel->width()) == doctest::Approx(eiChrome(ceiling).x));
			CHECK(static_cast<float>(panel->height()) == doctest::Approx(eiChrome(ceiling).y));
			// AND BACK UNDER IT IN THE SAME GESTURE follows the hand again — no latch at the ceiling.
			overlay->resizePanelBy(part);
			CHECK(std::abs(panel->width() - (eiChrome(minimum).x + part.x)) <= 0.5f / scale);

			// THROUGH THE REAL GRIP, FAR PAST IT: the frames themselves, logical and native.
			dragThroughRoot(f.root, eiGrip(overlay), eiGrip(overlay) + visage::Point { 3000.0f, 3000.0f });
			CHECK(static_cast<float>(panel->width()) == doctest::Approx(eiChrome(ceiling).x));
			CHECK(static_cast<float>(panel->height()) == doctest::Approx(eiChrome(ceiling).y));
			CHECK(static_cast<float>(p.inner->width()) == doctest::Approx(ceiling.x));
			CHECK(std::abs(static_cast<float>(panel->nativeWidth()) - scale * eiChrome(ceiling).x) <= 1.0f);
			CHECK(std::abs(static_cast<float>(panel->nativeHeight()) - scale * eiChrome(ceiling).y) <= 1.0f);
			CHECK(std::abs(static_cast<float>(p.inner->nativeWidth()) - scale * ceiling.x) <= 1.0f);
			// A SECOND DRAG FROM THE NEW GRIP MOVES NOTHING.
			dragThroughRoot(f.root, eiGrip(overlay), eiGrip(overlay) + visage::Point { 200.0f, 200.0f });
			CHECK(static_cast<float>(panel->width()) == doctest::Approx(eiChrome(ceiling).x));
			CHECK(static_cast<float>(panel->height()) == doctest::Approx(eiChrome(ceiling).y));

			// A SCALE CHANGE WITH THE PANEL AT ITS CEILING moves nothing logical, and the layout pass that
			// follows one converts the native rectangle (CLAUDE.md, backlog EC).
			const float next = scale * 1.25f;
			f.root->setDpiScale(next);
			overlay->resized();
			CHECK(static_cast<float>(panel->width()) == doctest::Approx(eiChrome(ceiling).x));
			CHECK(std::abs(static_cast<float>(panel->nativeWidth()) - next * eiChrome(ceiling).x) <= 1.0f);
			CHECK(std::abs(static_cast<float>(p.inner->nativeWidth()) - next * ceiling.x) <= 1.0f);
		}
	}
}
TEST_CASE("backlog EI: a panel that USES the room is NOT capped — SKLITTER's open circle grows with the grip on both axes") {
	ModFixture f;
	const EiPanel p = openPanelForEi(f, fx::sklitter::kTraits.tag);
	auto* content = dynamic_cast<ui::SklitterExpanded*>(p.inner);
	REQUIRE(content != nullptr);
	REQUIRE_FALSE(content->circleRailForTest().collapsed());
	ui::ExpandedPanel* panel = p.overlay->panelForTest();
	const visage::Point minimum = p.mounted->minimumSize();
	// PAST WHERE ANY FIXED-GRID CEILING WOULD SIT (a pad or so), and inside the editor's room.
	const visage::Point drag { 150.0f, 40.0f };
	const visage::Bounds at = p.overlay->panelBounds();
	REQUIRE(p.overlay->width() - at.x() - eiChrome(minimum).x > drag.x);
	REQUIRE(p.overlay->height() - at.y() - eiChrome(minimum).y > drag.y);
	dragThroughRoot(f.root, eiGrip(p.overlay), eiGrip(p.overlay) + drag);
	CHECK(static_cast<float>(panel->width()) == doctest::Approx(eiChrome(minimum).x + drag.x));
	CHECK(static_cast<float>(panel->height()) == doctest::Approx(eiChrome(minimum).y + drag.y));
	CHECK(content->surplusWidth() == doctest::Approx(drag.x)); // and the circle is what took it
}
TEST_CASE("backlog EI: the CEILING FOLLOWS THE CONTENT — a window above a ceiling that dropped shrinks to it") {
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	const EiPanel p = openPanelForEi(f, sc::kTraits.tag);
	auto* content = dynamic_cast<ui::SklitterExpanded*>(p.inner);
	REQUIRE(content != nullptr);
	ui::ExpandedOverlay* overlay = p.overlay;
	ui::ExpandedPanel* panel = overlay->panelForTest();
	const DeviceInstanceId id = f.bridge.fxDeviceAt(p.slot);

	SUBCASE("SKLITTER's circle closing caps the width it was using, and opening again gives the hand's width back") {
		REQUIRE_FALSE(content->circleRailForTest().collapsed());
		overlay->beginPanelResize();
		overlay->resizePanelBy({ 150.0f, 0.0f });
		const float wide = static_cast<float>(panel->width());
		REQUIRE(wide == doctest::Approx(eiChrome(p.mounted->minimumSize()).x + 150.0f));

		(void)content->circleRailForTest().setCollapsed(true);
		const visage::Point ceiling = p.mounted->maximumSize();
		REQUIRE(ceiling.x < ui::ExpandedView::kNoCap);
		REQUIRE(eiChrome(ceiling).x < wide); // NOT VACUOUS: the new ceiling is below the window's width
		CHECK(static_cast<float>(panel->width()) == doctest::Approx(eiChrome(ceiling).x));
		CHECK(static_cast<float>(content->width()) == doctest::Approx(ceiling.x));

		(void)content->circleRailForTest().setCollapsed(false);
		CHECK(static_cast<float>(panel->width()) == doctest::Approx(wide));
	}
	SUBCASE("a Mode change moves the ceiling with the grid, and a window above the new one shrinks to it") {
		// A TALLER EDITOR THAN THE DESIGN'S (backlog MJ): Record Gate gave the grid's fullest mode another row, and
		// its ceiling outgrew the design height's overlay — which this case needs room above, or it cannot show a
		// window shrinking to a ceiling.
		{
			uint32_t w = 1;
			uint32_t h = 1;
			REQUIRE(f.gui.adjustSize(&w, &h));
			REQUIRE(f.gui.setSize(w, h + h / 4));
		}
		// THE CIRCLE CLOSED, so the panel is the grid and three strips and both axes are capped.
		(void)content->circleRailForTest().setCollapsed(true);
		REQUIRE(p.mounted->maximumSize().y < ui::ExpandedView::kNoCap);
		const auto setMode = [&](size_t m) {
			f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(m));
			(void)f.gui.tickForTest(); // the editor's own tick notices, as in backlog HY's case
		};
		// THE SPARSEST AND FULLEST MODES BY CEILING, read off the panel rather than restated.
		size_t sparsest = 0;
		size_t fullest = 0;
		std::array<float, static_cast<size_t>(sc::Mode::kCount)> capY {};
		for (size_t m = 0; m < capY.size(); ++m) {
			setMode(m);
			capY[m] = p.mounted->maximumSize().y;
			sparsest = capY[m] < capY[sparsest] ? m : sparsest;
			fullest = capY[m] > capY[fullest] ? m : fullest;
		}
		REQUIRE(capY[fullest] > capY[sparsest]);
		MESSAGE("ceiling height sparsest=" << capY[sparsest] << " fullest=" << capY[fullest]
		        << " overlay height=" << overlay->height());
		REQUIRE(overlay->height() - overlay->panelBounds().y() - eiChrome({ 0.0f, capY[fullest] }).y > 0.0f);

		setMode(sparsest);
		overlay->beginPanelResize();
		overlay->resizePanelBy({ 3000.0f, 3000.0f });
		const visage::Point sparseCeiling = p.mounted->maximumSize();
		CHECK(static_cast<float>(panel->height()) == doctest::Approx(eiChrome(sparseCeiling).y));

		// FULLER: the ceiling rises with the grid; the window is held at the new floor, not left short.
		setMode(fullest);
		CHECK(p.mounted->maximumSize().y == doctest::Approx(capY[fullest]));
		CHECK(static_cast<float>(panel->height())
		      == doctest::Approx(eiChrome({ 0.0f, std::max(sparseCeiling.y, p.mounted->minimumSize().y) }).y));
		overlay->beginPanelResize();
		overlay->resizePanelBy({ 3000.0f, 3000.0f });
		CHECK(static_cast<float>(panel->height()) == doctest::Approx(eiChrome({ 0.0f, capY[fullest] }).y));

		// SPARSER AGAIN: the window is above the ceiling that just dropped, and shrinks to it — the FRAME,
		// read after the editor's own tick, not a recomputation.
		setMode(sparsest);
		CHECK(static_cast<float>(panel->height()) == doctest::Approx(eiChrome(sparseCeiling).y));
		CHECK(static_cast<float>(panel->width()) == doctest::Approx(eiChrome(sparseCeiling).x));
	}
}
TEST_CASE("DG: after a reorder, a device's card reads ITS OWN readings through the bridge host — OWLTT's meters and automod's phase, by the numbers published at each device's position") {
	ModFixture f;
	ParamBridge& bridge = f.bridge;
	feathers::BridgeDeviceHost& host = f.deviceHost;
	const size_t base = bridge.fxChain().count; // the default filter, position 0
	REQUIRE(bridge.addFxDevice(fx::owltt::kTraits.tag) == base);
	REQUIRE(bridge.addFxDevice(fx::automod::kTraits.tag) == base + 1);
	f.bridge.refresh();
	const DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	const DeviceInstanceId owlttId = bridge.fxDeviceAt(base);
	const DeviceInstanceId automodId = bridge.fxDeviceAt(base + 1);

	// THE RESTATEMENT: distinct numbers per position, published the way the audio thread publishes
	// them, so what a device's card must read is the set published at THAT device's position and
	// no other.
	const auto publishAll = [&] {
		for (size_t position = 0; position < bridge.fxChain().count; ++position) {
			const float p = static_cast<float>(position);
			bridge.publishOwlttMeters(position, { -1.0f - p, -2.0f - p, -3.0f - p },
			                         { -10.0f - p, -20.0f - p, -30.0f - p },
			                         { -4.0f - p, -5.0f - p, -6.0f - p }, position % 2 == 1);
			bridge.publishAutomodReadings(position, 0.1f + p * 0.2f, 0.5f + p * 0.1f);
		}
	};
	const auto owlttReadsItsOwn = [&] {
		const size_t position = bridge.fxSlotOf(owlttId);
		const float p = static_cast<float>(position);
		REQUIRE(host.readingCountAt(owlttId) == feathers::fx::owltt::Owltt::kReadingCount); // the effective ratios (20..25), then ten meters + 3 band lights + combined GR (backlog EL) + second-channel GR (LQ) + Key Track's note
		CHECK(host.readingAt(owlttId, 0) == -1.0f - p);
		CHECK(host.readingAt(owlttId, 3) == -10.0f - p);
		CHECK(host.readingAt(owlttId, 6) == -4.0f - p);
		CHECK(host.readingAt(owlttId, 9) == (position % 2 == 1 ? 1.0f : 0.0f));
	};
	const auto automodReadsItsOwn = [&] {
		const size_t position = bridge.fxSlotOf(automodId);
		const float p = static_cast<float>(position);
		REQUIRE(host.readingCountAt(automodId) == 2);
		CHECK(host.readingAt(automodId, 0) == doctest::Approx(0.1f + p * 0.2f));
		CHECK(host.readingAt(automodId, 1) == doctest::Approx(0.5f + p * 0.1f));
	};

	publishAll();
	// APPENDED ONLY: lanes equal positions, and the old code agreed with the new — the state every
	// fixture had been in, which is why nothing saw this.
	owlttReadsItsOwn();
	automodReadsItsOwn();
	CHECK(host.readingCountAt(filterId) == 0);

	// THE MOVE: OWLTT dragged to the front. Ids stay; positions shift; the publish is by
	// position and the card asks by id.
	REQUIRE(bridge.moveFxDevice(base, 0));
	REQUIRE(bridge.fxDeviceAt(0) == owlttId);
	REQUIRE(bridge.fxSlotOf(owlttId) == 0);
	REQUIRE(bridge.fxSlotOf(filterId) == 1);
	publishAll();
	owlttReadsItsOwn();
	automodReadsItsOwn();
	CHECK(host.readingCountAt(filterId) == 0);

	// AND THE AUTOMOD TO THE FRONT AS WELL, so every device has been away from the number it was
	// born at: automod at 0, owltt at 1, filter at 2.
	REQUIRE(bridge.moveFxDevice(2, 0));
	REQUIRE(bridge.fxSlotOf(automodId) == 0);
	REQUIRE(bridge.fxSlotOf(owlttId) == 1);
	REQUIRE(bridge.fxSlotOf(filterId) == 2);
	publishAll();
	owlttReadsItsOwn();
	automodReadsItsOwn();
	CHECK(host.readingCountAt(filterId) == 0);
	// A reading asked of the filter past its (zero) count is zero, not a neighbour's.
	CHECK(host.readingAt(filterId, 0) == 0.0f);
}
TEST_CASE("BI PROBE automod terrain rebuild cost") {
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::AutomodPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	const auto t0 = std::chrono::steady_clock::now();
	constexpr int kRounds = 20;
	for (int i = 0; i < kRounds; ++i) {
		pad->setZoom(i % 2 == 0 ? 2.0 : 1.0);
	}
	const auto t1 = std::chrono::steady_clock::now();
	const double us = std::chrono::duration<double, std::micro>(t1 - t0).count() / kRounds;
	MESSAGE("automod rebuildMap: " << us << " us per build (" << ui::AutomodPad::kMapWide * ui::AutomodPad::kMapHigh << " cells, " << us / (ui::AutomodPad::kMapWide * ui::AutomodPad::kMapHigh) << " us per cell)");
	CHECK(pad->rebuildsForTest() >= kRounds);
}
// ---------------------------------------------------------------------------------------------
// The coarse Type, Flavor and Mod value boxes moved far too fast under the mouse.
// ---------------------------------------------------------------------------------------------
TEST_CASE("a collapsed coordinate drags at the speed of the digit grabbed, never its whole window") {
	// The coarse Type, Flavor and Mod value boxes moved far too fast under the mouse. The answer used
	// to be a fixed sixteen carries per drag on the coarse half. The pair has since collapsed to one
	// control (BJ piece 3), and the answer is now the other rule decided the same day — the speed of
	// a mouse edit depends on where the box is grabbed — which is a
	// better answer to the same complaint: sixteen carries is one speed chosen once, and this is as
	// many speeds as the number has digits.
	//
	// **THE HAZARD IT REPLACED IS STILL HERE AND STILL HAS TO BE EXCLUDED.** The window is ±2^24, and
	// a `ValueBox` with no span reaches its whole declared range in one drag. The old seating kept
	// that away with `setDragSpan`; this one keeps it away by never falling back to the range at all —
	// a press on a digit takes the digit, and a press that misses one takes the FINEST digit
	// (`ValueBox::finestPlace`). Both are asserted, because it is the second that the collapse newly
	// depends on and that nothing else would have caught.
	namespace amd = feathers::fx::automod;
	AutomodStripFixture f;
	REQUIRE_FALSE(f.card->coordsForTest().empty());

	for (const std::unique_ptr<ui::ZoneCoordCell>& coord : f.card->coordsForTest()) {
		ui::ValueBox& box = coord->box();
		const device::ParamDesc& desc = box.control().desc();
		CAPTURE(desc.name);

		// **NO DECLARED SPAN, DELIBERATELY** — a span would OUTRANK the digit place (that precedence is
		// what keeps Gamma's sixteen laps and the shaper's field step working), so declaring one here
		// would silently switch this control back to a single fixed speed.
		CHECK(box.dragSpanForTest() == 0.0);

		// **WHAT IT WOULD BE WITHOUT ANY OF THIS: THE WHOLE WINDOW ACROSS 140 PIXELS.** More than a
		// hundred thousand units per pixel — the number behind "way too fast", asserted rather than
		// described, so the case says what it is protecting against.
		const double window = desc.max - desc.min;
		CHECK(window / ui::ValueBox::kDragRange > 1.0e5);

		// **A REAL DRAG, GRABBING A KNOWN COLUMN.** The value is set so the readout is "1024.00" and
		// the press lands on the leading digit — the thousands — so a quarter of a full drag moves a
		// quarter of ten thousand.
		box.control().setValue(1024.0);
		REQUIRE(box.readout() == "1024.00");
		const std::u32string wide = U"1024.00";
		const ui::ValueBox::ReadoutBox rb = box.readoutBox();
		const float total = ui::font(ui::kKnobReadoutSize).stringWidth(wide);
		const float left = rb.x + (rb.width - total) * 0.5f;
		const float firstDigit =
		    left + ui::font(ui::kKnobReadoutSize).stringWidth(wide.c_str(), 1) * 0.5f;

		constexpr float kTravel = 35.0f; // a quarter of a full drag
		box.onMouseDown().callback(at(firstDigit, 100.0f));
		box.onMouseDrag().callback(at(firstDigit, 100.0f - kTravel));
		box.onMouseUp().callback(at(firstDigit, 100.0f - kTravel));
		const double movedByDigit = box.control().value() - 1024.0;
		const double expected = static_cast<double>(kTravel) / ui::ValueBox::kDragRange * 1000.0
		                        * ui::ValueBox::kUnitsPerPlaceDrag;
		CHECK(movedByDigit == doctest::Approx(expected).epsilon(0.01));
		// 2500, NOT 4.2 MILLION: the same statement against the hazard rather than against a constant.
		CHECK(std::abs(movedByDigit) < window / 1000.0);

		// **AND A PRESS THAT MISSES EVERY DIGIT — on the padding past the number — FALLS BACK TO THE
		// FINEST COLUMN, NOT THE RANGE.** This is the road the collapse added: with no span declared,
		// the old fallback was `desc.max - desc.min`, which on this control is 2^25 wide.
		box.control().setValue(1024.0);
		const float pastTheEnd = left + total + 6.0f;
		REQUIRE(box.placeAt(pastTheEnd) == 0.0); // genuinely not on a digit
		box.onMouseDown().callback(at(pastTheEnd, 100.0f));
		box.onMouseDrag().callback(at(pastTheEnd, 100.0f - kTravel));
		box.onMouseUp().callback(at(pastTheEnd, 100.0f - kTravel));
		const double movedByMiss = box.control().value() - 1024.0;
		// The readout prints two decimals, so the finest column is hundredths: a quarter drag moves
		// a quarter of 0.1.
		CHECK(movedByMiss
		      == doctest::Approx(static_cast<double>(kTravel) / ui::ValueBox::kDragRange * 0.01
		                         * ui::ValueBox::kUnitsPerPlaceDrag)
		             .epsilon(0.01));
		// AND IT IS THE SLOWER OF THE TWO, which is the property that matters if the numbers change.
		CHECK(std::abs(movedByMiss) < std::abs(movedByDigit));
	}
}
// ---------------------------------------------------------------------------------------------
// Automod's rotate was bounded at 0-100%; Explore's and the Shaper's rotate are the reference.
// ---------------------------------------------------------------------------------------------
TEST_CASE("Automod's Rotate is an unbounded re-aiming, drawn and dragged the way the shaper's is") {
	namespace amd = feathers::fx::automod;
	namespace ps = feathers::fx::automod::phase_space;
	namespace shp = feathers::fx::shaper;

	// **THE DECLARATION IS A DRAG WINDOW, NOT A DOMAIN — and it is the shaper's own window.** It was
	// `0 .. 2pi`: one turn, and the editor drew a knob running 0-100% of it.
	const device::ParamDesc& rot = amd::kParamDescs[static_cast<size_t>(amd::Param::Rotate)];
	CHECK(rot.min < -1.0e6);
	CHECK(rot.max > 1.0e6);
	CHECK(rot.max - rot.min
	      == doctest::Approx(shp::kParamDescs[static_cast<size_t>(shp::Param::Rotate)].max
	                         - shp::kParamDescs[static_cast<size_t>(shp::Param::Rotate)].min));
	// A TURN IS STILL A TURN — the window is wider than one, which is the whole point.
	constexpr double kTurn = 6.2831853;
	CHECK(rot.max > kTurn * 100.0);

	// **A TURN IS NOT A PERIOD, WHICH IS WHY AN UNBOUNDED ROTATE IS WORTH HAVING — AND WHY THE
	// ENGINE'S OLD WRAP WAS A LOSS.** `settingsOf` used to fold rotate into `[0, 2pi)` believing "a
	// patch carrying 7 radians means the same basis as one carrying 7 - 2pi". The basis is
	// `sliceCoefficient` over phi triangles and phi is irrational: past the turn is NEW terrain, not
	// a repeat. Asserted, because it is the whole argument for both halves of this change.
	for (const double where : { 0.4, 2.0, 5.5 }) {
		CAPTURE(where);
		const ps::Basis a = ps::basisFor(where, 0.25);
		const ps::Basis b = ps::basisFor(where + kTurn, 0.25);
		double widest = 0.0;
		for (size_t i = 0; i < a.x.size(); ++i) {
			widest = std::max(widest, std::fabs(static_cast<double>(a.x[i] - b.x[i])));
			widest = std::max(widest, std::fabs(static_cast<double>(a.y[i] - b.y[i])));
		}
		CHECK(widest > 0.01);
	}

	// **AND NO STORED PATCH CHANGES MEANING.** Over the old declared range the wrap was the identity,
	// so every value a patch can hold derives to what it always did — through the REAL engine seam,
	// not a restatement of it.
	{
		amd::Row row = amd::rowOf(amd::Settings {});
		for (const double where : { 0.0, 0.4, 3.0, 6.2 }) {
			CAPTURE(where);
			row[static_cast<size_t>(amd::Param::Rotate)] = static_cast<float>(where);
			CHECK(amd::settingsOf(row).rotate == doctest::Approx(where).epsilon(1.0e-6));
		}
		// A value the old range could not hold now means itself rather than itself-minus-a-turn.
		row[static_cast<size_t>(amd::Param::Rotate)] = static_cast<float>(kTurn + 1.0);
		CHECK(amd::settingsOf(row).rotate == doctest::Approx(kTurn + 1.0).epsilon(1.0e-6));
	}

	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	// **A BOX, NOT A KNOB**, for the reason gamma is one — and a knob is what made it read 0-100%.
	const ui::GammaBox* rotate = f.panel->rotateForTest();
	REQUIRE(rotate != nullptr);
	for (const std::unique_ptr<ui::Knob>& knob : f.panel->knobsForTest()) {
		CHECK(f.panel->paramOfKnobForTest(knob.get()) != amd::Param::Rotate);
	}
	// ONE UNIT PER DRAG — the shaper's sensitivity, not GammaBox's sixteen.
	CHECK(rotate->dragSpanForTest() == doctest::Approx(1.0));

	// AND IT IS STILL REACHABLE, which is what the reachability sweep asks: a param no widget holds
	// is a param nobody can move, and Rotate stopped being a knob in this commit.
	const std::vector<amd::Param> addressed = f.panel->addressedParamsForTest();
	CHECK(std::find(addressed.begin(), addressed.end(), amd::Param::Rotate) != addressed.end());
}
// ---------------------------------------------------------------------------------------------
// Harmonic changes through the dropdown seemed not to work in the floating window only, and on
// inspection the window's controls on HARM were changing OWLTT's settings — the expand road handed the panel a POSITION where a panel is addressed by LANE.
// (That was the diagnosis in the world it was made in. A panel is addressed by the device's ID now,
// and there is no lane to hand it instead — kill-lanes steps 2 and 3b. What has not changed is the
// shape of the mistake: a place in a chain is not a device, and looking again at a place you
// remembered gives you whoever stands there now.)
// ---------------------------------------------------------------------------------------------
TEST_CASE("an expanded panel is addressed by its device's ID, so harm's window does not edit OWLTT") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);

	const size_t harmPos = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	const size_t owlttPos = f.bridge.addFxDevice(fx::owltt::kTraits.tag);
	REQUIRE(harmPos < feathers::kMaxFxSlots);
	REQUIRE(owlttPos < feathers::kMaxFxSlots);
	f.bridge.refresh();
	lane->resized();
	const DeviceInstanceId harmId = f.bridge.fxDeviceAt(harmPos);
	const DeviceInstanceId owlttId = f.bridge.fxDeviceAt(owlttPos);

	// **THE EXACT ARRANGEMENT.** Move harm to OWLTT's position: harm now STANDS where OWLTT stood, so
	// anything that remembered "OWLTT is the device at this index" and looked again lands on harm — and
	// the reverse, which is what was seen. A chain only ever appended to never separates the two, which is
	// why no fixture caught it.
	//
	// **IT SAID "LANE" HERE AND THE WORD HAS NO REFERENT NOW (kill-lanes step 3b).** The arrangement was
	// stated as harm's POSITION coming to equal OWLTT's LANE, captured through `fxLaneAt` before the move.
	// A device's coordinate IS its position since 3b, so `fxLaneAt(bridge, p)` is `p` and those two
	// captures were just `harmPos` and `owlttPos` wearing another name. The claims underneath are
	// unchanged and are stated directly: the move put harm where OWLTT was, and took it off its own index.
	REQUIRE(f.bridge.moveFxDevice(harmPos, owlttPos));
	const size_t harmNow = f.bridge.fxSlotOf(harmId);
	REQUIRE(harmNow == owlttPos); // harm stands where OWLTT stood — the collision, stated rather than assumed
	REQUIRE(harmNow != harmPos); // and it really moved, or this case proves nothing
	REQUIRE(f.bridge.fxDeviceAt(harmPos) != harmId); // its old index now answers with another device

	lane->scrollCardIntoView(harmNow);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true); // on the overlay, the host this case reads
	lane->onExpandDevice(harmNow);
	REQUIRE(overlay->open());

	const auto* panel = presentedAs<ui::HarmExpanded>(overlay);
	REQUIRE(panel != nullptr); // the press opened HARM's panel: the descriptor road was always right
	REQUIRE(panel->scopeForTest() != nullptr);
	// **THE VALUE THAT WAS WRONG.** Every control on this panel reads and writes through this number.
	CHECK(panel->scopeForTest()->device() == harmId);
	CHECK(panel->scopeForTest()->device() != owlttId);
}
//------------------------------------------------------------------------------------------------//
// BUS UTILITY'S METER, AND THE 0 dBFS LED'S TWO TIME CONSTANTS                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the 0 dBFS LED derives an instantaneous core and a decaying glow from ONE published age") {
	// A prominent 0 dBFS peak LED, with an instantaneous core and a glow that decays over time. Two
	// time constants, and BOTH ARE DRAWING DECISIONS taken from a single number the
	// audio thread publishes: `Reading::ClipAge`, seconds since the most recent sample at or over
	// full scale.
	//
	// **ASSERTED AS THE TWO VALUES, NOT AS PIXELS.** A rendered comparison would pass whenever
	// anything about the card differed, which is CLAUDE.md's rule and is at its weakest exactly here:
	// a glow is a partial effect by construction, so "the card drew differently" cannot tell a
	// correct fade from a wrong one, from one that never fades, from one that fades on the wrong
	// input.
	namespace bu = feathers::fx::busutil;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(bu::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	auto* card = dynamic_cast<ui::BusUtilCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);

	const auto publish = [&](float clipAge) {
		std::array<float, static_cast<size_t>(bu::Reading::kCount)> values {};
		values[static_cast<size_t>(bu::Reading::PeakL)] = 0.5f;
		values[static_cast<size_t>(bu::Reading::PeakR)] = 0.5f;
		values[static_cast<size_t>(bu::Reading::Momentary)] = -18.0f;
		values[static_cast<size_t>(bu::Reading::ShortTerm)] = -18.0f;
		values[static_cast<size_t>(bu::Reading::Integrated)] = -18.0f;
		values[static_cast<size_t>(bu::Reading::Correlation)] = 1.0f;
		values[static_cast<size_t>(bu::Reading::ClipAge)] = clipAge;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
	};

	SUBCASE("nothing published draws no meter at all, and no LED") {
		// **A METER WITH NO DATA AND A DEVICE READING SILENCE MUST NOT LOOK ALIKE** — `device_host.h`'s
		// rule, and the one OWLTT's frozen bars paid for. An LED dark because nothing has clipped and
		// an LED dark because nobody is measuring are the same picture; the card refusing to draw the
		// meter is what makes them different.
		CHECK_FALSE(card->hasReadings());
		CHECK_FALSE(card->ledCoreLit());
		CHECK(card->ledGlow() == 0.0f);
	}

	SUBCASE("an over this instant lights the core and the glow together") {
		publish(0.0f);
		REQUIRE(card->hasReadings());
		CHECK(card->ledCoreLit());
		CHECK(card->ledGlow() == doctest::Approx(1.0f));
	}

	SUBCASE("the core goes out while the glow is still visible — which is the whole point") {
		// **THE TWO CONSTANTS EXIST TO DISAGREE.** If the core and the glow died together the second
		// one would be decoration. This is the window where the LED says "an over happened A MOMENT
		// AGO" — the thing a hard latch cannot express and a bare instantaneous light cannot either.
		publish(ui::BusUtilCard::kCoreHoldSeconds * 2.0f);
		CHECK_FALSE(card->ledCoreLit());
		CHECK(card->ledGlow() > 0.5f);
	}

	SUBCASE("the glow is an exponential decay of the published age, by value") {
		// The curve itself, against an independent restatement — not against a second run of the card.
		publish(0.5f);
		CHECK(card->ledGlow()
		      == doctest::Approx(std::exp(-0.5f / ui::BusUtilCard::kGlowFallSeconds)).epsilon(1e-5));
		publish(1.5f);
		CHECK(card->ledGlow()
		      == doctest::Approx(std::exp(-1.5f / ui::BusUtilCard::kGlowFallSeconds)).epsilon(1e-5));
		// AND IT IS MONOTONE, which a difference check could not tell from a curve that wandered.
		publish(0.2f);
		const float near = card->ledGlow();
		publish(2.0f);
		CHECK(card->ledGlow() < near);
	}

	SUBCASE("a long-ago over leaves nothing lit") {
		publish(feathers::fx::busutil::kClipAgeMax);
		CHECK_FALSE(card->ledCoreLit());
		CHECK(card->ledGlow() == 0.0f);
	}

	SUBCASE("the card reads each reading BY NAME, so no two indices can answer for each other") {
		// automod's readings road is the reason this case exists: a first version answered index 0's
		// value for ANY index, and a second declared reading silently returned the first one's number.
		// Every value below is distinct, so a card that indexed wrongly cannot agree by coincidence.
		std::array<float, static_cast<size_t>(bu::Reading::kCount)> values {};
		values[static_cast<size_t>(bu::Reading::PeakL)] = 0.11f;
		values[static_cast<size_t>(bu::Reading::PeakR)] = 0.22f;
		values[static_cast<size_t>(bu::Reading::Momentary)] = -33.0f;
		values[static_cast<size_t>(bu::Reading::ShortTerm)] = -44.0f;
		values[static_cast<size_t>(bu::Reading::Integrated)] = -55.0f;
		values[static_cast<size_t>(bu::Reading::Correlation)] = -0.66f;
		values[static_cast<size_t>(bu::Reading::ClipAge)] = 7.0f;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());

		CHECK(card->reading(bu::Reading::PeakL) == doctest::Approx(0.11f));
		CHECK(card->reading(bu::Reading::PeakR) == doctest::Approx(0.22f));
		CHECK(card->reading(bu::Reading::Momentary) == doctest::Approx(-33.0f));
		CHECK(card->reading(bu::Reading::ShortTerm) == doctest::Approx(-44.0f));
		CHECK(card->reading(bu::Reading::Integrated) == doctest::Approx(-55.0f));
		CHECK(card->reading(bu::Reading::Correlation) == doctest::Approx(-0.66f));
		CHECK(card->reading(bu::Reading::ClipAge) == doctest::Approx(7.0f));
	}

	SUBCASE("a moving meter asks for a frame and a still one does not") {
		// `updateContent`'s contract. A meter that returned true every tick would repaint the whole
		// card at the host's timer rate whether or not anything had moved.
		publish(3.0f);
		CHECK(card->updateContent());  // first sight of these numbers
		CHECK_FALSE(card->updateContent()); // unchanged
		publish(0.0f);
		// **THE CLIP AGE ALONE MUST BE ENOUGH TO ASK FOR A FRAME.** An over during an otherwise steady
		// passage moves nothing else on this card — every other reading here is identical — so a
		// comparison that left the age out would light the LED only on whatever frame something else
		// happened to change.
		CHECK(card->updateContent());
	}
}
TEST_CASE("backlog ML: Bus Utility's 0 dBFS LED has three states by the published raw peak — normal under -2 dBFS, near to just under 0, clip at 0 and over") {
	namespace bu = feathers::fx::busutil;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(bu::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	auto* card = dynamic_cast<ui::BusUtilCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	CHECK(card->ledStateForTest() == ui::ClipLedState::Normal); // nothing published

	const auto stateAt = [&](double dbfs, float clipAge) {
		const float peak = static_cast<float>(std::pow(10.0, dbfs / 20.0));
		std::array<float, static_cast<size_t>(bu::Reading::kCount)> values {};
		values[static_cast<size_t>(bu::Reading::PeakL)] = peak;
		values[static_cast<size_t>(bu::Reading::PeakR)] = peak;
		values[static_cast<size_t>(bu::Reading::RawPeak)] = peak;
		values[static_cast<size_t>(bu::Reading::ClipAge)] = clipAge;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
		REQUIRE(card->reading(bu::Reading::RawPeak) == peak);
		return card->ledStateForTest();
	};
	const float none = bu::kClipAgeMax;
	CHECK(stateAt(-2.1, none) == ui::ClipLedState::Normal);
	CHECK(stateAt(-1.0, none) == ui::ClipLedState::Near);
	CHECK(stateAt(0.0, none) == ui::ClipLedState::Clip);
	CHECK(stateAt(0.1, none) == ui::ClipLedState::Clip);
	// The core's hold is the over the engine latched: a clip while it is lit, whatever the peak now reads.
	CHECK(stateAt(-20.0, 0.0f) == ui::ClipLedState::Clip);
}
TEST_CASE("backlog MK: an fx card's one-pixel output meter reads its device's published peak — hidden while nothing is published, the level on a dB line, near and clip in their colours, the over filling from the far end by the dB over") {
	// Every fx card carries a courtesy output meter; when clipping, the bar fills from the opposite end
	// by the amount over 0 dBFS, in the clip colour. EVERY EXPECTED LENGTH IS RESTATED HERE from
	// the spec's numbers (a -48 dBFS floor, 12 dB over fills the line, a 2 px least over) rather than read from the card.
	namespace bu = feathers::fx::busutil;
	for (const char* tag : { bu::kTraits.tag, feathers::fx::delay::kTraits.tag }) {
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		const ui::CardOutputMeter& meter = card->outputMeterForTest();

		// NOTHING PUBLISHED: no line at all, rather than a line reading silence.
		(void)card->update();
		CHECK_FALSE(meter.isVisible());
		CHECK(card->outputMeterPeakForTest() < 0.0f);

		// THE TRACK: one pixel along the head's inner edge, inside the border and clear of the corners.
		const bool spine = card->headEdge() == ui::CardHeadEdge::Left;
		const float w = static_cast<float>(card->width());
		const float h = static_cast<float>(card->height());
		const float length = spine ? h - 2.0f * ui::kCardCorner : w - 2.0f * ui::kCardCorner;
		REQUIRE(length > 40.0f);
		if (spine) {
			CHECK(meter.x() == doctest::Approx(ui::kCardHeadHeight - 2.0f));
			CHECK(meter.y() == doctest::Approx(ui::kCardCorner));
			CHECK(meter.width() == doctest::Approx(1.0f));
			CHECK(meter.height() == doctest::Approx(length));
		}
		else {
			CHECK(meter.x() == doctest::Approx(ui::kCardCorner));
			CHECK(meter.y() == doctest::Approx(ui::kCardHeadHeight - 2.0f));
			CHECK(meter.width() == doctest::Approx(length));
			CHECK(meter.height() == doctest::Approx(1.0f));
		}
		// The run along the line, whichever way the head runs: (from-start, length) of a mark.
		const auto along = [&](const visage::Bounds& b) { return spine ? b.height() : b.width(); };
		const auto startOf = [&](const visage::Bounds& b) { return spine ? length - (b.y() + b.height()) : b.x(); };

		const auto at = [&](double dbfs) {
			const auto peak = static_cast<float>(std::pow(10.0, dbfs / 20.0));
			f.bridge.publishFxOutputPeak(slot, peak);
			(void)card->update();
			REQUIRE(card->outputMeterPeakForTest() == peak); // the value the card read IS the one published
			REQUIRE(meter.isVisible());
			return meter.marks();
		};
		const auto levelFor = [&](double dbfs) { return std::round(static_cast<float>((dbfs + 48.0) / 48.0) * length); };

		SUBCASE("normal: the level from the start in the value colour, no over") {
			const ui::CardMeterMarks m = at(-24.0);
			CHECK(std::abs(along(m.level) - levelFor(-24.0)) <= 1.0f); // a pixel either way: two roundings of one number
			CHECK(startOf(m.level) == doctest::Approx(0.0f));
			CHECK(m.levelColour == ui::kMeterValue);
			CHECK(along(m.over) == doctest::Approx(0.0f));
		}
		SUBCASE("near, from -2 dBFS: the near colour, still no over") {
			const ui::CardMeterMarks m = at(-1.0);
			CHECK(std::abs(along(m.level) - levelFor(-1.0)) <= 1.0f);
			CHECK(m.levelColour == ui::kMeterNear);
			CHECK(along(m.over) == doctest::Approx(0.0f));
			CHECK(at(-2.1).levelColour == ui::kMeterValue); // and just under the threshold, normal
		}
		SUBCASE("over: a full line, and 6 dB over fills half of it from the FAR end in the clip colour") {
			const ui::CardMeterMarks m = at(6.0);
			CHECK(along(m.level) == doctest::Approx(length));
			CHECK(m.levelColour == ui::kMeterNear);
			CHECK(std::abs(along(m.over) - std::round(0.5f * length)) <= 1.0f);
			CHECK(startOf(m.over) + along(m.over) == doctest::Approx(length)); // it ends at the far end
			CHECK(m.overColour == ui::kMeterClip);
			// EXACTLY full scale is a clip (`clip_led.h`) with nothing over it: the least over, 2 px, so it still shows red.
			CHECK(along(at(0.0).over) == doctest::Approx(2.0f));
			// AND 12 dB OVER OR MORE IS THE WHOLE LINE.
			CHECK(along(at(18.0).over) == doctest::Approx(length));
		}
		SUBCASE("a reading that does not move asks for no frame, and one that does asks for one") {
			f.bridge.publishFxOutputPeak(slot, 0.25f);
			(void)card->update();
			CHECK_FALSE(card->syncOutputMeter());
			f.bridge.publishFxOutputPeak(slot, 0.9f);
			CHECK(card->syncOutputMeter());
		}
	}
}
TEST_CASE("a chain's card says what the chain HOLDS in its head, and carries the chain's own controls in its body (D6)") {
	// D6 (the summary as the head, the contents as the body, plus mix and gain), drawn 2026-09-13.
	// Before this a chain fell to `GenericCard` — eleven knobs on a grid and no word about what was
	// inside: a shell of a UI.
	//
	// THE SUMMARY IS READ FROM THE BODY THE HOST CARRIES, not from the running instance — the body is
	// the inner rack's own file — so the fixture writes a real rack file (two devices) and hands it to
	// the bridge as the chain's body, exactly the road a patch load takes. The expected text is restated
	// from the registry's names in the order the devices were added, never read back from the card.
	namespace cmp = feathers::composite;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::ChainCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	CHECK(dynamic_cast<ui::GenericCard*>(lane->cardAt(slot)) == nullptr);

	SUBCASE("the body: ten knobs — eight macros, mix, gain — and the head's enable make the eleven") {
		CHECK(card->knobsForTest().size() == cmp::kMacroCount + 2);
		std::set<size_t> addressed;
		for (const cmp::Param p : card->addressedParamsForTest()) {
			addressed.insert(static_cast<size_t>(p));
		}
		addressed.insert(static_cast<size_t>(cmp::Param::Enabled));
		CHECK(addressed.size() == static_cast<size_t>(cmp::Param::kCount));
		// A knob drives the device: turn Mix and the chain's row moves.
		const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
		const float before = f.deviceHost.fieldAt(id, static_cast<size_t>(cmp::Param::Mix));
		f.deviceHost.setField(id, static_cast<size_t>(cmp::Param::Mix), 0.25f);
		CHECK(f.deviceHost.fieldAt(id, static_cast<size_t>(cmp::Param::Mix)) == doctest::Approx(0.25f));
		CHECK(before != doctest::Approx(0.25f));
	}
	SUBCASE("the head: an empty chain says so, and a chain with two devices names them in order") {
		CHECK(card->heldCountForTest() == 0);
		CHECK(card->summaryForTest() == "empty");

		// A REAL RACK FILE — the standalone engine, two devices, saved the way a chain's body is saved.
		feathers::rack::RackEngine inner { feathers::rack::kRackAxes, feathers::hostCatalogue() };
		inner.prepare(48000.0, 4096);
		REQUIRE(inner.addDevice(feathers::fx::filter::kTraits.tag) != feathers::rack::RackEngine::kNoSlot);
		REQUIRE(inner.addDevice(feathers::fx::owltt::kTraits.tag) != feathers::rack::RackEngine::kNoSlot);
		struct Sink {
			std::vector<uint8_t> bytes;
			static int64_t CLAP_ABI write(const clap_ostream* stream, const void* buffer, uint64_t size) {
				auto* self = static_cast<Sink*>(stream->ctx);
				const auto* p = static_cast<const uint8_t*>(buffer);
				self->bytes.insert(self->bytes.end(), p, p + size);
				return static_cast<int64_t>(size);
			}
		} sink;
		const clap_ostream out { &sink, &Sink::write };
		REQUIRE(feathers::rack::saveRack(&out, inner));
		REQUIRE(!sink.bytes.empty());

		feathers::ContainerBodies bodies;
		const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
		REQUIRE(bodies.store(id, sink.bytes.data(), sink.bytes.size()));
		f.bridge.restoreFxBodies(std::move(bodies));
		REQUIRE(f.deviceHost.containerBodyGenerationOf(id) != 0);

		// THE CARD RE-READS WHEN THE HOST'S BODY MOVED — `updateContent` is the lane's per-tick question.
		CHECK(card->updateContent());
		const std::string expected = std::string(feathers::deviceForTag(feathers::fx::filter::kTraits.tag)->traits.name)
		                             + " \xE2\x80\xBA " + feathers::deviceForTag(feathers::fx::owltt::kTraits.tag)->traits.name;
		CHECK(card->summaryForTest() == expected);
		CHECK(card->heldCountForTest() == 2);
		// AND NOT AGAIN FOR THE SAME BODY.
		CHECK_FALSE(card->updateContent());
		// A folded chain keeps its head, and its head keeps the summary.
		card->setFolded(true);
		CHECK(card->laneHeight() == doctest::Approx(ui::ChainCard::foldedHeight()));
		CHECK(card->summaryForTest() == expected);
	}
}
TEST_CASE("a chain panel taller than its canvas card SCROLLS to its cards' feet — the inner chain answers only the wheel it can use, the rest bubbles to the mount") {
	// With a chain in the fx column expanded to the canvas, the header controls of the inner chain's
	// cards clipped off the canvas and could not be scrolled to. The rack editor wants 560 rows and
	// the band's card has fewer, so the mount scrolls — but the inner editor answered EVERY wheel
	// (returning true whether or not its chain moved), so no wheel over the panel ever reached the
	// mount. Now a wheel the chain cannot use bubbles, the way visage walks parents until one answers.
	namespace cmp = feathers::composite;
	feathers::rack::registerChainExpandedView();
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	feathers::rack::RackEngine outer { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane2 = outer.addDevice(cmp::kTraits.tag);
	REQUIRE(lane2 != feathers::rack::RackEngine::kNoSlot);
	void* storage = outer.slotAtLane(lane2).storage.get();
	feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(storage);
	REQUIRE(inner != nullptr);
	REQUIRE(inner->addDevice(feathers::fx::owltt::kTraits.tag) != feathers::kNoInstance);
	inner->drain();
	f.bridge.fxBodies().attach(id, storage);

	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(canvas->setCollapsed(false)); f.grantPreferredSize(); // the host grants the room (backlog EJ)
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(slot);
	REQUIRE(f.gui.expandedForTest()->open());
	auto* mounted = const_cast<ui::MountedFrame*>(dynamic_cast<const ui::MountedFrame*>(f.gui.expandedForTest()->contentForTest()));
	REQUIRE(mounted != nullptr);
	auto* view = dynamic_cast<feathers::rack::ChainExpanded*>(mounted->contentForTest());
	REQUIRE(view != nullptr);
	(void)view->updateContent();
	REQUIRE(view->liveForTest());
	feathers::rack::RackEditor* editor = view->editorForTest();
	REQUIRE(editor != nullptr);
	editor->syncCards();
	// A LAYER SHORTER THAN THE CHAIN. Until 2026-10-03 the band's own floor was that layer; now the
	// panel floats on the editor's whole overlay (canvas doc §3f), which is taller than a chain, so the
	// shape is made by giving the layer less room — what a small window at a large scale does.
	{
		ui::SurfaceLayers* layers = f.gui.surfacesForTest();
		REQUIRE(layers != nullptr);
		layers->setBounds(0.0f, 0.0f, layers->width(), 380.0f); // (400 before the route-list band went: the lane is 94 px shorter)
	}
	mounted->resized();
	REQUIRE_FALSE(editor->cardsForTest().empty());
	ui::FxCard* card = editor->cardsForTest()[0];
	REQUIRE(card != nullptr);

	// THE SHAPE OF THE COMPLAINT: the content is taller than the mount, so the card's foot is below
	// the mount's bottom edge and a scroll is the only way to it.
	REQUIRE(mounted->contentHeightForTest() > static_cast<float>(mounted->height()));
	const float footInMount = card->positionInWindow().y + card->height() - mounted->positionInWindow().y;
	REQUIRE(footInMount > static_cast<float>(mounted->height()));
	CHECK(mounted->scroll() == 0.0f);
	// AND THE CONTENT LEAVES THE GUTTER FOR THE BAR.
	CHECK(view->width() == mounted->width() - static_cast<int>(ui::MountedFrame::kScrollGutter));

	// A WHEEL OVER THE INNER CARD, walked the way visage's window handler walks it: the frame under
	// the pointer first, then up the parents until one answers.
	const auto wheelAt = [&](visage::Point window, float dy) {
		visage::MouseEvent e;
		e.window_position = window;
		e.wheel_delta_y = dy;
		visage::Frame* at = f.root->frameAtPoint(window);
		bool used = false;
		while (!used && at != nullptr) {
			while (at != nullptr && at->ignoresMouseEvents()) {
				at = at->parent();
			}
			if (at == nullptr) {
				break;
			}
			e.position = window - at->positionInWindow();
			e.event_frame = at;
			used = at->processMouseWheel(e);
			at = at->parent();
		}
		return used;
	};
	const visage::Point overCard = card->positionInWindow() + visage::Point { 20.0f, 20.0f };
	// THE CHAIN HOLDS ONE DEVICE, SO IT CANNOT SCROLL: the wheel is not its to answer, and the mount
	// takes it — the content moves up.
	CHECK(wheelAt(overCard, -1.0f));
	CHECK(mounted->scroll() > 0.0f);
	const float scrolled = mounted->scroll();
	// AND THE OTHER WAY brings it back; past the top it stays at the top.
	CHECK(wheelAt(overCard, 1.0f));
	CHECK(mounted->scroll() < scrolled);
	for (int i = 0; i < 10; ++i) {
		(void)wheelAt(overCard, 1.0f);
	}
	CHECK(mounted->scroll() == 0.0f);
	// ENOUGH WHEEL REACHES THE FOOT: after scrolling to the end the card's bottom is inside the mount.
	for (int i = 0; i < 40; ++i) {
		(void)wheelAt(overCard, -1.0f);
	}
	CHECK(mounted->scroll() > 0.0f);
	const float footNow = card->positionInWindow().y + card->height() - mounted->positionInWindow().y;
	CHECK(footNow <= static_cast<float>(mounted->height()) + 1.0f);
	f.gui.expandedForTest()->dismiss();
}
TEST_CASE("§7: inside a chain on the canvas, the chain's own arm lights its inner cards and their panels; an OUTER source lights the macro row AND the inner cards, and a drag on an inner knob writes an inner route from the outer source") {
	// In a chain, neither its own macros nor its inner cards' controls glowed. The chain panel's controls
	// answer the INNER rack's modulation state: its macro arms arm that state, and its cards' and
	// panels' knobs read it. So the inner arm must light them — and Feathers' own macro must not,
	// which is the design as built and is written down here so the difference is a decision, not a
	// surprise.
	namespace cmp = feathers::composite;
	feathers::rack::registerChainExpandedView();
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(cmp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	lane->resized();
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	feathers::rack::RackEngine outer { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	outer.prepare(48000.0, 4096);
	const size_t lane2 = outer.addDevice(cmp::kTraits.tag);
	REQUIRE(lane2 != feathers::rack::RackEngine::kNoSlot);
	void* storage = outer.slotAtLane(lane2).storage.get();
	feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(storage);
	REQUIRE(inner != nullptr);
	REQUIRE(inner->addDevice(feathers::fx::harm::kTraits.tag) != feathers::kNoInstance);
	inner->drain();
	f.bridge.fxBodies().attach(id, storage);

	ui::ExpandedCanvas* canvas = f.gui.canvasForTest();
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(canvas != nullptr);
	REQUIRE(overlay != nullptr);
	REQUIRE(canvas->setCollapsed(false)); f.grantPreferredSize(); // the host grants the room (backlog EJ)
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());
	auto* mounted = const_cast<ui::MountedFrame*>(dynamic_cast<const ui::MountedFrame*>(overlay->contentForTest()));
	REQUIRE(mounted != nullptr);
	auto* view = dynamic_cast<feathers::rack::ChainExpanded*>(mounted->contentForTest());
	REQUIRE(view != nullptr);
	(void)view->updateContent();
	REQUIRE(view->liveForTest());
	feathers::rack::RackEditor* editor = view->editorForTest();
	REQUIRE(editor != nullptr);
	editor->syncCards();
	mounted->resized();
	REQUIRE_FALSE(editor->cardsForTest().empty());
	ui::FxCard* card = editor->cardsForTest()[0];
	REQUIRE(card != nullptr);

	std::function<void(visage::Frame*, std::vector<ui::SourceArm*>*)> arms = [&](visage::Frame* at, std::vector<ui::SourceArm*>* out) {
		if (auto* arm = dynamic_cast<ui::SourceArm*>(at)) {
			out->push_back(arm);
		}
		for (visage::Frame* child : at->children()) {
			arms(child, out);
		}
	};
	const auto litOf = [&](visage::Frame* root, size_t* destinations) {
		std::vector<ui::Knob*> knobs;
		collectKnobs(root, &knobs);
		*destinations = 0;
		size_t lit = 0;
		for (ui::Knob* knob : knobs) {
			if (!knob->isModDestination()) {
				continue;
			}
			++*destinations;
			lit += knob->haloLit() ? 1 : 0;
		}
		return lit;
	};
	std::vector<ui::SourceArm*> innerArms;
	arms(editor, &innerArms);
	REQUIRE_FALSE(innerArms.empty()); // the chain's own macro arms

	// THE INNER CARD'S KNOBS: dark, then lit by the chain's own arm.
	size_t destinations = 0;
	CHECK(litOf(card, &destinations) == 0);
	REQUIRE(destinations > 0);
	innerArms.front()->onMouseDown().callback(at(4.0f, 4.0f));
	CHECK(litOf(card, &destinations) == destinations);

	// THE INNER DEVICE'S PANEL — beside the chain's on the OUTER canvas (backlog EB) — reads the inner state too.
	REQUIRE(card->onExpand);
	card->onExpand();
	REQUIRE(overlay->panelCount() == 2);
	size_t panelDestinations = 0;
	CHECK(litOf(overlay->panelForTest(), &panelDestinations) == panelDestinations);
	REQUIRE(panelDestinations > 0);

	// **AN OUTER SOURCE — Feathers' own macro — LIGHTS THE INNER CARDS AND THE INNER PANEL** (rack roadmap
	// §7: destinations volunteer to read the source): the inner state follows
	// the outer arm, every inner control volunteers, and the inner gate admits the outer source because
	// the host outside says it is upstream. The gate is what the plugin's converger hands a chain at
	// provision; this fixture attached the body by hand, so it hands the same gate the same way.
	innerArms.front()->onMouseDown().callback(at(4.0f, 4.0f)); // a second press disarms the inner arm
	CHECK(litOf(card, &destinations) == 0);
	feathers::rack::RackBridge* innerBridge = view->bridgeForTest();
	REQUIRE(innerBridge != nullptr);
	innerBridge->setOuterGate(feathers::rack::ChainRecord::gateOver(&f.bridge.chainRecord(), id));
	REQUIRE(f.gui.modForTest()->parentForTest() == nullptr);
	REQUIRE(view->modForTest().parentForTest() == f.gui.modForTest());
	// THE INNER INBOUND-ROUTE MENU CAN NAME THE OUTER SOURCES: the inner state lists its parent's,
	// spelled as this rack reads them, named by the host outside and marked as from outside.
	{
		const std::vector<backplane::Source> listed = view->modForTest().sources();
		const backplane::Source outerMacro = macroSourceAt(0).asOuter();
		CHECK(std::find(listed.begin(), listed.end(), outerMacro) != listed.end());
		CHECK(view->modForTest().sourceName(outerMacro).find(f.gui.modForTest()->sourceName(macroSourceAt(0))) != std::string::npos);
		CHECK(view->modForTest().sourceName(outerMacro) != f.gui.modForTest()->sourceName(macroSourceAt(0)));
	}
	// ARMED THROUGH THE STATE, not by a click on the strip's arrow: the chain window is 88 px wider since the Engine rail
	// (backlog OC) and now stands over that arrow in this fixture; the claim here is what an armed OUTER source reaches.
	f.gui.modForTest()->focus(macroSourceAt(0));
	f.gui.modForTest()->arm(true);
	REQUIRE(f.gui.modForTest()->armed());
	CHECK(litOf(card, &destinations) == destinations);
	CHECK(litOf(overlay->panelForTest(), &panelDestinations) == panelDestinations);
	{
		// AND A DRAG ON AN INNER KNOB WRITES AN INNER ROUTE FROM THE OUTER SOURCE.
		std::vector<ui::Knob*> innerKnobs;
		collectKnobs(card, &innerKnobs);
		ui::Knob* target = nullptr;
		for (ui::Knob* k : innerKnobs) {
			if (k->isModDestination() && dynamic_cast<ui::DeviceControl*>(k->control()) != nullptr) {
				target = k;
				break;
			}
		}
		REQUIRE(target != nullptr);
		REQUIRE(target->haloLit());
		const auto* dc = dynamic_cast<const ui::DeviceControl*>(target->control());
		const backplane::Source outerMacro = macroSourceAt(0).asOuter();
		CHECK(innerBridge->routeAmount(outerMacro, dc->destination()) == 0.0f);
		const visage::Point centre = f.faceCentre(target);
		dragThroughRoot(f.root, centre, centre - visage::Point { 0.0f, 60.0f });
		CHECK(innerBridge->routeAmount(outerMacro, dc->destination()) != 0.0f);
		// NOT an outer route: Feathers' own table holds nothing for this pair.
		CHECK(f.bridge.routeAmount(macroSourceAt(0), dc->destination()) == 0.0f);

		// **AND THE OUTER ROUTE LIST SEES IT** (rack roadmap §7, hygiene's other half): the Engine rail's
		// "routes" for the armed macro lists the route inside the chain, named through the chain, and
		// its × takes it away from outside — through the chain's own gate.
		auto* list = findInTree<ui::RouteList>(f.root);
		REQUIRE(list != nullptr);
		CHECK(list->rows().empty()); // Feathers' own routes: none
		const std::vector<ui::RouteList::Row> shown = list->allRows();
		REQUIRE(shown.size() == 1);
		CHECK(shown.front().chain == id);
		CHECK(shown.front().destination == dc->destination());
		CHECK(f.bridge.innerRoutesFrom(macroSourceAt(0)).size() == 1);
		CHECK(f.bridge.innerRouteAmount(macroSourceAt(0), id, dc->destination()) != 0.0f);
		const std::string named = f.bridge.innerDestinationName(id, dc->destination());
		CHECK(named.rfind("Chain", 0) == 0);
		CHECK(named.find(fx::harm::kDescriptor.params[dc->destination().index].name) != std::string::npos);
		{
			const visage::Bounds row = list->rowBounds(0);
			const visage::Point cross = list->positionInWindow()
			                            + visage::Point { row.right() - ui::RouteList::kRemoveWidth * 0.5f,
				                                          row.y() + row.height() * 0.5f };
			const ui::RouteList::Hit hit = list->hitAt(cross - list->positionInWindow());
			REQUIRE(hit.part == ui::RouteList::Part::Remove);
			CHECK(hit.chain == id);
			// A PANEL MAY HAVE ARRIVED OVER THE ENGINE RAIL — since 2026-10-03 the band is where a panel
			// prefers to land, not a fence (canvas doc §3f), and a chain plus its device do not both fit
			// it. So any panel over the × is moved off it first, as a hand would, by its head.
			const visage::Point local = cross - overlay->positionInWindow();
			for (const ui::ExpandedOverlay::PanelId pid : overlay->panelIdsForTest()) {
				const visage::Bounds over = overlay->panelBounds(pid);
				if (!over.contains(local)) {
					continue;
				}
				visage::MouseEvent up;
				up.position = { 5.0f, 5.0f };
				overlay->panelForTest(pid)->onMouseUp().callback(up); // raised, so the drag acts on it
				REQUIRE(overlay->frontPanelIdForTest() == pid);
				// WHOLLY CLEAR OF IT: to its left, its right, below or above — the first the clamp allows.
				for (int way = 0; way < 4 && overlay->panelBounds(pid).contains(local); ++way) {
					const visage::Bounds now = overlay->panelBounds(pid);
					const visage::Point move = way == 0   ? visage::Point { local.x - now.right() - 20.0f, 0.0f }
					                           : way == 1 ? visage::Point { local.x - now.x() + 20.0f, 0.0f }
					                           : way == 2 ? visage::Point { 0.0f, local.y - now.y() + 20.0f }
					                                      : visage::Point { 0.0f, local.y - now.bottom() - 20.0f };
					overlay->beginPanelDrag();
					overlay->dragPanelBy(move);
				}
				(void)over;
			}
			REQUIRE(f.root->frameAtPoint(cross) != nullptr);
			for (const ui::ExpandedOverlay::PanelId pid : overlay->panelIdsForTest()) {
				REQUIRE_FALSE(overlay->panelBounds(pid).contains(local));
			}
			dragThroughRoot(f.root, cross, cross);
		}
		CHECK(innerBridge->routeAmount(outerMacro, dc->destination()) == 0.0f);
		CHECK(list->allRows().empty());
		// AND UNDONE FROM OUTSIDE, the route is back inside.
		REQUIRE(f.gui.history()->canUndo());
		f.gui.history()->undo();
		CHECK(innerBridge->routeAmount(outerMacro, dc->destination()) != 0.0f);
		innerBridge->setRouteAmount(outerMacro, dc->destination(), 0.0f);
	}
	// DISARMED OUTSIDE, THE INNER CARDS GO DARK AGAIN.
	(void)f.gui.modForTest()->arm(false);
	CHECK(litOf(card, &destinations) == 0);
	f.gui.modForTest()->focus(macroSourceAt(0)); // (through the state: the wider chain window stands over the strip's arrow, see above)
	f.gui.modForTest()->arm(true);

	// **BUT THE CHAIN'S OWN MACRO ROW LIGHTS FOR IT** — the macros are destinations.
	// Inside a chain the eight macros are the chain device's params on the OUTER host, so the row is
	// rebuilt over outer device controls: every macro knob is a destination, lit for the outer arm,
	// and a drag on one writes the OUTER route to the chain device's macro param.
	REQUIRE(editor->macrosAreOuterForTest());
	const auto& macroKnobs = editor->macrosForTest();
	REQUIRE(macroKnobs.size() == feathers::composite::kMacroCount);
	for (const auto& slot : macroKnobs) {
		const ui::Knob* knob = &slot->knob();
		CHECK(knob->isModDestination());
		CHECK(knob->haloLit());
		CHECK(f.gui.modForTest()->watches(knob));
	}
	{
		ui::Knob* first = &macroKnobs.front()->knob();
		// THROUGH THE POLARITY VIEW (backlog EG): the knob stands on the macro's view of its mode, and
		// the outer device control — the parameter road this case is about — is the control underneath.
		const auto* view = dynamic_cast<const ui::BipolarMacro*>(first->control());
		REQUIRE(view != nullptr);
		CHECK(first->hasMacroMenuForTest());
		const auto* dc = dynamic_cast<const ui::DeviceControl*>(&view->inner());
		REQUIRE(dc != nullptr);
		CHECK(dc->destination().device == id);
		CHECK(dc->destination().index == static_cast<size_t>(feathers::composite::Param::Macro1));
		CHECK(f.bridge.routeAmount(macroSourceAt(0), dc->destination()) == 0.0f);
		const visage::Point centre = f.faceCentre(first);
		dragThroughRoot(f.root, centre, centre - visage::Point { 0.0f, 60.0f });
		CHECK(f.bridge.routeAmount(macroSourceAt(0), dc->destination()) != 0.0f);
		f.bridge.setRouteAmount(macroSourceAt(0), dc->destination(), 0.0f);
	}
	// AND DISARMED, THE ROW GOES DARK — while the inner arm still lights the inner cards and not the row.
	(void)f.gui.modForTest()->arm(false);
	CHECK_FALSE(macroKnobs.front()->knob().haloLit());
	innerArms.front()->onMouseDown().callback(at(4.0f, 4.0f));
	CHECK(litOf(card, &destinations) == destinations);
	CHECK_FALSE(macroKnobs.front()->knob().haloLit());
	overlay->dismiss(overlay->frontPanelIdForTest());
	overlay->dismiss();
}

