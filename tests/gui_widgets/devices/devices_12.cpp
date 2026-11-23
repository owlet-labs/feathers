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

// PIECE 12 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

namespace {
/// An EQ analyser frame: the meter, the counters `a` and `b`, the view it was computed in, and every PRE /
/// POST bin at one level.
std::array<float, feathers::fx::eq::kReadingCount> eqFrame(float a, float b, float preDb, float postDb, float view = 0.0f) {
	namespace eq = feathers::fx::eq;
	std::array<float, eq::kReadingCount> v {};
	v[static_cast<size_t>(eq::Reading::OutPeak)] = 0.5f;
	v[static_cast<size_t>(eq::Reading::FrameA)] = a;
	v[static_cast<size_t>(eq::Reading::View)] = view;
	for (size_t k = 0; k < eq::kAnalyserBins; ++k) {
		v[static_cast<size_t>(eq::Reading::PreFirst) + k] = preDb;
		v[static_cast<size_t>(eq::Reading::PostFirst) + k] = postDb;
	}
	v[static_cast<size_t>(eq::Reading::FrameB)] = b;
	return v;
}
} // namespace

TEST_CASE("T11: a nested rack's macro offers the host rows after its macro rows, on the knob and on the pad, and exposing binds it") {
	// THE RACK EDITOR'S OUTER ROW, AS IT IS BUILT (`RackEditor::setOuterDevice`): the macro is the rack DEVICE's
	// control in the host that holds it — a `DeviceControl` over that host — seen through the polarity view,
	// inside a `MacroSlot`. By design (T11): a nested rack's macros are exposable like any device control. The view
	// answered `Unavailable` before it forwarded the host verbs, so neither face listed a host row.
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	engine.prepare(48000.0, 4096);
	feathers::rack::RackBridge top { engine, nullptr, feathers::rack::RackBridge::HostSlots::OwnPool };
	const feathers::DeviceInstanceId rackId = top.addDevice(feathers::composite::kTraits.tag);
	top.drain();
	REQUIRE(rackId != feathers::kNoInstance);
	const feathers::device::Descriptor* desc = top.descriptorOf(rackId);
	REQUIRE(desc != nullptr);
	const size_t m1 = static_cast<size_t>(feathers::composite::Param::Macro1);
	ui::DeviceControl outer { top, rackId, *desc, m1 };
	ui::BipolarMacro view { outer, [] { return false; } };
	CHECK(view.hostSlot().state == feathers::device::HostSlotStatus::State::Unslotted);
	const ui::MacroMenuHooks hooks {
		.readBipolar = [] { return false; },
		.writeBipolar = [](bool) {},
		.readType = [] { return feathers::device::MacroType::Knob; },
		.writeType = [](feathers::device::MacroType) {},
		.beginRename = [] {},
	};
	ui::MacroSlot slot { view, "Macro 1", ui::MacroSlot::Naming {}, hooks };

	// THE KNOB'S MENU: the macro rows, a break, then the host row.
	std::optional<visage::PopupMenu> knobMenu = slot.knob().contextMenu();
	REQUIRE(knobMenu.has_value());
	const std::vector<SeenRow> knobRows = rowsOf(*knobMenu);
	REQUIRE(knobRows.size() >= 3);
	CHECK(knobRows.front().label != "Expose to host"); // the macro's own rows come first
	CHECK(knobRows[knobRows.size() - 2].label.empty()); // the break
	CHECK(knobRows.back().label == "Expose to host");

	// THE PAD'S MENU IS THE SAME, and its pick is the one taken here.
	visage::PopupMenu pad = slot.padMenu();
	const std::vector<SeenRow> padRows = rowsOf(pad);
	REQUIRE(padRows.size() == knobRows.size());
	for (size_t i = 0; i < padRows.size(); ++i) {
		CAPTURE(i);
		CHECK(padRows[i].label == knobRows[i].label);
	}
	pad.onSelection().callback(padRows.back().id);

	// EXPOSED: slot 0, the rack device's Macro 1 by path `[rack]` and key, named "Chain · Macro 1".
	const hs::SlotPool& pool = top.hostSlots()->pool();
	CHECK(pool.state(0) == hs::SlotState::Pinned);
	CHECK(pool.target(0).path == std::vector<feathers::DeviceInstanceId> { rackId });
	CHECK(pool.target(0).key == "macro_1");
	CHECK(pool.target(0).label == "Chain " + kSlotDot + " Macro 1");
	CHECK(slot.knob().hostMarked());
	// AND THE VISIT BINDS IT: the engine drives the rack device's macro from the host's slot.
	(void)top.serviceHostSlots();
	CHECK(top.hostSlots()->boundTo(rackId, m1) == 0u);
}
TEST_CASE("EQ's analyser takes whole frames and drops a torn one") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.publish(eqFrame(1.0f, 1.0f, -30.0f, -20.0f));
	CHECK(g->update());
	CHECK(g->analyserForTest().framesTaken == 1);
	CHECK(g->analyserForTest().pre[40] == -30.0f);
	CHECK(g->analyserForTest().post[40] == -20.0f);
	// THE COUNTERS DISAGREE: the device was caught mid-write. Nothing of it is taken.
	d.publish(eqFrame(2.0f, 3.0f, -80.0f, -80.0f));
	g->update();
	CHECK(g->analyserForTest().framesDropped == 1);
	CHECK(g->analyserForTest().framesTaken == 1);
	CHECK(g->analyserForTest().post[40] == -20.0f);
}
TEST_CASE("EQ's analyser attacks at once and releases at the chosen speed") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.publish(eqFrame(1.0f, 1.0f, -20.0f, -20.0f));
	g->update();
	REQUIRE(g->analyserForTest().post[10] == -20.0f);
	// A QUIETER FRAME: one tick of the Medium release (250 ms) moves -20 toward -60 by 1 - e^(-16/250).
	d.publish(eqFrame(2.0f, 2.0f, -60.0f, -60.0f));
	g->update();
	const float expected = -20.0f + (-40.0f) * (1.0f - std::exp(-ui::EqGraph::kTickSeconds / 0.25f));
	CHECK(g->analyserForTest().post[10] == doctest::Approx(expected).epsilon(1e-4));
	// A LOUDER ONE arrives at once.
	d.publish(eqFrame(3.0f, 3.0f, -5.0f, -5.0f));
	g->update();
	CHECK(g->analyserForTest().post[10] == -5.0f);
}
TEST_CASE("EQ's analyser switched off repaints nothing, and its levels join the crosshair when on") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	// SLOPE COMPENSATION OFF (index 2 is 0 dB/oct), so the crosshair reads the published level itself.
	d.f.deviceHost.setField(d.device, eq::extraIndex(eq::Extra::AnalyserTilt), 2.0f);
	d.publish(eqFrame(1.0f, 1.0f, -30.0f, -24.0f));
	g->update();
	const auto geo = g->geometry();
	const std::string text = g->crosshairText({ geo.xForHz(1000.0), geo.yForDb(0.0) });
	CAPTURE(text);
	CHECK(text.find("pre -30  post -24 dB") != std::string::npos);

	// BOTH TAPS OFF: once the change itself is drawn, new frames repaint nothing.
	d.f.deviceHost.setField(d.device, eq::extraIndex(eq::Extra::Pre), 1.0f);
	d.f.deviceHost.setField(d.device, eq::extraIndex(eq::Extra::Post), 1.0f);
	g->update();
	d.publish(eqFrame(2.0f, 2.0f, -10.0f, -10.0f));
	CHECK_FALSE(g->update());
	CHECK(g->crosshairText({ geo.xForHz(1000.0), geo.yForDb(0.0) }).find("pre") == std::string::npos);
}
TEST_CASE("EQ's Channel box sits in an on band's strip and writes the band's channel to the extra state") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	ui::EqControls* c = d.card->controlsForTest();
	// BAND 1 IS OFF BY DEFAULT: no channel is offered for a band that does nothing.
	REQUIRE(g->selected() == 0);
	CHECK_FALSE(c->channelBoxForTest(0)->isVisible());
	// BAND 4, a bell, on: its box, and no other band's.
	g->select(3);
	d.set(eq::bandIndex(3, eq::BandField::On), 1.0f);
	CHECK(d.card->updateContent());
	ui::ValueBox* box = c->channelBoxForTest(3);
	CHECK(box->isVisible());
	CHECK_FALSE(c->channelBoxForTest(0)->isVisible());
	CHECK(box->control().text() == "Stereo");
	// INSIDE THE STRIP, clear of the type box.
	CHECK(box->bounds().right() <= c->boxForTest(eq::bandIndex(3, eq::BandField::Type))->bounds().right()
	                                   + ui::EqControls::kChannelWidth + ui::EqControls::kGap + 0.5f);
	CHECK(box->bounds().x() >= c->boxForTest(eq::bandIndex(3, eq::BandField::Type))->bounds().right());
	// CHOSEN: stored in the band's own field of the block, by value.
	box->control().setValue(2.0);
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::extraIndex(eq::channelField(3))) == 2.0f);
	CHECK(box->control().text() == "Side");
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::extraIndex(eq::channelField(2))) == 0.0f);
	// NOT A DESTINATION: it is no param, so nothing may be routed to it.
	CHECK_FALSE(box->control().display().isDestination);
	// A CUT needs the switch row for Type and Slope, so its box moves to the knob row's empty third cell,
	// below both and right of Q.
	d.set(eq::bandIndex(3, eq::BandField::Type), static_cast<float>(eq::BandType::HighCut));
	CHECK(d.card->updateContent());
	CHECK(box->isVisible());
	CHECK(box->bounds().y() >= c->boxForTest(eq::bandIndex(3, eq::BandField::Slope))->bounds().bottom());
	CHECK(box->bounds().x() >= c->knobForTest(eq::bandIndex(3, eq::BandField::Q))->bounds().right() - 0.5f);
}
TEST_CASE("EQ's view draws its own path: a Mid band is in the Mid curve and out of the others, and dims there") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.set(eq::bandIndex(3, eq::BandField::On), 1.0f);
	d.set(eq::bandIndex(3, eq::BandField::Freq), static_cast<float>(std::log2(440.0)));
	d.set(eq::bandIndex(3, eq::BandField::Gain), 6.0f);
	d.f.deviceHost.setField(d.device, eq::extraIndex(eq::channelField(3)), static_cast<float>(eq::Channel::Mid));
	const auto geo = g->geometry();
	const visage::Point at { geo.xForHz(440.0), geo.yForDb(0.0) };
	const auto viewIs = [&](eq::Channel c) {
		d.f.deviceHost.setField(d.device, eq::extraIndex(eq::Extra::AnalyserChannel), static_cast<float>(c));
	};
	// STEREO: the band is on no left-and-right path, so the curve is flat at 440 — but the Mid path is not.
	auto v = g->view();
	CHECK(v.viewChannel == eq::Channel::Stereo);
	CHECK(ui::EqGraph::anyMidSide(v));
	CHECK(ui::EqGraph::pathDb(v, eq::Channel::Stereo, 440.0) == doctest::Approx(0.0).epsilon(1e-9));
	CHECK(ui::EqGraph::pathDb(v, eq::Channel::Mid, 440.0) == doctest::Approx(6.0).epsilon(1e-6));
	CHECK(g->crosshairText(at).find("EQ +0.0 dB") != std::string::npos);
	CHECK_FALSE(ui::EqGraph::inPath(v.channel[3], v.viewChannel));
	// MID: the curve and the readout are the band's.
	viewIs(eq::Channel::Mid);
	v = g->view();
	CHECK(v.viewChannel == eq::Channel::Mid);
	CHECK(g->crosshairText(at).find("Mid +6.0 dB") != std::string::npos);
	CHECK(ui::EqGraph::inPath(v.channel[3], v.viewChannel));
	// SIDE: flat again, and the band is out of it.
	viewIs(eq::Channel::Side);
	v = g->view();
	CHECK(g->crosshairText(at).find("Side +0.0 dB") != std::string::npos);
	CHECK_FALSE(ui::EqGraph::inPath(v.channel[3], v.viewChannel));
	// A STEREO BAND IS IN EVERY PATH.
	CHECK(ui::EqGraph::inPath(eq::Channel::Stereo, eq::Channel::Side));
	CHECK(ui::EqGraph::inPath(eq::Channel::Stereo, eq::Channel::Mid));
}
TEST_CASE("EQ's Phase is a two-state chip in the panel's globals row that writes the stored Phase, and not on the card") {
	namespace eq = feathers::fx::eq;
	// THE CARD: no chip (its globals row has no room for a fifth cell); Phase is not among what it reaches.
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	CHECK(d.card->controlsForTest()->chipForTest(eq::paramIndex(eq::Param::Phase)) == nullptr);
	// THE PANEL: the chip, level with the Output knob and to its right.
	std::vector<std::unique_ptr<ui::DeviceControl>> adapters;
	for (size_t i = 0; i < eq::kParamCount; ++i) {
		adapters.push_back(std::make_unique<ui::DeviceControl>(d.f.deviceHost, d.device, eq::kDescriptor, i));
	}
	ui::EqControls controls(*d.card, d.f.deviceHost, d.device,
	                        [&](size_t i) -> ui::DeviceControl& { return *adapters[i]; }, nullptr, 34.0f, true);
	controls.layOut({ 0.0f, 0.0f, 400.0f, 80.0f }, { 0.0f, 100.0f, 400.0f, 60.0f }, 3, true);
	ui::SwitchChip* chip = controls.chipForTest(eq::paramIndex(eq::Param::Phase));
	REQUIRE(chip != nullptr);
	CHECK(chip->isVisible());
	ui::Knob* output = controls.knobForTest(eq::paramIndex(eq::Param::Output));
	CHECK(chip->bounds().x() >= output->bounds().right() - 0.5f);
	CHECK(chip->bounds().y() >= output->bounds().y());
	CHECK(chip->bounds().bottom() <= output->bounds().bottom());
	// Two states, so the chip IS the choice (the multivalue-toggle rule): Minimum, then Linear.
	CHECK(chip->control().value() == 0.0);
	chip->control().setValue(1.0);
	CHECK(d.f.deviceHost.fieldAt(d.device, eq::paramIndex(eq::Param::Phase)) == 1.0f);
	const std::vector<size_t> addressed = controls.addressedParams();
	CHECK(std::find(addressed.begin(), addressed.end(), eq::paramIndex(eq::Param::Phase)) != addressed.end());
}
TEST_CASE("EQ's analyser drops a frame computed in another view, and a new view starts from nothing") {
	namespace eq = feathers::fx::eq;
	DynCardFixture<ui::EqCard> d(eq::kTraits.tag);
	ui::EqGraph* g = d.card->graphForTest();
	d.publish(eqFrame(1.0f, 1.0f, -30.0f, -20.0f));
	g->update();
	REQUIRE(g->analyserForTest().post[40] == -20.0f);
	// THE VIEW MOVES TO SIDE: what was shown was the stereo signal's, so it goes.
	d.f.deviceHost.setField(d.device, eq::extraIndex(eq::Extra::AnalyserChannel), static_cast<float>(eq::Channel::Side));
	d.publish(eqFrame(2.0f, 2.0f, -10.0f, -10.0f, static_cast<float>(eq::Channel::Stereo)));
	CHECK(g->update());
	CHECK(g->analyserForTest().post[40] == eq::kAnalyserFloorDb);
	// ...and the device's next frame, still in the old view, is not taken.
	CHECK(g->analyserForTest().framesOtherView == 1);
	CHECK(g->analyserForTest().framesTaken == 1);
	// THE FIRST SIDE FRAME is.
	d.publish(eqFrame(3.0f, 3.0f, -50.0f, -40.0f, static_cast<float>(eq::Channel::Side)));
	g->update();
	CHECK(g->analyserForTest().framesTaken == 2);
	CHECK(g->analyserForTest().post[40] == -40.0f);
}

namespace {
dspr::Settings disperserSettingsOf(const feathers::BridgeDeviceHost& host, DeviceInstanceId id) {
	dspr::Row row {};
	const size_t present = std::min(host.fieldsPresentAt(id), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host.fieldAt(id, i);
	}
	return dspr::stateOf(row, present).settings;
}
} // namespace

