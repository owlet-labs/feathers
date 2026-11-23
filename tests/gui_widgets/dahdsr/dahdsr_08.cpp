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

// PIECE 8 OF 12 of the dahdsr suite: its cases in their original order, with the helpers only they use. See dahdsr_common.h.

#include "dahdsr/dahdsr_common.h"

namespace {
/// THE PARAMETERS OF HARM THAT A FRAME TREE OFFERS A HAND RIGHT NOW — only widgets that are EFFECTIVELY visible (they and every ancestor
/// up to `root` shown), asked what they offer (`ui::ControlFace`), by descriptor identity. A control on a hidden tab is not on offer.
std::set<size_t> harmVisibleOffers(const visage::Frame* root) {
	std::set<size_t> out;
	std::vector<ui::ControlFace::Offered> list;
	std::vector<std::pair<const visage::Frame*, bool>> stack { { root, true } };
	const auto& desc = feathers::fx::harm::kDescriptor;
	while (!stack.empty()) {
		const auto [fr, shown] = stack.back();
		stack.pop_back();
		const bool here = shown && fr->isVisible();
		if (const auto* face = dynamic_cast<const ui::ControlFace*>(fr); face != nullptr && fr != root && here) {
			list.clear();
			face->offeredControls(list);
			for (const ui::ControlFace::Offered& o : list) {
				for (size_t i = 0; o.control != nullptr && i < desc.params.size(); ++i) {
					if (&o.control->desc() == &desc.params[i]) {
						out.insert(i);
					}
				}
			}
		}
		for (const visage::Frame* c : fr->children()) {
			stack.push_back({ c, here });
		}
	}
	return out;
}
} // namespace

