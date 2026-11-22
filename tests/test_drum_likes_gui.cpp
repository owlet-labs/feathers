/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "fx/drum/drum.h"
#include "fx/drum/drum_likes.h"
#include "fx/drum/drum_types.h"
#include "persist_rig.h"
#include "plugin/ui/drum_device_expanded.h"
#include "plugin/ui/drum_like_surface.h"
#include "plugin/ui/widget_registry.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <visage/ui.h>

// THE DRUM WINDOW'S LIKES, THROUGH ITS REAL WIDGETS over a real rack: the heart likes where the drum stands and unlikes on the like; the
// marks are drawn on the plane and Kind they were made on; Mutate's roll is the dice's; the pad's press lands through the warp; and the
// controls the Drum has are the terrain's own (Zone, Offset, Gamma, Rotate, Travel, the zoom), with none the Drum cannot act on.

namespace dd = feathers::fx::drumdevice;
using namespace feathers;

namespace {

struct Window {
	persist::Rig rig;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::DrumDeviceExpanded> win;
	std::chrono::steady_clock::time_point clock = std::chrono::steady_clock::time_point(std::chrono::hours(1));
	visage::Canvas canvas;

	Window() {
		rig.seat(dd::kTraits.tag);
		rig.block();
		win = std::make_unique<ui::DrumDeviceExpanded>(*rig.bridge, registry, rig.id, nullptr);
		win->setBounds(0.0f, 0.0f, ui::DrumDeviceExpanded::kWidth, ui::DrumDeviceExpanded::kMinimumHeight);
		frame();
	}
	void set(dd::Param p, double v) { rig.bridge->setField(rig.id, static_cast<size_t>(p), static_cast<float>(v)); }
	/// One frame: the window follows the row, the pad draws (which re-anchors it on a moved row).
	void frame() {
		rig.block(2); // the engine publishes the row the window reads as played
		(void)win->updateContent();
		// THE KEY-REBUILD THROTTLE READS A PINNED CLOCK, a second per frame: a real one defers a rebuild that comes within 100 ms of the last,
		// and the case would then depend on how fast the machine ran it.
		clock += std::chrono::seconds(1);
		win->padForTest().setClockForTest(clock);
		win->padForTest().draw(canvas);
	}
	void stand(double x, double y) {
		set(dd::Param::TravelX, x);
		set(dd::Param::TravelY, y);
		frame();
	}
	std::vector<dd::DrumLike> stored() {
		std::vector<float> block(dd::kLikesBlockWords);
		std::vector<dd::DrumLike> likes;
		if (rig.bridge->blockAt(rig.id, dd::kLikesBlock, block)) {
			(void)dd::readLikeBlock(block.data(), block.size(), &likes);
		}
		return likes;
	}
	dd::Settings settings() {
		rig.block(2);
		return win->modelForTest().settings();
	}
	void press(visage::Frame& f, visage::Point at, bool right = false) {
		visage::MouseEvent e;
		e.position = at;
		e.button_id = right ? visage::kMouseButtonRight : visage::kMouseButtonLeft;
		e.button_state = e.button_id;
		e.is_down = true;
		f.onMouseDown().callback(e);
	}
};

} // namespace

TEST_CASE("drum likes window: the heart likes the drum where it stands, by value, and removes the like it stands on") {
	Window w;
	w.stand(0.5, -0.25);
	CHECK(w.stored().empty());
	CHECK_FALSE(w.win->padForTest().likeUnderStar().has_value());
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	const std::vector<dd::DrumLike> one = w.stored();
	REQUIRE(one.size() == 1);
	// WHAT IT STORED: the place as set, the plane, the Kind, and the fields the engine decodes there (restated through `fieldsOf`).
	const dd::Settings s = w.settings();
	CHECK(one[0].x == 0.5);
	CHECK(one[0].y == -0.25);
	CHECK(one[0].zone == s.index(dd::Param::Zone));
	CHECK(one[0].kind == static_cast<uint16_t>(s.index(dd::Param::Kind)));
	CHECK(one[0].fields == dd::fieldsArray(dd::fieldsOf(s)));
	// THE STAR STANDS ON IT NOW, and a second press removes it (not a second like).
	w.frame();
	REQUIRE(w.win->padForTest().likeUnderStar().has_value());
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	CHECK(w.stored().empty());
	// ELSEWHERE a like is a second like, not the removal of the first.
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(3.0, 2.0);
	REQUIRE_FALSE(w.win->padForTest().likeUnderStar().has_value());
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	CHECK(w.stored().size() == 2);
}