TEST_CASE("DISPERSER's plot draws the engine's own group delay, point for point, and marks the centre") {
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	d.set(static_cast<size_t>(dspr::Param::KeyTrack), 0.0f);
	d.set(static_cast<size_t>(dspr::Param::Freq), 9.0f); // 512 Hz
	d.set(static_cast<size_t>(dspr::Param::Stages), 16.0f);
	(void)d.card->updateContent();
	ui::DisperserPlot* plot = d.card->plotForTest();
	REQUIRE(plot != nullptr);
	// THE EXPECTATION IS COMPUTED HERE, from the host's fields through the device's pure function — the
	// same function the engine's layout comes from (`test_fx_disperser` holds it to the engine's impulse
	// response); what this asserts is that the picture is drawn from it and not from something else.
	const dspr::ResponseModel want =
	    dspr::responseModelOf(disperserSettingsOf(d.f.deviceHost, d.device), false, 60.0, d.f.deviceHost.sampleRate());
	REQUIRE(plot->pointCountForTest() > 50);
	for (size_t i = 0; i < plot->pointCountForTest(); i += 7) {
		const auto g = dspr::groupDelayMs(want, plot->hzAtForTest(i));
		CAPTURE(i);
		CHECK(plot->delayMsForTest(i, 0) == doctest::Approx(g[0]).epsilon(1e-9));
		CHECK(plot->delayMsForTest(i, 1) == doctest::Approx(g[1]).epsilon(1e-9));
	}
	// THE HANDLE IS AT THE FREQ KNOB'S HERTZ.
	CHECK(plot->modelForTest().markerHz == doctest::Approx(512.0).epsilon(1e-6));
	CHECK(plot->markerXForTest() == doctest::Approx(plot->hzToXForTest(512.0)));
	// NON-VACUITY: there is a curve — the peak group delay near the centre is well above zero.
	double peak = 0.0;
	for (size_t i = 0; i < plot->pointCountForTest(); ++i) {
		peak = std::max(peak, plot->delayMsForTest(i, 0));
	}
	CHECK(peak > 1.0);
	CHECK(plot->axisTopMsForTest() >= peak);
}
TEST_CASE("DISPERSER's plot axis IS the Freq control, and a small drag never refits the delay axis under it") {
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	d.set(static_cast<size_t>(dspr::Param::KeyTrack), 0.0f);
	d.set(static_cast<size_t>(dspr::Param::Freq), 9.0f);
	(void)d.card->updateContent();
	ui::DisperserPlot* plot = d.card->plotForTest();
	const double axisBefore = plot->axisTopMsForTest();
	const float x0 = plot->markerXForTest();
	const float y = plot->height() * 0.5f;
	plot->onMouseDown().callback(at(x0, y));
	for (int step = 1; step <= 12; ++step) {
		const float x = x0 + static_cast<float>(step) * 2.0f;
		plot->onMouseDrag().callback(at(x, y));
		// THE VALUE WRITTEN IS THE POINTER'S HERTZ, AS LOG2 — the inverse of the marker's own mapping.
		CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::Freq))
		      == doctest::Approx(std::log2(plot->xToHzForTest(x))).epsilon(1e-5));
		CHECK(plot->axisTopMsForTest() == axisBefore);
	}
	plot->onMouseUp().callback(at(x0 + 24.0f, y));
	// NON-VACUITY: the drag moved the centre, and the curve with it.
	CHECK(plot->modelForTest().markerHz > 512.0 * 1.05);
}
TEST_CASE("DISPERSER's plot writes Offset, not Freq, when Key Track is on — and Offset's box is the one shown") {
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	REQUIRE(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::KeyTrack)) >= 0.5f); // the default
	(void)d.card->updateContent();
	CHECK(d.card->offsetBoxForTest()->isVisible());
	CHECK_FALSE(d.card->freqBoxForTest()->isVisible());
	ui::DisperserPlot* plot = d.card->plotForTest();
	const float freqBefore = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::Freq));
	const float x = plot->hzToXForTest(1000.0);
	plot->onMouseDown().callback(at(x, plot->height() * 0.5f));
	plot->onMouseUp().callback(at(x, plot->height() * 0.5f));
	// NO NOTE YET: the Offset knob is the absolute knob, index 64 at 0 octaves.
	const double index = std::log2(plot->xToHzForTest(x)) * 127.0 / 13.0;
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::Offset))
	      == doctest::Approx((index - 64.0) * 6.5 / 64.0).epsilon(1e-5));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::Freq)) == freqBefore);
	(void)d.card->updateContent();
	CHECK(plot->modelForTest().markerHz == doctest::Approx(plot->xToHzForTest(x)).epsilon(1e-3));
	// KEY TRACK OFF: the boxes trade places.
	d.set(static_cast<size_t>(dspr::Param::KeyTrack), 0.0f);
	CHECK(d.card->updateContent());
	CHECK(d.card->freqBoxForTest()->isVisible());
	CHECK_FALSE(d.card->offsetBoxForTest()->isVisible());
}
TEST_CASE("DISPERSER's control row: every value box shows its caption AND its number, side by side, in both Key states and at 1.75x") {
	// PHOTOGRAPHED (the lead's review of the first renders): the row read "Key  Offset 0.00  Stages 8  0.00"
	// — Gamma's caption lost under its number. A compact box draws its caption left and its number right
	// over the same row and never clips either, so a caption is readable exactly when the two do not meet.
	// That is measured here, with the fonts the box draws in, at every value each box can show.
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	const visage::Font captionFace = ui::font(ui::kKnobLabelSize);
	const visage::Font numberFace = ui::font(ui::kKnobReadoutSize);
	const auto widthOf = [](const visage::Font& face, const std::string& text) {
		return face.stringWidth(std::u32string(text.begin(), text.end()));
	};
	constexpr float kMinGap = ui::DisperserCard::kCaptionGap;
	const float was = d.card->dpiScale();
	for (const float factor : { 1.0f, 1.75f }) {
		CAPTURE(factor);
		d.f.root->setDpiScale(was * factor);
		d.card->resized();
		for (const float key : { 1.0f, 0.0f }) {
			CAPTURE(key);
			d.set(static_cast<size_t>(dspr::Param::KeyTrack), key);
			(void)d.card->updateContent();
			ui::ValueBox* centre = key >= 0.5f ? d.card->offsetBoxForTest() : d.card->freqBoxForTest();
			const std::vector<std::pair<ui::ValueBox*, dspr::Param>> row = {
				{ centre, key >= 0.5f ? dspr::Param::Offset : dspr::Param::Freq },
				{ d.card->stagesBoxForTest(), dspr::Param::Stages },
				{ d.card->gammaForTest(), dspr::Param::Gamma },
			};
			float lastRight = d.card->keyTrackForTest()->bounds().right();
			for (const auto& [box, param] : row) {
				REQUIRE(box != nullptr);
				const std::string caption = box->label();
				CAPTURE(caption);
				REQUIRE(box->isVisible());
				CHECK_FALSE(caption.empty());
				// INSIDE THE CARD, IN ORDER, NOT OVERLAPPING ITS NEIGHBOUR.
				CHECK(box->bounds().x() >= lastRight);
				CHECK(box->bounds().right() <= d.card->bodyLeft() + d.card->bodyWidth() + 0.5f);
				lastRight = box->bounds().right();
				// NATIVE follows the scale (CLAUDE.md's setBounds-on-a-scale rule).
				CHECK(static_cast<float>(box->nativeWidth())
				      == doctest::Approx(box->width() * was * factor).epsilon(0.03));
				const ui::ValueBox::ReadoutBox room = box->readoutBox();
				const device::ParamDesc& desc = dspr::kDescriptor.params[static_cast<size_t>(param)];
				const float before = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(param));
				// GAMMA HAS UNLIMITED RANGE (`GammaBox`'s rule) — ±8.4 million declared — and no row fits
				// eight digits beside a word. Its box is held to what a hand reaches by dragging: two digits.
				const float lo = param == dspr::Param::Gamma ? -99.75f : static_cast<float>(desc.min);
				const float hi = param == dspr::Param::Gamma ? 99.75f : static_cast<float>(desc.max);
				for (int i = 0; i <= 16; ++i) {
					const float v = lo + (hi - lo) * static_cast<float>(i) / 16.0f;
					d.set(static_cast<size_t>(param), v);
					const std::string number = box->readout();
					CAPTURE(number);
					CHECK(widthOf(captionFace, caption) + kMinGap + widthOf(numberFace, number) <= room.width);
				}
				d.set(static_cast<size_t>(param), before);
			}
		}
	}
	d.f.root->setDpiScale(was);
}
TEST_CASE("DISPERSER's Topo and Twist are the house's mode cells: no value changes the dropdown, and Topo's phase and Twist's are infinite") {
	// Topo and Twist are standard zone controls — a dropdown and an infinite value box in one cell — and
	// increasing the offset never changes the dropdown's entry. On the card and in the window.
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, dspr::kDescriptor);
	auto* panel = dynamic_cast<ui::DisperserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	const auto engineZone = [&](dspr::Param coordinateOf) {
		const dspr::Settings s = disperserSettingsOf(d.f.deviceHost, d.device);
		const double c = coordinateOf == dspr::Param::TopoMode ? s.topo : s.twist;
		const dspr::Coordinate k = dspr::splitCoordinate(c);
		return std::pair { static_cast<long>(k.lap), dspr::zoneOf(k.knob).index };
	};
	for (ui::ModeValueCell* cell : { d.card->topoForTest(), d.card->twistForTest(), panel->topoForTest(), panel->twistForTest() }) {
		REQUIRE(cell != nullptr);
	}
	// THE DROPDOWNS NAME THE DEVICE'S MODES, and the cells' followers are the gated controls.
	CHECK(d.card->topoForTest()->select().zoneCount() == 8);
	CHECK(d.card->twistForTest()->select().zoneCount() == 6);
	CHECK(d.card->twistForTest()->select().captionOf(5) == "Twist");
	CHECK(d.card->topoForTest()->followerParams() == std::vector<size_t> { static_cast<size_t>(dspr::Param::TopoPhase) });
	CHECK(d.card->twistForTest()->followerParams()
	      == std::vector<size_t> { static_cast<size_t>(dspr::Param::TwistPos), static_cast<size_t>(dspr::Param::TwistPhase) });
	// INFINITE (round 3, 2026-09-27): every Topo mode, the phase driven far past its zone through its own box —
	// it goes there, and the zone the engine reads is still the mode's, the rest carried as the lap.
	for (uint32_t mode = 0; mode < 8; ++mode) {
		d.set(static_cast<size_t>(dspr::Param::TopoMode), static_cast<float>(mode));
		(void)d.card->updateContent();
		CHECK(d.card->topoForTest()->activeParam() == static_cast<long>(dspr::Param::TopoPhase));
		ui::ValueBox* box = d.card->topoForTest()->boxFor(static_cast<size_t>(dspr::Param::TopoPhase));
		REQUIRE(box != nullptr);
		for (const double phase : { 5.0, -50.5, 50.25 }) {
			box->control().setValue(phase); // a hand dragging far past either end
			d.f.bridge.refresh();
			CAPTURE(mode);
			CAPTURE(phase);
			CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::TopoPhase)) == doctest::Approx(phase));
			CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::TopoMode)) == static_cast<float>(mode));
			CHECK(engineZone(dspr::Param::TopoMode) == std::pair { 0L, static_cast<int>(mode) });
			CHECK(disperserSettingsOf(d.f.deviceHost, d.device).topoLap
			      == doctest::Approx(phase - std::clamp(phase, 0.0, 1.0)));
		}
	}
	// THE FIVE FENCED TWIST MODES THE SAME, and the box shown is the position's.
	for (uint32_t mode = 0; mode < 5; ++mode) {
		d.set(static_cast<size_t>(dspr::Param::TwistMode), static_cast<float>(mode));
		(void)d.card->updateContent();
		CHECK(d.card->twistForTest()->activeParam() == static_cast<long>(dspr::Param::TwistPos));
		d.set(static_cast<size_t>(dspr::Param::TwistPos), 1.0f);
		CAPTURE(mode);
		CHECK(engineZone(dspr::Param::TwistMode) == std::pair { 0L, static_cast<int>(mode) });
	}
	// TWIST, THE INFINITE ONE: its phase shows, and it turns on through the laps.
	d.set(static_cast<size_t>(dspr::Param::TwistMode), 5.0f);
	CHECK(d.card->updateContent());
	CHECK(d.card->twistForTest()->activeParam() == static_cast<long>(dspr::Param::TwistPhase));
	d.set(static_cast<size_t>(dspr::Param::TwistPhase), 3.25f);
	const auto [lap, zone] = engineZone(dspr::Param::TwistMode);
	CHECK(lap == 3);
	CHECK(zone == 7); // 5/8 + 3.25 = 3.875: the eighth zone of the fourth lap
	(void)panel->updateContent();
	CHECK(panel->twistForTest()->activeParam() == static_cast<long>(dspr::Param::TwistPhase));
}
TEST_CASE("DISPERSER's pills are the Shaper's: an amount and a power over each derived quantity, and each one is heard") {
	// They are the Shaper's enable/value pills. A `FaderPill` per quantity, over
	// its amount and its power, on the card and in the window. The honesty rule asked as a VALUE: powered with its
	// amount at the ends, each pill moves the quantity the engine reads (`applyPills`, through the plot's model of
	// the engine); ON AT 0.5 — the default since round 3 — it is the zones' own; off it disables the extra.
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	// Twist a lap in, over Owlpass: every quantity live and none at its off value (`test_fx_disperser`'s pills case).
	d.set(static_cast<size_t>(dspr::Param::TwistMode), 5.0f);
	d.set(static_cast<size_t>(dspr::Param::TwistPhase), 1.1f);
	d.set(static_cast<size_t>(dspr::Param::TopoMode), 2.0f);
	d.set(static_cast<size_t>(dspr::Param::TopoPhase), 0.2f);
	(void)d.card->updateContent();
	const auto model = [&] {
		return dspr::responseModelOf(disperserSettingsOf(d.f.deviceHost, d.device), false, 60.0, 48000.0);
	};
	const auto quantity = [](const dspr::ResponseModel& m, dspr::Pill p) {
		switch (p) {
		case dspr::Pill::Width: return m.twist.width;
		case dspr::Pill::Punch: return m.twist.punch;
		case dspr::Pill::Chirp: return m.twist.chirpAmount;
		case dspr::Pill::Curve: return m.twist.spreadCurve;
		case dspr::Pill::QTilt: return m.twist.qTilt;
		case dspr::Pill::Q: return m.topo.q;
		case dspr::Pill::Detune: return m.topo.detuning;
		case dspr::Pill::Emphasis: return m.topo.emphasis;
		default: return m.topo.harmonicBlend;
		}
	};
	const visage::Bounds body { d.card->bodyLeft(), 0.0f, d.card->bodyWidth(), d.card->height() };
	for (size_t i = 0; i < dspr::kPillCount; ++i) {
		const auto pill = static_cast<dspr::Pill>(i);
		CAPTURE(std::string(dspr::kPillNames[i]));
		ui::FaderPill* widget = d.card->pillForTest(i);
		REQUIRE(widget != nullptr);
		CHECK(widget->isVisible());
		CHECK(widget->bounds().right() <= body.right() + 0.5f);
		CHECK(widget->bounds().bottom() <= d.card->height() + 0.5f);
		// THE DEFAULT, ON AT 0.5: the zones' own.
		const double zones = quantity(model(), pill);
		// ON, AT THE TOP AND AT THE BOTTOM: the quantity moves, and the two ends differ.
		d.set(static_cast<size_t>(dspr::pillAmountParam(pill)), 1.0f);
		const double top = quantity(model(), pill);
		d.set(static_cast<size_t>(dspr::pillAmountParam(pill)), 0.0f);
		const double bottom = quantity(model(), pill);
		CHECK(top != doctest::Approx(bottom));
		// OFF: the extra disabled, whatever the amount.
		d.set(static_cast<size_t>(dspr::pillAmountParam(pill)), 1.0f);
		d.set(static_cast<size_t>(dspr::pillOnParam(pill)), 0.0f);
		CHECK(quantity(model(), pill) == dspr::kPillOff[i]);
		CHECK(zones != doctest::Approx(dspr::kPillOff[i]).epsilon(1e-3));
		// BACK ON AT 0.5: the zones' own, exactly.
		d.set(static_cast<size_t>(dspr::pillOnParam(pill)), 1.0f);
		d.set(static_cast<size_t>(dspr::pillAmountParam(pill)), 0.5f);
		CHECK(quantity(model(), pill) == zones);
	}
	// NO TWO PILLS OVERLAP.
	for (size_t i = 0; i < dspr::kPillCount; ++i) {
		for (size_t j = i + 1; j < dspr::kPillCount; ++j) {
			const visage::Bounds a = d.card->pillForTest(i)->bounds();
			const visage::Bounds b = d.card->pillForTest(j)->bounds();
			CHECK((a.right() <= b.x() || b.right() <= a.x() || a.bottom() <= b.y() || b.bottom() <= a.y()));
		}
	}
	// AND THE WINDOW HAS THEM ALL.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, dspr::kDescriptor);
	auto* panel = dynamic_cast<ui::DisperserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	for (size_t i = 0; i < dspr::kPillCount; ++i) {
		REQUIRE(panel->pillForTest(i) != nullptr);
		CHECK(panel->pillForTest(i)->bounds().bottom() <= panel->height());
	}
}
TEST_CASE("DISPERSER's pills default ON at the zones' own values: every power lit on the card and in the window, and the plot is the firmware's") {
	// Round 3: pills default to on, and switching one off disables its extra; they default to the neutral
	// values that leave Twist as the factory has it. A fresh card: every pill powered at 0.5, and the
	// plot's model reads exactly what the firmware's two modes derive at the same coordinate.
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	// Twist a lap in, over Owlpass: every quantity live and none at its off value (`test_fx_disperser`'s pills case).
	d.set(static_cast<size_t>(dspr::Param::TwistMode), 5.0f);
	d.set(static_cast<size_t>(dspr::Param::TwistPhase), 1.1f);
	d.set(static_cast<size_t>(dspr::Param::TopoMode), 2.0f);
	d.set(static_cast<size_t>(dspr::Param::TopoPhase), 0.2f);
	(void)d.card->updateContent();
	const dspr::Settings s = disperserSettingsOf(d.f.deviceHost, d.device);
	const dspr::ResponseModel m = dspr::responseModelOf(s, false, 60.0, 48000.0);
	const dspr::Coordinate topoC = dspr::splitCoordinate(static_cast<double>(s.topo));
	const dspr::Coordinate twistC = dspr::splitCoordinate(static_cast<double>(s.twist));
	const dspr::TwistParams twist = dspr::computeTwistParams(twistC.knob, twistC.lap);
	const dspr::TopoParams topo = dspr::computeTopoParams(topoC.knob, topoC.lap, twist.phaseOffset);
	CHECK(m.twist.width == twist.width);
	CHECK(m.twist.punch == twist.punch);
	CHECK(m.twist.chirpAmount == twist.chirpAmount);
	CHECK(m.twist.spreadCurve == twist.spreadCurve);
	CHECK(m.twist.qTilt == twist.qTilt);
	CHECK(m.topo.q == topo.q);
	CHECK(m.topo.detuning == topo.detuning);
	CHECK(m.topo.emphasis == topo.emphasis);
	CHECK(m.topo.harmonicBlend == topo.harmonicBlend);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, dspr::kDescriptor);
	auto* panel = dynamic_cast<ui::DisperserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	for (size_t i = 0; i < dspr::kPillCount; ++i) {
		CAPTURE(std::string(dspr::kPillNames[i]));
		CHECK(s.pillOn[i]);
		CHECK(s.pillAmount[i] == 0.5f);
		REQUIRE(d.card->pillForTest(i) != nullptr);
		CHECK(d.card->pillForTest(i)->poweredForTest());
		REQUIRE(panel->pillForTest(i) != nullptr);
		CHECK(panel->pillForTest(i)->poweredForTest());
	}
}

namespace {
/// **A `PillStack` WITH NO CARD (backlog IL — the widget lands before its first user).** Three of DISPERSER's
/// pills, because that device has nine real amount-and-power pairs: the stack is mounted on the editor's root at
/// a knob cell's size, over adapters of its own, and taken off again before the adapters go.
struct PillStackFixture {
	ModFixture f;
	DeviceInstanceId device = kNoInstance;
	std::vector<std::unique_ptr<ui::DeviceControl>> controls;
	std::unique_ptr<ui::PillStack> stack;
	std::array<dspr::Pill, 3> pills { dspr::Pill::Punch, dspr::Pill::Chirp, dspr::Pill::Harmonic };

