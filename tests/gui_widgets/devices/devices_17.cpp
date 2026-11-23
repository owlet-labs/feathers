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

// PIECE 17 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("row lock: the lane draws with a row locked and unlocked, Melody and Drum, and FEATHERS_ROWLOCK_SHOT saves the picture for looking") {
	using namespace lane_check;
	namespace genes = feathers::mod::arp::genes;
	constexpr int kW = 520;
	constexpr int kH = 520;
	visage::Canvas canvas;
	markedWindowless(canvas, kW, kH);
	canvas.setColor(ui::kSurface);
	canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
	float top = 4.0f;
	std::deque<visage::Region> regions; // alive until the capture
	for (const genes::Mode mode : { genes::Mode::Melody, genes::Mode::Drum }) {
		Window w;
		w.f.deviceHost.setField(w.id, pt::at(0, pt::Field::Mode), static_cast<float>(mode));
		(void)w.window->updateContent();
		ui::PatternLane& lane = w.lane();
		(void)lane.poll();
		const pn::PatternCycle c = lane.cycleForTest();
		int id = 2;
		if (mode == genes::Mode::Melody) {
			size_t k = 0;
			while (k < c.length && c.steps[k].kind != pn::PatternStep::Kind::Note) {
				++k;
			}
			id = c.keyAt(k, 0) - c.rootKey;
		}
		for (const bool locked : { false, true }) {
			if (locked) {
				REQUIRE(lane.setRowLocked(id, true));
			}
			lane.setBounds(0.0f, 0.0f, static_cast<float>(kW) - 8.0f, 120.0f);
			regions.emplace_back();
			visage::Region& region = regions.back();
			region.setBounds(4, static_cast<int>(top), kW - 8, 120);
			canvas.addRegion(&region);
			canvas.beginRegion(&region);
			lane.draw(canvas);
			canvas.endRegion();
			top += 128.0f;
			(void)lane.poll();
		}
		CHECK(top > 0.0f);
	}
	const visage::Screenshot shot = takeMarkedCapture(canvas, kW, kH);
	REQUIRE(shot.width() == kW);
	if (const char* path = std::getenv("FEATHERS_ROWLOCK_SHOT")) {
		shot.save(path);
	}
}

namespace {
bool allVisible(const std::vector<visage::Frame*>& frames, bool want) {
	for (const visage::Frame* f : frames) {
		if (f == nullptr || f->isVisible() != want) {
			return false;
		}
	}
	return !frames.empty();
}
} // namespace

TEST_CASE("ERODER's card and window draw only the running engine's controls, and trade them when the engine changes") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)d.card->updateContent();
	(void)panel->updateContent();

	// THE DECLARED DEFAULT IS DUST.
	REQUIRE(eroField(d, ero::Param::Mode) == static_cast<float>(ero::Mode::Dust));
	CHECK(allVisible(d.card->dustWidgetsForTest(), true));
	CHECK(allVisible(d.card->shakerWidgetsForTest(), true));
	CHECK(allVisible(d.card->combWidgetsForTest(), false));
	CHECK(allVisible(d.card->rustWidgetsForTest(), false));
	CHECK(allVisible(panel->dustWidgetsForTest(), true));
	CHECK(allVisible(panel->combWidgetsForTest(), false));
	CHECK(allVisible(panel->rustWidgetsForTest(), false));
	CHECK(d.card->plotForTest()->showsDustForTest());
	CHECK(d.card->mixForTest()->isVisible());
	const float mixX = d.card->mixForTest()->x();

	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	CHECK(d.card->updateContent());
	CHECK(panel->updateContent());
	CHECK(allVisible(d.card->dustWidgetsForTest(), false));
	CHECK(allVisible(d.card->shakerWidgetsForTest(), false));
	CHECK(allVisible(d.card->rustWidgetsForTest(), false));
	CHECK(allVisible(d.card->combWidgetsForTest(), true));
	CHECK(d.card->trackForTest()->isVisible()); // Track is every engine's
	CHECK(allVisible(panel->dustWidgetsForTest(), false));
	CHECK(allVisible(panel->rustWidgetsForTest(), false));
	CHECK(allVisible(panel->combWidgetsForTest(), true));
	CHECK_FALSE(d.card->plotForTest()->showsDustForTest());
	CHECK_FALSE(panel->plotForTest()->showsDustForTest());
	// MIX IS BOTH ENGINES', and does not move on the card when the engine does.
	CHECK(d.card->mixForTest()->isVisible());
	CHECK(d.card->mixForTest()->x() == mixX);

	// RUST: the band's picture and the shaker's controls, its own Bias where Dust has Amount, nothing of the Comb's.
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Rust));
	CHECK(d.card->updateContent());
	CHECK(panel->updateContent());
	CHECK(allVisible(d.card->rustWidgetsForTest(), true));
	CHECK(allVisible(d.card->shakerWidgetsForTest(), true));
	CHECK(allVisible(d.card->dustWidgetsForTest(), false));
	CHECK(allVisible(d.card->combWidgetsForTest(), false));
	CHECK(allVisible(panel->rustWidgetsForTest(), true));
	CHECK(allVisible(panel->dustWidgetsForTest(), false));
	CHECK(allVisible(panel->combWidgetsForTest(), false));
	CHECK(d.card->plotForTest()->showsDustForTest());
	CHECK(d.card->plotForTest()->showsRustForTest());
	CHECK(d.card->rustWidgetsForTest()[0]->x() == d.card->dustWidgetsForTest()[0]->x());
	CHECK(d.card->mixForTest()->x() == mixX);

	// NOTHING OF ANY ENGINE OVERLAPS ITS NEIGHBOUR OR LEAVES THE CARD OR THE WINDOW, in any state.
	const auto within = [](const std::vector<visage::Frame*>& shown, float width, float height) {
		for (size_t i = 0; i < shown.size(); ++i) {
			CAPTURE(i);
			CHECK(shown[i]->width() > 20.0f);
			CHECK(shown[i]->x() >= 0.0f);
			CHECK(shown[i]->right() <= width);
			CHECK(shown[i]->bottom() <= height);
			for (size_t j = i + 1; j < shown.size(); ++j) {
				CAPTURE(j);
				const bool apart = shown[i]->right() <= shown[j]->x() || shown[j]->right() <= shown[i]->x()
				                   || shown[i]->bottom() <= shown[j]->y() || shown[j]->bottom() <= shown[i]->y();
				CHECK(apart);
			}
		}
	};
	for (const float mode : { 0.0f, 1.0f, 2.0f }) {
		CAPTURE(mode);
		d.set(eroAt(ero::Param::Mode), mode);
		(void)d.card->updateContent();
		(void)panel->updateContent();
		// THE WINDOW: every child it shows, whatever it is.
		std::vector<visage::Frame*> inWindow;
		for (visage::Frame* child : panel->children()) {
			if (child->isVisible()) {
				inWindow.push_back(child);
			}
		}
		REQUIRE(inWindow.size() >= 10);
		within(inWindow, panel->width(), panel->height());
		auto shown = mode == 1.0f ? d.card->combWidgetsForTest()
		                          : mode == 0.0f ? d.card->dustWidgetsForTest() : d.card->rustWidgetsForTest();
		if (mode != 1.0f) {
			for (visage::Frame* f : d.card->shakerWidgetsForTest()) {
				shown.push_back(f);
			}
		}
		shown.push_back(d.card->drivePairForTest());
		shown.push_back(d.card->emphasisForTest());
		shown.push_back(d.card->trackForTest());
		if (mode != 1.0f) {
			shown.push_back(d.card->resonanceForTest());
		}
		shown.push_back(d.card->mixForTest());
		CHECK(allVisible(shown, true));
		within(shown, d.card->width(), ui::EroderCard::openHeight());
	}
}
TEST_CASE("ERODER's plot draws Dust's band from the device's own mapping: the handle at Freq and Amount, the skirt the engine's filter") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	d.set(eroAt(ero::Param::Freq), 10.0f);    // 1024 Hz
	d.set(eroAt(ero::Param::Amount), -12.0f); // 244 microseconds
	d.set(eroAt(ero::Param::Width), 0.25f);
	(void)d.card->updateContent();
	ui::EroderPlot* plot = d.card->plotForTest();
	REQUIRE(plot != nullptr);
	const double rate = d.f.deviceHost.sampleRate();

	CHECK(plot->bandForTest().freqHz == doctest::Approx(1024.0).epsilon(1e-9));
	CHECK(plot->bandForTest().amountSeconds == doctest::Approx(std::exp2(-12.0)).epsilon(1e-9));
	CHECK(plot->handleXForTest() == doctest::Approx(plot->hzToXForTest(1024.0)));
	// THE AMOUNT'S HEIGHT: the foot is no travel, the top the most, even in octaves of time between.
	const float foot = plot->plotFootForTest();
	const float top = plot->plotTopForTest();
	REQUIRE(foot > top + 40.0f);
	const double t = (-12.0 - ero::kDustAmountLog2Min) / (ero::kDustAmountLog2Max - ero::kDustAmountLog2Min);
	CHECK(plot->handleYForTest() == doctest::Approx(foot - static_cast<float>(t) * (foot - top)).epsilon(1e-5));
	CHECK(plot->amountToYForTest(ero::kDustAmountLog2Min) == doctest::Approx(foot));
	CHECK(plot->amountToYForTest(ero::kDustAmountLog2Max) == doctest::Approx(top));

	// THE SKIRT, RESTATED HERE: two trapezoidal bandpasses in series are their analogue prototype at the warped
	// frequency, so the level is twice 20 log10 of one's, with Q even in octaves between narrow and wide.
	const double q = ero::kDustQNarrow * std::pow(ero::kDustQWide / ero::kDustQNarrow, 0.25);
	const auto want = [&](double hz) {
		const double x = std::tan(std::numbers::pi * hz / rate) / std::tan(std::numbers::pi * 1024.0 / rate);
		const double one = (x / q) * (x / q) / ((1.0 - x * x) * (1.0 - x * x) + (x / q) * (x / q));
		return 20.0 * std::log10(one);
	};
	CHECK(plot->bandDbForTest(1024.0) == doctest::Approx(0.0).epsilon(1e-9));
	for (const double hz : { 100.0, 512.0, 900.0, 1200.0, 2048.0, 9000.0 }) {
		CAPTURE(hz);
		CHECK(plot->bandDbForTest(hz) == doctest::Approx(want(hz)).epsilon(1e-6));
	}
	// NON-VACUITY: it is a band — an octave off is tens of dB down at this width.
	CHECK(plot->bandDbForTest(2048.0) < -20.0);

	// TRACK: the band follows the published note, an octave up for twelve semitones.
	d.set(eroAt(ero::Param::Track), 1.0f);
	auto frame = eroFrame(0.0f, 0.0f, -160.0f, -160.0f);
	frame[static_cast<size_t>(ero::Reading::PitchSemitones)] = 72.0f;
	d.publish(frame);
	d.f.bridge.refresh();
	(void)d.card->updateContent();
	CHECK(plot->bandForTest().freqHz == doctest::Approx(2048.0).epsilon(1e-6));
}
TEST_CASE("ERODER's plot handle writes Freq across and Amount up, the wheel Width; the Comb's picture writes nothing") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	d.card->setFocused(true); // ON A CARD THE WHEEL ACTS ONLY WHEN IT IS FOCUSED
	d.set(eroAt(ero::Param::Freq), 10.0f);
	d.set(eroAt(ero::Param::Amount), -13.0f);
	d.set(eroAt(ero::Param::Width), 0.5f);
	(void)d.card->updateContent();
	ui::EroderPlot* plot = d.card->plotForTest();
	const float y0 = 0.5f * (plot->plotTopForTest() + plot->plotFootForTest());
	const float x0 = plot->handleXForTest();

	// ACROSS: the pointer's hertz, as log2, and the Amount untouched inside the dead zone.
	plot->onMouseDown().callback(at(x0, y0));
	for (int step = 1; step <= 10; ++step) {
		const float x = x0 + 3.0f * static_cast<float>(step);
		plot->onMouseDrag().callback(at(x, y0 + ui::EroderPlot::kAmountDeadZone));
		CHECK(eroField(d, ero::Param::Freq) == doctest::Approx(std::log2(plot->xToHzForTest(x))).epsilon(1e-5));
		CHECK(eroField(d, ero::Param::Amount) == -13.0f);
	}
	plot->onMouseUp().callback(at(x0 + 30.0f, y0));
	CHECK(eroField(d, ero::Param::Freq) > 10.2f); // non-vacuity: it moved

	// UP: relative to the press, past the dead zone, the whole range to the plot's height.
	const float span = plot->plotFootForTest() - plot->plotTopForTest();
	const double perPixel = (ero::kDustAmountLog2Max - ero::kDustAmountLog2Min) / static_cast<double>(span);
	const float x1 = plot->handleXForTest();
	plot->onMouseDown().callback(at(x1, y0));
	plot->onMouseDrag().callback(at(x1, y0 - ui::EroderPlot::kAmountDeadZone - 20.0f));
	CHECK(eroField(d, ero::Param::Amount) == doctest::Approx(-13.0 + 20.0 * perPixel).epsilon(1e-5));
	plot->onMouseDrag().callback(at(x1, y0 + ui::EroderPlot::kAmountDeadZone + 10.0f));
	CHECK(eroField(d, ero::Param::Amount) == doctest::Approx(-13.0 - 10.0 * perPixel).epsilon(1e-5));
	plot->onMouseUp().callback(at(x1, y0));

	// THE WHEEL: one notch of Width each way, held inside 0..1.
	visage::MouseEvent wheel = at(x1, y0);
	wheel.wheel_delta_y = 1.0f;
	CHECK(plot->onMouseWheel().callback(wheel));
	CHECK(eroField(d, ero::Param::Width) == doctest::Approx(0.5 + ui::EroderPlot::kWidthPerNotch));
	wheel.wheel_delta_y = -1.0f;
	(void)plot->onMouseWheel().callback(wheel);
	(void)plot->onMouseWheel().callback(wheel);
	CHECK(eroField(d, ero::Param::Width) == doctest::Approx(0.5 - ui::EroderPlot::kWidthPerNotch));
	d.set(eroAt(ero::Param::Width), 0.99f);
	wheel.wheel_delta_y = 1.0f;
	(void)plot->onMouseWheel().callback(wheel);
	CHECK(eroField(d, ero::Param::Width) == 1.0f);

	// THE COMB: nothing on its picture is a control.
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	(void)d.card->updateContent();
	const float freqBefore = eroField(d, ero::Param::Freq);
	const float amountBefore = eroField(d, ero::Param::Amount);
	const float widthBefore = eroField(d, ero::Param::Width);
	plot->onMouseDown().callback(at(20.0f, y0));
	plot->onMouseDrag().callback(at(60.0f, y0 - 30.0f));
	plot->onMouseUp().callback(at(60.0f, y0 - 30.0f));
	CHECK_FALSE(plot->onMouseWheel().callback(wheel));
	CHECK(eroField(d, ero::Param::Freq) == freqBefore);
	CHECK(eroField(d, ero::Param::Amount) == amountBefore);
	CHECK(eroField(d, ero::Param::Width) == widthBefore);
}
TEST_CASE("Card wheel rule, Eroder: the card's plot takes the wheel only while its card is focused; the same plot in the window needs no focus") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	d.set(eroAt(ero::Param::Freq), 10.0f);
	d.set(eroAt(ero::Param::Amount), -13.0f);
	d.set(eroAt(ero::Param::Width), 0.5f);
	(void)d.card->updateContent();
	ui::EroderPlot* plot = d.card->plotForTest();
	const auto wheel = [&](ui::EroderPlot& p, float dy) {
		visage::MouseEvent e;
		e.position = { p.width() * 0.5f, p.height() * 0.5f };
		e.wheel_delta_y = dy;
		e.precise_wheel_delta_y = dy;
		return p.processMouseWheel(e);
	};
	d.card->setFocused(false);
	CHECK_FALSE(wheel(*plot, 1.0f));
	CHECK(eroField(d, ero::Param::Width) == 0.5f);
	d.card->setFocused(true);
	CHECK(wheel(*plot, 1.0f));
	CHECK(eroField(d, ero::Param::Width) == doctest::Approx(0.5 + ui::EroderPlot::kWidthPerNotch));
	d.card->setFocused(false);
	const float held = eroField(d, ero::Param::Width);
	CHECK_FALSE(wheel(*plot, -1.0f));
	CHECK(eroField(d, ero::Param::Width) == held);
	// THE WINDOW'S PLOT: no card above it, so it takes the wheel with nothing focused.
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)panel->updateContent();
	ui::EroderPlot& windowPlot = *panel->plotForTest();
	CHECK(wheel(windowPlot, -1.0f));
	CHECK(eroField(d, ero::Param::Width) == doctest::Approx(held - ui::EroderPlot::kWidthPerNotch));
}
TEST_CASE("ERODER's plot takes a spectrum frame only whole and only when it is new, and rests when nothing is published") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	(void)d.card->updateContent();
	ui::EroderPlot* plot = d.card->plotForTest();
	CHECK_FALSE(plot->hasSpectrumForTest());
	// AT REST: nothing moves, so nothing repaints.
	CHECK_FALSE(d.card->updateContent());
	CHECK_FALSE(d.card->updateContent());

	// HALF-WRITTEN (the counters disagree): dropped.
	d.publish(eroFrame(3.0f, 2.0f, -40.0f, -30.0f));
	d.f.bridge.refresh();
	(void)d.card->updateContent();
	CHECK_FALSE(plot->hasSpectrumForTest());

	// WHOLE: taken, bin for bin.
	d.publish(eroFrame(3.0f, 3.0f, -40.0f, -30.0f));
	d.f.bridge.refresh();
	CHECK(d.card->updateContent());
	REQUIRE(plot->hasSpectrumForTest());
	for (const size_t k : { size_t { 0 }, size_t { 17 }, feathers::dsp::kAnalyserBins - 1 }) {
		CHECK(plot->preDbForTest(k) == -40.0f - static_cast<float>(k));
		CHECK(plot->postDbForTest(k) == -30.0f - static_cast<float>(k));
	}
	// THE SAME FRAME AGAIN IS NOT NEWS.
	CHECK_FALSE(d.card->updateContent());
	// A NEW COUNTER IS.
	d.publish(eroFrame(4.0f, 4.0f, -50.0f, -45.0f));
	d.f.bridge.refresh();
	CHECK(d.card->updateContent());
	CHECK(plot->preDbForTest(0) == -50.0f);
}
TEST_CASE("ERODER's window sub-plot keeps the read head's history, one column for each new reading") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)panel->updateContent();
	CHECK(panel->subplotForTest()->headSamplesForTest() == 0);
	auto frame = eroFrame(1.0f, 1.0f, -60.0f, -60.0f);
	for (int n = 1; n <= 5; ++n) {
		frame[static_cast<size_t>(ero::Reading::HeadNearL)] = 0.0001f;
		frame[static_cast<size_t>(ero::Reading::HeadFarL)] = 0.0001f * static_cast<float>(n + 1);
		d.publish(frame);
		d.f.bridge.refresh();
		CHECK(panel->updateContent());
		CHECK(panel->subplotForTest()->headSamplesForTest() == static_cast<size_t>(n));
		// THE SAME READING AGAIN adds nothing and repaints nothing.
		CHECK_FALSE(panel->updateContent());
	}
}
TEST_CASE("ERODER's Comb picture is the eleven quantities Tone and Character turn, with the floors the engine applies") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	for (const auto& [tone, character, gamma] :
	     { std::array { 0.0f, 0.0f, 0.0f }, std::array { 0.37f, 2.61f, 0.0f }, std::array { 5.25f, 0.5f, 0.002f } }) {
		d.set(eroAt(ero::Param::Tone), tone);
		d.set(eroAt(ero::Param::Character), character);
		d.set(eroAt(ero::Param::Gamma), gamma);
		(void)d.card->updateContent();
		const ero::Phases phases = ero::phasesOf(tone, character, gamma);
		const ero::ToneBank t = ero::evalToneBank(phases.tone);
		const ero::CharacterBank c = ero::evalCharacterBank(phases.character);
		const auto& bars = d.card->plotForTest()->barsForTest();
		CAPTURE(tone);
		CHECK(bars[0] == std::max(static_cast<float>(ero::kBankFloor), t[0]));
		CHECK(bars[1] == std::max(static_cast<float>(ero::kBankFloor), t[1]));
		for (size_t i = 2; i < 6; ++i) {
			CHECK(bars[i] == t[i]);
		}
		CHECK(bars[6] == std::max(static_cast<float>(ero::kDepthFloor), c[0]));
		for (size_t i = 1; i < 5; ++i) {
			CHECK(bars[6 + i] == c[i]);
		}
		for (const float bar : bars) {
			CHECK(bar >= 0.0f);
			CHECK(bar <= 1.0f);
		}
	}
	// NON-VACUITY: the coordinates do move the bars.
	d.set(eroAt(ero::Param::Tone), 0.0f);
	(void)d.card->updateContent();
	const auto before = d.card->plotForTest()->barsForTest();
	d.set(eroAt(ero::Param::Tone), 0.37f);
	(void)d.card->updateContent();
	CHECK(before != d.card->plotForTest()->barsForTest());
}
TEST_CASE("ERODER's window knobs are watched for arm, every one of them, once the window is given the panel state") {
	// THE SUBSCRIPTION IS THE GLOW (the lane's own case says why, for the card): a knob the panel does not watch
	// cannot be told to light. The lane sweep covers cards only, so the window says it here.
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	REQUIRE(view != nullptr);
	view->setBounds(0.0f, 0.0f, view->minimumSize().x, view->minimumSize().y);
	ui::ModState* mod = d.f.gui.modForTest();
	REQUIRE(mod != nullptr);
	std::vector<ui::Knob*> knobs;
	collectKnobs(view.get(), &knobs);
	// One for every continuous control the device declares but the three that are not knobs in the window (the
	// Comb's two coordinates and Gamma) and Drive's Shape, which is a box in its mode cell.
	size_t continuous = 0;
	for (const auto& desc : ero::kParamDescs) {
		continuous += desc.stepped ? 0u : 1u;
	}
	// THE SHARED SECTION'S FOUR CONTINUOUS CONTROLS (Emphasis, Smear, Loop, Interval) are knobs in the window now.
	// ETCH'S SLOT SHAPES AND MIXES (four) are boxes inside the slots' Type cells; its two Amounts are knobs.
	// THE EMPHASIS OFFSET is a field of the Emphasis stack, and so are Freq, Width and the Emphasis amount (the Filter and Emphasis
	// boxes): four more continuous controls that are not knobs. The Input drive is a knob and a continuous control both.
	// AND THE DC AMOUNT, the third row of the Drive's mode cell, a box like its Shape.
	// THE SLOTS' MIXES ARE KNOBS IN THE WINDOW NOW (two), so only their two Shapes are slot boxes; and the Type's second-parameter boxes (the Drive's
	// Bal, Rate and Rate multiple, and each slot's three) are nine more, and the three Angles (the S row of a Paunch Type) three continuous controls that are boxes in the cells, not knobs. AND THE THIRD ROW'S OTHERS: Hard, Sharp and Bias in the three places are nine boxes and the three Spreads are pills (twelve continuous controls that are not knobs).
	REQUIRE(knobs.size() == continuous - 2 - 4 - 1 - 3 - 1 - 9 - 3 - 3 - 12); // and the Comb's Cutoff, the Filter box's top row there; and the Smear's Amount, Reso and Offset, rows of its own box
	for (const ui::Knob* knob : knobs) {
		CHECK_FALSE(mod->watches(knob));
	}
	view->setModulation(mod, nullptr);
	for (const ui::Knob* knob : knobs) {
		CHECK(mod->watches(knob));
	}
}
TEST_CASE("ERODER's plot asks for the spectrum while Dust's picture is shown, and not hidden or in Comb") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::EroderPlot* plot = d.card->plotForTest();
	const size_t slot = d.f.bridge.fxSlotOf(d.device);
	const auto spend = [&] {
		while (d.f.bridge.deviceReadingsWanted(slot, 24000)) {
		}
		REQUIRE(d.f.bridge.deviceReadingsLeaseForTest(slot) == 0);
	};
	REQUIRE(eroField(d, ero::Param::Mode) == static_cast<float>(ero::Mode::Dust));
	// SHOWN, DUST: the whole half second the bridge grants.
	spend();
	plot->setVisible(true);
	(void)plot->update();
	CHECK(d.f.bridge.deviceReadingsLeaseForTest(slot) == 24000);
	// HIDDEN: no renewal.
	spend();
	plot->setVisible(false);
	(void)plot->update();
	CHECK(d.f.bridge.deviceReadingsLeaseForTest(slot) == 0);
	// COMB draws no spectrum, so it asks for none.
	plot->setVisible(true);
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	(void)plot->update();
	(void)plot->update();
	CHECK(d.f.bridge.deviceReadingsLeaseForTest(slot) == 0);
}

