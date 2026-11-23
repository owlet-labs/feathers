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

// PIECE 6 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

namespace {
/// A hand's double-click, as visage delivers it (`windowing.cpp`): the second press of a pair arrives
/// with `repeat_click_count` 2, and each release carries its own press's count.
void doubleClick(ui::PerformPad& pad, float px, float py) {
	for (int count = 1; count <= 2; ++count) {
		visage::MouseEvent e;
		e.position = { px, py };
		e.repeat_click_count = count;
		pad.onMouseDown().callback(e);
		pad.onMouseUp().callback(e);
	}
}
} // namespace

TEST_CASE("backlog IV: a double-click anywhere on the perform pad centres the puck, as ONE undo step") {
	// Double-clicking the puck in Perform centres it. Through the real
	// panel, so the controls are the grid's own `DeviceControl`s and the history is the one they record
	// into — the road `gui.cpp` takes.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	p.armFrom();
	ui::PerformPad& pad = p.panel->padForTest();
	const feathers::ui::perform_pad::Disk d = pad.disk();
	REQUIRE(d.valid());
	ui::EditHistory& history = p.outerHistory;

	const auto axisOf = [&](sc::Param which) {
		return p.f.deviceHost.fieldAt(p.device, static_cast<size_t>(which));
	};
	// THE REST POINT FROM THE DECLARATION, not a literal 0 — the pad must read it the same way.
	const auto defOf = [](sc::Param which) {
		return static_cast<float>(sc::kDescriptor.params[static_cast<size_t>(which)].def);
	};
	const auto press = [&](float px, float py) {
		visage::MouseEvent e;
		e.position = { px, py };
		e.repeat_click_count = 1;
		pad.onMouseDown().callback(e);
		pad.onMouseUp().callback(e);
	};

	// OFF THE REST POINT ON BOTH AXES FIRST, or "it is at the centre" is where it started.
	press(d.cx + d.radius * 0.5f, d.cy - d.radius * 0.3f);
	REQUIRE(axisOf(sc::Param::PerfX) != defOf(sc::Param::PerfX));
	REQUIRE(axisOf(sc::Param::PerfY) != defOf(sc::Param::PerfY));

	SUBCASE("both axes land on the descriptor defaults, and the double-click is ONE step in all") {
		const float clickX = d.cx - d.radius * 0.4f;
		const float clickY = d.cy + d.radius * 0.6f;
		const size_t before = history.length();
		// WHERE THE PUCK WAS BEFORE THE DOUBLE-CLICK — what one undo must return to.
		const float preX = axisOf(sc::Param::PerfX);
		const float preY = axisOf(sc::Param::PerfY);
		// THE FIRST CLICK ALONE: what a click does today — the puck to the hand — as ONE step.
		press(clickX, clickY);
		REQUIRE(history.length() == before + 1);
		const float firstX = axisOf(sc::Param::PerfX);
		const float firstY = axisOf(sc::Param::PerfY);
		CHECK(firstX == doctest::Approx((clickX - d.cx) / d.radius).epsilon(1e-5));
		// THE SECOND PRESS OF THE PAIR, as visage numbers it.
		visage::MouseEvent second;
		second.position = { clickX, clickY };
		second.repeat_click_count = 2;
		pad.onMouseDown().callback(second);
		CHECK(axisOf(sc::Param::PerfX) == defOf(sc::Param::PerfX));
		CHECK(axisOf(sc::Param::PerfY) == defOf(sc::Param::PerfY));
		// FOLDED INTO THE FIRST CLICK'S STEP, not added after it.
		CHECK(history.length() == before + 1);
		// NO STRAY DRAG: a motion before the release must not carry the puck back off the rest point.
		visage::MouseEvent drag = second;
		drag.position = { d.cx + d.radius * 0.8f, d.cy };
		pad.onMouseDrag().callback(drag);
		pad.onMouseUp().callback(drag);
		CHECK(axisOf(sc::Param::PerfX) == defOf(sc::Param::PerfX));
		CHECK(axisOf(sc::Param::PerfY) == defOf(sc::Param::PerfY));
		CHECK(history.length() == before + 1);

		// ONE UNDO RETURNS TO BEFORE THE DOUBLE-CLICK — both axes, and not the first click's point.
		REQUIRE(preX != firstX);
		REQUIRE(preY != firstY);
		history.undo();
		CHECK(axisOf(sc::Param::PerfX) == preX);
		CHECK(axisOf(sc::Param::PerfY) == preY);
		history.redo();
		CHECK(axisOf(sc::Param::PerfX) == defOf(sc::Param::PerfX));
		CHECK(axisOf(sc::Param::PerfY) == defOf(sc::Param::PerfY));
	}

	SUBCASE("a double-click with something ELSE recorded in between makes its own step") {
		// The fold is only for the pad's own first click still on top; anything after it and the
		// centring must not be folded into someone else's entry.
		struct Other final : ui::EditRecord {
			void undo() const override {}
			void redo() const override {}
			const char* verb() const override { return "Other"; }
		};
		const float clickX = d.cx + d.radius * 0.2f;
		const float clickY = d.cy + d.radius * 0.4f;
		const size_t before = history.length();
		press(clickX, clickY);
		const float firstX = axisOf(sc::Param::PerfX);
		const float firstY = axisOf(sc::Param::PerfY);
		history.record(std::make_unique<Other>());
		REQUIRE(history.length() == before + 2);
		visage::MouseEvent second;
		second.position = { clickX, clickY };
		second.repeat_click_count = 2;
		pad.onMouseDown().callback(second);
		pad.onMouseUp().callback(second);
		CHECK(axisOf(sc::Param::PerfX) == defOf(sc::Param::PerfX));
		CHECK(axisOf(sc::Param::PerfY) == defOf(sc::Param::PerfY));
		CHECK(history.length() == before + 3);
		history.undo();
		CHECK(axisOf(sc::Param::PerfX) == firstX);
		CHECK(axisOf(sc::Param::PerfY) == firstY);
	}

	SUBCASE("a double-click whose FIRST click missed the disk does not fold into an earlier drag") {
		// The tip is the setup press's step; a first click in the corner moves nothing, so the
		// centring must be its own step and one undo must stop at the setup press's point.
		const float setX = axisOf(sc::Param::PerfX);
		const float setY = axisOf(sc::Param::PerfY);
		const size_t before = history.length();
		doubleClick(pad, 1.0f, 1.0f);
		CHECK(history.length() == before + 1);
		history.undo();
		CHECK(axisOf(sc::Param::PerfX) == setX);
		CHECK(axisOf(sc::Param::PerfY) == setY);
	}

	SUBCASE("an undo issued MID-DRAG is refused, and the drag is still one step") {
		const float x0 = axisOf(sc::Param::PerfX);
		const float y0 = axisOf(sc::Param::PerfY);
		const size_t before = history.length();
		REQUIRE(history.canUndo());
		visage::MouseEvent down;
		down.position = { d.cx - d.radius * 0.3f, d.cy - d.radius * 0.3f };
		down.repeat_click_count = 1;
		pad.onMouseDown().callback(down);
		visage::MouseEvent drag = down;
		drag.position = { d.cx - d.radius * 0.5f, d.cy - d.radius * 0.1f };
		pad.onMouseDrag().callback(drag);
		const float midX = axisOf(sc::Param::PerfX);
		const float midY = axisOf(sc::Param::PerfY);
		// THE HAND IS STILL DOWN: the stack does not move under an act in progress.
		CHECK_FALSE(history.canUndo());
		history.undo();
		CHECK(axisOf(sc::Param::PerfX) == midX);
		CHECK(axisOf(sc::Param::PerfY) == midY);
		CHECK(history.length() == before);
		pad.onMouseUp().callback(drag);
		CHECK(history.length() == before + 1);
		CHECK(history.canUndo());
		history.undo();
		CHECK(axisOf(sc::Param::PerfX) == x0);
		CHECK(axisOf(sc::Param::PerfY) == y0);
	}

	SUBCASE("a double-click OUTSIDE the grab reach, in the pad's corner, still centres") {
		// The pad's square corner is outside the disk and its slack — a single press there writes
		// nothing (the sibling case), so this is the "anywhere on the pad" half and not the puck's.
		const float cornerX = 1.0f;
		const float cornerY = 1.0f;
		REQUIRE_FALSE(feathers::ui::perform_pad::hits(d, cornerX, cornerY, pad.grabSlack()));
		const size_t before = history.length();
		doubleClick(pad, cornerX, cornerY);
		CHECK(axisOf(sc::Param::PerfX) == defOf(sc::Param::PerfX));
		CHECK(axisOf(sc::Param::PerfY) == defOf(sc::Param::PerfY));
		CHECK(history.length() == before + 1);
	}

	SUBCASE("a puck already at rest spends no step") {
		doubleClick(pad, 1.0f, 1.0f);
		const size_t before = history.length();
		doubleClick(pad, 1.0f, 1.0f);
		CHECK(history.length() == before);
	}

	SUBCASE("a drag of both axes is ONE step too — the group the double-click rides") {
		// It was two: each `DeviceControl::endGesture` records its own, and undo could take back X alone.
		const float x0 = axisOf(sc::Param::PerfX);
		const float y0 = axisOf(sc::Param::PerfY);
		const size_t before = history.length();
		visage::MouseEvent down;
		down.position = { d.cx - d.radius * 0.2f, d.cy + d.radius * 0.2f };
		down.repeat_click_count = 1;
		pad.onMouseDown().callback(down);
		visage::MouseEvent drag = down;
		drag.position = { d.cx - d.radius * 0.6f, d.cy + d.radius * 0.1f };
		pad.onMouseDrag().callback(drag);
		pad.onMouseUp().callback(drag);
		CHECK(history.length() == before + 1);
		history.undo();
		CHECK(axisOf(sc::Param::PerfX) == x0);
		CHECK(axisOf(sc::Param::PerfY) == y0);
	}
}
TEST_CASE("backlog IV: the pad's centring is one gesture on EACH control, and only the base moves") {
	// THE BRACKET COUNTED AT THE CONTROL, which the panel case cannot see: a `DeviceControl` gesture
	// reaches no bridge, so the Modulatable is where "exactly one begin/end" is a fact. A spy over the
	// real descriptor rows, so the defaults are still the declared ones.
	namespace sc = feathers::fx::sklitter;
	struct Spy final : feathers::device::Modulatable {
		const feathers::device::ParamDesc& d;
		double v = 0.0;
		int begins = 0, ends = 0, writes = 0, amounts = 0;
		explicit Spy(const feathers::device::ParamDesc& desc) : d(desc) {}
		const feathers::device::ParamDesc& desc() const override { return d; }
		double value() const override { return v; }
		void setValue(double value) override {
			++writes;
			v = value;
		}
		feathers::device::ModDisplay display() const override { return {}; }
		void setAmount(float) override { ++amounts; }
		void beginGesture() override { ++begins; }
		void endGesture() override { ++ends; }
		std::string text() const override { return {}; }
	};
	Spy x { sc::kDescriptor.params[static_cast<size_t>(sc::Param::PerfX)] };
	Spy y { sc::kDescriptor.params[static_cast<size_t>(sc::Param::PerfY)] };
	x.v = 0.5;
	y.v = -0.25;
	ui::PerformPad pad { x, y };
	pad.setBounds(0, 0, 200, 200);
	const feathers::ui::perform_pad::Disk d = pad.disk();
	REQUIRE(d.valid());

	// The first press of the pair, on its own: one bracket, the puck to the hand.
	visage::MouseEvent first;
	first.position = { d.cx + d.radius * 0.3f, d.cy };
	first.repeat_click_count = 1;
	pad.onMouseDown().callback(first);
	pad.onMouseUp().callback(first);
	REQUIRE(x.begins == 1);
	REQUIRE(x.ends == 1);
	const int writesBefore = x.writes;

	visage::MouseEvent second = first;
	second.repeat_click_count = 2;
	pad.onMouseDown().callback(second);
	pad.onMouseUp().callback(second);
	CHECK(x.begins == 2);
	CHECK(x.ends == 2);
	CHECK(y.begins == 2);
	CHECK(y.ends == 2);
	CHECK(x.writes == writesBefore + 1);
	CHECK(x.v == x.d.def);
	CHECK(y.v == y.d.def);
	// THE BASE ONLY: nothing about a route was touched.
	CHECK(x.amounts == 0);
	CHECK(y.amounts == 0);
}
TEST_CASE("the puck the pad draws is the one the PARAMETERS say — derived, never stored") {
	// **`perform_stage.h`'s FIRST RULE, and design §5 restates it because this surface is where it
	// bites:** the puck is `Perf X` and `Perf Y` read at the moment it is drawn, so there is no second
	// copy of the position to fall out of step. The way to assert that is to move the parameters
	// BEHIND the pad's back — no gesture at all — and read the point it would draw.
	namespace sc = feathers::fx::sklitter;
	namespace pp = feathers::ui::perform_pad;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::PerformPad& pad = p.panel->padForTest();
	const pp::Disk d = pad.disk();
	REQUIRE(d.valid());

	const auto setPuck = [&](float x, float y) {
		p.f.bridge.setDeviceControl(p.device, static_cast<size_t>(sc::Param::PerfX), x);
		p.f.bridge.setDeviceControl(p.device, static_cast<size_t>(sc::Param::PerfY), y);
	};

	setPuck(0.5f, -0.25f);
	auto [px, py] = pad.puckPointForTest();
	CHECK(px == doctest::Approx(d.cx + 0.5f * d.radius).epsilon(1e-5));
	CHECK(py == doctest::Approx(d.cy - 0.25f * d.radius).epsilon(1e-5));

	// **AND THE WEIGHTS THE MARKS ARE DRAWN AT ARE THE ENGINE'S OWN** — `performWeightsOf`, not a second
	// computation (`perform.h`'s contract).
	//
	// **THE SHARES ARE NO LONGER THE SCREEN AXES, WHICH IS THE 45° ROTATION (2026-09-19).** They were:
	// at (0.5, −0.25) this case used to read E = 0.5, N = 0.25, centre = 0.25 straight off the pair.
	// The waypoints are on the diagonals now, so the shares are the ROTATED axes `u = x − y` and
	// `v = x + y` over their total — restated here by hand, so a pad that went on drawing the screen
	// axes would fail rather than agree.
	const float root2 = std::sqrt(2.0f);
	const float u = 0.5f - -0.25f; // 0.75 — this puck's share of waypoint 2's axis
	const float v = 0.5f + -0.25f; // 0.25 — and of waypoint 3's
	const fx::sklitter::PerformWeights w = pad.weightsForTest();
	CHECK(w.corner[sc::kCfg2] == doctest::Approx(u / root2).epsilon(1e-5));
	CHECK(w.corner[sc::kCfg3] == doctest::Approx(v / root2).epsilon(1e-5));
	CHECK(w.corner[sc::kCfg1] == 0.0f); // x + y is positive, so nothing is spent on the top-left
	CHECK(w.corner[sc::kCfg4] == 0.0f); // x − y is positive, so nothing on the bottom-left
	CHECK(w.centre == doctest::Approx((root2 - (u + v)) / root2).epsilon(1e-5));
	// NOT VACUOUS, and it is the line that tells the rotation from the arrangement it replaced: under
	// the cardinal weights this puck spent a quarter on N and nothing on S, which is exactly backwards.
	CHECK(w.corner[sc::kCfg3] > 0.1f);

	// **THE NaN CASE IS NOT ASSERTED HERE, AND THE REASON IS A MEASUREMENT RATHER THAN A SHRUG.** The
	// pad reads through `performWeightsOf`, which de-NaNs, so a broken source would draw as centred —
	// which is what it SOUNDS like. **A NaN CANNOT REACH THE PAD BY THIS ROAD**: `ParamBridge::
	// setDeviceControl` returns early on `!std::isfinite(value)`, so the write below is DROPPED and the
	// puck stays where the previous line put it. MEASURED — a first draft of this case asserted the
	// centre here and read 96 against 66, the pad still drawing the (0.5, −0.25) above.
	//
	// So the guarantee lives where it can be asked: `performWeightsOf`'s de-NaN is pinned by value in
	// `test_fx_sklitter_perform.cpp` ("a non-finite puck axis reads as the centre…"), and what this
	// surface adds is only that it goes through that function — which the weights assertions above are.
	// Asserting it here anyway would have been a case that passes because nothing happened.
	// **AND THE DROP IS PER FIELD, WHICH THE SECOND DRAFT OF THIS ALSO GOT WRONG**: the NaN X is refused
	// and the finite Y beside it lands, so the puck ends at (0.5, 0) rather than unmoved. Both halves
	// asserted, because "one write of two was refused" is the fact and either half alone reads as the
	// other kind of failure.
	setPuck(std::numeric_limits<float>::quiet_NaN(), 0.0f);
	auto [nx, ny] = pad.puckPointForTest();
	CHECK(nx == doctest::Approx(px).epsilon(1e-5)); // X unmoved: the bridge refused that write
	CHECK(ny == doctest::Approx(d.cy).epsilon(1e-5)); // Y landed: 0 is finite

	// **AND A PUCK PAST THE RIM IS DRAWN PAST THE RIM.** `Perf X`/`Perf Y` are modulation destinations
	// clamped to the SQUARE, so (1, 1) is a state the engine can really be in and lies outside the disk
	// by √2. Drawing it on the rim would be the surface telling a story about a value it did not have.
	setPuck(1.0f, 1.0f);
	auto [fx_, fy] = pad.puckPointForTest();
	CHECK(std::hypot(fx_ - d.cx, fy - d.cy) == doctest::Approx(d.radius * std::sqrt(2.0f)).epsilon(1e-4));
}
TEST_CASE("a waypoint's MARK is drawn where a press would play that waypoint whole") {
	// **THE PICTURE AND THE SOUND, JOINED — and nothing joined them until the waypoints moved.**
	//
	// `waypointPoint` decides where a mark is drawn and `performWeightsOf` decides what a press means.
	// They are two readings of one rotation, and before 2026-09-19 the pair was so obvious (a cardinal
	// rim point either way) that no case ever asked. The 45° rotation is exactly the kind of change that
	// can reach one and not the other — and a mark drawn where its waypoint is NOT is a surface lying
	// about the instrument, which no weight assertion and no screenshot would catch.
	//
	// **THE ROUND TRIP IS THROUGH PIXELS, which is what stops this being `waypointAxis` compared with
	// itself.** The mark's position is taken in the pad's own coordinates, handed back through `axesAt`
	// — the same function a real press goes through, clamp and all — and the weights are read from the
	// engine. What is asserted is the end of that road, not a shared constant.
	namespace sc = feathers::fx::sklitter;
	namespace pp = feathers::ui::perform_pad;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	const pp::Disk d = p.panel->padForTest().disk();
	REQUIRE(d.valid());

	for (size_t i = 0; i < sc::kCorners; ++i) {
		CAPTURE(i);
		const auto c = static_cast<sc::Corner>(i);
		const auto [mx, my] = pp::waypointPoint(d, c);
		// A PRESS ON THE MARK IS THAT WAYPOINT, WHOLE. The pixel round trip costs a little precision,
		// so this is the one place the rim identity is `Approx` rather than `==`; the bit-exact form is
		// asserted on the parameters themselves in `test_fx_sklitter_perform.cpp`.
		const auto [ax, ay] = pp::axesAt(d, mx, my);
		const sc::PerformWeights w = sc::performWeightsOf(ax, ay);
		CHECK(w.corner[c] == doctest::Approx(1.0f).epsilon(1e-4));
		CHECK(w.centre == doctest::Approx(0.0f).epsilon(1e-4));
		for (size_t other = 0; other < sc::kCorners; ++other) {
			if (other != i) {
				CAPTURE(other);
				CHECK(w.corner[other] == doctest::Approx(0.0f).epsilon(1e-4));
			}
		}
		// **AND IT IS ON A DIAGONAL, said as a number** — the two offsets are equal in size and the mark
		// is on the rim. A rotation of 30° or 60° would satisfy every weight line above (they only ask
		// that the axes agree with themselves) and would fail this.
		CHECK(std::fabs(mx - d.cx) == doctest::Approx(std::fabs(my - d.cy)).epsilon(1e-4));
		CHECK(std::hypot(mx - d.cx, my - d.cy) == doctest::Approx(d.radius).epsilon(1e-4));
	}
	// NOT VACUOUS: the four marks are four DIFFERENT points, so the loop is not one point asserted four
	// times — which is what a `waypointPoint` that ignored its argument would look like.
	std::set<std::pair<int, int>> seen;
	for (size_t i = 0; i < sc::kCorners; ++i) {
		const auto [mx, my] = pp::waypointPoint(d, static_cast<sc::Corner>(i));
		seen.insert({ static_cast<int>(std::lround(mx)), static_cast<int>(std::lround(my)) });
	}
	CHECK(seen.size() == sc::kCorners);
}
TEST_CASE("a corner cell is SQUARE, captionless, and big enough for both of the rows it keeps") {
	// **THREE CHANGES, 2026-09-19:** the N/E/S/W Cfg labels are gone as clutter, the phase/dropdown
	// boxes are square with two rows, and they sit at the corners of the circle to stack more
	// efficiently. The corners are the case below; this one is the cell.
	//
	// **AND THE HALF THAT NEEDED A CASE RATHER THAN A CAMERA: the side is a CONSTANT over two FONT
	// MEASUREMENTS.** `rail.h` forbids `expandedWidth()` measuring anything, so `CircleRail::kCellSide`
	// cannot be written as the arithmetic it wants to be — it is stated, and this is what stops it
	// drifting away from the two strings it has to hold. A square one pixel too small crops the widest
	// region name or the widest coordinate, and `Canvas::text` says nothing about what did not fit.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	const auto& cells = p.panel->cornerCellsForTest();

	for (size_t i = 0; i < sc::kCorners; ++i) {
		CAPTURE(i);
		REQUIRE(cells[i] != nullptr);
		// **SQUARE, AND SQUARE AT THE STATED SIDE** — not merely "as wide as it is tall", which a cell
		// laid out at 0x0 also satisfies.
		CHECK(static_cast<float>(cells[i]->width()) == doctest::Approx(ui::CircleRail::kCellSide));
		CHECK(static_cast<float>(cells[i]->height()) == doctest::Approx(ui::CircleRail::kCellSide));
		// **THE ARRANGEMENT IS WHAT REMOVES THE CAPTION.** `ZoneCoordCell::draw` returns after the
		// plate in `Square`, and `resized` reserves no caption line — one fact, asserted where it is a
		// value. That the pixels then carry no word is checked by the capture, because a drawing is
		// the only thing that can say a drawing did not happen.
		CHECK(cells[i]->layout() == ui::ZoneCoordCell::Layout::Square);
		// **BIG ENOUGH FOR BOTH ROWS, measured against the real font and the real name list.**
		CHECK(ui::CircleRail::kCellSide >= cells[i]->minimumSquareSide());
		// **AND THE AIR THE SIDE WAS CHOSEN FOR.** `>=` alone would pass at exactly the requirement,
		// where the widest region name touches both chevrons — green, and wrong to look at. MEASURED
		// 2026-09-19: the requirement is 66 and the side is 72. The floor is 4 rather than 6 so that a
		// font change costing a pixel or two is a warning in the capture and not a red here; a change
		// that eats the margin whole is what this is for.
		CHECK(ui::CircleRail::kCellSide - cells[i]->minimumSquareSide() >= 4.0f);
		// NOT VACUOUS: the requirement is a real number rather than a zero that everything clears.
		CHECK(cells[i]->minimumSquareSide() > 2.0f * ui::ZoneCoordCell::kPlateInsetX);
		// AND THE TWO ROWS ARE INSIDE THE SQUARE, in order, not overlapping.
		// **THE SECOND ROW IS A SIGIL SINCE backlog IW.** Cfg's zones were an intensity ladder over a continuum, not
		// modes, so the cell is Free-only: the dropdown is hidden and a mark for the lap takes its row.
		const ui::ValueBox& box = cells[i]->box();
		CHECK_FALSE(cells[i]->select().isVisible());
		REQUIRE(cells[i]->sigil() != nullptr);
		const ui::Sigil& sigil = *cells[i]->sigil();
		CHECK(box.y() >= 0);
		CHECK(box.y() + box.height() <= sigil.y());
		CHECK(sigil.y() + sigil.height() <= cells[i]->height());
		CHECK(box.x() >= 0);
		CHECK(box.x() + box.width() <= cells[i]->width());
		// AND THE SIGIL HAS ITS FULL SIDE — a row too short draws a cropped mark.
		CHECK(static_cast<float>(sigil.width()) >= ui::Sigil::kSide);
		CHECK(static_cast<float>(sigil.height()) >= ui::Sigil::kSide);
	}
}
TEST_CASE("the four Cfg cells sit at the CORNERS around the disk, and closing the section puts them back on the grid") {
	// The Macro Cfg controls go in their respective corners — the corners of the circle, where they
	// stack more efficiently. So the four are no longer ON the cardinal axes — each is at a CORNER of the block,
	// in the corner that shares its own cardinal, running clockwise from the top-left in the compass's
	// own order N → E → S → W. Asserted against the pad's OWN disk, so a panel that moved the circle
	// moves the expectation with it.
	//
	// **WHAT THIS CASE CAN AND CANNOT SAY.** A corner touches two cardinals, so position no longer
	// NAMES a waypoint the way it did — that claim left with the captions and `CircleRail::resized`
	// says so. What is still checkable, and is what is checked: each cell is on the correct side in
	// BOTH axes, the four are in four different corners, and none of them is drawn over the disk.
	//
	// **AND THE HALF THAT MATTERS MORE: SHUT, THEY ARE BACK ON THE GRID.** A collapsed section that made
	// four controls unreachable would be the honesty rule failing on a feature, and nothing about a
	// closed section would say so.
	namespace sc = feathers::fx::sklitter;
	namespace pp = feathers::ui::perform_pad;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	REQUIRE_FALSE(p.panel->circleRailForTest().collapsed()); // it opens by default: the circle IS the macro now

	const pp::Disk d = p.panel->padForTest().disk();
	const auto& cells = p.panel->cornerCellsForTest();
	for (size_t i = 0; i < sc::kCorners; ++i) {
		CAPTURE(i);
		REQUIRE(cells[i] != nullptr);
		REQUIRE(cells[i]->isVisible());
	}
	// THE PAD'S DISK IS IN THE PAD'S SPACE; the cells are in the panel's, so the disk's centre is
	// offset by where the pad was put.
	const float cx = static_cast<float>(p.panel->padForTest().x()) + d.cx;
	const float cy = static_cast<float>(p.panel->padForTest().y()) + d.cy;
	const auto centreOf = [](const ui::ZoneCoordCell* c) {
		return std::pair { static_cast<float>(c->x()) + static_cast<float>(c->width()) * 0.5f,
			               static_cast<float>(c->y()) + static_cast<float>(c->height()) * 0.5f };
	};
	const auto [nx, ny] = centreOf(cells[sc::kCfg1]);
	const auto [ex, ey] = centreOf(cells[sc::kCfg2]);
	const auto [sx, sy] = centreOf(cells[sc::kCfg3]);
	const auto [wx, wy] = centreOf(cells[sc::kCfg4]);
	// **EACH CELL IS AT ITS OWN WAYPOINT'S CORNER — AND THE WAYPOINT IS ASKED, NOT RESTATED.**
	//
	// The first draft of the corner layout moved the cells and left the waypoints on the cardinals, so a
	// corner touched TWO of them and the pairing was a convention in a comment. The point is that
	// the new cardinals are rotated 45 degrees to match the box locations. The waypoints
	// rotated to meet the cells, and **this is the one place the two declarations meet**: a rotation
	// applied to `waypointAxis` and not to `CircleRail::resized`, or the other way round, is a red here
	// rather than a picture nobody checks.
	int quadrants = 0;
	for (size_t i = 0; i < sc::kCorners; ++i) {
		CAPTURE(i);
		const auto [mx, my] = pp::waypointPoint(d, static_cast<sc::Corner>(i));
		const float markX = static_cast<float>(p.panel->padForTest().x()) + mx;
		const float markY = static_cast<float>(p.panel->padForTest().y()) + my;
		const auto [cellX, cellY] = centreOf(cells[i]);
		// SAME SIDE IN BOTH AXES: the mark is up-and-left of the middle exactly when its cell is.
		CHECK((markX < cx) == (cellX < cx));
		CHECK((markY < cy) == (cellY < cy));
		// AND THE CELL IS FURTHER OUT THAN THE MARK — "beside the waypoint", outside the ring it is on.
		CHECK(std::hypot(cellX - cx, cellY - cy) > std::hypot(markX - cx, markY - cy));
		quadrants |= 1 << ((markX < cx ? 1 : 0) | (markY < cy ? 2 : 0));
	}
	// **NOT VACUOUS: the four marks are in four DIFFERENT quadrants.** On the cardinals each mark sat on
	// an axis, where "same side" is a coin toss that half the layouts would win; the rotation is what
	// makes this a real constraint, and this line is what says the rotation happened.
	CHECK(quadrants == 0b1111);
	// **AND THEY ARE IN FOUR DIFFERENT CORNERS**, which the eight lines above do not say on their own:
	// the two top cells share a row and the two left cells share a column, so a layout that stacked a
	// pair would still satisfy every inequality.
	CHECK(ny == doctest::Approx(ey).epsilon(1e-3));
	CHECK(sy == doctest::Approx(wy).epsilon(1e-3));
	CHECK(nx == doctest::Approx(wx).epsilon(1e-3));
	CHECK(ex == doctest::Approx(sx).epsilon(1e-3));
	// **AND THEY ARE AT THE BLOCK'S CORNERS RATHER THAN SHOULDER TO SHOULDER IN THE MIDDLE OF IT.**
	// Two centres are `2·kCornerOffset + kCellSide` apart — 300 — which is the corner geometry restated
	// from the offset rather than the layout's own expression repeated. It was
	// `kCellSide + kCellGap` (80) while the rail was 172 wide and the four squares were two touching
	// pairs; the spread is what the extra 220px bought and this is the line that says it arrived.
	CHECK(ex - nx
	      == doctest::Approx(2.0f * ui::CircleRail::kCornerOffset + ui::CircleRail::kCellSide));

	// **AND NONE OF THEM IS DRAWN OVER THE DISK — at its NEAREST CORNER, not at its centre, and against
	// the radius the pad GRABS at rather than the one it draws at.**
	//
	// Two sharpenings, both of which the move to the corners made necessary. The cardinal arrangement
	// could be checked at the cell's midpoint because a cell sat square-on to the disk; a square at a
	// corner presents a CORNER to the circle, and a midpoint test would pass a cell whose corner was
	// inside the ring. And the ring is drawn at `d.radius` while `PerformPad::onMouseDown` takes a
	// press out to `d.radius + kGrabSlack` — a cell overlapping THAT is a control sharing pixels with
	// the gesture behind it, which is worse than one merely drawn over a line.
	//
	// **THE PAD'S OWN SLACK, NOT THE 1:1 CONSTANT (2026-09-20).** `kGrabSlack` is what the pad grabs at
	// when it is drawn at `kReferenceDiameter`; since the circle scales with the panel it is
	// `grabSlack()` that `onMouseDown` passes, and they are the same number here only because this
	// fixture holds the panel at its own minimum. Reading the constant would be a case that stayed
	// green at one size and said nothing about any other.
	const float grabbed = d.radius + p.panel->padForTest().grabSlack();
	// **AND THE AIR IS REAL AIR, WHICH THE PAIR OF LINES BELOW CANNOT SAY.** `kDisk` is DERIVED from
	// `kCornerClear` (`kCornerOffset·√2 − kCornerClear`, doubled), so the two sides of the equality
	// below MOVE TOGETHER: a clearance of zero makes the disk exactly big enough to touch, and the
	// assertion is then a tautology. **MEASURED, not reasoned about — a known-bad setting
	// `kCornerClear` to 0 ran all 149 cases GREEN.** So the margin itself is asserted here, which is
	// `kCellSide`'s own line one case up arriving for the second constant in this geometry.
	CHECK(ui::CircleRail::kCornerClear >= 4.0f);
	for (size_t i = 0; i < sc::kCorners; ++i) {
		CAPTURE(i);
		const auto* c = cells[i];
		// The point of the cell's rectangle nearest the centre: its facing edge, or its facing corner.
		const float nearX = std::clamp(cx, static_cast<float>(c->x()),
		                               static_cast<float>(c->x() + c->width()));
		const float nearY = std::clamp(cy, static_cast<float>(c->y()),
		                               static_cast<float>(c->y() + c->height()));
		CHECK(std::hypot(nearX - cx, nearY - cy) >= grabbed);
		// **AND IT CLEARS IT BY THE STATED AIR, WHICH IS A VALUE RATHER THAN AN INEQUALITY.** `>=`
		// passes at tangency, where the corner touches the gesture and the picture is wrong — the
		// failure `kCellSide`'s own margin exists to stop, one constant over. `kDisk` is DERIVED from
		// this gap (`kCornerOffset·√2 − kCornerClear`, doubled), so a disk that grew without the cells
		// moving, or cells that moved without the disk shrinking, is a red here and nowhere else.
		CHECK(std::hypot(nearX - cx, nearY - cy)
		      == doctest::Approx(grabbed + ui::CircleRail::kCornerClear));
	}
	// NOT VACUOUS: the disk is a real disk and the cells are really beside it rather than everything
	// being laid out at zero.
	REQUIRE(d.radius > 0.0f);
	REQUIRE(cells[sc::kCfg1]->width() > 0);

	// **WHILE OPEN, THE GRID DOES NOT ALSO SEAT THEM** — one control, one visible widget.
	const auto gridVisible = [&](sc::Param which) {
		for (size_t n = 0; n < p.panel->cellsForTest().size(); ++n) {
			if (p.panel->paramIndicesForTest()[n] == static_cast<size_t>(which)) {
				return p.panel->cellsForTest()[n]->isVisible();
			}
		}
		FAIL("the grid never built a cell for it");
		return false;
	};
	CHECK_FALSE(gridVisible(sc::Param::Cfg1));
	CHECK_FALSE(gridVisible(sc::Param::Cfg4));
	// NOT VACUOUS: a control that never left the grid is still there, so "not visible" is the section
	// taking it rather than the whole grid being hidden.
	CHECK(gridVisible(sc::Param::PerfX));
	CHECK(gridVisible(sc::Param::PerfDepth));

	SUBCASE("folded, the corner cells go back to the grid and the pad goes away") {
		setRails(p, false, false);
		CHECK_FALSE(p.panel->padForTest().isVisible());
		for (size_t i = 0; i < sc::kCorners; ++i) {
			CAPTURE(i);
			CHECK_FALSE(cells[i]->isVisible());
		}
		// THE FOUR ARE REACHABLE AGAIN, which is the whole claim.
		CHECK(gridVisible(sc::Param::Cfg1));
		CHECK(gridVisible(sc::Param::Cfg2));
		CHECK(gridVisible(sc::Param::Cfg3));
		CHECK(gridVisible(sc::Param::Cfg4));
	}

	SUBCASE("the circle open takes four cells off the grid") {
		setRails(p, true, false);
		const size_t liveOpen = p.panel->liveCells();
		setRails(p, false, false);
		const size_t liveShut = p.panel->liveCells();
		CHECK(liveShut == liveOpen + sc::kCorners);
	}
}
TEST_CASE("THE WINDOW GROWS when a rail opens and gives the width back when it closes, through the real overlay") {
	// **THE DECISION, 2026-09-19, chosen over the alternative it replaces.** SKLITTER's two expansion
	// sections were fold-down trays stacked above the grid, and the grid gave up HEIGHT for them by
	// shrinking every knob. They now work exactly like the fx lane expansion in Feathers — a rail
	// against the edge that expands sideways — and the panel gets wider rather than the
	// controls getting smaller. The compression seam (`GenericExpanded::cellFaceDiameter`) is gone with
	// the case that used to assert it.
	//
	// **AND THE RISK THIS CASE EXISTS FOR IS NOT THE ARITHMETIC — IT IS WHETHER ANYTHING ASKS.**
	// `ExpandedOverlay::panelSizeOf` reads `minimumSize()` live on every layout pass, which makes the
	// growth POSSIBLE and does not make it HAPPEN: something has to run a pass, and nothing did. The
	// camera was working around it by hand — `widget-shot` called `overlay->resized()` after setting a
	// section, *"otherwise a tray that just opened is drawn inside the old rectangle and clipped" —
	// which is a workaround in the instrument for a road the editor did not have. So this drives the
	// whole road through a REAL editor and a REAL overlay: chevron state → rail → panel →
	// `MountedFrame` → host → rectangle.
	//
	// **AND THE ONE JOINT THAT WAS MISSING WAS THE MOUNT, which no amount of reading found.** Every
	// panel in either host is presented through a `MountedFrame`, which defers to its content for
	// `minimumSize()` and so looks transparent — the host wires the announcement on the MOUNT and the
	// panel sets it on ITSELF, and the two were never joined. With the fix in the panel and in the
	// overlay and nothing in the mount, the panel's rectangle did not move by one pixel.
	//
	// **THE CLAIM IS A WIDTH, WHICH IS A VALUE.** CLAUDE.md: a comparison between two renders is
	// evidence that something changed, never that the right thing did.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true); // the floating panel is the overlay's
	lane->onExpandDevice(slot);
	REQUIRE(overlay->open());

	auto* panel = findInTree<ui::SklitterExpanded>(f.root);
	REQUIRE(panel != nullptr);
	ui::CircleRail& circle = panel->circleRailForTest();
	ui::FxLaneRail& fx = panel->postRailForTest(); // one of the two; the claim is per-rail
	// THE STATE IT OPENS IN: the circle IS this device's gesture, the placeholder is a promise.
	REQUIRE_FALSE(circle.collapsed());
	REQUIRE(fx.collapsed());

	/// **THE RECTANGLE THE HOST APPLIED, NOT THE ONE IT WOULD COMPUTE IF ASKED.**
	///
	/// `ExpandedOverlay::panelBounds()` goes through `panelSizeOf`, which reads `minimumSize()` LIVE —
	/// so it answers "how big should this panel be" and never "how big is it". MEASURED: a first draft
	/// of this case asserted on `panelBounds().width()` and stayed GREEN with the announcement line
	/// deleted, which is precisely the mechanism it exists to prove — the measurement answered a
	/// question nobody asked it. The frames' own widths are what a layout pass actually wrote, so
	/// nothing but a pass can move them.
	///
	/// **DO NOT SIMPLIFY THIS BACK TO `overlay->panelBounds()`.** It is the shorter, more obvious call
	/// and it is the vacuous one; the two agree on every build where the mechanism works, which is
	/// exactly why swapping them looks safe. The red that proves the difference is `0 == Approx(266)`.
	const auto appliedPanelWidth = [&] {
		REQUIRE(overlay->panelForTest() != nullptr);
		return static_cast<float>(overlay->panelForTest()->width());
	};
	const auto appliedContentWidth = [&] { return static_cast<float>(panel->width()); };
	/// The width a rail's own declaration says it takes when it opens — restated from the RAIL rather
	/// than from the panel's sum, so this is an independent reference and not the implementation
	/// repeated.
	const auto swing = [](float expanded) { return expanded - ui::Rail::kCollapsedWidth; };

	const float circleOnly = appliedPanelWidth();
	const float circleOnlyContent = appliedContentWidth();

	SUBCASE("opening the placeholder widens the panel by exactly what that rail wants") {
		REQUIRE(fx.setCollapsed(false));
		const float bothOpen = appliedPanelWidth();
		// **NOT MEASURING THE CLAMP.** `panelSizeOf` bounds a panel by the layer it lives in, so a
		// case run in an editor too narrow for both rails would be reading the clamp and would pass
		// while nothing grew at all.
		REQUIRE(bothOpen < static_cast<float>(overlay->width()));
		CHECK(bothOpen - circleOnly == doctest::Approx(swing(ui::FxLaneRail::kWidth)));
		// AND THE CONTENT GOT THE ROOM, not just the chrome around it. The two are set in the same
		// pass, so this is the second reading that says the pass happened rather than a new claim.
		CHECK(appliedContentWidth() - circleOnlyContent
		      == doctest::Approx(swing(ui::FxLaneRail::kWidth)));
		CHECK(static_cast<float>(fx.width()) == doctest::Approx(ui::FxLaneRail::kWidth));

		// **AND IT COMES BACK.** A panel that only ever grew would keep the room a closed rail is not
		// using, which is the half a one-direction case would miss.
		REQUIRE(fx.setCollapsed(true));
		CHECK(appliedPanelWidth() == doctest::Approx(circleOnly));
		CHECK(appliedContentWidth() == doctest::Approx(circleOnlyContent));
		CHECK(static_cast<float>(fx.width()) == doctest::Approx(ui::Rail::kCollapsedWidth));
	}

	SUBCASE("and closing the circle gives back its own width") {
		// **THE EXACT DELTA HOLDS ONLY WHILE THE GRID STAYS FOUR COLUMNS WIDE**, because closing the
		// circle hands its four `Cfg` cells back to the grid — the grid is `kPerRow` wide either way
		// here, and this REQUIRE is what says so rather than leaving it to luck.
		REQUIRE(panel->liveCells() >= 4u);
		REQUIRE(circle.setCollapsed(true));
		REQUIRE(panel->liveCells() >= 4u);
		CHECK(circleOnly - appliedPanelWidth() == doctest::Approx(swing(ui::CircleRail::kWidth)));
		REQUIRE(circle.setCollapsed(false));
		CHECK(appliedPanelWidth() == doctest::Approx(circleOnly));
	}

	SUBCASE("and the grid does NOT compress in any of the four states") {
		// The other half of the decision, and the one a width assertion cannot see. `P Write` is an
		// ordinary continuous knob, live in every looper mode and on the grid whatever the rails do.
		const auto gridKnobFace = [&]() {
			for (size_t n = 0; n < panel->cellsForTest().size(); ++n) {
				if (panel->paramIndicesForTest()[n] != static_cast<size_t>(sc::Param::PWrite)) {
					continue;
				}
				auto* knob = dynamic_cast<ui::Knob*>(panel->cellsForTest()[n]);
				REQUIRE(knob != nullptr);
				REQUIRE(knob->isVisible());
				return knob->faceDiameter();
			}
			FAIL("the grid has no cell for P Write");
			return 0.0f;
		};
		for (const bool wantCircle : { false, true }) {
			for (const bool wantFx : { false, true }) {
				CAPTURE(wantCircle);
				CAPTURE(wantFx);
				circle.setCollapsed(!wantCircle);
				fx.setCollapsed(!wantFx);
				CHECK(gridKnobFace() == ui::Knob::kDiameter);
				CHECK(static_cast<float>(panel->cellHeightForTest())
				      == doctest::Approx(ui::Knob::columnHeight(ui::Knob::kDiameter)));
			}
		}
	}
}

