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

// PIECE 13 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

//------------------------------------------------------------------------------------------------//
// THE SAME FOUR, ON THE MODULATOR STRIP ("all of this applies to MOD devices").   //
// No modulator declares a window today (`hasExpanded` is false for every mod device), so the       //
// strip's half is: the selection colour, the two window roads bound like the lane's, and the       //
// honesty that a modulator with no window offers no expand chip, folded or not.                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("window header: a selected MODULATOR card's outline is its instance's window colour, and the strip "
          "binds the lane's two window roads") {
	ModFixture f;
	REQUIRE(f.panel != nullptr);
	REQUIRE(f.bridge.addRackDevice(feathers::mod::lfo::kTraits.tag) < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	REQUIRE(f.panel->deviceCount() >= 2);

	std::vector<uint32_t> seen;
	for (size_t slot = 0; slot < f.panel->deviceCount(); ++slot) {
		CAPTURE(slot);
		auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		const feathers::DeviceInstanceId id = card->device();
		REQUIRE(id != feathers::kNoInstance);
		CAPTURE(std::string(card->descriptor()->traits.name));

		// SELECTED BY A PRESS, the strip's own road (`onCardPressed` → `selectSlot`).
		REQUIRE(card->onCardPressed != nullptr);
		card->onCardPressed();
		REQUIRE(card->headFocused());
		const ui::CardFrameStyle style = card->frameStyle();
		CHECK(style.outline);
		// RESTATED FROM THE PALETTE: the colour a window for this instance would be outlined in.
		const uint32_t expected =
		    ui::withAlpha(ui::kInstanceTints[(id - 1) % ui::kInstanceTints.size()], ui::kPanelEdgeTintAlpha);
		CHECK(style.outlineColor == expected);
		CHECK(style.outlineColor == ui::instanceEdgeColor(ui::instanceTintFor(id, card->nest())));
		CHECK(style.outlineColor != ui::kAccent);
		seen.push_back(style.outlineColor);

		// THE ROADS ARE BOUND, card to strip to editor — so the day a modulator declares a window it
		// opens from the chip and comes forward from the head with nothing more wired.
		CHECK(card->onExpand != nullptr);
		CHECK(card->onHeadPressed != nullptr);

		// AND THE HONESTY HALF: no window behind a modulator, so no expand chip, folded or open.
		CHECK_FALSE(card->offersExpand());
		CHECK(card->expandBounds().width() == 0.0f);
	}
	REQUIRE(seen.size() >= 2);
	CHECK(seen[0] != seen[1]);
	CHECK(f.panel->onExpandDevice != nullptr);
	CHECK(f.panel->onFocusDeviceWindow != nullptr);
}
TEST_CASE("window header: the strip's focus road raises the device's open window, and a modulator's head press "
          "with no window of its own moves nothing") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	const size_t a = f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag);
	const size_t b = f.bridge.addFxDevice(feathers::fx::harm::kTraits.tag);
	lane->resized();
	lane->onExpandDevice(a);
	lane->onExpandDevice(b);
	REQUIRE(overlay->panelCount() == 2);
	const feathers::DeviceInstanceId idA = f.bridge.fxDeviceAt(a);
	const feathers::DeviceInstanceId idB = f.bridge.fxDeviceAt(b);
	REQUIRE(frontDevice(*overlay) == idB);

	// THE STRIP'S BINDING, called as its cards call it: by id and chain. A raises.
	REQUIRE(f.panel->onFocusDeviceWindow != nullptr);
	CHECK(f.panel->onFocusDeviceWindow(idA, {}));
	CHECK(frontDevice(*overlay) == idA);

	// A MODULATOR'S HEAD, PRESSED THROUGH ITS OWN MOUSE ROAD: it has no window, so nothing moves —
	// and in particular it raises no OTHER device's window (ids are one space across both strips).
	auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(f.panel->cardFrameForTest(0)));
	REQUIRE(card != nullptr);
	const std::optional<visage::Point> head = bareHeadPoint(*card);
	REQUIRE(head.has_value());
	pressCard(*card, *head);
	CHECK(overlay->panelCount() == 2);
	CHECK(frontDevice(*overlay) == idA);
	CHECK_FALSE(f.panel->onFocusDeviceWindow(card->device(), card->nest()));
}
TEST_CASE("RESONATE's presets: the shared browser in the panel's title, its own bank, a save and load by value, the bypass kept") {
	// RESONATE takes the fx lanes and presets. The browser SKLITTER's and Featherverb's panels hold, over
	// `Devices/RESONATE`; a load is the whole sound EXCEPT the bypass (`device_preset.h`).
	const ScopedPresetLibrary library("resonate-presets");
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	engine.prepare(48000.0, 4096);
	feathers::rack::RackBridge bridge(engine);
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(rsn::kTraits.tag);
	REQUIRE(id != kNoInstance);
	bridge.drain();
	bridge.publish(engine);
	ui::WidgetRegistry registry;
	ui::ResonateExpanded panel(bridge, registry, id);
	const visage::Point min = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, min.x, min.y);

	ui::PresetBrowser& browser = panel.presetBrowserForTest();
	CHECK(panel.titleAccessory() == &browser);
	CHECK(browser.bank() == devicePresetBank(rsn::kDescriptor));
	CHECK(browser.bank() == "Devices/RESONATE");
	REQUIRE(browser.indexOf("Init") >= 0);

	const auto jawari = static_cast<size_t>(rsn::Param::Jawari);
	const auto chord = static_cast<size_t>(rsn::Param::DroneChord);
	const auto enabled = static_cast<size_t>(rsn::Param::Enabled);
	bridge.setField(id, jawari, 0.7f);
	bridge.setField(id, chord, 8.0f); // Tanpura
	REQUIRE(browser.saveAs("Drones/Mine"));
	CHECK(std::filesystem::exists(library.path() / "Devices" / "RESONATE" / "Drones" / "Mine.fpreset"));

	// AWAY, AND BYPASSED; THEN LOAD: the sound comes back, the bypass stays where the session put it.
	bridge.setField(id, jawari, 0.1f);
	bridge.setField(id, chord, 0.0f);
	bridge.setField(id, enabled, 0.0f);
	const int mine = browser.indexOf("Mine", "Drones");
	REQUIRE(mine >= 0);
	REQUIRE(browser.selectIndex(mine));
	CHECK(bridge.fieldAt(id, jawari) == 0.7f);
	CHECK(bridge.fieldAt(id, chord) == 8.0f);
	CHECK(bridge.fieldAt(id, enabled) == 0.0f);

	// INIT IS THE DECLARED DEFAULTS.
	REQUIRE(browser.selectIndex(browser.indexOf("Init")));
	CHECK(bridge.fieldAt(id, jawari) == static_cast<float>(rsn::kParamDescs[jawari].def));
	CHECK(bridge.fieldAt(id, chord) == static_cast<float>(rsn::kParamDescs[chord].def));
	CHECK(bridge.fieldAt(id, enabled) == 0.0f);
}
TEST_CASE("RESONATE's panel: DRONE FX and WET FX rails over its two lanes, and Drone to Wet FX heads DRONE FX while WET FX holds a device") {
	// RESONATE takes the fx lanes, plus a routing switch for whether drone fx also go through wet fx. Each
	// rail edits ITS lane — by the bridge it holds, compared with the lane the device files — and the
	// switch is shown exactly when it changes the sound: with WET FX empty the two routings are bit-identical.
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	engine.prepare(48000.0, 4096);
	feathers::rack::RackBridge bridge(engine);
	bridge.prepare(48000.0, 4096);
	const DeviceInstanceId id = bridge.addDevice(rsn::kTraits.tag);
	REQUIRE(id != kNoInstance);
	bridge.drain();
	bridge.publish(engine);
	void* const surface = bridge.containerSurfaceOf(id);
	REQUIRE(surface != nullptr);
	ui::WidgetRegistry registry;
	ui::ResonateExpanded panel(bridge, registry, id);
	const visage::Point shut = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, shut.x, shut.y);

	ui::FxLaneRail& drone = panel.droneRailForTest();
	ui::FxLaneRail& wet = panel.wetRailForTest();
	CHECK(std::string(drone.spec().label) == ui::ResonateExpanded::kDroneLabel);
	CHECK(std::string(wet.spec().label) == ui::ResonateExpanded::kWetLabel);
	CHECK(std::string(drone.captionForTest()) == ui::ResonateExpanded::kDroneLine);
	// EACH RAIL OVER ITS OWN LANE, and the two lanes distinct.
	REQUIRE(drone.liveForTest());
	REQUIRE(wet.liveForTest());
	CHECK(drone.bridgeForTest() == rsn::laneChainOf(surface, rsn::Lane::Drone)->bridge());
	CHECK(wet.bridgeForTest() == rsn::laneChainOf(surface, rsn::Lane::Wet)->bridge());
	CHECK(drone.bridgeForTest() != wet.bridgeForTest());
	// SHUT BY DEFAULT, left to right face | DRONE FX | WET FX.
	CHECK(drone.collapsed());
	CHECK(wet.collapsed());
	CHECK(drone.x() < wet.x());

	// OPEN DRONE FX: the switch is NOT shown while WET FX is empty.
	(void)drone.setCollapsed(false);
	(void)panel.updateContent();
	CHECK_FALSE(drone.accessoryShownForTest());
	CHECK_FALSE(panel.routeChipForTest().isVisible());
	const float openNoSwitch = panel.minimumSize().y;

	// A DEVICE IN WET FX: the switch shows, at the top of DRONE FX, on (the default), and the panel's floor grows by its row.
	REQUIRE(wet.bridgeForTest()->addDevice(feathers::fx::busutil::kTraits.tag) != kNoInstance);
	CHECK(panel.updateContent());
	CHECK(drone.accessoryShownForTest());
	ui::SwitchChip& chip = panel.routeChipForTest();
	CHECK(chip.isVisible());
	CHECK(chip.parent() == &drone);
	CHECK(chip.y() < drone.columnBounds().y());
	CHECK(chip.on());
	CHECK(panel.minimumSize().y >= openNoSwitch);
	CHECK(drone.openMinimumHeight() == ui::FxLaneRail::kOpenMinimumHeight + ui::SwitchChip::kHeight + ui::FxLaneRail::kGap);

	// A PRESS WRITES THE ROUTING, through the device's own control.
	const auto route = static_cast<size_t>(rsn::Param::DroneToWetFx);
	CHECK(bridge.fieldAt(id, route) == 1.0f);
	chip.toggleForTest();
	CHECK(bridge.fieldAt(id, route) == 0.0f);

	// SHUTTING THE RAIL HIDES IT, and reopening restores it under its gate.
	(void)drone.setCollapsed(true);
	CHECK_FALSE(chip.isVisible());
	(void)drone.setCollapsed(false);
	CHECK(chip.isVisible());
}
TEST_CASE("Featherverb's card draws IN, WET and GR beside Input, at the dB positions its readings name") {
	namespace fvc = feathers::fx::featherverb;
	DynCardFixture<ui::FeatherverbCard> d(fvc::kTraits.tag);
	ui::FeatherverbCard* card = d.card;

	// INPUT IS ON THE FACE, AND THE METER IS IN ITS ROW, RIGHT OF IT.
	const auto& params = card->paramIndicesForTest();
	const auto& rows = card->rowsForTest();
	visage::Frame* input = nullptr;
	for (size_t i = 0; i < params.size(); ++i) {
		if (params[i] == static_cast<size_t>(fvc::Param::InputGain)) {
			input = rows[i];
		}
	}
	REQUIRE(input != nullptr);
	REQUIRE(input->isVisible());
	const visage::Bounds meter = card->meterBoundsForTest();
	CHECK(meter.width() > 0.0f);
	CHECK(meter.x() >= input->bounds().right());
	CHECK(meter.right() <= static_cast<float>(card->width()));
	CHECK(meter.y() == doctest::Approx(input->bounds().y()));
	CHECK(meter.height() == doctest::Approx(input->bounds().height()));

	// NOTHING PUBLISHED IS NOT SILENCE: "no signal", no fractions.
	CHECK_FALSE(card->meterForTest().has);

	const auto publish = [&](float in, float wet, float gr) {
		std::array<float, static_cast<size_t>(fvc::Reading::kCount)> v {};
		v[static_cast<size_t>(fvc::Reading::InPeak)] = in;
		v[static_cast<size_t>(fvc::Reading::WetPeak)] = wet;
		v[static_cast<size_t>(fvc::Reading::LimiterGr)] = gr;
		d.publish(v);
	};
	publish(0.5f, 0.25f, 6.0f);
	auto m = card->meterForTest();
	REQUIRE(m.has);
	CHECK(std::fabs(m.in - expectedLevel(0.5f)) < 1e-5f);
	CHECK(std::fabs(m.wet - expectedLevel(0.25f)) < 1e-5f);
	CHECK(std::fabs(m.gr - 0.25f) < 1e-6f); // 6 dB of 24
	CHECK(m.in != m.wet);                    // not vacuous: no reading answers for another

	// DELIVERY: a moved bar asks for a frame; the same readings again do not.
	RedrawProbe probe(card);
	CHECK(card->updateContent());
	CHECK(probe.count == 1);
	probe.rearm();
	CHECK_FALSE(card->updateContent());
	CHECK(probe.count == 1);
	publish(0.5f, 0.25f, 12.0f);
	CHECK(card->updateContent());
	CHECK(probe.count == 2);
	CHECK(std::fabs(card->meterForTest().gr - 0.5f) < 1e-6f);
}
TEST_CASE("Featherverb's window seats Input in its grid and the meter in the cell beside it") {
	VerbRig rig;
	ui::FeatherverbExpanded& panel = *rig.panel;
	visage::Frame* input = rig.cell(fvx::Param::InputGain);
	REQUIRE(input != nullptr);
	CHECK(input->isVisible());
	const visage::Bounds meter = panel.meterBoundsForTest();
	CHECK(meter.width() > 0.0f);
	CHECK(meter.x() >= input->bounds().right());
	CHECK(meter.right() <= input->bounds().right() + input->bounds().width());
	CHECK(meter.y() == doctest::Approx(input->bounds().y()));
	// NO CONTROL UNDER IT: the meter takes a cell the grid left empty.
	for (const visage::Frame* cell : panel.cellsForTest()) {
		if (cell != nullptr && cell->isVisible() && cell != input) {
			CAPTURE(cell->bounds().x());
			CAPTURE(cell->bounds().y());
			CHECK_FALSE(cell->bounds().overlaps(meter));
		}
	}
	// THE SAME ROAD AS THE CARD'S: nothing published here yet, so "no signal".
	CHECK_FALSE(panel.meterForTest().has);
}

namespace {
//------------------------------------------------------------------------------------------------//
// BACKLOG LQ — STEREO LINK AND THE L/R-M/S SWITCH ON COMP AND OWLTT                              //
//------------------------------------------------------------------------------------------------//
/// WHETHER TWO RECTANGLES SHARE ANY AREA — touching edges do not.
bool lqOverlap(const visage::Bounds& a, const visage::Bounds& b) {
	return a.x() < b.right() && b.x() < a.right() && a.y() < b.bottom() && b.y() < a.bottom();
}
} // namespace

