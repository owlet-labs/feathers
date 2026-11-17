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

// A RACK IN SERIES: chain 1's output feeds chain 2's input, and the last chain's is the wet. Every claim is a value against an independent
// restatement of the arithmetic (the test devices are exact: a gain, a square-law shaper, a pure delay), never "the output changed".

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "backplane/block_context.h"
#include "persist_rig.h"
#include "plugin/device_registry.h"
#include "rack/parallel_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {
std::atomic<bool> g_countAllocs { false };
std::atomic<uint64_t> g_allocs { 0 };
struct AllocWindow {
	AllocWindow() {
		g_allocs.store(0);
		g_countAllocs.store(true);
	}
	~AllocWindow() { g_countAllocs.store(false); }
	[[nodiscard]] uint64_t allocs() const { return g_allocs.load(); }
};
void noteAlloc() noexcept {
	if (g_countAllocs.load(std::memory_order_relaxed)) {
		g_allocs.fetch_add(1, std::memory_order_relaxed);
	}
}
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
[[gnu::noinline]] void* operator new(std::size_t n) {
	noteAlloc();
	void* p = feathers::test::heapAlloc(n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n) { return operator new(n); }
[[gnu::noinline]] void* operator new(std::size_t n, std::align_val_t al) {
	noteAlloc();
	void* p = feathers::test::heapAlignedAlloc(static_cast<std::size_t>(al), n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
[[gnu::noinline]] void* operator new[](std::size_t n, std::align_val_t al) { return operator new(n, al); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#pragma GCC diagnostic pop

using namespace feathers;
namespace par = feathers::rack::parallel;
using NoteEvent = device::NoteEvent;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

//--------------------------------------------------------------------------------------------------//
// THE TEST DEVICES: a gain of one half, a square-law shaper y = x|x|, and a pure delay that says how long it is.                    //
//--------------------------------------------------------------------------------------------------//

struct Nothing {};
struct Delay {
	std::vector<float> l, r;
	size_t write = 0;
	uint32_t n = 0;
	uint64_t since = 1u << 30; ///< samples since the last that was not silence went in
};
constexpr size_t kRing = 1u << 12;
Delay& delayAt(void* s) { return *static_cast<Delay*>(s); }

const device::AudioOps kGainOps {
    .stateBytes = sizeof(Nothing), .stateAlign = alignof(Nothing),
    .construct = [](void* s) { new (s) Nothing {}; }, .destruct = [](void*) {},
    .prepare = [](void*, double, int) {}, .reset = [](void*) {},
    .setFields = [](void*, const float*, size_t) {},
    .processStereo = [](void*, float* l, float* r, int n) {
	    for (int i = 0; i < n; ++i) {
		    l[i] *= 0.5f;
		    r[i] *= 0.5f;
	    }
    },
};
const device::AudioOps kShapeOps {
    .stateBytes = sizeof(Nothing), .stateAlign = alignof(Nothing),
    .construct = [](void* s) { new (s) Nothing {}; }, .destruct = [](void*) {},
    .prepare = [](void*, double, int) {}, .reset = [](void*) {},
    .setFields = [](void*, const float*, size_t) {},
    .processStereo = [](void*, float* l, float* r, int n) {
	    for (int i = 0; i < n; ++i) {
		    l[i] = l[i] * std::fabs(l[i]);
		    r[i] = r[i] * std::fabs(r[i]);
	    }
    },
};
const device::AudioOps kDelayOps {
    .stateBytes = sizeof(Delay), .stateAlign = alignof(Delay),
    .construct = [](void* s) { new (s) Delay {}; }, .destruct = [](void* s) { static_cast<Delay*>(s)->~Delay(); },
    .prepare = [](void* s, double, int) {
	    delayAt(s).l.assign(kRing, 0.0f);
	    delayAt(s).r.assign(kRing, 0.0f);
    },
    .reset = [](void* s) {
	    std::fill(delayAt(s).l.begin(), delayAt(s).l.end(), 0.0f);
	    std::fill(delayAt(s).r.begin(), delayAt(s).r.end(), 0.0f);
    },
    .setFields = [](void* s, const float* f, size_t n) {
	    delayAt(s).n = n > 0 && f != nullptr && f[0] > 0.0f ? static_cast<uint32_t>(std::lround(std::min(f[0], 2000.0f))) : 0;
    },
    .processStereo = [](void* s, float* l, float* r, int frames) {
	    Delay& d = delayAt(s);
	    for (int i = 0; i < frames; ++i) {
		    d.since = l[i] != 0.0f || r[i] != 0.0f ? 0 : d.since + 1;
		    d.l[d.write] = l[i];
		    d.r[d.write] = r[i];
		    l[i] = d.l[(d.write - d.n) & (kRing - 1)];
		    r[i] = d.r[(d.write - d.n) & (kRing - 1)];
		    d.write = (d.write + 1) & (kRing - 1);
	    }
    },
    .latencySamples = [](const void*, const float* f, size_t n, double) -> uint32_t {
	    return n > 0 && f != nullptr && f[0] > 0.0f ? static_cast<uint32_t>(std::lround(std::min(f[0], 2000.0f))) : 0u;
    },
    .holdsTail = [](const void* s) { return static_cast<const Delay*>(s)->since <= static_cast<const Delay*>(s)->n; },
};
constexpr std::array<device::ParamDesc, 1> kDelayParams { { { "latency", "Latency", 0.0, 2000.0, 0.0, true, device::Unit::Number, {} } } };
const device::Descriptor kGain { .traits = { .tag = "KGAIN", .name = "Half", .hasCard = false, .hasExpanded = false },
	                             .params = {}, .outputs = {}, .audio = &kGainOps };
const device::Descriptor kShape { .traits = { .tag = "KSHAPE", .name = "Shape", .hasCard = false, .hasExpanded = false },
	                              .params = {}, .outputs = {}, .audio = &kShapeOps };
const device::Descriptor kDelay { .traits = { .tag = "KDELAY", .name = "Lag", .hasCard = false, .hasExpanded = false },
	                              .params = kDelayParams, .outputs = {}, .audio = &kDelayOps };

// A NOTE RECORDER, as in the chain-key suite.
struct Rec {
	std::vector<NoteEvent> seen;
};
std::vector<Rec*>& recs() {
	static std::vector<Rec*> all;
	return all;
}
const device::AudioOps kRecAudio {
    .stateBytes = sizeof(Rec), .stateAlign = alignof(Rec),
    .construct = [](void* s) { recs().push_back(new (s) Rec()); },
    .destruct = [](void* s) {
	    auto* p = static_cast<Rec*>(s);
	    recs().erase(std::remove(recs().begin(), recs().end(), p), recs().end());
	    p->~Rec();
    },
    .prepare = [](void*, double, int) {}, .reset = [](void*) {},
    .setFields = [](void*, const float*, size_t) {},
    .processStereo = [](void*, float*, float*, int) {},
};
const device::NoteOps kRecNotes {
    .stateBytes = sizeof(Rec), .stateAlign = alignof(Rec),
    .construct = kRecAudio.construct,
    .destruct = kRecAudio.destruct,
    .prepare = [](void*, double) {}, .setFields = [](void*, const float*, size_t) {},
    .processNotes = [](void* s, const NoteEvent* in, size_t n, NoteEvent* out, size_t cap, size_t* made) {
	    auto* p = static_cast<Rec*>(s);
	    const size_t take = std::min(n, cap);
	    for (size_t i = 0; i < take; ++i) {
		    p->seen.push_back(in[i]);
		    out[i] = in[i];
	    }
	    *made = take;
    },
};
const device::Descriptor kRec { .traits = { .tag = "KREC", .name = "Rec", .hasCard = false, .hasExpanded = false },
	                            .params = {}, .outputs = {}, .audio = &kRecAudio, .notes = &kRecNotes };

const device::Descriptor* findTest(void*, const char* tag) {
	for (const device::Descriptor* d : { &kGain, &kShape, &kDelay, &kRec }) {
		if (device::sameTag(d->traits.tag, tag)) {
			return d;
		}
	}
	return deviceForTag(tag);
}
device::Catalogue testCatalogue() {
	return device::Catalogue { nullptr, &findTest };
}

float tone(size_t i) {
	return 0.9f * std::sin(0.031f * static_cast<float>(i)) + 0.2f * std::sin(0.17f * static_cast<float>(i) + 1.0f);
}

/// A RACK OF `chains` CHAINS in the chain plugin's shape, its audio fed the tone block by block.
struct Series {
	rack::RackEngine engine { rack::kRackAxes, testCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	DeviceInstanceId rackId = kNoInstance;
	par::Rack* rk = nullptr;
	std::vector<float> l = std::vector<float>(kBlock, 0.0f);
	std::vector<float> r = std::vector<float>(kBlock, 0.0f);
	size_t clock = 0;

	explicit Series(size_t chains, par::Mode mode = par::Mode::Series) {
		engine.prepare(kRate, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
		rackId = top->addDevice(par::kTraits.tag);
		REQUIRE(rackId != kNoInstance);
		block();
		block();
		rk = par::rackOf(top->containerSurfaceOf(rackId));
		REQUIRE(rk != nullptr);
		while (rk->chainCount() < chains) {
			(void)rk->addChain();
			block();
			block();
		}
		setMode(mode);
	}
	DeviceInstanceId add(size_t chain, const char* tag, float latency = 0.0f) {
		rack::RackBridge& b = *rk->chainAt(chain)->bridge();
		const DeviceInstanceId id = b.addDevice(tag);
		REQUIRE(id != kNoInstance);
		block();
		if (latency > 0.0f) {
			b.setField(id, 0, latency);
			block();
			block();
		}
		return id;
	}
	void setMode(par::Mode m) {
		top->setField(rackId, static_cast<size_t>(par::Param::Mode), static_cast<float>(m));
		for (int i = 0; i < 12; ++i) {
			block(); // the mode's fade and its landing
		}
	}
	/// One block of the tone through the rack, written to `outL`/`inL` when asked.
	void block(std::vector<float>* outL = nullptr, std::vector<float>* inL = nullptr, bool silence = false,
	           const std::vector<NoteEvent>& notes = {}) {
		for (int i = 0; i < kBlock; ++i) {
			l[static_cast<size_t>(i)] = r[static_cast<size_t>(i)] = silence ? 0.0f : tone(clock + static_cast<size_t>(i));
		}
		if (inL != nullptr) {
			inL->insert(inL->end(), l.begin(), l.end());
		}
		top->drain();
		engine.setBlockNotes(notes.data(), notes.size());
		engine.processStereo(l.data(), r.data(), kBlock, backplane::unknownBlock());
		top->publish(engine);
		top->reapRetired();
		clock += kBlock;
		if (outL != nullptr) {
			outL->insert(outL->end(), l.begin(), l.end());
		}
	}
	/// `blocks` settled blocks of audio: what came out, and what went in.
	std::pair<std::vector<float>, std::vector<float>> run(int blocks) {
		std::pair<std::vector<float>, std::vector<float>> io;
		for (int i = 0; i < blocks; ++i) {
			block(&io.first, &io.second);
		}
		return io;
	}
};

float half(float x) { return 0.5f * x; }
float shape(float x) { return x * std::fabs(x); }
float fader(float position) { return (2.0f * position) * (2.0f * position); }

bool closeTo(const std::vector<float>& got, const std::vector<float>& want, float rel = 1.0e-6f) {
	if (got.size() != want.size()) {
		return false;
	}
	for (size_t i = 0; i < got.size(); ++i) {
		if (std::fabs(got[i] - want[i]) > rel * (1.0f + std::fabs(want[i]))) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("series: two gain devices in series are the product; the same chains in parallel are the sum") {
	Series s(2);
	s.add(0, "KGAIN");
	s.add(1, "KGAIN");
	const auto [out, in] = s.run(6);
	// HALF OF HALF: a quarter of the input. Independent restatement; a parallel rack would sound 0.5 + 0.5 = the input.
	std::vector<float> want(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		want[i] = half(half(in[i]));
	}
	CHECK(closeTo(out, want));
	s.setMode(par::Mode::Parallel);
	const auto [pout, pin] = s.run(6);
	CHECK(closeTo(pout, pin)); // 0.5 x + 0.5 x
	CHECK_FALSE(closeTo(pout, std::vector<float>(pin.size(), 0.0f)));
}

TEST_CASE("series: order matters with a nonlinear device — the shaper then the half is not the half then the shaper") {
	const auto render = [&](bool shaperFirst) {
		Series s(2);
		s.add(shaperFirst ? 0 : 1, "KSHAPE");
		s.add(shaperFirst ? 1 : 0, "KGAIN");
		return s.run(6);
	};
	const auto [shapeThenHalf, in1] = render(true);
	const auto [halfThenShape, in2] = render(false);
	std::vector<float> a(in1.size()), b(in1.size());
	for (size_t i = 0; i < in1.size(); ++i) {
		a[i] = half(shape(in1[i]));
		b[i] = shape(half(in2[i]));
	}
	CHECK(closeTo(shapeThenHalf, a));
	CHECK(closeTo(halfThenShape, b));
	CHECK_FALSE(closeTo(a, b)); // the two orders are different sounds
}

TEST_CASE("series: a chain's level applies to its own output, an empty chain passes the signal through, a muted chain is bypassed") {
	Series s(3);
	s.add(0, "KGAIN");
	// chain 1 empty, chain 2 the shaper.
	s.add(2, "KSHAPE");
	s.rk->setLevel(0, 0.25f); // fader (2*0.25)^2 = 0.25
	for (int i = 0; i < 4; ++i) {
		s.block();
	}
	{
		const auto [out, in] = s.run(5);
		std::vector<float> want(in.size());
		for (size_t i = 0; i < in.size(); ++i) {
			want[i] = shape(fader(0.25f) * half(in[i])); // gain device, then its level, the empty chain, the shaper
		}
		CHECK(closeTo(out, want));
	}
	// MUTE chain 0: it is bypassed — the signal goes round it (and its level), not into silence.
	s.rk->setMuted(0, true);
	for (int i = 0; i < 6; ++i) {
		s.block(); // past the 5 ms ramp
	}
	{
		const auto [out, in] = s.run(5);
		std::vector<float> want(in.size());
		for (size_t i = 0; i < in.size(); ++i) {
			want[i] = shape(in[i]);
		}
		CHECK(closeTo(out, want));
	}
	// SOLO chain 2: the others pass through (chain 0 stays muted, chain 1 is bypassed by the solo): the shaper alone.
	s.rk->setMuted(0, false);
	s.rk->setSoloed(2, true);
	for (int i = 0; i < 6; ++i) {
		s.block();
	}
	{
		const auto [out, in] = s.run(5);
		std::vector<float> want(in.size());
		for (size_t i = 0; i < in.size(); ++i) {
			want[i] = shape(in[i]);
		}
		CHECK(closeTo(out, want));
	}
	// EVERY CHAIN LEFT OUT (the solo moved to the empty one): the rack is the input.
	s.rk->setSoloed(2, false);
	s.rk->setSoloed(1, true);
	for (int i = 0; i < 6; ++i) {
		s.block();
	}
	const auto [out, in] = s.run(5);
	CHECK(closeTo(out, in));
}

TEST_CASE("series: the rack's latency is the sum of its chains' (parallel's is the largest), and the signal arrives that late whether a chain is bypassed or not") {
	Series s(2);
	s.add(0, "KDELAY", 64.0f);
	s.add(1, "KDELAY", 96.0f);
	for (int i = 0; i < 3; ++i) {
		s.block();
	}
	CHECK(s.rk->latencySamples(kRate) == 160u);
	const auto delayed = [&](const std::vector<float>& in, const std::vector<float>& out, size_t by) {
		// out[i] must be the input `by` samples earlier, from where the delay lines are full.
		for (size_t i = by + kBlock; i < out.size(); ++i) {
			if (out[i] != in[i - by] && std::fabs(out[i] - in[i - by]) > 1.0e-6f) {
				return false;
			}
		}
		return true;
	};
	{
		const auto [out, in] = s.run(8);
		CHECK(delayed(in, out, 160));
		// NON-VACUITY: it is not 96, the parallel latency.
		CHECK_FALSE(delayed(in, out, 96));
	}
	// BYPASS CHAIN 0: the reported latency is unchanged (a host does not restart), and the signal still arrives 160 samples late, because
	// what a bypassed chain passes is held back by its own latency.
	s.rk->setMuted(0, true);
	for (int i = 0; i < 8; ++i) {
		s.block();
	}
	CHECK(s.rk->latencySamples(kRate) == 160u);
	{
		const auto [out, in] = s.run(8);
		CHECK(delayed(in, out, 160));
	}
	s.rk->setMuted(0, false);
	s.setMode(par::Mode::Parallel);
	CHECK(s.rk->latencySamples(kRate) == 96u);
}

TEST_CASE("series: one chain in Series is Parallel with one chain, bit for bit") {
	const auto render = [&](par::Mode mode) {
		Series s(1, mode);
		s.add(0, "KSHAPE");
		s.rk->setLevel(0, 0.4f);
		for (int i = 0; i < 4; ++i) {
			s.block();
		}
		return s.run(6);
	};
	const auto series = render(par::Mode::Series);
	const auto parallel = render(par::Mode::Parallel);
	CHECK(series.first == parallel.first);
	CHECK(series.first != series.second); // the shaper did something
}

TEST_CASE("series: every chain is handed the notes, and the notes still pass") {
	Series s(3);
	for (size_t c = 0; c < 3; ++c) {
		s.add(c, "KREC");
	}
	s.block(nullptr, nullptr, true, { NoteEvent {} });
	NoteEvent on;
	on.kind = NoteEvent::Kind::On;
	on.key = 61;
	on.velocity = 0.5;
	for (Rec* r : recs()) {
		r->seen.clear();
	}
	s.block(nullptr, nullptr, true, { on });
	REQUIRE(recs().size() == 3);
	for (Rec* r : recs()) {
		REQUIRE(r->seen.size() == 1);
		CHECK(r->seen[0].key == 61);
	}
	size_t out = 0;
	for (const NoteEvent& e : s.engine.blockNotesOut()) {
		out += e.kind == NoteEvent::Kind::On && e.key == 61 ? 1 : 0;
	}
	CHECK(out == 1); // once, not three times
}

TEST_CASE("series: the mode is saved by its label and loads as Series") {
	persist::Rig a(testCatalogue());
	a.seat(par::kTraits.tag);
	a.block(3);
	a.bridge->setField(a.id, static_cast<size_t>(par::Param::Mode), static_cast<float>(par::Mode::Series));
	a.block(3);
	const std::string text = a.save();
	CHECK(text.find("Series") != std::string::npos);
	persist::Rig b(testCatalogue());
	b.load(text);
	b.block(4);
	par::Rack* loaded = par::rackOf(b.bridge->containerSurfaceOf(b.id));
	REQUIRE(loaded != nullptr);
	CHECK(loaded->fieldForTest(par::Param::Mode) == static_cast<float>(par::Mode::Series));
	CHECK(std::string(par::kModeNames[static_cast<size_t>(par::Mode::Series)]) == "Series");
	CHECK(static_cast<size_t>(par::Mode::Series) == std::size(par::kModeNames) - 1); // appended, so no stored label moved
}

TEST_CASE("series: the tail is decided by the output — a latent chain holds the rack awake while its audio is in flight, then lets go") {
	Series s(2);
	s.add(0, "KDELAY", 512.0f);
	s.add(1, "KGAIN");
	for (int i = 0; i < 6; ++i) {
		s.block();
	}
	for (int i = 0; i < 2; ++i) {
		s.block(); // the tone, then the silence
	}
	s.block(nullptr, nullptr, true);
	CHECK(s.rk->holdsTail()); // 512 samples of the tone are still in the delay line
	for (int i = 0; i < 6; ++i) {
		s.block(nullptr, nullptr, true);
	}
	std::vector<float> quiet;
	s.block(&quiet, nullptr, true);
	CHECK(std::all_of(quiet.begin(), quiet.end(), [](float v) { return v == 0.0f; }));
	CHECK_FALSE(s.rk->holdsTail());
}

TEST_CASE("series: a rack in Series allocates nothing on the audio thread, with a bypass moving") {
	Series s(3);
	s.add(0, "KDELAY", 100.0f);
	s.add(1, "KSHAPE");
	s.add(2, "KGAIN");
	for (int i = 0; i < 6; ++i) {
		s.block();
	}
	s.top->drain();
	AllocWindow window;
	for (int rep = 0; rep < 60; ++rep) {
		if (rep % 15 == 0) {
			g_countAllocs.store(false); // the hand's mute is the message thread's; its ramp is what runs here
			s.rk->setMuted(1, rep % 30 == 0);
			g_countAllocs.store(true);
		}
		for (int i = 0; i < kBlock; ++i) {
			s.l[static_cast<size_t>(i)] = s.r[static_cast<size_t>(i)] = tone(static_cast<size_t>(rep * kBlock + i));
		}
		s.engine.setBlockNotes(nullptr, 0);
		s.engine.processStereo(s.l.data(), s.r.data(), kBlock, backplane::unknownBlock());
	}
	CHECK(window.allocs() == 0);
}
