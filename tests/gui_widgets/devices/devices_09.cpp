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

// PIECE 9 OF 18 of the devices suite: its cases in their original order, with the helpers only they use. See devices_common.h.

#include "devices/devices_common.h"

TEST_CASE("IK: with a source armed, a pill routes it — the face to the AMOUNT, the power symbol to the POWER") {
	// The fill pills have full modulation support — glow, modulated amount, modulated amount when armed —
	// and a little power symbol floats near each in mod assign mode. ROUTES read from the bridge's store by destinations built here, not from the pill.
	namespace shp = feathers::fx::shaper;
	// THE STATE ABOVE THE WIDGETS THAT WATCH IT (frame_watchers.h): the pills are on this list, so it must
	// outlive them. Declared below the fixture it died first, and the sanitizer build's assert said so.
	ui::ModState mod;
	ui::EditHistory history;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	f.panel->setModulation(&mod, &history);
	const backplane::Source source = ui::macroSourceAt(0);
	ui::FaderPill& sub = *f.panel->pillsForTest()[0];
	ui::FaderPill& deadzone = *f.panel->pillsForTest()[5];
	const auto routeTo = [&](shp::Param p) {
		return f.f.bridge.routeAmount(source,
		                              backplane::Destination::control(f.device, static_cast<size_t>(p)));
	};
	const auto valueOf = [&](shp::Param p) {
		const FxChain chain = f.f.bridge.fxChain();
		return fxDeviceAtPosition(chain, f.slot).row.field[static_cast<size_t>(p)];
	};

	// NOTHING ARMED: no symbol anywhere.
	CHECK_FALSE(sub.powerGlyphShownForTest());
	mod.focus(source);
	mod.arm(true);
	// ARMED: the symbol shows where the power is a destination — and since the decision of 2026-10-31 Deadzone's power is
	// one too (a MONO destination: it reshapes the shared table, so under Poly it takes one value for every voice).
	REQUIRE(sub.powerGlyphShownForTest());
	CHECK(deadzone.powerGlyphShownForTest());

	// THE FACE: a press seeds the knob's 0.4 and a drag of 35 px adds 35 / 70 -> 0.9, to the AMOUNT.
	const float y = sub.height() * 0.5f;
	const float x = sub.width() * 0.3f; // clear of the symbol at the right end
	sub.onMouseDown().callback(at(x, y));
	sub.onMouseDrag().callback(at(x + 35.0f, y));
	sub.onMouseUp().callback(at(x + 35.0f, y));
	CHECK(routeTo(shp::Param::Sub) == doctest::Approx(0.9f));
	CHECK(routeTo(shp::Param::SubOn) == 0.0f);
	CHECK(valueOf(shp::Param::Sub) == doctest::Approx(0.5f)); // a route, not a value
	CHECK(valueOf(shp::Param::SubOn) == 0.0f);

	// THE SYMBOL: the same gesture, to the POWER — 0.4 + 14 / 70 = 0.6 — and the amount's route stands.
	const visage::Bounds g = sub.powerGlyphForTest();
	const float gx = g.x() + g.width() * 0.5f;
	const float gy = g.y() + g.height() * 0.5f;
	sub.onMouseDown().callback(at(gx, gy));
	sub.onMouseDrag().callback(at(gx + 14.0f, gy));
	sub.onMouseUp().callback(at(gx + 14.0f, gy));
	CHECK(routeTo(shp::Param::SubOn) == doctest::Approx(0.6f));
	CHECK(routeTo(shp::Param::Sub) == doctest::Approx(0.9f));

	// DOUBLE-CLICK UNROUTES — the face's route only.
	sub.onMouseDown().callback(doubleAt(x, y));
	sub.onMouseUp().callback(doubleAt(x, y));
	CHECK(routeTo(shp::Param::Sub) == 0.0f);
	CHECK(routeTo(shp::Param::SubOn) == doctest::Approx(0.6f));

	// **AN UPWARD DRAG ROUTES TOO** — it was found the symbol answered only a SIDEWAYS drag, where every knob
	// routes on an upward one. 0.6 already stood; 21 px up adds 21 / 70 = 0.3 -> 0.9.
	sub.onMouseDown().callback(at(gx, gy));
	sub.onMouseDrag().callback(at(gx, gy - 21.0f));
	sub.onMouseUp().callback(at(gx, gy - 21.0f));
	CHECK(routeTo(shp::Param::SubOn) == doctest::Approx(0.9f));

	// ARMED, THE SYMBOL SPEAKS FOR THE POWER; the small ring is for the unarmed pill.
	CHECK_FALSE(sub.powerRingShownForTest());

	// DISARMED: the symbol goes, the routed power shows its ring (the active state has a modulated amount
	// display), and a click is the power again.
	mod.arm(false);
	CHECK_FALSE(sub.powerGlyphShownForTest());
	CHECK(sub.powerRingShownForTest());
	// AND A PILL WHOSE POWER NOBODY ROUTED SHOWS NO RING.
	CHECK_FALSE(f.panel->pillsForTest()[1]->powerRingShownForTest());
	sub.onMouseDown().callback(at(x, y));
	sub.onMouseUp().callback(at(x, y));
	CHECK(valueOf(shp::Param::SubOn) == 1.0f);
}
TEST_CASE("IJ: the Shaper pad pans as Explore does — the press lands the pictured point, a hand at the edge pans on time, the release glides home") {
	// The same pan drag as Explore. The joystick committed a delta per event and
	// re-derived the plane on every one, and measured the pointer and drew the map on different scales.
	namespace shp = feathers::fx::shaper;
	namespace ps = shp::phase_space;
	ShaperFixture f;
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::Colour), 0.38f);
	visage::Canvas canvas;
	pad->draw(canvas); // the pad takes the colour: a view that moved re-anchors
	const float w = pad->width();
	const float h = pad->height();
	REQUIRE(w > h); // the 420x300 pad — the shape the anisotropy lived on
	const visage::Point centre { w * 0.5f, h * 0.5f };
	const double k = ui::ShaperPad::kTravelPerHalfDrag / (std::min(w, h) * 0.5); // plane units per pixel at x1

	const auto rowTravel = [&] {
		const FxChain chain = f.f.bridge.fxChain();
		const DeviceRow& row = fxDeviceAtPosition(chain, f.slot).row;
		ps::Phases t {};
		for (size_t i = 0; i < ps::kPhaseCount; ++i) {
			t[i] = static_cast<double>(row.field[shp::kTravelFirst + i]);
		}
		return t;
	};
	// THE RESTATEMENT — origin plus the basis the frame is anchored on, carried to a plane point, wrapped.
	const ps::Phases origin = rowTravel();
	ps::Phases home = ps::homeFor(0.38, 0.0);
	for (size_t i = 0; i < ps::kPhaseCount; ++i) {
		home[i] += origin[i];
	}
	const feathers::SliceBasis<ps::kPhaseCount> basis = feathers::sliceBasisFor<ps::kPhaseCount>(0.0, ps::planeHomeOf(home));
	const auto expectAt = [&](double px, double py) {
		ps::Phases t {};
		for (size_t i = 0; i < ps::kPhaseCount; ++i) {
			const double v = origin[i] + basis.travelTerm(i, px, py);
			t[i] = v - std::floor(v);
		}
		return t;
	};
	const auto near = [](const ps::Phases& a, const ps::Phases& b) {
		double worst = 0.0;
		for (size_t i = 0; i < a.size(); ++i) {
			double d = std::fabs(a[i] - b[i]);
			d = std::min(d, 1.0 - d); // a phase is on a circle
			worst = std::max(worst, d);
		}
		return worst;
	};

	SUBCASE("the press commits the travel of the point pressed — one scale on both axes") {
		// 150 px RIGHT and 150 px UP are the same plane distance, which the joystick's map was not.
		pad->onMouseDown().callback(at(centre.x + 150.0f, centre.y));
		CHECK(near(rowTravel(), expectAt(150.0 * k, 0.0)) < 1e-5);
		pad->onMouseUp().callback(at(centre.x + 150.0f, centre.y));
		pad->finishGlideForTest();
	}
	SUBCASE("a drag inside the view puts the star exactly under the hand") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(centre.x - 120.0f, centre.y + 80.0f));
		CHECK(pad->puck().x == doctest::Approx(centre.x - 120.0f));
		CHECK(pad->puck().y == doctest::Approx(centre.y + 80.0f));
		CHECK(near(rowTravel(), expectAt(-120.0 * k, -80.0 * k)) < 1e-5);
		CHECK(pad->viewForTest().first == 0.0); // well inside: the view did not move
		pad->onMouseUp().callback(at(centre.x - 120.0f, centre.y + 80.0f));
	}
	// **THE HIT BOX IS THE EDGE, AND A HAND DWELLING THERE KEEPS PANNING.** The pan drag's hit box was narrow,
	// not at the actual edge, and hard to dwell on. The first
	// draft panned from 15% inside and only on drag EVENTS, so a still hand past the edge stopped it.
	using Pad = ui::ShaperPad;
	const double viewWide = static_cast<double>(w) * k;
	const double inset = Pad::kStarInsetPixels;
	SUBCASE("a hand held still past the edge keeps panning, frame after frame, with no mouse event") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w + 400.0f, centre.y)); // far past the right edge
		CHECK(pad->viewForTest().first == 0.0);                // an EVENT pans nothing: time does
		CHECK(pad->puck().x == doctest::Approx(w - inset));    // the puck stays whole on the edge crossed
		double lastView = 0.0;
		for (int i = 0; i < 10; ++i) {
			CAPTURE(i);
			CHECK(pad->tickForTest(0.05));
			const double v = pad->viewForTest().first;
			// FULL SPEED this deep, and no faster: 1.2 views/s for 50 ms.
			CHECK(v - lastView == doctest::Approx(Pad::kPanViewsPerSecond * 0.05 * viewWide));
			lastView = v;
		}
		CHECK(pad->viewForTest().second == 0.0); // right is right: nothing vertical
		// THE STAR RODE WITH THE VIEW, under the hand, and the sound is the travel of the star.
		const double star = lastView + (w - inset - w * 0.5) * k;
		CHECK(pad->starForTest().first == doctest::Approx(star));
		CHECK(pad->puck().x == doctest::Approx(w - inset));
		CHECK(near(rowTravel(), expectAt(star, 0.0)) < 1e-5);
		pad->onMouseUp().callback(at(w + 400.0f, centre.y));
		CHECK_FALSE(pad->tickForTest(0.05)); // released: the scroll is over
	}
	SUBCASE("the band is at the real edge: 30 px inside is still, 12 px inside scrolls at the ramp") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w - 30.0f, centre.y)); // the old 15% margin began 63 px in
		CHECK_FALSE(pad->tickForTest(0.05));
		CHECK(pad->viewForTest().first == 0.0);
		pad->onMouseDrag().callback(at(w - 12.0f, centre.y));
		CHECK(pad->tickForTest(0.05));
		const double ramp = (Pad::kEdgeBandPixels - 12.0) / Pad::kRampPixels;
		CHECK(pad->viewForTest().first == doctest::Approx(Pad::kPanViewsPerSecond * ramp * 0.05 * viewWide));
		pad->onMouseUp().callback(at(w - 12.0f, centre.y));
	}
	SUBCASE("above the top edge pans UP — up is more — and the left edge pans left") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(-200.0f, -200.0f));
		CHECK(pad->tickForTest(0.05));
		CHECK(pad->viewForTest().first < 0.0);
		CHECK(pad->viewForTest().second > 0.0);
		pad->onMouseUp().callback(at(-200.0f, -200.0f));
	}
	SUBCASE("the FRAME is what drives it: a draw while a hand dwells past the edge moves the view") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w + 400.0f, centre.y));
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		pad->draw(canvas);
		CHECK(pad->viewForTest().first > 0.0);
		pad->onMouseUp().callback(at(w + 400.0f, centre.y));
	}
	SUBCASE("a still PRESS in the band is a click, not a pan") {
		pad->onMouseDown().callback(at(w - 5.0f, centre.y));
		CHECK_FALSE(pad->tickForTest(0.05));
		CHECK(pad->viewForTest().first == 0.0);
		pad->onMouseUp().callback(at(w - 5.0f, centre.y));
	}
	SUBCASE("a long dwell keeps the map covering the view") {
		pad->onMouseDown().callback(at(centre.x, centre.y));
		pad->onMouseDrag().callback(at(w + 400.0f, centre.y));
		const size_t before = pad->rebuildsForTest();
		for (int i = 0; i < 40; ++i) {
			pad->tickForTest(0.05); // 2 s: 2.4 views
		}
		CHECK(pad->rebuildsForTest() > before);
		pad->onMouseUp().callback(at(w + 400.0f, centre.y));
	}
	SUBCASE("the release glides the view home, then the frame re-origins on the sound") {
		pad->onMouseDown().callback(at(centre.x + 100.0f, centre.y));
		pad->onMouseUp().callback(at(centre.x + 100.0f, centre.y));
		const size_t before = pad->rebuildsForTest();
		if (!ui::reducedMotionRequested()) {
			CHECK(pad->glidingForTest());
			CHECK(pad->puck().x == doctest::Approx(centre.x + 100.0f)); // the view has not moved yet
		}
		pad->finishGlideForTest();
		CHECK_FALSE(pad->glidingForTest());
		CHECK(pad->puck().x == doctest::Approx(centre.x));
		CHECK(pad->starForTest().first == 0.0);
		CHECK(pad->rebuildsForTest() > before);
		// AND THE SOUND IS WHERE THE PRESS PUT IT — landing moved the view, never the travel.
		CHECK(near(rowTravel(), expectAt(100.0 * k, 0.0)) < 1e-5);
	}
}
TEST_CASE("phaser Task 9a: the explore pad over the phaser's policy writes all thirty-four phases at kTravelFirst + i, and Rotate re-origins without moving them") {
	// THE GENERALISATION, PROVED PAST THE SHAPER: `ExplorePad<PhaserPadPolicy>` — N = 34 (thirty, and round 2's four), a constant home, the
	// phaser's travel offset — driven with no device behind it, so every number is restated from the phase space.
	namespace ph = feathers::fx::phaser;
	namespace ps = ph::phase_space;
	constexpr size_t kN = ps::kPhaseCount;
	static_assert(kN == 34);
	static_assert(ui::PhaserPad::kPhaseCount == kN);

	struct RotateSpy final : feathers::device::Modulatable {
		const feathers::device::ParamDesc& d;
		double v = 0.0;
		explicit RotateSpy(const feathers::device::ParamDesc& desc) : d(desc) {}
		const feathers::device::ParamDesc& desc() const override { return d; }
		double value() const override { return v; }
		void setValue(double value) override { v = value; }
		feathers::device::ModDisplay display() const override { return {}; }
		void setAmount(float) override {}
		void beginGesture() override {}
		void endGesture() override {}
		std::string text() const override { return {}; }
	};
	RotateSpy rotate { ph::kDescriptor.params[static_cast<size_t>(ph::Param::Rotate)] };

	// THE ROW: the declared params carry a sentinel the pad must never write, and the standing travel starts
	// somewhere that is not home so a read from the wrong offset would show.
	constexpr float kSentinel = 0.123f;
	std::array<float, ph::kTravelFirst + kN> row {};
	row.fill(kSentinel);
	for (size_t i = 0; i < kN; ++i) {
		row[ph::kTravelFirst + i] = static_cast<float>(i) / static_cast<float>(kN) * 0.9f;
	}
	std::vector<size_t> written;
	ui::PhaserPad pad {
		[&](size_t index, float value) {
			written.push_back(index);
			REQUIRE(index < kN);
			row[ph::kTravelFirst + index] = value;
		},
		[&](size_t field) { return row.at(field); }, rotate };
	pad.setBounds(0, 0, 300, 200);
	visage::Canvas canvas;
	// THE SIZE MOVED UNDER THE PAD (built at construction, at width 0): the first draw rebuilds the map at this size
	// and re-anchors the frame on the row. True since the Task 9a review put the size in the staleness check — the
	// case below holds it.
	pad.draw(canvas);
	REQUIRE(pad.builtSizeForTest() == std::pair { 300, 200 });
	const float w = pad.width();
	const float h = pad.height();
	const double k = ui::PhaserPad::kTravelPerHalfDrag / (std::min(w, h) * 0.5); // plane units per pixel at x1

	const auto rowTravel = [&] {
		ps::Phases t {};
		for (size_t i = 0; i < kN; ++i) {
			t[i] = static_cast<double>(row[ph::kTravelFirst + i]);
		}
		return t;
	};
	// THE RESTATEMENT — the phaser's own home and basis, carried to a plane point, wrapped by the phase space.
	const auto expectAt = [&](const ps::Phases& origin, double rot, double px, double py) {
		ps::Phases home = ps::homeFor();
		for (size_t i = 0; i < kN; ++i) {
			home[i] += origin[i];
		}
		const ps::Basis basis = ps::basisFor(rot, ps::planeHomeOf(home));
		ps::Phases t {};
		for (size_t i = 0; i < kN; ++i) {
			const double v = origin[i] + basis.travelTerm(i, px, py);
			t[i] = v - std::floor(v);
		}
		return t;
	};
	const auto near = [](const ps::Phases& a, const ps::Phases& b) {
		double worst = 0.0;
		for (size_t i = 0; i < a.size(); ++i) {
			double d = std::fabs(a[i] - b[i]);
			d = std::min(d, 1.0 - d); // a phase is on a circle
			worst = std::max(worst, d);
		}
		return worst;
	};
	const auto paramsUntouched = [&] {
		return std::all_of(row.begin(), row.begin() + ph::kTravelFirst, [&](float v) { return v == kSentinel; });
	};

	// A DRAG: press at the centre, move 60 px right and 40 px up, release.
	const ps::Phases standing = rowTravel();
	const visage::Point centre { w * 0.5f, h * 0.5f };
	pad.onMouseDown().callback(at(centre.x, centre.y));
	written.clear();
	pad.onMouseDrag().callback(at(centre.x + 60.0f, centre.y - 40.0f));
	// EXACTLY THIRTY WRITES, ONE PER PHASE, AND NOTHING ELSE IN THE ROW.
	REQUIRE(written.size() == kN);
	for (size_t i = 0; i < kN; ++i) {
		CHECK(written[i] == i);
	}
	CHECK(paramsUntouched());
	const ps::Phases dragged = expectAt(standing, 0.0, 60.0 * k, 40.0 * k);
	CHECK(near(rowTravel(), dragged) < 1e-6);
	CHECK(near(dragged, standing) > 1e-3); // not vacuous: the drag moved the sound
	pad.onMouseUp().callback(at(centre.x + 60.0f, centre.y - 40.0f));
	pad.finishGlideForTest(); // the release glide, landed as a frame loop would
	CHECK(pad.puck().x == doctest::Approx(centre.x));
	CHECK(pad.puck().y == doctest::Approx(centre.y));

	// ROTATE RE-ORIGINS — a new basis, a rebuilt map, the puck at the centre — AND MOVES NO STORED TRAVEL.
	const ps::Phases before = rowTravel();
	const size_t rebuilds = pad.rebuildsForTest();
	written.clear();
	rotate.v = 0.7;
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() > rebuilds);
	CHECK(pad.builtForTest().rotate == 0.7);
	CHECK(written.empty());
	CHECK(rowTravel() == before);
	CHECK(pad.puck().x == doctest::Approx(centre.x));
	CHECK(pad.puck().y == doctest::Approx(centre.y));

	// AND THE NEXT DRAG READS THE TURNED PLANE, anchored on the travel the row holds (read back at kTravelFirst + i).
	pad.onMouseDown().callback(at(centre.x, centre.y));
	CHECK(near(rowTravel(), before) < 1e-6); // the centre is where the sound already is
	pad.onMouseDrag().callback(at(centre.x + 60.0f, centre.y - 40.0f));
	const ps::Phases turned = expectAt(before, 0.7, 60.0 * k, 40.0 * k);
	CHECK(near(rowTravel(), turned) < 1e-6);
	CHECK(near(turned, expectAt(before, 0.0, 60.0 * k, 40.0 * k)) > 1e-3); // not vacuous: the turn changed the plane
	CHECK(paramsUntouched());
	pad.onMouseUp().callback(at(centre.x + 60.0f, centre.y - 40.0f));
	pad.finishGlideForTest();

	// THE TERRAIN (Task 9b's, the response model's distance from home) is a brightness: every cell finite and in [0, 1] even
	// over this sentinel row. Its VALUES are held by "PHASER's pad terrain" below.
	const auto& cells = pad.mapForTest();
	CHECK(std::all_of(cells.begin(), cells.end(), [](float c) { return std::isfinite(c) && c >= 0.0f && c <= 1.0f; }));
}
TEST_CASE("explore pad: the first map is built at the pad's own size, not the width-0 view it was constructed at") {
	// THE TASK 9a REVIEW: the key had no pixel size, so the map built at construction (width 0, a 2x2 view) stayed,
	// stretched across the real pad, until some other input moved. The size is in the staleness check now.
	namespace ph = feathers::fx::phaser;
	struct RotateSpy final : feathers::device::Modulatable {
		const feathers::device::ParamDesc& d;
		explicit RotateSpy(const feathers::device::ParamDesc& desc) : d(desc) {}
		const feathers::device::ParamDesc& desc() const override { return d; }
		double value() const override { return 0.0; }
		void setValue(double) override {}
		feathers::device::ModDisplay display() const override { return {}; }
		void setAmount(float) override {}
		void beginGesture() override {}
		void endGesture() override {}
		std::string text() const override { return {}; }
	};
	RotateSpy rotate { ph::kDescriptor.params[static_cast<size_t>(ph::Param::Rotate)] };
	ph::Row defaults = ph::rowOf(ph::State {});
	ui::PhaserPad pad { [](size_t, float) {},
		                [&](size_t field) { return field < defaults.size() ? defaults[field] : 0.0f; }, rotate };
	// NOTHING IS BUILT AT CONSTRUCTION (the final review): a terrain for a 0-wide pad is thrown away by the first draw.
	CHECK(pad.builtSizeForTest() == std::pair { -1, -1 });
	const size_t atConstruction = pad.rebuildsForTest();
	CHECK(atConstruction == 0);
	pad.setBounds(0, 0, 420, 300);
	visage::Canvas canvas;
	pad.draw(canvas);
	CHECK(pad.builtSizeForTest() == std::pair { 420, 300 });
	CHECK(pad.rebuildsForTest() == atConstruction + 1);
	// AT REST, NOTHING MORE — and a second size is a second build.
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() == atConstruction + 1);
	// A RESIZE SETTLES FIRST (the Task 9b review): the frame that first sees a new size only notes it, so a window
	// dragged by its grip does not rebuild every frame; the next frame at the same size rebuilds. A drag that keeps
	// moving keeps deferring.
	pad.setBounds(0, 0, 300, 300);
	pad.draw(canvas);
	CHECK(pad.builtSizeForTest() == std::pair { 420, 300 });
	pad.setBounds(0, 0, 310, 300);
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() == atConstruction + 1);
	pad.draw(canvas);
	CHECK(pad.builtSizeForTest() == std::pair { 310, 300 });
	CHECK(pad.rebuildsForTest() == atConstruction + 2);
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() == atConstruction + 2);
}
TEST_CASE("backlog LT: the phaser and flanger terrain is drawn at a fixed Mix of 50% — the same map at Mix 20% and 100%, "
          "and no rebuild when Mix moves") {
	// The Phaser/Flanger terrain is pre-Mix; decided 2026-10-10: a fixed Mix of 50% (wet-only is
	// black: the phaser's wet is an allpass cascade and the flanger's one tap, both flat — `kTerrainMix`'s note).
	namespace ph = feathers::fx::phaser;
	struct RotateSpy final : feathers::device::Modulatable {
		const feathers::device::ParamDesc& d;
		explicit RotateSpy(const feathers::device::ParamDesc& desc) : d(desc) {}
		const feathers::device::ParamDesc& desc() const override { return d; }
		double value() const override { return 0.0; }
		void setValue(double) override {}
		feathers::device::ModDisplay display() const override { return {}; }
		void setAmount(float) override {}
		void beginGesture() override {}
		void endGesture() override {}
		std::string text() const override { return {}; }
	};
	RotateSpy rotate { ph::kDescriptor.params[static_cast<size_t>(ph::Param::Rotate)] };
	const size_t mix = static_cast<size_t>(ph::Param::Mix);
	const size_t mode = static_cast<size_t>(ph::Param::Mode);
	for (const float modeValue : { 0.0f, 1.0f }) { // Phaser, Flanger
		CAPTURE(modeValue);
		ph::Row row = ph::rowOf(ph::State {});
		row[mode] = modeValue;
		const auto reader = [&](size_t field) { return field < row.size() ? row[field] : 0.0f; };
		// THE KEY: Mix is not in it — every Mix reads as 50% — so a Mix move is no new key.
		row[mix] = 0.2f;
		const auto key20 = ui::PhaserPadPolicy::keyOf(reader, 0.0, 1.0, {});
		row[mix] = 1.0f;
		const auto key100 = ui::PhaserPadPolicy::keyOf(reader, 0.0, 1.0, {});
		CHECK(key20.settings.mix == ui::PhaserPadPolicy::kTerrainMix);
		CHECK(key20 == key100);
		CHECK(key20.settings.mode == (modeValue == 0.0f ? ph::Mode::Phaser : ph::Mode::Flanger));

		// THE MAP: built once at Mix 20%, and the same map — every cell — at 100% and at 50% itself.
		const auto mapAt = [&](float m) {
			row[mix] = m;
			ui::PhaserPad pad { [](size_t, float) {}, reader, rotate };
			pad.setBounds(0, 0, 420, 300);
			visage::Canvas canvas;
			pad.draw(canvas);
			REQUIRE(pad.rebuildsForTest() == 1);
			return pad.mapForTest();
		};
		const auto at20 = mapAt(0.2f);
		const auto at100 = mapAt(1.0f);
		const auto at50 = mapAt(0.5f);
		CHECK(at20 == at100);
		CHECK(at20 == at50);
		// NOT VACUOUS: the map is a picture, not a blank.
		const auto [lo, hi] = std::minmax_element(at50.begin(), at50.end());
		CHECK(*hi - *lo > 0.2f);

		// AND A LIVE PAD DOES NOT REBUILD WHEN MIX MOVES.
		row[mix] = 0.5f;
		ui::PhaserPad live { [](size_t, float) {}, reader, rotate };
		live.setBounds(0, 0, 420, 300);
		visage::Canvas canvas;
		live.draw(canvas);
		const size_t built = live.rebuildsForTest();
		row[mix] = 0.1f;
		live.setClockForTest(std::chrono::steady_clock::now() + std::chrono::hours(1));
		live.draw(canvas);
		CHECK(live.rebuildsForTest() == built);
	}
}
TEST_CASE("phaser pad: a key that keeps moving rebuilds at most once per interval, and the settled map is the final key's") {
	// THE FINAL REVIEW: a phaser build is 11-17 ms and Center is in the key, so a knob drag rebuilt every frame. Now a
	// moving key rebuilds at most once per `kKeyRebuildInterval`, and once more after it settles. The throttle's clock
	// is pinned (`setClockForTest`), so the frames are exactly 16 ms apart whatever the host is doing.
	namespace ph = feathers::fx::phaser;
	using namespace std::chrono_literals;
	struct RotateSpy final : feathers::device::Modulatable {
		const feathers::device::ParamDesc& d;
		explicit RotateSpy(const feathers::device::ParamDesc& desc) : d(desc) {}
		const feathers::device::ParamDesc& desc() const override { return d; }
		double value() const override { return 0.0; }
		void setValue(double) override {}
		feathers::device::ModDisplay display() const override { return {}; }
		void setAmount(float) override {}
		void beginGesture() override {}
		void endGesture() override {}
		std::string text() const override { return {}; }
	};
	RotateSpy rotate { ph::kDescriptor.params[static_cast<size_t>(ph::Param::Rotate)] };
	static_assert(ui::PhaserPad::keyRebuildInterval() == 100ms);
	static_assert(ui::ShaperPad::keyRebuildInterval() == 0ms); // the shaper names none: unchanged
	constexpr auto kInterval = ui::PhaserPad::keyRebuildInterval();
	ph::Row row = ph::rowOf(ph::State {});
	const auto reader = [&](size_t field) { return field < row.size() ? row[field] : 0.0f; };
	ui::PhaserPad pad { [](size_t, float) {}, reader, rotate };
	pad.setBounds(0, 0, 420, 300);
	visage::Canvas canvas;
	auto now = std::chrono::steady_clock::time_point {} + 1h;
	pad.setClockForTest(now);
	pad.draw(canvas); // the first build, for the size
	const size_t atRest = pad.rebuildsForTest();
	REQUIRE(atRest == 1);

	// A KNOB DRAG ON CENTER: a new value every 16 ms frame for ~1 s, then frames with the knob still for 300 ms.
	constexpr auto kFrame = 16ms;
	constexpr int kMovingFrames = 62;
	constexpr int kStillFrames = 19;
	const size_t center = static_cast<size_t>(ph::Param::CenterHz);
	const auto start = now;
	for (int f = 0; f < kMovingFrames + kStillFrames; ++f) {
		now += kFrame;
		if (f < kMovingFrames) {
			row[center] += 0.01f;
		}
		pad.setClockForTest(now);
		pad.draw(canvas);
	}
	const auto elapsed = now - start;
	const size_t rebuilds = pad.rebuildsForTest() - atRest;
	const auto bound = static_cast<size_t>((elapsed + kInterval - 1ms) / kInterval) + 1; // ceil(elapsed / interval) + 1
	CAPTURE(rebuilds);
	CAPTURE(bound);
	// AT MOST ONE PER INTERVAL — every frame would be 62.
	CHECK(rebuilds <= bound);
	// NOT VACUOUS: the terrain followed the drag on the way, roughly once an interval, not only at the end.
	CHECK(rebuilds >= 5);

	// THE SETTLED MAP IS THE FINAL KEY'S: the key the pad holds is the row's as it now is, and the cells are those of a
	// fresh pad built from the final row (an independent build, not the throttle's own record of itself).
	ph::Row finalRow = row;
	ui::PhaserPadPolicy::Phases standing {};
	for (size_t i = 0; i < standing.size(); ++i) {
		standing[i] = static_cast<double>(reader(ph::kTravelFirst + i)); // past the params: 0, as the pad reads it
	}
	CHECK(pad.builtForTest() == ui::PhaserPadPolicy::keyOf(reader, 0.0, pad.zoom(), standing));
	ui::PhaserPad fresh { [](size_t, float) {},
		                  [&](size_t field) { return field < finalRow.size() ? finalRow[field] : 0.0f; }, rotate };
	fresh.setBounds(0, 0, 420, 300);
	fresh.draw(canvas);
	CHECK(pad.mapForTest() == fresh.mapForTest());
	// AND THE DRAG MOVED IT (the comparison above is not one map compared with itself at the defaults).
	ph::Row defaults = ph::rowOf(ph::State {});
	ui::PhaserPad atDefaults { [](size_t, float) {},
		                       [&](size_t field) { return field < defaults.size() ? defaults[field] : 0.0f; }, rotate };
	atDefaults.setBounds(0, 0, 420, 300);
	atDefaults.draw(canvas);
	CHECK(pad.mapForTest() != atDefaults.mapForTest());

	// AT REST NOTHING MORE; AND A CHANGE AFTER QUIET REBUILDS AT ONCE, on the frame that sees it.
	const size_t settled = pad.rebuildsForTest();
	now += 500ms;
	pad.setClockForTest(now);
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() == settled);
	row[center] -= 0.25f;
	now += kFrame;
	pad.setClockForTest(now);
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() == settled + 1);
	// A SECOND CHANGE ONE FRAME LATER IS DEFERRED, and built from the key as it then is once the interval has passed.
	row[center] -= 0.25f;
	now += kFrame;
	pad.setClockForTest(now);
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() == settled + 1);
	now += kInterval;
	pad.setClockForTest(now);
	pad.draw(canvas);
	CHECK(pad.rebuildsForTest() == settled + 2);
	CHECK(pad.builtForTest().settings == ui::PhaserPadPolicy::terrainSettingsOf(reader));
}
TEST_CASE("Repeat's face: the card splits the band into the subdivisions over the repeating duration") {
	// The ribbon's face for Repeat: a bar split horizontally, the repeating duration below and the
	// subdivisions on top. The face
	// is asserted as GEOMETRY off the card's own readings — a rendered band carries every quantity at once.
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

	const auto publish = [&](size_t steps, int seqBars, float repeatLength, int playing,
	                         const std::vector<float>& sources, const sc::GrooveFeel& groove) {
		std::vector<float> values(sc::kReadingCount, 0.0f);
		values[static_cast<size_t>(sc::Reading::Status)] = 3.0f;
		values[static_cast<size_t>(sc::Reading::Steps)] = static_cast<float>(steps);
		values[static_cast<size_t>(sc::Reading::Step)] = static_cast<float>(playing);
		values[static_cast<size_t>(sc::Reading::SeqBars)] = static_cast<float>(seqBars);
		values[static_cast<size_t>(sc::Reading::Groove)] = static_cast<float>(groove.mode);
		values[static_cast<size_t>(sc::Reading::GrooveAmount)] = static_cast<float>(groove.amount);
		values[static_cast<size_t>(sc::Reading::RepeatLength)] = repeatLength;
		for (size_t k = 0; k < steps; ++k) {
			const size_t base = sc::stepReading(k);
			values[base] = sources[k];
			values[base + 1u] = static_cast<float>(sc::StepFlag::Known);
		}
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
	};

	SUBCASE("one place read, sixteen times: sixteen swung cells over ONE span of a sixteenth") {
		const sc::GrooveFeel swung { sc::GrooveMode::Swing16, 0.5 };
		publish(16, 1, 0.0625f, 3, std::vector<float>(16, 0.9375f), swung);
		const ui::SklitterRepeatFace face = card->repeatFaceForTest();
		// THE TOP ROW: N cells, at the band's swung edges — the IG warp, restated per edge.
		REQUIRE(face.edges.size() == 17u);
		for (size_t i = 0; i < 16; ++i) {
			CAPTURE(i);
			CHECK(face.edges[i]
			      == doctest::Approx((static_cast<double>(i) + sc::grooveStepOffset(swung, static_cast<int>(i))) / 16.0)
			             .epsilon(1e-5));
		}
		CHECK(face.edges[16] == 1.0f);
		CHECK(face.edges[1] > 1.0f / 16.0f); // and it IS swung, or the restatement above proves nothing
		// THE BOTTOM ROW: ONE SOLID BAR under all sixteen repeats of the one place (ribbon truth: the ratchet's
		// vocabulary), exactly as wide as they are.
		REQUIRE(face.spans.size() == 1u);
		CHECK(face.spans[0].from == 0.0f);
		CHECK(face.spans[0].to == 1.0f);
		CHECK(face.spans[0].first == 0u);
		CHECK(face.spans[0].last == 15u);
		CHECK(face.spans[0].source == 0.9375f);
		CHECK(face.spans[0].playing);
	}

	SUBCASE("a bar read straight in fifths: five places, so five bars, each under its own repeat") {
		std::vector<float> src;
		for (int k = 0; k < 5; ++k) {
			src.push_back(static_cast<float>(k) / 5.0f);
		}
		publish(5, 1, 0.2f, 1, src, {});
		const ui::SklitterRepeatFace face = card->repeatFaceForTest();
		REQUIRE(face.edges.size() == 6u);
		REQUIRE(face.spans.size() == 5u);
		for (size_t k = 0; k < 5; ++k) {
			CAPTURE(k);
			CHECK(face.edges[k] == doctest::Approx(static_cast<double>(k) / 5.0));
			CHECK(face.spans[k].from == doctest::Approx(static_cast<double>(k) / 5.0));
			CHECK(face.spans[k].to == doctest::Approx(static_cast<double>(k + 1) / 5.0));
			CHECK(face.spans[k].playing == (k == 1));
		}
	}

	SUBCASE("two bars: each bar's run is its own solid bar, filling that bar's half of the band") {
		publish(6, 2, 0.25f, 4, { 0.75f, 0.75f, 0.75f, 0.5f, 0.5f, 0.5f }, {});
		const ui::SklitterRepeatFace face = card->repeatFaceForTest();
		REQUIRE(face.spans.size() == 2u);
		CHECK(face.spans[0].bar == 0);
		CHECK(face.spans[0].from == doctest::Approx(0.0));
		CHECK(face.spans[0].to == doctest::Approx(0.5));
		CHECK(face.spans[0].source == 0.75f);
		CHECK_FALSE(face.spans[0].playing);
		CHECK(face.spans[1].bar == 1);
		CHECK(face.spans[1].from == doctest::Approx(0.5));
		CHECK(face.spans[1].to == doctest::Approx(1.0));
		CHECK(face.spans[1].source == 0.5f);
		CHECK(face.spans[1].playing);
	}

	SUBCASE("one place read across a bar line is still a bar per bar — a run never crosses the rule") {
		publish(4, 2, 0.25f, 0, std::vector<float>(4, 0.75f), {});
		const ui::SklitterRepeatFace face = card->repeatFaceForTest();
		REQUIRE(face.spans.size() == 2u);
		CHECK(face.spans[0].to == doctest::Approx(0.5));
		CHECK(face.spans[1].from == doctest::Approx(0.5));
	}

	SUBCASE("no repeat length is the chunk face, unchanged") {
		publish(4, 1, 0.0f, 0, { 0.75f, 0.25f, 0.5f, 0.0f }, {});
		const ui::SklitterRepeatFace face = card->repeatFaceForTest();
		CHECK(face.edges.empty());
		CHECK(face.spans.empty());
		CHECK(card->chunksForTest().size() == 4u);
	}
}
TEST_CASE("MJ: SKLITTER's record light, on the card and in the window, is the device's Recording reading and repaints when it moves") {
	// THE LIGHT IS READ, NOT INFERRED: publish `Reading::Recording` and both surfaces must say exactly it, and each must
	// report (and so redraw) the change — a light that computed right and never repainted is the bug the band had.
	namespace sc = feathers::fx::sklitter;
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t slot = f.bridge.addFxDevice(sc::kTraits.tag);
	f.bridge.refresh();
	lane->resized();
	lane->scrollCardIntoView(slot);
	lane->resized();
	auto* card = dynamic_cast<ui::SklitterCard*>(lane->cardAt(slot));
	REQUIRE(card != nullptr);
	(void)f.gui.canvasForTest()->setCollapsed(true); // the floating panel
	REQUIRE(lane->onExpandDevice != nullptr);
	lane->onExpandDevice(slot);
	auto* panel = findInTree<ui::SklitterExpanded>(f.root);
	REQUIRE(panel != nullptr);

	const auto publish = [&](float recording) {
		std::vector<float> values(sc::kReadingCount, 0.0f);
		values[static_cast<size_t>(sc::Reading::Recording)] = recording;
		f.bridge.publishDeviceReadings(slot, values.data(), values.size());
	};
	publish(0.0f);
	(void)card->updateContentForTest();
	(void)panel->updateContentForTest();
	CHECK_FALSE(card->recordLightForTest());
	CHECK_FALSE(panel->recordLightForTest());
	CHECK_FALSE(card->updateContentForTest()); // still: nothing to repaint

	publish(1.0f);
	CHECK(card->recordLightForTest());
	CHECK(panel->recordLightForTest());
	CHECK(card->updateContentForTest()); // the light moved, and nothing else did
	CHECK(panel->updateContentForTest());
	CHECK_FALSE(card->updateContentForTest());

	publish(0.0f);
	CHECK_FALSE(card->recordLightForTest());
	CHECK_FALSE(panel->recordLightForTest());
	CHECK(card->updateContentForTest());
	CHECK(panel->updateContentForTest());
}
TEST_CASE("backlog IX: the card decodes a long grain, a ratchet, and the two stacked") {
	// A long-grain effect that draws a larger grain, the ratchet (the grain repeat effect), and the two
	// stacked. The device's side — that these are what PLAYS — is
	// `test_fx_sklitter.cpp`'s; this is the card reading them back, and where each cell's pieces begin.
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

	std::vector<float> values(sc::kReadingCount, 0.0f);
	values[static_cast<size_t>(sc::Reading::Status)] = 3.0f;
	values[static_cast<size_t>(sc::Reading::Steps)] = 8.0f;
	values[static_cast<size_t>(sc::Reading::SeqBars)] = 1.0f;
	const auto put = [&](size_t k, float source, uint32_t flags, float piece) {
		values[sc::stepReading(k)] = source;
		values[sc::stepReading(k) + 1u] = static_cast<float>(flags | static_cast<uint32_t>(sc::StepFlag::Known));
		values[sc::stepReading(k) + 2u] = piece;
	};
	const auto span = [](uint32_t n) { return (n - 1u) << sc::kStepSpanShift; };
	const auto ratchet = [](uint32_t n) { return (n - 1u) << sc::kStepRatchetShift; };
	const auto covered = static_cast<uint32_t>(sc::StepFlag::Covered);
	put(0, 0.0f, 0u, 0.0f);                              // plain
	put(1, 0.125f, ratchet(3), 0.3f);                    // a ratchet of three, each 0.3 of the slot
	put(2, 0.5f, span(2), 0.0f);                         // a long grain over steps 2-3
	put(3, 0.5f, covered, 0.0f);
	put(4, 0.25f, span(4) | ratchet(4), 0.125f);         // STACKED: a long grain over 4-7, ratcheting 4 times
	put(5, 0.25f, covered, 0.0f);
	put(6, 0.25f, covered, 0.0f);
	put(7, 0.25f, covered, 0.0f);
	f.bridge.publishDeviceReadings(slot, values.data(), values.size());

	const std::vector<ui::SklitterCard::Chunk> c = card->chunksForTest();
	REQUIRE(c.size() == 8u);
	CHECK(c[0].span == 1);
	CHECK(c[0].ratchet == 1);
	CHECK(ui::sklitterRatchetStarts(c[0]).empty()); // a plain cell draws as it always did
	CHECK(c[1].ratchet == 3);
	CHECK(c[1].piece == doctest::Approx(0.3));
	const std::vector<float> three = ui::sklitterRatchetStarts(c[1]);
	REQUIRE(three.size() == 3u);
	CHECK(three[1] == doctest::Approx(0.3));
	CHECK(three[2] == doctest::Approx(0.6));
	CHECK(c[2].span == 2);
	CHECK(c[3].covered);
	CHECK(c[4].span == 4);
	CHECK(c[4].ratchet == 4);
	CHECK(c[5].covered);
	CHECK(c[7].covered);
	// THE STACKED CELL'S PIECES RUN ACROSS THE WHOLE LONG GRAIN: four restarts at eighths of its span.
	const std::vector<float> stacked = ui::sklitterRatchetStarts(c[4]);
	REQUIRE(stacked.size() == 4u);
	for (size_t j = 0; j < 4; ++j) {
		CHECK(stacked[j] == doctest::Approx(0.125 * static_cast<double>(j)));
	}
	// AND A FLOORED PIECE THAT OVERRUNS THE SLOT DRAWS ONLY THE RESTARTS THAT BEGIN INSIDE IT.
	ui::SklitterChunk floored;
	floored.ratchet = 12;
	floored.piece = 0.25f;
	CHECK(ui::sklitterRatchetStarts(floored).size() == 4u);
}
TEST_CASE("backlog IX: the card decodes a Time combine wider than sixteen steps") {
	// Time's zone override (ported 2026-09-23, `stutterer.cpp:703-712`) combines up to the bar's whole slice
	// count — 32 at the finest rate — where the hashed long grain stopped at 8, so the span field is five bits.
	// A four-bit decode reads a span of 20 as 4 and draws sixteen covered steps as nothing.
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

	std::vector<float> values(sc::kReadingCount, 0.0f);
	values[static_cast<size_t>(sc::Reading::Status)] = 3.0f;
	values[static_cast<size_t>(sc::Reading::Steps)] = 32.0f;
	values[static_cast<size_t>(sc::Reading::SeqBars)] = 1.0f;
	const auto known = static_cast<uint32_t>(sc::StepFlag::Known);
	const auto covered = static_cast<uint32_t>(sc::StepFlag::Covered);
	for (size_t k = 0; k < 32; ++k) {
		uint32_t flags = known;
		if (k == 0) {
			flags |= (20u - 1u) << sc::kStepSpanShift; // one combine over steps 0-19
		}
		else if (k < 20) {
			flags |= covered;
		}
		else if (k == 20) {
			flags |= (12u - 1u) << sc::kStepSpanShift; // and one over 20-31
		}
		else {
			flags |= covered;
		}
		values[sc::stepReading(k) + 1u] = static_cast<float>(flags);
	}
	f.bridge.publishDeviceReadings(slot, values.data(), values.size());

	const std::vector<ui::SklitterCard::Chunk> c = card->chunksForTest();
	REQUIRE(c.size() == 32u);
	CHECK(c[0].span == 20);
	CHECK_FALSE(c[0].covered);
	CHECK(c[19].covered);
	CHECK(c[20].span == 12);
	CHECK_FALSE(c[20].covered);
	CHECK(c[31].covered);
}
TEST_CASE("ribbon truth: a ratchet cell is a solid bar under its pieces, and a step under a long grain draws nothing") {
	// It is meant to be a simple solid bar on the bottom with the subdivisions on top; the bottom drew
	// partially filled and partially greyed, with partially shaded empty cells. RENDERED and read
	// back, because what was wrong was pixels: a faint remainder beside the piece, gaps on top.
	constexpr int kW = 400;
	constexpr int kH = 120;
	visage::Canvas canvas;
	markedWindowless(canvas, kW, kH);
	canvas.setColor(ui::kSurfaceRaised); // the band's bed, which is also the tick's colour
	canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));

	// A RATCHET OF FOUR, each a quarter of the slot: the cell at (40, 10), 320 x 40.
	ui::SklitterChunk ratchet;
	ratchet.source = 0.4f;
	ratchet.known = true;
	ratchet.ratchet = 4;
	ratchet.piece = 0.25f;
	ui::drawSklitterCell(canvas, ratchet, 40.0f, 10.0f, 320.0f, 40.0f, 6.0f);
	// AND A RATCHET OF NINE IN A SIX-PIXEL CELL — the width a sixteenth has on a four-bar band. Eight ticks
	// cannot resolve there; drawn anyway they were a dark top with coloured edges, a ghost of an empty cell.
	ui::SklitterChunk narrow = ratchet;
	narrow.ratchet = 9;
	narrow.piece = 1.0f / 9.0f;
	ui::drawSklitterCell(canvas, narrow, 380.0f, 10.0f, 6.0f, 40.0f, 6.0f);

	// A FACE OF FOUR STEPS at (40, 70), 320 x 40: a plain step, then a long grain over steps 1-3. The covered
	// steps carry a DIFFERENT source and the reverse flag, so anything they drew of their own would show.
	std::vector<ui::SklitterChunk> picture(4);
	for (ui::SklitterChunk& c : picture) {
		c.known = true;
	}
	picture[0].source = 0.1f;
	picture[1].source = 0.6f;
	picture[1].span = 3;
	for (size_t k = 2; k < 4; ++k) {
		picture[k].covered = true;
		picture[k].source = 0.95f;
		picture[k].reversed = true;
	}
	ui::drawSklitterChunkFace(canvas, picture, { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f }, 40.0f, 70.0f, 320.0f, 40.0f, 66.0f,
	                          true);

	const visage::Screenshot shot = takeMarkedCapture(canvas, kW, kH);
	REQUIRE(shot.width() == kW);
	if (const char* path = std::getenv("FEATHERS_RIBBON_CELL_SHOT")) {
		shot.save(path); // a camera for LOOKING at what the assertions below measure; off unless named
	}
	const auto is = [&](int x, int y, uint32_t argb) {
		const visage::Color c = shot.sample(x, y);
		const auto near = [](unsigned got, uint32_t want) {
			return std::abs(static_cast<int>(got) - static_cast<int>(want & 0xFFu)) <= 3;
		};
		return near(c.hexRed(), argb >> 16) && near(c.hexGreen(), argb >> 8) && near(c.hexBlue(), argb);
	};
	const uint32_t fill = ui::sklitterRamp(ratchet.source);
	REQUIRE_FALSE(is(200, 5, fill)); // the bed is not the fill, or nothing below can tell them apart

	// THE BOTTOM HALF: ONE SOLID COLOUR ACROSS THE CELL'S WHOLE WIDTH — every column, on three rows of it.
	const float half = (40.0f - ui::kSklitterSplitGap) * 0.5f;
	const auto lowTop = static_cast<int>(std::ceil(10.0f + half + ui::kSklitterSplitGap));
	int offBottom = 0;
	for (const int y : { lowTop + 2, lowTop + 9, 50 - 3 }) {
		for (int x = 42; x < 358; ++x) {
			offBottom += is(x, y, fill) ? 0 : 1;
		}
	}
	CHECK(offBottom == 0);

	// THE TOP HALF: the same colour, cut into FOUR by exactly THREE ticks, each where its restart begins.
	std::vector<int> ticks;
	bool inTick = false;
	int offTop = 0;
	const int topRow = 10 + static_cast<int>(half * 0.5f);
	for (int x = 42; x < 358; ++x) {
		const bool filled = is(x, topRow, fill);
		if (!filled && !is(x, topRow, ui::kSurfaceRaised)) {
			++offTop; // neither the fill nor a tick: a faint or partial shade
		}
		if (!filled && !inTick) {
			ticks.push_back(x);
		}
		inTick = !filled;
	}
	CHECK(offTop == 0);
	// THE NARROW ONE IS SOLID TOP AND BOTTOM: no tick where none can resolve, and no shade.
	int offNarrow = 0;
	for (const int y : { topRow, lowTop + 9 }) {
		for (int x = 381; x < 385; ++x) {
			offNarrow += is(x, y, fill) ? 0 : 1;
		}
	}
	CHECK(offNarrow == 0);
	REQUIRE(ticks.size() == 3u);
	for (size_t j = 0; j < 3; ++j) {
		CAPTURE(j);
		CHECK(std::abs(ticks[j] - (40 + 80 * static_cast<int>(j + 1))) <= 1);
	}

	// THE LONG GRAIN: one cell in its own colour over steps 1-3 — nothing of the covered steps', no hairline
	// at their edges (x = 200 and 280), no reverse notch.
	const uint32_t grain = ui::sklitterRamp(0.6f);
	int offGrain = 0;
	for (const int y : { 76, 90, 104 }) {
		for (int x = 123; x < 356; ++x) {
			offGrain += is(x, y, grain) ? 0 : 1;
		}
	}
	CHECK(offGrain == 0);
}
TEST_CASE("ratchet count: a cell's numeral is its count where ticks cannot resolve and the cell is 8 device px, and never below") {
	// 2026-09-24, (a): the count as a numeral where the cell is wide enough — in DEVICE pixels, so a 1.75x
	// display gets numerals the 1x one does not — and nothing where the ticks already count it.
	ui::SklitterChunk c;
	c.known = true;
	c.ratchet = 4;
	c.piece = 0.25f;
	CHECK(ui::sklitterCountNumeral(c, 40.0f, 1.0f) == 0); // pieces of 10 px: the three ticks ARE the count
	CHECK(ui::sklitterCountNumeral(c, 10.0f, 1.0f) == 4); // pieces of 2.5 px: no ticks, 10 device px: the numeral
	CHECK(ui::sklitterCountNumeral(c, 6.0f, 1.0f) == 0);  // 6 device px: below the threshold
	CHECK(ui::sklitterCountNumeral(c, 6.0f, 1.75f) == 4); // the same cell on the development display: 10.5 device px
	CHECK(ui::sklitterCountNumeral(c, 8.0f / 1.75f - 0.01f, 1.75f) == 0); // just under 8 device px
	ui::SklitterChunk plain = c;
	plain.ratchet = 1;
	plain.piece = 0.0f;
	CHECK(ui::sklitterCountNumeral(plain, 20.0f, 1.0f) == 0);
	ui::SklitterChunk dry = c;
	dry.dry = true;
	CHECK(ui::sklitterCountNumeral(dry, 10.0f, 1.0f) == 0); // a dry slot does not restart (ribbon truth)

	// RENDERED: a cell over the threshold carries ink on its top half; one under it is solid there.
	constexpr int kW = 200;
	constexpr int kH = 60;
	visage::Canvas canvas;
	markedWindowless(canvas, kW, kH);
	canvas.setColor(ui::kSurfaceRaised);
	canvas.fill(0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH));
	const visage::Font& font = ui::font(ui::kKnobLabelSize); // the size the card and the panel pass
	ui::SklitterChunk nine = c;
	nine.ratchet = 9;
	nine.piece = 1.0f / 9.0f;
	ui::drawSklitterCell(canvas, nine, 20.0f, 10.0f, 16.0f, 40.0f, 6.0f, &font); // 16 px: a numeral
	ui::drawSklitterCell(canvas, nine, 100.0f, 10.0f, 6.0f, 40.0f, 6.0f, &font); // 6 px: none
	const visage::Screenshot shot = takeMarkedCapture(canvas, kW, kH);
	REQUIRE(shot.width() == kW);
	const uint32_t fill = ui::sklitterRamp(nine.source);
	const auto isFill = [&](int x, int y) {
		const visage::Color px = shot.sample(x, y);
		const auto near = [](unsigned got, uint32_t want) {
			return std::abs(static_cast<int>(got) - static_cast<int>(want & 0xFFu)) <= 3;
		};
		return near(px.hexRed(), fill >> 16) && near(px.hexGreen(), fill >> 8) && near(px.hexBlue(), fill);
	};
	int inkWide = 0;
	int inkNarrow = 0;
	for (int y = 11; y < 29; ++y) {
		for (int x = 22; x < 34; ++x) {
			inkWide += isFill(x, y) ? 0 : 1;
		}
		for (int x = 101; x < 105; ++x) {
			inkNarrow += isFill(x, y) ? 0 : 1;
		}
	}
	CHECK(inkWide > 4);    // the numeral is there
	CHECK(inkNarrow == 0); // and below the threshold nothing is
}
TEST_CASE("ratchet count: the head's xN is the HEARD count of the playing slot, off a real device's publish") {
	// 2026-09-24, (b). The device publishes the head's cell from the slot playing (`heardRatchet`); the plan's
	// own count can be more — a floored piece overruns the slot, a piece in the 15 ms release zone never sounds —
	// and a tag that printed that would name restarts nobody hears. RESTATED here from the slot's own numbers.
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

	size_t fewer = 0;   // slots whose plan asks for more restarts than are heard
	size_t checked = 0;
	// (Zone A, Zone B, the second waypoint's configuration with the puck on it, or centred below zero) — settings
	// the ratchet reaches, some with pieces floored past the slot's audible end.
	for (const auto& [za, zb, puck] : std::initializer_list<std::tuple<float, float, float>> {
	         { 0.05f, 0.8f, 0.4f }, { 0.55f, 0.8f, 0.4f }, { 0.95f, 0.2f, -1.0f }, { 0.05f, 0.5f, -1.0f } }) {
		sc::Sklitter s;
		s.prepare(48000.0, 4096);
		sc::Settings cfg;
		cfg.mode = sc::Mode::Shuffle;
		cfg.enabled = true;
		cfg.engage = true;
		cfg.rate = 0.5f;
		cfg.density = 1.0f;
		cfg.pWrite = 0.0f;
		cfg.zoneA = za;
		cfg.zoneB = zb;
		if (puck >= 0.0f) {
			cfg.cornerCfg[sc::kCfg2] = puck;
			const auto [ax, ay] = sc::waypointAxis(sc::kCfg2);
			cfg.perfX = ax;
			cfg.perfY = ay;
		}
		s.setTarget(cfg);
		std::vector<float> held(sc::kReadingCount, 0.0f);
		s.setReadingsSink(device::ReadingsSink { &held, [](void* ctx, const float* v, size_t n) {
			                                         auto* out = static_cast<std::vector<float>*>(ctx);
			                                         std::copy(v, v + std::min(n, out->size()), out->begin());
		                                         } });
		std::vector<float> l(480);
		std::vector<float> r(480);
		double beats = 0.0;
		for (int block = 0; block < 2000; ++block) {
			for (size_t k = 0; k < l.size(); ++k) {
				l[k] = r[k] = std::sin(static_cast<float>(block * 480 + static_cast<int>(k)) * 0.01f) * 0.5f;
			}
			device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(0.5);
			clock.songPositionBeats = beats;
			clock.playing = true;
			s.setTransportClock(clock);
			s.processStereo(l.data(), r.data(), 480);
			s.publishReadings();
			beats += 480.0 / 24000.0;
			if (s.statusForTest() != sc::Status::Playing || s.sliceIsDryForTest()) {
				continue;
			}
			const sc::SlicePlan& plan = s.planForTest();
			const size_t slotLen = s.scheduleForTest().sliceLength;
			const size_t bar = s.bufferFramesForTest();
			const auto audible = static_cast<size_t>(static_cast<double>(slotLen) * plan.lengthMult);
			// clear of the bar-end zone, where a second silence would enter the restatement
			if (plan.subdivisions <= 1 || s.slotStartFrameForTest() + audible + 2400 > bar) {
				continue;
			}
			// THE HEARD COUNT, RESTATED: pieces of `audible / count` floored at 24 ms, each heard if it starts before
			// the release zone (15 ms at 48 kHz) at the audible end.
			const size_t piece = s.subSliceLengthForTest(audible, static_cast<size_t>(plan.subdivisions));
			const size_t sounds = audible > 720 ? audible - 720 : audible;
			const auto heard = static_cast<int>(std::min<size_t>(static_cast<size_t>(plan.subdivisions),
			                                                     (sounds + piece - 1) / piece));
			f.bridge.publishDeviceReadings(slot, held.data(), held.size());
			CAPTURE(plan.subdivisions);
			CAPTURE(heard);
			const std::string want = heard > 1 ? "x" + std::to_string(heard) : "";
			CHECK(card->headLabelForTest() == want);
			// THE PANEL'S TAG IS THE SAME FUNCTION OVER THE SAME READINGS
			CHECK(ui::sklitterHeadRatchetLabel(card->chunksForTest(), card->playingStepForTest()) == want);
			fewer += heard < plan.subdivisions ? 1u : 0u;
			++checked;
		}
	}
	MESSAGE("ratcheting blocks checked " << checked << ", plan count above the heard one in " << fewer);
	CHECK(checked >= 6u);
	CHECK(fewer > 0u); // the plan's count and the heard one parted company, or this could not tell them apart
}
TEST_CASE("IW: on every card, a zoned cell shows a SIGIL exactly when it has no modes — and never both a sigil and a dropdown") {
	// The dropdown selects a mode, not a phase region; a Free-only control has no dropdown, and the room
	// goes to a sigil for the lap. Swept over every device's
	// card, and PINNED for the Eroder by name, whose Tone and Character had eight firmware names and now have none.
	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	size_t cellsSeen = 0;
	size_t eroderSigils = 0;
	for (const device::Descriptor* desc : probeLane->addableForTest()) {
		CAPTURE(std::string(desc->traits.tag));
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(desc->traits.tag);
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		std::vector<const ui::ZoneCoordCell*> cells;
		const std::function<void(const visage::Frame*)> walk = [&](const visage::Frame* frame) {
			if (const auto* cell = dynamic_cast<const ui::ZoneCoordCell*>(frame)) {
				cells.push_back(cell);
			}
			for (const visage::Frame* child : frame->children()) {
				walk(child);
			}
		};
		walk(card);
		for (const ui::ZoneCoordCell* cell : cells) {
			const std::string name = cell->box().control().desc().name;
			CAPTURE(name);
			++cellsSeen;
			CHECK((cell->sigil() != nullptr) == !cell->hasNames());
			if (cell->sigil() != nullptr) {
				CHECK_FALSE(cell->select().isVisible());
			}
			if (name == "Eroder Tone" || name == "Eroder Character") {
				CHECK(cell->sigil() != nullptr);
				eroderSigils += cell->sigil() != nullptr;
			}
		}
	}
	CHECK(cellsSeen > 0);
	CHECK(eroderSigils == 2); // NON-VACUITY: the Eroder's card was reached and both of its coordinates seen
}
TEST_CASE("IQ: a drag and a menu pick cannot disagree about the zone that plays — the Eroder has no label, OWLTT's pick is the zone") {
	// Dragging through the range stepped the select box's values, but while its label changed it felt as
	// though a pick from the box was still needed to get the true change. Measured then: the
	// Eroder's names were LABELS over a continuum, and just past a boundary the sound sat nearer the PREVIOUS zone's
	// centre — the label ran ahead of the ear. IW's answer removed the label rather than moving it. What this pins,
	// through the widgets' own roads and by value: (1) the Eroder's drag writes the coordinate WHILE the hand moves
	// and there is no zone name to run ahead of it; (2) on OWLTT, where a pick still exists, the pick IS the zone the
	// engine reads, and a drag of the value moves the amount and lap inside it without changing the label or the zone.
	SUBCASE("Eroder: a drag across a zone boundary writes the coordinate mid-gesture, and no zone name is shown") {
		namespace ero = feathers::fx::eroder;
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		REQUIRE(lane != nullptr);
		const size_t slot = f.bridge.addFxDevice(ero::kTraits.tag);
		REQUIRE(slot == 1);
		lane->resized();
		lane->scrollCardIntoView(slot);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		ui::ZoneCoordCell* tone = nullptr;
		const std::function<void(visage::Frame*)> walk = [&](visage::Frame* frame) {
			if (auto* cell = dynamic_cast<ui::ZoneCoordCell*>(frame);
			    cell != nullptr && std::string(cell->box().control().desc().name) == "Eroder Tone") {
				tone = cell;
			}
			for (visage::Frame* child : frame->children()) {
				walk(child);
			}
		};
		walk(card);
		REQUIRE(tone != nullptr);
		CHECK_FALSE(tone->hasNames());
		CHECK_FALSE(tone->select().isVisible());
		REQUIRE(tone->sigil() != nullptr);

		const auto stored = [&] {
			return static_cast<double>(f.bridge.fxChain().device[slot].row.field[static_cast<size_t>(ero::Param::Tone)]);
		};
		const auto zoneOf = [](double v) { return static_cast<long>(std::floor(v * static_cast<double>(ero::kZones))); };
		ui::ValueBox& box = tone->box();
		const float x = box.width() * 0.5f;
		const float y = box.height() * 0.5f;
		const double start = stored();
		box.onMouseDown().callback(at(x, y));
		float dy = 0.0f;
		while (zoneOf(stored()) == zoneOf(start) && dy < 2000.0f) {
			dy += 4.0f;
			box.onMouseDrag().callback(at(x, y - dy));
			// MID-GESTURE: what the engine reads (the row) is what the box shows, before any release.
			CHECK(stored() == doctest::Approx(box.control().value()));
		}
		const double crossed = stored();
		CHECK(zoneOf(crossed) != zoneOf(start)); // NON-VACUITY: the drag did cross a boundary
		box.onMouseUp().callback(at(x, y - dy));
		CHECK(stored() == crossed); // the release adds nothing: the change was already made
		CHECK_FALSE(tone->select().isVisible());
	}

	SUBCASE("OWLTT: a pick is the zone the engine reads; a drag moves the amount and the lap, never the zone or its name") {
		namespace dtt = feathers::fx::owltt;
		FilterFixture f;
		ui::WidgetRegistry registry;
		REQUIRE(f.bridge.addFxDevice(dtt::kTraits.tag) == 1);
		ui::OwlttExpanded panel { f.deviceHost, registry, f.bridge.fxDeviceAt(1) };
		panel.setBounds(0.0f, 0.0f, panel.minimumSize().x, panel.minimumSize().y);
		panel.resized();
		REQUIRE(panel.modeCellsForTest().size() == 2);
		ui::ModeValueCell& character = *panel.modeCellsForTest().front();
		ui::ValueBox& box = *character.boxFor(static_cast<size_t>(dtt::Param::Character));

		const auto row = [&] {
			dtt::Row r {};
			const auto field = f.bridge.fxChain().device[1].row.field; // BY VALUE: `fxChain()` returns a copy
			std::copy_n(field.begin(), r.size(), r.begin());
			return r;
		};
		// WHAT THE ENGINE READS: the settings the row decodes to (`stateOf`, the engine's own decode) through
		// `zoneModeCoordinate`, which is the whole of `Owltt::characterCoordinate` (private; owltt.h:266).
		const auto engineZone = [&] {
			const dtt::Settings s = dtt::stateOf(row());
			const double c = dtt::zoneModeCoordinate(s.characterMode, s.character);
			return static_cast<long>(std::floor((c - std::floor(c)) * 8.0));
		};
		const auto storedMode = [&] { return static_cast<long>(std::lround(row()[static_cast<size_t>(dtt::Param::CharacterMode)])); };
		const auto storedValue = [&] { return static_cast<double>(row()[static_cast<size_t>(dtt::Param::Character)]); };

		for (const size_t pick : { size_t { 3 }, size_t { 5 }, size_t { 0 } }) {
			CAPTURE(pick);
			const double valueBefore = storedValue();
			character.select().selectZone(pick); // the menu's own road (`ZoneSelect::pressAt`'s popup calls exactly this)
			CHECK(storedMode() == static_cast<long>(pick));
			CHECK(character.select().captionText() == dtt::kCharacterModeNames[pick]);
			CHECK(engineZone() == static_cast<long>(pick));
			CHECK(storedValue() == valueBefore); // a pick moves the MODE, not the value a drag writes

			// A DRAG WORTH MORE THAN A ZONE: under the old region box this would have rolled the label on.
			const double dragStart = storedValue();
			const float x = box.width() * 0.5f;
			const float y = box.height() * 0.5f;
			box.onMouseDown().callback(at(x, y));
			float dy = 0.0f;
			while (std::abs(storedValue() - dragStart) < 0.3 && dy < 4000.0f) {
				dy += 4.0f;
				box.onMouseDrag().callback(at(x, y - dy));
				CHECK(storedMode() == static_cast<long>(pick));
				CHECK(engineZone() == static_cast<long>(pick));
			}
			box.onMouseUp().callback(at(x, y - dy));
			CHECK(std::abs(storedValue() - dragStart) >= 0.3); // NON-VACUITY: the value really travelled
			CHECK(character.select().captionText() == dtt::kCharacterModeNames[pick]);
			CHECK(engineZone() == static_cast<long>(pick));
		}
	}
}
TEST_CASE("IW: the Shaper pad's terrain is the curve the MODE builds — Inflate enters its key and changes its picture") {
	// The terrain derives curves itself, so it has to learn Colour's mode the way the engine does (`curveOf`), or it
	// paints the field while the device plays the inflator. By its key's own rule the mode is a terrain input.
	namespace shp = feathers::fx::shaper;
	ShaperFixture f;
	REQUIRE(f.panel != nullptr);
	auto* pad = const_cast<ui::ShaperPad*>(f.panel->padForTest());
	REQUIRE(pad != nullptr);
	visage::Canvas canvas;
	const auto setMode = [&](uint32_t mode) {
		f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::ColourMode), static_cast<float>(mode));
		pad->draw(canvas);
	};
	// **THE FREE MAP IS REBUILT AT THE SAME GEOMETRY BEFORE IT IS COMPARED.** The first draft compared against the
	// map the pad was CONSTRUCTED with, and any rebuild moves most of that one (the construction map predates the
	// pad's final size) — so a known-bad that dropped Inflate from the terrain stayed green, 1366 of 1536 cells
	// "moved" without it. A round trip through Inflate and back gives a Free map built exactly as the Inflate one is.
	setMode(shp::kColourInflate);
	setMode(shp::kColourFree);
	CHECK(pad->builtForTest().inflate == -1.0); // Free
	const auto free = pad->mapForTest();
	const size_t settled = pad->rebuildsForTest();

	setMode(shp::kColourInflate);
	CHECK(pad->rebuildsForTest() > settled);
	CHECK(pad->builtForTest().inflate == 1.0); // the amount's default: the full inflator
	size_t moved = 0;
	for (size_t i = 0; i < free.size(); ++i) {
		moved += pad->mapForTest()[i] != free[i];
	}
	CAPTURE(moved);
	CHECK(moved * 4 > free.size()); // Inflate changed the picture across most of the map, not at an edge
	// AND BACK TO FREE REPRODUCES THE FREE MAP EXACTLY — so the difference above is the mode's, not a rebuild's.
	setMode(shp::kColourFree);
	CHECK(pad->mapForTest() == free);

	setMode(shp::kColourInflate); // the amount is read in Inflate
	f.f.bridge.setDeviceControl(f.device, static_cast<size_t>(shp::Param::InflateAmount), 0.3f);
	pad->draw(canvas);
	CHECK(pad->builtForTest().inflate == doctest::Approx(0.3));
}
TEST_CASE("PHOTO: every card's head, open and folded, in each lane (writes only under FEATHERS_HEAD_SHOT_DIR)") {
	// **A CAMERA, NOT A CLAIM.** Text and widgets collided in the card headers. This writes a picture of every card the three lanes can hold, both ways up, so a head
	// can be LOOKED at before and after a change — the rectangles are asserted by the head sweep. Off
	// unless the directory is named, so an ordinary run writes nothing.
	const char* dir = std::getenv("FEATHERS_HEAD_SHOT_DIR");
	if (dir == nullptr) {
		return;
	}
	const std::string out = dir;

	ModFixture probe;
	auto* probeLane = findInTree<ui::FxLane>(probe.root);
	REQUIRE(probeLane != nullptr);
	std::vector<std::string> fxTags;
	for (const device::Descriptor* d : probeLane->addableForTest()) {
		fxTags.emplace_back(d->traits.tag);
	}
	for (const std::string& tag : fxTags) {
		CAPTURE(tag);
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		const size_t slot = f.bridge.addFxDevice(tag.c_str());
		REQUIRE(slot < feathers::kMaxFxSlots);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		photographCard(*card, out + "/fx-" + tag + "-open.png");
		card->setFolded(true);
		lane->resized();
		photographCard(*card, out + "/fx-" + tag + "-folded.png");
	}

	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);
	const std::vector<const device::Descriptor*> offered = column.addable();
	for (size_t i = 0; i < offered.size(); ++i) {
		const std::string tag = offered[i]->traits.tag;
		CAPTURE(tag);
		const DeviceInstanceId added = column.addChoiceForTest(i);
		if (added == feathers::kNoInstance) {
			continue;
		}
		column.resized();
		ui::FxCard* card = column.cardOf(added);
		REQUIRE(card != nullptr);
		photographCard(*card, out + "/sklitter-" + tag + "-open.png");
		card->setFolded(true);
		column.resized();
		photographCard(*card, out + "/sklitter-" + tag + "-folded.png");
		// AND AT 120, where a long name has to elide — the one place the head's text gives way.
		card->setBounds(card->x(), card->y(), 120.0f, card->height());
		photographCard(*card, out + "/sklitter-" + tag + "-folded-120.png");
		CHECK(column.slotChoiceForTest(added, ui::ChainColumn::SlotVerb::Remove));
		column.resized();
	}

	// THE MODULATOR STRIP, whose head menu is the HOST's rather than the card's — the one lane whose
	// buttons are placed by a different rectangle, so the one to look at beside the fx lane's.
	for (const device::Descriptor& d : kDevices) {
		if (d.mod == nullptr) {
			continue;
		}
		const std::string tag = d.traits.tag;
		CAPTURE(tag);
		ModFixture f;
		size_t slot = 0;
		if (!feathers::device::sameTag(d.traits.tag, feathers::mod::dahdsr::kTraits.tag)) {
			slot = f.bridge.addRackDevice(d.traits.tag);
			if (slot >= feathers::kMaxRackSlots) {
				continue;
			}
		}
		f.panel->resized();
		auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(f.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		photographCard(*card, out + "/mod-" + tag + "-open.png");
	}
}
TEST_CASE("THE HEAD SWEEP: nothing in any card's head overlaps anything else, in every lane and every state") {
	// Text and widgets collided in the headers — for instance an unneeded "21 controls" caption on
	// Delay ran into the other widgets. It did, and not only on Delay: every card that fell to `GenericCard`
	// (Delay, Eroder, Pitch, SKLITTER, Note Expressions, the tracker) printed "N controls" into a box
	// that ran from x = 0 to the slot menu, straight under the enable square; and the chain card's
	// "empty" sat under the enable square folded and under the expand mark open. Photographed on xvfb
	// before the fix — the report names the files.
	//
	// **ONE QUESTION PER HEAD, ASKED OF EVERY CARD EACH LANE CAN HOLD, IN EACH STATE** — the lanes are
	// walked through their own "what can I add" lists, so a device registered tomorrow joins the sweep
	// without anyone editing it. The question itself is `test::checkHead`, shared with the rack
	// editor's sweep: marks and MEASURED text inside the head, pairwise disjoint, each text inside its
	// box, and no child frame over the head.
	//
	// **AND NARROWER THAN ANY LANE GRANTS TODAY**, down to where the name has to elide, because a card
	// width is a host's choice and the next host will choose differently. The name is the one occupant
	// allowed to give way — to "..." and then to nothing — and the caption goes first.
	const bool printTable = std::getenv("FEATHERS_HEAD_TABLE") != nullptr;
	const auto sweep = [&](ui::FxCard& card, const std::string& where) {
		test::checkHead(card, where);
		if (printTable) {
			MESSAGE(where << " (" << card.width() << "x" << card.height() << ")\n" << test::headTable(card));
		}
	};

	SUBCASE("Feathers' fx lane: every device it offers, open and folded, at the lane's width and narrower") {
		ModFixture probe;
		auto* probeLane = findInTree<ui::FxLane>(probe.root);
		REQUIRE(probeLane != nullptr);
		std::vector<std::string> tags;
		for (const device::Descriptor* d : probeLane->addableForTest()) {
			tags.emplace_back(d->traits.tag);
		}
		REQUIRE(tags.size() >= 8);
		for (const std::string& tag : tags) {
			CAPTURE(tag);
			ModFixture f;
			auto* lane = findInTree<ui::FxLane>(f.root);
			const size_t slot = f.bridge.addFxDevice(tag.c_str());
			REQUIRE(slot < feathers::kMaxFxSlots);
			lane->resized();
			ui::FxCard* card = lane->cardAt(slot);
			REQUIRE(card != nullptr);
			// THE LANE'S OWN WIDTH, which is `FxLane::kWidth` less its padding — the narrow lane.
			CHECK(card->width() <= ui::FxLane::kWidth);
			for (const bool folded : { false, true }) {
				card->setFolded(folded);
				lane->resized();
				const std::string state = folded ? " folded" : " open";
				sweep(*card, "fx lane " + tag + state);
				const float granted = card->width();
				// DOWN TO 120, where a card with a source dot and three buttons has ~30px left and its
				// name elides to one letter and dots. Below that the name has no room at all — MEASURED:
				// SKLITTER at 90 lays out an empty name box — and a head with no name is not a head a
				// lane should grant, so the sweep stops where a name can still be read from.
				for (const float w : { 200.0f, 160.0f, 120.0f }) {
					card->setBounds(card->x(), card->y(), w, card->height());
					sweep(*card, "fx lane " + tag + state + " at " + std::to_string(static_cast<int>(w)));
				}
				card->setBounds(card->x(), card->y(), granted, card->height());
			}
		}
	}

	SUBCASE("SKLITTER's inner lanes, post and grain: every device each offers, open and folded") {
		HeldChain held;
		for (const bool grain : { false, true }) {
			ui::WidgetRegistry registry;
			ui::ModState mod;
			ui::EditHistory history;
			ui::ChainColumn column(grain ? held.grainBridge() : held.bridge(), registry, mod, history,
			                       nullptr);
			column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);
			const std::vector<const device::Descriptor*> offered = column.addable();
			REQUIRE_FALSE(offered.empty());
			for (size_t i = 0; i < offered.size(); ++i) {
				const std::string tag = offered[i]->traits.tag;
				CAPTURE(tag);
				const DeviceInstanceId added = column.addChoiceForTest(i);
				REQUIRE(added != feathers::kNoInstance);
				column.resized();
				ui::FxCard* card = column.cardOf(added);
				REQUIRE(card != nullptr);
				const std::string lane = grain ? "SKLITTER grain lane " : "SKLITTER post lane ";
				sweep(*card, lane + tag + " open");
				card->setFolded(true);
				column.resized();
				sweep(*card, lane + tag + " folded");
				CHECK(column.slotChoiceForTest(added, ui::ChainColumn::SlotVerb::Remove));
				column.resized();
			}
		}
	}

	SUBCASE("the modulator strip: every modulator, open and minimised to its spine") {
		size_t swept = 0;
		for (const device::Descriptor& d : kDevices) {
			if (d.mod == nullptr) {
				continue;
			}
			const std::string tag = d.traits.tag;
			CAPTURE(tag);
			ModFixture f;
			size_t slot = 0;
			if (!feathers::device::sameTag(d.traits.tag, feathers::mod::dahdsr::kTraits.tag)) {
				slot = f.bridge.addRackDevice(d.traits.tag);
				if (slot >= feathers::kMaxRackSlots) {
					continue; // the strip does not take it
				}
			}
			f.panel->resized();
			auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(f.panel->cardFrameForTest(slot)));
			REQUIRE(card != nullptr);
			sweep(*card, "modulator strip " + tag + " open");
			const backplane::Source source = backplane::Source::output(f.bridge.rackDeviceAt(slot), 0);
			REQUIRE(f.panel->setCardMinimised(source, true));
			f.panel->resized();
			sweep(*card, "modulator strip " + tag + " minimised");
			++swept;
		}
		CHECK(swept >= 3);
	}

	SUBCASE("a scale change moves nothing in the head: the layout is logical and the card's native width is 1.75x") {
		// "A SCALE CHANGE MOVES NOTHING LOGICAL" (CLAUDE.md). The head is painted by the card in its own
		// logical coordinates, so at 1.75x every rectangle must be the one at 1x and the card itself
		// 1.75x as wide natively — the claim is NATIVE, because the logical one cannot change.
		ModFixture f;
		auto* lane = findInTree<ui::FxLane>(f.root);
		const size_t slot = f.bridge.addFxDevice("DELAY");
		REQUIRE(slot < feathers::kMaxFxSlots);
		lane->resized();
		ui::FxCard* card = lane->cardAt(slot);
		REQUIRE(card != nullptr);
		const std::string before = test::headTable(*card);
		const float logical = card->width();
		lane->setDpiScale(1.75f);
		lane->resized();
		CHECK(test::headTable(*card) == before);
		CHECK(std::abs(static_cast<float>(card->nativeWidth()) - 1.75f * logical) <= 1.0f);
		sweep(*card, "fx lane DELAY at 1.75x");
	}
}