namespace {
//------------------------------------------------------------------------------------------------------------------------------//
// THE DRUM DEVICE'S CARD AND WINDOW                                                                                            //
//------------------------------------------------------------------------------------------------------------------------------//

namespace ddv = feathers::fx::drumdevice;
/// A Drum in the lane, its card, and its window built over the same device, both addressed by control INDEX.
struct DrumUi {
	DynCardFixture<ui::DrumDeviceCard> d { ddv::kTraits.tag };
	ui::WidgetRegistry registry; // a device's controls have no clap id, so nothing watches by id
	std::unique_ptr<ui::DrumDeviceExpanded> win;
	DrumUi() {
		win = std::make_unique<ui::DrumDeviceExpanded>(d.f.deviceHost, registry, d.device, nullptr);
		win->setBounds(0.0f, 0.0f, ui::DrumDeviceExpanded::kWidth, ui::DrumDeviceExpanded::kMinimumHeight);
		(void)win->updateContent();
		(void)d.card->updateContent();
	}
	void set(ddv::Param p, double v) {
		d.set(static_cast<size_t>(p), static_cast<float>(v));
		(void)win->updateContent();
		(void)d.card->updateContent();
	}
	[[nodiscard]] float field(ddv::Param p) const { return d.f.deviceHost.fieldAt(d.device, static_cast<size_t>(p)); }
	[[nodiscard]] std::vector<float> row() const {
		std::vector<float> out;
		for (size_t i = 0; i < static_cast<size_t>(ddv::Param::kCount); ++i) {
			out.push_back(d.f.deviceHost.fieldAt(d.device, i));
		}
		return out;
	}
};
} // namespace

TEST_CASE("drum device card: each widget is bound to its control by index, writes that field and no other") {
	DrumUi u;
	const auto before = u.row();
	// KIND (a choice box), NOTE (a box), LEVEL and TUNE (pills): each addresses its own descriptor entry.
	CHECK(&u.d.card->kindForTest().control().desc() == &ddv::kParamDescs[static_cast<size_t>(ddv::Param::Kind)]);
	CHECK(&u.d.card->noteForTest().control().desc() == &ddv::kParamDescs[static_cast<size_t>(ddv::Param::Note)]);
	CHECK(&u.d.card->levelForTest().amountForTest().desc() == &ddv::kParamDescs[static_cast<size_t>(ddv::Param::Level)]);
	CHECK(&u.d.card->tuneForTest().amountForTest().desc() == &ddv::kParamDescs[static_cast<size_t>(ddv::Param::Tune)]);
	const auto write = [&](device::Modulatable& c, double v) {
		c.beginGesture();
		c.setValue(v);
		c.endGesture();
	};
	write(u.d.card->kindForTest().control(), 2.0);
	write(u.d.card->noteForTest().control(), 40.0);
	write(u.d.card->levelForTest().amountForTest(), -9.0);
	write(u.d.card->tuneForTest().amountForTest(), 5.0);
	const auto after = u.row();
	for (size_t i = 0; i < after.size(); ++i) {
		CAPTURE(i);
		const auto p = static_cast<ddv::Param>(i);
		const float want = p == ddv::Param::Kind ? 2.0f : p == ddv::Param::Note ? 40.0f : p == ddv::Param::Level ? -9.0f : p == ddv::Param::Tune ? 5.0f : before[i];
		CHECK(after[i] == want);
	}
	// THE TRIG PAD IS THERE and drives the device's own Trig control: a press holds it at 1 (the edge the drum strikes on), the release puts it back.
	REQUIRE(u.d.card->trigPadForTest() != nullptr);
	CHECK(&u.d.card->trigPadForTest()->control().desc() == &ddv::kParamDescs[static_cast<size_t>(ddv::Param::Trig)]);
	CHECK(u.field(ddv::Param::Trig) == 0.0f);
	u.d.card->trigPadForTest()->press();
	CHECK(u.field(ddv::Param::Trig) == 1.0f);
	u.d.card->trigPadForTest()->release();
	CHECK(u.field(ddv::Param::Trig) == 0.0f);
}
TEST_CASE("drum Trig pad: the card's and the window's pads let go through the release road, so a click quicker than a block still strikes") {
	DrumUi u;
	REQUIRE(u.d.card->trigPadForTest() != nullptr);
	REQUIRE(u.win->trigPadForTest() != nullptr);
	CHECK(u.d.card->trigPadForTest()->releaseRoadForTest());
	CHECK(u.win->trigPadForTest()->releaseRoadForTest());
}

