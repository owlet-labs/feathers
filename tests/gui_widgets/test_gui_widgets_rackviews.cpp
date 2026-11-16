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

#include "fx/drum/drum_types.h"
#include "plugin/ui/chain_column.h"
#include "plugin/ui/drum_device_card.h"
#include "plugin/device_registry.h"
#include "plugin/ui/fx_lane.h"
#include "plugin/ui/parallel_rack_card.h"
#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/rack_view_adapter.h"
#include "plugin/ui/rack_view_model.h"
#include "plugin/ui/rack_views.h"
#include "plugin/ui_note_ring.h"
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
		while (rack().chainCount() < count) {
			REQUIRE(rack().addChain() != SIZE_MAX);
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
	// A PRESS ON PAD 2 (key 2): the note is posted (on, then off at the release) and chain 2 is viewed.
	feathers::UiNoteEvent e;
	while (r.f.bridge.popNote(e)) {
	}
	REQUIRE(r.card->viewModelForTest().canTrigger());
	grid.pressForTest(grid.padCentre(2));
	grid.releaseForTest();
	REQUIRE(r.f.bridge.popNote(e));
	CHECK(e.key == 2);
	CHECK(e.on);
	REQUIRE(r.f.bridge.popNote(e));
	CHECK(e.key == 2);
	CHECK_FALSE(e.on);
	CHECK(r.field(pr::Param::ViewedChain) == 2.0f);
	// PAGES: a press on page 5 changes the rack's own PadsPage, and it is that page's keys the grid maps (80..95).
	pages.pressForTest(pages.pageCentre(5));
	r.refresh();
	CHECK(r.field(pr::Param::PadsPage) == 5.0f);
	grid.pressForTest(grid.padCentre(0));
	grid.releaseForTest();
	REQUIRE(r.f.bridge.popNote(e));
	CHECK(e.key == 80);
	while (r.f.bridge.popNote(e)) {
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
	// "+" ON EMPTY PAD 7 (key 7): a chain keyed to it, with a Drum on Any Key, and it becomes the viewed chain.
	const size_t before = r.rack().chainCount();
	grid.pressForTest(grid.plusCentre(7));
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

TEST_CASE("rack views over a real rack: Tabs is offered in the View control only up to eight chains (hidden past it, not greyed), on the card and the window") {
	RealRack r;
	r.chains(8, 0);
	r.refresh();
	CHECK(r.card->viewSegForTest().isVisible());
	CHECK_FALSE(r.card->viewPairForTest().isVisible());
	CHECK(r.panel->viewSegForTest().isVisible());
	CHECK_FALSE(r.panel->viewPairForTest().isVisible());
	// THE NINTH CHAIN: the choice is Strips | Pads, and it writes the SAME control (Pads is the control's 2).
	r.chains(9, 0);
	r.refresh();
	CHECK_FALSE(r.card->viewSegForTest().isVisible());
	CHECK(r.card->viewPairForTest().isVisible());
	CHECK_FALSE(r.panel->viewSegForTest().isVisible());
	CHECK(r.panel->viewPairForTest().isVisible());
	REQUIRE(r.card->viewPairForTest().control() != nullptr);
	r.card->viewPairForTest().control()->setValue(1.0);
	r.refresh();
	CHECK(r.field(pr::Param::View) == 2.0f);
	CHECK(r.card->padsShownForTest());
	r.card->viewPairForTest().control()->setValue(0.0);
	r.refresh();
	CHECK(r.field(pr::Param::View) == 0.0f);
	// A RACK ALREADY IN TABS (a load) still shows the three-way, which can say where it is.
	r.view_(1);
	CHECK(r.card->viewSegForTest().isVisible());
	CHECK_FALSE(r.card->viewPairForTest().isVisible());
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
	CHECK(card1->tabBarForTest().nativeWidth() == content);
	CHECK(card2->nativeWidth() == card1->frontForTest().nativeWidth());
	CHECK(card2->tabBarForTest().nativeWidth() == card2->nativeWidth());
	// A MEMBER CARD KEEPS NO PADDING OF ITS OWN across the flow: each rack's front is its content (its width less the card's two margins), and the
	// card inside it is exactly that wide.
	CHECK(card1->frontForTest().nativeWidth() == card1->nativeWidth() - static_cast<int>(2.0f * ui::ParallelRackCard::kPad * card1->dpiScale() + 0.5f));
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
	// THE BAR RUNS DOWN THE SIDE, THE FULL HEIGHT OF THE CARD, and the cards run right of it: opening the chain makes the card wider by a card.
	CHECK(r.card->tabBarForTest().nativeHeight() == r.card->nativeHeight());
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

TEST_CASE("rack views over a real rack: in Pads the selected pad's chain is in front as real cards, and viewing another pad rebinds") {
	RealRack r;
	r.chains(3, 2, true);
	r.view_(2);
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	ui::RackPadGrid& grid = r.card->padsForTest().gridForTest();
	feathers::rack::RackBridge& chain1 = *r.rack().chainAt(1)->bridge();
	grid.pressForTest(grid.padCentre(1)); // key 1 = chain 2
	grid.releaseForTest();
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	std::vector<ui::FxCard*> cards = shownCards(*r.card);
	REQUIRE(cards.size() == 1);
	CHECK(cards[0]->device() == chain1.deviceAtPosition(0));
	CHECK(r.card->frontForTest().chainIdForTest() == r.rack().chainIdAt(1));
	r.card->chainArrowForTest().toggleForTest();
	for (int i = 0; i < 3; ++i) {
		r.refresh();
	}
	cards = shownCards(*r.card);
	REQUIRE(cards.size() == 2);
	CHECK(cards[1]->device() == chain1.deviceAtPosition(1));
}
