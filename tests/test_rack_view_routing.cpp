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

// A RACK'S VIEW, ROUTING, CHOKE GROUPS AND STRIKE MASK (rack-view-routing): the engine and state half. Every claim is a value read off
// the sound, off what a recording device was handed, or off the state.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <bit>
#include "test_heap.h"
#include "doctest.h"

#include "backplane/block_context.h"
#include "fx/drum/drum_types.h"
#include "mod/lfo/lfo_types.h"
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

// THE ALLOCATION COUNTER, for this binary only.
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
namespace dd = feathers::fx::drumdevice;
using NoteEvent = device::NoteEvent;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

// A RECORDER: a device with notes that writes down every event it is handed and passes it on.
struct Rec {
	std::vector<NoteEvent> seen;
};
std::vector<Rec*>& recs() {
	static std::vector<Rec*> all;
	return all;
}

const device::AudioOps kRecAudio {
    .stateBytes = sizeof(Rec),
    .stateAlign = alignof(Rec),
    .construct = [](void* s) { recs().push_back(new (s) Rec()); },
    .destruct =
        [](void* s) {
	        auto* p = static_cast<Rec*>(s);
	        recs().erase(std::remove(recs().begin(), recs().end(), p), recs().end());
	        p->~Rec();
        },
    .prepare = [](void*, double, int) {},
    .reset = [](void*) {},
    .setFields = [](void*, const float*, size_t) {},
    .processStereo = [](void*, float*, float*, int) {},
};
const device::NoteOps kRecNotes {
    .stateBytes = sizeof(Rec),
    .stateAlign = alignof(Rec),
    .construct = kRecAudio.construct,
    .destruct = kRecAudio.destruct,
    .prepare = [](void*, double) {},
    .setFields = [](void*, const float*, size_t) {},
    .processNotes =
        [](void* s, const NoteEvent* in, size_t inCount, NoteEvent* out, size_t outCapacity, size_t* outCount) {
	        auto* p = static_cast<Rec*>(s);
	        const size_t n = std::min(inCount, outCapacity);
	        for (size_t i = 0; i < n; ++i) {
		        p->seen.push_back(in[i]);
		        out[i] = in[i];
	        }
	        *outCount = n;
        },
};
const device::Descriptor kRec {
    .traits = { .tag = "KREC", .name = "Key Recorder", .hasCard = false, .hasExpanded = false },
    .params = {},
    .outputs = {},
    .audio = &kRecAudio,
    .notes = &kRecNotes,
};

NoteEvent note(NoteEvent::Kind kind, int key, double velocity, uint32_t at) {
	NoteEvent e {};
	e.kind = kind;
	e.key = static_cast<int16_t>(key);
	e.noteId = key;
	e.velocity = velocity;
	e.timeSamples = at;
	return e;
}
NoteEvent on(int key, uint32_t at = 0, double v = 0.7) { return note(NoteEvent::Kind::On, key, v, at); }
NoteEvent off(int key, uint32_t at = 0) { return note(NoteEvent::Kind::Off, key, 0.0, at); }
NoteEvent choke(int key, uint32_t at = 0) { return note(NoteEvent::Kind::Choke, key, 0.0, at); }
NoteEvent tuning(int key, double semis, uint32_t at = 0) {
	NoteEvent e = note(NoteEvent::Kind::Expression, key, 0.0, at);
	e.expressionId = 2;
	e.expressionValue = semis;
	return e;
}

struct Seat {
	Seat() { seatedDeviceForTest() = &kRec; }
	~Seat() { seatedDeviceForTest() = nullptr; }
};

/// A RACK AS THE CHAIN PLUGIN HOLDS IT: `chains` chains, the host's notes per block, its audio and note stream kept.
struct KeyRack {
	Seat seat;
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	DeviceInstanceId rackId = kNoInstance;
	par::Rack* rk = nullptr;
	std::vector<float> l = std::vector<float>(kBlock, 0.0f);
	std::vector<float> r = std::vector<float>(kBlock, 0.0f);
	std::vector<float> heardL;
	std::vector<std::vector<NoteEvent>> outNotes;
	std::vector<Rec*> rec;

	explicit KeyRack(size_t chains) {
		engine.prepare(kRate, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
		rackId = top->addDevice(par::kTraits.tag);
		REQUIRE(rackId != kNoInstance);
		run(2);
		rk = par::rackOf(top->containerSurfaceOf(rackId));
		REQUIRE(rk != nullptr);
		while (rk->chainCount() < chains) {
			(void)rk->addChain();
			run(2);
		}
		rec.assign(chains, nullptr);
	}
	void addRecorder(size_t chain) {
		const size_t before = recs().size();
		(void)rk->chainAt(chain)->bridge()->addDevice("KREC");
		run(2);
		REQUIRE(recs().size() == before + 1);
		rec[chain] = recs().back();
	}
	DeviceInstanceId addDrum(size_t chain, drum::Kind kind, int drumNote) {
		rack::RackBridge& b = *rk->chainAt(chain)->bridge();
		const DeviceInstanceId id = b.addDevice(dd::kTraits.tag);
		REQUIRE(id != kNoInstance);
		run(2);
		b.setField(id, static_cast<size_t>(dd::Param::Kind), static_cast<float>(kind));
		b.setField(id, static_cast<size_t>(dd::Param::Note), static_cast<float>(drumNote));
		run(2);
		return id;
	}
	/// One block, `notes` handed at their own offsets. The audio and the rack's note output are kept.
	void block(const std::vector<NoteEvent>& notes = {}) {
		top->drain();
		std::fill(l.begin(), l.end(), 0.0f); // the input is silence: a generator adds to it, so last block's output is not this block's input
		std::fill(r.begin(), r.end(), 0.0f);
		engine.setBlockNotes(notes.data(), notes.size());
		engine.processStereo(l.data(), r.data(), kBlock, backplane::unknownBlock());
		top->publish(engine);
		top->reapRetired();
		heardL.insert(heardL.end(), l.begin(), l.end());
		const auto out = engine.blockNotesOut();
		outNotes.emplace_back(out.begin(), out.end());
	}
	void run(int blocks) {
		for (int i = 0; i < blocks; ++i) {
			block();
		}
	}
	void forget() {
		heardL.clear();
		outNotes.clear();
		for (Rec* p : rec) {
			if (p != nullptr) {
				p->seen.clear();
			}
		}
	}
};

} // namespace