TEST_CASE("drum Trig pad: a real press raises the Trig on the press and before the release, a Gate holds it across, a Drone hides the pad") {
	DrumUi u;
	const auto mouse = [](bool down) {
		visage::MouseEvent e;
		e.position = { 6.0f, 6.0f };
		e.button_id = visage::kMouseButtonLeft;
		e.button_state = down ? visage::kMouseButtonLeft : visage::kMouseButtonNone;
		e.is_down = down;
		return e;
	};
	for (const bool gate : { false, true }) {
		CAPTURE(gate);
		u.set(ddv::Param::Trigger, gate ? 1.0 : 0.0);
		for (ui::TrigPad* pad : { u.d.card->trigPadForTest(), u.win->trigPadForTest() }) {
			REQUIRE(pad != nullptr);
			REQUIRE(pad->isVisible());
			CHECK(u.field(ddv::Param::Trig) == 0.0f);
			// THE PRESS: the Trig is high from the press itself (the drum's edge detector strikes on this rise), the release has not happened.
			pad->onMouseDown().callback(mouse(true));
			CHECK(u.field(ddv::Param::Trig) == 1.0f);
			CHECK(pad->lit());
			// HELD ACROSS THE PRESS, in either trigger mode: nothing lets go until the release.
			(void)u.d.card->updateContent();
			(void)u.win->updateContent();
			CHECK(u.field(ddv::Param::Trig) == 1.0f);
			pad->onMouseUp().callback(mouse(false));
			CHECK(u.field(ddv::Param::Trig) == 0.0f);
		}
	}
	// A DRONE TAKES NO TRIGGER: hidden on the card and in the window, on a kind that can be held; a kind that cannot be held keeps it.
	u.set(ddv::Param::Kind, static_cast<double>(feathers::drum::Kind::FM));
	u.set(ddv::Param::Trigger, 2.0);
	CHECK_FALSE(u.d.card->trigPadForTest()->isVisible());
	CHECK_FALSE(u.win->trigPadForTest()->isVisible());
	u.set(ddv::Param::Trigger, 0.0);
	CHECK(u.d.card->trigPadForTest()->isVisible());
	CHECK(u.win->trigPadForTest()->isVisible());
	u.set(ddv::Param::Kind, static_cast<double>(feathers::drum::Kind::Membrane));
	u.set(ddv::Param::Trigger, 2.0); // not a sustaining kind: the Drone choice means a One-shot there, so the pad stays
	CHECK(u.d.card->trigPadForTest()->isVisible());
}
TEST_CASE("drum device: the picture's handles write their own controls by value, Tune in whole semitones and Shift fine, and a Kind shows only its own") {
	DrumUi u;
	using H = ui::HitPicture::Handle;
	ui::HitPicture& pic = u.win->pictureForTest();
	// A KICK: Tune, Decay, Level, Sweep Time, Drive, and the kick's shape are on the plot; a snare's are not.
	for (const H h : { H::Tune, H::Decay, H::Level, H::SweepTime, H::Drive, H::Hold, H::Dip, H::ClickSweep, H::StartPhase }) {
		CAPTURE(static_cast<int>(h));
		CHECK(pic.handleShown(h));
	}
	for (const H h : { H::WireDecay, H::ClickDecay, H::BellStart, H::BellEnd, H::Bell2, H::Spread, H::ClapBand }) {
		CHECK_FALSE(pic.handleShown(h));
	}
	const float tune0 = u.field(ddv::Param::Tune);
	pic.dragForTest(H::Tune, 0.0f, -17.0f);
	const float tuned = u.field(ddv::Param::Tune);
	CHECK(tuned > tune0);
	CHECK(tuned == std::round(tuned)); // whole semitones
	pic.dragForTest(H::Tune, 0.0f, -17.0f, true);
	CHECK(u.field(ddv::Param::Tune) != std::round(u.field(ddv::Param::Tune))); // Shift: fine, not rounded
	const auto row = u.row();
	pic.dragForTest(H::Decay, 40.0f, 0.0f);
	const auto moved = u.row();
	for (size_t i = 0; i < row.size(); ++i) {
		CAPTURE(i);
		CHECK((moved[i] != row[i]) == (static_cast<ddv::Param>(i) == ddv::Param::Decay));
	}
	// THE LEVEL HANDLE.
	const float level0 = u.field(ddv::Param::Level);
	pic.dragForTest(H::Level, 0.0f, 20.0f);
	CHECK(u.field(ddv::Param::Level) < level0);
	// A SNARE'S: its wires, click and bells are the plot's, a kick's shape is gone.
	u.set(ddv::Param::Kind, static_cast<double>(feathers::drum::Kind::Snare));
	for (const H h : { H::WireDecay, H::ClickDecay, H::BellStart, H::BellEnd, H::Bell2 }) {
		CHECK(pic.handleShown(h));
	}
	for (const H h : { H::Hold, H::Dip, H::ClickSweep, H::StartPhase, H::SweepTime }) {
		CHECK_FALSE(pic.handleShown(h));
	}
}
TEST_CASE("drum device window: a control the Kind does not read is hidden, one it reads is drawn, for every Kind") {
	DrumUi u;
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		CAPTURE(k);
		u.set(ddv::Param::Kind, static_cast<double>(k));
		for (size_t i = 0; i < static_cast<size_t>(ddv::Param::kCount); ++i) {
			const auto p = static_cast<ddv::Param>(i);
			visage::Frame* w = u.win->widgetForTest(p);
			if (w == nullptr) {
				continue; // the header chip's (Enabled)
			}
			CAPTURE(ddv::kParamDescs[i].name);
			const bool read = device::relevantFor(ddv::kParamDescs[i], ddv::kParamDescs[i].gateControl >= 0 ? u.field(static_cast<ddv::Param>(ddv::kParamDescs[i].gateControl)) : 0.0);
			// THE TRIGGER HAS TWO FACES: the plain two-way One-shot | Gate, and the three-way with Drone on a kind that can hold a hit.
			const bool holdable = feathers::drum::kindSustains(static_cast<feathers::drum::Kind>(k));
			CHECK(w->isVisible() == (p == ddv::Param::Trigger ? read && !holdable : read));
			if (p == ddv::Param::Trigger) {
				REQUIRE(u.win->triggerDroneForTest() != nullptr);
				CHECK(u.win->triggerDroneForTest()->isVisible() == holdable); // Drone is offered only where the engine can hold the hit
			}
		}
	}
	// AND AGAINST THE ENGINE'S OWN STATEMENT: a Kick reads Hold, a Clap does not; a Clap reads Claps, a Kick does not; Release only on a Gate.
	u.set(ddv::Param::Kind, 0.0);
	CHECK(u.win->widgetForTest(ddv::Param::KickHold)->isVisible());
	CHECK_FALSE(u.win->widgetForTest(ddv::Param::Claps)->isVisible());
	CHECK_FALSE(u.win->widgetForTest(ddv::Param::Release)->isVisible());
	u.set(ddv::Param::Trigger, 1.0);
	CHECK(u.win->widgetForTest(ddv::Param::Release)->isVisible());
	u.set(ddv::Param::Kind, static_cast<double>(feathers::drum::Kind::Clap));
	CHECK(u.win->widgetForTest(ddv::Param::Claps)->isVisible());
	CHECK_FALSE(u.win->widgetForTest(ddv::Param::KickHold)->isVisible());
}
TEST_CASE("drum tooltips follow the Kind: Tone, Snap, Decay, Tune and High Pass name what they do in the current Kind, on the window and the card") {
	DrumUi u;
	const auto tipOf = [&](ddv::Param p) { return std::string(ui::tooltipOf(&u.win->modelForTest().control(p))); };
	for (size_t k = 0; k < feathers::drum::kKindCount; ++k) {
		CAPTURE(k);
		u.set(ddv::Param::Kind, static_cast<double>(k));
		CHECK(tipOf(ddv::Param::FieldTone) == ddv::kToneTips[k]);
		CHECK(tipOf(ddv::Param::FieldSnap) == ddv::kSnapTips[k]);
		CHECK(tipOf(ddv::Param::Decay) == ddv::kDecayTips[k]);
		CHECK(tipOf(ddv::Param::Tune) == ddv::kTuneTips[k]);
		CHECK(tipOf(ddv::Param::HighPass) == ddv::kHighPassTips[k]);
		// THE CARD'S TUNE PILL shares the same model road: the card's own control reads the same text.
		CHECK(std::string(ui::tooltipOf(&u.d.card->modelForTest().control(ddv::Param::Tune))) == ddv::kTuneTips[k]);
	}
	// NOT VACUOUS: the Kind moved the text (a Kick's Tone is not a Snare's).
	u.set(ddv::Param::Kind, 0.0);
	const std::string kickTone = tipOf(ddv::Param::FieldTone);
	u.set(ddv::Param::Kind, static_cast<double>(feathers::drum::Kind::Snare));
	CHECK(tipOf(ddv::Param::FieldTone) != kickTone);
	// PITCHED: the key sets the pitch, so Tune names the key, in every Kind.
	u.set(ddv::Param::Keys, 2.0);
	CHECK(tipOf(ddv::Param::Tune) == ddv::kTunePitchedTip);
	u.set(ddv::Param::Keys, 0.0);
	CHECK(tipOf(ddv::Param::Tune) == ddv::kTuneTips[static_cast<size_t>(feathers::drum::Kind::Snare)]);
}
TEST_CASE("Delay's Time and Mix hover text follow the Mode, Sync and Dub, on the card and on the window alike") {
	namespace dly = feathers::fx::delay;
	DynCardFixture<ui::DelayCard> d { dly::kTraits.tag };
	ui::WidgetRegistry registry;
	auto win = std::make_unique<ui::DelayExpanded>(d.f.deviceHost, registry, d.device, nullptr);
	// THE TWO SURFACES' OWN CONTROLS, the card's from its boxes and the window's from its seam.
	const auto cardTip = [&](dly::Param p) { return std::string(ui::tooltipOf(&d.card->controlForTest(static_cast<size_t>(p)))); };
	const auto winTip = [&](dly::Param p) { return std::string(ui::tooltipOf(&win->controlForTest(p))); };
	const auto set = [&](dly::Param p, double v) {
		d.set(static_cast<size_t>(p), static_cast<float>(v));
		(void)win->updateContent();
		(void)d.card->updateContent();
	};
	const std::string declaredMix = dly::kParamDescs[static_cast<size_t>(dly::Param::Mix)].tooltip;
	// THE MODE NAMES THE TIME: a Multi-Tap's pattern, a Reverse's chunk, and a tap's own multiplier in the rest.
	set(dly::Param::Mode, static_cast<double>(dly::Mode::MultiTap));
	CHECK(cardTip(dly::Param::Time).rfind("How long the whole tap pattern is", 0) == 0);
	CHECK(winTip(dly::Param::Time) == cardTip(dly::Param::Time));
	set(dly::Param::Mode, static_cast<double>(dly::Mode::Reverse));
	CHECK(cardTip(dly::Param::Time).rfind("How long each reversed chunk is", 0) == 0);
	CHECK(winTip(dly::Param::Time) == cardTip(dly::Param::Time));
	set(dly::Param::Mode, static_cast<double>(dly::Mode::Clean));
	CHECK(cardTip(dly::Param::Time).rfind("How many of", 0) == 0);
	CHECK(winTip(dly::Param::Time) == cardTip(dly::Param::Time));
	// THE SYNC NAMES THE BASE: a free time, then a division.
	set(dly::Param::Sync, 0.0);
	CHECK(cardTip(dly::Param::Time).find("the free time") != std::string::npos);
	set(dly::Param::Sync, static_cast<double>(dly::kSyncNoteChoice));
	CHECK(cardTip(dly::Param::Time).find("division") != std::string::npos);
	CHECK(winTip(dly::Param::Time) == cardTip(dly::Param::Time));
	// MIX IS THE SEND WITH DUB ON, and the declared dry against wet with it off.
	set(dly::Param::Dub, 0.0);
	CHECK(cardTip(dly::Param::Mix) == declaredMix);
	CHECK(winTip(dly::Param::Mix) == declaredMix);
	set(dly::Param::Dub, 1.0);
	CHECK(cardTip(dly::Param::Mix) == ui::delay_tips::mixTip(1));
	CHECK(winTip(dly::Param::Mix) == ui::delay_tips::mixTip(1));
	CHECK(cardTip(dly::Param::Mix) != declaredMix);
}
TEST_CASE("OWLTT's crossovers say what Key Track makes them: an offset from the note with it on, the declared frequency with it off, on the card and the window") {
	namespace ow = feathers::fx::owltt;
	DynCardFixture<ui::OwlttCard> d { ow::kTraits.tag };
	ui::WidgetRegistry registry;
	auto win = std::make_unique<ui::OwlttExpanded>(d.f.deviceHost, registry, d.device, nullptr);
	const auto cardTip = [&](ow::Param p) { return std::string(ui::tooltipOf(&d.card->controlForTest(static_cast<size_t>(p)))); };
	const auto winTip = [&](ow::Param p) { return std::string(ui::tooltipOf(&win->controlForTest(p))); };
	const auto setKeyTrack = [&](double v) {
		d.set(static_cast<size_t>(ow::Param::KeyTrack), static_cast<float>(v));
		(void)win->updateContent();
		(void)d.card->updateContent();
	};
	// KEY TRACK OFF: the declared frequencies.
	setKeyTrack(0.0);
	CHECK(cardTip(ow::Param::LowCrossover) == "Low-mid split frequency");
	CHECK(winTip(ow::Param::HighCrossover) == "Mid-high split frequency");
	// ON: each is an offset from the note, and the two texts are not the declared ones (not vacuous).
	setKeyTrack(1.0);
	CHECK(cardTip(ow::Param::LowCrossover) == ow::kLowCrossoverTipKeyTrack);
	CHECK(cardTip(ow::Param::HighCrossover) == ow::kHighCrossoverTipKeyTrack);
	CHECK(winTip(ow::Param::LowCrossover) == ow::kLowCrossoverTipKeyTrack);
	CHECK(winTip(ow::Param::HighCrossover) == ow::kHighCrossoverTipKeyTrack);
	CHECK(cardTip(ow::Param::LowCrossover) != "Low-mid split frequency");
	// AND BACK OFF: the declared text returns on both surfaces.
	setKeyTrack(0.0);
	CHECK(cardTip(ow::Param::LowCrossover) == "Low-mid split frequency");
	CHECK(winTip(ow::Param::LowCrossover) == "Low-mid split frequency");
}
TEST_CASE("Sklitter's Groove Amount says what its Groove Mode makes it: the swing, the template share, the walk, or nothing in Straight, on the card and the window") {
	namespace sk = feathers::fx::sklitter;
	DynCardFixture<ui::SklitterCard> d { sk::kTraits.tag };
	ui::WidgetRegistry registry;
	auto win = std::make_unique<ui::SklitterExpanded>(d.f.deviceHost, registry, d.device, sk::kDescriptor, nullptr);
	const auto cardTip = [&] { return std::string(ui::tooltipOf(&d.card->controlForTest(static_cast<size_t>(sk::Param::GrooveAmount)))); };
	const auto winTip = [&] { return std::string(ui::tooltipOf(&win->controlForTest(static_cast<size_t>(sk::Param::GrooveAmount)))); };
	const auto setMode = [&](int m) {
		d.set(static_cast<size_t>(sk::Param::GrooveMode), static_cast<float>(m));
		(void)win->updateContentForTest();
		(void)d.card->updateContentForTest();
	};
	// ONE TEXT PER FAMILY, the same on both surfaces; each family's text differs from the others (not vacuous).
	setMode(0);
	CHECK(cardTip() == sk::kGrooveAmountTipStraight);
	CHECK(winTip() == sk::kGrooveAmountTipStraight);
	setMode(2);
	CHECK(cardTip() == sk::kGrooveAmountTipSwing);
	CHECK(winTip() == sk::kGrooveAmountTipSwing);
	setMode(5);
	CHECK(cardTip() == sk::kGrooveAmountTipGroove);
	CHECK(winTip() == sk::kGrooveAmountTipGroove);
	setMode(8);
	CHECK(cardTip() == sk::kGrooveAmountTipFree);
	CHECK(winTip() == sk::kGrooveAmountTipFree);
	CHECK(std::string(sk::kGrooveAmountTipSwing) != sk::kGrooveAmountTipGroove);
	// THE DECLARED TEXT NAMES NO SAMPLER (the hardware rule).
	CHECK(std::string(sk::kParamDescs[static_cast<size_t>(sk::Param::GrooveAmount)].tooltip).find("MPC") == std::string::npos);
}
TEST_CASE("Arp's Rate says what its Time Base makes it: steps per second on Hz, a multiple of the division otherwise, on the card and the window") {
	namespace apx = feathers::mod::arp;
	DynCardFixture<ui::ArpCard> d { apx::kTraits.tag };
	ui::WidgetRegistry registry;
	auto win = std::make_unique<ui::ArpExpanded>(d.f.deviceHost, registry, d.device, nullptr);
	const auto rateOn = [&](ui::DeviceControl& c) { return std::string(ui::tooltipOf(&c)); };
	const auto setTimeBase = [&](double v) {
		d.set(static_cast<size_t>(apx::Param::TimeBase), static_cast<float>(v));
		(void)win->updateContent();
		(void)d.card->updateContent();
	};
	setTimeBase(0.0); // Hz
	CHECK(rateOn(d.card->control(static_cast<size_t>(apx::Param::Rate))) == apx::kRateTipHz);
	CHECK(rateOn(win->controlForTest(static_cast<size_t>(apx::Param::Rate))) == apx::kRateTipHz);
	setTimeBase(static_cast<double>(apx::kDefaultTimeBase)); // 1/16
	CHECK(rateOn(d.card->control(static_cast<size_t>(apx::Param::Rate))) == apx::kRateTipDivision);
	CHECK(rateOn(win->controlForTest(static_cast<size_t>(apx::Param::Rate))) == apx::kRateTipDivision);
	CHECK(std::string(apx::kRateTipHz) != apx::kRateTipDivision); // not vacuous
}
TEST_CASE("Follower's Input is disabled in Audio, where it does nothing, and its hover text says why; in Control it is live with its own text") {
	namespace fl = feathers::mod::follower;
	DynCardFixture<ui::FollowerCard> d { fl::kTraits.tag };
	const auto inputTip = [&] { return std::string(ui::tooltipOf(d.card->input().control())); };
	const auto setMode = [&](double v) {
		d.set(static_cast<size_t>(fl::Param::Mode), static_cast<float>(v));
		(void)d.card->updateContent();
	};
	setMode(static_cast<double>(fl::Mode::Control));
	CHECK(d.card->input().enabled());
	CHECK(inputTip() == fl::kParamDescs[static_cast<size_t>(fl::Param::Input)].tooltip);
	setMode(static_cast<double>(fl::Mode::Audio));
	CHECK_FALSE(d.card->input().enabled());
	CHECK(inputTip().find("does nothing in Audio") != std::string::npos);
	CHECK(inputTip().find("Switch Mode to Control") != std::string::npos);
	// AND BACK: live again, the declared text.
	setMode(static_cast<double>(fl::Mode::Control));
	CHECK(d.card->input().enabled());
	CHECK(inputTip() == fl::kParamDescs[static_cast<size_t>(fl::Param::Input)].tooltip);
}
TEST_CASE("drum device window: every widget is bound to its control by index, and a write lands in that field alone") {
	DrumUi u;
	for (size_t i = 0; i < static_cast<size_t>(ddv::Param::kCount); ++i) {
		const auto p = static_cast<ddv::Param>(i);
		visage::Frame* w = u.win->widgetForTest(p);
		if (w == nullptr) {
			continue;
		}
		const device::Modulatable* bound = nullptr;
		if (auto* box = dynamic_cast<ui::ValueBox*>(w)) {
			bound = &box->control();
		}
		else if (auto* pill = dynamic_cast<ui::FaderPill*>(w)) {
			bound = &pill->amountForTest();
		}
		else if (auto* seg = dynamic_cast<ui::SegmentedControl*>(w)) {
			bound = seg->control();
		}
		else if (auto* chip = dynamic_cast<ui::SwitchChip*>(w)) {
			bound = &chip->control();
		}
		REQUIRE(bound != nullptr);
		CAPTURE(ddv::kParamDescs[i].name);
		CHECK(&bound->desc() == &ddv::kParamDescs[i]);
	}
}
TEST_CASE("drum device window: a note or a frequency typed into the readout sets Tune so the settled pitch lands there, within a cent") {
	DrumUi u;
	u.set(ddv::Param::PitchMode, 2.0); // the terrain's pitch off: a Kick's settled pitch is its resting pitch moved by Tune alone
	ui::PitchReadout& readout = u.win->readoutForTest();
	REQUIRE(readout.isVisible());
	for (const auto& [text, hz] : { std::pair<const char*, double> { "A1", 55.0 }, { "c#3", 138.5913154884 }, { "72.5 Hz", 72.5 } }) {
		CAPTURE(text);
		readout.clickForTest();
		ui::TextField* field = readout.typeInField();
		REQUIRE(field != nullptr);
		REQUIRE(field->editing());
		field->processTextInput(text);
		field->processKeyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true });
		(void)u.win->updateContent();
		const feathers::drum::SettledPitch settled = u.win->modelForTest().settled();
		REQUIRE(settled.defined);
		CHECK(std::abs(1200.0 * std::log2(settled.hz / hz)) < 1.0);
	}
	// A METAL IS APPROXIMATE: no one pitch, so the field does not open.
	u.set(ddv::Param::Kind, static_cast<double>(feathers::drum::Kind::Metal));
	ui::PitchReadout& metal = u.win->readoutForTest();
	const float tune = u.field(ddv::Param::Tune);
	metal.clickForTest();
	CHECK((metal.typeInField() == nullptr || !metal.typeInField()->editing()));
	CHECK(u.field(ddv::Param::Tune) == tune);
}
TEST_CASE("drum device window: the terrain pad writes Travel X and Travel Y only, unbounded, and the view follows") {
	DrumUi u;
	ui::DrumDevicePad& pad = u.win->padForTest();
	REQUIRE(pad.width() > 20.0f);
	visage::Canvas canvas;
	pad.draw(canvas);
	const auto row = u.row();
	const float half = std::min(pad.width(), pad.height()) * 0.5f;
	const float px = pad.width() * 0.5f + 30.0f;
	const float py = pad.height() * 0.5f - 20.0f;
	const auto at = [](float x, float y) {
		visage::MouseEvent e;
		e.position = { x, y };
		return e;
	};
	pad.onMouseDown().callback(at(px, py));
	pad.onMouseUp().callback(at(px, py));
	const auto moved = u.row();
	for (size_t i = 0; i < row.size(); ++i) {
		CAPTURE(i);
		const auto p = static_cast<ddv::Param>(i);
		CHECK((moved[i] != row[i]) == (p == ddv::Param::TravelX || p == ddv::Param::TravelY));
	}
	CHECK(u.field(ddv::Param::TravelX) == doctest::Approx(30.0 / half).epsilon(1.0e-5));
	CHECK(u.field(ddv::Param::TravelY) == doctest::Approx(20.0 / half).epsilon(1.0e-5));
	// UNBOUNDED: held at the pad's edge the view scrolls and the value goes past anything a window would hold.
	pad.draw(canvas);
	pad.onMouseDown().callback(at(pad.width() * 0.5f, pad.height() * 0.5f));
	pad.onMouseDrag().callback(at(pad.width() - 1.0f, pad.height() * 0.5f));
	for (int i = 0; i < 200; ++i) {
		(void)pad.tickForTest(0.05);
	}
	CHECK(u.field(ddv::Param::TravelX) > 5.0f);
	CHECK(std::abs(pad.viewForTest().first) > 5.0);
	pad.onMouseUp().callback(at(px, py));
}
TEST_CASE("drum device: Keys is a chip on the card and the window, and Pitched hides what it does not read (not greys it)") {
	DrumUi u;
	// THE CHIP, BOUND BY INDEX, on both surfaces; Keys is a two-state SwitchChip, not a box.
	REQUIRE(u.d.card->keysForTest().control() != nullptr);
	CHECK(&u.d.card->keysForTest().control()->desc() == &ddv::kParamDescs[static_cast<size_t>(ddv::Param::Keys)]);
	auto* windowKeys = dynamic_cast<ui::SegmentedControl*>(u.win->widgetForTest(ddv::Param::Keys));
	REQUIRE(windowKeys != nullptr);
	REQUIRE(windowKeys->control() != nullptr);
	CHECK(&windowKeys->control()->desc() == &ddv::kParamDescs[static_cast<size_t>(ddv::Param::Keys)]);
	const auto shown = [&](ddv::Param p) { return u.win->widgetForTest(p)->isVisible(); };
	// ONE KEY: Note, Bend and Terrain Pitch are drawn, the card's Note box too, and the pitch readout.
	CHECK(u.d.card->noteForTest().isVisible());
	CHECK(shown(ddv::Param::Note));
	CHECK(shown(ddv::Param::Bend));
	CHECK(shown(ddv::Param::PitchMode));
	CHECK(u.win->readoutForTest().isVisible());
	// PITCHED: the key is the pitch, so Note is not read, the kit's Bend share and Terrain Pitch are not read (the engine's Pitched
	// mode leaves them out), and there is no one settled pitch to read or type. Hidden everywhere, not greyed.
	// ANY KEY: Note is not read (every key is answered), but the drum's own pitch is, so Bend, Terrain Pitch and the readout stay.
	u.set(ddv::Param::Keys, 1.0);
	CHECK_FALSE(u.d.card->noteForTest().isVisible());
	CHECK_FALSE(shown(ddv::Param::Note));
	CHECK(shown(ddv::Param::Bend));
	CHECK(shown(ddv::Param::PitchMode));
	CHECK(u.win->readoutForTest().isVisible());
	u.set(ddv::Param::Keys, 2.0);
	CHECK_FALSE(u.d.card->noteForTest().isVisible());
	CHECK_FALSE(shown(ddv::Param::Note));
	CHECK_FALSE(shown(ddv::Param::Bend));
	CHECK_FALSE(shown(ddv::Param::PitchMode));
	CHECK_FALSE(u.win->readoutForTest().isVisible());
	// What it DOES read stays: Tune, the picture, Level.
	CHECK(shown(ddv::Param::Tune));
	CHECK(u.d.card->tuneForTest().isVisible());
	// BACK TO ONE KEY brings them back.
	u.set(ddv::Param::Keys, 0.0);
	CHECK(u.d.card->noteForTest().isVisible());
	CHECK(shown(ddv::Param::Note));
	CHECK(u.win->readoutForTest().isVisible());
	// THE PLAY BUTTON strikes middle C when Pitched (Note is not read), and the Note otherwise.
	CHECK(u.d.card->modelForTest().noteKey() == static_cast<int>(u.field(ddv::Param::Note)));
	u.set(ddv::Param::Keys, 2.0);
	CHECK(u.d.card->modelForTest().noteKey() == 60);
}
TEST_CASE("drum device: the window offers every control the card offers, by descriptor entry") {
	DrumUi u;
	const auto collect = [](visage::Frame* root) {
		std::set<const device::ParamDesc*> out;
		const std::function<void(visage::Frame*)> walk = [&](visage::Frame* f) {
			if (const auto* face = dynamic_cast<const ui::ControlFace*>(f)) {
				std::vector<ui::ControlFace::Offered> list;
				face->offeredControls(list);
				for (const auto& o : list) {
					const device::ParamDesc* d = &o.control->desc();
					if (d > std::begin(ddv::kParamDescs) && d < std::end(ddv::kParamDescs)) { // the DRUM's own controls, but Enabled: the head chip is the window's too
						out.insert(d);
					}
				}
			}
			for (visage::Frame* c : f->children()) {
				walk(c);
			}
		};
		walk(root);
		return out;
	};
	const auto card = collect(u.d.card);
	const auto window = collect(u.win.get());
	CHECK(card.size() >= 4);
	for (const device::ParamDesc* d : card) {
		CAPTURE(d - std::begin(ddv::kParamDescs));
		CAPTURE(d->name);
		CHECK(window.count(d) == 1);
	}
}
TEST_CASE("drum device window: the picture's handles are modulation targets: a halo when armed, a route on an armed press along the handle's own axis, an amount arc where the plot's axis puts it") {
	DrumUi u;
	ui::ModState* mod = u.d.f.gui.modForTest();
	REQUIRE(mod != nullptr);
	u.win->setModulation(mod, u.d.f.gui.history());
	(void)u.win->updateContent();
	using H = ui::HitPicture::Handle;
	ui::HitPicture& pic = u.win->pictureForTest();
	const auto at = [](float x, float y) {
		visage::MouseEvent e;
		e.position = { x, y };
		return e;
	};
	const backplane::Source macro = backplane::Source::core(0);
	const auto dest = [&](ddv::Param p) { return backplane::Destination::control(u.d.device, static_cast<size_t>(p)); };
	// NOTHING ARMED: offered (a destination) but quiet.
	ui::ModHandlePlan quiet = pic.modPlanOf(H::Tune);
	REQUIRE(quiet.axes.size() == 1);
	CHECK(quiet.axes[0].offered);
	CHECK_FALSE(quiet.halo);
	CHECK_FALSE(quiet.axes[0].hasStroke);
	// ARMED: the halo on every handle whose control the source may reach.
	mod->focus(macro);
	mod->arm(true);
	REQUIRE(u.d.f.bridge.mayRoute(macro, dest(ddv::Param::Tune)));
	CHECK(pic.modPlanOf(H::Tune).halo);
	CHECK(pic.modPlanOf(H::Decay).halo);
	CHECK(pic.modPlanOf(H::Level).halo);
	SUBCASE("an armed press on a VERTICAL handle (Tune) routes, and only vertical travel edits the amount; the value does not move") {
		const visage::Point h = pic.markAt(H::Tune);
		const float before = u.field(ddv::Param::Tune);
		pic.onMouseDown().callback(at(h.x, h.y));
		pic.onMouseDrag().callback(at(h.x + 40.0f, h.y)); // sideways: nothing
		CHECK(u.d.f.bridge.routeAmount(macro, dest(ddv::Param::Tune)) == doctest::Approx(ui::Knob::kRouteSeedAmount));
		pic.onMouseDrag().callback(at(h.x + 40.0f, h.y - 30.0f)); // up: more
		pic.onMouseUp().callback(at(h.x + 40.0f, h.y - 30.0f));
		CHECK(u.d.f.bridge.routeAmount(macro, dest(ddv::Param::Tune)) == doctest::Approx(ui::Knob::kRouteSeedAmount + 30.0f / ui::Knob::kRouteDragRange));
		CHECK(u.field(ddv::Param::Tune) == before);
		CHECK(u.d.f.bridge.routeAmount(macro, dest(ddv::Param::Decay)) == 0.0f);
	}
	SUBCASE("an armed press on an ACROSS handle (Decay) routes along its own axis: sideways edits the amount, vertical does nothing") {
		const visage::Point h = pic.markAt(H::Decay);
		const float before = u.field(ddv::Param::Decay);
		pic.onMouseDown().callback(at(h.x, h.y));
		pic.onMouseDrag().callback(at(h.x, h.y - 30.0f));
		CHECK(u.d.f.bridge.routeAmount(macro, dest(ddv::Param::Decay)) == doctest::Approx(ui::Knob::kRouteSeedAmount));
		pic.onMouseDrag().callback(at(h.x + 35.0f, h.y - 30.0f));
		pic.onMouseUp().callback(at(h.x + 35.0f, h.y - 30.0f));
		CHECK(u.d.f.bridge.routeAmount(macro, dest(ddv::Param::Decay)) == doctest::Approx(ui::Knob::kRouteSeedAmount + 35.0f / ui::Knob::kRouteDragRange));
		CHECK(u.field(ddv::Param::Decay) == before);
	}
	SUBCASE("the amount arc runs from the handle's base to base + amount through the plot's own axis (Tune: the frequency axis)") {
		const double hz = u.win->modelForTest().laws().hz;
		for (const float amount : { 0.05f, -0.05f }) {
			CAPTURE(amount);
			u.d.f.bridge.setRouteAmount(macro, dest(ddv::Param::Tune), amount);
			(void)u.win->updateContent();
			const ui::ModHandlePlan plan = pic.modPlanOf(H::Tune);
			REQUIRE(plan.axes[0].hasStroke);
			const double span = 96.0; // Tune's declared range: a full-depth route moves it this far
			const float a = pic.yOfHz(hz);
			const float b = pic.yOfHz(hz * std::exp2(static_cast<double>(amount) * span / 12.0));
			CHECK(plan.axes[0].strokeLo == doctest::Approx(std::min(a, b)).epsilon(1.0e-3));
			CHECK(plan.axes[0].strokeHi == doctest::Approx(std::max(a, b)).epsilon(1.0e-3));
			CHECK(plan.axes[0].base == doctest::Approx(a).epsilon(1.0e-3));
		}
	}
	// UNARMED AGAIN, A PRESS IS A VALUE DRAG AS BEFORE.
	mod->arm(false);
	const visage::Point h = pic.markAt(H::Level);
	const float level0 = u.field(ddv::Param::Level);
	pic.onMouseDown().callback(at(h.x, h.y));
	pic.onMouseDrag().callback(at(h.x, h.y + 20.0f));
	pic.onMouseUp().callback(at(h.x, h.y + 20.0f));
	CHECK(u.field(ddv::Param::Level) < level0);
}
TEST_CASE("A pair cell holds its two cells side by side under one caption, and a flowed grid never parts them") {
	visage::Frame a;
	visage::Frame b;
	ui::PairCell pair("Drive", a, b);
	CHECK(a.parent() == &pair);
	CHECK(b.parent() == &pair);
	CHECK(pair.caption() == "Drive");
	pair.setBounds(10.0f, 20.0f, 120.0f, ui::PairCell::heightFor(60.0f));
	CHECK(a.x() == 0.0f);
	CHECK(a.width() == 60.0f);
	CHECK(b.x() == 60.0f);
	CHECK(b.width() == 60.0f);
	CHECK(a.y() == ui::PairCell::kCaptionHeight);
	CHECK(a.height() == 60.0f);
	CHECK(b.height() == 60.0f);
	pair.setSplit(0.25f);
	CHECK(a.width() == 30.0f);
	CHECK(b.x() == 30.0f);
	CHECK(b.width() == 90.0f);

	// THE FLOW: three columns; two singles, then a pair that does not fit the one column left — it wraps whole.
	visage::Frame one;
	visage::Frame two;
	visage::Frame hidden;
	visage::Frame after;
	const std::vector<ui::FlowItem> items {
		{ &one, 1, true, 50.0f }, { &two, 1, true, 50.0f }, { &hidden, 1, false, 50.0f },
		{ &pair, 2, true, 62.0f }, { &after, 1, true, 50.0f },
	};
	const float foot = ui::flowCells(items, 3, 100.0f, 10.0f, 40.0f, 6.0f);
	CHECK(one.x() == 100.0f);
	CHECK(two.x() == 140.0f);
	CHECK(one.y() == 10.0f);
	CHECK_FALSE(hidden.isVisible());
	CHECK(pair.x() == 100.0f); // not the third column of the first row
	CHECK(pair.y() == 10.0f + 50.0f + 6.0f);
	CHECK(pair.width() == 80.0f);
	CHECK(after.x() == 180.0f); // beside it, on the pair's row
	CHECK(after.y() == pair.y());
	CHECK(foot == pair.y() + 62.0f);
	// AND A PAIR THAT FITS STAYS ON ITS ROW.
	const std::vector<ui::FlowItem> fits { { &one, 1, true, 50.0f }, { &pair, 2, true, 62.0f } };
	(void)ui::flowCells(fits, 3, 0.0f, 0.0f, 40.0f, 6.0f);
	CHECK(pair.x() == 40.0f);
	CHECK(pair.y() == 0.0f);
}
TEST_CASE("ERODER's Drive is one pair in the card and the window: the knob, and the Type whose Shape is its own control") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)d.card->updateContent();
	(void)panel->updateContent();

	const auto check = [&](ui::PairCell* pair, ui::Knob* knob, ui::ModeValueCell* cell, float surfaceWidth) {
		REQUIRE(pair != nullptr);
		// THE CARD'S PAIR IS CAPTIONED; THE WINDOW'S GROUP SAYS IT (the pair there is bare).
		CHECK((pair->caption() == "Drive") == !pair->bare());
		CHECK(knob->parent() == pair);
		CHECK(cell->parent() == pair);
		CHECK(pair->isVisible());
		// SIDE BY SIDE, ON ONE LINE, inside the surface.
		CHECK(knob->y() == cell->y());
		CHECK(knob->right() <= cell->x());
		CHECK(pair->x() >= 0.0f);
		CHECK(pair->right() <= surfaceWidth);
		// THE KNOB IS DRIVE; THE SELECTOR IS THE TYPE; THE BOX IS SHAPE, a control of its own — so a Shape at its end
		// has nowhere to roll to.
		REQUIRE(knob->control() != nullptr);
		CHECK(knob->control()->desc().key == std::string_view("eroder_drive_amount"));
		CHECK(cell->select().control().desc().key == std::string_view("eroder_drive_mode"));
		CHECK(cell->activeParam() == static_cast<long>(eroAt(ero::Param::DriveShape)));
		REQUIRE(cell->boxFor(eroAt(ero::Param::DriveShape)) != nullptr);
		CHECK(cell->boxFor(eroAt(ero::Param::DriveShape))->isVisible());
		CHECK(cell->boxFor(eroAt(ero::Param::DriveShape))->control().desc().key == std::string_view("eroder_drive_shape"));
	};
	check(d.card->drivePairForTest(), d.card->driveForTest(), d.card->driveCellForTest(), d.card->width());
	check(panel->drivePairForTest(), panel->driveForTest(), panel->driveCellForTest(), panel->width());

	// SHAPE AT ITS TOP, PUSHED FURTHER: it stays at the top and the Type is what it was.
	d.set(eroAt(ero::Param::DriveMode), static_cast<float>(ero::DriveMode::Fold));
	ui::ValueBox* shape = d.card->driveCellForTest()->boxFor(eroAt(ero::Param::DriveShape));
	shape->control().beginGesture();
	shape->control().setValue(1.0);
	shape->control().setValue(1.7);
	shape->control().endGesture();
	CHECK(eroField(d, ero::Param::DriveShape) == 1.0f);
	CHECK(eroField(d, ero::Param::DriveMode) == static_cast<float>(ero::DriveMode::Fold));
	shape->control().beginGesture();
	shape->control().setValue(-0.4);
	shape->control().endGesture();
	CHECK(eroField(d, ero::Param::DriveShape) == 0.0f);
	CHECK(eroField(d, ero::Param::DriveMode) == static_cast<float>(ero::DriveMode::Fold));

	// THE PAIR IS EVERY ENGINE'S; FOLLOW AND HP WET ARE THE WINDOW'S.
	CHECK(panel->followForTest()->isVisible());
	const float cardPairX = d.card->drivePairForTest()->x();
	const float cardPairY = d.card->drivePairForTest()->y();
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	(void)d.card->updateContent();
	(void)panel->updateContent();
	CHECK(d.card->drivePairForTest()->isVisible());
	CHECK(panel->drivePairForTest()->isVisible());
	// ON THE CARD IT DOES NOT MOVE when the engine does.
	CHECK(d.card->drivePairForTest()->x() == cardPairX);
	CHECK(d.card->drivePairForTest()->y() == cardPairY);
	CHECK(panel->drivePairForTest()->right() <= panel->width());
	CHECK(panel->drivePairForTest()->bottom() <= panel->height());
	// AS IS HP WET; SUB THRU IS NOT DRAWN IN THE COMB WHILE BLEND IS OFF (its own case), and is with Blend on.
	CHECK(panel->hpWetForTest()->isVisible());
	CHECK_FALSE(panel->subThruForTest()->isVisible());
	d.set(eroAt(ero::Param::Blend), 1.0f);
	(void)panel->updateContent();
	CHECK(panel->subThruForTest()->isVisible());
	CHECK(panel->followForTest()->isVisible());
}
TEST_CASE("ERODER draws out of the signal what another control makes inert: Width with no noise, Sub Thru with no highpass") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)d.card->updateContent();
	(void)panel->updateContent();
	// AS DECLARED: Source is all noise, HP Wet is at the bottom.
	CHECK(d.card->widthForTest()->active());
	CHECK(panel->widthBoxForTest()->enabled());
	CHECK_FALSE(panel->subThruForTest()->enabled());

	d.set(eroAt(ero::Param::Source), 0.0f);
	d.set(eroAt(ero::Param::HpWet), 9.0f);
	(void)d.card->updateContent();
	(void)panel->updateContent();
	CHECK_FALSE(d.card->widthForTest()->active());
	CHECK_FALSE(panel->widthBoxForTest()->enabled());
	CHECK(panel->subThruForTest()->enabled());

	// DETUNE AND MOD FOLD ARE THE SINE'S: in the signal here (Source is all sine), out of it once it is all noise.
	CHECK(panel->knobForTest(ero::Param::Detune)->active());
	CHECK(panel->knobForTest(ero::Param::ModFold)->active());

	d.set(eroAt(ero::Param::Source), 0.01f);
	(void)d.card->updateContent();
	(void)panel->updateContent();
	CHECK(d.card->widthForTest()->active());
	CHECK(panel->widthBoxForTest()->enabled());
	d.set(eroAt(ero::Param::Source), 1.0f);
	(void)panel->updateContent();
	CHECK_FALSE(panel->knobForTest(ero::Param::Detune)->active());
	CHECK_FALSE(panel->knobForTest(ero::Param::ModFold)->active());

	// IN THE COMB THE DRY IS WHOLE, so with the highpass on Sub Thru still has nothing to give back — until Blend
	// crossfades the dry away.
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	(void)panel->updateContent();
	CHECK_FALSE(panel->subThruForTest()->enabled());
	d.set(eroAt(ero::Param::Blend), 1.0f);
	(void)panel->updateContent();
	CHECK(panel->subThruForTest()->enabled());
	d.set(eroAt(ero::Param::HpWet), static_cast<float>(ero::kHpWetLog2Min));
	(void)panel->updateContent();
	CHECK_FALSE(panel->subThruForTest()->enabled());
	// BLEND ON: the chip is drawn (greyed here, with HP Wet at its bottom), not hidden.
	CHECK(panel->subThruForTest()->isVisible());
}
TEST_CASE("ERODER's Sub Thru chip and its road are not drawn in the Comb while Blend is off, and are everywhere else") {
	const auto eroAtLocal = [](ero::Param p) { return static_cast<size_t>(p); };
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	const auto open = [&] {
		auto v = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
		auto* p = dynamic_cast<ui::EroderExpanded*>(v.get());
		REQUIRE(p != nullptr);
		p->setBounds(0.0f, 0.0f, p->minimumSize().x, p->minimumSize().y);
		(void)p->updateContent();
		return v;
	};
	// A FRESH WINDOW ON A STATE THAT IS ALREADY COMB + BLEND OFF: hidden from the first frame.
	d.set(eroAtLocal(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	d.set(eroAtLocal(ero::Param::Blend), 0.0f);
	d.set(eroAtLocal(ero::Param::HpWet), 9.0f);
	{
		auto fresh = open();
		CHECK_FALSE(dynamic_cast<ui::EroderExpanded*>(fresh.get())->subThruForTest()->isVisible());
	}
	auto view = open();
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	ui::EroderFlow& flow = *panel->flowForTest();
	flow.setRecordingForTest(true);
	// The road's top run is a horizontal segment on the anchor's row; its plate is a backing rectangle.
	const auto roadSegments = [&] {
		visage::Canvas canvas;
		flow.draw(canvas);
		const float y = flow.anchorForTest(ui::EroderFlow::Chip::SubThru).y;
		size_t n = 0;
		for (const auto& ev : flow.drawnForTest()) {
			n += (!ev.backing && ev.y0 == y && ev.y1 == y) ? 1u : 0u;
		}
		return n;
	};
	const auto platePresent = [&] {
		const ui::EroderFlow::Rect r = flow.chipRectForTest(ui::EroderFlow::Chip::SubThru);
		for (const auto& ev : flow.drawnForTest()) {
			if (ev.backing && std::abs(ev.x0 - r.x) < 0.01f && std::abs(ev.y0 - r.y) < 0.01f) {
				return true;
			}
		}
		return false;
	};
	// A PRESS AT THE CHIP'S PLACE, through the hit test, then the chip's own handler on whatever took it.
	const auto press = [&]() -> bool {
		ui::SwitchChip* chip = panel->subThruForTest();
		const visage::Point where { flow.x() + chip->x() + chip->width() * 0.5f, flow.y() + chip->y() + chip->height() * 0.5f };
		const bool reached = panel->frameAtPoint(where) == chip;
		if (reached) {
			chip->toggleForTest();
		}
		return reached;
	};
	const auto sub = [&] { return panel->subThruForTest()->control().value(); };
	const auto settle = [&] {
		(void)d.card->updateContent();
		(void)panel->updateContent();
	};
	struct Case {
		ero::Mode mode;
		bool blend;
		bool shown;
	};
	const Case cases[] = { { ero::Mode::Comb, false, false }, { ero::Mode::Comb, true, true }, { ero::Mode::Dust, false, true },
		                   { ero::Mode::Dust, true, true },   { ero::Mode::Rust, false, true }, { ero::Mode::Rust, true, true },
		                   { ero::Mode::Comb, false, false } };
	for (const Case& c : cases) {
		CAPTURE(static_cast<int>(c.mode));
		CAPTURE(c.blend);
		d.set(eroAtLocal(ero::Param::Mode), static_cast<float>(c.mode));
		d.set(eroAtLocal(ero::Param::Blend), c.blend ? 1.0f : 0.0f);
		d.set(eroAtLocal(ero::Param::SubThru), 0.0f);
		settle();
		CHECK(panel->subThruForTest()->isVisible() == c.shown);
		CHECK(flow.pathForTest().bypass.shown == c.shown);
		CHECK((roadSegments() > 0) == c.shown);
		CHECK(platePresent() == c.shown);
		// THE SEAT DOES NOT MOVE, shown or not.
		CHECK(flow.chipRectForTest(ui::EroderFlow::Chip::SubThru).w > 0.0f);
		const float before = sub();
		CHECK(press() == c.shown);
		CHECK((sub() != before) == c.shown);
	}
	// THE OTHER CHIPS' SEATS DO NOT JUMP WHEN IT HIDES.
	d.set(eroAtLocal(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	d.set(eroAtLocal(ero::Param::Blend), 1.0f);
	settle();
	const auto tapShown = flow.chipRectForTest(ui::EroderFlow::Chip::Tap);
	const auto subShown = flow.chipRectForTest(ui::EroderFlow::Chip::SubThru);
	d.set(eroAtLocal(ero::Param::Blend), 0.0f);
	settle();
	CHECK(flow.chipRectForTest(ui::EroderFlow::Chip::Tap).x == tapShown.x);
	CHECK(flow.chipRectForTest(ui::EroderFlow::Chip::SubThru).x == subShown.x);
}
TEST_CASE("ERODER's HP Wet cell is the hertz knob with Track off and the semitone knob with Track on, each writing its own parameter") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	ui::WidgetRegistry registry;
	auto view = ui::makeExpandedView(d.f.deviceHost, &d.f.bridge, registry, d.device, ero::kDescriptor);
	auto* panel = dynamic_cast<ui::EroderExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	panel->setBounds(0.0f, 0.0f, panel->minimumSize().x, panel->minimumSize().y);
	(void)panel->updateContent();
	// TRACK OFF: the hertz knob, and only it.
	CHECK(panel->hpWetForTest()->isVisible());
	CHECK_FALSE(panel->hpWetSemisForTest()->isVisible());
	// TRACK ON: the semitone knob takes the cell, the hertz knob is hidden (not greyed), in the same place.
	const float knobX = panel->hpWetForTest()->x();
	const float knobY = panel->hpWetForTest()->y();
	d.set(eroAt(ero::Param::Track), 1.0f);
	(void)panel->updateContent();
	CHECK_FALSE(panel->hpWetForTest()->isVisible());
	REQUIRE(panel->hpWetSemisForTest()->isVisible());
	CHECK(panel->hpWetSemisForTest()->x() == knobX);
	CHECK(panel->hpWetSemisForTest()->y() == knobY);
	// IT WRITES ITS OWN PARAMETER and reads in semitones, "off" at the bottom.
	CHECK(&panel->hpWetSemisForTest()->control()->desc() == &ero::kParamDescs[eroAt(ero::Param::HpWetSemis)]);
	CHECK(panel->hpWetSemisForTest()->control()->text() == "off");
	d.set(eroAt(ero::Param::HpWetSemis), 12.0f);
	(void)panel->updateContent();
	CHECK(panel->hpWetSemisForTest()->control()->text().find("12") == 0);
	CHECK(panel->hpWetSemisForTest()->control()->text().find("st") != std::string::npos);
	// SUB THRU FOLLOWS THE LIVE ONE: the semitones are on, so it is enabled; HP Wet's hertz (at the bottom) does not matter now.
	CHECK(panel->subThruForTest()->enabled());
	d.set(eroAt(ero::Param::HpWetSemis), static_cast<float>(ero::kHpWetSemisMin));
	d.set(eroAt(ero::Param::HpWet), 9.0f);
	(void)panel->updateContent();
	CHECK_FALSE(panel->subThruForTest()->enabled());
	// BOTH FACES ARE REACHABLE: the window offers each parameter, whatever Track holds.
	const std::vector<ero::Param> addressed = panel->addressedParamsForTest();
	CHECK(std::find(addressed.begin(), addressed.end(), ero::Param::HpWet) != addressed.end());
	CHECK(std::find(addressed.begin(), addressed.end(), ero::Param::HpWetSemis) != addressed.end());
	// TRACK BACK OFF: the hertz knob returns, and the hertz decides Sub Thru again.
	d.set(eroAt(ero::Param::Track), 0.0f);
	(void)panel->updateContent();
	CHECK(panel->hpWetForTest()->isVisible());
	CHECK_FALSE(panel->hpWetSemisForTest()->isVisible());
	CHECK(panel->subThruForTest()->enabled());
	// THE COMB TRACKS TOO: with Track on its HP Wet is the semitone knob, the Track chip is drawn, and Sub Thru still waits on Blend.
	d.set(eroAt(ero::Param::Track), 1.0f);
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Comb));
	d.set(eroAt(ero::Param::HpWetSemis), 12.0f);
	(void)panel->updateContent();
	CHECK(panel->trackForTest()->isVisible());
	CHECK_FALSE(panel->hpWetForTest()->isVisible());
	CHECK(panel->hpWetSemisForTest()->isVisible());
	CHECK_FALSE(panel->subThruForTest()->enabled()); // the dry is whole without Blend: nothing to give back
	d.set(eroAt(ero::Param::Blend), 1.0f);
	(void)panel->updateContent();
	CHECK(panel->subThruForTest()->enabled());
	// AND WITH TRACK OFF the hertz knob is back in the Comb.
	d.set(eroAt(ero::Param::Track), 0.0f);
	(void)panel->updateContent();
	CHECK(panel->hpWetForTest()->isVisible());
	CHECK_FALSE(panel->hpWetSemisForTest()->isVisible());
	CHECK(panel->trackForTest()->isVisible());
}
TEST_CASE("ERODER's Drive cell's third row is the Type's own — Hard, Sharp, Bias, Rate, Bal, or the Key pill: exactly one for EVERY Type, in one place — on the card and in the window") {
	EroFaces e;
	REQUIRE(ui::EroderCard::openHeight() <= ui::kCardUnitHeight);
	const auto cells = [&] { return std::vector<ui::ModeValueCell*> { e.d.card->driveCellForTest(), e.panel->driveCellForTest() }; };
	const ero::Param second[] = { ero::Param::DriveBal, ero::Param::DriveRate, ero::Param::DriveRateMult, ero::Param::DriveHard, ero::Param::DriveSharp, ero::Param::DriveBias };
	// THE CELL'S OWN SIZE AND ITS ROW PLACES DO NOT MOVE WITH THE TYPE: the shown second row sits where the third row sits.
	std::vector<float> cardSize;
	std::vector<float> shapeY;
	std::vector<float> secondY;
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb, ero::Mode::Etch }) {
		e.mode(mode);
		for (int type = 0; type < static_cast<int>(ero::DriveMode::kCount); ++type) {
			const auto dm = static_cast<ero::DriveMode>(type);
			CAPTURE(static_cast<int>(mode));
			CAPTURE(std::string(ero::kDriveModeNames[type]));
			e.set(ero::Param::Drive, 0.5f);
			e.set(ero::Param::DriveMode, static_cast<float>(type));
			for (ui::ModeValueCell* cell : cells()) {
				int shown = 0;
				for (const ero::Param p : second) {
					ui::ValueBox* box = cell->boxFor(eroAt(p));
					REQUIRE(box != nullptr);
					shown += box->isVisible() ? 1 : 0;
				}
				ui::KeySpreadRow* pill = cell->pillFor(eroAt(ero::Param::DriveKey));
				REQUIRE(pill != nullptr);
				shown += pill->isVisible() ? 1 : 0;
				const ero::SecondParam want = ero::driveSecondParam(dm);
				// EVERY TYPE USES THE THIRD ROW, and exactly one thing is in it (the others hidden, not greyed).
				CHECK(want != ero::SecondParam::None);
				CHECK(shown == 1);
				const auto labelOf = [&](ero::Param p) { return cell->boxFor(eroAt(p))->label(); };
				CHECK(cell->boxFor(eroAt(ero::Param::DriveBal))->isVisible() == (want == ero::SecondParam::Bal));
				CHECK(cell->boxFor(eroAt(ero::Param::DriveHard))->isVisible() == (want == ero::SecondParam::Hard));
				CHECK(cell->boxFor(eroAt(ero::Param::DriveSharp))->isVisible() == (want == ero::SecondParam::Sharp));
				CHECK(cell->boxFor(eroAt(ero::Param::DriveBias))->isVisible() == (want == ero::SecondParam::Bias));
				CHECK(cell->boxFor(eroAt(ero::Param::DriveRate))->isVisible() == (want == ero::SecondParam::Rate)); // Track is off
				CHECK(pill->isVisible() == (want == ero::SecondParam::Key));
				if (want == ero::SecondParam::Bal) {
					CHECK(labelOf(ero::Param::DriveBal) == "Bal");
				}
				if (want == ero::SecondParam::Hard) {
					const std::string label = labelOf(ero::Param::DriveHard);
					CHECK((label == "Hard" || label == "Hrd"));
				}
				if (want == ero::SecondParam::Sharp) {
					const std::string label = labelOf(ero::Param::DriveSharp);
					CHECK((label == "Sharp" || label == "Shp"));
				}
				if (want == ero::SecondParam::Bias) {
					CHECK(labelOf(ero::Param::DriveBias) == "Bias");
				}
				if (want == ero::SecondParam::Key) {
					CHECK(pill->chip().caption() == "Key");
					CHECK(pill->fader().caption() == "Spr");
					CHECK(pill->bottom() <= static_cast<float>(cell->height()));
					if (cell == cells().front()) {
						secondY.push_back(pill->y());
					}
				}
				if (want == ero::SecondParam::Rate) {
					CHECK(cell->boxFor(eroAt(ero::Param::DriveRate))->isVisible()); // Track is off
					// (the card's narrow cell labels the row "Rt", the window's "Rate": the two would overlap their value in a fifth of a row)
					const std::string label = cell->boxFor(eroAt(ero::Param::DriveRate))->label();
					CHECK((label == "Rate" || label == "Rt"));
				}
				// THE S ROW IS ONE PLACE: Shape for every Type but the four Paunch Types, whose Angle (a key of its own) takes it.
				const bool angled = ero::driveIsPaunchLaw(dm) || ero::driveIsPaunchOdd(dm);
				ui::ValueBox* sh = cell->boxFor(eroAt(angled ? ero::Param::DriveAngle : ero::Param::DriveShape));
				ui::ValueBox* other = cell->boxFor(eroAt(angled ? ero::Param::DriveShape : ero::Param::DriveAngle));
				CHECK(sh->isVisible());
				CHECK_FALSE(other->isVisible());
				CHECK(sh->label() == "S");
				if (pill->isVisible()) {
					CHECK(pill->y() > sh->y());
				}
				for (const ero::Param p : second) {
					ui::ValueBox* box = cell->boxFor(eroAt(p));
					if (box->isVisible()) {
						CHECK(box->y() > sh->y());
						CHECK(box->bottom() <= static_cast<float>(cell->height()));
						if (cell == cells().front()) {
							secondY.push_back(box->y());
						}
					}
				}
				if (cell == cells().front()) {
					cardSize.push_back(static_cast<float>(cell->height()));
					shapeY.push_back(sh->y());
				}
			}
			CHECK(ui::EroderCard::openHeight() <= ui::kCardUnitHeight);
		}
	}
	// NOTHING SHIFTS BETWEEN TYPES: one cell height, one Shape row, one place for whichever second row is shown.
	for (const float h : cardSize) {
		CHECK(h == cardSize.front());
	}
	for (const float y : shapeY) {
		CHECK(y == shapeY.front());
	}
	for (const float y : secondY) {
		CHECK(y == secondY.front());
	}
	CHECK_FALSE(secondY.empty());
	// EACH ROW WRITES ITS OWN PARAMETER, on either surface.
	for (ui::ModeValueCell* cell : cells()) {
		e.set(ero::Param::Mode, static_cast<float>(ero::Mode::Dust));
		e.set(ero::Param::DriveMode, static_cast<float>(ero::DriveMode::Chonk));
		ui::ValueBox* bal = cell->boxFor(eroAt(ero::Param::DriveBal));
		bal->control().beginGesture();
		bal->control().setValue(0.4);
		bal->control().endGesture();
		CHECK(eroField(e.d, ero::Param::DriveBal) == doctest::Approx(0.4f));
		CHECK(bal->control().desc().key == std::string_view("eroder_drive_bal"));
		e.set(ero::Param::DriveBal, 1.0f);
		e.set(ero::Param::DriveMode, static_cast<float>(ero::DriveMode::PaunchS));
		ui::ValueBox* rate = cell->boxFor(eroAt(ero::Param::DriveRate));
		rate->control().beginGesture();
		rate->control().setValue(1.0);
		rate->control().endGesture();
		CHECK(eroField(e.d, ero::Param::DriveRate) == doctest::Approx(1.0f));
		CHECK(rate->control().desc().key == std::string_view("eroder_drive_rate"));
	}
}
TEST_CASE("ERODER's Key: the Amount knob reads the note's multiple while the switch is on, the pill's fader is live with the switch off, in the Drive and both slots, card and window") {
	EroFaces e;
	e.mode(ero::Mode::Etch);
	ui::EtchSlots& card = e.d.card->etchSlotsForTest();
	ui::EtchSlots& panel = e.panel->etchSlotsForTest();
	struct Place {
		ero::Param type, amount, key, spread;
		ui::Knob* knobs[2];
		ui::ModeValueCell* cells[2];
	};
	const Place places[] = {
		{ ero::Param::DriveMode, ero::Param::Drive, ero::Param::DriveKey, ero::Param::DriveSpread, { e.d.card->driveForTest(), e.panel->driveForTest() },
		  { e.d.card->driveCellForTest(), e.panel->driveCellForTest() } },
		{ ero::Param::SlotAType, ero::Param::SlotAAmount, ero::Param::SlotAKey, ero::Param::SlotASpread, { card.knobA.get(), panel.knobA.get() },
		  { card.cellA.get(), panel.cellA.get() } },
		{ ero::Param::SlotBType, ero::Param::SlotBAmount, ero::Param::SlotBKey, ero::Param::SlotBSpread, { card.knobB.get(), panel.knobB.get() },
		  { card.cellB.get(), panel.cellB.get() } },
	};
	for (const Place& p : places) {
		for (const ero::DriveMode type : { ero::DriveMode::Crush, ero::DriveMode::Decimate, ero::DriveMode::Slew, ero::DriveMode::Lopside, ero::DriveMode::Soft }) {
			CAPTURE(std::string(ero::kDriveModeNames[static_cast<size_t>(type)]));
			e.set(p.type, static_cast<float>(type));
			e.set(p.amount, 0.5f);
			e.set(p.key, 0.0f);
			e.refresh();
			for (ui::Knob* knob : p.knobs) {
				const std::string plain = knob->printedText();
				CHECK(plain.find("\xC3\x97") == std::string::npos);
				e.set(p.key, 1.0f);
				e.refresh();
				if (ero::driveHasKey(type)) {
					// THE MULTIPLE, restated: 32 * (1/4 / 32)^0.5 = 2.83
					CHECK(knob->printedText() == "\xC3\x97" "2.83");
				}
				else {
					CHECK(knob->printedText() == plain); // another Type: the switch is not read, and the text is the control's own
				}
				e.set(p.key, 0.0f);
				e.refresh();
				CHECK(knob->printedText() == plain);
			}
			if (!ero::driveHasKey(type)) {
				continue;
			}
			// THE FADER IS LIVE WITH THE SWITCH OFF: the row is shown, the chip is off, and the fader's drag writes the Spread.
			for (ui::ModeValueCell* cell : p.cells) {
				ui::KeySpreadRow* pill = cell->pillFor(eroAt(p.key));
				REQUIRE(pill != nullptr);
				CHECK(pill->isVisible());
				CHECK_FALSE(pill->chip().on());
				e.set(p.spread, 0.0f);
				pill->fader().amountForTest().beginGesture();
				pill->fader().amountForTest().setValue(0.6);
				pill->fader().amountForTest().endGesture();
				CHECK(eroField(e.d, p.spread) == doctest::Approx(0.6f));
				CHECK(pill->fader().amountOnly());
				CHECK(pill->chip().control().desc().key == ero::kParamDescs[eroAt(p.key)].key);
			}
		}
	}
}
TEST_CASE("ERODER's Key chip and Spread fader are two separate controls on the third row: the chip writes only Key, the fader only Spread and stays lit with Key off, the row is there for the four Types only, and it stays inside its cell at scale 1 and 1.75") {
	EroFaces e;
	struct Unit {
		const char* name;
		ero::Param type, key, spread;
		bool narrow;
		bool slot;
		ui::ModeValueCell* cell;
	};
	ui::EtchSlots& card = e.d.card->etchSlotsForTest();
	ui::EtchSlots& panel = e.panel->etchSlotsForTest();
	const Unit units[] = {
		{ "card Drive", ero::Param::DriveMode, ero::Param::DriveKey, ero::Param::DriveSpread, true, false, e.d.card->driveCellForTest() },
		{ "window Drive", ero::Param::DriveMode, ero::Param::DriveKey, ero::Param::DriveSpread, false, false, e.panel->driveCellForTest() },
		{ "card A", ero::Param::SlotAType, ero::Param::SlotAKey, ero::Param::SlotASpread, true, true, card.cellA.get() },
		{ "card B", ero::Param::SlotBType, ero::Param::SlotBKey, ero::Param::SlotBSpread, true, true, card.cellB.get() },
		{ "window A", ero::Param::SlotAType, ero::Param::SlotAKey, ero::Param::SlotASpread, false, true, panel.cellA.get() },
		{ "window B", ero::Param::SlotBType, ero::Param::SlotBKey, ero::Param::SlotBSpread, false, true, panel.cellB.get() },
	};
	e.mode(ero::Mode::Etch);
	e.set(ero::Param::Drive, 0.5f);
	for (const Unit& u : units) {
		CAPTURE(u.name);
		ui::KeySpreadRow* row = u.cell->pillFor(eroAt(u.key));
		REQUIRE(row != nullptr);
		// THE ROW IS THERE FOR THE FOUR TYPES ONLY.
		for (int type = 0; type < static_cast<int>(ero::DriveMode::kCount); ++type) {
			CAPTURE(std::string(ero::kDriveModeNames[type]));
			e.set(u.type, static_cast<float>(type));
			CHECK(row->isVisible() == ero::driveHasKey(static_cast<ero::DriveMode>(type)));
		}
		e.set(u.type, static_cast<float>(ero::DriveMode::Decimate));
		REQUIRE(row->isVisible());
		// TWO WIDGETS, each over its own control.
		CHECK(row->chip().control().desc().key == ero::kParamDescs[eroAt(u.key)].key);
		CHECK(row->fader().amountForTest().desc().key == ero::kParamDescs[eroAt(u.spread)].key);
		CHECK(row->fader().amountOnly());
		CHECK(row->chip().caption() == "Key");
		CHECK(row->fader().caption() == "Spr");
		// THE CHIP TOGGLES KEY AND LEAVES SPREAD ALONE.
		e.set(u.key, 0.0f);
		e.set(u.spread, 0.0f);
		const float cx = row->chip().width() * 0.5f;
		const float cy = row->chip().height() * 0.5f;
		row->chip().onMouseDown().callback(at(cx, cy));
		row->chip().onMouseUp().callback(at(cx, cy));
		CHECK(eroField(e.d, u.key) == 1.0f);
		CHECK(eroField(e.d, u.spread) == 0.0f);
		row->chip().onMouseDown().callback(at(cx, cy));
		row->chip().onMouseUp().callback(at(cx, cy));
		CHECK(eroField(e.d, u.key) == 0.0f);
		// WITH KEY OFF THE FADER IS LIT AND ITS DRAG WRITES SPREAD (40%), NOT KEY.
		CHECK_FALSE(row->chip().on());
		CHECK(row->fader().active());
		const float fx = row->fader().width() * 0.5f;
		const float fy = row->fader().height() * 0.5f;
		const float travel = 0.4f * ui::FaderPill::kSpan;
		row->fader().onMouseDown().callback(at(fx, fy));
		row->fader().onMouseDrag().callback(at(fx + travel, fy));
		row->fader().onMouseUp().callback(at(fx + travel, fy));
		CHECK(eroField(e.d, u.spread) == doctest::Approx(0.4f));
		CHECK(eroField(e.d, u.key) == 0.0f);
		// A CLICK ON THE FADER DOES NOT TOGGLE KEY (it has no power of its own).
		row->fader().onMouseDown().callback(at(fx, fy));
		row->fader().onMouseUp().callback(at(fx, fy));
		CHECK(eroField(e.d, u.key) == 0.0f);
		CHECK(row->fader().active());
	}
	// NATIVE WIDTHS: nothing in the row leaves the cell, the two controls do not overlap, at scale 1 and at the development display's 1.75.
	for (const float scale : { 1.0f, 1.75f }) {
		e.d.card->setDpiScale(scale);
		e.panel->setDpiScale(scale);
		for (const Unit& u : units) {
			CAPTURE(u.name);
			CAPTURE(scale);
			e.set(u.type, static_cast<float>(ero::DriveMode::Slew));
			ui::KeySpreadRow* row = u.cell->pillFor(eroAt(u.key));
			REQUIRE(row->isVisible());
			REQUIRE(u.cell->dpiScale() == scale);
			CHECK(row->nativeX() >= 0);
			CHECK(row->nativeRight() <= u.cell->nativeWidth());
			CHECK(row->nativeBottom() <= u.cell->nativeHeight());
			CHECK(row->chip().nativeX() >= 0);
			CHECK(row->chip().nativeRight() <= row->fader().nativeX());
			CHECK(row->fader().nativeRight() <= row->nativeWidth());
			CHECK(row->chip().nativeBottom() <= row->nativeHeight());
			// THE FADER HAS ROOM FOR ITS CAPTION (the window prints the percentage too, the narrow cell its fill alone).
			CHECK(row->fader().nativeWidth() >= static_cast<int>(std::lround(14.0f * scale)));
			if (u.narrow) {
				CHECK(row->fader().captionFits());
			}
		}
	}
	e.d.card->setDpiScale(1.0f);
	e.panel->setDpiScale(1.0f);
}
TEST_CASE("ERODER's Rate row follows Track: hertz with it off, the multiple of the note with it on, the other hidden; the multiple's drag lands on the detents and a typed value stays") {
	EroFaces e;
	e.mode(ero::Mode::Dust);
	e.set(ero::Param::Drive, 0.5f);
	e.set(ero::Param::DriveMode, static_cast<float>(ero::DriveMode::PaunchOS));
	for (const bool track : { false, true }) {
		e.set(ero::Param::Track, track ? 1.0f : 0.0f);
		for (ui::ModeValueCell* cell : { e.d.card->driveCellForTest(), e.panel->driveCellForTest() }) {
			CHECK(cell->boxFor(eroAt(ero::Param::DriveRate))->isVisible() == !track);
			CHECK(cell->boxFor(eroAt(ero::Param::DriveRateMult))->isVisible() == track);
		}
	}
	// THE MULTIPLE'S TEXT: off at 0, no trailing zeros, up to three decimals.
	CHECK(ui::rateMultText(0.0) == "off");
	CHECK(ui::rateMultText(0.5) == "0.5");
	CHECK(ui::rateMultText(0.25) == "0.25");
	CHECK(ui::rateMultText(1.0 / 3.0) == "0.333");
	CHECK(ui::rateMultText(0.0625) == "0.0625");
	CHECK(ui::rateMultText(1.0 / 12.0) == "0.0833");
	CHECK(ui::rateMultText(1.0) == "1");
	ui::ValueBox* mult = e.panel->driveCellForTest()->boxFor(eroAt(ero::Param::DriveRateMult));
	REQUIRE(mult != nullptr);
	CHECK(mult->readout() == "0.5"); // the default
	// A TYPED VALUE IS KEPT AS TYPED (the control is continuous): 0.37 is stored as 0.37.
	mult->control().beginGesture();
	mult->control().setValue(0.37);
	mult->control().endGesture();
	CHECK(eroField(e.d, ero::Param::DriveRateMult) == doctest::Approx(0.37f));
	CHECK(mult->readout() == "0.37");
	// THE DRAG: every plain drag, from several starting values and distances, lands on a detent and nowhere between.
	const std::vector<double>& detents = mult->detents();
	REQUIRE(detents.size() == 10);
	for (const double start : { 0.0, 0.2, 0.5, 0.9 }) {
		for (const float travel : { 3.0f, 9.0f, 17.0f, 31.0f, 55.0f, -4.0f, -13.0f, -40.0f }) {
			mult->control().beginGesture();
			mult->control().setValue(start);
			mult->control().endGesture();
			const float x = 10.0f;
			const float y0 = 40.0f;
			mult->onMouseDown().callback(at(x, y0));
			mult->onMouseDrag().callback(at(x, y0 - travel));
			mult->onMouseUp().callback(at(x, y0 - travel));
			const double v = mult->control().value();
			bool onDetent = false;
			for (const double d : detents) {
				onDetent = onDetent || std::abs(d - v) < 1.0e-6; // (the control holds a float)
			}
			CAPTURE(start);
			CAPTURE(travel);
			CHECK(onDetent);
		}
	}
}
TEST_CASE("ERODER's DC block is a chip in the window's topology picture, beside the dc stage, greyed unless a running stage makes an offset, and not on the card") {
	EroFaces e;
	e.mode(ero::Mode::Dust);
	ui::SwitchChip* dc = e.panel->dcForTest();
	REQUIRE(dc != nullptr);
	CHECK(dc->control().desc().key == std::string_view("eroder_dc_block"));
	CHECK(dc->parent() == e.panel->flowForTest());
	// SEATED IN THE PICTURE, over its own stage (the dc box).
	const auto path = ui::EroderFlow::pathFor(ero::rowOf(ero::State {}));
	(void)path;
	// GREYED UNLESS A STAGE THAT IS RUN MAKES AN OFFSET.
	e.set(ero::Param::Drive, 0.5f);
	e.set(ero::Param::DriveMode, static_cast<float>(ero::DriveMode::Soft));
	CHECK_FALSE(dc->enabled());
	for (const ero::DriveMode type : { ero::DriveMode::Tube, ero::DriveMode::Fold, ero::DriveMode::Lean, ero::DriveMode::PaunchS, ero::DriveMode::PaunchH }) {
		CAPTURE(static_cast<int>(type));
		e.set(ero::Param::DriveMode, static_cast<float>(type));
		CHECK(dc->enabled());
	}
	e.set(ero::Param::Drive, 0.0f);
	CHECK_FALSE(dc->enabled());
	e.set(ero::Param::Drive, 0.5f);
	e.set(ero::Param::DriveMode, static_cast<float>(ero::DriveMode::Chonk));
	CHECK_FALSE(dc->enabled());
	// AN ETCH SLOT THAT IS RUN MAKES ONE TOO.
	e.set(ero::Param::SlotAAmount, 0.6f);
	e.set(ero::Param::SlotAType, static_cast<float>(ero::DriveMode::PaunchS));
	CHECK_FALSE(dc->enabled());
	e.mode(ero::Mode::Etch);
	CHECK(dc->enabled());
	// IT WRITES eroder_dc_block.
	e.set(ero::Param::DcBlock, 1.0f);
	dc->control().beginGesture();
	dc->control().setValue(0.0);
	dc->control().endGesture();
	CHECK(eroField(e.d, ero::Param::DcBlock) == 0.0f);
	e.set(ero::Param::DcBlock, 1.0f);
	// THE PICTURE'S dc BOX IS LIT EXACTLY WHEN THE BLOCK RUNS: On and a stage that makes an offset.
	const auto lit = [](const ero::Settings& st) {
		const ui::EroderFlow::Path p = ui::EroderFlow::pathFor(ero::rowOf(ero::State { st }));
		for (const auto& stage : p.stages) {
			if (stage.label == "dc") {
				return stage.lit;
			}
		}
		return false;
	};
	ero::Settings st;
	st.drive = 0.5f;
	st.driveMode = ero::DriveMode::Tube;
	CHECK(lit(st));
	st.dc = 0.0f; // Off
	CHECK_FALSE(lit(st));
	st.dc = 1.0f;
	st.driveMode = ero::DriveMode::Soft;
	CHECK_FALSE(lit(st));
}
TEST_CASE("ERODER's card offers the shared section in every engine, HP Wet and Follow are the window's, and the card stays inside its height") {
	EroFaces e;
	auto& card = *e.d.card;
	REQUIRE(ui::EroderCard::openHeight() <= ui::kCardUnitHeight);
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb }) {
		CAPTURE(static_cast<int>(mode));
		e.mode(mode);
		const bool comb = mode == ero::Mode::Comb;
		std::vector<visage::Frame*> want = { card.mixForTest(), card.drivePairForTest(), card.emphasisForTest(), card.trackForTest() };
		if (comb) {
			for (visage::Frame* f : card.combWidgetsForTest()) {
				want.push_back(f);
			}
		}
		else {
			for (visage::Frame* f : card.shakerWidgetsForTest()) {
				want.push_back(f);
			}
			for (visage::Frame* f : mode == ero::Mode::Rust ? card.rustWidgetsForTest() : card.dustWidgetsForTest()) {
				want.push_back(f);
			}
			want.push_back(card.resonanceForTest());
		}
		const auto shown = eroVisible(&card, { card.plotForTest(), card.modeForTest() });
		CHECK(eroSameSet(shown, want));
		eroApart(shown, card.width(), ui::EroderCard::openHeight());
		// THE FEEDBACK CELL IS WHERE THE FOURTH COLUMN IS, in either of its two forms, and Emphasis beside it.
		visage::Frame* feedback = comb ? static_cast<visage::Frame*>(card.loopForTest())
		                               : static_cast<visage::Frame*>(card.resonanceForTest());
		CHECK(feedback->isVisible());
		CHECK(feedback->x() > card.emphasisForTest()->x());
		CHECK(feedback->y() == card.emphasisForTest()->y());
		CHECK(feedback->bottom() <= ui::EroderCard::openHeight());
		// THE KNOBS ON THE CARD ARE THE RIGHT CONTROLS.
		CHECK(card.emphasisForTest()->control()->desc().key == std::string_view("eroder_emph_amount"));
		const ui::Knob* feedbackKnob = comb ? card.loopForTest() : card.resonanceForTest();
		CHECK(feedbackKnob->control()->desc().key == std::string_view(comb ? "eroder_loop" : "eroder_resonance"));
		// HP WET AND FOLLOW ARE NOT ON THE CARD ANYWHERE, and are in the window.
		std::vector<ui::Knob*> knobs;
		collectKnobs(&card, &knobs);
		for (const ui::Knob* knob : knobs) {
			const std::string_view key = knob->control()->desc().key;
			CHECK(key != std::string_view(ero::kParamDescs[eroAt(ero::Param::HpWet)].key));
			CHECK(key != std::string_view(ero::kParamDescs[eroAt(ero::Param::Follow)].key));
		}
		CHECK(e.panel->hpWetForTest()->isVisible());
		CHECK(e.panel->followForTest()->isVisible());
	}
	// THE COMB'S SMALL ROW: Gamma, then the two chips, side by side.
	e.mode(ero::Mode::Comb);
	CHECK(card.tapForTest()->control().desc().key == std::string_view("eroder_fb_tap"));
	CHECK(card.restoreForTest()->control().desc().key == std::string_view("eroder_emph_restore"));
	CHECK(card.tapForTest()->y() == card.restoreForTest()->y());
	CHECK(card.tapForTest()->right() <= card.restoreForTest()->x());
	// AND THE SHAKERS' SMALL ROW HAS NEITHER.
	e.mode(ero::Mode::Dust);
	CHECK_FALSE(card.tapForTest()->isVisible());
	CHECK_FALSE(card.restoreForTest()->isVisible());
}
TEST_CASE("ERODER's window offers every shared-section control in every engine, its pairs whole, nothing overlapping") {
	EroFaces e;
	auto& panel = *e.panel;
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust, ero::Mode::Comb }) {
		CAPTURE(static_cast<int>(mode));
		e.mode(mode);
		const bool comb = mode == ero::Mode::Comb;
		const auto visible = [](visage::Frame* f) { return f != nullptr && f->isVisible(); };
		// THE SMEAR IS A STACK (Amount, Stages and Reso side by side, Offset), not a knob.
		CHECK(visible(panel.smearStackForTest()));
		CHECK(panel.knobForTest(ero::Param::Smear) == nullptr);
		CHECK(visible(panel.emphasisStackForTest()));
		CHECK(visible(panel.filterForTest()));
		CHECK(visible(panel.loopPairForTest()));
		CHECK(visible(panel.emphasisAmountForTest()));
		CHECK(visible(panel.shapeForTest()));
		CHECK(visible(panel.knobForTest(ero::Param::Loop)));
		CHECK(visible(panel.knobForTest(ero::Param::LoopInterval)));
		CHECK(visible(panel.restoreForTest()));
		CHECK(visible(panel.inLoopForTest()));
		CHECK(visible(panel.tapForTest()));
		// RESONANCE IS RUST'S AS WELL AS DUST'S; THE COMB'S BELL HAS ITS WIDTH.
		CHECK(visible(panel.knobForTest(ero::Param::Resonance)) == !comb);
		CHECK(visible(panel.widthBoxForTest()));
		CHECK(visible(panel.freqBoxForTest()) == !comb);
		CHECK(visible(panel.hpWetForTest()));
		CHECK(visible(panel.followForTest()));
		// THE PAIRS ARE WHOLE: both halves inside the pair, the pair two cells wide, on one row.
		const float cell = ui::EroderExpanded::kSideWidth / static_cast<float>(ui::EroderExpanded::kPerRow);
		for (ui::PairCell* pair : { panel.loopPairForTest() }) {
			CHECK(pair->width() == doctest::Approx(2.0f * cell));
			CHECK(pair->first().parent() == pair);
			CHECK(pair->second().parent() == pair);
			CHECK(pair->first().right() <= pair->second().x());
			CHECK(pair->first().y() == pair->second().y());
		}
		// BARE PAIRS: the group they sit in names them.
		CHECK(panel.loopPairForTest()->bare());
		CHECK(panel.shapeForTest()->parent() == panel.emphasisStackForTest());
		// NOTHING OVERLAPS AT THE WINDOW'S OWN WIDTH, counting what is inside the pairs.
		std::vector<visage::Frame*> flat;
		for (visage::Frame* child : eroVisible(&panel, { panel.plotForTest(), panel.modeForTest() })) {
			if (auto* pair = dynamic_cast<ui::PairCell*>(child)) {
				// A PAIR IS ONE BOX AT THIS LEVEL (its halves are checked above).
				flat.push_back(pair);
			}
			else {
				flat.push_back(child);
			}
		}
		eroApart(flat, panel.width(), panel.height());
	}
}
TEST_CASE("ERODER's shared controls say what they do not change: Width, Restore, Shape, Tap, Interval and In Loop on both faces") {
	EroFaces e;
	auto& card = *e.d.card;
	auto& panel = *e.panel;
	using ero::Param;
	// ---- WIDTH: the band's with a noise source, or the Bell's with an emphasis ----
	e.mode(ero::Mode::Dust);
	e.set(Param::Source, 0.0f);
	CHECK_FALSE(card.widthForTest()->active());
	CHECK_FALSE(panel.widthBoxForTest()->enabled());
	e.set(Param::EmphAmount, 9.0f); // a Tilt has no width
	CHECK_FALSE(card.widthForTest()->active());
	CHECK_FALSE(panel.widthBoxForTest()->enabled());
	e.set(Param::EmphShape, 1.0f); // Bell
	CHECK(card.widthForTest()->active());
	CHECK(panel.widthBoxForTest()->enabled());
	e.set(Param::EmphAmount, 0.0f); // a Bell of nothing has none either
	CHECK_FALSE(card.widthForTest()->active());
	CHECK_FALSE(panel.widthBoxForTest()->enabled());
	e.set(Param::Source, 1.0f); // and the noise band still has it
	CHECK(card.widthForTest()->active());
	CHECK(panel.widthBoxForTest()->enabled());
	// THE COMB HAS NO BAND: Source is not its business, the Bell is.
	e.mode(ero::Mode::Comb);
	e.set(Param::EmphShape, 0.0f);
	e.set(Param::EmphAmount, 9.0f);
	CHECK_FALSE(panel.widthBoxForTest()->enabled());
	e.set(Param::EmphShape, 1.0f);
	CHECK(panel.widthBoxForTest()->enabled());
	e.set(Param::EmphAmount, 0.0f);
	CHECK_FALSE(panel.widthBoxForTest()->enabled());

	// ---- RESTORE and SHAPE: nothing at Emphasis 0 ----
	CHECK_FALSE(card.restoreForTest()->enabled());
	CHECK_FALSE(panel.restoreForTest()->enabled());
	CHECK_FALSE(panel.emphasisOffsetForTest()->enabled());
	CHECK(panel.emphasisAmountForTest()->enabled()); // the amount is the way out of zero
	e.set(Param::EmphAmount, -6.0f); // either sign is an emphasis
	CHECK(card.restoreForTest()->enabled());
	CHECK(panel.restoreForTest()->enabled());
	CHECK(panel.emphasisOffsetForTest()->enabled());

	// ---- TAP: the Comb's inner loop never stops; Dust's and Rust's is Resonance, of either sign ----
	CHECK(card.tapForTest()->enabled());
	CHECK(panel.tapForTest()->enabled());
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust }) {
		CAPTURE(static_cast<int>(mode));
		e.mode(mode);
		e.set(Param::Resonance, 0.0f);
		CHECK_FALSE(panel.tapForTest()->enabled());
		e.set(Param::Resonance, 0.5f);
		CHECK(panel.tapForTest()->enabled());
		e.set(Param::Resonance, -0.5f);
		CHECK(panel.tapForTest()->enabled());
		e.set(Param::Resonance, 0.0f);
	}

	// ---- INTERVAL: the outer loop's, so nothing at Loop 0 ----
	e.set(Param::Loop, 0.0f);
	CHECK_FALSE(panel.knobForTest(Param::LoopInterval)->active());
	e.set(Param::Loop, 0.4f);
	CHECK(panel.knobForTest(Param::LoopInterval)->active());
	e.set(Param::Loop, 0.0f);

	// ---- IN LOOP: an emphasis AND a loop (the inner one, or the outer) ----
	for (const ero::Mode mode : { ero::Mode::Dust, ero::Mode::Rust }) {
		CAPTURE(static_cast<int>(mode));
		e.mode(mode);
		e.set(Param::EmphAmount, 0.0f);
		e.set(Param::Resonance, 0.5f);
		CHECK_FALSE(panel.inLoopForTest()->enabled()); // a loop, no emphasis
		e.set(Param::EmphAmount, 9.0f);
		CHECK(panel.inLoopForTest()->enabled());       // inner loop
		e.set(Param::Resonance, 0.0f);
		CHECK_FALSE(panel.inLoopForTest()->enabled()); // an emphasis, no loop
		e.set(Param::Loop, 0.5f);
		CHECK(panel.inLoopForTest()->enabled());       // outer loop
		e.set(Param::Loop, 0.0f);
	}
	e.mode(ero::Mode::Comb);
	e.set(Param::EmphAmount, 0.0f);
	CHECK_FALSE(panel.inLoopForTest()->enabled());
	e.set(Param::EmphAmount, 9.0f);
	CHECK(panel.inLoopForTest()->enabled()); // the Comb's own loop is always running
}
TEST_CASE("ERODER's new controls write their parameter from a gesture and carry their tooltip, on the card and in the window") {
	EroFaces e;
	using ero::Param;
	const auto desc = [](Param p) -> const device::ParamDesc& { return ero::kParamDescs[eroAt(p)]; };
	const auto knobWrites = [&](ui::Knob* knob, Param p, double value) {
		REQUIRE(knob != nullptr);
		CAPTURE(desc(p).key);
		REQUIRE(knob->control() != nullptr);
		CHECK(std::string(knob->tooltipAt({ 2.0f, 2.0f })) == desc(p).tooltip);
		knob->control()->beginGesture();
		knob->control()->setValue(value);
		knob->control()->endGesture();
		e.refresh();
		CHECK(eroField(e.d, p) == doctest::Approx(static_cast<float>(value)));
	};
	const auto chipWrites = [&](ui::SwitchChip* chip, Param p) {
		REQUIRE(chip != nullptr);
		CAPTURE(desc(p).key);
		REQUIRE(chip->enabled());
		CHECK(std::string(chip->tooltipAt({ 2.0f, 2.0f })) == desc(p).tooltip);
		const float before = eroField(e.d, p);
		chip->toggleForTest();
		e.refresh();
		CHECK(eroField(e.d, p) == 1.0f - before);
	};
	// A STATE IN WHICH EVERYTHING IS LIVE.
	e.mode(ero::Mode::Dust);
	e.set(Param::EmphAmount, 6.0f);
	e.set(Param::Resonance, 0.4f);
	e.set(Param::Loop, 0.4f);
	// THE WINDOW'S.
	// THE EMPHASIS AMOUNT IS A STACKED FIELD NOW: its own control, written through the box.
	{
		ui::ValueBox* box = e.panel->emphasisAmountForTest();
		CHECK(std::string(box->tooltipAt({ 2.0f, 2.0f })) == desc(Param::EmphAmount).tooltip);
		box->control().beginGesture();
		box->control().setValue(12.0);
		box->control().endGesture();
		e.refresh();
		CHECK(eroField(e.d, Param::EmphAmount) == doctest::Approx(12.0f));
	}
	// THE SMEAR'S THREE FIELDS write their own parameter and carry their own tooltip, each with the condition it is inactive under.
	for (const auto& [box, param, value] : { std::tuple { e.panel->smearAmountForTest(), Param::Smear, 0.7 },
	                                          std::tuple { e.panel->smearStagesForTest(), Param::SmearStages, 13.0 },
	                                          std::tuple { e.panel->smearResoForTest(), Param::SmearReso, 20.0 } }) {
		CAPTURE(desc(param).key);
		REQUIRE(box != nullptr);
		CHECK(std::string(box->tooltipAt({ 2.0f, 2.0f })) == desc(param).tooltip);
		box->control().beginGesture();
		box->control().setValue(value);
		box->control().endGesture();
		e.refresh();
		CHECK(eroField(e.d, param) == doctest::Approx(static_cast<float>(value)));
	}
	knobWrites(e.panel->knobForTest(Param::Loop), Param::Loop, 0.6);
	knobWrites(e.panel->knobForTest(Param::LoopInterval), Param::LoopInterval, 7.0);
	knobWrites(e.panel->knobForTest(Param::Resonance), Param::Resonance, -0.3);
	chipWrites(e.panel->restoreForTest(), Param::EmphRestore);
	chipWrites(e.panel->inLoopForTest(), Param::EmphInLoop);
	chipWrites(e.panel->tapForTest(), Param::FbTap);
	// THE SHAPE: a press on the selector's next arrow, which is the Bell.
	REQUIRE(eroField(e.d, Param::EmphShape) == 0.0f);
	CHECK(std::string(e.panel->shapeForTest()->tooltipAt({ 2.0f, 2.0f })) == desc(Param::EmphShape).tooltip);
	e.panel->shapeForTest()->onMouseDown().callback(
	    at(e.panel->shapeForTest()->width() - 2.0f, e.panel->shapeForTest()->height() * 0.5f));
	e.refresh();
	CHECK(eroField(e.d, Param::EmphShape) == 1.0f);
	// THE CARD'S.
	knobWrites(e.d.card->emphasisForTest(), Param::EmphAmount, -9.0);
	knobWrites(e.d.card->resonanceForTest(), Param::Resonance, 0.2);
	e.mode(ero::Mode::Comb);
	knobWrites(e.d.card->loopForTest(), Param::Loop, 0.25);
	chipWrites(e.d.card->tapForTest(), Param::FbTap);
	chipWrites(e.d.card->restoreForTest(), Param::EmphRestore);
}
TEST_CASE("ERODER's card and window knobs for the shared section are watched for arm") {
	EroFaces e;
	ui::ModState* mod = e.d.f.gui.modForTest();
	REQUIRE(mod != nullptr);
	const std::vector<ui::Knob*> cardKnobs = { e.d.card->emphasisForTest(), e.d.card->resonanceForTest(),
		                                      e.d.card->loopForTest() };
	e.d.card->setModulation(mod, nullptr);
	for (const ui::Knob* knob : cardKnobs) {
		REQUIRE(knob != nullptr);
		CHECK(mod->watches(knob));
	}
}
TEST_CASE("ERODER's plot draws the emphasis the engine's sections make: a tilt about Freq, a bell of Width, nothing at 0 or in the Comb, and the modulated value") {
	namespace ui_ = feathers::ui;
	ModFixture f;
	feathers::FxChain raw;
	const size_t slot = feathers::addFxDevice(&raw, ero::kTraits.tag);
	REQUIRE(slot < raw.device.size());
	auto& row = raw.device[slot].row.field;
	row[eroAt(ero::Param::Freq)] = 10.0f; // 1024 Hz
	row[eroAt(ero::Param::EmphAmount)] = 9.0f;
	row[eroAt(ero::Param::Width)] = 0.5f;
	f.bridge.restoreFxChain(raw);
	const DeviceInstanceId id = f.bridge.fxDeviceAt(slot);
	const auto put = [&](ero::Param p, float v) {
		raw.device[slot].row.field[eroAt(p)] = v;
		f.bridge.setDeviceControl(id, eroAt(p), v);
		f.bridge.refresh();
	};
	ui_::EroderPlot plot(f.deviceHost, id, true);
	plot.setBounds(0.0f, 0.0f, 500.0f, 300.0f);
	REQUIRE(plot.update());
	const double rate = f.deviceHost.sampleRate() > 0.0 ? f.deviceHost.sampleRate() : 48000.0;
	const double centre = plot.bandForTest().freqHz;
	REQUIRE(centre == doctest::Approx(1024.0).epsilon(0.01));

	// THE TILT, restated from its analogue prototype H(s) = (g s + 1) / (s + g), g = 10^(dB/20), s in units of the pivot,
	// the frequency warped as the bilinear transform does it.
	const auto tiltDb = [&](double db, double hz) {
		const double g = std::pow(10.0, db / 20.0);
		const double w = std::tan(std::numbers::pi * hz / rate) / std::tan(std::numbers::pi * centre / rate);
		return 10.0 * std::log10((g * g * w * w + 1.0) / (w * w + g * g));
	};
	for (const double hz : { 128.0, centre, 8192.0 }) {
		CAPTURE(hz);
		const auto drawn = plot.emphasisDbForTest(hz);
		REQUIRE(drawn.has_value());
		CHECK(std::abs(*drawn - tiltDb(9.0, hz)) < 0.2);
	}
	// NON-VACUITY: the curve is not flat — the highs are up and the lows are down.
	CHECK(*plot.emphasisDbForTest(8192.0) > 5.0);
	CHECK(*plot.emphasisDbForTest(128.0) < -5.0);
	// ITS PLACE ON THE PLOT: up for a boost.
	CHECK(plot.emphasisYForTest(*plot.emphasisDbForTest(8192.0)) < plot.emphasisYForTest(*plot.emphasisDbForTest(128.0)));

	// THE BELL, restated by its definition: the gain at the centre, half of it (in dB) at the half-gain points, which are
	// Width octaves apart; Width 0.5 is a quarter of an octave times twelve to the half.
	put(ero::Param::EmphShape, 1.0f);
	REQUIRE(plot.update());
	const double octaves = 0.25 * std::pow(12.0, 0.5);
	const double upper = centre * std::exp2(octaves * 0.5);
	const double lower = centre * std::exp2(-octaves * 0.5);
	CHECK(std::abs(*plot.emphasisDbForTest(centre) - 9.0) < 0.2);
	CHECK(std::abs(*plot.emphasisDbForTest(upper) - 4.5) < 0.2);
	CHECK(std::abs(*plot.emphasisDbForTest(lower) - 4.5) < 0.2);
	CHECK(std::abs(*plot.emphasisDbForTest(centre * 64.0)) < 0.5);
	// A CUT IS A DIP.
	put(ero::Param::EmphAmount, -9.0f);
	REQUIRE(plot.update());
	CHECK(std::abs(*plot.emphasisDbForTest(centre) + 9.0) < 0.2);

	// NONE AT 0.
	put(ero::Param::EmphAmount, 0.0f);
	REQUIRE(plot.update());
	CHECK_FALSE(plot.emphasisDbForTest(centre).has_value());

	// THE MODULATED ROW IS WHAT IS DRAWN: a route into Emphasis, the engine's value published a block later.
	put(ero::Param::EmphAmount, 3.0f);
	put(ero::Param::EmphShape, 0.0f);
	f.bridge.setRouteAmount(feathers::backplane::Source::core(0),
	                        feathers::backplane::Destination::control(id, eroAt(ero::Param::EmphAmount)), 0.5f);
	REQUIRE(plot.update());
	CHECK(std::abs(*plot.emphasisDbForTest(8192.0) - tiltDb(3.0, 8192.0)) < 0.2);
	feathers::FxChain moved = raw;
	moved.device[slot].row.field[eroAt(ero::Param::EmphAmount)] = 12.0f;
	f.bridge.publishModulated(feathers::defaultModRack(), moved, feathers::RackPlayheads {},
	                          feathers::mod::TransportClock::unknown());
	REQUIRE(plot.update());
	CHECK(std::abs(*plot.emphasisDbForTest(8192.0) - tiltDb(12.0, 8192.0)) < 0.2);
	CHECK(std::abs(*plot.emphasisDbForTest(8192.0) - tiltDb(3.0, 8192.0)) > 3.0);

	// THE WINDOW'S COMB PICTURE IS THE SPECTRUM TOO, with the comb's pitch where Freq is elsewhere: the curve is drawn about it.
	put(ero::Param::Mode, static_cast<float>(ero::Mode::Comb));
	REQUIRE(plot.update());
	CHECK(plot.emphasisDbForTest(centre).has_value());
}
TEST_CASE("ERODER's plot has a handle on the band's edge that drags Width across; with no noise there is no edge and the wheel turns nothing") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	d.set(eroAt(ero::Param::Freq), 9.0f);
	d.set(eroAt(ero::Param::Amount), -11.0f);
	d.set(eroAt(ero::Param::Width), 0.5f);
	(void)d.card->updateContent();
	ui::EroderPlot* plot = d.card->plotForTest();
	// THE EDGE IS WHERE THE DRAWN BAND IS 12 dB DOWN, above its centre.
	const double edgeHz = plot->edgeHzForTest();
	CHECK(edgeHz > plot->bandForTest().freqHz);
	CHECK(plot->bandDbForTest(edgeHz) == doctest::Approx(ui::EroderPlot::kEdgeDb).epsilon(1e-4));
	CHECK(plot->edgeXForTest() == doctest::Approx(plot->hzToXForTest(edgeHz)));

	// A PRESS ON IT AND A DRAG ACROSS: Width by the pointer's travel, Freq and Amount untouched.
	const float span = (plot->hzToXForTest(ui::EroderPlot::kMaxHz) - plot->hzToXForTest(ui::EroderPlot::kMinHz))
	                   * ui::EroderPlot::kWidthDragShare;
	const float ex = plot->edgeXForTest();
	const float ey = plot->edgeYForTest();
	plot->onMouseDown().callback(at(ex, ey));
	plot->onMouseDrag().callback(at(ex + 12.0f, ey - 9.0f));
	CHECK(eroField(d, ero::Param::Width) == doctest::Approx(0.5 + 12.0 / static_cast<double>(span)).epsilon(1e-5));
	plot->onMouseDrag().callback(at(ex - 6.0f, ey));
	CHECK(eroField(d, ero::Param::Width) == doctest::Approx(0.5 - 6.0 / static_cast<double>(span)).epsilon(1e-5));
	plot->onMouseDrag().callback(at(ex + 4000.0f, ey));
	CHECK(eroField(d, ero::Param::Width) == 1.0f);
	plot->onMouseUp().callback(at(ex, ey));
	CHECK(eroField(d, ero::Param::Freq) == 9.0f);
	CHECK(eroField(d, ero::Param::Amount) == -11.0f);

	// ALL SINE: the same press is the band's own handle (it writes Freq), and the wheel is nobody's.
	d.set(eroAt(ero::Param::Width), 0.5f);
	d.set(eroAt(ero::Param::Source), 0.0f);
	(void)d.card->updateContent();
	plot->onMouseDown().callback(at(ex, ey));
	plot->onMouseDrag().callback(at(ex + 12.0f, ey));
	plot->onMouseUp().callback(at(ex + 12.0f, ey));
	CHECK(eroField(d, ero::Param::Width) == 0.5f);
	CHECK(eroField(d, ero::Param::Freq) != 9.0f);
	visage::MouseEvent wheel = at(ex, ey);
	wheel.wheel_delta_y = 1.0f;
	CHECK_FALSE(plot->onMouseWheel().callback(wheel));
	CHECK(eroField(d, ero::Param::Width) == 0.5f);
}
TEST_CASE("ERODER's plot in Rust draws the band at a height of its own and writes no Amount") {
	DynCardFixture<ui::EroderCard> d(ero::kTraits.tag);
	d.set(eroAt(ero::Param::Mode), static_cast<float>(ero::Mode::Rust));
	d.set(eroAt(ero::Param::Freq), 9.0f);
	d.set(eroAt(ero::Param::Amount), -11.0f);
	(void)d.card->updateContent();
	ui::EroderPlot* plot = d.card->plotForTest();
	REQUIRE(plot->showsRustForTest());
	const float foot = plot->plotFootForTest();
	const float top = plot->plotTopForTest();
	CHECK(plot->handleYForTest() == doctest::Approx(foot - ui::EroderPlot::kRustBandShare * (foot - top)));
	// THE HEIGHT IS NOT THE AMOUNT'S: moving Dust's Amount underneath moves nothing.
	const float before = plot->handleYForTest();
	d.set(eroAt(ero::Param::Amount), -16.0f);
	(void)d.card->updateContent();
	CHECK(plot->handleYForTest() == before);

	// ACROSS IS STILL FREQ; UP AND DOWN ARE NOBODY'S.
	const float x0 = plot->handleXForTest();
	const float y0 = 0.5f * (top + foot);
	plot->onMouseDown().callback(at(x0, y0));
	plot->onMouseDrag().callback(at(x0 + 20.0f, y0 - 40.0f));
	plot->onMouseUp().callback(at(x0 + 20.0f, y0 - 40.0f));
	CHECK(eroField(d, ero::Param::Freq) == doctest::Approx(std::log2(plot->xToHzForTest(x0 + 20.0f))).epsilon(1e-5));
	CHECK(eroField(d, ero::Param::Amount) == -16.0f);
}
TEST_CASE("A pair cell can carry a narrow middle, and a bare pair nests in another") {
	visage::Frame a;
	visage::Frame b;
	visage::Frame mid;
	ui::PairCell pair("Slots", a, b);
	pair.setMiddle(mid, 22.0f);
	pair.setBounds(0.0f, 0.0f, 122.0f, ui::PairCell::heightFor(60.0f));
	// THE TWO HALVES SHARE WHAT THE MIDDLE LEAVES.
	CHECK(a.width() == 50.0f);
	CHECK(mid.x() == 50.0f);
	CHECK(mid.width() == 22.0f);
	CHECK(b.x() == 72.0f);
	CHECK(b.width() == 50.0f);
	// THE MIDDLE IS CENTRED ON THE CELLS, under the caption, and one given a height is centred at it.
	CHECK(mid.y() == ui::PairCell::kCaptionHeight);
	CHECK(mid.height() == 60.0f);
	pair.setMiddle(mid, 22.0f, 40.0f);
	CHECK(mid.y() == ui::PairCell::kCaptionHeight + 10.0f);
	CHECK(mid.height() == 40.0f);
	pair.setSplit(0.25f);
	CHECK(a.width() == 25.0f);
	CHECK(mid.x() == 25.0f);
	CHECK(b.x() == 47.0f);

	// WITHOUT A MIDDLE NOTHING IS CHANGED: the cells take the whole width.
	visage::Frame c;
	visage::Frame e;
	ui::PairCell plain("Drive", c, e);
	plain.setBounds(0.0f, 0.0f, 120.0f, ui::PairCell::heightFor(60.0f));
	CHECK(c.width() == 60.0f);
	CHECK(e.x() == 60.0f);
	CHECK(plain.middle() == nullptr);

	// A BARE PAIR HAS NO CAPTION BAND: its cells take the whole height, so it nests.
	visage::Frame p;
	visage::Frame q;
	ui::PairCell bare("", p, q);
	CHECK(bare.bare());
	bare.setBounds(0.0f, 0.0f, 80.0f, 60.0f);
	CHECK(p.y() == 0.0f);
	CHECK(p.height() == 60.0f);
	CHECK(q.x() == 40.0f);
}
TEST_CASE("A route switch shows both of its choices, lights the one the control holds, and a press on the other chooses it") {
	SlotRow r;
	ui::RouteSwitch& sw = *r.route;
	sw.setBounds(0.0f, 0.0f, ui::RouteSwitch::kWidth, ui::RouteSwitch::heightFor(ui::EroderCard::kKnobDiameter));
	CHECK(sw.control().desc().key == std::string_view("eroder_track"));

	// BOTH GLYPHS HAVE A RECTANGLE, one over the other, neither outside the switch.
	const visage::Bounds top = sw.glyphBounds(ui::RouteSwitch::Series);
	const visage::Bounds bottom = sw.glyphBounds(ui::RouteSwitch::Parallel);
	CHECK(top.width() > 0.0f);
	CHECK(top.height() > 0.0f);
	CHECK(top.bottom() <= bottom.y());
	CHECK(bottom.bottom() <= sw.height());
	CHECK(top.x() >= 0.0f);
	CHECK(top.right() <= sw.width());

	const auto centre = [](const visage::Bounds& b) { return visage::Point { b.x() + b.width() * 0.5f, b.y() + b.height() * 0.5f }; };
	const auto press = [&](visage::Point p) {
		sw.onMouseDown().callback(at(p.x, p.y));
		sw.onMouseUp().callback(at(p.x, p.y));
	};

	r.d.set(eroAt(ero::Param::Track), 0.0f);
	CHECK(sw.litChoice() == ui::RouteSwitch::Series);
	r.notes.said.clear();
	press(centre(bottom));
	CHECK(eroField(r.d, ero::Param::Track) == 1.0f);
	CHECK(sw.litChoice() == ui::RouteSwitch::Parallel);
	REQUIRE(r.notes.said.size() == 1);
	CHECK(r.notes.said[0] == "Eroder Track Parallel");
	press(centre(top));
	CHECK(eroField(r.d, ero::Param::Track) == 0.0f);
	CHECK(sw.litChoice() == ui::RouteSwitch::Series);
	REQUIRE(r.notes.said.size() == 2);
	CHECK(r.notes.said[1] == "Eroder Track Series");

	// A PRESS ON THE LIT GLYPH WRITES AND SAYS NOTHING.
	press(centre(top));
	CHECK(eroField(r.d, ero::Param::Track) == 0.0f);
	CHECK(r.notes.said.size() == 2);

	// EACH GLYPH'S INK IS INSIDE ITS PLATE WITH A MARGIN on every side, and the two weigh alike (the same height of line).
	for (const int choice : { static_cast<int>(ui::RouteSwitch::Series), static_cast<int>(ui::RouteSwitch::Parallel) }) {
		const visage::Bounds plate = sw.glyphBounds(choice);
		const visage::Bounds ink = sw.inkBounds(choice);
		CAPTURE(choice);
		CHECK(ink.x() >= plate.x() + ui::RouteSwitch::kInkMargin);
		CHECK(ink.right() <= plate.right() - ui::RouteSwitch::kInkMargin);
		CHECK(ink.y() >= plate.y() + ui::RouteSwitch::kInkMargin);
		CHECK(ink.bottom() <= plate.bottom() - ui::RouteSwitch::kInkMargin);
	}
	CHECK(sw.inkBounds(ui::RouteSwitch::Series).height() == sw.inkBounds(ui::RouteSwitch::Parallel).height());

	// A PRESS OUTSIDE BOTH GLYPHS — the gap between, and outside the switch — DOES NOTHING.
	const visage::Point gap { sw.width() * 0.5f, (top.bottom() + bottom.y()) * 0.5f };
	REQUIRE(top.bottom() < bottom.y());
	CHECK(sw.choiceAt(gap) == -1);
	press(gap);
	press({ sw.width() + 5.0f, 3.0f });
	CHECK(eroField(r.d, ero::Param::Track) == 0.0f);
	CHECK(r.notes.said.size() == 2);

	// THE LIT GLYPH FOLLOWS THE CONTROL, whoever moved it.
	r.d.set(eroAt(ero::Param::Track), 1.0f);
	CHECK(sw.litChoice() == ui::RouteSwitch::Parallel);

	// A RIGHT PRESS IS NOT A PRESS OF THE SWITCH, and a greyed switch takes none.
	r.d.set(eroAt(ero::Param::Track), 0.0f);
	sw.setEnabled(false);
	press(centre(bottom));
	CHECK(eroField(r.d, ero::Param::Track) == 0.0f);
	sw.setEnabled(true);

	// IT DECLARES ITS CONTROL, so the card/window superset guard can see it.
	std::vector<ui::ControlFace::Offered> offered;
	sw.offeredControls(offered);
	REQUIRE(offered.size() == 1);
	CHECK(offered[0].control == &sw.control());
	CHECK(std::string(sw.tooltipAt({ 3.0f, 3.0f })).size() > 0);
}
TEST_CASE("A stacked mode cell carries a selector and two value boxes in the height of one knob column") {
	SlotRow r;
	ui::ModeValueCell& m = *r.cellA;
	const float h = ui::Knob::columnHeight(ui::EroderCard::kKnobDiameter);
	m.setBounds(0.0f, 0.0f, r.cell - 6.0f, h);
	CHECK(m.stacked());
	CHECK(m.followerParams().size() == 2);
	ui::ValueBox* shape = m.boxFor(eroAt(ero::Param::DriveShape));
	ui::ValueBox* mix = m.boxFor(eroAt(ero::Param::Mix));
	REQUIRE(shape != nullptr);
	REQUIRE(mix != nullptr);
	CHECK(shape->isVisible());
	CHECK(mix->isVisible());
	// EACH ROW SAYS WHICH CONTROL IT IS, by an inline label.
	CHECK(shape->label() == "S");
	CHECK(mix->label() == "M");
	// THREE ROWS, TOP DOWN: the selector, Shape, Mix — no two overlap and all are inside the cell above the caption.
	const visage::Bounds sel { m.select().x(), m.select().y(), m.select().width(), m.select().height() };
	const visage::Bounds b1 { shape->x(), shape->y(), shape->width(), shape->height() };
	const visage::Bounds b2 { mix->x(), mix->y(), mix->width(), mix->height() };
	CHECK_FALSE(overlaps(sel, b1));
	CHECK_FALSE(overlaps(b1, b2));
	CHECK_FALSE(overlaps(sel, b2));
	CHECK(sel.bottom() <= b1.y());
	CHECK(b1.bottom() <= b2.y());
	CHECK(b2.bottom() <= ui::ModeValueCell::stackedRowsFor(ui::EroderCard::kKnobDiameter).caption);
	CHECK(b2.bottom() <= h);
	CHECK(sel.y() >= 0.0f);
	// AT THE WIDTH A QUARTER-ROW CELL GIVES ONCE THE SWITCH HAS TAKEN ITS SHARE, the selector still prints its widest name.
	// (`minimumWidth` is the widest Type name between its arrows: the card's own Drive cell is narrower than it too, and
	// shows a name that fits — see the slot-row case, which prints both.)
	CHECK(b1.width() > 0.0f);
	CHECK(m.select().width() > 0.0f);
	// THE TYPE BOX KEEPS ITS ARROWS IN THIS WIDTH: a tight face's arrow cells are narrow, the face prints each Type's short name at the
	// normal size between them, and a press in the middle (not on an arrow) opens the choice menu.
	CHECK(m.select().tightFace());
	CHECK(m.select().arrowsShown());
	CHECK(m.select().nameSize() == ui::kKnobReadoutSize);
	CHECK(m.select().verbAt({ 1.0f, 3.0f }) == ui::ZoneSelect::Verb::Prev);
	CHECK(m.select().verbAt({ m.select().width() - 1.0f, 3.0f }) == ui::ZoneSelect::Verb::Next);
	CHECK(m.select().verbAt({ m.select().width() * 0.5f, 3.0f }) == ui::ZoneSelect::Verb::Menu);
	// "Paunch-S" AND "Paunch-H" ARE AS LONG AS "Lopside": selected, each is drawn at a size that fits, not under the floor.
	for (const ero::DriveMode type : { ero::DriveMode::PaunchS, ero::DriveMode::PaunchH, ero::DriveMode::PaunchOS, ero::DriveMode::PaunchOH }) {
		m.select().selectZone(static_cast<size_t>(type));
		CAPTURE(static_cast<int>(type));
		CHECK(m.select().nameSize() >= ui::kNameFitFloorSize);
		CHECK(m.select().nameSize() <= ui::kKnobReadoutSize);
	}
	// A WIDE ENOUGH STACKED CELL KEEPS ITS ARROWS, and an unstacked one never drops them.
	m.setBounds(0.0f, 0.0f, 120.0f, h);
	CHECK(m.select().arrowsShown());
	CHECK(m.select().verbAt({ 2.0f, 3.0f }) == ui::ZoneSelect::Verb::Prev);
	m.setStacked(false);
	m.setBounds(0.0f, 0.0f, r.cell - 6.0f, h);
	CHECK(m.select().arrowsShown());
	CHECK_FALSE(m.select().tightFace());
	m.setStacked(true);
	m.setBounds(0.0f, 0.0f, r.cell - 6.0f, h);

	// A FOLLOWER THE GATE CLOSES LEAVES ITS ROW ONLY; the other keeps its place.
	bool mixLive = false;
	ui::ModeValueCell gated(*r.controls.front(), "Type",
	                        { { "A", {} }, { "B", {} } },
	                        [&](size_t p) { return p != eroAt(ero::Param::Mix) || mixLive; });
	gated.setFaceDiameter(ui::EroderCard::kKnobDiameter);
	(void)gated.addFollower(eroAt(ero::Param::DriveShape),
	                        std::make_unique<ui::ValueBox>(*r.controls.front(), ""));
	(void)gated.addFollower(eroAt(ero::Param::Mix), std::make_unique<ui::ValueBox>(*r.controls.front(), ""));
	gated.setStacked(true);
	gated.setBounds(0.0f, 0.0f, 60.0f, h);
	(void)gated.refresh();
	CHECK(gated.boxFor(eroAt(ero::Param::DriveShape))->isVisible());
	CHECK_FALSE(gated.boxFor(eroAt(ero::Param::Mix))->isVisible());
	mixLive = true;
	CHECK(gated.refresh()); // the second row appeared although the first live follower did not change
	CHECK(gated.boxFor(eroAt(ero::Param::Mix))->isVisible());
}
TEST_CASE("The slot row: two slots and a route switch between them are one item of four columns on one line") {
	for (const float scale : { 1.0f, 1.75f }) {
		CAPTURE(scale);
		SlotRow r;
		r.outer->setDpiScale(scale);
		r.outer->setBounds(0.0f, 0.0f, r.rowW, r.rowH);
		MESSAGE("slot row at scale " << scale << ": column " << r.cell << " row " << r.rowW << " x " << r.rowH
		                             << ", mode cell " << r.cellA->width() << " (minimum " << r.cellA->minimumWidth() << ")");
		const visage::Frame* const five[] = { r.knobA.get(), r.cellA.get(), r.route.get(), r.knobB.get(), r.cellB.get() };
		const visage::Bounds outerBounds { 0.0f, 0.0f, r.rowW, r.rowH };
		for (size_t i = 0; i < 5; ++i) {
			const visage::Bounds bi = r.inOuter(*five[i]);
			CHECK(bi.width() > 0.0f);
			CHECK(bi.x() >= 0.0f);
			CHECK(bi.y() >= ui::PairCell::kCaptionHeight);
			CHECK(bi.right() <= outerBounds.right());
			CHECK(bi.bottom() <= outerBounds.bottom());
			// NATIVE WIDTHS follow the scale, and are positive.
			CHECK(five[i]->nativeWidth() > 0);
			CHECK(static_cast<float>(five[i]->nativeWidth()) >= bi.width() * scale - 2.0f);
			for (size_t j = i + 1; j < 5; ++j) {
				CHECK_FALSE(overlaps(bi, r.inOuter(*five[j])));
			}
		}
		// ONE LINE, LEFT TO RIGHT: Amount, Type, route, Amount, Type, the three cells top-aligned.
		for (size_t i = 0; i + 1 < 5; ++i) {
			CHECK(r.inOuter(*five[i]).right() <= r.inOuter(*five[i + 1]).x());
			CHECK(r.inOuter(*five[i]).y() == r.inOuter(*five[0]).y());
		}
		CHECK(r.route->width() == ui::RouteSwitch::kWidth);
		// THE CAPTION AND THE SELECTORS FIT: the caption's tracked width is inside the row, and the Type cell prints its
		// widest name. (Knob captions are cut to their column by `Knob` itself and are not measured here.)
		const visage::Font& face = ui::font(ui::kKnobLabelSize);
		CHECK(ui::trackedWidth(face, "SLOTS", ui::kKnobLabelTracking) <= r.rowW);
		// THE ARITHMETIC OF THE ROW: the halves share what the 22 px middle leaves, and each splits in two.
		CHECK(r.cellA->width() == doctest::Approx(((r.rowW - ui::RouteSwitch::kWidth) * 0.5f) * 0.5f).epsilon(0.001));
		MESSAGE("Type cell " << r.cellA->width() << " wide; its minimumWidth " << r.cellA->minimumWidth()
		                     << "; the card's own Drive cell is " << r.d.card->driveCellForTest()->width()
		                     << " (minimum " << r.d.card->driveCellForTest()->minimumWidth() << ")");
		CHECK(r.cellA->width() >= r.d.card->driveCellForTest()->width() - 6.0f);
		// BOTH TYPE CELLS ARE THREE ROWS WITH NONE OVER ANOTHER.
		for (ui::ModeValueCell* m : { r.cellA.get(), r.cellB.get() }) {
			ui::ValueBox* shape = m->boxFor(eroAt(ero::Param::DriveShape));
			ui::ValueBox* mix = m->boxFor(eroAt(ero::Param::Mix));
			CHECK(m->select().bottom() <= shape->y());
			CHECK(shape->bottom() <= mix->y());
			CHECK(mix->bottom() <= m->height());
		}

		// THE FLOW: the composite is one item of span 4 and never splits.
		visage::Frame lead;
		visage::Frame after;
		const std::vector<ui::FlowItem> items { { &lead, 1, true, 40.0f }, { r.outer.get(), 4, true, r.rowH }, { &after, 1, true, 40.0f } };
		const float foot = ui::flowCells(items, 4, 10.0f, 0.0f, r.cell, 6.0f);
		CHECK(lead.x() == 10.0f);
		CHECK(r.outer->x() == 10.0f); // wrapped whole, to the start of the next row
		CHECK(r.outer->y() == 46.0f);
		CHECK(r.outer->width() == r.cell * 4.0f);
		CHECK(after.y() == r.outer->y() + r.rowH + 6.0f);
		CHECK(foot == after.y() + 40.0f);
		// AND ON A ROW OF ITS OWN IT STAYS.
		const std::vector<ui::FlowItem> alone { { r.outer.get(), 4, true, r.rowH } };
		(void)ui::flowCells(alone, 4, 0.0f, 0.0f, r.cell, 6.0f);
		CHECK(r.outer->x() == 0.0f);
		CHECK(r.outer->y() == 0.0f);
		CHECK(r.outer->width() == r.rowW);
	}
}

