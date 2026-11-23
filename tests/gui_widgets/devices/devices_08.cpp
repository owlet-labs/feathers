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

// PIECE 8 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

namespace {
/// **THE RULE BEFORE IR**, for the table's "before" column: a SQUARE block cut from the shorter room,
/// cells at its corners, the disk inset from it.
float squareBlockDisk(float railWidth, float railHeight) {
	const auto [across, down] = expectedRoomFor(railWidth, railHeight);
	const float block = std::min(across, down);
	const float offset = block * 0.5f - ui::CircleRail::kCellSide;
	return std::min(block, 2.0f * (offset * std::sqrt(2.0f) - ui::CircleRail::kCornerClear));
}
} // namespace

TEST_CASE("nothing the body draws sits under the chevron that folds it") {
	// **THE TRAP THIS EXISTS FOR IS A TOGGLE THAT IS DRAWN AND CANNOT BE PRESSED.** The body's rail is
	// the panel's FIRST child so that a knob keeps its own clicks — visage's `frameAtPoint` walks
	// children in reverse, so everything added after it is above it. The price of that order is that
	// any cell overlapping the chevron would swallow the press, leaving a mark on screen that does
	// nothing. A photograph cannot see this and neither can a geometry case that only checks the cells.
	//
	// `GenericExpanded::setBodyCollapsible` states the requirement — a `bodyTopInset()` of at least 26,
	// which SKLITTER clears with 65 — and this is what holds a future change to it.
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	ui::BodyRail* body = p.panel->bodyRailForTest();
	REQUIRE(body != nullptr);
	setFolds(p, true, true, false);

	// The rail is at the panel's origin, so its own coordinates ARE the panel's for this comparison.
	REQUIRE(static_cast<float>(body->x()) == doctest::Approx(0.0f));
	REQUIRE(static_cast<float>(body->y()) == doctest::Approx(0.0f));
	const visage::Bounds t = body->toggleBounds();

	size_t seen = 0;
	for (visage::Frame* c : p.panel->cellsForTest()) {
		if (!c->isVisible()) {
			continue;
		}
		++seen;
		const visage::Bounds cell { static_cast<float>(c->x()), static_cast<float>(c->y()),
			                        static_cast<float>(c->width()), static_cast<float>(c->height()) };
		CHECK_FALSE(t.overlaps(cell));
	}
	CHECK(seen > 0); // not vacuous: there are live cells to have collided with

	// **AND IT IS AT THE BODY'S INBOARD EDGE**, beside PERFORM rather than against the window's left —
	// `Rail::Side::Left` is what decides that, and it is the reason the body is a left-hand rail.
	CHECK(t.right() > static_cast<float>(body->width()) - ui::Rail::kCollapsedWidth);
	CHECK(t.right() <= static_cast<float>(body->width()));
	// AND THE BODY REALLY IS THE COLUMN THE GRID IS IN, not the whole panel.
	CHECK(static_cast<float>(body->width())
	      == doctest::Approx(static_cast<float>(p.panel->width()) - p.panel->railsWidth()));
}
TEST_CASE("INVARIANT, over every Rail subclass: expandedWidth() is the same before the rail has been laid out and after") {
	// **`expandedWidth()` MUST NOT MEASURE ANYTHING, AND NOTHING SAID SO UNTIL NOW.**
	//
	// `Rail::railWidth()` is `collapsed_ ? kCollapsedWidth : expandedWidth()`, and `gui.cpp` reads it
	// off a rail *while deciding that rail's bounds* — `Engine rail->setBounds(railEdge, …,
	// Engine rail->railWidth(), …)`. So at the instant it is asked, nothing under the rail has been
	// placed. A width derived from a laid-out child would be 0 on the first pass and right on the
	// next, which presents to a player as **"the panel is the wrong width until I drag the window"
	// — it self-heals, which is exactly what makes the family hard to catch. `ModPanel` was carrying a
	// live instance of the same shape while this was being written — a card asked its height before
	// being given a width and reported one chip row instead of four, so a seven-output modulator drew
	// four of its chips outside the card — and it was fixed hours later by making the width a
	// PARAMETER (`laneExtent(float cross)`, the `chip-foot` merge). **THE SHAPE OUTLIVED THAT FIX**,
	// which is the argument for a case rather than a comment.
	//
	// **THIS IS THE INSTRUMENT THAT FAMILY HAS NEVER HAD.** Build it, ask it COLD, lay it out, ask it
	// again. Three live instances of this shape and not one of them had a case like this.
	//
	// **WALKED, NOT WRITTEN FIVE TIMES**, so a sixth subclass has one place to be added rather than a
	// fifth copy to be forgotten. **AND THE LIST IS HAND-MAINTAINED, WHICH IS STATED RATHER THAN
	// PAPERED OVER**: `Rail` has no registry, so what the breadth counter below enforces is that this
	// walk is not short or empty — the empty-filter trap `run-cases.sh` exists to catch, reappearing
	// inside the case written to prevent it — and NOT that implementation six was added to it. The
	// grep that finds them all is `class .* : public Rail`.
	//
	// **THE THREE OLDER RAILS PASSING UNCHANGED IS ALSO THE PROOF THIS IS NOT VACUOUS**: they were
	// written before the rule was stated, so their green is evidence about the case rather than about
	// a rule they were built to satisfy.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;

	// The two SKLITTER rails are OWNED by a panel, so the panel is what gets built and laid out and
	// the rails are read out of it. Built here rather than inside the list so it outlives the walk.
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	f.bridge.refresh();
	std::unique_ptr<ui::ExpandedView> sklitter = ui::makeExpandedView(
	    f.deviceHost, &f.bridge, registry, f.bridge.fxDeviceAt(slot), sc::kDescriptor);
	auto* sklitterPanel = dynamic_cast<ui::SklitterExpanded*>(sklitter.get());
	REQUIRE(sklitterPanel != nullptr);

	/// One subclass under test. `owner` is what gets laid out — the rail itself for the three that
	/// stand alone, and the panel for the two that are seated inside one.
	struct Subject {
		const char* name;
		std::unique_ptr<visage::Frame> owned; // null when the rail is owned elsewhere
		visage::Frame* owner;
		ui::Rail* rail;
	};
	std::vector<Subject> subjects;
	const auto standalone = [&subjects](const char* name, std::unique_ptr<ui::Rail> rail) {
		ui::Rail* raw = rail.get();
		subjects.push_back(Subject { name, std::move(rail), raw, raw });
	};
	standalone("EngineRail", std::make_unique<ui::EngineRail>(f.bridge, registry));
	standalone("ModPanel", std::make_unique<ui::ModPanel>(f.bridge, f.deviceHost, registry, mod, history));
	standalone("FxLane", std::make_unique<ui::FxLane>(f.bridge, registry));
	// THE STAGE'S OWN RAIL (backlog NF), whose open width is its toggle's corner — a constant like the rest.
	standalone("StageRail", std::make_unique<ui::StageRail>());
	subjects.push_back(
	    Subject { "CircleRail", nullptr, sklitterPanel, &sklitterPanel->circleRailForTest() });
	// **BOTH FX RAILS, SEPARATELY, BECAUSE THEY CARRY DIFFERENT LABELS.** `GRAIN FX` and `POST FX`
	// replaced the single `FX LANES` when the lanes split into two rails (2026-09-20), and this walk is
	// the only thing that asks the FONT how wide a label actually is at `kCollapsedLabelSize`. A rail
	// added to the panel and not added here is a label nobody measured — which is how `MODULATORS`
	// nearly shipped clipped at 140px against a 130px run.
	subjects.push_back(
	    Subject { "FxLaneRail(GRAIN)", nullptr, sklitterPanel, &sklitterPanel->grainRailForTest() });
	subjects.push_back(
	    Subject { "FxLaneRail(POST)", nullptr, sklitterPanel, &sklitterPanel->postRailForTest() });
	// **THE SIXTH, AND THE FIRST ONE ADDED SINCE THE WALK SAID IT WAS HAND-MAINTAINED.** `BodyRail` is
	// owned by `GenericExpanded`, so like the two above it is read out of a panel rather than built
	// here. It is also the one subclass whose `expandedWidth()` nobody sums — the body is what is LEFT
	// once the rails have taken theirs — which makes it the likeliest of the six to drift into
	// measuring something, and therefore the one most worth having in this walk.
	REQUIRE(sklitterPanel->bodyRailForTest() != nullptr);
	subjects.push_back(
	    Subject { "BodyRail", nullptr, sklitterPanel, sklitterPanel->bodyRailForTest() });

	size_t walked = 0;
	for (Subject& s : subjects) {
		CAPTURE(std::string(s.name));
		REQUIRE(s.rail != nullptr);
		// **COLD: FRESHLY CONSTRUCTED, NO LAYOUT PASS HAS RUN.** That is the state `gui.cpp` asks in.
		//
		// MEASURED while writing this: `ModPanel` arrives from its constructor already 34px wide, so
		// "every frame in the subtree is zero" is NOT a thing this case may require — what it can
		// require is that no `resized()` has run, which is what is true here.
		const float cold = s.rail->expandedWidth();
		// A rail that answered ZERO cold is the defect itself, so this is not merely a sanity check.
		CHECK(cold > 0.0f);

		// **LAID OUT THREE TIMES AT DELIBERATELY DIFFERENT SIZES, AND THE FIRST IS SMALLER THAN ANY
		// CONTENT.** One pass would catch a width read off a child that had never been placed; three
		// also catch one that tracks its own frame or its content's, because a measured answer cannot
		// be the same number inside a 40px rail and a 1240px one.
		for (const int wide : { 40, 900, 1240 }) {
			CAPTURE(wide);
			s.owner->setBounds(0, 0, wide, 800);
			s.owner->resized();
			CHECK(s.rail->expandedWidth() == doctest::Approx(cold));
		}
		// NOT VACUOUS: the walk really did lay something out, at the width it was last told.
		CHECK(static_cast<float>(s.owner->width()) == doctest::Approx(1240.0f));
		CHECK(static_cast<float>(s.rail->width()) > 0.0f);
		++walked;
	}

	// **THE BREADTH COUNTER.** A walk that silently iterated nothing would report SUCCESS with every
	// assertion above unreached. Six is what `class .* : public Rail` finds today.
	REQUIRE(walked == subjects.size());
	CHECK(walked >= 8u);
}
TEST_CASE("backlog NF: a stage's caption clears the collapse toggle — centred while that clears the corner, slid "
          "left of it when it does not") {
	// The fx lane's "P⟩ST-FX" lesson on the stage: the caption shares the toggle's row, so where it starts is a
	// value with a wrong answer available. The toggle's corner is the stage's last `kOpenWidth`.
	const float text = 360.0f; // the full Explore caption, near enough, at the byline size
	for (const float stage : { 380.0f, 420.0f, 500.0f, 597.0f, 800.0f, 1200.0f }) {
		CAPTURE(stage);
		const float x = ui::StageRail::captionLeft(stage, text);
		const float centred = (stage - text) * 0.5f;
		CHECK(x + text <= stage - ui::StageRail::kOpenWidth + 1.0e-3f);
		// CENTRED WHENEVER CENTRED ALREADY CLEARS IT, so a wide stage looks exactly as it did.
		if (centred + text <= stage - ui::StageRail::kOpenWidth) {
			CHECK(x == doctest::Approx(centred));
		}
		else {
			CHECK(x < centred);
		}
	}
	// TOO NARROW FOR THE LINE: the answer is below the floor, which is what sends a stage to its short word.
	CHECK(ui::StageRail::captionLeft(380.0f, text) < ui::StageRail::kCaptionMinLeft);
}
TEST_CASE("STRUCTURAL: every card hands its press back to the base, in BOTH of the editor's hosts") {
	// Buttons on the LFO's card were not responding.
	//
	// `FxCard` splits the gesture and says so where it takes its half: *"THE DRAG AND THE RELEASE ARE
	// THE BASE'S, the press is the card's — see noteCardPress." The base assigns `onMouseDrag` and
	// `onMouseUp` in its constructor; a card that writes its own `onMouseDown` takes on the other third
	// of a contract that nothing in the type system states. Twelve cards opened their handler with
	// `noteCardPress`. The thirteenth did not, and it was the newest.
	//
	// WHAT THE OMISSION COSTS IS NOT SUBTLE, which is why it was reported as a feel: `noteCardPress`
	// dispatches the head's marks, arms the reorder drag, records the press so `noteCardRelease` can
	// FOLD on release (the fold has been a head press since the `(-)` mark was retired), and fires
	// `onCardPressed` — how a lane learns that Backspace and Delete now mean THIS slot. Miss it and the
	// head is inert, the card cannot be folded, and the keys go on naming whichever card was pressed
	// last: the defect `fx_card.h` already records against the DAHDSR's value boxes.
	//
	// **AND IT SWEEPS BOTH HOSTS, WHICH THE FIRST DRAFT OF THIS CASE DID NOT.** That draft walked
	// `FxLane::addableForTest()` and pressed every card it found — and stayed GREEN against a build
	// with the bug still in it, because the LFO is a MODULATOR and never appears in the fx lane's
	// addable list. It had a non-vacuity guard, and the guard passed: it proved the loop ran, which is
	// not the same as proving it ran over the thing the case is named for. One card class, two hosts,
	// two registries — the same shape `fx_card.h` records for the enable square, met from the other
	// side. So the enumeration is asked of each host's OWN list, and each half must find something.
	//
	// THE PROPERTY IS READ AS A VALUE through `onCardPressed`: press the head, and the host's focused
	// slot becomes that card's. A card that keeps the press leaves the focus where it was.
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	auto* probePanel = findInTree<ui::ModPanel>(probe.root);
	REQUIRE(probeLane != nullptr);
	REQUIRE(probePanel != nullptr);

	std::vector<std::string> fxTags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		fxTags.emplace_back(d->traits.tag);
	}
	std::vector<std::string> modTags;
	for (const device::Descriptor* d : probePanel->addableForTest()) {
		modTags.emplace_back(d->traits.tag);
	}
	REQUIRE(fxTags.size() >= 4);
	REQUIRE(modTags.size() >= 2);
	// THE CASE IS NAMED FOR A MODULATOR, so the modulator half must actually contain it — the exact
	// thing the first draft got wrong, asserted rather than assumed.
	CHECK(std::find(modTags.begin(), modTags.end(), std::string(mod::lfo::kTraits.tag))
	      != modTags.end());

	size_t sweptFx = 0;
	for (const std::string& tag : fxTags) {
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(tag.c_str());
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		// FOCUS SOMEWHERE ELSE FIRST, so "it moved" and "it was already there" are different numbers.
		lane->focusSlot(0);
		REQUIRE(lane->focusedSlotForTest() == 0);
		// **THE HEAD, NOT THE BODY.** DrawnCard and WaypointCard deliberately withhold a press that
		// lands on the puck they draw themselves, so that morphing the filter cannot also pick the card
		// out of the lane — documented at both, and a body sweep would assert a rule they are right to
		// break. Every card hands a HEAD press back; the fold and the marks are made of it.
		card->onMouseDown().callback(at(card->width() * 0.5f, ui::kCardHeadHeight * 0.5f));
		++sweptFx;
		CHECK(lane->focusedSlotForTest() == slot);
	}

	size_t sweptMod = 0;
	for (const std::string& tag : modTags) {
		CAPTURE(tag);
		ModFixture f;
		auto* panel = findInTree<ui::ModPanel>(f.root);
		REQUIRE(panel != nullptr);
		const size_t slot = f.bridge.addRackDevice(tag.c_str());
		REQUIRE(slot >= 1);
		f.bridge.refresh();
		panel->resized();
		auto* card = const_cast<visage::Frame*>(panel->cardFrameForTest(slot));
		REQUIRE(card != nullptr);
		panel->selectSlot(0);
		REQUIRE(panel->selectedSlotForTest() == 0);
		card->onMouseDown().callback(at(card->width() * 0.5f, ui::kCardHeadHeight * 0.5f));
		++sweptMod;
		CHECK(panel->selectedSlotForTest() == slot);
	}

	// AND BOTH HALVES SWEPT SOMETHING. One number for the pair would have been satisfied by the fx
	// half alone, which is exactly how the first draft passed.
	CAPTURE(sweptFx);
	CAPTURE(sweptMod);
	CHECK(sweptFx == fxTags.size());
	CHECK(sweptMod == modTags.size());
	CHECK(sweptFx >= 4);
	CHECK(sweptMod >= 2);
}
TEST_CASE("the LFO's card can reach every control its device declares") {
	// **THE HONESTY RULE'S OTHER DIRECTION.** The rule as CLAUDE.md states it is "a control is enabled
	// iff it changes sound through a real param" — a control that does nothing. This is the converse: a
	// PARAM with nowhere to put a number. The device declares it, the engine reads it, automation can
	// reach it, and the hand cannot.
	//
	// IT SHIPPED THAT WAY ON THIS CARD. `layout()` held Rate, Base, Smoothing and Amount while its own
	// header documented "Rate, Skew A, Skew B, Skew C" — so `SkewPrePeak` and `SkewPreCross` were
	// reachable from NOWHERE, and `SkewPreTrough` only as a chip that flipped a 0..100% weight between
	// 50 and 100. Three of the device's controls, two of them entirely unavailable.
	//
	// **AND THE SEAM THAT WOULD HAVE CAUGHT IT WAS A STUB.** `addressedParamsForTest` returned `{}`.
	// The guards in this file read exactly that list to ask what a card reaches; a card claiming to
	// reach nothing does not fail them, it empties them. An instrument wired to say "no findings"
	// reads like one that found none.
	namespace lfo = feathers::mod::lfo;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	REQUIRE(f.bridge.addRackDevice(lfo::kTraits.tag) >= 1);
	f.bridge.refresh();
	panel->resized();

	const auto* card = dynamic_cast<const ui::LfoCard*>(panel->cardFrameForTest(1));
	REQUIRE(card != nullptr);
	const std::vector<lfo::Param> reached = card->addressedParamsForTest();
	// THE SEAM IS NOT A STUB ANY MORE, asserted before it is trusted — the whole case rests on it.
	// ALL BUT PING-PONG, which is hidden (2026-11-22): the seam must answer with the rest, or it is a stub again.
	REQUIRE(reached.size() >= static_cast<size_t>(lfo::Param::kCount) - 1);

	for (int i = 0; i < static_cast<int>(lfo::Param::kCount); ++i) {
		const auto param = static_cast<lfo::Param>(i);
		// `CAPTURE` on a `const char*` prints the POINTER, not the text — playbook, and it printed "1" the
		// first time this case went red. `std::string` is what makes the failure name the control.
		CAPTURE(std::string(lfo::kParamDescs[static_cast<size_t>(i)].name));
		// PING-PONG IS HIDDEN (2026-11-22): the engine never reads it, so no card reaches it. Its own case asserts the absence.
		if (param == lfo::Param::PingPong) {
			continue;
		}
		CHECK(std::find(reached.begin(), reached.end(), param) != reached.end());
	}

	// AND EVERY BOX SITS ON A DIFFERENT CONTROL, or nine boxes could be one control nine times and the
	// sweep above would still be satisfied by the chips alone.
	const auto& boxes = ui::LfoCard::layout();
	std::vector<lfo::Param> sorted(boxes.begin(), boxes.end());
	std::sort(sorted.begin(), sorted.end());
	CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

	// AND NO CHIP CARRIES A CONTINUOUS CONTROL. A chip is a switch or a cycle; `SkewPreTrough` sat
	// here as a percentage, which is how 99 of its 100 positions became unreachable by hand. Asked of
	// the descriptor's own `stepped` flag rather than of a list of names, so a tenth chip added later
	// has to answer it too.
	for (const ui::LfoCard::Chip& chip : ui::LfoCard::chips()) {
		CAPTURE(std::string(lfo::kParamDescs[static_cast<size_t>(chip.param)].name));
		CHECK(lfo::kParamDescs[static_cast<size_t>(chip.param)].stepped);
	}
}
TEST_CASE("backlog NJ: the LFO's first Time Base reads \"Hz\", and Rate reads Hz there and a multiple elsewhere") {
	// decided 2026-10-17: the base that was "Off" is "Hz", and Rate reads Hz on it. Read off the boxes' own
	// readouts — the text a hand sees — with Rate at 1 (log2: 2 Hz, or 2X on a division).
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
	const auto& boxes = ui::LfoCard::layout();
	const auto boxOf = [&boxes](lfo::Param p) {
		return static_cast<size_t>(std::find(boxes.begin(), boxes.end(), p) - boxes.begin());
	};
	const ui::ValueBox* base = card->boxForTest(boxOf(lfo::Param::TimeBase));
	const ui::ValueBox* rate = card->boxForTest(boxOf(lfo::Param::TimeScale));
	REQUIRE(base != nullptr);
	REQUIRE(rate != nullptr);
	const auto set = [&](lfo::Param p, float v) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(p), v);
		f.bridge.refresh();
		card->updateContent();
	};
	set(lfo::Param::TimeScale, 1.0f);

	set(lfo::Param::TimeBase, 0.0f);
	CHECK(base->readout() == "Hz");
	CHECK(rate->readout() == "2Hz");

	set(lfo::Param::TimeBase, 13.0f); // 1/4
	CHECK(base->readout() == "1/4");
	CHECK(rate->readout() == "2X");
}
TEST_CASE("TV audit: the LFO's Ping-Pong is off the card, and its saved value still reads back") {
	// PING-PONG: the engine never reads it, so its chip is hidden in every mode. Its key stays, so a patch that stored it
	// loads and keeps the value. RATE: a frequency on Hz, a multiple of a division otherwise; the text is the card's override.
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

	const std::vector<lfo::Param> reached = card->addressedParamsForTest();
	CHECK(std::find(reached.begin(), reached.end(), lfo::Param::PingPong) == reached.end());
	CHECK(ui::LfoCard::kChipCount == 3);

	// THE VALUE STILL LOADS AND READS BACK: set it through the host, and the card's settings carry it.
	f.deviceHost.setField(card->device(), static_cast<size_t>(lfo::Param::PingPong), 1.0f);
	f.bridge.refresh();
	(void)card->updateContent();
	CHECK(card->restFromForTest().pingPong == true);
}
TEST_CASE("backlog NJ: the LFO's Rate types back in the unit its face prints — \"2hz\", \"2 Hz\", \"2x\" — any case") {
	// The lead's follow-up, 2026-10-17. On the Hz base the face prints "2Hz", so its own unit name must type back, with
	// or without a space, in any case; the declared multiple's "x" reads as it always did (exp2 of one number either
	// way). On a division the face prints "2X" and "hz" is not its unit: refused, nothing moves. Driven through the
	// box's own triple-click and field; the value read is the stored Rate (log2), the number the engine reads.
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
	const auto& boxes = ui::LfoCard::layout();
	const size_t rateAt =
	    static_cast<size_t>(std::find(boxes.begin(), boxes.end(), lfo::Param::TimeScale) - boxes.begin());
	auto* rate = const_cast<ui::ValueBox*>(card->boxForTest(rateAt));
	REQUIRE(rate != nullptr);
	const auto set = [&](lfo::Param p, float v) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(p), v);
		f.bridge.refresh();
		card->updateContent();
	};
	const auto stored = [&] { return f.deviceHost.fieldAt(card->device(), static_cast<size_t>(lfo::Param::TimeScale)); };
	const auto type = [&](const std::string& text) {
		const float x = rate->width() * 0.5f;
		const float y = rate->height() * 0.5f;
		rate->processMouseDown(at(x, y));
		rate->processMouseUp(at(x, y));
		rate->processMouseDown(doubleClickAt(x, y));
		rate->processMouseUp(at(x, y));
		rate->processMouseDown(tripleClickAt(x, y));
		rate->processMouseUp(at(x, y));
		ui::TextField* field = rate->typeInField();
		REQUIRE(field != nullptr);
		REQUIRE(field->editing());
		field->processTextInput(text);
		field->processKeyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true });
	};
	REQUIRE(rate->width() > 0.0f);

	SUBCASE("on the Hz base, the face's unit and the declared one both read") {
		set(lfo::Param::TimeBase, 0.0f);
		for (const auto& [text, hz] : std::vector<std::pair<std::string, double>> {
		         { "2hz", 2.0 }, { "2 Hz", 2.0 }, { "3HZ", 3.0 }, { "0.5 hZ ", 0.5 }, { "4x", 4.0 }, { "2X", 2.0 }, { "3", 3.0 } }) {
			CAPTURE(text);
			set(lfo::Param::TimeScale, 0.25f); // somewhere none of them lands
			type(text);
			CHECK(stored() == doctest::Approx(std::log2(hz)).epsilon(1e-6));
		}
	}

	SUBCASE("on a division, \"hz\" is not the face's unit: refused, nothing moves") {
		set(lfo::Param::TimeBase, 13.0f); // 1/4
		set(lfo::Param::TimeScale, 0.25f);
		type("2hz");
		CHECK(stored() == doctest::Approx(0.25f));
		type("2x");
		CHECK(stored() == doctest::Approx(1.0).epsilon(1e-6));
	}
}
TEST_CASE("the LFO shows the skews for a shaped wave and the seed for Random, and never both") {
	// **THE HONESTY RULE, ENFORCED RATHER THAN NARRATED.** Both statements were already written down —
	// "only meaningful in Random mode" on the seed, "periodic modes only" on the skews — in the
	// tooltips, which is prose a player has to hover to read. The card drew all four at all times, so
	// a triangle offered a seed that changes nothing and Random offered three skew weights that remap
	// a phase it does not have. Value boxes are mode-specific, so they are hidden when unused.
	//
	// DECLARED ON THE DESCRIPTOR, not decided by the card: `gateControl` + `gateMask`, which is what
	// `device::relevantFor` reads and what makes this a fact about the DEVICE that any surface can ask.
	namespace lfo = feathers::mod::lfo;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(slot >= 1);
	f.bridge.refresh();
	panel->resized();
	auto* card = const_cast<ui::LfoCard*>(
	    dynamic_cast<const ui::LfoCard*>(panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);

	const auto& boxes = ui::LfoCard::layout();
	const auto boxOf = [&boxes](lfo::Param p) {
		return static_cast<size_t>(std::find(boxes.begin(), boxes.end(), p) - boxes.begin());
	};
	const size_t peak = boxOf(lfo::Param::SkewPrePeak);
	const size_t cross = boxOf(lfo::Param::SkewPreCross);
	const size_t trough = boxOf(lfo::Param::SkewPreTrough);
	const size_t seed = boxOf(lfo::Param::Seed);
	REQUIRE(peak < boxes.size());
	REQUIRE(seed < boxes.size());

	const auto setWave = [&](lfo::Waveform w) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(lfo::Param::Waveform),
		                        static_cast<float>(static_cast<int>(w)));
		f.bridge.refresh();
		card->updateContent();  // the gate moved; the card reflows here, not only on a chip press
		panel->resized();
	};
	const auto shown = [&](size_t box) { return card->boxFrame(box)->isVisible(); };

	SUBCASE("a shaped wave carries the three weights and no seed") {
		for (const lfo::Waveform w :
		     { lfo::Waveform::Sine, lfo::Waveform::Triangle, lfo::Waveform::Square }) {
			CAPTURE(std::string(lfo::kWaveformNames[static_cast<size_t>(w)]));
			setWave(w);
			CHECK(shown(peak));
			CHECK(shown(cross));
			CHECK(shown(trough));
			CHECK_FALSE(shown(seed));
		}
	}

	SUBCASE("Random carries the seed and none of the weights") {
		setWave(lfo::Waveform::Random);
		CHECK(shown(seed));
		CHECK_FALSE(shown(peak));
		CHECK_FALSE(shown(cross));
		CHECK_FALSE(shown(trough));

		// AND THE SEED PACKS TO THE LEFT OF ITS ROW rather than sitting in the fourth column with three
		// holes beside it — the reflow, which is the difference between a gated card and a gappy one.
		const size_t firstOfRow = (seed / ui::LfoCard::kBoxesPerRow) * ui::LfoCard::kBoxesPerRow;
		CHECK(card->boxFrame(seed)->x() == doctest::Approx(card->boxFrame(firstOfRow)->x()));
	}

	SUBCASE("the row that is always live never moves") {
		// THE MSEG'S FIVE HAVE NO GATE, so no waveform may take one away — the non-vacuity guard for
		// the two subcases above, which would both be satisfied by a card that hid everything.
		for (const lfo::Waveform w : { lfo::Waveform::Sine, lfo::Waveform::Random }) {
			CAPTURE(std::string(lfo::kWaveformNames[static_cast<size_t>(w)]));
			setWave(w);
			for (size_t i = 0; i < ui::LfoCard::kBoxesPerRow; ++i) {
				CAPTURE(i);
				CHECK(shown(i));
			}
		}
	}
}
TEST_CASE("the LFO's drawn window starts where the CYCLE starts — pushing Phase turns the curve, it does not leave it still") {
	// Pushing Phase in any non-panning oscillator mode (all except the randoms) updates the drawn curve,
	// so phase 0 stays at the start of the drawn window.
	//
	// `curve()` drew `eval(s, x)` across the card: one period beginning at WAVEFORM phase 0, whatever
	// Phase was set to. The engine begins at `Settings::phase` — `Shape::restart` assigns
	// `position_.phase = s.phase` — so the control moved where the shape STARTS while the picture went
	// on showing a cycle starting somewhere the device never starts.
	//
	// **ASSERTED AGAINST THE ENGINE'S OWN FUNCTION, NOT AGAINST THE OTHER PICTURE.** `lfo::eval` is
	// what the DSP calls; computing the expected value from it here is the independent restatement,
	// and a curve that merely "changed when Phase moved" would pass on any rotation at all, including
	// the wrong way round.
	namespace lfo = feathers::mod::lfo;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(slot >= 1);
	f.bridge.refresh();
	panel->resized();
	auto* card = const_cast<ui::LfoCard*>(
	    dynamic_cast<const ui::LfoCard*>(panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);

	const auto set = [&](lfo::Param p, float v) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(p), v);
		f.bridge.refresh();
		card->updateContent();
	};

	for (const lfo::Waveform wave :
	     { lfo::Waveform::Sine, lfo::Waveform::Triangle, lfo::Waveform::Square }) {
		CAPTURE(std::string(lfo::kWaveformNames[static_cast<size_t>(wave)]));
		set(lfo::Param::Waveform, static_cast<float>(static_cast<int>(wave)));

		for (const float start : { 0.0f, 0.25f, 0.5f, 0.75f }) {
			CAPTURE(start);
			set(lfo::Param::Phase, start);
			const auto drawn = card->curve();
			REQUIRE(drawn.size() >= 2);

			// **THE REFERENCE IS BUILT HERE, NOT READ BACK OFF THE CARD.** Asking the card what it
			// thinks its settings are and then checking its drawing against them would be the card
			// agreeing with itself; these are the values this case SET, so the comparison is against
			// what was asked for. If the control did not reach the device the sweep below fails, which
			// is the right failure rather than a silently vacuous pass.
			lfo::Settings s;
			s.wave = wave;
			s.phase = start;

			// EVERY SAMPLE: the card at x must be the engine at the phase the card claims x is.
			for (const auto& point : drawn) {
				const float at = start + point.first;
				CAPTURE(point.first);
				CHECK(point.second == doctest::Approx(lfo::eval(s, at - std::floor(at))));
			}
			// AND THE LEFT EDGE IS THE START OF THE CYCLE, stated separately because it is the whole
			// of what was asked for and the sweep above would survive an off-by-one-sample drift.
			CHECK(drawn.front().second == doctest::Approx(lfo::eval(s, start)));
		}

		SUBCASE("and it really is a ROTATION — the same curve, turned, not a different one") {
			// THE NON-VACUITY HALF. Every check above would pass on a curve that ignored Phase if
			// `eval` also ignored it; this compares two DRAWINGS and pins the displacement, so a card
			// that drew the same picture at every Phase fails here even if `eval` were wrong too.
			set(lfo::Param::Phase, 0.0f);
			const auto atZero = card->curve();
			REQUIRE(atZero.size() >= 2);

			// **THE TURN IS A WHOLE NUMBER OF SAMPLES, WHICH A QUARTER IS NOT.** 128 points span the
			// period in 127 INTERVALS, so `0.25` lands between samples and the first draft's
			// `REQUIRE(quarter * 4 == size - 1)` reddened at 124 against 127 — doing its job. Turning
			// by `k/127` instead maps sample `i` onto sample `i + k` exactly, so the comparison needs
			// no interpolation and holds for the SQUARE too, where interpolating across the edge would
			// compare a value to the discontinuity it sits in the middle of.
			const size_t period = atZero.size() - 1; // index `period` repeats index 0
			const size_t k = period / 4;
			const float turn = static_cast<float>(k) / static_cast<float>(period);
			set(lfo::Param::Phase, turn);
			const auto turned = card->curve();
			REQUIRE(turned.size() == atZero.size());

			for (size_t i = 0; i < period; ++i) {
				CAPTURE(i);
				CHECK(turned[i].second == doctest::Approx(atZero[(i + k) % period].second));
			}
			// AND THE TWO ARE NOT THE SAME PICTURE, or the rotation above is a curve compared with
			// itself — which is exactly what a square wave turned by half a period would be.
			bool differs = false;
			for (size_t i = 0; i < atZero.size(); ++i) {
				differs = differs || (std::abs(atZero[i].second - turned[i].second) > 1e-3f);
			}
			CHECK(differs);
		}
	}

	SUBCASE("and the randoms are untouched — their window is over the song, not over one period") {
		// THE OTHER HALF OF THE SENTENCE ("all except the randoms"). Their scope is placed by the
		// playhead's step and fraction; `Phase` is not what positions it, and a rotation applied there
		// would move the window somewhere the walk is not.
		set(lfo::Param::Waveform, static_cast<float>(static_cast<int>(lfo::Waveform::Random)));
		set(lfo::Param::Phase, 0.0f);
		const auto atZero = card->curve();
		set(lfo::Param::Phase, 0.5f);
		const auto atHalf = card->curve();
		REQUIRE(atZero.size() == atHalf.size());
		for (size_t i = 0; i < atZero.size(); ++i) {
			CAPTURE(i);
			CHECK(atHalf[i].second == doctest::Approx(atZero[i].second));
		}
	}
}
TEST_CASE("the LFO card draws what SMOOTHING does to a shaped wave — and the tempo it needs to draw a synced one reaches it") {
	// Ideally, Smoothing's effect shows instantly, without an oscilloscope. It can: a filter's response to a periodic input is itself periodic, so
	// the smoothed wave is a static curve rather than something to watch settle.
	//
	// **THE REFERENCE IS A BRUTE-FORCE FILTER RUN, which shares nothing with the card's road except
	// `eval`.** The card reaches its answer through `rateHz` → `smoothingTau` → `smoothPeriodic`, a
	// closed form; this runs the ordinary one-pole recursion over the same wave until it has forgotten
	// where it started. Comparing a closed form against the limit it claims to be is the whole point,
	// and comparing it against itself would be no test at all.
	namespace lfo = feathers::mod::lfo;
	ModFixture f;
	auto* panel = findInTree<ui::ModPanel>(f.root);
	REQUIRE(panel != nullptr);
	const size_t slot = f.bridge.addRackDevice(lfo::kTraits.tag);
	REQUIRE(slot >= 1);
	f.bridge.refresh();
	panel->resized();
	auto* card = const_cast<ui::LfoCard*>(
	    dynamic_cast<const ui::LfoCard*>(panel->cardFrameForTest(slot)));
	REQUIRE(card != nullptr);

	const auto set = [&](lfo::Param p, float v) {
		f.deviceHost.setField(card->device(), static_cast<size_t>(p), v);
		f.bridge.refresh();
		card->updateContent();
	};
	// THE TEMPO, PUBLISHED THE WAY THE ENGINE PUBLISHES IT — through `publishModulated`, so this
	// exercises the same road rather than a test-only back door.
	const auto publishTempo = [&](double secondsPerBeat) {
		feathers::mod::TransportClock clock;
		clock.secondsPerBeat = secondsPerBeat;
		f.bridge.publishModulated(f.bridge.modRack(), feathers::FxChain {},
		                          feathers::RackPlayheads {}, clock);
	};
	// The one-pole, run to its steady state the slow way.
	const auto bruteForce = [](const std::vector<float>& raw, double period, double tau) {
		const size_t m = raw.size();
		const double r = std::exp(-(period / static_cast<double>(m)) / tau);
		const double a = 1.0 - r;
		double y = 0.0;
		for (int rep = 0; rep < 400; ++rep) {
			for (size_t i = 0; i < m; ++i) {
				y = r * y + a * static_cast<double>(raw[i]);
			}
		}
		std::vector<float> out(m);
		for (size_t i = 0; i < m; ++i) {
			y = r * y + a * static_cast<double>(raw[i]);
			out[i] = static_cast<float>(y);
		}
		return out;
	};

	set(lfo::Param::Waveform, static_cast<float>(static_cast<int>(lfo::Waveform::Square)));
	set(lfo::Param::TimeBase, 0.0f); // free-running: the rate is the scale read as Hz
	set(lfo::Param::TimeScale, 2.0f); // 2^2 = 4 Hz, so a period of 250 ms
	const float smoothing = -5.0f;    // 2^-5 = 31.25 ms, a third of the period: plainly visible
	set(lfo::Param::Smoothing, smoothing);
	publishTempo(0.0); // free-running does not need one, and must not depend on one

	const auto drawn = card->curve();
	REQUIRE(drawn.size() >= 2);
	const size_t period = drawn.size() - 1; // the last point repeats the first

	lfo::Settings s;
	s.wave = lfo::Waveform::Square;
	s.timeBase = 0;
	s.timeScale = 2.0f;
	s.smoothing = smoothing;
	std::vector<float> raw(period);
	for (size_t i = 0; i < period; ++i) {
		raw[i] = lfo::eval(s, static_cast<float>(i) / static_cast<float>(period));
	}
	const std::vector<float> want = bruteForce(raw, 1.0 / 4.0, std::exp2(smoothing));
	for (size_t i = 0; i < period; ++i) {
		CAPTURE(i);
		CHECK(drawn[i].second == doctest::Approx(want[i]).epsilon(1e-3));
	}

	// AND IT IS NOT THE RAW WAVE, which is the thing that was wrong: the card drew this identically at
	// every smoothing setting.
	bool differs = false;
	for (size_t i = 0; i < period; ++i) {
		differs = differs || (std::abs(drawn[i].second - raw[i]) > 0.02f);
	}
	CHECK(differs);

	SUBCASE("and a SYNCED wave follows the tempo — which is the whole of what the new seam is for") {
		// **THE END-TO-END PROOF THAT `secondsPerBeat` ARRIVES**: engine → `publishModulated` → the
		// bridge's atomic → `BridgeDeviceHost` → the card. A synced LFO's period in seconds is
		// `division × scale × secondsPerBeat`, so HALVING the tempo doubles the period and leaves the
		// same tau smoothing a wave twice as long — visibly less. Nothing about the device's own
		// settings changes between these two readings; only the transport does.
		set(lfo::Param::TimeBase, 8.0f); // some division — which one does not matter, only that it syncs
		set(lfo::Param::Smoothing, -3.0f); // 125 ms, long enough to bite at either tempo

		publishTempo(0.5); // 120 BPM
		const auto atFast = card->curve();
		publishTempo(1.0); // 60 BPM: every period twice as long
		const auto atSlow = card->curve();
		REQUIRE(atFast.size() == atSlow.size());

		// THE SLOWER TEMPO IS LESS SMOOTHED, because the same tau covers a smaller fraction of a
		// longer period. Asked as SPAN — how much of its range the wave still covers — because that is
		// what a one-pole takes away and it is a single number rather than a shape comparison.
		const auto span = [](const std::vector<std::pair<float, float>>& c) {
			float lo = 1e9f, hi = -1e9f;
			for (const auto& p : c) {
				lo = std::min(lo, p.second);
				hi = std::max(hi, p.second);
			}
			return hi - lo;
		};
		CHECK(span(atSlow) > span(atFast) + 0.02f);

		// **AND WITHOUT A TEMPO IT FALLS BACK TO THE ENGINE'S OWN RULE rather than drawing nothing.**
		// `rateHz` reads the scale as Hz when the host has said nothing, which is what `Shape::tick`
		// has always done — so there is no setting at which the card has to give up, and this asserts
		// that rather than leaving it as a claim in a comment.
		publishTempo(0.0);
		const auto noTempo = card->curve();
		REQUIRE(noTempo.size() == atFast.size());
		bool anyMotion = false;
		for (size_t i = 1; i < noTempo.size(); ++i) {
			anyMotion = anyMotion || (std::abs(noTempo[i].second - noTempo[0].second) > 1e-4f);
		}
		CHECK(anyMotion);
	}
}
TEST_CASE("the plot's grid and the drag's stickiness are the SAME lines") {
	// **THE ONE THING THIS SHARED HEADER EXISTS FOR.** it was asked for the grid and for stickiness on
	// "the time subdiv lines" in one sentence (2026-09-19), and a snap that lands where the picture
	// draws nothing is a control fighting the hand for reasons nothing on screen explains. Both read
	// `kFineDivisions`, so this case is really asking whether that is still true.
	namespace plot = feathers::ui::plot;

	SUBCASE("every division a drag sticks to is a division the grid draws") {
		// Walk the whole axis finely, collect what the snap pulls to, and check each landing is an
		// eighth — the set `drawGrid` puts on screen.
		int pulled = 0;
		for (int i = 0; i <= 1000; ++i) {
			const float ux = static_cast<float>(i) / 1000.0f;
			const float stuck = plot::stickToDivision(ux, 0.01f);
			if (stuck == ux) {
				continue;
			}
			++pulled;
			const float eighths = stuck * static_cast<float>(plot::kFineDivisions);
			CAPTURE(ux);
			CAPTURE(stuck);
			CHECK(std::abs(eighths - std::round(eighths)) < 1e-4f);
		}
		// NOT VACUOUS: the sweep really did get pulled somewhere, rather than passing because nothing
		// ever snapped.
		CHECK(pulled > 100);
	}

	SUBCASE("it sticks, it does not quantise — most of the axis passes through untouched") {
		// "A little stickiness" is the specification. With a 1% radius around nine divisions, well
		// under a fifth of the axis should be captured; a control that rounded everything onto the
		// grid would be a different feature and would fail here.
		int untouched = 0;
		for (int i = 0; i <= 1000; ++i) {
			const float ux = static_cast<float>(i) / 1000.0f;
			untouched += (plot::stickToDivision(ux, 0.01f) == ux) ? 1 : 0;
		}
		CHECK(untouched > 800);
	}

	SUBCASE("the ends are divisions too, because that is where a first and last breakpoint live") {
		CHECK(plot::stickToDivision(0.004f, 0.01f) == doctest::Approx(0.0f));
		CHECK(plot::stickToDivision(0.997f, 0.01f) == doctest::Approx(1.0f));
	}

	SUBCASE("a zero or non-finite tolerance is a pass-through rather than a snap to nowhere") {
		CHECK(plot::stickToDivision(0.31f, 0.0f) == doctest::Approx(0.31f));
		CHECK(plot::stickToDivision(0.31f, -1.0f) == doctest::Approx(0.31f));
		CHECK(std::isnan(plot::stickToDivision(std::nanf(""), 0.01f)));
	}
}
TEST_CASE("backlog HR round 2: grain onsets reach the pad — one larger spark per grain, inked by its gates") {
	// Larger sparks are emitted when grains fire, coloured by the grain's sampled gates. The
	// device publishes one COUNTER per gate class (`Reading::GrainOnsets`); the pad differences them. So
	// the claim is arithmetic: N grains of class c between two ticks is N onset sparks of class c — no
	// more, no fewer, whatever the tick rate — each in `sparkInk`'s colour for its DRY gate and
	// `sparkHole`'s ring for its REVERSE gate, and larger than a level spark.
	namespace sk = fx::sklitter;
	SparkRig r;
	r.puckAt(0.1f, 0.2f);
	ui::PerformPad::GrainOnsetCounts counts { 500, 70, 9000, 3 };
	// THE FIRST READING IS A BASELINE: an hour's worth of grains already counted fire nothing.
	r.tickOnsets(16, 0.2f, 0.0f, counts);
	CHECK(r.pad->onsetSparksEmittedForTest() == std::array<size_t, sk::kGrainOnsetClasses> { 0, 0, 0, 0 });

	// A RHYTHM, with each class firing at its own pace — asserted against a restatement of the sum.
	std::array<size_t, sk::kGrainOnsetClasses> fired {};
	for (int i = 0; i < 60; ++i) {
		for (size_t c = 0; c < sk::kGrainOnsetClasses; ++c) {
			const uint32_t n = (i % static_cast<int>(c + 2)) == 0 ? static_cast<uint32_t>(c % 2 + 1) : 0u;
			counts[c] += n;
			fired[c] += n;
		}
		r.tickOnsets(16, 0.2f, 0.0f, counts);
	}
	CAPTURE(fired[0]);
	CAPTURE(fired[3]);
	REQUIRE(fired[0] > 10u); // not vacuous
	CHECK(r.pad->onsetSparksEmittedForTest() == fired);

	// EACH LIVE ONSET SPARK CARRIES ITS CLASS, AND ITS CLASS PICKS ITS INK AND ITS RING.
	size_t onsetLive = 0;
	for (const ui::PerformPad::Spark& s : r.pad->sparksForTest()) {
		if (s.onset == 0) {
			CHECK(ui::PerformPad::sparkHole(s) == 0.0f);
			continue;
		}
		++onsetLive;
		const size_t c = s.onset - 1u;
		CAPTURE(c);
		CHECK(ui::PerformPad::sparkInk(s)
		      == (sk::grainOnsetClassIsDry(c) ? ui::PerformPad::kSparkOnsetDryInk
		                                      : ui::PerformPad::kSparkOnsetInk));
		CHECK(ui::PerformPad::sparkHole(s)
		      == (sk::grainOnsetClassIsReversed(c) ? ui::PerformPad::kSparkOnsetReverseHole : 0.0f));
		CHECK(!s.fromLane);
	}
	CHECK(onsetLive > 0u);
	// THE CLASS PACKING ITSELF, from the device's side: bit 0 dry, bit 1 reversed.
	CHECK(sk::grainOnsetClass(false, false) == 0u);
	CHECK(sk::grainOnsetClass(true, false) == 1u);
	CHECK(sk::grainOnsetClass(false, true) == 2u);
	CHECK(sk::grainOnsetClass(true, true) == 3u);

	SUBCASE("and an onset spark is drawn LARGER than a level spark, as submitted") {
		visage::Canvas canvas;
		r.pad->draw(canvas);
		const std::vector<float>& quads = r.pad->drawnQuadSidesForTest();
		REQUIRE(quads.size() == r.pad->drawnSparksForTest().size());
		const float level = ui::PerformPad::kSparkSizeBirth * r.pad->scale() * ui::PerformPad::kSparkStreakMax;
		size_t bigger = 0;
		for (float q : quads) {
			if (q >= level * ui::PerformPad::kSparkOnsetSizeScale * 0.999f) {
				++bigger;
			}
		}
		CHECK(bigger > 0u);
		CHECK(ui::PerformPad::kSparkOnsetSizeScale > 1.5f);
	}

	SUBCASE("a counter WRAPS at 2^24 and still counts one grain as one") {
		const size_t before = r.pad->onsetSparksEmittedForTest()[1];
		counts[1] = sk::kGrainOnsetWrap - 1u;
		r.tickOnsets(16, 0.2f, 0.0f, counts); // a jump: re-baselines, emits nothing
		CHECK(r.pad->onsetSparksEmittedForTest()[1] == before);
		counts[1] = 1u; // two grains: 2^24 − 1 → 0 → 1
		r.tickOnsets(16, 0.2f, 0.0f, counts);
		CHECK(r.pad->onsetSparksEmittedForTest()[1] == before + 2u);
	}

	SUBCASE("a SLOW tick throws its grains, capped — a slow UI loses nothing below the cap") {
		const auto before = r.pad->onsetSparksEmittedForTest();
		counts[0] += 9u;  // under the cap: all of them
		counts[3] += 40u; // over it: exactly the cap
		r.tickOnsets(400, 0.2f, 0.0f, counts);
		CHECK(r.pad->onsetSparksEmittedForTest()[0] == before[0] + 9u);
		CHECK(r.pad->onsetSparksEmittedForTest()[3] == before[3] + ui::PerformPad::kMaxOnsetsPerTick);
	}

	SUBCASE("a counter that went BACKWARDS — a reset device — throws NOTHING and re-baselines") {
		const auto before = r.pad->onsetSparksEmittedForTest();
		counts = { 0, 0, 0, 0 }; // the device was rebuilt and counts again from zero
		r.tickOnsets(16, 0.2f, 0.0f, counts);
		CHECK(r.pad->onsetSparksEmittedForTest() == before);
		counts[2] += 3u; // and from the new baseline it counts normally
		r.tickOnsets(16, 0.2f, 0.0f, counts);
		CHECK(r.pad->onsetSparksEmittedForTest()[2] == before[2] + 3u);
	}

	SUBCASE("at SILENCE the counters may run and NOTHING is asked — the silence promise, with onsets") {
		// A device at Mix 0 still counts its grains; the levels say nobody hears them.
		r.drain();
		const auto before = r.pad->onsetSparksEmittedForTest();
		size_t asked = 0;
		for (int i = 0; i < 200; ++i) {
			for (uint32_t& c : counts) {
				c += 1u;
			}
			if (r.tickOnsets(16, 0.0f, 0.0f, counts)) {
				++asked;
			}
		}
		CHECK(asked == 0u);
		CHECK(r.pad->sparksForTest().empty());
		CHECK(r.pad->onsetSparksEmittedForTest() == before);
		// NOT VACUOUS: the same counters with sound DO throw, and the backlog is not released — only
		// what fires after the sound returns.
		for (uint32_t& c : counts) {
			c += 1u;
		}
		CHECK(r.tickOnsets(16, 0.3f, 0.0f, counts));
		const auto after = r.pad->onsetSparksEmittedForTest();
		for (size_t c = 0; c < sk::kGrainOnsetClasses; ++c) {
			CHECK(after[c] == before[c] + 1u);
		}
	}
}
TEST_CASE("backlog HR round 2: kMaxSparks holds against a flood of onsets on top of both level emitters") {
	SparkRig r;
	r.puckAt(0.0f, 0.0f);
	ui::PerformPad::GrainOnsetCounts counts {};
	r.tickOnsets(16, 1.0f, 1.0f, counts);
	size_t offered = 0;
	size_t peak = 0;
	for (int i = 0; i < 120; ++i) {
		for (uint32_t& c : counts) {
			c += ui::PerformPad::kMaxOnsetsPerTick; // the most a tick believes, every class, every tick
		}
		offered += 4u * ui::PerformPad::kMaxOnsetsPerTick;
		r.tickOnsets(16, 1.0f, 1.0f, counts);
		peak = std::max(peak, r.pad->liveSparks());
		REQUIRE(r.pad->liveSparks() <= ui::PerformPad::kMaxSparks);
	}
	size_t onsets = 0;
	for (size_t n : r.pad->onsetSparksEmittedForTest()) {
		onsets += n;
	}
	CAPTURE(peak);
	CAPTURE(onsets);
	CAPTURE(offered);
	// NOT VACUOUS: the cap really bit — the pad reached it and refused onsets it was offered.
	CHECK(peak == ui::PerformPad::kMaxSparks);
	CHECK(onsets < offered);
}
TEST_CASE("backlog HR round 2: a SPLIT is one spark whose child lobe grows out and drifts off, inside its quad") {
	// **The split is a single spark with a special, growing shape.** The spare
	// per-quad float is the signed child kick; everything else derives. The fragment stage cannot be
	// asserted headlessly, so what is asserted is its C++ half — `sparkSplitFor`, `sparkChildOffsetAt`,
	// `sparkQuadSide` — and the drawn record, which both paths fill from them.
	using PP = ui::PerformPad;

	SUBCASE("the random decision: level sparks never, onset sparks at exactly the chance, and the float is bounded") {
		// The chance is ZERO since the repeat emitter gave the split a meaning (see `kSparkSplitChance`);
		// asserted against the constant so turning it back on keeps this case honest.
		size_t splits = 0;
		size_t left = 0;
		constexpr int kN = 4096;
		for (int i = 0; i < kN; ++i) {
			const float seed = (static_cast<float>(i) + 0.5f) / static_cast<float>(kN);
			CHECK(PP::sparkSplitFor(seed, false) == 0.0f); // a level spark never splits
			const float k = PP::sparkSplitFor(seed, true);
			// THE SPARE FLOAT'S MEANING: zero, or exactly ± the kick — nothing else.
			CHECK((k == 0.0f || k == PP::kSparkSplitKick || k == -PP::kSparkSplitKick));
			if (k != 0.0f) {
				++splits;
				left += k > 0.0f ? 1u : 0u;
			}
		}
		const double share = static_cast<double>(splits) / kN;
		CAPTURE(share);
		CHECK(std::abs(share - static_cast<double>(PP::kSparkSplitChance)) < 0.02);
		if (splits > 0) {
			CHECK(left > splits / 3);
			CHECK(left < 2 * splits / 3);
		}
	}

	SUBCASE("the repeat decision: EVERY repeat spark splits, both ways, the kick within its clamp") {
		size_t left = 0;
		constexpr int kN = 1024;
		for (int i = 0; i < kN; ++i) {
			const float seed = (static_cast<float>(i) + 0.5f) / static_cast<float>(kN);
			const float k = PP::sparkRepeatKick(seed, 0.2f);
			CHECK(std::abs(k) >= PP::kSparkRepeatKickMin);
			CHECK(std::abs(k) <= PP::kSparkRepeatKickMax);
			left += k > 0.0f ? 1u : 0u;
		}
		CHECK(left > kN / 3);
		CHECK(left < 2 * kN / 3);
		// AN UNKNOWN PERIOD takes the minimum rather than dividing by nothing.
		CHECK(std::abs(PP::sparkRepeatKick(0.2f, 0.0f)) == PP::kSparkRepeatKickMin);
		CHECK(std::abs(PP::sparkRepeatKick(0.2f, std::nanf(""))) == PP::kSparkRepeatKickMin);
	}

	SUBCASE("the child: nothing before the split, then out along the kick, bounded by |kick|/k, inside the quad") {
		PP::Spark s;
		s.vx = 0.7f;
		s.vy = -0.2f;
		s.life = PP::kSparkLife;
		s.seed = 0.5f;
		s.onset = 1;
		s.split = PP::kSparkSplitKick;
		const float ts = PP::sparkSplitAt(s);
		CHECK(ts >= s.life * PP::kSparkSplitAtEarliest);
		CHECK(ts <= s.life * PP::kSparkSplitAtLatest);
		// BEFORE: no child at all.
		CHECK(PP::sparkChildGrowthAt(s, ts * 0.99f) == 0.0f);
		const auto [bx, by] = PP::sparkChildOffsetAt(s, ts * 0.99f);
		CHECK(bx == 0.0f);
		CHECK(by == 0.0f);

		// AFTER: an INDEPENDENT restatement — the kick rotated off the heading by the angle, integrated
		// under the drag from the split.
		const float t = ts + 0.1f;
		const double heading = std::atan2(static_cast<double>(s.vy), static_cast<double>(s.vx));
		const double a = heading + static_cast<double>(PP::kSparkSplitAngle);
		const double reach = (1.0 - std::exp(-static_cast<double>(PP::kSparkDrag) * (t - ts)))
		                     / static_cast<double>(PP::kSparkDrag) * PP::kSparkSplitKick;
		const auto [cx, cy] = PP::sparkChildOffsetAt(s, t);
		CHECK(cx == doctest::Approx(std::cos(a) * reach).epsilon(1e-4));
		CHECK(cy == doctest::Approx(std::sin(a) * reach).epsilon(1e-4));
		// THE OTHER SIDE leaves mirrored about the heading.
		PP::Spark m = s;
		m.split = -PP::kSparkSplitKick;
		const auto [mx, my] = PP::sparkChildOffsetAt(m, t);
		const double b = heading - static_cast<double>(PP::kSparkSplitAngle);
		CHECK(mx == doctest::Approx(std::cos(b) * reach).epsilon(1e-4));
		CHECK(my == doctest::Approx(std::sin(b) * reach).epsilon(1e-4));
		// THE LOBE GROWS, from nothing to whole over `kSparkSplitGrow`.
		CHECK(PP::sparkChildGrowthAt(s, ts + 0.5f * PP::kSparkSplitGrow) > 0.0f);
		CHECK(PP::sparkChildGrowthAt(s, ts + 0.5f * PP::kSparkSplitGrow) < 1.0f);
		CHECK(PP::sparkChildGrowthAt(s, ts + PP::kSparkSplitGrow) == doctest::Approx(1.0f));

		// **AND AT EVERY AGE THE LOBE IS INSIDE THE QUAD IT WAS GIVEN** — or it clips. In pixels, on a
		// real disk radius and a real spark size (onset-scaled), at the farthest the parent's streak
		// and the child can each reach.
		const float radius = 150.0f;
		const float size = PP::kSparkSizeBirth * PP::kSparkOnsetSizeScale;
		const float half = 0.5f * PP::sparkQuadSide(size, s.split * radius);
		for (int i = 0; i <= 200; ++i) {
			const float age = s.life * static_cast<float>(i) / 200.0f;
			const auto [ox, oy] = PP::sparkChildOffsetAt(s, age);
			const float lobe = 0.5f * size * PP::kSparkSplitChildScale * PP::sparkChildGrowthAt(s, age);
			CAPTURE(age);
			CHECK(std::hypot(ox, oy) * radius + lobe <= half);
		}
		CHECK(0.5f * size * PP::kSparkStreakMax <= half);
		// THE BOUND IS REACHED FOR, not loose by an order: the farthest lobe edge is most of the half.
		CHECK(PP::kSparkSplitKick / PP::kSparkDrag * radius > 0.5f * half);
		// AND A SPARK THAT NEVER SPLITS KEEPS ITS OLD, SMALL QUAD — the fill is paid only by splitters.
		CHECK(PP::sparkQuadSide(size, 0.0f) == doctest::Approx(size * PP::kSparkStreakMax));
		CHECK(PP::sparkQuadSide(size, s.split * radius) > 3.0f * PP::sparkQuadSide(size, 0.0f));
	}

	SUBCASE("on the pad: repeat sparks split, onset sparks do not, children DRAWN where the closed form puts them") {
		SparkRig r;
		r.puckAt(0.0f, 0.0f);
		ui::PerformPad::SparkEvents ev;
		ev.repeatPeriod = 0.125f;
		r.tickEvents(16, 0.5f, 0.0f, ev);
		for (int i = 0; i < 30; ++i) {
			ev.onsets[i % 4] += 2u;
			if (i % 2 == 0) {
				ev.repeats += 1u; // a repeat every other tick
			}
			r.tickEvents(16, 0.5f, 0.0f, ev);
		}
		size_t onsetBorn = 0;
		for (size_t n : r.pad->onsetSparksEmittedForTest()) {
			onsetBorn += n;
		}
		REQUIRE(onsetBorn == 60u);
		CHECK(r.pad->repeatSparksEmittedForTest() == 15u);
		// **ONE SPLIT PER REPEAT, and the random split's share of the onsets on top** — zero by default.
		CAPTURE(r.pad->splitSparksEmittedForTest());
		CHECK(r.pad->splitSparksEmittedForTest()
		      == 15u + static_cast<size_t>(std::lround(PP::kSparkSplitChance * 60.0f)));

		// Every live spark's split field is its kind's function of its own seed.
		for (const PP::Spark& s : r.pad->sparksForTest()) {
			if (s.repeat) {
				CHECK(s.onset == 0);
				CHECK(s.split == PP::sparkRepeatKick(s.seed, ev.repeatPeriod));
				CHECK(PP::sparkInk(s) == PP::kSparkRepeatInk);
			}
			else {
				CHECK(s.split == PP::sparkSplitFor(s.seed, s.onset != 0));
			}
		}
		visage::Canvas canvas;
		r.pad->draw(canvas);
		const std::vector<std::pair<float, float>> gpuChildren = r.pad->drawnChildrenForTest();
		REQUIRE(!gpuChildren.empty()); // NOT VACUOUS: children really were due and drawn
		// BOTH PATHS RECORD THE SAME CHILDREN, bit for bit — the CPU reference draws the split too.
		r.pad->setSparkGpuForTest(false);
		r.pad->draw(canvas);
		CHECK(r.pad->drawnChildrenForTest() == gpuChildren);
		// AND THEY ARE AWAY FROM EVERY PARENT — a child recorded at its parent's point is no split.
		size_t apart = 0;
		const std::vector<std::pair<float, float>>& parents = r.pad->drawnSparksForTest();
		for (const auto& c : gpuChildren) {
			float nearest = 1.0e9f;
			for (const auto& p : parents) {
				nearest = std::min(nearest, std::hypot(c.first - p.first, c.second - p.second));
			}
			if (nearest > 0.5f) {
				++apart;
			}
		}
		CHECK(apart > gpuChildren.size() / 2);
	}
}
TEST_CASE("backlog HR round 2: a repeat spark's child PARTS about when the next repeat fires") {
	// Repeat slices get the split spark, splitting roughly every repeat. A repeat
	// spark splits at birth; its kick is chosen so the two centres are `kSparkRepeatPart` apart one
	// PERIOD later. Asserted through `sparkChildOffsetAt` (the C++ half of the shader's formula) at
	// t = period, against the constant — and where the clamp binds, on the side it must err.
	using PP = ui::PerformPad;
	for (const float period : { 0.04f, 0.0625f, 0.125f, 0.25f, 0.5f, 1.0f, 2.0f }) {
		CAPTURE(period);
		PP::Spark s;
		s.vx = 0.3f;
		s.vy = 0.4f;
		s.life = PP::kSparkLife * (1.0f + PP::kSparkLifeJitter); // the longest, so t = period is inside it
		s.seed = 0.3f;
		s.repeat = true;
		s.split = PP::sparkRepeatKick(s.seed, period);
		CHECK(PP::sparkSplitAt(s) == 0.0f); // the repeat IS the split
		const float t = std::min(period, s.life);
		const auto [cx, cy] = PP::sparkChildOffsetAt(s, t);
		const float apart = std::hypot(cx, cy);
		// THE INDEPENDENT RESTATEMENT of the unclamped kick.
		const double k = PP::kSparkDrag;
		const double want = PP::kSparkRepeatPart * k / (1.0 - std::exp(-k * period));
		CAPTURE(want);
		CAPTURE(apart);
		if (want > PP::kSparkRepeatKickMax) {
			CHECK(std::abs(s.split) == PP::kSparkRepeatKickMax);
			CHECK(apart < PP::kSparkRepeatPart); // a very fast repeat parts a little LATE, never early
		}
		else if (want < PP::kSparkRepeatKickMin) {
			CHECK(std::abs(s.split) == PP::kSparkRepeatKickMin);
			CHECK(apart >= PP::kSparkRepeatPart * 0.999f); // a slow one has parted by then
		}
		else {
			CHECK(std::abs(s.split) == doctest::Approx(want).epsilon(1e-4));
			CHECK(apart == doctest::Approx(PP::kSparkRepeatPart).epsilon(1e-3));
		}
		// AND THE LOBE HAS GROWN BY THE TIME IT PARTS, whenever the period allows it at all.
		if (period >= PP::kSparkRepeatGrow) {
			CHECK(PP::sparkChildGrowthAt(s, period) == doctest::Approx(1.0f));
		}
		// AND IT IS INSIDE ITS QUAD AT EVERY AGE — the repeat kick's clamp is what sizes it.
		const float radius = 150.0f;
		const float size = PP::kSparkSizeBirth * PP::kSparkRepeatSizeScale;
		const float half = 0.5f * PP::sparkQuadSide(size, s.split * radius);
		for (int i = 0; i <= 100; ++i) {
			const float age = s.life * static_cast<float>(i) / 100.0f;
			const auto [ox, oy] = PP::sparkChildOffsetAt(s, age);
			const float lobe = 0.5f * size * PP::kSparkSplitChildScale * PP::sparkChildGrowthAt(s, age);
			CHECK(std::hypot(ox, oy) * radius + lobe <= half);
		}
	}
	// NOT VACUOUS: the sweep reached all three branches — the clamp at both ends, and the paced middle.
	CHECK(PP::kSparkRepeatPart * PP::kSparkDrag / (1.0f - std::exp(-PP::kSparkDrag * 0.04f)) > PP::kSparkRepeatKickMax);
	CHECK(PP::kSparkRepeatPart * PP::kSparkDrag / (1.0f - std::exp(-PP::kSparkDrag * 2.0f)) < PP::kSparkRepeatKickMin);
}
TEST_CASE("backlog HR round 2: repeats reach the pad — one splitting spark each, and none at silence") {
	using PP = ui::PerformPad;
	SparkRig r;
	r.puckAt(0.2f, 0.0f);
	PP::SparkEvents ev;
	ev.repeats = 12345; // an hour of repeats already counted: a baseline, not a burst
	ev.repeatPeriod = 0.25f;
	r.tickEvents(16, 0.3f, 0.0f, ev);
	CHECK(r.pad->repeatSparksEmittedForTest() == 0u);
	for (int i = 0; i < 40; ++i) {
		ev.repeats += (i % 4 == 0) ? 1u : 0u;
		r.tickEvents(16, 0.3f, 0.0f, ev);
	}
	CHECK(r.pad->repeatSparksEmittedForTest() == 10u);
	// A backwards count (the device was rebuilt) throws nothing.
	ev.repeats = 0;
	r.tickEvents(16, 0.3f, 0.0f, ev);
	CHECK(r.pad->repeatSparksEmittedForTest() == 10u);

	SUBCASE("at silence the repeat counter may run and NOTHING is asked") {
		r.drain();
		size_t asked = 0;
		for (int i = 0; i < 100; ++i) {
			ev.repeats += 1u;
			if (r.tickEvents(16, 0.0f, 0.0f, ev)) {
				++asked;
			}
		}
		CHECK(asked == 0u);
		CHECK(r.pad->repeatSparksEmittedForTest() == 10u);
		// NOT VACUOUS: with sound, the next repeat throws.
		ev.repeats += 1u;
		CHECK(r.tickEvents(16, 0.3f, 0.0f, ev));
		CHECK(r.pad->repeatSparksEmittedForTest() == 11u);
	}
}
TEST_CASE("the onset inks are legibly apart — from the stars, from the level inks, and from each other") {
	const auto apart = [](uint32_t a, uint32_t b) {
		int sum = 0;
		for (int shift : { 16, 8, 0 }) {
			sum += std::abs(static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu));
		}
		return sum;
	};
	constexpr int kApart = 100; // the floor the level inks' case set, for its reason
	using PP = ui::PerformPad;
	for (uint32_t onset : { PP::kSparkOnsetInk, PP::kSparkOnsetDryInk }) {
		for (uint32_t other : { PP::kStarInk, PP::kSparkInk, PP::kSparkLaneInk }) {
			CAPTURE(apart(onset, other));
			CHECK(apart(onset, other) >= kApart);
		}
	}
	CAPTURE(apart(PP::kSparkOnsetInk, PP::kSparkOnsetDryInk));
	CHECK(apart(PP::kSparkOnsetInk, PP::kSparkOnsetDryInk) >= kApart);
	// AND THE REPEAT INK, against all five others and the stars.
	for (uint32_t other : { PP::kStarInk, PP::kSparkInk, PP::kSparkLaneInk, PP::kSparkOnsetInk,
	                        PP::kSparkOnsetDryInk }) {
		CAPTURE(apart(PP::kSparkRepeatInk, other));
		CHECK(apart(PP::kSparkRepeatInk, other) >= kApart);
	}
	// NOT VACUOUS: the palette's gold, kSeed7, the obvious third hue, fails against the wet ember.
	CHECK(apart(feathers::tokens::kSeed7, PP::kSparkInk) < kApart);
	// AND THE REVERSE GATE IS A SHAPE, a real hole, not a hue.
	CHECK(PP::kSparkOnsetReverseHole > 0.2f);
	CHECK(PP::kSparkOnsetReverseHole < 0.9f);
}
TEST_CASE("backlog HR MEASURE: what the CPU-side spark walk actually costs per frame" * doctest::skip(false)) {
	// **THE NUMBER THE GPU MANDATE TURNS ON, taken from the instrument rather than argued from the
	// shape.** The aim is that spark translation and any glitter properties are, ideally, done purely
	// on the GPU, and "ideally" is the operative word — so the honest answer is what the CPU road costs, measured, and
	// what the GPU road would cost to build. This is the first half.
	//
	// **IT IS A MEASUREMENT, NOT A BOUND — it asserts nothing about time** and cannot go red under load,
	// which is deliberate: a MICROSECOND assertion on a shared host is a flake, and this file already
	// carries two suites that are pinned for that reason. It prints, and the reading goes in backlog HR.
	SparkRig r;
	r.puckAt(0.0f, 0.0f);
	// Fill to the steady state first, so the walk being timed is over a full population rather than
	// over a vector still growing into one.
	for (int i = 0; i < 200; ++i) {
		r.tick(16, 1.0f, 1.0f);
	}
	const size_t population = r.pad->sparksForTest().size();
	REQUIRE(population > 40); // not vacuous: there is a real population to walk

	constexpr int kFrames = 20000;
	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < kFrames; ++i) {
		r.tick(16, 1.0f, 1.0f);
	}
	const auto t1 = std::chrono::steady_clock::now();
	const double perFrameNs =
	    static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count())
	    / static_cast<double>(kFrames);

	MESSAGE("sparks alive: " << population);
	MESSAGE("updateSparks: " << perFrameNs << " ns/frame");
	// A 60 Hz frame is 16,666,667 ns. Printed as the fraction, because that is the quantity "keep it
	// lightweight" is actually about.
	MESSAGE("share of a 60fps frame: " << (perFrameNs / 16666666.7 * 100.0) << " %");
	CHECK(perFrameNs > 0.0); // the clock moved, so the number above is a number
}
TEST_CASE("backlog GS: SKLITTER's Rate reads as the note value it plays, per mode") {
	// **Rate did not show the grain subdivision.** Rate is a 0..1 position and it printed as a
	// percentage; what it MEANS is a note value, and which one depends on the Mode. Every expected
	// string below is restated here from the definitions — 1/N where N is a whole note over the slice —
	// and none is read back from the device's own helpers.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	REQUIRE(slot == 1);
	lane->resized();
	const DeviceInstanceId device = f.bridge.fxDeviceAt(slot);
	const auto rate = static_cast<size_t>(sc::Param::Rate);
	ui::DeviceControl control(f.deviceHost, device, sc::kDescriptor, rate);
	const auto at = [&](sc::Mode mode, float position) {
		f.bridge.setDeviceControl(device, static_cast<size_t>(sc::Param::Mode), static_cast<float>(mode));
		f.bridge.setDeviceControl(device, rate, position);
		return control.text();
	};
	// Burst's length: a quarter at 0, four halvings to a sixty-fourth at 1 — `resolveSliceFrames`.
	const auto lengthName = [](double position) {
		const double beats = 1.0 * std::exp2(-4.0 * position);
		return "1/" + std::to_string(std::lround(4.0 / beats));
	};

	SUBCASE("Burst's five divisions name themselves exactly") {
		CHECK(at(sc::Mode::Burst, 0.0f) == "1/4");
		CHECK(at(sc::Mode::Burst, 0.25f) == "1/8");
		CHECK(at(sc::Mode::Burst, 0.5f) == "1/16");
		CHECK(at(sc::Mode::Burst, 0.75f) == "1/32");
		CHECK(at(sc::Mode::Burst, 1.0f) == "1/64");
	}

	SUBCASE("between two divisions Burst reads the honest reciprocal") {
		// 0.6 is 4 x 2^2.4 = 21.1 — not a sixteenth, not a thirty-second.
		CHECK(lengthName(0.6) == "1/21"); // the restatement is not vacuous
		CHECK(at(sc::Mode::Burst, 0.6f) == lengthName(0.6));
	}

	SUBCASE("a looper mode reads the COUNT the bar is cut into, which the Burst formula gets wrong") {
		// The reference's ladder in its 0..50 units: <4 one bar, <9 two, <16 four, <22 eight, <28
		// sixteen, else thirty-two. 0.4 is 20 units: EIGHT slices, so an eighth.
		CHECK(at(sc::Mode::Shuffle, 0.4f) == "1/8");
		CHECK(lengthName(0.4) == "1/12"); // what a one-formula readout would have printed
		CHECK(at(sc::Mode::Shuffle, 0.5f) == "1/16"); // the default
		CHECK(at(sc::Mode::Shuffle, 0.0f) == "1 bar");
		CHECK(at(sc::Mode::Shuffle, 1.0f) == "1/32"); // the ladder's top, NOT Burst's 1/64
		// Pitch halves the count — a pitched grain needs time to be heard as a pitch.
		CHECK(at(sc::Mode::Pitch, 0.5f) == "1/8");
	}

	SUBCASE("Repeat reads its own curve: how many whole slices of bar x (1 - r)^2 fit in the bar") {
		// (1 - 0.45)^2 = 0.3025 of a bar, so 3 whole slices fit: a third.
		CHECK(static_cast<long>(1.0 / ((1.0 - 0.45) * (1.0 - 0.45))) == 3);
		CHECK(at(sc::Mode::Repeat, 0.45f) == "1/3");
		CHECK(at(sc::Mode::Repeat, 0.0f) == "1 bar");
	}

	SUBCASE("during a live gesture the DEVICE's number wins over the knob's") {
		// The knob says a sixteenth; the device says it is playing 1/21 — what a quantized press plus
		// a turn produces (`test_fx_sklitter.cpp` asserts the device's side of this).
		CHECK(at(sc::Mode::Burst, 0.5f) == "1/16");
		std::vector<float> values(sc::kReadingCount, 0.0f);
		values[static_cast<size_t>(sc::Reading::Status)] = 3.0f; // playing
		values[static_cast<size_t>(sc::Reading::RateDivision)] = 21.1f;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
		CHECK(control.text() == "1/21");
		// AND AT REST THE KNOB AGAIN, whatever was last published — so a turn reads with no audio.
		values[static_cast<size_t>(sc::Reading::Status)] = 0.0f;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
		CHECK(control.text() == "1/16");
	}

	SUBCASE("the card's own Rate knob prints through the same road") {
		lane->scrollCardIntoView(slot);
		lane->resized();
		auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
		REQUIRE(card != nullptr);
		at(sc::Mode::Shuffle, 0.4f);
		CHECK(card->controlForTest(rate).text() == "1/8");
		// ANOTHER SKLITTER CONTROL IS DECLINED BY THE HOOK and prints its unit exactly as before.
		const auto mix = static_cast<size_t>(sc::Param::Mix);
		const ui::DeviceControl& mixControl = card->controlForTest(mix);
		CHECK(mixControl.text() == ui::formatValue(mixControl.desc(), mixControl.value()));
	}

	SUBCASE("a device with no text hook prints byte-identically to its declared unit") {
		namespace flt = feathers::fx::filter;
		REQUIRE(flt::kDescriptor.text == nullptr);
		const size_t filterSlot = f.bridge.addFxDevice(flt::kTraits.tag);
		const DeviceInstanceId filter = f.bridge.fxDeviceAt(filterSlot);
		for (size_t i = 0; i < flt::kDescriptor.params.size(); ++i) {
			CAPTURE(i);
			ui::DeviceControl c(f.deviceHost, filter, flt::kDescriptor, i);
			CHECK(c.text() == ui::formatValue(c.desc(), c.value()));
		}
	}
}

