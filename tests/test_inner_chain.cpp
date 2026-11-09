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

#include "backplane/axes.h"
#include "device/tick_window.h"
#include "rack/inner_chain.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// **THE CHAIN A DEVICE HOLDS, DRIVEN DIRECTLY.** `rack::InnerChain` is the extraction the bolt-on
// chains design's gap G1 asks for: engine + bridge lifecycle, catalogue adoption,
// the body codec, the drain placement, the tick-window chunking and the wet/dry ramp, packaged so the
// SECOND device to want an inner chain forwards to it rather than copying `rack_device.cpp`.
//
// **EVERY CLAIM HERE IS A VALUE, NOT A DIFFERENCE.** CLAUDE.md records four occasions where a
// comparison between two renders passed while the thing under test was deleted, because a difference
// check passes whenever ANYTHING changes — and every one was a PARTIAL effect surviving where a total
// one would have been caught. So: the mix is asserted against an arithmetic restatement of the
// intended path, the chunking against a COUNT of inner blocks (which a comparison of audio cannot
// see at all, since a chain holding a linear device sounds identical however it is cut), and the
// publish against a mirror value that only a publish can move.

using namespace feathers;
using rack::InnerChain;
using rack::RackEngine;

namespace {

//------------------------------------------------------------------------------------------------//
// THE PROBE — a device whose every effect is a number a case can read.                              //
//------------------------------------------------------------------------------------------------//

/// y = x * gain + offset. `processed` counts the calls the engine made, which is how a case reads the
/// chunking without asking the audio, and `frames` remembers the longest piece it was handed.
struct Probe {
	float gain = 1.0f;
	float offset = 0.0f;
	int processed = 0;
	int longest = 0;
	double rate = 0.0;
	int maxFrames = 0; // what `prepare` was told the longest block is (backlog GO)
	int resets = 0;
};

Probe& probeAt(void* storage) { return *static_cast<Probe*>(storage); }

void run(Probe& p, float* x, int frames) {
	for (int i = 0; i < frames; ++i) {
		x[i] = x[i] * p.gain + p.offset;
	}
}

constexpr device::AudioOps kProbeOps {
	.stateBytes = sizeof(Probe),
	.stateAlign = alignof(Probe),
	.construct = [](void* s) { new (s) Probe {}; },
	.destruct = [](void* s) { static_cast<Probe*>(s)->~Probe(); },
	.prepare =
	    [](void* s, double rate, int maxFrames) {
		    probeAt(s).rate = rate;
		    probeAt(s).maxFrames = maxFrames;
	    },
	.reset = [](void* s) { ++probeAt(s).resets; },
	.setSlot = nullptr,
	.setFields =
	    [](void* s, const float* f, size_t n) {
		    Probe& p = probeAt(s);
		    p.gain = n > 0 ? f[0] : 1.0f;
		    p.offset = n > 1 ? f[1] : 0.0f;
	    },
	.processStereo =
	    [](void* s, float* l, float* r, int frames) {
		    Probe& p = probeAt(s);
		    run(p, l, frames);
		    run(p, r, frames);
		    ++p.processed;
		    p.longest = frames > p.longest ? frames : p.longest;
	    },
};

constexpr std::array<device::ParamDesc, 2> kProbeParams { {
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {} },
	{ "offset", "Offset", -100.0, 100.0, 0.0, false, device::Unit::Number, {} },
} };

const device::Descriptor kProbeDesc {
	.traits = { .tag = "prbC", .name = "Probe C", .hasCard = false, .hasExpanded = false,
	            .perVoiceControl = -1 },
	.params = kProbeParams,
	.outputs = {},
	.audio = &kProbeOps,
};

/// THE SAME DEVICE WITH A PER-VOICE ROAD — its control 0 doubles as the Poly switch, so a case can
/// exercise the two-stage road without a second class.
constexpr std::array<device::ParamDesc, 3> kPolyParams { {
	{ "poly", "Poly", 0.0, 1.0, 1.0, true, device::Unit::Number, {}, /*modulatable=*/true },
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {} },
	{ "offset", "Offset", -100.0, 100.0, 0.0, false, device::Unit::Number, {} },
} };

struct PolyProbe {
	float poly = 1.0f;
	float gain = 1.0f;
	int processed = 0;
	int longest = 0; // the longest piece the per-voice road handed it (backlog GO)
};

PolyProbe& polyAt(void* storage) { return *static_cast<PolyProbe*>(storage); }

