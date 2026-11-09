/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "composite/composite_types.h"
#include "rack/inner_chain.h"
#include "rack/inner_chain_bundle.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// **SKLITTER AS A HOLDER — backlog EW's post lane, the spine of it.**
//
// The chain's own machinery is pinned in `test_inner_chain.cpp`; nothing here re-tests it. These cases
// are about the three things only THIS device can answer:
//
//   1. the chain runs on every road `processStereo` can take, including the four that return early;
//   2. what it is handed is this device's output AFTER the blend, whole;
//   3. its seven aux outputs reach the chain's core sources meaning what they mean outside it.
//
// **AND THE SECOND ONE IS WHY THERE IS A REFERENCE DEVICE IN HERE.** "The chain changed the output" is
// not the claim and would pass with the chain wired anywhere at all — before the render, on the dry
// path, on one channel. The claim is that the chain's input is EXACTLY what this device would have
// produced with no chain, which needs a second device producing exactly that, sample for sample. That
// is an independent restatement of the intended path rather than a difference check, and the
// difference check is kept only as the assertion that proves it is not vacuous.

using namespace feathers;
using namespace feathers::fx::sklitter;
using feathers::device::TransportClock;

namespace {

constexpr double kRate = 48000.0;

TransportClock at120bpm(double beats = 0.0, bool playing = true) {
	TransportClock c = TransportClock::fromSecondsPerBeat(0.5);
	c.songPositionBeats = beats;
	c.playing = playing;
	return c;
}

//------------------------------------------------------------------------------------------------//
// THE PROBE — the device put INSIDE SKLITTER, whose every effect is a number.                      //
//------------------------------------------------------------------------------------------------//

struct Probe {
	float gain = 1.0f;
	/// **THE FIRST SAMPLE IT WAS EVER HANDED, and the flag is what makes it the FIRST.** Without it a
	/// case reads whatever the last block left, which for a device that runs every block is a
	/// different claim wearing the same name.
	float firstSeen = 0.0f;
	bool sawAnything = false;
	int processed = 0;
	std::vector<float> seen;
};

Probe& probeAt(void* storage) { return *static_cast<Probe*>(storage); }

constexpr device::AudioOps kProbeOps {
	.stateBytes = sizeof(Probe),
	.stateAlign = alignof(Probe),
	.construct = [](void* s) { new (s) Probe {}; },
	.destruct = [](void* s) { static_cast<Probe*>(s)->~Probe(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setSlot = nullptr,
	.setFields = [](void* s, const float* f, size_t n) { probeAt(s).gain = n > 0 ? f[0] : 1.0f; },
	.processStereo =
	    [](void* s, float* l, float* r, int frames) {
		    Probe& p = probeAt(s);
		    if (!p.sawAnything && frames > 0) {
			    p.firstSeen = l[0];
			    p.sawAnything = true;
		    }
		    for (int i = 0; i < frames; ++i) {
			    p.seen.push_back(l[i]);
			    l[i] *= p.gain;
			    r[i] *= p.gain;
		    }
		    ++p.processed;
	    },
};

constexpr std::array<device::ParamDesc, 1> kProbeParams { {
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {} },
} };

const device::Descriptor kProbeDesc {
	.traits = { .tag = "prbS", .name = "Probe S", .hasCard = false, .hasExpanded = false,
	            .perVoiceControl = -1 },
	.params = kProbeParams,
	.outputs = {},
	.audio = &kProbeOps,
};

const device::Descriptor* findProbe(void*, const char* tag) {
	return device::sameTag(tag, kProbeDesc.traits.tag) ? &kProbeDesc : nullptr;
}

const device::Catalogue kProbes { nullptr, &findProbe };

/// **A CATALOGUE HOLDING BOTH CONTAINER KINDS** — the rack device and SKLITTER itself. What the
/// alternating-nest case needs, and the reason this suite is where that case lives: it is the only one
/// that can see both.
const device::Descriptor* findNesting(void*, const char* tag) {
	if (device::sameTag(tag, kProbeDesc.traits.tag)) {
		return &kProbeDesc;
	}
	if (device::sameTag(tag, composite::kTraits.tag)) {
		return &rack::kDeviceDescriptor;
	}
	if (device::sameTag(tag, kTraits.tag)) {
		return &kDescriptor;
	}
	return nullptr;
}

const device::Catalogue kNesting { nullptr, &findNesting };

/// **ADOPT THROUGH THE OPS TABLE, not through `postChain()`** — the table is what a host calls, and a
/// case that reached past it would be testing a road no host takes.
void adoptChain(Sklitter& s) {
	REQUIRE(kDescriptor.container != nullptr);
	REQUIRE(kDescriptor.container->adopt != nullptr);
	kDescriptor.container->adopt(&s, kProbes);
	REQUIRE(s.postChain() != nullptr);
	REQUIRE(s.postChain()->bridge() != nullptr);
	// **BOTH LANES ADOPT FROM THE ONE CALL** — `ContainerOps::adopt` fans out, and a grain lane that
	// never got a catalogue would offer nothing with no error (trap T5).
	REQUIRE(s.grainChain() != nullptr);
	REQUIRE(s.grainChain()->bridge() != nullptr);
}

/// A GRAIN-MODE SETTING, engaged, with the send override wherever the caller wants it.
Settings grainMode(float sendOverride) {
	Settings s;
	s.mode = Mode::Grain;
	s.enabled = true;
	s.engage = true;
	s.rate = 0.5f;
	s.quantized = true;
	s.mix = 1.0f;
	s.sendOverride = sendOverride;
	return s;
}

/// Put one Probe in a named chain at `gain`, and return it.
Probe& seatProbeIn(rack::InnerChain* chain, float gain) {
	REQUIRE(chain != nullptr);
	rack::RackBridge* b = chain->bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kProbeDesc.traits.tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, 0, gain);
	b->drain();
	const size_t lane = chain->engine().slotOf(id);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	return probeAt(chain->engine().slotAtLane(lane).storage.get());
}

/// Put one Probe in the MASTER lane at `gain`, and return it.
Probe& seatProbe(Sklitter& s, float gain) { return seatProbeIn(s.postChain(), gain); }

/// The Probe a LOADED chain holds at its first position — `seatProbeIn` returns the one it seated,
/// and after a `loadState` nobody seated anything, so the device has to be found through the engine.
Probe& firstProbeIn(rack::InnerChain* chain) {
	REQUIRE(chain != nullptr);
	// `deviceAtPosition` ALREADY answers in the pool's own index — it is one of the lane-named calls,
	// not an instance id — so `slotAtLane` takes it directly. (A first draft ran it through `slotOf`
	// and got `kNoSlot`, which is what a lane index looks like when it is read as an id.)
	const size_t lane = chain->engine().deviceAtPosition(0);
	REQUIRE(lane != rack::RackEngine::kNoSlot);
	return probeAt(chain->engine().slotAtLane(lane).storage.get());
}

Settings burst(float mix = 1.0f) {
	Settings s;
	s.mode = Mode::Burst;
	s.enabled = true;
	s.engage = false;
	s.rate = 0.5f;
	s.quantized = true;
	s.mix = mix;
	return s;
}

/// A RAMP, so every frame is distinguishable from every other — silence would make "carried the right
/// samples" and "carried any samples" the same observation.
void fillRamp(std::vector<float>& l, std::vector<float>& r, int from) {
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = 0.001f * static_cast<float>(from + static_cast<int>(i));
		r[i] = -l[i];
	}
}

