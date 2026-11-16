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

#include "fixtures.h"

#include "plugin/ui/rack_view_model.h"
#include "plugin/ui/rack_views.h"

// THE RACK'S TAB BAR AND PAD VIEW, over a fake model: tab counts and native widths, what a press does, the pad order and its pulse, the
// page strip, and a scale change. The model is the interface the Rack card will adapt its record to, so these hold whatever sits behind it.

namespace {

/// A control a case owns: the kit level and the playing chain are "real parameters" to the widgets, and this is all they need of one.
class OwnedControl final : public device::Modulatable {
public:
	double v = 0.0;
	std::vector<std::pair<std::string, double>> writes;
	const device::ParamDesc& desc() const override {
		static const device::ParamDesc kDesc { "owned", "Owned", 0.0, 7.0, 0.0, true, device::Unit::Number, {} };
		return kDesc;
	}
	double value() const override { return v; }
	void setValue(double value) override {
		v = value;
		writes.emplace_back("set", value);
	}
	device::ModDisplay display() const override { return {}; }
	void setAmount(float) override {}
	void beginGesture() override {}
	void endGesture() override {}
	std::string text() const override { return {}; }
};

class FakeRack final : public ui::RackViewModel {
public:
	ui::RackViewKind kind = ui::RackViewKind::Tabs;
	ui::RackRoutingKind route = ui::RackRoutingKind::Parallel;
	std::vector<ui::RackChainView> chains;
	size_t viewed = 0;
	size_t playing = 0;
	int pageNow = 0;
	std::array<bool, 16> open {};
	bool macros = false;
	ui::RackKeyMask struck {};
	bool noteRoad = true;
	std::vector<std::tuple<int, bool, float>> triggers;
	OwnedControl playingCtl;
	OwnedControl level;
	bool hasLevel = true;

	ui::RackViewKind view() const override { return kind; }
	ui::RackRoutingKind routing() const override { return route; }
	size_t chainSlots() const override { return chains.size(); }
	ui::RackChainView chain(size_t slot) const override { return slot < chains.size() ? chains[slot] : ui::RackChainView {}; }
	size_t viewedChain() const override { return viewed; }
	void viewChain(size_t slot) override { viewed = slot; }
	size_t playingChain() const override { return playing; }
	void setPlayingChain(size_t slot) override {
		playing = slot;
		playingCtl.setValue(static_cast<double>(slot));
	}
	device::Modulatable* playingControl() override { return &playingCtl; }
	int page() const override { return pageNow; }
	void setPage(int p) override { pageNow = p; }
	bool expanded(size_t slot) const override { return slot < open.size() && open[slot]; }
	void setExpanded(size_t slot, bool e) override {
		if (slot < open.size()) {
			open[slot] = e;
		}
	}
	bool macrosOpen() const override { return macros; }
	void setMacrosOpen(bool o) override { macros = o; }
	ui::RackKeyMask takeStruck() override {
		const ui::RackKeyMask out = struck;
		struck = {};
		return out;
	}
	bool canTrigger() const override { return noteRoad; }
	bool trigger(int key, bool on, float velocity) override {
		triggers.emplace_back(key, on, velocity);
		return true;
	}
	device::Modulatable* kitLevel() override { return hasLevel ? &level : nullptr; }

	/// `count` existing chains named A, B, ... with `key` = slot (pad keys), after `gaps` leading slots that do not exist.
	void fill(size_t count, size_t gaps = 0) {
		chains.clear();
		for (size_t i = 0; i < gaps; ++i) {
			chains.push_back({ false, "", ui::kChainAnyKey, 0 });
		}
		for (size_t i = 0; i < count; ++i) {
			chains.push_back({ true, std::string(1, static_cast<char>('A' + i)), static_cast<int>(i), 0 });
		}
	}
};

constexpr float kBar = ui::RackTabBar::kThickness;

} // namespace