TEST_CASE("LQ: COMP's card carries Stereo Link in every mode and the L/R-M/S chip at the Detector row's right end") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;
	ui::Knob* link = card->knobForTest(cp::Param::StereoLink);
	ui::SwitchChip* chip = card->stereoForTest();
	REQUIRE(link != nullptr);
	REQUIRE(chip != nullptr);
	// THE CONTROLS THEY DRAW, BY VALUE: each field set to a number only it holds.
	d.set(static_cast<size_t>(cp::Param::StereoLink), 0.37f);
	CHECK(link->control()->value() == doctest::Approx(0.37));
	CHECK(chip->caption() == "L/R");
	d.set(static_cast<size_t>(cp::Param::StereoMode), 1.0f);
	CHECK(chip->caption() == "M/S");
	for (const float mode : { 0.0f, 1.0f, 2.0f }) {
		CAPTURE(mode);
		d.set(static_cast<size_t>(cp::Param::Mode), mode);
		(void)card->updateContent();
		CHECK(link->isVisible());
		CHECK(chip->isVisible());
		// ON THE DETECTOR ROW (backlog QG moved it there: the mode row needs the card's width for three names), to the
		// right of Self/Side and not over it, and below the mode row.
		const visage::Bounds seg = card->modeForTest()->bounds();
		const visage::Bounds det = card->detectorForTest()->bounds();
		CHECK(chip->bounds().x() >= det.right());
		CHECK(chip->bounds().y() >= det.y() - 1.0f);
		CHECK(chip->bounds().bottom() <= det.bottom() + 1.0f);
		CHECK(chip->bounds().y() >= seg.bottom());
		// AND LINK SITS IN THE GRID, under the mode row and not on any other live knob.
		CHECK(link->bounds().y() >= seg.bottom());
		for (const cp::Param p : card->addressedParamsForTest()) {
			ui::Knob* k = card->knobForTest(p);
			if (k != nullptr && k != link && k->isVisible()) {
				CAPTURE(std::string(cp::kParamDescs[static_cast<size_t>(p)].name));
				CHECK_FALSE(lqOverlap(k->bounds(), link->bounds()));
			}
		}
	}
	// THE REACHABILITY ANSWER NAMES BOTH.
	const auto addressed = card->addressedParamsForTest();
	CHECK(std::count(addressed.begin(), addressed.end(), cp::Param::StereoLink) == 1);
	CHECK(std::count(addressed.begin(), addressed.end(), cp::Param::StereoMode) == 1);
}
TEST_CASE("LQ: OWLTT's card and window both carry Link and the L/R-M/S chip") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::OwlttCard* card = d.card;
	REQUIRE(card->stereoForTest() != nullptr);
	REQUIRE(card->linkForTest() != nullptr);
	d.set(static_cast<size_t>(ow::Param::StereoLink), 0.42f);
	d.set(static_cast<size_t>(ow::Param::StereoMode), 1.0f);
	CHECK(card->linkForTest()->control().value() == doctest::Approx(0.42));
	CHECK(card->stereoForTest()->caption() == "M/S");
	CHECK(card->stereoForTest()->isVisible());
	CHECK(card->linkForTest()->isVisible());
	// THE STRIP SITS BETWEEN THE PICTURE AND THE KNOBS, overlapping neither.
	const visage::Bounds plot = card->crossoverBounds();
	for (visage::Frame* f : { static_cast<visage::Frame*>(card->stereoForTest()),
	                          static_cast<visage::Frame*>(card->linkForTest()) }) {
		CHECK(f->bounds().y() >= plot.bottom());
		for (const auto& knob : card->knobsForTest()) {
			CHECK_FALSE(lqOverlap(f->bounds(), knob->bounds()));
		}
	}
	CHECK_FALSE(lqOverlap(card->stereoForTest()->bounds(), card->linkForTest()->bounds()));

	// THE WINDOW: Link is a box in the GLOBAL row, the chip is in the top row beside the crossover picker.
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	int linkBoxes = 0;
	for (const auto& box : panel.globalBoxesForTest()) {
		if (panel.paramOfGlobalForTest(box.get()) == ow::Param::StereoLink) {
			++linkBoxes;
			CHECK(box->control().value() == doctest::Approx(0.42));
		}
	}
	CHECK(linkBoxes == 1);
	REQUIRE(panel.stereoForTest() != nullptr);
	CHECK(panel.stereoForTest()->caption() == "M/S");
	const visage::Bounds chip = panel.stereoForTest()->bounds();
	CHECK_FALSE(lqOverlap(chip, panel.modeBounds()));
	CHECK(chip.right() <= panel.modeBounds().x());
	CHECK(chip.x() >= 0.0f);
	CHECK(chip.bottom() <= panel.modalBottom() + 1.0f);
	const auto addressed = panel.addressedParamsForTest();
	CHECK(std::count(addressed.begin(), addressed.end(), ow::Param::StereoMode) == 1);
	CHECK(std::count(addressed.begin(), addressed.end(), ow::Param::StereoLink) == 1);
}
TEST_CASE("LR: OWLTT's card carries the global Width in the stereo strip, and the window carries it and each band's") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::OwlttCard* card = d.card;
	ui::ValueBox* width = card->widthForTest();
	REQUIRE(width != nullptr);
	// THE CONTROLS THEY DRAW, BY VALUE: each field set to a number only it holds.
	d.set(static_cast<size_t>(ow::Param::Width), 1.37f);
	d.set(static_cast<size_t>(ow::Param::LowWidth), 0.21f);
	d.set(static_cast<size_t>(ow::Param::MidWidth), 0.62f);
	d.set(static_cast<size_t>(ow::Param::HighWidth), 1.83f);
	CHECK(width->control().value() == doctest::Approx(1.37));
	CHECK(width->isVisible());
	// IN THE STRIP, after Link, inside the card, over nothing else.
	const visage::Bounds box = width->bounds();
	CHECK(box.width() > 40.0f);
	CHECK(box.right() <= static_cast<float>(card->width()) + 0.5f);
	CHECK(box.x() >= card->linkForTest()->bounds().right());
	CHECK(box.y() >= card->crossoverBounds().bottom());
	CHECK_FALSE(lqOverlap(box, card->linkForTest()->bounds()));
	CHECK_FALSE(lqOverlap(box, card->stereoForTest()->bounds()));
	for (const auto& knob : card->knobsForTest()) {
		CHECK_FALSE(lqOverlap(box, knob->bounds()));
	}

	// THE WINDOW: all four, once each, reading their own values.
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	// The global Width is a box in the GLOBAL row; each band's Width is the WIDTH row of the plot's table, a box under its band.
	const std::pair<ow::Param, double> widths[] = { { ow::Param::Width, 1.37 }, { ow::Param::LowWidth, 0.21 },
		                                            { ow::Param::MidWidth, 0.62 }, { ow::Param::HighWidth, 1.83 } };
	for (const auto& [param, value] : widths) {
		const std::string name = ow::kParamDescs[static_cast<size_t>(param)].name;
		CAPTURE(name);
		int found = 0;
		for (const auto& box : panel.globalBoxesForTest()) {
			if (panel.paramOfGlobalForTest(box.get()) == param) {
				++found;
				CHECK(box->control().value() == doctest::Approx(value));
				CHECK(box->isVisible());
				CHECK(box->bounds().bottom() <= want.y);
			}
		}
		for (size_t band = 0; band < 3; ++band) {
			const ui::ValueBox* cell = panel.graph()->tableBoxForTest(3, band);
			REQUIRE(cell != nullptr);
			if (static_cast<size_t>(ui::OwlttGraph::tableParam(3, band)) == static_cast<size_t>(param)) {
				++found;
				CHECK(cell->control().value() == doctest::Approx(value));
				CHECK(cell->isVisible());
			}
		}
		CHECK(found == 1);
	}
}
TEST_CASE("NK: OWLTT's window carries the three band Widths in the plot's table — Low, Mid, High left to right, each box its band's own control, under its band") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	d.set(static_cast<size_t>(ow::Param::LowWidth), 0.21f);
	d.set(static_cast<size_t>(ow::Param::MidWidth), 0.62f);
	d.set(static_cast<size_t>(ow::Param::HighWidth), 1.83f);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	REQUIRE(panel.graph() != nullptr);
	const ow::Param order[] = { ow::Param::LowWidth, ow::Param::MidWidth, ow::Param::HighWidth };
	const double values[] = { 0.21, 0.62, 1.83 };
	constexpr size_t kWidthRow = 3;
	float previousRight = -1.0f;
	for (size_t band = 0; band < 3; ++band) {
		CAPTURE(band);
		ui::ValueBox* cell = panel.graph()->tableBoxForTest(kWidthRow, band);
		REQUIRE(cell != nullptr);
		CHECK(ui::OwlttGraph::tableParam(kWidthRow, band) == order[band]);
		CHECK(std::string(ui::OwlttGraph::tableCaption(kWidthRow)) == "WIDTH");
		// THE CONTROL BY VALUE: the box reads the field only its band holds, and it is declared through ControlFace.
		CHECK(cell->control().value() == doctest::Approx(values[band]));
		std::vector<ui::ControlFace::Offered> offered;
		cell->offeredControls(offered);
		REQUIRE(offered.size() == 1);
		CHECK(offered[0].control == &cell->control());
		CHECK(std::string_view(offered[0].control->desc().key)
		      == std::string_view(ow::kParamDescs[static_cast<size_t>(order[band])].key));
		// LOW LEFT OF MID LEFT OF HIGH, never overlapping, inside the plot, under the bars.
		CHECK(cell->bounds().x() >= previousRight);
		previousRight = cell->bounds().right();
		CHECK(cell->bounds().y() >= panel.graph()->tableArea().y());
		CHECK(cell->bounds().bottom() <= panel.graph()->height());
		CHECK(cell->bounds().right() <= panel.graph()->width());
		CHECK(cell->tooltipAt({ 4.0f, 4.0f }) != nullptr);
	}
	// NO PILL BANK, NO KNOB FOR THE THREE: the table replaced both, it does not duplicate them.
	for (const auto& box : panel.globalBoxesForTest()) {
		const ow::Param p = panel.paramOfGlobalForTest(box.get());
		CHECK(p != ow::Param::LowWidth);
		CHECK(p != ow::Param::MidWidth);
		CHECK(p != ow::Param::HighWidth);
	}
	const auto addressed = panel.addressedParamsForTest();
	for (const ow::Param p : order) {
		CHECK(std::count(addressed.begin(), addressed.end(), p) == 1);
	}
}
TEST_CASE("ND: OWLTT's window carries the Lookahead as a box with its menu in the modal row, reading the stored choice") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	d.set(static_cast<size_t>(ow::Param::Lookahead), 3.0f);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::ValueBox* box = panel.lookaheadForTest();
	REQUIRE(box != nullptr);
	CHECK(box->control().value() == doctest::Approx(3.0));
	CHECK(box->control().text() == "3 ms");
	CHECK(box->isVisible());
	// IN THE MODAL ROW, left of the L/R-M/S chip, over nothing.
	CHECK(box->bounds().right() <= panel.stereoForTest()->bounds().x());
	CHECK(box->bounds().bottom() <= panel.graphBounds().y());
	CHECK(box->bounds().x() >= ui::OwlttExpanded::kPad);
	// A CHOICE OF SIX: a menu (every choice seen before it applies), never a click that cycles.
	CHECK(box->choiceMenuAvailable());
	const auto addressed = panel.addressedParamsForTest();
	CHECK(std::count(addressed.begin(), addressed.end(), ow::Param::Lookahead) == 1);
}
TEST_CASE("NB: OWLTT's Drive is a vertical bar LEFT of the graph on the card and in the window — as tall as the picture, reading the Drive, declared through ControlFace") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::OwlttCard* card = d.card;
	ui::VerticalFader* bar = card->driveForTest();
	REQUIRE(bar != nullptr);
	const auto drive = static_cast<size_t>(ow::Param::Drive);
	const auto valueOf = [&] {
		const FxChain chain = d.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, d.slot).row.field[drive];
	};
	d.set(drive, 6.5f);
	CHECK(bar->control().value() == doctest::Approx(6.5));
	CHECK(bar->readout() == "+6.5");
	CHECK(bar->caption() == "Drive");
	CHECK(bar->isVisible());
	// LEFT OF THE PICTURE, AS TALL AS IT, INSIDE THE CARD, OVER NOTHING.
	const auto checkBeside = [](const visage::Bounds& b, const visage::Bounds& plot) {
		CHECK(b.width() >= 20.0f);
		CHECK(b.right() <= plot.x());
		// NEAR IT: only the level meter (and its gap) stands between the fader and the picture.
		CHECK(plot.x() - b.right() < 12.0f + ui::OwlttLevelMeter::kWidth + 4.0f);
		CHECK(b.y() == doctest::Approx(plot.y()));
		CHECK(b.height() == doctest::Approx(plot.height()));
		CHECK(b.x() >= 0.0f);
	};
	checkBeside(bar->bounds(), card->crossoverBounds());
	REQUIRE(card->graph() != nullptr);
	checkBeside(bar->bounds(), card->graph()->bounds());
	for (const auto& knob : card->knobsForTest()) {
		CHECK_FALSE(lqOverlap(bar->bounds(), knob->bounds()));
	}
	CHECK_FALSE(lqOverlap(bar->bounds(), card->stereoForTest()->bounds()));
	// IT DECLARES THE DRIVE AS ITS FACE, which is what the window-superset guard reads.
	std::vector<ui::ControlFace::Offered> offered;
	bar->offeredControls(offered);
	REQUIRE(offered.size() == 1u);
	CHECK(offered[0].control->desc().key == std::string_view("drive"));
	CHECK(offered[0].road == ui::ControlFace::Road::Face);
	CHECK(std::string(bar->tooltipAt({ 4.0f, 40.0f })).find("Gain in front") != std::string::npos);

	// AN UPWARD DRAG RAISES IT: a quarter of `kSpan` is a quarter of the 36 dB range, 6.5 + 9 = 15.5 dB.
	const float cx = bar->width() * 0.5f;
	const float cy = bar->height() * 0.5f;
	const float quarter = 0.25f * ui::VerticalFader::kSpan;
	bar->onMouseDown().callback(at(cx, cy));
	bar->onMouseDrag().callback(at(cx, cy - quarter));
	bar->onMouseUp().callback(at(cx, cy - quarter));
	CHECK(valueOf() == doctest::Approx(15.5f));
	// DOUBLE-CLICK RESETS to the declared 0 dB.
	bar->onMouseDown().callback(doubleAt(cx, cy));
	bar->onMouseUp().callback(doubleAt(cx, cy));
	CHECK(valueOf() == 0.0f);
	CHECK(bar->readout() == "0");

	// ARMED, A DRAG ROUTES: the knob's seed 0.4, plus 35 px up over 70 -> 0.9, to the DRIVE, and the value stands.
	const backplane::Source source = backplane::Source::core(0);
	d.f.arm(source);
	bar->onMouseDown().callback(at(cx, cy));
	bar->onMouseDrag().callback(at(cx, cy - 35.0f));
	bar->onMouseUp().callback(at(cx, cy - 35.0f));
	CHECK(d.f.bridge.routeAmount(source, backplane::Destination::control(d.device, drive)) == doctest::Approx(0.9f));
	CHECK(valueOf() == 0.0f);

	// FOLDED, IT IS HIDDEN WITH THE PICTURE.
	card->setFolded(true);
	CHECK_FALSE(bar->isVisible());
	card->setFolded(false);
	CHECK(bar->isVisible());

	// THE WINDOW: its own bar, over the same Drive, left of its own graph.
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::VerticalFader* windowBar = panel.driveForTest();
	REQUIRE(windowBar != nullptr);
	d.set(drive, -4.0f);
	CHECK(windowBar->control().value() == doctest::Approx(-4.0));
	CHECK(windowBar->readout() == "-4.0");
	checkBeside(windowBar->bounds(), panel.graphBounds());
	REQUIRE(panel.graph() != nullptr);
	checkBeside(windowBar->bounds(), panel.graph()->bounds());
	CHECK(windowBar->bounds().y() >= panel.modalBottom());
	const auto addressed = panel.addressedParamsForTest();
	CHECK(std::count(addressed.begin(), addressed.end(), ow::Param::Drive) == 1);

	// THE OUTPUT FADER MIRRORS IT: the same widget class at the same size, the same distance from the plot and from the
	// window's edge on the other side, one control (`output_gain`) read in dB.
	ui::VerticalFader* outBar = panel.outputForTest();
	REQUIRE(outBar != nullptr);
	CHECK(outBar->bounds().width() == doctest::Approx(windowBar->bounds().width()));
	CHECK(outBar->bounds().height() == doctest::Approx(windowBar->bounds().height()));
	CHECK(outBar->bounds().y() == doctest::Approx(windowBar->bounds().y()));
	const visage::Bounds plotB = panel.graphBounds();
	// THE LEFT GAP HOLDS THE INPUT METER; the right gap holds none (the output meter is beyond the fader).
	CHECK(plotB.x() - windowBar->bounds().right() == doctest::Approx(outBar->bounds().x() - plotB.right() + ui::OwlttLevelMeter::kWidth + ui::OwlttExpanded::kLevelGap));
	CHECK(windowBar->bounds().x() == doctest::Approx(want.x - panel.outputMeterBounds().right()));
	CHECK(outBar->bounds().x() >= plotB.right());
	CHECK(std::string_view(outBar->control().desc().key) == "output_gain");
	CHECK(outBar->caption() == "Output");
	std::vector<ui::ControlFace::Offered> outOffered;
	outBar->offeredControls(outOffered);
	REQUIRE(outOffered.size() == 1u);
	CHECK(outOffered[0].road == ui::ControlFace::Road::Face);
	CHECK(std::count(addressed.begin(), addressed.end(), ow::Param::OutputGain) == 1);
	// ITS READOUT IS THE GAIN THE DSP APPLIES, in dB: the knob's 0.5 is unity, 1.0 is +16 dB, 0.25 is -6 dB.
	d.set(static_cast<size_t>(ow::Param::OutputGain), 0.5f);
	CHECK(outBar->readout() == "0");
	d.set(static_cast<size_t>(ow::Param::OutputGain), 1.0f);
	CHECK(outBar->readout() == "+16");
	d.set(static_cast<size_t>(ow::Param::OutputGain), 0.25f);
	CHECK(outBar->readout() == "-6.0");
}
TEST_CASE("OWLTT's card carries Output: a fader right of the picture, mirroring the Drive, bound to output_gain, declared through ControlFace") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	ui::OwlttCard* card = d.card;
	ui::VerticalFader* out = card->outputForTest();
	REQUIRE(out != nullptr);
	const auto gain = static_cast<size_t>(ow::Param::OutputGain);
	const auto valueOf = [&] {
		const FxChain chain = d.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, d.slot).row.field[gain];
	};
	// BOUND TO THE DEVICE'S OUTPUT GAIN, by its key, and labelled as the window labels it.
	CHECK(std::string_view(out->control().desc().key) == "output_gain");
	CHECK(out->caption() == "Output");
	// THE READOUT IS THE DSP'S GAIN IN dB, the same mapping the window uses: 0.5 unity, 1.0 +16 dB, 0.25 -6 dB.
	d.set(gain, 1.0f);
	CHECK(out->control().value() == doctest::Approx(1.0));
	CHECK(out->readout() == "+16");
	d.set(gain, 0.25f);
	CHECK(out->readout() == "-6.0");
	d.set(gain, 0.5f);
	CHECK(out->readout() == "0");

	// BESIDE THE PICTURE, THE MIRROR OF THE DRIVE, inside the card and over nothing.
	const visage::Bounds plot = card->crossoverBounds();
	const visage::Bounds drive = card->driveBounds();
	const visage::Bounds bar = out->bounds();
	CHECK(bar.width() == doctest::Approx(drive.width()));
	CHECK(bar.height() == doctest::Approx(plot.height()));
	CHECK(bar.y() == doctest::Approx(plot.y()));
	CHECK(bar.x() >= plot.right());
	CHECK(bar.x() - plot.right() + ui::OwlttLevelMeter::kWidth + ui::OwlttCard::kLevelGap == doctest::Approx(plot.x() - drive.right()));
	CHECK(bar.right() <= static_cast<float>(card->width()) + 0.5f);
	for (const auto& knob : card->knobsForTest()) {
		CHECK_FALSE(lqOverlap(bar, knob->bounds()));
	}
	CHECK_FALSE(lqOverlap(bar, card->stereoForTest()->bounds()));
	CHECK_FALSE(lqOverlap(bar, card->linkForTest()->bounds()));
	CHECK_FALSE(lqOverlap(bar, card->widthForTest()->bounds()));

	// IT DECLARES THE OUTPUT AS ITS FACE, which is what the window-superset guard reads.
	std::vector<ui::ControlFace::Offered> offered;
	out->offeredControls(offered);
	REQUIRE(offered.size() == 1u);
	CHECK(std::string_view(offered[0].control->desc().key) == "output_gain");
	CHECK(offered[0].road == ui::ControlFace::Road::Face);

	// AN UPWARD DRAG RAISES IT, and the value it writes is the device's: a quarter of the span is a quarter of the
	// 0..1 knob, so 0.5 + 0.25 = 0.75.
	d.set(gain, 0.5f);
	const float cx = out->width() * 0.5f;
	const float cy = out->height() * 0.5f;
	const float quarter = 0.25f * ui::VerticalFader::kSpan;
	out->onMouseDown().callback(at(cx, cy));
	out->onMouseDrag().callback(at(cx, cy - quarter));
	out->onMouseUp().callback(at(cx, cy - quarter));
	CHECK(valueOf() == doctest::Approx(0.75f));
}
TEST_CASE("NB: OWLTT's crossover labels never touch a channel letter or each other — on the card and in the window, at the defaults and with the crossovers crowded") {
	// the card read "2.0 kHzR" once the Drive bar narrowed the graph, and the mid band's "L" had been under "200 Hz"
	// since the letters arrived. Asked of the RECTANGLES the graph draws its glyphs in (`OwlttGraph::bottomText`).
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
	struct Scene {
		const char* name;
		float low;
		float high;
		float ms;
	};
	const Scene scenes[] = { { "defaults", 200.0f, 2000.0f, 0.0f }, { "close", 800.0f, 900.0f, 0.0f },
		                     { "crowded", 450.0f, 900.0f, 0.0f },
		                     { "crowded M/S", 450.0f, 900.0f, 1.0f }, { "wide", 60.0f, 9000.0f, 0.0f } };
	size_t lettersSeen = 0;
	for (const Scene& s : scenes) {
		d.set(static_cast<size_t>(ow::Param::LowCrossover), s.low);
		d.set(static_cast<size_t>(ow::Param::HighCrossover), s.high);
		d.set(static_cast<size_t>(ow::Param::StereoMode), s.ms);
		for (ui::OwlttGraph* graph : { d.card->graph(), panel.graph() }) {
			REQUIRE(graph != nullptr);
			CAPTURE(s.name);
			CAPTURE(graph == panel.graph());
			const ui::OwlttGraph::BottomText t = graph->bottomText();
			lettersSeen += t.letters.size();
			for (size_t which = 0; which < 2; ++which) {
				const visage::Bounds label = t.frequency[which];
				CAPTURE(t.frequencyText[which]);
				CHECK(label.width() > 10.0f); // a real text width, not a zero box that overlaps nothing
				CHECK(label.x() >= 0.0f);
				CHECK(label.right() <= graph->width());
				CHECK(label.y() >= 0.0f);
				for (const visage::Bounds& letter : t.letters) {
					CHECK_FALSE(overlap(label, letter));
				}
			}
			CHECK_FALSE(overlap(t.frequency[0], t.frequency[1]));
			// AND NEVER OVER A HANDLE: not the crossover handles in the lane, not any bar or strip ring, not a ratio thumb, and
			// not the lane-free card's lights (checked as rings: the bars' rings are what a label could be struck through by).
			using H = ui::OwlttGraph::Handle;
			std::vector<visage::Bounds> handles;
			for (size_t band = 0; band < 3; ++band) {
				for (const H h : { H::ThresholdBar, H::BandwidthBar, H::RatioStrip, H::LevelStrip }) {
					const visage::Point c = graph->ringCentre(band, h);
					handles.push_back({ c.x - 4.0f, c.y - 4.0f, 8.0f, 8.0f });
				}
				if (graph->editor()) {
					for (const H h : { H::DownRatio, H::UpRatio }) {
						const visage::Point c = graph->ringCentre(band, h);
						handles.push_back({ c.x - 6.0f, c.y - 6.0f, 12.0f, 12.0f });
					}
				}
			}
			if (graph->editor()) {
				for (size_t which = 0; which < 2; ++which) {
					const visage::Point c = graph->crossoverHandle(which);
					handles.push_back({ c.x - 4.0f, c.y - 4.0f, 8.0f, 8.0f });
				}
			}
			for (size_t which = 0; which < 2; ++which) {
				for (const visage::Bounds& hb : handles) {
					CAPTURE(which);
					CHECK_FALSE(overlap(t.frequency[which], hb));
				}
			}
		}
	}
	CHECK(lettersSeen > 0u); // NOT VACUOUS: the letters were there to be avoided
}
TEST_CASE("LQ: OWLTT's band meters read each channel's own gain reduction off the lane's road") {
	// THE LANE PUBLISHES OWLTT THROUGH THE BRIDGE'S OWN STORES (`publishOwlttMeters`), not the generic sink, so the
	// second channel has to cross that road too — or the graph would draw the first channel twice.
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d(ow::kTraits.tag);
	feathers::BridgeDeviceHost& host = d.f.deviceHost;
	const std::array<float, 3> first { -0.5f, -0.25f, 0.0f };
	const std::array<float, 3> second { 0.0f, -0.125f, -0.75f };
	d.f.bridge.publishOwlttMeters(d.slot, first, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, false,
	                              { false, false, false }, 3.0f, &second);
	REQUIRE(host.readingCountAt(d.device) == feathers::fx::owltt::Owltt::kReadingCount); // `Owltt::kReadingCount` since the ratio readout (20 since Key Track (17 since LQ)
	for (size_t b = 0; b < 3; ++b) {
		CAPTURE(b);
		CHECK(host.readingAt(d.device, b) == first[b]);
		CHECK(host.readingAt(d.device, 14 + b) == second[b]);
	}
	const ui::OwlttGraph::Meters m = d.card->graph()->meters();
	CHECK(m.grDB == first);
	CHECK(m.grSecond == second);
	// A PUBLISHER WITH ONE LINKED GAIN (no second channel) reads the first again, never a stale second.
	d.f.bridge.publishOwlttMeters(d.slot, first, { 0.5f, 0.5f, 0.5f }, { 0.4f, 0.4f, 0.4f }, false);
	CHECK(d.card->graph()->meters().grSecond == first);
}
TEST_CASE("OWLTT's gain-reduction meter wears the legend's colours: reduction the DOWN colour, boost the UP colour, both opaque") {
	// THE LEGEND'S KEY: DOWN is kValue, UP is kAccent (owltt_graph.cpp, the colour key). Boost at 0.4 alpha read as a
	// disabled band. Asked through `grFillColour`, which the meter draws every half through.
	const auto opaque = [](uint32_t c) { return (c >> 24) == 0xFFu; };
	CHECK(ui::OwlttGraph::grFillColour(-0.5f) == kValue);
	CHECK(ui::OwlttGraph::grFillColour(-0.001f) == kValue);
	CHECK(ui::OwlttGraph::grFillColour(0.5f) == kAccent);
	CHECK(ui::OwlttGraph::grFillColour(0.001f) == kAccent);
	CHECK(opaque(ui::OwlttGraph::grFillColour(-0.5f)));
	CHECK(opaque(ui::OwlttGraph::grFillColour(0.5f)));
	CHECK(kValue != kAccent);
}

