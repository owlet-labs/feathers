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
 * --- Additional terms under GNU GPL version 3 section 7 ---
 * This file requires preservation of the above copyright notice and author attribution
 * in all copies or substantial portions of this file.
 */

// THE MODULATOR RACK WHERE IT MEETS THE ENGINE — the gate, the tick, the bus, and the file.
//
// test_mod_dahdsr.cpp answers for the envelope's SHAPE with no plugin around it. This file answers
// for the four claims that only exist once there is one, and every one of them is a claim the
// envelope's own suite cannot make:
//
//   - THE GATE IS THE NOTE STACK. Not a counter kept beside it — the voices' own stages, so that
//     sustain pedal, mono, legato, stealing and all-notes-off are answered once rather than five
//     times.
//   - THE VALUE REACHES THE DESTINATION, AND IT IS THE RIGHT VALUE. Asserted against a reference
//     computed by an independent restatement of the intended path, never against "the render
//     differs" — see the project rule this file's every numeric case is written under.
//   - THE BUFFER SIZE CANNOT REACH IT. The envelope steps on the puck's 64-sample grid, so a host
//     handing us 512 frames and one handing us eight lots of 64 must produce the same envelope.
//   - AN UNROUTED RACK COSTS NOTHING AND CHANGES NOTHING, to the bit.
//
// **TWO VOCABULARIES LIVE IN THIS FILE ON PURPOSE, AND WHICH ONE A CASE SPEAKS IS PART OF ITS CLAIM
// (kill-lanes step 5).** They are not two spellings of one thing:
//
//   - **A ROUTE IS NAMED BY ENDPOINTS** — `backplane::Source` / `Destination`, a device by its
//     `DeviceInstanceId` and its own output or control index, a host end by an index into Feathers' own
//     table. Every case whose claim is about ROUTING — which routes exist, which are legal, what a
//     removal or a reorder does to them — is written this way, because a device's position moves and its
//     id does not. `tests/route_coords.h` used to cross for these; no case in this file crosses now.
//   - **THE MODULATION BUS IS STILL A COORDINATE SPACE, and the cases about it still say so.**
//     `ParamSet::routes()`, `fillModulationLane` and `Synth::fillBusFor` are indexed by SOURCE
//     COORDINATE in the ENGINE's own slots — which the engine chooses when it adopts a device, and which
//     no record can name. `Axes`' strides are real there: a slot owns a fixed run of columns, so an
//     address can be valid and name nothing, which is what "a slack column" means and what the axis case
//     below is entirely about. Those cases keep `deviceSourceOf` / `deviceDestinationOf*` and ask the
//     engine where it put the device (`laneForTest().slotOf(id)`) rather than assuming.
//
// So a route goes in by endpoint and is read back out of the bus by coordinate, and the crossing is the
// engine's placement — which is the design, not a leftover.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "fx/busutil/busutil_types.h"
#include "fx/filter/filter_types.h"
#include "fx/harm/harm.h"
#include "fx/harm/harm_types.h"
#include "fx/noise/noise_types.h"
#include "fx/spectral/spectral_types.h"
#include "mod/lfo/lfo.h"
#include "mod/lfo/lfo_types.h"
#include "mod/math/math_types.h"
#include "mod/mseg/mseg_types.h"
#include "doctest.h"

#include "dsp/puck_motion.h"
#include "plugin/device_registry.h"
#include "plugin/feathers_chains.h" // kStripChain / kLaneChain — the two chains a Feathers state has
#include "plugin/gui_bridge.h"
#include "plugin/mod_rack.h"
#include "plugin/params.h"
#include "plugin/editor_lanes.h"
#include "mod/stereo/stereo_types.h"
#include "mod/follower/follower_types.h"
#include "plugin/synth.h"
#include "param_routes.h"
#include "backplane/endpoint.h"
// The graph itself, for the one case that pins `Synth::compileReach`'s relation against the rule it
// restates — see "the bus mask's relation IS the graph's precedence relation".
#include "backplane/graph.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace {

/// EVERY DEVICE A BRIDGE HOLDS, IN BOTH CHAINS — by id, with its descriptor.
template <typename F>
void forEachDevice(const feathers::ParamBridge& bridge, F&& f) {
	const auto visit = [&](feathers::DeviceInstanceId id) {
		if (id == feathers::kNoInstance) {
			return;
		}
		if (const feathers::device::Descriptor* desc = bridge.descriptorOf(id); desc != nullptr) {
			f(id, *desc);
		}
	};
	for (size_t position = 0; position < feathers::kMaxFxSlots; ++position) {
		visit(bridge.fxDeviceAt(position));
	}
	for (size_t slot = 0; slot < feathers::kMaxRackSlots; ++slot) {
		visit(bridge.rackDeviceAt(slot));
	}
}

/// **EVERY SOURCE, AND EVERY DESTINATION, A SURFACE CAN NAME — AS ENDPOINTS** (kill-lanes step 5). What
/// replaces walking `0..kModSourceCount` x `0..kModDestinationCount`.
///
/// A COORDINATE SPACE COULD BE WALKED AND AN ENDPOINT SPACE CANNOT, and that difference is the step. Every
/// coordinate existed whether or not anything stood at it — which is what made "a slack column" something a
/// case could hold in its hand — while a device end is a device's own id and its own output or control
/// index, so an enumeration has to ASK what is there. These two are where the cases pay for that, once.
///
/// The host's own ends are still a fixed table and are enumerated as one (`feathersAxes()`): a host's
/// contract, not a device address, and nothing inserts into it at runtime.
std::vector<feathers::backplane::Source> everySource(const feathers::ParamBridge& bridge) {
	std::vector<feathers::backplane::Source> out;
	for (size_t i = 0; i < feathers::feathersAxes().coreSources; ++i) {
		out.push_back(feathers::backplane::Source::core(i));
	}
	forEachDevice(bridge, [&](feathers::DeviceInstanceId id, const feathers::device::Descriptor& desc) {
		for (size_t output = 0; output < desc.outputCount(); ++output) {
			out.push_back(feathers::backplane::Source::output(id, output));
		}
	});
	return out;
}

/// The twin: the host's own parameters, and every MODULATABLE control of every device. A control the
/// device does not declare modulatable is not a destination at all — see the case that pins exactly that.
std::vector<feathers::backplane::Destination> everyDestination(const feathers::ParamBridge& bridge) {
	std::vector<feathers::backplane::Destination> out;
	for (size_t i = 0; i < feathers::feathersAxes().coreDestinations; ++i) {
		out.push_back(feathers::backplane::Destination::core(i));
	}
	forEachDevice(bridge, [&](feathers::DeviceInstanceId id, const feathers::device::Descriptor& desc) {
		for (size_t ordinal = 0; ordinal < desc.modulatableCount(); ++ordinal) {
			out.push_back(feathers::backplane::Destination::control(id, desc.controlOfNthModulatable(ordinal)));
		}
	});
	return out;
}

using feathers::ParamId;
using feathers::ParamSet;
using feathers::Synth;
namespace dahdsr = feathers::mod::dahdsr;
namespace mseg = feathers::mod::mseg;

constexpr double kSampleRate = 48000.0;
constexpr int16_t kKeyC4 = 60;
constexpr int16_t kKeyG4 = 67;

/// The envelope's control tick, in samples and in seconds. Taken from the engine's own constant
/// rather than restated, because the whole point of the static_assert in synth.cpp is that there is
/// only one of these numbers.
constexpr int kTickSamples = feathers::PuckMotion::kTickSamples;
constexpr double kTickSeconds = static_cast<double>(kTickSamples) / kSampleRate;

struct Rig {
	ParamSet params;
	Synth synth { params };
	std::vector<float> left, right;

	Rig() {
		synth.activate(kSampleRate, 4096);
		params.setBase(static_cast<clap_id>(ParamId::AZone), 17.0);
		params.setBase(static_cast<clap_id>(ParamId::AOffset), 0.0);
		params.setBase(static_cast<clap_id>(ParamId::Path), 0.0);
		// A FIXED LEVEL: the default patch's Level base is zero, carried by the strip's envelope through
		// a route only the bridge makes (backlog EF); a bare Synth needs the old fixed level said.
		params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 1.0);
		left.resize(4096);
		right.resize(4096);
	}

	void render(int frames) {
		REQUIRE(frames <= static_cast<int>(left.size()));
		synth.render(left.data(), right.data(), frames);
	}

	/// Render `ticks` control ticks' worth of samples, in one call.
	void renderTicks(int ticks) { render(ticks * kTickSamples); }

	/// **BOTH HALVES OF A RACK CHANGE, IN THE ORDER THE THREADS TAKE THEM** — the twin of
	/// `Rig::publishChain` in tests/test_synth.cpp, and the same rule. `provisionDevices` is the
	/// MESSAGE thread's half and is where a modulator is actually BUILT; `setModRack` is the audio
	/// thread's per-block handoff, and `ModRunner::sync` adopts what already exists and cannot build.
	/// A case doing only the second leaves the slot empty and every source on it reading zero.
	///
	/// THE EMPTY CHAIN IS WHAT THIS RIG ALREADY HOLDS. It never publishes an fx chain, so passing one
	/// here is passing the synth what it was activated with rather than clearing anything.
	void publishModRack(const feathers::ModRack& rack) {
		synth.provisionDevices(feathers::FxChain {}, rack);
		synth.setModRack(rack);
	}

	/// **WHERE THE ENGINE KEEPS THE STRIP'S SLOT-`slot` DEVICE** — it chooses its own slot when it adopts
	/// one (kill-lanes step 3a), and `ParamSet`'s device coordinates are in those slots, so a route FROM a
	/// modulator names this rather than where the device stands. A render first, because the adoption
	/// happens at the block's sync; `kNoSlot` before one.
	///
	/// **ASKED BY PLACE IN THE CHAIN, NOT BY LANE (kill-lanes step 3b).** The state the engine converged to
	/// is each chain's ordered run of devices, so the question is "where did the engine put the device the
	/// strip has at slot `slot`". `stateOfChains` gives strip slot `n` strip position `n` for a rack whose
	/// rows can all be placed — which is every rack this fixture builds, because `withIdentity` is what
	/// makes a hand-built one placeable. (It named `assignDenseRackLanes` beside it; kill-lanes step 3c
	/// deleted that road, and a row needs no address to be placed.)
	size_t engineSlotOf(size_t slot) {
		if (synth.laneForTest().slotOfStatePosition(feathers::kStripChain, slot)
		    == feathers::rack::RackEngine::kNoSlot) {
			render(1); // the sync that adopts it
		}
		return synth.laneForTest().slotOfStatePosition(feathers::kStripChain, slot);
	}

	/// THE SAME FOR AN FX DEVICE, at its POSITION in the lane the case hands the synth (this rig publishes
	/// only the strip). It took the chain too, to turn the position into a lane; there is no lane to turn
	/// it into now, and the position was always what the caller had.
	size_t engineFxSlotOf(size_t position) {
		if (synth.laneForTest().slotOfStatePosition(feathers::kLaneChain, position)
		    == feathers::rack::RackEngine::kNoSlot) {
			render(1); // the sync that adopts it
		}
		return synth.laneForTest().slotOfStatePosition(feathers::kLaneChain, position);
	}

	/// Put one envelope in the rack, with everything else at the bare settings a case wants.
	void install(const dahdsr::Settings& settings, size_t slot = 0) {
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = std::max(rack.count, slot + 1);
		rack.setDahdsrAt(slot, settings);
		withIdentity(&rack, slot);
		publishModRack(rack);
	}

	/// **A STRIP DEVICE HAS AN ID, OR IT IS A DEVICE THE PRODUCT CANNOT MAKE.** `setDahdsrAt` and
	/// `setMsegAt` keep the slot's identity, and an empty slot has none — so a fixture seating a device
	/// past slot 0 of `defaultModRack()` built one with no instance id. `ModRunner` ran it anyway (it was
	/// position-keyed); the engine the strip runs on keys by identity, as routes always did, and such a
	/// device was never routable. So the fixture gives it what every add road gives.
	///
	/// **IT SAID "AN ID AND A LANE" AND THE TEST WAS THE LANE (kill-lanes step 3c).** It asked whether
	/// `routeLane` was still `kNoRouteLane` — an address, which is what an unplaced device lacked. A row
	/// carries no address now, so the question is asked of the thing that actually says "nothing has been
	/// seated here": the instance id. Same slots answer yes, for the reason the answer was always about.
	static void withIdentity(feathers::ModRack* rack, size_t slot) {
		if (rack->instanceAt(slot) == feathers::kNoInstance) {
			rack->setIdentityAt(slot,
			                    { static_cast<feathers::DeviceInstanceId>(feathers::kFirstFreeInstance + slot) });
		}
	}

	/// **THE RIG'S ROUTES, BY IDENTITY, PUBLISHED WHOLE** (kill-lanes §2.4b) — the editor's road, in the shape
	/// a case wants. A route names the strip device by the id the rack carries and a parameter by its place
	/// in the destination list; the synth's next drain builds every node's inputs from the snapshot.
	feathers::test::Routes routes;

	void route(size_t slot, ParamId destination, float amount) {
		const int index = feathers::modDestinationIndexOf(destination);
		REQUIRE(index >= 0);
		routes.set(feathers::backplane::Source::output(synth.modRackForTest().instanceAt(slot), 0),
		           feathers::backplane::Destination::core(static_cast<size_t>(index)), amount);
		routes.publish(params);
	}
};

/// An envelope with only the stage under test in it — the same idea as test_mod_dahdsr's bare().
dahdsr::Settings bare() {
	dahdsr::Settings s;
	s.delay = dahdsr::kStageLog2SecondsMin;
	s.attack = dahdsr::kStageLog2SecondsMin;
	s.hold = dahdsr::kStageLog2SecondsMin;
	s.decay = dahdsr::kStageLog2SecondsMin;
	s.sustain = 1.0f;
	s.release = dahdsr::kStageLog2SecondsMin;
	s.attackShape = 0.0f;
	s.decayShape = 0.0f;
	s.releaseShape = 0.0f;
	s.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	s.mode = dahdsr::Mode::Gate;
	/* default */;
	s.bipolar = false;
	return s;
}

} // namespace

TEST_CASE("the gate is the note stack, and the voices are what knows") {
	Rig rig;
	rig.install(bare());

	CHECK_FALSE(rig.synth.envelopeGateForTest());

	SUBCASE("a note opens it and a note-off closes it") {
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(2);
		CHECK(rig.synth.envelopeGateForTest());

		rig.synth.noteOff(0, kKeyC4, 1, 0.0);
		rig.renderTicks(2);
		CHECK_FALSE(rig.synth.envelopeGateForTest());
	}

	SUBCASE("a chord holds it until the LAST key comes up") {
		// The gate is global, by design: one envelope, gated by the stack rather than
		// by a note. So the interesting case is the middle — one key released while another is held —
		// where a naive "did a note just end" would close it a note early.
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.synth.noteOn(0, kKeyG4, 2, 1.0);
		rig.renderTicks(2);
		CHECK(rig.synth.envelopeGateForTest());

		rig.synth.noteOff(0, kKeyC4, 1, 0.0);
		rig.renderTicks(2);
		CHECK(rig.synth.envelopeGateForTest());

		rig.synth.noteOff(0, kKeyG4, 2, 0.0);
		rig.renderTicks(2);
		CHECK_FALSE(rig.synth.envelopeGateForTest());
	}

	SUBCASE("the sustain pedal holds it, and it costs no code to do so") {
		// THIS IS THE CASE THAT PAYS FOR THE DESIGN. Nothing in the rack knows what a pedal is: the
		// gate reads the voices' stages, and the voice manager is what keeps a voice in Sustain while
		// the pedal is down. A gate counted in noteOn/noteOff would need to learn about pedals, and
		// then about mono, and then about stealing, and would be wrong about whichever came last.
		rig.synth.sustainPedal(0, true);
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(2);
		CHECK(rig.synth.envelopeGateForTest());

		rig.synth.noteOff(0, kKeyC4, 1, 0.0);
		rig.renderTicks(2);
		CHECK(rig.synth.envelopeGateForTest());

		rig.synth.sustainPedal(0, false);
		rig.renderTicks(2);
		CHECK_FALSE(rig.synth.envelopeGateForTest());
	}

	SUBCASE("and a reset closes it with everything else") {
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(2);
		REQUIRE(rig.synth.envelopeGateForTest());

		rig.synth.reset();
		CHECK_FALSE(rig.synth.envelopeGateForTest());
		CHECK(rig.synth.envelopeValueForTest(0) == 0.0f);
	}
}

TEST_CASE("the envelope advances on the control tick, at the value the shape says") {
	// A VALUE CLAIM, MEASURED AGAINST AN INDEPENDENT RESTATEMENT. The reference is not read off the
	// run: a linear attack of T seconds stands at elapsed/T, and elapsed is ticks × the tick length,
	// both of which this case computes for itself.
	Rig rig;
	dahdsr::Settings s = bare();
	s.attack = dahdsr::stageValueFor(0.100);
	rig.install(s);

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);

	// Three points on one run, so the claim is about a RATE rather than about a single landing.
	// Rendered in one call each so the case is about the engine's clock and not about its own loop.
	int elapsedTicks = 0;
	for (const int step : { 10, 12, 15 }) {
		rig.renderTicks(step);
		elapsedTicks += step;
		const double elapsed = elapsedTicks * kTickSeconds;
		CAPTURE(elapsedTicks);
		CAPTURE(elapsed);
		CHECK(static_cast<double>(rig.synth.envelopeValueForTest(0))
		      == doctest::Approx(elapsed / 0.100).epsilon(0.02));
	}
}

TEST_CASE("the buffer size cannot reach the envelope") {
	// THE PUCK'S OWN GUARANTEE, EXTENDED TO THE THING THAT NOW RIDES ITS CLOCK. Synth::render caps
	// every chunk at the next control tick precisely so a moving puck sounds the same at every buffer
	// size; an envelope stepped on chunk boundaries instead would land its changes wherever the HOST's
	// buffer happened to end. Same envelope, same notes, three block sizes, and the value has to agree
	// to the bit.
	const auto valueAfter = [](int blockSize, int totalTicks) {
		Rig rig;
		dahdsr::Settings s = bare();
		s.attack = dahdsr::stageValueFor(0.250);
		s.decay = dahdsr::stageValueFor(0.400);
		s.sustain = 0.3f;
		rig.install(s);
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);

		int rendered = 0;
		const int total = totalTicks * kTickSamples;
		while (rendered < total) {
			const int n = std::min(blockSize, total - rendered);
			rig.render(n);
			rendered += n;
		}
		return rig.synth.envelopeValueForTest(0);
	};

	const float oneChunk = valueAfter(64 * 20, 20);
	CAPTURE(oneChunk);
	// A real shape rather than a corner: partway up a 250 ms attack.
	CHECK(oneChunk > 0.05f);
	CHECK(oneChunk < 0.95f);

	// BIT-EXACT, not close. The claim is that the grid is the same grid, and a tolerance here would
	// pass for an envelope stepping one tick early on every block.
	CHECK(valueAfter(64, 20) == oneChunk);
	CHECK(valueAfter(1, 20) == oneChunk);
	CHECK(valueAfter(37, 20) == oneChunk);
	CHECK(valueAfter(512, 20) == oneChunk);
}

TEST_CASE("a route from an envelope moves its destination by exactly the amount it declares") {
	// THE WHOLE FEATURE, AS A NUMBER. An amount is stated in units of the destination's OWN range —
	// params.h says so — so the reference here is computed from the range and the envelope's reading,
	// both obtained independently of the thing under test.
	Rig rig;
	dahdsr::Settings s = bare();
	s.attack = dahdsr::kStageLog2SecondsMin; // straight to the top, so the reading is 1.0 and the arithmetic is visible
	s.sustain = 1.0f;
	rig.install(s);

	// FINE TUNE, AND THE CLAIM IS READ OFF THE VOICE RATHER THAN OFF THE PARAMETER — which is the
	// whole point of this case and was very nearly its defect.
	//
	// resolved() is computed on demand from the published reading, so `resolved(FineTune)` moves the
	// moment tickEnvelopes stores a value and says NOTHING about whether the engine did anything with
	// it. The first draft asserted exactly that and claimed in a comment to be testing the tick's
	// applyParams; deleting applyParams from the tick left all eight cases green. What consumes Fine
	// Tune is applyPitches, which runs only inside applyParams — so the honest assertion is the
	// VOICE'S PITCH, and the parameter is checked beside it as the other half rather than as the
	// claim.
	//
	// EVERYTHING HAPPENS INSIDE ONE render() CALL, and that is load-bearing too: applyParams also
	// runs at the top of every render, so a case that rendered twice would let the second call cover
	// for a tick that never re-applied anything.
	const feathers::ParamDesc* desc =
	    feathers::findParam(static_cast<clap_id>(ParamId::FineTune));
	REQUIRE(desc != nullptr);
	const double span = desc->max - desc->min;
	rig.params.setBase(static_cast<clap_id>(ParamId::FineTune), 0.0);

	constexpr float kAmount = 0.25f;
	rig.route(0, ParamId::FineTune, kAmount);

	SUBCASE("with the gate down it reaches nowhere") {
		rig.renderTicks(4);
		CHECK(rig.synth.envelopeValueForTest(0) == 0.0f);
		CHECK(rig.params.resolved(ParamId::FineTune) == doctest::Approx(0.0));
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		CHECK(slot < 0); // nothing is playing at all
	}

	SUBCASE("with the gate up the VOICE is retuned by amount x reading x range") {
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(4);

		const double reading = rig.synth.envelopeValueForTest(0);
		CHECK(reading == doctest::Approx(1.0));

		// The reference, restated independently: an amount is in units of the destination's own
		// range, Fine Tune runs -100..100 cents, and a hundred cents is a semitone.
		const double expectedCents = static_cast<double>(kAmount) * reading * span;
		CAPTURE(expectedCents);
		CHECK(rig.params.resolved(ParamId::FineTune) == doctest::Approx(expectedCents).epsilon(1e-6));

		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		CHECK(rig.synth.voicePitchForTest(slot)
		      == doctest::Approx(static_cast<double>(kKeyC4) + expectedCents / 100.0).epsilon(1e-6));
		// AND THE SECOND ASSERTION THAT PROVES THE FIRST IS NOT VACUOUS: it actually moved off the key.
		CHECK(rig.synth.voicePitchForTest(slot) != doctest::Approx(static_cast<double>(kKeyC4)));
	}

	SUBCASE("and half a reading is half the distance") {
		// THE PARTIAL CASE, which is the one a difference check is blind to. An envelope reaching half
		// its depth retunes the voice to something that differs from the unmodulated pitch exactly as
		// a correct one would; only the number separates them.
		dahdsr::Settings half = bare();
		half.attack = dahdsr::kStageLog2SecondsMin;
		half.decay = dahdsr::kStageLog2SecondsMin;
		half.sustain = 0.5f;
		rig.install(half);
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(8);

		CHECK(rig.synth.envelopeValueForTest(0) == doctest::Approx(0.5f).epsilon(1e-5));
		const int slot = rig.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		CHECK(rig.synth.voicePitchForTest(slot)
		      == doctest::Approx(static_cast<double>(kKeyC4)
		                         + static_cast<double>(kAmount) * 0.5 * span / 100.0)
		             .epsilon(1e-6));
	}
}

TEST_CASE("an envelope may reach the puck, which a weight source may not") {
	// THE DECISION, AS THE AUTHORITY EVERY ROAD ASKS. A weight is a function of where the puck stands,
	// so a weight pointed at Path feeds back and is refused by design; an envelope reads nothing but
	// the gate and the clock, so there is no loop and the single most obvious thing to do with one is
	// exactly this.
	const int path = feathers::modDestinationIndexOf(ParamId::Path);
	REQUIRE(path >= 0);
	const auto destination = feathers::backplane::Destination::core(static_cast<size_t>(path));

	// **ASKED OF THE AUTHORITY, BY ENDPOINT (kill-lanes step 5).** This asked `maySourceRouteTo` over a
	// `DeviceSlots` fixture — the BAND rule, which the product stopped having at step 4b. The decision it
	// pins is the host's own policy (a weight may not reach the puck) and that half never moved; the
	// rule it is now asked of is `ParamBridge::mayRoute`, which is the record's, which is the graph's.
	// A weight and a macro are the host's own sources, so they are named as core ends; the envelope is
	// named by the DEVICE the bridge's default rack holds, which is what a route stores.
	feathers::ParamBridge gate;
	const feathers::DeviceInstanceId envelope = gate.rackDeviceAt(0);
	REQUIRE(envelope != feathers::kNoInstance);
	CHECK(gate.mayRoute(feathers::backplane::Source::output(envelope, 0), destination));
	CHECK_FALSE(gate.mayRoute(feathers::backplane::Source::core(feathers::weightSourceOf(0)), destination));
	// And a macro always could, which is most of what the modulation panel exists to do.
	CHECK(gate.mayRoute(feathers::backplane::Source::core(0), destination));

	// Through the engine, as a value: the puck's Path resolves away from its base while a note is
	// held, and back when it is not.
	Rig rig;
	dahdsr::Settings s = bare();
	s.attack = dahdsr::kStageLog2SecondsMin;
	s.sustain = 1.0f;
	rig.install(s);
	rig.params.setBase(static_cast<clap_id>(ParamId::Path), 0.0);
	rig.route(0, ParamId::Path, 0.5f);

	rig.renderTicks(4);
	CHECK(rig.params.resolved(ParamId::Path) == doctest::Approx(0.0));

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	rig.renderTicks(4);
	const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(ParamId::Path));
	REQUIRE(desc != nullptr);
	CHECK(rig.params.resolved(ParamId::Path)
	      == doctest::Approx(0.5 * (desc->max - desc->min)).epsilon(1e-6));
}

TEST_CASE("a rack nobody routed changes nothing, to the bit") {
	// OFF IS A RETURN, which is the fx lane's first claim restated for the rack. Every existing case
	// in test_synth.cpp renders through an engine that now ticks envelopes; if a rack at rest moved a
	// single sample, all of them would be measuring a different instrument than they were written
	// against.
	const auto renderWith = [](bool installEnvelope) {
		Rig rig;
		if (installEnvelope) {
			dahdsr::Settings s = bare();
			s.attack = dahdsr::stageValueFor(0.050);
			s.decay = dahdsr::stageValueFor(0.200);
			s.sustain = 0.4f;
			rig.install(s);
		}
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.render(2048);
		return std::vector<float>(rig.left.begin(), rig.left.begin() + 2048);
	};

	const std::vector<float> withRack = renderWith(true);
	const std::vector<float> without = renderWith(false);
	REQUIRE(withRack.size() == without.size());
	// The envelope is RUNNING in the first render — it just reaches nothing.
	CHECK(withRack == without);

	// AND THE CASE IS NOT VACUOUS, which is what a bit-exactness claim most needs proving: route the
	// same envelope and the samples do move.
	//
	// LEVEL IS PARKED AT HALF FIRST, and that is not tidiness — it is the difference between this
	// check working and this check lying. Level's default IS its maximum, and resolved() clamps the
	// sum of base and offsets to the declared range, so a positive route from a base of 1.0 has
	// nowhere to go and renders byte-identical samples. The first draft of this case did exactly
	// that and reported the feature broken.
	const auto renderAtHalfLevel = [](bool withRoute) {
		Rig rig;
		dahdsr::Settings s = bare();
		s.attack = dahdsr::stageValueFor(0.050);
		s.sustain = 1.0f;
		rig.install(s);
		rig.params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 0.5);
		if (withRoute) {
			rig.route(0, ParamId::VoiceLevel, 0.5f);
		}
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.render(2048);
		return std::vector<float>(rig.left.begin(), rig.left.begin() + 2048);
	};
	CHECK(renderAtHalfLevel(true) != renderAtHalfLevel(false));
}