TEST_CASE("rack tabs: one tab per existing chain, dividing the bar evenly in native pixels, in both flows and at two scales") {
	for (const ui::RackFlow flow : { ui::RackFlow::TopDown, ui::RackFlow::LeftRight }) {
		for (const float scale : { 1.0f, 1.75f }) {
			for (const size_t count : { 1u, 2u, 3u, 4u, 7u, 8u }) {
				FakeRack rack;
				rack.fill(count);
				visage::Frame root;
				root.setBounds(0.0f, 0.0f, 400.0f, 400.0f);
				root.setDpiScale(scale);
				ui::RackTabBar bar(rack, flow);
				root.addChild(&bar);
				if (flow == ui::RackFlow::TopDown) {
					bar.setBounds(0.0f, 0.0f, 261.0f, kBar); // a width that does not divide: the parts still sum to it
				}
				else {
					bar.setBounds(0.0f, 0.0f, kBar, 303.0f);
				}
				CAPTURE(static_cast<int>(flow));
				CAPTURE(scale);
				CAPTURE(count);
				const int total = flow == ui::RackFlow::TopDown ? bar.nativeWidth() : bar.nativeHeight();
				REQUIRE(bar.tabsForTest().size() == count);
				int sum = 0;
				int smallest = total;
				int largest = 0;
				int cursor = 0;
				for (size_t k = 0; k < count; ++k) {
					const auto [a, b] = bar.tabNativeExtent(k);
					CHECK(a == cursor); // the tabs touch: no gap, no overlap
					cursor = b;
					sum += b - a;
					smallest = std::min(smallest, b - a);
					largest = std::max(largest, b - a);
				}
				CHECK(sum == total);           // the bar is divided whole: never small tabs and a gap
				CHECK(largest - smallest <= 1); // equal within one native pixel
			}
		}
	}
}

TEST_CASE("rack tabs: a chain that does not exist gets no tab, and no more than eight are shown") {
	FakeRack rack;
	rack.fill(5, 2); // slots 0 and 1 do not exist
	rack.chains[4].exists = false;
	ui::RackTabBar bar(rack, ui::RackFlow::TopDown);
	bar.setBounds(0.0f, 0.0f, 240.0f, kBar);
	CHECK(bar.tabsForTest() == std::vector<size_t> { 2, 3, 5, 6 });
	rack.fill(12);
	CHECK(bar.tabsForTest().size() == 8);
	CHECK(bar.tabsForTest().back() == 7);
	rack.fill(0);
	CHECK(bar.tabsForTest().empty());
}

TEST_CASE("rack tabs: a press views that chain, and in Select the dot sets the playing chain and leaves the viewed one") {
	FakeRack rack;
	rack.fill(4, 1); // slots 1..4 exist
	rack.viewed = 1;
	rack.playing = 1;
	ui::RackTabBar bar(rack, ui::RackFlow::TopDown);
	bar.setBounds(0.0f, 0.0f, 260.0f, kBar);

	SUBCASE("Parallel routing: a press anywhere on a tab views it; there is no dot") {
		rack.route = ui::RackRoutingKind::Parallel;
		bar.pressForTest(bar.dotCentre(2)); // where the dot would be
		CHECK(rack.viewed == 3);
		CHECK(rack.playing == 1);
		CHECK_FALSE(bar.dotShown(0));
		CHECK_FALSE(bar.dotShown(2));
	}
	SUBCASE("Select routing") {
		rack.route = ui::RackRoutingKind::Select;
		bar.pressForTest(bar.tabCentre(2));
		CHECK(rack.viewed == 3); // the tab body views
		CHECK(rack.playing == 1);
		// THE DOT of another tab: playing moves, viewed does not, and the write went through the real parameter.
		const size_t before = rack.playingCtl.writes.size();
		bar.pressForTest(bar.dotCentre(3));
		CHECK(rack.playing == 4);
		CHECK(rack.viewed == 3);
		CHECK(rack.playingCtl.writes.size() == before + 1);
		CHECK(rack.playingCtl.v == 4.0);
		// THE TWO MARKS ARE DISTINCT: the playing chain wears the dot, the viewed one is the raised tab, and they differ here.
		CHECK(bar.dotShown(3));
		CHECK_FALSE(bar.dotShown(2));
		CHECK(bar.raised(2));
		CHECK_FALSE(bar.raised(3));
		// AND THE BAR DECLARES THE PLAYING CONTROL, which the window must offer too.
		std::vector<ui::ControlFace::Offered> offered;
		bar.offeredControls(offered);
		REQUIRE(offered.size() == 1);
		CHECK(offered[0].control == &rack.playingCtl);
	}
}

