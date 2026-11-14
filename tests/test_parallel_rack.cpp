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

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

// THE RACK (backlog NO, stage 1): parallel chains summed through level, mute and solo, then Mix and Gain.
// Every claim is a VALUE: the probes below multiply by a known gain or delay by a known count, so the sum the
// rack should make is restated arithmetically beside what it made (CLAUDE.md: a rendered buffer is not a value).

#include "composite/composite_types.h"
#include "fx/owltt/crossover.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/filter/filter_types.h"
#include "fx/phaser/phaser_types.h"
#include "plugin/device_registry.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/synth.h"
#include "rack/chain_record.h"
#include "rack/parallel_rack.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"

#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <new>
#include <vector>

using namespace feathers;
namespace pr = feathers::rack::parallel;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;
/// The 5 ms ramp at `kRate`, restated: a case that reads a gain waits this long first.
constexpr int kRamp = 240;

//------------------------------------------------------------------------------------------------//
// PROBES                                                                                           //
//------------------------------------------------------------------------------------------------//

/// MULTIPLIES BY ITS GAIN — a chain's contribution is then a number a case can predict.
struct Gain {
	float gain = 1.0f;
};
constexpr std::array<device::ParamDesc, 1> kGainParams { {
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {} },
} };
constexpr device::AudioOps kGainOps {
	.stateBytes = sizeof(Gain),
	.stateAlign = alignof(Gain),
	.construct = [](void* s) { new (s) Gain {}; },
	.destruct = [](void* s) { static_cast<Gain*>(s)->~Gain(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setSlot = nullptr,
	.setFields = [](void* s, const float* f, size_t n) { static_cast<Gain*>(s)->gain = n > 0 ? f[0] : 1.0f; },
	.processStereo =
	    [](void* s, float* l, float* r, int frames) {
		    const float g = static_cast<Gain*>(s)->gain;
		    for (int i = 0; i < frames; ++i) {
			    l[i] *= g;
			    r[i] *= g;
		    }
	    },
};
const device::Descriptor kGainDesc {
	.traits = { .tag = "prbG", .name = "Probe G", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = kGainParams,
	.outputs = {},
	.audio = &kGainOps,
};

/// A PURE DELAY THAT SAYS HOW LATE IT IS — a latent device whose latency is exactly its control.
struct Late {
	uint32_t samples = 0;
	std::array<float, 4096> l {};
	std::array<float, 4096> r {};
	size_t at = 0;
};
constexpr std::array<device::ParamDesc, 1> kLateParams { {
	{ "samples", "Samples", 0.0, 4000.0, 0.0, true, device::Unit::Number, {} },
} };
constexpr device::AudioOps kLateOps {
	.stateBytes = sizeof(Late),
	.stateAlign = alignof(Late),
	.construct = [](void* s) { new (s) Late {}; },
	.destruct = [](void* s) { static_cast<Late*>(s)->~Late(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setSlot = nullptr,
	.setFields =
	    [](void* s, const float* f, size_t n) { static_cast<Late*>(s)->samples = n > 0 ? static_cast<uint32_t>(f[0]) : 0u; },
	.processStereo =
	    [](void* s, float* l, float* r, int frames) {
		    Late& d = *static_cast<Late*>(s);
		    for (int i = 0; i < frames; ++i) {
			    d.l[d.at] = l[i];
			    d.r[d.at] = r[i];
			    const size_t read = (d.at + d.l.size() - d.samples) % d.l.size();
			    l[i] = d.l[read];
			    r[i] = d.r[read];
			    d.at = (d.at + 1) % d.l.size();
		    }
	    },
	.latencySamples = [](const void*, const float* f, size_t n, double) {
		return n > 0 ? static_cast<uint32_t>(f[0]) : 0u;
	},
};
const device::Descriptor kLateDesc {
	.traits = { .tag = "prbL", .name = "Probe L", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = kLateParams,
	.outputs = {},
	.audio = &kLateOps,
};

/// SUMS TO MONO — both channels become ½(L+R). Handed the Side in form A, (S, −S), it outputs exactly zero (§5).
struct Mono {};
constexpr device::AudioOps kMonoOps {
	.stateBytes = sizeof(Mono),
	.stateAlign = alignof(Mono),
	.construct = [](void* s) { new (s) Mono {}; },
	.destruct = [](void* s) { static_cast<Mono*>(s)->~Mono(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setSlot = nullptr,
	.setFields = [](void*, const float*, size_t) {},
	.processStereo =
	    [](void*, float* l, float* r, int frames) {
		    for (int i = 0; i < frames; ++i) {
			    const float m = 0.5f * (l[i] + r[i]);
			    l[i] = m;
			    r[i] = m;
		    }
	    },
};
const device::Descriptor kMonoDesc {
	.traits = { .tag = "prbM", .name = "Probe M", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = {},
	.outputs = {},
	.audio = &kMonoOps,
};

/// AN OSCILLATOR THAT IGNORES ITS INPUT: a sine of `hz` at amplitude 0.5 — what a distortion's harmonics stand in for.
struct Tone {
	double hz = 8000.0;
	double phase = 0.0;
};
constexpr std::array<device::ParamDesc, 1> kToneParams { {
	{ "hz", "Hz", 20.0, 20000.0, 8000.0, false, device::Unit::Number, {} },
} };
constexpr device::AudioOps kToneOps {
	.stateBytes = sizeof(Tone),
	.stateAlign = alignof(Tone),
	.construct = [](void* s) { new (s) Tone {}; },
	.destruct = [](void* s) { static_cast<Tone*>(s)->~Tone(); },
	.prepare = [](void*, double, int) {},
	.reset = [](void* s) { static_cast<Tone*>(s)->phase = 0.0; },
	.setSlot = nullptr,
	.setFields = [](void* s, const float* f, size_t n) { static_cast<Tone*>(s)->hz = n > 0 ? f[0] : 8000.0; },
	.processStereo =
	    [](void* s, float* l, float* r, int frames) {
		    Tone& t = *static_cast<Tone*>(s);
		    for (int i = 0; i < frames; ++i) {
			    const auto v = static_cast<float>(0.5 * std::sin(t.phase));
			    l[i] = v;
			    r[i] = v;
			    t.phase += 2.0 * 3.14159265358979323846 * t.hz / 48000.0;
		    }
	    },
};
const device::Descriptor kToneDesc {
	.traits = { .tag = "prbT", .name = "Probe T", .hasCard = false, .hasExpanded = false, .perVoiceControl = -1 },
	.params = kToneParams,
	.outputs = {},
	.audio = &kToneOps,
};

/// THE PROBES, AND THIS BUILD'S DEVICES BEHIND THEM — so a Chain and a Rack can be seated beside a probe.
device::Catalogue probes() {
	return device::Catalogue { nullptr, [](void*, const char* tag) -> const device::Descriptor* {
		                          if (device::sameTag(tag, kGainDesc.traits.tag)) {
			                          return &kGainDesc;
		                          }
		                          if (device::sameTag(tag, kLateDesc.traits.tag)) {
			                          return &kLateDesc;
		                          }
		                          if (device::sameTag(tag, kMonoDesc.traits.tag)) {
			                          return &kMonoDesc;
		                          }
		                          if (device::sameTag(tag, kToneDesc.traits.tag)) {
			                          return &kToneDesc;
		                          }
		                          return deviceForTag(tag);
	                          } };
}

//------------------------------------------------------------------------------------------------//
// A RACK, STANDING ALONE                                                                           //
//------------------------------------------------------------------------------------------------//

struct RackRig {
	void* mem = nullptr;
	std::array<float, static_cast<size_t>(pr::Param::kCount)> fields {};

	explicit RackRig(device::Catalogue catalogue = probes()) {
		mem = ::operator new(pr::kAudioOps.stateBytes, std::align_val_t { pr::kAudioOps.stateAlign });
		pr::kAudioOps.construct(mem);
		pr::kContainerOps.adopt(mem, catalogue);
		pr::kAudioOps.prepare(mem, kRate, 4096);
		for (size_t i = 0; i < fields.size(); ++i) {
			fields[i] = static_cast<float>(pr::kParamDescs[i].def);
		}
	}
	~RackRig() {
		pr::kAudioOps.destruct(mem);
		::operator delete(mem, std::align_val_t { pr::kAudioOps.stateAlign });
	}
	RackRig(const RackRig&) = delete;
	RackRig& operator=(const RackRig&) = delete;

	pr::Rack& rack() { return *pr::rackOf(mem); }
	void set(pr::Param p, float v) { fields[static_cast<size_t>(p)] = v; }

	/// ONE BLOCK, the host's way: fields, then the bus. Returns the left channel; `in` feeds both.
	std::vector<float> run(const std::vector<float>& in, std::vector<float>* rightOut = nullptr) {
		std::vector<float> l = in;
		std::vector<float> r = in;
		pr::kAudioOps.setFields(mem, fields.data(), fields.size());
		pr::kAudioOps.processStereo(mem, l.data(), r.data(), static_cast<int>(l.size()));
		if (rightOut != nullptr) {
			*rightOut = r;
		}
		return l;
	}
	/// Run `blocks` blocks of a constant, so every ramp has settled and every drain happened.
	void settle(int blocks = 4) {
		for (int b = 0; b < blocks; ++b) {
			(void)run(std::vector<float>(kBlock, 0.0f));
		}
	}

	DeviceInstanceId seat(size_t chain, const device::Descriptor& d, float field0) {
		rack::RackBridge* bridge = rack().chainAt(chain)->bridge();
		REQUIRE(bridge != nullptr);
		const DeviceInstanceId id = bridge->addDevice(d.traits.tag);
		REQUIRE(id != kNoInstance);
		bridge->setField(id, 0, field0);
		return id;
	}

	std::vector<uint8_t> save() const {
		std::vector<uint8_t> body;
		const device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
			                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
			                             out->insert(out->end(), data, data + bytes);
		                             } };
		REQUIRE(pr::kContainerOps.saveState(mem, sink));
		return body;
	}
};

/// A RAMP 0.01, 0.02, … — every sample distinct and none zero, so a misplaced or doubled sample shows.
std::vector<float> ramp(int n = kBlock) {
	std::vector<float> x(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		x[static_cast<size_t>(i)] = 0.01f * static_cast<float>(i + 1);
	}
	return x;
}

} // namespace

TEST_CASE("NO: a fresh rack holds one empty chain, and it is the identity — an empty chain is the dry path") {
	RackRig rig;
	CHECK(rig.rack().chainCount() == 1);
	rig.settle();
	const std::vector<float> in = ramp();
	const std::vector<float> out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == in[i]);
	}
}

TEST_CASE("NO: every chain hears the same input and the rack sums them — x2 and x3 and an empty chain make 6x") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	REQUIRE(rig.rack().addChain() == 2);
	(void)rig.seat(0, kGainDesc, 2.0f);
	(void)rig.seat(1, kGainDesc, 3.0f);
	// chain 2 holds nothing: the dry path, once more.
	rig.settle();
	CHECK(rig.rack().liveChainsForTest() == 3);
	const std::vector<float> in = ramp();
	std::vector<float> right;
	const std::vector<float> out = rig.run(in, &right);
	for (size_t i = 0; i < in.size(); ++i) {
		const float want = 2.0f * in[i] + 3.0f * in[i] + in[i];
		REQUIRE(out[i] == doctest::Approx(want).epsilon(1e-6));
		REQUIRE(right[i] == doctest::Approx(want).epsilon(1e-6));
	}
}

TEST_CASE("NO: a chain's level, mute and solo, by value — the level is the Chain's fader curve, a mute is silence, a solo silences the rest") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	(void)rig.seat(0, kGainDesc, 2.0f);
	(void)rig.seat(1, kGainDesc, 3.0f);
	rig.settle();
	const std::vector<float> in = ramp();
	const auto expectSum = [&](float g0, float g1) {
		rig.settle(); // past the 5 ms ramp
		CHECK(rig.rack().chainGainForTest(0) == doctest::Approx(g0));
		CHECK(rig.rack().chainGainForTest(1) == doctest::Approx(g1));
		const std::vector<float> out = rig.run(in);
		for (size_t i = 0; i < in.size(); ++i) {
			REQUIRE(out[i] == doctest::Approx(g0 * 2.0f * in[i] + g1 * 3.0f * in[i]).epsilon(1e-6));
		}
	};
	expectSum(1.0f, 1.0f);
	rig.rack().setLevel(0, 0.25f); // (2 * 0.25)^2
	expectSum(0.25f, 1.0f);
	rig.rack().setLevel(1, 0.75f); // (2 * 0.75)^2
	expectSum(0.25f, 2.25f);
	rig.rack().setMuted(1, true);
	expectSum(0.25f, 0.0f);
	rig.rack().setMuted(1, false);
	rig.rack().setSoloed(1, true);
	expectSum(0.0f, 2.25f);
	rig.rack().setSoloed(0, true); // two soloed: both heard
	expectSum(0.25f, 2.25f);
	rig.rack().setMuted(0, true); // a mute outranks its own solo
	expectSum(0.0f, 2.25f);
}

TEST_CASE("NO: a level change is a 5 ms fade, not a step") {
	RackRig rig;
	(void)rig.seat(0, kGainDesc, 1.0f);
	rig.settle();
	rig.rack().setMuted(0, true);
	const std::vector<float> ones(kBlock, 1.0f);
	const std::vector<float> out = rig.run(ones);
	CHECK(out[0] > 0.99f);          // the first sample has barely moved
	CHECK(out[kRamp / 2] > 0.4f);   // half way down
	CHECK(out[kRamp / 2] < 0.6f);
	CHECK(out[kRamp + 1] == 0.0f);  // and down by the end of the ramp
}

TEST_CASE("NO: Mix 0 is the dry bit for bit, Mix 1 is the chains' sum alone") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	(void)rig.seat(0, kGainDesc, 2.0f);
	(void)rig.seat(1, kGainDesc, 3.0f);
	const std::vector<float> in = ramp();
	rig.set(pr::Param::Mix, 0.0f);
	rig.settle();
	CHECK(rig.rack().wetForTest() == 0.0f);
	std::vector<float> out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == in[i]);
	}
	rig.set(pr::Param::Mix, 1.0f);
	rig.settle();
	CHECK(rig.rack().dryForTest() == 0.0f);
	out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == doctest::Approx(5.0f * in[i]).epsilon(1e-6));
	}
	rig.set(pr::Param::Mix, 0.25f);
	rig.settle();
	out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == doctest::Approx(0.25f * 5.0f * in[i] + 0.75f * in[i]).epsilon(1e-6));
	}
}

TEST_CASE("NO: the rack's output fader scales wet and dry together, unity at twelve o'clock") {
	RackRig rig;
	(void)rig.seat(0, kGainDesc, 2.0f);
	const std::vector<float> in = ramp();
	rig.set(pr::Param::Mix, 0.5f);
	rig.set(pr::Param::Gain, 0.75f); // (2 * 0.75)^2 = 2.25
	rig.settle();
	CHECK(rig.rack().wetForTest() == doctest::Approx(0.5f * 2.25f));
	CHECK(rig.rack().dryForTest() == doctest::Approx(0.5f * 2.25f));
	const std::vector<float> out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == doctest::Approx(2.25f * (0.5f * 2.0f * in[i] + 0.5f * in[i])).epsilon(1e-6));
	}
}

TEST_CASE("NO: bypassed and settled the rack passes its input, and its chains do not sound") {
	RackRig rig;
	(void)rig.seat(0, kGainDesc, 3.0f);
	rig.set(pr::Param::Enabled, 0.0f);
	rig.settle();
	const std::vector<float> in = ramp();
	const std::vector<float> out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == in[i]);
	}
}

TEST_CASE("NO: latency — every chain waits for the slowest to the sample, the dry too, and the rack reports the slowest") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	REQUIRE(rig.rack().addChain() == 2);
	(void)rig.seat(0, kLateDesc, 100.0f);
	(void)rig.seat(1, kLateDesc, 37.0f);
	// chain 2 empty: no latency of its own.
	rig.set(pr::Param::Mix, 0.5f);
	rig.settle();
	CHECK(pr::kAudioOps.latencySamples(rig.mem, rig.fields.data(), rig.fields.size(), kRate) == 100u);
	std::vector<float> impulse(kBlock, 0.0f);
	impulse[0] = 1.0f;
	const std::vector<float> out = rig.run(impulse);
	// ALL THREE CHAINS AND THE DRY LAND ON SAMPLE 100: wet 0.5 x 3 chains + dry 0.5.
	for (int i = 0; i < kBlock; ++i) {
		CAPTURE(i);
		REQUIRE(out[static_cast<size_t>(i)] == doctest::Approx(i == 100 ? 0.5f * 3.0f + 0.5f : 0.0f));
	}
}

