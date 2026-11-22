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

// A CHAIN'S KEY (rack-chain-key): each chain of a parallel Rack may answer one note, so only that key's notes enter its devices. Every
// claim is read off what a recording device in the chain was handed, or off the sound, by value; the rack's own note output is held
// equal with and without keys.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "backplane/block_context.h"
#include "fx/drum/drum_types.h"
#include "fx/harm/harm_types.h"
#include "persist_rig.h"
#include "plugin/device_registry.h"
#include "rack/parallel_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

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

bool sameEvent(const NoteEvent& a, const NoteEvent& b) {
	// (the rack re-bases an event's offset to its tick, so the offset is not compared: the order, kind, key and values are)
	return a.kind == b.kind && a.key == b.key && a.velocity == b.velocity
	       && a.noteId == b.noteId && a.expressionId == b.expressionId && a.expressionValue == b.expressionValue;
}

bool sameEvents(const std::vector<NoteEvent>& a, const std::vector<NoteEvent>& b) {
	if (a.size() != b.size()) {
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i) {
		if (!sameEvent(a[i], b[i])) {
			return false;
		}
	}
	return true;
}

/// A SCRIPT OF THE HOST'S NOTES, per block: every kind, on three keys.
const std::vector<std::vector<NoteEvent>> kScript = {
	{ on(36, 10), on(38, 40, 0.5), on(40, 90) },
	{ tuning(36, 2.0, 5), tuning(38, -1.0, 6), tuning(40, 3.0, 7) },
	{ off(38, 20), choke(40, 60), off(36, 100) },
	{ on(38, 3), on(36, 70, 0.9) },
	{ off(36, 30), off(38, 50) },
};

} // namespace

TEST_CASE("chain key: a keyed chain's devices are handed only their key's notes; Any is handed every one") {
	KeyRack k(3);
	k.addRecorder(0);
	k.addRecorder(1);
	k.addRecorder(2);
	k.rk->setChainKey(0, 36);
	k.rk->setChainKey(1, 38);
	REQUIRE(k.rk->chainKey(0) == 36);
	REQUIRE(k.rk->chainKey(1) == 38);
	REQUIRE(k.rk->chainKey(2) == par::kAnyKey);
	k.run(2);
	k.forget();
	std::vector<NoteEvent> all;
	for (const auto& b : kScript) {
		k.block(b);
		all.insert(all.end(), b.begin(), b.end());
	}
	const auto only = [&](int key) {
		std::vector<NoteEvent> out;
		for (const NoteEvent& e : all) {
			if (e.key == key) {
				out.push_back(e);
			}
		}
		return out;
	};
	// WHAT THE RECORDER WAS HANDED, by value, in the host's order and at the host's offsets.
	CHECK(sameEvents(k.rec[0]->seen, only(36)));
	CHECK(sameEvents(k.rec[1]->seen, only(38)));
	CHECK(sameEvents(k.rec[2]->seen, all));
	// NON-VACUITY: the script has other keys, and each chain was handed fewer than the whole.
	CHECK(only(36).size() >= 4);
	CHECK(k.rec[0]->seen.size() < all.size());
	CHECK(k.rec[1]->seen.size() < all.size());
}

TEST_CASE("chain key: a key set after the notes began leaves a held note its off, and takes no new note of the old key") {
	for (const bool fromAny : { false, true }) {
		CAPTURE(fromAny);
		KeyRack k(1);
		k.addRecorder(0);
		k.rk->setChainKey(0, fromAny ? par::kAnyKey : 36);
		k.run(2);
		k.forget();
		k.block({ on(36, 5) });
		// THE KEY MOVES UNDER THE HELD NOTE.
		k.rk->setChainKey(0, 40);
		k.block({ tuning(36, 1.0, 3) });
		k.block({ on(36, 8, 0.5), on(40, 20), off(36, 30) });
		k.block({ off(40, 4) });
		std::vector<NoteEvent> want = { on(36, 5), tuning(36, 1.0, 3) };
		// The held 36's off enters (a hung note is the failure); the NEW 36 on, which the key no longer answers, does not.
		want.push_back(on(40, 20));
		want.push_back(off(36, 30));
		want.push_back(off(40, 4));
		CHECK(sameEvents(k.rec[0]->seen, want));
		// An off with no on held under the old key does not enter either.
		k.block({ off(36, 2) });
		CHECK(k.rec[0]->seen.size() == want.size());
	}
}

