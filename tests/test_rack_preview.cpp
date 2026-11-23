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
 */

// LATENCY — backlog JM 1–3. A device declares how late it is; an engine
// sums that along its serial path; a rack device and an inner chain delay the dry they mix against by the
// same count; and both plugins report the total through `clap_plugin_latency`, asking the host for a restart
// when it moves while active. Every claim here is a VALUE — a count, an index, an amplitude — never "the
// render differs".
//
// THE DEVICE THAT DECLARES A LATENCY IS WRITTEN HERE, because none ships yet (the EQ's linear phase will be
// the first): `latN` is a pure delay by its `Latency` control, and it declares exactly that.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"
#include "portable_aligned_alloc.h"

#include "fx/drum/drum.h"
#include "fx/drum/drum_types.h"
#include "plugin/feathers_test_chain.h"
#include "plugin/ui/drum_device_ui.h"
#include "plugin/ui_note_ring.h"
#include "rack/rack_bridge.h"
#include "rack/rack_clap.h"
#include "rack/rack_test_ext.h"

#include "clap/clap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

// THE STANDALONE RACK'S ROAD FOR AN EDITOR KEY (a Drum's play button): `RackBridge::postNote` -> the audio thread's drain at the top of a block ->
// the engine's notes at offset 0, under the reserved `kUiNoteId`, never emitted to the host. Mirrors the synth plugin's cases in test_clap_render.
// Its own binary: it replaces operator new to count allocations, only while armed.

namespace {
struct Allocs {
	bool armed = false;
	size_t allocations = 0;
};
Allocs allocs;
void* countedAlloc(size_t bytes, size_t align) {
	if (allocs.armed) {
		++allocs.allocations;
	}
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}
} // namespace