TEST_CASE("NO: latency with the FLANGER on one branch — an empty sibling and the dry arrive exactly when the flanger's output does") {
	RackRig rig(hostCatalogue());
	REQUIRE(rig.rack().addChain() == 1);
	rack::RackBridge* a = rig.rack().chainAt(0)->bridge();
	const DeviceInstanceId flanger = a->addDevice(fx::phaser::kTraits.tag);
	REQUIRE(flanger != kNoInstance);
	a->setField(flanger, static_cast<size_t>(fx::phaser::Param::Mode), static_cast<float>(fx::phaser::Mode::Flanger));
	rig.settle();
	const uint32_t want = fx::phaser::latencySamplesFor(fx::phaser::Mode::Flanger, kRate);
	REQUIRE(want > 0u);
	CHECK(pr::kAudioOps.latencySamples(rig.mem, rig.fields.data(), rig.fields.size(), kRate) == want);
	// THE EMPTY SIBLING, SOLOED: the impulse comes out of it at exactly the flanger's latency, whole.
	rig.rack().setSoloed(1, true);
	rig.settle(8);
	std::vector<float> in(4096, 0.0f);
	in[0] = 1.0f;
	std::vector<float> out = rig.run(in);
	for (size_t i = 0; i < out.size(); ++i) {
		CAPTURE(i);
		REQUIRE(out[i] == (i == want ? 1.0f : 0.0f));
	}
	// AND THE DRY, AT MIX 0: the same sample.
	rig.rack().setSoloed(1, false);
	rig.set(pr::Param::Mix, 0.0f);
	rig.settle(8);
	out = rig.run(in);
	for (size_t i = 0; i < out.size(); ++i) {
		CAPTURE(i);
		REQUIRE(out[i] == (i == want ? 1.0f : 0.0f));
	}
}

TEST_CASE("NO: a chain added while running is built off the audio thread and joins at the next drain; a removed one is freed after the drain") {
	RackRig rig;
	rig.settle();
	REQUIRE(rig.rack().liveChainsForTest() == 1);
	REQUIRE(rig.rack().addChain() == 1);
	CHECK(rig.rack().chainCount() == 2);
	CHECK(rig.rack().liveChainsForTest() == 1); // the audio thread has not drained yet
	(void)rig.run(ramp());
	CHECK(rig.rack().liveChainsForTest() == 2);
	REQUIRE(rig.rack().removeChain(0));
	CHECK(rig.rack().liveChainsForTest() == 2); // still running it until the drain
	CHECK(rig.rack().pendingFreesForTest() > 0);
	(void)rig.run(ramp());
	CHECK(rig.rack().liveChainsForTest() == 1);
	(void)pr::kContainerOps.service(rig.mem); // the host's beat frees what the audio thread moved past
	CHECK(rig.rack().pendingFreesForTest() == 1); // the live lineup only
}

TEST_CASE("NO: save and load — the chains, their devices, and each chain's level, mute and solo come back by value") {
	std::vector<uint8_t> body;
	std::vector<float> before;
	const std::vector<float> in = ramp();
	{
		RackRig rig;
		REQUIRE(rig.rack().addChain() == 1);
		REQUIRE(rig.rack().addChain() == 2);
		(void)rig.seat(0, kGainDesc, 2.0f);
		(void)rig.seat(1, kGainDesc, 3.0f);
		(void)rig.seat(2, kLateDesc, 5.0f);
		rig.rack().setLevel(0, 0.25f);
		rig.rack().setMuted(1, true);
		rig.rack().setSoloed(2, true);
		rig.rack().setSoloed(0, true);
		rig.settle();
		before = rig.run(in);
		body = rig.save();
	}
	RackRig fresh;
	REQUIRE(pr::kContainerOps.loadState(fresh.mem, body.data(), body.size()));
	REQUIRE(fresh.rack().chainCount() == 3);
	CHECK(fresh.rack().level(0) == 0.25f);
	CHECK(fresh.rack().level(1) == static_cast<float>(composite::kFaderUnity));
	CHECK_FALSE(fresh.rack().muted(0));
	CHECK(fresh.rack().muted(1));
	CHECK(fresh.rack().soloed(0));
	CHECK_FALSE(fresh.rack().soloed(1));
	CHECK(fresh.rack().soloed(2));
	for (size_t c = 0; c < 3; ++c) {
		const rack::RackBridge* b = fresh.rack().chainAt(c)->bridge();
		REQUIRE(b != nullptr);
		REQUIRE(b->chainLength() == 1);
	}
	fresh.settle();
	const std::vector<float> after = fresh.run(in);
	// AND THE SAME SOUND: 0.25·2x (soloed) + muted 3x + solo 5-late x, at the same latency.
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(after[i] == doctest::Approx(before[i]));
	}
	// A TRUNCATED BODY IS REFUSED, AND THE RACK IS LEFT AS IT WAS.
	CHECK_FALSE(pr::kContainerOps.loadState(fresh.mem, body.data(), body.size() - 1));
	CHECK(fresh.rack().chainCount() == 3);
	CHECK(fresh.rack().soloed(2));
}

TEST_CASE("hardening: a body declaring more chains than a rack may hold is refused at once and leaves the rack as it was") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	rig.settle();
	std::vector<uint8_t> body = rig.save();
	REQUIRE(body.size() > 10);
	// THE COUNT SITS AFTER THE 4-BYTE MAGIC AND THE 2-BYTE VERSION; written big, it is far past what the bytes could hold.
	const auto withCount = [&](uint32_t count) {
		std::vector<uint8_t> b = body;
		std::memcpy(b.data() + 6, &count, sizeof(count));
		return b;
	};
	const std::vector<uint8_t> huge = withCount(0x7FFFFFFFu);
	const auto start = std::chrono::steady_clock::now();
	CHECK_FALSE(pr::kContainerOps.loadState(rig.mem, huge.data(), huge.size()));
	const auto took = std::chrono::steady_clock::now() - start;
	CHECK(took < std::chrono::milliseconds(200));
	CHECK(rig.rack().chainCount() == 2);
	// ONE PAST THE CAP, WITH EVERY CHAIN'S BYTES PRESENT: a body that is whole in every other way, so only the count
	// can refuse it (without the cap it builds a thousand and more inner engines).
	std::vector<uint8_t> whole(body.begin(), body.begin() + 6);
	const uint32_t over = pr::kMaxChains + 1;
	whole.insert(whole.end(), reinterpret_cast<const uint8_t*>(&over), reinterpret_cast<const uint8_t*>(&over) + sizeof(over));
	for (uint32_t c = 0; c < over; ++c) {
		const uint16_t id = static_cast<uint16_t>(c);
		whole.insert(whole.end(), reinterpret_cast<const uint8_t*>(&id), reinterpret_cast<const uint8_t*>(&id) + sizeof(id));
		whole.push_back(0);                      // no fields
		whole.insert(whole.end(), 4, uint8_t {}); // no body
	}
	whole.insert(whole.end(), 2, uint8_t {}); // no exports
	const auto startWhole = std::chrono::steady_clock::now();
	CHECK_FALSE(pr::kContainerOps.loadState(rig.mem, whole.data(), whole.size()));
	CHECK(std::chrono::steady_clock::now() - startWhole < std::chrono::milliseconds(200));
	CHECK(rig.rack().chainCount() == 2);
	// AND THE ORIGINAL BODY STILL LOADS.
	REQUIRE(pr::kContainerOps.loadState(rig.mem, body.data(), body.size()));
	CHECK(rig.rack().chainCount() == 2);
}

TEST_CASE("NO: nested — a Rack in a Chain in a Rack sums by value, and survives a save and load") {
	RackRig rig;
	// OUTER RACK, chain 0: a Chain device; inside it, a Rack with two chains of x2 and x3.
	rack::RackBridge* outer = rig.rack().chainAt(0)->bridge();
	const DeviceInstanceId chainDevice = outer->addDevice(composite::kTraits.tag);
	REQUIRE(chainDevice != kNoInstance);
	rig.settle(); // the outer chain provisions the Chain device
	rack::RackEngine& outerEngine = rig.rack().chainAt(0)->engine();
	void* chainStorage = outerEngine.slotAtLane(outerEngine.slotOf(chainDevice)).storage.get();
	REQUIRE(chainStorage != nullptr);
	rack::RackBridge* inChain = rack::bridgeOf(chainStorage);
	REQUIRE(inChain != nullptr);
	const DeviceInstanceId innerRack = inChain->addDevice(pr::kTraits.tag);
	REQUIRE(innerRack != kNoInstance);
	rig.settle();
	rack::RackEngine& chainEngine = *rack::engineOf(chainStorage);
	void* innerStorage = chainEngine.slotAtLane(chainEngine.slotOf(innerRack)).storage.get();
	REQUIRE(innerStorage != nullptr);
	pr::Rack& inner = *pr::rackOf(innerStorage);
	REQUIRE(inner.addChain() == 1);
	for (size_t c = 0; c < 2; ++c) {
		rack::RackBridge* b = inner.chainAt(c)->bridge();
		const DeviceInstanceId g = b->addDevice(kGainDesc.traits.tag);
		REQUIRE(g != kNoInstance);
		b->setField(g, 0, c == 0 ? 2.0f : 3.0f);
	}
	rig.settle();
	const std::vector<float> in = ramp();
	const std::vector<float> out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == doctest::Approx(5.0f * in[i]).epsilon(1e-6));
	}
	// THE WHOLE NEST THROUGH THE OUTER RACK'S BODY.
	const std::vector<uint8_t> body = rig.save();
	RackRig fresh;
	REQUIRE(pr::kContainerOps.loadState(fresh.mem, body.data(), body.size()));
	fresh.settle();
	const std::vector<float> again = fresh.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(again[i] == doctest::Approx(5.0f * in[i]).epsilon(1e-6));
	}
}

namespace {

/// FEATHERS PLAYING A NOTE through its lane: `build` seats what the lane holds. Returns the left channel.
template <typename Build> std::vector<float> renderLane(Build&& build) {
	ParamSet params;
	Synth synth { params };
	ParamBridge bridge;
	synth.activate(kRate, 4096);
	params.setBase(static_cast<clap_id>(ParamId::VoiceLevel), 1.0);
	synth.setCatalogue(hostCatalogue());
	synth.setOuterGateMaker(&rack::ChainRecord::gateOver, &bridge.chainRecord());
	const auto provision = [&] {
		publishEditorLanes(bridge, params, synth);
		(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	};
	std::vector<float> l(64, 0.0f);
	std::vector<float> r(64, 0.0f);
	const auto block = [&] { synth.render(l.data(), r.data(), 64); };
	build(bridge, provision, block);
	provision();
	for (int i = 0; i < 4; ++i) {
		block();
	}
	synth.noteOn(0, 60, 1, 0.8);
	std::vector<float> out;
	for (int i = 0; i < 24; ++i) {
		block();
		out.insert(out.end(), l.begin(), l.end());
	}
	return out;
}

/// A RACK IN THE LANE, `withFilter` putting a Poly filter in its first chain; `extraEmpty` adds an empty second chain.
std::vector<float> renderRack(bool extraEmpty) {
	return renderLane([&](ParamBridge& bridge, const auto& provision, const auto& block) {
		const size_t slot = bridge.addFxDevice(pr::kTraits.tag);
		REQUIRE(slot < kMaxFxSlots);
		bridge.refresh();
		const DeviceInstanceId id = bridge.fxDeviceAt(slot);
		provision();
		block();
		pr::Rack* rackDevice = pr::rackOf(bridge.fxBodies().surfaceOf(id));
		REQUIRE(rackDevice != nullptr);
		if (extraEmpty) {
			REQUIRE(rackDevice->addChain() == 1);
		}
		rack::RackBridge* chain = rackDevice->chainAt(0)->bridge();
		const DeviceInstanceId filter = chain->addDevice(fx::filter::kTraits.tag);
		REQUIRE(filter != kNoInstance);
		chain->setField(filter, static_cast<size_t>(fx::filter::kDescriptor.traits.perVoiceControl), 1.0f);
	});
}

std::vector<float> renderDirect(bool withFilter) {
	return renderLane([&](ParamBridge& bridge, const auto&, const auto&) {
		if (!withFilter) {
			return;
		}
		const size_t slot = bridge.addFxDevice(fx::filter::kTraits.tag);
		REQUIRE(slot < kMaxFxSlots);
		bridge.refresh();
		bridge.setDeviceControl(bridge.fxDeviceAt(slot),
		                        static_cast<size_t>(fx::filter::kDescriptor.traits.perVoiceControl), 0.0f); // Poly OFF: one filter, on the bus
	});
}

float worstOf(const std::vector<float>& a, const std::vector<float>& b, float* peak) {
	REQUIRE(a.size() == b.size());
	float worst = 0.0f;
	*peak = 0.0f;
	for (size_t i = 0; i < a.size(); ++i) {
		*peak = std::max(*peak, std::abs(a[i]));
		worst = std::max(worst, std::abs(a[i] - b[i]));
	}
	return worst;
}

} // namespace

TEST_CASE("NO, per voice (option A): the host's voices do not pass through a rack — a Poly filter in a rack's chain runs once on the bus, rendering what the same filter with Poly OFF renders in the lane") {
	const std::vector<float> direct = renderDirect(true);
	const std::vector<float> inRack = renderRack(false);
	float peak = 0.0f;
	const float worst = worstOf(direct, inRack, &peak);
	REQUIRE(peak > 0.05f);
	CHECK(worst < peak * 1e-3f);
}

TEST_CASE("NO, per voice (option A): beside an empty chain, the rack is that one bus filter PLUS the dry — by value against two lane renders") {
	const std::vector<float> filtered = renderDirect(true);
	const std::vector<float> dry = renderDirect(false);
	const std::vector<float> inRack = renderRack(true);
	std::vector<float> want(filtered.size());
	for (size_t i = 0; i < want.size(); ++i) {
		want[i] = filtered[i] + dry[i];
	}
	float peak = 0.0f;
	const float worst = worstOf(want, inRack, &peak);
	REQUIRE(peak > 0.05f);
	CHECK(worst < peak * 1e-3f);
	// AND IT IS NOT EITHER ALONE — so the first assertion is not vacuous.
	float p2 = 0.0f;
	CHECK(worstOf(filtered, inRack, &p2) > peak * 0.05f);
}

namespace {

struct DirtyCount {
	int marks = 0;
};

device::OuterGate countingGate(DirtyCount& count) {
	device::OuterGate gate;
	gate.dirtyHost = &count;
	gate.markDirty = [](void* host) { ++static_cast<DirtyCount*>(host)->marks; };
	return gate;
}

} // namespace

TEST_CASE("NO: every chain edit marks the holder's patch dirty — add, remove, put back, level, mute, solo — and a load or a write that changes nothing does not") {
	RackRig rig;
	DirtyCount dirty;
	pr::kContainerOps.setOuterGate(rig.mem, countingGate(dirty));
	pr::Rack& rack = rig.rack();
	const auto marked = [&](auto&& edit) {
		const int before = dirty.marks;
		edit();
		return dirty.marks - before;
	};
	CHECK(marked([&] { REQUIRE(rack.addChain() == 1); }) == 1);
	CHECK(marked([&] { rack.setLevel(1, 0.25f); }) == 1);
	CHECK(marked([&] { rack.setLevel(1, 0.25f); }) == 0); // the same value: nothing a save would write differently
	CHECK(marked([&] { rack.setMuted(1, true); }) == 1);
	CHECK(marked([&] { rack.setMuted(1, true); }) == 0);
	CHECK(marked([&] { rack.setSoloed(0, true); }) == 1);
	const pr::Rack::ChainSnapshot taken = rack.snapshotChain(1);
	CHECK(marked([&] { REQUIRE(rack.removeChain(1)); }) == 1);
	CHECK(marked([&] { REQUIRE(rack.insertChain(1, taken) == 1); }) == 1);
	// AN EDIT INSIDE A CHAIN MARKS IT TOO — the chain's own bridge holds the same gate.
	CHECK(marked([&] { (void)rig.seat(0, kGainDesc, 2.0f); }) >= 1);
	// A LOAD NEVER DOES: opening a patch must not report it modified.
	const std::vector<uint8_t> body = rig.save();
	CHECK(marked([&] { REQUIRE(pr::kContainerOps.loadState(rig.mem, body.data(), body.size())); }) == 0);
}

TEST_CASE("NO: a chain put back is the chain that went — its id, devices, level, mute and solo by value") {
	RackRig rig;
	pr::Rack& rack = rig.rack();
	REQUIRE(rack.addChain() == 1);
	(void)rig.seat(1, kGainDesc, 3.0f);
	rack.setLevel(1, 0.75f);
	rack.setMuted(1, true);
	const uint16_t id = rack.chainIdAt(1);
	const pr::Rack::ChainSnapshot taken = rack.snapshotChain(1);
	REQUIRE(rack.removeChain(1));
	CHECK(rack.insertChain(0, taken) == 0); // at the index asked
	CHECK(rack.insertChain(0, taken) == SIZE_MAX); // its id is held now: refused
	CHECK(rack.chainIdAt(0) == id);
	CHECK(rack.level(0) == 0.75f);
	CHECK(rack.muted(0));
	REQUIRE(rack.chainAt(0)->bridge() != nullptr);
	CHECK(rack.chainAt(0)->bridge()->chainLength() == 1);
	// AND IT SOUNDS AS IT DID: unmuted, x3 at 2.25 beside the empty chain's x1.
	rack.setMuted(0, false);
	rig.settle();
	const std::vector<float> in = ramp();
	const std::vector<float> out = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == doctest::Approx(2.25f * 3.0f * in[i] + in[i]).epsilon(1e-6));
	}
	// A NEW CHAIN AFTER IT TAKES A FRESH ID, never the put-back one's.
	REQUIRE(rack.addChain() == 2);
	CHECK(rack.chainIdAt(2) != id);
	CHECK(rack.chainIdAt(2) != rack.chainIdAt(1));
}

