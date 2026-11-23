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

// PIECE 16 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("delay-love S3: the decay line's slope is the loop gain the lane makes, and the floor it puts under Time is "
          "shaded") {
	namespace dl = feathers::fx::delay;
	DelayRig rig;
	feathers::rack::InnerChain* lane = dl::ensureFeedbackLane(rig.surface());
	REQUIRE(lane != nullptr);
	const DeviceInstanceId util = lane->bridge()->addDevice(feathers::fx::busutil::kTraits.tag);
	REQUIRE(util != kNoInstance);
	lane->bridge()->setField(util, static_cast<size_t>(feathers::fx::busutil::Param::Gain), 0.4f);
	const DeviceInstanceId clip = lane->bridge()->addDevice(feathers::fx::clip::kTraits.tag);
	REQUIRE(clip != kNoInstance);
	runDelayRig(rig, 48000);
	ui::WidgetRegistry registry;
	ui::DelayExpanded panel(rig.bridge, registry, rig.id);
	const visage::Point min = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel.updateContent();
	dl::TapPicture p;
	REQUIRE(ui::delayPicture(rig.bridge, rig.id, &p));
	// THE LANE'S PASS GAIN, MEASURED: Bus Utility's fader at 0.4 (CLIP below its knee passes at unity).
	const float g = feathers::fx::busutil::faderGain(0.4f);
	CHECK(p.lanePass == doctest::Approx(g).epsilon(0.01));
	const double fb = static_cast<double>(rig.bridge.fieldAt(rig.id, static_cast<size_t>(dl::Param::Feedback)));
	CHECK(panel.planeForTest().decaySlopeDbForTest() == doctest::Approx(20.0 * std::log10(fb * g)).epsilon(0.01));
	// THE FLOOR: CLIP at 4x is 6 samples late, so the shortest tap is 6 + 16 samples.
	CHECK(p.tapFloor == doctest::Approx((feathers::fx::clip::kOversampleLatency + dl::kLoopChunkMin) / 48000.0).epsilon(1e-4));
	CHECK(panel.planeForTest().floorRightForTest() == doctest::Approx(panel.planeForTest().strip().xAt(p.tapFloor)));
	CHECK(panel.planeForTest().floorRightForTest() > 0.0f);
}
TEST_CASE("delay-love S4: the window's HOLD band — Duck and its Release, the Freeze, and Clear as a momentary") {
	namespace dl = feathers::fx::delay;
	DelayRig rig;
	ui::WidgetRegistry registry;
	ui::DelayExpanded panel(rig.bridge, registry, rig.id);
	const visage::Point min = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel.updateContent();
	for (const dl::Param p : { dl::Param::Duck, dl::Param::DuckRelease, dl::Param::Freeze, dl::Param::Clear }) {
		CAPTURE(static_cast<int>(p));
		visage::Frame* w = panel.widgetForTest(p);
		REQUIRE(w != nullptr);
		CHECK(w->isVisible());
		// UNDER THE MODE BAND, inside the face.
		CHECK(w->y() > panel.widgetForTest(dl::Param::Feedback)->bottom());
		CHECK(w->bottom() <= panel.height());
		CHECK(w->right() <= panel.laneRailForTest().x());
	}
	// CLEAR: high while held, low on release — one rising edge a press.
	const auto clear = static_cast<size_t>(dl::Param::Clear);
	ui::ClearButton& button = panel.clearForTest();
	CHECK(rig.bridge.fieldAt(rig.id, clear) == 0.0f);
	button.pressForTest();
	CHECK(button.heldForTest());
	CHECK(rig.bridge.fieldAt(rig.id, clear) == 1.0f);
	button.releaseForTest();
	CHECK(rig.bridge.fieldAt(rig.id, clear) == 0.0f);
	// THE CARD DRAWS NONE OF THEM (Duck and Freeze left the card, not the window; CLEAR never was, a
	// grid cell would latch it). The window above holds them all; the card case below pins the rest.
	ui::DelayCard card(rig.bridge, rig.id);
	card.setBounds(0.0f, 0.0f, 240.0f, 600.0f);
	const auto& params = card.paramIndicesForTest();
	for (const dl::Param p : { dl::Param::Duck, dl::Param::DuckRelease, dl::Param::Freeze, dl::Param::Clear }) {
		CHECK(std::find(params.begin(), params.end(), static_cast<size_t>(p)) == params.end());
	}
}
TEST_CASE("delay card slimmed: no Duck, Release, Freeze or Spray on the card, all four in the window, and the card shorter "
          "by exactly their rows") {
	// The Delay card was too large, so Duck, Freeze and Spray left the card (not the window). PINNED BY VALUE in Clean and Granular: (1) the live card's controls; (2) the window still offers each
	// one, visible in its band; (3) the face's height, against the SAME card machinery fed the face as it was (the old
	// `kFaceOrder`, written out here), drops by an independent restatement of the rows that went — the DUCK / FREEZE
	// heading and its row in every mode, and in Granular the row Swarm sat on alone once Spray left the grain row.
	namespace dl = feathers::fx::delay;
	using P = dl::Param;
	const auto u8 = [](P p) { return static_cast<uint8_t>(p); };
	static const uint8_t kOldFace[] = {
		u8(P::Sync),       u8(P::Time),         u8(P::Mode),      u8(P::Dub),        u8(P::Mix),
		u8(P::Feedback),   u8(P::Tone),         u8(P::PingPong),  u8(P::Spread),
		u8(P::Duck),       u8(P::DuckRelease),  u8(P::Freeze),
		u8(P::GrainSize),  u8(P::Density),      u8(P::Spray),     u8(P::Pitch),      u8(P::Swarm),
		u8(P::Wow),        u8(P::Flutter),      u8(P::Saturation), u8(P::HfLoss),
		u8(P::Taps),       u8(P::TapPattern),   u8(P::TapLevels), u8(P::TapWidth),
		u8(P::BbdStages),  u8(P::BbdCompander), u8(P::BbdNoise),  u8(P::BbdDrive),
	};
	static const feathers::device::FaceSection kOldSections[] = {
		{ "FEEDBACK", u8(P::Feedback) },   { "DUCK / FREEZE", u8(P::Duck) }, { "GRAIN", u8(P::GrainSize) },
		{ "TAPE", u8(P::Wow) },            { "TAPS", u8(P::Taps) },          { "BUCKETS", u8(P::BbdStages) },
	};
	static const feathers::device::Descriptor kOld = [&] {
		feathers::device::Descriptor d = dl::kDescriptor;
		d.traits.faceOrder = kOldFace;
		d.traits.faceSections = kOldSections;
		return d;
	}();
	// ONE GRID ROW'S EXTENT, restated: its tallest cell (a stacked control shares the cell above it) plus the row gap.
	const auto rowExtent = [](std::initializer_list<P> row) {
		float tallest = 0.0f;
		float cell = 0.0f;
		bool first = true;
		for (const P p : row) {
			const feathers::device::ParamDesc& d = dl::kDescriptor.params[static_cast<size_t>(p)];
			const float h = ui::GenericCard::cellHeightFor(d);
			cell = (!first && d.stacked) ? cell + ui::GenericCard::kStackGap + h : h;
			tallest = std::max(tallest, cell);
			first = false;
		}
		return tallest + ui::GenericCard::kRowGap;
	};
	const float duckFreeze = ui::GenericCard::kSectionHeight + rowExtent({ P::Duck, P::DuckRelease, P::Freeze });
	DelayRig rig;
	ui::WidgetRegistry registry;
	ui::DelayExpanded window(rig.bridge, registry, rig.id);
	const visage::Point min = window.minimumSize();
	window.setBounds(0.0f, 0.0f, min.x, min.y);
	const auto mode = static_cast<size_t>(P::Mode);
	float stripInset = -1.0f;
	for (const dl::Mode m : { dl::Mode::Clean, dl::Mode::Granular }) {
		CAPTURE(static_cast<int>(m));
		rig.bridge.setField(rig.id, mode, static_cast<float>(static_cast<int>(m)));
		ui::DelayCard card(rig.bridge, rig.id);
		ui::GenericCard now(rig.bridge, rig.id, dl::kDescriptor, nullptr);
		ui::GenericCard before(rig.bridge, rig.id, kOld, nullptr);
		for (ui::GenericCard* c : { static_cast<ui::GenericCard*>(&card), &now, &before }) {
			c->setBounds(0.0f, 0.0f, 240.0f, 900.0f);
			(void)c->updateContent();
			c->resized();
		}
		(void)window.updateContent();
		const auto& params = card.paramIndicesForTest();
		for (const P p : { P::Duck, P::DuckRelease, P::Freeze, P::Spray }) {
			CAPTURE(static_cast<int>(p));
			CHECK(std::find(params.begin(), params.end(), static_cast<size_t>(p)) == params.end());
			// THE OLD FACE DID OFFER IT (Spray in Granular only, gated with the grain row): what the card lost, not a
			// control it never had.
			if (p != P::Spray || m == dl::Mode::Granular) {
				const auto& was = before.paramIndicesForTest();
				CHECK(std::find(was.begin(), was.end(), static_cast<size_t>(p)) != was.end());
			}
		}
		// THE WINDOW STILL HAS THEM: Duck, Release and Freeze in the HOLD band always, Spray in the Granular band.
		for (const P p : { P::Duck, P::DuckRelease, P::Freeze }) {
			REQUIRE(window.widgetForTest(p) != nullptr);
			CHECK(window.widgetForTest(p)->isVisible());
		}
		REQUIRE(window.widgetForTest(P::Spray) != nullptr);
		CHECK(window.widgetForTest(P::Spray)->isVisible() == (m == dl::Mode::Granular));
		// THE HEIGHT, BY VALUE.
		const float want = m == dl::Mode::Clean
		                       ? duckFreeze
		                       : duckFreeze + rowExtent({ P::GrainSize, P::Density, P::Spray, P::Pitch }) + rowExtent({ P::Swarm })
		                             - rowExtent({ P::GrainSize, P::Density, P::Pitch, P::Swarm });
		CHECK(want > 0.0f);
		CHECK(before.contentHeight() - now.contentHeight() == doctest::Approx(want).epsilon(1e-6));
		// AND THE DELAY CARD IS THAT FACE under its strip — the same inset in both modes, so it shrank by the same amount.
		const float inset = card.contentHeight() - now.contentHeight();
		if (stripInset < 0.0f) {
			stripInset = inset;
		}
		CHECK(inset == doctest::Approx(stripInset));
	}
}
TEST_CASE("delay-love S5: the Delay's window carries the presets browser in its title, over Devices/DELAY") {
	DelayRig rig;
	ui::WidgetRegistry registry;
	ui::DelayExpanded panel(rig.bridge, registry, rig.id);
	ui::PresetBrowser& browser = panel.presetBrowserForTest();
	CHECK(panel.titleAccessory() == &browser);
	CHECK(browser.bank() == "Devices/DELAY");
	CHECK(browser.indexOf("Init") >= 0);
}
TEST_CASE("Pattern's sources on the surfaces (design §7.10): the card shows the open tab's — its gate beside the seven, or its eight lanes in Modulation — every host lays it so; the window shows every gate and the open tab's Mod 1–8 in Modulation alone") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId dev = card->device();
	const auto shownChips = [&]() {
		f.panel->resized();
		std::set<size_t> shown;
		for (size_t o = 0; o < pt::kDescriptor.outputCount(); ++o) {
			if (f.panel->outputChipBounds(backplane::Source::output(dev, o)).width() > 0.0f) {
				shown.insert(o);
			}
		}
		return shown;
	};
	const auto expect = [](uint64_t mask) {
		std::set<size_t> s;
		for (size_t o = 0; o < 64; ++o) {
			if (((mask >> o) & 1u) != 0u) {
				s.insert(o);
			}
		}
		return s;
	};
	// TAB 1, MELODY: the seven and Tab 1's gate — the descriptor's own default, before and after the card asks.
	CHECK(pt::kDescriptor.outputsShownByDefault() == pt::cardOutputsFor(0, false));
	CHECK(f.bridge.outputsShown(dev) == pt::cardOutputsFor(0, false));
	CHECK(shownChips() == expect(pt::cardOutputsFor(0, false)));
	// TAB 4 IN MODULATION, OPENED: its eight lanes, nothing of the other tabs.
	f.deviceHost.setField(dev, pt::at(3, pt::Field::Mode), static_cast<float>(pt::Mode::Modulation));
	card->setFocus(3);
	(void)card->updateContent();
	CHECK(f.bridge.outputsShown(dev) == pt::cardOutputsFor(3, true));
	const std::set<size_t> four = shownChips();
	CHECK(four == expect(pt::cardOutputsFor(3, true)));
	CHECK(four.count(pt::laneOutput(3, 7)) == 1);
	CHECK(four.count(pt::laneOutput(0, 0)) == 0);
	// BACK TO TAB 1: Tab 4's lanes leave the card (a route from one reads on: test_rack_resolve's case).
	card->setFocus(0);
	(void)card->updateContent();
	CHECK(shownChips() == expect(pt::cardOutputsFor(0, false)));
	// THE WINDOW (the per-step modulators Mod 1–8 show only in Modulation mode, and only the active tab's):
	// the seven and every gate, and the OPEN tab's Mod 1–8 only while it is in Modulation — by value, at each tab.
	ui::ModState mod;
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, dev, pt::kDescriptor);
	REQUIRE(view != nullptr);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setModulation(&mod, nullptr);
	const visage::Point min = window->minimumSize();
	window->setBounds(0.0f, 0.0f, min.x, min.y);
	const auto shownArms = [&]() {
		std::set<size_t> s;
		for (size_t o = 0; o < pt::kDescriptor.outputCount(); ++o) {
			const ui::SourceArm* arm = window->armForTest(o);
			REQUIRE(arm != nullptr);
			if (arm->isVisible() && arm->width() > 0) {
				CHECK(arm->bottom() <= min.y);
				s.insert(o);
			}
		}
		return s;
	};
	for (size_t tab = 0; tab < pt::kPatterns; ++tab) {
		CAPTURE(tab);
		window->setFocus(tab);
		(void)window->updateContent();
		const bool modulation = tab == 3; // tab 4 was put in Modulation above; tabs 1–3 are Melody
		CHECK(shownArms() == expect(pt::windowOutputsFor(tab, modulation)));
		CHECK(shownArms().count(pt::laneOutput(3, 0)) == (modulation ? 1u : 0u)); // Tab 4's lanes on its own tab only
		CHECK(shownArms().count(pt::tabOutput(tab)) == 1u);
	}
	// A MODE CHANGE ON THE OPEN TAB moves them without a focus change: tab 4 back to Melody, its lanes go.
	f.deviceHost.setField(dev, pt::at(3, pt::Field::Mode), static_cast<float>(pt::Mode::Melody));
	(void)window->updateContent();
	CHECK(shownArms() == expect(pt::windowOutputsFor(3, false)));
	view.reset();
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("Modulation's lanes are named as themselves, never the kit's: the grid's rows, the inspector, the lane chips and the sources read Mod 1–8") {
	using namespace lane_check;
	namespace gn = feathers::mod::arp::genes;
	Window w(200.0f);
	w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::Modulation));
	(void)w.window->updateContent();
	w.window->setBounds(0.0f, 0.0f, w.window->minimumSize().x, w.window->minimumSize().y);
	ui::PatternLane& lane = w.lane();
	REQUIRE(lane.drumForTest());
	REQUIRE(lane.modulationForTest());
	const size_t eight = 7;
	const float x = lane.xOfStep(5) + 2.0f;
	const float y = lane.yOfLane(eight) + 2.0f;
	REQUIRE(lane.laneAt(y) == eight);
	lane.onMouseDown().callback(once(at(x, y)));
	lane.onMouseUp().callback(once(at(x, y)));
	CHECK(w.window->inspector().labelForTest() == "Step 6 · Mod 8");
	for (size_t l = 0; l < pn::kDrumLanes; ++l) {
		CAPTURE(l);
		const ui::SwitchChip* chip = w.window->drumChipForTest(l);
		REQUIRE(chip != nullptr);
		CHECK(chip->label() == "Mod " + std::to_string(l + 1));
		CHECK(std::string(pt::kOutputs[pt::laneOutput(0, l)].name) == "T1 Mod " + std::to_string(l + 1));
		CHECK(std::string(pt::kOutputs[pt::laneOutput(3, l)].name) == "T4 Mod " + std::to_string(l + 1));
	}
	// AND IN DRUM THE KIT'S NAMES STAY.
	w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::Drum));
	(void)w.window->updateContent();
	w.window->setBounds(0.0f, 0.0f, w.window->minimumSize().x + 1.0f, w.window->minimumSize().y);
	CHECK_FALSE(lane.modulationForTest());
	CHECK(w.window->drumChipForTest(7)->label() == pn::kDrumLaneShort[7]);
}
TEST_CASE("the step inspector's cell level: Velocity on a Drum hit, Value on any Modulation cell in its tab's polarity — set by value into the hand's edits, read back by the lane; and the lane switches' tooltips name Mod N in Modulation") {
	using namespace lane_check;
	using What = ui::StepControl::What;
	namespace gn = feathers::mod::arp::genes;
	for (const bool modulation : { false, true }) {
		for (const bool bipolar : { false, true }) {
			if (!modulation && bipolar) {
				continue;
			}
			CAPTURE(modulation);
			CAPTURE(bipolar);
			Window w(200.0f);
			w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::Mode),
			                        static_cast<float>(modulation ? gn::Mode::Modulation : gn::Mode::Drum));
			w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::ModPolarity), bipolar ? 1.0f : 0.0f);
			(void)w.window->updateContent();
			ui::PatternLane& lane = w.lane();
			// A CELL WITH A HIT (Drum's level is a hit's).
			size_t k = 0;
			while (k < lane.cycleForTest().length && lane.cycleForTest().steps[k].drumHits == 0u) {
				++k;
			}
			REQUIRE(k < lane.cycleForTest().length);
			const auto l = static_cast<size_t>(std::countr_zero(static_cast<unsigned>(lane.cycleForTest().steps[k].drumHits)));
			const float x = lane.xOfStep(k) + 2.0f;
			const float y = lane.yOfLane(l) + 2.0f;
			lane.onMouseDown().callback(once(at(x, y)));
			lane.onMouseUp().callback(once(at(x, y)));
			REQUIRE(lane.selectedStep() == static_cast<int>(k));
			REQUIRE(lane.selectedLane() == l);
			const What what = modulation ? What::CellValue : What::CellVelocity;
			CHECK(w.window->inspector().widgetForTest(what)->isVisible());
			CHECK_FALSE(w.window->inspector().widgetForTest(modulation ? What::CellVelocity : What::CellValue)->isVisible());
			ui::StepControl& cell = w.window->inspector().controlForTest(what);
			CHECK(std::string(cell.desc().name) == (modulation ? "Value" : "Velocity"));
			CHECK(cell.desc().min == (bipolar ? -1.0 : 0.0));
			// SET IT: the shown value, stored 0..1 at the block's step, read back by the lane's decode.
			const double shown = bipolar ? -0.5 : 0.42;
			cell.setValue(shown);
			const float stored = pt::quantizeLevel(static_cast<float>(bipolar ? 0.25 : 0.42));
			const pt::EditState edits = pt::editStateOf(w.edits().data(), w.edits().size());
			CHECK(edits[0].steps[k].cellLevel[l] == stored);
			CHECK(edits[0].steps[k].cellSet == static_cast<uint8_t>(1u << l));
			(void)w.window->updateContent();
			const float accent = ((lane.cycleForTest().steps[k].drumAccent >> l) & 1u) != 0u ? 0.25f : 0.0f;
			CHECK(lane.cycleForTest().steps[k].drumVelocity[l] == std::min(1.0f, stored + accent));
			if (accent == 0.0f) {
				CHECK(cell.value() == doctest::Approx(bipolar ? 2.0 * static_cast<double>(stored) - 1.0 : static_cast<double>(stored)));
			}
			// THE LANE SWITCH'S TOOLTIP: Mod N in Modulation, the kit's lane in Drum.
			w.window->setBounds(0.0f, 0.0f, w.window->minimumSize().x, w.window->minimumSize().y);
			const ui::SwitchChip* chip = w.window->drumChipForTest(l);
			REQUIRE(chip != nullptr);
			const std::string tip = chip->tooltipAt({ 1.0f, 1.0f }) != nullptr ? chip->tooltipAt({ 1.0f, 1.0f }) : "";
			if (modulation) {
				CHECK(tip.find(pn::kModLaneNames[l]) != std::string::npos);
				CHECK(tip.find(pn::kDrumLaneNames[l]) == std::string::npos);
			}
			else {
				CHECK(tip.find(pn::kDrumLaneNames[l]) != std::string::npos);
			}
		}
	}
}
TEST_CASE("SHOT Pattern's sources on the card and the window: a note tab, then a Modulation tab (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA for the 2026-11-07 report (every mod output was always visible on the card): the
	// whole editor with the Pattern in the modulator lane and its window open, on tab 1 (Melody) and on tab 4 (Modulation).
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	auto* root = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(root != nullptr);
	root->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	f.deviceHost.setField(id, pt::at(3, pt::Field::Mode), static_cast<float>(pt::Mode::Modulation));
	ui::PatternExpanded* window = nullptr;
	const auto shoot = [&](const char* name) {
		for (int i = 0; i < 8; ++i) {
			if (window != nullptr) {
				(void)window->updateContent();
			}
			(void)card->updateContent();
			f.panel->resized();
			root->redrawAll();
			root->drawWindow();
		}
		const visage::Screenshot& shot = root->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("SKIP no screenshot");
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		const visage::Point c = card->positionInWindow();
		MESSAGE(name << " scale " << root->dpiScale() << " card " << c.x << "," << c.y << " " << card->width() << "x" << card->height()
		             << " -> " << path);
	};
	// THE CARD ALONE FIRST (the window, once open, stands over it).
	card->setFocus(0);
	shoot("sources_card_tab1_melody");
	card->setFocus(3);
	shoot("sources_card_tab4_modulation");
	REQUIRE(f.panel->onExpandDevice != nullptr);
	f.panel->onExpandDevice(id);
	window = findInTree<ui::PatternExpanded>(f.root);
	REQUIRE(window != nullptr);
	window->setFocus(0);
	shoot("sources_window_tab1_melody");
	window->setFocus(3);
	shoot("sources_window_tab4_modulation");
}
TEST_CASE("Go's pending look (\"go button should have some pending ui state after you click it, until it fires\"): the window's Go reads the device's armed fill, published beside its playhead, and nothing else") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId dev = card->device();
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		f.deviceHost.setField(dev, pt::at(p, pt::Field::Fill), p >= 2 ? 1.0f : 0.0f);
	}
	ui::ModState mod;
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, dev, pt::kDescriptor);
	REQUIRE(view != nullptr);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, window->minimumSize().x, window->minimumSize().y);
	const auto publish = [&](size_t armedTab) {
		mod::Playhead where;
		where.ordinal = mod::Playhead::kIdle; // pending even while nothing plays
		where.level = armedTab < pt::kPatterns ? static_cast<float>(armedTab + 1) / 8.0f : 0.0f;
		where.haveLevel = true;
		f.bridge.publishRackPlayhead(slot, where);
		(void)window->updateContent();
	};
	publish(pt::kPatterns);
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		CHECK_FALSE(window->goChipForTest(p).pending());
	}
	publish(2); // tab 3's fill armed
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		CAPTURE(p);
		CHECK(window->goChipForTest(p).pending() == (p == 2));
	}
	CHECK_FALSE(window->goChipForTest(2).on()); // pending is not the press: the trigger itself is back at rest
	publish(pt::kPatterns); // fired
	CHECK_FALSE(window->goChipForTest(2).pending());
	view.reset();
	REQUIRE(f.bridge.removeRackDevice(slot));
}
TEST_CASE("SHOT Pattern's Go: idle, pending, fired (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA for the pending look, under FEATHERS_REDUCED_MOTION=1: the look must not need motion.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	auto* root = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(root != nullptr);
	root->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	f.deviceHost.setField(id, pt::at(pt::Global::PlayMode), 1.0f); // Cycle: the tab cells, with F and Go, on the window
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		f.deviceHost.setField(id, pt::at(p, pt::Field::Enable), 1.0f);
		f.deviceHost.setField(id, pt::at(p, pt::Field::Fill), p == 2 ? 1.0f : 0.0f);
	}
	REQUIRE(f.panel->onExpandDevice != nullptr);
	f.panel->onExpandDevice(id);
	auto* window = findInTree<ui::PatternExpanded>(f.root);
	REQUIRE(window != nullptr);
	const auto shoot = [&](const char* name, size_t armedTab) {
		mod::Playhead where;
		where.level = armedTab < pt::kPatterns ? static_cast<float>(armedTab + 1) / 8.0f : 0.0f;
		where.haveLevel = true;
		f.bridge.publishRackPlayhead(slot, where);
		for (int i = 0; i < 8; ++i) {
			(void)window->updateContent();
			root->redrawAll();
			root->drawWindow();
		}
		const visage::Screenshot& shot = root->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("SKIP no screenshot");
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		const visage::Point g = window->goChipForTest(2).positionInWindow();
		MESSAGE(name << " go " << g.x << "," << g.y << " " << window->goChipForTest(2).width() << "x"
		             << window->goChipForTest(2).height() << " pending " << window->goChipForTest(2).pending() << " -> " << path);
	};
	shoot("go_idle", pt::kPatterns);
	shoot("go_pending", 2);
	shoot("go_fired", pt::kPatterns);
}
TEST_CASE("F asleep while E is off (a fill tab does not join the cycle when E is off): the window's F reads dormant, and says why, exactly while its tab is a fill with E off") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId dev = card->device();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, dev, pt::kDescriptor);
	REQUIRE(view != nullptr);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, window->minimumSize().x, window->minimumSize().y);
	const std::string defaultTip = pt::kParamDescs[pt::at(1, pt::Field::Fill)].tooltip;
	// EVERY COMBINATION, BY VALUE: dormant iff F on and E off, and the tooltip names the reason exactly then.
	for (const bool fill : { false, true }) {
		for (const bool enable : { false, true }) {
			CAPTURE(fill);
			CAPTURE(enable);
			f.deviceHost.setField(dev, pt::at(1, pt::Field::Fill), fill ? 1.0f : 0.0f);
			f.deviceHost.setField(dev, pt::at(1, pt::Field::Enable), enable ? 1.0f : 0.0f);
			(void)window->updateContent();
			const ui::SwitchChip& chip = window->fillChipForTest(1);
			const bool asleep = fill && !enable;
			CHECK(chip.dormant() == asleep);
			CHECK(chip.on() == fill); // still the switch: dormant is a look, never a value
			const char* tip = chip.tooltipAt({ 1.0f, 1.0f });
			REQUIRE(tip != nullptr);
			CHECK((std::string(tip).find("E is off") != std::string::npos) == asleep);
			if (!asleep) {
				CHECK(std::string(tip) == defaultTip);
			}
		}
	}
	// TAB 1 (E on by default) never sleeps under its F.
	f.deviceHost.setField(dev, pt::at(0, pt::Field::Fill), 1.0f);
	(void)window->updateContent();
	CHECK_FALSE(window->fillChipForTest(0).dormant());
	view.reset();
}
TEST_CASE("Go's release takes the momentary's road: a press quicker than a block stays up until the synth has adopted it, then lands (lead, 2026-11-07: the press was LOST)") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId dev = card->device();
	f.deviceHost.setField(dev, pt::at(1, pt::Field::Fill), 1.0f);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, dev, pt::kDescriptor);
	REQUIRE(view != nullptr);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, window->minimumSize().x, window->minimumSize().y);
	(void)window->updateContent();
	const size_t go = pt::at(1, pt::Field::FillTrigger);
	// A HOST THAT LANDS (the plugin's visit): nothing adopted yet, so a rise published now is unread.
	f.bridge.landHeldControls(0);
	ui::SwitchChip& chip = window->goChipForTest(1);
	chip.onMouseDown().callback(at(1.0f, 1.0f));
	CHECK(f.deviceHost.fieldAt(dev, go) == 1.0f);
	chip.onMouseUp().callback(at(1.0f, 1.0f));
	CHECK(f.deviceHost.fieldAt(dev, go) == 1.0f); // HELD: no block has read the press
	f.bridge.landHeldControls(f.bridge.publishedChains().sequence()); // the synth adopted it
	CHECK(f.deviceHost.fieldAt(dev, go) == 0.0f); // landed: back at rest
	view.reset();
}
TEST_CASE("Pattern's Run (Note mode replaces Only w/ Transport): one segmented row in the window, every choice on its face — Always, Transport, Note — and a press on a segment writes that mode") {
	namespace pt = feathers::mod::arp::pat;
	namespace ap = feathers::mod::arp;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId dev = card->device();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, dev, pt::kDescriptor);
	REQUIRE(view != nullptr);
	view->setBounds(0.0f, 0.0f, std::max(view->minimumSize().x, 900.0f), std::max(view->minimumSize().y, 600.0f));
	const size_t run = pt::at(pt::Global::Run);
	std::vector<ui::SegmentedControl*> segs;
	std::vector<visage::Frame*> stack { view.get() };
	while (!stack.empty()) {
		visage::Frame* fr = stack.back();
		stack.pop_back();
		if (auto* s = dynamic_cast<ui::SegmentedControl*>(fr); s != nullptr && s->control() != nullptr
		    && std::string(s->control()->desc().key) == "run") {
			segs.push_back(s);
		}
		for (visage::Frame* c : fr->children()) {
			stack.push_back(c);
		}
	}
	REQUIRE(segs.size() == 1);
	ui::SegmentedControl& seg = *segs.front();
	CHECK(seg.isVisible());
	CHECK(seg.width() >= 200.0f); // its own row, wide enough for three names
	CHECK(f.deviceHost.fieldAt(dev, run) == static_cast<float>(ap::RunMode::Transport)); // the default
	seg.select(static_cast<int>(ap::RunMode::Note));
	CHECK(f.deviceHost.fieldAt(dev, run) == static_cast<float>(ap::RunMode::Note));
	seg.select(static_cast<int>(ap::RunMode::Always));
	CHECK(f.deviceHost.fieldAt(dev, run) == static_cast<float>(ap::RunMode::Always));
	view.reset();
}
TEST_CASE("SHOT the glow draws: armed, routed (focused), routed from an unfocused source (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA for backlog QK's new draws, under FEATHERS_REDUCED_MOTION=1 and VISAGE_FULL_REDRAW=1:
	// Humanize's ZoneSelect glow and ring, Latch routed upward while ON, Go routed, the Selector knob's unfocused dot.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	auto* root = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(root != nullptr);
	root->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = card->device();
	f.deviceHost.setField(id, pt::at(pt::Global::PlayMode), 1.0f); // Cycle: F and Go on the window
	f.deviceHost.setField(id, pt::at(pt::Global::Latch), 1.0f);    // ON, then routed upward
	for (size_t p = 0; p < pt::kPatterns; ++p) {
		f.deviceHost.setField(id, pt::at(p, pt::Field::Enable), 1.0f);
		f.deviceHost.setField(id, pt::at(p, pt::Field::Fill), p == 2 ? 1.0f : 0.0f);
	}
	REQUIRE(f.panel->onExpandDevice != nullptr);
	f.panel->onExpandDevice(id);
	auto* window = findInTree<ui::PatternExpanded>(f.root);
	REQUIRE(window != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	const auto findIn = [&](visage::Frame* from, const char* key) -> visage::Frame* {
		std::vector<visage::Frame*> stack { from };
		while (!stack.empty()) {
			visage::Frame* fr = stack.back();
			stack.pop_back();
			const device::Modulatable* c = nullptr;
			if (auto* s = dynamic_cast<ui::SwitchChip*>(fr)) {
				c = &s->control();
			}
			else if (auto* z = dynamic_cast<ui::ZoneSelect*>(fr)) {
				c = &z->control();
			}
			else if (auto* k = dynamic_cast<ui::Knob*>(fr)) {
				c = k->control();
			}
			if (c != nullptr && fr->isVisible() && c->desc().key != nullptr && std::string(c->desc().key) == key) {
				return fr;
			}
			for (visage::Frame* ch : fr->children()) {
				stack.push_back(ch);
			}
		}
		return nullptr;
	};
	const auto find = [&](const char* key) -> visage::Frame* {
		visage::Frame* w = findIn(window, key); // the window's face first: the card's is half under the window
		return w != nullptr ? w : findIn(f.root, key);
	};
	const char* keys[] = { "humanize", "latch", "p3.fill_trigger", "selector", "p1.rate", "p1.length" };
	const backplane::Source source = backplane::Source::core(0);
	const auto shoot = [&](const char* name) {
		for (int i = 0; i < 8; ++i) {
			(void)window->updateContent();
			root->redrawAll();
			root->drawWindow();
		}
		const visage::Screenshot& shot = root->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("SKIP no screenshot");
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		for (const char* k : keys) {
			visage::Frame* w = find(k);
			if (w == nullptr) {
				MESSAGE(std::string(name) << " " << std::string(k) << " NOT FOUND");
				continue;
			}
			const visage::Point g = w->positionInWindow();
			const auto* face = dynamic_cast<const ui::ControlFace*>(w);
			const device::Modulatable* c = nullptr;
			if (auto* s = dynamic_cast<ui::SwitchChip*>(w)) c = &s->control();
			else if (auto* z = dynamic_cast<ui::ZoneSelect*>(w)) c = &z->control();
			else if (auto* kn = dynamic_cast<ui::Knob*>(w)) c = kn->control();
			const ui::ControlFace::ModLook look = face != nullptr && c != nullptr ? face->modLookFor(*c) : ui::ControlFace::ModLook {};
			MESSAGE(std::string(name) << " " << std::string(k) << " at " << g.x << "," << g.y << " " << w->width() << "x" << w->height() << " glow "
			             << look.armedGlow << " mark " << look.routedMark << " -> " << path);
		}
	};
	shoot("glow_rest");
	mod->focus(source);
	mod->arm(true);
	shoot("glow_armed");
	for (const char* k : keys) {
		if (visage::Frame* w = find(k)) {
			const device::Modulatable* c = nullptr;
			if (auto* s = dynamic_cast<ui::SwitchChip*>(w)) c = &s->control();
			else if (auto* z = dynamic_cast<ui::ZoneSelect*>(w)) c = &z->control();
			else if (auto* kn = dynamic_cast<ui::Knob*>(w)) c = kn->control();
			if (const auto* road = dynamic_cast<const ui::DeviceControl*>(c)) {
				f.bridge.setRouteAmount(source, road->destination(), 0.5f);
			}
		}
	}
	// THE PUCK AND THE START MARKER TOO: routed on both axes, and Start.
	if (auto* terrain = findInTree<ui::PatternTerrain>(window)) {
		f.bridge.setRouteAmount(source, terrain->placeXForTest().destination(), 0.5f);
		f.bridge.setRouteAmount(source, terrain->placeYForTest().destination(), 0.5f);
		const visage::Point g = terrain->positionInWindow();
		const visage::Point p = terrain->puckPosition();
		MESSAGE("puck at " << g.x + p.x << "," << g.y + p.y);
	}
	if (auto* lane = findInTree<ui::PatternLane>(window); lane != nullptr && lane->startControlForTest() != nullptr) {
		f.bridge.setRouteAmount(source, lane->startControlForTest()->destination(), 0.5f);
		const visage::Point g = lane->positionInWindow();
		MESSAGE("start marker at " << g.x + lane->markX(lane->startMarkForTest()) << "," << g.y);
	}
	mod->arm(false);
	shoot("glow_routed_focused");
	mod->focus(backplane::Source::core(1)); // the routes are another source's now
	shoot("glow_routed_unfocused");
	// A KNOB FULLY ON SCREEN (the strip card's are cut at the strip's edge, and the window covers the rest): the voice
	// section's Detune, routed through `Modulatable::setAmount` under the focused source — the knob's draw is the same for any
	// control. The window's overlay closed first.
	visage::Frame* top = window;
	while (top->parent() != nullptr && top->parent() != f.root) {
		top = top->parent();
	}
	top->setVisible(false);
	ui::Knob* detune = nullptr;
	{
		std::vector<visage::Frame*> stack { f.root };
		while (!stack.empty() && detune == nullptr) {
			visage::Frame* fr = stack.back();
			stack.pop_back();
			if (auto* k = dynamic_cast<ui::Knob*>(fr); k != nullptr && k->isVisible() && k->control() != nullptr
			                                           && k->positionInWindow().x > 40.0f && k->positionInWindow().x < 110.0f
			                                           && k->positionInWindow().y < 200.0f && k->control()->display().isDestination) {
				detune = k;
			}
			for (visage::Frame* ch : fr->children()) {
				stack.push_back(ch);
			}
		}
	}
	REQUIRE(detune != nullptr);
	const visage::Point dp = detune->positionInWindow();
	MESSAGE("detune at " << dp.x << "," << dp.y << " " << detune->width() << "x" << detune->height());
	const auto shootKnob = [&](const char* name) {
		shoot(name);
		const ui::ControlFace::ModLook look = detune->modLookFor(*detune->control());
		MESSAGE(std::string(name) << " detune glow " << look.armedGlow << " mark " << look.routedMark);
	};
	mod->arm(false);
	mod->focus(source);
	shootKnob("knob_rest");
	mod->arm(true);
	shootKnob("knob_armed");
	mod->arm(false);
	detune->control()->setAmount(0.5f); // routed from the focused source
	shootKnob("knob_routed_focused");
	mod->focus(backplane::Source::core(1));
	shootKnob("knob_routed_unfocused");
}

namespace {
/// A WHEEL AT A FRAME, walked the way visage's window handler walks it (`WindowEventHandler`): the frame under the
/// pointer first, then up the parents until one answers. Returns the frame that took it, or null.
visage::Frame* wheelThrough(visage::Frame* start, visage::Point local, float dy, int modifiers) {
	visage::MouseEvent e;
	e.wheel_delta_y = dy;
	e.precise_wheel_delta_y = dy;
	e.modifiers = modifiers;
	const visage::Point window = start->positionInWindow() + local;
	e.window_position = window;
	for (visage::Frame* at = start; at != nullptr; at = at->parent()) {
		if (at->ignoresMouseEvents()) {
			continue;
		}
		e.position = window - at->positionInWindow();
		e.event_frame = at;
		if (at->processMouseWheel(e)) {
			return at;
		}
	}
	return nullptr;
}
/// TWIN FIELD IN THE FX LANE, with the lane short enough to scroll.
struct TwinFieldLaneFixture {
	FilterFixture f;
	size_t slot = 0;
	ui::TwinFieldCard* card = nullptr;
	TwinFieldLaneFixture() {
		slot = f.bridge.addFxDevice(feathers::mod::twinfield::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		// THREE MORE BEHIND IT, so the lane is taller than its view and has somewhere to scroll — without shrinking the
		// lane, which would leave the first card's pad outside the lane where no press can reach it.
		for (int more = 0; more < 3; ++more) {
			REQUIRE(f.bridge.addFxDevice(feathers::mod::twinfield::kTraits.tag) < feathers::kMaxFxSlots);
		}
		f.lane->resized();
		card = dynamic_cast<ui::TwinFieldCard*>(f.lane->cardAt(slot));
		REQUIRE(card != nullptr);
	}
	[[nodiscard]] double field(feathers::mod::twinfield::LayerParam p) const {
		return card->control(feathers::mod::twinfield::layerParam(0, p)).value();
	}
};
} // namespace

TEST_CASE("Twin Field's card in the fx lane: with the orbit Off, Phase takes Rate's cell and Sync hides; Free puts them back") {
	// THE VERTICAL LANE'S LAYOUT (the rack's horizontal one is test_rack_editor's TFIELD Off case): one cell, Rate while the
	// orbit runs and Phase while it is Off, each shown only where it changes the sound.
	namespace tf = feathers::mod::twinfield;
	TwinFieldLaneFixture t;
	ui::TwinFieldCard& card = *t.card;
	REQUIRE(card.laneAxis() == ui::FxCard::LaneAxis::Vertical);
	(void)card.updateContent();
	REQUIRE(card.rate(0).isVisible());
	CHECK(card.sync(0).isVisible());
	CHECK_FALSE(card.orbitPhase(0).isVisible());
	const visage::Bounds rateCell { card.rate(0).x(), card.rate(0).y(), card.rate(0).width(), card.rate(0).height() };
	card.control(tf::layerParam(0, tf::LayerParam::Mode)).setValue(static_cast<double>(tf::Mode::Off));
	(void)card.updateContent();
	CHECK_FALSE(card.rate(0).isVisible());
	CHECK_FALSE(card.sync(0).isVisible());
	REQUIRE(card.orbitPhase(0).isVisible());
	CHECK(card.orbitPhase(0).x() == rateCell.x());
	CHECK(card.orbitPhase(0).y() == rateCell.y());
	CHECK(card.orbitPhase(0).width() == rateCell.width());
	card.control(tf::layerParam(0, tf::LayerParam::Mode)).setValue(static_cast<double>(tf::Mode::Free));
	(void)card.updateContent();
	CHECK(card.rate(0).isVisible());
	CHECK(card.sync(0).isVisible());
	CHECK_FALSE(card.orbitPhase(0).isVisible());
}
TEST_CASE("Twin Field's card: the wheel over the terrain scrolls the lane and leaves Zoom until the card is focused; then a plain wheel zooms") {
	// Scrolling across the lane, the Twin Field card's terrain took over the wheel as it passed under the
	// pointer. FOCUS ALONE DECIDES (`ui::cardTakesWheel`): an unfocused card hands every wheel to the lane, Ctrl/Cmd
	// included; a focused card's terrain zooms on a plain wheel.
	namespace tf = feathers::mod::twinfield;
	TwinFieldLaneFixture t;
	ui::FxLane& lane = *t.f.lane;
	// A LANE THAT CAN SCROLL, so "the lane took it" is a value and not a no-op.
	REQUIRE(lane.maxScroll() > 0.0f);
	lane.setScroll(0.0f);
	ui::TwinFieldPad& pad = t.card->pad();
	REQUIRE(pad.width() > 1.0f);
	const visage::Point middle { pad.width() * 0.5f, pad.height() * 0.5f };
	const double zoom0 = t.field(tf::LayerParam::Zoom);
	t.card->setFocused(false);

	// UNFOCUSED: the lane answers, it scrolled, and Zoom is unchanged by value.
	CHECK(wheelThrough(&pad, middle, -1.0f, 0) == &lane);
	CHECK(lane.scroll() > 0.0f);
	CHECK(t.field(tf::LayerParam::Zoom) == zoom0);
	// AND NO MODIFIER ROAD: Ctrl and Cmd are declined too.
	lane.setScroll(0.0f);
	CHECK(wheelThrough(&pad, middle, 1.0f, visage::kModifierRegCtrl) == &lane);
	CHECK(wheelThrough(&pad, middle, 1.0f, visage::kModifierCmd) == &lane);
	CHECK(t.field(tf::LayerParam::Zoom) == zoom0);

	// FOCUSED: a PLAIN wheel zooms by exactly one notch each way, and the lane does not move.
	lane.setScroll(0.0f);
	t.card->setFocused(true);
	CHECK(wheelThrough(&pad, middle, 1.0f, 0) == &pad);
	CHECK(std::abs(t.field(tf::LayerParam::Zoom) - (zoom0 + ui::TwinFieldPad::kZoomStepOct)) <= 1e-6);
	CHECK(wheelThrough(&pad, middle, -1.0f, 0) == &pad);
	CHECK(std::abs(t.field(tf::LayerParam::Zoom) - zoom0) <= 1e-6);
	CHECK(lane.scroll() == 0.0f);
}
TEST_CASE("Twin Field's pad pans in AutoMod's and Explore's direction: the puck follows the hand, right is +X, up is +Y") {
	// The puck panned the opposite way from every other surface. The SAME GESTURE on both pads — press at
	// the centre, move 30 px right and 20 px up, still held — read by value: AutoMod's star and Twin Field's pan each
	// move by the pixels times their own plane-per-pixel, in the same direction.
	namespace tf = feathers::mod::twinfield;
	AutomodPanelFixture a;
	REQUIRE(a.panel != nullptr);
	auto* amPad = const_cast<ui::AutomodPad*>(a.panel->padForTest());
	REQUIRE(amPad != nullptr);
	const float dx = 30.0f;
	const float dy = -20.0f; // screen up
	const auto gesture = [&](visage::Frame& pad) {
		const visage::Point c { pad.width() * 0.5f, pad.height() * 0.5f };
		pad.processMouseDown(at(c.x, c.y));
		pad.processMouseDrag(at(c.x + dx, c.y + dy));
	};
	gesture(*amPad);
	const auto [amX, amY] = amPad->starForTest();
	const double amK = amPad->planePerPixelForTest();
	CHECK(amX == doctest::Approx(dx * amK));
	CHECK(amY == doctest::Approx(-dy * amK)); // UP IS MORE
	REQUIRE(amX > 0.0);
	REQUIRE(amY > 0.0);

	const size_t slot = a.f.bridge.addFxDevice(tf::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	a.f.lane->resized();
	auto* card = dynamic_cast<ui::TwinFieldCard*>(a.f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	ui::TwinFieldPad& pad = card->pad();
	REQUIRE(pad.width() > 1.0f);
	const auto pan = [&](tf::LayerParam p) { return card->control(tf::layerParam(0, p)).value(); };
	const double x0 = pan(tf::LayerParam::PanX);
	const double y0 = pan(tf::LayerParam::PanY);
	const double k = pad.planePerPixel();
	gesture(pad);
	const double tfX = pan(tf::LayerParam::PanX) - x0;
	const double tfY = pan(tf::LayerParam::PanY) - y0;
	pad.processMouseUp(at(pad.width() * 0.5f + dx, pad.height() * 0.5f + dy));
	CHECK(tfX == doctest::Approx(dx * k).epsilon(1e-4));
	CHECK(tfY == doctest::Approx(-dy * k).epsilon(1e-4));
	// THE SAME DIRECTION AS AUTOMOD'S, axis by axis.
	CHECK((tfX > 0.0) == (amX > 0.0));
	CHECK((tfY > 0.0) == (amY > 0.0));
}

namespace {
/// A PRESS AT `window`, the whole of `WindowEventHandler::handleMouseDown`: the keyboard focus moves first (the hit
/// frame, up to the first ancestor that accepts keystrokes — `keyboardFocusAfterPress`), the frame that loses it is
/// told `focusChanged(false, true)` and the one that gains it `focusChanged(true, true)`, then the press and release
/// go through the tree (`dragThroughRoot`). Returns the new focus, which the caller carries to the next press.
visage::Frame* pressWithFocus(visage::Frame* root, visage::Frame* focus, visage::Point window) {
	visage::Frame* next = keyboardFocusAfterPress(root, window);
	if (focus != nullptr && focus != next) {
		focus->processFocusChanged(false, true);
	}
	if (next != nullptr) {
		next->processFocusChanged(true, true);
	}
	dragThroughRoot(root, window, window);
	return next;
}
} // namespace

TEST_CASE("Twin Field's pad: a press on it selects its CARD (visage's focus goes up to the card), and that is what lets a plain wheel zoom") {
	namespace tf = feathers::mod::twinfield;
	TwinFieldLaneFixture t;
	ui::FxLane& lane = *t.f.lane;
	REQUIRE(lane.maxScroll() > 0.0f);
	lane.setScroll(0.0f);
	ui::TwinFieldPad& pad = t.card->pad();
	REQUIRE(pad.isVisible());
	const visage::Point middle { pad.width() * 0.5f, pad.height() * 0.5f };
	const double zoom0 = t.field(tf::LayerParam::Zoom);
	t.card->setFocused(false);

	// UNSELECTED: the wheel scrolls the lane, Zoom unchanged — and the wheel did not select the card.
	CHECK(wheelThrough(&pad, middle, -1.0f, 0) == &lane);
	CHECK(t.field(tf::LayerParam::Zoom) == zoom0);
	CHECK_FALSE(t.card->headFocused());
	lane.setScroll(0.0f);

	// A PRESS ON THE PAD: the keyboard focus lands on the card (the pad takes no keystrokes) and selects it.
	(void)pressWithFocus(t.f.root, nullptr, pad.positionInWindow() + middle);
	CHECK(t.card->headFocused());
	CHECK(wheelThrough(&pad, middle, 1.0f, 0) == &pad);
	CHECK(std::abs(t.field(tf::LayerParam::Zoom) - (zoom0 + ui::TwinFieldPad::kZoomStepOct)) <= 1e-6);
	CHECK(lane.scroll() == 0.0f);

	// DESELECTED again: the lane's.
	t.card->setFocused(false);
	const double zoom1 = t.field(tf::LayerParam::Zoom);
	CHECK(wheelThrough(&pad, middle, -1.0f, 0) == &lane);
	CHECK(t.field(tf::LayerParam::Zoom) == zoom1);
}
TEST_CASE("Card wheel rule: a press anywhere on a card selects it for the gate; a neighbour's press or a click on the bare lane ends it, and the bare lane wakes no card") {
	namespace tf = feathers::mod::twinfield;
	TwinFieldLaneFixture t;
	ui::FxLane& lane = *t.f.lane;
	REQUIRE(lane.maxScroll() > 0.0f);
	lane.setScroll(0.0f);
	ui::TwinFieldPad& pad = t.card->pad();
	const visage::Point middle { pad.width() * 0.5f, pad.height() * 0.5f };
	auto* other = dynamic_cast<ui::TwinFieldCard*>(lane.cardAt(t.slot + 1));
	REQUIRE(other != nullptr);
	ui::TwinFieldPad& otherPad = other->pad();
	const visage::Point otherMiddle { otherPad.width() * 0.5f, otherPad.height() * 0.5f };
	const auto zoomOf = [&](ui::TwinFieldCard& c) { return c.control(tf::layerParam(0, tf::LayerParam::Zoom)).value(); };
	const double zoom0 = zoomOf(*t.card);
	const double otherZoom0 = zoomOf(*other);
	// A POINT OF THE BARE LANE: one the lane itself answers to (no card, no child under it).
	const auto findBare = [&] {
		for (float y = 0.0f; y < lane.height(); y += 3.0f) {
			for (float x = 0.0f; x < lane.width(); x += 3.0f) {
				const visage::Point w = lane.positionInWindow() + visage::Point { x, y };
				if (t.f.root->frameAtPoint(w) == &lane) {
					return w;
				}
			}
		}
		FAIL("the lane has no bare point");
		return visage::Point {};
	};
	const visage::Point bare = findBare();

	// A PRESS ON THE BARE LANE FIRST (it takes visage's focus): NO CARD IS WOKEN by the lane's own focus.
	visage::Frame* focus = pressWithFocus(t.f.root, nullptr, bare);
	CHECK(wheelThrough(&pad, middle, 1.0f, 0) == &lane);
	CHECK(wheelThrough(&otherPad, otherMiddle, 1.0f, 0) != &otherPad);
	CHECK(zoomOf(*t.card) == zoom0);
	CHECK(zoomOf(*other) == otherZoom0);
	lane.setScroll(0.0f);

	// A PRESS ON THE CARD'S HEAD (not its pad) makes it the focused card: its pad zooms by a notch, the neighbour's does not.
	focus = pressWithFocus(t.f.root, focus, t.card->positionInWindow() + visage::Point { 4.0f, 4.0f });
	REQUIRE(t.card->headFocused());
	CHECK(wheelThrough(&pad, middle, 1.0f, 0) == &pad);
	CHECK(std::abs(zoomOf(*t.card) - (zoom0 + ui::TwinFieldPad::kZoomStepOct)) <= 1e-6);
	CHECK(lane.scroll() == 0.0f);
	CHECK(wheelThrough(&otherPad, otherMiddle, 1.0f, 0) != &otherPad);
	CHECK(zoomOf(*other) == otherZoom0);

	// A PRESS ON A NEIGHBOUR CARD moves it: the first card declines again.
	lane.scrollCardIntoView(t.slot + 1);
	lane.resized();
	const visage::Point onOther = other->positionInWindow() + visage::Point { 4.0f, 4.0f };
	REQUIRE(t.f.root->frameAtPoint(onOther) != nullptr);
	focus = pressWithFocus(t.f.root, focus, onOther);
	CHECK(other->headFocused());
	CHECK_FALSE(t.card->headFocused());
	const double zoom1 = zoomOf(*t.card);
	CHECK(wheelThrough(&pad, middle, -1.0f, 0) != &pad);
	CHECK(zoomOf(*t.card) == zoom1);

	// A CLICK ON THE BARE LANE clears the selection: the second card declines too.
	focus = pressWithFocus(t.f.root, focus, findBare());
	CHECK_FALSE(other->headFocused());
	CHECK(wheelThrough(&otherPad, otherMiddle, -1.0f, 0) != &otherPad);
	CHECK(zoomOf(*other) == otherZoom0);
}
TEST_CASE("Wobble on the card (by design): a small box at Humanize's right on Pattern and at the Feel cell's right on the classic Arp — its own control, beside the cell and never on it, inside the card; in a chain's unit Pattern leaves it to the window and the Arp keeps it in its Feel row, and nothing overflows") {
	namespace pt = feathers::mod::arp::pat;
	namespace ap = feathers::mod::arp;
	const auto overlaps = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() - 0.5f && b.x() < a.right() - 0.5f && a.y() < b.bottom() - 0.5f && b.y() < a.bottom() - 0.5f;
	};
	const auto inCard = [](const ui::FxCard& card, const visage::Frame& w) {
		const visage::Bounds b = w.bounds();
		return b.x() >= -0.5f && b.right() <= card.width() + 0.5f && b.y() >= -0.5f && b.bottom() <= card.height() + 0.5f;
	};
	SUBCASE("Pattern") {
		ModFixture f;
		const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		ui::ValueBox& wobble = card->wobbleForTest();
		CHECK(std::string(wobble.control().desc().key) == "wobble");
		CHECK(wobble.isVisible());
		CHECK(inCard(*card, wobble));
		const visage::Bounds cell = card->humanizeForTest().bounds();
		CHECK(wobble.bounds().x() >= cell.right()); // at its right, never on it
		CHECK_FALSE(overlaps(wobble.bounds(), cell));
		CHECK(wobble.width() >= 40.0f);
		wobble.control().setValue(0.25);
		CHECK(f.deviceHost.fieldAt(card->device(), pt::at(pt::Global::Wobble)) == doctest::Approx(0.25f));
		checkFitsChainUnit(*card);
		CHECK_FALSE(wobble.isVisible()); // the unit leaves it to the window, as it does Humanize
	}
	SUBCASE("the classic Arp") {
		ModFixture f;
		const size_t slot = f.bridge.addRackDevice(ap::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::ArpCard*>(dynamic_cast<const ui::ArpCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		ui::ValueBox& wobble = card->wobbleForTest();
		CHECK(std::string(wobble.control().desc().key) == "wobble");
		CHECK(wobble.isVisible());
		CHECK(inCard(*card, wobble));
		const visage::Bounds cell = card->feel().bounds();
		CHECK(wobble.bounds().x() >= cell.right());
		CHECK_FALSE(overlaps(wobble.bounds(), cell));
		CHECK(wobble.width() >= 40.0f);
		wobble.control().setValue(-0.5);
		CHECK(f.deviceHost.fieldAt(card->device(), ap::at(ap::Param::Wobble)) == doctest::Approx(-0.5f));
		checkFitsChainUnit(*card);
		CHECK(wobble.isVisible()); // in the Feel's own row
		CHECK_FALSE(overlaps(wobble.bounds(), card->feel().bounds()));
	}
}
TEST_CASE("SHOT Wobble on the Pattern and Arp cards (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA for the decided layout, under FEATHERS_REDUCED_MOTION=1 and VISAGE_FULL_REDRAW=1.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	for (const char* tag : { feathers::mod::arp::pat::kTraits.tag, feathers::mod::arp::kTraits.tag }) {
		ModFixture f;
		auto* root = dynamic_cast<visage::ApplicationEditor*>(f.root);
		REQUIRE(root != nullptr);
		root->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
		const size_t slot = f.bridge.addRackDevice(tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		const visage::Frame* card = f.panel->cardFrameForTest(slot);
		REQUIRE(card != nullptr);
		// THE STRIP AS TALL AS THE WINDOW, so the whole card is on screen (the fixture's strip scrolls a tall card).
		f.panel->setBounds(f.panel->x(), 0.0f, f.panel->width(), f.root->height());
		f.panel->resized();
		for (int i = 0; i < 8; ++i) {
			f.gui.tickForTest();
			root->redrawAll();
			root->drawWindow();
		}
		const visage::Screenshot& shot = root->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("SKIP no screenshot");
			return;
		}
		const std::string path = std::string(dir) + "/card_" + tag + ".png";
		shot.save(path);
		const visage::Point g = card->positionInWindow();
		MESSAGE(std::string(tag) << " card at " << g.x << "," << g.y << " " << card->width() << "x" << card->height() << " -> " << path);
	}
}
TEST_CASE("Pattern's note range in the window: Lowest and Highest show as note names while a pitched pattern is in range, and hide when only Drum or Modulation plays (backlog QT)") {
	namespace pt = feathers::mod::arp::pat;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId dev = card->device();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, dev, pt::kDescriptor);
	REQUIRE(view != nullptr);
	auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, std::max(window->minimumSize().x, 900.0f), std::max(window->minimumSize().y, 600.0f));
	const auto boxOf = [&](const char* key) -> ui::ValueBox* {
		std::vector<visage::Frame*> stack { window };
		while (!stack.empty()) {
			visage::Frame* fr = stack.back();
			stack.pop_back();
			if (auto* b = dynamic_cast<ui::ValueBox*>(fr); b != nullptr && b->control().desc().key != nullptr
			                                              && std::string(b->control().desc().key) == key) {
				return b;
			}
			for (visage::Frame* c : fr->children()) {
				stack.push_back(c);
			}
		}
		return nullptr;
	};
	ui::ValueBox* low = boxOf("note_low");
	ui::ValueBox* high = boxOf("note_high");
	REQUIRE(low != nullptr);
	REQUIRE(high != nullptr);
	const auto settle = [&] {
		(void)window->updateContent();
		window->resized();
	};
	settle();
	CHECK(low->isVisible()); // Melody, the default
	CHECK(high->isVisible());
	CHECK(low->control().text() == "B-1"); // MIDI 23, in the plugin's note names (60 is C3)
	CHECK(high->control().text() == "G8");  // MIDI 127
	for (const pt::Mode m : { pt::Mode::Drum, pt::Mode::Modulation }) {
		CAPTURE(static_cast<int>(m));
		f.deviceHost.setField(dev, pt::at(0, pt::Field::Mode), static_cast<float>(m));
		settle();
		CHECK_FALSE(low->isVisible());
		CHECK_FALSE(high->isVisible());
	}
	f.deviceHost.setField(dev, pt::at(0, pt::Field::Mode), static_cast<float>(pt::Mode::Chords));
	settle();
	CHECK(low->isVisible());
	view.reset();
}

namespace {
/// EVERY VISIBLE FRAME UNDER `root`, depth first — the cards' controls are children of children (a site's chips, a
/// pill row's pills), so a check that walked only the direct children would miss the ones that clip.
void visibleFramesUnder(visage::Frame* root, std::vector<visage::Frame*>& out) {
	for (visage::Frame* child : root->children()) {
		if (child != nullptr && child->isVisible()) {
			out.push_back(child);
			visibleFramesUnder(child, out);
		}
	}
}
} // namespace

TEST_CASE("every fx-lane card at its default fits its labels and keeps its controls off its outputs strip") {
	// **THREE CARDS SHIPPED WITH THESE FAULTS AND A PHOTOGRAPH FOUND THEM** (the README's card mosaic, 2026-11-09): Noise's
	// Seed row lay under its output chips, Random Trigger's three-way choices ran their labels together
	// ("eguliPoissCluster"), and Shaper's six pills cut their captions to fragments ("eedbac"). Every geometry case each
	// card had asked about its own rows; none asked whether a label fits the box it was given, or whether the body ends
	// where the strip the lane hangs the output chips on begins. Asked here of every card the lane can hold, at the
	// width the lane gives it, from the measure `draw` itself uses.
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> tags;
	for (const feathers::device::Descriptor* d : probeLane->addableForTest()) {
		tags.emplace_back(d->traits.tag);
	}
	REQUIRE(tags.size() >= 20);
	size_t segmented = 0;
	size_t pills = 0;
	size_t stripped = 0;
	for (const std::string& tag : tags) {
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(tag.c_str());
		REQUIRE(slot < feathers::kMaxFxSlots);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		(void)card->update();
		lane->resized();
		const float stripTop = card->ownFrame().outputs.y();
		const bool hasStrip = card->ownFrame().outputs.height() > 0.0f;
		stripped += hasStrip ? 1u : 0u;
		std::vector<visage::Frame*> frames;
		visibleFramesUnder(card, frames);
		for (visage::Frame* frame : frames) {
			if (auto* seg = dynamic_cast<ui::SegmentedControl*>(frame)) {
				CAPTURE(seg->width());
				CHECK(seg->labelsFit());
				++segmented;
			}
			if (auto* pill = dynamic_cast<ui::FaderPill*>(frame)) {
				CAPTURE(pill->caption());
				CAPTURE(pill->width());
				CHECK(pill->captionFits());
				++pills;
			}
		}
		// THE BODY ENDS WHERE THE STRIP BEGINS: no direct child of the card reaches into it.
		if (hasStrip) {
			for (visage::Frame* child : card->children()) {
				if (child == nullptr || !child->isVisible() || child->height() <= 0 || child->width() <= 0) {
					continue;
				}
				CAPTURE(child->y());
				CAPTURE(child->height());
				CAPTURE(stripTop);
				// A frame covering the whole card (a veil, an overlay) is not a control in the body.
				if (child->y() <= 0.5f && child->height() >= card->height() - 0.5f) {
					continue;
				}
				CHECK(static_cast<float>(child->bottom()) <= stripTop + 0.5f);
			}
		}
	}
	// NOT VACUOUS: the sweep met segmented controls, pills, and cards with an outputs strip.
	CHECK(segmented > 10);
	CHECK(pills > 6);
	CHECK(stripped > 1);
}
TEST_CASE("the Rack card grows with its chains, so Mix and Gain stay on the card") {
	// **THE CARD COMPUTED THE RIGHT HEIGHT AND NOBODY ASKED FOR IT AGAIN.** `bodyHeight()` counts a row per chain, but
	// the lane asks a card its extent only when the lane itself is laid out — a fold, an add, a resize. Chains arrive
	// through the running body, which is none of those, so a rack given three chains kept the height it had with one
	// and drew its Mix and Gain knobs off its own foot (the README's card mosaic, 2026-11-09).
	ModFixture f;
	RackOnLane r(f);
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	lane->scrollCardIntoView(r.slot);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	REQUIRE(card != nullptr);
	(void)card->update();
	const float before = card->height();
	REQUIRE(card->stripsForTest().size() == 1);
	while (r.rack().chainCount() < 3) {
		(void)r.rack().addChain();
	}
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 3);
	// THE VALUE: the card is the height it says it needs, at the width it has.
	const float wanted = card->laneExtent(static_cast<float>(card->width()));
	CAPTURE(before);
	CAPTURE(wanted);
	CHECK(static_cast<float>(card->height()) == doctest::Approx(wanted));
	// NOT VACUOUS: three chains need more than one did.
	CHECK(wanted > before + 1.0f);
	// AND EVERY VISIBLE CONTROL — the strips and the Mix and Gain knobs under them — is inside the card.
	for (visage::Frame* child : card->children()) {
		if (child != nullptr && child->isVisible()) {
			CAPTURE(child->y());
			CAPTURE(child->height());
			CHECK(static_cast<float>(child->bottom()) <= static_cast<float>(card->height()) + 0.5f);
		}
	}
}
TEST_CASE("every modulator-strip card at its default fits its labels") {
	// THE STRIP'S TWIN OF THE LANE SWEEP ABOVE, at the strip's own (narrower) card width — Random Trigger lives in both.
	ModFixture probe;
	REQUIRE(probe.panel != nullptr);
	const size_t offered = probe.panel->addableForTest().size();
	REQUIRE(offered >= 10);
	size_t segmented = 0;
	for (size_t choice = 0; choice < offered; ++choice) {
		ModFixture f;
		const auto* d = f.panel->addableForTest()[choice];
		CAPTURE(std::string(d->traits.tag));
		const size_t slot = f.panel->addChoiceForTest(choice);
		REQUIRE(slot < feathers::kMaxRackSlots);
		visage::Frame* card = const_cast<visage::Frame*>(f.panel->cardFrameForTest(slot));
		REQUIRE(card != nullptr);
		std::vector<visage::Frame*> frames;
		visibleFramesUnder(card, frames);
		for (visage::Frame* frame : frames) {
			if (auto* seg = dynamic_cast<ui::SegmentedControl*>(frame)) {
				CAPTURE(seg->width());
				CHECK(seg->labelsFit());
				++segmented;
			}
			if (auto* pill = dynamic_cast<ui::FaderPill*>(frame)) {
				CAPTURE(pill->caption());
				CHECK(pill->captionFits());
			}
		}
	}
	CHECK(segmented > 5);
}
#ifndef FEATHERS_TEST_SANITISED
#define FEATHERS_ALLOC_PROBE 1
namespace alloc_probe {
/// THIS THREAD'S ALLOCATIONS WHILE ARMED. The global operator new below counts through it; it measures nothing
/// until a case arms it, and a thread's own counter cannot be moved by another thread's allocation.
thread_local bool armed = false;
thread_local size_t count = 0;
inline void* take(std::size_t n) {
	if (armed) {
		++count;
	}
	return std::malloc(n != 0 ? n : 1);
}
} // namespace alloc_probe

// EVERY PLAIN, ARRAY AND NOTHROW FORM, with its delete (sized and not), so nothing is mismatched. The over-aligned forms
// are left to the library: they pair with each other and never reach these.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
	if (void* p = alloc_probe::take(n)) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
	if (void* p = alloc_probe::take(n)) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return alloc_probe::take(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return alloc_probe::take(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

namespace {
/// THE ALLOCATIONS `work` makes on this thread; always 0 when the probe is compiled out (`FEATHERS_ALLOC_PROBE`), which
/// is why every assertion on it sits under that guard rather than passing on the zero.
template <typename F> size_t allocationsIn(F&& work) {
#ifdef FEATHERS_ALLOC_PROBE
	alloc_probe::count = 0;
	alloc_probe::armed = true;
	work();
	alloc_probe::armed = false;
	return alloc_probe::count;
#else
	work();
	return 0;
#endif
}
} // namespace

TEST_CASE("hardening: the pattern lane's, the terrain's and the EQ graph's polls at rest allocate nothing and report unchanged") {
	namespace pt = feathers::mod::arp::pat;
	{
		ModFixture f;
		const size_t slot = f.bridge.addRackDevice(pt::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
		ui::WidgetRegistry registry;
		auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pt::kDescriptor);
		auto* window = dynamic_cast<ui::PatternExpanded*>(view.get());
		REQUIRE(window != nullptr);
		ui::EditHistory history;
		window->setModulation(nullptr, &history);
		window->setBounds(0.0f, 0.0f, 900.0f, 1400.0f);
		(void)window->updateContent();
		ui::PatternLane& lane = window->lane();
		ui::PatternTerrain& terrain = window->terrain();
		// SETTLED: the first polls build the view and size the buffers.
		for (int i = 0; i < 4; ++i) {
			(void)lane.poll();
			(void)terrain.poll();
		}
		int moved = 0;
		const size_t lanes = allocationsIn([&] {
			for (int i = 0; i < 50; ++i) {
				moved += lane.poll() ? 1 : 0;
			}
		});
		const size_t terrains = allocationsIn([&] {
			for (int i = 0; i < 50; ++i) {
				moved += terrain.poll() ? 1 : 0;
			}
		});
		CHECK(moved == 0);
#ifdef FEATHERS_ALLOC_PROBE
		CHECK(lanes == 0);
		CHECK(terrains == 0);
#else
		(void)lanes;
		(void)terrains;
		MESSAGE("allocation counts NOT MEASURED in this build (a sanitiser owns the allocator); the unchanged-report was checked");
#endif
		// AND THE SEAM IS NOT BLIND: a real change is heard, by the same polls.
		f.deviceHost.setField(id, pt::at(0, pt::Field::TransposeSteps), 3.0f);
		const bool laneHeard = lane.poll(); // both polled: `|` was the same without short-circuiting, and newer clang flags it
		const bool terrainHeard = terrain.poll();
		CHECK((laneHeard || terrainHeard));
	}
	{
		namespace eq = feathers::fx::eq;
		DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
		ui::EqGraph* g = d.card->graphForTest();
		g->setBounds(0.0f, 0.0f, 400.0f, 200.0f);
		// THE ANALYSER OFF, so the poll is the field comparison alone.
		d.set(eq::extraIndex(eq::Extra::Pre), 1.0f);
		d.set(eq::extraIndex(eq::Extra::Post), 1.0f);
		for (int i = 0; i < 4; ++i) {
			(void)g->update();
		}
		// THE PLUGIN HOST'S OWN READING-COUNT CALL COPIES THE CHAIN (not the graph's): that is the floor a poll cannot go
		// below, so the graph's own share is what is left over.
		const size_t hostFloor = allocationsIn([&] { (void)d.f.deviceHost.readingCountAt(d.device); });
		bool changed = false;
		const size_t allocations = allocationsIn([&] {
			for (int i = 0; i < 50; ++i) {
				changed = g->update() || changed;
			}
		});
		CHECK_FALSE(changed);
#ifdef FEATHERS_ALLOC_PROBE
		CHECK(allocations <= 50 * hostFloor);
#else
		(void)allocations;
		(void)hostFloor;
		MESSAGE("EQ allocation count NOT MEASURED in this build; the unchanged-report was checked");
#endif
		d.set(eq::paramIndex(eq::Param::Tilt), 2.0f);
		CHECK(g->update());
	}
}
TEST_CASE("hardening: the route list builds its rows once per hit test, and the geometry is the one the per-row calls gave") {
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	f.arm(backplane::Source::core(0));
	REQUIRE(mod->armed());
	// TWO ROUTES, from the one armed source.
	for (const ParamId p : { ParamId::Path, ParamId::FineTune }) {
		ui::Knob* knob = f.knobFor(p);
		REQUIRE(knob != nullptr);
		const int destination = modDestinationIndexOf(p);
		REQUIRE(destination >= 0);
		const visage::Point face = f.faceCentre(knob);
		dragThroughRoot(f.root, face, { face.x, face.y - 35.0f });
		REQUIRE(f.bridge.routeAmount(backplane::Source::core(0), backplane::Destination::core(static_cast<size_t>(destination))) > 0.0f);
	}
	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	f.root->resized();
	const size_t rowCount = list->allRows().size();
	REQUIRE(rowCount >= 2);

	// THE WORK OF ONE HIT TEST IS ONE BUILD, however many rows there are.
	const visage::Point aim { 5.0f, list->rowBounds(rowCount - 1).y() + 2.0f };
	const size_t before = list->rowBuildsForTest();
	(void)list->hitAt(aim);
	CHECK(list->rowBuildsForTest() - before == 1);

	// AND THE GEOMETRY IS UNCHANGED: each row under the one before it by one row height, the amount and chip inside it.
	const size_t first = list->firstRow();
	for (size_t i = first; i < rowCount; ++i) {
		CAPTURE(i);
		const visage::Bounds row = list->rowBounds(i);
		const visage::Bounds again = list->rowBoundsFrom(i, first);
		CHECK(again.x() == row.x());
		CHECK(again.y() == row.y());
		CHECK(again.width() == row.width());
		CHECK(again.height() == row.height());
		if (i > first) {
			CHECK(row.y() - list->rowBounds(i - 1).y() == doctest::Approx(row.height()));
		}
		CHECK(list->amountBoundsFrom(i, first).x() == list->amountBounds(i).x());
		CHECK(list->chipBoundsFrom(i, first).x() == list->chipBounds(i).x());
		CHECK(list->chipBounds(i).y() >= row.y());
		CHECK(list->chipBounds(i).bottom() <= row.bottom());
	}
}
TEST_CASE("hardening: a card drag follows its device, not its slot, and a lost release cancels") {
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	REQUIRE(f.bridge.addFxDevice(fx::harm::kTraits.tag) == 2);
	lane->resized();
	REQUIRE(lane->deviceCount() == 3);
	const DeviceInstanceId filter = f.bridge.fxDeviceAt(0);
	const DeviceInstanceId owltt = f.bridge.fxDeviceAt(1);
	const DeviceInstanceId harm = f.bridge.fxDeviceAt(2);
	const auto dragTo = [&](size_t slot, size_t gap) {
		lane->resized();
		const visage::Bounds card = lane->cardBounds(slot);
		const visage::Bounds bumper = lane->gapBounds(gap);
		const float inLaneY = std::min(bumper.y() + bumper.height() * 0.5f, static_cast<float>(lane->height()) - 1.0f);
		lane->cardDragForTest(slot, ui::FxCard::DragPhase::Move, { 20.0f, inLaneY - card.y() });
	};

	SUBCASE("a device removed ahead of the dragged one: the dragged one moves, not whatever slid into its slot") {
		lane->cardDragForTest(1, ui::FxCard::DragPhase::Begin, { 20.0f, 4.0f });
		REQUIRE(f.bridge.removeFxDevice(0));
		lane->resized();
		REQUIRE(lane->deviceCount() == 2);
		// THE DRAGGED CARD IS NOW SLOT 0, and the lane says so.
		REQUIRE(lane->dragSlot().has_value());
		CHECK(*lane->dragSlot() == 0);
		dragTo(0, 2); // the gap after the last card
		lane->cardDragForTest(0, ui::FxCard::DragPhase::End, { 20.0f, 4.0f });
		lane->resized();
		CHECK(f.bridge.fxDeviceAt(0) == harm);
		CHECK(f.bridge.fxDeviceAt(1) == owltt);
		(void)filter;
	}

	SUBCASE("the dragged device removed: the drop moves nothing, and a tick ends the drag") {
		lane->cardDragForTest(2, ui::FxCard::DragPhase::Begin, { 20.0f, 4.0f });
		dragTo(2, 0);
		REQUIRE(f.bridge.removeFxDevice(2));
		lane->resized();
		CHECK_FALSE(lane->dragSlot().has_value());
		(void)lane->tickDrag();
		CHECK_FALSE(lane->dropGap().has_value());
		lane->cardDragForTest(0, ui::FxCard::DragPhase::End, { 20.0f, 4.0f });
		CHECK(f.bridge.fxDeviceAt(0) == filter);
		CHECK(f.bridge.fxDeviceAt(1) == owltt);
	}

	SUBCASE("a cancelled drag drops nothing") {
		lane->cardDragForTest(0, ui::FxCard::DragPhase::Begin, { 20.0f, 4.0f });
		dragTo(0, 3);
		REQUIRE(lane->dropGap().has_value());
		lane->cardDragForTest(0, ui::FxCard::DragPhase::Cancel, { 20.0f, 4.0f });
		CHECK_FALSE(lane->dragSlot().has_value());
		CHECK_FALSE(lane->dropGap().has_value());
		lane->cardDragForTest(0, ui::FxCard::DragPhase::End, { 20.0f, 4.0f });
		CHECK(f.bridge.fxDeviceAt(0) == filter);
		CHECK(f.bridge.fxDeviceAt(2) == harm);
	}
}

namespace {
//------------------------------------------------------------------------------------------------//
// OWLTT'S WINDOW, REBUILT AROUND ITS ANALYZER: per-band handles and the table under the bars.     //
//------------------------------------------------------------------------------------------------//

/// THE KEYS A BAND'S CONTROLS ARE STORED UNDER, restated here by name so a handle bound to its neighbour's control is a
/// red, not a pass: nothing below reads a key off the code it checks.
struct OwlttBandKeys {
	const char* prefix;
};
constexpr OwlttBandKeys kOwlttBands[3] = { { "low" }, { "mid" }, { "high" } };
std::string owlttKey(size_t band, const char* suffix) {
	return std::string(kOwlttBands[band].prefix) + "_" + suffix;
}
} // namespace

TEST_CASE("OWLTT's window: each band's threshold and bandwidth handles are two controls, and a drag on one leaves the other alone") {
	namespace ow = feathers::fx::owltt;
	using Handle = ui::OwlttGraph::Handle;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	REQUIRE(graph->editor());

	const auto row = [&] {
		const FxChain chain = d.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, d.slot).row.field;
	};
	const auto indexOfKey = [](const std::string& key) {
		for (size_t i = 0; i < std::size(ow::kParamDescs); ++i) {
			if (key == ow::kParamDescs[i].key) {
				return i;
			}
		}
		return std::size(ow::kParamDescs);
	};

	for (size_t band = 0; band < 3; ++band) {
		CAPTURE(band);
		// EACH HANDLE IS BOUND TO THE PARAMETER ITS POSITION SAYS, by key.
		const std::pair<Handle, std::string> bound[] = {
			{ Handle::ThresholdBar, owlttKey(band, "threshold") }, { Handle::BandwidthBar, owlttKey(band, "bandwidth") },
			{ Handle::RatioStrip, owlttKey(band, "ratio") },       { Handle::LevelStrip, owlttKey(band, "level") },
			{ Handle::DownRatio, owlttKey(band, "down_ratio") },   { Handle::UpRatio, owlttKey(band, "up_ratio") },
		};
		for (const auto& [handle, key] : bound) {
			CAPTURE(key);
			CHECK(std::string_view(graph->handleControl(band, handle).desc().key) == key);
		}
		CHECK(&graph->handleControl(band, Handle::ThresholdBar) != &graph->handleControl(band, Handle::BandwidthBar));
	}

	const visage::Bounds area = graph->barArea();
	const auto yOf = [&](float v) { return area.bottom() - v * area.height(); };

	for (size_t band = 0; band < 3; ++band) {
		CAPTURE(band);
		const size_t thresholdAt = indexOfKey(owlttKey(band, "threshold"));
		const size_t bandwidthAt = indexOfKey(owlttKey(band, "bandwidth"));
		REQUIRE(thresholdAt < std::size(ow::kParamDescs));
		REQUIRE(bandwidthAt < std::size(ow::kParamDescs));

		// THE THRESHOLD HANDLE, dragged to 70% of the way up the plot: the threshold control reads 0.7 (the global at its
		// neutral shifts nothing) and EVERY other field of the row is what it was.
		{
			const auto before = row();
			const visage::Point c = graph->ringCentre(band, Handle::ThresholdBar);
			graph->onMouseDown().callback(at(c.x, c.y));
			graph->onMouseDrag().callback(at(c.x, yOf(0.7f)));
			graph->onMouseUp().callback(at(c.x, yOf(0.7f)));
			const auto after = row();
			CHECK(after[thresholdAt] == doctest::Approx(0.7f));
			// THE TOP BAR ALSO WRITES ITS BAND'S BANDWIDTH, so the up threshold stays where it was (the engine derives it from the
			// two); nothing else in the row moves. The dedicated case below checks the held threshold by value.
			CHECK(after[bandwidthAt] != before[bandwidthAt]);
			for (size_t i = 0; i < before.size(); ++i) {
				if (i != thresholdAt && i != bandwidthAt) {
					CAPTURE(std::string(ow::kParamDescs[std::min(i, std::size(ow::kParamDescs) - 1)].key));
					CHECK(after[i] == before[i]);
				}
			}
		}
		// THE BANDWIDTH HANDLE, dragged so its bar (the UP threshold, below the threshold bar by 0.75 x the bandwidth) stands at 30%:
		// the bandwidth is (0.7 - 0.3) / 0.75, and the threshold is untouched.
		{
			const auto before = row();
			const visage::Point c = graph->ringCentre(band, Handle::BandwidthBar);
			graph->onMouseDown().callback(at(c.x, c.y));
			graph->onMouseDrag().callback(at(c.x, yOf(0.3f)));
			graph->onMouseUp().callback(at(c.x, yOf(0.3f)));
			const auto after = row();
			CHECK(after[bandwidthAt] == doctest::Approx((0.7f - 0.3f) / 0.75f).epsilon(0.001));
			for (size_t i = 0; i < before.size(); ++i) {
				if (i != bandwidthAt) {
					CAPTURE(std::string(ow::kParamDescs[std::min(i, std::size(ow::kParamDescs) - 1)].key));
					CHECK(after[i] == before[i]);
				}
			}
		}
		// THE TWO THUMBS: dragged along their track to 80%, each writes its own direction's trim and nothing else.
		for (const Handle thumb : { Handle::DownRatio, Handle::UpRatio }) {
			const bool down = thumb == Handle::DownRatio;
			const size_t at0 = indexOfKey(owlttKey(band, down ? "down_ratio" : "up_ratio"));
			REQUIRE(at0 < std::size(ow::kParamDescs));
			const auto before = row();
			const ui::OwlttGraph::Track track = graph->ratioTrack(band);
			const visage::Point c = graph->ringCentre(band, thumb);
			const float x = track.x0 + 0.8f * (track.x1 - track.x0);
			graph->onMouseDown().callback(at(c.x, c.y));
			graph->onMouseDrag().callback(at(x, c.y));
			graph->onMouseUp().callback(at(x, c.y));
			const auto after = row();
			// The band's effective ratio is 0.5 at the defaults, so the trim that lands the thumb at 0.8 is 0.8.
			CHECK(after[at0] == doctest::Approx(0.8f).epsilon(0.001));
			for (size_t i = 0; i < before.size(); ++i) {
				if (i != at0) {
					CAPTURE(std::string(ow::kParamDescs[std::min(i, std::size(ow::kParamDescs) - 1)].key));
					CHECK(after[i] == before[i]);
				}
			}
			// AND THE THUMB NOW STANDS WHERE THE VALUE IS: asked back through `ringCentre`.
			CHECK(graph->ringCentre(band, thumb).x == doctest::Approx(x).epsilon(0.002));
		}
	}
}
TEST_CASE("OWLTT's window: the table under the bars is IN, WIDTH, ATK and REL, one column per band, every box bound to its band's control and declared through ControlFace") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);

	// THE ROWS AND THEIR CAPTIONS, once at the left.
	const char* captions[] = { "IN", "OUT", "RATIO", "WIDTH", "ATK", "REL" };
	const char* suffixes[] = { "input", "level", "", "width", "attack", "release" };
	const char* tips[] = { "input level", "output level", "", "stereo width", "attack", "release" };
	for (size_t row = 0; row < ui::OwlttGraph::kTableRows; ++row) {
		CHECK(std::string(ui::OwlttGraph::tableCaption(row)) == captions[row]);
	}
	const visage::Bounds table = graph->tableArea();
	CHECK(table.height() == doctest::Approx(ui::OwlttGraph::kTableHeight));
	CHECK(table.bottom() == doctest::Approx(static_cast<float>(graph->height())));
	CHECK(graph->barArea().bottom() <= table.y() + 0.01f);

	for (size_t row = 0; row < ui::OwlttGraph::kTableRows; ++row) {
		for (size_t band = 0; band < 3; ++band) {
			CAPTURE(row);
			CAPTURE(band);
			if (row == ui::OwlttGraph::kRatioRow) {
				CHECK(graph->tableBoxForTest(row, band) == nullptr); // the read-out row holds no box
				continue;
			}
			ui::ValueBox* box = graph->tableBoxForTest(row, band);
			REQUIRE(box != nullptr);
			// BOUND TO THE PARAMETER ITS POSITION SAYS (restated by key), through ControlFace.
			std::vector<ui::ControlFace::Offered> offered;
			box->offeredControls(offered);
			REQUIRE(offered.size() == 1u);
			CHECK(std::string_view(offered[0].control->desc().key) == owlttKey(band, suffixes[row]));
			CHECK(offered[0].road == ui::ControlFace::Road::Face);
			// A TOOLTIP THAT NAMES IT, from its parameter.
			const char* tip = box->tooltipAt({ 3.0f, 3.0f });
			REQUIRE(tip != nullptr);
			CHECK(std::string(tip).find(tips[row]) != std::string::npos);
			// IT SITS IN THE TABLE, and in ITS band's column: the centre of the cell is between the crossover lines that
			// bound the band (the plot's own edges at the two ends).
			const visage::Bounds b = box->bounds();
			CHECK(b.y() >= table.y());
			CHECK(b.bottom() <= table.bottom());
			const float centre = b.x() + b.width() * 0.5f;
			const float lo = band == 0 ? 0.0f : graph->bandEdge(band - 1);
			const float hi = band == 2 ? static_cast<float>(graph->width()) : graph->bandEdge(band);
			CHECK(centre > lo);
			CHECK(centre < hi);
			// ROWS DESCEND, COLUMNS ASCEND, nothing overlaps its neighbour.
			if (row > 0) {
				const size_t above = row - 1 == ui::OwlttGraph::kRatioRow ? row - 2 : row - 1;
				CHECK(b.y() >= graph->tableBoxForTest(above, band)->bounds().bottom());
			}
			if (band > 0) {
				CHECK(b.x() >= graph->tableBoxForTest(row, band - 1)->bounds().right());
			}
		}
	}

	// THE BOXES WRITE THEIR OWN FIELD: setting one through its control moves that one field of the row.
	const auto fields = [&] {
		const FxChain chain = d.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, d.slot).row.field;
	};
	const auto before = fields();
	ui::ValueBox* attack = graph->tableBoxForTest(4, 1);
	attack->control().beginGesture();
	attack->control().setValue(0.9);
	attack->control().endGesture();
	const auto after = fields();
	size_t changed = 0;
	for (size_t i = 0; i < before.size(); ++i) {
		if (after[i] != before[i]) {
			++changed;
			CHECK(i == static_cast<size_t>(ow::Param::MidAttack));
		}
	}
	CHECK(changed == 1u);
	CHECK(after[static_cast<size_t>(ow::Param::MidAttack)] == doctest::Approx(0.9f));

	// THE COLUMNS FOLLOW THE CROSSOVERS: moving them moves the cells, and nothing overlaps.
	d.set(static_cast<size_t>(ow::Param::LowCrossover), 600.0f);
	d.set(static_cast<size_t>(ow::Param::HighCrossover), 5000.0f);
	const float midBefore = graph->tableBoxForTest(0, 1)->bounds().x();
	(void)graph->update();
	const float midAfter = graph->tableBoxForTest(0, 1)->bounds().x();
	CHECK(midAfter != doctest::Approx(midBefore));
	for (size_t row = 0; row < ui::OwlttGraph::kTableRows; ++row) {
		for (size_t band = 1; band < 3; ++band) {
			if (row == ui::OwlttGraph::kRatioRow) {
				continue;
			}
			CHECK(graph->tableBoxForTest(row, band)->bounds().x() >= graph->tableBoxForTest(row, band - 1)->bounds().right());
		}
	}
}
TEST_CASE("OWLTT's window: a routed per-band threshold draws its ModHandle stroke and halo, the crossovers have no knobs, and the rows are captioned without holes") {
	namespace ow = feathers::fx::owltt;
	using Handle = ui::OwlttGraph::Handle;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	ui::ModState* mod = d.f.gui.modForTest();
	REQUIRE(mod != nullptr);
	// THE OVERLAY GIVES A WINDOW ITS MODULATION STATE when it opens one; built bare, it is given it here.
	panel.modulationBound(mod, nullptr);

	// A ROUTE ON THE MID BAND'S THRESHOLD: nothing armed draws no halo, armed draws one, and the stroke is the handle's own.
	const backplane::Source macro = backplane::Source::core(0);
	const backplane::Destination midThreshold =
	    backplane::Destination::control(d.device, static_cast<size_t>(ow::Param::MidThreshold));
	d.f.bridge.setRouteAmount(macro, midThreshold, 0.2f);
	{
		const ui::ModHandlePlan quiet = graph->bandModPlan(1, Handle::ThresholdBar);
		REQUIRE(quiet.axes.size() == 1u);
		CHECK(quiet.axes[0].offered);
		CHECK_FALSE(quiet.halo);
	}
	mod->focus(macro);
	mod->arm(true);
	const ui::ModHandlePlan plan = graph->bandModPlan(1, Handle::ThresholdBar);
	REQUIRE(plan.axes.size() == 1u);
	CHECK(plan.halo);
	CHECK(plan.axes[0].halo);
	REQUIRE(plan.axes[0].hasStroke);
	const visage::Bounds area = graph->barArea();
	const float stored = d.f.bridge.fxChain().device[d.slot].row.field[static_cast<size_t>(ow::Param::MidThreshold)];
	CHECK(plan.axes[0].strokeHi - plan.axes[0].strokeLo
	      == doctest::Approx(0.2f * area.height()).epsilon(0.002));
	CHECK(plan.axes[0].base == doctest::Approx(area.bottom() - stored * area.height()).epsilon(0.001));
	// A THUMB IS ONE TOO: horizontal, over its own ratio trim.
	CHECK(graph->bandModHandle(1, Handle::DownRatio).axis(0).horizontal);
	CHECK_FALSE(graph->bandModHandle(1, Handle::ThresholdBar).axis(0).horizontal);
	mod->arm(false);

	// THE CROSSOVERS HAVE NO KNOBS IN THE WINDOW: no box or knob anywhere in it is bound to either, and the plot's two
	// lane handles are how they are reached.
	std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
		if (const auto* face = dynamic_cast<const ui::ControlFace*>(frame)) {
			if (frame != graph) {
				std::vector<ui::ControlFace::Offered> offered;
				face->offeredControls(offered);
				for (const auto& o : offered) {
					CHECK(std::string_view(o.control->desc().key) != "low_crossover");
					CHECK(std::string_view(o.control->desc().key) != "high_crossover");
				}
			}
		}
		for (const visage::Frame* child : frame->children()) {
			walk(child);
		}
	};
	walk(&panel);
	const auto addressed = panel.addressedParamsForTest();
	CHECK(std::count(addressed.begin(), addressed.end(), ow::Param::LowCrossover) == 1);
	CHECK(std::count(addressed.begin(), addressed.end(), ow::Param::HighCrossover) == 1);
	CHECK(graph->crossoverHandle(0).x < graph->crossoverHandle(1).x);

	// THE TOP ROW, in order: Key Track, Lookahead, L/R, the crossover type, then the Clip choice (a box with a menu of four).
	CHECK(panel.keyTrackBounds().right() <= panel.lookaheadBounds().x());
	CHECK(panel.lookaheadBounds().right() <= panel.stereoBounds().x());
	CHECK(panel.stereoBounds().right() <= panel.modeBounds().x());
	CHECK(panel.modeBounds().right() <= panel.clipBounds().x());
	CHECK(panel.clipBounds().bottom() <= panel.graphBounds().y());
	ui::ValueBox* clip = panel.clipForTest();
	REQUIRE(clip != nullptr);
	CHECK(clip->choiceMenuAvailable());
	REQUIRE(clip->entryCount() == 4u);
	// (a menu row carries a mark for the current choice, so the label is looked for in it)
	CHECK(clip->choiceLabelAt(0).find("Off") != std::string::npos);
	CHECK(clip->choiceLabelAt(1).find("Both") != std::string::npos);
	CHECK(clip->choiceLabelAt(2).find("Master") != std::string::npos);
	CHECK(clip->choiceLabelAt(3).find("Bands") != std::string::npos);
	CHECK(std::string_view(clip->control().desc().key) == "soft_clip");

	// THE GLOBAL ROW, in order and with no hole: nine boxes, left to right at one height, each gap the same.
	const auto& boxes = panel.globalBoxesForTest();
	const ow::Param order[] = { ow::Param::Threshold, ow::Param::Bandwidth, ow::Param::Ratio,   ow::Param::UpDownSkew,
		                        ow::Param::Attack,    ow::Param::Release,   ow::Param::StereoLink, ow::Param::Width,
		                        ow::Param::Blend };
	REQUIRE(boxes.size() == std::size(order));
	for (size_t i = 0; i < boxes.size(); ++i) {
		CAPTURE(i);
		CHECK(panel.paramOfGlobalForTest(boxes[i].get()) == order[i]);
		CHECK(boxes[i]->bounds().y() == doctest::Approx(boxes[0]->bounds().y()));
		if (i > 0) {
			CHECK(boxes[i]->bounds().x() - boxes[i - 1]->bounds().right() == doctest::Approx(ui::OwlttExpanded::kBoxGap).epsilon(0.001));
		}
	}
	CHECK(boxes.back()->bounds().right() == doctest::Approx(static_cast<float>(panel.width()) - ui::OwlttExpanded::kPad));
	// AND TONE: Character and Vibe side by side as one group, under the global row.
	REQUIRE(panel.modeCellsForTest().size() == 2u);
	const visage::Bounds character = panel.modeCellsForTest()[0]->bounds();
	const visage::Bounds vibe = panel.modeCellsForTest()[1]->bounds();
	CHECK(character.right() <= vibe.x());
	CHECK(character.y() == doctest::Approx(vibe.y()));
	CHECK(character.y() >= boxes[0]->bounds().bottom());
}
TEST_CASE("OWLTT's window: offsets and ratio trims read and type as signed percentages from their centre, the stored value unchanged") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	ui::ValueBox* attack = graph->tableBoxForTest(4, 0);
	REQUIRE(attack != nullptr);
	CHECK(attack->control().text() == "0"); // the default centre
	d.set(static_cast<size_t>(ow::Param::LowAttack), 0.75f);
	CHECK(attack->control().text() == "+25%");
	CHECK(attack->readout() == "+25%"); // and the box prints it so
	d.set(static_cast<size_t>(ow::Param::LowAttack), 0.1f);
	CHECK(attack->control().text() == "-40%");
	// THE THUMB'S CONTROL READS THE SAME WAY.
	d.set(static_cast<size_t>(ow::Param::MidDownRatio), 0.9f);
	CHECK(graph->handleControl(1, ui::OwlttGraph::Handle::DownRatio).text() == "+40%");
	// TYPED ENTRY TAKES THE SIGNED FORM (and a bare number, and a percent sign), as the device's own parse.
	const auto& ops = *ow::kDescriptor.text;
	device::TextView view {};
	double v = 0.0;
	for (const auto& [text, want01] : { std::pair { "+25%", 0.75 }, std::pair { "-40", 0.1 }, std::pair { "0", 0.5 },
	                                    std::pair { "-80%", 0.0 } }) {
		CAPTURE(text);
		REQUIRE(ops.parse(static_cast<size_t>(ow::Param::HighRelease), text, view, &v));
		CHECK(v == doctest::Approx(want01).epsilon(1e-9));
	}
	CHECK_FALSE(ops.parse(static_cast<size_t>(ow::Param::HighRelease), "abc", view, &v));
	// THE GLOBAL ROW IS LEFT AS IT WAS: Attack still reads as a percentage of its range.
	CHECK(panel.globalBoxesForTest()[4]->control().text() == "25%");
}
TEST_CASE("OWLTT: the combined-reduction readout has a place of its own — no strip header, no crossover label, no clip square, at every crossover position, in the window and on the card") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	const auto overlap = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
	};
	// A NON-ZERO REDUCTION IS PUBLISHED, so the readout is the one the player sees while signal passes (it draws only above 0.1 dB).
	const std::array<float, 3> gr { -0.5f, -0.25f, -0.125f };
	d.f.bridge.publishOwlttMeters(d.slot, gr, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, false, { false, false, false }, 3.0f);
	REQUIRE(d.card->graph()->meters().combinedGrDb == doctest::Approx(3.0f));
	REQUIRE(panel.graph()->meters().combinedGrDb == doctest::Approx(3.0f));

	struct Scene {
		const char* name;
		float low;
		float high;
	};
	const Scene scenes[] = { { "defaults", 200.0f, 2000.0f }, { "high at the bottom", 200.0f, 300.0f },
		                     { "high at the top", 200.0f, 20000.0f }, { "both high", 1800.0f, 20000.0f },
		                     { "close", 800.0f, 900.0f } };
	size_t headersChecked = 0;
	for (const Scene& s : scenes) {
		d.set(static_cast<size_t>(ow::Param::LowCrossover), s.low);
		d.set(static_cast<size_t>(ow::Param::HighCrossover), s.high);
		for (ui::OwlttGraph* graph : { d.card->graph(), panel.graph() }) {
			CAPTURE(s.name);
			CAPTURE(graph == panel.graph());
			const visage::Bounds readout = graph->readoutBounds();
			REQUIRE(readout.width() > 0.0f);
			CHECK(readout.x() >= 0.0f);
			CHECK(readout.right() <= graph->width());
			const ui::OwlttGraph::BottomText t = graph->bottomText();
			for (const visage::Bounds& label : t.frequency) {
				CHECK_FALSE(overlap(readout, label));
			}
			if (graph->editor()) {
				for (size_t band = 0; band < 3; ++band) {
					for (size_t which = 0; which < 2; ++which) {
						CAPTURE(band);
						CAPTURE(which);
						CHECK_FALSE(overlap(readout, graph->stripHeader(band, which)));
						++headersChecked;
					}
				}
			}
		}
	}
	CHECK(headersChecked == 5u * 6u); // not vacuous: the headers were there to be avoided
}

