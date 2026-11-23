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

// PIECE 14 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("STEPSEQ card: it shows only the controls that change sound — Rate and Base under Clock = Rate, Lock under "
          "Turing, Poly under Retrig — and every control it seats is one it declares") {
	ModFixture f;
	const StepSeqSeat seat = seatStepSeq(f);
	ui::StepSeqCard& card = *seat.card;
	(void)card.updateContent();
	CHECK(card.rateKnob().isVisible());
	CHECK(card.timeBaseBox().isVisible());
	CHECK_FALSE(card.lockKnob().isVisible()); // Turing is off by default
	CHECK(card.polyChip().isVisible());       // Retrig is on by default

	f.deviceHost.setField(seat.device, ssq::at(ssq::Param::Clock), static_cast<float>(ssq::Clock::Trig));
	f.deviceHost.setField(seat.device, ssq::at(ssq::Param::Turing), 1.0f);
	f.deviceHost.setField(seat.device, ssq::at(ssq::Param::Retrig), 0.0f);
	(void)card.updateContent();
	CHECK_FALSE(card.rateKnob().isVisible());
	CHECK_FALSE(card.timeBaseBox().isVisible());
	CHECK(card.lockKnob().isVisible());
	CHECK_FALSE(card.polyChip().isVisible());

	// EVERY CONTROL BUT THE ENABLE (the head's square) HAS A WIDGET ON THE CARD, each addressed once.
	std::vector<size_t> addressed = card.addressedParamsForTest();
	std::sort(addressed.begin(), addressed.end());
	CHECK(std::adjacent_find(addressed.begin(), addressed.end()) == addressed.end());
	addressed.push_back(ssq::at(ssq::Param::Enabled));
	std::sort(addressed.begin(), addressed.end());
	std::vector<size_t> all(ssq::kParamCount);
	for (size_t i = 0; i < all.size(); ++i) {
		all[i] = i;
	}
	CHECK(addressed == all);
	// THE TWO PADS ARE THE ADVANCE AND THE RESET.
	CHECK(dynamic_cast<const ui::DeviceControl&>(card.advancePad().control()).control() == ssq::at(ssq::Param::Trig));
	CHECK(dynamic_cast<const ui::DeviceControl&>(card.resetPad().control()).control() == ssq::at(ssq::Param::Reset));
}
TEST_CASE("NU: the step sequencer's Reset pad and S&H's Trig pad carry the normal through the controls that already store it — Retrig, and Clock Note or Trig") {
	ModFixture f;
	const StepSeqSeat seq = seatStepSeq(f);
	const SampleHoldSeat snhSeat = seatSampleHold(f);
	ui::EditHistory* history = f.bridge.editHistory();
	REQUIRE(history != nullptr);

	// THE SEQUENCER: Reset carries it, Advance does not (no note advances a pattern).
	ui::TrigPad& reset = seq.card->resetPad();
	CHECK_FALSE(seq.card->advancePad().hasNormal());
	REQUIRE(reset.hasNormal());
	CHECK(reset.normalled()); // Retrig is on by default
	{
		visage::PopupMenu menu = reset.menuForTest();
		REQUIRE(menu.size() == 1);
		CHECK(menu.options()[0].selected());
		menu.onSelection().callback(ui::TrigPad::kNormalRow);
	}
	CHECK(f.deviceHost.fieldAt(seq.device, ssq::at(ssq::Param::Retrig)) == 0.0f);
	CHECK_FALSE(reset.normalled());
	history->undo();
	CHECK(f.deviceHost.fieldAt(seq.device, ssq::at(ssq::Param::Retrig)) == 1.0f);

	// S&H: OFFERED ONLY UNDER NOTE OR TRIG, checked under Note, flipping the two.
	ui::TrigPad& pad = snhSeat.card->trigPad();
	const DeviceInstanceId snhId = f.bridge.rackDeviceAt(snhSeat.slot);
	const size_t clock = static_cast<size_t>(snh::Param::Clock);
	REQUIRE(f.deviceHost.fieldAt(snhId, clock) == static_cast<float>(snh::Clock::Rate));
	CHECK_FALSE(pad.hasNormal());
	CHECK(pad.menuForTest().size() == 0);
	f.deviceHost.setField(snhId, clock, static_cast<float>(snh::Clock::Random));
	CHECK_FALSE(pad.hasNormal());
	f.deviceHost.setField(snhId, clock, static_cast<float>(snh::Clock::Note));
	REQUIRE(pad.hasNormal());
	CHECK(pad.normalled());
	pad.setNormalled(false);
	CHECK(f.deviceHost.fieldAt(snhId, clock) == static_cast<float>(snh::Clock::Trig));
	CHECK(pad.hasNormal());
	CHECK_FALSE(pad.normalled());
	CHECK_FALSE(pad.menuForTest().options()[0].selected());
	pad.setNormalled(true);
	CHECK(f.deviceHost.fieldAt(snhId, clock) == static_cast<float>(snh::Clock::Note));
}
TEST_CASE("NT: STEPSEQ card — each page has its own Randomise and Trig pad: a press randomises that page's lane, a pad fires that lane's Trig with no undo step, and a route into a hidden lane's Trig shows when its page opens") {
	ModFixture f;
	const StepSeqSeat seat = seatStepSeq(f);
	ui::StepSeqCard& card = *seat.card;
	ui::EditHistory* history = f.bridge.editHistory();
	REQUIRE(history != nullptr);

	// A ROUTE INTO THE SLEW PAGE'S TRIG WHILE THE VALUE PAGE IS SHOWN.
	const backplane::Source macro = backplane::Source::core(0);
	f.bridge.setRouteAmount(macro, backplane::Destination::control(seat.device, ssq::at(ssq::Param::RandSlew)), 1.0f);
	f.bridge.refresh();

	for (size_t l = 0; l < ssq::kLaneCount; ++l) {
		const auto lane = static_cast<ssq::Lane>(l);
		CAPTURE(l);
		card.grid().setLane(lane);
		// ONLY THIS PAGE'S PAIR ON THE FACE, and its pad is this lane's Trig.
		for (size_t o = 0; o < ssq::kLaneCount; ++o) {
			const auto other = static_cast<ssq::Lane>(o);
			CHECK(card.randomisePad(other).isVisible() == (o == l));
			CHECK(card.randomiseButton(other).isVisible() == (o == l));
		}
		CHECK(&card.randomiseButton() == &card.randomiseButton(lane));
		ui::TrigPad& pad = card.randomisePad(lane);
		CHECK(dynamic_cast<const ui::DeviceControl&>(pad.control()).control() == ssq::at(ssq::randomiseParam(lane)));
		// THE HIDDEN LANE'S ROUTE IS ON ITS PAD when its page opens, and only there.
		CHECK(ui::inboundSources(pad.control()).size() == (lane == ssq::Lane::Slew ? 1u : 0u));

		// THE PRESS: exactly this lane, one undo step.
		const ssq::Pattern before = card.grid().pattern();
		const size_t length = history->length();
		card.randomiseButton().press();
		ssq::Pattern drawn = ssq::randomise(before, ssq::kDefaultSteps, lane, 1.0f, 0);
		drawn.basis = card.grid().pattern().basis;
		CHECK(card.grid().pattern() == drawn);
		CHECK(history->length() == length + 1);

		// THE PAD: the Trig held at 1 for the hand and back at 0 — a fire for the engine, and no undo step.
		pad.press(true); // Alt: fire by hand, whatever is armed
		CHECK(f.deviceHost.fieldAt(seat.device, ssq::at(ssq::randomiseParam(lane))) == 1.0f);
		pad.release();
		CHECK(f.deviceHost.fieldAt(seat.device, ssq::at(ssq::randomiseParam(lane))) == 0.0f);
		CHECK(history->length() == length + 1);

		// BESIDE ITS BUTTON, inside the body, and the button has room for its word.
		const visage::Bounds p = pad.bounds();
		const visage::Bounds b = card.randomiseButton().bounds();
		CHECK(p.right() <= b.x());
		CHECK(p.bottom() <= ui::kCardHeadHeight + ui::StepSeqCard::bodyHeight());
		CHECK(b.width() >= 60.0f);
	}
}
TEST_CASE("NT: STEPSEQ card — the randomise amount reads Rand Amt, and the output Amount is a box of its own on the outputs' row, inside the body") {
	ModFixture f;
	const StepSeqSeat seat = seatStepSeq(f);
	ui::StepSeqCard& card = *seat.card;
	CHECK(card.randomAmountBox().label() == "Rand Amt");
	CHECK(dynamic_cast<const ui::DeviceControl&>(card.randomAmountBox().control()).control()
	      == ssq::at(ssq::Param::RandomAmount));
	CHECK(card.amountBox().label() == "Amount");
	CHECK(dynamic_cast<const ui::DeviceControl&>(card.amountBox().control()).control() == ssq::at(ssq::Param::Amount));
	CHECK(card.amountBox().isVisible());
	// ON THE OUTPUTS' ROW, between Poly and Bipolar, and inside the body the card asks for.
	const visage::Bounds a = card.amountBox().bounds();
	const visage::Bounds p = card.polyChip().bounds();
	CHECK(a.x() > p.right());
	CHECK(a.y() <= p.y());
	CHECK(a.bottom() >= p.bottom());
	CHECK(a.bottom() <= ui::kCardHeadHeight + ui::StepSeqCard::bodyHeight());
	CHECK(a.width() > 0.0f);
	// A HAND ON IT WRITES THE NEW CONTROL, and leaves the randomise amount alone.
	card.amountBox().control().beginGesture();
	card.amountBox().control().setValue(-0.5);
	card.amountBox().control().endGesture();
	CHECK(f.deviceHost.fieldAt(seat.device, ssq::at(ssq::Param::Amount)) == -0.5f);
	CHECK(f.deviceHost.fieldAt(seat.device, ssq::at(ssq::Param::RandomAmount)) == 1.0f);
}
TEST_CASE("STEPSEQ card: the global Smoothing is the LFO's box (Sm), under the Amount, inside the body, and a hand on it writes the field") {
	ModFixture f;
	const StepSeqSeat seat = seatStepSeq(f);
	ui::StepSeqCard& card = *seat.card;
	CHECK(card.smoothingBox().label() == "Sm");
	CHECK(dynamic_cast<const ui::DeviceControl&>(card.smoothingBox().control()).control()
	      == ssq::at(ssq::Param::Smoothing));
	CHECK(card.smoothingBox().isVisible());
	const visage::Bounds s = card.smoothingBox().bounds();
	const visage::Bounds a = card.amountBox().bounds();
	CHECK(s.x() == a.x());
	CHECK(s.y() >= a.bottom());
	CHECK(s.bottom() <= ui::kCardHeadHeight + ui::StepSeqCard::bodyHeight());
	CHECK(f.deviceHost.fieldAt(seat.device, ssq::at(ssq::Param::Smoothing)) == mod::kSmoothingLog2SecondsMin);
	card.smoothingBox().control().beginGesture();
	card.smoothingBox().control().setValue(-3.0);
	card.smoothingBox().control().endGesture();
	CHECK(f.deviceHost.fieldAt(seat.device, ssq::at(ssq::Param::Smoothing)) == -3.0f);
}

