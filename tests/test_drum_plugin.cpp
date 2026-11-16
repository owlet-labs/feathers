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

#include "clap/clap.h"

#include "device/format_value.h"
#include "fx/drum/drum.h"
#include "fx/drum/drum_types.h"
#include "plugin/device_registry.h"
#include "plugin/feathers_test_chain.h"
#include "plugin/ui/drum_device_expanded.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/plugin_window.h"
#include "plugin/ui/widget_registry.h"
#include "portable_aligned_alloc.h"
#include "rack/drum_plugin.h"
#include "rack/rack_bridge.h"
#include "rack/rack_clap.h"
#include "rack/rack_entry.h"
#include "rack/rack_gui.h"
#include "rack/rack_test_ext.h"
#include "state/state_trailer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

// FEATHERS DRUM — THE DRUM DEVICE AS A CLAP INSTRUMENT, driven through the real CLAP API (create by id, ports, params, notes, audio,
// state, editor) the way test_rack_clap drives the rack. The reference for the sound is the device itself, run directly.

using namespace feathers;
namespace dd = feathers::fx::drumdevice;
namespace dp = feathers::rack::drumplugin;

// OL (asan-triage 2026-10-19): the gate builds with -UNDEBUG, where visage's InstanceCounter<Window> asserts at static destruction that
// no Window is alive, and `PluginWindow` abandons dead windows on purpose (backlog GP). Release them once every case has run.
namespace {
struct ReleaseAbandonedWindows : doctest::IReporter {
	ReleaseAbandonedWindows(const doctest::ContextOptions&) {}
	void report_query(const doctest::QueryData&) override {}
	void test_run_start() override {}
	void test_run_end(const doctest::TestRunStats&) override { feathers::ui::PluginWindow::releaseAbandonedForTest(); }
	void test_case_start(const doctest::TestCaseData&) override {}
	void test_case_reenter(const doctest::TestCaseData&) override {}
	void test_case_end(const doctest::CurrentTestCaseStats&) override {}
	void test_case_exception(const doctest::TestCaseException&) override {}
	void subcase_start(const doctest::SubcaseSignature&) override {}
	void subcase_end() override {}
	void log_assert(const doctest::AssertData&) override {}
	void log_message(const doctest::MessageData&) override {}
	void test_case_skipped(const doctest::TestCaseData&) override {}
};
}
REGISTER_LISTENER("release-abandoned-windows", 1, ReleaseAbandonedWindows);

