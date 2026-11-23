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

// PIECE 10 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("MATH card: its knobs take routes") {
	// ARMED THROUGH THE EDITOR'S OWN `ModState` and driven by each knob's own press-and-drag — the road a
	// hand takes. A, B and Amount are MATH's modulatable controls.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	const MathSeat seat = seatMath(f);
	const backplane::Source m1 = backplane::Source::core(0);
	mod->focus(m1);
	mod->arm(true);
	REQUIRE(mod->armed());

	for (const mth::Param p : { mth::Param::A, mth::Param::B, mth::Param::Amount }) {
		CAPTURE(static_cast<size_t>(p));
		ui::Knob& knob = p == mth::Param::A ? seat.card->a() : p == mth::Param::B ? seat.card->b() : seat.card->amount();
		const backplane::Destination dest = backplane::Destination::control(seat.device, static_cast<size_t>(p));
		REQUIRE(addressOn(seat.device, knob.control()) == static_cast<size_t>(p));
		REQUIRE(f.bridge.mayRoute(m1, dest));
		CHECK(knob.control()->display().routable);
		CHECK(knob.mappable());
		REQUIRE(f.bridge.routeAmount(m1, dest) == 0.0f);
		// ON THE FACE — for Amount that is the left of its row, where `LabelPlacement::Right` puts it.
		const float cx = knob.labelPlacement() == ui::Knob::LabelPlacement::Right ? knob.faceDiameter() * 0.5f
		                                                                          : knob.width() * 0.5f;
		const float cy = knob.labelPlacement() == ui::Knob::LabelPlacement::Right ? knob.height() * 0.5f
		                                                                          : knob.faceDiameter() * 0.5f;
		knob.onMouseDown().callback(at(cx, cy));
		knob.onMouseDrag().callback(at(cx, cy - 60.0f));
		knob.onMouseUp().callback(at(cx, cy - 60.0f));
		CHECK(f.bridge.routeAmount(m1, dest) > 0.05f);
	}
}
TEST_CASE("MATH card is shorter than the generic grid") {
	// MEASURED BOTH WAYS: the card the strip built and what the strip reserved for it, against the generic
	// grid's height for the same device at the same width — the card this one replaced.
	ModFixture f;
	const MathSeat seat = seatMath(f);
	const float generic = ui::GenericCard::heightFor(mth::kDescriptor, ui::kModCardWidth);
	const float compact = seat.card->laneExtent(ui::kModCardWidth);
	MESSAGE("MATH card: generic grid " << generic << "px, compact card " << compact << "px");
	CHECK(compact == doctest::Approx(ui::MathCard::openHeight(1)));
	CHECK(static_cast<float>(seat.card->height()) == doctest::Approx(compact));
	// "MUCH SMALLER": at least a third off, not a trim.
	CHECK(compact < generic * (2.0f / 3.0f));
	// AND NOTHING DRAWN OUTSIDE IT — the second row ends inside the body, above the output strip.
	CHECK(seat.card->transfer().y() + seat.card->transfer().height()
	      <= compact - seat.card->outputStripHeight() + 0.001f);
}
TEST_CASE("a MATH operator menu left open across the card's destruction writes nothing") {
	// visage parents the menu to the ROOT frame and it keeps its selection callback until a pick; the strip
	// can free the card under it. A pick after that must reach no freed card — `MacrosCard`'s case, for
	// this card's dropdowns. Under plain ctest a freed read may not fault; the ASan build is where this
	// earns its keep.
	ModFixture f;
	const MathSeat seat = seatMath(f);
	const visage::PopupMenu held = seat.card->guardedMenuFor(seat.card->op());
	const visage::PopupMenu heldTransfer = seat.card->transferMenu();
	REQUIRE(f.bridge.removeRackDevice(seat.slot));
	f.bridge.refresh();
	f.panel->resized();
	REQUIRE(f.panel->cardFrameForTest(seat.slot) != seat.card);
	REQUIRE(f.panel->retiredCountForTest() > 0);
	f.panel->releaseRetiredCards();
	REQUIRE(f.panel->retiredCountForTest() == 0);
	// THE PICKS ARRIVE LATE, and do nothing.
	held.onSelection().callback(2);
	heldTransfer.onSelection().callback(1);
	CHECK(f.panel->retiredCountForTest() == 0);
}

namespace {
//------------------------------------------------------------------------------------------------//
// THE STEREO TOOL (backlog KU, plan Task 14b) — its scope, its tabs, its controls.
//------------------------------------------------------------------------------------------------//

namespace stt = feathers::mod::stereo;
struct StereoSeat {
	size_t slot = 0;
	DeviceInstanceId device = feathers::kNoInstance;
	ui::StereoToolCard* card = nullptr;
};
/// A STEREO TOOL IN THE STRIP AND THE CARD THE STRIP BUILT — the cast is the claim that the strip builds the
/// tool's own card for the tag rather than the generic grid.
StereoSeat seatStereo(ModFixture& f) {
	StereoSeat seat;
	seat.slot = f.bridge.addRackDevice(stt::kTraits.tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	seat.device = f.bridge.rackDeviceAt(seat.slot);
	REQUIRE(seat.device != feathers::kNoInstance);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::StereoToolCard*>(
	    dynamic_cast<const ui::StereoToolCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
void setStereo(ModFixture& f, const StereoSeat& seat, stt::Param p, double v) {
	seat.card->control(static_cast<size_t>(p)).setValue(v);
	f.bridge.refresh();
}
/// PUBLISH IN'S RIGHT EAR as the engine does while a stereo route reaches it (`publishRackRightEar`).
void publishInRight(ModFixture& f, const StereoSeat& seat, float right) {
	std::array<float, feathers::kMaxDeviceFields> values {};
	values[static_cast<size_t>(stt::Param::In)] = right;
	feathers::device::ControlMask mask {};
	mask.set(static_cast<size_t>(stt::Param::In));
	f.bridge.publishRackRightEar(seat.slot, mask, values);
}
/// THE CONTROLS OF THE STEREO TOOL a frame offers while EFFECTIVELY visible, by descriptor identity — the tooltip
/// guard's `collectOffers`, so the tabs' own view control (no param of the device) is never counted.
std::set<size_t> stereoOffers(const visage::Frame* root) {
	std::set<size_t> out;
	std::vector<ui::ControlFace::Offered> list;
	std::vector<const visage::Frame*> stack { root };
	while (!stack.empty()) {
		const visage::Frame* fr = stack.back();
		stack.pop_back();
		if (!fr->isVisible()) {
			continue;
		}
		if (fr != root) {
			if (const auto* face = dynamic_cast<const ui::ControlFace*>(fr)) {
				list.clear();
				face->offeredControls(list);
				for (const ui::ControlFace::Offered& o : list) {
					for (size_t i = 0; o.control != nullptr && i < stt::kDescriptor.params.size(); ++i) {
						if (&o.control->desc() == &stt::kDescriptor.params[i]) {
							out.insert(i);
						}
					}
				}
			}
		}
		for (const visage::Frame* c : fr->children()) {
			stack.push_back(c);
		}
	}
	return out;
}
} // namespace

TEST_CASE("Stereo Tool card: the scope's two traces are the selected output's two ears, from the device's own arithmetic") {
	// EVERY EXPECTED VALUE IS RESTATED HERE from design §3, in double — never read back from `evaluate`, which is
	// the function under test on both sides of the card.
	ModFixture f;
	const StereoSeat seat = seatStereo(f);
	ui::StereoToolCard& card = *seat.card;
	constexpr double kTol = 1e-6;

	// SPLIT, the default tab, at rest: In 0, so both ears 0.
	REQUIRE(card.shown() == stt::Output::Split);
	card.update();
	CHECK(card.scopeLastForTest().left == 0.0f);
	CHECK(card.scopeLastForTest().right == 0.0f);
	// IN 100%, AMOUNT AT ITS 100% DEFAULT: full inversion, (+1, −1) — "turn the knobs to 100%".
	setStereo(f, seat, stt::Param::In, 1.0);
	card.update();
	CHECK(card.scopeLastForTest().left == 1.0f);
	CHECK(card.scopeLastForTest().right == -1.0f);
	// IN 0.6, AMOUNT 25%: L = i, R = i·(1 − 2a) = 0.6·0.5 = 0.3.
	setStereo(f, seat, stt::Param::In, 0.6);
	setStereo(f, seat, stt::Param::Amount, 0.25);
	card.update();
	CHECK(std::abs(card.scopeLastForTest().left - 0.6) <= kTol);
	CHECK(std::abs(card.scopeLastForTest().right - 0.3) <= kTol);
	// AMOUNT 150%: L = i·a = 0.9, R = −i·a = −0.9.
	setStereo(f, seat, stt::Param::Amount, 1.5);
	card.update();
	CHECK(std::abs(card.scopeLastForTest().left - 0.9) <= kTol);
	CHECK(std::abs(card.scopeLastForTest().right + 0.9) <= kTol);

	// WIDTH over a STEREO In — left 0.2 (the row), right 0.8 (the right ear the engine published) — at 150%:
	// mid 0.5, side 0.3, L = 0.5 − 0.45 = 0.05, R = 0.5 + 0.45 = 0.95.
	setStereo(f, seat, stt::Param::In, 0.2);
	setStereo(f, seat, stt::Param::Width, 1.5);
	publishInRight(f, seat, 0.8f);
	card.show(stt::Output::Width);
	card.update();
	CHECK(std::abs(card.scopeLastForTest().left - 0.05) <= kTol);
	CHECK(std::abs(card.scopeLastForTest().right - 0.95) <= kTol);
	// SPLIT READS IN'S LEFT EAR ONLY: the same stereo In, Amount 150% → (0.3, −0.3), the right ear 0.8 ignored.
	card.show(stt::Output::Split);
	card.update();
	CHECK(std::abs(card.scopeLastForTest().left - 0.3) <= kTol);
	CHECK(std::abs(card.scopeLastForTest().right + 0.3) <= kTol);
	card.show(stt::Output::Width);
	// THE RIGHT EAR IS LOAD-BEARING: with it taken back (mono In), side is 0 and both ears read the mid 0.2.
	f.bridge.publishRackRightEar(seat.slot, {}, {});
	card.update();
	CHECK(std::abs(card.scopeLastForTest().left - 0.2) <= kTol);
	CHECK(std::abs(card.scopeLastForTest().right - 0.2) <= kTol);
	// SWAP exchanges the ears.
	publishInRight(f, seat, 0.8f);
	setStereo(f, seat, stt::Param::Swap, 1.0);
	card.update();
	CHECK(std::abs(card.scopeLastForTest().left - 0.95) <= kTol);
	CHECK(std::abs(card.scopeLastForTest().right - 0.05) <= kTol);
	// AND THE BYPASS: every output rests at 0.
	setStereo(f, seat, stt::Param::Enabled, 0.0);
	card.update();
	CHECK(card.scopeLastForTest().left == 0.0f);
	CHECK(card.scopeLastForTest().right == 0.0f);
}
TEST_CASE("Stereo Tool card: turning the tab changes which output the scope shows, and starts its trace over") {
	ModFixture f;
	const StereoSeat seat = seatStereo(f);
	ui::StereoToolCard& card = *seat.card;
	// ONE SET OF CONTROLS, TWO OUTPUTS: a mono In of 0.4 at Amount 100% splits to (0.4, −0.4); Width over it is
	// (0.4, 0.4) whatever the width.
	setStereo(f, seat, stt::Param::In, 0.4);
	// THE TABS ARE ALL ON THE FACE — two segments, the outputs' own names, neither applied by looking.
	CHECK(card.tabs().segmentAt({ 1.0f, card.tabs().height() * 0.5f }) == 0);
	CHECK(card.tabs().segmentAt({ card.tabs().width() - 1.0f, card.tabs().height() * 0.5f }) == 1);

	for (int frame = 0; frame < 3; ++frame) {
		card.update();
	}
	CHECK(card.scopeCountForTest() == 3u);
	CHECK(std::abs(card.scopeLastForTest().left - 0.4) <= 1e-6);
	CHECK(std::abs(card.scopeLastForTest().right + 0.4) <= 1e-6);

	// A CLICK ON THE WIDTH SEGMENT, through the widget's own hit test.
	card.tabs().select(card.tabs().segmentAt({ card.tabs().width() * 0.75f, card.tabs().height() * 0.5f }));
	REQUIRE(card.shown() == stt::Output::Width);
	CHECK(card.scopeCountForTest() == 0u); // the old output's history is not this one's past
	card.update();
	CHECK(std::abs(card.scopeLastForTest().left - 0.4) <= 1e-6);
	CHECK(std::abs(card.scopeLastForTest().right - 0.4) <= 1e-6);
	// AND IT IS A VIEW, NOT A PARAM: no control of the device moved.
	CHECK(f.bridge.rackFieldAt(seat.slot, static_cast<size_t>(stt::Param::Amount)) == 1.0f);
	CHECK(card.control(static_cast<size_t>(stt::Param::In)).value() == doctest::Approx(0.4));
}
TEST_CASE("Stereo Tool card: it offers every control of the device, each under the output it changes") {
	ModFixture f;
	const StereoSeat seat = seatStereo(f);
	ui::StereoToolCard& card = *seat.card;
	const auto P = [](stt::Param p) { return static_cast<size_t>(p); };

	card.show(stt::Output::Split);
	CHECK(stereoOffers(&card) == std::set<size_t> { P(stt::Param::In), P(stt::Param::Amount) });
	card.show(stt::Output::Width);
	CHECK(stereoOffers(&card) == std::set<size_t> { P(stt::Param::In), P(stt::Param::Width), P(stt::Param::Swap) });

	// EVERY CONTROL THE DEVICE DECLARES IS REACHED: the four on the face across the tabs, the enable by the head.
	std::set<size_t> reached;
	for (const stt::Param p : card.addressedParamsForTest()) {
		reached.insert(P(p));
	}
	REQUIRE(stt::kTraits.enableControl >= 0);
	reached.insert(static_cast<size_t>(stt::kTraits.enableControl));
	std::set<size_t> declared;
	for (size_t i = 0; i < stt::kDescriptor.params.size(); ++i) {
		declared.insert(i);
	}
	CHECK(reached == declared);
	// THE CONTINUOUS ONES ARE KNOBS (they take routes); Swap, two states, is a chip.
	CHECK(addressOn(seat.device, card.amount().control()) == P(stt::Param::Amount));
	CHECK(addressOn(seat.device, card.in().control()) == P(stt::Param::In));
	CHECK(addressOn(seat.device, card.width().control()) == P(stt::Param::Width));
	CHECK(addressOn(seat.device, &card.swap().control()) == P(stt::Param::Swap));
}
TEST_CASE("a generic modulator card in the rail takes routes: M1 armed, a drag on the tracker makes an edge") {
	// **THE STRIP BUILT ITS GENERIC CARDS WITHOUT THE PANEL'S MODULATION STATE**, so
	// `DeviceControl::display` never reported `routable` and a modulatable knob there — the tracker's, and
	// MATH's before it had a card of its own — was drawn as a destination and refused every route. Asked
	// of the card the STRIP built, armed through the editor's own `ModState`, and driven by the knob's own
	// press-and-drag. (MATH's knobs: "MATH card: its knobs take routes".)
	namespace trk = feathers::mod::tracker;
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	/// The first knob on the strip's card for `tag` whose control is a destination, with its control's address.
	struct Seat {
		ui::Knob* knob = nullptr;
		backplane::Destination dest {};
	};
	const auto seat = [&](const char* tag) {
		Seat s;
		const size_t slot = f.bridge.addRackDevice(tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		auto* card = const_cast<ui::GenericCard*>(dynamic_cast<const ui::GenericCard*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		for (const auto& knob : card->knobsForTest()) {
			const auto* road = dynamic_cast<const ui::DeviceControl*>(knob->control());
			if (road != nullptr && knob->control()->display().isDestination) {
				s.knob = knob.get();
				s.dest = backplane::Destination::control(road->device(), road->control());
				break;
			}
		}
		REQUIRE(s.knob != nullptr);
		return s;
	};
	const Seat tracker = seat(trk::kTraits.tag);

	const backplane::Source m1 = backplane::Source::core(0);
	mod->focus(m1);
	mod->arm(true);
	REQUIRE(mod->armed());

	for (const Seat* s : { &tracker }) {
		REQUIRE(f.bridge.mayRoute(m1, s->dest));
		// THE GATE: armed, a destination, and routable — the one answer the knob's press asks.
		CHECK(s->knob->control()->display().routable);
		CHECK(s->knob->mappable());
		// THE GESTURE: a press and an upward drag on the knob write an edge from M1 to it.
		REQUIRE(f.bridge.routeAmount(m1, s->dest) == 0.0f);
		const float cx = s->knob->width() * 0.5f;
		const float cy = s->knob->height() * 0.5f;
		s->knob->onMouseDown().callback(at(cx, cy));
		s->knob->onMouseDrag().callback(at(cx, cy - 60.0f));
		s->knob->onMouseUp().callback(at(cx, cy - 60.0f));
		CHECK(f.bridge.routeAmount(m1, s->dest) > 0.05f);
	}
}

namespace {
/// A mono float WAV at 48 kHz, written by hand.
void writeMonoWav(const std::string& path, const std::vector<float>& x) {
	FILE* f = std::fopen(path.c_str(), "wb");
	REQUIRE(f != nullptr);
	const auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	const auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	const auto bytes = static_cast<uint32_t>(x.size() * 4);
	std::fwrite("RIFF", 1, 4, f);
	u32(36 + bytes);
	std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16);
	u16(3);
	u16(1);
	u32(48000);
	u32(48000 * 4);
	u16(4);
	u16(32);
	std::fwrite("data", 1, 4, f);
	u32(bytes);
	std::fwrite(x.data(), 4, x.size(), f);
	std::fclose(f);
}
} // namespace

TEST_CASE("Featherverb's panel seats the conv controls in its drawer, under the grid, and folds them away") {
	VerbRig rig;
	ui::FeatherverbExpanded& panel = *rig.panel;
	REQUIRE(panel.drawerOpen());
	const visage::Bounds drawer = panel.drawerBoundsForTest();
	for (const fvx::Param p : { fvx::Param::ConvMode, fvx::Param::ConvPredelay, fvx::Param::ConvFeedback }) {
		CAPTURE(static_cast<int>(p));
		visage::Frame* c = rig.cell(p);
		REQUIRE(c != nullptr);
		CHECK(c->isVisible());
		CHECK(c->y() >= drawer.y() + ui::FeatherverbExpanded::kStripHeight + ui::FeatherverbExpanded::kViewHeight);
		CHECK(c->bottom() <= drawer.bottom() + 0.5f);
	}
	// THE GRID ABOVE HOLDS NONE OF THEM: every other visible cell ends above the drawer.
	const auto& params = panel.paramIndicesForTest();
	const auto& cells = panel.cellsForTest();
	for (size_t n = 0; n < params.size(); ++n) {
		const bool conv = params[n] == static_cast<size_t>(fvx::Param::ConvMode)
		                  || params[n] == static_cast<size_t>(fvx::Param::ConvPredelay)
		                  || params[n] == static_cast<size_t>(fvx::Param::ConvFeedback);
		if (!conv && cells[n]->isVisible()) {
			CAPTURE(params[n]);
			CHECK(cells[n]->bottom() <= drawer.y() + 0.5f);
		}
	}

	const float openHeight = panel.minimumSize().y;
	panel.setDrawerOpen(false);
	CHECK(openHeight - panel.minimumSize().y
	      == doctest::Approx(ui::FeatherverbExpanded::kViewHeight + panel.cellHeightForTest() + 8.0f + 12.0f));
	CHECK_FALSE(rig.cell(fvx::Param::ConvMode)->isVisible());
	CHECK_FALSE(rig.cell(fvx::Param::ConvFeedback)->isVisible());
	CHECK_FALSE(panel.irViewForTest().isVisible());
}
TEST_CASE("Featherverb's drawer greys Predelay, Feedback and Stretch while Conv is Off — they change nothing then") {
	VerbRig rig;
	auto* feedback = dynamic_cast<ui::Knob*>(rig.cell(fvx::Param::ConvFeedback));
	REQUIRE(feedback != nullptr);
	CHECK_FALSE(feedback->enabled());
	CHECK_FALSE(rig.panel->stretchKnobForTest().enabled());
	rig.bridge->setField(rig.id, static_cast<size_t>(fvx::Param::ConvMode), static_cast<float>(fvx::kConvParallel));
	(void)rig.panel->updateContent();
	CHECK(feedback->enabled());
	CHECK(rig.panel->stretchKnobForTest().enabled());
}
TEST_CASE("Featherverb's drawer: a dropped file loads into the device, the strip names it, a marker drag commits on release") {
	VerbRig rig;
	CHECK(rig.panel->stripTextForTest() == "Generated · Room");
	const std::string path = "test_fv_drawer.wav";
	std::vector<float> x(4800);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(std::exp(-static_cast<double>(i) / 800.0)) * ((i % 2) != 0u ? -1.0f : 1.0f);
	}
	writeMonoWav(path, x);
	ui::IrView& view = rig.panel->irViewForTest();
	view.dropFiles({ path });
	std::remove(path.c_str());
	CHECK(rig.verb().irPath() == path);
	(void)rig.panel->updateContent();
	CHECK(rig.panel->stripTextForTest() == path);

	// THE END MARKER, DRAGGED FROM THE RIGHT EDGE TO THE MIDDLE: nothing reaches the device until release.
	const visage::Bounds span = view.spanBoundsForTest();
	const float endX = span.right();
	const float midX = span.x() + 0.5f * span.width();
	const float y = span.y() + 0.5f * span.height();
	view.onMouseDown().callback(at(endX, y));
	view.onMouseDrag().callback(at(midX, y));
	CHECK(rig.verb().irTrim().end == 1.0f);           // the drag previews
	CHECK(view.previewForTest().end == doctest::Approx(0.5f).epsilon(0.02));
	view.onMouseUp().callback(at(midX, y));
	CHECK(rig.verb().irTrim().end == doctest::Approx(0.5f).epsilon(0.02)); // the release commits
}
TEST_CASE("PHOTO: Featherverb's panel with the conv drawer open, an IR loaded and shaped (writes only under FEATHERS_CONV_DRAWER_SHOT)") {
	const char* path = std::getenv("FEATHERS_CONV_DRAWER_SHOT");
	if (path == nullptr) {
		return;
	}
	VerbRig rig;
	const std::string wav = "test_fv_drawer_shot.wav";
	std::vector<float> x(48000 * 2);
	uint32_t s = 0x1234u;
	for (size_t i = 0; i < x.size(); ++i) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		const double env = std::exp(-static_cast<double>(i) / 18000.0);
		x[i] = static_cast<float>(env * (static_cast<double>(s & 0xffff) / 32768.0 - 1.0));
	}
	writeMonoWav(wav, x);
	REQUIRE(rig.verb().loadIr(wav));
	std::remove(wav.c_str());
	rig.bridge->setField(rig.id, static_cast<size_t>(fvx::Param::ConvMode), static_cast<float>(fvx::kConvParallel));
	feathers::mod::mseg::Settings env;
	env.startLevel = 1.0f;
	env.count = 2;
	env.segment[0] = { -1.0f, 0.7f, 0.5f };
	env.segment[1] = { -1.0f, 0.1f, -0.4f };
	REQUIRE(rig.verb().setIrEnvelope(env));
	REQUIRE(rig.verb().setIrTrim({ 0.05f, 0.8f }));
	(void)rig.panel->updateContent();
	rig.panel->resized();
	// A REAL WINDOWLESS EDITOR draws the whole tree, as the lane's camera case does — every child in its
	// own region, which no hand-rolled walk reproduces.
	const int w = static_cast<int>(std::ceil(rig.panel->width()));
	const int h = static_cast<int>(std::ceil(rig.panel->height()));
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.addChild(rig.panel.get());
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		editor.removeChild(rig.panel.get());
		return;
	}
	editor.redrawAll();
	editor.drawWindow();
	editor.drawWindow();
	editor.takeScreenshot().save(path);
	editor.removeChild(rig.panel.get());
}
TEST_CASE("Featherverb's Load IR... and Choose folder... open the OS's dialogs, and the answer reaches the device") {
	namespace fs = std::filesystem;
	const fs::path home = fs::absolute(fs::path("irdialog_home"));
	fs::remove_all(home);
	const fs::path irRoot = home / "IRs";
	const fs::path lib = home / "Library";
	fs::create_directories(lib / "Halls");
	fs::create_directories(irRoot);
	ScopedEnv scopedHome("HOME", home.string().c_str());
	ScopedEnv scopedRoot("FEATHERS_IR_ROOT", irRoot.string().c_str());
	std::vector<float> x(2400);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(std::exp(-static_cast<double>(i) / 400.0));
	}
	writeMonoWav((lib / "Plate A.wav").string(), x);
	writeMonoWav((lib / "Halls" / "Big.wav").string(), x);
	{
		FILE* bad = std::fopen((lib / "Broken.wav").string().c_str(), "wb");
		REQUIRE(bad != nullptr);
		std::fputs("not audio", bad);
		std::fclose(bad);
	}