namespace {
//------------------------------------------------------------------------------------------------//
// THE FOLLOWER (backlog MY) — its plot, its threshold line, the line saying what it hears.
//------------------------------------------------------------------------------------------------//

namespace fol = feathers::mod::follower;
struct FollowerSeat {
	size_t slot = 0;
	ui::FollowerCard* card = nullptr;
};
/// A FOLLOWER IN THE STRIP AND THE CARD THE STRIP BUILT — the cast is the claim that the strip builds its own card.
FollowerSeat seatFollower(ModFixture& f) {
	FollowerSeat seat;
	seat.slot = f.bridge.addRackDevice(fol::kTraits.tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::FollowerCard*>(dynamic_cast<const ui::FollowerCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
/// WHAT THE ENGINE PUBLISHES for the follower, through the strip's playhead channel: its level and its pulse count.
void publishFollower(ModFixture& f, const FollowerSeat& seat, float level, uint32_t fires) {
	feathers::mod::Playhead where { 0, 0.0f };
	where.level = level;
	where.haveLevel = true;
	where.trigFires = fires;
	where.haveTrigFires = true;
	f.bridge.publishRackPlayhead(seat.slot, where);
}
} // namespace

TEST_CASE("Follower card: the plot scrolls the level and the pulses the engine published, and the line sits at the resolved threshold") {
	ModFixture f;
	const FollowerSeat seat = seatFollower(f);
	ui::FollowerPlot& plot = seat.card->plot();
	// THE FIRST READING SETS WHERE COUNTING STARTS — a count of 5 on the first frame is not a pulse on that frame.
	publishFollower(f, seat, 0.3f, 5);
	plot.update();
	CHECK(plot.levelAtForTest(0) == 0.3f);
	CHECK_FALSE(plot.firedAtForTest(0));
	publishFollower(f, seat, 0.7f, 6);
	plot.update();
	CHECK(plot.levelAtForTest(0) == 0.7f);
	CHECK(plot.firedAtForTest(0));
	CHECK(plot.levelAtForTest(1) == 0.3f); // scrolled: the older reading one frame back
	publishFollower(f, seat, 0.7f, 6);
	plot.update();
	CHECK_FALSE(plot.firedAtForTest(0)); // one pulse, one mark
	// THE LINE: the default 50%, then where the control is moved.
	CHECK(plot.thresholdForTest() == 0.5f);
	seat.card->control(static_cast<size_t>(fol::Param::Threshold)).setValue(0.8);
	f.bridge.refresh();
	plot.update();
	CHECK(std::abs(plot.thresholdForTest() - 0.8f) <= 1e-6f);
}
TEST_CASE("Follower card: a press on the plot puts Threshold where the pointer is, in the upper half when bipolar") {
	ModFixture f;
	const FollowerSeat seat = seatFollower(f);
	ui::FollowerPlot& plot = seat.card->plot();
	REQUIRE(plot.height() > 20.0f);
	// UNDER SIGNED (the default) the plot is bipolar: zero is its middle, and a threshold v is drawn at (1 + v) / 2.
	REQUIRE(plot.bipolar());
	const auto yOf = [&](float v) { return plot.levelToYForTest(0.5f * (v + 1.0f)); };
	const float y = yOf(0.25f);
	plot.onMouseDown().callback(at(plot.width() * 0.5f, y));
	plot.onMouseUp().callback(at(plot.width() * 0.5f, y));
	f.bridge.refresh();
	CHECK(std::abs(seat.card->control(static_cast<size_t>(fol::Param::Threshold)).value() - 0.25) <= 1e-3);
	// AND A DRAG MOVES IT ON: up to 0.75.
	const float y2 = yOf(0.75f);
	plot.onMouseDown().callback(at(plot.width() * 0.5f, y));
	plot.onMouseDrag().callback(at(plot.width() * 0.5f, y2));
	plot.onMouseUp().callback(at(plot.width() * 0.5f, y2));
	f.bridge.refresh();
	CHECK(std::abs(seat.card->control(static_cast<size_t>(fol::Param::Threshold)).value() - 0.75) <= 1e-3);
}
TEST_CASE("Follower card: one line says what it hears; Control follows Input, Audio greys Input and offers Peak | RMS") {
	ModFixture f;
	const FollowerSeat seat = seatFollower(f);
	ui::FollowerCard& card = *seat.card;
	// THE DEFAULT IS CONTROL AND SIGNED: Input live, the three detectors, a bipolar plot, Floor greyed, Poly greyed.
	CHECK(card.hearsForTest() == "follows its Input: route any source into it");
	CHECK(card.input().isVisible());
	CHECK(card.input().active());
	CHECK(card.input().bounds().right() <= card.plot().bounds().x()); // the plot gives up the Input column
	CHECK(card.detector().isVisible());
	CHECK(card.detector().control() == &card.control(static_cast<size_t>(fol::Param::Detector)));
	CHECK(card.plot().bipolar());
	CHECK_FALSE(card.floor().active());
	CHECK_FALSE(card.poly().usable());
	CHECK(std::string(card.control(static_cast<size_t>(fol::Param::Poly)).unavailable())
	      == fol::kTraits.perVoiceRequiresWhy);
	// AUDIO: Input greyed, Peak (the flip took it from Signed), Floor live, Poly live, and the line says what it hears.
	card.mode().select(static_cast<int>(fol::Mode::Audio));
	f.bridge.refresh();
	card.updateContent();
	CHECK(card.hearsForTest() == "hears the audio here in the chain");
	CHECK(card.input().isVisible());
	CHECK_FALSE(card.input().active());
	CHECK(f.deviceHost.fieldAt(card.device(), static_cast<size_t>(fol::Param::Detector))
	      == static_cast<float>(fol::Detector::Peak));
	CHECK_FALSE(card.plot().bipolar());
	CHECK(card.floor().active());
	CHECK(card.poly().usable());
	CHECK(card.control(static_cast<size_t>(fol::Param::Poly)).unavailable() == nullptr);
	card.control(static_cast<size_t>(fol::Param::Poly)).setValue(1.0);
	f.bridge.refresh();
	card.updateContent();
	CHECK(card.hearsForTest() == "hears its own voice, here in the chain");
	// THE TAP'S CONTROLS ARE ONE ROW NOW: the Sidechain source stays gone, and every control has a widget.
	for (const device::ParamDesc& p : fol::kDescriptor.params) {
		CHECK(std::string(p.key) != "source");
	}
	std::vector<size_t> reached = card.addressedParamsForTest();
	reached.push_back(static_cast<size_t>(fol::kTraits.enableControl));
	std::sort(reached.begin(), reached.end());
	CHECK(reached.size() == fol::kDescriptor.params.size());
}
TEST_CASE("Follower card: a flip to Audio deletes the routes Audio forbids, names them in the tray, and one undo puts all of it back") {
	ModFixture f;
	const FollowerSeat seat = seatFollower(f);
	ui::FollowerCard& card = *seat.card;
	ui::EditHistory* history = f.bridge.editHistory();
	REQUIRE(history != nullptr);
	// THE ENV INTO THE VOICE'S LEVEL — legal in Control (the strip's modulators run before the voice); in Audio the
	// follower's output is computed from the voice's audio, so feeding the voice would read it before it is written.
	const backplane::Source env = backplane::Source::output(card.device(), static_cast<size_t>(fol::Output::Env));
	const backplane::Destination level =
	    backplane::Destination::core(static_cast<size_t>(modDestinationIndexOf(ParamId::VoiceLevel)));
	f.bridge.setRouteAmount(env, level, 0.5f);
	REQUIRE(f.deviceHost.routeAmount(env, level) == 0.5f);
	const size_t start = history->length();

	card.mode().select(static_cast<int>(fol::Mode::Audio));
	f.bridge.refresh();
	CHECK(f.deviceHost.routeAmount(env, level) == 0.0f);
	CHECK(f.deviceHost.fieldAt(card.device(), static_cast<size_t>(fol::Param::Mode)) == 1.0f);
	CHECK(history->length() == start + 1); // ONE step: the mode, the detector it moved, the route it cost
	const std::string said = card.modeControl().lastSaidForTest();
	CAPTURE(said);
	CHECK(said.rfind("Mode Audio", 0) == 0);
	CHECK(said.find("removed Follower Env") != std::string::npos);
	CHECK(said.find(f.deviceHost.destinationName(level)) != std::string::npos);

	history->undo();
	f.bridge.refresh();
	CHECK(f.deviceHost.fieldAt(card.device(), static_cast<size_t>(fol::Param::Mode)) == 0.0f);
	CHECK(f.deviceHost.fieldAt(card.device(), static_cast<size_t>(fol::Param::Detector))
	      == static_cast<float>(fol::Detector::Signed));
	CHECK(f.deviceHost.routeAmount(env, level) == 0.5f);
	// AND REDO TAKES IT AGAIN.
	history->redo();
	f.bridge.refresh();
	CHECK(f.deviceHost.fieldAt(card.device(), static_cast<size_t>(fol::Param::Mode)) == 1.0f);
	CHECK(f.deviceHost.routeAmount(env, level) == 0.0f);
	// A FLIP THAT COSTS NOTHING SAYS ONLY THE MODE.
	card.mode().select(static_cast<int>(fol::Mode::Control));
	CHECK(card.modeControl().lastSaidForTest() == "Mode Control");
}
TEST_CASE("Follower card: photographs of the strip (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — the venue for LOOKING (the Delay card's). Writes PNG frames of the whole editor with the
	// Follower in the strip in Control (Signed, a bipolar trace) and in Audio under Poly, and prints where the card
	// sits. Skipped without the variable.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	ModFixture f;
	FollowerSeat seat = seatFollower(f);
	auto* editor = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(editor != nullptr);
	editor->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	const auto shoot = [&](const char* name) {
		f.bridge.refresh();
		(void)seat.card->updateContent();
		f.root->redrawAll();
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
		const visage::Point where = seat.card->positionInWindow();
		MESSAGE(name << " " << shot.width() << "x" << shot.height() << " card " << where.x << "," << where.y << " "
		             << seat.card->width() << "x" << seat.card->height() << " -> " << path);
	};
	// CONTROL, SIGNED: an LFO-like swing through zero, drawn about the middle (0.5 is zero), with a pulse each crest.
	uint32_t fires = 0;
	for (int frame = 0; frame < 200; ++frame) {
		const float v = 0.8f * std::sin(6.2831853f * static_cast<float>(frame) / 70.0f);
		if (frame % 70 == 12) {
			++fires;
		}
		publishFollower(f, seat, 0.5f * (v + 1.0f), fires);
		seat.card->plot().update();
	}
	shoot("follower-control");
	// AUDIO, POLY: swells that cross the threshold.
	seat.card->mode().select(static_cast<int>(fol::Mode::Audio));
	seat.card->control(static_cast<size_t>(fol::Param::Poly)).setValue(1.0);
	float level = 0.0f;
	for (int frame = 0; frame < 200; ++frame) {
		const int phase = frame % 45;
		level = phase < 4 ? 0.95f : level * 0.93f;
		if (phase == 0) {
			++fires;
		}
		publishFollower(f, seat, level, fires);
		seat.card->plot().update();
	}
	shoot("follower-audio-poly");
}
TEST_CASE("NO: the Rack's card has a strip per chain that follows the chains by id, and its window has a rail per chain that + Chain adds to") {
	namespace pr = feathers::rack::parallel;
	// THE RUNNING INSTANCE, built where a rack engine builds it, and filed as the host files it — the tooltips guard's road.
	feathers::rack::RackEngine bodies { feathers::rack::kRackAxes, feathers::hostCatalogue() };
	bodies.prepare(48000.0, 4096);
	const size_t at = bodies.addDevice(pr::kTraits.tag);
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot);
	void* body = bodies.slotAtLane(at).storage.get();
	REQUIRE(body != nullptr);
	pr::Rack& rack = *pr::rackOf(body);

	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(pr::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	f.bridge.fxBodies().attach(id, body);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 1);
	CHECK(card->stripsForTest()[0]->summaryForTest() == "Chain 1 · empty"); // the chain's one label, never its number
	CHECK(card->headCaption() == "1 chain");

	// A SECOND CHAIN, AND A FILTER IN THE FIRST: two strips, each saying what its chain holds.
	REQUIRE(rack.addChain() == 1);
	feathers::rack::RackBridge* first = rack.chainAt(0)->bridge();
	REQUIRE(first->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 2);
	CHECK(card->stripsForTest()[0]->summaryForTest() == std::string(feathers::fx::filter::kTraits.name)); // the label IS what it holds
	CHECK(card->stripsForTest()[1]->summaryForTest() == "Chain 2 · empty");
	CHECK(card->headCaption() == "2 chains");

	// THE STRIP'S CONTROLS ARE THAT CHAIN'S, by value: mute the second through its chip's own control.
	std::vector<ui::ControlFace::Offered> offered;
	card->stripsForTest()[1]->muteChip().offeredControls(offered);
	REQUIRE(offered.size() == 1);
	const_cast<feathers::device::Modulatable*>(offered[0].control)->setValue(1.0);
	CHECK(rack.muted(1));
	CHECK_FALSE(rack.muted(0));

	// AND IT FOLLOWS ITS CHAIN BY ID: remove the first, and the muted chain — now first — is still the one muted.
	REQUIRE(rack.removeChain(0));
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 1);
	CHECK(rack.muted(0));
	CHECK(card->stripsForTest()[0]->muteChip().on());

	// THE WINDOW: a rail per chain, and "+ Chain" adds one with its rail.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	CHECK(panel->railCountForTest() == 1);
	CHECK(panel->railForTest(0).labelForTest() == "Chain 1");
	panel->addChainForTest();
	CHECK(rack.chainCount() == 2);
	CHECK(panel->railCountForTest() == 2);
	CHECK(panel->railForTest(1).labelForTest() == "Chain 2");
	CHECK(panel->railForTest(1).liveForTest()); // its column is over that chain
	view.reset();
	f.bridge.fxBodies().detach(id, body);
	REQUIRE(f.bridge.removeFxDevice(slot));
}
TEST_CASE("NO: a chain's level, mute and solo are each ONE undo step from the card's strip, and the window's add and remove undo with the chain whole") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	REQUIRE(card != nullptr);
	ui::EditHistory history;
	card->setModulation(nullptr, &history);
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 1);

	// A MUTE, AS THE CHIP MAKES IT: begin, set, end — one step.
	std::vector<ui::ControlFace::Offered> offered;
	card->stripsForTest()[0]->muteChip().offeredControls(offered);
	REQUIRE(offered.size() == 1);
	auto* mute = const_cast<feathers::device::Modulatable*>(offered[0].control);
	mute->beginGesture();
	mute->setValue(1.0);
	mute->endGesture();
	CHECK(r.rack().muted(0));
	CHECK(history.length() == 1);
	history.undo();
	CHECK_FALSE(r.rack().muted(0));
	history.redo();
	CHECK(r.rack().muted(0));
	// A LEVEL DRAG OF MANY WRITES IS STILL ONE STEP.
	offered.clear();
	card->stripsForTest()[0]->levelPill().offeredControls(offered);
	REQUIRE(!offered.empty());
	auto* level = const_cast<feathers::device::Modulatable*>(offered[0].control);
	level->beginGesture();
	for (const double v : { 0.45, 0.4, 0.3, 0.25 }) {
		level->setValue(v);
	}
	level->endGesture();
	CHECK(history.length() == 2);
	CHECK(r.rack().level(0) == 0.25f);
	history.undo();
	CHECK(r.rack().level(0) == static_cast<float>(feathers::composite::kFaderUnity));
	history.redo();

	// THE WINDOW: "+ Chain" is one step; a removal of a chain with a device in it undoes with its device.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->modulationBound(nullptr, &history);
	const size_t steps = history.length();
	panel->addChainForTest();
	REQUIRE(r.rack().chainCount() == 2);
	CHECK(history.length() == steps + 1);
	const uint16_t added = r.rack().chainIdAt(1);
	history.undo();
	CHECK(r.rack().chainCount() == 1);
	history.redo();
	REQUIRE(r.rack().chainCount() == 2);
	CHECK(r.rack().chainIdAt(1) == added); // the same chain, under the same id

	REQUIRE(r.rack().chainAt(1)->bridge()->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
	r.rack().setLevel(1, 0.75f);
	panel->removeChainForTest(added);
	CHECK(r.rack().chainCount() == 1);
	history.undo();
	REQUIRE(r.rack().chainCount() == 2);
	CHECK(r.rack().chainIdAt(1) == added);
	CHECK(r.rack().chainAt(1)->bridge()->chainLength() == 1);
	CHECK(r.rack().level(1) == 0.75f);
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("NO: inside a Rack a filter's Poly switch is greyed, refuses the hand and says why; the same switch in Feathers' lane does not") {
	namespace pr = feathers::rack::parallel;
	namespace flt = feathers::fx::filter;
	ModFixture f;
	RackOnLane r(f);
	rack::RackBridge* chain = r.rack().chainAt(0)->bridge();
	REQUIRE(chain != nullptr);
	const feathers::DeviceInstanceId inRack = chain->addDevice(flt::kTraits.tag);
	REQUIRE(inRack != feathers::kNoInstance);
	const size_t poly = static_cast<size_t>(flt::kDescriptor.traits.perVoiceControl);

	// THE CONTROL, as every card's widget holds it.
	ui::DeviceControl rackPoly(*chain, inRack, flt::kDescriptor, poly);
	CHECK(std::string(rackPoly.unavailable() != nullptr ? rackPoly.unavailable() : "") == pr::kPolyInsideRack); // by content: a pointer compare fails under ASan
	CHECK(std::string(ui::tooltipOf(&rackPoly)) == pr::kPolyInsideRack);
	ui::DeviceControl rackCutoff(*chain, inRack, flt::kDescriptor, 1);
	CHECK(rackCutoff.unavailable() == nullptr); // only the Poly switch

	// THE FILTER'S CARD IN THE RACK'S WINDOW: its Poly segments are not usable, and a press changes nothing.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	(void)panel->update();
	ui::ChainColumn* column = panel->railForTest(0).columnForTest();
	REQUIRE(column != nullptr);
	ui::FilterCard* card = nullptr;
	for (ui::FxCard* c : column->cardsForTest()) {
		card = card != nullptr ? card : dynamic_cast<ui::FilterCard*>(c);
	}
	REQUIRE(card != nullptr);
	REQUIRE(card->polyForTest() != nullptr);
	CHECK_FALSE(card->polyForTest()->usable());
	const float before = chain->fieldAt(inRack, poly);
	// A REAL PRESS, through the chip's own handler (`toggleForTest` bypasses the gate a press goes through).
	ui::SwitchChip* chip = card->polyForTest();
	chip->onMouseDown().callback(at(chip->width() * 0.5f, chip->height() * 0.5f));
	chip->onMouseUp().callback(at(chip->width() * 0.5f, chip->height() * 0.5f));
	CHECK(chain->fieldAt(inRack, poly) == before);

	// AND IN FEATHERS' OWN LANE THE SAME SWITCH IS LIVE.
	const size_t slot = f.bridge.addFxDevice(flt::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	ui::DeviceControl lanePoly(f.deviceHost, f.bridge.fxDeviceAt(slot), flt::kDescriptor, poly);
	CHECK(lanePoly.unavailable() == nullptr);
	view.reset();
	REQUIRE(f.bridge.removeFxDevice(slot));
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}

namespace {
/// A FRAME THE EDITOR WOULD DRAW: it and every ancestor up to `root` is visible.
bool drawnUnder(const visage::Frame* frame, const visage::Frame* root) {
	for (const visage::Frame* f = frame; f != nullptr; f = f->parent()) {
		if (!f->isVisible()) {
			return false;
		}
		if (f == root) {
			return true;
		}
	}
	return false;
}
/// RUN `frames` OF CONSTANT `amplitude` THROUGH A RACK'S BODY, as the engine does, so its chains publish their peaks.
void feedRack(void* body, float amplitude, int blocks = 2) {
	namespace pr = feathers::rack::parallel;
	std::vector<float> l(256, amplitude);
	std::vector<float> r(256, amplitude);
	for (int b = 0; b < blocks; ++b) {
		std::fill(l.begin(), l.end(), amplitude);
		std::fill(r.begin(), r.end(), amplitude);
		pr::kAudioOps.processStereo(body, l.data(), r.data(), 256);
	}
}
} // namespace

TEST_CASE("OC: the Rack window seats Mix and Gain on top, an H-line, then the eight macros TWO ACROSS AND FOUR TALL in a narrow block at the left, and a small + Chain below them") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	// THE DEFAULT HEIGHT IS THE MINIMUM (the overlay opens a window at it): the block must fit in it, nothing clipped.
	const float minimumHeight = panel->minimumSize().y;
	panel->setBounds(0.0f, 0.0f, 700.0f, minimumHeight);

	const auto order = panel->addressedParamsForTest();
	REQUIRE(order.size() == 10);
	REQUIRE(order[0] == pr::Param::Mix);
	REQUIRE(order[1] == pr::Param::Gain);
	REQUIRE(order[2] == pr::Param::Macro1);
	REQUIRE(order[9] == pr::Param::Macro8);
	const visage::Bounds mix = panel->knobBoundsForTest(0);
	const visage::Bounds gain = panel->knobBoundsForTest(1);
	const visage::Bounds line = panel->dividerBoundsForTest();
	const visage::Bounds add = panel->addBoundsForTest();

	// MIX AND GAIN: one row, side by side, ABOVE the line.
	CHECK(mix.y() == gain.y());
	CHECK(mix.right() <= gain.x());
	CHECK(gain.bottom() <= line.y());
	// THE LINE spans the column and is thin.
	CHECK(line.x() == ui::ParallelRackExpanded::kPad);
	CHECK(line.width() == ui::ParallelRackExpanded::kColumnWidth);
	CHECK(line.height() <= 1.0f);
	// THE MACROS STAND IN THEIR OWN PANE after the rack's: two columns, four rows; row r holds macros 2r and 2r+1, left to right.
	const float macrosLeft = panel->containerWidth();
	for (size_t row = 0; row < 4; ++row) {
		const visage::Bounds left = panel->knobBoundsForTest(2 + 2 * row);
		const visage::Bounds right = panel->knobBoundsForTest(3 + 2 * row);
		CHECK(left.y() == right.y());
		CHECK(left.right() <= right.x());
		CHECK(left.x() == macrosLeft + mix.x()); // the first column is Mix's, the second Gain's, shifted into the macros' pane
		CHECK(right.x() == macrosLeft + gain.x());
		CHECK(left.y() >= ui::ParallelRackExpanded::kPad); // the pane's rows start under its toggle's head row, from its own top
		if (row > 0) {
			CHECK(left.y() >= panel->knobBoundsForTest(2 * row).bottom()); // below the row before
		}
	}
	// THE LAST ROW ENDS INSIDE THE PANEL, and "+ Chain" is small and below it, inside the panel too: nothing is clipped.
	const visage::Bounds last = panel->knobBoundsForTest(9);
	CHECK(last.bottom() <= minimumHeight);
	// STRIPS: "+ Chain" stands after the last chain's rail, level with the strips' heads (not below the macros any more).
	CHECK(add.x() >= panel->railForTest(panel->railCountForTest() - 1).bounds().right());
	CHECK(add.bottom() <= minimumHeight);
	CHECK(add.width() < ui::ParallelRackExpanded::kColumnWidth);
	CHECK(ui::ParallelRackExpanded::kColumnWidth < 240.0f);
	// AND THE RAILS START RIGHT AFTER THE COLUMN: nothing of the old wide column is left.
	CHECK(panel->railForTest(0).bounds().x() == 2.0f * (ui::ParallelRackExpanded::kColumnWidth + 2.0f * ui::ParallelRackExpanded::kPad)); // after both panes
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("OC: a chain's header works while the chain is collapsed — level, mute, solo and a delete by value, and the delete undoes whole") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	REQUIRE(r.rack().addChain() == 1);
	// THIS CASE IS ABOUT A COLLAPSED CHAIN, and a new chain starts open: both are folded here, as a hand would.
	r.rack().setOpen(0, false);
	r.rack().setOpen(1, false);
	REQUIRE(r.rack().chainAt(1)->bridge()->addDevice(feathers::fx::filter::kTraits.tag) != feathers::kNoInstance);
	r.rack().setLevel(1, 0.75f);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	ui::EditHistory history;
	panel->modulationBound(nullptr, &history);
	panel->setBounds(0.0f, 0.0f, 700.0f, panel->minimumSize().y + 40.0f);
	(void)panel->update();
	REQUIRE(panel->railCountForTest() == 2);

	for (size_t i = 0; i < 2; ++i) {
		// COLLAPSED (a rail starts closed), and its strip is still drawn, laid out as a column, and holds every part.
		ui::FxLaneRail& rail = panel->railForTest(i);
		REQUIRE(rail.collapsed());
		ui::ChainStrip& strip = panel->stripForTest(i);
		CHECK(drawnUnder(&strip, panel));
		CHECK(strip.columnLayoutForTest());
		CHECK(strip.width() < ui::ChainStrip::kColumnBelow);
		for (const visage::Frame* part : { static_cast<const visage::Frame*>(&strip.muteChip()),
		                                   static_cast<const visage::Frame*>(&strip.soloChip()),
		                                   static_cast<const visage::Frame*>(&strip.levelFrame()) }) {
			CHECK(drawnUnder(part, panel));
			CHECK(part->width() > 0.0f);
			CHECK(part->height() > 0.0f);
			CHECK(part->x() >= 0.0f);
			CHECK(part->x() + part->width() <= strip.width());
			CHECK(part->y() + part->height() <= strip.height());
		}
		const visage::Bounds cross = strip.removeBoundsForTest();
		CHECK(cross.width() > 0.0f);
		CHECK(cross.y() + cross.height() <= strip.height());
		CHECK(cross.x() + cross.width() <= strip.width());
	}

	// LEVEL, MUTE AND SOLO, through the strip's own widgets, land on THAT chain by value.
	const auto controlOf = [](auto& widget) {
		std::vector<ui::ControlFace::Offered> offered;
		widget.offeredControls(offered);
		REQUIRE(!offered.empty());
		return const_cast<feathers::device::Modulatable*>(offered[0].control);
	};
	controlOf(panel->stripForTest(1).muteChip())->setValue(1.0);
	CHECK(r.rack().muted(1));
	CHECK_FALSE(r.rack().muted(0));
	controlOf(panel->stripForTest(0).soloChip())->setValue(1.0);
	CHECK(r.rack().soloed(0));
	CHECK_FALSE(r.rack().soloed(1));
	auto* level = controlOf(panel->stripForTest(0).levelFace());
	level->beginGesture();
	level->setValue(0.3);
	level->endGesture();
	CHECK(r.rack().level(0) == 0.3f);
	CHECK(r.rack().level(1) == 0.75f);
	CHECK(history.length() == 3); // each one undo step, while collapsed

	// THE LEVEL FADER IS VERTICAL AND SIDE BY SIDE WITH THE 1 PX METER, in the column (the pill is the row's).
	{
		ui::ChainStrip& strip = panel->stripForTest(0);
		const visage::Bounds fader = strip.levelFader().bounds();
		const visage::Bounds meter = strip.meterBoundsForTest();
		CHECK_FALSE(strip.levelPill().isVisible());
		CHECK(strip.levelFader().isVisible());
		CHECK(fader.height() > fader.width());
		CHECK(fader.y() == meter.y());
		CHECK(fader.height() == meter.height());
		CHECK(fader.right() <= meter.x());
		CHECK(meter.width() == 1.0f);
		// A VERTICAL DRAG SETS THE LEVEL BY VALUE, one undo step, UP RAISES it; a double-click is unity.
		const size_t before = history.length();
		visage::MouseEvent press;
		press.position = { 3.0f, 60.0f };
		strip.levelFader().onMouseDown().callback(press);
		visage::MouseEvent drag;
		drag.position = { 3.0f, 60.0f - 0.2f * ui::ChainFader::kSpan }; // 24 px up: +0.2 of the range
		strip.levelFader().onMouseDrag().callback(drag);
		strip.levelFader().onMouseUp().callback(drag);
		CHECK(r.rack().level(0) == doctest::Approx(0.3f + 0.2f).epsilon(1e-4));
		CHECK(history.length() == before + 1);
		visage::MouseEvent twice;
		twice.position = { 3.0f, 60.0f };
		twice.repeat_click_count = 1;
		strip.levelFader().onMouseDown().callback(twice);
		CHECK(r.rack().level(0) == static_cast<float>(feathers::composite::kFaderUnity));
		r.rack().setLevel(0, 0.3f); // back to the value the rest of the case expects
		history.undo();
		history.undo();
		controlOf(strip.levelFace())->setValue(0.3);
	}
	// THE DELETE REMOVES THAT CHAIN (the second) and its undo puts it back whole: id, device, level, mute.
	const uint16_t secondId = r.rack().chainIdAt(1);
	panel->stripForTest(1).pressRemoveForTest();
	CHECK(r.rack().chainCount() == 1);
	(void)panel->update();
	CHECK(panel->railCountForTest() == 1);
	history.undo();
	REQUIRE(r.rack().chainCount() == 2);
	CHECK(r.rack().chainIdAt(1) == secondId);
	CHECK(r.rack().chainAt(1)->bridge()->chainLength() == 1);
	CHECK(r.rack().level(1) == 0.75f);
	CHECK(r.rack().muted(1));
	(void)panel->update();
	CHECK(panel->railCountForTest() == 2);

	// OPEN, the same header is a ROW with the same parts.
	panel->railForTest(0).setCollapsed(false);
	panel->setBounds(0.0f, 0.0f, 900.0f, panel->minimumSize().y + 40.0f);
	ui::ChainStrip& open = panel->stripForTest(0);
	CHECK_FALSE(open.columnLayoutForTest());
	CHECK(drawnUnder(&open, panel));
	CHECK(open.height() == ui::ChainStrip::kHeight);
	// "CHAIN N" IS A SHORT LABEL NOW, and the long sentence is the level's tooltip instead of a line of text.
	CHECK(panel->railForTest(0).labelForTest() == "Chain 1");
	CHECK(panel->railForTest(0).captionForTest()[0] == '\0');
	CHECK(std::string(open.levelPill().tooltipAt(visage::Point { 2.0f, 2.0f })).find("parallel") != std::string::npos);
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("OU: the Rack's window offers Racks presets in its title row, and its rails offer none of their own") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	ui::EditHistory history;
	panel->modulationBound(nullptr, &history);
	panel->setBounds(0.0f, 0.0f, 900.0f, panel->minimumSize().y + 40.0f);
	(void)panel->update();
	REQUIRE(panel->railCountForTest() == 1);
	// THE RACK'S OWN, in the title row: the Racks bank, and only that kind.
	CHECK(panel->titleAccessory() == &panel->presetBrowserForTest());
	CHECK(panel->presetBrowserForTest().bank() == feathers::kRackPresetBank);
	CHECK(panel->presetBrowserForTest().count() >= 1); // Init, at least
	for (size_t i = 0; i < panel->presetBrowserForTest().count(); ++i) {
		CHECK(feathers::isPresetOf(feathers::rackPresetDescriptor(), panel->presetBrowserForTest().entry(i).data.bytes));
	}
	// A RAIL HAS NO BROWSER (to browse inside a rail, add a chain and use ITS browser): nothing under the window is a preset
	// browser but the title row's.
	panel->railForTest(0).setCollapsed(false);
	panel->setBounds(0.0f, 0.0f, 900.0f, panel->minimumSize().y + 40.0f);
	(void)panel->update();
	CHECK(findInTree<ui::PresetBrowser>(&panel->railForTest(0)) == nullptr);
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("OC: each chain's header meter reads THAT chain's published peak, vertically, and the window offers every control the card does") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	REQUIRE(r.rack().addChain() == 1);
	r.rack().setLevel(1, 0.25f); // the Chain's fader curve: a gain of 0.25
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 700.0f, panel->minimumSize().y + 40.0f);
	(void)panel->update();
	REQUIRE(panel->railCountForTest() == 2);

	// BEFORE ANYTHING HAS RUN there is no reading to draw.
	CHECK(panel->stripForTest(0).meterReadingForTest() == ui::CardMeterReading {});
	CHECK(panel->stripForTest(0).meterMarksForTest().level.height() == 0.0f);

	feedRack(r.body, 0.5f);
	(void)panel->update();
	// THE READINGS, restated independently: 20 log10(peak), 48 dB of floor. Chain 0 hears 0.5, chain 1 hears 0.5 x 0.25.
	const auto levelFor = [](float peak) { return (20.0f * std::log10(peak) + 48.0f) / 48.0f; };
	const ui::CardMeterReading first = panel->stripForTest(0).meterReadingForTest();
	const ui::CardMeterReading second = panel->stripForTest(1).meterReadingForTest();
	CHECK(first.level == doctest::Approx(levelFor(0.5f)).epsilon(1e-3));
	CHECK(second.level == doctest::Approx(levelFor(0.125f)).epsilon(1e-3));
	CHECK(first.level > second.level);
	// VERTICAL: a track taller than wide, the level standing on its foot, in whole pixels.
	for (size_t i = 0; i < 2; ++i) {
		ui::ChainStrip& strip = panel->stripForTest(i);
		const visage::Bounds track = strip.meterBoundsForTest();
		REQUIRE(track.height() > track.width());
		CHECK(strip.meterShownForTest());
		const ui::CardMeterMarks& marks = strip.meterMarksForTest();
		const float want = std::round(strip.meterReadingForTest().level * track.height());
		CHECK(marks.level.height() == want);
		CHECK(marks.level.bottom() == track.height());
		CHECK(marks.level.width() == ui::ChainStrip::kMeterWidth);
	}

	// WINDOW ⊇ CARD for the chain controls: the card's strip offers level, mute and solo per chain; the window's header
	// offers the same keys for the same chains, drawn under the window while the rails are closed AND once open.
	auto* card = [&]() -> ui::ParallelRackCard* {
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		lane->resized();
		return dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	}();
	REQUIRE(card != nullptr);
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 2);
	// THE CARD'S STRIPS METER THEIR CHAINS TOO, each its own: the same readings the window's headers have.
	for (size_t i = 0; i < 2; ++i) {
		CHECK(card->stripsForTest()[i]->meterShownForTest());
		CHECK(card->stripsForTest()[i]->meterBoundsForTest().height() > card->stripsForTest()[i]->meterBoundsForTest().width());
		CHECK(card->stripsForTest()[i]->meterReadingForTest() == panel->stripForTest(i).meterReadingForTest());
	}
	CHECK(card->stripsForTest()[0]->meterReadingForTest().level > card->stripsForTest()[1]->meterReadingForTest().level);
	const auto keysOf = [](ui::ChainStrip& strip) {
		std::vector<std::string> keys;
		for (visage::Frame* part : { static_cast<visage::Frame*>(&strip.levelFrame()), static_cast<visage::Frame*>(&strip.muteChip()),
		                             static_cast<visage::Frame*>(&strip.soloChip()) }) {
			std::vector<ui::ControlFace::Offered> offered;
			dynamic_cast<ui::ControlFace*>(part)->offeredControls(offered);
			for (const auto& o : offered) {
				keys.emplace_back(o.control->desc().key);
			}
		}
		return keys;
	};
	for (const bool open : { false, true }) {
		for (size_t i = 0; i < 2; ++i) {
			panel->railForTest(i).setCollapsed(!open);
		}
		panel->setBounds(0.0f, 0.0f, 900.0f, panel->minimumSize().y + 40.0f);
		for (size_t i = 0; i < 2; ++i) {
			CHECK(keysOf(panel->stripForTest(i)) == keysOf(*card->stripsForTest()[i]));
			CHECK(drawnUnder(&panel->stripForTest(i), panel));
		}
	}
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("OC: a chain's fold survives closing and reopening the Rack window and a save and load — chain 2 folded, chain 1 open — and is no undo step") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	REQUIRE(r.rack().addChain() == 1);
	ui::WidgetRegistry registry;
	ui::EditHistory history;
	std::vector<uint8_t> body;
	{
		auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
		auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->modulationBound(nullptr, &history);
		panel->setBounds(0.0f, 0.0f, 900.0f, panel->minimumSize().y + 40.0f);
		(void)panel->update();
		REQUIRE(panel->railCountForTest() == 2);
		// A FRESH RACK'S CHAINS ARE OPEN (a new chain starts open, and the rack says so); a hand folds both.
		CHECK_FALSE(panel->railForTest(0).collapsed());
		CHECK_FALSE(panel->railForTest(1).collapsed());
		CHECK(r.rack().open(0));
		CHECK(r.rack().open(1));
		REQUIRE(panel->railForTest(0).setCollapsed(true));
		REQUIRE(panel->railForTest(1).setCollapsed(true));
		CHECK_FALSE(r.rack().open(0));
		// A HAND OPENS CHAIN 1 (the rail's own toggle) — the rack hears it, by value.
		REQUIRE(panel->railForTest(0).setCollapsed(false));
		CHECK(r.rack().open(0));
		CHECK_FALSE(r.rack().open(1));
		// OPEN THE SECOND TOO, THEN FOLD IT AGAIN: "fold chain 2".
		REQUIRE(panel->railForTest(1).setCollapsed(false));
		CHECK(r.rack().open(1));
		REQUIRE(panel->railForTest(1).setCollapsed(true));
		CHECK_FALSE(r.rack().open(1));
		CHECK(r.rack().open(0));
		CHECK(history.length() == 0); // view state: no undo step
		view.reset(); // CLOSE THE EDITOR
	}
	{
		// REOPENED: the rails are built as the rack remembers them.
		auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
		auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		(void)panel->update();
		REQUIRE(panel->railCountForTest() == 2);
		CHECK_FALSE(panel->railForTest(0).collapsed()); // chain 1 open
		CHECK(panel->railForTest(1).collapsed());       // chain 2 folded
		// A LOAD THAT CHANGES IT IS FOLLOWED BY AN OPEN WINDOW, and the window does not write the half-applied picture back.
		r.rack().setOpen(0, false);
		r.rack().setOpen(1, true);
		(void)panel->update();
		CHECK(panel->railForTest(0).collapsed());
		CHECK_FALSE(panel->railForTest(1).collapsed());
		CHECK_FALSE(r.rack().open(0));
		CHECK(r.rack().open(1));
		r.rack().setOpen(0, true);
		r.rack().setOpen(1, false);
		body.clear();
		const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
			                                       auto* out = static_cast<std::vector<uint8_t>*>(ctx);
			                                       out->insert(out->end(), data, data + bytes);
		                                       } };
		REQUIRE(r.rack().saveState(sink));
		view.reset();
	}
	// RELOAD INTO THE SAME RACK (a load replaces the chains by id): the state comes back with them.
	r.rack().setOpen(0, false);
	REQUIRE(r.rack().loadState(body.data(), body.size()));
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	(void)panel->update();
	REQUIRE(panel->railCountForTest() == 2);
	CHECK_FALSE(panel->railForTest(0).collapsed());
	CHECK(panel->railForTest(1).collapsed());
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("F2: Spectral's card and window share the filter tab — the card's Slope is the selected filter's, and the window shows that filter's panel") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::SpectralCard* card = d.card;
	d.set(static_cast<size_t>(spx::Param::FilterOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::Filter2On), 1.0f);
	(void)card->updateContent();
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	(void)window->updateContent();
	using C = feathers::SpectralControl;
	// FILTER 1'S TAB: the card's Slope is filter 1's, and the window's filter panel is filter 1's.
	CHECK(card->widgetForTest(spx::Param::FilterSlope)->isVisible());
	CHECK_FALSE(card->widgetForTest(spx::Param::Filter2Slope)->isVisible());
	CHECK(window->frameForTest(0, C::FilterOn)->isVisible());
	CHECK_FALSE(window->frameForTest(0, C::Filter2On)->isVisible());
	// THE WINDOW'S TAB MOVES THE CARD: one tab per device, never saved, shared by its two views.
	window->filterTabsForTest(0, 0).select(1);
	(void)window->updateContent();
	(void)card->updateContent();
	CHECK(window->frameForTest(0, C::Filter2On)->isVisible());
	CHECK_FALSE(window->frameForTest(0, C::FilterOn)->isVisible());
	CHECK(card->widgetForTest(spx::Param::Filter2Slope)->isVisible());
	CHECK_FALSE(card->widgetForTest(spx::Param::FilterSlope)->isVisible());
	// AND NOTHING THE DEVICE STORES MOVED: the tab is view state.
	CHECK(d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(spx::Param::FilterRouting)) == 0.0f);
	// THE CARD'S PLOT CARRIES BOTH HANDLES, filter 2's in front now.
	CHECK(card->plotForTest().handleForTest(ui::SpectrumPlot::Handle::Filter).has_value());
	CHECK(card->plotForTest().handleForTest(ui::SpectrumPlot::Handle::Filter2).has_value());
	window->filterTabsForTest(0, 1).select(0);
	(void)window->updateContent();
	(void)card->updateContent();
	CHECK(card->widgetForTest(spx::Param::FilterSlope)->isVisible());
}
TEST_CASE("SHOT F2: Spectral's window and card with both filters on, each tab in turn (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA. Writes spectral_window_f2_tab1.png / _tab2.png (the window at 960x600 on a windowless editor)
	// and spectral_card_f2_tab2.raw (the whole editor with the card; its position is printed). Filter 1 a Low Pass at 6 kHz, filter 2
	// a Bell at 800 Hz, +8 dB, in Parallel. Run under `scripts/venue-xvfb.sh` with FEATHERS_REDUCED_MOTION=1.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	d.set(static_cast<size_t>(spx::Param::FilterOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::FilterCutoff), static_cast<float>(std::log2(6000.0)));
	d.set(static_cast<size_t>(spx::Param::Filter2On), 1.0f);
	d.set(static_cast<size_t>(spx::Param::Filter2Mode), 4.0f);
	d.set(static_cast<size_t>(spx::Param::Filter2Cutoff), static_cast<float>(std::log2(800.0)));
	d.set(static_cast<size_t>(spx::Param::Filter2Gain), 8.0f);
	d.set(static_cast<size_t>(spx::Param::FilterRouting), 1.0f);
	constexpr int w = 960;
	constexpr int h = 600;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	editor.addChild(window);
	window->setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	const auto shoot = [&](const char* name) {
		for (int i = 0; i < 20; ++i) {
			(void)window->updateContent();
			editor.redrawAll();
			editor.drawWindow();
		}
		const visage::Screenshot& shot = editor.takeScreenshot();
		REQUIRE(shot.width() == w);
		const std::string path = std::string(dir) + "/" + name + ".png";
		shot.save(path);
		MESSAGE("wrote " << path);
	};
	shoot("spectral_window_f2_tab1");
	window->filterTabsForTest(0, 0).select(1);
	shoot("spectral_window_f2_tab2");
	editor.removeChild(window);
	// THE CARD, under tab 2 (the window's choice, shared): the whole editor, raw, with where the card sits.
	auto* root = dynamic_cast<visage::ApplicationEditor*>(d.f.root);
	REQUIRE(root != nullptr);
	root->setWindowless(static_cast<int>(d.f.root->width()), static_cast<int>(d.f.root->height()));
	d.lane->scrollCardIntoView(d.slot);
	window->filterTabsForTest(0, 1).select(1);
	for (int i = 0; i < 3; ++i) {
		(void)d.card->updateContent();
		d.f.root->redrawAll();
		root->drawWindow();
	}
	const visage::Screenshot& card = root->takeScreenshot();
	if (card.width() > 0) {
		const std::string path = std::string(dir) + "/spectral_card_f2_tab2.png";
		card.save(path);
		const visage::Point at = d.card->positionInWindow();
		MESSAGE("card " << at.x << "," << at.y << " " << d.card->width() << "x" << d.card->height() << " -> " << path);
	}
}
TEST_CASE("OH: Spectral's plot greys the partials Partials culls — the kept ones at their gain, the rest turned down by Depth — and its window's Partials is a mode cell") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::SpectralCard* card = d.card;
	const auto frame = [&] {
		(void)card->updateContent();
		return card->plotForTest().frameForTest();
	};
	const feathers::SpectralPlotFrame base = frame(); // Partials off: every partial at its shaped level
	REQUIRE(base.count > 10);
	d.set(static_cast<size_t>(spx::Param::PartialsOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::PartialsMode), 1.0f); // Harmonics
	d.set(static_cast<size_t>(spx::Param::PartialsCount), 3.5f);
	d.set(static_cast<size_t>(spx::Param::PartialsDepth), 1.0f);
	const feathers::SpectralPlotFrame harmonics = frame();
	for (int i = 0; i < base.count; ++i) {
		CAPTURE(i);
		const auto u = static_cast<size_t>(i);
		const float want = i < 3 ? base.left[u] : i == 3 ? 0.5f * base.left[u] : 0.0f; // the fourth at the count's fraction
		CHECK(harmonics.left[u] == doctest::Approx(want).epsilon(1e-6));
		CHECK(harmonics.terrain[u] == base.terrain[u]); // the terrain bar stays: the culled partial reads as greyed
	}
	// DEPTH HALF: a culled partial at half its level, a kept one whole.
	d.set(static_cast<size_t>(spx::Param::PartialsDepth), 0.5f);
	const feathers::SpectralPlotFrame half = frame();
	CHECK(half.left[0] == doctest::Approx(base.left[0]).epsilon(1e-6));
	CHECK(half.left[9] == doctest::Approx(0.5f * base.left[9]).epsilon(1e-6));
	// PEAKS: the reference terrain falls as 1/k, so the loudest N are the lowest N here — the same partials kept, by the other road.
	d.set(static_cast<size_t>(spx::Param::PartialsDepth), 1.0f);
	d.set(static_cast<size_t>(spx::Param::PartialsMode), 0.0f);
	d.set(static_cast<size_t>(spx::Param::PartialsCount), 2.0f);
	const feathers::SpectralPlotFrame peaks = frame();
	CHECK(peaks.left[1] == doctest::Approx(base.left[1]).epsilon(1e-6));
	CHECK(peaks.left[2] == 0.0f);
	// THE WINDOW: Partials' scheme is a mode cell whose box is the count; Depth stays its own knob.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	(void)window->updateContent();
	auto* cell = dynamic_cast<ui::ModeValueCell*>(window->boxWidgetForTest(spx::Param::PartialsMode));
	REQUIRE(cell != nullptr);
	CHECK(cell->activeParam() == static_cast<long>(spx::Param::PartialsCount));
	CHECK(window->boxWidgetForTest(spx::Param::PartialsCount) == cell->boxFor(static_cast<size_t>(spx::Param::PartialsCount)));
	CHECK(dynamic_cast<ui::Knob*>(window->boxWidgetForTest(spx::Param::PartialsDepth)) != nullptr);
}
TEST_CASE("SHOT OH: Spectral's card and window with Partials culling to 5 Harmonics (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — A CAMERA: spectral_card_oh.png (the whole editor; the card's position printed) and spectral_window_oh.png.
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	d.set(static_cast<size_t>(spx::Param::PartialsOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::PartialsMode), 1.0f);
	d.set(static_cast<size_t>(spx::Param::PartialsCount), 5.0f);
	auto* root = dynamic_cast<visage::ApplicationEditor*>(d.f.root);
	REQUIRE(root != nullptr);
	root->setWindowless(static_cast<int>(d.f.root->width()), static_cast<int>(d.f.root->height()));
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	d.lane->scrollCardIntoView(d.slot);
	for (int i = 0; i < 3; ++i) {
		(void)d.card->updateContent();
		d.f.root->redrawAll();
		root->drawWindow();
	}
	const visage::Screenshot& card = root->takeScreenshot();
	REQUIRE(card.width() > 0);
	card.save(std::string(dir) + "/spectral_card_oh.png");
	const visage::Point at = d.card->positionInWindow();
	MESSAGE("card " << at.x << "," << at.y << " " << d.card->width() << "x" << d.card->height());
	constexpr int w = 960;
	constexpr int h = 600;
	visage::ApplicationEditor editor;
	editor.setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	editor.setWindowless(w, h);
	editor.onDraw() = [&editor](visage::Canvas& canvas) {
		canvas.setColor(0xff101418);
		canvas.fill(0.0f, 0.0f, editor.width(), editor.height());
	};
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	editor.addChild(view.get());
	view->setBounds(0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h));
	for (int i = 0; i < 20; ++i) {
		(void)view->updateContent();
		editor.redrawAll();
		editor.drawWindow();
	}
	editor.takeScreenshot().save(std::string(dir) + "/spectral_window_oh.png");
	// AND IN CENTERS (OI): the cell's box is the set, the plot keeps the set's handful.
	d.set(static_cast<size_t>(spx::Param::PartialsMode), 2.0f);
	d.set(static_cast<size_t>(spx::Param::PartialsSet), 1.3f);
	for (int i = 0; i < 20; ++i) {
		(void)view->updateContent();
		editor.redrawAll();
		editor.drawWindow();
	}
	editor.takeScreenshot().save(std::string(dir) + "/spectral_window_oi_centers.png");
	editor.removeChild(view.get());
}
TEST_CASE("OI: Spectral's Partials cell shows the set in Centers and the count elsewhere, the card hides its count in Centers, and the plot keeps the set's harmonics") {
	DynCardFixture<ui::SpectralCard> d(spx::kTraits.tag);
	ui::SpectralCard* card = d.card;
	d.set(static_cast<size_t>(spx::Param::PartialsOn), 1.0f);
	d.set(static_cast<size_t>(spx::Param::PartialsDepth), 1.0f);
	(void)card->updateContent();
	const feathers::SpectralPlotFrame base = [&] {
		d.set(static_cast<size_t>(spx::Param::PartialsOn), 0.0f);
		(void)card->updateContent();
		const feathers::SpectralPlotFrame f = card->plotForTest().frameForTest();
		d.set(static_cast<size_t>(spx::Param::PartialsOn), 1.0f);
		return f;
	}();
	(void)card->updateContent();
	CHECK(card->widgetForTest(spx::Param::PartialsCount)->isVisible());
	d.set(static_cast<size_t>(spx::Param::PartialsMode), 2.0f); // Centers
	d.set(static_cast<size_t>(spx::Param::PartialsSet), 1.3f);
	(void)card->updateContent();
	CHECK_FALSE(card->widgetForTest(spx::Param::PartialsCount)->isVisible()); // not read by Centers: the window shows the set
	const feathers::SpectralPlotFrame frame = card->plotForTest().frameForTest();
	for (int i = 0; i < 12; ++i) {
		CAPTURE(i);
		const auto u = static_cast<size_t>(i);
		const double w = i < 7 ? feathers::partials::sparseKeep(i + 1, 1.3f) : 0.0; // the shared generator
		CHECK(frame.left[u] == doctest::Approx(w * base.left[u]).epsilon(1e-6));
	}
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, spx::kDescriptor);
	auto* window = dynamic_cast<ui::SpectralExpanded*>(view.get());
	REQUIRE(window != nullptr);
	(void)window->updateContent();
	auto* cell = dynamic_cast<ui::ModeValueCell*>(window->boxWidgetForTest(spx::Param::PartialsMode));
	REQUIRE(cell != nullptr);
	CHECK(cell->activeParam() == static_cast<long>(spx::Param::PartialsSet));
	d.set(static_cast<size_t>(spx::Param::PartialsMode), 1.0f);
	(void)window->updateContent();
	CHECK(cell->activeParam() == static_cast<long>(spx::Param::PartialsCount));
}
TEST_CASE("COMP's Detector control is Compress's, and the card says \"sidechain silent\" only for Side over a silent sidechain") {
	namespace cp = feathers::fx::comp;
	DynCardFixture<ui::CompCard> d(cp::kTraits.tag);
	ui::CompCard* card = d.card;
	const auto publish = [&](float sounding) {
		std::array<float, static_cast<size_t>(cp::Reading::kCount)> v {};
		v[static_cast<size_t>(cp::Reading::SidechainSounding)] = sounding;
		d.publish(v);
	};
	// SELF (the default): both choices are on the face, and nothing is ever said about a sidechain.
	CHECK(card->detectorForTest()->isVisible());
	publish(0.0f);
	CHECK_FALSE(card->sidechainSilent());
	// SIDE over a silent sidechain says so; over a sounding one it does not; and each change is reported.
	d.set(static_cast<size_t>(cp::Param::Detector), static_cast<float>(cp::Detector::Side));
	CHECK(card->updateContent());
	CHECK(card->sidechainSilent());
	publish(1.0f);
	CHECK(card->updateContent());
	CHECK_FALSE(card->sidechainSilent());
	publish(0.0f);
	CHECK(card->updateContent());
	CHECK(card->sidechainSilent());
	// LIMIT NEVER READS THE SIDECHAIN: the control is hidden and the line is gone with it.
	d.set(static_cast<size_t>(cp::Param::Mode), 1.0f);
	CHECK(card->updateContent());
	CHECK_FALSE(card->detectorForTest()->isVisible());
	CHECK_FALSE(card->sidechainSilent());
}
TEST_CASE("Sidechain Inject's card draws the injected level from its readings and says \"sidechain silent\" only when nothing sounds") {
	namespace ij = feathers::fx::inject;
	DynCardFixture<ui::InjectCard> d(ij::kTraits.tag);
	ui::InjectCard* card = d.card;
	// NOTHING PUBLISHED IS NOT A CLAIM: no fractions, and no "silent" line for a card that was told nothing.
	CHECK_FALSE(card->hasReadings());
	CHECK_FALSE(card->sidechainSilent());
	CHECK(card->barFraction() == 0.0f);
	// THE CARD REACHES EXACTLY MIX, GAIN AND THE ENABLE.
	CHECK(card->addressedParamsForTest().size() == 3);
	REQUIRE(card->knobForTest(ij::Param::Mix) != nullptr);
	REQUIRE(card->knobForTest(ij::Param::Gain) != nullptr);
	CHECK(card->knobForTest(ij::Param::Mix)->isVisible());
	const auto publish = [&](float injected, float sounding) {
		std::array<float, static_cast<size_t>(ij::Reading::kCount)> v {};
		v[static_cast<size_t>(ij::Reading::Injected)] = injected;
		v[static_cast<size_t>(ij::Reading::SidechainSounding)] = sounding;
		d.publish(v);
	};
	publish(0.0f, 0.0f);
	CHECK(card->updateContent());
	CHECK(card->sidechainSilent());
	publish(0.5f, 1.0f);
	CHECK(card->updateContent());
	CHECK_FALSE(card->sidechainSilent());
	CHECK(card->barFraction() == doctest::Approx(ui::dyn_meter::levelFraction(0.5f)));
	CHECK(card->barFraction() > 0.0f);
	// NO CHANGE, NO REPORT: the tick says "moved" only when something drawn did.
	CHECK_FALSE(card->updateContent());
	publish(0.5f, 0.0f);
	CHECK(card->updateContent());
	CHECK(card->sidechainSilent());
}
TEST_CASE("Sidechain cards: photographs of COMP's Detector row and the Inject card (FEATHERS_SHOT_DIR)") {
	// NOT AN ASSERTION — the venue for LOOKING (the Follower card's). Writes RGBA `.raw` frames of the whole editor with COMP
	// under Side over a silent sidechain, then Inject with readings. Skipped without the variable.
	namespace cp = feathers::fx::comp;
	namespace ij = feathers::fx::inject;
	const char* dir = std::getenv("FEATHERS_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	ModFixture f;
	auto* editor = dynamic_cast<visage::ApplicationEditor*>(f.root);
	REQUIRE(editor != nullptr);
	editor->setWindowless(static_cast<int>(f.root->width()), static_cast<int>(f.root->height()));
	if (!visage::Renderer::instance().supported()) {
		MESSAGE("SKIP no renderer: " << visage::Renderer::instance().errorMessage());
		return;
	}
	ui::FxLane* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t compSlot = f.bridge.addFxDevice(cp::kTraits.tag);
	const size_t injectSlot = f.bridge.addFxDevice(ij::kTraits.tag);
	lane->resized();
	auto* comp = dynamic_cast<ui::CompCard*>(lane->cardAt(compSlot));
	auto* inject = dynamic_cast<ui::InjectCard*>(lane->cardAt(injectSlot));
	REQUIRE(comp != nullptr);
	REQUIRE(inject != nullptr);
	const auto setField = [&](size_t slot, size_t control, float value) {
		f.bridge.setDeviceControl(f.bridge.fxDeviceAt(slot), control, value);
	};
	setField(compSlot, static_cast<size_t>(cp::Param::Detector), static_cast<float>(cp::Detector::Side));
	std::array<float, static_cast<size_t>(cp::Reading::kCount)> cv {};
	cv[static_cast<size_t>(cp::Reading::InPeak)] = 0.3f;
	cv[static_cast<size_t>(cp::Reading::OutPeak)] = 0.3f;
	f.bridge.publishDeviceReadings(compSlot, cv.data(), cv.size());
	std::array<float, static_cast<size_t>(ij::Reading::kCount)> iv {};
	iv[static_cast<size_t>(ij::Reading::Injected)] = 0.4f;
	iv[static_cast<size_t>(ij::Reading::SidechainSounding)] = 1.0f;
	f.bridge.publishDeviceReadings(injectSlot, iv.data(), iv.size());
	f.bridge.refresh();
	const auto shoot = [&](const char* name, ui::FxCard* card) {
		lane->scrollCardIntoView(card == comp ? compSlot : injectSlot);
		lane->resized();
		(void)comp->updateContent();
		(void)inject->updateContent();
		f.root->redrawAll();
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
		const visage::Point where = card->positionInWindow();
		MESSAGE(name << " " << shot.width() << "x" << shot.height() << " card " << where.x << "," << where.y << " "
		             << card->width() << "x" << card->height() << " -> " << path);
	};
	shoot("comp-side-silent", comp);
	shoot("inject-sounding", inject);
	iv[static_cast<size_t>(ij::Reading::Injected)] = 0.0f;
	iv[static_cast<size_t>(ij::Reading::SidechainSounding)] = 0.0f;
	f.bridge.publishDeviceReadings(injectSlot, iv.data(), iv.size());
	f.bridge.refresh();
	shoot("inject-silent", inject);
}
TEST_CASE("Rack stage 2: the card and the window offer the Mode as a dropdown and the Position only where the mode reads it — hidden, not greyed, in Parallel — and the Position's readout names the chain") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	REQUIRE(r.rack().addChain() == 1);
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	REQUIRE(card != nullptr);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	const visage::Point min = panel->minimumSize();
	panel->setBounds(0.0f, 0.0f, std::max(min.x, 900.0f), std::max(min.y, 600.0f));
	const auto refresh = [&] {
		(void)card->update();
		(void)panel->update();
	};
	refresh();

	// THE MODE IS A LIST SHOWN BEFORE ANYTHING MOVES (CLAUDE.md: a multivalue toggle is an antipattern): every mode,
	// the menu the box opens itself, in the window (the card carries no mode selector).
	for (ui::ValueBox* box : { &panel->modeBoxForTest() }) {
		CHECK(box->choiceMenuAvailable());
		CHECK(box->opensMenu());
		CHECK(box->entryCount() == static_cast<size_t>(pr::Mode::kCount));
		CHECK(box->readout() == "Parallel");
		CHECK(box->control().desc().key == std::string("rack_mode"));
	}

	// PARALLEL: the position is not drawn at all.
	CHECK_FALSE(card->positionKnobForTest().isVisible());
	CHECK_FALSE(panel->positionKnobForTest().isVisible());
	for (const pr::Mode mode : { pr::Mode::Select, pr::Mode::Blend }) {
		CAPTURE(static_cast<int>(mode));
		f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::Mode), static_cast<float>(mode));
		refresh();
		CHECK(card->positionKnobForTest().isVisible());
		CHECK(panel->positionKnobForTest().isVisible());
		CHECK(panel->modeBoxForTest().readout() == pr::kModeNames[static_cast<size_t>(mode)]);
	}
	// THE READOUT IS THE RACK'S OWN WORDS, through the device's hook and the rack's live chain count.
	f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::Mode), static_cast<float>(pr::Mode::Select));
	f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::Position), 1.0f);
	refresh();
	CHECK(card->positionKnobForTest().control()->text() == "Chain 2");
	CHECK(panel->positionKnobForTest().control()->text() == "Chain 2");
	// TYPED IN THE SAME WORDS: "Chain 1" is the first chain's centre.
	const std::optional<double> typed = card->positionKnobForTest().control()->parseText("Chain 1", 1.0);
	REQUIRE(typed.has_value());
	CHECK(*typed == 0.0);

	f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::Mode), static_cast<float>(pr::Mode::Parallel));
	refresh();
	CHECK_FALSE(card->positionKnobForTest().isVisible());
	CHECK_FALSE(panel->positionKnobForTest().isVisible());
	CHECK(card->positionKnobForTest().control()->text() == feathers::device::formatValue(pr::kParamDescs[static_cast<size_t>(pr::Param::Position)], 1.0));
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("Rack stage 2: the Unused chip shows only in the role modes (Mid/Side, Left/Right, Bands), on the card and in the window, and writes Ring out / Stop") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	REQUIRE(card != nullptr);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 900.0f, std::max(panel->minimumSize().y, 600.0f));
	for (size_t m = 0; m < static_cast<size_t>(pr::Mode::kCount); ++m) {
		CAPTURE(m);
		f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::Mode), static_cast<float>(m));
		(void)card->update();
		(void)panel->update();
		const bool roles = m == static_cast<size_t>(pr::Mode::MidSide) || m == static_cast<size_t>(pr::Mode::LeftRight)
		                   || m == static_cast<size_t>(pr::Mode::Bands);
		CHECK(card->unusedChipForTest().isVisible() == roles);
		CHECK(panel->unusedChipForTest().isVisible() == roles);
	}
	// ITS CONTROL IS rack_unused: on writes Stop.
	std::vector<ui::ControlFace::Offered> offered;
	card->unusedChipForTest().offeredControls(offered);
	REQUIRE(offered.size() == 1);
	CHECK(offered[0].control->desc().key == std::string("rack_unused"));
	const_cast<feathers::device::Modulatable*>(offered[0].control)->setValue(1.0);
	CHECK(f.deviceHost.fieldAt(r.id, static_cast<size_t>(pr::Param::Unused)) == 1.0f);
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("Rack stage 2, Bands: the band controls show only in Bands, and a corner only below the band count — 1 of them at 2 bands, 2 at 3, 4 at 5 — on the card's spectrum strip and as the window's knobs") {
	namespace pr = feathers::rack::parallel;
	ModFixture f;
	RackOnLane r(f);
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	REQUIRE(card != nullptr);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, pr::kDescriptor);
	auto* panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, 900.0f, std::max(panel->minimumSize().y, 600.0f));
	const auto refresh = [&] {
		(void)card->update();
		(void)panel->update();
	};
	refresh();
	for (size_t k = 0; k < 4; ++k) {
		CHECK_FALSE(panel->cutKnobForTest(k).isVisible()); // Parallel: none
	}
	CHECK(card->spectrumForTest().cornerCount() == 0);
	CHECK_FALSE(card->bandsBoxForTest().isVisible());
	const float parallelHeight = card->laneHeightAt(300.0f);
	f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::Mode), static_cast<float>(pr::Mode::Bands));
	for (const auto& [choice, shown] : std::vector<std::pair<float, size_t>> { { 0.0f, 1 }, { 1.0f, 2 }, { 2.0f, 4 } }) {
		CAPTURE(choice);
		f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::BandCount), choice);
		refresh();
		CHECK(card->bandsBoxForTest().isVisible());
		CHECK(card->bandsBoxForTest().choiceMenuAvailable()); // three counts: a dropdown
		for (size_t k = 0; k < 4; ++k) {
			CAPTURE(k);
			CHECK(panel->cutKnobForTest(k).isVisible() == (k < shown));
		}
		// THE CARD CARRIES THE CORNERS ON ITS SPECTRUM STRIP, which a drag writes (the knobs are the
		// window's, so the card fits a horizontal chain's unit).
		CHECK(card->spectrumForTest().isVisible());
		CHECK(card->spectrumForTest().cornerCount() == static_cast<int>(shown));
	}
	// THE CARD GROWS BY THE TWO BAND ROWS, and gives them back outside Bands.
	CHECK(card->laneHeightAt(300.0f) > parallelHeight);
	f.deviceHost.setField(r.id, static_cast<size_t>(pr::Param::Mode), static_cast<float>(pr::Mode::Parallel));
	refresh();
	CHECK(card->laneHeightAt(300.0f) == parallelHeight);
	view.reset();
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}

