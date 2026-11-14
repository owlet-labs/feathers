/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/automod/automod_activity.h"
#include "fx/automod/automod_derived.h"
#include "mod/math/math_types.h"
#include "mod/twinfield/twinfield.h"
#include "mod/twinfield/twinfield_types.h"
#include "persist_rig.h"
#include "plugin/device_registry.h"
#include "plugin/mod_rack.h"
#include "plugin/ui/automod_terrain.h"
#include "rack/device_browser.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

// TWIN FIELD.
//
// **THE REFERENCE IS RESTATED, NOT SHARED.** `restatedLayer` below reaches the terrain through AutoMod's
// FULL derive (`deriveAutomodAt` then `activityOf`, the pad's own road) and places the eight points from
// the design's words — not through `readLayer`, `pointOffset` or the fast `activityAtPhases` the device
// uses — so a mistake in any of those disagrees with it instead of being copied into it.

using namespace feathers;
namespace tf = feathers::mod::twinfield;
namespace amd = feathers::fx::automod;
namespace ps = feathers::fx::automod::phase_space;
using LP = tf::LayerParam;

namespace {

constexpr double kTol = 1e-6;

std::vector<float> defaultRow() {
	std::vector<float> row;
	for (const device::ParamDesc& p : tf::kDescriptor.params) {
		row.push_back(static_cast<float>(p.def));
	}
	return row;
}

float& lf(std::vector<float>& row, size_t layer, LP p) { return row[tf::layerParam(layer, p)]; }
float& gf(std::vector<float>& row, tf::Param p) { return row[tf::at(p)]; }

/// A ROW WHERE THE TWO LAYERS DIFFER IN EVERY VIEW CONTROL.
std::vector<float> distinctRow() {
	std::vector<float> row = defaultRow();
	lf(row, 0, LP::PanX) = 0.11f;
	lf(row, 0, LP::PanY) = -0.07f;
	lf(row, 0, LP::Zoom) = 0.5f;
	lf(row, 0, LP::Spread) = 0.8f;
	lf(row, 0, LP::Rotate) = 0.3f;
	lf(row, 0, LP::Gamma) = 0.2f;
	lf(row, 1, LP::PanX) = -0.4f;
	lf(row, 1, LP::PanY) = 0.25f;
	lf(row, 1, LP::Zoom) = -1.0f;
	lf(row, 1, LP::Spread) = 1.3f;
	lf(row, 1, LP::Rotate) = -1.2f;
	lf(row, 1, LP::Gamma) = 1.7f;
	return row;
}

/// THE DESIGN'S WORDS, RESTATED: AutoMod's default coordinate pushed by gamma, the plane `basisFor(rotate,
/// mean home)`, a ring `Spread` zones wide (one zone = 0.125 plane units; zoom is NOT an input since 2026-11-08), Mod 1 east and counter-clockwise,
/// the puck orbiting the pan point `depth` rings wide; each reading AutoMod's pad brightness there.
std::array<double, tf::kPoints> restatedLayerAtAngle(const std::vector<float>& row, size_t layer, double angle);
std::array<double, tf::kPoints> restatedLayer(const std::vector<float>& row, size_t layer, double phase) {
	return restatedLayerAtAngle(row, layer, 2.0 * std::numbers::pi * phase);
}
/// THE SAME, WITH THE PUCK GIVEN AS AN ANGLE ON ITS ORBIT (radians, counter-clockwise from east) — what Orbit Phase
/// says in the design's words, so Off's reading is restated from the angle and not from a conversion to turns.
std::array<double, tf::kPoints> restatedLayerAtAngle(const std::vector<float>& row, size_t layer, double angle) {
	const double panX = lf(const_cast<std::vector<float>&>(row), layer, LP::PanX);
	const double panY = lf(const_cast<std::vector<float>&>(row), layer, LP::PanY);
	const double spread = lf(const_cast<std::vector<float>&>(row), layer, LP::Spread);
	const double rotate = lf(const_cast<std::vector<float>&>(row), layer, LP::Rotate);
	const double gamma = lf(const_cast<std::vector<float>&>(row), layer, LP::Gamma);
	const double depth = lf(const_cast<std::vector<float>&>(row), layer, LP::Depth);

	ps::Phases home = ps::homeFor(0.30, 0.70, 0.10);
	for (double& h : home) {
		h += gamma;
	}
	double mean = 0.0;
	for (const double h : home) {
		mean += h;
	}
	mean /= static_cast<double>(ps::kTriangles);
	const ps::Basis basis = ps::basisFor(rotate, mean);

	const double ring = spread * 0.125;
	const double puckX = panX + depth * ring * std::cos(angle);
	const double puckY = panY + depth * ring * std::sin(angle);
	amd::AutomodCoords coords {};
	coords.lfoHz = 1.0f;
	std::array<double, tf::kPoints> out {};
	for (size_t k = 0; k < tf::kPoints; ++k) {
		const double angle = std::numbers::pi / 4.0 * static_cast<double>(k);
		const double x = puckX + ring * std::cos(angle);
		const double y = puckY + ring * std::sin(angle);
		out[k] = amd::activityOf(amd::deriveAutomodAt(coords, ps::phasesAt(home, basis, x, y)));
	}
	return out;
}

device::ModTickContext stoppedContext() {
	device::ModTickContext c {};
	c.clock = device::TransportClock::unknown();
	return c;
}

tf::TwinField prepared(const std::vector<float>& row) {
	tf::TwinField f;
	f.prepare(48000.0);
	f.setFields(row.data(), row.size());
	return f;
}

} // namespace