/// Run `blocks` blocks of ramp through a device and return the left channel it produced.
std::vector<float> runBlocks(Sklitter& s, int blocks, int chunk) {
	std::vector<float> out;
	double beats = 0.0;
	for (int b = 0; b < blocks; ++b) {
		std::vector<float> l(static_cast<size_t>(chunk));
		std::vector<float> r(static_cast<size_t>(chunk));
		fillRamp(l, r, b * chunk);
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), chunk);
		out.insert(out.end(), l.begin(), l.end());
		beats += static_cast<double>(chunk) / (kRate * 0.5);
	}
	return out;
}

} // namespace

//================================================================================================//
// 1. THE CHAIN RUNS ON EVERY ROAD, INCLUDING THE FOUR THAT RETURN EARLY                           //
//================================================================================================//

TEST_CASE("the post chain runs while this device is BYPASSED — a reverb inside keeps its tail when "
          "the stutter is switched off") {
	// **TRAP T6, AVOIDED BY CONSTRUCTION RATHER THAN BY A FADE.** `renderStereo`'s second statement is
	// `if (!target_.enabled) { engage_.reset(); return; }` — one of five early returns — and the chain
	// sits in `processStereo`, outside it. `RackEngine::processBus` calls `processStereo`
	// unconditionally (`traits.enableControl` gates the modulation OUTPUTS, not the audio call), so
	// the road is real and not hypothetical.
	Sklitter s;
	s.prepare(kRate, 4096);
	adoptChain(s);
	Probe& probe = seatProbe(s, 2.0f);

	Settings cfg = burst();
	cfg.enabled = false; // the bypass road
	s.setTarget(cfg);
	s.setTransportClock(at120bpm());

	std::vector<float> l(64);
	std::vector<float> r(64);
	fillRamp(l, r, 1000);
	const float in0 = l[0];
	s.processStereo(l.data(), r.data(), 64);

	CHECK(probe.processed > 0);
	// **BYPASSED, THE CHAIN IS HANDED SILENCE — and that is the point of the road rather than a
	// failure of it.** The master dry/wet is post-fx by design, so the lane is an insert
	// on the WET, and a bypassed device produces no wet. The chain still RUNS (the assertion above,
	// which is trap T6's whole concern), so a reverb inside goes on decaying its tail into silence —
	// instead of reverberating the untouched dry, which is what it did while the lane sat after the
	// blend and which read oddly the moment it was written down.
	CHECK(probe.firstSeen == 0.0f);
	// AND THE DEVICE'S OUTPUT IS THE INPUT, BIT FOR BIT. Not `Approx`: with no wet written, the blend
	// coefficient is zero at every sample and the blend is skipped outright, so these are the same
	// floats that went in — the promise `mixCoeff_`'s declaration makes.
	CHECK(l[0] == in0);
	CHECK(l[63] == 0.001f * 1063.0f);
}

TEST_CASE("the post chain runs while this device is IDLE — the road that returns after the looper's "
          "pre-warm") {
	Sklitter s;
	s.prepare(kRate, 4096);
	adoptChain(s);
	Probe& probe = seatProbe(s, 2.0f);

	Settings cfg = burst();
	cfg.enabled = true;
	cfg.engage = false; // armed, nothing pressed: `engage_.status() == Off`
	s.setTarget(cfg);
	s.setTransportClock(at120bpm());

	std::vector<float> l(64);
	std::vector<float> r(64);
	fillRamp(l, r, 2000);
	const float in0 = l[0];
	s.processStereo(l.data(), r.data(), 64);

	// IDLE PRODUCES NO WET EITHER, so the chain runs on silence and the output is the input untouched.
	CHECK(probe.processed > 0);
	CHECK(probe.firstSeen == 0.0f);
	CHECK(l[0] == in0);
}

TEST_CASE("the post chain runs while this device is RECORDING — the road inside the capture that "
          "returns before any renderer") {
	Sklitter s;
	s.prepare(kRate, 4096);
	adoptChain(s);
	Probe& probe = seatProbe(s, 2.0f);

	Settings cfg = burst();
	cfg.engage = true; // pressed: captures a slice before it plays anything
	s.setTarget(cfg);
	s.setTransportClock(at120bpm());

	std::vector<float> l(64);
	std::vector<float> r(64);
	fillRamp(l, r, 3000);
	const float in0 = l[0];
	s.processStereo(l.data(), r.data(), 64);

	REQUIRE(s.statusForTest() != Status::Playing); // a 16th at 120 BPM is 6000 frames; 64 is not it
	// **THE CAPTURE WRITES THE RING AND PRODUCES NO OUTPUT**, which is precisely the region of a block
	// that no renderer writes — see `mixCoeff_`'s declaration, where that is the reason the coefficient
	// doubles as a written-mask. The chain runs on silence; the input passes through untouched.
	CHECK(probe.processed > 0);
	CHECK(probe.firstSeen == 0.0f);
	CHECK(l[0] == in0);
}

//================================================================================================//
// 2. WHAT THE CHAIN IS HANDED IS THIS DEVICE'S WET, BEFORE THE BLEND                              //
//                                                                                                 //
// **THIS SECTION ASSERTED THE OPPOSITE UNTIL 2026-09-20, AND THE REVERSAL IS A DESIGN DECISION**: //
// the master dry/wet is post-fx. The master lane shipped AFTER the blend because decision D2      //
// judged the wet-only placement too expensive to reach, while calling it musically the more       //
// useful answer. It is now where D2 wanted it, and the                                            //
// two cases below are the old pair restated rather than adjusted: each one's value is different   //
// under the new order, not merely shifted.                                                        //
//================================================================================================//