// ALLOCATIONS ARE COUNTED THE WAY test_clap_render COUNTS THEM: the global operators are replaced and the count is read either side of
// a process() call.
namespace {
std::atomic<long> gAllocations { 0 };
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(size_t size) {
	gAllocations.fetch_add(1, std::memory_order_relaxed);
	void* p = feathers::test::heapAlloc(size == 0 ? 1 : size);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size) {
	return ::operator new(size);
}
void* operator new(size_t size, std::align_val_t alignment) {
	gAllocations.fetch_add(1, std::memory_order_relaxed);
	void* p = feathers::test::portableAlignedAlloc(size, static_cast<size_t>(alignment));
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](size_t size, std::align_val_t alignment) {
	return ::operator new(size, alignment);
}
void operator delete(void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

/// The smallest host clap-helpers accepts (timer support is offered, as `guiCreate` insists on one).
clap_host makeHost() {
	clap_host h {};
	h.clap_version = CLAP_VERSION_INIT;
	h.name = "drum-venue";
	h.vendor = "Owlet";
	h.url = "";
	h.version = "1";
	h.get_extension = [](const clap_host*, const char* id) -> const void* {
		static const clap_host_timer_support kTimer {
			[](const clap_host*, uint32_t, clap_id* out) {
				*out = 1;
				return true;
			},
			[](const clap_host*, clap_id) { return true; },
		};
		if (id != nullptr && std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) {
			return &kTimer;
		}
		return nullptr;
	};
	h.request_restart = [](const clap_host*) {};
	h.request_process = [](const clap_host*) {};
	h.request_callback = [](const clap_host*) {};
	return h;
}

/// ONE BLOCK'S INPUT EVENTS, any mix of notes and parameter writes, in the order pushed.
struct Events {
	std::vector<std::vector<unsigned char>> raw;
	clap_input_events in {};

	Events() {
		in.ctx = this;
		in.size = [](const clap_input_events* l) -> uint32_t { return static_cast<uint32_t>(static_cast<const Events*>(l->ctx)->raw.size()); };
		in.get = [](const clap_input_events* l, uint32_t i) -> const clap_event_header* {
			return reinterpret_cast<const clap_event_header*>(static_cast<const Events*>(l->ctx)->raw[i].data());
		};
	}
	Events(const Events&) = delete;
	Events& operator=(const Events&) = delete;

	void note(uint16_t type, uint32_t time, int16_t key, int32_t id, double velocity = 1.0) {
		clap_event_note e {};
		e.header.size = sizeof(e);
		e.header.time = time;
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = type;
		e.note_id = id;
		e.key = key;
		e.velocity = velocity;
		push(&e, sizeof(e));
	}
	void param(clap_id id, double value, uint32_t time = 0) {
		clap_event_param_value e {};
		e.header.size = sizeof(e);
		e.header.time = time;
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = CLAP_EVENT_PARAM_VALUE;
		e.param_id = id;
		e.value = value;
		e.note_id = -1;
		e.port_index = -1;
		e.channel = -1;
		e.key = -1;
		push(&e, sizeof(e));
	}
	void clear() { raw.clear(); }

private:
	void push(const void* p, size_t bytes) {
		raw.emplace_back(static_cast<const unsigned char*>(p), static_cast<const unsigned char*>(p) + bytes);
	}
};

clap_output_events sinkEvents() {
	clap_output_events out {};
	out.ctx = nullptr;
	out.try_push = [](const clap_output_events*, const clap_event_header*) { return true; };
	return out;
}

/// An output-only instrument, created by id from the bundle's factory, initialised and activated.
struct Venue {
	clap_host host = makeHost();
	const clap_plugin* plugin = nullptr;
	std::vector<float> l, r;
	clap_audio_buffer out {};
	float* ptr[2] {};
	long allocations = 0;

	explicit Venue(uint32_t maxFrames = 512, double rate = 48000.0) : l(maxFrames), r(maxFrames) {
		const auto* factory = static_cast<const clap_plugin_factory*>(rack::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
		REQUIRE(factory != nullptr);
		plugin = factory->create_plugin(factory, &host, dp::kPluginId);
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
		REQUIRE(plugin->activate(plugin, rate, 1, maxFrames));
		REQUIRE(plugin->start_processing(plugin));
		ptr[0] = l.data();
		ptr[1] = r.data();
		out.data32 = ptr;
		out.channel_count = 2;
	}
	~Venue() {
		if (plugin != nullptr) {
			plugin->stop_processing(plugin);
			plugin->deactivate(plugin);
			plugin->destroy(plugin);
		}
	}

	[[nodiscard]] const clap_plugin_params* params() const {
		return static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
	}
	[[nodiscard]] const clap_plugin_feathers_rack_test* test() const {
		return static_cast<const clap_plugin_feathers_rack_test*>(plugin->get_extension(plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	}
	[[nodiscard]] rack::RackBridge& bridge() const { return *static_cast<rack::RackBridge*>(test()->bridge(plugin)); }

	/// ONE BLOCK OF `frames` (output buffers zeroed first, as a host may hand them dirty: they are not read).
	clap_process_status run(uint32_t frames, const Events* events = nullptr, std::vector<float>* keepL = nullptr) {
		static const Events none;
		clap_output_events sink = sinkEvents();
		clap_process p {};
		p.frames_count = frames;
		p.audio_inputs = nullptr;
		p.audio_inputs_count = 0;
		p.audio_outputs = &out;
		p.audio_outputs_count = 1;
		p.in_events = events != nullptr ? &events->in : &none.in;
		p.out_events = &sink;
		std::fill(l.begin(), l.end(), 0.7f); // dirty on purpose: an instrument must overwrite, not add to, what it is handed
		std::fill(r.begin(), r.end(), 0.7f);
		const long before = gAllocations.load(std::memory_order_relaxed);
		const clap_process_status status = plugin->process(plugin, &p);
		allocations = gAllocations.load(std::memory_order_relaxed) - before;
		if (keepL != nullptr) {
			keepL->insert(keepL->end(), l.begin(), l.begin() + frames);
		}
		return status;
	}
	/// The main-thread visit a host's `request_callback` brings: the slots' mirror and the dirty mark.
	void mainThread() { plugin->on_main_thread(plugin); }

	[[nodiscard]] float peak(uint32_t frames) const {
		float p = 0.0f;
		for (uint32_t i = 0; i < frames; ++i) {
			p = std::max({ p, std::fabs(l[i]), std::fabs(r[i]) });
		}
		return p;
	}
	[[nodiscard]] bool silent(uint32_t frames) const {
		for (uint32_t i = 0; i < frames; ++i) {
			if (l[i] != 0.0f || r[i] != 0.0f) {
				return false;
			}
		}
		return true;
	}
	[[nodiscard]] clap_id idOf(const char* name) const {
		const uint32_t n = params()->count(plugin);
		for (uint32_t i = 0; i < n; ++i) {
			clap_param_info info {};
			REQUIRE(params()->get_info(plugin, i, &info));
			if (std::string(info.name).find(name) != std::string::npos) {
				return info.id;
			}
		}
		FAIL("no parameter named " << name);
		return CLAP_INVALID_ID;
	}
};

const device::Descriptor& drumDesc() {
	static const device::Descriptor* d = [] {
		for (const device::Descriptor& desc : kDevices) {
			if (std::strcmp(desc.traits.tag, dd::kTraits.tag) == 0) {
				return &desc;
			}
		}
		return static_cast<const device::Descriptor*>(nullptr);
	}();
	REQUIRE(d != nullptr);
	return *d;
}

size_t controlOfKey(const char* key) {
	const device::Descriptor& d = drumDesc();
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (std::strcmp(d.params[i].key, key) == 0) {
			return i;
		}
	}
	return d.params.size();
}

/// THE DEVICE ITSELF, RUN DIRECTLY ON ONE NOTE AT SAMPLE 0 (the reference every render below is held against, bit for bit).
std::vector<float> referenceHit(size_t frames) {
	std::vector<float> l(frames, 0.0f), r(frames, 0.0f);
	const device::AudioOps& a = dd::kAudioOps;
	std::vector<std::byte> storage(a.stateBytes + a.stateAlign);
	void* st = storage.data();
	size_t space = storage.size();
	st = std::align(a.stateAlign, a.stateBytes, st, space);
	REQUIRE(st != nullptr);
	a.construct(st);
	a.prepare(st, 48000.0, 512);
	dd::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(dd::kParamDescs[i].def);
	}
	row[static_cast<size_t>(dd::Param::Keys)] = 1.0f; // THE PLUGIN'S DRUM ANSWERS ANY KEY
	a.setFields(st, row.data(), row.size());
	device::NoteEvent e;
	e.key = 36;
	device::NoteEvent out[2];
	size_t n = 0;
	dd::kNoteOps.processNotes(st, &e, 1, out, 2, &n);
	a.processStereo(st, l.data(), r.data(), static_cast<int>(frames));
	a.destruct(st);
	return l;
}

/// Run until the plugin sleeps (or `limit` blocks), returning how many blocks it took; -1 if it never did.
int blocksToSleep(Venue& v, uint32_t frames, int limit) {
	for (int b = 0; b < limit; ++b) {
		if (v.run(frames) == CLAP_PROCESS_SLEEP) {
			return b;
		}
	}
	return -1;
}

} // namespace

//---------------------------------------------------------------------------------------------------------------------------------
// THE BUNDLE
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("the factory lists the Drum beside the two existing plugins, whose ids and names are unchanged, and creates each by id") {
	const auto* factory = static_cast<const clap_plugin_factory*>(rack::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
	REQUIRE(factory != nullptr);
	REQUIRE(factory->get_plugin_count(factory) == 4);
	const char* const ids[3] = { "com.owlet.feathers-chain", "com.owlet.feathers.noterack", "com.owlet.feathers.drum" };
	const char* const names[3] = { "Feathers Chain", "Feathers Note Rack", "Feathers Drum" };
	for (uint32_t i = 0; i < 3; ++i) {
		const clap_plugin_descriptor* d = factory->get_plugin_descriptor(factory, i);
		REQUIRE(d != nullptr);
		CHECK(std::string(d->id) == ids[i]);
		CHECK(std::string(d->name) == names[i]);
		clap_host host = makeHost();
		const clap_plugin* p = factory->create_plugin(factory, &host, ids[i]);
		REQUIRE(p != nullptr);
		CHECK(std::string(p->desc->id) == ids[i]);
		p->destroy(p);
	}
	CHECK(factory->get_plugin_descriptor(factory, 4) == nullptr); // the Rack is the fourth (test_rack_plugin)
	clap_host host = makeHost();
	CHECK(factory->create_plugin(factory, &host, "com.owlet.feathers.nothing") == nullptr);
	// THE EXISTING ONES' ACCESSORS ANSWER THE SAME DESCRIPTORS.
	CHECK(std::string(rack::descriptor()->id) == ids[0]);
	CHECK(std::string(rack::noteRackDescriptor()->id) == ids[1]);
	CHECK(std::string(rack::drumDescriptor()->id) == ids[2]);
}

TEST_CASE("the host-parameter list is append-only: the first 54 places are what shipped to this point, and Keys follows them") {
	// A FINGERPRINT OF THE FIRST 54 KEYS IN ORDER (FNV-1a over each key and its NUL): moving, removing or inserting before any of them
	// changes it, and a host's lanes live on those ids. New controls are appended after, and this number stays.
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < 54; ++i) {
		for (const char* c = dp::kHostKeys[i];; ++c) {
			h ^= static_cast<unsigned char>(*c);
			h *= 0x100000001b3ull;
			if (*c == '\0') {
				break;
			}
		}
	}
	CHECK(h == 0x4015774dbc430875ull);
	REQUIRE(dp::kHostParamCount >= 55);
	CHECK(std::string(dp::kHostKeys[54]) == "drum_keys");
}

TEST_CASE("a new drum answers Any Key, and Keys is a host parameter that reaches Pitched") {
	Venue v;
	const clap_id keys = v.idOf("Keys");
	CHECK(keys == 0x1000 + 54);
	double value = -1.0;
	REQUIRE(v.params()->get_value(v.plugin, keys, &value));
	CHECK(value == 0.5); // Any Key of One Key | Any Key | Pitched
	Events e;
	e.param(keys, 1.0);
	v.run(128, &e);
	v.mainThread();
	CHECK(v.test()->resolved_field(v.plugin, v.bridge().deviceAtPosition(0), controlOfKey("drum_keys")) == 2.0f);
}

TEST_CASE("the Drum declares itself an instrument and a drum, with notes in, stereo audio out and nothing else") {
	const clap_plugin_descriptor* d = rack::drumDescriptor();
	std::set<std::string> features;
	for (const char* const* f = d->features; *f != nullptr; ++f) {
		features.insert(*f);
	}
	CHECK(features.count(CLAP_PLUGIN_FEATURE_INSTRUMENT) == 1);
	CHECK(features.count(CLAP_PLUGIN_FEATURE_DRUM) == 1);
	CHECK(features.count(CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0);
	CHECK(features.count(CLAP_PLUGIN_FEATURE_NOTE_EFFECT) == 0);

	Venue v;
	const auto* audio = static_cast<const clap_plugin_audio_ports*>(v.plugin->get_extension(v.plugin, CLAP_EXT_AUDIO_PORTS));
	REQUIRE(audio != nullptr);
	CHECK(audio->count(v.plugin, true) == 0);
	REQUIRE(audio->count(v.plugin, false) == 1);
	clap_audio_port_info port {};
	REQUIRE(audio->get(v.plugin, 0, false, &port));
	CHECK(port.channel_count == 2);
	CHECK((port.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0);
	CHECK(std::string(port.port_type) == CLAP_PORT_STEREO);
	const auto* notes = static_cast<const clap_plugin_note_ports*>(v.plugin->get_extension(v.plugin, CLAP_EXT_NOTE_PORTS));
	REQUIRE(notes != nullptr);
	CHECK(notes->count(v.plugin, true) == 1);
	CHECK(notes->count(v.plugin, false) == 0);
	clap_note_port_info note {};
	REQUIRE(notes->get(v.plugin, 0, true, &note));
	CHECK((note.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0);
}

//---------------------------------------------------------------------------------------------------------------------------------
// SOUND
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("a note sounds from its own sample, bit for bit the device's own hit, whatever key it arrives on") {
	constexpr uint32_t kAt = 100;
	const std::vector<float> ref = referenceHit(512);
	int refFirst = -1;
	for (size_t i = 0; i < ref.size() && refFirst < 0; ++i) {
		refFirst = ref[i] != 0.0f ? static_cast<int>(i) : -1;
	}
	REQUIRE(refFirst >= 0);
	// THE DEFAULT DRUM ANSWERS KEY 36 AS A PAD; IN A HOST'S DRUM CONTAINER THE PAD'S OWN NOTE ARRIVES, WHATEVER IT IS.
	for (const int16_t key : { int16_t { 36 }, int16_t { 60 }, int16_t { 0 }, int16_t { 127 } }) {
		Venue v;
		Events e;
		e.note(CLAP_EVENT_NOTE_ON, kAt, key, 7);
		REQUIRE(v.run(512, &e) == CLAP_PROCESS_CONTINUE);
		for (uint32_t i = 0; i < kAt; ++i) {
			REQUIRE(v.l[i] == 0.0f);
			REQUIRE(v.r[i] == 0.0f);
		}
		bool same = true;
		for (size_t i = 0; i + kAt < v.l.size(); ++i) {
			same = same && v.l[kAt + i] == ref[i];
		}
		CAPTURE(static_cast<int>(key));
		CHECK(same);
		CHECK(v.peak(512) > 1.0e-3f);
	}
}

TEST_CASE("SLEEP is promised only over exact silence with nothing left to ring: a hit's tail, and a hit still waiting its Delay, keep it awake") {
	constexpr uint32_t kFrames = 256;
	{
		Venue v;
		// ASLEEP BEFORE ANYTHING PLAYS: nothing sounds and nothing is owed.
		CHECK(blocksToSleep(v, kFrames, 8) >= 0);
		Events e;
		e.note(CLAP_EVENT_NOTE_ON, 0, 36, 1);
		CHECK(v.run(kFrames, &e) == CLAP_PROCESS_CONTINUE);
		e.clear();
		// THE PROMISE, EVERY BLOCK: SLEEP never follows a sounding block, and the block after a SLEEP is silent.
		int blocks = 1;
		bool slept = false;
		for (; blocks < 20 * 48000 / static_cast<int>(kFrames) && !slept; ++blocks) {
			const clap_process_status status = v.run(kFrames);
			if (status == CLAP_PROCESS_SLEEP) {
				CHECK(v.silent(kFrames));
				v.run(kFrames);
				CHECK(v.silent(kFrames));
				slept = true;
			}
		}
		CHECK(slept);
		CHECK(blocks > 48000 / static_cast<int>(kFrames) / 8); // the hit rang for a good while before the plugin slept
	}
	{
		// A HIT WAITING ITS DELAY is exactly silent and still to come: the plugin must not sleep over it (the delay's-repeats shape).
		// The Delay's longest is 10 ms (480 samples), so blocks of 64 leave several silent ones before the strike.
		constexpr uint32_t kSmall = 64;
		Venue v;
		REQUIRE(blocksToSleep(v, kSmall, 8) >= 0);
		const clap_id delay = v.idOf("Delay");
		Events e;
		e.param(delay, 1.0);
		v.run(kSmall, &e);
		e.clear();
		e.note(CLAP_EVENT_NOTE_ON, 0, 36, 1);
		bool awakeWhileSilent = false;
		bool firstSound = false;
		for (int b = 0; b < 40 && !firstSound; ++b) {
			const clap_process_status status = v.run(kSmall, b == 0 ? &e : nullptr);
			if (!v.silent(kSmall)) {
				firstSound = true;
			}
			else {
				CHECK(status == CLAP_PROCESS_CONTINUE); // never SLEEP over a hit that has not struck yet
				awakeWhileSilent = true;
			}
		}
		CHECK(awakeWhileSilent); // the Delay really did hold the strike back, or this proved nothing
		CHECK(firstSound);
	}
}

TEST_CASE("a note's samples are the same at any host block size") {
	constexpr uint32_t kTotal = 6144;
	const auto render = [&](uint32_t block) {
		Venue v(1024);
		std::vector<float> out;
		uint32_t at = 0;
		// A STREAM OF NOTES AT ABSOLUTE SAMPLES, NOT AT BLOCK EDGES: on, a second on a different key, off, a choke.
		struct Ev {
			uint32_t t;
			uint16_t type;
			int16_t key;
			int32_t id;
		};
		const Ev stream[] = { { 130, CLAP_EVENT_NOTE_ON, 36, 1 }, { 1500, CLAP_EVENT_NOTE_ON, 38, 2 },
			                  { 1530, CLAP_EVENT_NOTE_OFF, 36, 1 }, { 3001, CLAP_EVENT_NOTE_ON, 40, 3 },
			                  { 4097, CLAP_EVENT_NOTE_CHOKE, 40, 3 } };
		while (at < kTotal) {
			const uint32_t frames = std::min(block, kTotal - at);
			Events e;
			for (const Ev& ev : stream) {
				if (ev.t >= at && ev.t < at + frames) {
					e.note(ev.type, ev.t - at, ev.key, ev.id);
				}
			}
			v.run(frames, &e, &out);
			at += frames;
		}
		return out;
	};
	const std::vector<float> whole = render(1024);
	REQUIRE(whole.size() == kTotal);
	float peak = 0.0f;
	for (float s : whole) {
		peak = std::max(peak, std::fabs(s));
	}
	REQUIRE(peak > 1.0e-3f);
	for (const uint32_t block : { 32u, 37u, 64u, 100u, 128u, 255u, 512u }) {
		const std::vector<float> got = render(block);
		REQUIRE(got.size() == whole.size());
		size_t first = got.size();
		for (size_t i = 0; i < got.size(); ++i) {
			if (got[i] != whole[i]) {
				first = i;
				break;
			}
		}
		CAPTURE(block);
		CAPTURE(first);
		CHECK(first == got.size());
	}
}

TEST_CASE("the audio thread allocates nothing after activate: notes, chokes, parameter writes and the editor's key") {
	constexpr uint32_t kFrames = 256;
	Venue v;
	const clap_id level = v.idOf("Level");
	const clap_id tune = v.idOf("Tune");
	Events e;
	e.note(CLAP_EVENT_NOTE_ON, 10, 36, 1);
	e.param(level, 0.8);
	e.param(tune, 0.4, 20);
	v.run(kFrames, &e);
	CHECK(v.allocations == 0);
	e.clear();
	e.note(CLAP_EVENT_NOTE_ON, 0, 40, 2);
	e.note(CLAP_EVENT_NOTE_OFF, 100, 36, 1);
	e.note(CLAP_EVENT_NOTE_CHOKE, 200, 40, 2);
	for (int b = 0; b < 6; ++b) {
		v.run(kFrames, b == 0 ? &e : nullptr);
		CHECK(v.allocations == 0);
	}
	REQUIRE(v.bridge().postNote(36, true, 0.9f));
	v.run(kFrames);
	CHECK(v.allocations == 0);
	CHECK(v.peak(kFrames) > 0.0f);
}

TEST_CASE("the editor's play button, pressed on the plugin's own bridge, sounds the drum with no host event at all") {
	constexpr uint32_t kFrames = 256;
	Venue v;
	REQUIRE(blocksToSleep(v, kFrames, 8) >= 0);
	rack::RackBridge& bridge = v.bridge();
	const DeviceInstanceId id = bridge.deviceAtPosition(0);
	REQUIRE(id != kNoInstance);
	ui::WidgetRegistry registry;
	std::unique_ptr<ui::ExpandedView> view = ui::makeExpandedView(bridge, nullptr, registry, id, drumDesc());
	REQUIRE(view != nullptr);
	auto* panel = dynamic_cast<ui::DrumDeviceExpanded*>(view.get());
	REQUIRE(panel != nullptr);
	REQUIRE(panel->playForTest() != nullptr);
	visage::MouseEvent press;
	press.position = { 4.0f, 4.0f };
	press.button_id = visage::kMouseButtonLeft;
	press.button_state = visage::kMouseButtonLeft;
	press.is_down = true;
	panel->playForTest()->onMouseDown().callback(press);
	CHECK(bridge.hasPendingNotes());
	CHECK(v.run(kFrames) == CLAP_PROCESS_CONTINUE);
	float peak = v.peak(kFrames);
	for (int b = 0; b < 3; ++b) {
		v.run(kFrames);
		peak = std::max(peak, v.peak(kFrames));
	}
	CHECK(peak > 1.0e-3f);
	CHECK_FALSE(bridge.hasPendingNotes());
	visage::MouseEvent release = press;
	release.is_down = false;
	panel->playForTest()->onMouseUp().callback(release);
}

//---------------------------------------------------------------------------------------------------------------------------------
// PARAMETERS
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("the host's parameters are the device's controls, named, with ids that follow the list and none beside them") {
	Venue v;
	const clap_plugin_params* params = v.params();
	REQUIRE(params != nullptr);
	REQUIRE(params->count(v.plugin) == dp::kHostParamCount);
	const device::Descriptor& desc = drumDesc();
	// EVERY CONTROL OF THE DEVICE HAS EXACTLY ONE PLACE IN THE LIST, AND EVERY PLACE IS A CONTROL: a control that joins the device must
	// be appended (the ids are for ever), and a key that left would break the list.
	std::set<std::string> listed;
	for (const char* key : dp::kHostKeys) {
		CHECK(listed.insert(key).second);
		CHECK(controlOfKey(key) < desc.params.size());
	}
	for (const device::ParamDesc& p : desc.params) {
		CAPTURE(p.key);
		if (p.hostSlot) {
			CHECK(listed.count(p.key) == 1);
		}
	}
	std::set<clap_id> ids;
	for (uint32_t i = 0; i < dp::kHostParamCount; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, i, &info));
		const size_t control = controlOfKey(dp::kHostKeys[i]);
		REQUIRE(control < desc.params.size());
		CAPTURE(i);
		CAPTURE(info.name);
		// STABLE: the id is the slot place's, fixed for ever, in the list's order.
		CHECK(info.id == 0x1000 + i);
		CHECK(ids.insert(info.id).second);
		// NAMED after the control (the host shows "Drum · Level"), under the module "Drum".
		CHECK(std::string(info.name).find(desc.params[control].name) != std::string::npos);
		CHECK(std::string(info.module) == "Drum");
		CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) == 0);
		CHECK(info.min_value == 0.0);
		CHECK(info.max_value == 1.0);
		// FLAGS THE CONTROL'S OWN DECLARATION EARNS.
		const bool modulatable = (info.flags & CLAP_PARAM_IS_MODULATABLE) != 0;
		const bool automatable = (info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0;
		const bool editorOnly = device::isDragWindow(desc.params[control]);
		CHECK(automatable == !editorOnly);
		CHECK(modulatable == (desc.params[control].modulatable && !editorOnly));
	}
	// THE TWO UNBOUNDED COORDINATES ARE THE EDITOR'S (a drag window, not a lane): neither automatable nor modulatable.
	for (const char* key : { "drum_travel_x", "drum_travel_y" }) {
		const uint32_t index = static_cast<uint32_t>(std::find_if(std::begin(dp::kHostKeys), std::end(dp::kHostKeys),
		                                                          [&](const char* k) { return std::strcmp(k, key) == 0; })
		                                             - std::begin(dp::kHostKeys));
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, index, &info));
		CHECK((info.flags & (CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_MODULATABLE)) == 0);
	}
	// THE KIND AND THE TRIGGER ARE NOT MODULATABLE (the device says so), and still automatable.
	{
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, 1, &info));
		CHECK(std::string(dp::kHostKeys[1]) == "drum_kind");
		CHECK((info.flags & CLAP_PARAM_IS_MODULATABLE) == 0);
		CHECK((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
	}
	// NO MACROS AND NO OTHER SLOT: ids 0..7 and every slot past the list are no parameter at all. Asked of the plugin's own index
	// (the one clap-helpers is told), not of the host API: a non-NDEBUG build of clap-helpers TERMINATES on a host that asks about an id
	// the plugin does not have (the sanitizer gate's build), and that is a host's fault, not a state to provoke here.
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(chain->param_index_of != nullptr);
	for (uint32_t i = 0; i < dp::kHostParamCount; ++i) {
		CHECK(chain->param_index_of(v.plugin, 0x1000 + i) == static_cast<int32_t>(i));
	}
	for (const uint32_t id : { 0u, 7u, 8u, 0x0fffu, 0x1000u + static_cast<uint32_t>(dp::kHostParamCount), 0x1000u + 200u, 0x1000u + 255u, 0x1100u }) {
		CHECK(chain->param_index_of(v.plugin, id) == -1);
	}
}