namespace {
/// AN ENGINE RUN, AND WHAT IT PUBLISHED.
struct DelayRun {
	dl::Delay engine;
	std::array<float, dl::kReadingCount> readings {};
	size_t count = 0;

	explicit DelayRun(const dl::Settings& s, int frames = 9600) {
		engine.prepare(48000.0);
		engine.setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(0.5));
		engine.setReadingsSink(feathers::device::ReadingsSink {
		    this, [](void* context, const float* values, size_t n) {
			    auto* self = static_cast<DelayRun*>(context);
			    self->count = n;
			    for (size_t i = 0; i < std::min(n, self->readings.size()); ++i) {
				    self->readings[i] = values[i];
			    }
		    } });
		engine.setTarget(s);
		run(frames);
	}
	void run(int frames) {
		std::vector<float> l(256), r(256);
		for (int done = 0; done < frames; done += 256) {
			const int n = std::min(256, frames - done);
			for (int i = 0; i < n; ++i) {
				l[static_cast<size_t>(i)] = 0.5f * std::sin(0.02f * static_cast<float>(done + i));
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			engine.processStereo(l.data(), r.data(), n);
		}
	}
};
dl::Settings delayDefaults() {
	return dl::stateOf(dl::rowOf(dl::State {}), static_cast<size_t>(dl::Param::kCount)).settings;
}
/// THE CARD'S FIELDS TO MATCH `s`, so the grid and the axis are the device's.
void setDelayFields(DynCardFixture<ui::DelayCard>& d, const dl::Settings& s) {
	const dl::Row row = dl::rowOf(dl::State { s });
	for (size_t i = 0; i < row.size(); ++i) {
		d.set(i, row[i]);
	}
}
} // namespace