TEST_CASE("what the chain is handed does not depend on Param::Mix AT ALL — the lane is on the wet, "
          "ahead of the blend, so two devices at mix 0 and mix 1 feed it the same samples") {
	// **THIS IS THE ONE VALUE THAT SEPARATES "ON THE WET" FROM "ON THE OUTPUT", and it is bit-exact.**
	// While the lane sat after the blend, its input carried `Param::Mix` — so mix 0 fed it the dry and
	// mix 1 fed it the stutter, two completely different signals. On the wet path the mix has not been
	// applied yet, so the SAME samples reach the chain either way. Two devices, two recording probes,
	// one comparison.
	//
	// **A COMPARISON OF TWO PROBE STREAMS IS A VALUE CLAIM AND NOT A DIFFERENCE CHECK**, because the
	// claim IS an equality — and the second assertion below proves it is not vacuous by showing the two
	// devices' OUTPUTS are wildly different, which is the only way "they agree" could be trivial.
	Sklitter dry;
	Sklitter wet;
	dry.prepare(kRate, 4096);
	wet.prepare(kRate, 4096);
	adoptChain(dry);
	adoptChain(wet);
	Probe& dryProbe = seatProbe(dry, 1.0f);
	Probe& wetProbe = seatProbe(wet, 1.0f);

	Settings atZero = burst(/*mix=*/0.0f);
	Settings atOne = burst(/*mix=*/1.0f);
	atZero.engage = true;
	atOne.engage = true;
	dry.setTarget(atZero);
	wet.setTarget(atOne);

	std::vector<float> outAtZero;
	std::vector<float> outAtOne;
	double beats = 0.0;
	constexpr int kChunk = 512;
	constexpr int kBlocks = 24; // past a 16th at 120 BPM (6000 frames), so it is PLAYING by the end
	for (int b = 0; b < kBlocks; ++b) {
		std::vector<float> dl(kChunk);
		std::vector<float> dr(kChunk);
		std::vector<float> wl(kChunk);
		std::vector<float> wr(kChunk);
		fillRamp(dl, dr, b * kChunk);
		fillRamp(wl, wr, b * kChunk);
		dry.setTransportClock(at120bpm(beats));
		wet.setTransportClock(at120bpm(beats));
		dry.processStereo(dl.data(), dr.data(), kChunk);
		wet.processStereo(wl.data(), wr.data(), kChunk);
		outAtZero.insert(outAtZero.end(), dl.begin(), dl.end());
		outAtOne.insert(outAtOne.end(), wl.begin(), wl.end());
		beats += static_cast<double>(kChunk) / (kRate * 0.5);
	}
	REQUIRE(dry.statusForTest() == Status::Playing);
	REQUIRE(wet.statusForTest() == Status::Playing);

	// THE CLAIM: the same samples reached both chains, bit for bit.
	REQUIRE(dryProbe.seen.size() == wetProbe.seen.size());
	size_t chainMismatches = 0;
	for (size_t i = 0; i < dryProbe.seen.size(); ++i) {
		if (dryProbe.seen[i] != wetProbe.seen[i]) {
			++chainMismatches;
		}
	}
	CHECK(chainMismatches == 0);

	// **AND THE ASSERTION THAT PROVES IT IS NOT VACUOUS.** The two devices must be audibly different
	// from each other, or "their chains agree" would be a sentence about two identical devices. At mix
	// 0 the output is the input; at mix 1 it is the stutter.
	REQUIRE(outAtZero.size() == outAtOne.size());
	size_t outputsDiffer = 0;
	for (size_t i = 0; i < outAtZero.size(); ++i) {
		if (outAtZero[i] != outAtOne[i]) {
			++outputsDiffer;
		}
	}
	CHECK(outputsDiffer > outAtZero.size() / 4);

	// AND THE CHAIN'S INPUT IS NOT MERELY THE DEVICE'S INPUT EITHER — it is the stutter, so a lane
	// wired before the render, or onto the dry, would also fail here.
	size_t movedByTheDevice = 0;
	for (size_t i = 0; i < wetProbe.seen.size(); ++i) {
		if (wetProbe.seen[i] != 0.001f * static_cast<float>(i)) {
			++movedByTheDevice;
		}
	}
	CHECK(movedByTheDevice > wetProbe.seen.size() / 4);
}

TEST_CASE("this device's OWN mix is NOT applied when the chain sees the block — at mix 0 the chain "
          "is handed the full stutter while the output stands still at the dry input") {
	// **THE EXACT REVERSAL OF WHAT THIS CASE ASSERTED UNTIL 2026-09-20, and it is worth stating as a
	// reversal rather than quietly rewriting.** It used to read *"DECISION D2, STATED AS THE ONE VALUE
	// THAT DISTINGUISHES IT — 'after the blend' means the chain's input carries `Param::Mix`"*, and it
	// asserted that at mix 0 the chain was handed the input sample for sample. The lane has since moved
	// onto the wet (the master dry/wet is post-fx), so the mix no longer reaches it: at mix 0 the
	// chain hears everything and the listener hears nothing.
	Sklitter s;
	s.prepare(kRate, 4096);
	adoptChain(s);
	Probe& probe = seatProbe(s, 1.0f);

	Settings cfg = burst(/*mix=*/0.0f);
	cfg.engage = true;
	s.setTarget(cfg);

	double beats = 0.0;
	constexpr int kChunk = 512;
	std::vector<float> in;
	std::vector<float> lastOut;
	for (int b = 0; b < 24; ++b) {
		std::vector<float> l(kChunk);
		std::vector<float> r(kChunk);
		fillRamp(l, r, b * kChunk);
		in.insert(in.end(), l.begin(), l.end());
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), kChunk);
		lastOut.insert(lastOut.end(), l.begin(), l.end()); // what came OUT, block by block
		beats += static_cast<double>(kChunk) / (kRate * 0.5);
	}
	REQUIRE(s.statusForTest() == Status::Playing);

	REQUIRE(probe.seen.size() == in.size());

	// **THE CHAIN IS HANDED THE STUTTER, NOT THE INPUT** — the reversal, as a count. While the lane sat
	// after the blend this loop found ZERO differences, because at mix 0 the device's output was its
	// input. On the wet path the mix has not happened yet, so what the chain hears is the full stutter
	// however far down the device is mixed.
	size_t chainHeardSomethingElse = 0;
	for (size_t i = 0; i < in.size(); ++i) {
		if (probe.seen[i] != in[i]) {
			++chainHeardSomethingElse;
		}
	}
	CHECK(chainHeardSomethingElse > in.size() / 4);

	// **AND THE DEVICE'S OUTPUT IS STILL THE INPUT, BIT FOR BIT** — which is what makes the line above
	// a statement about the LANE's position rather than about the mix being broken. At mix 0 the blend
	// coefficient is zero at every sample, the blend is skipped, and these are the floats that went in.
	// The two assertions together are the whole of "an insert on the wet, muted by the mix".
	size_t outputMoved = 0;
	for (size_t i = 0; i < in.size(); ++i) {
		if (lastOut[i] != in[i]) {
			++outputMoved;
		}
	}
	CHECK(outputMoved == 0);

	// **A TAIL BUILDS WHERE NOBODY CAN HEAR IT, AND THAT IS ACCEPTED BY DESIGN** (2026-09-20, with both
	// consequences known). The chain runs at full level on a signal the mix is discarding,
	// so a reverb inside is charging up the whole time the device is mixed out and appears the instant
	// the mix comes up. It is what an insert on a wet path IS, it had no case at all when it was
	// approved, and this is that case: the chain did real work while the output stood still.
	CHECK(probe.processed > 0);
}

TEST_CASE("a device whose chain holds NOTHING is bit for bit the device that has no chain at all") {
	// **THE INSERTION MUST BE FREE WHEN IT IS UNUSED, and `Approx` cannot say so.** Every existing
	// patch has an empty chain; if the blend ran anyway, the whole device would have moved by an
	// arithmetic identity that is exact for every finite sample EXCEPT −0.0. Asserted as EQUALITY on
	// every sample, against a device that never adopted a chain at all.
	Sklitter bare;
	Sklitter empty;
	bare.prepare(kRate, 4096);
	empty.prepare(kRate, 4096);
	adoptChain(empty); // adopted, and holding nothing

	Settings cfg = burst();
	cfg.engage = true;
	bare.setTarget(cfg);
	empty.setTarget(cfg);

	double beats = 0.0;
	constexpr int kChunk = 512;
	size_t differing = 0;
	for (int b = 0; b < 24; ++b) {
		std::vector<float> bl(kChunk);
		std::vector<float> br(kChunk);
		std::vector<float> el(kChunk);
		std::vector<float> er(kChunk);
		fillRamp(bl, br, b * kChunk);
		fillRamp(el, er, b * kChunk);
		bare.setTransportClock(at120bpm(beats));
		empty.setTransportClock(at120bpm(beats));
		bare.processStereo(bl.data(), br.data(), kChunk);
		empty.processStereo(el.data(), er.data(), kChunk);
		for (int i = 0; i < kChunk; ++i) {
			if (bl[static_cast<size_t>(i)] != el[static_cast<size_t>(i)]
			    || br[static_cast<size_t>(i)] != er[static_cast<size_t>(i)]) {
				++differing;
			}
		}
		beats += static_cast<double>(kChunk) / (kRate * 0.5);
	}
	REQUIRE(bare.statusForTest() == Status::Playing);
	CHECK(differing == 0);
}