namespace {
/// The tag of the first device the chain will actually take, so a case names a real one without
/// hard-coding which. A chain refusing every tag would make every add case vacuously green.
const char* firstAddableTag(feathers::ui::ChainColumn& column) {
	const std::vector<const device::Descriptor*> offered = column.addable();
	return offered.empty() ? nullptr : offered.front()->traits.tag;
}
} // namespace

TEST_CASE("SKLITTER's post chain reaches its editor: adopt builds the bridge, and postChainOf is the cast") {
	// **THE BINDING, ASSERTED AS VALUES RATHER THAN INFERRED FROM A DRAWING.** `rack::bridgeOf` is the
	// rack device's cast from the host's opaque handle to its bridge; `fx::sklitter::postChainOf` is
	// SKLITTER's, and this is the case that says the two ends meet.
	//
	// **AND THAT `adopt` IS WHAT BUILDS THE BRIDGE, which is trap T5 and is invisible otherwise**: a
	// container whose bridge was built at construct holds an empty catalogue and offers no devices,
	// with no error anywhere.
	feathers::fx::sklitter::Sklitter device;
	REQUIRE(device.postChain() != nullptr);
	CHECK(device.postChain()->bridge() == nullptr); // before adopt: nothing to edit through

	CHECK(feathers::fx::sklitter::postChainOf(static_cast<void*>(nullptr)) == nullptr);
	CHECK(feathers::fx::sklitter::postChainOf(&device) == device.postChain());

	device.postChain()->adopt(feathers::hostCatalogue());
	REQUIRE(device.postChain()->bridge() != nullptr);
	// THE CHAIN IS EMPTY AND ITS CAPACITY IS REAL — the two numbers the column's (+) is drawn from.
	CHECK(device.postChain()->bridge()->chainLength(0) == 0);
	CHECK(device.postChain()->bridge()->capacity() > 0);
}
TEST_CASE("a chain column adds, removes and reorders devices in a holder's inner chain") {
	// **EVERY CLAIM HERE IS A VALUE READ OFF THE BRIDGE**, never a comparison of two drawings: CLAUDE.md's
	// rule, paid for four times. A render that differs is evidence that something changed, never that
	// the right thing did — and a chain editor is exactly where a partial effect would survive it (the
	// card appears, the engine never hears about it).
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);

	// **WHAT IT OFFERS IS WHAT THE CHAIN CAN RUN**, asked of the chain's own catalogue rather than of
	// the build's device table — a menu row the chain would refuse is a control that does nothing.
	const std::vector<const device::Descriptor*> offered = column.addable();
	REQUIRE_FALSE(offered.empty());
	for (const device::Descriptor* d : offered) {
		CHECK(held.bridge().descriptorFor(d->traits.tag) != nullptr);
	}
	// AND MODULATORS ARE AMONG THEM, which is EW's whole "except": the rack sorts by CAPABILITY, so a
	// chain takes modulators and effects together and a filter-only menu would have thrown that away.
	const bool anyMod = std::any_of(offered.begin(), offered.end(),
	                                [](const device::Descriptor* d) { return d->mod != nullptr; });
	const bool anyAudio = std::any_of(offered.begin(), offered.end(),
	                                  [](const device::Descriptor* d) { return d->audio != nullptr; });
	CHECK(anyMod);
	CHECK(anyAudio);

	// ADD ONE: the CHAIN grew, and the card followed it.
	const DeviceInstanceId first = column.addChoiceForTest(0);
	REQUIRE(first != feathers::kNoInstance);
	CHECK(held.bridge().chainLength(0) == 1);
	CHECK(held.bridge().deviceAtPosition(0) == first);
	CHECK(held.bridge().descriptorOf(first) == offered[0]);
	CHECK(column.cardCountForTest() == 1);
	CHECK(column.cardOf(first) != nullptr);

	// ADD A SECOND, and it lands AFTER the first — the (+) is a picture of an append.
	const size_t second_choice = offered.size() > 1 ? 1 : 0;
	const DeviceInstanceId second = column.addChoiceForTest(second_choice);
	REQUIRE(second != feathers::kNoInstance);
	CHECK(held.bridge().chainLength(0) == 2);
	CHECK(held.bridge().deviceAtPosition(0) == first);
	CHECK(held.bridge().deviceAtPosition(1) == second);
	CHECK(column.cardCountForTest() == 2);

	// REORDER: the ORDER moved and no card was rebuilt, which is what makes a reorder safe under a
	// live press. The card pointers are read BEFORE and compared AFTER — an identity claim, so it is
	// the pointers rather than the count.
	ui::FxCard* cardFirst = column.cardOf(first);
	ui::FxCard* cardSecond = column.cardOf(second);
	CHECK(column.slotChoiceForTest(second, ui::ChainColumn::SlotVerb::MoveUp));
	CHECK(held.bridge().deviceAtPosition(0) == second);
	CHECK(held.bridge().deviceAtPosition(1) == first);
	CHECK(column.cardOf(first) == cardFirst);
	CHECK(column.cardOf(second) == cardSecond);
	// AND THE CARDS ARE PLACED IN THE NEW ORDER — the layout reads the chain, not a remembered list.
	CHECK(column.cardsForTest().size() == 2);
	CHECK(column.cardsForTest()[0] == cardSecond);

	// **AND THEY ARE ACTUALLY SEATED, WHICH THE ORDER ABOVE DOES NOT SAY.** A column that built its
	// cards and never placed them would pass every line so far: `cardsForTest` reads the chain, not
	// the frames. A card at a zero rectangle is one `setVisible` away from painting at the window
	// origin — the discipline `FxLane::resized` records — so the claim is the frames' own bounds,
	// taken from the column's arithmetic and checked against what was applied.
	CHECK(cardSecond->width() > 0);
	CHECK(cardSecond->height() > 0);
	CHECK(static_cast<float>(cardSecond->x()) == doctest::Approx(column.cardBounds(0).x()));
	CHECK(static_cast<float>(cardSecond->y()) == doctest::Approx(column.cardBounds(0).y()));
	CHECK(static_cast<float>(cardFirst->y()) == doctest::Approx(column.cardBounds(1).y()));
	// THE ONE THAT IS SECOND IN THE CHAIN IS BELOW THE ONE THAT IS FIRST — a vertical lane, stated as
	// a rectangle rather than as an index.
	CHECK(cardFirst->y() >= cardSecond->y() + cardSecond->height());
	// AND EACH SITS INSIDE THE COLUMN'S PADDING RATHER THAN OVER ITS EDGE.
	CHECK(static_cast<float>(cardSecond->x()) == doctest::Approx(ui::ChainColumn::kPadX));
	CHECK(static_cast<float>(cardSecond->x() + cardSecond->width())
	      == doctest::Approx(static_cast<float>(column.width()) - ui::ChainColumn::kPadX));

	// THE MENU'S OWN ANSWER ABOUT THE ENDS, so a case reads the column's rules rather than restating
	// them: the device at the top cannot move up and the one at the bottom cannot move down.
	CHECK_FALSE(column.slotChoiceForTest(second, ui::ChainColumn::SlotVerb::MoveUp));
	CHECK_FALSE(column.slotChoiceForTest(first, ui::ChainColumn::SlotVerb::MoveDown));

	// "ADD BEFORE": the anchor is the DEVICE, so the new one lands in front of THAT card.
	const DeviceInstanceId inserted = column.insertChoiceForTest(first, 0);
	REQUIRE(inserted != feathers::kNoInstance);
	CHECK(held.bridge().chainLength(0) == 3);
	CHECK(held.bridge().deviceAtPosition(1) == inserted);
	CHECK(held.bridge().deviceAtPosition(2) == first);

	// REMOVE: the chain shortened AND the card went. A card left behind for a device that is gone is
	// the shape of the use-after-frees this repo has shipped through a green ctest.
	CHECK(column.slotChoiceForTest(inserted, ui::ChainColumn::SlotVerb::Remove));
	CHECK(held.bridge().chainLength(0) == 2);
	CHECK(held.bridge().positionOf(inserted) == feathers::rack::RackBridge::kNoSlot);
	CHECK(column.cardOf(inserted) == nullptr);
	CHECK(column.cardCountForTest() == 2);
}