TEST_CASE("rack pads: the keys of a page run left to right then bottom to top, at the four corners of page 0 and page 5") {
	FakeRack rack;
	rack.kind = ui::RackViewKind::Pads;
	rack.fill(0);
	ui::RackStrikePulse pulse;
	ui::RackPadGrid grid(rack, pulse);
	grid.setBounds(0.0f, 0.0f, 204.0f, 204.0f);
	for (const int page : { 0, 5 }) {
		rack.pageNow = page;
		struct Corner {
			float x, y;
			int offset;
		};
		// A POINT NEAR EACH CORNER, written down here and not taken from the widget's own centres.
		for (const Corner c : { Corner { 5.0f, 199.0f, 0 }, Corner { 199.0f, 199.0f, 3 }, Corner { 5.0f, 5.0f, 12 }, Corner { 199.0f, 5.0f, 15 } }) {
			CAPTURE(page);
			CAPTURE(c.offset);
			rack.triggers.clear();
			grid.pressForTest({ c.x, c.y });
			grid.releaseForTest();
			REQUIRE(rack.triggers.size() == 2);
			CHECK(std::get<0>(rack.triggers[0]) == page * 16 + c.offset);
			CHECK(std::get<1>(rack.triggers[0]));   // note on
			CHECK_FALSE(std::get<1>(rack.triggers[1])); // note off, the same key
			CHECK(std::get<0>(rack.triggers[1]) == page * 16 + c.offset);
			CHECK(grid.padAt({ c.x, c.y }) == c.offset);
		}
	}
	// THE MIDDLE OF THE BOTTOM ROW AND THE ROW ABOVE IT: 0 1 2 3, then 4 5 6 7 above them.
	rack.pageNow = 0;
	CHECK(grid.padAt({ 70.0f, 199.0f }) == 1);
	CHECK(grid.padAt({ 70.0f, 140.0f }) == 5);
}

TEST_CASE("rack pads: a used pad shows its chain, an empty one its note, and a press triggers the key and views the chain") {
	FakeRack rack;
	rack.kind = ui::RackViewKind::Pads;
	rack.chains = { { true, "Kick", 0, 0 }, { true, "Snare", 2, 0 }, { false, "Gone", 3, 0 }, { true, "Far", 83, 0 } };
	rack.viewed = 0;
	ui::RackStrikePulse pulse;
	ui::RackPadGrid grid(rack, pulse);
	grid.setBounds(0.0f, 0.0f, 204.0f, 204.0f);
	CHECK(grid.usedForTest(0));
	CHECK(grid.labelForTest(0) == "Kick");
	CHECK(grid.usedForTest(2));
	CHECK(grid.labelForTest(2) == "Snare");
	// EMPTY PADS: a chain that does not exist does not light its key, and the pad shows the note (36 is C1 in this app's naming).
	CHECK_FALSE(grid.usedForTest(3));
	CHECK(grid.labelForTest(3) == "D#-2");
	CHECK_FALSE(grid.usedForTest(1));
	CHECK(grid.selectedForTest(0));
	CHECK_FALSE(grid.selectedForTest(2));
	// A PRESS ON THE SNARE'S PAD: triggers key 2 and views chain 1.
	grid.pressForTest(grid.padCentre(2));
	grid.releaseForTest();
	CHECK(rack.viewed == 1);
	REQUIRE(rack.triggers.size() == 2);
	CHECK(std::get<0>(rack.triggers[0]) == 2);
	CHECK(grid.selectedForTest(2));
	CHECK_FALSE(grid.selectedForTest(0));
	// A PRESS ON AN EMPTY PAD: the key plays, no chain is selected.
	rack.triggers.clear();
	grid.pressForTest(grid.padCentre(7));
	grid.releaseForTest();
	CHECK(rack.viewed == 1);
	CHECK(std::get<0>(rack.triggers[0]) == 7);
	// THE LATER PAGE: key 83 is pad 3 of page 5.
	rack.pageNow = 5;
	CHECK(grid.usedForTest(3));
	CHECK(grid.labelForTest(3) == "Far");
	CHECK_FALSE(grid.usedForTest(0));
	// WITHOUT A NOTE ROAD nothing is triggered, and the chain is still viewed.
	rack.noteRoad = false;
	rack.triggers.clear();
	grid.pressForTest(grid.padCentre(3));
	grid.releaseForTest();
	CHECK(rack.triggers.empty());
	CHECK(rack.viewed == 3);
}