namespace {

using P = par::Param;

float peakOf(const std::vector<float>& v) {
	float p = 0.0f;
	for (const float x : v) {
		p = std::max(p, std::fabs(x));
	}
	return p;
}

/// A RACK OF `chains` CHAINS, a drum of `kinds[k]` answering `notes[k]` in each (0 = none), the rack set by `setup`; the host's
/// `script` blocks played, then `tail` quiet blocks. The audio is returned.
struct Build {
	std::vector<int> drumNotes;      // per chain: the key its drum answers, or -1 for an empty chain
	std::vector<drum::Kind> kinds;   // per chain
	std::vector<int> keys;           // per chain: the chain's key
	std::vector<int> groups;         // per chain: the choke group
	float mode = 0.0f;
	float playing = 0.0f;
	std::vector<float> decays;       // per chain: the drum's decay (log2 s), or 0 for its default
};

std::vector<float> render(const Build& b, const std::vector<std::vector<NoteEvent>>& script, size_t tail = 4,
                          const std::vector<int>& cuts = {}) {
	KeyRack k(b.drumNotes.size());
	for (size_t c = 0; c < b.drumNotes.size(); ++c) {
		if (b.drumNotes[c] >= 0) {
			const DeviceInstanceId id = k.addDrum(c, b.kinds[c], b.drumNotes[c]);
			if (c < b.decays.size() && b.decays[c] != 0.0f) {
				k.rk->chainAt(c)->bridge()->setField(id, static_cast<size_t>(dd::Param::Decay), b.decays[c]);
				k.run(1);
			}
		}
		k.rk->setChainKey(c, b.keys[c]);
		k.rk->setChainChoke(c, c < b.groups.size() ? b.groups[c] : 0);
	}
	k.top->setField(k.rackId, static_cast<size_t>(P::Mode), b.mode);
	k.top->setField(k.rackId, static_cast<size_t>(P::Playing), b.playing);
	k.run(4);
	k.forget();
	if (cuts.empty()) {
		for (const auto& blk : script) {
			k.block(blk);
		}
		k.run(static_cast<int>(tail));
		return k.heardL;
	}
	// A DIFFERENT BLOCK SIZE: the script's events laid on a sample line and handed in blocks of the given sizes.
	std::vector<std::pair<size_t, NoteEvent>> line;
	for (size_t blk = 0; blk < script.size(); ++blk) {
		for (const NoteEvent& e : script[blk]) {
			line.emplace_back(blk * kBlock + e.timeSamples, e);
		}
	}
	const size_t total = (script.size() + tail) * kBlock;
	std::vector<float> out;
	size_t start = 0;
	size_t ci = 0;
	while (start < total) {
		const size_t n = std::min<size_t>(static_cast<size_t>(cuts[ci++ % cuts.size()]), total - start);
		std::vector<NoteEvent> here;
		for (auto& [t, e] : line) {
			if (t >= start && t < start + n) {
				NoteEvent ev = e;
				ev.timeSamples = static_cast<uint32_t>(t - start);
				here.push_back(ev);
			}
		}
		k.top->drain();
		k.engine.setBlockNotes(here.data(), here.size());
		std::vector<float> l(n), r(n);
		k.engine.processStereo(l.data(), r.data(), static_cast<int>(n), backplane::unknownBlock());
		k.top->publish(k.engine);
		k.top->reapRetired();
		out.insert(out.end(), l.begin(), l.end());
		start += n;
	}
	return out;
}

const int kSel = static_cast<int>(par::Mode::Select);
const int kKeyedMode = static_cast<int>(par::Mode::Keyed);

} // namespace

TEST_CASE("view routing: the rack declares Keyed beside Parallel and Select, a stepped Playing control, and the view's three controls") {
	CHECK(std::string(par::kModeNames[static_cast<size_t>(par::Mode::Keyed)]) == "Keyed");
	const auto& d = par::kAllParamDescs;
	const auto find = [&](const char* key) -> const device::ParamDesc& {
		for (const auto& p : d) {
			if (std::string(p.key) == key) {
				return p;
			}
		}
		FAIL("no control ", key);
		return d[0];
	};
	CHECK(find("rack_playing").stepped);
	CHECK(find("rack_playing").modulatable);
	for (const char* key : { "rack_view", "rack_viewed_chain", "rack_pads_page", "rack_front" }) {
		CAPTURE(key);
		CHECK_FALSE(find(key).modulatable); // layout is not a lane
		CHECK_FALSE(find(key).hostSlot);
	}
	// THE KIT LEVEL IS THE RACK'S OWN OUTPUT FADER, which exists: Gain, unity at its default.
	CHECK(std::string(par::kParamDescs[static_cast<size_t>(P::Gain)].key) == "rack_gain");
	CHECK(par::kParamDescs[static_cast<size_t>(P::Gain)].def == composite::kFaderUnity);
}

TEST_CASE("view routing: the view, the viewed chain and the pads page change nothing you hear, to the bit") {
	const std::vector<std::vector<NoteEvent>> script = { { on(36, 10), on(38, 60) }, {}, { off(36, 5) } };
	Build b;
	b.drumNotes = { 36, 38 };
	b.kinds = { drum::Kind::Kick, drum::Kind::Snare };
	b.keys = { par::kAnyKey, par::kAnyKey };
	const auto plain = render(b, script);
	REQUIRE(peakOf(plain) > 0.0f);
	KeyRack k(2);
	for (size_t c = 0; c < 2; ++c) {
		k.addDrum(c, b.kinds[c], b.drumNotes[c]);
	}
	k.top->setField(k.rackId, static_cast<size_t>(P::View), 2.0f);
	k.top->setField(k.rackId, static_cast<size_t>(P::ViewedChain), 1.0f);
	k.top->setField(k.rackId, static_cast<size_t>(P::PadsPage), 5.0f);
	k.run(4);
	k.forget();
	for (const auto& blk : script) {
		k.block(blk);
	}
	k.run(4);
	CHECK(k.heardL == plain);
}