namespace {
//--------------------------------------------------------------------------------------------------//
// backlog IG — GROOVE, THE SIDE COLUMN, AND THE PERFORM RAIL AT ITS NEW FLOOR.                      //
//--------------------------------------------------------------------------------------------------//

/// The columns a panel's visible cells occupy, read off the layout. By cell CENTRE over the column pitch,
/// because a pad-shaped chip is inset 2px inside its column and a raw x would count it as a column.
std::set<int> columnsOf(const ui::GenericExpanded& panel) {
	std::set<int> columns;
	for (const visage::Frame* cell : panel.cellsForTest()) {
		if (cell->isVisible()) {
			const float centre = static_cast<float>(cell->x()) + 0.5f * static_cast<float>(cell->width());
			columns.insert(static_cast<int>(std::floor((centre - 12.0f) / ui::GenericExpanded::kSideColumnWidth)));
		}
	}
	return columns;
}
} // namespace

TEST_CASE("backlog IG: SKLITTER's CONTROLS grid is FIVE columns — the side column holds Groove, Transport "
          "Locked, Seq Length and Buffer Length top to bottom — and a device that declares none keeps FOUR") {
	namespace sc = feathers::fx::sklitter;
	namespace flt = feathers::fx::filter;
	ModFixture f;
	ui::WidgetRegistry reg;
	const DeviceInstanceId id = f.bridge.fxDeviceAt(f.bridge.addFxDevice(sc::kTraits.tag));
	ui::GenericExpanded panel(f.deviceHost, reg, id, sc::kDescriptor, nullptr);
	// A GROOVE THAT IS ON.
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Swing8));
	for (const sc::Mode mode : { sc::Mode::Shuffle, sc::Mode::Pitch, sc::Mode::Repeat }) {
		CAPTURE(static_cast<int>(mode));
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(mode));
		const visage::Point want = panel.minimumSize();
		panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
		panel.resized();
		CHECK(columnsOf(panel).size() == 5u);
		// FOUR SINCE THE IO ADDENDUM: Groove is one zone-knob cell (`modeCell`), Amount and Free Phase inside it.
		CHECK(panel.liveSideCells() == 4u);
		CHECK(want.x == doctest::Approx(ui::GenericExpanded::kGridWidth + ui::GenericExpanded::kSideColumnWidth));
		// THE SIDE COLUMN'S CELLS, IN THE DECLARED ORDER, AT ONE X, DESCENDING.
		const float sideX = 12.0f + 4.0f * ui::GenericExpanded::kSideColumnWidth;
		float lastY = -1.0f;
		for (const uint8_t p : sc::kSideColumn) {
			CAPTURE(static_cast<int>(p));
			const auto& indices = panel.paramIndicesForTest();
			const auto at = std::find(indices.begin(), indices.end(), static_cast<size_t>(p));
			REQUIRE(at != indices.end());
			const visage::Frame* cell = panel.cellsForTest()[static_cast<size_t>(at - indices.begin())];
			CHECK(cell->isVisible());
			CHECK(static_cast<float>(cell->x()) == doctest::Approx(sideX));
			CHECK(static_cast<float>(cell->y()) > lastY);
			lastY = static_cast<float>(cell->y());
		}
		// AND NO GRID CELL SHARES THE SIDE COLUMN'S X — nothing flowed into it.
		for (size_t n = 0; n < panel.cellsForTest().size(); ++n) {
			const visage::Frame* cell = panel.cellsForTest()[n];
			if (cell->isVisible() && !panel.inSideColumn(panel.paramIndicesForTest()[n])) {
				CHECK(static_cast<float>(cell->x()) < sideX - 1.0f);
			}
		}
	}
	// AT STRAIGHT THE GROOVE CELL STAYS — its selector is how Straight is left — and prints "straight".
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Straight));
	CHECK(panel.liveSideCells() == 4u);
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Swing8));
	// IN GRAIN GROOVE IS GATED OFF — the one cell; IN BURST ALL FOUR ARE, AND THE COLUMN GOES.
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Grain));
	CHECK(panel.liveSideCells() == 3u);
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Burst));
	CHECK(panel.liveSideCells() == 0u);
	CHECK(panel.minimumSize().x == doctest::Approx(ui::GenericExpanded::kGridWidth));

	// A DEVICE THAT DECLARES NO SIDE COLUMN: four across, byte for byte the panel it was.
	REQUIRE(flt::kTraits.sideColumn.empty());
	const DeviceInstanceId filter = f.bridge.fxDeviceAt(f.bridge.addFxDevice(flt::kTraits.tag));
	ui::GenericExpanded plain(f.deviceHost, reg, filter, flt::kDescriptor, nullptr);
	const visage::Point want = plain.minimumSize();
	plain.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	plain.resized();
	REQUIRE(plain.liveCells() > 4u); // NOT VACUOUS: enough cells that a fifth column could have appeared
	CHECK(columnsOf(plain).size() == 4u);
	CHECK(plain.liveSideCells() == 0u);
	CHECK(want.x == doctest::Approx(ui::GenericExpanded::kGridWidth));
}
TEST_CASE("backlog IO: Groove is a MODE box with a menu of ten feels and an AMOUNT box that prints the mode's own terms") {
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	ui::WidgetRegistry reg;
	const DeviceInstanceId id = f.bridge.fxDeviceAt(f.bridge.addFxDevice(sc::kTraits.tag));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Shuffle));
	ui::GenericExpanded panel(f.deviceHost, reg, id, sc::kDescriptor, nullptr);
	// THE MODE IS THE GROOVE CELL'S SELECTOR SINCE THE IO ADDENDUM (`modeCell`): a zone knob's `‹ name ›`.
	ui::ModeValueCell* groove = sklitterCellFor<ui::ModeValueCell>(panel, sc::Param::GrooveMode);
	ui::ValueBox* amount = sklitterCellFor<ui::ValueBox>(panel, sc::Param::GrooveAmount);
	REQUIRE(groove != nullptr);
	REQUIRE(amount != nullptr);
	ui::ZoneSelect* mode = &groove->select();
	// THE MODE IS A LIST ON DEMAND, NOT A CYCLING CHIP — every feel is on its menu before one is applied.
	REQUIRE(mode->zoneCount() == 10u);
	const std::array<const char*, 10> names { "Straight", "Swing 4",   "Swing 8", "Swing 16", "Groove 4",
		                                      "Groove 8", "Groove 16", "Free 4",  "Free 8",   "Free 16" };
	for (size_t i = 0; i < names.size(); ++i) {
		CAPTURE(i);
		CHECK(mode->captionOf(i) == names[i]);
	}
	// THE MODE IS NOT A DESTINATION AND THE AMOUNT IS — no Choice on this device is one.
	CHECK_FALSE(sc::kDescriptor.params[static_cast<size_t>(sc::Param::GrooveMode)].modulatable);
	CHECK(sc::kDescriptor.params[static_cast<size_t>(sc::Param::GrooveAmount)].modulatable);

	struct Case {
		sc::GrooveMode mode;
		double amount;
		const char* readout;
	};
	// Swing runs the MPC's 50 → 75% over the amount, printed to a tenth: amount = (pct − 50) / 25. The MPC's
	// classic settings are exact readings. Free prints its phase.
	for (const Case c : { Case { sc::GrooveMode::Swing16, 0.32, "58.0%" },
	                      Case { sc::GrooveMode::Swing8, 0.64, "66.0%" },
	                      Case { sc::GrooveMode::Swing4, 0.84, "71.0%" },
	                      Case { sc::GrooveMode::Swing4, 1.0, "75.0%" },
	                      Case { sc::GrooveMode::Groove8, 0.4, "40.0%" },
	                      Case { sc::GrooveMode::Groove16, 1.0, "100.0%" } }) {
		CAPTURE(static_cast<int>(c.mode));
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(c.mode));
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveAmount), static_cast<float>(c.amount));
		CHECK(mode->captionText() == names[static_cast<size_t>(c.mode)]);
		CHECK(amount->readout() == c.readout);
	}
	// FREE PHASE PRINTS ITS POSITION AS THE NUMBER IT IS, to a thousandth, however far along — a VALUE BOX with
	// the digit drag, not a `GammaBox`, though its range is a drag window (backlog IO).
	ui::ValueBox* phase = sklitterCellFor<ui::ValueBox>(panel, sc::Param::FreePhase);
	REQUIRE(phase != nullptr);
	CHECK(dynamic_cast<ui::GammaBox*>(phase) == nullptr);
	CHECK(sc::kDescriptor.params[static_cast<size_t>(sc::Param::FreePhase)].modulatable);
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Free8));
	for (const auto& [value, text] : { std::pair { 0.375, "0.375" }, std::pair { 1.375, "1.375" },
	                                   std::pair { 12.05, "12.050" } }) {
		CAPTURE(value);
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::FreePhase), static_cast<float>(value));
		CHECK(phase->readout() == text);
	}
	// AND IT FITS THE BOX IT IS PRINTED IN, at the widest amount and widest mode name this control has — each
	// box asked in a mode that shows it (the amount and the phase are never laid out together).
	const auto fits = [&](sc::GrooveMode m, ui::ValueBox* box, const char* text) {
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(m));
		const visage::Point want = panel.minimumSize();
		panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
		panel.resized();
		const std::string s(text);
		const float wide = ui::font(ui::kKnobReadoutSize).stringWidth(std::u32string(s.begin(), s.end()));
		CAPTURE(text);
		CHECK(wide < static_cast<float>(box->width()));
	};
	fits(sc::GrooveMode::Groove16, amount, "100.0%");
	fits(sc::GrooveMode::Free8, phase, "1234.567");
	// THE WIDEST FEEL NAME FITS BETWEEN THE SELECTOR'S ARROWS.
	{
		const std::string s = "Groove 16";
		const float wide = ui::font(ui::kKnobReadoutSize).stringWidth(std::u32string(s.begin(), s.end()));
		CHECK(wide < mode->captionBounds().width());
	}
}
TEST_CASE("backlog IO: Groove's amount cannot roll into another mode, a mode change leaves it alone, it hides "
          "in Straight, and a grabbed digit moves that place of what it prints") {
	// The groove zone controls are fenced, so the offset explores a wider space within one groove setting
	// without rolling into the next, with a fractional-precision drag for finer control — a mode plus a
	// phase offset. Every claim is asserted as the stored VALUES the device decodes and as the text the box
	// prints — never as a rendered change.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	ui::WidgetRegistry reg;
	const DeviceInstanceId id = f.bridge.fxDeviceAt(f.bridge.addFxDevice(sc::kTraits.tag));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Shuffle));
	// ON, so the amount is live and laid out (at Straight it hides).
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Swing8));
	ui::GenericExpanded panel(f.deviceHost, reg, id, sc::kDescriptor, nullptr);
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	panel.resized();
	// THE MODE IS THE GROOVE CELL'S SELECTOR (backlog IO addendum): its menu pick is `selectZone`.
	ui::ModeValueCell* grooveCell = sklitterCellFor<ui::ModeValueCell>(panel, sc::Param::GrooveMode);
	REQUIRE(grooveCell != nullptr);
	ui::ZoneSelect* modeBox = &grooveCell->select();
	ui::ValueBox* amountBox = sklitterCellFor<ui::ValueBox>(panel, sc::Param::GrooveAmount);
	REQUIRE(amountBox != nullptr);
	ui::ValueBox* phaseBox = sklitterCellFor<ui::ValueBox>(panel, sc::Param::FreePhase);
	REQUIRE(phaseBox != nullptr);
	// THE BOX UNDER THE HAND — the amount's, until a subcase in Free points it at the phase's.
	ui::ValueBox* current = amountBox;
	ui::ValueBox& box = *amountBox;
	REQUIRE(box.width() > 20);

	// Sets the mode and the number that mode reads: the depth in a Swing or Groove, the phase in a Free.
	const auto set = [&](sc::GrooveMode mode, double amount) {
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(mode));
		const bool free = sc::isFree(mode);
		f.bridge.setDeviceControl(id, static_cast<size_t>(free ? sc::Param::FreePhase : sc::Param::GrooveAmount),
		                          static_cast<float>(amount));
		current = free ? phaseBox : amountBox;
	};
	const auto feel = [&] {
		sc::Settings s;
		s.mode = sc::Mode::Shuffle;
		s.grooveMode = static_cast<float>(modeBox->control().value());
		s.grooveAmount = static_cast<float>(amountBox->control().value());
		s.freePhase = static_cast<float>(phaseBox->control().value());
		return s.grooveFeel(); // THE ENGINE'S OWN DECODE of the stored numbers
	};
	// A drag of `dy` pixels upward, pressed at x, on the box under the hand.
	const auto drag = [&](float x, float dy) {
		const float y = static_cast<float>(current->height()) * 0.5f;
		current->onMouseDown().callback(at(x, y));
		current->onMouseDrag().callback(at(x, y - dy));
		current->onMouseUp().callback(at(x, y - dy));
	};
	// THE COLUMN WORTH `place` IN THE VALUE'S UNITS, asked of the box's own hit test. To 1%, because the
	// slope is MEASURED from the printed ends of the range, so it carries the readout's own rounding (none
	// at 50.0% and 75.0%, but a range whose end prints rounded would); the places it separates are ten times apart.
	const auto xOfPlace = [&](double place) {
		for (float x = 0.0f; x < static_cast<float>(current->width()); x += 0.25f) {
			// RELATIVE, BY HAND: `doctest::Approx` adds a scale of one, which makes every place this small
			// "equal" to zero — the first draft of this case found its tenths at an x holding no digit at all.
			if (std::fabs(current->placeAt(x) - place) <= 1e-2 * place) {
				return x;
			}
		}
		return -1.0f;
	};
	// A PRESS ON NO DIGIT: the box's left edge, inside its padding. The fallback there is the whole range
	// per full drag — which is one mode's amount now, where it used to be seven zones.
	const float offText = 1.0f;

	SUBCASE("a drag past the top or bottom stays in the mode and clamps the amount there") {
		set(sc::GrooveMode::Swing8, 0.6);
		REQUIRE(box.placeAt(offText) == 0.0);
		// 28 px of 140 is a fifth of the range: 0.6 → 0.8, and 50 + 25·0.8 = 70.0%.
		drag(offText, 28.0f);
		CHECK(feel().mode == sc::GrooveMode::Swing8);
		CHECK(feel().amount == doctest::Approx(0.8).epsilon(1e-4));
		CHECK(box.readout() == "70.0%");
		// FAR PAST THE TOP: still Swing 8, at the top of it — 75.0% — and NOT Swing 16 at nothing.
		drag(offText, 2000.0f);
		CHECK(feel().mode == sc::GrooveMode::Swing8);
		CHECK(feel().amount == 1.0);
		CHECK(box.readout() == "75.0%");
		CHECK(modeBox->captionText() == "Swing 8");
		// FAR PAST THE BOTTOM: Swing 8 at nothing, not Swing 4 at full.
		drag(offText, -4000.0f);
		CHECK(feel().mode == sc::GrooveMode::Swing8);
		CHECK(feel().amount == 0.0);
		CHECK(box.readout() == "50.0%");
	}

	SUBCASE("a mode change leaves the amount alone, and the amount's print follows the mode") {
		set(sc::GrooveMode::Swing8, 0.48);
		REQUIRE(box.readout() == "62.0%");
		const double stored = box.control().value();
		for (const sc::GrooveMode m : { sc::GrooveMode::Swing16, sc::GrooveMode::Groove4, sc::GrooveMode::Free16,
		                                sc::GrooveMode::Straight, sc::GrooveMode::Swing4 }) {
			CAPTURE(static_cast<int>(m));
			modeBox->selectZone(static_cast<size_t>(m)); // what the menu's pick commits
			CHECK(feel().mode == m);
			CHECK(box.control().value() == stored);
		}
		CHECK(box.readout() == "62.0%"); // back in a Swing, the same amount reads the same swing
		modeBox->selectZone(static_cast<size_t>(sc::GrooveMode::Groove4));
		CHECK(box.readout() == "48.0%");
		// AND FREE LEAVES IT ALONE TOO — it reads its own phase, and the amount's stored value waits for a Swing.
		const double phase = phaseBox->control().value();
		modeBox->selectZone(static_cast<size_t>(sc::GrooveMode::Free4));
		CHECK(feel().amount == phase);
		CHECK(box.control().value() == stored);
	}

	SUBCASE("the amount shows in Swing and Groove, the phase in Free, neither in Straight, and both hide where the slice mode takes no groove") {
		set(sc::GrooveMode::Straight, 0.5);
		CHECK(panel.relevant(static_cast<size_t>(sc::Param::GrooveMode)));
		CHECK_FALSE(panel.relevant(static_cast<size_t>(sc::Param::GrooveAmount)));
		CHECK_FALSE(panel.relevant(static_cast<size_t>(sc::Param::FreePhase)));
		set(sc::GrooveMode::Groove16, 0.5);
		CHECK(panel.relevant(static_cast<size_t>(sc::Param::GrooveAmount)));
		CHECK_FALSE(panel.relevant(static_cast<size_t>(sc::Param::FreePhase)));
		set(sc::GrooveMode::Free8, 0.5);
		CHECK_FALSE(panel.relevant(static_cast<size_t>(sc::Param::GrooveAmount)));
		CHECK(panel.relevant(static_cast<size_t>(sc::Param::FreePhase)));
		// BURST: the mode is hidden by its own gate, and the phase by the CHAIN — its gate is a hidden control.
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Burst));
		CHECK_FALSE(panel.relevant(static_cast<size_t>(sc::Param::GrooveMode)));
		CHECK_FALSE(panel.relevant(static_cast<size_t>(sc::Param::GrooveAmount)));
		CHECK_FALSE(panel.relevant(static_cast<size_t>(sc::Param::FreePhase)));
	}

	SUBCASE("fine drag: a grabbed digit moves that place of the percent, through the readout's own slope") {
		// SWING: the MPC's 50 → 75% across 0..1, so one printed percent is 1/25 of the value.
		set(sc::GrooveMode::Swing16, 0.48); // 62.0%
		REQUIRE(box.readout() == "62.0%");
		const double percent = 1.0 / 25.0;
		const float tenths = xOfPlace(0.1 * percent);
		const float units = xOfPlace(1.0 * percent);
		const float tens = xOfPlace(10.0 * percent);
		REQUIRE(tenths >= 0.0f);
		REQUIRE(units >= 0.0f);
		REQUIRE(tens >= 0.0f);
		CHECK(tens < units);
		CHECK(units < tenths);
		// A full drag rolls the grabbed digit ten of its own places: half a drag on the tenths is +0.5%.
		drag(tenths, 70.0f);
		CHECK(box.readout() == "62.5%");
		CHECK(feel().amount == doctest::Approx(0.5).epsilon(1e-4));
		CHECK(feel().mode == sc::GrooveMode::Swing16);
		// A tenth of a drag on the units is +1%.
		drag(xOfPlace(1.0 * percent), 14.0f);
		CHECK(box.readout() == "63.5%");
		// GROOVE: 0 → 100% across 0..1, so one printed percent is 1/100 of the value.
		set(sc::GrooveMode::Groove8, 0.4);
		REQUIRE(box.readout() == "40.0%");
		const float grooveTenths = xOfPlace(0.1 * (1.0 / 100.0));
		REQUIRE(grooveTenths >= 0.0f);
		drag(grooveTenths, -140.0f);
		CHECK(box.readout() == "39.0%");
		CHECK(feel().amount == doctest::Approx(0.39).epsilon(1e-4));
		// FREE PHASE: the readout IS the phase, so its digits are the value's own — the thousandths move 0.01
		// per full drag.
		set(sc::GrooveMode::Free16, 0.375);
		REQUIRE(current == phaseBox);
		// The phase box is hidden until the panel lays out again; give it the amount box's place to be hit in.
		panel.resized();
		REQUIRE(phaseBox->width() > 20);
		REQUIRE(phaseBox->readout() == "0.375");
		const float thousandths = xOfPlace(0.001);
		REQUIRE(thousandths >= 0.0f);
		drag(thousandths, 70.0f);
		CHECK(phaseBox->readout() == "0.380");
		CHECK(feel().amount == doctest::Approx(0.38).epsilon(1e-4));
		// AND PAST 1 IT KEEPS GOING — a full drag on the units column is ten units: 0.380 → 10.380, no wrap, no
		// ceiling near (by design it does not wrap, and goes arbitrarily high).
		drag(xOfPlace(1.0), 140.0f);
		CHECK(phaseBox->readout() == "10.380");
		CHECK(feel().amount == doctest::Approx(10.38).epsilon(1e-5));
		// A BOX NOT DECLARED `digitBox` KEEPS REFUSING A TRANSLATED READOUT — the exception is opt-in.
		CHECK_FALSE(sc::kDescriptor.params[static_cast<size_t>(sc::Param::Mix)].digitBox);
	}
}
TEST_CASE("backlog IG: the band draws the SWUNG grid — each chunk as wide as the slot that plays it") {
	namespace sc = feathers::fx::sklitter;
	// STRAIGHT IS THE EVEN GRID, EXACTLY.
	const std::vector<float> even = ui::sklitterStepEdges(sc::GrooveFeel {}, 16, 1);
	REQUIRE(even.size() == 17u);
	for (size_t i = 0; i <= 16; ++i) {
		CHECK(even[i] == doctest::Approx(static_cast<float>(i) / 16.0f));
	}
	// SWING 16 AT THE TRIPLET: every odd edge a third of a step late, restated.
	const sc::GrooveFeel triplet { sc::GrooveMode::Swing16, 2.0 / 3.0 }; // two thirds of the MPC's travel
	const std::vector<float> swung = ui::sklitterStepEdges(triplet, 16, 1);
	for (size_t i = 0; i < 16; ++i) {
		CAPTURE(i);
		const double want = (static_cast<double>(i) + (i % 2 == 1 ? 1.0 / 3.0 : 0.0)) / 16.0;
		CHECK(swung[i] == doctest::Approx(want).epsilon(1e-5));
	}
	CHECK(swung[16] == 1.0f);
	// TWO BARS: the second bar is the first again, in the band's second half.
	const std::vector<float> twoBars = ui::sklitterStepEdges(triplet, 32, 2);
	for (size_t i = 0; i < 16; ++i) {
		CAPTURE(i);
		CHECK(twoBars[i] == doctest::Approx(swung[i] * 0.5f));
		CHECK(twoBars[16 + i] == doctest::Approx(0.5f + swung[i] * 0.5f));
	}
	// AND THE DEVICE PUBLISHES THE GROOVE WHERE IT ACTS, AND NOTHING WHERE IT DOES NOT.
	for (const sc::Mode mode : { sc::Mode::Shuffle, sc::Mode::Grain, sc::Mode::Burst }) {
		CAPTURE(static_cast<int>(mode));
		sc::Sklitter s;
		s.prepare(48000.0, 4096);
		sc::Settings cfg;
		cfg.mode = mode;
		cfg.grooveMode = static_cast<float>(sc::GrooveMode::Swing16);
		cfg.grooveAmount = 0.5f;
		s.setTarget(cfg);
		std::vector<float> published;
		s.setReadingsSink(feathers::device::ReadingsSink {
		    &published, [](void* ctx, const float* v, size_t n) {
			    static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
		    } });
		s.publishReadings();
		REQUIRE(published.size() > static_cast<size_t>(sc::Reading::GrooveAmount));
		const bool acts = mode == sc::Mode::Shuffle;
		CHECK(published[static_cast<size_t>(sc::Reading::Groove)]
		      == (acts ? static_cast<float>(sc::GrooveMode::Swing16) : 0.0f));
		CHECK(published[static_cast<size_t>(sc::Reading::GrooveAmount)] == (acts ? 0.5f : 0.0f));
	}
	// FREE IS DRAWN BY THE SAME WARP: a Free 8 band's edges are the device's own step offsets (the walk's rule
	// is restated, by value, in `test_fx_sklitter.cpp`; the band's claim is only that it draws what plays).
	const sc::GrooveFeel free8 { sc::GrooveMode::Free8, 3.3 };
	const std::vector<float> walked = ui::sklitterStepEdges(free8, 8, 1);
	bool moved = false;
	for (int j = 1; j < 8; ++j) {
		CAPTURE(j);
		const double late = sc::grooveStepOffset(free8, j);
		moved = moved || late > 0.0;
		CHECK(walked[static_cast<size_t>(j)] == doctest::Approx((j + late) / 8.0).epsilon(1e-5));
	}
	CHECK(moved);
}
TEST_CASE("backlog IO addendum: the band draws Free Phase where it IS — past 1, below 0, at 137.25 — and "
          "repaints for every move") {
	// The ribbon did not always update when the groove controls changed, and the unbounded groove value
	// was bounded. ONE CAUSE FOR BOTH: the card decoded the published amount through
	// a 0..1 clamp that was Groove Amount's rule, so Free Phase drew as 1 from 1 upward and as 0 below 0 —
	// the band froze, and to the eye the phase had stopped. Asserted as the FEEL the card decodes and the
	// repaint the lane's per-frame road reports, never as a rendered difference.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);

	std::vector<float> values(sc::kReadingCount, 0.0f);
	values[static_cast<size_t>(sc::Reading::Steps)] = 8.0f;
	values[static_cast<size_t>(sc::Reading::SeqBars)] = 1.0f;
	const auto publish = [&](sc::GrooveMode mode, float amount) {
		values[static_cast<size_t>(sc::Reading::Groove)] = static_cast<float>(mode);
		values[static_cast<size_t>(sc::Reading::GrooveAmount)] = amount;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
	};
	publish(sc::GrooveMode::Free8, 0.4f);
	lane->updateCards();

	// Each is a MOVE from the one before, so each must repaint and each must decode to itself.
	for (const float phase : { 2.7f, 3.9f, -3.2f, -0.5f, 137.25f, 137.5f, 0.4f }) {
		CAPTURE(phase);
		publish(sc::GrooveMode::Free8, phase);
		const sc::GrooveFeel drawn = ui::sklitterGroove(f.deviceHost, id);
		CHECK(drawn.mode == sc::GrooveMode::Free8);
		CHECK(drawn.amount == static_cast<double>(phase));
		CHECK(lane->updateCards() > 0);
		// THE EDGES THE CARD DRAWS ARE THE ONES THAT PHASE PLAYS — restated through the device's own offsets.
		const std::vector<float> edges = ui::sklitterStepEdges(drawn, 8, 1);
		const sc::GrooveFeel plays { sc::GrooveMode::Free8, static_cast<double>(phase) };
		for (int j = 1; j < 8; ++j) {
			CAPTURE(j);
			CHECK(edges[static_cast<size_t>(j)]
			      == doctest::Approx((j + sc::grooveStepOffset(plays, j)) / 8.0).epsilon(1e-5));
		}
	}
	// NOT VACUOUS: two phases past 1 draw different grids, so a band clamped at 1 is red above.
	const auto edgesAt = [](double phase) {
		return ui::sklitterStepEdges(sc::GrooveFeel { sc::GrooveMode::Free8, phase }, 8, 1);
	};
	CHECK(edgesAt(2.7) != edgesAt(3.9));
	CHECK(edgesAt(-3.2) != edgesAt(-0.5));

	// **PHASE 0 DRAWS THE STRAIGHT GRID (backlog IO addendum).** Two roads to
	// it and both are held: the device publishes Straight for Free at 0 (asserted in `test_fx_sklitter.cpp`),
	// and a Free 8 at 0 reaching the card anyway draws exactly the straight edges, `j / 8`, from either side.
	for (const float zero : { 0.0f, -0.0f }) {
		publish(sc::GrooveMode::Free8, zero);
		CHECK(lane->updateCards() > 0);
		const std::vector<float> edges = ui::sklitterStepEdges(ui::sklitterGroove(f.deviceHost, id), 8, 1);
		CHECK(edges == ui::sklitterStepEdges(sc::GrooveFeel {}, 8, 1));
		for (int j = 0; j <= 8; ++j) {
			CAPTURE(j);
			CHECK(edges[static_cast<size_t>(j)] == static_cast<float>(j) / 8.0f);
		}
		publish(sc::GrooveMode::Free8, 2.7f); // away again, so the next zero is a move the lane must repaint
		CHECK(lane->updateCards() > 0);
	}
	CHECK(edgesAt(0.0) != edgesAt(0.4));

	// SWING AND GROOVE KEEP THEIR DEPTH'S 0..1, which the device clamps before it publishes; the card's
	// decode of a depth is the same numbers.
	publish(sc::GrooveMode::Swing16, 0.3f);
	CHECK(ui::sklitterGroove(f.deviceHost, id).amount == doctest::Approx(0.3));
	CHECK(lane->updateCards() > 0);
	publish(sc::GrooveMode::Groove8, 0.7f);
	CHECK(ui::sklitterGroove(f.deviceHost, id).amount == doctest::Approx(0.7));
	CHECK(lane->updateCards() > 0);
	// A STILL PICTURE ASKS FOR NOTHING.
	CHECK(lane->updateCards() == 0);
}
TEST_CASE("backlog IO addendum: Free Phase, dragged as a hand drags it, goes past 1, 10, 100 and 1000 — "
          "stored, printed and decoded") {
	// THE REPORT'S OTHER READING: the unbounded groove value was bounded. The case above drags to 10.38;
	// this drives the digit a hand grabs to 137.25 and on past a thousand, and below zero. Groove is not on
	// the card (`kFace`), so the panel's side column is where it is played.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	ui::WidgetRegistry reg;
	const DeviceInstanceId id = f.bridge.fxDeviceAt(f.bridge.addFxDevice(sc::kTraits.tag));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Shuffle));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Free8));
	ui::GenericExpanded panel(f.deviceHost, reg, id, sc::kDescriptor, nullptr);
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	panel.resized();
	ui::ValueBox* box = sklitterCellFor<ui::ValueBox>(panel, sc::Param::FreePhase);
	REQUIRE(box != nullptr);
	CHECK(box->isVisible());
	CHECK(dynamic_cast<ui::GammaBox*>(box) == nullptr); // the digit road, not sixteen laps
	REQUIRE(box->width() > 20);

	const auto stored = [&] { return box->control().value(); };
	const auto decoded = [&] {
		sc::Settings s;
		s.mode = sc::Mode::Shuffle;
		s.grooveMode = static_cast<float>(sc::GrooveMode::Free8);
		s.freePhase = static_cast<float>(stored());
		return s.grooveFeel().amount; // THE ENGINE'S OWN DECODE
	};
	const auto xOfPlace = [&](double place) {
		for (float x = 0.0f; x < static_cast<float>(box->width()); x += 0.25f) {
			if (std::fabs(box->placeAt(x) - place) <= 1e-2 * place) {
				return x;
			}
		}
		return -1.0f;
	};
	const auto drag = [&](float x, float dy) {
		const float y = static_cast<float>(box->height()) * 0.5f;
		box->onMouseDown().callback(at(x, y));
		box->onMouseDrag().callback(at(x, y - dy));
		box->onMouseUp().callback(at(x, y - dy));
	};
	box->control().setValue(0.25);
	REQUIRE(box->readout() == "0.250");
	// PAST 1: a tenth of a full drag on the units is one unit.
	drag(xOfPlace(1.0), 14.0f);
	CHECK(stored() == doctest::Approx(1.25));
	CHECK(box->readout() == "1.250");
	// PAST 10: a full drag on the units is ten.
	drag(xOfPlace(1.0), 140.0f);
	CHECK(stored() == doctest::Approx(11.25));
	CHECK(box->readout() == "11.250");
	// PAST 100: the tens, now printed, a full drag and a quarter.
	REQUIRE(xOfPlace(10.0) >= 0.0f);
	drag(xOfPlace(10.0), 175.0f);
	CHECK(stored() == doctest::Approx(136.25));
	CHECK(box->readout() == "136.250");
	drag(xOfPlace(1.0), 14.0f);
	CHECK(stored() == doctest::Approx(137.25));
	CHECK(box->readout() == "137.250");
	CHECK(decoded() == doctest::Approx(137.25));
	// AND ON, through the hundreds, to 1137.25 — no ceiling near.
	REQUIRE(xOfPlace(100.0) >= 0.0f);
	drag(xOfPlace(100.0), 140.0f);
	CHECK(stored() == doctest::Approx(1137.25));
	CHECK(box->readout() == "1137.250");
	CHECK(decoded() == doctest::Approx(1137.25));
	// AND BELOW ZERO the same way.
	box->control().setValue(0.25);
	drag(xOfPlace(1.0), -140.0f);
	CHECK(stored() == doctest::Approx(-9.75));
	CHECK(box->readout() == "-9.750");
	CHECK(decoded() == doctest::Approx(-9.75));
}

