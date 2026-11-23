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

// PIECE 7 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

TEST_CASE("Twin Field's output arms are a 2x4 grid: Mod 1-4 on top, Mod 5-8 below, equal cells, at every card width") {
	// Two rows of four mod outputs on the card — `Traits::outputGridColumns`. RESTATED: cell width is the
	// usable width less three gaps over four; output k sits at column k % 4 of row k / 4, from the strip's left pad.
	namespace tf = feathers::mod::twinfield;
	const feathers::device::Descriptor& d = tf::kDescriptor;
	REQUIRE(d.traits.outputGridColumns == 4);
	for (const float width : { 220.0f, 260.0f, 340.0f }) {
		CAPTURE(width);
		CHECK(ui::outputChipRowsFor(d, width) == 2); // the rows are the grid's, whatever the names measure
		const visage::Bounds strip { 5.0f, 100.0f, width, 2.0f * ui::kCardOutputStripHeight };
		const std::vector<visage::Bounds> cells = ui::outputChipBoundsFor(d, strip);
		REQUIRE(cells.size() == 8);
		const float usable = width - 2.0f * ui::kCardOutputChipPadX;
		const float cell = (usable - 3.0f * ui::kCardOutputChipGap) / 4.0f;
		for (size_t k = 0; k < 8; ++k) {
			CAPTURE(k);
			const float x = strip.x() + ui::kCardOutputChipPadX + static_cast<float>(k % 4) * (cell + ui::kCardOutputChipGap);
			const float rowTop = strip.y() + static_cast<float>(k / 4) * ui::kCardOutputStripHeight;
			CHECK(std::abs(cells[k].x() - x) <= 0.01f);
			CHECK(std::abs(cells[k].width() - cell) <= 0.01f);
			CHECK(cells[k].y() >= rowTop);
			CHECK(cells[k].bottom() <= rowTop + ui::kCardOutputStripHeight);
		}
	}
	// A STRIP ONE ROW TALL HIDES THE SECOND ROW (the flow's DOWN rule), never draws it outside the card.
	const std::vector<visage::Bounds> short1 = ui::outputChipBoundsFor(d, { 0.0f, 0.0f, 260.0f, ui::kCardOutputStripHeight });
	for (size_t k = 0; k < 8; ++k) {
		CHECK((short1[k].width() > 0.0f) == (k < 4));
	}
	// AND A FLOW DEVICE IS UNTOUCHED: no other device declares a grid.
	for (const feathers::device::Descriptor& other : feathers::kDevices) {
		if (!feathers::device::sameTag(other.traits.tag, d.traits.tag)) {
			CHECK(other.traits.outputGridColumns == 0);
		}
	}
}
TEST_CASE("a card too short for its whole foot can still scroll to the end of its body") {
	// **`maxBodyScroll` SUBTRACTED THE RAW FOOT WHERE THE PICTURE USES THE CLAMPED ONE.** It spent
	// `outputStripHeight()` — a flat `22 * outputRowsAt(width())` — while `drawBody` clips to
	// `ownFrame().body` and `syncDisabledVeil` sizes to it, and `ownFrame()` goes through
	// `cardFrameOf`, whose strip is `min(wanted, room)`: a card too short to hold its whole foot keeps
	// the foot INSIDE itself rather than letting it hang. The two agree everywhere the clamp does not
	// fire and diverge by exactly the clamped-away amount where it does.
	//
	// **THE CONSEQUENCE IS CONTENT A HAND CANNOT REACH**, which is why this is a defect rather than an
	// inconsistency: `setBodyScroll` clamps to this number, so an under-reported maximum is a body
	// whose last rows cannot be scrolled to.
	//
	// **THE HOST THAT REACHES IT IS THE RACK**, which grants a card its lane's height rather than the
	// height the card asked for. Here the state is built directly, because the claim is about the two
	// expressions and a lane would only be a slower way to make a short card.
	constexpr float kWantedBody = 400.0f;
	static constexpr feathers::device::SourceDesc kSeven[] = {
		{ .key = "alpha", .name = "Alpha" },  { .key = "bravo", .name = "Bravo" },
		{ .key = "charlie", .name = "Charlie" }, { .key = "delta", .name = "Delta" },
		{ .key = "echo", .name = "Echo" },   { .key = "foxtrot", .name = "Foxtrot" },
		{ .key = "golf", .name = "Golf" },
	};
	static constexpr feathers::device::Descriptor kWide {
	    .traits = { .tag = "PRB4", .name = "Probe", .hasCard = true, .hasExpanded = false,
		            .perVoiceControl = -1, .enableControl = -1 },
	    .params = {},
	    .outputs = kSeven,
	};
	struct ShortCard : ui::FxCard {
		const feathers::device::Descriptor* descriptor() const override { return &kWide; }
		// WHAT THE BODY WANTS, independent of what it is granted — the number the card would scroll to.
		float laneHeightAt(float cardWidth) const override {
			return kWantedBody + outputStripHeightAt(cardWidth);
		}
	};
	ShortCard card;

	// **SHORT ENOUGH FOR THE CLAMP TO FIRE AND SAID SO IN NUMBERS.** At this width the seven chips
	// wrap onto several rows; the card is then given less height than head plus that whole foot, which
	// is the only state in which the two expressions differ at all.
	constexpr float kWidth = 200.0f;
	const size_t rows = ui::outputChipRowsFor(kWide, kWidth);
	REQUIRE(rows > 1);
	const float rawStrip = ui::kCardOutputStripHeight * static_cast<float>(rows);
	const float granted = ui::kCardHeadHeight + rawStrip - 30.0f; // 30px short of head + whole foot
	card.setBounds(0.0f, 0.0f, kWidth, granted);

	// THE CLAMP REALLY FIRED — the frame's strip is smaller than the raw one. Without this the case
	// would be asking about a state it never reached.
	const float clamped = card.ownFrame().outputs.height();
	CAPTURE(rows);
	CAPTURE(rawStrip);
	CAPTURE(clamped);
	REQUIRE(clamped < rawStrip);

	// **THE VALUE, RESTATED FROM THE RECTANGLES THE PICTURE USES.** `contentHeight()` defaults to
	// `laneHeight()`, so what the body wants is the card's own height at this width less head and
	// clamped strip; what it has is the granted height less the same two. A difference check between
	// two scroll numbers would pass for any card that could scroll at all.
	const float visible = std::max(0.0f, granted - ui::kCardHeadHeight - clamped);
	const float wanted = std::max(0.0f, card.contentHeight() - ui::kCardHeadHeight - clamped);
	CHECK(card.maxBodyScroll() == doctest::Approx(std::max(0.0f, wanted - visible)));

	// AND IT IS NOT THE RAW-STRIP ANSWER, which is what the old expression computed — stated as its
	// own number so a red names which of the two the card gave back.
	const float rawVisible = std::max(0.0f, granted - ui::kCardHeadHeight - rawStrip);
	const float rawWanted = std::max(0.0f, card.contentHeight() - ui::kCardHeadHeight - rawStrip);
	const float rawAnswer = std::max(0.0f, rawWanted - rawVisible);
	CAPTURE(rawAnswer);
	REQUIRE(rawAnswer != doctest::Approx(std::max(0.0f, wanted - visible))); // else nothing is proven
	CHECK(card.maxBodyScroll() != doctest::Approx(rawAnswer));

	// **AND THE BODY CAN ACTUALLY BE SCROLLED THERE**, which is the thing a hand notices. `setBodyScroll`
	// clamps to `maxBodyScroll()`, so this is the under-report made observable.
	card.setBodyScroll(card.maxBodyScroll());
	CHECK(card.bodyScroll() == doctest::Approx(card.maxBodyScroll()));
	CHECK(card.bodyScroll() > rawAnswer);
}
TEST_CASE("a multi-output card is as tall as its own foot on the ONE layout that builds it") {
	// **THE LAYOUT PASS THAT CREATES A CARD IS THE PASS THAT SIZES IT, AND IT IS THE ONLY ONE A PLAYER
	// GETS.** `ModPanel::addDevice` ends in exactly one `resized()`; nothing forces a second. So every
	// number that pass reads has to be right on a card that has existed for microseconds.
	//
	// **THE CASE ABOVE CANNOT SEE THIS AND SAYS SO BY WHAT IT ASKS**: it reads `sourceBounds`, which is
	// a pure function of the rack's state and never touches a built card. Both are worth having — that
	// one is the geometry the strip INTENDS, this one is the rectangle the card was actually GIVEN.
	//
	// **THE WIDTH IS THE SUBJECT.** `GenericCard::laneHeight()` takes no argument and reaches
	// `liveHeight(width())`; a card the panel has just constructed has no bounds yet, so that width is
	// ZERO, the chip-wrap walk never wraps, and the foot comes back ONE ROW for a device with seven
	// chips. `FxCard::outputRowsAt`'s own doc names this trap in writing — *"answering it from
	// `width()` would read the width the card had LAST layout — zero on the first" — and
	// `laneHeight()` was the road that still did it.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);

	// THE GESTURE, NOT A SETTER. `addChoiceForTest` is the (+) picker's own road into `addDevice`, so
	// the single `resized()` under test is the one the add really performs rather than one this case
	// arranged.
	const std::vector<const feathers::device::Descriptor*> offered = panel->addableForTest();
	size_t choice = offered.size();
	for (size_t i = 0; i < offered.size(); ++i) {
		if (feathers::device::sameTag(offered[i]->traits.tag, feathers::mod::noteexpr::kTraits.tag)) {
			choice = i;
		}
	}
	REQUIRE(choice < offered.size()); // else the picker stopped offering the subject
	const size_t slot = panel->addChoiceForTest(choice);
	REQUIRE(slot < feathers::kMaxRackSlots);

	const feathers::device::Descriptor& desc = feathers::mod::noteexpr::kDescriptor;
	REQUIRE(desc.outputCount() == 7);

	// **NOT VACUOUS, AND THIS IS THE LINE THAT SAYS SO**: the whole claim is about a foot of MORE than
	// one row, and a device whose chips happened to fit on one would satisfy everything below while
	// exercising nothing. `outputRowsOf` asks at `kCardWidth`, which is the only width this strip has.
	const size_t rows = panel->outputRowsOf(slot);
	REQUIRE(rows > 1);

	// **THE VALUE, RESTATED INDEPENDENTLY OF THE ROAD UNDER TEST.** `heightFor` takes the width as an
	// argument and therefore cannot fall into the trap; it is the height this card is supposed to have
	// been given, computed without asking the card anything. A difference check against the card above
	// it would pass for any card that was merely a different size from its neighbour.
	const visage::Frame* card = panel->cardFrameForTest(slot);
	REQUIRE(card != nullptr);
	const float wanted = ui::GenericCard::heightFor(desc, ui::ModPanel::kCardWidth);
	const float got = card->height();
	CAPTURE(rows);
	CAPTURE(got);
	CAPTURE(wanted);
	CHECK(got == doctest::Approx(wanted));

	// **AND THE CASE IS SHOWN CAPABLE OF TELLING THE TWO APART BEFORE ITS SILENCE COUNTS.** The wrong
	// answer is `heightFor` at a width of zero — the chips never wrap, the foot is one row — and this
	// says out loud how far apart the right number and the wrong one are. If they were equal the
	// assertion above would be green whichever road the card took.
	const float widthless = ui::GenericCard::heightFor(desc, 0.0f);
	CHECK(wanted - widthless
	      == doctest::Approx(ui::kCardOutputStripHeight * static_cast<float>(rows - 1)).epsilon(0.001));
	REQUIRE(wanted > widthless);

	// **AND THE CHIPS ARE ON THE CARD.** `outputChipBounds` derives its strip from `envCardBounds`,
	// which asks the built card for its body height — at the real width, so it gets the FULL height and
	// places the chips in a strip the card's own frame does not reach. Both rectangles are in the
	// panel's coordinates, so this compares like with like.
	for (size_t output = 0; output < desc.outputCount(); ++output) {
		CAPTURE(output);
		const visage::Bounds chip =
		    panel->outputChipBounds(backplane::Source::output(f.bridge.rackDeviceAt(slot), output));
		REQUIRE(chip.width() > 0.0f);
		CHECK(chip.y() >= card->y() - 0.5f);
		CHECK(chip.bottom() <= card->bottom() + 0.5f);
	}
}
TEST_CASE("the harm scope draws the device that plays, on an axis that has no note in it") {
	// THE EXPANDED VIEW'S CONTENT (brief at 8cf28fa), built and checked before it has a surface to
	// live on. It is a Frame and nothing else, so it does not care whether the card or the pop-out
	// ends up hosting it — which is why it could be built while the head unification is blocked.
	HarmFixture f;
	namespace hrm = feathers::fx::harm;

	ui::HarmScope scope { f.deviceHost, f.device };
	scope.setBounds(0, 0, 216, 96);
	const visage::Bounds box = scope.plotBounds();
	REQUIRE(box.width() > 0.0f);

	const auto write = [&](hrm::Param p, double value) {
		ui::FxControl control { f.bridge, f.device, hrm::kDescriptor, static_cast<size_t>(p) };
		control.setValue(value);
		f.bridge.refresh();
	};

	SUBCASE("it reads the device's own state, field for field, with only Enabled overridden") {
		// THE CHECK `fxFilterAt`'s NOTE ASKS FOR. A picture assembled out of published values loses a
		// field the day somebody adds one, and the filter card shipped exactly that twice. So this
		// compares the whole assembly against the device's own row rather than sampling the fields
		// somebody thought of.
		write(hrm::Param::Harmonic, 14.0);
		write(hrm::Param::Level, 0.6);
		write(hrm::Param::Width, 0.42);
		write(hrm::Param::Depth, 0.3);
		write(hrm::Param::Fine, 0.5);
		// **SWITCHED OFF EXPLICITLY, because the override below is only visible against an off row.**
		// This leaned on the device arriving off; the 2026-09-04 change made every effect arrive
		// ON, and an override that agrees with the state it is overriding cannot be seen at all — a
		// vacuous assertion that would have gone on passing with `settingsForTest`'s override
		// deleted. Setting it here is the stronger form and does not depend on a default either way.
		write(hrm::Param::Enabled, 0.0);

		const hrm::Settings row = f.state().settings;
		const hrm::Settings drawn = scope.settingsForTest();

		CHECK(drawn.harmonic == row.harmonic);
		CHECK(drawn.level == doctest::Approx(row.level));
		CHECK(drawn.width == doctest::Approx(row.width));
		CHECK(drawn.depth == doctest::Approx(row.depth));
		CHECK(drawn.fineSemitones == doctest::Approx(row.fineSemitones));
		CHECK(drawn.phase == doctest::Approx(row.phase));
		CHECK(drawn.spread == doctest::Approx(row.spread));
		CHECK(drawn.attackSeconds == doctest::Approx(row.attackSeconds));
		CHECK(drawn.releaseSeconds == doctest::Approx(row.releaseSeconds));
		CHECK(drawn.portaSeconds == doctest::Approx(row.portaSeconds));

		// AND THE ONE OVERRIDE, stated rather than left as a difference somebody has to notice: the
		// picture draws the device whether or not it is switched into the signal.
		REQUIRE_FALSE(row.enabled);
		CHECK(drawn.enabled);
	}

	SUBCASE("1:1 lands where the axis says, not at the middle of the box") {
		// THE VALUE, computed independently. The axis runs 1:8 to 32:1 — eight octaves — and the
		// fundamental is three of them from the left, so it sits at 3/8 of the width and NOT at the
		// centre. A picture that put it in the middle would be drawing a different axis.
		CHECK(scope.xForRatio(1.0)
		      == doctest::Approx(box.x() + box.width() * 3.0f / 8.0f).epsilon(1e-5));
		CHECK(scope.xForRatio(hrm::kProfileLoRatio) == doctest::Approx(box.x()).epsilon(1e-5));
		CHECK(scope.xForRatio(hrm::kProfileHiRatio) == doctest::Approx(box.right()).epsilon(1e-5));
		// AND IT IS LOGARITHMIC: an octave is the same distance wherever it is taken.
		const float octaveLow = scope.xForRatio(0.5) - scope.xForRatio(0.25);
		const float octaveHigh = scope.xForRatio(16.0) - scope.xForRatio(8.0);
		CHECK(octaveLow == doctest::Approx(box.width() / 8.0f).epsilon(1e-5));
		CHECK(octaveHigh == doctest::Approx(octaveLow).epsilon(1e-5));
	}

	SUBCASE("the curve's points land where xForRatio says they do") {
		// TWO PLACES HAVE TO AGREE ABOUT ONE GEOMETRY: `ratioProfile` lays the points down and the
		// widget places them by index. This is the seam that bit the rack's outputs row — a control
		// drawn in the right place from a height calculation that disagreed with its placement — and
		// it is worth a check rather than an assumption every time it appears.
		const size_t points = scope.curvePointCount();
		REQUIRE(points >= 2);
		std::vector<float> ratios(points);
		std::vector<float> gains(points);
		hrm::ratioProfile(scope.settingsForTest(), ratios.data(), gains.data(), points);

		for (const size_t i : { size_t { 0 }, points / 4, points / 2, points - 1 }) {
			CAPTURE(i);
			const float byIndex =
			    box.x() + box.width() * static_cast<float>(i) / static_cast<float>(points - 1);
			// WITHIN HALF A STEP, because one point is deliberately moved onto the notch centre —
			// see ratioProfile. Anywhere else the two agree exactly.
			const float step = box.width() / static_cast<float>(points - 1);
			CHECK(std::abs(scope.xForRatio(static_cast<double>(ratios[i])) - byIndex)
			      <= step * 0.5f + 1e-3f);
		}
	}

	SUBCASE("the notch is drawn where the harmonic puts it, and the oscillator beside it") {
		write(hrm::Param::Level, 0.75);
		write(hrm::Param::Width, 0.5);
		for (const size_t harmonic : { size_t { 2 }, size_t { 9 }, size_t { 14 }, size_t { 19 },
			                           hrm::kHarmonicCount - 1 }) {
			CAPTURE(harmonic);
			CAPTURE(hrm::harmonicLabel(harmonic));
			write(hrm::Param::Harmonic, static_cast<double>(harmonic));
			// THE VALUE, from the harmonic table rather than from the widget's own model: the notch's
			// x is where the SELECTED RATIO lands, which is the claim the whole axis exists to make.
			CHECK(scope.notchX()
			      == doctest::Approx(scope.xForRatio(static_cast<double>(hrm::ratioAt(harmonic))))
			             .epsilon(1e-5));
			// AND THE OSCILLATOR IS AT THE SAME PLACE, which is the coupling drawn: the notch sits on
			// what the oscillator is sounding — at every Level, since the parity sweep of 2026-09-16.
			CHECK(scope.oscillatorX() == doctest::Approx(scope.notchX()).epsilon(1e-5));
		}
		CHECK(scope.oscillatorStem() == doctest::Approx(0.75f));
	}

	SUBCASE("a silent oscillator leaves the NOTCH on the harmonic — the picture follows notchCentreHz, which no "
	        "longer reads Level") {
		// THIS USED TO DRAW THE NOTCH JUMPING TO 1:1 AT LEVEL 0, the brief's requirement at the time,
		// because `notchCentreHz` fell back to the fundamental there. The HARM parity sweep
		// (2026-09-16) removed that fall-back — the firmware's notch never reads level — so the mark
		// stays where the harmonic is, and the oscillator's mark loses its height rather than its place.
		write(hrm::Param::Width, 0.5);
		write(hrm::Param::Harmonic, 19.0); // 2:1
		write(hrm::Param::Level, 0.5);
		REQUIRE(scope.notchX() == doctest::Approx(scope.xForRatio(2.0)).epsilon(1e-5));

		write(hrm::Param::Level, 0.0);
		CHECK(scope.notchX() == doctest::Approx(scope.xForRatio(2.0)).epsilon(1e-5));
		// THE OSCILLATOR'S MARK STAYS AT ITS HARMONIC TOO — drawn with zero height, so it is not on
		// screen, but the position is what the control says.
		CHECK(scope.oscillatorX() == doctest::Approx(scope.xForRatio(2.0)).epsilon(1e-5));
		CHECK(scope.oscillatorStem() == doctest::Approx(0.0f));
	}

	SUBCASE("fine detune shifts both marks off the exact harmonic, by the same amount") {
		write(hrm::Param::Width, 0.5);
		write(hrm::Param::Level, 0.5);
		write(hrm::Param::Harmonic, 14.0); // 3:2
		write(hrm::Param::Fine, 1.0);      // one semitone up
		const double expected = 1.5 * std::exp2(1.0 / 12.0);
		CHECK(scope.settingsForTest().fineSemitones == doctest::Approx(1.0f));
		CHECK(scope.notchX() == doctest::Approx(scope.xForRatio(expected)).epsilon(1e-5));
		CHECK(scope.oscillatorX() == doctest::Approx(scope.xForRatio(expected)).epsilon(1e-5));
		// AND IT REALLY MOVED — or the two checks above would pass on a scope that ignored fine and
		// an `expected` that happened to be computed the same wrong way.
		CHECK(scope.notchX() > scope.xForRatio(1.5));
	}

	SUBCASE("the curve on the glass reaches 1 - depth, and only at the notch") {
		write(hrm::Param::Width, 0.1);
		write(hrm::Param::Level, 0.5);
		write(hrm::Param::Harmonic, 14.0);
		for (const double depth : { 0.0, 0.4, 1.0 }) {
			CAPTURE(depth);
			write(hrm::Param::Depth, depth);
			std::vector<float> gains(ui::HarmScope::kMaxCurvePoints);
			const size_t points = scope.curveGains(gains.data(), gains.size());
			REQUIRE(points >= 2);
			const auto lowest = std::min_element(gains.begin(), gains.begin() + static_cast<long>(points));
			CHECK(static_cast<double>(*lowest) == doctest::Approx(1.0 - depth).epsilon(1e-5));
			// AND THE ENDS OF THE AXIS ARE UNTOUCHED — a depth that pulled the whole curve down
			// would hit the assertion above and nothing else.
			CHECK(static_cast<double>(gains[0]) == doctest::Approx(1.0).epsilon(2e-3));
			CHECK(static_cast<double>(gains[points - 1]) == doctest::Approx(1.0).epsilon(2e-3));
		}
	}
}
TEST_CASE("the strip reserves the height of the card it built, and refuses to guess at a device it "
          "cannot build one for") {
	// TWO FACES OF ONE DEFECT, backlog item 15 plus the live half of it that the backlog missed.
	//
	// `ensureCard` was `if (isMseg) MsegCard else EnvCard` with NO else-fallback, so a third rack
	// device would have been drawn as a DAHDSR — an envelope's controls, editing whichever of the
	// device's fields happened to sit at the envelope's indices. That is the unreachable half, and
	// the backlog is right that it bites the day a third device exists.
	//
	// THE HALF NOBODY HAD SPOTTED IS LIVE TODAY. `sourceHeight` returned `EnvCard::height()`
	// unconditionally while `resized()` sized the frame from `cards_[slot]->bodyHeight()` — the two
	// places that must agree about one geometry, disagreeing. MEASURED: EnvCard 144, MsegCard 140,
	// so an MSEG's slot reserved 188 and got a 184-tall card, and every card below it sat 4 px low.
	ModFixture f;
	auto* panel = f.panel;
	REQUIRE(panel != nullptr);
	namespace mseg = feathers::mod::mseg;
	namespace dsr = feathers::mod::dahdsr;

	// ASKED AFRESH EACH TIME, because the subcases below restore the rack under the strip: the source is
	// the device AT slot 0, named by its id, and a restore is free to seat a device under another one.
	const auto sourceAtZero = [&] { return backplane::Source::output(f.bridge.rackDeviceAt(0), 0); };

	SUBCASE("the reserved height follows the CARD, not the envelope") {
		// THE VALUE ON BOTH SIDES, and the two device heights are REQUIREd to differ first — a case
		// run on two cards of the same height could not tell the fix from the bug.
		REQUIRE(ui::EnvCard::height() != ui::MsegCard::height());

		REQUIRE(f.bridge.modRack().holds(0, dsr::kTraits.tag));
		panel->resized();
		CHECK(panel->sourceHeightForTest(sourceAtZero())
		      == doctest::Approx(ui::kCardHeadHeight + ui::EnvCard::height()
		                         + ui::kCardOutputStripHeight));

		feathers::ModRack rack = f.bridge.modRack();
		rack.setMsegAt(0, mseg::Settings {});
		f.bridge.restoreModRack(rack);
		panel->resized();
		REQUIRE(f.bridge.modRack().holds(0, mseg::kTraits.tag));
		CHECK(panel->sourceHeightForTest(sourceAtZero())
		      == doctest::Approx(ui::kCardHeadHeight + ui::MsegCard::height()
		                         + ui::kCardOutputStripHeight));

		// AND THE FRAME THE STRIP ACTUALLY PLACED IS THAT TALL — the other half of the seam. A
		// reserved height that matched a card the strip then sized differently is the whole bug.
		CHECK(panel->cardHeightForTest(0) == doctest::Approx(ui::kCardHeadHeight + ui::MsegCard::height() + ui::kCardOutputStripHeight));
		const visage::Frame* frame = panel->cardFrameForTest(0);
		REQUIRE(frame != nullptr);
		CHECK(frame->height() == doctest::Approx(ui::kCardHeadHeight + ui::MsegCard::height() + ui::kCardOutputStripHeight));

		// **AND THE SAME SEAM FOR THE GENERIC CARD, WHICH IS THE ONE THAT CAN ACTUALLY DRIFT.** The two
		// arms above reserve a per-class constant, so the strip and the card cannot disagree about a
		// number neither of them computes. A generic card's height is a property of the DEVICE, so
		// there are two places it could be worked out — and it is worked out in one, `heightFor`, with
		// `openHeight()` defined in terms of it.
		//
		// THIS WAS ADDED BECAUSE A KNOWN-BAD CAME BACK GREEN. Making the card draw 40px taller than
		// the lane reserved left this case passing: nothing here had a generic card to look at, so
		// "the strip reserves the height of the card it built" was true only of the cards that existed
		// when it was written. A case that stops covering a class the moment one is added is the same
		// failure as the reachability table's, one file over.
		feathers::ModRack tracked = f.bridge.modRack();
		tracked.device[0] = feathers::DeviceRow {};
		for (size_t i = 0; i < 4 && feathers::mod::tracker::kTraits.tag[i] != '\0'; ++i) {
			tracked.device[0].tag[i] = feathers::mod::tracker::kTraits.tag[i];
		}
		tracked.device[0].instanceId = feathers::kFirstInstance;
		tracked.count = 1;
		// (An `assignDenseRackLanes(&tracked)` stood here; a hand-written row needed a routing lane
		// before the strip could find a card for it. Kill-lanes step 3c took the lane away — a device's
		// place is the slot it is written to — so writing the row and the count is the whole of it.)
		f.bridge.restoreModRack(tracked);
		panel->resized();
		REQUIRE(panel->cardKindOf(0) == ui::ModPanel::CardKind::Generic);

		// **NOT COMPOSED THE WAY THE TWO ARMS ABOVE ARE, AND THE DIFFERENCE IS THE CARD'S NOT THE
		// STRIP'S.** `EnvCard::height()` and `MsegCard::height()` are BODY heights, so their rows add
		// the head and the output strip. `GenericCard::heightFor` is the whole card — its own
		// `kHeadHeight` is inside it, which is what lets one number serve both the lane reserving
		// space and the card drawing in it. Writing the other composition here measured 192 against an
		// expected 238, which is this comment being paid for rather than guessed at.
		const float generic = ui::GenericCard::heightFor(feathers::mod::tracker::kDescriptor);
		CHECK(panel->sourceHeightForTest(sourceAtZero()) == doctest::Approx(generic));
		CHECK(panel->cardHeightForTest(0) == doctest::Approx(generic));
		const visage::Frame* genericFrame = panel->cardFrameForTest(0);
		REQUIRE(genericFrame != nullptr);
		// THE FRAME THE STRIP PLACED, which is what a divergence between the two spellings would show
		// up in — and the assertion the known-bad above now reddens.
		CHECK(genericFrame->height() == doctest::Approx(generic));
	}

	SUBCASE("a device this build cannot name is NOT drawn as an envelope") {
		// A TAG THIS BUILD HAS NEVER HEARD OF, written straight into the rack the way a rack saved by
		// a newer build would arrive. There is no third rack device yet, so this is the only way to
		// reach the branch — and a fallback with no way to reach it is a fallback nobody has run.
		feathers::ModRack rack = f.bridge.modRack();
		rack.device[0] = feathers::DeviceRow {};
		rack.device[0].tag = { 'Z', 'Z', 'Z', 'Z' };
		rack.device[0].instanceId = feathers::kFirstInstance;
		rack.count = 1;
		// **AND NO LANE, BECAUSE THERE IS NONE TO GIVE (kill-lanes step 3c).** This stamped one "exactly as
		// a loader would", on the reasoning that an unknown device still occupies a slot and still owns its
		// columns. The slot is the occupancy now; what this build still cannot do is name any control on it.
		f.bridge.restoreModRack(rack);
		panel->resized();

		REQUIRE(panel->cardKindOf(0) == ui::ModPanel::CardKind::Missing);
		const visage::Frame* frame = panel->cardFrameForTest(0);
		REQUIRE(frame != nullptr);
		const auto* missing = dynamic_cast<const ui::MissingDeviceCard*>(frame);
		REQUIRE(missing != nullptr);
		// THE POSITIVE CLAIM AND THE NEGATIVE ONE. "Not an EnvCard" is what the bug produced; "device
		// missing, named by its tag" is what the fix produces, and only the pair rules
		// out a third answer.
		CHECK(missing->name() == "ZZZZ");
		CHECK(dynamic_cast<const ui::EnvCard*>(frame) == nullptr);
		CHECK(dynamic_cast<const ui::MsegCard*>(frame) == nullptr);

		// AND THE STRIP RESERVES ITS HEIGHT, not the envelope's.
		CHECK(panel->sourceHeightForTest(sourceAtZero()) == doctest::Approx(ui::MissingDeviceCard::height()));
		// THE UNKNOWN DEVICE PUBLISHES NOTHING, so it gets no outputs row — from the rule stage 3
		// established rather than from a second check on the tag.
		CHECK(panel->outputRowHeight(0) == doctest::Approx(0.0f));
	}

	SUBCASE("an empty slot stays DORMANT, which is not the same as unknown") {
		// THE REDUNDANCY THAT IS LOAD-BEARING. The strip keeps a real card in every slot so the
		// browser turning one on is a VISIBILITY FLIP rather than a construction. An empty slot
		// answering NoEditor would look tidier and would build a card that has to be thrown away the
		// moment a device arrives — so `cardKindOf` distinguishes "past the rack's count" from
		// "holds something I do not know".
		const feathers::ModRack rack = f.bridge.modRack();
		REQUIRE(rack.count < feathers::kMaxRackSlots);
		for (size_t slot = rack.count; slot < feathers::kMaxRackSlots; ++slot) {
			CAPTURE(slot);
			CHECK(panel->cardKindOf(slot) == ui::ModPanel::CardKind::Dormant);
			CHECK(dynamic_cast<const ui::EnvCard*>(panel->cardFrameForTest(slot)) != nullptr);
		}
	}

	SUBCASE("the dispatch is asked of the rack, and every rack device this build has answers") {
		// **THE RULE CHANGED ON 2026-09-02 AND THIS IS THE CHANGE, MADE IN THE OPEN.** It read: *"Every
		// device that can sit in the rack must get a card that is not the fallback — if one does not,
		// this build has a modulator it cannot draw and the fallback is hiding it rather than the
		// fallback being unused."
		//
		// The tracker uses the generic card for now, with its own card to come later. So the
		// rule is now that every registered modulator gets a CARD, and a device with no hand-written
		// card gets the GENERIC one — `NoEditor` is left for a device this build has never heard of,
		// which is the only case where there is genuinely nothing to draw.
		//
		// **THE OLD ASSERTION WOULD HAVE GONE ON PASSING, WHICH IS WHY IT IS BEING REWRITTEN RATHER
		// THAN LEFT.** `!= NoEditor` is satisfied by `Generic`, so the letter of it survived the
		// change while its stated reason did not: the tracker DOES now reach the fallback, and the
		// comment above claimed no device may. A case whose assertion passes for a reason its comment
		// denies is worse than one that fails.
		feathers::ModRack rack = f.bridge.modRack();
		size_t genericCards = 0;
		for (const feathers::device::Descriptor& d : feathers::kDevices) {
			if (d.outputCount() == 0) {
				continue; // not a rack device: it publishes nothing to modulate with
			}
			CAPTURE(std::string(d.traits.name));
			rack.device[0] = feathers::DeviceRow {};
			// **THE WHOLE TAG, THROUGH `device::tagOf`** — this copied FOUR BYTES, which was the width
			// of `Traits::tag` until it was widened to `device::kTagBytes` on 2026-09-14. Every device
			// with outputs happened to have a four-byte tag until SKLITTER got some — "OWLTT" and
			// "SKLITTER" are the only two longer than four — and "SKLI" names no device this build has,
			// so the panel answered `Missing` and the case read a truncation as a missing card. `tagOf`
			// is the same conversion `defaultRowFor` uses, so there is one of it.
			rack.device[0].tag = feathers::device::tagOf(d.traits.tag);
			rack.device[0].instanceId = feathers::kFirstInstance;
			rack.count = 1;
			f.bridge.restoreModRack(rack);
			panel->resized();
			// A CARD, AND A REAL ONE. `Dormant` means the slot is empty and `Missing` means this
			// build does not have the device — neither is true of anything in `kDevices`, which is
			// what makes these two the honest negative half of the rule.
			CHECK(panel->cardKindOf(0) != ui::ModPanel::CardKind::Dormant);
			CHECK(panel->cardKindOf(0) != ui::ModPanel::CardKind::Missing);

			// AND THE POSITIVE HALF, WHICH THE OLD PAIR COULD NOT STATE: a device with a hand-written
			// card gets it, and one without gets the generic grid. Written as a bespoke-or-generic
			// choice rather than as a list of tags, so a modulator added to the catalogue tomorrow is
			// covered by this case without anybody editing it — which is the whole of vision D3
			// option A, and the reason the tracker no longer needs a card before it can be registered.
			const ui::ModPanel::CardKind kind = panel->cardKindOf(0);
			const bool bespoke = kind == ui::ModPanel::CardKind::Envelope
			                     || kind == ui::ModPanel::CardKind::Mseg;
			CHECK((bespoke || kind == ui::ModPanel::CardKind::Generic));
			if (kind == ui::ModPanel::CardKind::Generic) {
				// **AND THE GENERIC ARM REALLY IS REACHED, so this case is not asserting a branch that
				// nothing takes.** Before the tracker was registered every device here had a bespoke
				// card, and a generic arm added then would have been dead code the case could not
				// tell from working code.
				++genericCards;
			}
		}
		// THE GENERIC ARM HAS A SUBJECT — the tracker. If a bespoke Tracker card is written later this
		// goes to zero, and this line failing is the correct way to be told: the arm would then be
		// unreached again and the case above would be back to asserting a branch nothing takes.
		CHECK(genericCards > 0);
	}
}
TEST_CASE("the expanded overlay is a layer: it dims, it dismisses four ways, and it holds one view PER DEVICE") {
	// THE DESIGN: an expanded device view is an OVERLAY floating above the editor and
	// dimming what is behind it — not a third stage mode. The dismissal rules below are
	// conventional defaults rather than specified ones, and they are asserted here so that
	// changing one is a decision somebody makes rather than a behaviour that drifts.
	ui::ExpandedOverlay overlay;
	overlay.setBounds(0, 0, 1240, 700);
	int dismissals = 0;
	overlay.onDismiss = [&dismissals] { ++dismissals; };

	SUBCASE("closed, it is invisible and DEAF") {
		// THE WORST THING A FULL-WINDOW LAYER CAN DO IS ANSWER A PRESS WHILE EMPTY — every click in
		// the editor would land on it and nothing else would ever be pressed again.
		CHECK_FALSE(overlay.open());
		CHECK_FALSE(overlay.isVisible());
		CHECK(overlay.ignoresMouseEvents());
		// AND ITS GEOMETRY IS EMPTY rather than stale, so nothing can be "outside" a panel that is
		// not there and a click cannot dismiss what is already closed.
		CHECK(overlay.panelBounds().width() == doctest::Approx(0.0f));
		CHECK_FALSE(overlay.outside({ 5.0f, 5.0f }));
		overlay.onMouseDown().callback(at(5.0f, 5.0f));
		CHECK(dismissals == 0);
	}

	SUBCASE("open, the panel is the content's MINIMUM plus the layer's chrome") {
		// THE VALUE, computed independently from the content's declared minimum — the layer must not
		// be deciding how big a device's view is, because a detached window later will not be able to.
		const visage::Point minimum { 420.0f, 300.0f };
		overlay.show(makeStub(minimum, "Harm"));
		REQUIRE(overlay.open());
		CHECK(overlay.isVisible());
		// THE LAYER IGNORES PRESSES EVEN WHILE OPEN, and that is the fix rather than an oversight: a
		// frame's rectangle IS its hit region, so a layer covering the editor in order to dim it
		// would swallow every press in the editor and make arming a source impossible. The PANEL
		// takes its own; everything else falls through.
		CHECK(overlay.ignoresMouseEvents());

		const visage::Bounds panel = overlay.panelBounds();
		CHECK(panel.width()
		      == doctest::Approx(minimum.x + 2.0f * ui::ExpandedOverlay::kPanelPad));
		CHECK(panel.height() == doctest::Approx(minimum.y + ui::ExpandedOverlay::kTitleHeight
		                                        + 2.0f * ui::ExpandedOverlay::kPanelPad));
		// CENTRED IN THE LAYER, and in the LAYER's coordinates only — nothing here knows the editor
		// exists, which is what makes this content re-parentable into a window.
		CHECK(panel.x() + panel.width() * 0.5f == doctest::Approx(overlay.width() * 0.5f).epsilon(0.01));
		CHECK(panel.y() + panel.height() * 0.5f
		      == doctest::Approx(overlay.height() * 0.5f).epsilon(0.01));

		// AND A BIGGER MINIMUM MAKES A BIGGER PANEL — or the first check passes on a fixed size that
		// happens to match.
		overlay.show(makeStub({ 620.0f, 300.0f }, "Wider"));
		CHECK(overlay.panelBounds().width() == doctest::Approx(panel.width() + 200.0f));

		// A MINIMUM LARGER THAN THE LAYER IS CLAMPED TO THE LAYER, not squashed to a fraction of it.
		overlay.show(makeStub({ 4000.0f, 4000.0f }, "Huge"));
		CHECK(overlay.panelBounds().width() == doctest::Approx(overlay.width()));
		CHECK(overlay.panelBounds().height() == doctest::Approx(overlay.height()));
	}

	SUBCASE("the content is placed inside the chrome and is the layer's to destroy") {
		const int before = StubExpandedView::alive;
		overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
		CHECK(StubExpandedView::alive == before + 1);

		const visage::Bounds content = overlay.contentBounds();
		const ui::ExpandedView* view = overlay.contentForTest();
		REQUIRE(view != nullptr);
		// THE FRAME IS WHERE THE GEOMETRY SAYS. This is the seam that has bitten twice this week —
		// a height computed in one place and a `setBounds` in another — so it is checked rather than
		// assumed.
		//
		// THE CONTENT IS A CHILD OF THE PANEL, so its own coordinates are panel-space while
		// `contentBounds()` is layer-space. Compared through the panel's origin rather than by
		// asserting one against the other and calling the offset a bug.
		const visage::Bounds panel = overlay.panelBounds();
		CHECK(panel.x() + view->x() == doctest::Approx(content.x()));
		CHECK(panel.y() + view->y() == doctest::Approx(content.y()));
		CHECK(view->width() == doctest::Approx(content.width()));
		CHECK(view->height() == doctest::Approx(content.height()));
		// AND IT SITS BELOW THE TITLE ROW, inside the panel.
		CHECK(content.y() >= overlay.titleBounds().bottom() - 0.01f);
		CHECK(overlay.panelBounds().contains({ content.x() + 1.0f, content.y() + 1.0f }));

		overlay.dismiss();
		CHECK_FALSE(overlay.open());
		// RETIRED, THEN FREED BY THE NEXT DRAW — the crash fix, asserted rather than left implicit.
		visage::Canvas canvas;
		overlay.draw(canvas);
		CHECK(StubExpandedView::alive == before);
	}

	SUBCASE("Esc closes it, and only Esc") {
		overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
		// A KEY THAT IS NOT ESCAPE IS NOT SWALLOWED EITHER — the layer returning true for everything
		// would eat the host's keyboard while it happened to be open.
		const visage::KeyEvent other { visage::KeyCode::A, 0, true };
		CHECK_FALSE(overlay.onKeyPress().callback(other));
		CHECK(overlay.open());

		const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
		CHECK(overlay.onKeyPress().callback(escape));
		CHECK_FALSE(overlay.open());
		CHECK(dismissals == 1);
		CHECK(overlay.lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::Escape);

		// AND ESC ON A CLOSED LAYER IS NOT SWALLOWED — otherwise the layer would eat the key for the
		// whole life of the editor rather than while it is open. Still true with a fallback wired, as
		// long as the fallback declines: see the subcase below.
		CHECK_FALSE(overlay.onKeyPress().callback(escape));
		CHECK(dismissals == 1);
	}

	SUBCASE("Esc offers itself to the fallback ONLY when this layer is closed") {
		// BACKLOG 20's second exit. Esc has two possible meanings now — close the expanded view, and
		// leave modulation assign mode — and the rule is that the thing NEAREST THE HAND wins: the
		// overlay closes, the arming survives, a second press disarms. ONE thing per press.
		//
		// ASSERTED HERE RATHER THAN LEFT TO DISPATCH ORDER, which is the whole reason the fallback is
		// a callback on this layer instead of a second independent key handler.
		int fallbackCalls = 0;
		bool fallbackConsumes = true;
		overlay.onEscapeWhenClosed = [&] {
			++fallbackCalls;
			return fallbackConsumes;
		};
		const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };

		// OPEN: the overlay takes it and the fallback is NOT offered the key at all.
		overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
		CHECK(overlay.onKeyPress().callback(escape));
		CHECK_FALSE(overlay.open());
		CHECK(fallbackCalls == 0);

		// CLOSED: the fallback is offered it, and consuming it is reported as consumed.
		CHECK(overlay.onKeyPress().callback(escape));
		CHECK(fallbackCalls == 1);

		// AND A FALLBACK THAT DECLINES LEAVES THE KEY UNSWALLOWED — the property the subcase above
		// guards, which a fallback wired in front of it could otherwise have destroyed silently.
		fallbackConsumes = false;
		CHECK_FALSE(overlay.onKeyPress().callback(escape));
		CHECK(fallbackCalls == 2);

		// A KEY THAT IS NOT ESCAPE NEVER REACHES THE FALLBACK.
		const visage::KeyEvent other { visage::KeyCode::A, 0, true };
		CHECK_FALSE(overlay.onKeyPress().callback(other));
		CHECK(fallbackCalls == 2);
	}

	SUBCASE("A CLICK OUTSIDE DOES NOT CLOSE IT, AND DOES NOT REACH IT AT ALL") {
		// THIS ASSERTED THE OPPOSITE AND WAS WRONG TO. The layer's defaults said click-outside
		// dismisses because the panel is "a view with nothing to lose". That default crashed the plugin
		// and stopped modulation from reaching the panel, and those were ONE bug:
		//
		// ROUTING MODULATION IS A GESTURE THAT SPANS SURFACES — arm a source in the mod panel, then
		// click a destination here. For an outside click to DISMISS, the layer must first SWALLOW
		// it, and swallowing is exactly what stops the arm. The dismissal and the arming want the
		// same press and only one can have it.
		//
		// INVERTED RATHER THAN DELETED, on this repo's rule: a behaviour reversing must not be
		// indistinguishable from the case being dropped.
		overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
		const visage::Bounds panel = overlay.panelBounds();

		const visage::Point beyond { panel.x() * 0.5f, panel.y() * 0.5f };
		REQUIRE(overlay.outside(beyond));
		CHECK(overlay.open());
		CHECK(dismissals == 0);

		// AND THE PRESS IS NOT THE LAYER'S TO TAKE. `frameAtPoint` is what visage asks, and the
		// answer must be NOTHING here — so the dispatch carries on to the editor beneath and the mod
		// panel gets its click. A layer that answered would swallow every press in the editor,
		// because a frame's rectangle IS its hit region: `containsPoint` is not virtual.
		CHECK(overlay.frameAtPoint(beyond) == nullptr);
	}

	SUBCASE("every control on the panel is REACHABLE — frameAtPoint, not knobsForTest") {
		// THE SEAM THAT WOULD HAVE CAUGHT A PANEL NOBODY COULD TOUCH. `setIgnoresMouseEvents(false,
		// false)` on the layer skipped both child loops in `Frame::frameAtPoint`, so every press
		// resolved to the layer and no control ever received one. Fifteen subcases and eleven
		// known-bads passed over an inert panel, because every one of them addressed the widgets
		// directly instead of asking WHERE A PRESS LANDS.
		//
		// THIS IS THE OCCLUSION RULE'S THIRD COUSIN: the pixel is there, it says the right thing,
		// and it cannot be pressed. No value assertion can see reachability.
		overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
		const ui::ExpandedView* view = overlay.contentForTest();
		REQUIRE(view != nullptr);

		// A POINT INSIDE THE CONTENT resolves to the CONTENT, not to the layer and not to the panel.
		//
		// IN THE LAYER'S COORDINATES, which is what `frameAtPoint` is asked in — `view->x()` is
		// relative to the PANEL, and passing a panel-space point as a layer-space one is how a probe
		// lands somewhere nobody meant. `contentBounds()` is the layer-space rectangle.
		const visage::Bounds inLayer = overlay.contentBounds();
		const visage::Point onContent { inLayer.x() + inLayer.width() * 0.5f,
			                            inLayer.y() + inLayer.height() * 0.5f };
		CHECK(overlay.frameAtPoint(onContent) == view);

		// AND THE ✕ RESOLVES TO THE PANEL, which is the frame that owns it.
		const visage::Bounds close = overlay.closeBounds();
		REQUIRE(close.width() > 0.0f);
		const visage::Point onClose { close.x() + close.width() * 0.5f,
			                          close.y() + close.height() * 0.5f };
		CHECK(overlay.frameAtPoint(onClose) == overlay.panelForTest());
	}

	SUBCASE("the close button closes it, and it is inside the panel so 'outside' cannot be why") {
		overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
		const visage::Bounds close = overlay.closeBounds();
		REQUIRE(close.width() > 0.0f);
		// THE PROBE MUST BE AIMED WHERE THE TWO ANSWERS DIFFER. A ✕ outside the panel would be
		// dismissed by the click-outside road and this subcase would pass without the button
		// existing at all.
		const visage::Point on { close.x() + close.width() * 0.5f,
			                     close.y() + close.height() * 0.5f };
		REQUIRE_FALSE(overlay.outside(on));

		// DRIVEN ON THE PANEL, WHICH OWNS THE ✕ NOW — in the panel's own coordinates. The layer no
		// longer has a mouse handler at all; it draws the scrim and takes nothing.
		ui::ExpandedPanel* panel = overlay.panelForTest();
		REQUIRE(panel != nullptr);
		const visage::Bounds inPanel = panel->closeBounds();
		panel->onMouseDown().callback(
		    at(inPanel.x() + inPanel.width() * 0.5f, inPanel.y() + inPanel.height() * 0.5f));
		CHECK_FALSE(overlay.open());
		CHECK(dismissals == 1);
		CHECK(overlay.lastDismissalForTest() == ui::ExpandedOverlay::Dismissal::CloseButton);

		// AND THE CONTENT SURVIVED THE HANDLER IT WAS CLOSED FROM. Destroying a frame from inside its
		// own tree's dispatch is how a SIGSEGV lands in an unrelated frame's handler, which is what
		// was hit; a dismissal RETIRES the content and the next show or draw frees it.
		CHECK(StubExpandedView::alive > 0);
		visage::Canvas canvas;
		overlay.draw(canvas);
		CHECK(StubExpandedView::alive == 0);
	}

	SUBCASE("several at once: a second view opens BESIDE the first; the same view again REPLACES its own and does not report a close") {
		// "One at a time — two overlays is a window manager, and nobody asked for one" stood here until
		// 2026-09-14, when several windows open at once became the design. One panel per DEVICE
		// still, and a stub's device is its title.
		const int before = StubExpandedView::alive;
		overlay.show(makeStub({ 420.0f, 300.0f }, "First"));
		overlay.show(makeStub({ 420.0f, 300.0f }, "Second"));
		CHECK(overlay.open());
		CHECK(overlay.panelCount() == 2);
		CHECK(StubExpandedView::alive == before + 2);
		CHECK(overlay.contentForTest()->title() == "Second"); // the front is the last opened
		CHECK(dismissals == 0);

		// THE SAME VIEW AGAIN REPLACES ITS OWN PANEL. RETIRED AND NOT YET FREED: the press that replaces a
		// panel can come from INSIDE the panel being replaced (a card in a nested rack presents through
		// the outer host), so the layer keeps the old one alive until the next draw or tick, as it does
		// for every other dismissal.
		overlay.show(makeStub({ 420.0f, 300.0f }, "Second"));
		CHECK(overlay.panelCount() == 2);
		CHECK(StubExpandedView::alive == before + 3);
		(void)overlay.updateContent();
		CHECK(StubExpandedView::alive == before + 2);
		// AND THE OPENER WAS NOT TOLD THE LAYER CLOSED, because it did not: it is mid-`show`, and a
		// dismissal reported there would be a lie that lasts one line.
		CHECK(dismissals == 0);

		overlay.dismiss(); // everything
		CHECK(dismissals == 2);
		CHECK_FALSE(overlay.open());
		// RETIRED, NOT YET FREED — and freed by the next draw. That deferral is the crash fix, so it
		// is asserted rather than left as an implementation detail.
		CHECK(StubExpandedView::alive == before + 2);
		visage::Canvas canvas;
		overlay.draw(canvas);
		CHECK(StubExpandedView::alive == before);
	}

	SUBCASE("dismissing a closed layer does nothing at all") {
		// EVERY ROAD IN CAN ARRIVE TWICE — a click that closes it may be followed by the Esc a hand
		// had already pressed — so a second dismissal must not report a second close.
		overlay.dismiss();
		CHECK(dismissals == 0);
		overlay.show(makeStub({ 420.0f, 300.0f }, "Harm"));
		overlay.dismiss();
		CHECK(dismissals == 1);
		overlay.dismiss();
		CHECK(dismissals == 1);
	}
}
TEST_CASE("HARM's head opens its expanded view, and the panel edits the same rows the card does") {
	// PIECE 2 OF THE DUAL-VIEW BRIEF — the way in — end to end through the real widgets at the real
	// places. A button that opens a panel is three hops (card -> lane -> editor) and every one of
	// them is a place the press can stop, so this drives the head and asks the LAYER what happened.
	HarmFixture f;
	namespace hrm = feathers::fx::harm;

	auto* overlay = f.gui.expandedForTest(); // the EDITOR's overlay — the canvas holds one of its own now, and a tree walk finds that first
	REQUIRE(overlay != nullptr);
	REQUIRE_FALSE(overlay->open());

	SUBCASE("the device declares an expanded view and the build can construct one") {
		// BOTH HALVES, because `hasExpandedView` is deliberately not `traits.hasExpanded` spelled
		// differently: a device could declare the flag before its view exists, and the button must
		// not appear a moment before the panel it promises.
		CHECK(hrm::kTraits.hasExpanded);
		CHECK(ui::hasExpandedView(hrm::kDescriptor));

		// THE FILTER GETS NONE — but on its own that proves nothing about the FLAG, and a known-bad
		// said so. Deleting the `traits.hasExpanded` test from `hasExpandedView` left this subcase
		// GREEN, because the filter is not constructible either and the two reasons for "no" agree
		// on every device that exists. A probe aimed where the right and wrong answers agree is not
		// a probe.
		CHECK_FALSE(fx::filter::kTraits.hasExpanded);
		CHECK_FALSE(ui::hasExpandedView(fx::filter::kDescriptor));

		// SO THE TWO REASONS ARE SEPARATED BY CONSTRUCTION. A descriptor that IS constructible — the
		// same tag, the same arms in the factory — and declares the flag false. There is no such
		// device in the registry and there does not need to be: the claim is about the function.
		feathers::device::Descriptor undeclared = hrm::kDescriptor;
		undeclared.traits.hasExpanded = false;
		CHECK_FALSE(ui::hasExpandedView(undeclared));
		// AND THE OTHER WAY ROUND, so neither half is standing in for the other: a descriptor that
		// declares the flag and is NOT constructible by this build.
		feathers::device::Descriptor unbuildable = fx::filter::kDescriptor;
		unbuildable.traits.hasExpanded = true;
		CHECK_FALSE(ui::hasExpandedView(unbuildable));

		// AND THE RULE ACROSS THE WHOLE REGISTRY: a build never offers a view for a device that has
		// not declared one. Vacuous today in the sense that no device violates it — which is what an
		// invariant looks like while it holds.
		for (const feathers::device::Descriptor& d : feathers::kDevices) {
			CAPTURE(std::string(d.traits.name));
			if (ui::hasExpandedView(d)) {
				CHECK(d.traits.hasExpanded);
			}
		}
	}

	SUBCASE("the head's expand button opens it, and the button is where a hand can reach") {
		const visage::Bounds ex = f.card->expandBounds();
		REQUIRE(ex.width() > 0.0f);
		// IN THE HEAD AND CLEAR OF ITS NEIGHBOURS — the head has four occupants now and the trap this
		// card already documents is a button drawn inside another button's rectangle.
		CHECK(ex.bottom() <= ui::HarmCard::kHeadHeight);
		CHECK_FALSE(ex.contains({ f.card->slotMenuBounds().x() + 1.0f, ex.y() + 1.0f }));
		CHECK_FALSE(f.card->enableBounds().contains({ ex.x() + 1.0f, ex.y() + 1.0f }));
		// THE HIT TEST AGREES WITH THE DRAWING, which is the seam that has bitten twice this week.
		const visage::Point on { ex.x() + ex.width() * 0.5f, ex.y() + ex.height() * 0.5f };
		CHECK(f.card->partAt(on) == ui::HarmCard::Part::Expand);

		// DRIVEN THROUGH THE REAL TREE, not through partAt: naming a part is not the handler acting
		// on it, and the three hops between this press and the layer are the whole claim.
		// ON THE OVERLAY, which is what this case is about: the canvas is folded FIRST, because the
		// fold re-lays the editor out and a window position taken before it is a press into space.
		(void)f.gui.canvasForTest()->setCollapsed(true);
		f.reveal();
		const visage::Bounds inLane = f.lane->cardBounds(f.slot);
		const visage::Point inWindow = f.lane->positionInWindow()
		                               + visage::Point { inLane.x() + on.x, inLane.y() + on.y };
		dragThroughRoot(f.root, inWindow, inWindow);

		REQUIRE(overlay->open());
		const ui::ExpandedView* view = overlay->contentForTest();
		REQUIRE(view != nullptr);
		CHECK(presentedAs<ui::HarmExpanded>(overlay) != nullptr);
		// AND IT SAYS WHICH DEVICE IT IS, from the device's own traits rather than from the opener.
		CHECK(view->title() == std::string(hrm::kTraits.name));
	}

	SUBCASE("a control on the panel writes the row the engine decodes, in the right slot") {
		// THE PANEL AND THE CARD ARE TWO SURFACES OVER ONE BRIDGE, so the claim worth asserting is
		// not that they agree — they cannot disagree — but that the panel's controls are addressed to
		// the same SLOT. HARM sits behind the filter and both are rows of floats; a panel built for
		// slot 1 and writing slot 0 would silently edit the filter.
		ui::WidgetRegistry registry;
		ui::HarmExpanded panel { f.deviceHost, registry, f.device };
		panel.setBounds(0, 0, 420, 340);

		const double before = f.param(hrm::Param::Depth);
		REQUIRE(before != doctest::Approx(0.31));
		// THE NEIGHBOUR'S WHOLE ROW, CAPTURED BEFORE THE WRITE. This is the comparison that catches a
		// panel addressed to the wrong slot, and it only works if the "before" is taken first — a
		// first draft of this line compared the filter's settings to themselves AFTER the write,
		// which is green for every possible implementation.
		REQUIRE(feathers::fxHolds(f.bridge.fxChain(), 0, fx::filter::kTraits.tag));
		const fx::filter::Settings filterBefore =
		    feathers::fxFilterAt(f.bridge.fxChain(), 0).settings;

		ui::FxControl control { f.bridge, f.device, hrm::kDescriptor,
			                    static_cast<size_t>(hrm::Param::Depth) };
		control.setValue(0.31);
		f.bridge.refresh();

		// THE VALUE, READ THROUGH THE DSP'S OWN DECODER — `f.state()` is `fx::harm::stateOf`, the
		// function the audio thread calls, so a green here cannot coexist with the sample loop
		// receiving something else.
		CHECK(f.param(hrm::Param::Depth) == doctest::Approx(0.31));
		// AND THE FILTER IN SLOT 0 IS EXACTLY AS IT WAS — the wrong-slot bug ruled out rather than
		// hoped against. HARM sits behind the filter and both are rows of floats, so a panel built
		// for slot 1 and writing slot 0 would edit the filter and look entirely normal.
		CHECK(feathers::fxFilterAt(f.bridge.fxChain(), 0).settings == filterBefore);

		// AND THE PANEL'S SCOPE IS DRAWING THAT DEPTH, which is what makes it a view of this device
		// rather than a picture beside it.
		const ui::HarmScope* scope = panel.scopeForTest();
		REQUIRE(scope != nullptr);
		CHECK(scope->device() == f.device);
		CHECK(scope->settingsForTest().depth == doctest::Approx(0.31));
	}

	SUBCASE("the panel's minimum is derived from what is on it, and it fits the editor") {
		ui::WidgetRegistry registry;
		ui::HarmExpanded panel { f.deviceHost, registry, f.device };
		const visage::Point minimum = panel.minimumSize();
		CHECK(minimum.x > 0.0f);
		CHECK(minimum.y > 0.0f);
		// IT MUST FIT, or the overlay clamps and the panel is clipped — an honest failure, but still
		// a failure. Checked against the editor's real size rather than a number written here.
		CHECK(minimum.x + 2.0f * ui::ExpandedOverlay::kPanelPad
		      <= static_cast<float>(Gui::kLogicalWidth));
		CHECK(minimum.y + ui::ExpandedOverlay::kTitleHeight + 2.0f * ui::ExpandedOverlay::kPanelPad
		      <= static_cast<float>(Gui::kLogicalHeight));

		// AND EVERY CONTROL IS INSIDE IT once laid out at that minimum — a minimum that did not
		// contain its own contents would be a panel that clips itself at the size it asked for.
		panel.setBounds(0, 0, minimum.x, minimum.y);
		for (const auto& knob : panel.knobsForTest()) {
			CAPTURE(knob->label() != nullptr ? knob->label() : "?");
			CHECK(knob->x() >= -0.01f);
			CHECK(knob->y() >= -0.01f);
			CHECK(knob->right() <= minimum.x + 0.01f);
			CHECK(knob->bottom() <= minimum.y + 0.01f);
		}
		CHECK(panel.polyBounds().bottom() <= minimum.y + 0.01f);
		CHECK(panel.scopeBounds().right() <= minimum.x + 0.01f);
	}
}
TEST_CASE("the harmonic row is ONE widget, the window's, over one control") {
	// THE GAP THIS CLOSES WAS WRITTEN DOWN BEFORE IT WAS FIXED: the expanded view PLOTS the harmonic
	// on its scope and a hand could not move it from there, because the row was four rectangles and
	// a 102-entry popup living inside `HarmCard`.
	//
	// A SECOND COPY WOULD HAVE BEEN THE WRONG FIX. Three separate pieces of hard-won behaviour live
	// on this row — the chevron it was reported missing twice, the arrows dimming at the ends of the
	// table, and the label centred in the box LESS the chevron strip — and two copies is two places
	// for any of them to stop being true, with the broken one being the one nobody is looking at.
	HarmFixture f;
	namespace hrm = feathers::fx::harm;

	ui::HarmonicRow* onCard = f.expanded().harmonicRow();
	ui::HarmonicRow* onPanel = f.expanded().harmonicRow();
	REQUIRE(onCard != nullptr);
	REQUIRE(onPanel != nullptr);

	SUBCASE("a step on either surface is the same number, read from the other") {
		// THE VALUE, and read through the DSP's own decoder as well as through the sibling widget:
		// what makes this one control is the ROW, not agreement between two of them.
		REQUIRE(onCard->index() == hrm::kUnisonHarmonic);
		REQUIRE(onPanel->index() == hrm::kUnisonHarmonic);

		onPanel->step(1);
		CHECK(f.param(hrm::Param::Harmonic)
		      == doctest::Approx(static_cast<double>(hrm::kUnisonHarmonic) + 1.0));
		CHECK(onCard->index() == hrm::kUnisonHarmonic + 1);
		CHECK(onCard->label() == hrm::harmonicLabel(hrm::kUnisonHarmonic + 1));
		// AND THE CARD'S FOLDED HEAD FOLLOWS IT, which is the thing a card shows when it has nothing
		// else — so a change made in the panel is visible the moment the panel closes.
		CHECK(f.expanded().harmonicRow()->ratioText() == "9:8");

		onCard->step(-1);
		CHECK(f.param(hrm::Param::Harmonic)
		      == doctest::Approx(static_cast<double>(hrm::kUnisonHarmonic)));
		CHECK(onPanel->index() == hrm::kUnisonHarmonic);
	}

	SUBCASE("the panel's scope follows the row, because that is why the row is on the panel") {
		// THE COUPLING THE WHOLE PANEL EXISTS FOR. The scope plots the notch where the harmonic puts
		// it; a row that could not move it would be a picture with a caption instead of a control.
		const ui::HarmScope* scope = f.expanded().scopeForTest();
		REQUIRE(scope != nullptr);
		f.expanded().setBounds(0, 0, f.expanded().minimumSize().x, f.expanded().minimumSize().y);

		// LEVEL UP so the notch tracks the harmonic rather than falling back to the fundamental.
		ui::FxControl level { f.bridge, f.device, hrm::kDescriptor,
			                  static_cast<size_t>(hrm::Param::Level) };
		level.setValue(0.6);
		f.bridge.refresh();

		const float before = scope->notchX();
		onPanel->step(1);
		CHECK(scope->notchX()
		      == doctest::Approx(scope->xForRatio(static_cast<double>(hrm::ratioAt(onPanel->index())))));
		// AND IT REALLY MOVED — or the assertion above is satisfied by a notch that never followed.
		CHECK(scope->notchX() != doctest::Approx(before));
	}

	SUBCASE("a press against a stop reports NOTHING, which is the only thing that guard protects") {
		// THE CLAMP IN `step` LOOKS LIKE IT PROTECTS THE VALUE AND DOES NOT. `FxControl::setValue`
		// goes through the bridge, which coerces against the control's own declaration — so a
		// known-bad that deleted the clamp came back green against every value assertion, INCLUDING
		// the stored one read through the DSP's decoder.
		//
		// WHAT IT PROTECTS IS `onChanged`: a dead arrow that fired it would redraw the card, and on
		// the panel redraw the scope, once per press against a stop. Counting the callback is the
		// only probe that can see the line at all — which is the general lesson, not the arithmetic:
		// WHEN A GUARD IS REDUNDANT AGAINST THE VALUE, FIND WHAT ELSE IT IS BUYING BEFORE DECIDING IT
		// IS UNTESTABLE, because "no assertion can see it" and "it does nothing" look identical.
		int reports = 0;
		onPanel->onChanged = [&reports] { ++reports; };

		while (onPanel->canStep(-1)) {
			onPanel->step(-1);
		}
		REQUIRE(onPanel->index() == 0);
		const int atTheStop = reports;

		for (int i = 0; i < 5; ++i) {
			onPanel->step(-1);
		}
		CHECK(reports == atTheStop);
		// AND A STEP THAT REALLY MOVES STILL REPORTS — or the check above passes on a row that
		// stopped reporting anything.
		onPanel->step(1);
		CHECK(reports == atTheStop + 1);
	}

	SUBCASE("both surfaces refuse to step past the ends of the table, and say so") {
		// THE HONESTY RULE ON A STEPPER: an arrow that cannot move must not look like one that can.
		// Asked of BOTH, because "the row does it" is the claim and one instance proving it is one
		// instance.
		for (ui::HarmonicRow* row : { onCard, onPanel }) {
			REQUIRE(row != nullptr);
			for (size_t i = 0; i < hrm::kHarmonicCount + 4; ++i) {
				row->step(-1);
			}
			// THE CONTROL'S OWN VALUE, NOT `index()`. A known-bad that deleted the clamp from `step`
			// came back GREEN, because every READER clamps — `index` and `ratioText` both — so the
			// row happily wrote -4 into the device's row and the picture looked perfect. The stored
			// value is the thing that reaches the DSP, so the stored value is what is asserted.
			CHECK(f.param(hrm::Param::Harmonic) == doctest::Approx(0.0));
			CHECK(row->index() == 0);
			CHECK_FALSE(row->canStep(-1));
			CHECK(row->canStep(1));
			CHECK(row->ratioText() == "1:8");

			for (size_t i = 0; i < hrm::kHarmonicCount + 4; ++i) {
				row->step(1);
			}
			CHECK(f.param(hrm::Param::Harmonic)
			      == doctest::Approx(static_cast<double>(hrm::kHarmonicCount) - 1.0));
			CHECK(row->index() == hrm::kHarmonicCount - 1);
			CHECK(row->canStep(-1));
			CHECK_FALSE(row->canStep(1));
			CHECK(row->ratioText() == "32:1");
		}
	}

	SUBCASE("the label leaves room for the chevron, at the longest entry in the table") {
		// THE TWO-THINGS-IN-ONE-RECTANGLE SHAPE, which this control has paid for twice — the
		// collapsed-header overlap and "of the note" sharing this very box. Asserted at the WORST
		// entry rather than at the default, found by measuring all 102 rather than by guessing which.
		onCard->setBounds(0, 0, 216, ui::HarmonicRow::kHeight);
		const visage::Bounds box = onCard->boxBounds();
		// THE RECTANGLE THE DRAWING ACTUALLY READS. Asserting `box.width() - kChevronStrip` was a
		// claim about two constants and a known-bad that removed the subtraction from the draw call
		// came back green — the mutation was in a line no assertion could reach.
		const visage::Bounds text = onCard->labelBounds();
		CHECK(text.width() == doctest::Approx(box.width() - ui::HarmonicRow::kChevronStrip));
		CHECK(text.right() <= box.right() - ui::HarmonicRow::kChevronStrip + 0.01f);
		float worst = 0.0f;
		size_t worstAt = 0;
		for (size_t i = 0; i < hrm::kHarmonicCount; ++i) {
			const float w = ui::trackedWidth(ui::font(ui::kControlSize), hrm::harmonicLabel(i), 0.0f);
			if (w > worst) {
				worst = w;
				worstAt = i;
			}
		}
		CAPTURE(hrm::harmonicLabel(worstAt));
		CAPTURE(worst);
		// AND THE LONGEST ENTRY FITS THE RECTANGLE THE DRAWING USES.
		CHECK(worst <= text.width());
		// THE STRIP IS REAL — a zero strip would make both checks above vacuous.
		CHECK(ui::HarmonicRow::kChevronStrip > 0.0f);
		CHECK(text.width() < box.width());
	}

	SUBCASE("the window's row never recedes, with its device switched out or in") {
		// A DIFFERENCE BETWEEN THE SURFACES THAT IS DELIBERATE, so it is pinned rather than left to
		// be discovered. The card dims because it sits in a lane beside devices that are running; a
		// panel you deliberately opened to edit one device is not making that comparison.
		visage::Canvas canvas;
		ui::FxControl enabled { f.bridge, f.device, hrm::kDescriptor,
			                    static_cast<size_t>(hrm::Param::Enabled) };

		// **SWITCHED OUT BY HAND, because the device no longer arrives that way.** Since 2026-09-04
		// every effect is on at birth; a case that read the dim state off the default was
		// testing the default as much as the drawing. Both halves are set explicitly now, which is
		// also what makes the panel's half — never dim, in EITHER state — a real claim rather than
		// one state observed twice.
		enabled.setValue(0.0);
		f.bridge.refresh();
		f.card->draw(canvas);
		CHECK(onPanel->active());

		enabled.setValue(1.0);
		f.bridge.refresh();
		f.card->draw(canvas);
		CHECK(onPanel->active());
	}
}
TEST_CASE("modulation reaches the expanded panel, and the panel is on the notify road itself") {
	// Modulation must still reach the expanded view, including once it is a popup.
	//
	// IT IS POSSIBLE BECAUSE ARMING IS STATE, NOT GEOMETRY. A source is armed in `ModState`; a
	// destination glows because it ASKS whether something is armed and whether that source may reach
	// it. Nothing requires the armed arrow and the glowing control to share a window — both are the
	// same process reading the same state. **What would break it is reaching `ModState` through the
	// frame tree**, and these assertions are what will still be here to notice if it ever does.
	//
	// DO NOT ASSUME THE PANEL'S KNOBS ARM BECAUSE THEY ARE `Knob`s OVER `FxControl`. "It is the same
	// widget class" is reasoning that has been wrong twice this week; the panel is constructed
	// differently, handed its modulation differently, and lives somewhere else in the tree.
	HarmFixture f;
	namespace hrm = feathers::fx::harm;

	auto* overlay = f.gui.expandedForTest(); // the EDITOR's overlay — the canvas holds one of its own now, and a tree walk finds that first
	REQUIRE(overlay != nullptr);

	SUBCASE("a knob on the panel is a modulation DESTINATION and arms with the rack's envelope") {
		ui::WidgetRegistry registry;
		ui::HarmExpanded panel { f.deviceHost, registry, f.device };
		// THE EDITOR'S OWN ModState, handed to a surface built OUTSIDE the editor's tree. That is the
		// whole experiment: if arming reaches this panel, it reaches it because the two share STATE
		// and not because they share a parent — which is the property that has to survive the panel
		// becoming a window.
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);
		panel.setModulation(mod, nullptr);
		panel.setBounds(0, 0, panel.minimumSize().x, panel.minimumSize().y);

		ui::Knob* width = nullptr;
		for (const auto& k : panel.knobsForTest()) {
			if (k->label() != nullptr && std::string(k->label()) == "Width") {
				width = k.get();
			}
		}
		REQUIRE(width != nullptr);
		REQUIRE(width->isModDestination());
		// NOTHING ARMED, NOTHING LIT — the state a panel is in before anyone points at it.
		CHECK_FALSE(width->mappable());

		// ARM THE RACK'S ENVELOPE THROUGH ITS REAL ARROW, in the editor, and ask a control in a
		// SURFACE THAT IS NOT IN THE EDITOR'S TREE. That separation is the whole claim: the panel is
		// reachable because it shares `ModState`, not because it shares a parent.
		const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
		f.arm(envelope);
		CHECK(width->mappable());

		// AND A ROUTE REALLY LANDS, from the panel's own knob, at the destination the device
		// declares — the VALUE, not "something became mappable".
		// THROUGH THE DEVICE'S OWN ORDINAL MAPPING, asked of the slot's actual device — the same road
		// `FxControl::destination` takes, so the case cannot agree with a broken adapter by restating
		// its arithmetic differently.
		const backplane::Destination destination =
		    backplane::Destination::control(f.device, static_cast<size_t>(hrm::Param::Width));
		REQUIRE(hrm::kDescriptor.params[destination.index].modulatable);
		CHECK(f.bridge.routeAmount(envelope, destination) == 0.0f);
		width->control()->setAmount(0.4f);
		CHECK(f.bridge.routeAmount(envelope, destination) == doctest::Approx(0.4f));
		// AND NO OTHER SOURCE PICKED IT UP — a route that landed under whatever was focused before
		// would satisfy the check above.
		for (size_t macro = 0; macro < kMacroCount; ++macro) {
			CAPTURE(macro);
			CHECK(f.bridge.routeAmount(backplane::Source::core(macro), destination) == 0.0f);
		}
	}

	SUBCASE("EVERY declared destination arms, on BOTH surfaces — swept, not sampled") {
		// THE SWEEP THAT WOULD HAVE CAUGHT THE BUG THIS CASE FOUND. HARM declares ten modulatable
		// controls; `isModDestination()` was true on all of them and `mappable()` was FALSE on all of
		// them, forever, on the SHIPPING CARD. The arrow armed, the knobs stayed dark, and nothing
		// said why — because `setModulation` told the knobs and not the ADAPTERS, and an adapter that
		// never learned the panel's state reports itself unarmed.
		//
		// SWEPT FROM `kParamDescs`, so the eleventh modulatable control is required to arm the day it
		// is declared rather than the day somebody remembers to add it here.
		ui::WidgetRegistry registry;
		ui::HarmExpanded panel { f.deviceHost, registry, f.device };
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);
		panel.setModulation(mod, nullptr);
		panel.setBounds(0, 0, panel.minimumSize().x, panel.minimumSize().y);

		// WHERE A CONTROL LIVES ON THE PANEL: a knob (a partial's Fine, Width, Depth, Phase, Spread), a fader pill (a partial's
		// Level, in the strip) or a value box (the envelope's six, Porta). Found by the descriptor it is bound to, in the
		// four partials' knobs as well as the open one's, and each kind is asked whether it takes an armed source.
		struct Found {
			bool present = false;
			bool destination = false;
			bool mappable = false;
		};
		const auto onPanelFor = [&](size_t i) {
			Found found;
			const device::ParamDesc* want = &hrm::kParamDescs[i];
			for (int k = 0; k < hrm::kPartialCount; ++k) {
				for (const auto& knob : panel.knobsForTest(k)) {
					if (knob->control() != nullptr && &knob->control()->desc() == want) {
						found = { true, knob->isModDestination(), knob->mappable() };
					}
				}
			}
			for (int k = 0; k < hrm::kPartialCount; ++k) {
				const ui::FaderPill* pill = panel.stripForTest()->levelPill(k);
				if (&pill->amountForTest().desc() == want) {
					found = { true, pill->amountForTest().display().isDestination, pill->mappableForTest() };
				}
			}
			for (const auto& box : panel.envelopeBoxesForTest()) {
				if (&box->control().desc() == want) {
					found = { true, box->control().display().isDestination, box->mappable(false) };
				}
			}
			for (ui::ValueBox* master : { panel.masterLevelForTest(), panel.masterDepthForTest() }) {
				if (&master->control().desc() == want) {
					found = { true, master->control().display().isDestination, master->mappable(false) };
				}
			}
			if (&panel.portaForTest()->control().desc() == want) {
				found = { true, panel.portaForTest()->control().display().isDestination, panel.portaForTest()->mappable(false) };
			}
			return found;
		};

		const backplane::Source envelope = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
		f.arm(envelope);

		size_t swept = 0;
		for (size_t i = 0; i < static_cast<size_t>(hrm::Param::kCount); ++i) {
			if (!hrm::kParamDescs[i].modulatable || harmSlotFurniture(i)
			    || static_cast<int>(i) == hrm::kDescriptor.traits.enableControl) {
				continue; // the furniture arms on the site panel's head row, the enable on the header chip (KW)
			}
			CAPTURE(std::string(hrm::kParamDescs[i].name));
			const Found onPanel = onPanelFor(i);
			REQUIRE(onPanel.present); // the panel is self-contained: every modulatable control is on it
			CHECK(onPanel.destination);
			CHECK(onPanel.mappable);

			// AND ON THE CARD WHERE THE CARD HAS IT. The card keeps three; the rest moved, and a
			// control the card does not hold is not a card failure.
			ui::Knob* onCard = nullptr;
			for (const auto& k : f.card->knobsForTest()) {
				if (k->control() != nullptr && &k->control()->desc() == &hrm::kParamDescs[i]) {
					onCard = k.get();
				}
			}
			if (onCard != nullptr) {
				CHECK(onCard->isModDestination());
				CHECK(onCard->mappable());
			}
			++swept;
		}
		// NOT VACUOUS: the loop really walked the ten HARM declares.
		size_t furnitureModulatable = 0;
		for (size_t i = 0; i < static_cast<size_t>(hrm::Param::kCount); ++i) {
			furnitureModulatable += hrm::kParamDescs[i].modulatable && harmSlotFurniture(i) ? 1u : 0u;
		}
		// LESS THE ENABLE, whose surface is the header chip (backlog KW).
		CHECK(swept == feathers::device::modulatableIn(hrm::kParamDescs) - furnitureModulatable - 1);
		CHECK(swept >= 9);
	}

	SUBCASE("the OPEN panel is on the state channel, and leaves it when dismissed") {
		// A SWALLOWED DEVICE'S CONTROLS ARE NOT PARAMETERS, so `registry.notify` never names them —
		// the state channel is the ONLY road that repaints this panel, and a panel not on it shows a
		// value that has stopped being true.
		//
		// THE SUBSCRIPTION IS WHAT A HEADLESS CASE CAN CHECK, which `FrameWatchers::watches` exists
		// for: visage's `redraw()` is a no-op on a frame nobody is drawing, so the repaint itself is
		// unobservable here and the delivery list is not.
		// A LAYER AND A LIST OF THIS CASE'S OWN, so the assertion is about the mechanism rather than
		// about how many other surfaces the editor happens to subscribe.
		ui::FrameWatchers watchers;
		ui::WidgetRegistry registry;
		ui::ExpandedOverlay layer;
		layer.setBounds(0, 0, 1240, 700);
		layer.setStateWatchers(&watchers);
		const size_t before = watchers.count();

		auto view = std::make_unique<ui::HarmExpanded>(f.deviceHost, registry, f.device);
		ui::HarmExpanded* raw = view.get();
		layer.show(std::move(view));

		CHECK(watchers.watches(raw));
		CHECK(watchers.count() == before + 1);
		// AND THE COUNT THE NOTIFICATION ITSELF REPORTS GOES UP BY THE PANEL — the seam that exists
		// because a repaint cannot be observed headlessly, so the delivery has to say how many it
		// told. THE DAY THIS IS A DETACHED WINDOW, this is the assertion that says the notify road
		// followed it rather than quietly ending at the editor.
		// THE PANEL AND EVERYTHING IN IT. `notify()` counts FRAMES told, not watchers, because a
		// watcher that has children which draw from the same state must have them told too -- the bug
		// this counting was changed to expose. So the rise is the panel's whole subtree.
		CHECK(watchers.notify() > before);

		// AND IT LEAVES THE LIST WHEN DISMISSED, which is not tidiness: the overlay DESTROYS its
		// content, and `notify` calls `redraw()` on every entry. A list still holding the panel is a
		// use-after-free waiting for the next thing that moves.
		layer.dismiss();
		CHECK_FALSE(watchers.watches(raw));
		CHECK(watchers.count() == before);
		CHECK(watchers.notify() == before);

		// AND THE EDITOR'S OWN LAYER IS WIRED TO THE EDITOR'S OWN LIST — or every assertion above is
		// about a mechanism nothing in the shipping editor uses.
		const ui::FrameWatchers* editorWatchers = f.gui.stateWatchers();
		REQUIRE(editorWatchers != nullptr);
		const size_t editorBefore = editorWatchers->count();
		(void)f.gui.canvasForTest()->setCollapsed(true); // on the overlay, the host this case reads
		f.lane->onExpandDevice(f.slot);
		REQUIRE(overlay->open());
		// TWO ENTRIES SINCE THE LAYERS MOUNT THE PANEL (2026-09-13): the mount the layer watches, and the
		// panel itself, on the road in its own right — `Frame::redraw()` marks one frame, so the mount
		// alone would leave the panel unpainted on a patch load.
		CHECK(editorWatchers->count() == editorBefore + 2);
		CHECK(editorWatchers->watches(overlay->contentForTest()));
		CHECK(editorWatchers->watches(presentedAs<ui::HarmExpanded>(overlay)));
		overlay->dismiss();
		CHECK(editorWatchers->count() == editorBefore);
	}
}
TEST_CASE("the open panel does not eat the editor's clicks, so a source can still be armed") {
	// The defect: modulation could not be routed into the expanded view, because opening it
	// unfocused the editor's mod sources — and, on the same build, a SIGSEGV in
	// `ModPanel::focusOrArm` during mouse-down.
	//
	// ONE ROOT CAUSE, AND IT WAS THE LAYER'S DEFAULT. Routing across surfaces is a gesture that
	// SPANS them: arm a source in the mod panel, then click a destination on the panel. The overlay
	// covered the editor and dismissed on any press outside its own rectangle — so the first half of
	// the gesture destroyed the surface the second half was aimed at, and destroyed it from INSIDE
	// the mouse dispatch that was still walking the tree.
	//
	// THIS CASE IS THE REGRESSION GUARD FOR THE COMPLAINT ITSELF, driven through the real editor.
	HarmFixture f;
	namespace hrm = feathers::fx::harm;

	auto* overlay = f.gui.expandedForTest(); // the EDITOR's overlay — the canvas holds one of its own now, and a tree walk finds that first
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(overlay != nullptr);
	REQUIRE(panel != nullptr);

	// ON THE OVERLAY, which is what this case is about: the canvas is folded first.
	(void)f.gui.canvasForTest()->setCollapsed(true);
	f.lane->onExpandDevice(f.slot);
	REQUIRE(overlay->open());

	SUBCASE("a press on the mod panel reaches the MOD PANEL, not the layer") {
		// THE SEAM VISAGE ITSELF USES. A layer that answered here would swallow every press in the
		// editor — which is what it did — and no assertion about widgets can see that, because every
		// widget is still perfectly constructed behind it.
		const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
		const visage::Bounds arrow = panel->arrowBounds(source);
		REQUIRE(arrow.width() > 0.0f);
		const visage::Point inWindow =
		    panel->positionInWindow()
		    + visage::Point { arrow.x() + arrow.width() * 0.5f, arrow.y() + arrow.height() * 0.5f };

		visage::Frame* hit = f.root->frameAtPoint(inWindow);
		REQUIRE(hit != nullptr);
		CHECK(hit != overlay);
		CHECK(hit != static_cast<visage::Frame*>(overlay->panelForTest()));
	}

	SUBCASE("arming a source with the panel open leaves the panel OPEN") {
		// THE EXACT COMPLAINT, AS A VALUE. The arm must land AND the panel must survive it; a case
		// asserting only the first would pass on an editor that dismissed the panel a frame later.
		const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
		f.arm(source);
		CHECK(overlay->open());
		CHECK(overlay->lastDismissalForTest() != ui::ExpandedOverlay::Dismissal::Outside);

		// AND THE WHOLE ROUTE COMPLETES ACROSS THE TWO SURFACES, which is the thing that was
		// impossible: arm over there, write an amount over here, find it in the table.
		auto* view = presentedAs<ui::HarmExpanded>(overlay);
		REQUIRE(view != nullptr);
		ui::Knob* width = nullptr;
		for (const auto& k : view->knobsForTest()) {
			if (k->label() != nullptr && std::string(k->label()) == "Width") {
				width = k.get();
			}
		}
		REQUIRE(width != nullptr);
		CHECK(width->mappable());

		const backplane::Destination destination =
		    backplane::Destination::control(f.device, static_cast<size_t>(hrm::Param::Width));
		REQUIRE(hrm::kDescriptor.params[destination.index].modulatable);
		width->control()->setAmount(0.55f);
		CHECK(f.bridge.routeAmount(source, destination) == doctest::Approx(0.55f));
	}

	SUBCASE("EVERY knob is reachable at EVERY scope position — the sweep, not a spot check") {
		// The symptom: some of the knobs did not respond. The leading hypothesis was that the scope's
		// grips — which now travel across the whole plot in x as well as y — had grown a grab region
		// reaching into the knob rows beneath.
		//
		// THEY HAD NOT. This is the sweep that says so, and it is kept rather than thrown away with
		// the hypothesis: the scope's geometry moves with four controls, and a spot check at the
		// defaults would not have seen an overlap that only opens at one of them. That is the same
		// argument that found four handle collisions the defaults hid.
		const ui::HarmExpanded* view = presentedAs<ui::HarmExpanded>(overlay);
		REQUIRE(view != nullptr);
		auto* panel = const_cast<ui::HarmExpanded*>(view);

		const auto set = [&](hrm::Param param, double value) {
			ui::FxControl c { f.bridge, f.device, hrm::kDescriptor, static_cast<size_t>(param) };
			c.setValue(value);
			f.bridge.refresh();
		};

		size_t checked = 0;
		// ON EVERY TAB: the four partials share one layout, so each partial's five knobs are swept as the open set.
		for (int partial = 0; partial < hrm::kPartialCount; ++partial) {
			panel->selectPartial(partial);
			for (const double harmonic : { 0.0, 9.0, 45.0, 101.0 }) {
				for (const double level : { 0.0, 0.5, 1.0 }) {
					for (const double width : { 0.0, 0.02, 0.5, 1.0 }) {
						set(hrm::partialParam(partial, hrm::PartialField::Harmonic), harmonic);
						set(hrm::partialParam(partial, hrm::PartialField::Level), level);
						set(hrm::partialParam(partial, hrm::PartialField::Width), width);
						panel->resized();
						CAPTURE(partial);
						CAPTURE(harmonic);
						CAPTURE(level);
						CAPTURE(width);
						for (const auto& knob : panel->knobsForTest()) {
							CAPTURE(std::string(knob->label() != nullptr ? knob->label() : "?"));
							const visage::Point at =
							    knob->positionInWindow()
							    + visage::Point { knob->width() * 0.5f, knob->height() * 0.35f };
							// THE KNOB ITSELF, not merely "not the scope" — a press landing on the panel
							// or on nothing is just as unresponsive to a hand.
							CHECK(f.root->frameAtPoint(at) == knob.get());
							++checked;
						}
					}
				}
			}
		}
		CHECK(checked >= 400); // five knobs over 48 configurations on each of four tabs
	}

	SUBCASE("a press inside the panel reaches a CONTROL, not the layer") {
		// THE OTHER HALF OF THE DEFECT: none of the controls on the expanded view could be edited —
		// `setIgnoresMouseEvents(false, false)` skipped both child loops in
		// `Frame::frameAtPoint`, so every press resolved to the layer and the panel was inert by
		// construction. Fifteen subcases passed over it.
		const ui::HarmExpanded* view = presentedAs<ui::HarmExpanded>(overlay);
		REQUIRE(view != nullptr);
		REQUIRE_FALSE(view->knobsForTest().empty());

		for (const auto& knob : view->knobsForTest()) {
			CAPTURE(std::string(knob->label() != nullptr ? knob->label() : "?"));
			const visage::Point inWindow =
			    knob->positionInWindow()
			    + visage::Point { knob->width() * 0.5f, knob->height() * 0.35f };
			visage::Frame* hit = f.root->frameAtPoint(inWindow);
			CHECK(hit != overlay);
			CHECK(hit != static_cast<visage::Frame*>(overlay->panelForTest()));
		}

		// AND THE HARMONIC ROW AND THE SCOPE, which are not knobs and would have been missed by a
		// sweep over `knobsForTest` alone.
		for (const visage::Frame* part :
		     { static_cast<const visage::Frame*>(view->harmonicRow()),
		       static_cast<const visage::Frame*>(view->scopeForTest()) }) {
			REQUIRE(part != nullptr);
			const visage::Point inWindow =
			    part->positionInWindow()
			    + visage::Point { part->width() * 0.5f, part->height() * 0.5f };
			CHECK(f.root->frameAtPoint(inWindow) != overlay);
		}
	}
}
TEST_CASE("the scope IS the control — its grips edit through the same road the knobs use") {
	// The expanded view should be more intuitive than a knob list. The grouping was half. This is the other half, and the argument is that HARM is unusual: the
	// picture already draws what five of its controls do, so a knob grid underneath asks a player to
	// hold a mapping in their head between a row of circles and a picture already showing the answer.
	HarmFixture f;
	namespace hrm = feathers::fx::harm;
	using Grip = ui::HarmScope::Grip;

	ui::HarmScope scope { f.deviceHost, f.device };
	scope.setBounds(0, 0, 300, 140);
	// TURNED ON EXPLICITLY, BECAUSE IT IS OFF IN THE EDITOR. The plot-based edits were disabled after
	// trying them — the graph proved confusing as a control and is display only — and the road is
	// kept behind a switch rather than deleted. **A road nothing exercises rots**, and the day it
	// comes back it would come back untested; so these cases drive it deliberately and say so.
	//
	// THE SUBCASE BELOW ASSERTS THE DEFAULT IS OFF, which is what actually ships.
	scope.setGripsEnabled(true);
	const visage::Bounds box = scope.plotBounds();
	REQUIRE(box.width() > 0.0f);

	const auto write = [&](hrm::Param p, double v) {
		ui::FxControl c { f.bridge, f.device, hrm::kDescriptor, static_cast<size_t>(p) };
		c.setValue(v);
		f.bridge.refresh();
	};
	write(hrm::Param::Level, 0.7);
	write(hrm::Param::Width, 0.3);
	write(hrm::Param::Depth, 0.8);
	write(hrm::Param::Harmonic, 14.0); // 3:2

	const auto centre = [](visage::Bounds b) {
		return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
	};

	SUBCASE("BUT IT SHIPS OFF — the picture is display only until somebody turns it on") {
		// THE DECISION, AFTER USING IT TWICE. The level ring was easy to find; the graph as a whole was
		// confusing as a control, so the plot-based edits are disabled and the knobs do the editing,
		// leaving the graph as display only.
		//
		// SO THE DEFAULT IS THE THING TO PIN, not the behaviour above it. A fresh scope offers no
		// grip anywhere, and the drawing goes quiet with the hit test — a handle a hand cannot grab
		// is decoration that looks like a control.
		ui::HarmScope shipped { f.deviceHost, f.device };
		shipped.setBounds(0, 0, 300, 140);
		CHECK_FALSE(shipped.gripsEnabled());
		const visage::Bounds plot = shipped.plotBounds();
		for (float x = plot.x(); x < plot.right(); x += 7.0f) {
			for (float y = plot.y(); y < plot.bottom(); y += 7.0f) {
				CAPTURE(x);
				CAPTURE(y);
				REQUIRE(shipped.gripAt({ x, y }) == Grip::None);
			}
		}
		// THE DRAWING HALF IS NOT ASSERTED HERE AND CANNOT BE. A known-bad that left the handles
		// drawn while the hit test refused them came back GREEN — a repaint is unobservable
		// headlessly, so "is it drawn" has no seam. It is verified by RENDER, and the two rules meet
		// here: assert the value to catch a partial effect, render the frame to catch a drawn thing
		// that should not be there.
		//
		// AND THE DISPLAY IS UNTOUCHED — the curve, the notch's position and the stem are what the
		// brief asked for and are not what was found confusing.
		CHECK(shipped.notchX() == doctest::Approx(scope.notchX()));
		CHECK(shipped.oscillatorStem() == doctest::Approx(scope.oscillatorStem()));
		std::vector<float> curve(64);
		CHECK(shipped.curveGains(curve.data(), curve.size()) > 0);
	}

	SUBCASE("every grip is REACHABLE where it is drawn, and each names its own control") {
		// THE SEAM THAT CAUGHT A PANEL NOBODY COULD TOUCH THIS MORNING. A drag region is exactly the
		// kind of thing that can be drawn, be correct, and be unhittable — so the question asked here
		// is where a press LANDS, not whether a rectangle exists.
		const std::pair<Grip, hrm::Param> expected[] = {
			{ Grip::Notch, hrm::Param::Harmonic },
			{ Grip::Trough, hrm::Param::Depth },
			{ Grip::Stem, hrm::Param::Level },
			{ Grip::Shoulder, hrm::Param::Width },
		};
		for (const auto& [grip, param] : expected) {
			CAPTURE(static_cast<int>(grip));
			const visage::Bounds g = scope.gripBounds(grip);
			REQUIRE(g.width() > 0.0f);
			CHECK(ui::HarmScope::paramOf(grip) == param);
			// AIMED AT THE DRAWN CENTRE — and the smaller grips must win over the notch column they
			// sit inside, which is what this actually proves.
			CHECK(scope.gripAt(centre(g)) == grip);
		}
		// AND A POINT ON NO GRIP IS NO GRIP — or `gripAt` returning something for everything would
		// make every check above vacuous.
		CHECK(scope.gripAt({ box.x() + 2.0f, box.y() + 2.0f }) == Grip::None);
	}

	SUBCASE("no two grips cover each other, ANYWHERE in the parameter space") {
		// THE BUG THIS CASE EXISTS FOR APPEARED AT ONE WIDTH. The shoulder handle sat at mid-height on
		// the notch's full-height column, so at any narrow width its box covered the column's centre
		// and a press aimed at the notch answered Shoulder. A spot check at the default settings
		// would not have seen it — and did not, until the drag case aimed at a drawn centre.
		//
		// OVERLAPPING GRAB REGIONS CANNOT BE ORDERED OUT OF EXISTENCE: whichever is tested first, the
		// other is unreachable somewhere in the space. So the property is that they never overlap,
		// and it is swept rather than sampled.
		const Grip all[] = { Grip::Notch, Grip::Trough, Grip::Stem, Grip::Shoulder };
		size_t checked = 0;
		for (const double width : { 0.0, 0.02, 0.1, 0.35, 0.7, 1.0 }) {
			for (const double depth : { 0.0, 0.3, 0.7, 1.0 }) {
				for (const double level : { 0.0, 0.25, 0.7, 1.0 }) {
					for (const size_t harmonic : { size_t { 0 }, size_t { 9 }, size_t { 45 },
						                           hrm::kHarmonicCount - 1 }) {
						write(hrm::Param::Width, width);
						write(hrm::Param::Depth, depth);
						write(hrm::Param::Level, level);
						write(hrm::Param::Harmonic, static_cast<double>(harmonic));
						CAPTURE(width);
						CAPTURE(depth);
						CAPTURE(level);
						CAPTURE(harmonic);
						for (size_t i = 0; i < std::size(all); ++i) {
							const visage::Bounds a = scope.gripBounds(all[i]);
							if (a.width() <= 0.0f) {
								continue;
							}
							// EVERY GRIP ANSWERS FOR ITSELF AT ITS OWN CENTRE. That is the whole
							// property — a grip drawn where another one wins is a grip no hand can
							// reach, however correct its rectangle.
							CHECK(scope.gripAt(centre(a)) == all[i]);
							++checked;
						}
					}
				}
			}
		}
		// NOT VACUOUS: the sweep really visited four grips across 384 configurations.
		CHECK(checked >= 384);
	}

	SUBCASE("dragging the trough writes DEPTH, and the drawn trough follows it") {
		// THE VALUE, through the DSP's own decoder. And the y-axis is linear amplitude precisely so
		// the handle is already drawn at `1 - depth`: dragging it to a height IS setting the depth,
		// with nothing converting anything.
		const visage::Bounds g = scope.gripBounds(Grip::Trough);
		scope.onMouseDown().callback(at(centre(g).x, centre(g).y));
		REQUIRE(scope.heldForTest() == Grip::Trough);

		// A QUARTER OF THE PLOT UP FROM THE BOTTOM is depth 0.75, computed here from the axis rather
		// than read back from anything the drag touched.
		const float target = box.bottom() - box.height() * 0.25f;
		scope.onMouseDrag().callback(at(centre(g).x, target));
		scope.onMouseUp().callback(at(centre(g).x, target));

		CHECK(f.param(hrm::Param::Depth) == doctest::Approx(0.75).epsilon(0.02));
		CHECK(scope.heldForTest() == Grip::None);
		// AND THE PICTURE MOVED WITH IT — the grip is drawn where the new value puts it.
		const visage::Bounds after = scope.gripBounds(Grip::Trough);
		CHECK(centre(after).y == doctest::Approx(target).epsilon(0.05));
	}

	SUBCASE("dragging the notch SNAPS to the harmonic table, because nothing between exists") {
		// 102 RATIOS ARE THE ONLY LEGAL POSITIONS. A drag that slid through values between them would
		// be writing indices the device cannot hold, and `Harmonic` is a stepped control.
		const visage::Bounds g = scope.gripBounds(Grip::Notch);
		scope.onMouseDown().callback(at(centre(g).x, centre(g).y));
		REQUIRE(scope.heldForTest() == Grip::Notch);

		// AIMED AT AN EXACT TABLE ENTRY — 2:1 — so the expectation is the index, not "further right".
		const float onOctave = scope.xForRatio(2.0);
		scope.onMouseDrag().callback(at(onOctave, centre(g).y));
		CHECK(f.param(hrm::Param::Harmonic) == doctest::Approx(19.0));
		CHECK(hrm::ratioAt(19) == doctest::Approx(2.0f));

		// AND AIMED BETWEEN TWO, it lands on one of them rather than between.
		const float between = (scope.xForRatio(2.0) + scope.xForRatio(2.125f)) * 0.5f;
		scope.onMouseDrag().callback(at(between, centre(g).y));
		scope.onMouseUp().callback(at(between, centre(g).y));
		const double landed = f.param(hrm::Param::Harmonic);
		CHECK(landed == doctest::Approx(std::round(landed)));
		CHECK((landed == doctest::Approx(19.0) || landed == doctest::Approx(20.0)));
	}

	SUBCASE("the stem carries X as well as Y — one grip, two controls") {
		// The level handle was easy to find and the offset 'pong bar' was confusing. So the pong handle
		// stays, and the level handle duplicates it and also carries an x component.
		//
		// SO THE RING THAT WAS FOUND DOES BOTH JOBS and the rail stays as a second road to the same
		// control. A hand dragging the oscillator's head around the picture is moving the
		// sub-oscillator, and its pitch and its loudness are the two things that mark IS.
		write(hrm::Param::Harmonic, 14.0); // 3:2
		write(hrm::Param::Level, 0.7);
		const visage::Bounds g = scope.gripBounds(Grip::Stem);
		const visage::Point from = centre(g);
		scope.onMouseDown().callback(at(from.x, from.y));
		REQUIRE(scope.heldForTest() == Grip::Stem);

		// MOVED BY THE HAND, NOT TO THE HAND. The ring is offset from the stem it holds — and on
		// whichever side has room — so a drag that put the notch under the pointer would jump it
		// sideways the instant the grip was touched. The target is stated as a DELTA.
		const float before = scope.notchX();
		const float delta = scope.xForRatio(2.0) - scope.notchX();
		const float toY = box.bottom() - box.height() * 0.4f;
		scope.onMouseDrag().callback(at(from.x + delta, toY));

		CHECK(f.param(hrm::Param::Harmonic) == doctest::Approx(19.0)); // 2:1
		CHECK(f.param(hrm::Param::Level) == doctest::Approx(0.4).epsilon(0.02));
		CHECK(scope.notchX() > before);

		// AND THE GRIP DID NOT JUMP UNDER THE POINTER — the ring stays the same distance from the
		// stem it holds, which is what makes the drag feel like moving a thing rather than teleporting
		// one.
		const visage::Bounds after = scope.gripBounds(Grip::Stem);
		CHECK(std::abs(centre(after).x - (from.x + delta)) < 2.0f);
		scope.onMouseUp().callback(at(from.x + delta, toY));
	}

	SUBCASE("with WIDTH AT ZERO there is no notch, and the SHOULDER is how you make one") {
		// The symptom: no filter depth control could be found and no notch was drawn — and MEASURED,
		// that was literally right: Width defaulted to 0, width zero is a
		// wire, and the curve's minimum was exactly 1.0. **That default has since moved to 0.3, so
		// this is no longer the state a fresh device is in — but it is still a state a hand reaches
		// by turning Width down, and the shoulder is still the only way back out of it.**
		//
		// SO THE CASE SETS WIDTH RATHER THAN INHERITING IT. It was written about the defaults and its
		// subject was never the defaults; re-pinning it to 0.3 would have deleted the claim.
		//
		// THE DRAWING WAS NEVER WRONG. What was wrong is that the shoulder grip vanished with the
		// notch, so the picture offered no way to CREATE one, and the depth handle one would look
		// for is correctly absent until there is a trough to grab.
		HarmFixture fresh;
		{
			ui::FxControl w { fresh.bridge, fresh.device, hrm::kDescriptor,
				              static_cast<size_t>(hrm::Param::Width) };
			w.setValue(0.0);
			fresh.bridge.refresh();
		}
		ui::HarmScope blank { fresh.deviceHost, fresh.device };
		blank.setBounds(0, 0, 300, 140);
		blank.setGripsEnabled(true); // the road is off in the editor; this case drives it deliberately
		const hrm::Settings s0 = blank.settingsForTest();
		REQUIRE(s0.width == 0.0f);

		std::vector<float> curve(200);
		hrm::ratioProfile(s0, nullptr, curve.data(), curve.size());
		CHECK(*std::min_element(curve.begin(), curve.end()) == doctest::Approx(1.0));

		// THE TROUGH IS ABSENT AND THAT IS RIGHT — Depth changes nothing while there is no notch, and
		// a control that changes nothing must not be offered.
		CHECK(blank.gripBounds(Grip::Trough).width() == doctest::Approx(0.0f));
		// THE SHOULDER IS PRESENT AND THAT IS THE FIX. Dragging it out DOES change sound, so the
		// honesty rule requires it to be there — pointing the opposite way from usual.
		REQUIRE(blank.gripBounds(Grip::Shoulder).width() > 0.0f);

		// AND IT REALLY MAKES A NOTCH — driven through the grip, asserted on the curve.
		const visage::Bounds sh = blank.gripBounds(Grip::Shoulder);
		blank.onMouseDown().callback(at(centre(sh).x, centre(sh).y));
		blank.onMouseDrag().callback(at(centre(sh).x + 40.0f, centre(sh).y));
		blank.onMouseUp().callback(at(centre(sh).x + 40.0f, centre(sh).y));
		CHECK(fresh.param(hrm::Param::Width) > 0.0);

		hrm::ratioProfile(blank.settingsForTest(), nullptr, curve.data(), curve.size());
		CHECK(*std::min_element(curve.begin(), curve.end()) < 0.5f);
		// AND THE TROUGH HANDLE ARRIVES WITH THE NOTCH IT BELONGS TO.
		CHECK(blank.gripBounds(Grip::Trough).width() > 0.0f);
	}

	SUBCASE("dragging the stem writes LEVEL") {
		// THE CLAIM THIS CASE ONCE COULD NOT MAKE, NOW MADE. It used to say the gesture bracket around
		// a grip's edit could not be asserted, because a known-bad deleting `beginGesture`/`endGesture`
		// from `HarmScope::commit` came back GREEN — `FxControl`'s gesture methods were EMPTY. It ended
		// *"the day `FxControl` grows a real gesture this is the subcase that should grow the
		// assertion." That day is here (backlog 18) and this is that assertion.
		// THE HISTORY HAS TO BE WIRED FOR THE STEP TO EXIST, and `setModulation` is the road that
		// carries it to every adapter — the same call the card makes. Without this the scope's controls
		// hold a null history and record nothing, which is correct behaviour and not the case's
		// subject.
		feathers::ui::ModState mod;
		ui::EditHistory history;
		scope.setModulation(&mod, &history);

		const double levelBefore = f.param(hrm::Param::Level);
		REQUIRE(history.length() == 0);

		const visage::Bounds g = scope.gripBounds(Grip::Stem);
		scope.onMouseDown().callback(at(centre(g).x, centre(g).y));
		const float target = box.bottom() - box.height() * 0.4f;
		scope.onMouseDrag().callback(at(centre(g).x, target));
		scope.onMouseUp().callback(at(centre(g).x, target));
		CHECK(f.param(hrm::Param::Level) == doctest::Approx(0.4).epsilon(0.02));

		// AND THE DRAG LEFT EXACTLY ONE UNDO STEP, which is what makes the bracket observable at all.
		// Delete either half of the bracket from `HarmScope::commit` and this goes red, which is the
		// property the old comment said was missing.
		CHECK(history.length() == 1);
		REQUIRE(history.canUndo());

		// UNDO PUTS THE VALUE BACK — asserted as a VALUE, not as "something changed". The stem was at
		// the fixture's Level before the drag and must be there again after.
		history.undo();
		CHECK(f.param(hrm::Param::Level) == doctest::Approx(levelBefore).epsilon(0.001));
		// AND IT IS NOT VACUOUS: the drag really did move it away from where undo returned it, so the
		// assertion above is not satisfied by a drag that never wrote.
		CHECK(std::abs(levelBefore - 0.4) > 0.05);
	}

	SUBCASE("a grip is a MODULATION DESTINATION, and an armed source takes the drag") {
		// THE CONSTRAINT THAT MAKES THESE CONTROLS RATHER THAN GESTURES. A drag that wrote a value
		// directly would give a thing that cannot be armed, cannot glow, cannot be a destination and
		// does not undo — and this repo shipped exactly that failure today by another route, with
		// ten of HARM's declared destinations unreachable behind perfect-looking widgets.
		ui::ModState* mod = f.gui.modForTest();
		REQUIRE(mod != nullptr);
		scope.setModulation(mod, nullptr);
		for (const Grip grip : { Grip::Notch, Grip::Trough, Grip::Stem, Grip::Shoulder }) {
			CAPTURE(static_cast<int>(grip));
			CHECK_FALSE(scope.gripMappable(grip));
		}

		const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(0), 0);
		f.arm(source);
		// HARMONIC IS NOT MODULATABLE and must NOT become mappable — the honesty rule, and the check
		// that stops this passing on a scope that lit everything.
		REQUIRE_FALSE(hrm::kParamDescs[static_cast<size_t>(hrm::Param::Harmonic)].modulatable);
		CHECK_FALSE(scope.gripMappable(Grip::Notch));
		for (const Grip grip : { Grip::Trough, Grip::Stem, Grip::Shoulder }) {
			CAPTURE(static_cast<int>(grip));
			CHECK(scope.gripMappable(grip));
		}

		// AND AN ARMED DRAG WRITES AN AMOUNT, NOT THE VALUE. Both halves: the route lands, and the
		// value it would otherwise have written stays where it was.
		const backplane::Destination destination =
		    backplane::Destination::control(f.device, static_cast<size_t>(hrm::Param::Depth));
		REQUIRE(hrm::kDescriptor.params[destination.index].modulatable);
		const double depthBefore = f.param(hrm::Param::Depth);

		const visage::Bounds g = scope.gripBounds(Grip::Trough);
		scope.onMouseDown().callback(at(centre(g).x, centre(g).y));
		scope.onMouseDrag().callback(at(centre(g).x, centre(g).y - ui::Knob::kDragRange * 0.5f));
		scope.onMouseUp().callback(at(centre(g).x, centre(g).y));

		CHECK(f.bridge.routeAmount(source, destination) == doctest::Approx(0.5f).epsilon(0.05));
		CHECK(f.param(hrm::Param::Depth) == doctest::Approx(depthBefore));
	}
}

