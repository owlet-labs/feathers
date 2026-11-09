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
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "backplane/axes.h"
#include "backplane/block_context.h"
#include "rack/rack_axes.h"
#include "rack/rack_engine.h"
#include "rack_endpoints.h"

#include <array>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// THE RACK ENGINE RUNS FEATHERS' PER-VOICE ARRANGEMENT ITSELF — step 1 of moving the fx lane onto it.
//
// Feathers renders a device whose Poly switch is on INSIDE EACH VOICE before the sum, and every other
// device once on the summed bus. That lived on a second runner (`FxRunner`'s per-voice instance), which
// is one of the reasons the lane and the rack were two chains. These cases pin the engine's own version:
// an instance per voice beside the bus instance, living and dying on the same road as the bus one, run in
// chain order, flipped without rebuilding.
//
// EVERY ASSERTION READS A VALUE — which instance ran, what note it was told, what number came out — never
// "the output differs", which a partial fault survives.

using namespace feathers;
using rack::RackEngine;

//------------------------------------------------------------------------------------------------//
// THE ALLOCATION PROBE — counting only when armed, frees as well as allocations, because the drain  //
// may do neither.                                                                                    //
//------------------------------------------------------------------------------------------------//

namespace {

struct Allocs {
	bool armed = false;
	size_t allocations = 0;
	size_t frees = 0;
};
Allocs allocs;

void* countedAlloc(size_t bytes, size_t align) {
	if (allocs.armed) {
		++allocs.allocations;
	}
	// An aligned request may ask for less than a pointer: see portable_aligned_alloc.h.
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}

void countedFree(void* p) noexcept {
	if (p != nullptr && allocs.armed) {
		++allocs.frees;
	}
	feathers::test::heapFree(p);
}

} // namespace

