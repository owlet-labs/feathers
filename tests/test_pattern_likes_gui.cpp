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

#include "mod/arp/pattern_likes.h"
#include "mod/arp/pattern_types.h"
#include "persist_rig.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/pattern_expanded.h"
#include "plugin/ui/widget_registry.h"
#include "rack/rack_bridge.h"

#include <chrono>
#include <string>
#include <vector>
#include <visage/ui.h>

// THE PATTERN WINDOW'S LIKES held to the Drum's standard: the heart on a like removes it, Clear (the heart's menu) empties the focused
// pattern's likes only and survives a save, each is one undo step, and no like mark hides under a box lying in the map.

namespace pt = feathers::mod::arp::pat;
namespace lk = feathers::mod::arp::likes;
using namespace feathers;
using Field = pt::Field;

namespace {

struct Window {
	persist::Rig rig;
	ui::WidgetRegistry registry;
	ui::EditHistory history;
	std::unique_ptr<ui::PatternExpanded> win;
	visage::Canvas canvas;

	Window() {
		rig.seat(pt::kTraits.tag);
		rig.block();
		win = std::make_unique<ui::PatternExpanded>(*rig.bridge, registry, rig.id, nullptr);
		win->setBounds(0.0f, 0.0f, 900.0f, 700.0f);
		win->setFocus(1);
		win->terrain().setModulation(nullptr, &history);
		frame();
	}
	/// The window's own focus change re-binds the terrain to ITS history (null here), so the case's history is bound again.
	void focus(size_t pattern) {
		win->setFocus(pattern);
		win->terrain().setModulation(nullptr, &history);
	}
	void set(size_t control, double v) { rig.bridge->setField(rig.id, control, static_cast<float>(v)); }
	void frame() {
		rig.block(2);
		(void)win->updateContent();
		(void)win->terrain().poll();
		win->terrain().draw(canvas);
	}
	void stand(double x, double y) {
		set(pt::at(1, Field::PlaceX), x);
		set(pt::at(1, Field::PlaceY), y);
		frame();
	}
	lk::AllLikes stored() {
		std::vector<float> block(lk::kBlockWords);
		lk::AllLikes all {};
		if (rig.bridge->blockAt(rig.id, pt::kLikesBlock, block)) {
			(void)lk::read(block.data(), block.size(), &all);
		}
		return all;
	}
};

} // namespace

TEST_CASE("pattern likes: the heart on a like removes it, elsewhere it likes, and each is one undo step") {
	Window w;
	ui::PatternTerrain& t = w.win->terrain();
	w.set(pt::at(1, Field::Zone), 512.0);
	w.stand(0.1, 0.05);
	CHECK_FALSE(t.likeUnderStar().has_value());
	REQUIRE(t.toggleLike());
	CHECK(w.stored()[1].count == 1);
	w.frame();
	REQUIRE(t.likeUnderStar().has_value());
	// ON THE LIKE: a second press is the removal, not a second like.
	REQUIRE(t.toggleLike());
	CHECK(w.stored()[1].count == 0);
	// ELSEWHERE it is a like again.
	REQUIRE(t.toggleLike());
	CHECK(w.stored()[1].count == 1);
	// EACH WAS ONE UNDO STEP, walked back one at a time: the like, the unlike, the like.
	w.history.undo();
	CHECK(w.stored()[1].count == 0);
	w.history.undo();
	CHECK(w.stored()[1].count == 1);
	w.history.undo();
	CHECK(w.stored()[1].count == 0);
	w.history.redo();
	CHECK(w.stored()[1].count == 1);
}

TEST_CASE("pattern likes: Clear empties the focused pattern's likes only, drops its marks, undoes, and the empty set survives a save") {
	Window w;
	ui::PatternTerrain& t = w.win->terrain();
	w.set(pt::at(1, Field::Zone), 512.0);
	w.stand(0.1, 0.05);
	REQUIRE(t.like());
	w.stand(-0.2, 0.1);
	REQUIRE(t.like());
	w.focus(2);
	w.set(pt::at(2, Field::Zone), 300.0);
	w.frame();
	REQUIRE(t.like());
	w.focus(1);
	w.frame();
	REQUIRE(w.stored()[1].count == 2);
	REQUIRE(w.stored()[2].count == 1);
	// THE MENU: Clear is offered (not greyed) and says whose likes it clears.
	visage::PopupMenu menu = t.likesMenu();
	menu.onSelection().callback(ui::PatternTerrain::kClearLikes);
	CHECK(w.stored()[1].count == 0);
	CHECK(w.stored()[2].count == 1); // another pattern's stay
	CHECK(t.likeMarks().empty());
	w.frame();
	CHECK(t.likeMarksDrawnForTest() == 0);
	// NOTHING TO CLEAR is not an edit.
	CHECK_FALSE(t.clearLikes());
	// ONE UNDO STEP puts both back.
	w.history.undo();
	CHECK(w.stored()[1].count == 2);
	w.history.redo();
	// AND A SAVE CARRIES THE CLEARED SET.
	persist::Rig back;
	back.load(w.rig.save());
	back.block();
	std::vector<float> block(lk::kBlockWords);
	lk::AllLikes all {};
	REQUIRE(back.bridge->blockAt(back.id, pt::kLikesBlock, block));
	REQUIRE(lk::read(block.data(), block.size(), &all));
	CHECK(all[1].count == 0);
	CHECK(all[2].count == 1);
}

TEST_CASE("pattern likes: a like mark is moved out from under a box lying in the map, by the mark's own rectangle") {
	Window w;
	ui::PatternTerrain& t = w.win->terrain();
	REQUIRE_FALSE(t.insetsForTest().empty()); // the window gave it the heart, Rotate and the foot's boxes
	const auto overlaps = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() && a.right() > b.x() && a.y() < b.bottom() && a.bottom() > b.y();
	};
	for (const visage::Bounds& box : t.insetsForTest()) {
		const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
		REQUIRE(overlaps(t.markRect(centre), box));
		const visage::Point drawn = t.markAt(centre);
		for (const visage::Bounds& other : t.insetsForTest()) {
			CHECK_FALSE(overlaps(t.markRect(drawn), other));
		}
		CHECK(drawn.x == centre.x); // straight up or down
	}
	// A MARK CLEAR OF EVERY BOX STAYS PUT.
	const visage::Point middle { static_cast<float>(t.width()) * 0.5f, static_cast<float>(t.height()) * 0.5f };
	bool clear = true;
	for (const visage::Bounds& box : t.insetsForTest()) {
		clear = clear && !overlaps(t.markRect(middle), box);
	}
	REQUIRE(clear);
	CHECK(t.markAt(middle).y == middle.y);
}