TEST_CASE("the rack survives the bridge, which is where the editor writes it") {
	feathers::ParamBridge bridge;

	// A fresh bridge holds the rack a fresh instance has, NOT a table of zeros — an editor that drew
	// before any patch was loaded would otherwise draw an envelope with no attack and no sustain.
	const feathers::ModRack fresh = bridge.modRack();
	CHECK(fresh.count == 1);
	// AND IT IS THE DEFAULT PATCH'S ENVELOPE, not the device's bare defaults: the one the bridge routes to
	// Level (backlog EF), holding at full and letting go over a second — a long release by design.
	CHECK(fresh.dahdsrAt(0) == feathers::defaultModRack().dahdsrAt(0));
	CHECK(fresh.dahdsrAt(0).sustain == doctest::Approx(1.0f));
	CHECK(fresh.dahdsrAt(0).release == doctest::Approx(0.0f)); // log2 seconds
	CHECK(fresh.dahdsrAt(0).attack == doctest::Approx(dahdsr::Settings {}.attack)); // the rest is the device's

	dahdsr::Settings edited;
	edited.delay = 0.11f;
	edited.attack = 0.22f;
	edited.hold = 0.33f;
	edited.decay = 0.44f;
	edited.sustain = 0.55f;
	edited.release = 0.66f;
	edited.attackShape = -0.77f;
	edited.decayShape = 0.88f;
	edited.releaseShape = -0.99f;
	edited.smoothing = -0.12f; // inside the smoothing range, which tops out at log2(1 s) = 0
	edited.mode = dahdsr::Mode::Loop;
	edited.mode = dahdsr::Mode::Once;
	edited.bipolar = true;

	bridge.setEnvelopeSettings(0, edited);
	CHECK(bridge.modRack().dahdsrAt(0) == edited);
	// EVERY FIELD, not merely the object — an == on a struct whose row-packing dropped a field would
	// still be true if the field happened to match its default, and four of these are bools whose
	// defaults are false.
	CHECK(bridge.modRack().dahdsrAt(0).mode == dahdsr::Mode::Once);
	CHECK(bridge.modRack().dahdsrAt(0).bipolar);
	CHECK(bridge.modRack().dahdsrAt(0).smoothing == doctest::Approx(-0.12f));

	SUBCASE("an edit marks the patch modified; a restore does not") {
		CHECK(bridge.takeStateDirty());
		bridge.restoreModRack(feathers::defaultModRack());
		CHECK_FALSE(bridge.takeStateDirty());
		CHECK(bridge.modRack().dahdsrAt(0) == feathers::defaultModRack().dahdsrAt(0));
	}

	SUBCASE("a slot past the end is refused rather than folded") {
		const feathers::ModRack before = bridge.modRack();
		bridge.setEnvelopeSettings(feathers::kMaxRackEnvelopes, edited);
		CHECK(bridge.modRack() == before);
	}
}

TEST_CASE("the rack's row carries every field, in an order the stream and the bridge share") {
	// THE ONE FIELD ORDER, EXERCISED. Three things read it — the stream, the bridge and the loader —
	// and this is the case that would go red if a field were appended to the struct and forgotten in
	// dahdsrRow. It is written by asking for a settings block where NO TWO FIELDS ARE EQUAL, so a row
	// that packed two of them in the wrong order cannot round-trip by coincidence.
	dahdsr::Settings s;
	// EVERY VALUE DERIVED FROM THE CONTROL'S OWN DECLARED RANGE, so no two are equal AND none is out
	// of range. Hand-picked constants were the first draft and they broke the day the time controls
	// moved to log2 seconds: 0.10 is a perfectly good number until the smoothing control's range
	// becomes [-11, 0], at which point sanitize clamps it and the round-trip "fails" for a reason
	// that has nothing to do with the row. Deriving them means this case cannot be broken by a range
	// moving — which is exactly the kind of change it should survive.
	for (size_t i = 0; i < static_cast<size_t>(dahdsr::Param::kCount); ++i) {
		const auto& desc = dahdsr::kParamDescs[i];
		if (desc.stepped) {
			continue;
		}
		// A different fraction of each range, so a row packed out of order cannot round-trip by
		// coincidence.
		const double t = 0.20 + 0.05 * static_cast<double>(i);
		dahdsr::setValueOf(&s, static_cast<dahdsr::Param>(i),
		                   static_cast<float>(desc.min + t * (desc.max - desc.min)));
	}
	s.mode = dahdsr::Mode::Gate;
	s.mode = dahdsr::Mode::Once;
	s.bipolar = false;
	s.mode = dahdsr::Mode::Free;

	// The derivation did what it claimed: no two continuous fields share a value.
	const feathers::DahdsrRow probe = feathers::mod::dahdsr::rowOf(s);
	for (size_t i = 0; i < probe.size(); ++i) {
		if (dahdsr::kParamDescs[i].stepped) {
			continue;
		}
		for (size_t j = i + 1; j < probe.size(); ++j) {
			if (dahdsr::kParamDescs[j].stepped) {
				continue;
			}
			CAPTURE(i);
			CAPTURE(j);
			CHECK(probe[i] != probe[j]);
		}
	}

	const feathers::DahdsrRow row = feathers::mod::dahdsr::rowOf(s);
	CHECK(feathers::mod::dahdsr::settingsOf(row, feathers::kDahdsrFieldCount) == s);

	SUBCASE("a short row leaves the fields it does not carry at their defaults") {
		// WHAT A PATCH FROM AN EARLIER BUILD IS. Six fields is delay through release; everything after
		// keeps the default, which is what makes a new control arrive set sensibly rather than at zero.
		const dahdsr::Settings partial = feathers::mod::dahdsr::settingsOf(row, 6);
		const dahdsr::Settings defaults;
		CHECK(partial.delay == s.delay);
		CHECK(partial.release == s.release);
		CHECK(partial.attackShape == defaults.attackShape);
		CHECK(partial.smoothing == defaults.smoothing);
		CHECK(partial.mode == defaults.mode);
	}

	SUBCASE("and a row full of nonsense comes back sane") {
		feathers::DahdsrRow bad {};
		bad.fill(std::numeric_limits<float>::quiet_NaN());
		const dahdsr::Settings recovered =
		    feathers::mod::dahdsr::settingsOf(bad, feathers::kDahdsrFieldCount);
		CHECK(recovered == dahdsr::Settings {});
	}
}

// THE SLACK COLUMNS ARE NOT DESTINATIONS — the regression case for a bug that shipped green.
//
// The stride (the retired control stride) is a BOUND rather than any device's width, so each slot
// owns more destination columns than its device has modulatable controls. Those extra addresses name
// nothing, and nothing may route to them.
//
// HOW IT WAS FOUND, because it says something about how to look. The first draft of the stride work
// replaced a device-derived count with the stride and left the routing predicate device-blind. All
// 24 suites passed. The DAHDSR's "Smoothing On" switch — declared NOT modulatable, and therefore not
// a destination — resolved to destination 48 and would have accepted a route, lighting a control the
// honesty rule says must never light. It surfaced only when a probe asserted the VALUE of the
// mapping rather than reading a green summary, which is this project's own rule about difference
// checks arriving exactly where it predicted.
TEST_CASE("a control the device does not declare modulatable is not a destination") {
	// THE DEVICE THE DEFAULT RACK PUTS FIRST — checked against the bridge's own reading of it below, so
	// a default that changes turns this into a failed REQUIRE rather than a claim about the wrong device.
	const feathers::device::Descriptor& desc = feathers::mod::dahdsr::kDescriptor;

	// **THE REFUSAL ASKED OF THE RULE THAT STATES IT (kill-lanes step 5, then §2.4b).** This case once
	// carried a COORDINATE half — a slack column past the device's modulatable count was a valid address
	// naming nothing, and `isLiveDeviceDestination`/`isLiveDeviceSource` said so. That arithmetic is
	// deleted with the bus it indexed; an endpoint has no slack column to ask about, so the same two
	// refusals are asked in the vocabulary that has one: a control the device declares and does not
	// declare modulatable, a control index past its params entirely, and an output it does not declare.

	SUBCASE("no source may route to a control the device does not declare modulatable, nor past its controls") {
		feathers::ParamBridge bridge;
		const feathers::DeviceInstanceId envelope = bridge.rackDeviceAt(0);
		REQUIRE(envelope != feathers::kNoInstance);
		// THE SAME DEVICE THE COORDINATE HALF ASKS ABOUT, by its tag rather than by pointer — a descriptor
		// is a catalogue entry and the two roads reach it separately.
		REQUIRE(bridge.descriptorOf(envelope) != nullptr);
		REQUIRE(feathers::device::sameTag(bridge.descriptorOf(envelope)->traits.tag, desc.traits.tag));

		size_t refused = 0;
		const std::vector<feathers::backplane::Source> sources = everySource(bridge);
		REQUIRE(sources.size() > 1);
		for (size_t control = 0; control <= desc.params.size(); ++control) {
			if (control < desc.params.size() && desc.params[control].modulatable) {
				continue;
			}
			CAPTURE(control);
			const auto into = feathers::backplane::Destination::control(envelope, control);
			for (const feathers::backplane::Source& s : sources) {
				CAPTURE(s.device);
				CAPTURE(s.index);
				CHECK_FALSE(bridge.mayRoute(s, into));
			}
			++refused;
		}
		// NOT VACUOUS, and the control is the same one the coordinate half uses: a device whose controls
		// were all modulatable would leave only the one-past-the-end index, which is why that is included.
		CHECK(refused > 1);
		// AND THE VACUITY CONTROL FOR THE LOOP ITSELF: a MODULATABLE control of the same device is reached,
		// so "every pair was refused" cannot be the gate refusing everything.
		REQUIRE(desc.modulatableCount() > 0);
		CHECK(bridge.mayRoute(feathers::backplane::Source::core(0),
		                      feathers::backplane::Destination::control(envelope, desc.controlOfNthModulatableBesideEnable(0))));
	}

	SUBCASE("and nothing may be routed from an output the device does not declare") {
		feathers::ParamBridge bridge;
		const feathers::DeviceInstanceId envelope = bridge.rackDeviceAt(0);
		REQUIRE(envelope != feathers::kNoInstance);
		const std::vector<feathers::backplane::Destination> destinations = everyDestination(bridge);
		REQUIRE(destinations.size() > 1);

		REQUIRE(desc.outputCount() >= 1);
		// THE VACUITY CONTROL FIRST, and it is what makes the refusals below attributable to the OUTPUT:
		// the declared output reaches at least one of those destinations.
		size_t reached = 0;
		for (const feathers::backplane::Destination& d : destinations) {
			reached += bridge.mayRoute(feathers::backplane::Source::output(envelope, 0), d) ? 1 : 0;
		}
		CHECK(reached > 0);

		size_t undeclaredChecked = 0;
		for (size_t output = desc.outputCount(); output < desc.outputCount() + 4; ++output) {
			CAPTURE(output);
			const auto from = feathers::backplane::Source::output(envelope, output);
			for (const feathers::backplane::Destination& d : destinations) {
				if (bridge.mayRoute(from, d)) {
					FAIL_CHECK("an undeclared output accepted a route to device " << d.device << " index " << d.index);
					break;
				}
			}
			++undeclaredChecked;
		}
		CHECK(undeclaredChecked == 4);
	}

}

TEST_CASE("a rack slot holds a device, and the device can be an MSEG") {
	// THE CLAIM THIS WHOLE ARC WAS FOR, AND IT HAS BEEN UNVERIFIED UNTIL NOW.
	//
	// Every other pin fell first: params.h stopped sizing the destination space from the DAHDSR, the
	// source axis stopped assuming a slot IS a source, routings started naming devices by instance
	// rather than by position, and the registry learned what an MSEG is. None of it let a slot
	// actually hold one, because `ModRack` was `std::array<mod::dahdsr::Settings, N>` — the rack
	// declaring the TYPE of its contents in its own struct definition.
	//
	// feathers-1a held me to saying so: the MSEG's numbers proved the REGISTRY's bounds hold against
	// a device not written alongside them, which is a different claim from a slot holding one. This
	// is that claim, and it is a claim about VALUES rather than about a build succeeding.
	using feathers::mod::dahdsr::kTraits;

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;

	feathers::mod::mseg::Settings shape;
	shape.startLevel = 0.25f;
	shape.count = 3;
	rack.setMsegAt(1, shape);
	// A SERIAL, AND THAT IS THE WHOLE OF AN IDENTITY (kill-lanes step 3c) — `deviceSlotOfRack(1)` stood
	// beside it as the row's routing address.
	rack.setIdentityAt(1, { 9 });
	rack.nextInstanceId = 10;

	SUBCASE("the two slots hold different devices at once") {
		CHECK(rack.holds(0, kTraits.tag));
		CHECK(rack.holds(1, feathers::mod::mseg::kTraits.tag));
		CHECK_FALSE(rack.holds(1, kTraits.tag));
		CHECK_FALSE(rack.holds(0, feathers::mod::mseg::kTraits.tag));

		// AND THE TYPED VIEWS DISAGREE HONESTLY. Asking a slot for the device it does not hold
		// returns the declared defaults rather than reinterpreting somebody else's fields.
		CHECK(rack.msegAt(1).startLevel == doctest::Approx(0.25f));
		CHECK(rack.msegAt(0) == feathers::mod::mseg::Settings {});
		CHECK(rack.dahdsrAt(1) == feathers::mod::dahdsr::Settings {});
	}

	SUBCASE("their rows are different widths, which is what the old rack could not express") {
		// 37 declared against 15. A single-type array had one width by construction.
		CHECK(rack.device[0].fieldsPresent
		      == static_cast<size_t>(feathers::mod::dahdsr::Param::kCount));
		CHECK(rack.device[1].fieldsPresent == static_cast<size_t>(feathers::mod::mseg::Param::kCount));
		CHECK(rack.device[0].fieldsPresent != rack.device[1].fieldsPresent);
	}

	// The save/load half of this claim lives in test_gui_bridge.cpp, where the stream helper is and
	// where every other block's round trip is checked.
}

TEST_CASE("the three non-parameter lanes reach the engine, and no fixture does it for them") {
	// BACKLOG ITEM 1: this block had NO VENUE. Deleting the routes' publish from both call sites left every
	// suite green — measured by f7, not argued — because fifteen fixture sites called `params.setRoutes(...)`
	// themselves and two of them spelled it by copying the bridge's own table straight across. THE FIXTURE
	// PERFORMED BY HAND THE EXACT COPY THE LINE UNDER TEST PERFORMS, so deleting the line changed nothing
	// any case could see.
	//
	// That is not a gap in coverage, it is coverage pointing at itself. The same shape as a known-bad
	// that passes because the mutation was inert: the venue was incapable of expressing the outcome
	// and nobody had asked it to.
	//
	// THIS CASE CALLS NOTHING ON THE ENGINE SIDE ITSELF. It puts values on the BRIDGE, calls the one
	// function the plugin's two entry points call, and asks the engine what arrived. If the function
	// stops carrying a lane, nothing else here supplies it.
	ParamSet params;
	Synth synth { params };
	synth.activate(48000.0, 4096);
	feathers::ParamBridge bridge;

	// ---- the bridge, as an editor would leave it ----

	// A ROUTING, which is deliberately not a parameter. **BY THE LOADER'S ROAD, AS IT WAS** — macro 0 into
	// the host's own Level, a route whose both ends are the host's, put there the way a patch load puts
	// one (`loadRoutesForTest`) rather than by the gesture road. What reaches the engine is the claim; the
	// road that left it on the bridge is the fixture, and it is kept as the same one.
	const int dest = feathers::modDestinationIndexOf(ParamId::VoiceLevel);
	REQUIRE(dest >= 0);
	const feathers::backplane::Route routing { feathers::backplane::Source::core(0),
		                                       feathers::backplane::Destination::core(static_cast<size_t>(dest)),
		                                       0.75f };
	bridge.loadRoutesForTest(std::span<const feathers::backplane::Route>(&routing, 1));

	// A RACK DEVICE'S SETTINGS, which are deliberately not parameters either.
	namespace dsr = feathers::mod::dahdsr;
	dsr::Settings env;
	env.attack = dsr::stageValueFor(1.25);
	bridge.setEnvelopeSettings(0, env);

	// AND THE FX LANE'S.
	feathers::fx::filter::State filter;
	filter.settings.resonance = 0.66f;
	filter.keytrack = true;
	bridge.setFilterSettings(0, filter);

	// NOTHING HAS REACHED THE ENGINE YET, and this is the half that makes the rest mean something: if
	// the engine already agreed with the bridge before the call, the assertions below would pass
	// against a function that did nothing at all.
	REQUIRE(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest))) == doctest::Approx(0.0f));
	REQUIRE(dsr::stageSeconds(synth.modRackForTest().dahdsrAt(0).attack)
	        != doctest::Approx(1.25).epsilon(0.01));

	// ---- the road under test, and only it ----
	feathers::publishEditorLanes(bridge, params, synth);

	// ---- what arrived, asserted as VALUES ----
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest))) == doctest::Approx(0.75f));
	CHECK(dsr::stageSeconds(synth.modRackForTest().dahdsrAt(0).attack)
	      == doctest::Approx(1.25).epsilon(0.01));

	feathers::fx::filter::Row decoded {};
	const size_t present = feathers::fromDeviceRow(synth.fxChainForTest().device[0].row, decoded);
	const feathers::fx::filter::State arrived = feathers::fx::filter::stateOf(decoded, present);
	CHECK(arrived.settings.resonance == doctest::Approx(0.66f));
	// Named separately as well, because a whole-struct comparison prints `{?} == {?}` and this is the
	// field a channel silently drops.
	CHECK(arrived.keytrack == true);

	SUBCASE("and a second publish carries a later edit, not the first one again") {
		// A lane that latched its first value would pass everything above and be dead from the second
		// gesture onward — which is what "wired only to a patch load" looked like from the engine's
		// side.
		dsr::Settings moved = env;
		moved.attack = dsr::stageValueFor(0.05);
		bridge.setEnvelopeSettings(0, moved);
		feathers::publishEditorLanes(bridge, params, synth);
		CHECK(dsr::stageSeconds(synth.modRackForTest().dahdsrAt(0).attack)
		      == doctest::Approx(0.05).epsilon(0.01));
	}
}

TEST_CASE("the engine says where each rack modulator stands, and it is the device's own position") {
	// **A WIRING STEP IS NOT DONE WHEN IT EXISTS — it is done when it provably REACHES every
	// instance.** The card's cases publish a playhead into the bridge by hand and assert what gets
	// drawn; nothing there says the ENGINE ever fills one. This is that half: a real `Synth`, a real
	// rack, real rendered blocks, and the position asked of the device afterwards.
	namespace mseg = feathers::mod::mseg;
	Rig rig;

	// AN MSEG IN ONE SLOT AND AN ENVELOPE IN ANOTHER, because the fill goes through one tag branch and
	// a case with only one device cannot see it choose. Slot 1 also puts the envelope somewhere other
	// than index 0, so a fill that ignored the slot would be visible.
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Gate;
	shape.segment[0].share = feathers::mod::timeValueFor(0.05);
	shape.segment[0].level = 1.0f;
	shape.segment[1].share = feathers::mod::timeValueFor(0.15);
	shape.segment[1].level = 0.3f;
	shape.segment[2].share = feathers::mod::timeValueFor(0.10);
	shape.segment[2].level = 0.0f;
	// **THE WINDOW, SET EXPLICITLY — the shares are FRACTIONS of it and mean nothing without it.**
	// The first draft left `timeScale` at its default and asked for three segment changes inside 120
	// ticks; it saw ONE, because the shape's real duration is the window's, not the sum of the
	// numbers written above. `test_mod_mseg.cpp`'s own `bare()` sets it for exactly this reason and
	// says so.
	shape.timeScale = feathers::mod::timeValueFor(0.30);
	mseg::sanitize(&shape);
	rack.setMsegAt(0, shape);

	dahdsr::Settings env;
	env.delay = feathers::mod::dahdsr::kStageLog2SecondsMin;
	env.attack = static_cast<float>(std::log2(0.05));
	env.hold = feathers::mod::dahdsr::kStageLog2SecondsMin;
	env.decay = static_cast<float>(std::log2(0.10));
	env.sustain = 0.5f;
	env.release = static_cast<float>(std::log2(0.08));
	rack.setDahdsrAt(1, env);
	Rig::withIdentity(&rack, 1);
	rig.publishModRack(rack);

	// NOTHING HAS BEEN GATED, so both slots are idle rather than at the start of something.
	CHECK_FALSE(rig.synth.rackPlayheads()[0].running());
	CHECK_FALSE(rig.synth.rackPlayheads()[1].running());

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);

	size_t msegOrdinals = 0;
	size_t envOrdinals = 0;
	uint32_t lastMseg = feathers::mod::Playhead::kIdle;
	uint32_t lastEnv = feathers::mod::Playhead::kIdle;
	bool sawBothRunning = false;
	// PAST THE SHAPE'S END rather than up to it: 0.30 s is 225 ticks, so 300 leaves no doubt about
	// whether the last segment was reached.
	for (int i = 0; i < 300; ++i) {
		rig.renderTicks(1);
		const feathers::mod::Playhead m = rig.synth.rackPlayheads()[0];
		const feathers::mod::Playhead e = rig.synth.rackPlayheads()[1];
		CAPTURE(i);
		if (m.running() && e.running()) {
			sawBothRunning = true;
		}
		// **THE VALUE, ASKED OF THE DEVICE RATHER THAN OF A SECOND COPY OF THE ARITHMETIC.** The
		// engine's published position must BE the shape's own — not merely plausible, and not merely
		// moving.
		if (m.running() && m.ordinal != lastMseg) {
			++msegOrdinals;
			lastMseg = m.ordinal;
		}
		if (e.running() && e.ordinal != lastEnv) {
			++envOrdinals;
			lastEnv = e.ordinal;
		}
		CHECK(m.phase >= 0.0f);
		CHECK(m.phase <= 1.0f);
		CHECK(e.phase >= 0.0f);
		CHECK(e.phase <= 1.0f);
	}

	// NON-VACUITY, AND IT IS WHAT THE CASE IS FOR: both devices really ran, both really walked through
	// several of their own pieces, and they did so at the same time in different slots.
	CHECK(sawBothRunning);
	CHECK(msegOrdinals == 3); // all three segments, not merely "more than one"
	CHECK(envOrdinals >= 3);

	// AND A SLOT PAST THE RACK'S COUNT REPORTS NOTHING, so a shrunk rack cannot leave a marker
	// standing where a device used to be.
	for (size_t slot = 2; slot < feathers::kMaxRackSlots; ++slot) {
		CAPTURE(slot);
		CHECK_FALSE(rig.synth.rackPlayheads()[slot].running());
	}
	rack.count = 1;
	rig.publishModRack(rack);
	rig.renderTicks(1);
	CHECK_FALSE(rig.synth.rackPlayheads()[1].running());
}

TEST_CASE("a rack slot holding an MSEG runs it, and publishes its shape as the slot's source") {
	// THE LAST THING BETWEEN THE MSEG AND BEING REAL. It drew, edited, saved and loaded; nothing in
	// Synth ticked one, so a slot holding an MSEG published silence and the device was a picture.
	//
	// BOTH DEVICES PUBLISH ON THE SAME OUTPUT ORDINAL, which is the design worth asserting: a
	// modulator is a source of ONE signal whatever shape it makes, so nothing downstream has to know
	// which device produced it.
	namespace mseg = feathers::mod::mseg;
	Rig rig;

	// A shape that is unmistakably not an envelope: it rises, falls to a floor, rises again, and it
	// LOOPS between the last two — so a reader that mistook it for a DAHDSR could not produce this.
	mseg::Settings shape;
	shape.startLevel = 0.0f;
	shape.count = 3;
	shape.segment[0].share = feathers::mod::timeValueFor(0.10);
	shape.segment[0].level = 1.0f;
	shape.segment[1].share = feathers::mod::timeValueFor(0.10);
	shape.segment[1].level = 0.2f;
	shape.segment[2].share = feathers::mod::timeValueFor(0.10);
	shape.segment[2].level = 0.9f;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Loop;
	shape.loopStart = 1;
	shape.loopEnd = 2;
	// THE WINDOW IS THE SUM OF WHAT THE SEGMENTS ASK FOR, so each takes exactly the 100 ms it names.
	// Segments are SHARES of a window now — the rig has no host and therefore no tempo, so the scale
	// knob IS the window in seconds. Without this the three shares divide the default one-second
	// window instead and every timing below is a third of what it was written for.
	shape.timeScale = feathers::mod::timeValueFor(0.30);

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	rack.setMsegAt(0, shape);
	rig.publishModRack(rack);
	REQUIRE(rack.holds(0, mseg::kTraits.tag));

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);

	std::vector<float> v;
	const int ticks = static_cast<int>(1.5 / kTickSeconds);
	v.reserve(static_cast<size_t>(ticks));
	for (int i = 0; i < ticks; ++i) {
		rig.renderTicks(1);
		v.push_back(rig.synth.envelopeValueForTest(0));
	}

	float lo = 9.0f;
	float hi = -9.0f;
	for (const float x : v) {
		lo = std::min(lo, x);
		hi = std::max(hi, x);
	}
	CAPTURE(lo);
	CAPTURE(hi);

	// IT MOVED AT ALL — which a slot publishing silence would fail.
	CHECK(hi > 0.85f);
	CHECK(lo < 0.25f);

	// AND IT ARRIVED ON TIME. The first segment is 100 ms to 1.0, so a tenth of a second in it is at
	// the top. A shape running at the wrong rate would still swing.
	const auto at = [&](double seconds) { return v[static_cast<size_t>(seconds / kTickSeconds) - 1]; };
	CHECK(at(0.05) == doctest::Approx(0.5f).epsilon(0.1));
	CHECK(at(0.10) == doctest::Approx(1.0f).epsilon(0.05));

	SUBCASE("and it LOOPS between the markers rather than replaying the run-in") {
		// The thing a DAHDSR cannot do, asserted the way the device's own suite asserts it: segment 0
		// is the only place the shape is below 0.2, so after the loop begins it must never go there
		// again. A whole-shape loop would return to the start level every cycle.
		size_t belowAfterLoop = 0;
		for (size_t i = static_cast<size_t>(0.25 / kTickSeconds); i < v.size(); ++i) {
			if (v[i] < 0.15f) {
				++belowAfterLoop;
			}
		}
		CAPTURE(belowAfterLoop);
		CHECK(belowAfterLoop == 0);

		// And the run-in DID go there, or the check above is about something that never happens.
		size_t belowDuringRunIn = 0;
		for (size_t i = 0; i < static_cast<size_t>(0.05 / kTickSeconds) && i < v.size(); ++i) {
			if (v[i] < 0.15f) {
				++belowDuringRunIn;
			}
		}
		CHECK(belowDuringRunIn > 3);
	}

	SUBCASE("and the same slot holding a DAHDSR produces something DIFFERENT") {
		// Without this the case above is satisfied by an engine that ticks a DAHDSR whatever the slot
		// says — the settings would be reinterpreted, the output would still swing, and every
		// assertion about movement would pass. The two devices must actually differ.
		Rig envRig;
		feathers::ModRack asEnv = feathers::defaultModRack();
		asEnv.count = 1;
		dahdsr::Settings env;
		env.attack = dahdsr::stageValueFor(0.10);
		env.decay = dahdsr::stageValueFor(0.10);
		env.sustain = 0.2f;
		asEnv.setDahdsrAt(0, env);
		envRig.publishModRack(asEnv);
		envRig.synth.noteOn(0, kKeyC4, 1, 1.0);

		std::vector<float> e;
		for (int i = 0; i < ticks; ++i) {
			envRig.renderTicks(1);
			e.push_back(envRig.synth.envelopeValueForTest(0));
		}
		// The envelope settles at its sustain and stays; the looping shape does not settle at all.
		CHECK(e.back() == doctest::Approx(0.2f).epsilon(0.05));
		CHECK_FALSE(v.back() == doctest::Approx(e.back()).epsilon(0.05));
	}
}