TEST_CASE("every named parameter id reaches its control: a write by id moves the device's value (read from the engine) and reads back by id") {
	constexpr uint32_t kFrames = 128;
	const device::Descriptor& desc = drumDesc();
	Venue v;
	const clap_plugin_params* params = v.params();
	const DeviceInstanceId device = v.bridge().deviceAtPosition(0);
	REQUIRE(device != kNoInstance);
	for (uint32_t i = 0; i < dp::kHostParamCount; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, i, &info));
		const size_t control = controlOfKey(dp::kHostKeys[i]);
		const device::ParamDesc& p = desc.params[control];
		CAPTURE(info.name);
		// A POSITION AWAY FROM THE DEFAULT'S.
		double def = 0.0;
		REQUIRE(params->get_value(v.plugin, info.id, &def));
		const double position = def > 0.5 ? 0.25 : 0.75;
		// THE VALUE THE DEVICE SHOULD HOLD, restated from the control's declaration: linear in device units; a stepped control
		// lands on a step counted from its minimum.
		const double span = p.max - p.min;
		double expected = p.min + position * span;
		if (p.stepped) {
			expected = p.min + std::round(position * span);
		}
		Events e;
		e.param(info.id, position);
		v.run(kFrames, &e);
		v.mainThread();
		// THE ENGINE'S OWN VALUE of the control: what the audio thread resolved from the host's write.
		const float engineValue = v.test()->resolved_field(v.plugin, device, control);
		CHECK(engineValue == doctest::Approx(expected).epsilon(1e-5).scale(std::max(1.0, std::fabs(expected))));
		// AND THE HOST'S READ-BACK BY ID is where the write put the position.
		double back = -1.0;
		REQUIRE(params->get_value(v.plugin, info.id, &back));
		const double wantBack = p.stepped ? (expected - p.min) / span : position;
		CHECK(back == doctest::Approx(wantBack).epsilon(1e-5).scale(1.0));
		// AND IT IS NOT THE DEFAULT, so the check above could not pass on an unmoved control.
		CHECK(std::fabs(expected - p.def) > 0.0);
	}
}