	PillStackFixture() {
		const size_t slot = f.bridge.addFxDevice(dspr::kTraits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		device = f.bridge.fxDeviceAt(slot);
		controls.resize(static_cast<size_t>(dspr::Param::kCount));
		std::array<ui::PillStack::Spec, ui::PillStack::kCount> specs;
		for (size_t k = 0; k < ui::PillStack::kCount; ++k) {
			specs[k] = { &control(dspr::pillAmountParam(pills[k])), &control(dspr::pillOnParam(pills[k])),
				         dspr::kPillNames[static_cast<size_t>(pills[k])] };
		}
		stack = std::make_unique<ui::PillStack>(std::move(specs), "Echoes");
		f.root->addChild(stack.get());
		stack->setBounds(10.0f, 10.0f, ui::PillStack::kMinWidth, ui::PillStack::kHeight);
	}
	~PillStackFixture() { f.root->removeChild(stack.get()); }

	ui::DeviceControl& control(dspr::Param p) {
		auto& c = controls[static_cast<size_t>(p)];
		if (c == nullptr) {
			c = std::make_unique<ui::DeviceControl>(f.deviceHost, device, dspr::kDescriptor, static_cast<size_t>(p));
		}
		return *c;
	}
	void set(dspr::Param p, float v) {
		f.bridge.setDeviceControl(device, static_cast<size_t>(p), v);
		f.bridge.refresh();
	}
	float field(dspr::Param p) const { return f.deviceHost.fieldAt(device, static_cast<size_t>(p)); }
	/// Every pill amount and power on the device, in `dspr::Pill` order — what a drag or a click may move.
	std::vector<float> allPillFields() const {
		std::vector<float> out;
		for (size_t i = 0; i < dspr::kPillCount; ++i) {
			out.push_back(field(dspr::pillAmountParam(static_cast<dspr::Pill>(i))));
			out.push_back(field(dspr::pillOnParam(static_cast<dspr::Pill>(i))));
		}
		return out;
	}
	/// The expected readout, through a SEPARATE adapter over the same control rather than through the stack.
	std::string text(dspr::Pill p) {
		ui::DeviceControl c(f.deviceHost, device, dspr::kDescriptor, static_cast<size_t>(dspr::pillAmountParam(p)));
		return std::string(dspr::kPillNames[static_cast<size_t>(p)]) + " " + c.text();
	}
};
} // namespace

TEST_CASE("IL: a pill stack's three pills each bind their own amount and power: a drag moves only that pill's amount, a click only its power") {
	PillStackFixture s;
	for (size_t k = 0; k < ui::PillStack::kCount; ++k) {
		const dspr::Pill pill = s.pills[k];
		const auto index = static_cast<size_t>(pill);
		CAPTURE(std::string(dspr::kPillNames[index]));
		ui::FaderPill& widget = s.stack->pill(k);
		CHECK(widget.caption() == dspr::kPillNames[index]);
		// A DRAG: 30 px right of the press is a quarter of the range (`FaderPill::kSpan`), so the amount goes
		// 0.5 -> 0.75 — and not one of the device's other seventeen pill controls moves.
		const std::vector<float> before = s.allPillFields();
		const float y = widget.height() * 0.5f;
		widget.onMouseDown().callback(at(10.0f, y));
		widget.onMouseDrag().callback(at(40.0f, y));
		widget.onMouseUp().callback(at(40.0f, y));
		std::vector<float> after = s.allPillFields();
		CHECK(after[2 * index] == doctest::Approx(0.75f).epsilon(1e-6));
		after[2 * index] = before[2 * index];
		CHECK(after == before);
		// A CLICK: its power goes off, its amount stays, and nothing else moves.
		widget.onMouseDown().callback(at(10.0f, y));
		widget.onMouseUp().callback(at(10.0f, y));
		after = s.allPillFields();
		CHECK(after[2 * index + 1] == 0.0f);
		CHECK(after[2 * index] == doctest::Approx(0.75f).epsilon(1e-6));
		after[2 * index + 1] = before[2 * index + 1];
		after[2 * index] = before[2 * index];
		CHECK(after == before);
		s.set(dspr::pillAmountParam(pill), 0.5f);
		s.set(dspr::pillOnParam(pill), 1.0f);
	}
}
TEST_CASE("D1: an amount-only pill stack (the two-pill form): a drag moves the amount, a click moves nothing, each pill offers only its amount, and the two sit where a three-stack's first two do") {
	PillStackFixture s;
	const dspr::Pill a = dspr::Pill::Punch;
	const dspr::Pill b = dspr::Pill::Chirp;
	ui::PillStack two(std::array<ui::PillStack::Spec, 2> { ui::PillStack::Spec { &s.control(dspr::pillAmountParam(a)), nullptr, "A" },
	                                                       ui::PillStack::Spec { &s.control(dspr::pillAmountParam(b)), nullptr, "B" } },
	                  "Pair");
	s.f.root->addChild(&two);
	two.setBounds(100.0f, 10.0f, ui::PillStack::kMinWidth, ui::PillStack::kHeight);
	CHECK(two.count() == 2);
	// WHERE A THREE-STACK'S FIRST TWO SIT, so a row of mixed stacks lines up.
	for (size_t k = 0; k < 2; ++k) {
		CAPTURE(k);
		CHECK(two.pillBounds(k).y() == doctest::Approx(s.stack->pillBounds(k).y()));
		CHECK(two.pillBounds(k).height() == doctest::Approx(s.stack->pillBounds(k).height()));
	}
	for (size_t k = 0; k < 2; ++k) {
		CAPTURE(k);
		const dspr::Pill pill = k == 0 ? a : b;
		ui::FaderPill& widget = two.pill(k);
		CHECK(widget.amountOnly());
		std::vector<ui::ControlFace::Offered> offered;
		widget.offeredControls(offered);
		REQUIRE(offered.size() == 1);
		CHECK(offered[0].control == &s.control(dspr::pillAmountParam(pill)));
		const float y = widget.height() * 0.5f;
		const std::vector<float> before = s.allPillFields();
		// A CLICK MOVES NOTHING: the section's chip is the switch, not the pill.
		widget.onMouseDown().callback(at(10.0f, y));
		widget.onMouseUp().callback(at(10.0f, y));
		CHECK(s.allPillFields() == before);
		// A DRAG MOVES ONLY ITS AMOUNT, a quarter of the range for 30 px.
		widget.onMouseDown().callback(at(10.0f, y));
		widget.onMouseDrag().callback(at(40.0f, y));
		widget.onMouseUp().callback(at(40.0f, y));
		const auto index = static_cast<size_t>(pill);
		std::vector<float> after = s.allPillFields();
		CHECK(after[2 * index] == doctest::Approx(before[2 * index] + 0.25f).epsilon(1e-6));
		after[2 * index] = before[2 * index];
		CHECK(after == before);
		s.set(dspr::pillAmountParam(pill), before[2 * index]);
	}
	// AND IT GREYS AS ONE.
	two.setActive(false);
	CHECK_FALSE(two.pill(0).active());
	CHECK_FALSE(two.pill(1).active());
	s.f.root->removeChild(&two);
}
TEST_CASE("IL: a pill stack's readout prints the hovered pill's value, else the last touched one's, else the group's name — and follows a value moved under it") {
	PillStackFixture s;
	ui::PillStack& stack = *s.stack;
	s.set(dspr::pillAmountParam(dspr::Pill::Punch), 0.2f);
	s.set(dspr::pillAmountParam(dspr::Pill::Chirp), 0.9f);
	(void)stack.refresh();
	// NOTHING TOUCHED: the group's name.
	CHECK(stack.shown() == -1);
	CHECK(stack.readoutText() == "Echoes");
	// HOVER CHIRP: Chirp's value.
	ui::FaderPill& punch = stack.pill(0);
	ui::FaderPill& chirp = stack.pill(1);
	chirp.onMouseEnter().callback(at(5.0f, 5.0f));
	CHECK(stack.shown() == 1);
	CHECK(stack.readoutText() == s.text(dspr::Pill::Chirp));
	CHECK(stack.readoutText() != s.text(dspr::Pill::Punch)); // the two differ, so the line is not vacuous
	// LEAVE IT: none hovered and none touched, the name again.
	chirp.onMouseExit().callback(at(-5.0f, 5.0f));
	CHECK(stack.readoutText() == "Echoes");
	// DRAG PUNCH, THEN HOVER NOTHING: the last touched — Punch, at the value the drag left.
	const float y = punch.height() * 0.5f;
	punch.onMouseEnter().callback(at(10.0f, y));
	punch.onMouseDown().callback(at(10.0f, y));
	punch.onMouseDrag().callback(at(34.0f, y));
	CHECK(stack.readoutText() == s.text(dspr::Pill::Punch)); // mid-drag, already the new value
	punch.onMouseUp().callback(at(34.0f, y));
	punch.onMouseExit().callback(at(-5.0f, y));
	CHECK(stack.shown() == 0);
	CHECK(s.field(dspr::pillAmountParam(dspr::Pill::Punch)) == doctest::Approx(0.4f).epsilon(1e-6));
	CHECK(stack.readoutText() == s.text(dspr::Pill::Punch));
	// HOVER BEATS TOUCHED: over 2f the readout reads 2f; off it, Punch again.
	stack.pill(2).onMouseEnter().callback(at(5.0f, 5.0f));
	CHECK(stack.readoutText() == s.text(dspr::Pill::Harmonic));
	stack.pill(2).onMouseExit().callback(at(-5.0f, 5.0f));
	CHECK(stack.readoutText() == s.text(dspr::Pill::Punch));
	// A VALUE MOVED UNDER IT WITH NO HAND ON IT (automation, undo): one repaint, and none at rest.
	(void)stack.refresh();
	RedrawProbe probe(&stack);
	CHECK_FALSE(stack.refresh());
	CHECK(probe.count == 0);
	const std::string dragged = stack.readoutText();
	s.set(dspr::pillAmountParam(dspr::Pill::Punch), 0.85f);
	probe.rearm();
	CHECK(stack.refresh());
	CHECK(probe.count == 1);
	CHECK(stack.readoutText() == s.text(dspr::Pill::Punch));
	CHECK(stack.readoutText() != dragged); // it moved off the dragged value
}
TEST_CASE("IL: a pill stack fits a knob's cell at every UI scale — no hit areas overlap, every pill inside the cell above its readout, the widest readout inside the cell, native rects at the scale") {
	PillStackFixture s;
	ui::PillStack& stack = *s.stack;
	// THE CELL IS A KNOB COLUMN — the Engine rail face's, as the cards' grids seat it.
	CHECK(ui::PillStack::kHeight == ui::Knob::columnHeight(ui::Knob::kEngineRailDiameter));
	const visage::Font readout = ui::font(ui::kKnobReadoutSize);
	const visage::Font caption = ui::font(ui::kKnobLabelSize);
	const auto widthOf = [](const visage::Font& face, const std::string& t) {
		return face.stringWidth(std::u32string(t.begin(), t.end()));
	};
	const float base = s.f.root->dpiScale(); // the display's; the user's scale multiplies it (EJ)
	for (const float factor : { 1.0f, 1.25f, 1.5f, 1.75f, 2.0f }) {
		CAPTURE(factor);
		const float scale = base * factor;
		s.f.root->setDpiScale(scale);
		stack.setBounds(10.0f, 10.0f, ui::PillStack::kMinWidth, ui::PillStack::kHeight);
		CHECK(std::abs(static_cast<float>(stack.nativeWidth()) - scale * ui::PillStack::kMinWidth) <= 1.0f);
		CHECK(std::abs(static_cast<float>(stack.nativeHeight()) - scale * ui::PillStack::kHeight) <= 1.0f);
		for (size_t k = 0; k < ui::PillStack::kCount; ++k) {
			const ui::FaderPill& p = stack.pill(k);
			CAPTURE(p.caption());
			// WHERE THE STACK SAYS, INSIDE IT, ABOVE THE READOUT, at the scale.
			CHECK(p.bounds() == stack.pillBounds(k));
			CHECK(p.x() >= 0.0f);
			CHECK(p.right() <= stack.width() + 0.01f);
			CHECK(p.bottom() <= stack.readoutBounds().y() + 0.01f);
			CHECK(p.height() >= 14.0f);
			CHECK(std::abs(static_cast<float>(p.nativeHeight()) - scale * p.height()) <= 1.0f);
			CHECK(std::abs(static_cast<float>(p.nativeWidth()) - scale * p.width()) <= 1.0f);
			// ITS CAPTION FITS ITS FACE.
			CHECK(widthOf(caption, p.caption()) <= p.width() - 8.0f);
			// NO TWO PILLS' HIT AREAS OVERLAP.
			for (size_t j = k + 1; j < ui::PillStack::kCount; ++j) {
				const visage::Bounds a = p.bounds();
				const visage::Bounds b = stack.pill(j).bounds();
				CHECK((a.right() <= b.x() || b.right() <= a.x() || a.bottom() <= b.y() || b.bottom() <= a.y()));
			}
		}
		// THE WIDEST READOUT ANY PILL OF DISPERSER'S NINE PRINTS — every caption at both ends and the middle — fits
		// the narrowest cell, and so does the title.
		for (size_t i = 0; i < dspr::kPillCount; ++i) {
			const auto param = dspr::pillAmountParam(static_cast<dspr::Pill>(i));
			for (const float v : { 0.0f, 0.5f, 1.0f }) {
				s.set(param, v);
				const std::string line = std::string(dspr::kPillNames[i]) + " "
				                         + ui::DeviceControl(s.f.deviceHost, s.device, dspr::kDescriptor,
				                                             static_cast<size_t>(param))
				                               .text();
				CAPTURE(line);
				CHECK(widthOf(readout, line) <= ui::PillStack::kMinWidth);
			}
			s.set(param, 0.5f);
		}
		CHECK(widthOf(readout, stack.title()) <= stack.width());
	}
}
TEST_CASE("DISPERSER's card repaints zero times at rest, and once when a value its plot draws moves") {
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	REQUIRE(d.card->isVisible());
	REQUIRE(d.card->isDrawing());
	(void)d.card->updateContent();
	RedrawProbe probe(d.card);
	for (int i = 0; i < 20; ++i) {
		CHECK_FALSE(d.card->updateContent());
	}
	CHECK(probe.count == 0);
	d.set(static_cast<size_t>(dspr::Param::TopoPhase), 0.4375f);
	probe.rearm();
	CHECK(d.card->updateContent());
	CHECK(probe.count == 1);
	probe.rearm();
	CHECK_FALSE(d.card->updateContent());
	CHECK(probe.count == 1);
}
TEST_CASE("DISPERSER's window: the feedback strip is the device's readings, and it repaints zero times at rest") {
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	// THROUGH THE FACTORY — so the case also proves the window is registered and reachable.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, dspr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panelPtr = dynamic_cast<ui::DisperserExpanded*>(view.get());
	REQUIRE(panelPtr != nullptr);
	ui::DisperserExpanded& panel = *panelPtr;
	panel.setBounds(0.0f, 0.0f, 700.0f, 420.0f);
	(void)panel.updateContent();
	CHECK_FALSE(panel.stripForTest().live); // nothing published is not silence
	std::array<float, static_cast<size_t>(dspr::Reading::kCount)> v {};
	v[static_cast<size_t>(dspr::Reading::ChirpGain)] = 0.3f;
	v[static_cast<size_t>(dspr::Reading::LoopGain)] = 1.1f;
	v[static_cast<size_t>(dspr::Reading::EchoMs)] = 127.0f;
	d.publish(v);
	CHECK(panel.updateContent());
	const auto strip = panel.stripForTest();
	CHECK(strip.live);
	CHECK(strip.chirp == 0.3f);
	CHECK(strip.loop == 1.1f);
	CHECK(strip.echoMs == 127.0f);
	const int repaints = panel.stripRepaintsForTest();
	for (int i = 0; i < 20; ++i) {
		CHECK_FALSE(panel.updateContent());
	}
	CHECK(panel.stripRepaintsForTest() == repaints);
}
TEST_CASE("DISPERSER's window plot: a vertical drag on the handle sets the Q offset, up sharpens, and the sideways road writes only Freq") {
	// In the window, where x is the offset, a y-axis drag modifies Q (added to the native Q setting). The
	// window's handle rides at the Q offset's height; a vertical drag moves it, relative to
	// the press and past a dead zone; X stays Freq's (or Offset's) alone. The card keeps the same road since NH (below).
	DynCardFixture<ui::DisperserCard> d(dspr::kTraits.tag);
	d.set(static_cast<size_t>(dspr::Param::KeyTrack), 0.0f);
	d.set(static_cast<size_t>(dspr::Param::Freq), 9.0f);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, dspr::kDescriptor);
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DisperserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 700.0f, 420.0f);
	(void)panel->updateContent();
	ui::DisperserPlot* plot = panel->plotForTest();
	const auto qOffset = [&] { return d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::QOffset)); };
	const auto freq = [&] { return d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(dspr::Param::Freq)); };
	const float ppo = plot->pixelsPerQOctaveForTest();
	const float dead = ui::DisperserPlot::kQDeadZone;
	REQUIRE(ppo > 20.0f);
	REQUIRE(qOffset() == 0.0f);
	// AT REST THE HANDLE IS AT THE MIDDLE, and the readout's seam maps the ends to ±3 octaves.
	const float y0 = plot->qHandleYForTest();
	CHECK(y0 == doctest::Approx(plot->qOffsetToYForTest(0.0)));
	CHECK(plot->qOffsetToYForTest(dspr::kQOffsetOctaves) == doctest::Approx(y0 - 3.0f * ppo));
	const float x0 = plot->markerXForTest();
	const double nativeQ = plot->modelForTest().topo.q;

	// A PRESS AND A WOBBLE INSIDE THE DEAD ZONE: the centre moves to the pointer, Q does not.
	plot->onMouseDown().callback(at(x0, y0));
	plot->onMouseDrag().callback(at(x0 + 6.0f, y0 - (dead - 0.5f)));
	CHECK(qOffset() == 0.0f);
	CHECK(freq() == doctest::Approx(std::log2(plot->xToHzForTest(x0 + 6.0f))).epsilon(1e-5));
	// UP BY A OCTAVE AND A HALF'S PIXELS PAST THE DEAD ZONE, AND SIDEWAYS: Q +1.5, and Freq is the pointer's
	// x alone — the same value a level drag to that x writes.
	const float x1 = x0 + 30.0f;
	plot->onMouseDrag().callback(at(x1, y0 - dead - 1.5f * ppo));
	CHECK(qOffset() == doctest::Approx(1.5f).epsilon(1e-4));
	CHECK(freq() == doctest::Approx(std::log2(plot->xToHzForTest(x1))).epsilon(1e-5));
	// FAR PAST THE TOP: held at +3.
	plot->onMouseDrag().callback(at(x1, -2000.0f));
	CHECK(qOffset() == doctest::Approx(dspr::kQOffsetOctaves));
	CHECK(freq() == doctest::Approx(std::log2(plot->xToHzForTest(x1))).epsilon(1e-5));
	plot->onMouseUp().callback(at(x1, -2000.0f));
	(void)panel->updateContent();
	// THE HANDLE SITS AT ITS NEW HEIGHT, AND THE PLOT'S MODEL IS THE SHARED LAW: native Q times 2^3, held to range.
	CHECK(plot->qHandleYForTest() == doctest::Approx(plot->qOffsetToYForTest(dspr::kQOffsetOctaves)));
	const dspr::Settings s = disperserSettingsOf(d.f.deviceHost, d.device);
	CHECK(plot->modelForTest().topo.q == dspr::responseModelOf(s, false, 60.0, d.f.deviceHost.sampleRate()).topo.q);
	CHECK(plot->modelForTest().topo.q == doctest::Approx(std::clamp(nativeQ * 8.0, dspr::kQMin, dspr::kQMax)));
	CHECK(plot->modelForTest().topo.q > nativeQ); // up sharpened

	// DOWN, FROM WHERE THE NEXT PRESS LANDS: two octaves' pixels past the dead zone takes +3 to +1.
	const float y3 = plot->qHandleYForTest();
	plot->onMouseDown().callback(at(x1, y3));
	plot->onMouseDrag().callback(at(x1, y3 + dead + 2.0f * ppo));
	CHECK(qOffset() == doctest::Approx(1.0f).epsilon(1e-4));
	plot->onMouseUp().callback(at(x1, y3 + dead + 2.0f * ppo));
	// A LEVEL DRAG: Freq moves, Q does not.
	(void)panel->updateContent();
	const float yLevel = plot->qHandleYForTest();
	plot->onMouseDown().callback(at(x1, yLevel));
	plot->onMouseDrag().callback(at(x1 - 40.0f, yLevel));
	plot->onMouseUp().callback(at(x1 - 40.0f, yLevel));
	CHECK(qOffset() == doctest::Approx(1.0f).epsilon(1e-4));
	CHECK(freq() == doctest::Approx(std::log2(plot->xToHzForTest(x1 - 40.0f))).epsilon(1e-5));

	// THE HANDLE SPEAKS FOR THE Q OFFSET, the rest of the plot for Freq.
	(void)panel->updateContent();
	const char* onHandle = plot->tooltipAt({ plot->markerXForTest(), plot->qHandleYForTest() });
	REQUIRE(onHandle != nullptr);
	CHECK(std::string(onHandle).find("Octaves of Q") != std::string::npos);
	const char* elsewhere = plot->tooltipAt({ plot->markerXForTest() + 80.0f, plot->qHandleYForTest() + 60.0f });
	CHECK((elsewhere == nullptr || std::string(elsewhere).find("Octaves of Q") == std::string::npos));
	// AND THE WINDOW SEATS A BOX FOR IT — typed, reset and armed for routes there.
	REQUIRE(panel->qOffsetBoxForTest() != nullptr);
	const auto reached = panel->addressedParamsForTest();
	CHECK(std::find(reached.begin(), reached.end(), dspr::Param::QOffset) != reached.end());

	// THE CARD'S PLOT TAKES THE SAME VERTICAL ROAD (backlog NH,, reversing "the card stays X only"):
	// its handle rides at the Q offset's height too, and a drag up from it, past the dead zone, adds octaves.
	(void)d.card->updateContent();
	ui::DisperserPlot* small = d.card->plotForTest();
	const float before = qOffset();
	REQUIRE(before == doctest::Approx(1.0f).epsilon(1e-4));
	const float cx = small->markerXForTest();
	const float cy = small->qHandleYForTest();
	CHECK(cy == doctest::Approx(small->qOffsetToYForTest(1.0)));
	const float smallPpo = small->pixelsPerQOctaveForTest();
	small->onMouseDown().callback(at(cx, cy));
	small->onMouseDrag().callback(at(cx, cy - dead - smallPpo));
	small->onMouseUp().callback(at(cx, cy - dead - smallPpo));
	CHECK(qOffset() == doctest::Approx(2.0f).epsilon(1e-4));
}