TEST_CASE("OWLTT Comb: the plot claims no frequency range: columns named for their classes, the spacings where the corners were, two handles") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	const auto overlap = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
	};
	const std::array<float, 3> gr { -0.5f, -0.25f, -0.125f };
	d.f.bridge.publishOwlttMeters(d.slot, gr, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, false, { false, false, false }, 3.0f);
	// NOT VACUOUS: in an ordinary mode the corners read as frequencies and no column is headed by a class.
	d.set(static_cast<size_t>(ow::Param::Mode), 8.0f);
	for (ui::OwlttGraph* graph : { d.card->graph(), panel.graph() }) {
		const ui::OwlttGraph::BottomText t = graph->bottomText();
		CHECK(t.frequencyText[0] == "200 Hz");
		CHECK(t.frequencyText[1] == "2.0 kHz");
		for (const std::string& c : t.classText) {
			CHECK(c.empty());
		}
	}
	d.set(static_cast<size_t>(ow::Param::Mode), static_cast<float>(ow::kCombMode));
	d.set(static_cast<size_t>(ow::Param::LowCrossover), 100.0f);
	// THE HIGH CORNER OFF ITS DEFAULT: at 200 Hz and 2 kHz the corners fall on exact thirds of the 20 Hz .. 20 kHz axis, which would hide a column edge that still claimed one.
	d.set(static_cast<size_t>(ow::Param::HighCrossover), 5000.0f);
	// 100 and 1000 Hz: ten times, so the on-teeth band is cut at a whole ratio, and named so.
	const std::array<const char*, 3> classes { "Between", "Others", "Every 10" };
	for (ui::OwlttGraph* graph : { d.card->graph(), panel.graph() }) {
		CAPTURE(graph == panel.graph());
		const ui::OwlttGraph::BottomText t = graph->bottomText();
		// The one readout is the fundamental where the first corner's was; the second corner says nothing.
		CHECK(t.frequencyText[0] == "f1 100 Hz");
		CHECK(t.frequencyText[1] == "f2 1.0 kHz"); // High at 5000 reads as a fifth of it, in Hz
		// Each column is headed by its class, centred in a third of the plot.
		const visage::Bounds whole = graph->bounds();
		for (size_t b = 0; b < 3; ++b) {
			CAPTURE(b);
			CHECK(t.classText[b] == classes[b]);
			const float centre = t.classLabel[b].x() + t.classLabel[b].width() * 0.5f;
			CHECK(centre == doctest::Approx(whole.width() * (static_cast<float>(b) + 0.5f) / 3.0f).epsilon(0.01));
			CHECK_FALSE(overlap(t.classLabel[b], t.frequency[0]));
			CHECK_FALSE(overlap(t.classLabel[b], graph->readoutBounds()));
		}
	}
	// THE LANE'S FIRST HANDLE IS f0, on its own log scale (an octave is the same distance anywhere), and the second is out of reach.
	ui::OwlttGraph* lane = panel.graph();
	std::array<float, 3> x {};
	size_t at = 0;
	for (const float hz : { 200.0f, 400.0f, 800.0f }) {
		d.set(static_cast<size_t>(ow::Param::LowCrossover), hz);
		const visage::Point h = lane->crossoverHandle(0);
		CHECK(lane->bottomText().frequencyText[0] == "f1 " + ui::OwlttGraph::frequencyText(hz));
		x[at++] = h.x;
		CHECK(lane->hitTestThreshold(h) == ui::OwlttGraph::ThresholdHit::kLowCrossover);
	}
	CHECK(x[1] > x[0]);
	CHECK((x[1] - x[0]) == doctest::Approx(x[2] - x[1]).epsilon(0.01));
	// THE SECOND HANDLE IS THE SECOND SPACING, on the same spacing scale: an octave is the same distance there too, and the defaults (200 and 400 Hz) are an octave apart.
	std::array<float, 3> y {};
	at = 0;
	d.set(static_cast<size_t>(ow::Param::LowCrossover), 20.0f); // out of the way: the handles share one scale
	for (const float knob : { 1000.0f, 2000.0f, 4000.0f }) {
		d.set(static_cast<size_t>(ow::Param::HighCrossover), knob);
		const visage::Point h = lane->crossoverHandle(1);
		CHECK(lane->hitTestThreshold(h) == ui::OwlttGraph::ThresholdHit::kHighCrossover);
		y[at++] = h.x;
	}
	CHECK(y[1] > y[0]);
	CHECK((y[1] - y[0]) == doctest::Approx(y[2] - y[1]).epsilon(0.01));
}