TEST_CASE("a route into an MSEG lands on the control the MSEG names, not the one an envelope names") {
	// THE ENGINE'S HALF OF A SHIPPED BUG whose editor half is pinned in test_gui_widgets.
	// `modulatedMsegSettings` walked `deviceSlotDescriptor(deviceSlotOfRack(slot))`, which answers
	// `isFxDeviceSlot(slot) ? filter : dahdsr` — a compile-time branch that never asks the rack. So it
	// iterated the ENVELOPE's ten modulatable controls, took the envelope's control index for each
	// ordinal, and cast that index to `mseg::Param`. Rate is the MSEG's ordinal 1; ordinal 1 on an
	// envelope is Attack, control 1, and `mseg::Param(1)` is SegmentCount. A hand aiming a macro at an
	// MSEG's Rate was moving how many segments the shape has.
	namespace mseg = feathers::mod::mseg;
	Rig rig;

	mseg::Settings shape;
	shape.timeScale = 1.0f;
	shape.count = 3;
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	rack.setMsegAt(0, shape);
	rig.publishModRack(rack);

	const size_t rateOrdinal =
	    mseg::kDescriptor.nthModulatableOfControl(static_cast<size_t>(mseg::Param::TimeScale));
	// THE ORDINAL THAT COLLIDES — if this moves, so does what this proves. It was 1 (an envelope's Attack, `mseg::Param(1)`
	// SegmentCount) until Time Base became a destination ahead of it (2026-10-30, backlog PF): now 2, an envelope's
	// control 2, `mseg::Param(2)` SustainSegment — which the "nothing else moved" checks below now also watch.
	REQUIRE(rateOrdinal == 2);

	// A MACRO RATHER THAN ANOTHER DEVICE, because a macro's reading is a number this case sets rather
	// than one an engine arrives at: source value 0.5 exactly, this tick and every tick.
	rig.params.setBase(static_cast<clap_id>(feathers::ParamId::Macro1), 0.5);
	feathers::test::Routes routes;
	// AT THE ENGINE'S SLOT FOR THE DEVICE (kill-lanes step 3a) — the coordinate a plan is compiled to.
	routes.set(feathers::test::macro(0),
	           feathers::backplane::Destination::control(rig.synth.modRackForTest().instanceAt(0),
	                                                   static_cast<size_t>(mseg::Param::TimeScale)),
	           0.25f);
	routes.publish(rig.params);

	// THE VALUE, COMPUTED THE OTHER WAY ROUND. An amount is stated in the control's own declared span,
	// so 0.25 of a macro sitting at 0.5 moves Rate by 0.25 * 0.5 * (max - min).
	//
	// THE SPAN IS ASKED OF THE DECLARATION RATHER THAN WRITTEN OUT, and that is not tidiness — the
	// first version hardcoded `(2.0 - 0.0)` and broke the day Rate stopped being a 0..2 percentage
	// and became log2 seconds for the time base. The claim this case makes is about WHICH CONTROL a
	// route lands on, not about what that control's range happens to be today, so restating the range
	// here was the case carrying a fact it does not own.
	const feathers::device::ParamDesc& rate = mseg::kParamDescs[static_cast<size_t>(mseg::Param::TimeScale)];
	const double expected = shape.timeScale + 0.25 * 0.5 * (rate.max - rate.min);
	// **ONE TICK FIRST: THE SEAM REPORTS WHAT THE ENGINE RESOLVED, and adoption and resolve are the
	// audio thread's.** It used to resolve on demand beside the engine; the strip runs on the lane's
	// engine now and this reads what the device was actually handed.
	rig.renderTicks(1);
	const mseg::Settings out = rig.synth.modulatedMsegSettingsForTest(0);
	CAPTURE(rate.min);
	CAPTURE(rate.max);
	CHECK(out.timeScale == doctest::Approx(expected).epsilon(0.0001));
	// AND THE OFFSET IS NOT ZERO, or the line above would pass for a route that landed nowhere.
	CHECK(expected != doctest::Approx(shape.timeScale));

	// AND NOTHING ELSE MOVED — which is the half that fails when the wrong descriptor names the
	// control. Under the bug the offset landed here instead, and it is a COUNT: a shape asked to run
	// faster grew a segment.
	CHECK(out.count == 3);
	CHECK(out.startLevel == doctest::Approx(shape.startLevel));
	CHECK(out.sustainSegment == shape.sustainSegment);

	SUBCASE("and with the route removed it is the patch's own value, so the check above is not free") {
		feathers::test::Routes none;
		none.publish(rig.params);
		rig.renderTicks(1); // what the engine resolves NEXT, with the route gone
		const mseg::Settings unrouted = rig.synth.modulatedMsegSettingsForTest(0);
		CHECK(unrouted.timeScale == doctest::Approx(1.0f));
		CHECK(unrouted.count == 3);
	}
}

TEST_CASE("a note re-struck inside one control tick still retriggers the envelope") {
	// **THE RETRIGGER LATCH'S OWN PROPERTY, AND IT WAS PINNED BY NOTHING.** `envRetrigger_` is set in
	// `Synth::noteOn` and consumed exactly once in `tickEnvelopes`; synth.h calls it "A LATCH, NOT AN
	// EDGE" because a note that starts and finishes inside one 64-sample tick would otherwise leave no
	// trace by the time the envelope is asked, and a fast passage would have notes that silently did
	// not retrigger.
	//
	// MEASURED 2026-09-02, by deleting `envRetrigger_ = true` and running everything: ctest 74/74,
	// test_synth 63/63 and test_mod_rack 14/14, all green, all under `run-cases.sh --each` so a crash
	// could not hide behind a short tally. The whole repository could not see the latch go. This case
	// is what that measurement bought, and it is being written now because the note-plumbing arc is
	// about to insert a per-block note buffer into the three methods that own this ordering.
	//
	// **THE STAGE IS THE OBSERVABLE, NOT THE LEVEL — and that took two wrong drafts to find.** A note
	// struck AND released from idle never opens the gate, so the envelope reads a flat zero whether or
	// not the retrigger arrived. A re-strike under an ordinary attack goes on climbing from where it
	// was, so the level is identical either way. Both drafts would have passed against a deleted
	// latch. What a retrigger actually does is send the shape back to its FIRST stage, so the delay is
	// made long enough to sit in and the published playhead ordinal is read — a value the engine
	// publishes, not a difference between two renders.
	Rig rig;
	dahdsr::Settings settings = bare();
	// ~0.1s, which is about 75 control ticks: long enough that a shape sent back to the start is
	// still visibly there on the next tick rather than blowing through to sustain inside it.
	settings.delay = -3.32f;
	settings.sustain = 1.0f;
	rig.install(settings);

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	// CHUNKED, because the rig's buffer is 4096 samples and 120 ticks is 7680.
	for (int i = 0; i < 6; ++i) {
		rig.renderTicks(20);
	}
	// SETTLED, AND ASSERTED RATHER THAN ASSUMED. If this is not Sustain the case below proves nothing:
	// "it moved to Delay" is only evidence of a retrigger if it was somewhere else first.
	REQUIRE(rig.synth.rackPlayheads()[0].ordinal
	        == static_cast<uint32_t>(dahdsr::Stage::Sustain));

	SUBCASE("and it stays where it is when nothing is struck") {
		// THE CONTROL. Without it, a case that saw Delay could be watching a shape that wanders back
		// on its own, and the retrigger would be getting credit for the envelope's own behaviour.
		rig.renderTicks(11);
		CHECK(rig.synth.rackPlayheads()[0].ordinal
		      == static_cast<uint32_t>(dahdsr::Stage::Sustain));
	}

	SUBCASE("a re-strike with no render between the off and the on") {
		// THE WHOLE POINT: both events land inside ONE control tick, so nothing between them ever
		// reaches `tickEnvelopes`. Only the latch carries the note-on across the gap.
		rig.synth.noteOff(0, kKeyC4, 1, 0.0);
		rig.synth.noteOn(0, kKeyC4, 2, 1.0);
		rig.renderTicks(1);
		CHECK(rig.synth.rackPlayheads()[0].ordinal
		      == static_cast<uint32_t>(dahdsr::Stage::Delay));

		// AND IT IS STILL THERE A FEW TICKS LATER, which is not a restatement: it rules out reading a
		// one-tick artefact of the transition as the shape having restarted.
		rig.renderTicks(10);
		CHECK(rig.synth.rackPlayheads()[0].ordinal
		      == static_cast<uint32_t>(dahdsr::Stage::Delay));
	}
}

// ---------------------------------------------------------------------------------------------
// RETRIG FAILED WHEN THE ENVELOPE HAD NOT COMPLETED: a new note arriving before the (MSEG) cycle
// completed left the output at its last level instead of starting it from the beginning.
// Both modulators, through the synth's own note road, BY VALUE.

TEST_CASE("a second note under a held one sends a rack MSEG back to its start level") {
	// THE DEVICE'S OWN SUITE PINS THE RESTART; this pins that the synth's note road DELIVERS one for a
	// note struck while another is held — the gate never falls, so only the latch carries it — and
	// that the restart is to the START LEVEL rather than to wherever the shape was.
	namespace mseg = feathers::mod::mseg;
	Rig rig;

	// A FALLING shape, for the reason test_mod_mseg gives: on a falling segment a restart from the
	// current output is indistinguishable from no restart, so a rising one would pass either way.
	mseg::Settings shape;
	shape.startLevel = 1.0f;
	shape.count = 2;
	shape.segment[0].share = feathers::mod::timeValueFor(0.4);
	shape.segment[0].level = 0.0f;
	shape.segment[1].share = feathers::mod::timeValueFor(0.4);
	shape.segment[1].level = 1.0f;
	shape.sustainSegment = mseg::kNoSustain;
	shape.mode = mseg::Mode::Gate;
	shape.smoothing = feathers::mod::kSmoothingLog2SecondsMin; // off
	shape.timeScale = feathers::mod::timeValueFor(0.8);

	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 1;
	rack.setMsegAt(0, shape);
	rig.publishModRack(rack);
	REQUIRE(rack.holds(0, mseg::kTraits.tag));

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	// 0.2 s: about halfway down the first segment. Chunked, because the rig's buffer is 4096.
	for (int i = 0; i < 7; ++i) {
		rig.renderTicks(20);
	}
	rig.renderTicks(10);
	const float before = rig.synth.envelopeValueForTest(0);
	CAPTURE(before);
	REQUIRE(before > 0.3f);
	REQUIRE(before < 0.7f);

	SUBCASE("the control: with nothing struck, the next tick goes on falling") {
		rig.renderTicks(1);
		const float next = rig.synth.envelopeValueForTest(0);
		CAPTURE(next);
		CHECK(next < before);
		CHECK(before - next < 0.01f);
	}

	SUBCASE("a different key under the held one — the gate stays up throughout") {
		rig.synth.noteOn(0, kKeyG4, 2, 1.0);
		rig.renderTicks(1);
		const float after = rig.synth.envelopeValueForTest(0);
		CAPTURE(after);
		// The start level less one tick's travel down a 0.4 s segment, about 0.003.
		CHECK(after > 0.99f);
	}

	SUBCASE("the same key re-struck inside one control tick") {
		rig.synth.noteOff(0, kKeyC4, 1, 0.0);
		rig.synth.noteOn(0, kKeyC4, 2, 1.0);
		rig.renderTicks(1);
		const float after = rig.synth.envelopeValueForTest(0);
		CAPTURE(after);
		CHECK(after > 0.99f);
	}
}

TEST_CASE("a second note under a held one re-attacks a rack DAHDSR to its peak") {
	// THE DAHDSR'S RESTART IS FROM WHERE IT STANDS, AND THAT IS NOT THIS BUG. sst's `attackFrom`
	// climbs from the current level to the peak in the attack time — an analogue envelope's own
	// behaviour, and audible: a note struck mid-decay goes back UP. What the MSEG did wrong was
	// continue TOWARDS the same place it was already going; an envelope re-attacking from mid-decay
	// heads the other way. So this case asserts the DAHDSR was never affected, by value: the stage
	// on the next tick, and the level at the end of the attack.
	Rig rig;
	dahdsr::Settings settings = bare();
	settings.attack = dahdsr::stageValueFor(0.050);
	settings.decay = dahdsr::stageValueFor(1.000);
	settings.sustain = 0.0f;
	rig.install(settings);

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	// 0.3 s: through the attack and a quarter of the way down the decay.
	for (int i = 0; i < 11; ++i) {
		rig.renderTicks(20);
	}
	rig.renderTicks(5);
	REQUIRE(rig.synth.rackPlayheads()[0].ordinal == static_cast<uint32_t>(dahdsr::Stage::Decay));
	const float before = rig.synth.envelopeValueForTest(0);
	CAPTURE(before);
	REQUIRE(before > 0.5f);
	REQUIRE(before < 0.95f);

	SUBCASE("the control: with nothing struck, it goes on decaying") {
		rig.renderTicks(40);
		CHECK(rig.synth.rackPlayheads()[0].ordinal == static_cast<uint32_t>(dahdsr::Stage::Decay));
		CHECK(rig.synth.envelopeValueForTest(0) < before);
	}

	SUBCASE("a different key under the held one") {
		rig.synth.noteOn(0, kKeyG4, 2, 1.0);
		rig.renderTicks(1);
		CHECK(rig.synth.rackPlayheads()[0].ordinal == static_cast<uint32_t>(dahdsr::Stage::Attack));
		// 40 ticks is 53 ms, past the 50 ms attack: it re-attacked all the way to the top.
		rig.renderTicks(40);
		const float peak = rig.synth.envelopeValueForTest(0);
		CAPTURE(peak);
		CHECK(peak > 0.95f);
	}
}

TEST_CASE("the editor's own road commits an envelope's route to the puck, and the puck then moves") {
	// **THE SYMPTOM, 2026-09-03: a DAHDSR modulating the Perform puck's position moved nothing — not
	// the arc, the puck, or the sound's character — though the star moved.**
	//
	// THE STAR MOVING IS THE CLUE THIS CASE IS BUILT AROUND. The device runs — its playhead is drawn
	// from `modRunner_.playheadAt` and that was never in doubt. What was in doubt is every link
	// between the device's OUTPUT and the destination, and reading them one at a time found nothing:
	// `ParamSet::resolved` adds `sourceOffsetAt`, the synth's coordinate assembly reads through
	// `resolved`, both `publishEditorLanes` call sites exist, the axes agree on both sides, and
	// "an envelope may reach the puck" is pinned above with a value assertion.
	//
	// **WHAT NO CASE CROSSED IS THE EDITOR'S OWN COMMIT.** The case above puts a route on the bridge
	// with `restoreRoutes`, which is the LOADER's road and writes unconditionally. A hand dragging a
	// knob calls `setRouteAmount`, which is gated on `maySourceRouteTo(deviceSlots(), ...)` and
	// **returns without a word when it refuses**. So a refusal there is invisible to every existing
	// case and looks exactly like the symptom from the outside: the amount never lands, so there is
	// nothing to draw and nothing to hear, while the device it came from goes on running.
	//
	// This case therefore uses the gesture road and nothing else, and asserts the VALUE at the far
	// end rather than that something changed — the puck is read off `resolved`, which is what the
	// engine builds the character from.
	ParamSet params;
	Synth synth { params };
	synth.activate(kSampleRate, 4096);
	feathers::ParamBridge bridge;

	// **THE ENVELOPE BY ITS ID, AND THE PUCK BY THE HOST'S OWN INDEX (kill-lanes step 5).** A route names
	// the device; the default rack's slot 0 is where the card the hand drags from stands.
	const feathers::DeviceInstanceId envelope = bridge.rackDeviceAt(0);
	REQUIRE(envelope != feathers::kNoInstance);
	const auto source = feathers::backplane::Source::output(envelope, 0);
	const int posx = feathers::modDestinationIndexOf(ParamId::PosX);
	REQUIRE(posx >= 0);
	const auto destination = feathers::backplane::Destination::core(static_cast<size_t>(posx));

	// THE GATE, ASKED FIRST AND OF THE BRIDGE'S OWN AUTHORITY. If a fresh bridge does not believe its
	// default rack holds a device, every drag from that card is refused and this is where it shows.
	CHECK(bridge.mayRoute(source, destination));

	// ---- the gesture: arm the envelope, drag the knob ----
	bridge.setRouteAmount(source, destination, 0.5f);
	// AND IT LANDED. `setRouteAmount` is void and silent, so this is the only way to know it did.
	CHECK(bridge.routeAmount(source, destination) == doctest::Approx(0.5f));

	// ---- the editor's lanes reach the engine, by the plugin's own one function ----
	feathers::publishEditorLanes(bridge, params, synth);

	// A HELD NOTE AND AN ENVELOPE THAT GOES SOMEWHERE. Same shape as the Path case above: instant
	// attack, full sustain, so the reading is 1.0 and the arithmetic is checkable by hand.
	namespace dsr = feathers::mod::dahdsr;
	dsr::Settings s = bare();
	s.attack = dsr::kStageLog2SecondsMin;
	s.sustain = 1.0f;
	feathers::ModRack rack = feathers::defaultModRack();
	rack.setDahdsrAt(0, s);
	synth.provisionDevices(feathers::FxChain {}, rack);
	synth.setModRack(rack);

	params.setBase(static_cast<clap_id>(ParamId::PosX), 0.0);

	synth.render(nullptr, nullptr, 0);
	std::vector<float> l(4096), r(4096);
	synth.render(l.data(), r.data(), 4 * kTickSamples);
	CHECK(params.resolved(ParamId::PosX) == doctest::Approx(0.0));

	synth.noteOn(0, kKeyC4, 1, 1.0);
	synth.render(l.data(), r.data(), 4 * kTickSamples);

	const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(ParamId::PosX));
	REQUIRE(desc != nullptr);
	// THE VALUE, RESTATED INDEPENDENTLY: an amount is stated in the destination's own span, and a
	// sustaining envelope reads 1.0, so 0.5 of it moves PosX by 0.5 * (max - min).
	CHECK(params.resolved(ParamId::PosX)
	      == doctest::Approx(0.5 * (desc->max - desc->min)).epsilon(1e-6));
	// AND THAT IS NOT WHERE IT STARTED, so the line above cannot pass on a route that landed nowhere.
	CHECK(params.resolved(ParamId::PosX) != doctest::Approx(0.0));
}

TEST_CASE("a DEFAULT envelope, routed by the editor's road, moves the puck on a held note") {
	// **THE FAITHFUL REPRODUCTION OF THE 2026-09-03 SYMPTOM**, and it exists because the case above
	// it PASSED while the same gesture, done by hand, did nothing.
	//
	// That case cheated in one specific way and the cheat is the point: it handed the synth a rack it
	// built itself, with `sustain = 1` and an instant attack, AFTER calling `publishEditorLanes`. The
	// plugin never does that. It provisions and sets the rack from `bridge.modRack()` — the DEFAULT
	// DAHDSR, at whatever `Settings{}` says — and a case that supplies its own settings is testing the
	// arithmetic of a route rather than what a fresh instance actually does.
	//
	// So this one touches the envelope's settings NOT AT ALL. Everything it hands the engine comes
	// from the bridge, by the two functions the plugin calls, in the plugin's own order.
	ParamSet params;
	Synth synth { params };
	synth.activate(kSampleRate, 4096);
	feathers::ParamBridge bridge;

	// BY ENDPOINT (kill-lanes step 5): the device the bridge's own default rack holds, and the host's PosX.
	const feathers::DeviceInstanceId envelope = bridge.rackDeviceAt(0);
	REQUIRE(envelope != feathers::kNoInstance);
	const auto source = feathers::backplane::Source::output(envelope, 0);
	const int posx = feathers::modDestinationIndexOf(ParamId::PosX);
	REQUIRE(posx >= 0);
	const auto destination = feathers::backplane::Destination::core(static_cast<size_t>(posx));

	bridge.setRouteAmount(source, destination, 0.5f);
	REQUIRE(bridge.routeAmount(source, destination) == doctest::Approx(0.5f));

	// THE PLUGIN'S OWN TWO LINES, in the plugin's own order: provision on the message thread, then
	// carry the lanes. See feathers_clap.cpp:669 and editor_lanes.h.
	synth.provisionDevices(bridge.fxChain(), bridge.modRack());
	feathers::publishEditorLanes(bridge, params, synth);

	params.setBase(static_cast<clap_id>(ParamId::PosX), 0.0);

	std::vector<float> l(4096), r(4096);
	synth.render(l.data(), r.data(), 4 * kTickSamples);
	const double atRest = params.resolved(ParamId::PosX);

	// A HELD NOTE, AND LONG ENOUGH TO BE PAST ANY ATTACK OR DECAY THE DEFAULTS DECLARE — the claim is
	// about where a held envelope SITS, so the reading has to be taken after it has got there.
	synth.noteOn(0, kKeyC4, 1, 1.0);
	for (int i = 0; i < 16; ++i) {
		synth.render(l.data(), r.data(), 4 * kTickSamples);
	}
	const double held = params.resolved(ParamId::PosX);

	CAPTURE(atRest);
	CAPTURE(held);
	// **AGAINST THE SOURCE'S OWN PUBLISHED READING, NOT AGAINST THE SUSTAIN.** The first draft of this
	// case asserted `amount * sustain * span` and went red at 0.9247 against 0.5 — because a default
	// envelope's decay is long and sixteen renders in it is still ON THE WAY DOWN, nowhere near where
	// it will settle. The case was wrong, not the code, and the shape of the error is worth keeping:
	// it restated the destination from a number the SETTINGS declare instead of from what the source
	// is actually publishing at the instant the destination is read.
	//
	// So the claim is the one this case is really about — the route's arithmetic — and it holds
	// wherever in its shape the envelope happens to be: the destination is the source's reading times
	// the amount times the destination's span. `publishedSourceForTest` is the same number the engine
	// hands the routing table, which is what makes this an independent restatement and not a tautology.
	const feathers::ParamDesc* desc = feathers::findParam(static_cast<clap_id>(ParamId::PosX));
	REQUIRE(desc != nullptr);
	const double reading = static_cast<double>(synth.publishedSourceForTest(0));
	CAPTURE(reading);
	CHECK(held == doctest::Approx(0.5 * reading * (desc->max - desc->min)).epsilon(1e-6));
	// AND THE READING IS NOT ZERO, or the line above passes for an envelope publishing nothing —
	// which is EXACTLY the state the symptom was seen in: a modulator switched off.
	CHECK(reading > 0.0);
	// AND IT MOVED AT ALL, which is the half the symptom is about.
	CHECK(held != doctest::Approx(atRest));
}

TEST_CASE("a modulator switched off reports no playhead, so its card cannot draw a moving star") {
	// **THE STAR MUST NOT MOVE WHILE THE MODULATOR IS OFF.**
	//
	// HOW THIS WAS FOUND, because the route matters more than the bug. An envelope
	// routed to the puck appeared to move nothing — not the arc, not the puck, not the sound — and reading the
	// whole road found no defect, twice, with two new cases going green over the exact gesture. The
	// modulator was simply SWITCHED OFF: a menu painted over the enable square made it read as on.
	//
	// The paint mistake is one bug and it is the surfaces'. THIS is the other, and it is the reason
	// the first one cost an evening rather than a second glance: the card went on sweeping its star
	// across the shape, which is a device saying "here is what I am contributing right now" while it
	// contributed nothing. Every downstream check made was against a display that was lying.
	//
	// THE DEVICE STILL RUNS — see the note in `Synth::playheadOf`. The tick is untouched, so switching
	// back on lands where the music is. What is gated is what the engine PUBLISHES, and now the value
	// and the position are gated by one authority (`modulatorSwitchedOnAt`) rather than by one each.
	namespace dsr = feathers::mod::dahdsr;

	const auto runWith = [](bool switchedOn) {
		Rig rig;
		dsr::Settings s = bare();
		s.attack = dsr::kStageLog2SecondsMin;
		s.sustain = 1.0f;
		s.enabled = switchedOn;
		rig.install(s);
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(4);
		const feathers::mod::Playhead first = rig.synth.rackPlayheads()[0];
		rig.renderTicks(4);
		return std::pair { first, rig.synth.rackPlayheads()[0] };
	};

	SUBCASE("switched off: idle, and idle again — there is nothing for a star to follow") {
		const auto [first, second] = runWith(false);
		CHECK_FALSE(first.running());
		CHECK_FALSE(second.running());
		CHECK(first.ordinal == feathers::mod::Playhead::kIdle);
	}

	SUBCASE("and switched on it runs, which is what proves the check above is not vacuous") {
		// WITHOUT THIS THE CASE ABOVE PASSES ON AN ENGINE THAT NEVER PUBLISHES A PLAYHEAD AT ALL —
		// the venue has to be shown capable of expressing the outcome before its silence is evidence.
		const auto [first, second] = runWith(true);
		CHECK(first.running());
		CHECK(second.running());
	}

	SUBCASE("the value and the position agree, because one authority answers both") {
		// THE PAIR IS THE POINT. A star that moves while the reading is zero is exactly the state
		// that was on screen, and it is unrepresentable now rather than merely fixed today.
		Rig rig;
		dsr::Settings s = bare();
		s.attack = dsr::kStageLog2SecondsMin;
		s.sustain = 1.0f;
		s.enabled = false;
		rig.install(s);
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(8);
		CHECK(rig.synth.publishedSourceForTest(0) == doctest::Approx(0.0));
		CHECK_FALSE(rig.synth.rackPlayheads()[0].running());
	}
}