namespace {
/// A RACK ON A LANE WITH ITS CARD AND WINDOW, laid out — the step-4 cases' common ground.
struct RackViews {
	ModFixture f;
	RackOnLane r { f };
	ui::ParallelRackCard* card = nullptr;
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view;
	ui::ParallelRackExpanded* panel = nullptr;
	RackViews() {
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		lane->resized();
		card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
		REQUIRE(card != nullptr);
		view = ui::makeExpandedView(f.deviceHost, &f.bridge, registry, r.id, feathers::rack::parallel::kDescriptor);
		panel = dynamic_cast<ui::ParallelRackExpanded*>(view.get());
		REQUIRE(panel != nullptr);
		panel->setBounds(0.0f, 0.0f, 1200.0f, std::max(panel->minimumSize().y, 700.0f));
		refresh();
	}
	~RackViews() {
		view.reset();
		f.bridge.fxBodies().detach(r.id, r.body);
		(void)f.bridge.removeFxDevice(r.slot);
	}
	void set(feathers::rack::parallel::Param p, float v) { f.deviceHost.setField(r.id, static_cast<size_t>(p), v); }
	float field(feathers::rack::parallel::Param p) const { return f.deviceHost.fieldAt(r.id, static_cast<size_t>(p)); }
	void refresh() {
		(void)card->update();
		(void)panel->update();
		card->resized();
		panel->resized();
		(void)card->update();
		(void)panel->update();
	}
};
} // namespace