TEST_CASE("OWLTT Comb: Res Home is in the window's header right of the Mode box, only in a Comb mode, and writes its parameter") {
	namespace ow = feathers::fx::owltt;
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(ow::kTraits.tag) == 1);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, id };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::ValueBox* box = panel.resHomeForTest();
	REQUIRE(box != nullptr);
	const size_t mode = static_cast<size_t>(ow::Param::Mode);
	// HIDDEN in every other mode: an engine choice could be the only thing to enable it, so it is not drawn rather than greyed.
	for (const float other : { 1.0f, 6.0f, 11.0f, 14.0f }) {
		f.deviceHost.setField(id, mode, other);
		(void)panel.update();
		CAPTURE(other);
		CHECK_FALSE(box->isVisible());
	}
	const float clipBefore = panel.clipBounds().x();
	for (const long comb : { ow::kCombMode, ow::kCombSharpMode }) {
		f.deviceHost.setField(id, mode, static_cast<float>(comb));
		(void)panel.update();
		CAPTURE(comb);
		CHECK(box->isVisible());
		// RIGHT OF THE MODE BOX, on its row, with Clip moved along rather than overlapped.
		const visage::Bounds m = panel.modeBounds();
		CHECK(box->x() >= m.right());
		CHECK(box->x() - m.right() < 16.0f);
		CHECK(box->width() > 40.0f);
		CHECK(panel.clipBounds().x() >= box->x() + box->width());
		CHECK(panel.clipBounds().x() > clipBefore);
	}
	// THE BOX WRITES RES HOME (it is the control's own face).
	ui::DeviceControl c { f.deviceHost, id, ow::kDescriptor, static_cast<size_t>(ow::Param::ResHome) };
	c.setValue(0.4);
	CHECK(f.deviceHost.fieldAt(id, static_cast<size_t>(ow::Param::ResHome)) == doctest::Approx(0.4f));
	// BACK OUT OF COMB, it goes and Clip returns.
	f.deviceHost.setField(id, mode, 6.0f);
	(void)panel.update();
	CHECK_FALSE(box->isVisible());
	CHECK(panel.clipBounds().x() == clipBefore);
	CHECK(ow::kParamDescs[static_cast<size_t>(ow::Param::ResHome)].modulatable);
}