TEST_CASE("Level, written by id, scales the render by the decibels it names, and Tune changes it") {
	constexpr uint32_t kFrames = 512;
	const auto hit = [&](const std::vector<std::pair<clap_id, double>>& writes, const char* name1, double v1) {
		Venue v;
		Events w;
		for (const auto& [id, value] : writes) {
			w.param(id, value);
		}
		if (name1 != nullptr) {
			w.param(v.idOf(name1), v1);
		}
		v.run(kFrames, &w);
		Events e;
		e.note(CLAP_EVENT_NOTE_ON, 0, 36, 1);
		v.run(kFrames, &e);
		return v.peak(kFrames);
	};
	const device::ParamDesc& level = drumDesc().params[controlOfKey("drum_level")];
	const double zeroDb = (0.0 - level.min) / (level.max - level.min);
	const double minus12 = (-12.0 - level.min) / (level.max - level.min);
	const float loud = hit({}, "Level", zeroDb);
	const float quiet = hit({}, "Level", minus12);
	REQUIRE(loud > 1.0e-3f);
	CHECK(quiet / loud == doctest::Approx(std::pow(10.0, -12.0 / 20.0)).epsilon(0.02));
	const device::ParamDesc& tune = drumDesc().params[controlOfKey("drum_tune")];
	const float tuned = hit({}, "Tune", (12.0 - tune.min) / (tune.max - tune.min));
	CHECK(tuned != loud);
}