TEST_CASE("NO: inside a Rack a device's Poly switch says it is unavailable — the chain's host answers why, a Chain nested in the Rack answers the same, and a Chain in Feathers' lane does not") {
	RackRig rig(hostCatalogue());
	rack::RackBridge* chain = rig.rack().chainAt(0)->bridge();
	REQUIRE(chain != nullptr);
	CHECK(chain->perVoiceUnavailable() == pr::kPolyInsideRack);
	// A CHAIN INSIDE THE RACK'S CHAIN: its devices have no voices either, and its host says so.
	const DeviceInstanceId chainDevice = chain->addDevice(composite::kTraits.tag);
	REQUIRE(chainDevice != kNoInstance);
	rig.settle();
	rack::RackEngine& engine = rig.rack().chainAt(0)->engine();
	rack::RackBridge* nested = rack::bridgeOf(engine.slotAtLane(engine.slotOf(chainDevice)).storage.get());
	REQUIRE(nested != nullptr);
	CHECK(nested->perVoiceUnavailable() == pr::kPolyInsideRack);
	// AND A CHAIN THAT DOES PASS VOICES THROUGH — at the top of Feathers' lane — is not marked.
	ParamSet params;
	Synth synth { params };
	ParamBridge bridge;
	synth.activate(kRate, 4096);
	synth.setCatalogue(hostCatalogue());
	synth.setOuterGateMaker(&ParamBridge::outerGateOver, &bridge);
	const size_t slot = bridge.addFxDevice(composite::kTraits.tag);
	REQUIRE(slot < kMaxFxSlots);
	bridge.refresh();
	publishEditorLanes(bridge, params, synth);
	(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	rack::RackBridge* lane = rack::bridgeOf(bridge.fxBodies().surfaceOf(bridge.fxDeviceAt(slot)));
	REQUIRE(lane != nullptr);
	CHECK(lane->perVoiceUnavailable() == nullptr);
}

TEST_CASE("NO: a Rack chain edit in Feathers' lane marks FEATHERS' patch dirty, through the gate the plugin hands its containers") {
	ParamSet params;
	Synth synth { params };
	ParamBridge bridge;
	synth.activate(kRate, 4096);
	synth.setCatalogue(hostCatalogue());
	synth.setOuterGateMaker(&ParamBridge::outerGateOver, &bridge); // as feathers_clap.cpp wires it
	const size_t slot = bridge.addFxDevice(pr::kTraits.tag);
	REQUIRE(slot < kMaxFxSlots);
	bridge.refresh();
	publishEditorLanes(bridge, params, synth);
	(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
	pr::Rack* rack = pr::rackOf(bridge.fxBodies().surfaceOf(bridge.fxDeviceAt(slot)));
	REQUIRE(rack != nullptr);
	(void)bridge.takeStateDirty(); // the add itself
	CHECK_FALSE(bridge.takeStateDirty());
	rack->setMuted(0, true);
	CHECK(bridge.takeStateDirty());
	REQUIRE(rack->addChain() == 1);
	CHECK(bridge.takeStateDirty());
	CHECK_FALSE(bridge.takeStateDirty());
}

TEST_CASE("OC: each chain publishes its own output peak — after its fader, mute and solo, held, and absent before it has run") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	REQUIRE(rig.rack().addChain() == 2);
	(void)rig.seat(0, kGainDesc, 2.0f);
	(void)rig.seat(1, kGainDesc, 3.0f);
	// chain 2 is empty: the dry path, so its peak is the input's.
	float peak = -7.0f;
	CHECK_FALSE(rig.rack().chainPeak(0, peak)); // nothing has run: no meter, not a silent one
	CHECK(peak == -7.0f);                       // and the caller's value is untouched
	rig.rack().setLevel(0, 0.25f);              // the Chain's fader curve: position 0.25 is a gain of 0.25
	rig.rack().setMuted(2, true);
	rig.settle();
	const std::vector<float> steady(kBlock, 0.1f);
	(void)rig.run(steady);
	// PER CHAIN, AND EACH ITS OWN: 0.1 x probe gain x the fader's gain (three different numbers, so a swap shows).
	REQUIRE(rig.rack().chainPeak(0, peak));
	CHECK(peak == doctest::Approx(0.1f * 2.0f * 0.25f).epsilon(1e-5));
	REQUIRE(rig.rack().chainPeak(1, peak));
	CHECK(peak == doctest::Approx(0.1f * 3.0f).epsilon(1e-5));
	REQUIRE(rig.rack().chainPeak(2, peak));
	CHECK(peak == 0.0f); // muted: ran, and heard nothing
	CHECK_FALSE(rig.rack().chainPeak(3, peak)); // no such chain
	// A SOLO SILENCES THE OTHERS' METERS TOO (they are not heard), and the soloed chain keeps its reading.
	rig.rack().setMuted(2, false);
	rig.rack().setSoloed(1, true);
	rig.settle();
	// SETTLE ran silence: the held peaks must not have fallen yet (the hold is half a second, a settle is 21 ms).
	REQUIRE(rig.rack().chainPeak(1, peak));
	CHECK(peak == doctest::Approx(0.1f * 3.0f).epsilon(1e-5));
	// …and a quieter block after the hold has run out reads quieter: the meter FALLS.
	const std::vector<float> silence(kBlock, 0.0f);
	for (int b = 0; b < 200; ++b) { // 200 x 256 samples = 1.07 s at 48 kHz: the 0.5 s hold, then 0.57 s of fall
		(void)rig.run(silence);
	}
	float later = 0.0f;
	REQUIRE(rig.rack().chainPeak(1, later));
	CHECK(later < peak);
	CHECK(later > 0.0f);
	// THE SOLOED-OUT CHAIN, driven again, stays silent in its meter: its fader is shut, so nothing reaches the sum.
	(void)rig.run(steady);
	REQUIRE(rig.rack().chainPeak(0, peak));
	CHECK(peak < 0.1f * 2.0f * 0.25f);
	// A RESET FORGETS THEM.
	pr::kAudioOps.reset(rig.mem);
	CHECK_FALSE(rig.rack().chainPeak(0, peak));
}

TEST_CASE("OC: a chain's open state is kept with the patch by chain id — closed by default, saved and loaded by value, dirty on change, and put back with the chain") {
	RackRig rig;
	DirtyCount dirty;
	pr::kContainerOps.setOuterGate(rig.mem, countingGate(dirty));
	pr::Rack& rack = rig.rack();
	REQUIRE(rack.addChain() == 1);
	REQUIRE(rack.addChain() == 2);
	CHECK_FALSE(rack.open(0)); // closed by default, every chain
	CHECK_FALSE(rack.open(1));
	CHECK_FALSE(rack.open(7)); // no such chain
	const auto marked = [&](auto&& edit) {
		const int before = dirty.marks;
		edit();
		return dirty.marks - before;
	};
	// VIEW STATE LIKE A CARD'S FOLD: a change marks the holder dirty, the same value does not.
	CHECK(marked([&] { rack.setOpen(1, true); }) == 1);
	CHECK(marked([&] { rack.setOpen(1, true); }) == 0);
	CHECK(rack.open(1));
	CHECK_FALSE(rack.open(0));
	CHECK_FALSE(rack.open(2));
	// IT FOLLOWS THE CHAIN, NOT THE POSITION: take chain 0 out, and the open chain is now at 0.
	const uint16_t openId = rack.chainIdAt(1);
	const pr::Rack::ChainSnapshot first = rack.snapshotChain(0);
	REQUIRE(rack.removeChain(0));
	CHECK(rack.chainIdAt(0) == openId);
	CHECK(rack.open(0));
	// A CHAIN PUT BACK COMES BACK AS IT WENT — open stays with the snapshot of an open chain.
	const pr::Rack::ChainSnapshot taken = rack.snapshotChain(0);
	CHECK(taken.open);
	REQUIRE(rack.removeChain(0));
	REQUIRE(rack.insertChain(0, taken) == 0);
	CHECK(rack.open(0));
	CHECK_FALSE(first.open);
	REQUIRE(rack.insertChain(0, first) == 0);
	CHECK_FALSE(rack.open(0));
	CHECK(rack.open(1));
	// SAVED AND LOADED INTO A FRESH RACK, BY VALUE, and the load is not an edit.
	rack.setOpen(2, true);
	const std::vector<uint8_t> body = rig.save();
	RackRig fresh;
	DirtyCount freshDirty;
	pr::kContainerOps.setOuterGate(fresh.mem, countingGate(freshDirty));
	REQUIRE(pr::kContainerOps.loadState(fresh.mem, body.data(), body.size()));
	REQUIRE(fresh.rack().chainCount() == 3);
	CHECK_FALSE(fresh.rack().open(0));
	CHECK(fresh.rack().open(1));
	CHECK(fresh.rack().open(2));
	CHECK(freshDirty.marks == 0);
	// AND IT IS NOT AN UNDO STEP, NOR SOUND: the sum is the same with every chain open or closed.
	rig.settle();
	const std::vector<float> in = ramp();
	const std::vector<float> a = rig.run(in);
	rack.setOpen(1, false);
	const std::vector<float> b = rig.run(in);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(a[i] == b[i]);
	}
}

//------------------------------------------------------------------------------------------------//
// STAGE 2, STEP 1: SELECT AND BLEND                                                                //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE WEIGHT §1 GIVES CHAIN `k` OF `n` — restated here from the spec's words, not read from the rack: Select hears the
/// nearest chain (chain k at k/(n−1)), Blend the linear triangle of the two either side.
float specWeight(pr::Mode mode, double position, size_t k, size_t n) {
	if (mode == pr::Mode::Parallel || n == 1) {
		return 1.0f;
	}
	const double x = position * static_cast<double>(n - 1);
	if (mode == pr::Mode::Select) {
		return std::fabs(x - static_cast<double>(k)) < 0.5 || (std::fabs(x - static_cast<double>(k)) == 0.5 && x < static_cast<double>(k))
		           ? 1.0f
		           : 0.0f;
	}
	const double d = std::fabs(x - static_cast<double>(k));
	return d < 1.0 ? static_cast<float>(1.0 - d) : 0.0f;
}

/// A RACK OF `gains.size()` CHAINS, chain k holding one ×gains[k] probe, in `mode` at `position`, settled.
void buildTagged(RackRig& rig, const std::vector<float>& gains, pr::Mode mode, float position) {
	for (size_t k = 1; k < gains.size(); ++k) {
		REQUIRE(rig.rack().addChain() == k);
	}
	for (size_t k = 0; k < gains.size(); ++k) {
		(void)rig.seat(k, kGainDesc, gains[k]);
	}
	rig.set(pr::Param::Mode, static_cast<float>(mode));
	rig.set(pr::Param::Position, position);
	rig.settle();
}

} // namespace

TEST_CASE("Rack stage 2: with every chain empty, Select and Blend pass the input — at one, two, three and five chains and any position") {
	// §1's last column: an empty chain is the dry path, and the weights sum to one, so the rack is the identity.
	for (const pr::Mode mode : { pr::Mode::Select, pr::Mode::Blend }) {
		for (const size_t chains : { size_t { 1 }, size_t { 2 }, size_t { 3 }, size_t { 5 } }) {
			for (const float position : { 0.0f, 0.25f, 0.5f, 0.7f, 1.0f }) {
				CAPTURE(static_cast<int>(mode));
				CAPTURE(chains);
				CAPTURE(position);
				RackRig rig;
				for (size_t k = 1; k < chains; ++k) {
					REQUIRE(rig.rack().addChain() == k);
				}
				rig.set(pr::Param::Mode, static_cast<float>(mode));
				rig.set(pr::Param::Position, position);
				rig.settle();
				REQUIRE(rig.rack().modeForTest() == mode);
				const std::vector<float> in = ramp();
				std::vector<float> right;
				const std::vector<float> out = rig.run(in, &right);
				// AT A CHAIN (every Select position, a Blend position on a node) the one chain at weight 1: bit for bit.
				const double x = static_cast<double>(position) * static_cast<double>(chains - 1);
				const bool onNode = mode == pr::Mode::Select || chains == 1 || x == std::floor(x);
				for (size_t i = 0; i < in.size(); ++i) {
					if (onNode) {
						REQUIRE(out[i] == in[i]);
						REQUIRE(right[i] == in[i]);
					}
					else {
						// (1 − f)·x + f·x in float: the input to the last bit or so.
						REQUIRE(out[i] == doctest::Approx(in[i]).epsilon(1e-6));
						REQUIRE(right[i] == doctest::Approx(in[i]).epsilon(1e-6));
					}
				}
			}
		}
	}
}

TEST_CASE("Rack stage 2, Select: the chain nearest the position is the one heard — chains tagged x2, x3, x5, read at each position") {
	const std::vector<float> gains { 2.0f, 3.0f, 5.0f };
	// THE MIDPOINTS BETWEEN CHAINS ARE 0.25 AND 0.75: either side of each, by value.
	const std::vector<std::pair<float, size_t>> cases {
		{ 0.0f, 0 }, { 0.24f, 0 }, { 0.26f, 1 }, { 0.5f, 1 }, { 0.74f, 1 }, { 0.76f, 2 }, { 1.0f, 2 },
	};
	for (const auto& [position, heard] : cases) {
		CAPTURE(position);
		RackRig rig;
		buildTagged(rig, gains, pr::Mode::Select, position);
		CHECK(pr::selectedChain(position, 3) == heard);
		for (size_t k = 0; k < 3; ++k) {
			CHECK(rig.rack().chainWeightForTest(k) == specWeight(pr::Mode::Select, position, k, 3));
			CHECK(rig.rack().chainWeightForTest(k) == (k == heard ? 1.0f : 0.0f));
		}
		std::vector<float> impulse(kBlock, 0.0f);
		impulse[0] = 1.0f;
		const std::vector<float> out = rig.run(impulse);
		CHECK(out[0] == gains[heard]);
		for (size_t i = 1; i < out.size(); ++i) {
			REQUIRE(out[i] == 0.0f);
		}
	}
}

TEST_CASE("Rack stage 2, Select: a change of chain is a 10 ms EQUAL-GAIN crossfade, read sample by sample") {
	RackRig rig;
	buildTagged(rig, { 2.0f, 3.0f, 5.0f }, pr::Mode::Select, 0.0f);
	REQUIRE(rig.rack().chainWeightForTest(0) == 1.0f);
	// 10 ms AT 48 kHz, restated: 480 samples. Chain x2 out, chain x5 in, linearly and on one count.
	constexpr int kSelect = 480;
	rig.set(pr::Param::Position, 1.0f);
	const std::vector<float> ones(1024, 1.0f);
	const std::vector<float> out = rig.run(ones);
	for (int i = 0; i < 1024; ++i) {
		CAPTURE(i);
		const float t = i < kSelect ? static_cast<float>(i + 1) / static_cast<float>(kSelect) : 1.0f;
		REQUIRE(out[static_cast<size_t>(i)] == doctest::Approx(2.0f * (1.0f - t) + 5.0f * t).epsilon(1e-5));
	}
	// EQUAL-GAIN AT THE MIDPOINT, BY THE WEIGHTS THEMSELVES: half each, summing to one (not 0.707 each).
	rig.set(pr::Param::Position, 0.0f);
	(void)rig.run(std::vector<float>(kSelect / 2, 1.0f));
	CHECK(rig.rack().chainWeightForTest(0) == doctest::Approx(0.5f));
	CHECK(rig.rack().chainWeightForTest(2) == doctest::Approx(0.5f));
	CHECK(rig.rack().chainWeightForTest(1) == 0.0f);
	CHECK(rig.rack().chainWeightForTest(0) + rig.rack().chainWeightForTest(2) == doctest::Approx(1.0f).epsilon(1e-5));
}