constexpr device::AudioOps kPolyOps {
	.stateBytes = sizeof(PolyProbe),
	.stateAlign = alignof(PolyProbe),
	.construct = [](void* s) { new (s) PolyProbe {}; },
	.destruct = [](void* s) { static_cast<PolyProbe*>(s)->~PolyProbe(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setSlot = nullptr,
	.setFields =
	    [](void* s, const float* f, size_t n) {
		    PolyProbe& p = polyAt(s);
		    p.poly = n > 0 ? f[0] : 1.0f;
		    p.gain = n > 1 ? f[1] : 1.0f;
	    },
	.processStereo =
	    [](void* s, float* l, float* r, int frames) {
		    PolyProbe& p = polyAt(s);
		    for (int i = 0; i < frames; ++i) {
			    l[i] *= p.gain;
			    r[i] *= p.gain;
		    }
		    ++p.processed;
		    p.longest = frames > p.longest ? frames : p.longest;
	    },
};

const device::Descriptor kPolyDesc {
	.traits = { .tag = "prbP", .name = "Probe P", .hasCard = false, .hasExpanded = false,
	            .perVoiceControl = 0 },
	.params = kPolyParams,
	.outputs = {},
	.audio = &kPolyOps,
};

const device::Descriptor* findProbe(void*, const char* tag) {
	if (device::sameTag(tag, kProbeDesc.traits.tag)) {
		return &kProbeDesc;
	}
	if (device::sameTag(tag, kPolyDesc.traits.tag)) {
		return &kPolyDesc;
	}
	return nullptr;
}

const device::Catalogue kProbes { nullptr, &findProbe };

constexpr double kRate = 48000.0;
/// 5 ms at `kRate`, restated here rather than read from the unit — the ramp length is the claim.
constexpr int kRamp = 240;

/// A CHAIN READY TO RUN, holding one Probe at `gain`. Returns the probe's slot.
///
/// **THROUGH THE BRIDGE AND NOT THROUGH THE ENGINE, and the first draft of this helper did the
/// opposite and was wrong in a way worth recording.** Once a chain has adopted a catalogue the RECORD
/// is what the chain is; the drain converges the engine ONTO it. A device added straight to the engine
/// is not in the record, so the very next drain — which `process` performs — takes it out again, and
/// every case reads an untouched buffer while looking like a chain that simply does not work.
size_t seatProbe(InnerChain& chain, float gain, float offset = 0.0f) {
	rack::RackBridge* b = chain.bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kProbeDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, 0, gain);
	b->setField(id, 1, offset);
	b->drain();
	const size_t lane = chain.engine().slotOf(id);
	REQUIRE(lane != RackEngine::kNoSlot);
	return lane;
}

Probe& probeIn(InnerChain& chain, size_t lane) {
	return probeAt(chain.engine().slotAtLane(lane).storage.get());
}

} // namespace

//================================================================================================//
// THE CHAIN'S OWN MIX — decision D2                                                               //
//================================================================================================//

TEST_CASE("the chain's own mix is a value: out = in·(mix·chainGain + (1 − mix)), computed apart from "
          "the code that produced it") {
	// **DECISION D2.** SKLITTER blends its dry and wet per sample and IN PLACE (`Sklitter::blendAt`),
	// so a chain on the wet path alone would need a wet scratch buffer and a rewrite of all four
	// renderers' write-backs. "After the blend" is a small insertion, "post" is the honest reading of
	// the name, and a chain that carries its OWN mix costs nothing structural — it is this.
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	seatProbe(chain, 2.0f);
	chain.setMix(0.25f);

	// SETTLED BEFORE THE MEASUREMENT: the first call primes the ramp, and a coefficient still moving
	// is not the coefficient under test. `blendForTest` is what says it has arrived.
	std::array<float, 64> warmL {};
	std::array<float, 64> warmR {};
	chain.process(warmL.data(), warmR.data(), 64);
	REQUIRE(chain.blendForTest().wet == doctest::Approx(0.25f));
	REQUIRE(chain.blendForTest().dry == doctest::Approx(0.75f));

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	l.fill(0.5f);
	r.fill(-0.25f);
	chain.process(l.data(), r.data(), 64);

	// THE REFERENCE, RESTATED INDEPENDENTLY — not "it differs from the bypassed run".
	const float expectedL = 0.5f * (0.25f * 2.0f + 0.75f);
	const float expectedR = -0.25f * (0.25f * 2.0f + 0.75f);
	CHECK(l[0] == doctest::Approx(expectedL));
	CHECK(l[63] == doctest::Approx(expectedL));
	CHECK(r[0] == doctest::Approx(expectedR));
	// AND THE DIFFERENCE CHECK ONLY AS A SECOND ASSERTION, proving the first is not vacuous: at
	// mix 0.25 through a gain of 2 the answer is 1.25x, which is neither the input nor the chain's
	// output, so neither a dead chain nor an unmixed one could have produced it.
	CHECK(l[0] != doctest::Approx(0.5f));
	CHECK(l[0] != doctest::Approx(1.0f));
}

TEST_CASE("mix 0 is the input bit for bit, and mix 1 is the chain's output bit for bit") {
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	seatProbe(chain, 3.0f);

	std::array<float, 32> l {};
	std::array<float, 32> r {};
	chain.setMix(0.0f);
	l.fill(0.125f);
	r.fill(0.125f);
	chain.process(l.data(), r.data(), 32);
	// **BIT FOR BIT**, not Approx: a dry path that is `x·1 + x·0` must be the identity, and 0.125 is
	// exact in binary so a rounding difference would be a real one.
	CHECK(l[31] == 0.125f);

	chain.setMix(1.0f);
	// The mix change ramps over 5 ms; run past it before reading.
	std::array<float, 512> ramp {};
	std::array<float, 512> rampR {};
	chain.process(ramp.data(), rampR.data(), 512);
	REQUIRE(chain.blendForTest().wet == doctest::Approx(1.0f));
	REQUIRE(chain.blendForTest().dry == doctest::Approx(0.0f));
	l.fill(0.125f);
	r.fill(0.125f);
	chain.process(l.data(), r.data(), 32);
	CHECK(l[31] == doctest::Approx(0.375f));
}