//---------------------------------------------------------------------------------------------------------------------------------
// STATE
//---------------------------------------------------------------------------------------------------------------------------------

namespace {

struct MemStream {
	std::vector<unsigned char> bytes;
	size_t at = 0;
	clap_ostream out {};
	clap_istream in {};
	MemStream() {
		out.ctx = this;
		out.write = [](const clap_ostream* s, const void* buffer, uint64_t size) -> int64_t {
			auto* m = static_cast<MemStream*>(s->ctx);
			const auto* p = static_cast<const unsigned char*>(buffer);
			m->bytes.insert(m->bytes.end(), p, p + size);
			return static_cast<int64_t>(size);
		};
		in.ctx = this;
		in.read = [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
			auto* m = static_cast<MemStream*>(s->ctx);
			const size_t n = std::min<size_t>(size, m->bytes.size() - m->at);
			std::memcpy(buffer, m->bytes.data() + m->at, n);
			m->at += n;
			return static_cast<int64_t>(n);
		};
	}
};

std::vector<unsigned char> save(const clap_plugin* p) {
	const auto* state = static_cast<const clap_plugin_state*>(p->get_extension(p, CLAP_EXT_STATE));
	MemStream s;
	REQUIRE(state->save(p, &s.out));
	return s.bytes;
}

bool load(const clap_plugin* p, const std::vector<unsigned char>& bytes) {
	const auto* state = static_cast<const clap_plugin_state*>(p->get_extension(p, CLAP_EXT_STATE));
	MemStream s;
	s.bytes = bytes;
	return state->load(p, &s.in);
}

std::vector<float> struck(Venue& v) {
	std::vector<float> out;
	Events e;
	e.note(CLAP_EVENT_NOTE_ON, 0, 36, 1);
	v.run(512, &e, &out);
	for (int b = 0; b < 6; ++b) {
		v.run(512, nullptr, &out);
	}
	return out;
}

} // namespace