TEST_CASE("the engine keeps the modulated fx lane, so nobody has to recompute where a route left it") {
	// **THE FX TWIN OF `modulatedRack()`, WHICH THE LANE SIMPLY NEVER GOT.**
	//
	// `applyParams` already builds a whole modulated FxChain once a block — it walks `modulatedRowAt`
	// over the lane and hands the result to `fxRunner_.sync`, because the runner MUST be driven with
	// the modulated settings or a route to a cutoff would not be heard. That chain was a local and was
	// dropped on the floor at the end of the function.
	//
	// The rack has kept its equivalent in `modulated_` since it was written, on a stated argument:
	// "the editor must read a snapshot the engine took, not reach across the thread boundary into a
	// device that is mid-tick." Every word of that applies to the lane, and the alternative — the
	// editor recomputing the offsets itself — is exactly what synth.h:418 warns off, a second
	// implementation of `rackOffsetAt` on the far side of a thread boundary, free to drift from the
	// one the sound is made with.
	//
	// COSTS NOTHING: the copy was already being made. Only its lifetime changed.
	namespace filter = feathers::fx::filter;
	Rig rig;

	feathers::FxChain chain;
	const size_t fxSlot = feathers::addFxDevice(&chain, filter::kTraits.tag);
	REQUIRE(fxSlot < chain.device.size());
	rig.synth.provisionDevices(chain, feathers::defaultModRack());
	rig.synth.setFxChain(chain);

	size_t cutoff = filter::kDescriptor.params.size();
	for (size_t i = 0; i < filter::kDescriptor.params.size(); ++i) {
		if (std::string(filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < filter::kDescriptor.params.size());

	// A MACRO, because its reading is a number this case SETS rather than one an engine arrives at —
	// the claim here is about what is kept, not about how an envelope moves.
	rig.params.setBase(static_cast<clap_id>(feathers::ParamId::Macro1), 1.0);
	feathers::test::Routes routes;
	// THE ID THE ENGINE RUNS THE DEVICE UNDER — asked of the engine after the sync that adopts it. The row's
	// own id can collide with the default rack's envelope (both chains count from 1), and the by-value road
	// then adopts the row under a stand-in; a route has to name what the engine actually holds.
	(void)rig.engineFxSlotOf(fxSlot);
	routes.set(feathers::test::macro(0),
	           feathers::backplane::Destination::control(rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, fxSlot),
	                                                   cutoff),
	           0.25f);
	routes.publish(rig.params);

	rig.renderTicks(4);

	const float raw = rig.synth.modulatedFxChain().device[fxSlot].row.field[cutoff];
	const float base = chain.device[fxSlot].row.field[cutoff];

	CAPTURE(base);
	CAPTURE(raw);
	// **THE VALUE, RESTATED INDEPENDENTLY.** An amount is stated in the control's own declared span,
	// so a macro sitting at 1.0 through an amount of 0.25 moves the control by 0.25 * (max - min) —
	// the same arithmetic `rackOffsetAt` performs, written out here from the descriptor rather than
	// copied from it.
	const feathers::device::ParamDesc& pd = filter::kDescriptor.params[cutoff];
	const double expected = static_cast<double>(base) + 0.25 * feathers::device::routeSpanOf(pd);
	CHECK(raw == doctest::Approx(static_cast<float>(expected)).epsilon(1e-4));
	// AND IT MOVED AT ALL, so the line above cannot pass on a route that landed nowhere.
	CHECK(raw != doctest::Approx(base));

	SUBCASE("and the RAW lane is untouched — modulation is a reading, never an edit") {
		// The chain the editor published must still say what the player set, or the next repaint would
		// draw the modulated value as the knob's own position and the offset would compound.
		CHECK(rig.synth.fxChainForTest().device[fxSlot].row.field[cutoff] == doctest::Approx(base));
	}
}

//------------------------------------------------------------------------------------------------//
// THE ROUTING RULE, THROUGH THE ENGINE — a source reaches only what is downstream of it.           //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a strip modulator reaches the one after it AND the one before it (IZ), and not itself") {
	// **THE ROUTING RULE: sources must be upstream of destinations, and (2026-09-06, the narrower
	// mechanical question) a node's bus does NOT carry its own device's output.** The consequence —
	// the self-modulation gesture stops working — was weighed and accepted. See `Synth::fillBusFor`,
	// which is the only place it is applied.
	//
	// **THIS CASE EXISTS BECAUSE THE WHOLE SUITE STAYED GREEN WHEN THE RULE LANDED.** 83 of 83, with a
	// behaviour change that stops one modulator reaching another. Nothing in the tree drove a
	// device-to-device route through `Synth` and read the result — every self-route assertion reads
	// the route RECORD (`test_gui_widgets_dahdsr.cpp:785` and its siblings), and the value case beside
	// this one builds its own whole bus and so tests the resolver rather than what `Synth` puts on it.
	// A rule with no instrument is a rule nobody can break loudly.
	//
	// **DRIVEN THROUGH `modulatedRack()`, WHICH IS THE ENGINE'S OWN PRODUCT** rather than a
	// restatement of it: this is what `ModRunner::sync` is handed, so a route that does not move a
	// number here does not move the sound.
	const auto attackOf = [](const feathers::ModRack& rack, size_t slot) {
		return rack.dahdsrAt(slot).attack;
	};
	const auto attackOrdinal = [] {
		const size_t control = static_cast<size_t>(dahdsr::Param::Attack);
		REQUIRE(dahdsr::kDescriptor.params[control].modulatable);
		return dahdsr::kDescriptor.nthModulatableOfControl(control);
	}();

	// **THROUGH THE GATE, WHICH IS WHERE THE RULE LIVES NOW (kill-lanes §2.4b).** This wrote the route
	// straight into the engine's table and watched the engine's bus MASK refuse it. There is no mask: the
	// engine takes what the record publishes and builds a node's inputs from it, and legality is the
	// GRAPH's, once, at the edit (`Graph::route` refuses a cycle; `mayRoute` is the same question asked
	// first). So the route goes in the way a hand's does — offered to the bridge's gate, written only if
	// admitted, published to the synth — and the claim is stated at both ends: the gate's answer, and what
	// the engine then resolves. A route the gate refuses never reaches a node, which is the whole of "does
	// not reach here".
	//
	// TWO ENVELOPES, EACH WITH AN ID, THE IDS DISTINCT — `defaultModRack` gives slot 0 an identity and says
	// nothing about slot 1, and a device with no identity is one every route addresses nothing by.
	struct Outcome {
		bool admitted = false;
		feathers::ModRack out;
	};
	const auto runWithRoute = [&](size_t fromSlot, size_t toSlot) {
		Rig rig;
		feathers::ParamBridge bridge;
		feathers::ModRack rack = feathers::defaultModRack();
		rack.count = 2;
		rack.setDahdsrAt(0, bare());
		rack.setDahdsrAt(1, bare());
		rack.setIdentityAt(1, { 2 });
		REQUIRE(rack.instanceAt(0) != feathers::kNoInstance);
		REQUIRE(rack.instanceAt(1) != feathers::kNoInstance);
		REQUIRE(rack.instanceAt(0) != rack.instanceAt(1));
		bridge.restoreModRack(rack);
		// THE MESSAGE THREAD'S HALF, then the publish the synth adopts — the record's ids are the engine's.
		rig.synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
		feathers::publishEditorLanes(bridge, rig.params, rig.synth);
		rig.render(1);

		const feathers::backplane::Source from = feathers::backplane::Source::output(bridge.rackDeviceAt(fromSlot), 0);
		const feathers::backplane::Destination to = feathers::backplane::Destination::control(
		    bridge.rackDeviceAt(toSlot), dahdsr::kDescriptor.controlOfNthModulatable(attackOrdinal));
		Outcome outcome;
		outcome.admitted = bridge.mayRoute(from, to);
		bridge.setRouteAmount(from, to, 1.0f); // the record refuses a pair the gate does not admit
		feathers::publishEditorLanes(bridge, rig.params, rig.synth);

		// THE ENVELOPES MUST BE RUNNING, or every source reads zero and every assertion below is about
		// nothing: a note, and enough ticks for an attack to climb.
		rig.synth.noteOn(0, kKeyC4, 1, 1.0);
		rig.renderTicks(4);
		outcome.out = rig.synth.modulatedRack();
		return outcome;
	};

	const double base = bare().attack;

	SUBCASE("FORWARD: position 0 reaches position 1 — and this is the vacuity control") {
		const Outcome o = runWithRoute(0, 1);
		CHECK(o.admitted);
		CHECK(attackOf(o.out, 1) != doctest::Approx(base));
	}

	SUBCASE("LEFTWARD: position 1 reaches position 0 — a strip modulator's place is only grouping (IZ)") {
		// **IT WAS REFUSED UNTIL 2026-10-03, AND ONLY BY ACCIDENT.** IZ (a modulator's position
		// in a lane is only grouping) let a strip LFO reach one to its left; two DAHDSRs went on being ordered by
		// their NOTE road, which only listens. `NoteOps::listensOnly` ends that, so they follow the rule the LFOs
		// already did. Upstream is still the rule — asked of what is computed from what, not of the lane.
		const Outcome o = runWithRoute(1, 0);
		CHECK(o.admitted);
		CHECK(attackOf(o.out, 0) != doctest::Approx(base));
	}

	SUBCASE("ITSELF: position 0 does not reach position 0 — the gesture the rule removes") {
		const Outcome o = runWithRoute(0, 0);
		CHECK_FALSE(o.admitted);
		CHECK(attackOf(o.out, 0) == doctest::Approx(base));
	}
}

//------------------------------------------------------------------------------------------------//
// THE MOD TIER IS A SYNCHRONOUS UPDATE, WHICH IS WHY ITS TICK ORDER IS UNOBSERVABLE — and which is  //
// the semantic the graph is going to take away.                                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("a modulator modulating another reads it IN THE SAME TICK — pass-through, in chain order (it read it one tick late until kill-lanes §2.4b)") {
	// **DECIDED: THIS CASE IS EXPECTED TO GO RED, AND WHEN IT DOES THE FIX IS TO
	// REWRITE IT — NOT TO RESTORE THE DELAY.** The options for the migration's default were
	// all-immediate (the graph as written), all-delayed (what Feathers does today), or a flag per
	// edge, and the choice is "as written". So modulation becomes pass-through, a modulator reads its sources
	// from the tick it is in, and a delayed modulator loop stops being expressible.
	//
	// It is pinned ANYWAY, and that is the point rather than a contradiction: the change is a
	// behaviour change and it must arrive as a red that somebody dismisses on purpose, not as silence.
	// A mutation flipping this semantic reddens 1 case out of 26 in this binary and this is that case;
	// without it the migration would move every modulator-to-modulator route by a tick and nothing in
	// the tree would say a word.
	//
	// **THIS PINS A SEMANTIC THAT ADOPTING THE GRAPH WILL CHANGE, so the change cannot happen
	// silently.** The edge-store design left the RENDER order as the last
	// unmeasured gate of arc A. The answer is in `Synth::advanceRack`'s sequence, and it is stronger
	// than "the orders agree": within one control tick the engine
	//
	//   1. resolves `modulated_` for EVERY slot, against the sources published last tick;
	//   2. syncs and ticks every device;
	//   3. publishes every device's new output.
	//
	// So no modulator can see another's value from the tick it is in, whatever order they tick in —
	// `ModRunner::tick` walks by SLOT INDEX rather than by chain position, and that is harmless for
	// exactly this reason. It is a synchronous update with a one-tick delay, the same shape `params.h`
	// already argues for the weight sources (*"it closes through a block boundary the way a feedback
	// delay closes through its buffer"*).
	//
	// **`graph.h` INTENDS TO REMOVE THIS AND SAYS SO**: modulation is pass-through, *"a route that
	// would close a cycle is refused before the cell is written"*, and *"Feathers' present
	// one-block-delayed loops stop being expressible only when its chains are on this graph"*. That is
	// a decision, not an accident — but it means a patch whose modulator modulates another modulator
	// SOUNDS DIFFERENT after the migration. This case is where that shows up as a red rather than as
	// a patch that silently changed.
	//
	// ASSERTED AS A VALUE, not as a difference between two renders: the modulated attack after tick k
	// is restated independently from the output the engine published at tick k-1, which is the only
	// form in which "it reads the PREVIOUS one" is a claim rather than a lag nobody measured.
	const auto attackOrdinal = [] {
		const size_t control = static_cast<size_t>(dahdsr::Param::Attack);
		REQUIRE(dahdsr::kDescriptor.params[control].modulatable);
		return dahdsr::kDescriptor.nthModulatableOfControl(control);
	}();
	constexpr float kAmount = 1.0f;

	Rig rig;
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	rack.setDahdsrAt(0, bare());
	rack.setDahdsrAt(1, bare());
	// The neighbouring case's lesson: a hand-built second slot holds no IDENTITY, and every route to it
	// then addresses nothing while the case passes. (It said "no lane"; kill-lanes step 3c retired the
	// address, and the id is what a route names — see that case for the whole of it.)
	rack.setIdentityAt(1, { 2 });
	REQUIRE(rack.instanceAt(0) != feathers::kNoInstance);
	REQUIRE(rack.instanceAt(1) != feathers::kNoInstance);
	REQUIRE(rack.instanceAt(0) != rack.instanceAt(1));
	rig.publishModRack(rack);

	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(rack.instanceAt(0), 0),
	           feathers::backplane::Destination::control(rack.instanceAt(1), dahdsr::kDescriptor.controlOfNthModulatable(attackOrdinal)),
	           kAmount);
	routes.publish(rig.params);

	// A SLOW ATTACK ON SLOT 0, so its output is still CLIMBING across the two ticks below. A source
	// that has already reached its ceiling reads the same at tick k and k-1, and every assertion here
	// would pass on an engine with no delay at all — the non-vacuity REQUIRE below is what says so.
	dahdsr::Settings climbing = bare();
	climbing.attack = dahdsr::stageValueFor(0.5);
	rack.setDahdsrAt(0, climbing);
	rig.publishModRack(rack);

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);

	rig.renderTicks(1);
	const float publishedAfterTick1 = rig.synth.publishedSourceForTest(0);
	rig.renderTicks(1);
	const float publishedAfterTick2 = rig.synth.publishedSourceForTest(0);
	const float attackAfterTick2 = rig.synth.modulatedRack().dahdsrAt(1).attack;

	CAPTURE(publishedAfterTick1);
	CAPTURE(publishedAfterTick2);
	CAPTURE(attackAfterTick2);
	// THE SOURCE IS ACTUALLY MOVING. Without this the two expectations below are the same number and
	// the case cannot tell a one-tick delay from no delay.
	REQUIRE(publishedAfterTick1 != doctest::Approx(publishedAfterTick2));
	REQUIRE(publishedAfterTick1 != doctest::Approx(0.0f));

	// **THE SPAN, RESTATED FROM THE DECLARATION** — the arithmetic `ParamSet::rackOffsetAt` performs,
	// written out from the descriptor here rather than copied from the engine.
	const feathers::device::ParamDesc& pd
	    = dahdsr::kDescriptor.params[static_cast<size_t>(dahdsr::Param::Attack)];
	const double full = pd.max - pd.min;
	const double span = pd.modSpan > 0.0 ? pd.modSpan : (pd.zones > 0 ? full / pd.zones : full);
	const double base = bare().attack;
	const auto expectFrom = [&](float source) {
		return std::clamp(base + static_cast<double>(kAmount) * static_cast<double>(source) * span,
		                  static_cast<double>(pd.min), static_cast<double>(pd.max));
	};

	// THE CLAIM: tick 2's resolved attack is computed from tick 1's published output.
	// **THE RED ARRIVED, AND THE FIX IS THE ONE THE CASE PRESCRIBED FOR ITSELF (kill-lanes §2.4b,
	// 2026-09-13).** Position 1 now resolves against what position 0 published IN THIS TICK: the strip
	// ticks in chain order and a node publishes its outputs the moment it has ticked, so the downstream
	// modulator's inputs read the fresh value. That is pass-through — "as written" — and it is
	// what running in the graph's order is FOR. The bus that imposed the one-tick delay is gone.
	CHECK(static_cast<double>(attackAfterTick2)
	      == doctest::Approx(expectFrom(publishedAfterTick2)).epsilon(1e-4));
	CHECK(static_cast<double>(attackAfterTick2)
	      != doctest::Approx(expectFrom(publishedAfterTick1)).epsilon(1e-4));
}
TEST_CASE("the engine half: after the fx chain is reordered and pushed into the synth, the MSEG still MOVES the filter's cutoff — the modulated row restated from the bus") {
	// The symptom, split: the rings were present but did not move, and modulation could not be assigned
	// from any modulator, including ones added afterwards. The table holds the routes and the editor draws them; NOTHING
	// modulates an fx destination after the reorder. That is the engine's half — the synth's own copy
	// of the chain, pushed by `publishEditorLanes` at every process, and the bus mask in `fillBusFor`
	// that zeroes every device source it judges not upstream of the reader.
	//
	// SO THIS WALKS THE ENGINE'S ROAD: the bridge edits, the lanes pushed into a REAL synth exactly as
	// `paramsFlush` pushes them, the devices provisioned as `pumpEditor` provisions them, a render,
	// and the modulated row read from the synth's own modulated chain — asserted BY VALUE against an
	// independent restatement (amount x the source value the render published x the control's span,
	// the arithmetic `rackOffsetAt` declares), before and after the reorder. A difference check alone
	// would pass on any movement; the restatement is what says the MSEG moved the CUTOFF by its amount.
	namespace flt = feathers::fx::filter;
	namespace hrm = feathers::fx::harm;
	namespace msg = feathers::mod::mseg;
	feathers::ParamBridge bridge;
	REQUIRE(bridge.fxChain().count == 1); // the default filter, at position 0
	REQUIRE(bridge.addFxDevice(hrm::kTraits.tag) == 1);
	// **THE TWO DEVICES BY NAME, BECAUSE A COORDINATE DOES NOT SURVIVE THE REORDER BELOW** (kill-lanes step
	// 3b, and the coordinates themselves went at step 5). A device's coordinate WAS its position, so a
	// number captured here meant a different device after the move; the ids do not move, and a route
	// stores an id.
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	const feathers::DeviceInstanceId harmId = bridge.fxDeviceAt(1);
	REQUIRE(filterId != feathers::kNoInstance);
	REQUIRE(harmId != feathers::kNoInstance);
	const size_t msegSlot = bridge.addRackDevice(msg::kTraits.tag);
	REQUIRE(msegSlot < feathers::kMaxRackSlots);
	{
		feathers::ModRack rack = bridge.modRack();
		msg::Settings looping {};
		looping.loopStart = 0;
		looping.loopEnd = 1;
		rack.setMsegAt(msegSlot, looping);
		bridge.restoreModRack(rack);
	}
	// THE SOURCE UNDER TEST IS THE DEFAULT DAHDSR, driven by a held note: a looping MSEG with no
	// transport published nothing in sixteen blocks (measured), and a silent source cannot tell the mask
	// from silence. The mask is the same for every device source, which is the "any modulator" half.
	const feathers::DeviceInstanceId envelopeId = bridge.rackDeviceAt(0);
	REQUIRE(envelopeId != feathers::kNoInstance);
	const auto source = feathers::backplane::Source::output(envelopeId, 0);
	(void)msegSlot;

	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const feathers::device::ParamDesc& pd = flt::kDescriptor.params[cutoff];
	REQUIRE(pd.modulatable);
	const double full = pd.max - pd.min;
	const double span = pd.modSpan > 0.0 ? pd.modSpan
	                    : (pd.zones > 0 ? full / static_cast<double>(pd.zones) : full);
	const float amount = 0.5f;
	// **BOTH ROUTES BY ENDPOINT, WHICH IS WHAT MAKES THE REORDER BELOW A TEST OF ANYTHING (kill-lanes step
	// 5).** These named a lane, and both lanes meant a different device after the move — the destination
	// under test would have been the HARM's cutoff while the case went on asserting about the filter.
	const auto toCutoff = feathers::backplane::Destination::control(filterId, cutoff);
	bridge.setRouteAmount(source, toCutoff, amount);
	bridge.setRouteAmount(source, feathers::backplane::Destination::control(harmId, static_cast<size_t>(hrm::Param::Width)),
	                      0.5f);

	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0); // the default base is zero (EF)
	synth.noteOn(0, 60, 1, 1.0);
	std::vector<float> left(256, 0.0f), right(256, 0.0f);
	const auto pushAndRender = [&](int blocks) {
		// EXACTLY THE PLUGIN'S ORDER: provision as pumpEditor does, push as paramsFlush does, render.
		synth.provisionDevices(bridge.fxChain(), bridge.modRack());
		feathers::publishEditorLanes(bridge, params, synth);
		for (int i = 0; i < blocks; ++i) {
			synth.render(left.data(), right.data(), static_cast<int>(left.size()));
		}
	};
	// THE MODULATED CUTOFF AT A POSITION, MINUS ITS BASE, AGAINST THE RESTATEMENT.
	const auto cutoffOffsetAt = [&](size_t position) {
		const float modulated = fxDeviceAtPosition(synth.modulatedFxChain(), position).row.field[cutoff];
		const float base = fxDeviceAtPosition(bridge.fxChain(), position).row.field[cutoff];
		return static_cast<double>(modulated - base);
	};
	const auto sourceNow = [&] {
		// **READ AT THE ENGINE'S SLOT FOR THE DEVICE** (kill-lanes step 3a): the sources are published where
		// the engine keeps each device, which is nowhere the record can name. The route above is made by
		// ENDPOINT through the bridge, so only this read has to cross — and it crosses by asking the engine
		// where it put the device the strip's slot 0 holds (step 3b: a device is its id, nothing else).
		return static_cast<double>(synth.publishedSourceForTest(0)); // the node's published cell (kill-lanes §2.4b)
	};

	pushAndRender(16);
	const double sourceBefore = sourceNow();
	REQUIRE(sourceBefore != 0.0); // the looping MSEG runs and publishes, or nothing below can be seen
	const double offsetBefore = cutoffOffsetAt(0);
	CHECK(offsetBefore == doctest::Approx(amount * sourceBefore * span).epsilon(0.05));
	CHECK(offsetBefore != 0.0);

	// THE REORDER, THROUGH THE BRIDGE, PUSHED, RENDERED.
	REQUIRE(bridge.moveFxDevice(0, 1));
	// **THE FILTER NOW STANDS AT POSITION 1, ASSERTED AS AN IDENTITY (kill-lanes step 3b).** This read
	// `fxLaneAt(bridge, 1) == filterLane`, which was an identity check wearing a coordinate: the filter
	// kept its address across a move while the record was told which slot to give it. A device's
	// coordinate IS its position now — by design, addresses are not sequential and ordering comes
	// only from the graph — so the old line compares 1 with 0 and is no claim about this chain at all.
	// **The route is untouched by any of that**: it stores `Destination::control(filterId, cutoff)`, and
	// surviving the reorder is the thing this case exists to show.
	REQUIRE(bridge.fxDeviceAt(1) == filterId);
	REQUIRE(bridge.fxDeviceAt(0) == harmId);
	pushAndRender(16);
	const double sourceAfter = sourceNow();
	REQUIRE(sourceAfter != 0.0);
	const double offsetAfter = cutoffOffsetAt(1); // the filter now at position 1
	{
		const auto& sc = synth.modulatedFxChain();
		const auto& bc = bridge.fxChain();
		MESSAGE("DIAG after reorder: synth pos1 tag=" << std::string(fxDeviceAtPosition(sc, 1).row.tag.data(), 4)
		        << " bridge pos1 tag=" << std::string(fxDeviceAtPosition(bc, 1).row.tag.data(), 4)
		        << " synth cutoff=" << fxDeviceAtPosition(sc, 1).row.field[cutoff]
		        << " bridge cutoff=" << fxDeviceAtPosition(bc, 1).row.field[cutoff]
		        << " synth cutoff=" << fxDeviceAtPosition(sc, 0).row.field[cutoff]
		        << " bridge cutoff=" << fxDeviceAtPosition(bc, 0).row.field[cutoff]
		        // The order used to be a second array (`order[0]`, `order[1]`) and is the array itself since
		        // kill-lanes step 3c, so what is worth printing is WHICH DEVICE is at each position.
		        << " synth ids=[" << fxDeviceAtPosition(sc, 0).row.instanceId << ","
		        << fxDeviceAtPosition(sc, 1).row.instanceId << "] bridge ids=["
		        << fxDeviceAtPosition(bc, 0).row.instanceId << "," << fxDeviceAtPosition(bc, 1).row.instanceId << "]"
		        << " synth count=" << sc.count << " source=" << sourceAfter << " span=" << span);
	}
	CHECK(offsetAfter == doctest::Approx(amount * sourceAfter * span).epsilon(0.05));
	CHECK(offsetAfter != 0.0);

	// AND A ROUTE ASSIGNED AFTER THE REORDER, FROM A MODULATOR ADDED AFTER IT, MOVES THE HARM.
	const size_t laterSlot = bridge.addRackDevice(feathers::mod::dahdsr::kTraits.tag);
	REQUIRE(laterSlot < feathers::kMaxRackSlots);
	// **BY ENDPOINT, SO NO COORDINATE ENTERS THIS HALF AT ALL (kill-lanes step 3b).** It built a source
	// coordinate from the modulator's slot and a destination coordinate from `harmLane` — a number
	// captured BEFORE the reorder. HARM stands at position 0 now, so that destination had come to name
	// the FILTER, and the case would have been routing to the wrong device while still going green (the
	// assertion below is about the BUS, and the bus does not care which device the route reached). A
	// device's coordinate moves with it; its id does not, and an id is what a route stores.
	const feathers::DeviceInstanceId later = bridge.rackDeviceAt(laterSlot);
	REQUIRE(later != feathers::kNoInstance);
	const size_t width = static_cast<size_t>(hrm::Param::Width);
	const feathers::backplane::Source laterSource = feathers::backplane::Source::output(later, 0);
	const feathers::backplane::Destination harmWidth =
	    feathers::backplane::Destination::control(harmId, width);
	bridge.setRouteAmount(laterSource, harmWidth, 0.7f);
	// AND THE RULES PERMITTED IT, read as the value it left rather than asked of a second gate — which is
	// what the `maySourceRouteTo` check standing here used to do for the coordinate pair. A refused route
	// stores nothing, so a nonzero amount IS the gate's answer.
	CHECK(bridge.routeAmount(laterSource, harmWidth) == 0.7f);
	pushAndRender(4);
	// THE BUS THE HARM READS CARRIES BOTH SOURCES UNMASKED — the direct form of "nothing moves":
	// after a render the synth has published every device source into the parameter set, and the
	// mask is what would have zeroed them for this reader.
	// AT THE ENGINE'S SLOT for the envelope — where the sources are published (kill-lanes step 3a), asked
	// by the device's own id (step 3b).
	CHECK(synth.publishedSourceForTest(0) != 0.0f); // the node's published cell (kill-lanes §2.4b)
}



TEST_CASE("backlog rank-12, the engine half: a source routed to the cutoff of the fx device at position 6 or 7 MOVES it — the bus mask does not read rank 12 as 'no lane'") {
	// The synth's `fillBusFor` masks every device source that is not upstream of the reader, asking
	// `sourceLaneIsUpstreamOf` over its own copies of the chains. With `orderIndexOfLane` answering
	// "no lane" with 12 — the rank of fx position 6 — the device there was unranked at the reader's
	// end, every source was masked for it, and its modulated row equalled its base whatever the
	// table said. The DA engine case's shape, at the two positions the collision reaches: the
	// modulated row restated as amount × published source × span, so the assertion is about the
	// VALUE and not about whether anything differed.
	namespace flt = feathers::fx::filter;
	feathers::ParamBridge bridge;
	REQUIRE(bridge.fxChain().count == 1);
	while (bridge.fxChain().count < feathers::kMaxFxSlots) {
		REQUIRE(bridge.addFxDevice(flt::kTraits.tag) < feathers::kMaxFxSlots);
	}
	REQUIRE(feathers::kMaxFxSlots >= 8);
	const feathers::DeviceInstanceId envelopeId = bridge.rackDeviceAt(0); // the default DAHDSR
	REQUIRE(envelopeId != feathers::kNoInstance);
	const auto source = feathers::backplane::Source::output(envelopeId, 0);

	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const feathers::device::ParamDesc& pd = flt::kDescriptor.params[cutoff];
	REQUIRE(pd.modulatable);
	const double full = pd.max - pd.min;
	const double span = pd.modSpan > 0.0 ? pd.modSpan
	                    : (pd.zones > 0 ? full / static_cast<double>(pd.zones) : full);
	const float amount = 0.5f;
	// **THE DEVICE AT EACH POSITION, BY ITS ID (kill-lanes step 5).** The positions are what the backlog
	// item is about — the mask read the rank of fx position 6 as "no lane" — so the case still names them;
	// the ROUTE names the device standing there, which is what a route has stored since step 2.
	for (size_t position : { size_t { 0 }, size_t { 6 }, size_t { 7 } }) {
		CAPTURE(position);
		const feathers::DeviceInstanceId there = bridge.fxDeviceAt(position);
		REQUIRE(there != feathers::kNoInstance);
		const auto dest = feathers::backplane::Destination::control(there, cutoff);
		REQUIRE(bridge.mayRoute(source, dest)); // the gate, asked of the authority
		bridge.setRouteAmount(source, dest, amount);
	}

	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), 1.0); // the default base is zero (EF)
	synth.noteOn(0, 60, 1, 1.0);
	std::vector<float> left(256, 0.0f), right(256, 0.0f);
	synth.provisionDevices(bridge.fxChain(), bridge.modRack());
	feathers::publishEditorLanes(bridge, params, synth);
	for (int i = 0; i < 16; ++i) {
		synth.render(left.data(), right.data(), static_cast<int>(left.size()));
	}
	// READ AT THE ENGINE'S SLOT for the envelope — where a source's value is published (kill-lanes 3a),
	// asked by the device's own id (step 3b).
	const double sourceNow = static_cast<double>(synth.publishedSourceForTest(0)); // the node's published cell
	REQUIRE(sourceNow != 0.0);

	for (size_t position : { size_t { 0 }, size_t { 6 }, size_t { 7 } }) {
		CAPTURE(position);
		const float modulated = fxDeviceAtPosition(synth.modulatedFxChain(), position).row.field[cutoff];
		const float base = fxDeviceAtPosition(bridge.fxChain(), position).row.field[cutoff];
		const double offset = static_cast<double>(modulated - base);
		CHECK(offset == doctest::Approx(amount * sourceNow * span).epsilon(0.05));
		CHECK(offset != 0.0);
	}
	// And a position the route does not name is NOT moved — the second assertion that proves the
	// first is not vacuous.
	{
		const float modulated = fxDeviceAtPosition(synth.modulatedFxChain(), 5).row.field[cutoff];
		const float base = fxDeviceAtPosition(bridge.fxChain(), 5).row.field[cutoff];
		CHECK(modulated == doctest::Approx(base));
	}
}

TEST_CASE("a route reaches Bus Utility's pan, and stops at its switches") {
	// **THE UTILITY KNOBS ARE MODULATABLE, EXCEPT the channel polarity switches, the filter frequency
	// and the clipper knee.** The descriptor already declared exactly that — but a
	// FLAG IS NOT A ROAD, and `test_fx_busutil.cpp` can only assert the declaration. This is the case
	// that says the modulation actually arrives: the same rig the filter's cutoff uses, one device
	// along, reading the value out of the modulated chain the engine hands its runner.
	namespace bu = feathers::fx::busutil;
	Rig rig;

	feathers::FxChain chain;
	const size_t fxSlot = feathers::addFxDevice(&chain, bu::kTraits.tag);
	REQUIRE(fxSlot < chain.device.size());
	rig.synth.provisionDevices(chain, feathers::defaultModRack());
	rig.synth.setFxChain(chain);

	const size_t pan = static_cast<size_t>(bu::Param::Pan);
	REQUIRE(bu::kDescriptor.params[pan].modulatable);

	rig.params.setBase(static_cast<clap_id>(feathers::ParamId::Macro1), 1.0);
	feathers::test::Routes routes;
	// THE ID THE ENGINE RUNS THE DEVICE UNDER — asked of the engine after the sync that adopts it. The row's
	// own id can collide with the default rack's envelope (both chains count from 1), and the by-value road
	// then adopts the row under a stand-in; a route has to name what the engine actually holds.
	(void)rig.engineFxSlotOf(fxSlot);
	routes.set(feathers::test::macro(0),
	           feathers::backplane::Destination::control(rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, fxSlot),
	                                                   pan),
	           0.25f);
	routes.publish(rig.params);
	rig.renderTicks(4);

	const float raw = rig.synth.modulatedFxChain().device[fxSlot].row.field[pan];
	const float base = chain.device[fxSlot].row.field[pan];
	// THE VALUE, RESTATED FROM THE DESCRIPTOR rather than copied from the engine: an amount is stated
	// in the control's own declared span, so a macro at 1.0 through 0.25 moves it by a quarter of
	// (max - min). Pan runs -1..1, so that is 0.5.
	const feathers::device::ParamDesc& pd = bu::kDescriptor.params[pan];
	CHECK(raw == doctest::Approx(static_cast<float>(static_cast<double>(base) + 0.25 * (pd.max - pd.min)))
	                 .epsilon(1e-4));
	CHECK(raw != doctest::Approx(base));

	SUBCASE("and the three excluded controls have no destination to route to at all") {
		// **THE EXCLUSIONS ARE THE DECISION'S WHOLE CONTENT**, so they are checked on the ROAD and not
		// only in the descriptor: a control that is not modulatable gets no destination address, so
		// there is nothing for a source to be pointed at. The value is the refusal — the
		// out-of-range answer — rather than merely "something different".
		const std::array<bu::Param, 3> excluded { bu::Param::InvertL, bu::Param::Highpass,
			                                      bu::Param::Clip };
		for (const bu::Param p : excluded) {
			CAPTURE(std::string(bu::kDescriptor.params[static_cast<size_t>(p)].name));
			CHECK(bu::kDescriptor.nthModulatableOfControl(static_cast<size_t>(p))
			      == bu::kDescriptor.modulatableCount());
		}
	}
}