void* operator new(size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new[](size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new(size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void* operator new[](size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"

using namespace feathers;
namespace dd = feathers::fx::drumdevice;

namespace {

struct Host {
	int processRequests = 0;
	clap_host host {};
	Host() {
		host.clap_version = CLAP_VERSION_INIT;
		host.host_data = this;
		host.name = "preview-venue";
		host.vendor = "Owlet";
		host.url = "";
		host.version = "1";
		host.get_extension = [](const clap_host*, const char*) -> const void* { return nullptr; };
		host.request_restart = [](const clap_host*) {};
		host.request_process = [](const clap_host* h) { ++static_cast<Host*>(h->host_data)->processRequests; };
		host.request_callback = [](const clap_host*) {};
	}
};

/// THE STANDALONE RACK holding one Drum, activated, with the output port's notes captured.
struct Rack {
	Host host;
	const clap_plugin* plugin = nullptr;
	std::vector<float> l = std::vector<float>(256, 0.0f);
	std::vector<float> r = std::vector<float>(256, 0.0f);
	std::vector<clap_event_note> out; ///< every note event the plugin handed the host's output port
	std::vector<clap_event_note> hostNotes;

	Rack() {
		out.reserve(4096); // the capture is the venue's, not the plugin's: it must not allocate inside an armed block
		plugin = rack::create(&host.host);
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
		REQUIRE(rackTest()->add_device(plugin, dd::kTraits.tag) != 0);
		REQUIRE(plugin->activate(plugin, 48000.0, 1, 256));
		REQUIRE(plugin->start_processing(plugin));
		run(); // the block whose drain adopts the device
	}
	~Rack() {
		plugin->stop_processing(plugin);
		plugin->deactivate(plugin);
		plugin->destroy(plugin);
	}
	Rack(const Rack&) = delete;
	Rack& operator=(const Rack&) = delete;

	const clap_plugin_feathers_rack_test* rackTest() const {
		return static_cast<const clap_plugin_feathers_rack_test*>(plugin->get_extension(plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	}
	rack::RackBridge& bridge() const { return *static_cast<rack::RackBridge*>(rackTest()->bridge(plugin)); }

	/// One block; `hostNote` sends a host note-on of key 40 (id 7), `hostEvent` a host event of `type` on `hostKey` (id 9) instead.
	clap_process_status run(bool hostNote = false, int hostEvent = -1, int16_t hostKey = 36) {
		clap_audio_buffer buf {};
		float* ptr[2] { l.data(), r.data() };
		buf.data32 = ptr;
		buf.channel_count = 2;
		struct In {
			bool note;
			clap_event_note e {};
		} in { hostNote || hostEvent >= 0 };
		in.e.header.size = sizeof(in.e);
		in.e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		in.e.header.type = static_cast<uint16_t>(hostEvent >= 0 ? hostEvent : CLAP_EVENT_NOTE_ON);
		in.e.header.time = 0;
		in.e.note_id = hostEvent >= 0 ? 9 : 7;
		in.e.key = hostEvent >= 0 ? hostKey : 40;
		in.e.velocity = hostEvent == CLAP_EVENT_NOTE_OFF ? 0.0 : 0.75;
		clap_input_events events {};
		events.ctx = &in;
		events.size = [](const clap_input_events* list) -> uint32_t { return static_cast<const In*>(list->ctx)->note ? 1u : 0u; };
		events.get = [](const clap_input_events* list, uint32_t) -> const clap_event_header* { return &static_cast<const In*>(list->ctx)->e.header; };
		clap_output_events sink {};
		sink.ctx = this;
		sink.try_push = [](const clap_output_events* list, const clap_event_header* h) {
			if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE)) {
				static_cast<Rack*>(list->ctx)->out.push_back(*reinterpret_cast<const clap_event_note*>(h));
			}
			return true;
		};
		clap_process p {};
		p.frames_count = static_cast<uint32_t>(l.size());
		p.audio_outputs = &buf;
		p.audio_outputs_count = 1;
		p.in_events = &events;
		p.out_events = &sink;
		return plugin->process(plugin, &p);
	}
	[[nodiscard]] float peak() const {
		float m = 0.0f;
		for (size_t i = 0; i < l.size(); ++i) {
			m = std::max({ m, std::fabs(l[i]), std::fabs(r[i]) });
		}
		return m;
	}
};

/// The drum device driven directly with its default key struck at sample 0: the reference a preview must equal, bit for bit.
std::vector<float> referenceHit(size_t frames) {
	const device::AudioOps& a = dd::kAudioOps;
	std::vector<std::byte> storage(a.stateBytes + a.stateAlign);
	void* st = storage.data();
	std::size_t space = storage.size();
	st = std::align(a.stateAlign, a.stateBytes, st, space);
	REQUIRE(st != nullptr);
	a.construct(st);
	a.prepare(st, 48000.0, 256);
	dd::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(dd::kParamDescs[i].def);
	}
	a.setFields(st, row.data(), row.size());
	device::NoteEvent e;
	e.key = 36;
	e.velocity = 0.75; // exactly representable as a float, so the ring's float is the same number
	device::NoteEvent out[2];
	size_t n = 0;
	dd::kNoteOps.processNotes(st, &e, 1, out, 2, &n);
	std::vector<float> ref(frames, 0.0f);
	std::vector<float> r(frames, 0.0f);
	a.processStereo(st, ref.data(), r.data(), static_cast<int>(frames));
	a.destruct(st);
	return ref;
}

} // namespace

TEST_CASE("rack plugin: a key the editor posts, with no host event at all, sounds the Drum at its sample 0, equal to the device struck directly; the rack never promises SLEEP") {
	Rack rack;
	REQUIRE(rack.bridge().canPostNote());
	CHECK(rack.run() == CLAP_PROCESS_CONTINUE);
	CHECK(rack.peak() == 0.0f); // silence first: nothing posted, nothing heard
	const std::vector<float> ref = referenceHit(rack.l.size());
	REQUIRE(rack.bridge().postNote(36, true, 0.75f));
	CHECK(rack.run() == CLAP_PROCESS_CONTINUE);
	bool same = true;
	for (size_t i = 0; i < ref.size(); ++i) {
		same = same && rack.l[i] == ref[i];
	}
	CHECK(rack.peak() > 1.0e-3f);
	CHECK(same);
	// ANOTHER KEY SOUNDS NOTHING: the Drum answers its own Note alone.
	Rack other;
	REQUIRE(other.bridge().postNote(41, true, 0.8f));
	other.run();
	CHECK(other.peak() == 0.0f);
}

TEST_CASE("rack plugin: a posted key and its release equal a host note-on and note-off at the same blocks, bit for bit (a Gate drum, so the off matters), and a key outside 0..127 is refused") {
	const auto render = [](bool fromEditor) {
		Rack rack;
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(rack.plugin->get_extension(rack.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		REQUIRE(chain->set_control(rack.plugin, 0, "Trigger", 1.0)); // Gate: a released key damps the hit
		REQUIRE(chain->set_control(rack.plugin, 0, "Release", -6.0));
		rack.run();
		std::vector<float> all;
		const auto take = [&] { all.insert(all.end(), rack.l.begin(), rack.l.end()); };
		if (fromEditor) {
			REQUIRE(rack.bridge().postNote(36, true, 0.75f));
			rack.run();
		}
		else {
			rack.run(false, CLAP_EVENT_NOTE_ON, 36);
		}
		take();
		if (fromEditor) {
			REQUIRE(rack.bridge().postNote(36, false, 0.0f));
			rack.run();
		}
		else {
			rack.run(false, CLAP_EVENT_NOTE_OFF, 36);
		}
		take();
		for (int block = 0; block < 5; ++block) {
			rack.run();
			take();
		}
		return all;
	};
	const std::vector<float> editor = render(true);
	const std::vector<float> host = render(false);
	REQUIRE(editor.size() == host.size());
	CHECK(std::any_of(editor.begin(), editor.end(), [](float v) { return v != 0.0f; }));
	CHECK(editor == host);
	Rack rack;
	CHECK_FALSE(rack.bridge().postNote(128, true, 0.8f));
	CHECK_FALSE(rack.bridge().postNote(-1, true, 0.8f));
}

TEST_CASE("rack plugin: a full ring drops and counts, a block drains it without a crash, and a block with posted keys allocates nothing") {
	Rack rack;
	for (size_t i = 0; i < UiNoteRing::kCapacity; ++i) {
		REQUIRE(rack.bridge().postNote(36, (i % 2) == 0, 0.8f));
	}
	CHECK_FALSE(rack.bridge().postNote(36, true, 0.8f));
	CHECK_FALSE(rack.bridge().postNote(36, true, 0.8f));
	CHECK(rack.bridge().droppedNotes() == 2);
	allocs.allocations = 0;
	allocs.armed = true;
	rack.run();
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
	CHECK_FALSE(rack.bridge().hasPendingNotes()); // all of it taken in one block
	// A BLOCK WITH A HOST NOTE AND KEYS TOGETHER, still no allocation.
	REQUIRE(rack.bridge().postNote(36, true, 0.8f));
	allocs.allocations = 0;
	allocs.armed = true;
	rack.run(true);
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
}

TEST_CASE("rack plugin: nothing with the reserved id leaves on the output port, while a host note's own id still does") {
	Rack rack;
	REQUIRE(rack.bridge().postNote(36, true, 0.8f));
	rack.run(true); // and a host note with id 7, on key 40
	rack.run();
	REQUIRE(rack.bridge().postNote(36, false, 0.0f));
	rack.run();
	bool sawHost = false;
	for (const clap_event_note& n : rack.out) {
		CHECK(n.note_id != kUiNoteId);
		sawHost = sawHost || (n.note_id == 7 && n.key == 40);
	}
	CHECK(sawHost); // the capture works: the host's note is there
}