TEST_CASE("rack pads: a struck key flashes its pad and its page cell, the poll reports the repaint, and the flash decays") {
	FakeRack rack;
	rack.chains = { { true, "Kick", 3, 0 } };
	double t = 100.0;
	ui::RackStrikePulse pulse;
	pulse.setReducedMotion(false);
	pulse.setClock([&] { return t; });
	ui::RackPadGrid grid(rack, pulse);
	grid.setBounds(0.0f, 0.0f, 204.0f, 204.0f);
	ui::RackPadPages pages(rack, pulse, ui::RackFlow::TopDown);
	pages.setBounds(0.0f, 0.0f, 208.0f, ui::RackPadPages::kThickness);
	// NOTHING STRUCK: nothing is told.
	CHECK(pulse.poll(rack) == 0);
	CHECK(grid.flashForTest(3) == 0.0f);
	// KEY 3 STRUCK: both watchers repainted (the count is the evidence), the flash is full and then decays linearly over 150 ms.
	maskSet(rack.struck, 3);
	CHECK(pulse.poll(rack) >= 2);
	CHECK(grid.flashForTest(3) == doctest::Approx(1.0f).epsilon(1e-4));
	CHECK(pages.cellFlashForTest(0, 3) == doctest::Approx(1.0f).epsilon(1e-4));
	CHECK(grid.flashForTest(2) == 0.0f); // only the struck key
	t = 100.075;
	CHECK(pulse.poll(rack) >= 2); // still lit: repainted
	CHECK(grid.flashForTest(3) == doctest::Approx(0.5f).epsilon(1e-3));
	t = 100.2;
	CHECK(pulse.poll(rack) >= 2); // the repaint that puts it out
	CHECK(grid.flashForTest(3) == 0.0f);
	t = 100.3;
	CHECK(pulse.poll(rack) == 0); // and then it is quiet
	// A STRIKE ON ANOTHER PAGE flashes that page's cell, not this page's pad.
	maskSet(rack.struck, 83);
	CHECK(pulse.poll(rack) >= 2);
	CHECK(pages.cellFlashForTest(5, 3) == doctest::Approx(1.0f).epsilon(1e-4));
	CHECK(grid.flashForTest(3) == 0.0f);
}

TEST_CASE("rack pads: under reduced motion a strike is one steady highlight until the next look, with no decay") {
	FakeRack rack;
	rack.chains = { { true, "Kick", 3, 0 } };
	double t = 5.0;
	ui::RackStrikePulse pulse;
	pulse.setReducedMotion(true);
	pulse.setClock([&] { return t; });
	ui::RackPadGrid grid(rack, pulse);
	grid.setBounds(0.0f, 0.0f, 204.0f, 204.0f);
	maskSet(rack.struck, 3);
	CHECK(pulse.poll(rack) >= 1);
	CHECK(grid.flashForTest(3) == 1.0f);
	t = 9.0; // time passes and the highlight does not fade
	CHECK(grid.flashForTest(3) == 1.0f);
	CHECK(pulse.poll(rack) >= 1); // the next look: nothing struck, so the highlight goes out, with a repaint
	CHECK(grid.flashForTest(3) == 0.0f);
	CHECK(pulse.poll(rack) == 0);
}