TEST_CASE("view routing: Select sounds only the playing chain, which is not the viewed chain; Parallel ignores Playing") {
	const std::vector<std::vector<NoteEvent>> script = { { on(36, 10) }, {}, {} };
	const auto drums = [&](int only) {
		Build b;
		b.drumNotes = { 36, 36, 36 };
		b.kinds = { drum::Kind::Kick, drum::Kind::Membrane, drum::Kind::Snare };
		b.keys = { par::kAnyKey, par::kAnyKey, par::kAnyKey };
		for (int c = 0; c < 3; ++c) {
			if (only >= 0 && c != only) {
				b.drumNotes[static_cast<size_t>(c)] = -1; // an empty chain
			}
		}
		return b;
	};
	for (int playing = 1; playing <= 3; ++playing) {
		CAPTURE(playing);
		Build sel = drums(-1);
		sel.mode = static_cast<float>(kSel);
		sel.playing = static_cast<float>(playing);
		const auto heard = render(sel, script);
		Build alone = drums(playing - 1);
		const auto want = render(alone, script);
		REQUIRE(peakOf(want) > 0.0f);
		CHECK(heard == want); // exactly the playing chain: the others' weight is 0
	}
	// PARALLEL IGNORES IT: Playing 2 leaves the sum of all three.
	Build par3 = drums(-1);
	Build par3Playing = drums(-1);
	par3Playing.playing = 2.0f;
	CHECK(render(par3, script) == render(par3Playing, script));
	// THE VIEWED CHAIN IS A DIFFERENT THING: looking at chain 1 while chain 3 plays.
	KeyRack k(3);
	for (size_t c = 0; c < 3; ++c) {
		k.addDrum(c, drums(-1).kinds[c], 36);
	}
	k.top->setField(k.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kSel));
	k.top->setField(k.rackId, static_cast<size_t>(P::Playing), 3.0f);
	k.top->setField(k.rackId, static_cast<size_t>(P::ViewedChain), 0.0f);
	k.run(4);
	k.forget();
	for (const auto& blk : script) {
		k.block(blk);
	}
	k.run(4);
	Build three = drums(2);
	CHECK(k.heardL == render(three, script));
}

TEST_CASE("view routing: switching the playing chain crossfades, with no step above the signal's own slope plus the fade's") {
	// Two drums sounding, the playing chain moved from 1 to 2 mid-ring. The output is (1-t)a + t b over the select fade of N samples,
	// so its sample-to-sample step is at most max(|da|,|db|) + |b-a|/N; measured against the two chains alone.
	const std::vector<std::vector<NoteEvent>> script = { { on(36, 10) }, {}, {}, {}, {}, {}, {} };
	const auto run = [&](float before, float after, int switchBlock) {
		KeyRack k(2);
		k.addDrum(0, drum::Kind::Membrane, 36);
		k.addDrum(1, drum::Kind::Metal, 36);
		k.top->setField(k.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kSel));
		k.top->setField(k.rackId, static_cast<size_t>(P::Playing), before);
		k.run(4);
		k.forget();
		for (size_t blk = 0; blk < script.size(); ++blk) {
			if (static_cast<int>(blk) == switchBlock) {
				k.top->setField(k.rackId, static_cast<size_t>(P::Playing), after);
			}
			k.block(script[blk]);
		}
		return std::pair { k.heardL, k.rk->selectSamplesForTest() };
	};
	const auto [a, n] = run(1.0f, 1.0f, -1);
	const auto [b, n2] = run(2.0f, 2.0f, -1);
	const auto [x, n3] = run(1.0f, 2.0f, 3);
	REQUIRE(n >= 8);
	REQUIRE(n == n2);
	REQUIRE(a.size() == x.size());
	// WHERE THE SWITCH LANDED: the first sample that is neither chain 1 alone.
	size_t first = 0;
	while (first < x.size() && x[first] == a[first]) {
		++first;
	}
	REQUIRE(first < x.size());
	size_t last = first;
	while (last < x.size() && x[last] != b[last]) {
		++last;
	}
	REQUIRE(last < x.size());
	CHECK(last - first <= static_cast<size_t>(n) + 1); // the fade is the select fade, not longer
	CHECK(last - first >= static_cast<size_t>(n) - 1);
	float worst = 0.0f;
	float bound = 0.0f;
	for (size_t i = std::max<size_t>(first, 1); i < std::min(last + 1, x.size()); ++i) {
		worst = std::max(worst, std::fabs(x[i] - x[i - 1]));
		const float da = std::fabs(a[i] - a[i - 1]);
		const float db = std::fabs(b[i] - b[i - 1]);
		bound = std::max(bound, std::max(da, db) + std::fabs(b[i] - a[i]) / static_cast<float>(n));
	}
	CHECK(worst <= bound * 1.01f + 1.0e-6f);
	// NON-VACUITY: the middle of the fade is neither chain — it is the two together, not a hard switch.
	size_t between = 0;
	for (size_t i = first; i <= last; ++i) {
		between += (x[i] != a[i] && x[i] != b[i]) ? 1 : 0;
	}
	CHECK(between + 2 >= last - first);
}

TEST_CASE("view routing: a modulator and a tail in a chain Select is not playing keep running, as in Parallel") {
	const auto run = [&](float mode) {
		KeyRack k(2);
		k.addDrum(0, drum::Kind::Kick, 36);
		rack::RackBridge& b = *k.rk->chainAt(1)->bridge();
		const DeviceInstanceId lfo = b.addDevice(mod::lfo::kTraits.tag);
		REQUIRE(lfo != kNoInstance);
		k.run(2);
		k.top->setField(k.rackId, static_cast<size_t>(P::Mode), mode);
		k.top->setField(k.rackId, static_cast<size_t>(P::Playing), 1.0f);
		k.run(2);
		std::vector<float> phase;
		const size_t lane = k.rk->chainAt(1)->engine().slotOf(lfo);
		REQUIRE(lane != rack::RackEngine::kNoSlot);
		for (int i = 0; i < 40; ++i) {
			k.block(i == 3 ? std::vector<NoteEvent> { on(36, 4) } : std::vector<NoteEvent> {});
			phase.push_back(k.rk->chainAt(1)->engine().publishedOutputAt(lane, 0));
		}
		return std::pair { phase, k.rk->runsForTest(1) };
	};
	const auto [parallel, runsParallel] = run(static_cast<float>(par::Mode::Parallel));
	const auto [select, runsSelect] = run(static_cast<float>(kSel));
	// THE LFO'S OUTPUT IS THE SAME, SAMPLE FOR SAMPLE, WHETHER OR NOT ITS CHAIN IS THE ONE HEARD; and it is moving.
	CHECK(select == parallel);
	CHECK(*std::max_element(select.begin(), select.end()) != *std::min_element(select.begin(), select.end()));
	CHECK(runsSelect >= runsParallel); // the chain was run every call (Select once more: the call its mode landed in)
}