TEST_CASE("chain key: the rack's note output is the same with keys as without — every incoming note still passes") {
	const auto play = [&](bool keyed) {
		KeyRack k(2);
		k.addRecorder(0);
		k.addRecorder(1);
		if (keyed) {
			k.rk->setChainKey(0, 36);
			k.rk->setChainKey(1, 38);
		}
		k.run(2);
		k.forget();
		for (const auto& b : kScript) {
			k.block(b);
		}
		k.run(2);
		return k.outNotes;
	};
	const auto plain = play(false);
	const auto keyed = play(true);
	REQUIRE(plain.size() == keyed.size());
	size_t total = 0;
	for (size_t b = 0; b < plain.size(); ++b) {
		CAPTURE(b);
		CHECK(sameEvents(plain[b], keyed[b]));
		total += plain[b].size();
	}
	CHECK(total == 13); // the script's events came out: nothing was consumed
}

TEST_CASE("chain key: two chains keyed to different notes sound only on their own, each as it would alone") {
	// A kick (the drum answers key 36) in chain 0 and a snare (38) in chain 1. The drum's own Note decides what it plays; the chain's
	// key decides what it is handed, so keys that do not match the drums silence them, and keys that do are the drums alone.
	const auto render = [&](int key0, int key1, const std::vector<std::vector<NoteEvent>>& script, bool kick, bool snare) {
		KeyRack k(2);
		if (kick) {
			k.addDrum(0, drum::Kind::Kick, 36);
		}
		if (snare) {
			k.addDrum(1, drum::Kind::Snare, 38);
		}
		k.rk->setChainKey(0, key0);
		k.rk->setChainKey(1, key1);
		k.run(2);
		k.forget();
		for (const auto& b : script) {
			k.block(b);
		}
		k.run(6);
		return k.heardL;
	};
	const std::vector<std::vector<NoteEvent>> just36 = { { on(36, 17) }, {}, {} };
	const std::vector<std::vector<NoteEvent>> just38 = { { on(38, 17) }, {}, {} };
	const std::vector<std::vector<NoteEvent>> both = { { on(36, 17), on(38, 17) }, {}, {} };
	const auto peak = [](const std::vector<float>& v) {
		float p = 0.0f;
		for (const float x : v) {
			p = std::max(p, std::fabs(x));
		}
		return p;
	};
	const int any = par::kAnyKey;
	const auto kickOnly = render(any, any, just36, true, false);
	const auto snareOnly = render(any, any, just38, false, true);
	REQUIRE(peak(kickOnly) > 0.0f);
	REQUIRE(peak(snareOnly) > 0.0f);
	CHECK(kickOnly != snareOnly);
	// KEYED TO THEIR OWN NOTES: each note sounds its own drum alone, bit for bit, and both together are the sum.
	CHECK(render(36, 38, just36, true, true) == kickOnly);
	CHECK(render(36, 38, just38, true, true) == snareOnly);
	const auto together = render(36, 38, both, true, true);
	REQUIRE(together.size() == kickOnly.size());
	bool sum = true;
	for (size_t i = 0; i < together.size(); ++i) {
		sum = sum && std::fabs(together[i] - (kickOnly[i] + snareOnly[i])) <= 1.0e-6f * peak(together); // (a float sum, a few ulp of the peak)
	}
	if (!sum) {
		float worst = 0.0f;
		for (size_t i = 0; i < together.size(); ++i) {
			worst = std::max(worst, std::fabs(together[i] - (kickOnly[i] + snareOnly[i])));
		}
		MESSAGE("worst difference " << worst << " peak " << peak(together));
	}
	CHECK(sum);
	// KEYED TO THE OTHER'S NOTE (or to one nobody plays): the drum is handed nothing and is silent — the chain's key, not the drum's.
	CHECK(peak(render(38, 36, both, true, true)) == 0.0f);
	CHECK(peak(render(40, 41, both, true, true)) == 0.0f);
	CHECK(peak(render(any, any, both, true, true)) > 0.0f);
}