namespace {
/// **A HAND ON A CARD HEAD, HELD** — `dragThroughRoot` split into its three events so a case can look
/// at the lane MID-drag, which is where the bumper lives. The same replication of
/// `WindowEventHandler`: the down-frame is found by hit test, and it alone receives the drag and the
/// release wherever the pointer goes.
struct HeldPress {
	visage::Frame* down = nullptr;

	visage::MouseEvent eventAt(visage::Point windowPosition, bool isDown) const {
		visage::MouseEvent e;
		e.window_position = windowPosition;
		e.position = windowPosition - down->positionInWindow();
		e.event_frame = down;
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = isDown ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = isDown;
		e.repeat_click_count = 1;
		return e;
	}
	void press(visage::Frame* root, visage::Point at) {
		down = root->frameAtPoint(at);
		REQUIRE(down != nullptr);
		down->processMouseDown(eventAt(at, true));
	}
	void moveTo(visage::Point at) const { down->processMouseDrag(eventAt(at, true)); }
	void release(visage::Point at) const { down->processMouseUp(eventAt(at, false)); }
};
/// The first control of a device that a route may land on, or the descriptor's size if none.
size_t firstModulatable(const device::Descriptor& d) {
	for (size_t n = 0; n < d.params.size(); ++n) {
		if (d.params[n].modulatable) {
			return n;
		}
	}
	return d.params.size();
}
} // namespace