void* operator new(size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new[](size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new(size_t bytes, std::align_val_t align) {
	return countedAlloc(bytes, static_cast<size_t>(align));
}
void* operator new[](size_t bytes, std::align_val_t align) {
	return countedAlloc(bytes, static_cast<size_t>(align));
}
void operator delete(void* p) noexcept { countedFree(p); }
void operator delete[](void* p) noexcept { countedFree(p); }
void operator delete(void* p, size_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t) noexcept { countedFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { countedFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { countedFree(p); }
#include "nothrow_new_forwarding.h"

namespace {

//------------------------------------------------------------------------------------------------//
// THE PROBE — a device whose every effect is a number a case can read.                              //
//------------------------------------------------------------------------------------------------//

int gConstructed = 0;
int gDestructed = 0;

/// y = x * gain + offset, and it remembers what it was told. `tail` stands in for a delay line: it is
/// what a reset clears and what a flip must not replay.
struct Probe {
	float poly = 0.0f;
	float gain = 1.0f;
	float offset = 0.0f;
	float pitch = -1.0f;
	bool gate = false;
	double rate = 0.0;
	int processed = 0;
	int resets = 0;
	float tail = 0.0f;
	/// A HAND'S BLOCK (`device::HandBlock`), pulled by the engine into every instance, bus and voices alike.
	device::HandBlock<2> hand;
	device::HandBlockCopy<2> handCopy;
	std::array<float, 2> handScratch {};
};

Probe& probeAt(void* storage) { return *static_cast<Probe*>(storage); }

void run(Probe& p, float* x, int frames) {
	for (int i = 0; i < frames; ++i) {
		x[i] = x[i] * p.gain + p.offset;
		p.tail = x[i];
	}
}

constexpr device::AudioOps kProbeOps {
	.stateBytes = sizeof(Probe),
	.stateAlign = alignof(Probe),
	.construct = [](void* s) {
		new (s) Probe {};
		++gConstructed;
	},
	.destruct = [](void* s) {
		static_cast<Probe*>(s)->~Probe();
		++gDestructed;
	},
	.prepare = [](void* s, double rate, int) { probeAt(s).rate = rate; },
	.reset = [](void* s) {
		probeAt(s).tail = 0.0f;
		++probeAt(s).resets;
	},
	.setSlot = nullptr,
	.setFields = [](void* s, const float* f, size_t n) {
		Probe& p = probeAt(s);
		p.poly = n > 0 ? f[0] : 0.0f;
		p.gain = n > 1 ? f[1] : 1.0f;
		p.offset = n > 2 ? f[2] : 0.0f;
	},
	.setNoteContext = [](void* s, const device::NoteContext& note) {
		probeAt(s).pitch = note.pitchSemitones;
		probeAt(s).gate = note.gate;
	},
	.processStereo = [](void* s, float* l, float* r, int frames) {
		Probe& p = probeAt(s);
		run(p, l, frames);
		run(p, r, frames);
		++p.processed;
	},
	.processMono = [](void* s, float* mono, int frames) {
		Probe& p = probeAt(s);
		run(p, mono, frames);
		++p.processed;
	},
	.pullHandBlocks = [](void* s, const void* source) {
		(void)probeAt(s).handCopy.pull(static_cast<const Probe*>(source)->hand, probeAt(s).handScratch);
	},
};

constexpr std::array<device::ParamDesc, 3> kProbeParams { {
	{ "poly", "Poly", 0.0, 1.0, 0.0, true, device::Unit::Number, {}, /*modulatable=*/true },
	{ "gain", "Gain", 0.0, 8.0, 1.0, false, device::Unit::Number, {} },
	{ "offset", "Offset", -100.0, 100.0, 0.0, false, device::Unit::Number, {} },
} };

/// A DEVICE WITH A PER-VOICE ROAD: its control 0 is the Poly switch.
const device::Descriptor kPoly {
	.traits = { .tag = "prbV", .name = "Probe V", .hasCard = false, .hasExpanded = false,
	            .perVoiceControl = 0 },
	.params = kProbeParams,
	.outputs = {},
	.audio = &kProbeOps,
};

/// THE SAME DEVICE WITH NO PER-VOICE ROAD — its control 0 is a setting and nothing more.
const device::Descriptor kBus {
	.traits = { .tag = "prbB", .name = "Probe B", .hasCard = false, .hasExpanded = false,
	            .perVoiceControl = -1 },
	.params = kProbeParams,
	.outputs = {},
	.audio = &kProbeOps,
};

bool applyProbeHand(void* s, size_t index, const float* in, size_t n) {
	if (index != 0 || n != 2) {
		return false;
	}
	probeAt(s).hand.write(in, n);
	return true;
}
constexpr device::StateBlockDesc kHandBlocks[] = { { .key = "hand", .floats = 2, .handEdited = true } };
constexpr device::StateOps kHandState { .blocks = kHandBlocks, .apply = &applyProbeHand };
/// THE PER-VOICE DEVICE WITH A HAND'S BLOCK.
const device::Descriptor kPolyHand {
	.traits = { .tag = "prbH", .name = "Probe H", .hasCard = false, .hasExpanded = false, .perVoiceControl = 0 },
	.params = kProbeParams,
	.outputs = {},
	.audio = &kProbeOps,
	.state = &kHandState,
};

const device::Descriptor* findProbe(void*, const char* tag) {
	if (device::sameTag(tag, kPoly.traits.tag)) {
		return &kPoly;
	}
	if (device::sameTag(tag, kPolyHand.traits.tag)) {
		return &kPolyHand;
	}
	if (device::sameTag(tag, kBus.traits.tag)) {
		return &kBus;
	}
	return nullptr;
}

const device::Catalogue kProbes { nullptr, &findProbe };

Probe& busOf(const RackEngine& e, size_t lane) {
	return probeAt(e.slotAtLane(lane).storage.get());
}

Probe& voiceOf(const RackEngine& e, size_t lane, size_t voice) {
	return probeAt(e.slotAtLane(lane).voices.at(voice).get());
}

device::NoteContext noteAt(float pitch) {
	device::NoteContext n;
	n.pitchSemitones = pitch;
	n.gate = true;
	return n;
}

constexpr int kFrames = 8;
const backplane::BlockContext kBlock = backplane::unknownBlock();

} // namespace

TEST_CASE("a rack with voices builds an instance per voice for a device with a per-voice road, and "
          "none for one without") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(4);
	e.prepare(48000.0, 4096);
	const size_t poly = e.addDevice("prbV");
	const size_t bus = e.addDevice("prbB");
	REQUIRE(poly != RackEngine::kNoSlot);
	REQUIRE(bus != RackEngine::kNoSlot);
	CHECK(e.slotAtLane(poly).voices.size() == 4);
	CHECK(e.slotAtLane(bus).voices.empty());
	// AND EVERY ONE OF THEM PREPARED AT THE RATE THE BUS ONE WAS — a voice instance built without a
	// step the bus one had would run at a rate of zero.
	for (size_t v = 0; v < 4; ++v) {
		CHECK(voiceOf(e, poly, v).rate == 48000.0);
	}
	e.prepare(96000.0, 4096);
	CHECK(busOf(e, poly).rate == 96000.0);
	for (size_t v = 0; v < 4; ++v) {
		CHECK(voiceOf(e, poly, v).rate == 96000.0);
	}
}

TEST_CASE("a rack with no voices runs a Poly device on the bus") {
	// THE STANDALONE RACK: no voices, so the switch is a setting and the device runs once on the sum.
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 1.0f); // Poly on
	e.setField(lane, 1, 3.0f);
	CHECK(e.slotAtLane(lane).voices.empty());

	std::array<float, kFrames> l {};
	std::array<float, kFrames> r {};
	l.fill(0.5f);
	r.fill(0.25f);
	e.processStereo(l.data(), r.data(), kFrames, kBlock);
	CHECK_FALSE(e.polyForTest(lane));
	CHECK(busOf(e, lane).processed == 1);
	CHECK(l[kFrames - 1] == doctest::Approx(1.5f));
	CHECK(r[kFrames - 1] == doctest::Approx(0.75f));
}

TEST_CASE("Poly on: each voice runs its own instance with its own note, and the bus instance does "
          "not run") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(3);
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 1.0f);
	e.setField(lane, 1, 2.0f);

	std::array<float, kFrames> voice0 {};
	std::array<float, kFrames> voice2 {};
	std::array<float, kFrames> busL {};
	std::array<float, kFrames> busR {};
	voice0.fill(1.0f);
	voice2.fill(0.25f);
	busL.fill(0.5f);
	busR.fill(0.5f);

	e.beginBlock(kFrames, kBlock);
	e.processVoice(0, voice0.data(), nullptr, kFrames, noteAt(60.0f));
	e.processVoice(2, voice2.data(), nullptr, kFrames, noteAt(72.0f));
	e.processBus(busL.data(), busR.data(), kFrames);
	e.endBlock();

	CHECK(e.polyForTest(lane));
	CHECK(e.anyPerVoice());
	// WHICH INSTANCE RAN, AND WHAT IT WAS TOLD.
	CHECK(voiceOf(e, lane, 0).processed == 1);
	CHECK(voiceOf(e, lane, 0).pitch == 60.0f);
	CHECK(voiceOf(e, lane, 2).processed == 1);
	CHECK(voiceOf(e, lane, 2).pitch == 72.0f);
	CHECK(voiceOf(e, lane, 1).processed == 0);
	CHECK(busOf(e, lane).processed == 0);
	// AND THE SETTINGS REACHED EVERY VOICE, including the one that did not sound.
	CHECK(voiceOf(e, lane, 1).gain == 2.0f);
	// WHAT CAME OUT — the mono road, since the voice handed no right channel.
	CHECK(voice0[kFrames - 1] == doctest::Approx(2.0f));
	CHECK(voice2[kFrames - 1] == doctest::Approx(0.5f));
	// THE BUS WAS NOT TOUCHED BY A DEVICE THAT RAN INSIDE THE VOICES.
	CHECK(busL[kFrames - 1] == 0.5f);
	CHECK(busR[kFrames - 1] == 0.5f);
}