	FakeIrDialogs dialogs;
	ui::ScopedFileDialogBackend useFake(dialogs);
	VerbRig rig;
	ui::ItemStepper& stepper = rig.panel->stepperForTest();
	using Status = ui::FileDialogResult::Status;

	// LOAD IR...: an OPEN-FILE dialog, audio only, starting where the browser walks (the fixed folder, on a room).
	stepper.choose(ui::ItemStepper::kActionBase + ui::FeatherverbExpanded::kLoadIrAction);
	REQUIRE(dialogs.starts == 1);
	CHECK(dialogs.last.mode == ui::FileDialogOptions::Mode::OpenFile);
	CHECK(dialogs.last.startDir == irRoot);
	REQUIRE(dialogs.last.filters.size() == 1);
	CHECK(dialogs.last.filters[0].extensions == std::vector<std::string> { "wav", "aif", "aiff", "flac" });
	dialogs.answer(*rig.panel, { Status::Chosen, lib / "Plate A.wav", {} });
	CHECK(rig.verb().irPath() == (lib / "Plate A.wav").string());
	(void)rig.panel->updateContent();
	CHECK(rig.panel->irListForTest()[5] == "X  Plate A.wav"); // the browser now walks that file's folder

	// A FILE THAT WILL NOT LOAD changes nothing and says why; a CANCEL says nothing.
	stepper.choose(ui::ItemStepper::kActionBase + ui::FeatherverbExpanded::kLoadIrAction);
	CHECK(dialogs.last.startDir == lib); // the folder of what is loaded now
	dialogs.answer(*rig.panel, { Status::Chosen, lib / "Broken.wav", {} });
	CHECK(rig.verb().irPath() == (lib / "Plate A.wav").string());
	CHECK(rig.panel->stripTextForTest().rfind("could not load", 0) == 0);
	stepper.choose(ui::ItemStepper::kActionBase + ui::FeatherverbExpanded::kLoadIrAction);
	dialogs.answer(*rig.panel, { Status::Cancelled, {}, {} });
	CHECK(rig.verb().irPath() == (lib / "Plate A.wav").string());

	// CHOOSE FOLDER...: an OPEN-FOLDER dialog, and the folder's first impulse that loads.
	stepper.choose(ui::ItemStepper::kActionBase + ui::FeatherverbExpanded::kChooseFolderAction);
	CHECK(dialogs.last.mode == ui::FileDialogOptions::Mode::OpenFolder);
	CHECK(rig.panel->pendingFolderMenuForTest().empty()); // the OS's picker, not the menu
	dialogs.answer(*rig.panel, { Status::Chosen, lib / "Halls", {} });
	CHECK(rig.verb().irPath() == (lib / "Halls" / "Big.wav").string());
	CHECK(dialogs.starts == 4);

	// NO DIALOG ON THIS DESKTOP: Choose folder... is the menu again, and Load IR... is greyed in the list.
	dialogs.canShow = false;
	stepper.choose(ui::ItemStepper::kActionBase + ui::FeatherverbExpanded::kChooseFolderAction);
	CHECK(dialogs.starts == 4);
	CHECK(rig.panel->pendingFolderMenuForTest() == (lib / "Halls").string());
	bool loadGreyed = false;
	for (const ui::ItemStepper::Row& row : stepper.listRows()) {
		if (row.label == "Load IR...") {
			loadGreyed = !row.enabled;
		}
	}
	CHECK(loadGreyed);
	fs::remove_all(home);
}
TEST_CASE("Featherverb's presets: the shared browser in the title, a preset carries the IR, and Init is the generated Room") {
	namespace fs = std::filesystem;
	namespace conv = feathers::fx::conv;
	const fs::path dir = fs::absolute(fs::path("fv_preset_ir"));
	fs::remove_all(dir);
	fs::create_directories(dir);
	std::vector<float> x(2400);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(std::exp(-static_cast<double>(i) / 400.0));
	}
	const std::string wav = (dir / "Hall.wav").string();
	writeMonoWav(wav, x);

	VerbRig rig;
	// THE SHARED BROWSER (SKLITTER's), in the panel's title row.
	CHECK(rig.panel->titleAccessory() == &rig.panel->presetBrowserForTest());

	// A PRESET CARRIES THE BODY: the file by name, its markers, and a control.
	REQUIRE(rig.verb().loadIr(wav));
	REQUIRE(rig.verb().setIrTrim({ 0.1f, 0.7f }));
	rig.bridge->setField(rig.id, static_cast<size_t>(fvx::Param::Room), 0.9f);
	std::vector<uint8_t> saved;
	REQUIRE(captureDevicePreset(*rig.bridge, fvx::kDescriptor, rig.id, &saved));
	DevicePresetContents contents;
	REQUIRE(readDevicePreset(saved, &contents));
	CHECK_FALSE(contents.body.empty());

	// MOVE AWAY FROM IT — a generated room, a full span, another Room — and APPLY: all three come back.
	rig.verb().useGeneratedIr(conv::kGeneratedPlate);
	REQUIRE(rig.verb().setIrTrim({ 0.0f, 1.0f }));
	rig.bridge->setField(rig.id, static_cast<size_t>(fvx::Param::Room), 0.2f);
	REQUIRE(applyDevicePreset(*rig.bridge, fvx::kDescriptor, rig.id, saved, {}) == DevicePresetLoad::Loaded);
	CHECK(rig.verb().irPath() == wav);
	CHECK(rig.verb().irShape().begin == doctest::Approx(0.1f));
	CHECK(rig.verb().irShape().end == doctest::Approx(0.7f));
	CHECK(rig.bridge->fieldAt(rig.id, static_cast<size_t>(fvx::Param::Room)) == doctest::Approx(0.9f));

	// INIT: the declared defaults AND the device's own IR — the generated Room, a full span.
	REQUIRE(applyDevicePreset(*rig.bridge, fvx::kDescriptor, rig.id, initDevicePreset(fvx::kDescriptor), {})
	        == DevicePresetLoad::Loaded);
	CHECK(rig.verb().irPath().empty());
	CHECK(rig.verb().generatedRoom() == conv::kGeneratedRoom);
	CHECK(rig.verb().irShape().begin == 0.0f);
	CHECK(rig.verb().irShape().end == 1.0f);
	fs::remove_all(dir);
}
TEST_CASE("Featherverb's Oversample is a switch in a cell of its own, hidden in Aurora where it changes nothing") {
	VerbRig rig;
	auto* chip = dynamic_cast<ui::SwitchChip*>(rig.cell(fvx::Param::Oversample));
	REQUIRE(chip != nullptr);
	CHECK(chip->label() == "Oversample");
	(void)rig.panel->updateContent();
	rig.panel->resized();
	CHECK(chip->isVisible());
	// NOT SEATED INSIDE THE SIZE CELL as a gated control otherwise would be: a box of its own, beside Character.
	const visage::Frame* character = rig.cell(fvx::Param::Character);
	REQUIRE(character != nullptr);
	CHECK(chip->parent() == character->parent());
	CHECK(chip->width() > 40.0f);

	// A PRESS TURNS IT ON, through the device's own road.
	CHECK(rig.bridge->fieldAt(rig.id, static_cast<size_t>(fvx::Param::Oversample)) == 0.0f);
	chip->toggleForTest();
	CHECK(rig.bridge->fieldAt(rig.id, static_cast<size_t>(fvx::Param::Oversample)) == 1.0f);

	// AURORA: gone, and back with the next mode.
	rig.bridge->setField(rig.id, static_cast<size_t>(fvx::Param::SizeMode), static_cast<float>(fvx::kAuroraMenu));
	(void)rig.panel->updateContent();
	rig.panel->resized();
	CHECK_FALSE(chip->isVisible());
	rig.bridge->setField(rig.id, static_cast<size_t>(fvx::Param::SizeMode), static_cast<float>(fvx::kVastMenu));
	(void)rig.panel->updateContent();
	rig.panel->resized();
	CHECK(chip->isVisible());
}
TEST_CASE("Featherverb's drawer browses: the generated rooms, then the folder; skipping what will not load; and back again") {
	namespace fs = std::filesystem;
	// A SCRATCH HOME, so the fixed folder (`~/Feathers/IRs`, made on first use) is never made in a real one.
	const fs::path home = fs::absolute(fs::path("irbrowse_home"));
	fs::remove_all(home);
	fs::create_directories(home);
	ScopedEnv scopedHome("HOME", home.string().c_str());
	const fs::path irRoot = home / "IRs";
	ScopedEnv scopedRoot("FEATHERS_IR_ROOT", irRoot.string().c_str()); // the override presets-2 mirrors
	const fs::path dir = fs::path("irbrowse_drawer");
	fs::remove_all(dir);
	fs::create_directories(dir);
	std::vector<float> x(2400);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(std::exp(-static_cast<double>(i) / 400.0));
	}
	writeMonoWav((dir / "A.wav").string(), x);
	{
		FILE* bad = std::fopen((dir / "B.wav").string().c_str(), "wb"); // a .wav that is not one
		REQUIRE(bad != nullptr);
		std::fputs("not audio", bad);
		std::fclose(bad);
	}
	writeMonoWav((dir / "C.wav").string(), x);

	FakeIrDialogs dialogs; // the list asks whether a dialog can be shown; never the real desktop
	ui::ScopedFileDialogBackend useFake(dialogs);
	VerbRig rig;
	// THE SHARED BROWSER ROW (presets-2's ItemStepper, backlog JH), pressed as a hand does, in its own coordinates.
	visage::Frame& strip = rig.panel->stepperForTest();
	const auto press = [&](float x) { strip.onMouseDown().callback(at(x, strip.height() * 0.5f)); };
	// ON THE ROOM, THE ARROWS WALK THE GENERATED ROOMS — there is always somewhere to go.
	CHECK(rig.panel->canBrowse());
	press(ui::FeatherverbExpanded::nextArrowXForTest());
	CHECK(rig.verb().irPath().empty());
	CHECK(rig.verb().generatedRoom() == feathers::fx::conv::kGeneratedPlate);
	CHECK(rig.panel->drawerOpen()); // an arrow is not a fold
	CHECK(fs::exists(irRoot)); // the fixed folder (here the override), made for the files to go in

	// A FILE: ▶ SKIPS THE BROKEN B TO C, ▶ AGAIN WRAPS INTO THE ROOMS, ◀ COMES BACK TO C — the folder remembered.
	REQUIRE(rig.verb().loadIr((dir / "A.wav").string()));
	(void)rig.panel->updateContent(); // the panel's tick: a new IR re-reads the browser's list
	press(ui::FeatherverbExpanded::nextArrowXForTest());
	CHECK(fs::path(rig.verb().irPath()).filename() == "C.wav");
	press(ui::FeatherverbExpanded::nextArrowXForTest());
	CHECK(rig.verb().irPath().empty());
	CHECK(rig.verb().generatedRoom() == feathers::fx::conv::kGeneratedRoom);
	press(ui::FeatherverbExpanded::prevArrowXForTest());
	CHECK(fs::path(rig.verb().irPath()).filename() == "C.wav");

	// THE LIST: the rooms, then the folder, the current one marked.
	const std::vector<std::string> rows = rig.panel->irListForTest();
	CHECK(rows == std::vector<std::string> { "   Generated · Room", "   Generated · Plate", "   Generated · Hall",
	                                          "   Generated · Ambience", "   A.wav", "   B.wav", "X  C.wav",
	                                          "   Load IR...", "   Choose folder..." });
	fs::remove_all(dir);
	fs::remove_all(home);
}
TEST_CASE("Featherverb's browser chooses a folder: the list's verb opens a menu that walks the disk, and Use loads the folder's first impulse") {
	namespace fs = std::filesystem;
	namespace conv = feathers::fx::conv;
	using Kind = conv::FolderRow::Kind;
	const fs::path home = fs::absolute(fs::path("irfolder_home"));
	fs::remove_all(home);
	const fs::path irRoot = home / "IRs";
	const fs::path base = home / "Library";
	fs::create_directories(base / "Halls 10");
	fs::create_directories(base / "Halls 2");
	fs::create_directories(base / "Empty");
	fs::create_directories(base / ".cache");
	fs::create_directories(irRoot);
	ScopedEnv scopedHome("HOME", home.string().c_str());
	ScopedEnv scopedRoot("FEATHERS_IR_ROOT", irRoot.string().c_str());
	std::vector<float> x(2400);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(std::exp(-static_cast<double>(i) / 400.0));
	}
	writeMonoWav((base / "Z.wav").string(), x);
	{
		FILE* bad = std::fopen((base / "Halls 2" / "A.wav").string().c_str(), "wb"); // first by name, and broken
		REQUIRE(bad != nullptr);
		std::fputs("not audio", bad);
		std::fclose(bad);
	}
	writeMonoWav((base / "Halls 2" / "B.wav").string(), x);
	writeMonoWav((base / "Halls 2" / "C.wav").string(), x);

	// THE MENU, AS A LITERAL: where it is, Use with the count, up, the subfolders in natural order ("2" before "10") with
	// the hidden one left out, then the two places to jump to.
	const std::vector<conv::FolderRow> rows = conv::folderRows(base.string(), irRoot.string(), home.string());
	const std::vector<conv::FolderRow> expected {
		{ Kind::Header, "~/Library", {}, false },
		{ Kind::Use, "Use this folder (1 impulse)", base.string(), true },
		{ Kind::Break, {}, {}, false },
		{ Kind::Enter, "Up: irfolder_home", home.string(), true },
		{ Kind::Enter, "Empty/", (base / "Empty").string(), true },
		{ Kind::Enter, "Halls 2/", (base / "Halls 2").string(), true },
		{ Kind::Enter, "Halls 10/", (base / "Halls 10").string(), true },
		{ Kind::Break, {}, {}, false },
		{ Kind::Enter, "Impulse folder", irRoot.string(), true },
		{ Kind::Enter, "Home", home.string(), true },
	};
	CHECK(rows == expected);
	// AN EMPTY FOLDER CANNOT BE USED; AT HOME, "Home" IS NOT OFFERED AGAIN.
	const auto empty = conv::folderRows((base / "Empty").string(), irRoot.string(), home.string());
	REQUIRE(empty.size() > 1);
	CHECK(empty[1].label == "Use this folder (no impulses)");
	CHECK_FALSE(empty[1].enabled);
	const auto atHome = conv::folderRows(home.string(), irRoot.string(), home.string());
	CHECK(std::none_of(atHome.begin(), atHome.end(), [](const conv::FolderRow& r) { return r.label == "Home"; }));

	FakeIrDialogs dialogs; // NO OS DIALOG: the menu is the fallback this case walks
	dialogs.canShow = false;
	ui::ScopedFileDialogBackend useFake(dialogs);
	VerbRig rig;
	// THE LIST'S VERB asks for the menu at the folder the browser walks — on a generated room, the fixed one.
	rig.panel->stepperForTest().choose(ui::ItemStepper::kActionBase + ui::FeatherverbExpanded::kChooseFolderAction);
	CHECK(rig.panel->pendingFolderMenuForTest() == irRoot.string());
	(void)rig.panel->updateContent(); // the next tick opens it
	CHECK(rig.panel->pendingFolderMenuForTest().empty());

	// USE: the folder's first impulse THAT LOADS (A is broken), and the arrows then walk that folder.
	CHECK(rig.panel->useFolder((base / "Halls 2").string()));
	CHECK(fs::path(rig.verb().irPath()).filename() == "B.wav");
	(void)rig.panel->updateContent();
	CHECK(rig.panel->irListForTest() == std::vector<std::string> { "   Generated · Room", "   Generated · Plate",
	                                                                 "   Generated · Hall", "   Generated · Ambience",
	                                                                 "   A.wav", "X  B.wav", "   C.wav",
	                                                                 "   Load IR...", "   Choose folder..." });
	// A FOLDER WHERE NOTHING LOADS changes nothing, and says so.
	CHECK_FALSE(rig.panel->useFolder((base / "Empty").string()));
	CHECK(fs::path(rig.verb().irPath()).filename() == "B.wav");
	CHECK(rig.panel->stripTextForTest().find("nothing in Empty loads") != std::string::npos);
	fs::remove_all(home);
}