TEST_CASE("Delay card: the taps strip draws the engine's live heads and gains, in every mode, sync on and off") {
	DynCardFixture<ui::DelayCard> d(dl::kTraits.tag);
	ui::DelayCard* card = d.card;
	// NOTHING PUBLISHED IS NOTHING DRAWN — no tick, no span, rather than a picture of zeros.
	CHECK(card->ticksForTest().empty());
	CHECK(card->spanForTest() == 0.0);

	for (const size_t sync : { size_t { 1 }, dl::kDefaultSync }) {
		for (int m = 0; m < static_cast<int>(dl::Mode::kCount); ++m) {
			CAPTURE(sync);
			CAPTURE(m);
			dl::Settings s = delayDefaults();
			s.sync = sync;
			s.timeMultiple = 1.5;
			s.spread = 0.4f;
			s.mode = static_cast<dl::Mode>(m);
			DelayRun run(s);
			REQUIRE(run.count == dl::kReadingCount);
			setDelayFields(d, s);
			d.publish(run.readings);

			const auto ticks = card->ticksForTest();
			// THE FIRST PASS ON EACH SIDE, FROM THAT SIDE'S INPUT: the heads the engine read at, times its
			// tap fractions, at its tap gains times Mix — read off the engine's seams, not its readings.
			for (int c = 0; c < 2; ++c) {
				CAPTURE(c);
				std::vector<std::pair<double, float>> drawn;
				for (const dl::StripTick& t : ticks) {
					if (t.pass == 0 && t.channel == c && t.origin == c) {
						drawn.emplace_back(t.seconds, t.gain);
					}
				}
				std::vector<std::pair<double, float>> engine;
				if (s.mode == dl::Mode::Reverse) {
					// REVERSE (backlog MB): the mirrors every half chunk from the leading head's, the two in the next
					// chunk each returning an impulse arriving now at twice its distance, at sin² of it — restated from
					// the engine's chunk and the age of its leading head, its seams.
					const double chunk = run.engine.reverseChunkSecondsForTest(c);
					const double age = run.engine.reverseAgeSecondsForTest(c);
					for (int k = 0; k < 8; ++k) {
						const double o = -age + 0.5 * chunk * static_cast<double>(k);
						const double w = std::pow(std::sin(3.14159265358979323846 * o / chunk), 2.0);
						if (o >= 0.0 && o < chunk && s.mix * w > dl::kStripGainFloor) {
							engine.emplace_back(2.0 * o, static_cast<float>(s.mix * w));
						}
					}
				}
				for (int k = 0; k < run.engine.tapCountForTest() && s.mode != dl::Mode::Reverse; ++k) {
					engine.emplace_back(run.engine.headSecondsForTest(c) * run.engine.tapFractionForTest(static_cast<size_t>(k)),
					                    s.mix * run.engine.tapGainForTest(c, static_cast<size_t>(k)));
				}
				std::sort(drawn.begin(), drawn.end());
				std::sort(engine.begin(), engine.end());
				REQUIRE(drawn.size() == engine.size());
				for (size_t i = 0; i < drawn.size(); ++i) {
					CAPTURE(i);
					CHECK(std::fabs(drawn[i].first - engine[i].first) < 1.0e-6);
					CHECK(std::fabs(drawn[i].second - engine[i].second) < 1.0e-6f);
				}
			}
			// THE DRY PATH at zero, at 1 - Mix.
			int dry = 0;
			for (const dl::StripTick& t : ticks) {
				if (t.pass < 0) {
					CHECK(t.seconds == 0.0);
					CHECK(std::fabs(t.gain - (1.0f - s.mix)) < 1.0e-6f);
					++dry;
				}
			}
			CHECK(dry == 2);
			// THE REPEATS FADE BY FEEDBACK: each own-side pass's base tap is Feedback times the last.
			if (s.mode == dl::Mode::Clean) {
				std::vector<float> left;
				for (const dl::StripTick& t : ticks) {
					if (t.channel == 0 && t.origin == 0 && t.pass >= 0) {
						left.push_back(t.gain);
					}
				}
				REQUIRE(left.size() >= 2);
				for (size_t i = 1; i < left.size(); ++i) {
					CHECK(std::fabs(left[i] - left[i - 1] * s.feedback) < 1.0e-6f);
				}
			}
			// GRANULAR: every published grain, on both sides.
			const size_t grains = s.mode == dl::Mode::Granular
			                          ? 2 * std::min(run.engine.activeGrainsForTest(), dl::kGrainReadings)
			                          : 0;
			CHECK(card->grainsForTest().size() == grains);
			// THE AXIS IS THE HAND'S: three of the tap the fields ask for (the longer channel's), snapped up.
			const double handTap = (sync == 1 ? 0.1 : 0.25) * 1.5 * (1.0 + dl::kSpreadFraction * 0.4);
			CHECK(card->spanForTest() == dl::stripSpanSeconds(handTap));
			CHECK(card->spanForTest() >= 3.0 * handTap);
		}
	}
}
TEST_CASE("Delay card: left taps are drawn above the line and right taps below, and Ping-Pong crosses them") {
	DynCardFixture<ui::DelayCard> d(dl::kTraits.tag);
	ui::DelayCard* card = d.card;
	dl::Settings s = delayDefaults();
	s.sync = 1;
	s.spread = 0.4f;
	s.feedback = 0.6f;
	s.pingPong = dl::PingPong::Stereo;
	DelayRun run(s);
	setDelayFields(d, s);
	d.publish(run.readings);

	const visage::Bounds strip = card->stripBoundsForTest();
	const float mid = strip.y() + strip.height() * 0.5f;
	const auto ticks = card->ticksForTest();
	REQUIRE(ticks.size() >= 8);
	int crossed = 0;
	for (const dl::StripTick& t : ticks) {
		CAPTURE(t.seconds);
		CAPTURE(t.channel);
		const float tip = card->tipYForTest(t.channel, t.gain);
		if (t.channel == 0) {
			CHECK(tip < mid);
		}
		else {
			CHECK(tip > mid);
		}
		CHECK(tip >= strip.y());
		CHECK(tip <= strip.bottom());
		// **PING-PONG: AN INPUT'S PASSES ALTERNATE SIDES**, starting on its own.
		if (t.pass >= 0) {
			CHECK(t.channel == (t.origin + t.pass) % 2);
			crossed += t.channel != t.origin ? 1 : 0;
		}
	}
	CHECK(crossed >= 2);
	// THE LEFT INPUT'S FIRST CROSSED ARRIVAL: one left head then one right head, on the RIGHT side —
	// 100 ms + 110 ms, the time no uncrossed arrival can have with Spread at 40%.
	bool found = false;
	for (const dl::StripTick& t : ticks) {
		if (t.origin == 0 && t.pass == 1) {
			CHECK(t.channel == 1);
			CHECK(std::fabs(t.seconds - 0.210) < 1.0e-6);
			found = true;
		}
	}
	CHECK(found);
	// AND TIME RUNS LEFT TO RIGHT across the strip, inside it.
	CHECK(card->xForTest(0.0) >= strip.x());
	CHECK(card->xForTest(0.1) < card->xForTest(0.2));
	CHECK(card->xForTest(card->spanForTest()) <= strip.right());
}
TEST_CASE("Delay card: the strip sits above the grid, and the card delivers a redraw when the taps move") {
	DynCardFixture<ui::DelayCard> d(dl::kTraits.tag);
	ui::DelayCard* card = d.card;
	REQUIRE(card->isVisible());
	REQUIRE(card->isDrawing());

	// THE GRID STARTS BELOW THE STRIP: every visible row's top is under the strip's bottom.
	const visage::Bounds strip = card->stripBoundsForTest();
	int rows = 0;
	for (visage::Frame* row : card->rowsForTest()) {
		if (row->isVisible()) {
			CHECK(static_cast<float>(row->bounds().y()) >= strip.bottom());
			++rows;
		}
	}
	CHECK(rows > 0);
	CHECK(card->laidOutBottomForTest() <= static_cast<float>(card->height()));

	(void)card->updateContent();
	RedrawProbe probe(card);
	dl::Settings s = delayDefaults();
	s.mode = dl::Mode::Tape;
	DelayRun run(s);
	d.publish(run.readings);
	CHECK(card->updateContent());
	CHECK(probe.count == 1);
	// NOTHING MOVED: no report and no ask.
	probe.rearm();
	CHECK_FALSE(card->updateContent());
	CHECK(probe.count == 1);
	// **THE TAPE'S WOW MOVES THE STRIP**: the same settings a little later read a different head.
	const double before = card->ticksForTest()[2].seconds;
	run.run(4800);
	d.publish(run.readings);
	probe.rearm();
	CHECK(card->updateContent());
	CHECK(probe.count == 2);
	CHECK(card->ticksForTest()[2].seconds != before);
}
TEST_CASE("Delay card: Classic Ping-Pong draws one chain bouncing above, below, above; Stereo draws a centred sound on both sides") {
	// 2026-10-11: the strip showed ticks on both sides at once — true of the audio: Stereo (the old `On`)
	// repeats a centred sound on both sides. Classic is the bounce; asked of the card's own ticks and its geometry.
	DynCardFixture<ui::DelayCard> d(dl::kTraits.tag);
	ui::DelayCard* card = d.card;
	const visage::Bounds strip = card->stripBoundsForTest();
	const float mid = strip.y() + strip.height() * 0.5f;
	for (const dl::PingPong pp : { dl::PingPong::Stereo, dl::PingPong::Classic }) {
		CAPTURE(static_cast<int>(pp));
		dl::Settings s = delayDefaults();
		s.sync = 1;
		s.feedback = 0.6f;
		s.pingPong = pp;
		DelayRun run(s);
		setDelayFields(d, s);
		d.publish(run.readings);
		const auto ticks = card->ticksForTest();
		// PER PASS: WHICH SIDES CARRY A TICK.
		std::array<std::array<int, 2>, 8> sides {};
		for (const dl::StripTick& t : ticks) {
			if (t.pass >= 0 && t.pass < 8) {
				++sides[static_cast<size_t>(t.pass)][static_cast<size_t>(t.channel)];
				const float tip = card->tipYForTest(t.channel, t.gain);
				CHECK((t.channel == 0 ? tip < mid : tip > mid));
			}
		}
		int passes = 0;
		for (size_t pass = 0; pass < sides.size(); ++pass) {
			if (sides[pass][0] + sides[pass][1] == 0) {
				continue;
			}
			CAPTURE(pass);
			++passes;
			if (pp == dl::PingPong::Classic) {
				// ONE TICK A PASS, ON ONE SIDE, ALTERNATING FROM THE LEFT.
				CHECK(sides[pass][pass % 2] == 1);
				CHECK(sides[pass][1 - pass % 2] == 0);
			}
			else {
				// BOTH SIDES AT ONCE — the inputs' two chains, each crossing.
				CHECK(sides[pass][0] == 1);
				CHECK(sides[pass][1] == 1);
			}
		}
		CHECK(passes >= 4);
	}
}
TEST_CASE("Delay card: photographs of the strip (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — the venue for LOOKING, as `test_gui_widgets_lane`'s filter shots are. Writes RGBA `.raw`
	// frames of the whole editor with the Delay card in each state, and prints where the card sits, for a human to
	// crop and convert (`convert -size WxH -depth 8 rgba:<raw> <png>`). Skipped without the variable.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	DynCardFixture<ui::DelayCard> d(dl::kTraits.tag);
	auto* editor = dynamic_cast<visage::ApplicationEditor*>(d.f.root);
	REQUIRE(editor != nullptr);
	editor->setWindowless(static_cast<int>(d.f.root->width()), static_cast<int>(d.f.root->height()));
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	const auto shoot = [&](const char* name, const dl::Settings& s) {
		DelayRun run(s);
		setDelayFields(d, s);
		d.publish(run.readings);
		d.lane->scrollCardIntoView(d.slot);
		d.f.bridge.refresh();
		(void)d.card->updateContent();
		d.f.root->redrawAll();
		editor->drawWindow();
		editor->drawWindow();
		editor->drawWindow();
		const visage::Screenshot& shot = editor->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("no frame for " << name);
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".raw";
		std::ofstream out(path, std::ios::binary);
		out.write(reinterpret_cast<const char*>(shot.data()), static_cast<std::streamsize>(shot.width()) * shot.height() * 4);
		const visage::Point at = d.card->positionInWindow();
		MESSAGE(name << " " << shot.width() << "x" << shot.height() << " card " << at.x << "," << at.y << " "
		             << d.card->width() << "x" << d.card->height() << " -> " << path);
	};
	dl::Settings s = delayDefaults();
	s.sync = 1;
	s.feedback = 0.6f;
	s.pingPong = dl::PingPong::Stereo;
	shoot("delay-pingpong-stereo", s);
	s.pingPong = dl::PingPong::Classic;
	shoot("delay-pingpong-classic", s);
	// REVERSE (backlog MB): the two returns a pass, each with the whisker of the chunk it plays back in.
	s.mode = dl::Mode::Reverse;
	s.pingPong = dl::PingPong::Off;
	shoot("delay-reverse", s);
	s.pingPong = dl::PingPong::Classic;
	shoot("delay-reverse-classic", s);
	s.pingPong = dl::PingPong::Off;
	s.sync = dl::kDefaultSync; // 1/8 at 120 BPM: a 250 ms chunk
	s.spread = 0.5f;
	shoot("delay-reverse-sync-spread", s);
}
TEST_CASE("FA: the Shaper's input filters stay lit at Knee 0 on the card and in the window; the Mix greys with the curve") {
	// THE HONESTY RULE, BOTH SURFACES. The filters run ahead of the Knee-0 bypass, so they still change the
	// sound there (test_fx_shaper's "each new control changes the sound" measures it); the crossfade has nothing
	// to cross at the bypass, so it greys with the rest. The card carries HPF and LPF and not the Mix; the window
	// carries all three.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.card != nullptr);
	REQUIRE(f.panel != nullptr);
	const auto activeOf = [](const auto& surface, shp::Param p) -> int {
		for (const auto& knob : surface.knobsForTest()) {
			if (surface.paramOfKnobForTest(knob.get()) == p) {
				return knob->active() ? 1 : 0;
			}
		}
		return -1; // not on this surface
	};
	f.card->showInSignal(false);
	f.panel->showInSignal(false);
	CHECK(activeOf(*f.card, shp::Param::InputHpf) == 1);
	CHECK(activeOf(*f.card, shp::Param::InputLpf) == 1);
	// AND TRIM, WHICH RUNS ABOVE THE BYPASS TOO (the FA follow-up): it was greyed here while it still moved what
	// the clamp sees. test_fx_shaper's "FA follow-up: Trim acts at Knee 0" asserts the value it leaves there.
	CHECK(activeOf(*f.card, shp::Param::Trim) == 1);
	CHECK(activeOf(*f.card, shp::Param::DryWet) == -1);
	CHECK(activeOf(*f.card, shp::Param::Drive) == 0); // non-vacuous: the bypass did grey the card
	CHECK(activeOf(*f.panel, shp::Param::InputHpf) == 1);
	CHECK(activeOf(*f.panel, shp::Param::InputLpf) == 1);
	CHECK(activeOf(*f.panel, shp::Param::Trim) == 1);
	CHECK(activeOf(*f.panel, shp::Param::DryWet) == 0);
	CHECK(activeOf(*f.panel, shp::Param::Threshold) == 0);
	f.card->showInSignal(true);
	f.panel->showInSignal(true);
	CHECK(activeOf(*f.card, shp::Param::Drive) == 1);
	CHECK(activeOf(*f.panel, shp::Param::DryWet) == 1);

	SUBCASE("the card's filters sit under Knee and Colour, the window's at the end of its top row with Mix beneath LPF") {
		const auto boundsOf = [](const auto& surface, shp::Param p) -> visage::Bounds {
			for (const auto& knob : surface.knobsForTest()) {
				if (surface.paramOfKnobForTest(knob.get()) == p) {
					return knob->bounds();
				}
			}
			return {};
		};
		const auto& cardCells = f.card->cellsForTest();
		REQUIRE(cardCells.size() == ui::ShaperCard::kKnobCells);
		const visage::Bounds hpf = boundsOf(*f.card, shp::Param::InputHpf);
		const visage::Bounds lpf = boundsOf(*f.card, shp::Param::InputLpf);
		CHECK(hpf.x() == doctest::Approx(cardCells[0]->bounds().x()));
		CHECK(lpf.x() == doctest::Approx(cardCells[1]->bounds().x()));
		CHECK(hpf.y() > cardCells[0]->bounds().bottom());
		CHECK(hpf.width() > 0.0f);
		const visage::Bounds wHpf = boundsOf(*f.panel, shp::Param::InputHpf);
		const visage::Bounds wLpf = boundsOf(*f.panel, shp::Param::InputLpf);
		const visage::Bounds wMix = boundsOf(*f.panel, shp::Param::DryWet);
		CHECK(wHpf.y() == doctest::Approx(wLpf.y()));
		CHECK(wLpf.x() > wHpf.x());
		CHECK(wMix.x() == doctest::Approx(wLpf.x()));
		CHECK(wMix.y() > wLpf.bottom());
		// INSIDE THE WINDOW'S OWN MINIMUM, which is where the fixture sized it.
		CHECK(wLpf.right() <= f.panel->width() + 0.5f);
		CHECK(wMix.bottom() <= f.panel->height() + 0.5f);
	}
}

namespace {
constexpr double kC3 = 261.6255653005986;
} // namespace