//================================================================================================//
// TRAP T4 — THE TICK-WINDOW CHUNKING                                                              //
//================================================================================================//

TEST_CASE("a host block is cut on the tick window, so the inner modulators run on the 64-sample grid "
          "— counted, because a linear chain sounds identical however it is cut") {
	// **TRAP T4.** A naive `engine.processBus(l, r, frames)` once per host block gives the inner
	// chain's modulators ONE tick per host block: at 512 frames, eight times too slow and
	// host-buffer-dependent, which is the class of bug this repo calls "the host changing the sound".
	//
	// **AND THE AUDIO CANNOT SEE IT.** A chain holding a gain renders the same samples whether it is
	// cut into one piece or eight, so a difference check over the output is blind here by
	// construction. The count is the claim.
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	const size_t lane = seatProbe(chain, 1.0f);

	std::array<float, 512> l {};
	std::array<float, 512> r {};
	chain.process(l.data(), r.data(), 512);

	CHECK(chain.blocksBegunForTest() == 512 / device::kTickWindow); // 8, not 1
	CHECK(probeIn(chain, lane).processed == 512 / device::kTickWindow);
	// NO PIECE LONGER THAN THE WINDOW — the count alone would also be satisfied by seven windows and
	// one long remainder.
	CHECK(probeIn(chain, lane).longest == device::kTickWindow);
	// AND THE PHASE CLOSED EXACTLY: 512 is eight whole windows.
	CHECK(chain.tickPhaseForTest() == 0);
}

TEST_CASE("the tick phase is carried ACROSS host blocks, so a partition that does not divide the "
          "window still puts every tick on one grid") {
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	seatProbe(chain, 1.0f);

	std::array<float, 100> l {};
	std::array<float, 100> r {};
	chain.process(l.data(), r.data(), 100);
	// 100 = 64 + 36: two chunks, and the phase stands at 36 for the NEXT block to finish the window.
	CHECK(chain.blocksBegunForTest() == 2);
	CHECK(chain.tickPhaseForTest() == 36);

	chain.process(l.data(), r.data(), 100);
	// 36 + 28 closes the window, then 64 closes another, then 8 remains.
	CHECK(chain.tickPhaseForTest() == (100 + 100) % device::kTickWindow);
	CHECK(chain.blocksBegunForTest() == 5);
}

//================================================================================================//
// TRAP T3 — THE DRAIN AND THE PUBLISH                                                             //
//================================================================================================//

TEST_CASE("EVERY return path publishes — a chain handed no frames at all still moves the mirror the "
          "editor reads") {
	// **TRAP T3.** `rack_device.cpp`'s publish is an RAII destructor *"so every return path
	// publishes"*, and a holder with early returns that published at the bottom would freeze the inner
	// editor in exactly the states a player reaches (bypassed, idle, recording). `Sklitter` has five
	// early returns in its own process.
	//
	// **THE MIRROR IS THE OBSERVABLE, AND ONLY A PUBLISH MOVES IT.** `RackBridge::publish` copies
	// `engine.macro(i)`; `publishCoreSource` writes the engine and not the mirror.
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	REQUIRE(chain.bridge() != nullptr);

	chain.publishCoreSource(0, 0.75f);
	REQUIRE(chain.engine().macro(0) == doctest::Approx(0.75f));
	REQUIRE(chain.bridge()->macro(0) == doctest::Approx(0.0f)); // not yet said

	chain.process(nullptr, nullptr, 0); // the frames==0 return path
	CHECK(chain.bridge()->macro(0) == doctest::Approx(0.75f));

	// AND THE BYPASSED-AND-SETTLED RETURN PATH, which is a different `return` statement.
	chain.setEnabled(false);
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	chain.process(l.data(), r.data(), 64); // finishes the fade
	chain.process(l.data(), r.data(), 64); // now the true-pass return
	REQUIRE(chain.blendForTest().dry == doctest::Approx(1.0f));
	chain.publishCoreSource(0, 0.25f);
	chain.process(l.data(), r.data(), 64);
	CHECK(chain.bridge()->macro(0) == doctest::Approx(0.25f));
}