TEST_CASE("a sample no renderer wrote passes through UNTOUCHED — the sign of its zero survives, and "
          "so does a block that follows one the device did render") {
	// **THE TWO PROPERTIES THE WET BUFFERS ARE DECLARED WITH, ASSERTED DIRECTLY** (see `mixCoeff_` in
	// `sklitter.h`). Moving the blend past the lanes means every sample now travels through a buffer
	// that the renderer may not have written, and both halves of that are load-bearing:
	//
	//   1. AT `m == 0` THE BLEND IS SKIPPED, NOT COMPUTED. `dry + (wet − dry)·0` is `dry + 0.0f`, which
	//      is `+0.0f` when `dry` is `−0.0f` — so the arithmetically-identical form silently flips the
	//      sign of every negative zero in a region the device did not render.
	//   2. THE COEFFICIENT IS CLEARED EVERY CHUNK. It is the written-mask as well as the mix, so a
	//      stale one makes a silent block replay the PREVIOUS block's blend.
	//
	// **THE INSTALLED BIT-FOR-BIT CASE ABOVE CANNOT SEE EITHER PROPERTY, BECAUSE IT COMPARES TWO
	// DEVICES THAT BOTH TRAVEL THE NEW PATH — a fault in the path moves both the same way. Its passing
	// was never evidence the path was right.**
	//
	// **THAT IS A DISTINCT FAILURE FROM THE ONES THIS REPO ALREADY COLLECTS, AND WORTH NAMING.**
	// CLAUDE.md's rule is about a DIFFERENCE check passing whenever anything changes; the neighbouring
	// trap is a correct assertion asked in the wrong state. This is a third thing: a DIFFERENTIAL case
	// whose two arms SHARE the machinery under test. It is blind to anything common to both of them —
	// which is precisely what a restructuring changes. So when a shared path is introduced beneath an
	// existing comparison, that comparison silently stops covering the thing it was written for, and
	// nothing goes red to say so. The cases below ask about ONE device's output against a value.
	Sklitter s;
	s.prepare(kRate, 4096);
	adoptChain(s);
	seatProbe(s, 2.0f); // a chain that would DOUBLE anything it was allowed to reach

	// (1) BYPASSED, WITH NEGATIVE ZEROS IN THE INPUT.
	Settings off = burst();
	off.enabled = false;
	s.setTarget(off);
	s.setTransportClock(at120bpm());
	std::vector<float> zl(64, -0.0f);
	std::vector<float> zr(64, -0.0f);
	s.processStereo(zl.data(), zr.data(), 64);
	size_t signFlipped = 0;
	for (int i = 0; i < 64; ++i) {
		// `-0.0f == +0.0f` is TRUE in IEEE, so equality cannot ask this question; the sign bit can.
		if (!std::signbit(zl[static_cast<size_t>(i)]) || !std::signbit(zr[static_cast<size_t>(i)])) {
			++signFlipped;
		}
	}
	CHECK(signFlipped == 0);

	// (2) NOW PLAY, so the coefficients fill with something, and THEN go quiet again.
	Settings on = burst(/*mix=*/1.0f);
	on.engage = true;
	s.setTarget(on);
	double beats = 0.0;
	for (int b = 0; b < 24; ++b) {
		std::vector<float> l(512);
		std::vector<float> r(512);
		fillRamp(l, r, b * 512);
		s.setTransportClock(at120bpm(beats));
		s.processStereo(l.data(), r.data(), 512);
		beats += 512.0 / (kRate * 0.5);
	}
	REQUIRE(s.statusForTest() == Status::Playing);

	s.setTarget(off); // bypassed again: this block renders nothing at all
	std::vector<float> l(512);
	std::vector<float> r(512);
	std::vector<float> want(512);
	fillRamp(l, r, 99000);
	std::copy(l.begin(), l.end(), want.begin());
	s.setTransportClock(at120bpm(beats));
	s.processStereo(l.data(), r.data(), 512);

	size_t carriedOver = 0;
	for (size_t i = 0; i < want.size(); ++i) {
		if (l[i] != want[i]) {
			++carriedOver;
		}
	}
	CHECK(carriedOver == 0);
}

//================================================================================================//
// 3. THE SEVEN AUX OUTPUTS, AS THE CHAIN'S CORE SOURCES — decision D3(a)                          //
//================================================================================================//

TEST_CASE("the seven aux outputs reach the chain's core sources, and a bipolar one arrives as "
          "itself rather than as 2v − 1 of itself") {
	// **DECISION D3(a).** The chain's `Axes` declare `coreSources = kPerformOutputs`, and the polarity
	// of each is taken from `kOutputs` — the same table every OUTER reader of these seven is sized by.
	// A source that meant one thing inside this device and another outside it would be the same name
	// for two signals.
	//
	// **WHY NOT `Source::outerOutput`, which reads as the proper road:** trap T1, measured in
	// `test_rack_nesting.cpp` — `upstreamOf(self, output(self, n))` answers false for all seven,
	// because `Graph::reaches` returns true when `from == to`. A sibling answers true; only the
	// self-case is refused.
	Sklitter s;
	s.prepare(kRate, 4096);
	adoptChain(s);

	REQUIRE(s.postChain()->engine().axes().coreSources == kPerformOutputs);

	// THE PUCK HARD LEFT AND DOWN. `setPerformFields` takes the device's own row; the two puck
	// controls are `Param::PerfX` and `Param::PerfY`, and the row's own defaults fill the rest.
	Row row = rowOf(Settings {});
	row[static_cast<size_t>(Param::PerfX)] = -1.0f;
	row[static_cast<size_t>(Param::PerfY)] = 1.0f;
	s.setPerformFields(row.data(), row.size());

	std::vector<float> l(64);
	std::vector<float> r(64);
	fillRamp(l, r, 0);
	s.setTarget(burst());
	s.setTransportClock(at120bpm());
	s.processStereo(l.data(), r.data(), 64);

	const rack::RackEngine& inner = s.postChain()->engine();
	for (size_t n = 0; n < kPerformOutputs; ++n) {
		CAPTURE(n);
		CAPTURE(kOutputs[n].name);
		// THE CELL, SPELLED THE WAY THE ENGINE SPELLS IT: `2·raw − 1` for a bipolar core source, `raw`
		// for a unipolar one. The claim is that it equals what the device PUBLISHES, which is what an
		// outer route from this device reads.
		const float cell = kOutputs[n].bipolar ? 2.0f * inner.macro(n) - 1.0f : inner.macro(n);
		// **THE POLARITY ASSERT IS NOT REDUNDANT WITH THE VALUE ONE — MEASURED.** With the declaration
		// dropped (known-bad: `setCoreBipolar(n, false)` for all seven), the value assert stays GREEN:
		// the engine then stores the clamped raw, and at the puck's extremes `2·clamp(x,0,1) − 1` happens
		// to equal x. Only asking the engine what polarity it thinks the cell has sees it. Same shape as
		// the endpoint coincidence recorded in `test_inner_chain.cpp`, one layer up.
		CHECK(inner.macroBipolar(n) == kOutputs[n].bipolar);
		CHECK(cell == doctest::Approx(s.performOutput(n)));
	}

	// AND THE ONE THAT WOULD HAVE BEEN OUT OF RANGE: `Perf X` at −1 is a real value a hand can reach,
	// and `2·(−1) − 1 = −3` is what the cell would hold if the mapping were dropped.
	CHECK(s.performOutput(outIndex(Out::X)) == doctest::Approx(-1.0f));
	CHECK(2.0f * inner.macro(outIndex(Out::X)) - 1.0f == doctest::Approx(-1.0f));
	CHECK(inner.macro(outIndex(Out::X)) == doctest::Approx(0.0f));
}