TEST_CASE("Rack stage 2, Blend: the weights at 0, 1/4, 1/2 and 1 across three chains, read as the summed gain; identical chains are flat across a sweep") {
	const std::vector<float> gains { 2.0f, 3.0f, 5.0f };
	for (const float position : { 0.0f, 0.25f, 0.5f, 0.7f, 1.0f }) {
		CAPTURE(position);
		RackRig rig;
		buildTagged(rig, gains, pr::Mode::Blend, position);
		float want = 0.0f;
		for (size_t k = 0; k < 3; ++k) {
			const float w = specWeight(pr::Mode::Blend, position, k, 3);
			CHECK(rig.rack().chainWeightForTest(k) == doctest::Approx(w).epsilon(1e-6));
			want += w * gains[k];
		}
		const std::vector<float> in = ramp();
		const std::vector<float> out = rig.run(in);
		for (size_t i = 0; i < in.size(); ++i) {
			REQUIRE(out[i] == doctest::Approx(want * in[i]).epsilon(1e-5));
		}
	}
	// THE SUMS RESTATED BY HAND, so `specWeight` is not checked only against itself: 2, (2+3)/2, 3, 5.
	CHECK(specWeight(pr::Mode::Blend, 0.25, 0, 3) * 2.0f + specWeight(pr::Mode::Blend, 0.25, 1, 3) * 3.0f == 2.5f);
	// IDENTICAL CHAINS SOUND THE SAME AT EVERY POSITION, moving block by block (the glide included).
	RackRig flat;
	buildTagged(flat, { 3.0f, 3.0f, 3.0f }, pr::Mode::Blend, 0.0f);
	const std::vector<float> in = ramp();
	for (int step = 0; step <= 20; ++step) {
		flat.set(pr::Param::Position, static_cast<float>(step) / 20.0f);
		const std::vector<float> out = flat.run(in);
		for (size_t i = 0; i < in.size(); ++i) {
			REQUIRE(out[i] == doctest::Approx(3.0f * in[i]).epsilon(1e-5));
		}
	}
}

TEST_CASE("Rack stage 2: a mode change fades the chains' sum to silence over 5 ms and back over 5 ms, sample by sample, and the chains are not reset") {
	// PARALLEL x2 + x3 = 5, THEN SELECT AT 1: the sum falls to 0 over 240 samples, the mode switches, and Select's x3
	// rises over the next 240.
	RackRig rig;
	buildTagged(rig, { 2.0f, 3.0f }, pr::Mode::Parallel, 1.0f);
	REQUIRE(rig.rack().modeForTest() == pr::Mode::Parallel);
	rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	const std::vector<float> ones(1024, 1.0f);
	const std::vector<float> out = rig.run(ones);
	for (int i = 0; i < 1024; ++i) {
		CAPTURE(i);
		float want = 3.0f;
		if (i < kRamp) {
			want = 5.0f * (1.0f - static_cast<float>(i + 1) / static_cast<float>(kRamp));
		}
		else if (i < 2 * kRamp) {
			want = 3.0f * static_cast<float>(i - kRamp + 1) / static_cast<float>(kRamp);
		}
		REQUIRE(out[static_cast<size_t>(i)] == doctest::Approx(want).epsilon(1e-5));
	}
	CHECK(out[static_cast<size_t>(kRamp - 1)] == doctest::Approx(0.0f));
	CHECK(rig.rack().modeForTest() == pr::Mode::Select);
	CHECK(rig.rack().modeGainForTest() == 1.0f);

	// THE CHAINS KEEP THEIR STATE ACROSS THE SWITCH: an impulse sent into a 1000-sample delay in the same block the mode
	// changes comes out of it 1000 samples later, long after the fade — a chain reset at the switch would have lost it.
	RackRig late;
	REQUIRE(late.rack().addChain() == 1);
	(void)late.seat(0, kLateDesc, 1000.0f);
	late.set(pr::Param::Mode, static_cast<float>(pr::Mode::Parallel));
	late.settle(8);
	late.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	late.set(pr::Param::Position, 0.0f); // the delay's chain
	std::vector<float> impulse(2048, 0.0f);
	impulse[0] = 1.0f;
	const std::vector<float> got = late.run(impulse);
	for (size_t i = 0; i < got.size(); ++i) {
		CAPTURE(i);
		// SELECT HEARS THE DELAY'S CHAIN ONLY (the empty one, aligned to it, is not heard): exactly 1 at 1000.
		REQUIRE(got[i] == (i == 1000 ? 1.0f : 0.0f));
	}
}

TEST_CASE("Rack stage 2: a stage-1 row (no Mode, no Position) runs as Parallel — the same sum stage 1 made") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	(void)rig.seat(0, kGainDesc, 2.0f);
	(void)rig.seat(1, kGainDesc, 3.0f);
	// THE ROW STAGE 1 WROTE STOPS AT MACRO 8.
	const size_t stage1 = static_cast<size_t>(pr::Param::Macro8) + 1;
	REQUIRE(stage1 == static_cast<size_t>(pr::Param::Mode));
	rig.fields[static_cast<size_t>(pr::Param::Mode)] = static_cast<float>(pr::Mode::Select); // NOT handed over
	const std::vector<float> in = ramp();
	std::vector<float> out;
	for (int b = 0; b < 5; ++b) {
		std::vector<float> l = in;
		std::vector<float> r = in;
		pr::kAudioOps.setFields(rig.mem, rig.fields.data(), stage1);
		pr::kAudioOps.processStereo(rig.mem, l.data(), r.data(), static_cast<int>(l.size()));
		out = l;
	}
	CHECK(rig.rack().fieldForTest(pr::Param::Mode) == 0.0f);
	CHECK(rig.rack().modeForTest() == pr::Mode::Parallel);
	for (size_t i = 0; i < in.size(); ++i) {
		REQUIRE(out[i] == doctest::Approx(5.0f * in[i]).epsilon(1e-6));
	}
}

TEST_CASE("Rack stage 2, Select: the reported latency is the slowest of ALL chains and does not move on a switch; the selected chain lands on it to the sample") {
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	(void)rig.seat(0, kLateDesc, 100.0f);
	// chain 1 empty: no latency of its own.
	rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	for (const float position : { 0.0f, 1.0f, 0.0f, 1.0f }) {
		CAPTURE(position);
		rig.set(pr::Param::Position, position);
		rig.settle(8);
		CHECK(pr::kAudioOps.latencySamples(rig.mem, rig.fields.data(), rig.fields.size(), kRate) == 100u);
		std::vector<float> impulse(kBlock, 0.0f);
		impulse[0] = 1.0f;
		const std::vector<float> out = rig.run(impulse);
		for (int i = 0; i < kBlock; ++i) {
			CAPTURE(i);
			REQUIRE(out[static_cast<size_t>(i)] == (i == 100 ? 1.0f : 0.0f));
		}
	}
	// AND IN BLEND, BETWEEN THE TWO: still 100, both halves on sample 100.
	rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Blend));
	rig.set(pr::Param::Position, 0.5f);
	rig.settle(8);
	CHECK(pr::kAudioOps.latencySamples(rig.mem, rig.fields.data(), rig.fields.size(), kRate) == 100u);
}

TEST_CASE("Rack stage 2: holdsTail counts only the chains the mode lets through — an unselected chain still running a tail does not keep the rack awake") {
	// A CHAIN WHOSE DELAY STILL HOLDS SOUND (`Late` is not a tailed device, so its alignment does: the empty chain beside
	// a 2000-sample delay holds the impulse for 2000 samples) — the rack asks only the chains it hears.
	RackRig rig;
	REQUIRE(rig.rack().addChain() == 1);
	(void)rig.seat(1, kLateDesc, 2000.0f); // chain 1 slow; chain 0 empty, aligned to it
	rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	rig.set(pr::Param::Position, 0.0f); // the EMPTY chain heard
	rig.settle(8);
	std::vector<float> impulse(kBlock, 0.0f);
	impulse[0] = 1.0f;
	(void)rig.run(impulse);
	// THE HEARD CHAIN'S ALIGNMENT HOLDS THE IMPULSE: awake.
	CHECK(pr::kAudioOps.holdsTail(rig.mem));
	// NOW HEAR NOTHING BUT AN IDLE CHAIN... the delay's chain is selected; the empty one, still holding the impulse in its
	// alignment, is NOT heard once the crossfade is over.
	rig.set(pr::Param::Position, 1.0f);
	(void)rig.run(std::vector<float>(kBlock, 0.0f)); // mid crossfade (480 > 256): the outgoing chain still counts
	CHECK(pr::kAudioOps.holdsTail(rig.mem));
	(void)rig.run(std::vector<float>(kBlock, 0.0f)); // past it
	CHECK(rig.rack().chainWeightForTest(0) == 0.0f);
	// The selected chain's own delay holds the impulse too, so the rack is still awake for IT; mute it to isolate the
	// unselected one: then nothing heard holds anything.
	rig.rack().setMuted(1, true);
	rig.settle(2);
	CHECK_FALSE(pr::kAudioOps.holdsTail(rig.mem));
}

TEST_CASE("Rack stage 2: Mode and Position are saved with the patch — each mode into a fresh instance by value, and the running rack takes it") {
	for (const pr::Mode mode : { pr::Mode::Parallel, pr::Mode::Select, pr::Mode::Blend, pr::Mode::MidSide, pr::Mode::LeftRight,
	                             pr::Mode::Bands }) {
		CAPTURE(static_cast<int>(mode));
		std::vector<uint8_t> body;
		DeviceInstanceId inner = kNoInstance;
		{
			RackRig rig;
			rack::RackBridge* outer = rig.rack().chainAt(0)->bridge();
			inner = outer->addDevice(pr::kTraits.tag);
			REQUIRE(inner != kNoInstance);
			outer->setField(inner, static_cast<size_t>(pr::Param::Mode), static_cast<float>(mode));
			outer->setField(inner, static_cast<size_t>(pr::Param::Position), 0.3f);
			outer->setField(inner, static_cast<size_t>(pr::Param::BandCount), 2.0f);  // 5 bands
			outer->setField(inner, static_cast<size_t>(pr::Param::Filtering), 1.0f); // After
			outer->setField(inner, static_cast<size_t>(pr::Param::Cut2), 9.5f);
			rig.settle();
			body = rig.save();
		}
		RackRig fresh;
		REQUIRE(pr::kContainerOps.loadState(fresh.mem, body.data(), body.size()));
		rack::RackBridge* outer = fresh.rack().chainAt(0)->bridge();
		CHECK(outer->fieldAt(inner, static_cast<size_t>(pr::Param::Mode)) == static_cast<float>(mode));
		CHECK(outer->fieldAt(inner, static_cast<size_t>(pr::Param::Position)) == 0.3f);
		fresh.settle();
		rack::RackEngine& engine = fresh.rack().chainAt(0)->engine();
		void* storage = engine.slotAtLane(engine.slotOf(inner)).storage.get();
		REQUIRE(storage != nullptr);
		CHECK(pr::rackOf(storage)->modeForTest() == mode);
		CHECK(pr::rackOf(storage)->fieldForTest(pr::Param::Position) == 0.3f);
		CHECK(outer->fieldAt(inner, static_cast<size_t>(pr::Param::BandCount)) == 2.0f);
		CHECK(outer->fieldAt(inner, static_cast<size_t>(pr::Param::Filtering)) == 1.0f);
		CHECK(outer->fieldAt(inner, static_cast<size_t>(pr::Param::Cut2)) == 9.5f);
		if (mode == pr::Mode::Bands) {
			CHECK(pr::rackOf(storage)->bandsForTest() == 5);
			CHECK(pr::rackOf(storage)->filteringForTest() == pr::Filtering::After);
		}
	}
}

TEST_CASE("Rack stage 2: the position in the DAW's words and back — 'Chain 2' in Select, 'Chain 2 → 3, 40%' in Blend, the unit in Parallel") {
	char text[64] = {};
	// SELECT, three chains: the nearest, 1-based.
	REQUIRE(pr::formatPosition(pr::Mode::Select, 3, 0.0, text, sizeof text));
	CHECK(std::string(text) == "Chain 1");
	REQUIRE(pr::formatPosition(pr::Mode::Select, 3, 0.5, text, sizeof text));
	CHECK(std::string(text) == "Chain 2");
	REQUIRE(pr::formatPosition(pr::Mode::Select, 3, 0.8, text, sizeof text));
	CHECK(std::string(text) == "Chain 3");
	// BLEND: 0.7 of three chains is 1.4 — 40% of the way from chain 2 to chain 3; on a chain, that chain.
	REQUIRE(pr::formatPosition(pr::Mode::Blend, 3, 0.7, text, sizeof text));
	CHECK(std::string(text) == "Chain 2 \xE2\x86\x92 3, 40%");
	REQUIRE(pr::formatPosition(pr::Mode::Blend, 3, 0.5, text, sizeof text));
	CHECK(std::string(text) == "Chain 2");
	REQUIRE(pr::formatPosition(pr::Mode::Blend, 3, 1.0, text, sizeof text));
	CHECK(std::string(text) == "Chain 3");
	REQUIRE(pr::formatPosition(pr::Mode::Blend, 1, 0.6, text, sizeof text));
	CHECK(std::string(text) == "Chain 1");
	// PARALLEL: the unit prints.
	CHECK_FALSE(pr::formatPosition(pr::Mode::Parallel, 3, 0.5, text, sizeof text));

	// THE INVERSE: a chain's number is its centre; the Blend form is where it says.
	double p = -1.0;
	REQUIRE(pr::parsePosition(pr::Mode::Select, 3, "Chain 2", &p));
	CHECK(p == 0.5);
	REQUIRE(pr::parsePosition(pr::Mode::Select, 3, "3", &p));
	CHECK(p == 1.0);
	REQUIRE(pr::parsePosition(pr::Mode::Blend, 5, "chain 1", &p));
	CHECK(p == 0.0);
	REQUIRE(pr::parsePosition(pr::Mode::Blend, 3, "Chain 2 \xE2\x86\x92 3, 40%", &p));
	CHECK(p == doctest::Approx(0.7).epsilon(1e-12));
	REQUIRE(pr::parsePosition(pr::Mode::Blend, 3, "2 -> 3, 40", &p));
	CHECK(p == doctest::Approx(0.7).epsilon(1e-12));
	CHECK_FALSE(pr::parsePosition(pr::Mode::Select, 3, "Chain 4", &p));      // no such chain
	CHECK_FALSE(pr::parsePosition(pr::Mode::Blend, 3, "Chain 2 -> 1, 40%", &p)); // not a neighbour above
	CHECK_FALSE(pr::parsePosition(pr::Mode::Select, 3, "50%", &p));          // the unit's to read
	CHECK_FALSE(pr::parsePosition(pr::Mode::Parallel, 3, "Chain 2", &p));
	// EVERY TEXT READS BACK TO A POSITION THAT PRINTS THE SAME TEXT.
	for (const pr::Mode mode : { pr::Mode::Select, pr::Mode::Blend }) {
		for (int step = 0; step <= 40; ++step) {
			const double at = static_cast<double>(step) / 40.0;
			CAPTURE(at);
			REQUIRE(pr::formatPosition(mode, 4, at, text, sizeof text));
			double back = -1.0;
			REQUIRE(pr::parsePosition(mode, 4, text, &back));
			char again[64] = {};
			REQUIRE(pr::formatPosition(mode, 4, back, again, sizeof again));
			CHECK(std::string(again) == std::string(text));
		}
	}
	// AND THE DESCRIPTOR'S HOOK IS THESE TWO: only the position, only with a rack behind the view.
	REQUIRE(pr::kDescriptor.text == &pr::kTextOps);
	REQUIRE(pr::kTextOps.format != nullptr);
	REQUIRE(pr::kTextOps.parse != nullptr);
}

TEST_CASE("Rack stage 2: a mode change whose fade reaches silence on a chunk's LAST sample hands the next chunk the new mode's weights") {
	// THE EDGE OF THE SWITCH: a 240-sample block is exactly the 5 ms fade, so silence lands on its last sample and no
	// sample of that chunk is the new mode's — the weights must still be the new mode's when the chunk ends.
	RackRig rig;
	buildTagged(rig, { 2.0f, 3.0f }, pr::Mode::Parallel, 1.0f);
	REQUIRE(rig.rack().chainWeightForTest(0) == 1.0f);
	REQUIRE(rig.rack().chainWeightForTest(1) == 1.0f);
	rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Select));
	const std::vector<float> ones(static_cast<size_t>(kRamp), 1.0f);
	const std::vector<float> down = rig.run(ones);
	for (int i = 0; i < kRamp; ++i) {
		CAPTURE(i);
		REQUIRE(down[static_cast<size_t>(i)] == doctest::Approx(5.0f * (1.0f - static_cast<float>(i + 1) / static_cast<float>(kRamp))).epsilon(1e-5));
	}
	// AFTER THE CHUNK, BY VALUE: Select at position 1 hears chain 1 alone, the mode has switched, the fade is at silence.
	CHECK(rig.rack().modeForTest() == pr::Mode::Select);
	CHECK(rig.rack().modeGainForTest() == 0.0f);
	CHECK(rig.rack().chainWeightForTest(0) == 0.0f);
	CHECK(rig.rack().chainWeightForTest(1) == 1.0f);
	// AND THE NEXT CHUNK FADES IN THE NEW MODE ONLY: x3, rising over 240 samples.
	const std::vector<float> up = rig.run(ones);
	for (int i = 0; i < kRamp; ++i) {
		CAPTURE(i);
		REQUIRE(up[static_cast<size_t>(i)] == doctest::Approx(3.0f * static_cast<float>(i + 1) / static_cast<float>(kRamp)).epsilon(1e-5));
	}
	CHECK(rig.rack().modeGainForTest() == 1.0f);
}