TEST_CASE("backlog JA (\"in the fx lanes I cant reorder effects\"): a card in SKLITTER's GRAIN FX and POST FX lanes is dragged by its head through the real event road to a new place, routes follow it, and one undo puts it back") {
	// **THE DEFECT, MEASURED BEFORE IT WAS FIXED.** `ChainColumn` — the column both of SKLITTER's fx rails
	// draw — built its cards with a fold, a slot menu, focus and expand, and never bound `onCardDrag`.
	// The card's own drag road ran (it is the base's) and reported to nobody: the card lifted past the
	// threshold and the release moved nothing. `FxLane`, `ModPanel` and `RackEditor` all bind it.
	//
	// **DRIVEN THROUGH THE PANEL'S TREE, NOT INTO A CALLBACK.** The press is hit-tested from the panel
	// and the drag and release go to the frame that took the press — which is how the hand's events
	// arrive, and the only way a case can notice a head control that swallows the press.
	namespace sc = feathers::fx::sklitter;
	for (const bool grainLane : { true, false }) {
		CAPTURE(grainLane);
		HeldChain held;
		PerformPanel p;
		REQUIRE(p.panel != nullptr);
		setLanes(p, /*circleOpen=*/false, /*grainOpen=*/grainLane, /*postOpen=*/!grainLane);
		ui::FxLaneRail& rail = grainLane ? p.panel->grainRailForTest() : p.panel->postRailForTest();
		feathers::rack::RackBridge& bridge = grainLane ? held.grainBridge() : held.bridge();
		REQUIRE(rail.follow(held.surface()));
		ui::ChainColumn* column = rail.columnForTest();
		REQUIRE(column != nullptr);

		// A FILTER FIRST AND — in the post lane — A SKLITTER SECOND, so the moved device is one with a
		// declared self-route (backlog HS) and the route that must survive is the one a reorder is most
		// likely to break. In the grain lane an OWLTT, which declares none.
		const char* movedTag = grainLane ? feathers::fx::owltt::kTraits.tag : sc::kTraits.tag;
		REQUIRE(bridge.descriptorFor(feathers::fx::filter::kTraits.tag) != nullptr);
		REQUIRE(bridge.descriptorFor(movedTag) != nullptr);
		const DeviceInstanceId first = column->addDevice(feathers::fx::filter::kTraits.tag);
		const DeviceInstanceId moved = column->addDevice(movedTag);
		REQUIRE(first != feathers::kNoInstance);
		REQUIRE(moved != feathers::kNoInstance);
		// FOLDED TO THEIR HEADS, so both cards and every gap are on screen in a rail of the panel's
		// minimum height — the gesture is the same on a folded card, and this is the state a player
		// with a long chain is in.
		column->cardOf(first)->setFolded(true);
		column->cardOf(moved)->setFolded(true);
		(void)rail.updateCards();

		// **A ROUTE ON EACH DEVICE, BY IDENTITY.** The holder's first perform output onto the filter,
		// and (post lane) the nested SKLITTER's own first output onto its own Rate — a self-route.
		const device::Descriptor* filterDesc = bridge.descriptorOf(first);
		REQUIRE(filterDesc != nullptr);
		const size_t filterControl = firstModulatable(*filterDesc);
		REQUIRE(filterControl < filterDesc->params.size());
		const backplane::Source core = backplane::Source::core(0);
		const backplane::Destination onFilter = backplane::Destination::control(first, filterControl);
		bridge.setRouteAmount(core, onFilter, 0.5f);
		REQUIRE(bridge.routeAmount(core, onFilter) == doctest::Approx(0.5f));
		const backplane::Source selfSource = backplane::Source::output(moved, 0);
		const backplane::Destination selfRate =
		    backplane::Destination::control(moved, static_cast<size_t>(sc::Param::Rate));
		if (!grainLane) {
			bridge.setRouteAmount(selfSource, selfRate, 0.25f);
			REQUIRE(bridge.routeAmount(selfSource, selfRate) == doctest::Approx(0.25f));
		}

		// THE OUTER LANE, so the case can say the drag did not escape to it.
		auto* outerLane = findInTree<ui::FxLane>(p.f.root);
		REQUIRE(outerLane != nullptr);
		const feathers::DeviceInstanceId outerFirst = feathers::instanceOfFxSlot(p.f.bridge.fxChain(), 0);

		// THE PRESS: the moved card's HEAD, left of centre where no mark sits. NOT VACUOUS — the hit
		// test must answer the card itself, or the press is some child's and the case asks nothing.
		ui::FxCard* card = column->cardOf(moved);
		REQUIRE(card != nullptr);
		const visage::Point from = card->positionInWindow()
		                           + visage::Point { card->width() * 0.3f, ui::FxCard::kSlotHeadHeight * 0.5f };
		REQUIRE(p.panel->frameAtPoint(from) == card);
		// AIMED AT THE GAP ABOVE THE FIRST CARD, asked of the column and expressed in the panel's frame.
		const visage::Bounds bumper = column->gapBounds(0);
		REQUIRE(bumper.height() > 0.0f);
		const visage::Point to = column->positionInWindow()
		                         + visage::Point { from.x - column->positionInWindow().x,
			                                       bumper.y() + bumper.height() * 0.5f };
		REQUIRE(to.y - column->positionInWindow().y >= 0.0f); // inside the column, or it is the cancel

		HeldPress hand;
		hand.press(p.panel, from);
		CHECK_FALSE(column->dragDevice().has_value()); // a press is not yet a drag
		hand.moveTo(to);
		// MID-DRAG: the column holds the device and the bumper names the gap the hand is over.
		REQUIRE(column->dragDevice().has_value());
		CHECK(*column->dragDevice() == moved);
		REQUIRE(column->dropGap().has_value());
		CHECK(*column->dropGap() == 0);
		// AND NOTHING OUTSIDE IT MOVED: the outer lane holds no drag and its chain is as it was.
		CHECK_FALSE(outerLane->dragSlot().has_value());
		hand.release(to);

		// THE VALUE: the moved device is first, by id.
		CHECK(bridge.deviceAtPosition(0) == moved);
		CHECK(bridge.deviceAtPosition(1) == first);
		CHECK_FALSE(column->dragDevice().has_value());
		CHECK_FALSE(column->dropGap().has_value());
		// AND IT IS DRAWN THERE — the card is placed where the chain now says, not where it was.
		CHECK(static_cast<float>(card->y()) == doctest::Approx(column->cardBounds(0).y()));
		// THE ROUTES FOLLOWED THEIR DEVICES — by identity, the same amounts on the same endpoints.
		CHECK(bridge.routeAmount(core, onFilter) == doctest::Approx(0.5f));
		if (!grainLane) {
			CHECK(bridge.routeAmount(selfSource, selfRate) == doctest::Approx(0.25f));
		}
		// AND THE OUTER CHAIN IS UNTOUCHED.
		CHECK(feathers::instanceOfFxSlot(p.f.bridge.fxChain(), 0) == outerFirst);

		// ONE UNDO PUTS IT BACK — and the cards follow on the next tick, which is the road an undo has:
		// its record holds the bridge, not the column.
		ui::EditHistory& history = column->historyForTest();
		CHECK(history.length() == 1);
		REQUIRE(history.canUndo());
		history.undo();
		(void)rail.updateCards();
		CHECK(bridge.deviceAtPosition(0) == first);
		CHECK(bridge.deviceAtPosition(1) == moved);
		CHECK(static_cast<float>(card->y()) == doctest::Approx(column->cardBounds(1).y()));
		CHECK(bridge.routeAmount(core, onFilter) == doctest::Approx(0.5f));
		if (!grainLane) {
			CHECK(bridge.routeAmount(selfSource, selfRate) == doctest::Approx(0.25f));
		}

		// AND A DROP WHERE IT ALREADY IS MOVES NOTHING AND SPENDS NO STEP.
		const visage::Point again = card->positionInWindow()
		                            + visage::Point { card->width() * 0.3f, ui::FxCard::kSlotHeadHeight * 0.5f };
		dragThroughRoot(p.panel, again, again + visage::Point { 0.0f, ui::FxCard::kDragThreshold * 2.0f });
		CHECK(bridge.deviceAtPosition(0) == first);
		CHECK(bridge.deviceAtPosition(1) == moved);
		CHECK(history.canRedo()); // no new step truncated the redo tail
	}
}
TEST_CASE("backlog JA: Feathers' own fx lane reorders by a hand's drag on a card head, through the editor's tree") {
	// **THE MEASUREMENT OF THE LANE THAT WORKED**, driven the same way as the case above so the two are
	// one question asked of two lanes: press on the head through the root, drag to a gap, release.
	FilterFixture f;
	REQUIRE(f.lane != nullptr);
	REQUIRE(f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag) == 1);
	f.lane->resized();
	REQUIRE(f.lane->deviceCount() == 2);
	const feathers::DeviceInstanceId a = feathers::instanceOfFxSlot(f.bridge.fxChain(), 0);
	const feathers::DeviceInstanceId b = feathers::instanceOfFxSlot(f.bridge.fxChain(), 1);

	ui::FxCard* card = f.lane->cardAt(1);
	REQUIRE(card != nullptr);
	const visage::Point from = card->positionInWindow()
	                           + visage::Point { card->width() * 0.3f, ui::FxCard::kSlotHeadHeight * 0.5f };
	REQUIRE(f.root->frameAtPoint(from) == card);
	const visage::Bounds bumper = f.lane->gapBounds(0);
	const visage::Point to { from.x, f.lane->positionInWindow().y + bumper.y() + bumper.height() * 0.5f };
	dragThroughRoot(f.root, from, to);

	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 0) == b);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == a);
	REQUIRE(f.gui.history()->canUndo());
	f.gui.history()->undo();
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 0) == a);
	CHECK(feathers::instanceOfFxSlot(f.bridge.fxChain(), 1) == b);
}
TEST_CASE("a chain column grows past the capacity it was built with, and its add stays live") {
	// **IT REFUSED AT CAPACITY until unbounded devices (step 6)**, and went dim to say so. A chain is never full
	// now: the record behind the column grows. What is asserted is the growth as values — every add lands, the
	// length is what was added, `full()` never says yes — and that the (+) and every "Add before" stay live.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);

	const char* tag = firstAddableTag(column);
	REQUIRE(tag != nullptr);
	const size_t capacity = held.bridge().capacity();
	REQUIRE(capacity > 0);
	for (size_t i = 0; i < capacity + 8; ++i) {
		CAPTURE(i);
		CHECK(column.addDevice(tag) != feathers::kNoInstance);
	}
	CHECK(held.bridge().chainLength(0) == capacity + 8);
	CHECK_FALSE(column.full());
	CHECK(column.addBounds().height() == doctest::Approx(ui::ChainColumn::kAddHeight));
	for (const ui::ChainColumn::SlotOption& option :
	     column.slotOptionsFor(held.bridge().deviceAtPosition(0))) {
		if (option.verb == ui::ChainColumn::SlotVerb::AddBefore) {
			CHECK(option.enabled);
		}
	}
}
TEST_CASE("a holder's own tag is offered inside its own chain, and the nesting a hand can build stops where the save format does") {
	// **THIS IS BOLT-ON-CHAINS GAP G4 BECOMING REACHABLE, so it is pinned rather than assumed.** *"Nothing
	// declares what a container may HOLD": `adopt` hands over the whole host catalogue unfiltered and the
	// rack accepts anything, so SKLITTER is in its own chain's menu. Until this column existed nobody could
	// put ANYTHING in that chain, so the question had never been asked by a hand.
	//
	// **IT IS LEFT OFFERED, DELIBERATELY, AND THE REASON IS THE BOUND BELOW.** A second SKLITTER on the
	// output is a real musical idea, and excluding it here would be this widget inventing a policy that
	// belongs to `device::` (G4's own framing: a container should DECLARE what it may hold).
	//
	// **THE EXPAND ROAD ARRIVED, AND THIS CASE WENT RED EXACTLY AS IT SAID IT WOULD.** The old text
	// stood here: *"IF AN EXPAND ROAD IS EVER ADDED TO THIS COLUMN, THIS CASE IS THE ONE THAT SHOULD GO
	// RED, because that is the change that makes the nesting unbounded from a hand." It did, on the
	// `CHECK_FALSE(onExpand)` line, when the missing expand button for a SKLITTER inside a SKLITTER
	// was answered — which is a prediction earning its keep rather than a broken test.
	//
	// **SO THE BOUND MOVED FROM "THERE IS NO WAY DEEPER" TO "THE WAY DEEPER STOPS WHERE THE FORMAT
	// STOPS." It was never safe because one level was the right number; it was safe because a hand
	// could not go further. Now it can, so the number has to be one somebody chose — and the only
	// defensible one is the loader's, `rack::kMaxDepth`, because a patch a player can build and cannot
	// reload is the honesty rule failing at the worst possible moment. `ChainColumn::nestingFull` is
	// that rule and it is asserted below, at the depth where it bites rather than only at depth 0.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);

	const std::vector<const device::Descriptor*> offered = column.addable();
	const bool offersItself =
	    std::any_of(offered.begin(), offered.end(), [](const device::Descriptor* d) {
		    return device::sameTag(d->traits.tag, fx::sklitter::kTraits.tag);
	    });
	CHECK(offersItself);

	// IT GOES IN, AND THE CHAIN SAYS SO — a value, not a drawing.
	const DeviceInstanceId inner = column.addDevice(fx::sklitter::kTraits.tag);
	REQUIRE(inner != feathers::kNoInstance);
	CHECK(held.bridge().chainLength(0) == 1);
	// **AGAINST THE CATALOGUE'S ANSWER, NOT AGAINST `kDescriptor`.** `kDevices` holds a COPY of each
	// device's descriptor and `deviceForTag` hands back `&kDevices[i]`, so the registry's address is not
	// the header constant's — a first draft compared the two and went red on a perfectly correct add.
	CHECK(held.bridge().descriptorOf(inner) == held.bridge().descriptorFor(fx::sklitter::kTraits.tag));
	CHECK(held.bridge().descriptorOf(inner) != nullptr);
	CHECK(device::sameTag(held.bridge().heldTagOf(inner).data(), fx::sklitter::kTraits.tag));
	REQUIRE(column.cardOf(inner) != nullptr);

	// THE ROAD IS BOUND NOW — and so is the slot menu, which is what keeps "the road exists" from being
	// satisfied by a column that binds everything to nothing.
	CHECK(static_cast<bool>(column.cardOf(inner)->onExpand));
	CHECK(static_cast<bool>(column.cardOf(inner)->onSlotMenu));

	// **AT THE TOP OF THE TREE THERE IS ROOM, SO THE MENU STILL OFFERS A CONTAINER.** Asserted as the
	// column's own answer, because a case that restated `kMaxDepth` here would be checking its own
	// arithmetic against itself.
	CHECK_FALSE(column.nestingFull());

	// **AND AT THE FORMAT'S LIMIT IT DOES NOT.** Driven at a nest one short of the loader's refusal,
	// which is the only depth where the rule has anything to say — a case asking at depth 0 would pass
	// with the whole filter deleted, which is the shape a green known-bad exists to expose.
	ui::DeviceNest deep;
	while (static_cast<size_t>(deep.depth) < rack::kMaxUsefulDepth) {
		deep = ui::DeviceNest::under(deep, 1);
	}
	column.setNest(deep);
	CHECK(column.nestingFull());

	// **THE MENU STILL OFFERS EVERYTHING, AND SAYS NO BY GOING DIM.** An earlier draft dropped the
	// container rows out of `addable()` and that was wrong twice over: it is the vanishing this file
	// already rules against — *"a control that vanished at the bound would leave a player wondering
	// where it went" — and it silently renumbered `addChoiceForTest`, so a case naming the nth device
	// named a different one at depth. The list is stable; only the ROWS change.
	const std::vector<const device::Descriptor*> atLimit = column.addable();
	CHECK(atLimit.size() == column.addable().size());
	CHECK(std::any_of(atLimit.begin(), atLimit.end(),
	                  [](const device::Descriptor* d) { return d->container != nullptr; }));

	// **AND THE REFUSAL IS REAL, NOT ONLY DRAWN.** `addDevice` is the one road both the (+) and the
	// card menu take, so the bound is asserted THERE — a guard only in the menu would be a guard on one
	// of two roads. An ordinary effect at the same depth is untouched, which is what makes this a
	// nesting bound rather than a dead chain.
	const DeviceInstanceId refused = column.addDevice(fx::sklitter::kTraits.tag);
	CHECK(refused == feathers::kNoInstance);
	CHECK(held.bridge().chainLength(0) == 1); // still just the SKLITTER added above

	const char* plain = nullptr;
	for (const device::Descriptor* d : atLimit) {
		if (d->container == nullptr) {
			plain = d->traits.tag;
			break;
		}
	}
	REQUIRE(plain != nullptr);
	CHECK(column.addDevice(plain) != feathers::kNoInstance);
	CHECK(held.bridge().chainLength(0) == 2);

	// **ONE LEVEL SHALLOWER AND THE CONTAINER GOES IN** — the bound sits exactly on the edge rather
	// than near it, and `test_rack_nesting.cpp` is where that edge is pinned against the LOADER.
	ui::DeviceNest room;
	while (static_cast<size_t>(room.depth) + 1 < rack::kMaxUsefulDepth) {
		room = ui::DeviceNest::under(room, 1);
	}
	column.setNest(room);
	CHECK_FALSE(column.nestingFull());
	CHECK(column.addDevice(fx::sklitter::kTraits.tag) != feathers::kNoInstance);
}
TEST_CASE("at the nesting wall the chain congratulates you, in its specified words, and the refusal is honest with or without them") {
	// The runtime is bounded: if nesting goes over 20 deep, or as far as the plugin can still be saved,
	// a popup says "you've done it. you've nested as much as you can nest. you win. congratulations."
	//
	// **THE WORDS ARE ASSERTED, NOT PHOTOGRAPHED**, and they are asserted VERBATIM — lower case, four
	// sentences, no full stop added or removed. It is an easter egg and an honest refusal at once, and
	// tidying it into an error string would cost the first without improving the second.
	const std::string said = ui::ChainColumn::kNestingFull;
	CHECK(said == "you've done it. you've nested as much as you can nest. you win. congratulations.");
	// The properties that make it the specified text rather than a paraphrase of it.
	CHECK(said.find("congratulations") != std::string::npos);
	CHECK(said.find("you win") != std::string::npos);
	CHECK(said == said); // and lower case: no capital opens it
	CHECK(static_cast<bool>(std::islower(static_cast<unsigned char>(said.front()))));

	// **AND IT IS SAID ON THE ONE ROAD A REFUSAL IS WORDED ON.** A notifier that records what it was
	// told, so the claim is the message reaching a tray rather than a constant existing.
	struct Recorder final : ui::Notifier {
		std::vector<std::string> lines;
		void say(std::string_view text) override { lines.emplace_back(text); }
	};
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	Recorder tray;
	ui::ChainColumn column(held.bridge(), registry, mod, history, &tray);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);

	ui::DeviceNest deep;
	while (static_cast<size_t>(deep.depth) < rack::kMaxUsefulDepth) {
		deep = ui::DeviceNest::under(deep, 1);
	}
	column.setNest(deep);
	REQUIRE(column.nestingFull());
	CHECK(column.addDevice(fx::sklitter::kTraits.tag) == feathers::kNoInstance);
	REQUIRE(tray.lines.size() == 1);
	CHECK(tray.lines.front() == said);

	// **THE REFUSAL IS HONEST WITHOUT A TRAY TO SAY IT ON**, which is the half that must not depend on
	// the joke: a column built with a null notifier still refuses, and the add still does nothing.
	ui::ChainColumn quiet(held.bridge(), registry, mod, history, nullptr);
	quiet.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);
	quiet.setNest(deep);
	const size_t was = held.bridge().chainLength(0);
	CHECK(quiet.addDevice(fx::sklitter::kTraits.tag) == feathers::kNoInstance);
	CHECK(held.bridge().chainLength(0) == was);
}
TEST_CASE("the holder's own sources reach inside the chain in the ENGINE, and the column still builds "
          "no arm for them — the road that opened is the perform pane's, not this one's") {
	// **THE GAP THIS ARC LEAVES, PINNED RATHER THAN DESCRIBED**, because a surface that quietly lacks a
	// verb reads as one that has it. Decision D3(a) of `2026-09-19-bolt-on-chains.md` gave SKLITTER's
	// seven perform outputs to its inner chain as that engine's CORE SOURCES
	// (`kPostChainAxes.coreSources = kPerformOutputs`, published every block by `runPostChain`), and
	// trap T1 is why they are core sources rather than `Source::outerOutput`: `upstreamOf` answers
	// FALSE for every output a device publishes into its own chain.
	//
	// So the ENGINE road is open. This case used to say **the UI road is closed at BOTH of its two
	// ends** and name them so a later session fixing one knew the other existed. **BOTH HAVE SINCE BEEN
	// OPENED, BY ROADS NEITHER END PREDICTED** (backlog HM, 2026-09-21), and the case is kept — rewritten
	// — because what it now pins is the shape of the fix rather than the shape of the gap:
	//
	//  - the ARM is the perform pane's existing chip, not a new widget in this column. It arms the OUTER
	//    state with SKLITTER's own output, and `FxLaneRail` spells that `Source::core(n)` in here
	//    (`ModState::setOuterSpelling`) — so one chip is one arm with two spellings, rather than a
	//    second chip a hand would have to choose between. **This column still builds none**, which is
	//    the assertion below and is now a claim about the FIX being where it is rather than a gap;
	//  - the MENU is open: this column installs a `setSourceLister` and the seven are in it, which the
	//    "closed end two" assertion below used to deny.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);

	const char* tag = firstAddableTag(column);
	REQUIRE(tag != nullptr);
	const DeviceInstanceId inner = column.addDevice(tag);
	REQUIRE(inner != feathers::kNoInstance);

	// **THE DESTINATION IS THE FIRST MODULATABLE CONTROL, FOUND RATHER THAN GUESSED.** A first draft
	// used index 0 and went red at `0 == Approx(0.5)` — `backplane::mayRoute` refuses a destination
	// whose `params[i].modulatable` is false, and a device's first control is often a mode chip. The
	// red was the case picking a bad target, not the road being shut; asserting the road from a
	// refused pair would have reported the opposite of the truth.
	const device::Descriptor* desc = held.bridge().descriptorOf(inner);
	REQUIRE(desc != nullptr);
	size_t control = desc->params.size();
	for (size_t i = 0; i < desc->params.size(); ++i) {
		if (desc->params[i].modulatable) {
			control = i;
			break;
		}
	}
	REQUIRE(control < desc->params.size());

	// **THE ENGINE ROAD IS OPEN, AS A VALUE.** A route from this chain's own core source 0 — SKLITTER's
	// `Perf X` — to a modulatable control of a device inside the chain is ACCEPTED by the record and
	// reads back at the amount it was given. Nothing gates it out: a core source is this engine's own,
	// which is decision D3(a) working.
	const backplane::Source perfX = backplane::Source::core(0);
	const backplane::Destination innerControl = backplane::Destination::control(inner, control);
	held.bridge().setRouteAmount(perfX, innerControl, 0.5f);
	CHECK(held.bridge().routeAmount(perfX, innerControl) == doctest::Approx(0.5f));
	// AND THE SEVENTH IS TOO — the whole family, not just the one a lucky index reached.
	const backplane::Source perfLast = backplane::Source::core(fx::sklitter::kPerformOutputs - 1);
	held.bridge().setRouteAmount(perfLast, innerControl, 0.25f);
	CHECK(held.bridge().routeAmount(perfLast, innerControl) == doctest::Approx(0.25f));
	// AND AN EIGHTH DOES NOT EXIST — `coreSources` is seven, so this is the bound rather than a typo.
	held.bridge().setRouteAmount(backplane::Source::core(fx::sklitter::kPerformOutputs), innerControl, 0.25f);
	CHECK(held.bridge().routeAmount(backplane::Source::core(fx::sklitter::kPerformOutputs), innerControl)
	      == doctest::Approx(0.0f));

	// **CLOSED END ONE: NOTHING CAN ARM IT.** Arming is what makes a destination glow and a press route
	// (`ModState`), and an arm is a `SourceArm` widget. `RackEditor` builds one per core source beside
	// each macro knob (`macroArms_`); this column builds none, because the seven are published by the
	// DEVICE and have no knob here to sit beside. Asserted as the absence of the widget rather than as
	// a drawing, for `fx_lane.h`'s reason: a picture of a rail with no arm looks exactly like a picture
	// of a rail whose arm is painted in the background colour.
	//
	// **COUNTED AGAINST WHAT THE CHAIN'S OWN DEVICES DECLARE, RATHER THAN AGAINST ZERO, AND THE
	// DIFFERENCE IS THE WHOLE POINT OF THE EDIT THAT MADE IT.** This read `arms == 0` while the column
	// drew no chips at all — including for the devices IN the chain, which was the defect fixed on
	// 2026-09-20. A zero here now measures the wrong absence: it would go red the moment a modulator
	// was put in the chain, which is the case working correctly, and it would go GREEN on a build that
	// had lost the inner arms again. So a publishing device is put in deliberately and the claim is
	// that the arms are exactly the inner outputs and not one more — seven more would be the holder's.
	const device::Descriptor* publishing = nullptr;
	for (const device::Descriptor* d : column.addable()) {
		if (d->outputCount() > 0) {
			publishing = d;
			break;
		}
	}
	REQUIRE(publishing != nullptr); // else the count below proves nothing about the inner road either
	REQUIRE(column.addDevice(publishing->traits.tag) != feathers::kNoInstance);

	size_t arms = 0;
	for (const visage::Frame* child : column.children()) {
		arms += dynamic_cast<const ui::SourceArm*>(child) != nullptr ? 1 : 0;
	}
	size_t declared = 0;
	for (size_t position = 0; position < column.deviceCount(); ++position) {
		const device::Descriptor* d = held.bridge().descriptorOf(column.deviceAt(position));
		declared += d != nullptr ? d->outputCount() : 0;
	}
	CHECK(declared >= 1); // not vacuous: there is something for an arm to be, so `==` is a real test
	// **THE ARMS ARE EXACTLY THE INNER OUTPUTS.** A build that had also grown arms for the holder's
	// seven would read `declared + 7` here and this would go red — which is the absence this case is
	// about, said as a number rather than as a zero that stopped meaning it. (Written as one `==`
	// rather than as a second `!=` against `declared + 7`: that second line would be implied by this
	// one and would read as though it could fail on its own.)
	CHECK(arms == declared);

	// **WHAT WAS "CLOSED END TWO" IS OPEN, AND THIS LINE IS THE ONE THAT PROVED IT WAS SHUT.** It read
	// `CHECK(mod.sources().empty())` — an inner control's "route from…" menu walks `ModState::sources()`,
	// that list is whatever a `setSourceLister` installed, and this column installed none, so
	// `inboundSources` could never find a route however many were live and `showRouteMenu` refused a
	// menu with nothing in it. it surfaced as a right-click on an fx lane param showing no context
	// menu of its mapped mod sources, and this assertion going RED is what said the fix had landed.
	// **The list is asserted by CONTENT rather than by non-emptiness**, because "not empty" is satisfied
	// by any list at all — including the wrong one.
	const std::vector<backplane::Source> listed = mod.sources();
	for (size_t i = 0; i < fx::sklitter::kPerformOutputs; ++i) {
		CAPTURE(i);
		CHECK(std::find(listed.begin(), listed.end(), backplane::Source::core(i)) != listed.end());
	}
	// AND THE EIGHTH IS NOT THERE — the same bound the engine road asserted above, said of the menu, so
	// the two cannot drift apart into a row offering a source that does not exist.
	CHECK(std::find(listed.begin(), listed.end(),
	                backplane::Source::core(fx::sklitter::kPerformOutputs))
	      == listed.end());

	// **AND THE OUTER ROAD IS STILL SHUT FOR THESE SEVEN, WHICH IS WHY THE CORE SPELLING IS THE FIX.**
	// On the real panel `FxLaneRail::setOuterModulation` parents this state to SKLITTER's, so
	// `ModState::sources()` also yields the OUTER host's sources — a Feathers LFO reaches an inner
	// control that way, spelled `.asOuter()`. **SKLITTER's own seven cannot be spelled that way**: an
	// outer source is gated by `devices.outerUpstream`, which is `RackEngine::upstreamOf`, which answers
	// FALSE for every output a device publishes into its own chain. That is trap T1, measured and pinned
	// by three cases in `tests/test_rack_nesting.cpp` — cited rather than re-proved here, because proving
	// it needs a host to hand the chain an outer gate and this fixture has none. `setOuterSpelling` is
	// what translates the holder's own output onto the core source it really is in here; the cases at
	// "backlog HM" drive that road end to end.
}
TEST_CASE("backlog HA: EVERY output of EVERY device a chain column can hold has an arm — inside its "
          "own card and armed to that device's own endpoint in THIS chain") {
	// **THE DEFECT: an LFO put in the grain fx had no modulation output on its card.** `ChainColumn` was extracted out of `RackEditor`'s card half
	// during the nested-popup work and the output-arm machinery did not come with it — `SourceArm`
	// appeared in `mod_panel`, `fx_lane` and `rack_editor` and nowhere else. So a modulator in an
	// inner chain drew no chip, and with no chip there was no arm, and with no arm its output could
	// not be routed from the surface it was sitting on at all. The honesty rule failing by omission.
	//
	// **NOT LFO-SPECIFIC AND NOT GRAIN-SPECIFIC, WHICH IS WHY THIS WALKS THE WHOLE OFFER.** Nothing in
	// the column knew what an output was, so every publishing device in either lane was affected; a
	// case naming the LFO would have gone green on a fix that only knew about the LFO.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	// TALL ENOUGH THAT NO CARD IS SCROLLED OUT — an arm follows its card's visibility, so a short
	// column would hide chips BY THE RULE and this case would be measuring the scroll.
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);

	size_t publishing = 0;
	size_t outputsChecked = 0;
	size_t multiOutputDevices = 0;
	const std::vector<const device::Descriptor*> offered = column.addable();
	REQUIRE(offered.size() >= 4);

	for (const device::Descriptor* desc : offered) {
		if (desc->outputCount() == 0) {
			continue; // an effect publishes nothing and gets no strip — the other half of the rule
		}
		++publishing;
		const std::string tag(desc->traits.tag); // NUL-terminated, any length: a fixed 4 overruns "eq"
		CAPTURE(tag);
		// **A COLUMN PER DEVICE**, so a crowded chain cannot scroll one of them out and turn a real
		// absence into a hidden one.
		HeldChain each;
		ui::ModState eachMod;
		ui::EditHistory eachHistory;
		ui::ChainColumn one(each.bridge(), registry, eachMod, eachHistory, nullptr);
		one.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);
		const DeviceInstanceId device = one.addDevice(desc->traits.tag);
		REQUIRE(device != feathers::kNoInstance);

		ui::FxCard* card = one.cardOf(device);
		REQUIRE(card != nullptr);
		REQUIRE(card->isVisible());
		REQUIRE_FALSE(card->folded());
		const visage::Bounds box = card->bounds();
		REQUIRE(box.width() > 0.0f);
		// **THE CARD RESERVED THE ROWS ITS CHIPS NEED**, asked of the FRAME — the only side of it that
		// can answer, and the side a host lays arms into. A card in this column grows to hold its own
		// foot (`laneExtent`), so the clamp must not be firing; that is a claim with a false side.
		CHECK(card->ownFrame().outputs.height()
		      == doctest::Approx(ui::kCardOutputStripHeight * static_cast<float>(card->outputRows())));

		float previousRight = -1.0f;
		float previousTop = -1.0f;
		for (size_t output = 0; output < desc->outputCount(); ++output) {
			CAPTURE(output);
			CAPTURE(nameOrUnnamed(desc->outputs[output].name));
			// **THE ARM EXISTS AND IS SHOWN.** An arm placed and hidden is an output a hand cannot
			// reach, which is the same failure as no arm at all.
			ui::SourceArm* arm = one.outputArmOf(device, output);
			REQUIRE(arm != nullptr);
			if (((card->outputsShown() >> output) & 1u) == 0u) {
				CHECK_FALSE(arm->isVisible()); // NOT ON THE CARD (`DeviceHost::outputsShown`): hidden, its route reading on
				continue;
			}
			CHECK(arm->isVisible());
			const visage::Bounds chip = one.outputArmBoundsForTest(device, output);
			REQUIRE(chip.width() > 0.0f);
			// INSIDE ITS CARD, ALL FOUR EDGES — `visage::Bounds` has `contains(Point)` and not
			// `contains(Bounds)`, so the four inequalities are written out.
			CHECK(chip.x() >= box.x() - 0.5f);
			CHECK(chip.right() <= box.right() + 0.5f);
			CHECK(chip.y() >= box.y() - 0.5f);
			CHECK(chip.bottom() <= box.bottom() + 0.5f);
			// AND AFTER THE ONE BEFORE IT IN READING ORDER: further right on the same row, or a row
			// below. See the walk's own note — this is the half of backlog HA about FORMATTING.
			const bool sameRow = std::abs(chip.y() - previousTop) < 0.5f;
			if (output > 0 && sameRow) {
				CHECK(chip.x() >= previousRight);
			}
			else if (output > 0) {
				CHECK(chip.y() > previousTop);
			}
			previousRight = chip.right();
			previousTop = chip.y();
			++outputsChecked;
		}

		// **AND THE ARM ARMS ITS OWN ENDPOINT, WHICH THE GEOMETRY ABOVE CANNOT SAY.** A chip wired to
		// the wrong source sits in exactly the right rectangle and draws identically. **The LAST
		// output, not the first** — a column that built one arm and laid the rest on top of it would
		// pass a press on output 0.
		//
		// **AND THE ENDPOINT NAMES A DEVICE OF *THIS* CHAIN**, which is what keeps the offer honest:
		// a route written from this chip goes through this column's own bridge, so an id from any
		// other authority would be an arm offering a route the engine refuses.
		const size_t last = desc->outputCount() - 1;
		ui::SourceArm* lastArm = one.outputArmOf(device, last);
		REQUIRE(lastArm != nullptr);
		lastArm->onMouseDown().callback(at(4.0f, 4.0f));
		CHECK(eachMod.armed());
		CHECK(eachMod.source() == backplane::Source::output(device, last));
		CHECK(eachMod.source().isDevice());
		CHECK(each.bridge().positionOf(device) != feathers::rack::RackBridge::kNoSlot);

		// AND THE ARMS GO WHEN THE DEVICE DOES, rather than staying aimed at something nothing holds.
		REQUIRE(one.outputArmCountForTest() == desc->outputCount());
		REQUIRE(one.removeDevice(device));
		CHECK(one.outputArmCountForTest() == 0);

		if (desc->outputCount() > 1) {
			++multiOutputDevices;
		}
	}
	// NOT VACUOUS, AND EACH COUNTER COUNTS SOMETHING THE LOOP COULD HAVE SKIPPED. The breadth is
	// carried by the `offered.size() >= 4` above.
	CHECK(publishing >= 1);
	CHECK(outputsChecked >= 7);
	CHECK(multiOutputDevices >= 1);
}
TEST_CASE("a chain column's chips follow their card — folded away with its body and gone with its "
          "device — and BOTH of a SKLITTER's lanes have them") {
	// **THE THREE STATES THE PLACEMENT WALK OWES, asked of the column that had none of them.** Each is
	// a rule the other hosts already follow and `fx_lane.cpp` records the reason for: the strip is
	// BODY, so a fold takes it (an arrow that survived a fold would be the head carrying an output
	// again by the back door); a card scrolled out of view is hidden and an arm left shown would draw
	// a chip where no card is.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState master;
	ui::ModState grain;
	ui::EditHistory history;

	const auto publisherTag = [&](ui::ChainColumn& column) -> const char* {
		for (const device::Descriptor* d : column.addable()) {
			if (d->outputCount() > 0) {
				return d->traits.tag;
			}
		}
		return nullptr;
	};

	ui::ChainColumn masterColumn(held.bridge(), registry, master, history, nullptr);
	masterColumn.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);
	// **THE GRAIN LANE IS A SECOND CHAIN BEHIND A SECOND BRIDGE**, and it is the lane the report
	// named. A fix applied to whichever lane a case happened to build would leave the other silent.
	ui::ChainColumn grainColumn(held.grainBridge(), registry, grain, history, nullptr);
	grainColumn.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);

	const char* tag = publisherTag(masterColumn);
	REQUIRE(tag != nullptr);
	const DeviceInstanceId inMaster = masterColumn.addDevice(tag);
	const DeviceInstanceId inGrain = grainColumn.addDevice(tag);
	REQUIRE(inMaster != feathers::kNoInstance);
	REQUIRE(inGrain != feathers::kNoInstance);
	REQUIRE(masterColumn.outputArmCountForTest() >= 1);
	CHECK(grainColumn.outputArmCountForTest() == masterColumn.outputArmCountForTest());

	ui::SourceArm* grainArm = grainColumn.outputArmOf(inGrain, 0);
	REQUIRE(grainArm != nullptr);
	CHECK(grainArm->isVisible());

	// **AND THE TWO LANES ARM SEPARATELY, which is the authority claim.** `SklitterExpanded` gives each
	// lane its own `ModState` so that a master-lane control is not lit for a grain-lane modulator the
	// engine will refuse to route; a shared one would make this second check fail.
	grainArm->onMouseDown().callback(at(4.0f, 4.0f));
	CHECK(grain.armed());
	CHECK(grain.source() == backplane::Source::output(inGrain, 0));
	CHECK_FALSE(master.armed());

	// FOLDED: the strip is body, so the chip goes with it.
	ui::FxCard* card = grainColumn.cardOf(inGrain);
	REQUIRE(card != nullptr);
	card->setFolded(true);
	grainColumn.resized();
	CHECK_FALSE(grainArm->isVisible());
	card->setFolded(false);
	grainColumn.resized();
	CHECK(grainArm->isVisible());

	// SCROLLED OUT: a short column hides the card, and the chip follows it rather than drawing where
	// no card is. **The card is driven out of view by the column's own scroll**, not by hiding it by
	// hand, so this is the road a wheel takes.
	grainColumn.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 120);
	grainColumn.resized();
	REQUIRE(grainColumn.maxScroll() > 0.0f);
	grainColumn.setScroll(grainColumn.maxScroll());
	if (!card->isVisible()) {
		CHECK_FALSE(grainArm->isVisible());
	}

	// GONE WITH THE DEVICE: the arm is destroyed, not left aimed at an id the chain no longer holds.
	grainColumn.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);
	REQUIRE(grainColumn.removeDevice(inGrain));
	CHECK(grainColumn.outputArmCountForTest() == 0);
	CHECK(grainColumn.outputArmOf(inGrain, 0) == nullptr);
	// AND THE OTHER LANE IS UNTOUCHED — two chains, two sets of arms.
	CHECK(masterColumn.outputArmCountForTest() >= 1);
}
TEST_CASE("the FX LANES rail follows the holder's handle, and drops its column when the handle goes") {
	// **THE ABA RULE, AS A CASE.** The pointer a host files for a container is a CONTEMPORANEOUS
	// HANDLE: the converger replaces a container's whole device instance when a body arrives (trap
	// T9), so a rail that remembered the pointer would draw a bridge inside a destroyed device. The
	// claim is that a DIFFERENT answer rebuilds and a null answer drops.
	// **THE TWO CHAINS ARE DECLARED BEFORE THE PANEL, SO THEY OUTLIVE IT.** A column holds a
	// `RackBridge&` into its chain and its cards address devices through it; a chain destroyed while a
	// column is still up is a use-after-free in the FIXTURE rather than in the code under test, and it
	// is the kind that reads freed memory without faulting (which is why `sanitize-gate.sh` exists).
	HeldChain a;
	HeldChain b;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::FxLaneRail& rail = p.panel->postRailForTest();
	setRails(p, false, true);

	// NO RUNNING INSTANCE (this fixture has no DSP behind the bridge) — so no column, and nothing that
	// answers a hand. A (+) over a null bridge is a button that cannot add.
	CHECK_FALSE(rail.liveForTest());
	CHECK(rail.columnForTest() == nullptr);
	CHECK(rail.children().empty());

	CHECK(rail.follow(a.surface()));
	REQUIRE(rail.liveForTest());
	CHECK(rail.bridgeForTest() == &a.bridge());
	CHECK(rail.children().size() == 1); // ONE lane per rail again, since the split into two rails
	// PUT SOMETHING IN `a`'s CHAIN, so "which chain is the column over" has a VALUE to be read from
	// rather than only an address.
	REQUIRE(rail.columnForTest() != nullptr);
	const char* tag = firstAddableTag(*rail.columnForTest());
	REQUIRE(tag != nullptr);
	REQUIRE(rail.columnForTest()->addDevice(tag) != feathers::kNoInstance);
	CHECK(rail.columnForTest()->deviceCount() == 1);

	// THE SAME HANDLE AGAIN IS NOT A CHANGE — a rail that reported one every tick would rebuild the
	// card stack sixty times a second, which is a rebuild under every live press.
	CHECK_FALSE(rail.follow(a.surface()));
	CHECK(rail.columnForTest()->deviceCount() == 1); // not rebuilt: the chain it draws is still a's

	// **A DIFFERENT INSTANCE IS A DIFFERENT CHAIN: rebuilt over the new bridge, not left over the old.**
	//
	// **AND THE CLAIM IS THE CHAIN'S CONTENTS, NOT THE COLUMN'S ADDRESS — because the address is the
	// very thing this rule says not to trust.** A first draft asserted `columnForTest() != was` and
	// went RED here: the allocator handed the rebuilt column the address the freed one had just
	// vacated, so a genuine rebuild read as no rebuild at all. That is ABA, in a case written about
	// ABA, and it is the second reason `b`'s chain is left empty: 1 → 0 cannot be faked by an address.
	CHECK(rail.follow(b.surface()));
	CHECK(rail.bridgeForTest() == &b.bridge());
	REQUIRE(rail.columnForTest() != nullptr);
	CHECK(rail.columnForTest()->deviceCount() == 0);

	// AND A HANDLE THAT GOES TAKES THE COLUMN WITH IT.
	CHECK(rail.follow(nullptr));
	CHECK_FALSE(rail.liveForTest());
	CHECK(rail.children().empty());
	CHECK_FALSE(rail.follow(nullptr)); // a null that stays null is not a change either
}
TEST_CASE("an open FX LANES rail puts a FLOOR under the panel's height, so its first card is not a sliver") {
	// **FOUND BY LOOKING, AND IT IS A DEFECT THIS ARC INTRODUCED.** While the rail held three lines of
	// text it needed no height at all; holding CARDS, it does. `SklitterExpanded::minimumSize` already
	// maxes against `CircleRail::kOpenMinimumHeight` for exactly this reason — *"a grid with few live
	// cells is shorter than a 132px disk … and a panel that believed the grid would clip the circle" —
	// and the fx rail had no such floor. PHOTOGRAPHED on the private display with the body collapsed
	// and the circle closed: the panel came out 392px wide and short enough that a filter card was cut
	// off mid-plot, with the grain line immediately under it.
	//
	// **THE FLOOR IS DERIVED FROM WHAT THE RAIL HOLDS, NOT CHOSEN**, and it is a FLOOR rather than a
	// size for the same reason the circle's is: a rail is as tall as the panel, so this is the shortest
	// the panel may be while the rail is open.
	// **THE BODY IS COLLAPSED AND THE CIRCLE CLOSED, WHICH IS THE ONLY STATE THE DEFECT LIVES IN — and
	// the first draft of this case did not drive it.** With the body OPEN the grid already asks for
	// more height than this floor, so every assertion below passed with the floor deleted: a known-bad
	// removing `std::max(p.y, FxLaneRail::kOpenMinimumHeight)` left all 159 cases GREEN. That is the
	// venue rule from the other end — a case must be shown capable of expressing the outcome before its
	// silence counts as evidence — and `setFolds` is what expresses it.
	//
	// **A GREEN KNOWN-BAD IS HOW YOU FIND OUT A CASE CANNOT SEE ITS OWN SUBJECT.** A mutation is
	// usually run to check the CODE; the more valuable use is checking the CASE, because a case that
	// cannot express its subject passes for the same reason a correct one does and there is nothing in
	// its text that says which. Nothing about this case LOOKED wrong — it named the floor, read
	// `minimumSize()`, and compared against the constant. It was asking in the one state where the
	// answer is the same either way.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	ui::FxLaneRail& post = p.panel->postRailForTest();

	// CLOSED, THE RAIL ASKS FOR NOTHING DOWN — a 34px strip has no card in it, and a floor that applied
	// while shut would make every SKLITTER panel taller for a rail nobody opened.
	setFolds(p, /*bodyOpen=*/false, /*circleOpen=*/false, /*fxOpen=*/false);
	const float shutFloor = p.panel->minimumSize().y;
	CHECK(shutFloor < ui::FxLaneRail::kOpenMinimumHeight); // the state the floor must NOT reach into

	// **EITHER RAIL ALONE RAISES IT, AND THE SECOND ONE COSTS NOTHING MORE.** Both lanes are floors on
	// ONE height, so `minimumSize` MAXes rather than sums — which is the whole reason the split into
	// two rails is free in height and expensive only in width. Driven per rail, because a `||` that
	// had lost one of its operands would pass a both-open check for ever.
	setLanes(p, /*circleOpen=*/false, /*grainOpen=*/true, /*postOpen=*/false);
	const float grainOnly = p.panel->minimumSize().y;
	CHECK(grainOnly >= ui::FxLaneRail::kOpenMinimumHeight);
	setLanes(p, /*circleOpen=*/false, /*grainOpen=*/false, /*postOpen=*/true);
	const float postOnly = p.panel->minimumSize().y;
	CHECK(postOnly >= ui::FxLaneRail::kOpenMinimumHeight);

	setFolds(p, /*bodyOpen=*/false, /*circleOpen=*/false, /*fxOpen=*/true);
	const float openFloor = p.panel->minimumSize().y;
	CHECK(openFloor >= ui::FxLaneRail::kOpenMinimumHeight);
	CHECK(openFloor > shutFloor); // strictly, because the shut panel is below the floor
	// AND THE SECOND RAIL ADDED NO HEIGHT — the MAX, read as a number rather than trusted.
	CHECK(openFloor == doctest::Approx(grainOnly));
	CHECK(openFloor == doctest::Approx(postOnly));

	// **AND EACH COLUMN ACTUALLY GETS THE ROOM** — the half the floor alone cannot promise, since a
	// panel that asked for the height and then laid the rail out inside the old rectangle would pass
	// the line above. Read off the applied rectangles.
	REQUIRE(grain.columnForTest() == nullptr); // this fixture has no running instance; the rails are empty
	CHECK(grain.columnBounds().height() >= ui::ChainColumn::kMinimumUsefulHeight);
	CHECK(post.columnBounds().height() >= ui::ChainColumn::kMinimumUsefulHeight);

	// AND WITH CHAINS BEHIND THEM, the same rectangles hold a whole card head plus some of its body —
	// which is what "not a sliver" means as a number rather than as an impression.
	HeldChain held;
	REQUIRE(grain.follow(held.surface()));
	REQUIRE(post.follow(held.surface()));
	REQUIRE(grain.columnForTest() != nullptr);
	REQUIRE(post.columnForTest() != nullptr);
	CHECK(static_cast<float>(grain.columnForTest()->height()) >= ui::ChainColumn::kMinimumUsefulHeight);
	CHECK(static_cast<float>(post.columnForTest()->height()) >= ui::ChainColumn::kMinimumUsefulHeight);

	// **AND IT COSTS THE COMMON ARRANGEMENT NOTHING, which is the claim that matters to whoever merges
	// this next to a panel whose geometry was just rewritten.** The circle's floor is the taller, so
	// with PERFORM open the panel's height is the circle's answer and this floor never binds.
	//
	// **AND THE MARGIN IS BACK, WHICH IT WAS NOT AN HOUR AGO.** While both lanes were stacked in ONE
	// rail this constant was 422 and `CircleRail::kOpenMinimumHeight` is 422 — two unrelated sums
	// landing on one number, so the line below passed at EXACT equality with nothing to spare for the
	// next thing added to the rail. Splitting into two rails drops it to 231, because a rail holding
	// one lane needs one caption where the stacked one needed two captions and two column floors. The
	// STRICT `<` is the assertion that says the margin exists rather than merely that the comparison
	// holds — and it is what would have caught the coincidence had it been written that way then.
	CHECK(ui::FxLaneRail::kOpenMinimumHeight < ui::CircleRail::kOpenMinimumHeight);
	setFolds(p, /*bodyOpen=*/false, /*circleOpen=*/true, /*fxOpen=*/true);
	const float bothOpen = p.panel->minimumSize().y;
	setFolds(p, /*bodyOpen=*/false, /*circleOpen=*/true, /*fxOpen=*/false);
	CHECK(p.panel->minimumSize().y == doctest::Approx(bothOpen)); // opening the fx rail added no height
}
TEST_CASE("the panel reads grid | PERFORM | GRAIN FX | POST FX, left to right, which is signal order") {
	// **THE DECISION, AS GEOMETRY**: a second vertical tray, grain fx to the left of post fx. Asserted on the APPLIED rectangles' x, because that is what a hand sees; the
	// order the rails are constructed in and the order `resized()` walks them are both backwards from
	// the reading order, so a case that read either of those would be checking the implementation
	// against itself.
	//
	// **AND IT IS SIGNAL ORDER, WHICH IS THE POINT AND NOT A COINCIDENCE.** A chosen grain is diverted
	// into the grain lane, rejoins the wet, and the post lane runs on the sum. The arrangement this
	// replaces stacked POST above GRAIN in one rail and had to state the order in words.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/true);

	const float circleX = static_cast<float>(p.panel->circleRailForTest().x());
	const float grainX = static_cast<float>(p.panel->grainRailForTest().x());
	const float postX = static_cast<float>(p.panel->postRailForTest().x());
	CHECK(circleX < grainX);
	CHECK(grainX < postX);
	// AND THEY DO NOT OVERLAP — three rails placed from one running edge, so an arithmetic slip puts
	// one on top of another rather than leaving a gap that a left-to-right check would still pass.
	CHECK(grainX >= circleX + static_cast<float>(p.panel->circleRailForTest().width()));
	CHECK(postX >= grainX + static_cast<float>(p.panel->grainRailForTest().width()));
	// AND THE OUTERMOST ONE ENDS AT THE PANEL'S EDGE, which is what says the sum spent everything it
	// claimed rather than leaving a strip of panel behind the rails.
	CHECK(postX + static_cast<float>(p.panel->postRailForTest().width())
	      == doctest::Approx(static_cast<float>(p.panel->width())));

	// **THE SPLIT'S WHOLE COST, AS A NUMBER: the panel is one `kWidth` wider than it was.** Read off
	// `railsWidth()`, which is the sum `minimumSize` adds to the grid and the sum `resized` spends, so
	// this is the one arithmetic and not a restatement of it.
	const float bothOpen = p.panel->railsWidth();
	setLanes(p, /*circleOpen=*/true, /*grainOpen=*/false, /*postOpen=*/true);
	CHECK(bothOpen - p.panel->railsWidth()
	      == doctest::Approx(ui::FxLaneRail::kWidth - ui::Rail::kCollapsedWidth));
}
TEST_CASE("the two fx rails collapse INDEPENDENTLY, each keeping its own 34px strip") {
	// **THE THIRD OF THE THREE THINGS THE SPLIT WAS SUPPOSED TO BUY**, and the one that is structural
	// rather than arithmetic: `collapsed_` is per-`Rail` and `railsWidth()` sums `railWidth()`, so a
	// player who wants only the grain lane pays 34px for the post lane and nothing else. Driven across
	// all FOUR combinations rather than at the two obvious ones, because a shared bool passes both of
	// those.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	ui::FxLaneRail& post = p.panel->postRailForTest();

	for (const bool wantGrain : { false, true }) {
		for (const bool wantPost : { false, true }) {
			CAPTURE(wantGrain);
			CAPTURE(wantPost);
			setLanes(p, /*circleOpen=*/false, wantGrain, wantPost);
			CHECK(grain.collapsed() == !wantGrain);
			CHECK(post.collapsed() == !wantPost);
			// **A CLOSED RAIL IS MINIMISED, NEVER GONE** — the toggle that closed it has to be there to
			// open it, so it keeps a strip. Read off the APPLIED width, not off `railWidth()`, which is
			// the number the panel was told rather than the one it used.
			CHECK(static_cast<float>(grain.width())
			      == doctest::Approx(wantGrain ? ui::FxLaneRail::kWidth : ui::Rail::kCollapsedWidth));
			CHECK(static_cast<float>(post.width())
			      == doctest::Approx(wantPost ? ui::FxLaneRail::kWidth : ui::Rail::kCollapsedWidth));
		}
	}
}
TEST_CASE("each fx rail edits its OWN lane — GRAIN FX reaches the grain chain, POST FX the post chain") {
	// **TWO RAILS OVER ONE HANDLE, AND THE FAILURE THIS CATCHES IS TWO RAILS OVER ONE CHAIN.** The
	// panel asks `containerSurfaceOf` once and hands the same pointer to both rails; each casts it
	// through its own lane's accessor (`FxLaneRail::bridgeIn`). A rail built with the wrong `Lane`, or
	// a `bridgeIn` that ignored it, draws two columns that mirror each other and looks almost right.
	HeldChain held;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	ui::FxLaneRail& post = p.panel->postRailForTest();
	setRails(p, false, true);
	REQUIRE(grain.lane() == ui::FxLaneRail::Lane::Grain);
	REQUIRE(post.lane() == ui::FxLaneRail::Lane::Post);

	REQUIRE(grain.follow(held.surface()));
	REQUIRE(post.follow(held.surface()));
	REQUIRE(grain.columnForTest() != nullptr);
	REQUIRE(post.columnForTest() != nullptr);
	// TWO BRIDGES, EACH THE ONE ITS LANE OWNS. Addresses compared in ONE pass against the fixture's own
	// chains, which is the comparison CLAUDE.md's ABA rule permits.
	CHECK(grain.bridgeForTest() == &held.grainBridge());
	CHECK(post.bridgeForTest() == &held.bridge());
	CHECK(grain.bridgeForTest() != post.bridgeForTest());

	// **AND THE CONTENTS ARE THE CLAIM, because two distinct addresses do not prove two distinct
	// chains** — one chain reached through two bridge objects satisfies every line above.
	const char* tag = firstAddableTag(*post.columnForTest());
	REQUIRE(tag != nullptr);
	const DeviceInstanceId inPost = post.columnForTest()->addDevice(tag);
	REQUIRE(inPost != feathers::kNoInstance);
	CHECK(post.columnForTest()->deviceCount() == 1);
	CHECK(grain.columnForTest()->deviceCount() == 0); // the line that catches one chain twice

	const DeviceInstanceId inGrain = grain.columnForTest()->addDevice(tag);
	REQUIRE(inGrain != feathers::kNoInstance);
	CHECK(post.columnForTest()->deviceCount() == 1);
	CHECK(grain.columnForTest()->deviceCount() == 1);

	// **THE IDS DIFFER, AND THAT IS GAP G3 — THE THING THE WHOLE ARRANGEMENT RESTS ON.** Both chains
	// issue ids from ONE `backplane::InstanceCounter` (`Sklitter::laneIds_`). If they did not, both
	// lanes would name their first device 1, and since both rails carry the SAME `DeviceNest` — one
	// holder, one nest — `ExpandedOverlay::keyOf` (`title#path:device`) would give the two devices ONE
	// key. Opening a panel in one lane would then retire the panel about a different device in the
	// other, which is the SKLITTER-inside-SKLITTER bug arriving by a new road. **Splitting one rail
	// into two frames did not change this and must not be read as having changed it.**
	CHECK(inGrain != inPost);

	// AND THE CAPTIONS FOLLOW THE LANE, not the position — asked of each rail rather than restated.
	CHECK(std::string(grain.captionForTest()) == ui::FxLaneRail::kGrainLine);
	CHECK(std::string(post.captionForTest()) == ui::FxLaneRail::kPostLine);
	CHECK(grain.labelForTest() == ui::FxLaneRail::kGrainLabel);
	CHECK(post.labelForTest() == ui::FxLaneRail::kPostLabel);
}