TEST_CASE("OWLTT Comb: each column's curve peaks at its band's harmonics, sharpens with Res Home and follows the second spacing; the three colours differ") {
	namespace ow = feathers::fx::owltt;
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(ow::kTraits.tag) == 1);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, id };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	f.deviceHost.setField(id, static_cast<size_t>(ow::Param::Mode), static_cast<float>(ow::kCombMode));
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	constexpr size_t kPoints = 641; // a step of 0.0125 in harmonic number over 0 .. 8
	const auto peaks = [&](size_t band) {
		const std::vector<float> c = graph->combCurve(band, kPoints);
		std::vector<double> at;
		for (size_t i = 1; i + 1 < c.size(); ++i) {
			if (c[i] > 0.75f && c[i] >= c[i - 1] && c[i] > c[i + 1]) {
				at.push_back(ui::OwlttGraph::kCombCurveSpan * static_cast<double>(i) / static_cast<double>(kPoints - 1));
			}
		}
		return at;
	};
	const auto expectPeaks = [&](size_t band, double first, double step, size_t count) {
		const std::vector<double> got = peaks(band);
		CAPTURE(band);
		REQUIRE(got.size() == count);
		for (size_t k = 0; k < count; ++k) {
			CHECK(got[k] == doctest::Approx(first + step * static_cast<double>(k)).epsilon(0.01).scale(1.0));
		}
	};
	const size_t high = static_cast<size_t>(ow::Param::HighCrossover);
	const size_t res = static_cast<size_t>(ow::Param::ResHome);
	for (const float home : { 0.0f, 0.8f }) {
		CAPTURE(home);
		f.deviceHost.setField(id, res, home);
		// THE DEFAULTS (x1, x2): Between on the half harmonics, Odd on 1 3 5 ..., Even on 2 4 6 ... (the edges of the span are not interior peaks).
		expectPeaks(0, 0.5, 1.0, 8);
		expectPeaks(1, 1.0, 2.0, 4);
		expectPeaks(2, 2.0, 2.0, 3);
	}
	// A SECOND SPACING OF x3: High keeps every third harmonic, so its peaks are 3 and 6.
	f.deviceHost.setField(id, res, 0.0f);
	f.deviceHost.setField(id, high, 3000.0f);
	expectPeaks(2, 3.0, 3.0, 2);
	f.deviceHost.setField(id, high, 2000.0f);
	// RES HOME NARROWS THE TEETH: the width at half height of Odd's first peak falls as it rises.
	const auto halfWidth = [&](float home) {
		f.deviceHost.setField(id, res, home);
		const std::vector<float> c = graph->combCurve(1, kPoints);
		const size_t centre = 80; // u = 1
		const float half = c[centre] * 0.5f;
		size_t a = centre;
		while (a > 0 && c[a] > half) {
			--a;
		}
		size_t b = centre;
		while (b + 1 < c.size() && c[b] > half) {
			++b;
		}
		return b - a;
	};
	CHECK(halfWidth(0.9f) < halfWidth(0.0f));
	CHECK(halfWidth(0.5f) < halfWidth(0.0f));
	// THREE COLOURS, apart from each other and from the orange and blue the bars use for down and up.
	const auto distance = [](uint32_t a, uint32_t b) {
		const auto channel = [](uint32_t c, int shift) { return static_cast<double>((c >> shift) & 0xff); };
		double sum = 0.0;
		for (const int shift : { 0, 8, 16 }) {
			sum += (channel(a, shift) - channel(b, shift)) * (channel(a, shift) - channel(b, shift));
		}
		return std::sqrt(sum);
	};
	const std::array<uint32_t, 5> all { ui::OwlttGraph::combBandColour(0), ui::OwlttGraph::combBandColour(1), ui::OwlttGraph::combBandColour(2),
		                                feathers::tokens::kValue, feathers::tokens::kAccent };
	for (size_t i = 0; i < 3; ++i) {
		for (size_t j = i + 1; j < all.size(); ++j) {
			CAPTURE(i);
			CAPTURE(j);
			CHECK(distance(all[i], all[j]) > 60.0);
		}
	}
}