TEST_CASE("drum likes window: a like's mark is drawn on the plane and Kind it was made on, and nowhere else") {
	Window w;
	w.stand(0.5, -0.25);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(3.0, 2.0);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	REQUIRE(w.stored().size() == 2);
	w.stand(0.5, -0.25);
	w.frame();
	const int here = w.win->padForTest().likeMarksDrawn();
	CHECK(here >= 1); // (the second like may be off the pad's view)
	// THE MARK OF THE LIKE UNDER THE STAR IS UNDER THE PUCK.
	const visage::Point puck = w.win->padForTest().puck();
	const visage::Point mark = w.win->padForTest().likePixel(w.stored()[0]);
	CHECK(mark.x == doctest::Approx(puck.x).epsilon(1e-6));
	CHECK(mark.y == doctest::Approx(puck.y).epsilon(1e-6));
	// ANOTHER PLANE: Gamma moves the home every place is read at, so none of the marks is of this one.
	w.set(dd::Param::Gamma, 0.25);
	w.frame();
	w.frame();
	CHECK(w.win->padForTest().likeMarksDrawn() == 0);
	CHECK(w.win->likesForTest().applied().empty());
	// ANOTHER KIND, likewise; and back, both return.
	w.set(dd::Param::Gamma, 0.0);
	w.set(dd::Param::Kind, 2.0);
	w.frame();
	w.frame();
	CHECK(w.win->padForTest().likeMarksDrawn() == 0);
	w.set(dd::Param::Kind, 0.0);
	w.frame();
	w.frame();
	CHECK(w.win->padForTest().likeMarksDrawn() == here);
}

TEST_CASE("drum likes window: the heart's menu clears this Kind's likes only, and the strength is its own") {
	Window w;
	w.stand(0.5, -0.25);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.set(dd::Param::Kind, 2.0);
	w.frame();
	w.stand(0.5, -0.25);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	REQUIRE(w.stored().size() == 2);
	visage::PopupMenu menu = w.win->likesForTest().menu();
	menu.onSelection().callback(ui::DrumLikeBar::kClearLikes);
	const std::vector<dd::DrumLike> left = w.stored();
	REQUIRE(left.size() == 1);
	CHECK(left[0].kind == 0); // the Snare's like is gone, the Kick's stays
	CHECK(w.win->likesForTest().strength() == 1);
	menu.onSelection().callback(ui::DrumLikeBar::kLikeStrength + 4);
	CHECK(w.win->likesForTest().strength() == 4);
	CHECK(w.win->likesForTest().state() == nullptr); // (no like of the Snare's now: nothing applies)
	w.set(dd::Param::Kind, 0.0);
	w.frame();
	REQUIRE(w.win->likesForTest().state() != nullptr);
	CHECK(w.win->likesForTest().state()->strength == ui::kWarpStrength * 4.0);
}

TEST_CASE("drum likes window: Mutate with no likes rolls the dice's own step, and with likes the warped one, by value") {
	Window w;
	w.stand(1.0, 2.0);
	const double reach = w.win->likesForTest().reach();
	// THE DICE, RESTATED: the bar's xorshift from its seed, the first roll up and the second sideways.
	uint32_t d = 12345u;
	const auto next = [&d] {
		d ^= d << 13;
		d ^= d >> 17;
		d ^= d << 5;
		return static_cast<double>(d) / 4294967296.0 * 2.0 - 1.0;
	};
	const double up = next();
	const double sideways = next();
	w.win->likesForTest().seedDiceForTest(12345u);
	w.press(w.win->likesForTest(), { ui::DrumLikeBar::kHeartWidth + ui::DrumLikeBar::kGap + 5.0f, 5.0f });
	const dd::Settings after = w.settings();
	CHECK(after.at(dd::Param::TravelX) == static_cast<float>(1.0 + sideways * reach));
	CHECK(after.at(dd::Param::TravelY) == static_cast<float>(2.0 + up * reach));
	// WITH LIKES the roll is the warped one: the same two numbers, carried through the derived state.
	w.stand(0.9, -0.1);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(3.0, 2.0);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(0.9, -0.1);
	const auto state = w.win->likesForTest().state();
	REQUIRE(state != nullptr);
	d = 777u;
	const double up2 = next();
	const double sideways2 = next();
	const std::array<double, 2> want = ui::drumMutate(state.get(), static_cast<double>(0.9f), static_cast<double>(-0.1f), sideways2, up2, reach); // (the control holds a float)
	const std::array<double, 2> plain = { 0.9 + sideways2 * reach, -0.1 + up2 * reach };
	CHECK(want != plain); // not vacuous: the likes moved the roll
	w.win->likesForTest().seedDiceForTest(777u);
	w.press(w.win->likesForTest(), { ui::DrumLikeBar::kHeartWidth + ui::DrumLikeBar::kGap + 5.0f, 5.0f });
	const dd::Settings landed = w.settings();
	CHECK(landed.at(dd::Param::TravelX) == static_cast<float>(want[0]));
	CHECK(landed.at(dd::Param::TravelY) == static_cast<float>(want[1]));
}