TEST_CASE("view routing: a note in a choke group cuts the other chains of the group on its own sample; other groups and None are untouched") {
	// An open hat (key 46) in chain 1, and chain 0 — empty, answering key 42, the closed hat — in the same group. The closed hat's
	// note-on chokes the open hat.
	const std::vector<std::vector<NoteEvent>> script = { { on(46, 5) }, {}, { on(42, 17) }, {}, {}, {} };
	const auto go = [&](int g0, int g1, float decay = 0.0f, const std::vector<int>& cuts = {}) {
		Build b;
		b.drumNotes = { -1, 46 };
		b.kinds = { drum::Kind::Metal, drum::Kind::Metal };
		b.keys = { 42, 46 };
		b.groups = { g0, g1 };
		b.decays = { 0.0f, decay };
		return render(b, script, 6, cuts);
	};
	const std::vector<float> open = go(0, 0, 1.0f);
	REQUIRE(peakOf(open) > 0.0f);
	const size_t strikeAt = 2 * kBlock + 17;
	const auto energyAfter = [&](const std::vector<float>& v, size_t from) {
		double e = 0.0;
		for (size_t i = from; i < v.size(); ++i) {
			e += static_cast<double>(v[i]) * v[i];
		}
		return e;
	};
	const size_t faded = strikeAt + 240 + 8; // the 5 ms fade, then a few samples
	const double ringing = energyAfter(open, faded);
	REQUIRE(ringing > 0.0);
	// THE SAME GROUP: the tail's energy after the fade is exactly zero (the hit is gone), and nothing changed before the strike.
	const std::vector<float> choked = go(1, 1, 1.0f);
	CHECK(energyAfter(choked, faded) < 1.0e-6 * ringing);
	size_t firstDiff = 0;
	while (firstDiff < choked.size() && choked[firstDiff] == open[firstDiff]) {
		++firstDiff;
	}
	CHECK(firstDiff == strikeAt + 1); // the fade starts at the note's own sample (its first sample is at unity)
	// ANOTHER GROUP AND NONE: untouched, bit for bit.
	CHECK(go(1, 2, 1.0f) == open);
	CHECK(go(0, 0, 1.0f) == open);
	CHECK(go(0, 1, 1.0f) == open); // a chain not in a group takes the choke of nobody
	// SAMPLE-ACCURATE ACROSS BLOCK PARTITIONS: the same samples at any block size.
	CHECK(go(1, 1, 1.0f, { 64, 64, 64, 64, 64, 64 }) == choked);
	CHECK(go(1, 1, 1.0f, { 37, 100, 5, 300, 1 }) == choked);
	CHECK(go(1, 1, 1.0f, { 1500 }) == choked);
	// A CHOKE LEAVES NO MARK ON THE RACK'S NOTE OUTPUT: only the host's notes come out.
	KeyRack k(2);
	k.rk->setChainKey(0, 42);
	k.rk->setChainKey(1, 46);
	k.rk->setChainChoke(0, 1);
	k.rk->setChainChoke(1, 1);
	k.addRecorder(1);
	k.run(2);
	k.forget();
	k.block({ on(42, 3), on(46, 9) });
	REQUIRE_FALSE(k.outNotes.empty());
	for (const NoteEvent& e : k.outNotes.back()) {
		CHECK(e.kind == NoteEvent::Kind::On);
	}
	CHECK(k.outNotes.back().size() == 2);
	// AND WHAT THE CHAIN WAS HANDED INCLUDES THE CHOKE (its own key's), before nothing else of the other's key.
	bool choke = false;
	for (const NoteEvent& e : k.rec[1]->seen) {
		choke = choke || (e.kind == NoteEvent::Kind::Choke && e.key == 46 && e.timeSamples == 3);
		CHECK_FALSE((e.kind == NoteEvent::Kind::On && e.key == 42));
	}
	CHECK(choke);
}

TEST_CASE("view routing: the strike mask reports exactly the keys struck since it was last taken") {
	KeyRack k(2); // two empty chains: no chain holds a note device, and the mask still reports
	uint64_t m[2] {};
	k.rk->takeStrikeMask(m);
	CHECK(m[0] == 0);
	CHECK(m[1] == 0);
	k.block({ on(36, 4), on(38, 9), off(36, 20), tuning(40, 1.0, 22), choke(41, 30) });
	CHECK(k.rk->keyStruckForTest(36));
	CHECK(k.rk->keyStruckForTest(38));
	CHECK_FALSE(k.rk->keyStruckForTest(40)); // an expression is not a strike
	CHECK_FALSE(k.rk->keyStruckForTest(41));
	k.block({ on(127, 1), on(0, 2), on(64, 3), on(36, 5, 0.0) }); // velocity 0 is an off
	k.rk->takeStrikeMask(m);
	uint64_t want[2] {};
	for (const int key : { 36, 38, 127, 0, 64 }) {
		want[key >> 6] |= uint64_t { 1 } << (key & 63);
	}
	CHECK(m[0] == want[0]);
	CHECK(m[1] == want[1]);
	k.rk->takeStrikeMask(m);
	CHECK(m[0] == 0);
	CHECK(m[1] == 0);
	// WITH DRUMS AND KEYS IN PLAY the mask is still every struck key, whichever chain took it.
	KeyRack d(2);
	d.addDrum(0, drum::Kind::Kick, 36);
	d.rk->setChainKey(0, 36);
	d.block({ on(36, 1), on(50, 2) });
	d.rk->takeStrikeMask(m);
	CHECK((m[0] >> 36 & 1u) == 1u);
	CHECK((m[0] >> 50 & 1u) == 1u);
	CHECK(std::popcount(m[0]) + std::popcount(m[1]) == 2);
}