namespace {
/// THE Q OFFSET'S PIXELS PER OCTAVE, RESTATED from the plot's geometry rather than asked of it: the curve runs from
/// the inset plus the caption row to the inset plus the foot, and that height spans the offset's six octaves.
float restatedPixelsPerQOctave(const ui::DisperserPlot& plot, bool large) {
	const float inset = large ? 8.0f : 4.0f;
	const float top = inset + (large ? 12.0f : 10.0f);
	const float foot = plot.height() - inset - 7.0f;
	return (foot - top) / static_cast<float>(2.0 * dspr::kQOffsetOctaves);
}
/// A PRESS ON THE Q HANDLE AND A DRAG UP BY `octaves` PAST THE DEAD ZONE, delivered the way visage's window
/// delivers it — hit-tested from the root — and REQUIREs the press lands on the plot itself.
void dragQHandleThroughRoot(visage::Frame* root, ui::DisperserPlot* plot, bool large, float octaves) {
	const float ppo = restatedPixelsPerQOctave(*plot, large);
	REQUIRE(plot->pixelsPerQOctaveForTest() == doctest::Approx(ppo));
	const visage::Point handle =
	    plot->positionInWindow() + visage::Point { plot->markerXForTest(), plot->qHandleYForTest() };
	REQUIRE(root->frameAtPoint(handle) == plot);
	dragThroughRoot(root, handle, { handle.x, handle.y - ui::DisperserPlot::kQDeadZone - octaves * ppo });
}
} // namespace

