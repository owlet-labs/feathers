/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// CHAIN ACCESS SITES (backlog NR): the slot's furniture — a Mix and a
// wet Gain the ENGINE blends — and its PRE and POST chains. Every claim is a VALUE computed apart from the code (a
// difference between two renders says only that something changed): the blend's coefficients, the order the three
// stages ran in, where the dry waits, the latency the engine reports and the lifetime of a site the converger owns.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "backplane/container_bodies.h"
#include "fx/harm/harm_types.h"
#include "rack/inner_chain.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"
#include "rack/site_chains.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace feathers;
using rack::InnerChain;
using rack::RackEngine;
using rack::SiteChains;

namespace {

constexpr double kRate = 48000.0;
/// 5 ms at `kRate`, restated: the ramp's length is a claim.
constexpr int kRamp = 240;

//------------------------------------------------------------------------------------------------//
// FIXTURES                                                                                         //
//------------------------------------------------------------------------------------------------//

/// ONE STATE FOR EVERY PROBE: what its row said, and a ring for the latent one.
struct Probe {
	std::array<float, 4> f {};
	std::array<float, 2048> ringL {};
	std::array<float, 2048> ringR {};
	size_t write = 0;
	int processed = 0;
};
Probe& at(void* s) { return *static_cast<Probe*>(s); }

template <void (*Run)(Probe&, float*, float*, int)>
constexpr device::AudioOps opsOf(uint32_t (*latency)(const void*, const float*, size_t, double) = nullptr,
                                 bool (*tail)(const void*) = nullptr) {
	return device::AudioOps {
		.stateBytes = sizeof(Probe),
		.stateAlign = alignof(Probe),
		.construct = [](void* s) { new (s) Probe {}; },
		.destruct = [](void* s) { static_cast<Probe*>(s)->~Probe(); },
		.prepare = [](void*, double, int) {},
		.reset = [](void*) {},
		.setSlot = nullptr,
		.setFields =
		    [](void* s, const float* f, size_t n) {
			    for (size_t i = 0; i < at(s).f.size(); ++i) {
				    at(s).f[i] = i < n ? f[i] : 0.0f;
			    }
		    },
		.processStereo =
		    [](void* s, float* l, float* r, int frames) {
			    Run(at(s), l, r, frames);
			    ++at(s).processed;
		    },
		.latencySamples = latency,
		.holdsTail = tail,
	};
}

/// x · f[0] — the device under test: control 0 its gain, 1 its Mix role, 2 its wet Gain role.
void runGain(Probe& p, float* l, float* r, int n) {
	for (int i = 0; i < n; ++i) {
		l[i] *= p.f[0];
		r[i] *= p.f[0];
	}
}
/// x + f[0] — what a site's lanes hold, so the ORDER of the stages is in the number.
void runOffset(Probe& p, float* l, float* r, int n) {
	for (int i = 0; i < n; ++i) {
		l[i] += p.f[0];
		r[i] += p.f[0];
	}
}
/// x · 3 whatever f[0] says — a device that makes its own mix (`mixInDevice`).
void runTriple(Probe& p, float* l, float* r, int n) {
	(void)p;
	for (int i = 0; i < n; ++i) {
		l[i] *= 3.0f;
		r[i] *= 3.0f;
	}
}
/// x delayed by f[0] samples — a latent device, whose row declares its latency.
void runDelay(Probe& p, float* l, float* r, int n) {
	const auto d = static_cast<size_t>(p.f[0]);
	for (int i = 0; i < n; ++i) {
		p.ringL[p.write % p.ringL.size()] = l[i];
		p.ringR[p.write % p.ringR.size()] = r[i];
		l[i] = p.write >= d ? p.ringL[(p.write - d) % p.ringL.size()] : 0.0f;
		r[i] = p.write >= d ? p.ringR[(p.write - d) % p.ringR.size()] : 0.0f;
		++p.write;
	}
}
uint32_t delayLatency(const void*, const float* row, size_t n, double) {
	return n > 0 ? static_cast<uint32_t>(row[0]) : 0u;
}
bool alwaysTail(const void*) { return true; }

constexpr device::AudioOps kGainOps = opsOf<&runGain>();
constexpr device::AudioOps kOffsetOps = opsOf<&runOffset>();
constexpr device::AudioOps kTripleOps = opsOf<&runTriple>();
constexpr device::AudioOps kDelayOps = opsOf<&runDelay>(&delayLatency);
constexpr device::AudioOps kTailOps = opsOf<&runOffset>(nullptr, &alwaysTail);

constexpr std::array<device::ParamDesc, 3> kFurnishedParams { {
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {}, true },
	{ "mix", "Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true },
	{ "wet_gain", "Wet Gain", 0.0, 1.0, 0.5, false, device::Unit::Fader, {}, true },
} };
constexpr std::array<device::ParamDesc, 3> kLatentParams { {
	{ "latency", "Latency", 0.0, 1024.0, 0.0, true, device::Unit::Number, {} },
	{ "mix", "Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true },
	{ "wet_gain", "Wet Gain", 0.0, 1.0, 0.5, false, device::Unit::Fader, {}, true },
} };
constexpr std::array<device::ParamDesc, 1> kOffsetParams { {
	{ "offset", "Offset", -100.0, 100.0, 0.0, false, device::Unit::Number, {}, true },
} };
constexpr std::array<device::ParamDesc, 1> kOwnMixParams { {
	{ "mix", "Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true },
} };

const device::Descriptor kFurnished {
	.traits = { .tag = "nrMx", .name = "Furnished", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1,
	            .mixControl = 1, .wetGainControl = 2 },
	.params = kFurnishedParams, .outputs = {}, .audio = &kGainOps,
};
const device::Descriptor kLatent {
	.traits = { .tag = "nrLt", .name = "Latent", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1,
	            .mixControl = 1, .wetGainControl = 2 },
	.params = kLatentParams, .outputs = {}, .audio = &kDelayOps,
};
const device::Descriptor kOffset {
	.traits = { .tag = "nrOf", .name = "Offset", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = kOffsetParams, .outputs = {}, .audio = &kOffsetOps,
};
const device::Descriptor kOwnMix {
	.traits = { .tag = "nrDv", .name = "Own Mix", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1,
	            .mixControl = 0, .mixInDevice = true },
	.params = kOwnMixParams, .outputs = {}, .audio = &kTripleOps,
};
const device::Descriptor kGenerator {
	.traits = { .tag = "nrGn", .name = "Generator", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1,
	            .siteLanes = device::Traits::SiteLanes::PostOnly },
	.params = kOffsetParams, .outputs = {}, .audio = &kOffsetOps,
};
const device::Descriptor kTail {
	.traits = { .tag = "nrTl", .name = "Tail", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = kOffsetParams, .outputs = {}, .audio = &kTailOps,
};

constexpr std::array<device::ParamDesc, 4> kPolyParams { {
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {}, true },
	{ "mix", "Mix", 0.0, 1.0, 1.0, false, device::Unit::Percent, {}, true },
	{ "wet_gain", "Wet Gain", 0.0, 1.0, 0.5, false, device::Unit::Fader, {}, true },
	{ "poly", "Poly", 0.0, 1.0, 1.0, true, device::Unit::Number, {}, true },
} };
const device::Descriptor kPoly {
	.traits = { .tag = "nrPv", .name = "Poly", .hasCard = false, .hasExpanded = false, .perVoiceControl = 3,
	            .mixControl = 1, .wetGainControl = 2 },
	.params = kPolyParams, .outputs = {}, .audio = &kGainOps,
};

/// A MODULATOR THAT PUTS OUT ITS OWN CONTROL — a held value standing in for an LFO, so a route's arrival is a number
/// known in advance (backlog NZ).
struct Held {
	float value = 0.0f;
};
constexpr device::ModOps kHeldOps {
	.stateBytes = sizeof(Held),
	.stateAlign = alignof(Held),
	.construct = [](void* s) { new (s) Held {}; },
	.destruct = [](void* s) { static_cast<Held*>(s)->~Held(); },
	.prepare = [](void*, double) {},
	.reset = [](void*) {},
	.setFields = [](void* s, const float* f, size_t n) { static_cast<Held*>(s)->value = n > 0 ? f[0] : 0.0f; },
	.blockSamples = 64,
	.tick = [](void*, const device::ModTickContext&) {},
	.outputAt = [](const void* s, size_t) { return static_cast<const Held*>(s)->value; },
	.playhead = nullptr,
};
constexpr std::array<device::ParamDesc, 1> kHeldParams { {
	{ "value", "Value", -1.0, 1.0, 0.0, false, device::Unit::Number, {} },
} };
constexpr std::array<device::SourceDesc, 1> kHeldOutputs { { { "out", "Out" } } };
const device::Descriptor kHeld {
	.traits = { .tag = "nrLf", .name = "Held", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = kHeldParams, .outputs = kHeldOutputs, .mod = &kHeldOps,
};

const device::Descriptor* findProbe(void*, const char* tag) {
	for (const device::Descriptor* d : { &kFurnished, &kLatent, &kOffset, &kOwnMix, &kGenerator, &kTail, &kPoly, &kHeld,
	                                   &fx::harm::kDescriptor }) {
		if (device::sameTag(tag, d->traits.tag)) {
			return d;
		}
	}
	return nullptr;
}
const device::Catalogue kProbes { nullptr, &findProbe };

/// ONE HOST BLOCK THROUGH A BARE ENGINE.
void run(RackEngine& e, std::vector<float>& l, std::vector<float>& r) {
	e.processStereo(l.data(), r.data(), static_cast<int>(l.size()), backplane::unknownBlock());
}

/// A SITE INSTALLED BY HAND on a bare engine's device — the drain's half, as the converger would drive it.
void install(RackEngine& e, DeviceInstanceId id, SiteChains* site) {
	e.offerSite(new RackEngine::SiteOffer { id, site, nullptr });
	e.adoptSites();
	for (RackEngine::SiteOffer* spent = e.takeSpentSiteOffers(); spent != nullptr;) {
		RackEngine::SiteOffer* next = spent->next;
		delete spent;
		spent = next;
	}
}

/// A device in one of a site's lanes, at `offset` — through the lane's bridge (its record is what it runs).
void seat(InnerChain& lane, const char* tag, float control0) {
	rack::RackBridge* b = lane.bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, 0, control0);
}

} // namespace

//------------------------------------------------------------------------------------------------//
// THE FURNITURE                                                                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NR: a device at its furniture defaults with no site is the device bit for bit — negative zero and all") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kFurnished.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 1.5f);
	std::vector<float> l(256);
	std::vector<float> r(256);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = std::sin(0.03f * static_cast<float>(i));
		r[i] = -l[i];
	}
	l[7] = -0.0f; // `x·1 + y·0` would make this +0
	const std::vector<float> inL = l;
	const std::vector<float> inR = r;
	run(e, l, r);
	for (size_t i = 0; i < l.size(); ++i) {
		CHECK(l[i] == inL[i] * 1.5f);
		CHECK(r[i] == inR[i] * 1.5f);
	}
	CHECK(std::signbit(l[7]));

	// **AND THE SHAPE A BLEND AT THE IDENTITY WOULD BREAK**: a wet of −0 against a positive dry. `w·1 + d·0` is
	// `−0 + +0`, which is +0 — the only value that tells "skipped" from "blended at 1 and 0".
	e.setField(lane, 0, -0.0f);
	std::fill(l.begin(), l.end(), 0.5f);
	std::fill(r.begin(), r.end(), 0.5f);
	run(e, l, r);
	CHECK(l[0] == 0.0f);
	CHECK(std::signbit(l[0]));
	CHECK(std::signbit(r[255]));
}

TEST_CASE("NR: the slot blends the declared Mix and wet Gain — out = wet·mix·gain + in·(1 − mix), by value") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kFurnished.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 2.0f);  // the device: x · 2
	e.setField(lane, 1, 0.25f); // Mix 25%
	e.setField(lane, 2, 0.75f); // wet Gain at 3/4 of the fader: (2·0.75)² = 2.25
	const float wet = 0.25f * (1.5f * 1.5f);
	const float dry = 1.0f - 0.25f;
	std::vector<float> l(128);
	std::vector<float> r(128);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = 0.01f * static_cast<float>(i) - 0.5f;
		r[i] = 0.3f;
	}
	const std::vector<float> inL = l;
	const std::vector<float> inR = r;
	run(e, l, r); // a device's first block starts AT its Mix: nothing glides in from the identity
	for (size_t i = 0; i < l.size(); ++i) {
		CHECK(std::abs(l[i] - ((inL[i] * 2.0f) * wet + inL[i] * dry)) <= 1e-6f); // Apple arm64 fuses a·b + c (FMA); the last bit differs
		CHECK(std::abs(r[i] - ((inR[i] * 2.0f) * wet + inR[i] * dry)) <= 1e-6f);
	}
}