TEST_CASE("rack pads: the page strip lights the keys that have a chain, marks the current page, and a press changes it, in both flows") {
	for (const ui::RackFlow flow : { ui::RackFlow::TopDown, ui::RackFlow::LeftRight }) {
		CAPTURE(static_cast<int>(flow));
		FakeRack rack;
		rack.chains = { { true, "Kick", 0, 0 }, { true, "Snare", 2, 0 }, { true, "Far", 83, 0 }, { true, "Edge", 127, 0 } };
		ui::RackStrikePulse pulse;
		visage::Frame root;
		root.setBounds(0.0f, 0.0f, 400.0f, 400.0f);
		root.setDpiScale(1.75f);
		ui::RackPadPages pages(rack, pulse, flow);
		root.addChild(&pages);
		if (flow == ui::RackFlow::TopDown) {
			pages.setBounds(0.0f, 0.0f, 203.0f, ui::RackPadPages::kThickness);
		}
		else {
			pages.setBounds(0.0f, 0.0f, ui::RackPadPages::kThickness, 203.0f);
		}
		// EIGHT EVEN PARTS of the strip, native.
		int sum = 0;
		int smallest = 1 << 20;
		int largest = 0;
		for (int p = 0; p < 8; ++p) {
			const auto [a, b] = pages.pageNativeExtent(p);
			sum += b - a;
			smallest = std::min(smallest, b - a);
			largest = std::max(largest, b - a);
		}
		CHECK(sum == (flow == ui::RackFlow::TopDown ? pages.nativeWidth() : pages.nativeHeight()));
		CHECK(largest - smallest <= 1);
		// USED CELLS: keys 0 and 2 on page 0, 83 on page 5 (cell 3), 127 on page 7 (cell 15).
		CHECK(pages.cellUsedForTest(0, 0));
		CHECK(pages.cellUsedForTest(0, 2));
		CHECK_FALSE(pages.cellUsedForTest(0, 1));
		CHECK(pages.cellUsedForTest(5, 3));
		CHECK(pages.cellUsedForTest(7, 15));
		CHECK_FALSE(pages.cellUsedForTest(4, 3));
		// THE CURRENT PAGE is marked and a press elsewhere moves it.
		CHECK(pages.currentForTest(0));
		for (const int page : { 5, 7, 0, 3 }) {
			CAPTURE(page);
			REQUIRE(pages.pageAt(pages.pageCentre(page)) == page);
			pages.pressForTest(pages.pageCentre(page));
			CHECK(rack.pageNow == page);
			CHECK(pages.currentForTest(page));
			CHECK_FALSE(pages.currentForTest((page + 1) % 8));
		}
	}
}