TEST_CASE("DISPERSER's Q handle answers a hand on the card and in the window: a drag up through the editor's hit test adds octaves of Q") {
	// Backlog NH: the Q handle in the Disperser card's plot did not respond. MEASURED FIRST: the card's
	// press reached the plot and wrote only Freq/Offset — the card had been made X only on 2026-10-03 while drawing
	// the dot the window drags for Q. Every press here goes through the root, so the handle must be REACHABLE as
	// well as wired; the value is read off the device.
	FilterFixture f;
	const size_t slot = f.bridge.addFxDevice(dspr::kTraits.tag);
	f.lane->resized();
	auto* card = dynamic_cast<ui::DisperserCard*>(f.lane->cardAt(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	const auto qOffset = [&] { return f.deviceHost.fieldAt(device, static_cast<size_t>(dspr::Param::QOffset)); };
	(void)card->updateContent();
	REQUIRE(qOffset() == 0.0f);

	SUBCASE("the card") {
		dragQHandleThroughRoot(f.root, card->plotForTest(), false, 1.25f);
		CHECK(qOffset() == doctest::Approx(1.25f).epsilon(1e-3));
		// AND THE DOT FOLLOWS: it is drawn where the new offset maps.
		(void)card->updateContent();
		CHECK(card->plotForTest()->qHandleYForTest() == doctest::Approx(card->plotForTest()->qOffsetToYForTest(1.25)));
	}
	const auto openWindow = [&](bool docked) {
		auto* overlay = f.gui.expandedForTest();
		REQUIRE(overlay != nullptr);
		if (docked) {
			REQUIRE(f.gui.canvasForTest()->setCollapsed(false));
			f.grantPreferredSize();
		}
		else {
			(void)f.gui.canvasForTest()->setCollapsed(true);
		}
		f.lane->onExpandDevice(slot);
		REQUIRE(overlay->open());
		auto* panel = presentedAs<ui::DisperserExpanded>(overlay);
		REQUIRE(panel != nullptr);
		(void)panel->updateContent();
		return panel->plotForTest();
	};
	SUBCASE("the window, on the overlay") {
		dragQHandleThroughRoot(f.root, openWindow(false), true, 1.25f);
		CHECK(qOffset() == doctest::Approx(1.25f).epsilon(1e-3));
	}
	SUBCASE("the window, docked in the canvas") {
		dragQHandleThroughRoot(f.root, openWindow(true), true, 1.25f);
		CHECK(qOffset() == doctest::Approx(1.25f).epsilon(1e-3));
	}
}

namespace {
//------------------------------------------------------------------------------------------------//
// THE PHASER/FLANGER — its card, its plot and its window (phaser plan, Task 9b)                   //
//------------------------------------------------------------------------------------------------//

namespace phr = feathers::fx::phaser;
/// THE SETTINGS RESTATED HERE from the host's STORED fields — the ops table's decode, not the plot's helper.
phr::Settings phaserStoredSettingsOf(const feathers::BridgeDeviceHost& host, DeviceInstanceId id) {
	phr::Row row {};
	const size_t present = std::min(host.fieldsPresentAt(id), row.size());
	for (size_t i = 0; i < present; ++i) {
		row[i] = host.fieldAt(id, i);
	}
	phr::Settings s = phr::stateOf(row, present).settings;
	for (size_t i = 0; i < phr::phase_space::kPhaseCount; ++i) {
		s.travel[i] = host.fieldAt(id, phr::kTravelFirst + i);
	}
	return s;
}
using PhaserReadings = std::array<float, phr::kReadingCount>;
PhaserReadings phaserReadings(float lfoPhase, float env, float barber, float noteHz) {
	PhaserReadings r {};
	r[static_cast<size_t>(phr::Reading::LfoPhase)] = lfoPhase;
	r[static_cast<size_t>(phr::Reading::Env)] = env;
	r[static_cast<size_t>(phr::Reading::BarberPhase)] = barber;
	r[static_cast<size_t>(phr::Reading::NoteHz)] = noteHz;
	return r;
}
} // namespace

TEST_CASE("PHASER's card builds and lays out at the lane width: every shown control inside the body, none overlapping, in both modes and at 1.75x") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	REQUIRE(d.card->laneHeightAt(d.card->width()) == doctest::Approx(ui::PhaserCard::openHeight()));
	const float was = d.card->dpiScale();
	for (const float factor : { 1.0f, 1.75f }) {
		CAPTURE(factor);
		d.f.root->setDpiScale(was * factor);
		d.card->resized();
		for (const float mode : { 0.0f, 1.0f }) {
			CAPTURE(mode);
			d.set(static_cast<size_t>(phr::Param::Mode), mode);
			(void)d.card->updateContent();
			std::vector<visage::Frame*> shown;
			for (visage::Frame* child : d.card->children()) {
				if (child->isVisible() && child->width() > 0.0f && child->height() > 0.0f
				    && (dynamic_cast<ui::TooltipSource*>(child) != nullptr)) {
					shown.push_back(child);
				}
			}
			REQUIRE(shown.size() >= 14);
			const float bodyRight = d.card->bodyLeft() + d.card->bodyWidth();
			for (size_t i = 0; i < shown.size(); ++i) {
				const visage::Bounds b = shown[i]->bounds();
				CHECK(b.x() >= d.card->bodyLeft() - 0.5f);
				CHECK(b.right() <= bodyRight + 0.5f);
				CHECK(b.bottom() <= d.card->laneHeightAt(d.card->width()) + 0.5f);
				// NATIVE FOLLOWS THE SCALE (CLAUDE.md's setBounds-on-a-scale rule).
				CHECK(static_cast<float>(shown[i]->nativeWidth()) == doctest::Approx(b.width() * was * factor).epsilon(0.05));
				for (size_t j = i + 1; j < shown.size(); ++j) {
					CHECK_FALSE(overlaps(b, shown[j]->bounds()));
				}
			}
			// EVERY SHOWN BOX PRINTS ITS NUMBER.
			for (const phr::Param p : { phr::Param::Feedback, phr::Param::Mix, phr::Param::Position, phr::Param::Stereo,
			                            phr::Param::Drive, phr::Param::Damping }) {
				ui::ValueBox* box = d.card->boxForTest(p);
				REQUIRE(box != nullptr);
				CHECK(box->isVisible());
				CHECK_FALSE(box->readout().empty());
			}
		}
	}
	d.f.root->setDpiScale(was);
}
TEST_CASE("PHASER's window lays out every card control at its minimum size: inside the view, none overlapping, in both modes") {
	// THE WINDOW IS A SUPERSET OF THE CARD (the tooltips suite's guard), and this is the other half: what it holds
	// FITS. At the window's least size — the tightest it is ever drawn — every shown control is inside the view and
	// clear of the pad's column, and no two overlap; the Mode's hidden boxes are hidden.
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	for (const float mode : { 0.0f, 1.0f }) {
		CAPTURE(mode);
		d.set(static_cast<size_t>(phr::Param::Mode), mode);
		(void)panel->updateContent();
		std::vector<visage::Frame*> shown;
		for (visage::Frame* child : panel->children()) {
			if (child->isVisible() && child->width() > 0.0f && child->height() > 0.0f
			    && dynamic_cast<ui::TooltipSource*>(child) != nullptr && child != panel->plotForTest()) {
				shown.push_back(child);
			}
		}
		// TWENTY-NINE EITHER WAY: the ten explore pills under the pad (round 2), and Mode, the centre box, Feedback, Mix, Sweep, Key, Rate, Sync, the two pills, the
		// follower's three, the output row's four and Rotate — and Stages in the Phaser, TZ in the Flanger.
		REQUIRE(shown.size() == 29);
		const visage::Bounds pad = panel->padForTest()->bounds();
		for (size_t i = 0; i < shown.size(); ++i) {
			const visage::Bounds b = shown[i]->bounds();
			CHECK(b.x() >= 0.0f);
			CHECK(b.y() >= 0.0f);
			CHECK(b.right() <= min.x + 0.5f);
			CHECK(b.bottom() <= min.y + 0.5f);
			// ROTATE SITS IN THE PAD'S CORNER BY DESIGN; everything else is clear of the pad and of the plot.
			if (shown[i] != panel->rotateForTest()) {
				CHECK_FALSE(overlaps(b, pad));
			}
			CHECK_FALSE(overlaps(b, panel->plotForTest()->bounds()));
			for (size_t j = i + 1; j < shown.size(); ++j) {
				CHECK_FALSE(overlaps(b, shown[j]->bounds()));
			}
		}
		const bool phaser = mode == 0.0f;
		CHECK(panel->boxForTest(phr::Param::CenterHz)->isVisible() == phaser);
		CHECK(panel->boxForTest(phr::Param::DelayMs)->isVisible() == !phaser);
		CHECK(panel->boxForTest(phr::Param::Stages)->isVisible() == phaser);
		CHECK(panel->throughZeroForTest()->isVisible() == !phaser);
		CHECK(panel->plotForTest()->height() >= ui::PhaserExpanded::kMinPlotHeight - 0.5f);
	}
}
TEST_CASE("PHASER's Mode shows Stages and Center in the Phaser, Delay and TZ in the Flanger") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	d.set(static_cast<size_t>(phr::Param::Mode), 0.0f);
	(void)d.card->updateContent();
	CHECK(d.card->centerBoxForTest()->isVisible());
	CHECK_FALSE(d.card->delayBoxForTest()->isVisible());
	CHECK(d.card->stagesBoxForTest()->isVisible());
	CHECK_FALSE(d.card->throughZeroForTest()->isVisible());
	CHECK(d.card->plotForTest()->draggedControlForTest() == static_cast<size_t>(phr::Param::CenterHz));
	// THE FLANGER, by automation rather than the hand: the card re-flows on its next update.
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	CHECK(d.card->updateContent());
	CHECK_FALSE(d.card->centerBoxForTest()->isVisible());
	CHECK(d.card->delayBoxForTest()->isVisible());
	CHECK_FALSE(d.card->stagesBoxForTest()->isVisible());
	CHECK(d.card->throughZeroForTest()->isVisible());
	CHECK(d.card->plotForTest()->draggedControlForTest() == static_cast<size_t>(phr::Param::DelayMs));
	// THE ROW RE-FLOWS: three cells take the width four did, so Delay is wider than Center was.
	const float delayW = d.card->delayBoxForTest()->width();
	d.set(static_cast<size_t>(phr::Param::Mode), 0.0f);
	(void)d.card->updateContent();
	CHECK(delayW > d.card->centerBoxForTest()->width() * 1.2f);
	// AND THE MODE IS A SEGMENTED CONTROL: both choices on the face.
	CHECK(d.card->modeForTest()->segmentAt({ 1.0f, 1.0f }) == 0);
	CHECK(d.card->modeForTest()->segmentAt({ d.card->modeForTest()->width() - 1.0f, 1.0f }) == 1);
}
TEST_CASE("PHASER's Rate is disabled while the LFO is off, as well as under a division; Sweep and Stereo while it is off") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	ui::ValueBox* rate = panel->boxForTest(phr::Param::RateHz);
	ui::ValueBox* sync = panel->boxForTest(phr::Param::Sync);
	REQUIRE(rate != nullptr);
	REQUIRE(sync != nullptr);
	d.set(static_cast<size_t>(phr::Param::Sync), 0.0f);
	d.set(static_cast<size_t>(phr::Param::LfoOn), 1.0f);
	(void)panel->updateContent();
	(void)d.card->updateContent();
	CHECK(rate->enabled());
	CHECK(sync->enabled());
	CHECK(d.card->sweepForTest()->enabled());
	CHECK(d.card->boxForTest(phr::Param::Stereo)->enabled());
	// THE LFO OFF: Rate's one gate (Sync) is still open, and the card's own gate closes it.
	d.set(static_cast<size_t>(phr::Param::LfoOn), 0.0f);
	CHECK(panel->updateContent());
	(void)d.card->updateContent();
	CHECK_FALSE(rate->enabled());
	CHECK_FALSE(sync->enabled());
	// SWEEP STAYS LIVE (by design): with the LFO off the barberpole still differs from Normal once the voices
	// differ — the Fb tilt's per-voice loops, the flanger's per-tap spread (`test_fx_phaser`'s "sweep is heard with the
	// LFO off" case renders it) — so it has no gate.
	CHECK(d.card->sweepForTest()->enabled());
	CHECK(panel->sweepForTest()->enabled());
	CHECK_FALSE(d.card->boxForTest(phr::Param::Stereo)->enabled());
	// ON AGAIN, UNDER A DIVISION: the descriptor's gate.
	d.set(static_cast<size_t>(phr::Param::LfoOn), 1.0f);
	d.set(static_cast<size_t>(phr::Param::Sync), 5.0f);
	(void)panel->updateContent();
	CHECK_FALSE(rate->enabled());
	CHECK(sync->enabled());
	// THE LFO PILL CARRIES WHAT THE LFO RUNS AT — "LFO" left, and at its right end the division now, the rate when free.
	(void)d.card->updateContent();
	CHECK(d.card->lfoPillForTest()->caption() == "LFO");
	CHECK(d.card->lfoPillForTest()->readout() == phr::kSyncNames[5]);
	d.set(static_cast<size_t>(phr::Param::Sync), 0.0f);
	(void)d.card->updateContent();
	CHECK(d.card->lfoPillForTest()->readout() == "0.30 Hz"); // the default rate, through `kTextOps`
	// THE FOLLOWER'S SHAPE WITH ITS POWER.
	d.set(static_cast<size_t>(phr::Param::EnvOn), 0.0f);
	(void)panel->updateContent();
	CHECK_FALSE(panel->boxForTest(phr::Param::EnvAttack)->enabled());
	d.set(static_cast<size_t>(phr::Param::EnvOn), 1.0f);
	(void)panel->updateContent();
	CHECK(panel->boxForTest(phr::Param::EnvAttack)->enabled());
}
TEST_CASE("PHASER's Center stays live while a note is tracked, prints the offset from it, and the plot's axis moves the tracked notch") {
	// ONE KNOB (retiring Key Offset): while a note is tracked — the published NoteHz, not the Key
	// switch — Center (or Delay) is the offset from the note, so it keeps the hand, and the boxes print semitones.
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	d.set(static_cast<size_t>(phr::Param::KeyTrack), 1.0f);
	d.publish(phaserReadings(0.0f, 0.0f, 0.0f, 0.0f));
	(void)d.card->updateContent();
	(void)panel->updateContent();
	// KEY ON, NO NOTE YET: the knob's own Hz, on the card and in the window.
	ui::PhaserPlot* plot = d.card->plotForTest();
	CHECK_FALSE(plot->trackingForTest());
	CHECK(d.card->centerBoxForTest()->enabled());
	CHECK(d.card->centerBoxForTest()->readout() == "800Hz");
	CHECK(panel->boxForTest(phr::Param::CenterHz)->readout() == "800Hz");
	// A NOTE: still live, and the readout is the offset — "+0" at the default, which is "on the note".
	d.publish(phaserReadings(0.0f, 0.0f, 0.0f, 440.0f));
	CHECK(d.card->updateContent());
	CHECK(panel->updateContent());
	CHECK(plot->trackingForTest());
	CHECK(d.card->centerBoxForTest()->enabled());
	CHECK(d.card->centerBoxForTest()->readout() == "0.0 st");
	CHECK(panel->boxForTest(phr::Param::CenterHz)->enabled());
	CHECK(panel->boxForTest(phr::Param::CenterHz)->readout() == "0.0 st");
	// CENTER AT 1600 Hz READS +12 st — restated as 12·log2(1600/800).
	d.set(static_cast<size_t>(phr::Param::CenterHz), static_cast<float>(std::log2(1600.0)));
	CHECK(d.card->centerBoxForTest()->readout() == "+12.0 st");
	d.set(static_cast<size_t>(phr::Param::CenterHz), static_cast<float>(phr::kCenterKeyRefLog2));
	// THE AXIS TAKES THE HAND: a drag right raises the knob by the axis's octaves, and the handle — the tracked note's
	// corner, moved by the knob's offset — follows it.
	const double sr = d.f.deviceHost.sampleRate() > 0.0 ? d.f.deviceHost.sampleRate() : 48000.0;
	const auto handleWant = [&] {
		const phr::Targets t = phr::targetsOf(phaserStoredSettingsOf(d.f.deviceHost, d.device), 440.0, sr);
		return plot->hzToXForTest(phr::phaserCornerHz(t.centerLog2, 0.0, sr));
	};
	(void)plot->update();
	CHECK(plot->handleXForTest() == doctest::Approx(handleWant()));
	const float before = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::CenterHz));
	const float x = plot->handleXForTest();
	plot->onMouseDown().callback(at(x, plot->height() * 0.5f));
	plot->onMouseDrag().callback(at(x + 20.0f, plot->height() * 0.5f));
	plot->onMouseUp().callback(at(x + 20.0f, plot->height() * 0.5f));
	const float after = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::CenterHz));
	CHECK(after > before);
	(void)plot->update();
	CHECK(plot->handleXForTest() == doctest::Approx(handleWant()));
	CHECK(plot->handleXForTest() > x + 5.0f);
	// THE FLANGER'S DELAY THE SAME: live, and semitones.
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	(void)d.card->updateContent();
	CHECK(d.card->delayBoxForTest()->enabled());
	CHECK(d.card->delayBoxForTest()->readout() == "0.0 st");
	// THE NOTE GOES (Key Track off publishes 0): the knob's own ms again.
	d.publish(phaserReadings(0.0f, 0.0f, 0.0f, 0.0f));
	CHECK(d.card->updateContent());
	CHECK(d.card->delayBoxForTest()->readout() == "2.5ms");
}
TEST_CASE("PHASER's plot draws the response model at every pixel, left and right, from the readings and the settings") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	d.set(static_cast<size_t>(phr::Param::Feedback), 0.6f);
	d.set(static_cast<size_t>(phr::Param::Stages), 3.0f); // "8"
	d.set(static_cast<size_t>(phr::Param::Stereo), 120.0f);
	d.f.deviceHost.setField(d.device, phr::kTravelFirst + 0, 0.21f); // a little travel: a spread first stage
	d.f.bridge.refresh();
	const PhaserReadings r = phaserReadings(0.3f, 0.0f, 0.0f, 0.0f);
	d.publish(r);
	(void)d.card->updateContent();
	ui::PhaserPlot* plot = d.card->plotForTest();
	// ONE POINT PER PIXEL OF THE CURVE.
	CHECK(static_cast<float>(plot->pointCountForTest()) >= plot->width() - 2.0f * 4.0f);
	CHECK(static_cast<float>(plot->pointCountForTest()) <= plot->width() - 2.0f * 4.0f + 2.0f);
	const double sr = d.f.deviceHost.sampleRate() > 0.0 ? d.f.deviceHost.sampleRate() : 48000.0;
	const phr::Settings s = phaserStoredSettingsOf(d.f.deviceHost, d.device);
	double worstNotch = 0.0;
	double lrApart = 0.0;
	for (int ch = 0; ch < 2; ++ch) {
		const phr::ResponseModel m = phr::responseModelOf(s, phr::responseInputsOf(r.data(), r.size(), ch), sr);
		for (size_t i = 0; i < plot->pointCountForTest(); ++i) {
			const float want = phr::magnitudeDb(m, plot->hzAtForTest(i));
			CAPTURE(ch);
			CAPTURE(i);
			CHECK(plot->dbAtForTest(i, ch) == want);
			worstNotch = std::min(worstNotch, static_cast<double>(want));
		}
	}
	for (size_t i = 0; i < plot->pointCountForTest(); ++i) {
		lrApart = std::max(lrApart, std::fabs(static_cast<double>(plot->dbAtForTest(i, 0) - plot->dbAtForTest(i, 1))));
	}
	// NON-VACUITY: there are notches, and L and R differ (the LFO runs Stereo apart).
	CHECK(worstNotch < -12.0);
	CHECK(lrApart > 1.0);
	// THE HZ ARE LOG-SPACED OVER THE AXIS, end to end.
	CHECK(plot->hzAtForTest(0) == doctest::Approx(ui::PhaserPlot::kMinHz));
	CHECK(plot->hzAtForTest(plot->pointCountForTest() - 1) == doctest::Approx(ui::PhaserPlot::kMaxHz));
}
TEST_CASE("PHASER's plot does zero work at rest, and recomputes once when the LFO's published phase moves") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	d.publish(phaserReadings(0.25f, 0.0f, 0.0f, 0.0f));
	(void)d.card->updateContent();
	ui::PhaserPlot* plot = d.card->plotForTest();
	const int at = plot->recomputesForTest();
	for (int i = 0; i < 20; ++i) {
		CHECK_FALSE(plot->update());
	}
	// THE SAME READINGS PUBLISHED AGAIN (an engine at rest still publishes) are not a change.
	d.publish(phaserReadings(0.25f, 0.0f, 0.0f, 0.0f));
	CHECK_FALSE(plot->update());
	CHECK(plot->recomputesForTest() == at);
	// THE LFO MOVES: exactly one recompute.
	d.publish(phaserReadings(0.26f, 0.0f, 0.0f, 0.0f));
	CHECK(plot->update());
	CHECK_FALSE(plot->update());
	CHECK(plot->recomputesForTest() == at + 1);
	// THE PLOT'S MOTION IS THE PLOT'S FRAME: the card itself is not asked to repaint for it.
	{
		RedrawProbe probe(d.card);
		d.publish(phaserReadings(0.27f, 0.0f, 0.0f, 0.0f));
		CHECK(d.card->updateContent());
		CHECK(probe.count == 0);
	}

	// **SOURCES OFF, THE ENGINE STILL RUNNING** (the Task 9b review): the LFO's phase and the follower go on advancing
	// in the readings, and the sweep, corners and delays are republished, with both sources switched off — the setup
	// where an external modulator drives Position. Nothing the model reads moves, so nothing is recomputed.
	d.set(static_cast<size_t>(phr::Param::LfoOn), 0.0f);
	d.set(static_cast<size_t>(phr::Param::EnvOn), 0.0f);
	(void)plot->update();
	const int off = plot->recomputesForTest();
	for (int i = 0; i < 20; ++i) {
		PhaserReadings r = phaserReadings(0.05f * static_cast<float>(i), 0.03f * static_cast<float>(i), 0.0f, 0.0f);
		r[static_cast<size_t>(phr::Reading::SweepL)] = 0.01f * static_cast<float>(i);
		r[static_cast<size_t>(phr::Reading::CornerHzL)] = 800.0f + static_cast<float>(i);
		r[static_cast<size_t>(phr::Reading::DelayMsL)] = 2.5f + 0.01f * static_cast<float>(i);
		d.publish(r);
		CHECK_FALSE(plot->update());
	}
	CHECK(plot->recomputesForTest() == off);
	// NON-VACUITY: the same advancing phase with the LFO back on IS a recompute.
	d.set(static_cast<size_t>(phr::Param::LfoOn), 1.0f);
	(void)plot->update();
	const int on = plot->recomputesForTest();
	d.publish(phaserReadings(0.9f, 0.0f, 0.0f, 0.0f));
	CHECK(plot->update());
	CHECK(plot->recomputesForTest() == on + 1);
}
TEST_CASE("PHASER's plot: a drag writes Center (Phaser) or Delay (Flanger) through one gesture, and the Position tick follows the MODULATED Position") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	d.set(static_cast<size_t>(phr::Param::KeyTrack), 0.0f);
	(void)d.card->updateContent();
	ui::PhaserPlot* plot = d.card->plotForTest();
	const float y = plot->height() * 0.5f;
	// RELATIVE TO THE PRESS: the hand's travel in the axis's octaves, added to the value the press found — so a press
	// anywhere writes nothing, and the handle moves exactly as far as the hand.
	const float x0 = plot->hzToXForTest(500.0);
	const double c0 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::CenterHz));
	(void)d.card->updateContent();
	const float h0 = plot->handleXForTest();
	plot->onMouseDown().callback(at(x0, y));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::CenterHz)) == static_cast<float>(c0));
	for (int step = 1; step <= 6; ++step) {
		const float x = x0 + static_cast<float>(step) * 5.0f;
		plot->onMouseDrag().callback(at(x, y));
		// THE AXIS'S OCTAVES, restated: log2 of the hertz ratio between the two x positions.
		CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::CenterHz))
		      == doctest::Approx(c0 + std::log2(plot->xToHzForTest(x) / plot->xToHzForTest(x0))).epsilon(1e-5));
	}
	plot->onMouseUp().callback(at(x0 + 30.0f, y));
	(void)d.card->updateContent();
	CHECK(plot->handleXForTest() == doctest::Approx(h0 + 30.0f).epsilon(1e-3));
	// THE FLANGER: the axis is 1/Delay, so right is shorter.
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	(void)d.card->updateContent();
	const float xf = plot->hzToXForTest(400.0);
	const double d0 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::DelayMs));
	const float hf = plot->handleXForTest();
	plot->onMouseDown().callback(at(xf, y));
	plot->onMouseDrag().callback(at(xf + 20.0f, y));
	plot->onMouseUp().callback(at(xf + 20.0f, y));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::DelayMs))
	      == doctest::Approx(d0 - std::log2(plot->xToHzForTest(xf + 20.0f) / plot->xToHzForTest(xf))).epsilon(1e-5));
	(void)d.card->updateContent();
	CHECK(plot->handleXForTest() == doctest::Approx(hf + 20.0f).epsilon(1e-3));

	// THE POSITION TICK READS WHERE MODULATION LEFT POSITION, not the stored value: an external modulator's road.
	d.set(static_cast<size_t>(phr::Param::Mode), 0.0f);
	d.set(static_cast<size_t>(phr::Param::Position), 0.0f);
	(void)d.card->updateContent();
	const float rest = plot->positionXForTest();
	CHECK(rest == doctest::Approx(plot->handleXForTest()));
	feathers::FxChain moved = d.f.bridge.fxChain();
	moved.device[d.slot].row.field[static_cast<size_t>(phr::Param::Position)] = 0.5f;
	d.f.bridge.publishModulated(d.f.bridge.modRack(), moved, feathers::RackPlayheads {},
	                            feathers::mod::TransportClock::unknown());
	REQUIRE(d.f.deviceHost.modulatedFieldAt(d.device, static_cast<size_t>(phr::Param::Position)) == 0.5f);
	CHECK(plot->update());
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::Position)) == 0.0f);
	const double sr = d.f.deviceHost.sampleRate() > 0.0 ? d.f.deviceHost.sampleRate() : 48000.0;
	const phr::Settings s = phaserStoredSettingsOf(d.f.deviceHost, d.device);
	CHECK(plot->positionXForTest()
	      == doctest::Approx(plot->hzToXForTest(phr::phaserCornerHz(s.centerLog2Hz, 0.5, sr))).epsilon(1e-4));
	CHECK(plot->positionXForTest() > rest + 5.0f);
}
TEST_CASE("PHASER's plot drags in x AND y in every mode: Center or Delay across, Feedback up, relative, one gesture a control") {
	// A drag on the response plot acts in both x and y, in every mode. x is the
	// centre control (Center, or Delay in the Flanger — tracked or not), y is Feedback, both relative to the press. The
	// expected values are restated from the plot's two axes as it DRAWS them: the hertz between two x positions, and
	// the pixels between the ring's +1 and −1.
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::PhaserPlot* plot = d.card->plotForTest();
	ui::EditHistory history;
	plot->setModulation(nullptr, &history);
	const auto field = [&](phr::Param p) { return static_cast<double>(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(p))); };
	struct Arrangement {
		float mode;
		float sweep;
		float tz;
		float noteHz;
	};
	const Arrangement arrangements[] = {
		{ 0.0f, 0.0f, 0.0f, 0.0f },   { 1.0f, 0.0f, 0.0f, 0.0f },   { 0.0f, 1.0f, 0.0f, 0.0f },
		{ 1.0f, 2.0f, 1.0f, 0.0f },   { 0.0f, 0.0f, 0.0f, 440.0f }, { 1.0f, 0.0f, 0.0f, 440.0f },
	};
	for (const Arrangement& a : arrangements) {
		CAPTURE(a.mode);
		CAPTURE(a.sweep);
		CAPTURE(a.tz);
		CAPTURE(a.noteHz);
		d.set(static_cast<size_t>(phr::Param::Mode), a.mode);
		d.set(static_cast<size_t>(phr::Param::Sweep), a.sweep);
		d.set(static_cast<size_t>(phr::Param::ThroughZero), a.tz);
		d.set(static_cast<size_t>(phr::Param::KeyTrack), a.noteHz > 0.0f ? 1.0f : 0.0f);
		d.set(static_cast<size_t>(phr::Param::Feedback), 0.1f);
		d.publish(phaserReadings(0.0f, 0.0f, 0.0f, a.noteHz));
		(void)d.card->updateContent();
		(void)plot->update();
		const bool flanger = a.mode == 1.0f;
		const phr::Param centre = flanger ? phr::Param::DelayMs : phr::Param::CenterHz;
		REQUIRE(plot->draggedControlForTest() == static_cast<size_t>(centre));
		const float x0 = plot->hzToXForTest(500.0);
		const float y0 = plot->height() * 0.5f;
		const double c0 = field(centre);
		const double fb0 = field(phr::Param::Feedback);
		const size_t steps0 = history.length();

		// A PRESS WITH NO MOTION WRITES NOTHING, and records nothing.
		plot->onMouseDown().callback(at(x0 + 7.0f, y0 - 9.0f));
		plot->onMouseUp().callback(at(x0 + 7.0f, y0 - 9.0f));
		CHECK(field(centre) == c0);
		CHECK(field(phr::Param::Feedback) == fb0);
		CHECK(history.length() == steps0);

		// RIGHT AND UP: the centre control by the axis's octaves (Delay the other way), Feedback by the ring's scale.
		plot->onMouseDown().callback(at(x0, y0));
		plot->onMouseDrag().callback(at(x0 + 15.0f, y0 - 6.0f));
		plot->onMouseDrag().callback(at(x0 + 30.0f, y0 - 12.0f));
		plot->onMouseUp().callback(at(x0 + 30.0f, y0 - 12.0f));
		const double octaves = std::log2(plot->xToHzForTest(x0 + 30.0f) / plot->xToHzForTest(x0));
		CHECK(field(centre) == doctest::Approx(flanger ? c0 - octaves : c0 + octaves).epsilon(1e-5));
		// A FIXED SPAN, the full ±1 over 200 px on card and window alike, counted past the 3 px dead zone: 12 − 3 = 9.
		const double unitsPerPixel = 2.0 / 200.0;
		REQUIRE(plot->feedbackPerPixelForTest() == unitsPerPixel);
		const double past = 12.0 - static_cast<double>(ui::PhaserPlot::kFeedbackDeadZone);
		CHECK(field(phr::Param::Feedback) == doctest::Approx(fb0 + past * unitsPerPixel).epsilon(1e-5));
		CHECK(field(phr::Param::Feedback) > fb0);
		if (flanger) {
			CHECK(field(centre) < c0);
		}
		else {
			CHECK(field(centre) > c0);
		}
		// ONE GESTURE A CONTROL: exactly one undo step for each of the two (a second Begin or End would re-anchor or
		// record twice).
		CHECK(history.length() == steps0 + 2);
		// THE RING SITS AT THE NEW FEEDBACK.
		(void)plot->update();
		CHECK(plot->feedbackDotYForTest() == doctest::Approx(plot->feedbackToYForTest(field(phr::Param::Feedback))));

		// SHIFT IS THE HOUSE FINE FACTOR, on both axes.
		const double c1 = field(centre);
		const double fb1 = field(phr::Param::Feedback);
		visage::MouseEvent press = at(x0, y0);
		press.modifiers = visage::kModifierShift;
		plot->onMouseDown().callback(press);
		visage::MouseEvent move = at(x0 + 30.0f, y0 + 12.0f);
		move.modifiers = visage::kModifierShift;
		plot->onMouseDrag().callback(move);
		plot->onMouseUp().callback(move);
		const double fineOct = octaves * static_cast<double>(ui::Knob::kFineFactor);
		CHECK(field(centre) == doctest::Approx(flanger ? c1 - fineOct : c1 + fineOct).epsilon(1e-5));
		CHECK(field(phr::Param::Feedback)
		      == doctest::Approx(fb1 - past * unitsPerPixel * static_cast<double>(ui::Knob::kFineFactor)).epsilon(1e-5));
		d.set(static_cast<size_t>(centre), static_cast<float>(c0));
	}
	// THE PLOT SAYS WHAT BOTH AXES DO.
	d.set(static_cast<size_t>(phr::Param::Mode), 0.0f);
	CHECK(std::string(plot->tooltipAt({ 10.0f, 10.0f })) == ui::PhaserPlot::kPhaserDragTip);
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	CHECK(std::string(plot->tooltipAt({ 10.0f, 10.0f })) == ui::PhaserPlot::kFlangerDragTip);
}
TEST_CASE("PHASER's plot opens a control's gesture only when that axis moves: a click touches nothing, a sideways drag never Feedback") {
	// THE HOST'S GESTURE IS A PROMISE TO TOUCH AUTOMATION: a Begin on Feedback while the host records in touch mode
	// flattens Feedback's lane for as long as the hand is down. So each axis opens its gesture on its first real change
	// (`Knob::commit`), y only past a dead zone (`DisperserPlot`'s Q road). OBSERVED AT THE HOST: the plot is built on
	// a forwarding host that records every touch, Begin and End by control (`RecordingDeviceHost::hands`).
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	RecordingDeviceHost spy { d.f.deviceHost };
	ui::PhaserPlot owned { spy, d.device, false };
	owned.setBounds(0.0f, 0.0f, 300.0f, 104.0f);
	ui::PhaserPlot* plot = &owned;
	ui::EditHistory history;
	plot->setModulation(nullptr, &history);
	// TOUCHED = the host heard a touch AND a Begin; each Begin is matched by one End.
	const auto slotted = [&](phr::Param p) {
		const auto c = static_cast<size_t>(p);
		CHECK(spy.handsOn(c, 'b') == spy.handsOn(c, 'e'));
		return spy.handsOn(c, 't') > 0 || spy.handsOn(c, 'b') > 0;
	};
	const float x0 = plot->hzToXForTest(500.0);
	const float y0 = plot->height() * 0.5f;
	const float c0 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::CenterHz));
	const float fb0 = d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::Feedback));
	// A CLICK: nothing written, nothing touched, nothing recorded.
	plot->onMouseDown().callback(at(x0, y0));
	plot->onMouseUp().callback(at(x0, y0));
	CHECK_FALSE(slotted(phr::Param::CenterHz));
	CHECK_FALSE(slotted(phr::Param::Feedback));
	CHECK(history.length() == 0);
	// SIDEWAYS, with a wobble inside the dead zone: Center moves and is touched; Feedback is neither.
	plot->onMouseDown().callback(at(x0, y0));
	plot->onMouseDrag().callback(at(x0 + 10.0f, y0 + 2.0f));
	plot->onMouseDrag().callback(at(x0 + 25.0f, y0 - 2.5f));
	plot->onMouseUp().callback(at(x0 + 25.0f, y0 - 2.5f));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::CenterHz)) != c0);
	CHECK(slotted(phr::Param::CenterHz));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::Feedback)) == fb0);
	CHECK_FALSE(slotted(phr::Param::Feedback));
	CHECK(history.length() == 1);
	// AND STRAIGHT UP: Feedback now, and no second step for Center (it did not move).
	plot->onMouseDown().callback(at(x0, y0));
	plot->onMouseDrag().callback(at(x0, y0 - 20.0f));
	plot->onMouseUp().callback(at(x0, y0 - 20.0f));
	CHECK(slotted(phr::Param::Feedback));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::Feedback)) > fb0);
	CHECK(history.length() == 2);
}
TEST_CASE("PHASER's plot tooltip names both axes, by mode") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::PhaserPlot* plot = d.card->plotForTest();
	d.set(static_cast<size_t>(phr::Param::Mode), 0.0f);
	CHECK(std::string(plot->tooltipAt({ 10.0f, 10.0f })) == ui::PhaserPlot::kPhaserDragTip);
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	CHECK(std::string(plot->tooltipAt({ 10.0f, 10.0f })) == ui::PhaserPlot::kFlangerDragTip);
}
TEST_CASE("PHASER's window: the pad writes the folded travel at kTravelFirst + i, and touches no param") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 760.0f, 460.0f);
	ui::PhaserPad* pad = panel->padForTest();
	REQUIRE(pad != nullptr);
	visage::Canvas canvas;
	pad->draw(canvas);
	CHECK(pad->builtSizeForTest() == std::pair { static_cast<int>(pad->width()), static_cast<int>(pad->height()) });
	// ROTATE IS IN THE PAD'S CORNER, over it.
	CHECK(pad->bounds().contains(panel->rotateForTest()->bounds().topLeft()));
	std::array<float, phr::kParamCount> params {};
	for (size_t i = 0; i < params.size(); ++i) {
		params[i] = d.f.deviceHost.fieldAt(d.device, i);
	}
	const visage::Point c { pad->width() * 0.5f, pad->height() * 0.5f };
	pad->onMouseDown().callback(at(c.x, c.y));
	pad->onMouseDrag().callback(at(c.x + 50.0f, c.y - 30.0f));
	pad->onMouseUp().callback(at(c.x + 50.0f, c.y - 30.0f));
	size_t movedPhases = 0;
	for (size_t i = 0; i < phr::phase_space::kPhaseCount; ++i) {
		const float v = d.f.deviceHost.fieldAt(d.device, phr::kTravelFirst + i);
		CHECK(std::isfinite(v));
		movedPhases += v != 0.0f ? 1 : 0;
	}
	CHECK(movedPhases > phr::phase_space::kPhaseCount / 2);
	for (size_t i = 0; i < params.size(); ++i) {
		CAPTURE(i);
		CHECK(d.f.deviceHost.fieldAt(d.device, i) == params[i]);
	}
	// AND THE TRAVEL REACHES THE PICTURE: the plot's model is built from it.
	(void)panel->updateContent();
	// The baseline is THIS row with the travel zeroed (the review: `Settings{}` differed from the row in more than travel,
	// so a plot that ignored the travel could still pass).
	phr::Settings untravelled = phaserStoredSettingsOf(d.f.deviceHost, d.device);
	REQUIRE(untravelled.travel != decltype(untravelled.travel) {});
	untravelled.travel = {};
	const double sr = d.f.deviceHost.sampleRate() > 0.0 ? d.f.deviceHost.sampleRate() : 48000.0;
	const phr::ResponseModel home = phr::responseModelOf(untravelled, {}, sr);
	const phr::ResponseModel& drawn = panel->plotForTest()->modelForTest(0);
	bool anyStageMoved = false;
	for (size_t k = 0; k < phr::kMaxStages; ++k) {
		anyStageMoved = anyStageMoved || drawn.G[0][k] != home.G[0][k];
	}
	CHECK(anyStageMoved);
}
TEST_CASE("PHASER's pad terrain rebuilds on Stages, Depth and every pill, and not while the LFO runs or its Rate moves") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 760.0f, 460.0f);
	ui::PhaserPad* pad = panel->padForTest();
	visage::Canvas canvas;
	const auto t0 = std::chrono::steady_clock::now();
	pad->draw(canvas); // the first build at the pad's own size
	const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	MESSAGE("phaser terrain build (" << ui::PhaserPadPolicy::kGrid << "² cells, " << ui::PhaserPadPolicy::kSummaryPoints
	                                 << " frequencies, " << ui::PhaserPadPolicy::kSweepSamples
	                                 << " instants, Phaser 6 stages): " << ms << " ms");
	const size_t built = pad->rebuildsForTest();
	// THE LFO RUNS: its phase, the follower and the ramp move in the readings; and its speed and the follower move.
	for (int i = 0; i < 8; ++i) {
		d.publish(phaserReadings(0.1f * static_cast<float>(i), 0.05f * static_cast<float>(i), 0.1f, 0.0f));
		(void)panel->updateContent();
		pad->draw(canvas);
	}
	d.set(static_cast<size_t>(phr::Param::RateHz), 1.0f);
	d.set(static_cast<size_t>(phr::Param::Stereo), 30.0f);
	d.set(static_cast<size_t>(phr::Param::EnvAttack), -3.0f); // the follower's shape: how the input moves it, not how far
	d.set(static_cast<size_t>(phr::Param::EnvSens), 6.0f);
	pad->draw(canvas);
	CHECK(pad->rebuildsForTest() == built);
	// THE TERRAIN IS THE DISTANCE FROM THE TEXTBOOK RESPONSE: dark at the centre, where the pad at rest IS home (zero
	// travel, so exactly zero distance at the frame's origin, where the odd grid puts a cell centre), and not flat
	// elsewhere.
	const auto& cells = pad->mapForTest();
	const auto [lo, hi] = std::minmax_element(cells.begin(), cells.end());
	MESSAGE("terrain cells lo " << *lo << " hi " << *hi);
	CHECK(*hi - *lo > 0.2f);
	constexpr size_t kW = ui::PhaserPad::kMapWide;
	constexpr size_t kH = ui::PhaserPad::kMapHigh;
	const float centre = 0.25f * (cells[(kH / 2 - 1) * kW + kW / 2 - 1] + cells[(kH / 2 - 1) * kW + kW / 2]
	                              + cells[(kH / 2) * kW + kW / 2 - 1] + cells[(kH / 2) * kW + kW / 2]);
	CAPTURE(centre);
	// AT ×1 HOME IS DARK BUT NOT A POOL: the cell on it is exactly 0 dB, and averaged over the sweep's instants with all
	// thirty-four phases live the distance climbs most of its plateau within about a tenth of a plane unit — under one
	// coarse cell (0.16 units at ×1) — so the bilinear map has no room to draw the pool, and its four centre pixels (half
	// a map cell off the grid's centre) read low, not lowest. ZOOMED IN, it resolves.
	CHECK(centre < 0.3f * *hi);
	{
		pad->setZoom(8.0);
		pad->draw(canvas);
		const auto& zoomed = pad->mapForTest();
		const auto [zlo, zhi] = std::minmax_element(zoomed.begin(), zoomed.end());
		const float zc = zoomed[(kH / 2) * kW + kW / 2];
		CAPTURE(zc);
		CAPTURE(*zhi);
		CHECK(*zhi - *zlo > 0.5f);
		CHECK(zc < 0.25f * *zhi);
		pad->setZoom(1.0);
		pad->draw(canvas);
	}
	// THE THROTTLE'S CLOCK IS PINNED and stepped a whole second per change: each change below is a separate edit,
	// not a drag, so none may fall inside `kKeyRebuildInterval` of the last (the drag is the case above).
	auto clock = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	size_t expected = pad->rebuildsForTest(); // the zoom above rebuilt twice
	const auto edit = [&](phr::Param p, float v) {
		clock += std::chrono::seconds(1);
		pad->setClockForTest(clock);
		const auto before = pad->mapForTest();
		d.set(static_cast<size_t>(p), v);
		pad->draw(canvas);
		++expected;
		CAPTURE(static_cast<int>(p));
		CHECK(pad->rebuildsForTest() == expected);
		return before != pad->mapForTest();
	};
	// STAGES MOVES THE MODEL: one rebuild, and a different picture.
	CHECK(edit(phr::Param::Stages, 5.0f)); // "12"
	CHECK(pad->builtForTest().settings.stages == 12);
	// DEPTH SETS WHERE THE INSTANTS FALL (round 2's averaging), so it is a terrain input now.
	CHECK(edit(phr::Param::LfoDepth, 0.9f));
	// AND THE FOLLOWER'S REACH (the review): Env On joins the instants.
	CHECK(edit(phr::Param::EnvOn, 1.0f));
	CHECK(pad->builtForTest().settings.envOn);
	CHECK(pad->builtForTest().settings.lfoDepth == 0.9f);
	// **EVERY PILL'S KNOB AND POWER IS IN THE KEY** (the terrain shows what the pills allow): each moves it once.
	for (size_t g = 0; g < phr::phase_space::kGroupCount; ++g) {
		CAPTURE(g);
		const auto group = static_cast<phr::phase_space::Group>(g);
		(void)edit(phr::pillKnobParam(group), 0.8f);
		CHECK(pad->builtForTest().settings.pillKnob[g] == 0.8f);
		(void)edit(phr::pillPowerParam(group), 0.0f);
		CHECK_FALSE(pad->builtForTest().settings.pillOn[g]);
	}
	// AND THE PICTURE FOLLOWS THEM: with every pill off the whole plane is home — every cell at zero distance.
	{
		const auto& off = pad->mapForTest();
		const auto [a, b] = std::minmax_element(off.begin(), off.end());
		CHECK(*a == 0.0f);
		CHECK(*b == 0.0f);
	}
	// AND A KNOB AWAY FROM 0.5 MOVES THE SOUND AT HOME TOO (the house law: the field pulled toward 1 wherever the puck
	// is), so with Spread back on at 80% EVERY cell is off the textbook — the puck's included — not only where it travels.
	CHECK(edit(phr::pillPowerParam(phr::phase_space::Group::Spread), 1.0f));
	{
		const auto& lit = pad->mapForTest();
		CHECK(*std::min_element(lit.begin(), lit.end()) > 0.0f);
	}
	// AND FEEDBACK, AND THE MODE.
	(void)edit(phr::Param::Feedback, -0.7f);
	const auto t1 = std::chrono::steady_clock::now();
	(void)edit(phr::Param::Mode, 1.0f);
	const double msFlanger = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
	MESSAGE("phaser terrain build (Flanger): " << msFlanger << " ms");
}
TEST_CASE("PHASER's pad terrain is not blank in the Flanger under through-zero at Position 0") {
	// THE DEFECT ROUND 2 FIXED (seen in a capture): the first painter took ONE frozen instant, s = Position, and in the
	// Flanger under through-zero at Position 0 that instant is the crossing — every tap's relative delay is 0 whatever
	// the travel does to its span — so every cell was the same and the map was one flat colour. Averaged over the
	// sweep's instants, the plane has structure wherever the travel does: min and max differ, and the centre (home) is
	// the dark end. Feedback 0 AND the Fb tilt pill off: round 2's tilt makes a loop out of Feedback 0, which would
	// structure the crossing on its own and hide whether the averaging does.
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	d.set(static_cast<size_t>(phr::Param::ThroughZero), 1.0f);
	d.set(static_cast<size_t>(phr::Param::Position), 0.0f);
	d.set(static_cast<size_t>(phr::Param::Feedback), 0.0f);
	d.set(static_cast<size_t>(phr::Param::XFbTiltOn), 0.0f);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 760.0f, 460.0f);
	ui::PhaserPad* pad = panel->padForTest();
	visage::Canvas canvas;
	pad->draw(canvas);
	REQUIRE(pad->builtForTest().settings.mode == phr::Mode::Flanger);
	REQUIRE(pad->builtForTest().settings.throughZero);
	const auto& cells = pad->mapForTest();
	const auto [lo, hi] = std::minmax_element(cells.begin(), cells.end());
	MESSAGE("Flanger + TZ at Position 0: terrain cells lo " << *lo << " hi " << *hi);
	CHECK(*hi - *lo > 0.5f);
	constexpr size_t kW = ui::PhaserPad::kMapWide;
	constexpr size_t kH = ui::PhaserPad::kMapHigh;
	CHECK(cells[(kH / 2) * kW + kW / 2] < 0.3f * *hi); // home is dark (the case above says why not a pool)
}
TEST_CASE("PHASER's pad terrain is not blank under through-zero at Position 0 with the follower moving the sweep and the LFO off") {
	// THE REVIEW'S CASE: the follower moves the sweep too, so with the LFO off and Env on the instants walk Position +
	// Env Amount·[0, 1] — without that, the map was one instant again, the crossing, and blank.
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	d.set(static_cast<size_t>(phr::Param::ThroughZero), 1.0f);
	d.set(static_cast<size_t>(phr::Param::Position), 0.0f);
	d.set(static_cast<size_t>(phr::Param::Feedback), 0.0f);
	d.set(static_cast<size_t>(phr::Param::XFbTiltOn), 0.0f);
	d.set(static_cast<size_t>(phr::Param::LfoOn), 0.0f);
	d.set(static_cast<size_t>(phr::Param::EnvOn), 1.0f);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 760.0f, 460.0f);
	ui::PhaserPad* pad = panel->padForTest();
	visage::Canvas canvas;
	pad->draw(canvas);
	REQUIRE_FALSE(pad->builtForTest().settings.lfoOn);
	REQUIRE(pad->builtForTest().settings.envOn);
	const auto& cells = pad->mapForTest();
	const auto [lo, hi] = std::minmax_element(cells.begin(), cells.end());
	MESSAGE("Flanger + TZ at Position 0, Env only: terrain cells lo " << *lo << " hi " << *hi);
	CHECK(*hi - *lo > 0.5f);
}
TEST_CASE("PHASER's pad zoom buttons step one kZoomStep a click, clamp at the ends, and never move the sound") {
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 760.0f, 460.0f);
	ui::PhaserPad* pad = panel->padForTest();
	REQUIRE(pad != nullptr);
	visage::Canvas canvas;
	pad->draw(canvas);
	static_assert(ui::PhaserPad::zoomControlShown());
	// THE SHAPER'S PAD NAMES NO ZOOM CONTROL, so it draws and handles none (its look is the capture's proof).
	static_assert(!ui::ExplorePad<ui::ShaperPadPolicy>::zoomControlShown());
	// INSIDE THE PAD, ON ROTATE'S ROW, CLEAR OF IT.
	const visage::Bounds box = pad->zoomBounds();
	CHECK(box.x() >= 0.0f);
	CHECK(box.right() < pad->width());
	const visage::Bounds rot = panel->rotateForTest()->bounds();
	CHECK(box.right() + pad->x() < rot.x());
	CHECK(std::fabs((box.y() + pad->y()) - rot.y()) < 1.0f);
	std::array<float, phr::phase_space::kPhaseCount> travel {};
	for (size_t i = 0; i < travel.size(); ++i) {
		travel[i] = d.f.deviceHost.fieldAt(d.device, phr::kTravelFirst + i);
	}
	const auto click = [&](visage::Bounds b) {
		const visage::Point c { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		pad->onMouseDown().callback(at(c.x, c.y));
		pad->onMouseUp().callback(at(c.x, c.y));
	};
	CHECK(pad->zoom() == 1.0);
	CHECK(pad->zoomText() == "100%");
	click(pad->zoomPlusBounds());
	CHECK(std::fabs(pad->zoom() - ui::PhaserPad::kZoomStep) < 1.0e-12);
	CHECK(pad->zoomText() == "112%");
	click(pad->zoomMinusBounds());
	click(pad->zoomMinusBounds());
	CHECK(std::fabs(pad->zoom() - 1.0 / ui::PhaserPad::kZoomStep) < 1.0e-12);
	// THE READOUT SWALLOWS A PRESS: no step, and no star.
	const double before = pad->zoom();
	click({ box.x() + ui::PhaserPad::kZoomButton + 2.0f, box.y(), ui::PhaserPad::kZoomReadout - 4.0f, box.height() });
	CHECK(pad->zoom() == before);
	// CLAMPED AT BOTH ENDS, as the wheel is.
	for (int i = 0; i < 80; ++i) {
		click(pad->zoomPlusBounds());
	}
	CHECK(pad->zoom() == ui::PhaserPad::kMaxZoom);
	CHECK(pad->zoomText() == "6400%");
	for (int i = 0; i < 80; ++i) {
		click(pad->zoomMinusBounds());
	}
	CHECK(pad->zoom() == ui::PhaserPad::kMinZoom);
	CHECK(pad->zoomText() == "25%");
	// NONE OF IT MOVED THE SOUND: the travel is where it was, every float.
	for (size_t i = 0; i < travel.size(); ++i) {
		CAPTURE(i);
		CHECK(std::bit_cast<uint32_t>(d.f.deviceHost.fieldAt(d.device, phr::kTravelFirst + i))
		      == std::bit_cast<uint32_t>(travel[i]));
	}
	// AND A PRESS OUTSIDE THE BOX IS THE PAD'S OWN (the star goes there): not swallowed.
	pad->setZoom(1.0);
	pad->onMouseDown().callback(at(pad->width() * 0.5f + 40.0f, pad->height() * 0.5f + 30.0f));
	pad->onMouseUp().callback(at(pad->width() * 0.5f + 40.0f, pad->height() * 0.5f + 30.0f));
	size_t moved = 0;
	for (size_t i = 0; i < travel.size(); ++i) {
		moved += d.f.deviceHost.fieldAt(d.device, phr::kTravelFirst + i) != travel[i] ? 1 : 0;
	}
	CHECK(moved > travel.size() / 2);
}
TEST_CASE("PHASER's window seats the ten explore pills under the pad, each on its own two params, gated as the engine hears them") {
	namespace ps = phr::phase_space;
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point least = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, std::max(760.0f, least.x), std::max(460.0f, least.y));
	(void)panel->updateContent();
	const visage::Bounds pad = panel->padForTest()->bounds();
	// EVERY PILL: under the pad, inside the window, clear of every other pill, and offering exactly its own two params.
	for (size_t g = 0; g < ps::kGroupCount; ++g) {
		CAPTURE(g);
		const auto group = static_cast<ps::Group>(g);
		ui::FaderPill* pill = panel->explorePillForTest(group);
		REQUIRE(pill != nullptr);
		const visage::Bounds b = pill->bounds();
		CHECK(b.y() >= pad.bottom());
		CHECK(b.x() >= pad.x() - 0.5f);
		CHECK(b.right() <= pad.right() + 0.5f);
		CHECK(b.bottom() <= panel->height());
		for (size_t h = g + 1; h < ps::kGroupCount; ++h) {
			CHECK_FALSE(b.overlaps(panel->explorePillForTest(static_cast<ps::Group>(h))->bounds()));
		}
		std::vector<ui::ControlFace::Offered> offered;
		pill->offeredControls(offered);
		REQUIRE(offered.size() == 2); // Start and End (held; End is drawn only on a fill)
	}
	const auto addressed = panel->addressedParamsForTest();
	for (size_t g = 0; g < ps::kGroupCount; ++g) {
		const auto group = static_cast<ps::Group>(g);
		CHECK(std::find(addressed.begin(), addressed.end(), phr::pillKnobParam(group)) != addressed.end());
		CHECK(std::find(addressed.begin(), addressed.end(), phr::pillPowerParam(group)) != addressed.end());
	}
	// A DRAG SETS THE KNOB, A CLICK THE POWER — on the pill's own params.
	ui::FaderPill* pairing = panel->explorePillForTest(ps::Group::Pairing);
	const float y = pairing->height() * 0.5f;
	pairing->onMouseDown().callback(at(10.0f, y));
	pairing->onMouseDrag().callback(at(10.0f - 30.0f, y));
	pairing->onMouseUp().callback(at(10.0f - 30.0f, y));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::XPairing)) < 0.5f); // from the neutral, down
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::XSpread)) == 0.5f); // the neutral, untouched
	ui::FaderPill* curve = panel->explorePillForTest(ps::Group::Curve);
	curve->onMouseDown().callback(at(10.0f, y));
	curve->onMouseUp().callback(at(10.0f, y));
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(phr::Param::XCurveOn)) == 0.0f);
	// THE GATES. Defaults: Phaser, LFO on, Damping open, Drive 0 — so Colour and Bias are grey and the rest live.
	const auto live = [&](ps::Group g) {
		(void)panel->updateContent();
		return panel->explorePillActiveForTest(g);
	};
	CHECK(live(ps::Group::Spread));
	CHECK(live(ps::Group::Stagger));
	CHECK(live(ps::Group::Pairing));
	CHECK_FALSE(live(ps::Group::Colour));
	CHECK_FALSE(live(ps::Group::Bias));
	d.set(static_cast<size_t>(phr::Param::Damping), static_cast<float>(std::log2(3000.0)));
	CHECK(live(ps::Group::Colour));
	d.set(static_cast<size_t>(phr::Param::Drive), 0.3f);
	CHECK(live(ps::Group::Bias));
	d.set(static_cast<size_t>(phr::Param::LfoOn), 0.0f);
	CHECK_FALSE(live(ps::Group::Stagger));
	CHECK_FALSE(live(ps::Group::Shape));
	CHECK_FALSE(live(ps::Group::RateSkew)); // Normal sweep
	d.set(static_cast<size_t>(phr::Param::Sweep), 1.0f);
	CHECK(live(ps::Group::RateSkew)); // a barberpole hears it with the LFO off
	d.set(static_cast<size_t>(phr::Param::Sweep), 0.0f);
	CHECK(live(ps::Group::Curve));
	CHECK(live(ps::Group::FbTilt));
	d.set(static_cast<size_t>(phr::Param::Mode), 1.0f);
	CHECK_FALSE(live(ps::Group::Pairing));
	CHECK(live(ps::Group::Spread));
	CHECK(live(ps::Group::Skew));
}
TEST_CASE("PHASER UI fuzz: card, plot, window and pad under extreme modulation and readings return, every frame") {
	// THE 2026-10-03 LOCK-UP (the whole synth locked up, even after the modulator was disabled). Every surface
	// the phaser draws, fed modulated rows, stored rows, travel and readings at random and at their extremes, with
	// a watchdog: a frame that does not come back within 2 s is a hang, not a slow frame.
	DynCardFixture<ui::PhaserCard> d(phr::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, phr::kDescriptor);
	auto* panel = dynamic_cast<ui::PhaserExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 1400.0f, 800.0f);
	ui::PhaserPad* pad = panel->padForTest();
	REQUIRE(pad != nullptr);
	visage::Canvas canvas;
	std::atomic<long long> startedNs { 0 };
	std::atomic<int> stage { -1 };
	std::atomic<bool> done { false };
	std::thread dog([&] {
		while (!done) {
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			const long long s = startedNs.load();
			if (s != 0 && std::chrono::steady_clock::now().time_since_epoch().count() - s > 2'000'000'000LL) {
				std::fprintf(stderr, "PHASER UI FUZZ WATCHDOG: stuck in stage %d\n", stage.load());
				std::_Exit(3);
			}
		}
	});
	std::mt19937 rng(20261003u);
	std::uniform_real_distribution<float> u(0.0f, 1.0f);
	const float nan = std::numeric_limits<float>::quiet_NaN();
	const float inf = std::numeric_limits<float>::infinity();
	const auto pick = [&](size_t i) {
		const auto& desc = phr::kParamDescs[i];
		const auto lo = static_cast<float>(desc.min);
		const auto hi = static_cast<float>(desc.max);
		switch (rng() % 8) {
		case 0: return lo;
		case 1: return hi;
		case 2: return lo - 10.0f * (hi - lo);
		case 3: return hi + 10.0f * (hi - lo);
		case 4: return rng() % 4 == 0 ? nan : inf;
		default: return lo + (hi - lo) * u(rng);
		}
	};
	const auto anyValue = [&] {
		const float vals[] = { 0.0f, 0.5f, 1.0f, -1.0f, 1e9f, -1e9f, nan, inf, -inf, 0.999999f, 20000.0f };
		return rng() % 2 ? u(rng) : vals[rng() % std::size(vals)];
	};
	double worstMs = 0.0;
	std::array<double, 10> stageMs {};
	auto lastMark = std::chrono::steady_clock::now();
	const auto mark = [&](int s) {
		const auto now = std::chrono::steady_clock::now();
		const int prev = stage.load();
		if (prev >= 0 && prev < 10) {
			stageMs[static_cast<size_t>(prev)] = std::max(stageMs[static_cast<size_t>(prev)],
			                                              std::chrono::duration<double, std::milli>(now - lastMark).count());
		}
		lastMark = now;
		stage = s;
	};
	for (int it = 0; it < 400; ++it) {
		CAPTURE(it);
		const auto begin = std::chrono::steady_clock::now();
		startedNs = begin.time_since_epoch().count();
		mark(0);
		if (rng() % 5 == 0) {
			const size_t p = rng() % phr::kParamCount;
			d.set(p, std::clamp(pick(p), static_cast<float>(phr::kParamDescs[p].min),
			                    static_cast<float>(phr::kParamDescs[p].max)));
		}
		if (rng() % 10 == 0) {
			for (size_t i = 0; i < phr::phase_space::kPhaseCount; ++i) {
				d.f.deviceHost.setField(d.device, phr::kTravelFirst + i, rng() % 3 ? 4.0f * (u(rng) - 0.5f) : anyValue());
			}
			d.f.bridge.refresh();
		}
		mark(1);
		feathers::FxChain moved = d.f.bridge.fxChain();
		for (size_t i = 0; i < phr::kParamCount; ++i) {
			if (phr::kParamDescs[i].modulatable && rng() % 2 == 0) {
				moved.device[d.slot].row.field[i] = pick(i);
			}
		}
		d.f.bridge.publishModulated(d.f.bridge.modRack(), moved, feathers::RackPlayheads {},
		                            feathers::mod::TransportClock::unknown());
		PhaserReadings r {};
		for (float& v : r) {
			v = anyValue();
		}
		d.publish(r);
		mark(2);
		(void)d.card->updateContent();
		mark(3);
		(void)panel->updateContent();
		mark(4);
		d.card->plotForTest()->draw(canvas);
		mark(5);
		panel->plotForTest()->draw(canvas);
		mark(6);
		pad->setClockForTest(std::chrono::steady_clock::time_point {} + std::chrono::seconds(it));
		pad->draw(canvas);
		mark(7);
		if (rng() % 8 == 0) {
			const float x = pad->width() * u(rng);
			const float y = pad->height() * u(rng);
			pad->onMouseDown().callback(at(x, y));
			pad->onMouseDrag().callback(at(x + 400.0f * (u(rng) - 0.5f), y + 400.0f * (u(rng) - 0.5f)));
			pad->onMouseUp().callback(at(x, y));
		}
		mark(8);
		if (rng() % 8 == 0) {
			ui::PhaserPlot* plot = panel->plotForTest();
			const float x = plot->width() * (1.5f * u(rng) - 0.25f);
			plot->onMouseDown().callback(at(x, plot->height() * 0.5f));
			plot->onMouseDrag().callback(at(x + 300.0f * (u(rng) - 0.5f), plot->height() * 0.5f));
			plot->onMouseUp().callback(at(x, plot->height() * 0.5f));
		}
		mark(9);
		startedNs = 0;
		worstMs = std::max(worstMs,
		                   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
	}
	done = true;
	dog.join();
	MESSAGE("phaser UI fuzz: worst iteration " << worstMs << " ms");
	for (size_t s = 0; s < 9; ++s) {
		MESSAGE("stage " << s << " worst " << stageMs[s] << " ms");
	}
}