namespace {
//------------------------------------------------------------------------------------------------//
// THE ADD MENUS, FILED BY CATEGORY.                                             //
//------------------------------------------------------------------------------------------------//

/// **THE MENU A LANE WOULD SHOW, AGAINST AN INDEPENDENT RESTATEMENT OF THE RULE** — not against
/// `groupByCategory`, which is the code under test. For every category in declaration order, the
/// offered devices that declare it, names sorted case-insensitively; a category with none is absent;
/// an offer that is one category is flat. Then every row's id must name the offered device whose name
/// it prints, every offered device must appear exactly once, and every row must be `enabled`.
void checkFiledByCategory(const visage::PopupMenu& menu, const std::vector<const device::Descriptor*>& offered,
                          bool enabled) {
	const auto lowered = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	};
	std::vector<std::pair<std::string, std::vector<std::string>>> expected;
	// THE REAL CATEGORIES IN DECLARATION ORDER (`None` is 0), THEN `None` LAST.
	std::vector<device::Category> order;
	for (size_t c = 1; c < device::kCategoryCount; ++c) {
		order.push_back(static_cast<device::Category>(c));
	}
	order.push_back(device::Category::None);
	for (const device::Category cat : order) {
		std::vector<std::string> names;
		for (const device::Descriptor* d : offered) {
			if (d->category == cat) {
				names.emplace_back(d->traits.name);
			}
		}
		std::stable_sort(names.begin(), names.end(),
		                 [&](const std::string& a, const std::string& b) { return lowered(a) < lowered(b); });
		// THE SHORTCUTS (`device_shortcuts.h`) follow the devices, in the category of the device they add.
		for (const ui::AddShortcut& shortcut : ui::kAddShortcuts) {
			for (const device::Descriptor* d : offered) {
				if (d->category == cat && std::string(d->traits.tag) == shortcut.tag) {
					names.emplace_back(shortcut.name);
				}
			}
		}
		if (!names.empty()) {
			expected.push_back({ device::categoryName(cat), names });
		}
	}
	REQUIRE_FALSE(expected.empty());

	// THE MENU'S OWN ROWS, preamble (a disabled note with no id, a break) skipped.
	std::vector<const visage::PopupMenu*> top;
	for (const visage::PopupMenu& o : menu.options()) {
		if (!o.isBreak() && !(o.id() < 0 && !o.hasOptions())) {
			top.push_back(&o);
		}
	}
	std::vector<int> seen(offered.size(), 0);
	const auto checkItem = [&](const visage::PopupMenu& item, const std::string& name) {
		CAPTURE(name);
		CHECK_FALSE(item.hasOptions());
		CHECK(item.name().toUtf8() == name);
		REQUIRE(item.id() >= 0);
		if (const ui::AddShortcut* shortcut = ui::shortcutOfId(offered.size(), item.id()); shortcut != nullptr) {
			CHECK(std::string(shortcut->name) == name); // a shortcut: a device set up on the way in
			CHECK(item.enabled() == enabled);
			return;
		}
		REQUIRE(static_cast<size_t>(item.id()) < offered.size());
		CHECK(std::string(offered[static_cast<size_t>(item.id())]->traits.name) == name);
		CHECK(item.enabled() == enabled);
		++seen[static_cast<size_t>(item.id())];
	};
	if (expected.size() == 1) {
		// ONE CATEGORY: FLAT.
		const auto& names = expected.front().second;
		REQUIRE(top.size() == names.size());
		for (size_t i = 0; i < names.size(); ++i) {
			checkItem(*top[i], names[i]);
		}
	}
	else {
		REQUIRE(top.size() == expected.size());
		for (size_t g = 0; g < expected.size(); ++g) {
			const visage::PopupMenu& sub = *top[g];
			CAPTURE(expected[g].first);
			CHECK(sub.name().toUtf8() == expected[g].first);
			REQUIRE(sub.hasOptions());
			// A HEADING IS NEVER GREYED — a disabled one cannot be hovered open, and a full lane must
			// still show what it could have had.
			CHECK(sub.enabled());
			const auto& names = expected[g].second;
			REQUIRE(sub.options().size() == names.size());
			for (size_t i = 0; i < names.size(); ++i) {
				checkItem(sub.options()[i], names[i]);
			}
		}
	}
	for (size_t i = 0; i < offered.size(); ++i) {
		CAPTURE(offered[i]->traits.name);
		CHECK(seen[i] == 1);
	}
}
/// The row a hand would click for `name`, at any depth — or -1.
int menuIdOf(const visage::PopupMenu& menu, const std::string& name) {
	for (const visage::PopupMenu& o : menu.options()) {
		if (o.hasOptions()) {
			const int inner = menuIdOf(o, name);
			if (inner >= 0) {
				return inner;
			}
		}
		else if (o.id() >= 0 && o.name().toUtf8() == name) {
			return o.id();
		}
	}
	return -1;
}
/// Every device name a menu offers, in the order a hand meets them.
void menuNames(const visage::PopupMenu& menu, std::vector<std::string>& out) {
	for (const visage::PopupMenu& o : menu.options()) {
		if (o.hasOptions()) {
			menuNames(o, out);
		}
		else if (o.id() >= 0) {
			out.push_back(o.name().toUtf8());
		}
	}
}
/// The registry's tag for a device NAME — independent of any menu id.
const char* tagOfName(const std::string& name) {
	for (const ui::AddShortcut& shortcut : ui::kAddShortcuts) {
		if (name == shortcut.name) {
			return shortcut.tag; // a shortcut adds the device it is a way in for
		}
	}
	for (const device::Descriptor& d : feathers::kDevices) {
		if (name == d.traits.name) {
			return d.traits.tag;
		}
	}
	return nullptr;
}
} // namespace

TEST_CASE("the fx lane's add menu is filed by category, and picking a row adds exactly that device") {
	std::vector<std::string> names;
	{
		FilterFixture f;
		const auto offered = f.lane->addableForTest();
		REQUIRE(offered.size() >= 8);
		const visage::PopupMenu menu = f.lane->addMenuForTest();
		checkFiledByCategory(menu, offered, true);
		// AND THE MODULATORS HEADING IS THERE — INVERTED 2026-10-04. It said "audio devices only, so the
		// Modulators heading is not there at all"; the lane takes modulators now (every mod effect is in
		// scope in the Feathers fx rack), so they are filed under their own heading like any category,
		// and the per-row loop below picks each one and finds it in the chain.
		const auto headings = headingsOf(menu);
		CHECK(std::find(headings.begin(), headings.end(), "Modulators") != headings.end());
		CHECK(std::find(headings.begin(), headings.end(), "Filter & EQ") != headings.end());
		menuNames(menu, names);
		// AND THE "Add before…" ROAD IS THE SAME MENU.
		checkFiledByCategory(f.lane->addMenuForTest(0), offered, true);
	}
	REQUIRE(names.size() >= 8);
	// EVERY ROW, EACH IN A FRESH EDITOR: the device the chain gained is the one the ROW NAMES, looked
	// up by name in the registry, not by the id the menu carries.
	for (const std::string& name : names) {
		CAPTURE(name);
		FilterFixture f;
		const size_t before = f.lane->deviceCount();
		const visage::PopupMenu menu = f.lane->addMenuForTest();
		const int id = menuIdOf(menu, name);
		REQUIRE(id >= 0);
		menu.onSelection().callback(id);
		CHECK(f.lane->deviceCount() == before + 1);
		CHECK(fxHolds(f.bridge.fxChain(), before, tagOfName(name)));
		// A SHORTCUT'S DEVICE ARRIVES SET UP: the Rack in the view and routing the row names.
		for (const ui::AddShortcut& shortcut : ui::kAddShortcuts) {
			if (name == shortcut.name) {
				const feathers::DeviceInstanceId made = f.bridge.fxDeviceAt(before);
				CHECK(f.deviceHost.fieldAt(made, shortcut.view) == shortcut.viewValue);
				CHECK(f.deviceHost.fieldAt(made, shortcut.mode) == shortcut.modeValue);
			}
		}
	}
	// AN INSERT FROM THE CARD'S MENU LANDS BEFORE THAT CARD.
	{
		FilterFixture f;
		REQUIRE(f.lane->deviceCount() == 1);
		const visage::PopupMenu menu = f.lane->addMenuForTest(0);
		menu.onSelection().callback(menuIdOf(menu, "Delay"));
		CHECK(f.lane->deviceCount() == 2);
		CHECK(fxHolds(f.bridge.fxChain(), 0, fx::delay::kTraits.tag));
		CHECK(fxHolds(f.bridge.fxChain(), 1, fx::filter::kTraits.tag));
	}
}
TEST_CASE("an fx lane past its starting size still files its menu by category, every device live") {
	// IT GREYED EVERY DEVICE HERE until unbounded devices (step 6), when the lane stopped at eight. A lane is
	// never full now, so the menu past that size is the ordinary one, and a pick from it lands.
	FilterFixture f;
	const auto offered = f.lane->addableForTest();
	while (f.lane->deviceCount() < feathers::kMaxFxSlots) {
		REQUIRE(f.lane->addChoiceForTest(0) != feathers::kNoPosition);
	}
	const visage::PopupMenu menu = f.lane->addMenuForTest();
	checkFiledByCategory(menu, offered, true);
	menu.onSelection().callback(menuIdOf(menu, "Delay"));
	CHECK(f.lane->deviceCount() == feathers::kMaxFxSlots + 1);
}
TEST_CASE("the modulation rack's add menu files two categories, Modulators then Note FX, each alphabetical") {
	ModFixture f;
	const auto offered = f.panel->addableForTest();
	REQUIRE(offered.size() >= 4);
	const visage::PopupMenu menu = f.panel->addMenuForTest();
	checkFiledByCategory(menu, offered, true);
	// TWO HEADINGS SINCE 2026-11-04 (the arps get their own "note" category, in scope for the modulation rack).
	CHECK(headingsOf(menu) == std::vector<std::string> { "Modulators", "Note FX" });
	std::vector<std::string> names;
	menuNames(menu, names);
	for (const std::string& name : names) {
		CAPTURE(name);
		ModFixture each;
		const size_t before = each.panel->deviceCount();
		const visage::PopupMenu m = each.panel->addMenuForTest();
		m.onSelection().callback(menuIdOf(m, name));
		CHECK(each.panel->deviceCount() == before + 1);
		// THE PICK LANDS AT THE END — or, for a device that emits notes with "Arps sent to front of chain" on (its default),
		// at the FRONT (`emitters_front.h`).
		const feathers::device::Descriptor* picked = feathers::deviceForTag(tagOfName(name));
		const size_t at = picked != nullptr && ui::emitsNotes(*picked) && ui::emittersToFront() ? 0 : before;
		CHECK(each.bridge.modRack().holds(at, tagOfName(name)));
	}
}
TEST_CASE("a chain column's add menu is filed by category — effects AND modulators — and a pick adds that device") {
	std::vector<std::string> names;
	{
		HeldChain held;
		ui::WidgetRegistry registry;
		ui::ModState mod;
		ui::EditHistory history;
		ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
		column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);
		const auto offered = column.addable();
		const visage::PopupMenu menu = column.addMenuForTest();
		checkFiledByCategory(menu, offered, true);
		const auto headings = headingsOf(menu);
		CHECK(std::find(headings.begin(), headings.end(), "Modulators") != headings.end());
		CHECK(std::find(headings.begin(), headings.end(), "Rhythm & Glitch") != headings.end());
		menuNames(menu, names);
	}
	REQUIRE(names.size() >= 8);
	for (const std::string& name : names) {
		CAPTURE(name);
		HeldChain held;
		ui::WidgetRegistry registry;
		ui::ModState mod;
		ui::EditHistory history;
		ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
		column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 600);
		const visage::PopupMenu menu = column.addMenuForTest();
		menu.onSelection().callback(menuIdOf(menu, name));
		REQUIRE(held.bridge().chainLength(0) == 1);
		const device::Descriptor* got = held.bridge().descriptorOf(held.bridge().deviceAtPosition(0));
		REQUIRE(got != nullptr);
		// A SHORTCUT ADDS THE DEVICE IT IS A WAY IN FOR (the Rack), set up on the way in.
		const char* expectedTag = tagOfName(name);
		CHECK(std::string(got->traits.tag) == expectedTag);
		for (const ui::AddShortcut& shortcut : ui::kAddShortcuts) {
			if (name == shortcut.name) {
				const feathers::DeviceInstanceId made = held.bridge().deviceAtPosition(0);
				CHECK(held.bridge().fieldAt(made, shortcut.view) == shortcut.viewValue);
				CHECK(held.bridge().fieldAt(made, shortcut.mode) == shortcut.modeValue);
			}
		}
	}
}

namespace {
/// THE GATE TABLE, RESTATED FROM THE DESCRIPTOR — what a control's visibility must equal in `mode`.
template <size_t N>
bool gateSays(const feathers::device::ParamDesc (&descs)[N], size_t param, int mode) {
	return feathers::device::relevantFor(descs[param], static_cast<double>(mode));
}
} // namespace