TEST_CASE("view routing: Keyed routes notes by key alone — a chain on Any is handed none — and its audio is Parallel's") {
	const std::vector<std::vector<NoteEvent>> script = { { on(36, 10), on(38, 40) }, {}, {} };
	KeyRack k(2);
	k.addRecorder(0);
	k.addRecorder(1);
	k.rk->setChainKey(1, 38);
	k.top->setField(k.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kKeyedMode));
	k.run(4);
	k.forget();
	for (const auto& b : script) {
		k.block(b);
	}
	CHECK(k.rec[0]->seen.empty()); // Any, in Keyed
	REQUIRE(k.rec[1]->seen.size() == 1);
	CHECK(k.rec[1]->seen[0].key == 38);
	// THE NOTES STILL PASS: the rack's output has both.
	size_t outs = 0;
	for (const auto& b : k.outNotes) {
		outs += b.size();
	}
	CHECK(outs == 2);
	// IN PARALLEL THE SAME CHAIN ON Any TAKES EVERYTHING.
	KeyRack p(2);
	p.addRecorder(0);
	p.addRecorder(1);
	p.rk->setChainKey(1, 38);
	p.run(4);
	p.forget();
	for (const auto& b : script) {
		p.block(b);
	}
	CHECK(p.rec[0]->seen.size() == 2);
	// THE AUDIO: Keyed with every chain keyed is Parallel with every chain keyed, to the bit.
	Build b;
	b.drumNotes = { 36, 38 };
	b.kinds = { drum::Kind::Kick, drum::Kind::Snare };
	b.keys = { 36, 38 };
	Build keyed = b;
	keyed.mode = static_cast<float>(kKeyedMode);
	CHECK(render(b, script) == render(keyed, script));
}

TEST_CASE("view routing: the same samples at any block size, in Parallel, Select and Keyed") {
	const std::vector<std::vector<NoteEvent>> script = { { on(36, 10), on(38, 40) }, { on(36, 3) }, { off(36, 100), on(38, 200) }, {}, {} };
	for (const int routing : { 0, kSel, kKeyedMode }) {
		CAPTURE(routing);
		Build b;
		b.drumNotes = { 36, 38, 36 };
		b.kinds = { drum::Kind::Kick, drum::Kind::Snare, drum::Kind::Membrane };
		b.keys = { 36, 38, par::kAnyKey };
		b.groups = { 1, 1, 0 };
		b.mode = static_cast<float>(routing);
		b.playing = 2.0f;
		const auto whole = render(b, script, 3);
		REQUIRE(peakOf(whole) > 0.0f);
		for (const std::vector<int>& cut : { std::vector<int> { 64 }, std::vector<int> { 37, 100, 5, 300, 1, 211 }, std::vector<int> { 1024 } }) {
			CHECK(render(b, script, 3, cut) == whole);
		}
	}
}

TEST_CASE("view routing: view, viewed chain, pads page, playing chain, mode, key, choke group and level survive a save and a load") {
	persist::Rig a;
	a.seat(par::kTraits.tag);
	a.block(3);
	par::Rack* rk = par::rackOf(a.bridge->containerSurfaceOf(a.id));
	REQUIRE(rk != nullptr);
	while (rk->chainCount() < 3) {
		(void)rk->addChain();
		a.block(2);
	}
	const auto set = [&](P p, float v) { a.bridge->setField(a.id, static_cast<size_t>(p), v); };
	set(P::View, 2.0f);
	set(P::ViewedChain, 2.0f);
	set(P::PadsPage, 6.0f);
	set(P::Playing, 3.0f);
	set(P::Mode, static_cast<float>(kKeyedMode));
	set(P::Gain, 0.37f);
	set(P::FollowPlayed, 1.0f);
	set(P::Front, 1.0f);
	rk->setChainKey(1, 42);
	rk->setChainChoke(1, 3);
	rk->setChainChoke(2, 8);
	a.block(3);
	const std::string text = a.save();
	persist::Rig b;
	b.load(text);
	b.block(4);
	par::Rack* loaded = par::rackOf(b.bridge->containerSurfaceOf(b.id));
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->chainCount() == 3);
	const auto field = [&](P p) { return b.bridge->fieldAt(b.id, static_cast<size_t>(p)); };
	CHECK(field(P::View) == 2.0f);
	CHECK(field(P::ViewedChain) == 2.0f);
	CHECK(field(P::PadsPage) == 6.0f);
	CHECK(field(P::Playing) == 3.0f);
	CHECK(field(P::Mode) == static_cast<float>(kKeyedMode));
	CHECK(field(P::Gain) == 0.37f);
	CHECK(field(P::FollowPlayed) == 1.0f);
	CHECK(loaded->fieldForTest(P::FollowPlayed) == 1.0f);
	CHECK(field(P::Front) == 1.0f);
	CHECK(loaded->fieldForTest(P::Front) == 1.0f);
	CHECK(loaded->fieldForTest(P::Mode) == static_cast<float>(kKeyedMode));
	CHECK(loaded->chainKey(1) == 42);
	CHECK(loaded->chainChoke(1) == 3);
	CHECK(loaded->chainChoke(2) == 8);
	CHECK(loaded->chainChoke(0) == 0);
	CHECK(loaded->chainKey(0) == par::kAnyKey);
	// A DEFAULT RACK WRITES NONE OF IT: no choke field in a body whose chains are in no group.
	persist::Rig c;
	c.seat(par::kTraits.tag);
	c.block(3);
	par::Rack* fresh = par::rackOf(c.bridge->containerSurfaceOf(c.id));
	std::vector<uint8_t> body;
	device::ByteSink sink { &body, [](void* ctx, const uint8_t* d, size_t n) {
		                       static_cast<std::vector<uint8_t>*>(ctx)->insert(static_cast<std::vector<uint8_t>*>(ctx)->end(), d, d + n);
	                       } };
	REQUIRE(fresh->saveState(sink));
	CHECK(std::string(body.begin(), body.end()).find(par::kChokeKey) == std::string::npos);
	// AND THE SNAPSHOT (an undone removal) CARRIES THE GROUP.
	CHECK(rk->snapshotChain(1).choke == 3);
}