TEST_CASE("OWLTT Comb: the three curves are overlaid across the full plot width, each owning its harmonics, in its column's colour; frequency modes draw none") {
	namespace ow = feathers::fx::owltt;
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(ow::kTraits.tag) == 1);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, id };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	const size_t mode = static_cast<size_t>(ow::Param::Mode);
	// A FREQUENCY MODE DRAWS NO COMB CURVES.
	f.deviceHost.setField(id, mode, 6.0f);
	CHECK(graph->combLayers(641).empty());
	f.deviceHost.setField(id, mode, static_cast<float>(ow::kCombMode));
	constexpr size_t kPoints = 641;
	const std::vector<ui::OwlttGraph::CombLayer> layers = graph->combLayers(kPoints);
	REQUIRE(layers.size() == 3);
	const visage::Bounds plot = graph->plot();
	// RECONSTRUCT THE DRAWN CURVE: sample at the x of harmonic number u, y read back to a response by the plot's own pad (an independent restatement).
	const auto response = [&](const ui::OwlttGraph::CombLayer& l, double u) {
		const float x = graph->combCurveX(u);
		const float t = (x - l.x0) / (l.x1 - l.x0);
		const auto i = static_cast<size_t>(std::lround(t * static_cast<float>(kPoints - 1)));
		const float yTop = l.y[i];
		const visage::Bounds area = graph->barAreaForTest();
		return (area.bottom() - ui::OwlttGraph::kCombBottomPad - yTop) / (area.height() - ui::OwlttGraph::kCombTopMargin - ui::OwlttGraph::kCombBottomPad);
	};
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		// FULL WIDTH: the curve starts and ends at the bar area's edges (not a third of it), which fill the plot's width.
		const visage::Bounds area = graph->barAreaForTest();
		CHECK(layers[b].band == b);
		CHECK(layers[b].x0 == doctest::Approx(area.x()));
		CHECK(layers[b].x1 == doctest::Approx(area.right()));
		CHECK((layers[b].x1 - layers[b].x0) > 0.8f * plot.width());
		CHECK(layers[b].colour == ui::OwlttGraph::combBandColour(b));
	}
	CHECK(layers[0].colour != layers[1].colour);
	CHECK(layers[1].colour != layers[2].colour);
	// AT HARMONIC k (defaults) the owner peaks and the others are low; Between owns the half harmonics.
	for (int k = 1; k <= 7; ++k) {
		CAPTURE(k);
		const size_t owner = k % 2 == 1 ? 1 : 2;
		const size_t other = owner == 1 ? 2 : 1;
		CHECK(response(layers[owner], k) > 0.9f);
		CHECK(response(layers[other], k) < 0.2f);
		CHECK(response(layers[0], k) < 0.2f);
		CHECK(response(layers[0], k + 0.5) > 0.9f);
		CHECK(response(layers[1], k + 0.5) < 0.5f);
		CHECK(response(layers[2], k + 0.5) < 0.5f);
	}
}