namespace {
/// **WHETHER A CONTROL IS LIVE, RESTATED FROM THE DESCRIPTOR** — its gate chain walked against the stored
/// fields, independently of both surfaces' own `relevant`.
bool liveByDescriptor(const backplane::DeviceHost& host, DeviceInstanceId id, const device::Descriptor& d,
                      size_t index) {
	size_t at = index;
	for (size_t hops = 0; hops <= d.params.size(); ++hops) {
		const device::ParamDesc& p = d.params[at];
		if (p.gateControl < 0 || static_cast<size_t>(p.gateControl) >= d.params.size()) {
			return true;
		}
		if (!device::relevantFor(p, host.fieldAt(id, static_cast<size_t>(p.gateControl)))) {
			return false;
		}
		at = static_cast<size_t>(p.gateControl);
	}
	return true;
}
} // namespace

TEST_CASE("backlog IO addendum: Swing -> Free swaps Groove Amount for Free Phase on the OPEN panel, though the "
          "live count does not move") {
	// The switch from Groove Amount to Free sometimes did not happen: in Free mode the groove value was
	// still shown. THE MECHANISM: the panel re-laid its cells out only when the FIRST gate it
	// found (Mode) or the live-cell COUNT moved. Swing -> Free changes neither — it hides one cell and shows
	// another — so the Amount stayed on screen, under the hand, clamped at 1 ("bounded"). Straight -> Free
	// changes the count, which is why it was "sometimes". The hand's road: the panel open, the mode written
	// through the bridge, then the per-tick hook.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	ui::WidgetRegistry reg;
	const DeviceInstanceId id = f.bridge.fxDeviceAt(f.bridge.addFxDevice(sc::kTraits.tag));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Shuffle));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Swing16));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveAmount), 0.8f);
	ui::GenericExpanded panel(f.deviceHost, reg, id, sc::kDescriptor, nullptr);
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	panel.resized();
	(void)panel.update(); // settle, as the first tick does
	ui::ValueBox* amount = sklitterCellFor<ui::ValueBox>(panel, sc::Param::GrooveAmount);
	ui::ValueBox* phase = sklitterCellFor<ui::ValueBox>(panel, sc::Param::FreePhase);
	REQUIRE(amount != nullptr);
	REQUIRE(phase != nullptr);
	REQUIRE(amount->isVisible());
	REQUIRE_FALSE(phase->isVisible());
	const size_t countInSwing = panel.liveCells();
	// THE BOXES LIVE INSIDE THE GROOVE CELL (`modeCell`, IO addendum): the place a hand presses is the cell's
	// origin plus the box's, in the panel's coordinates.
	ui::ModeValueCell* groove = sklitterCellFor<ui::ModeValueCell>(panel, sc::Param::GrooveMode);
	REQUIRE(groove != nullptr);
	const visage::Point origin { static_cast<float>(groove->x()), static_cast<float>(groove->y()) };
	const visage::Point slot { origin.x + static_cast<float>(amount->x()) + 0.5f * static_cast<float>(amount->width()),
		                       origin.y + static_cast<float>(amount->y()) + 0.5f * static_cast<float>(amount->height()) };

	// A drag at the cell's place, through the panel's own hit test — so it lands on whatever is shown there.
	const auto dragAtSlot = [&](float dy) {
		visage::Frame* hit = panel.frameAtPoint(slot);
		auto* box = dynamic_cast<ui::ValueBox*>(hit);
		REQUIRE(box != nullptr);
		const visage::Point local { slot.x - origin.x - static_cast<float>(box->x()),
			                        slot.y - origin.y - static_cast<float>(box->y()) };
		box->onMouseDown().callback(at(1.0f, local.y)); // on no digit: the finest digit's road
		box->onMouseDrag().callback(at(1.0f, local.y - dy));
		box->onMouseUp().callback(at(1.0f, local.y - dy));
		return box;
	};

	for (const sc::GrooveMode free : { sc::GrooveMode::Free16, sc::GrooveMode::Free4 }) {
		CAPTURE(static_cast<int>(free));
		// SWING -> FREE, THE SAME COUNT.
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(free));
		CHECK(panel.liveCells() == countInSwing); // the condition the old trigger could not see
		CHECK(panel.update());
		CHECK_FALSE(amount->isVisible());
		CHECK(phase->isVisible());
		CHECK(groove->activeParam() == static_cast<long>(sc::Param::FreePhase));
		CHECK(phase->x() == amount->x()); // the same place in the cell
		// AND A HAND ON THAT PLACE MOVES THE PHASE, NOT THE AMOUNT.
		const float amountBefore = f.deviceHost.fieldAt(id, static_cast<size_t>(sc::Param::GrooveAmount));
		const float phaseBefore = f.deviceHost.fieldAt(id, static_cast<size_t>(sc::Param::FreePhase));
		CHECK(dragAtSlot(70.0f) == phase);
		CHECK(f.deviceHost.fieldAt(id, static_cast<size_t>(sc::Param::GrooveAmount)) == amountBefore);
		CHECK(f.deviceHost.fieldAt(id, static_cast<size_t>(sc::Param::FreePhase)) != phaseBefore);

		// FREE -> SWING, THE SAME COUNT, BACK.
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(sc::GrooveMode::Groove8));
		CHECK(panel.update());
		CHECK(amount->isVisible());
		CHECK_FALSE(phase->isVisible());
		const float phaseHeld = f.deviceHost.fieldAt(id, static_cast<size_t>(sc::Param::FreePhase));
		CHECK(dragAtSlot(-14.0f) == amount);
		CHECK(f.deviceHost.fieldAt(id, static_cast<size_t>(sc::Param::FreePhase)) == phaseHeld);
	}
	// A STILL PANEL ASKS FOR NOTHING.
	CHECK_FALSE(panel.update());
}
TEST_CASE("backlog IO addendum: Groove is ONE zone knob — its box drives Amount or Free Phase by mode, its "
          "selector drives Groove Mode and stops at both ends") {
	// Groove wears the normal zone knob look again. Presentation only: the three
	// params stay three. Asserted as the param each widget writes and the value it stores.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	ui::WidgetRegistry reg;
	const DeviceInstanceId id = f.bridge.fxDeviceAt(f.bridge.addFxDevice(sc::kTraits.tag));
	f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Shuffle));
	ui::GenericExpanded panel(f.deviceHost, reg, id, sc::kDescriptor, nullptr);
	const visage::Point want = panel.minimumSize();
	panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
	panel.resized();
	(void)panel.update();
	ui::ModeValueCell* groove = sklitterCellFor<ui::ModeValueCell>(panel, sc::Param::GrooveMode);
	REQUIRE(groove != nullptr);
	REQUIRE(panel.modeCellsForTest().size() == 1u);
	ui::ValueBox* amount = groove->boxFor(static_cast<size_t>(sc::Param::GrooveAmount));
	ui::ValueBox* phase = groove->boxFor(static_cast<size_t>(sc::Param::FreePhase));
	REQUIRE(amount != nullptr);
	REQUIRE(phase != nullptr);
	// EACH BOX IS BOUND TO ITS OWN CONTROL, for good — its digit drag and its modulation display are that one's.
	CHECK(std::string(amount->control().desc().name) == "Groove Amount");
	CHECK(std::string(phase->control().desc().name) == "Free Phase");
	const auto field = [&](sc::Param p) { return f.deviceHost.fieldAt(id, static_cast<size_t>(p)); };

	SUBCASE("the box follows the mode, through the hand's road") {
		struct Case {
			sc::GrooveMode mode;
			long active;
		};
		for (const Case c : { Case { sc::GrooveMode::Straight, -1 },
		                      Case { sc::GrooveMode::Swing16, static_cast<long>(sc::Param::GrooveAmount) },
		                      Case { sc::GrooveMode::Free8, static_cast<long>(sc::Param::FreePhase) },
		                      Case { sc::GrooveMode::Groove4, static_cast<long>(sc::Param::GrooveAmount) },
		                      Case { sc::GrooveMode::Straight, -1 } }) {
			CAPTURE(static_cast<int>(c.mode));
			f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::GrooveMode), static_cast<float>(c.mode));
			(void)panel.update();
			CHECK(groove->isVisible()); // the cell stays: its selector is how Straight is left
			CHECK(groove->activeParam() == c.active);
			CHECK(amount->isVisible() == (c.active == static_cast<long>(sc::Param::GrooveAmount)));
			CHECK(phase->isVisible() == (c.active == static_cast<long>(sc::Param::FreePhase)));
			CHECK(groove->select().captionText() == sc::kGrooveModeNames[static_cast<size_t>(c.mode)]);
		}
		// STRAIGHT PRINTS "straight" — the Amount's own print for that mode, drawn by the cell, not a box.
		CHECK(amount->readout() == "straight");
		// IN A MODE GROOVE DOES NOT ACT IN, THE CELL GOES WITH ITS GATE.
		f.bridge.setDeviceControl(id, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Grain));
		(void)panel.update();
		CHECK_FALSE(groove->isVisible());
	}

	SUBCASE("the selector's arrows step Groove Mode and stop at both ends; its menu pick lands on the step") {
		ui::ZoneSelect& select = groove->select();
		const visage::Point next { select.nextBounds().x() + 2.0f, select.nextBounds().y() + 2.0f };
		const visage::Point prev { select.prevBounds().x() + 2.0f, select.prevBounds().y() + 2.0f };
		REQUIRE(field(sc::Param::GrooveMode) == 0.0f);
		// PREV AT STRAIGHT: nothing — no lap to Free 16.
		CHECK(select.pressAt(prev));
		CHECK(field(sc::Param::GrooveMode) == 0.0f);
		// NEXT walks the list, one step a press, the caption naming each.
		for (int m = 1; m < sc::kGrooveModeCount; ++m) {
			CAPTURE(m);
			CHECK(select.pressAt(next));
			CHECK(field(sc::Param::GrooveMode) == static_cast<float>(m));
			CHECK(select.captionText() == sc::kGrooveModeNames[static_cast<size_t>(m)]);
		}
		// NEXT AT FREE 16: nothing — no lap to Straight. And the last step reads as ITSELF, not as Free 8.
		CHECK(select.pressAt(next));
		CHECK(field(sc::Param::GrooveMode) == static_cast<float>(sc::GrooveMode::Free16));
		CHECK(select.zoneIndex() == static_cast<size_t>(sc::GrooveMode::Free16));
		// THE MENU'S PICK is the step itself.
		select.selectZone(static_cast<size_t>(sc::GrooveMode::Swing8));
		CHECK(field(sc::Param::GrooveMode) == static_cast<float>(sc::GrooveMode::Swing8));
		// THE AMOUNT AND THE PHASE ARE UNTOUCHED BY ANY OF IT — the mode is a different number.
		CHECK(field(sc::Param::GrooveAmount) == 0.0f);
		CHECK(field(sc::Param::FreePhase) == 0.0f);
	}
}
TEST_CASE("backlog IO addendum: every gate move in the registry leaves every cell shown iff it is live — on the "
          "panel and on the card, whatever the count does") {
	// THE SWEEP BEHIND THE CASE ABOVE: every device, every control some other control is gated on, every step
	// of it written through the bridge with the surfaces open, then one tick. After it each cell must be
	// visible exactly when the descriptor's gate chain says it is live. Counts the moves that swapped the
	// live set at an unchanged count, which is the shape the old trigger missed, so the sweep cannot pass
	// vacuously.
	size_t swaps = 0;
	size_t devices = 0;
	for (const device::Descriptor& d : feathers::kDevices) {
		std::set<size_t> gates;
		for (const device::ParamDesc& p : d.params) {
			if (p.gateControl >= 0 && static_cast<size_t>(p.gateControl) < d.params.size()) {
				gates.insert(static_cast<size_t>(p.gateControl));
			}
		}
		if (gates.empty()) {
			continue;
		}
		ModFixture f;
		const size_t slot = f.bridge.addFxDevice(d.traits.tag);
		if (slot >= feathers::kMaxFxSlots) {
			continue;
		}
		const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
		ui::WidgetRegistry reg;
		ui::GenericExpanded panel(f.deviceHost, reg, id, d, nullptr);
		ui::GenericCard card(f.deviceHost, id, d);
		const auto fit = [&] {
			const visage::Point want = panel.minimumSize();
			panel.setBounds(0, 0, static_cast<int>(want.x), static_cast<int>(want.y));
			panel.resized();
			card.setBounds(0, 0, 220, 2000);
		};
		fit();
		(void)panel.update();
		(void)card.update();
		++devices;
		const std::string deviceName = d.traits.name;
		CAPTURE(deviceName);
		std::vector<bool> before;
		const auto liveSet = [&] {
			std::vector<bool> out;
			for (size_t i = 0; i < d.params.size(); ++i) {
				out.push_back(liveByDescriptor(f.deviceHost, id, d, i));
			}
			return out;
		};
		for (const size_t g : gates) {
			const device::ParamDesc& gp = d.params[g];
			std::vector<double> values;
			if (gp.stepped && gp.max - gp.min <= 32.0) {
				for (double v = gp.min; v <= gp.max; v += 1.0) {
					values.push_back(v);
				}
			}
			else {
				values = { gp.min, gp.def, gp.max };
			}
			for (const double v : values) {
				CAPTURE(g);
				CAPTURE(v);
				before = liveSet();
				const size_t countBefore = static_cast<size_t>(std::count(before.begin(), before.end(), true));
				f.bridge.setDeviceControl(id, g, static_cast<float>(v));
				(void)panel.update();
				(void)card.update();
				const std::vector<bool> now = liveSet();
				if (now != before && static_cast<size_t>(std::count(now.begin(), now.end(), true)) == countBefore) {
					++swaps;
				}
				const auto& pi = panel.paramIndicesForTest();
				for (size_t n = 0; n < pi.size(); ++n) {
					CAPTURE(pi[n]);
					CHECK(panel.cellsForTest()[n]->isVisible() == now[pi[n]]);
				}
				const auto& ci = card.paramIndicesForTest();
				for (size_t n = 0; n < ci.size(); ++n) {
					CAPTURE(ci[n]);
					CHECK(card.rowsForTest()[n]->isVisible() == now[ci[n]]);
				}
			}
		}
	}
	CHECK(devices >= 2);
	CHECK(swaps >= 2); // SKLITTER's Swing <-> Free at least; the sweep met the shape it is for
}
TEST_CASE("backlog IG: at the yield floor, with every panel open at the default editor, every PERFORM "
          "tenant fits — heading, chevron, four Cfg cells, disk and puck, chips, sparks") {
	// design option (A): the circle yields to what the default editor leaves it with everything open,
	// rather than the panel clipping — and every other tenant of the Perform pane must still fit.
	namespace sc = feathers::fx::sklitter;
	static_assert(ui::CircleRail::kDesignEditorWidth == static_cast<float>(feathers::Gui::kLogicalWidth),
	              "the floor is derived from the editor's width; the two must be one number");
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	p.armFrom();
	p.f.bridge.setDeviceControl(p.device, static_cast<size_t>(sc::Param::Mode), static_cast<float>(sc::Mode::Shuffle));
	setRails(p, /*circleOpen=*/true, /*fxOpen=*/true);
	// THE PANEL AS THE DEFAULT EDITOR GIVES IT: 1240 less the overlay's two pads.
	const float given = static_cast<float>(feathers::Gui::kLogicalWidth) - 2.0f * ui::ExpandedOverlay::kPanelPad;
	const float tall = p.panel->minimumSize().y;
	p.panel->setBounds(0, 0, static_cast<int>(given), static_cast<int>(tall));
	p.panel->resized();
	ui::CircleRail& circle = p.panel->circleRailForTest();
	CHECK(static_cast<float>(circle.width()) == doctest::Approx(ui::CircleRail::kYieldFloorWidth));
	// NO CLIP: the rails as laid out plus the body end exactly at the panel's right edge.
	CHECK(p.panel->minimumSize().x - (ui::CircleRail::kWidth - ui::CircleRail::kYieldFloorWidth)
	      == doctest::Approx(given));
	CHECK(static_cast<float>(p.panel->postRailForTest().x() + p.panel->postRailForTest().width())
	      <= given + 0.5f);

	const float w = static_cast<float>(circle.width());
	const float h = static_cast<float>(circle.height());
	struct Tenant {
		std::string name;
		visage::Bounds r;
	};
	std::vector<Tenant> rects;
	rects.push_back({ "heading", circle.headingBounds() });
	rects.push_back({ "chevron", circle.toggleBounds() });
	for (size_t c = 0; c < sc::kCorners; ++c) {
		const ui::ZoneCoordCell* cell = circle.corners()[c];
		REQUIRE(cell != nullptr);
		CHECK(cell->isVisible());
		rects.push_back({ "cfg " + std::to_string(c + 1),
			              { static_cast<float>(cell->x()), static_cast<float>(cell->y()),
			                static_cast<float>(cell->width()), static_cast<float>(cell->height()) } });
	}
	const auto& arms = circle.outputArmsForTest();
	for (size_t n = 0; n < arms.size(); ++n) {
		REQUIRE(arms[n] != nullptr);
		CHECK(arms[n]->isVisible());
		CHECK(arms[n]->width() > 0);
		rects.push_back({ std::string("chip ") + sc::kOutputs[n].name,
			              { static_cast<float>(arms[n]->x()), static_cast<float>(arms[n]->y()),
			                static_cast<float>(arms[n]->width()), static_cast<float>(arms[n]->height()) } });
	}
	// EVERY RECTANGLE INSIDE THE RAIL, AND NO TWO OVERLAPPING. The heading and the chevron share the head
	// row by design and `headingBounds` starts past the chevron; they are checked like any other pair.
	for (size_t i = 0; i < rects.size(); ++i) {
		const visage::Bounds& a = rects[i].r;
		INFO("tenant := " << rects[i].name);
		CHECK(a.x() >= -0.5f);
		CHECK(a.y() >= -0.5f);
		CHECK(a.right() <= w + 0.5f);
		CHECK(a.bottom() <= h + 0.5f);
		for (size_t j = 0; j < i; ++j) {
			const visage::Bounds& b = rects[j].r;
			INFO("against := " << rects[j].name);
			const bool apart = a.right() <= b.x() + 0.5f || b.right() <= a.x() + 0.5f
			                   || a.bottom() <= b.y() + 0.5f || b.bottom() <= a.y() + 0.5f;
			CHECK(apart);
		}
	}
	// THE DISK IS A CIRCLE, SO IT IS CHECKED AS ONE: its grab reach (half the pad's frame) clears every
	// rectangle tenant by at least `kCornerClear` less a rounding pixel, and it sits inside the rail.
	ui::PerformPad& pad = circle.pad();
	const float cx = static_cast<float>(pad.x()) + 0.5f * static_cast<float>(pad.width());
	const float cy = static_cast<float>(pad.y()) + 0.5f * static_cast<float>(pad.height());
	const float reach = 0.5f * static_cast<float>(pad.width());
	CHECK(cx - reach >= -0.5f);
	CHECK(cx + reach <= w + 0.5f);
	for (const Tenant& t : rects) {
		INFO("tenant := " << t.name);
		const float nx = std::clamp(cx, t.r.x(), t.r.right());
		const float ny = std::clamp(cy, t.r.y(), t.r.bottom());
		CHECK(std::hypot(cx - nx, cy - ny) >= reach + ui::CircleRail::kCornerClear - 1.0f);
	}
	// THE PUCK, THE MARKS AND THE SPARKS ARE INSIDE THE DISK: the furniture never scales below 1:1, so at
	// a small disk they are their full size and the question is whether the disk still holds them.
	const float diskRadius = pad.disk().radius;
	CHECK(pad.scale() == doctest::Approx(1.0f));
	CHECK(pad.puckRadius() * 2.0f < diskRadius);
	CHECK(ui::PerformPad::kSparkSizeBirth * pad.scale() * 4.0f < diskRadius);
	CHECK(ui::PerformPad::kSparkReach <= 1.0f); // retired at the rim — the stars' aperture — so never on the rail
	MESSAGE("at the floor " << w << ": room across " << circle.layout().across << ", disk " << pad.width()
	                        << " (playable radius " << diskRadius << "), puck radius " << pad.puckRadius()
	                        << ", spark birth size " << ui::PerformPad::kSparkSizeBirth << ", chip rows "
	                        << ui::CircleRail::outputRowsFor(w) << ", cfg cell " << ui::CircleRail::kCellSide
	                        << ", cell-to-cell gap "
	                        << (static_cast<float>(circle.corners()[1]->x())
	                            - static_cast<float>(circle.corners()[0]->x() + circle.corners()[0]->width())));
}