TEST_CASE("NR: a Mix that moves ramps over 5 ms counted in samples, and lands exactly on its target") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kFurnished.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 0.0f); // the device silences, so the output IS the dry coefficient times the input
	std::vector<float> l(512, 1.0f);
	std::vector<float> r(512, 1.0f);
	run(e, l, r);
	CHECK(l[0] == 0.0f); // Mix 100%: the wet, which is silence
	e.setField(lane, 1, 0.5f);
	std::fill(l.begin(), l.end(), 1.0f);
	std::fill(r.begin(), r.end(), 1.0f);
	run(e, l, r);
	// THE DRY COEFFICIENT, stepping 0 → 0.5 over kRamp samples: sample k has taken k+1 steps.
	CHECK(l[0] == doctest::Approx(0.5f / kRamp).epsilon(1e-4));
	CHECK(l[kRamp / 2 - 1] == doctest::Approx(0.25f).epsilon(1e-4));
	CHECK(l[kRamp - 2] < 0.5f);
	CHECK(l[kRamp - 1] == 0.5f); // landed, exactly
	CHECK(l[511] == 0.5f);
}

TEST_CASE("NR: a device that makes its own mix is never blended by the slot, whatever its Mix says") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kOwnMix.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 0.0f); // Mix 0% — the slot would hand back the input
	std::vector<float> l(64, 0.5f);
	std::vector<float> r(64, 0.5f);
	run(e, l, r);
	CHECK(l[0] == 1.5f);
	CHECK(r[63] == 1.5f);
}