TEST_CASE("OWLTT HP Wet: the shared element is in the window's tone row in every mode, the semitone knob replaces the hertz one under Key Track, and Sub Thru is inactive without a filter") {
	namespace ow = feathers::fx::owltt;
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(ow::kTraits.tag) == 1);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, id };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::WetHighpassControl* hp = panel.hpForTest();
	REQUIRE(hp != nullptr);
	REQUIRE(hp->hertz() != nullptr);
	REQUIRE(hp->semitones() != nullptr);
	REQUIRE(hp->subThru() != nullptr);
	const size_t mode = static_cast<size_t>(ow::Param::Mode);
	const size_t hpWet = static_cast<size_t>(ow::Param::HpWet);
	const size_t semis = static_cast<size_t>(ow::Param::HpWetSemis);
	for (const float m : { 1.0f, 8.0f, 11.0f, 15.0f, 16.0f }) {
		f.deviceHost.setField(id, mode, m);
		(void)panel.update();
		CAPTURE(m);
		// IN THE TONE ROW, to the right of its two cells, in every mode.
		CHECK(hp->hertz()->isVisible());
		CHECK_FALSE(hp->semitones()->isVisible());
		const visage::Bounds row = panel.toneRowForTest();
		CHECK(hp->hertz()->x() >= row.x() + row.width() * 0.5f);
		CHECK(hp->hertz()->right() <= hp->subThru()->x());
		CHECK(hp->subThru()->right() <= row.right() + 0.5f);
		for (const auto& cell : panel.modeCellsForTest()) {
			CHECK(cell->right() <= hp->hertz()->x() + 0.5f);
		}
		// SUB THRU HAS NOTHING TO GIVE BACK WITHOUT A FILTER.
		CHECK_FALSE(hp->subThru()->enabled());
	}
	f.deviceHost.setField(id, hpWet, static_cast<float>(std::log2(500.0)));
	(void)panel.update();
	CHECK(hp->subThru()->enabled());
	// KEY TRACK: the semitone knob takes the hertz one's place, and the filter's presence is the semitones' (their bottom is none).
	f.deviceHost.setField(id, static_cast<size_t>(ow::Param::KeyTrack), 1.0f);
	f.deviceHost.setField(id, semis, static_cast<float>(ow::kHpWetSemisMin));
	(void)panel.update();
	CHECK_FALSE(hp->hertz()->isVisible());
	CHECK(hp->semitones()->isVisible());
	CHECK_FALSE(hp->subThru()->enabled());
	f.deviceHost.setField(id, semis, 12.0f);
	(void)panel.update();
	CHECK(hp->subThru()->enabled());
	f.deviceHost.setField(id, static_cast<size_t>(ow::Param::KeyTrack), 0.0f);
	(void)panel.update();
	CHECK(hp->hertz()->isVisible());
	CHECK_FALSE(hp->semitones()->isVisible());
	CHECK(ow::kParamDescs[hpWet].modulatable);
	CHECK(ow::kParamDescs[semis].modulatable);
}