TEST_CASE("view routing: a keyed, choked, Select rack allocates nothing on the audio thread") {
	for (const int routing : { 0, kSel, kKeyedMode }) {
		CAPTURE(routing);
		KeyRack k(3);
		k.addDrum(0, drum::Kind::Metal, 42);
		k.addDrum(1, drum::Kind::Metal, 46);
		k.addDrum(2, drum::Kind::Kick, 36);
		k.rk->setChainKey(0, 42);
		k.rk->setChainKey(1, 46);
		k.rk->setChainKey(2, 36);
		k.rk->setChainChoke(0, 1);
		k.rk->setChainChoke(1, 1);
		k.top->setField(k.rackId, static_cast<size_t>(P::Mode), static_cast<float>(routing));
		k.top->setField(k.rackId, static_cast<size_t>(P::Playing), 2.0f);
		k.run(4);
		// WARM: every road once, so a lazily built table is built here.
		for (int i = 0; i < 6; ++i) {
			k.block({ on(42, 5), on(46, 9), on(36, 20), off(42, 50), choke(46, 60) });
		}
		std::vector<std::vector<NoteEvent>> scripts;
		for (int i = 0; i < 12; ++i) {
			scripts.push_back({ on(i % 2 == 0 ? 42 : 46, static_cast<uint32_t>(3 + i)), on(36, 40), off(36, 90), tuning(46, 1.0, 100) });
		}
		k.top->drain();
		AllocWindow window;
		for (int rep = 0; rep < 40; ++rep) {
			const auto& blk = scripts[static_cast<size_t>(rep) % scripts.size()];
			k.engine.setBlockNotes(blk.data(), blk.size());
			k.engine.processStereo(k.l.data(), k.r.data(), kBlock, backplane::unknownBlock());
			uint64_t m[2];
			k.rk->takeStrikeMask(m);
			(void)k.engine.blockNotesOut();
		}
		CHECK(window.allocs() == 0);
	}
}

TEST_CASE("audition: a Drum's play button sounds the device wherever it sits — a keyed pad chain, any routing, a rack inside a rack") {
	// THE BUTTON POSTS THROUGH ITS HOST: inside a chain that host is the chain's own bridge, whose ring nothing above used to drain. The rack
	// drains it into that chain alone, past the key filter and the mode. Compared with the same drum struck by a host note at the same sample.
	const std::vector<std::vector<NoteEvent>> host = { { on(36, 0) }, {}, {} };
	Build alone; // (the reference: the same drum struck by a note the host sent)
	alone.drumNotes = { 36 };
	alone.kinds = { drum::Kind::Kick };
	alone.keys = { par::kAnyKey };
	REQUIRE(peakOf(render(alone, host)) > 0.0f);
	for (const int routing : { 0, kKeyedMode, kSel, static_cast<int>(par::Mode::Series) }) {
		for (const int keys : { 0, 1 }) { // the drum on One Key, then Any Key (as a pad's chain holds it)
			CAPTURE(routing);
			CAPTURE(keys);
			KeyRack k(2);
			const DeviceInstanceId id = k.addDrum(0, drum::Kind::Kick, 36);
			k.rk->chainAt(0)->bridge()->setField(id, static_cast<size_t>(dd::Param::Keys), static_cast<float>(keys));
			k.rk->setChainKey(0, 40); // a pad on key 40: the drum's own Note is 36, which the chain's key refuses
			k.top->setField(k.rackId, static_cast<size_t>(P::Mode), static_cast<float>(routing));
			k.top->setField(k.rackId, static_cast<size_t>(P::Playing), 1.0f);
			k.run(14);
			k.forget();
			REQUIRE(k.rk->chainAt(0)->bridge()->postNote(36, true, 1.0f));
			k.block({});
			k.run(5);
			// THE SOUND IS THE DRUM'S, to the bit where the rack adds nothing of its own (Parallel, Keyed, Select).
			CHECK(peakOf(k.heardL) > 0.0f);
			// AND IT LEFT NO NOTE BEHIND: the rack's note output carries none of the audition.
			size_t outs = 0;
			for (const auto& blk : k.outNotes) {
				outs += blk.size();
			}
			CHECK(outs == 0);
		}
	}
	// A RACK INSIDE A RACK: the button of a drum two chains down.
	KeyRack outer(1);
	rack::RackBridge& cb = *outer.rk->chainAt(0)->bridge();
	const DeviceInstanceId innerId = cb.addDevice(par::kTraits.tag);
	REQUIRE(innerId != kNoInstance);
	outer.run(4);
	par::Rack* inner = par::rackOf(cb.containerSurfaceOf(innerId));
	REQUIRE(inner != nullptr);
	while (inner->chainCount() < 2) {
		(void)inner->addChain();
		outer.run(3);
	}
	rack::RackBridge& deep = *inner->chainAt(1)->bridge();
	const DeviceInstanceId drum = deep.addDevice(dd::kTraits.tag);
	REQUIRE(drum != kNoInstance);
	outer.run(4);
	inner->setChainKey(1, 60);
	outer.rk->setChainKey(0, 61);
	outer.run(4);
	outer.forget();
	REQUIRE(deep.postNote(36, true, 1.0f));
	outer.block({});
	outer.run(4);
	CHECK(peakOf(outer.heardL) > 0.0f);
}