TEST_CASE("a host with no two-stage road drains at the bus; a host that begins its block drains "
          "there instead, and the bus does not drain twice") {
	// **TRAP T3's OTHER HALF** — the drain PLACEMENT, which gap G1 names as the reason this extraction
	// is the most valuable in its list: *"four interacting booleans whose comments record two prior
	// wrong versions. A second container getting it NEARLY right produces a device that works until a
	// host uses the two-stage road."*
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	REQUIRE(chain.bridge() != nullptr);

	// An edit made through the bridge is in the record and not in the engine until a drain.
	chain.bridge()->addDevice(kProbeDesc.traits.tag);
	CHECK(chain.engine().chainLength(0) == 0);

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	chain.process(l.data(), r.data(), 64);
	CHECK(chain.engine().chainLength(0) == 1); // the bus drained, because no host began

	// NOW THE TWO-STAGE ROAD: the begin is the drain point from here on.
	chain.bridge()->addDevice(kProbeDesc.traits.tag);
	CHECK(chain.engine().chainLength(0) == 1);
	chain.beginPerVoiceBlock();
	CHECK(chain.engine().chainLength(0) == 2);

	// AND THE BUS DOES NOT DRAIN AGAIN INSIDE THE SAME HOST BLOCK — an edit landing between the host's
	// begin and the bus stage must wait for the NEXT begin, or a device appears mid-block.
	chain.bridge()->addDevice(kProbeDesc.traits.tag);
	chain.process(l.data(), r.data(), 64);
	CHECK(chain.engine().chainLength(0) == 2);
	chain.beginPerVoiceBlock();
	CHECK(chain.engine().chainLength(0) == 3);
}

//================================================================================================//
// TRAP T6 — THE BYPASS FADE                                                                       //
//================================================================================================//

TEST_CASE("a bypass FADES to dry over 5 ms rather than cutting — the coefficient is read, not the "
          "audio") {
	// **TRAP T6.** A holder that returns dry immediately cuts its chain's tail in one sample — a
	// reverb inside cut dead. The rack device fades 5 ms on both roads because a one-sample cut was
	// found once (backlog EH).
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	seatProbe(chain, 2.0f);

	std::array<float, 64> l {};
	std::array<float, 64> r {};
	chain.process(l.data(), r.data(), 64); // prime
	REQUIRE(chain.blendForTest().wet == doctest::Approx(1.0f));

	chain.setEnabled(false);
	chain.process(l.data(), r.data(), 64);
	// ONE QUARTER OF THE WAY THROUGH A 240-SAMPLE RAMP, STATED AS A NUMBER: the wet has fallen by
	// 64/240 of its distance to zero and the dry risen by 64/240 of its distance to one.
	CHECK(chain.blendForTest().wet == doctest::Approx(1.0f - 64.0f / kRamp).epsilon(0.001));
	CHECK(chain.blendForTest().dry == doctest::Approx(64.0f / kRamp).epsilon(0.001));
	// NOT A CUT: a device that simply returned dry would read 0 and 1 here.
	CHECK(chain.blendForTest().wet > 0.5f);

	// AND IT ARRIVES, exactly, rather than approaching: the ramp lands ON its target.
	std::array<float, 256> restL {};
	std::array<float, 256> restR {};
	chain.process(restL.data(), restR.data(), 256);
	CHECK(chain.blendForTest().wet == 0.0f);
	CHECK(chain.blendForTest().dry == 1.0f);
}

TEST_CASE("the per-voice road carries its OWN fade, because the bus blends against a signal that "
          "already contains the inner per-voice devices") {
	// **BACKLOG EH, SHAPE 6.** `voiceWet` is kept apart from `wet` because that one carries the mix and
	// the fader, which the voice road must not apply — so at mix 0.5 the voice road is still at 1.
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	chain.setMix(0.5f);

	std::array<float, 128> l {};
	std::array<float, 128> r {};
	chain.process(l.data(), r.data(), 64);
	CHECK(chain.blendForTest().wet == doctest::Approx(0.5f));
	CHECK(chain.blendForTest().voiceWet == doctest::Approx(1.0f)); // the mix is the BUS's, not the voice's

	chain.setEnabled(false);
	chain.process(l.data(), r.data(), 120);
	CHECK(chain.blendForTest().voiceWet == doctest::Approx(1.0f - 120.0f / kRamp).epsilon(0.001));
}