TEST_CASE("drum likes window: a press on the pad lands through the warp, at the world's place the picture shows there") {
	Window w;
	w.stand(0.5, -0.25);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(3.0, 2.0);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(0.5, -0.25);
	ui::DrumDevicePad& pad = w.win->padForTest();
	const auto state = w.win->likesForTest().state();
	REQUIRE(state != nullptr);
	// A PRESS A QUARTER OF THE PAD FROM THE CENTRE (the star placed there, in the frame anchored on the like).
	const visage::Point centre = { pad.width() * 0.5f, pad.height() * 0.5f };
	const visage::Point at = { centre.x + 30.0f, centre.y - 20.0f };
	const auto view = pad.viewForTest();
	const double k = pad.planePerPixelNow();
	const double vx = view.first + 30.0 * k;
	const double vy = view.second + 20.0 * k; // UP IS MORE
	const std::array<double, 2> origin = { pad.frameOrigin()[0], pad.frameOrigin()[1] };
	// THE EXPECTED WORLD PLACE, from the derived warp directly (not the pad's policy).
	const ui::WarpPoint world = state->derivation.warp.worldAt({ (origin[0] + vx) * ui::kDrumWarpPerTravel, (origin[1] + vy) * ui::kDrumWarpPerTravel });
	const double wantX = world.x / ui::kDrumWarpPerTravel;
	const double wantY = world.y / ui::kDrumWarpPerTravel;
	w.press(pad, at);
	CHECK(w.settings().at(dd::Param::TravelX) == doctest::Approx(wantX).epsilon(1e-5));
	CHECK(w.settings().at(dd::Param::TravelY) == doctest::Approx(wantY).epsilon(1e-5));
	// NOT VACUOUS: the unwarped place would have been elsewhere.
	CHECK(std::hypot(wantX - (origin[0] + vx), wantY - (origin[1] + vy)) > 0.01);
}

TEST_CASE("drum likes window: the terrain controls are the Drum's own and none the Drum cannot act on is offered; Width follows the Kind") {
	Window w;
	// THE TERRAIN'S NAMES, as the synth's chart calls them (its corners say Gamma and Rotate; its pad has a zoom, and so has this one).
	const auto nameOf = [](dd::Param p) { return std::string(dd::kParamDescs[static_cast<size_t>(p)].name); };
	CHECK(nameOf(dd::Param::Gamma) == "Gamma");
	CHECK(nameOf(dd::Param::Rotate) == "Rotate");
	CHECK(nameOf(dd::Param::Zone) == "Zone");
	CHECK(nameOf(dd::Param::ZoneOffset) == "Offset");
	CHECK(ui::DrumDevicePad::zoomControlShown());
	// NO CONTROL OF THE SYNTH'S BOTTOM BAR THE DRUM HAS NO REAL PARAMETER FOR: not hidden by a gate, absent from the device.
	for (const device::ParamDesc& d : dd::kParamDescs) {
		for (const char* synthOnly : { "Path", "Relief", "Weather", "Pos X", "Pos Y" }) {
			CHECK(std::string(d.name) != synthOnly);
		}
	}
	// THE DRUM'S OWN WIDTH is the Snare's and the Clap's: hidden, not greyed, under the Kinds that do not read it.
	const auto* width = w.win->widgetForTest(dd::Param::Width);
	REQUIRE(width != nullptr);
	for (int kind = 0; kind < static_cast<int>(feathers::drum::kKindCount); ++kind) {
		w.set(dd::Param::Kind, kind);
		w.frame();
		const bool reads = kind == 2 || kind == 4;
		CHECK(width->isVisible() == reads);
	}
}