TEST_CASE("chain key: an Any chain renders what it did — the same sound whether the key was never touched or set to Any again") {
	const auto render = [&](bool touched) {
		KeyRack k(1);
		k.addDrum(0, drum::Kind::Membrane, 36);
		if (touched) {
			k.rk->setChainKey(0, 40);
			k.run(1);
			k.rk->setChainKey(0, par::kAnyKey);
		}
		k.run(2);
		k.forget();
		for (const auto& b : kScript) {
			k.block(b);
		}
		return k.heardL;
	};
	const auto a = render(false);
	CHECK(a == render(true));
	bool sounded = false;
	for (const float x : a) {
		sounded = sounded || x != 0.0f;
	}
	CHECK(sounded);
}

TEST_CASE("chain key: the key is saved with the patch, defaults to Any, writes nothing when it is Any, and survives a load") {
	std::vector<uint8_t> anyBody;
	std::vector<uint8_t> keyedBody;
	const auto save = [](par::Rack& r, std::vector<uint8_t>* out) {
		device::ByteSink sink { out, [](void* ctx, const uint8_t* d, size_t n) {
			                       static_cast<std::vector<uint8_t>*>(ctx)->insert(static_cast<std::vector<uint8_t>*>(ctx)->end(), d, d + n);
		                       } };
		REQUIRE(r.saveState(sink));
	};
	const auto has = [](const std::vector<uint8_t>& body) {
		const std::string text(body.begin(), body.end());
		return text.find(par::kKeyKey) != std::string::npos;
	};
	persist::Rig a;
	a.seat(par::kTraits.tag);
	a.block(3);
	par::Rack* rk = par::rackOf(a.bridge->containerSurfaceOf(a.id));
	REQUIRE(rk != nullptr);
	while (rk->chainCount() < 3) {
		(void)rk->addChain();
		a.block(2);
	}
	save(*rk, &anyBody);
	CHECK_FALSE(has(anyBody)); // every chain on Any: the five fields it always wrote
	rk->setChainKey(0, 36);
	rk->setChainKey(2, 127);
	a.block(2);
	save(*rk, &keyedBody);
	CHECK(has(keyedBody));
	CHECK(keyedBody.size() > anyBody.size());
	const std::string text = a.save();
	persist::Rig b;
	b.load(text);
	b.block(3);
	par::Rack* loaded = par::rackOf(b.bridge->containerSurfaceOf(b.id));
	REQUIRE(loaded != nullptr);
	REQUIRE(loaded->chainCount() == 3);
	CHECK(loaded->chainKey(0) == 36);
	CHECK(loaded->chainKey(1) == par::kAnyKey);
	CHECK(loaded->chainKey(2) == 127);
	// AN OUT-OF-RANGE KEY IS ANY.
	rk->setChainKey(1, 200);
	CHECK(rk->chainKey(1) == par::kAnyKey);
	rk->setChainKey(1, -5);
	CHECK(rk->chainKey(1) == par::kAnyKey);
	// A REMOVED CHAIN COMES BACK WITH ITS KEY (the undo's snapshot).
	const par::Rack::ChainSnapshot snap = rk->snapshotChain(0);
	CHECK(snap.key == 36);
}

TEST_CASE("chain key: one Rack holds more than 128 chains, and a key is per chain whatever its slot") {
	KeyRack k(130);
	REQUIRE(k.rk->chainCount() == 130);
	k.rk->setChainKey(129, 60);
	k.rk->setChainKey(0, 61);
	CHECK(k.rk->chainKey(129) == 60);
	CHECK(k.rk->chainKey(0) == 61);
	CHECK(k.rk->slotOf(129) == par::kNoChainSlot); // past the 64 slots: no route, still a key
}