namespace {
/// THE HEAD'S BUTTONS — expand, enable and whichever menu is there — right to left, as `headLayout`
/// places them. The tint bar and the source dot sit at the LEFT end and are not part of the row.
std::vector<ui::FxCard::HeadPart> headButtonsRightToLeft(const ui::FxCard& card) {
	std::vector<ui::FxCard::HeadPart> out;
	for (const ui::FxCard::HeadPart& m : card.headLayout().marks) {
		const std::string what = m.what;
		if (what == "expand" || what == "enable" || what == "slot menu" || what == "host menu") {
			out.push_back(m);
		}
	}
	std::sort(out.begin(), out.end(),
	          [](const ui::FxCard::HeadPart& a, const ui::FxCard::HeadPart& b) { return a.box.x() > b.box.x(); });
	return out;
}
} // namespace

TEST_CASE("a modulator card's head buttons sit flush right, at an fx card's inset and spacing") {
	// Mod card header widgets were not aligned full right like the audio effects'. The fx lane's menu is the card's own (`slotMenuBounds`, 12px, 10px off the right edge);
	// the strip's is the HOST's (`ui::cardMenuBounds`, 18px), which still stood where it was put when
	// the retired fold mark held the corner: `kModCardPadX + 2·18 + 2` off the edge. MEASURED before the
	// fix, both cards 216px wide: fx menu right inset 10, enable 28, expand 46; strip menu (18px) right
	// inset 29, enable 53, expand 71 (SKLITTER) — the row 25px inboard, the painted dots 22px.
	//
	// ASSERTED BY VALUE against an fx card set to the strip card's width, so the claim is "the same
	// row", not a number copied here: the rightmost button's inset, each button's size, and every gap.
	ModFixture f;
	auto* lane = findInTree<ui::FxLane>(f.root);
	REQUIRE(lane != nullptr);
	const size_t fxSlot = f.bridge.addFxDevice("OWLTT"); // all three buttons: expand, enable, menu
	REQUIRE(fxSlot < feathers::kMaxFxSlots);
	lane->resized();
	ui::FxCard* fx = lane->cardAt(fxSlot);
	REQUIRE(fx != nullptr);

	size_t compared = 0;
	for (const device::Descriptor& d : kDevices) {
		if (d.mod == nullptr) {
			continue;
		}
		const std::string tag = d.traits.tag;
		CAPTURE(tag);
		ModFixture m;
		size_t slot = 0;
		if (!feathers::device::sameTag(d.traits.tag, feathers::mod::dahdsr::kTraits.tag)) {
			slot = m.bridge.addRackDevice(d.traits.tag);
			if (slot >= feathers::kMaxRackSlots) {
				continue; // the strip does not take it
			}
		}
		m.panel->resized();
		auto* card = dynamic_cast<ui::FxCard*>(const_cast<visage::Frame*>(m.panel->cardFrameForTest(slot)));
		REQUIRE(card != nullptr);
		REQUIRE(card->headEdge() == ui::CardHeadEdge::Top);
		REQUIRE(card->headMarks().menu); // the strip's menu is the host's: the road under test
		fx->setBounds(fx->x(), fx->y(), card->width(), fx->height());

		const auto mine = headButtonsRightToLeft(*card);
		const auto ref = headButtonsRightToLeft(*fx);
		REQUIRE(mine.size() >= 2); // a menu and an enable at least, or there is no row to compare
		REQUIRE(ref.size() >= mine.size());
		const float w = card->width();
		const visage::Bounds head = card->headLayout().head;
		if (std::getenv("FEATHERS_HEAD_TABLE") != nullptr) {
			std::string row = tag + " at " + std::to_string(w) + ":";
			for (const auto* side : { &mine, &ref }) {
				row += side == &mine ? "\n  strip" : "\n  fx   ";
				for (const ui::FxCard::HeadPart& p : *side) {
					row += std::string(" ") + p.what + " x[" + std::to_string(p.box.x()) + ","
					       + std::to_string(p.box.right()) + ") inset " + std::to_string(w - p.box.right());
				}
			}
			MESSAGE(row);
		}

		// THE RIGHTMOST BUTTON, AT THE FX CARD'S INSET.
		CHECK(w - mine.front().box.right() == doctest::Approx(w - ref.front().box.right()));
		for (size_t i = 0; i < mine.size(); ++i) {
			CAPTURE(mine[i].what);
			// THE SAME SIZE AND THE SAME ROW — the fx card's button in that position.
			CHECK(mine[i].box.width() == doctest::Approx(ref[i].box.width()));
			CHECK(mine[i].box.height() == doctest::Approx(ref[i].box.height()));
			CHECK(mine[i].box.y() == doctest::Approx(ref[i].box.y()));
			// INSIDE THE HEAD.
			CHECK(mine[i].box.x() >= head.x());
			CHECK(mine[i].box.right() <= head.right());
			CHECK(mine[i].box.y() >= head.y());
			CHECK(mine[i].box.bottom() <= head.bottom());
			if (i + 1 < mine.size()) {
				// PACKED AT THE FX CARD'S GAP — no empty slot between two buttons, and none overlapping.
				const float gap = mine[i].box.x() - mine[i + 1].box.right();
				const float refGap = ref[i].box.x() - ref[i + 1].box.right();
				CHECK(gap == doctest::Approx(refGap));
				CHECK(gap > 0.0f);
			}
		}
		++compared;
	}
	CHECK(compared >= 3);
}
TEST_CASE("IW: dragging a Free-only coordinate's box repaints its SIGIL — not only when something else repaints the panel") {
	// The glyphs did not update until the puck moved. The box commits the drag and repaints
	// itself; the sigil beside it read the value only when drawn, so it waited for the next whole-panel repaint. The
	// repaint is unobservable headlessly, so the hook COUNTS what it delivered, and the lap is read by value.
	namespace amd = feathers::fx::automod;
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	const auto& coords = f.panel->coordsForTest();
	REQUIRE_FALSE(coords.empty());
	ui::ZoneCoordCell& cell = *coords.front(); // Type
	REQUIRE(cell.sigil() != nullptr);
	ui::ValueBox& box = cell.box();
	const double before = box.control().value();
	const size_t repaintsBefore = cell.neighbourRepaintsForTest();
	const float x = box.width() * 0.5f;
	const float y = box.height() * 0.5f;
	box.onMouseDown().callback(at(x, y));
	for (int i = 1; i <= 40; ++i) {
		box.onMouseDrag().callback(at(x, y - 10.0f * static_cast<float>(i)));
	}
	box.onMouseUp().callback(at(x, y - 400.0f));
	const double after = box.control().value();
	CAPTURE(before);
	CAPTURE(after);
	REQUIRE(after != before); // the drag moved the value, or this proves nothing
	CHECK(cell.neighbourRepaintsForTest() > repaintsBefore);
	CHECK(cell.sigil()->lap() == static_cast<int64_t>(std::floor(after)));
	(void)amd::kZones;
}
TEST_CASE("IW: a zone cell's column is value, then its sigil or selector centred in the gap, then its caption at the bottom") {
	// The zone boxes with glyphs wasted vertical space: the "Zone A" caption sat centred, leaving a large gap under
	// the value and pushing the glyph off the bottom. The caption belongs at the bottom with the glyph centred between
	// it and the value, and the plain zone boxes had the same problem. One arithmetic serves both cells; asserted as values.
	for (const float face : { 30.0f, ui::Knob::kEngineRailDiameter, 52.0f }) {
		CAPTURE(face);
		const auto rows = ui::ZoneCoordCell::columnRowsFor(face);
		const float plateBottom = ui::Knob::columnHeight(face);
		const float valueBottom = rows.value + ui::ValueBox::kCompactHeight;
		const float middleBottom = rows.middle + ui::Knob::kReadoutHeight;
		// IN ORDER, AND NONE OVERLAPPING.
		CHECK(rows.value >= 0.0f);
		CHECK(valueBottom <= rows.middle);
		CHECK(middleBottom <= rows.caption);
		// THE CAPTION AT THE BOTTOM — its line ends at the plate's bottom inset, no dead gap under it.
		CHECK(rows.caption + ui::Knob::kLabelHeight == doctest::Approx(plateBottom - ui::ZoneCoordCell::kPlateInset));
		// THE MIDDLE ROW CENTRED IN THE GAP between the value and the caption.
		CHECK(rows.middle - valueBottom == doctest::Approx(rows.caption - middleBottom));
		// AND A SIGIL FITS IN IT WHOLE — "the glyph is pushed off the bottom" was the report.
		CHECK(ui::Knob::kReadoutHeight >= ui::Sigil::kSide);
	}
	// AND THE REAL CELLS PUT THEIR CHILDREN THERE — Automod's Type (a sigil) on its panel.
	AutomodPanelFixture f;
	REQUIRE(f.panel != nullptr);
	const ui::ZoneCoordCell& cell = *f.panel->coordsForTest().front();
	REQUIRE(cell.layout() == ui::ZoneCoordCell::Layout::Column);
	REQUIRE(cell.sigil() != nullptr);
	const auto rows = ui::ZoneCoordCell::columnRowsFor(cell.faceDiameter());
	CHECK(cell.box().y() == doctest::Approx(rows.value));
	CHECK(cell.sigil()->y() == doctest::Approx(rows.middle));
	CHECK(cell.sigil()->y() + cell.sigil()->height() <= rows.caption);
}

