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

// BACKLOG RN — NOTES THROUGH A PARALLEL RACK (built as proposed). An Arp in a
// Rack's chain plays; what each chain emits leaves by the rack's mode and Mix. Every case reads the notes that leave the
// Chain plugin's engine (`blockNotesOut`, its note port), by value: kinds, keys, velocities, and that every on is paired.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "backplane/block_context.h"
#include "mod/arp/arp_types.h"
#include "plugin/device_registry.h"
#include "rack/parallel_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <cstring>
#include <map>
#include <memory>
#include <vector>

using namespace feathers;
namespace par = feathers::rack::parallel;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

size_t keyIndex(const device::Descriptor& d, const char* key) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (d.params[i].key != nullptr && std::strcmp(d.params[i].key, key) == 0) {
			return i;
		}
	}
	FAIL("no control ", key);
	return 0;
}

struct Heard {
	uint64_t at = 0;
	device::NoteEvent e {};
};

/// THE CHAIN PLUGIN'S SHAPE, a Rack in it: chain 0 holds an Arp at a fixed velocity of 0.8; chain 1 holds nothing, so it
/// passes the played notes through. Transport playing at 120 BPM.
struct RackNotes {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	DeviceInstanceId rackId = kNoInstance;
	par::Rack* rk = nullptr;
	DeviceInstanceId arp = kNoInstance;
	std::vector<float> l = std::vector<float>(kBlock, 0.0f);
	std::vector<float> r = std::vector<float>(kBlock, 0.0f);
	uint64_t now = 0;
	std::vector<Heard> heard;

	explicit RackNotes(bool passChain = true) {
		engine.prepare(kRate, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
		rackId = top->addDevice(par::kTraits.tag);
		REQUIRE(rackId != kNoInstance);
		run(2);
		rk = par::rackOf(top->containerSurfaceOf(rackId));
		REQUIRE(rk != nullptr);
		while (rk->chainCount() < (passChain ? 2u : 1u)) {
			(void)rk->addChain();
			run(2);
		}
		rack::RackBridge& a = *rk->chainAt(0)->bridge();
		arp = a.addDevice("ARP");
		REQUIRE(arp != kNoInstance);
		run(2);
		const device::Descriptor& d = *a.descriptorOf(arp);
		a.setField(arp, keyIndex(d, "fixed_velocity"), 1.0f);
		a.setField(arp, keyIndex(d, "velocity"), 0.8f);
		run(2);
		heard.clear();
	}
	void set(par::Param p, float v) {
		top->setField(rackId, static_cast<size_t>(p), v);
	}
	void run(int blocks, std::vector<device::NoteEvent> notes = {}) {
		for (int b = 0; b < blocks; ++b) {
			top->drain();
			const std::vector<device::NoteEvent> in = b == 0 ? notes : std::vector<device::NoteEvent> {};
			engine.setBlockNotes(in.data(), in.size());
			device::TransportClock clock = device::TransportClock::fromSecondsPerBeat(0.5);
			clock.playing = true;
			clock.songPositionSeconds = static_cast<double>(now) / kRate;
			clock.songPositionBeats = clock.songPositionSeconds / 0.5;
			engine.processStereo(l.data(), r.data(), kBlock, backplane::BlockContext { clock });
			top->publish(engine);
			top->reapRetired();
			for (const device::NoteEvent& e : engine.blockNotesOut()) {
				heard.push_back({ now + e.timeSamples, e });
			}
			now += kBlock;
		}
	}
	static device::NoteEvent note(device::NoteEvent::Kind kind, int16_t key, double velocity = 0.6) {
		device::NoteEvent e {};
		e.kind = kind;
		e.key = key;
		e.noteId = key;
		e.velocity = velocity;
		return e;
	}
	/// HOLD `key` for `blocks`, then let it go and run `after` more.
	void hold(int16_t key, int blocks, int after) {
		run(1, { note(device::NoteEvent::Kind::On, key) });
		run(blocks - 1);
		run(1, { note(device::NoteEvent::Kind::Off, key) });
		run(after);
	}
	/// The ons heard of `key`, or (key < 0) of any key but `except`.
	std::vector<Heard> ons(int key, int except = -1000) const {
		std::vector<Heard> out;
		for (const Heard& h : heard) {
			if (h.e.kind == device::NoteEvent::Kind::On && (key < 0 ? h.e.key != except : h.e.key == key)) {
				out.push_back(h);
			}
		}
		return out;
	}
	/// EVERY ON PAIRED WITH AN OFF after it, and nothing still sounding: the count of notes left open (must be 0).
	size_t open() const {
		std::map<std::pair<int, int>, int> held;
		for (const Heard& h : heard) {
			const auto k = std::pair { static_cast<int>(h.e.channel), static_cast<int>(h.e.key) };
			if (h.e.kind == device::NoteEvent::Kind::On) {
				++held[k];
			}
			else if (h.e.kind == device::NoteEvent::Kind::Off || h.e.kind == device::NoteEvent::Kind::Choke) {
				held[k] = std::max(0, held[k] - 1);
			}
		}
		size_t n = 0;
		for (const auto& [k, c] : held) {
			n += static_cast<size_t>(c);
		}
		return n;
	}
};

constexpr int16_t kPlayed = 48; // the Arp plays it, on its own octave: its notes are 48 too, so ons are told by velocity
constexpr double kArpVelocity = 0.8;
constexpr double kPlayedVelocity = 0.6;

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

} // namespace