namespace {
/// **A CARD IN THIS COLUMN WHOSE CONTROLS A CASE CAN ASK `display()` OF.** `GenericCard` is the one
/// that exposes them (`controlForTest`); a device with a bespoke card keeps its controls to itself.
/// Returned with the device id because a control is addressed by its device.
struct ColumnControl {
	feathers::DeviceInstanceId device = feathers::kNoInstance;
	ui::DeviceControl* control = nullptr;
};
ColumnControl firstGenericDestinationIn(ui::ChainColumn& column) {
	for (const device::Descriptor* desc : column.addable()) {
		size_t modulatable = desc->params.size();
		for (size_t i = 0; i < desc->params.size(); ++i) {
			if (desc->params[i].modulatable) {
				modulatable = i;
				break;
			}
		}
		if (modulatable == desc->params.size()) {
			continue; // nothing here can be a destination at all
		}
		const feathers::DeviceInstanceId id = column.addDevice(desc->traits.tag);
		if (id == feathers::kNoInstance) {
			continue;
		}
		auto* card = dynamic_cast<ui::GenericCard*>(column.cardOf(id));
		if (card == nullptr) {
			column.removeDevice(id);
			continue;
		}
		return { id, const_cast<ui::DeviceControl*>(&card->controlForTest(modulatable)) };
	}
	return {};
}
} // namespace