TEST_CASE("rack views: a scale change re-cuts the native cells (the layout is logical and set again)") {
	FakeRack rack;
	rack.fill(5);
	rack.kind = ui::RackViewKind::Pads;
	ui::RackStrikePulse pulse;
	visage::Frame root;
	root.setBounds(0.0f, 0.0f, 600.0f, 600.0f);
	ui::RackPadsPanel panel(rack, pulse, ui::RackFlow::TopDown);
	ui::RackTabBar bar(rack, ui::RackFlow::LeftRight);
	root.addChild(&panel);
	root.addChild(&bar);
	panel.setBounds(0.0f, 0.0f, panel.naturalSize().x, panel.naturalSize().y);
	bar.setBounds(300.0f, 0.0f, kBar, 301.0f);
	for (const float scale : { 1.75f, 1.0f, 1.5f }) {
		root.setDpiScale(scale);
		panel.resized();
		CAPTURE(scale);
		// THE GRID'S NATIVE SIZE FOLLOWS THE SCALE and its four columns are even within a pixel and sum to it.
		CHECK(std::abs(static_cast<float>(panel.gridForTest().nativeWidth()) - scale * ui::RackPadsPanel::gridSide()) <= 1.0f);
		int sum = 0;
		int lo = 1 << 20;
		int hi = 0;
		for (int c = 0; c < 4; ++c) {
			const auto [a, b] = panel.gridForTest().columnNativeExtent(c);
			sum += b - a;
			lo = std::min(lo, b - a);
			hi = std::max(hi, b - a);
		}
		CHECK(sum == panel.gridForTest().nativeWidth());
		CHECK(hi - lo <= 1);
		// THE TAB BAR'S EXTENTS sum to the native height at every scale.
		int tabs = 0;
		for (size_t k = 0; k < 5; ++k) {
			const auto [a, b] = bar.tabNativeExtent(k);
			tabs += b - a;
		}
		CHECK(tabs == bar.nativeHeight());
	}
}

TEST_CASE("rack pads: the panel holds the grid, the strip across the flow and the kit level pill, which exists only with a level control") {
	for (const ui::RackFlow flow : { ui::RackFlow::TopDown, ui::RackFlow::LeftRight }) {
		FakeRack rack;
		rack.fill(3);
		ui::RackStrikePulse pulse;
		ui::RackPadsPanel panel(rack, pulse, flow);
		panel.setBounds(0.0f, 0.0f, panel.naturalSize().x, panel.naturalSize().y);
		REQUIRE(panel.levelForTest() != nullptr);
		// THE LEVEL PILL IS BOUND TO THE KIT LEVEL and declares it on its face.
		std::vector<ui::ControlFace::Offered> offered;
		panel.levelForTest()->offeredControls(offered);
		REQUIRE(offered.size() == 1);
		CHECK(offered[0].control == &rack.level);
		// THE STRIP LIES ACROSS THE FLOW: a row under the grid top-down, a column beside it left-right, and it covers the grid's extent.
		const visage::Bounds g = panel.gridForTest().bounds();
		const visage::Bounds p = panel.pagesForTest().bounds();
		if (flow == ui::RackFlow::TopDown) {
			CHECK(p.y() >= g.bottom());
			CHECK(p.width() == doctest::Approx(g.width()));
		}
		else {
			CHECK(p.x() >= g.right());
			CHECK(p.height() == doctest::Approx(g.height()));
		}
	}
	FakeRack bare;
	bare.hasLevel = false;
	ui::RackStrikePulse pulse;
	ui::RackPadsPanel panel(bare, pulse, ui::RackFlow::TopDown);
	CHECK(panel.levelForTest() == nullptr);
}

TEST_CASE("rack views: the expand arrows toggle through the model, and point down or right while closed") {
	FakeRack rack;
	rack.fill(2);
	for (const ui::RackFlow flow : { ui::RackFlow::TopDown, ui::RackFlow::LeftRight }) {
		rack.open = {};
		rack.macros = false;
		ui::RackDrawerArrow chain(flow, "Open the chain", [&] { return rack.expanded(0); }, [&](bool o) { rack.setExpanded(0, o); });
		ui::RackDrawerArrow macros(flow, "Macros", [&] { return rack.macrosOpen(); }, [&](bool o) { rack.setMacrosOpen(o); });
		CHECK_FALSE(rack.expanded(0)); // collapsed by default
		CHECK_FALSE(rack.macrosOpen());
		chain.toggleForTest();
		CHECK(rack.expanded(0));
		CHECK_FALSE(rack.macrosOpen()); // the two arrows are independent
		macros.toggleForTest();
		CHECK(rack.macrosOpen());
		chain.toggleForTest();
		CHECK_FALSE(rack.expanded(0));
	}
}