namespace {
void typeInto(ui::TextField& field, const std::string& text) {
	field.processTextInput(text);
	field.processKeyPress(visage::KeyEvent { visage::KeyCode::Return, 0, true });
}
} // namespace

TEST_CASE("MACROS card: four slots over the four controls") {
	ModFixture f;
	const MacrosSeat seat = seatMacros(f);
	for (size_t n = 0; n < mac::kMacroCount; ++n) {
		CAPTURE(n);
		ui::MacroSlot& slot = seat.card->slot(n);
		// THE SLOT'S CONTROLS ARE THE VIEW, AND THE VIEW IS OVER `Macro n` OF THIS DEVICE — asked of the
		// parameter road underneath, not of a caption.
		CHECK(slot.knob().control() == &seat.card->view(n));
		CHECK(&slot.chip().control() == &seat.card->view(n));
		const auto* road = dynamic_cast<const ui::DeviceControl*>(&seat.card->view(n).inner());
		REQUIRE(road != nullptr);
		CHECK(road->device() == seat.device);
		CHECK(road->control() == mac::macroParam(n));
		// THE SHORT CAPTION while the output carries no label of its own — and printed whole.
		CHECK(slot.caption() == "M" + std::to_string(n + 1));
		CHECK(slot.knob().drawnCaption() == "M" + std::to_string(n + 1));
		// THE KNOB'S CAPTION STAYS THE SHORT ONE, as the output chip under it prints the same word (the grid cell is now the knob's column,
		// 52px, where the declared name would just fit, so the choice is about reading as one pair, no longer about fitting).
		// ON SHOW AS A KNOB — the declared default type — and laid inside the card.
		CHECK(slot.knob().isVisible());
		CHECK(slot.width() > 0.0f);
		CHECK(slot.x() + slot.width() <= seat.card->width() + 0.5f);
	}
	// A VALUE WRITTEN THROUGH THE SLOT'S CONTROL LANDS ON THE DEVICE'S FIELD — distinct per macro, so a
	// slot wired to its neighbour's field fails here rather than agreeing by accident.
	for (size_t n = 0; n < mac::kMacroCount; ++n) {
		seat.card->slot(n).knob().control()->setValue(0.2 + 0.15 * static_cast<double>(n));
	}
	for (size_t n = 0; n < mac::kMacroCount; ++n) {
		CAPTURE(n);
		CHECK(f.deviceHost.fieldAt(seat.device, mac::macroParam(n))
		      == doctest::Approx(0.2 + 0.15 * static_cast<double>(n)));
	}
	// AND THE KNOBS ARE DESTINATIONS, which a header macro's are not.
	CHECK(seat.card->slot(1).knob().hasModulationForTest());
	CHECK(seat.card->view(1).display().isDestination);
}
TEST_CASE("a slot set to Momentary on the card writes the device's Type") {
	ModFixture f;
	const MacrosSeat seat = seatMacros(f);
	REQUIRE(f.gui.history() != nullptr);
	const size_t before = f.gui.history()->length();
	ui::MacroSlot& slot = seat.card->slot(2);
	ui::chooseMacroMenuEntry(slot.menuHooks(), ui::kMacroMenuMomentary);
	// THE DEVICE'S FIELD, as the number the declaration orders the types by.
	CHECK(f.deviceHost.fieldAt(seat.device, mac::typeParam(2)) == doctest::Approx(2.0));
	// ONLY THAT ONE.
	CHECK(f.deviceHost.fieldAt(seat.device, mac::typeParam(1)) == doctest::Approx(0.0));
	CHECK(f.deviceHost.fieldAt(seat.device, mac::typeParam(3)) == doctest::Approx(0.0));
	// THE SLOT RE-SEATED AT ONCE — a momentary pad, not the knob.
	CHECK(slot.type() == feathers::device::MacroType::Momentary);
	CHECK_FALSE(slot.knob().isVisible());
	CHECK(slot.chip().isVisible());
	CHECK(slot.chip().behaviour() == ui::SwitchChip::Behaviour::Momentary);
	// AS A GESTURE: one undo step, and undoing it puts the type back.
	CHECK(f.gui.history()->length() == before + 1);
	f.gui.history()->undo();
	CHECK(f.deviceHost.fieldAt(seat.device, mac::typeParam(2)) == doctest::Approx(0.0));
	// AND A TYPE MOVED UNDER THE CARD (the undo) IS CAUGHT BY THE CARD'S OWN TICK.
	(void)seat.card->update();
	CHECK(slot.type() == feathers::device::MacroType::Knob);
	CHECK(slot.knob().isVisible());
}
TEST_CASE("bipolar on the card flips the output") {
	ModFixture f;
	const MacrosSeat seat = seatMacros(f);
	ui::MacroSlot& slot = seat.card->slot(1);
	seat.card->view(1).setValue(0.8); // unipolar: the stored value IS the view
	REQUIRE(f.deviceHost.fieldAt(seat.device, mac::macroParam(1)) == doctest::Approx(0.8));
	ui::chooseMacroMenuEntry(slot.menuHooks(), ui::kMacroMenuBipolar);
	CHECK(f.deviceHost.fieldAt(seat.device, mac::bipolarParam(1)) == doctest::Approx(1.0));
	CHECK(f.deviceHost.fieldAt(seat.device, mac::bipolarParam(0)) == doctest::Approx(0.0));
	// THE STORED VALUE KEPT ITS SHAPE; THE VIEW READS 2v − 1 AND SO DOES THE DEVICE'S OUTPUT. The output
	// is asked of the device's own ops over the row the card wrote — `setFields` then `outputAt`, the
	// engine's two calls — and the expected number is restated here, not read back from either.
	CHECK(seat.card->view(1).bipolar());
	CHECK(seat.card->view(1).value() == doctest::Approx(2.0 * 0.8 - 1.0));
	CHECK(seat.card->view(1).desc().min == doctest::Approx(-1.0));
	std::array<float, static_cast<size_t>(mac::Param::kCount)> row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = f.deviceHost.fieldAt(seat.device, i);
	}
	mac::Macros device;
	device.setFields(row.data(), row.size());
	CHECK(device.outputAt(1) == doctest::Approx(0.6));
	CHECK(device.outputAt(1) != doctest::Approx(0.8)); // not vacuous: unipolar would publish the base
	// AND BACK: unipolar is the identity again.
	ui::chooseMacroMenuEntry(slot.menuHooks(), ui::kMacroMenuUnipolar);
	CHECK(f.deviceHost.fieldAt(seat.device, mac::bipolarParam(1)) == doctest::Approx(0.0));
	CHECK(seat.card->view(1).value() == doctest::Approx(0.8));
}
TEST_CASE("renaming a slot labels the output") {
	ModFixture f;
	const MacrosSeat seat = seatMacros(f);
	ui::MacroSlot& slot = seat.card->slot(1);
	ui::chooseMacroMenuEntry(slot.menuHooks(), ui::kMacroMenuRename);
	REQUIRE(slot.renaming());
	CHECK(slot.field().text() == "Macro 2"); // the declared name, offered for editing
	// SELECT-ALL IS THE FIELD'S OPENING STATE, so typing replaces the name.
	typeInto(slot.field(), "Wobble");
	CHECK_FALSE(slot.renaming());
	CHECK(f.bridge.outputLabel(seat.device, 1) == "Wobble");
	CHECK(f.bridge.outputRelabelled(seat.device, 1));
	CHECK_FALSE(f.bridge.outputRelabelled(seat.device, 0));
	CHECK(slot.caption() == "Wobble");
	CHECK(seat.card->slot(0).caption() == "M1");
	// THE ROUTE LISTS' NAME — the one authority every list and tray asks.
	const std::string head = "Macros " + std::to_string(f.bridge.rackSlotOf(seat.device) + 1);
	CHECK(ui::modSourceName(f.bridge, backplane::Source::output(seat.device, 1)) == head + " \xc2\xb7 Wobble");
	// A LABEL MOVED BY SOMEBODY ELSE (a load, another surface) RELABELS THE SLOT at once.
	f.bridge.setOutputLabel(seat.device, 3, "Twenty-four byte name!!!");
	CHECK(seat.card->slot(3).caption() == "Twenty-four byte name!!!");
	// AND CLEARED, the short caption returns.
	f.bridge.setOutputLabel(seat.device, 1, "");
	CHECK(slot.caption() == "M2");
}
TEST_CASE("the output arms read the label") {
	ModFixture f;
	const MacrosSeat seat = seatMacros(f);
	const ui::SourceArm* before = f.panel->outputArmForTest(seat.slot, 2);
	REQUIRE(before != nullptr);
	CHECK(before->label() == "Macro 3");
	f.bridge.setOutputLabel(seat.device, 2, "Filter sweep");
	// THE STRIP HEARD IT AND RE-LAID: the arm on output 3 carries the label, and its grid cell (the knob's column) is narrower than the word, so the
	// chip prints its M-number and the name is the hover; the route lists read the name (`outputName`).
	const ui::SourceArm* after = f.panel->outputArmForTest(seat.slot, 2);
	REQUIRE(after != nullptr);
	CHECK(after->label() == "Filter sweep");
	CHECK(f.panel->outputName(seat.slot, 2) == "Filter sweep");
	CHECK(after->width() < ui::SourceArm::widthFor("Filter sweep"));
	CHECK(after->shownText() == "M3");
	CHECK(after->tooltipForTest() == "Filter sweep");
	// ITS NEIGHBOURS KEEP THEIR DECLARED NAMES.
	REQUIRE(f.panel->outputArmForTest(seat.slot, 1) != nullptr);
	CHECK(f.panel->outputArmForTest(seat.slot, 1)->label() == "Macro 2");
}

