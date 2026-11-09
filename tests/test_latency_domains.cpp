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

// BACKLOG LW — EVENTS AND MODULATION THAT FOLLOW THE AUDIO PAST A LATENT DEVICE (option A,
// approved). Steps 1–2: each node's arrival latency A(n), and the note a device is handed
// shifted by it. Every case reads VALUES: the arrival the engine summed, and the sample at which a device was first
// handed a gate — through a probe that IS HARM (its descriptor, its instance) with a recorder on its two calls.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "backplane/block_context.h"
#include "composite/composite_types.h"
#include "fx/eq/eq_types.h"
#include "fx/filter/filter_types.h"
#include "fx/harm/harm_types.h"
#include "fx/spectral/spectral_types.h"
#include "plugin/device_registry.h"
#include "plugin/editor_lanes.h"
#include "plugin/gui_bridge.h"
#include "plugin/synth.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/site_chains.h"

#include <memory>
#include <vector>

using namespace feathers;

namespace {

constexpr double kRate = 48000.0;
const uint32_t kL = fx::eq::linearLatency(kRate);
const size_t kPhase = fx::eq::paramIndex(fx::eq::Param::Phase);

//------------------------------------------------------------------------------------------------//
// THE PROBE: HARM, recorded. What it was handed, and the frame count it had run when the gate first rose and fell.   //
//------------------------------------------------------------------------------------------------//

struct ProbeLog {
	uint64_t frames = 0;
	bool gate = false;
	int64_t onset = -1;
	int64_t release = -1;
	size_t calls = 0;
	float pitch = 0.0f;
	/// HOW MANY TIMES THE GATE ROSE, as the device ran: the notes it played.
	size_t rises = 0;
	bool wasUp = false;
	/// THE ROW (step 3): Level as the last `setFields` handed it, and the frame count when it first read `levelTarget`.
	float level = -1.0f;
	float levelTarget = -2.0f;
	int64_t levelAt = -1;
	/// THE CLOCK (step 3): as the last `setTransportClock` handed it, and the frame count it was handed at.
	device::TransportClock clock {};
	bool clocked = false;
	uint64_t clockFrame = 0; ///< the frame count the clock was handed at

	/// AND THE FIRST FRAME AT WHICH THE CLOCK READ BEFORE `jumpBelow` SECONDS — a locate, arriving.
	double jumpBelow = -1.0;
	int64_t jumpAt = -1;
};
ProbeLog gLog;
device::AudioOps gOps {};
device::Descriptor gDesc {};
void (*gHarmNote)(void*, const device::NoteContext&) = nullptr;
void (*gHarmRun)(void*, float*, float*, int) = nullptr;
void (*gHarmMono)(void*, float*, int) = nullptr;
void (*gHarmFields)(void*, const float*, size_t) = nullptr;
const size_t kLevel = static_cast<size_t>(fx::harm::Param::Level);
/// UNDER HARM'S OWN TAG: seated (`seatedDeviceForTest`) for the case's life, it is what every host's catalogue builds
/// for "HARM" — Feathers' lane too, whose bridge admits only a registered tag.
const char* const kProbe = fx::harm::kTraits.tag;

struct Seat {
	Seat() {
		gLog = ProbeLog {};
		gOps = *fx::harm::kDescriptor.audio;
		gHarmNote = gOps.setNoteContext;
		gHarmRun = gOps.processStereo;
		gOps.setNoteContext = [](void* s, const device::NoteContext& n) {
			gLog.gate = n.gate;
			gLog.pitch = n.pitchSemitones;
			gHarmNote(s, n);
		};
		gHarmFields = gOps.setFields;
		gOps.setFields = [](void* s, const float* fields, size_t n) {
			if (kLevel < n) {
				gLog.level = fields[kLevel];
			}
			gHarmFields(s, fields, n);
		};
		gOps.setTransportClock = [](void*, const device::TransportClock& c) {
			gLog.clock = c;
			gLog.clocked = true;
			gLog.clockFrame = gLog.frames;
		};
		gHarmMono = gOps.processMono;
		gOps.processMono = [](void* s, float* m, int f) {
			if (gLog.gate && gLog.onset < 0) {
				gLog.onset = static_cast<int64_t>(gLog.frames);
			}
			if (!gLog.gate && gLog.onset >= 0 && gLog.release < 0) {
				gLog.release = static_cast<int64_t>(gLog.frames);
			}
			gLog.frames += static_cast<uint64_t>(f);
			++gLog.calls;
			gHarmMono(s, m, f);
		};
		gOps.processStereo = [](void* s, float* l, float* r, int f) {
			if (gLog.level == gLog.levelTarget && gLog.levelAt < 0) {
				gLog.levelAt = static_cast<int64_t>(gLog.frames);
			}
			if (gLog.clocked && gLog.jumpBelow >= 0.0 && gLog.jumpAt < 0 && gLog.clock.songPositionSeconds < gLog.jumpBelow) {
				gLog.jumpAt = static_cast<int64_t>(gLog.frames);
			}
			if (gLog.gate && gLog.onset < 0) {
				gLog.onset = static_cast<int64_t>(gLog.frames);
			}
			if (!gLog.gate && gLog.onset >= 0 && gLog.release < 0) {
				gLog.release = static_cast<int64_t>(gLog.frames);
			}
			gLog.rises += gLog.gate && !gLog.wasUp ? 1 : 0;
			gLog.wasUp = gLog.gate;
			gLog.frames += static_cast<uint64_t>(f);
			++gLog.calls;
			gHarmRun(s, l, r, f);
		};
		gDesc = fx::harm::kDescriptor;
		gDesc.audio = &gOps;
		seatedDeviceForTest() = &gDesc;
	}
	~Seat() { seatedDeviceForTest() = nullptr; }
};

device::NoteEvent note(device::NoteEvent::Kind kind, uint32_t at, int16_t key = 60) {
	device::NoteEvent e {};
	e.kind = kind;
	e.timeSamples = at;
	e.key = key;
	e.noteId = key;
	return e;
}

/// THE CHAIN PLUGIN'S SHAPE: a top bridge over an engine a block runs on, silence in, notes per block.
struct Chain {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> top;
	std::vector<float> l = std::vector<float>(4096, 0.0f);
	std::vector<float> r = std::vector<float>(4096, 0.0f);