TEST_CASE("COMP's card shows each mode's knobs, hides the rest, and closes up the row") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;

	const auto shown = [&](cp::Param p) {
		ui::Knob* k = card->knobForTest(p);
		REQUIRE(k != nullptr);
		return k->isVisible();
	};
	const auto checkAgainstGates = [&](int mode) {
		for (const cp::Param p : card->addressedParamsForTest()) {
			if (ui::Knob* k = card->knobForTest(p)) {
				CAPTURE(std::string(cp::kParamDescs[static_cast<size_t>(p)].name));
				CAPTURE(mode);
				CHECK(k->isVisible() == gateSays(cp::kParamDescs, static_cast<size_t>(p), mode));
			}
		}
		CHECK(card->detectForTest()->isVisible()
		      == gateSays(cp::kParamDescs, static_cast<size_t>(cp::Param::Detect), mode));
		// THE SIDECHAIN HIGH-PASS IS LIVE IN BOTH MODES.
		CHECK(card->scHpfForTest()->isVisible());
		CHECK(card->modeForTest()->isVisible());
	};

	// COMPRESS, THE DEFAULT.
	REQUIRE(static_cast<int>(cp::kParamDescs[static_cast<size_t>(cp::Param::Mode)].def) == 0);
	CHECK(shown(cp::Param::Threshold));
	CHECK_FALSE(shown(cp::Param::Ceiling));
	CHECK(shown(cp::Param::Drive)); // backlog MI: Drive is live in Compress too, and opens the grid
	checkAgainstGates(0);
	const float firstCellX = card->knobForTest(cp::Param::Drive)->x();
	const float secondCellX = card->knobForTest(cp::Param::Threshold)->x();
	CHECK(secondCellX > firstCellX);

	// LIMIT — written to the FIELD, the road automation and undo take, and noticed by the tick.
	d.set(static_cast<size_t>(cp::Param::Mode), 1.0f);
	CHECK(card->updateContent());
	CHECK_FALSE(shown(cp::Param::Threshold));
	CHECK(shown(cp::Param::Ceiling));
	CHECK(shown(cp::Param::Drive));
	CHECK(shown(cp::Param::Release));
	checkAgainstGates(1);
	// CLOSED UP: Drive keeps the first cell, and Limit's next knob takes the cell Threshold had, not a hole after it.
	CHECK(card->knobForTest(cp::Param::Drive)->x() == doctest::Approx(firstCellX));
	CHECK(card->knobForTest(cp::Param::Ceiling)->x() == doctest::Approx(secondCellX));

	// COMPANDER (backlog QG): its own Ratio and Range in the cells Compress's Ratio and Knee had, Ref where Thresh was.
	d.set(static_cast<size_t>(cp::Param::Mode), 2.0f);
	CHECK(card->updateContent());
	checkAgainstGates(2);
	CHECK(shown(cp::Param::CmpRatio));
	CHECK(shown(cp::Param::CmpRange));
	CHECK_FALSE(shown(cp::Param::Ratio));
	CHECK_FALSE(shown(cp::Param::Knee));
	CHECK_FALSE(shown(cp::Param::Ceiling));
	CHECK(card->knobForTest(cp::Param::Threshold)->x() == doctest::Approx(secondCellX));
	CHECK(std::string(card->knobForTest(cp::Param::Threshold)->label()) == "Ref");

	// AND BACK.
	d.set(static_cast<size_t>(cp::Param::Mode), 0.0f);
	CHECK(card->updateContent());
	checkAgainstGates(0);
	CHECK(std::string(card->knobForTest(cp::Param::Threshold)->label()) == "Thresh");
}
TEST_CASE("Drive negative: COMP's Drive knob rests at the centre of -24..+24 on the card and in the window, prints a negative "
          "value, and the plot moves 12 dB left") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::Knob* card = d.card->knobForTest(cp::Param::Drive);
	REQUIRE(card != nullptr);
	REQUIRE(card->control() != nullptr);
	CHECK(card->control()->restNormalized() == doctest::Approx(0.5));
	ui::WidgetRegistry registry;
	ui::CompExpanded panel { d.f.deviceHost, registry, d.device };
	ui::Knob* window = panel.knobForTest(cp::Param::Drive);
	REQUIRE(window != nullptr);
	REQUIRE(window->control() != nullptr);
	CHECK(window->control()->restNormalized() == doctest::Approx(0.5));
	// A NEGATIVE VALUE PRINTS AS ONE, through the knob's own control.
	d.set(static_cast<size_t>(cp::Param::Drive), -12.0f);
	CHECK(card->control()->text().find("-12") != std::string::npos);
	// THE PLOT: Compress at T -18, 4:1, knee 0 — with -12 of drive, 0 dBFS in is -12 driven: 6 over, so -16.5 out.
	d.set(static_cast<size_t>(cp::Param::Threshold), -18.0f);
	d.set(static_cast<size_t>(cp::Param::Ratio), 4.0f);
	d.set(static_cast<size_t>(cp::Param::Knee), 0.0f);
	CHECK(std::fabs(d.card->transferDb(0.0f) - (-16.5f)) < 1e-3f);
	CHECK(std::fabs(d.card->transferDb(-24.0f) - (-36.0f)) < 1e-3f);
}
TEST_CASE("COMP's hover text follows its Mode, on the card and in the window") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::CompExpanded panel { d.f.deviceHost, registry, d.device };
	// THE EXPECTED TEXT IS RESTATED HERE, LITERALLY, not read back from the table under test.
	const auto hover = [](ui::Knob* k) { return ui::hoverText(*k, visage::Point { k->width() * 0.5f, k->height() * 0.5f }); };
	// THE HOVER ROAD MAY PUT THE CONTROL'S NAME IN FRONT ("Comp Threshold — …", when its caption is cut), so the
	// text under test is its tail: it must END in the mode's entry, and nothing else of the descriptor's may survive.
	const auto check = [&](int mode, cp::Param p, const char* want) {
		CAPTURE(mode);
		CAPTURE(static_cast<int>(p));
		for (ui::Knob* k : { d.card->knobForTest(p), panel.knobForTest(p) }) {
			REQUIRE(k != nullptr);
			const std::string text = hover(k);
			const std::string tail(want);
			CAPTURE(text);
			REQUIRE(text.size() >= tail.size());
			CHECK(text.compare(text.size() - tail.size(), tail.size(), tail) == 0);
		}
	};
	const auto setMode = [&](float m) {
		d.set(static_cast<size_t>(cp::Param::Mode), m);
		CHECK(d.card->updateContent());
		CHECK(panel.updateContent());
	};

	// COMPRESS, THE DEFAULT: all three say what they do here.
	check(0, cp::Param::Threshold, "The level above which the signal is turned down");
	check(0, cp::Param::Release, "How fast gain reduction lets go — off at the bottom is instant");
	check(0, cp::Param::Drive, "Gain in front of the compressor — how hard the signal is pushed into it; negative backs it off. The dry is never driven");

	// COMPANDER: Threshold is Ref, Release is the level follower, Drive is the compander's input.
	setMode(2.0f);
	check(2, cp::Param::Threshold, "The level above which the signal is turned down. Here it is Ref, the level left alone: louder is turned down, quieter is turned up");
	check(2, cp::Param::Release, "How fast the level follower falls — off at the bottom is instant");
	check(2, cp::Param::Drive, "Gain in front of the compander — how hard the signal is pushed into it; negative backs it off. The dry is never driven");

	// LIMIT: Release and Drive change text; Threshold is hidden there, so it is not read.
	setMode(1.0f);
	check(1, cp::Param::Release, "How fast gain reduction lets go — off at the bottom is instant");
	check(1, cp::Param::Drive, "Gain in front of the limiter — how hard the signal is pushed into it; negative backs it off. The dry is never driven");

	// BACK TO COMPRESS: the Compander's text does not survive the round trip.
	setMode(0.0f);
	check(0, cp::Param::Threshold, "The level above which the signal is turned down");
	check(0, cp::Param::Drive, "Gain in front of the compressor — how hard the signal is pushed into it; negative backs it off. The dry is never driven");
}
TEST_CASE("COMP's window shows the card's meter — the same IN, OUT and GR bars from the same readings, by value, in every mode") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::CompExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point size = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, size.x, size.y);
	ui::CompMeterView& meter = panel.meterForTest();
	// NOTHING PUBLISHED: neither draws a meter.
	CHECK_FALSE(meter.meter().has);
	// THE METER SITS INSIDE THE WINDOW'S FACE, right of the knobs, and overlaps none of them.
	CHECK(meter.isVisible());
	CHECK(meter.right() <= size.x - panel.laneRailForTest().width() + 0.5f);
	const auto publish = [&](float in, float out, float gr) {
		std::array<float, static_cast<size_t>(cp::Reading::kCount)> v {};
		v[static_cast<size_t>(cp::Reading::InPeak)] = in;
		v[static_cast<size_t>(cp::Reading::OutPeak)] = out;
		v[static_cast<size_t>(cp::Reading::GainReduction)] = gr;
		d.publish(v);
	};
	for (const float mode : { 0.0f, 1.0f, 2.0f }) {
		CAPTURE(mode);
		d.set(static_cast<size_t>(cp::Param::Mode), mode);
		publish(0.5f, 0.25f, 6.0f + mode);
		(void)panel.updateContent();
		(void)d.card->updateContent();
		const auto onCard = d.card->meterForTest();
		const ui::comp_plot::Meter inWindow = meter.meter();
		REQUIRE(inWindow.has);
		CHECK(inWindow.in == onCard.in);
		CHECK(inWindow.out == onCard.out);
		CHECK(inWindow.gr == onCard.gr);
		// AND THE NUMBERS ARE THE READINGS': 6 dB (+ mode) of GR on the 24 dB scale.
		CHECK(std::fabs(inWindow.gr - (6.0f + mode) / 24.0f) < 1e-6f);
		for (ui::Knob* k : { panel.knobForTest(cp::Param::Drive), panel.knobForTest(cp::Param::Release) }) {
			CHECK_FALSE((k->isVisible() && k->right() > meter.x() && k->x() < meter.right()));
		}
	}
	// A READING MOVING IS A REDRAW OF THE WINDOW'S METER (the delivery, not only the report).
	(void)panel.updateContent();
	RedrawProbe probe(&meter);
	publish(0.5f, 0.25f, 12.0f);
	CHECK(panel.updateContent());
	CHECK(probe.count == 1);
}
TEST_CASE("QG: COMP's transfer plot draws each mode's static curve by value, and a turned knob redraws it") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;
	REQUIRE(card->plotBounds().width() > 40.0f);
	REQUIRE(card->plotBounds().height() == doctest::Approx(ui::CompCard::kPlotHeight));
	// THE PLOT SITS INSIDE THE CARD, under the last row.
	CHECK(card->plotBounds().bottom() <= ui::CompCard::openHeight());

	// COMPANDER, Ref -18, 2:1, Range 12: restated by hand — a gain of -(x - Ref)/2, clamped to +-12.
	d.set(static_cast<size_t>(cp::Param::Mode), 2.0f);
	d.set(static_cast<size_t>(cp::Param::Threshold), -18.0f);
	d.set(static_cast<size_t>(cp::Param::CmpRatio), 2.0f);
	d.set(static_cast<size_t>(cp::Param::CmpRange), 12.0f);
	CHECK(std::fabs(card->transferDb(-6.0f) - (-12.0f)) < 1e-3f);
	CHECK(std::fabs(card->transferDb(-30.0f) - (-24.0f)) < 1e-3f);
	CHECK(std::fabs(card->transferDb(-18.0f) - (-18.0f)) < 1e-3f);
	CHECK(std::fabs(card->transferDb(-54.0f) - (-42.0f)) < 1e-3f); // the boost stops at +12
	// RATIO 4: a quarter of the slope, so -6 (12 over Ref) leaves 3 over it.
	d.set(static_cast<size_t>(cp::Param::CmpRatio), 4.0f);
	CHECK(std::fabs(card->transferDb(-6.0f) - (-15.0f)) < 1e-3f);

	// COMPRESS, T -20, R 4, knee 0: above T it is T + (x - T)/4; below, x.
	d.set(static_cast<size_t>(cp::Param::Mode), 0.0f);
	d.set(static_cast<size_t>(cp::Param::Threshold), -20.0f);
	d.set(static_cast<size_t>(cp::Param::Ratio), 4.0f);
	d.set(static_cast<size_t>(cp::Param::Knee), 0.0f);
	CHECK(std::fabs(card->transferDb(-4.0f) - (-16.0f)) < 1e-3f);
	CHECK(std::fabs(card->transferDb(-40.0f) - (-40.0f)) < 1e-3f);

	// LIMIT, ceiling -3: min(x, -3).
	d.set(static_cast<size_t>(cp::Param::Mode), 1.0f);
	d.set(static_cast<size_t>(cp::Param::Ceiling), -3.0f);
	CHECK(std::fabs(card->transferDb(0.0f) - (-3.0f)) < 1e-3f);
	CHECK(std::fabs(card->transferDb(-10.0f) - (-10.0f)) < 1e-3f);

	// A KNOB THAT SHAPES THE CURVE IS A REDRAW, with no reading and no mode moving.
	(void)card->updateContent();
	RedrawProbe probe(card);
	d.set(static_cast<size_t>(cp::Param::Ceiling), -6.0f);
	CHECK(card->updateContent());
	CHECK(probe.count == 1);
	probe.rearm();
	CHECK_FALSE(card->updateContent());
}