TEST_CASE("NR: the furniture's dry waits for a latent device — an impulse at Mix 50% comes out ONCE, at the latency") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kLatent.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 64.0f);
	e.setField(lane, 1, 0.5f);
	CHECK(e.latencySamples(kRate) == 64u);
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	run(e, l, r);
	// UNALIGNED this is two half-height impulses, at 0 and at 64: a comb.
	for (size_t i = 0; i < l.size(); ++i) {
		CHECK(l[i] == (i == 64 ? 1.0f : 0.0f));
	}
}

//------------------------------------------------------------------------------------------------//
// THE SITE                                                                                         //
//------------------------------------------------------------------------------------------------//

TEST_CASE("NR: PRE runs before the device and POST after it, and POST is INSIDE the Mix — a wet-only insert") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kFurnished.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 2.0f);
	const DeviceInstanceId id = e.slotAtLane(lane).id;
	SiteChains site { id, kProbes, kRate, 4096 };
	seat(site.pre(), kOffset.traits.tag, 1.0f);
	seat(site.post(), kOffset.traits.tag, 0.5f);
	install(e, id, &site);
	REQUIRE(e.siteOf(id) == &site);

	std::vector<float> l(64, 0.25f);
	std::vector<float> r(64, 0.25f);
	run(e, l, r); // the lanes' own drains land their devices
	std::fill(l.begin(), l.end(), 0.25f);
	std::fill(r.begin(), r.end(), 0.25f);
	run(e, l, r);
	CHECK(l[0] == (0.25f + 1.0f) * 2.0f + 0.5f); // in that order: 3.0, where any other order is not
	CHECK(r[63] == 3.0f);

	// AT MIX 50% THE DRY IS WHAT ENTERED THE SLOT, BEFORE PRE, and POST is on the wet alone.
	e.setField(lane, 1, 0.5f);
	for (int block = 0; block < 8; ++block) { // past the ramp
		std::fill(l.begin(), l.end(), 0.25f);
		std::fill(r.begin(), r.end(), 0.25f);
		run(e, l, r);
	}
	CHECK(l[0] == 3.0f * 0.5f + 0.25f * 0.5f);
}