TEST_CASE("Poly off: the bus instance runs and the voices do not") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 1, 4.0f);

	std::array<float, kFrames> voice {};
	std::array<float, kFrames> busL {};
	std::array<float, kFrames> busR {};
	voice.fill(1.0f);
	busL.fill(0.5f);
	busR.fill(0.25f);

	e.beginBlock(kFrames, kBlock);
	e.processVoice(0, voice.data(), nullptr, kFrames, noteAt(60.0f));
	e.processBus(busL.data(), busR.data(), kFrames);
	e.endBlock();

	CHECK_FALSE(e.polyForTest(lane));
	CHECK_FALSE(e.anyPerVoice());
	CHECK(voiceOf(e, lane, 0).processed == 0);
	CHECK(busOf(e, lane).processed == 1);
	CHECK(voice[kFrames - 1] == 1.0f);
	CHECK(busL[kFrames - 1] == doctest::Approx(2.0f));
	CHECK(busR[kFrames - 1] == doctest::Approx(1.0f));
}

TEST_CASE("the voice pass runs the per-voice devices in chain order, and the bus pass the rest") {
	// A → B → C, where A and C run per voice and B has no per-voice road. y = x·gain + offset does not
	// commute, so the ORDER is in the number: A then C gives (x·2)+10; C then A would give (x+10)·2.
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(1);
	e.prepare(48000.0, 4096);
	const size_t a = e.addDevice("prbV");
	const size_t b = e.addDevice("prbB");
	const size_t c = e.addDevice("prbV");
	REQUIRE(c != RackEngine::kNoSlot);
	e.setField(a, 0, 1.0f);
	e.setField(a, 1, 2.0f);
	e.setField(b, 2, 1.0f); // B adds one, on the bus
	e.setField(c, 0, 1.0f);
	e.setField(c, 2, 10.0f);

	std::array<float, kFrames> voice {};
	std::array<float, kFrames> busL {};
	std::array<float, kFrames> busR {};
	voice.fill(3.0f);
	e.beginBlock(kFrames, kBlock);
	e.processVoice(0, voice.data(), nullptr, kFrames, noteAt(60.0f));
	// THE HOST SUMS THE VOICES INTO THE BUS — one voice, so the sum is the voice.
	busL = voice;
	busR = voice;
	e.processBus(busL.data(), busR.data(), kFrames);
	e.endBlock();

	CHECK(voice[kFrames - 1] == doctest::Approx(16.0f)); // (3·2)+10
	CHECK(busL[kFrames - 1] == doctest::Approx(17.0f));  // + 1 from B, after the sum
	CHECK(busOf(e, b).processed == 1);
	CHECK(voiceOf(e, a, 0).processed == 1);
	CHECK(voiceOf(e, c, 0).processed == 1);

	// AND A REORDER IS A DIFFERENT NUMBER — C before A: (3+10)·2.
	REQUIRE(e.moveDevice(e.positionOfLane(c), 0).has_value());
	voice.fill(3.0f);
	e.beginBlock(kFrames, kBlock);
	e.processVoice(0, voice.data(), nullptr, kFrames, noteAt(60.0f));
	e.processBus(busL.data(), busR.data(), kFrames);
	e.endBlock();
	CHECK(voice[kFrames - 1] == doctest::Approx(26.0f));
}