namespace {
/// A ONE- OR EIGHT-MACRO DEVICE IN THE STRIP, and its card — `seatMacros` for another tag.
MacrosSeat seatVariant(ModFixture& f, const char* tag) {
	MacrosSeat seat;
	seat.slot = f.bridge.addRackDevice(tag);
	REQUIRE(seat.slot < feathers::kMaxRackSlots);
	seat.device = f.bridge.rackDeviceAt(seat.slot);
	REQUIRE(seat.device != feathers::kNoInstance);
	f.bridge.refresh();
	f.panel->resized();
	seat.card = const_cast<ui::MacrosCard*>(dynamic_cast<const ui::MacrosCard*>(f.panel->cardFrameForTest(seat.slot)));
	REQUIRE(seat.card != nullptr);
	return seat;
}
struct VariantCase {
	const char* tag;
	size_t count;
};
constexpr VariantCase kVariantCases[] = { { "MACRO1", 1 }, { "MACROS8", 8 } };
} // namespace

TEST_CASE("MACRO1 and MACROS8 cards: every slot is over its own macro, and does what the four-macro card's does") {
	for (const VariantCase& vc : kVariantCases) {
		CAPTURE(vc.tag);
		const size_t count = vc.count;
		ModFixture f;
		const MacrosSeat seat = seatVariant(f, vc.tag);
		REQUIRE(seat.card->slotCount() == count);
		// THE CARD OFFERS EVERY CONTROL OF THE DEVICE BUT THE ENABLE: three per macro, each once.
		const std::vector<mac::Param> addressed = seat.card->addressedParamsForTest();
		REQUIRE(addressed.size() == 3 * count);
		for (size_t c = 0; c < 3 * count; ++c) {
			CHECK(std::find(addressed.begin(), addressed.end(), static_cast<mac::Param>(c)) != addressed.end());
		}
		for (size_t n = 0; n < count; ++n) {
			CAPTURE(n);
			ui::MacroSlot& slot = seat.card->slot(n);
			const auto* road = dynamic_cast<const ui::DeviceControl*>(&seat.card->view(n).inner());
			REQUIRE(road != nullptr);
			CHECK(road->device() == seat.device);
			CHECK(road->control() == mac::macroField(n));
			CHECK(slot.knob().isVisible());
			CHECK(slot.x() + slot.width() <= seat.card->width() + 0.5f);
			// A VALUE WRITTEN THROUGH THE SLOT LANDS ON THIS MACRO'S FIELD ONLY.
			slot.knob().control()->setValue(0.1 + 0.1 * static_cast<double>(n));
			for (size_t m = 0; m <= n; ++m) {
				CHECK(f.deviceHost.fieldAt(seat.device, mac::macroField(m)) == doctest::Approx(0.1 + 0.1 * static_cast<double>(m)));
			}
			for (size_t m = n + 1; m < count; ++m) {
				CHECK(f.deviceHost.fieldAt(seat.device, mac::macroField(m)) == doctest::Approx(0.0));
			}
			// EACH KNOB IS A DESTINATION, and each output an arm that routes out.
			CHECK(slot.knob().hasModulationForTest());
			const ui::SourceArm* arm = f.panel->outputArmForTest(seat.slot, n);
			REQUIRE(arm != nullptr);
			CHECK(arm->label() == "Macro " + std::to_string(n + 1));
		}
		// THE LAST SLOT'S TYPE AND POLARITY, from its menu: its own fields, and no other macro's.
		const size_t last = count - 1;
		ui::MacroSlot& slot = seat.card->slot(last);
		ui::chooseMacroMenuEntry(slot.menuHooks(), ui::kMacroMenuMomentary);
		CHECK(f.deviceHost.fieldAt(seat.device, mac::typeField(count, last)) == doctest::Approx(2.0));
		CHECK(slot.type() == feathers::device::MacroType::Momentary);
		ui::chooseMacroMenuEntry(slot.menuHooks(), ui::kMacroMenuBipolar);
		CHECK(f.deviceHost.fieldAt(seat.device, mac::bipolarField(count, last)) == doctest::Approx(1.0));
		CHECK(seat.card->view(last).bipolar());
		for (size_t m = 0; m < last; ++m) {
			CHECK(f.deviceHost.fieldAt(seat.device, mac::typeField(count, m)) == doctest::Approx(0.0));
			CHECK(f.deviceHost.fieldAt(seat.device, mac::bipolarField(count, m)) == doctest::Approx(0.0));
		}
		// RENAME the first: the label is that output's, and the card's caption follows.
		ui::MacroSlot& first = seat.card->slot(0);
		ui::chooseMacroMenuEntry(first.menuHooks(), ui::kMacroMenuRename);
		REQUIRE(first.renaming());
		typeInto(first.field(), "Wobble");
		CHECK(f.bridge.outputLabel(seat.device, 0) == "Wobble");
		CHECK(first.caption() == "Wobble");
		if (count > 1) {
			CHECK_FALSE(f.bridge.outputRelabelled(seat.device, 1));
		}
	}
}
TEST_CASE("MACRO1 and MACROS8 cards: the slots lie where the card says — one large knob, or two rows of four") {
	ModFixture f;
	const MacrosSeat one = seatVariant(f, "MACRO1");
	const MacrosSeat eight = seatVariant(f, "MACROS8");
	const MacrosSeat four = seatMacros(f);
	// ONE: a single slot, with a full column to itself and a face larger than the four's.
	CHECK(one.card->slotCount() == 1);
	CHECK(one.card->slot(0).width() > four.card->slot(0).width() * 2.0f);
	CHECK(one.card->slot(0).knob().width() > four.card->slot(0).knob().width());
	CHECK(one.card->slot(0).caption() == "Macro 1");
	// EIGHT: the first four share a row, the next four lie in the row below, column for column.
	REQUIRE(eight.card->slotCount() == 8);
	for (size_t n = 0; n < 4; ++n) {
		CAPTURE(n);
		CHECK(eight.card->slot(n).y() == doctest::Approx(eight.card->slot(0).y()));
		CHECK(eight.card->slot(n + 4).y() > eight.card->slot(n).y() + eight.card->slot(n).height() - 0.5f);
		CHECK(eight.card->slot(n + 4).x() == doctest::Approx(eight.card->slot(n).x()));
		// the same column width as the four-macro card: a standard modulator card grid
		CHECK(eight.card->slot(n).width() == doctest::Approx(four.card->slot(n).width()));
		CHECK(eight.card->slot(n + 4).y() + eight.card->slot(n + 4).height() <= eight.card->height() + 0.5f);
	}
	// THE FOUR IS UNCHANGED: one row.
	for (size_t n = 1; n < 4; ++n) {
		CHECK(four.card->slot(n).y() == doctest::Approx(four.card->slot(0).y()));
	}
	CHECK(ui::MacrosCard::openHeight(1) == doctest::Approx(ui::kCardHeadHeight + ui::MacrosCard::bodyHeight() + ui::kCardOutputStripHeight));
	CHECK(ui::MacrosCard::openHeight(2, ui::kMacros8Shape) > ui::MacrosCard::openHeight(2));
}
TEST_CASE("MACROS and MACROS8: each output chip stands in its knob's column (the same grid, 1x4 and 2x4), prints M1..Mn, and a renamed macro keeps the M-number with its name on hover") {
	struct Shape {
		const char* tag;
		size_t count;
		size_t rows;
	};
	for (const Shape& shape : { Shape { "MACROS", 4, 1 }, Shape { "MACROS8", 8, 2 } }) for (const float factor : { 1.0f, 1.75f }) {
		CAPTURE(shape.tag);
		CAPTURE(factor);
		ModFixture f;
		const float scale = f.root->dpiScale() * factor;
		f.root->setDpiScale(scale);
		const MacrosSeat seat = seatVariant(f, shape.tag);
		REQUIRE(seat.card->slotCount() == shape.count);
		const auto centreX = [](const visage::Frame& frame) { return frame.x() + 0.5f * frame.width(); };
		float firstRowY = 0.0f;
		for (size_t n = 0; n < shape.count; ++n) {
			CAPTURE(n);
			const ui::SourceArm* arm = f.panel->outputArmForTest(seat.slot, n);
			REQUIRE(arm != nullptr);
			REQUIRE(arm->width() > 0.0f);
			const ui::MacroSlot& slot = seat.card->slot(n);
			// THE CHIP'S CENTRE IS THE KNOB COLUMN'S, within a pixel (both in the panel's coordinates).
			const float knobColumn = seat.card->x() + centreX(slot);
			CHECK(std::abs(centreX(*arm) - knobColumn) <= 1.0f);
			// THE ROWS MIRROR THE KNOBS: four to a row, in order.
			if (n == 0) {
				firstRowY = arm->y();
			}
			if (n < 4) {
				CHECK(arm->y() == doctest::Approx(firstRowY));
			}
			else {
				CHECK(arm->y() > firstRowY + 10.0f);
				CHECK(arm->x() == doctest::Approx(f.panel->outputArmForTest(seat.slot, n - 4)->x()));
			}
			// THE WORD ON THE CHIP is the short M-number whole, in a cell narrower than the name.
			CHECK(arm->shownText() == "M" + std::to_string(n + 1));
			// EVERY CHIP LIES FULLY INSIDE THE CARD with the card's own gutter on the right as on the left, in native pixels at this scale.
			CHECK(arm->nativeRight() <= seat.card->nativeRight() - static_cast<int>(std::lround(ui::kCardOutputChipPadX * scale)) + 1);
			CHECK(arm->nativeX() >= seat.card->nativeX() + static_cast<int>(std::lround(ui::kCardOutputChipPadX * scale)) - 1);
			// AND THE WORD LIES INSIDE ITS OWN CHIP: the arrow and "M4" end before the chip's right border (they ran over it when the cell kept the
			// full chip's inset), and begin after its left.
			const ui::SourceArm::ContentLayout content = arm->contentLayout();
			CHECK(content.right <= static_cast<float>(arm->width()) - 2.0f);
			CHECK(content.inset >= 2.0f);
		}
		// A RENAMED MACRO: the chip still says its number; the name is the hover and the route lists' word.
		f.bridge.setOutputLabel(seat.device, 1, "Wobble");
		f.panel->resized();
		const ui::SourceArm* renamed = f.panel->outputArmForTest(seat.slot, 1);
		REQUIRE(renamed != nullptr);
		CHECK(renamed->shownText() == "M2");
		CHECK(renamed->tooltipForTest().find("Wobble") != std::string::npos);
		CHECK(renamed->label().find("Wobble") != std::string::npos);
	}
}
TEST_CASE("a relabelled MACROS output in a chain column renames its arm and re-lays the column") {
	// **THE COLUMN KEEPS ITS ARMS ACROSS A RELABEL** (they are rebuilt only when the stack changes), so
	// the word has to be HANDED to the arm and the column laid out again for the chip to take the new
	// word's width. `SourceArm::setLabel` only repaints: a chip whose rectangle is the new word's width is
	// one a layout pass placed, which is what makes the width the evidence for the relayout.
	HeldChain held;
	ui::WidgetRegistry registry;
	ui::ModState mod;
	ui::EditHistory history;
	ui::ChainColumn column(held.bridge(), registry, mod, history, nullptr);
	// TALL ENOUGH THAT THE CARD IS NOT SCROLLED OUT — an arm follows its card's visibility.
	column.setBounds(0, 0, static_cast<int>(ui::FxLaneRail::kWidth), 4000);
	const DeviceInstanceId macros = column.addDevice(mac::kTraits.tag);
	REQUIRE(macros != feathers::kNoInstance);
	ui::SourceArm* arm = column.outputArmOf(macros, 2);
	REQUIRE(arm != nullptr);
	CHECK(arm->label() == "Macro 3");
	// A GRID CELL, NOT THE NAME'S WIDTH: the chip prints its M-number and keeps the name for the hover.
	CHECK(column.outputArmBoundsForTest(macros, 2).width() < ui::SourceArm::widthFor("Macro 3"));
	CHECK(arm->shownText() == "M3");

	held.bridge().setOutputLabel(macros, 2, "Filter sweep");
	// THE SAME ARM, TOLD THE WORD — the cell does not move with the name, the chip still says M3, and the name is its hover.
	CHECK(column.outputArmOf(macros, 2) == arm);
	CHECK(arm->label() == "Filter sweep");
	CHECK(arm->shownText() == "M3");
	CHECK(arm->tooltipForTest() == "Filter sweep");
	CHECK(column.outputArmBoundsForTest(macros, 2).width() < ui::SourceArm::widthFor("Filter sweep"));
	// ITS NEIGHBOURS KEEP THEIR DECLARED NAMES.
	REQUIRE(column.outputArmOf(macros, 1) != nullptr);
	CHECK(column.outputArmOf(macros, 1)->label() == "Macro 2");
}
TEST_CASE("a retired MACROS card stops hearing relabels") {
	// **A REORDER REBUILDS THE CARD OF A DEVICE THAT STAYS** — the card is keyed by position AND device,
	// so moving MACROS above an LFO retires the card it had and builds another at the new position. The
	// retired one waits for `releaseRetiredCards`; its device is alive, so without `FxCard::retired` it
	// would go on refreshing its slots on that device's relabels, unseen.
	ModFixture f;
	const size_t lfoSlot = f.bridge.addRackDevice("LFO");
	REQUIRE(lfoSlot < feathers::kMaxRackSlots);
	const MacrosSeat seat = seatMacros(f);
	REQUIRE(seat.slot == lfoSlot + 1);
	REQUIRE(f.bridge.moveRackDevice(seat.slot, lfoSlot));
	f.bridge.refresh();
	f.panel->resized();
	REQUIRE(f.bridge.rackDeviceAt(lfoSlot) == seat.device);
	// RETIRED, AND NOT YET FREED — the state the question is about.
	const auto* rebuilt = dynamic_cast<const ui::MacrosCard*>(f.panel->cardFrameForTest(lfoSlot));
	REQUIRE(rebuilt != nullptr);
	REQUIRE(rebuilt != seat.card);
	REQUIRE(f.panel->retiredCountForTest() > 0);
	REQUIRE(seat.card->slot(1).caption() == "M2");

	f.bridge.setOutputLabel(seat.device, 1, "Wobble");
	// THE LIVE CARD HEARD IT; the retired one did not.
	CHECK(rebuilt->slot(1).caption() == "Wobble");
	CHECK(seat.card->slot(1).caption() == "M2");
	f.panel->releaseRetiredCards();
}
TEST_CASE("a popup left open across the card's destruction writes nothing") {
	// visage parents a slot's menu to the ROOT frame and the menu keeps a COPY of the hooks until a pick;
	// the card can be rebuilt under it (a nested chain's editor is rebuilt on a tick). A pick after that
	// must reach no freed card. Under plain ctest a freed read may not fault; the ASan build of this
	// binary is where this case earns its keep.
	ModFixture f;
	const MacrosSeat seat = seatMacros(f);
	const ui::MacroMenuHooks held = seat.card->slot(1).menuHooks();
	const ui::MacroSlot::Naming naming = seat.card->slot(1).naming();
	REQUIRE(held.complete());
	// THE CARD GOES — the device leaves the strip and the strip frees what it retired.
	REQUIRE(f.bridge.removeRackDevice(seat.slot));
	f.bridge.refresh();
	f.panel->resized();
	// RETIRED — no longer the card at its slot — and then FREED, which is the state the menu outlives.
	REQUIRE(f.panel->cardFrameForTest(seat.slot) != seat.card);
	REQUIRE(f.panel->retiredCountForTest() > 0);
	f.panel->releaseRetiredCards();
	REQUIRE(f.panel->retiredCountForTest() == 0);

	ui::chooseMacroMenuEntry(held, ui::kMacroMenuMomentary);
	ui::chooseMacroMenuEntry(held, ui::kMacroMenuBipolar);
	naming.rename("Too late");
	// THE READERS ANSWER THE DEFAULTS rather than reading through the dead card.
	CHECK(held.readType() == feathers::device::MacroType::Knob);
	CHECK_FALSE(held.readBipolar());
	CHECK(naming.name() == "Macro 2");
	CHECK_FALSE(naming.renamed());
}