TEST_CASE("NR: a generator's site runs POST and never PRE (D5)") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kGenerator.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 0.25f);
	const DeviceInstanceId id = e.slotAtLane(lane).id;
	SiteChains site { id, kProbes, kRate, 4096 };
	seat(site.pre(), kOffset.traits.tag, 10.0f);
	seat(site.post(), kOffset.traits.tag, 0.5f);
	install(e, id, &site);
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	run(e, l, r);
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	run(e, l, r);
	CHECK(l[0] == 0.75f); // 0 + 0.25 + 0.5; a PRE that ran would add 10
}

TEST_CASE("NR: Harm's site runs PRE before it (its notch and high-pass process the passing signal) — a PRE "
          "device changes what reaches it, by value") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(fx::harm::kTraits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	const DeviceInstanceId id = e.slotAtLane(lane).id;
	REQUIRE(rack::siteLanesOf(fx::harm::kDescriptor) == device::Traits::SiteLanes::PreAndPost);
	e.setField(lane, static_cast<size_t>(fx::harm::Param::Width), 0.0f); // no notch: the identity on what enters it
	std::vector<float> l(64, 0.25f);
	std::vector<float> r(64, 0.25f);
	run(e, l, r);
	// AT LEVEL 0 AND WIDTH 0 HARM IS THE IDENTITY, so the signal is what entered it.
	REQUIRE(l[63] == 0.25f);
	SiteChains site { id, kProbes, kRate, 4096 };
	seat(site.pre(), kOffset.traits.tag, 1.0f);
	seat(site.post(), kOffset.traits.tag, 0.5f);
	install(e, id, &site);
	run(e, l, r); // the lanes' own drains land their devices
	std::fill(l.begin(), l.end(), 0.25f);
	std::fill(r.begin(), r.end(), 0.25f);
	run(e, l, r);
	CHECK(l[63] == 0.25f + 1.0f + 0.5f); // PRE's +1 reaches Harm, POST's +0.5 follows; a skipped PRE would give 0.75
	CHECK(r[0] == 1.75f);
}

TEST_CASE("NR: a site's latency is the slot's — device + PRE + POST — and the dry waits for all of it") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kLatent.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 16.0f);
	const DeviceInstanceId id = e.slotAtLane(lane).id;
	SiteChains site { id, kProbes, kRate, 4096 };
	seat(site.pre(), kLatent.traits.tag, 8.0f);
	seat(site.post(), kLatent.traits.tag, 32.0f);
	install(e, id, &site);
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	run(e, l, r); // the lanes' devices land
	CHECK(e.latencySamples(kRate) == 16u + 8u + 32u);
	e.setField(lane, 1, 0.5f);
	// A second of silence through it, so every ring is at rest and the ramp has run out at Mix 50%.
	for (int block = 0; block < 4; ++block) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		run(e, l, r);
	}
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	run(e, l, r);
	for (size_t i = 0; i < l.size(); ++i) {
		CHECK(l[i] == (i == 56 ? 1.0f : 0.0f));
	}
}

TEST_CASE("NR: a site holds the slot's tail while a device inside it does, and not before it is installed") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kOffset.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	const DeviceInstanceId id = e.slotAtLane(lane).id;
	SiteChains site { id, kProbes, kRate, 4096 };
	seat(site.post(), kTail.traits.tag, 0.0f);
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	site.post().process(l.data(), r.data(), 64); // the lane's device lands
	CHECK_FALSE(e.holdsTail());
	install(e, id, &site);
	CHECK(e.holdsTail());
	install(e, id, nullptr);
	CHECK(e.siteOf(id) == nullptr);
	CHECK_FALSE(e.holdsTail());
}

TEST_CASE("NR: the drain installs a site only into the slot of the device it belongs to") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(kRate, 4096);
	const size_t a = e.addDevice(kOffset.traits.tag);
	const size_t b = e.addDevice(kOffset.traits.tag);
	REQUIRE(a != RackEngine::kNoSlot);
	REQUIRE(b != RackEngine::kNoSlot);
	const DeviceInstanceId idA = e.slotAtLane(a).id;
	const DeviceInstanceId idB = e.slotAtLane(b).id;
	SiteChains site { idA, kProbes, kRate, 4096 };
	install(e, idB, &site); // offered under the wrong device: refused
	CHECK(e.siteOf(idB) == nullptr);
	install(e, idA, &site);
	CHECK(e.siteOf(idA) == &site);
	// AND A REMOVAL LETS GO OF IT WITHOUT FREEING IT — the site is still this case's, whole.
	REQUIRE(e.removeDeviceAtLane(a));
	CHECK_FALSE(e.holdsSite(&site));
	CHECK(site.owner() == idA);
}

TEST_CASE("NR: under Poly the device and its furniture run in the voices with no PRE, and POST runs on their sum (D4)") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(1);
	e.prepare(kRate, 4096);
	const size_t lane = e.addDevice(kPoly.traits.tag);
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 2.0f);
	e.setField(lane, 1, 0.5f); // the Mix, blended per voice
	const DeviceInstanceId id = e.slotAtLane(lane).id;
	SiteChains site { id, kProbes, kRate, 4096 };
	seat(site.pre(), kOffset.traits.tag, 10.0f);
	seat(site.post(), kOffset.traits.tag, 0.5f);
	install(e, id, &site);
	std::vector<float> voiceL(64);
	std::vector<float> voiceR(64);
	std::vector<float> busL(64);
	std::vector<float> busR(64);
	const auto blockOf = [&](float voiceIn, float busIn) {
		std::fill(voiceL.begin(), voiceL.end(), voiceIn);
		std::fill(voiceR.begin(), voiceR.end(), voiceIn);
		std::fill(busL.begin(), busL.end(), busIn);
		std::fill(busR.begin(), busR.end(), busIn);
		e.beginBlock(64, backplane::unknownBlock());
		e.processVoice(0, voiceL.data(), voiceR.data(), 64, device::NoteContext {});
		e.processBus(busL.data(), busR.data(), 64);
		e.endBlock();
	};
	blockOf(0.0f, 0.0f); // the lanes' devices land
	blockOf(0.25f, 1.0f);
	REQUIRE(e.polyForTest(lane));
	// THE VOICE: 0.25 · 2 at Mix 50% against itself — no PRE (which would add 10).
	CHECK(voiceL[0] == (0.25f * 2.0f) * 0.5f + 0.25f * 0.5f);
	// THE BUS IS THE VOICES' SUM, and POST runs on it at the device's place: 1 + 0.5.
	CHECK(busL[0] == 1.5f);
	CHECK(busR[63] == 1.5f);
}