TEST_CASE("HARM's window: a strip of four partials that is always on show, whose cells choose the detail") {
	HarmFixture f;
	namespace hrm = feathers::fx::harm;
	ui::HarmExpanded& panel = f.expanded();
	const ui::HarmPartialStrip* strip = panel.stripForTest();
	REQUIRE(strip != nullptr);

	const auto write = [&](hrm::Param p, double v) {
		ui::FxControl c { f.bridge, f.device, hrm::kDescriptor, static_cast<size_t>(p) };
		c.setValue(v);
		f.bridge.refresh();
	};
	const auto idx = [](int k, hrm::PartialField field) { return static_cast<size_t>(hrm::partialParam(k, field)); };
	// the fields the DETAIL is made of, and the two the STRIP is
	const hrm::PartialField detailFields[] = { hrm::PartialField::Harmonic, hrm::PartialField::Fine, hrm::PartialField::Width,
		                                       hrm::PartialField::Depth,    hrm::PartialField::Phase, hrm::PartialField::Spread,
		                                       hrm::PartialField::Mode };
	const hrm::PartialField stripFields[] = { hrm::PartialField::Level, hrm::PartialField::Active };

	for (int k = 0; k < hrm::kPartialCount; ++k) {
		// SELECTED BY A PRESS ON THE CELL, as a hand does it
		const visage::Bounds head = strip->headerBounds(k);
		const_cast<ui::HarmPartialStrip*>(strip)->onMouseDown().callback(at(head.x() + 6.0f, head.y() + 4.0f));
		(void)panel.updateContent();
		INFO("cell " << k + 1);
		CHECK(panel.selectedPartial() == k);
		CHECK(strip->selected() == k);
		CHECK(panel.scopeForTest()->selectedPartial() == k);

		const std::set<size_t> offered = harmVisibleOffers(&panel);
		for (int j = 0; j < hrm::kPartialCount; ++j) {
			INFO("partial " << j + 1);
			// THE DETAIL IS BOUND TO THE OPEN PARTIAL'S PARAMETERS AND TO NO OTHER'S
			for (const hrm::PartialField field : detailFields) {
				CHECK((offered.count(idx(j, field)) == 1) == (j == k));
			}
			// THE STRIP IS ON SHOW WHICHEVER TAB IS OPEN: every partial's Level and Active, bound
			for (const hrm::PartialField field : stripFields) {
				CHECK(offered.count(idx(j, field)) == 1);
			}
		}
		// the envelope and the master row are the device's, whichever tab is open
		for (const hrm::Param p : { hrm::Param::Delay, hrm::Param::Attack, hrm::Param::Hold, hrm::Param::Decay, hrm::Param::Sustain,
		                            hrm::Param::Release, hrm::Param::Porta, hrm::Param::Poly, hrm::Param::DepthFollowsEnv }) {
			CHECK(offered.count(static_cast<size_t>(p)) == 1);
		}
		// the knobs and the row really are the open partial's
		CHECK(panel.harmonicRow() == panel.harmonicRow(k));
		CHECK(panel.knobsForTest().size() == 5);
		CHECK(strip->activeChip(k)->isVisible());
	}

	SUBCASE("a press on a strip cell — its header or empty space — opens that partial; a press on its chip or pill does not, and selection is view state") {
		const auto row = [&] {
			hrm::Row r {};
			for (size_t i = 0; i < r.size(); ++i) {
				r[i] = f.deviceHost.fieldAt(f.device, i);
			}
			return r;
		}();
		ui::HarmPartialStrip* s = const_cast<ui::HarmPartialStrip*>(strip);
		const visage::Bounds head = s->headerBounds(3);
		s->onMouseDown().callback(at(head.x() + 6.0f, head.y() + 4.0f));
		CHECK(panel.selectedPartial() == 3);
		// empty space in the cell, below the header and right of the chip's end but inside the cell's padding
		const visage::Bounds cell = s->columnBounds(2);
		s->onMouseDown().callback(at(cell.x() + 1.0f, cell.bottom() - 1.0f));
		CHECK(panel.selectedPartial() == 2);
		hrm::Row after {};
		for (size_t i = 0; i < after.size(); ++i) {
			after[i] = f.deviceHost.fieldAt(f.device, i);
		}
		CHECK(row == after); // selecting wrote nothing

		// a press on the Active chip toggles THAT partial and leaves the selection where it was
		panel.selectPartial(0);
		const visage::Bounds chip = s->chipBounds(1);
		const bool was = strip->activeChip(1)->on();
		s->activeChip(1)->onMouseDown().callback(at(chip.width() * 0.5f, chip.height() * 0.5f));
		s->activeChip(1)->onMouseUp().callback(at(chip.width() * 0.5f, chip.height() * 0.5f));
		CHECK(panel.selectedPartial() == 0);
		CHECK(strip->activeChip(1)->on() != was);
		// and the pill likewise
		const visage::Bounds pill = s->pillBounds(2);
		s->levelPill(2)->onMouseDown().callback(at(pill.width() * 0.5f, pill.height() * 0.5f));
		s->levelPill(2)->onMouseUp().callback(at(pill.width() * 0.5f, pill.height() * 0.5f));
		CHECK(panel.selectedPartial() == 0);
	}

	SUBCASE("the strip's chips and pills write the row the engine decodes, for every partial, from any tab") {
		panel.selectPartial(1);
		for (int k = 0; k < hrm::kPartialCount; ++k) {
			INFO("partial " << k + 1);
			ui::FxControl chip { f.bridge, f.device, hrm::kDescriptor, idx(k, hrm::PartialField::Active) };
			CHECK(strip->activeChip(k)->on() == (k == 0)); // partial 1 on by default, the rest off
			write(hrm::partialParam(k, hrm::PartialField::Active), k == 0 ? 0.0 : 1.0);
			CHECK(strip->activeChip(k)->on() == (k != 0));
			write(hrm::partialParam(k, hrm::PartialField::Level), 0.25);
			const hrm::State state = f.state();
			const float level = k == 0 ? state.settings.level : state.settings.more[static_cast<size_t>(k - 1)].level;
			CHECK(level == doctest::Approx(0.25f));
			CHECK(strip->levelPill(k)->amountForTest().value() == doctest::Approx(0.25));
		}
	}
}
TEST_CASE("HARM's scope draws every active partial, the selected one emphasised and last") {
	HarmFixture f;
	namespace hrm = feathers::fx::harm;
	ui::HarmExpanded& panel = f.expanded();
	ui::HarmScope* scope = panel.scopeForTest();
	REQUIRE(scope != nullptr);
	const auto write = [&](hrm::Param p, double v) {
		ui::FxControl c { f.bridge, f.device, hrm::kDescriptor, static_cast<size_t>(p) };
		c.setValue(v);
		f.bridge.refresh();
	};
	const auto ratioIndex = [](int num, int den) { return static_cast<double>(hrm::harmonicIndexOf(num, den)); };

	// partial 1 on (default), partial 3 on at 3:1 and level 0.6, partial 2 and 4 off
	write(hrm::partialParam(2, hrm::PartialField::Active), 1.0);
	write(hrm::partialParam(2, hrm::PartialField::Harmonic), ratioIndex(3, 1));
	write(hrm::partialParam(2, hrm::PartialField::Level), 0.6);

	SUBCASE("tab 1 selected: partials 1 and 3 are drawn, 1 emphasised and last") {
		panel.selectPartial(0);
		const auto plan = scope->drawPlan();
		REQUIRE(plan.size() == 2);
		CHECK(plan[0].partial == 2);
		CHECK_FALSE(plan[0].selected);
		CHECK(plan[1].partial == 0);
		CHECK(plan[1].selected);
		CHECK(plan[1].active);
		// WHERE EACH SITS — the ratio the table holds for the control's index, on the scope's own axis
		CHECK(plan[0].notchX == doctest::Approx(scope->xForRatio(3.0)));
		CHECK(plan[1].notchX == doctest::Approx(scope->xForRatio(1.0)));
		CHECK(plan[0].stem == doctest::Approx(0.6f));
		CHECK(plan[1].stem == doctest::Approx(1.0f));
	}

	SUBCASE("tab 3 selected: the same two, now 3 emphasised and last") {
		panel.selectPartial(2);
		const auto plan = scope->drawPlan();
		REQUIRE(plan.size() == 2);
		CHECK(plan[0].partial == 0);
		CHECK_FALSE(plan[0].selected);
		CHECK(plan[1].partial == 2);
		CHECK(plan[1].selected);
		// and each partial's own curve has its own trough: partial 3's null sits at 3:1
		std::vector<float> gains(ui::HarmScope::kMaxCurvePoints);
		const size_t n = scope->curveGainsFor(2, gains.data(), gains.size());
		REQUIRE(n > 2);
		size_t deepest = 0;
		for (size_t i = 0; i < n; ++i) {
			if (gains[i] < gains[deepest]) {
				deepest = i;
			}
		}
		CHECK(gains[deepest] == doctest::Approx(0.0f).epsilon(1.0e-3));
		const visage::Bounds box = scope->plotBounds();
		const float x = box.x() + box.width() * static_cast<float>(deepest) / static_cast<float>(n - 1);
		CHECK(x == doctest::Approx(scope->xForRatio(3.0)).epsilon(0.01));
	}

	SUBCASE("an inactive partial is not in the plan unless it is the one open, and then as a ghost") {
		panel.selectPartial(1);
		const auto plan = scope->drawPlan();
		REQUIRE(plan.size() == 3);
		CHECK(plan.back().partial == 1);
		CHECK(plan.back().selected);
		CHECK_FALSE(plan.back().active);
		for (const auto& e : plan) {
			CHECK((e.partial == 1 || e.active));
		}
		write(hrm::partialParam(2, hrm::PartialField::Active), 0.0);
		CHECK(scope->drawPlan().size() == 2); // partial 3 is out of the picture the moment it is out of the sound
	}
}
TEST_CASE("HARM's envelope window: the curve is the envelope's shape, and the six boxes are the six stages") {
	HarmFixture f;
	namespace hrm = feathers::fx::harm;
	ui::HarmExpanded& panel = f.expanded();
	const ui::HarmEnvelopeView* view = panel.envelopeViewForTest();
	REQUIRE(view != nullptr);
	const auto write = [&](hrm::Param p, double v) {
		ui::FxControl c { f.bridge, f.device, hrm::kDescriptor, static_cast<size_t>(p) };
		c.setValue(v);
		f.bridge.refresh();
		(void)panel.updateContent();
	};

	SUBCASE("the default envelope is the attack-and-release one: no delay, hold or decay stage, full sustain") {
		const auto& spans = view->spansForTest();
		std::vector<hrm::Envelope::Stage> stages;
		for (const auto& s : spans) {
			stages.push_back(s.stage);
		}
		// attack is off (instant) in the default row, so only the plateau and the release take width
		CHECK(stages == std::vector<hrm::Envelope::Stage> { hrm::Envelope::Stage::Sustain, hrm::Envelope::Stage::Release });
		CHECK(view->timesForTest().sustain == 1.0);
	}

	SUBCASE("every stage shows once it is set, and the curve lands where the law puts each stage") {
		write(hrm::Param::Delay, -4.0);
		write(hrm::Param::Attack, -6.0);
		write(hrm::Param::Hold, -4.0);
		write(hrm::Param::Decay, -3.0);
		write(hrm::Param::Sustain, 0.5);
		write(hrm::Param::Release, -1.0);
		const auto& spans = view->spansForTest();
		REQUIRE(spans.size() == 6);
		const hrm::Envelope::Stage order[] = { hrm::Envelope::Stage::Delay, hrm::Envelope::Stage::Attack, hrm::Envelope::Stage::Hold,
			                                   hrm::Envelope::Stage::Decay, hrm::Envelope::Stage::Sustain, hrm::Envelope::Stage::Release };
		for (size_t i = 0; i < spans.size(); ++i) {
			CHECK(spans[i].stage == order[i]);
			CHECK(spans[i].to > spans[i].from);
			if (i > 0) {
				CHECK(spans[i].from == doctest::Approx(spans[i - 1].to));
			}
		}
		CHECK(spans.back().to == doctest::Approx(1.0f));
		const auto& curve = view->curvePointsForTest();
		// the level at the END of each span, against an independent restatement: 0 through the delay, ~1 at the attack's and
		// hold's ends (the attack hands over within 0.1 %), the sustain at the decay's, and ~0 at the release's
		const auto levelAtX = [&](float x) {
			float best = 0.0f;
			float bestDx = 1.0e9f;
			for (const auto& p : curve) {
				if (std::fabs(p.x - x) < bestDx) {
					bestDx = std::fabs(p.x - x);
					best = p.y;
				}
			}
			return best;
		};
		CHECK(levelAtX(spans[0].to) == doctest::Approx(0.0f).epsilon(1.0e-3));
		CHECK(levelAtX(spans[1].to) == doctest::Approx(1.0f).epsilon(2.0e-3));
		CHECK(levelAtX(spans[2].to) == doctest::Approx(1.0f).epsilon(2.0e-3));
		CHECK(levelAtX(spans[3].to) == doctest::Approx(0.5f).epsilon(2.0e-3));
		CHECK(levelAtX(spans[4].to) == doctest::Approx(0.5f).epsilon(1.0e-6));
		CHECK(std::fabs(levelAtX(spans[5].to)) < 1.0e-3f);
		// and the picture follows the control: its times are the ones the device decodes
		const hrm::State s = f.state();
		CHECK(view->timesForTest().attack == doctest::Approx(static_cast<double>(s.settings.attackSeconds)));
		CHECK(view->timesForTest().sustain == doctest::Approx(static_cast<double>(s.settings.sustain)));
	}

	SUBCASE("each box is bound to its own stage's control") {
		const auto& boxes = panel.envelopeBoxesForTest();
		REQUIRE(boxes.size() == 6);
		const hrm::Param order[] = { hrm::Param::Delay, hrm::Param::Attack, hrm::Param::Hold,
			                         hrm::Param::Decay, hrm::Param::Sustain, hrm::Param::Release };
		for (size_t i = 0; i < boxes.size(); ++i) {
			CHECK(&boxes[i]->control().desc() == &hrm::kParamDescs[static_cast<size_t>(order[i])]);
		}
	}
}
TEST_CASE("HARM's card offers each partial's Active chip in its frame, and each is the partial's own") {
	HarmFixture f;
	namespace hrm = feathers::fx::harm;
	std::set<int> onCard;
	for (const hrm::Param p : f.card->addressedParamsForTest()) {
		onCard.insert(static_cast<int>(p));
	}
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		CHECK(onCard.count(static_cast<int>(hrm::partialParam(k, hrm::PartialField::Active))) == 1);
	}
	// AND THE WINDOW'S STRIP OFFERS EACH ONE TOO, bound to the partial's own control.
	std::set<int> inWindow;
	for (const hrm::Param p : f.expanded().addressedParamsForTest()) {
		inWindow.insert(static_cast<int>(p));
	}
	for (int k = 0; k < hrm::kPartialCount; ++k) {
		CHECK(inWindow.count(static_cast<int>(hrm::partialParam(k, hrm::PartialField::Active))) == 1);
	}
}
TEST_CASE("hardening: OWLTT's graph repaints for a hover only when a tooltip appears, moves or goes") {
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();
	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	ui::OwlttGraph* graph = card->graph();
	REQUIRE(graph != nullptr);
	const auto move = [&](visage::Point p) {
		visage::MouseEvent e;
		e.position = p;
		graph->onMouseMove().callback(e);
	};
	// A BAR, asked of the graph: where a tooltip is.
	const visage::Point bar { graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).x,
		                      graph->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y };
	// AND A POINT WITH NOTHING UNDER IT: found by looking, so the case cannot hover a tooltip by accident.
	visage::Point bare { -1.0f, -1.0f };
	bool found = false;
	for (float y = 2.0f; y < graph->height() && !found; y += 7.0f) {
		for (float x = 2.0f; x < graph->width() && !found; x += 7.0f) {
			const size_t before = graph->hoverRedrawsForTest();
			move({ x, y });
			if (graph->hoverRedrawsForTest() == before) {
				bare = { x, y };
				found = true;
			}
			move({ -50.0f, -50.0f }); // off every target, settling the state for the next probe
		}
	}
	REQUIRE(found);
	graph->onMouseExit().callback(visage::MouseEvent {});

	const size_t start = graph->hoverRedrawsForTest();
	for (int i = 0; i < 20; ++i) {
		move({ bare.x + static_cast<float>(i % 3), bare.y }); // moves among points with nothing under them
	}
	CHECK(graph->hoverRedrawsForTest() == start);
	move(bar); // a tooltip appears
	CHECK(graph->hoverRedrawsForTest() == start + 1);
	graph->onMouseExit().callback(visage::MouseEvent {}); // and goes
	CHECK(graph->hoverRedrawsForTest() == start + 2);
	graph->onMouseExit().callback(visage::MouseEvent {}); // leaving with none shown draws nothing
	CHECK(graph->hoverRedrawsForTest() == start + 2);
}
TEST_CASE("hardening: the envelope card's curve is built once for the settings and handed out without a copy") {
	namespace dsr = feathers::mod::dahdsr;
	ModFixture f;
	feathers::ui::ModState mod;
	feathers::ui::EditHistory history;
	const DeviceInstanceId id = f.bridge.rackDeviceAt(0);
	ui::EnvCard card { f.deviceHost, id, mod, history };
	card.setBounds(0, 0, static_cast<int>(ui::ModPanel::kCardWidth), static_cast<int>(ui::EnvCard::height()));
	const std::vector<visage::Point>& first = card.curvePointsForTest();
	const size_t builds = card.curveRebuildsForTest();
	REQUIRE_FALSE(first.empty());
	for (int i = 0; i < 10; ++i) {
		const std::vector<visage::Point>& again = card.curvePointsForTest();
		CHECK(&again == &first); // the cache itself, not a copy of it
	}
	CHECK(card.curveRebuildsForTest() == builds);
	// A SETTING MOVES: one rebuild, and the points differ.
	const std::vector<visage::Point> before = first;
	const size_t attack = static_cast<size_t>(dsr::Param::Attack);
	const float now = f.deviceHost.fieldAt(id, attack);
	f.deviceHost.setField(id, attack, now > 0.5f ? 0.1f : 0.9f);
	const std::vector<visage::Point>& after = card.curvePointsForTest();
	CHECK(card.curveRebuildsForTest() == builds + 1);
	CHECK(after != before);
}
TEST_CASE("a bar drag on OWLTT's graph moves the control it is on") {
	// THE GUARD FOR "I CANT SEEM TO DRAG ANYTHING IN THE GRAPH VIEW".
	//
	// The bar logic was installed on `onMouseMove`, which visage delivers for a HOVER. Presses and
	// releases arrived; not one motion between them did. Every existing case drove the handlers
	// directly or checked geometry, so the suite stayed green over a graph nobody could drag.
	//
	// SO THIS DRIVES THE SLOT visage WOULD DRIVE, rather than calling the lambda by name: if the
	// logic moves to the wrong callback again, this goes red.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);

	// SWITCH IT ON, or the bars are drawn over a device doing nothing and the value road is the only
	// thing under test.
	const size_t enableIdx = static_cast<size_t>(fx::owltt::Param::Enabled);
	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), enableIdx, 1.0f);

	const size_t thresholdIdx = static_cast<size_t>(fx::owltt::Param::LowThreshold);
	const float before = f.bridge.fxChain().device[1].row.field[thresholdIdx];

	// ASKED, NOT COMPUTED — and the three cases here used to compute it.
	//
	// They aimed at `box.bottom() - value * box.height()`, which is the STRIP's mapping. A threshold
	// sweeps 0.2..1.0 over the box, so the true bar is at `bottom - ((v - 0.2) / 0.8) * height`. In a
	// 48px box those two are 6px apart and the 8px hit band swallowed the difference; the cases
	// passed on a coincidence. The graph is 160px now and they are 20px apart, so all three missed
	// at once.
	//
	// THE FIX IS NOT BETTER ARITHMETIC HERE. `ringCentre` is where the card puts the handle, so a
	// case that asks it aims where a hand aims and cannot drift when the geometry moves again. A test
	// that restates a layout is the same defect as code that does — this file has now paid for that
	// twice in the same three cases.
	const visage::Point on { card->graph()->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).x,
		                     card->graph()->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y };

	card->graph()->onMouseDown().callback(at(on.x, on.y));
	card->graph()->onMouseDrag().callback(at(on.x, on.y - 20.0f));
	card->graph()->onMouseUp().callback(at(on.x, on.y - 20.0f));

	const float after = f.bridge.fxChain().device[1].row.field[thresholdIdx];
	CAPTURE(before);
	CAPTURE(after);
	CHECK(after != before);
}
TEST_CASE("OWLTT's graph handles light for the armed source and draw the routed reach — including the band level bars") {
	// THE STRIPS AND BARS TOOK A MODULATION DRAG AND NEVER SAID SO. `routeGestureWanted` asked the
	// right question on a press; `draw` never asked it, painted the Ratio strip in the modulation colour
	// at rest and the Level strip never, and drew no reach for a route once made. So a hand could route
	// a macro at a band Level and see nothing change, before or after. `lookOf` and `reachOf` are what
	// the drawing reads now, and they are what this case reads — a colour is not observable headlessly,
	// the answer that chooses it is.
	FilterFixture f;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	f.lane->resized();
	auto* card = dynamic_cast<ui::OwlttCard*>(f.lane->cardAt(1));
	REQUIRE(card != nullptr);
	ui::OwlttGraph* graph = card->graph();
	REQUIRE(graph != nullptr);
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);
	using Handle = ui::OwlttGraph::Handle;
	constexpr std::array<Handle, 4> kHandles { Handle::BandwidthBar, Handle::ThresholdBar, Handle::RatioStrip, Handle::LevelStrip };

	// THE ROUTED REACH IS THE HANDLE'S OWN AMOUNT STROKE (`ModHandle`'s plan): this reads it back as the rectangle the strokes spans,
	// along the handle's vertical axis, so the assertions below keep their form.
	const auto reachOf = [&](size_t band, Handle h) {
		const ui::ModHandlePlan plan = graph->bandModPlan(band, h);
		if (plan.axes.empty() || !plan.axes[0].hasStroke) {
			return visage::Bounds {};
		}
		const visage::Point c = graph->ringCentre(band, h);
		return visage::Bounds { c.x - 4.0f, plan.axes[0].strokeLo, 8.0f, plan.axes[0].strokeHi - plan.axes[0].strokeLo };
	};
	const backplane::Source macro = backplane::Source::core(0);
	const DeviceInstanceId owltt = f.bridge.fxDeviceAt(1);
	const backplane::Destination lowLevel =
	    backplane::Destination::control(owltt, static_cast<size_t>(fx::owltt::Param::LowLevel));
	const backplane::Destination lowThreshold =
	    backplane::Destination::control(owltt, static_cast<size_t>(fx::owltt::Param::LowThreshold));

	// NOTHING ARMED: no handle is lit and nothing reaches, on every handle of every band.
	for (size_t b = 0; b < 3; ++b) {
		for (const Handle h : kHandles) {
			CAPTURE(b);
			CAPTURE(static_cast<int>(h));
			CHECK_FALSE(graph->lookOf(b, h).lit);
			CHECK(reachOf(b, h).height() == 0.0f);
		}
	}

	// ARMED, UNROUTED: every handle the macro may reach is lit, and there is still nothing to draw as a
	// reach — the glow says "you may point here", a zero-length reach would be a mark with nothing to say.
	mod->focus(macro);
	mod->arm(true);
	REQUIRE(f.bridge.mayRoute(macro, lowLevel));
	for (size_t b = 0; b < 3; ++b) {
		for (const Handle h : kHandles) {
			CAPTURE(b);
			CAPTURE(static_cast<int>(h));
			CHECK(graph->lookOf(b, h).lit);
			CHECK(reachOf(b, h).height() == 0.0f);
		}
	}

	SUBCASE("a route at the Level strip draws its reach from the value, the sign choosing the side") {
		constexpr float kAmount = 0.2f;
		f.bridge.setRouteAmount(macro, lowLevel, kAmount);
		const ui::OwlttGraph::HandleLook look = graph->lookOf(0, Handle::LevelStrip);
		CHECK(look.lit);
		CHECK(look.amount == doctest::Approx(kAmount));
		CHECK_FALSE(look.bipolar); // a macro in unipolar mode

		const visage::Point onValue = graph->ringCentre(0, Handle::LevelStrip);
		const visage::Bounds up = reachOf(0, Handle::LevelStrip);
		CAPTURE(onValue.y);
		CAPTURE(up.y());
		CAPTURE(up.bottom());
		REQUIRE(up.height() > 0.0f);
		// FROM THE VALUE UPWARD: the reach's bottom edge is the ring's centre (the value), and it covers
		// the strip's own columns, not the band's — so it reads as the strip's.
		CHECK(up.bottom() == doctest::Approx(onValue.y).epsilon(0.02));
		CHECK(up.x() <= onValue.x);
		CHECK(up.right() >= onValue.x);
		// AND ONLY THAT STRIP: the Ratio strip beside it has no route and draws no reach.
		CHECK(reachOf(0, Handle::RatioStrip).height() == 0.0f);
		CHECK(reachOf(1, Handle::LevelStrip).height() == 0.0f);

		// TWICE THE AMOUNT IS TWICE THE REACH — the restatement that pins the mapping as linear in the
		// value without restating the box's height here.
		f.bridge.setRouteAmount(macro, lowLevel, 2.0f * kAmount);
		const visage::Bounds twice = reachOf(0, Handle::LevelStrip);
		CHECK(twice.height() == doctest::Approx(2.0f * up.height()).epsilon(0.02));

		// A NEGATIVE AMOUNT HANGS BELOW THE VALUE, same size.
		f.bridge.setRouteAmount(macro, lowLevel, -kAmount);
		const visage::Bounds down = reachOf(0, Handle::LevelStrip);
		CHECK(down.y() == doctest::Approx(onValue.y).epsilon(0.02));
		CHECK(down.height() == doctest::Approx(up.height()).epsilon(0.02));

		// A BIPOLAR SOURCE REACHES BOTH WAYS: the macro switched to bipolar mode (backlog EG) straddles
		// the value with twice the span, ±a identical by construction.
		f.bridge.setMacroBipolar(0, true);
		CHECK(graph->lookOf(0, Handle::LevelStrip).bipolar);
		const visage::Bounds both = reachOf(0, Handle::LevelStrip);
		CHECK(both.y() < onValue.y);
		CHECK(both.bottom() > onValue.y);
		CHECK(both.height() == doctest::Approx(2.0f * up.height()).epsilon(0.02));
		f.bridge.setRouteAmount(macro, lowLevel, kAmount);
		const visage::Bounds bothPlus = reachOf(0, Handle::LevelStrip);
		CHECK(bothPlus.y() == doctest::Approx(both.y()).epsilon(0.001));
		CHECK(bothPlus.height() == doctest::Approx(both.height()).epsilon(0.001));
		f.bridge.setMacroBipolar(0, false);

		// DISARMED: nothing lit, and - as on the crossovers and the Drawn puck - the amount stroke stays, since the handle draws its
		// route in both states.
		mod->arm(false);
		CHECK_FALSE(graph->lookOf(0, Handle::LevelStrip).lit);
		CHECK(reachOf(0, Handle::LevelStrip).height() > 0.0f);
		CHECK(f.bridge.routeAmount(macro, lowLevel) == doctest::Approx(kAmount));
	}

	SUBCASE("a route at the threshold bar draws its reach across the band, from the bar") {
		constexpr float kAmount = 0.1f;
		f.bridge.setRouteAmount(macro, lowThreshold, kAmount);
		// A BAR IS A `ModHandle`: its route is the handle's own amount stroke and live dot, not a rectangle (`reachOf` is the
		// strips' alone). The plan is read by value: the stroke runs from the bar's height at the stored threshold to its
		// height at the threshold plus the amount, both through the plot's 0..1 vertical mapping, restated here.
		const visage::Bounds area = graph->barArea();
		const float stored = f.bridge.fxChain().device[1].row.field[static_cast<size_t>(fx::owltt::Param::LowThreshold)];
		const auto yOf = [&](float v) { return area.bottom() - v * area.height(); };
		const ui::ModHandlePlan plan = graph->bandModPlan(0, Handle::ThresholdBar);
		REQUIRE(plan.axes.size() == 1u);
		CHECK(plan.axes[0].offered);
		REQUIRE(plan.axes[0].hasStroke);
		CHECK(plan.axes[0].strokeHi == doctest::Approx(yOf(stored)).epsilon(0.001));
		CHECK(plan.axes[0].strokeLo == doctest::Approx(yOf(stored + kAmount)).epsilon(0.001));
		CHECK(plan.axes[0].base == doctest::Approx(yOf(stored)).epsilon(0.001));
		// AND THE BANDWIDTH BAR, which a threshold route does not touch, draws no stroke.
		const ui::ModHandlePlan other = graph->bandModPlan(0, Handle::BandwidthBar);
		REQUIRE(other.axes.size() == 1u);
		CHECK_FALSE(other.axes[0].hasStroke);
	}
}
TEST_CASE("a destroyed card leaves no pointer behind in the state watchers") {
	// BACKLOG 19 -- the reproduction, before the fix.
	//
	// The crash was hit: `ParamControl::setAmount` -> `mod_->touched()` -> `FrameWatchers::notify()`,
	// which calls `redraw()` on EVERY entry. `ui_state.h` predicts this failure in its own words --
	// "a list holding a freed frame is a use-after-free the next time anything moves" -- and provides
	// `unwatch` for it. Nothing but the expanded overlay ever calls it, and `FxLane::ensureCard`
	// subscribes every card it builds.
	//
	// SO THE ASSERTION IS ABOUT THE LIST, NOT ABOUT A FAULT. A use-after-free is not reliably
	// observable: without a sanitiser the freed card's memory usually still looks like a card and the
	// redraw returns quietly, which is exactly why this shipped. What IS deterministic is whether the
	// dead frame is still on the list, and `watches()` only COMPARES the pointer -- it never
	// dereferences one -- so the case can ask the question without performing the bug.
	FilterFixture f;
	auto* lane = f.lane;
	const ui::FrameWatchers* watchers = f.gui.stateWatchers();
	REQUIRE(watchers != nullptr);

	const size_t before = watchers->count();

	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();
	const visage::Frame* card = lane->cardAt(1);
	REQUIRE(card != nullptr);
	REQUIRE(watchers->watches(card)); // it subscribed as it was built -- ensureCard says so

	// AND NOW KILL IT, the way a hand does: remove the device. `ensureCard` retires the card on the
	// first pass and frees the retired list on the second, which is the two-step this file added to
	// stop a card being destroyed under a scheduled paint.
	REQUIRE(f.bridge.removeFxDevice(1));
	lane->resized();
	lane->resized();

	// `card` is dangling from here on and is only ever COMPARED.
	CHECK_FALSE(watchers->watches(card));
	CHECK(watchers->count() == before);
}
TEST_CASE("cycling a rack slot's device does not grow the redraw list") {
	// THE HALF THAT THE FIRST FIX FOR THIS BUG MISSED, and the reason the sibling case below is not
	// enough on its own. `ExpandedOverlay::dismissBy` already unwatched its content when it retired
	// it -- correct about the mechanism, correct about the ORDER, and about ONE FRAME. Every knob
	// inside that content stayed on the list, because `Knob::setModulation` subscribes each
	// individually.
	//
	// A OWLTT CARD CANNOT SHOW THIS. Its controls are device controls, repainted through the state
	// channel, and they never subscribe themselves -- so for that card the subtree IS the root and a
	// root-only fix passes. A RACK card is the opposite: `env_card.cpp:153-155` and
	// `mseg_card.cpp:126-128` put the card AND each of its value boxes on the list separately.
	//
	// A LEAK IS ASSERTED AS A LEAK -- cycle the slot and require the count to come back -- rather than
	// by naming the widgets that should have gone. Nothing has to know how many boxes a card has, or
	// that the two cards have different numbers of them, and a subscription added to either card
	// later is covered without touching this case. A root-only unwatch grows the list on every
	// crossing, so three round trips fail it by six.
	ModFixture f;
	ui::ModState* mod = f.gui.modForTest();
	REQUIRE(mod != nullptr);

	// SETTLE FIRST. The panel builds a card per pool slot up front, so the baseline is taken after
	// the pool exists rather than before -- otherwise this measures construction, not leaking.
	f.panel->resized();
	const size_t before = mod->watchers().count();

	const size_t slot = f.bridge.addRackDevice(mod::mseg::kTraits.tag);
	REQUIRE(slot < kMaxRackSlots);
	f.panel->resized();

	// NON-VACUITY: the slot really did change what it holds, so the rebuild this case is about
	// happened at all. Without this the loop below could cycle nothing and still come back equal.
	REQUIRE(mod->watchers().count() != before);

	for (int round = 0; round < 3; ++round) {
		REQUIRE(f.bridge.removeRackDevice(slot));
		f.panel->resized();
		f.panel->resized();
		REQUIRE(f.bridge.addRackDevice(mod::mseg::kTraits.tag) == slot);
		f.panel->resized();
		f.panel->resized();
	}
	REQUIRE(f.bridge.removeRackDevice(slot));
	f.panel->resized();
	f.panel->resized();

	CHECK(mod->watchers().count() == before);
}
TEST_CASE("every knob on the OWLTT card has its own space, and the card reserves it") {
	// THE GUARD AGAINST THE ROW THAT PILES UP.
	//
	// `layOut` was `(row == 0) ? row1Y : row2Y` -- so the grid did not hold six knobs, it held six
	// SAFELY and drew every further one on top of the knob three places back. THE FAILURE STILL
	// RENDERS, which is why it was never seen: two knobs in one rectangle look like one knob, and the
	// hidden one is simply unclickable.
	//
	// That is what made the twelfth control expensive, and the six that were REMOVED to make room --
	// Attack, Release, Character, Vibe, Output, Blend -- left with the reachability table edited to
	// record them as absent. Four of a compressor's most basic controls.
	//
	// TWO CLAIMS, BOTH ASSERTED: no knob overlaps another (the layout is honest), and the card
	// reserves enough height for the last row (`openHeight` agrees with `layOut`). The second is not
	// implied by the first -- a perfect grid drawn past the bottom of the card is exactly what a
	// static height function that was told the wrong row count produces.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);

	std::vector<ui::Knob*> knobs;
	collectKnobs(card, &knobs);
	// NOT A SPOT CHECK. Every control the card declares must be here, so a knob dropped to make room
	// fails this rather than quietly shrinking the card's vocabulary.
	REQUIRE(knobs.size() == ui::OwlttCard::kCardKnobCount);

	const float reserved = ui::OwlttCard::openHeight();
	for (size_t i = 0; i < knobs.size(); ++i) {
		const visage::Bounds a = knobs[i]->bounds();
		CAPTURE(i);
		CHECK(a.width() > 0.0f);
		CHECK(a.height() > 0.0f);
		// INSIDE THE CARD IT IS DRAWN ON. A knob below this line is laid out correctly and clipped.
		CHECK(a.bottom() <= reserved);

		for (size_t j = i + 1; j < knobs.size(); ++j) {
			const visage::Bounds b = knobs[j]->bounds();
			CAPTURE(j);
			const bool apart = a.right() <= b.x() || b.right() <= a.x() || a.bottom() <= b.y()
			                   || b.bottom() <= a.y();
			CHECK(apart);
		}
	}
}
TEST_CASE("OWLTT's strips win the pixels they share with a threshold bar") {
	// The level slider's hit box sits on top of the threshold bar's.
	//
	// The bars span the band's full width; the strips are its right 30% and run the FULL HEIGHT. So
	// wherever a bar crosses a strip they want the same pixel, and with the bars asked first the
	// strip carried a dead band 16px tall at whatever height the threshold currently sat -- which at
	// the default is its middle.
	//
	// THE CASE AIMS AT THE OVERLAP ITSELF, which is the only place the order is observable. Both
	// halves are asserted: the strip's control must take the value the drag implies, AND the
	// threshold must be untouched. Checking only that "something moved" would pass on the old order
	// too, because on the old order something did -- the wrong thing.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Enabled), 1.0f);

	const size_t levelIdx = static_cast<size_t>(fx::owltt::Param::LowLevel);
	const size_t threshIdx = static_cast<size_t>(fx::owltt::Param::LowThreshold);

	const visage::Bounds box = card->graph()->plot();
	const float lowEdge = card->graph()->bandEdge(0);
	const float bandW = lowEdge - box.x();

	// THE OVERLAP: inside the level strip's 0.85..1.00 of the band, at the exact height the upper
	// threshold bar is drawn. Asked of the card rather than assumed -- the bar moves with its value.
	const float thresholdBefore = f.bridge.fxChain().device[1].row.field[threshIdx];
	const float x = box.x() + bandW * 0.92f;
	// THE BAR'S OWN HEIGHT, asked of the card. See the note in the bar-drag case above: this used to
	// be `bottom - value * height`, which is the STRIP's mapping and not a threshold's.
	const float barY = card->graph()->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y;

	const float endY = barY - 20.0f;
	card->graph()->onMouseDown().callback(at(x, barY));
	card->graph()->onMouseDrag().callback(at(x, endY));
	card->graph()->onMouseUp().callback(at(x, endY));

	// THE VALUE THE DRAG IMPLIES, restated independently of the handler rather than compared against
	// where it started: the strip maps the pointer's height onto the box as a 0..1 sweep.
	const float expected = std::clamp((box.bottom() - endY) / box.height(), 0.0f, 1.0f);
	CHECK(f.bridge.fxChain().device[1].row.field[levelIdx] == doctest::Approx(expected));
	// AND THE BAR DID NOT MOVE. This is the half that fails on the old order.
	CHECK(f.bridge.fxChain().device[1].row.field[threshIdx] == doctest::Approx(thresholdBefore));

	SUBCASE("and the bar is still grabbable over the width the strips do not claim") {
		// The strips are bought with 30% of a wide target, not with the bar's reachability. If this
		// fails the fix traded one unreachable control for another.
		const float barX = box.x() + bandW * 0.35f; // clear of the strips at 0.70..1.00
		card->graph()->onMouseDown().callback(at(barX, barY));
		card->graph()->onMouseDrag().callback(at(barX, barY - 20.0f));
		card->graph()->onMouseUp().callback(at(barX, barY - 20.0f));

		// A BAR AND A STRIP DO NOT SHARE A MAPPING, which is worth pinning here because the two are
		// a pixel apart on screen and this case's first draft assumed they did. A strip sweeps 0..1
		// over the box; a THRESHOLD sweeps 0.2..1.0 over the same box, because 0.2 is the floor the
		// card draws from. Restating the strip's arithmetic for the bar predicted 0.917 where the
		// handler produced 0.933 -- a wrong number that is close enough to look like a rounding
		// argument, which is the exact reason the reference has to be the intended path rather than
		// a plausible one.
		// (SINCE THE PER-BAND REBUILD a threshold is its own 0..1 over the box too: the plot reads the parameter, not the
		// compressor's internal 0.2 + 0.8 t, so the mapping the paragraph above warns about is the strip's again.)
		const float wanted = std::clamp((box.bottom() - (barY - 20.0f)) / box.height(), 0.0f, 1.0f);
		CHECK(f.bridge.fxChain().device[1].row.field[threshIdx] == doctest::Approx(wanted));
	}
}
TEST_CASE("OWLTT's graph bars reset on a double-click") {
	// THE GUARD FOR "DOUBLE CLICK ON BARS DOESNT SEEM TO DEFAULT THE SETTINGS?".
	//
	// Nothing was broken. The branch had never been written: `Knob::onMouseDown` asks
	// `repeatClickCount()` before it anchors, and this handler reimplemented the DRAG faithfully --
	// borrowing Knob's own constants for it -- while the gesture that is NOT a drag had nowhere to
	// live except inside the dial it came from.
	//
	// IT ASSERTS THE VALUE IS THE DEFAULT, not that the double-click changed something. A difference
	// check passes for a second drag starting from wherever the bar already sat, which is exactly the
	// bug -- and this repo's rule about partial effects surviving difference checks was written for
	// cases that look no worse than this one would have.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Enabled), 1.0f);

	const size_t idx = static_cast<size_t>(fx::owltt::Param::LowThreshold);
	const auto def = static_cast<float>(fx::owltt::kParamDescs[idx].def);

	// MOVE IT OFF THE DEFAULT BY DRAGGING THE BAR -- through the very handler under test, so the
	// case cannot pass by resetting a control the gesture could never have reached in the first
	// place. The bar is where the card draws it: the threshold's own height in the crossover box.
	const visage::Bounds box = card->graph()->plot();
	const float lowEdge = card->graph()->bandEdge(0);
	const float x = box.x() + (lowEdge - box.x()) * 0.5f;
	const float barY = card->graph()->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y;

	card->graph()->onMouseDown().callback(at(x, barY));
	card->graph()->onMouseDrag().callback(at(x, barY - 25.0f));
	card->graph()->onMouseUp().callback(at(x, barY - 25.0f));

	const float moved = f.bridge.fxChain().device[1].row.field[idx];
	CAPTURE(def);
	CAPTURE(moved);
	REQUIRE(moved != def); // the drag reached it -- otherwise the reset below proves nothing

	// AND NOW THE DOUBLE-CLICK, AIMED AT THE BAR WHERE IT NOW IS rather than where it started —
	// asked of the card a SECOND time, because the bar moved and its position is a function of the
	// value, not a number this case may carry forward. Recomputing it here from `moved` is what it
	// used to do, with the strip's mapping, and it aimed 20px off the bar in a 160px box.
	const float movedY = card->graph()->ringCentre(0, ui::OwlttGraph::Handle::ThresholdBar).y;
	card->graph()->onMouseDown().callback(doubleClickAt(x, movedY));

	CHECK(f.bridge.fxChain().device[1].row.field[idx] == doctest::Approx(def));

	SUBCASE("and the reset is not dragged straight back off it") {
		// A double-click that armed the drag would have the bar following the pointer afterwards, so
		// the release below would write the pointer's height over the default. `dragState_.active`
		// must stay false.
		card->graph()->onMouseDrag().callback(at(x, movedY - 30.0f));
		card->graph()->onMouseUp().callback(at(x, movedY - 30.0f));
		CHECK(f.bridge.fxChain().device[1].row.field[idx] == doctest::Approx(def));
	}
}
TEST_CASE("OWLTT's level strip is draggable where it is drawn") {
	// THE GUARD FOR "those yellow bars ... dont seem to be draggable".
	//
	// The strips were DRAWN at 0.70..1.00 of each band and HIT-TESTED at 0.33..0.67. So the visible
	// strip answered nothing, and the middle of the band -- where the GR meter is -- silently dragged
	// ratio and level. One bug a hand can see and one it cannot.
	//
	// This aims at the strip's DRAWN position, which is the only place a hand can aim, so a drift
	// between the two geometries fails here rather than in a player's hands.
	FilterFixture f;
	auto* lane = f.lane;
	REQUIRE(f.bridge.addFxDevice(fx::owltt::kTraits.tag) == 1);
	lane->resized();

	auto* card = dynamic_cast<ui::OwlttCard*>(lane->cardAt(1));
	REQUIRE(card != nullptr);
	f.bridge.setDeviceControl(f.bridge.fxDeviceAt(1), static_cast<size_t>(fx::owltt::Param::Enabled), 1.0f);

	const size_t levelIdx = static_cast<size_t>(fx::owltt::Param::LowLevel);
	const float before = f.bridge.fxChain().device[1].row.field[levelIdx];

	// THE LOW BAND'S LEVEL STRIP. The bands are NOT equal thirds -- they are split by crossover
	// FREQUENCY on a log axis -- so the edge is asked of the card rather than assumed. My first
	// draft of this case divided the width by three and aimed into the mid band, which is the same
	// mistake in a test that the code had in its hit test.
	const visage::Bounds box = card->graph()->plot();
	const float lowEdge = card->graph()->bandEdge(0);
	const float bandW = lowEdge - box.x();
	const float x = box.x() + bandW * 0.92f; // inside 0.85..1.00 of the low band
	// CLEAR OF BOTH BARS. The bar hit test runs first and claims anything within 8px of an edge; at
	// the default threshold the upper bar sits about six pixels from the box's centre, so aiming at
	// the middle tests the bar rather than the strip -- a position assumed instead of derived, which
	// is the same class of mistake as the bug under test.
	const float y = box.y() + box.height() * 0.15f;

	card->graph()->onMouseDown().callback(at(x, y));
	card->graph()->onMouseDrag().callback(at(x, y - 20.0f));
	card->graph()->onMouseUp().callback(at(x, y - 20.0f));

	const float after = f.bridge.fxChain().device[1].row.field[levelIdx];
	CAPTURE(before);
	CAPTURE(after);
	CHECK(after != before);
}
// =================================================================================================
// THE FIVE FIELD MODIFIER KNOBS ON EXPLORE (2026-08-25)
// =================================================================================================