//================================================================================================//
// 4. THE CONTAINER OPS, DRIVEN AS A HOST DRIVES THEM                                              //
//================================================================================================//

TEST_CASE("the descriptor declares a container beside its audio, mod and output roads — the first "
          "device in the registry that is all four") {
	REQUIRE(kDescriptor.audio != nullptr);
	REQUIRE(kDescriptor.mod != nullptr);
	REQUIRE(kDescriptor.container != nullptr);
	CHECK(kDescriptor.outputs.size() == kPerformOutputs);
	// EVERY OP FILLED — a host tests `container != nullptr && <op> != nullptr` at every site, so a null
	// here is a capability silently absent rather than an error.
	const device::ContainerOps& ops = *kDescriptor.container;
	CHECK(ops.adopt != nullptr);
	CHECK(ops.saveState != nullptr);
	CHECK(ops.loadState != nullptr);
	// **TWO LANES' WORTH, PLUS THE FRAME** — this device's body is a bundle since the grain lane
	// (backlog EW, gap G2), so the ceiling is not one rack's any more. It bounds how far a reader will
	// walk a length out of a file; nothing allocates it.
	CHECK(ops.maxBodyBytes == rack::bundleMaxBytes(2));
	CHECK(ops.maxBodyBytes > rack::kMaxBodyBytes); // not vacuous: the bundle really is the larger bound
	CHECK(ops.setOuterGate != nullptr);
	CHECK(ops.service != nullptr);
	CHECK(ops.setVoices != nullptr);
	CHECK(ops.forEachRouteFrom != nullptr);
	CHECK(ops.routeAmountFrom != nullptr);
	CHECK(ops.setRouteFrom != nullptr);
	CHECK(ops.nameDestination != nullptr);
}

TEST_CASE("the audio ops carry the note and the outer-source reader through to the chain — the half "
          "that `ContainerOps::setOuterGate` would otherwise be lying about") {
	// **DECLARING THE GATE WITHOUT THE READER ADMITS A ROUTE AND THEN READS NOTHING FROM IT.**
	// `ContainerOps::setOuterGate` is the message-thread half: it lets the chain's editor admit a route
	// from a source OUTSIDE this device. `AudioOps::setOuterSources` is the audio half — the reader that
	// finds that source's cell. One without the other is a silent zero, not an error, which is why both
	// are asserted here rather than left to whichever host happens to call them.
	REQUIRE(kDescriptor.audio->setOuterSources != nullptr);
	REQUIRE(kDescriptor.audio->setNoteContext != nullptr);
	REQUIRE(kDescriptor.container->setOuterGate != nullptr);

	Sklitter s;
	s.prepare(kRate, 4096);
	adoptChain(s);

	// THE NOTE IS A VALUE THIS CASE CAN READ BACK: the engine derives its gate from it, and a
	// keytracking filter inside the chain is the reason the WHOLE note is forwarded and not only the gate.
	//
	// **ASSERTED ON BOTH LANES, AND THAT IS NOT BELT-AND-BRACES — IT FOUND THREE REAL GAPS.** When the
	// grain lane arrived, `setNoteContext`, `setOuterSources` and `Sklitter::setTransportClock` all
	// still forwarded to the master lane alone, so the grain lane would have held a keytracking filter
	// that never heard a note and a tempo-synced delay that never heard the clock. Nothing was red:
	// every seam was declared, and the one chain anybody asked about worked. This is the device's own
	// recurring shape — `blendAt`'s header lists five earlier instances of a fix reaching one road and
	// stopping — so the lanes are asked SEPARATELY rather than through whichever one is nearest.
	device::NoteContext note {};
	note.gate = true;
	note.pitchSemitones = 60.0f;
	CHECK_FALSE(s.postChain()->engine().gate());
	CHECK_FALSE(s.grainChain()->engine().gate());
	kDescriptor.audio->setNoteContext(&s, note);
	CHECK(s.postChain()->engine().gate());
	CHECK(s.grainChain()->engine().gate());
	note.gate = false;
	kDescriptor.audio->setNoteContext(&s, note);
	CHECK_FALSE(s.postChain()->engine().gate());
	CHECK_FALSE(s.grainChain()->engine().gate());

	// **AND THE HOST'S CLOCK REACHES BOTH.** A tempo-synced device in either lane reads it; this device
	// is the only thing between them and the host.
	s.setTransportClock(at120bpm(4.0));
	CHECK(s.postChain()->clockForTest().secondsPerBeat == doctest::Approx(0.5));
	CHECK(s.grainChain()->clockForTest().secondsPerBeat == doctest::Approx(0.5));

	// **THE READER ITSELF IS NOT OBSERVABLE FROM HERE AND THIS CASE DOES NOT PRETEND IT IS.**
	// `RackEngine` exposes no getter for it, and what it enables — an inner control reading an outer
	// source — needs an outer host to publish one, which is `test_rack_nesting.cpp`'s ground. What is
	// checked here is the thing only this device can get wrong: that the op is declared at all. It is
	// called with an empty reader to pin that it survives one.
	kDescriptor.audio->setOuterSources(&s, device::OuterSources {});
}