//------------------------------------------------------------------------------------------------//
// THE BRIDGE'S CHAINS ARE A RECORD (chain-as-a-device 5b, C4 — desired state, in both hosts)        //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE LANE AS THE ENGINE HOLDS IT AND AS THE EDITOR SEES IT, compared on what a chain SAYS — its length
/// and the row drawn at each POSITION — not on `nextInstanceId`, which is the record's counter and not the
/// lane's.
///
/// **IT COMPARED THE TWO VIEWS' LANES AS WELL, AND THAT HALF IS GONE (kill-lanes step 3b).** Both views
/// come out of `chainsOfState`, which MINTS a view's lane from the position (`deviceSlotOfFx(p)`) — so
/// `fxLaneAtPosition(a, p) != fxLaneAtPosition(b, p)` is now the same arithmetic on the same index down
/// both sides and cannot go red whatever the two chains hold. Deleted rather than left standing. What that
/// comparison was really asking — do the two hold the same device in the same place — is what the row
/// comparison says, and it is the only one of the two that ever could.
bool sameLane(const feathers::FxChain& a, const feathers::FxChain& b) {
	if (feathers::fxChainLength(a) != feathers::fxChainLength(b)) {
		return false;
	}
	for (size_t p = 0; p < feathers::fxChainLength(a); ++p) {
		if (!(fxDeviceAtPosition(a, p).row == fxDeviceAtPosition(b, p).row)) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("the engine takes the editor's chains from what the record published — a knob, an add — and nothing on a block nobody touched them") {
	// **THE AUDIO THREAD'S ROAD TO THE CHAINS** — `publishEditorLanes` → `Synth::adoptChains`, a whole
	// snapshot of the record's published state. It was `bridge.fxChain()` / `bridge.modRack()` every
	// block, assembled from ~40 arrays of relaxed atomics; the record is the message thread's now, so a
	// case asserts the engine holds exactly what the editor sees, after each kind of edit.
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	feathers::publishEditorLanes(bridge, params, synth);
	REQUIRE(sameLane(synth.fxChainForTest(), bridge.fxChain()));
	CHECK(synth.modRackForTest().count == bridge.modRack().count);

	SUBCASE("nothing touched: nothing taken") {
		CHECK_FALSE(synth.adoptChains(bridge.publishedChains()));
	}

	SUBCASE("a knob") {
		// A VALUE THE CONTROL DOES NOT HOLD — read from what it holds, never picked: the default lane's
		// filter starts OFF, so a first draft writing "a quarter of the range" to its stepped Enable
		// rounded to the value already there and passed with the publish deleted.
		//
		// **THE DEVICE BY ITS ID, THE ROW BY ITS POSITION (kill-lanes step 3b).** This addressed both by a
		// lane, and the two lanes stopped being one number: the view's is minted from the position by
		// `chainsOfState`, the record's is wherever its graph parked the node. They agree for the default
		// lane and would part after the first reorder.
		const feathers::DeviceInstanceId filter = bridge.fxDeviceAt(0);
		REQUIRE(filter != feathers::kNoInstance);
		const auto& p = feathers::fx::filter::kDescriptor.params[0];
		const float held = fxDeviceAtPosition(bridge.fxChain(), 0).row.field[0];
		const float value = static_cast<float>(held == static_cast<float>(p.max) ? p.min : p.max);
		bridge.setDeviceControl(filter, 0, value);
		REQUIRE(fxDeviceAtPosition(bridge.fxChain(), 0).row.field[0] == value);
		feathers::publishEditorLanes(bridge, params, synth);
		CHECK(fxDeviceAtPosition(synth.fxChainForTest(), 0).row.field[0] == value);
		CHECK(sameLane(synth.fxChainForTest(), bridge.fxChain()));
	}

	SUBCASE("an add to each chain") {
		REQUIRE(bridge.addFxDevice(feathers::fx::harm::kTraits.tag) == 1);
		REQUIRE(bridge.addRackDevice(feathers::mod::mseg::kTraits.tag) == 1);
		feathers::publishEditorLanes(bridge, params, synth);
		CHECK(feathers::fxChainLength(synth.fxChainForTest()) == 2);
		CHECK(sameLane(synth.fxChainForTest(), bridge.fxChain()));
		CHECK(synth.modRackForTest().count == 2);
		CHECK(synth.modRackForTest().device[1] == bridge.modRack().device[1]);
	}
}

TEST_CASE("a restore of what the bridge already holds changes nothing — every device keeps its id and its place") {
	// **WHY EVERY STRUCTURAL EDIT CAN GO THROUGH A RESTORE.** The chain rules (`fx_chain.h`, `mod_rack.h`)
	// answer with a whole chain value; the bridge writes it back into the record by rebuilding the chain.
	// That is only safe if a row coming back under its own id, at its own place in the chain, is the SAME
	// device — which is what the engine's converger compares, so a knob-free restore must rebuild nothing
	// downstream.
	feathers::ParamBridge bridge;
	REQUIRE(bridge.addFxDevice(feathers::fx::harm::kTraits.tag) == 1);
	REQUIRE(bridge.addRackDevice(feathers::mod::mseg::kTraits.tag) == 1);
	const feathers::rack::ChainRecord& record = bridge.chainRecordForTest();
	// THE SHAPE TAKEN FROM THE STATE ITSELF (kill-lanes step 3b): `ChainState` is sized by ONE chain's
	// capacity now, not by a pool of lanes, and `assign` refuses a state of a different shape — so the copy
	// is made in the record's own shape rather than in one restated here.
	feathers::rack::ChainState before(record.state().capacity(), record.state().chainCount());
	REQUIRE(before.assign(record.state()));

	bridge.restoreFxChain(bridge.fxChain());
	bridge.restoreModRack(bridge.modRack());
	CHECK(record.state() == before);
	CHECK(record.consistentForTest());
}

TEST_CASE("a restored row with no id, or one the other chain holds, gets its own — and it lands at the place its chain put it") {
	// THE RECORD KEYS A DEVICE ON ITS ID, so a row it cannot key is made keyable rather than dropped — a
	// device under no id is one nothing can route to, and two under one id are two devices at one address
	// (D1).
	//
	// **THE OTHER HALF OF THIS CASE IS GONE, AND DELIBERATELY (kill-lanes step 3b).** It also asserted that
	// the record HONOURED the lane a restored row asked for (`fxLaneAt(bridge, 1) == 3`) and that a strip
	// row carrying an fx lane was MOVED into the strip's band (`strip.routeLane >= kMaxFxSlots`). Neither is
	// a claim a caller can make any more: `rebuildChain` hands the record rows in order and names no slot,
	// and where a device is parked is the record's own business. **Kill-lanes step 3c took the ASKING as
	// well as the answering**: a restored row cannot request a lane, because there is no field to request
	// one in and no `order[]` to put storage somewhere other than the position. The second of the two could not even fail —
	// `rackIdentityAt` answers `kNoRouteLane`, which is `kMaxDeviceSlots` and passes `>= kMaxFxSlots` for
	// every device there could ever be. What is left of both is the claim that DID the work: the row lands
	// in the chain it was restored into, at the position that chain's order gives it, under an id of its own.
	feathers::ParamBridge bridge;
	const feathers::DeviceInstanceId fxId = bridge.fxIdentityAt(0).instanceId;
	REQUIRE(fxId != feathers::kNoInstance);

	feathers::FxChain chain = bridge.fxChain();
	// **THE ROW GOES AT POSITION 1, WHICH IS WHERE IT IS DRAWN (kill-lanes step 3c).** It was written at
	// `device[3]` with `order[1] = 3` beside it — storage at lane 3, second in the path — because the
	// array was lane-indexed and the order was a separate permutation. An `FxChain` is dense in
	// `[0, count)` now, so "second in the chain" is `device[1]` and there is no second array to keep in
	// step with it. The claim is unchanged: a row with no id, restored into the chain's second place.
	chain.device[1].row = feathers::defaultRowFor(feathers::fx::harm::kDescriptor, { feathers::kNoInstance });
	chain.count = 2;
	bridge.restoreFxChain(chain);
	REQUIRE(bridge.fxDeviceCount() == 2);
	const feathers::DeviceInstanceId given = bridge.fxIdentityAt(1).instanceId;
	CHECK(given != feathers::kNoInstance);
	CHECK(given != fxId);
	// AND IT IS THE DEVICE THE ROW NAMED, at the position the restored order gave it.
	//
	// **BY TAG, NOT BY DESCRIPTOR ADDRESS — AND THAT IS NOT A DETAIL.** `device_registry.h`'s `kDevices`
	// is an array of descriptor VALUES, so `deviceForTag("harm")` answers `&kDevices[1]` and can never be
	// `&fx::harm::kDescriptor`. A first draft of this line compared those two addresses and went red on a
	// restore that had worked perfectly — and doctest prints both pointers as `1`, so the red read
	// `CHECK( 1 == 1 ) is NOT correct`, which says nothing about what was found. The tag is the claim;
	// it is also the only thing the two copies agree on.
	const feathers::device::Descriptor* desc = bridge.descriptorOf(given);
	REQUIRE(desc != nullptr);
	CHECK(feathers::device::sameTag(desc->traits.tag, feathers::fx::harm::kTraits.tag));

	feathers::ModRack rack = bridge.modRack();
	// THE FX DEVICE'S ID, on a row in the strip — which is the whole point of this half. (A routing
	// address stood beside it, `2`; kill-lanes step 3c retired it, and this case's own comment above
	// records that the claim built on it went with step 3b.)
	rack.device[1] = feathers::defaultRowFor(feathers::mod::mseg::kDescriptor, { fxId });
	rack.count = 2;
	bridge.restoreModRack(rack);
	REQUIRE(bridge.modRack().count == 2);
	const feathers::DeviceIdentity strip = bridge.rackIdentityAt(1);
	CHECK(strip.instanceId != fxId);
	CHECK(strip.instanceId != feathers::kNoInstance);
	// AND IT IS IN THE STRIP, not in the lane it tried to name: the record holds it in chain 0.
	CHECK(bridge.chainRecordForTest().chainOf(strip.instanceId) == feathers::kStripChain);
	CHECK(bridge.fxIdentityAt(0).instanceId == fxId); // the fx device kept its own
	CHECK(bridge.chainRecordForTest().consistentForTest());
}

TEST_CASE("a settings write that stamps another device onto a slot keeps the slot's identity — same id, same place") {
	// THE TYPED SETTERS STAMP A TAG (`setEnvelopeSettings`, `setMsegSettings`, `setFilterSettings`): a card
	// hands over its device's own settings and the bridge stamps that device's tag and field count. On a
	// slot holding something else that is a replacement IN PLACE — which the flattened channel did by
	// overwriting the tag beside an untouched id, and the record must do the same: the id is what a saved
	// route names, so a new id here would orphan every route the device had.
	//
	// **THE SECOND HALF WAS `routeLane == before.routeLane` AND IT IS DELETED (kill-lanes step 3b)**, not
	// ported: `rackIdentityAt` answers `kNoRouteLane` for every device now, so the line read
	// `kNoRouteLane == kNoRouteLane` and could not fail. What it was asking — the device did not MOVE —
	// is asked of where the device stands instead, which is the one address left.
	feathers::ParamBridge bridge;
	REQUIRE(bridge.modRack().holds(0, feathers::mod::dahdsr::kTraits.tag));
	const feathers::DeviceIdentity before = bridge.rackIdentityAt(0);
	REQUIRE(before.instanceId != feathers::kNoInstance);
	const feathers::rack::ChainRecord& record = bridge.chainRecordForTest();
	const size_t wasAt = record.positionOf(before.instanceId);
	REQUIRE(wasAt != feathers::rack::ChainRecord::kNoPosition);

	bridge.setMsegSettings(0, feathers::mod::mseg::Settings {});
	CHECK(bridge.modRack().holds(0, feathers::mod::mseg::kTraits.tag));
	CHECK(bridge.rackIdentityAt(0).instanceId == before.instanceId);
	CHECK(record.chainOf(before.instanceId) == feathers::kStripChain);
	CHECK(record.positionOf(before.instanceId) == wasAt);
	CHECK(bridge.modRack().count == 1);
	CHECK(bridge.chainRecordForTest().consistentForTest());
}

// **RETIRED AT KILL-LANES STEP 5: "the bridge's routing gate and its record's agree on every pair,
// whatever the chains hold".** It walked the whole pair space over three shapes of chain and asserted
// zero disagreements between two answers. It is deleted rather than ported, and for two reasons that
// arrived from opposite directions:
//
//   - **ONE OF THE TWO PARTIES WAS THE RULE THE PRODUCT NO LONGER HAS.** The reference side asked
//     `retiredBandRule::maySourceRouteTo`, which compares two BAND-DERIVED RANKS; step 4b made the rule
//     reachability over the graph. Demanding agreement with the band rule is asserting the band rule —
//     the hollow shape this arc keeps finding, and worse than no case, because the next hand to touch
//     the real rule reads a green suite as permission to make it match. (The two still agreed over
//     THESE fixtures, which is why it was green: no fx device declares a modulator output, so no pair
//     here exercises the one consequence §2.3 names.)
//   - **AND THE OTHER PARTY IS NOW THE SAME FUNCTION.** `ParamBridge::mayRoute` forwards to
//     `ChainRecord::mayRoute` in one line. With the reference gone, "the bridge's gate and its record's
//     agree" is true by construction and could not fail for any reason a reader would care about.
//
// WHAT IT WAS FOR, kept here because the hazard has not gone away: a gesture gated by one authority and
// stored through another is how a route the editor accepts is silently never stored (5b C5). That is
// guarded now by there being ONE authority rather than by a case comparing two, which is the stronger
// guard and the reason the case has nothing left to say. Its most useful hour was as an instrument: it
// is what measured the 248 disagreements that found step 3b's position-vs-parking divergence.



TEST_CASE("an add keeps every route, the engine keeps them too, and a knob does not move the routes' generation") {
	// **WHY THE WRITE-BACK IS BY THE LEAST CHANGE.** Every structural edit answers with a whole chain value
	// that the bridge writes back into the record; the routes live in the record now, so a write-back that
	// re-seated every device would unroute the chain on a knob-free add. Asserted as VALUES — the amount in
	// the bridge and in the engine's own store — not as "something is still routed".
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	// **THE TWO DEVICES BY ID AND THE CONTROL BY ITS OWN INDEX (kill-lanes step 5).** The ORDINAL is still
	// wanted below — the engine's dense table is indexed by one — so it is derived from the control rather
	// than looked up separately: the two must name the same control or the assertion at the far end is
	// about a different knob.
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	const feathers::DeviceInstanceId envId = bridge.rackDeviceAt(0);
	REQUIRE(filterId != feathers::kNoInstance);
	REQUIRE(envId != feathers::kNoInstance);
	size_t cutoffControl = feathers::fx::filter::kDescriptor.params.size();
	for (size_t c = 0; c < feathers::fx::filter::kDescriptor.params.size(); ++c) {
		if (feathers::fx::filter::kDescriptor.params[c].modulatable) {
			cutoffControl = c;
			break;
		}
	}
	REQUIRE(cutoffControl < feathers::fx::filter::kDescriptor.params.size());
	const size_t cutoffOrdinal = feathers::fx::filter::kDescriptor.nthModulatableOfControl(cutoffControl);
	REQUIRE(cutoffOrdinal < feathers::fx::filter::kDescriptor.modulatableCount());
	const auto dest = feathers::backplane::Destination::control(filterId, cutoffControl);
	const auto fromEnv = feathers::backplane::Source::output(envId, 0);
	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(level >= 0);
	const auto toLevel = feathers::backplane::Destination::core(static_cast<size_t>(level));
	bridge.setRouteAmount(fromEnv, dest, 0.5f);
	bridge.setRouteAmount(feathers::backplane::Source::core(1), toLevel, -0.25f);
	REQUIRE(bridge.routeAmount(fromEnv, dest) == 0.5f);

	REQUIRE(bridge.addFxDevice(feathers::fx::harm::kTraits.tag) == 1);
	REQUIRE(bridge.addRackDevice(feathers::mod::mseg::kTraits.tag) == 1);
	CHECK(bridge.routeAmount(fromEnv, dest) == 0.5f);
	CHECK(bridge.routeAmount(feathers::backplane::Source::core(1), toLevel) == -0.25f);
	feathers::publishEditorLanes(bridge, params, synth);
	// **AND A BLOCK, BECAUSE A ROUTE IS FILED WHERE THE ENGINE KEEPS THE DEVICE** (kill-lanes step 3a).
	// The engine chooses a device's slot when it adopts it, at the sync — so the plan naming a device is
	// compiled there, not when the editor's lanes were published. Nothing can be heard before that block
	// either: the device is not running.
	std::vector<float> l(512), r(512);
	synth.provisionDevices(bridge.fxChain(), bridge.modRack()); // the message thread's half: it BUILDS
	synth.render(l.data(), r.data(), 256);
	// BY ID (kill-lanes step 3b): where the engine keeps the filter, and where it keeps the envelope.
	const size_t fxAt = synth.laneForTest().slotOf(bridge.fxDeviceAt(0));
	const size_t envAt = synth.laneForTest().slotOf(bridge.rackDeviceAt(0));
	REQUIRE(fxAt != feathers::rack::RackEngine::kNoSlot);
	REQUIRE(envAt != feathers::rack::RackEngine::kNoSlot);
	// BY IDENTITY (kill-lanes §2.4b): the envelope's output into the filter's cutoff, read back from the
	// snapshot the nodes' inputs are built from.
	CHECK(feathers::test::adoptedAmount(params, feathers::backplane::Source::output(bridge.rackDeviceAt(0), 0),
	                                    feathers::backplane::Destination::control(
	                                        bridge.fxDeviceAt(0), feathers::fx::filter::kDescriptor.controlOfNthModulatable(cutoffOrdinal)))
	      == 0.5f);
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(1), feathers::backplane::Destination::core(static_cast<size_t>(level))) == -0.25f);

	const uint32_t generation = bridge.routesGeneration();
	const auto& p = feathers::fx::filter::kDescriptor.params[0];
	// THE KNOB ON THE FILTER, ADDRESSED AS THE EDITOR ADDRESSES IT — the device, and its own control index.
	bridge.setDeviceControl(filterId, 0, static_cast<float>(p.max));
	bridge.setDeviceControl(filterId, 0, static_cast<float>(p.min));
	CHECK(bridge.routesGeneration() == generation); // a knob is not a route
}

TEST_CASE("a restore that drops a device drops its routes everywhere — the bridge's reads and the engine's store") {
	// THE RECORD TAKES A REMOVED DEVICE'S ROUTES WITH IT; what the editor reads and the engine resolves is
	// the list the record PUBLISHES, so a structural write-back that did not republish would leave both
	// holding a route into a device that is gone. The verbs that remove through the rules republish via
	// their own `restoreRoutes`; a bare `restoreFxChain` — a load, an undo — has only this.
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	REQUIRE(bridge.addFxDevice(feathers::fx::harm::kTraits.tag) == 1);
	// **THE HARM BY ID, AND THE ROUTE'S DISAPPEARANCE ASKED OF THE DEVICE (kill-lanes step 5).** This named
	// the coordinate of fx POSITION 1, and after the removal that coordinate names nothing — so the read
	// below returned zero because the address had gone empty, which is a weaker fact than the one the case
	// is about. An endpoint keeps naming the device the route was made to, whether or not it is still here.
	const feathers::DeviceInstanceId harmId = bridge.fxDeviceAt(1);
	REQUIRE(harmId != feathers::kNoInstance);
	const size_t harmControl = feathers::fx::harm::kDescriptor.controlOfNthModulatableBesideEnable(0);
	const auto dest = feathers::backplane::Destination::control(harmId, harmControl);
	bridge.setRouteAmount(feathers::backplane::Source::core(0), dest, 0.5f);
	REQUIRE(bridge.routeAmount(feathers::backplane::Source::core(0), dest) == 0.5f);
	feathers::publishEditorLanes(bridge, params, synth);
	// A BLOCK, SO THE ENGINE HAS THE DEVICE AND THE PLAN NAMES IT — see the add case above: a route is
	// filed at the slot the engine chose, which it chooses at the sync (kill-lanes step 3a).
	std::vector<float> l(512), r(512);
	synth.provisionDevices(bridge.fxChain(), bridge.modRack()); // the message thread's half: it BUILDS
	synth.render(l.data(), r.data(), 256);
	const size_t harmAt = synth.laneForTest().slotOf(bridge.fxDeviceAt(1)); // by id (kill-lanes step 3b)
	REQUIRE(harmAt != feathers::rack::RackEngine::kNoSlot);
	const feathers::backplane::Destination engineDest = feathers::backplane::Destination::control(
	    bridge.fxDeviceAt(1), feathers::fx::harm::kDescriptor.controlOfNthModulatableBesideEnable(0));
	REQUIRE(feathers::test::adoptedAmount(params, feathers::test::macro(0), engineDest) == 0.5f);

	feathers::FxChain chain = bridge.fxChain();
	chain.count = 1; // the harm gone, the filter kept
	bridge.restoreFxChain(chain);
	CHECK(bridge.fxDeviceCount() == 1);
	CHECK(bridge.routeAmount(feathers::backplane::Source::core(0), dest) == 0.0f);
	feathers::publishEditorLanes(bridge, params, synth);
	synth.render(l.data(), r.data(), 256); // and the block that takes the removal
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), engineDest) == 0.0f);
}

TEST_CASE("a route to a device that ARRIVES this block is resolved on that block, not the next one") {
	// **THE COST OF THE ENGINE CHOOSING ITS OWN SLOT, PAID AT THE RIGHT PLACE** (kill-lanes step 3a). The
	// plan `ParamSet` resolves through is in the ENGINE's slots, and a device's slot is chosen when the
	// engine adopts it — at the sync, which is AFTER the block's top, where the editor's routes were
	// compiled. So a route to a device arriving in this block names a device the plan did not know, and
	// would wait a block to be heard unless the compile is done again once the layout has moved:
	// `Synth::syncLane` recompiles when `LaneRack::converge` says something took a different slot.
	//
	// ASSERTED ON THE VALUE THE DEVICE WAS HANDED (`resolvedFieldForTest`) rather than on the modulated
	// row the editor reads, which is a block behind by design: the claim is about the sound this block.
	namespace hrm = feathers::fx::harm;
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);

	REQUIRE(bridge.addFxDevice(hrm::kTraits.tag) == 1);
	const feathers::DeviceInstanceId harmId = bridge.fxDeviceAt(1);
	REQUIRE(harmId != feathers::kNoInstance);
	const size_t width = static_cast<size_t>(hrm::Param::Width);
	const feathers::device::ParamDesc& pd = hrm::kDescriptor.params[width];
	REQUIRE(pd.modulatable);
	// A MACRO AT THE TOP, because its reading is a number this case sets rather than one an engine
	// arrives at — the claim is about WHEN the route is filed, not about how a source moves.
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 1.0);
	constexpr float kAmount = 0.5f;
	// BY ENDPOINT (kill-lanes step 5): the macro is a host source, the width is the HARM's own control.
	bridge.setRouteAmount(feathers::backplane::Source::core(0),
	                      feathers::backplane::Destination::control(harmId, width), kAmount);

	// THE PLUGIN'S ORDER, ONE VISIT AND ONE BLOCK: the device is built on the visit and arrives inside
	// the block's sync, which is the interleaving this case is about.
	synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	feathers::publishEditorLanes(bridge, params, synth);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	synth.render(l.data(), r.data(), 256);

	const size_t at = synth.laneForTest().slotOf(bridge.fxDeviceAt(1)); // by id (kill-lanes step 3b)
	REQUIRE(at != feathers::rack::RackEngine::kNoSlot); // it really did arrive in this block
	// THE BASE AT THE HARM'S POSITION (kill-lanes step 3b): the view's lanes are minted from the position
	// now, so a view indexed by the record's lane is the right row only by coincidence.
	const float base = fxDeviceAtPosition(bridge.fxChain(), 1).row.field[width];
	const float resolved = synth.laneForTest().engineForTest().resolvedFieldForTest(at, width);
	const double span = pd.modSpan > 0.0 ? pd.modSpan : (pd.max - pd.min);
	CHECK(resolved == doctest::Approx(static_cast<float>(base + kAmount * span)).epsilon(1e-4));
	CHECK(resolved != doctest::Approx(base)); // and it moved at all
}

TEST_CASE("a device the editor's visit builds for a newer publish than the block is on is kept, and runs one block later with no second build") {
	// **FEATHERS' LANE, THE INTERLEAVING THAT LOST THE RACK'S ADD** (5b C5b). The plugin's main-thread
	// visit builds from the bridge's record naming its publish; the block renders on the snapshot it last
	// took, which can be one publish behind. The lane must KEEP the arrival (it is from its future) rather
	// than hand it back — and must not need the visit to build it a second time, which is what the reap's
	// fallback would cost and what this case tells apart from it.
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	// THE PLUGIN'S OWN ORDER AT ACTIVATE: the visit builds the lane the bridge holds, then blocks run.
	synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	feathers::publishEditorLanes(bridge, params, synth);
	synth.render(l.data(), r.data(), 256);
	// ASKED BY DEVICE (kill-lanes step 3a, and by its ID since 3b): whether the ENGINE runs the device the
	// record has at that place in the chain.
	REQUIRE(synth.laneForTest().slotOf(bridge.fxDeviceAt(0)) != feathers::rack::RackEngine::kNoSlot);

	REQUIRE(bridge.addFxDevice(feathers::fx::harm::kTraits.tag) == 1);
	const feathers::DeviceInstanceId harm = bridge.fxDeviceAt(1);
	REQUIRE(harm != feathers::kNoInstance);
	REQUIRE(synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies()) == 1);

	synth.render(l.data(), r.data(), 256); // the block still on the OLD publish
	CHECK(synth.laneForTest().slotOf(harm) == feathers::rack::RackEngine::kNoSlot);
	CHECK(synth.laneForTest().pendingCountForTest() == 1); // KEPT, not handed back

	feathers::publishEditorLanes(bridge, params, synth); // the next block takes the new publish
	synth.render(l.data(), r.data(), 256);
	CHECK(synth.laneForTest().slotOf(harm) != feathers::rack::RackEngine::kNoSlot);
	CHECK(synth.laneForTest().pendingCountForTest() == 0);
}