namespace {
/// EVERY KNOB UNDER `scope` THAT A HAND CAN TURN, by the declaration it drives. The `ParamDesc` is the
/// knob's identity here because a card may rebuild its knobs when a control moves, and a pointer to a
/// destroyed knob is no identity at all.
std::map<const device::ParamDesc*, ui::Knob*> turnableKnobs(visage::Frame* scope) {
	std::vector<ui::Knob*> knobs;
	collectKnobs(scope, &knobs);
	std::map<const device::ParamDesc*, ui::Knob*> out;
	for (ui::Knob* k : knobs) {
		if (k->control() != nullptr && k->enabled() && k->width() > 0.0f && k->isVisible()) {
			out.emplace(&k->control()->desc(), k);
		}
	}
	return out;
}
/// TURN EVERY KNOB OF ONE CARD BY HAND, one at a time, and after each turn read EVERY knob turned before
/// it: none may have moved. Returns how many readings were checked.
size_t turnEachAndCheckTheRest(ModFixture& f, const std::function<visage::Frame*()>& card, size_t* moved) {
	std::map<const device::ParamDesc*, double> expected;
	std::vector<const device::ParamDesc*> order;
	for (const auto& [desc, knob] : turnableKnobs(card())) {
		order.push_back(desc);
	}
	size_t checked = 0;
	for (const device::ParamDesc* target : order) {
		visage::Frame* scope = card();
		REQUIRE(scope != nullptr);
		auto knobs = turnableKnobs(scope);
		const auto it = knobs.find(target);
		if (it == knobs.end()) {
			continue; // hidden by an earlier edit (a mode that shows other controls)
		}
		ui::Knob* knob = it->second;
		const double before = knob->control()->value();
		const visage::Point from = f.faceCentre(knob);
		dragThroughRoot(f.root, from, from + visage::Point { 0.0f, before >= knob->control()->desc().max ? 40.0f : -40.0f });
		f.gui.tickForTest();
		knobs = turnableKnobs(card());
		if (const auto again = knobs.find(target); again != knobs.end()) {
			expected[target] = again->second->control()->value();
			*moved += expected[target] != before ? 1 : 0;
		}
		for (const auto& [desc, value] : expected) {
			const auto k = knobs.find(desc);
			if (k == knobs.end()) {
				continue;
			}
			CAPTURE(std::string(target->name != nullptr ? target->name : "?"));
			CAPTURE(std::string(desc->name != nullptr ? desc->name : "?"));
			CHECK(k->second->control()->value() == value);
			++checked;
		}
	}
	return checked;
}
} // namespace