TEST_CASE("a held chain saves what it holds and takes it back, through the ops table") {
	std::vector<uint8_t> body;
	const device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
		                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                             out->insert(out->end(), d, d + n);
	                             } };
	{
		Sklitter s;
		s.prepare(kRate, 4096);
		adoptChain(s);
		seatProbe(s, 2.0f);
		REQUIRE(kDescriptor.container->saveState(&s, sink));
		REQUIRE(!body.empty());
	}
	{
		Sklitter s;
		s.prepare(kRate, 4096);
		adoptChain(s);
		REQUIRE(kDescriptor.container->loadState(&s, body.data(), body.size()));
		CHECK(s.postChain()->engine().chainLength(0) == 1);
		// **AND IT PLAYS AT THE GAIN IT WAS SAVED WITH, WHICH IS A CLAIM ABOUT THE ROW AND NOT ONLY ABOUT
		// THE TAG.** A body that restored the DEVICE but not its controls would satisfy the count above
		// and sound wrong; a first draft of this case read the gain straight after the load and got the
		// CONSTRUCTION default, because a member's row reaches it when the block resolves and not when
		// it is provisioned. So the block is run first, which is also what a host does.
		std::vector<float> l(64);
		std::vector<float> r(64);
		fillRamp(l, r, 0);
		s.setTarget(burst());
		s.setTransportClock(at120bpm());
		s.processStereo(l.data(), r.data(), 64);
		Probe& probe = probeAt(
		    s.postChain()->engine().slotAtLane(s.postChain()->engine().deviceAtPosition(0)).storage.get());
		// THE LOAD'S OWN CLAIM: the field came back, so the body was read and provisioned.
		CHECK(probe.gain == doctest::Approx(2.0f));
		// **AND THE LOADED CHAIN IS LIVE — asserted by the chain RUNNING rather than by the device's
		// output, which no longer shows it.** This device is idle here (nothing engaged), so it writes
		// no wet, the lane is handed silence and the output is the input bit for bit. Before the lane
		// moved onto the wet path the chain sat on the whole output and doubled it, which is what these
		// two lines used to read.
		CHECK(probe.processed > 0);
		CHECK(l[10] == 0.001f * 10.0f);
	}
	{
		// A BODY IT CANNOT READ WHOLE IS REFUSED, and the device is left as it was.
		Sklitter s;
		s.prepare(kRate, 4096);
		adoptChain(s);
		CHECK_FALSE(kDescriptor.container->loadState(&s, body.data(), body.size() / 2));
		CHECK(s.postChain()->engine().chainLength(0) == 0);
	}
}

TEST_CASE("BOTH lanes ride in ONE body — a device seated in the GRAIN lane comes back with its "
          "controls, in the GRAIN lane") {
	// **THE HALF OF GAP G2 NOTHING WAS ASKING.** `ContainerOps::saveState` writes
	// `rack::writeChainBundle(lanesOf(...))`, which names both lanes; the case above it drives only
	// the master lane, so *"a held chain saves what it holds"* was true of one lane and untested of
	// the other. A lane a player can FILL and cannot SAVE is worse than one they cannot fill, and the
	// fx rail's second column is what makes the grain lane fillable — so this is the case that had to
	// exist before that column could ship.
	//
	// **THE TWO GAINS ARE DIFFERENT ON PURPOSE, AND THAT IS THE WHOLE INSTRUMENT.** With both probes
	// at the same gain, a load that put BOTH devices in the master lane, or swapped the two lanes'
	// contents, or wrote one lane twice, passes every count and every value below. 2 against 3 is what
	// makes "it came back in the lane it was saved from" a readable claim rather than "something came
	// back". CLAUDE.md: compute the reference by an independent restatement of the intended path.
	std::vector<uint8_t> body;
	const device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
		                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                             out->insert(out->end(), d, d + n);
	                             } };
	{
		Sklitter s;
		s.prepare(kRate, 4096);
		adoptChain(s);
		seatProbeIn(s.postChain(), 2.0f);
		seatProbeIn(s.grainChain(), 3.0f);
		REQUIRE(kDescriptor.container->saveState(&s, sink));
		REQUIRE(!body.empty());
	}
	{
		Sklitter s;
		s.prepare(kRate, 4096);
		adoptChain(s);
		REQUIRE(kDescriptor.container->loadState(&s, body.data(), body.size()));
		// ONE DEVICE IN EACH, not two in one — the count that catches a bundle reader that walked the
		// frame and put every lane's devices into the first chain it had.
		CHECK(s.postChain()->engine().chainLength(0) == 1);
		CHECK(s.grainChain()->engine().chainLength(0) == 1);

		// **THE BLOCK IS RUN BEFORE THE FIELDS ARE READ**, for the reason the case above records: a
		// member's row reaches it when the block resolves and not when it is provisioned, so a read
		// straight after the load returns the CONSTRUCTION default and says nothing about the body.
		// GRAIN MODE with the override at +100%, so the grain lane is genuinely fed and its probe's
		// `processed` is a real observation rather than a lane running on silence.
		s.setTarget(grainMode(/*sendOverride=*/1.0f));
		(void)runBlocks(s, 240, 512);
		REQUIRE(s.statusForTest() == Status::Playing);

		Probe& master = firstProbeIn(s.postChain());
		Probe& grain = firstProbeIn(s.grainChain());
		// **THE VALUES, EACH IN ITS OWN LANE.** This is the assertion; the counts above only prove it
		// is not vacuous.
		CHECK(master.gain == doctest::Approx(2.0f));
		CHECK(grain.gain == doctest::Approx(3.0f));
		// AND BOTH LOADED DEVICES ARE LIVE — a body that restored a device the engine never runs is a
		// patch that loads and does nothing.
		CHECK(master.processed > 0);
		CHECK(grain.processed > 0);
	}
}

TEST_CASE("a device built but never adopted holds a chain that refuses everything, with no error") {
	// **TRAP T5.** `adopt` is the only moment a container may build its record. The chain OBJECT exists
	// from construction — building it lazily in `processStereo` would allocate on the audio thread
	// (trap T2) — but it holds no catalogue and no bridge until a host hands one over.
	Sklitter s;
	s.prepare(kRate, 4096);
	REQUIRE(s.postChain() != nullptr);
	CHECK(s.postChain()->bridge() == nullptr);

	std::vector<float> l(64);
	std::vector<float> r(64);
	fillRamp(l, r, 0);
	const float in0 = l[0];
	s.setTarget(burst());
	s.setTransportClock(at120bpm());
	s.processStereo(l.data(), r.data(), 64);
	CHECK(l[0] == in0); // bit for bit: an unadopted chain is not a chain
}

//================================================================================================//
// 5. GAP G4 — TWO CONTAINER KINDS, ONE DEPTH COUNTER                                              //
//================================================================================================//

namespace {

/// A body for a container, written through its own ops table.
std::vector<uint8_t> bodyOf(const device::ContainerOps& ops, const void* storage) {
	std::vector<uint8_t> out;
	const device::ByteSink sink { &out, [](void* ctx, const uint8_t* d, size_t n) {
		                             static_cast<std::vector<uint8_t>*>(ctx)->insert(
		                                 static_cast<std::vector<uint8_t>*>(ctx)->end(), d, d + n);
	                             } };
	REQUIRE(ops.saveState(storage, sink));
	return out;
}

/// A rack device, built the way a host builds one, with a Probe inside it.
struct RackInstance {
	std::vector<unsigned char> raw;
	void* mem = nullptr;

	explicit RackInstance(const device::Catalogue& catalogue) {
		const device::Descriptor& d = rack::kDeviceDescriptor;
		raw.resize(d.audio->stateBytes + d.audio->stateAlign);
		mem = raw.data();
		d.audio->construct(mem);
		d.audio->prepare(mem, kRate, 4096);
		d.container->adopt(mem, catalogue);
	}
	~RackInstance() { rack::kDeviceDescriptor.audio->destruct(mem); }
	RackInstance(const RackInstance&) = delete;
	RackInstance& operator=(const RackInstance&) = delete;
};

} // namespace