TEST_CASE("the fast terrain reading is bit-equal to AutoMod's derive") {
	// THE DEVICE READS THROUGH `rawActivityAtPhases` (seven triangles); AutoMod's pad through the whole derive.
	// Equal to the BIT over planes, gammas and travels, or the device publishes a terrain the card does not draw.
	amd::AutomodCoords coords {};
	coords.lfoHz = 1.0f;
	size_t checked = 0;
	float lo = 1e9f;
	float hi = -1e9f;
	for (const double rotate : { 0.0, 0.37, -2.5 }) {
		for (const double gamma : { 0.0, 0.5, 3.25 }) {
			const tf::Plane plane = tf::planeFor(rotate, gamma);
			for (int i = -10; i <= 10; ++i) {
				for (int j = -10; j <= 10; ++j) {
					const ps::Phases ph = ps::phasesAt(plane.home, plane.basis, i * 0.031, j * 0.027);
					const float fast = amd::rawActivityAtPhases(ph);
					const float full = amd::rawActivityOf(amd::deriveAutomodAt(coords, ph));
					CHECK(fast == full);
					lo = std::min(lo, fast);
					hi = std::max(hi, fast);
					++checked;
				}
			}
		}
	}
	CHECK(checked == 9 * 21 * 21);
	CHECK(hi - lo > 0.5f); // NOT VACUOUS: the sweep crosses real terrain, not one flat value
}

TEST_CASE("AutoMod's pad reads the very functions the modulator reads, unchanged") {
	// THE MOVE TO `automod_activity.h` IS A RE-EXPORT, NOT A COPY: the UI's names ARE the device-level ones, and
	// the normaliser is the measured 3.556. AutoMod's own suites and its pad's terrain cases pin the picture.
	CHECK(&ui::automod_terrain::activityOf == &amd::activityOf);
	CHECK(&ui::automod_terrain::rawActivityOf == &amd::rawActivityOf);
	CHECK(ui::automod_terrain::kActivityFull == 3.556f);
	CHECK(&ui::automod_terrain::kActivityFull == &amd::kActivityFull);
}

TEST_CASE("each output is the crossfade of the two layers' readings, each restated independently") {
	std::vector<float> row = distinctRow();
	for (const float x : { 0.0f, 0.3f, 0.5f, 1.0f }) {
		CAPTURE(x);
		gf(row, tf::Param::Crossfade) = x;
		tf::TwinField f = prepared(row);
		const device::ModTickContext c = stoppedContext();
		for (int t = 0; t < 37; ++t) {
			f.tick(c);
		}
		const auto a = restatedLayer(row, 0, f.phaseForTest(0));
		const auto b = restatedLayer(row, 1, f.phaseForTest(1));
		bool differ = false;
		for (size_t k = 0; k < tf::kPoints; ++k) {
			CAPTURE(k);
			CHECK(std::abs(f.readingForTest(0, k) - a[k]) <= kTol);
			CHECK(std::abs(f.readingForTest(1, k) - b[k]) <= kTol);
			const double want = (1.0 - x) * a[k] + x * b[k];
			CHECK(std::abs(f.outputAt(k) - want) <= kTol);
			differ = differ || std::abs(a[k] - b[k]) > 0.01;
		}
		CHECK(differ); // NOT VACUOUS: the layers read different ground, so the crossfade has something to blend
		CHECK(f.phaseForTest(0) > 0.0); // and the orbit has moved, so the puck term is live
	}
}

TEST_CASE("the crossfade blends output values, not a terrain position between the layers") {
	// THE TRAP: a blend of positions reads the terrain at the midpoint. At x = 0.5 with A and B at
	// different pans, the output is the MEAN of the two readings, which is not the midpoint's reading.
	std::vector<float> row = distinctRow();
	lf(row, 0, LP::Depth) = 0.0f;
	lf(row, 1, LP::Depth) = 0.0f;
	lf(row, 1, LP::Rotate) = lf(row, 0, LP::Rotate);
	lf(row, 1, LP::Gamma) = lf(row, 0, LP::Gamma);
	lf(row, 1, LP::Zoom) = lf(row, 0, LP::Zoom);
	lf(row, 1, LP::Spread) = lf(row, 0, LP::Spread);
	gf(row, tf::Param::Crossfade) = 0.5f;
	tf::TwinField f = prepared(row);
	f.tick(stoppedContext());
	std::vector<float> mid = row;
	lf(mid, 0, LP::PanX) = 0.5f * (lf(row, 0, LP::PanX) + lf(row, 1, LP::PanX));
	lf(mid, 0, LP::PanY) = 0.5f * (lf(row, 0, LP::PanY) + lf(row, 1, LP::PanY));
	const auto atMid = restatedLayer(mid, 0, 0.0);
	const auto a = restatedLayer(row, 0, 0.0);
	const auto b = restatedLayer(row, 1, 0.0);
	bool midDiffers = false;
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CHECK(std::abs(f.outputAt(k) - 0.5 * (a[k] + b[k])) <= kTol);
		midDiffers = midDiffers || std::abs(f.outputAt(k) - atMid[k]) > 0.01;
	}
	CHECK(midDiffers);
}