	Chain() {
		engine.prepare(kRate, 4096);
		top = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
	}
	DeviceInstanceId add(rack::RackBridge& b, const char* tag) {
		const DeviceInstanceId id = b.addDevice(tag);
		REQUIRE(id != kNoInstance);
		run(64);
		run(64);
		return id;
	}
	DeviceInstanceId add(const char* tag) { return add(*top, tag); }
	void run(int frames, std::vector<device::NoteEvent> notes = {},
	         const backplane::BlockContext& block = backplane::unknownBlock()) {
		top->drain();
		engine.setBlockNotes(notes.data(), notes.size());
		std::fill_n(l.begin(), frames, 0.0f);
		std::fill_n(r.begin(), frames, 0.0f);
		engine.processStereo(l.data(), r.data(), frames, block);
		top->publish(engine);
		top->reapRetired();
	}
	uint32_t arrival(const rack::RackEngine& e, DeviceInstanceId id) const { return e.arrivalForTest(e.slotOf(id)); }
	uint32_t arrival(DeviceInstanceId id) const { return arrival(engine, id); }
	/// A NOTE OF `held` SAMPLES at `at` into a 256-sample block, then on until `total` samples have run.
	void play(uint32_t at, uint64_t held, uint64_t total) {
		// 100-SAMPLE BLOCKS: the note's piece starts off the tick grid, and 9216 later is no host or tick boundary, so the
		// shifted moment lands on its own sample only if the engine cuts there.
		constexpr int kBlock = 100;
		uint64_t ran = 0;
		bool on = false;
		bool off = false;
		while (ran < total) {
			std::vector<device::NoteEvent> ev;
			if (!on && ran + kBlock > at) {
				ev.push_back(note(device::NoteEvent::Kind::On, static_cast<uint32_t>(at - ran)));
				on = true;
			}
			if (on && !off && ran + kBlock > at + held) {
				ev.push_back(note(device::NoteEvent::Kind::Off, static_cast<uint32_t>(at + held - ran)));
				off = true;
			}
			run(kBlock, ev);
			ran += kBlock;
		}
	}
};

rack::RackBridge& inside(rack::RackBridge& b, DeviceInstanceId container) {
	rack::RackBridge* in = rack::bridgeOf(b.containerSurfaceOf(container));
	REQUIRE(in != nullptr);
	return *in;
}

/// HARM BEHIND AN EQ (linear or not) in the Chain plugin: the probe's onset and release, from a note at 1000 held 4000.
ProbeLog probeBehindEq(bool linear) {
	Seat seat;
	Chain c;
	const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
	const DeviceInstanceId h = c.add(kProbe);
	c.top->setField(eq, kPhase, linear ? 1.0f : 0.0f);
	c.run(64);
	CHECK(c.arrival(h) == (linear ? kL : 0u));
	gLog = ProbeLog {};
	c.play(1000, 4000, 1000 + 4000 + kL + 4096);
	return gLog;
}

} // namespace