TEST_CASE("Rack stage 2, step 4: every multi-choice control shows its choices before one is applied — Mode, Bands and Filtering open a list, Slope and Unused are segmented, never a cycling chip") {
	namespace pr = feathers::rack::parallel;
	RackViews v;
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Bands));
	v.refresh();
	for (ui::ValueBox* box : { &v.panel->modeBoxForTest(), &v.card->bandsBoxForTest() }) {
		CHECK(box->choiceMenuAvailable());
		CHECK(box->opensMenu());
	}
	// SLOPE AND UNUSED: every choice on the face, one segment each.
	CHECK(v.card->slopeForTest().isVisible());
	CHECK(v.card->unusedChipForTest().isVisible());
	const float slopeW = v.card->slopeForTest().width();
	REQUIRE(slopeW > 0.0f);
	// THE SLOPE HAS MORE CHOICES THAN A FACE HOLDS (the LR pairs and the linear steepnesses): a list that opens before anything moves.
	CHECK(v.card->slopeForTest().choiceMenuAvailable());
	CHECK(v.card->slopeForTest().opensMenu());
	CHECK(v.panel->unusedChipForTest().isVisible());
}
TEST_CASE("Rack stage 2, step 4 / §7a: the spectrum strip shows in Bands, Mid/Side and Left/Right only, its corner lines stand at the corners the ENGINE runs, and a drag writes the cut — pushing a neighbour a third of an octave clear") {
	namespace pr = feathers::rack::parallel;
	RackViews v;
	bool previousStrip = false;
	for (size_t m = 0; m < static_cast<size_t>(pr::Mode::kCount); ++m) {
		CAPTURE(m);
		if (static_cast<pr::Mode>(m) == pr::Mode::Keyed) {
			continue; // Parallel's view and audio; it comes after Bands here, where the strip's lease is still running down
		}
		v.set(pr::Param::Mode, static_cast<float>(m));
		v.refresh();
		const bool strip = (pr::kSpectrumModes & pr::kModeBit(static_cast<pr::Mode>(m))) != 0;
		CHECK(v.card->spectrumForTest().isVisible() == strip);
		CHECK(v.panel->spectrumForTest().isVisible() == strip);
		// THE LEASE: a strip that is shown asks the rack for readings; none is asked for where none is shown. A lease is a time (it runs out
		// by itself, and no audio runs here), so the first mode WITHOUT a strip after one WITH it still holds the last one's: not asked.
		if (strip || !previousStrip) {
			CHECK((v.r.rack().leaseForTest() > 0) == strip);
		}
		previousStrip = strip;
	}
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Bands));
	v.set(pr::Param::BandCount, 2.0f); // 5 bands
	v.refresh();
	ui::RackSpectrumStrip& strip = v.card->spectrumForTest();
	REQUIRE(strip.cornerCount() == 4);
	const float w = strip.width();
	REQUIRE(w > 0.0f);
	// BY VALUE: x = (log2 f − log2 20) / (log2 20000 − log2 20) · width, f the corner the engine runs (`orderedCorners`).
	float cuts[4];
	for (size_t k = 0; k < 4; ++k) {
		cuts[k] = v.field(static_cast<pr::Param>(static_cast<size_t>(pr::Param::Cut1) + k));
	}
	double hz[4];
	pr::orderedCorners(cuts, hz);
	for (int k = 0; k < 4; ++k) {
		const double want = (std::log2(hz[k]) - std::log2(20.0)) / (std::log2(20000.0) - std::log2(20.0)) * w;
		CHECK(strip.cornerXForTest(k) == doctest::Approx(want).epsilon(1e-4));
	}
	// A DRAG: corner 2 to 1 kHz writes Cut 2 = log2(1000), within a pixel's worth.
	strip.dragCornerForTest(1, strip.xOfHz(1000.0));
	CHECK(v.field(pr::Param::Cut2) == doctest::Approx(std::log2(1000.0)).epsilon(1e-3));
	// AND PAST ITS NEIGHBOUR: corner 1 dragged to 3 kHz pushes corner 2 to a third of an octave above it.
	strip.dragCornerForTest(0, strip.xOfHz(3000.0));
	CHECK(v.field(pr::Param::Cut1) == doctest::Approx(std::log2(3000.0)).epsilon(1e-3));
	CHECK(v.field(pr::Param::Cut2) == doctest::Approx(std::log2(3000.0) + pr::kMinCornerOctaves).epsilon(1e-3));
	// THE DRAG IS OFFERED ON THE STRIP'S FACE: the guard sees the cuts there.
	std::vector<ui::ControlFace::Offered> offered;
	strip.offeredControls(offered);
	CHECK(offered.size() == 4);
}
TEST_CASE("Rack stage 2, step 4: the position bar in Select and Blend — its cursor stands over the chain Select hears, a drag to a chain's segment selects it, and Blend's fills are the weights the sum uses") {
	namespace pr = feathers::rack::parallel;
	RackViews v;
	REQUIRE(v.r.rack().addChain() == 1);
	REQUIRE(v.r.rack().addChain() == 2);
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	v.set(pr::Param::Position, 0.5f);
	v.refresh();
	ui::RackPositionBar& bar = v.card->positionBarForTest();
	REQUIRE(bar.isVisible());
	const float seg = bar.width() / 3.0f;
	CHECK(bar.cursorXForTest() == doctest::Approx(1.5f * seg)); // chain 2's centre
	// AND AT THE ENDS, where a plain `position × width` map would part from it: position 0 is chain 1's CENTRE.
	v.set(pr::Param::Position, 0.0f);
	v.refresh();
	(void)bar.refresh();
	CHECK(bar.cursorXForTest() == doctest::Approx(0.5f * seg));
	bar.dragForTest(0.5f * seg, 2.5f * seg);                    // to chain 3's centre
	CHECK(v.field(pr::Param::Position) == doctest::Approx(1.0f));
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Blend));
	v.set(pr::Param::Position, 0.7f);
	v.refresh();
	(void)bar.refresh();
	for (size_t k = 0; k < 3; ++k) {
		CHECK(bar.fillForTest(k) == doctest::Approx(pr::positionWeight(pr::Mode::Blend, 0.7, k, 3)));
	}
	CHECK(bar.fillForTest(1) == doctest::Approx(0.6f)); // 0.7 of 3 chains is 1.4: chain 2 at 0.6, chain 3 at 0.4
	CHECK(v.panel->positionBarForTest().isVisible());
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Parallel));
	v.refresh();
	CHECK_FALSE(bar.isVisible());
}
TEST_CASE("Rack stage 2, step 4 / §2a: a role no chain fills is drawn as an EMPTY CHAIN — role, level, M, S and what passes — made real by its first edit, never by a mode change") {
	namespace pr = feathers::rack::parallel;
	RackViews v;
	REQUIRE(v.r.rack().chainCount() == 1);
	// A MODE CHANGE MAKES NO CHAINS (§2a): Mid/Side, Bands of five and Left/Right over one chain leave it one chain.
	for (const float mode : { static_cast<float>(pr::Mode::MidSide), static_cast<float>(pr::Mode::Bands),
	                          static_cast<float>(pr::Mode::LeftRight), static_cast<float>(pr::Mode::MidSide) }) {
		v.set(pr::Param::Mode, mode);
		v.set(pr::Param::BandCount, 2.0f);
		v.refresh();
		CHECK(v.r.rack().chainCount() == 1);
	}
	// THE FACE: the card's phantom strip names the role and what passes, and carries a level, an M and an S.
	REQUIRE(v.card->placeholderCountForTest() == 1);
	ui::ChainStrip& phantom = v.card->phantomForTest(0);
	CHECK(phantom.summaryForTest() == "2  Side \xC2\xB7 empty");
	// ITS LABEL FITS ITS ROOM — not truncated — and what passes is its hover.
	CHECK(phantom.summaryTextWidthForTest() <= phantom.summaryWidth());
	CHECK(phantom.tooltipAt({ 2.0f, 4.0f }) == std::string("empty, passes the side"));
	CHECK(phantom.levelPill().isVisible());
	CHECK(phantom.muteChip().isVisible());
	CHECK(phantom.soloChip().isVisible());
	CHECK(v.card->stripsForTest()[0]->summaryForTest().find("Mid") != std::string::npos);
	// AND THE WINDOW'S PHANTOM RAIL: the role, the strip, the zone saying what passes.
	REQUIRE(v.panel->placeholderCountForTest() == 1);
	CHECK(v.panel->phantomForTest(0).labelForTest() == "2 Side");
	CHECK(v.panel->phantomForTest(0).passesForTest() == "empty, passes the side");
	// AND IT IS THE RAIL A REAL EMPTY CHAIN GETS: chain 1 is a real empty chain here, and its rail and the phantom's
	// have the same width, closed and open, and the same strip — the vertical fader of the closed column.
	// (A NEW CHAIN STARTS OPEN, so the real chain is folded here to compare it with the phantom's closed rail.)
	v.r.rack().setOpen(0, false);
	v.refresh();
	ui::FxLaneRail& realRail = v.panel->railForTest(0);
	ui::FxLaneRail& phantomRail = v.panel->phantomForTest(0).railForTest();
	REQUIRE(realRail.collapsed());
	CHECK(phantomRail.collapsed());
	CHECK(phantomRail.railWidth() == realRail.railWidth());
	CHECK(phantomRail.bounds().width() == realRail.bounds().width());
	CHECK(v.panel->phantomForTest(0).stripForTest().columnLayoutForTest() == v.panel->stripForTest(0).columnLayoutForTest());
	CHECK(v.panel->phantomForTest(0).stripForTest().columnLayoutForTest()); // the vertical fader
	(void)realRail.setCollapsed(false);
	(void)phantomRail.setCollapsed(false);
	CHECK(phantomRail.railWidth() == realRail.railWidth());
	(void)realRail.setCollapsed(true);
	(void)phantomRail.setCollapsed(true);
	v.refresh();
	// THE FIRST EDIT MAKES THE CHAIN: the phantom's M pressed mutes a real chain 2.
	phantom.muteControlForTest().setValue(1.0);
	REQUIRE(v.r.rack().chainCount() == 2);
	CHECK(v.r.rack().muted(1));
	v.refresh();
	CHECK(v.card->placeholderCountForTest() == 0);
	CHECK(v.panel->placeholderCountForTest() == 0);
	CHECK(v.card->stripsForTest()[1]->summaryForTest().find("Side") != std::string::npos);

	// A DRAG IS HELD UNTIL THE HAND LETS GO, so the strip under it is not rebuilt mid-gesture; then the chain — and the
	// empty ones before it, which its role needs — are made, at the level the hand left.
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Bands));
	v.refresh();
	REQUIRE(v.card->placeholderCountForTest() == 3); // bands 3, 4 and 5 over two chains
	ui::ChainControl& level5 = v.card->phantomForTest(2).levelControlForTest();
	level5.beginGesture();
	level5.setValue(0.25);
	CHECK(v.r.rack().chainCount() == 2); // held
	CHECK(level5.value() == doctest::Approx(0.25));
	level5.endGesture();
	REQUIRE(v.r.rack().chainCount() == 5);
	CHECK(v.r.rack().level(4) == doctest::Approx(0.25f));
	CHECK(v.r.rack().level(2) == doctest::Approx(static_cast<float>(feathers::composite::kFaderUnity)));

	// THE WINDOW'S DEVICE ZONE MAKES ITS CHAIN TOO.
	RackViews w;
	w.set(pr::Param::Mode, static_cast<float>(pr::Mode::LeftRight));
	w.refresh();
	REQUIRE(w.panel->placeholderCountForTest() == 1);
	CHECK(w.panel->phantomForTest(0).passesForTest() == "empty, passes the right");
	w.panel->phantomForTest(0).pressZoneForTest();
	CHECK(w.r.rack().chainCount() == 2);
}
TEST_CASE("Rack stage 2, step 4: in the smallest view the editor can give the window, every Bands control and Unused lie wholly inside it — none runs off the bottom") {
	namespace pr = feathers::rack::parallel;
	// THE SMALLEST VISIBLE AREA: the editor's design height (`Gui::kLogicalHeight`, its minimum) less the overlay's title
	// and its two pads — restated from the overlay's own constants (`ExpandedOverlay::fitted`).
	const float visible = static_cast<float>(feathers::Gui::kLogicalHeight) - ui::ExpandedOverlay::kTitleHeight
	                      - 2.0f * ui::ExpandedOverlay::kPanelPad;
	RackViews v;
	REQUIRE(v.r.rack().addChain() == 1);
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Bands));
	v.set(pr::Param::BandCount, 2.0f); // 5 bands: all four corners
	v.refresh();
	const visage::Point min = v.panel->minimumSize();
	CAPTURE(min.y);
	CHECK(min.y <= visible); // the window fits
	v.panel->setBounds(0.0f, 0.0f, min.x, visible);
	v.refresh();
	std::vector<visage::Frame*> frames { &v.panel->bandsBoxForTest(), &v.panel->filteringBoxForTest(), &v.panel->slopeForTest() };
	for (size_t k = 0; k < 4; ++k) {
		frames.push_back(&v.panel->cutKnobForTest(k));
	}
	for (visage::Frame* f : frames) {
		CAPTURE(f->bounds().y());
		CHECK(f->isVisible());
		CHECK(f->bounds().y() >= 0.0f);
		CHECK(f->bounds().bottom() <= visible);
		CHECK(f->bounds().right() <= min.x);
	}
	// UNUSED HAS ITS ROOM: the column's width, not one cell's.
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::MidSide));
	v.refresh();
	CHECK(v.panel->unusedChipForTest().isVisible());
	CHECK(v.panel->unusedChipForTest().width() >= ui::ParallelRackExpanded::kColumnWidth);
	CHECK(v.panel->unusedChipForTest().bounds().bottom() <= visible);
}
TEST_CASE("Rack Key Track: the switch shows only in Bands, in the window; while a note is tracked the strip's corners follow it, a drag writes the cut's OFFSET, and a Cut reads in semitones") {
	namespace pr = feathers::rack::parallel;
	RackViews v;
	v.set(pr::Param::KeyTrack, 1.0f);
	v.refresh();
	CHECK_FALSE(v.panel->keyTrackForTest().isVisible()); // Parallel
	v.set(pr::Param::Mode, static_cast<float>(pr::Mode::Bands));
	v.set(pr::Param::BandCount, 2.0f);
	v.refresh();
	CHECK(v.panel->keyTrackForTest().isVisible());
	// THE RACK PUBLISHES THE NOTE IT TRACKS (`Reading::NoteHz`): A3, 220 Hz.
	std::vector<float> readings(pr::kReadingCount, 0.0f);
	readings[static_cast<size_t>(pr::Reading::NoteHz)] = 220.0f;
	v.f.bridge.publishDeviceReadings(v.r.slot, readings.data(), readings.size());
	v.refresh();
	ui::RackSpectrumStrip& strip = v.card->spectrumForTest();
	const double ref = static_cast<double>(static_cast<float>(pr::kParamDescs[static_cast<size_t>(pr::Param::Cut1)].def));
	for (int k = 0; k < 4; ++k) {
		CAPTURE(k);
		const double cut = static_cast<double>(v.field(static_cast<pr::Param>(static_cast<size_t>(pr::Param::Cut1) + static_cast<size_t>(k))));
		const double want = 220.0 * std::pow(2.0, cut - ref); // restated: the note moved by the cut's offset
		CHECK(strip.cornerXForTest(k) == doctest::Approx(strip.xOfHz(want)).epsilon(1e-4));
	}
	CHECK(strip.cornerXForTest(0) == doctest::Approx(strip.xOfHz(220.0)).epsilon(1e-4)); // cut 1's default: the note
	// THE READOUT: Cut 1 at its default is "0.0 st"; dragged to 440 Hz it is an octave over the note.
	CHECK(v.panel->cutKnobForTest(0).control()->text() == "0.0 st");
	strip.dragCornerForTest(0, strip.xOfHz(440.0));
	CHECK(v.field(pr::Param::Cut1) == doctest::Approx(ref + 1.0).epsilon(1e-3));
	CHECK(v.panel->cutKnobForTest(0).control()->text() == "+12.0 st");
	// NOT TRACKING (the reading 0): Hz again, and the strip on the cuts themselves.
	readings[static_cast<size_t>(pr::Reading::NoteHz)] = 0.0f;
	v.f.bridge.publishDeviceReadings(v.r.slot, readings.data(), readings.size());
	v.refresh();
	CHECK(v.panel->cutKnobForTest(0).control()->text().find("st") == std::string::npos);
	CHECK(strip.cornerXForTest(0) == doctest::Approx(strip.xOfHz(std::exp2(ref + 1.0))).epsilon(1e-3));
}