TEST_CASE("Solo A or Solo B overrides the crossfade; both soloed is the crossfade") {
	std::vector<float> row = distinctRow();
	gf(row, tf::Param::Crossfade) = 0.3f;
	const auto run = [&](bool soloA, bool soloB) {
		gf(row, tf::Param::SoloA) = soloA ? 1.0f : 0.0f;
		gf(row, tf::Param::SoloB) = soloB ? 1.0f : 0.0f;
		tf::TwinField f = prepared(row);
		f.tick(stoppedContext());
		std::array<std::array<double, 3>, tf::kPoints> out {};
		for (size_t k = 0; k < tf::kPoints; ++k) {
			out[k] = { f.outputAt(k), f.readingForTest(0, k), f.readingForTest(1, k) };
		}
		return out;
	};
	for (const auto& [soloA, soloB] : { std::pair { true, false }, std::pair { false, true }, std::pair { true, true },
	                                    std::pair { false, false } }) {
		CAPTURE(soloA);
		CAPTURE(soloB);
		const auto o = run(soloA, soloB);
		for (size_t k = 0; k < tf::kPoints; ++k) {
			const double a = o[k][1];
			const double b = o[k][2];
			const double want = (soloA && !soloB) ? a : (soloB && !soloA) ? b : 0.7 * a + 0.3 * b;
			CHECK(std::abs(o[k][0] - want) <= kTol);
		}
	}
}

TEST_CASE("each layer's spread, pan, rotate and gamma change its own readings and not the other's") {
	const std::vector<float> base = distinctRow();
	const auto readings = [](const std::vector<float>& row) {
		tf::TwinField f = prepared(row);
		f.tick(stoppedContext());
		std::array<std::array<float, tf::kPoints>, tf::kLayers> out {};
		for (size_t l = 0; l < tf::kLayers; ++l) {
			for (size_t k = 0; k < tf::kPoints; ++k) {
				out[l][k] = f.readingForTest(l, k);
			}
		}
		return out;
	};
	const auto before = readings(base);
	for (size_t layer = 0; layer < tf::kLayers; ++layer) {
		for (const auto& [p, delta] : { std::pair { LP::Spread, 0.9f }, std::pair { LP::PanX, 0.2f },
		                                std::pair { LP::PanY, -0.2f }, std::pair { LP::Rotate, 0.9f },
		                                std::pair { LP::Gamma, 0.35f } }) {
			CAPTURE(layer);
			CAPTURE(static_cast<int>(p));
			std::vector<float> row = base;
			lf(row, layer, p) += delta;
			const auto after = readings(row);
			const size_t other = 1 - layer;
			CHECK(after[other] == before[other]); // the OTHER layer: bit for bit
			bool moved = false;
			for (size_t k = 0; k < tf::kPoints; ++k) {
				moved = moved || std::abs(after[layer][k] - before[layer][k]) > 1e-4f;
			}
			CHECK(moved); // ITS OWN layer: the control reaches the readings
			// AND BY VALUE, against the restatement.
			tf::TwinField f = prepared(row);
			f.tick(stoppedContext());
			const auto want = restatedLayer(row, layer, f.phaseForTest(layer));
			for (size_t k = 0; k < tf::kPoints; ++k) {
				CHECK(std::abs(after[layer][k] - want[k]) <= kTol);
			}
		}
	}
}

TEST_CASE("determinism: the same song position gives the same outputs, however the song got there") {
	// FREE, PLAYING: the orbit phase is derived from the song (beats for a division, seconds for a free rate),
	// so an instance that played from bar 1 and a FRESH one dropped at the same position agree.
	for (const int sync : { 2 + 4, 1 }) { // a division (past the two free rates), and the 1 Hz free rate
		CAPTURE(sync);
		std::vector<float> row = distinctRow();
		lf(row, 0, LP::Sync) = static_cast<float>(sync);
		lf(row, 1, LP::Sync) = static_cast<float>(sync);
		lf(row, 1, LP::Rate) = 1.0f;
		const double spb = 0.5; // 120 bpm
		const double tickSeconds = 64.0 / 48000.0;
		// AutoMod's free-rate entries are 0.1 Hz and 1 Hz; index 6 is a division.
	const auto at = [&](int tick) {
			device::ModTickContext c {};
			c.clock.secondsPerBeat = spb;
			c.clock.songPositionSeconds = 3.0 + tick * tickSeconds;
			c.clock.songPositionBeats = (3.0 + tick * tickSeconds) / spb;
			c.clock.playing = true;
			return c;
		};
		tf::TwinField played = prepared(row);
		for (int t = 0; t < 400; ++t) {
			played.tick(at(t));
		}
		tf::TwinField dropped = prepared(row);
		dropped.tick(at(399));
		bool live = false;
		for (size_t k = 0; k < tf::kPoints; ++k) {
			CHECK(std::abs(played.outputAt(k) - dropped.outputAt(k)) <= 1e-5);
		}
		for (size_t l = 0; l < tf::kLayers; ++l) {
			// THE PHASE IS THE SONG'S, RESTATED: position over the period, in the period's own units.
			const tf::LayerSettings ls = tf::settingsOf(row.data(), row.size()).layers[l];
			const double mult = std::exp2(static_cast<double>(ls.rate));
			const double turns = amd::isSyncedRate(sync)
			                         ? at(399).clock.songPositionBeats / (amd::syncDivisionBeats(sync) / mult)
			                         : at(399).clock.songPositionSeconds * (amd::freeRateHzFor(sync) * mult);
			CHECK(std::abs(dropped.phaseForTest(l) - (turns - std::floor(turns))) <= 1e-9);
			CHECK(std::abs(played.phaseForTest(l) - dropped.phaseForTest(l)) <= 1e-6);
			live = live || played.phaseForTest(l) > 0.01;
		}
		CHECK(live);
	}
	// STOPPED: counted from reset, so two fresh instances run the same ticks to the same outputs, bit for bit.
	const std::vector<float> row = distinctRow();
	tf::TwinField x = prepared(row);
	tf::TwinField y = prepared(row);
	for (int t = 0; t < 123; ++t) {
		x.tick(stoppedContext());
		y.tick(stoppedContext());
	}
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CHECK(x.outputAt(k) == y.outputAt(k));
	}
}