TEST_CASE("flipping Poly resets the instances that stop being responsible, and only those") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);

	std::array<float, kFrames> voice {};
	std::array<float, kFrames> busL {};
	std::array<float, kFrames> busR {};
	const auto block = [&](bool poly) {
		e.setField(lane, 0, poly ? 1.0f : 0.0f);
		voice.fill(1.0f);
		busL.fill(1.0f);
		busR.fill(1.0f);
		e.beginBlock(kFrames, kBlock);
		e.processVoice(0, voice.data(), nullptr, kFrames, noteAt(60.0f));
		e.processVoice(1, voice.data(), nullptr, kFrames, noteAt(64.0f));
		e.processBus(busL.data(), busR.data(), kFrames);
		e.endBlock();
	};

	block(false);
	CHECK(busOf(e, lane).resets == 0);
	CHECK(busOf(e, lane).tail == 1.0f);

	// ON: the bus instance stops being responsible — its tail goes.
	block(true);
	CHECK(busOf(e, lane).resets == 1);
	CHECK(busOf(e, lane).tail == 0.0f);
	CHECK(voiceOf(e, lane, 0).resets == 0);
	CHECK(voiceOf(e, lane, 1).tail == 1.0f);

	// STILL ON: no flip, no reset — a reset every block would be a device that never rings.
	block(true);
	CHECK(busOf(e, lane).resets == 1);
	CHECK(voiceOf(e, lane, 0).resets == 0);

	// OFF: the voices stop being responsible — both of them, the tail each held goes.
	block(false);
	CHECK(voiceOf(e, lane, 0).resets == 1);
	CHECK(voiceOf(e, lane, 1).resets == 1);
	CHECK(voiceOf(e, lane, 1).tail == 0.0f);
	CHECK(busOf(e, lane).resets == 1);
}