TEST_CASE("drum likes window: Clear empties the device's like store, the warp and the marks, and the empty store survives save and load") {
	Window w;
	w.stand(0.5, -0.25);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(3.0, 2.0);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.stand(0.5, -0.25);
	REQUIRE(w.stored().size() == 2);
	REQUIRE(w.win->likesForTest().state() != nullptr);
	// NOT VACUOUS: a patch saved now carries them.
	persist::Rig withLikes;
	withLikes.load(w.rig.save());
	withLikes.block();
	std::vector<float> got(dd::kLikesBlockWords);
	std::vector<dd::DrumLike> back;
	REQUIRE(withLikes.bridge->blockAt(withLikes.id, dd::kLikesBlock, got));
	REQUIRE(dd::readLikeBlock(got.data(), got.size(), &back));
	CHECK(back.size() == 2);
	// THE MENU'S CLEAR.
	visage::PopupMenu menu = w.win->likesForTest().menu();
	menu.onSelection().callback(ui::DrumLikeBar::kClearLikes);
	CHECK(w.stored().empty());
	CHECK(w.win->likesForTest().state() == nullptr);
	CHECK(w.win->likesForTest().applied().empty());
	w.frame();
	CHECK(w.win->padForTest().likeMarksDrawn() == 0);
	// THE WARP IS GONE FROM THE PAD'S MAP: a place of the view is the place of the world.
	const auto at = ui::DrumDevicePadPolicy::worldFromView(ui::DrumPadContext { w.settings(), w.win->likesForTest().state() }, { 0.0, 0.0 }, 0.7, -0.3);
	CHECK(at.first == 0.7);
	CHECK(at.second == -0.3);
	// AND THE EMPTY STORE IS WHAT A SAVE CARRIES.
	persist::Rig after;
	after.load(w.rig.save());
	after.block();
	std::vector<float> none(dd::kLikesBlockWords);
	std::vector<dd::DrumLike> left;
	const bool have = after.bridge->blockAt(after.id, dd::kLikesBlock, none);
	CHECK((!have || (dd::readLikeBlock(none.data(), none.size(), &left) && left.empty())));
}

TEST_CASE("drum likes window: a like in the corner under Rotate's box is drawn beside it, never under it, by the mark's own rectangle") {
	Window w;
	w.stand(0.5, -0.25);
	w.press(w.win->likesForTest(), { 5.0f, 5.0f });
	w.frame();
	ui::DrumDevicePad& pad = w.win->padForTest();
	const dd::DrumLike like = w.stored()[0];
	// THE WINDOW GAVE THE PAD ROTATE'S BOX: the inset is where the Rotate widget really is, in the pad's own coordinates.
	const visage::Frame* rotate = w.win->widgetForTest(dd::Param::Rotate);
	REQUIRE(rotate != nullptr);
	const visage::Bounds inset = pad.insetForTest();
	CHECK(inset.x() == doctest::Approx(rotate->x() - pad.x()));
	CHECK(inset.y() == doctest::Approx(rotate->y() - pad.y()));
	CHECK(inset.width() == doctest::Approx(rotate->width()));
	CHECK(inset.height() == doctest::Approx(rotate->height()));
	// A LIKE WHOSE PLACE IS UNDER THE BOX: the inset moved to cover the mark's own place.
	const visage::Point place = pad.likePixel(like);
	const auto overlaps = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() && a.right() > b.x() && a.y() < b.bottom() && a.bottom() > b.y();
	};
	pad.setInset({ place.x - 20.0f, place.y - 10.0f, 40.0f, 20.0f });
	REQUIRE(overlaps(pad.markRect(place), pad.insetForTest()));
	const visage::Point drawn = pad.markAt(place);
	CHECK_FALSE(overlaps(pad.markRect(drawn), pad.insetForTest()));
	CHECK(drawn.x == place.x); // moved straight down, not sideways
	// A MARK CLEAR OF THE BOX STAYS PUT.
	pad.setInset({ place.x + 60.0f, place.y - 10.0f, 40.0f, 20.0f });
	CHECK(pad.markAt(place).x == place.x);
	CHECK(pad.markAt(place).y == place.y);
}