TEST_CASE("OWLTT Comb: a handle drag writes its own knob, a tracked drag lands on the ratio detents, and an untracked one does not") {
	namespace ow = feathers::fx::owltt;
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(ow::kTraits.tag) == 1);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, id };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	const size_t low = static_cast<size_t>(ow::Param::LowCrossover);
	const size_t high = static_cast<size_t>(ow::Param::HighCrossover);
	f.deviceHost.setField(id, static_cast<size_t>(ow::Param::Mode), static_cast<float>(ow::kCombMode));
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	const std::array<float, 3> zero { 0.0f, 0.0f, 0.0f };
	const auto publishNote = [&](float hz) {
		f.bridge.publishOwlttMeters(1, zero, zero, zero, false, { false, false, false }, 0.0f, nullptr, hz);
		(void)panel.update();
	};
	// THE LANE'S SCALE, RESTATED: the spacing a knob means, on one log scale of 20 Hz .. 4 kHz, a ring radius in from each end of the plot.
	const auto laneX = [&](double knob, bool isHigh) {
		const visage::Bounds box = graph->plot();
		const float inset = ui::OwlttGraph::kRingRadius;
		const double t = std::log2(ow::combSpacingHz(knob, isHigh) / 20.0) / std::log2(200.0);
		return box.x() + inset + static_cast<float>(t) * (box.width() - 2.0f * inset);
	};
	const auto drag = [&](ui::OwlttGraph::ThresholdHit which, float toX) {
		const visage::Point h = graph->crossoverHandle(which == ui::OwlttGraph::ThresholdHit::kLowCrossover ? 0 : 1);
		graph->onMouseDown().callback(at(h.x, h.y));
		graph->onMouseDrag().callback(at(toX, h.y));
		graph->onMouseUp().callback(at(toX, h.y));
	};
	// TRACKING A NOTE: a drag to a hair off x2 (knob 408) lands on 400 exactly; off a detent (x1.4: 280) it stays where it was put.
	f.deviceHost.setField(id, static_cast<size_t>(ow::Param::KeyTrack), 1.0f);
	publishNote(220.0f);
	drag(ui::OwlttGraph::ThresholdHit::kLowCrossover, laneX(408.0, false));
	CHECK(f.deviceHost.fieldAt(id, low) == doctest::Approx(400.0f).epsilon(1e-4));
	CHECK(f.deviceHost.fieldAt(id, high) == doctest::Approx(2000.0f)); // the other knob untouched
	drag(ui::OwlttGraph::ThresholdHit::kLowCrossover, laneX(280.0, false));
	CHECK(f.deviceHost.fieldAt(id, low) == doctest::Approx(280.0f).epsilon(0.01));
	// THE HIGH HANDLE: x3 is a knob of 3000.
	drag(ui::OwlttGraph::ThresholdHit::kHighCrossover, laneX(3050.0, true));
	CHECK(f.deviceHost.fieldAt(id, high) == doctest::Approx(3000.0f).epsilon(1e-4));
	CHECK(graph->bottomText().frequencyText[1] == "f2 x3");
	// NOT TRACKING: no note, the same drag is a plain Hz value, no snapping.
	f.deviceHost.setField(id, static_cast<size_t>(ow::Param::KeyTrack), 0.0f);
	publishNote(0.0f);
	drag(ui::OwlttGraph::ThresholdHit::kLowCrossover, laneX(408.0, false));
	CHECK(f.deviceHost.fieldAt(id, low) == doctest::Approx(408.0f).epsilon(0.01));
}

namespace {
//------------------------------------------------------------------------------------------------//
// OWLTT'S MODULATION DISPLAY: the glow arrives with the arm, the live dot follows a moving source,  //
// and the amount drag runs along the handle's own axis.                                           //
//------------------------------------------------------------------------------------------------//

/// An OWLTT with its window OPEN over the editor, the way a hand opens it: the card's plot and the window's, both live.
struct OwlttOpenRig {
	DynCardFixture<ui::OwlttCard> d { feathers::fx::owltt::kTraits.tag };
	ui::OwlttExpanded* panel = nullptr;
	ui::ModState* mod = nullptr;

	OwlttOpenRig() {
		mod = d.f.gui.modForTest();
		REQUIRE(mod != nullptr);
		REQUIRE(d.lane->onExpandDevice != nullptr);
		d.lane->onExpandDevice(d.slot);
		panel = findInTree<ui::OwlttExpanded>(d.f.root);
		REQUIRE(panel != nullptr);
		REQUIRE(panel->graph() != nullptr);
	}
	size_t indexOf(const device::Modulatable& c) const {
		for (size_t i = 0; i < std::size(feathers::fx::owltt::kParamDescs); ++i) {
			if (std::string_view(c.desc().key) == feathers::fx::owltt::kParamDescs[i].key) {
				return i;
			}
		}
		return std::size(feathers::fx::owltt::kParamDescs);
	}
};
} // namespace