TEST_CASE("Retrigger restarts the orbit on a note, Once orbits once and holds") {
	std::vector<float> row = distinctRow();
	lf(row, 0, LP::Mode) = static_cast<float>(tf::Mode::Retrigger);
	lf(row, 1, LP::Mode) = static_cast<float>(tf::Mode::Once);
	lf(row, 0, LP::Sync) = 1.0f; // 1 Hz
	lf(row, 1, LP::Sync) = 1.0f;
	tf::TwinField f = prepared(row);
	device::ModTickContext c = stoppedContext();
	for (int t = 0; t < 100; ++t) {
		f.tick(c);
	}
	CHECK(f.phaseForTest(0) > 0.1); // Retrigger runs before any note
	CHECK(f.phaseForTest(1) == 0.0); // Once waits for one
	c.retrigger = true;
	f.tick(c);
	CHECK(f.phaseForTest(0) == 0.0);
	CHECK(f.phaseForTest(1) == 0.0);
	c.retrigger = false;
	f.tick(c);
	const double step = 64.0 / 48000.0; // 1 Hz, one tick
	CHECK(std::abs(f.phaseForTest(0) - step) <= 1e-12);
	CHECK(std::abs(f.phaseForTest(1) - step) <= 1e-12);
	for (int t = 0; t < 800; ++t) { // past one orbit (750 ticks)
		f.tick(c);
	}
	CHECK(f.phaseForTest(1) == 0.0); // held at the start of the circle, where the orbit ends
	CHECK(f.phaseForTest(0) > 0.0);  // Retrigger keeps running
}

TEST_CASE("Twin Field registers eight outputs, every one routable as a modulation source, by value") {
	const device::Descriptor* d = deviceForTag(tf::kTraits.tag);
	REQUIRE(d != nullptr);
	CHECK(d->outputCount() == 8);
	CHECK(std::string(d->outputs[7].name) == "Mod 8");
	CHECK(maySitInModRack(*d));
	CHECK(isTaggedModulator(*d));
	bool listed = false;
	for (const rack::BrowserGroup& g : rack::browseDevices(kDevices)) {
		for (const rack::BrowserEntry& e : g.entries) {
			listed = listed || device::sameTag(e.deviceTag, tf::kTraits.tag);
		}
	}
	CHECK(listed);

	namespace fm = feathers::mod::math;
	for (size_t o = 0; o < tf::kPoints; ++o) {
		CAPTURE(o);
		rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
		const size_t field = engine.addDevice(tf::kTraits.tag);
		const size_t math = engine.addDevice(fm::kTraits.tag);
		REQUIRE(field != rack::RackEngine::kNoSlot);
		REQUIRE(math != rack::RackEngine::kNoSlot);
		engine.prepare(48000.0, 4096);
		const std::vector<float> row = distinctRow();
		for (size_t i = 0; i < row.size(); ++i) {
			engine.setField(field, i, row[i]);
		}
		const size_t a = static_cast<size_t>(fm::Param::A);
		engine.setRouteAmount(test::outputOf(engine, field, o), test::controlOf(engine, math, a), 0.5f);
		std::array<float, 64> l {};
		std::array<float, 64> r {};
		for (int b = 0; b < 3; ++b) {
			engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		}
		const void* storage = engine.slotAtLane(field).storage.get();
		REQUIRE(storage != nullptr);
		const float published = engine.publishedOutputAt(field, o);
		CHECK(published == tf::kModOps.outputAt(storage, o));
		CHECK(published > 0.0f); // NOT VACUOUS: a reading the route can carry
		// MATH's A spans -1..1, so a full-depth route moves it by 2: base 0 + 0.5 x value x 2.
		CHECK(std::abs(engine.resolvedFieldForTest(math, a) - 0.5 * 2.0 * published) <= kTol);
	}
}

