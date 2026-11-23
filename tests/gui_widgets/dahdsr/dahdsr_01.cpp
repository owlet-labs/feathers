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

// PIECE 1 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

TEST_CASE("the envelope card puts A D S R on the top row and the rest out of the way") {
	// A, D, S and R sit on the top row; the other stages have defaults such that a player who only
	// wants an ADSR never needs to touch them.
	//
	// The engine half of that requirement — that an untouched envelope BEHAVES as an ADSR — is pinned
	// in test_mod_dahdsr.cpp. This is the other half: that the card SHOWS one. Both are needed and
	// neither implies the other; a card could show A D S R over an envelope with a hold in it.
	namespace dsr = feathers::mod::dahdsr;
	const auto& layout = ui::EnvCard::layout();

	REQUIRE(layout.size() == ui::EnvCard::kBoxCount);
	REQUIRE(ui::EnvCard::kTopRow == 4);

	CHECK(layout[0] == dsr::Param::Attack);
	CHECK(layout[1] == dsr::Param::Decay);
	CHECK(layout[2] == dsr::Param::Sustain);
	CHECK(layout[3] == dsr::Param::Release);
	// And the second row is exactly what an ADSR does not have, plus the smoothing time.
	CHECK(layout[4] == dsr::Param::Delay);
	CHECK(layout[5] == dsr::Param::Hold);
	CHECK(layout[6] == dsr::Param::Smoothing);
	// AND THE AMOUNT (backlog DH), last on the second row — the row had room for a fourth box.
	REQUIRE(ui::EnvCard::kBottomRow == 4);
	CHECK(layout[7] == dsr::Param::Amount);

	// EVERY ONE OF THE SECOND ROW IS OUT OF THE WAY AT ITS DEFAULT, which is what makes the first row
	// the only row a player has to meet.
	const dsr::Settings defaults;
	CHECK(dsr::stageSeconds(defaults.delay) == 0.0);
	CHECK(dsr::stageSeconds(defaults.hold) == 0.0);
	// SMOOTHING TOO, and it is out of the way by being at the bottom of its own travel rather than by
	// a switch beside it — the switch is deleted, so this is the same claim about
	// one control instead of two.
	CHECK(defaults.smoothing == feathers::mod::kSmoothingLog2SecondsMin);
}
TEST_CASE("the card's boxes tile their rows without overlapping or leaving the card") {
	// GEOMETRY IS RENDERED BEFORE IT IS BELIEVED — the project's rule, applied to a layout whose whole
	// selling point is that seven controls fit. Two rows with different column counts is exactly where
	// a shared width constant leaves one row short of the card's edge.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	CHECK(ui::EnvCard::height() <= 320.0f); // the design doc's hard cap on a card

	for (size_t row = 0; row < 2; ++row) {
		const size_t first = row == 0 ? 0 : ui::EnvCard::kTopRow;
		const size_t count = row == 0 ? ui::EnvCard::kTopRow : ui::EnvCard::kBottomRow;
		float previousRight = -1.0f;
		for (size_t i = first; i < first + count; ++i) {
			CAPTURE(i);
			const visage::Bounds b = card.boxBounds(i);
			CHECK(b.width() > 0.0f);
			CHECK(b.x() >= ui::EnvCard::kPadX - 0.01f);
			CHECK(b.right() <= card.width() - ui::EnvCard::kPadX + 0.01f);
			// No overlap, and in order.
			CHECK(b.x() > previousRight);
			previousRight = b.right();
			// The rows are rows: every box in one shares a top edge.
			CHECK(b.y() == doctest::Approx(card.boxBounds(first).y()));
		}
		// The row reaches the card's far padding — no short row.
		CHECK(previousRight == doctest::Approx(card.width() - ui::EnvCard::kPadX).epsilon(0.01));
	}

	// The two rows do not collide with each other or with the curve above them.
	CHECK(card.boxBounds(ui::EnvCard::kTopRow).y() >= card.boxBounds(0).bottom());
	CHECK(card.boxBounds(0).y() >= card.curveBounds().bottom());
}
TEST_CASE("the drawn curve is the envelope that plays, and follows its controls") {
	// "THE CURVE IS THE ENVELOPE THAT PLAYS" is true BY CONSTRUCTION here — the card runs the real
	// envelope rather than a second closed form, which is what the filter's card needed a case to hold
	// together. So what is worth asserting is the other half: that the PICTURE follows the CONTROLS,
	// which is a claim about the x axis rather than about the DSP.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	namespace dsr = feathers::mod::dahdsr;

	const std::vector<visage::Point> shape = card.curvePoints();
	REQUIRE(shape.size() > 8);
	// It starts at the floor and runs left to right without going backwards.
	CHECK(shape.front().x == doctest::Approx(0.0f));
	CHECK(shape.front().y == doctest::Approx(0.0f));
	for (size_t i = 1; i < shape.size(); ++i) {
		CAPTURE(i);
		CHECK(shape[i].x >= shape[i - 1].x - 1e-5f);
		CHECK(shape[i].y >= -0.01f);
		CHECK(shape[i].y <= 1.01f);
	}
	// It reaches the top, because the default envelope has a real attack.
	float peak = 0.0f;
	for (const visage::Point& p : shape) {
		peak = std::max(peak, p.y);
	}
	CHECK(peak > 0.95f);

	SUBCASE("a longer attack widens the attack segment") {
		// THE AXIS CLAIM, AS A NUMBER. A stage's width is its control's position in its own range, so
		// lengthening the attack must move the boundary between attack and what follows it — and a
		// picture that ignored the control would leave it exactly where it was.
		const float before = card.stageEndX(dsr::Stage::Attack);

		dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
		s.attack = dsr::stageValueFor(5.0); // five seconds, against the default ten milliseconds
		f.bridge.setEnvelopeSettings(0, s);

		const float after = card.stageEndX(dsr::Stage::Attack);
		CAPTURE(before);
		CAPTURE(after);
		CHECK(after > before + 0.05f);
	}

	SUBCASE("and a stage at the bottom of its travel takes no width at all") {
		// The convention device.h states, seen from the card: a time at its minimum is a stage that
		// does not happen, so it gets no segment rather than a sliver.
		dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
		s.hold = dsr::stageValueFor(2.0);
		f.bridge.setEnvelopeSettings(0, s);
		const float withHold = card.stageEndX(dsr::Stage::Hold) - card.stageEndX(dsr::Stage::Attack);
		CHECK(withHold > 0.05f);

		s.hold = dsr::kStageLog2SecondsMin;
		f.bridge.setEnvelopeSettings(0, s);
		const float without = card.stageEndX(dsr::Stage::Hold) - card.stageEndX(dsr::Stage::Attack);
		CHECK(without == doctest::Approx(0.0f).epsilon(1e-4));
	}
}
TEST_CASE("a value box prints its control's declared unit, and says off at the bottom") {
	// THE READOUT IS THE THING A VALUE BOX EXISTS FOR — it is the whole reason it beats a dial for a
	// time — so it is asserted as TEXT rather than inferred from pixels.
	namespace dsr = feathers::mod::dahdsr;
	const auto& attack = dsr::kParamDescs[static_cast<size_t>(dsr::Param::Attack)];
	const auto& sustain = dsr::kParamDescs[static_cast<size_t>(dsr::Param::Sustain)];
	const auto& curve = dsr::kParamDescs[static_cast<size_t>(dsr::Param::DecayShape)];

	// Ten milliseconds, which is the default and the number Bitwig prints beside its own attack.
	CHECK(ui::formatValue(attack, dsr::stageValueFor(0.010)) == "10ms");
	CHECK(ui::formatValue(attack, dsr::stageValueFor(0.841)) == "841ms");
	CHECK(ui::formatValue(attack, dsr::stageValueFor(0.0012)) == "1.2ms");
	// AT THE BOTTOM OF A LogSeconds RANGE, "off" — the convention device.h states, because there is no
	// zero in a logarithmic domain and "0.5ms" would promise a stage the device is about to skip.
	CHECK(ui::formatValue(attack, attack.min) == "off");

	// A percentage is unsigned when its range is, and signed when the sign is the whole message.
	//
	// WHOLE PERCENT, NOT A DECIMAL. Bitwig prints "53.5 %" beside its own sustain and has the width
	// for it; the top row here gives each of A D S R 46px, and a percent is not a quantity anyone
	// aims at to a tenth. So 0.535 reads as 54 — rounded, not truncated, which is the half of this a
	// case should pin because truncation would look identical at most values.
	CHECK(ui::formatValue(sustain, 0.53) == "53%");
	CHECK(ui::formatValue(sustain, 0.535) == "54%");
	CHECK(ui::formatValue(curve, 0.42) == "+42%");
	CHECK(ui::formatValue(curve, -0.42) == "-42%");
}
TEST_CASE("every live stage of the drawn curve actually carries its own shape") {
	// THE CASE THE OTHER GEOMETRY CASES COULD NOT BE, and it exists because a screenshot found what
	// they all missed.
	//
	// The curve is built by running the real envelope and filing each sample under a stage. A first
	// version read the stage back AFTER the tick, which is a different question — where the envelope
	// ENDED UP. On the first tick it is still Idle, so the rate multiplier came out 1, a 10 ms attack
	// finished inside that single tick, and its one sample was filed under the stage it had already
	// moved into. THE ATTACK CONTRIBUTED NO POINTS AT ALL and the polyline ran straight from the
	// origin to the first decay sample.
	//
	// Every existing case passed: the segments were in order, the widths followed their controls, the
	// shape was monotone and reached the top. All true of a curve with a whole segment missing. What
	// separates them is asking whether each segment carries a SHAPE rather than merely existing —
	// which for the attack means it rises ACROSS its own width instead of jumping at the edge.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));
	namespace dsr = feathers::mod::dahdsr;

	// THE DEFAULT ENVELOPE FIRST, AND IT IS THE DEFAULT THAT MATTERS. A first draft of this case set a
	// delay before checking anything, which quietly moved it off the bug: with a delay, the first tick
	// is a delay tick and the envelope is in a real stage by the time the attack runs. It is the
	// no-delay shape — which is the SHIPPING default, an ADSR — where the very first tick is the
	// attack and the stage read back afterwards is still Idle. The known-bad passed twice against the
	// version of this case that had a delay in it.
	SUBCASE("the shipping default, whose first tick IS the attack") {
		const std::vector<visage::Point> defaults = card.curvePoints();
		const float attackEndDefault = card.stageEndX(dsr::Stage::Attack);
		std::vector<float> ys;
		for (const visage::Point& p : defaults) {
			if (p.x > 1e-4f && p.x < attackEndDefault - 1e-4f) {
				ys.push_back(p.y);
			}
		}
		CAPTURE(attackEndDefault);
		CAPTURE(ys.size());
		REQUIRE(ys.size() >= 3);
		// It RISES ACROSS its own width. A segment drawn flat at the top — which is what a lost
		// attack looks like — has interior points, and they are all 1.
		CHECK(*std::max_element(ys.begin(), ys.end()) - *std::min_element(ys.begin(), ys.end())
		      > 0.5f);
	}

	// And then a shape with every stage live, so no segment is legitimately empty.
	dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
	s.delay = dsr::stageValueFor(0.05);
	s.attack = dsr::stageValueFor(0.01);
	s.hold = dsr::stageValueFor(0.05);
	s.decay = dsr::stageValueFor(0.5);
	s.sustain = 0.5f;
	s.release = dsr::stageValueFor(0.25);
	f.bridge.setEnvelopeSettings(0, s);

	const std::vector<visage::Point> shape = card.curvePoints();
	REQUIRE(shape.size() > 8);

	// Points strictly inside a segment, so an endpoint shared with a neighbour cannot stand in for
	// the segment having any of its own.
	const auto inside = [&shape](float from, float to) {
		std::vector<float> ys;
		for (const visage::Point& p : shape) {
			if (p.x > from + 1e-4f && p.x < to - 1e-4f) {
				ys.push_back(p.y);
			}
		}
		return ys;
	};

	const float delayEnd = card.stageEndX(dsr::Stage::Delay);
	const float attackEnd = card.stageEndX(dsr::Stage::Attack);
	const float holdEnd = card.stageEndX(dsr::Stage::Hold);
	const float decayEnd = card.stageEndX(dsr::Stage::Decay);
	const float sustainEnd = card.stageEndX(dsr::Stage::Sustain);

	// EVERY LIVE SEGMENT HAS INTERIOR POINTS. This alone is what the missing attack failed.
	CHECK(inside(0.0f, delayEnd).size() >= 3);
	CHECK(inside(delayEnd, attackEnd).size() >= 3);
	CHECK(inside(attackEnd, holdEnd).size() >= 3);
	CHECK(inside(holdEnd, decayEnd).size() >= 3);
	CHECK(inside(sustainEnd, 1.0f).size() >= 3);

	// AND THE SHAPED ONES CARRY A SHAPE — a rise or a fall ACROSS the segment rather than a jump at
	// its edge, which is the difference between drawing a stage and merely reserving room for it.
	const std::vector<float> attackYs = inside(delayEnd, attackEnd);
	REQUIRE(attackYs.size() >= 3);
	const float attackLow = *std::min_element(attackYs.begin(), attackYs.end());
	const float attackHigh = *std::max_element(attackYs.begin(), attackYs.end());
	CAPTURE(attackLow);
	CAPTURE(attackHigh);
	CHECK(attackHigh - attackLow > 0.5f);

	const std::vector<float> decayYs = inside(holdEnd, decayEnd);
	REQUIRE(decayYs.size() >= 3);
	CHECK(*std::max_element(decayYs.begin(), decayYs.end())
	          - *std::min_element(decayYs.begin(), decayYs.end())
	      > 0.2f);

	const std::vector<float> releaseYs = inside(sustainEnd, 1.0f);
	REQUIRE(releaseYs.size() >= 3);
	CHECK(*std::max_element(releaseYs.begin(), releaseYs.end())
	          - *std::min_element(releaseYs.begin(), releaseYs.end())
	      > 0.2f);

	// The flat ones are flat, which is the other half: a hold that sloped would mean the samples were
	// coming from the wrong stage in the other direction.
	const std::vector<float> holdYs = inside(attackEnd, holdEnd);
	REQUIRE(holdYs.size() >= 3);
	CHECK(*std::max_element(holdYs.begin(), holdYs.end())
	          - *std::min_element(holdYs.begin(), holdYs.end())
	      < 0.05f);

	// And it ends on the floor, because a release that stopped short would be a picture of an
	// envelope that never lets go.
	CHECK(shape.back().x == doctest::Approx(1.0f));
	CHECK(shape.back().y < 0.05f);
}
TEST_CASE("an envelope can be armed from its card and pointed at a knob") {
	// THE WHOLE POINT OF A MODULATOR, END TO END, through the real widgets at the real places: press
	// the card's arm arrow, watch the destinations light, drag one, and find a route from the
	// ENVELOPE — not from a macro — reaching the engine.
	//
	// It is written against the ends rather than against a macro's road because the two are the same
	// road: ModState holds one focused SOURCE and the knobs draw amounts for whichever it is. The
	// risk this case answers is that "whichever it is" was only ever true of the four it started
	// with.
	ModFixture f;
	const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);

	// Nothing armed, nothing lit — the state a card is in before anyone points it.
	ui::Knob* path = f.knobFor(ParamId::Path);
	REQUIRE(path != nullptr);
	REQUIRE(path->isModDestination());
	CHECK_FALSE(path->mappable());

	// THE CARD'S OWN ARROW, pressed where it is drawn — the fixture's arm() drives the real button
	// through the real tree rather than calling into ModState, which is the only way to find out
	// whether the arrow is reachable at all.
	REQUIRE(f.panel->arrowBounds(envelope).width() > 0.0f);
	f.arm(envelope);

	// THE ENVELOPE IS THE ARMED SOURCE NOW, read off the DESTINATION rather than off the flag: what
	// matters is that a knob would take a route from it, which is the thing a hand can see.
	CHECK(path->mappable());

	// AND A DRAG WRITES AN AMOUNT FROM THE ENVELOPE. The value must not move — that is the whole
	// meaning of mapping mode, and a drag that changed both would be the arm doing nothing.
	const double valueBefore = f.bridge.value(static_cast<clap_id>(ParamId::Path));
	const visage::Point knobAt = f.faceCentre(path);
	dragThroughRoot(f.root, knobAt, knobAt - visage::Point { 0.0f, 40.0f });

	CHECK(f.bridge.value(static_cast<clap_id>(ParamId::Path)) == doctest::Approx(valueBefore));
	const int destination = feathers::modDestinationIndexOf(ParamId::Path);
	REQUIRE(destination >= 0);
	const backplane::Destination pathDest = backplane::Destination::core(static_cast<size_t>(destination));
	const float amount = f.bridge.routeAmount(envelope, pathDest);
	CAPTURE(amount);
	CHECK(amount > 0.0f);
	// AND NO MACRO PICKED IT UP, which is what says the route went where the arrow pointed rather
	// than to whatever was focused before.
	for (size_t macro = 0; macro < kMacroCount; ++macro) {
		CAPTURE(macro);
		CHECK(f.bridge.routeAmount(backplane::Source::core(macro), pathDest) == 0.0f);
	}

	SUBCASE("and the route list names the envelope and what it reaches") {
		// THE NAMES ARE THE HALF A HAND ACTUALLY READS, and both of them used to be wrong for this
		// source: modSourceName answered "Source" for an envelope, and the notification road indexed a
		// FOUR-ENTRY macro table with a source of twelve.
		// NUMBERED EVEN THOUGH ONLY ONE IS ON SCREEN, and that is deliberate rather than an accident
		// of the pool size. All four slots have destinations in the route table today — a patch can
		// carry a routing to slot 3 whether or not the editor is showing it — so an unnumbered name
		// would be ambiguous NOW, not merely later. It is also stable: a name that changed from
		// "DAHDSR" to "DAHDSR 1" the moment a second device appeared would rewrite every route list a
		// player had already learned to read.
		CHECK(ui::modSourceName(f.bridge, envelope) == "DAHDSR 1");
		CHECK(ui::modDestinationName(f.bridge, pathDest) == std::string("Path"));
	}

	SUBCASE("and an envelope may be pointed at another device control, which is the unified surface") {
		// A MODULATOR REACHING A MODULATOR. The destination is not a parameter at all, so this is the
		// road that did not exist before the rack's controls joined the table.
		//
		// **IT NOW NEEDS A SECOND MODULATOR, AND THAT IS THE CASE'S OWN TITLE BEING TAKEN
		// LITERALLY.** It said "ANOTHER device control" and reached slot 0's own Attack — slot 0 was
		// standing in for both ends because, with one device in the rack, that was the only device
		// there was. The routing rule (2026-09-05: sources must be upstream of destinations) and the
		// rule that a node's bus never carries its own device's output (2026-09-06) make that
		// arrangement illegal, so the fixture grows the device the title always implied.
		//
		// **A SECOND DEVICE RATHER THAN A WEAKER ASSERTION**: the claim is that one modulator can
		// reach another, and proving it needs two. Arming a macro here would have kept the case green
		// and stopped it saying anything about modulators at all.
		REQUIRE(f.bridge.addRackDevice(feathers::mod::dahdsr::kTraits.tag) == 1);
		f.bridge.refresh();
		const DeviceInstanceId downstream = f.bridge.rackDeviceAt(1);
		REQUIRE(f.bridge.descriptorOf(downstream) != nullptr);

		// ADDRESSED BY THE DEVICE'S OWN CONTROL INDEX (kill-lanes step 2) — a destination is the device
		// and its control; the modulatable ordinal that used to pack a column is gone.
		constexpr auto kAttack = static_cast<size_t>(feathers::mod::dahdsr::Param::Attack);
		const backplane::Destination attack = backplane::Destination::control(downstream, kAttack);
		CHECK(f.bridge.mayRoute(envelope, attack));
		CHECK(ui::modDestinationName(f.bridge, attack) == std::string("Env Attack 2"));

		// **AND THE OTHER WAY TOO, SINCE 2026-10-03** — IZ (a modulator's position in a lane is only
		// grouping). Two envelopes were held in lane order by their note road, which only listens; once that
		// stopped ordering them (`NoteOps::listensOnly`) they follow the rule two LFOs already did. What is still
		// refused is a device reaching ITSELF, which keeps this about the rule rather than about two devices existing.
		const backplane::Source backwards = backplane::Source::output(downstream, 0);
		const backplane::Destination firstAttack = backplane::Destination::control(f.bridge.rackDeviceAt(0), kAttack);
		CHECK(f.bridge.mayRoute(backwards, firstAttack));
		CHECK_FALSE(f.bridge.mayRoute(backwards, attack));
	}
}
TEST_CASE("a looping envelope DRAWS a loop — the picture changes shape, not just pixels") {
	// The defect: looping appeared not to work and the card drew a wrong curve. The engine was
	// looping correctly the whole time. Measured at the control tick, holding a note, the value cycles
	// 0.55 → 0.80 → 0.94 → 0.85 → … for as long as the gate is up. What was dead was the CARD: it built
	// one fixed run — delay, attack, hold, decay, a sustain plateau, release — and dropped the gate the
	// instant it saw sustain, so a looping envelope drew a curve BYTE-IDENTICAL to a non-looping one.
	// Toggling Loop changed nothing on screen, and a control that changes nothing on screen reads as a
	// control that does nothing.
	//
	// SO THIS ASSERTS THE SHAPE, NOT A DIFFERENCE. "The looping curve differs from the non-looping one"
	// would pass on any change at all — a stray sample, a rounding wobble — and the whole standing rule
	// here is that a difference check is blind exactly where the bug is subtle. What makes a loop a loop
	// is that the curve RISES AGAIN AFTER FALLING, so that is the quantity: count the rises.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	namespace dsr = feathers::mod::dahdsr;

	// The number of times the curve climbs by more than a third of full scale after having fallen by
	// more than a third. Deliberately coarse: it is counting SHAPE, and it must not be able to answer
	// "two" because of sampling noise on a curve that only rises once.
	const auto risesAfterFalling = [&card]() {
		const std::vector<visage::Point> pts = card.curvePoints();
		constexpr float kSwing = 0.33f;
		int rises = 0;
		bool climbing = false;
		float low = pts.empty() ? 0.0f : pts.front().y;
		float high = low;
		for (const visage::Point& p : pts) {
			if (climbing) {
				high = std::max(high, p.y);
				if (high - p.y > kSwing) { // the top is behind us
					climbing = false;
					low = p.y;
				}
			}
			else {
				low = std::min(low, p.y);
				if (p.y - low > kSwing) {
					++rises;
					climbing = true;
					high = p.y;
				}
			}
		}
		return rises;
	};

	dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
	s.attack = dsr::stageValueFor(0.05);
	s.decay = dsr::stageValueFor(0.30);
	s.sustain = 0.5f;
	s.release = dsr::stageValueFor(0.10);
	s.mode = dsr::Mode::Gate;
	f.bridge.setEnvelopeSettings(0, s);

	const int once = risesAfterFalling();
	CAPTURE(once);
	CHECK(once == 1); // attack up, decay down, rest, release down — one climb and no more

	s.mode = dsr::Mode::Loop;
	f.bridge.setEnvelopeSettings(0, s);

	const int looping = risesAfterFalling();
	CAPTURE(looping);
	CHECK(looping == 2); // and the second climb is the whole point of the toggle

	SUBCASE("and it ends mid-cycle rather than resting and releasing to nothing") {
		// While the gate is up a looping envelope never reaches a sustain plateau and never releases, so
		// drawing either of those would be drawing something that does not happen. The right edge of the
		// picture is therefore somewhere in the middle of the travel, not on the floor.
		const std::vector<visage::Point> loopEnd = card.curvePoints();
		REQUIRE(loopEnd.size() > 8);
		CHECK(loopEnd.back().y > 0.1f);

		dsr::Settings back = f.bridge.modRack().dahdsrAt(0);
		back.mode = dsr::Mode::Gate;
		f.bridge.setEnvelopeSettings(0, back);
		const std::vector<visage::Point> restEnd = card.curvePoints();
		CHECK(restEnd.back().y < 0.05f); // released, which is where a non-looping shape has to finish
	}
}
TEST_CASE("the curve fills the box it is given, whatever the times are set to") {
	// The defect: depending on the time settings, the curve spanned either the whole box or only its
	// first half.
	//
	// THE SHAPE'S WIDTH IS THE BOX. Every segment the shape declares gets samples, so the polyline
	// reaches the right-hand edge no matter how lopsided the stage times are against each other — and
	// a run that stalls partway leaves the rest of the box empty, which is what was seen.
	//
	// The case that found it is the last pair below: a 1 ms attack against a 5 s decay, looping. The
	// tick that crossed the loop's seam carried the decay's rate multiplier, so the whole attack
	// completed inside it, the observed stage read Decay on both sides of the seam, and the run never
	// advanced past the first cycle — 49 points ending at x = 0.5, exactly half the domain. The ratio
	// is what matters rather than the absolute times, which is why the pathological pair is here
	// alongside ordinary ones.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	namespace dsr = feathers::mod::dahdsr;

	struct Times {
		const char* what;
		double attack, decay, release;
	};
	const std::array<Times, 5> kTimes { {
	    { "even", 0.05, 0.30, 0.10 },
	    { "fast", 0.01, 0.05, 0.05 },
	    { "slow", 1.00, 2.00, 1.00 },
	    { "a tiny attack against a long decay", 0.001, 5.00, 0.001 },
	    { "a long attack against a tiny decay", 5.000, 0.001, 5.000 },
	} };

	for (const Times& t : kTimes) {
		for (const bool loop : { false, true }) {
			CAPTURE(t.what);
			CAPTURE(loop);
			dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
			s.attack = dsr::stageValueFor(t.attack);
			s.decay = dsr::stageValueFor(t.decay);
			s.release = dsr::stageValueFor(t.release);
			s.sustain = 0.5f;
			s.mode = loop ? dsr::Mode::Loop : dsr::Mode::Gate;
			f.bridge.setEnvelopeSettings(0, s);

			const std::vector<visage::Point> pts = card.curvePoints();
			REQUIRE(pts.size() > 8);
			float rightmost = 0.0f;
			for (const visage::Point& p : pts) {
				rightmost = std::max(rightmost, p.x);
			}
			// THE WHOLE WIDTH, not most of it. Half a box was the symptom; anything short of the edge
			// is the same defect with a different ratio.
			CHECK(rightmost == doctest::Approx(1.0f).epsilon(0.001));
		}
	}
}
TEST_CASE("a bipolar envelope draws in both halves of the box") {
	// The defect: in bipolar mode the display truncated the bottom half and was otherwise wrong.
	//
	// TWO CLAIMS, AND THE SECOND IS THE ONE THAT MATTERS. That the curve GOES negative is about the
	// envelope, and it did all along. What was broken was the MAPPING from a value to a height: the
	// drawing clamped to 0..1, so every negative value landed flat on the floor and everything else
	// sat in the wrong half of the box. No assertion about curvePoints could have caught that, because
	// the points were right the whole time — so the mapping is asserted directly, as a value.
	namespace dsr = feathers::mod::dahdsr;

	// Unipolar: the floor is zero and nothing moves.
	CHECK(ui::EnvCard::plotUnit(0.0f, false) == doctest::Approx(0.0f));
	CHECK(ui::EnvCard::plotUnit(0.5f, false) == doctest::Approx(0.5f));
	CHECK(ui::EnvCard::plotUnit(1.0f, false) == doctest::Approx(1.0f));

	// Bipolar: zero is the MIDDLE of the box, which is the assertion the old code failed — it put
	// zero on the floor, indistinguishable from the bottom of the travel.
	CHECK(ui::EnvCard::plotUnit(-1.0f, true) == doctest::Approx(0.0f));
	CHECK(ui::EnvCard::plotUnit(0.0f, true) == doctest::Approx(0.5f));
	CHECK(ui::EnvCard::plotUnit(1.0f, true) == doctest::Approx(1.0f));
	CHECK(ui::EnvCard::plotUnit(-0.5f, true) == doctest::Approx(0.25f));

	// And the lower half stays DISTINCT rather than collapsing onto one line, which is what
	// "truncates the bottom half" looked like.
	CHECK(ui::EnvCard::plotUnit(-0.25f, true) > ui::EnvCard::plotUnit(-0.75f, true) + 0.2f);

	// A genuine overshoot is still contained — the clamp is kept, after the mapping rather than
	// instead of it.
	CHECK(ui::EnvCard::plotUnit(1.4f, false) == doctest::Approx(1.0f));
	CHECK(ui::EnvCard::plotUnit(-1.4f, true) == doctest::Approx(0.0f));

	// The other half: the curve really does carry negative values, or the mapping above would be
	// exercising a range nothing reaches.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
	s.attack = dsr::stageValueFor(0.05);
	s.decay = dsr::stageValueFor(0.30);
	s.release = dsr::stageValueFor(0.10);
	s.sustain = 0.5f; // which is ZERO once it is doubled and shifted — the middle of the box
	s.bipolar = true;
	f.bridge.setEnvelopeSettings(0, s);

	const std::vector<visage::Point> pts = card.curvePoints();
	float lowest = 9.0f;
	float highest = -9.0f;
	for (const visage::Point& p : pts) {
		lowest = std::min(lowest, p.y);
		highest = std::max(highest, p.y);
	}
	CAPTURE(lowest);
	CAPTURE(highest);
	CHECK(highest > 0.9f);  // it still reaches the top
	CHECK(lowest < -0.9f);  // and it releases to the BOTTOM of a bipolar range, which is -1
	// Seeded at rest rather than at a literal zero: an idle bipolar envelope reads -1, so the curve
	// starts against the floor of the box and not across its middle.
	CHECK(pts.front().y == doctest::Approx(-1.0f));
}
TEST_CASE("while it loops, the sustain box says what it has become") {
	// Some controls change meaning when loop turns on, so their display must follow.
	//
	// AND THE MEASUREMENT THAT MADE IT MORE THAN TIDINESS. dahdsr_types.h already states that while
	// looping "Sustain stops being a level to rest at and becomes the bottom the cycle falls to" — so
	// with the shipping defaults, a full-depth route from a looping envelope spends half its depth as
	// a DC pedestal before the cycle even starts. Measured on Level with base 0.5, amount 1.0 and the
	// default sustain of 0.5: the destination sat against its ceiling 100% of the time and swung by
	// 0.00, while the envelope underneath cycled at 4 Hz. A control called "S" gives a player no
	// reason to look there; one called "Floor" invites the question.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	namespace dsr = feathers::mod::dahdsr;
	const auto& layout = ui::EnvCard::layout();
	size_t sustainBox = ui::EnvCard::kBoxCount;
	for (size_t i = 0; i < layout.size(); ++i) {
		if (layout[i] == dsr::Param::Sustain) {
			sustainBox = i;
		}
	}
	REQUIRE(sustainBox < ui::EnvCard::kBoxCount);

	CHECK(card.boxCaption(sustainBox) == "S");

	dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
	s.mode = dsr::Mode::Loop;
	f.bridge.setEnvelopeSettings(0, s);
	(void)card.curvePoints(); // the seam a settings change arrives through
	CHECK(card.boxCaption(sustainBox) == "Floor");

	// AND BACK, because a caption that follows a mode has to follow it in both directions — a control
	// stuck reading "Floor" on a shape that rests is the same defect wearing the other name.
	s.mode = dsr::Mode::Gate;
	f.bridge.setEnvelopeSettings(0, s);
	(void)card.curvePoints();
	CHECK(card.boxCaption(sustainBox) == "S");

	SUBCASE("and nothing else on the card is renamed by it") {
		// The claim is about ONE control's meaning changing. If a mode swept every caption it would be
		// a theme rather than an honesty fix, and the boxes either side would stop being findable.
		std::vector<std::string> before;
		for (size_t i = 0; i < ui::EnvCard::kBoxCount; ++i) {
			before.push_back(card.boxCaption(i));
		}
		dsr::Settings on = f.bridge.modRack().dahdsrAt(0);
		on.mode = dsr::Mode::Loop;
		f.bridge.setEnvelopeSettings(0, on);
		(void)card.curvePoints();
		for (size_t i = 0; i < ui::EnvCard::kBoxCount; ++i) {
			CAPTURE(i);
			if (i == sustainBox) {
				CHECK(card.boxCaption(i) != before[i]);
			}
			else {
				CHECK(card.boxCaption(i) == before[i]);
			}
		}
	}
}
TEST_CASE("the stage that owns the period owns the width") {
	// The defect: a loop period appeared to be about 10 seconds long. Ten seconds is
	// EXACTLY the top of a stage's declared range, and the engine was measured exact at those settings
	// — period 0.4373 s against 0.437 declared, at every sample rate from 44.1k to 192k and every
	// block size from 64 to 1024 frames. So the time was going somewhere the card could not show.
	//
	// IT COULD NOT SHOW IT BECAUSE THE AXIS WAS LOGARITHMIC. A stage's width came from its control's
	// POSITION IN ITS RANGE, and that range is log2 seconds — so a 10 s hold drew 1.67x the width of a
	// 197 ms attack while taking 50x the time. A stage can own 96% of the period and look like a
	// slightly wide one. The whole reason to draw a shape is to see where its time goes.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	namespace dsr = feathers::mod::dahdsr;

	const auto widthOf = [&card](dsr::Stage stage, dsr::Stage previous) {
		return card.stageEndX(stage) - card.stageEndX(previous);
	};

	SUBCASE("a stage fifty times longer is drawn very much wider, not slightly wider") {
		dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
		s.attack = dsr::stageValueFor(0.197);
		s.hold = dsr::stageValueFor(10.0); // the top of the range — the missing ten seconds
		s.decay = dsr::stageValueFor(0.240);
		s.sustain = 0.0f;
		s.release = dsr::stageValueFor(0.010);
		f.bridge.setEnvelopeSettings(0, s);

		const float attack = widthOf(dsr::Stage::Attack, dsr::Stage::Delay);
		const float hold = widthOf(dsr::Stage::Hold, dsr::Stage::Attack);
		CAPTURE(attack);
		CAPTURE(hold);
		// It is 50.8x the attack in TIME. On the old axis it drew 1.67x. Anything under an order of
		// magnitude is a picture that hides where the period went.
		CHECK(hold > attack * 10.0f);
		// And it dominates the shape outright, which is the thing a glance has to convey.
		CHECK(hold > 0.7f);
	}

	SUBCASE("two stages of nearly equal length are drawn nearly equal") {
		// The other half, and the one that says the axis is proportional rather than merely monotone:
		// 197 ms against 240 ms is a ratio of 1.22, and the drawing has to agree.
		dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
		s.attack = dsr::stageValueFor(0.197);
		s.decay = dsr::stageValueFor(0.240);
		s.hold = dsr::kStageLog2SecondsMin;
		s.delay = dsr::kStageLog2SecondsMin;
		s.sustain = 0.0f;
		f.bridge.setEnvelopeSettings(0, s);

		const float attack = widthOf(dsr::Stage::Attack, dsr::Stage::Delay);
		const float decay = widthOf(dsr::Stage::Decay, dsr::Stage::Hold);
		CAPTURE(attack);
		CAPTURE(decay);
		REQUIRE(attack > 0.0f);
		CHECK(decay / attack == doctest::Approx(240.0f / 197.0f).epsilon(0.05));
	}

	SUBCASE("and a stage far shorter than the rest is still visible") {
		// The one concession, and it is about PIXELS rather than about time: proportional alone makes a
		// 1 ms attack 0.02% of the width, which is not a shape, it is a missing edge.
		dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
		s.attack = dsr::stageValueFor(0.001);
		s.decay = dsr::stageValueFor(5.0);
		s.hold = dsr::kStageLog2SecondsMin;
		s.delay = dsr::kStageLog2SecondsMin;
		f.bridge.setEnvelopeSettings(0, s);

		const float attack = widthOf(dsr::Stage::Attack, dsr::Stage::Delay);
		CAPTURE(attack);
		CHECK(attack > 0.02f); // a visible edge
		CHECK(attack < 0.20f); // but still plainly the short one
	}

	SUBCASE("a stage that does not happen still takes no width at all") {
		// The floor raises stages that HAPPEN. It must not resurrect one that is switched off, which
		// is the convention device.h states for the bottom of a LogSeconds range.
		dsr::Settings s = f.bridge.modRack().dahdsrAt(0);
		s.attack = dsr::stageValueFor(0.197);
		s.decay = dsr::stageValueFor(0.240);
		s.hold = dsr::kStageLog2SecondsMin;
		s.delay = dsr::kStageLog2SecondsMin;
		f.bridge.setEnvelopeSettings(0, s);
		CHECK(widthOf(dsr::Stage::Hold, dsr::Stage::Attack) == doctest::Approx(0.0f).epsilon(1e-4));
		CHECK(card.stageEndX(dsr::Stage::Delay) == doctest::Approx(0.0f).epsilon(1e-4));
	}
}
TEST_CASE("a control says it is modulated even when its source is not the focused one") {
	// The question: can adjusting attack (or another control) in modulation mode create an invisible
	// self route, with no glow on the boxes and no route shown in the Engine rail?
	//
	// MEASURED, AND HALF OF IT WAS TRUE. The bar was there while the envelope was focused — it runs
	// horizontally along the bottom edge, which was found later. What was NOT there is any
	// mark at all once the focus moved: the same box reported amount 1 with a bar, then amount 0 with
	// nothing, one call apart, with the route untouched in the bridge throughout and still making the
	// sound.
	//
	// `amount` is deliberately about the FOCUSED source — it is what a drag edits and what the bar's
	// length has to mean. The cost is that every other route to the control was drawn as NOTHING,
	// which is the same picture a control nobody has touched draws. This is the honesty rule read
	// backwards: a control must not stay silent about what is being done TO it.
	ModFixture f;
	feathers::ui::ModState mod;
	namespace dsr = feathers::mod::dahdsr;

	ui::RackControl attack { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack, &mod };
	ui::ValueBox box { attack, "A" };
	// A bar's length is a fraction of the WIDTH, so a box with no bounds has no bar whatever is
	// routed — which cost a diagnostic a wrong answer before this line existed.
	box.setBounds(0, 0, 60, 34);

	// Nothing routed anywhere: no bar, and nothing to disclose.
	CHECK(attack.display().amount == doctest::Approx(0.0f));
	CHECK(attack.display().routedElsewhere == false);
	CHECK(box.barBounds().width() <= 0.0f);

	// **THIS WAS "ARM THE ENVELOPE ON ITS OWN CARD AND DRAG ITS OWN ATTACK", AND THAT GESTURE NO
	// LONGER EXISTS.**
	//
	// The routing rule (2026-09-05) is that sources must be upstream of destinations, and the narrower
	// decision (2026-09-06) is that a node's bus never carries its own device's output, with a patch
	// still holding such a route dropping it on load. So an
	// envelope can no longer be pointed at its own control: `maySourceRouteTo` refuses it, the knob
	// does not glow for it, and no amount can be written.
	//
	// **THE CASE IS NOT ABOUT THAT GESTURE, WHICH IS WHY IT SURVIVES.** Its subject is disclosure —
	// a control saying something is routed here when the thing routed here is not what is focused —
	// and any source that can legally reach this control proves it. Macro 1 is one: a macro is the
	// plugin's rather than a device's, so nothing can be reordered past it and it is upstream of
	// everything.
	//
	// The gesture's fate is asserted where it now belongs: `test_mod_rack.cpp`, "a modulator reaches
	// devices AFTER it and not before, and not itself".
	// Macro 2 on screen; macro 1 below is the one focus moves TO.
	const backplane::Source armed = backplane::Source::core(1);
	mod.focus(armed);
	mod.arm(true);
	attack.setAmount(1.0f);

	// While it is focused, the bar is the reading, and it is not the disclosure dot's job.
	CHECK(attack.display().amount == doctest::Approx(1.0f));
	CHECK(box.barBounds().width() > 0.0f);
	CHECK(attack.display().routedElsewhere == false);

	SUBCASE("and the moment the focus moves, the control still says something is on it") {
		mod.focus(backplane::Source::core(0)); // another macro — one press of another column is all it takes
		mod.arm(false);

		// The bar is correctly gone: nothing is routed here FROM THE NEWLY FOCUSED MACRO, and a bar
		// means an amount.
		CHECK(attack.display().amount == doctest::Approx(0.0f));
		CHECK(box.barBounds().width() <= 0.0f);

		// THE ASSERTION THIS CASE EXISTS FOR. The route is still live and still making the sound, so
		// the control has to say so — this is the flag that was missing, and without it the box drew
		// exactly like an untouched one.
		CHECK(attack.display().routedElsewhere == true);

		// And it is really still there, or the flag above would be disclosing nothing.
		CHECK(f.bridge.routeAmount(armed, attack.destination()) == doctest::Approx(1.0f));
	}

	SUBCASE("a control with nothing on it never claims otherwise") {
		// The other half, so the flag is not simply always true: a DIFFERENT control on the same
		// device, with the focus moved the same way, has nothing to disclose.
		ui::RackControl decay { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Decay, &mod };
		mod.focus(backplane::Source::core(0));
		mod.arm(false);
		CHECK(decay.display().routedElsewhere == false);
		CHECK(attack.display().routedElsewhere == true);
	}
}
TEST_CASE("with no panel a seating discloses EVERY route, macro 1's included") {
	// **TWO ANSWERS TO ONE QUESTION (backlog DX.2).** A seating handed no `ModState` has nothing armed,
	// so every route on its control is "elsewhere" — which is what `DeviceControl` and `FxControl` say:
	// they except a source nobody publishes. `RackControl` and `MsegControl` excepted their no-panel
	// DEFAULT, `Source::core(0)`, so a control driven by MACRO 1 alone reported itself undriven. The
	// route is live and making the sound; the mark is the control's only way of saying so.
	//
	// MACRO 1 IS THE SOURCE THAT BITES and nothing else would: any other source was disclosed correctly,
	// which is why a case picked by feel would have passed over this.
	ModFixture f;
	feathers::ui::ModState mod;
	namespace dsr = feathers::mod::dahdsr;
	namespace mseg = feathers::mod::mseg;

	const backplane::Source macro1 = backplane::Source::core(0);
	// THE ROUTE IS WRITTEN THROUGH A SEATING THAT HAS A PANEL — a hand's road, not a direct write to
	// the record, so what is disclosed below is a route somebody could actually have made.
	ui::RackControl armedAttack { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack, &mod };
	mod.focus(macro1);
	mod.arm(true);
	armedAttack.setAmount(1.0f);
	REQUIRE(f.bridge.routeAmount(macro1, armedAttack.destination()) == doctest::Approx(1.0f));

	SUBCASE("the envelope's seating") {
		ui::RackControl noPanel { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack };
		CHECK(noPanel.display().routedElsewhere == true);
		// NOT VACUOUS: a control with nothing on it still says nothing, so the flag is not simply on.
		ui::RackControl untouched { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Decay };
		CHECK(untouched.display().routedElsewhere == false);
	}

	SUBCASE("the MSEG's seating, which reads the same line") {
		const size_t slot = f.bridge.addRackDevice(mseg::kTraits.tag);
		REQUIRE(slot < feathers::kMaxRackSlots);
		const DeviceInstanceId device = f.bridge.rackDeviceAt(slot);
		REQUIRE(device != feathers::kNoInstance);
		ui::MsegControl armedScale { f.bridge, device, mseg::Param::TimeScale, &mod };
		REQUIRE(armedScale.display().isDestination);
		armedScale.setAmount(1.0f);
		REQUIRE(f.bridge.routeAmount(macro1, armedScale.destination()) == doctest::Approx(1.0f));

		ui::MsegControl noPanel { f.bridge, device, mseg::Param::TimeScale };
		CHECK(noPanel.display().routedElsewhere == true);
	}
}
TEST_CASE("the modulation bar fills the box rather than lining its bottom edge") {
	// The bar ran horizontally along the bottom edge and was tiny. It now fills the value box
	// vertically and responds to a vertical drag.
	//
	// The report describes an indicator that could not be found on a surface whose entire job is to make
	// routing obvious. A 3px strip under a two-line box is a detail; the amount is not a detail.
	//
	// The drag was ALREADY vertical for both a value and an amount — `anchorY_ - e.position.y`, the
	// rule every drag in this editor follows — so the indicator was the half that disagreed with the
	// gesture. Now it grows along the axis the hand moves.
	ModFixture f;
	feathers::ui::ModState mod;
	namespace dsr = feathers::mod::dahdsr;

	ui::RackControl sustain { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Sustain, &mod };
	ui::ValueBox box { sustain, "S" };
	constexpr int kW = 60;
	constexpr int kH = 34;
	box.setBounds(0, 0, kW, kH);

	// THE BASE IS PUT LOW ON PURPOSE. A bar spans from the base toward where the source would carry
	// the value, and it is CLAMPED at the control's own ends — so with sustain at its default 0.5, an
	// amount of 0.5 and an amount of 1.0 both draw exactly half the box, because both run into the
	// ceiling. A first draft of this case asserted that a full amount fills the whole box and failed
	// with 17 against 34, which was the case being wrong rather than the geometry.
	sustain.setValue(0.2);
	mod.focus(armingSource());
	mod.arm(true);
	sustain.setAmount(0.5f);

	const visage::Bounds bar = box.barBounds();
	CAPTURE(bar.x());
	CAPTURE(bar.y());
	CAPTURE(bar.width());
	CAPTURE(bar.height());

	// FULL WIDTH, which is what "fill the box" means on the axis that is not the amount.
	CHECK(bar.x() == doctest::Approx(0.0f));
	CHECK(bar.width() == doctest::Approx(static_cast<float>(kW)));

	// AND ITS HEIGHT IS THE AMOUNT. Half the control's range, so half the box — against the 3px
	// strip the old one drew whatever the amount was.
	CHECK(bar.height() == doctest::Approx(0.5f * kH).epsilon(0.02));
	CHECK(bar.height() > 6.0f); // taller than any strip, on any box this card would ever use

	SUBCASE("it grows with the amount rather than sliding along an edge") {
		// The claim that makes it an INDICATOR: the extent tracks the number. A bar of fixed size that
		// merely moved would satisfy every check above and say nothing about how far the source
		// reaches.
		const float half = box.barBounds().height();
		sustain.setAmount(0.25f);
		const float quarter = box.barBounds().height();
		CAPTURE(half);
		CAPTURE(quarter);
		CHECK(half > quarter * 1.8f);
		CHECK(quarter == doctest::Approx(0.25f * kH).epsilon(0.02));

		// And with the base low enough to allow it, a large amount really does take nearly all of the
		// box — which is the "fill the whole value box" it was asked for.
		sustain.setAmount(0.8f);
		CHECK(box.barBounds().height() == doctest::Approx(0.8f * kH).epsilon(0.02));
	}

	SUBCASE("a negative amount fills DOWNWARD from the base, not upward") {
		// Which way it reaches is the other half of what the bar says, and it is why the bar is
		// positioned from the base rather than anchored to an edge.
		// Back to the middle, so there is room to reach BOTH ways without either end clamping.
		sustain.setValue(0.5);
		sustain.setAmount(0.4f);
		const visage::Bounds up = box.barBounds();
		sustain.setAmount(-0.4f);
		const visage::Bounds down = box.barBounds();
		CAPTURE(up.y());
		CAPTURE(down.y());
		// Down-the-box is larger y in this coordinate system, so a negative amount starts lower.
		CHECK(down.y() > up.y());
		CHECK(down.height() == doctest::Approx(up.height()).epsilon(0.05));
	}
}
TEST_CASE("every box on the card repaints when the armed flag moves") {
	// The defect: the param boxes did not show the 'modulable' glow.
	//
	// THE GLOW WAS COMPUTED CORRECTLY THE WHOLE TIME. display() reported isDestination and armed, and
	// draw() colours the border from exactly those two — measured, in the case above this one. What
	// was missing is that nobody told the box to draw again when the flag moved.
	//
	// The card watched ModState and the comment on that line claimed the boxes followed it, on the
	// assumption that repainting a card repaints the boxes inside it. visage does not promise that:
	// redraw() marks ONE frame. Knob had it right from the start — knob.cpp watches per widget — and
	// the value box, written later, inherited the assumption instead of the pattern.
	//
	// ASSERTED AS A SUBSCRIPTION, because that is the only half of this a headless case can reach:
	// FrameWatchers says so itself ("a repaint is unobservable headlessly, so what a headless test can
	// check is the SUBSCRIPTION"). The alternative — comparing rendered pixels — is the difference
	// check this project has a standing rule against, and it would pass on any repaint for any reason.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	ui::EnvCard card { f.deviceHost, f.bridge.rackDeviceAt(0), mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth),
	               static_cast<int>(ui::EnvCard::height()));

	// THE CARD ITSELF, which was the only thing subscribed before — kept, because the curve and the
	// chips are drawn by the card and they follow the flag too.
	CHECK(mod.watches(&card));

	// AND EVERY BOX, which is the fix. All of them rather than the one under test: the boxes are
	// built in a loop and a subscription added to one of them by hand is the shape of bug this is.
	for (size_t i = 0; i < ui::EnvCard::kBoxCount; ++i) {
		CAPTURE(i);
		CAPTURE(card.boxCaption(i));
		const visage::Frame* box = card.boxFrame(i);
		REQUIRE(box != nullptr);
		CHECK(mod.watches(box));
	}

	SUBCASE("and the flag really does change what a box would draw") {
		// The other half, or the subscription above is delivering a signal about nothing: the border
		// is the mapping signal, so arming has to move the state the border is drawn from.
		namespace dsr = feathers::mod::dahdsr;
		ui::RackControl attack { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack, &mod };

		CHECK(attack.display().isDestination == true);
		CHECK(attack.display().armed == false);

		mod.focus(armingSource());
		mod.arm(true);
		CHECK(attack.display().armed == true);

		mod.arm(false);
		CHECK(attack.display().armed == false);
	}
}
TEST_CASE("a knob can be built over a device's control, not only over a host parameter") {
	// THE POINT OF THE WHOLE GENERALIZATION. device.h promises a device may define its own interface
	// and still join the shared modulation surface, and that promise was empty while the editor's
	// most-used control could only be built over a clap_id: a swallowed filter's cutoff has no host
	// parameter and never will.
	//
	// RackControl already implemented device::Modulatable, so the proof the interface was sufficient
	// existed — what was missing was Knob taking it. This case is that, end to end: a real Knob over a
	// real device control, reading and writing through the interface alone.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	WidgetRegistry registry;
	namespace dsr = feathers::mod::dahdsr;

	ui::RackControl decay { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Decay, &mod, &history };
	ui::Knob knob { decay, "D" };
	knob.setBounds(0, 0, 52, static_cast<int>(ui::Knob::kHeight));

	// IT READS THE DEVICE'S VALUE, on the knob's own 0..1 sweep. The decay defaults to 500 ms on a
	// log2-seconds range, so it sits well inside the travel rather than at either end — which is what
	// makes this a reading rather than a coincidence.
	const float expected = static_cast<float>(decay.normalized());
	CHECK(knob.normalized() == doctest::Approx(expected));
	CHECK(expected > 0.05f);
	CHECK(expected < 0.95f);

	SUBCASE("and turning it moves the device, not a parameter") {
		const double before = decay.value();
		// Straight through the control, which is what the knob's own drag does.
		decay.setValue(dsr::stageValueFor(2.0));
		CHECK(decay.value() != before);
		// THE VALUE, asserted where the engine will read it — not merely that the knob's sweep moved.
		CHECK(dsr::stageSeconds(f.bridge.modRack().dahdsrAt(0).decay) == doctest::Approx(2.0).epsilon(0.01));
		CHECK(knob.normalized() == doctest::Approx(static_cast<float>(decay.normalized())));
	}

	SUBCASE("its readout comes from the DEVICE's declared unit") {
		// The two roads print differently on purpose — see Modulatable::text. A device control prints
		// from device::Unit; a host parameter prints through the plugin's own formatter, which is what
		// the DAW shows in its automation lane. A knob must not know which it is holding.
		decay.setValue(dsr::stageValueFor(0.25));
		CHECK(decay.text() == "250ms");
	}

	SUBCASE("and it can be routed to, through the interface") {
		mod.focus(armingSource());
		mod.arm(true);
		CHECK(knob.mappable() == true);

		decay.beginAmountGesture();
		decay.setAmount(0.5f);
		decay.endAmountGesture();

		const backplane::Destination dest = backplane::Destination::control(f.bridge.rackDeviceAt(0), static_cast<size_t>(dsr::Param::Decay));
		CHECK(f.bridge.routeAmount(armingSource(), dest) == doctest::Approx(0.5f));

		// AND THE RING DRAWS IT. What reached the glass, rather than what is behind it.
		const ui::Knob::ModRing ring = knob.modRing();
		CHECK(ring.drawn == true);
		CHECK(ring.hi > ring.lo);
	}
}
TEST_CASE("an amount dragged onto a device control can be undone") {
	// IT COULD NOT BE, AND NOTHING SAID SO. ValueBox captured where the amount started, in a member
	// called beforeAmount_, and then never used it — so a route dragged onto one of the envelope
	// card's boxes was not an undo step at all. The knob's road recorded one; the box's did not, for
	// no reason anyone chose.
	//
	// The amount-gesture pair is where that belongs, because the control is the thing that knows how
	// its route is addressed — a widget that recorded this itself would need the source and the
	// destination index, which is exactly what a device control does not have to give.
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	namespace dsr = feathers::mod::dahdsr;

	ui::RackControl attack { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack, &mod, &history };
	mod.focus(armingSource());
	mod.arm(true);

	const backplane::Source source = armingSource();
	const backplane::Destination dest = backplane::Destination::control(f.bridge.rackDeviceAt(0), static_cast<size_t>(dsr::Param::Attack));
	REQUIRE(f.bridge.routeAmount(source, dest) == doctest::Approx(0.0f));

	attack.beginAmountGesture();
	attack.setAmount(0.75f);
	attack.endAmountGesture();
	CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(0.75f));

	// ONE STEP FOR THE WHOLE GESTURE, and it goes back to where the drag started.
	history.undo();
	CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(0.0f));
	history.redo();
	CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(0.75f));

	SUBCASE("and a gesture that changed nothing spends no step") {
		// The same rule the value road follows: a click that turns nothing must not cost an undo.
		const float held = f.bridge.routeAmount(source, dest);
		attack.beginAmountGesture();
		attack.endAmountGesture();
		history.undo();
		// If the empty gesture had recorded a step, this undo would have consumed it and the amount
		// would still read 0.75. It undoes the REAL one instead.
		CHECK(f.bridge.routeAmount(source, dest) == doctest::Approx(0.0f));
		CHECK(held == doctest::Approx(0.75f));
	}
}
TEST_CASE("the gate and the glow are one answer for every widget") {
	// A weight source may not reach a puck destination — maySourceRouteTo says so, and the bridge's
	// write gate enforces it. Knob asked that question before lighting; ValueBox asked only "armed and
	// a destination", so the two surfaces could disagree about whether a drag was legal and the box
	// was the one that would light for a drag the bridge then refused.
	//
	// ModDisplay::routable is now the single answer both read. This case pins that it is the SAME
	// answer as the authority, rather than a third computation that happens to agree today.
	ModFixture f;
	feathers::ui::ModState mod;
	namespace dsr = feathers::mod::dahdsr;

	ui::RackControl attack { f.bridge, f.bridge.rackDeviceAt(0), dsr::Param::Attack, &mod };
	const backplane::Destination dest = backplane::Destination::control(f.bridge.rackDeviceAt(0), static_cast<size_t>(dsr::Param::Attack));

	// Nothing armed: not routable, whatever else is true.
	mod.focus(armingSource());
	CHECK(attack.display().isDestination == true);
	CHECK(attack.display().routable == false);

	mod.arm(true);
	CHECK(attack.display().routable == f.bridge.mayRoute(armingSource(), dest));
	CHECK(attack.display().routable == true);

	SUBCASE("ALT BYPASSES THE ARMED SOURCE: the drag edits the VALUE and creates no route") {
		// BACKLOG 20. Alt (Linux) / Option (macOS) must bypass modulation assign mode, and had stopped
		// doing so. `ValueBox::mappable()` took no modifier at all, so an armed source captured every
		// drag on a value box and nothing released it — a defect on a surface separate from the knob,
		// which is why it gets its own case rather than riding on the knob's.
		//
		// THE CONTROL ABOVE IS ALREADY ARMED AND ROUTABLE, which is the whole fixture this needs.
		REQUIRE(attack.display().routable == true);

		ui::ValueBox box { attack, "A" };
		box.setBounds(0, 0, 40, 90);

		const double valueBefore = attack.value();
		const float amountBefore = attack.display().amount;

		// ALT HELD THROUGH THE WHOLE GESTURE. `beginDrag` reads the modifier at the press, which is
		// where the branch is taken, so the press is the event that has to carry it.
		box.onMouseDown().callback(optAt(20.0f, 70.0f));
		box.onMouseDrag().callback(optAt(20.0f, 20.0f));
		box.onMouseUp().callback(optAt(20.0f, 20.0f));

		// BOTH HALVES, AND THE SECOND IS THE ONE THAT MATTERS. "The value moved" alone passes on a
		// build where Alt does nothing and the drag was simply a plain value drag — which is exactly
		// the broken state, so a one-sided check would have been green before the fix.
		CHECK(attack.value() != doctest::Approx(valueBefore));
		CHECK(attack.display().amount == doctest::Approx(amountBefore));

		SUBCASE("and WITHOUT Alt the same drag routes instead, which is what makes the case above mean anything") {
			// THE NON-VACUITY HALF, AS A GESTURE RATHER THAN AN ASSERTION ABOUT ONE. If an armed drag
			// wrote the value too, the subcase above would pass on a control that had no bypass to
			// exercise.
			ui::ValueBox plain { attack, "A" };
			plain.setBounds(0, 0, 40, 90);
			const double v0 = attack.value();

			plain.onMouseDown().callback(at(20.0f, 70.0f));
			plain.onMouseDrag().callback(at(20.0f, 20.0f));
			plain.onMouseUp().callback(at(20.0f, 20.0f));

			CHECK(attack.display().amount != doctest::Approx(amountBefore));
			CHECK(attack.value() == doctest::Approx(v0));
		}
	}

	SUBCASE("and a source the authority REFUSES does not light the control") {
		// THE FIRST DRAFT OF THIS CASE WAS VACUOUS AND ITS KNOWN-BAD SAID SO. It walked every source
		// against this rack destination, checked `routable == f.bridge.mayRoute(...)`, and passed with
		// `out.routable = out.armed` — the whole gate deleted — because maySourceRouteTo NEVER refuses
		// a rack destination. The case was comparing two expressions that agree everywhere it looked.
		//
		// The refusal the gate exists for is a WEIGHT source aiming at a PUCK destination — a design
		// decision, a matter of taste rather than stability — so that is the pair to ask about, and it
		// needs a parameter control rather than a rack one, because only a parameter can BE a puck
		// destination.
		int puck = -1;
		for (size_t d = 0; d < feathers::kParamDestinationCount; ++d) {
			if (isPuckDestination(d)) {
				puck = static_cast<int>(d);
				break;
			}
		}
		REQUIRE(puck >= 0);
		const ParamId id = feathers::kModDestinations[static_cast<size_t>(puck)];
		const ParamDesc* pdesc = feathers::findParam(static_cast<clap_id>(id));
		REQUIRE(pdesc != nullptr);

		ui::ParamControl path { f.bridge, pdesc, &mod };

		// A MACRO may reach the puck.
		mod.focus(backplane::Source::core(0));
		mod.arm(true);
		const backplane::Destination puckDest = backplane::Destination::core(static_cast<size_t>(puck));
		REQUIRE(f.bridge.mayRoute(backplane::Source::core(0), puckDest) == true);
		CHECK(path.display().routable == true);

		// A WEIGHT SOURCE may not — and this is the assertion the deleted gate fails.
		const backplane::Source weight = backplane::Source::core(feathers::weightSourceOf(0));
		mod.focus(weight);
		REQUIRE(f.bridge.mayRoute(weight, puckDest) == false);
		CHECK(path.display().armed == true);        // armed, and a destination...
		CHECK(path.display().isDestination == true);
		CHECK(path.display().routable == false);    // ...and still must not light
	}
}
TEST_CASE("the scope watches every channel its own picture is made of") {
	// THE WATCH LIST IS HAND-KEPT AND THE CODE SAYS SO ABOUT ITSELF. wave_display.cpp:
	//
	//   "THIS LIST IS A HAND-KEPT SUBSET OF WHAT characterNow() READS, which is the hazard rather
	//    than the fix ... Anything added to characterFromParams has to be added here too, or the
	//    picture quietly stops being of the sound."
	//
	// IT HAS ALREADY FIRED TWICE ON THIS ONE WIDGET. Once on drift — the scope drew a sound with no
	// drift on nobody's plane while the engine played a drifted one, measured at travelX 0.84 against
	// the scope's 0.47 — and once on Smoothness and FM Depth, which sit in the same placement block
	// but outside the SoundField enum, so the constructor's loop could not reach them. it was found the
	// second: "it updates terrain and audible waveform, but not the oscilloscope."
	//
	// Seventeen references to this widget in these tests, and not one of them tied the watch list to
	// what the picture is actually made of. So this DERIVES the list instead of restating it: drive
	// every declared parameter, ask whether the CHARACTER moved, and require a watch for exactly
	// those that did. A channel added to characterFromParams and forgotten here fails this case
	// rather than waiting to be noticed on screen.
	WaveFixture f;

	// THROUGH publish(), WHICH IS THE MODULATED ROAD. characterNow() reads bridge_.modulated(), not
	// the base — the picture follows host modulation exactly as the engine does — so a case that set
	// bases and never published would be driving a road the widget does not read.
	f.publish();

	// A value that is genuinely different from where the parameter sits, inside its declared range.
	// Stepped controls step; continuous ones move a quarter of the way from the default toward
	// whichever end is further, so nothing lands on a clamp and reads as "no change".
	const auto perturbed = [](const ParamDesc& d) {
		if ((d.flags & CLAP_PARAM_IS_STEPPED) != 0) {
			return d.def + 1.0 <= d.max ? d.def + 1.0 : d.def - 1.0;
		}
		const double up = d.max - d.def;
		const double down = d.def - d.min;
		return up >= down ? d.def + up * 0.5 : d.def - down * 0.5;
	};

	std::vector<std::string> reachesPictureButUnwatched;
	// PER PASS: [0] the default engine, [1] Additive. And the names that reached the picture on each, so the Additive pass
	// can be required to have SEEN its own channels rather than only to have found nothing unwatched.
	std::array<size_t, 2> reachesPicture {};
	std::array<std::vector<std::string>, 2> reached;

	// ON THE DEFAULT ENGINE AND AGAIN ON ADDITIVE (2026-10-10): the Shape Amount, the Additive Shape and the six bias knobs
	// reach the picture ONLY on Additive, so a sweep on the default engine alone could never see them — and the scope
	// shipped without watching the first two for exactly that reason.
	for (const bool onAdditive : { false, true }) {
		CAPTURE(onAdditive);
		const size_t pass = onAdditive ? 1 : 0;
		const auto onEngine = [&](ParamSet& s) {
			if (onAdditive) {
				s.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(OscType::Additive));
			}
		};
		{
			ParamSet start;
			onEngine(start);
			f.bridge.publish(start);
			f.bridge.refresh();
		}
		const Character baseline = f.wave.characterNowForTest();
		if (onAdditive) {
			REQUIRE(baseline.engine == feathers::OscEngine::Partials); // this pass really is on Additive
		}
		for (const ParamDesc& desc : paramDescs()) {
			const auto id = static_cast<clap_id>(desc.id);
			const double moved = perturbed(desc);
			if (moved == desc.def || !(desc.max > desc.min)) {
				continue; // nothing to say about a control with nowhere to go
			}
			if (onAdditive && desc.id == ParamId::OscType) {
				continue; // the engine is this pass's ground, swept on the first
			}

			// A FRESH SET EACH TIME, so one channel's perturbation cannot be mistaken for another's.
			ParamSet probe;
			onEngine(probe);
			probe.setBase(id, moved);
			f.bridge.publish(probe);
			// AND REFRESH, WHICH IS THE HALF publish() ALONE DOES NOT DO. seenMod_ — what modulated()
			// reads and therefore what the picture is made of — is filled by refresh(), not by publish().
			// The first draft published without it and the non-vacuity check reported that ZERO
			// parameters reached the picture, which is the case refusing to pass while proving nothing.
			f.bridge.refresh();
			const Character after = f.wave.characterNowForTest();

			// Put it back, so the next probe starts from the same place this one did.
			ParamSet restore;
			onEngine(restore);
			f.bridge.publish(restore);
			f.bridge.refresh();

			if (after == baseline) {
				continue; // this channel is not part of the picture; nothing is owed
			}
			++reachesPicture[pass];
			reached[pass].push_back(std::string(desc.name));

			const std::span<visage::Frame* const> watchers = f.registry.watchers(id);
			const bool watched =
			    std::find(watchers.begin(), watchers.end(), static_cast<visage::Frame*>(&f.wave))
			    != watchers.end();
			if (!watched) {
				reachesPictureButUnwatched.push_back(std::string(desc.name) + (onAdditive ? " (on Additive)" : ""));
			}
		}
	}

	// THE FINDING, NAMED. A count would say "three are missing" and leave the next reader to work out
	// which — and the whole failure mode here is a channel nobody noticed.
	std::string missing;
	for (const std::string& name : reachesPictureButUnwatched) {
		missing += name + "; ";
	}
	CAPTURE(missing);
	CHECK(reachesPictureButUnwatched.empty());

	// AND THE LOOP HAD SOMETHING TO FIND, or the check above is a check on an empty set. This is the
	// non-vacuity half and it is not optional: a `characterNow()` that returned a constant would make
	// every parameter look irrelevant and pass the assertion above perfectly.
	CAPTURE(reachesPicture[0]);
	CAPTURE(reachesPicture[1]);
	// MEASURED AT 26 on the day this was written. The bar is well under that so an ordinary parameter
	// change does not fail the case, and well over zero so a characterNow() that stopped reading
	// anything would. Both passes.
	CHECK(reachesPicture[0] >= 20);
	CHECK(reachesPicture[1] >= 20);
	// AND THE ADDITIVE PASS SAW ADDITIVE'S OWN CHANNELS, BY NAME: a pass that silently stayed on the default engine
	// would find every one of these irrelevant and the unwatched list above empty.
	for (const char* name : { "A Shape Amount", "A Rolloff", "A Formant", "A Odd/Even", "A Comb", "A Detail",
	                          "A Off-grid" }) {
		CAPTURE(name);
		REQUIRE(std::find(reached[1].begin(), reached[1].end(), name) != reached[1].end());
	}
}
TEST_CASE("a channel the scope forgets is caught rather than noticed on screen") {
	// The other half of the case above, and the reason it is worth its runtime: it must FAIL when a
	// channel reaches the picture unwatched. Rather than mutate the widget, this asks the same
	// question of a frame that watches NOTHING — which is the state a newly added channel is in.
	//
	// If this ever passes, the derivation has stopped deriving: it would mean no parameter reaches
	// the picture at all, and the case above would be green for the worst possible reason.
	WaveFixture f;
	f.publish();
	const Character baseline = f.wave.characterNowForTest();

	visage::Frame stranger;
	size_t reaches = 0;
	size_t unwatchedByStranger = 0;

	for (const ParamDesc& desc : paramDescs()) {
		const auto id = static_cast<clap_id>(desc.id);
		if (!(desc.max > desc.min)) {
			continue;
		}
		ParamSet probe;
		probe.setBase(id, desc.max);
		f.bridge.publish(probe);
		f.bridge.refresh();
		const Character after = f.wave.characterNowForTest();
		ParamSet restore;
		f.bridge.publish(restore);
		f.bridge.refresh();
		if (after == baseline) {
			continue;
		}
		++reaches;
		const std::span<visage::Frame* const> watchers = f.registry.watchers(id);
		if (std::find(watchers.begin(), watchers.end(), &stranger) == watchers.end()) {
			++unwatchedByStranger;
		}
	}

	CAPTURE(reaches);
	CAPTURE(unwatchedByStranger);
	CHECK(reaches > 0);
	// Every channel that reaches the picture is unwatched BY THE STRANGER, which is what the case
	// above would report if the real widget forgot one.
	CHECK(unwatchedByStranger == reaches);
}
TEST_CASE("every field the terrain builder reads is in the content key") {
	// terrain.h:425 says this is hand-kept and says what that costs:
	//
	//   "THIS IS WRITTEN OUT BY HAND, WHICH INVERTS A LESSON THIS PROJECT PAID FOR THREE TIMES ... the
	//    per-sound pin, the residue and the travel centre each cost an install to learn that a window
	//    keyed without something the picture depends on leaves the old map up and the verb looks dead."
	//
	// Three installs, one bug. It ends: "ANY NEW FIELD THE BUILDER READS MUST BE ADDED HERE BY HAND —
	// that is the price of the line above, and THE CASE CANNOT COLLECT IT FOR YOU."
	//
	// This case collects it. The claim is derivable from the same seam the exclusion is: perturb one
	// field, build, and compare the map BYTES. If the map moved, the builder read that field, and the
	// key must therefore distinguish the two windows.
	//
	// THE OBVIOUS WAY IS CIRCULAR AND SILENTLY PASSES, which is why the sentence above was written and
	// why it is worth explaining rather than just fixing. Two separate places make the key decide
	// whether a build even happens:
	//
	//   * `TerrainMap::request` opens with `if (wanted_ == window) return;` — so requesting a window
	//     that differs only in an UNKEYED field is coalesced away and the builder never runs.
	//   * `settle()` waits for `now->window == wanted`, which is satisfied immediately by the OLD
	//     snapshot for exactly the same reason.
	//
	// So a naive derivation asks "did the map change?" about a build that never happened, gets "no",
	// and reports that the field is not read — which is the answer the key already assumed. It would
	// pass on the very field it was written to catch.
	//
	// THE REMEDY IS AN INTERPOSER: between the baseline and each probe, request a window the key
	// certainly distinguishes, so the coalescer cannot skip and `wanted_` is genuinely reset. The
	// probe build then really runs, with the probe's values, and the bytes are a measurement of the
	// BUILDER rather than an echo of the key.
	TerrainMap map;
	const double span = 64.0 / 1023.0;

	// A baseline with every field somewhere interesting, so a perturbation cannot be lost in a
	// degenerate corner — a map built at defaults can be insensitive to a field that matters
	// everywhere else.
	// ONE BASELINE IS NOT ENOUGH, AND FINDING THAT OUT IS HALF THE VALUE OF THIS CASE.
	//
	// The first draft probed from a single Morph window and reported SIX fields the builder ignores,
	// four of which the header says it reads. The probe was weak, not the header: this file's own
	// case says "morph is byte-identical across two spellings of the same homes and stair is not", so
	// zoneBaseline and offsetBaseline are genuinely inert on Morph. A field that matters only for
	// another engine reads as unread when it is only ever asked about on this one.
	//
	// So the sweep runs from several baselines and a field counts as READ if it moves the map under
	// ANY of them. That is the honest form of the question — the key is one key for every window the
	// chart will ever be asked for, not for the one a case happened to pick.
	const auto baselineFor = [&](OscType engine) {
		TerrainWindow w = windowAt(0.5, span, 2.0);
		w.engine = engine;
		w.smoothnessScale = 0.6f;
		w.fmDepth = 0.3f;
		w.gammaBase = 0.25;
		return w;
	};
	const std::array<OscType, 4> kEngines { OscType::Morph, OscType::Stair, OscType::V5, OscType::Resonate };

	// A window the key distinguishes from everything below — its home centre is nowhere near theirs.
	const TerrainWindow interposer = windowAt(0.9, span, 2.0);


	struct Field {
		const char* name;
		void (*mutate)(TerrainWindow&);
	};
	// The ENGINE is not probed as a field: it is the axis the sweep runs along, and a window whose
	// engine changed is a different chart by inspection rather than by measurement.
	const std::array<Field, 13> kFields { {
	    { "pin", [](TerrainWindow& w) { w.pin.pinned = !w.pin.pinned; } },
	    { "residue", [](TerrainWindow& w) { w.residue.morph.offset[0] += 0.3; } },
	    { "gammaBase", [](TerrainWindow& w) { w.gammaBase = 0.75; } },
	    { "zoneBaseline", [](TerrainWindow& w) { w.zoneBaseline += 40.0; } },
	    { "offsetBaseline", [](TerrainWindow& w) { w.offsetBaseline += 0.35; } },
	    { "homeCentre", [](TerrainWindow& w) { w.travelYCentre += 0.2; } },
	    { "homeSpan", [](TerrainWindow& w) { w.travelYSpan *= 2.0; } },
	    { "travelCentre", [](TerrainWindow& w) { w.travelXCentre += 0.4; } },
	    { "travelSpan", [](TerrainWindow& w) { w.travelXSpan = 3.0; } },
	    { "smoothnessScale", [](TerrainWindow& w) { w.smoothnessScale = 1.4f; } },
	    { "fmDepth", [](TerrainWindow& w) { w.fmDepth = 0.9f; } },
	    { "weaveModel", [](TerrainWindow& w) { w.weaveModel = feathers::WeaveModel::Membrane; } },
	    { "resonateShaping", [](TerrainWindow& w) { w.resonateShaping.stiffness = 3.0f; } },
	} };

	std::vector<std::string> readButUnkeyed;
	std::vector<std::string> neverMovedAnyMap;
	size_t movedSomeMap = 0;

	// EACH ENGINE'S BASELINE IS BUILT ONCE, not once per field (backlog KB). It is the same window every
	// time, so the 33 rebuilds were the same bytes 33 times — 60 of this case's 132 builds, on a case that
	// took 83 s at load 56 against `run-cases.sh --each`'s 120 s watchdog. Still built behind the
	// interposer, so it is a real build of the baseline and not whatever the map happened to hold.
	std::array<std::vector<uint32_t>, kEngines.size()> baselines;
	for (size_t e = 0; e < kEngines.size(); ++e) {
		const TerrainWindow base = baselineFor(kEngines[e]);
		map.request(interposer);
		REQUIRE(settle(map, interposer));
		map.request(base);
		REQUIRE(settle(map, base));
		baselines[e] = map.snapshot()->texels;
	}

	for (const Field& f : kFields) {
		CAPTURE(std::string(f.name));
		bool movedHere = false;
		bool keyedHere = false;

		for (size_t e = 0; e < kEngines.size(); ++e) {
			const TerrainWindow base = baselineFor(kEngines[e]);
			const std::vector<uint32_t>& baseline = baselines[e];

			TerrainWindow probe = base;
			f.mutate(probe);
			map.request(interposer);
			REQUIRE(settle(map, interposer));
			map.request(probe);
			REQUIRE(settle(map, probe));

			if (map.snapshot()->texels != baseline) {
				movedHere = true;
			}
			if (!(probe == base)) {
				keyedHere = true;
			}
		}

		if (movedHere) {
			++movedSomeMap;
			// THE WHOLE CLAIM. The builder read this field on at least one chart, so a window that
			// changes it needs a different map — and a key that calls the two equal leaves the old map
			// up, which is the three-install bug.
			if (!keyedHere) {
				readButUnkeyed.push_back(std::string(f.name));
			}
		}
		else {
			neverMovedAnyMap.push_back(std::string(f.name));
		}
	}

	std::string missing;
	for (const std::string& f : readButUnkeyed) {
		missing += f + "; ";
	}
	// NAMED, NOT COUNTED — the failure mode here is nobody noticing WHICH field, three times over.
	CAPTURE(missing);
	CHECK(readButUnkeyed.empty());

	std::string inert;
	for (const std::string& f : neverMovedAnyMap) {
		inert += f + "; ";
	}
	CAPTURE(inert);

	// NON-VACUITY, AND IT IS NOT A FORMALITY HERE. If the interposer stopped working — if the
	// coalescer swallowed every probe — every map would equal its baseline, nothing would be "read
	// but unkeyed", and the check above would pass while measuring nothing at all. That is exactly
	// the failure this case exists to prevent, turned on itself.
	CAPTURE(movedSomeMap);
	// MEASURED AT 8 OF 11 across the three engines. The bar sits under that so an ordinary change does
	// not fail the case, and well clear of zero so a harness that stopped building would.
	CHECK(movedSomeMap >= 6);

	// THE ONES NO PROBE COULD MOVE, recorded rather than glossed: pin and offsetBaseline. Both are IN
	// the key and this case could not demonstrate the builder reading them, which is a limit of the
	// probing rather than a finding about the code:
	//
	//   * `pin` is toggled here with a DEFAULT generator, and pinning an orientation to nothing is
	//     probably indistinguishable from not pinning it. A stronger probe needs a real generator.
	//   * `offsetBaseline` moved nothing at these homes on any of the three engines.
	//
	// BEING IN THE KEY IS THE SAFE SIDE FOR BOTH. An over-keyed field costs a rebuild of an identical
	// map; an under-keyed one leaves the wrong map on screen, which is the bug that cost three
	// installs. So this case deliberately does NOT assert that everything in the key is read — that
	// direction is guarded by the gamma exclusion case, where the cost of over-keying was real and
	// measured. What it asserts is the dangerous direction only.

	SUBCASE("and gammaBase is IN the key, because the builder now reads it") {
		// **THIS SUBCASE IS INVERTED BY backlog GY, and the inversion is the point.** It used to assert
		// that `viewGamma` was EXCLUDED — terrain.h's reasoning being that the builder never read it,
		// so keying it would ask for a map identical to the one already on screen. That was true while
		// the chart drew home down its vertical axis: gamma moved the view and the band together, and
		// the rows were built at the sound's own dials with gamma nowhere in the arithmetic.
		//
		// By design both display axes are displacements in the decoded plane and `gammaBase`
		// completes the ONE home the whole map is taken at. Move it and every texel is a different
		// place. So the field the builder ignored has become the field it depends on most, and the
		// exclusion that was correct is now exactly the under-keying that "cost three installs".
		//
		// DELETING THIS SUBCASE WOULD HAVE BEEN THE EASY MOVE AND THE WRONG ONE: what it guards is the
		// agreement between `operator==` and what the builder reads, and that agreement still needs a
		// guard — it just points the other way now.
		TerrainWindow reference = baselineFor(OscType::Morph);
		TerrainWindow moved = reference;
		moved.gammaBase = 0.75;
		CHECK_FALSE(moved == reference);

		// AND THE BUILDER REALLY READS IT, which is the half that makes the key honest rather than
		// merely conservative. The probe table above moves `gammaBase` like every other field; if it
		// ever stops moving the bytes, the key has become over-keyed and this is where that shows.
		CAPTURE(inert);
		CHECK(std::find(neverMovedAnyMap.begin(), neverMovedAnyMap.end(), std::string("gammaBase"))
		      == neverMovedAnyMap.end());
	}
}
TEST_CASE("explore watches every parameter its terrain window is made of, on every engine") {
	// THE SCOPE GUARD'S METHOD, ON THE MAP (2026-10-10). The stage's watch list is hand-kept beside askForTerrain, and a
	// parameter the window reads but the stage does not watch is a map that goes stale under a turned knob — the old map
	// stays up and the verb looks dead. So DERIVE it: on each engine, perturb every parameter's base, ask whether the
	// window the stage would request moved, and require an Explore watch for exactly those that did.
	ExploreFixture f;
	const auto perturbed = [](const ParamDesc& d) {
		if ((d.flags & CLAP_PARAM_IS_STEPPED) != 0) {
			return d.def + 1.0 <= d.max ? d.def + 1.0 : d.def - 1.0;
		}
		const double up = d.max - d.def;
		const double down = d.def - d.min;
		return up >= down ? d.def + up * 0.5 : d.def - down * 0.5;
	};
	std::vector<std::string> unwatched;
	std::vector<std::string> reachedOnAdditive;
	for (size_t e = 0; e < static_cast<size_t>(OscType::kCount); ++e) {
		const auto engine = static_cast<OscType>(e);
		CAPTURE(static_cast<int>(engine));
		const auto onEngine = [&](ParamSet& s) {
			s.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(engine));
		};
		ParamSet start;
		onEngine(start);
		f.bridge.publish(start);
		f.bridge.refresh();
		const TerrainWindow baseline = f.stage.askForTerrainForTest();
		REQUIRE(baseline.engine == engine);
		size_t reached = 0;
		for (const ParamDesc& desc : paramDescs()) {
			const auto id = static_cast<clap_id>(desc.id);
			const double moved = perturbed(desc);
			if (moved == desc.def || !(desc.max > desc.min) || desc.id == ParamId::OscType) {
				continue; // nowhere to go, or the engine this pass stands on
			}
			ParamSet probe;
			onEngine(probe);
			probe.setBase(id, moved);
			f.bridge.publish(probe);
			f.bridge.refresh();
			const TerrainWindow after = f.stage.askForTerrainForTest();
			ParamSet restore;
			onEngine(restore);
			f.bridge.publish(restore);
			f.bridge.refresh();
			if (after == baseline) {
				continue;
			}
			++reached;
			if (engine == OscType::Additive) {
				reachedOnAdditive.push_back(std::string(desc.name));
			}
			const std::span<visage::Frame* const> watchers = f.registry.watchers(id);
			if (std::find(watchers.begin(), watchers.end(), static_cast<visage::Frame*>(&f.stage)) == watchers.end()) {
				unwatched.push_back(std::string(desc.name) + " (engine " + std::to_string(e) + ")");
			}
		}
		CAPTURE(reached);
		CHECK(reached >= 3); // the zone, the offset and the selected sound's Smoothness at least, on every engine
	}
	std::string missing;
	for (const std::string& name : unwatched) {
		missing += name + "; ";
	}
	CAPTURE(missing);
	CHECK(unwatched.empty());
	// NOT VACUOUS WHERE IT MATTERS: on Additive the window really is made of the Shape Amount and the six bias knobs.
	for (const char* name : { "A Shape Amount", "A Rolloff", "A Formant", "A Odd/Even", "A Comb", "A Detail",
	                          "A Off-grid", "Additive Shape" }) {
		CAPTURE(name);
		CHECK(std::find(reachedOnAdditive.begin(), reachedOnAdditive.end(), name) != reachedOnAdditive.end());
	}
}
TEST_CASE("the terrain window is assembled from bases, not from modulated readings") {
	// TWO ADJACENT WIDGETS ON DELIBERATELY OPPOSITE ROADS, and until this case neither file said so.
	//
	// WaveDisplay::characterNow reads MODULATED and explains itself: "the picture is of the sound
	// being played". ExploreStage::askForTerrain reads BASE, and explained nothing — it simply did,
	// and the behaviour it was asked for held by accident:
	//
	//   "field controls should be modulable, but no need to redraw the terrain on modulation."
	//
	// THE TERRAIN IS A MAP OF A PLACE, not a picture of a moment. Modulation moves where you are
	// STANDING — the puck, which is drawn from modulated values — not what the country looks like.
	//
	// AND GETTING IT BACKWARDS DOES NOT DRAW A WRONG PICTURE, IT REBUILDS. The window IS the content
	// key and the key gates TerrainMap::request, so reading modulated there asks for a fresh 128x128
	// map every modulated block: the expensive half of exactly what the design excludes, arriving as a
	// performance complaint rather than as a visibly wrong chart.
	//
	// So this pins the road. Modulate a param the key CONTAINS, and require that the window does not
	// move and that no build is asked for.
	ExploreFixture f;

	// BOTH FIELDS A ROUTE CAN ACTUALLY REACH, and that is the whole of the extension made on
	// 2026-08-24. This case pinned `smoothnessScale` alone, and `fmDepth` — the other window field
	// fed by a modulatable parameter — was uncovered: MEASURED, by changing that one site from
	// `value()` to `modulated()` and watching this case stay GREEN while the same mutation at the
	// smoothness site turned it red. One guarded site next to one unguarded one is the worst
	// arrangement of the two, because the guarded one makes the file look covered.
	//
	// Both are in the content key — terrain.h's hand-written `operator==` names
	// `smoothnessScale` and `fmDepth` in its last line — so either of them reading modulated is the
	// rebuild-per-block the design excludes, not merely a wrong number.
	constexpr double kBase = 0.6;
	constexpr double kFmBase = 0.3;
	f.set(ParamId::ASmoothness, kBase);
	f.set(ParamId::AFmDepth, kFmBase);
	const TerrainWindow before = f.stage.askForTerrainForTest();
	CHECK(before.smoothnessScale == doctest::Approx(static_cast<float>(kBase)));
	CHECK(before.fmDepth == doctest::Approx(static_cast<float>(kFmBase)));

	// LET THE FIRST BUILD FINISH BEFORE COUNTING. builds() is incremented by the WORKER, not by
	// request(), so a count taken straight after asking reads whatever the thread has got to — the
	// first draft captured 0 and compared it against 1 a moment later, which is a race in the case
	// rather than a rebuild in the code.
	REQUIRE(settle(f.terrain, before));
	const uint64_t buildsBefore = f.terrain.builds();

	// NOW MODULATE IT FOR REAL — a macro routed at depth, which is the road the plugin uses rather
	// than a value poked into a field. resolved() moves; the base does not.
	feathers::test::Routes routes;
	const int destination = feathers::modDestinationIndexOf(ParamId::ASmoothness);
	const int fmDestination = feathers::modDestinationIndexOf(ParamId::AFmDepth);
	REQUIRE(destination >= 0);
	REQUIRE(fmDestination >= 0);
	// ONE MACRO INTO BOTH, so a single publish moves both readings and neither field can be the one
	// that happened not to be modulated when the assertion below ran.
	routes.set(feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(destination)), 1.0f);
	routes.set(feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(fmDestination)), 1.0f);
	routes.publish(f.params);
	f.params.setBase(static_cast<clap_id>(ParamId::Macro1), 1.0);
	f.bridge.publish(f.params);
	f.bridge.refresh();

	// THE MODULATION IS REAL, or everything below is a claim about nothing. This is the non-vacuity
	// half and it is the one that would have made this case decoration: a route that reached nowhere
	// would leave base and modulated identical and every assertion would pass for the wrong reason.
	const double base = f.bridge.value(static_cast<clap_id>(ParamId::ASmoothness));
	const double modulated = f.bridge.modulated(static_cast<clap_id>(ParamId::ASmoothness));
	const double fmBase = f.bridge.value(static_cast<clap_id>(ParamId::AFmDepth));
	const double fmModulated = f.bridge.modulated(static_cast<clap_id>(ParamId::AFmDepth));
	CAPTURE(base);
	CAPTURE(modulated);
	CAPTURE(fmBase);
	CAPTURE(fmModulated);
	REQUIRE(base == doctest::Approx(kBase));
	REQUIRE(modulated != doctest::Approx(base));
	REQUIRE(fmBase == doctest::Approx(kFmBase));
	REQUIRE(fmModulated != doctest::Approx(fmBase));

	// THE CLAIM, AS A VALUE. Not "the window compares equal" — that would ask the content key about
	// itself, which is the circularity the key's own derivation case had to work around. This reads
	// the field and requires the BASE number.
	const TerrainWindow after = f.stage.askForTerrainForTest();
	CAPTURE(after.smoothnessScale);
	CAPTURE(after.fmDepth);
	CHECK(after.smoothnessScale == doctest::Approx(static_cast<float>(base)));
	CHECK(after.smoothnessScale != doctest::Approx(static_cast<float>(modulated)));
	CHECK(after.fmDepth == doctest::Approx(static_cast<float>(fmBase)));
	CHECK(after.fmDepth != doctest::Approx(static_cast<float>(fmModulated)));

	SUBCASE("and the additive Shape Amount on Additive, on the same terms") {
		// ITS OWN SUBCASE for the reason the next one gives: it moves base values (the engine), which should rebuild.
		// The Shape Amount is in the content key on Additive (`TerrainWindow::additive`), so reading it modulated there
		// is the rebuild-per-block the design excludes.
		constexpr double kShapeBase = 0.4;
		f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
		f.set(ParamId::AShapeAmount, kShapeBase);
		const int shapeDestination = feathers::modDestinationIndexOf(ParamId::AShapeAmount);
		REQUIRE(shapeDestination >= 0);
		routes.set(feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(shapeDestination)),
		           1.0f);
		routes.publish(f.params);
		f.bridge.publish(f.params);
		f.bridge.refresh();
		const double shapeBase = f.bridge.value(static_cast<clap_id>(ParamId::AShapeAmount));
		const double shapeModulated = f.bridge.modulated(static_cast<clap_id>(ParamId::AShapeAmount));
		CAPTURE(shapeBase);
		CAPTURE(shapeModulated);
		REQUIRE(shapeBase == doctest::Approx(kShapeBase));
		REQUIRE(shapeModulated != doctest::Approx(shapeBase)); // the route is real
		const TerrainWindow shaped = f.stage.askForTerrainForTest();
		CHECK(shaped.additive.shapeAmount == doctest::Approx(static_cast<float>(shapeBase)));
		CHECK(shaped.additive.shapeAmount != doctest::Approx(static_cast<float>(shapeModulated)));
	}

	SUBCASE("and the six additive bias knobs on Additive, on the same terms") {
		// ITS OWN SUBCASE for Shape Amount's reason: it moves base values. All six are in the content key on Additive
		// (`TerrainWindow::additive.bias`), so reading one modulated there is the rebuild-per-block the design excludes.
		f.set(ParamId::OscType, static_cast<double>(OscType::Additive));
		const struct { const char* name; ParamId id; float feathers::PartialBias::* field; } kBias[] = {
		    { "rolloff", feathers::additiveRolloffParam(0), &feathers::PartialBias::rolloff },
		    { "formant", feathers::additiveFormantParam(0), &feathers::PartialBias::formant },
		    { "odd/even", feathers::additiveOddEvenParam(0), &feathers::PartialBias::oddEven },
		    { "comb", feathers::additiveCombParam(0), &feathers::PartialBias::comb },
		    { "detail", feathers::additiveDetailParam(0), &feathers::PartialBias::detail },
		    { "off-grid", feathers::additiveOffGridParam(0), &feathers::PartialBias::offGrid },
		};
		for (const auto& bias : kBias) {
			CAPTURE(bias.name);
			const int dest = feathers::modDestinationIndexOf(bias.id);
			REQUIRE(dest >= 0);
			feathers::test::Routes one;
			one.set(feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest)), 1.0f);
			one.publish(f.params);
			f.params.setBase(static_cast<clap_id>(bias.id), 0.25);
			f.params.setBase(static_cast<clap_id>(ParamId::Macro1), 1.0);
			f.bridge.publish(f.params);
			f.bridge.refresh();
			const double biasBase = f.bridge.value(static_cast<clap_id>(bias.id));
			const double biasMod = f.bridge.modulated(static_cast<clap_id>(bias.id));
			CAPTURE(biasBase);
			CAPTURE(biasMod);
			REQUIRE(biasBase == doctest::Approx(0.25));
			REQUIRE(biasMod != doctest::Approx(biasBase)); // the route is real
			const TerrainWindow w = f.stage.askForTerrainForTest();
			CHECK(w.additive.bias.*bias.field == doctest::Approx(static_cast<float>(biasBase)));
			CHECK(w.additive.bias.*bias.field != doctest::Approx(static_cast<float>(biasMod)));
		}
	}

	SUBCASE("and the five field modifier scales are read from base on the same terms") {
		// ITS OWN SUBCASE, NOT THE SHARED BODY, AND THAT IS A CORRECTNESS FIX RATHER THAN TIDINESS.
		// doctest re-runs the whole case body once per subcase, so a loop sitting in the shared body
		// runs during "and no rebuild was asked for" too — and this loop moves BASE values, which
		// SHOULD rebuild the terrain. It did: that sibling went red with builds() == 2 against a
		// buildsBefore of 1, reporting a rebuild-on-modulation regression that had not happened.
		//
		// The lesson is the file's own: a case that changes shared state is a case that can falsify
		// its neighbours, and the failure names the neighbour rather than the culprit.
		// THE FIVE FIELD MODIFIER KNOBS, ON EXACTLY THE SAME TERMS (2026-08-25). This is the second half
		// of backlog §4 and it is the half that is a constraint rather than a feature: the
		// controls become modulation destinations, AND modulation of them must not trigger a terrain
		// rebuild. All five are in the content key, so `value()` in askForTerrain is the only thing
		// holding the second clause — and before this loop existed it was holding it unguarded.
		//
		// Each is routed, each is checked non-vacuously (the route really moved the resolved value), and
		// each window field is required to be the BASE number.
		const struct { const char* name; ParamId id; float TerrainWindow::* field; } kScales[] = {
		    { "quantize", feathers::quantizeParam(0), &TerrainWindow::quantizeScale },
		    { "sync", feathers::syncParam(0), &TerrainWindow::syncScale },
		    { "feedback", feathers::feedbackParam(0), &TerrainWindow::feedbackScale },
		    { "points", feathers::pointsParam(0), &TerrainWindow::pointScale },
		    { "grit", feathers::gritParam(0), &TerrainWindow::gritScale },
		};
		for (const auto& scale : kScales) {
			CAPTURE(scale.name);
			const int dest = feathers::modDestinationIndexOf(scale.id);
			REQUIRE(dest >= 0);
			feathers::test::Routes one;
			one.set(feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest)), 1.0f);
			one.publish(f.params);
			f.params.setBase(static_cast<clap_id>(scale.id), 0.25);
			f.params.setBase(static_cast<clap_id>(ParamId::Macro1), 1.0);
			f.bridge.publish(f.params);
			f.bridge.refresh();

			const double scaleBase = f.bridge.value(static_cast<clap_id>(scale.id));
			const double scaleMod = f.bridge.modulated(static_cast<clap_id>(scale.id));
			CAPTURE(scaleBase);
			CAPTURE(scaleMod);
			// NON-VACUITY FIRST. Without this, a route that reached nowhere would leave the two roads
			// identical and the assertion below would pass for the worst possible reason.
			REQUIRE(scaleBase == doctest::Approx(0.25));
			REQUIRE(scaleMod != doctest::Approx(scaleBase));

			const TerrainWindow w = f.stage.askForTerrainForTest();
			CHECK(w.*scale.field == doctest::Approx(static_cast<float>(scaleBase)));
			CHECK(w.*scale.field != doctest::Approx(static_cast<float>(scaleMod)));
		}
	}

	SUBCASE("and no rebuild was asked for") {
		// The half that matters most. The window did not move, so request() coalesces and the
		// worker is never woken — which is what "no need to redraw the terrain on modulation" means in
		// the only terms that can be measured.
		// request() coalesces under its own mutex and returns before any thread can act, so a count
		// read straight after it cannot miss a build that was going to happen.
		f.stage.askForTerrainForTest();
		CHECK(f.terrain.builds() == buildsBefore);
	}

	SUBCASE("and the window's identity reads are no route's to reach, and the per-sound fields pinned above are") {
		// WHY THIS CASE CAN STOP WHERE IT DOES, DERIVED RATHER THAN ASSERTED.
		//
		// IT SAID TWO UNTIL 2026-08-25, then SEVEN, and each growth made the old count false rather than
		// merely incomplete. A case whose stated reason outlives the fact it rested on is the shape this
		// file's own comments keep warning about, so it no longer names a count.
		//
		// WHAT IS PINNED ABOVE: the selected sound's Smoothness and FM Depth, the five field modifiers,
		// and on Additive its Shape Amount and six bias knobs — every PER-SOUND destination the window
		// reads but one. NOT PINNED BY THIS CASE, and said so rather than implied: the per-sound Warp
		// depth, and the engine-wide destinations the window also reads (Resonate's Stiffness and
		// Coupling, the Basic Shapes mix). Those are reads of `bridge_.value()` too, unguarded here.
		//
		// The remaining reads — the engine, the zone baseline, the offset baseline — are a different
		// kind, and the obvious objection is that they are unguarded. They are unguardABLE: a
		// mutation at any of them is INERT, because `modulated()` and `value()` cannot differ for a
		// parameter no route can address, and none of those three is a modulation destination.
		//
		// params.h says why, in a sentence about this exact surface: *"A32 IS UNTOUCHED. Explore
		// watches IDENTITY — Zone, Offset, Gamma — and not one of these three is that." So the
		// omission is a consequence of the destination table rather than of anybody's diligence.
		//
		// AND THAT IS WHY IT IS ASSERTED HERE RATHER THAN WRITTEN IN A COMMENT. The day one of those
		// three becomes modulatable, a real hazard appears at a site this case does not cover — and
		// the sentence explaining why it need not cover it would still read as true. This fails
		// instead, which is the only form of the argument that survives its own premise changing.
		CHECK(feathers::modDestinationIndexOf(feathers::smoothnessParam(0)) >= 0);
		CHECK(feathers::modDestinationIndexOf(feathers::fmDepthParam(0)) >= 0);
		// AND THE FIVE, which is the point of the work that added them: it was asked for field modifier
		// knobs "as mod targets", so a knob that is not a destination is the feature not landing.
		CHECK(feathers::modDestinationIndexOf(feathers::quantizeParam(0)) >= 0);
		CHECK(feathers::modDestinationIndexOf(feathers::syncParam(0)) >= 0);
		CHECK(feathers::modDestinationIndexOf(feathers::feedbackParam(0)) >= 0);
		CHECK(feathers::modDestinationIndexOf(feathers::pointsParam(0)) >= 0);
		CHECK(feathers::modDestinationIndexOf(feathers::gritParam(0)) >= 0);
		// AND ADDITIVE'S SEVEN: Shape Amount and the six bias knobs.
		for (const ParamId id : { feathers::shapeAmountParam(0), feathers::additiveRolloffParam(0),
		                          feathers::additiveFormantParam(0), feathers::additiveOddEvenParam(0),
		                          feathers::additiveCombParam(0), feathers::additiveDetailParam(0),
		                          feathers::additiveOffGridParam(0) }) {
			CHECK(feathers::modDestinationIndexOf(id) >= 0);
		}
		CHECK(feathers::modDestinationIndexOf(ParamId::OscType) < 0);
		// IDENTITY IS EDIT-ONLY SINCE BACKLOG PQ — a Relative macro's edit road, never compiled as a modulation — so
		// the terrain window still reads a base nothing modulates.
		for (const ParamId id : { ParamId::AZone, ParamId::AOffset, ParamId::AGamma }) {
			const int at = feathers::modDestinationIndexOf(id);
			CHECK((at < 0 || feathers::isEditOnlyDestination(static_cast<size_t>(at))));
		}

		// AND THE TWO THIS PINS ARE THE ONES THE WINDOW ACTUALLY READS, which the checks above do not
		// say on their own — they are about the destination table, not about `askForTerrain`. The
		// fixture's selection is slot 0, so these are the ids that assembly reaches for.
		CHECK(feathers::smoothnessParam(0) == ParamId::ASmoothness);
		CHECK(feathers::fmDepthParam(0) == ParamId::AFmDepth);
		CHECK(feathers::quantizeParam(0) == ParamId::AQuantize);
		CHECK(feathers::syncParam(0) == ParamId::ASync);
		CHECK(feathers::feedbackParam(0) == ParamId::AFeedback);
		CHECK(feathers::pointsParam(0) == ParamId::APoints);
		CHECK(feathers::gritParam(0) == ParamId::AGrit);
	}

	SUBCASE("while the same modulation DOES move the scope, which is the whole contrast") {
		// Without this the case above is satisfied by a bridge that publishes no modulation at all —
		// and the two widgets would be consistent for the worst possible reason. The wave display is
		// built on the same bridge and reads the other road, so its character must move.
		WaveDisplay scope { f.bridge, f.registry };
		scope.setBounds(0.0f, 0.0f, WaveDisplay::kWidth, WaveDisplay::kHeight);
		const Character modulatedCharacter = scope.characterNowForTest();

		ParamSet unrouted;
		unrouted.setBase(static_cast<clap_id>(ParamId::ASmoothness), kBase);
		f.bridge.publish(unrouted);
		f.bridge.refresh();
		const Character plainCharacter = scope.characterNowForTest();

		CHECK_FALSE(modulatedCharacter == plainCharacter);
	}
}