//------------------------------------------------------------------------------------------------//
// STAGE 2, STEP 2: MID/SIDE AND LEFT/RIGHT, ROLES, AND THE CHAINS THEY DO NOT ROUTE (§2a, §5)       //
//------------------------------------------------------------------------------------------------//

namespace {

/// A STEREO INPUT WHOSE M/S ARITHMETIC IS EXACT IN FLOAT — small dyadic values, so ½(L+R) ± ½(L−R) is L and R to the
/// bit and an identity can be asserted with `==`.
void dyadic(std::vector<float>& l, std::vector<float>& r, int n = kBlock) {
	l.resize(static_cast<size_t>(n));
	r.resize(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		l[static_cast<size_t>(i)] = static_cast<float>(i % 37 + 1) / 256.0f;
		r[static_cast<size_t>(i)] = -static_cast<float>(i % 11) / 64.0f;
	}
}

/// ONE BLOCK OF A STEREO INPUT through the rig; both channels out.
void runStereo(RackRig& rig, const std::vector<float>& inL, const std::vector<float>& inR, std::vector<float>& outL,
               std::vector<float>& outR) {
	outL = inL;
	outR = inR;
	pr::kAudioOps.setFields(rig.mem, rig.fields.data(), rig.fields.size());
	pr::kAudioOps.processStereo(rig.mem, outL.data(), outR.data(), static_cast<int>(outL.size()));
}

} // namespace

TEST_CASE("Rack stage 2: with every chain empty, Mid/Side and Left/Right pass the input EXACTLY — at one chain (a role missing), two, and three (one unrouted)") {
	for (const pr::Mode mode : { pr::Mode::MidSide, pr::Mode::LeftRight }) {
		for (const size_t chains : { size_t { 1 }, size_t { 2 }, size_t { 3 } }) {
			CAPTURE(static_cast<int>(mode));
			CAPTURE(chains);
			RackRig rig;
			for (size_t k = 1; k < chains; ++k) {
				REQUIRE(rig.rack().addChain() == k);
			}
			rig.set(pr::Param::Mode, static_cast<float>(mode));
			rig.settle();
			REQUIRE(rig.rack().modeForTest() == mode);
			std::vector<float> inL, inR, outL, outR;
			dyadic(inL, inR);
			runStereo(rig, inL, inR, outL, outR);
			for (size_t i = 0; i < inL.size(); ++i) {
				REQUIRE(outL[i] == inL[i]);
				REQUIRE(outR[i] == inR[i]);
			}
		}
	}
}

TEST_CASE("Rack stage 2, Mid/Side (form A): the Mid chain is handed (M, M) and the Side (S, −S) — by value through x2, x0 and a mono-summing Side") {
	std::vector<float> inL, inR, outL, outR;
	dyadic(inL, inR);
	const auto expect = [&](float midGain, float sideGain) {
		for (size_t i = 0; i < inL.size(); ++i) {
			const float m = 0.5f * (inL[i] + inR[i]);
			const float s = 0.5f * (inL[i] - inR[i]);
			REQUIRE(outL[i] == midGain * m + sideGain * s);
			REQUIRE(outR[i] == midGain * m - sideGain * s);
		}
	};
	{
		RackRig rig; // THE SIDE AT x2: (M + 2S, M − 2S) — the −S on the right is the side's own sign
		REQUIRE(rig.rack().addChain() == 1);
		(void)rig.seat(1, kGainDesc, 2.0f);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::MidSide));
		rig.settle();
		runStereo(rig, inL, inR, outL, outR);
		expect(1.0f, 2.0f);
	}
	{
		RackRig rig; // THE SIDE AT x0: MONO, (M, M), exactly
		REQUIRE(rig.rack().addChain() == 1);
		(void)rig.seat(1, kGainDesc, 0.0f);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::MidSide));
		rig.settle();
		runStereo(rig, inL, inR, outL, outR);
		expect(1.0f, 0.0f);
		for (size_t i = 0; i < inL.size(); ++i) {
			REQUIRE(outL[i] == outR[i]);
		}
	}
	{
		RackRig rig; // FORM A, PINNED: a device that sums to mono, handed (S, −S), outputs ZERO — the side is gone
		REQUIRE(rig.rack().addChain() == 1);
		(void)rig.seat(1, kMonoDesc, 0.0f);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::MidSide));
		rig.settle();
		runStereo(rig, inL, inR, outL, outR);
		expect(1.0f, 0.0f);
	}
	{
		RackRig rig; // THE MID AT x3 with the side untouched
		REQUIRE(rig.rack().addChain() == 1);
		(void)rig.seat(0, kGainDesc, 3.0f);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::MidSide));
		rig.settle();
		runStereo(rig, inL, inR, outL, outR);
		expect(3.0f, 1.0f);
	}
}

TEST_CASE("Rack stage 2, Left/Right (form A): the L chain is handed (L, 0) and the R chain (0, R) — a muted R chain zeroes the right, a mono L chain spreads into it") {
	std::vector<float> inL, inR, outL, outR;
	dyadic(inL, inR);
	{
		RackRig rig;
		REQUIRE(rig.rack().addChain() == 1);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::LeftRight));
		rig.rack().setMuted(1, true);
		rig.settle();
		runStereo(rig, inL, inR, outL, outR);
		for (size_t i = 0; i < inL.size(); ++i) {
			REQUIRE(outL[i] == inL[i]);
			REQUIRE(outR[i] == 0.0f);
		}
	}
	{
		RackRig rig; // (L, 0) SUMMED TO MONO: ½L in both, and the R chain's (0, R) on top
		REQUIRE(rig.rack().addChain() == 1);
		(void)rig.seat(0, kMonoDesc, 0.0f);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::LeftRight));
		rig.settle();
		runStereo(rig, inL, inR, outL, outR);
		for (size_t i = 0; i < inL.size(); ++i) {
			REQUIRE(outL[i] == 0.5f * inL[i]);
			REQUIRE(outR[i] == 0.5f * inL[i] + inR[i]);
		}
	}
}

TEST_CASE("Rack stage 2, roles: a missing role is an empty chain ALIGNED to the slowest; a chain past the roles is in neither the sum nor the latency") {
	std::vector<float> inL, inR, outL, outR;
	{
		// ONE CHAIN, 100 SAMPLES LATE, IN MID/SIDE: the missing Side passes the side, 100 samples late, so the rack is
		// the input delayed by exactly 100.
		RackRig rig;
		(void)rig.seat(0, kLateDesc, 100.0f);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::MidSide));
		rig.settle(8);
		CHECK(pr::kAudioOps.latencySamples(rig.mem, rig.fields.data(), rig.fields.size(), kRate) == 100u);
		dyadic(inL, inR, 1024);
		runStereo(rig, inL, inR, outL, outR);
		dyadic(inL, inR, 1024);
		runStereo(rig, inL, inR, outL, outR);
		for (size_t i = 0; i < outL.size(); ++i) {
			CAPTURE(i);
			const size_t from = (i + 1024 - 100) % 1024;
			REQUIRE(outL[i] == inL[from]);
			REQUIRE(outR[i] == inR[from]);
		}
	}
	{
		// THREE CHAINS, THE THIRD 100 SAMPLES LATE, IN LEFT/RIGHT: the third has no role — the latency is 0 and the
		// output is the input, now.
		RackRig rig;
		REQUIRE(rig.rack().addChain() == 1);
		REQUIRE(rig.rack().addChain() == 2);
		(void)rig.seat(2, kLateDesc, 100.0f);
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::LeftRight));
		rig.settle(8);
		CHECK(pr::kAudioOps.latencySamples(rig.mem, rig.fields.data(), rig.fields.size(), kRate) == 0u);
		dyadic(inL, inR);
		runStereo(rig, inL, inR, outL, outR);
		for (size_t i = 0; i < inL.size(); ++i) {
			REQUIRE(outL[i] == inL[i]);
			REQUIRE(outR[i] == inR[i]);
		}
	}
}

namespace {

/// A RACK OF THREE CHAINS, the third holding a full-wet reverb, run in Parallel on a burst, then switched to Mid/Side
/// (where chain 3 has no role) and run on silence. Returns the third chain's run count at the switch's end.
struct RingRig {
	RackRig rig { hostCatalogue() };
	explicit RingRig(bool stop) {
		REQUIRE(rig.rack().addChain() == 1);
		REQUIRE(rig.rack().addChain() == 2);
		rack::RackBridge* b = rig.rack().chainAt(2)->bridge();
		const DeviceInstanceId reverb = b->addDevice(fx::featherverb::kTraits.tag);
		REQUIRE(reverb != kNoInstance);
		b->setField(reverb, static_cast<size_t>(fx::featherverb::Param::Mix), 1.0f);
		b->setField(reverb, static_cast<size_t>(fx::featherverb::Param::Decay), 0.5f);
		rig.set(pr::Param::Unused, stop ? 1.0f : 0.0f);
		rig.settle(4);
		for (int k = 0; k < 8; ++k) {
			(void)rig.run(ramp()); // a burst into the reverb
		}
		rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::MidSide));
		for (int k = 0; k < 4; ++k) {
			(void)rig.run(std::vector<float>(kBlock, 0.0f)); // past the 10 ms of the switch
		}
		REQUIRE(rig.rack().modeForTest() == pr::Mode::MidSide);
	}
};

} // namespace

TEST_CASE("Rack stage 2, rack_unused = Ring out: a reverb unrouted mid-tail runs on silence until exact zero, then stops — and the output and the tail ignore it throughout") {
	RingRig ring(false);
	pr::Rack& rack = ring.rig.rack();
	const uint64_t atSwitch = rack.runsForTest(2);
	REQUIRE(atSwitch > 0);
	std::vector<float> inL, inR, outL, outR;
	int blocks = 0;
	uint64_t last = atSwitch;
	bool stopped = false;
	for (; blocks < 60 * 48000 / kBlock; ++blocks) {
		// THE OUTPUT IS THE MID/SIDE IDENTITY of the input, exactly — the ringing chain reaches nothing.
		dyadic(inL, inR);
		runStereo(ring.rig, inL, inR, outL, outR);
		for (size_t i = 0; i < inL.size(); ++i) {
			REQUIRE(outL[i] == inL[i]);
			REQUIRE(outR[i] == inR[i]);
		}
		// AND SLEEP IGNORES IT: nothing heard holds a tail.
		REQUIRE_FALSE(pr::kAudioOps.holdsTail(ring.rig.mem));
		const uint64_t now = rack.runsForTest(2);
		if (now == last) {
			stopped = true;
			break;
		}
		last = now;
	}
	CAPTURE(blocks);
	// IT RAN ON FOR A WHILE — a half-decay reverb's tail is long — and then it stopped, for good.
	CHECK(last > atSwitch + 10);
	CHECK(stopped);
	for (int k = 0; k < 20; ++k) {
		(void)ring.rig.run(std::vector<float>(kBlock, 0.0f));
	}
	CHECK(rack.runsForTest(2) == last);
}

TEST_CASE("Rack stage 2, rack_unused = Stop: an unrouted chain never runs after the switch, and comes back RESET — its frozen tail does not resume") {
	RingRig stop(true);
	pr::Rack& rack = stop.rig.rack();
	const uint64_t atSwitch = rack.runsForTest(2);
	for (int k = 0; k < 50; ++k) {
		(void)stop.rig.run(std::vector<float>(kBlock, 0.0f));
		REQUIRE(rack.runsForTest(2) == atSwitch);
	}
	// BACK TO PARALLEL on silence: chains 1 and 2 are empty, and chain 3 was reset — so the sum is exactly zero once
	// the switch is over. Under Ring out the same return would have brought its tail back.
	stop.rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Parallel));
	for (int k = 0; k < 4; ++k) {
		(void)stop.rig.run(std::vector<float>(kBlock, 0.0f));
	}
	CHECK(rack.runsForTest(2) > atSwitch);
	for (int k = 0; k < 8; ++k) {
		const std::vector<float> out = stop.rig.run(std::vector<float>(kBlock, 0.0f));
		for (const float v : out) {
			REQUIRE(v == 0.0f);
		}
	}
	// THE CONTROL: under Ring out, back after 50 blocks, the tail IS still there.
	RingRig ring(false);
	for (int k = 0; k < 50; ++k) {
		(void)ring.rig.run(std::vector<float>(kBlock, 0.0f));
	}
	ring.rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Parallel));
	for (int k = 0; k < 4; ++k) {
		(void)ring.rig.run(std::vector<float>(kBlock, 0.0f));
	}
	float peak = 0.0f;
	for (const float v : ring.rig.run(std::vector<float>(kBlock, 0.0f))) {
		peak = std::max(peak, std::fabs(v));
	}
	CHECK(peak > 0.0f);
}

//------------------------------------------------------------------------------------------------//
// STAGE 2, STEP 3: BANDS (§4)                                                                       //
//------------------------------------------------------------------------------------------------//

namespace {

namespace ox = feathers::fx::owltt;
constexpr double kPi = 3.14159265358979323846;

/// THE DECLARED DEFAULT CORNER k, in Hz, as the rack decodes it (a float field, then exp2).
double defaultCornerHz(size_t k) {
	const auto field = static_cast<float>(pr::kParamDescs[static_cast<size_t>(pr::Param::Cut1) + k].def);
	return std::exp2(static_cast<double>(field));
}

/// THE BILINEAR TRANSFORM'S WARPED Ω = tan(πf/fs) / tan(πfc/fs): the digital LR pair is the analogue prototype there.
double warped(double f, double fc) { return std::tan(kPi * f / kRate) / std::tan(kPi * fc / kRate); }
/// |LP| AND |HP| OF A LINKWITZ-RILEY PAIR, restated from the Butterworth prototypes squared — not from the code.
double lrLow(bool lr4, double w) { return lr4 ? 1.0 / (1.0 + std::pow(w, 4.0)) : 1.0 / (1.0 + w * w); }
double lrHigh(bool lr4, double w) { return lr4 ? std::pow(w, 4.0) / (1.0 + std::pow(w, 4.0)) : w * w / (1.0 + w * w); }
/// |BAND k| OF A K-BAND CASCADE at f: every corner below it high-passes it, its own corner low-passes it.
double bandGain(bool lr4, int bands, int k, const std::vector<double>& corners, double f) {
	double g = 1.0;
	for (int j = 0; j < k; ++j) {
		g *= lrHigh(lr4, warped(f, corners[static_cast<size_t>(j)]));
	}
	if (k + 1 < bands) {
		g *= lrLow(lr4, warped(f, corners[static_cast<size_t>(k)]));
	}
	return g;
}

int bandsOfChoice(int choice) { return choice == 0 ? 2 : (choice == 1 ? 3 : 5); }

/// A BANDS RACK of `chains` empty chains.
void bandsRig(RackRig& rig, size_t chains, int bandChoice, pr::Slope slope, pr::Filtering filtering) {
	for (size_t k = 1; k < chains; ++k) {
		REQUIRE(rig.rack().addChain() == k);
	}
	rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Bands));
	rig.set(pr::Param::BandCount, static_cast<float>(bandChoice));
	rig.set(pr::Param::Slope, static_cast<float>(slope));
	rig.set(pr::Param::Filtering, static_cast<float>(filtering));
	rig.settle();
	REQUIRE(rig.rack().modeForTest() == pr::Mode::Bands);
	REQUIRE(rig.rack().bandsForTest() == bandsOfChoice(bandChoice));
}

/// THE RACK'S GAIN AT `hz`: a unit sine run half a second to settle, then the RMS of out over in for half a second.
double gainAt(RackRig& rig, double hz) {
	double phase = 0.0;
	double in = 0.0;
	double out = 0.0;
	for (int b = 0; b < 2 * 48000 / kBlock; ++b) {
		std::vector<float> x(kBlock);
		for (float& v : x) {
			v = static_cast<float>(std::sin(phase));
			phase += 2.0 * kPi * hz / kRate;
		}
		const std::vector<float> y = rig.run(x);
		if (b >= 48000 / kBlock) {
			for (int i = 0; i < kBlock; ++i) {
				in += static_cast<double>(x[static_cast<size_t>(i)]) * x[static_cast<size_t>(i)];
				out += static_cast<double>(y[static_cast<size_t>(i)]) * y[static_cast<size_t>(i)];
			}
		}
	}
	return std::sqrt(out / in);
}

/// Π AP OF THE DEFAULT CORNERS OF A `bands`-BAND CASCADE, run on `x` by the crossover's own allpass chain — an
/// independent restatement of what an empty Bands rack must be.
template <class Split> std::vector<float> allpassOf(const std::vector<float>& x, int bands, ox::AllpassChain<Split, 5>& chain) {
	typename Split::Coeffs corners[4];
	for (int k = 0; k + 1 < bands; ++k) {
		corners[k] = Split::coeffsFor(static_cast<float>(defaultCornerHz(static_cast<size_t>(k))), static_cast<float>(kRate));
	}
	std::vector<float> y(x.size());
	for (size_t i = 0; i < x.size(); ++i) {
		y[i] = chain.process(x[i], corners, bands);
	}
	return y;
}