namespace {
Rect rectOf(const visage::Frame& fr) {
	return { fr.x(), fr.y(), fr.width(), fr.height() };
}
} // namespace

TEST_CASE("MATH card: Operator is a dropdown, Transfer a glyph with a menu, Bipolar is a chip") {
	// ASKED OF THE CARD THE STRIP BUILT, so the claim is about what a hand gets. A multivalue toggle is the
	// project's antipattern: a nine-way Operator and a four-way Transfer must show their entries before
	// one is applied (a menu), and the two-way Bipolar is a switch (a chip).
	ModFixture f;
	const MathSeat seat = seatMath(f);
	ui::MathCard& card = *seat.card;

	CHECK(addressOn(seat.device, &card.op().control()) == static_cast<size_t>(mth::Param::Operator));
	CHECK(addressOn(seat.device, &card.transfer().mapControl()) == static_cast<size_t>(mth::Param::Map));
	CHECK(card.op().choiceMenuAvailable());
	CHECK(card.op().opensMenu());
	REQUIRE(card.op().entryCount() == 9u);
	const visage::PopupMenu transfer = card.transferMenu();
	REQUIRE(transfer.options().size() == 4u);
	for (size_t i = 0; i < 4; ++i) {
		CAPTURE(i);
		CHECK(transfer.options()[i].name().toUtf8().find(mth::kMapNames[i]) != std::string::npos);
	}
	// THE CAPTION IS THE WORD, and so is the descriptor's name for it — what every other surface prints.
	CHECK(std::string(mth::kParamDescs[static_cast<size_t>(mth::Param::Map)].name) == "Transfer");
	CHECK(std::string(mth::kParamDescs[static_cast<size_t>(mth::Param::Map)].name) == "Transfer");

	// THE SWITCH IS BIPOLAR, AND THE THREE CONTINUOUS CONTROLS ARE KNOBS.
	CHECK(addressOn(seat.device, &card.bipolar().control()) == static_cast<size_t>(mth::Param::Bipolar));
	CHECK(addressOn(seat.device, card.a().control()) == static_cast<size_t>(mth::Param::A));
	CHECK(addressOn(seat.device, card.b().control()) == static_cast<size_t>(mth::Param::B));
	CHECK(addressOn(seat.device, card.amount().control()) == static_cast<size_t>(mth::Param::Amount));
}
TEST_CASE("MATH card: A, the operator and B share the first row, operator between") {
	ModFixture f;
	const MathSeat seat = seatMath(f);
	const Rect a = rectOf(seat.card->a());
	const Rect op = rectOf(seat.card->op());
	const Rect b = rectOf(seat.card->b());
	CAPTURE(a.x);
	CAPTURE(op.x);
	CAPTURE(b.x);
	// ONE ROW: A and B at the same top and height, the operator's band inside theirs, on the knob FACES'
	// centre line (the face is the top `kFaceDiameter` of a knob column).
	CHECK(a.y == b.y);
	CHECK(a.h == b.h);
	CHECK(op.y >= a.y);
	CHECK(op.bottom() <= a.bottom());
	CHECK(op.midY() == doctest::Approx(a.y + ui::MathCard::kFaceDiameter * 0.5f));
	// THE ORDER IS THE EXPRESSION: A, then the operator, then B — each clear of the next.
	CHECK(a.right() <= op.x);
	CHECK(op.right() <= b.x);
	// AND NARROW: the operator is the sign between two dials, not a third column.
	CHECK(op.w < a.w);
	CHECK(op.w < b.w);
	// INSIDE THE CARD.
	CHECK(a.x >= 0.0f);
	CHECK(b.right() <= seat.card->width());
}
TEST_CASE("MATH card: Amount, Bipolar and Transfer share the second row in thirds") {
	ModFixture f;
	const MathSeat seat = seatMath(f);
	const Rect a = rectOf(seat.card->a());
	const Rect amt = rectOf(seat.card->amount());
	const Rect bip = rectOf(seat.card->bipolar());
	const Rect tr = rectOf(seat.card->transfer());
	// BELOW THE FIRST ROW.
	CHECK(amt.y >= a.bottom());
	CHECK(bip.y >= a.bottom());
	CHECK(tr.y >= a.bottom());
	// ONE ROW, LEVEL WITH AMOUNT'S FACE: the chip on the face's centre line, the dropdown's top on the
	// face's top, and both inside Amount's column.
	const float face = ui::MathCard::kAmountDiameter;
	CHECK(bip.midY() == doctest::Approx(amt.y + face * 0.5f));
	CHECK(tr.y == doctest::Approx(amt.y));
	CHECK(bip.bottom() <= amt.bottom());
	CHECK(tr.bottom() <= amt.bottom());
	// THIRDS: three equal widths, in order Amount | Bipolar | Transfer, with the body's padding and the
	// gaps accounted for — so the three together span the body less exactly those.
	CHECK(amt.w == doctest::Approx(bip.w));
	CHECK(bip.w == doctest::Approx(tr.w));
	CHECK(amt.right() <= bip.x);
	CHECK(bip.right() <= tr.x);
	const float inner = seat.card->bodyWidth() - 2.0f * ui::MathCard::kPadX;
	CHECK(amt.w == doctest::Approx((inner - 2.0f * ui::MathCard::kGap) / 3.0f));
	CHECK(amt.x == doctest::Approx(seat.card->bodyLeft() + ui::MathCard::kPadX));
	CHECK(tr.right() == doctest::Approx(seat.card->bodyLeft() + ui::MathCard::kPadX + inner));
	// AMOUNT IS A SMALLER KNOB COLUMN, AND ITS CAPTION IS WHOLE in a third — the reason it is not a list
	// row (`math_card.h`: beside a face, "AMOUNT" drew as "AMOUN").
	CHECK(seat.card->amount().faceDiameter() < seat.card->a().faceDiameter());
	CHECK(seat.card->amount().labelPlacement() == ui::Knob::LabelPlacement::Below);
	CHECK(seat.card->amount().drawnCaption() == "AMOUNT");
}