TEST_CASE("Spectral's card draws the drawer's plot over the device: the partials of f0 through the device's own operators") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	d.set(static_cast<size_t>(spx::Param::FilterOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::FilterCutoff), static_cast<float>(std::log2(1200.0)));
	d.set(static_cast<size_t>(spx::Param::KeyOffset), 12.0f); // Key Track off: f0 = C4
	(void)d.card->updateContent();
	const feathers::SpectralPlotFrame& frame = d.card->plotForTest().frameForTest();
	// NO INPUT YET: the reference terrain (a sawtooth's 1/k) at f0 = C3 raised by Offset, and the label says so.
	REQUIRE(frame.count > 8);
	CHECK(frame.f0 == doctest::Approx(2.0 * kC3).epsilon(1e-5));
	CHECK(frame.playing == 0);
	CHECK(d.card->plotForTest().noteLabel().rfind("reference", 0) == 0);
	// THE RESULT, RESTATED: the device's params at f0, the shaper's gains over that terrain, times the terrain.
	spx::Settings s;
	s.filterOn = true;
	s.filterCutoff = static_cast<float>(std::log2(1200.0));
	s.keyOffset = 12.0f;
	const double f0 = 2.0 * kC3;
	const auto count = static_cast<size_t>(frame.count);
	std::vector<float> k(count), hz(count), terrain(count), out(count);
	for (size_t i = 0; i < count; ++i) {
		k[i] = static_cast<float>(i + 1);
		hz[i] = static_cast<float>(f0 * static_cast<double>(i + 1));
		terrain[i] = 1.0f / static_cast<float>(i + 1);
	}
	feathers::spectral::gains(feathers::spectral::prepare(spx::shaperParamsOf(s, f0), f0), k.data(), hz.data(), terrain.data(),
	                          frame.count, out.data());
	size_t bad = 0;
	for (size_t i = 0; i < count; ++i) {
		bad += frame.left[i] != out[i] * terrain[i];
	}
	CHECK(bad == 0);
	CHECK(frame.left[count - 1] < 0.01f * frame.terrain[count - 1]); // the low pass is drawn
}
TEST_CASE("C1: Spectral's card is the plot, the badge and the knobs the handles do not carry — no switches, no box; an operator's own knob shows only while it is on") {
	// The card is simplified, without the toggles and the cell above them — the operator chips and the
	// Resolution/Key/Poly/Offset box are gone from the card; every one of them is in the window (asserted below).
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::SpectralCard* card = d.card;
	for (const spx::Param p : { spx::Param::Resolution, spx::Param::KeyTrack, spx::Param::Poly, spx::Param::KeyOffset,
	                            spx::Param::FilterOn, spx::Param::CombOn, spx::Param::VowelOn, spx::Param::TiltOn, spx::Param::OddEvenOn,
	                            spx::Param::PartialsOn, spx::Param::PartialsMode, spx::Param::VocodeOn, spx::Param::CurveOn,
	                            spx::Param::FilterMode, spx::Param::VocodeCarrier }) {
		CAPTURE(static_cast<int>(p));
		CHECK(card->widgetForTest(p) == nullptr);
	}
	CHECK(card->widgetForTest(spx::Param::Mix)->isVisible());
	CHECK_FALSE(card->widgetForTest(spx::Param::FilterSlope)->isVisible());
	CHECK_FALSE(card->widgetForTest(spx::Param::CombPhase)->isVisible());
	CHECK_FALSE(card->widgetForTest(spx::Param::OddEvenBalance)->isVisible());
	for (const spx::Param p : { spx::Param::FilterOn, spx::Param::CombOn, spx::Param::OddEvenOn }) {
		d.set(static_cast<size_t>(p), 1.0f);
	}
	(void)card->updateContent();
	CHECK(card->widgetForTest(spx::Param::FilterSlope)->isVisible());
	CHECK(card->widgetForTest(spx::Param::CombPhase)->isVisible());
	CHECK(card->widgetForTest(spx::Param::OddEvenBalance)->isVisible());
	// AND THE WINDOW HAS EVERY ONE THE CARD DROPPED.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	for (const spx::Param p : { spx::Param::Resolution, spx::Param::KeyTrack, spx::Param::Poly, spx::Param::KeyOffset,
	                            spx::Param::VocodeOn, spx::Param::VocodeCarrier, spx::Param::PartialsOn, spx::Param::PartialsMode }) {
		CAPTURE(static_cast<int>(p));
		CHECK(window->boxWidgetForTest(p) != nullptr);
	}
	using C = feathers::SpectralControl;
	for (const C c : { C::FilterOn, C::CombOn, C::FormantOn, C::TiltOn, C::OddEvenOn, C::FilterMode }) {
		CAPTURE(static_cast<int>(c));
		CHECK(window->frameForTest(0, c) != nullptr);
	}
}
TEST_CASE("Spectral's Offset reads the pitch it sets with Key Track off and semitones with it on (the window's strip)") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	auto* box = dynamic_cast<ui::ValueBox*>(window->boxWidgetForTest(spx::Param::KeyOffset));
	REQUIRE(box != nullptr);
	d.set(static_cast<size_t>(spx::Param::KeyOffset), 7.0f);
	(void)window->updateContent();
	CHECK(box->readout() == "G3 +7");
	d.set(static_cast<size_t>(spx::Param::KeyTrack), 1.0f);
	(void)window->updateContent();
	CHECK(box->readout() == "+7 st");
}
TEST_CASE("Spectral's card: the filter handle is the drawer's — it writes the device's cutoff as a harmonic of f0, and Notch moves it across only") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	d.set(static_cast<size_t>(spx::Param::FilterOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::KeyOffset), 12.0f); // f0 = C4: the corner is drawn an octave above its knob
	(void)d.card->updateContent();
	ui::SpectrumPlot& plot = d.card->plotForTest();
	using P = ui::SpectrumPlot;
	const auto grab = plot.handleForTest(P::Handle::Filter);
	REQUIRE(grab.has_value());
	CHECK(grab->x == doctest::Approx(plot.hzToX(2.0 * std::exp2(spx::kLog2Of2k))).epsilon(1e-4)); // 2 kHz knob, C4: at 4 kHz
	plot.pressForTest(*grab);
	plot.dragForTest({ plot.hzToX(1000.0), plot.dbToY(P::kFilterBaseDb + 0.5 * P::kFilterSpanDb) });
	plot.releaseForTest();
	// THE POINTER IS THE TRACKED CORNER: 1 kHz at f0 = C4 is a cutoff of 500 Hz before the tracking.
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(spx::Param::FilterCutoff)) == doctest::Approx(std::log2(500.0)).epsilon(1e-3));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(spx::Param::FilterResonance)) == doctest::Approx(0.5).epsilon(1e-3));
	d.set(static_cast<size_t>(spx::Param::FilterMode), 3.0f); // Notch
	(void)d.card->updateContent();
	CHECK_FALSE(plot.verticalForTest(P::Handle::Filter));
}
TEST_CASE("Spectral's window: the Draw tool paints the device's curve, stored in dB, the floor exact silence (the card has no Draw tool since C1)") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 1200.0f, 640.0f);
	(void)window->updateContent();
	ui::SpectrumPlot& plot = window->plotForTest();
	window->drawToolForTest().toggleForTest(); // the Draw tool on (a press on its chip)
	(void)window->updateContent();
	// BAND 3 IS HARMONIC 4 OF f0 (C3: 1046.5 Hz).
	const float x = plot.hzToX(4.0 * kC3);
	plot.pressForTest({ x, plot.dbToY(-47.0) });
	plot.releaseForTest();
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(spx::Param::CurveOn)) == 1.0f); // a stroke switches it on
	std::array<float, spx::kCurveBlockFloats> curve {};
	REQUIRE(d.f.deviceHost.blockAt(d.device, 1, curve)); // THE CURVE IS THE HAND'S BLOCK (spectral_curve_db), not the row
	CHECK(curve[3] == spx::kCurveFloorDb);               // silence
	plot.pressForTest({ x, plot.dbToY(6.0) });
	plot.releaseForTest();
	REQUIRE(d.f.deviceHost.blockAt(d.device, 1, curve));
	CHECK(curve[3] == doctest::Approx(6.0).epsilon(1e-2));
	CHECK(d.f.deviceHost.fieldsPresentAt(d.device) <= spx::kParamCount); // and the row carries no curve
}
TEST_CASE("Spectral's window: a drawn curve's undo and redo put the hand's block back exactly, whole") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	auto& source = window->sourceForTest();
	const auto block = [&] {
		std::array<float, spx::kCurveBlockFloats> db {};
		(void)d.f.deviceHost.blockAt(d.device, 1, db);
		return db;
	};
	ui::EditHistory history;
	const std::array<float, spx::kCurveBlockFloats> flat {};
	REQUIRE(block() == flat); // nothing drawn: flat, as the editor draws it
	// A STROKE: several band writes, then ONE record over the whole curve (what the plot's gesture does).
	const spectral::DrawnCurve before = source.curve(0);
	source.setCurveBand(0, 3, 0.0f);   // the floor: exact silence
	source.setCurveBand(0, 7, 2.0f);   // +6 dB
	source.setCurveBand(0, 8, 0.25f);  // -12 dB
	const spectral::DrawnCurve after = source.curve(0);
	const std::array<float, spx::kCurveBlockFloats> drawn = block();
	CHECK(drawn[3] == spx::kCurveFloorDb);
	CHECK(drawn[7] == doctest::Approx(6.0206).epsilon(1e-3));
	CHECK(drawn[8] == doctest::Approx(-12.0412).epsilon(1e-3));
	history.record(std::make_unique<ui::CurveEdit>(source.curveWriter(0), 0, before, after, "Draw"));
	history.undo();
	CHECK(block() == flat);
	history.redo();
	CHECK(block() == drawn); // exactly the same floats, bit for bit
	history.undo();
	history.redo();
	CHECK(block() == drawn);
	// FLATTEN IS ONE WRITE TOO.
	source.flattenCurve(0);
	CHECK(block() == flat);
}
TEST_CASE("Spectral's card lays out at the lane width: every shown control inside the body, none overlapping, in every state and at 1.75x") {
	// THE AUDITION OF 2026-10-13: the filter's controls clipped each other. The sweep that would have caught it.
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	REQUIRE(d.card->laneHeightAt(d.card->width()) == doctest::Approx(ui::SpectralCard::openHeight()));
	const float was = d.card->dpiScale();
	for (const float factor : { 1.0f, 1.75f }) {
		CAPTURE(factor);
		d.f.root->setDpiScale(was * factor);
		d.card->resized();
		for (int state = 0; state < 8; ++state) {
			CAPTURE(state);
			d.set(static_cast<size_t>(spx::Param::FilterOn), (state & 1) != 0 ? 1.0f : 0.0f);
			d.set(static_cast<size_t>(spx::Param::CombOn), (state & 2) != 0 ? 1.0f : 0.0f);
			d.set(static_cast<size_t>(spx::Param::OddEvenOn), (state & 4) != 0 ? 1.0f : 0.0f);
			(void)d.card->updateContent();
			std::vector<visage::Frame*> shown;
			for (visage::Frame* w : d.card->widgetsForTest()) {
				if (w->isVisible() && w->width() > 0.0f && w->height() > 0.0f) {
					shown.push_back(w);
				}
			}
			// THE PLOT, MIX, AND ONE KNOB PER OPERATOR SWITCHED ON (C1: the card hides an off operator's knob, and has no switches).
			REQUIRE(shown.size() >= 2 + static_cast<size_t>(((state & 1) != 0) + ((state & 2) != 0) + ((state & 4) != 0)));
			const float bodyRight = d.card->bodyLeft() + d.card->bodyWidth();
			for (size_t i = 0; i < shown.size(); ++i) {
				const visage::Bounds b = shown[i]->bounds();
				CHECK(b.x() >= d.card->bodyLeft() - 0.5f);
				CHECK(b.right() <= bodyRight + 0.5f);
				CHECK(b.bottom() <= d.card->laneHeightAt(d.card->width()) + 0.5f);
				CHECK(static_cast<float>(shown[i]->nativeWidth()) == doctest::Approx(b.width() * was * factor).epsilon(0.05));
				for (size_t j = i + 1; j < shown.size(); ++j) {
					CHECK_FALSE(overlaps(b, shown[j]->bounds()));
				}
			}
		}
	}
	d.f.root->setDpiScale(was);
}
TEST_CASE("Spectral's window is the Additive Spectrum panel over the device, DENSE: stacks of pills, menus, Tone, the curve on the plot, the strip") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	auto* panel = dynamic_cast<ui::SpectrumPanel*>(window);
	REQUIRE(panel != nullptr); // THE SAME CLASS the drawer's panel is
	window->setBounds(0.0f, 0.0f, 1200.0f, 640.0f);
	(void)window->updateContent();
	using C = feathers::SpectralControl;
	// THE DEVICE'S OPERATORS KEEP THEIR CHIPS (the section chips stay, by design); TILT AND ODD/EVEN ARE TONE'S PILLS, EACH ITS OWN
	// SWITCH; THE ONES THE DEVICE LACKS HAVE NOTHING.
	for (const C sw : { C::FilterOn, C::CombOn, C::FormantShiftOn, C::FormantOn }) {
		CHECK(panel->switchForTest(0, sw) != nullptr);
	}
	for (const C sw : { C::TiltOn, C::OddEvenOn }) {
		CHECK(panel->switchForTest(0, sw) == nullptr);
		ui::FaderPill* pill = panel->pillForTest(0, sw);
		REQUIRE(pill != nullptr);
		CHECK_FALSE(pill->amountOnly()); // its click is the operator's switch
	}
	CHECK(panel->switchForTest(0, C::FormantShiftLink) != nullptr);
	for (const C sw : { C::PartialsOn, C::DampOn, C::StretchOn, C::ShiftOn }) {
		CHECK(panel->switchForTest(0, sw) == nullptr);
	}
	// THE FILTER KEEPS FULL KNOBS (backlog OG); THE OTHER AMOUNTS ARE AMOUNT-ONLY PILLS, NO KNOB: their section's chip is their switch.
	for (const C c : { C::FilterCutoff, C::FilterResonance, C::FilterSlope }) {
		CAPTURE(static_cast<int>(c));
		CHECK(panel->knobForTest(0, c) != nullptr);
		CHECK(panel->pillForTest(0, c) == nullptr);
	}
	for (const C c : { C::CombSpacing, C::CombDepth, C::CombPhase,
	                   C::FormantShiftLow, C::FormantShiftHigh, C::FormantShiftCrossover, C::FormantVowel, C::FormantDepth }) {
		CAPTURE(static_cast<int>(c));
		CHECK(panel->knobForTest(0, c) == nullptr);
		ui::FaderPill* pill = panel->pillForTest(0, c);
		REQUIRE(pill != nullptr);
		CHECK(pill->amountOnly());
	}
	CHECK(panel->knobForTest(0, C::FilterKeyTrack) == nullptr); // the device's cutoff always follows f0
	// FILTER MODE IS A MENU (all four on demand), not a segmented row.
	ui::ValueBox* mode = panel->modeBoxForTest(0);
	REQUIRE(mode != nullptr);
	CHECK(mode->isVisible());
	CHECK(mode->choiceMenuAvailable());
	// THE CURVE IS ON THE PLOT: its switch, Flatten and Draw lie inside the plot's bounds.
	const std::vector<visage::Frame*> overlay = panel->overlayForTest(0);
	REQUIRE(overlay.size() == 3);
	const visage::Bounds plot = panel->plotBounds();
	for (visage::Frame* f : overlay) {
		REQUIRE(f != nullptr);
		CHECK(f->isVisible());
		const visage::Bounds b = f->bounds();
		CHECK(b.x() >= plot.x());
		CHECK(b.y() >= plot.y());
		CHECK(b.right() <= plot.right());
		CHECK(b.bottom() <= plot.bottom());
	}
	// AND THE STRIP: Resolution (a menu), Key Track, Poly, Offset and Mix, above the plot.
	for (const spx::Param p : { spx::Param::Resolution, spx::Param::KeyTrack, spx::Param::Poly, spx::Param::KeyOffset, spx::Param::Mix }) {
		visage::Frame* w = window->boxWidgetForTest(p);
		REQUIRE(w != nullptr);
		CHECK(w->isVisible());
		CHECK(w->bounds().bottom() <= plot.y());
	}
	// THE MULTI-WAY CHOICES ARE MENUS: Resolution, Carrier, Stereo.
	for (const spx::Param p : { spx::Param::Resolution, spx::Param::VocodeCarrier, spx::Param::VocodeStereo }) {
		CAPTURE(static_cast<int>(p));
		auto* box = dynamic_cast<ui::ValueBox*>(window->boxWidgetForTest(p));
		REQUIRE(box != nullptr);
		CHECK(box->choiceMenuAvailable());
	}
	// GREYED, NOT HIDDEN: the filter off greys its pills; on, they are live; Notch greys Resonance; the device off greys all.
	CHECK_FALSE(panel->liveForTest(0, C::FilterCutoff));
	CHECK_FALSE(panel->liveForTest(0, C::FilterMode));
	d.set(static_cast<size_t>(spx::Param::FilterOn), 1.0f);
	(void)window->updateContent();
	CHECK(panel->liveForTest(0, C::FilterCutoff));
	CHECK(panel->liveForTest(0, C::FilterResonance));
	CHECK(panel->liveForTest(0, C::FilterMode));
	d.set(static_cast<size_t>(spx::Param::FilterMode), 3.0f);
	(void)window->updateContent();
	CHECK_FALSE(panel->liveForTest(0, C::FilterResonance));
	d.set(static_cast<size_t>(spx::Param::Enabled), 0.0f);
	(void)window->updateContent();
	CHECK_FALSE(panel->liveForTest(0, C::FilterCutoff));
	CHECK_FALSE(panel->liveForTest(0, C::TiltAmount));
}
TEST_CASE("Spectral's dense window gives the plot back more than a row: at 790x560 it is a row of cells taller than the four-row window's 112 px, Partials beside Vocode in row one") {
	// THE FOUR-ROW WINDOW (the box, Vocode, two operator rows) left the plot 112 px at 790x560 (captured on xvfb before D1).
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 790.0f, 560.0f);
	(void)window->updateContent();
	const float plot = window->plotBounds().height();
	MESSAGE("dense window, 790x560: " << window->rowsForTest() << " rows, plot " << plot << " px");
	CHECK(window->rowsForTest() == 2);
	CHECK(plot >= 112.0f + ui::SpectrumPanel::rowHeight());
	// C1: PARTIALS SITS BESIDE VOCODE IN THE FIRST ROW. With Zone and Widths in Vocode's head, it grew wide
	// enough to push Partials into row two; with them in a cell beside Vocode's knobs, both fit in row one.
	const float half = 0.5f * ui::SpectrumPanel::rowHeight();
	const float vocodeY = window->boxWidgetForTest(spx::Param::VocodeOn)->positionInWindow().y;
	const float partialsY = window->boxWidgetForTest(spx::Param::PartialsOn)->positionInWindow().y;
	CHECK(std::fabs(partialsY - vocodeY) < half);
	// ZONE AND WIDTHS ARE IN VOCODE'S ROW TOO (Z7: two mode cells side by side after Depth): below its head, never in another row.
	const float zoneY = window->boxWidgetForTest(spx::Param::VocodeZone)->positionInWindow().y;
	const float widthsY = window->boxWidgetForTest(spx::Param::VocodeWidthZone)->positionInWindow().y;
	CHECK(zoneY > vocodeY);
	CHECK(zoneY < vocodeY + ui::SpectrumPanel::rowHeight());
	CHECK(widthsY == doctest::Approx(zoneY));
	CHECK(window->boxWidgetForTest(spx::Param::VocodeWidthZone)->positionInWindow().x
	      >= window->boxWidgetForTest(spx::Param::VocodeZone)->positionInWindow().x + window->boxWidgetForTest(spx::Param::VocodeZone)->width());
}
TEST_CASE("OG: Spectral's window keeps FULL KNOBS in both filter sections and pill stacks everywhere else") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 790.0f, 560.0f);
	(void)window->updateContent();
	using C = SpectralControl;
	for (const C c : { C::FilterCutoff, C::FilterResonance, C::FilterSlope, C::FilterGain, C::Filter2Cutoff, C::Filter2Resonance,
	                   C::Filter2Slope, C::Filter2Gain }) {
		CAPTURE(static_cast<int>(c));
		CHECK(window->knobForTest(0, c) != nullptr);
		CHECK(window->pillForTest(0, c) == nullptr);
	}
	// THE OTHER SECTIONS ARE STILL PILLS: the change is the filter's alone.
	for (const C c : { C::CombSpacing, C::CombDepth, C::CombPhase }) {
		CAPTURE(static_cast<int>(c));
		CHECK(window->knobForTest(0, c) == nullptr);
		CHECK(window->pillForTest(0, c) != nullptr);
	}
	// AND THE TWO-ROW GEOMETRY HOLDS WITH THE KNOBS.
	CHECK(window->rowsForTest() == 2);
}
TEST_CASE("Z7: Spectral's Zone and Widths are the house's mode cells — the box is the value the mode reads (none in Even), and Free's keeps turning past 1") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 1200.0f, 640.0f);
	d.set(static_cast<size_t>(spx::Param::VocodeOn), 1.0f);
	(void)window->updateContent();
	struct Case {
		spx::Param mode, amount, free;
		std::span<const char* const> names;
		size_t freeIndex;
	};
	for (const Case& c : { Case { spx::Param::VocodeZone, spx::Param::VocodeZoneAmount, spx::Param::VocodeZoneFree, spx::kZoneNames, spx::kZoneFree },
	                       Case { spx::Param::VocodeWidthZone, spx::Param::VocodeWidthAmount, spx::Param::VocodeWidthFree, spx::kWidthZoneNames,
	                              spx::kWidthZoneFree } }) {
		CAPTURE(static_cast<int>(c.mode));
		auto* cell = dynamic_cast<ui::ModeValueCell*>(window->boxWidgetForTest(c.mode));
		REQUIRE(cell != nullptr);
		// THE DROPDOWN IS THE MODE: every name in order, Free last.
		REQUIRE(cell->select().zoneCount() == c.names.size());
		for (size_t i = 0; i < c.names.size(); ++i) {
			CHECK(cell->select().nameOf(i) == c.names[i]);
		}
		CHECK(std::string(c.names[c.freeIndex]) == "Free");
		// THE BOX: none in Even (nothing to set), the fenced amount in each named mode, Free's coordinate in Free.
		for (size_t m = 0; m < c.names.size(); ++m) {
			CAPTURE(m);
			d.set(static_cast<size_t>(c.mode), static_cast<float>(m));
			(void)window->updateContent();
			const long want = m == 0 ? -1L : m == c.freeIndex ? static_cast<long>(c.free) : static_cast<long>(c.amount);
			CHECK(cell->activeParam() == want);
		}
		// FREE'S COORDINATE IS STORED UNCLAMPED, past 1 and below 0 (the named amount stays fenced 0..1).
		d.set(static_cast<size_t>(c.free), 7.5f);
		d.set(static_cast<size_t>(c.amount), 3.0f);
		const spx::Settings s = window->sourceForTest().settings();
		CHECK((c.mode == spx::Param::VocodeZone ? s.vocodeZoneFree : s.vocodeWidthFree) == 7.5f);
		CHECK((c.mode == spx::Param::VocodeZone ? s.vocodeZoneAmount : s.vocodeWidthAmount) == 1.0f);
		d.set(static_cast<size_t>(c.free), -12.25f);
		const spx::Settings t = window->sourceForTest().settings();
		CHECK((c.mode == spx::Param::VocodeZone ? t.vocodeZoneFree : t.vocodeWidthFree) == -12.25f);
		d.set(static_cast<size_t>(c.mode), 0.0f);
	}
}
TEST_CASE("Z7c: Spectral's Offset box moves 12 semitones a full drag, a fifth of that fine — and Free's coordinate box keeps the house's digit road") {
	// MEASURED BEFORE: no span, and "C3 +0" is not a readout of the value, so a drag reached the whole declared range — 72
	// semitones a full drag, 0.51 st/px — which is why the Offset knob felt twitchy.
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 1200.0f, 640.0f);
	(void)window->updateContent();
	auto* offset = dynamic_cast<ui::ValueBox*>(window->boxWidgetForTest(spx::Param::KeyOffset));
	REQUIRE(offset != nullptr);
	CHECK(offset->dragSpanForTest() == doctest::Approx(ui::SpectralExpanded::kOffsetDragSpan));
	const auto drag = [&](ui::ValueBox* box, float pixels, bool fine) {
		const float x = box->width() * 0.5f;
		const float y = box->height() * 0.5f;
		box->onMouseDown().callback(fine ? fineAt(x, y) : at(x, y));
		box->onMouseDrag().callback(fine ? fineAt(x, y - pixels) : at(x, y - pixels));
		box->onMouseUp().callback(fine ? fineAt(x, y - pixels) : at(x, y - pixels));
	};
	const auto keyOffset = [&] { return static_cast<double>(window->sourceForTest().settings().keyOffset); };
	// THE VALUE A HAND GETS, not the configuration: half a drag up, then a full drag down, each from where the last left it.
	drag(offset, 0.5f * ui::ValueBox::kDragRange, false);
	CHECK(keyOffset() == doctest::Approx(6.0).epsilon(0.01));
	drag(offset, -ui::ValueBox::kDragRange, false);
	CHECK(keyOffset() == doctest::Approx(-6.0).epsilon(0.01));
	// FINE (Shift): a fifth.
	drag(offset, ui::ValueBox::kDragRange, true);
	CHECK(keyOffset() == doctest::Approx(-6.0 + 12.0 * ui::ValueBox::kFineFactor).epsilon(0.01));
	// WITH KEY TRACK ON the readout is semitones ("+7"), and the declared span still outranks the digit under the press.
	d.set(static_cast<size_t>(spx::Param::KeyTrack), 1.0f);
	d.set(static_cast<size_t>(spx::Param::KeyOffset), 0.0f);
	(void)window->updateContent();
	drag(offset, ui::ValueBox::kDragRange, false);
	CHECK(keyOffset() == doctest::Approx(12.0).epsilon(0.01));
	// FREE'S COORDINATE: no span of its own, as OWLTT's coordinate boxes — the house's digit road (a press on a digit moves ten of
	// that digit a full drag; elsewhere ten of the finest), never the 2^24 declared range.
	d.set(static_cast<size_t>(spx::Param::VocodeOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::VocodeZone), static_cast<float>(spx::kZoneFree));
	(void)window->updateContent();
	auto* free = dynamic_cast<ui::ValueBox*>(window->boxWidgetForTest(spx::Param::VocodeZoneFree));
	REQUIRE(free != nullptr);
	CHECK(free->dragSpanForTest() == 0.0);
	const double before = static_cast<double>(window->sourceForTest().settings().vocodeZoneFree);
	drag(free, ui::ValueBox::kDragRange, false);
	const double moved = static_cast<double>(window->sourceForTest().settings().vocodeZoneFree) - before;
	MESSAGE("Free coordinate, one full drag from the box's centre: " << moved);
	CHECK(moved > 0.05);
	CHECK(moved <= 10.0 + 1e-6);
}
TEST_CASE("Z7c: Spectral's cutoff handle reaches the plot's right edge and the control's top, and dragging past the edge holds it there") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 1200.0f, 640.0f);
	d.set(static_cast<size_t>(spx::Param::FilterOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::FilterCutoff), static_cast<float>(spx::kLog2Of20k));
	(void)window->updateContent();
	ui::SpectrumPlot& plot = window->plotForTest();
	const auto handle = plot.handleForTest(ui::SpectrumPlot::Handle::Filter);
	REQUIRE(handle.has_value());
	const float right = plot.width() - ui::SpectrumPlot::kPadRight;
	MESSAGE("cutoff at its top: handle x " << handle->x << ", plot right edge " << right);
	CHECK(handle->x == doctest::Approx(right).epsilon(1e-4));
	// FROM THE DEFAULT, A DRAG FAR PAST THE RIGHT EDGE LANDS ON THE CONTROL'S TOP, exactly.
	d.set(static_cast<size_t>(spx::Param::FilterCutoff), static_cast<float>(spx::kLog2Of2k));
	(void)window->updateContent();
	const auto at2k = plot.handleForTest(ui::SpectrumPlot::Handle::Filter);
	REQUIRE(at2k.has_value());
	plot.pressForTest(*at2k);
	plot.dragForTest({ right + 200.0f, at2k->y });
	plot.releaseForTest();
	CHECK(window->sourceForTest().settings().filterCutoff == doctest::Approx(spx::kLog2Of20k).epsilon(1e-6));
}
TEST_CASE("Z7d: Spectral greys Resonance and Slope while its filter sits at its open end — the window's pills and the card's Slope, Notch at both ends (Z9b)") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 1200.0f, 640.0f);
	using C = SpectralControl;
	d.set(static_cast<size_t>(spx::Param::FilterOn), 1.0f);
	struct At {
		float mode, cutoff, offset, keyTrack;
		bool open;
	};
	for (const At a : { At { 0.0f, static_cast<float>(spx::kLog2Of20k), 0.0f, 0.0f, true },  // Low Pass at its top
	                    At { 0.0f, static_cast<float>(spx::kLog2Of2k), 0.0f, 0.0f, false },  // well inside
	                    At { 1.0f, static_cast<float>(spx::kLog2Of20), 0.0f, 0.0f, true },   // High Pass at its bottom
	                    At { 2.0f, static_cast<float>(spx::kLog2Of20k), 0.0f, 0.0f, false }, // Band Pass has no open end
	                    At { 3.0f, static_cast<float>(spx::kLog2Of20k), 0.0f, 0.0f, true },  // Notch opens at the top (Z9b)
	                    At { 3.0f, static_cast<float>(spx::kLog2Of20), 0.0f, 0.0f, true },   // and at the bottom
	                    // THE CORNER'S ROAD: Offset +12 puts the corner an octave above an octave-short control — open, with Key
	                    // Track off (f0 is fixed); with it on the note decides, so the face goes by the control alone.
	                    At { 0.0f, static_cast<float>(spx::kLog2Of20k - 1.0), 12.0f, 0.0f, true },
	                    At { 0.0f, static_cast<float>(spx::kLog2Of20k - 1.0), 12.0f, 1.0f, false } }) {
		CAPTURE(a.mode);
		CAPTURE(a.cutoff);
		CAPTURE(a.offset);
		CAPTURE(a.keyTrack);
		d.set(static_cast<size_t>(spx::Param::FilterMode), a.mode);
		d.set(static_cast<size_t>(spx::Param::FilterCutoff), a.cutoff);
		d.set(static_cast<size_t>(spx::Param::KeyOffset), a.offset);
		d.set(static_cast<size_t>(spx::Param::KeyTrack), a.keyTrack);
		(void)window->updateContent();
		(void)d.card->updateContent();
		CHECK(window->liveForTest(0, C::FilterResonance) == !a.open);
		CHECK(window->liveForTest(0, C::FilterSlope) == !a.open);
		CHECK(window->liveForTest(0, C::FilterCutoff)); // the way back out
		auto* slope = dynamic_cast<ui::Knob*>(d.card->widgetForTest(spx::Param::FilterSlope));
		REQUIRE(slope != nullptr);
		CHECK(slope->active() == !a.open);
		// AND THE ENGINE AGREES: at the open end the device's gains are exactly 1 whatever Resonance and Slope say.
		if (a.open && a.keyTrack == 0.0f) {
			spx::Settings s = window->sourceForTest().settings();
			const double f0 = spx::shaperF0(s, false, 0.0);
			CHECK(feathers::spectral::prepare(spx::shaperParamsOf(s, f0), f0).identity);
		}
	}
}
TEST_CASE("Spectral's window: at every width it can be given, every control lies inside it and none overlaps another or the plot") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	for (const float w : { ui::SpectralExpanded::kMinimumWidth, 900.0f, 1240.0f }) {
		CAPTURE(w);
		window->setBounds(0.0f, 0.0f, w, ui::SpectralExpanded::kMinimumHeight);
		(void)window->updateContent();
		const std::vector<visage::Frame*> controls = window->controlsForTest(0);
		const visage::Bounds plot = window->plotBounds();
		CHECK(plot.height() > 80.0f);
		// THE CURVE'S OVERLAY IS ON THE PLOT BY DESIGN: inside it, clear of each other.
		const std::vector<visage::Frame*> overlay = window->overlayForTest(0);
		for (size_t i = 0; i < overlay.size(); ++i) {
			const visage::Bounds b = overlay[i]->bounds();
			CHECK(b.x() >= plot.x());
			CHECK(b.right() <= plot.right());
			for (size_t j = i + 1; j < overlay.size(); ++j) {
				CHECK_FALSE(overlaps(b, overlay[j]->bounds()));
			}
		}
		for (size_t i = 0; i < controls.size(); ++i) {
			const visage::Bounds b = controls[i]->bounds();
			CHECK(b.x() >= 0.0f);
			CHECK(b.right() <= w + 0.5f);
			CHECK(b.bottom() <= ui::SpectralExpanded::kMinimumHeight + 0.5f);
			CHECK_FALSE(overlaps(b, plot));
			for (size_t j = i + 1; j < controls.size(); ++j) {
				CHECK_FALSE(overlaps(b, controls[j]->bounds()));
			}
		}
	}
}
TEST_CASE("Spectral's Formant Shift High and Crossover handles are offered only while the Link is off, on the card's plot and the window's") {
	namespace spx = feathers::fx::spectral;
	using H = ui::SpectrumPlot::Handle;
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::SpectralExpanded panel { d.f.deviceHost, registry, d.device };
	d.set(static_cast<size_t>(spx::Param::FShiftOn), 1.0f);
	// THE ENGINE READS THE SAME RULE (spectralControlInert): linked, High and the crossover change nothing, so the plot has no handle for them.
	for (const bool linked : { true, false }) {
		CAPTURE(linked);
		d.set(static_cast<size_t>(spx::Param::FShiftLink), linked ? 1.0f : 0.0f);
		d.card->updateContent();
		panel.updateContent();
		for (ui::SpectrumPlot* plot : { &d.card->plotForTest(), &panel.plotForTest() }) {
			CHECK(plot->handleForTest(H::ShiftLow).has_value());
			CHECK(plot->handleForTest(H::ShiftHigh).has_value() == !linked);
			CHECK(plot->handleForTest(H::Crossover).has_value() == !linked);
		}
	}
}
TEST_CASE("NOISE's plot hover names the routed drag while a modulator is armed on Cutoff") {
	namespace nz = feathers::fx::noise;
	DynCardFixture<ui::NoiseCard> d(nz::kTraits.tag);
	ui::NoisePlot* plot = d.card->plotForTest();
	REQUIRE(plot != nullptr);
	const visage::Point at { plot->width() * 0.5f, plot->height() * 0.2f };
	// ARMED THROUGH THE REAL ARROW: the Cutoff is routable, so the drag routes its amount.
	d.f.arm(backplane::Source::core(0));
	CHECK(ui::hoverText(*plot, at)
	      == "Drag up or down: how much of Cutoff's modulation routes (Alt-drag places Cutoff across and Resonance up instead)");
}
TEST_CASE("Spectral's routing choice is shown only while both filters are on, in the window, for every combination of the two") {
	namespace spx = feathers::fx::spectral;
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::SpectralExpanded panel { d.f.deviceHost, registry, d.device };
	// Both filters' chips are the one control; the shown tab's is visible, the other is hidden with its panel.
	const auto shownWith = [&](float filter1, float filter2) {
		d.set(static_cast<size_t>(spx::Param::FilterOn), filter1);
		d.set(static_cast<size_t>(spx::Param::Filter2On), filter2);
		panel.updateContent();
		CAPTURE(filter1);
		CAPTURE(filter2);
		return std::make_pair(panel.routingForTest(0, 0).isVisible(), panel.routingForTest(0, 1).isVisible());
	};
	// THE TWO FILTERS OFF, ONE AT A TIME, AND BOTH: only the last shows the choice.
	CHECK(shownWith(0.0f, 0.0f) == std::make_pair(false, false));
	CHECK(shownWith(1.0f, 0.0f) == std::make_pair(false, false));
	CHECK(shownWith(0.0f, 1.0f) == std::make_pair(false, false));
	// The tab shows filter 1's panel, so only its chip is in the row; filter 2's waits behind its tab.
	CHECK(shownWith(1.0f, 1.0f) == std::make_pair(true, false));
	// AND BACK: a switch moved off hides it again at once.
	CHECK(shownWith(1.0f, 0.0f) == std::make_pair(false, false));
}
TEST_CASE("Spectral's mode-dependent hover text follows the Filter, Filter 2, Vocode and Key Track, on the card and in the window") {
	namespace spx = feathers::fx::spectral;
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::SpectralExpanded panel { d.f.deviceHost, registry, d.device };
	// THE CARD'S SLOPE IS A KNOB; ITS RESONANCE IS A PLOT HANDLE, which answers from the source's `tooltip` (read below).
	auto* slope = dynamic_cast<ui::Knob*>(d.card->widgetForTest(spx::Param::FilterSlope));
	REQUIRE(slope != nullptr);
	const auto hover = [](ui::Knob* k) { return ui::hoverText(*k, visage::Point { k->width() * 0.5f, k->height() * 0.5f }); };
	const auto handleText = [&] {
		const char* t = panel.sourceForTest().tooltip(0, feathers::SpectralControl::FilterResonance);
		return std::string(t != nullptr ? t : "");
	};
	// THE CARD'S TEXT MAY CARRY THE CONTROL'S NAME IN FRONT, so the tail is read; THE WINDOW'S OWN CONTROLS ARE READ BY THEIR OVERRIDE.
	const auto tailIs = [](const std::string& text, const char* want) {
		const std::string tail(want);
		return text.size() >= tail.size() && text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
	};
	const auto windowText = [&](spx::Param p) {
		const char* own = panel.sourceForTest().deviceControl(p).tooltipOverride();
		return std::string(own != nullptr ? own : "");
	};
	const auto refresh = [&] {
		d.card->updateContent();
		panel.updateContent();
	};

	// LOW PASS: the filter's peak and its falloff, as the plain words say.
	d.set(static_cast<size_t>(spx::Param::FilterMode), 0.0f);
	refresh();
	CHECK(tailIs(handleText(),"A peak drawn at the cutoff"));
	CHECK(tailIs(hover(slope), "How steeply the filter falls, from gentle (6 dB/oct) to a brick wall (96 dB/oct)"));
	CHECK(windowText(spx::Param::FilterResonance) == "A peak drawn at the cutoff");

	// BELL: the same two controls are the Bell's width and its sides.
	d.set(static_cast<size_t>(spx::Param::FilterMode), 4.0f);
	refresh();
	CHECK(tailIs(handleText(),"How wide the Bell is, from 3 octaves down to a sixth of one"));
	CHECK(tailIs(hover(slope), "How steep the Bell's sides are: a classic bell at the default, softer below, a flat-topped band above"));
	CHECK(windowText(spx::Param::FilterSlope)
	      == "How steep the Bell's sides are: a classic bell at the default, softer below, a flat-topped band above");

	// KEY TRACK moves where Key Offset is measured from; VOCODE moves whose envelope Formant Shift follows. Both are in the window.
	d.set(static_cast<size_t>(spx::Param::KeyTrack), 1.0f);
	refresh();
	CHECK(windowText(spx::Param::KeyOffset)
	      == "Moves the fundamental the comb, Odd/Even and the curve are measured in, in semitones: from the played note");
	d.set(static_cast<size_t>(spx::Param::KeyTrack), 0.0f);
	refresh();
	CHECK(windowText(spx::Param::KeyOffset)
	      == "Moves the fundamental the comb, Odd/Even and the curve are measured in, in semitones: from C3 (so C0 to C6)");
	d.set(static_cast<size_t>(spx::Param::VocodeOn), 1.0f);
	refresh();
	CHECK(tailIs(windowText(spx::Param::FShiftOn), "using the sidechain's envelope"));
	d.set(static_cast<size_t>(spx::Param::VocodeOn), 0.0f);
	refresh();
	CHECK(tailIs(windowText(spx::Param::FShiftOn), "using the sound's own envelope"));
}
TEST_CASE("Spectral's Vocode: the card says sidechain silent while it is on and nothing sounds there, and the plot draws the sidechain's envelope while vocoding") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::SpectralCard* card = d.card;
	d.set(static_cast<size_t>(spx::Param::VocodeOn), 1.0f);
	(void)card->updateContent();
	CHECK(card->badgeForTest().silent()); // no readings at all: nothing has sounded
	// READINGS FROM THE DEVICE: a sidechain sounding, its envelope a slope.
	std::array<float, spx::kReadingCount> r {};
	r.fill(spx::kBandFloorDb);
	r[spx::kReadingF0] = static_cast<float>(kC3);
	r[spx::kReadingSideSounding] = 1.0f;
	for (int b = 0; b < spx::kSpectrumBands; ++b) {
		r[spx::kReadingSideEnvelope + static_cast<size_t>(b)] = -20.0f - 0.5f * static_cast<float>(b);
	}
	d.publish(r);
	(void)card->updateContent();
	CHECK_FALSE(card->badgeForTest().silent());
	// THE OVERLAY: the device's source answers with the envelope's points, which the plot draws over the bars.
	ui::WidgetRegistry registry;
	spx::DeviceSpectrumSource source(d.f.deviceHost, registry, d.device);
	std::array<float, 64> hz {}, db {};
	const int n = source.overlay(hz.data(), db.data(), 64);
	REQUIRE(n == spx::kSpectrumBands);
	CHECK(db[0] == doctest::Approx(-20.0f));
	CHECK(db[10] == doctest::Approx(-25.0f));
	CHECK(hz[0] == doctest::Approx(spx::bandCentreHz(0)));
	// VOCODE OFF: no overlay.
	d.set(static_cast<size_t>(spx::Param::VocodeOn), 0.0f);
	CHECK(source.overlay(hz.data(), db.data(), 64) == 0);
}
TEST_CASE("Spectral's Vocode is hidden under Poly, on the card and in the window") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	window->setBounds(0.0f, 0.0f, 1200.0f, 640.0f);
	d.set(static_cast<size_t>(spx::Param::VocodeOn), 1.0f); // the badge's line shows only while Vocode is on
	for (const float poly : { 0.0f, 1.0f }) {
		CAPTURE(poly);
		d.set(static_cast<size_t>(spx::Param::Poly), poly);
		(void)d.card->updateContent();
		(void)window->updateContent();
		CHECK(d.card->badgeForTest().isVisible() == (poly == 0.0f)); // C1: the card shows Vocode only by its badge's line
		CHECK(window->boxWidgetForTest(spx::Param::VocodeOn)->isVisible() == (poly == 0.0f));
	}
}
//------------------------------------------------------------------------------------------------//
// NOISE (backlog MZ) — its card around its picture. Visibility and the plotted numbers, as values. //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NOISE's plot hover names both of the controls it drives, and the Resonance part follows the Source") {
	namespace nz = feathers::fx::noise;
	DynCardFixture<ui::NoiseCard> d(nz::kTraits.tag);
	ui::NoisePlot* plot = d.card->plotForTest();
	REQUIRE(plot != nullptr);
	// A point above the plot's foot: the footer's clicks text does not apply there.
	const visage::Point at { plot->width() * 0.5f, plot->height() * 0.2f };
	// THE EXPECTED TEXT IS RESTATED HERE, LITERALLY. No modulator is armed, so a drag places both controls.
	// Noise source: Resonance rings the filter and nothing else.
	CHECK(ui::hoverText(*plot, at)
	      == "Drag across: Cutoff, where the filter acts. Drag up or down: Resonance, how sharply it rings");
	// Impulses source: the same two controls, and what high resonance does to the clicks.
	d.set(static_cast<size_t>(nz::Param::Source), 1.0f);
	CHECK(ui::hoverText(*plot, at)
	      == "Drag across: Cutoff, where the filter acts. Drag up or down: Resonance, how sharply it rings, and high "
	         "resonance turns the clicks into pitched pings");
}
TEST_CASE("NOISE's card shows a control exactly where the device reads it, through the gate chain") {
	namespace nz = feathers::fx::noise;
	using P = nz::Param;
	DynCardFixture<ui::NoiseCard> d(nz::kTraits.tag);
	ui::NoiseCard* card = d.card;
	const P gated[] = { P::Colour, P::Density, P::Sync, P::Timing, P::Jitter, P::Cluster, P::HitSize, P::Polarity,
		                P::Seed };
	int states = 0;
	for (const float source : { 0.0f, 1.0f }) {
		for (const float timing : { 0.0f, 1.0f, 2.0f }) {
			for (const float sync : { 0.0f, 5.0f }) {
				for (const float seedMode : { 0.0f, 1.0f }) {
					d.set(static_cast<size_t>(P::Source), source);
					d.set(static_cast<size_t>(P::Timing), timing);
					d.set(static_cast<size_t>(P::Sync), sync);
					d.set(static_cast<size_t>(P::SeedMode), seedMode);
					(void)card->updateContent();
					++states;
					CAPTURE(source);
					CAPTURE(timing);
					CAPTURE(sync);
					CAPTURE(seedMode);
					// THE EXPECTATION, RESTATED BY HAND from the design — not read back from the descriptor's gates.
					const bool impulses = source == 1.0f;
					const auto expected = [&](P p) {
						switch (p) {
						case P::Colour: return !impulses;
						case P::Density: return impulses && sync == 0.0f;
						case P::Jitter: return impulses && timing == 0.0f;
						case P::Cluster: return impulses && timing == 2.0f;
						case P::Seed: return seedMode == 0.0f;
						default: return impulses;
						}
					};
					for (const P p : gated) {
						CAPTURE(std::string(nz::kParamDescs[static_cast<size_t>(p)].name));
						visage::Frame* w = card->widgetForTest(p);
						REQUIRE(w != nullptr);
						CHECK(w->isVisible() == expected(p));
						CHECK(card->isLive(p) == expected(p));
					}
					// THE COMMON FACE IS THERE IN EVERY STATE.
					for (const P p : { P::Source, P::Poly, P::Level, P::Stereo, P::FilterType, P::KeyTrack,
					                   P::SeedMode, P::Cutoff }) {
						CHECK(card->widgetForTest(p)->isVisible());
					}
				}
			}
		}
	}
	CHECK(states == 24);
	// AND THE CARD IS ONE HEIGHT FOR BOTH SOURCES: Noise's plot takes the impulse rows' room.
	d.set(static_cast<size_t>(P::Source), 0.0f);
	(void)card->updateContent();
	const float noisePlot = card->plotForTest()->height();
	d.set(static_cast<size_t>(P::Source), 1.0f);
	(void)card->updateContent();
	CHECK(card->plotForTest()->height() < noisePlot);
	// THE BODY AND THE OUTPUTS STRIP UNDER IT — the strip is what the three output chips hang on, and it is not empty.
	REQUIRE(card->outputStripHeightAt(300.0f) > 0.0f);
	CHECK(card->laneHeightAt(300.0f)
	      == doctest::Approx(ui::NoiseCard::openHeight() + card->outputStripHeightAt(300.0f)));
}
TEST_CASE("NOISE's plot draws the installed spectrum and the generator's own hits") {
	namespace nz = feathers::fx::noise;
	using P = nz::Param;
	DynCardFixture<ui::NoiseCard> d(nz::kTraits.tag);
	ui::NoisePlot* plot = d.card->plotForTest();
	REQUIRE(plot != nullptr);
	const auto dbAt = [&](double hz) {
		size_t best = 0;
		for (size_t i = 1; i < plot->pointCountForTest(); ++i) {
			if (std::fabs(std::log(plot->hzAtForTest(i) / hz)) < std::fabs(std::log(plot->hzAtForTest(best) / hz))) {
				best = i;
			}
		}
		return plot->spectrumDbForTest(best);
	};
	// WHITE THROUGH AN OPEN FILTER (the default lowpass at 20 kHz) is flat at 0 dB through the midrange.
	d.set(static_cast<size_t>(P::Colour), 0.0f);
	(void)d.card->updateContent();
	CHECK(std::fabs(dbAt(1000.0)) < 0.2);
	CHECK(std::fabs(dbAt(200.0)) < 0.2);
	// BROWN FALLS 6 dB AN OCTAVE across the picture, as the device's tilt does.
	d.set(static_cast<size_t>(P::Colour), 1.0f);
	(void)d.card->updateContent();
	CHECK(std::fabs(dbAt(2000.0) - dbAt(500.0) + 12.04) < 1.0);
	CHECK_FALSE(plot->showsHitsForTest());

	// IMPULSES, REGULAR WITH NO JITTER AT 8 A SECOND: sixteen hits in the strip's two seconds, an eighth of a second
	// apart, each of size 1 — the device's generator, from the device's settings.
	d.set(static_cast<size_t>(P::Source), 1.0f);
	d.set(static_cast<size_t>(P::Timing), 0.0f);
	d.set(static_cast<size_t>(P::Jitter), 0.0f);
	d.set(static_cast<size_t>(P::Density), 3.0f);
	(void)d.card->updateContent();
	CHECK(plot->showsHitsForTest());
	const std::vector<float>& times = plot->hitTimesForTest();
	const std::vector<float>& values = plot->hitValuesForTest();
	CHECK(times.size() == 16);
	for (size_t i = 1; i < times.size(); ++i) {
		CHECK(times[i] - times[i - 1] == doctest::Approx(0.125).epsilon(0.002));
	}
	for (const float v : values) {
		CHECK(v == 1.0f);
	}
	// RANDOM POLARITY: some hits go down, and the times do not move (the core's fixed draw order).
	const std::vector<float> before = times;
	d.set(static_cast<size_t>(P::Polarity), 1.0f);
	(void)d.card->updateContent();
	size_t negative = 0;
	for (const float v : plot->hitValuesForTest()) {
		negative += v < 0.0f ? 1 : 0;
	}
	CHECK(negative > 0);
	const bool sameTimes = plot->hitTimesForTest() == before;
	CHECK(sameTimes);
}