TEST_CASE("a new note resets that voice's instances and no other voice's") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(3);
	e.prepare(48000.0, 4096);
	const size_t a = e.addDevice("prbV");
	const size_t b = e.addDevice("prbV");
	REQUIRE(b != RackEngine::kNoSlot);
	e.resetVoice(1);
	CHECK(voiceOf(e, a, 1).resets == 1);
	CHECK(voiceOf(e, b, 1).resets == 1);
	CHECK(voiceOf(e, a, 0).resets == 0);
	CHECK(voiceOf(e, b, 2).resets == 0);
	CHECK(busOf(e, a).resets == 0);
}

TEST_CASE("a removed Poly device's voice instances go the bus instance's road: parked by the drain, "
          "destructed by the reaper, never inside a block") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(4);
	e.prepare(48000.0, 4096);
	gConstructed = 0;
	gDestructed = 0;
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	CHECK(gConstructed == 5);

	// A BLOCK IN FLIGHT: begun, not ended.
	e.beginBlock(kFrames, kBlock);
	allocs = { .armed = true };
	REQUIRE(e.removeDeviceAtLane(lane));
	allocs.armed = false;
	// THE DRAIN NEITHER ALLOCATES NOR FREES — the voices were swapped into the parking, not assigned.
	CHECK(allocs.allocations == 0);
	CHECK(allocs.frees == 0);
	CHECK(e.slotAtLane(lane).voices.empty());

	// THE BLOCK MAY BE INSIDE ANY OF THEM, so the reaper must wait.
	e.reapRetired();
	CHECK(gDestructed == 0);

	std::array<float, kFrames> busL {};
	std::array<float, kFrames> busR {};
	e.processBus(busL.data(), busR.data(), kFrames);
	e.endBlock();
	e.reapRetired();
	CHECK(gDestructed == 5);
}

TEST_CASE("the drain's add and remove of a Poly device neither allocate nor free, on parking that has "
          "been used before") {
	// **A FRESH PARKING ENTRY CANNOT CATCH AN ASSIGNMENT WHERE A SWAP BELONGS** — an empty vector with no
	// buffer frees nothing when assigned over. So the ring goes round twice first: after that the entry
	// the removal parks into holds a buffer the reaper emptied, and the slot the add adopts into holds
	// one the last removal handed back. An assignment over either frees it, on the drain.
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(4);
	e.prepare(48000.0, 4096);
	for (size_t lap = 0; lap < 2 * e.retiredCapacityForTest(); ++lap) {
		const size_t lane = e.addDevice("prbV");
		REQUIRE(lane != RackEngine::kNoSlot);
		REQUIRE(e.removeDeviceAtLane(lane));
		e.reapRetired();
	}
	// PROVISIONED WHERE ALLOCATION IS ALLOWED...
	RackEngine::Provision p = e.provision("prbV");
	REQUIRE(p.voices.size() == 4);
	CHECK(e.slotAtLane(0).voices.capacity() > 0); // the case is not vacuous: there is a buffer to free
	// ...ADOPTED AND REMOVED ON THE DRAIN.
	allocs = { .armed = true };
	const size_t lane = e.adoptDevice(0, std::move(p));
	const bool removed = lane != RackEngine::kNoSlot && e.removeDeviceAtLane(lane);
	allocs.armed = false;
	REQUIRE(lane == 0);
	REQUIRE(removed);
	CHECK(allocs.allocations == 0);
	CHECK(allocs.frees == 0);
}

TEST_CASE("a route onto the Poly switch moves the device between the bus and the voices") {
	// THE RESOLVED ROW DECIDES, as `FxRunner` synced the modulated one — so the switch is a destination
	// like any other, and the base stays where the hand put it.
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(1);
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	const backplane::Destination dest = test::ordinalOf(e, lane, kPoly.nthModulatableOfControl(0));
	e.setRouteAmount(test::macro(0), dest, 1.0f);

	std::array<float, kFrames> busL {};
	std::array<float, kFrames> busR {};
	e.processStereo(busL.data(), busR.data(), kFrames, kBlock);
	CHECK_FALSE(e.polyForTest(lane));

	e.setMacro(0, 1.0f);
	e.processStereo(busL.data(), busR.data(), kFrames, kBlock);
	CHECK(e.polyForTest(lane));
	CHECK(e.fieldAt(lane, 0) == 0.0f);
}