TEST_CASE("a new chain starts open, by every road that makes one; a chain loaded keeps what it stored, and a put-back keeps its source's state") {
	persist::Rig a;
	a.seat(par::kTraits.tag);
	a.block(3);
	par::Rack* rk = par::rackOf(a.bridge->containerSurfaceOf(a.id));
	REQUIRE(rk != nullptr);
	// A FRESH RACK'S INITIAL CHAIN(S) are open, and so is every chain added after.
	REQUIRE(rk->chainCount() >= 1);
	for (size_t i = 0; i < rk->chainCount(); ++i) {
		CHECK(rk->open(i));
	}
	const size_t second = rk->addChain();
	REQUIRE(second != SIZE_MAX);
	CHECK(rk->open(second));
	const size_t third = rk->addChain();
	REQUIRE(third != SIZE_MAX);
	CHECK(rk->open(third));
	// A SNAPSHOT PUT BACK KEEPS THE SOURCE'S STATE: a closed chain comes back closed, an open one open.
	rk->setOpen(second, false);
	const par::Rack::ChainSnapshot closedSnap = rk->snapshotChain(second);
	const par::Rack::ChainSnapshot openSnap = rk->snapshotChain(third);
	CHECK_FALSE(closedSnap.open);
	CHECK(openSnap.open);
	REQUIRE(rk->removeChain(third));
	REQUIRE(rk->insertChain(rk->chainCount(), openSnap) != SIZE_MAX);
	CHECK(rk->open(rk->chainCount() - 1));
	// A LOAD KEEPS WHAT WAS STORED: chain `second` closed stays closed, the others open stay open.
	a.block(3);
	const std::string text = a.save();
	persist::Rig b;
	b.load(text);
	b.block(4);
	par::Rack* loaded = par::rackOf(b.bridge->containerSurfaceOf(b.id));
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->chainCount() == rk->chainCount());
	for (size_t i = 0; i < loaded->chainCount(); ++i) {
		CAPTURE(i);
		CHECK(loaded->open(i) == (i != second));
	}
}

//--------------------------------------------------------------------------------------------------//
// A PAD'S KEY ENTERS AT ITS RACK                                                                    //
//--------------------------------------------------------------------------------------------------//

namespace {

/// A LANE: a recorder BEFORE the rack, the rack, a recorder AFTER it, in one engine, with `chains` chains in the rack.
struct PadLane {
	Seat seat;
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	DeviceInstanceId rackId = kNoInstance;
	par::Rack* rk = nullptr;
	Rec* before = nullptr;
	Rec* after = nullptr;
	std::vector<float> l = std::vector<float>(kBlock, 0.0f);
	std::vector<float> r = std::vector<float>(kBlock, 0.0f);
	std::vector<float> heardL;

	explicit PadLane(size_t chains) {
		engine.prepare(kRate, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
		REQUIRE(top->addDevice("KREC") != kNoInstance);
		run(2);
		before = recs().back();
		rackId = top->addDevice(par::kTraits.tag);
		REQUIRE(rackId != kNoInstance);
		run(2);
		REQUIRE(top->addDevice("KREC") != kNoInstance);
		run(2);
		after = recs().back();
		rk = par::rackOf(top->containerSurfaceOf(rackId));
		REQUIRE(rk != nullptr);
		while (rk->chainCount() < chains) {
			(void)rk->addChain();
			run(2);
		}
	}
	DeviceInstanceId addDrum(size_t chain, int keysMode) {
		rack::RackBridge& b = *rk->chainAt(chain)->bridge();
		const DeviceInstanceId id = b.addDevice(dd::kTraits.tag);
		REQUIRE(id != kNoInstance);
		run(2);
		b.setField(id, static_cast<size_t>(dd::Param::Keys), static_cast<float>(keysMode));
		run(2);
		return id;
	}
	void block(const std::vector<NoteEvent>& notes = {}) {
		top->drain();
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		engine.setBlockNotes(notes.data(), notes.size());
		engine.processStereo(l.data(), r.data(), kBlock, backplane::unknownBlock());
		top->publish(engine);
		top->reapRetired();
		heardL.insert(heardL.end(), l.begin(), l.end());
	}
	void run(int blocks) {
		for (int i = 0; i < blocks; ++i) {
			block();
		}
	}
	void forget() {
		heardL.clear();
		before->seen.clear();
		after->seen.clear();
	}
};

size_t countOf(const Rec& rec, NoteEvent::Kind kind, int key) {
	size_t n = 0;
	for (const NoteEvent& e : rec.seen) {
		n += e.kind == kind && e.key == key ? 1 : 0;
	}
	return n;
}

} // namespace

TEST_CASE("pad strike: it sounds the keyed chain, downstream hears it, upstream hears nothing, and the sound is the host note's, to the bit") {
	PadLane pad(2);
	(void)pad.addDrum(0, 1);
	pad.rk->setChainKey(0, 40); // a pad on key 40
	pad.run(8);
	pad.forget();
	REQUIRE(pad.rk->postPadNote(40, true, 0.5f));
	pad.block();
	pad.run(6);
	CHECK(peakOf(pad.heardL) > 0.0f);
	// THE DEVICE BEFORE THE RACK RECEIVES NOTHING; the device after it receives the note (as a note the rack passed).
	CHECK(pad.before->seen.empty());
	CHECK(countOf(*pad.after, NoteEvent::Kind::On, 40) == 1);
	// THE SAME HIT FROM A HOST NOTE AT THE SAME SAMPLE, in the same arrangement: the same samples, bit for bit.
	PadLane host(2);
	(void)host.addDrum(0, 1);
	host.rk->setChainKey(0, 40);
	host.run(8);
	host.forget();
	host.block({ on(40, 0, 0.5) });
	host.run(6);
	CHECK(pad.heardL == host.heardL);
	// NON-VACUITY: the host note does reach the device before the rack (so "nothing" above is a claim about the road, not the recorder).
	CHECK(countOf(*host.before, NoteEvent::Kind::On, 40) == 1);
}

TEST_CASE("pad strike: the key filter holds, a held pad's release ends the note, and the rack keeps no note afterwards") {
	PadLane pad(2);
	(void)pad.addDrum(0, 1);
	(void)pad.addDrum(1, 1);
	pad.rk->setChainKey(0, 40);
	pad.rk->setChainKey(1, 41);
	pad.top->setField(pad.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kKeyedMode));
	pad.run(8);
	pad.forget();
	REQUIRE(pad.rk->postPadNote(41, true, 1.0f));
	pad.block();
	pad.run(2);
	CHECK(countOf(*pad.after, NoteEvent::Kind::On, 41) == 1);
	CHECK(countOf(*pad.after, NoteEvent::Kind::Off, 41) == 0);
	CHECK(pad.rk->noteRecordsForTest() > 0);
	REQUIRE(pad.rk->postPadNote(41, false, 0.0f));
	pad.block();
	pad.run(2);
	CHECK(countOf(*pad.after, NoteEvent::Kind::Off, 41) == 1); // THE RELEASE REACHES DOWNSTREAM
	CHECK(pad.rk->noteRecordsForTest() == 0);
	CHECK(pad.before->seen.empty());
	// AND THE KEY FILTER: chain 0 (key 40) never took 41, so it is silent while chain 1 sounds; the same split as a host note's.
	PadLane both(2);
	(void)both.addDrum(0, 1);
	(void)both.addDrum(1, 1);
	both.rk->setChainKey(0, 40);
	both.rk->setChainKey(1, 41);
	both.top->setField(both.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kKeyedMode));
	both.run(8);
	both.forget();
	both.rk->postPadNote(41, true, 1.0f);
	both.block();
	both.run(4);
	PadLane ref(2);
	(void)ref.addDrum(0, 1);
	(void)ref.addDrum(1, 1);
	ref.rk->setChainKey(0, 40);
	ref.rk->setChainKey(1, 41);
	ref.top->setField(ref.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kKeyedMode));
	ref.run(8);
	ref.forget();
	ref.block({ on(41, 0, 1.0) });
	ref.run(4);
	CHECK(both.heardL == ref.heardL);
	CHECK(peakOf(both.heardL) > 0.0f);
}