namespace {
/// EVERY CONTROL OFFERED ON A FACE under `frame`, depth first.
void collectFaces(const visage::Frame* frame, std::vector<ui::ControlFace::Offered>& out) {
	if (const auto* face = dynamic_cast<const ui::ControlFace*>(frame); face != nullptr) {
		face->offeredControls(out);
	}
	for (const visage::Frame* child : frame->children()) {
		collectFaces(child, out);
	}
}
} // namespace

TEST_CASE("OWLTT Key Track: the window offers the switch; while a note is tracked the graph's crossovers stand on the note moved by their offsets, a drag writes the offset, and the knobs read in semitones") {
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	const feathers::DeviceInstanceId id = f.bridge.fxDeviceAt(1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, id };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	const size_t low = static_cast<size_t>(fx::owltt::Param::LowCrossover);
	const size_t high = static_cast<size_t>(fx::owltt::Param::HighCrossover);
	// THE SWITCH IS ON THE WINDOW'S FACE.
	bool offered = false;
	std::vector<ui::ControlFace::Offered> faces;
	collectFaces(&panel, faces);
	for (const auto& o : faces) {
		offered = offered || o.control->desc().key == std::string("owltt_key_track");
	}
	CHECK(offered);
	f.deviceHost.setField(id, static_cast<size_t>(fx::owltt::Param::KeyTrack), 1.0f);
	// THE DEVICE PUBLISHES THE NOTE IT TRACKS (reading 17), on the lane's own OWLTT road: A3.
	const std::array<float, 3> zero { 0.0f, 0.0f, 0.0f };
	f.bridge.publishOwlttMeters(1, zero, zero, zero, false, { false, false, false }, 0.0f, nullptr, 220.0f);
	(void)panel.update();
	ui::OwlttGraph* graph = panel.graph();
	REQUIRE(graph != nullptr);
	const float lowKnob = f.deviceHost.fieldAt(id, low);
	const float highKnob = f.deviceHost.fieldAt(id, high);
	CHECK(graph->cornerHz(0) == doctest::Approx(220.0 * lowKnob / 200.0).epsilon(1e-6));
	CHECK(graph->cornerHz(1) == doctest::Approx(220.0 * highKnob / 200.0).epsilon(1e-6));
	// A DRAG OF THE LOW LINE TO 440 Hz WRITES THE KNOB WHOSE OFFSET PUTS IT THERE: 400 Hz, an octave over the reference.
	const visage::Point handle = graph->crossoverHandle(0);
	const visage::Bounds box = graph->plot();
	const float toX = box.x() + box.width() * static_cast<float>(std::log2(440.0 / 20.0) / std::log2(20000.0 / 20.0));
	graph->onMouseDown().callback(at(handle.x, handle.y));
	graph->onMouseDrag().callback(at(toX, handle.y));
	graph->onMouseUp().callback(at(toX, handle.y));
	CHECK(f.deviceHost.fieldAt(id, low) == doctest::Approx(400.0f).epsilon(0.01));
	CHECK(graph->cornerHz(0) == doctest::Approx(440.0).epsilon(0.01));
	// THE KNOB'S OWN WORDS WHILE TRACKED, by `fx::owltt::kTextOps`.
	ui::DeviceControl lowControl { f.deviceHost, id, fx::owltt::kDescriptor, low };
	CHECK(lowControl.text().find("st") != std::string::npos);
}
TEST_CASE("OWLTT Key Track: the window's chip reads 'Key Track', as the Rack's does, with the room to say it") {
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	ui::WidgetRegistry registry;
	ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0.0f, 0.0f, want.x, want.y);
	panel.resized();
	ui::SwitchChip* chip = panel.keyTrackForTest();
	REQUIRE(chip != nullptr);
	CHECK(chip->label() == "Key Track");
	const visage::Font& face = ui::font(ui::kKnobLabelSize);
	const std::string text = chip->label();
	CHECK(face.stringWidth(std::u32string(text.begin(), text.end())) <= chip->width());
}
TEST_CASE("Rack card in a horizontal chain: in every mode, at 1 to 7 chains and every band count, the card's content fits "
          "the chain's card unit — the chains it has no room for are counted on a line, and the window holds them all") {
	namespace pr = feathers::rack::parallel;
	RackViews v;
	for (size_t chains = 1; chains <= 7; ++chains) {
		while (v.r.rack().chainCount() < chains) {
			(void)v.r.rack().addChain();
		}
		for (int m = 0; m <= static_cast<int>(pr::Mode::Bands); ++m) {
			v.set(pr::Param::Mode, static_cast<float>(m));
			for (float bands : { 0.0f, 1.0f, 2.0f, 3.0f }) {
				CAPTURE(chains);
				CAPTURE(m);
				CAPTURE(bands);
				v.set(pr::Param::BandCount, bands);
				v.refresh();
				// LAID OUT AS THE CHAIN WINDOW LAYS IT: horizontal, one unit tall (`RackEditor::placeLane`).
				v.card->setLaneAxis(ui::FxCard::LaneAxis::Horizontal);
				v.card->setBounds(0.0f, 0.0f, v.card->laneExtent(ui::kCardUnitHeight), ui::kCardUnitHeight);
				v.refresh();
				CHECK(v.card->contentHeight() <= ui::kCardUnitHeight);
				CHECK_FALSE(v.card->scrollable());
				// EVERY CHAIN ROW IS SHOWN OR COUNTED, never silently dropped.
				const size_t roles = pr::roleCount(static_cast<pr::Mode>(m), chains, pr::bandCountOf(bands));
				const bool roleMode = (pr::kRoleModes & pr::kModeBit(static_cast<pr::Mode>(m))) != 0;
				const size_t rows = chains + (roleMode && roles > chains ? roles - chains : 0);
				size_t shown = 0;
				for (const auto& strip : v.card->stripsForTest()) {
					shown += strip->isVisible() ? 1 : 0;
				}
				for (size_t k = 0; k < v.card->placeholderCountForTest(); ++k) {
					shown += v.card->phantomForTest(k).isVisible() ? 1 : 0;
				}
				CHECK(shown + v.card->hiddenRowsForTest() == rows);
				if (v.card->hiddenRowsForTest() > 0) {
					CHECK(v.card->overflowTextForTest().find(std::to_string(v.card->hiddenRowsForTest())) != std::string::npos);
				}
			}
		}
	}
}
TEST_CASE("Pattern and Arp cards in a horizontal chain: at every Mode and every tab focus the content fits the chain's card "
          "unit, no two controls overlap, and every control lies in the card — the Rack card's rule, for the note devices") {
	// In a horizontal chain the card overlapped its own widgets. The Rack card's case (above)
	// asserted the unit; this asserts it and the overlaps, per state.
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	namespace ap = feathers::mod::arp;
	ModFixture f;
	const size_t patSlot = f.bridge.addRackDevice(pt::kTraits.tag);
	const size_t arpSlot = f.bridge.addRackDevice(ap::kTraits.tag);
	REQUIRE(patSlot < feathers::kMaxRackSlots);
	REQUIRE(arpSlot < feathers::kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* pattern = const_cast<ui::PatternCard*>(dynamic_cast<const ui::PatternCard*>(f.panel->cardFrameForTest(patSlot)));
	auto* arp = const_cast<ui::ArpCard*>(dynamic_cast<const ui::ArpCard*>(f.panel->cardFrameForTest(arpSlot)));
	REQUIRE(pattern != nullptr);
	REQUIRE(arp != nullptr);
	const DeviceInstanceId id = pattern->device();
	for (size_t m = 0; m < static_cast<size_t>(gn::Mode::kCount); ++m) {
		for (size_t focus = 0; focus < pt::kPatterns; ++focus) {
			CAPTURE(m);
			CAPTURE(focus);
			f.deviceHost.setField(id, pt::at(focus, pt::Field::Mode), static_cast<float>(m));
			pattern->showPage(focus);
			checkFitsChainUnit(*pattern);
		}
	}
	// EVERY SCALE'S NAME, in the pitched compact box (Melody, the first tab).
	f.deviceHost.setField(id, pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::Melody));
	pattern->showPage(0);
	for (size_t s = 0; s < feathers::mod::arp::theory::kScaleCount; ++s) {
		CAPTURE(s);
		f.deviceHost.setField(id, pt::at(0, pt::Field::Scale), static_cast<float>(s));
		checkFitsChainUnit(*pattern);
	}
	// EVERY FEEL, its Amount box shown where it has one.
	for (size_t feel = 0; feel < std::size(ap::humanize::kModeNames); ++feel) {
		CAPTURE(feel);
		f.deviceHost.setField(arp->device(), ap::at(ap::Param::Feel), static_cast<float>(feel));
		checkFitsChainUnit(*arp);
	}
}