TEST_CASE("state: a fresh instance loads another's state bit for bit, sounds the same, and reads every parameter back the same") {
	Venue a;
	// A PATCH THAT IS NOT THE DEFAULT: a handful of controls of different kinds, written by id as a host's lanes would.
	Events e;
	for (const auto& [name, position] : std::vector<std::pair<const char*, double>> { { "Kind", 0.4 }, { "Level", 0.8 }, { "Tune", 0.3 }, { "Decay", 0.6 },
	                                                                                   { "Drive", 0.9 }, { "Pan", 0.2 }, { "Seed", 0.7 } }) {
		e.param(a.idOf(name), position);
	}
	a.run(256, &e);
	a.mainThread();
	const std::vector<unsigned char> saved = save(a.plugin);
	REQUIRE(saved.size() > 64);

	Venue b;
	REQUIRE(load(b.plugin, saved));
	b.run(256);
	b.mainThread();
	CHECK(save(b.plugin) == saved); // bit for bit
	for (uint32_t i = 0; i < dp::kHostParamCount; ++i) {
		clap_param_info info {};
		REQUIRE(a.params()->get_info(a.plugin, i, &info));
		double va = -1.0, vb = -2.0;
		REQUIRE(a.params()->get_value(a.plugin, info.id, &va));
		REQUIRE(b.params()->get_value(b.plugin, info.id, &vb));
		CAPTURE(info.name);
		CHECK(va == vb);
	}
	const std::vector<float> ra = struck(a);
	const std::vector<float> rb = struck(b);
	REQUIRE(ra.size() == rb.size());
	CHECK(ra == rb);
	// NOT THE DEFAULT: the saved patch sounds unlike a fresh instance, so the equality above could not pass on defaults.
	Venue fresh;
	CHECK(struck(fresh) != ra);
	// A SAVED STATE IS SEALED (the end marker the other plugins write), and one that is cut short is refused, nothing changed.
	std::vector<unsigned char> cut(saved.begin(), saved.end() - 7);
	Venue c;
	const std::vector<unsigned char> before = save(c.plugin);
	CHECK_FALSE(load(c.plugin, cut));
	CHECK(save(c.plugin) == before);
}