TEST_CASE("the two container kinds count on ONE depth counter, which is what makes an ALTERNATING "
          "nest bounded — a rack inside SKLITTER inside a rack") {
	// **GAP G4, AND THIS CASE IS THE WITNESS THE SAME-KIND ONE COULD NOT BE.** The hole was never
	// "a container does not count its own levels"; each kind counted its own perfectly. It was that
	// they counted SEPARATELY: `rack_device.cpp` kept a `thread_local loadDepth` private to itself, so
	// two DIFFERENT kinds alternating (A → B → A → …) each saw depth 1 at every level and neither
	// guard ever fired. The document's own words: *"even two correct copies do not compose."*
	//
	// **A SKLITTER→SKLITTER CASE PASSES WITH THE BUG STILL PRESENT**, because one kind against itself
	// is exactly the case a private counter gets right. Only a case that puts one kind INSIDE THE
	// OTHER can see it, which is why this lives here — the only suite that can name both.
	//
	// **MEASURED, and the asymmetry is the reason this case exists.** With `rack_device.cpp` put back
	// on a private `thread_local` counter (the pre-fix state), this case goes RED and the whole of
	// `test_inner_chain.cpp` — seventeen cases including its own same-kind depth case — stays GREEN.
	// Seventeen green cases about a counter, and not one of them can see the defect the counter has.
	//
	// **AND IT IS A VALUE, NOT A CRASH.** The alternative witness — nest until the stack blows — is a
	// test that either kills the process or proves nothing, and it is the shape a hostile PATCH takes
	// rather than the shape a case should. The counter itself is the observable.

	// A REAL BODY FROM EACH KIND, so what is refused below is a body that would otherwise LOAD. A
	// guard that refuses everything is not a guard.
	Sklitter sk;
	sk.prepare(kRate, 4096);
	kDescriptor.container->adopt(&sk, kNesting);
	seatProbe(sk, 2.0f);
	const std::vector<uint8_t> sklitterBody = bodyOf(*kDescriptor.container, &sk);
	REQUIRE(!sklitterBody.empty());

	RackInstance rk { kNesting };
	REQUIRE(rack::bridgeOf(rk.mem) != nullptr);
	rack::bridgeOf(rk.mem)->addDevice(kProbeDesc.traits.tag);
	rack::bridgeOf(rk.mem)->drain();
	const std::vector<uint8_t> rackBody = bodyOf(*rack::kDeviceDescriptor.container, rk.mem);
	REQUIRE(!rackBody.empty());

	// NON-VACUITY: at rest, both bodies load.
	REQUIRE(rack::containerLoadDepth() == 0);
	{
		Sklitter fresh;
		fresh.prepare(kRate, 4096);
		kDescriptor.container->adopt(&fresh, kNesting);
		CHECK(kDescriptor.container->loadState(&fresh, sklitterBody.data(), sklitterBody.size()));
	}
	{
		RackInstance fresh { kNesting };
		CHECK(rack::kDeviceDescriptor.container->loadState(fresh.mem, rackBody.data(), rackBody.size()));
	}

	// **NOW STAND THE SHARED COUNTER WHERE A DEEP NEST WOULD HAVE PUT IT, and BOTH kinds must refuse.**
	// This is the whole claim: the rack device reads the same counter SKLITTER raised, so a level spent
	// by one is a level the other cannot spend again. Before the fix, the rack device read its own
	// counter — still 0 — and would have taken the body here.
	rack::containerLoadDepth() = rack::kMaxDepth - 1;
	{
		Sklitter fresh;
		fresh.prepare(kRate, 4096);
		kDescriptor.container->adopt(&fresh, kNesting);
		CHECK_FALSE(kDescriptor.container->loadState(&fresh, sklitterBody.data(), sklitterBody.size()));
	}
	{
		RackInstance fresh { kNesting };
		CHECK_FALSE(
		    rack::kDeviceDescriptor.container->loadState(fresh.mem, rackBody.data(), rackBody.size()));
	}
	rack::containerLoadDepth() = 0;

	// AND THE GUARD UNWINDS on both roads: a refused load must not leave the counter raised, or the
	// next load in the same process starts one level down for no reason.
	CHECK(rack::containerLoadDepth() == 0);
}

TEST_CASE("an ALTERNATING nest is buildable and loads — SKLITTER holding a rack holding SKLITTER — "
          "which is what makes the counter's sharing matter rather than theoretical") {
	// **THE STRUCTURE THE GAP IS ABOUT, BUILT.** Until SKLITTER gained a chain there was one container
	// kind and nothing to alternate with; the hole was real and unreachable. This is the reachable
	// version, and it is a thing a hand can assemble in the editor: put a Chain in SKLITTER's post
	// lane, and a SKLITTER in that chain.
	Sklitter outer;
	outer.prepare(kRate, 4096);
	kDescriptor.container->adopt(&outer, kNesting);

	rack::RackBridge* chain = outer.postChain()->bridge();
	REQUIRE(chain != nullptr);
	const DeviceInstanceId innerRack = chain->addDevice(composite::kTraits.tag);
	REQUIRE(innerRack != kNoInstance);
	chain->drain();
	const size_t lane = outer.postChain()->engine().slotOf(innerRack);
	REQUIRE(lane != rack::RackEngine::kNoSlot);

	// THE RACK INSIDE SKLITTER, and a SKLITTER inside THAT — three levels, two kinds, alternating.
	rack::RackBridge* innerBridge = rack::bridgeOf(outer.postChain()->engine().slotAtLane(lane).storage.get());
	REQUIRE(innerBridge != nullptr);
	const DeviceInstanceId innermost = innerBridge->addDevice(kTraits.tag);
	REQUIRE(innermost != kNoInstance);
	innerBridge->drain();
	CHECK(rack::engineOf(outer.postChain()->engine().slotAtLane(lane).storage.get()) != nullptr);

	// IT SAVES AND IT COMES BACK, which is the road the depth guard sits on: `loadState` recursing
	// through one kind into the other is exactly where the two counters used to disagree.
	const std::vector<uint8_t> body = bodyOf(*kDescriptor.container, &outer);
	REQUIRE(!body.empty());
	Sklitter reloaded;
	reloaded.prepare(kRate, 4096);
	kDescriptor.container->adopt(&reloaded, kNesting);
	REQUIRE(kDescriptor.container->loadState(&reloaded, body.data(), body.size()));
	CHECK(reloaded.postChain()->engine().chainLength(0) == 1);
	CHECK(rack::containerLoadDepth() == 0);

	// AND IT RENDERS THROUGH ALL THREE LEVELS WITHOUT EXPLODING.
	std::vector<float> l(64);
	std::vector<float> r(64);
	fillRamp(l, r, 0);
	reloaded.setTarget(burst());
	reloaded.setTransportClock(at120bpm());
	reloaded.processStereo(l.data(), r.data(), 64);
	CHECK(std::isfinite(l[63]));
}