//------------------------------------------------------------------------------------------------//
// THE HOST: A SITE THE CONVERGER BUILDS, OFFERS, SAVES AND FREES                                    //
//------------------------------------------------------------------------------------------------//

namespace {

/// A HOST WITH A RECORD — an inner chain, whose bridge is a `RackBridge` over its engine — holding one furnished
/// device at gain 2.
DeviceInstanceId hostWithDevice(InnerChain& host) {
	host.prepare(kRate, 4096);
	host.adopt(kProbes);
	rack::RackBridge* b = host.bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(kFurnished.traits.tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, 0, 2.0f);
	return id;
}

SiteChains* siteOfHost(InnerChain& host, DeviceInstanceId id) {
	return static_cast<SiteChains*>(host.bridge()->containerSurfaceOf(siteKeyOf(id)));
}

void block(InnerChain& host, std::vector<float>& l, std::vector<float>& r, float fill) {
	std::fill(l.begin(), l.end(), fill);
	std::fill(r.begin(), r.end(), fill);
	host.process(l.data(), r.data(), static_cast<int>(l.size()));
}

} // namespace

TEST_CASE("NR: a site is made on first use — nothing before it, the host's converger builds it, the drain installs it") {
	InnerChain host { rack::kRackAxes };
	const DeviceInstanceId id = hostWithDevice(host);
	rack::RackBridge* b = host.bridge();
	std::vector<float> l(64);
	std::vector<float> r(64);
	block(host, l, r, 0.25f);
	CHECK_FALSE(b->hasSite(id));
	CHECK(siteOfHost(host, id) == nullptr);
	CHECK(host.engine().siteOf(id) == nullptr);
	CHECK(l[0] == 0.5f);

	REQUIRE(b->openSite(id));
	CHECK(b->hasSite(id));
	SiteChains* site = siteOfHost(host, id);
	REQUIRE(site != nullptr);
	block(host, l, r, 0.25f);
	CHECK(host.engine().siteOf(id) == site);
	seat(site->pre(), kOffset.traits.tag, 1.0f);
	block(host, l, r, 0.25f); // the lane's drain lands it
	block(host, l, r, 0.25f);
	CHECK(l[0] == 2.5f); // (0.25 + 1) · 2
	CHECK(site->deviceCount(SiteChains::kPreLane) == 1u);
}

TEST_CASE("NR: a site saves with its device and loads back into a fresh host, chains and all") {
	std::vector<uint8_t> saved;
	DeviceInstanceId id = kNoInstance;
	{
		InnerChain host { rack::kRackAxes };
		id = hostWithDevice(host);
		REQUIRE(host.bridge()->openSite(id));
		SiteChains* site = siteOfHost(host, id);
		REQUIRE(site != nullptr);
		seat(site->post(), kOffset.traits.tag, 0.5f);
		std::vector<float> l(64);
		std::vector<float> r(64);
		block(host, l, r, 0.0f);
		const device::ByteSink sink { &saved, [](void* ctx, const uint8_t* data, size_t n) {
			                             auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                             v->insert(v->end(), data, data + n);
		                             } };
		REQUIRE(host.saveState(sink));
	}
	InnerChain again { rack::kRackAxes };
	again.prepare(kRate, 4096);
	again.adopt(kProbes);
	REQUIRE(again.loadState(saved.data(), saved.size()));
	CHECK(again.bridge()->hasSite(id));
	std::vector<float> l(64);
	std::vector<float> r(64);
	block(again, l, r, 0.25f);
	block(again, l, r, 0.25f);
	SiteChains* site = siteOfHost(again, id);
	REQUIRE(site != nullptr);
	CHECK(site->deviceCount(SiteChains::kPostLane) == 1u);
	CHECK(again.engine().siteOf(id) == site);
	CHECK(l[0] == 1.0f); // 0.25 · 2 + 0.5
}

TEST_CASE("NR: removing the device frees its site once nothing holds it, and not before the drain has let it go") {
	InnerChain host { rack::kRackAxes };
	const DeviceInstanceId id = hostWithDevice(host);
	rack::RackBridge* b = host.bridge();
	REQUIRE(b->openSite(id));
	std::vector<float> l(64);
	std::vector<float> r(64);
	block(host, l, r, 0.0f);
	REQUIRE(b->siteCountForTest() == 1u);
	b->removeDevice(id);
	CHECK(b->siteCountForTest() == 1u); // still installed: the drain has not run
	block(host, l, r, 0.0f);            // the drain removes the device and lets the site go
	b->reapRetired();
	b->reapRetired();
	CHECK(b->siteCountForTest() == 0u);
	CHECK_FALSE(b->hasSite(id));
}