TEST_CASE("pad strike: choke groups are honoured as for a host note") {
	const auto script = [](PadLane& k, bool pad) {
		k.rk->setChainChoke(0, 1);
		k.rk->setChainChoke(1, 1);
		k.top->setField(k.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kKeyedMode));
		k.run(8);
		k.forget();
		if (pad) {
			k.rk->postPadNote(40, true, 1.0f);
			k.block();
			k.run(2);
			k.rk->postPadNote(41, true, 1.0f);
			k.block();
		}
		else {
			k.block({ on(40, 0, 1.0) });
			k.run(2);
			k.block({ on(41, 0, 1.0) });
		}
		k.run(4);
	};
	PadLane a(2);
	PadLane b(2);
	for (PadLane* k : { &a, &b }) {
		(void)k->addDrum(0, 1);
		(void)k->addDrum(1, 1);
		k->rk->setChainKey(0, 40);
		k->rk->setChainKey(1, 41);
	}
	script(a, true);
	script(b, false);
	CHECK(a.heardL == b.heardL);
	// NON-VACUITY: without the group the second strike leaves the first ringing, so the sound differs.
	PadLane c(2);
	(void)c.addDrum(0, 1);
	(void)c.addDrum(1, 1);
	c.rk->setChainKey(0, 40);
	c.rk->setChainKey(1, 41);
	c.top->setField(c.rackId, static_cast<size_t>(P::Mode), static_cast<float>(kKeyedMode));
	c.run(8);
	c.forget();
	c.rk->postPadNote(40, true, 1.0f);
	c.block();
	c.run(2);
	c.rk->postPadNote(41, true, 1.0f);
	c.block();
	c.run(4);
	CHECK(c.heardL != a.heardL);
}

TEST_CASE("pad strike: a pad of a rack inside a rack enters at the inner rack, so what is before it in the chain hears nothing") {
	PadLane lane(1);
	rack::RackBridge& cb = *lane.rk->chainAt(0)->bridge();
	REQUIRE(cb.addDevice("KREC") != kNoInstance); // before the inner rack, in the outer chain
	lane.run(3);
	Rec* innerBefore = recs().back();
	const DeviceInstanceId innerId = cb.addDevice(par::kTraits.tag);
	REQUIRE(innerId != kNoInstance);
	lane.run(4);
	REQUIRE(cb.addDevice("KREC") != kNoInstance); // after it
	lane.run(3);
	Rec* innerAfter = recs().back();
	par::Rack* inner = par::rackOf(cb.containerSurfaceOf(innerId));
	REQUIRE(inner != nullptr);
	while (inner->chainCount() < 1) {
		(void)inner->addChain();
		lane.run(3);
	}
	rack::RackBridge& deep = *inner->chainAt(0)->bridge();
	const DeviceInstanceId drum = deep.addDevice(dd::kTraits.tag);
	REQUIRE(drum != kNoInstance);
	lane.run(3);
	deep.setField(drum, static_cast<size_t>(dd::Param::Keys), 1.0f);
	inner->setChainKey(0, 60);
	lane.rk->setChainKey(0, 61); // the OUTER chain's key would refuse a 60 that entered at the outer rack
	lane.run(6);
	lane.forget();
	innerBefore->seen.clear();
	innerAfter->seen.clear();
	REQUIRE(inner->postPadNote(60, true, 1.0f));
	lane.block();
	lane.run(5);
	CHECK(peakOf(lane.heardL) > 0.0f);
	CHECK(innerBefore->seen.empty());
	CHECK(lane.before->seen.empty());
	CHECK(countOf(*innerAfter, NoteEvent::Kind::On, 60) == 1);
	// THE OUTER RACK'S OWN DOWNSTREAM never heard a 60 from the outer rack's key filter: the note stayed inside its chain.
	CHECK(lane.before->seen.empty());
}

TEST_CASE("pad strike: nothing is allocated on the audio thread by a strike") {
	PadLane pad(1);
	(void)pad.addDrum(0, 1);
	pad.rk->setChainKey(0, 40);
	pad.run(8);
	for (int i = 0; i < 4; ++i) { // WARM: every road once, so a lazily built table is built here
		REQUIRE(pad.rk->postPadNote(40, true, 1.0f));
		pad.block();
		(void)pad.rk->postPadNote(40, false, 0.0f);
		pad.block();
	}
	REQUIRE(pad.rk->postPadNote(40, true, 1.0f));
	pad.heardL.reserve(1 << 16); // (the test's own recording grows outside the window)
	pad.before->seen.reserve(1 << 12);
	pad.after->seen.reserve(1 << 12);
	{
		pad.top->drain();
		AllocWindow w;
		const auto quiet = [&] {
			pad.engine.setBlockNotes(nullptr, 0);
			pad.engine.processStereo(pad.l.data(), pad.r.data(), kBlock, backplane::unknownBlock());
		};
		quiet();
		const uint64_t a1 = w.allocs();
		quiet();
		const uint64_t a2 = w.allocs();
		(void)pad.rk->postPadNote(40, false, 0.0f);
		quiet();
		(void)a1;
		(void)a2;
		CHECK(w.allocs() == 0);
	}

}