namespace {
/// **EVERY PERFORM TENANT, CHECKED ON THE LAID-OUT WIDGETS (backlog IR)** — IG's tenant check, made a
/// function so it can run at every size rather than one. Inside the rail, no two rectangles overlapping,
/// the disk inside its room and clearing every rectangle by `kCornerClear`, and each Cfg cell wholly in
/// its own waypoint's quadrant and further out than the waypoint's mark. Returns the disk's side.
float checkPerformTenants(ui::CircleRail& circle) {
	namespace sc = feathers::fx::sklitter;
	namespace pp = feathers::ui::perform_pad;
	const float w = static_cast<float>(circle.width());
	const float h = static_cast<float>(circle.height());
	struct Tenant {
		std::string name;
		visage::Bounds r;
	};
	const auto boundsOf = [](const visage::Frame& f) {
		return visage::Bounds { static_cast<float>(f.x()), static_cast<float>(f.y()), static_cast<float>(f.width()),
			                    static_cast<float>(f.height()) };
	};
	std::vector<Tenant> rects;
	rects.push_back({ "heading", circle.headingBounds() });
	rects.push_back({ "chevron", circle.toggleBounds() });
	for (size_t c = 0; c < sc::kCorners; ++c) {
		const ui::ZoneCoordCell* cell = circle.corners()[c];
		REQUIRE(cell != nullptr);
		CHECK(cell->isVisible());
		CHECK(static_cast<float>(cell->width()) == doctest::Approx(ui::CircleRail::kCellSide));
		rects.push_back({ "cfg " + std::to_string(c + 1), boundsOf(*cell) });
	}
	for (size_t n = 0; n < circle.outputArmsForTest().size(); ++n) {
		const std::unique_ptr<ui::SourceArm>& arm = circle.outputArmsForTest()[n];
		if (arm != nullptr) {
			CHECK(arm->width() > 0);
			rects.push_back({ std::string("chip ") + sc::kOutputs[n].name, boundsOf(*arm) });
		}
	}
	for (size_t i = 0; i < rects.size(); ++i) {
		const visage::Bounds& a = rects[i].r;
		INFO("tenant := " << rects[i].name);
		CHECK(a.x() >= -0.5f);
		CHECK(a.y() >= -0.5f);
		CHECK(a.right() <= w + 0.5f);
		CHECK(a.bottom() <= h + 0.5f);
		for (size_t j = 0; j < i; ++j) {
			const visage::Bounds& b = rects[j].r;
			INFO("against := " << rects[j].name);
			CHECK((a.right() <= b.x() + 0.5f || b.right() <= a.x() + 0.5f || a.bottom() <= b.y() + 0.5f
			       || b.bottom() <= a.y() + 0.5f));
		}
	}
	// THE DISK: square, inside the room (inside the padding, under the heading row, above the band).
	ui::PerformPad& pad = circle.pad();
	CHECK(pad.width() == pad.height());
	const float side = static_cast<float>(pad.width());
	const float cx = static_cast<float>(pad.x()) + 0.5f * side;
	const float cy = static_cast<float>(pad.y()) + 0.5f * side;
	const float reach = 0.5f * side;
	CHECK(static_cast<float>(pad.x()) >= ui::CircleRail::kPadX - 0.5f);
	CHECK(static_cast<float>(pad.x()) + side <= w - ui::CircleRail::kPadX + 0.5f);
	CHECK(static_cast<float>(pad.y())
	      >= ui::Rail::kPadTop + ui::CircleRail::kHeadingHeight + ui::CircleRail::kGap - 0.5f);
	CHECK(static_cast<float>(pad.y()) + side
	      <= circle.outputStripBounds().y() - ui::CircleRail::kGap + 0.5f);
	for (const Tenant& t : rects) {
		INFO("tenant := " << t.name);
		const float nx = std::clamp(cx, t.r.x(), t.r.right());
		const float ny = std::clamp(cy, t.r.y(), t.r.bottom());
		CHECK(std::hypot(cx - nx, cy - ny) >= reach + ui::CircleRail::kCornerClear - 1.0f);
	}
	// **THE WAYPOINT PAIRING — the waypoint is ASKED (`waypointPoint`), not restated.** Each cell is
	// WHOLLY on its mark's side of both centre lines — its own quadrant — and further out than the mark.
	// And each cell is as close as the air allows: exactly `kCornerClear` from the grab reach, to a pixel.
	const pp::Disk d = pad.disk();
	for (size_t i = 0; i < sc::kCorners; ++i) {
		CAPTURE(i);
		const auto [mx, my] = pp::waypointPoint(d, static_cast<sc::Corner>(i));
		const float markX = static_cast<float>(pad.x()) + mx;
		const float markY = static_cast<float>(pad.y()) + my;
		const visage::Bounds c = rects[2 + i].r;
		if (markX < cx) {
			CHECK(c.right() <= cx + 0.5f);
		} else {
			CHECK(c.x() >= cx - 0.5f);
		}
		if (markY < cy) {
			CHECK(c.bottom() <= cy + 0.5f);
		} else {
			CHECK(c.y() >= cy - 0.5f);
		}
		const float nx = std::clamp(cx, c.x(), c.right());
		const float ny = std::clamp(cy, c.y(), c.bottom());
		CHECK(std::hypot(nx - cx, ny - cy) > std::hypot(markX - cx, markY - cy));
		CHECK(std::abs(std::hypot(nx - cx, ny - cy) - reach - ui::CircleRail::kCornerClear) <= 1.0f);
	}
	return side;
}
} // namespace