namespace {
//------------------------------------------------------------------------------------------------//
// BACKLOG PA: THE PLOTS DRAW THE RIGHT EAR — the LFO's and the MSEG's, while Stereo Phase is not 0  //
//------------------------------------------------------------------------------------------------//

/// **HOW MANY PIXELS OF THE CARD WEAR THE RIGHT EAR'S PINK** — the card's own paint, photographed alone; saved to
/// `$FEATHERS_STEREO_TRACE_SHOT_DIR/<name>.png` when that names a directory (a camera for LOOKING, off by default).
size_t pinkPixelsOf(ui::FxCard& card, const char* name) {
	const int width = static_cast<int>(std::ceil(card.width()));
	const int height = static_cast<int>(std::ceil(card.height()));
	REQUIRE(width > 0);
	REQUIRE(height > 0);
	visage::Canvas canvas;
	markedWindowless(canvas, width, height);
	canvas.setColor(0xff000000);
	canvas.fill(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
	visage::Region region;
	region.setBounds(0, 0, width, height);
	canvas.addRegion(&region);
	canvas.beginRegion(&region);
	card.draw(canvas);
	canvas.endRegion();
	const visage::Screenshot shot = takeMarkedCapture(canvas, width, height);
	if (const char* dir = std::getenv("FEATHERS_STEREO_TRACE_SHOT_DIR")) {
		shot.save((std::string(dir) + "/" + name + ".png").c_str());
	}
	const uint32_t pink = ui::kModulationR;
	const auto near = [](unsigned got, uint32_t want) {
		return std::abs(static_cast<int>(got) - static_cast<int>(want & 0xFFu)) <= 24;
	};
	size_t count = 0;
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const visage::Color c = shot.sample(x, y);
			count += near(c.hexRed(), pink >> 16) && near(c.hexGreen(), pink >> 8) && near(c.hexBlue(), pink) ? 1u : 0u;
		}
	}
	return count;
}
} // namespace