//================================================================================================//
// 4. THE GRAIN LANE — backlog EW, and it is different in KIND from the master lane above          //
//                                                                                                 //
// The master lane sees the summed output. This one is fed GRAIN BY GRAIN, by a per-grain decision //
// (`SlicePlan::shouldSendToLane`), so what reaches it has been chosen. Its output rejoins the wet  //
// BEFORE the master lane, by design, so the master's input — and                             //
// its dry path — includes it.                                                                     //
//                                                                                                 //
// **EVERY CLAIM HERE IS A VALUE.** "The lane changed the sound" would pass with the lane wired     //
// anywhere at all, and a lane at unity gain is INAUDIBLE by design — so a difference check is both //
// too weak and, for the transparency claim, exactly backwards.                                     //
//================================================================================================//

TEST_CASE("the grain lane is fed in GRAIN MODE, which is the mode the per-slice road cannot reach — "
          "and the override decides how much of the cloud goes down it") {
	// **THIS IS THE Q0 DECISION, AS A NUMBER.** `shouldDelay` has exactly one reader, `advanceSlice`,
	// which `playGrain` never calls — so the echo's road does not exist in Grain and the lane would
	// have been dark there if it had been built on that reader. It is built on the PLAN instead, which
	// `grainPlanFor` already produced per voice per wrap, so Grain is served by the same one decision.
	// Dark-in-Grain is not acceptable: the grain fx applies in all modes.
	Sklitter every;
	Sklitter none;
	every.prepare(kRate, 4096);
	none.prepare(kRate, 4096);
	adoptChain(every);
	adoptChain(none);
	Probe& everyProbe = seatProbeIn(every.grainChain(), 1.0f);
	Probe& noneProbe = seatProbeIn(none.grainChain(), 1.0f);

	every.setTarget(grainMode(/*sendOverride=*/1.0f));  // every grain down the lane
	none.setTarget(grainMode(/*sendOverride=*/-1.0f));  // none of them
	(void)runBlocks(every, 240, 512);
	(void)runBlocks(none, 240, 512);
	REQUIRE(every.statusForTest() == Status::Playing);
	REQUIRE(none.statusForTest() == Status::Playing);

	// THE LANE RUNS EITHER WAY — it is an insert, not a thing that is switched off.
	REQUIRE(everyProbe.processed > 0);
	REQUIRE(noneProbe.processed > 0);

	// **AT −100% THE LANE IS HANDED SILENCE, SAMPLE FOR SAMPLE.** `overriddenProbability(p, −1)` is
	// exactly 0 and `evalBoolFast`'s comparison is `< 0`, which nothing satisfies — so "never" is
	// literally never rather than nearly never, and the count below is 0 rather than small.
	size_t quiet = 0;
	for (const float v : noneProbe.seen) {
		quiet += (v == 0.0f) ? 1 : 0;
	}
	CHECK(quiet == noneProbe.seen.size());

	// **AND AT +100% IT IS HANDED THE CLOUD.**
	//
	// **THE BAR IS NOT THE RUN, WHICH IS WHY THIS IS NOT A FRACTION OF `seen.size()`.** Grain is a
	// looper mode: it captures a whole BAR before it plays anything, and a bar at 120 BPM is 96,000
	// frames of the ~123,000 this case pushes through. So most of what the lane was handed is the
	// silence of the capture, and a fraction of the total would be measuring the capture's length.
	// MEASURED here: 14,623 non-zero samples against the `none` device's 0.
	//
	// The bar is several GRAINS' worth instead — `refreshGrainGeometry` floors a grain at `kMinGrain`
	// = 1024 frames, so this cannot be carried by one stray window, and it is a number about the thing
	// under test rather than about how long the case happens to run.
	size_t loud = 0;
	for (const float v : everyProbe.seen) {
		loud += (v != 0.0f) ? 1 : 0;
	}
	CAPTURE(loud);
	CAPTURE(everyProbe.seen.size());
	CHECK(loud > 4 * 1024);

	// **AND THE DECISION IS READ AS A VALUE, not inferred from the audio** — at +100% both of Grain's
	// two voices are in the lane, at −100% neither. `playGrain` runs exactly two crossfaded readers,
	// so these two flags are the whole of "which grains took the detour" in this mode.
	CHECK(every.grainLaneVoiceForTest(false));
	CHECK(every.grainLaneVoiceForTest(true));
	CHECK_FALSE(none.grainLaneVoiceForTest(false));
	CHECK_FALSE(none.grainLaneVoiceForTest(true));
}

TEST_CASE("a grain lane holding NOTHING is inaudible however many grains are diverted into it — the "
          "lane is a diversion, not a parallel send") {
	// **THIS CASE GUARDS A DECISION, NOT JUST SOME ARITHMETIC.** Diversion is right, and it stays.
	// Backlog EW's own wording points the other way — the lane picks grains *"exactly as
	// `shouldDelay` picks which go to the internal delay"*, and that delay is a parallel send — so an
	// empty lane being bit-for-bit transparent is the one observable property that tells the two
	// designs apart, and it is what stops somebody restoring EW's reading in six months.
	//
	// **KNOWN-BAD 6 IS THE INSTRUMENT: build the lane as a parallel send and this case goes red**
	// (measured; nothing else in the suite moves). That is what makes the sentence above a guard
	// rather than a note.
	//
	// The property itself: a grain that goes down the lane LEAVES the wet and comes back as the lane's
	// output. As a parallel send — the grain staying in the wet and the return added on top — merely
	// putting an EMPTY lane on the device would make every chosen grain 6 dB louder, which is the
	// master lane's own bit-for-bit promise failing in the place a player notices first.
	//
	// **GRAIN MODE IS THE VENUE FOR A SPECIFIC REASON.** The override steers the internal echo as well
	// as the lane, so in a looper mode +1 against −1 would also change the echo and the two outputs
	// would differ for a reason that is not this one. `playGrain` has no echo at all, so in Grain the
	// override moves the lane and nothing else — which makes this comparison attributable.
	Sklitter every;
	Sklitter none;
	every.prepare(kRate, 4096);
	none.prepare(kRate, 4096);
	adoptChain(every); // both lanes adopted and both EMPTY
	adoptChain(none);

	every.setTarget(grainMode(/*sendOverride=*/1.0f));
	none.setTarget(grainMode(/*sendOverride=*/-1.0f));
	const std::vector<float> withLane = runBlocks(every, 240, 512);
	const std::vector<float> withoutLane = runBlocks(none, 240, 512);
	REQUIRE(every.statusForTest() == Status::Playing);
	REQUIRE(none.statusForTest() == Status::Playing);
	REQUIRE(withLane.size() == withoutLane.size());

	size_t differing = 0;
	for (size_t i = 0; i < withLane.size(); ++i) {
		if (withLane[i] != withoutLane[i]) {
			++differing;
		}
	}
	CHECK(differing == 0);

	// **AND THE ASSERTION THAT PROVES IT IS NOT VACUOUS**, which matters more here than usual: the
	// claim is that two outputs are IDENTICAL, and the cheapest way for that to be true by accident is
	// for the device to be producing silence, or for nothing to have gone down the lane at all. A
	// third device says the diversion really happened at +1 and really did not at −1.
	size_t moving = 0;
	for (const float v : withLane) {
		moving += (v != 0.0f) ? 1 : 0;
	}
	CHECK(moving > 4 * 1024); // several grains, not a fraction of a run that is mostly capture
	CHECK(every.grainLaneVoiceForTest(false));
	CHECK_FALSE(none.grainLaneVoiceForTest(false));
}