TEST_CASE("a route onto a stepped control lands it on a step — the landing rule Feathers has always used") {
	// `device::withRouteOffset`: the rack did not round a stepped control, so a switch at 0.3 of a step
	// was handed to the device as 0.3 — a value the control cannot take. The switch here is the probe's
	// Poly, stepped and modulatable.
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	REQUIRE(kPoly.params[0].stepped);
	const backplane::Destination dest = test::ordinalOf(e, lane, kPoly.nthModulatableOfControl(0));
	std::array<float, kFrames> l {};
	std::array<float, kFrames> r {};
	e.setMacro(0, 1.0f);
	e.setRouteAmount(test::macro(0), dest, 0.3f);
	e.processStereo(l.data(), r.data(), kFrames, kBlock);
	CHECK(e.resolvedFieldForTest(lane, 0) == 0.0f);
	e.setRouteAmount(test::macro(0), dest, 0.6f);
	e.processStereo(l.data(), r.data(), kFrames, kBlock);
	CHECK(e.resolvedFieldForTest(lane, 0) == 1.0f);
	CHECK(busOf(e, lane).poly == 1.0f); // and that is what the device was handed
}

TEST_CASE("an engine destroyed with Poly devices in it destructs every voice instance, live or parked") {
	gConstructed = 0;
	gDestructed = 0;
	{
		RackEngine e { rack::kRackAxes, kProbes };
		e.setVoices(3);
		e.prepare(48000.0, 4096);
		REQUIRE(e.addDevice("prbV") != RackEngine::kNoSlot);
		const size_t parked = e.addDevice("prbV");
		REQUIRE(parked != RackEngine::kNoSlot);
		// PARKED AND NEVER REAPED — the destructor empties the parking whatever the epoch says.
		e.beginBlock(kFrames, kBlock);
		REQUIRE(e.removeDeviceAtLane(parked));
		CHECK(gConstructed == 8);
		CHECK(gDestructed == 0);
	}
	CHECK(gDestructed == 8);
}

TEST_CASE("a provision that dies unadopted destructs every device it built, bus and voices") {
	// A BYTE ARRAY'S DELETER RUNS NO DESTRUCTOR. Before this a refused add freed its device's bytes and
	// leaked whatever the device owned — and a rack in a slot owns a whole engine.
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	gConstructed = 0;
	gDestructed = 0;
	{
		RackEngine::Provision p = e.provision("prbV");
		REQUIRE(p.live());
		CHECK(gConstructed == 3);
		// MOVE-ASSIGNED OVER, the old contents go first — the lane road's displaced provision.
		p = e.provision("prbB");
		CHECK(gConstructed == 4);
		CHECK(gDestructed == 3);
	}
	CHECK(gDestructed == 4);
	// AND AN ADOPTED ONE LEAVES A HUSK THAT DESTRUCTS NOTHING — the device is the slot's now.
	gConstructed = 0;
	gDestructed = 0;
	{
		RackEngine::Provision p = e.provision("prbV");
		REQUIRE(e.adoptDevice(0, std::move(p)) == 0);
	}
	CHECK(gConstructed == 3);
	CHECK(gDestructed == 0);
	CHECK(e.slotAtLane(0).voices.size() == 2);
}

TEST_CASE("endBlock ends the block even when no bus pass ran, so a removal is not parked forever") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	gConstructed = 0;
	gDestructed = 0;
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	e.beginBlock(kFrames, kBlock);
	REQUIRE(e.removeDeviceAtLane(lane));
	e.processBus(nullptr, nullptr, 0);
	e.endBlock();
	e.reapRetired();
	CHECK(gDestructed == 3);
}

TEST_CASE("a lane reused after a Poly device left takes the new device's voices, not the old one's") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	const size_t first = e.addDevice("prbV");
	REQUIRE(first != RackEngine::kNoSlot);
	REQUIRE(e.removeDeviceAtLane(first));
	e.reapRetired();
	// A DEVICE WITH NO PER-VOICE ROAD ON THE SAME LANE HAS NO VOICES — nothing of the last one's.
	const size_t second = e.addDevice("prbB");
	REQUIRE(second == first);
	CHECK(e.slotAtLane(second).voices.empty());
	REQUIRE(e.removeDeviceAtLane(second));
	e.reapRetired();
	// AND A POLY ONE GETS ITS OWN, freshly built and prepared.
	gConstructed = 0;
	const size_t third = e.addDevice("prbV");
	REQUIRE(third == first);
	CHECK(gConstructed == 3);
	CHECK(e.slotAtLane(third).voices.size() == 2);
	CHECK(voiceOf(e, third, 1).processed == 0);
	CHECK(voiceOf(e, third, 1).rate == 48000.0);
}