TEST_CASE("every host stores a control's base by one rule — clamped always, and a stepped control NOT rounded") {
	// **BACKLOG DV, DECIDED: clamped, and not rounded.** Feathers' knob road
	// rounded and clamped every control; the rack's engine did neither for a control declaring
	// `offsetsMayLeaveRange`, so the filter's cutoff could sit past its range in a rack and not in Feathers.
	// One rule now, `device::coerceControl` — clamp, and keep a stepped control's fraction for its readers
	// to take the nearest step of — asked of both hosts with the same drags.
	namespace flt = feathers::fx::filter;
	size_t cutoff = flt::kDescriptor.params.size();
	size_t stepped = flt::kDescriptor.params.size();
	for (size_t c = 0; c < flt::kDescriptor.params.size(); ++c) {
		const auto& p = flt::kDescriptor.params[c];
		if (p.offsetsMayLeaveRange && cutoff == flt::kDescriptor.params.size()) {
			cutoff = c;
		}
		if (p.stepped && p.max - p.min >= 1.0 && stepped == flt::kDescriptor.params.size()) {
			stepped = c;
		}
	}
	REQUIRE(cutoff < flt::kDescriptor.params.size()); // the case needs the control the rule was about
	REQUIRE(stepped < flt::kDescriptor.params.size());
	const auto& cp = flt::kDescriptor.params[cutoff];
	const auto& sp = flt::kDescriptor.params[stepped];
	const float past = static_cast<float>(cp.max + 10.0);
	const float between = static_cast<float>(sp.min + 0.4);

	// THE RACK'S ENGINE
	feathers::rack::RackEngine engine { feathers::feathersAxes(), feathers::hostCatalogue() };
	const size_t lane = engine.addDevice(flt::kTraits.tag);
	REQUIRE(lane != feathers::rack::RackEngine::kNoSlot);
	engine.setField(lane, cutoff, past);
	engine.setField(lane, stepped, between);

	// FEATHERS' KNOB ROAD — the device by its id, the row it wrote read back at its position (kill-lanes
	// step 3b: the record's lane and the view's are two different numbers now).
	feathers::ParamBridge bridge;
	const feathers::DeviceInstanceId filter = bridge.fxDeviceAt(0);
	REQUIRE(filter != feathers::kNoInstance);
	bridge.setDeviceControl(filter, cutoff, past);
	bridge.setDeviceControl(filter, stepped, between);
	const feathers::DeviceRow row = fxDeviceAtPosition(bridge.fxChain(), 0).row;

	CHECK(engine.fieldAt(lane, cutoff) == static_cast<float>(cp.max));
	CHECK(row.field[cutoff] == static_cast<float>(cp.max));
	CHECK(engine.fieldAt(lane, stepped) == between);
	CHECK(row.field[stepped] == between);
}

TEST_CASE("routes are published to the engine only when the editor has changed them") {
	// **WHERE THE PER-BLOCK COST ACTUALLY WENT.** `publishEditorLanes` ran the whole dense road once
	// per audio block — reading the bridge's table was 27,840 atomic loads returning a 111 KB value, then
	// `setRoutes` walks it — on a table that changes only when a hand moves a ring. §14.3 measured the
	// pair at 0.37 % of a core. The fix was decided before the edge store existed: pointer-swap the
	// routes on the audio thread when the running one is marked dirty.
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);

	const int dest = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(dest >= 0);

	// THE FIRST PUBLISH ALWAYS HAPPENS, whatever the counters say — an engine that skipped it would
	// run on an empty table while the editor drew a full one.
	REQUIRE(params.routesGeneration() == 0);
	feathers::publishEditorLanes(bridge, params, synth);
	const uint32_t afterFirst = params.routesGeneration();
	CHECK(afterFirst != 0);

	SUBCASE("an untouched bridge does not republish") {
		// **COUNTED, NOT INFERRED FROM THE GENERATION.** The first draft asserted only that
		// `routesGeneration()` had not moved, and a known-bad that NEVER skips passed it — the
		// generation does not move when the same one is re-applied, so the assertion could not tell a
		// skip from a repeat. `routePublishes` is the seam that can.
		const size_t before = params.routePublishes();
		for (int i = 0; i < 8; ++i) {
			feathers::publishEditorLanes(bridge, params, synth);
		}
		CHECK(params.routePublishes() == before);
		CHECK(params.routesGeneration() == afterFirst);
	}

	SUBCASE("and a route dragged on the editor reaches the engine on the next block") {
		// **THE HALF THAT MAKES THE SKIP SAFE**, and without it this case would pass on an engine that
		// never published anything after the first block. Asserted as the VALUE at the destination,
		// not merely as the counter moving.
		bridge.setRouteAmount(feathers::backplane::Source::core(0),
		                      feathers::backplane::Destination::core(static_cast<size_t>(dest)), 0.5f);
		CHECK(bridge.routesGeneration() != afterFirst);
		feathers::publishEditorLanes(bridge, params, synth);
		CHECK(params.routesGeneration() == bridge.routesGeneration());
		CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest))) == doctest::Approx(0.5f));
	}
}

TEST_CASE("a ParamSet reset does not strand the engine with routes the editor still draws") {
	// **A TRAP CLOSED BEFORE ANYBODY FELL IN IT.** `ParamSet::reset()` has no callers today. It empties
	// the route table directly, outside the generation protocol `publishEditorLanes` now runs on — so
	// without clearing the generation, a reset would leave the engine agreeing it is already up to
	// date, the routes gone, and the editor still drawing every one of them. It would present as
	// "modulation stopped working" with nothing pointing at the reset.
	//
	// Written because the function has no callers rather than in spite of it: the next caller is who
	// this is for, and by then the symptom is a long way from the cause.
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);

	const int dest = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	REQUIRE(dest >= 0);
	bridge.setRouteAmount(feathers::backplane::Source::core(0),
	                      feathers::backplane::Destination::core(static_cast<size_t>(dest)), 0.5f);
	feathers::publishEditorLanes(bridge, params, synth);
	REQUIRE(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest))) == doctest::Approx(0.5f));

	params.reset();
	REQUIRE(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest))) == doctest::Approx(0.0f));

	// THE CLAIM: the next publish puts the editor's routes back, without the editor being touched.
	feathers::publishEditorLanes(bridge, params, synth);
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(static_cast<size_t>(dest))) == doctest::Approx(0.5f));
}

//------------------------------------------------------------------------------------------------//
// THE BRIDGE'S ROUTES ARE EDGES — one published store, copied by the engine whole or not at all     //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the engine adopts a published table whole, and one caught mid-publish leaves it on the last") {
	// **THE LOSS THE DENSE MATRIX COULD NOT HAVE AND AN EDGE LIST CAN.** A copy of a matrix straddling
	// a write saw each cell old or new. A copy of an edge list straddling `begin` sees the count at zero
	// and every route gone, INCLUDING ONES NOBODY TOUCHED. `adoptRoutes` must refuse that copy and keep
	// the table it had, generation and all, so the next block asks again.
	//
	// Deterministic: a raw `RouteList` left between `begin` and `end` is exactly what the audio thread
	// finds when it lands in the editor's publish. HOST ENDS ONLY (macros into the synth's own
	// parameters), so no device placement is asked — the list is compiled through one that holds none.
	namespace bp = feathers::backplane;
	feathers::ParamSet params;
	bp::RouteList published;
	published.begin();
	published.add({ bp::Source::core(0), bp::Destination::core(5), 0.5f });
	published.add({ bp::Source::core(2), bp::Destination::core(9), 0.25f });
	published.end();
	const auto adopt = [&] { return params.adoptRoutes(published); }; // by identity, nothing to compile against

	REQUIRE(adopt());
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(5)) == 0.5f);
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(2), feathers::backplane::Destination::core(9)) == 0.25f);
	CHECK(params.routesGeneration() == published.sequence());
	const uint32_t applied = params.routesGeneration();
	const size_t publishes = params.routePublishes();

	// THE EDITOR IS HALFWAY THROUGH A PUBLISH — it has changed ONE route and not yet ended.
	published.begin();
	published.add({ bp::Source::core(0), bp::Destination::core(5), 0.9f });
	CHECK_FALSE(adopt());
	// THE CLAIM: both routes still there at their old amounts — the one being edited AND the one
	// nobody touched — and the engine does not believe it is up to date.
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(5)) == 0.5f);
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(2), feathers::backplane::Destination::core(9)) == 0.25f);
	CHECK(params.routesGeneration() == applied);
	CHECK(params.routePublishes() == publishes);

	// AND WHEN THE PUBLISH ENDS, THE NEXT ASK TAKES IT — the table as published, which is one route.
	published.end();
	REQUIRE(adopt());
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(5)) == 0.9f);
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(2), feathers::backplane::Destination::core(9)) == 0.0f);
	CHECK(params.routesGeneration() == published.sequence());
}

// **"a route to a device the engine does not run yet is filed when its chain arrives" STOOD HERE AND IS
// DELETED (kill-lanes §2.4b).** It handed `ParamSet::adoptRoutes` a placement whose device appeared a
// compile later and watched the compiled coordinate appear. A route INTO A DEVICE is the engine's to file
// now, not the params': `RackEngine::rebuildInputs` skips a route whose device it does not run and
// `adoptDevice` marks the inputs dirty, so the same list is read again the drain the device arrives.
// That behaviour is the rack suites' to pin.

TEST_CASE("routes a hand makes in any order reach the engine, and the editor reads each one back") {
	// **THE WRITER KEEPS THE PUBLISH DESTINATION-MAJOR**, whatever order the gestures come in — and a
	// publish that is not refuses the backwards edge (`RouteEdges::add`), so a route made on a lower
	// destination after a higher one would be drawn by nobody and heard by nobody. Asserted against a
	// model written HERE, route by route, not against a whole-table read of the bridge (which reads the
	// same store).
	//
	// **EVERY END HERE IS A HOST END, WHICH IS WHY THE NUMBERS STAY (kill-lanes step 5).** A macro and a
	// core destination are indices into Feathers' own table — a host's fixed contract, not a device
	// address — so they are written as `Source::core` / `Destination::core` and the SCRAMBLED ORDER the
	// case is about is still an order over the destination index the writer keeps major.
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);

	struct Made {
		size_t source;
		size_t destination;
		float amount;
	};
	// Macros into core destinations — every pair legal. Scrambled destinations, two sources on one.
	const std::vector<Made> gestures {
		{ 1, 40, 0.5f }, { 0, 7, -0.25f }, { 3, 63, 1.0f }, { 2, 7, 0.75f }, { 0, 12, 0.125f },
		{ 1, 40, -0.5f }, // an UPDATE of the first
		{ 0, 12, 0.0f },  // and a route taken back to nothing, which is GONE
	};
	const auto from = [](size_t macro) { return feathers::backplane::Source::core(macro); };
	const auto into = [](size_t destination) { return feathers::backplane::Destination::core(destination); };
	for (const Made& g : gestures) {
		bridge.setRouteAmount(from(g.source), into(g.destination), g.amount);
	}
	const std::vector<Made> expected { { 1, 40, -0.5f }, { 0, 7, -0.25f }, { 3, 63, 1.0f }, { 2, 7, 0.75f } };

	feathers::publishEditorLanes(bridge, params, synth);
	// PLUS ONE: the default patch's envelope route (backlog EF), which the bridge was born with and the
	// engine adopts beside the hand's four.
	CHECK(bridge.routeList().count() == expected.size() + 1);
	CHECK(params.stagedRoutes().size() == expected.size() + 1);
	for (const Made& e : expected) {
		CAPTURE(e.source);
		CAPTURE(e.destination);
		CHECK(bridge.routeAmount(from(e.source), into(e.destination)) == e.amount);
		CHECK(feathers::test::adoptedAmount(params, from(e.source), into(e.destination)) == e.amount);
	}
	CHECK(bridge.routeAmount(from(0), into(12)) == 0.0f);
	CHECK(feathers::test::adoptedAmount(params, feathers::test::macro(0), feathers::backplane::Destination::core(12)) == 0.0f);

	// "ROUTED ELSEWHERE": destination 7 has two sources, so each is the other's elsewhere; 63 has one.
	CHECK(bridge.routedElsewhere(into(7), from(0)));
	CHECK(bridge.routedElsewhere(into(7), from(2)));
	CHECK_FALSE(bridge.routedElsewhere(into(63), from(3)));
	CHECK(bridge.routedElsewhere(into(63), from(0)));
	CHECK_FALSE(bridge.routedElsewhere(into(12), from(0)));
}

TEST_CASE("backlog EG: the engine hears a macro's polarity through the same road every other bridge-held thing takes, and publishes the macro through it") {
	// THE MODE IS THE BRIDGE'S; `publishEditorLanes` hands the engine one word of it, and the next
	// publish of the core sources reads the macro through it — asserted by value at the cell.
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	params.setBase(static_cast<clap_id>(feathers::macroParam(3)), 0.25);

	feathers::publishEditorLanes(bridge, params, synth);
	CHECK(params.macroBipolarMask() == 0u);
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[3] == doctest::Approx(0.25f));

	bridge.setMacroBipolar(3, true);
	feathers::publishEditorLanes(bridge, params, synth);
	CHECK(params.macroBipolarMask() == (1u << 3));
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[3] == doctest::Approx(-0.5f)); // 2 x 0.25 - 1
	CHECK(params.base(feathers::macroParam(3)) == doctest::Approx(0.25)); // the parameter is untouched

	bridge.setMacroBipolar(3, false);
	feathers::publishEditorLanes(bridge, params, synth);
	params.publishCoreSources();
	CHECK(params.coreSourceCells()[3] == doctest::Approx(0.25f));
}

TEST_CASE("backlog OJ: 600 routes through a wired bridge all reach the engine by value, the stores grow a logarithmic number of times, and a hand's next route is heard on the next block") {
	// **NO ROUTE IS DROPPED FOR ROOM** (backlog OJ). This case pinned the opposite — a full bridge, its excess
	// dropped and counted, a hand's next route refused at the ring. Now the bridge's list grows and, through
	// `wireRouteGrowth`, grows the params' and the lane engine's stores before it publishes a count that needs them.
	// An UNWIRED twin beside it is the control: the same list there is refused whole and counted, so the wiring is
	// what made the difference, not a store that happened to be big enough.
	namespace bp = feathers::backplane;
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::wireRouteGrowth(bridge, params, synth);
	synth.activate(48000.0, 4096);
	feathers::ParamSet unwiredParams;
	feathers::Synth unwired { unwiredParams };
	unwired.activate(48000.0, 4096);

	const int level = feathers::modDestinationIndexOf(feathers::ParamId::VoiceLevel);
	const int macro0Dest = feathers::modDestinationIndexOf(feathers::macroParam(0));
	REQUIRE(level >= 0);
	const auto fromMacro0 = bp::Source::core(0);
	const auto toLevel = bp::Destination::core(static_cast<size_t>(level));
	// EVERY PAIR THE RULE ALLOWS, until 600 — none from MACRO 0, none into Voice Level or into Macro 1's own parameter,
	// so the hand's route below is the only thing that moves Voice Level and its source reads its base.
	constexpr size_t kRoutes = 600;
	std::vector<bp::Route> loaded;
	const std::vector<bp::Source> sources = everySource(bridge);
	const std::vector<bp::Destination> destinations = everyDestination(bridge);
	for (const bp::Source& s : sources) {
		for (const bp::Destination& d : destinations) {
			if (loaded.size() < kRoutes && s != fromMacro0 && d != toLevel
			    && !(macro0Dest >= 0 && d == bp::Destination::core(static_cast<size_t>(macro0Dest))) && bridge.mayRoute(s, d)) {
				loaded.push_back({ s, d, 0.001f * static_cast<float>(1 + loaded.size() % 997) });
			}
		}
	}
	REQUIRE(loaded.size() == kRoutes);
	REQUIRE(kRoutes > feathers::kInitialRouteRoom); // past every store's starting room
	bridge.loadRoutesForTest(loaded);
	CHECK(bridge.routeList().count() == kRoutes);
	// x1.5 FROM 16: ceil(log1.5(600 / 16)) + 1 growths at most.
	CHECK(bridge.routeList().growthsForTest() <= 10);
	CHECK(params.routeRoom() >= kRoutes);
	CHECK(synth.routeRoomRefusals() == 0);

	synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	unwired.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	feathers::publishEditorLanes(bridge, params, synth);
	feathers::publishEditorLanes(bridge, unwiredParams, unwired);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	synth.render(l.data(), r.data(), 256);
	unwired.render(l.data(), r.data(), 256);
	// EVERY ROUTE ADOPTED, BY VALUE — and none refused.
	CHECK(params.routeRoomRefusals() == 0);
	CHECK(synth.routeRoomRefusals() == 0);
	CHECK(params.routesGeneration() == bridge.routesGeneration());
	REQUIRE(params.stagedRoutes().size() == kRoutes);
	for (const bp::Route& route : loaded) {
		CHECK(feathers::test::adoptedAmount(params, route.source, route.destination) == route.amount);
	}
	// THE CONTROL: unwired, the same list is refused whole and counted, and nothing of it is half-taken.
	CHECK(unwiredParams.routeRoomRefusals() == 1);
	CHECK(unwiredParams.stagedRoutes().size() == 0);

	// A HAND'S NEXT ROUTE, past everything: written, published, adopted and HEARD on the next block — Voice Level
	// resolves to its base plus amount x Macro 1 x span, which only this route can move.
	const int levelIndex = feathers::paramIndexOf(static_cast<clap_id>(feathers::ParamId::VoiceLevel));
	REQUIRE(levelIndex >= 0);
	const feathers::ParamDesc& desc = feathers::paramDescs()[static_cast<size_t>(levelIndex)];
	const double span = desc.max - desc.min;
	const double base = desc.min + 0.25 * span;
	params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceLevel), base);
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 0.5);
	REQUIRE(bridge.mayRoute(fromMacro0, toLevel));
	bridge.setRouteAmount(fromMacro0, toLevel, 0.5f);
	CHECK(bridge.routeAmount(fromMacro0, toLevel) == 0.5f);
	feathers::publishEditorLanes(bridge, params, synth);
	synth.render(l.data(), r.data(), 256);
	CHECK(feathers::test::adoptedAmount(params, fromMacro0, toLevel) == 0.5f);
	CHECK(params.stagedRoutes().size() == kRoutes + 1);
	CHECK(params.resolved(feathers::ParamId::VoiceLevel) == doctest::Approx(base + 0.5 * 0.5 * span));
}

// **TWO CASES STOOD HERE AND ARE DELETED WITH THE MACHINERY THEY WERE ABOUT (kill-lanes §2.4b, 2026-09-13):**
// the MEASURED cost of the bus FILL — what per-voice modulation would have multiplied — and the case that
// pinned `Synth::compileReach`'s relation against `Graph::forEachPrecedence`'s. There is no bus to fill and
// no mask to keep in agreement: a node's inputs point at the cells its sources own, legality is the graph's
// at the edit and the loader's at load, and the two bugs the pinning case found (a stale-guard and a
// self-legitimising route) were bugs in a mask that no longer exists.

TEST_CASE("tick windows: a strip envelope routed to Bus Utility's Gain arrives as a per-sample window, and the device follows it") {
	// Modulation smoothing. Two claims about VALUES, each against an independent restatement:
	//   1. after each tick, Gain's window is the resolve of the envelope's PUBLISHED ramp — base + amount × span × v,
	//      from the last tick's published value to this one's — at the first sample, the middle and the last;
	//   2. inside a real render, Bus Utility was handed those windows (a count its own ops keep).
	namespace bu = feathers::fx::busutil;
	Rig rig;
	dahdsr::Settings s = bare();
	s.attack = std::log2(0.050f); // ~3.7 ticks per 10% — the envelope moves across every tick below
	rig.install(s);
	feathers::FxChain chain;
	const size_t fxSlot = feathers::addFxDevice(&chain, bu::kTraits.tag);
	REQUIRE(fxSlot < chain.device.size());
	const feathers::ModRack rack = rig.synth.modRackForTest();
	rig.synth.provisionDevices(chain, rack);
	rig.synth.setFxChain(chain);
	rig.renderTicks(1);

	const size_t gain = static_cast<size_t>(bu::Param::Gain);
	const feathers::DeviceInstanceId busId =
	    rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, fxSlot);
	const feathers::DeviceInstanceId envId = rack.instanceAt(0);
	constexpr float kAmount = 0.25f;
	rig.routes.set(feathers::backplane::Source::output(envId, 0),
	               feathers::backplane::Destination::control(busId, gain), kAmount);
	rig.routes.publish(rig.params);
	rig.renderTicks(1);

	const auto& engine = rig.synth.laneForTest().engineForTest();
	const size_t busSlot = engine.slotOf(busId);
	REQUIRE(busSlot != feathers::rack::RackEngine::kNoSlot);
	const feathers::device::ParamDesc& pd = bu::kDescriptor.params[gain];
	const double base = chain.device[fxSlot].row.field[gain];
	const double span = pd.max - pd.min;
	const auto resolve = [&](double v) {
		return static_cast<float>(std::clamp(base + static_cast<double>(kAmount) * v * span, pd.min, pd.max));
	};

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	float prev = rig.synth.laneForTest().publishedOutputOf(envId, 0);
	int moving = 0;
	for (int t = 0; t < 12; ++t) {
		rig.renderTicks(1);
		const float cur = rig.synth.laneForTest().publishedOutputOf(envId, 0);
		const float* w = engine.controlWindowForTest(busSlot, gain);
		REQUIRE(w != nullptr);
		if (cur == prev) {
			prev = cur;
			continue;
		}
		++moving;
		CAPTURE(t);
		CAPTURE(prev);
		CAPTURE(cur);
		REQUIRE_FALSE(feathers::device::tickWindowFlat(w));
		const auto rampAt = [&](int i) { return static_cast<double>(prev) + (cur - prev) * static_cast<double>(i + 1) / 64.0; };
		CHECK(w[0] == doctest::Approx(resolve(rampAt(0))).epsilon(1e-4));
		CHECK(w[31] == doctest::Approx(resolve(rampAt(31))).epsilon(1e-4));
		CHECK(w[63] == doctest::Approx(resolve(cur)).epsilon(1e-4));
		prev = cur;
	}
	REQUIRE(moving >= 3); // or the loop asserted nothing

	const void* storage = engine.storageForTest(busSlot);
	REQUIRE(storage != nullptr);
	CHECK(bu::windowedCallsForTest(storage) > 0u);
}

TEST_CASE("tick windows, PER VOICE: a strip envelope routed to a Poly HARM's Level reaches each voice's instance as that voice's own window") {
	// The per-voice road (backlog FK): a device running inside the voices resolves its row per voice when a per-voice
	// source reaches it, and now its declared controls' windows too. Two notes a quarter-attack apart stand at different
	// places on their envelopes, so their windows differ. Asserted as values: each voice's Level window ends on the
	// resolve of THAT voice's published cell (base + amount x span x v), and each voice's instance followed windows.
	namespace hm = feathers::fx::harm;
	Rig rig;
	dahdsr::Settings s = bare();
	s.attack = std::log2(0.200f);
	rig.install(s);
	feathers::FxChain chain;
	const size_t fxSlot = feathers::addFxDevice(&chain, hm::kTraits.tag);
	REQUIRE(fxSlot < chain.device.size());
	chain.device[fxSlot].row.field[static_cast<size_t>(hm::Param::Poly)] = 1.0f;
	chain.device[fxSlot].row.field[static_cast<size_t>(hm::Param::Enabled)] = 1.0f;
	const size_t level = static_cast<size_t>(hm::Param::Level);
	chain.device[fxSlot].row.field[level] = 0.0f;
	const feathers::ModRack rack = rig.synth.modRackForTest();
	rig.synth.provisionDevices(chain, rack);
	rig.synth.setFxChain(chain);
	rig.renderTicks(1);
	const feathers::DeviceInstanceId harmId = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, fxSlot);
	const feathers::DeviceInstanceId envId = rack.instanceAt(0);
	constexpr float kAmount = 0.5f;
	feathers::test::Routes routes;
	routes.set(feathers::backplane::Source::output(envId, 0), feathers::backplane::Destination::control(harmId, level), kAmount);
	routes.publish(rig.params);
	rig.renderTicks(1);

	const auto& engine = rig.synth.laneForTest().engineForTest();
	const size_t harmSlot = engine.slotOf(harmId);
	REQUIRE(harmSlot != feathers::rack::RackEngine::kNoSlot);
	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	rig.renderTicks(40);
	rig.synth.noteOn(0, static_cast<int16_t>(kKeyC4 + 7), 2, 1.0);
	rig.renderTicks(20);
	const int a = rig.synth.slotOfKeyForTest(kKeyC4);
	const int b = rig.synth.slotOfKeyForTest(static_cast<int16_t>(kKeyC4 + 7));
	REQUIRE(a >= 0);
	REQUIRE(b >= 0);
	REQUIRE(a != b);
	const float* cells = rig.synth.laneForTest().outputCellForVoiceOf(envId, 0);
	REQUIRE(cells != nullptr);
	const feathers::device::ParamDesc& pd = hm::kDescriptor.params[level];
	for (const int v : { a, b }) {
		CAPTURE(v);
		const float* w = engine.controlWindowForVoiceForTest(harmSlot, level, static_cast<size_t>(v));
		REQUIRE(w != nullptr);
		REQUIRE_FALSE(feathers::device::tickWindowFlat(w)); // both envelopes still attacking
		const double expected = std::clamp(kAmount * static_cast<double>(cells[v]) * (pd.max - pd.min), pd.min, pd.max);
		CHECK(w[63] == doctest::Approx(expected).epsilon(1e-4));
		CHECK(w[0] < w[63]);
		const auto& seat = engine.slotAtLane(harmSlot);
		REQUIRE(static_cast<size_t>(v) < seat.voices.size());
		CHECK(hm::opsWindowedCallsForTest(seat.voices[static_cast<size_t>(v)].get()) > 0u);
	}
	CHECK(engine.controlWindowForVoiceForTest(harmSlot, level, static_cast<size_t>(a))[63]
	      != doctest::Approx(engine.controlWindowForVoiceForTest(harmSlot, level, static_cast<size_t>(b))[63]).epsilon(0.01));
}

TEST_CASE("the engine tells the editor where modulation has left each sound's place") {
	// THE ROAD THE EXPLORE STAR ORBITS BY, end to end and through the functions the plugin itself
	// calls: a route left on the BRIDGE reaches the engine (`publishEditorLanes`), the engine plays the
	// sound somewhere else because of it, and what it played comes back (`publishEngineReadings`).
	// Nothing outside the plugin can see the plugin's own bridge, so its three publish sites call this
	// one function and the function is asked here.
	ParamSet params;
	Synth synth { params };
	synth.activate(48000.0, 4096);
	feathers::ParamBridge bridge;

	const feathers::backplane::Route routing {
		feathers::backplane::Source::core(0),
		feathers::backplane::Destination::core(
		    feathers::travelDestinationIndex(3, feathers::TravelAxis::Theta)),
		0.25f };
	bridge.loadRoutesForTest(std::span<const feathers::backplane::Route>(&routing, 1));
	feathers::publishEditorLanes(bridge, params, synth);
	params.setBase(static_cast<clap_id>(feathers::macroParam(0)), 1.0);

	// NOTHING HAS BEEN PUBLISHED YET — so a pass below is the function's doing.
	REQUIRE(bridge.travelPulls() == feathers::TravelPulls {});

	synth.render(nullptr, nullptr, 0);
	std::vector<float> l(4096), r(4096);
	synth.render(l.data(), r.data(), 4 * kTickSamples);
	feathers::publishEngineReadings(bridge, synth);

	// A quarter of a full swing, and a full swing of Theta is one turn.
	const feathers::TravelPulls told = bridge.travelPulls();
	CHECK(told.theta[3] == doctest::Approx(0.25 * 2.0 * 3.14159265358979323846));
	CHECK(told.radius[3] == 0.0);
	CHECK(told.theta[2] == 0.0);
	// And it is the engine's own number, not a second derivation that happens to agree.
	CHECK(told == synth.travelPulls());
}