TEST_CASE("Twin Field's settings survive save and load into a fresh instance, value by value") {
	persist::Rig run {};
	std::vector<float> row = distinctRow();
	gf(row, tf::Param::Crossfade) = 0.27f;
	gf(row, tf::Param::SoloB) = 1.0f;
	lf(row, 0, LP::Sync) = 7.0f;
	lf(row, 1, LP::Mode) = static_cast<float>(tf::Mode::Once);
	lf(row, 0, LP::Rate) = -0.75f;
	lf(row, 0, LP::Depth) = 0.0f; // still pucks, so the outputs below do not depend on how long each instance ran
	lf(row, 1, LP::Depth) = 0.0f;
	run.seat(tf::kTraits.tag, row);
	run.block(3);
	const size_t lane = run.engine.slotOf(run.id);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	std::array<float, tf::kPoints> outs {};
	for (size_t k = 0; k < tf::kPoints; ++k) {
		outs[k] = run.engine.publishedOutputAt(lane, k);
	}
	const std::string saved = run.save();

	persist::Rig back {};
	back.load(saved);
	back.block(3);
	const size_t lane2 = back.engine.slotOf(back.id);
	REQUIRE(lane2 != rack::RackEngine::kNoSlot);
	for (size_t i = 0; i < row.size(); ++i) {
		CAPTURE(i);
		CHECK(back.engine.fieldAt(lane2, i) == row[i]);
	}
	bool someNonDefault = false;
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CHECK(back.engine.publishedOutputAt(lane2, k) == outs[k]);
		someNonDefault = someNonDefault || outs[k] != 0.0f;
	}
	CHECK(someNonDefault);
}

TEST_CASE("the plane is held while nothing moves and re-derived when gamma moves") {
	std::vector<float> row = distinctRow();
	lf(row, 0, LP::Depth) = 0.0f;
	tf::TwinField f = prepared(row);
	f.tick(stoppedContext());
	const float first = f.readingForTest(0, 3);
	f.tick(stoppedContext());
	CHECK(f.readingForTest(0, 3) == first);
	lf(row, 0, LP::Gamma) += 0.25f;
	f.setFields(row.data(), row.size());
	f.tick(stoppedContext());
	CHECK(f.readingForTest(0, 3) != first); // a gamma change reaches the plane on the next tick
}

TEST_CASE("Zoom leaves the eight points' terrain coordinates unchanged; Spread scales them about the pan point") {
	// THE DECOUPLING (the zoom/radius coupling was unconvincing; decision: option 1). Zoom is the
	// picture's scale only; the ring — and the orbit, `Depth` rings wide — are sized on the terrain by Spread.
	std::vector<float> row = distinctRow();
	const double phase = 0.37;
	const auto coordsAt = [&](const std::vector<float>& r) {
		return tf::pointCoords(tf::settingsOf(r.data(), r.size()).layers[0], phase);
	};
	const auto base = coordsAt(row);
	for (const float zoom : { -2.0f, 0.0f, 1.5f, 6.0f }) {
		CAPTURE(zoom);
		std::vector<float> zoomed = row;
		lf(zoomed, 0, LP::Zoom) = zoom;
		CHECK(coordsAt(zoomed) == base); // bit for bit
		// AND THE READINGS, through the device: zoom moves no output.
		tf::TwinField a = prepared(row);
		tf::TwinField b = prepared(zoomed);
		a.tick(stoppedContext());
		b.tick(stoppedContext());
		for (size_t k = 0; k < tf::kPoints; ++k) {
			CHECK(a.outputAt(k) == b.outputAt(k));
		}
	}
	// SPREAD SCALES every point's offset from the pan point (the ring AND the orbit), by the ratio of the spreads.
	const double panX = lf(row, 0, LP::PanX);
	const double panY = lf(row, 0, LP::PanY);
	const double s1 = lf(row, 0, LP::Spread);
	std::vector<float> wider = row;
	lf(wider, 0, LP::Spread) = 2.4f;
	const double ratio = static_cast<double>(2.4f) / s1;
	const auto scaled = coordsAt(wider);
	bool moved = false;
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CAPTURE(k);
		CHECK(std::abs((scaled[k][0] - panX) - ratio * (base[k][0] - panX)) <= 1e-12);
		CHECK(std::abs((scaled[k][1] - panY) - ratio * (base[k][1] - panY)) <= 1e-12);
		moved = moved || std::abs(scaled[k][0] - base[k][0]) > 1e-3;
	}
	CHECK(moved);
	// RESTATED RADIUS: point k at Depth 0 sits Spread x 0.125 plane units from the pan point.
	std::vector<float> still = row;
	lf(still, 0, LP::Depth) = 0.0f;
	const auto ring = coordsAt(still);
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CHECK(std::abs(std::hypot(ring[k][0] - panX, ring[k][1] - panY) - s1 * 0.125) <= 1e-12);
	}
}

TEST_CASE("Spread's default reads where the zoom-coupled ring read at default zoom, to float precision") {
	// THE OLD RING, RESTATED: 0.6 of a one-zone half-view divided by 2^zoom, zoom 0. The new default is 0.6 zones, stored
	// as a FLOAT, so the coordinates agree to ~1e-9 and the eight outputs to the figure measured here.
	const std::vector<float> row = defaultRow();
	const tf::LayerSettings ls = tf::settingsOf(row.data(), row.size()).layers[0];
	REQUIRE(ls.zoomOct == 0.0);
	const double oldRing = 0.6 * 0.125 / std::exp2(0.0);
	double worstCoord = 0.0;
	double worstOutput = 0.0;
	for (int i = 0; i < 64; ++i) {
		const double phase = i / 64.0;
		const auto now = tf::pointCoords(ls, phase);
		const tf::Plane plane = tf::planeFor(ls.rotate, ls.gamma);
		for (size_t k = 0; k < tf::kPoints; ++k) {
			const double a = 2.0 * std::numbers::pi * phase;
			const double ak = std::numbers::pi / 4.0 * static_cast<double>(k);
			const double ox = ls.panX + ls.depth * oldRing * std::cos(a) + oldRing * std::cos(ak);
			const double oy = ls.panY + ls.depth * oldRing * std::sin(a) + oldRing * std::sin(ak);
			worstCoord = std::max({ worstCoord, std::abs(now[k][0] - ox), std::abs(now[k][1] - oy) });
			worstOutput = std::max(worstOutput, static_cast<double>(std::abs(tf::terrainAt(plane, now[k][0], now[k][1])
			                                                                   - tf::terrainAt(plane, ox, oy))));
		}
	}
	MESSAGE("default patch, old ring vs Spread 0.6: worst coordinate " << worstCoord << ", worst output " << worstOutput);
	CHECK(worstCoord <= 1e-8);
	CHECK(worstOutput <= 1e-5);
}