TEST_CASE("LW step 1: each node's arrival latency A(n) is what precedes it — serial, inside a Chain, in each of a Rack's chains and either side of a site — and a latency change moves it at the next block") {
	Seat seat;
	Chain c;
	const DeviceInstanceId eq1 = c.add(fx::eq::kTraits.tag);
	const DeviceInstanceId filter = c.add(fx::filter::kTraits.tag);
	const DeviceInstanceId eq2 = c.add(fx::eq::kTraits.tag);
	const DeviceInstanceId h = c.add(kProbe);
	c.top->setField(eq1, kPhase, 1.0f);
	c.top->setField(eq2, kPhase, 1.0f);
	c.run(64);
	// SERIAL: a device is late by what is before it, not by itself.
	CHECK(c.arrival(eq1) == 0u);
	CHECK(c.arrival(filter) == kL);
	CHECK(c.arrival(eq2) == kL);
	CHECK(c.arrival(h) == 2 * kL);
	// A LATENCY CHANGE (the first EQ back to minimum phase) moves every arrival after it at the next block.
	c.top->setField(eq1, kPhase, 0.0f);
	c.run(64);
	CHECK(c.arrival(filter) == 0u);
	CHECK(c.arrival(eq2) == 0u);
	CHECK(c.arrival(h) == kL);

	SUBCASE("inside a Chain: the inner engine sums its own, and the Chain stands where its host puts it") {
		Chain n;
		const DeviceInstanceId outerEq = n.add(fx::eq::kTraits.tag);
		const DeviceInstanceId chain = n.add(composite::kTraits.tag);
		rack::RackBridge& in = inside(*n.top, chain);
		const DeviceInstanceId innerEq = n.add(in, fx::eq::kTraits.tag);
		const DeviceInstanceId innerProbe = n.add(in, kProbe);
		n.top->setField(outerEq, kPhase, 1.0f);
		in.setField(innerEq, kPhase, 1.0f);
		n.run(64);
		n.run(64);
		CHECK(n.arrival(chain) == kL);
		CHECK(n.arrival(in.engineForTest(), innerProbe) == kL);
	}
	SUBCASE("in a Rack's chains: each chain sums its own from the Rack's input") {
		Chain p;
		const DeviceInstanceId outerEq = p.add(fx::eq::kTraits.tag);
		const DeviceInstanceId rackId = p.add(rack::parallel::kTraits.tag);
		rack::parallel::Rack* rk = rack::parallel::rackOf(p.top->containerSurfaceOf(rackId));
		REQUIRE(rk != nullptr);
		while (rk->chainCount() < 2) {
			(void)rk->addChain();
			p.run(64);
		}
		rack::RackBridge& a = *rk->chainAt(0)->bridge();
		rack::RackBridge& b = *rk->chainAt(1)->bridge();
		const DeviceInstanceId aEq = p.add(a, fx::eq::kTraits.tag);
		const DeviceInstanceId aProbe = p.add(a, kProbe);
		const DeviceInstanceId bFilter = p.add(b, fx::filter::kTraits.tag);
		p.top->setField(outerEq, kPhase, 1.0f);
		a.setField(aEq, kPhase, 1.0f);
		p.run(64);
		p.run(64);
		CHECK(p.arrival(rackId) == kL);
		CHECK(p.arrival(a.engineForTest(), aProbe) == kL);
		CHECK(p.arrival(b.engineForTest(), bFilter) == 0u);
	}
	SUBCASE("either side of a site: the device after its PRE, its POST after PRE and the device, and what follows after all three") {
		Chain s;
		const DeviceInstanceId holder = s.add(fx::eq::kTraits.tag);
		REQUIRE(s.top->openSite(holder));
		s.run(64);
		s.run(64);
		auto* site = static_cast<rack::SiteChains*>(s.top->containerSurfaceOf(siteKeyOf(holder)));
		REQUIRE(site != nullptr);
		const DeviceInstanceId preEq = s.add(*site->pre().bridge(), fx::eq::kTraits.tag);
		const DeviceInstanceId after = s.add(fx::filter::kTraits.tag);
		site->pre().bridge()->setField(preEq, kPhase, 1.0f);
		s.top->setField(holder, kPhase, 1.0f);
		s.run(64);
		s.run(64);
		const rack::RackEngine& e = s.engine;
		CHECK(e.arrivalForTest(e.slotOf(holder)) == kL);
		CHECK(e.arrivalPostForTest(e.slotOf(holder)) == 2 * kL);
		CHECK(s.arrival(after) == 2 * kL);
	}
}

TEST_CASE("LW step 2: HARM behind a linear-phase EQ is handed its note exactly eq::linearLatency late — onset and release — on its own sample") {
	const ProbeLog early = probeBehindEq(false);
	const ProbeLog late = probeBehindEq(true);
	REQUIRE(early.onset >= 0);
	REQUIRE(early.release > early.onset);
	REQUIRE(late.onset >= 0);
	REQUIRE(late.release > late.onset);
	CHECK(late.onset - early.onset == static_cast<int64_t>(kL));
	CHECK(late.release - early.release == static_cast<int64_t>(kL));
	// NOT ON A BOUNDARY BY LUCK: the note's piece starts on the 100-sample host grid, and that start plus 9216 is on
	// neither it nor the 64-sample tick, so this passes only if the late piece is cut exactly there.
	CHECK((early.onset + static_cast<int64_t>(kL)) % 100 != 0);
}

TEST_CASE("LW step 2: nested — HARM inside a Chain behind an EQ, the Chain behind another: handed its note 2 × eq::linearLatency late") {
	const auto onsetOf = [](bool linear) {
		Seat seat;
		Chain c;
		const DeviceInstanceId outerEq = c.add(fx::eq::kTraits.tag);
		const DeviceInstanceId chain = c.add(composite::kTraits.tag);
		rack::RackBridge& in = inside(*c.top, chain);
		const DeviceInstanceId innerEq = c.add(in, fx::eq::kTraits.tag);
		(void)c.add(in, kProbe);
		c.top->setField(outerEq, kPhase, linear ? 1.0f : 0.0f);
		in.setField(innerEq, kPhase, linear ? 1.0f : 0.0f);
		c.run(64);
		c.run(64);
		gLog = ProbeLog {};
		c.play(1000, 3000, 1000 + 3000 + 2 * kL + 4096);
		return gLog;
	};
	const ProbeLog early = onsetOf(false);
	const ProbeLog late = onsetOf(true);
	REQUIRE(early.onset >= 0);
	REQUIRE(late.onset >= 0);
	CHECK(late.onset - early.onset == static_cast<int64_t>(2 * kL));
	CHECK(late.release - early.release == static_cast<int64_t>(2 * kL));
}