TEST_CASE("OWLTT: arming and disarming a source repaints the plot and every assignable face with no mouse event") {
	OwlttOpenRig rig;
	auto& d = rig.d;
	ui::OwlttGraph* cardGraph = d.card->graph();
	ui::OwlttGraph* windowGraph = rig.panel->graph();
	std::vector<visage::Frame*> frames { cardGraph, windowGraph, rig.panel->driveForTest(), rig.panel->outputForTest() };
	for (const auto& box : rig.panel->globalBoxesForTest()) {
		frames.push_back(box.get());
	}
	for (size_t row = 0; row < ui::OwlttGraph::kTableRows; ++row) {
		for (size_t band = 0; band < 3; ++band) {
			if (row == ui::OwlttGraph::kRatioRow) {
				continue;
			}
			frames.push_back(windowGraph->tableBoxForTest(row, band));
		}
	}
	for (visage::Frame* frame : frames) {
		REQUIRE(frame != nullptr);
		CAPTURE(frame->width());
		// THE ROAD THAT DELIVERS: every one is on the modulation state's watch list, which `arm` notifies.
		CHECK(rig.mod->watches(frame));
	}
	std::vector<std::unique_ptr<RedrawProbe>> probes;
	for (visage::Frame* frame : frames) {
		probes.push_back(std::make_unique<RedrawProbe>(frame));
	}
	const auto drainCounts = [&] {
		size_t told = 0;
		for (auto& probe : probes) {
			told += probe->count > 0 ? 1 : 0;
		}
		return told;
	};
	// ARMED WITH NO MOUSE EVENT: the graph and each face asked for a repaint.
	rig.mod->focus(backplane::Source::core(0));
	rig.mod->arm(true);
	(void)d.f.gui.tickForTest();
	for (size_t i = 0; i < frames.size(); ++i) {
		CAPTURE(i);
		CHECK(probes[i]->count > 0);
	}
	CHECK(drainCounts() == frames.size());
	for (auto& probe : probes) {
		probe->count = 0;
		probe->rearm();
	}
	// AND DISARMED: told again.
	rig.mod->arm(false);
	(void)d.f.gui.tickForTest();
	for (size_t i = 0; i < frames.size(); ++i) {
		CAPTURE(i);
		CHECK(probes[i]->count > 0);
	}
}
TEST_CASE("OWLTT: every plot handle draws the live modulated position on its own axis, the handle stays at the base, and a quiet plot never repaints") {
	namespace ow = feathers::fx::owltt;
	using Handle = ui::OwlttGraph::Handle;
	OwlttOpenRig rig;
	auto& d = rig.d;
	ui::OwlttGraph* graph = rig.panel->graph();
	const backplane::Source macro = backplane::Source::core(0);
	rig.mod->focus(macro);
	rig.mod->arm(true);
	const visage::Bounds area = graph->barArea();
	const visage::Bounds plot = graph->plot();
	const auto yOf = [&](double v) { return area.bottom() - static_cast<float>(std::clamp(v, 0.0, 1.0)) * area.height(); };
	// THE LOG FREQUENCY AXIS, restated: 20 Hz to 20 kHz across the plot.
	const auto xOfHz = [&](double hz) { return plot.x() + plot.width() * static_cast<float>(std::log2(hz / 20.0) / std::log2(1000.0)); };

	struct Case {
		size_t band;
		Handle handle;
		bool horizontal;
	};
	size_t checked = 0;
	for (size_t band = 0; band < 3; ++band) {
		for (const Handle h : { Handle::ThresholdBar, Handle::BandwidthBar, Handle::RatioStrip, Handle::LevelStrip, Handle::DownRatio,
		                        Handle::UpRatio }) {
			device::Modulatable& control = graph->handleControl(band, h);
			const size_t idx = rig.indexOf(control);
			const bool horizontal = h == Handle::DownRatio || h == Handle::UpRatio;
			const backplane::Destination dest = backplane::Destination::control(d.device, idx);
			d.f.bridge.setRouteAmount(macro, dest, 0.2f);
			const double base = control.value();
			const ui::ModHandle& mh = graph->bandModHandle(band, h);
			CHECK(mh.axis(0).horizontal == horizontal);
			for (const double offset : { 0.1, -0.1 }) {
				CAPTURE(band);
				CAPTURE(static_cast<int>(h));
				CAPTURE(offset);
				d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(base + offset));
				const ui::ModHandlePlan plan = graph->bandModPlan(band, h);
				REQUIRE(plan.axes[0].hasDot);
				const double live = base + offset;
				float want = 0.0f;
				// THE EXPECTED COORDINATE, restated from the plot's own mapping at the defaults (every global at neutral).
				switch (h) {
				case Handle::ThresholdBar:
				case Handle::RatioStrip:
				case Handle::LevelStrip: want = yOf(live); break;
				case Handle::BandwidthBar: want = yOf(std::max(0.0, 0.5 - 0.75 * live)); break; // threshold 0.5 at the default
				case Handle::DownRatio:
				case Handle::UpRatio: {
					const ui::OwlttGraph::Track t = graph->ratioTrack(band);
					want = t.x0 + static_cast<float>(std::clamp(0.5 + live - 0.5, 0.0, 1.0)) * (t.x1 - t.x0);
					break;
				}
				}
				CHECK(plan.axes[0].dot == doctest::Approx(want).epsilon(1e-3));
				// THE HANDLE ITSELF STAYS AT THE BASE.
				const visage::Point c = graph->ringCentre(band, h);
				CHECK((horizontal ? c.x : c.y) == doctest::Approx(plan.axes[0].base).epsilon(0.01));
				++checked;
			}
			d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(base));
			d.f.bridge.setRouteAmount(macro, dest, 0.0f);
		}
	}
	// BOTH CROSSOVERS, positive and negative, in hertz on the log axis.
	for (size_t which = 0; which < 2; ++which) {
		const size_t idx = static_cast<size_t>(which == 0 ? ow::Param::LowCrossover : ow::Param::HighCrossover);
		const backplane::Destination dest = backplane::Destination::control(d.device, idx);
		d.f.bridge.setRouteAmount(macro, dest, 0.05f);
		const double base = graph->crossoverModHandle(which).axis(0).control->value();
		for (const double factor : { 1.3, 0.7 }) {
			d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(base * factor));
			const ui::ModHandlePlan plan = graph->crossoverModPlan(which);
			REQUIRE(plan.axes[0].hasDot);
			CHECK(plan.axes[0].dot == doctest::Approx(xOfHz(base * factor)).epsilon(1e-3));
			CHECK(graph->crossoverHandle(which).x == doctest::Approx(xOfHz(base)).epsilon(1e-3));
			++checked;
		}
		d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(base));
		d.f.bridge.setRouteAmount(macro, dest, 0.0f);
	}
	CHECK(checked == 3u * 6u * 2u + 2u * 2u);

	// A MOVING ROUTED SOURCE REPAINTS THE PLOT; A STILL ONE, AND A PLOT WITH NOTHING ROUTED, DO NOT.
	(void)graph->update(); // settle: the first read takes the baseline
	(void)graph->update();
	RedrawProbe probe(graph);
	CHECK_FALSE(graph->update());
	CHECK(probe.count == 0);
	const size_t mid = static_cast<size_t>(ow::Param::MidThreshold);
	d.f.bridge.setRouteAmount(macro, backplane::Destination::control(d.device, mid), 0.2f);
	d.f.bridge.setFxModulatedForTest(d.slot, mid, 0.62f);
	CHECK(graph->update());
	CHECK(probe.count > 0);
	probe.count = 0;
	probe.rearm();
	CHECK_FALSE(graph->update()); // held where it is: no repaint
	CHECK(probe.count == 0);
	d.f.bridge.setFxModulatedForTest(d.slot, mid, 0.66f);
	CHECK(graph->update()); // moved again: told again
}
TEST_CASE("OWLTT: an amount drag runs along the handle's own axis — across for crossovers and thumbs, up and down for rings and strips — and toward higher values raises it") {
	namespace ow = feathers::fx::owltt;
	using Handle = ui::OwlttGraph::Handle;
	OwlttOpenRig rig;
	auto& d = rig.d;
	ui::OwlttGraph* graph = rig.panel->graph();
	const backplane::Source macro = backplane::Source::core(0);
	rig.mod->focus(macro);
	rig.mod->arm(true);
	constexpr float kPx = 14.0f;
	const float step = kPx / ui::Knob::kRouteDragRange;
	const float seed = ui::Knob::kRouteSeedAmount;

	struct Target {
		visage::Point at;
		size_t index;
		bool horizontal;
		std::string name;
	};
	std::vector<Target> targets;
	for (size_t band = 0; band < 3; ++band) {
		for (const Handle h : { Handle::ThresholdBar, Handle::BandwidthBar, Handle::RatioStrip, Handle::LevelStrip, Handle::DownRatio,
		                        Handle::UpRatio }) {
			targets.push_back({ graph->ringCentre(band, h), rig.indexOf(graph->handleControl(band, h)),
			                    h == Handle::DownRatio || h == Handle::UpRatio,
			                    std::string(graph->handleControl(band, h).desc().key) });
		}
	}
	for (size_t which = 0; which < 2; ++which) {
		targets.push_back({ graph->crossoverHandle(which), rig.indexOf(*graph->crossoverModHandle(which).axis(0).control), true,
		                    which == 0 ? "low_crossover" : "high_crossover" });
	}
	const auto amountOf = [&](size_t index) {
		return d.f.bridge.routeAmount(macro, backplane::Destination::control(d.device, index));
	};
	for (const Target& t : targets) {
		CAPTURE(t.name);
		const backplane::Destination dest = backplane::Destination::control(d.device, t.index);
		const auto press = [&](float dx, float dy) {
			d.f.bridge.setRouteAmount(macro, dest, 0.0f);
			graph->onMouseDown().callback(at(t.at.x, t.at.y));
			graph->onMouseDrag().callback(at(t.at.x + dx, t.at.y + dy));
			graph->onMouseUp().callback(at(t.at.x + dx, t.at.y + dy));
			const float got = amountOf(t.index);
			d.f.bridge.setRouteAmount(macro, dest, 0.0f);
			return got;
		};
		if (t.horizontal) {
			CHECK(press(kPx, 0.0f) == doctest::Approx(seed + step).epsilon(0.001));   // toward higher values (right) raises it
			CHECK(press(-kPx, 0.0f) == doctest::Approx(seed - step).epsilon(0.001));
			CHECK(press(0.0f, kPx) == doctest::Approx(seed).epsilon(0.001));          // across the axis: nothing
			CHECK(press(0.0f, -kPx) == doctest::Approx(seed).epsilon(0.001));
		}
		else {
			// UP IS HIGHER for a threshold, a ratio and a level; for the BANDWIDTH ring it is down (a wider bandwidth puts the
			// up bar lower), and "toward higher values raises it" follows the control, not the screen.
			const float toward = t.name.find("bandwidth") != std::string::npos ? kPx : -kPx;
			CHECK(press(0.0f, toward) == doctest::Approx(seed + step).epsilon(0.001));
			CHECK(press(0.0f, -toward) == doctest::Approx(seed - step).epsilon(0.001));
			CHECK(press(kPx, 0.0f) == doctest::Approx(seed).epsilon(0.001));
			CHECK(press(-kPx, 0.0f) == doctest::Approx(seed).epsilon(0.001));
		}
	}
	CHECK(targets.size() == 20u);
}
TEST_CASE("OWLTT: dragging a band's top (threshold) handle holds its up threshold still, stops at it, takes it along past the bandwidth's range, and is one undo step") {
	namespace ow = feathers::fx::owltt;
	using Handle = ui::OwlttGraph::Handle;
	OwlttOpenRig rig;
	auto& d = rig.d;
	ui::OwlttGraph* graph = rig.panel->graph();
	constexpr size_t kBand = 1;
	const size_t thrAt = static_cast<size_t>(ow::Param::MidThreshold);
	const size_t bwAt = static_cast<size_t>(ow::Param::MidBandwidth);
	const visage::Bounds area = graph->barArea();
	const auto yOf = [&](float v) { return area.bottom() - v * area.height(); };
	const auto fieldOf = [&](size_t i) {
		const FxChain chain = d.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, d.slot).row.field[i];
	};
	// WHAT THE COMPRESSOR HOLDS: the stored row run through the engine, read back through its own seams (knob units).
	struct Thresholds {
		float down;
		float up;
	};
	const auto engineThresholds = [&] {
		ow::Row row {};
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = fieldOf(i);
		}
		ow::Settings s = ow::stateOf(row);
		ow::Owltt dsp;
		dsp.prepare(48000.0);
		dsp.setTarget(s);
		std::vector<float> l(256, 0.1f), r(256, 0.1f);
		dsp.processStereo(l.data(), r.data(), 256);
		return Thresholds { dsp.thresholdDownForTest(kBand), dsp.thresholdUpForTest(kBand) };
	};
	const auto dragTopTo = [&](float target) {
		const visage::Point c = graph->ringCentre(kBand, Handle::ThresholdBar);
		graph->onMouseDown().callback(at(c.x, c.y));
		graph->onMouseDrag().callback(at(c.x, yOf(target)));
		graph->onMouseUp().callback(at(c.x, yOf(target)));
	};

	const Thresholds start = engineThresholds();
	CHECK(start.down == doctest::Approx(0.5f));
	CHECK(start.up == doctest::Approx(0.125f)); // 0.5 - 0.75 x 0.5, the engine's own gap
	// SEVERAL SIZES, BOTH WAYS: the down threshold lands on the pointer and the up threshold has not moved.
	for (const float target : { 0.8f, 0.6f, 0.4f, 0.2f, 0.7f, 0.3f }) {
		CAPTURE(target);
		dragTopTo(target);
		const Thresholds now = engineThresholds();
		CHECK(now.down == doctest::Approx(target).epsilon(1e-3));
		CHECK(now.up == doctest::Approx(start.up).epsilon(1e-3));
	}
	// THE LOW LIMIT: below the up threshold the bar stops there, the bandwidth reaching zero.
	dragTopTo(0.02f);
	{
		const Thresholds now = engineThresholds();
		CHECK(now.down == doctest::Approx(start.up).epsilon(1e-3));
		CHECK(now.up == doctest::Approx(start.up).epsilon(1e-3));
		CHECK(fieldOf(bwAt) == doctest::Approx(0.0f).epsilon(1e-3));
	}
	// THE HIGH LIMIT: past what the bandwidth can span (1.0 -> 0.75 below), the bandwidth sits at its maximum and the up threshold FOLLOWS.
	dragTopTo(0.5f);
	dragTopTo(0.97f);
	{
		const Thresholds now = engineThresholds();
		CHECK(fieldOf(bwAt) == doctest::Approx(1.0f));
		CHECK(now.down == doctest::Approx(0.97f).epsilon(1e-3));
		// the engine's own relation, restated: up = down - 0.6 x 1 on the internal scale, in knob units
		const float downInternal = 0.2f + 0.8f * 0.97f;
		CHECK(now.up == doctest::Approx((std::max(0.2f, downInternal - 0.6f) - 0.2f) / 0.8f).epsilon(1e-3));
		CHECK(now.up > start.up);
	}

	// ONE GESTURE, ONE UNDO: from a fresh stroke, a drag leaves one step and undoing it restores BOTH parameters.
	d.set(thrAt, 0.5f);
	d.set(bwAt, 0.5f);
	REQUIRE(d.f.gui.history() != nullptr);
	while (d.f.gui.history()->canUndo()) {
		d.f.gui.history()->undo();
	}
	d.set(thrAt, 0.5f);
	d.set(bwAt, 0.5f);
	dragTopTo(0.8f);
	CHECK(fieldOf(thrAt) == doctest::Approx(0.8f));
	CHECK(fieldOf(bwAt) != doctest::Approx(0.5f));
	REQUIRE(d.f.gui.history()->canUndo());
	d.f.gui.history()->undo();
	CHECK_FALSE(d.f.gui.history()->canUndo()); // the drag was ONE step, not two
	CHECK(fieldOf(thrAt) == doctest::Approx(0.5f));
	CHECK(fieldOf(bwAt) == doctest::Approx(0.5f));

	// A DOUBLE-CLICK RESET KEEPS ITS SINGLE-PARAMETER MEANING: only the threshold returns to its default.
	d.set(thrAt, 0.8f);
	d.set(bwAt, 0.3f);
	{
		const visage::Point c = graph->ringCentre(kBand, Handle::ThresholdBar);
		graph->onMouseDown().callback(doubleAt(c.x, c.y));
		graph->onMouseUp().callback(doubleAt(c.x, c.y));
	}
	CHECK(fieldOf(thrAt) == doctest::Approx(0.5f));
	CHECK(fieldOf(bwAt) == doctest::Approx(0.3f));
}
TEST_CASE("OWLTT: a routed crossover's amount stroke and live dot never lie under its label, and a full-scale source puts the dot exactly at the stroke's end — on the log axis and on a linear one") {
	namespace ow = feathers::fx::owltt;
	using Handle = ui::OwlttGraph::Handle;
	OwlttOpenRig rig;
	auto& d = rig.d;
	ui::OwlttGraph* graph = rig.panel->graph();
	const backplane::Source macro = backplane::Source::core(0);
	rig.mod->focus(macro);
	rig.mod->arm(true);
	const auto overlap = [](const visage::Bounds& a, const visage::Bounds& b) {
		return a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
	};

	for (size_t which = 0; which < 2; ++which) {
		const size_t idx = static_cast<size_t>(which == 0 ? ow::Param::LowCrossover : ow::Param::HighCrossover);
		const backplane::Destination dest = backplane::Destination::control(d.device, idx);
		const device::ParamDesc& desc = ow::kParamDescs[idx];
		const double span = device::routeSpanOf(desc);
		for (const float amount : { 0.05f, -0.05f }) {
			CAPTURE(which);
			CAPTURE(amount);
			d.f.bridge.setRouteAmount(macro, dest, amount);
			const double base = graph->crossoverModHandle(which).axis(0).control->value();
			// A FULL-SCALE SOURCE: the modulated value is the base plus the whole amount (clamped to the range), which is the end of
			// the stroke by definition; the dot is drawn at the same VALUE's coordinate, through the log axis, never a linear pixel scale.
			const double full = std::clamp(base + static_cast<double>(amount) * span, desc.min, desc.max);
			d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(full));
			const ui::ModHandlePlan plan = graph->crossoverModPlan(which);
			REQUIRE(plan.axes[0].hasStroke);
			REQUIRE(plan.axes[0].hasDot);
			const float end = amount > 0.0f ? plan.axes[0].strokeHi : plan.axes[0].strokeLo;
			CHECK(plan.axes[0].dot == doctest::Approx(end).epsilon(1e-4));
			// A HALF-SCALE SOURCE is NOT at the end on the log axis: the dot sits at the half-VALUE's coordinate, inside the stroke.
			d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(base + 0.5 * (full - base)));
			const float half = graph->crossoverModPlan(which).axes[0].dot;
			CHECK(half > std::min(plan.axes[0].strokeLo, plan.axes[0].strokeHi) - 0.01f);
			CHECK(half < std::max(plan.axes[0].strokeLo, plan.axes[0].strokeHi) + 0.01f);
			CHECK(half != doctest::Approx(end).epsilon(1e-4));
			d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(full));

			// THE LABEL KEEPS OFF BOTH.
			const ui::OwlttGraph::BottomText t = graph->bottomText();
			const visage::Bounds lane = graph->rail();
			const visage::Bounds strokeBox { plan.axes[0].strokeLo, lane.y(), plan.axes[0].strokeHi - plan.axes[0].strokeLo, lane.height() };
			const visage::Bounds dotBox { plan.axes[0].dot - 4.0f, lane.y(), 8.0f, lane.height() };
			CHECK_FALSE(overlap(t.frequency[which], strokeBox));
			CHECK_FALSE(overlap(t.frequency[which], dotBox));
			d.f.bridge.setFxModulatedForTest(d.slot, idx, static_cast<float>(base));
		}
		d.f.bridge.setRouteAmount(macro, dest, 0.0f);
	}

	// A LINEAR AXIS, THE SAME RULE: a band's threshold ring (vertical, height = value).
	const size_t thr = static_cast<size_t>(ow::Param::MidThreshold);
	const backplane::Destination dest = backplane::Destination::control(d.device, thr);
	const device::ParamDesc& desc = ow::kParamDescs[thr];
	for (const float amount : { 0.3f, -0.3f }) {
		CAPTURE(amount);
		d.f.bridge.setRouteAmount(macro, dest, amount);
		const double base = graph->handleControl(1, Handle::ThresholdBar).value();
		const double full = std::clamp(base + static_cast<double>(amount) * device::routeSpanOf(desc), desc.min, desc.max);
		d.f.bridge.setFxModulatedForTest(d.slot, thr, static_cast<float>(full));
		const ui::ModHandlePlan plan = graph->bandModPlan(1, Handle::ThresholdBar);
		REQUIRE(plan.axes[0].hasStroke);
		REQUIRE(plan.axes[0].hasDot);
		// On the vertical axis a larger value is a SMALLER y: a positive amount ends at strokeLo.
		const float end = amount > 0.0f ? plan.axes[0].strokeLo : plan.axes[0].strokeHi;
		CHECK(plan.axes[0].dot == doctest::Approx(end).epsilon(1e-4));
		d.f.bridge.setFxModulatedForTest(d.slot, thr, static_cast<float>(base));
	}
}
TEST_CASE("the Pattern window's lane: a row's header offers 'Lock row' / 'Unlock row' before anything is applied, locking holds the row's steps while the place moves, and one undo step undoes it — pitched rows and Drum lanes") {
	using namespace lane_check;
	using Kind = pn::PatternStep::Kind;
	namespace genes = feathers::mod::arp::genes;
	for (const genes::Mode mode : { genes::Mode::Melody, genes::Mode::Drum, genes::Mode::Modulation }) {
		CAPTURE(static_cast<int>(mode));
		Window w;
		w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::Mode), static_cast<float>(mode));
		(void)w.window->updateContent();
		ui::PatternLane& lane = w.lane();
		(void)lane.poll();
		const bool drum = genes::decodesDrum(mode);
		REQUIRE(lane.cycleForTest().length == 16);
		// THE ROW: the lane (or pitch) with the most hits.
		const pn::PatternCycle before = lane.cycleForTest();
		int id = -1000;
		size_t most = 0;
		const auto hits = [&](const pn::PatternCycle& c, int r) {
			std::vector<bool> on(c.length, false);
			for (size_t k = 0; k < c.length; ++k) {
				const pn::PatternStep& st = c.steps[k];
				on[k] = drum ? ((st.drumHits >> r) & 1u) != 0u : (st.kind == Kind::Note && c.keyAt(k, 0) - c.rootKey == r);
			}
			return on;
		};
		for (int r = drum ? 0 : -36; r < (drum ? 8 : 36); ++r) {
			const std::vector<bool> on = hits(before, r);
			const auto n = static_cast<size_t>(std::count(on.begin(), on.end(), true));
			if (n > most) {
				most = n;
				id = r;
			}
		}
		REQUIRE(id != -1000);
		// THE HEADER: a point in the gutter at the row finds it; outside the gutter finds none.
		const float y = drum ? lane.yOfLane(static_cast<size_t>(id)) + 1.0f : lane.yOfKey(before.rootKey + id) + 2.0f;
		const visage::Point header { lane.gutterWidth() * 0.5f, y };
		CHECK(lane.rowAt(header) == id);
		CHECK(lane.rowAt({ lane.xOfStep(2) + 2.0f, y }) == ui::PatternLane::kNoRow);
		// THE MENU NAMES THE VERB BEFORE IT IS APPLIED: one option, "Lock row", and nothing is locked yet.
		const auto options = lane.rowMenuOptions(id);
		REQUIRE(options.size() == 1);
		CHECK(options[0].second == "Lock row");
		CHECK_FALSE(lane.rowLocked(id));
		const std::vector<float> noLocks = [&] {
			std::vector<float> b(pt::kRowLockWords, 0.0f);
			(void)w.f.deviceHost.blockAt(w.id, pt::kRowLockBlock, b);
			return b;
		}();
		CHECK(w.history.length() == 0);
		// APPLIED: locked, the mark's state is the lane's, the verb flips, ONE undo step.
		REQUIRE(lane.setRowLocked(id, true));
		CHECK(lane.rowLocked(id));
		CHECK(lane.rowMenuOptions(id)[0].second == "Unlock row");
		CHECK(w.history.length() == 1);
		const std::vector<bool> held = hits(lane.cycleForTest(), id);
		CHECK(held == hits(before, id));
		// THE PLACE MOVES: the locked row is as it was; the cycle as a whole is not.
		w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::Zone), 777.0f);
		(void)w.window->updateContent();
		(void)lane.poll();
		CHECK(hits(lane.cycleForTest(), id) == held);
		CHECK(lane.rowLocked(id));
		bool otherMoved = false;
		for (int r = drum ? 0 : -36; r < (drum ? 8 : 36); ++r) {
			otherMoved = otherMoved || (r != id && hits(lane.cycleForTest(), r) != hits(before, r));
		}
		CHECK(otherMoved);
		// UNDO: the row follows the terrain again (one step), redo locks it again.
		w.history.undo();
		(void)lane.poll();
		CHECK_FALSE(lane.rowLocked(id));
		CHECK(w.history.length() == 1);
		w.history.redo();
		(void)lane.poll();
		CHECK(lane.rowLocked(id));
		CHECK(hits(lane.cycleForTest(), id) == held);
		// UNLOCK: one more step, back to the decode of the place now.
		REQUIRE(lane.setRowLocked(id, false));
		CHECK_FALSE(lane.rowLocked(id));
		CHECK(w.history.length() == 2);
		CHECK_FALSE(lane.setRowLocked(id, false)); // not locked: nothing written, no step
		CHECK(w.history.length() == 2);
		CHECK(hits(lane.cycleForTest(), id) != held);
		// A TAB'S RESET TAKES ITS LOCKS WITH IT; A COPY BRINGS THEM.
		REQUIRE(lane.setRowLocked(id, true));
		ui::choosePatternTabMenu(w.f.deviceHost, w.id, 0, 1, &w.history); // copy tab 1 to tab 2
		std::vector<float> block(pt::kRowLockWords, 0.0f);
		REQUIRE(w.f.deviceHost.blockAt(w.id, pt::kRowLockBlock, block));
		const pt::RowLockState copied = pt::rowLockStateOf(block.data(), block.size());
		CHECK(copied[1].any());
		CHECK(copied[1] == copied[0]);
		ui::choosePatternTabMenu(w.f.deviceHost, w.id, 0, ui::kPatternMenuReset, &w.history);
		REQUIRE(w.f.deviceHost.blockAt(w.id, pt::kRowLockBlock, block));
		CHECK_FALSE(pt::rowLockStateOf(block.data(), block.size())[0].any());
		CHECK(pt::rowLockStateOf(block.data(), block.size())[1].any());
		w.history.undo();
		REQUIRE(w.f.deviceHost.blockAt(w.id, pt::kRowLockBlock, block));
		CHECK(pt::rowLockStateOf(block.data(), block.size())[0].any());
	}
}
TEST_CASE("the Pattern card's lane draws a locked row's mark but offers no menu: only the window's lane locks, and its header tooltip says how") {
	using namespace lane_check;
	Window w;
	ui::PatternLane& lane = w.lane();
	(void)lane.poll();
	const pn::PatternCycle c = lane.cycleForTest();
	size_t k = 0;
	while (k < 16 && c.steps[k].kind != pn::PatternStep::Kind::Note) {
		++k;
	}
	REQUIRE(k < 16);
	const int id = c.keyAt(k, 0) - c.rootKey;
	REQUIRE(lane.setRowLocked(id, true));
	// THE CARD'S LANE: a view over the same device.
	ui::PatternLane card(w.f.deviceHost, w.id);
	card.setBounds(0.0f, 0.0f, 400.0f, 120.0f);
	card.setFocus(0);
	(void)card.poll();
	CHECK(card.rowLocked(id));
	CHECK(card.locksForTest().any());
	const size_t locks = w.history.length();
	card.onMouseDown().callback([&] {
		visage::MouseEvent e = at(card.gutterWidth() * 0.5f, 60.0f);
		e.button_id = visage::kMouseButtonRight;
		e.button_state = visage::kMouseButtonRight;
		return e;
	}());
	CHECK(w.history.length() == locks); // a view: nothing happens
	CHECK(card.tooltipAt({ 4.0f, 60.0f }) == nullptr);
	// THE WINDOW'S: the header's tooltip is the lock's, and says what still acts on a locked row.
	const std::string tip = lane.tooltipAt({ lane.gutterWidth() * 0.5f, lane.yOfKey(c.rootKey + id) + 2.0f });
	CHECK(tip == ui::PatternLane::kRowGestures);
	for (const char* named : { "Right-click", "header", "Transpose", "Wobble", "edit" }) {
		CAPTURE(named);
		CHECK(tip.find(named) != std::string::npos);
	}
}