TEST_CASE("state: a Drum refuses a state that is not one drum, and loading is safe while blocks run") {
	// A CHAIN'S STATE (the Chain plugin's, here an empty rack and one with a different device) is not a drum's.
	const auto* factory = static_cast<const clap_plugin_factory*>(rack::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
	clap_host host = makeHost();
	const clap_plugin* chain = factory->create_plugin(factory, &host, "com.owlet.feathers-chain");
	REQUIRE(chain != nullptr);
	REQUIRE(chain->init(chain));
	const std::vector<unsigned char> chainState = save(chain);
	chain->destroy(chain);
	Venue v;
	const std::vector<unsigned char> before = save(v.plugin);
	CHECK_FALSE(load(v.plugin, chainState));
	CHECK(save(v.plugin) == before);
	// A LOAD BETWEEN BLOCKS OF A RUNNING PLUGIN lands by the next block, and the render is the loaded patch's.
	Venue a;
	Events e;
	e.param(a.idOf("Level"), 0.5);
	a.run(256, &e);
	a.mainThread();
	const std::vector<unsigned char> saved = save(a.plugin);
	Venue b;
	b.run(256);
	REQUIRE(load(b.plugin, saved));
	b.run(256);
	b.mainThread();
	CHECK(struck(a) == struck(b));
}

//---------------------------------------------------------------------------------------------------------------------------------
// THE EDITOR
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("the editor is the drum's own window: it opens at the view's size, reopens the same, and has no chain editor") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui = static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
	REQUIRE(gui != nullptr);
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
	const char* api = CLAP_WINDOW_API_WIN32;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	uint32_t w = 0, h = 0;
	REQUIRE(gui->get_size(v.plugin, &w, &h));
	CHECK(w == static_cast<uint32_t>(ui::DrumDeviceExpanded::kWidth));
	CHECK(h == static_cast<uint32_t>(ui::DrumDeviceExpanded::kMinimumHeight) + rack::RackGui::kDrumHeaderHeight); // the view under the Libra strip
	CHECK(v.test()->editor(v.plugin) == nullptr); // the chain editor does not exist in a drum
	gui->destroy(v.plugin);
	REQUIRE(gui->create(v.plugin, api, false));
	uint32_t w2 = 0, h2 = 0;
	REQUIRE(gui->get_size(v.plugin, &w2, &h2));
	CHECK(w2 == w);
	CHECK(h2 == h);
	gui->destroy(v.plugin);
}