TEST_CASE("each fx rail has its OWN modulation state — an arm in one does not light the other, and an "
          "OUTER arm lights both, which is asserted as the GLOW and not as the flag") {
	// **THIS CASE'S HEADING SAID "LIGHTS" AND ITS ASSERTIONS SAID `armed()`, AND A HAND FOUND THE
	// DIFFERENCE BEFORE THE SUITE DID** (backlog HM, 2026-09-21: arming a Perform modulation did not
	// make the grain fx params glow). Every assertion below the arm was
	// true while the glow was dead: the flag really did reach both lanes and `tellChildren()` really did
	// repaint them, but the glow is `ModDisplay::routable`, which is `armed && mayRoute(source(), dest)`
	// — and `source()` answered an endpoint the inner gate must refuse (trap T1). **A heading is a claim
	// about a quantity, and the quantity in this one was never read.** So the glow half is now asserted
	// as the glow, off a real control in a real chain, and the flag half stays beside it.
	//
	// **A CLAIM ABOUT STATE, SO IT IS ASSERTED AS STATE** (CLAUDE.md's oldest rule): whether a control
	// would light is not recoverable from a drawing, and the failure here is a PARTIAL one — the lights
	// come on, they are simply on in a lane that cannot take the route — which is exactly where a
	// difference check is blind.
	//
	// **WHY IT MATTERS RATHER THAN BEING TIDINESS.** An arm is an offer to route, and a route runs
	// through ONE `rack::InnerChain`'s authority: a post-lane control cannot read a grain-lane
	// modulator's output, because the two lanes are two routing graphs. A shared state would light
	// every control in BOTH rails whenever a modulator in either was armed, and half those lights would
	// be controls volunteering for a route the engine refuses — the honesty rule failing in the place
	// it is most expensive to notice, because it only shows up when you click.
	//
	// The split into two rails makes this structural rather than careful — each rail owns its state by
	// construction — but the SHARING of the outer state is still a line that can be deleted, so both
	// halves are driven.
	// **THE OUTER STATE AND ITS HISTORY ARE DECLARED BEFORE THE PANEL, SO THEY OUTLIVE IT — and the
	// first draft of this case declared them after, which is a real use-after-free.** Going through
	// `SklitterExpanded::setModulation` rather than the rail's own `setOuterModulation` reaches the
	// GRID's controls too, and a control WATCHES the `EditHistory` it is handed. Unfollowing at the end
	// with `setModulation(nullptr, &outerHistory)` clears the modulation parent and leaves those
	// watchers exactly where they were — so an `EditHistory` declared after the panel dies with
	// watchers still on it.
	//
	// **`ctest` WAS GREEN AND THE SANITIZER GATE WAS NOT.** `FrameWatchers::~FrameWatchers` asserts on
	// this, and that assertion is compiled out of the ordinary build — so the case passed 19 of 19
	// assertions and then aborted in a destructor, which is precisely the class CLAUDE.md keeps the
	// gate for. The same ordering note guards `HeldChain` against `PerformPanel`, and it is why the
	// chain is declared FIRST here: the rails hold columns over ITS bridges, so a chain destroyed
	// before the panel leaves two columns reading a dead bridge.
	HeldChain held;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::FxLaneRail& grainRail = p.panel->grainRailForTest();
	ui::FxLaneRail& postRail = p.panel->postRailForTest();
	ui::ModState& grain = grainRail.modForTest();
	ui::ModState& post = postRail.modForTest();
	REQUIRE(&grain != &post);
	setRails(p, false, true);

	// A LOCAL ARM IN ONE LANE STAYS THERE.
	REQUIRE_FALSE(grain.armed());
	REQUIRE_FALSE(post.armed());
	(void)grain.arm(true);
	CHECK(grain.armed());
	CHECK_FALSE(post.armed()); // the whole case, in one line
	(void)grain.arm(false);
	CHECK_FALSE(grain.armed());

	// **AND THE OUTER ARM REACHES BOTH, which is the half a "keep them apart" fix breaks.** The outer
	// source is the one thing a control in EITHER chain can genuinely route from
	// (`kPostChainAxes.coreSources = kPerformOutputs`, SKLITTER's own seven), so an outer arm has to
	// light both rails. A panel that parented only one would leave the other's controls unable to
	// volunteer for `Perf X` — silently, and only under assign mode. (Declared at the top of the case;
	// see the ordering note there for why that is not a style choice.)
	p.armFrom();
	// **ARMED AT SKLITTER'S OWN OUTPUT, WHICH IS WHAT THE PERFORM PANE'S CHIPS ARM** — the source this
	// case's heading is really about. It used to arm whatever the outer state happened to be focused on,
	// which is `core(0)` by construction, and a Feathers core source reaches an inner control by the
	// ordinary outer road — so the glow below would have been green for a reason that has nothing to do
	// with the seven the sentence names.
	const backplane::Source own = backplane::Source::output(p.panel->watchedDevice(), 0);
	p.outer.focus(own);
	(void)p.outer.arm(true);
	CHECK(grain.armed());
	CHECK(post.armed());
	// AND NEITHER IS ARMED LOCALLY — they are FOLLOWING, not copying, which is what lets a local arm
	// still take precedence.
	CHECK_FALSE(grain.armedLocally());
	CHECK_FALSE(post.armedLocally());

	// **AND NOW THE WORD "LIGHTS", READ OFF A CONTROL.** A chain behind each rail, a real card in each,
	// and the control's own `routable` — the branch a knob's `draw()` takes. This is the assertion the
	// heading always promised and never made; with the boundary spelling reverted it reads false while
	// every `armed()` above stays true, which is precisely the mutation this case used to survive.
	REQUIRE(grainRail.follow(held.surface()));
	REQUIRE(postRail.follow(held.surface()));
	REQUIRE(grainRail.columnForTest() != nullptr);
	REQUIRE(postRail.columnForTest() != nullptr);
	const ColumnControl inGrain = firstGenericDestinationIn(*grainRail.columnForTest());
	const ColumnControl inPost = firstGenericDestinationIn(*postRail.columnForTest());
	REQUIRE(inGrain.control != nullptr);
	REQUIRE(inPost.control != nullptr);
	// NOT VACUOUS: both CAN be destinations, so a false below is the gate's answer rather than "there
	// was nothing to route to".
	REQUIRE(inGrain.control->isDestination());
	REQUIRE(inPost.control->isDestination());
	CHECK(inGrain.control->display().routable);
	CHECK(inPost.control->display().routable);

	// UNFOLLOWED, BOTH GO DARK — the assertion that stops the ones above being satisfied by two states
	// that were armed for some other reason. **Asserted on the GLOW too**, so "dark" means what the
	// heading means by "lights".
	p.panel->setModulation(nullptr, &p.outerHistory);
	CHECK_FALSE(grain.armed());
	CHECK_FALSE(post.armed());
	CHECK_FALSE(inGrain.control->display().routable);
	CHECK_FALSE(inPost.control->display().routable);
}
TEST_CASE("backlog HM: an arm on SKLITTER's OWN perform output reaches the controls inside its lanes — "
          "spelled as the chain's own core source, because the outer spelling is one the gate refuses") {
	// **THE DEFECT: Perform's modulations reached only inner modulation destinations, and the grain fx
	// params did not glow.**
	//
	// **THE CASE ABOVE ASSERTS `armed()` AND PASSES, AND THE REPORT IS RIGHT ANYWAY** — which is CLAUDE.md's
	// commonest wrong claim, an unaffected STORED FLAG offered as unaffected BEHAVIOUR. The flag really
	// does reach both lanes and `tellChildren()` really does repaint them. What decides the glow is
	// `ModDisplay::routable`, and that is `armed && host.mayRoute(mod->source(), dest)` — so the
	// question is what `source()` ANSWERS inside a chain, not whether the arm arrived.
	//
	// **IT ANSWERED A SOURCE THE INNER GATE MUST REFUSE.** A following state spells its parent's source
	// `asOuter()`; the perform pane's chips arm SKLITTER's OWN output, so the inner controls volunteered
	// for `outer(output(sklitter, n))`, and `backplane::mayRoute` sends an outer source to
	// `devices.outerUpstream` → `RackEngine::upstreamOf(self, output(self, n))`, which is FALSE for all
	// seven because `Graph::reaches(from, to)` is true when `from == to`. That is trap T1, pinned by
	// three cases in `tests/test_rack_nesting.cpp` and named in `sklitter.cpp`'s `kPostChainAxes`
	// comment; it is cited here rather than re-proved, because proving it needs an outer host and this
	// fixture has none.
	//
	// **AND THE SIGNAL WAS INSIDE ALL ALONG, UNDER ANOTHER NAME.** `kPostChainAxes.coreSources ==
	// kPerformOutputs` and the device publishes `publishCoreSource(n, performOutput(n))` into both
	// lanes — so inside either chain Perf n IS `Source::core(n)`. The fix is the SPELLING at the
	// boundary, and it is the honest one because the knob WRITES through `mod_->source()` too: the
	// control lights for the same endpoint the drag then routes, through the authority that runs it.
	HeldChain held;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	setRails(p, false, true);
	p.armFrom();
	REQUIRE(grain.follow(held.surface()));
	ui::ChainColumn* column = grain.columnForTest();
	REQUIRE(column != nullptr);

	const ColumnControl seated = firstGenericDestinationIn(*column);
	REQUIRE(seated.control != nullptr);
	// **NOT VACUOUS IN THE DIRECTION THAT MATTERS**: this control CAN be a destination, so a `routable`
	// of false below is the gate's answer and not "there was nothing to route to".
	REQUIRE(seated.control->isDestination());
	CHECK_FALSE(seated.control->display().routable); // nothing armed yet

	// **THE ARM THE PERFORM CHIP MAKES** — `SklitterExpanded::setModulation` hands the circle rail the
	// OUTER state, so its chips arm SKLITTER's own output on it. Driven through the state rather than
	// through the chip so the claim is about the road, not about a rectangle.
	const backplane::Source own = backplane::Source::output(p.panel->watchedDevice(), 0);
	p.outer.focus(own);
	(void)p.outer.arm(true);
	REQUIRE(grain.modForTest().armed());

	// **THE SPELLING, AS A VALUE.** A drawing could not say this and neither could `armed()`.
	CHECK(grain.modForTest().source() == backplane::Source::core(0));
	// AND THE AUTHORITY THAT WILL RUN THE ROUTE AGREES — asked of the bridge the knob writes through,
	// so the glow cannot be true of one authority and the route of another.
	CHECK(held.grainBridge().mayRoute(backplane::Source::core(0),
	                                  backplane::Destination::control(seated.device,
	                                                                  seated.control->control())));
	// **AND THE CONTROL LIGHTS** — the effect it was reported missing, read off the control rather than
	// off a frame.
	CHECK(seated.control->display().routable);

	// **AND THE PRESS WRITES THE ROUTE THE GLOW PROMISED, WHICH IS THE HONESTY RULE CLOSED RATHER THAN
	// HALF-CLOSED.** `DeviceControl::setAmount` routes through `mod_->source()` — the same spelling
	// `display()` lit from — so a build where the two disagreed would light a control and then write
	// somewhere else. Asserted as the AMOUNT read back from the authority, not as "something changed".
	seated.control->beginAmountGesture();
	seated.control->setAmount(0.4f);
	seated.control->endAmountGesture();
	CHECK(held.grainBridge().routeAmount(backplane::Source::core(0),
	                                     backplane::Destination::control(seated.device,
	                                                                     seated.control->control()))
	      == doctest::Approx(0.4f));
	// AND NOWHERE ELSE — the outer spelling holds no route, which is what says the write did not merely
	// land somewhere plausible.
	CHECK(held.grainBridge().routeAmount(own.asOuter(),
	                                     backplane::Destination::control(seated.device,
	                                                                     seated.control->control()))
	      == doctest::Approx(0.0f));

	// **THE SPELLING IS SELECTIVE, WHICH IS THE HALF A BLANKET `core(index)` WOULD ALSO SATISFY.** A
	// source of the outer host that is NOT this holder's own output is still an OUTER source in here —
	// a Feathers core source reaches an inner control by the outer road, and must keep doing so.
	p.outer.focus(backplane::Source::core(0));
	CHECK(grain.modForTest().source() == backplane::Source::core(0).asOuter());
	CHECK(grain.modForTest().source().isOuter());

	// AND BOTH LANES, because the holder publishes the same seven into each and a fix reaching one rail
	// is this panel's standing failure mode (`forEachFxRail` exists for it).
	ui::FxLaneRail& post = p.panel->postRailForTest();
	REQUIRE(post.follow(held.surface()));
	p.outer.focus(own);
	CHECK(post.modForTest().source() == backplane::Source::core(0));
}
TEST_CASE("backlog HS: an arm on SKLITTER's own perform output lights the panel's MAIN GRID — every "
          "modulatable control EXCEPT the puck the outputs are made of — and the press writes that route") {
	// The main controls, other than those that drive Perform, sit downstream of Perform so Perform
	// can modulate them.
	//
	// **THE GRID IS NOT INSIDE A LANE**, so none of backlog HM's spelling applies: these controls watch
	// the OUTER state directly (`GenericExpanded::setModulation`), the armed source really is
	// `output(self, n)`, the destination really is `control(self, k)`, and the authority is Feathers'
	// own record. What made every one of them dark was trap T1 — a device is not upstream of itself —
	// and what lights them is the device's declaration (`device::SelfRouting`), read by that authority.
	//
	// **`routable` IS ASSERTED, NOT `armed()`** — the HM defect was exactly a case asserting the flag
	// while the glow was false.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	p.armFrom();

	const DeviceInstanceId self = p.panel->watchedDevice();
	REQUIRE(self != kNoInstance);
	// THE SET, RESTATED FROM THE MOD ROAD rather than read off the declaration: `setPerformFields` reads
	// the puck and the engine's publish reads the bypass.
	const auto feeds = [](size_t control) {
		return control == static_cast<size_t>(sc::Param::PerfX) || control == static_cast<size_t>(sc::Param::PerfY)
		       || control == static_cast<size_t>(sc::Param::PerfDepth) // decided 2026-09-22
		       || control == static_cast<size_t>(sc::Param::Enabled);
	};

	for (size_t control = 0; control < sc::kDescriptor.params.size(); ++control) {
		CAPTURE(std::string(sc::kDescriptor.params[control].name));
		CHECK_FALSE(p.panel->controlForTest(control).display().routable); // nothing armed yet
	}

	size_t lit = 0;
	size_t darkForFeeding = 0;
	for (size_t output = 0; output < sc::kPerformOutputs; ++output) {
		const backplane::Source own = backplane::Source::output(self, output);
		p.outer.focus(own);
		(void)p.outer.arm(true);
		REQUIRE(p.outer.armed());
		for (size_t control = 0; control < sc::kDescriptor.params.size(); ++control) {
			CAPTURE(output);
			CAPTURE(std::string(sc::kDescriptor.params[control].name));
			const bool modulatable = sc::kDescriptor.params[control].modulatable;
			const bool expected = modulatable && !feeds(control);
			// THE GLOW, AND THE AUTHORITY BEHIND IT — asked separately so neither can be true alone.
			CHECK(p.panel->controlForTest(control).display().routable == expected);
			CHECK(p.f.bridge.mayRoute(own, backplane::Destination::control(self, control)) == expected);
			lit += expected ? 1 : 0;
			darkForFeeding += modulatable && feeds(control) ? 1 : 0;
		}
	}
	// NEITHER HALF VACUOUS: Perf X, Perf Y and Perf Depth (decided 2026-09-22) are destinations
	// for everything else and dark for these.
	// FOUR SINCE BACKLOG KW: the enable is a destination too, and a self-route into it is refused (the publish gates on it).
	CHECK(darkForFeeding == 4 * sc::kPerformOutputs);
	CHECK(lit == (sc::kDescriptor.modulatableCount() - 4) * sc::kPerformOutputs);

	// **AND THE PRESS WRITES THE ROUTE THE GLOW PROMISED** — Perf X into Rate, read back from the
	// authority as the AMOUNT. And the refused one writes nothing.
	const backplane::Source perfX = backplane::Source::output(self, sc::outIndex(sc::Out::X));
	p.outer.focus(perfX);
	ui::DeviceControl& rate = p.panel->controlForTest(static_cast<size_t>(sc::Param::Rate));
	rate.beginAmountGesture();
	rate.setAmount(0.4f);
	rate.endAmountGesture();
	CHECK(p.f.bridge.routeAmount(perfX, backplane::Destination::control(self, static_cast<size_t>(sc::Param::Rate)))
	      == doctest::Approx(0.4f));

	ui::DeviceControl& puck = p.panel->controlForTest(static_cast<size_t>(sc::Param::PerfX));
	puck.beginAmountGesture();
	puck.setAmount(0.4f);
	puck.endAmountGesture();
	CHECK(p.f.bridge.routeAmount(perfX, backplane::Destination::control(self, static_cast<size_t>(sc::Param::PerfX)))
	      == doctest::Approx(0.0f));

	// A SOURCE THAT IS NOT THIS DEVICE'S OWN STILL REACHES THE PUCK — the refusal is about the loop, not
	// about Perf X being a destination.
	p.outer.focus(backplane::Source::core(0));
	CHECK(puck.display().routable);
}
TEST_CASE("backlog HM: SKLITTER's chains declare exactly as many core sources as it has perform "
          "outputs — the premise the spelling above rests on") {
	// **THE ASSUMPTION, PINNED WHERE IT CAN ROT.** The spelling maps the holder's output `n` onto
	// `Source::core(n)` INSIDE the chain, and that is legal only because `kPostChainAxes.coreSources ==
	// kPerformOutputs` and `publishCoreSource(n, performOutput(n))` fills them in that order. Both live
	// in `sklitter.cpp`, which this rail cannot see; if either changes, the spelling starts naming the
	// wrong signal and nothing else in this suite would notice.
	//
	// **IT IS NOT A GENERAL RULE AND MUST NOT BECOME ONE.** A nested rack's chain declares EIGHT core
	// sources and they are its MACROS — inferring the mapping from `coreSources` alone would map a
	// rack's output 0 onto Macro 1. That is why `FxLaneRail` states it rather than deriving it.
	HeldChain held;
	CHECK(held.bridge().axes().coreSources == feathers::fx::sklitter::kPerformOutputs);
	CHECK(held.grainBridge().axes().coreSources == feathers::fx::sklitter::kPerformOutputs);
	// NOT VACUOUS: seven is a real number of real outputs, so the equality is a claim rather than two
	// zeroes agreeing.
	CHECK(feathers::fx::sklitter::kPerformOutputs >= 7u);
	CHECK(feathers::fx::sklitter::kDescriptor.outputCount() == feathers::fx::sklitter::kPerformOutputs);
}
TEST_CASE("backlog HM: a param inside a chain column offers the sources routed into it — the column "
          "says what this chain has, and the menu could not before") {
	// **THE DEFECT: right-clicking a param in the fx lane did not show its mapped mod sources' context
	// menu.**
	//
	// **THE SAME OMISSION AS THE MISSING OUTPUT ARMS, ONE DOOR ALONG — and this suite had already
	// written it down as a known gap** (the "closed end two" note on the case above): `ChainColumn` was
	// extracted out of `RackEditor`'s card half and neither passenger came with it. With no
	// `setSourceLister`, `ModState::sources()` is empty; `inboundSources` walks that list, so it could
	// never find a route however many were live; and `showRouteMenu` refuses an empty menu so the press
	// falls through silently. **A capability that exists in the engine and cannot be reached from the
	// surface it is sitting on** — the honesty rule from the other side.
	//
	// **ASSERTED AT `inboundSources`, WHICH IS THE GATE `showRouteMenu` READS.** A popup needs a window
	// and synthesising the click is banned on the development display, which is exactly why `mod_route_menu.h`
	// splits the verbs from the popup; the list is the claim and the popup is a `PopupMenu::show`.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.grainBridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);

	// **THE HOLDER'S OWN SEVEN ARE LISTED**, which is the half the older case recorded as shut.
	const std::vector<backplane::Source> listed = mod.sources();
	REQUIRE_FALSE(listed.empty());
	for (size_t i = 0; i < feathers::fx::sklitter::kPerformOutputs; ++i) {
		CAPTURE(i);
		CHECK(std::find(listed.begin(), listed.end(), backplane::Source::core(i)) != listed.end());
	}

	// A MODULATOR TO ROUTE FROM AND A DESTINATION TO ROUTE INTO — both real devices of THIS chain, so
	// the route below is one the chain's own authority accepts rather than one the test asserts about.
	const device::Descriptor* publisher = nullptr;
	for (const device::Descriptor* d : column.addable()) {
		if (d->outputCount() > 0) {
			publisher = d;
			break;
		}
	}
	REQUIRE(publisher != nullptr);
	const DeviceInstanceId source = column.addDevice(publisher->traits.tag);
	REQUIRE(source != feathers::kNoInstance);
	const ColumnControl seated = firstGenericDestinationIn(column);
	REQUIRE(seated.control != nullptr);
	REQUIRE(seated.control->isDestination());

	// **NOTHING ROUTED, NOTHING OFFERED** — the false side, so the list below is not simply everything.
	CHECK(ui::inboundSources(*seated.control).empty());

	const backplane::Source from = backplane::Source::output(source, 0);
	const backplane::Destination into =
	    backplane::Destination::control(seated.device, seated.control->control());
	REQUIRE(held.grainBridge().mayRoute(from, into)); // else the write below proves nothing
	held.grainBridge().setRouteAmount(from, into, 0.5f);
	REQUIRE(held.grainBridge().routeAmount(from, into) == doctest::Approx(0.5f));

	// **AND THE MENU NOW HAS THE ROUTE IN IT** — by ENDPOINT, which is the assertion a count could not
	// make: a list that named the wrong device would be the same length.
	const std::vector<device::Source> inbound = ui::inboundSources(*seated.control);
	REQUIRE(inbound.size() == 1);
	CHECK(inbound.front() == from);

	// **AND IT IS NAMED AFTER THE DEVICE, not after an ordinal** — the row is read before it is picked.
	const std::string named = mod.sourceName(from);
	CHECK(named.find(publisher->traits.name) != std::string::npos);
	// A CORE SOURCE WITH NO HOLDER TO NAME IT FALLS BACK RATHER THAN GUESSING — this bare column has no
	// `setCoreSourceNamer`; the panel's rail installs one, which the next case reads.
	CHECK(mod.sourceName(backplane::Source::core(0)) == "Source 1");
}
TEST_CASE("backlog HM: the rail names this chain's core sources after SKLITTER's own outputs") {
	// **A ROW YOU HAVE TO PICK TO FIND OUT WHAT IT WAS IS NOT A MENU.** The column knows the devices IN
	// the chain and cannot know the device the chain is IN, so the holder's surface says; `FxLaneRail`
	// is that surface and `sklitter::kOutputs` is the table. Asserted as the STRING, because the whole
	// value of the row is what it reads.
	HeldChain held;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::FxLaneRail& grain = p.panel->grainRailForTest();
	setRails(p, false, true);
	p.armFrom();
	REQUIRE(grain.follow(held.surface()));
	REQUIRE(grain.columnForTest() != nullptr);
	for (size_t i = 0; i < feathers::fx::sklitter::kPerformOutputs; ++i) {
		CAPTURE(i);
		CHECK(grain.modForTest().sourceName(backplane::Source::core(i))
		      == std::string(feathers::fx::sklitter::kOutputs[i].name));
	}
	// NOT VACUOUS: the names are distinct, so seven agreements are seven facts rather than one repeated.
	CHECK(std::string(feathers::fx::sklitter::kOutputs[0].name) != std::string(feathers::fx::sklitter::kOutputs[1].name));
}
TEST_CASE("backlog HM: folding a card in a chain column moves the cards below it — BOTH fold roads, "
          "because the claim is the LAYOUT and not which hook happens to be installed") {
	// **THE QUESTION WAS PUT TO ME AS A FACT AND THE FACT WAS TRUE OF THE FIELD AND FALSE OF THE
	// EFFECT**, which is this repo's commonest wrong claim (CLAUDE.md). The observation was exact:
	// `FxLane` installs `onFoldChanged` and `ChainColumn` and `RackEditor` install ZERO. The conclusion
	// drawn from it — that folding a card in a chain does not re-place the cards, so the old defect —
	// collapsing an audio effect hid its controls but left the card's size alone — is still live
	// inside SKLITTER — does NOT follow, because both of those hosts install `onHeadFold` instead and
	// their handlers call `resized()` in so many words. **An absent hook is not an absent behaviour.**
	//
	// **SO THE CASE IS WRITTEN AGAINST THE LAYOUT, WHICH IS WHAT THE USER CAN SEE**, and it drives BOTH
	// roads into `FxCard::setFolded` rather than the one that happens to be wired today:
	//
	//   - `onHeadFold`, which the head's double-click calls when a host installs one. This is the road
	//     a chain column really takes, and it was already right.
	//   - `setFolded` DIRECTLY, which is the fallback `FxCard` takes when a host installs no
	//     `onHeadFold` (fx_card.h's *"the card's own `folded_` IS the state there"), and the road any
	//     future card that folds itself would take. Nothing re-placed the column on that one, and
	//     `onFoldChanged` is what closes it — so the invariant now holds however the fold arrives
	//     rather than because one caller remembered.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.grainBridge(), registry, mod, history, nullptr);
	// TALL ENOUGH THAT NEITHER CARD IS SCROLLED OUT — a hidden card moves for a different reason.
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);

	const char* tag = firstAddableTag(column);
	REQUIRE(tag != nullptr);
	const DeviceInstanceId first = column.addDevice(tag);
	const DeviceInstanceId second = column.addDevice(tag);
	REQUIRE(first != feathers::kNoInstance);
	REQUIRE(second != feathers::kNoInstance);
	ui::FxCard* top = column.cardOf(first);
	ui::FxCard* below = column.cardOf(second);
	REQUIRE(top != nullptr);
	REQUIRE(below != nullptr);
	REQUIRE_FALSE(top->folded());
	// NOT VACUOUS: the second card really is BELOW the first, so "moved up" is a thing that can happen.
	REQUIRE(below->bounds().y() > top->bounds().y());

	const auto foldAndMeasure = [&](const std::function<void()>& fold) {
		const float belowBefore = below->bounds().y();
		const float topBefore = static_cast<float>(top->height());
		fold();
		CHECK(top->folded());
		// THE CARD ITSELF SHRANK — without this a column that moved nothing would still "pass" the
		// comparison below by having nothing to move.
		const float given = topBefore - static_cast<float>(top->height());
		CHECK(given > 0.0f);
		// **AND THE ONE UNDER IT MOVED UP BY EXACTLY WHAT THE TOP CARD GAVE UP.** An independent
		// restatement rather than "it moved": a layout that re-placed the stack wrongly would still
		// move it, which is precisely where a difference check is blind.
		CHECK(belowBefore - below->bounds().y() == doctest::Approx(given));
	};

	SUBCASE("through the head fold the host installs — the road a chain column really takes") {
		REQUIRE(static_cast<bool>(top->onHeadFold));
		foldAndMeasure([&] { top->onHeadFold(); });
	}

	SUBCASE("and through setFolded, which no host handler sits on") {
		// **THE ROAD THAT WAS OPEN.** `FxCard::setFolded` fires `onFoldChanged` and nothing else; with
		// no installer the column never heard, so the card shrank and the stack stayed where it was —
		// the exact shape of the original report, one surface along.
		foldAndMeasure([&] { top->setFolded(true); });
	}
}
TEST_CASE("backlog HM: one hand, one arm — arming anywhere in the follow-tree clears every other arm, "
          "and the per-lane states stay separate") {
	// **THE DEFECT: a modulator in the fx lane and a modulator on Perform could be assigning at the same
	// time.** Two glowing surfaces for one gesture: the
	// next press routes from ONE source and nothing on screen says which.
	//
	// **THE FIX IS NOT "SHARE THE STATE", AND THIS CASE IS WRITTEN TO FAIL IF SOMEBODY LATER DOES
	// THAT.** Per-lane `ModState` is deliberate and `chain_column.h` argues it at length: an arm is an
	// offer to route, a route runs inside one `InnerChain`'s authority, and one shared state would light
	// controls volunteering for routes the engine refuses. So the assertions below are in two halves —
	// the arms are mutually exclusive, AND the states are still three distinct objects whose sources are
	// their own.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	p.armFrom();
	ui::ModState& grain = p.panel->grainRailForTest().modForTest();
	ui::ModState& post = p.panel->postRailForTest().modForTest();
	REQUIRE(&grain != &post);
	REQUIRE(grain.parentForTest() == &p.outer);

	// A LOCAL ARM IN THE GRAIN LANE — as a chip on a card in that column makes.
	(void)grain.arm(true);
	REQUIRE(grain.armedLocally());

	// **AND NOW THE PERFORM PANE'S, WHICH IS THE OUTER STATE.** Before the fix both stood: the outer
	// answered armed for its own reason and the grain lane for its own, so both surfaces glowed.
	(void)p.outer.arm(true);
	CHECK(p.outer.armedLocally());
	CHECK_FALSE(grain.armedLocally()); // the whole case, in one line

	// **THE GRAIN LANE IS STILL LIT — BY FOLLOWING, WHICH IS A DIFFERENT FACT AND THE HONEST ONE.** It
	// is not armed on its own account; it answers the outer's arm, so its controls volunteer for the
	// OUTER source and a second press cannot mean two things.
	CHECK(grain.armed());
	CHECK(post.armed());
	CHECK(grain.source() == p.outer.source().asOuter());

	// **AND THE OTHER DIRECTION**, because a fix that only cleared downward would leave the outer
	// glowing behind an inner arm — the same two surfaces, the other way round.
	(void)post.arm(true);
	CHECK(post.armedLocally());
	CHECK_FALSE(p.outer.armedLocally());
	CHECK_FALSE(grain.armedLocally());
	// AND THE SIBLING LANE GOES DARK WITH IT, rather than following an outer arm that is no longer on.
	CHECK_FALSE(grain.armed());

	// **STILL THREE STATES WITH THREE FOCUSES** — the half a shared `ModState` would also satisfy, and
	// the reason this is a clearing rather than a merge.
	post.focus(backplane::Source::core(3));
	p.outer.focus(backplane::Source::core(1));
	CHECK(post.source() == backplane::Source::core(3));
	CHECK(p.outer.source() == backplane::Source::core(1));
	// AND THREE WATCHER LISTS, which is what a merge would have collapsed into one.
	CHECK(&post.watchers() != &grain.watchers());
	CHECK(&post.watchers() != &p.outer.watchers());

	// **DISARMING CASCADES NOTHING.** Taking your own arm off is not an instruction to anybody else; a
	// cascade there would let `arm(false)` silently clear a surface the hand never touched.
	(void)p.outer.arm(true);
	REQUIRE(p.outer.armedLocally());
	REQUIRE_FALSE(post.armedLocally());
	(void)p.outer.arm(false);
	CHECK_FALSE(post.armedLocally()); // it was already off; nothing turned it ON either
	CHECK_FALSE(grain.armed());

	// **ARMING WHILE ALREADY ARMED TELLS NOBODY AND CLEARS NOBODY** — `arm`'s own zero-return contract,
	// asserted here because the clearing walk is new code on that path.
	(void)grain.arm(true);
	REQUIRE(grain.armedLocally());
	CHECK(grain.arm(true) == 0u);
	CHECK(grain.armedLocally());

	// **A BORROWED ARM CLEARS NOBODY, AND THIS HALF WAS FOUND BY AN EXISTING CASE RATHER THAN BY
	// READING.** `removeAllInbound` arms a state for the length of a walk so that `setAmount` writes the
	// right source, then puts it back; with exclusivity on every arm, picking "Remove all modulators"
	// in an inner panel silently took the OUTER state out of assign mode — which
	// `test_rack_editor.cpp`'s DI-across-the-boundary case asserts against in so many words (*"while the
	// outer stays armed as it was") and went red the moment the rule was added. An arm is exclusive
	// because it is a HAND's; the editor's own borrow is not one.
	(void)grain.arm(false);
	(void)p.outer.arm(true);
	REQUIRE(p.outer.armedLocally());
	(void)post.arm(true, ui::ModState::Arm::Borrowed);
	CHECK(post.armedLocally());
	CHECK(p.outer.armedLocally()); // the line the rack editor's case was really making
	(void)post.arm(false, ui::ModState::Arm::Borrowed);
	CHECK(p.outer.armedLocally());
}
TEST_CASE("backlog HM: exclusivity reaches exactly as far as the follow-tree — the ordinary Feathers "
          "editor has no tree to walk, and two editors in one process are two trees") {
	// **THE BLAST-RADIUS CLAIM, ASSERTED RATHER THAN REASONED TO.** `mod_state.h` is shared by
	// `ModPanel`, `RackEditor` and both chain surfaces, so "an arm anywhere clears every other" is a
	// change to all of them. The defence offered for it was *"there is only one root in Feathers'
	// editor" — which is a claim about the SHAPE of a live object graph, and the kind of claim this
	// repo has been wrong about before by reading rather than asking.
	//
	// **WHAT MAKES IT SAFE IS STRUCTURAL AND IS READ OFF THE STATE**: `arm` walks to `root()` and
	// recurses into `children_`, so a state with no parent and no children reaches nobody and is reached
	// by nobody. The editor's own state is exactly that until an inner-chain surface parents itself to
	// it, which only `FxLaneRail::setOuterModulation` and `ChainExpanded::setModulation` ever do.
	SUBCASE("the real editor's state is a lone node, so an arm there can clear nothing") {
		ModFixture f;
		ui::ModState* editor = f.gui.modForTest();
		REQUIRE(editor != nullptr);
		// THE SHAPE, FROM THE LIVE EDITOR — not from a constructed `ModState`, which would be a lone
		// node whatever the editor does.
		CHECK(editor->parentForTest() == nullptr);
		CHECK(editor->childCountForTest() == 0u);
		// AND THE BEHAVIOUR THAT FOLLOWS FROM IT: arming is what it always was, including `arm`'s own
		// zero-return for a flag that did not move.
		(void)editor->arm(true);
		CHECK(editor->armedLocally());
		CHECK(editor->arm(true) == 0u);
		CHECK(editor->armedLocally());
		(void)editor->arm(false);
		CHECK_FALSE(editor->armed());
	}

	SUBCASE("two unrelated states are two trees — arming one clears nothing in the other") {
		// **THE CASE THAT IS NOT HYPOTHETICAL**: Feathers' editor and a rack editor run in one process,
		// each with its own `ModState` and no parent link between them. If exclusivity reached across
		// them, opening assign mode in one window would silently close it in another.
		ui::ModState a;
		ui::ModState b;
		REQUIRE(a.parentForTest() == nullptr);
		REQUIRE(b.parentForTest() == nullptr);
		(void)a.arm(true);
		(void)b.arm(true);
		CHECK(a.armedLocally());
		CHECK(b.armedLocally()); // the whole subcase, in one line
	}

	SUBCASE("and a tree of three is one tree — the far corner is cleared, not just the near one") {
		// THE POSITIVE CONTROL for the two above: with a parent link the walk really does reach, so the
		// negatives are about the SHAPE and not about a cascade that never runs.
		ui::ModState outer;
		ui::ModState left;
		ui::ModState right;
		left.setParent(&outer);
		right.setParent(&outer);
		REQUIRE(outer.childCountForTest() == 2u);
		(void)left.arm(true);
		REQUIRE(left.armedLocally());
		// ARMED ON THE FAR SIBLING, cleared on the near one — through the root, which is the walk.
		(void)right.arm(true);
		CHECK(right.armedLocally());
		CHECK_FALSE(left.armedLocally());
		// AND FROM THE ROOT DOWNWARD TOO.
		(void)outer.arm(true);
		CHECK(outer.armedLocally());
		CHECK_FALSE(right.armedLocally());
		CHECK_FALSE(left.armedLocally());
	}
}

