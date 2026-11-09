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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "device/device.h"
#include "mod/lfo/lfo_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/samplehold/samplehold_types.h"
#include "mod/stepseq/stepseq_types.h"
#include "plugin/device_registry.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"

#include <cmath>
#include <string>
#include <vector>

// TIME BASE AS A MODULATION DESTINATION ON THE LFO, STEP SEQ, S&H AND MSEG (Time Base is a modulation destination,
// read as general across these devices). By value, each device in a rack engine on a
// playing 120 bpm transport, a macro routed into its Time Base and moved: the engine resolves WHOLE indices only, and the
// device runs at the resolved division from the next tick (its published playhead counts the new period) with no
// jump — at most one cycle or step per block, and an LFO's phase continuous across the change.

namespace device = feathers::device;
namespace backplane = feathers::backplane;
namespace rack = feathers::rack;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 64;
constexpr double kBpm = 120.0;
constexpr double kBlockBeats = kBlock / kRate * kBpm / 60.0;

/// WHAT A DEVICE'S PLAYHEAD COUNTS: an LFO's phase wraps once a cycle; an MSEG's segment ordinal falls once a cycle; a
/// step sequencer's step changes once a step; a S&H's ordinal is its count of samples taken.
enum class Count { PhaseWraps, OrdinalFalls, OrdinalChanges };

struct Case {
	const device::Descriptor* desc;
	std::vector<std::pair<std::string, float>> fields;
	float baseIndex;
	Count count;
	/// The beats one cycle (or step) lasts at a Time Base index — the device's own reading of its table, restated.
	double (*periodBeats)(float index);
	/// THE ORDINAL'S WRAP: a step sequencer counts steps modulo its pattern (16 at the default); 0 for a count that only grows.
	uint32_t wrap = 0;
};

size_t controlNamed(const device::Descriptor& d, const std::string& name) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (name == d.params[i].name) {
			return i;
		}
	}
	return d.params.size();
}

double hzTablePeriod(float index) {
	// "Hz" first, then every division: index − 1 into device.h's table (Rate at its default, ×1).
	return device::kDivisionBeats[static_cast<size_t>(index) - 1];
}
double divisionTablePeriod(float index) {
	return device::kDivisionBeats[static_cast<size_t>(index)]; // the MSEG's table has no Hz
}

device::TransportClock clockAt(double beats) {
	device::TransportClock c = device::TransportClock::fromSecondsPerBeat(60.0 / kBpm);
	c.songPositionBeats = beats;
	c.songPositionSeconds = beats * 60.0 / kBpm;
	c.playing = true;
	return c;
}

void run(const Case& c) {
	const std::string tag = c.desc->traits.tag;
	CAPTURE(tag);
	constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 4 };
	rack::RackEngine engine(kAxes, feathers::hostCatalogue());
	engine.prepare(kRate, 4096);
	const size_t slot = engine.addDevice(c.desc->traits.tag);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	for (const auto& [name, value] : c.fields) {
		const size_t i = controlNamed(*c.desc, name);
		REQUIRE_MESSAGE(i < c.desc->params.size(), name);
		engine.setField(slot, i, value);
	}
	const size_t control = controlNamed(*c.desc, "Time Base");
	REQUIRE(control < c.desc->params.size());
	REQUIRE(c.desc->params[control].modulatable);
	engine.setField(slot, control, c.baseIndex);
	const backplane::Destination dest =
	    feathers::test::ordinalOf(engine, slot, c.desc->nthModulatableOfControl(control));
	engine.setRouteAmount(feathers::test::macro(0), dest, 0.25f);

	std::vector<float> l(kBlock, 0.0f), r(kBlock, 0.0f);
	double beats = 0.0;
	bool havePrev = false;
	uint32_t prevOrdinal = 0;
	float prevPhase = 0.0f;
	// ONE HELD STRETCH: `blocks` blocks at a macro value, counting cycles; the first block of a stretch is where the
	// division changes, and the continuity checks hold across it.
	const auto stretch = [&](float macro, int blocks, float* resolvedOut) {
		engine.setMacro(0, macro);
		int cycles = 0;
		float resolved = -1.0f;
		for (int b = 0; b < blocks; ++b) {
			engine.processStereo(l.data(), r.data(), kBlock, backplane::BlockContext { clockAt(beats) });
			beats += kBlockBeats;
			const float now = engine.resolvedFieldForTest(slot, control);
			CHECK(now == std::round(now)); // STEPPED: a whole index, never between two divisions
			if (resolved < 0.0f) {
				resolved = now;
			}
			CHECK(now == resolved); // and held while the macro is held
			uint32_t ordinal = 0;
			float phase = 0.0f;
			REQUIRE(engine.playheadAt(slot, ordinal, phase));
			if (havePrev) {
				switch (c.count) {
				case Count::PhaseWraps: {
					float d = phase - prevPhase;
					if (d < 0.0f) {
						d += 1.0f;
						++cycles;
					}
					CHECK(d < 0.3f); // NO JUMP: the phase moves on by a block's worth, change or no change
					break;
				}
				case Count::OrdinalFalls:
					cycles += ordinal < prevOrdinal ? 1 : 0;
					break;
				case Count::OrdinalChanges:
					CHECK((c.wrap > 0 ? (ordinal + c.wrap - prevOrdinal) % c.wrap : ordinal - prevOrdinal) <= 1u); // one step a block at most
					cycles += ordinal != prevOrdinal ? 1 : 0;
					break;
				}
			}
			prevOrdinal = ordinal;
			prevPhase = phase;
			havePrev = true;
		}
		// FOLLOWS THE RESOLVED DIVISION: the cycles counted are the song's beats over the period at that index.
		const double expected = blocks * kBlockBeats / c.periodBeats(resolved);
		CAPTURE(macro);
		CAPTURE(resolved);
		CAPTURE(expected);
		CHECK(std::abs(static_cast<double>(cycles) - expected) <= 1.5);
		*resolvedOut = resolved;
	};
	float atRest = 0.0f;
	float moved = 0.0f;
	stretch(0.0f, 600, &atRest);
	CHECK(atRest == c.baseIndex);
	stretch(1.0f, 600, &moved); // the route moves it, from the next tick
	CHECK(moved != atRest);
	float back = 0.0f;
	stretch(0.0f, 600, &back);
	CHECK(back == c.baseIndex);
}

} // namespace

TEST_CASE("Time Base is a modulation destination on the LFO, Step Seq, S&H and MSEG: a route lands on whole divisions "
          "and the device runs at the new one from the next tick, with no jump") {
	namespace lfo = feathers::mod::lfo;
	namespace sq = feathers::mod::stepseq;
	namespace snh = feathers::mod::samplehold;
	namespace ms = feathers::mod::mseg;
	// The base is 1/64 (fast enough to count cycles in a few seconds); the route reaches slower divisions.
	const Case cases[] = {
		{ &lfo::kDescriptor, { { "Mode", 3.0f } }, 1.0f, Count::PhaseWraps, &hzTablePeriod },        // Free
		{ &sq::kDescriptor, { { "Retrig", 0.0f } }, 1.0f, Count::OrdinalChanges, &hzTablePeriod, 16 }, // on the song
		{ &snh::kDescriptor, {}, 1.0f, Count::OrdinalChanges, &hzTablePeriod },                      // Clock: Rate
		{ &ms::kDescriptor, { { "Mode", 3.0f } }, 0.0f, Count::OrdinalFalls, &divisionTablePeriod }, // Free
	};
	for (const Case& c : cases) {
		run(c);
	}
}