namespace {
/// A COMP IN A RACK, with the editor's road to it — `DelayRig`'s shape: here a container surface exists, so the lane can.
struct CompRig {
	feathers::rack::RackEngine engine { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	feathers::rack::RackBridge bridge { engine };
	DeviceInstanceId id = kNoInstance;
	CompRig() {
		engine.prepare(48000.0, 4096);
		bridge.prepare(48000.0, 4096);
		id = bridge.addDevice(feathers::fx::comp::kTraits.tag);
		REQUIRE(id != kNoInstance);
		bridge.drain();
		bridge.publish(engine);
	}
	void* surface() const { return bridge.containerSurfaceOf(id); }
	void set(feathers::fx::comp::Param p, float v) {
		bridge.setField(id, static_cast<size_t>(p), v);
		bridge.drain();
		bridge.publish(engine);
	}
};
} // namespace

TEST_CASE("QG: COMP's card shows the topology and the lane chip only in Compander, and the chip counts the lane") {
	namespace cp = feathers::fx::comp;
	CompRig rig;
	REQUIRE(rig.surface() != nullptr);
	ui::WidgetRegistry registry;
	auto owned = std::make_unique<ui::CompCard>(rig.bridge, registry, rig.id);
	ui::CompCard* card = owned.get();
	card->setBounds(0.0f, 0.0f, 236.0f, ui::CompCard::openHeight());
	struct { CompRig& r; void set(size_t p, float v) { r.set(static_cast<cp::Param>(p), v); } } d { rig };
	REQUIRE(card->topologyForTest() != nullptr);
	CHECK_FALSE(card->topologyForTest()->isVisible());
	CHECK_FALSE(card->laneChipForTest().isVisible());
	d.set(static_cast<size_t>(cp::Param::Mode), 2.0f);
	(void)card->updateContent();
	CHECK(card->topologyForTest()->isVisible());
	CHECK(card->laneChipForTest().isVisible());
	CHECK(card->laneChipTextForTest() == ui::CompCard::kLaneChipEmpty);
	// THE CHIP SITS INSIDE THE PLOT, and the topology row above the knobs, overlapping nothing.
	const visage::Bounds plot = card->plotBounds();
	const visage::Bounds chip = card->laneChipForTest().bounds();
	CHECK(chip.x() >= plot.x());
	CHECK(chip.right() <= plot.right());
	CHECK(chip.y() >= plot.y());
	CHECK(card->topologyForTest()->bounds().y() >= card->modeForTest()->bounds().bottom());
	// A DEVICE IN THE LANE: the chip counts it.
	rack::InnerChain* lane = feathers::fx::comp::ensureCompanderLane(rig.surface());
	REQUIRE(lane != nullptr);
	REQUIRE(lane->bridge() != nullptr);
	(void)lane->bridge()->addDevice(feathers::fx::busutil::kDescriptor.traits.tag);
	(void)card->updateContent();
	CHECK(card->laneChipTextForTest() == std::string(ui::CompCard::kLaneChipPrefix) + "1");
	// BACK TO COMPRESS: both go, the lane's device stays.
	d.set(static_cast<size_t>(cp::Param::Mode), 0.0f);
	(void)card->updateContent();
	CHECK_FALSE(card->topologyForTest()->isVisible());
	CHECK_FALSE(card->laneChipForTest().isVisible());
	CHECK(lane->bridge()->chainLength() == 1u);
}
TEST_CASE("QG: COMP's plot draws the active topology's halves and their net, by value") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;
	d.set(static_cast<size_t>(cp::Param::Mode), 2.0f);
	d.set(static_cast<size_t>(cp::Param::Threshold), -18.0f);
	d.set(static_cast<size_t>(cp::Param::CmpRatio), 2.0f);
	d.set(static_cast<size_t>(cp::Param::CmpRange), 24.0f);
	// EXPAND, single-ended: (R - 1)(x - Ref) — -6 in leaves at +6.
	d.set(static_cast<size_t>(cp::Param::Topology), static_cast<float>(cp::Topology::Expand));
	CHECK_FALSE(card->curves().paired());
	CHECK(std::fabs(card->transferDb(-6.0f) - 6.0f) < 1e-3f);
	CHECK(std::fabs(card->transferDb(-30.0f) - (-42.0f)) < 1e-3f);
	// COMPANDER, LINKED: each half moves, the net is the unity line.
	d.set(static_cast<size_t>(cp::Param::Topology), static_cast<float>(cp::Topology::Compander));
	REQUIRE(card->curves().paired());
	CHECK(std::fabs(card->curves().firstDb(-6.0f) - (-12.0f)) < 1e-3f);
	CHECK(std::fabs(card->curves().secondDb(-12.0f) - (-6.0f)) < 1e-3f);
	for (const float x : { -54.0f, -30.0f, -18.0f, -6.0f }) {
		CHECK(std::fabs(card->transferDb(x) - x) < 1e-3f);
	}
	// UNLINKED, RATIO 2 AT 4: the expander at 4 lifts the compressor's -12 by 3 x 6 dB — the net is +6 at -6.
	d.set(static_cast<size_t>(cp::Param::CmpLink), 0.0f);
	d.set(static_cast<size_t>(cp::Param::CmpRatio2), 4.0f);
	CHECK(std::fabs(card->transferDb(-6.0f) - 6.0f) < 1e-3f);
	// EXCOMPER, LINKED: the expander first, the compressor after, the net the unity line again.
	d.set(static_cast<size_t>(cp::Param::CmpLink), 1.0f);
	d.set(static_cast<size_t>(cp::Param::Topology), static_cast<float>(cp::Topology::Excomper));
	CHECK(std::fabs(card->curves().firstDb(-6.0f) - 6.0f) < 1e-3f);
	CHECK(std::fabs(card->transferDb(-6.0f) - (-6.0f)) < 1e-3f);
}
TEST_CASE("QG: COMP's window — the lane rail only in Compander, the second half only unlinked, and unlinking starts the "
          "second half at the first half's values") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::WidgetRegistry registry;
	ui::CompExpanded panel { d.f.deviceHost, registry, d.device };
	const visage::Point size = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, size.x, size.y);
	(void)panel.updateContent();
	const auto shown = [&](cp::Param p) {
		ui::Knob* k = panel.knobForTest(p);
		REQUIRE(k != nullptr);
		return k->isVisible();
	};
	// COMPRESS: no rail, no topology, no Link, no second half.
	CHECK_FALSE(panel.laneRailForTest().isVisible());
	CHECK_FALSE(panel.topologyForTest().isVisible());
	CHECK_FALSE(panel.linkForTest().isVisible());
	CHECK_FALSE(shown(cp::Param::CmpRatio2));
	// COMPANDER, the default topology (Compress, single-ended): the rail and the topology; still no Link.
	d.set(static_cast<size_t>(cp::Param::Mode), 2.0f);
	(void)panel.updateContent();
	CHECK(panel.laneRailForTest().isVisible());
	CHECK(panel.topologyForTest().isVisible());
	CHECK_FALSE(panel.linkForTest().isVisible());
	CHECK(panel.minimumSize().x > size.x); // the rail takes room
	// A PAIRED TOPOLOGY: Link shows; linked, the second half does not.
	d.set(static_cast<size_t>(cp::Param::Topology), static_cast<float>(cp::Topology::Excomper));
	(void)panel.updateContent();
	CHECK(panel.linkForTest().isVisible());
	CHECK_FALSE(shown(cp::Param::CmpRatio2));
	// THE FIRST HALF SOMEWHERE ELSE, then UNLINK THROUGH THE CHIP: the second half starts where the first is, by value.
	d.set(static_cast<size_t>(cp::Param::CmpRatio), 3.25f);
	d.set(static_cast<size_t>(cp::Param::Attack), -9.5f);
	d.set(static_cast<size_t>(cp::Param::Release), -1.25f);
	REQUIRE(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(cp::Param::CmpRatio2)) == doctest::Approx(2.0f));
	panel.pressLinkForTest();
	d.f.bridge.refresh();
	(void)panel.updateContent();
	const auto at = [&](cp::Param p) { return d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(p)); };
	CHECK(at(cp::Param::CmpLink) == doctest::Approx(0.0f));
	CHECK(at(cp::Param::CmpRatio2) == doctest::Approx(3.25f));
	CHECK(at(cp::Param::CmpAttack2) == doctest::Approx(-9.5f));
	CHECK(at(cp::Param::CmpRelease2) == doctest::Approx(-1.25f));
	CHECK(shown(cp::Param::CmpRatio2));
	CHECK(shown(cp::Param::CmpAttack2));
	CHECK(shown(cp::Param::CmpRelease2));
	// RE-LINKING COPIES NOTHING BACK: the second half keeps its values, hidden.
	d.set(static_cast<size_t>(cp::Param::CmpRatio2), 1.75f);
	panel.pressLinkForTest();
	d.f.bridge.refresh();
	(void)panel.updateContent();
	CHECK(at(cp::Param::CmpLink) == doctest::Approx(1.0f));
	CHECK(at(cp::Param::CmpRatio2) == doctest::Approx(1.75f));
	CHECK_FALSE(shown(cp::Param::CmpRatio2));
	// THE WINDOW OFFERS EVERY CONTROL THE CARD DOES (the guard in test_gui_widgets_tooltips walks the states).
	const auto offered = panel.addressedParamsForTest();
	for (const cp::Param p : d.card->addressedParamsForTest()) {
		if (p == cp::Param::Enabled) {
			continue; // the window's enable is the overlay header's chip
		}
		CAPTURE(static_cast<int>(p));
		CHECK(std::find(offered.begin(), offered.end(), p) != offered.end());
	}
}
TEST_CASE("SHOT QG: COMP's card in each mode, the Compander's transfer plot among them (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — the venue for LOOKING. Writes the editor with COMP's card in Compander, Compress and Limit,
	// a reading published so the meter and the plot's dot draw. Skipped without the variable.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;
	auto* editor = dynamic_cast<visage::ApplicationEditor*>(d.f.root);
	REQUIRE(editor != nullptr);
	editor->setWindowless(static_cast<int>(d.f.root->width()), static_cast<int>(d.f.root->height()));
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	std::array<float, static_cast<size_t>(cp::Reading::kCount)> v {};
	v[static_cast<size_t>(cp::Reading::InPeak)] = 0.35f;
	v[static_cast<size_t>(cp::Reading::OutPeak)] = 0.25f;
	v[static_cast<size_t>(cp::Reading::GainReduction)] = 3.0f;
	d.publish(v);
	const auto shoot = [&](const char* name) {
		(void)card->updateContent();
		d.f.root->redrawAll();
		editor->drawWindow();
		editor->drawWindow();
		editor->drawWindow();
		const visage::Screenshot& shot = editor->takeScreenshot();
		if (shot.width() == 0) {
			MESSAGE("no frame for " << name);
			return;
		}
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		const visage::Point where = card->positionInWindow();
		MESSAGE(name << " " << shot.width() << "x" << shot.height() << " card " << where.x << "," << where.y << " "
		             << card->width() << "x" << card->height() << " -> " << path);
	};
	d.set(static_cast<size_t>(cp::Param::Mode), 2.0f);
	shoot("comp-compander");
	d.set(static_cast<size_t>(cp::Param::CmpRatio), 4.0f);
	d.set(static_cast<size_t>(cp::Param::CmpRange), 18.0f);
	shoot("comp-compander-4to1-range18");
	d.set(static_cast<size_t>(cp::Param::Mode), 0.0f);
	shoot("comp-compress");
	d.set(static_cast<size_t>(cp::Param::Mode), 1.0f);
	shoot("comp-limit");

	// STAGE 2: Excomper, unlinked, a device in the lane — the card, then the window (rail, Link, the second half).
	d.set(static_cast<size_t>(cp::Param::Mode), 2.0f);
	d.set(static_cast<size_t>(cp::Param::Topology), static_cast<float>(cp::Topology::Excomper));
	d.set(static_cast<size_t>(cp::Param::CmpLink), 0.0f);
	d.set(static_cast<size_t>(cp::Param::CmpRatio2), 3.0f);
	shoot("comp-excomper-unlinked");
	// THE WINDOW OVER A COMP IN A RACK (`CompRig`), where the lane can exist: a Bus Utility in it.
	CompRig rig;
	rig.set(cp::Param::Mode, 2.0f);
	rig.set(cp::Param::Topology, static_cast<float>(cp::Topology::Excomper));
	rig.set(cp::Param::CmpLink, 0.0f);
	rig.set(cp::Param::CmpRatio2, 3.0f);
	rack::InnerChain* inner = feathers::fx::comp::ensureCompanderLane(rig.surface());
	REQUIRE(inner != nullptr);
	(void)inner->bridge()->addDevice(feathers::fx::busutil::kDescriptor.traits.tag);
	// SOUND THROUGH IT, so the window's meter (the GR meter on the popout window) has readings to draw.
	const auto play = [&rig] {
		std::vector<float> l(512), r(512);
		uint32_t seed = 3;
		for (int b = 0; b < 40; ++b) {
			for (size_t i = 0; i < l.size(); ++i) {
				seed = seed * 1664525u + 1013904223u;
				l[i] = r[i] = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
			}
			rig.engine.processStereo(l.data(), r.data(), 512, feathers::backplane::BlockContext {});
		}
		rig.bridge.publish(rig.engine);
	};
	play();
	constexpr int w = 960;
	constexpr int h = 520;
	visage::ApplicationEditor windowEditor;
	windowEditor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	windowEditor.setWindowless(w, h);
	windowEditor.onDraw() = [&windowEditor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, windowEditor.width(), windowEditor.height());
	};
	ui::WidgetRegistry registry;
	auto view = std::make_unique<ui::CompExpanded>(rig.bridge, registry, rig.id);
	ui::CompExpanded* window = view.get();
	windowEditor.addChild(window);
	window->setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	const auto shootWindow = [&](const char* name) {
		for (int i = 0; i < 20; ++i) {
			(void)window->updateContent();
			windowEditor.redrawAll();
			windowEditor.drawWindow();
		}
		const visage::Screenshot& shot = windowEditor.takeScreenshot();
		REQUIRE(shot.width() == w);
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		MESSAGE("wrote " << path);
	};
	shootWindow("comp-window-excomper-unlinked-lane");
	rig.set(cp::Param::Topology, static_cast<float>(cp::Topology::Compander));
	rig.set(cp::Param::CmpLink, 1.0f);
	play();
	shootWindow("comp-window-compander-linked");
	rig.set(cp::Param::Mode, 0.0f);
	play();
	shootWindow("comp-window-compress");
	windowEditor.removeChild(window);
}
TEST_CASE("COMP's meter draws IN, OUT and GR at the dB positions its readings name") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;

	// NOTHING PUBLISHED IS NOT SILENCE: no meter, and no fractions to draw.
	CHECK_FALSE(card->hasReadings());
	CHECK(card->meterForTest().in == 0.0f);
	CHECK(card->meterForTest().gr == 0.0f);

	const auto publish = [&](float in, float out, float gr) {
		std::array<float, static_cast<size_t>(cp::Reading::kCount)> v {};
		v[static_cast<size_t>(cp::Reading::InPeak)] = in;
		v[static_cast<size_t>(cp::Reading::OutPeak)] = out;
		v[static_cast<size_t>(cp::Reading::GainReduction)] = gr;
		d.publish(v);
	};

	publish(0.5f, 0.25f, 6.0f);
	REQUIRE(card->hasReadings());
	auto m = card->meterForTest();
	CAPTURE(m.in);
	CAPTURE(m.out);
	CAPTURE(m.gr);
	// −6.02 dB → (60 − 6.02)/60; −12.04 dB → (60 − 12.04)/60; 6 dB of 24 → a quarter.
	CHECK(std::fabs(m.in - expectedLevel(0.5f)) < 1e-5f);
	CHECK(std::fabs(m.out - expectedLevel(0.25f)) < 1e-5f);
	CHECK(std::fabs(m.gr - 0.25f) < 1e-6f);
	// AND NOT VACUOUS: three different numbers, so no two readings can answer for each other.
	CHECK(m.in != m.out);

	// THE ENDS: full scale and above fill, the floor and below empty, 24 dB of reduction and more fill.
	publish(1.0f, 2.0f, 30.0f);
	m = card->meterForTest();
	CHECK(m.in == doctest::Approx(1.0f));
	CHECK(m.out == doctest::Approx(1.0f));
	CHECK(m.gr == doctest::Approx(1.0f));
	publish(0.0005f, 0.0f, 0.0f); // −66 dB, and a true zero
	m = card->meterForTest();
	CHECK(m.in == 0.0f);
	CHECK(m.out == 0.0f);
	CHECK(m.gr == 0.0f);
	// A MID GR, so the mapping is shown linear rather than merely clamped at both ends.
	publish(0.1f, 0.1f, 18.0f);
	CHECK(std::fabs(card->meterForTest().gr - 0.75f) < 1e-6f);
}
TEST_CASE("backlog LG: a meter's bars are data and keep the mock's colours under any chrome hue, while the card's "
          "chrome around them turns") {
	// RENDERED, because the claim is about what a call site PAINTS: the meter fills read `kMeterAccent` and
	// `kMeterValue`, and a call site left on the live `kAccent` would compile and pass every value case in
	// test_chrome_hue. The pixel at the middle of the full IN bar and of the full GR bar is read at offset 0
	// and at +120°, and compared with the mock's tokens; the card's own surface beside the meter is the
	// control that the turn reached this picture at all.
	namespace cp = feathers::fx::comp;
	struct Reset {
		~Reset() { (void)ui::setChromeHueOffset(0.0); }
	} reset;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;
	std::array<float, static_cast<size_t>(cp::Reading::kCount)> full {};
	full[static_cast<size_t>(cp::Reading::InPeak)] = 1.0f;
	full[static_cast<size_t>(cp::Reading::OutPeak)] = 1.0f;
	full[static_cast<size_t>(cp::Reading::GainReduction)] = 30.0f;
	d.publish(full);
	REQUIRE(card->hasReadings());
	REQUIRE(card->meterForTest().in == doctest::Approx(1.0f));
	REQUIRE(card->meterForTest().gr == doctest::Approx(1.0f));

	auto* editor = dynamic_cast<visage::ApplicationEditor*>(d.f.root);
	REQUIRE(editor != nullptr);
	editor->setWindowless(static_cast<int>(d.f.root->width()), static_cast<int>(d.f.root->height()));
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	const visage::Bounds box = card->meterBounds();
	const float third = box.width() / 3.0f;
	const float barHeight = box.height() - ui::dyn_meter::kCaptionHeight - 2.0f;
	const visage::Point inBar = card->positionInWindow() + visage::Point { box.x() + third * 0.5f, box.y() + barHeight * 0.5f };
	const visage::Point grBar = card->positionInWindow() + visage::Point { box.x() + third * 2.5f, box.y() + barHeight * 0.5f };
	// THE CARD'S SURFACE: just inside the card's left edge, below its head.
	const visage::Point surface = card->positionInWindow() + visage::Point { 3.0f, ui::kCardHeadHeight + 6.0f };

	struct Rgb {
		int r, g, b;
		bool operator==(const Rgb&) const = default;
	};
	const auto shoot = [&](double offset) {
		(void)ui::setChromeHueOffset(offset);
		d.f.root->redrawAll();
		editor->drawWindow();
		editor->drawWindow();
		const visage::Screenshot& shot = editor->takeScreenshot();
		REQUIRE(shot.width() > 0);
		const auto at = [&](visage::Point p) {
			const int x = static_cast<int>(p.x);
			const int y = static_cast<int>(p.y);
			REQUIRE(x >= 0);
			REQUIRE(y >= 0);
			REQUIRE(x < shot.width());
			REQUIRE(y < shot.height());
			const uint8_t* px = shot.data() + (static_cast<size_t>(y) * shot.width() + x) * 4;
			return Rgb { px[0], px[1], px[2] };
		};
		return std::array<Rgb, 3> { at(inBar), at(grBar), at(surface) };
	};
	const auto token = [](uint32_t argb) {
		return Rgb { static_cast<int>((argb >> 16) & 0xff), static_cast<int>((argb >> 8) & 0xff),
			         static_cast<int>(argb & 0xff) };
	};
	const auto near = [](Rgb a, Rgb b) {
		return std::abs(a.r - b.r) <= 2 && std::abs(a.g - b.g) <= 2 && std::abs(a.b - b.b) <= 2;
	};

	const std::array<Rgb, 3> atZero = shoot(0.0);
	const std::array<Rgb, 3> turned = shoot(120.0);
	CAPTURE(atZero[0].r);
	CAPTURE(atZero[0].g);
	CAPTURE(atZero[0].b);
	CAPTURE(turned[0].r);
	CAPTURE(turned[0].g);
	CAPTURE(turned[0].b);
	// THE BARS ARE THE MOCK'S COLOURS, at 0 and at +120°, and bit-identical between the two.
	CHECK(near(atZero[0], token(feathers::tokens::kAccent)));
	CHECK(near(atZero[1], token(feathers::tokens::kValue)));
	CHECK(turned[0] == atZero[0]);
	CHECK(turned[1] == atZero[1]);
	// AND THE TURN REACHED THIS PICTURE: the card's surface moved.
	CAPTURE(atZero[2].r);
	CAPTURE(atZero[2].b);
	CAPTURE(turned[2].r);
	CAPTURE(turned[2].b);
	CHECK_FALSE(turned[2] == atZero[2]);
}
TEST_CASE("COMP's card delivers a redraw when a reading or the mode moves, and not otherwise") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;
	// THE VENUE MUST BE ABLE TO EXPRESS A REDRAW, or a zero below would be silence about nothing.
	REQUIRE(card->isVisible());
	REQUIRE(card->isDrawing());
	(void)card->updateContent(); // settle whatever the construction left pending
	RedrawProbe probe(card);

	std::array<float, static_cast<size_t>(cp::Reading::kCount)> v {};
	v[static_cast<size_t>(cp::Reading::InPeak)] = 0.5f;
	v[static_cast<size_t>(cp::Reading::OutPeak)] = 0.4f;
	v[static_cast<size_t>(cp::Reading::GainReduction)] = 3.0f;
	d.publish(v);
	CHECK(card->updateContent());
	CHECK(probe.count == 1);

	// NOTHING MOVED: no report and no ask.
	probe.rearm();
	CHECK_FALSE(card->updateContent());
	CHECK(probe.count == 1);

	// THE GR ALONE MOVING IS ENOUGH — it is the reading this device exists to show.
	v[static_cast<size_t>(cp::Reading::GainReduction)] = 4.5f;
	d.publish(v);
	probe.rearm();
	CHECK(card->updateContent());
	CHECK(probe.count == 2);

	// AND SO IS THE MODE, with the readings still.
	d.set(static_cast<size_t>(cp::Param::Mode), 1.0f);
	probe.rearm();
	CHECK(card->updateContent());
	CHECK(probe.count == 3);
}
TEST_CASE("COMP's Ratio prints as N:1, and as infinity at the top of its travel") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	// THROUGH THE KNOB'S OWN CONTROL — the text the knob draws and the notifier speaks.
	ui::Knob* ratio = d.card->knobForTest(cp::Param::Ratio);
	REQUIRE(ratio != nullptr);
	REQUIRE(ratio->control() != nullptr);
	const auto text = [&](float r) {
		d.set(static_cast<size_t>(cp::Param::Ratio), r);
		return ratio->control()->text();
	};
	CHECK(text(4.0f) == "4.0:1");
	CHECK(text(1.0f) == "1.0:1");
	CHECK(text(19.9f) == "19.9:1");
	// AT kRatioInfinite THE SLOPE IS ZERO (`slopeFor`), and the print says so.
	REQUIRE(cp::kRatioInfinite == 20.0f);
	CHECK(text(20.0f) == "\xE2\x88\x9E:1");
	// EVERY OTHER CONTROL FALLS THROUGH TO ITS UNIT — the hook answers for Ratio only.
	char out[32] = {};
	CHECK_FALSE(cp::kDescriptor.text->format(static_cast<size_t>(cp::Param::Threshold), -18.0,
	                                         feathers::device::TextView {}, out, sizeof(out)));
}
TEST_CASE("CLIP's card shows exactly each mode's own controls") {
	namespace cl = feathers::fx::clip;
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	ui::ClipCard* card = d.card;

	// THE TABLE, RESTATED BY HAND FROM THE PLAN (Task 7) — which mode reads which control. Checked
	// against the card AND against the descriptor's gates, so a card and a descriptor that drifted
	// together would still be caught by the first.
	const std::array<cl::Param, 13> gated { cl::Param::Knee, cl::Param::Freq, cl::Param::Foldback, cl::Param::Grit,
		                                    cl::Param::Depth, cl::Param::Bits, cl::Param::Shape, cl::Param::DcBlock, cl::Param::Bal, cl::Param::Rate,
		                                    cl::Param::Angle, cl::Param::Hard, cl::Param::Bias };
	const std::array<std::set<cl::Param>, 15> expected {
		std::set<cl::Param> { cl::Param::Knee },                                         // Soft
		std::set<cl::Param> { cl::Param::Knee, cl::Param::Freq, cl::Param::Foldback },   // Texture
		std::set<cl::Param> { cl::Param::Knee, cl::Param::Grit },                        // Grit
		std::set<cl::Param> { cl::Param::Depth },                                        // Fold
		std::set<cl::Param> { cl::Param::Knee, cl::Param::Bits },                        // Bits
		std::set<cl::Param> { cl::Param::Shape, cl::Param::DcBlock, cl::Param::Hard },   // Tube
		std::set<cl::Param> { cl::Param::Shape, cl::Param::Bias },                       // Jag
		std::set<cl::Param> { cl::Param::Shape, cl::Param::DcBlock, cl::Param::Rate },   // Lean
		std::set<cl::Param> { cl::Param::Shape, cl::Param::Bal },                        // Chonk
		std::set<cl::Param> { cl::Param::Angle, cl::Param::DcBlock, cl::Param::Rate },   // Paunch-S
		std::set<cl::Param> { cl::Param::Angle, cl::Param::DcBlock, cl::Param::Rate },   // Paunch-H
		std::set<cl::Param> { cl::Param::Angle, cl::Param::Rate },                       // Paunch-OS
		std::set<cl::Param> { cl::Param::Angle, cl::Param::Rate },                       // Paunch-OH
		std::set<cl::Param> { cl::Param::Shape },                                        // Slew
		std::set<cl::Param> { cl::Param::Shape },                                        // Lopside
	};
	REQUIRE(expected.size() == static_cast<size_t>(cl::Mode::kCount));

	for (int mode = 0; mode < static_cast<int>(cl::Mode::kCount); ++mode) {
		d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(mode));
		(void)card->updateContent();
		CAPTURE(std::string(cl::kModeNames[mode]));
		std::set<cl::Param> shown;
		for (const cl::Param p : gated) {
			visage::Frame* w = card->widgetForTest(p);
			REQUIRE(w != nullptr);
			CAPTURE(std::string(cl::kParamDescs[static_cast<size_t>(p)].name));
			CHECK(w->isVisible() == gateSays(cl::kParamDescs, static_cast<size_t>(p), mode));
			if (w->isVisible()) {
				shown.insert(p);
			}
		}
		CHECK(shown == expected[static_cast<size_t>(mode)]);
		// THE GLOBALS AND ROW 1 ARE THERE IN EVERY MODE.
		for (const cl::Param p : { cl::Param::Drive, cl::Param::Ceiling, cl::Param::Output, cl::Param::Mode,
		                           cl::Param::Oversample }) {
			CHECK(card->widgetForTest(p)->isVisible());
		}
	}
}
TEST_CASE("CLIP's OVER LED follows the held Over reading, and its bars the dB mapping") {
	namespace cl = feathers::fx::clip;
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	ui::ClipCard* card = d.card;

	// NOTHING PUBLISHED: no LED, whatever a zero would have meant.
	CHECK_FALSE(card->hasReadings());
	CHECK_FALSE(card->ledLit());

	const auto publish = [&](float in, float out, float over) {
		std::array<float, static_cast<size_t>(cl::Reading::kCount)> v {};
		v[static_cast<size_t>(cl::Reading::InPeak)] = in;
		v[static_cast<size_t>(cl::Reading::OutPeak)] = out;
		v[static_cast<size_t>(cl::Reading::Over)] = over;
		d.publish(v);
	};
	publish(0.5f, 0.25f, 1.0f);
	CHECK(card->ledLit());
	publish(0.5f, 0.25f, 0.0f);
	CHECK_FALSE(card->ledLit());
	// THE THRESHOLD IS A HALF, from both sides.
	publish(0.5f, 0.25f, 0.49f);
	CHECK_FALSE(card->ledLit());
	publish(0.5f, 0.25f, 0.5f);
	CHECK(card->ledLit());

	const auto m = card->meterForTest();
	CHECK(std::fabs(m.in - expectedLevel(0.5f)) < 1e-5f);
	CHECK(std::fabs(m.out - expectedLevel(0.25f)) < 1e-5f);
}
TEST_CASE("CLIP's card delivers a redraw when the Over reading alone moves") {
	namespace cl = feathers::fx::clip;
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	ui::ClipCard* card = d.card;
	REQUIRE(card->isVisible());
	REQUIRE(card->isDrawing());
	(void)card->updateContent();
	RedrawProbe probe(card);

	std::array<float, static_cast<size_t>(cl::Reading::kCount)> v {};
	v[static_cast<size_t>(cl::Reading::InPeak)] = 0.9f;
	v[static_cast<size_t>(cl::Reading::OutPeak)] = 0.8f;
	d.publish(v);
	CHECK(card->updateContent());
	CHECK(probe.count == 1);

	probe.rearm();
	CHECK_FALSE(card->updateContent());
	CHECK(probe.count == 1);

	// AN OVER IN AN OTHERWISE STEADY PASSAGE — the levels identical, only the LED's reading moving.
	v[static_cast<size_t>(cl::Reading::Over)] = 1.0f;
	d.publish(v);
	probe.rearm();
	CHECK(card->updateContent());
	CHECK(probe.count == 2);

	// AND A MODE CHANGE.
	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Fold));
	probe.rearm();
	CHECK(card->updateContent());
	CHECK(probe.count == 3);
}
TEST_CASE("CLIP's mode controls close up: Fold's Depth takes the first cell, Bits sits straight after Knee") {
	namespace cl = feathers::fx::clip;
	DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
	ui::ClipCard* card = d.card;
	// CELL 0 IS WHERE ROW 2'S FIRST KNOB IS — the same column grid, one row down.
	const float cell0 = card->widgetForTest(cl::Param::Drive)->x();

	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Fold));
	(void)card->updateContent();
	REQUIRE(card->widgetForTest(cl::Param::Depth)->isVisible());
	CHECK(card->widgetForTest(cl::Param::Depth)->x() == doctest::Approx(cell0));

	d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(cl::Mode::Bits));
	(void)card->updateContent();
	visage::Frame* knee = card->widgetForTest(cl::Param::Knee);
	visage::Frame* bits = card->widgetForTest(cl::Param::Bits);
	REQUIRE(knee->isVisible());
	REQUIRE(bits->isVisible());
	CHECK(knee->x() == doctest::Approx(cell0));
	// THE NEXT CELL, LESS NOTHING: Freq, Foldback, Grit and Depth are all hidden and took no room.
	CHECK(bits->x() == doctest::Approx(knee->x() + knee->width() + ui::ClipCard::kBitsInset));

	// THE ERODER'S SIX: Shape is the row's one knob, in the first cell, in each of them.
	for (const cl::Mode mode : { cl::Mode::Tube, cl::Mode::Lean, cl::Mode::PaunchS, cl::Mode::PaunchH, cl::Mode::PaunchOS, cl::Mode::PaunchOH, cl::Mode::Lopside }) {
		d.set(static_cast<size_t>(cl::Param::Mode), static_cast<float>(mode));
		(void)card->updateContent();
		// (the four Paunch modes read Angle, a key of its own, in the same first cell; Shape is hidden there)
		const bool paunch = mode >= cl::Mode::PaunchS && mode <= cl::Mode::PaunchOH;
		visage::Frame* first = card->widgetForTest(paunch ? cl::Param::Angle : cl::Param::Shape);
		REQUIRE(first->isVisible());
		CHECK(first->x() == doctest::Approx(cell0));
		CHECK_FALSE(card->widgetForTest(paunch ? cl::Param::Shape : cl::Param::Angle)->isVisible());
	}
}
TEST_CASE("a dynamics card whose readings stop being published repaints to no signal") {
	// THE COMPARISON RUNS ONLY WHILE READINGS EXIST, so published -> unpublished had to be a change of
	// its own: without it the last bars stayed on screen for a device that had stopped measuring.
	SUBCASE("COMP") {
		namespace cp = feathers::fx::comp;
		DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
		std::array<float, static_cast<size_t>(cp::Reading::kCount)> v { 0.5f, 0.4f, 3.0f };
		d.publish(v);
		REQUIRE(d.card->updateContent());
		RedrawProbe probe(d.card);
		d.f.bridge.publishDeviceReadings(d.slot, v.data(), 0); // the count withdrawn
		REQUIRE_FALSE(d.card->hasReadings());
		CHECK(d.card->updateContent());
		CHECK(probe.count == 1);
		probe.rearm();
		CHECK_FALSE(d.card->updateContent()); // and once is enough
		CHECK(probe.count == 1);
	}
	SUBCASE("CLIP") {
		namespace cl = feathers::fx::clip;
		DynCardFixture<ui::ClipCard> d(cl::kTraits.tag);
		std::array<float, static_cast<size_t>(cl::Reading::kCount)> v { 0.5f, 0.4f, 1.0f };
		d.publish(v);
		REQUIRE(d.card->updateContent());
		RedrawProbe probe(d.card);
		d.f.bridge.publishDeviceReadings(d.slot, v.data(), 0);
		REQUIRE_FALSE(d.card->hasReadings());
		CHECK(d.card->updateContent());
		CHECK(probe.count == 1);
		probe.rearm();
		CHECK_FALSE(d.card->updateContent());
		CHECK(probe.count == 1);
	}
}