TEST_CASE("a Poly device inside runs per voice, and the chain answers for it") {
	InnerChain chain { rack::kRackAxes };
	chain.setVoices(3);
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	const DeviceInstanceId id = chain.bridge()->addDevice(kPolyDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	chain.bridge()->setField(id, 0, 1.0f); // Poly on
	chain.bridge()->setField(id, 1, 2.0f); // gain
	chain.bridge()->drain();
	REQUIRE(chain.engine().slotOf(id) != RackEngine::kNoSlot);

	CHECK(chain.beginPerVoiceBlock());
	std::array<float, 16> voice {};
	voice.fill(0.25f);
	device::NoteContext note {};
	note.gate = true;
	chain.processVoice(0, voice.data(), nullptr, 16, note);
	// ONE VOICE'S CHANNEL, MIRRORED INTO A PAIR AND AVERAGED BACK: 0.25 through a gain of 2 is 0.5 on
	// both channels, whose mid is 0.5, blended at voiceWet 1.
	CHECK(voice[0] == doctest::Approx(0.5f));
	CHECK(voice[15] == doctest::Approx(0.5f));

	std::array<float, 16> l {};
	std::array<float, 16> r {};
	chain.process(l.data(), r.data(), 16);
}

//================================================================================================//
// DECISION D3(a) — THE HOLDER'S OWN OUTPUTS AS THE CHAIN'S CORE SOURCES                           //
//================================================================================================//

TEST_CASE("a bipolar published source reaches the cell as itself, not as 2v−1 of itself") {
	// **DECISION D3(a), and the arithmetic is the whole of it.** `RackEngine::setMacro` stores `2v−1`
	// for a cell declared bipolar, because a macro is a 0..1 PARAMETER a hand holds and the bipolar
	// switch is about how a route reads it (backlog EG). A holder's published source is not a
	// parameter: a bipolar one already reads −1..+1. Handing SKLITTER's `Perf X` straight to
	// `setMacro` would put `2x − 1` in the cell — the range −3..+1 for an input of −1..+1 — and
	// NOTHING IN THE AUDIO WOULD SAY SO, which is why this is a value assertion on the cell.
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);

	chain.setCoreBipolar(0, true);
	chain.publishCoreSource(0, -1.0f);
	// `macro()` hands back the RAW value the cell was derived from; the cell is `2·raw − 1`.
	CHECK(chain.engine().macro(0) == doctest::Approx(0.0f));
	CHECK(2.0f * chain.engine().macro(0) - 1.0f == doctest::Approx(-1.0f));

	chain.publishCoreSource(0, 1.0f);
	CHECK(2.0f * chain.engine().macro(0) - 1.0f == doctest::Approx(1.0f));
	// **THE MIDDLE OF THE RANGE IS THE ONLY ONE OF THE THREE THAT IS NOT VACUOUS, MEASURED.** With the
	// mapping deleted (known-bad KB3: the unipolar branch taken for both), −1 clamps to 0 whose cell is
	// −1, and +1 clamps to 1 whose cell is +1 — so BOTH ENDPOINTS AGREE WITH THE BUG and the case would
	// have gone green on them alone. Only 0, whose cell should be 0 and reads −1, sees it. The same
	// coincidence is why the clamping case below cannot stand in for this one.
	chain.publishCoreSource(0, 0.0f);
	CHECK(2.0f * chain.engine().macro(0) - 1.0f == doctest::Approx(0.0f));

	// A UNIPOLAR SOURCE PASSES STRAIGHT THROUGH — the cell IS the value.
	chain.setCoreBipolar(1, false);
	chain.publishCoreSource(1, 0.375f);
	CHECK(chain.engine().macro(1) == doctest::Approx(0.375f));
}

TEST_CASE("a published source is clamped to the range its polarity declares") {
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	chain.setCoreBipolar(0, true);
	chain.publishCoreSource(0, -4.0f);
	CHECK(2.0f * chain.engine().macro(0) - 1.0f == doctest::Approx(-1.0f));
	chain.publishCoreSource(1, 9.0f);
	CHECK(chain.engine().macro(1) == doctest::Approx(1.0f));
}

//================================================================================================//
// THE BODY — ContainerOps::saveState / loadState                                                  //
//================================================================================================//

TEST_CASE("a chain saves what it holds and takes it back, and refuses a body it cannot read whole") {
	std::vector<uint8_t> body;
	const device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
		                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                             out->insert(out->end(), d, d + n);
	                             } };
	{
		InnerChain chain { rack::kRackAxes };
		chain.prepare(kRate, 4096);
		chain.adopt(kProbes);
		chain.bridge()->addDevice(kProbeDesc.traits.tag);
		chain.bridge()->drain();
		REQUIRE(chain.engine().chainLength(0) == 1);
		REQUIRE(chain.saveState(sink));
		REQUIRE(!body.empty());
	}
	{
		InnerChain chain { rack::kRackAxes };
		chain.prepare(kRate, 4096);
		chain.adopt(kProbes);
		CHECK(chain.loadState(body.data(), body.size()));
		CHECK(chain.engine().chainLength(0) == 1);
	}
	{
		// **ALL OR NOTHING** — `ContainerOps::loadState`'s own rule: the host cannot tell a truncated
		// body from a short one, so the device validates its own bytes and a refusal leaves it as it was.
		InnerChain chain { rack::kRackAxes };
		chain.prepare(kRate, 4096);
		chain.adopt(kProbes);
		CHECK_FALSE(chain.loadState(body.data(), body.size() / 2));
		CHECK(chain.engine().chainLength(0) == 0);
		CHECK_FALSE(chain.loadState(nullptr, 0));
	}
}

TEST_CASE("the load-depth guard is ONE counter for every kind of container, so two different "
          "containers alternating cannot slip past it") {
	// **GAP G4.** `rack_device.cpp` keeps a `thread_local loadDepth` of its own, so two DIFFERENT
	// container kinds alternating (A → B → A → …) each see depth 1 at every level and neither guard
	// ever fires — *"even two correct copies do not compose"*. One counter that every container
	// increments composes by construction. This case pins the counter's identity, which is the part a
	// second container's author cannot see by reading their own file.
	std::vector<uint8_t> body;
	const device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
		                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                             out->insert(out->end(), d, d + n);
	                             } };
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	REQUIRE(chain.saveState(sink));

	CHECK(rack::containerLoadDepth() == 0);
	// AT THE LIMIT, THE BODY IS REFUSED RATHER THAN RECURSED INTO. Standing the counter up by hand is
	// what a nest of holders does to it, and is the only way to reach the limit without one.
	rack::containerLoadDepth() = rack::kMaxDepth - 1;
	CHECK_FALSE(chain.loadState(body.data(), body.size()));
	rack::containerLoadDepth() = 0;
	CHECK(chain.loadState(body.data(), body.size()));
	// AND THE GUARD UNWINDS: a refused load must not leave the counter raised.
	CHECK(rack::containerLoadDepth() == 0);
}