namespace {
//------------------------------------------------------------------------------------------------//
// RANDOM TRIGGER (backlog NA (a)) — the card drawn around events                                   //
// SAMPLE & HOLD (backlog MW)                                                                      //
//------------------------------------------------------------------------------------------------//

namespace rtg = feathers::mod::rtrig;
struct RtrigSeat {
	size_t slot = 0;
	ui::RandomTriggerCard* card = nullptr;
};
RtrigSeat seatRtrig(ModFixture& f) {
	RtrigSeat seat;
	seat.slot = f.bridge.addRackDevice(rtg::kTraits.tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::RandomTriggerCard*>(
	    dynamic_cast<const ui::RandomTriggerCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
/// PUBLISH THE DEVICE'S READING as the engine's telemetry does: the hit count and the newest size on the playhead,
/// the Decay's magnitude beside it.
void publishRtrig(ModFixture& f, const RtrigSeat& seat, uint32_t hits, float size, float decay) {
	feathers::mod::Playhead where;
	where.ordinal = hits;
	where.phase = 0.5f * (size + 1.0f);
	where.level = decay;
	where.haveLevel = true;
	f.bridge.publishRackPlayhead(seat.slot, where);
}
void setSampleHold(ModFixture& f, const SampleHoldSeat& seat, snh::Param p, double v) {
	seat.card->control(static_cast<size_t>(p)).setValue(v);
	f.bridge.refresh();
	(void)seat.card->updateContent();
}
/// THE ROW'S PATTERN FLOATS, AS THE HOST HOLDS THEM — compared to the bit.
std::vector<uint32_t> patternBits(ModFixture& f, DeviceInstanceId device) {
	std::vector<uint32_t> out;
	for (size_t i = ssq::kParamCount; i < ssq::kParamCount + ssq::kExtraFloats; ++i) {
		const float v = i < f.deviceHost.fieldsPresentAt(device) ? f.deviceHost.fieldAt(device, i) : 0.0f;
		out.push_back(std::bit_cast<uint32_t>(v));
	}
	return out;
}
} // namespace

TEST_CASE("NA: the Random Trigger card's timeline is the engine's hit count and sizes, one column a frame") {
	ModFixture f;
	const RtrigSeat seat = seatRtrig(f);
	ui::RandomTriggerCard& card = *seat.card;
	// THE FIRST READING ONLY SETS THE COUNT: hits made before the card looked are not a column's.
	publishRtrig(f, seat, 40, 0.5f, 0.25f);
	card.sampleForTest();
	REQUIRE(card.timelineCountForTest() == 1);
	CHECK(card.timelineAtForTest(0).hits == 0);
	// THREE HITS SINCE, THE NEWEST -0.5 WITH ITS DECAY AT 0.25: the column says three, -0.5, and the decay DOWN.
	publishRtrig(f, seat, 43, -0.5f, 0.25f);
	card.sampleForTest();
	CHECK(card.timelineAtForTest(0).hits == 3);
	CHECK(card.timelineAtForTest(0).size == -0.5f);
	CHECK(card.timelineAtForTest(0).decay == -0.25f);
	// NONE SINCE: an empty column, the size still the newest hit's, the decay falling.
	publishRtrig(f, seat, 43, -0.5f, 0.125f);
	card.sampleForTest();
	CHECK(card.timelineAtForTest(0).hits == 0);
	CHECK(card.timelineAtForTest(0).decay == -0.125f);
	CHECK(card.timelineAtForTest(1).hits == 3);
	// THE COUNT WRAPS AT 31 BITS (the playhead's idle ordinal is kept out of reach), and a wrap is still one hit.
	publishRtrig(f, seat, 0x7FFFFFFFu, 1.0f, 1.0f);
	card.sampleForTest();
	publishRtrig(f, seat, 0u, 1.0f, 1.0f);
	card.sampleForTest();
	CHECK(card.timelineAtForTest(0).hits == 1);
}
TEST_CASE("NA: a frame that saw several Random Trigger hits draws every one's size, from the device's newest hits") {
	ModFixture f;
	const RtrigSeat seat = seatRtrig(f);
	ui::RandomTriggerCard& card = *seat.card;
	const auto publish = [&](uint32_t hits, std::vector<feathers::device::RecentEvent> recent) {
		feathers::mod::Playhead where;
		where.ordinal = hits;
		where.phase = 0.5f * ((recent.empty() ? 1.0f : recent.front().value) + 1.0f); // the newest size, else 1
		where.level = 0.0f;
		where.haveLevel = true;
		for (size_t i = 0; i < recent.size(); ++i) {
			where.recent[i] = recent[i];
		}
		f.bridge.publishRackPlayhead(seat.slot, where);
	};
	publish(10, { { 10, 0.5f }, { 9, 0.25f } });
	card.sampleForTest(); // the first reading only sets the count
	// THREE HITS SINCE (11, 12, 13), the ring also holding 10 and 9 an earlier frame drew: exactly the three, newest first.
	publish(13, { { 13, -0.75f }, { 12, 0.5f }, { 11, 1.0f }, { 10, 0.5f }, { 9, 0.25f } });
	card.sampleForTest();
	const ui::RandomTriggerCard::Column c = card.timelineAtForTest(0);
	CHECK(c.hits == 3u);
	REQUIRE(c.sized == 3u);
	CHECK(c.sizes[0] == -0.75f);
	CHECK(c.sizes[1] == 0.5f);
	CHECK(c.sizes[2] == 1.0f);
	CHECK(c.size == -0.75f); // the newest is still the newest
	// NONE SINCE: no stems.
	card.sampleForTest();
	CHECK(card.timelineAtForTest(0).sized == 0u);
	// A HOST THAT KEEPS NO EVENTS: one stem at the newest size, as before.
	publish(14, {});
	card.sampleForTest();
	REQUIRE(card.timelineAtForTest(0).sized == 1u);
	CHECK(card.timelineAtForTest(0).sizes[0] == 1.0f);
}
TEST_CASE("NA: the Random Trigger card shows a control exactly where the device reads it") {
	ModFixture f;
	const RtrigSeat seat = seatRtrig(f);
	ui::RandomTriggerCard& card = *seat.card;
	const auto shown = [&](rtg::Param p) {
		visage::Frame* w = card.widgetForTest(p);
		REQUIRE(w != nullptr);
		return w->isVisible();
	};
	const auto set = [&](rtg::Param p, double v) {
		card.control(p).setValue(v);
		f.bridge.refresh();
		card.update();
	};
	// THE DEFAULT: Poisson, Free, Stable — Density and Seed on the face, Jitter and Cluster not.
	CHECK(shown(rtg::Param::Density));
	CHECK(shown(rtg::Param::Seed));
	CHECK_FALSE(shown(rtg::Param::Jitter));
	CHECK_FALSE(shown(rtg::Param::Cluster));
	set(rtg::Param::Timing, static_cast<double>(dsp::EventTiming::Regular));
	CHECK(shown(rtg::Param::Jitter));
	CHECK_FALSE(shown(rtg::Param::Cluster));
	set(rtg::Param::Timing, static_cast<double>(dsp::EventTiming::Clustered));
	CHECK_FALSE(shown(rtg::Param::Jitter));
	CHECK(shown(rtg::Param::Cluster));
	set(rtg::Param::Sync, 3.0);
	CHECK_FALSE(shown(rtg::Param::Density));
	set(rtg::Param::SeedMode, 1.0);
	CHECK_FALSE(shown(rtg::Param::Seed));
	// THE TRIG IS A PAD, NOT A KNOB (backlog MV addendum).
	CHECK(dynamic_cast<ui::TrigPad*>(card.widgetForTest(rtg::Param::Trig)) != nullptr);
	// EVERY CONTROL HAS A WIDGET, the enable aside (the head's square).
	for (size_t i = 0; i < static_cast<size_t>(rtg::Param::kCount); ++i) {
		if (i == static_cast<size_t>(rtg::Param::Enabled)) {
			continue;
		}
		CAPTURE(i);
		CHECK(card.widgetForTest(static_cast<rtg::Param>(i)) != nullptr);
	}
}
TEST_CASE("PHOTO: the Random Trigger card over a real stream (writes only under FEATHERS_RTRIG_SHOT_DIR)") {
	const char* dir = std::getenv("FEATHERS_RTRIG_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	struct Look {
		const char* name;
		int timing;
		int size;
		bool random;
	};
	for (const Look look : { Look { "poisson-fixed", 1, 0, false }, Look { "regular-uniform-bipolar", 0, 1, true },
	                         Look { "clustered-exp", 2, 2, false } }) {
		ModFixture f;
		const RtrigSeat seat = seatRtrig(f);
		ui::RandomTriggerCard& card = *seat.card;
		card.control(rtg::Param::Timing).setValue(look.timing);
		card.control(rtg::Param::HitSize).setValue(look.size);
		card.control(rtg::Param::Polarity).setValue(look.random ? 1.0 : 0.0);
		card.control(rtg::Param::Density).setValue(std::log2(6.0));
		card.control(rtg::Param::Jitter).setValue(0.4);
		f.bridge.refresh();
		// THE DEVICE ITSELF, ticked as the engine ticks it, twelve ticks a 60 Hz frame; its reading published per frame.
		const device::ModOps& ops = *rtg::kDescriptor.mod;
		std::vector<unsigned char> mem(ops.stateBytes + 64);
		void* at = mem.data() + (64 - reinterpret_cast<uintptr_t>(mem.data()) % 64) % 64;
		ops.construct(at);
		ops.prepare(at, 48000.0);
		rtg::Row row = rtg::rowOf(rtg::Settings {});
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = f.bridge.rackFieldAt(seat.slot, i);
		}
		ops.setFields(at, row.data(), row.size());
		for (int frame = 0; frame < 400; ++frame) {
			for (int t = 0; t < 12; ++t) {
				ops.tick(at, device::ModTickContext {});
			}
			uint32_t hits = 0;
			float phase = 0.0f;
			ops.playhead(at, &hits, &phase);
			publishRtrig(f, seat, hits, 2.0f * phase - 1.0f, ops.playheadLevel(at));
			card.update();
		}
		ops.destruct(at);
		photographCard(card, std::string(dir) + "/rtrig-" + look.name + ".png");
	}
}
TEST_CASE("MW: the Sample & Hold card draws a box only where its control changes the sound, and reflows the rest") {
	ModFixture f;
	const SampleHoldSeat seat = seatSampleHold(f);
	ui::SampleHoldCard& card = *seat.card;
	// THE DEFAULTS: white noise on the Rate clock, Stable — Input is on the face but greyed; Rate, Base and Seed are live.
	CHECK(card.box(snh::Param::Input).isVisible());
	CHECK_FALSE(card.box(snh::Param::Input).enabled());
	CHECK(card.box(snh::Param::Rate).isVisible());
	CHECK(card.box(snh::Param::TimeBase).isVisible());
	CHECK(card.box(snh::Param::Seed).isVisible());
	CHECK(card.trigPad().isVisible());
	// INPUT AS THE SOURCE: the Input box is live.
	setSampleHold(f, seat, snh::Param::Source, static_cast<double>(snh::Source::Input));
	CHECK(card.box(snh::Param::Input).isVisible());
	CHECK(card.box(snh::Param::Input).enabled());
	// THE TRIG CLOCK HAS NO RATE: Rate and Base go, and the boxes after them move up into their places.
	const float seedXBefore = card.box(snh::Param::Seed).x();
	setSampleHold(f, seat, snh::Param::Clock, static_cast<double>(snh::Clock::Trig));
	CHECK_FALSE(card.box(snh::Param::Rate).isVisible());
	CHECK_FALSE(card.box(snh::Param::TimeBase).isVisible());
	CHECK(card.box(snh::Param::Seed).x() != seedXBefore);
	// RANDOM RUNS AT RATE'S MEAN: both come back.
	setSampleHold(f, seat, snh::Param::Clock, static_cast<double>(snh::Clock::Random));
	CHECK(card.box(snh::Param::Rate).isVisible());
	// FREE PLAYS THE SEED TOO (backlog NM), so its box stays.
	setSampleHold(f, seat, snh::Param::SeedMode, static_cast<double>(snh::SeedMode::Free));
	CHECK(card.box(snh::Param::Seed).isVisible());
	// AND EVERY CONTROL IS ON THE FACE: ten boxes, three chips, the Trig pad.
	CHECK(card.addressedParamsForTest().size() == 14u);
}
TEST_CASE("OO: the Sample & Hold Input knob is always drawn, greyed with its reason under every noise source") {
	ModFixture f;
	const SampleHoldSeat seat = seatSampleHold(f);
	ui::SampleHoldCard& card = *seat.card;
	for (const snh::Source s : { snh::Source::White, snh::Source::Pink, snh::Source::Brown, snh::Source::Input }) {
		setSampleHold(f, seat, snh::Param::Source, static_cast<double>(s));
		ui::ValueBox& input = card.box(snh::Param::Input);
		CHECK(input.isVisible());
		CHECK(input.enabled() == (s == snh::Source::Input));
		const char* tip = input.tooltipAt({ 1.0f, 1.0f });
		REQUIRE(tip != nullptr);
		CHECK((std::string(tip).find("set Source to Input") != std::string::npos) == (s != snh::Source::Input));
	}
	if (const char* dir = std::getenv("FEATHERS_OO_SHOT_DIR")) {
		auto* editor = dynamic_cast<visage::ApplicationEditor*>(f.root);
		REQUIRE(editor != nullptr);
		editor->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
		if (visage::Renderer::instance().supported()) {
			const auto shoot = [&](snh::Source s, const char* name) {
				setSampleHold(f, seat, snh::Param::Source, static_cast<double>(s));
				f.bridge.refresh();
				(void)card.updateContent();
				f.root->redrawAll();
				editor->drawWindow();
				editor->drawWindow();
				editor->drawWindow();
				editor->takeScreenshot().save((std::string(dir) + "/" + name).c_str());
			};
			shoot(snh::Source::White, "sh-input-greyed.png");
			shoot(snh::Source::Input, "sh-input-live.png");
		}
	}
}
TEST_CASE("OO: the Sample & Hold Input box takes a modulation drop — an armed source dragged on it routes to Input, not to another control") {
	ModFixture f;
	const SampleHoldSeat seat = seatSampleHold(f);
	ui::SampleHoldCard& card = *seat.card;
	setSampleHold(f, seat, snh::Param::Source, static_cast<double>(snh::Source::Input));
	ui::ValueBox& input = card.box(snh::Param::Input);
	REQUIRE(input.isVisible());
	REQUIRE(input.enabled());
	const backplane::Destination onInput = backplane::Destination::control(f.bridge.rackDeviceAt(seat.slot),
	                                                                       static_cast<size_t>(snh::Param::Input));
	const backplane::Destination onProb = backplane::Destination::control(f.bridge.rackDeviceAt(seat.slot),
	                                                                      static_cast<size_t>(snh::Param::Probability));
	const backplane::Source macro = backplane::Source::core(0);
	REQUIRE(f.bridge.mayRoute(macro, onInput));
	CHECK(f.bridge.routeAmount(macro, onInput) == 0.0f);

	// NOT ARMED: the same drag edits the value and routes nothing.
	const visage::Point centre = input.positionInWindow() + visage::Point { input.width() * 0.5f, input.height() * 0.5f };
	const double before = card.control(static_cast<size_t>(snh::Param::Input)).value();
	dragThroughRoot(f.root, centre, centre - visage::Point { 0.0f, 20.0f });
	CHECK(f.bridge.routeAmount(macro, onInput) == 0.0f);
	CHECK(card.control(static_cast<size_t>(snh::Param::Input)).value() != doctest::Approx(before));

	// ARMED: the drag writes the route onto Input (and no other control), leaving the value where it stood.
	const double rest = card.control(static_cast<size_t>(snh::Param::Input)).value();
	f.arm(macro);
	dragThroughRoot(f.root, centre, centre - visage::Point { 0.0f, 20.0f });
	CHECK(f.bridge.routeAmount(macro, onInput) != 0.0f);
	CHECK(f.bridge.routeAmount(macro, onProb) == 0.0f);
	CHECK(card.control(static_cast<size_t>(snh::Param::Input)).value() == doctest::Approx(rest));
}
TEST_CASE("MW: the Sample & Hold scope reads the engine's published hold, marks each sample taken, and glides by Slew's own pole") {
	ModFixture f;
	const SampleHoldSeat seat = seatSampleHold(f);
	ui::SampleHoldCard& card = *seat.card;
	// NOTHING PUBLISHED: no reading, and none invented.
	(void)card.pollScope(1.0 / 60.0);
	CHECK(card.scopeCountForTest() == 0u);

	const auto publish = [&](uint32_t ordinal, float level) {
		feathers::mod::Playhead where { ordinal, 0.0f };
		where.level = level;
		where.haveLevel = true;
		f.bridge.publishRackPlayhead(seat.slot, where);
		f.bridge.refresh();
	};
	publish(1, 0.25f);
	(void)card.pollScope(1.0 / 60.0);
	REQUIRE(card.scopeCountForTest() == 1u);
	CHECK(card.scopeAtForTest(0).level == 0.25f);
	CHECK(card.scopeAtForTest(0).glided == 0.25f); // the first reading starts the glide where the hold is
	CHECK_FALSE(card.scopeAtForTest(0).taken);

	// A SAMPLE TAKEN (the ordinal moves), Slew off: the glide IS the hold.
	publish(2, 0.75f);
	(void)card.pollScope(1.0 / 60.0);
	CHECK(card.scopeAtForTest(0).taken);
	CHECK(card.scopeAtForTest(0).level == 0.75f);
	CHECK(card.scopeAtForTest(0).glided == 0.75f);
	(void)card.pollScope(1.0 / 60.0);
	CHECK_FALSE(card.scopeAtForTest(0).taken); // ONE mark per sample

	// SLEW 100 ms: a frame of 20 ms closes 1 - exp(-0.2) of the gap, the device's exact pole.
	setSampleHold(f, seat, snh::Param::Slew, std::log2(0.1));
	publish(3, 0.25f);
	(void)card.pollScope(0.02);
	const double expected = 0.75 + (0.25 - 0.75) * (1.0 - std::exp(-0.2));
	CHECK(std::abs(static_cast<double>(card.scopeAtForTest(0).glided) - expected) < 1e-6);
	CHECK(card.scopeAtForTest(0).level == 0.25f);
}
TEST_CASE("MW: a Sample & Hold clock faster than the frame rate records every step since the last frame, not fewer") {
	ModFixture f;
	const SampleHoldSeat seat = seatSampleHold(f);
	ui::SampleHoldCard& card = *seat.card;
	const auto publish = [&](uint32_t ordinal, std::vector<feathers::device::RecentEvent> recent) {
		feathers::mod::Playhead where { ordinal, 0.0f };
		where.level = recent.empty() ? 0.5f : recent.front().value;
		where.haveLevel = true;
		for (size_t i = 0; i < recent.size(); ++i) {
			where.recent[i] = recent[i];
		}
		f.bridge.publishRackPlayhead(seat.slot, where);
		f.bridge.refresh();
	};
	publish(4, { { 4, 0.5f }, { 3, 0.0f } });
	(void)card.pollScope(1.0 / 60.0);
	// FOUR STEPS IN ONE FRAME (5..8), the newest at 0.4: the column counts four and spans their 0.1..0.9, while 3's 0.0
	// (an earlier frame's) is not in it.
	publish(8, { { 8, 0.4f }, { 7, 0.9f }, { 6, 0.1f }, { 5, 0.6f }, { 4, 0.5f }, { 3, 0.0f } });
	(void)card.pollScope(1.0 / 60.0);
	const ui::SampleHoldCard::Reading r = card.scopeAtForTest(0);
	CHECK(r.taken);
	CHECK(r.steps == 4u);
	CHECK(r.level == 0.4f);
	CHECK(r.lo == 0.1f);
	CHECK(r.hi == 0.9f);
	// ONE STEP: its own level is the whole span.
	publish(9, { { 9, 0.7f }, { 8, 0.4f } });
	(void)card.pollScope(1.0 / 60.0);
	CHECK(card.scopeAtForTest(0).steps == 1u);
	CHECK(card.scopeAtForTest(0).lo == 0.7f);
	CHECK(card.scopeAtForTest(0).hi == 0.7f);
	// NONE: no steps.
	(void)card.pollScope(1.0 / 60.0);
	CHECK(card.scopeAtForTest(0).steps == 0u);
}
TEST_CASE("MW: a box row the gates empty is given to the Sample & Hold scope, and the chips do not move") {
	ModFixture f;
	const SampleHoldSeat seat = seatSampleHold(f);
	ui::SampleHoldCard& card = *seat.card;
	constexpr float kRow = ui::ValueBox::kHeight + ui::SampleHoldCard::kRowGap;
	// THE DEFAULTS FILL THREE ROWS (nine live boxes): the scope at its own height.
	REQUIRE(card.boxRowsUsed() == 3u);
	CHECK(card.scopeBounds().height() == ui::SampleHoldCard::kScopeHeight);
	const float chipY = card.chip(snh::Param::Poly).y();
	const float firstBoxY = card.box(snh::Param::Source).y();
	// THE TRIG CLOCK LEAVES SEVEN: two rows, and the third row's height is the scope's.
	setSampleHold(f, seat, snh::Param::Clock, static_cast<double>(snh::Clock::Trig));
	REQUIRE(card.boxRowsUsed() == 2u);
	CHECK(card.scopeBounds().height() == ui::SampleHoldCard::kScopeHeight + kRow);
	CHECK(card.box(snh::Param::Source).y() == firstBoxY + kRow); // the grid moved down by the row it gave up
	CHECK(card.chip(snh::Param::Poly).y() == chipY);
	// A FREE SEED KEEPS ITS BOX (backlog NM: Free plays the Seed too), so Free takes no row from the boxes.
	setSampleHold(f, seat, snh::Param::SeedMode, static_cast<double>(snh::SeedMode::Free));
	CHECK(card.box(snh::Param::Seed).isVisible());
	CHECK(card.boxRowsUsed() == 2u);
	// THE LAST LIVE BOX ENDS ABOVE THE CHIPS: nothing overlaps.
	CHECK(card.box(snh::Param::Probability).bottom() < chipY);
	// AND BACK: Rate and Base take their row back.
	setSampleHold(f, seat, snh::Param::Clock, static_cast<double>(snh::Clock::Rate));
	CHECK(card.boxRowsUsed() == 3u);
	CHECK(card.scopeBounds().height() == ui::SampleHoldCard::kScopeHeight);
}
TEST_CASE("STEPSEQ card: a drag on the grid draws the lane shown, a lock survives Randomise, and Randomise is one undo "
          "step that undo takes back to the bit") {
	ModFixture f;
	const StepSeqSeat seat = seatStepSeq(f);
	ui::StepSeqCard& card = *seat.card;
	ui::StepGrid& grid = card.grid();
	ui::EditHistory* history = f.bridge.editHistory();
	REQUIRE(history != nullptr);
	REQUIRE(grid.width() > 0.0f);
	const size_t start = history->length();

	// DRAW: a press a quarter of the way down the bars of step 2 sets its value to 75%, as one undo step.
	const visage::Bounds bars = grid.barsBounds();
	const visage::Bounds two = grid.stepBounds(2);
	const float x2 = two.x() + two.width() * 0.5f;
	grid.onMouseDown().callback(at(x2, bars.y() + bars.height() * 0.25f));
	grid.onMouseUp().callback(at(x2, bars.y() + bars.height() * 0.25f));
	CHECK(std::abs(grid.pattern().step[2].value - 0.75f) <= 1e-6f);
	CHECK(grid.pattern().step[3].value == 0.0f);
	CHECK(history->length() == start + 1);

	// LOCK step 5 from the strip over the bars.
	const visage::Bounds five = grid.stepBounds(5);
	const float x5 = five.x() + five.width() * 0.5f;
	grid.onMouseDown().callback(at(x5, grid.lockTop() + 2.0f));
	grid.onMouseUp().callback(at(x5, grid.lockTop() + 2.0f));
	REQUIRE(grid.pattern().step[5].locked);
	CHECK(history->length() == start + 2);

	// RANDOMISE THE VALUE LANE: everything unlocked moves, step 5 does not, and it is ONE step.
	const ssq::Pattern before = grid.pattern();
	const std::vector<uint32_t> beforeBits = patternBits(f, seat.device);
	REQUIRE(card.randomise());
	const ssq::Pattern after = grid.pattern();
	const std::vector<uint32_t> afterBits = patternBits(f, seat.device);
	CHECK(history->length() == start + 3);
	CHECK(after.generation == before.generation + 1);
	CHECK(after.step[5] == before.step[5]);
	int moved = 0;
	for (size_t s = 0; s < ssq::kDefaultSteps; ++s) {
		moved += after.step[s].value != before.step[s].value ? 1 : 0;
	}
	CHECK(moved == static_cast<int>(ssq::kDefaultSteps) - 1);
	// AND IT IS WHAT THE ACT DRAWS, from the seed and Amount on the card — under a fresh basis, a hand's edit (backlog MX).
	ssq::Pattern drawnByAct = ssq::randomise(before, ssq::kDefaultSteps, ssq::Lane::Value, 1.0f, 0);
	CHECK(after.basis != before.basis);
	drawnByAct.basis = after.basis;
	CHECK(after == drawnByAct);

	// UNDO TAKES IT BACK EXACTLY, REDO PUTS IT BACK EXACTLY.
	history->undo();
	CHECK(patternBits(f, seat.device) == beforeBits);
	history->redo();
	CHECK(patternBits(f, seat.device) == afterBits);
	// AND THE TWO EARLIER STEPS ARE THEIR OWN: two more undos clear the lock and the drawn value.
	history->undo();
	history->undo();
	history->undo();
	CHECK_FALSE(grid.pattern().step[5].locked);
	CHECK(grid.pattern().step[2].value == 0.0f);
}