TEST_CASE("NR: an edit inside either of a site's lanes marks the host's patch dirty, and Poly inside a site says it is unavailable") {
	InnerChain host { rack::kRackAxes };
	const DeviceInstanceId id = hostWithDevice(host);
	rack::RackBridge* b = host.bridge();
	REQUIRE(b->openSite(id));
	CHECK(b->takeStateDirty()); // making the site is an edit of the patch
	SiteChains* site = siteOfHost(host, id);
	REQUIRE(site != nullptr);
	CHECK_FALSE(b->takeStateDirty());
	seat(site->pre(), kOffset.traits.tag, 1.0f);
	CHECK(b->takeStateDirty()); // an add inside PRE
	rack::RackBridge* post = site->post().bridge();
	const DeviceInstanceId inner = post->addDevice(kOffset.traits.tag);
	REQUIRE(inner != kNoInstance);
	(void)b->takeStateDirty();
	post->setField(inner, 0, 0.5f);
	CHECK(b->takeStateDirty()); // a value inside POST
	// BUS ONLY, SAID WHERE A HAND WOULD ASK: a Poly switch inside a site is greyed with this line.
	CHECK(std::string(site->pre().bridge()->perVoiceUnavailable()) == rack::kPolyInsideSite);
	CHECK(std::string(post->perVoiceUnavailable()) == rack::kPolyInsideSite);
}

TEST_CASE("NR: a site travels with its device — two devices, each with its own site, reordered: each site's contents by device, by value") {
	InnerChain host { rack::kRackAxes };
	host.prepare(kRate, 4096);
	host.adopt(kProbes);
	rack::RackBridge* b = host.bridge();
	const DeviceInstanceId a = b->addDevice(kFurnished.traits.tag);
	const DeviceInstanceId c = b->addDevice(kFurnished.traits.tag);
	REQUIRE(a != kNoInstance);
	REQUIRE(c != kNoInstance);
	b->setField(a, 0, 2.0f);
	b->setField(c, 0, 3.0f);
	REQUIRE(b->openSite(a));
	REQUIRE(b->openSite(c));
	SiteChains* siteA = siteOfHost(host, a);
	SiteChains* siteC = siteOfHost(host, c);
	REQUIRE(siteA != nullptr);
	REQUIRE(siteC != nullptr);
	seat(siteA->pre(), kOffset.traits.tag, 1.0f);
	seat(siteC->pre(), kOffset.traits.tag, 10.0f);
	std::vector<float> l(64);
	std::vector<float> r(64);
	block(host, l, r, 0.25f);
	block(host, l, r, 0.25f);
	CHECK(l[0] == ((0.25f + 1.0f) * 2.0f + 10.0f) * 3.0f); // A (its PRE +1) then C (its PRE +10)
	REQUIRE(b->moveDeviceTo(c, 0));
	block(host, l, r, 0.25f);
	block(host, l, r, 0.25f);
	CHECK(host.engine().siteOf(a) == siteA);
	CHECK(host.engine().siteOf(c) == siteC);
	CHECK(l[0] == ((0.25f + 10.0f) * 3.0f + 1.0f) * 2.0f); // C then A, each WITH ITS OWN PRE
}

//------------------------------------------------------------------------------------------------//
// A PRE MODULATOR REACHES DOWNSTREAM (backlog NZ)                                                    //
//------------------------------------------------------------------------------------------------//

namespace {

/// A device in one of a site's lanes, its id back.
DeviceInstanceId seatId(InnerChain& lane, const char* tag, float control0) {
	rack::RackBridge* b = lane.bridge();
	REQUIRE(b != nullptr);
	const DeviceInstanceId id = b->addDevice(tag);
	REQUIRE(id != kNoInstance);
	b->setField(id, 0, control0);
	return id;
}

/// The control as the engine resolved it for the block just run — the VALUE, not a render.
float resolvedOf(const RackEngine& e, DeviceInstanceId id, size_t control) {
	const size_t slot = e.slotOf(id);
	REQUIRE(slot != RackEngine::kNoSlot);
	return e.resolvedFieldForTest(slot, control);
}

/// A HOST HOLDING THE FURNISHED DEVICE (gain 2) WITH A SITE: a held 0.5 in PRE and an Offset at 0 in POST.
struct Downstream {
	InnerChain host { rack::kRackAxes };
	DeviceInstanceId id = kNoInstance;
	SiteChains* site = nullptr;
	DeviceInstanceId held = kNoInstance;
	DeviceInstanceId offset = kNoInstance;
	std::vector<float> l = std::vector<float>(64);
	std::vector<float> r = std::vector<float>(64);

	Downstream() {
		id = hostWithDevice(host);
		REQUIRE(host.bridge()->openSite(id));
		site = siteOfHost(host, id);
		REQUIRE(site != nullptr);
		run();
		held = seatId(site->pre(), kHeld.traits.tag, 0.5f);
		offset = seatId(site->post(), kOffset.traits.tag, 0.0f);
		run();
	}
	void run(int blocks = 3) {
		for (int i = 0; i < blocks; ++i) {
			block(host, l, r, 0.25f);
		}
	}
};

} // namespace