//================================================================================================//
// LIFECYCLE                                                                                       //
//================================================================================================//

TEST_CASE("the bridge is built at adopt and never before — a chain built with no catalogue offers "
          "nothing, with no error") {
	// **TRAP T5.** `adopt` is the only moment a container may build its record, before `loadState` and
	// before any road that builds a member, *"because a record built before it would hold an empty
	// catalogue"*. Build the bridge in `construct` instead and the inner browser offers nothing.
	InnerChain chain { rack::kRackAxes };
	CHECK(chain.bridge() == nullptr);
	CHECK(chain.engine().addDevice(kProbeDesc.traits.tag) == RackEngine::kNoSlot);
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	REQUIRE(chain.bridge() != nullptr);
	CHECK(chain.engine().addDevice(kProbeDesc.traits.tag) != RackEngine::kNoSlot);
}

TEST_CASE("a chain adopted before it was prepared is prepared when the rate arrives, and one "
          "prepared first hands the rate to the bridge at adopt") {
	{
		InnerChain chain { rack::kRackAxes };
		chain.adopt(kProbes);
		chain.prepare(kRate, 4096);
		const size_t lane = seatProbe(chain, 1.0f);
		CHECK(probeIn(chain, lane).rate == kRate);
	}
	{
		InnerChain chain { rack::kRackAxes };
		chain.prepare(kRate, 4096);
		chain.adopt(kProbes);
		const size_t lane = seatProbe(chain, 1.0f);
		CHECK(probeIn(chain, lane).rate == kRate);
	}
}

//================================================================================================//
// BACKLOG GO — `prepare` CARRIES THE HOST'S LONGEST BLOCK                                          //
//================================================================================================//

TEST_CASE("GO: the host's longest block reaches every device the chain holds, by value — one built "
          "before the chain is prepared again and one built after — and sizes the chain's scratch") {
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 777);
	chain.adopt(kProbes);
	const size_t first = seatProbe(chain, 1.0f);
	CHECK(probeIn(chain, first).maxFrames == 777); // built AFTER prepare: the bridge builds at the chain's
	CHECK(chain.scratchFramesForTest() == 777);

	// A NEW LONGEST BLOCK (a host's buffer-size change) re-prepares what the chain holds, in place.
	chain.prepare(kRate, 333);
	CHECK(probeIn(chain, first).maxFrames == 333);
	const size_t second = seatProbe(chain, 1.0f);
	CHECK(probeIn(chain, second).maxFrames == 333);
	CHECK(chain.scratchFramesForTest() == 333);

	// AND AN ABSURD ANNOUNCEMENT IS A PROMISE, NOT A BUDGET: the device is told it, the scratch is capped.
	chain.prepare(kRate, std::numeric_limits<int32_t>::max());
	CHECK(probeIn(chain, first).maxFrames == std::numeric_limits<int32_t>::max());
	CHECK(chain.scratchFramesForTest() == static_cast<size_t>(device::kMaxScratchFrames));
}