TEST_CASE("backlog PA: the LFO's plot draws the right ear exactly while Stereo Phase is not 0 — the wave read the offset on, by value") {
	namespace lfo = feathers::mod::lfo;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(slot >= 1);
	f.bridge.refresh();
	panel->resized();
	auto* card = const_cast<ui::LfoCard*>(dynamic_cast<const ui::LfoCard*>(panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const auto set = [&](lfo::Param p, float v) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(p), v);
		f.bridge.refresh();
		card->updateContent();
	};
	set(lfo::Param::Smoothing, static_cast<float>(lfo::kParamDescs[static_cast<size_t>(lfo::Param::Smoothing)].min));

	SUBCASE("at 0: one line, the left ear's, and no pink anywhere") {
		CHECK(card->curveRight().empty());
		CHECK(pinkPixelsOf(*card, "lfo-mono") == 0);
		CHECK(card->rightStrokesDrawnForTest() == 0);
	}
	SUBCASE("a shaped wave: the right ear's line is the wave at the left ear's x + offset, wrapped") {
		for (const lfo::Waveform wave : { lfo::Waveform::Sine, lfo::Waveform::Triangle, lfo::Waveform::Square }) {
			for (const double degrees : { 90.0, 180.0 }) {
				CAPTURE(static_cast<int>(wave));
				CAPTURE(degrees);
				set(lfo::Param::Waveform, static_cast<float>(static_cast<int>(wave)));
				set(lfo::Param::Phase, 0.25f);
				set(lfo::Param::StereoPhase, static_cast<float>(degrees * M_PI / 180.0));
				// THE REFERENCE IS BUILT FROM WHAT THIS CASE SET and the engine's own `eval`, not read back off the card.
				lfo::Settings s;
				s.wave = wave;
				s.phase = 0.25f;
				const auto right = card->curveRight();
				const auto left = card->curve();
				REQUIRE(right.size() == left.size());
				size_t apart = 0;
				for (size_t i = 0; i + 1 < right.size(); ++i) {
					CAPTURE(i);
					CHECK(right[i].first == left[i].first);
					const double at = 0.25 + degrees / 360.0 + right[i].first;
					CHECK(right[i].second == doctest::Approx(lfo::eval(s, static_cast<float>(at - std::floor(at)))).epsilon(1e-5));
					apart += std::abs(right[i].second - left[i].second) > 1e-3f ? 1u : 0u;
				}
				CHECK(apart > right.size() / 4);
				CHECK(pinkPixelsOf(*card, "lfo-shaped") > 50);
				CHECK(card->rightStrokesDrawnForTest() == right.size() - 1);
			}
		}
	}
	SUBCASE("a walk: the right ear's scope is the same walk's, ending the offset past now") {
		set(lfo::Param::Waveform, static_cast<float>(static_cast<int>(lfo::Waveform::Random)));
		set(lfo::Param::StereoPhase, static_cast<float>(M_PI));
		// THE SETTINGS THE RECORD HOLDS (the seed is whatever the device was given), decoded by the device's own rule.
		const auto& held = f.bridge.chainRecord().rowOf(card->device());
		lfo::Row row {};
		for (size_t i = 0; i < row.size(); ++i) {
			row[i] = held.field[i];
		}
		const lfo::Settings s = lfo::settingsOf(row, row.size());
		REQUIRE(s.wave == lfo::Waveform::Random);
		REQUIRE(s.stereoPhase == static_cast<float>(M_PI));
		// NO RUNNING DEVICE, so the left scope ends at the walk's start and the right one half a step past it.
		const auto want = lfo::randomTrace(s, 0, 0.5f);
		const auto right = card->curveRight();
		REQUIRE(right.size() == want.size());
		for (size_t i = 0; i < right.size(); ++i) {
			CAPTURE(i);
			CHECK(right[i].second == doctest::Approx(want[i].second).epsilon(1e-6));
		}
		CHECK(right != card->curve()); // NOT VACUOUS
		CHECK(pinkPixelsOf(*card, "lfo-walk") > 50);
	}
	SUBCASE("and back to 0: the pink line goes") {
		set(lfo::Param::StereoPhase, 1.0f);
		REQUIRE(pinkPixelsOf(*card, "lfo-on") > 50);
		set(lfo::Param::StereoPhase, 0.0f);
		CHECK(card->curveRight().empty());
		CHECK(pinkPixelsOf(*card, "lfo-off") == 0);
		CHECK(card->rightStrokesDrawnForTest() == 0);
	}
}
TEST_CASE("backlog PA: the MSEG's plot draws the right ear exactly while Stereo Phase is not 0 — the engine's walk at the left line's xs") {
	namespace mseg = feathers::mod::mseg;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	const size_t slot = f.bridge.addRackDevice(mseg::kTraits.tag);
	REQUIRE(slot >= 1);
	f.bridge.refresh();
	panel->resized();
	auto* card = const_cast<ui::MsegCard*>(dynamic_cast<const ui::MsegCard*>(panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	const auto set = [&](mseg::Param p, float v) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(p), v);
		f.bridge.refresh();
		card->updateContent();
	};
	// THE STEREO PHASE BOX IS ON THE CARD, in every mode — a hand can reach it.
	bool boxed = false;
	for (const mseg::Param p : ui::MsegCard::layout()) {
		boxed = boxed || p == mseg::Param::StereoPhase;
	}
	CHECK(boxed);

	CHECK(card->curveRight().empty());
	CHECK(pinkPixelsOf(*card, "mseg-mono") == 0);
	CHECK(card->rightStrokesDrawnForTest() == 0);

	for (const feathers::mod::Mode mode : { feathers::mod::Mode::Gate, feathers::mod::Mode::Loop, feathers::mod::Mode::Once, feathers::mod::Mode::Free }) {
		CAPTURE(static_cast<int>(mode));
		set(mseg::Param::Mode, static_cast<float>(static_cast<int>(mode)));
		set(mseg::Param::StereoPhase, static_cast<float>(M_PI / 2.0));
		// THE REFERENCE: the settings the record holds, through the geometry's walk (itself checked against the running
		// engine in test_mod_mseg), so the card is asked whether it draws THAT — and at the left line's xs.
		const auto& row = f.bridge.chainRecord().rowOf(card->device());
		mseg::Row r {};
		for (size_t i = 0; i < r.size(); ++i) {
			r[i] = row.field[i];
		}
		const mseg::Settings s = mseg::settingsOf(r, r.size());
		REQUIRE(s.stereoPhase == static_cast<float>(M_PI / 2.0));
		const auto want = mseg::polylineRight(s);
		const auto right = card->curveRight();
		REQUIRE(right.size() == want.size());
		REQUIRE(right.size() == card->curve().size());
		for (size_t i = 0; i < right.size(); ++i) {
			CAPTURE(i);
			CHECK(right[i] == want[i]);
		}
		CHECK(right != card->curve()); // NOT VACUOUS
		const std::string name = "mseg-mode" + std::to_string(static_cast<int>(mode));
		CHECK(pinkPixelsOf(*card, name.c_str()) > 50);
		CHECK(card->rightStrokesDrawnForTest() == right.size() - 1);
	}
	set(mseg::Param::StereoPhase, 0.0f);
	CHECK(card->curveRight().empty());
	CHECK(pinkPixelsOf(*card, "mseg-off") == 0);
	CHECK(card->rightStrokesDrawnForTest() == 0);
}
TEST_CASE("NO, + Chain: the Rack window offers '+ Chain' in Parallel, Select and Blend only — in Bands, Mid/Side and "
          "Left/Right it is not drawn and a press where it stood adds nothing") {
	namespace pr = feathers::rack::parallel;
	RackViews v;
	const visage::Bounds slot = v.panel->addBoundsForTest();
	REQUIRE(slot.width() > 0.0f); // Parallel, the default
	for (int m = 0; m < static_cast<int>(pr::Mode::kCount); ++m) {
		CAPTURE(m);
		v.set(pr::Param::Mode, static_cast<float>(m));
		v.refresh();
		const bool roleMode = (pr::kRoleModes & pr::kModeBit(static_cast<pr::Mode>(m))) != 0;
		CHECK((v.panel->addBoundsForTest().width() > 0.0f) == !roleMode);
		// A PRESS WHERE IT STANDS (or stood): a chain more exactly where it is offered, none where it is not.
		const size_t before = v.r.rack().chainCount();
		visage::MouseEvent e;
		// WHERE IT STANDS NOW (after the last strip: it moves with the rails), or where it stood in the first mode.
		const visage::Bounds at = v.panel->addBoundsForTest().width() > 0.0f ? v.panel->addBoundsForTest() : slot;
		e.position = { at.x() + at.width() * 0.5f, at.y() + at.height() * 0.5f };
		v.panel->onMouseDown().callback(e);
		CHECK(v.r.rack().chainCount() == before + (roleMode ? 0 : 1));
	}
}
TEST_CASE("NO item 2: a chain's level, mute and solo are destinations through its slot param — they light for an armed source and a drag routes there; past the 32nd chain they are not and say why; an undo puts the slot's base back with the body") {
	namespace pr = feathers::rack::parallel;
	// THE STATE AND THE HISTORY ABOVE THE FIXTURE (backlog PM): the lane's widgets watch them, so they must die after
	// every frame does — `frame_watchers.h`'s ordering rule, which ASan's `-UNDEBUG` build asserts.
	ui::ModState mod;
	ui::EditHistory history;
	ModFixture f;
	RackOnLane r(f);
	// THE HOLDER'S GATE, as the plugin hands it (`ParamBridge::outerGateOver`): the mirror writes through it, and the
	// plugin's service beat writes the mirror a new gate owes.
	r.rack().setOuterGate(ParamBridge::outerGateOver(&f.bridge, r.id));
	(void)f.bridge.serviceOwnControls();
	REQUIRE(r.rack().mirroredForTest());
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	REQUIRE(card != nullptr);
	const backplane::Source source = backplane::Source::core(0);
	mod.focus(source);
	mod.arm(true);
	card->setModulation(&mod, &history);
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 1);
	ui::ChainStrip& strip = *card->stripsForTest()[0];
	const uint8_t slot = r.rack().slotOf(0);
	REQUIRE(slot < pr::kChainSlots);
	const auto base = [&](pr::SlotControl c) { return f.deviceHost.fieldAt(r.id, pr::slotParam(slot, c)); };

	// EACH OF THE THREE LIGHTS for the armed source, and its amount drag lands on ITS slot param.
	const std::pair<ui::ChainControl*, pr::SlotControl> controls[] = {
		{ &strip.levelControlForTest(), pr::SlotControl::Level },
		{ &strip.muteControlForTest(), pr::SlotControl::Mute },
		{ &strip.soloControlForTest(), pr::SlotControl::Solo },
	};
	for (const auto& [control, which] : controls) {
		CAPTURE(static_cast<int>(which));
		const backplane::Destination dest = backplane::Destination::control(r.id, pr::slotParam(slot, which));
		CHECK(control->desc().modulatable);
		const feathers::device::ModDisplay before = control->display();
		CHECK(before.isDestination);
		CHECK(before.routable);
		const size_t steps = history.length();
		control->beginAmountGesture();
		control->setAmount(0.5f);
		control->endAmountGesture();
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(0.5f));
		CHECK(control->amountFor(source) == doctest::Approx(0.5f));
		CHECK(control->display().amount == doctest::Approx(0.5f)); // the glow's amount is the route's
		CHECK(history.length() == steps + 1);
		CHECK(std::string(ui::tooltipOf(control)) != pr::kNoSlotWhy);
	}

	// ONE TRUTH THROUGH AN UNDO: the hand's level is the body's, and the slot's base follows it both ways.
	ui::ChainControl& level = strip.levelControlForTest();
	const float unity = static_cast<float>(feathers::composite::kFaderUnity);
	CHECK(base(pr::SlotControl::Level) == unity);
	level.beginGesture();
	level.setValue(0.3);
	level.endGesture();
	CHECK(r.rack().level(0) == 0.3f);
	CHECK(base(pr::SlotControl::Level) == 0.3f);
	history.undo();
	CHECK(r.rack().level(0) == unity);
	CHECK(base(pr::SlotControl::Level) == unity);
	history.redo();
	CHECK(base(pr::SlotControl::Level) == 0.3f);
	strip.muteControlForTest().setValue(1.0);
	CHECK(base(pr::SlotControl::Mute) == 1.0f);
	history.undo();
	CHECK_FALSE(r.rack().muted(0));
	CHECK(base(pr::SlotControl::Mute) == 0.0f);

	// PAST THE 32ND CHAIN: no slot, so not a destination — dark under the armed source — and the tooltip says why.
	while (r.rack().chainCount() < pr::kChainSlots + 1) {
		REQUIRE(r.rack().addChain() != SIZE_MAX);
	}
	CHECK(r.rack().slotOf(pr::kChainSlots - 1) < pr::kChainSlots);
	REQUIRE(r.rack().slotOf(pr::kChainSlots) == pr::kNoChainSlot);
	const uint16_t past = r.rack().chainIdAt(pr::kChainSlots);
	for (const auto kind : { ui::ChainControl::Kind::Level, ui::ChainControl::Kind::Mute, ui::ChainControl::Kind::Solo }) {
		CAPTURE(static_cast<int>(kind));
		ui::ChainControl c(f.deviceHost, r.id, past, kind);
		c.setModulation(&mod, &history);
		CHECK_FALSE(c.desc().modulatable);
		CHECK_FALSE(c.display().isDestination);
		CHECK_FALSE(c.display().routable);
		CHECK(std::string(ui::tooltipOf(&c)) == pr::kNoSlotWhy);
		const size_t steps = history.length();
		c.setAmount(0.5f); // refused: there is nowhere for it to go
		CHECK(c.amountFor(source) == 0.0f);
		CHECK(history.length() == steps);
	}
	// AND THE HAND STILL MOVES IT: only modulation is unavailable there.
	ui::ChainControl pastLevel(f.deviceHost, r.id, past, ui::ChainControl::Kind::Level);
	pastLevel.setValue(0.2);
	CHECK(r.rack().level(pr::kChainSlots) == 0.2f);
	card->setModulation(nullptr, nullptr);
	f.bridge.fxBodies().detach(r.id, r.body);
	REQUIRE(f.bridge.removeFxDevice(r.slot));
}
TEST_CASE("a chain strip's key box shows Any or the note name, edits the chain's key as one undo step, and is no destination") {
	namespace pr = feathers::rack::parallel;
	ui::ModState mod;
	ui::EditHistory history;
	ModFixture f;
	RackOnLane r(f);
	r.rack().setOuterGate(ParamBridge::outerGateOver(&f.bridge, r.id));
	(void)f.bridge.serviceOwnControls();
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	lane->resized();
	auto* card = dynamic_cast<ui::ParallelRackCard*>(lane->cardAt(r.slot));
	REQUIRE(card != nullptr);
	card->setModulation(&mod, &history);
	(void)card->update();
	REQUIRE(card->stripsForTest().size() == 1);
	ui::ChainStrip& strip = *card->stripsForTest()[0];
	ui::ChainControl& key = strip.keyControlForTest();
	// DEFAULT ANY: the control's value is 0 and the box's text says so.
	CHECK(r.rack().chainKey(0) == pr::kAnyKey);
	CHECK(key.value() == 0.0);
	CHECK(key.text() == "Any");
	// WHERE IT SHOWS: a wide row has it; a narrow card strip leaves the room to the level pill's caption; the window's column has it.
	strip.setBounds(0.0f, 0.0f, 600.0f, 20.0f);
	CHECK(strip.keyBox().isVisible());
	strip.setBounds(0.0f, 0.0f, 150.0f, 20.0f);
	CHECK_FALSE(strip.keyBox().isVisible());
	strip.setInRail(true);
	strip.setBounds(0.0f, 0.0f, 40.0f, 160.0f);
	CHECK(strip.columnLayoutForTest());
	CHECK(strip.keyBox().isVisible());
	// 61 is C#4 (a key plus one is the Choice's value).
	key.setValue(62.0);
	CHECK(r.rack().chainKey(0) == 61);
	CHECK(key.text() == "C#4");
	CHECK(key.value() == 62.0);
	history.undo();
	CHECK(r.rack().chainKey(0) == pr::kAnyKey);
	CHECK(key.text() == "Any");
	history.redo();
	CHECK(r.rack().chainKey(0) == 61);
	// THE CHOKE GROUP BESIDE IT: None, then 1..8; one undo step; the column has the box, a narrow card row does not.
	ui::ChainControl& choke = strip.chokeControlForTest();
	CHECK(choke.text() == "None");
	choke.setValue(3.0);
	CHECK(r.rack().chainChoke(0) == 3);
	CHECK(choke.text() == "3");
	history.undo();
	CHECK(r.rack().chainChoke(0) == 0);
	history.redo();
	CHECK(r.rack().chainChoke(0) == 3);
	CHECK(strip.chokeBox().isVisible());
	CHECK_FALSE(choke.display().isDestination);
	// NOT A DESTINATION: it has no slot param, so nothing can be routed to it and it offers no host slot.
	CHECK_FALSE(key.display().isDestination);
	CHECK(key.hostSlot().state == feathers::device::HostSlotStatus::State::Unavailable);
	card->setModulation(nullptr, nullptr);
}
TEST_CASE("the Arp is offered by Feathers' modulator-lane picker, and a pick seats it with its own card") {
	// 2026-10-30: the Arp showed in the rack but not in this picker. The picker offers what the rack can hold AND
	// what calls itself a modulator (`isTaggedModulator`); the browser's category test could not see that, because the
	// offer is the TAG's, not the category's.
	namespace ap = feathers::mod::arp;
	ModFixture f;
	const std::vector<const feathers::device::Descriptor*> offered = f.panel->addableForTest();
	size_t index = offered.size();
	for (size_t i = 0; i < offered.size(); ++i) {
		if (feathers::device::sameTag(offered[i]->traits.tag, ap::kTraits.tag)) {
			index = i;
		}
	}
	REQUIRE_MESSAGE(index < offered.size(), "the picker does not offer the Arp");
	// THE PICKER'S OWN ROAD (`addChoiceForTest` → `ModPanel::addDevice`), not the bridge's.
	const size_t before = f.panel->deviceCount();
	const size_t slot = f.panel->addChoiceForTest(index);
	// AT THE FRONT while "Arps sent to front of chain" is on (its default), at the end otherwise (`emitters_front.h`).
	REQUIRE(slot == (ui::emittersToFront() ? 0 : before));
	const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
	REQUIRE(id != kNoInstance);
	const feathers::device::Descriptor* seated = f.deviceHost.descriptorOf(id);
	REQUIRE(seated != nullptr);
	CHECK(feathers::device::sameTag(seated->traits.tag, ap::kTraits.tag)); // the registry's copy, so by tag
	CHECK(dynamic_cast<const ui::ArpCard*>(f.panel->cardFrameForTest(slot)) != nullptr);
}
TEST_CASE("arps sent to front of chain: in Feathers' modulator lane a picked arp lands first while on and last while off; "
          "an Add before keeps its place; a device that emits no notes appends either way") {
	// 2026-10-30. Through the picker's own roads (`addChoiceForTest`, `insertChoiceForTest`), read by value: the
	// strip's device at each slot.
	namespace ap = feathers::mod::arp;
	struct Reset {
		bool before = ui::emittersToFront();
		~Reset() { (void)ui::setEmittersToFront(before); }
	} reset;
	ModFixture f;
	const std::vector<const feathers::device::Descriptor*> offered = f.panel->addableForTest();
	const auto indexOf = [&](const char* tag) {
		for (size_t i = 0; i < offered.size(); ++i) {
			if (feathers::device::sameTag(offered[i]->traits.tag, tag)) {
				return i;
			}
		}
		return offered.size();
	};
	const size_t arp = indexOf(ap::kTraits.tag);
	const size_t lfo = indexOf(feathers::mod::lfo::kTraits.tag);
	REQUIRE(arp < offered.size());
	REQUIRE(lfo < offered.size());
	const auto tagAt = [&](size_t slot) {
		const DeviceInstanceId id = f.bridge.rackDeviceAt(slot);
		const feathers::device::Descriptor* d = f.deviceHost.descriptorOf(id);
		return d != nullptr ? std::string(d->traits.tag) : std::string();
	};
	const size_t before = f.panel->deviceCount(); // the strip's default modulators
	REQUIRE(before > 0);

	(void)ui::setEmittersToFront(true);
	CHECK(f.panel->addChoiceForTest(arp) == 0);
	CHECK(tagAt(0) == ap::kTraits.tag);
	CHECK(f.panel->deviceCount() == before + 1);
	// NOT AN EMITTER: the LFO appends.
	CHECK(f.panel->addChoiceForTest(lfo) == before + 1);
	// "ADD BEFORE…" slot 2 names its place.
	CHECK(f.panel->insertChoiceForTest(2, arp) == 2);
	CHECK(tagAt(2) == ap::kTraits.tag);

	(void)ui::setEmittersToFront(false);
	const size_t end = f.panel->deviceCount();
	CHECK(f.panel->addChoiceForTest(arp) == end);
	CHECK(tagAt(end) == ap::kTraits.tag);
}