std::vector<float> noise(int n, uint32_t& seed) {
	std::vector<float> x(static_cast<size_t>(n));
	for (float& v : x) {
		seed = seed * 1664525u + 1013904223u;
		v = 0.5f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
	}
	return x;
}

} // namespace

TEST_CASE("Rack stage 2, Bands: with every chain empty the rack is Π AP of its corners — 2, 3 and 5 bands, LR4 and LR2, Before, After and Both, with chains below, at and above the band count") {
	for (const int choice : { 0, 1, 2 }) {
		const int bands = bandsOfChoice(choice);
		for (const pr::Slope slope : { pr::Slope::Lr4, pr::Slope::Lr2 }) {
			// BOTH JOINED THIS CASE WITH backlog OS: its half before and half after multiply to the LR band, so it sums
			// to Π AP too (it dipped about 6 dB at every corner when it filtered with the whole pair twice).
			for (const pr::Filtering filtering : { pr::Filtering::Before, pr::Filtering::After, pr::Filtering::Both }) {
				for (const size_t chains : { size_t { 1 }, static_cast<size_t>(bands), static_cast<size_t>(bands + 1) }) {
					CAPTURE(bands);
					CAPTURE(static_cast<int>(slope));
					CAPTURE(static_cast<int>(filtering));
					CAPTURE(chains);
					RackRig rig;
					bandsRig(rig, chains, choice, slope, filtering);
					// THE REFERENCE STARTS FROM REST WITH THE RACK: the rack's filters were reset at the switch into Bands,
					// and the settle fed them silence.
					ox::AllpassChain<ox::Lr4Split, 5> ap4;
					ox::AllpassChain<ox::Lr2Split, 5> ap2;
					uint32_t seed = 99u;
					float worst = 0.0f;
					for (int b = 0; b < 8; ++b) {
						const std::vector<float> x = noise(kBlock, seed);
						const std::vector<float> want = slope == pr::Slope::Lr4 ? allpassOf(x, bands, ap4) : allpassOf(x, bands, ap2);
						const std::vector<float> got = rig.run(x);
						for (size_t i = 0; i < x.size(); ++i) {
							worst = std::max(worst, std::fabs(got[i] - want[i]));
						}
					}
					CAPTURE(worst);
					// FLOAT ROUNDING through biquads with poles near the unit circle (LR4 at a 120 Hz corner): MEASURED
					// 1.95e-5 at worst on a ±0.5 noise, LR2 far below; the same order as the crossover's own K-band case.
					CHECK(worst < 5.0e-5f);
				}
			}
		}
	}
}

TEST_CASE("Rack stage 2, Bands (Before): each chain hears its band — a soloed chain's gain at its band's centre is the cascade's |band k|, restated from the Linkwitz-Riley prototypes") {
	std::vector<double> corners;
	for (size_t k = 0; k < 4; ++k) {
		corners.push_back(defaultCornerHz(k));
	}
	for (const pr::Slope slope : { pr::Slope::Lr4, pr::Slope::Lr2 }) {
		const bool lr4 = slope == pr::Slope::Lr4;
		for (int k = 0; k < 5; ++k) {
			CAPTURE(static_cast<int>(slope));
			CAPTURE(k);
			RackRig rig;
			bandsRig(rig, 5, 2, slope, pr::Filtering::Before);
			rig.rack().setSoloed(static_cast<size_t>(k), true);
			rig.settle();
			const double centre = k == 0 ? corners[0] / 2.0 : (k == 4 ? corners[3] * 2.0 : std::sqrt(corners[static_cast<size_t>(k - 1)] * corners[static_cast<size_t>(k)]));
			const double want = bandGain(lr4, 5, k, corners, centre);
			CAPTURE(want);
			CHECK(gainAt(rig, centre) == doctest::Approx(want).epsilon(0.01));
		}
	}
}

TEST_CASE("Rack stage 2, Bands (After): a chain's own output is kept to its band — a tone made in band 1 reaches the output at |LP(f)| of it, and in full under Before") {
	for (const pr::Slope slope : { pr::Slope::Lr4, pr::Slope::Lr2 }) {
		CAPTURE(static_cast<int>(slope));
		const bool lr4 = slope == pr::Slope::Lr4;
		const double cut = lr4 ? 4000.0 : 2000.0; // where the 8 kHz tone is measurably down, not lost in rounding
		double rms[2] = {};
		for (const pr::Filtering filtering : { pr::Filtering::Before, pr::Filtering::After }) {
			RackRig rig;
			REQUIRE(rig.rack().addChain() == 1);
			(void)rig.seat(0, kToneDesc, 8000.0f);
			rig.set(pr::Param::Cut1, static_cast<float>(std::log2(cut)));
			rig.set(pr::Param::Mode, static_cast<float>(pr::Mode::Bands));
			rig.set(pr::Param::BandCount, 0.0f); // 2 bands
			rig.set(pr::Param::Slope, static_cast<float>(slope));
			rig.set(pr::Param::Filtering, static_cast<float>(filtering));
			rig.settle(200); // a second: the filters settle on the tone
			double sq = 0.0;
			for (int b = 0; b < 100; ++b) {
				for (const float v : rig.run(std::vector<float>(kBlock, 0.0f))) {
					sq += static_cast<double>(v) * v;
				}
			}
			rms[filtering == pr::Filtering::Before ? 0 : 1] = std::sqrt(sq / (100.0 * kBlock));
		}
		const double tone = 0.5 / std::sqrt(2.0);
		CHECK(rms[0] == doctest::Approx(tone).epsilon(0.001)); // Before: the tone, whole — its harmonics land anywhere
		const double kept = lrLow(lr4, warped(8000.0, static_cast<double>(static_cast<float>(cut))));
		CAPTURE(kept);
		CHECK(rms[1] == doctest::Approx(kept * tone).epsilon(0.02)); // After: band 1's lowpass of it
		CHECK(rms[1] < 0.1 * rms[0]);
	}
}

TEST_CASE("OS: Rack Bands (Both) sums FLAT at every corner — LR4 and LR2, 2, 3 and 5 bands, by value; a soloed chain's output is its whole LR band (to be confirmed)") {
	// BACKLOG OS (decided 2026-10-30, to be confirmed — it changes the sound of Both): one Butterworth section of each
	// LR pair before the chain and the other after it, so empty chains multiply to the LR crossover and sum flat. It was
	// MEASURED −6.02 dB (LR4) and −6.0 to −7.8 dB (LR2) at the corners, filtering with the whole pair twice.
	std::vector<double> corners;
	for (size_t k = 0; k < 4; ++k) {
		corners.push_back(defaultCornerHz(k));
	}
	for (const pr::Slope slope : { pr::Slope::Lr4, pr::Slope::Lr2 }) {
		for (const int choice : { 0, 1, 2 }) {
			const int bands = bandsOfChoice(choice);
			for (int k = 0; k + 1 < bands; ++k) {
				CAPTURE(static_cast<int>(slope));
				CAPTURE(bands);
				CAPTURE(k);
				RackRig rig;
				bandsRig(rig, static_cast<size_t>(bands), choice, slope, pr::Filtering::Both);
				const double db = 20.0 * std::log10(gainAt(rig, corners[static_cast<size_t>(k)]));
				CAPTURE(db);
				CHECK(std::fabs(db) < 0.02);
			}
		}
	}
	// A SOLOED CHAIN UNDER BOTH IS ITS WHOLE LR BAND at its centre, as under Before: the halves multiply to it.
	for (const pr::Slope slope : { pr::Slope::Lr4, pr::Slope::Lr2 }) {
		const bool lr4 = slope == pr::Slope::Lr4;
		for (int k = 0; k < 5; ++k) {
			CAPTURE(static_cast<int>(slope));
			CAPTURE(k);
			RackRig rig;
			bandsRig(rig, 5, 2, slope, pr::Filtering::Both);
			rig.rack().setSoloed(static_cast<size_t>(k), true);
			rig.settle();
			const double centre = k == 0 ? corners[0] / 2.0 : (k == 4 ? corners[3] * 2.0 : std::sqrt(corners[static_cast<size_t>(k - 1)] * corners[static_cast<size_t>(k)]));
			CHECK(gainAt(rig, centre) == doctest::Approx(bandGain(lr4, 5, k, corners, centre)).epsilon(0.01));
		}
	}
}

TEST_CASE("Rack stage 2, Bands: at Mix 50% the dry runs through the bands' allpass, so nothing cancels at a corner — Before, After and Both give the input's magnitude") {
	std::vector<double> corners;
	for (size_t k = 0; k < 4; ++k) {
		corners.push_back(defaultCornerHz(k));
	}
	for (const pr::Slope slope : { pr::Slope::Lr4, pr::Slope::Lr2 }) {
		for (const pr::Filtering filtering : { pr::Filtering::Before, pr::Filtering::After, pr::Filtering::Both }) {
			for (size_t k = 0; k < 4; ++k) {
				CAPTURE(static_cast<int>(slope));
				CAPTURE(static_cast<int>(filtering));
				CAPTURE(k);
				RackRig rig;
				rig.set(pr::Param::Mix, 0.5f);
				bandsRig(rig, 5, 2, slope, filtering);
				CHECK(gainAt(rig, corners[k]) == doctest::Approx(1.0).epsilon(0.005)); // Both flat too since backlog OS
			}
		}
	}
}

TEST_CASE("Rack stage 2, Bands: the corners stay ORDERED however the cuts are modulated — cut 2 swept down through cut 1, read as the corners the engine runs at") {
	RackRig rig;
	bandsRig(rig, 5, 2, pr::Slope::Lr4, pr::Filtering::Before);
	const double third = std::exp2(1.0 / 3.0);
	rig.set(pr::Param::Cut1, static_cast<float>(std::log2(1000.0)));
	for (int step = 0; step <= 40; ++step) {
		// CUT 2 FROM 4 kHz DOWN TO 62.5 Hz, a block a step: a modulator crossing it over cut 1.
		const double cut2 = 4000.0 * std::exp2(-0.15 * static_cast<double>(step));
		CAPTURE(cut2);
		rig.set(pr::Param::Cut2, static_cast<float>(std::log2(cut2)));
		(void)rig.run(ramp());
		CHECK(rig.rack().cornerHzForTest(0) == doctest::Approx(1000.0).epsilon(1e-6));
		for (size_t k = 1; k < 4; ++k) {
			CHECK(rig.rack().cornerHzForTest(k) >= rig.rack().cornerHzForTest(k - 1) * third * (1.0 - 1e-9));
		}
		// BY VALUE: above the floor it is the cut; below, the floor.
		const double want2 = std::max(static_cast<double>(std::exp2(static_cast<double>(static_cast<float>(std::log2(cut2))))), 1000.0 * third);
		CHECK(rig.rack().cornerHzForTest(1) == doctest::Approx(want2).epsilon(1e-6));
	}
}

TEST_CASE("Rack stage 2, Bands: a corner's coefficients are recomputed once per sub-block and only when it moved") {
	RackRig rig;
	bandsRig(rig, 5, 2, pr::Slope::Lr4, pr::Filtering::Before);
	const uint64_t settled = rig.rack().coefficientUpdatesForTest();
	for (int b = 0; b < 10; ++b) {
		(void)rig.run(ramp());
	}
	CHECK(rig.rack().coefficientUpdatesForTest() == settled); // nothing moved: nothing recomputed
	rig.set(pr::Param::Cut3, static_cast<float>(std::log2(2500.0)));
	(void)rig.run(ramp());
	CHECK(rig.rack().coefficientUpdatesForTest() == settled + 1); // one corner moved: one recomputed
	(void)rig.run(ramp());
	CHECK(rig.rack().coefficientUpdatesForTest() == settled + 1);
}

TEST_CASE("Rack stage 2, Bands: the filters reach EXACT zero after a burst, and the rack then holds no tail — LR4 and LR2, every filtering") {
	for (const pr::Slope slope : { pr::Slope::Lr4, pr::Slope::Lr2 }) {
		for (const pr::Filtering filtering : { pr::Filtering::Before, pr::Filtering::After, pr::Filtering::Both }) {
			CAPTURE(static_cast<int>(slope));
			CAPTURE(static_cast<int>(filtering));
			RackRig rig;
			rig.set(pr::Param::Mix, 0.5f); // the dry's allpass too
			bandsRig(rig, 5, 2, slope, filtering);
			uint32_t seed = 5u;
			for (int b = 0; b < 8; ++b) {
				(void)rig.run(noise(kBlock, seed));
			}
			CHECK(pr::kAudioOps.holdsTail(rig.mem)); // ringing: awake
			int lastSounding = -1;
			for (int b = 0; b < 2000; ++b) {
				for (const float v : rig.run(std::vector<float>(kBlock, 0.0f))) {
					if (v != 0.0f) {
						lastSounding = b;
						break;
					}
				}
			}
			CAPTURE(lastSounding);
			CHECK(lastSounding < 1000);
			CHECK_FALSE(pr::kAudioOps.holdsTail(rig.mem));
		}
	}
}

TEST_CASE("Rack stage 2, Bands: a latent chain sets the latency, and every band — the empty ones too — lands on it to the sample") {
	RackRig rig;
	bandsRig(rig, 3, 1, pr::Slope::Lr4, pr::Filtering::Before);
	(void)rig.seat(1, kLateDesc, 100.0f);
	rig.settle(8);
	CHECK(pr::kAudioOps.latencySamples(rig.mem, rig.fields.data(), rig.fields.size(), kRate) == 100u);
	// A 2048-sample impulse response against Π AP of the impulse, 100 samples late. The filters are at rest: the
	// settle fed silence, and an impulse is all the input there is.
	std::vector<float> impulse(2048, 0.0f);
	impulse[0] = 1.0f;
	ox::AllpassChain<ox::Lr4Split, 5> ap;
	const std::vector<float> want = allpassOf(impulse, 3, ap);
	const std::vector<float> got = rig.run(impulse);
	float worst = 0.0f;
	for (size_t i = 0; i < got.size(); ++i) {
		worst = std::max(worst, std::fabs(got[i] - (i >= 100 ? want[i - 100] : 0.0f)));
	}
	CAPTURE(worst);
	CHECK(worst < 1.0e-5f); // float rounding of the band sum against the chain (MEASURED 1.0e-6)
	// AND IT IS THE IMPULSE'S ALLPASS, LATE: nothing at all before sample 100.
	for (size_t i = 0; i < 100; ++i) {
		REQUIRE(got[i] == 0.0f);
	}
}

//------------------------------------------------------------------------------------------------//
// STAGE 2, STEP 4: THE SPECTRUM STRIP'S ANALYSER (§7a)                                              //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE LOG BIN HOLDING `hz`, restated from the bin map (`dsp::analyserBinHz` gives each bin's centre).
size_t binOfHz(double hz) {
	size_t best = 0;
	for (size_t k = 1; k < dsp::kAnalyserBins; ++k) {
		if (std::fabs(std::log(dsp::analyserBinHz(k) / hz)) < std::fabs(std::log(dsp::analyserBinHz(best) / hz))) {
			best = k;
		}
	}
	return best;
}

/// A SINE OF `hz` FOR `seconds`, a block at a time, the view asking for readings before every block when `asked`.
void runSineAsked(RackRig& rig, double hz, double seconds, bool asked) {
	double phase = 0.0;
	for (int b = 0; b < static_cast<int>(seconds * kRate) / kBlock; ++b) {
		if (asked) {
			rig.rack().wantReadings();
		}
		std::vector<float> x(kBlock);
		for (float& v : x) {
			v = static_cast<float>(0.25 * std::sin(phase));
			phase += 2.0 * kPi * hz / kRate;
		}
		(void)rig.run(x);
	}
}

} // namespace

TEST_CASE("Rack stage 2 / §7a: the strip's analyser puts a sine in ITS log bin — read from the readings the rack publishes, PRE (the input) and POST (the output)") {
	RackRig rig;
	bandsRig(rig, 3, 1, pr::Slope::Lr4, pr::Filtering::Before);
	std::vector<float> published;
	pr::kAudioOps.setReadingsSink(rig.mem, device::ReadingsSink { &published, [](void* ctx, const float* v, size_t n) {
		                                                             static_cast<std::vector<float>*>(ctx)->assign(v, v + n);
	                                                             } });
	runSineAsked(rig, 1000.0, 1.5, true);
	REQUIRE(published.size() == pr::kReadingCount);
	CHECK(published[static_cast<size_t>(pr::Reading::FrameA)] > 0.0f);
	CHECK(published[static_cast<size_t>(pr::Reading::FrameA)] == published[static_cast<size_t>(pr::Reading::FrameB)]);
	const auto loudest = [&](pr::Reading first) {
		size_t best = 0;
		for (size_t k = 0; k < dsp::kAnalyserBins; ++k) {
			if (published[static_cast<size_t>(first) + k] > published[static_cast<size_t>(first) + best]) {
				best = k;
			}
		}
		return best;
	};
	CHECK(loudest(pr::Reading::PreFirst) == binOfHz(1000.0));
	CHECK(loudest(pr::Reading::PostFirst) == binOfHz(1000.0));
	// AN EMPTY BANDS RACK IS AN ALLPASS: POST'S LEVEL IN THE SINE'S BIN IS PRE'S.
	const size_t k = binOfHz(1000.0);
	CHECK(std::fabs(published[static_cast<size_t>(pr::Reading::PostFirst) + k] - published[static_cast<size_t>(pr::Reading::PreFirst) + k]) < 0.1f);
}