TEST_CASE("the held-note stack never allocates on the audio thread, however many notes are held") {
	// FOUND BY A LYING NOTE DEVICE IN FEATHERS' STRIP: the engine kept its held keys in a vector it
	// pushed onto from the block's note pass, so the first time more notes were held than ever before,
	// the audio thread allocated — and a stream of note-ons with no offs grew it without bound. Bounded
	// now: reserved at construction, and a full stack drops its OLDEST key to make room.
	RackEngine e { rack::kRackAxes, kProbes };
	e.prepare(48000.0, 4096);
	std::array<device::NoteEvent, RackEngine::kMaxNotes> ons {};
	for (size_t i = 0; i < ons.size(); ++i) {
		ons[i] = { device::NoteEvent::Kind::On, 0, static_cast<int32_t>(i), 0, 0,
			       static_cast<int16_t>(i % 128), 1.0 };
	}
	std::array<float, kFrames> l {};
	std::array<float, kFrames> r {};
	allocs = { .armed = true };
	for (int block = 0; block < 4; ++block) { // 4 x 256 note-ons, never released
		e.setBlockNotes(ons.data(), ons.size());
		e.processStereo(l.data(), r.data(), kFrames, kBlock);
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
	CHECK(allocs.frees == 0);
}

TEST_CASE("the voice pass allocates nothing") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(2);
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbV");
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 1.0f);
	std::array<float, kFrames> voice {};
	std::array<float, kFrames> busL {};
	std::array<float, kFrames> busR {};
	allocs = { .armed = true };
	for (int i = 0; i < 4; ++i) {
		e.setField(lane, 0, (i & 1) != 0 ? 1.0f : 0.0f); // and flipping, which resets
		e.beginBlock(kFrames, kBlock);
		e.processVoice(0, voice.data(), nullptr, kFrames, noteAt(60.0f));
		e.processVoice(1, voice.data(), nullptr, kFrames, noteAt(62.0f));
		e.resetVoice(0);
		e.processBus(busL.data(), busR.data(), kFrames);
		e.endBlock();
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
	CHECK(allocs.frees == 0);
}

TEST_CASE("a hand's block applied to the bus instance reaches EVERY voice instance by the engine's pull, value by value") {
	RackEngine e { rack::kRackAxes, kProbes };
	e.setVoices(3);
	e.prepare(48000.0, 4096);
	const size_t lane = e.addDevice("prbH");
	REQUIRE(lane != RackEngine::kNoSlot);
	e.setField(lane, 0, 1.0f); // Poly on: the voices run it, the bus instance does not
	std::array<float, kFrames> v {};
	std::array<float, kFrames> busL {}, busR {};
	const auto block = [&] {
		v.fill(1.0f);
		e.beginBlock(kFrames, kBlock);
		e.processVoice(0, v.data(), nullptr, kFrames, noteAt(60.0f));
		e.processBus(busL.data(), busR.data(), kFrames);
		e.endBlock();
	};
	block();
	for (size_t i = 0; i < 3; ++i) {
		CHECK(voiceOf(e, lane, i).handCopy.serial == 0); // nothing written: nothing taken
	}
	const std::array<float, 2> drawn { 3.25f, -7.5f };
	REQUIRE(kHandState.apply(e.slotAtLane(lane).storage.get(), 0, drawn.data(), drawn.size()));
	// A WRITE IS NOT HEARD UNTIL THE AUDIO THREAD'S NEXT BLOCK:
	CHECK(voiceOf(e, lane, 1).handCopy.values[0] == 0.0f);
	block();
	for (size_t i = 0; i < 3; ++i) {
		CAPTURE(i);
		CHECK(voiceOf(e, lane, i).handCopy.values == drawn); // every voice, the silent ones too
	}
	// AN UNCHANGED BLOCK TAKES NOTHING AGAIN (the serial is the identity, not the address).
	const uint64_t serial = voiceOf(e, lane, 0).handCopy.serial;
	block();
	CHECK(voiceOf(e, lane, 0).handCopy.serial == serial);
}