namespace {
/// THE PANEL, OPENED THE WAY A HAND OPENS IT — the lane's expand, onto the editor's overlay — so its frames
/// are drawing: a `RedrawProbe` on a frame that is not being drawn counts nothing, whatever happens.
ui::ResonateExpanded* openResonatePanel(ResonateFixture& d) {
	auto* overlay = d.f.gui.expandedForTest();
	REQUIRE(overlay != nullptr);
	(void)d.f.gui.canvasForTest()->setCollapsed(true);
	d.lane->onExpandDevice(d.slot);
	REQUIRE(overlay->open());
	auto* mounted = const_cast<ui::ExpandedView*>(overlay->contentForTest());
	REQUIRE(mounted != nullptr);
	const auto* inner = dynamic_cast<const ui::MountedFrame*>(mounted);
	REQUIRE(inner != nullptr);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(inner->contentForTest());
	REQUIRE(panel != nullptr);
	return panel;
}
} // namespace

TEST_CASE("RESONATE's card is compact: near the simplest cards' height, and far below the old one") {
	// The card was far too big. The bound is stated, and the neighbours are measured beside it
	// so the number means something. The OLD card is restated from its formula at the lane's width: a caption
	// and a knob row (85), the pictograms (30), a 0.86-aspect view (193.8), the tone block (144) and the
	// filter/out row (79), inside the head's 28 and the pad's 10 — 569.8 px.
	ResonateFixture d(rsn::kTraits.tag);
	const float width = static_cast<float>(d.card->width());
	const float resonate = d.card->laneHeightAt(width);
	constexpr float kOldResonate = 569.8f;
	// THE BOUND IS COMP'S NEIGHBOURHOOD: two knob rows of three (captions whole) under a readable string view.
	constexpr float kBound = 350.0f;
	MESSAGE("card heights at " << width << " px: Resonate " << resonate << " (old " << kOldResonate << "), CLIP "
	                           << ui::ClipCard::openHeight() << ", COMP " << ui::CompCard::openHeight());
	CHECK(resonate <= kBound);
	CHECK(resonate < 0.62f * kOldResonate);
	// AND THE STRING VIEW IS STILL THE CARD'S LARGEST THING — checked in the case above; here, that it takes
	// at least two fifths of the card's height.
	const ui::ResonateRingView* ring = d.card->face().ringView();
	CHECK(static_cast<float>(ring->height()) >= 0.4f * resonate);
}
TEST_CASE("RESONATE's panel seats every control but Enabled, and shows Porta only in Mono") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel->update();
	panel->resized();
	const ui::ResonateFace& face = panel->face();
	for (size_t i = 0; i < static_cast<size_t>(rsn::Param::kCount); ++i) {
		const auto p = static_cast<rsn::Param>(i);
		CAPTURE(std::string(rsn::kParamDescs[i].name));
		// ENABLED is the window header's chip; DRONE TO WET FX heads the DRONE FX rail (its own case below).
		CHECK((face.widgetFor(p) != nullptr) == (p != rsn::Param::Enabled && p != rsn::Param::DroneToWetFx));
	}
	visage::Frame* porta = face.widgetFor(rsn::Param::Porta);
	CHECK_FALSE(porta->isVisible()); // Poly by default
	d.set(static_cast<size_t>(rsn::Param::Voicing), static_cast<float>(rsn::Voicing::Mono));
	(void)panel->update();
	CHECK(porta->isVisible());
	CHECK(porta->isVisible() == gateSays(rsn::kParamDescs, static_cast<size_t>(rsn::Param::Porta), 1));
	visage::Frame* voicing = face.widgetFor(rsn::Param::Voicing);
	CHECK(porta->x() > voicing->x() + voicing->width() - 0.5f);
	// NOTHING OVERLAPS, bar the two axis boxes over the view's bands.
	std::vector<visage::Frame*> shown;
	for (size_t i = 0; i < static_cast<size_t>(rsn::Param::kCount); ++i) {
		visage::Frame* w = face.widgetFor(static_cast<rsn::Param>(i));
		if (w != nullptr && w->isVisible() && std::find(shown.begin(), shown.end(), w) == shown.end()) {
			shown.push_back(w);
		}
	}
	shown.push_back(face.meter());
	shown.push_back(face.ringView());
	const visage::Frame* axis[] = { face.widgetFor(rsn::Param::Character), face.widgetFor(rsn::Param::Morph) };
	for (size_t i = 0; i < shown.size(); ++i) {
		CHECK(shown[i]->x() + shown[i]->width() <= min.x + 0.01f);
		CHECK(shown[i]->y() + shown[i]->height() <= min.y + 0.01f);
		for (size_t j = i + 1; j < shown.size(); ++j) {
			const bool axisOverRing = (shown[i] == face.ringView() || shown[j] == face.ringView())
			                          && (std::find(std::begin(axis), std::end(axis), shown[i]) != std::end(axis)
			                              || std::find(std::begin(axis), std::end(axis), shown[j]) != std::end(axis));
			if (!axisOverRing) {
				CAPTURE(i);
				CAPTURE(j);
				CHECK_FALSE(overlaps(shown[i], shown[j]));
			}
		}
	}
}
TEST_CASE("RESONATE's panel: the Chord over its Tuning, two drone rows, every drone control greys at Drone Out 0 and a greyed cell ignores the hand, and FILTER and OUT under them") {
	// THE HONESTY RULE WHERE THE GATE CANNOT REACH: a gate is a mask over stepped values and cannot say "level above
	// 0", so the panel reads the drone's output and greys its knobs. (The Phase and Chord are value boxes, which have
	// no disabled state.) And the chosen layout, 2026-09-26: the Phase box directly above the Chord box as one unit; Drone
	// Out in OUT beside Blend, Level and Mix; FILTER and OUT moved under the drone.
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel->update();
	panel->resized();
	const auto enabled = [&](rsn::Param p) {
		auto* k = dynamic_cast<ui::Knob*>(panel->face().widgetFor(p));
		REQUIRE(k != nullptr);
		return k->enabled();
	};
	const rsn::Param dependants[] = { rsn::Param::DroneExcite, rsn::Param::Root, rsn::Param::DroneAttack,
		                              rsn::Param::DroneDecay, rsn::Param::DroneWidth };
	REQUIRE(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::DroneOut)) > 0.0f);
	for (const rsn::Param p : dependants) {
		CHECK(enabled(p));
	}
	d.set(static_cast<size_t>(rsn::Param::DroneOut), 0.0f);
	(void)panel->update();
	for (const rsn::Param p : dependants) {
		CAPTURE(std::string(rsn::kParamDescs[static_cast<size_t>(p)].name));
		CHECK_FALSE(enabled(p));
	}
	CHECK(enabled(rsn::Param::DroneOut));
	d.set(static_cast<size_t>(rsn::Param::DroneOut), 0.25f);
	(void)panel->update();
	for (const rsn::Param p : dependants) {
		CHECK(enabled(p));
	}
	const auto at = [&](rsn::Param p) { return panel->face().widgetFor(p); };
	// THE CHORD UNIT: the Chord box over the Tuning chip, the same left edge.
	auto* chord = dynamic_cast<ui::ValueBox*>(at(rsn::Param::DroneChord));
	auto* tuning = dynamic_cast<ui::SwitchChip*>(at(rsn::Param::DroneTuning));
	REQUIRE(chord != nullptr);
	REQUIRE(tuning != nullptr);
	CHECK(std::fabs(tuning->x() - chord->x()) < 0.5f);
	CHECK(chord->y() + chord->height() <= tuning->y() + 0.5f);
	// THEN THE DRONE KNOBS, ONE ROW to the unit's right, in signal order.
	const rsn::Param row[] = { rsn::Param::DroneExcite, rsn::Param::Root, rsn::Param::DroneAttack, rsn::Param::DroneDecay,
		                       rsn::Param::DroneWidth };
	CHECK(at(row[0])->x() >= chord->x() + chord->width() - 0.5f);
	for (size_t i = 1; i < std::size(row); ++i) {
		CAPTURE(i);
		CHECK(at(row[i])->x() >= at(row[i - 1])->x() + at(row[i - 1])->width() - 0.5f);
		CHECK(std::fabs(at(row[i])->y() - at(row[0])->y()) < 0.5f);
	}
	// THE SECOND ROW UNDER IT: the two zone cells (Drone Character, Cycle), the Cycle's division box, then the voice's two.
	auto* character = dynamic_cast<ui::ZoneCoordCell*>(at(rsn::Param::DroneCharacter));
	REQUIRE(character != nullptr);
	REQUIRE(dynamic_cast<ui::ZoneCoordCell*>(at(rsn::Param::DroneCycle)) != nullptr);
	const rsn::Param row2[] = { rsn::Param::DroneCharacter, rsn::Param::DroneCycle, rsn::Param::DroneCycleSync,
		                        rsn::Param::DroneVoice, rsn::Param::DroneStiffness };
	// THE DIVISION BOX IS A BOX (a Choice with a menu), centred in the row rather than topped with it.
	REQUIRE(dynamic_cast<ui::ValueBox*>(at(rsn::Param::DroneCycleSync)) != nullptr);
	CHECK(at(row2[0])->y() >= at(row[0])->y() + at(row[0])->height() - 0.5f);
	CHECK(std::fabs(at(row2[0])->x() - chord->x()) < 0.5f);
	for (size_t i = 1; i < std::size(row2); ++i) {
		CAPTURE(i);
		CHECK(at(row2[i])->x() >= at(row2[i - 1])->x() + at(row2[i - 1])->width() - 0.5f);
		CHECK(at(row2[i])->y() >= at(row2[0])->y() - 0.5f);
		CHECK(at(row2[i])->y() + at(row2[i])->height() <= at(row2[0])->y() + at(row2[0])->height() + 0.5f);
	}
	// THE TWO STACKS ON THE LEFT (v21: everything in two stacks of knobs to the left): the notes' (Pluck, Jawari, Curve, Bounce,
	// Sympathy) one under another, the drone's (Pluck, Jawari, Thread, Bounce, Sympathy) beside them row for row (Thread in Curve's row:
	// the strings' point has no shape, but a place), both left of the string view, under EXCITE, every knob at least 50 px wide.
	{
		const rsn::Param notes[] = { rsn::Param::Pluck, rsn::Param::Jawari, rsn::Param::BridgeCurve, rsn::Param::Bounce, rsn::Param::Sympathy };
		const rsn::Param drone[] = { rsn::Param::DronePluck, rsn::Param::DroneJawari, rsn::Param::DroneThread, rsn::Param::DroneBounce,
		                             rsn::Param::DroneSympathy };
		const int droneRow[] = { 0, 1, 2, 3, 4 };
		const float x0 = at(notes[0])->x();
		const float y0 = at(notes[0])->y();
		const float h = at(notes[0])->height();
		CHECK(y0 < at(rsn::Param::DroneChord)->y() + 30.0f); // v22: the stacks start at the top (EXCITE went under the string view)
		for (size_t i = 0; i < std::size(notes); ++i) {
			CAPTURE(i);
			CHECK(std::fabs(at(notes[i])->x() - x0) < 0.5f);
			CHECK(std::fabs(at(notes[i])->y() - (y0 + h * static_cast<float>(i))) < 0.5f);
			CHECK(at(notes[i])->width() >= 50.0f);
		}
		for (size_t i = 0; i < std::size(drone); ++i) {
			CAPTURE(i);
			CHECK(std::fabs(at(drone[i])->x() - (x0 + at(notes[0])->width())) < 0.5f);
			CHECK(std::fabs(at(drone[i])->y() - (y0 + h * static_cast<float>(droneRow[i]))) < 0.5f);
			CHECK(at(drone[i])->width() >= 50.0f);
			CHECK(at(drone[i])->x() + at(drone[i])->width() <= panel->face().ringView()->x() + 0.5f);
		}
	}
	// FILTER AND OUT UNDER THE DRONE, one row: Size, Width, then Blend, Level, Mix, the drone's own output and its gourds.
	const rsn::Param out[] = { rsn::Param::Size, rsn::Param::Width, rsn::Param::Blend, rsn::Param::Level, rsn::Param::Mix,
		                       rsn::Param::DroneOut, rsn::Param::DroneBody, rsn::Param::DroneTwin };
	CHECK(at(out[0])->y() >= at(row2[0])->y() + at(row2[0])->height() - 0.5f);
	CHECK(std::fabs(at(out[0])->x() - chord->x()) < 0.5f); // moved left, under the drone
	for (size_t i = 1; i < std::size(out); ++i) {
		CAPTURE(i);
		CHECK(at(out[i])->x() >= at(out[i - 1])->x() + at(out[i - 1])->width() - 0.5f);
		CHECK(std::fabs(at(out[i])->y() - at(out[0])->y()) < 0.5f);
	}
	// EVERY KNOB KEEPS A WIDTH ITS CAPTION READS AT, the zone cells theirs and their zone names', and the Chord box its
	// longest name. The second row's four one-word knobs (Rate, Voice, Stiff, Body, Twin) read at 40.
	for (const rsn::Param p : { rsn::Param::DroneInput, rsn::Param::DroneExcite, rsn::Param::Root, rsn::Param::DroneAttack, rsn::Param::DroneDecay,
	                            rsn::Param::DroneWidth, rsn::Param::DroneVoice, rsn::Param::DroneStiffness }) {
		CAPTURE(std::string(rsn::kParamDescs[static_cast<size_t>(p)].name));
		CHECK(at(p)->width() >= 50.0f);
	}
	// THE OUT ROW'S EIGHT, one-word captions, at 44.
	for (const rsn::Param p : out) {
		CHECK(at(p)->width() >= 44.0f);
	}
	CHECK(at(rsn::Param::DroneCharacter)->width() >= 108.0f); // "DRONE CHARACTER" (at 100 it touched the cell's edge)
	CHECK(at(rsn::Param::DroneCycle)->width() >= 76.0f);
	CHECK(at(rsn::Param::DroneCycleSync)->width() >= 56.0f); // "4 barT"
	MESSAGE("Chord box " << chord->width() << " px");
	CHECK(chord->width() >= 90.0f);
	// THE CHARACTER CELL GREYS WITH THE KNOBS, and a greyed cell ignores the hand: a drag on its box at Drone Out 0
	// moves nothing, and the same drag moves it once the drone sounds.
	ui::ValueBox& phase = character->box();
	CHECK(phase.enabled());
	CHECK(chord->enabled());
	auto* frame = dynamic_cast<visage::Frame*>(view.get());
	REQUIRE(frame != nullptr);
	const visage::Point centre { character->x() + phase.x() + phase.width() * 0.5f,
		                         character->y() + phase.y() + phase.height() * 0.5f };
	const auto phaseValue = [&] { return d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::DroneCharacter)); };
	d.set(static_cast<size_t>(rsn::Param::DroneOut), 0.0f);
	(void)panel->update();
	CHECK_FALSE(phase.enabled());
	CHECK_FALSE(chord->enabled());
	const float before = phaseValue();
	dragThroughRoot(frame, centre, { centre.x, centre.y - 40.0f });
	CHECK(phaseValue() == before);
	d.set(static_cast<size_t>(rsn::Param::DroneOut), 0.5f);
	(void)panel->update();
	CHECK(phase.enabled());
	CHECK(chord->enabled());
	dragThroughRoot(frame, centre, { centre.x, centre.y - 40.0f });
	CHECK(phaseValue() != before); // not vacuous: the same drag moves it when enabled
	// THE CYCLE'S PATTERN GREYS WHILE ITS RATE IS 0 (it does nothing then), and lights with the rate.
	auto* cycle = dynamic_cast<ui::ZoneCoordCell*>(at(rsn::Param::DroneCycle));
	CHECK_FALSE(cycle->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneCycleSync), static_cast<float>(rsn::kCycleOneBar));
	(void)panel->update();
	CHECK(cycle->enabled());
	// NOTE RELEASE sits in TONE beside Poly | Mono, as a chip.
	CHECK(dynamic_cast<ui::SwitchChip*>(at(rsn::Param::HeadRelease)) != nullptr);
	CHECK(std::fabs(at(rsn::Param::HeadRelease)->y() + at(rsn::Param::HeadRelease)->height() * 0.5f
	                - (at(rsn::Param::Voicing)->y() + at(rsn::Param::Voicing)->height() * 0.5f)) < 1.0f);
}
TEST_CASE("RESONATE's Drone Excite greys at Drone Drive 0 and is live above it") {
	// THE HONESTY RULE (v20): Drone Excite chooses how the input reaches the drone's strings (through each string's ear, or
	// direct), and the engine's feed is Drone Drive x the input before that choice, so at Drone Drive 0 Excite changes nothing:
	// it greys there, and is live at any Drone Drive above 0 (with the drone on). Its tooltip says why.
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel->update();
	panel->resized();
	auto* excite = dynamic_cast<ui::Knob*>(panel->face().widgetFor(rsn::Param::DroneExcite));
	REQUIRE(excite != nullptr);
	REQUIRE(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::DroneInput)) > 0.0f);
	CHECK(excite->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneInput), 0.0f);
	(void)panel->update();
	CHECK_FALSE(excite->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneInput), 0.1f);
	(void)panel->update();
	CHECK(excite->enabled());
	CHECK(std::string(rsn::kParamDescs[static_cast<size_t>(rsn::Param::DroneExcite)].tooltip).find("acts only when Drone Drive is up")
	      != std::string::npos);
	CHECK(std::string(rsn::kParamDescs[static_cast<size_t>(rsn::Param::DroneInput)].name) == "Resonate Drone Drive");
}
TEST_CASE("RESONATE's Drone Bounce greys at Drone Jawari 0 and is live above it") {
	// THE HONESTY RULE (v21): Drone Bounce is how the drone strings come off their bridge, and at Drone Jawari 0 there is none
	// (NodeString contacts only while its jawari is above 0), so it greys there and is live above (with the drone on).
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel->update();
	panel->resized();
	auto* bounce = dynamic_cast<ui::Knob*>(panel->face().widgetFor(rsn::Param::DroneBounce));
	REQUIRE(bounce != nullptr);
	REQUIRE(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::DroneJawari)) == 0.0f);
	CHECK_FALSE(bounce->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneJawari), 0.4f);
	(void)panel->update();
	CHECK(bounce->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneOut), 0.0f);
	(void)panel->update();
	CHECK_FALSE(bounce->enabled());
}
TEST_CASE("RESONATE's Follow chip sits beside Voice and greys at Voice 0 and with the drone off; Drone Thread greys with Drone Bounce") {
	// v21: Follow gates the drone's scanned voices on the main string's motion, so at Drone Voice 0 (no scanned voice) or Drone
	// Out 0 it does nothing, and greys. It sits in the Transport column, beside the Voice knob. Drone Thread is the drone
	// strings' point's place, so like Drone Bounce it greys at Drone Jawari 0.
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	(void)panel->update();
	panel->resized();
	auto* follow = dynamic_cast<ui::SwitchChip*>(panel->face().widgetFor(rsn::Param::DroneFollow));
	auto* voice = panel->face().widgetFor(rsn::Param::DroneVoice);
	auto* transport = panel->face().widgetFor(rsn::Param::DroneCycleTransport);
	auto* thread = dynamic_cast<ui::Knob*>(panel->face().widgetFor(rsn::Param::DroneThread));
	REQUIRE(follow != nullptr);
	REQUIRE(voice != nullptr);
	REQUIRE(transport != nullptr);
	REQUIRE(thread != nullptr);
	CHECK(std::fabs(follow->x() - transport->x()) < 0.5f);
	CHECK(follow->y() >= transport->y() + transport->height() - 0.5f);
	CHECK(follow->x() + follow->width() <= voice->x() + 0.5f);
	CHECK(voice->x() - (follow->x() + follow->width()) < 12.0f);
	REQUIRE(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(rsn::Param::DroneVoice)) > 0.0f);
	CHECK(follow->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneVoice), 0.0f);
	(void)panel->update();
	CHECK_FALSE(follow->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneVoice), 0.5f);
	(void)panel->update();
	CHECK(follow->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneOut), 0.0f);
	(void)panel->update();
	CHECK_FALSE(follow->enabled());
	d.set(static_cast<size_t>(rsn::Param::DroneOut), 0.5f);
	(void)panel->update();
	CHECK_FALSE(thread->enabled()); // Drone Jawari 0 (its default)
	d.set(static_cast<size_t>(rsn::Param::DroneJawari), 0.4f);
	(void)panel->update();
	CHECK(thread->enabled());
}
TEST_CASE("RESONATE's Size reads as a whole number of taps, while its value stays continuous") {
	// THE LEAD, 2026-09-26: Size read "64.00", and taps are heard as whole numbers. Display only.
	const device::ParamDesc& size = rsn::kParamDescs[static_cast<size_t>(rsn::Param::Size)];
	REQUIRE_FALSE(size.stepped);
	CHECK(device::formatValue(size, 64.0) == "64");
	CHECK(device::formatValue(size, 64.4) == "64");
	CHECK(device::formatValue(size, 64.6) == "65");
	CHECK(device::formatValue(size, 256.0) == "256");
	// AND ON THE CARD'S KNOB, the readout a hand sees, over a value that keeps its fraction.
	ResonateFixture d(rsn::kTraits.tag);
	d.set(static_cast<size_t>(rsn::Param::Size), 100.3f);
	(void)d.card->updateContent();
	auto* knob = dynamic_cast<ui::Knob*>(d.card->widgetForTest(rsn::Param::Size));
	REQUIRE(knob != nullptr);
	REQUIRE(knob->control() != nullptr);
	CHECK(knob->control()->value() == doctest::Approx(100.3).epsilon(1e-6));
	CHECK(knob->control()->text() == "100");
	// A CONTINUOUS NUMBER THAT DID NOT ASK KEEPS ITS TWO PLACES — the field is Size's, not every Number's.
	device::ParamDesc plain = size;
	plain.wholeReadout = false;
	CHECK(device::formatValue(plain, 64.0) == "64.00");
}
TEST_CASE("RESONATE's modulatable controls are mod destinations on the surface that seats them — Size on the card, the drone's on the panel") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ModState* mod = d.f.gui.modForTest();
	REQUIRE(mod != nullptr);
	d.card->setModulation(mod, d.f.gui.history());
	(void)d.card->updateContent();
	d.f.arm(backplane::Source::core(0));
	REQUIRE(mod->armed());
	// SIZE, NOW CONTINUOUS AND MODULATABLE (2026-09-26): armed on the card, and a drag routes it.
	auto* size = dynamic_cast<ui::Knob*>(d.card->widgetForTest(rsn::Param::Size));
	REQUIRE(size != nullptr);
	REQUIRE(size->control() != nullptr);
	const device::ModDisplay shown = size->control()->display();
	CHECK(shown.isDestination);
	CHECK(shown.armed);
	CHECK(shown.routable);
	const auto dest = backplane::Destination::control(d.device, static_cast<size_t>(rsn::Param::Size));
	REQUIRE(d.f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
	dragThroughRoot(d.f.root, d.f.faceCentre(size), { d.f.faceCentre(size).x, d.f.faceCentre(size).y - 30.0f });
	CHECK(d.f.bridge.routeAmount(backplane::Source::core(0), dest) != 0.0f);
	// THE DRONE'S THREE AND ITS ROOT ON THE PANEL: every modulatable one a destination, armed and routable.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setModulation(mod, d.f.gui.history());
	for (const rsn::Param p : { rsn::Param::DroneOut, rsn::Param::DroneAttack, rsn::Param::DroneDecay, rsn::Param::Size }) {
		CAPTURE(std::string(rsn::kParamDescs[static_cast<size_t>(p)].name));
		REQUIRE(rsn::kParamDescs[static_cast<size_t>(p)].modulatable);
		auto* k = dynamic_cast<ui::Knob*>(panel->face().widgetFor(p));
		REQUIRE(k != nullptr);
		const device::ModDisplay m = k->control()->display();
		CHECK(m.isDestination);
		CHECK(m.armed);
		CHECK(m.routable);
	}
	// THE DRONE'S CHARACTER, A ZONED COORDINATE IN ITS CELL, and its WIDTH: the same road (the Chord, a Choice, is not one).
	CHECK_FALSE(rsn::kParamDescs[static_cast<size_t>(rsn::Param::DroneChord)].modulatable);
	{
		REQUIRE(rsn::kParamDescs[static_cast<size_t>(rsn::Param::DroneCharacter)].modulatable);
		auto* cell = dynamic_cast<ui::ZoneCoordCell*>(panel->face().widgetFor(rsn::Param::DroneCharacter));
		REQUIRE(cell != nullptr);
		const device::ModDisplay m = cell->box().control().display();
		CHECK(m.isDestination);
		CHECK(m.armed);
		CHECK(m.routable);
		auto* width = dynamic_cast<ui::Knob*>(panel->face().widgetFor(rsn::Param::DroneWidth));
		REQUIRE(width != nullptr);
		CHECK(width->control()->display().isDestination);
	}
	// v10, THE 32-CONTROL STRIDE (decided 2026-09-28): String is a destination, String Excite and Porta are NOT —
	// no mod ring, no drop target (the honesty rule: a control the hand cannot route shows nothing to route to).
	{
		auto* blend = dynamic_cast<ui::ValueBox*>(panel->face().widgetFor(rsn::Param::StringBlend));
		auto* excite = dynamic_cast<ui::ValueBox*>(panel->face().widgetFor(rsn::Param::StringExcite));
		auto* porta = dynamic_cast<ui::ValueBox*>(panel->face().widgetFor(rsn::Param::Porta));
		REQUIRE(blend != nullptr);
		REQUIRE(excite != nullptr);
		REQUIRE(porta != nullptr);
		const device::ModDisplay mb = blend->control().display();
		CHECK(mb.isDestination);
		CHECK(mb.routable);
		for (ui::ValueBox* box : { excite, porta }) {
			const device::ModDisplay m = box->control().display();
			CHECK_FALSE(m.isDestination);
			CHECK_FALSE(m.routable);
		}
		// v11 (Twin and Body had very little impact on the sound): Root takes Twin's slot — Root a destination,
		// Twin not.
		auto* root = dynamic_cast<ui::Knob*>(panel->face().widgetFor(rsn::Param::Root));
		auto* twin = dynamic_cast<ui::Knob*>(panel->face().widgetFor(rsn::Param::DroneTwin));
		REQUIRE(root != nullptr);
		REQUIRE(twin != nullptr);
		CHECK(root->control()->display().isDestination);
		CHECK(root->control()->display().routable);
		CHECK_FALSE(twin->control()->display().isDestination);
		CHECK_FALSE(twin->control()->display().routable);
	}
	// AND EVERY MODULATABLE CONTROL OF THE DEVICE IS A DESTINATION ON ONE SURFACE OR THE OTHER.
	for (size_t i = 0; i < static_cast<size_t>(rsn::Param::kCount); ++i) {
		const auto p = static_cast<rsn::Param>(i);
		// THE ENABLE IS A DESTINATION ON THE HEADER CHIP (backlog KW), not on a face.
		if (!rsn::kParamDescs[i].modulatable || p == rsn::Param::Enabled) {
			continue;
		}
		CAPTURE(std::string(rsn::kParamDescs[i].name));
		CHECK((d.card->widgetForTest(p) != nullptr || panel->face().widgetFor(p) != nullptr));
	}
}
TEST_CASE("RESONATE's card is a design call simplified face: the string, the pictograms and six knobs, nothing else") {
	// The card was far too big, so it is greatly simplified.
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateCard* card = d.card;
	(void)card->updateContent();
	const std::vector<rsn::Param> addressed = card->addressedParamsForTest();
	using P = rsn::Param;
	const std::set<P> want { P::Drive, P::Size, P::Pluck, P::Blend, P::Sustain, P::Mix,
		                     P::Model, P::Character, P::Morph, P::Enabled };
	CHECK(std::set<P>(addressed.begin(), addressed.end()) == want);
	// NOTHING ELSE IS SEATED: no meter, no tone block, no Bow, no drone.
	CHECK(card->face().meter() == nullptr);
	for (const P p : { P::Bow, P::Voicing, P::Porta, P::Octave, P::Semitone, P::Fine, P::DroneOut, P::Root,
	                   P::DroneAttack, P::DroneDecay, P::DroneChord, P::Width, P::Level }) {
		CAPTURE(std::string(rsn::kParamDescs[static_cast<size_t>(p)].name));
		CHECK(card->widgetForTest(p) == nullptr);
	}
	// THE SIX, IN THE ORDER, READ ACROSS TWO ROWS OF THREE (one row of six ellipsised every caption).
	const P order[] = { P::Drive, P::Size, P::Pluck, P::Blend, P::Sustain, P::Mix };
	for (size_t i = 0; i < std::size(order); ++i) {
		visage::Frame* k = card->widgetForTest(order[i]);
		REQUIRE(k != nullptr);
		CHECK(dynamic_cast<ui::Knob*>(k) != nullptr);
		visage::Frame* rowStart = card->widgetForTest(order[(i / 3) * 3]);
		CHECK(k->y() == doctest::Approx(rowStart->y()));
		if (i % 3 > 0) {
			CHECK(k->x() > card->widgetForTest(order[i - 1])->x());
		}
	}
	CHECK(card->widgetForTest(P::Blend)->y() > card->widgetForTest(P::Drive)->y());
	CHECK(card->widgetForTest(P::Blend)->x() == doctest::Approx(card->widgetForTest(P::Drive)->x()));

	// NOTHING OVERLAPS — except Character's and Morph's boxes, which are seated OVER the view's bands by
	// design, and must then lie inside those bands and clear of the plot.
	const ui::ResonateRingView* ring = card->face().ringView();
	const visage::Frame* axisBoxes[] = { card->widgetForTest(rsn::Param::Character),
		                                 card->widgetForTest(rsn::Param::Morph) };
	std::vector<visage::Frame*> shown;
	for (const rsn::Param p : addressed) {
		visage::Frame* w = card->widgetForTest(p);
		if (w != nullptr && w->isVisible() && std::find(shown.begin(), shown.end(), w) == shown.end()) {
			shown.push_back(w);
		}
	}
	for (visage::Frame* w : { static_cast<visage::Frame*>(const_cast<ui::ResonateRingView*>(ring)),
	                          static_cast<visage::Frame*>(card->face().modelPicker()) }) {
		if (std::find(shown.begin(), shown.end(), w) == shown.end()) {
			shown.push_back(w);
		}
	}
	for (size_t i = 0; i < shown.size(); ++i) {
		CHECK(shown[i]->x() >= card->bodyLeft() - 0.01f);
		CHECK(shown[i]->x() + shown[i]->width() <= static_cast<float>(card->width()) + 0.01f);
		CHECK(shown[i]->y() + shown[i]->height() <= card->laneHeightAt(static_cast<float>(card->width())) + 0.01f);
		for (size_t j = i + 1; j < shown.size(); ++j) {
			const bool axisOverRing = (shown[i] == ring || shown[j] == ring)
			                          && (std::find(std::begin(axisBoxes), std::end(axisBoxes), shown[i]) != std::end(axisBoxes)
			                              || std::find(std::begin(axisBoxes), std::end(axisBoxes), shown[j]) != std::end(axisBoxes));
			if (!axisOverRing) {
				CAPTURE(i);
				CAPTURE(j);
				CHECK_FALSE(overlaps(shown[i], shown[j]));
			}
		}
	}
	const visage::Bounds plot = ring->plotBounds();
	for (const visage::Frame* box : axisBoxes) {
		const float top = box->y() - ring->y();
		const float bottom = top + box->height();
		CHECK((bottom <= plot.y() + 0.01f || top >= plot.bottom() - 0.01f));
	}
	for (visage::Frame* w : shown) {
		if (w != ring) {
			CHECK(w->width() * w->height() < ring->width() * ring->height());
		}
	}
}
TEST_CASE("RESONATE's Character and Morph are mod destinations: armed, a drag on their box routes the source, and the puck follows the MODULATED value") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateCard* card = d.card;
	(void)card->updateContent();
	ui::ModState* mod = d.f.gui.modForTest();
	REQUIRE(mod != nullptr);
	card->setModulation(mod, d.f.gui.history());
	d.f.arm(backplane::Source::core(0));
	REQUIRE(mod->armed());
	for (const rsn::Param p : { rsn::Param::Character, rsn::Param::Morph }) {
		CAPTURE(std::string(rsn::kParamDescs[static_cast<size_t>(p)].name));
		auto* box = dynamic_cast<ui::ValueBox*>(card->widgetForTest(p));
		REQUIRE(box != nullptr);
		REQUIRE(box->isVisible());
		// THE GLOW'S OWN FACTS, asked of the control the box holds.
		const device::ModDisplay shown = box->control().display();
		CHECK(shown.isDestination);
		CHECK(shown.armed);
		CHECK(shown.routable);
		// THE ROUTE, THROUGH THE REAL ROAD: a drag on the box while armed sets an amount on the host.
		const auto dest = backplane::Destination::control(d.device, static_cast<size_t>(p));
		REQUIRE(d.f.bridge.routeAmount(backplane::Source::core(0), dest) == 0.0f);
		const visage::Point c = centreInRoot(box);
		dragThroughRoot(d.f.root, c, { c.x, c.y - 30.0f });
		CHECK(d.f.bridge.routeAmount(backplane::Source::core(0), dest) != 0.0f);
		CHECK(box->control().amountFor(backplane::Source::core(0)) != 0.0f);
	}

	// THE PUCK AT THE MODULATED PAIR, THE GHOST AT THE BASE: the audio side publishes Morph moved.
	ui::ResonateRingView* ring = card->face().ringView();
	(void)card->updateContent();
	const visage::Point before = ring->puckPoint();
	CHECK(before.y == doctest::Approx(ring->puckPoint(true).y));
	feathers::FxChain moved = d.f.bridge.fxChain();
	const float base = moved.device[d.slot].row.field[static_cast<size_t>(rsn::Param::Morph)];
	moved.device[d.slot].row.field[static_cast<size_t>(rsn::Param::Morph)] = base + 0.4f;
	d.f.bridge.publishModulated(d.f.bridge.modRack(), moved, feathers::RackPlayheads {}, feathers::mod::TransportClock::unknown());
	CHECK(ring->refresh());
	const visage::Bounds plot = ring->plotBounds();
	CHECK(ring->puckPoint().y == doctest::Approx(plot.bottom() - plot.height() * (base + 0.4f)).epsilon(1e-4));
	CHECK(ring->puckPoint(true).y == doctest::Approx(plot.bottom() - plot.height() * base).epsilon(1e-4));
}
TEST_CASE("RESONATE's ring view draws each model from a synthetic Reading, on its own geometry") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	REQUIRE(ring != nullptr);
	(void)d.card->updateContent();
	for (int m = 0; m < static_cast<int>(rsn::Model::kCount); ++m) {
		const auto model = static_cast<rsn::Model>(m);
		CAPTURE(m);
		publishReading(d, syntheticReading(model));
		(void)d.card->updateContent();
		REQUIRE(ring->hasReading());
		CHECK(ring->model() == model);
		const visage::Bounds box = ring->plotBounds();
		const float cx = box.x() + box.width() * 0.5f;
		const float cy = box.y() + box.height() * 0.5f;
		const std::vector<visage::Point> live = ring->livePath();
		const std::vector<visage::Point> rest = ring->restPath();
		REQUIRE(live.size() == rest.size());
		switch (model) {
		case rsn::Model::Ring:
		case rsn::Model::Stiffening: {
			REQUIRE(live.size() == 32);
			const auto radius = [&](visage::Point p) { return std::hypot(p.x - cx, p.y - cy); };
			CHECK(radius(live[8]) > radius(rest[8]));
			CHECK(radius(live[24]) < radius(rest[24]));
			CHECK(std::fabs(live[0].x - cx) < 0.01f);
			CHECK(live[0].y < cy);
			break;
		}
		case rsn::Model::Bar:
			REQUIRE(live.size() == 32);
			CHECK(live.front().x == doctest::Approx(box.x()));
			CHECK(live.back().x == doctest::Approx(box.right()));
			CHECK(live[8].y < cy);
			CHECK(live[24].y > cy);
			break;
		case rsn::Model::Tube:
			REQUIRE(live.size() == 34);
			CHECK(live.front().x == doctest::Approx(box.x()));
			CHECK(live.front().y == doctest::Approx(cy));
			CHECK(live.back().x == doctest::Approx(box.right()));
			CHECK(live.back().y == doctest::Approx(cy));
			break;
		case rsn::Model::Membrane: {
			REQUIRE(live.size() == 32);
			// THE SCAN CIRCLE, radius 0.6 of the grid's half-side.
			const float r = std::min(box.width(), box.height()) * 0.5f * 0.6f;
			for (const visage::Point p : live) {
				CHECK(std::hypot(p.x - cx, p.y - cy) == doctest::Approx(r).epsilon(1e-4));
			}
			// THE CELLS ARE ROW-MAJOR WITH ROW 0 AT THE TOP: node 1 right of node 0, node 8 below it, node 63
			// at the bottom right — restated from the layout, not read back.
			const visage::Bounds n0 = ring->membraneCell(0);
			CHECK(ring->membraneCell(1).x() > n0.x());
			CHECK(ring->membraneCell(1).y() == doctest::Approx(n0.y()));
			CHECK(ring->membraneCell(8).y() > n0.y());
			CHECK(ring->membraneCell(8).x() == doctest::Approx(n0.x()));
			CHECK(ring->membraneCell(63).right() == doctest::Approx(n0.x() + 8.0f * n0.width()));
			CHECK(ring->membraneCell(63).bottom() == doctest::Approx(n0.y() + 8.0f * n0.height()));
			CHECK(n0.x() >= box.x() - 0.01f);
			CHECK(n0.y() >= box.y() - 0.01f);
			break;
		}
		case rsn::Model::kCount: break;
		}
	}
}
TEST_CASE("RESONATE's scan marker sits at the TRAVEL phase, out and back on Bar and Tube") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	(void)d.card->updateContent();
	// RING: a quarter of the way round from the top, clockwise, is node 8 — due right of the centre.
	publishReading(d, syntheticReading(rsn::Model::Ring, 0.5f, 0.25f));
	(void)d.card->updateContent();
	const visage::Bounds box = ring->plotBounds();
	CHECK(ring->travelPoint().x == doctest::Approx(ring->livePath()[8].x));
	CHECK(ring->travelPoint().x > box.x() + box.width() * 0.5f);
	// BAR: p = 0.75 is on the way BACK, at node 15.5 — x = 15.5 / 31 of the plot.
	publishReading(d, syntheticReading(rsn::Model::Bar, 0.5f, 0.75f));
	(void)d.card->updateContent();
	CHECK(ring->travelPoint().x == doctest::Approx(box.x() + box.width() * (15.5f / 31.0f)));
	// TUBE: the same node, between the pins — x = (15.5 + 1) / 33 of the plot.
	publishReading(d, syntheticReading(rsn::Model::Tube, 0.5f, 0.75f));
	(void)d.card->updateContent();
	CHECK(ring->travelPoint().x == doctest::Approx(box.x() + box.width() * (16.5f / 33.0f)));
	// AND THE SOUNDING HEAD IS A CHIP, its note and its brightness from the reading.
	const std::vector<ui::ResonateRingView::Chip> chips = ring->chips();
	REQUIRE(chips.size() == 1);
	CHECK(chips[0].note == 60.0f);
	CHECK(chips[0].alpha == doctest::Approx(1.0f));
}
TEST_CASE("RESONATE's ring view and meter repaint ZERO times at rest — even with the Drone's head reading round — and once when the travel moves") {
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	// THE METER IS THE PANEL'S NOW (the simplified card has none), so the panel is opened for real.
	ui::ResonateExpanded* panel = openResonatePanel(d);
	ui::ResonateMeter* meter = panel->face().meter();
	REQUIRE(meter != nullptr);
	REQUIRE(ring->isVisible());
	REQUIRE(ring->isDrawing());
	REQUIRE(meter->isDrawing());
	const auto tick = [&] {
		(void)d.card->updateContent();
		(void)panel->update();
	};
	rsn::Reading r = syntheticReading(rsn::Model::Ring, 0.5f, 0.1f);
	r.inLevel = 0.3f;
	publishReading(d, r);
	tick();
	RedrawProbe ringProbe(ring);
	RedrawProbe meterProbe(meter);

	// AT REST, WITH THE DRONE SOUNDING: the head's audio-rate phase moves every tick, the pluck's age counts
	// up past the flash, the energy wanders — and none of it is drawn, so nothing repaints.
	for (int i = 0; i < 6; ++i) {
		r.heads[0] = 0.13f * static_cast<float>(i);
		r.pluckAge = 200.0f + static_cast<float>(i);
		r.energy = 0.01f * static_cast<float>(i);
		publishReading(d, r);
		ringProbe.rearm();
		meterProbe.rearm();
		tick();
		CHECK(ringProbe.count == 0);
		CHECK(meterProbe.count == 0);
	}
	// THE TRAVEL MOVES: one redraw of the view, none of the meter.
	r.travel = 0.3f;
	publishReading(d, r);
	ringProbe.rearm();
	meterProbe.rearm();
	tick();
	CHECK(ringProbe.count == 1);
	CHECK(meterProbe.count == 0);
	// THE INPUT'S LEVEL MOVES: one redraw of the meter.
	r.inLevel = 0.6f;
	publishReading(d, r);
	meterProbe.rearm();
	tick();
	CHECK(meterProbe.count == 1);
}
TEST_CASE("RESONATE's ring view does not repaint for a travel it does not draw: nothing sounding, the marker hidden") {
	// MEASURED ON THE REAL DSP: with Bow up and no head sounding the travel keeps moving while the string is
	// still, and the marker is drawn only while a head sounds — so a moving travel alone is not a change.
	ResonateFixture d(rsn::kTraits.tag);
	ui::ResonateRingView* ring = d.card->face().ringView();
	REQUIRE(ring->isDrawing());
	rsn::Reading r = syntheticReading(rsn::Model::Ring, 0.5f, 0.1f);
	r.headGains = {};
	publishReading(d, r);
	(void)d.card->updateContent();
	RedrawProbe probe(ring);
	for (int i = 1; i <= 4; ++i) {
		r.travel = 0.1f + 0.05f * static_cast<float>(i);
		publishReading(d, r);
		probe.rearm();
		(void)d.card->updateContent();
		CHECK(probe.count == 0);
	}
	// A HEAD SOUNDS: the marker appears, and from then on the travel is drawn and counts.
	r.headGains[0] = 1.0f;
	publishReading(d, r);
	probe.rearm();
	(void)d.card->updateContent();
	CHECK(probe.count == 1);
	r.travel += 0.05f;
	publishReading(d, r);
	probe.rearm();
	(void)d.card->updateContent();
	CHECK(probe.count == 2);
}
TEST_CASE("RESONATE shows a chip for every sounding head: an eight-string raga and two held notes on the card and the panel") {
	// THE OVERFLOW (2026-09-27): a fixed 26 px chip fitted about six, so a raga's eight strings under held notes lost
	// strings. Ten sounding heads must give ten chips, the played notes first and on the band itself, none
	// overlapping, none narrower than a note name, all inside the view — on the card's view and the panel's.
	ResonateFixture d(rsn::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, rsn::kDescriptor);
	auto* panel = dynamic_cast<ui::ResonateExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, min.x, min.y);
	rsn::Reading r = syntheticReading(rsn::Model::Ring);
	r.headNotes = {};
	r.headGains = {};
	r.headNotes[0] = 64.0f;
	r.headGains[0] = 0.8f;
	r.headNotes[1] = 67.0f;
	r.headGains[1] = 0.5f;
	r.droneCount = 8.0f;
	r.droneNotes = { 48.0f, 50.0f, 52.0f, 53.0f, 55.0f, 57.0f, 59.0f, 60.0f };
	r.droneGains = { 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f, 0.3f, 0.2f };
	publishReading(d, r);
	(void)panel->update();
	(void)d.card->updateContent();
	const ui::ResonateRingView* views[] = { panel->face().ringView(), d.card->face().ringView() };
	for (const ui::ResonateRingView* ring : views) {
		CAPTURE(ring->width());
		const std::vector<ui::ResonateRingView::Chip> chips = ring->chips();
		REQUIRE(chips.size() == 10);
		const float bandY = ring->characterBand().y();
		for (size_t i = 0; i < chips.size(); ++i) {
			CAPTURE(i);
			const visage::Bounds& b = chips[i].box;
			CHECK(chips[i].note == (i < 2 ? r.headNotes[i] : r.droneNotes[i - 2]));
			CHECK(chips[i].drone == (i >= 2));
			CHECK(b.width() >= ui::ResonateRingView::kChipMinWidth);
			CHECK(b.x() >= ring->characterBand().right());
			CHECK(b.right() <= static_cast<float>(ring->width()));
			CHECK(b.y() >= 0.0f);
			CHECK(b.bottom() <= static_cast<float>(ring->height()));
			// AND CLEAR OF THE PLOT, so neither the puck nor a cell ever sits under a chip.
			CHECK(b.y() >= ring->plotBounds().bottom());
			for (size_t j = 0; j < i; ++j) {
				const visage::Bounds& o = chips[j].box;
				const bool apart = b.right() <= o.x() || o.right() <= b.x() || b.bottom() <= o.y() || o.bottom() <= b.y();
				CHECK(apart);
			}
		}
		// THE PLAYED NOTES ON THE BAND ITSELF, the rightmost two.
		CHECK(chips[0].box.y() == bandY);
		CHECK(chips[1].box.y() == bandY);
		CHECK(chips[0].box.x() > chips[1].box.x());
	}
}