namespace {
/// A PRESS, MOVE OR RELEASE ON THE GLYPH, in its own coordinates — the road a hand takes.
void pressGlyph(ui::MathTransferGlyph& g, float x, float y) { g.onMouseDown().callback(at(x, y)); }
void dragGlyph(ui::MathTransferGlyph& g, float x, float y) { g.onMouseDrag().callback(at(x, y)); }
void releaseGlyph(ui::MathTransferGlyph& g, float x, float y) {
	visage::MouseEvent e = at(x, y);
	e.is_down = false;
	e.button_state = visage::kMouseButtonNone;
	g.onMouseUp().callback(e);
}
void setMath(ui::MathCard& card, mth::Param p, double v) {
	card.control(static_cast<size_t>(p)).setValue(v);
	(void)card.updateContent();
}
} // namespace

TEST_CASE("PY: MATH card — the Transfer glyph draws the curve the device evaluates, at every map, coefficient and polarity") {
	// THE POINTS, RESTATED THROUGH THE DEVICE ITSELF: `mth::evaluate` with B = 1 under × and Amount 1 is the map of A
	// alone, so each drawn y must be the box's centre less `evaluate` at that x times the half-height — over −1..1
	// with Bipolar on and 0..1 with it off. `evaluate` is what the engine runs per sample, so a glyph plotting a
	// different curve (the route's stepped one, a default exponent, Lin) goes red here.
	ModFixture f;
	const MathSeat seat = seatMath(f);
	ui::MathCard& card = *seat.card;
	ui::MathTransferGlyph& glyph = card.transfer();
	CHECK(addressOn(seat.device, &glyph.mapControl()) == static_cast<size_t>(mth::Param::Map));
	const visage::Bounds box = glyph.plotBounds();
	REQUIRE(box.width() > 20.0f);
	REQUIRE(box.height() > 10.0f);
	const float half = box.height() * 0.5f - 1.0f;
	const float cy = box.y() + box.height() * 0.5f;
	struct Shape {
		mth::Map map;
		double octaves;
		double exponent;
	};
	// OFF-STEP COEFFICIENTS ON PURPOSE (2.7, 3.3, 5.55): MATH's controls are continuous, a route's are steps.
	const Shape shapes[] = {
		{ mth::Map::Lin, 4.0, 2.0 },  { mth::Map::Log, 4.0, 2.0 }, { mth::Map::Log, 4.0, 5.55 },
		{ mth::Map::Exp, 4.0, 2.7 },  { mth::Map::Exp, 4.0, 1.0 }, { mth::Map::Phi, 0.0, 2.0 },
		{ mth::Map::Phi, 3.3, 2.0 },  { mth::Map::Phi, 7.0, 2.0 },
	};
	size_t compared = 0;
	for (const bool bipolar : { true, false }) {
		for (const Shape& s : shapes) {
			CAPTURE(bipolar);
			CAPTURE(static_cast<int>(s.map));
			CAPTURE(s.octaves);
			CAPTURE(s.exponent);
			setMath(card, mth::Param::Bipolar, bipolar ? 1.0 : 0.0);
			setMath(card, mth::Param::Map, static_cast<double>(s.map));
			setMath(card, mth::Param::Periods, s.octaves);
			setMath(card, mth::Param::Exponent, s.exponent);
			const std::vector<visage::Point> points = glyph.plotPoints();
			REQUIRE(points.size() >= 13u);
			CHECK(points.front().x == doctest::Approx(box.x()));
			CHECK(points.back().x == doctest::Approx(box.right()));
			const float lo = bipolar ? -1.0f : 0.0f;
			for (const visage::Point& p : points) {
				const float x = lo + (1.0f - lo) * (p.x - box.x()) / box.width();
				const float engine = mth::evaluate(x, 1.0f, mth::Op::Multiply, s.map, 1.0f, bipolar,
				                                   static_cast<float>(s.octaves), static_cast<float>(s.exponent));
				CHECK(std::abs(p.y - (cy - engine * half)) < 1e-3f);
				++compared;
			}
		}
	}
	CHECK(compared > 200u);
	// NOT VACUOUS: a bent curve's middle sits off Lin's, and a φ curve is drawn two points a pixel.
	setMath(card, mth::Param::Bipolar, 0.0);
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Lin));
	const std::vector<visage::Point> lin = glyph.plotPoints();
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Exp));
	const std::vector<visage::Point> exp = glyph.plotPoints();
	REQUIRE(lin.size() == exp.size());
	CHECK(std::abs(lin[6].y - exp[6].y) > 1.0f);
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Phi));
	CHECK(glyph.plotPoints().size() == static_cast<size_t>(box.width() * 2.0f) + 1u);
}
TEST_CASE("PY: MATH card — a vertical drag on the Transfer glyph sets the coefficient, with a readout, as one undo step") {
	ModFixture f;
	const MathSeat seat = seatMath(f);
	ui::MathCard& card = *seat.card;
	ui::MathTransferGlyph& glyph = card.transfer();
	ui::EditHistory& history = *f.gui.history();
	const float cx = glyph.width() * 0.5f;
	const float cy = ui::MathCard::kAmountDiameter * 0.5f;
	const auto value = [&](mth::Param p) { return card.control(static_cast<size_t>(p)).value(); };

	// EXP AT 2: 32 px up is 16 steps at two pixels a step — one octave, exponent 4.
	for (const mth::Map bend : { mth::Map::Exp, mth::Map::Log }) {
		CAPTURE(static_cast<int>(bend));
		setMath(card, mth::Param::Map, static_cast<double>(bend));
		setMath(card, mth::Param::Exponent, 2.0);
		CHECK(glyph.readout() == std::string(mth::kMapNames[static_cast<size_t>(bend)]) + " 2.00");
		const size_t steps = history.length();
		pressGlyph(glyph, cx, cy);
		dragGlyph(glyph, cx, cy - 16.0f);
		CHECK(glyph.dragging());
		dragGlyph(glyph, cx, cy - 32.0f);
		CHECK(value(mth::Param::Exponent) == doctest::Approx(4.0));
		CHECK(glyph.readout() == std::string(mth::kMapNames[static_cast<size_t>(bend)]) + " 4.00");
		releaseGlyph(glyph, cx, cy - 32.0f);
		CHECK_FALSE(glyph.dragging());
		CHECK_FALSE(ui::menuOpenFor(glyph)); // a drag is not a click
		CHECK(value(mth::Param::Exponent) == doctest::Approx(4.0));
		CHECK(value(mth::Param::Map) == static_cast<double>(bend)); // the drag moves the coefficient, never the map
		// ONE STEP for the whole drag, and undo puts the exponent back.
		CHECK(history.length() == steps + 1);
		history.undo();
		CHECK(value(mth::Param::Exponent) == doctest::Approx(2.0));
		// DOWN PAST THE BOTTOM: exponent 1, clamped.
		pressGlyph(glyph, cx, cy);
		dragGlyph(glyph, cx, cy + 600.0f);
		releaseGlyph(glyph, cx, cy + 600.0f);
		CHECK(value(mth::Param::Exponent) == doctest::Approx(1.0));
	}

	// PHI AT 16 PERIODS (octaves 4): 32 px up is 32 steps at a pixel a step — one octave, 32 periods.
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Phi));
	setMath(card, mth::Param::Periods, 4.0);
	CHECK(glyph.readout() == "Phi 16");
	const size_t steps = history.length();
	pressGlyph(glyph, cx, cy);
	dragGlyph(glyph, cx, cy - 32.0f);
	CHECK(value(mth::Param::Periods) == doctest::Approx(5.0));
	CHECK(glyph.readout() == "Phi 32");
	releaseGlyph(glyph, cx, cy - 32.0f);
	CHECK(history.length() == steps + 1);
	// A TREMBLE IS NOT A DRAG: under the threshold nothing moves, and the release is a click (the menu).
	pressGlyph(glyph, cx, cy);
	dragGlyph(glyph, cx, cy - 2.0f);
	CHECK_FALSE(glyph.dragging());
	CHECK(value(mth::Param::Periods) == doctest::Approx(5.0));
	releaseGlyph(glyph, cx, cy - 2.0f);
	CHECK(ui::menuOpenFor(glyph));
	ui::dismissMenuOf(glyph);
	CHECK(history.length() == steps + 1);
}
TEST_CASE("PY: MATH card — a click on the Transfer glyph opens the Transfer menu, and its pick is one step") {
	ModFixture f;
	const MathSeat seat = seatMath(f);
	ui::MathCard& card = *seat.card;
	ui::MathTransferGlyph& glyph = card.transfer();
	ui::EditHistory& history = *f.gui.history();
	const float cx = glyph.width() * 0.5f;
	const float cy = ui::MathCard::kAmountDiameter * 0.5f;
	// A CURVE WITH A COEFFICIENT: the release of a press that did not move opens it.
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Exp));
	pressGlyph(glyph, cx, cy);
	CHECK_FALSE(ui::menuOpenFor(glyph));
	releaseGlyph(glyph, cx, cy);
	CHECK(ui::menuOpenFor(glyph));
	ui::dismissMenuOf(glyph);
	// LIN: the press itself, as the route chip's.
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Lin));
	pressGlyph(glyph, cx, cy);
	CHECK(ui::menuOpenFor(glyph));
	ui::dismissMenuOf(glyph);
	releaseGlyph(glyph, cx, cy);

	// THE MENU: every map, by name, the current one marked — and a pick writes the map as one undo step.
	const visage::PopupMenu menu = card.transferMenu();
	REQUIRE(menu.options().size() == static_cast<size_t>(mth::Map::kCount));
	for (size_t i = 0; i < menu.options().size(); ++i) {
		CAPTURE(i);
		CHECK(menu.options()[i].id() == static_cast<int>(i));
		CHECK(menu.options()[i].name().toUtf8() == ui::markedChoiceLabel(mth::kMapNames[i], i == 0));
	}
	const size_t steps = history.length();
	menu.onSelection().callback(static_cast<int>(mth::Map::Phi));
	CHECK(card.control(static_cast<size_t>(mth::Param::Map)).value() == static_cast<double>(mth::Map::Phi));
	CHECK(history.length() == steps + 1);
	CHECK(glyph.readout() == "Phi 16");
	history.undo();
	CHECK(card.control(static_cast<size_t>(mth::Param::Map)).value() == static_cast<double>(mth::Map::Lin));
}
TEST_CASE("PY: MATH card — a curve without a coefficient ignores a drag on the Transfer glyph") {
	ModFixture f;
	const MathSeat seat = seatMath(f);
	ui::MathCard& card = *seat.card;
	ui::MathTransferGlyph& glyph = card.transfer();
	ui::EditHistory& history = *f.gui.history();
	const float cx = glyph.width() * 0.5f;
	const float cy = ui::MathCard::kAmountDiameter * 0.5f;
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Lin));
	setMath(card, mth::Param::Periods, 3.0);
	setMath(card, mth::Param::Exponent, 3.0);
	CHECK(glyph.coefficientControl() == nullptr);
	CHECK(glyph.readout() == "Lin");
	const size_t steps = history.length();
	for (const float dy : { -40.0f, 40.0f, -300.0f }) {
		CAPTURE(dy);
		pressGlyph(glyph, cx, cy);
		ui::dismissMenuOf(glyph);
		dragGlyph(glyph, cx, cy + dy);
		CHECK_FALSE(glyph.dragging());
		releaseGlyph(glyph, cx, cy + dy);
		CHECK(card.control(static_cast<size_t>(mth::Param::Map)).value() == 0.0);
		CHECK(card.control(static_cast<size_t>(mth::Param::Periods)).value() == 3.0);
		CHECK(card.control(static_cast<size_t>(mth::Param::Exponent)).value() == 3.0);
		CHECK(glyph.readout() == "Lin");
	}
	CHECK(history.length() == steps);
	// AND THE CARD OFFERS MAP ALONE THERE, the coefficient only under the map that reads it.
	const auto addressed = card.addressedParamsForTest();
	CHECK(std::find(addressed.begin(), addressed.end(), mth::Param::Map) != addressed.end());
	CHECK(std::find(addressed.begin(), addressed.end(), mth::Param::Periods) == addressed.end());
	CHECK(std::find(addressed.begin(), addressed.end(), mth::Param::Exponent) == addressed.end());
	setMath(card, mth::Param::Map, static_cast<double>(mth::Map::Phi));
	const auto phi = card.addressedParamsForTest();
	CHECK(std::find(phi.begin(), phi.end(), mth::Param::Periods) != phi.end());
}
TEST_CASE("PY: MATH card — the second row stays in thirds under every map, and a comparison greys Bipolar") {
	ModFixture f;
	const MathSeat seat = seatMath(f);
	ui::MathCard& card = *seat.card;
	const float inner = card.bodyWidth() - 2.0f * ui::MathCard::kPadX;
	for (const mth::Map m : { mth::Map::Lin, mth::Map::Log, mth::Map::Exp, mth::Map::Phi }) {
		CAPTURE(static_cast<int>(m));
		setMath(card, mth::Param::Map, static_cast<double>(m));
		const Rect tr = rectOf(card.transfer());
		CHECK(tr.w == doctest::Approx((inner - 2.0f * ui::MathCard::kGap) / 3.0f));
		CHECK(tr.right() == doctest::Approx(card.bodyLeft() + ui::MathCard::kPadX + inner));
		CHECK(tr.y == doctest::Approx(rectOf(card.amount()).y));
		CHECK(tr.h == doctest::Approx(rectOf(card.amount()).h));
	}
	// A COMPARISON: a 0/1 gate reads the same under either polarity, so Bipolar greys; back to × and it is live.
	for (const mth::Op op : { mth::Op::Greater, mth::Op::Less, mth::Op::GreaterEq, mth::Op::LessEq }) {
		CAPTURE(static_cast<int>(op));
		setMath(card, mth::Param::Operator, static_cast<double>(op));
		CHECK_FALSE(card.bipolar().enabled());
	}
	setMath(card, mth::Param::Operator, static_cast<double>(mth::Op::Multiply));
	CHECK(card.bipolar().enabled());
}
TEST_CASE("MATH card: the operator menu lists × + − min max") {
	// THE MENU THE PRESS OPENS, read row by row: `ValueBox`'s mark cell ("X  " on the current entry, five
	// spaces on the rest) and then the entry, EXACTLY — so "A × B" or "Min" coming back is red.
	ModFixture f;
	const MathSeat seat = seatMath(f);
	REQUIRE(seat.card->op().control().value() == 0.0); // × is the default
	const visage::PopupMenu menu = seat.card->guardedMenuFor(seat.card->op());
	// AND THE FOUR COMPARISONS AFTER THEM (backlog PY).
	const std::vector<std::string> expected { "×", "+", "−", "min", "max", ">", "<", ">=", "<=" };
	REQUIRE(menu.options().size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i) {
		CAPTURE(i);
		CHECK(menu.options()[i].id() == static_cast<int>(i));
		CHECK(menu.options()[i].name().toUtf8() == (i == 0 ? "X  " : "     ") + expected[i]);
		CHECK(std::string(mth::kOpNames[i]) == expected[i]);
	}
	// AND A PICK THROUGH IT WRITES THE ENTRY — the guard passes a live card's pick through.
	menu.onSelection().callback(3);
	CHECK(seat.card->op().control().value() == 3.0);
	CHECK(seat.card->op().readout() == "min");
}