TEST_CASE("NZ: a PRE modulator moves its device's control and a POST device's control, by value") {
	Downstream h;
	rack::RackBridge* b = h.host.bridge();
	const backplane::Source out = b->siteExport(h.id, backplane::Source::output(h.held, 0));
	REQUIRE(out.isExport());
	CHECK(out.device == h.id);
	// THE SAME INNER SOURCE IS ONE ROW, however often it is asked for.
	CHECK(b->siteExport(h.id, backplane::Source::output(h.held, 0)) == out);

	// INTO THE DEVICE ITSELF, through the host's own route verb: gain 2 + 0.25 · 0.5 · 8 (the gain's span) = 3.
	REQUIRE(b->mayRoute(out, backplane::Destination::control(h.id, 0)));
	b->setRouteAmount(out, backplane::Destination::control(h.id, 0), 0.25f);
	CHECK(b->routeAmount(out, backplane::Destination::control(h.id, 0)) == 0.25f);
	// INTO POST, through POST's own record, spelled as the host outside names it: 0 + 0.01 · 0.5 · 200 = 1.
	rack::RackBridge* post = h.site->post().bridge();
	REQUIRE(post->mayRoute(out.asOuter(), backplane::Destination::control(h.offset, 0)));
	post->setRouteAmount(out.asOuter(), backplane::Destination::control(h.offset, 0), 0.01f);
	h.run();
	CHECK(resolvedOf(h.host.engine(), h.id, 0) == doctest::Approx(3.0f).epsilon(1e-6));
	CHECK(resolvedOf(h.site->post().engine(), h.offset, 0) == doctest::Approx(1.0f).epsilon(1e-6));
	// AND THE SOUND AGREES — the second assertion, proving the first is not about a value nothing reads: 0.25 · 3 + 1.
	CHECK(h.l[0] == doctest::Approx(1.75f).epsilon(1e-6));

	// THE HELD VALUE MOVES, AND BOTH FOLLOW IT: −0.5 gives 2 − 1 and 0 − 1.
	h.site->pre().bridge()->setField(h.held, 0, -0.5f);
	h.run();
	CHECK(resolvedOf(h.host.engine(), h.id, 0) == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(resolvedOf(h.site->post().engine(), h.offset, 0) == doctest::Approx(-1.0f).epsilon(1e-6));
}

TEST_CASE("NZ: a PRE modulator is refused for everything upstream of it — a device before its holder, PRE itself — "
          "and a POST modulator exports nothing") {
	// A DEVICE BEFORE THE HOLDER: Offset first, then the furnished device with the site.
	InnerChain host { rack::kRackAxes };
	host.prepare(kRate, 4096);
	host.adopt(kProbes);
	rack::RackBridge* b = host.bridge();
	const DeviceInstanceId before = b->addDevice(kOffset.traits.tag);
	const DeviceInstanceId id = b->addDevice(kFurnished.traits.tag);
	const DeviceInstanceId after = b->addDevice(kOffset.traits.tag);
	REQUIRE(before != kNoInstance);
	REQUIRE(id != kNoInstance);
	REQUIRE(after != kNoInstance);
	REQUIRE(b->openSite(id));
	std::vector<float> l(64);
	std::vector<float> r(64);
	block(host, l, r, 0.0f);
	SiteChains* site = siteOfHost(host, id);
	REQUIRE(site != nullptr);
	const DeviceInstanceId held = seatId(site->pre(), kHeld.traits.tag, 0.5f);
	const DeviceInstanceId pre = seatId(site->pre(), kOffset.traits.tag, 0.0f);
	const DeviceInstanceId late = seatId(site->post(), kHeld.traits.tag, 0.5f);
	block(host, l, r, 0.0f);
	const backplane::Source out = b->siteExport(id, backplane::Source::output(held, 0));
	REQUIRE(out.isExport());

	// THE CONTROL: the holder itself is admitted.
	CHECK(b->mayRoute(out, backplane::Destination::control(id, 0)));
	// UPSTREAM OF THE HOLDER: refused by the gate, and a write is not made.
	CHECK_FALSE(b->mayRoute(out, backplane::Destination::control(before, 0)));
	b->setRouteAmount(out, backplane::Destination::control(before, 0), 0.5f);
	CHECK(b->routeAmount(out, backplane::Destination::control(before, 0)) == 0.0f);
	// A DEVICE AFTER THE HOLDER IS DOWNSTREAM, OUT THE BACK (rung 4): admitted — the control the refusals are measured
	// against.
	CHECK(b->mayRoute(out, backplane::Destination::control(after, 0)));
	// PRE ITSELF, through its own export: refused inside, where PRE's record asks.
	CHECK_FALSE(site->pre().bridge()->mayRoute(out.asOuter(), backplane::Destination::control(pre, 0)));
	// A POST MODULATOR REACHES ONLY WHAT FOLLOWS IT (decision O3): no row, so no source to route from.
	CHECK(b->siteExport(id, backplane::Source::output(late, 0)) == backplane::Source::nobody());
}

TEST_CASE("NZ: a PRE modulator's routes to its device and to POST save with the patch and arrive in a fresh host, "
          "by value") {
	std::vector<uint8_t> saved;
	DeviceInstanceId id = kNoInstance;
	DeviceInstanceId offset = kNoInstance;
	{
		Downstream h;
		id = h.id;
		offset = h.offset;
		rack::RackBridge* b = h.host.bridge();
		const backplane::Source out = b->siteExport(h.id, backplane::Source::output(h.held, 0));
		REQUIRE(out.isExport());
		b->setRouteAmount(out, backplane::Destination::control(h.id, 0), 0.25f);
		h.site->post().bridge()->setRouteAmount(out.asOuter(), backplane::Destination::control(h.offset, 0), 0.01f);
		h.run();
		const device::ByteSink sink { &saved, [](void* ctx, const uint8_t* data, size_t n) {
			                             auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                             v->insert(v->end(), data, data + n);
		                             } };
		REQUIRE(h.host.saveState(sink));
	}
	InnerChain again { rack::kRackAxes };
	again.prepare(kRate, 4096);
	again.adopt(kProbes);
	REQUIRE(again.loadState(saved.data(), saved.size()));
	std::vector<float> l(64);
	std::vector<float> r(64);
	for (int i = 0; i < 4; ++i) {
		block(again, l, r, 0.25f);
	}
	SiteChains* site = siteOfHost(again, id);
	REQUIRE(site != nullptr);
	CHECK(resolvedOf(again.engine(), id, 0) == doctest::Approx(3.0f).epsilon(1e-6));
	CHECK(resolvedOf(site->post().engine(), offset, 0) == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("NZ: the site's macros are PRE's, and one reaches the device and POST through the same export road, by value") {
	Downstream h;
	rack::RackBridge* b = h.host.bridge();
	rack::RackBridge* pre = h.site->pre().bridge();
	pre->setMacro(0, 0.5f);
	const backplane::Source out = b->siteExport(h.id, backplane::Source::core(0));
	REQUIRE(out.isExport());
	CHECK(h.site->exportedSource(out.exportRow()) == backplane::Source::core(0));
	// A MACRO PAST PRE'S SET IS NOT A SOURCE.
	CHECK(b->siteExport(h.id, backplane::Source::core(pre->axes().coreSources)) == backplane::Source::nobody());
	b->setRouteAmount(out, backplane::Destination::control(h.id, 0), 0.25f);
	h.site->post().bridge()->setRouteAmount(out.asOuter(), backplane::Destination::control(h.offset, 0), 0.01f);
	h.run();
	// gain 2 + 0.25 · 0.5 · 8; offset 0 + 0.01 · 0.5 · 200.
	CHECK(resolvedOf(h.host.engine(), h.id, 0) == doctest::Approx(3.0f).epsilon(1e-6));
	CHECK(resolvedOf(h.site->post().engine(), h.offset, 0) == doctest::Approx(1.0f).epsilon(1e-6));
}

TEST_CASE("NZ rung 5: a PRE source reaches inside the site of a LATER device — its PRE and its POST — by value, and not "
          "inside the site of an earlier one") {
	// EARLY (with a site), HOLDER (with a site and a held 0.5 in PRE), LATE (with a site): an Offset in each lane.
	InnerChain host { rack::kRackAxes };
	host.prepare(kRate, 4096);
	host.adopt(kProbes);
	rack::RackBridge* b = host.bridge();
	const DeviceInstanceId early = b->addDevice(kFurnished.traits.tag);
	const DeviceInstanceId holder = b->addDevice(kFurnished.traits.tag);
	const DeviceInstanceId late = b->addDevice(kFurnished.traits.tag);
	REQUIRE(b->openSite(early));
	REQUIRE(b->openSite(holder));
	REQUIRE(b->openSite(late));
	std::vector<float> l(64);
	std::vector<float> r(64);
	block(host, l, r, 0.0f);
	SiteChains* earlySite = siteOfHost(host, early);
	SiteChains* holderSite = siteOfHost(host, holder);
	SiteChains* lateSite = siteOfHost(host, late);
	REQUIRE(earlySite != nullptr);
	REQUIRE(holderSite != nullptr);
	REQUIRE(lateSite != nullptr);
	const DeviceInstanceId held = seatId(holderSite->pre(), kHeld.traits.tag, 0.5f);
	const DeviceInstanceId earlyPre = seatId(earlySite->pre(), kOffset.traits.tag, 0.0f);
	const DeviceInstanceId latePre = seatId(lateSite->pre(), kOffset.traits.tag, 0.0f);
	const DeviceInstanceId latePost = seatId(lateSite->post(), kOffset.traits.tag, 0.0f);
	block(host, l, r, 0.0f);
	const backplane::Source out = b->siteExport(holder, backplane::Source::output(held, 0));
	REQUIRE(out.isExport());

	// EARLIER: refused, and nothing is written.
	const auto inEarly = backplane::Destination::control(earlyPre, 0);
	CHECK_FALSE(earlySite->pre().bridge()->mayRoute(out.asOuter(), inEarly));
	earlySite->pre().bridge()->setRouteAmount(out.asOuter(), inEarly, 0.01f);
	CHECK(earlySite->pre().bridge()->routeAmount(out.asOuter(), inEarly) == 0.0f);
	// LATER, BOTH LANES: 0 + 0.01 · 0.5 · 200 = 1, and −0.02 · 0.5 · 200 = −2.
	const auto inLatePre = backplane::Destination::control(latePre, 0);
	const auto inLatePost = backplane::Destination::control(latePost, 0);
	REQUIRE(lateSite->pre().bridge()->mayRoute(out.asOuter(), inLatePre));
	REQUIRE(lateSite->post().bridge()->mayRoute(out.asOuter(), inLatePost));
	lateSite->pre().bridge()->setRouteAmount(out.asOuter(), inLatePre, 0.01f);
	lateSite->post().bridge()->setRouteAmount(out.asOuter(), inLatePost, -0.02f);
	for (int i = 0; i < 3; ++i) {
		block(host, l, r, 0.0f);
	}
	CHECK(resolvedOf(lateSite->pre().engine(), latePre, 0) == doctest::Approx(1.0f).epsilon(1e-6));
	CHECK(resolvedOf(lateSite->post().engine(), latePost, 0) == doctest::Approx(-2.0f).epsilon(1e-6));
	// THE LATE SITE'S OWN EXPORT IS STILL REFUSED IN ITS OWN PRE — the line rung 5 drew is "another holder's", not "any".
	const DeviceInstanceId lateHeld = seatId(lateSite->pre(), kHeld.traits.tag, 0.5f);
	block(host, l, r, 0.0f);
	const backplane::Source own = b->siteExport(late, backplane::Source::output(lateHeld, 0));
	REQUIRE(own.isExport());
	CHECK_FALSE(lateSite->pre().bridge()->mayRoute(own.asOuter(), inLatePre));
	CHECK(lateSite->post().bridge()->mayRoute(own.asOuter(), inLatePost));
}