namespace {

/// The amplitude of `hz` in `x` (a Goertzel bin over the whole run, scaled so a unit sine reads 1).
double toneAmplitude(const std::vector<float>& x, double hz) {
	const double w = 2.0 * 3.14159265358979323846 * hz / kRate;
	double re = 0.0;
	double im = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		re += x[i] * std::cos(w * static_cast<double>(i));
		im += x[i] * std::sin(w * static_cast<double>(i));
	}
	return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size());
}

} // namespace

TEST_CASE("chain key: a HARM behind a drum in a keyed chain sounds its harmonic of the chain's key") {
	// Key 60 is 261.63 Hz; HARM is set to the 4:1 harmonic, so its sine is expected at 1046.5 Hz. (The drum alone has nothing there to speak of.)
	constexpr double kExpected = 261.6255653 * 4.0;
	const auto render = [&](int chainKey, bool drum, bool harm) {
		KeyRack k(1);
		if (drum) {
			k.addDrum(0, drum::Kind::Kick, 60);
		}
		if (harm) {
			rack::RackBridge& b = *k.rk->chainAt(0)->bridge();
			const DeviceInstanceId id = b.addDevice("HARM");
			REQUIRE(id != kNoInstance);
			k.run(2);
			b.setField(id, static_cast<size_t>(feathers::fx::harm::Param::Harmonic),
			           static_cast<float>(feathers::fx::harm::harmonicIndexOf(4, 1)));
			k.run(2);
		}
		k.rk->setChainKey(0, chainKey);
		k.run(2);
		k.forget();
		k.block({ on(60, 17) });
		k.run(40);
		return k.heardL;
	};
	const auto harmOnly = render(par::kAnyKey, false, true);
	const auto drumOnly = render(par::kAnyKey, true, false);
	const auto drumHarm = render(par::kAnyKey, true, true);
	const auto keyed = render(60, true, true);
	const double floor = 0.02; // a HARM sine at full Level is a good fraction of unity; the drum's own energy at 1046 Hz is below this
	CHECK(toneAmplitude(drumOnly, kExpected) < floor);
	CHECK(toneAmplitude(harmOnly, kExpected) > floor);
	CHECK(toneAmplitude(drumHarm, kExpected) > floor);
	CHECK(toneAmplitude(keyed, kExpected) > floor);
}

TEST_CASE("chain key: a pad strike (the Pads view's own note) reaches a HARM behind a drum, which sounds its harmonic of the pad's key") {
	constexpr double kExpected = 261.6255653 * 4.0;
	const auto render = [&](bool drum, bool harm) {
		KeyRack k(1);
		if (drum) {
			k.addDrum(0, drum::Kind::Kick, 60);
		}
		if (harm) {
			rack::RackBridge& b = *k.rk->chainAt(0)->bridge();
			const DeviceInstanceId id = b.addDevice("HARM");
			REQUIRE(id != kNoInstance);
			k.run(2);
			b.setField(id, static_cast<size_t>(feathers::fx::harm::Param::Harmonic),
			           static_cast<float>(feathers::fx::harm::harmonicIndexOf(4, 1)));
			k.run(2);
		}
		k.rk->setChainKey(0, 60);
		k.run(2);
		k.forget();
		REQUIRE(k.rk->postPadNote(60, true, 0.8f));
		k.run(40); // the pad held down
		return k.heardL;
	};
	const auto drumOnly = render(true, false);
	const auto harmOnly = render(false, true);
	const auto drumHarm = render(true, true);
	const double floor = 0.02;
	CHECK(toneAmplitude(drumOnly, kExpected) < floor);
	CHECK(toneAmplitude(harmOnly, kExpected) > floor);
	CHECK(toneAmplitude(drumHarm, kExpected) > floor);
}