namespace {

constexpr uint64_t kDefaultStoppedHash = 18370288327403777445ull;
constexpr uint64_t kDefaultPlayingHash = 14952428909164161333ull;

/// FNV-1a over the bits of every output and both published phases, tick by tick.
struct BitHash {
	uint64_t h = 1469598103934665603ull;
	void add(const void* p, size_t n) {
		const auto* b = static_cast<const unsigned char*>(p);
		for (size_t i = 0; i < n; ++i) {
			h = (h ^ b[i]) * 1099511628211ull;
		}
	}
	void addTick(const tf::TwinField& f) {
		for (size_t k = 0; k < tf::kPoints; ++k) {
			const float v = f.outputAt(k);
			add(&v, sizeof v);
		}
		for (size_t l = 0; l < tf::kLayers; ++l) {
			const double p = f.phaseRead(l);
			add(&p, sizeof p);
		}
	}
};

} // namespace

TEST_CASE("the default patch's outputs are bit-identical to the build before the orbit had an Off") {
	// THE GOLDENS WERE TAKEN FROM MAINLINE (2fd85cf68) BY THIS SAME CASE, before Mode grew "Off" and the layers grew
	// an Orbit Phase: the default row, ticked stopped (the accumulator road) and playing (the song road), every
	// output's bits and both published phases folded into one hash each. A change to what the default patch reads
	// moves them.
	const std::vector<float> row = defaultRow();
	BitHash stopped;
	{
		tf::TwinField f = prepared(row);
		for (int t = 0; t < 2000; ++t) {
			f.tick(stoppedContext());
			stopped.addTick(f);
		}
	}
	BitHash playing;
	{
		tf::TwinField f = prepared(row);
		const double tickSeconds = 64.0 / 48000.0;
		for (int t = 0; t < 2000; ++t) {
			device::ModTickContext c {};
			c.clock.secondsPerBeat = 0.5;
			c.clock.songPositionSeconds = 1.25 + t * tickSeconds;
			c.clock.songPositionBeats = (1.25 + t * tickSeconds) / 0.5;
			c.clock.playing = true;
			c.retrigger = (t % 300) == 0;
			f.tick(c);
			playing.addTick(f);
		}
	}
	MESSAGE("stopped " << stopped.h << " playing " << playing.h);
	CHECK(stopped.h == kDefaultStoppedHash);
	CHECK(playing.h == kDefaultPlayingHash);
}

//-- MODE OFF: THE ORBIT STANDS WHERE ORBIT PHASE PUTS IT -------------------------------------------------------------//

namespace {

device::ModTickContext playingAt(int tick, bool retrigger) {
	device::ModTickContext c {};
	const double tickSeconds = 64.0 / 48000.0;
	c.clock.secondsPerBeat = 0.5;
	c.clock.songPositionSeconds = 2.0 + tick * tickSeconds;
	c.clock.songPositionBeats = (2.0 + tick * tickSeconds) / 0.5;
	c.clock.playing = true;
	c.retrigger = retrigger;
	return c;
}

/// A ROW WITH BOTH LAYERS OFF, each at its own Phase, with the rates and syncs that would move a running orbit fast.
std::vector<float> offRow(float phaseA, float phaseB) {
	std::vector<float> row = distinctRow();
	for (size_t l = 0; l < tf::kLayers; ++l) {
		lf(row, l, LP::Mode) = static_cast<float>(tf::Mode::Off);
		lf(row, l, LP::Sync) = 1.0f; // 1 Hz, were it running
		lf(row, l, LP::Rate) = 1.5f;
		lf(row, l, LP::Depth) = 0.9f;
	}
	lf(row, 0, LP::OrbitPhase) = phaseA;
	lf(row, 1, LP::OrbitPhase) = phaseB;
	return row;
}

} // namespace