TEST_CASE("GO: a block AT the longest renders in one piece, and one a frame PAST it is cut to the scratch "
          "and still renders every sample — on the per-voice road and on the bus") {
	// 40, BELOW the 64-sample tick window, so the bus road's cut is the scratch's and not the window's.
	constexpr int kMax = 40;
	InnerChain chain { rack::kRackAxes };
	chain.setVoices(1);
	chain.prepare(kRate, kMax);
	chain.adopt(kProbes);
	const DeviceInstanceId id = chain.bridge()->addDevice(kPolyDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	chain.bridge()->setField(id, 0, 1.0f); // Poly on
	chain.bridge()->setField(id, 1, 2.0f); // gain
	chain.bridge()->drain();
	const size_t lane = chain.engine().slotOf(id);
	REQUIRE(lane != RackEngine::kNoSlot);
	REQUIRE(chain.scratchFramesForTest() == static_cast<size_t>(kMax));

	device::NoteContext note {};
	note.gate = true;
	for (const int frames : { kMax, kMax + 1 }) {
		CAPTURE(frames);
		PolyProbe& voice = polyAt(chain.engine().slotAtLane(lane).voices[0].get());
		const int before = voice.processed;
		voice.longest = 0;
		std::vector<float> buffer(static_cast<size_t>(frames), 0.25f);
		REQUIRE(chain.beginPerVoiceBlock());
		chain.processVoice(0, buffer.data(), nullptr, frames, note);
		// EVERY SAMPLE, the one past the scratch included: 0.25 through a gain of 2, mirrored and averaged back.
		for (int i = 0; i < frames; ++i) {
			CAPTURE(i);
			CHECK(buffer[static_cast<size_t>(i)] == 0.5f);
		}
		// AND THE CUT IS THE SCRATCH'S: one piece at the longest, two (40 + 1) a frame past it.
		CHECK(voice.longest == kMax);
		CHECK(voice.processed - before == (frames > kMax ? 2 : 1));
		std::vector<float> l(static_cast<size_t>(frames), 0.0f);
		std::vector<float> r(static_cast<size_t>(frames), 0.0f);
		chain.process(l.data(), r.data(), frames); // closes the block the voice stage began
	}

	// THE BUS ROAD, through a Probe (gain 3, no per-voice road): the same two lengths, every sample a value. A fresh
	// chain for each, so the tick phase starts at 0 and the only cut inside the call is the scratch's.
	for (const int frames : { kMax, kMax + 1 }) {
		CAPTURE(frames);
		InnerChain bus { rack::kRackAxes };
		bus.prepare(kRate, kMax);
		bus.adopt(kProbes);
		const size_t busLane = seatProbe(bus, 3.0f);
		std::vector<float> l(static_cast<size_t>(frames), 0.125f);
		std::vector<float> r(static_cast<size_t>(frames), -0.5f);
		bus.process(l.data(), r.data(), frames);
		for (int i = 0; i < frames; ++i) {
			CAPTURE(i);
			CHECK(l[static_cast<size_t>(i)] == doctest::Approx(0.375f));
			CHECK(r[static_cast<size_t>(i)] == doctest::Approx(-1.5f));
		}
		CHECK(probeIn(bus, busLane).longest == kMax);
		CHECK(probeIn(bus, busLane).processed == (frames > kMax ? 2 : 1));
	}
}

TEST_CASE("a chain holding NOTHING is a true pass, bit for bit — including the sign of a zero") {
	// **THE REASON IS CORRECTNESS, NOT COST.** The blend is `x·wet + dry·dry`; at wet 1 and dry 0 that
	// is `x·1 + y·0`, which equals x for every finite x EXCEPT −0.0, where `−0.0 + 0.0` is `+0.0`. So a
	// holder that gained a post chain would have flipped the sign of its zeros on every block for a
	// chain nobody had put anything in. Asserted with `std::signbit`, because `−0.0 == 0.0` is TRUE and
	// an equality check here would be vacuous — the exact shape this file's banner is about.
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	REQUIRE(chain.engine().chainLength(0) == 0);

	std::array<float, 96> l {};
	std::array<float, 96> r {};
	l[0] = -0.0f;
	l[1] = 0.375f;
	l[2] = -1.5f;
	r[0] = -0.0f;
	chain.process(l.data(), r.data(), 96);
	CHECK(std::signbit(l[0]));
	CHECK(std::signbit(r[0]));
	CHECK(l[1] == 0.375f);
	CHECK(l[2] == -1.5f);

	// AND THE PHASE STILL ADVANCED, because it is this chain's place in the HOST's stream rather than a
	// count of work done: a chain that stood still while empty would put its inner grid at an arbitrary
	// offset from the outer one the moment a device was added.
	CHECK(chain.tickPhaseForTest() == 96 % device::kTickWindow);

	// THE PASS IS NOT UNCONDITIONAL: a chain at mix 0.5 has to blend even with nothing in it, because
	// half of nothing is not the input.
	chain.setMix(0.5f);
	std::array<float, 512> settle {};
	std::array<float, 512> settleR {};
	chain.process(settle.data(), settleR.data(), 512);
	REQUIRE(chain.blendForTest().wet == doctest::Approx(0.5f));
	l[1] = 0.375f;
	chain.process(l.data(), r.data(), 96);
	CHECK(l[1] == doctest::Approx(0.375f * 0.5f + 0.375f * 0.5f));
	CHECK(l[1] == doctest::Approx(0.375f)); // the same number by two roads; the road is the claim
}

TEST_CASE("a chain with no scratch yet renders nothing rather than reading past a buffer") {
	// A HOLDER THAT NEVER PREPARED is a holder a host built and did not activate. The audio road must
	// not touch the scratch it has not sized — trap T2 from the other side.
	InnerChain chain { rack::kRackAxes };
	chain.adopt(kProbes);
	seatProbe(chain, 2.0f);
	std::array<float, 16> l {};
	std::array<float, 16> r {};
	l.fill(0.5f);
	chain.process(l.data(), r.data(), 16);
	CHECK(l[0] == 0.5f);
}

//================================================================================================//
// GAP G3 — TWO CHAINS IN ONE HOLDER ISSUE THE SAME INSTANCE IDS                                   //
//================================================================================================//

// **THE COLLISION THIS FIXES WAS MEASURED BEFORE IT WAS FIXED, and these two cases are the same
// instrument read twice.** Every `RackBridge` used to own its `backplane::InstanceCounter`, so two
// chains in one holder each issued id 1 and BOTH answered `descriptorOf(1)` non-null. That is not
// untidiness: `ContainerOps`' four route verbs take a `device::Destination{deviceId, index}` with NO
// lane argument, so a holder of two chains must dispatch on the id alone and either lane is a valid
// answer. Backlog EW's grain lane is the first caller that would have met it.
//
// **THE CLAIM IS A VALUE — the ids themselves — and not "the two chains behave differently".** A
// difference check here would pass for any reason at all (CLAUDE.md's rule, paid for four times).

TEST_CASE("G3: two chains lent ONE counter issue disjoint instance ids, so a destination names one "
          "device rather than one per lane") {
	backplane::InstanceCounter holder;
	InnerChain grain(rack::kRackAxes, &holder);
	InnerChain master(rack::kRackAxes, &holder);
	grain.prepare(kRate, 4096);
	master.prepare(kRate, 4096);
	grain.adopt(kProbes);
	master.adopt(kProbes);

	const DeviceInstanceId inGrain = grain.bridge()->addDevice(kProbeDesc.traits.tag);
	const DeviceInstanceId inMaster = master.bridge()->addDevice(kProbeDesc.traits.tag);
	MESSAGE("first id in the grain chain = " << inGrain << ", in the master chain = " << inMaster);
	REQUIRE(inGrain != kNoInstance);
	REQUIRE(inMaster != kNoInstance);
	CHECK(inGrain != inMaster);

	// **AND EACH ID IS ANSWERED BY EXACTLY ONE CHAIN**, which is the property the route verbs actually
	// need — `inGrain != inMaster` alone would hold even if both chains still knew both ids.
	CHECK(grain.bridge()->descriptorOf(inGrain) != nullptr);
	CHECK(master.bridge()->descriptorOf(inGrain) == nullptr);
	CHECK(master.bridge()->descriptorOf(inMaster) != nullptr);
	CHECK(grain.bridge()->descriptorOf(inMaster) == nullptr);

	// EVERY id, not only the first — the counter advances across both chains, so a second device in
	// each is disjoint from all three that came before it.
	const DeviceInstanceId g2 = grain.bridge()->addDevice(kProbeDesc.traits.tag);
	const DeviceInstanceId m2 = master.bridge()->addDevice(kProbeDesc.traits.tag);
	CHECK(g2 != m2);
	CHECK(g2 != inMaster);
	CHECK(m2 != inGrain);
}

TEST_CASE("G3: a chain lent NO counter still counts alone, so the standalone rack is unchanged") {
	// THE OTHER HALF OF THE DEFAULT, asserted rather than assumed: a lone chain must keep issuing from
	// `kFirstInstance`, because `rack_device.cpp` passes nothing and its bodies name those ids.
	InnerChain alone(rack::kRackAxes);
	alone.prepare(kRate, 4096);
	alone.adopt(kProbes);
	CHECK(alone.bridge()->addDevice(kProbeDesc.traits.tag) == kFirstInstance);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG NR'S CHEAP WINS — the drain and the publish skip the work of a block where nothing changed   //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NR: a drain with nothing new skips the converge pass, and an edit made after it still lands on the very next block") {
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	rack::RackBridge* b = chain.bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kProbeDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	for (int block = 0; block < 4; ++block) {
		chain.process(l.data(), r.data(), 64); // the device lands, and the pass that placed it settles
	}
	const uint64_t settled = b->convergesForTest();
	for (int block = 0; block < 16; ++block) {
		chain.process(l.data(), r.data(), 64);
	}
	CHECK(b->convergesForTest() == settled); // sixteen quiet drains, no pass

	// THE EDIT LANDS ON THE NEXT BLOCK: the pass runs once, and the value is the one the hand set.
	b->setField(id, 0, 3.0f);
	l.fill(0.5f);
	r.fill(0.5f);
	chain.process(l.data(), r.data(), 64);
	CHECK(b->convergesForTest() == settled + 1);
	const size_t slot = chain.engine().slotOf(id);
	REQUIRE(slot != RackEngine::kNoSlot);
	CHECK(chain.engine().slotAtLane(slot).base[0] == 3.0f);
	CHECK(l[0] == 1.5f); // 0.5 · 3, at the chain's mix 1
	CHECK(r[63] == 1.5f);
}

TEST_CASE("NR: an empty chain's publish walks the mirror once, and a device arriving is mirrored on the block it lands") {
	InnerChain chain { rack::kRackAxes };
	chain.prepare(kRate, 4096);
	chain.adopt(kProbes);
	rack::RackBridge* b = chain.bridge();
	REQUIRE(b != nullptr);
	std::array<float, 64> l {};
	std::array<float, 64> r {};
	chain.process(l.data(), r.data(), 64);
	const uint64_t walks = b->mirrorWalksForTest();
	CHECK(walks >= 1);
	for (int block = 0; block < 16; ++block) {
		chain.process(l.data(), r.data(), 64);
	}
	CHECK(b->mirrorWalksForTest() == walks); // empty, and empty last time: nothing to write

	const DeviceInstanceId id = b->addDevice(kProbeDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	float peak = 0.0f;
	CHECK_FALSE(b->outputPeakAt(id, peak)); // the mirror does not name it yet
	chain.process(l.data(), r.data(), 64);
	CHECK(b->mirrorWalksForTest() > walks);
	CHECK(b->outputPeakAt(id, peak)); // mirrored on the block it landed

	// AND A CHAIN THAT HOLDS A DEVICE NEVER SKIPS: an edit's value reaches the mirror on the next block.
	const uint64_t holding = b->mirrorWalksForTest();
	b->setField(id, 0, 3.0f);
	chain.process(l.data(), r.data(), 64);
	CHECK(b->mirrorWalksForTest() == holding + 1);
	CHECK(b->modulatedFieldAt(id, 0) == 3.0f);
}