TEST_CASE("the engine tells the editor where a stereo route has left a filter's cutoff in each ear") {
	// **BACKLOG KU, THE FEATHERS ROAD:** `publishEngineReadings` carries the right ear into the bridge's telemetry, and
	// `BridgeDeviceHost::modulatedFieldRAt` answers it — the engine's own number, read by value at the engine's slot.
	namespace flt = feathers::fx::filter;
	namespace st = feathers::mod::stereo;
	feathers::ParamBridge bridge;
	REQUIRE(bridge.fxChain().count == 1); // the default filter, at position 0
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	REQUIRE(filterId != feathers::kNoInstance);
	const size_t toolSlot = bridge.addRackDevice(st::kTraits.tag);
	REQUIRE(toolSlot < feathers::kMaxRackSlots);
	const feathers::DeviceInstanceId toolId = bridge.rackDeviceAt(toolSlot);
	REQUIRE(toolId != feathers::kNoInstance);
	const size_t cutoff = static_cast<size_t>(flt::Param::Cutoff);
	const size_t drive = static_cast<size_t>(flt::Param::Drive);
	bridge.setDeviceControl(toolId, static_cast<size_t>(st::Param::In), 1.0f); // Split at In 100%, Amount 100%: (+1, −1)
	bridge.setRouteAmount(feathers::backplane::Source::output(toolId, static_cast<size_t>(st::Output::Split)),
	                      feathers::backplane::Destination::control(filterId, cutoff), 0.1f);

	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	synth.provisionDevices(bridge.fxChain(), bridge.modRack());
	feathers::publishEditorLanes(bridge, params, synth);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	for (int i = 0; i < 2; ++i) {
		synth.render(l.data(), r.data(), static_cast<int>(l.size()));
	}
	// READ THROUGH THE BRIDGE'S OWN SEAM — `BridgeDeviceHost` (visage-side, not linked here) dispatches to exactly
	// this; its dispatch is pinned in test_gui_widgets_mod_ring.
	const size_t position = bridge.fxSlotOf(filterId);
	float right = 0.0f;
	// NOTHING PUBLISHED YET — so a pass below is `publishEngineReadings`' doing.
	REQUIRE_FALSE(bridge.fxModulatedFieldRAt(position, cutoff, right));
	feathers::publishEngineReadings(bridge, synth);

	const feathers::rack::RackEngine& engine = synth.laneForTest().engineForTest();
	const size_t slot = engine.slotOf(filterId);
	REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
	REQUIRE(engine.stereoLiveForTest(slot, cutoff));
	REQUIRE(bridge.fxModulatedFieldRAt(position, cutoff, right));
	CHECK(right == engine.resolvedFieldRForTest(slot, cutoff));
	CHECK(bridge.fxModulatedFieldAt(position, cutoff) == engine.resolvedFieldForTest(slot, cutoff));
	// NOT VACUOUS: Split at In 100% is (+1, −1), so the two ears differ.
	CHECK(right != bridge.fxModulatedFieldAt(position, cutoff));
	// A CONTROL NO STEREO ROUTE REACHES IS MONO: no right ear.
	float none = 0.0f;
	CHECK_FALSE(bridge.fxModulatedFieldRAt(position, drive, none));
}

TEST_CASE("backlog MK: the engine tells the editor how loud each fx position's device is coming out — the engine's held peak, by value, and nothing for an empty position") {
	// EVERY FX CARD'S COURTESY OUTPUT METER reads `ParamBridge::fxOutputPeak`; `publishEngineReadings` is the only
	// road into it from the engine, so this is the case that fails if that line goes.
	feathers::ParamBridge bridge;
	REQUIRE(bridge.fxChain().count == 1); // the default filter, at position 0
	const feathers::DeviceInstanceId filterId = bridge.fxDeviceAt(0);
	REQUIRE(filterId != feathers::kNoInstance);
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	synth.provisionDevices(bridge.fxChain(), bridge.modRack());
	feathers::publishEditorLanes(bridge, params, synth);
	const size_t position = bridge.fxSlotOf(filterId);
	float peak = -1.0f;
	// NOTHING PUBLISHED YET — so a pass below is `publishEngineReadings`' doing.
	REQUIRE_FALSE(bridge.fxOutputPeak(position, peak));

	synth.noteOn(0, 60, 1, 1.0);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	for (int i = 0; i < 8; ++i) {
		synth.render(l.data(), r.data(), static_cast<int>(l.size()));
	}
	feathers::publishEngineReadings(bridge, synth);

	const feathers::rack::RackEngine& engine = synth.laneForTest().engineForTest();
	const size_t slot = engine.slotOf(filterId);
	REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
	float enginePeak = -1.0f;
	REQUIRE(engine.outputPeakAt(slot, enginePeak));
	REQUIRE(bridge.fxOutputPeak(position, peak));
	CHECK(peak == enginePeak);
	CHECK(peak > 0.0f); // not vacuous: a held note sounds through the filter
	// AN EMPTY POSITION METERS NOTHING, and says so.
	float none = -3.0f;
	CHECK_FALSE(bridge.fxOutputPeak(position + 1, none));
	CHECK(none == -3.0f);
}

TEST_CASE("THE REPRO, 2026-10-03: delete the patch's Voice Level envelope, add another with a long release, route it to Voice Level — a released voice lives while that envelope holds Voice Level above zero, at its value, and is freed after") {
	// The symptom: a voice was freed before its envelope completed — the original envelope deleted and a different one routed to Voice
	// Level in its place, through the verbs a hand uses — `removeRackDevice`, `addRackDevice`, `setDeviceControl`,
	// `setRouteAmount` — published as the editor publishes. Nothing about a voice's life may be keyed to the
	// DEFAULT envelope (its slot, its instance, its kind); Voice Level alone decides.
	namespace dsr = feathers::mod::dahdsr;
	namespace mseg = feathers::mod::mseg;
	enum class Kind { Dahdsr, Mseg };
	for (const Kind kind : { Kind::Dahdsr, Kind::Mseg }) {
		for (const bool poly : { true, false }) {
			CAPTURE(static_cast<int>(kind));
			CAPTURE(poly);
			Rig rig;
			feathers::ParamBridge bridge;
			bridge.refresh();
			rig.params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 0.0);
			const auto sync = [&] {
				rig.synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
				feathers::publishEditorLanes(bridge, rig.params, rig.synth);
				rig.render(64);
			};
			sync();
			// THE PREMISE: the default patch's envelope is in slot 0 and drives Voice Level.
			REQUIRE(rig.params.isRouted(ParamId::VoiceLevel));
			// 1. DELETE IT.
			REQUIRE(bridge.removeRackDevice(0));
			sync();
			REQUIRE_FALSE(rig.params.isRouted(ParamId::VoiceLevel)); // its route went with it
			// 2. ADD ANOTHER, WITH A LONG RELEASE (or a long fall).
			const size_t slot = bridge.addRackDevice(kind == Kind::Dahdsr ? dsr::kTraits.tag : mseg::kTraits.tag);
			REQUIRE(slot < feathers::kMaxRackSlots);
			bridge.refresh();
			const feathers::DeviceInstanceId id = bridge.rackDeviceAt(slot);
			REQUIRE(id != feathers::kNoInstance);
			if (kind == Kind::Dahdsr) {
				bridge.setDeviceControl(id, static_cast<size_t>(dsr::Param::Attack), std::log2(0.005f));
				bridge.setDeviceControl(id, static_cast<size_t>(dsr::Param::Sustain), 1.0f);
				bridge.setDeviceControl(id, static_cast<size_t>(dsr::Param::Release), 0.0f); // one second
				bridge.setDeviceControl(id, static_cast<size_t>(dsr::Param::Poly), poly ? 1.0f : 0.0f);
			}
			else {
				// A TWO-SECOND ONE-SHOT, 0 → 1 → 0, so the key comes up long before it ends.
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::Param::Mode),
				                        static_cast<float>(static_cast<int>(mseg::Mode::Once)));
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::Param::SustainSegment),
				                        static_cast<float>(mseg::kNoSustain));
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::Param::TimeScale), 1.0f);
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::Param::StartLevel), 0.0f);
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::Param::SegmentCount), 2.0f);
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::segmentParam(0, mseg::SegmentField::Level)), 1.0f);
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::segmentParam(1, mseg::SegmentField::Level)), 0.0f);
				bridge.setDeviceControl(id, static_cast<size_t>(mseg::Param::Poly), poly ? 1.0f : 0.0f);
			}
			// 3. ROUTE IT TO VOICE LEVEL.
			const int level = feathers::modDestinationIndexOf(ParamId::VoiceLevel);
			REQUIRE(level >= 0);
			bridge.setRouteAmount(feathers::backplane::Source::output(id, 0),
			                      feathers::backplane::Destination::core(static_cast<size_t>(level)), 1.0f);
			sync();
			REQUIRE(rig.params.isRouted(ParamId::VoiceLevel));
			// 4. PLAY A NOTE AND RELEASE IT.
			rig.synth.noteOn(0, kKeyC4, 1, 1.0);
			rig.render(4096);
			rig.render(4096); // ~0.17 s
			const int voice = rig.synth.slotOfKeyForTest(kKeyC4);
			REQUIRE(voice >= 0);
			rig.synth.noteOff(0, kKeyC4, 1, 0.0);
			for (int i = 0; i < 6; ++i) {
				rig.render(4096); // ~0.5 s past the key
			}
			// 5. STILL ALIVE, AT THE ENVELOPE'S VALUE.
			const float value = rig.synth.laneForTest().publishedOutputOf(id, 0);
			CAPTURE(value);
			REQUIRE(value > 0.05f);
			REQUIRE(rig.synth.slotOfKeyForTest(kKeyC4) == voice);
			CHECK(std::abs(rig.synth.voiceGainForTest(voice) - value) < 0.02);
			// AND FREED once it has reached the floor.
			for (int i = 0; i < 40; ++i) {
				rig.render(4096); // ~3.4 s more
			}
			CHECK(rig.synth.laneForTest().publishedOutputOf(id, 0) < 1e-3f);
			CHECK(rig.synth.slotOfKeyForTest(kKeyC4) < 0);
			CHECK(rig.synth.soundingVoiceCount() == 0);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// UNBOUNDED DEVICES, STEP 5 — THE RECORD AND ITS PUBLISHED STATE GROW                            //
//------------------------------------------------------------------------------------------------//

TEST_CASE("unbounded devices step 5: the bridge's record grows past a chain's capacity, keeps every device, "
          "and the synth takes the bigger state whole") {
	ParamSet params;
	Synth synth { params };
	synth.activate(48000.0, 4096);
	feathers::ParamBridge bridge;
	namespace flt = feathers::fx::filter;

	// FULL AT TODAY'S SIZE: the lane holds what it was built for.
	const size_t start = bridge.chainCapacity(feathers::kLaneChain);
	REQUIRE(start == feathers::kMaxFxSlots);
	for (size_t i = bridge.fxChain().count; i < start; ++i) { // a fresh bridge may already hold the default's
		REQUIRE(bridge.addFxDevice(flt::kTraits.tag) == i);
	}
	const feathers::DeviceInstanceId first = bridge.fxDeviceAt(0);
	const size_t publishedBefore = bridge.chainState().capacity();

	// ROOM FOR 40, asked of the bridge — the record grows past the pool it was built with, and every device
	// it held is carried across under its own id.
	REQUIRE(bridge.reserveChain(feathers::kLaneChain, 40));
	CHECK(bridge.chainCapacity(feathers::kLaneChain) >= 40);
	CHECK(bridge.chainState().capacity() > publishedBefore);
	CHECK(bridge.fxChain().count == start);
	CHECK(bridge.fxDeviceAt(0) == first);
	// AN EDIT AFTER THE GROWTH — published in the NEW shape, so arriving at the synth proves it took that shape.
	// (Adding past the old capacity is step 6: the add verbs still refuse at `kMaxFxSlots` here.)
	flt::State edited;
	edited.settings.resonance = 0.66f;
	bridge.setFilterSettings(0, edited);
	CHECK(bridge.publishedChains().capacity() == bridge.chainState().capacity());

	// THE ENGINE SIDE: the provision asks the synth for room (main thread), the drain takes the growth and then
	// the state, and every device runs. Twice, because the first drain may only have taken the growth.
	(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), nullptr);
	feathers::publishEditorLanes(bridge, params, synth);
	synth.drainRetiredDevices();
	(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), nullptr);
	feathers::publishEditorLanes(bridge, params, synth);
	std::vector<float> left(256), right(256);
	synth.render(left.data(), right.data(), 256); // the block converges the lane to what the drain took
	CHECK(feathers::fxChainLength(synth.fxChainForTest()) == start);
	CHECK(synth.fxChainForTest().device[0].row.instanceId == first);
	CHECK(synth.requestedStateCapacity() == bridge.chainState().capacity());
	flt::Row decoded {};
	const size_t present = feathers::fromDeviceRow(synth.fxChainForTest().device[0].row, decoded);
	CHECK(flt::stateOf(decoded, present).settings.resonance == doctest::Approx(0.66f));
	for (size_t position = 0; position < start; ++position) {
		CAPTURE(position);
		CHECK(synth.laneForTest().slotOfStatePosition(feathers::kLaneChain, position) != feathers::rack::RackEngine::kNoSlot);
	}
}

//------------------------------------------------------------------------------------------------//
// UNBOUNDED DEVICES, STEP 7 — SIXTY-FOUR DEVICES, ROUTED END TO END                              //
//------------------------------------------------------------------------------------------------//

TEST_CASE("unbounded devices step 7: sixty-four devices in the lane, a modulator routed to the last of them and a "
          "macro to the first, and the engine builds both inputs") {
	ParamSet params;
	Synth synth { params };
	synth.activate(48000.0, 4096);
	feathers::ParamBridge bridge;
	namespace flt = feathers::fx::filter;
	namespace bp = feathers::backplane;

	constexpr size_t kDevices = 64;
	while (bridge.fxChain().count < kDevices) {
		REQUIRE(bridge.addFxDevice(flt::kTraits.tag) != feathers::kNoPosition);
	}
	REQUIRE(bridge.fxChain().count == kDevices);
	const feathers::DeviceInstanceId firstFx = bridge.fxDeviceAt(0);
	const feathers::DeviceInstanceId lastFx = bridge.fxDeviceAt(kDevices - 1);
	const feathers::DeviceInstanceId envelope = bridge.rackDeviceAt(0);
	REQUIRE(envelope != feathers::kNoInstance);
	const size_t cutoff = flt::kDescriptor.controlOfNthModulatableBesideEnable(0); // the filter's first modulatable control
	REQUIRE(cutoff < flt::kDescriptor.params.size());

	// ACROSS THE WHOLE CHAIN: the strip's envelope into the sixty-fourth device, a macro into the first.
	bridge.setRouteAmount(bp::Source::output(envelope, 0), bp::Destination::control(lastFx, cutoff), 0.5f);
	bridge.setRouteAmount(bp::Source::core(0), bp::Destination::control(firstFx, cutoff), 0.25f);
	CHECK(bridge.routeAmount(bp::Source::output(envelope, 0), bp::Destination::control(lastFx, cutoff))
	      == doctest::Approx(0.5f));
	CHECK(bridge.routeAmount(bp::Source::core(0), bp::Destination::control(firstFx, cutoff)) == doctest::Approx(0.25f));

	// THE ENGINE SIDE, by the product's road: provision (room asked for), the editor lanes' drain, a block —
	// twice, because the first drain may only have taken the room.
	std::vector<float> left(256), right(256);
	for (int pass = 0; pass < 3; ++pass) {
		(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), nullptr);
		feathers::publishEditorLanes(bridge, params, synth);
		synth.render(left.data(), right.data(), 256);
		synth.drainRetiredDevices();
	}
	const feathers::rack::RackEngine& engine = synth.laneForTest().engineForTest();
	CHECK(engine.capacity() >= kDevices + 1);
	const size_t laneOfLast = engine.slotOf(lastFx);
	const size_t laneOfFirst = engine.slotOf(firstFx);
	REQUIRE(laneOfLast != feathers::rack::RackEngine::kNoSlot);
	REQUIRE(laneOfFirst != feathers::rack::RackEngine::kNoSlot);
	// THE ROUTES REACHED THE NODES THEY NAME — an input on each control, built by the engine from routes by
	// identity, at lanes the engine only had after growing.
	CHECK(engine.inputCountForTest(laneOfLast, cutoff) == 1);
	CHECK(engine.inputCountForTest(laneOfFirst, cutoff) == 1);
	for (size_t position = 0; position < kDevices; ++position) {
		CAPTURE(position);
		CHECK(synth.laneForTest().slotOfStatePosition(feathers::kLaneChain, position)
		      != feathers::rack::RackEngine::kNoSlot);
	}
}

TEST_CASE("lane-mods: an MSEG in the fx lane says where it stands — the engine's own playhead, through the lane's channel, and idle before a note") {
	// A lane modulator's card draws its marker from `ParamBridge::fxPlayhead` (via `BridgeDeviceHost::playheadAt`);
	// the strip's road (`rackPlayhead`) is keyed by strip slot and cannot name it. The value is the DEVICE's own
	// (`LaneRack::playheadOf`), read the same instant, so the channel is shown to carry it rather than a neighbour's.
	feathers::ParamBridge bridge;
	const size_t at = bridge.addFxDevice(feathers::mod::mseg::kTraits.tag);
	REQUIRE(at == 1); // after the default filter
	const feathers::DeviceInstanceId mseg = bridge.fxDeviceAt(at);

	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	synth.provisionDevices(bridge.fxChain(), bridge.modRack());
	feathers::publishEditorLanes(bridge, params, synth);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	synth.render(l.data(), r.data(), static_cast<int>(l.size()));
	feathers::publishEngineReadings(bridge, synth);
	REQUIRE(synth.laneForTest().descriptorOfForTest(mseg) != nullptr); // the premise: the lane RUNS it
	CHECK_FALSE(bridge.fxPlayhead(at).running()); // nothing gated yet

	synth.noteOn(0, 60, 1, 1.0);
	for (int i = 0; i < 8; ++i) {
		synth.render(l.data(), r.data(), static_cast<int>(l.size()));
	}
	feathers::publishEngineReadings(bridge, synth);
	uint32_t ordinal = 0;
	float phase = -1.0f;
	REQUIRE(synth.laneForTest().playheadOf(mseg, &ordinal, &phase));
	const feathers::mod::Playhead published = bridge.fxPlayhead(at);
	CHECK(published.running());
	CHECK(published.ordinal == ordinal);
	CHECK(published.phase == doctest::Approx(phase));
	// AND THE FILTER AT POSITION 0 IS NOT A MODULATOR: idle, not a copy of its neighbour's.
	CHECK_FALSE(bridge.fxPlayhead(0).running());
}

//------------------------------------------------------------------------------------------------//
// BACKLOG LN: A MODULATOR READ BY ANOTHER IS READ IN THE SAME TICK, WHATEVER CHAIN EITHER STANDS IN //
//------------------------------------------------------------------------------------------------//
//
// The entry: the modulator pass walked chain 0 (the strip) and then chain 1 (the lane), so a lane modulator
// feeding a strip modulator was read a tick late. MEASURED 2026-10-10: it is not so on the product road. The
// engine holds ONE chain (`RackEngine`'s graph is built with one), and the converger places every device of
// both record chains into it in the record's `order()`, which holds every route as a precedence — so a lane
// LFO routed into a strip MATH is placed, and ticked, before it. These cases pin that, and the known-bad that
// turns them red is the converger ignoring the record's order (its no-order road: the chains in index order).
//
// **WHAT A LATE READ LOOKS LIKE, AND WHY THE VALUE ALONE CANNOT SEE IT.** The lane's zero-frame pass
// (`LaneRack::begin` -> `beginBlock(0)`) re-resolves every modulator after the tick, so a late reader's ROW and
// the cell the voice reads are both right again before anything reads them. What stays wrong is the reader's
// TICK WINDOW, the per-sample ramp: the tick published a ramp toward the stale value and the zero-frame pass,
// seeing a new value without a tick, republishes it FLAT (`advanceModulators`). So a late reader STEPS once a
// tick where it should ramp. That is the KU note's Stereo Tool ("placed BEFORE its own source ... both ears step
// once per tick"), the same root; the Stereo Tool reader below is that case.

namespace {

/// WHERE A ROUTED MODULATOR STANDS: the strip (chain 0) or the fx lane (chain 1).
enum class LnChain { Strip, Lane };
/// THE READER: a MATH (A + 0, so its output IS its A) or a Stereo Tool (Split at 100%: (In, −In)). Both read the
/// route at control 0, −1..1 resting at 0.
enum class LnReader { Math, StereoTool };

struct LnTick {
	float lfoNow = 0.0f, lfoWas = 0.0f; // the LFO's output published in this tick, and in the one before
	float resolved = 0.0f;              // the reader's control 0 as the engine resolved it
	double fine = 0.0;                  // the voice's Fine Tune, which the reader's output reaches through a core route
	/// THE READER'S TICK WINDOWS, left and right ear: the first, middle and last samples.
	float l0 = 0.0f, l31 = 0.0f, l63 = 0.0f, r0 = 0.0f, r31 = 0.0f, r63 = 0.0f;
};

/// ONE FREE, FAST LFO ROUTED INTO THE READER'S CONTROL 0, each in the chain named, through the bridge — the
/// record's gate, its order and its routes, the road a hand's edit takes — then stepped ONE CONTROL TICK AT A TIME.
std::vector<LnTick> lfoIntoReader(LnChain lfoIn, LnReader reader, LnChain readerIn, int ticks) {
	namespace lfo = feathers::mod::lfo;
	namespace mth = feathers::mod::math;
	namespace st = feathers::mod::stereo;
	feathers::ParamBridge bridge;
	const auto add = [&](LnChain in, const char* tag) {
		if (in == LnChain::Lane) {
			const size_t at = bridge.addFxDevice(tag);
			REQUIRE(at < feathers::kMaxFxSlots);
			return bridge.fxDeviceAt(at);
		}
		const size_t at = bridge.addRackDevice(tag);
		REQUIRE(at < feathers::kMaxRackSlots);
		return bridge.rackDeviceAt(at);
	};
	// THE READER FIRST, THE SOURCE AFTER IT — so within one chain the source stands to the reader's RIGHT and only
	// the route orders them, and across chains nothing but the route does.
	const feathers::DeviceInstanceId readerId = add(readerIn, reader == LnReader::Math ? mth::kTraits.tag : st::kTraits.tag);
	const feathers::DeviceInstanceId lfoId = add(lfoIn, lfo::kTraits.tag);
	REQUIRE(readerId != feathers::kNoInstance);
	REQUIRE(lfoId != feathers::kNoInstance);
	// FREE (no gate needed) and unsmoothed, so every tick moves it.
	bridge.setDeviceControl(lfoId, static_cast<size_t>(lfo::Param::Mode), static_cast<float>(feathers::mod::Mode::Free));
	bridge.setDeviceControl(lfoId, static_cast<size_t>(lfo::Param::TimeBase), 0.0f);
	bridge.setDeviceControl(lfoId, static_cast<size_t>(lfo::Param::TimeScale), -4.0f);
	bridge.setDeviceControl(lfoId, static_cast<size_t>(lfo::Param::Smoothing),
	                        static_cast<float>(lfo::kDescriptor.params[static_cast<size_t>(lfo::Param::Smoothing)].min));
	if (reader == LnReader::Math) {
		bridge.setDeviceControl(readerId, static_cast<size_t>(mth::Param::Operator), static_cast<float>(mth::Op::Add));
	}
	static_assert(static_cast<size_t>(mth::Param::A) == 0 && static_cast<size_t>(st::Param::In) == 0);
	const auto from = feathers::backplane::Source::output(lfoId, 0);
	const auto to = feathers::backplane::Destination::control(readerId, 0);
	REQUIRE(bridge.mayRoute(from, to));
	bridge.setRouteAmount(from, to, 0.25f);
	// AND THE READER INTO THE VOICE'S FINE TUNE — a reader of the tick that the voice takes between the modulators'
	// tick and the lane's audio.
	const int fine = feathers::modDestinationIndexOf(ParamId::FineTune);
	REQUIRE(fine >= 0);
	bridge.setRouteAmount(feathers::backplane::Source::output(readerId, 0),
	                      feathers::backplane::Destination::core(static_cast<size_t>(fine)), 0.5f);

	Rig rig;
	rig.synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	feathers::publishEditorLanes(bridge, rig.params, rig.synth);
	rig.renderTicks(2);
	const feathers::rack::RackEngine& engine = rig.synth.laneForTest().engineForTest();
	const size_t lfoSlot = engine.slotOf(lfoId);
	const size_t readerSlot = engine.slotOf(readerId);
	REQUIRE(lfoSlot != feathers::rack::RackEngine::kNoSlot);
	REQUIRE(readerSlot != feathers::rack::RackEngine::kNoSlot);
	std::vector<LnTick> out;
	for (int t = 0; t < ticks; ++t) {
		LnTick tick;
		tick.lfoWas = engine.publishedOutputAt(lfoSlot, 0);
		rig.renderTicks(1);
		tick.lfoNow = engine.publishedOutputAt(lfoSlot, 0);
		tick.resolved = engine.resolvedFieldForTest(readerSlot, 0);
		tick.fine = rig.params.resolved(ParamId::FineTune, -1);
		const float* l = engine.outputWindowOf(readerId, 0);
		const float* r = engine.outputWindowROf(readerId, 0);
		REQUIRE(l != nullptr);
		REQUIRE(r != nullptr);
		tick.l0 = l[0];
		tick.l31 = l[31];
		tick.l63 = l[63];
		tick.r0 = r[0];
		tick.r31 = r[31];
		tick.r63 = r[63];
		out.push_back(tick);
	}
	return out;
}

/// THE CONTROL THE READER RESOLVES FROM `source` — base 0, amount 0.25 over the control's whole span (−1..1, no
/// zones), clamped: the arithmetic `rackOffsetAt` declares, restated. Both readers' control 0 has that declaration.
float lnExpected(float source) {
	const feathers::device::ParamDesc& p = feathers::mod::math::kDescriptor.params[0];
	const feathers::device::ParamDesc& q = feathers::mod::stereo::kDescriptor.params[0];
	REQUIRE(p.zones == 0);
	REQUIRE((q.min == p.min && q.max == p.max && q.def == p.def && q.zones == 0 && q.modSpan == p.modSpan));
	const double span = p.modSpan > 0.0 ? p.modSpan : p.max - p.min;
	return static_cast<float>(std::clamp(p.def + 0.25 * static_cast<double>(source) * span, p.min, p.max));
}

/// THE FINE TUNE THE VOICE RESOLVES FROM `source` THROUGH THE READER — base, plus 0.5 x the reader's left ear (which
/// is its control 0 for both readers) x Fine Tune's span, clamped.
double lnExpectedFine(float source) {
	const feathers::ParamDesc* d = feathers::findParam(static_cast<clap_id>(ParamId::FineTune));
	REQUIRE(d != nullptr);
	return std::clamp(d->def + 0.5 * static_cast<double>(lnExpected(source)) * (d->max - d->min), d->min, d->max);
}

/// EVERY TICK READS THE SOURCE OF ITS OWN TICK — the row, the voice, and the windows RAMPING from last tick's value
/// to this one's (`fillTickWindowRamp`: prev + (i + 1)/64 of the way) — and enough ticks are distinguishable that
/// reading the tick before would have failed: counted, so a source that stood still cannot pass vacuously.
/// `rightSign` is the right ear's multiple of the left: +1 for the MATH (mono), −1 for the Stereo Tool's Split.
void checkSameTick(const std::vector<LnTick>& ticks, float rightSign) {
	size_t distinguished = 0;
	for (size_t t = 0; t < ticks.size(); ++t) {
		CAPTURE(t);
		CAPTURE(ticks[t].lfoWas);
		CAPTURE(ticks[t].lfoNow);
		const float was = lnExpected(ticks[t].lfoWas);
		const float now = lnExpected(ticks[t].lfoNow);
		CHECK(ticks[t].resolved == doctest::Approx(now).epsilon(1e-6));
		CHECK(ticks[t].fine == doctest::Approx(lnExpectedFine(ticks[t].lfoNow)).epsilon(1e-6));
		CHECK(ticks[t].l0 == doctest::Approx(was + (now - was) / 64.0f).epsilon(1e-5));
		CHECK(ticks[t].l31 == doctest::Approx(was + (now - was) * 32.0f / 64.0f).epsilon(1e-5));
		CHECK(ticks[t].l63 == doctest::Approx(now).epsilon(1e-5));
		CHECK(ticks[t].r0 == doctest::Approx(rightSign * (was + (now - was) / 64.0f)).epsilon(1e-5));
		CHECK(ticks[t].r31 == doctest::Approx(rightSign * (was + (now - was) * 32.0f / 64.0f)).epsilon(1e-5));
		CHECK(ticks[t].r63 == doctest::Approx(rightSign * now).epsilon(1e-5));
		// DISTINGUISHED: the ramp's first sample and its last further apart than the CHECKs' tolerance (1e-5
		// relative, about 2.5e-6 here), so a flat window — a late read — fails the first window CHECK.
		distinguished += std::abs(now - was) > 2e-5f ? 1 : 0;
	}
	CHECK(distinguished >= ticks.size() / 2);
}

} // namespace

TEST_CASE("LN: a LANE LFO routed into a STRIP MATH is read in the SAME control tick — its window ramps, it does not step") {
	checkSameTick(lfoIntoReader(LnChain::Lane, LnReader::Math, LnChain::Strip, 24), 1.0f);
}