TEST_CASE("Mode Off is appended after Once, so a stored Free, Retrigger or Once reads the mode it did, and Free stays the default") {
	REQUIRE(std::size(tf::kModeNames) == 4);
	CHECK(std::string(tf::kModeNames[0]) == "Free");
	CHECK(std::string(tf::kModeNames[1]) == "Retrigger");
	CHECK(std::string(tf::kModeNames[2]) == "Once");
	CHECK(std::string(tf::kModeNames[3]) == "Off");
	for (size_t l = 0; l < tf::kLayers; ++l) {
		CHECK(tf::kParamDescs[tf::layerParam(l, LP::Mode)].def == 0.0);
		CHECK(tf::kParamDescs[tf::layerParam(l, LP::OrbitPhase)].def == 0.0);
		CHECK(std::string(tf::kParamDescs[tf::layerParam(l, LP::OrbitPhase)].key) == (l == 0 ? "a.orbit_phase" : "b.orbit_phase"));
		CHECK(tf::kParamDescs[tf::layerParam(l, LP::OrbitPhase)].modulatable);
		// THE GATES: Rate and Sync while the orbit runs, Phase only while it is Off — each on ITS layer's Mode.
		for (int m = 0; m < static_cast<int>(tf::Mode::kCount); ++m) {
			CAPTURE(l);
			CAPTURE(m);
			const bool off = m == static_cast<int>(tf::Mode::Off);
			CHECK(tf::kParamDescs[tf::layerParam(l, LP::Rate)].gateControl == static_cast<int16_t>(tf::layerParam(l, LP::Mode)));
			CHECK(device::relevantFor(tf::kParamDescs[tf::layerParam(l, LP::Rate)], m) == !off);
			CHECK(device::relevantFor(tf::kParamDescs[tf::layerParam(l, LP::Sync)], m) == !off);
			CHECK(device::relevantFor(tf::kParamDescs[tf::layerParam(l, LP::OrbitPhase)], m) == off);
		}
	}
}

TEST_CASE("Mode Off holds the eight outputs fixed over many blocks, stopped or playing, through notes, at the restated reading") {
	const float phaseA = 1.1f;
	const float phaseB = 4.0f;
	const std::vector<float> row = offRow(phaseA, phaseB);
	tf::TwinField f = prepared(row);
	f.tick(stoppedContext());
	std::array<float, tf::kPoints> first {};
	for (size_t k = 0; k < tf::kPoints; ++k) {
		first[k] = f.outputAt(k);
	}
	// THE VALUE: each layer's reading restated at the ANGLE Phase names, blended by the default crossfade (0.5).
	const auto a = restatedLayerAtAngle(row, 0, phaseA);
	const auto b = restatedLayerAtAngle(row, 1, phaseB);
	bool layersDiffer = false;
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CAPTURE(k);
		CHECK(std::abs(f.readingForTest(0, k) - a[k]) <= kTol);
		CHECK(std::abs(f.readingForTest(1, k) - b[k]) <= kTol);
		CHECK(std::abs(first[k] - (0.5 * a[k] + 0.5 * b[k])) <= kTol);
		layersDiffer = layersDiffer || std::abs(a[k] - b[k]) > 0.01;
	}
	CHECK(layersDiffer);
	// AND IT STAYS: 3000 ticks (four seconds) stopped, then playing with a note every 100 ticks — every output's bits
	// equal to the first tick's, and the published phase the angle's share of the circle.
	for (int t = 0; t < 3000; ++t) {
		f.tick(t < 1500 ? stoppedContext() : playingAt(t, t % 100 == 0));
		for (size_t k = 0; k < tf::kPoints; ++k) {
			REQUIRE(f.outputAt(k) == first[k]);
		}
	}
	CHECK(std::abs(f.phaseRead(0) - static_cast<double>(phaseA) / (2.0 * std::numbers::pi)) <= 1e-12);
	CHECK(std::abs(f.phaseRead(1) - static_cast<double>(phaseB) / (2.0 * std::numbers::pi)) <= 1e-12);
	// NOT VACUOUS: the same row with the orbit running moves the outputs over the same ticks.
	std::vector<float> running = row;
	lf(running, 0, LP::Mode) = static_cast<float>(tf::Mode::Free);
	lf(running, 1, LP::Mode) = static_cast<float>(tf::Mode::Free);
	tf::TwinField g = prepared(running);
	g.tick(stoppedContext());
	const float g0 = g.outputAt(0);
	for (int t = 0; t < 200; ++t) {
		g.tick(stoppedContext());
	}
	CHECK(std::abs(g.outputAt(0) - g0) > 1e-3);
}

TEST_CASE("Orbit Phase puts the read point at that angle on the orbit; at 0 it reads the orbit's own origin") {
	for (const double angle : { 0.0, 0.5 * std::numbers::pi, 2.0, std::numbers::pi, 5.5 }) {
		CAPTURE(angle);
		const std::vector<float> row = offRow(static_cast<float>(angle), static_cast<float>(angle));
		tf::TwinField f = prepared(row);
		f.tick(stoppedContext());
		const double stored = static_cast<double>(static_cast<float>(angle));
		for (size_t l = 0; l < tf::kLayers; ++l) {
			const auto want = restatedLayerAtAngle(row, l, stored);
			for (size_t k = 0; k < tf::kPoints; ++k) {
				CAPTURE(l);
				CAPTURE(k);
				CHECK(std::abs(f.readingForTest(l, k) - want[k]) <= kTol);
			}
		}
	}
	// PHASE 0 IS WHERE THE ORBIT STARTS: a Free orbit's first tick after reset reads phase 0, and Off at 0 reads the same
	// eight values, bit for bit, whatever Rate and Sync are.
	const std::vector<float> off = offRow(0.0f, 0.0f);
	std::vector<float> free = off;
	lf(free, 0, LP::Mode) = static_cast<float>(tf::Mode::Free);
	lf(free, 1, LP::Mode) = static_cast<float>(tf::Mode::Free);
	tf::TwinField x = prepared(off);
	tf::TwinField y = prepared(free);
	x.tick(stoppedContext());
	y.tick(stoppedContext());
	REQUIRE(y.phaseRead(0) == 0.0);
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CHECK(x.outputAt(k) == y.outputAt(k));
	}
	// NOT VACUOUS: a quarter turn reads other ground.
	tf::TwinField q = prepared(offRow(static_cast<float>(0.5 * std::numbers::pi), 0.0f));
	q.tick(stoppedContext());
	bool moved = false;
	for (size_t k = 0; k < tf::kPoints; ++k) {
		moved = moved || std::abs(q.readingForTest(0, k) - x.readingForTest(0, k)) > 1e-3;
	}
	CHECK(moved);
}