TEST_CASE("Rack stage 2 / §7a: the analyser runs ONLY while a view asks for readings — no view, no transforms; a view's lease lapses half a second after its last ask; never outside the strip's modes") {
	RackRig rig;
	bandsRig(rig, 3, 1, pr::Slope::Lr4, pr::Filtering::Before);
	runSineAsked(rig, 500.0, 2.0, false);
	CHECK(rig.rack().transformsForTest() == 0); // NO VIEW: not one FFT
	runSineAsked(rig, 500.0, 1.0, true);
	const int asked = rig.rack().transformsForTest();
	CHECK(asked > 0);
	runSineAsked(rig, 500.0, 2.0, false); // the last ask's half second, then nothing
	const int lapsed = rig.rack().transformsForTest();
	runSineAsked(rig, 500.0, 2.0, false);
	CHECK(rig.rack().transformsForTest() == lapsed); // stopped
	// PARALLEL HAS NO STRIP: asked or not, nothing.
	RackRig parallel;
	parallel.settle();
	runSineAsked(parallel, 500.0, 1.0, true);
	CHECK(parallel.rack().transformsForTest() == 0);
}

//------------------------------------------------------------------------------------------------//
// KEY TRACK: the Phaser's contract on the Bands corners                                            //
//------------------------------------------------------------------------------------------------//

namespace {

/// A NOTE AS THE HOST HANDS IT: held (`gate`) or released, at `pitch` semitones (A4 = 69).
void note(RackRig& rig, float pitch, bool held) {
	device::NoteContext n;
	n.pitchSemitones = pitch;
	n.gate = held;
	pr::kAudioOps.setNoteContext(rig.mem, n);
}

/// THE CORNER k KEY TRACK MUST PLACE, restated from the rule rather than from the engine: the note's own Hz,
/// 440·2^((pitch − 69)/12), moved by the cut's offset from cut 1's default (as the row stores it), clamped to
/// 20 Hz .. 20 kHz — then ordered a third of an octave apart.
std::array<double, 4> trackedRestated(RackRig& rig, double pitch) {
	const double noteHz = 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0);
	const double ref = static_cast<double>(static_cast<float>(pr::kParamDescs[static_cast<size_t>(pr::Param::Cut1)].def));
	std::array<double, 4> hz {};
	for (size_t k = 0; k < 4; ++k) {
		const double cut = static_cast<double>(rig.fields[static_cast<size_t>(pr::Param::Cut1) + k]);
		hz[k] = std::clamp(noteHz * std::pow(2.0, cut - ref), 20.0, 20000.0);
		if (k > 0) {
			hz[k] = std::max(hz[k], hz[k - 1] * std::pow(2.0, 1.0 / 3.0));
		}
	}
	return hz;
}

std::array<double, 4> untrackedRestated(RackRig& rig) {
	std::array<double, 4> hz {};
	for (size_t k = 0; k < 4; ++k) {
		const double cut = static_cast<double>(rig.fields[static_cast<size_t>(pr::Param::Cut1) + k]);
		hz[k] = std::pow(2.0, std::clamp(cut, std::log2(20.0), std::log2(20000.0)));
		if (k > 0) {
			hz[k] = std::max(hz[k], hz[k - 1] * std::pow(2.0, 1.0 / 3.0));
		}
	}
	return hz;
}

void requireCorners(RackRig& rig, const std::array<double, 4>& want) {
	for (size_t k = 0; k < 4; ++k) {
		CAPTURE(k);
		CHECK(rig.rack().cornerHzForTest(k) == doctest::Approx(want[k]).epsilon(1e-6));
	}
}

} // namespace

TEST_CASE("Rack Key Track: at a held note each corner sits on the note moved by its cut's offset — restated from the rule, at two notes and moved cuts") {
	RackRig rig;
	rig.set(pr::Param::KeyTrack, 1.0f);
	bandsRig(rig, 5, 2, pr::Slope::Lr4, pr::Filtering::Before);
	note(rig, 57.0f, true); // A3, 220 Hz
	rig.settle(40);         // past the 5 ms glide, landed (`kGlideSnap`)
	requireCorners(rig, trackedRestated(rig, 57.0));
	// CUT 1 AT ITS DEFAULT IS THE NOTE ITSELF.
	CHECK(rig.rack().cornerHzForTest(0) == doctest::Approx(220.0).epsilon(1e-9));
	// ANOTHER NOTE, AND MOVED CUTS: still the restatement.
	rig.set(pr::Param::Cut2, static_cast<float>(std::log2(900.0)));
	note(rig, 64.0f, true);
	rig.settle(40);
	requireCorners(rig, trackedRestated(rig, 64.0));
	// THE PUBLISHED NOTE: what the strip and the readouts ask.
	CHECK(rig.rack().readingsForTest()[static_cast<size_t>(pr::Reading::NoteHz)] == doctest::Approx(440.0 * std::pow(2.0, (64.0 - 69.0) / 12.0)).epsilon(1e-6));
}

TEST_CASE("Rack Key Track: no note yet, the knobs rule; a held note is LATCHED and holds after the release; a reset forgets it") {
	RackRig rig;
	rig.set(pr::Param::KeyTrack, 1.0f);
	bandsRig(rig, 5, 2, pr::Slope::Lr4, pr::Filtering::Before);
	requireCorners(rig, untrackedRestated(rig)); // no note yet
	CHECK(rig.rack().readingsForTest()[static_cast<size_t>(pr::Reading::NoteHz)] == 0.0f);
	note(rig, 60.0f, true);
	rig.settle(40);
	requireCorners(rig, trackedRestated(rig, 60.0));
	// RELEASED, AT ANOTHER PITCH (a bend in the release tail): ignored — the corners hold the held note's.
	note(rig, 72.0f, false);
	rig.settle(40);
	requireCorners(rig, trackedRestated(rig, 60.0));
	CHECK(rig.rack().noteSemitonesForTest() == 60.0);
	// A NEW HELD NOTE MOVES THEM.
	note(rig, 48.0f, true);
	rig.settle(40);
	requireCorners(rig, trackedRestated(rig, 48.0));
	// THE HOST'S RESET: no note again, the knobs rule.
	pr::kAudioOps.reset(rig.mem);
	rig.settle(2);
	CHECK_FALSE(rig.rack().heardNoteForTest());
	requireCorners(rig, untrackedRestated(rig));
}

TEST_CASE("Rack Key Track: Off is the rack before Key Track — notes change nothing, bit for bit, and the corners are the cuts") {
	RackRig quiet;
	RackRig noted;
	for (RackRig* r : { &quiet, &noted }) {
		bandsRig(*r, 5, 2, pr::Slope::Lr4, pr::Filtering::Both);
	}
	uint32_t seedA = 31u;
	uint32_t seedB = 31u;
	for (int b = 0; b < 16; ++b) {
		note(noted, 40.0f + static_cast<float>(b), b % 3 != 0);
		const std::vector<float> a = quiet.run(noise(kBlock, seedA));
		const std::vector<float> c = noted.run(noise(kBlock, seedB));
		for (size_t i = 0; i < a.size(); ++i) {
			REQUIRE(a[i] == c[i]);
		}
	}
	requireCorners(noted, untrackedRestated(noted));
	CHECK(noted.rack().readingsForTest()[static_cast<size_t>(pr::Reading::NoteHz)] == 0.0f);
}

TEST_CASE("Rack Key Track: the order clamp and the audible range hold on the TRACKED corners — a cut below its neighbour, and a note pushing the top past 20 kHz") {
	RackRig rig;
	rig.set(pr::Param::KeyTrack, 1.0f);
	bandsRig(rig, 5, 2, pr::Slope::Lr4, pr::Filtering::Before);
	// CUT 2 BELOW CUT 1: its tracked corner would sit under corner 1; the order lifts it a third of an octave above.
	rig.set(pr::Param::Cut2, static_cast<float>(std::log2(60.0)));
	note(rig, 57.0f, true);
	rig.settle(40);
	requireCorners(rig, trackedRestated(rig, 57.0));
	CHECK(rig.rack().cornerHzForTest(1) == doctest::Approx(220.0 * std::pow(2.0, 1.0 / 3.0)).epsilon(1e-9));
	// A HIGH NOTE: 6 kHz's offset over C8 lands past 20 kHz — clamped there, and the corners stay ordered and finite.
	rig.set(pr::Param::Cut2, static_cast<float>(pr::kParamDescs[static_cast<size_t>(pr::Param::Cut2)].def));
	note(rig, 108.0f, true);
	rig.settle(40);
	requireCorners(rig, trackedRestated(rig, 108.0));
	CHECK(rig.rack().cornerHzForTest(3) >= rig.rack().cornerHzForTest(2) * std::pow(2.0, 1.0 / 3.0) * (1.0 - 1e-9));
}

TEST_CASE("Rack Key Track: the switch is saved with the patch — into a fresh instance, and the loaded rack tracks the next note") {
	std::vector<uint8_t> body;
	DeviceInstanceId inner = kNoInstance;
	{
		RackRig rig;
		rack::RackBridge* outer = rig.rack().chainAt(0)->bridge();
		inner = outer->addDevice(pr::kTraits.tag);
		REQUIRE(inner != kNoInstance);
		outer->setField(inner, static_cast<size_t>(pr::Param::Mode), static_cast<float>(pr::Mode::Bands));
		outer->setField(inner, static_cast<size_t>(pr::Param::KeyTrack), 1.0f);
		rig.settle();
		body = rig.save();
	}
	RackRig fresh;
	REQUIRE(pr::kContainerOps.loadState(fresh.mem, body.data(), body.size()));
	rack::RackBridge* outer = fresh.rack().chainAt(0)->bridge();
	CHECK(outer->fieldAt(inner, static_cast<size_t>(pr::Param::KeyTrack)) == 1.0f);
	fresh.settle();
	rack::RackEngine& engine = fresh.rack().chainAt(0)->engine();
	void* storage = engine.slotAtLane(engine.slotOf(inner)).storage.get();
	REQUIRE(storage != nullptr);
	pr::Rack& loaded = *pr::rackOf(storage);
	CHECK(loaded.fieldForTest(pr::Param::KeyTrack) == 1.0f);
	// THE NOTE ROAD INTO THE INNER RACK IS THE ENGINE'S: a held note handed straight to it tracks.
	device::NoteContext n;
	n.pitchSemitones = 57.0f;
	n.gate = true;
	pr::kAudioOps.setNoteContext(storage, n);
	fresh.settle(40);
	CHECK(loaded.heardNoteForTest());
	CHECK(loaded.cornerHzForTest(0) == doctest::Approx(220.0).epsilon(1e-9));
}

//------------------------------------------------------------------------------------------------//
// NO ITEM 2: A CHAIN'S LEVEL, MUTE AND SOLO AS MODULATION DESTINATIONS                             //
//------------------------------------------------------------------------------------------------//

namespace {

/// A RACK HELD BY A HOST THAT MIRRORS: the outer rig's chain 0 is the holder (a `RackBridge`, whose gate carries
/// `setOwnField`), the Rack under test sits in it, and the holder's macros — the OUTER rack's Macro fields, handed to
/// every chain — are the sources routed into the slot params.
struct SlotRig {
	RackRig rig;
	rack::RackBridge* holder = nullptr;
	DeviceInstanceId innerId = kNoInstance;

	SlotRig() {
		holder = rig.rack().chainAt(0)->bridge();
		REQUIRE(holder != nullptr);
		innerId = holder->addDevice(pr::kTraits.tag);
		REQUIRE(innerId != kNoInstance);
		rig.settle();
		service();
	}
	/// After a load: the rig's own chain 0 holds the Rack at position 0.
	explicit SlotRig(const std::vector<uint8_t>& body) {
		REQUIRE(pr::kContainerOps.loadState(rig.mem, body.data(), body.size()));
		holder = rig.rack().chainAt(0)->bridge();
		REQUIRE(holder != nullptr);
		innerId = holder->deviceAtPosition(0);
		REQUIRE(innerId != kNoInstance);
		rig.settle();
		service();
	}
	pr::Rack& inner() {
		rack::RackEngine& e = rig.rack().chainAt(0)->engine();
		void* storage = e.slotAtLane(e.slotOf(innerId)).storage.get();
		REQUIRE(storage != nullptr);
		return *pr::rackOf(storage);
	}
	/// THE HOLDER'S SERVICE BEAT (the prune and the mirror), then blocks so the drains and the fades settle.
	void service() {
		(void)holder->service();
		rig.settle(8);
	}
	void macro(size_t n, float v) { rig.set(static_cast<pr::Param>(static_cast<size_t>(pr::Param::Macro1) + n), v); }
	backplane::Destination dest(uint8_t slot, pr::SlotControl c) const {
		return backplane::Destination::control(innerId, pr::slotParam(slot, c));
	}
	void route(size_t m, uint8_t slot, pr::SlotControl c, float amount) {
		holder->setRouteAmount(backplane::Source::core(m), dest(slot, c), amount);
	}
	float routed(size_t m, uint8_t slot, pr::SlotControl c) const {
		return holder->routeAmount(backplane::Source::core(m), dest(slot, c));
	}
	/// THE SLOT PARAM'S BASE, as the holder stores it.
	float base(uint8_t slot, pr::SlotControl c) const { return holder->fieldAt(innerId, pr::slotParam(slot, c)); }
	/// A Gain of `g` into the Rack's chain `chain`.
	void seat(size_t chain, float g) {
		rack::RackBridge* b = inner().chainAt(chain)->bridge();
		REQUIRE(b != nullptr);
		const DeviceInstanceId id = b->addDevice(kGainDesc.traits.tag);
		REQUIRE(id != kNoInstance);
		b->setField(id, 0, g);
	}
	/// One settled block of a constant `x`; the left channel's first sample (every sample is checked equal).
	float level(float x = 0.5f) {
		rig.settle(8);
		const std::vector<float> out = rig.run(std::vector<float>(kBlock, x));
		for (const float v : out) {
			REQUIRE(v == doctest::Approx(out[0]).epsilon(1e-6));
		}
		return out[0];
	}
};

constexpr float kUnity = static_cast<float>(composite::kFaderUnity);

/// THE RESOLVED LEVEL a route of `amount` at a source reading `v` gives, restated from the routing rule (base plus
/// amount × reading × the declaration's span, clamped to the range).
float routedLevel(float base, float amount, float v) {
	const device::ParamDesc& p = pr::kAllParamDescs[pr::slotParam(0, pr::SlotControl::Level)];
	return std::clamp(static_cast<float>(base + amount * v * device::routeSpanOf(p)), 0.0f, 1.0f);
}

} // namespace

TEST_CASE("NO item 2 (a): a routed chain level moves that chain's output by value — the slot param resolved, through the fader curve, the hand's level untouched") {
	SlotRig s;
	REQUIRE(s.inner().mirroredForTest());
	s.seat(0, 2.0f);
	const uint8_t slot = s.inner().slotOf(0);
	REQUIRE(slot == 0);
	CHECK(s.base(slot, pr::SlotControl::Level) == kUnity); // the mirror: the body's level
	CHECK(s.level() == doctest::Approx(0.5f * 2.0f * pr::faderGain(kUnity)).epsilon(1e-6));

	s.route(0, slot, pr::SlotControl::Level, -0.25f);
	s.macro(0, 1.0f);
	const float want = routedLevel(kUnity, -0.25f, 1.0f);
	REQUIRE(want < kUnity - 0.1f);
	const float heard = s.level();
	CHECK(s.inner().slotFieldForTest(static_cast<size_t>(slot) * pr::kSlotControls) == doctest::Approx(want).epsilon(1e-6));
	CHECK(heard == doctest::Approx(0.5f * 2.0f * pr::faderGain(want)).epsilon(1e-5));
	CHECK(s.inner().level(0) == kUnity); // the body is the hand's, not the route's
	// HALF THE READING, HALF THE REACH.
	s.macro(0, 0.5f);
	CHECK(s.level() == doctest::Approx(0.5f * 2.0f * pr::faderGain(routedLevel(kUnity, -0.25f, 0.5f))).epsilon(1e-5));
	s.macro(0, 0.0f);
	CHECK(s.level() == doctest::Approx(0.5f * 2.0f * pr::faderGain(kUnity)).epsilon(1e-6));
}