TEST_CASE("KO: turning one knob on a device card never puts another of its knobs back, on every fx card") {
	// THE DEFECT: controls clicked back to their defaults after another parameter on the same device
	// changed — and not on the synth's own params. THE CARD'S OWN KNOBS, turned by a
	// press and a drag through the editor's tree (the road a hand takes), with the editor ticked between, and
	// every knob already turned read again after each new turn. Each device alone in the lane, as the
	// armable sweep above has it.
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> tags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		tags.emplace_back(d->traits.tag);
	}
	REQUIRE(tags.size() >= 4);
	size_t checked = 0;
	size_t moved = 0; // turns that moved their own knob: the sweep is vacuous without them
	for (const std::string& tag : tags) {
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(tag.c_str());
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		f.gui.tickForTest();
		checked += turnEachAndCheckTheRest(f, [&]() -> visage::Frame* { return lane->cardAt(slot); }, &moved);
	}
	MESSAGE("knob readings checked: " << checked << ", turns that moved: " << moved << " over " << tags.size() << " fx cards");
	CHECK(moved > 50);
	CHECK(checked > 50);
}
TEST_CASE("KO: turning one knob on a modulator card never puts another of its knobs back, on every modulator") {
	std::vector<std::string> tags;
	for (const device::Descriptor& d : kDevices) {
		if (d.mod != nullptr && d.traits.tag != nullptr) {
			tags.emplace_back(d.traits.tag);
		}
	}
	REQUIRE_FALSE(tags.empty());
	size_t checked = 0;
	size_t moved = 0; // turns that moved their own knob: the sweep is vacuous without them
	for (const std::string& tag : tags) {
		CAPTURE(tag);
		ModFixture f;
		const size_t slot = f.bridge.addRackDevice(tag.c_str());
		if (slot >= kMaxRackEnvelopes) {
			continue; // a modulator the strip does not take
		}
		f.panel->resized();
		f.gui.tickForTest();
		checked += turnEachAndCheckTheRest(f, [&]() -> visage::Frame* { return f.panel; }, &moved);
	}
	MESSAGE("knob readings checked: " << checked << ", turns that moved: " << moved << " over " << tags.size() << " modulators");
	CHECK(moved > 20);
}

namespace {
//------------------------------------------------------------------------------------------------//
// THE WINDOW HEADER'S ENABLE CHIP — ONE, FOR EVERY DEVICE, because devices were inconsistently     //
// adding their own active chips in the window. Before this, five panels drew an enable in their    //
// bodies (HARM, OWLTT, Automod and Shaper a hand-drawn square, Smooth an "On" chip), each in its    //
// own place, and six drew none. Every case below asserts a VALUE — the field the engine reads —    //
// never a rendered pixel.                                                                          //
//------------------------------------------------------------------------------------------------//

/// ONE DEVICE'S WINDOW, OPENED THE WAY THE EDITOR OPENS IT: `makeExpandedView`, the modulation state
/// handed over, and presented through `SurfaceLayers` — so the chrome is asking a `MountedFrame`, as it
/// does in both hosts, and not the panel directly. Declaration order is the teardown contract: the
/// layers (and the view in them) go before the history and the host their controls point at.
struct HeaderChipWindow {
	std::unique_ptr<HeldChain> held; ///< SKLITTER's chains, which its panel reads; outlives the fixture
	FilterFixture f;
	ui::ModState mod;
	ui::EditHistory history;
	ui::WidgetRegistry registry;
	ui::SurfaceLayers layers;
	const device::Descriptor& desc;
	size_t slot = 0;
	DeviceInstanceId device = kNoInstance;
	size_t enableIndex = 0;
	ui::ExpandedView* view = nullptr;   ///< the device's own panel — the BODY
	ui::ExpandedPanel* panel = nullptr; ///< the chrome around it — the HEADER

	explicit HeaderChipWindow(const device::Descriptor& d) : desc(d) {
		if (device::sameTag(d.traits.tag, fx::sklitter::kTraits.tag)) {
			held = std::make_unique<HeldChain>();
		}
		layers.setBounds(0, 0, 1400, 900);
		slot = f.bridge.addFxDevice(d.traits.tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		device = f.bridge.fxDeviceAt(slot);
		if (held != nullptr) {
			f.bridge.fxBodies().attach(device, held->surface());
		}
		f.lane->resized();
		REQUIRE(d.traits.enableControl >= 0);
		enableIndex = static_cast<size_t>(d.traits.enableControl);
		std::unique_ptr<ui::ExpandedView> built = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, device, d);
		REQUIRE(built != nullptr);
		built->setModulation(&mod, &history);
		view = built.get();
		REQUIRE(layers.present(std::move(built), ui::SurfaceNeed::SpansGesture, d.traits.name, {}) != ui::kNoSurface);
		panel = layers.overlay().panelForTest();
		REQUIRE(panel != nullptr);
	}

	/// THE FIELD THE ENGINE READS — every assertion's reference.
	float field() const { return f.deviceHost.fieldAt(device, enableIndex); }
	void setField(float v) { f.deviceHost.setField(device, enableIndex, v); }
	/// The card in the lane, whose head square is the other surface of the same switch.
	ui::FxCard* card() const { return dynamic_cast<ui::FxCard*>(f.lane->cardAt(slot)); }
	/// A PRESS ON THE CHIP, WHERE A HAND PRESSES IT — through the panel's own mouse road, press and release.
	void pressChip() {
		const visage::Bounds sw = panel->enableBounds();
		const float x = sw.x() + sw.width() * 0.5f;
		const float y = sw.y() + sw.height() * 0.5f;
		panel->onMouseDown().callback(at(x, y));
		panel->onMouseUp().callback(at(x, y));
	}
};
/// EVERY DEVICE THIS BUILD CAN OPEN A WINDOW FOR IN FEATHERS' OWN FX LANE. The chain container is left
/// to its own case in `test_gui_widgets_nest.cpp`: its panel is registered by the rack module and it
/// is opened from inside a chain, not from this lane.
std::vector<const device::Descriptor*> windowedFxDevices() {
	std::vector<const device::Descriptor*> out;
	for (const device::Descriptor& desc : kDevices) {
		if (ui::hasExpandedView(desc) && !device::sameTag(desc.traits.tag, composite::kTraits.tag)) {
			out.push_back(&desc);
		}
	}
	return out;
}
} // namespace