TEST_CASE("backlog IR: the perform disk is as LARGE as the rail's room allows, and the Cfg cells go where "
          "the free space is — at every size, in its own quadrant") {
	// In various modes the Perform window's corner boxes left space above or beside the circle that
	// would let the circle be larger at various scales, so the boxes lay out by where the free space is.
	//
	// **THE CLAIM IS A DIAMETER, SO IT IS ASSERTED AS ONE** against `expectedOptimumDisk`, an independent
	// restatement of the largest disk the room holds with a cell in each quadrant. The pre-IR rule
	// (`squareBlockDisk`) is printed beside it; it is the known-bad, and it fails the `>=` below
	// wherever the room is not square.
	namespace sc = feathers::fx::sklitter;
	PerformPanel p;
	REQUIRE(p.panel != nullptr);
	p.armFrom();
	ui::CircleRail& circle = p.panel->circleRailForTest();
	ui::PerformPad& pad = p.panel->padForTest();
	const auto report = [&](const char* name) {
		const float w = static_cast<float>(circle.width());
		const float h = static_cast<float>(circle.height());
		const float laid = checkPerformTenants(circle);
		const float optimum = expectedOptimumDisk(w, h);
		const ui::CircleRail::Layout at = circle.layout();
		MESSAGE(std::string(name) << ": rail " << w << "x" << h << ", room " << at.across << "x" << at.down
		             << " — disk before IR " << squareBlockDisk(w, h) << ", optimum " << optimum
		             << ", laid out " << laid << "; cells at ±" << at.dx << ", ±" << at.dy);
		CHECK(laid >= optimum - 1.0f);
		CHECK(laid <= optimum + 1.0f);
		return laid;
	};

	SUBCASE("the named sizes") {
		// THE DEFAULT PANEL — the fixture's own minimum, circle open, fx rails as they open by default.
		const float atDefault = report("default panel");
		// ALL PANELS OPEN AT THE DEFAULT EDITOR — IG's yield floor, where the disk was 67.
		setRails(p, /*circleOpen=*/true, /*fxOpen=*/true);
		const float given = static_cast<float>(feathers::Gui::kLogicalWidth) - 2.0f * ui::ExpandedOverlay::kPanelPad;
		const float tall = p.panel->minimumSize().y;
		p.panel->setBounds(0, 0, static_cast<int>(given), static_cast<int>(tall));
		p.panel->resized();
		REQUIRE(static_cast<float>(circle.width()) == doctest::Approx(ui::CircleRail::kYieldFloorWidth));
		const float atFloor = report("all open at 1240");
		// **THE GAIN IS REAL HERE — the case's not-vacuous line.** The old rule gave 67.2.
		CHECK(squareBlockDisk(static_cast<float>(circle.width()), static_cast<float>(circle.height()))
		      == doctest::Approx(67.196f).epsilon(0.001));
		CHECK(atFloor > 150.0f);
		// ALL OPEN IN A TALL EDITOR (1240×1000 less the overlay's pads).
		p.panel->setBounds(0, 0, static_cast<int>(given), static_cast<int>(1000.0f - 2.0f * ui::ExpandedOverlay::kPanelPad));
		p.panel->resized();
		report("all open, 1240x1000 editor");
		// THE DEFAULT PANEL DRAGGED TALL, THEN WIDE, THEN BOTH.
		setRails(p, /*circleOpen=*/true, /*fxOpen=*/false);
		const float askW = p.panel->minimumSize().x;
		const float askH = p.panel->minimumSize().y;
		p.panel->setBounds(0, 0, static_cast<int>(askW), static_cast<int>(askH + 400.0f));
		p.panel->resized();
		report("default panel +400 tall");
		p.panel->setBounds(0, 0, static_cast<int>(askW + 400.0f), static_cast<int>(askH));
		p.panel->resized();
		report("default panel +400 wide");
		p.panel->setBounds(0, 0, static_cast<int>(askW + 400.0f), static_cast<int>(askH + 400.0f));
		p.panel->resized();
		report("default panel +400 both");

		// **1.75× — A SCALE CHANGE MOVES NOTHING LOGICAL, SO THE CLAIM IS NATIVE.** The logical layout
		// is the default's; the pad and a cell must be 1.75× that natively, to a pixel.
		p.panel->setBounds(0, 0, static_cast<int>(askW), static_cast<int>(askH));
		p.panel->resized();
		const float logicalPad = report("default panel at 1:1");
		p.panel->setDpiScale(1.75f);
		p.panel->resized();
		CHECK(report("default panel at 1.75x") == doctest::Approx(logicalPad));
		CHECK(std::abs(static_cast<float>(pad.nativeWidth()) - 1.75f * logicalPad) <= 1.0f);
		const ui::ZoneCoordCell* cell = circle.corners()[sc::kCfg1];
		CHECK(std::abs(static_cast<float>(cell->nativeWidth()) - 1.75f * ui::CircleRail::kCellSide) <= 1.0f);
		CHECK(std::abs(static_cast<float>(cell->nativeX()) - 1.75f * static_cast<float>(cell->x())) <= 1.0f);
		(void)atDefault;
	}

	SUBCASE("every width from the yield floor, at every height — HX's sweep and IG's tenants, both axes") {
		// THE RAIL ALONE, laid out directly: this is about the rail's geometry over (width, height), not
		// about how a panel arrives at a size, which the subcase above and the grip case drive.
		size_t sizes = 0;
		size_t gained = 0;
		for (float w = ui::CircleRail::kYieldFloorWidth; w <= 1000.0f; w += 13.0f) {
			for (float h = ui::CircleRail::kOpenMinimumHeight; h <= 1300.0f; h += 31.0f) {
				CAPTURE(w);
				CAPTURE(h);
				circle.setBounds(0, 0, w, h);
				circle.resized();
				const float laid = checkPerformTenants(circle);
				const float optimum = expectedOptimumDisk(w, h);
				CHECK(laid >= optimum - 1.0f);
				CHECK(laid <= optimum + 1.0f);
				++sizes;
				gained += laid > squareBlockDisk(w, h) + 1.0f ? 1 : 0;
			}
		}
		MESSAGE(sizes << " rail sizes; the disk is larger than the pre-IR rule gave at " << gained);
		CHECK(gained > sizes / 2);
	}
}
TEST_CASE("IK: a fader pill's click is the POWER and its drag the AMOUNT — on the card and the panel, and neither touches the other") {
	// Amount and on/off must not be conflated under modulation — so a pill drives two controls. A click toggles the power and leaves the amount where it was; a
	// sideways drag (`FaderPill::kSpan` px for the whole range) sets the amount and leaves the power.
	// VALUES, read back from the device's row.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.card != nullptr);
	REQUIRE(f.panel != nullptr);
	const auto valueOf = [&](shp::Param p) {
		const FxChain chain = f.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, f.slot).row.field[static_cast<size_t>(p)];
	};
	const auto exercise = [&](ui::FaderPill& pill, shp::Param amount, shp::Param power) {
		REQUIRE(pill.width() > 0.0f);
		const float cx = pill.width() * 0.5f;
		const float cy = pill.height() * 0.5f;
		const auto click = [&] {
			pill.onMouseDown().callback(at(cx, cy));
			pill.onMouseUp().callback(at(cx, cy));
		};
		REQUIRE(valueOf(amount) == doctest::Approx(0.5f)); // the field's own, by default
		REQUIRE(valueOf(power) == 0.0f);
		click();
		CHECK(valueOf(power) == 1.0f);
		CHECK(valueOf(amount) == doctest::Approx(0.5f)); // the click did not move the amount
		CHECK(pill.poweredForTest());
		click();
		CHECK(valueOf(power) == 0.0f);

		// A DRAG A QUARTER OF THE SPAN RIGHT: 0.5 -> 0.75, live while it moves, and the power stays off.
		const float quarter = 0.25f * ui::FaderPill::kSpan;
		pill.onMouseDown().callback(at(cx, cy));
		pill.onMouseDrag().callback(at(cx + quarter, cy));
		CHECK(valueOf(amount) == doctest::Approx(0.75f));
		pill.onMouseUp().callback(at(cx + quarter, cy));
		CHECK(valueOf(amount) == doctest::Approx(0.75f));
		CHECK(valueOf(power) == 0.0f);

		// INSIDE THE SLOP A PRESS IS STILL A CLICK — the power, not a two-pixel amount.
		pill.onMouseDown().callback(at(cx, cy));
		pill.onMouseDrag().callback(at(cx + 2.0f, cy));
		pill.onMouseUp().callback(at(cx + 2.0f, cy));
		CHECK(valueOf(power) == 1.0f);
		CHECK(valueOf(amount) == doctest::Approx(0.75f));

		// AND THE ↔ CURSOR'S STATE follows the pointer onto and off the pill.
		pill.onMouseExit().callback(at(-5.0f, -5.0f));
		CHECK_FALSE(pill.hoveringForTest());
		pill.onMouseEnter().callback(at(cx, cy));
		CHECK(pill.hoveringForTest());
		pill.onMouseExit().callback(at(-5.0f, -5.0f));
		CHECK_FALSE(pill.hoveringForTest());
	};
	SUBCASE("on the panel") {
		REQUIRE(f.panel->pillsForTest().size() == shp::kExtraSwitchCount);
		exercise(*f.panel->pillsForTest()[1], shp::Param::Feedback, shp::Param::FeedbackOn);
	}
	SUBCASE("on the card") {
		REQUIRE(f.card->pillsForTest().size() == shp::kExtraSwitchCount);
		exercise(*f.card->pillsForTest()[2], shp::Param::Mangle, shp::Param::MangleOn);
	}
}

