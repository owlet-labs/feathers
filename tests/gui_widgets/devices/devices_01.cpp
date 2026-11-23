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

// PIECE 1 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("the Shaper's panel lays out inside the size it asks for") {
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.card != nullptr);
	REQUIRE(f.panel != nullptr);

	const visage::Point min = f.panel->minimumSize();
	CHECK(min.x > 0.0f);
	CHECK(min.y > 0.0f);

	SUBCASE("every part is inside the panel") {
		// THE OVERLAY CLAMPS RATHER THAN SCROLLING, so anything past the minimum is not merely ugly —
		// it is unreachable. Waypoint's panel states the rule; this is the case for it.
		const auto within = [&](const visage::Bounds& b, const char* what) {
			CAPTURE(what);
			CHECK(b.x() >= 0.0f);
			CHECK(b.y() >= 0.0f);
			CHECK(b.right() <= min.x + 0.5f);
			CHECK(b.bottom() <= min.y + 0.5f);
		};
		within(f.panel->padBounds(), "pad");
		within(f.panel->plotBounds(), "plot");
		within(f.panel->readoutBounds(), "readout");
		// (NO "enable" ANY MORE: the square is the window header's chip, 2026-09-26.)
		for (size_t i = 0; i < ui::ShaperExpanded::kExtraCount; ++i) {
			within(f.panel->extraBounds(i), "extra");
		}
	}

	SUBCASE("the pad and the plot do not overlap") {
		const visage::Bounds pad = f.panel->padBounds();
		const visage::Bounds plot = f.panel->plotBounds();
		CHECK(plot.x() >= pad.right());
	}

	SUBCASE("the five extras switches are side by side and none is on top of another") {
		for (size_t i = 1; i < ui::ShaperExpanded::kExtraCount; ++i) {
			CAPTURE(i);
			const visage::Bounds prev = f.panel->extraBounds(i - 1);
			const visage::Bounds here = f.panel->extraBounds(i);
			CHECK(here.x() >= prev.right());
			CHECK(here.y() == doctest::Approx(prev.y()));
			CHECK(here.width() > 0.0f);
		}
		// AND PAST THE END IS EMPTY rather than a rectangle off the edge — an index is only an index
		// while it names something.
		CHECK(f.panel->extraBounds(ui::ShaperExpanded::kExtraCount).width() == 0.0f);
	}

	SUBCASE("the knob row is below the pad and every knob has a real rectangle") {
		const auto& knobs = f.panel->children();
		size_t placed = 0;
		for (const auto* child : knobs) {
			// A KNOB, A COORDINATE BOX, OR GAMMA'S BOX — the row holds all three kinds now, and a
			// check that walked only one would report the row correctly laid out while ignoring a
			// cell in it. **Gamma joined this list when it stopped being a `Knob` (by design), which is
			// the cell most likely to be wrong and was.**
			// **AND A `ZoneCoordCell` COUNTS TOO — the widget automod's coordinates moved to when their
			// pairs collapsed (BJ piece 3).** This listed the old `ZoneCoordBox`, so after the swap the
			// three coordinate cells stopped being counted and the case read `10 == 13`. **It failed
			// LOUDLY, which is the good outcome of an exhaustive count**: a sweep that had checked
			// "every knob is placed" would have gone green while three cells went unexamined.
			const bool isCell = dynamic_cast<const ui::Knob*>(child) != nullptr
			                    || dynamic_cast<const ui::ZoneCoordCell*>(child) != nullptr
			                    || dynamic_cast<const ui::GammaBox*>(child) != nullptr
			                    || dynamic_cast<const ui::ModeValueCell*>(child) != nullptr; // Colour's mode cell (backlog IW)
			if (!isCell) {
				continue;
			}
			// ROTATE IS NOT A ROW CELL SINCE backlog IJ — it sits INSIDE the terrain box, asserted below.
			if (child == f.panel->rotateForTest()) {
				continue;
			}
			++placed;
			CAPTURE(placed);
			CHECK(child->width() > 0.0f);
			CHECK(child->height() > 0.0f);
			CHECK(child->y() >= f.panel->padBounds().bottom());
			CHECK(child->bounds().bottom() <= min.y + 0.5f);
		}
		CHECK(placed == ui::ShaperExpanded::kKnobCells);

		// **AND ROTATE IS INSIDE THE TERRAIN BOX**, where it belongs beside the terrain it turns. Wholly within the pad's rectangle, with a size, and drawn OVER the pad (a later child).
		const auto* rotate = f.panel->rotateForTest();
		REQUIRE(rotate != nullptr);
		const visage::Bounds pad = f.panel->padBounds();
		CHECK(rotate->width() > 0.0f);
		CHECK(rotate->height() > 0.0f);
		CHECK(rotate->x() >= pad.x());
		CHECK(rotate->y() >= pad.y());
		CHECK(rotate->bounds().right() <= pad.right());
		CHECK(rotate->bounds().bottom() <= pad.bottom());
		const auto& kids = f.panel->children();
		const auto at = [&](const visage::Frame* frame) {
			return std::find(kids.begin(), kids.end(), frame) - kids.begin();
		};
		CHECK(at(rotate) > at(f.panel->padForTest()));
	}
}
TEST_CASE("the Shaper's pad commits TRAVEL through the plane, not Colour and Gamma") {
	// **THIS CASE USED TO ASSERT THE PAD WROTE COLOUR AND GAMMA, "and the right way up".** It did, and
	// that was the defect: MEASURED in `shaper_phase_space.h`, those two point the SAME WAY — 0 degrees
	// apart at colour 0, 5.5 degrees at colour 1 — so the pad was a line drawn as a square. Two knobs
	// on one scalar, presented as a plane.
	//
	// It now commits TRAVEL: a drag is decoded through the basis into the eight folded-travel floats
	// past the declared params, which is the only thing that moves the phases independently.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	REQUIRE(pad->width() > 1.0f);
	REQUIRE(pad->height() > 1.0f);

	const auto field = [&](size_t index) {
		const FxChain chain = f.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, f.slot).row.field[index];
	};
	const auto travelNow = [&] {
		std::array<float, shp::phase_space::kPhaseCount> out {};
		for (size_t i = 0; i < out.size(); ++i) {
			out[i] = field(shp::kTravelFirst + i);
		}
		return out;
	};

	const float colourBefore = field(static_cast<size_t>(shp::Param::Colour));
	const float gammaBefore = field(static_cast<size_t>(shp::Param::Gamma));
	const auto before = travelNow();

	// A DRAG FROM THE CENTRE, up and to the right.
	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
	pad->onMouseDown().callback(at(centre.x, centre.y));
	pad->onMouseDrag().callback(at(centre.x + pad->width() * 0.25f, centre.y - pad->height() * 0.25f));
	pad->onMouseUp().callback(at(centre.x, centre.y));
	pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)

	const auto after = travelNow();

	// **THE TRAVEL MOVED**, and in more than one phase — a decode that wrote the same number to every
	// entry, or to only one, would not be a plane.
	int moved = 0;
	for (size_t i = 0; i < before.size(); ++i) {
		CAPTURE(i);
		if (before[i] != after[i]) {
			++moved;
		}
	}
	CAPTURE(moved);
	CHECK(moved >= 2);

	// **AND COLOUR AND GAMMA DID NOT.** This is the whole behaviour change: the coordinates are where
	// the field's HOME is, and the pad no longer touches them. A pad that still wrote them would be
	// the old line-drawn-as-a-square wearing a new decode.
	CHECK(field(static_cast<size_t>(shp::Param::Colour)) == colourBefore);
	CHECK(field(static_cast<size_t>(shp::Param::Gamma)) == gammaBefore);

	SUBCASE("the puck returns to centre, so there is no standing travel for Rotate to move") {
		// The chart's origin IS the current position. That is what keeps a basis turn silent at rest,
		// which `Shaper :: Shaper Rotate` being on the dead list depends on.
		const visage::Point p = pad->puck();
		CHECK(p.x == doctest::Approx(pad->width() * 0.5f).epsilon(0.001));
		CHECK(p.y == doctest::Approx(pad->height() * 0.5f).epsilon(0.001));
	}

}
TEST_CASE("the Shaper's panel readout prints what the field is actually saying") {
	// THE READOUT STANDS IN FOR NINE KNOBS THIS DEVICE DELIBERATELY DOES NOT HAVE, so it had better
	// be reporting the field rather than plausible-looking text. Asserted by moving Colour and
	// checking the lines MOVE — and by checking one of them against `deriveCurve` directly.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);

	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Colour), 0.2f);
	const std::vector<std::string> atLow = f.panel->readoutLinesForTest();
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Colour), 0.65f);
	const std::vector<std::string> atHigh = f.panel->readoutLinesForTest();

	CHECK(atLow.size() == 6);
	CHECK(atHigh.size() == 6);
	CHECK(atLow != atHigh);
	for (const std::string& line : atHigh) {
		CHECK_FALSE(line.empty());
	}

	SUBCASE("and the sub's line names the interval the mapper chose") {
		// A RATIO IS A COUNT OF ZERO CROSSINGS AND NOBODY HEARS ONE; the INTERVAL is what they hear.
		// MEASURED at colour 0.65: the field picks ratio 3, which is a twelfth.
		const shp::CurveParams p = shp::deriveCurve(0.5f, 0.65f, 0.0f, 0.5f);
		REQUIRE(p.subRatio == 3);
		CHECK(atHigh[0].find("twelfth") != std::string::npos);
	}
}
TEST_CASE("the Shaper's panel readout is curveOf the device's row — travel included — and prints what is heard") {
	// The little numbers beside the extras appeared never to change.
	// TWO ANSWERS, and this pins both:
	//   - FIVE OF THE SIX ARE THE COORDINATES' — Sub, Comb, Rotate, Slew and Hysteresis come from Colour
	//     and Gamma alone (`deriveCurve` reads no travel for them), so the pad does not move them BY
	//     DESIGN; Colour and Gamma do (the case above).
	//   - ASYMMETRY IS THE PAD'S TOO, and the readout could not see it: it called `deriveCurve` of the
	//     coordinates with no travel. MEASURED, the second drag below takes it 0.30 -> 0.63 through
	//     `curveOf`, where the old readout stayed at 0.30.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Colour), 0.38f);
	// COMB AND LOWPASS POWERED, at their default amount, the field's own (backlog II, IK) — the lines print
	// what is HEARD, and with the power off
	// both amounts are 0 before and after any drag, which would make "they moved" vacuous.
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::FeedbackOn), 1.0f);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::LowpassOn), 1.0f);
	const std::vector<std::string> before = f.panel->readoutLinesForTest();

	// A WALK OF TEN DRAGS FROM THE CENTRE TO THE NINE COMPASS POINTS AND ROUND AGAIN — MEASURED under the
	// pad's Explore-style pan (backlog IJ) to carry t(7) into Asymmetry's narrow triangle on the tenth:
	// 0.30 at every step before it, 0.59 after it. The joystick reached it on the second drag of a
	// different pair; its commits re-derived the plane on every event, so the same pixels landed elsewhere.
	const visage::Point centre { pad->width() * 0.5f, pad->height() * 0.5f };
	for (int d = 0; d < 10; ++d) {
		const float x = centre.x + pad->width() * 0.45f * static_cast<float>((d % 3) - 1);
		const float y = centre.y - pad->height() * 0.45f * static_cast<float>(((d / 3) % 3) - 1);
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(x, y));
		pad->onMouseUp().callback(at(x, y));
		pad->finishGlideForTest(); // the release glide, landed as a frame loop would (backlog IJ)
	}
	const std::vector<std::string> after = f.panel->readoutLinesForTest();

	// THE STATE, restated from the row the drags committed — params AND travel.
	const FxChain chain = f.f.bridge.fxChain();
	const DeviceRow& row = fxDeviceAtPosition(chain, f.slot).row;
	shp::Row params {};
	for (size_t i = 0; i < params.size(); ++i) {
		params[i] = row.field[i];
	}
	shp::Settings s = shp::stateOf(params, params.size());
	for (size_t i = 0; i < shp::phase_space::kPhaseCount; ++i) {
		s.travel[i] = row.field[shp::kTravelFirst + i];
	}
	const float withTravel = shp::curveOf(s).asymmetry;
	const float coordinatesOnly = shp::deriveCurve(s.knee, s.colour, s.gamma, shp::kSlewHarmonics).asymmetry;
	char want[32];
	std::snprintf(want, sizeof(want), "Asymmetry  %.2f", static_cast<double>(withTravel));
	CHECK(after[5] == want);
	// NON-VACUOUS: at this position travel changes the answer, so a readout that ignored it fails here.
	CAPTURE(withTravel);
	CAPTURE(coordinatesOnly);
	REQUIRE(std::fabs(withTravel - coordinatesOnly) > 0.05f);

	// **AND THE EXTRAS MOVE WITH THE DRAG TOO — backlog II, 2026-09-23.** This pinned the opposite for a
	// day: the pad's travel reached only the curve's eight phases, so Sub, Comb, Rotate and Slew stayed
	// where Colour put them. In the firmware they are attached to the zone knob, which ties back to the
	// pad — and travel now carries their phases as well. Asserted as the
	// VALUES `curveOf` gives for the committed row, not as "something changed".
	const shp::CurveParams heard = shp::curveOf(s);
	char comb[64];
	std::snprintf(comb, sizeof(comb), "Comb  %d smp  x%.2f", static_cast<int>(heard.stride),
	              static_cast<double>(heard.feedback));
	CHECK(after[1] == comb);
	char slew[32];
	std::snprintf(slew, sizeof(slew), "Slew  x%.2f",
	              static_cast<double>(std::max(heard.lowpassAmount, heard.integratorAmount)));
	CHECK(after[3] == slew);
	// AND THEY DID MOVE — MEASURED at this position, Comb 2 smp -> 32 smp and Slew 0.75 -> 0.11.
	CHECK(after[1] != before[1]);
	CHECK(after[3] != before[3]);

	SUBCASE("and Hysteresis prints what is HEARD: 0 at its default knob of 0, whatever the field offers") {
		REQUIRE(s.hysteresis == 0.0f);
		CHECK(after[4] == "Hysteresis  0.00");
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Hysteresis), 1.0f);
		const std::vector<std::string> full = f.panel->readoutLinesForTest();
		// KNOB 1 LIFTS THE FIELD'S MAGNITUDE TO ONE, KEEPING ITS SIGN — `fieldWithOverride`, the engine's law.
		const float field = shp::curveOf(s).hysteresis;
		CHECK(full[4] == (field < 0.0f ? "Hysteresis  -1.00" : "Hysteresis  1.00"));
	}
}
TEST_CASE("the Shaper's card is the gamma-zero line, and says so by what it does not carry") {
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.card != nullptr);

	SUBCASE("four knobs, and Gamma is not one of them") {
		const auto reached = f.card->addressedParamsForTest();
		// THE ROW IS STILL `kKnobCells` WIDE; one of its cells is no longer a knob. Counted as CELLS
		// so the claim is about the row rather than about which widget kind happens to fill it.
		CHECK(f.card->cellsForTest().size() == ui::ShaperCard::kKnobCells);
		CHECK(f.card->knobsForTest().size() + (f.card->colourCellForTest() != nullptr ? 1u : 0u)
		      == ui::ShaperCard::kKnobCells);
		CHECK(std::find(reached.begin(), reached.end(), shp::Param::Gamma) == reached.end());
		CHECK(std::find(reached.begin(), reached.end(), shp::Param::Enabled) != reached.end());
	}

	SUBCASE("backlog IW: Colour is a MODE cell — Inflate, then Free — and its box is the live mode's value") {
		// The dropdown selects a mode, not a phase region. Colour's eight region
		// names were labels over a continuum; its dropdown lists its MODES now, and the box beside it follows the mode:
		// the unbounded field in Free, the Inflate amount in Inflate.
		const ui::ModeValueCell* colour = f.card->colourCellForTest();
		REQUIRE(colour != nullptr);
		CHECK(f.card->coordsForTest().empty());
		CHECK(colour->select().zoneCount() == 2);
		CHECK(colour->select().captionText() == "Free"); // the default: a fresh shaper is the field
		CHECK(colour->activeParam() == static_cast<long>(shp::Param::Colour));
		// THE MODE, SET THROUGH THE BRIDGE, SWAPS THE BOX — through the card's own update, as a host change would.
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::ColourMode),
		                            static_cast<float>(shp::kColourInflate));
		f.card->update();
		CHECK(colour->select().captionText() == "Inflate");
		CHECK(colour->activeParam() == static_cast<long>(shp::Param::InflateAmount));
		REQUIRE(colour->boxFor(static_cast<size_t>(shp::Param::InflateAmount)) != nullptr);
		CHECK(colour->boxFor(static_cast<size_t>(shp::Param::InflateAmount))->isVisible());
		CHECK_FALSE(colour->boxFor(static_cast<size_t>(shp::Param::Colour))->isVisible());
		// AND ALL THREE ARE REACHABLE from the card — the mode, the field and the amount.
		const auto reached = f.card->addressedParamsForTest();
		for (const shp::Param p : { shp::Param::ColourMode, shp::Param::Colour, shp::Param::InflateAmount }) {
			CHECK(std::find(reached.begin(), reached.end(), p) != reached.end());
		}

		// AND KNEE IS THE OTHER HALF OF THE CLAIM, still: a normal range knob, not a "slow" one. Its
		// eight steps are a monotone drive
		// ladder — `test_fx_shaper.cpp` measures them strictly increasing, 3.3% to 23.3% THD — so it
		// is a bounded quantity and keeps the dial that says so.
		size_t knees = 0;
		for (const auto& knob : f.card->knobsForTest()) {
			if (f.card->paramOfKnobForTest(knob.get()) == shp::Param::Knee) {
				++knees;
				CHECK(dynamic_cast<const ui::ZoneCoordCell*>(knob.get()) == nullptr);
			}
		}
		CHECK(knees == 1);
	}

	SUBCASE("the plot is under the head and inside the open card") {
		const visage::Bounds plot = f.card->plotBounds();
		CHECK(plot.y() >= ui::ShaperCard::kSlotHeadHeight);
		CHECK(plot.bottom() <= ui::ShaperCard::openHeight());
		CHECK(plot.width() > 0.0f);
	}

	SUBCASE("a folded card is its head, and says which Colour mode it is in") {
		CHECK(f.card->laneHeight() == doctest::Approx(ui::ShaperCard::openHeight()));
		f.card->setFolded(true);
		CHECK(f.card->laneHeight() == doctest::Approx(ui::ShaperCard::foldedHeight()));
		// KNEE DEFAULTS TO 0.5 AND COLOUR'S MODE TO FREE (backlog IW: the mode, no longer a zone name).
		CHECK(f.card->foldedSummaryForTest() == "Free");
		// AND AT KNEE 0 IT SAYS BYPASS INSTEAD, because naming a colour zone on a device that is
		// passing the signal through would be naming a character nobody can hear.
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Knee), 0.0f);
		CHECK(f.card->foldedSummaryForTest() == "Bypass");
	}
}
TEST_CASE("dismissing an expanded view takes its knobs off the MODULATION list too") {
	// FOUND BY `hunt-state` BY READING, CONFIRMED HERE BY COUNTING. It is the crash 96d94be fixed
	// for FxLane cards, one door over: `ExpandedOverlay::dismissBy` cleaned `watchers_` — the state
	// road — and the knobs inside an expanded view are not on that list. `Knob::setModulation` puts
	// each one on `ModState`'s own list individually so the glow follows arming, and the overlay had
	// no pointer to it at all.
	//
	// ASSERTED AS A COUNT, NOT AS A CRASH, and that is the stronger form. A dangling read is
	// undefined rather than reliably fatal — before the fix this sequence LEAKED but did not
	// segfault, because nothing had reused the memory yet. Measured then: 124 -> 133 -> 133 -> 148
	// -> 148, never coming down. A case waiting for a crash would have passed.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	REQUIRE(mod != nullptr);

	const size_t harm = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(harm);
	lane->resized();

	const size_t before = mod->watchers().count();
	REQUIRE(lane->onExpandDevice != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true); // on the overlay, the host this case reads
	lane->onExpandDevice(harm);
	REQUIRE(overlay->open());

	// THE PANEL PUT ITS KNOBS ON THE LIST — without this the case would pass on an overlay that
	// registered nothing, which is the vacuous version of the same green.
	const size_t open = mod->watchers().count();
	CHECK(open > before);

	overlay->dismiss();
	CHECK_FALSE(overlay->open());

	// AND TOOK THEM OFF AGAIN. Exactly, not approximately: a leftover is a freed pointer the next
	// `ModState::focus()` will walk, and one is as fatal as nine.
	CHECK(mod->watchers().count() == before);

	// AND IT SURVIVES A SECOND CYCLE, because the first fix for this bug was also partial — the
	// second open is what frees the retired content and reuses the memory.
	const size_t second = f.bridge.addFxDevice(fx::owltt::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(second);
	lane->resized();
	const size_t baseline = mod->watchers().count(); // the CARD's own knobs are not a leak
	lane->onExpandDevice(second);
	REQUIRE(overlay->open());
	overlay->dismiss();
	CHECK(mod->watchers().count() == baseline);

	// AND NOTIFYING DOES NOT WALK A CORPSE. Survival is not much of an assertion on its own, so the
	// count above is the real one; this is here because it is the gesture that actually crashed.
	CHECK(mod->watchers().notify() > 0);
}
TEST_CASE("a rack source is named by the device in the slot, not by the envelope") {
	// THE SECOND HALF OF AN ALREADY-REPORTED BUG. `ModPanel::deviceName` was fixed to ask the
	// registry after the card head said "DAHDSR" over an MSEG's editor and read as the wrong
	// device having been added (mod_panel.h records that). `modSourceName`, twelve functions up in
	// the same file, kept the hardcode — so the same misreading survived in the route list, the
	// notification tray, the Engine rail and the panel's own source readout.
	//
	// Found by hunt-state, who confirmed an MSEG can actually reach that function before believing
	// it was live rather than latent — which is the check I skipped when I first asserted it.
	ModFixture f;

	// SWEPT FROM THE REGISTRY over every device that can sit in the rack, so a third modulator type
	// is covered without anybody returning. A hardcoded pair would have passed the day MSEG shipped
	// and failed nobody the day the next one does.
	size_t swept = 0;
	for (const device::Descriptor& d : feathers::kDevices) {
		if (!feathers::maySitInModRack(d)) {
			continue;
		}
		ModFixture g;
		const size_t slot = g.bridge.addRackDevice(d.traits.tag);
		if (slot >= feathers::kMaxRackSlots) {
			continue;
		}
		g.bridge.refresh();
		const backplane::Source source = backplane::Source::output(g.bridge.rackDeviceAt(slot), 0);
		const std::string name = ui::modSourceName(g.bridge, source);
		CAPTURE(d.traits.name);
		CAPTURE(name);
		// THE DEVICE'S OWN NAME IS IN IT. Not equality: the label is qualified by position once the
		// rack holds more than one, and that qualification is a separate claim with its own case.
		CHECK(name.find(d.traits.name) != std::string::npos);
		++swept;
	}
	// AND THE SWEEP VISITED SOMETHING. Without this the case passes on a registry that returns
	// nothing rack-seatable, which is the vacuous green this file has been bitten by twice today.
	REQUIRE(swept >= 2);

	// A DEVICE NOTHING HOLDS NAMES NOTHING, rather than naming the envelope — the rule
	// `modDestinationName` already follows by answering "Route". (It was "an empty slot"; a source
	// names a device by id now, and an id nothing wears is the same question.)
	constexpr DeviceInstanceId kNobody = 999999;
	REQUIRE(f.bridge.descriptorOf(kNobody) == nullptr);
	const std::string empty = ui::modSourceName(f.bridge, backplane::Source::output(kNobody, 0));
	CHECK(empty == "Source");
}
TEST_CASE("undoing a route repaints the surfaces that draw one") {
	// A VALUE THE ENGINE HEARS AND A PICTURE THAT DISAGREES. `RouteEdit::undo` wrote the amount
	// straight through the bridge and told nobody, so the ring under the hand, the mod panel's route
	// list and the rack cards went on drawing the pre-undo amount until some unrelated gesture
	// happened to dirty them.
	//
	// AND THE STATE CHANNEL DOES NOT COVER FOR IT, which is the part that made it survivable-looking:
	// `setRouteAmount` does raise `repaintDirty_`, and `Gui::Impl::tick` turns that into
	// `stateWatchers.notify()` — but that list holds the STAGES and the fx cards. The knobs, the mod
	// panel and the rack cards are on `ModState`'s list, which is why every live road calls
	// `touched()` by hand. The one road that could not reach a ModState was the one with no
	// substitute.
	//
	// ASSERTED ON THE NOTIFICATION, BECAUSE A REPAINT IS NOT OBSERVABLE HEADLESSLY. `FrameWatchers::
	// notify()` returns how many frames it told and `ModState::touched()` hands that on; the tally is
	// the accumulation of those returns, so it cannot rise for a call that reached an empty list. A
	// counter standing beside the call could.
	ModFixture f;
	f.arm(backplane::Source::core(0));
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const backplane::Destination dest = backplane::Destination::core(static_cast<size_t>(destination));

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	// THE LIVE DRAG TELLS SOMEBODY, SAID FIRST. If the tally never moved here the road would not be
	// instrumented at all and every check below would be measuring nothing — the venue has to be
	// shown capable of expressing the outcome before its silence counts.
	const size_t beforeDrag = mod->framesToldForTest();
	const visage::Point centre = f.faceCentre(path);
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });
	const float amount = f.bridge.routeAmount(backplane::Source::core(0), dest);
	REQUIRE(amount > 0.0f);
	REQUIRE(mod->framesToldForTest() > beforeDrag);

	// AND SHOWN CAPABLE OF STAYING SILENT. A refresh moves parameters and says nothing about
	// routings, so a tally that rose here would be measuring "something happened" rather than "the
	// routings were announced", and every check below would pass on a build with the fix removed.
	const size_t idle = mod->framesToldForTest();
	f.bridge.refresh();
	REQUIRE(mod->framesToldForTest() == idle);

	REQUIRE(f.gui.history() != nullptr);
	REQUIRE(f.gui.history()->canUndo());

	SUBCASE("undo restores the value AND says so") {
		const size_t told = mod->framesToldForTest();
		f.gui.history()->undo();
		// THE VALUE FIRST — it was already right, and a case that only checked the tally could pass
		// on an undo that announced a change it had not made.
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		CHECK(mod->framesToldForTest() > told);
	}

	SUBCASE("and so does redo") {
		f.gui.history()->undo();
		const size_t told = mod->framesToldForTest();
		f.gui.history()->redo();
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == doctest::Approx(amount));
		CHECK(mod->framesToldForTest() > told);
	}
}
TEST_CASE("every road that writes an amount announces it, including the fx lane's") {
	// `Modulatable::setAmount` IS THE CONVENTION AND IT HAD FOUR IMPLEMENTATIONS, THREE OF WHICH
	// FOLLOWED IT. ParamControl, RackControl and MsegControl each end with `mod_->touched()`;
	// `FxControl::setAmount` did not. `Knob::commitRoute` asserts the convention in a comment —
	// "setAmount notifies the panel itself — see ParamControl::setAmount and RackControl::setAmount"
	// — and names the two it checked.
	//
	// IT WAS PARTLY MASKED, which is why it survived: an fx CARD is on the state channel (`FxLane`
	// is handed `stateWatchers`), and `setRouteAmount` raises `repaintDirty_`, so the card under the
	// hand did repaint on the next tick. Nothing on ModState's list did — and the mod panel's route
	// list draws these amounts too.
	//
	// FOUND WHILE CHECKING WHETHER THE CONVENTION EXISTED, before making `RouteEdit` join it. It did
	// not, so the fix was two lines rather than one.
	FilterFixture f;
	f.arm(backplane::Source::core(0));
	ui::Knob* cutoff = f.card->knobsForTest().front().get();
	REQUIRE(cutoff != nullptr);
	REQUIRE(cutoff->mappable());

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	const visage::Point centre = f.faceCentre(cutoff);
	REQUIRE(f.root->frameAtPoint(centre) == cutoff);

	const size_t before = mod->framesToldForTest();
	dragThroughRoot(f.root, centre, { centre.x, centre.y - 35.0f });
	f.settle();

	// THE DRAG REALLY WROTE AN AMOUNT — asked of the control the drag went through, not recomputed
	// from a slot and an ordinal here, so this cannot agree with the widget about a column both are
	// wrong about.
	auto* control = dynamic_cast<ui::DeviceControl*>(cutoff->control());
	REQUIRE(control != nullptr);
	REQUIRE(control->isDestination());
	REQUIRE(f.bridge.routeAmount(backplane::Source::core(0), control->destination()) > 0.0f);

	// AND IT ANNOUNCED IT. Without this the fx lane's amounts move and the surfaces that list them
	// do not.
	CHECK(mod->framesToldForTest() > before);
}
TEST_CASE("forgetFrameTree clears all three redraw lists, and each one independently") {
	// THE FUNCTION THAT EXISTS TO PREVENT THREE USE-AFTER-FREES HAD NO CASE OF ITS OWN. Every test of
	// it was indirect — through a card teardown or an overlay dismissal — which is how it came to be
	// called with a different subset of its lists at every one of its call sites.
	//
	// AND THE SECOND PARAMETER USED TO HAVE NO IDENTITY. `watchers` meant the STATE channel at
	// `fx_lane.cpp` and the MODULATION channel at `mod_panel.cpp`: one slot, two different lists, and
	// nothing at either site to say which. A caller needing both made TWO CALLS, so "this list does
	// not apply here" and "I forgot this list" were the same text. That ambiguity shipped three times
	// in one week — 96d94be (SIGSEGV), 25282de (the same, one door over) and 683070d (an amount
	// written to a list nobody was told about).
	//
	// ASSERTED PER LIST, WHICH IS THE POINT. A single "everything is clean" check would go on
	// reporting success while covering one list, which is the failure this whole refactor is about.
	// Every list is cleared in its own CHECK and, below, every list's OMISSION is reddable on its own.
	WidgetRegistry registry;
	ui::FrameWatchers state;
	ui::FrameWatchers modulation;

	// A REAL SUBTREE, THREE DEEP. The root is not the problem — the bug this function exists for is
	// the knobs INSIDE a card — so a case over a lone frame would pass against a version that never
	// recursed.
	visage::Frame root;
	visage::Frame child;
	visage::Frame grandchild;
	root.addChild(&child);
	child.addChild(&grandchild);
	const std::array<visage::Frame*, 3> all { &root, &child, &grandchild };

	const auto id = static_cast<clap_id>(ParamId::Path);
	// **THE TOKENS ARE PARKED, NOT DROPPED.** `watch` is `[[nodiscard]]` now — see `ui::Watch` — and
	// this case is about `forgetFrameTree` sweeping a subtree, which is a different road from a token
	// releasing itself. Holding them here keeps the subscriptions alive for the sweep to find, which
	// is exactly what the case needs; letting them fall would unsubscribe everything before the
	// assertion and the case would pass while testing nothing.
	std::vector<ui::Watch> parked;
	const auto subscribeAll = [&] {
		for (visage::Frame* frame : all) {
			parked.push_back(registry.watchToken(id, frame));
			parked.push_back(state.watch(frame));
			parked.push_back(modulation.watch(frame));
		}
	};
	const auto registryHolds = [&](const visage::Frame* frame) {
		for (const visage::Frame* watcher : registry.watchers(id)) {
			if (watcher == frame) {
				return true;
			}
		}
		return false;
	};

	subscribeAll();
	// THE SETUP REALLY SUBSCRIBED, SAID FIRST AND PER LIST. Without this every assertion below would
	// be satisfied by three empty lists, which is exactly what a torn-down editor leaves behind.
	for (visage::Frame* frame : all) {
		REQUIRE(registryHolds(frame));
		REQUIRE(state.watches(frame));
		REQUIRE(modulation.watches(frame));
	}

	SUBCASE("all three named clears all three") {
		ui::forgetFrameTree(&root, &registry, &state, &modulation);
		for (visage::Frame* frame : all) {
			CHECK_FALSE(registryHolds(frame));
			CHECK_FALSE(state.watches(frame));
			CHECK_FALSE(modulation.watches(frame));
		}
	}

	SUBCASE("a null registry leaves the registry alone and clears the other two") {
		// A NULL IS A DECLARATION RATHER THAN AN OMISSION, and this is what makes that true: the
		// list a caller declined to name is left exactly as it was. `ExpandedOverlay` passes null
		// here because an ExpandedView's knobs are built through the `device::Modulatable`
		// constructor and never register.
		ui::forgetFrameTree(&root, nullptr, &state, &modulation);
		for (visage::Frame* frame : all) {
			CHECK(registryHolds(frame));
			CHECK_FALSE(state.watches(frame));
			CHECK_FALSE(modulation.watches(frame));
		}
	}

	SUBCASE("a null state list leaves the state list alone and clears the other two") {
		// `ModPanel` passes null here — it holds no `FrameWatchers*` and is on no state channel. Under
		// the two-argument signature it put the MODULATION list in this slot instead, which is the
		// confusion the third parameter removes.
		ui::forgetFrameTree(&root, &registry, nullptr, &modulation);
		for (visage::Frame* frame : all) {
			CHECK_FALSE(registryHolds(frame));
			CHECK(state.watches(frame));
			CHECK_FALSE(modulation.watches(frame));
		}
	}

	SUBCASE("a null modulation list leaves the modulation list alone and clears the other two") {
		// AND THIS IS THE ONE NOBODY MEANT TO PASS. Every historical instance of this bug is a caller
		// that behaved as though it had written this null without writing it.
		ui::forgetFrameTree(&root, &registry, &state, nullptr);
		for (visage::Frame* frame : all) {
			CHECK_FALSE(registryHolds(frame));
			CHECK_FALSE(state.watches(frame));
			CHECK(modulation.watches(frame));
		}
	}

	SUBCASE("a frame outside the subtree is untouched") {
		// THE OTHER HALF OF "CLEARS EVERYTHING UNDER IT": a version that cleared the whole list would
		// satisfy every check above and would silently unsubscribe every other surface in the editor.
		visage::Frame stranger;
		parked.push_back(registry.watchToken(id, &stranger));
		parked.push_back(state.watch(&stranger));
		parked.push_back(modulation.watch(&stranger));
		ui::forgetFrameTree(&root, &registry, &state, &modulation);
		CHECK(registryHolds(&stranger));
		CHECK(state.watches(&stranger));
		CHECK(modulation.watches(&stranger));
	}
}
TEST_CASE("a card cycle returns EVERY watcher list in the editor to where it started") {
	// THE GUARD `forgetFrameTree`'S SIGNATURE CANNOT PROVIDE. Making its three lists named parameters
	// is what stops one being FORGOTTEN; it says nothing about a list the function cannot reach at
	// all, and there are four of those.
	//
	// MEASURED, by enumerating every `FrameWatchers` in src/: SEVEN lists exist and `forgetFrameTree`
	// reaches THREE. The registry, the state channel and `ModState`'s have an exit; `previewWatchers`,
	// `EditHistory`'s, `SoundSelection`'s and `StageMode`'s do not. Those four are safe TODAY because
	// every subscriber they have is an editor-lifetime surface — a fact about who happened to
	// register rather than a guarantee, and precisely the property the three shipped
	// dangling-watcher bugs had until a card became removable.
	//
	// SO THE CLAIM IS A RETURN RATHER THAN A CLEANUP: add a device to each chain, remove both, and
	// require every list to come back to its baseline. Nothing here knows which widgets a card
	// builds, how many value boxes it has, or which list any of them chose — so the day a future card
	// subscribes to the history or the selection, the count grows and this reddens with nobody having
	// had to think about it. That is the half a signature cannot buy.
	//
	// THE ENUMERATION IS THE EDITOR'S, NOT THIS CASE'S. `Gui::watcherCountsForTest` fills one struct
	// from all seven members; a case that listed them itself would go on passing the day an eighth
	// list is added and the omission would be invisible from here.
	ModFixture f;
	ui::FxLane* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);

	// SETTLE FIRST. Both strips build a card per pool slot up front, so a baseline taken before that
	// measures construction rather than leaking — the sibling case's lesson, applied to every list
	// rather than to the one it watches.
	f.panel->resized();
	lane->resized();
	const Gui::WatcherCounts base = f.gui.watcherCountsForTest();

	// THE LISTS ARE REALLY POPULATED, said before anything is cycled. A struct of zeros would satisfy
	// every equality below, and zeros are exactly what a torn-down editor leaves behind.
	REQUIRE(base.registry > 0);
	REQUIRE(base.state > 0);
	REQUIRE(base.preview > 0);
	REQUIRE(base.modulation > 0);
	REQUIRE(base.selection > 0);
	REQUIRE(base.mode > 0);
	// AND THE HISTORY IS THE ONE THAT MAY LEGITIMATELY BE EMPTY on this fixture — ExploreStage is its
	// only subscriber and it is not always built — so it is required to be *observable* rather than
	// non-zero, and its return is still asserted below.
	CAPTURE(base.history);

	// THE CYCLE: a device into each chain, then both back out. Both lanes, because their teardowns
	// are two functions that have already proved they can lose a line independently.
	const size_t rackSlot = f.bridge.addRackDevice(mod::mseg::kTraits.tag);
	REQUIRE(rackSlot < kMaxRackSlots);
	f.panel->resized();
	const size_t fxSlot = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	REQUIRE(fxSlot < kMaxFxSlots);
	lane->resized();

	// THE CYCLE REALLY REBUILT SOMETHING. Without this the whole case is arithmetic about numbers
	// that never changed — if no list moved, nothing was subscribed and nothing was destroyed.
	const Gui::WatcherCounts mid = f.gui.watcherCountsForTest();
	REQUIRE((mid.state != base.state || mid.modulation != base.modulation));

	REQUIRE(f.bridge.removeFxDevice(fxSlot));
	lane->resized();
	REQUIRE(f.bridge.removeRackDevice(rackSlot));
	f.panel->resized();

	// AND EVERY LIST IS BACK, ONE CHECK EACH so a failure names WHICH list leaked rather than only
	// that something did.
	const Gui::WatcherCounts after = f.gui.watcherCountsForTest();
	CHECK(after.registry == base.registry);
	CHECK(after.state == base.state);
	CHECK(after.preview == base.preview);
	CHECK(after.modulation == base.modulation);
	CHECK(after.history == base.history);
	CHECK(after.selection == base.selection);
	CHECK(after.mode == base.mode);

	// WHICH LEGS ARE LIVE AND WHICH ARE INERT, RECORDED RATHER THAN LEFT TO BE MISREAD. Deleting
	// `forgetFrameTree`'s state leg reddens `after.state`, and its modulation leg reddens
	// `after.modulation`. Deleting its REGISTRY leg reddens NOTHING here, and that is not weakness in
	// this case: measured across this exact cycle the registry total does not move at all, because
	// card knobs are built through the `device::Modulatable` constructor which deliberately does not
	// register (knob.cpp, controls.cpp). There is nothing on that list to leak. The registry leg is
	// covered directly by `forgetFrameTree clears all three redraw lists, and each one independently`,
	// and the two cases are complementary — neither is sufficient alone.
	CHECK(mid.registry == base.registry);
}
TEST_CASE("a handle grabbed off-centre moves BY the hand, not TO it") {
	// The popout editor felt twitchy.
	//
	// SOMETHING CAN. `hitAt` takes a press anywhere within `kGrabRadius` of a handle, and the drag
	// road maps the RAW POINTER to a value — so the offset between where the hand pressed and where
	// the handle actually is gets spent, all at once, on the first drag event. The handle snaps to
	// the pointer instead of following it.
	//
	// MEASURED BEFORE THE FIX, on this panel, through these handlers:
	//     press 0px off-centre, hand moves 1px  -> handle travels  1.00px   (error 0.00px)
	//     press 6px off-centre, hand moves 1px  -> handle travels  6.74px   (error 6.00px)
	//     press 12px off-centre, hand moves 1px -> handle travels 12.73px   (error 12.00px)
	// The error IS the press offset, to five figures. On the 1.75x display twelve logical pixels
	// is about twenty-one device pixels of jump in response to one pixel of hand movement, and the
	// radius axis is warped, so the same slip is a nudge near the origin and a leap near the rim.
	// That is the "sometimes fine, sometimes wild" the word describes.
	//
	// AND IT IS NOT ON THE PRESS, WHICH IS WHERE I EXPECTED IT. Measured first as a press with no
	// drag: zero movement at every offset out to 12px, because `onMouseDown` only latches `held_` and
	// opens the gestures. A case written against that guess would have passed on the broken code.
	//
	// THE PATTERN IS ALREADY IN THIS CODEBASE, one file over. `HarmScope` stores `anchorNotchDx_` at
	// its press and says why in as many words: *"so a horizontal drag moves it BY the hand rather
	// than TO the hand... a mapping that put the notch under the pointer would jump it sideways the
	// moment a grip was touched." Same sentence, same fix, three surfaces that did not get it.
	//
	// WHY NO EXISTING CASE CAUGHT IT: every drag case in this file presses at `poleCentre(probe)` or
	// `zeroCentre(probe)` exactly, where the offset is zero by construction. The bug lives only
	// off-centre, and nothing had ever pressed off-centre. The same lesson as the round-trip sweep —
	// the sample point was where the bug is not.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();
	panel->selectFrame(0);

	const size_t probe = 0;
	ui::FxControl angle { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
		                  dr::bandFieldIndex(0, probe, dr::PoleAngle) };
	ui::FxControl radius { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
		                   dr::bandFieldIndex(0, probe, dr::PoleRadius) };

	SUBCASE("a press alone moves nothing, however far off centre it lands") {
		// A GUARD ON THE FIX RATHER THAN A DEMONSTRATION OF THE BUG, and said so plainly: this passes
		// on the broken code too. It is here because the obvious wrong fix — calling the drag road
		// from `onMouseDown` so the handle is "under the hand" from the first frame — would make a
		// press that was only ever a press into a patch edit. The perform stage does exactly that
		// with its stars, measured at 14px of movement from a press with no drag.
		const visage::Point centre = panel->poleCentre(probe);
		for (const float off : { 0.0f, 6.0f, 12.0f }) {
			CAPTURE(off);
			const visage::Point press { centre.x + off * 0.7071f, centre.y + off * 0.7071f };
			REQUIRE(panel->hitAt(press).what == ui::DrawnExpanded::Grab::Pole);
			const double a0 = angle.value();
			const double r0 = radius.value();
			panel->onMouseDown().callback(at(press.x, press.y));
			panel->onMouseUp().callback(at(press.x, press.y));
			f.bridge.refresh();
			CHECK(angle.value() == doctest::Approx(a0));
			CHECK(radius.value() == doctest::Approx(r0));
		}
	}

	SUBCASE("a drag moves the handle by exactly the hand's travel") {
		// THE REFERENCE IS A CENTRED DRAG OF THE SAME TRAVEL, not a restatement of the axis maths.
		// The axes are private and re-deriving the warped radius here would be a second copy of the
		// implementation that goes stale the day a shaping constant moves. Instead: grab the handle
		// dead centre — the one press with no offset to lose — drag it by the travel, and record what
		// the controls hold. THAT is where the handle belongs, and the claim is that grabbing it
		// anywhere within the grab radius and making the same hand movement puts it in the same place.
		//
		// A PIXEL ASSERTION SITS BESIDE THE VALUES, so a pair of values that agreed for some other
		// reason is caught by geometry and a geometry that agreed is caught by the values.
		constexpr float kTravelX = 9.0f;
		constexpr float kTravelY = -7.0f;
		const visage::Point home = panel->poleCentre(probe);

		const auto dragFrom = [&](visage::Point press) {
			panel->onMouseDown().callback(at(press.x, press.y));
			panel->onMouseDrag().callback(at(press.x + kTravelX, press.y + kTravelY));
			panel->onMouseUp().callback(at(press.x + kTravelX, press.y + kTravelY));
			f.bridge.refresh();
		};
		const auto putBack = [&] {
			const visage::Point now = panel->poleCentre(probe);
			panel->onMouseDown().callback(at(now.x, now.y));
			panel->onMouseDrag().callback(at(home.x, home.y));
			panel->onMouseUp().callback(at(home.x, home.y));
			f.bridge.refresh();
			REQUIRE(std::hypot(panel->poleCentre(probe).x - home.x,
			                   panel->poleCentre(probe).y - home.y)
			        < 0.5f);
		};

		dragFrom(home);
		const double refAngle = angle.value();
		const double refRadius = radius.value();
		const visage::Point refAt = panel->poleCentre(probe);
		// THE REFERENCE REALLY MOVED, or every comparison below is between two unmoved handles.
		REQUIRE(std::hypot(refAt.x - home.x, refAt.y - home.y) > 5.0f);
		putBack();

		for (const float off : { 6.0f, 12.0f }) {
			CAPTURE(off);
			const visage::Point press { home.x + off * 0.7071f, home.y + off * 0.7071f };
			REQUIRE(panel->hitAt(press).what == ui::DrawnExpanded::Grab::Pole);
			dragFrom(press);

			CHECK(angle.value() == doctest::Approx(refAngle).epsilon(0.001));
			CHECK(radius.value() == doctest::Approx(refRadius).epsilon(0.001));
			const visage::Point after = panel->poleCentre(probe);
			CHECK(std::hypot(after.x - refAt.x, after.y - refAt.y) < 0.5f);
			putBack();
		}
	}
}
TEST_CASE("a waypoint dot grabbed off-centre moves BY the hand too") {
	// THE SAME DEFECT AS `DrawnExpanded`'s, ON THE CARD RATHER THAN THE PANEL, and found by asking
	// every planar-drag surface the same question rather than by a second report. `dotAt` takes a
	// press anywhere within `kGrabRadius` (11px here, 13 there) and `dragDot` maps the raw pointer
	// onto the plane, so the press offset is spent on the first drag event.
	//
	// MEASURED BEFORE THE FIX, through these handlers:
	//     press  0px off-centre, hand moves 1px -> dot travels  1.00px
	//     press  5px off-centre, hand moves 1px -> dot travels  5.75px
	//     press 10px off-centre, hand moves 1px -> dot travels 10.73px
	// The same signature to the same precision as the panel's, which is what makes it one bug with
	// two instances rather than two bugs.
	//
	// THE FIXTURE WRITES FOUR DISTINCT WAYPOINTS FIRST, and that is not decoration. At the declared
	// defaults the dots coincide, so `dotAt(dotCentre(0))` answers a different dot and a case that
	// skipped this would fail at its own REQUIRE — which is how I found out, twice.
	namespace wp = feathers::fx::waypoint;
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(wp::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	f.lane->resized();
	auto* card = dynamic_cast<ui::WaypointCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	card->setBounds(0.0f, 0.0f, 240.0f, card->laneHeight());
	card->resized();
	// AND THE PLANE HAS A SIZE. An unsized card makes `dragDot` return at its own width guard, and a
	// probe against it reports zero movement at EVERY offset — including the zero-offset control,
	// which is what exposed it. A venue has to be shown able to express the outcome.
	REQUIRE(card->planeBounds().width() > 0.0f);

	const auto write = [&](wp::Param p, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor, static_cast<size_t>(p) };
		c.setValue(v);
		f.bridge.refresh();
	};
	for (size_t i = 0; i < wp::kWaypoints; ++i) {
		const size_t base = static_cast<size_t>(wp::Param::Cutoff0) + i * 4;
		write(static_cast<wp::Param>(base), std::log2(250.0 * std::pow(2.0, static_cast<double>(i))));
		write(static_cast<wp::Param>(base + 1), 0.1 * static_cast<double>(i + 1));
	}

	constexpr size_t kProbe = 0;
	ui::FxControl cutoff { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor,
		                   static_cast<size_t>(wp::Param::Cutoff0) };
	ui::FxControl reso { f.bridge, f.bridge.fxDeviceAt(slot), wp::kDescriptor,
		                 static_cast<size_t>(wp::Param::Resonance0) };

	constexpr float kTravelX = 7.0f;
	constexpr float kTravelY = -5.0f;
	const visage::Point home = card->dotCentre(kProbe);
	REQUIRE(card->dotAt(home) == kProbe);

	const auto dragFrom = [&](visage::Point press) {
		card->onMouseDown().callback(at(press.x, press.y));
		card->onMouseDrag().callback(at(press.x + kTravelX, press.y + kTravelY));
		card->onMouseUp().callback(at(press.x + kTravelX, press.y + kTravelY));
		f.bridge.refresh();
	};
	const auto putBack = [&] {
		const visage::Point now = card->dotCentre(kProbe);
		card->onMouseDown().callback(at(now.x, now.y));
		card->onMouseDrag().callback(at(home.x, home.y));
		card->onMouseUp().callback(at(home.x, home.y));
		f.bridge.refresh();
		REQUIRE(std::hypot(card->dotCentre(kProbe).x - home.x, card->dotCentre(kProbe).y - home.y)
		        < 0.5f);
	};

	// THE REFERENCE IS A CENTRED DRAG OF THE SAME TRAVEL — the one press with no offset to lose —
	// rather than a restatement of the plane's axis maths, which would be a second copy of the
	// implementation that goes stale when a constant moves.
	dragFrom(home);
	const double refCutoff = cutoff.value();
	const double refReso = reso.value();
	const visage::Point refAt = card->dotCentre(kProbe);
	REQUIRE(std::hypot(refAt.x - home.x, refAt.y - home.y) > 3.0f); // it really moved
	putBack();

	for (const float off : { 5.0f, 10.0f }) {
		CAPTURE(off);
		const visage::Point press { home.x + off * 0.7071f, home.y + off * 0.7071f };
		REQUIRE(card->dotAt(press) == kProbe);
		dragFrom(press);
		CHECK(cutoff.value() == doctest::Approx(refCutoff).epsilon(0.001));
		CHECK(reso.value() == doctest::Approx(refReso).epsilon(0.001));
		const visage::Point after = card->dotCentre(kProbe);
		CHECK(std::hypot(after.x - refAt.x, after.y - refAt.y) < 0.5f);
		putBack();
	}
}
TEST_CASE("clicking and holding a star grabs it rather than placing it") {
	// THE DESIGN RULE: clicking and holding a star grabs it.
	//
	// IT DID NOT. `onMouseDown` called `dragStarTo` directly, so the press itself moved the sound to
	// wherever the pointer landed — up to `kStarGrabPixels` (16) away from the star it was aimed at.
	// The file's own comment calls this "grabbing a sound"; the code was placing one.
	//
	// AND THIS ONE EDITS PATCH STATE, which is what separates it from the two handle surfaces fixed
	// alongside it. A sound's place is a parameter with a host gesture around it, so the press did
	// not merely look wrong — it posted a complete edit the host records as undoable.
	//
	// MEASURED THROUGH THESE HANDLERS, before the fix — movement AND the gesture bracket:
	//     press  0px off-centre, no drag -> moved  0.00px, gestures 0 begin / 0 value / 0 end
	//     press  8px off-centre, no drag -> moved  8.00px, gestures 2 / 2 / 2
	//     press 15px off-centre, no drag -> moved 15.00px, gestures 2 / 2 / 2
	// Two of each because a place is two parameters. The zero-offset row is the control: it proves
	// the counters can read zero, which is what makes the 2/2/2 attributable to the offset rather
	// than to the fixture.
	//
	// THE GESTURE COUNT IS THE HALF THAT MATTERS MOST and it is asserted separately, because a fix
	// that restored the VALUE while still opening and closing the bracket would leave an undo step
	// that undoes nothing — the shape the backlog already carries against StanceEdit. `commit()`
	// opens a gesture only for a value that actually moves, so the honest fix produces no bracket at
	// all rather than a balanced empty one.
	//
	// THE PUCK IS DELIBERATELY NOT TOUCHED. Its click-to-position is intended and the file says so:
	// "the puck can be reached anywhere else in the disk". The rule is about the star.
	StageFixture f;
	constexpr size_t kSlot = 1;
	const clap_id px = static_cast<clap_id>(feathers::soundParam(kSlot, feathers::SoundField::PlaceX));
	const clap_id py = static_cast<clap_id>(feathers::soundParam(kSlot, feathers::SoundField::PlaceY));

	/// Every event the editor posted for THIS sound's place since the last call: begins, values, ends.
	const auto drainPlace = [&] {
		std::array<size_t, 3> counts { 0, 0, 0 };
		f.bridge.drain([&](const UiParamEvent& e) {
			if (e.id != px && e.id != py) {
				return;
			}
			if (e.kind == UiParamEvent::Kind::GestureBegin) { ++counts[0]; }
			else if (e.kind == UiParamEvent::Kind::Value) { ++counts[1]; }
			else { ++counts[2]; }
		});
		return counts;
	};
	drainPlace(); // whatever the fixture's own setup posted is not this case's business

	const BlendPoint home = f.placeOf(kSlot);
	const visage::Point centre = f.stage.toPixels(home);

	SUBCASE("a press and release moves the sound nowhere and costs no undo step") {
		for (const float off : { 0.0f, 8.0f, 15.0f }) {
			CAPTURE(off);
			const visage::Point press { centre.x + off * 0.7071f, centre.y + off * 0.7071f };
			f.stage.onMouseDown().callback(at(press.x, press.y));
			f.stage.onMouseUp().callback(at(press.x, press.y));
			f.bridge.refresh();

			// THE VALUE, FIRST AND ON BOTH AXES.
			const BlendPoint after = f.placeOf(kSlot);
			CHECK(after.x == doctest::Approx(home.x));
			CHECK(after.y == doctest::Approx(home.y));

			// AND THE UNDO STEP THAT MUST NOT EXIST. Not "balanced" — absent.
			const std::array<size_t, 3> counts = drainPlace();
			CHECK(counts[0] == 0u);
			CHECK(counts[1] == 0u);
			CHECK(counts[2] == 0u);
		}
	}

	SUBCASE("a drag moves the star by the hand's travel wherever it was grabbed") {
		// THE REFERENCE IS A CENTRED DRAG OF THE SAME TRAVEL — the one press with no offset to lose.
		constexpr float kTravelX = 11.0f;
		constexpr float kTravelY = -8.0f;
		const auto dragFrom = [&](visage::Point press) {
			f.stage.onMouseDown().callback(at(press.x, press.y));
			f.stage.onMouseDrag().callback(at(press.x + kTravelX, press.y + kTravelY));
			f.stage.onMouseUp().callback(at(press.x + kTravelX, press.y + kTravelY));
			f.bridge.refresh();
			drainPlace();
		};
		const auto putBack = [&] {
			const visage::Point now = f.stage.toPixels(f.placeOf(kSlot));
			f.stage.onMouseDown().callback(at(now.x, now.y));
			f.stage.onMouseDrag().callback(at(centre.x, centre.y));
			f.stage.onMouseUp().callback(at(centre.x, centre.y));
			f.bridge.refresh();
			drainPlace();
		};

		dragFrom(centre);
		const BlendPoint reference = f.placeOf(kSlot);
		const visage::Point refAt = f.stage.toPixels(reference);
		// IT REALLY MOVED, or every comparison below is between two unmoved stars.
		REQUIRE(std::hypot(refAt.x - centre.x, refAt.y - centre.y) > 5.0f);
		putBack();

		for (const float off : { 8.0f, 15.0f }) {
			CAPTURE(off);
			const visage::Point press { centre.x + off * 0.7071f, centre.y + off * 0.7071f };
			dragFrom(press);
			const BlendPoint after = f.placeOf(kSlot);
			CHECK(after.x == doctest::Approx(reference.x).epsilon(0.002));
			CHECK(after.y == doctest::Approx(reference.y).epsilon(0.002));
			const visage::Point moved = f.stage.toPixels(after);
			CHECK(std::hypot(moved.x - refAt.x, moved.y - refAt.y) < 0.5f);
			putBack();
		}
	}
}
TEST_CASE("a control refuses a source that cannot reach it, on every surface that draws one") {
	// `ModDisplay::routable` IS DOCUMENTED AS "ONE ANSWER RATHER THAN TWO COMPUTATIONS" — the gate on
	// the drag and the glow, asked once so a knob and a value box cannot drift apart. There are FOUR
	// computations of it, and MEASURED: deleting the `maySourceRouteTo` half of three of them left all
	// 36 suites green. Only `ParamControl`'s was exercised.
	//
	// I FIRST READ THAT AS UNREACHABLE AND WAS WRONG, which is why this case exists rather than a
	// note. The reasoning was that `maySourceRouteToIgnoringDevice` refuses only a weight source at a
	// PUCK destination, and a puck is a parameter, so no device control could ever be refused.
	// The instrument says otherwise, on the shipped tree:
	//
	//     filter outputCount            = 0        -> every source column of an fx lane is SLACK
	//     deviceSourceOf(lane, 0)       = 12       isDeviceSource=true, isLiveDeviceSource=FALSE
	//     ModState::focus(12)                      accepted; source()=12, armed=true
	//     maySourceRouteTo(12 -> cutoff)= FALSE
	//     display()                     isDestination=true armed=true routable=FALSE
	//
	// The term I had missed is the SOURCE half — `isDeviceSource(source) && !isLiveDeviceSource(...)`
	// — which `maySourceRouteTo` adds on top of the ignoring-device rules. A device that declares no
	// outputs has nothing but slack columns, and `focus()` is bounded only by `kModSourceCount`.
	//
	// SO WITHOUT THE GATE the control would glow and take an amount drag from a source that cannot
	// reach it, and the bridge would then refuse the write — a lit control offering a gesture that
	// does nothing, which is the honesty rule's exact failure.
	//
	// THE LIVE-SOURCE ROW IS THE CONTROL and is not decoration: without it, a `routable` that was
	// always false would satisfy every refusal below.
	namespace flt = feathers::fx::filter;
	namespace dh = feathers::mod::dahdsr;
	namespace ms = feathers::mod::mseg;

	FilterFixture f;
	ui::ModState mod;
	ui::EditHistory history;

	/// Arm `source` and report what `control` then says about itself.
	const auto displayWith = [&](device::Modulatable& control, backplane::Source source) {
		mod.focus(source);
		mod.arm(true);
		return control.display();
	};
	/// WHETHER A SOURCE IS LIVE — the declaration of the device under its id, asked of the record.
	const auto live = [&](backplane::Source s) { return backplane::SourceView({}, f.bridge.devices()).isLive(s); };

	SUBCASE("an fx control") {
		const DeviceInstanceId filterId = f.bridge.fxDeviceAt(0);
		REQUIRE(filterId != kNoInstance);
		const feathers::device::Descriptor* desc = feathers::deviceForTag(flt::kTraits.tag);
		REQUIRE(desc != nullptr);
		// THE PREMISE, ASSERTED RATHER THAN ASSUMED: this device publishes nothing, so every output
		// index it could be named by is slack. The day it declares an output this REQUIRE fails and
		// says to pick another.
		REQUIRE(desc->outputCount() == 0u);
		const backplane::Source slack = backplane::Source::output(filterId, 0);
		REQUIRE(slack.isDevice());
		REQUIRE_FALSE(live(slack));

		ui::FxControl cutoff { f.bridge, f.bridge.fxDeviceAt(0), flt::kDescriptor,
			                   static_cast<size_t>(flt::Param::Cutoff) };
		cutoff.setModulation(&mod, &history);

		const device::ModDisplay refused = displayWith(cutoff, slack);
		// IT IS A DESTINATION AND A SOURCE IS ARMED — so `routable` is the only thing standing
		// between the hand and a drag the bridge will refuse.
		CHECK(refused.isDestination);
		CHECK(refused.armed);
		CHECK_FALSE(refused.routable);

		// AND A MACRO, WHICH CAN REACH IT.
		const device::ModDisplay allowed = displayWith(cutoff, backplane::Source::core(0));
		CHECK(allowed.isDestination);
		CHECK(allowed.armed);
		CHECK(allowed.routable);
	}

	SUBCASE("a rack control") {
		// A RACK DEVICE DOES PUBLISH, so its slack columns are the ones past its declared count
		// rather than all of them — a different way to reach the same refusal.
		//
		// **THE SOURCE COMES FROM AN UPSTREAM DEVICE, AND THAT KEEPS THIS CASE ABOUT SLACK.** It used
		// to take both the live and the slack source from the very device the control sits on, which
		// the routing rule now refuses outright (2026-09-05: a node's bus may not carry
		// its own device's output). Had the source stayed put, the `refused` arm would have passed for
		// the WRONG REASON — the order, not the slackness — and the case would have stopped saying
		// what its title says. Both sources now come from a device that is legitimately upstream, so
		// the only thing separating them is whether the output is declared.
		const size_t upstream = f.bridge.addRackDevice(dh::kTraits.tag);
		REQUIRE(upstream < feathers::kMaxRackSlots);
		const size_t slot = f.bridge.addRackDevice(dh::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		REQUIRE(slot > upstream); // the rack appends, so this is the order — said rather than assumed
		const DeviceInstanceId upstreamId = f.bridge.rackDeviceAt(upstream);
		REQUIRE(upstreamId != kNoInstance);
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dh::kTraits.tag);
		REQUIRE(desc != nullptr);
		REQUIRE(desc->outputCount() < feathers::device::kMaxDeviceOutputs); // or there is no slack to find

		const backplane::Source liveSource = backplane::Source::output(upstreamId, 0);
		const backplane::Source slack = backplane::Source::output(upstreamId, feathers::device::kMaxDeviceOutputs - 1);
		REQUIRE(live(liveSource));
		REQUIRE_FALSE(live(slack));

		ui::RackControl attack { f.bridge, f.bridge.rackDeviceAt(slot), dh::Param::Attack, &mod, &history };
		const device::ModDisplay refused = displayWith(attack, slack);
		CHECK(refused.isDestination);
		CHECK(refused.armed);
		CHECK_FALSE(refused.routable);

		const device::ModDisplay allowed = displayWith(attack, liveSource);
		CHECK(allowed.isDestination);
		CHECK(allowed.routable);
	}

	SUBCASE("an mseg control") {
		// THE SAME UPSTREAM SOURCE AS THE RACK SUBCASE ABOVE, and for the reason stated there: taking
		// the source from the control's own device would make the refusal about the order rather than
		// about the slack this case is named for.
		const size_t upstream = f.bridge.addRackDevice(ms::kTraits.tag);
		REQUIRE(upstream < feathers::kMaxRackSlots);
		const size_t slot = f.bridge.addRackDevice(ms::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		REQUIRE(slot > upstream);
		const DeviceInstanceId upstreamId = f.bridge.rackDeviceAt(upstream);
		REQUIRE(upstreamId != kNoInstance);
		const feathers::device::Descriptor* desc = feathers::deviceForTag(ms::kTraits.tag);
		REQUIRE(desc != nullptr);
		REQUIRE(desc->outputCount() < feathers::device::kMaxDeviceOutputs);

		const backplane::Source liveSource = backplane::Source::output(upstreamId, 0);
		const backplane::Source slack = backplane::Source::output(upstreamId, feathers::device::kMaxDeviceOutputs - 1);
		REQUIRE(live(liveSource));
		REQUIRE_FALSE(live(slack));

		ui::MsegControl rate { f.bridge, f.bridge.rackDeviceAt(slot), ms::Param::StartLevel, &mod, &history };
		const device::ModDisplay refused = displayWith(rate, slack);
		CHECK(refused.isDestination);
		CHECK(refused.armed);
		CHECK_FALSE(refused.routable);

		const device::ModDisplay allowed = displayWith(rate, liveSource);
		CHECK(allowed.isDestination);
		CHECK(allowed.routable);
	}
}
TEST_CASE("a rail that grew taller does not stay scrolled past its own contents") {
	// THREE RAILS CLAMP THEIR SCROLL IN `resized()` FOR ONE REASON, stated in the Engine rail's own
	// comment: *"A rail that got taller has room for more of its contents, and a scroll left where it
	// was would hold the top of the panel off a frame that no longer needs it held anywhere."
	//
	// MEASURED: deleting that clamp reddens `gui-widgets` for FxLane and for ModPanel, and NOTHING
	// for EngineRail — 36 of 36 still green. One rule, three implementations, two guarded.
	//
	// AND IT IS THE `resized()` CLAMP SPECIFICALLY, not the wheel. `setScroll` clamps on its own and
	// the wheel goes through it, so scrolling can never leave the range; the resize road cannot use
	// `setScroll` because that lays out and would recurse, so it clamps by hand — and that hand-written
	// one is the untested copy. A case driven through the wheel would pass against the bug.
	//
	// THE GESTURE IS ORDINARY: scroll a rail down, then make the window taller. Without the clamp the
	// panel stays held off the top and draws blank space where its first rows belong.
	ParamBridge bridge;
	/// THE SAME BRIDGE AS THE DEVICE SEAM. Cards and panels are converging onto `backplane::DeviceHost`
	/// so the rack can build them; `BridgeDeviceHost` is what keeps Feathers handing them the parameter
	/// road underneath, and a fixture needs one for the same reason `Gui::Impl` does.
	feathers::BridgeDeviceHost deviceHost { bridge };
	WidgetRegistry registry;
	ui::EngineRail engineRail(bridge, registry);

	// SHORT ENOUGH TO SCROLL. If the panel already fits its contents there is no scroll to strand and
	// the case would pass by asking nothing — the same shape as a degenerate plane.
	engineRail.setBounds(0.0f, 0.0f, ui::EngineRail::kWidth, 120.0f);
	engineRail.resized();
	REQUIRE(engineRail.maxScroll() > 0.0f);

	engineRail.setScroll(engineRail.maxScroll());
	const float parked = engineRail.scroll();
	REQUIRE(parked > 0.0f); // it really scrolled

	// NOW GIVE IT ROOM FOR EVERYTHING. `maxScroll()` becomes zero, so any surviving scroll is the
	// panel holding itself off a top it no longer needs to.
	engineRail.setBounds(0.0f, 0.0f, ui::EngineRail::kWidth, 4000.0f);
	engineRail.resized();
	REQUIRE(engineRail.maxScroll() == doctest::Approx(0.0f));
	CHECK(engineRail.scroll() == doctest::Approx(0.0f));

	SUBCASE("and a partial grow lands on the new maximum rather than the old scroll") {
		// THE HALFWAY CASE, because clamping to zero is also what "always reset on resize" would do,
		// and that would be a different bug — a rail that forgets where you were every time the
		// window moves. Growing to a height that still scrolls must keep the scroll, capped.
		engineRail.setBounds(0.0f, 0.0f, ui::EngineRail::kWidth, 120.0f);
		engineRail.resized();
		engineRail.setScroll(engineRail.maxScroll());
		const float wasMax = engineRail.maxScroll();
		REQUIRE(wasMax > 0.0f);

		engineRail.setBounds(0.0f, 0.0f, ui::EngineRail::kWidth, 200.0f);
		engineRail.resized();
		const float nowMax = engineRail.maxScroll();
		REQUIRE(nowMax > 0.0f);    // still scrollable, or this is the zero case again
		REQUIRE(nowMax < wasMax);  // and it really did grow
		CHECK(engineRail.scroll() == doctest::Approx(nowMax));
	}
}
TEST_CASE("a dismissed expanded view leaves nothing drawn and nothing to press") {
	// MEASURED FIRST, WRITTEN SECOND. Both `setVisible(false)` calls in `ExpandedOverlay::dismissBy`
	// — the one on the layer and the one on the panel — could be deleted INDEPENDENTLY with the
	// whole suite green, 37 of 37 twice. Nothing anywhere asserted that dismissing an expanded view
	// makes it go away.
	//
	// AND THE FIRST STORY TOLD ABOUT THAT WAS WRONG, WHICH IS WHY THIS COMMENT NAMES THE MECHANISM.
	// The obvious reading — "a layer left visible swallows every press in the editor" — is false
	// here: `ExpandedOverlay`'s constructor calls `setIgnoresMouseEvents(true, true)` ONCE and never
	// changes it, so the scrim is deaf for the whole life of the editor whether it is visible or
	// not. Hit participation was never coupled to visibility. A case written to that story asserted
	// its own premise, and the premise failed while the overlay was open, visible and full-window.
	//
	// SO WHAT THE TWO LINES ACTUALLY BUY, measured rather than read:
	//   * `Frame::frameAtPoint` skips a child that is not visible, and the panel is a child of the
	//     layer. So HIDING EITHER ONE ALONE HIDES BOTH — each line is redundant while the other
	//     stands, and that is why deleting one at a time left the suite honestly green.
	//   * DELETING BOTH IS THE DEFECT. The layer's `draw` early-returns when nothing is open, so the
	//     scrim does not come back — but the PANEL draws its own chrome and owns its own hit region
	//     (`wire()` sets `setIgnoresMouseEvents(false, true)`), so a closed editor keeps a panel
	//     rectangle that is painted and that answers presses, ✕ included.
	//
	// HENCE AN ASSERTION PAIR RATHER THAN ONE OF EACH: the two flags catch either line on its own,
	// and the hit test catches the pair. A case with only the flags would pass an editor whose
	// closed panel still took clicks; a case with only the hit test would let either line go.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	auto* overlay = f.gui.expandedForTest();
	REQUIRE(lane != nullptr);
	REQUIRE(overlay != nullptr);
	REQUIRE_FALSE(overlay->open());

	const size_t harm = f.bridge.addFxDevice(fx::harm::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(harm);
	lane->resized();
	REQUIRE(lane->onExpandDevice != nullptr);

	/// Is `frame`, or anything it sits inside, the overlay? A press lands on the DEEPEST frame that
	/// takes it, so the question is about the chain and not about one pointer.
	const auto insideOverlay = [&](visage::Frame* frame) {
		for (visage::Frame* at = frame; at != nullptr; at = at->parent()) {
			if (at == static_cast<visage::Frame*>(overlay)) {
				return true;
			}
		}
		return false;
	};

	// OPEN IT, AND TAKE THE PANEL'S CENTRE IN WINDOW COORDINATES WHILE IT IS OPEN. After a dismissal
	// `panelBounds()` returns an empty rectangle — it is computed from the content, and there is no
	// content — so the place to ask about has to be captured here. The panel's own bounds survive,
	// because `resized()` leaves them alone once nothing is open.
	(void)f.gui.canvasForTest()->setCollapsed(true); // on the overlay, the host this case reads
	lane->onExpandDevice(harm);
	REQUIRE(overlay->open());
	ui::ExpandedPanel* panel = overlay->panelForTest();
	REQUIRE(panel != nullptr);
	REQUIRE(panel->width() > 0.0f);
	const visage::Point centre =
	    panel->positionInWindow()
	    + visage::Point { panel->width() * 0.5f, panel->height() * 0.5f };

	// THE PREMISE, ASSERTED. Without these three the "no longer" checks below could all pass on an
	// overlay that was never visible and never occupied that point — the vacuous green this case
	// exists to be immune to.
	REQUIRE(overlay->isVisible());
	REQUIRE(panel->isVisible());
	REQUIRE(insideOverlay(f.root->frameAtPoint(centre)));

	/// What a dismissal must leave behind, whichever road took it. Asked of the REAL tree from the
	/// root, which is where a press enters — not of the layer, which would skip the very visibility
	/// check that makes the flags matter.
	const auto assertGone = [&] {
		CHECK_FALSE(overlay->open());
		CHECK_FALSE(overlay->isVisible());
		CHECK_FALSE(panel->isVisible());
		// THE CONSEQUENCE: the panel's own rectangle belongs to the editor again. This is the half
		// that no flag can stand in for — a rectangle that is painted and answers the ✕ on a device
		// nobody has open is an input defect, and it is invisible to every widget-level assertion
		// because every widget behind it is still perfectly constructed.
		CHECK_FALSE(insideOverlay(f.root->frameAtPoint(centre)));
	};

	SUBCASE("closed with the ✕") {
		// THROUGH THE PANEL'S OWN HANDLER, in the panel's coordinates — the layer has no mouse
		// handler at all, so this is the road the hand takes.
		const visage::Bounds close = panel->closeBounds();
		REQUIRE(close.width() > 0.0f);
		panel->onMouseDown().callback(
		    at(close.x() + close.width() * 0.5f, close.y() + close.height() * 0.5f));
		CHECK(overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::CloseButton);
		assertGone();
	}

	SUBCASE("closed with Escape") {
		// A SECOND ROAD BECAUSE IT IS DIFFERENT CODE, not for symmetry: Esc goes through the key
		// handler and the ✕ through the panel's press. `PopupMenuFrame` one door over has no Escape
		// handling at all, so "Esc closes it" is a thing to check rather than to assume.
		const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
		CHECK(overlay->onKeyPress().callback(escape));
		CHECK(overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::Escape);
		assertGone();
	}
}
TEST_CASE("the stage shows ONE surface, and the other is not there to be pressed") {
	// MEASURED FIRST: BOTH LINES OF `Gui::Impl::showSurface` CAN BE DELETED WITH THE WHOLE SUITE
	// GREEN — `perform->setVisible(!exploring)` and `explore->setVisible(exploring)`, 473 of 473
	// each. The editor's only surface swap was asserted by nothing.
	//
	// AND THE REASON IS THE "CHECK WHAT A TEST USES, NOT WHAT IT IS CALLED" ONE. There are two cases
	// about the switch and neither has an editor in it: *"the header's switch chooses a surface"
	// builds a bare `StageMode` and a `HeaderBar` over it, and *"a surface change swaps the stage
	// without waiting for a tick" builds a `StageMode` and NOTHING ELSE — its "stage" is a lambda
	// counting calls. Both are honest about what they test; between them they never once ask the
	// editor which surface is on screen.
	//
	// WHAT THE GATE IS HOLDING UP, from `showSurface`'s own comment: *"A hidden surface costs
	// nothing: it is not drawn, it is not ticked, and the terrain's worker sleeps because nothing
	// asks it for a window." All three of those are `isVisible()` — `tick()` calls
	// `explore->update()` only when it is visible — so the one bit is the whole of the claim.
	//
	// SO: THE FLAG AND THE PRESS, because they fail differently. Two surfaces visible at once is a
	// stage that draws both and ticks a terrain nobody is looking at; which of them ANSWERS a press
	// is then decided by child order, which is not a thing this editor states anywhere.
	//
	// AND THE PRESS HALF IS NOT DECORATION — MEASURED. With `explore->setVisible(exploring)` forced
	// open, a press at the centre of the PERFORM stage resolves INSIDE EXPLORE: `within(hit, shown)`
	// false and `within(hit, hidden)` true. In Perform mode the hand aiming at a star would be
	// dragging the terrain, on a stage where both surfaces are drawn and every widget on both is
	// perfectly constructed. Explore is the later child, so it wins the point.
	//
	// THE MEASUREMENT ALSO SHOWED HOW A FATAL PREMISE HIDES A CONSEQUENCE, which is the crash-masking
	// rule at assertion scale: with the premise below written as `REQUIRE_FALSE`, that same mutation
	// ends the case at the first line and reports ONE failure — the flag — and the three the press
	// half would have added never run. The `REQUIRE` is still right, because a case that continues
	// past a false premise is measuring an editor it did not set up; the note is here so the next
	// person reads the single failure as truncation rather than as the whole story.
	ModFixture f;
	auto* header = findInTree<ui::HeaderBar>(f.root);
	auto* perform = findInTree<ui::PerformStage>(f.root);
	auto* explore = findInTree<ui::ExploreStage>(f.root);
	REQUIRE(header != nullptr);
	REQUIRE(perform != nullptr);
	REQUIRE(explore != nullptr);

	/// The point on the switch that names `want`, ASKED OF THE HEADER rather than restated from its
	/// constants — `modeBounds()` is private and `hitsMode`/`surfaceAt` are public for exactly this.
	/// A case that hardcoded the rectangle would keep clicking the old place after the header moved.
	const auto pointNaming = [&](ui::StageMode::Surface want) {
		const float y = header->height() * 0.5f;
		for (float x = 0.0f; x < header->width(); x += 1.0f) {
			const visage::Point probe { x, y };
			if (header->hitsMode(probe) && header->surfaceAt(probe) == want) {
				return probe;
			}
		}
		return visage::Point { -1.0f, -1.0f };
	};

	const auto pressSwitch = [&](ui::StageMode::Surface want) {
		const visage::Point on = pointNaming(want);
		REQUIRE(on.x >= 0.0f);
		const visage::Point inWindow = header->positionInWindow() + on;
		// THROUGH THE ROOT, so the switch has to be reachable as well as correct.
		dragThroughRoot(f.root, inWindow, inWindow);
	};

	/// Is `frame`, or anything it sits inside, `surface`? A press lands on the deepest frame that
	/// takes it — a knob on the stage, not the stage — so the question is about the chain.
	const auto within = [](visage::Frame* frame, visage::Frame* surface) {
		for (visage::Frame* at = frame; at != nullptr; at = at->parent()) {
			if (at == surface) {
				return true;
			}
		}
		return false;
	};

	/// The whole contract, asked of whichever surface is meant to be showing.
	const auto onlyOneOnStage = [&](visage::Frame* shown, visage::Frame* hidden, const char* what) {
		INFO("showing " << std::string(what));
		INFO("showing " << what);
		CHECK(shown->isVisible());
		CHECK_FALSE(hidden->isVisible());

		// THE CONSEQUENCE, AND IT IS NOT THE FLAG SAID TWICE. The two surfaces share the stage's
		// rectangle, so with both visible the press goes to whichever is later in child order —
		// a hand aiming at the terrain would move a star, or the reverse, and every widget on both
		// surfaces would still be perfectly constructed.
		const visage::Point centre =
		    shown->positionInWindow()
		    + visage::Point { shown->width() * 0.5f, shown->height() * 0.5f };
		visage::Frame* hit = f.root->frameAtPoint(centre);
		REQUIRE(hit != nullptr);
		CHECK(within(hit, shown));
		CHECK_FALSE(within(hit, hidden));
	};

	// IT SHIPS SHOWING PERFORM, and that is the premise the two swaps below are measured against.
	REQUIRE_FALSE(explore->isVisible());
	onlyOneOnStage(perform, explore, "Perform, as it ships");

	pressSwitch(ui::StageMode::Surface::Explore);
	onlyOneOnStage(explore, perform, "Explore, after the switch");

	// AND BACK, because a swap that only works once is a swap that leaks a surface: `showSurface`
	// writes both bits every time and either one alone would satisfy a one-way case.
	pressSwitch(ui::StageMode::Surface::Perform);
	onlyOneOnStage(perform, explore, "Perform again");
}
TEST_CASE("a collapsed rail keeps nothing on screen but its own way back") {
	// MEASURED FIRST. `Rail::collapsedChanged()` — the loop that hides a collapsed rail's children —
	// can be forced to `setVisible(true)` with all 473 cases green, and so can
	// `ModPanel`'s `knobs_[macro]->setVisible(!collapsed())`. Two gates, nothing asserting either.
	//
	// AND IT IS THE OVERLAY'S SHAPE AGAIN: the loop is REDUNDANT for two rails and load-bearing for
	// one. `ModPanel` and `FxLane` re-decide their cards' visibility in their own `resized()`, so the
	// base loop is a second answer to a question they already answer. `EngineRail` has no
	// `setVisible` anywhere — its steppers, its two knobs and its route list are hidden by the base
	// loop and by nothing else.
	//
	// MEASURED, which is how the split above is known rather than read: forcing the base loop open
	// leaves ELEVEN visible children on the ENGINE RAIL and none on the other two; forcing `ModPanel`'s
	// macro line open leaves FOUR on the MODULATORS and none elsewhere. Two gates, two rails, no
	// overlap — so a case that asked only one rail would have caught only one of them, and a case
	// that asked only the modulators would have missed the rail whose gate has no second answer.
	//
	// WHY THE EXISTING COLLAPSE CASES DO NOT COVER IT, and this is the seam that missed an inert
	// panel once already: they ask `hitAt(...).part == Part::Collapse`, which is the panel's OWN
	// arithmetic over its own rectangle. That answer is unchanged by a child frame sitting on top of
	// the toggle. `frameAtPoint` is the seam visage itself uses, and it is the one that can see a
	// button that is drawn, correct, and covered.
	//
	// EVERY RAIL, FROM THE TREE. Three today; a fourth added tomorrow is asked the same question
	// without editing this case, which is the difference between a sweep and three copies.
	ModFixture f;
	std::vector<ui::Rail*> rails;
	std::function<void(visage::Frame*)> collect = [&](visage::Frame* frame) {
		for (visage::Frame* child : frame->children()) {
			if (auto* rail = dynamic_cast<ui::Rail*>(child)) {
				rails.push_back(rail);
			}
			collect(child);
		}
	};
	collect(f.root);
	// THE COUNT IS ASSERTED, because a walk that finds nothing passes every question asked of it.
	REQUIRE(rails.size() == 4); // the stage's own since backlog NF

	for (ui::Rail* rail : rails) {
		// NAMED FOR THE READER, so a failure says WHICH rail rather than a mangled type. The list
		// above is still built generically — the naming is a label, not the sweep's membership test.
		const char* which = dynamic_cast<ui::EngineRail*>(rail) != nullptr ? "Engine rail"
		                    : dynamic_cast<ui::ModPanel*>(rail) != nullptr     ? "modulators"
		                    : dynamic_cast<ui::FxLane*>(rail) != nullptr       ? "fx lane"
		                    : dynamic_cast<ui::StageRail*>(rail) != nullptr    ? "stage"
		                                                                       : "a rail this case has no name for";
		INFO("rail: " << std::string(which));
		REQUIRE_FALSE(rail->collapsed());

		/// The toggle's centre in window coordinates, re-read every time: closing one rail moves the
		/// others, so a point cached across a collapse is a point aimed at where the button was.
		const auto toggleCentre = [&] {
			const visage::Bounds toggle = rail->toggleBounds();
			return rail->positionInWindow()
			       + visage::Point { toggle.x() + toggle.width() * 0.5f,
				                     toggle.y() + toggle.height() * 0.5f };
		};

		// CLOSE IT THROUGH THE REAL TREE. `dragThroughRoot` resolves the press with `frameAtPoint`
		// and delivers it to whatever that returns, so a covered toggle is a toggle this cannot
		// press — which is the whole point of driving it this way rather than calling setCollapsed.
		CHECK(f.root->frameAtPoint(toggleCentre()) == static_cast<visage::Frame*>(rail));
		dragThroughRoot(f.root, toggleCentre(), toggleCentre());
		REQUIRE(rail->collapsed());

		// NOTHING OF THE PANEL IS LEFT ON SCREEN. Not "the rail is narrow" — the rail is still
		// visible and still drawn, and that is the point of a collapsed rail; what must be gone is
		// its CONTENTS, which are laid out for a width they no longer have.
		CHECK(rail->isVisible());
		size_t visibleChildren = 0;
		for (const visage::Frame* child : rail->children()) {
			if (child->isVisible()) {
				++visibleChildren;
			}
		}
		CHECK(visibleChildren == 0);

		// AND THE WAY BACK IS STILL PRESSABLE. STATED AS A STANDING CLAIM, NOT AS THE MUTATION'S
		// CATCHER — measured: with either gate forced open the leftover children do NOT cover the
		// toggle, so both mutations are caught by the count above and this line stays green through
		// them. It is here because "the rail can be reopened" is the promise a collapsed rail makes,
		// and the panel's own `hitAt` cannot check it: a covered button answers `hitAt` correctly and
		// never receives the press.
		CHECK(f.root->frameAtPoint(toggleCentre()) == static_cast<visage::Frame*>(rail));
		dragThroughRoot(f.root, toggleCentre(), toggleCentre());
		CHECK_FALSE(rail->collapsed());
	}
}
TEST_CASE("every implementation of setAmount announces it, and the tree names them") {
	// FOUR IMPLEMENTATIONS OF `Modulatable::setAmount`, AND THE SUITE ASSERTED ONE. Measured by
	// deleting each `mod_->touched()` in turn and running the whole binary:
	//
	//   FxControl::setAmount      deleted -> RED   (the case one door up, written when it was missing)
	//   ParamControl::setAmount   deleted -> RED
	//   RackControl::setAmount    deleted -> GREEN, 475 of 475
	//   MsegControl::setAmount    deleted -> GREEN, 475 of 475
	//
	// SO TWO OF THE FOUR COULD SIMPLY STOP TELLING ANYBODY. The convention is stated in a comment at
	// `knob.cpp:388` — *"setAmount notifies the panel itself — see ParamControl::setAmount and
	// RackControl::setAmount" — which names two of the four, and the last time somebody checked that
	// comment against the code they found `FxControl` had never followed it.
	//
	// FOUND BY TYPE FROM THE TREE, NOT BY A LIST WRITTEN HERE. A fifth implementation added tomorrow
	// is asked the same question without editing this case; a hardcoded list of four is a list that
	// goes stale the way the comment did. Both widget kinds are walked, because the four controls are
	// split across them — `MsegControl` and `RackControl` live under a `ValueBox`, the other two
	// mostly under a `Knob` — and a sweep through one accessor finds half the population.
	//
	// ASSERTED AS THE VALUE AND THE ANNOUNCEMENT. `touched()` returns what `FrameWatchers::notify()`
	// returned, so the tally cannot rise for a call that reached an empty list; and the amount is
	// read back so a road that announced without writing cannot pass either.
	ModFixture f;
	namespace ms = feathers::mod::mseg;
	namespace hrm = feathers::fx::harm;
	REQUIRE(f.bridge.addRackDevice(ms::kTraits.tag) < kMaxRackSlots);
	REQUIRE(f.bridge.addFxDevice(hrm::kTraits.tag) < kMaxFxSlots);
	f.bridge.refresh();
	f.root->resized();
	// EACH STRIP ASKED TO PLACE ITS OWN CARDS. A card is BUILT by its rail's `resized()`, so a device
	// added but never laid out has no widgets in the tree and the sweep below would look right and
	// find nothing of its kind.
	f.panel->resized();
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();

	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	f.arm(backplane::Source::core(0));
	REQUIRE(mod->armed());

	// EVERY MODULATABLE IN THE EDITOR, THROUGH BOTH WIDGET KINDS.
	std::vector<device::Modulatable*> controls;
	std::function<void(visage::Frame*)> collect = [&](visage::Frame* frame) {
		for (visage::Frame* child : frame->children()) {
			if (auto* knob = dynamic_cast<ui::Knob*>(child); knob != nullptr && knob->control() != nullptr) {
				controls.push_back(knob->control());
			}
			else if (auto* box = dynamic_cast<ui::ValueBox*>(child)) {
				controls.push_back(&box->control());
			}
			collect(child);
		}
	};
	collect(f.root);
	REQUIRE(controls.size() > 4);

	// GROUPED BY THE CONCRETE TYPE, which is the population the claim is about: "every implementation
	// of setAmount", not "every control".
	std::vector<std::string> kinds;
	std::vector<std::vector<device::Modulatable*>> byKind;
	for (device::Modulatable* control : controls) {
		const std::string kind = typeid(*control).name();
		size_t at = kinds.size();
		for (size_t i = 0; i < kinds.size(); ++i) {
			if (kinds[i] == kind) {
				at = i;
			}
		}
		if (at == kinds.size()) {
			kinds.push_back(kind);
			byKind.emplace_back();
		}
		byKind[at].push_back(control);
	}
	// ALL FIVE ARE STANDING UP. Without this the loop below would pass by asking three, or one, or
	// none — the way a tree walk usually fails. NAMED IN THE LOG, so a failure here says which kind
	// the fixture did not stand up rather than only that the count is short.
	std::string found;
	for (const std::string& kind : kinds) {
		found += kind + " ";
	}
	INFO("kinds found: " << found);
	// FOUR, DOWN FROM SIX, AND THE FALL IS THE CONVERGENCE ARRIVING.
	//
	// It was five, then six while `HarmCard` alone had moved to `DeviceControl`. It is now four:
	// `RackControl` and `MsegControl` have LEFT THE TREE ENTIRELY, because `EnvCard` and `MsegCard`
	// were ported to `backplane::DeviceHost` and build `DeviceControl` like everything else. Two
	// whole control kinds deleted rather than added — which is what the note below predicted would
	// happen, one tier earlier than it expected.
	//
	// **AND IT FELL TO THREE, WHICH THIS LINE PREDICTED.** It read `== 4` and said *"IT SHOULD FALL
	// AGAIN TO THREE when those five fx cards move and `FxControl` leaves too." `FilterCard` was the
	// last of them; it reads its row through `DeviceHost` now, and `FxControl` has no constructor call
	// left anywhere in `src/` — only comments referring back to it.
	//
	// WHAT REMAINS AND WHY: `ParamControl` is the plugin's own parameter road; `DeviceControl` is the
	// converged one; `GammaControl` is Explore's. A reader finding this number RISEN should ask what
	// new control kind was introduced and whether it should have been `DeviceControl`.
	//
	// The count is not decoration: it is what stops the loop below passing by asking one kind, or none.
	//
	// **FOUR AGAIN, 2026-09-15, AND IT IS NOT A NEW ROAD.** `BipolarMacro` (backlog EG) is a VIEW over
	// the macro row's `ParamControl` — it re-ranges a macro for the eye and forwards every write,
	// `setAmount` included, to the control underneath. It stands only on the four macro knobs, which
	// are never destinations, so the property rule below excuses it as it excuses Explore's gamma;
	// the count rises because the tree walk sees the view rather than the control it wraps.
	//
	// **FIVE, 2026-09-21, AND THIS ONE *IS* A NEW ROAD — so the question above gets an answer.**
	// `TravelControl` is Explore's Radius and Theta: a sound's place in its own plane, which it was asked
	// to have as modulation destinations *without* spending CLAP parameters (hence the internal param
	// type). SHOULD IT HAVE BEEN `DeviceControl`? No, and for a reason rather than a preference:
	// `DeviceControl` addresses a control through `backplane::DeviceHost` BY DEVICE INSTANCE ID, and a
	// sound's travel belongs to no device — it is the synth core's own patch state, one pair per SLOT,
	// and its destination is `Destination::core(...)` in the core table's internal region. What it
	// shares with `DeviceControl` is the descriptor type (`device::ParamDesc`) and with `ParamControl`
	// the routing half, line for line. It is `GammaControl`'s shape with `display()` filled in.
	//
	// UNLIKE GAMMA IT IS NOT EXCUSED BELOW: it declares itself modulatable, so the loop finds an armed
	// instance and requires its `setAmount` to write AND announce, like the other real roads.
	REQUIRE(kinds.size() == 5);

	for (size_t kind = 0; kind < kinds.size(); ++kind) {
		INFO("control kind " << kinds[kind]);
		// **A KIND THAT CANNOT BE A DESTINATION AT ALL IS EXCUSED, AND ONLY THAT KIND.**
		// `GammaBaseControl` is the unbounded half of Explore's gamma: it has no parameter id by
		// design (`sound_gamma.h`), so `setAmount` on it is correctly a no-op that announces nothing,
		// and demanding an armed instance of it would be demanding the split not exist.
		//
		// STATED AS A PROPERTY, NOT AS A NAME. The excuse is "no instance of this kind declares
		// itself modulatable", which a kind that gained a route would stop satisfying — a name in an
		// exceptions list would go on excusing it.
		bool anyModulatable = false;
		for (device::Modulatable* control : byKind[kind]) {
			anyModulatable = anyModulatable || control->desc().modulatable;
		}
		if (!anyModulatable) {
			continue;
		}
		// ONE THAT THE ARMED SOURCE MAY ACTUALLY REACH. A control that is not a destination returns
		// from setAmount without writing and without telling, correctly — so asking it would be
		// asking the wrong question, and finding none of its kind reachable is itself a failure.
		device::Modulatable* usable = nullptr;
		for (device::Modulatable* control : byKind[kind]) {
			if (control->display().isDestination && control->display().armed) {
				usable = control;
				break;
			}
		}
		REQUIRE(usable != nullptr);

		const size_t told = mod->framesToldForTest();
		usable->setAmount(0.4f);
		// THE WRITE LANDED — read back off the control, not recomputed here from a slot and an
		// ordinal, so this cannot agree with the widget about a destination both are wrong about.
		CHECK(usable->display().amount == doctest::Approx(0.4f));
		// AND IT WAS ANNOUNCED. The two together: a road that wrote silently fails the second, a road
		// that announced without writing fails the first.
		CHECK(mod->framesToldForTest() > told);
	}
}
TEST_CASE("the route list's own edits announce themselves, and so does a double-click unroute") {
	// THREE MORE SILENT ROADS, MEASURED THE SAME WAY AS THE FOUR `setAmount`S: delete the
	// `touched()` and run the whole binary.
	//
	//   RouteList::commitAmount   (mod_panel.cpp:274)  deleted -> GREEN, 475 of 475
	//   RouteList::remove         (mod_panel.cpp:296)  deleted -> GREEN, 475 of 475
	//   ParamControl::unroute     (param_control.cpp:204) deleted -> GREEN, 475 of 475
	//
	// AND THEY ARE THE ROADS WHERE SILENCE COSTS MOST, because the surfaces that draw an amount are
	// not the surface the hand is on: the ring under the pointer, the mod panel's list, and the rack
	// cards all show the same number, and only `ModState`'s list reaches the last two. The state
	// channel does not cover for it — `setRouteAmount` raises `repaintDirty_` and `tick()` turns that
	// into `stateWatchers.notify()`, which holds the STAGES and the fx cards, not these.
	//
	// DRIVEN AS GESTURES, not by calling the functions: `commitAmount` and `remove` are private and
	// the only way in is a press on a row, which is also the only way to prove the row is where the
	// case thinks it is.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	f.arm(backplane::Source::core(0));
	REQUIRE(mod->armed());

	// A ROUTE TO EDIT, made the way a hand makes one.
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	const int destination = modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const backplane::Destination dest = backplane::Destination::core(static_cast<size_t>(destination));
	const visage::Point face = f.faceCentre(path);
	dragThroughRoot(f.root, face, { face.x, face.y - 35.0f });
	const float made = f.bridge.routeAmount(backplane::Source::core(0), dest);
	REQUIRE(made > 0.0f);

	auto* list = findInTree<ui::RouteList>(f.root);
	REQUIRE(list != nullptr);
	f.root->resized();
	// THE LIST HAS THE ROW, and it is the row this case is about. A list that showed nothing would
	// make every press below land on empty space and pass by doing nothing at all.
	const auto rows = list->rows();
	REQUIRE(std::find(rows.begin(), rows.end(), dest) != rows.end());
	size_t index = rows.size();
	for (size_t i = 0; i < rows.size(); ++i) {
		if (rows[i] == dest) {
			index = i;
		}
	}
	REQUIRE(index < rows.size());

	/// A point inside row `index`, in window coordinates, on the part `want` — asked of the list
	/// rather than computed from kRowHeight here, so the case cannot aim at a row the list has moved.
	const auto pointOn = [&](ui::RouteList::Part want) {
		const visage::Bounds row = list->rowBounds(index);
		REQUIRE(row.width() > 0.0f);
		const float y = row.y() + row.height() * 0.5f;
		for (float x = row.x() + 1.0f; x < row.right(); x += 1.0f) {
			const ui::RouteList::Hit hit = list->hitAt({ x, y });
			if (hit.part == want && hit.destination == dest) {
				return list->positionInWindow() + visage::Point { x, y };
			}
		}
		return visage::Point { -1.0f, -1.0f };
	};

	SUBCASE("an amount dragged in the list") {
		const visage::Point on = pointOn(ui::RouteList::Part::Amount);
		REQUIRE(on.x >= 0.0f);
		// THE PRESS REACHES THE LIST. Without this the drag below could be landing on the Engine rail
		// behind it and the tally could be rising for something else entirely.
		REQUIRE(f.root->frameAtPoint(on) == static_cast<visage::Frame*>(list));

		const size_t told = mod->framesToldForTest();
		dragThroughRoot(f.root, on, { on.x, on.y - 20.0f });
		// THE VALUE MOVED — the drag is a real edit, not a press that missed.
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) != doctest::Approx(made));
		// AND IT WAS ANNOUNCED.
		CHECK(mod->framesToldForTest() > told);
	}

	SUBCASE("a route removed from the list") {
		const visage::Point on = pointOn(ui::RouteList::Part::Remove);
		REQUIRE(on.x >= 0.0f);
		REQUIRE(f.root->frameAtPoint(on) == static_cast<visage::Frame*>(list));

		const size_t told = mod->framesToldForTest();
		dragThroughRoot(f.root, on, on);
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		CHECK(mod->framesToldForTest() > told);
	}

	SUBCASE("a double-click on the knob itself") {
		// `Knob::unroute` -> `ParamControl::unroute`, which is a different function from
		// `setAmount` and had its own silence. The gesture is the mock's: double-click an armed,
		// routed control to take the connection back.
		const size_t told = mod->framesToldForTest();
		const visage::Point local { path->width() * 0.5f, path->faceDiameter() * 0.5f };
		path->processMouseDown(doubleClickAt(local.x, local.y));
		CHECK(f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		CHECK(mod->framesToldForTest() > told);
	}
}
TEST_CASE("the drawn plane names its bands, and a frame menu edits a frame that is not on screen") {
	// ASKED FOR BOTH OF THESE AND NEITHER EXISTED. 2026-08-28: "I would like to get floating
	// numbers or abcd near the filter curve editor points so I can try to line them up between
	// frames", and "can we get a right click dropdown on the expanded editor frame tabs with options
	// like flatten/reset and copy to (other frame)."
	//
	// WHAT IS ASSERTED IS THE VALUE, not the picture. A label is drawn ink and this suite cannot read
	// ink, so what is pinned is the thing the label is derived from — the band's own index — and the
	// menu's edits are pinned as the settings they write. "It looks right" is not a claim a case can
	// hold; "band b's letter is 'b' in every frame" and "frame 3 now equals frame 1" are.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	// THE STATE ABOVE THE WIDGETS THAT WATCH IT (frame_watchers.h): the panel's knobs are on this list,
	// so a state scoped inside a subcase would die under them.
	ui::ModState mod;
	ui::EditHistory history;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();

	SUBCASE("the letter for a band is the same in every frame, because that is what a morph pairs") {
		// THE IDENTITY THE LABEL CARRIES IS THE BAND INDEX, and the reason is the design itself: a hand lines
		// handles up BETWEEN frames, and the puck blends band b of one frame with band b of another.
		// A label that changed per frame would answer a question nobody asked.
		for (size_t frame = 0; frame < dr::kFrames; ++frame) {
			panel->selectFrame(frame);
			for (size_t b = 0; b < dr::kBands; ++b) {
				CAPTURE(frame);
				CAPTURE(b);
				CHECK(ui::DrawnExpanded::bandLabelText(b) == std::string(1, static_cast<char>('a' + b)));
			}
		}
		// AND IT IS LETTERS, NOT NUMERALS, BY DESIGN — the frame tabs are numbered, and six
		// band numerals beside four frame numerals is two number systems on one surface. Pinned so a
		// later tidy cannot quietly unify them.
		CHECK(ui::DrawnExpanded::bandLabelText(0) == "a");
		CHECK(ui::DrawnExpanded::bandLabelText(dr::kBands - 1) == "f");
		CHECK(ui::DrawnExpanded::frameTabText(0).find('1') != std::string::npos);
	}

	SUBCASE("flatten keeps the handles where they are; reset puts them back") {
		panel->selectFrame(0);
		// MOVE A POLE so the two operations have something to disagree about. On an untouched frame
		// they agree — the shipped default IS flat — and a case run only there would pass against a
		// flatten implemented as a reset.
		ui::FxControl radius { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
			                   dr::bandFieldIndex(0, 2, dr::PoleRadius) };
		radius.setValue(0.80);
		f.bridge.refresh();
		const float moved = panel->frameBand(0, 2).pole.radius;
		REQUIRE(moved == doctest::Approx(0.80f).epsilon(1e-3));

		panel->flattenFrame(0);
		for (size_t b = 0; b < dr::kBands; ++b) {
			CAPTURE(b);
			const dr::Band band = panel->frameBand(0, b);
			// FLAT: the zero sits on the pole, which is the Bypass placement.
			CHECK(band.zero.radius == doctest::Approx(band.pole.radius).epsilon(1e-4));
			CHECK(band.zero.angleLog2 == doctest::Approx(band.pole.angleLog2).epsilon(1e-4));
		}
		// AND THE HANDLE DID NOT MOVE — the assertion that separates flatten from reset, and the one
		// a flatten-implemented-as-reset fails while passing everything above it.
		CHECK(panel->frameBand(0, 2).pole.radius == doctest::Approx(moved).epsilon(1e-4));

		panel->resetFrame(0);
		CHECK(panel->frameBand(0, 2).pole.radius
		      == doctest::Approx(dr::kDefaultFrames[0].band[2].pole.radius).epsilon(1e-4));
	}

	SUBCASE("copy writes the other frame, and an identical copy records nothing") {
		panel->selectFrame(0);
		ui::FxControl radius { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor,
			                   dr::bandFieldIndex(0, 1, dr::PoleRadius) };
		radius.setValue(0.70);
		f.bridge.refresh();
		REQUIRE(panel->frameBand(0, 1).pole.radius == doctest::Approx(0.70f).epsilon(1e-3));
		REQUIRE(panel->frameBand(2, 1).pole.radius != doctest::Approx(0.70f).epsilon(1e-3));

		// THE FRAME EDITED IS NOT THE FRAME ON SCREEN. The menu offers "copy to Frame 3" while you are
		// looking at Frame 1, so every road it uses takes the frame explicitly instead of reading the
		// selection — which is the bug this subcase would catch if one of them read `frame_`.
		panel->copyFrame(0, 2);
		CHECK(panel->frameBand(2, 1).pole.radius == doctest::Approx(0.70f).epsilon(1e-3));
		CHECK(panel->frameAt(2).band[1].zero.radius
		      == doctest::Approx(panel->frameAt(0).band[1].zero.radius).epsilon(1e-4));

		// AND A COPY THAT CHANGES NOTHING MUST RECORD NOTHING — the StanceEdit shape this repo has
		// open in the backlog.
		//
		// WHAT THIS ACTUALLY PINS IS `FxControl::endGesture`'s rule, not `writeFrame`'s skip, and the
		// difference was found by a known-bad rather than by reading: disabling the skip left this
		// green, because endGesture already refuses to record a gesture that changed nothing. So this
		// assertion guards the property from the outside — which is the right place for it — and it
		// would survive `writeFrame` being rewritten in any way that keeps the property true.
		//
		// ASKED THROUGH `canUndo()` RATHER THAN A DEPTH, because that is what EditHistory exposes and
		// inventing an accessor for one assertion is how a test-only surface grows. A fresh history
		// cannot undo; if the second copy wrote anything it can.
		panel->setModulation(&mod, &history);
		REQUIRE_FALSE(history.canUndo());
		panel->copyFrame(0, 2);
		CHECK_FALSE(history.canUndo());
		// AND THE GUARD IS NOT VACUOUS: a copy that DOES change something records one.
		panel->copyFrame(1, 2);
		CHECK(history.canUndo());
	}
}
TEST_CASE("a frame's TYPES travel with its handles, so the next drag does not reset them") {
	// **BACKLOG CM1, AND THE GESTURE IN THE REPORT IS NOT THE CULPRIT.** The report was that Drawn's
	// double tap (click and drag) resets the waypoint type. Nothing in this device's mouse handling writes a
	// type — `applyType` is the only writer of `TypeIndex` in the whole UI — and every gesture was
	// driven against a Notch and came back a Notch: one press-drag-release, a press with a click count
	// of two, click-then-press-then-drag, a press that hit nothing, and the card's own
	// double-tap-and-drag and double-tap-to-fold.
	//
	// WHAT THE DRAG DOES IS REVEAL A FRAME THE MENU LEFT IN TWO MINDS. `writeFrame` moved four fields
	// per band and left the fifth, so a copied frame was DRAWN as its source and STORED as itself; the
	// first `dragPole` then re-asserted the stored type through `zeroPlacementFor` — "types stick",
	// working exactly as designed — and the drawing snapped to it. `flattenFrame` had the same
	// omission spelled out by hand.
	//
	// **SO WHAT IS ASSERTED IS THE STORED TYPE, WHICH IS THE THING THE REPORT IS ABOUT.** A geometry
	// comparison cannot make this claim: after the copy the destination's POINTS were already right —
	// that is precisely why the defect was invisible until a hand moved something — so a case that
	// asked whether the zero had arrived would have passed against the bug. CLAUDE.md's rule, met in
	// its purest form: the partial effect survived and only the value assertion kills it.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();

	const size_t band = 3;
	const auto valueAt = [&](size_t index) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		return c.value();
	};
	const auto write = [&](size_t index, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		c.setValue(v);
	};
	const auto typeAt = [&](size_t frame) {
		return valueAt(dr::bandFieldIndex(frame, band, dr::TypeIndex));
	};
	const auto asIndex = [](dr::Type t) { return static_cast<double>(static_cast<int>(t)); };

	/// Press this band's pole in the frame ON SCREEN, drag it, release — the road a hand takes — and
	/// hand back the pole it ended on, read from the store.
	const auto dragPole = [&](size_t frame) {
		panel->selectFrame(frame);
		const visage::Bounds box = panel->planeBounds();
		const visage::Point from = panel->poleCentre(band);
		const visage::Point to { box.x() + box.width() * 0.5f, box.bottom() - box.height() * 0.6f };
		REQUIRE(panel->hitAt(from).what == ui::DrawnExpanded::Grab::Pole);
		panel->onMouseDown().callback(at(from.x, from.y));
		panel->onMouseDrag().callback(at(to.x, to.y));
		panel->onMouseUp().callback(at(to.x, to.y));
		f.bridge.refresh();
		return dr::Point {
			static_cast<float>(valueAt(dr::bandFieldIndex(frame, band, dr::PoleRadius))),
			static_cast<float>(valueAt(dr::bandFieldIndex(frame, band, dr::PoleAngle))),
		};
	};

	// FRAME 0'S BAND MADE A NOTCH, at a pole of this case's own choosing. Not borrowed from the
	// default frame: the sibling case above records what leaning on a default costs when the default
	// changes.
	panel->selectFrame(0);
	write(dr::bandFieldIndex(0, band, dr::PoleRadius), 0.8);
	write(dr::bandFieldIndex(0, band, dr::PoleAngle), -4.0);
	f.bridge.refresh();
	panel->applyType(band, dr::Type::Notch);
	f.bridge.refresh();
	REQUIRE(typeAt(0) == doctest::Approx(asIndex(dr::Type::Notch)));

	SUBCASE("a copied frame arrives as its types, and one click-and-drag leaves them alone") {
		// THE DESTINATION IS SOMETHING ELSE FIRST, or "it is a Notch afterwards" is a claim about a
		// field nothing had to move.
		REQUIRE(typeAt(1) != doctest::Approx(asIndex(dr::Type::Notch)));
		panel->copyFrame(0, 1);
		f.bridge.refresh();

		// THE VALUE. This is the assertion the defect fails, and it fails it while every geometric
		// field below is already correct.
		CHECK(typeAt(1) == doctest::Approx(asIndex(dr::Type::Notch)));
		// AND THE GEOMETRY CAME TOO — the second assertion, proving the first is not about a copy that
		// moved the type and nothing else.
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius))
		      == doctest::Approx(valueAt(dr::bandFieldIndex(0, band, dr::ZeroRadius))));
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::PoleAngle))
		      == doctest::Approx(valueAt(dr::bandFieldIndex(0, band, dr::PoleAngle))));

		// NOW THE GESTURE FROM THE REPORT, on the copied frame.
		const dr::Point ended = dragPole(1);
		// THE POLE REALLY MOVED, or the two assertions below are about a drag that did nothing.
		REQUIRE(ended.angleLog2 != doctest::Approx(-4.0));
		CHECK(typeAt(1) == doctest::Approx(asIndex(dr::Type::Notch)));
		// AND THE ZERO IS WHERE A NOTCH PUTS IT AT THE POLE THE DRAG ENDED ON — computed by the
		// device's own `zeroPlacementFor` rather than by a number typed here, which is the independent
		// restatement of the intended path. Against the defect this reads as the Bypass placement
		// instead: the zero sits exactly on the pole and the Notch is gone.
		const auto want = dr::zeroPlacementFor(dr::Type::Notch, ended);
		REQUIRE(want.has_value());
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroRadius))
		      == doctest::Approx(static_cast<double>(want->radius)).epsilon(1e-5));
		CHECK(valueAt(dr::bandFieldIndex(1, band, dr::ZeroAngle))
		      == doctest::Approx(static_cast<double>(want->angleLog2)).epsilon(1e-5));
	}

	SUBCASE("reset puts the shipped TYPE back, not only the shipped points") {
		panel->resetFrame(0);
		f.bridge.refresh();
		// EVERY BAND, against `kDefaultFrames` — the same table the descriptor's declared defaults are
		// taken from, so this cannot drift from what "default" means everywhere else.
		for (size_t b = 0; b < dr::kBands; ++b) {
			CAPTURE(b);
			CHECK(valueAt(dr::bandFieldIndex(0, b, dr::TypeIndex))
			      == doctest::Approx(static_cast<double>(
			             static_cast<int>(dr::kDefaultFrames[0].band[b].type))));
		}
		// AND THE POINTS CAME BACK TOO, which is what the case above this one already pins and what
		// makes the type assertion a claim about the MISSING field rather than about reset at large.
		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::PoleRadius))
		      == doctest::Approx(static_cast<double>(dr::kDefaultFrames[0].band[band].pole.radius))
		             .epsilon(1e-4));
	}

	SUBCASE("a flattened band is still flat after the next drag") {
		// **FLATTEN'S PROMISE IS ABOUT THE SOUND** — *"the section contributes nothing" — and a band
		// that stores `Notch` goes back to contributing the moment a hand moves it. So the assertion is
		// not that the zero is on the pole today; it is that it is STILL on the pole after a drag.
		panel->flattenFrame(0);
		f.bridge.refresh();
		CHECK(typeAt(0) == doctest::Approx(asIndex(dr::Type::Bypass)));

		const dr::Point ended = dragPole(0);
		REQUIRE(ended.angleLog2 != doctest::Approx(-4.0));
		CHECK(typeAt(0) == doctest::Approx(asIndex(dr::Type::Bypass)));
		// THE ZERO ON THE POLE, both coordinates, which IS the Bypass placement. Read against the pole
		// the drag ended on rather than against a literal, so the claim is "these two points are the
		// same point" and not "they are both at some number this case guessed".
		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::ZeroRadius))
		      == doctest::Approx(static_cast<double>(ended.radius)).epsilon(1e-5));
		CHECK(valueAt(dr::bandFieldIndex(0, band, dr::ZeroAngle))
		      == doctest::Approx(static_cast<double>(ended.angleLog2)).epsilon(1e-5));
	}
}
TEST_CASE("copy to all others writes every other frame, whole, as one undo step") {
	// **BACKLOG CM2** — Drawn's "Copy to all others". The WHOLE frame, which is
	// what the singular "Copy to Frame N" beside it copies: all six bands' pole, zero and type.
	//
	// WHAT IS ASSERTED IS FRAME EQUALITY, `dr::Frame`'s own — so the type is in the claim by
	// construction rather than by a reader remembering to check it. That matters here more than
	// usual: CM1 was exactly a copy that moved every point and no type, and it passed every
	// geometric assertion anybody had written.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	// THE STATE ABOVE THE WIDGETS THAT WATCH IT (frame_watchers.h): the panel's knobs are on this list.
	ui::ModState mod;
	ui::EditHistory history;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();

	panel->setModulation(&mod, &history);

	const auto write = [&](size_t index, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		c.setValue(v);
	};

	// **THE SOURCE FRAME MADE UNLIKE ANY OTHER, IN ALL THREE KINDS OF FIELD.** Poles moved, and every
	// band given a DIFFERENT type — so a copy that carried geometry and dropped types, or carried one
	// band's type to all six, fails rather than passing by coincidence.
	const size_t source = 1;
	panel->selectFrame(source);
	for (size_t b = 0; b < dr::kBands; ++b) {
		write(dr::bandFieldIndex(source, b, dr::PoleRadius), 0.5 + 0.06 * static_cast<double>(b));
		write(dr::bandFieldIndex(source, b, dr::PoleAngle), -10.0 + 1.5 * static_cast<double>(b));
	}
	f.bridge.refresh();
	// SIX DISTINCT TYPES, taken from the device's own enumeration rather than named here — Free is
	// skipped because it places no zero and this fixture wants each band's zero moved too.
	const std::array<dr::Type, dr::kBands> types { dr::Type::Lowpass, dr::Type::Highpass,
		                                           dr::Type::Bandpass, dr::Type::Notch,
		                                           dr::Type::Bell,    dr::Type::Allpass };
	for (size_t b = 0; b < dr::kBands; ++b) {
		panel->applyType(b, types[b]);
	}
	f.bridge.refresh();
	const dr::Frame wanted = panel->frameAt(source);
	// THE FIXTURE IS DOING WHAT IT CLAIMS: the source differs from every other frame, so each
	// destination has work to do.
	for (size_t to = 0; to < dr::kFrames; ++to) {
		if (to != source) {
			CAPTURE(to);
			REQUIRE_FALSE(panel->frameAt(to) == wanted);
		}
	}

	SUBCASE("every other frame becomes the source, and the source is untouched") {
		panel->copyFrameToAll(source);
		f.bridge.refresh();
		for (size_t to = 0; to < dr::kFrames; ++to) {
			CAPTURE(to);
			// INCLUDING THE SOURCE, which must still equal what it was — a loop that failed to skip
			// `from` would copy a frame onto itself, which is harmless, but one that wrote the source
			// from a partly-copied neighbour would not be.
			CHECK(panel->frameAt(to) == wanted);
		}
	}

	SUBCASE("it is ONE undo step, and one press puts all three frames back") {
		std::array<dr::Frame, dr::kFrames> before {};
		for (size_t i = 0; i < dr::kFrames; ++i) {
			before[i] = panel->frameAt(i);
		}
		const size_t steps = history.length();

		panel->copyFrameToAll(source);
		f.bridge.refresh();
		// THE COUNT. Three `writeFrame` calls would read three; the pre-fix per-field road would read
		// up to ninety.
		CHECK(history.length() == steps + 1);

		history.undo();
		f.bridge.refresh();
		// ALL FOUR FRAMES, from one press. This is the assertion that a bulk edit has to earn before
		// it is worth having at all.
		for (size_t i = 0; i < dr::kFrames; ++i) {
			CAPTURE(i);
			CHECK(panel->frameAt(i) == before[i]);
		}

		history.redo();
		f.bridge.refresh();
		for (size_t i = 0; i < dr::kFrames; ++i) {
			CAPTURE(i);
			CHECK(panel->frameAt(i) == wanted);
		}
	}

	SUBCASE("copying onto frames that already match records nothing") {
		panel->copyFrameToAll(source);
		f.bridge.refresh();
		const size_t steps = history.length();
		panel->copyFrameToAll(source);
		f.bridge.refresh();
		CHECK(history.length() == steps);
	}

	SUBCASE("the menu offers it, and offers it once") {
		// THE ENTRY ITSELF. A verb with no way to reach it is the honesty failure this repo has
		// shipped twice on this very panel's neighbours — a declared expanded view with no registry
		// entry, and an expand button that drew nothing.
		//
		// **ASKED OF THE LIST THE POPUP IS BUILT FROM**, which is as close to the menu as a headless
		// case gets: `showFrameMenu` adds exactly these options and a shown `visage::PopupMenu` is not
		// something this suite can press. So what is pinned is that the option list carries the entry
		// exactly once, alongside the three per-frame copies it is the plural of.
		size_t all = 0;
		size_t singles = 0;
		int allId = 0;
		for (const auto& option : ui::DrawnExpanded::frameMenuOptions(source)) {
			if (option.second == "Copy to all others") {
				++all;
				allId = option.first;
			}
			else if (option.second.rfind("Copy to ", 0) == 0) {
				++singles;
			}
		}
		CHECK(all == 1);
		// AND IT DID NOT REPLACE THEM: three others, one entry each.
		CHECK(singles == dr::kFrames - 1);
		// AND ITS ID IS NOT A FRAME INDEX, which is what keeps the selection handler's two branches
		// apart — a non-negative id in this menu IS the frame to copy to.
		CHECK(allId == ui::DrawnExpanded::kFrameMenuCopyAll);
		CHECK(allId < 0);
	}
}
TEST_CASE("one frame-menu click is ONE undo step, and undoing it puts the whole frame back") {
	// **A MENU CLICK IS ONE ACTION AND WAS NOT ONE STEP.** The rule: a menu click is one action and
	// must be one undo step. Each frame verb bracketed every changed field as its own
	// `DeviceControl` gesture and `endGesture` records a `DeviceValueEdit` apiece, so "Copy to Frame 2"
	// cost up to twenty-four presses of undo — and could be taken back HALF way, which is a state the
	// editor should not be able to be in.
	//
	// **SO WHAT IS ASSERTED IS THE COUNT, NOT `canUndo()`.** The sibling case above asks whether the
	// history moved, which is the right question for "an identical copy records nothing" and is blind
	// to this defect entirely: twenty-four steps and one step both answer yes. A count is the only
	// assertion that separates them.
	namespace dr = feathers::fx::drawn;
	FilterFixture f;
	ui::WidgetRegistry registry;
	const size_t slot = f.bridge.addFxDevice(dr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	// THE STATE ABOVE THE WIDGETS THAT WATCH IT (frame_watchers.h): the panel's knobs are on this list.
	ui::ModState mod;
	ui::EditHistory history;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), dr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrawnExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 560.0f, panel->minimumSize().y);
	panel->resized();

	panel->setModulation(&mod, &history);

	const auto write = [&](size_t index, double v) {
		ui::FxControl c { f.bridge, f.bridge.fxDeviceAt(slot), dr::kDescriptor, index };
		c.setValue(v);
	};

	// FRAME 0 MOVED IN EVERY BAND, so a verb aimed at it has plenty to change and "one step" is a
	// claim about many fields rather than about one that happened to move.
	panel->selectFrame(0);
	for (size_t b = 0; b < dr::kBands; ++b) {
		write(dr::bandFieldIndex(0, b, dr::PoleRadius), 0.55 + 0.05 * static_cast<double>(b));
		write(dr::bandFieldIndex(0, b, dr::PoleAngle), -9.0 + static_cast<double>(b));
	}
	f.bridge.refresh();
	for (size_t b = 0; b < dr::kBands; ++b) {
		panel->applyType(b, dr::Type::Notch);
	}
	f.bridge.refresh();
	// THE FIXTURE IS DOING WHAT IT CLAIMS: frame 0 and frame 1 really differ, so a copy between them
	// has work to do and the step count below is not counting an empty verb.
	REQUIRE_FALSE(panel->frameAt(0) == panel->frameAt(1));

	SUBCASE("copy is one step, and undo restores every field of the frame it overwrote") {
		const dr::Frame destinationBefore = panel->frameAt(1);
		const dr::Frame source = panel->frameAt(0);
		const size_t stepsBefore = history.length();

		panel->copyFrame(0, 1);
		f.bridge.refresh();

		// THE COUNT. Against the defect this reads 30 (six bands times five fields), which is the
		// number a hand would have had to press undo.
		CHECK(history.length() == stepsBefore + 1);
		// AND THE COPY REALLY HAPPENED — the second assertion, so "one step" is not one step of
		// nothing.
		CHECK(panel->frameAt(1) == source);

		history.undo();
		f.bridge.refresh();
		// **THE WHOLE FRAME, IN ONE PRESS.** Asserted as the frame's own equality — every band's pole,
		// zero AND type — rather than as a spot check on one field, because the defect this guards is
		// precisely a verb coming back in pieces.
		CHECK(panel->frameAt(1) == destinationBefore);
		// AND ONE UNDO DID NOT OVERSHOOT INTO THE FIXTURE'S OWN EDITS: the frame the copy READ is
		// untouched. Without this, "one press put it back" would also be satisfied by a press that put
		// back more than the copy.
		CHECK(panel->frameAt(0) == source);

		history.redo();
		f.bridge.refresh();
		CHECK(panel->frameAt(1) == source);
	}

	SUBCASE("flatten is one step, and reset is one step") {
		const dr::Frame before = panel->frameAt(0);
		size_t steps = history.length();

		panel->flattenFrame(0);
		f.bridge.refresh();
		CHECK(history.length() == steps + 1);
		// THE FLATTEN HAPPENED, or the count is about a verb that did nothing.
		REQUIRE_FALSE(panel->frameAt(0) == before);
		history.undo();
		f.bridge.refresh();
		CHECK(panel->frameAt(0) == before);
		history.redo();
		f.bridge.refresh();

		steps = history.length();
		panel->resetFrame(0);
		f.bridge.refresh();
		CHECK(history.length() == steps + 1);
		// AND RESET LANDED ON THE SHIPPED FRAME, which is what makes the step a reset and not any
		// other single-step verb.
		CHECK(panel->frameAt(0) == dr::kDefaultFrames[0]);
	}

	SUBCASE("the TYPE menu is one step too, however many fields it moves") {
		// **THE SAME RULE, ON THE OTHER MENU THIS PANEL OPENS.** `applyType` writes the type and up to
		// two zero coordinates, and its own comment already claimed to bracket them "as one edit ... a
		// host recording automation wants one step rather than three bare sets". It did not: nesting
		// `DeviceControl` gestures nests nothing, so each `endGesture` recorded its own step and a type
		// choice cost three undos. The intent was written down and the mechanism could not express it,
		// which is exactly the kind of comment a case has to be pointed at.
		//
		// FRAME 2, UNTOUCHED BY THE FIXTURE ABOVE, so the band starts as the shipped Bypass and a Notch
		// really moves all three fields.
		panel->selectFrame(2);
		const size_t band = 1;
		const dr::Band before = panel->frameBand(2, band);
		REQUIRE(before.type == dr::Type::Bypass);
		const size_t steps = history.length();

		panel->applyType(band, dr::Type::Notch);
		f.bridge.refresh();
		CHECK(history.length() == steps + 1);
		// MORE THAN ONE FIELD REALLY MOVED, or "one step" is one step of one field and the case proves
		// nothing about compounding. **NOT "three": MEASURED at two here** — a Bypass band's zero
		// already sits at its pole's angle, so Notch moves the type and the zero RADIUS and leaves the
		// angle where it was. The count of fields is the device's business; what this case owns is
		// that however many move, they move as one step.
		const dr::Band after = panel->frameBand(2, band);
		REQUIRE(after.type == dr::Type::Notch);
		REQUIRE_FALSE(after.zero == before.zero);

		history.undo();
		f.bridge.refresh();
		// THE WHOLE BAND BACK IN ONE PRESS — type and zero together, which is the thing three separate
		// steps could not promise.
		CHECK(panel->frameBand(2, band) == before);
	}

	SUBCASE("a verb that changes nothing still records nothing") {
		// THE PROPERTY THE SIBLING CASE PINS, RE-ASSERTED HERE because it moved house: it used to fall
		// out of `DeviceControl::endGesture` refusing an unmoved value, and it is now `recordFields`
		// refusing an empty list. A property that survives a change of mechanism is worth asserting at
		// the new mechanism too.
		panel->copyFrame(0, 1);
		f.bridge.refresh();
		const size_t steps = history.length();
		panel->copyFrame(0, 1);
		f.bridge.refresh();
		CHECK(history.length() == steps);
		// NOT VACUOUS: a copy that DOES change something still records its one step.
		panel->copyFrame(2, 1);
		f.bridge.refresh();
		CHECK(history.length() == steps + 1);
	}
}
TEST_CASE("an amount gesture is one undo step on EVERY control kind, not just the rack's") {
	// THE CASE ONE DOOR UP ASSERTS THIS FOR `RackControl` AND ONLY FOR IT. Measured by dropping each
	// `history_->record(...)` in `src/plugin/ui` — construct the record and discard it, so no argument
	// is stranded and the mutation cannot fail to compile — and running the whole binary:
	//
	//   RackControl::endAmountGesture   dropped -> RED    (that case)
	//   FxControl::endAmountGesture     dropped -> GREEN, 477 of 477
	//   MsegControl::endAmountGesture   dropped -> GREEN, 477 of 477
	//
	// SO AN AMOUNT DRAGGED ONTO AN FX CONTROL OR AN MSEG CONTROL WAS NOT AN UNDO STEP AT ALL, which is
	// precisely the bug the rack's case was written for — *"ValueBox captured where the amount
	// started, in a member called beforeAmount_, and then never used it" — surviving on the two
	// chains that case does not reach. The convention was fixed where it was found and asserted where
	// it was found.
	//
	// CONSTRUCTED THE WAY THE RACK'S CASE CONSTRUCTS ITS CONTROL, deliberately: the claim is about the
	// CONTROL's gesture pair, not about any widget's drag, and a road driven through a knob would be
	// asserting the knob as well.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace ms = feathers::mod::mseg;
	namespace flt = feathers::fx::filter;

	REQUIRE(f.bridge.addFxDevice(flt::kTraits.tag) < kMaxFxSlots);
	REQUIRE(f.bridge.addRackDevice(ms::kTraits.tag) < kMaxRackSlots);
	f.bridge.refresh();

	// A MACRO IS THE SOURCE, because it may reach both chains — a rack source reaching its own
	// device's control is a different question and not this one.
	const backplane::Source source = backplane::Source::core(0);
	mod.focus(source);
	mod.arm(true);
	REQUIRE(mod.armed());

	/// One gesture on one control: the amount lands, it costs exactly one step, and the step goes
	/// back to where the drag started.
	const auto oneStep = [&](device::Modulatable& control, backplane::Destination dest, const char* what) {
		INFO("control " << std::string(what));
		REQUIRE(f.bridge.mayRoute(source, dest));
		const float before = f.bridge.routeAmount(source, dest);
		const size_t steps = history.length();

		control.beginAmountGesture();
		control.setAmount(0.75f);
		control.endAmountGesture();
		// THE WRITE LANDED. Without this the undo below would be undoing nothing and passing for it.
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(0.75f));
		// EXACTLY ONE STEP FOR THE WHOLE GESTURE — not "at least one", because a road that recorded
		// per mouse-move would satisfy a `>` and cost a player one ctrl-Z per pixel.
		CHECK(history.length() == steps + 1);

		history.undo();
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(before));
		history.redo();
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(0.75f));
	};

	SUBCASE("an fx device control") {
		ui::FxControl cutoff { f.bridge, f.bridge.fxDeviceAt(0), flt::Param::Cutoff, &mod, &history };
		oneStep(cutoff, cutoff.destination(), "FxControl");
	}

	SUBCASE("an MSEG control") {
		// SLOT 1: the fixture's editor already holds a rack device at 0, so the MSEG added above is
		// the second one. Taken from the rack rather than assumed, for the reason the fold case
		// learned the hard way.
		const size_t slot = f.bridge.modRack().count - 1;
		ui::MsegControl rate { f.bridge, f.bridge.rackDeviceAt(slot), ms::Param::TimeScale, &mod, &history };
		oneStep(rate, rate.destination(), "MsegControl");
	}
}

