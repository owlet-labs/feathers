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
#include "menu_placement_check.h"

#include <chrono>
#include <filesystem>
#include <unistd.h>
#include <set>
#include <thread>

#include "fx/drum/drum_types.h"
#include "plugin/preset_store.h"
#include "plugin/ui/chain_column.h"
#include "plugin/ui/lane_selection.h"
#include "plugin/ui/text_field.h"
#include "plugin/ui/drum_device_card.h"
#include "plugin/device_registry.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/parallel_rack_card.h"
#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/rack_view_adapter.h"
#include "plugin/ui/rack_view_model.h"
#include "plugin/ui/rack_views.h"
#include "plugin/ui_note_ring.h"
#include "rack/fragment.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"

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
	/// THE ENGINE'S ONE MASK, when several models stand for one rack (the card's and the window's): a look takes it, whoever looks.
	std::shared_ptr<ui::RackKeyMask> source;
	ui::RackStrikeKey rackKey {};
	bool follow = false;
	OwnedControl followCtl;
	ui::RackStrikeKey strikeKey() const override { return rackKey; }
	bool followPlayed() const override { return follow; }
	device::Modulatable* followControl() override { return &followCtl; }
	ui::RackKeyMask takeStruck() override {
		ui::RackKeyMask& from = source ? *source : struck;
		const ui::RackKeyMask out = from;
		from = {};
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
	grid.setBounds(0.0f, 0.0f, 196.0f, 196.0f);
	for (const int page : { 0, 5 }) {
		rack.pageNow = page;
		struct Corner {
			float x, y;
			int offset;
		};
		// A POINT NEAR EACH CORNER, written down here and not taken from the widget's own centres.
		for (const Corner c : { Corner { 5.0f, 191.0f, 0 }, Corner { 191.0f, 191.0f, 3 }, Corner { 5.0f, 5.0f, 12 }, Corner { 191.0f, 5.0f, 15 } }) {
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
	CHECK(grid.padAt({ 70.0f, 191.0f }) == 1);
	CHECK(grid.padAt({ 70.0f, 140.0f }) == 5);
}

TEST_CASE("rack pads: a used pad shows its chain, an empty one its note, and a press triggers the key and views the chain") {
	FakeRack rack;
	rack.kind = ui::RackViewKind::Pads;
	rack.chains = { { true, "Kick", 0, 0 }, { true, "Snare", 2, 0 }, { false, "Gone", 3, 0 }, { true, "Far", 83, 0 } };
	rack.viewed = 0;
	ui::RackStrikePulse pulse;
	ui::RackPadGrid grid(rack, pulse);
	grid.setBounds(0.0f, 0.0f, 196.0f, 196.0f);
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
	grid.setBounds(0.0f, 0.0f, 196.0f, 196.0f);
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
	grid.setBounds(0.0f, 0.0f, 196.0f, 196.0f);
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

//==================================================================================================
// OVER A REAL RACK: the card and the window in each view
//==================================================================================================

namespace {

namespace pr = feathers::rack::parallel;

/// A rack on the lane, as the devices suite's step-4 cases build one, with the card and the window laid out.
struct RealRack {
	ModFixture f;
	feathers::rack::RackEngine bodies { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	void* body = nullptr;
	feathers::DeviceInstanceId id = feathers::kNoInstance;
	size_t slot = 0;
	ui::ParallelRackCard* card = nullptr;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::ParallelRackExpanded* panel = nullptr;
	bool initialClosed_ = false;

	RealRack() {
		bodies.prepare(48000.0, 4096);
		const size_t at = bodies.addDevice(pr::kTraits.tag);
		REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
		body = bodies.slotAtLane(at).storage.get();
		REQUIRE(body != nullptr);
		slot = f.bridge.addFxDevice(pr::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		id = f.bridge.fxDeviceAt(slot);
		f.bridge.fxBodies().attach(id, body);
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		lane->resized();
		card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);
		view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pr::kDescriptor);
		panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->setBounds(0.0f, 0.0f, 1200.0f, std::max(panel->minimumSize().y, 700.0f));
		refresh();
	}
	~RealRack() {
		view.reset();
		f.bridge.fxBodies().detach(id, body);
		(void)f.bridge.removeFxDevice(slot);
	}
	pr::Rack& rack() const { return *pr::rackOf(body); }
	void set(pr::Param p, float v) { f.deviceHost.setField(id, static_cast<size_t>(p), v); }
	float field(pr::Param p) const { return f.deviceHost.fieldAt(id, static_cast<size_t>(p)); }
	void refresh() {
		(void)card->update();
		(void)panel->update();
		card->resized();
		panel->resized();
		(void)card->update();
		(void)panel->update();
	}
	/// Chains up to `count`, the first `withDevices` holding a Drum then a Delay, each keyed to its own index when `keyed`.
	void chains(size_t count, size_t withDevices, bool keyed = false) {
		if (!initialClosed_ && rack().chainCount() >= 1) {
			rack().setOpen(0, false); // the rack's own first chain, closed as the cases were written (see below)
			initialClosed_ = true;
		}
		while (rack().chainCount() < count) {
			const size_t made = rack().addChain();
			REQUIRE(made != SIZE_MAX);
			// THESE CASES WERE WRITTEN WITH A CLOSED CHAIN (the front shows its first card): a new chain starts open now, so the helper closes it.
			rack().setOpen(made, false);
		}
		for (size_t i = 0; i < count; ++i) {
			if (keyed) {
				rack().setChainKey(i, static_cast<int>(i));
			}
			if (i < withDevices) {
				feathers::rack::RackBridge& b = *rack().chainAt(i)->bridge();
				if (b.chainLength() == 0) {
					(void)b.addDevice("DRUM");
					(void)b.addDevice("DELAY");
				}
			}
		}
		refresh();
	}
	void view_(int v) {
		set(pr::Param::View, static_cast<float>(v));
		refresh();
	}
};

} // namespace

TEST_CASE("rack views over a real rack: Strips draws as it always did, and a round trip through Tabs and Pads puts it back exactly") {
	RealRack r;
	r.chains(3, 3);
	CHECK_FALSE(r.card->tabsShownForTest());
	CHECK_FALSE(r.card->padsShownForTest());
	CHECK_FALSE(r.card->frontShownForTest());
	CHECK(r.card->stripsForTest().size() == 3);
	const float height = r.card->laneHeightAt(static_cast<float>(r.card->width()));
	std::vector<visage::Bounds> bounds;
	for (const auto& strip : r.card->stripsForTest()) {
		CHECK(strip->isVisible());
		bounds.push_back(strip->bounds());
	}
	for (const int v : { 1, 2, 1, 0 }) {
		r.view_(v);
	}
	CHECK(r.card->laneHeightAt(static_cast<float>(r.card->width())) == height);
	for (size_t i = 0; i < bounds.size(); ++i) {
		CHECK(r.card->stripsForTest()[i]->isVisible());
		CHECK(r.card->stripsForTest()[i]->bounds() == bounds[i]);
	}
	CHECK_FALSE(r.card->tabsShownForTest());
	// THE WINDOW'S RAILS: all of them, with neither piece of the new views.
	CHECK(r.panel->railShownForTest(0));
	CHECK(r.panel->railShownForTest(2));
	CHECK_FALSE(r.panel->tabBarForTest().isVisible());
}

namespace {

/// The REAL cards in front of a rack card, in order: what a viewer sees (a card the column holds but did not show is not counted).
std::vector<ui::FxCard*> shownCards(ui::ParallelRackCard& card) {
	std::vector<ui::FxCard*> out;
	if (ui::ChainColumn* column = card.frontForTest().columnForTest(); column != nullptr) {
		for (ui::FxCard* c : column->cardsForTest()) {
			if (c != nullptr && c->isVisible()) {
				out.push_back(c);
			}
		}
	}
	return out;
}

std::string deviceNameOf(const ui::FxCard& card) {
	return card.descriptor() != nullptr ? card.descriptor()->traits.name : "";
}

} // namespace

TEST_CASE("rack views over a real rack: Tabs shows a tab per chain and the viewed chain's REAL first card, a press on a tab rebinds to that chain's device, and the arrow opens the whole chain") {
	RealRack r;
	r.chains(3, 1); // chain 1 holds a Drum and a Delay; chains 2 and 3 are empty
	(void)r.rack().chainAt(1)->bridge()->addDevice("DELAY"); // chain 2 holds a Delay alone
	r.refresh();
	r.view_(1);
	REQUIRE(r.card->tabsShownForTest());
	CHECK(r.card->frontShownForTest());
	CHECK_FALSE(r.card->padsShownForTest());
	// NO PER-CHAIN LEVEL, MUTE OR SOLO ON THE CARD: the strips are gone.
	for (const auto& strip : r.card->stripsForTest()) {
		CHECK_FALSE(strip->isVisible());
	}
	ui::RackTabBar& bar = r.card->tabBarForTest();
	CHECK(bar.tabsForTest().size() == 3);
	// THE FRONT IS THE VIEWED CHAIN'S FIRST DEVICE alone, as its REAL card type; the chain holds two, so the arrow is there.
	feathers::rack::RackBridge& chain0 = *r.rack().chainAt(0)->bridge();
	const feathers::DeviceInstanceId first = chain0.deviceAtPosition(0);
	std::vector<ui::FxCard*> cards = shownCards(*r.card);
	REQUIRE(cards.size() == 1);
	CHECK(cards[0]->device() == first); // by device id
	auto* drum = dynamic_cast<ui::DrumDeviceCard*>(cards[0]);
	REQUIRE(drum != nullptr); // the card type the lane builds for that device, not a stand-in
	CHECK(r.card->chainArrowForTest().isVisible());
	// THE CARD DRIVES ITS REAL PARAM: a Level written on the embedded card is the device's Level in the chain.
	drum->levelForTest().amountForTest().setValue(-9.0);
	CHECK(chain0.fieldAt(first, static_cast<size_t>(fx::drumdevice::Param::Level)) == doctest::Approx(-9.0f));
	const float closed = r.card->laneHeightAt(static_cast<float>(r.card->width()));
	r.card->chainArrowForTest().toggleForTest();
	r.refresh();
	CHECK(r.rack().open(0));
	cards = shownCards(*r.card);
	REQUIRE(cards.size() == 2); // N devices, N cards, in chain order
	CHECK(cards[0]->device() == chain0.deviceAtPosition(0));
	CHECK(cards[1]->device() == chain0.deviceAtPosition(1));
	CHECK(deviceNameOf(*cards[1]) == "Delay");
	CHECK(cards[1]->bounds().y() > cards[0]->bounds().y());
	CHECK(r.card->laneHeightAt(static_cast<float>(r.card->width())) > closed); // the card sizes to what is in front
	// A PRESS ON TAB 2 REBINDS to chain 2's device (by id), and the old chain's cards are gone from the column.
	feathers::rack::RackBridge& chain1 = *r.rack().chainAt(1)->bridge();
	const feathers::DeviceInstanceId second = chain1.deviceAtPosition(0);
	const uint16_t before = r.card->frontForTest().chainIdForTest();
	bar.pressForTest(bar.tabCentre(1));
	r.refresh();
	CHECK(r.field(pr::Param::ViewedChain) == 1.0f);
	CHECK(r.card->frontForTest().chainIdForTest() != before);
	CHECK(r.card->frontForTest().chainIdForTest() == r.rack().chainIdAt(1));
	cards = shownCards(*r.card);
	REQUIRE(cards.size() == 1);
	CHECK(cards[0]->device() == second);
	CHECK(deviceNameOf(*cards[0]) == "Delay");
	REQUIRE(r.card->frontForTest().columnForTest() != nullptr);
	CHECK(r.card->frontForTest().columnForTest()->cardCountForTest() == 1); // the first chain's two cards were torn down, not kept
	CHECK_FALSE(r.card->chainArrowForTest().isVisible());                    // one device: nothing more to open
	// AN EMPTY CHAIN IN FRONT: no card, the (+) so its first device can be added.
	bar.pressForTest(bar.tabCentre(2));
	r.refresh();
	CHECK(shownCards(*r.card).empty());
	CHECK(r.card->frontForTest().columnForTest()->addShown());
	CHECK_FALSE(r.card->chainArrowForTest().isVisible());
}

TEST_CASE("rack views over a real rack: in Select the playing chain differs from the viewed one and the dot moves the real parameter") {
	RealRack r;
	r.chains(3, 3);
	r.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	r.view_(1);
	ui::RackTabBar& bar = r.card->tabBarForTest();
	bar.pressForTest(bar.tabCentre(0));
	r.refresh();
	CHECK(r.card->viewModelForTest().viewedChain() == 0);
	const float playingBefore = r.field(pr::Param::Playing);
	bar.pressForTest(bar.dotCentre(2));
	r.refresh();
	CHECK(r.field(pr::Param::Playing) == 3.0f); // chain 3, one-based in the control
	CHECK(r.card->viewModelForTest().playingChain() == 2);
	CHECK(r.card->viewModelForTest().viewedChain() == 0); // the viewed one did not move
	CHECK(r.field(pr::Param::Playing) != playingBefore);
	CHECK(bar.dotShown(2));
	CHECK(bar.raised(0));
	CHECK_FALSE(bar.raised(2));
}

TEST_CASE("rack views over a real rack: Pads lights the keys that have a chain, a press posts the key and views its chain, and a struck key flashes") {
	RealRack r;
	r.chains(3, 3, true); // keys 0, 1, 2
	r.view_(2);
	REQUIRE(r.card->padsShownForTest());
	ui::RackPadGrid& grid = r.card->padsForTest().gridForTest();
	for (int i = 0; i < 16; ++i) {
		CHECK(grid.usedForTest(i) == (i < 3));
	}
	// THE STRIP: page 0's cells for those keys are lit, and a later page is empty.
	ui::RackPadPages& pages = r.card->padsForTest().pagesForTest();
	CHECK(pages.cellUsedForTest(0, 1));
	CHECK_FALSE(pages.cellUsedForTest(0, 5));
	CHECK_FALSE(pages.cellUsedForTest(5, 1));
	// A PRESS ON PAD 2 (key 2): the key enters AT THIS RACK (its own pad ring, drained by the rack's emit stage), on then off at the release,
	// and chain 2 is viewed. NOTHING IS POSTED TO THE PLUGIN'S FRONT: the host's note road stays empty.
	feathers::UiNoteEvent e;
	while (r.f.bridge.popNote(e)) {
	}
	const auto drainRack = [&](size_t& ons, size_t& offs, int key) {
		feathers::device::TransportClock clock {};
		feathers::device::NoteEvent got[32];
		uint32_t until = 0;
		const size_t n = r.rack().emitNotes(clock, 0, got, 32, &until);
		ons = offs = 0;
		for (size_t i = 0; i < n; ++i) {
			if (got[i].key == key && got[i].noteId == feathers::kPadNoteId) {
				ons += got[i].kind == feathers::device::NoteEvent::Kind::On ? 1 : 0;
				offs += got[i].kind == feathers::device::NoteEvent::Kind::Off ? 1 : 0;
			}
		}
	};
	REQUIRE(r.card->viewModelForTest().canTrigger());
	grid.pressForTest(grid.padCentre(2));
	grid.releaseForTest();
	CHECK_FALSE(r.f.bridge.popNote(e));
	size_t ons = 0, offs = 0;
	drainRack(ons, offs, 2);
	CHECK(ons == 1);
	CHECK(offs == 1);
	CHECK(r.field(pr::Param::ViewedChain) == 2.0f);
	// PAGES: a press on page 5 changes the rack's own PadsPage, and it is that page's keys the grid maps (80..95).
	pages.pressForTest(pages.pageCentre(5));
	r.refresh();
	CHECK(r.field(pr::Param::PadsPage) == 5.0f);
	grid.pressForTest(grid.padCentre(0));
	grid.releaseForTest();
	CHECK_FALSE(r.f.bridge.popNote(e));
	drainRack(ons, offs, 80);
	CHECK(ons == 1);
	CHECK(offs == 1);
	// A PAD HELD WHEN THE EDITOR GOES is released by the grid itself: no note is left sounding.
	{
		RealRack held;
		held.chains(3, 3, true);
		held.view_(2);
		ui::RackPadGrid& g = held.panel->padsForTest().gridForTest();
		g.pressForTest(g.padCentre(1));
		held.panel = nullptr;
		held.view.reset(); // the window torn down with the pad still down
		size_t o = 0, f2 = 0;
		feathers::device::TransportClock clock {};
		feathers::device::NoteEvent got[32];
		uint32_t until = 0;
		const size_t n = held.rack().emitNotes(clock, 0, got, 32, &until);
		for (size_t i = 0; i < n; ++i) {
			if (got[i].key == 1 && got[i].noteId == feathers::kPadNoteId) {
				o += got[i].kind == feathers::device::NoteEvent::Kind::On ? 1 : 0;
				f2 += got[i].kind == feathers::device::NoteEvent::Kind::Off ? 1 : 0;
			}
		}
		CHECK(o == 1);
		CHECK(f2 == 1);
	}
	// THE PULSE FROM THE REAL STRIKE MASK: a note entering the rack flashes its pad (reduced motion: one steady highlight), and at rest
	// nothing is told.
	r.set(pr::Param::PadsPage, 0.0f);
	r.refresh();
	r.card->pulseForTest().setReducedMotion(true);
	(void)r.card->pulseForTest().poll(r.card->viewModelForTest()); // drains whatever the setup struck
	feathers::device::NoteEvent on {};
	on.kind = feathers::device::NoteEvent::Kind::On;
	on.key = 1;
	on.velocity = 1.0;
	feathers::device::NoteEvent out[8];
	(void)r.rack().processNotes(&on, 1, out, 8);
	(void)r.card->update();
	CHECK(grid.flashForTest(1) == 1.0f);
	CHECK(grid.flashForTest(0) == 0.0f);
	(void)r.card->update(); // the next look: nothing struck, the highlight goes out
	CHECK(grid.flashForTest(1) == 0.0f);
	CHECK(r.card->pulseForTest().poll(r.card->viewModelForTest()) == 0); // and at rest, no frame is told
}

TEST_CASE("rack views over a real rack: the window in Tabs shows only the chain in front, in Pads the panel and the selected chain, and a + on an empty pad makes a keyed chain") {
	RealRack r;
	r.chains(3, 3, true);
	r.view_(1);
	CHECK(r.panel->tabBarForTest().isVisible());
	CHECK(r.panel->railShownForTest(0));
	CHECK_FALSE(r.panel->railShownForTest(1));
	r.panel->tabBarForTest().pressForTest(r.panel->tabBarForTest().tabCentre(1));
	r.refresh();
	CHECK_FALSE(r.panel->railShownForTest(0));
	CHECK(r.panel->railShownForTest(1));
	// PADS: the panel and only the selected pad's chain.
	r.view_(2);
	CHECK(r.panel->padsForTest().isVisible());
	CHECK_FALSE(r.panel->tabBarForTest().isVisible());
	ui::RackPadGrid& grid = r.panel->padsForTest().gridForTest();
	CHECK_FALSE(grid.usedForTest(7));
	// "+" ON EMPTY PAD 7 (key 7) ASKS WHICH DEVICE (a press makes nothing); the Drum's row then makes a chain keyed to it with a Drum on Any
	// Key, and it becomes the viewed chain.
	const size_t before = r.rack().chainCount();
	grid.pressForTest(grid.plusCentre(7));
	r.refresh();
	REQUIRE(r.rack().chainCount() == before);
	const std::vector<const device::Descriptor*> offered = r.panel->padAddForTest().offeredForTest();
	for (size_t i = 0; i < offered.size(); ++i) {
		if (std::string(offered[i]->traits.tag) == "DRUM") {
			REQUIRE(r.panel->padAddForTest().pickForTest(7, static_cast<int>(i)) == before);
		}
	}
	r.refresh();
	REQUIRE(r.rack().chainCount() == before + 1);
	CHECK(r.rack().chainKey(before) == 7);
	CHECK(grid.usedForTest(7));
	CHECK(r.rack().chainAt(before)->bridge()->chainLength() == 1);
	CHECK(r.panel->viewModelForTest().viewedChain() == before);
	// THE ADD CONTROL IS GONE AT EIGHT CHAINS IN TABS (not greyed), and back in Strips.
	r.chains(8, 0);
	r.view_(1);
	CHECK(r.panel->addBoundsForTest().width() == 0.0f);
	r.view_(0);
	CHECK(r.panel->addBoundsForTest().width() > 0.0f);
}

TEST_CASE("rack views over a real rack: Tabs is offered in the View control only up to eight chains (hidden past it, not greyed), in the window — the card has no selectors") {
	RealRack r;
	r.chains(8, 0);
	r.refresh();
	CHECK(r.panel->viewSegForTest().isVisible());
	CHECK_FALSE(r.panel->viewPairForTest().isVisible());
	// THE NINTH CHAIN: the choice is Strips | Pads, and it writes the SAME control (Pads is the control's 2).
	r.chains(9, 0);
	r.refresh();
	CHECK_FALSE(r.panel->viewSegForTest().isVisible());
	CHECK(r.panel->viewPairForTest().isVisible());
	REQUIRE(r.panel->viewPairForTest().control() != nullptr);
	r.panel->viewPairForTest().control()->setValue(1.0);
	r.refresh();
	CHECK(r.field(pr::Param::View) == 2.0f);
	CHECK(r.card->padsShownForTest());
	r.panel->viewPairForTest().control()->setValue(0.0);
	r.refresh();
	CHECK(r.field(pr::Param::View) == 0.0f);
	// A RACK ALREADY IN TABS (a load) still shows the three-way, which can say where it is.
	r.view_(1);
	CHECK(r.panel->viewSegForTest().isVisible());
	CHECK_FALSE(r.panel->viewPairForTest().isVisible());
}

TEST_CASE("rack card: it draws neither the Mode selector nor the Strips | Tabs | Pads selector, in any view and in any mode") {
	RealRack r;
	r.chains(3, 1, true);
	const auto offersAny = [](visage::Frame* root, size_t first, size_t last) {
		bool found = false;
		const std::function<void(visage::Frame*)> walk = [&](visage::Frame* f) {
			if (const auto* face = dynamic_cast<const ui::ControlFace*>(f); face != nullptr && f->isVisible()) {
				std::vector<ui::ControlFace::Offered> list;
				face->offeredControls(list);
				for (const auto& o : list) {
					const auto* d = &o.control->desc();
					const auto* base = pr::kDescriptor.params.data();
					if (d >= base + first && d <= base + last) {
						found = true;
					}
				}
			}
			for (visage::Frame* c : f->children()) {
				if (c->isVisible()) {
					walk(c);
				}
			}
		};
		walk(root);
		return found;
	};
	for (const int view : { 0, 1, 2 }) {
		for (const pr::Mode mode : { pr::Mode::Parallel, pr::Mode::Select, pr::Mode::Bands }) {
			r.set(pr::Param::Mode, static_cast<float>(mode));
			r.view_(view);
			for (int i = 0; i < 2; ++i) {
				r.refresh();
			}
			CAPTURE(view);
			CAPTURE(static_cast<int>(mode));
			const size_t modeIndex = static_cast<size_t>(pr::Param::Mode);
			const size_t viewIndex = static_cast<size_t>(pr::Param::View);
			CHECK_FALSE(offersAny(r.card, modeIndex, modeIndex));
			CHECK_FALSE(offersAny(r.card, viewIndex, viewIndex));
			// AND THE WINDOW HAS THEM (the superset rule is the other direction, but they must still be somewhere).
			CHECK(offersAny(r.panel, modeIndex, modeIndex));
			CHECK(offersAny(r.panel, viewIndex, viewIndex));
		}
	}
}

TEST_CASE("rack views over a real rack: the window offers every control the card offers, in Strips, Tabs and Pads, with chains of every kind") {
	RealRack r;
	r.chains(3, 3, true);
	const auto collect = [](visage::Frame* root) {
		std::set<const device::ParamDesc*> out;
		const auto* first = pr::kDescriptor.params.data();
		// THE RACK'S OWN CONTROLS (the entries past `Param::kCount` are the chain slots' level, mute and solo, which a strip's routes use).
		const auto* last = first + static_cast<size_t>(pr::Param::kCount);
		const std::function<void(visage::Frame*)> walk = [&](visage::Frame* f) {
			if (const auto* face = dynamic_cast<const ui::ControlFace*>(f)) {
				std::vector<ui::ControlFace::Offered> list;
				face->offeredControls(list);
				for (const auto& o : list) {
					const device::ParamDesc* d = &o.control->desc();
					if (d > first && d < last) { // but Enabled (entry 0): the head chip is the window's too
						out.insert(d);
					}
				}
			}
			for (visage::Frame* c : f->children()) {
				walk(c);
			}
		};
		walk(root);
		return out;
	};
	for (const int view : { 0, 1, 2 }) {
		for (const pr::Mode mode : { pr::Mode::Parallel, pr::Mode::Select, pr::Mode::Keyed }) {
			r.set(pr::Param::Mode, static_cast<float>(mode));
			r.view_(view);
			const auto card = collect(r.card);
			const auto window = collect(r.panel);
			CAPTURE(view);
			CAPTURE(static_cast<int>(mode));
			CHECK(card.size() >= 5);
			for (const device::ParamDesc* d : card) {
				CAPTURE(std::string(d->name));
				CAPTURE(d - pr::kDescriptor.params.data());
				CHECK(window.count(d) == 1);
			}
		}
	}
	// NON-VACUITY: the playing chain's control is on the card's bar in Select + Tabs, and is among what is compared.
	r.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	r.view_(1);
	std::vector<ui::ControlFace::Offered> offered;
	r.card->tabBarForTest().offeredControls(offered);
	REQUIRE(offered.size() == 1);
	CHECK(&offered[0].control->desc() == &pr::kDescriptor.params[static_cast<size_t>(pr::Param::Playing)]);
}

namespace {

/// A RACK INSIDE A CHAIN of `bridge`, with `chains` chains of its own each holding a Drum, in Tabs view when `tabs`; returns its id.
feathers::DeviceInstanceId nestedRack(feathers::rack::RackBridge& bridge, size_t chains, bool tabs) {
	const feathers::DeviceInstanceId id = bridge.addDevice(pr::kTraits.tag);
	REQUIRE(id != feathers::kNoInstance);
	bridge.drain();
	pr::Rack* inner = pr::rackOf(bridge.containerSurfaceOf(id));
	REQUIRE(inner != nullptr);
	while (inner->chainCount() < chains) {
		REQUIRE(inner->addChain() != SIZE_MAX);
	}
	if (tabs) {
		bridge.setField(id, static_cast<size_t>(pr::Param::View), 1.0f);
	}
	return id;
}

} // namespace

TEST_CASE("rack views over a real rack: a Tabs rack as the first device shows its own bar inside, full width, and two levels stack three bars") {
	RealRack r;
	r.chains(2, 0);
	feathers::rack::RackBridge& chain0 = *r.rack().chainAt(0)->bridge();
	const feathers::DeviceInstanceId n1 = nestedRack(chain0, 3, true);
	pr::Rack* inner1 = pr::rackOf(chain0.containerSurfaceOf(n1));
	feathers::rack::RackBridge& inner1chain0 = *inner1->chainAt(0)->bridge();
	const feathers::DeviceInstanceId n2 = nestedRack(inner1chain0, 2, true);
	(void)n2;
	r.view_(1);
	for (int i = 0; i < 4; ++i) {
		r.refresh();
	}
	std::vector<ui::FxCard*> level1 = shownCards(*r.card);
	REQUIRE(level1.size() == 1);
	auto* card1 = dynamic_cast<ui::ParallelRackCard*>(level1[0]);
	REQUIRE(card1 != nullptr);
	CHECK(card1->device() == n1);
	CHECK(card1->tabsShownForTest());
	std::vector<ui::FxCard*> level2 = shownCards(*card1);
	REQUIRE(level2.size() == 1);
	auto* card2 = dynamic_cast<ui::ParallelRackCard*>(level2[0]);
	REQUIRE(card2 != nullptr);
	CHECK(card2->tabsShownForTest());
	// THE CHILD'S BAR SPANS THE FULL WIDTH OF ITS PARENT'S CONTENT, in native pixels, at every level: the parent's front column is as wide as
	// the first child's card and its bar, and the first child's front as the second child's card and its bar.
	const int content = r.card->frontForTest().nativeWidth();
	CHECK(content > 0);
	CHECK(card1->nativeWidth() == content);
	// A MEMBER RACK TAKES NO PADDING OF ITS OWN across the flow (its host gave it that), so its bar, its front and the card inside it are all
	// as wide as it is, at every depth: the outermost rack's content.
	CHECK(card1->tabBarForTest().nativeWidth() == content);
	CHECK(card1->frontForTest().nativeWidth() == content);
	CHECK(card2->nativeWidth() == content);
	CHECK(card2->tabBarForTest().nativeWidth() == content);
	// THE OUTERMOST RACK'S BAR is inside its outline: the card's padding either side.
	CHECK(r.card->tabBarForTest().nativeWidth() == content);
	// THREE BARS STACKED, top to bottom, none over another.
	const float y0 = r.card->tabBarForTest().positionInWindow().y;
	const float y1 = card1->tabBarForTest().positionInWindow().y;
	const float y2 = card2->tabBarForTest().positionInWindow().y;
	CHECK(y0 < y1);
	CHECK(y1 < y2);
	CHECK(y1 >= y0 + ui::RackTabBar::kThickness);
	CHECK(y2 >= y1 + ui::RackTabBar::kThickness);
	// EACH IS ITS RACK'S OWN: the bars count their own chains (2, 3, 2).
	CHECK(r.card->tabBarForTest().tabsForTest().size() == 2);
	CHECK(card1->tabBarForTest().tabsForTest().size() == 3);
	CHECK(card2->tabBarForTest().tabsForTest().size() == 2);
}

TEST_CASE("rack views over a real rack: the macro row is behind its own arrow, collapsed by default, and there is no arrow unless the rack has macros") {
	RealRack r;
	r.chains(2, 1);
	r.view_(1);
	// NO MACROS (none named, all at zero): no arrow, no row.
	CHECK_FALSE(r.card->macroArrowShownForTest());
	CHECK_FALSE(r.card->macroRowShownForTest());
	// A MACRO OFF ZERO: the arrow is offered, the row stays collapsed.
	r.set(pr::Param::Macro3, 0.5f);
	r.refresh();
	CHECK(r.card->macroArrowShownForTest());
	CHECK_FALSE(r.card->macroRowShownForTest());
	const float closed = r.card->laneHeightAt(static_cast<float>(r.card->width()));
	// THE ARROW OPENS IT (the editor's own state, not the rack's): eight knobs on the rack's own macro controls, the card taller by the row.
	r.card->macroArrowForTest().toggleForTest();
	r.refresh();
	CHECK(r.card->viewModelForTest().macrosOpen());
	CHECK(r.card->macroRowShownForTest());
	CHECK(r.card->laneHeightAt(static_cast<float>(r.card->width())) > closed);
	for (size_t i = 0; i < 8; ++i) {
		REQUIRE(r.card->macroKnobForTest(i).control() != nullptr);
		CHECK(&r.card->macroKnobForTest(i).control()->desc() == &pr::kDescriptor.params[static_cast<size_t>(pr::Param::Macro1) + i]);
	}
	// A KNOB ON THE ROW WRITES ITS MACRO.
	r.card->macroKnobForTest(0).control()->setValue(0.25);
	CHECK(r.field(pr::Param::Macro1) == doctest::Approx(0.25f));
	// BACK TO ZERO EVERYWHERE: the arrow goes (a rack without macros has none), and so does the row.
	r.card->macroKnobForTest(0).control()->setValue(0.0);
	r.set(pr::Param::Macro3, 0.0f);
	r.refresh();
	CHECK_FALSE(r.card->macroArrowShownForTest());
	CHECK_FALSE(r.card->macroRowShownForTest());
	// AND STRIPS NEVER SHOWS IT.
	r.set(pr::Param::Macro3, 0.5f);
	r.view_(0);
	CHECK_FALSE(r.card->macroArrowShownForTest());
	CHECK_FALSE(r.card->macroRowShownForTest());
}

TEST_CASE("rack tabs: eight tabs keep their room for names (the set-playing dot shows on the playing tab and under the pointer only), and a name that does not fit is cut with an ellipsis") {
	FakeRack rack;
	rack.fill(8);
	rack.route = ui::RackRoutingKind::Select;
	rack.playing = 2;
	rack.chains[5].name = "A very long chain name";
	ui::RackTabBar bar(rack, ui::RackFlow::TopDown);
	bar.setBounds(0.0f, 0.0f, 260.0f, kBar);
	// THE DOT: lit on the playing tab alone; no other tab claims any of its room unless the pointer is over it.
	for (size_t k = 0; k < 8; ++k) {
		CHECK(bar.dotShown(k) == (k == 2));
		CHECK_FALSE(bar.hoverDotShown(k));
	}
	bar.hoverForTest(bar.tabCentre(5));
	CHECK(bar.hoverDotShown(5));
	CHECK_FALSE(bar.hoverDotShown(4));
	CHECK_FALSE(bar.hoverDotShown(2)); // the playing tab's is the lit one
	bar.hoverForTest({ -1.0f, -1.0f });
	CHECK_FALSE(bar.hoverDotShown(5));
	// THE DOT'S PRESS STILL SETS PLAYING (the hover only shows where it is).
	bar.pressForTest(bar.dotCentre(4));
	CHECK(rack.playing == 4);
	// THE NAMES: a short one is whole; the long one ends in an ellipsis ("...", three full stops) and is shorter than itself.
	CHECK(bar.labelForTest(0) == "A");
	const std::string cut = bar.labelForTest(5);
	CHECK(cut.size() >= 3);
	CHECK(cut.substr(cut.size() - 3) == "...");
	CHECK(cut.size() < rack.chains[5].name.size());
	CHECK(bar.labelRoomForTest(5) > 0.0f);
	// A TAB'S ROOM FOR ITS NAME IS ITS WIDTH LESS A FIXED MARGIN: the dot takes none of it.
	CHECK(bar.labelRoomForTest(0) == doctest::Approx(260.0f / 8.0f - 8.0f).epsilon(0.05));
}

TEST_CASE("rack views over a real rack: the cards in front are as wide as the card's content across the flow, top-down and left-right, so the hairline adds no space") {
	RealRack r;
	r.chains(2, 2);
	r.view_(1);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	// TOP-DOWN: a member card is exactly the card's width less its two margins (native pixels).
	{
		std::vector<ui::FxCard*> cards = shownCards(*r.card);
		REQUIRE(cards.size() == 1);
		const int margins = static_cast<int>(std::lround(2.0f * ui::ParallelRackCard::kPad * r.card->dpiScale()));
		CHECK(cards[0]->nativeWidth() == r.card->nativeWidth() - margins);
		CHECK(r.card->frontForTest().nativeWidth() == cards[0]->nativeWidth());
	}
	// LEFT-RIGHT: the same across the other axis: the card's height less its two margins.
	r.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
		r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	}
	std::vector<ui::FxCard*> cards = shownCards(*r.card);
	REQUIRE(cards.size() == 1);
	const int margins = static_cast<int>(std::lround(2.0f * ui::ParallelRackCard::kPad * r.card->dpiScale()));
	CHECK(cards[0]->nativeHeight() == r.card->nativeHeight() - margins);
	CHECK(r.card->frontForTest().columnForTest()->horizontal());
	// THE BAR RUNS DOWN THE SIDE, as tall as the cards beside it, and the cards run right of it: opening the chain makes the card wider by a card.
	CHECK(r.card->tabBarForTest().nativeHeight() == r.card->nativeHeight() - margins); // inside the outline, the cards' own cross extent
	CHECK(r.card->tabBarForTest().nativeWidth() < r.card->nativeHeight());
	const float closed = r.card->laneExtent(ui::kCardUnitHeight);
	r.card->chainArrowForTest().toggleForTest();
	r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
		r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	}
	cards = shownCards(*r.card);
	REQUIRE(cards.size() == 2);
	CHECK(r.card->laneExtent(ui::kCardUnitHeight) > closed);
	CHECK(cards[1]->bounds().x() > cards[0]->bounds().x()); // in flow order: left to right
	CHECK(cards[1]->bounds().y() == doctest::Approx(cards[0]->bounds().y()));
}

TEST_CASE("rack views over a real rack: in Pads the selected pad's chain steps closed, first, whole by a less/more pair, in both flows, and Tabs keeps its one arrow") {
	for (const bool horizontal : { false, true }) {
		RealRack r;
		r.chains(3, 2, true);
		(void)r.rack().chainAt(1)->bridge()->addDevice("DRUM"); // chain 2 holds three cards; chain 3 none yet
		r.view_(2);
		const auto settle = [&] {
			for (int i = 0; i < 3; ++i) {
				r.refresh();
				if (horizontal) {
					r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
				}
			}
		};
		if (horizontal) {
			r.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
		}
		settle();
		ui::RackPadGrid& grid = r.card->padsForTest().gridForTest();
		feathers::rack::RackBridge& chain1 = *r.rack().chainAt(1)->bridge();
		grid.pressForTest(grid.padCentre(1)); // key 1 = chain 2, three devices
		grid.releaseForTest();
		settle();
		ui::RackPadsStep& step = r.card->padsStepForTest();
		const auto extent = [&] { return horizontal ? r.card->laneExtent(ui::kCardUnitHeight) : r.card->laneHeightAt(static_cast<float>(r.card->width())); };
		// FIRST BY DEFAULT: the first card alone, the pair present with MORE and LESS both drawn, and Tabs' arrow not drawn in Pads.
		std::vector<ui::FxCard*> cards = shownCards(*r.card);
		REQUIRE(cards.size() == 1);
		CHECK(cards[0]->device() == chain1.deviceAtPosition(0));
		CHECK(r.card->frontForTest().chainIdForTest() == r.rack().chainIdAt(1));
		CHECK(r.card->padsFrontForTest() == 1);
		REQUIRE(step.isVisible());
		CHECK(step.lessShown());
		CHECK(step.moreShown());
		CHECK_FALSE(r.card->chainArrowForTest().isVisible());
		// THE ARROWS ARE THE TABS ARROW'S SIZE (one small glyph each, side by side), and their rects are the same in every state.
		CHECK(step.lessBounds().width() == doctest::Approx(ui::RackDrawerArrow::kSize));
		CHECK(step.lessBounds().height() == doctest::Approx(ui::RackDrawerArrow::kSize));
		CHECK(step.moreBounds().width() == doctest::Approx(ui::RackDrawerArrow::kSize));
		CHECK(step.moreBounds().height() == doctest::Approx(ui::RackDrawerArrow::kSize));
		const visage::Bounds fixedBounds = step.bounds();
		const visage::Bounds fixedLess = step.lessBounds();
		const visage::Bounds fixedMore = step.moreBounds();
		const float first = extent();
		if (!horizontal) {
			CHECK(step.bounds().y() >= r.card->padsForTest().bounds().y() + r.card->padsForTest().height());
			CHECK(step.bounds().y() + step.height() <= r.card->frontForTest().bounds().y());
		}
		else {
			// LEFT TO RIGHT: UNDER THE GRID, in its column, before the cards: a place the cards' expansion cannot move.
			CHECK(step.bounds().x() == doctest::Approx(r.card->padsForTest().bounds().x()));
			CHECK(step.bounds().y() >= r.card->padsForTest().bounds().y() + r.card->padsForTest().height() - 0.01f);
			CHECK(step.bounds().x() + step.width() <= r.card->frontForTest().bounds().x());
		}
		// MORE brings the whole chain (all three, in order); the arrow that cannot step further is gone, and the card is bigger.
		step.pressMoreForTest();
		settle();
		cards = shownCards(*r.card);
		REQUIRE(cards.size() == 3);
		for (size_t i = 0; i < 3; ++i) {
			CHECK(cards[i]->device() == chain1.deviceAtPosition(i));
		}
		CHECK(r.card->padsFrontForTest() == 2);
		CHECK(step.lessShown());
		CHECK_FALSE(step.moreShown());
		CHECK(extent() > first);
		// FIXED: the pair did not move as the cards expanded.
		CHECK(step.bounds() == fixedBounds);
		CHECK(step.lessBounds() == fixedLess);
		CHECK(step.moreBounds() == fixedMore);
		step.pressMoreForTest(); // not drawn: does nothing
		settle();
		CHECK(shownCards(*r.card).size() == 3);
		// LESS twice: closed, no card, the pair still there with only MORE, and the card smaller than at first (it sizes to its content).
		step.pressLessForTest();
		step.pressLessForTest();
		settle();
		CHECK(shownCards(*r.card).empty());
		CHECK(r.card->padsFrontForTest() == 0);
		REQUIRE(step.isVisible());
		CHECK_FALSE(step.lessShown());
		CHECK(step.moreShown());
		CHECK(extent() < first);
		CHECK(step.bounds() == fixedBounds); // and not as they folded away either
		step.pressLessForTest(); // not drawn: does nothing
		CHECK(r.card->padsFrontForTest() == 0);
		// THE GRID and its kit level are still there closed.
		CHECK(r.card->padsForTest().isVisible());
		// MORE from closed is first again, and viewing another pad keeps the step (per rack, not per chain).
		step.pressMoreForTest();
		settle();
		CHECK(shownCards(*r.card).size() == 1);
		grid.pressForTest(grid.padCentre(0));
		grid.releaseForTest();
		settle();
		CHECK(r.card->frontForTest().chainIdForTest() == r.rack().chainIdAt(0));
		CHECK(shownCards(*r.card).size() == 1);
		// AN EMPTY CHAIN: nothing to step, so the pair is absent. A ONE-DEVICE CHAIN: first is the whole chain, so LESS is drawn and MORE is not.
		grid.pressForTest(grid.padCentre(2));
		grid.releaseForTest();
		settle();
		CHECK_FALSE(step.isVisible());
		(void)r.rack().chainAt(2)->bridge()->addDevice("DRUM");
		settle();
		REQUIRE(step.isVisible());
		CHECK(step.lessShown());
		CHECK_FALSE(step.moreShown());
		CHECK(shownCards(*r.card).size() == 1);
	}
}

TEST_CASE("rack views over a real rack: Tabs' arrow is drawn only when the chain holds more than one card, and the Pads pair is never drawn in Tabs") {
	RealRack r;
	r.chains(2, 0, true);
	(void)r.rack().chainAt(0)->bridge()->addDevice("DRUM"); // one card
	(void)r.rack().chainAt(1)->bridge()->addDevice("DRUM");
	(void)r.rack().chainAt(1)->bridge()->addDevice("DELAY"); // two
	r.view_(1);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	ui::RackTabBar& bar = r.card->tabBarForTest();
	bar.pressForTest(bar.tabCentre(0));
	r.refresh();
	CHECK(r.card->frontForTest().chainIdForTest() == r.rack().chainIdAt(0));
	CHECK_FALSE(r.card->chainArrowForTest().isVisible()); // nothing more to open: no dead control
	CHECK_FALSE(r.card->padsStepForTest().isVisible());
	bar.pressForTest(bar.tabCentre(1));
	r.refresh();
	CHECK(r.card->chainArrowForTest().isVisible());
	CHECK_FALSE(r.card->padsStepForTest().isVisible());
}

namespace {

/// A pulse whose clock a case sets, so two surfaces can poll "in one tick".
struct TickClock {
	double now = 10.0;
};

} // namespace

TEST_CASE("rack strike pulse: two surfaces of one rack, one strike — both show it, whichever looks first, and the hub goes with its last pulse") {
	const ui::RackStrikeKey key { 7 };
	const size_t hubsBefore = ui::RackStrikeHub::hubCountForTest();
	for (const bool windowFirst : { false, true }) {
		CAPTURE(windowFirst);
		auto engine = std::make_shared<ui::RackKeyMask>();
		FakeRack cardModel;
		FakeRack windowModel;
		for (FakeRack* m : { &cardModel, &windowModel }) {
			m->source = engine;
			m->rackKey = key;
			m->kind = ui::RackViewKind::Pads;
			m->fill(0);
		}
		TickClock clock;
		{
			ui::RackStrikePulse cardPulse;
			ui::RackStrikePulse windowPulse;
			for (ui::RackStrikePulse* p : { &cardPulse, &windowPulse }) {
				p->setReducedMotion(false);
				p->setClock([&clock] { return clock.now; });
			}
			ui::RackPadGrid cardGrid(cardModel, cardPulse); // each surface has a frame the pulse repaints
			ui::RackPadGrid windowGrid(windowModel, windowPulse);
			// ONE STRIKE, one tick: whichever surface looks first takes the engine's mask once.
			maskSet(*engine, 38);
			ui::RackStrikePulse& first = windowFirst ? windowPulse : cardPulse;
			ui::RackStrikePulse& second = windowFirst ? cardPulse : windowPulse;
			FakeRack& firstModel = windowFirst ? windowModel : cardModel;
			FakeRack& secondModel = windowFirst ? cardModel : windowModel;
			CHECK(first.poll(firstModel) > 0);
			CHECK(second.poll(secondModel) >= 0);
			CHECK(first.onHubForTest());
			CHECK(second.onHubForTest());
			CHECK(ui::RackStrikeHub::subscribersForTest(key) == 2);
			// BOTH REPORT THE PULSE, and both were told to repaint by it.
			CHECK(cardPulse.level(38, clock.now) > 0.0f);
			CHECK(windowPulse.level(38, clock.now) > 0.0f);
			CHECK(cardPulse.level(39, clock.now) == 0.0f);
			CHECK(windowPulse.level(39, clock.now) == 0.0f);
			CHECK(cardPulse.lastToldForTest() > 0);
			CHECK(windowPulse.lastToldForTest() > 0);
			// THE NEXT TICK, nothing struck: the flash decays on both, and once it is out nothing is told (zero repaints at rest).
			clock.now += 0.02;
			(void)first.poll(firstModel);
			(void)second.poll(secondModel);
			CHECK(cardPulse.level(38, clock.now) < 1.0f);
			clock.now += 1.0;
			(void)first.poll(firstModel); // the repaint that clears the last frame
			(void)second.poll(secondModel);
			clock.now += 0.02;
			CHECK(first.poll(firstModel) == 0);
			CHECK(second.poll(secondModel) == 0);
			CHECK(cardPulse.level(38, clock.now) == 0.0f);
		}
		// UNSUBSCRIBED ON DESTRUCTION: no hub is left standing for the rack.
		CHECK(ui::RackStrikeHub::subscribersForTest(key) == 0);
		CHECK(ui::RackStrikeHub::hubCountForTest() == hubsBefore);
	}
	// A PULSE WITH NO KEY (a model that names no rack) takes the mask itself, as it always did.
	FakeRack plain;
	plain.fill(0);
	ui::RackStrikePulse alone;
	maskSet(plain.struck, 5);
	(void)alone.poll(plain);
	CHECK_FALSE(alone.onHubForTest());
	CHECK(alone.level(5, alone.now()) > 0.0f);
}

TEST_CASE("rack strike pulse: reduced motion keeps one steady highlight on every surface until the next look") {
	auto engine = std::make_shared<ui::RackKeyMask>();
	FakeRack a;
	FakeRack b;
	for (FakeRack* m : { &a, &b }) {
		m->source = engine;
		m->rackKey = { 9 };
		m->fill(0);
	}
	TickClock clock;
	ui::RackStrikePulse pa;
	ui::RackStrikePulse pb;
	for (ui::RackStrikePulse* p : { &pa, &pb }) {
		p->setReducedMotion(true);
		p->setClock([&clock] { return clock.now; });
	}
	pa.join(a.strikeKey()); // a surface joins when it is made
	pb.join(b.strikeKey());
	maskSet(*engine, 40);
	(void)pa.poll(a);
	(void)pb.poll(b); // the same tick: it must not wipe what the first look handed it
	CHECK(pa.level(40, clock.now) == 1.0f);
	CHECK(pb.level(40, clock.now) == 1.0f);
	clock.now += 0.016; // the next tick (past the hub's round), nothing struck: both clear
	(void)pb.poll(b);
	(void)pa.poll(a);
	CHECK(pa.level(40, clock.now) == 0.0f);
	CHECK(pb.level(40, clock.now) == 0.0f);
}

TEST_CASE("rack pads: Follow played note — off moves nothing; on views the struck key's chain and turns to its page; a key with no chain moves nothing") {
	const auto setup = [](FakeRack& rack) {
		rack.kind = ui::RackViewKind::Pads;
		// Chains on keys 36 (slot 0), 38 (slot 1) and 83 (slot 2: page 5).
		rack.chains = { { true, "Kick", 36, 0 }, { true, "Snare", 38, 0 }, { true, "Far", 83, 0 } };
		rack.viewed = 0;
		rack.pageNow = 2;
	};
	for (const bool on : { false, true }) {
		CAPTURE(on);
		FakeRack rack;
		setup(rack);
		rack.follow = on;
		ui::RackStrikePulse pulse;
		// A STRUCK KEY WITH A CHAIN.
		maskSet(rack.struck, 38);
		(void)pulse.poll(rack);
		if (on) {
			CHECK(rack.viewed == 1);
			CHECK(rack.pageNow == 2); // 38 is on page 2 already
		}
		else {
			CHECK(rack.viewed == 0);
			CHECK(rack.pageNow == 2);
		}
		// A KEY ON ANOTHER PAGE turns the page.
		maskSet(rack.struck, 83);
		(void)pulse.poll(rack);
		CHECK(rack.viewed == (on ? 2u : 0u));
		CHECK(rack.pageNow == (on ? 5 : 2));
		// A KEY WITH NO CHAIN moves nothing, on or off.
		const size_t viewedBefore = rack.viewed;
		const int pageBefore = rack.pageNow;
		maskSet(rack.struck, 100);
		(void)pulse.poll(rack);
		CHECK(rack.viewed == viewedBefore);
		CHECK(rack.pageNow == pageBefore);
		// A CHORD settles on the highest key that has a chain; keys without one are passed over.
		FakeRack chord;
		setup(chord);
		chord.follow = on;
		ui::RackStrikePulse chordPulse;
		maskSet(chord.struck, 36);
		maskSet(chord.struck, 38);
		maskSet(chord.struck, 99);
		(void)chordPulse.poll(chord);
		CHECK(chord.viewed == (on ? 1u : 0u));
	}
}

TEST_CASE("rack pads: Follow played note is a patch param like the view, with a switch in the window's pads panel — off by default") {
	namespace pr = feathers::rack::parallel;
	const device::ParamDesc& d = pr::kDescriptor.params[static_cast<size_t>(pr::Param::FollowPlayed)];
	CHECK(std::string(d.key) == "rack_follow_played");
	CHECK(d.def == 0.0);
	CHECK(d.stepped);
	CHECK_FALSE(d.modulatable);
	CHECK_FALSE(d.hostSlot);
}

TEST_CASE("rack strike pulse over a real rack: the card and the window both show one strike, and Follow played note acts on it once") {
	RealRack r;
	r.chains(3, 3, true); // chains on keys 0, 1, 2
	r.view_(2);
	for (ui::RackStrikePulse* p : { &r.card->pulseForTest(), &r.panel->pulseForTest() }) {
		p->setReducedMotion(false);
	}
	const auto strike = [&](int key) {
		feathers::device::NoteEvent e;
		e.kind = feathers::device::NoteEvent::Kind::On;
		e.key = static_cast<int16_t>(key);
		e.velocity = 0.8;
		feathers::device::NoteEvent out[8];
		(void)r.rack().processNotes(&e, 1, out, 8);
	};
	// ONE STRIKE, BOTH SURFACES OPEN: the engine's mask is taken once and both pulses show it.
	strike(1);
	(void)r.card->update();
	(void)r.panel->update();
	CHECK(r.card->pulseForTest().onHubForTest());
	CHECK(r.panel->pulseForTest().onHubForTest());
	CHECK(r.card->pulseForTest().level(1, r.card->pulseForTest().now()) > 0.0f);
	CHECK(r.panel->pulseForTest().level(1, r.panel->pulseForTest().now()) > 0.0f);
	CHECK(r.panel->pulseForTest().level(2, r.panel->pulseForTest().now()) == 0.0f);
	// FOLLOW IS OFF BY DEFAULT: nothing moved.
	CHECK(r.field(pr::Param::FollowPlayed) == 0.0f);
	CHECK(r.field(pr::Param::ViewedChain) == 0.0f);
	// ON: a struck key that has a chain makes it the viewed one — once, from whichever surface looked first.
	r.set(pr::Param::FollowPlayed, 1.0f);
	r.refresh();
	std::this_thread::sleep_for(std::chrono::milliseconds(20)); // past the hub's round
	strike(2);
	(void)r.panel->update();
	(void)r.card->update();
	CHECK(r.field(pr::Param::ViewedChain) == 2.0f);
	CHECK(r.field(pr::Param::PadsPage) == 0.0f);
	// A KEY WITH NO CHAIN moves nothing.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	strike(100);
	(void)r.card->update();
	(void)r.panel->update();
	CHECK(r.field(pr::Param::ViewedChain) == 2.0f);
	CHECK(r.field(pr::Param::PadsPage) == 0.0f);
	// THE WINDOW'S SWITCH is in the pads panel's view and reads the param.
	CHECK(r.panel->followChipForTest().isVisible());
}

TEST_CASE("rack window: the view selector gives every label its segment, and the mode, view, mix and gain block overlaps nothing, at 1x and 1.75x") {
	RealRack r;
	r.chains(2, 0);
	for (const float scale : { 1.0f, 1.75f, 1.0f }) {
		CAPTURE(scale);
		r.panel->setDpiScale(scale);
		r.panel->resized();
		r.refresh();
		for (const int mode : { 0, 1 }) { // Parallel, then Select (which adds the Playing box and the Position knob)
			CAPTURE(mode);
			r.set(pr::Param::Mode, static_cast<float>(mode));
			r.refresh();
			ui::SegmentedControl& seg = r.panel->viewSegForTest();
			REQUIRE(seg.isVisible());
			// EACH LABEL INSIDE ITS CELL, as the control measures it, at the native size it was given.
			CHECK(seg.labelsFit());
			// THE SELECTOR IS THE COLUMN'S WIDTH, a third of it to each of Strips, Tabs and Pads — never the 94 it was squeezed into.
			const float nativeColumn = scale * ui::ParallelRackExpanded::kColumnWidth;
			CHECK(static_cast<float>(seg.nativeWidth()) >= nativeColumn - 2.0f);
			// NOTHING OVERLAPS: the block's frames, two by two (the boxes that are shown).
			std::vector<std::pair<const char*, visage::Bounds>> block = {
				{ "mode", r.panel->modeBoxForTest().bounds() },
				{ "view", seg.bounds() },
				{ "viewed", r.panel->viewedBoxForTest().bounds() },
				{ "page", r.panel->pageBoxForTest().bounds() },
				{ "mix", r.panel->knobBoundsForTest(0) },
				{ "gain", r.panel->knobBoundsForTest(1) },
			};
			if (mode == 1) {
				block.emplace_back("playing", r.panel->playingBoxForTest().bounds());
				block.emplace_back("position", r.panel->positionKnobForTest().bounds());
			}
			for (size_t i = 0; i < block.size(); ++i) {
				for (size_t j = i + 1; j < block.size(); ++j) {
					const visage::Bounds& a = block[i].second;
					const visage::Bounds& b = block[j].second;
					const bool apart = a.right() <= b.x() + 0.01f || b.right() <= a.x() + 0.01f || a.bottom() <= b.y() + 0.01f
					                   || b.bottom() <= a.y() + 0.01f;
					CAPTURE(block[i].first);
					CAPTURE(block[j].first);
					CHECK(apart);
				}
			}
		}
	}
}

namespace {

/// A REAL PRESS ON THE CARD at `inCard` (the card's own coordinates): found by the frame tree as the window finds it, delivered as it
/// delivers (to the frame under the point, then up through its parents), and the note keys it posted, on then off.
std::vector<std::pair<int, bool>> pressOnCard(RealRack& r, visage::Point inCard) {
	// (THE KEYS ENTER AT THE RACK: they are read back as the rack's emit stage hands them on, and the plugin's own note road must stay empty.)
	const auto takePads = [&] {
		feathers::device::TransportClock clock {};
		feathers::device::NoteEvent got[64];
		uint32_t until = 0;
		const size_t n = r.rack().emitNotes(clock, 0, got, 64, &until);
		std::vector<std::pair<int, bool>> keys;
		for (size_t i = 0; i < n; ++i) {
			if (got[i].noteId == feathers::kPadNoteId) {
				keys.emplace_back(got[i].key, got[i].kind == feathers::device::NoteEvent::Kind::On);
			}
		}
		return keys;
	};
	(void)takePads();
	visage::Frame* target = r.card->frameAtPoint(inCard);
	if (target != nullptr) {
		visage::MouseEvent down {};
		down.event_frame = target;
		const visage::Point topLeft = target->positionInWindow() - r.card->positionInWindow();
		down.position = inCard - topLeft;
		down.window_position = r.card->positionInWindow() + inCard;
		target->processMouseDown(down);
		visage::MouseEvent up = down;
		target->processMouseUp(up);
	}
	std::vector<std::pair<int, bool>> out = takePads();
	feathers::UiNoteEvent front;
	CHECK_FALSE(r.f.bridge.popNote(front));
	return out;
}

} // namespace

TEST_CASE("rack pads: a press strikes the pad's own cell and nothing else — not a gap, not the rest of the card — with one pad populated or several") {
	for (const size_t populated : { size_t { 1 }, size_t { 3 } }) {
		CAPTURE(populated);
		RealRack r;
		r.chains(populated, 0, true); // chain k answers key k: pads 0..populated-1
		r.view_(2);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		ui::RackPadGrid& grid = r.card->padsForTest().gridForTest();
		const visage::Point origin = grid.positionInWindow() - r.card->positionInWindow();
		const auto inCard = [&](visage::Point inGrid) { return visage::Point { origin.x + inGrid.x, origin.y + inGrid.y }; };
		// EVERY CELL'S CENTRE strikes its own key (on, then off), populated or empty.
		for (int index = 0; index < 16; ++index) {
			CAPTURE(index);
			const auto got = pressOnCard(r, inCard(grid.padCentre(index)));
			REQUIRE(got.size() == 2);
			CHECK(got[0] == std::pair<int, bool> { index, true });
			CHECK(got[1] == std::pair<int, bool> { index, false });
		}
		// A GAP between two cells (the inset a pad leaves) strikes nothing.
		const visage::Point gap = inCard({ grid.padCentre(0).x + (grid.padCentre(1).x - grid.padCentre(0).x) * 0.5f, grid.padCentre(0).y });
		CHECK(pressOnCard(r, gap).empty());
		// THE GRID'S MARGIN, THE STRIP, THE REST OF THE CARD: nothing is struck. Points all over the card outside the grid.
		const float w = static_cast<float>(r.card->width());
		const float h = static_cast<float>(r.card->height());
		for (const visage::Point p : { visage::Point { 2.0f, 2.0f }, visage::Point { w - 2.0f, 40.0f }, visage::Point { w * 0.5f, h - 4.0f },
		                               visage::Point { 3.0f, h * 0.5f }, visage::Point { origin.x - 4.0f, origin.y + 20.0f },
		                               visage::Point { origin.x + static_cast<float>(grid.width()) + 4.0f, origin.y + 20.0f },
		                               visage::Point { origin.x + 20.0f, origin.y + static_cast<float>(grid.height()) + 3.0f } }) {
			CAPTURE(p.x);
			CAPTURE(p.y);
			CHECK(pressOnCard(r, p).empty());
		}
	}
}

TEST_CASE("add menus: Containers lists Tabs and Pads next to Rack and Chain, and picking one adds a Rack already in that view and routing") {
	RealRack r;
	r.chains(1, 0);
	r.view_(1);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	ui::ChainColumn* column = r.card->frontForTest().columnForTest();
	REQUIRE(column != nullptr);
	const size_t offered = column->addable().size();
	struct Want {
		const char* name;
		float view;
		pr::Mode mode;
	};
	for (const Want want : { Want { "Tabs", 1.0f, pr::Mode::Parallel }, Want { "Pads", 2.0f, pr::Mode::Keyed } }) {
		CAPTURE(want.name);
		visage::PopupMenu menu = column->addMenuForTest();
		// THE ROW IS UNDER CONTAINERS, with the container devices, and after the devices the menu lists.
		const visage::PopupMenu* containers = nullptr;
		for (const visage::PopupMenu& entry : menu.options()) {
			if (entry.name().toUtf8() == "Containers") {
				containers = &entry;
			}
		}
		REQUIRE(containers != nullptr);
		std::vector<std::string> names;
		int id = -1;
		for (const visage::PopupMenu& row : containers->options()) {
			names.push_back(row.name().toUtf8());
			if (row.name().toUtf8() == want.name) {
				id = row.id();
			}
		}
		CHECK(std::find(names.begin(), names.end(), "Rack") != names.end());
		CHECK(std::find(names.begin(), names.end(), "Chain") != names.end());
		REQUIRE(id >= static_cast<int>(offered));
		// PICKING IT: a Rack arrives in the chain, in that view, with that routing.
		feathers::rack::RackBridge& chain = *r.rack().chainAt(0)->bridge();
		const size_t before = chain.chainLength();
		menu.onSelection().callback(id);
		REQUIRE(chain.chainLength() == before + 1);
		const feathers::DeviceInstanceId made = chain.deviceAtPosition(before);
		REQUIRE(chain.descriptorOf(made) != nullptr);
		CHECK(std::string(chain.descriptorOf(made)->traits.tag) == "RACK");
		CHECK(chain.fieldAt(made, static_cast<size_t>(pr::Param::View)) == want.view);
		CHECK(chain.fieldAt(made, static_cast<size_t>(pr::Param::Mode)) == static_cast<float>(want.mode));
	}
}

TEST_CASE("rack window: in Tabs the chain in front is always open and has no collapse control, whatever its stored flag says; Strips is as it was") {
	RealRack r;
	r.chains(3, 1, true);
	for (size_t i = 0; i < 3; ++i) {
		r.rack().setOpen(i, false); // every chain stored closed
	}
	r.refresh();
	// STRIPS: closed as stored, with its toggle.
	for (size_t i = 0; i < 3; ++i) {
		CHECK(r.panel->railForTest(i).collapsed());
		CHECK(r.panel->railForTest(i).collapsible());
	}
	const visage::Bounds toggle = r.panel->railForTest(0).toggleBounds();
	CHECK(r.panel->railForTest(0).toggleHit({ toggle.x() + 2.0f, toggle.y() + 2.0f }));
	// TABS: open, no toggle (a press where it was does nothing), the stored flag untouched.
	r.view_(1);
	r.refresh();
	for (size_t i = 0; i < 3; ++i) {
		CAPTURE(i);
		CHECK_FALSE(r.panel->railForTest(i).collapsed());
		CHECK_FALSE(r.panel->railForTest(i).collapsible());
		CHECK_FALSE(r.rack().open(i)); // the card's arrow's own state is left alone
	}
	CHECK_FALSE(r.panel->railForTest(0).toggleHit({ toggle.x() + 2.0f, toggle.y() + 2.0f }));
	CHECK(r.panel->railForTest(0).width() > 40.0f); // the open column, not the 34 px strip
	// BACK TO STRIPS: each as it was stored.
	r.view_(0);
	r.refresh();
	for (size_t i = 0; i < 3; ++i) {
		CHECK(r.panel->railForTest(i).collapsed());
		CHECK(r.panel->railForTest(i).collapsible());
	}
}

TEST_CASE("rack views over a real rack: the tab bar sits inside the card's outline, a member rack's cross extent is the outermost rack's content at every depth, and the group is bracketed along the flow with no cross-flow cost") {
	for (const bool horizontal : { false, true }) {
		CAPTURE(horizontal);
		RealRack r;
		r.chains(2, 0);
		feathers::rack::RackBridge& chain0 = *r.rack().chainAt(0)->bridge();
		const feathers::DeviceInstanceId n1 = nestedRack(chain0, 2, true);
		feathers::rack::RackBridge& c1 = *pr::rackOf(chain0.containerSurfaceOf(n1))->chainAt(0)->bridge();
		const feathers::DeviceInstanceId n2 = nestedRack(c1, 2, true);
		feathers::rack::RackBridge& c2 = *pr::rackOf(c1.containerSurfaceOf(n2))->chainAt(0)->bridge();
		(void)nestedRack(c2, 2, true);
		r.view_(1);
		if (horizontal) {
			r.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
		}
		for (int i = 0; i < 6; ++i) {
			r.refresh();
			if (horizontal) {
				r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
			}
		}
		std::vector<ui::ParallelRackCard*> racks { r.card };
		for (int depth = 0; depth < 3; ++depth) {
			std::vector<ui::FxCard*> in = shownCards(*racks.back());
			REQUIRE(in.size() == 1);
			auto* next = dynamic_cast<ui::ParallelRackCard*>(in[0]);
			REQUIRE(next != nullptr);
			racks.push_back(next);
		}
		const auto cross = [&](const visage::Frame& c) { return horizontal ? c.nativeHeight() : c.nativeWidth(); };
		const int content = cross(r.card->frontForTest());
		REQUIRE(content > 0);
		for (size_t d = 0; d < racks.size(); ++d) {
			CAPTURE(d);
			ui::ParallelRackCard& card = *racks[d];
			ui::RackTabBar& bar = card.tabBarForTest();
			// THE BAR IS INSIDE THE CARD'S OUTLINE, in logical coordinates, at every depth.
			CHECK(bar.x() >= 0.0f);
			CHECK(bar.y() >= 0.0f);
			CHECK(bar.x() + bar.width() <= card.width() + 0.01f);
			CHECK(bar.y() + bar.height() <= card.height() + 0.01f);
			if (d == 0) {
				// THE OUTERMOST CARD KEEPS ITS OWN PADDING across the flow: a clear margin between the bar and the outline.
				if (!horizontal) {
					CHECK(bar.x() >= ui::ParallelRackCard::kPad);
					CHECK(bar.x() + bar.width() <= card.width() - ui::ParallelRackCard::kPad + 0.01f);
				}
				else {
					CHECK(bar.y() >= ui::ParallelRackCard::kPad);
					CHECK(bar.y() + bar.height() <= card.height() - ui::ParallelRackCard::kPad + 0.01f);
				}
			}
			else {
				// A MEMBER'S CROSS EXTENT IS THE OUTERMOST RACK'S CONTENT, whatever the depth.
				CHECK(cross(card) == content);
				CHECK(cross(card.tabBarForTest()) == content);
			}
			// THE OPENING RULE is on the bar, in this rack's own colour; the closing rule is one pixel across the cards' full cross extent.
			CHECK(bar.ruleForTest() != 0u);
			const visage::Bounds rule = card.closingRuleForTest();
			REQUIRE(rule.width() > 0.0f);
			if (!horizontal) {
				CHECK(rule.height() == doctest::Approx(1.0f));
				CHECK(rule.width() == doctest::Approx(card.frontForTest().width()));
				CHECK(rule.x() == doctest::Approx(card.frontForTest().x()));
			}
			else {
				CHECK(rule.width() == doctest::Approx(1.0f));
				CHECK(rule.height() == doctest::Approx(card.frontForTest().height()));
				CHECK(rule.y() == doctest::Approx(card.frontForTest().y()));
			}
			// NO CROSS-FLOW PIXEL IS SPENT ON GROUPING: the cards in front are exactly as wide across the flow as the rack's content.
			const int pad = d == 0 ? static_cast<int>(std::lround(2.0f * ui::ParallelRackCard::kPad * card.dpiScale())) : 0;
			CHECK(cross(card.frontForTest()) == cross(card) - pad);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// THE RACK WINDOW'S TABS PANE (`rack_front` = Window): the front chain's first device's own window, held in the window.
// ---------------------------------------------------------------------------------------------------------------------------------------

namespace {

/// A rack window in Tabs with the pane chosen, over `chains` chains each holding a Drum then a Delay.
struct PaneRack {
	RealRack r;
	explicit PaneRack(size_t chains = 3, size_t withDevices = 3) {
		r.chains(chains, withDevices);
		r.set(pr::Param::Front, 1.0f);
		r.view_(1);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
	}
	ui::RackWindowPane& pane() { return r.panel->paneForTest(); }
	feathers::rack::RackBridge& chain(size_t i) { return *r.rack().chainAt(i)->bridge(); }
	ui::ChainColumn* column(size_t i) { return r.panel->railForTest(i).column(); }
	void settle() {
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
	}
	void view(size_t chainIndex) {
		r.set(pr::Param::ViewedChain, static_cast<float>(chainIndex));
		settle();
	}
};

} // namespace

TEST_CASE("rack pane: rack_front is a view-state choice (Cards by default, not modulatable, no host slot), and the window offers it in Tabs and Pads, not Strips") {
	const device::ParamDesc& d = pr::kDescriptor.params[static_cast<size_t>(pr::Param::Front)];
	CHECK(std::string(d.key) == "rack_front");
	CHECK(d.def == 0.0);
	CHECK(d.stepped);
	CHECK_FALSE(d.modulatable);
	CHECK_FALSE(d.hostSlot);
	REQUIRE(d.names.size() == 2);
	CHECK(std::string(d.names[0]) == "Cards");
	CHECK(std::string(d.names[1]) == "Window");
	RealRack r;
	r.chains(2, 2);
	CHECK(r.field(pr::Param::Front) == 0.0f);
	CHECK_FALSE(r.panel->frontSegForTest().isVisible()); // Strips
	r.view_(2);
	CHECK(r.panel->frontSegForTest().isVisible()); // Pads (the same stored choice as Tabs)
	r.view_(1);
	CHECK(r.panel->frontSegForTest().isVisible()); // Tabs
	// CARDS IS THE VIEW AS IT WAS: no pane, the first card shown in the rail.
	CHECK_FALSE(r.panel->paneOnForTest());
	CHECK(r.panel->paneForTest().hosting() == ui::RackWindowPane::Hosting::Nothing);
	CHECK_FALSE(r.panel->paneForTest().isVisible());
	REQUIRE(r.panel->railForTest(0).column() != nullptr);
	CHECK(r.panel->railForTest(0).column()->hiddenLead() == 0);
}

TEST_CASE("rack pane: the pane holds the front chain's FIRST device (by id), follows a tab switch, and is not rebuilt while nothing changes") {
	PaneRack p;
	REQUIRE(p.r.panel->paneOnForTest());
	REQUIRE(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
	const feathers::DeviceInstanceId first0 = p.chain(0).deviceAtPosition(0);
	REQUIRE(first0 != feathers::kNoInstance);
	CHECK(p.pane().device() == first0);
	CHECK(p.pane().chainIdForTest() == p.r.rack().chainIdAt(0));
	REQUIRE(p.pane().contentForTest() != nullptr);
	CHECK(p.pane().contentForTest()->watchedDevice() == first0);
	CHECK(p.pane().isVisible());
	// THE FIRST CARD IS HELD BACK FROM THE RAIL (the pane shows it) and the rest of the chain is its cards, in the lane's flow.
	ui::ChainColumn* col0 = p.column(0);
	REQUIRE(col0 != nullptr);
	CHECK(col0->hiddenLead() == 1);
	const std::vector<ui::FxCard*> cards = col0->cardsForTest();
	REQUIRE(cards.size() == 2);
	CHECK_FALSE(cards[0]->isVisible());
	CHECK(cards[1]->isVisible());
	CHECK(col0->addShown());
	// NOT REBUILT WHILE NOTHING CHANGES.
	const size_t builds = p.pane().buildsForTest();
	for (int i = 0; i < 5; ++i) {
		p.r.refresh();
	}
	CHECK(p.pane().buildsForTest() == builds);
	// A TAB SWITCH REBINDS to the other chain's first device, exactly once.
	const feathers::DeviceInstanceId first1 = p.chain(1).deviceAtPosition(0);
	REQUIRE(first1 != first0);
	p.view(1);
	CHECK(p.pane().device() == first1);
	CHECK(p.pane().chainIdForTest() == p.r.rack().chainIdAt(1));
	CHECK(p.pane().contentForTest()->watchedDevice() == first1);
	CHECK(p.pane().buildsForTest() == builds + 1);
	// AND NOTHING IS HELD BACK ON THE CHAIN NO LONGER IN FRONT.
	CHECK(p.column(0)->hiddenLead() == 0);
	CHECK(p.column(1)->hiddenLead() == 1);
	// CHOOSING CARDS PUTS THE VIEW BACK: the pane gone, the first card shown again.
	p.r.set(pr::Param::Front, 0.0f);
	p.settle();
	CHECK(p.pane().hosting() == ui::RackWindowPane::Hosting::Nothing);
	CHECK(p.column(1)->hiddenLead() == 0);
	CHECK(p.column(1)->cardsForTest()[0]->isVisible());
}

TEST_CASE("rack pane: a reorder, a replace and a delete under the pane rebind it to the device now first, or empty it") {
	PaneRack p(2, 2);
	feathers::rack::RackBridge& b = p.chain(0);
	const feathers::DeviceInstanceId drum = b.deviceAtPosition(0);
	const feathers::DeviceInstanceId delay = b.deviceAtPosition(1);
	REQUIRE(p.pane().device() == drum);
	// REORDER: the Delay goes first.
	REQUIRE(b.moveDeviceTo(delay, 0));
	p.settle();
	CHECK(p.pane().device() == delay);
	CHECK(p.pane().contentForTest()->watchedDevice() == delay);
	CHECK(p.column(0)->hiddenLead() == 1);
	// REPLACE: remove the first, add another in its place at the front.
	b.removeDevice(delay);
	const feathers::DeviceInstanceId fresh = b.addDeviceBefore(drum, "DELAY");
	REQUIRE(fresh != feathers::kNoInstance);
	p.settle();
	CHECK(p.pane().device() == fresh);
	CHECK(p.pane().contentForTest()->watchedDevice() == fresh);
	// DELETE THE FIRST: the next is first.
	b.removeDevice(fresh);
	p.settle();
	CHECK(p.pane().device() == drum);
	// DELETE THE LAST DEVICE: the pane is empty, nothing is held back, and the rail's (+) is the empty chain's.
	b.removeDevice(drum);
	p.settle();
	CHECK(p.pane().hosting() == ui::RackWindowPane::Hosting::Nothing);
	CHECK(p.pane().device() == feathers::kNoInstance);
	CHECK_FALSE(p.pane().isVisible());
	CHECK(p.column(0)->hiddenLead() == 0);
	CHECK(p.column(0)->deviceCount() == 0);
	CHECK(p.column(0)->addShown());
	// A DEVICE ADDED TO THE EMPTY CHAIN FILLS THE PANE.
	const feathers::DeviceInstanceId again = b.addDevice("DRUM");
	p.settle();
	CHECK(p.pane().device() == again);
	// DELETE THE CHAIN UNDER THE VIEW: the pane follows to the chain that is now in front (the second), by its own id.
	const feathers::DeviceInstanceId other = p.chain(1).deviceAtPosition(0);
	REQUIRE(p.r.rack().removeChain(0));
	p.settle();
	CHECK(p.r.rack().chainCount() == 1);
	CHECK(p.pane().device() == other);
	CHECK(p.pane().chainIdForTest() == p.r.rack().chainIdAt(0));
	// AND THE LAST CHAIN GOING EMPTIES IT.
	REQUIRE(p.r.rack().removeChain(0));
	p.settle();
	CHECK(p.pane().hosting() == ui::RackWindowPane::Hosting::Nothing);
	CHECK_FALSE(p.pane().isVisible());
}

TEST_CASE("rack pane: a first device with no window of its own shows its card centred in the pane; an empty chain keeps the empty chain's (+); a first device that is a rack shows its window without a pane of its own") {
	// (a) NO WINDOW: the first device whose build has none that a chain can hold.
	const device::Descriptor* plain = nullptr;
	{
		PaneRack probe(1, 0);
		for (const device::Descriptor& d : feathers::kDevices) {
			if (!ui::hasExpandedView(d) && d.traits.hasCard && probe.chain(0).descriptorFor(d.traits.tag) != nullptr && d.container == nullptr) {
				plain = &d;
				break;
			}
		}
	}
	REQUIRE(plain != nullptr);
	{
		PaneRack p(1, 0);
		const feathers::DeviceInstanceId id = p.chain(0).addDevice(plain->traits.tag);
		REQUIRE(id != feathers::kNoInstance);
		p.chain(0).addDevice("DELAY");
		p.settle();
		REQUIRE(p.pane().hosting() == ui::RackWindowPane::Hosting::Card);
		CHECK(p.pane().device() == id);
		REQUIRE(p.pane().cardForTest() != nullptr);
		CHECK(std::string(p.pane().cardForTest()->descriptor()->traits.tag) == plain->traits.tag);
		CHECK(p.pane().contentForTest() == nullptr);
		const visage::Bounds c = p.pane().cardForTest()->bounds();
		// CENTRED: equal margins left and right (within a pixel), and the pane is no wider than the card and its margins.
		CHECK(std::abs(c.x() - (static_cast<float>(p.pane().width()) - c.right())) <= 1.0f);
		CHECK(std::abs(c.y() - (static_cast<float>(p.pane().height()) - c.bottom())) <= 1.0f);
		CHECK(p.column(0)->hiddenLead() == 1);
	}
	// (b) EMPTY CHAIN: no pane, the column is the empty chain's with its (+).
	{
		PaneRack p(2, 1);
		p.view(1);
		CHECK(p.pane().hosting() == ui::RackWindowPane::Hosting::Nothing);
		CHECK_FALSE(p.pane().isVisible());
		REQUIRE(p.column(1) != nullptr);
		CHECK(p.column(1)->deviceCount() == 0);
		CHECK(p.column(1)->addShown());
		CHECK(p.column(1)->hiddenLead() == 0);
	}
	// (c) THE FIRST DEVICE IS A RACK: its window is held without a pane of its own, however it is set.
	{
		PaneRack p(1, 0);
		const feathers::DeviceInstanceId inner = nestedRack(p.chain(0), 2, true);
		p.chain(0).setField(inner, static_cast<size_t>(pr::Param::Front), 1.0f);
		// THE INNER RACK'S FRONT CHAIN HOLDS A DEVICE, so a pane of its own would have something to show.
		REQUIRE(pr::rackOf(p.chain(0).containerSurfaceOf(inner))->chainAt(0)->bridge()->addDevice("DELAY") != feathers::kNoInstance);
		p.settle();
		REQUIRE(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
		auto* innerPanel = dynamic_cast<ui::ParallelRackExpanded*>(p.pane().contentForTest());
		REQUIRE(innerPanel != nullptr);
		CHECK(innerPanel->watchedDevice() == inner);
		CHECK(innerPanel->hostedInPaneForTest());
		for (int i = 0; i < 3; ++i) {
			(void)innerPanel->update();
		}
		// IT IS IN TABS WITH THE WINDOW CHOSEN, AND HAS NO PANE: it lays out as with Cards, and offers no choice that would do nothing.
		CHECK(innerPanel->viewModelForTest().view() == ui::RackViewKind::Tabs);
		CHECK_FALSE(innerPanel->paneOnForTest());
		CHECK(innerPanel->paneForTest().hosting() == ui::RackWindowPane::Hosting::Nothing);
		CHECK_FALSE(innerPanel->paneForTest().isVisible());
		CHECK_FALSE(innerPanel->frontSegForTest().isVisible());
	}
}

TEST_CASE("rack pane: a control turned in the pane is the parameter the device's card shows") {
	PaneRack p(1, 0);
	const feathers::DeviceInstanceId id = p.chain(0).addDevice("DELAY");
	p.chain(0).addDevice("DRUM");
	p.settle();
	REQUIRE(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
	REQUIRE(p.pane().device() == id);
	// A CONTROL OFFERED IN THE PANE (asked of every face in it), and the same-keyed one offered by the card the rail holds back.
	const auto offered = [](visage::Frame* root) {
		std::vector<device::Modulatable*> out;
		std::function<void(visage::Frame*)> walk = [&](visage::Frame* f) {
			if (const auto* face = dynamic_cast<const ui::ControlFace*>(f); face != nullptr) {
				std::vector<ui::ControlFace::Offered> got;
				face->offeredControls(got);
				for (const auto& o : got) {
					if (o.control != nullptr) {
						out.push_back(const_cast<device::Modulatable*>(o.control));
					}
				}
			}
			for (visage::Frame* c : f->children()) {
				walk(c);
			}
		};
		walk(root);
		return out;
	};
	device::Modulatable* inPane = nullptr;
	for (device::Modulatable* c : offered(p.pane().contentForTest())) {
		if (!c->desc().stepped && c->desc().max > c->desc().min) {
			inPane = c;
			break;
		}
	}
	REQUIRE(inPane != nullptr);
	const device::ParamDesc& desc = inPane->desc();
	ui::FxCard* held = p.column(0)->cardsForTest()[0];
	REQUIRE(held != nullptr);
	device::Modulatable* onCard = nullptr;
	for (device::Modulatable* c : offered(held)) {
		if (std::string(c->desc().key) == desc.key) {
			onCard = c;
		}
	}
	REQUIRE(onCard != nullptr);
	const double target = desc.min + (desc.max - desc.min) * 0.75;
	inPane->beginGesture();
	inPane->setValue(target);
	inPane->endGesture();
	// THE HOST'S FIELD (what the engine reads and every view shows), and the card's own control, hold it.
	size_t control = SIZE_MAX;
	const device::Descriptor* owner = p.pane().contentForTest()->watchedDescriptor();
	for (size_t i = 0; i < owner->params.size(); ++i) {
		if (std::string(owner->params[i].key) == desc.key) {
			control = i;
		}
	}
	REQUIRE(control != SIZE_MAX);
	CHECK(p.chain(0).fieldAt(id, control) == doctest::Approx(static_cast<float>(target)).epsilon(1e-4));
	CHECK(onCard->value() == doctest::Approx(inPane->value()));
	// AND IT IS ONE UNDO STEP in the rail's history, which an undo takes back on both.
	CHECK(p.r.panel->railForTest(0).column()->historyForTest().canUndo());
}

TEST_CASE("rack pane: the content's extent is the device window's natural extent, at 1x and 1.75x, and the bounds follow a scale change") {
	PaneRack p(2, 2);
	visage::Frame root;
	root.setBounds(0.0f, 0.0f, 1400.0f, 900.0f);
	root.addChild(p.r.panel);
	struct Detach {
		visage::Frame& root;
		visage::Frame* child;
		~Detach() { root.removeChild(child); }
	} detach { root, p.r.panel };
	for (const float scale : { 1.0f, 1.75f, 1.0f }) {
		CAPTURE(scale);
		root.setDpiScale(scale);
		p.r.panel->setBounds(0.0f, 0.0f, 1400.0f, 900.0f);
		p.r.panel->resized();
		p.settle();
		REQUIRE(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
		// THE DEVICE'S OWN WINDOW, built the way its card opens it, is what the pane holds.
		const feathers::DeviceInstanceId id = p.pane().device();
		const device::Descriptor* desc = p.chain(0).descriptorOf(id);
		REQUIRE(desc != nullptr);
		auto own = ui::makeExpandedView(p.chain(0), nullptr, p.r.registry, id, *desc);
		REQUIRE(own != nullptr);
		const visage::Point want = own->minimumSize();
		const visage::Bounds got = p.pane().contentBounds();
		CHECK(got.width() == doctest::Approx(want.x));
		CHECK(got.height() == doctest::Approx(want.y));
		// NATIVE WIDTHS: the content's and the pane's follow the scale (a scale change moves no logical rectangle).
		ui::ExpandedView* content = p.pane().contentForTest();
		CHECK(std::abs(static_cast<float>(content->nativeWidth()) - scale * want.x) <= 1.0f);
		CHECK(std::abs(static_cast<float>(content->nativeHeight()) - scale * want.y) <= 1.0f);
		CHECK(std::abs(static_cast<float>(p.pane().nativeWidth()) - scale * p.pane().naturalSize().x) <= 1.0f);
		// THE WINDOW SIZES TO WHAT IS IN FRONT: its minimum holds the pane and the rail beside it, not the sum of every tab.
		const visage::Point minimum = p.r.panel->minimumSize();
		CHECK(minimum.x >= p.pane().naturalSize().x);
		CHECK(minimum.y >= p.pane().naturalSize().y);
	}
}

TEST_CASE("rack views over a real rack: the cards' expand marks survive a tab switch, a view toggle and the window's panel being destroyed, on the card's front") {
	RealRack r;
	r.chains(2, 2, true); // Drum then Delay on each chain
	r.view_(1);
	for (int i = 0; i < 4; ++i) {
		r.refresh();
	}
	const auto frontMarks = [&] {
		float widest = 0.0f;
		for (ui::FxCard* c : shownCards(*r.card)) {
			widest = std::max(widest, c->expandBounds().width());
		}
		return widest;
	};
	CHECK(frontMarks() > 0.0f);
	// A TAB SWITCH rebuilds the front column over another chain's bridge, then back.
	ui::RackTabBar& bar = r.card->tabBarForTest();
	bar.pressForTest(bar.tabCentre(1));
	r.refresh();
	CHECK(frontMarks() > 0.0f);
	bar.pressForTest(bar.tabCentre(0));
	r.refresh();
	CHECK(frontMarks() > 0.0f);
	// THE VIEW TOGGLED Tabs -> Strips -> Pads -> Tabs.
	for (const int v : { 0, 2, 1 }) {
		r.view_(v);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
	}
	CHECK(frontMarks() > 0.0f);
	// THE WINDOW'S OWN COLUMN CLOSED (its panel destroyed) and the card's marks stay; reopened, both show.
	r.view.reset();
	r.panel = nullptr;
	for (int i = 0; i < 3; ++i) {
		(void)r.card->update();
		r.card->resized();
	}
	CHECK(frontMarks() > 0.0f);
}

namespace {

/// Every FxCard under `frame` (a card inside a card too), depth first.
void collectCards(visage::Frame* frame, std::vector<ui::FxCard*>& out) {
	if (auto* card = dynamic_cast<ui::FxCard*>(frame)) {
		out.push_back(card);
	}
	for (visage::Frame* child : frame->children()) {
		collectCards(child, out);
	}
}

std::string gBadMarks;

/// Every card under `frame` whose device has a window must draw its expand mark: a rectangle that is not empty and lies inside the card's own
/// bounds (a chip that is present but clipped or placed outside the card is as missing as one not drawn). Returns the cards checked.
size_t checkMarks(visage::Frame* frame, const char* where) {
	std::vector<ui::FxCard*> cards;
	collectCards(frame, cards);
	size_t checked = 0;
	for (ui::FxCard* card : cards) {
		const device::Descriptor* d = card->descriptor();
		if (d == nullptr || !ui::hasExpandedView(*d) || !card->isVisible() || card->width() <= 0 || card->height() <= 0) {
			continue;
		}
		// A card held back by its column (the pane's lead) or beyond the fold of its parent draws nothing and has no mark to lose.
		if (card->x() < 0 && card->x() + card->width() <= 0) {
			continue;
		}
		const visage::Bounds m = card->expandBounds();
		const bool inside = m.width() > 0.0f && m.height() > 0.0f && m.x() >= -0.01f && m.y() >= -0.01f
		                    && m.x() + m.width() <= static_cast<float>(card->width()) + 0.01f
		                    && m.y() + m.height() <= static_cast<float>(card->height()) + 0.01f;
		if (!inside) {
			gBadMarks += std::string("[") + where + "] " + d->traits.name + " card " + std::to_string(card->width()) + "x"
			             + std::to_string(card->height()) + " mark " + std::to_string(m.x()) + "," + std::to_string(m.y()) + " "
			             + std::to_string(m.width()) + "x" + std::to_string(m.height()) + " folded=" + (card->folded() ? "1" : "0")
			             + " nest=" + std::to_string(card->nest().depth) + "\n";
		}
		++checked;
	}
	return checked;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
// THE PADS VIEW'S FOUR FIXES: the viewed chain has one source of truth, a new chain starts open, both pad readers see a new chain on the
// tick it exists, and "+" asks which device.
// ---------------------------------------------------------------------------------------------------------------------------------------

namespace {

/// The viewed chain as each reader sees it: the stored parameter, the window's Viewing box, the pad grid's selected cell on each surface, the
/// chain the front column shows (card) and the rail in front (window).
struct ViewedReaders {
	int field = -1;
	double box = -1.0;
	int gridWindow = -1;
	int gridCard = -1;
	int railWindow = -1;
	int front = -1;
};

ViewedReaders readViewed(RealRack& r) {
	ViewedReaders v;
	v.field = static_cast<int>(r.field(pr::Param::ViewedChain));
	v.box = r.panel->viewedBoxForTest().control().value();
	// THE GRID'S SELECTED CELL AS A CHAIN SLOT: the chain whose key that cell stands for.
	const auto slotOfSelected = [&](ui::RackPadGrid& grid, ui::RackViewModel& model) {
		for (int i = 0; i < ui::kPadsPerPage; ++i) {
			if (grid.selectedForTest(i)) {
				return static_cast<int>(ui::chainForKey(model, ui::padKey(model.page(), i)));
			}
		}
		return -1;
	};
	v.gridWindow = slotOfSelected(r.panel->padsForTest().gridForTest(), r.panel->viewModelForTest());
	v.gridCard = slotOfSelected(r.card->padsForTest().gridForTest(), r.card->viewModelForTest());
	for (size_t i = 0; i < r.rack().chainCount(); ++i) {
		if (r.panel->railShownForTest(i)) {
			v.railWindow = static_cast<int>(i);
		}
	}
	v.front = r.card->frontForTest().chainIdForTest() == 0xFFFF ? -1 : static_cast<int>(r.rack().indexOfChain(r.card->frontForTest().chainIdForTest()));
	return v;
}

} // namespace

TEST_CASE("rack views over a real rack: every card with a window keeps a non-empty expand mark inside its own bounds on every surface (card front, window rail, pane), through the window opening, Cards|Window, view and pad changes, a nested rack and a chain removal, at 1x and 1.75x") {
	for (const float dpi : { 1.0f, 1.75f }) {
		CAPTURE(dpi);
		gBadMarks.clear();
		ui::SurfaceLayers outer; // what the real window is presented on (declared first: it outlives the surfaces over it)
		outer.setBounds(0, 0, 1400, 900);
		RealRack r;
		r.f.root->setDpiScale(dpi);
		r.panel->setPresentedOn(&outer);
		r.chains(3, 2, true); // Drum then Delay on each of three chains
		(void)nestedRack(*r.rack().chainAt(0)->bridge(), 2, true);
		for (size_t c = 0; c < r.rack().chainCount(); ++c) {
			r.rack().setOpen(c, true); // the window's rails open, so their cards are on screen
		}
		r.view_(1);
		const auto sweep = [&](const char* step) {
			for (int i = 0; i < 4; ++i) {
				r.refresh();
			}
			if (r.card != nullptr) {
				CHECK(checkMarks(r.card, (std::string("card: ") + step).c_str()) > 0);
			}
			if (r.panel != nullptr) {
				CHECK(checkMarks(r.panel, (std::string("window: ") + step).c_str()) > 0);
			}
		};
		sweep("window open, Tabs");
		r.set(pr::Param::Front, 1.0f); // the window's pane holds the first device's own window; the first card is held back
		sweep("Front = Window");
		r.set(pr::Param::Front, 0.0f);
		sweep("Front = Cards again");
		r.view_(2);
		ui::RackPadGrid& grid = r.card->padsForTest().gridForTest();
		grid.pressForTest(grid.padCentre(1));
		grid.releaseForTest();
		sweep("Pads, pad 2");
		r.view_(0);
		sweep("Strips");
		r.view_(1);
		sweep("Tabs again");
		REQUIRE(r.rack().removeChain(2));
		sweep("a chain removed while both surfaces show the rack");
		r.view.reset(); // the window closed: the card's marks stay
		r.panel = nullptr;
		for (int i = 0; i < 3; ++i) {
			(void)r.card->update();
			r.card->resized();
		}
		CHECK(checkMarks(r.card, "window closed") > 0);
		CAPTURE(gBadMarks);
		CHECK(gBadMarks.empty());
	}
}

TEST_CASE("rack pads fix E: every kind of selection writes rack_viewed_chain, and the field, the Viewing box, both grids, the front column and the rail agree") {
	RealRack r;
	r.chains(3, 3, true); // chains on keys 0, 1, 2
	r.view_(2);
	r.set(pr::Param::PadsPage, 0.0f);
	r.refresh();
	const auto allAgree = [&](int want, const char* how) {
		CAPTURE(how);
		const ViewedReaders v = readViewed(r);
		CHECK(v.field == want);
		CHECK(v.box == doctest::Approx(static_cast<double>(want)));
		// THE BOX READS THE CHAIN'S ONE LABEL (never its number).
		CHECK(r.panel->viewedBoxForTest().control().text() == r.panel->viewModelForTest().chainLabel(static_cast<size_t>(want)));
		CHECK(v.gridWindow == want);
		CHECK(v.gridCard == want);
		CHECK(v.front == want);
	};
	// A PAD PRESSED IN THE WINDOW'S GRID.
	{
		auto& grid = r.panel->padsForTest().gridForTest();
		grid.pressForTest(grid.padCentre(2));
		grid.releaseForTest();
		r.refresh();
		allAgree(2, "pad in the window");
	}
	// A PAD PRESSED IN THE CARD'S GRID.
	{
		auto& grid = r.card->padsForTest().gridForTest();
		grid.pressForTest(grid.padCentre(1));
		grid.releaseForTest();
		r.refresh();
		allAgree(1, "pad on the card");
	}
	// TYPING IN THE VIEWING BOX moves the selection: "3" is the third chain, and words are refused.
	{
		auto& control = r.panel->viewedBoxForTest().control();
		const std::optional<double> three = control.parseText("3", 0.0);
		REQUIRE(three.has_value());
		CHECK(*three == 2.0);
		CHECK(control.parseText("0", 0.0).value_or(-1.0) == 0.0); // below the first is the first
		CHECK_FALSE(control.parseText("two", 0.0).has_value());
		control.setValue(*three);
		r.refresh();
		allAgree(2, "typed 3 in the Viewing box");
		control.setValue(0.0);
		r.refresh();
		allAgree(0, "typed in the Viewing box");
	}
	// A TAB, in Tabs, on the window's bar and on the card's.
	r.view_(1);
	{
		auto& bar = r.panel->tabBarForTest();
		bar.pressForTest(bar.tabCentre(2));
		r.refresh();
		CHECK(readViewed(r).field == 2);
		CHECK(readViewed(r).box == doctest::Approx(2.0));
		CHECK(readViewed(r).railWindow == 2);
		CHECK(readViewed(r).front == 2);
		auto& cardBar = r.card->tabBarForTest();
		cardBar.pressForTest(cardBar.tabCentre(1));
		r.refresh();
		CHECK(readViewed(r).field == 1);
		CHECK(readViewed(r).box == doctest::Approx(1.0));
		CHECK(readViewed(r).railWindow == 1);
		CHECK(readViewed(r).front == 1);
	}
}

namespace {

/// The menu row id of the device with tag `tag` in a pad menu's offer.
int rowOf(const std::vector<const device::Descriptor*>& offered, const char* tag) {
	for (size_t i = 0; i < offered.size(); ++i) {
		if (std::string(offered[i]->traits.tag) == tag) {
			return static_cast<int>(i);
		}
	}
	return -1;
}

/// The keys of the chains, as the rack holds them, and the pads each reader draws used on `page`: they must be the same set.
std::set<int> keysOf(RealRack& r) {
	std::set<int> keys;
	for (size_t i = 0; i < r.rack().chainCount(); ++i) {
		if (r.rack().chainKey(i) != pr::kAnyKey) {
			keys.insert(r.rack().chainKey(i));
		}
	}
	return keys;
}

std::set<int> usedOn(ui::RackPadGrid& grid, int page) {
	std::set<int> used;
	for (int i = 0; i < ui::kPadsPerPage; ++i) {
		if (grid.usedForTest(i)) {
			used.insert(ui::padKey(page, i));
		}
	}
	return used;
}

std::set<int> usedInStrip(ui::RackPadPages& pages, int page) {
	std::set<int> used;
	for (int i = 0; i < ui::kPadsPerPage; ++i) {
		if (pages.cellUsedForTest(page, i)) {
			used.insert(ui::padKey(page, i));
		}
	}
	return used;
}

} // namespace

TEST_CASE("rack pads fix A: a pad's + asks which device (the lanes' menu, oscillators first), the pick makes the keyed chain holding it, closing makes nothing, on the card and the window") {
	for (const bool onCard : { false, true }) {
		CAPTURE(onCard);
		RealRack r;
		r.chains(2, 2, true); // keys 0 and 1
		r.view_(2);
		ui::RackPadAdd& add = onCard ? r.card->padAddForTest() : r.panel->padAddForTest();
		ui::RackPadGrid& grid = onCard ? r.card->padsForTest().gridForTest() : r.panel->padsForTest().gridForTest();
		const std::vector<const device::Descriptor*> offered = add.offeredForTest();
		REQUIRE(offered.size() > 10); // any device a chain can hold, not a Drum alone
		// THE MENU: the Oscillators heading first, a Drum under it, and the other categories after.
		visage::PopupMenu menu = add.menuForTest(5);
		REQUIRE_FALSE(menu.options().empty());
		CHECK(menu.options().front().name().toUtf8() == "Oscillators");
		std::vector<std::string> first;
		for (const visage::PopupMenu& row : menu.options().front().options()) {
			first.push_back(row.name().toUtf8());
		}
		CHECK(std::find(first.begin(), first.end(), "Drum") != first.end());
		CHECK(menu.options().size() > 3);
		// A PRESS ON THE "+" OPENS A MENU AND MAKES NOTHING; closing it (no row picked) leaves the rack as it was.
		const size_t before = r.rack().chainCount();
		grid.pressForTest(grid.plusCentre(5));
		grid.releaseForTest();
		r.refresh();
		CHECK(r.rack().chainCount() == before);
		CHECK(add.pickForTest(5, -1) == ui::kNoChain);
		CHECK(r.rack().chainCount() == before);
		// A PICK: a Delay (not a Drum) makes the chain on key 5 holding a Delay, and views it.
		const int delay = rowOf(offered, "DELAY");
		REQUIRE(delay >= 0);
		const size_t at = add.pickForTest(5, delay);
		REQUIRE(at == before);
		REQUIRE(r.rack().chainCount() == before + 1);
		CHECK(r.rack().chainKey(at) == 5);
		feathers::rack::RackBridge& bridge = *r.rack().chainAt(at)->bridge();
		REQUIRE(bridge.chainLength() == 1);
		CHECK(std::string(bridge.descriptorOf(bridge.deviceAtPosition(0))->traits.tag) == "DELAY");
		CHECK(r.field(pr::Param::ViewedChain) == static_cast<float>(at));
		// AND A DRUM FROM THE SAME MENU, on the next pad: held, set to Any Key.
		const size_t drumAt = add.pickForTest(6, rowOf(offered, "DRUM"));
		REQUIRE(drumAt == before + 1);
		feathers::rack::RackBridge& drumBridge = *r.rack().chainAt(drumAt)->bridge();
		CHECK(std::string(drumBridge.descriptorOf(drumBridge.deviceAtPosition(0))->traits.tag) == "DRUM");
		CHECK(r.rack().chainKey(drumAt) == 6);
		// A ROW THAT IS NOT A DEVICE (a heading's id) makes nothing.
		CHECK(add.pickForTest(7, 9999) == ui::kNoChain);
		CHECK(r.rack().chainCount() == before + 2);
	}
}

TEST_CASE("rack pads fix B: a chain made by a pad's + starts open, with its first card unfolded, in the window and on the card") {
	for (const int view : { 2, 1, 0 }) {
		CAPTURE(view);
		RealRack r;
		r.chains(2, 2, true); // keys 0 and 1
		r.view_(view);
		r.set(pr::Param::PadsPage, 0.0f);
		r.refresh();
		const size_t before = r.rack().chainCount();
		const int drum = rowOf(r.panel->padAddForTest().offeredForTest(), "DRUM");
		REQUIRE(drum >= 0);
		REQUIRE(r.panel->padAddForTest().pickForTest(5, drum) == before);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		const size_t at = before;
		// THE CHAIN'S OWN OPEN FLAG (what the card's arrow and the window's rail are brought to), and the rail and its first card as drawn.
		CHECK(r.rack().open(at));
		CHECK_FALSE(r.panel->railForTest(at).collapsed());
		ui::ChainColumn* column = r.panel->railForTest(at).column();
		REQUIRE(column != nullptr);
		REQUIRE(column->cardsForTest().size() >= 1);
		CHECK_FALSE(column->cardsForTest()[0]->folded());
		CHECK_FALSE(r.rack().chainAt(at)->bridge()->deviceFolded(r.rack().chainAt(at)->bridge()->deviceAtPosition(0)));
	}
}

TEST_CASE("rack pads fix D: both pad readers (the 4x4 grid and the page strip) show a new chain's key on the tick it exists, on the card and the window, whichever surface made it") {
	RealRack r;
	r.chains(2, 2, true); // keys 0 and 1
	r.view_(2);
	r.set(pr::Param::PadsPage, 0.0f);
	r.refresh();
	const auto check = [&](const char* how) {
		CAPTURE(how);
		const std::set<int> keys = keysOf(r);
		CHECK(usedOn(r.panel->padsForTest().gridForTest(), 0) == keys);
		CHECK(usedOn(r.card->padsForTest().gridForTest(), 0) == keys);
		CHECK(usedInStrip(r.panel->padsForTest().pagesForTest(), 0) == keys);
		CHECK(usedInStrip(r.card->padsForTest().pagesForTest(), 0) == keys);
	};
	check("start");
	const std::vector<const device::Descriptor*> offered = r.panel->padAddForTest().offeredForTest();
	// MADE ON THE WINDOW: every reader, on both surfaces, shows it BEFORE any tick and after exactly one.
	REQUIRE(r.panel->padAddForTest().pickForTest(5, rowOf(offered, "DRUM")) != ui::kNoChain);
	check("window add, before a tick");
	(void)r.panel->update();
	(void)r.card->update();
	check("window add, one tick");
	// MADE ON THE CARD, read on the window.
	REQUIRE(r.card->padAddForTest().pickForTest(9, rowOf(offered, "DELAY")) != ui::kNoChain);
	check("card add, before a tick");
	(void)r.card->update();
	(void)r.panel->update();
	check("card add, one tick");
	// MADE BY THE RACK ITSELF (a load, an undo, another surface): the model's own add.
	REQUIRE(r.panel->viewModelForTest().addKeyedChain(12, nullptr) != ui::kNoChain);
	(void)r.card->update();
	(void)r.panel->update();
	check("model add, one tick");
	CHECK(keysOf(r) == std::set<int> { 0, 1, 5, 9, 12 });
	// THE STRIP'S PAGE FOR ANOTHER PAGE follows too: a chain on key 40 lights page 2's cell 8, in both surfaces' strips.
	REQUIRE(r.panel->viewModelForTest().addKeyedChain(40, nullptr) != ui::kNoChain);
	(void)r.card->update();
	(void)r.panel->update();
	CHECK(usedInStrip(r.panel->padsForTest().pagesForTest(), 2) == std::set<int> { 40 });
	CHECK(usedInStrip(r.card->padsForTest().pagesForTest(), 2) == std::set<int> { 40 });
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// THE PANE IN PADS (rack_front is the one stored choice, shared with Tabs): the selected pad's chain's first device's window.
// ---------------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("rack pane in Pads: the pane follows the pad selected, the grid, strip and kit level stay beside it, Strips ignores the choice, and the choice is one value for Tabs and Pads") {
	PaneRack p(3, 3);
	p.r.chains(3, 3, true); // keys 0, 1, 2
	p.r.set(pr::Param::Front, 1.0f);
	p.r.view_(2);
	p.r.set(pr::Param::PadsPage, 0.0f);
	p.settle();
	REQUIRE(p.r.panel->paneOnForTest());
	CHECK(p.r.panel->frontSegForTest().isVisible());
	REQUIRE(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
	const feathers::DeviceInstanceId first0 = p.chain(0).deviceAtPosition(0);
	CHECK(p.pane().device() == first0);
	CHECK(p.pane().chainIdForTest() == p.r.rack().chainIdAt(0));
	CHECK(p.column(0)->hiddenLead() == 1);
	// THE PADS' PIECES ARE STILL THERE, and the pane stands clear of them.
	ui::RackPadsPanel& pads = p.r.panel->padsForTest();
	CHECK(pads.isVisible());
	CHECK(pads.gridForTest().isVisible());
	CHECK(pads.pagesForTest().isVisible());
	REQUIRE(pads.levelForTest() != nullptr);
	CHECK(pads.levelForTest()->isVisible());
	CHECK(p.pane().isVisible());
	CHECK(static_cast<float>(p.pane().x()) >= static_cast<float>(pads.x() + pads.width()));
	// A PAD SELECTED REBINDS THE PANE to that chain's first device, once.
	const size_t builds = p.pane().buildsForTest();
	auto& grid = pads.gridForTest();
	grid.pressForTest(grid.padCentre(2));
	grid.releaseForTest();
	p.settle();
	const feathers::DeviceInstanceId first2 = p.chain(2).deviceAtPosition(0);
	CHECK(p.pane().device() == first2);
	CHECK(p.pane().chainIdForTest() == p.r.rack().chainIdAt(2));
	CHECK(p.pane().contentForTest()->watchedDevice() == first2);
	CHECK(p.pane().buildsForTest() == builds + 1);
	CHECK(p.column(0)->hiddenLead() == 0);
	CHECK(p.column(2)->hiddenLead() == 1);
	// THE CHAIN UNDER THE SELECTED PAD DELETED: the pane follows to the chain now selected.
	REQUIRE(p.r.rack().removeChain(2));
	p.settle();
	CHECK(p.r.rack().chainCount() == 2);
	REQUIRE(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
	CHECK(p.pane().chainIdForTest() == p.r.rack().chainIdAt(p.r.panel->viewModelForTest().viewedChain()));
	// STRIPS IGNORES IT: no pane, no choice.
	p.r.view_(0);
	p.settle();
	CHECK_FALSE(p.r.panel->paneOnForTest());
	CHECK(p.pane().hosting() == ui::RackWindowPane::Hosting::Nothing);
	CHECK_FALSE(p.r.panel->frontSegForTest().isVisible());
	// ONE STORED VALUE: back in Tabs the choice is still Window, and the pane is there.
	p.r.view_(1);
	p.settle();
	CHECK(p.r.field(pr::Param::Front) == 1.0f);
	CHECK(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
	p.r.view_(2);
	p.settle();
	CHECK(p.pane().hosting() == ui::RackWindowPane::Hosting::Window);
}

TEST_CASE("rack pane in Pads: an empty rack or chain shows no pane and keeps the (+); a first device with no window shows its card; a rack in the pane has no pane of its own") {
	// NO CHAIN AT ALL: nothing to host.
	{
		RealRack r;
		r.set(pr::Param::Front, 1.0f);
		r.view_(2);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		CHECK(r.panel->paneForTest().hosting() == ui::RackWindowPane::Hosting::Nothing);
		CHECK_FALSE(r.panel->paneForTest().isVisible());
		CHECK(r.panel->padsForTest().isVisible());
	}
	// AN EMPTY SELECTED CHAIN: no pane, the column is the empty chain's with its (+).
	{
		RealRack r;
		r.chains(2, 0, true);
		r.set(pr::Param::Front, 1.0f);
		r.view_(2);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		CHECK(r.panel->paneForTest().hosting() == ui::RackWindowPane::Hosting::Nothing);
		REQUIRE(r.panel->railForTest(0).column() != nullptr);
		CHECK(r.panel->railForTest(0).column()->addShown());
		CHECK(r.panel->railForTest(0).column()->hiddenLead() == 0);
	}
	// NO WINDOW OF ITS OWN: the card, centred.
	{
		RealRack r;
		r.chains(1, 0, true);
		const device::Descriptor* plain = nullptr;
		for (const device::Descriptor& d : feathers::kDevices) {
			if (!ui::hasExpandedView(d) && d.traits.hasCard && r.rack().chainAt(0)->bridge()->descriptorFor(d.traits.tag) != nullptr && d.container == nullptr) {
				plain = &d;
				break;
			}
		}
		REQUIRE(plain != nullptr);
		const feathers::DeviceInstanceId id = r.rack().chainAt(0)->bridge()->addDevice(plain->traits.tag);
		r.set(pr::Param::Front, 1.0f);
		r.view_(2);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		CHECK(r.panel->paneForTest().hosting() == ui::RackWindowPane::Hosting::Card);
		CHECK(r.panel->paneForTest().device() == id);
	}
	// A RACK AS THE FIRST DEVICE: its window, laid out with no pane of its own.
	{
		RealRack r;
		r.chains(1, 0, true);
		const feathers::DeviceInstanceId inner = nestedRack(*r.rack().chainAt(0)->bridge(), 2, false);
		r.rack().chainAt(0)->bridge()->setField(inner, static_cast<size_t>(pr::Param::View), 2.0f);
		r.rack().chainAt(0)->bridge()->setField(inner, static_cast<size_t>(pr::Param::Front), 1.0f);
		REQUIRE(pr::rackOf(r.rack().chainAt(0)->bridge()->containerSurfaceOf(inner))->chainAt(0)->bridge()->addDevice("DELAY") != feathers::kNoInstance);
		r.set(pr::Param::Front, 1.0f);
		r.view_(2);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		REQUIRE(r.panel->paneForTest().hosting() == ui::RackWindowPane::Hosting::Window);
		auto* innerPanel = dynamic_cast<ui::ParallelRackExpanded*>(r.panel->paneForTest().contentForTest());
		REQUIRE(innerPanel != nullptr);
		for (int i = 0; i < 3; ++i) {
			(void)innerPanel->update();
		}
		CHECK(innerPanel->hostedInPaneForTest());
		CHECK_FALSE(innerPanel->paneOnForTest());
		CHECK(innerPanel->paneForTest().hosting() == ui::RackWindowPane::Hosting::Nothing);
		CHECK_FALSE(innerPanel->frontSegForTest().isVisible());
	}
}

TEST_CASE("a new chain starts open from the window's + Chain and from a Tabs or Pads shortcut's own chains too") {
	RealRack r;
	r.chains(1, 1, true);
	r.view_(0);
	r.panel->addChainForTest();
	r.refresh();
	CHECK(r.rack().open(r.rack().chainCount() - 1));
	CHECK_FALSE(r.panel->railForTest(r.rack().chainCount() - 1).collapsed());
	// THE SHORTCUTS (Tabs, Pads) make a Rack in a chain: its own chains start open.
	ui::ChainColumn* column = r.panel->railForTest(0).column();
	REQUIRE(column != nullptr);
	const size_t offered = column->addable().size();
	visage::PopupMenu menu = column->addMenuForTest();
	for (const visage::PopupMenu& entry : menu.options()) {
		if (entry.name().toUtf8() != "Containers") {
			continue;
		}
		for (const visage::PopupMenu& row : entry.options()) {
			if (row.name().toUtf8() == "Tabs" || row.name().toUtf8() == "Pads") {
				REQUIRE(row.id() >= static_cast<int>(offered));
				feathers::rack::RackBridge& chain = *r.rack().chainAt(0)->bridge();
				const size_t before = chain.chainLength();
				menu.onSelection().callback(row.id());
				REQUIRE(chain.chainLength() == before + 1);
				pr::Rack* inner = pr::rackOf(chain.containerSurfaceOf(chain.deviceAtPosition(before)));
				REQUIRE(inner != nullptr);
				REQUIRE(inner->chainCount() >= 1);
				for (size_t i = 0; i < inner->chainCount(); ++i) {
					CHECK(inner->open(i));
				}
			}
		}
	}
}

TEST_CASE("rack pads fix D, repaint delivery: every pad grid and page strip showing the rack is asked to redraw within the tick a chain is added, keyed or given a device, on both surfaces; a pad is populated by a chain with its key, not by a device in it") {
	RealRack r;
	r.chains(2, 2, true); // keys 0 and 1
	r.view_(2);
	r.set(pr::Param::PadsPage, 0.0f);
	r.refresh();
	struct Counts {
		size_t gridWindow, gridCard, stripWindow, stripCard;
	};
	const auto counts = [&] {
		return Counts { r.panel->padsForTest().gridForTest().refreshesForTest(), r.card->padsForTest().gridForTest().refreshesForTest(),
		                r.panel->padsForTest().pagesForTest().refreshesForTest(), r.card->padsForTest().pagesForTest().refreshesForTest() };
	};
	// ONE TICK OF BOTH SURFACES, then every reader on both must have been asked to redraw since `before`.
	const auto tickAndCheck = [&](const Counts& before, const char* how) {
		CAPTURE(how);
		(void)r.panel->update();
		(void)r.card->update();
		const Counts after = counts();
		CHECK(after.gridWindow > before.gridWindow);
		CHECK(after.gridCard > before.gridCard);
		CHECK(after.stripWindow > before.stripWindow);
		CHECK(after.stripCard > before.stripCard);
	};
	const std::vector<const device::Descriptor*> offered = r.panel->padAddForTest().offeredForTest();
	// A CHAIN ADDED FROM THE WINDOW'S MENU PICK, THE CARD'S, AND THE MODEL'S OWN ADD.
	{
		const Counts before = counts();
		REQUIRE(r.panel->padAddForTest().pickForTest(5, rowOf(offered, "DRUM")) != ui::kNoChain);
		tickAndCheck(before, "window pick");
	}
	{
		const Counts before = counts();
		REQUIRE(r.card->padAddForTest().pickForTest(6, rowOf(offered, "DELAY")) != ui::kNoChain);
		tickAndCheck(before, "card pick");
	}
	{
		const Counts before = counts();
		REQUIRE(r.panel->viewModelForTest().addKeyedChain(7, nullptr) != ui::kNoChain);
		tickAndCheck(before, "model add");
	}
	// A CHAIN KEYED AFTER IT EXISTS (the strip's key control, by the rack's own setter): the pad shows on the next tick.
	{
		const size_t any = r.rack().addChain();
		REQUIRE(any != SIZE_MAX);
		r.refresh();
		CHECK_FALSE(usedOn(r.panel->padsForTest().gridForTest(), 0).count(9) > 0);
		const Counts before = counts();
		r.rack().setChainKey(any, 9);
		tickAndCheck(before, "chain keyed");
		CHECK(usedOn(r.panel->padsForTest().gridForTest(), 0).count(9) == 1);
		CHECK(usedOn(r.card->padsForTest().gridForTest(), 0).count(9) == 1);
	}
	// A DEVICE ADDED TO A CHAIN THAT HAS A KEY AND NO DEVICE: populated already (the key is what populates), and every reader is still told.
	{
		const size_t empty = r.rack().addChain();
		REQUIRE(empty != SIZE_MAX);
		r.rack().setChainKey(empty, 10);
		r.refresh();
		CHECK(usedOn(r.panel->padsForTest().gridForTest(), 0).count(10) == 1); // before any device: the key alone populates it
		const Counts before = counts();
		(void)r.rack().chainAt(empty)->bridge()->addDevice("DELAY");
		tickAndCheck(before, "device added to a keyed chain");
		CHECK(usedOn(r.panel->padsForTest().gridForTest(), 0).count(10) == 1);
	}
	// A DEVICE ADDED TO A CHAIN WITH NO KEY (an Any chain made by "+ Chain"): the pad stays EMPTY, because populated means a chain on that key.
	{
		const std::set<int> usedBefore = usedOn(r.panel->padsForTest().gridForTest(), 0);
		const size_t anyChain = r.rack().addChain();
		REQUIRE(anyChain != SIZE_MAX);
		(void)r.rack().chainAt(anyChain)->bridge()->addDevice("DRUM");
		r.refresh();
		CHECK(r.rack().chainKey(anyChain) == pr::kAnyKey);
		CHECK(usedOn(r.panel->padsForTest().gridForTest(), 0) == usedBefore);
		CHECK(usedOn(r.card->padsForTest().gridForTest(), 0) == usedBefore);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// LAYOUT POLISH: no bumper before the first card, the Pads arrows in the Tabs arrow's language at a fixed place, a thin tab column in a
// left-right lane (nested racks stacking left to right), and the Strips window's add button after the last strip.
// ---------------------------------------------------------------------------------------------------------------------------------------

namespace {

void settleLeftRight(RealRack& r) {
	r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
		r.card->setBounds(0.0f, 0.0f, r.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
	}
}

} // namespace

TEST_CASE("rack layout: the first card is one card gap after the grid or the tab bar, in both flows, with nothing reserved between") {
	for (const int view : { 1, 2 }) {
		for (const bool horizontal : { false, true }) {
			CAPTURE(view);
			CAPTURE(horizontal);
			RealRack r;
			r.chains(2, 2, true);
			r.view_(view);
			if (horizontal) {
				r.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
				settleLeftRight(r);
			}
			else {
				for (int i = 0; i < 3; ++i) {
					r.refresh();
				}
			}
			constexpr float kGap = ui::RackFrontColumn::kGap;
			static_assert(ui::ParallelRackCard::kRowGap == ui::RackFrontColumn::kGap, "the card's row gap is the lane's card gap");
			const visage::Bounds front = r.card->frontForTest().bounds();
			if (view == 1) {
				// TABS: THE ARROW STANDS BETWEEN THE BAR AND THE FIRST CARD (the chain holds two devices), one gap either side.
				const visage::Bounds bar = r.card->tabBarForTest().bounds();
				const visage::Bounds arrow = r.card->chainArrowForTest().bounds();
				REQUIRE(r.card->chainArrowForTest().isVisible());
				if (horizontal) {
					CHECK(arrow.x() - bar.right() == doctest::Approx(kGap));
					CHECK(front.x() - arrow.right() == doctest::Approx(kGap));
				}
				else {
					CHECK(arrow.y() - bar.bottom() == doctest::Approx(kGap));
					CHECK(front.y() - arrow.bottom() == doctest::Approx(kGap));
				}
			}
			else if (horizontal) {
				const visage::Bounds pads = r.card->padsForTest().bounds();
				CHECK(front.x() - pads.right() == doctest::Approx(kGap));
			}
			else {
				// TOP-DOWN PADS: the pair stands between the grid and the cards, one gap either side.
				const visage::Bounds pads = r.card->padsForTest().bounds();
				const visage::Bounds step = r.card->padsStepForTest().bounds();
				CHECK(step.y() - pads.bottom() == doctest::Approx(kGap));
				CHECK(front.y() - step.bottom() == doctest::Approx(kGap));
			}
		}
	}
}

TEST_CASE("rack layout: in a left-right lane the tabs are a thin full-height column, subdivided evenly, with the first card one gap right of it, and a nested rack's tab column stands right of its parent's") {
	RealRack r;
	r.chains(3, 2, true);
	r.view_(1);
	r.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	settleLeftRight(r);
	ui::RackTabBar& bar = r.card->tabBarForTest();
	// SMALL AND FIXED WIDTH; its height is the cards' cross extent (the content height), level with them.
	CHECK(bar.width() == doctest::Approx(ui::RackTabBar::kThickness));
	CHECK(bar.height() == doctest::Approx(r.card->frontForTest().bounds().height()));
	CHECK(bar.bounds().y() == doctest::Approx(r.card->frontForTest().bounds().y()));
	CHECK(bar.width() < static_cast<float>(r.card->width()) * 0.25f);
	// EVENLY: three tabs along the height differ by at most a native pixel and sum to it exactly.
	const std::vector<size_t> tabs = bar.tabsForTest();
	REQUIRE(tabs.size() == 3);
	int sum = 0;
	int lo = 1 << 20;
	int hi = 0;
	for (size_t k = 0; k < tabs.size(); ++k) {
		const auto [a, b] = bar.tabNativeExtent(k);
		sum += b - a;
		lo = std::min(lo, b - a);
		hi = std::max(hi, b - a);
	}
	CHECK(sum == bar.nativeHeight());
	CHECK(hi - lo <= 1);
	// A TAB'S NAME IS ELIDED RATHER THAN CLIPPED.
	CHECK_FALSE(bar.labelForTest(0).empty());
	// NESTED RACKS: a Tabs rack as the first device stacks its tab column to the RIGHT of the parent's.
	RealRack n;
	n.chains(1, 0, true);
	const feathers::DeviceInstanceId inner = nestedRack(*n.rack().chainAt(0)->bridge(), 2, true);
	(void)inner;
	n.view_(1);
	n.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
	settleLeftRight(n);
	std::vector<ui::FxCard*> level1 = shownCards(*n.card);
	REQUIRE(level1.size() == 1);
	auto* child = dynamic_cast<ui::ParallelRackCard*>(level1[0]);
	REQUIRE(child != nullptr);
	CHECK(child->tabBarForTest().isVisible());
	CHECK(child->tabBarForTest().positionInWindow().x > n.card->tabBarForTest().positionInWindow().x + n.card->tabBarForTest().width());
	CHECK(child->tabBarForTest().width() == doctest::Approx(ui::RackTabBar::kThickness));
}

TEST_CASE("rack layout: the Strips window's add-chain button stands after the last chain's strip and moves as chains are added") {
	RealRack r;
	r.chains(2, 0);
	r.view_(0);
	const visage::Bounds before = r.panel->addBoundsForTest();
	REQUIRE(before.width() > 0.0f);
	// AFTER THE LAST STRIP, not in the left controls column (which is kPad + kColumnWidth wide).
	const visage::Bounds lastRail = r.panel->railForTest(r.panel->railCountForTest() - 1).bounds();
	CHECK(before.x() >= lastRail.right());
	CHECK(before.x() > ui::ParallelRackExpanded::kPad + ui::ParallelRackExpanded::kColumnWidth);
	r.panel->addChainForTest();
	r.refresh();
	r.panel->setBounds(0.0f, 0.0f, std::max(static_cast<float>(r.panel->width()), r.panel->minimumSize().x), static_cast<float>(r.panel->height()));
	r.refresh();
	const visage::Bounds after = r.panel->addBoundsForTest();
	CHECK(after.x() > before.x());
	CHECK(after.width() == doctest::Approx(before.width()));
	const visage::Bounds newLast = r.panel->railForTest(r.panel->railCountForTest() - 1).bounds();
	CHECK(after.x() >= newLast.right());
	// THE WINDOW IS WIDE ENOUGH FOR IT.
	CHECK(r.panel->minimumSize().x >= after.right());
	// A hit on the button adds a chain (the press the hand makes), as it did where it stood before.
	const size_t chains = r.rack().chainCount();
	visage::MouseEvent press {};
	press.position = { after.x() + 1.0f, after.y() + 1.0f };
	r.panel->onMouseDown().callback(press);
	CHECK(r.rack().chainCount() == chains + 1);
	// TABS: A "+" AT THE END OF THE TAB COLUMN, under the last tab (one tab's width).
	r.view_(1);
	const visage::Bounds tabsAdd = r.panel->addBoundsForTest();
	const visage::Bounds column = r.panel->tabBarForTest().bounds();
	CHECK(tabsAdd.x() == doctest::Approx(column.x()));
	CHECK(tabsAdd.y() >= column.bottom());
	CHECK(tabsAdd.width() == doctest::Approx(ui::RackTabBar::kThickness));
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// CHAIN NAMES: one label per chain, read the same on every surface; "Rename…" writes it, one undo step.
// ---------------------------------------------------------------------------------------------------------------------------------------

namespace {

/// EVERY STRING THAT NAMES CHAIN `slot`, from every surface that shows one, tagged by where it came from.
std::vector<std::pair<std::string, std::string>> labelsOf(RealRack& r, size_t slot) {
	std::vector<std::pair<std::string, std::string>> out;
	out.emplace_back("rack", r.rack().chainLabel(slot));
	out.emplace_back("adapter chain()", r.panel->viewModelForTest().chain(slot).name);
	out.emplace_back("adapter label", r.panel->viewModelForTest().chainLabel(slot));
	out.emplace_back("card adapter", r.card->viewModelForTest().chain(slot).name);
	// THE TABS, on the window's bar and the card's.
	for (const auto& [where, bar] : { std::pair<const char*, ui::RackTabBar*> { "window tab", &r.panel->tabBarForTest() },
	                                   std::pair<const char*, ui::RackTabBar*> { "card tab", &r.card->tabBarForTest() } }) {
		const std::vector<size_t> tabs = bar->tabsForTest();
		for (size_t k = 0; k < tabs.size(); ++k) {
			if (tabs[k] == slot) {
				out.emplace_back(where, bar->labelForTest(k));
			}
		}
	}
	// THE PAD CELL, on both grids (the chain's key's cell).
	const int key = r.rack().chainKey(slot);
	if (key != pr::kAnyKey) {
		for (const auto& [where, grid] : { std::pair<const char*, ui::RackPadGrid*> { "window pad", &r.panel->padsForTest().gridForTest() },
		                                    std::pair<const char*, ui::RackPadGrid*> { "card pad", &r.card->padsForTest().gridForTest() } }) {
			const int page = key / ui::kPadsPerPage;
			r.set(pr::Param::PadsPage, static_cast<float>(page));
			r.refresh();
			out.emplace_back(where, grid->labelForTest(key % ui::kPadsPerPage));
		}
	}
	// THE WINDOW'S RAIL AND STRIP CAPTIONS.
	out.emplace_back("window rail", r.panel->railForTest(slot).labelForTest());
	return out;
}

} // namespace

TEST_CASE("chain names: every surface shows one and the same label for a chain — renamed, unnamed with a device, unnamed and empty") {
	RealRack r;
	r.chains(3, 0, true); // keys 0, 1, 2; every chain empty
	(void)r.rack().chainAt(1)->bridge()->addDevice("DRUM");
	r.set(pr::Param::View, 1.0f);
	r.refresh();
	// THE THREE KINDS: chain 0 unnamed and empty, chain 1 unnamed with a Drum, chain 2 renamed.
	REQUIRE(r.rack().setChainName(2, "Hats"));
	r.refresh();
	const std::vector<std::string> wanted = { "Chain 1", "Drum", "Hats" };
	for (size_t slot = 0; slot < 3; ++slot) {
		CAPTURE(slot);
		const auto labels = labelsOf(r, slot);
		REQUIRE(labels.size() >= 6);
		for (const auto& [where, text] : labels) {
			CAPTURE(where);
			CHECK(text == wanted[slot]);
		}
	}
	// THE READOUTS: the Viewing and Playing values print the label too (never a number).
	r.set(pr::Param::ViewedChain, 2.0f);
	r.set(pr::Param::Playing, 3.0f);
	r.refresh();
	CHECK(r.panel->viewedBoxForTest().control().text() == "Hats");
	CHECK(r.panel->playingBoxForTest().control().text() == "Hats");
	// AND THE TEXT THE BOXES DRAW (not only the control's): the label, never the unit's bare number.
	CHECK(r.panel->viewedBoxForTest().drawnReadoutForTest() == "Hats");
	CHECK(r.panel->playingBoxForTest().drawnReadoutForTest() == "Hats");
	CHECK(r.panel->viewedBoxForTest().readout() == "Hats");
	r.set(pr::Param::Playing, 0.0f);
	CHECK(r.panel->playingBoxForTest().control().text() == "Position");
	// AND THEIR MENUS NAME THE CHAINS, in chain order (Playing has Position first).
	visage::PopupMenu viewing = r.panel->chainPickerMenuForTest(false);
	REQUIRE(viewing.options().size() == 3);
	CHECK(viewing.options()[0].name().toUtf8().find("Chain 1") != std::string::npos);
	CHECK(viewing.options()[1].name().toUtf8().find("Drum") != std::string::npos);
	CHECK(viewing.options()[2].name().toUtf8().find("Hats") != std::string::npos);
	visage::PopupMenu playing = r.panel->chainPickerMenuForTest(true);
	REQUIRE(playing.options().size() == 4);
	CHECK(playing.options()[0].name().toUtf8().find("Position") != std::string::npos);
	viewing.onSelection().callback(1);
	CHECK(r.field(pr::Param::ViewedChain) == 1.0f);
	// TYPING A NAME INTO THE BOX FINDS THE CHAIN.
	const std::optional<double> typed = r.panel->viewedBoxForTest().control().parseText("hats", 0.0);
	REQUIRE(typed.has_value());
	CHECK(*typed == 2.0);
	// NOT BLIND: a reader still printing the number would differ — the sweep's own comparison catches one (chain 1's "Chain 2" is not "Drum").
	CHECK(std::string("Chain 2") != labelsOf(r, 1).front().second);
}

TEST_CASE("chain names: Rename... from a tab, a pad and the menu renames one chain by id, as one undo step, and clearing reverts to the derived name") {
	RealRack r;
	r.chains(3, 2, true);
	r.view_(1);
	ui::EditHistory history;
	r.panel->modulationBound(nullptr, &history);
	r.card->setModulation(nullptr, &history);
	ui::RackChainRename& rename = r.panel->chainRenameForTest();
	// THE MENU: "Rename..." always, "Clear name" only while the chain has a name.
	{
		visage::PopupMenu menu = rename.menuForTest(1);
		REQUIRE(menu.options().size() == 2);
		CHECK(menu.options()[0].name().toUtf8() == "Rename...");
	}
	// A RIGHT PRESS ON A TAB asks for the menu (the handler is called with that tab's chain).
	size_t asked = 99;
	r.panel->tabBarForTest().setContextHandler([&](size_t slot, const visage::Bounds&) { asked = slot; });
	r.panel->tabBarForTest().pressContextForTest(r.panel->tabBarForTest().tabCentre(2));
	CHECK(asked == 2);
	r.panel->tabBarForTest().setContextHandler([&](size_t slot, const visage::Bounds& b) {
		rename.showMenu(slot, r.panel->tabBarForTest(), b);
	});
	// THE FIELD: opened on chain 1, committed as the field's Enter commits it.
	rename.begin(1, r.panel->tabBarForTest(), visage::Bounds { 0.0f, 0.0f, 80.0f, 20.0f });
	REQUIRE(rename.renaming());
	CHECK(rename.field().text() == "Drum 2"); // opens on the label it shows (the second chain led by a Drum)
	const size_t undoable = history.length();
	// A REORDER UNDER THE OPEN FIELD renames the SAME chain: the id is kept, not the place.
	const uint16_t id = r.rack().chainIdAt(1);
	REQUIRE(rename.commitForTest("  Kick  "));
	CHECK(r.rack().chainName(r.rack().indexOfChain(id)) == "Kick");
	CHECK(history.length() == undoable + 1);
	for (size_t slot = 0; slot < 3; ++slot) {
		if (r.rack().chainIdAt(slot) == id) {
			for (const auto& [where, text] : labelsOf(r, slot)) {
				CAPTURE(where);
				CHECK(text == "Kick");
			}
		}
	}
	// UNDO PUTS THE DERIVED NAME BACK, REDO THE NAME.
	history.undo();
	r.refresh();
	CHECK(r.rack().chainName(1).empty());
	CHECK(r.panel->viewModelForTest().chain(1).name == "Drum 2");
	history.redo();
	r.refresh();
	CHECK(r.rack().chainName(1) == "Kick");
	// CAPPED like a macro name (24 bytes), and a name past it is cut, never refused.
	rename.begin(1, r.panel->tabBarForTest(), visage::Bounds { 0.0f, 0.0f, 80.0f, 20.0f });
	REQUIRE(rename.commitForTest(std::string(60, 'x')));
	CHECK(r.rack().chainName(1).size() <= 24);
	// CLEAR NAME goes through the menu's own row and is an undo step too.
	{
		visage::PopupMenu menu = rename.menuForTest(1);
		const size_t before = history.length();
		menu.onSelection().callback(2);
		CHECK(r.rack().chainName(1).empty());
		CHECK(history.length() == before + 1);
	}
	// A PAD'S RIGHT PRESS asks the same, for a used pad only.
	r.view_(2);
	size_t padAsked = 99;
	auto& grid = r.panel->padsForTest().gridForTest();
	grid.setContextHandler([&](size_t slot, const visage::Bounds&) { padAsked = slot; });
	grid.pressContextForTest(grid.padCentre(1));
	CHECK(padAsked == 1);
	padAsked = 99;
	grid.pressContextForTest(grid.padCentre(9)); // an empty pad
	CHECK(padAsked == 99);
}

TEST_CASE("rack layout: in the Rack window's Tabs view the tabs are a thin column down the left, each at most a cap long, a + at its end, and the window is no wider for eight tabs than for two") {
	const auto measure = [](size_t chains, bool pane) {
		RealRack r;
		r.chains(chains, 0, true);
		for (size_t i = 0; i < chains; ++i) {
			(void)r.rack().chainAt(i)->bridge()->addDevice("DRUM"); // the same front content in every tab
		}
		r.set(pr::Param::Front, pane ? 1.0f : 0.0f);
		r.view_(1);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		r.panel->setBounds(0.0f, 0.0f, std::max(1400.0f, r.panel->minimumSize().x), std::max(900.0f, r.panel->minimumSize().y));
		r.refresh();
		struct Out {
			float minWidth;
			visage::Bounds column;
			visage::Bounds add;
			size_t tabs;
			float railX;
			float firstTab;
			float tabLength;
		} out {};
		out.minWidth = r.panel->minimumSize().x;
		out.column = r.panel->tabBarForTest().bounds();
		out.add = r.panel->addBoundsForTest();
		out.tabs = r.panel->tabBarForTest().tabsForTest().size();
		out.railX = r.panel->railForTest(r.panel->viewModelForTest().viewedChain()).bounds().x();
		out.firstTab = r.panel->tabBarForTest().x();
		out.tabLength = out.tabs > 0 ? static_cast<float>(r.panel->tabBarForTest().height()) / static_cast<float>(out.tabs) : 0.0f;
		return out;
	};
	for (const bool pane : { false, true }) {
		CAPTURE(pane);
		const auto two = measure(2, pane);
		const auto eight = measure(8, pane);
		REQUIRE(two.tabs == 2);
		REQUIRE(eight.tabs == 8);
		// A THIN COLUMN down the left, after the controls column, before the content.
		CHECK(two.column.width() == doctest::Approx(ui::RackTabBar::kThickness));
		CHECK(eight.column.width() == doctest::Approx(ui::RackTabBar::kThickness));
		CHECK(two.column.height() > two.column.width());
		CHECK(two.column.x() == doctest::Approx(ui::ParallelRackExpanded::kColumnWidth + 2.0f * ui::ParallelRackExpanded::kPad));
		CHECK(two.railX >= two.column.right());
		// EACH TAB AT MOST THE CAP LONG: two tabs are a short column, not the window's whole height.
		CHECK(two.tabLength <= ui::RackTabBar::kMaxLength + 0.5f);
		CHECK(eight.tabLength <= ui::RackTabBar::kMaxLength + 0.5f);
		// THE WINDOW IS DEFINED BY THE CONTENT: eight tabs, the same width as two.
		CHECK(eight.minWidth == doctest::Approx(two.minWidth));
		// A "+" AT THE END, level with the column and one tab's width — except at eight chains, where the add is gone (hidden, not greyed).
		CHECK(two.add.x() == doctest::Approx(two.column.x()));
		CHECK(two.add.y() >= two.column.bottom());
		CHECK(eight.add.width() == 0.0f);
	}
}

TEST_CASE("rack layout: the Tabs arrow stands at a fixed place between the bar and the first card — the same rect folded and open, in both flows — and the Pads pair does too") {
	for (const bool horizontal : { false, true }) {
		CAPTURE(horizontal);
		RealRack r;
		r.chains(2, 2, true);
		r.view_(1);
		if (horizontal) {
			r.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
			settleLeftRight(r);
		}
		else {
			for (int i = 0; i < 3; ++i) {
				r.refresh();
			}
		}
		ui::RackDrawerArrow& arrow = r.card->chainArrowForTest();
		REQUIRE(arrow.isVisible());
		const visage::Bounds closed = arrow.bounds();
		CHECK(closed.width() == doctest::Approx(horizontal ? ui::RackDrawerArrow::kSize : closed.width()));
		CHECK(closed.height() == doctest::Approx(ui::RackDrawerArrow::kSize));
		const size_t shownClosed = shownCards(*r.card).size();
		arrow.toggleForTest(); // OPEN the chain: more cards, a bigger card
		if (horizontal) {
			settleLeftRight(r);
		}
		else {
			for (int i = 0; i < 3; ++i) {
				r.refresh();
			}
		}
		REQUIRE(shownCards(*r.card).size() > shownClosed);
		CHECK(arrow.bounds() == closed); // did not move as the cards expanded
		arrow.toggleForTest(); // and shut again
		if (horizontal) {
			settleLeftRight(r);
		}
		else {
			for (int i = 0; i < 3; ++i) {
				r.refresh();
			}
		}
		CHECK(shownCards(*r.card).size() == shownClosed);
		CHECK(arrow.bounds() == closed);
		// THE ARROW IS BEFORE THE CARDS (between the bar and the first card), not after them.
		const visage::Bounds front = r.card->frontForTest().bounds();
		if (horizontal) {
			CHECK(arrow.bounds().right() <= front.x());
		}
		else {
			CHECK(arrow.bounds().bottom() <= front.y());
		}
	}
}

TEST_CASE("rack layout: the Pads grid, page strip and kit level take the card's content width in a top-down lane (up to a cap, centred past it), cells staying square") {
	RealRack r;
	r.chains(2, 2, true);
	r.view_(2);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	ui::RackPadsPanel& pads = r.card->padsForTest();
	// WHERE THE CARDS BELOW IT ARE AS WIDE: the grid is exactly the card's content width.
	const float content = static_cast<float>(r.card->frontForTest().width());
	CHECK(pads.gridForTest().width() == doctest::Approx(content));
	CHECK(pads.pagesForTest().width() == doctest::Approx(pads.gridForTest().width()));
	REQUIRE(pads.levelForTest() != nullptr);
	CHECK(pads.levelForTest()->width() == doctest::Approx(pads.gridForTest().width()));
	CHECK(pads.gridForTest().width() == doctest::Approx(pads.gridForTest().height())); // square cells: a square grid
	CHECK(pads.gridForTest().width() >= ui::RackPadsPanel::gridSide());
	// THE CAP: a panel far wider than a sensible cell allows keeps the cap and centres the grid.
	struct Fake final : ui::RackViewModel {
		ui::RackViewKind view() const override { return ui::RackViewKind::Pads; }
		ui::RackRoutingKind routing() const override { return ui::RackRoutingKind::Parallel; }
		size_t chainSlots() const override { return 0; }
		ui::RackChainView chain(size_t) const override { return {}; }
		size_t viewedChain() const override { return 0; }
		void viewChain(size_t) override {}
		size_t playingChain() const override { return 0; }
		void setPlayingChain(size_t) override {}
		device::Modulatable* playingControl() override { return nullptr; }
		int page() const override { return 0; }
		void setPage(int) override {}
		bool expanded(size_t) const override { return false; }
		void setExpanded(size_t, bool) override {}
		bool macrosOpen() const override { return false; }
		void setMacrosOpen(bool) override {}
		ui::RackKeyMask takeStruck() override { return {}; }
		device::Modulatable* kitLevel() override { return nullptr; }
	} fake;
	ui::RackStrikePulse pulse;
	ui::RackPadsPanel panel(fake, pulse, ui::RackFlow::TopDown);
	const float wide = 1000.0f;
	panel.setBounds(0.0f, 0.0f, wide, panel.naturalSizeFor(wide).y);
	CHECK(panel.gridForTest().width() == doctest::Approx(ui::RackPadsPanel::kMaxSide));
	CHECK(panel.gridForTest().x() == doctest::Approx((wide - ui::RackPadsPanel::kMaxSide) * 0.5f));
	CHECK(panel.pagesForTest().width() == doctest::Approx(ui::RackPadsPanel::kMaxSide));
	// AND NEVER BELOW THE NATURAL SIDE.
	panel.setBounds(0.0f, 0.0f, 120.0f, panel.naturalSizeFor(120.0f).y);
	CHECK(panel.gridForTest().width() == doctest::Approx(ui::RackPadsPanel::gridSide()));
}

TEST_CASE("chain names: the Position readout, route and destination names, the host's slot captions, and a double-click rename all read the same label") {
	RealRack r;
	r.chains(3, 0, true);
	(void)r.rack().chainAt(1)->bridge()->addDevice("DRUM");
	(void)r.rack().chainAt(0)->bridge()->addDevice("DELAY");
	REQUIRE(r.rack().setChainName(2, "Hats"));
	r.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	r.refresh();
	const std::vector<std::string> wanted = { "Delay", "Drum", "Hats" };
	// (1) THE POSITION KNOB'S READOUT NAMES THE CHAIN IT SELECTS (and the blend between two by their names), never "Chain N".
	device::Modulatable& position = *r.panel->positionKnobForTest().control();
	for (size_t i = 0; i < 3; ++i) {
		position.setValue(static_cast<double>(i) / 2.0);
		CHECK(position.text() == wanted[i]);
	}
	r.set(pr::Param::Mode, static_cast<float>(pr::Mode::Blend));
	position.setValue(0.25);
	CHECK(position.text().find("Delay") != std::string::npos);
	CHECK(position.text().find("Drum") != std::string::npos);
	CHECK(position.text().find("Chain") == std::string::npos);
	// AND A NAME TYPED INTO IT FINDS THE CHAIN (the readout reads back).
	const std::optional<double> typed = position.parseText("hats", 0.0);
	REQUIRE(typed.has_value());
	CHECK(*typed == doctest::Approx(1.0));
	const std::optional<double> blend = position.parseText(position.text(), 0.0);
	REQUIRE(blend.has_value());
	CHECK(*blend == doctest::Approx(0.25).epsilon(0.01));
	// (2) A ROUTE INTO A DEVICE IN A CHAIN NAMES THE CHAIN FIRST: "Hats · Delay · ...".
	feathers::rack::RackBridge& hats = *r.rack().chainAt(2)->bridge();
	const feathers::DeviceInstanceId inHats = hats.addDevice("DELAY");
	REQUIRE(inHats != feathers::kNoInstance);
	r.refresh();
	char name[96] = {};
	const size_t written = pr::kContainerOps.nameDestination(r.body, feathers::backplane::Destination::control(inHats, 0), name, sizeof name);
	REQUIRE(written > 0);
	CHECK(std::string(name).rfind("Hats \xC2\xB7 ", 0) == 0);
	// (3) THE HOST'S NAMES FOR THE CHAIN SLOTS' CONTROLS use the label (the macro-rename road: captions sent with the macros').
	r.rack().refreshSlotCaptions();
	const auto& captions = r.rack().slotCaptionsForTest();
	const auto captionFor = [&](size_t chain, pr::SlotControl c) {
		const uint8_t slot = r.rack().slotOf(chain);
		for (const auto& cap : captions) {
			if (cap.control == pr::slotParam(slot, c)) {
				return cap.name;
			}
		}
		return std::string("<none>");
	};
	CHECK(captionFor(0, pr::SlotControl::Level) == "Delay Level");
	CHECK(captionFor(1, pr::SlotControl::Mute) == "Drum Mute");
	CHECK(captionFor(2, pr::SlotControl::Solo) == "Hats Solo");
	r.rack().setChainName(0, "Open");
	const auto& after = r.rack().slotCaptionsForTest();
	bool renamed = false;
	for (const auto& cap : after) {
		renamed = renamed || cap.name == "Open Level";
	}
	CHECK(renamed);
	// A REMOVED CHAIN HAS NO CAPTION (the declared static name stands for a slot nothing holds).
	const uint8_t gone = r.rack().slotOf(1);
	REQUIRE(r.rack().removeChain(1));
	for (const auto& cap : r.rack().slotCaptionsForTest()) {
		CHECK(cap.control != pr::slotParam(gone, pr::SlotControl::Level));
	}
	// (4) A DOUBLE-CLICK ON A TAB, AND ON A STRIP'S CAPTION, OPENS THE SAME FIELD over that chain.
	r.set(pr::Param::Mode, static_cast<float>(pr::Mode::Parallel));
	r.view_(1);
	ui::RackChainRename& rename = r.panel->chainRenameForTest();
	r.panel->tabBarForTest().doubleClickForTest(r.panel->tabBarForTest().tabCentre(1));
	REQUIRE(rename.renaming());
	CHECK(rename.chainForTest() == r.rack().chainIdAt(1));
	CHECK(rename.commitForTest("Second"));
	CHECK(r.rack().chainName(1) == "Second");
	r.view_(0);
	r.refresh();
	ui::ChainStrip& strip = r.panel->stripForTest(0);
	strip.doubleClickForTest(visage::Point { 2.0f, 2.0f });
	REQUIRE(rename.renaming());
	CHECK(rename.chainForTest() == r.rack().chainIdAt(0));
}

TEST_CASE("chain names: the Viewing and Playing boxes DRAW the label, and a label too long for them is cut with an ellipsis from the end, never to its tail or a number") {
	RealRack r;
	r.chains(3, 0, true);
	(void)r.rack().chainAt(2)->bridge()->addDevice("DRUM");
	(void)r.rack().chainAt(1)->bridge()->addDevice("DRUM");
	(void)r.rack().chainAt(0)->bridge()->addDevice("DRUM"); // "Drum", "Drum 2", "Drum 3"
	r.set(pr::Param::ViewedChain, 2.0f);
	r.set(pr::Param::Playing, 3.0f);
	r.refresh();
	// THE BOXES ARE NARROW (a third of the controls column): "Drum 3" is drawn whole or shrunk to fit, never reduced to its last character.
	const std::string drawn = r.panel->viewedBoxForTest().drawnReadoutForTest();
	CHECK(drawn.find("Drum") != std::string::npos);
	CHECK(drawn != "3");
	CHECK(r.panel->playingBoxForTest().drawnReadoutForTest().find("Drum") != std::string::npos);
	// A NAME LONGER THAN THE ROOM: cut from the END with "...", still starting with its own first letters.
	const std::string longName = "Very long chain name indeed";
	REQUIRE(r.rack().setChainName(2, longName));
	r.refresh();
	for (ui::ValueBox* box : { &r.panel->viewedBoxForTest(), &r.panel->playingBoxForTest() }) {
		const std::string shown = box->drawnReadoutForTest();
		CAPTURE(shown);
		REQUIRE(shown.size() >= 4);
		CHECK(shown.substr(shown.size() - 3) == "...");
		CHECK(longName.rfind(shown.substr(0, 3), 0) == 0); // its own beginning, not its tail
	}
}

TEST_CASE("rack keys: Ctrl+V pastes into an EMPTY chain from the pad or tab that selected it, on the card and in the window, and nothing happens with nothing focused") {
	for (const int view : { 2, 1 }) { // Pads, Tabs
		CAPTURE(view);
		RealRack r;
		r.chains(2, 0, true); // two EMPTY chains
		// THE CLIPBOARD HOLDS ONE DEVICE, lifted from a donor chain.
		feathers::rack::RackEngine donorEngine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
		donorEngine.prepare(48000.0, 4096);
		feathers::rack::RackBridge donor(donorEngine);
		donor.prepare(48000.0, 4096);
		const feathers::DeviceInstanceId made = donor.addDevice(feathers::fx::drumdevice::kTraits.tag);
		REQUIRE(made != feathers::kNoInstance);
		const feathers::DeviceInstanceId ids[] = { made };
		feathers::rack::setPasteboard(feathers::rack::liftFragment(donor, ids));
		r.view_(view);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		const visage::KeyEvent paste { visage::KeyCode::V, visage::kModifierRegCtrl, true };
		const auto lengthOf = [&](size_t chain) { return r.rack().chainAt(chain)->bridge()->chainLength(0); };
		// NOTHING FOCUSED: the key goes nowhere and nothing is pasted.
		CHECK_FALSE(keyFromFocus(nullptr, paste));
		CHECK(lengthOf(1) == 0);
		// THE CARD: select chain 2 (a pad, or a tab), take the focus the framework gives that press, send Ctrl+V.
		visage::Point where {};
		if (view == 2) {
			ui::RackPadGrid& grid = r.card->padsForTest().gridForTest();
			grid.pressForTest(grid.padCentre(1));
			grid.releaseForTest();
			where = grid.positionInWindow();
			where.x += static_cast<float>(grid.width()) * 0.5f; // the press focuses the nearest key-accepting frame under the pointer
			where.y += static_cast<float>(grid.height()) * 0.5f;
		}
		else {
			ui::RackTabBar& bar = r.card->tabBarForTest();
			bar.pressForTest(bar.tabCentre(1));
			where = bar.positionInWindow();
			where.x += bar.tabCentre(1).x;
			where.y += bar.tabCentre(1).y;
		}
		r.refresh();
		CHECK(r.field(pr::Param::ViewedChain) == 1.0f);
		const visage::Point cardOrigin = r.card->positionInWindow(); // the card alone: the window panel is a sibling over the same room
		visage::Frame* focus = keyboardFocusAfterPress(r.card, visage::Point { where.x - cardOrigin.x, where.y - cardOrigin.y });
		REQUIRE(focus != nullptr);
		CHECK(keyFromFocus(focus, paste));
		CHECK(lengthOf(1) == 1);
		CHECK(lengthOf(0) == 0);
		// THE WINDOW: the same from its pad or tab, into the OTHER chain.
		if (view == 2) {
			ui::RackPadGrid& grid = r.panel->padsForTest().gridForTest();
			grid.pressForTest(grid.padCentre(0));
			grid.releaseForTest();
			where = grid.positionInWindow();
			where.x += static_cast<float>(grid.width()) * 0.5f;
			where.y += static_cast<float>(grid.height()) * 0.5f;
		}
		else {
			ui::RackTabBar& bar = r.panel->tabBarForTest();
			bar.pressForTest(bar.tabCentre(0));
			where = bar.positionInWindow();
			where.x += bar.tabCentre(0).x;
			where.y += bar.tabCentre(0).y;
		}
		r.refresh();
		CHECK(r.field(pr::Param::ViewedChain) == 0.0f);
		visage::Frame* windowFocus = keyboardFocusAfterPress(r.panel, where);
		REQUIRE(windowFocus != nullptr);
		CHECK(keyFromFocus(windowFocus, paste));
		CHECK(lengthOf(0) == 1);
	}
}

TEST_CASE("rack keys: while a chain's name is being typed the keys are the FIELD's (nothing is pasted, cut, copied or deleted on the chain), and with the field closed Ctrl+V pastes again") {
	for (const int view : { 1, 2 }) { // Tabs, Pads
		CAPTURE(view);
		RealRack r;
		r.chains(2, 0, true);
		feathers::rack::RackEngine donorEngine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
		donorEngine.prepare(48000.0, 4096);
		feathers::rack::RackBridge donor(donorEngine);
		donor.prepare(48000.0, 4096);
		const feathers::DeviceInstanceId made = donor.addDevice(feathers::fx::drumdevice::kTraits.tag);
		REQUIRE(made != feathers::kNoInstance);
		const feathers::DeviceInstanceId ids[] = { made };
		feathers::rack::setPasteboard(feathers::rack::liftFragment(donor, ids));
		r.view_(view);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		const auto lengthOf = [&](size_t chain) { return r.rack().chainAt(chain)->bridge()->chainLength(0); };
		const auto key = [](visage::KeyCode c, int mods = 0) { return visage::KeyEvent { c, mods, true }; };
		struct Venue {
			visage::Frame* root;
			ui::RackChainRename* rename;
			ui::RackTabBar* bar;
			ui::RackPadGrid* grid;
		};
		const Venue venues[] = {
			{ r.f.root, &r.card->chainRenameForTest(), &r.card->tabBarForTest(), &r.card->padsForTest().gridForTest() },
			{ r.panel, &r.panel->chainRenameForTest(), &r.panel->tabBarForTest(), &r.panel->padsForTest().gridForTest() },
		};
		for (const Venue& v : venues) {
			const size_t chain = &v == &venues[0] ? 1 : 0; // a different chain in each venue
			INFO("venue chain " << chain);
			visage::Point where {};
			if (view == 1) {
				v.bar->pressForTest(v.bar->tabCentre(chain));
				where = v.bar->positionInWindow();
				where.x += v.bar->tabCentre(chain).x;
				where.y += v.bar->tabCentre(chain).y;
			}
			else {
				v.grid->pressForTest(v.grid->padCentre(chain));
				v.grid->releaseForTest();
				where = v.grid->positionInWindow();
				where.x += static_cast<float>(v.grid->width()) * 0.5f;
				where.y += static_cast<float>(v.grid->height()) * 0.5f;
			}
			r.refresh();
			const size_t before = lengthOf(chain);
			v.rename->begin(chain, *v.bar, visage::Bounds { 0.0f, 0.0f, 80.0f, 20.0f });
			REQUIRE(v.rename->renaming());
			ui::TextField& field = v.rename->field();
			field.textInput("v");
			CHECK(field.text() == "v"); // a plain "v" types a v
			for (const visage::KeyCode c : { visage::KeyCode::Backspace, visage::KeyCode::Delete }) {
				CHECK(keyFromFocus(&field, key(c))); // the field's, and nothing travels on
				CHECK(lengthOf(chain) == before);
			}
			// A CHORD is the host's, so the field leaves it: it must STILL not reach the chain.
			for (const visage::KeyCode c : { visage::KeyCode::V, visage::KeyCode::C, visage::KeyCode::X }) {
				CHECK_FALSE(keyFromFocus(&field, key(c, visage::kModifierRegCtrl)));
				CHECK(lengthOf(chain) == before);
				CHECK(lengthOf(1 - chain) == 0);
				CHECK(v.rename->renaming());
			}
			CHECK(keyFromFocus(&field, key(visage::KeyCode::Escape))); // Esc closes only the field
			CHECK_FALSE(v.rename->renaming());
			CHECK(lengthOf(chain) == before);
			// THE FIELD CLOSED: Ctrl+V pastes from the focus a press gives.
			visage::Frame* focus = &v == &venues[0] ? static_cast<visage::Frame*>(r.card) : static_cast<visage::Frame*>(r.panel); // the frame a press on its pad or tab focuses
			(void)where;
			CHECK(keyFromFocus(focus, key(visage::KeyCode::V, visage::kModifierRegCtrl)));
			CHECK(lengthOf(chain) == before + 1);
			if (view == 1) {
				size_t asked = 99;
				v.bar->setContextHandler([&](size_t slot, const visage::Bounds&) { asked = slot; });
				v.bar->pressContextForTest(v.bar->tabCentre(chain));
				CHECK(asked == chain); // a right press asks for the rename menu
				v.bar->setContextHandler([&](size_t slot, const visage::Bounds& b) { v.rename->showMenu(slot, *v.bar, b); });
				v.bar->doubleClickForTest(v.bar->tabCentre(chain));
				CHECK(v.rename->renaming()); // a double click renames
				CHECK(keyFromFocus(&v.rename->field(), key(visage::KeyCode::Escape)));
			}
			r.rack().chainAt(chain)->bridge()->removeDevice(r.rack().chainAt(chain)->bridge()->deviceAtPosition(before, 0));
		}
	}
}

//--------------------------------------------------------------------------------------------------//
// RENAME FIELD: WHAT THE USER SEES — visible, whole, topmost, focused, reached by both roads         //
//--------------------------------------------------------------------------------------------------//

namespace {

enum class RenameRoad { Menu, DoubleClick };

struct RenameSeen {
	bool opened = false;
	bool visible = false;
	bool inside = false;
	bool topmost = false;
	bool focused = false;
	bool typed = false;
	visage::Bounds rect {};
	std::string hit;
};

/// Opens the rename field on chain `chain` by `road` on the surface `s`, then reports what a user would see and does. `rename` is the
/// surface's own `RackChainRename`, `root` the top of the tree it sits in, `bar`/`grid` the view's controls.
RenameSeen renameSeen(RenameRoad road, visage::Frame* root, ui::RackChainRename& rename, ui::RackTabBar* bar, ui::RackPadGrid* grid,
                      size_t chain, const std::function<void()>& settle) {
	RenameSeen out;
	visage::Frame* source = bar != nullptr ? static_cast<visage::Frame*>(bar) : static_cast<visage::Frame*>(grid);
	if (road == RenameRoad::Menu) {
		if (bar != nullptr) {
			bar->pressContextForTest(bar->tabCentre(chain));
		}
		else {
			grid->pressContextForTest(grid->padCentre(chain));
		}
		visage::PopupMenuFrame* popup = menu_check::newestPopup(*source);
		if (popup == nullptr || !menu_check::pickRow(*popup, "Rename...")) {
			return out;
		}
	}
	else if (bar != nullptr) {
		bar->doubleClickForTest(bar->tabCentre(chain));
	}
	else {
		return out; // pads have no double-click road
	}
	settle(); // the popup's own exit timer and a refresh, as the host would give them
	out.opened = rename.renaming();
	ui::TextField& field = rename.field();
	out.visible = field.isVisible() && field.isDrawing();
	out.rect = visage::Bounds { field.positionInWindow().x, field.positionInWindow().y, static_cast<float>(field.width()),
		                        static_cast<float>(field.height()) };
	const visage::Bounds window { 0.0f, 0.0f, static_cast<float>(root->width()), static_cast<float>(root->height()) };
	out.inside = out.rect.width() > 0.0f && out.rect.height() > 0.0f && out.rect.x() >= window.x() && out.rect.y() >= window.y()
	             && out.rect.right() <= window.right() + 0.01f && out.rect.bottom() <= window.bottom() + 0.01f;
	const visage::Point centre { out.rect.x() + out.rect.width() * 0.5f, out.rect.y() + out.rect.height() * 0.5f };
	visage::Frame* hit = root->frameAtPoint(centre);
	out.topmost = hit == &field;
	out.hit = hit != nullptr ? typeid(*hit).name() : "null";
	out.focused = field.hasKeyboardFocus();
	return out;
}

} // namespace

namespace {

/// WHAT THE USER SEES AFTER A RENAME ROAD, asserted as values: the field is open, drawn, whole inside its window, the TOPMOST frame at its own
/// centre (a hit-test from the window root returns it, nothing overlays it), and typed characters plus Enter change the label. `reachable`
/// is whether the pad or tab itself can be pressed in this fixture (a card scrolled under the knob tray cannot, and says so).
void checkRenameSurface(const char* what, RenameRoad road, visage::Frame* root, ui::RackChainRename& rename, ui::RackTabBar* bar, ui::RackPadGrid* grid,
                        size_t chain, const std::function<void()>& settle, const std::function<std::string()>& labelOf) {
	INFO("surface: " << std::string(what));
	CAPTURE(road == RenameRoad::Menu);
	const RenameSeen s = renameSeen(road, root, rename, bar, grid, chain, settle);
	REQUIRE(s.opened);
	CHECK(s.visible);
	CHECK(s.inside);
	CHECK(s.rect.width() >= 60.0f);
	CHECK(s.rect.height() >= 16.0f);
	CHECK(s.rect.height() <= 40.0f); // a line, not a slab as tall as a card
	CAPTURE(s.hit);
	CHECK(s.topmost);
	ui::TextField& field = rename.field();
	field.textInput("Kick");
	CHECK(keyFromFocus(&field, visage::KeyEvent { visage::KeyCode::Return, 0, true }));
	settle();
	CHECK_FALSE(rename.renaming());
	CHECK(labelOf() == "Kick");
}

} // namespace

TEST_CASE("rename field: opened by the menu's own pick and by double-click, it is drawn whole, on top of everything, and typing renames - Tabs and Pads, on the card and in the window, pane on and off, nested") {
	for (const bool windowSide : { false, true }) {
		for (const int view : { 1, 2 }) {
			for (const RenameRoad road : { RenameRoad::Menu, RenameRoad::DoubleClick }) {
				if (view == 2 && road == RenameRoad::DoubleClick) {
					continue; // a pad has the menu only
				}
				for (const bool pane : { false, true }) {
					if (pane && !windowSide) {
						continue; // the pane is the window's
					}
					RealRack r;
					r.chains(3, 2, true);
					if (pane) {
						r.set(pr::Param::Front, 1.0f);
					}
					r.view_(view);
					for (int i = 0; i < 3; ++i) {
						r.refresh();
					}
					ui::RackTabBar* bar = windowSide ? &r.panel->tabBarForTest() : &r.card->tabBarForTest();
					ui::RackPadGrid* grid = windowSide ? &r.panel->padsForTest().gridForTest() : &r.card->padsForTest().gridForTest();
					ui::RackChainRename& rename = windowSide ? r.panel->chainRenameForTest() : r.card->chainRenameForTest();
					visage::Frame* root = windowSide ? static_cast<visage::Frame*>(r.panel) : r.f.root;
					const size_t chain = 1;
					// A CARD IN THE FIXTURE'S LANE IS SCROLLED PAST ITS PADS (under the knob tray): its pads are not a place a hand can press, so its Pads
					// are checked in the real window (test_gui_windowed), and here only for what does not depend on the tray.
					if (!windowSide && view == 2) {
						continue;
					}
					checkRenameSurface(windowSide ? (pane ? "window, pane on" : "window") : "card", road, root, rename, view == 1 ? bar : nullptr,
					                   view == 2 ? grid : nullptr, chain, [&] { r.refresh(); }, [&] { return r.rack().chainLabel(chain); });
				}
			}
		}
	}
	// THE STRIP CAPTION'S DOUBLE-CLICK, on the card and in the window (Strips view).
	{
		RealRack r;
		r.chains(3, 2, true);
		r.view_(0);
		for (const bool windowSide : { true }) {
			ui::RackChainRename& rename = r.panel->chainRenameForTest();
			ui::ChainStrip* strip = &r.panel->stripForTest(1);
			strip->doubleClickForTest(visage::Point { 2.0f, 2.0f });
			r.refresh();
			REQUIRE(rename.renaming());
			ui::TextField& field = rename.field();
			visage::Frame* root = windowSide ? static_cast<visage::Frame*>(r.panel) : r.f.root;
			const visage::Point at = field.positionInWindow();
			const visage::Point centre { at.x + static_cast<float>(field.width()) * 0.5f, at.y + static_cast<float>(field.height()) * 0.5f };
			CAPTURE(windowSide);
			CHECK(field.isVisible());
			CHECK(field.width() > 0);
			CHECK(root->frameAtPoint(centre) == static_cast<visage::Frame*>(&field));
			(void)keyFromFocus(&field, visage::KeyEvent { visage::KeyCode::Escape, 0, true });
		}
	}
	// A NESTED RACK'S OWN TABS, inside the outer window's column (Cards, so its card is drawn there).
	{
		RealRack r;
		r.chains(1, 0);
		feathers::rack::RackBridge& chain0 = *r.rack().chainAt(0)->bridge();
		const feathers::DeviceInstanceId inner = nestedRack(chain0, 3, true);
		r.view_(1);
		for (int i = 0; i < 3; ++i) {
			r.refresh();
		}
		ui::ParallelRackCard* nestedCard = findInTree<ui::ParallelRackCard>(r.panel);
		REQUIRE(nestedCard != nullptr);
		REQUIRE(nestedCard->isDrawing());
		ui::RackTabBar& bar = nestedCard->tabBarForTest();
		pr::Rack* innerRack = pr::rackOf(chain0.containerSurfaceOf(inner));
		REQUIRE(innerRack != nullptr);
		checkRenameSurface("nested card", RenameRoad::DoubleClick, r.panel, nestedCard->chainRenameForTest(), &bar, nullptr, 1,
		                   [&] { r.refresh(); }, [&] { return innerRack->chainLabel(1); });
		checkRenameSurface("nested card, menu", RenameRoad::Menu, r.panel, nestedCard->chainRenameForTest(), &bar, nullptr, 2,
		                   [&] { r.refresh(); }, [&] { return innerRack->chainLabel(2); });
	}
}

//--------------------------------------------------------------------------------------------------//
// A DEVICE'S PRESETS FROM THE HEADER MENU OF A RACK CHAIN'S COLUMN                                    //
//--------------------------------------------------------------------------------------------------//

namespace {

struct ColumnPresetRoot {
	std::filesystem::path dir;
	std::string was;
	bool had = false;
	ColumnPresetRoot() {
		dir = std::filesystem::temp_directory_path() / ("feathers-hc-" + std::to_string(::getpid()) + "-" + std::to_string(reinterpret_cast<uintptr_t>(this)));
		std::filesystem::remove_all(dir);
		std::filesystem::create_directories(dir);
		if (const char* existing = std::getenv("FEATHERS_PRESET_ROOT"); existing != nullptr) {
			had = true;
			was = existing;
		}
		::setenv("FEATHERS_PRESET_ROOT", dir.c_str(), 1);
	}
	~ColumnPresetRoot() {
		if (had) {
			::setenv("FEATHERS_PRESET_ROOT", was.c_str(), 1);
		}
		else {
			::unsetenv("FEATHERS_PRESET_ROOT");
		}
		std::error_code ec;
		std::filesystem::remove_all(dir, ec);
	}
};

bool popupHasSubmenu(visage::PopupMenuFrame& popup, const std::string& name) {
	visage::PopupList* rows = menu_check::rowsOf(popup);
	for (int i = 0; rows != nullptr && i < rows->numOptions(); ++i) {
		if (rows->option(i).hasOptions() && rows->option(i).name().toUtf8() == name) {
			return true;
		}
	}
	return false;
}

/// THE MENU OF `device` IN `column`, through the road its header's dots take: the entries exist, the save opens a field over the header
/// that is the topmost frame at its centre inside `root`, typing and Enter write the file, and a pick of the saved preset is one undo step.
void checkColumnPresets(const char* what, ui::ChainColumn& column, visage::Frame* root, feathers::rack::RackBridge& bridge, const std::string& name) {
	INFO("host: " << std::string(what));
	const feathers::DeviceInstanceId device = bridge.deviceAtPosition(0);
	REQUIRE(device != feathers::kNoInstance);
	const device::Descriptor* desc = bridge.descriptorOf(device);
	REQUIRE(desc != nullptr);
	column.showSlotMenu(device);
	visage::PopupMenuFrame* popup = menu_check::newestPopup(column);
	REQUIRE(popup != nullptr);
	CHECK(popupHasSubmenu(*popup, "Presets"));
	CHECK(menu_check::findRow(*popup, "     Init") != nullptr);
	REQUIRE(menu_check::pickRow(*popup, "Save preset..."));
	ui::LaneSelection& selection = column.selectionForTest();
	REQUIRE(selection.namingPreset());
	ui::TextField* field = selection.presetNameFieldForTest();
	REQUIRE(field != nullptr);
	CHECK(field->isVisible());
	CHECK(field->width() > 0);
	CHECK(field->height() > 0);
	if (root != nullptr) {
		const visage::Point at = field->positionInWindow();
		const visage::Point centre { at.x + static_cast<float>(field->width()) * 0.5f, at.y + static_cast<float>(field->height()) * 0.5f };
		CHECK(root->frameAtPoint(centre) == static_cast<visage::Frame*>(field));
	}
	field->textInput(name);
	CHECK(field->keyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true }));
	CHECK_FALSE(selection.namingPreset());
	CHECK(std::filesystem::exists(feathers::PresetStore::pathFor(name, feathers::devicePresetBank(*desc))));
	column.showSlotMenu(device);
	popup = menu_check::newestPopup(column);
	REQUIRE(popup != nullptr);
	CHECK(menu_check::findRow(*popup, "X  " + name) != nullptr);
	REQUIRE(menu_check::pickRow(*popup, "X  " + name));
}

} // namespace

TEST_CASE("header presets: a rack chain's device card menu offers Presets and Save preset on the card's column and the window's, and the name field is on top") {
	ColumnPresetRoot root;
	RealRack r;
	r.chains(2, 2, true);
	r.view_(1);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	feathers::rack::RackBridge& chain0 = *r.rack().chainAt(0)->bridge();
	ui::ChainColumn* windowColumn = r.panel->railForTest(0).column();
	REQUIRE(windowColumn != nullptr);
	checkColumnPresets("window rail column", *windowColumn, r.panel, chain0, "Window Saved");
	ui::ChainColumn* cardColumn = r.card->frontForTest().columnForTest();
	REQUIRE(cardColumn != nullptr);
	feathers::rack::RackBridge& viewed = *r.rack().chainAt(static_cast<size_t>(r.field(pr::Param::ViewedChain)))->bridge();
	checkColumnPresets("card column", *cardColumn, nullptr, viewed, "Card Saved");
}