TEST_CASE("NO item 2 (a): a routed mute switches at half, and the switch is the chain's 5 ms fade read sample by sample, not a step") {
	SlotRig s;
	s.seat(0, 2.0f);
	REQUIRE(s.inner().addChain() == 1);
	s.seat(1, 3.0f);
	s.rig.settle(8);
	const uint8_t slot = s.inner().slotOf(0);
	s.route(1, slot, pr::SlotControl::Mute, 1.0f);
	const float both = 0.5f * (2.0f + 3.0f);
	const float alone = 0.5f * 3.0f;
	s.macro(1, 0.45f);
	CHECK(s.level() == doctest::Approx(both).epsilon(1e-6)); // below half: not muted
	s.macro(1, 0.55f);
	// THE SWITCH, READ SAMPLE BY SAMPLE across the blocks it lands in: from `both` to `alone` in steps no larger than a
	// 5 ms (240-sample) ramp's, through more than 200 samples strictly between.
	std::vector<float> trace { both }; // the settled sample before the switch, asserted just above
	for (int b = 0; b < 4; ++b) {
		const std::vector<float> out = s.rig.run(std::vector<float>(kBlock, 0.5f));
		trace.insert(trace.end(), out.begin(), out.end());
	}
	const float step = (both - alone) / static_cast<float>(kRamp);
	float worst = 0.0f;
	int between = 0;
	for (size_t i = 1; i < trace.size(); ++i) {
		worst = std::max(worst, std::abs(trace[i] - trace[i - 1]));
		between += trace[i] < both - 1e-5f && trace[i] > alone + 1e-5f ? 1 : 0;
	}
	CHECK(trace[1] < both); // the fade starts in the block the route crosses half…
	CHECK(trace[1] > both - step * 1.05f); // …one ramp step down, not a jump
	CHECK(trace.back() == doctest::Approx(alone).epsilon(1e-6));
	CHECK(worst <= step * 1.05f);
	CHECK(between > 200);
	CHECK_FALSE(s.inner().muted(0)); // the hand's mute is still off
	s.macro(1, 0.0f);
	CHECK(s.level() == doctest::Approx(both).epsilon(1e-6));
}

TEST_CASE("NO item 2 (a): a routed solo switches at half and ORs with a hand's solo — across two chains, and a route below a hand's solo cannot unsolo it") {
	SlotRig s;
	s.seat(0, 2.0f);
	REQUIRE(s.inner().addChain() == 1);
	s.seat(1, 3.0f);
	const uint8_t a = s.inner().slotOf(0);
	const uint8_t b = s.inner().slotOf(1);
	REQUIRE(a != b);
	s.inner().setSoloed(1, true); // THE HAND solos chain 1
	CHECK(s.base(b, pr::SlotControl::Solo) == 1.0f);
	CHECK(s.level() == doctest::Approx(0.5f * 3.0f).epsilon(1e-6));
	s.route(2, a, pr::SlotControl::Solo, 1.0f);
	s.macro(2, 0.45f);
	CHECK(s.level() == doctest::Approx(0.5f * 3.0f).epsilon(1e-6)); // below half: chain 0 is not soloed
	s.macro(2, 0.55f);
	CHECK(s.level() == doctest::Approx(0.5f * 5.0f).epsilon(1e-6)); // both soloed: the route ORs with the hand
	// A ROUTE PULLING CHAIN 1'S SOLO TO ZERO: the hand's solo holds.
	s.route(3, b, pr::SlotControl::Solo, -1.0f);
	s.macro(3, 1.0f);
	CHECK(s.level() == doctest::Approx(0.5f * 5.0f).epsilon(1e-6));
	s.macro(2, 0.0f);
	CHECK(s.level() == doctest::Approx(0.5f * 3.0f).epsilon(1e-6));
	// THE HAND LETS GO: only the route solos, and it solos chain 0 alone.
	s.inner().setSoloed(1, false);
	s.macro(2, 1.0f);
	CHECK(s.level() == doctest::Approx(0.5f * 2.0f).epsilon(1e-6));
}

TEST_CASE("NO item 2 (b): removing and putting back OTHER chains never moves a route — it stays on its chain's slot, by value") {
	SlotRig s;
	s.seat(0, 2.0f);
	REQUIRE(s.inner().addChain() == 1);
	s.seat(1, 3.0f);
	REQUIRE(s.inner().addChain() == 2);
	s.seat(2, 5.0f);
	const uint16_t idA = s.inner().chainIdAt(0);
	const uint16_t idB = s.inner().chainIdAt(1);
	const uint16_t idC = s.inner().chainIdAt(2);
	const uint8_t slotB = s.inner().slotOf(1);
	s.route(0, slotB, pr::SlotControl::Level, -0.5f);
	s.macro(0, 1.0f);
	const float g = pr::faderGain(routedLevel(kUnity, -0.5f, 1.0f));
	REQUIRE(g < 0.9f);
	const auto want = [&](bool a, bool c) { return 0.5f * ((a ? 2.0f : 0.0f) + 3.0f * g + (c ? 5.0f : 0.0f)); };
	CHECK(s.level() == doctest::Approx(want(true, true)).epsilon(1e-5));

	// A, the chain BEFORE B, removed: B is at index 0 now and keeps its slot and its route.
	const pr::Rack::ChainSnapshot snapA = s.inner().snapshotChain(0);
	REQUIRE(s.inner().removeChain(0));
	s.service();
	REQUIRE(s.inner().indexOfChain(idB) == 0);
	CHECK(s.inner().slotOf(0) == slotB);
	CHECK(s.routed(0, slotB, pr::SlotControl::Level) == -0.5f);
	CHECK(s.level() == doctest::Approx(want(false, true)).epsilon(1e-5));

	// A PUT BACK FIRST and C moved to the front: B is at index 2 and still the routed one.
	REQUIRE(s.inner().insertChain(0, snapA) == 0);
	const pr::Rack::ChainSnapshot snapC = s.inner().snapshotChain(s.inner().indexOfChain(idC));
	REQUIRE(s.inner().removeChain(s.inner().indexOfChain(idC)));
	s.service();
	REQUIRE(s.inner().insertChain(0, snapC) == 0);
	s.service();
	REQUIRE(s.inner().indexOfChain(idA) == 1);
	REQUIRE(s.inner().indexOfChain(idB) == 2);
	CHECK(s.inner().slotOf(2) == slotB);
	CHECK(s.routed(0, slotB, pr::SlotControl::Level) == -0.5f);
	CHECK(s.level() == doctest::Approx(want(true, true)).epsilon(1e-5));
	s.macro(0, 0.0f);
	CHECK(s.level() == doctest::Approx(0.5f * 10.0f).epsilon(1e-5)); // and the route is what moved it
}

TEST_CASE("NO item 2 (c): a removed chain's slot is not reused until the host has pruned its routes — the chain that takes it inherits nothing") {
	SlotRig s;
	s.seat(0, 2.0f);
	REQUIRE(s.inner().addChain() == 1);
	s.seat(1, 3.0f);
	const uint8_t freed = s.inner().slotOf(1);
	s.route(0, freed, pr::SlotControl::Level, -0.5f);
	s.route(1, freed, pr::SlotControl::Mute, 1.0f);
	s.macro(0, 1.0f);
	s.macro(1, 1.0f);
	CHECK(s.level() == doctest::Approx(0.5f * 2.0f).epsilon(1e-5)); // the routed chain is muted by its route

	// REMOVED, AND A CHAIN ADDED BEFORE THE HOST'S SERVICE: the freed slot is still awaiting its prune.
	REQUIRE(s.inner().removeChain(1));
	REQUIRE(s.inner().addChain() == 1);
	s.seat(1, 5.0f);
	CHECK(s.inner().slotOf(1) != freed);
	CHECK(s.level() == doctest::Approx(0.5f * (2.0f + 5.0f)).epsilon(1e-5)); // no route reaches it

	// THE SERVICE PRUNES, and the next chain takes the freed slot — with nothing routed into it.
	s.service();
	CHECK(s.routed(0, freed, pr::SlotControl::Level) == 0.0f);
	CHECK(s.routed(1, freed, pr::SlotControl::Mute) == 0.0f);
	REQUIRE(s.inner().addChain() == 2);
	s.seat(2, 7.0f);
	CHECK(s.inner().slotOf(2) == freed);
	CHECK(s.base(freed, pr::SlotControl::Level) == kUnity); // its base put to a fresh chain's
	CHECK(s.base(freed, pr::SlotControl::Mute) == 0.0f);
	CHECK(s.level() == doctest::Approx(0.5f * (2.0f + 5.0f + 7.0f)).epsilon(1e-5));
}

TEST_CASE("NO item 2 (d): one source of truth — each slot param's base equals its chain's body after a hand edit, a put-back and a preset applied") {
	SlotRig s;
	REQUIRE(s.inner().addChain() == 1);
	const auto agrees = [&](size_t i) {
		const uint8_t slot = s.inner().slotOf(i);
		REQUIRE(slot < pr::kChainSlots);
		CHECK(s.base(slot, pr::SlotControl::Level) == s.inner().level(i));
		CHECK(s.base(slot, pr::SlotControl::Mute) == (s.inner().muted(i) ? 1.0f : 0.0f));
		CHECK(s.base(slot, pr::SlotControl::Solo) == (s.inner().soloed(i) ? 1.0f : 0.0f));
	};
	// A HAND'S EDIT, at once (no service beat between).
	s.inner().setLevel(1, 0.3f);
	s.inner().setMuted(1, true);
	s.inner().setSoloed(0, true);
	CHECK(s.base(s.inner().slotOf(1), pr::SlotControl::Level) == 0.3f);
	agrees(0);
	agrees(1);
	// A PUT-BACK (an undone removal): the chain comes back with its values, and so does its slot's base.
	const pr::Rack::ChainSnapshot snap = s.inner().snapshotChain(1);
	REQUIRE(s.inner().removeChain(1));
	s.service();
	REQUIRE(s.inner().insertChain(1, snap) == 1);
	CHECK(s.inner().level(1) == 0.3f);
	agrees(1);
	// A PRESET APPLIED INTO THE RUNNING RACK: another rack's body, its chains' values unlike these; mirrored at the
	// service beat, and the body is what is heard until then.
	SlotRig other;
	REQUIRE(other.inner().addChain() == 1);
	REQUIRE(other.inner().addChain() == 2);
	other.inner().setLevel(0, 0.1f);
	other.inner().setMuted(2, true);
	other.inner().setSoloed(1, true);
	std::vector<uint8_t> body;
	const device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                             out->insert(out->end(), data, data + bytes);
	                             } };
	REQUIRE(other.inner().saveState(sink));
	REQUIRE(s.inner().applyState(body.data(), body.size()));
	CHECK_FALSE(s.inner().mirroredForTest());
	s.service();
	CHECK(s.inner().mirroredForTest());
	REQUIRE(s.inner().chainCount() == 3);
	for (size_t i = 0; i < 3; ++i) {
		CAPTURE(i);
		agrees(i);
	}
}

TEST_CASE("NO item 2 (e): save, then load into a FRESH host — each chain keeps its slot, and the routes into it work by value") {
	std::vector<uint8_t> body;
	uint8_t slotC = pr::kNoChainSlot;
	{
		SlotRig s;
		s.seat(0, 2.0f);
		REQUIRE(s.inner().addChain() == 1);
		s.seat(1, 3.0f);
		REQUIRE(s.inner().addChain() == 2);
		s.seat(2, 5.0f);
		REQUIRE(s.inner().removeChain(0)); // a gap: the slots no longer match the positions
		s.service();
		slotC = s.inner().slotOf(1);
		REQUIRE(slotC == 2);
		s.route(0, slotC, pr::SlotControl::Level, -0.5f);
		s.route(1, s.inner().slotOf(0), pr::SlotControl::Mute, 1.0f);
		body = s.rig.save();
	}
	SlotRig fresh(body);
	REQUIRE(fresh.inner().chainCount() == 2);
	CHECK(fresh.inner().slotOf(0) == 1);
	CHECK(fresh.inner().slotOf(1) == slotC);
	CHECK(fresh.routed(0, slotC, pr::SlotControl::Level) == -0.5f);
	REQUIRE(fresh.inner().mirroredForTest());
	CHECK(fresh.level() == doctest::Approx(0.5f * (3.0f + 5.0f)).epsilon(1e-5));
	fresh.macro(0, 1.0f);
	const float g = pr::faderGain(routedLevel(kUnity, -0.5f, 1.0f));
	CHECK(fresh.level() == doctest::Approx(0.5f * (3.0f + 5.0f * g)).epsilon(1e-5));
	fresh.macro(1, 1.0f);
	CHECK(fresh.level() == doctest::Approx(0.5f * 5.0f * g).epsilon(1e-5));
}

TEST_CASE("NO item 2: every chain slot param of every slot changes the sound, by value — level, mute and solo, each routed in turn") {
	// **THE HONESTY SWEEP'S HALF FOR THE SLOTS** (test_fx_devices' `sweptControls`): there a bare rack hears its body,
	// so the slots are swept here, in a holder that mirrors. Chain i holds a Gain of (i + 1) / 8 (the probe's range is
	// 0..8, and there are `kChainSlots` of them), so each chain's share of
	// the sum is its own number, and each route's effect is restated from it.
	// 64 SINCE THE FIELD CAP WENT 160 -> 256: chains 33..64 by value below.
	CHECK(pr::kChainSlots == 64);
	SlotRig s;
	while (s.inner().chainCount() < pr::kChainSlots) {
		REQUIRE(s.inner().addChain() != SIZE_MAX);
	}
	float sum = 0.0f;
	for (size_t i = 0; i < pr::kChainSlots; ++i) {
		s.seat(i, 0.125f * static_cast<float>(i + 1));
		sum += 0.125f * static_cast<float>(i + 1);
	}
	const float x = 0.01f; // small, so the sum stays well inside a float's exact range
	CHECK(s.level(x) == doctest::Approx(x * sum).epsilon(1e-5));
	s.macro(0, 1.0f);
	const float g = pr::faderGain(routedLevel(kUnity, -0.5f, 1.0f));
	int heard = 0;
	for (size_t i = 0; i < pr::kChainSlots; ++i) {
		CAPTURE(i);
		const uint8_t slot = s.inner().slotOf(i);
		REQUIRE(slot == i); // a fresh rack's chains take the slots in order
		const float own = 0.125f * static_cast<float>(i + 1);
		const std::pair<pr::SlotControl, float> each[] = {
			{ pr::SlotControl::Level, x * (sum - own + own * g) },
			{ pr::SlotControl::Mute, x * (sum - own) },
			{ pr::SlotControl::Solo, x * own },
		};
		for (const auto& [which, want] : each) {
			CAPTURE(static_cast<int>(which));
			s.route(0, slot, which, which == pr::SlotControl::Level ? -0.5f : 1.0f);
			const float got = s.level(x);
			CHECK(got == doctest::Approx(want).epsilon(1e-5));
			heard += got != doctest::Approx(x * sum).epsilon(1e-6) ? 1 : 0;
			s.route(0, slot, which, 0.0f); // removed
		}
	}
	CHECK(heard == static_cast<int>(pr::kChainSlots * pr::kSlotControls)); // every one moved the sound
	CHECK(s.level(x) == doctest::Approx(x * sum).epsilon(1e-5));
}

TEST_CASE("NO item 2 follow-up: a base the host moved is adopted into the body at the service beat — the output, the save and the mirror follow — and never while a mirror is owed") {
	SlotRig s;
	s.seat(0, 2.0f);
	const uint8_t slot = s.inner().slotOf(0);
	// THE HOLDER'S RECORD WRITE, as the slot owner's visit makes it for a DAW on the slot param.
	s.holder->setField(s.innerId, pr::slotParam(slot, pr::SlotControl::Level), 0.2f);
	s.holder->setField(s.innerId, pr::slotParam(slot, pr::SlotControl::Solo), 1.0f);
	s.service();
	CHECK(s.inner().level(0) == 0.2f);
	CHECK(s.inner().soloed(0));
	CHECK(s.level() == doctest::Approx(0.5f * 2.0f * pr::faderGain(0.2f)).epsilon(1e-5));
	// THE SAVE CARRIES IT: a fresh rack loaded from the body plays the adopted level.
	SlotRig fresh(s.rig.save());
	CHECK(fresh.inner().level(0) == 0.2f);
	CHECK(fresh.inner().soloed(0));
	// WHILE A MIRROR IS OWED (a preset applied), the bases are stale and the body wins: nothing adopted.
	std::vector<uint8_t> body;
	const device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                             auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                             out->insert(out->end(), data, data + bytes);
	                             } };
	SlotRig other;
	other.inner().setLevel(0, 0.9f);
	REQUIRE(other.inner().saveState(sink));
	REQUIRE(s.inner().applyState(body.data(), body.size()));
	REQUIRE_FALSE(s.inner().mirroredForTest());
	CHECK_FALSE(s.inner().adoptOwnFields(nullptr, 0));
	s.service();
	CHECK(s.inner().level(0) == 0.9f);
	CHECK(s.base(s.inner().slotOf(0), pr::SlotControl::Level) == 0.9f);
}