TEST_CASE("Orbit Phase is a modulation destination of its own, and a route on it moves the outputs to the restated angle") {
	// THE SOURCE IS A SECOND TWIN FIELD WITH STILL PUCKS (Depth 0), so its outputs are constant and the route's offset
	// is a known number: amount x value x span (2 pi), on Orbit Phase's base.
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	const size_t source = engine.addDevice(tf::kTraits.tag);
	const size_t field = engine.addDevice(tf::kTraits.tag);
	REQUIRE(source != rack::RackEngine::kNoSlot);
	REQUIRE(field != rack::RackEngine::kNoSlot);
	engine.prepare(48000.0, 4096);
	std::vector<float> still = defaultRow();
	lf(still, 0, LP::Depth) = 0.0f;
	lf(still, 1, LP::Depth) = 0.0f;
	for (size_t i = 0; i < still.size(); ++i) {
		engine.setField(source, i, still[i]);
	}
	const float base = 0.7f;
	const std::vector<float> row = offRow(base, base);
	for (size_t i = 0; i < row.size(); ++i) {
		engine.setField(field, i, row[i]);
	}
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	const auto run = [&] {
		for (int b = 0; b < 4; ++b) {
			engine.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		}
	};
	run();
	std::array<float, tf::kPoints> unrouted {};
	for (size_t k = 0; k < tf::kPoints; ++k) {
		unrouted[k] = engine.publishedOutputAt(field, k);
	}
	const size_t phaseA = tf::layerParam(0, LP::OrbitPhase);
	const float amount = 0.4f;
	engine.setRouteAmount(test::outputOf(engine, source, 2), test::controlOf(engine, field, phaseA), amount);
	run();
	const double value = engine.publishedOutputAt(source, 2);
	REQUIRE(value > 0.05);
	const double angle = base + amount * value * 2.0 * std::numbers::pi;
	REQUIRE(angle < 2.0 * std::numbers::pi);
	CHECK(std::abs(engine.resolvedFieldForTest(field, phaseA) - angle) <= 1e-5);
	// THE OUTPUTS ARE THE BLEND OF A AT THE ROUTED ANGLE AND B AT ITS UNROUTED BASE, restated.
	const auto a = restatedLayerAtAngle(row, 0, angle);
	const auto b = restatedLayerAtAngle(row, 1, base);
	bool moved = false;
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CAPTURE(k);
		const float now = engine.publishedOutputAt(field, k);
		CHECK(std::abs(now - (0.5 * a[k] + 0.5 * b[k])) <= 1e-5);
		moved = moved || std::abs(now - unrouted[k]) > 1e-3;
	}
	CHECK(moved); // NOT VACUOUS: the route changed what a destination receives
	// AND NOT RATE'S: Rate on the same layer is not moved by it.
	CHECK(engine.resolvedFieldForTest(field, tf::layerParam(0, LP::Rate)) == doctest::Approx(row[tf::layerParam(0, LP::Rate)]));
}

TEST_CASE("Mode Off and Orbit Phase survive save and load into a fresh instance, and the outputs come back bit for bit") {
	persist::Rig run {};
	const std::vector<float> row = offRow(2.3f, 5.1f);
	run.seat(tf::kTraits.tag, row);
	run.block(3);
	const size_t lane = run.engine.slotOf(run.id);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	std::array<float, tf::kPoints> outs {};
	for (size_t k = 0; k < tf::kPoints; ++k) {
		outs[k] = run.engine.publishedOutputAt(lane, k);
	}
	const std::string saved = run.save();
	CHECK(saved.find("orbit_phase") != std::string::npos);

	persist::Rig back {};
	back.load(saved);
	back.block(7); // a different run length: Off does not depend on it
	const size_t lane2 = back.engine.slotOf(back.id);
	REQUIRE(lane2 != rack::RackEngine::kNoSlot);
	CHECK(back.engine.fieldAt(lane2, tf::layerParam(0, LP::Mode)) == static_cast<float>(tf::Mode::Off));
	CHECK(back.engine.fieldAt(lane2, tf::layerParam(0, LP::OrbitPhase)) == 2.3f);
	CHECK(back.engine.fieldAt(lane2, tf::layerParam(1, LP::OrbitPhase)) == 5.1f);
	for (size_t i = 0; i < row.size(); ++i) {
		CAPTURE(i);
		CHECK(back.engine.fieldAt(lane2, i) == row[i]);
	}
	bool someNonDefault = false;
	for (size_t k = 0; k < tf::kPoints; ++k) {
		CHECK(back.engine.publishedOutputAt(lane2, k) == outs[k]);
		someNonDefault = someNonDefault || outs[k] != 0.0f;
	}
	CHECK(someNonDefault);
}