TEST_CASE("window header: every device's window carries ONE enable chip, reading and writing the device's "
          "own enable, with the card's undo, and the card agrees") {
	const std::vector<const device::Descriptor*> devices = windowedFxDevices();
	// THE SWEEP MUST HAVE SWEPT SOMETHING — the empty-filter trap. Eleven fx devices open a window
	// today; a floor rather than an equality, so the next panel joins the sweep without an edit here.
	CAPTURE(devices.size());
	REQUIRE(devices.size() >= 11);

	for (const device::Descriptor* desc : devices) {
		CAPTURE(std::string(desc->traits.name));
		HeaderChipWindow w(*desc);

		// THE CHIP EXISTS, IN THE HEADER, WHERE THE CARD PUTS ITS OWN: the card's 12px square, inboard
		// of the host's marks — left of the fold chevron, inside the title row, clear of the title text.
		const visage::Bounds sw = w.panel->enableBounds();
		REQUIRE(sw.width() > 0.0f);
		CHECK(sw.width() == doctest::Approx(ui::ExpandedPanel::kEnableSize));
		CHECK(sw.height() == doctest::Approx(ui::ExpandedPanel::kEnableSize));
		const visage::Bounds row = w.panel->titleBounds();
		CHECK(sw.y() >= row.y());
		CHECK(sw.bottom() <= row.bottom());
		CHECK(sw.right() <= w.panel->foldBounds().x());
		CHECK(sw.x() >= ui::ExpandedOverlay::kPanelPad
		                    + ui::trackedWidth(ui::font(ui::kControlSize), desc->traits.name, ui::kControlTracking));
		// AND IT IS THE PANEL'S OWN PRESS, not a child's: nothing is seated on top of it.
		CHECK(w.panel->frameAtPoint({ sw.x() + sw.width() * 0.5f, sw.y() + sw.height() * 0.5f }) == w.panel);

		// IT READS THE FIELD, both ways — through the tick, which is the road a card's toggle, an undo, a
		// patch load and automation all arrive by.
		w.setField(0.0f);
		(void)w.layers.updateContent();
		CHECK_FALSE(w.panel->enableShownForTest());
		CHECK_FALSE(w.view->deviceEnabled());
		w.setField(1.0f);
		(void)w.layers.updateContent();
		CHECK(w.panel->enableShownForTest());

		// A PRESS WRITES IT — the VALUE the engine reads, restated independently as "the opposite of what
		// it was" — and the card's head reads the same number.
		ui::FxCard* card = w.card();
		REQUIRE(card != nullptr);
		REQUIRE(card->hasEnable());
		CHECK(card->headEnabled());
		const size_t steps = w.history.length();
		w.pressChip();
		CHECK(w.field() == 0.0f);
		CHECK_FALSE(card->headEnabled());
		CHECK_FALSE(w.panel->enableShownForTest());
		// ONE UNDO STEP, the card's road (`FxCard::toggleEnable` brackets the same gesture), and undoing it
		// puts the device back on.
		CHECK(w.history.length() == steps + 1);
		REQUIRE(w.history.canUndo());
		w.history.undo();
		CHECK(w.field() == 1.0f);
		(void)w.layers.updateContent();
		CHECK(w.panel->enableShownForTest());

		// THE CARD'S OWN SQUARE MOVES THE WINDOW'S CHIP — the other direction of "the card agrees".
		REQUIRE(card->toggleEnable().has_value());
		CHECK(w.field() == 0.0f);
		(void)w.layers.updateContent();
		CHECK_FALSE(w.panel->enableShownForTest());
		w.pressChip();
		CHECK(w.field() == 1.0f);

		// AND A PRESS ON THE CHIP IS NOT A PRESS ON THE HEAD: a double-click on it toggles twice and
		// neither folds the window nor leaves a drag armed.
		const ui::ExpandedOverlay::PanelId id = w.layers.overlay().frontPanelIdForTest();
		const float cx = sw.x() + sw.width() * 0.5f;
		const float cy = sw.y() + sw.height() * 0.5f;
		w.panel->onMouseDown().callback(at(cx, cy));
		w.panel->onMouseDown().callback(doubleAt(cx, cy));
		w.panel->onMouseUp().callback(doubleAt(cx, cy));
		CHECK(w.field() == 1.0f);
		CHECK_FALSE(w.layers.overlay().folded(id));
	}
}
TEST_CASE("window header: no window BODY carries a device-enable control of its own — the header chip is "
          "the only one") {
	// **ASKED OF WHAT IS BUILT AND OF WHAT A PRESS DOES, not of a list.** Two ways a body can carry an
	// enable, and this finds both:
	//   - A WIDGET bound to the enable (Smooth's "On" `SwitchChip` was one): every `Knob`, `ValueBox`
	//     and `SwitchChip` under the body is asked which control it holds.
	//   - A HAND-DRAWN SQUARE with its own hit test (HARM, OWLTT, Automod and Shaper each had one):
	//     invisible to a widget walk, so the body's OWN press road is swept across its whole face and
	//     the enable field must never move.
	// Other switches that look alike — EQ's per-band On, Shaper's extras powers, Automod's Keytrack,
	// Featherverb's Lite — are different controls, and this asserts only about the DEVICE's enable.
	const std::vector<const device::Descriptor*> devices = windowedFxDevices();
	REQUIRE(devices.size() >= 11);
	size_t widgetsSeen = 0;
	for (const device::Descriptor* desc : devices) {
		CAPTURE(std::string(desc->traits.name));
		HeaderChipWindow w(*desc);
		const device::ParamDesc* enable = &desc->params[w.enableIndex];

		std::vector<std::string> bound;
		const auto walk = [&](visage::Frame* frame, auto& self) -> void {
			const device::Modulatable* c = nullptr;
			if (const auto* knob = dynamic_cast<const Knob*>(frame); knob != nullptr) {
				c = knob->control();
			}
			else if (const auto* box = dynamic_cast<const ui::ValueBox*>(frame); box != nullptr) {
				c = &box->control();
			}
			else if (const auto* chip = dynamic_cast<const ui::SwitchChip*>(frame); chip != nullptr) {
				c = &chip->control();
			}
			if (c != nullptr) {
				++widgetsSeen;
				// BY THE DESCRIPTOR ENTRY'S ADDRESS, which is the control's identity in its device.
				if (&c->desc() == enable) {
					bound.emplace_back(c->desc().name != nullptr ? c->desc().name : "?");
				}
			}
			for (visage::Frame* child : frame->children()) {
				self(child, self);
			}
		};
		walk(w.view, walk);
		CHECK(bound.empty());

		// THE BODY'S OWN PRESS ROAD, over its whole face. The field starts on and must still be on.
		w.setField(1.0f);
		const float wide = static_cast<float>(w.view->width());
		const float tall = static_cast<float>(w.view->height());
		REQUIRE(wide > 0.0f);
		REQUIRE(tall > 0.0f);
		size_t moved = 0;
		for (float y = 2.0f; y < tall; y += 6.0f) {
			for (float x = 2.0f; x < wide; x += 6.0f) {
				w.view->onMouseDown().callback(at(x, y));
				w.view->onMouseUp().callback(at(x, y));
				if (w.field() != 1.0f) {
					++moved;
					w.setField(1.0f);
				}
			}
		}
		CHECK(moved == 0);
	}
	// NON-VACUITY: the walk met real widgets, so "none is bound to the enable" is about something.
	CHECK(widgetsSeen > 50);
}
TEST_CASE("window header: a selected card's outline is the colour its window is outlined in") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	REQUIRE(lane->onExpandDevice != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);

	// TWO DEVICES, SO TWO HUES: a colour that merely matched SOME constant could not match both.
	const size_t slots[] = { f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag),
		                     f.bridge.addFxDevice(feathers::fx::harm::kTraits.tag) };
	lane->resized();
	std::vector<uint32_t> seen;
	for (const size_t slot : slots) {
		CAPTURE(slot);
		REQUIRE(slot < feathers::kMaxFxSlots);
		const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
		REQUIRE(id != feathers::kNoInstance);
		lane->onExpandDevice(slot);
		REQUIRE(frontDevice(*overlay) == id);
		const ui::ExpandedPanel* panel = overlay->panelForTest();
		REQUIRE(panel != nullptr);
		auto* card = dynamic_cast<ui::FxCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);

		// THE REFERENCE, RESTATED FROM THE PALETTE AND NOT FROM EITHER DRAWER: the instance's hue at
		// the panel edge's strength.
		const uint32_t hue = ui::kInstanceTints[(id - 1) % ui::kInstanceTints.size()];
		const uint32_t expected = ui::withAlpha(hue, ui::kPanelEdgeTintAlpha);
		CHECK(panel->edgeColor() == expected);

		// SELECTED BY A PRESS, the lane's own road (`onCardPressed` → `focusSlot`).
		REQUIRE(card->onCardPressed != nullptr);
		card->onCardPressed();
		REQUIRE(card->headFocused());
		const ui::CardFrameStyle style = card->frameStyle();
		CHECK(style.outline);
		CHECK(style.outlineColor == expected);
		CHECK(style.outlineColor == panel->edgeColor());
		// NOT THE OLD ACCENT — the colour this replaced, so a revert cannot pass by coincidence.
		CHECK(style.outlineColor != ui::kAccent);
		seen.push_back(style.outlineColor);
	}
	// NON-VACUITY: the two selections wore two different colours.
	REQUIRE(seen.size() == 2);
	CHECK(seen[0] != seen[1]);
	// AND ONLY THE SELECTED ONE IS OUTLINED: the first card lost its outline when the second was pressed.
	CHECK_FALSE(dynamic_cast<ui::FxCard*>(lane->cardAt(slots[0]))->frameStyle().outline);
	// A CARD WITH NO INSTANCE (no tint to share) keeps the accent rather than going invisible.
	CHECK(ui::cardFrameStyleOf(true, false, false, 0).outlineColor == ui::kAccent);
	CHECK(ui::cardFrameStyleOf(false, false, false, 0xff5aa9e6).outlineColor == 0u);
}
TEST_CASE("window header: a press on an effect's card head brings its open window to the front, and does "
          "nothing else") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);

	const size_t a = f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag);
	const size_t b = f.bridge.addFxDevice(feathers::fx::harm::kTraits.tag);
	const size_t c = f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag); // never opened
	lane->resized();
	const feathers::DeviceInstanceId idA = f.bridge.fxDeviceAt(a);
	const feathers::DeviceInstanceId idB = f.bridge.fxDeviceAt(b);
	auto* cardA = dynamic_cast<ui::FxCard*>(lane->cardAt(a));
	auto* cardB = dynamic_cast<ui::FxCard*>(lane->cardAt(b));
	auto* cardC = dynamic_cast<ui::FxCard*>(lane->cardAt(c));
	REQUIRE(cardA != nullptr);
	REQUIRE(cardB != nullptr);
	REQUIRE(cardC != nullptr);

	lane->onExpandDevice(a);
	lane->onExpandDevice(b);
	REQUIRE(overlay->panelCount() == 2);
	REQUIRE(frontDevice(*overlay) == idB); // the last opened is on top: A is behind

	// A's HEAD, PRESSED ONCE: A's window comes to the front.
	const std::optional<visage::Point> headA = bareHeadPoint(*cardA);
	REQUIRE(headA.has_value());
	pressCard(*cardA, *headA);
	CHECK(frontDevice(*overlay) == idA);
	// AND NOTHING ELSE MOVED: both windows still open, neither folded, the card not folded.
	CHECK(overlay->panelCount() == 2);
	for (const ui::ExpandedOverlay::PanelId id : overlay->panelIdsForTest()) {
		CHECK_FALSE(overlay->folded(id));
	}
	CHECK_FALSE(cardA->folded());

	// AND BACK: B's head brings B's.
	const std::optional<visage::Point> headB = bareHeadPoint(*cardB);
	REQUIRE(headB.has_value());
	pressCard(*cardB, *headB);
	CHECK(frontDevice(*overlay) == idB);

	// A HEAD WITH NO WINDOW OPEN KEEPS ITS OLD BEHAVIOUR: nothing opens, nothing reorders.
	const std::optional<visage::Point> headC = bareHeadPoint(*cardC);
	REQUIRE(headC.has_value());
	pressCard(*cardC, *headC);
	CHECK(overlay->panelCount() == 2);
	CHECK(frontDevice(*overlay) == idB);

	// A BODY PRESS IS NOT A HEAD PRESS: A's body does not raise A's window.
	const visage::Point bodyA = cardA->headEdge() == ui::CardHeadEdge::Left
	                                ? visage::Point { cardA->bodyLeft() + 4.0f, static_cast<float>(cardA->height()) - 4.0f }
	                                : visage::Point { static_cast<float>(cardA->width()) - 4.0f, static_cast<float>(cardA->height()) - 4.0f };
	REQUIRE_FALSE(cardA->pressOnHead(bodyA));
	pressCard(*cardA, bodyA);
	CHECK(frontDevice(*overlay) == idB);
}

namespace {
/// HOW MANY OPEN PANELS ARE WINDOWS ONTO `device` — read panel by panel, not inferred from the count.
size_t panelsOf(const ui::ExpandedOverlay& overlay, feathers::DeviceInstanceId device) {
	size_t n = 0;
	for (const ui::ExpandedOverlay::PanelId id : overlay.panelIdsForTest()) {
		const ui::ExpandedView* content = overlay.contentForTest(id);
		n += content != nullptr && content->watchedDevice() == device ? 1 : 0;
	}
	return n;
}
/// THE OPEN PANEL FOR `device`, or `kNoPanel`.
ui::ExpandedOverlay::PanelId panelIdOf(const ui::ExpandedOverlay& overlay, feathers::DeviceInstanceId device) {
	for (const ui::ExpandedOverlay::PanelId id : overlay.panelIdsForTest()) {
		const ui::ExpandedView* content = overlay.contentForTest(id);
		if (content != nullptr && content->watchedDevice() == device) {
			return id;
		}
	}
	return ui::ExpandedOverlay::kNoPanel;
}
} // namespace

TEST_CASE("expand toggle, the fx lane: a card's expand mark opens its device's window, closes it when it is "
          "open, and opens it again where it was left") {
	// Clicking a card's window-expand mark while its window is already expanded closes it. Pressed
	// through the CARD's own `onExpand`, which is the road the mark takes — card to
	// lane to editor — so a binding missed anywhere on it is seen.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	const size_t a = f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag);
	const size_t b = f.bridge.addFxDevice(feathers::fx::harm::kTraits.tag);
	lane->resized();
	const feathers::DeviceInstanceId idA = f.bridge.fxDeviceAt(a);
	const feathers::DeviceInstanceId idB = f.bridge.fxDeviceAt(b);
	auto* cardA = dynamic_cast<ui::FxCard*>(lane->cardAt(a));
	auto* cardB = dynamic_cast<ui::FxCard*>(lane->cardAt(b));
	REQUIRE(cardA != nullptr);
	REQUIRE(cardB != nullptr);
	REQUIRE(cardA->onExpand != nullptr);
	REQUIRE(cardB->onExpand != nullptr);
	REQUIRE_FALSE(overlay->open());

	SUBCASE("open, close, open") {
		cardA->onExpand();
		CHECK(panelsOf(*overlay, idA) == 1);
		cardA->onExpand();
		CHECK(panelsOf(*overlay, idA) == 0);
		CHECK(overlay->lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::ExpandToggle);
		cardA->onExpand();
		CHECK(panelsOf(*overlay, idA) == 1);
	}

	SUBCASE("with two windows open, toggling one leaves the other") {
		cardA->onExpand();
		cardB->onExpand();
		REQUIRE(panelsOf(*overlay, idA) == 1);
		REQUIRE(panelsOf(*overlay, idB) == 1);
		cardA->onExpand();
		CHECK(panelsOf(*overlay, idA) == 0);
		CHECK(panelsOf(*overlay, idB) == 1);
		cardB->onExpand();
		CHECK(panelsOf(*overlay, idB) == 0);
		CHECK_FALSE(overlay->open());
	}

	SUBCASE("the closed window's placement is kept") {
		cardA->onExpand();
		const ui::ExpandedOverlay::PanelId first = panelIdOf(*overlay, idA);
		REQUIRE(first != ui::ExpandedOverlay::kNoPanel);
		const visage::Bounds opened = overlay->panelBounds(first);
		overlay->beginPanelDrag();
		overlay->dragPanelBy({ -37.0f, -23.0f });
		const visage::Bounds moved = overlay->panelBounds(first);
		REQUIRE((moved.x() != opened.x() || moved.y() != opened.y())); // the drag really moved it
		cardA->onExpand();
		REQUIRE(panelsOf(*overlay, idA) == 0);
		cardA->onExpand();
		const ui::ExpandedOverlay::PanelId again = panelIdOf(*overlay, idA);
		REQUIRE(again != ui::ExpandedOverlay::kNoPanel);
		CHECK(overlay->panelBounds(again).x() == doctest::Approx(moved.x()));
		CHECK(overlay->panelBounds(again).y() == doctest::Approx(moved.y()));
	}
}
TEST_CASE("expand toggle, the modulator strip: its expand road closes the device's open window and opens it "
          "again") {
	// NO MODULATOR DECLARES A WINDOW TODAY (the strip case above), so the strip's binding is driven with
	// an effect's id — ids are one space across both strips, and it is the strip's own road through
	// `descriptorOf` to the editor's one opener that is under test.
	ModFixture f;
	REQUIRE(f.panel != nullptr);
	REQUIRE(f.panel->onExpandDevice != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	const size_t a = f.bridge.addFxDevice(feathers::fx::owltt::kTraits.tag);
	const size_t b = f.bridge.addFxDevice(feathers::fx::harm::kTraits.tag);
	f.bridge.refresh();
	const feathers::DeviceInstanceId idA = f.bridge.fxDeviceAt(a);
	const feathers::DeviceInstanceId idB = f.bridge.fxDeviceAt(b);
	REQUIRE(idA != feathers::kNoInstance);
	REQUIRE(idB != feathers::kNoInstance);

	f.panel->onExpandDevice(idA);
	f.panel->onExpandDevice(idB);
	REQUIRE(panelsOf(*overlay, idA) == 1);
	REQUIRE(panelsOf(*overlay, idB) == 1);
	f.panel->onExpandDevice(idA);
	CHECK(panelsOf(*overlay, idA) == 0);
	CHECK(panelsOf(*overlay, idB) == 1);
	f.panel->onExpandDevice(idA);
	CHECK(panelsOf(*overlay, idA) == 1);
	CHECK(panelsOf(*overlay, idB) == 1);
}
TEST_CASE("window header: a FOLDED card keeps its expand chip, drawn and pressable, and it opens the window "
          "without unfolding the card") {
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	ui::ExpandedOverlay* overlay = f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);

	// TWO CARDS THAT DRAW THEIR FOLDED HEADS BY DIFFERENT CODE (OWLTT's summary, HARM's ratio), both of
	// which measure their text from the chip's left edge.
	for (const auto& tag : { feathers::fx::owltt::kTraits.tag, feathers::fx::harm::kTraits.tag }) {
		const size_t slot = f.bridge.addFxDevice(tag);
		REQUIRE(slot < feathers::kMaxFxSlots);
		lane->resized();
		const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
		auto* card = dynamic_cast<ui::FxCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);
		CAPTURE(std::string(card->descriptor()->traits.name));
		card->setFolded(true);
		lane->resized();
		REQUIRE(card->folded());

		// THE CHIP IS THERE, INSIDE THE FOLDED CARD, AND THE HEAD LISTS IT AS DRAWN.
		const visage::Bounds ex = card->expandBounds();
		REQUIRE(ex.width() > 0.0f);
		CHECK(ex.x() >= 0.0f);
		CHECK(ex.y() >= 0.0f);
		CHECK(ex.right() <= static_cast<float>(card->width()));
		CHECK(ex.bottom() <= static_cast<float>(card->height()));
		size_t listed = 0;
		for (const ui::FxCard::HeadPart& part : card->headLayout().marks) {
			if (std::string_view(part.what) == "expand") {
				++listed;
				CHECK(part.box == ex);
			}
		}
		CHECK(listed == 1);

		// A PRESS ON IT OPENS THIS DEVICE'S WINDOW, and the card stays folded.
		const size_t before = overlay->panelCount();
		pressCard(*card, { ex.x() + ex.width() * 0.5f, ex.y() + ex.height() * 0.5f });
		CHECK(overlay->panelCount() == before + 1);
		CHECK(frontDevice(*overlay) == id);
		CHECK(card->folded());
	}
}
TEST_CASE("window header: SKLITTER's title row at its minimum width keeps the preset browser beside the "
          "enable chip, clear of both the title and the chip") {
	// The resume note's open question: the chip took 12px + a gap from the title row, and SKLITTER is the
	// one panel whose title row also carries a control (its preset browser, `titleAccessory`). A panel
	// opens at its content's MINIMUM, so the default-opened panel is the narrowest case a hand sees.
	const device::Descriptor* sk = nullptr;
	for (const device::Descriptor& desc : kDevices) {
		if (device::sameTag(desc.traits.tag, fx::sklitter::kTraits.tag)) {
			sk = &desc;
		}
	}
	REQUIRE(sk != nullptr);
	HeaderChipWindow w(*sk);
	REQUIRE(w.panel->accessory() != nullptr);
	// AT THE MINIMUM: nothing a hand chose, so the panel is the content's floor plus the chrome.
	REQUIRE_FALSE(w.layers.overlay().sizedForTest());
	const visage::Bounds acc = w.panel->accessoryBounds();
	const visage::Bounds sw = w.panel->enableBounds();
	CAPTURE(w.panel->width());
	REQUIRE(sw.width() > 0.0f);
	CHECK(acc.width() > 0.0f);
	CHECK(w.panel->accessory()->isVisible());
	CHECK(acc.right() <= sw.x());
	CHECK(acc.x() >= ui::ExpandedOverlay::kPanelPad
	                     + ui::trackedWidth(ui::font(ui::kControlSize), sk->traits.name, ui::kControlTracking));
}