TEST_CASE("LW step 2: in Feathers' fx lane, HARM behind a linear-phase EQ is handed its note exactly eq::linearLatency late") {
	const auto onsetOf = [](bool linear) {
		Seat seat;
		ParamSet params;
		Synth synth { params };
		ParamBridge bridge;
		synth.activate(kRate, 4096);
		synth.setCatalogue(hostCatalogue());
		std::vector<float> l(100, 0.0f);
		std::vector<float> r(100, 0.0f);
		const auto blocks = [&](int n) {
			for (int i = 0; i < n; ++i) {
				publishEditorLanes(bridge, params, synth);
				(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
				synth.render(l.data(), r.data(), 100); // off any grid 9216 sits on
			}
		};
		const size_t eqSlot = bridge.addFxDevice(fx::eq::kTraits.tag);
		REQUIRE(eqSlot < kMaxFxSlots);
		const size_t probeSlot = bridge.addFxDevice(kProbe);
		REQUIRE(probeSlot < kMaxFxSlots);
		bridge.refresh();
		bridge.setDeviceControl(bridge.fxDeviceAt(eqSlot), kPhase, linear ? 1.0f : 0.0f);
		blocks(4);
		gLog = ProbeLog {};
		synth.noteOn(0, 60, 1, 1.0);
		blocks(41); // 4100 samples
		synth.noteOff(0, 60, 1, 0.0);
		blocks(static_cast<int>((kL + 48000) / 100) + 2);
		return gLog;
	};
	const ProbeLog early = onsetOf(false);
	const ProbeLog late = onsetOf(true);
	REQUIRE(early.onset >= 0);
	REQUIRE(late.onset >= 0);
	CHECK(late.onset - early.onset == static_cast<int64_t>(kL));
	CHECK((early.onset + static_cast<int64_t>(kL)) % 100 != 0);
	REQUIRE(early.release > early.onset);
	REQUIRE(late.release > late.onset);
	CHECK(late.release - early.release == static_cast<int64_t>(kL));
}

TEST_CASE("LW zero cost: with no latent device — or a latent one with nothing after it — no delay work is done, every device is handed the collapsed note, and nothing is cut") {
	for (const bool eqAtEnd : { false, true }) {
		CAPTURE(eqAtEnd);
		Seat seat;
		Chain c;
		(void)c.add(fx::filter::kTraits.tag);
		const DeviceInstanceId h = c.add(kProbe);
		if (eqAtEnd) {
			const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
			c.top->setField(eq, kPhase, 1.0f); // latent, and last: nothing hears it late
		}
		c.run(64);
		gLog = ProbeLog {};
		c.play(1000, 3000, 8192);
		CHECK(c.engine.lateWorkForTest() == 0u);
		CHECK(c.arrival(h) == 0u);
		const size_t at = c.engine.slotOf(h);
		CHECK(c.engine.busNoteForTest(at).gate == c.engine.noteContextForTest().gate);
		CHECK(c.engine.busNoteForTest(at).pitchSemitones == c.engine.noteContextForTest().pitchSemitones);
		// ONE CALL PER PIECE, as before: the 100-sample host blocks cut on the 64-sample tick, and nothing else.
		size_t pieces = 0;
		for (uint64_t at = 0, phase = 0; at < 8200; at += 100) {
			for (uint64_t done = 0; done < 100;) {
				const uint64_t m = std::min<uint64_t>(100 - done, 64 - phase);
				++pieces;
				phase = (phase + m) % 64;
				done += m;
			}
		}
		CHECK(gLog.calls == pieces);
		CHECK_FALSE(c.engine.holdsTail());
		CHECK(c.engine.modRingsInUseForTest() == 0u);
		CHECK(c.engine.pendingFieldsForTest() == 0u);
	}
}

TEST_CASE("LW: a latency that moves under a held note never strands one — the note ends, on time for where the device now stands") {
	Seat seat;
	Chain c;
	const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
	const DeviceInstanceId h = c.add(kProbe);
	c.top->setField(eq, kPhase, 1.0f);
	c.run(64);
	REQUIRE(c.arrival(h) == kL);

	SUBCASE("linear to minimum mid-note: the device takes the note as it now is, and lets go at the note-off") {
		gLog = ProbeLog {};
		c.run(256, { note(device::NoteEvent::Kind::On, 0) });
		for (int b = 0; b < 8; ++b) {
			c.run(256);
		}
		CHECK_FALSE(gLog.gate); // still early in its delay
		c.top->setField(eq, kPhase, 0.0f);
		c.run(256);
		CHECK(c.arrival(h) == 0u);
		CHECK(gLog.gate); // now on time: the note is held
		c.run(256, { note(device::NoteEvent::Kind::Off, 0) });
		c.run(256);
		CHECK_FALSE(gLog.gate);
		for (int b = 0; b < 60; ++b) {
			c.run(256);
			CHECK_FALSE(gLog.gate);
		}
	}
	SUBCASE("minimum to linear after the note-off: the device is never handed the note again") {
		c.top->setField(eq, kPhase, 0.0f);
		c.run(64);
		REQUIRE(c.arrival(h) == 0u);
		gLog = ProbeLog {};
		c.run(256, { note(device::NoteEvent::Kind::On, 0) });
		c.run(256);
		c.run(256, { note(device::NoteEvent::Kind::Off, 0) });
		c.run(256);
		REQUIRE_FALSE(gLog.gate);
		c.top->setField(eq, kPhase, 1.0f);
		for (int b = 0; b < 60; ++b) {
			c.run(256);
			CHECK_FALSE(gLog.gate);
		}
	}
	SUBCASE("minimum to linear mid-note: the release comes exactly eq::linearLatency after the note-off") {
		c.top->setField(eq, kPhase, 0.0f);
		c.run(64);
		gLog = ProbeLog {};
		c.run(256, { note(device::NoteEvent::Kind::On, 0) });
		c.top->setField(eq, kPhase, 1.0f);
		for (int b = 0; b < 4; ++b) {
			c.run(256);
		}
		const int64_t offAt = static_cast<int64_t>(gLog.frames);
		c.run(256, { note(device::NoteEvent::Kind::Off, 0) });
		for (int b = 0; b < static_cast<int>(kL / 256) + 4; ++b) {
			c.run(256);
		}
		CHECK_FALSE(gLog.gate);
		CHECK(gLog.release - offAt == static_cast<int64_t>(kL));
	}
}

TEST_CASE("LW, the SLEEP rule: a note still on its way to a late device is sound to come — the engine holds it until the device has been handed it") {
	Seat seat;
	Chain c;
	const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
	(void)c.add(kProbe);
	c.top->setField(eq, kPhase, 1.0f);
	c.run(64);
	gLog = ProbeLog {};
	// A SHORT NOTE INTO SILENCE: nothing the EQ holds is audio, and the device has not heard the note yet.
	c.run(256, { note(device::NoteEvent::Kind::On, 0), note(device::NoteEvent::Kind::Off, 128) });
	CHECK(c.engine.holdsTail());
	uint64_t ran = 256;
	while (c.engine.holdsTail() && ran < 4 * kL) {
		c.run(256);
		ran += 256;
	}
	// HELD UNTIL THE RELEASE WAS HANDED OVER, and not a block past it.
	CHECK(gLog.release >= 0);
	CHECK(ran >= static_cast<uint64_t>(gLog.release));
	CHECK(ran < static_cast<uint64_t>(gLog.release) + 2 * 256);
	CHECK_FALSE(gLog.gate);
}

TEST_CASE("LW step 3: a DAW automation step reaches HARM behind a linear-phase EQ exactly eq::linearLatency later, on its own sample — and a hand's edit lands at once") {
	enum class Road { Edit, Host };
	const auto landedAt = [](Road road, bool linear) {
		Seat seat;
		Chain c;
		const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
		const DeviceInstanceId h = c.add(kProbe);
		c.top->setField(eq, kPhase, linear ? 1.0f : 0.0f);
		c.run(64);
		uint32_t slot = UINT32_MAX;
		if (road == Road::Host) {
			REQUIRE(c.top->exposeToHost(h, kLevel, nullptr));
			slot = c.top->hostSlotOf(h, kLevel).slot;
			REQUIRE(slot != UINT32_MAX);
		}
		for (int i = 0; i < 3; ++i) {
			(void)c.top->serviceHostSlots(); // the binding reaches the engine at its drain, as the Chain plugin's beat does
			c.run(64);
		}
		gLog = ProbeLog {};
		gLog.levelTarget = 0.25f;
		for (int b = 0; b < 10; ++b) {
			c.run(100);
		}
		if (road == Road::Edit) {
			c.top->setField(h, kLevel, 0.25f);
		}
		else {
			c.top->hostSlots()->bus().onHostValue(slot, 0.25f);
		}
		c.run(100);
		// WAITING ITS TURN, AND SAID SO: a write still to land is sound to come (the SLEEP rule). A hand's edit does not wait.
		const bool waits = linear && road == Road::Host;
		CHECK(c.engine.pendingFieldsForTest() == (waits ? 1u : 0u));
		if (waits) {
			CHECK(c.engine.holdsTail());
		}
		for (int b = 0; b < static_cast<int>(kL / 100) + 20; ++b) {
			// THE MAIN THREAD'S BEAT, every block: the host's write reaches the record, and every drain writes the record
			// back — which must not land the write early while it waits (the host-slot rule, kept while it waits).
			(void)c.top->serviceHostSlots();
			c.run(100);
		}
		CHECK(c.engine.pendingFieldsForTest() == 0u);
		return gLog.levelAt;
	};
	for (const Road road : { Road::Edit, Road::Host }) {
		CAPTURE(static_cast<int>(road));
		const int64_t early = landedAt(road, false);
		const int64_t late = landedAt(road, true);
		REQUIRE(early >= 0);
		REQUIRE(late >= 0);
		CHECK(early == 1000);
		CHECK(late - early == (road == Road::Host ? static_cast<int64_t>(kL) : 0));
	}
}

TEST_CASE("LW step 3: HARM behind a linear-phase EQ is handed the song's clock as it stood eq::linearLatency ago — running on between, and a locate arriving exactly that much later") {
	const auto run = [](bool linear, double* lagSeconds) {
		Seat seat;
		Chain c;
		const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
		(void)c.add(kProbe);
		c.top->setField(eq, kPhase, linear ? 1.0f : 0.0f);
		c.run(64);
		gLog = ProbeLog {};
		gLog.jumpBelow = 1.0;
		// PLAYING AT 120, from 10 s; at 20000 samples the host locates to 0 s and plays on from there.
		uint64_t t = 0;
		const auto clockAt = [](uint64_t at) {
			device::TransportClock k = device::TransportClock::fromSecondsPerBeat(0.5);
			k.playing = true;
			const double seconds = at < 20000 ? 10.0 + static_cast<double>(at) / kRate : static_cast<double>(at - 20000) / kRate;
			k.songPositionSeconds = seconds;
			k.songPositionBeats = seconds / 0.5;
			return k;
		};
		while (t < 20000 + kL + 2000) {
			c.run(100, {}, backplane::BlockContext { clockAt(t) });
			t += 100;
			// RUNNING ON BETWEEN, a while after the device turned late and before the locate: its clock is the host's L ago.
			if (t == 15000) {
				*lagSeconds = clockAt(gLog.clockFrame).songPositionSeconds - gLog.clock.songPositionSeconds;
			}
		}
		return gLog.jumpAt;
	};
	double lagEarly = 0.0;
	double lagLate = 0.0;
	const int64_t early = run(false, &lagEarly);
	const int64_t late = run(true, &lagLate);
	REQUIRE(early >= 0);
	REQUIRE(late >= 0);
	CHECK(late - early == static_cast<int64_t>(kL));
	CHECK(std::abs(lagEarly) < 1e-9);
	CHECK(std::abs(lagLate - static_cast<double>(kL) / kRate) < 1e-9);
}

//------------------------------------------------------------------------------------------------//
// STEP 4 — MODULATION READ THROUGH A RING PER (SOURCE, LATE DOMAIN)                                //
//------------------------------------------------------------------------------------------------//

namespace {

size_t cutoffIndex() {
	const auto& desc = fx::filter::kDescriptor;
	size_t c = 0;
	while (c < desc.params.size() && (!desc.params[c].modulatable || static_cast<int>(c) == desc.traits.enableControl)) {
		++c;
	}
	REQUIRE(c < desc.params.size());
	return c;
}

/// A DAHDSR's output routed into a Filter's cutoff, the Filter behind an EQ (linear or not); a note at block 10. The
/// Filter's resolved cutoff, one reading per 64-sample block — one per control tick.
std::vector<float> envelopeOnFilter(bool linear, size_t* rings = nullptr) {
	Chain c;
	const DeviceInstanceId env = c.add("DHDS");
	const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
	const DeviceInstanceId filter = c.add(fx::filter::kTraits.tag);
	const DeviceInstanceId filter2 = c.add(fx::filter::kTraits.tag);
	c.top->setField(eq, kPhase, linear ? 1.0f : 0.0f);
	const size_t cutoff = cutoffIndex();
	c.top->setRouteAmount(backplane::Source::output(env, 0), backplane::Destination::control(filter, cutoff), 0.5f);
	c.top->setRouteAmount(backplane::Source::output(env, 0), backplane::Destination::control(filter2, cutoff), -0.25f);
	for (int b = 0; b < 4; ++b) {
		c.run(64);
	}
	std::vector<float> seen;
	const size_t at = c.engine.slotOf(filter);
	for (int b = 0; b < 600; ++b) {
		std::vector<device::NoteEvent> ev;
		if (b == 10) {
			ev.push_back(note(device::NoteEvent::Kind::On, 0));
		}
		if (b == 40) {
			ev.push_back(note(device::NoteEvent::Kind::Off, 0));
		}
		c.run(64, ev);
		seen.push_back(c.engine.resolvedFieldForTest(at, cutoff));
	}
	if (rings != nullptr) {
		*rings = c.engine.modRingsInUseForTest();
	}
	return seen;
}

} // namespace

TEST_CASE("LW step 4: an envelope routed to a Filter behind a linear-phase EQ moves the Filter's resolved cutoff exactly eq::linearLatency later — the same values, tick for tick") {
	REQUIRE(kL % device::kTickWindow == 0);
	const size_t ticks = kL / device::kTickWindow; // 144
	size_t ringsEarly = 99;
	size_t ringsLate = 99;
	const std::vector<float> early = envelopeOnFilter(false, &ringsEarly);
	const std::vector<float> late = envelopeOnFilter(true, &ringsLate);
	// NOT VACUOUS: the envelope moved the cutoff on time, and the late one had not moved by then.
	size_t movedEarly = early.size();
	for (size_t k = 1; k < early.size(); ++k) {
		if (early[k] != early[0]) {
			movedEarly = k;
			break;
		}
	}
	REQUIRE(movedEarly < early.size());
	CHECK(late[movedEarly] == late[0]);
	// THE SAME TRAJECTORY, `ticks` LATER, value for value.
	size_t matched = 0;
	for (size_t k = 0; k + ticks < late.size(); ++k) {
		CAPTURE(k);
		CHECK(late[k + ticks] == early[k]);
		matched += late[k + ticks] == early[k] ? 1 : 0;
	}
	CHECK(matched == late.size() - ticks);
	// ONE RING FOR THE PAIR (the DAHDSR, the EQ's domain), shared by both routes into it; none on time.
	CHECK(ringsLate == 1u);
	CHECK(ringsEarly == 0u);
}

TEST_CASE("LW step 4: nested — a Filter inside a Chain behind an EQ, the Chain behind an EQ, reads the host's envelope 2 × eq::linearLatency late") {
	const auto trajectory = [](bool linear, bool innerLinear) {
		Chain c;
		const DeviceInstanceId env = c.add("DHDS");
		const DeviceInstanceId outerEq = c.add(fx::eq::kTraits.tag);
		const DeviceInstanceId chain = c.add(composite::kTraits.tag);
		rack::RackBridge& in = inside(*c.top, chain);
		const DeviceInstanceId innerEq = c.add(in, fx::eq::kTraits.tag);
		const DeviceInstanceId filter = c.add(in, fx::filter::kTraits.tag);
		c.top->setField(outerEq, kPhase, linear ? 1.0f : 0.0f);
		in.setField(innerEq, kPhase, innerLinear ? 1.0f : 0.0f);
		const size_t cutoff = cutoffIndex();
		const backplane::Source outer = backplane::Source::output(env, 0).asOuter();
		const backplane::Destination dest = backplane::Destination::control(filter, cutoff);
		REQUIRE(in.mayRoute(outer, dest));
		in.setRouteAmount(outer, dest, 0.5f);
		for (int b = 0; b < 4; ++b) {
			c.run(64);
		}
		std::vector<float> seen;
		for (int b = 0; b < 700; ++b) {
			std::vector<device::NoteEvent> ev;
			if (b == 10) {
				ev.push_back(note(device::NoteEvent::Kind::On, 0));
			}
			if (b == 40) {
				ev.push_back(note(device::NoteEvent::Kind::Off, 0));
			}
			c.run(64, ev);
			const rack::RackEngine& e = in.engineForTest();
			seen.push_back(e.resolvedFieldForTest(e.slotOf(filter), cutoff));
		}
		return seen;
	};
	const size_t ticks = 2 * kL / device::kTickWindow;
	const std::vector<float> early = trajectory(false, false);
	const std::vector<float> late = trajectory(true, true);
	{
		const auto first = [](const std::vector<float>& v) {
			size_t k = 0;
			while (k < v.size() && v[k] == v[0]) {
				++k;
			}
			return k;
		};
		// EACH STEP ALONE, ONE eq::linearLatency: the host's EQ before the Chain, and the Chain's own EQ before the Filter.
		const size_t onTime = first(early);
		const size_t one = kL / device::kTickWindow;
		CHECK(first(trajectory(true, false)) == onTime + one);
		CHECK(first(trajectory(false, true)) == onTime + one);
	}
	REQUIRE(early.size() == late.size());
	bool movedEarly = false;
	for (const float v : early) {
		movedEarly = movedEarly || v != early[0];
	}
	REQUIRE(movedEarly);
	size_t mismatched = 0;
	for (size_t k = 0; k + ticks < late.size(); ++k) {
		mismatched += late[k + ticks] == early[k] ? 0 : 1;
	}
	CHECK(mismatched == 0u);
}

TEST_CASE("LW step 4, the voice road: HARM running per voice behind a per-voice Spectral is handed its voice's note exactly the Spectral's latency late") {
	const auto onsetOf = [](bool spectralPoly, uint32_t* perVoice, uint32_t* busArrival) {
		Seat seat;
		ParamSet params;
		Synth synth { params };
		ParamBridge bridge;
		synth.activate(kRate, 4096);
		synth.setCatalogue(hostCatalogue());
		std::vector<float> l(100, 0.0f);
		std::vector<float> r(100, 0.0f);
		const auto blocks = [&](int n) {
			for (int i = 0; i < n; ++i) {
				publishEditorLanes(bridge, params, synth);
				(void)synth.provisionDevices(bridge.chainState(), bridge.publishedChains().sequence(), &bridge.fxBodies());
				synth.render(l.data(), r.data(), 100);
			}
		};
		const size_t spectralSlot = bridge.addFxDevice("spectral");
		REQUIRE(spectralSlot < kMaxFxSlots);
		const size_t probeSlot = bridge.addFxDevice(kProbe);
		REQUIRE(probeSlot < kMaxFxSlots);
		const size_t busSlot = bridge.addFxDevice(fx::filter::kTraits.tag); // on the bus: after every voice
		REQUIRE(busSlot < kMaxFxSlots);
		bridge.refresh();
		bridge.setDeviceControl(bridge.fxDeviceAt(spectralSlot), static_cast<size_t>(fx::spectral::Param::Poly),
		                        spectralPoly ? 1.0f : 0.0f);
		bridge.setDeviceControl(bridge.fxDeviceAt(probeSlot), static_cast<size_t>(fx::harm::Param::Poly), 1.0f);
		blocks(6);
		*perVoice = synth.laneForTest().perVoiceLatencySamples();
		const rack::RackEngine& e = synth.laneForTest().engineForTest();
		*busArrival = e.arrivalForTest(e.slotOf(bridge.fxDeviceAt(busSlot)));
		gLog = ProbeLog {};
		synth.noteOn(0, 60, 1, 1.0);
		blocks(41);
		synth.noteOff(0, 60, 1, 0.0);
		blocks(60);
		return gLog;
	};
	uint32_t none = 99;
	uint32_t road = 0;
	uint32_t busEarly = 99;
	uint32_t busLate = 0;
	const ProbeLog early = onsetOf(false, &none, &busEarly);
	const ProbeLog late = onsetOf(true, &road, &busLate);
	REQUIRE(none == 0u);
	REQUIRE(road > 0u);
	// A BUS DEVICE IS LATE BY THE WHOLE PER-VOICE ROAD — every voice reaches the bus that late — and, with the Spectral
	// on the bus instead, by the Spectral there.
	CHECK(busLate == road);
	CHECK(busEarly == road);
	REQUIRE(early.onset >= 0);
	REQUIRE(late.onset >= 0);
	// COUNTED IN THE VOICE'S OWN SAMPLES (a voice instance runs only while its voice sounds): on time, the gate rises as
	// the voice starts; behind the Spectral, exactly its latency later, and the release likewise.
	CHECK(early.onset == 0);
	CHECK(late.onset == static_cast<int64_t>(road));
	REQUIRE(early.release > 0);
	REQUIRE(late.release > 0);
	CHECK(late.release - early.release == static_cast<int64_t>(road));
}

TEST_CASE("LW step 2: inside a Chain, behind a device whose latency is off the control tick, HARM is handed its note exactly that latency late — the Chain cuts its own chunks there") {
	for (const bool inRack : { false, true }) {
	CAPTURE(inRack);
	const auto run = [inRack](const char* before, uint32_t* late) {
		Seat seat;
		Chain c;
		// A CHAIN (`rack_device`'s chunk loop) or one of a Rack's chains (`InnerChain`'s): each cuts its own.
		rack::RackBridge* holder = nullptr;
		if (inRack) {
			const DeviceInstanceId rackId = c.add(rack::parallel::kTraits.tag);
			rack::parallel::Rack* rk = rack::parallel::rackOf(c.top->containerSurfaceOf(rackId));
			REQUIRE(rk != nullptr);
			if (rk->chainCount() == 0) {
				(void)rk->addChain();
				c.run(64);
			}
			holder = rk->chainAt(0)->bridge();
		}
		else {
			holder = &inside(*c.top, c.add(composite::kTraits.tag));
		}
		REQUIRE(holder != nullptr);
		rack::RackBridge& in = *holder;
		(void)c.add(in, before);
		const DeviceInstanceId h = c.add(in, kProbe);
		c.run(64);
		c.run(64);
		*late = c.arrival(in.engineForTest(), h);
		gLog = ProbeLog {};
		c.play(1000, 3000, 1000 + 3000 + 4096);
		return gLog;
	};
	uint32_t none = 99;
	uint32_t a = 0;
	const ProbeLog early = run(fx::filter::kTraits.tag, &none);
	const ProbeLog late = run("spectral", &a);
	REQUIRE(none == 0u);
	REQUIRE(a > 0u);
	REQUIRE(a % device::kTickWindow != 0); // off the tick: the inner chunk grid alone cannot land it
	REQUIRE(early.onset >= 0);
	REQUIRE(late.onset >= 0);
	CHECK(late.onset - early.onset == static_cast<int64_t>(a));
	CHECK(late.release - early.release == static_cast<int64_t>(a));
	}
}

TEST_CASE("LW: a history fuller than it holds — notes 4 samples apart behind the linear EQ, 2304 changes in its window — drops whole notes only, counts every one, and leaves nothing sounding") {
	// THE RULE (`RackEngine::makeRoomInHistory`, provisional): moments no late device can read go first, then changes
	// under a held gate merge, then whole notes go — an on with the off after it, never an off alone.
	const auto play = [](bool linear, uint64_t* dropped, uint64_t* merged) {
		Seat seat;
		Chain c;
		const DeviceInstanceId eq = c.add(fx::eq::kTraits.tag);
		(void)c.add(kProbe);
		c.top->setField(eq, kPhase, linear ? 1.0f : 0.0f);
		c.run(64);
		gLog = ProbeLog {};
		// A NOTE EVERY 8 SAMPLES, HELD FOR 4: 4-sample host blocks, on and off in turn, for 3 × the EQ's window.
		size_t notes = 0;
		for (uint32_t b = 0; b < 3 * kL / 4; ++b) {
			const bool on = b % 2 == 0;
			c.run(4, { note(on ? device::NoteEvent::Kind::On : device::NoteEvent::Kind::Off, 0) });
			notes += on ? 1 : 0;
		}
		// THEN QUIET FOR LONGER THAN THE WINDOW: whatever the late device still owed has reached it.
		for (int b = 0; b < static_cast<int>(kL / 256) + 8; ++b) {
			c.run(256);
		}
		*dropped = c.engine.lateNotesDroppedForTest();
		*merged = c.engine.lateMergesForTest();
		CHECK_FALSE(gLog.gate); // NOTHING LEFT SOUNDING
		CHECK_FALSE(c.engine.holdsTail());
		return std::pair { notes, gLog.rises };
	};
	uint64_t droppedEarly = 9;
	uint64_t mergedEarly = 9;
	uint64_t dropped = 0;
	uint64_t merged = 0;
	const auto [sentEarly, playedEarly] = play(false, &droppedEarly, &mergedEarly);
	const auto [sent, played] = play(true, &dropped, &merged);
	REQUIRE(2 * kL / 4 > rack::RackEngine::kNoteHistory); // the window really does hold more changes than the history
	// ON TIME: every note played, nothing given up.
	CHECK(playedEarly == sentEarly);
	CHECK(droppedEarly == 0u);
	CHECK(mergedEarly == 0u);
	// LATE: some whole notes given up, and EVERY ONE COUNTED — played and dropped add up to what was sent, exactly.
	CHECK(dropped > 0u);
	CHECK(merged == 0u); // strictly on, off, on: nothing changes under a held gate to merge
	CHECK(played + dropped == sent);
}