TEST_CASE("RN: today's rule, kept — a Rack whose chains take no notes hands every note on untouched") {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	engine.prepare(kRate, 4096);
	rack::RackBridge top(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
	(void)top.addDevice(par::kTraits.tag);
	std::vector<float> l(kBlock), r(kBlock);
	for (int b = 0; b < 3; ++b) {
		top.drain();
		engine.processStereo(l.data(), r.data(), kBlock, backplane::unknownBlock());
		top.publish(engine);
	}
	const device::NoteEvent in[] = { RackNotes::note(device::NoteEvent::Kind::On, 60, 0.37) };
	top.drain();
	engine.setBlockNotes(in, 1);
	engine.processStereo(l.data(), r.data(), kBlock, backplane::unknownBlock());
	REQUIRE(engine.blockNotesOut().size() == 1);
	CHECK(engine.blockNotesOut()[0].kind == device::NoteEvent::Kind::On);
	CHECK(engine.blockNotesOut()[0].velocity == 0.37);
}

TEST_CASE("RN Parallel (and Mid/Side, Left/Right, Bands): every chain's notes leave, merged in time — the Arp's at its velocity, the played note through the other chain at its own") {
	for (const par::Mode mode : { par::Mode::Parallel, par::Mode::MidSide, par::Mode::LeftRight, par::Mode::Bands }) {
		CAPTURE(static_cast<int>(mode));
		RackNotes rn;
		rn.set(par::Param::Mode, static_cast<float>(mode));
		rn.hold(kPlayed, 100, 40);
		size_t arpOns = 0;
		size_t played = 0;
		uint64_t last = 0;
		for (const Heard& h : rn.heard) {
			CHECK(h.at >= last); // in time order
			last = h.at;
			if (h.e.kind != device::NoteEvent::Kind::On) {
				continue;
			}
			arpOns += near(h.e.velocity, kArpVelocity) ? 1 : 0;
			played += near(h.e.velocity, kPlayedVelocity) ? 1 : 0;
		}
		CHECK(arpOns >= 4);
		CHECK(played == 1);
		CHECK(rn.open() == 0u);
	}
}

TEST_CASE("RN Select: only the selected chain's notes leave, and a switch hands over at the next note-on — no note cut short") {
	RackNotes rn;
	rn.set(par::Param::Mode, static_cast<float>(par::Mode::Select));
	rn.set(par::Param::Position, 0.0f); // the Arp's chain
	rn.run(1, { RackNotes::note(device::NoteEvent::Kind::On, kPlayed) });
	rn.run(30);
	const size_t arpSoFar = rn.ons(kPlayed).size();
	CHECK(arpSoFar >= 1);
	for (const Heard& h : rn.ons(kPlayed)) {
		CHECK(near(h.e.velocity, kArpVelocity)); // the played note did not leave: its chain is not selected
	}
	// SWITCH to the pass-through chain mid-arp: no new arp note; the played note, already down, is not started late.
	rn.set(par::Param::Position, 1.0f);
	rn.run(40);
	CHECK(rn.ons(kPlayed).size() == arpSoFar);
	// THE NEXT NOTE-ON is the new chain's.
	rn.run(1, { RackNotes::note(device::NoteEvent::Kind::On, 64) });
	rn.run(4);
	REQUIRE(rn.ons(64).size() == 1);
	CHECK(near(rn.ons(64)[0].e.velocity, kPlayedVelocity));
	rn.run(1, { RackNotes::note(device::NoteEvent::Kind::Off, 64), RackNotes::note(device::NoteEvent::Kind::Off, kPlayed) });
	rn.run(40);
	CHECK(rn.open() == 0u); // every note that left, the arp's last one included, ended with its own off
}

TEST_CASE("RN Blend: each chain's note-ons at its weight — 0, 0.25 and 1 — and a chain at 0 starts no new note") {
	for (const float w : { 0.0f, 0.25f, 1.0f }) {
		CAPTURE(w);
		RackNotes rn;
		rn.set(par::Param::Mode, static_cast<float>(par::Mode::Blend));
		rn.set(par::Param::Position, 1.0f - w); // two chains: the Arp's weight is 1 − position
		rn.run(2);
		REQUIRE(par::positionWeight(par::Mode::Blend, 1.0 - w, 0, 2) == doctest::Approx(w));
		rn.hold(kPlayed, 100, 40);
		size_t arp = 0;
		size_t played = 0;
		for (const Heard& h : rn.ons(kPlayed)) {
			if (near(h.e.velocity, std::max(kArpVelocity * w, 1.0 / 127.0)) && w > 0.0f) {
				++arp;
			}
			if (near(h.e.velocity, std::max(kPlayedVelocity * (1.0 - w), 1.0 / 127.0)) && w < 1.0f) {
				++played;
			}
		}
		CHECK(rn.ons(kPlayed).size() == arp + played); // nothing at any other velocity
		CHECK(arp == (w > 0.0f ? arp : 0u));
		CHECK((w > 0.0f) == (arp >= 4));
		CHECK(played == (w < 1.0f ? 1u : 0u));
		CHECK(rn.open() == 0u);
	}
}

TEST_CASE("RN: a weight swept through 0 mid-note leaves that note to end with its own off — no choke, no new note, nothing stuck") {
	RackNotes rn(false); // the Arp's chain alone
	rn.set(par::Param::Mode, static_cast<float>(par::Mode::Blend));
	rn.run(1, { RackNotes::note(device::NoteEvent::Kind::On, kPlayed) });
	// TO THE FIRST ARP NOTE, then the weight to 0 while it sounds (one chain: Blend's weight is its Mix, the knob a route sweeps).
	while (rn.ons(kPlayed).empty()) {
		rn.run(1);
	}
	const size_t before = rn.heard.size();
	REQUIRE(rn.open() == 1u);
	rn.set(par::Param::Mix, 0.0f);
	rn.run(60);
	CHECK(rn.ons(kPlayed).size() == 1u); // no NEW arp note at weight 0
	CHECK(rn.open() == 0u);               // and the one that sounded ended with its own off
	bool offAfter = false;
	for (size_t i = before; i < rn.heard.size(); ++i) {
		offAfter = offAfter || rn.heard[i].e.kind == device::NoteEvent::Kind::Off;
	}
	CHECK(offAfter);
	rn.set(par::Param::Mix, 1.0f);
	rn.run(1, { RackNotes::note(device::NoteEvent::Kind::Off, kPlayed) });
	rn.run(40);
	CHECK(rn.open() == 0u);
}

TEST_CASE("RN Mix: 0 hands on the played notes only, 100 the chains' only, 50 both at half velocity — an Arp's chain alone") {
	for (const float mix : { 0.0f, 0.5f, 1.0f }) {
		CAPTURE(mix);
		RackNotes rn(false);
		rn.set(par::Param::Mix, mix);
		rn.hold(kPlayed, 100, 40);
		size_t arp = 0;
		size_t played = 0;
		for (const Heard& h : rn.ons(kPlayed)) {
			arp += mix > 0.0f && near(h.e.velocity, kArpVelocity * mix) ? 1 : 0;
			played += mix < 1.0f && near(h.e.velocity, kPlayedVelocity * (1.0 - mix)) ? 1 : 0;
		}
		CHECK(rn.ons(kPlayed).size() == arp + played);
		CHECK(played == (mix < 1.0f ? 1u : 0u));
		CHECK((arp >= 4) == (mix > 0.0f));
		CHECK(rn.open() == 0u);
	}
}

TEST_CASE("RN: a passed note never rounds to velocity 0 — a weight of 0.001 still sounds at one MIDI step") {
	RackNotes rn(false);
	rn.set(par::Param::Mix, 0.001f);
	rn.hold(kPlayed, 100, 40);
	const std::vector<Heard> ons = rn.ons(kPlayed);
	REQUIRE_FALSE(ons.empty());
	for (const Heard& h : ons) {
		CHECK(h.e.velocity >= 1.0 / 127.0);
	}
	CHECK(rn.open() == 0u);
}

TEST_CASE("RN: nothing sticks — a mode change, a chain removed, and the Arp removed, each mid-note") {
	SUBCASE("a mode change") {
		RackNotes rn;
		rn.run(1, { RackNotes::note(device::NoteEvent::Kind::On, kPlayed) });
		rn.run(20);
		rn.set(par::Param::Mode, static_cast<float>(par::Mode::Select));
		rn.set(par::Param::Position, 1.0f);
		rn.run(20);
		rn.run(1, { RackNotes::note(device::NoteEvent::Kind::Off, kPlayed) });
		rn.run(40);
		CHECK(rn.open() == 0u);
	}
	SUBCASE("the Arp's chain removed") {
		RackNotes rn;
		rn.run(1, { RackNotes::note(device::NoteEvent::Kind::On, kPlayed) });
		while (rn.open() < 2) { // the played note through the other chain, and an arp note
			rn.run(1);
		}
		rn.rk->removeChain(0);
		rn.run(4);
		rn.run(1, { RackNotes::note(device::NoteEvent::Kind::Off, kPlayed) });
		rn.run(20);
		CHECK(rn.open() == 0u);
	}
	SUBCASE("the Arp removed from its chain") {
		RackNotes rn;
		rn.run(1, { RackNotes::note(device::NoteEvent::Kind::On, kPlayed) });
		while (rn.open() < 2) {
			rn.run(1);
		}
		rn.rk->chainAt(0)->bridge()->removeDevice(rn.arp);
		rn.run(4);
		rn.run(1, { RackNotes::note(device::NoteEvent::Kind::Off, kPlayed) });
		rn.run(20);
		CHECK(rn.open() == 0u);
	}
}

TEST_CASE("RN: the same note passed through by two chains (and the dry, at Mix 50) leaves ONCE, at its own velocity — never doubled") {
	for (const float mix : { 1.0f, 0.5f }) {
		CAPTURE(mix);
		RackNotes rn;
		(void)rn.rk->addChain(); // a second chain that passes its notes through
		rn.run(2);
		REQUIRE(rn.rk->chainCount() == 3u);
		rn.set(par::Param::Mix, mix);
		rn.hold(64, 40, 20); // not the Arp's key: the Arp (chain 0) holds it too, but steps on its own clock
		size_t played = 0;
		for (const Heard& h : rn.ons(64)) {
			played += near(h.e.velocity, kPlayedVelocity) ? 1 : 0;
		}
		CHECK(played == 1u);
		CHECK(rn.open() == 0u);
	}
}