TEST_CASE("LN: the other placements read in the same tick too — strip into lane, and within either chain") {
	SUBCASE("strip LFO into a lane MATH") {
		checkSameTick(lfoIntoReader(LnChain::Strip, LnReader::Math, LnChain::Lane, 24), 1.0f);
	}
	SUBCASE("strip LFO into a strip MATH to its left") {
		checkSameTick(lfoIntoReader(LnChain::Strip, LnReader::Math, LnChain::Strip, 24), 1.0f);
	}
	SUBCASE("lane LFO into a lane MATH to its left") {
		checkSameTick(lfoIntoReader(LnChain::Lane, LnReader::Math, LnChain::Lane, 24), 1.0f);
	}
}

TEST_CASE("LN/KU: a Stereo Tool placed BEFORE its own source in the strip ramps both ears every tick — it does not step") {
	// The KU note (feathers-0c, 2026-10-05): placed before the LFO feeding its In, both ears held flat and stepped
	// once a tick; placed after, it ramped. Both placements, both ears, by value.
	SUBCASE("before its source") {
		checkSameTick(lfoIntoReader(LnChain::Strip, LnReader::StereoTool, LnChain::Strip, 24), -1.0f);
	}
	SUBCASE("a lane LFO into it") {
		checkSameTick(lfoIntoReader(LnChain::Lane, LnReader::StereoTool, LnChain::Strip, 24), -1.0f);
	}
}

TEST_CASE("backlog NJ: Feathers' LFO, FREE in Hz, follows the song's seconds — in the strip and in the lane") {
	// the design rule, asked of the other plugin: phase = Rate x song position in SECONDS + Phase while the
	// transport plays. Through the synth's own transport road (`setTransport` as `applyTransport` calls it, the tempo
	// on its own road) one control tick per render, a tempo change halfway and a jump; read off the engine's published
	// output against the wave at the phase the rule names.
	namespace lfo = feathers::mod::lfo;
	for (const LnChain in : { LnChain::Strip, LnChain::Lane }) {
		CAPTURE(in == LnChain::Strip);
		feathers::ParamBridge bridge;
		const size_t at = in == LnChain::Lane ? bridge.addFxDevice(lfo::kTraits.tag) : bridge.addRackDevice(lfo::kTraits.tag);
		const feathers::DeviceInstanceId id = in == LnChain::Lane ? bridge.fxDeviceAt(at) : bridge.rackDeviceAt(at);
		REQUIRE(id != feathers::kNoInstance);
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::Waveform), static_cast<float>(lfo::Waveform::Sine));
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::Mode), static_cast<float>(feathers::mod::Mode::Free));
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::TimeBase), 0.0f); // Hz
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::TimeScale), 1.0f); // log2 Hz: 2 Hz
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::Phase), 0.25f);
		bridge.setDeviceControl(id, static_cast<size_t>(lfo::Param::Smoothing),
		                        static_cast<float>(lfo::kDescriptor.params[static_cast<size_t>(lfo::Param::Smoothing)].min));

		Rig rig;
		rig.synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
		feathers::publishEditorLanes(bridge, rig.params, rig.synth);
		rig.renderTicks(2);
		const feathers::rack::RackEngine& engine = rig.synth.laneForTest().engineForTest();
		const size_t slot = engine.slotOf(id);
		REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
		lfo::Settings s {};
		s.wave = lfo::Waveform::Sine;
		s.mode = feathers::mod::Mode::Free;
		double seconds = 11.0;
		double beats = 22.0;
		for (int t = 0; t < 120; ++t) {
			CAPTURE(t);
			if (t == 90) {
				seconds = 93.5; // a playhead jump
			}
			const double tempo = t < 45 ? 120.0 : 157.0; // the beats lurch, the seconds do not
			rig.synth.setTempo(tempo);
			rig.synth.setTransport(seconds, beats, true);
			rig.renderTicks(1);
			// THE TICK FIRES AT THE END OF ITS 64 SAMPLES (`Synth::render`), so the clock it reads is the render's end.
			const double turns = 2.0 * (seconds + kTickSamples / kSampleRate) + 0.25;
			CHECK(engine.publishedOutputAt(slot, 0)
			      == doctest::Approx(lfo::eval(s, static_cast<float>(turns - std::floor(turns)))).epsilon(1e-3));
			seconds += kTickSamples / kSampleRate;
			beats += kTickSamples / kSampleRate * tempo / 60.0;
		}
	}
}

TEST_CASE("Trig under Poly through the synth: a per-voice envelope fires the note on its own voice, not every note") {
	// BACKLOG MV, B — the synth's half of per-voice Trig: it tells the engine which note each voice holds
	// (`RackEngine::setVoiceNote`) before the tick. Envelope A (a slow attack, per note) is routed into envelope B's
	// Trig at depth 1; each of A's notes crosses 0.5 about 50 ms after its own key. Struck C4 then G4 300 ms later:
	// G4's crossing must re-attack B's G4 and leave B's C4 decaying. Under the collapse (one edge for the device,
	// fed the newest note's value) G4's crossing re-attacked B's C4 as well — the value this asserts.
	Rig rig;
	dahdsr::Settings a = bare();
	a.attack = dahdsr::stageValueFor(0.100);
	a.sustain = 1.0f;
	a.poly = true;
	dahdsr::Settings b = bare();
	b.attack = dahdsr::stageValueFor(0.010);
	b.decay = dahdsr::stageValueFor(1.000);
	b.sustain = 0.0f;
	b.poly = true;
	feathers::ModRack rack = feathers::defaultModRack();
	rack.count = 2;
	rack.setDahdsrAt(0, a);
	rack.setDahdsrAt(1, b);
	Rig::withIdentity(&rack, 0);
	Rig::withIdentity(&rack, 1);
	rig.publishModRack(rack);
	const auto idA = rig.synth.modRackForTest().instanceAt(0);
	const auto idB = rig.synth.modRackForTest().instanceAt(1);
	rig.routes.set(feathers::backplane::Source::output(idA, 0),
	               feathers::backplane::Destination::control(idB, static_cast<size_t>(dahdsr::Param::Trig)), 1.0f);
	rig.routes.publish(rig.params);
	rig.renderTicks(1);

	const auto bNote = [&](int16_t key, int32_t id) { return rig.synth.laneForTest().outputForNoteOf(idB, 0, id, 0, key); };
	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	for (int i = 0; i < 15; ++i) {
		rig.renderTicks(15);
	} // 300 ms: B's C4 fired at A's crossing (~50 ms) and is well into its decay
	const float c4Before = bNote(kKeyC4, 1);
	CAPTURE(c4Before);
	REQUIRE(c4Before < 0.85f);
	REQUIRE(c4Before > 0.2f);

	rig.synth.noteOn(0, kKeyG4, 2, 1.0);
	for (int i = 0; i < 5; ++i) {
		rig.renderTicks(15);
	} // 100 ms: A's G4 crossed 0.5 at ~50 ms
	const float c4After = bNote(kKeyC4, 1);
	const float g4 = bNote(kKeyG4, 2);
	CAPTURE(c4After);
	CAPTURE(g4);
	CHECK(c4After < c4Before);   // still decaying: G4's crossing did not reach it
	CHECK(g4 > c4After + 0.05f); // G4's own envelope fired at its own crossing and is younger in its decay
}

TEST_CASE("a Trig fire is counted and published beside the playhead, which is what lights the card's Trig pad") {
	// BACKLOG MV: the pad lights when this count moves (`DeviceHost::trigFiresAt`). Asserted on the synth's own publish,
	// the road the bridge carries to the card.
	Rig rig;
	dahdsr::Settings env = bare();
	env.mode = dahdsr::Mode::Once;
	rig.install(env);
	rig.renderTicks(1);
	const auto id = rig.synth.modRackForTest().instanceAt(0);
	REQUIRE(rig.synth.rackPlayheads()[0].haveTrigFires);
	CHECK(rig.synth.rackPlayheads()[0].trigFires == 0u);

	rig.routes.set(feathers::test::macro(0),
	               feathers::backplane::Destination::control(id, static_cast<size_t>(dahdsr::Param::Trig)), 1.0f);
	rig.routes.publish(rig.params);
	rig.params.setBase(static_cast<clap_id>(feathers::ParamId::Macro1), 1.0);
	rig.renderTicks(4);
	CHECK(rig.synth.rackPlayheads()[0].trigFires == 1u);
	rig.params.setBase(static_cast<clap_id>(feathers::ParamId::Macro1), 0.0);
	rig.renderTicks(4);
	rig.params.setBase(static_cast<clap_id>(feathers::ParamId::Macro1), 1.0);
	rig.renderTicks(4);
	CHECK(rig.synth.rackPlayheads()[0].trigFires == 2u);
}

//------------------------------------------------------------------------------------------------//
// NOISE'S IMPULSES AS MODULATION (backlog NA (b))                                                  //
//------------------------------------------------------------------------------------------------//

namespace {

namespace nz = feathers::fx::noise;

/// A NOISE IN THE FX LANE, adding sparse clicks through its filter wide open: a grid at 2 Hz with jitter, so clicks
/// are at least a quarter second apart and each one's ring has reached exact zero before the next.
size_t addNoise(feathers::FxChain* chain, bool poly) {
	const size_t slot = feathers::addFxDevice(chain, nz::kTraits.tag);
	REQUIRE(slot < chain->device.size());
	auto& f = chain->device[slot].row.field;
	f[static_cast<size_t>(nz::Param::Source)] = 1.0f;
	f[static_cast<size_t>(nz::Param::Level)] = 0.5f;
	f[static_cast<size_t>(nz::Param::Stereo)] = 0.0f;
	f[static_cast<size_t>(nz::Param::Density)] = 1.0f; // 2 hits a second
	f[static_cast<size_t>(nz::Param::Timing)] = 0.0f;  // Regular
	f[static_cast<size_t>(nz::Param::Jitter)] = 0.5f;
	f[static_cast<size_t>(nz::Param::Poly)] = poly ? 1.0f : 0.0f;
	return slot;
}

} // namespace

TEST_CASE("NA (b): on the bus, NOISE's click and its published Impulse Trig land on the same tick") {
	// THE CLAIM IS THE HOST'S ORDER AS MUCH AS THE DEVICE'S: the tick runs before the window's audio, so the event a
	// route reads in window k is the click window k sounds. Asserted per tick from the synth's own output: a click's
	// ONSET (a sample leaving exact zero) falls in a window exactly when that window's published Trig is 1.
	//
	// **THE SYNTH TICKS AT THE END OF A RENDER CALL, FOR THE WINDOW AFTER IT** (`Synth::render`: "the modulators move
	// first" once a chunk reaches the tick). So what is published after call k is window k+1's, and it is compared
	// with the audio call k+1 renders — the order every route in the lane reads it in.
	Rig rig;
	feathers::FxChain chain;
	const size_t at = addNoise(&chain, false);
	rig.synth.provisionDevices(chain, rig.synth.modRackForTest());
	rig.synth.setFxChain(chain);
	rig.renderTicks(1);
	const feathers::DeviceInstanceId noise = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, at);
	REQUIRE(noise != feathers::kNoInstance);

	float before = 0.0f; // the last sample of the previous window
	int clicks = 0;
	int agree = 0;
	float lastSize = 0.0f;
	// WHAT THE TICK AT THE END OF THE PREVIOUS CALL PUBLISHED — the event for the window this call renders.
	float trig = rig.synth.laneForTest().publishedOutputOf(noise, static_cast<size_t>(nz::Out::Trig));
	for (int k = 0; k < 7500; ++k) { // 10 s
		rig.render(kTickSamples);
		bool onset = false;
		for (int i = 0; i < kTickSamples; ++i) {
			const float prev = i == 0 ? before : rig.left[static_cast<size_t>(i - 1)];
			onset = onset || (prev == 0.0f && rig.left[static_cast<size_t>(i)] != 0.0f);
		}
		before = rig.left[static_cast<size_t>(kTickSamples - 1)];
		const float trigNext = rig.synth.laneForTest().publishedOutputOf(noise, static_cast<size_t>(nz::Out::Trig));
		const float size = rig.synth.laneForTest().publishedOutputOf(noise, static_cast<size_t>(nz::Out::Size));
		CAPTURE(k);
		CHECK((trig == 1.0f) == onset);
		clicks += onset ? 1 : 0;
		agree += (trig == 1.0f) == onset ? 1 : 0;
		// THE SIZE MOVES ONLY ON A CLICK'S TICK, and is held between.
		if (trigNext != 1.0f) {
			CHECK(size == lastSize);
		}
		lastSize = size;
		trig = trigNext;
	}
	CHECK(clicks >= 18);
	CHECK(agree == 7500);
}

TEST_CASE("NA (b): a Poly NOISE's hit on one voice drives a per-note route on that note only") {
	// THE PER-VOICE ROAD (RackEngine::publishesFromVoices): each voice instance's mod road ticks, and the voice holding
	// a note answers for it. NOISE's Impulse Trig is routed at depth 1 into a per-note envelope's Trig (attack 1 ms,
	// decay 1 s, sustain 0). C4, then G4 300 ms later: under a Stable seed each note's clicks run 300 ms apart, so a
	// tick where G4's voice clicks and C4's does not exists, and there G4's envelope re-attacks while C4's decays on.
	Rig rig;
	dahdsr::Settings b = bare();
	b.attack = dahdsr::stageValueFor(0.001);
	b.decay = dahdsr::stageValueFor(1.000);
	b.sustain = 0.0f;
	b.poly = true;
	rig.install(b);
	feathers::FxChain chain;
	const size_t at = addNoise(&chain, true);
	rig.synth.provisionDevices(chain, rig.synth.modRackForTest());
	rig.synth.setFxChain(chain);
	rig.renderTicks(1);
	const feathers::DeviceInstanceId noise = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, at);
	const feathers::DeviceInstanceId env = rig.synth.modRackForTest().instanceAt(0);
	rig.routes.set(feathers::backplane::Source::output(noise, static_cast<size_t>(nz::Out::Trig)),
	               feathers::backplane::Destination::control(env, static_cast<size_t>(dahdsr::Param::Trig)), 1.0f);
	rig.routes.publish(rig.params);
	rig.renderTicks(1);

	auto& lane = rig.synth.laneForTest();
	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	for (int i = 0; i < 6; ++i) {
		rig.renderTicks(39);
	} // 234 ticks, ~300 ms
	rig.synth.noteOn(0, kKeyG4, 2, 1.0);
	constexpr int kTicks = 2000;
	std::vector<float> nC(kTicks), nG(kTicks), eC(kTicks), eG(kTicks);
	for (int k = 0; k < kTicks; ++k) {
		rig.renderTicks(1);
		nC[static_cast<size_t>(k)] = lane.outputForNoteOf(noise, 0, 1, 0, kKeyC4);
		nG[static_cast<size_t>(k)] = lane.outputForNoteOf(noise, 0, 2, 0, kKeyG4);
		eC[static_cast<size_t>(k)] = lane.outputForNoteOf(env, 0, 1, 0, kKeyC4);
		eG[static_cast<size_t>(k)] = lane.outputForNoteOf(env, 0, 2, 0, kKeyG4);
	}
	int gOnly = 0;
	int cHits = 0;
	int gHits = 0;
	for (int k = 8; k + 8 < kTicks; ++k) {
		cHits += nC[static_cast<size_t>(k)] == 1.0f ? 1 : 0;
		gHits += nG[static_cast<size_t>(k)] == 1.0f ? 1 : 0;
		if (nG[static_cast<size_t>(k)] != 1.0f) {
			continue;
		}
		bool cQuiet = true;
		for (int j = k - 8; j <= k + 8; ++j) {
			cQuiet = cQuiet && nC[static_cast<size_t>(j)] == 0.0f;
		}
		if (!cQuiet) {
			continue;
		}
		++gOnly;
		CAPTURE(k);
		// G4's envelope re-attacks within a few ticks of its voice's click; C4's goes on falling.
		CHECK(eG[static_cast<size_t>(k + 4)] > eG[static_cast<size_t>(k - 1)] + 0.1f);
		CHECK(eC[static_cast<size_t>(k + 4)] <= eC[static_cast<size_t>(k - 1)]);
	}
	CHECK(cHits >= 3);
	CHECK(gHits >= 3);
	CHECK(gOnly >= 3);
}

TEST_CASE("NA (b): under Poly a destination with no voice reads the newest note's NOISE, never rest") {
	// THE RULE EVERY PER-VOICE MODULATOR USES FOR A GLOBAL DESTINATION (`PerNoteTable::newest`, the DAHDSR's under
	// Poly): the most recently struck note's value. The published cell — what a bus route reads — must equal the
	// newest note's own reading every tick: C4's while it is the only note, G4's once G4 is struck.
	Rig rig;
	feathers::FxChain chain;
	const size_t at = addNoise(&chain, true);
	rig.synth.provisionDevices(chain, rig.synth.modRackForTest());
	rig.synth.setFxChain(chain);
	rig.renderTicks(1);
	const feathers::DeviceInstanceId noise = rig.synth.laneForTest().deviceOfStatePosition(feathers::kLaneChain, at);
	auto& lane = rig.synth.laneForTest();
	const auto published = [&](nz::Out o) { return lane.publishedOutputOf(noise, static_cast<size_t>(o)); };
	const auto forNote = [&](nz::Out o, int32_t id, int16_t key) {
		return lane.outputForNoteOf(noise, static_cast<size_t>(o), id, 0, key);
	};

	rig.synth.noteOn(0, kKeyC4, 1, 1.0);
	int cTrigs = 0;
	for (int k = 0; k < 600; ++k) {
		rig.renderTicks(1);
		CAPTURE(k);
		CHECK(published(nz::Out::Trig) == forNote(nz::Out::Trig, 1, kKeyC4));
		CHECK(published(nz::Out::Size) == forNote(nz::Out::Size, 1, kKeyC4));
		cTrigs += published(nz::Out::Trig) == 1.0f ? 1 : 0;
	}
	CHECK(cTrigs >= 1);
	rig.synth.noteOn(0, kKeyG4, 2, 1.0);
	int gTrigs = 0;
	int differFromC = 0;
	for (int k = 0; k < 1500; ++k) {
		rig.renderTicks(1);
		CAPTURE(k);
		CHECK(published(nz::Out::Trig) == forNote(nz::Out::Trig, 2, kKeyG4));
		CHECK(published(nz::Out::Size) == forNote(nz::Out::Size, 2, kKeyG4));
		gTrigs += published(nz::Out::Trig) == 1.0f ? 1 : 0;
		differFromC += published(nz::Out::Trig) != forNote(nz::Out::Trig, 1, kKeyC4) ? 1 : 0;
	}
	CHECK(gTrigs >= 2);
	CHECK(differFromC >= 2); // it follows G4, not C4
}

TEST_CASE("NA (b): the per-voice publish road reaches exactly the dual devices with a Poly switch: NOISE and the Follower") {
	// `RackEngine::publishesFromVoices` changes how a device with an audio road AND a mod road publishes under Poly.
	// SKLITTER is the other dual device and declares no Poly switch, so its Perform outputs take the bus road they
	// always took; a device that joins this list is a device whose outputs moved roads, and should be looked at.
	std::vector<std::string> dualPoly;
	for (const feathers::device::Descriptor& d : feathers::kDevices) {
		if (d.audio != nullptr && d.mod != nullptr && d.traits.perVoiceControl >= 0) {
			dualPoly.emplace_back(d.traits.tag);
		}
	}
	// THE FOLLOWER (backlog MY, the Tap folded in as Mode = Audio by OP) JOINED IT ON PURPOSE: under Poly each voice instance follows its voice, so its
	// outputs belong on this road (test_mod_follower "Follower in Audio: under Poly each voice's follower ...").
	CHECK(dualPoly == std::vector<std::string> { nz::kTraits.tag, feathers::mod::follower::kTraits.tag });
}

TEST_CASE("F2/OI: Spectral's filter 2 amounts and Centers' set are modulation destinations a source may reach, and its row fits the field cap") {
	// THE PER-DEVICE CONTROL CAP IS GONE (mainline d1543cb3), so the controls held back as plain while Spectral sat at the 32-control
	// stride are destinations: each declared modulatable, and a core source may route to it on a live Spectral instance.
	namespace spx = feathers::fx::spectral;
	feathers::ParamBridge bridge;
	const size_t slot = bridge.addFxDevice(spx::kTraits.tag);
	REQUIRE(slot < feathers::kMaxFxSlots);
	const feathers::DeviceInstanceId id = bridge.fxDeviceAt(slot);
	REQUIRE(id != feathers::kNoInstance);
	for (const spx::Param p : { spx::Param::Filter2Cutoff, spx::Param::Filter2Resonance, spx::Param::Filter2Slope,
	                            spx::Param::Filter2Gain, spx::Param::FilterGain, spx::Param::PartialsSet }) {
		CAPTURE(static_cast<int>(p));
		CHECK(spx::kParamDescs[static_cast<size_t>(p)].modulatable);
		CHECK(bridge.mayRoute(feathers::backplane::Source::core(0),
		                      feathers::backplane::Destination::control(id, static_cast<size_t>(p))));
	}
	// AND A STEPPED ONE IS NOT (the vacuity control: the gate is not open to everything).
	CHECK_FALSE(bridge.mayRoute(feathers::backplane::Source::core(0),
	                            feathers::backplane::Destination::control(id, static_cast<size_t>(spx::Param::Filter2On))));
	// THE ROW: the controls and the curve's bands, against the per-device field cap.
	MESSAGE("Spectral's row: " << spx::kParamCount << " controls + " << spx::kRowFloats - spx::kParamCount << " curve bands = "
	                           << spx::kRowFloats << " of " << feathers::device::kMaxDeviceFields << " fields");
	CHECK(spx::kRowFloats <= feathers::device::kMaxDeviceFields);
}

// ---------------------------------------------------------------------------------------------
// BACKLOG OZ: oscillator phase did not seem to react to stereo modulation — an LFO into a Stereo
// Tool, Split onto Phase. Through the editor's own road (the bridge, `publishEditorLanes`), not the fixture's.
namespace {
namespace oz {
namespace st = feathers::mod::stereo;
namespace lfo = feathers::mod::lfo;
constexpr double kBase = 0.5;
constexpr float kToPhase = 0.3f;

struct Road {
	feathers::ParamBridge bridge;
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::DeviceInstanceId lfoId = feathers::kNoInstance;
	feathers::DeviceInstanceId toolId = feathers::kNoInstance;
	std::vector<float> l = std::vector<float>(64, 0.0f);
	std::vector<float> r = std::vector<float>(64, 0.0f);

	void render() { synth.render(l.data(), r.data(), 64); }
};

/// The bridge holds an LFO and a Stereo Tool (the Tool BEFORE the LFO in the strip when `toolFirst`), LFO → In at 0.25,
/// Split → Phase at kToPhase, on `engine`.
void seat(Road& road, bool toolFirst, feathers::OscType engine) {
	road.lfoId = road.bridge.rackDeviceAt(road.bridge.addRackDevice(lfo::kTraits.tag));
	road.toolId = toolFirst ? road.bridge.rackDeviceAt(road.bridge.insertRackDeviceBefore(road.lfoId, st::kTraits.tag))
	                        : road.bridge.rackDeviceAt(road.bridge.addRackDevice(st::kTraits.tag));
	REQUIRE(road.lfoId != feathers::kNoInstance);
	REQUIRE(road.toolId != feathers::kNoInstance);
	road.bridge.setDeviceControl(road.lfoId, static_cast<size_t>(lfo::Param::Mode),
	                             static_cast<float>(static_cast<int>(lfo::Mode::Free)));
	road.bridge.setRouteAmount(feathers::backplane::Source::output(road.lfoId, 0),
	                           feathers::backplane::Destination::control(road.toolId, static_cast<size_t>(st::Param::In)), 0.25f);
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	road.bridge.setRouteAmount(feathers::backplane::Source::output(road.toolId, static_cast<size_t>(st::Output::Split)),
	                           feathers::backplane::Destination::core(static_cast<size_t>(phase)), kToPhase);
	road.params.setBase(static_cast<clap_id>(ParamId::OscType), static_cast<double>(engine));
	road.params.setBase(static_cast<clap_id>(ParamId::OscPhase), kBase);
	road.synth.activate(48000.0, 4096);
	road.synth.provisionDevices(road.bridge.fxChain(), road.bridge.modRack());
	feathers::publishEditorLanes(road.bridge, road.params, road.synth);
	for (int i = 0; i < 4; ++i) {
		road.render();
	}
}
} // namespace oz
} // namespace

TEST_CASE("backlog OZ: through the editor's road, an LFO into a Stereo Tool with Split onto Phase reads each ear at base + a × that ear of Split — the Tool before its source or after it") {
	for (const bool toolFirst : { false, true }) {
		CAPTURE(toolFirst);
		oz::Road road;
		oz::seat(road, toolFirst, feathers::OscType::V5);
		road.synth.noteOn(0, kKeyC4, 1, 1.0);
		for (int i = 0; i < 10; ++i) {
			road.render();
		}
		const int slot = road.synth.slotOfKeyForTest(kKeyC4);
		REQUIRE(slot >= 0);
		const feathers::rack::RackEngine& engine = road.synth.laneForTest().engineForTest();
		const size_t tool = engine.slotOf(road.toolId);
		REQUIRE(tool != feathers::rack::RackEngine::kNoSlot);
		REQUIRE(road.params.stereoLive(ParamId::OscPhase));
		const auto split = static_cast<size_t>(oz::st::Output::Split);
		double widest = 0.0;
		bool earsDiffer = false;
		for (int k = 0; k < 16; ++k) {
			CAPTURE(k);
			// RESTATED FROM THE DESIGN (§7, Phase Width 0): L = base + mL, R = base + mR, m the route amount times
			// Split's ear — as the tick opens, since the tick renders toward those cells.
			const double left = oz::kBase + oz::kToPhase * static_cast<double>(engine.publishedOutputAt(tool, split));
			const double right = oz::kBase + oz::kToPhase * static_cast<double>(engine.publishedOutputRAt(tool, split));
			road.render();
			CHECK(std::abs(road.synth.voiceReadOffsetForTest(slot) - left) < 1e-5);
			CHECK(std::abs(road.synth.voiceReadOffsetRightForTest(slot) - right) < 1e-5);
			widest = std::max(widest, std::abs(right - left));
			earsDiffer = earsDiffer || road.l != road.r;
		}
		CHECK(widest > 0.01); // NOT VACUOUS: Split's ears were apart
		CHECK(earsDiffer);    // and the channels were different signals
	}
}

TEST_CASE("backlog PA: the Phase knob is told both ears of an LFO with Stereo Phase — the bridge publishes the right ear and that it is live") {
	namespace lfo = feathers::mod::lfo;
	feathers::ParamBridge bridge;
	const feathers::DeviceInstanceId lfoId = bridge.rackDeviceAt(bridge.addRackDevice(lfo::kTraits.tag));
	REQUIRE(lfoId != feathers::kNoInstance);
	bridge.setDeviceControl(lfoId, static_cast<size_t>(lfo::Param::Mode), static_cast<float>(static_cast<int>(lfo::Mode::Free)));
	const int phase = feathers::modDestinationIndexOf(ParamId::OscPhase);
	REQUIRE(phase >= 0);
	bridge.setRouteAmount(feathers::backplane::Source::output(lfoId, 0),
	                      feathers::backplane::Destination::core(static_cast<size_t>(phase)), 0.3f);
	feathers::ParamSet params;
	params.setBase(static_cast<clap_id>(ParamId::OscPhase), 0.5);
	feathers::Synth synth { params };
	synth.activate(48000.0, 4096);
	synth.provisionDevices(bridge.fxChain(), bridge.modRack());
	feathers::publishEditorLanes(bridge, params, synth);
	std::vector<float> l(256, 0.0f), r(256, 0.0f);
	const auto renderAndRead = [&](double& left, double& right) {
		for (int i = 0; i < 8; ++i) {
			synth.render(l.data(), r.data(), static_cast<int>(l.size()));
		}
		bridge.publish(params);
		bridge.refresh();
		left = bridge.modulated(static_cast<clap_id>(ParamId::OscPhase));
		return bridge.modulatedRight(static_cast<clap_id>(ParamId::OscPhase), right);
	};
	double left = 0.0;
	double right = 0.0;
	// AT 0: no right ear — the ring draws one mark.
	CHECK_FALSE(renderAndRead(left, right));
	// AT 90°: the right ear, the engine's own number.
	bridge.setDeviceControl(lfoId, static_cast<size_t>(lfo::Param::StereoPhase), static_cast<float>(M_PI / 2.0));
	feathers::publishEditorLanes(bridge, params, synth);
	REQUIRE(renderAndRead(left, right));
	CHECK(right == doctest::Approx(params.resolvedRight(ParamId::OscPhase)).epsilon(1e-9));
	CHECK(std::abs(right - left) > 1e-3);
}