namespace {
/// A LEFT PRESS AND RELEASE AT A FRAME'S CENTRE, delivered to the frame — a single click.
void clickCentre(visage::Frame& f) {
	visage::MouseEvent e;
	e.position = { f.width() * 0.5f, f.height() * 0.5f };
	e.event_frame = &f;
	e.button_id = visage::kMouseButtonLeft;
	e.button_state = visage::kMouseButtonLeft;
	e.is_down = true;
	e.repeat_click_count = 1;
	f.processMouseDown(e);
	e.button_state = visage::kMouseButtonNone;
	e.is_down = false;
	f.processMouseUp(e);
}
/// HOW MANY POPUPS ARE OPEN OVER `f` — a choice menu opened from a box is parented on its top frame.
size_t popupsOver(visage::Frame& f) {
	size_t n = 0;
	for (visage::Frame* c : f.topParentFrame()->children()) {
		if (dynamic_cast<visage::PopupMenuFrame*>(c) != nullptr && c->isVisible()) {
			++n;
		}
	}
	return n;
}
void valueBoxesIn(visage::Frame* f, std::vector<ui::ValueBox*>& out) {
	for (visage::Frame* c : f->children()) {
		if (auto* box = dynamic_cast<ui::ValueBox*>(c); box != nullptr) {
			out.push_back(box);
		}
		valueBoxesIn(c, out);
	}
}
} // namespace

TEST_CASE("an armed source and a press on a dropdown ASSIGNS: the Arp's Time Base takes the route and opens no menu; "
          "disarmed, the same press opens the menu") {
	// Time Base is a modulation destination: the box glowed when armed, but a click to assign only opened the
	// context menu. Read by value: the route's amount, and the popups open.
	namespace ap = feathers::mod::arp;
	ModFixture f;
	const size_t slot = f.bridge.addRackDevice(ap::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	f.bridge.refresh();
	f.panel->resized();
	auto* card = const_cast<ui::ArpCard*>(dynamic_cast<const ui::ArpCard*>(f.panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);
	ui::ValueBox& box = card->timeBase();
	REQUIRE(box.choiceMenuAvailable());
	const backplane::Destination dest = card->control(ap::at(ap::Param::TimeBase)).destination();
	const backplane::Source source = backplane::Source::core(0); // a macro: it may reach the strip
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	SUBCASE("armed: the glow, and the press makes the route") {
		mod->focus(source);
		mod->arm(true);
		REQUIRE(f.bridge.mayRoute(source, dest));
		CHECK(box.litForTest());
		const size_t popups = popupsOver(box);
		CHECK(f.bridge.routeAmount(source, dest) == 0.0f);
		clickCentre(box);
		CHECK(f.bridge.routeAmount(source, dest) != 0.0f); // the route is made, as a knob's press makes it
		CHECK(popupsOver(box) == popups);                  // and no menu opened
	}
	SUBCASE("the control: disarmed, no glow, and the press opens the menu and makes no route") {
		mod->arm(false);
		CHECK_FALSE(box.litForTest());
		const size_t popups = popupsOver(box);
		clickCentre(box);
		CHECK(popupsOver(box) == popups + 1);
		CHECK(f.bridge.routeAmount(source, dest) == 0.0f);
	}
}
TEST_CASE("every dropdown that glows for an armed source takes the assign — on every modulator card and every fx card") {
	// THE GLOW AND THE PRESS AGREE (`ValueBox::lit`, `mappable`): a list that glows under an armed macro, pressed, makes
	// the route and opens no menu. Every device the strip or the fx lane offers, every box on its card that opens a list.
	ModFixture f;
	const backplane::Source source = backplane::Source::core(0);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	ui::FxLane* lane = f.gui.fxLaneForTest();
	REQUIRE(lane != nullptr);
	size_t lists = 0;
	size_t glowing = 0;
	const auto sweep = [&](visage::Frame* card, const std::string& tag) {
		std::vector<ui::ValueBox*> boxes;
		valueBoxesIn(card, boxes);
		for (ui::ValueBox* box : boxes) {
			if (!box->choiceMenuAvailable() || !box->isVisible()) {
				continue;
			}
			++lists;
			auto* road = dynamic_cast<ui::DeviceControl*>(&box->control());
			mod->focus(source);
			mod->arm(true);
			if (!box->litForTest()) {
				// NOT GLOWING: then a press must not assign either — it is the menu's.
				const bool couldAssign = road != nullptr && f.bridge.mayRoute(source, road->destination()) && box->enabled();
				CHECK_FALSE(couldAssign);
				continue;
			}
			++glowing;
			CAPTURE(tag);
			CAPTURE(box->label());
			REQUIRE(road != nullptr);
			const float before = f.bridge.routeAmount(source, road->destination());
			const size_t popups = popupsOver(*box);
			clickCentre(*box);
			CHECK(f.bridge.routeAmount(source, road->destination()) != before);
			CHECK(popupsOver(*box) == popups);
		}
		mod->arm(false);
	};
	std::set<std::string> onStrip; // backlog FY: the lane half skips exactly these, by name
	size_t devicesSwept = 0;
	for (const feathers::device::Descriptor* d : f.panel->addableForTest()) {
		const size_t slot = f.bridge.addRackDevice(d->traits.tag);
		REQUIRE(slot < kMaxRackSlots);
		f.bridge.refresh();
		f.panel->resized();
		sweep(const_cast<visage::Frame*>(f.panel->cardFrameForTest(slot)), d->traits.tag);
		onStrip.insert(d->traits.tag);
		++devicesSwept;
		REQUIRE(f.bridge.removeRackDevice(slot)); // the strip is four long: one at a time
		f.bridge.refresh();
		f.panel->resized();
	}
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		// **SKIP WHAT THE STRIP SWEPT, NOT EVERY DEVICE WITH A MOD HALF** (backlog FY, 2026-10-30): `d.mod != nullptr`
		// skipped a device that has a modulator half but no modulator TAG, which is in neither list, so it was
		// never swept anywhere.
		if (!feathers::maySitInLane(d) || onStrip.count(d.traits.tag) > 0) {
			continue; // the modulators were swept on the strip
		}
		const size_t slot = f.bridge.addFxDevice(d.traits.tag);
		REQUIRE(slot < kMaxFxSlots);
		f.bridge.refresh();
		lane->resized();
		sweep(lane->cardAt(slot), d.traits.tag);
		++devicesSwept;
		REQUIRE(f.bridge.removeFxDevice(slot));
		f.bridge.refresh();
		lane->resized();
	}
	MESSAGE("dropdowns: " << lists << ", glowing under an armed macro: " << glowing);
	CHECK(glowing > 0); // not vacuous: the Arp's Time Base and Order are among them
	// AND EVERY DEVICE EITHER HOST OFFERS WAS SWEPT, counted against the registry rather than the loop.
	size_t offered = 0;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		offered += (onStrip.count(d.traits.tag) > 0 || feathers::maySitInLane(d)) ? 1 : 0;
	}
	CHECK(devicesSwept == offered);
	CHECK(onStrip.size() == f.panel->addableForTest().size());
}
TEST_CASE("NO item 2 follow-up: undoing a chain's removal puts its routes back into the slot it takes, by value, and a redo prunes them again") {
	namespace pr = feathers::rack::parallel;
	MirroredRack m;
	m.panel->addChainForTest();
	REQUIRE(m.r.rack().chainCount() == 2);
	const uint16_t chain = m.r.rack().chainIdAt(1);
	const uint8_t slot = m.r.rack().slotOf(1);
	const backplane::Source a = backplane::Source::core(0);
	const backplane::Source b = backplane::Source::core(1);
	m.f.bridge.setRouteAmount(a, m.dest(slot, pr::SlotControl::Level), 0.5f);
	m.f.bridge.setRouteAmount(b, m.dest(slot, pr::SlotControl::Mute), 1.0f);
	m.f.bridge.setRouteAmount(a, m.dest(slot, pr::SlotControl::Solo), -0.25f);

	m.panel->removeChainForTest(chain);
	REQUIRE(m.r.rack().chainCount() == 1);
	(void)m.f.bridge.serviceOwnControls(); // the prune
	CHECK(m.f.bridge.routeAmount(a, m.dest(slot, pr::SlotControl::Level)) == 0.0f);
	CHECK(m.f.bridge.routeAmount(b, m.dest(slot, pr::SlotControl::Mute)) == 0.0f);

	// UNDO: the chain and its three routes, into the slot it took (its own: the prune has run, so it is free).
	m.history.undo();
	REQUIRE(m.r.rack().chainCount() == 2);
	const size_t at = m.r.rack().indexOfChain(chain);
	REQUIRE(at != SIZE_MAX);
	const uint8_t back = m.r.rack().slotOf(at);
	CHECK(back == slot);
	CHECK(m.f.bridge.routeAmount(a, m.dest(back, pr::SlotControl::Level)) == 0.5f);
	CHECK(m.f.bridge.routeAmount(b, m.dest(back, pr::SlotControl::Mute)) == 1.0f);
	CHECK(m.f.bridge.routeAmount(a, m.dest(back, pr::SlotControl::Solo)) == -0.25f);

	// REDO: removed again, and the host's next beat prunes them again.
	m.history.redo();
	REQUIRE(m.r.rack().chainCount() == 1);
	(void)m.f.bridge.serviceOwnControls();
	CHECK(m.f.bridge.routeAmount(a, m.dest(slot, pr::SlotControl::Level)) == 0.0f);
	CHECK(m.f.bridge.routeAmount(a, m.dest(slot, pr::SlotControl::Solo)) == 0.0f);

	// AND AN UNDO BEFORE THE PRUNE: the chain's own slot still awaits it, so it takes another — and its routes follow.
	m.history.undo();
	m.history.redo();
	m.history.undo(); // removed and put back with no service between
	const size_t again = m.r.rack().indexOfChain(chain);
	REQUIRE(again != SIZE_MAX);
	const uint8_t other = m.r.rack().slotOf(again);
	CHECK(other != slot);
	CHECK(m.f.bridge.routeAmount(a, m.dest(other, pr::SlotControl::Level)) == 0.5f);
	CHECK(m.f.bridge.routeAmount(b, m.dest(other, pr::SlotControl::Mute)) == 1.0f);
	(void)m.f.bridge.serviceOwnControls(); // the old slot's copies go; the chain's stay
	CHECK(m.f.bridge.routeAmount(a, m.dest(slot, pr::SlotControl::Level)) == 0.0f);
	CHECK(m.f.bridge.routeAmount(a, m.dest(other, pr::SlotControl::Level)) == 0.5f);
}