TEST_CASE("the left flank's four stand in order, and the Warp dropdown sits under its knob") {
	// THE GEOMETRY THE LEFT STACK NEVER HAD A CASE FOR, and the omission had teeth one flank over:
	// when Warp briefly took a sixth slot on the RIGHT, the run overflowed and the top knob landed at
	// y = -28. That was caught only because the right stack's case checks `y >= 0`. The left stack
	// grew from three to four in the same arc with nothing watching it at all.
	ExploreFixture f;
	f.set(ParamId::OscType, 2.0 /* V5 */);
	f.set(ParamId::AEnable, 1.0);
	f.stage.update();

	using FlagshipKind = ExploreStage::FlagshipKind;
	const FlagshipKind kinds[] = { FlagshipKind::Width, FlagshipKind::FmDepth, FlagshipKind::Warp,
		                          FlagshipKind::Smoothness };

	const visage::Bounds right = f.stage.fieldKindBounds(ExploreStage::FieldKind::Quantize);
	float previousBottom = -1.0f;
	for (const FlagshipKind kind : kinds) {
		CAPTURE(static_cast<int>(kind));
		const visage::Bounds b = f.stage.flagshipKindBounds(kind);
		CHECK(b.width() > 0.0f);
		CHECK(b.height() > 0.0f);
		// LEFT OF THE CHART AND CLEAR OF THE RIGHT STACK — the two flanks are two ideas and must not
		// come to share a column.
		CHECK(b.x() + b.width() < right.x());
		// TOP TO BOTTOM IN ENUM ORDER, AND NOT OVERLAPPING. Ordering alone would pass on four knobs
		// drawn on top of each other one pixel apart.
		CHECK(b.y() >= previousBottom);
		previousBottom = b.y() + b.height();
		// ON SCREEN. The stack went three -> four when Warp moved here (to the left, near FM), and this
		// is the assertion that says it still fits.
		CHECK(b.y() >= 0.0f);
		CHECK(b.y() + b.height() <= f.stage.height());
	}

	SUBCASE("Warp is next to FM Depth, which is the whole reason it is on this flank") {
		// THE ADJACENCY IS THE REQUEST, so it is asserted rather than left to the enum order to imply.
		// FM Depth and Warp are the engine's two per-sound AMOUNTS; the knobs around them are biases.
		const visage::Bounds fm = f.stage.flagshipKindBounds(FlagshipKind::FmDepth);
		const visage::Bounds warp = f.stage.flagshipKindBounds(FlagshipKind::Warp);
		CHECK(warp.y() > fm.y());
		for (const FlagshipKind kind : kinds) {
			if (kind == FlagshipKind::FmDepth || kind == FlagshipKind::Warp) {
				continue;
			}
			// NOTHING ELSE LIES BETWEEN THEM — strictly between, which is the claim. The first
			// spelling of this asked only "is it below FM Depth" and flagged Smoothness, which sits
			// below WARP and is therefore not between anything.
			const visage::Bounds other = f.stage.flagshipKindBounds(kind);
			// A NAMED BOOL RATHER THAN THE CONJUNCTION INLINE — doctest expands the expression and
			// refuses an `&&`: "Expression Too Complex Please Rewrite As Binary Comparison".
			const bool between = other.y() > fm.y() + 0.5f && other.y() < warp.y() - 0.5f;
			CHECK_FALSE(between);
		}
	}

	SUBCASE("each knob is hit-testable at the centre of its own rectangle") {
		Knob* knobs[] = { &f.stage.widthKnobForTest(0), &f.stage.fmDepthKnobForTest(0),
			              &f.stage.warpKnobForTest(0), &f.stage.smoothnessKnobForTest(0) };
		for (size_t i = 0; i < std::size(kinds); ++i) {
			CAPTURE(i);
			REQUIRE(knobs[i]->isVisible());
			const visage::Bounds b = f.stage.flagshipKindBounds(kinds[i]);
			const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
			CHECK(f.stage.frameAtPoint(centre) == knobs[i]);
		}
	}

	SUBCASE("the dropdown sits under the Warp knob, on screen, and reads the engine's mode") {
		const visage::Bounds warp = f.stage.flagshipKindBounds(FlagshipKind::Warp);
		const visage::Bounds box = f.stage.warpModeBounds();
		CHECK(box.width() > 0.0f);
		CHECK(box.height() > 0.0f);
		// UNDER IT AND ALIGNED WITH IT — "a drop down near the knob" means adjacent to that knob, not
		// merely on the same flank.
		CHECK(box.y() >= warp.y());
		CHECK(box.x() == doctest::Approx(warp.x()));
		CHECK(box.y() + box.height() <= f.stage.height());

		// **AND A PRESS AT ITS CENTRE REACHES IT** — the half this case was missing, and the half that
		// matters. The defect: the dropdown was hidden by another knob's hit box.
		//
		// BOUNDS BEING SANE IS NOT THE SAME FACT AS BEING REACHABLE, which the right flank's own case
		// says in as many words and this one did not do. The box is drawn by the STAGE, so the stage
		// is what must answer a pointer at its centre; a Knob answering there means the box renders
		// under a control that swallows the click, which is this family's most-repeated bug.
		const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
		CHECK(dynamic_cast<Knob*>(f.stage.frameAtPoint(centre)) == nullptr);

		// AND IT IS CLEAR OF EVERY KNOB RECTANGLE, stated separately so a failure says WHICH — a
		// hit-test result alone names the winner, not the overlap that produced it.
		for (const FlagshipKind kind : kinds) {
			CAPTURE(static_cast<int>(kind));
			const visage::Bounds k = f.stage.flagshipKindBounds(kind);
			const bool overlaps = box.y() < k.y() + k.height() && k.y() < box.y() + box.height();
			CHECK_FALSE(overlaps);
		}

		// AND IT NAMES WHAT THE ENGINE IS DOING, by value rather than by picture.
		CHECK(std::string(f.stage.warpModeLabel()) == "Off");
		f.set(ParamId::WarpMode, static_cast<double>(feathers::WarpMode::Mirror));
		f.stage.update();
		CHECK(std::string(f.stage.warpModeLabel()) == "Mirror");
	}

	SUBCASE("the dropdown is dead off V5, exactly like the knobs above it, and not drawn") {
		visage::Canvas canvas;
		CHECK(f.stage.warpModeLive());
		CHECK(f.stage.drawWarpModeBoxForTest(canvas));
		f.set(ParamId::OscType, 0.0 /* Morph */);
		f.stage.update();
		CHECK_FALSE(f.stage.warpModeLive());
		CHECK_FALSE(f.stage.drawWarpModeBoxForTest(canvas));
	}
}
TEST_CASE("the five field modifier knobs stand on the right flank, and each is reachable") {
	// TWO OF THE LAST THREE UI BUGS IN THIS FAMILY WERE "DRAWN BUT UNREACHABLE" — a mark that renders,
	// looks grabbable, and answers no pointer. So the geometry claim and the HIT-TEST claim are made
	// separately: bounds being sane is not the same fact as frameAtPoint returning the knob.
	ExploreFixture f;
	f.set(ParamId::OscType, 2.0 /* V5 */);
	f.set(ParamId::AEnable, 1.0);
	f.stage.update();

	using FieldKind = ExploreStage::FieldKind;
	const FieldKind kinds[] = { FieldKind::Quantize, FieldKind::Sync, FieldKind::Feedback,
		                       FieldKind::Points, FieldKind::Grit };

	// THE RIGHT STRIP, NOT THE LEFT. The left flank's three keep their V16 geometry untouched, which
	// is half of why the five went right; asserting the separation is what keeps a later layout tidy
	// from quietly stacking eight knobs in one column.
	const visage::Bounds left = f.stage.flagshipKindBounds(ExploreStage::FlagshipKind::Width);
	float previousTop = -1.0f;
	for (const FieldKind kind : kinds) {
		CAPTURE(static_cast<int>(kind));
		const visage::Bounds b = f.stage.fieldKindBounds(kind);
		CHECK(b.width() > 0.0f);
		CHECK(b.height() > 0.0f);
		// Right of the chart, and clear of the left stack entirely.
		CHECK(b.x() > left.x() + left.width());
		// Top to bottom in enum order, so the stack reads in the order the header names them.
		CHECK(b.y() > previousTop);
		previousTop = b.y();
		// ON SCREEN. A stack of five is taller than the stack of three this strip was sized beside,
		// and a knob pushed off the frame is the failure mode that costs nothing to check for.
		CHECK(b.y() >= 0.0f);
		CHECK(b.y() + b.height() <= f.stage.height());
	}

	// AND EACH IS ACTUALLY HIT-TESTABLE at the centre of its own rectangle — asked of the stage, the
	// way a pointer asks it, rather than of the knob's own bounds.
	for (const FieldKind kind : kinds) {
		CAPTURE(static_cast<int>(kind));
		Knob& knob = f.stage.fieldKnobForTest(kind, 0);
		REQUIRE(knob.isVisible());
		const visage::Bounds b = f.stage.fieldKindBounds(kind);
		const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
		CHECK(f.stage.frameAtPoint(centre) == &knob);
	}
}
TEST_CASE("a field modifier knob edits its own sound's own parameter, and only that one") {
	// The same claim the width knob's case makes, for the five that joined it: the visible knob is
	// the selected slot's, a drag lands on THAT slot's parameter, and the other seven slots are
	// untouched. Spelled as eight slot-bound Knobs and a visibility flip, so the binding can never
	// point somewhere the label does not say.
	ExploreFixture f;
	f.set(ParamId::OscType, 2.0 /* V5 */);
	f.set(ParamId::AEnable, 1.0);
	f.set(ParamId::BEnable, 1.0);
	f.stage.update();

	using FieldKind = ExploreStage::FieldKind;
	const struct { FieldKind kind; ParamId slotA; ParamId slotB; } kCases[] = {
	    { FieldKind::Quantize, ParamId::AQuantize, ParamId::BQuantize },
	    { FieldKind::Sync, ParamId::ASync, ParamId::BSync },
	    { FieldKind::Feedback, ParamId::AFeedback, ParamId::BFeedback },
	    { FieldKind::Points, ParamId::APoints, ParamId::BPoints },
	    { FieldKind::Grit, ParamId::AGrit, ParamId::BGrit },
	};

	for (const auto& c : kCases) {
		CAPTURE(static_cast<int>(c.kind));
		REQUIRE(f.selection.slot() == 0);
		Knob& forA = f.stage.fieldKnobForTest(c.kind, 0);
		REQUIRE(forA.isVisible());
		REQUIRE_FALSE(f.stage.fieldKnobForTest(c.kind, 1).isVisible());

		// The default rests at neutral, which is the field as the terrain authored it.
		REQUIRE(f.bridge.value(static_cast<clap_id>(c.slotA))
		        == doctest::Approx(feathers::kFieldNeutral));

		// Drag DOWN from neutral, so the committed value is unambiguously below where it started —
		// an upward drag from 0.5 would also pass a "it moved" check while proving less.
		forA.processMouseDown(at(26.0f, 60.0f));
		forA.processMouseDrag(at(26.0f, 60.0f + Knob::kDragRange * 0.25f));
		forA.processMouseUp(at(26.0f, 60.0f + Knob::kDragRange * 0.25f));
		f.settleGesture();

		const double moved = f.bridge.value(static_cast<clap_id>(c.slotA));
		CAPTURE(moved);
		CHECK(moved < feathers::kFieldNeutral);
		// AND SLOT B DID NOT MOVE. The hidden knobs are the same objects, kept alive and sized, so
		// "the drag went to the visible one" is a real claim rather than a tautology.
		CHECK(f.bridge.value(static_cast<clap_id>(c.slotB))
		      == doctest::Approx(feathers::kFieldNeutral));
	}
}
TEST_CASE("resonate: the Explore surround shows Resonate's own knobs, live, and V5's are untouched") {
	// The Explore ring's surround modifiers are exposed, and renamed for Resonate. On Resonate
	// each Resonate knob takes its slot — it is what a press at that slot's centre reaches — and the per-sound
	// knob that sat there steps aside; a drag writes the RESONATE param. On V5 nothing of Resonate's shows
	// and the per-sound knobs are live as they always were.
	using RK = ExploreStage::ResonateKnob;
	struct Slot {
		RK knob;
		ParamId param;
	};
	const Slot kSlots[] = {
		{ RK::Width, ParamId::ResonateWidth },         { RK::Bow, ParamId::ResonateBow },
		{ RK::Shimmer, ParamId::ResonateShimmer },     { RK::Stiffness, ParamId::ResonateStiffness },
		{ RK::Coupling, ParamId::ResonateCoupling },   { RK::Decay, ParamId::ResonateDecay },
		{ RK::Pluck, ParamId::ResonatePluck },         { RK::Travel, ParamId::ResonateTravel },
		{ RK::Speed, ParamId::ResonateSpeed }, // in the Warp slot, which Resonate had nothing for
	};

	SUBCASE("on Resonate") {
		ExploreFixture f;
		f.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (const Slot& s : kSlots) {
			CAPTURE(static_cast<int>(s.knob));
			Knob& knob = f.stage.resonateKnobForTest(s.knob);
			CHECK(knob.isVisible());
			CHECK(knob.enabled());
			const visage::Bounds b = f.stage.resonateKnobBounds(s.knob);
			const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
			CHECK(f.stage.frameAtPoint(centre) == &knob);

			// A drag UP writes THIS knob's Resonate param (every one rests below its top).
			const double before = f.bridge.value(static_cast<clap_id>(s.param));
			knob.processMouseDown(at(26.0f, 60.0f));
			knob.processMouseDrag(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
			knob.processMouseUp(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
			f.settleGesture();
			CHECK(f.bridge.value(static_cast<clap_id>(s.param)) > before);
		}
		// The displaced per-sound knobs are hidden.
		CHECK_FALSE(f.stage.widthKnobForTest(0).isVisible());
		CHECK_FALSE(f.stage.fmDepthKnobForTest(0).isVisible());
		CHECK_FALSE(f.stage.smoothnessKnobForTest(0).isVisible());
		CHECK_FALSE(f.stage.warpKnobForTest(0).isVisible()); // its slot is Speed's
		CHECK_FALSE(f.stage.fieldKnobForTest(ExploreStage::FieldKind::Grit, 0).isVisible());
		// The mode box is the bow mode, live, naming the Resonate choice, and writing it.
		CHECK(f.stage.bowModeLive());
		CHECK(std::string(f.stage.bowModeLabel()) == "Laws");
		f.set(ParamId::ResonateBowMode, 2.0);
		f.stage.update();
		CHECK(std::string(f.stage.bowModeLabel()) == "StickSlip");
	}

	SUBCASE("on V5 nothing of Resonate's shows, and the flagship's knobs are live") {
		ExploreFixture f;
		f.set(ParamId::OscType, 2.0 /* V5 */);
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (const Slot& s : kSlots) {
			CAPTURE(static_cast<int>(s.knob));
			CHECK_FALSE(f.stage.resonateKnobForTest(s.knob).isVisible());
		}
		CHECK(f.stage.widthKnobForTest(0).isVisible());
		CHECK(f.stage.widthKnobForTest(0).enabled());
		CHECK(f.stage.fmDepthKnobForTest(0).enabled());
		CHECK(f.stage.fieldKnobForTest(ExploreStage::FieldKind::Quantize, 0).enabled());
		CHECK_FALSE(f.stage.bowModeLive());
		CHECK(f.stage.warpModeLive());
	}
}
TEST_CASE("basic shapes: the right flank is the shape mix, live, and every per-sound control is live exactly where the engine reads it") {
	// Sine / Triangle / Saw / Square take the right flank's top four
	// slots and write the engine-wide params; FM Depth, Warp (and its mode box) and Grit are live, because the
	// bank keeps V5's recipe and morph modifiers for this engine; Width is live (backlog KV); Smoothness is not drawn.
	using SK = ExploreStage::ShapesKnob;
	using FieldKind = ExploreStage::FieldKind;
	struct Slot {
		SK knob;
		ParamId param;
	};
	const Slot kSlots[] = {
		{ SK::Sine, ParamId::ShapesSine },
		{ SK::Triangle, ParamId::ShapesTriangle },
		{ SK::Saw, ParamId::ShapesSaw },
		{ SK::Square, ParamId::ShapesSquare },
	};

	SUBCASE("on Basic Shapes") {
		ExploreFixture f;
		f.set(ParamId::OscType, static_cast<double>(OscType::BasicShapes));
		f.set(ParamId::AEnable, 1.0);
		f.set(ParamId::ShapesSine, 0.5);
		f.stage.update();
		for (const Slot& s : kSlots) {
			CAPTURE(static_cast<int>(s.knob));
			Knob& knob = f.stage.shapesKnobForTest(s.knob);
			CHECK(knob.isVisible());
			CHECK(knob.enabled());
			const visage::Bounds b = f.stage.shapesKnobBounds(s.knob);
			const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
			CHECK(f.stage.frameAtPoint(centre) == &knob);
			const double before = f.bridge.value(static_cast<clap_id>(s.param));
			knob.processMouseDown(at(26.0f, 60.0f));
			knob.processMouseDrag(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
			knob.processMouseUp(at(26.0f, 60.0f - Knob::kDragRange * 0.25f));
			f.settleGesture();
			CHECK(f.bridge.value(static_cast<clap_id>(s.param)) > before);
		}
		// Live: the engine reads them (V5's FM, warp and grit through the recipe the bank keeps).
		CHECK(f.stage.fmDepthKnobForTest(0).isVisible());
		CHECK(f.stage.fmDepthKnobForTest(0).enabled());
		CHECK(f.stage.warpKnobForTest(0).enabled());
		CHECK(f.stage.warpModeLive());
		CHECK(f.stage.fieldKnobForTest(FieldKind::Grit, 0).isVisible());
		CHECK(f.stage.fieldKnobForTest(FieldKind::Grit, 0).enabled());
		// Live since backlog KV: the synth builds two ears for Basic Shapes from the per-sound Width.
		CHECK(f.stage.widthKnobForTest(0).isVisible());
		CHECK(f.stage.widthKnobForTest(0).enabled());
		// Live since backlog KV: the bank operates the mix by the per-sound Smoothness (flagship.h, Basic Shapes' tier).
		CHECK(f.stage.smoothnessKnobForTest(0).isVisible());
		CHECK(f.stage.smoothnessKnobForTest(0).enabled());
		// On the Mix page their slots carry the mix; the page box names the page and stands under the right flank.
		CHECK(f.stage.shapesPageLive());
		CHECK(f.stage.shapesPageForTest() == ExploreStage::ShapesPage::Mix);
		for (const FieldKind k : { FieldKind::Quantize, FieldKind::Sync, FieldKind::Feedback, FieldKind::Points }) {
			CAPTURE(static_cast<int>(k));
			CHECK_FALSE(f.stage.fieldKnobForTest(k, 0).isVisible());
		}
		// THE OPERATORS PAGE (backlog KV): the four operators V5 seats in those slots, live, and the mix hidden.
		f.stage.chooseShapesPage(ExploreStage::ShapesPage::Operators);
		for (const FieldKind k : { FieldKind::Quantize, FieldKind::Sync, FieldKind::Feedback, FieldKind::Points }) {
			CAPTURE(static_cast<int>(k));
			Knob& knob = f.stage.fieldKnobForTest(k, 0);
			CHECK(knob.isVisible());
			CHECK(knob.enabled());
			const visage::Bounds b = knob.bounds();
			CHECK(f.stage.frameAtPoint({ b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f }) == &knob);
		}
		for (const Slot& s : kSlots) {
			CHECK_FALSE(f.stage.shapesKnobForTest(s.knob).isVisible());
		}
		CHECK(f.stage.shapesPageLive());
		f.stage.chooseShapesPage(ExploreStage::ShapesPage::Mix);
		CHECK(f.stage.shapesKnobForTest(kSlots[0].knob).isVisible());
		CHECK_FALSE(f.stage.fieldKnobForTest(FieldKind::Quantize, 0).isVisible());
	}

	SUBCASE("on V5 none of the mix shows and the per-sound flank is back") {
		ExploreFixture f;
		f.set(ParamId::OscType, static_cast<double>(OscType::V5));
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (const Slot& s : kSlots) {
			CAPTURE(static_cast<int>(s.knob));
			CHECK_FALSE(f.stage.shapesKnobForTest(s.knob).isVisible());
		}
		CHECK(f.stage.fieldKnobForTest(FieldKind::Quantize, 0).isVisible());
		CHECK(f.stage.fieldKnobForTest(FieldKind::Quantize, 0).enabled());
		CHECK(f.stage.widthKnobForTest(0).enabled());
	}
}
TEST_CASE("basic shapes: the terrain pictures the FM each place gives, and is one colour without FM") {
	// A Basic Shapes texel is the mix read through that place's FM. At
	// FM depth 0 the place changes nothing about the cycle, so every texel is the same; with FM the interval
	// field moves under the puck and so does the picture.
	const feathers::ShapesMix saw { 0.0f, 0.0f, 1.0f, 0.0f };
	const auto at = [&](uint16_t zone, float fmDepth) {
		feathers::FieldScales scales;
		scales.fmDepth = fmDepth;
		return terrainFeaturesAt(OscType::BasicShapes, SliceOrientationPin {}, zone, 0.37f, 0.0, 0.0, kNoPlaneResidue,
		                         0.0, scales, feathers::WarpMode::Off, feathers::WeaveModel::Ring, {}, saw);
	};
	float lo = 1e9f;
	float hi = -1e9f;
	for (uint16_t zone = 0; zone < 1000; zone += 37) {
		const TerrainFeatures flat = at(zone, 0.0f);
		CHECK(flat.centroid == at(0, 0.0f).centroid);
		CHECK(flat.energy == at(0, 0.0f).energy);
		const float c = at(zone, 0.6f).centroid;
		lo = std::min(lo, c);
		hi = std::max(hi, c);
	}
	MESSAGE("FM'd saw centroid across the zones: " << lo << " .. " << hi);
	CHECK(hi - lo > 0.02f);
	// And the mix is drawn: a square's texel is not a saw's.
	feathers::FieldScales none;
	const TerrainFeatures sq = terrainFeaturesAt(OscType::BasicShapes, SliceOrientationPin {}, 100, 0.0f, 0.0, 0.0,
	                                             kNoPlaneResidue, 0.0, none, feathers::WarpMode::Off,
	                                             feathers::WeaveModel::Ring, {}, feathers::ShapesMix { 0, 0, 0, 1 });
	CHECK(sq.parity > at(100, 0.0f).parity + 0.2f);
}
TEST_CASE("additive: the terrain pictures each place's own spectrum, and is one colour when every sound is the Shape") {
	// THE MINIMAL ARM (plan Task 9b; design §4.4's spectrum-read features are backlog LY): a texel is the place's
	// spectrum (buildPartialSound through its plane, at the Smoothness knob) summed over the lens's fifteen partials at
	// Newman's flat phases, reduced like every engine's cycle. Restated here with a sine per partial; never a table.
	const auto at = [](uint16_t zone, float offset, const AdditivePicture& pic, float smooth = feathers::kFieldNeutral) {
		feathers::FieldScales scales;
		scales.smoothness = smooth;
		return terrainFeaturesAt(OscType::Additive, SliceOrientationPin {}, zone, offset, 0.0, 0.0, kNoPlaneResidue, 0.0,
		                         scales, feathers::WarpMode::Off, feathers::WeaveModel::Ring, {}, {}, pic);
	};
	const auto restated = [](uint16_t zone, float offset, float smooth) {
		const double home = feathers::sliceHome(zone, offset, 0.0);
		const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(0.0, home, {});
		feathers::partials::PartialSpectrum s;
		feathers::buildPartialSound(frame, home, 0.0, 0.0, s, smooth);
		std::array<float, kTerrainCycleSamples> cycle {};
		constexpr int n = kAdditiveLensPartials; // fifteen: below the lens's Nyquist
		for (int i = 0; i < kTerrainCycleSamples; ++i) {
			double sum = 0.0;
			for (int k = 0; k < n; ++k) {
				sum += double(s.envAtRatio[k]) * double(s.fine[k])
				       * std::sin(2.0 * std::numbers::pi * s.ratio[k] * i / double(kTerrainCycleSamples)
				                  + std::numbers::pi * k * k / n);
			}
			cycle[size_t(i)] = static_cast<float>(sum);
		}
		return terrainFeaturesOfCycle(cycle);
	};
	float lo = 1e9f, hi = -1e9f;
	for (uint16_t zone = 0; zone < 1000; zone += 97) {
		for (const float offset : { 0.0f, 1.7f, 4.3f }) {
			CAPTURE(zone);
			CAPTURE(offset);
			const TerrainFeatures got = at(zone, offset, {});
			const TerrainFeatures want = restated(zone, offset, feathers::kFieldNeutral);
			CHECK(got.energy == doctest::Approx(want.energy).epsilon(1e-4));
			CHECK(got.centroid == doctest::Approx(want.centroid).epsilon(1e-4));
			lo = std::min(lo, got.centroid);
			hi = std::max(hi, got.centroid);
		}
	}
	MESSAGE("additive centroid across the places: " << lo << " .. " << hi);
	CHECK(hi - lo > 0.02f); // the map varies with the place
	// THE SMOOTHNESS KNOB reaches the picture as it reaches the sound.
	const TerrainFeatures smooth = at(300, 1.7f, {}, 1.0f);
	const TerrainFeatures want = restated(300, 1.7f, 1.0f);
	CHECK(smooth.centroid == doctest::Approx(want.centroid).epsilon(1e-4));
	// AT SHAPE AMOUNT 1 every place IS the Shape, so the map is one colour, and a square's is not a saw's.
	const AdditivePicture square { feathers::partials::ShapeKind::Square, 1.0f };
	const AdditivePicture saw { feathers::partials::ShapeKind::Saw, 1.0f };
	for (uint16_t zone = 0; zone < 1000; zone += 197) {
		CHECK(at(zone, 2.1f, square).centroid == doctest::Approx(at(0, 0.0f, square).centroid).epsilon(1e-5));
	}
	CHECK(at(100, 0.0f, square).parity > at(100, 0.0f, saw).parity + 0.2f);
}
TEST_CASE("additive: the terrain pictures the selected sound's bias on each place, as the bank puts it on the sound") {
	// A texel at a bias knob of 1 is the place's spectrum with that knob's field(s) SET TO 1 BY HAND (the raw fields and
	// detail through buildSound, not the override seam), summed over the lens's fifteen partials as the minimal arm sums.
	// Neutral is the unbiased picture to the bit.
	namespace partials = feathers::partials;
	const auto at = [](uint16_t zone, float offset, const AdditivePicture& pic) {
		return terrainFeaturesAt(OscType::Additive, SliceOrientationPin {}, zone, offset, 0.0, 0.0, kNoPlaneResidue, 0.0,
		                         {}, feathers::WarpMode::Off, feathers::WeaveModel::Ring, {}, {}, pic);
	};
	using Fields = partials::PartialFields;
	struct Knob1 {
		const char* name;
		float feathers::PartialBias::* knob;
		void (*force)(Fields&);
	};
	const Knob1 kKnobs[] = {
		{ "Rolloff", &feathers::PartialBias::rolloff, [](Fields& f) { f.rolloff = 1.0f; } },
		{ "Formant", &feathers::PartialBias::formant, [](Fields& f) { f.formant1Gain = f.formant2Gain = 1.0f; } },
		{ "Odd/Even", &feathers::PartialBias::oddEven, [](Fields& f) { f.oddEven = 1.0f; } },
		{ "Comb", &feathers::PartialBias::comb, [](Fields& f) { f.combDepth = 1.0f; } },
		{ "Detail", &feathers::PartialBias::detail, [](Fields& f) { f.detail = 1.0f; } },
		{ "Off-grid", &feathers::PartialBias::offGrid, [](Fields& f) { f.offGrid = 1.0f; } },
	};
	const auto restated = [](uint16_t zone, float offset, void (*force)(Fields&)) {
		const double home = feathers::sliceHome(zone, offset, 0.0);
		const auto frame = feathers::sliceFrameFor<feathers::kPartialTriangles>(0.0, home, {});
		Fields fields = feathers::readPartialFields(frame, home, 0.0, 0.0);
		force(fields);
		std::array<float, partials::kMaxPartials> detail {};
		feathers::readPartialDetail(frame, home, 0.0, 0.0, detail);
		partials::PartialSpectrum s;
		partials::buildSound(fields, detail, s);
		std::array<float, kTerrainCycleSamples> cycle {};
		constexpr int n = kAdditiveLensPartials;
		for (int i = 0; i < kTerrainCycleSamples; ++i) {
			double sum = 0.0;
			for (int k = 0; k < n; ++k) {
				sum += double(s.envAtRatio[k]) * double(s.fine[k])
				       * std::sin(2.0 * std::numbers::pi * s.ratio[k] * i / double(kTerrainCycleSamples)
				                  + std::numbers::pi * k * k / n);
			}
			cycle[size_t(i)] = static_cast<float>(sum);
		}
		return terrainFeaturesOfCycle(cycle);
	};
	for (const Knob1& k : kKnobs) {
		CAPTURE(k.name);
		int moved = 0;
		for (uint16_t zone = 0; zone < 1000; zone += 197) {
			for (const float offset : { 0.0f, 2.9f, 6.3f }) {
				CAPTURE(zone);
				CAPTURE(offset);
				AdditivePicture pic;
				pic.bias.*(k.knob) = 1.0f;
				const TerrainFeatures got = at(zone, offset, pic);
				const TerrainFeatures want = restated(zone, offset, k.force);
				CHECK(got.energy == doctest::Approx(want.energy).epsilon(1e-4));
				CHECK(got.centroid == doctest::Approx(want.centroid).epsilon(1e-4));
				CHECK(got.parity == doctest::Approx(want.parity).epsilon(1e-4));
				const TerrainFeatures plain = at(zone, offset, {});
				moved += std::abs(got.centroid - plain.centroid) > 1e-3f || std::abs(got.parity - plain.parity) > 1e-3f
				             ? 1
				             : 0;
				// Neutral is the unbiased picture, to the bit.
				AdditivePicture neutral;
				neutral.bias = feathers::PartialBias {};
				const TerrainFeatures same = at(zone, offset, neutral);
				CHECK(same.centroid == plain.centroid);
				CHECK(same.energy == plain.energy);
			}
		}
		CHECK(moved > 0); // the knob reaches the picture somewhere, or the restatement proved nothing
	}
}
TEST_CASE("resonate: the Model box is live only on Resonate, reachable under the right flank, and names what it writes") {
	ExploreFixture f;
	f.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
	f.set(ParamId::AEnable, 1.0);
	f.stage.update();
	// Width is V5-only (the synth's string is mono; the stereo dual-tap is backlog) and so are FM Depth and
	// Smoothness: greyed on Resonate.
	CHECK_FALSE(f.stage.widthKnobForTest(0).enabled());
	CHECK_FALSE(f.stage.fmDepthKnobForTest(0).enabled());
	using FieldKind = ExploreStage::FieldKind;

	// Under the last knob, on screen, clear of every knob, and answered by the STAGE at its centre.
	const visage::Bounds box = f.stage.modelBoxBounds();
	const visage::Bounds last = f.stage.fieldKindBounds(FieldKind::Grit);
	CHECK(box.width() > 0.0f);
	CHECK(box.y() >= last.bottom());
	CHECK(box.x() == doctest::Approx(last.x()));
	CHECK(box.y() + box.height() <= f.stage.height());
	CHECK(f.stage.fieldKindBounds(FieldKind::Quantize).y() >= 0.0f);
	const visage::Point centre { box.x() + box.width() * 0.5f, box.y() + box.height() * 0.5f };
	CHECK(dynamic_cast<Knob*>(f.stage.frameAtPoint(centre)) == nullptr);

	// It reads the engine's model, and choosing writes one bracketed edit to the Model param.
	CHECK(f.stage.modelBoxLive());
	CHECK(std::string(f.stage.modelLabel()) == "Ring");
	f.set(ParamId::ResonateModel, 3.0);
	f.stage.update();
	CHECK(std::string(f.stage.modelLabel()) == "Membrane");
	f.bridge.drain([](const UiParamEvent&) {});
	f.stage.chooseModel(2);
	std::vector<UiParamEvent> posted;
	f.bridge.drain([&](const UiParamEvent& e) { posted.push_back(e); });
	REQUIRE(posted.size() == 3);
	CHECK(posted[1].id == static_cast<clap_id>(ParamId::ResonateModel));
	CHECK(posted[1].value == 2.0);

	// THE HONESTY HALF: on every other engine the string is not what plays, so the box is dead, and NOT DRAWN AT
	// ALL (it was greyed but always shown). The draw is asked, not the predicate it should obey.
	visage::Canvas canvas;
	CHECK(f.stage.drawModelBoxForTest(canvas));
	for (const OscType other : { OscType::Morph, OscType::Stair, OscType::V5 }) {
		CAPTURE(static_cast<int>(other));
		f.set(ParamId::OscType, static_cast<double>(other));
		f.stage.update();
		CHECK_FALSE(f.stage.modelBoxLive());
		CHECK_FALSE(f.stage.drawModelBoxForTest(canvas));
	}
	// And back on Resonate it is there again.
	f.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
	f.stage.update();
	CHECK(f.stage.drawModelBoxForTest(canvas));
}
TEST_CASE("Grit stays live on a plain Morph patch where the other four are not drawn") {
	// THE HONESTY RULE READ IN THE DIRECTION THAT GETS FORGOTTEN. Four of the five scale FLAGSHIP
	// fields and buy nothing off V5, exactly as FM Depth and Smoothness do. Grit is not one of them:
	// it scales the morph TABLE's ampNoise, which PreparedCharacter::ampNoise() hands back for
	// OscEngine::Morph — and OscType::Morph maps to that engine. So greying Grit on a Morph patch
	// would draw a working control as a dead one.
	//
	// AND THE FOUR ARE NOT DRAWN off V5 rather than greyed: only switching the engine
	// could ungrey them. A PARKED sound still greys them, because its switch is on this face.
	//
	// The engine half of this claim is pinned in test_flagship.cpp ("Grit scales the morph table's
	// amp noise, and reaches plain Morph rather than only V5"); this is the control agreeing with it.
	using FieldKind = ExploreStage::FieldKind;
	const FieldKind flagshipOnly[] = { FieldKind::Quantize, FieldKind::Sync, FieldKind::Feedback,
		                              FieldKind::Points };

	SUBCASE("on V5 all five are live") {
		ExploreFixture f;
		f.set(ParamId::OscType, 2.0 /* V5 */);
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (const FieldKind kind : flagshipOnly) {
			CAPTURE(static_cast<int>(kind));
			CHECK(f.stage.fieldKnobForTest(kind, 0).isVisible());
			CHECK(f.stage.fieldKnobForTest(kind, 0).enabled());
		}
		CHECK(f.stage.fieldKnobForTest(FieldKind::Grit, 0).isVisible());
		CHECK(f.stage.fieldKnobForTest(FieldKind::Grit, 0).enabled());
	}

	SUBCASE("on Morph only Grit is") {
		ExploreFixture f;
		f.set(ParamId::OscType, 0.0 /* Morph */);
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (const FieldKind kind : flagshipOnly) {
			CAPTURE(static_cast<int>(kind));
			CHECK_FALSE(f.stage.fieldKnobForTest(kind, 0).isVisible());
			CHECK_FALSE(f.stage.fieldKnobForTest(kind, 0).enabled());
		}
		CHECK(f.stage.fieldKnobForTest(FieldKind::Grit, 0).isVisible());
		CHECK(f.stage.fieldKnobForTest(FieldKind::Grit, 0).enabled());
	}

	SUBCASE("on Stair none of them are — ampNoise() returns zero and there is no flagship tier") {
		ExploreFixture f;
		f.set(ParamId::OscType, 1.0 /* Stair */);
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (const FieldKind kind : flagshipOnly) {
			CAPTURE(static_cast<int>(kind));
			CHECK_FALSE(f.stage.fieldKnobForTest(kind, 0).isVisible());
			CHECK_FALSE(f.stage.fieldKnobForTest(kind, 0).enabled());
		}
		CHECK_FALSE(f.stage.fieldKnobForTest(FieldKind::Grit, 0).isVisible());
		CHECK_FALSE(f.stage.fieldKnobForTest(FieldKind::Grit, 0).enabled());
	}

	SUBCASE("on Resonate none of them are — the string reads no table and has no flagship tier") {
		// The voices scan the synth's ONE string (OscEngine::Weave), whose laws come from buildWeaveFor;
		// no field modifier reaches it, Grit included (PreparedCharacter::ampNoise() gates on Morph).
		ExploreFixture f;
		f.set(ParamId::OscType, static_cast<double>(OscType::Resonate));
		f.set(ParamId::AEnable, 1.0);
		f.stage.update();
		for (const FieldKind kind : flagshipOnly) {
			CAPTURE(static_cast<int>(kind));
			CHECK_FALSE(f.stage.fieldKnobForTest(kind, 0).isVisible());
			CHECK_FALSE(f.stage.fieldKnobForTest(kind, 0).enabled());
		}
		CHECK_FALSE(f.stage.fieldKnobForTest(FieldKind::Grit, 0).isVisible());
		CHECK_FALSE(f.stage.fieldKnobForTest(FieldKind::Grit, 0).enabled());
	}

	SUBCASE("and a parked sound greys all five whatever the engine") {
		// A sound that is not in the constellation is not in the character, so its field modifiers
		// buy nothing until the switch comes back — the same case Width and FM Depth answer.
		ExploreFixture f;
		f.set(ParamId::OscType, 2.0 /* V5 */);
		f.set(ParamId::AEnable, 0.0);
		f.stage.update();
		for (const FieldKind kind : flagshipOnly) {
			CAPTURE(static_cast<int>(kind));
			CHECK(f.stage.fieldKnobForTest(kind, 0).isVisible());
			CHECK_FALSE(f.stage.fieldKnobForTest(kind, 0).enabled());
		}
		CHECK(f.stage.fieldKnobForTest(FieldKind::Grit, 0).isVisible());
		CHECK_FALSE(f.stage.fieldKnobForTest(FieldKind::Grit, 0).enabled());
	}
}
TEST_CASE("explore: on every engine a control only another engine could wake is not drawn, and one this face could wake is greyed") {
	// Explore prefers not drawn over greyed for engine settings, since there is no way to ungrey them
	// without switching engine. The test is what it takes to make a control live: if
	// only OscType would, the control is NOT ON THE FACE (and a pointer at it reaches nothing); if something on
	// this face would — the selected sound's own switch — it stays drawn and greyed.
	//
	// THE TABLE IS WRITTEN HERE, not derived from the stage's predicates: per engine, which controls are on the
	// face, and of those, which are live while the sound plays. Its rows come from reading the engine, one
	// engine at a time — flagship.cpp is the only reader of Width, Smoothness and the four flagship fields (V5);
	// Basic Shapes keeps V5's FM, Warp, Width (two ears, backlog KV) and morph modifiers (CharacterBank::buildOne)
	// and takes the top four field slots for its mix; Grit is the morph table's ampNoise (Morph, V5, Basic Shapes); Resonate's surround and
	// Model are the one string's; the bow mode is Resonate's and the warp mode V5's and Basic Shapes'.
	enum Ctl : size_t {
		Width, FmDepth, Warp, Smoothness, Quantize, Sync, Feedback, Points, Grit, ModeBox, ModelBox,
		Radius, Theta, ResonateSurround, ShapesMix, ShapeAmount, Bias, StringScalers, PluckSurround,
		kCtlCount
	};
	const char* const kNames[kCtlCount] = { "Width", "FM Depth", "Warp", "Smoothness", "Quantize", "Sync",
		                                    "Feedback", "Points", "Grit", "mode box", "Model box", "Radius",
		                                    "Theta", "Resonate surround", "Shapes mix", "Shape Amount",
		                                    "Additive bias", "Pluck string scalers", "Pluck surround" };
	struct Row {
		OscType engine;
		const char* face; // one char per Ctl: 'L' drawn and live, '-' not drawn
	};
	// ADDITIVE (2026-10-10, design §4.1) reads each sound's Smoothness and Shape Amount (buildPartialsFor) and builds two
	// ears from Width (Synth's stereo predicate); its voice runs no FM, Warp, Grit or flagship field (design C4), and its
	// mode box is the Additive Shape, and its six per-sound bias knobs (Rolloff in FM Depth's slot, the other five in the
	// right flank's) are read by the bank on a Partials build alone (character_params.h).
	// PLUCK (2026-10-31, KS design §4.2) reads Smoothness (its pluck's tone is the Additive spectrum) and its five string
	// scalers (the bank's String build), and its engine-wide knobs (the String page's Width, Decay and Release here; the Pluck
	// page's four are pinned by test_gui_widgets_explore's Pluck case); its left box is the page box.
	// DRUM, in Kit mode with pad A (a Kick) selected: the surround holds the seven kit trims and nothing else, so none of this
	// table's controls is drawn (a pad's own scalers are per layer on the pad editor, and a kit reads no drift or orbit, so no
	// Radius or Theta).
	//                                    W  F  Wp S  Q  Sy Fb P  G  Mb Mo R  T  Rs Sh SA Bi St Pk
	const Row kTable[] = {
		{ OscType::Morph,       "--------L--LL------" },
		{ OscType::Stair,       "-----------LL------" },
		{ OscType::V5,          "LLLLLLLLLL-LL------" },
		{ OscType::Resonate,    "---------LLLLL-----" },
		{ OscType::BasicShapes, "LLLL----LL-LL-L----" }, // Q/Sy/Fb/P on its Operators page (backlog KV)
		{ OscType::Additive,    "L--L-----L-LL--LL--" },
		{ OscType::Pluck,       "---L-----L-LL----LL" },
	};
	REQUIRE(std::size(kTable) == static_cast<size_t>(OscType::kCount));
	// PER-SOUND controls grey when the selected sound is parked (its switch is on this face); the engine-wide
	// ones (the boxes, Resonate's surround, the shapes mix) and the polar pair do not.
	const bool kPerSoundGreys[kCtlCount] = { true, true, true, true, true, true, true, true, true,
		                                     false, false, false, false, false, false, true, true, true, false };

	using FieldKind = ExploreStage::FieldKind;
	using FlagshipKind = ExploreStage::FlagshipKind;
	using RK = ExploreStage::ResonateKnob;
	using SK = ExploreStage::ShapesKnob;
	visage::Canvas canvas;

	for (const Row& row : kTable) {
		REQUIRE(std::strlen(row.face) == kCtlCount);
		for (const double enable : { 1.0, 0.0 }) {
			CAPTURE(static_cast<int>(row.engine));
			CAPTURE(enable);
			ExploreFixture f;
			f.set(ParamId::OscType, static_cast<double>(row.engine));
			f.set(ParamId::AEnable, enable);
			f.stage.update();
			const size_t slot = f.selection.slot();
			REQUIRE(slot == 0);

			// The knobs each control is; a group (surround, mix) is every one of its knobs.
			const auto knobsOf = [&](Ctl c) -> std::vector<Knob*> {
				switch (c) {
				case Width: return { &f.stage.widthKnobForTest(slot) };
				case FmDepth: return { &f.stage.fmDepthKnobForTest(slot) };
				case Warp: return { &f.stage.warpKnobForTest(slot) };
				case Smoothness: return { &f.stage.smoothnessKnobForTest(slot) };
				case Quantize: return { &f.stage.fieldKnobForTest(FieldKind::Quantize, slot) };
				case Sync: return { &f.stage.fieldKnobForTest(FieldKind::Sync, slot) };
				case Feedback: return { &f.stage.fieldKnobForTest(FieldKind::Feedback, slot) };
				case Points: return { &f.stage.fieldKnobForTest(FieldKind::Points, slot) };
				case Grit: return { &f.stage.fieldKnobForTest(FieldKind::Grit, slot) };
				case Radius: return { &f.stage.radiusKnobForTest(slot) };
				case Theta: return { &f.stage.thetaKnobForTest(slot) };
				case ResonateSurround: {
					std::vector<Knob*> all;
					for (size_t i = 0; i < ExploreStage::kResonateKnobCount; ++i) {
						all.push_back(&f.stage.resonateKnobForTest(static_cast<RK>(i)));
					}
					return all;
				}
				case ShapesMix: {
					std::vector<Knob*> all;
					for (size_t i = 0; i < ExploreStage::kShapesKnobCount; ++i) {
						all.push_back(&f.stage.shapesKnobForTest(static_cast<SK>(i)));
					}
					return all;
				}
				case ShapeAmount: return { &f.stage.shapeAmountKnobForTest(slot) };
				case Bias: {
					std::vector<Knob*> all;
					for (size_t i = 0; i < ExploreStage::kAdditiveBiasCount; ++i) {
						all.push_back(&f.stage.additiveBiasKnobForTest(static_cast<ExploreStage::AdditiveBiasKind>(i), slot));
					}
					return all;
				}
				case StringScalers: {
					std::vector<Knob*> all;
					for (size_t i = 0; i < ExploreStage::kStringKindCount; ++i) {
						all.push_back(&f.stage.stringKnobForTest(static_cast<ExploreStage::StringKind>(i), slot));
					}
					return all;
				}
				case PluckSurround: {
					using PK = ExploreStage::PluckKnob;
					return { &f.stage.pluckKnobForTest(PK::Width), &f.stage.pluckKnobForTest(PK::Decay),
						     &f.stage.pluckKnobForTest(PK::Release) };
				}
				default: return {};
				}
			};

			for (size_t c = 0; c < kCtlCount; ++c) {
				const std::string control = kNames[c];
				CAPTURE(control);
				const bool drawn = row.face[c] == 'L';
				const bool live = drawn && (enable != 0.0 || !kPerSoundGreys[c]);
				if (c == ModeBox) {
					// Asked of the draw itself, and of the press the stage's hit test answers.
					CHECK(f.stage.drawWarpModeBoxForTest(canvas) == drawn);
					CHECK((f.stage.warpModeLive() || f.stage.bowModeLive() || f.stage.additiveShapeLive()
					       || f.stage.pluckPageLive())
					      == drawn);
					continue;
				}
				if (c == ModelBox) {
					// The right box slot holds Basic Shapes' page box (backlog KV), which draws there in the model's place.
					CHECK(f.stage.drawModelBoxForTest(canvas) == (drawn || f.stage.shapesPageLive()));
					CHECK(f.stage.shapesPageLive() == (row.engine == OscType::BasicShapes));
					CHECK(f.stage.modelBoxLive() == drawn);
					continue;
				}
				for (Knob* knob : knobsOf(static_cast<Ctl>(c))) {
					CHECK(knob->isVisible() == drawn);
					// Live means enabled; a control not drawn is dead too, not merely undrawn.
					CHECK(knob->enabled() == live);
					// A HIDDEN KNOB'S RECTANGLE DOES NOT REACH IT: whatever answers there, it is not this knob.
					const visage::Bounds b = knob->bounds();
					const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
					if (!drawn) {
						CHECK(f.stage.frameAtPoint(centre) != knob);
					}
					else {
						CHECK(f.stage.frameAtPoint(centre) == knob);
					}
				}
			}

			// AND A PRESS WHERE A HIDDEN CONTROL WOULD BE DOES NOTHING. Every per-sound slot whose knob is not drawn
			// and whose rectangle nothing else on this engine claims is answered by the STAGE — and the stage,
			// pressed, dragged and released there, writes no parameter and opens no menu. The same for the two
			// boxes when they are not drawn.
			const auto popups = [&] {
				int n = 0;
				for (visage::Frame* child : f.stage.children()) {
					n += dynamic_cast<visage::PopupMenuFrame*>(child) != nullptr ? 1 : 0;
				}
				return n;
			};
			const auto pressAt = [&](visage::Point p) {
				f.bridge.drain([](const UiParamEvent&) {});
				const int before = popups();
				f.stage.processMouseDown(at(p.x, p.y));
				f.stage.processMouseDrag(at(p.x, p.y - 40.0f));
				f.stage.processMouseUp(at(p.x, p.y - 40.0f));
				CHECK_FALSE(f.bridge.hasPendingWork());
				CHECK(popups() == before);
			};
			std::vector<visage::Bounds> hiddenSpots;
			const FlagshipKind left[] = { FlagshipKind::Width, FlagshipKind::FmDepth, FlagshipKind::Warp,
				                          FlagshipKind::Smoothness };
			for (size_t i = 0; i < std::size(left); ++i) {
				if (row.face[Width + i] == '-') {
					hiddenSpots.push_back(f.stage.flagshipKindBounds(left[i]));
				}
			}
			for (size_t k = 0; k < ExploreStage::kFieldKindCount; ++k) {
				if (row.face[Quantize + k] == '-') {
					hiddenSpots.push_back(f.stage.fieldKindBounds(static_cast<FieldKind>(k)));
				}
			}
			if (row.face[ModeBox] == '-') {
				hiddenSpots.push_back(f.stage.warpModeBounds());
			}
			if (row.face[ModelBox] == '-' && !f.stage.shapesPageLive()) { // Basic Shapes' page box stands there (KV)
				hiddenSpots.push_back(f.stage.modelBoxBounds());
			}
			for (const visage::Bounds& b : hiddenSpots) {
				const visage::Point centre { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f };
				if (f.stage.frameAtPoint(centre) != &f.stage) {
					continue; // another engine's control holds this slot here (Resonate's surround, the mix)
				}
				CAPTURE(centre.x);
				CAPTURE(centre.y);
				pressAt(centre);
			}
			// AND THE PRESS IS CAPABLE OF SAYING SOMETHING: on an engine where the mode box IS drawn, the same
			// press at its centre opens its menu — so the silence above is the gate, not a deaf instrument.
			if (row.face[ModeBox] == 'L') {
				const visage::Bounds b = f.stage.warpModeBounds();
				const int before = popups();
				f.stage.processMouseDown(at(b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f));
				f.stage.processMouseUp(at(b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f));
				CHECK(popups() == before + 1);
			}
		}
	}
}

