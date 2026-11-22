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
#include "plugin/device_registry.h"
#include "plugin/feathers_test_chain.h"
#include "plugin/chain_preset.h"
#include "plugin/factory_presets.h"
#include "plugin/preset_store.h"
#include "plugin/ui/container_presets.h"
#include "plugin/ui/device_preset_menu.h"
#include "plugin/ui/edit_history.h"
#include "plugin/ui/surface_layers.h"
#include "fx/filter/filter_types.h"
#include "plugin/ui/parallel_rack_expanded.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/ui/expanded_views.h"
#include "plugin/ui/plugin_window.h"
#include "plugin/ui/widget_registry.h"
#include "portable_aligned_alloc.h"
#include "rack/parallel_rack.h"
#include "rack/rack_plugin.h"
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
#include <filesystem>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

// FEATHERS RACK — THE PARALLEL RACK AS A PLUGIN OF ITS OWN, driven through the real CLAP API (create by id, ports, params, notes, audio,
// state, editor) the way test_rack_clap drives the rack. Its sound is held to the rack's own routing, by value.

using namespace feathers;
namespace pr = feathers::rack::parallel;
namespace rp = feathers::rack::rackplugin;

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
	h.name = "rack-venue";
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

/// An audio effect, created by id from the bundle's factory, initialised and activated.
struct Venue {
	clap_host host = makeHost();
	const clap_plugin* plugin = nullptr;
	std::vector<float> l, r;
	clap_audio_buffer out {};
	float* ptr[2] {};
	clap_audio_buffer in {};
	/// THE INPUT, held for every block: a constant stereo signal the case chooses (default silence).
	std::vector<float> inL, inR;
	long allocations = 0;

	explicit Venue(uint32_t maxFrames = 512, double rate = 48000.0) : l(maxFrames), r(maxFrames) {
		const auto* factory = static_cast<const clap_plugin_factory*>(rack::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
		REQUIRE(factory != nullptr);
		plugin = factory->create_plugin(factory, &host, rp::kPluginId);
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
		REQUIRE(plugin->activate(plugin, rate, 1, maxFrames));
		REQUIRE(plugin->start_processing(plugin));
		ptr[0] = l.data();
		ptr[1] = r.data();
		out.data32 = ptr;
		out.channel_count = 2;
		inL.assign(maxFrames, 0.0f);
		inR.assign(maxFrames, 0.0f);
		in.data32 = ptr;
		in.channel_count = 2;
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
		p.audio_inputs = &in;
		p.audio_inputs_count = 1;
		p.audio_outputs = &out;
		p.audio_outputs_count = 1;
		p.in_events = events != nullptr ? &events->in : &none.in;
		p.out_events = &sink;
		std::copy(inL.begin(), inL.end(), l.begin()); // in place, as a host may hand it: the input is the output buffer
		std::copy(inR.begin(), inR.end(), r.begin());
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

} // namespace


namespace {

const device::Descriptor& rackDesc() {
	return pr::kDescriptor;
}

size_t controlOfKey(const char* key) {
	const device::Descriptor& d = rackDesc();
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (std::strcmp(d.params[i].key, key) == 0) {
			return i;
		}
	}
	return d.params.size();
}

/// THE ROOT RACK of a plugin, as the editor's own view reaches it.
pr::Rack& rackOf(Venue& v) {
	const DeviceInstanceId id = v.bridge().deviceAtPosition(0);
	REQUIRE(id != kNoInstance);
	pr::Rack* rack = pr::rackOf(v.bridge().containerSurfaceOf(id));
	REQUIRE(rack != nullptr);
	return *rack;
}

/// Hold the input at (l, r) and run `blocks` blocks of 512 (well past the rack's fades); the last block's output is left in the venue.
void runHeld(Venue& v, float l, float r, int blocks = 24) {
	for (int b = 0; b < blocks; ++b) {
		std::fill(v.inL.begin(), v.inL.end(), l);
		std::fill(v.inR.begin(), v.inR.end(), r);
		v.run(512);
	}
}

} // namespace

//---------------------------------------------------------------------------------------------------------------------------------
// THE BUNDLE
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("the factory lists the Rack as a fourth plugin, the three before it unchanged, and creates each by id") {
	const auto* factory = static_cast<const clap_plugin_factory*>(rack::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
	REQUIRE(factory != nullptr);
	REQUIRE(factory->get_plugin_count(factory) == 4);
	const char* const ids[4] = { "com.owlet.feathers-chain", "com.owlet.feathers.noterack", "com.owlet.feathers.drum", "com.owlet.feathers.rack" };
	const char* const names[4] = { "Feathers Chain", "Feathers Note Rack", "Feathers Drum", "Feathers Rack" };
	for (uint32_t i = 0; i < 4; ++i) {
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
	CHECK(factory->get_plugin_descriptor(factory, 4) == nullptr);
	CHECK(std::string(rack::rackDescriptor()->id) == ids[3]);
	// AN AUDIO EFFECT, as the Chain is: a host files it with the effects.
	bool effect = false, instrument = false;
	for (const char* const* f = rack::rackDescriptor()->features; f != nullptr && *f != nullptr; ++f) {
		effect = effect || std::string(*f) == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT;
		instrument = instrument || std::string(*f) == CLAP_PLUGIN_FEATURE_INSTRUMENT;
	}
	CHECK(effect);
	CHECK_FALSE(instrument);
}

TEST_CASE("the root is one Rack device, in the default mode, fully wet") {
	Venue v;
	REQUIRE(v.bridge().deviceAtPosition(0) != kNoInstance);
	CHECK(v.bridge().deviceAtPosition(1) == kNoInstance); // one device and no other
	const DeviceInstanceId id = v.bridge().deviceAtPosition(0);
	const device::Descriptor* desc = v.bridge().descriptorOf(id);
	REQUIRE(desc != nullptr);
	CHECK(std::string(desc->traits.tag) == "RACK");
	CHECK(rackOf(v).modeForTest() == pr::Mode::Parallel);
	CHECK(v.test()->resolved_field(v.plugin, id, controlOfKey("rack_mix")) == 1.0f);
	CHECK(v.test()->resolved_field(v.plugin, id, controlOfKey("rack_enabled")) == 1.0f);
}

//---------------------------------------------------------------------------------------------------------------------------------
// THE HOST'S PARAMETERS
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("the host-parameter list is append-only: the first 17 places are what ships, and the rest is the pool") {
	// A FINGERPRINT OF THE KEYS IN ORDER (FNV-1a over each key and its NUL): moving, removing or inserting before any of them changes it,
	// and a host's lanes live on those ids. New controls are appended after, and this number is then replaced by the prefix's.
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < 17; ++i) {
		for (const char* c = rp::kHostKeys[i];; ++c) {
			h ^= static_cast<unsigned char>(*c);
			h *= 0x100000001b3ull;
			if (*c == '\0') {
				break;
			}
		}
	}
	CHECK(h == 0xc9a4a5e089e24293ull);
	REQUIRE(rp::kHostParamCount >= 17);
}

TEST_CASE("the host's parameters are the rack's controls, named, then the pool: ids follow the list and nothing structural is among them") {
	Venue v;
	const clap_plugin_params* params = v.params();
	REQUIRE(params != nullptr);
	CHECK(params->count(v.plugin) == 256); // every slot is declared; the engine's own macros are not parameters here
	const device::Descriptor& desc = rackDesc();
	std::set<std::string> listed;
	for (const char* key : rp::kHostKeys) {
		CHECK(listed.insert(key).second);
		REQUIRE(controlOfKey(key) < desc.params.size());
		CHECK(desc.params[controlOfKey(key)].hostSlot);
	}
	// EVERY CONTROL A SLOT MAY CARRY IS LISTED, and the structural ones (Mode, Unused, Bands, Slope, Filtering) are not.
	// (THE DEVICE'S LATER PARAMS ARE THE 64 CHAINS' LEVEL, MUTE AND SOLO DESTINATIONS: the pool's, taken by a touch, never pinned.)
	for (size_t c = 0; c < static_cast<size_t>(pr::Param::kCount); ++c) {
		const device::ParamDesc& p = desc.params[c];
		CAPTURE(p.key);
		CHECK((listed.count(p.key) == 1) == p.hostSlot);
	}
	for (const char* key : { "rack_mode", "rack_unused", "rack_bands", "rack_slope", "rack_filtering" }) {
		CHECK(listed.count(key) == 0);
	}
	for (uint32_t i = 0; i < rp::kHostParamCount; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, i, &info));
		const size_t control = controlOfKey(rp::kHostKeys[i]);
		CAPTURE(i);
		CAPTURE(info.name);
		CHECK(info.id == 0x1000 + i);
		std::string want = desc.params[control].name;
		if (want.rfind("Rack ", 0) == 0) {
			want = want.substr(5); // the module says "Rack"; the name is the control's without it
		}
		const char* shortName = desc.params[control].shortName;
		const bool named = std::string(info.name).find(want) != std::string::npos
		                   || (shortName != nullptr && std::string(info.name).find(shortName) != std::string::npos);
		CHECK(named);
		CHECK(std::string(info.module) == "Rack");
		CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) == 0);
		CHECK((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
	}
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	for (uint32_t i = 0; i < 256; ++i) {
		CHECK(chain->param_index_of(v.plugin, 0x1000 + i) == static_cast<int32_t>(i));
	}
	for (const uint32_t id : { 0u, 7u, 8u, 0x0fffu, 0x1000u + 256u, 0x1100u }) {
		CHECK(chain->param_index_of(v.plugin, id) == -1);
	}
}

TEST_CASE("every named parameter id reaches its control: a write by id moves the rack's value (read from the engine) and reads back by id") {
	const device::Descriptor& desc = rackDesc();
	Venue v;
	const DeviceInstanceId device = v.bridge().deviceAtPosition(0);
	for (uint32_t i = 0; i < rp::kHostParamCount; ++i) {
		const size_t control = controlOfKey(rp::kHostKeys[i]);
		const device::ParamDesc& p = desc.params[control];
		CAPTURE(rp::kHostKeys[i]);
		const clap_id id = 0x1000 + i;
		double def = 0.0;
		REQUIRE(v.params()->get_value(v.plugin, id, &def));
		const double position = def > 0.5 ? 0.25 : 0.75;
		const double span = p.max - p.min;
		double expected = p.min + position * span;
		if (p.stepped) {
			expected = p.min + std::round(position * span);
		}
		Events e;
		e.param(id, position);
		v.run(128, &e);
		v.mainThread();
		CHECK(v.test()->resolved_field(v.plugin, device, control)
		      == doctest::Approx(expected).epsilon(1e-5).scale(std::max(1.0, std::fabs(expected))));
		double back = -1.0;
		REQUIRE(v.params()->get_value(v.plugin, id, &back));
		CHECK(back == doctest::Approx(p.stepped ? (expected - p.min) / span : position).epsilon(1e-5).scale(1.0));
		CHECK(std::fabs(expected - p.def) > 0.0);
	}
}

//---------------------------------------------------------------------------------------------------------------------------------
// ROUTING
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("a new Rack is the identity, and Mix by id is the dry/wet it names") {
	Venue v;
	runHeld(v, 0.75f, 0.25f);
	CHECK(v.l[100] == doctest::Approx(0.75f).epsilon(1e-4));
	CHECK(v.r[100] == doctest::Approx(0.25f).epsilon(1e-4));
	// MIX 0 PASSES THE INPUT; with one empty chain Mix 1 is the same, so a muted chain makes the two differ: the wet sum is then silence.
	rackOf(v).setMuted(0, true);
	runHeld(v, 0.75f, 0.25f);
	CHECK(v.l[100] == 0.0f);
	Events e;
	e.param(0x1000 + 1, 0.0); // rack_mix
	v.run(512, &e);
	runHeld(v, 0.75f, 0.25f);
	CHECK(v.l[100] == doctest::Approx(0.75f).epsilon(1e-4));
	CHECK(v.r[100] == doctest::Approx(0.25f).epsilon(1e-4));
}

TEST_CASE("the mode changes the routing: in Mid/Side the side is chain 2's, and a muted chain 2 takes it out of the sum") {
	// L 0.75 R 0.25: mid 0.5, side 0.25. Chain 1 muted in Parallel would be silence; chain 2 muted leaves chain 1's whole input.
	Venue v;
	REQUIRE(rackOf(v).addChain() == 1);
	rackOf(v).setMuted(1, true);
	const DeviceInstanceId id = v.bridge().deviceAtPosition(0);
	runHeld(v, 0.75f, 0.25f);
	CHECK(v.l[100] == doctest::Approx(0.75f).epsilon(1e-6));
	CHECK(v.r[100] == doctest::Approx(0.25f).epsilon(1e-6));
	v.bridge().setField(id, controlOfKey("rack_mode"), static_cast<float>(pr::Mode::MidSide));
	runHeld(v, 0.75f, 0.25f);
	v.mainThread();
	CHECK(rackOf(v).modeForTest() == pr::Mode::MidSide);
	// ONLY THE MID IS HEARD: (M, M).
	CHECK(v.l[100] == doctest::Approx(0.5f).epsilon(1e-6));
	CHECK(v.r[100] == doctest::Approx(0.5f).epsilon(1e-6));
	// AND UNMUTING CHAIN 2 RESTORES THE SIDE: (M + S, M - S) is the input again.
	rackOf(v).setMuted(1, false);
	runHeld(v, 0.75f, 0.25f);
	CHECK(v.l[100] == doctest::Approx(0.75f).epsilon(1e-6));
	CHECK(v.r[100] == doctest::Approx(0.25f).epsilon(1e-6));
}

//---------------------------------------------------------------------------------------------------------------------------------
// THE AUDIO THREAD
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("the audio thread allocates nothing after activate: blocks, parameter writes, a mode change and a chain's mute") {
	Venue v;
	REQUIRE(rackOf(v).addChain() == 1);
	const DeviceInstanceId id = v.bridge().deviceAtPosition(0);
	Events e;
	e.param(0x1000 + 1, 0.5);
	e.param(0x1000 + 3, 0.3, 20);
	for (int b = 0; b < 4; ++b) {
		std::fill(v.inL.begin(), v.inL.end(), 0.5f);
		std::fill(v.inR.begin(), v.inR.end(), -0.25f);
		v.run(256, b == 0 ? &e : nullptr);
		CHECK(v.allocations == 0);
	}
	for (const pr::Mode mode : { pr::Mode::Select, pr::Mode::MidSide, pr::Mode::LeftRight, pr::Mode::Bands, pr::Mode::Parallel }) {
		v.bridge().setField(id, controlOfKey("rack_mode"), static_cast<float>(mode));
		for (int b = 0; b < 3; ++b) {
			v.run(256);
			CHECK(v.allocations == 0);
		}
	}
	CHECK(v.peak(256) > 0.0f);
}

//---------------------------------------------------------------------------------------------------------------------------------
// STATE
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("state: a fresh instance loads another's state bit for bit, sounds the same, and reads every parameter back the same") {
	Venue a;
	REQUIRE(rackOf(a).addChain() == 1);
	rackOf(a).setMuted(1, true);
	const DeviceInstanceId id = a.bridge().deviceAtPosition(0);
	a.bridge().setField(id, controlOfKey("rack_mode"), static_cast<float>(pr::Mode::MidSide));
	Events e;
	e.param(0x1000 + 1, 0.6); // mix
	e.param(0x1000 + 3, 0.7); // macro 1
	e.param(0x1000 + 2, 0.4); // gain
	a.run(256, &e);
	a.mainThread();
	const std::vector<unsigned char> saved = save(a.plugin);
	REQUIRE(saved.size() > 64);

	Venue b;
	REQUIRE(load(b.plugin, saved));
	b.run(256);
	b.mainThread();
	CHECK(save(b.plugin) == saved); // bit for bit
	CHECK(rackOf(b).modeForTest() == pr::Mode::MidSide);
	CHECK(rackOf(b).liveChainsForTest() == 2);
	for (uint32_t i = 0; i < rp::kHostParamCount; ++i) {
		double va = -1.0, vb = -2.0;
		REQUIRE(a.params()->get_value(a.plugin, 0x1000 + i, &va));
		REQUIRE(b.params()->get_value(b.plugin, 0x1000 + i, &vb));
		CAPTURE(rp::kHostKeys[i]);
		CHECK(va == vb);
	}
	runHeld(a, 0.75f, 0.25f);
	runHeld(b, 0.75f, 0.25f);
	CHECK(a.l == b.l);
	CHECK(a.r == b.r);
	// NOT THE DEFAULT: a fresh instance does not sound like it.
	Venue fresh;
	runHeld(fresh, 0.75f, 0.25f);
	CHECK(fresh.l != a.l);
	// SEALED: one that is cut short is refused, nothing changed.
	std::vector<unsigned char> cut(saved.begin(), saved.end() - 7);
	Venue c;
	const std::vector<unsigned char> before = save(c.plugin);
	CHECK_FALSE(load(c.plugin, cut));
	CHECK(save(c.plugin) == before);
}

TEST_CASE("state: a Rack refuses a state that is not one rack, and the Chain's patches stay the Chain's") {
	const auto* factory = static_cast<const clap_plugin_factory*>(rack::entryGetFactory(CLAP_PLUGIN_FACTORY_ID));
	clap_host host = makeHost();
	const clap_plugin* chain = factory->create_plugin(factory, &host, "com.owlet.feathers-chain");
	REQUIRE(chain != nullptr);
	REQUIRE(chain->init(chain));
	const std::vector<unsigned char> chainState = save(chain);
	Venue v;
	const std::vector<unsigned char> before = save(v.plugin);
	CHECK_FALSE(load(v.plugin, chainState));
	CHECK(save(v.plugin) == before);
	// AND THE RACK'S STATE IS NOT A CHAIN'S TO LOAD AS ITS OWN ROOT: the Chain takes it only as the one Rack device it holds, with its
	// own ids; here it is enough that the Rack's state loads in a Rack and leaves a Chain's save a different byte string.
	CHECK(before != chainState);
	chain->destroy(chain);
}

//---------------------------------------------------------------------------------------------------------------------------------
// THE EDITOR
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("the editor opens on the Rack's own view, with the mode selector visible, and has no chain editor") {
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
	CHECK(w > 300);
	CHECK(h > 300);
	CHECK(v.test()->editor(v.plugin) == nullptr); // the chain editor does not exist in a Rack
	gui->destroy(v.plugin);
	REQUIRE(gui->create(v.plugin, api, false));
	uint32_t w2 = 0, h2 = 0;
	REQUIRE(gui->get_size(v.plugin, &w2, &h2));
	CHECK(w2 == w);
	CHECK(h2 == h);
	gui->destroy(v.plugin);
}

//---------------------------------------------------------------------------------------------------------------------------------
// THE HEADER ROW AND THE OPEN CHAIN
//---------------------------------------------------------------------------------------------------------------------------------

TEST_CASE("a new Rack opens with its first chain expanded, so the effect slot is visible") {
	Venue v;
	CHECK(rackOf(v).open(0));
	// AND IT SURVIVES A SAVE AND A LOAD: view state, kept with the patch.
	Venue b;
	REQUIRE(load(b.plugin, save(v.plugin)));
	CHECK(rackOf(b).open(0));
}

TEST_CASE("the header's browser lists the Racks bank, and loading a rack through it replaces the root Rack: Bands, three chains") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	// THE RACK TO LOAD: another instance, in Bands mode with three chains, captured as a Racks preset.
	Venue a;
	REQUIRE(rackOf(a).addChain() == 1);
	REQUIRE(rackOf(a).addChain() == 2);
	const DeviceInstanceId aId = a.bridge().deviceAtPosition(0);
	a.bridge().setField(aId, controlOfKey("rack_mode"), static_cast<float>(pr::Mode::Bands));
	a.bridge().setField(aId, controlOfKey("rack_bands"), 1.0f); // three bands
	runHeld(a, 0.5f, 0.5f, 4);
	ui::PresetData preset;
	REQUIRE(captureContainerPreset(a.bridge(), rackPresetDescriptor(), aId, &preset.bytes));

	Venue v;
	const DeviceInstanceId id = v.bridge().deviceAtPosition(0);
	CHECK(rackOf(v).modeForTest() == pr::Mode::Parallel);
	CHECK(rackOf(v).liveChainsForTest() == 1);
	// THE HEADER'S BROWSER IS THE RACK VIEW'S OWN (the window seats it above the view): the Racks bank, from Init.
	ui::WidgetRegistry registry;
	const device::Descriptor* desc = v.bridge().descriptorOf(id);
	REQUIRE(desc != nullptr);
	std::unique_ptr<ui::ExpandedView> view = ui::makeExpandedView(v.bridge(), nullptr, registry, id, *desc, nullptr);
	REQUIRE(view != nullptr);
	auto* browser = dynamic_cast<ui::PresetBrowser*>(view->titleAccessory());
	REQUIRE(browser != nullptr);
	CHECK(browser->bank() == kRackPresetBank);
	REQUIRE(browser->count() >= 1);
	CHECK(browser->entry(0).name == "Init");
	// LOADING THROUGH THE SAME PROVIDER THE BROWSER HOLDS PUTS THE RACK IN BANDS MODE WITH THREE CHAINS.
	ui::ContainerPresets presets = ui::makeRackDevicePresets(v.bridge(), id, nullptr);
	REQUIRE(presets.provider->apply(preset));
	v.run(512);
	v.mainThread();
	CHECK(rackOf(v).modeForTest() == pr::Mode::Bands);
	CHECK(rackOf(v).liveChainsForTest() == 3);
	// AND THE ROOT IS STILL ONE RACK DEVICE: the load replaced the rack's contents, not the plugin's root.
	CHECK(v.bridge().deviceAtPosition(1) == kNoInstance);
}

TEST_CASE("factory Rack presets: each loads into the Rack plugin while its window's view is built over the live bridge and audio runs, in every view") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	size_t shipped = 0;
	for (const feathers::FactoryPresetFile& f : feathers::factoryPresetFiles()) {
		if (f.path.rfind("Racks/", 0) != 0) {
			continue;
		}
		++shipped;
		feathers::PresetMeta meta;
		ui::PresetData preset;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(f.bytes.begin(), f.bytes.end()), &meta, &preset.bytes));
		for (const int view : { 0, 1, 2, 3 }) {
			for (const bool twice : { false, true }) {
				CAPTURE(std::string(f.path));
				CAPTURE(view);
				CAPTURE(twice);
				Venue v;
				const DeviceInstanceId id = v.bridge().deviceAtPosition(0);
				v.bridge().setField(id, controlOfKey("rack_view"), static_cast<float>(view));
				v.run(512);
				ui::WidgetRegistry registry;
				const device::Descriptor* desc = v.bridge().descriptorOf(id);
				REQUIRE(desc != nullptr);
				std::unique_ptr<ui::ExpandedView> window = ui::makeExpandedView(v.bridge(), nullptr, registry, id, *desc, nullptr);
				REQUIRE(window != nullptr);
				window->setBounds(0.0f, 0.0f, 1200.0f, 700.0f);
				// AND THE RACK'S CARD, beside the window, over the same bridge (a lane shows both).
				ui::ParallelRackCard card(v.bridge(), registry, id, nullptr);
				card.setBounds(0.0f, 0.0f, 320.0f, 420.0f);
				for (int i = 0; i < 3; ++i) {
					v.run(512);
					(void)window->update();
					(void)card.update();
				}
				ui::ContainerPresets presets = ui::makeRackDevicePresets(v.bridge(), id, nullptr);
				for (int load = 0; load < (twice ? 2 : 1); ++load) {
					REQUIRE(presets.provider->apply(preset));
					// THE EDITOR'S TICK AND THE AUDIO THREAD'S BLOCKS INTERLEAVE, as a host runs them.
					for (int i = 0; i < 24; ++i) {
						v.run(512);
						if (i % 3 == 0) {
							v.mainThread();
						}
						// A RESIZE OR A REPAINT CAN COME BEFORE THE TICK that rebinds what a preset replaced: layout first, then the tick.
						window->resized();
						card.resized();
						(void)window->update();
						(void)card.update();
					}
				}
				for (const float x : v.l) {
					REQUIRE(std::isfinite(x));
				}
			}
		}
	}
	REQUIRE(shipped > 0);
}

//---------------------------------------------------------------------------------------------------------------------------------
// THE SAME DEFECT FAMILY, ROAD BY ROAD: a preset (or a state) replaces the Rack's chains under whatever the editor has open
//---------------------------------------------------------------------------------------------------------------------------------

namespace {

/// A RACK PLUGIN WITH ITS WINDOW, ITS CARD AND ITS PRESENTING LAYERS OPEN over the live bridge, three chains, a Filter in the first two.
struct OpenRack {
	Venue& v;
	DeviceInstanceId id;
	ui::WidgetRegistry registry;
	ui::SurfaceLayers layers;
	std::unique_ptr<ui::ExpandedView> window;
	std::unique_ptr<ui::ParallelRackCard> card;
	ui::ParallelRackExpanded* panel = nullptr;

	OpenRack(Venue& venue, int view) : v(venue), id(venue.bridge().deviceAtPosition(0)) {
		while (rackOf(v).chainCount() < 3) {
			REQUIRE(rackOf(v).addChain() != SIZE_MAX);
		}
		for (size_t c = 0; c < 2; ++c) {
			REQUIRE(rackOf(v).chainAt(c)->bridge()->addDevice(fx::filter::kTraits.tag) != kNoInstance);
		}
		v.bridge().setField(id, controlOfKey("rack_view"), static_cast<float>(view));
		for (int i = 0; i < 4; ++i) {
			v.run(512);
		}
		const device::Descriptor* desc = v.bridge().descriptorOf(id);
		REQUIRE(desc != nullptr);
		window = ui::makeExpandedView(v.bridge(), nullptr, registry, id, *desc, nullptr);
		REQUIRE(window != nullptr);
		panel = dynamic_cast<ui::ParallelRackExpanded*>(window.get());
		REQUIRE(panel != nullptr);
		layers.setBounds(0.0f, 0.0f, 1200.0f, 700.0f);
		panel->setPresentedOn(&layers);
		window->setBounds(0.0f, 0.0f, 1200.0f, 700.0f);
		card = std::make_unique<ui::ParallelRackCard>(v.bridge(), registry, id, nullptr);
		card->setBounds(0.0f, 0.0f, 320.0f, 420.0f);
		tick(3);
	}
	/// LAYOUT FIRST, THEN THE EDITOR'S TICK, with the audio thread's blocks and the host's visits between, as a host runs them.
	void tick(int rounds) {
		for (int i = 0; i < rounds; ++i) {
			v.run(512);
			if (i % 3 == 0) {
				v.mainThread();
			}
			window->resized();
			card->resized();
			(void)window->update();
			(void)card->update();
		}
		for (const float x : v.l) {
			REQUIRE(std::isfinite(x));
		}
	}
	rack::RackBridge& chain(size_t i) { return *rackOf(v).chainAt(i)->bridge(); }
};

std::vector<ui::PresetData> shippedRackPresets() {
	std::vector<ui::PresetData> out;
	for (const feathers::FactoryPresetFile& f : feathers::factoryPresetFiles()) {
		if (f.path.rfind("Racks/", 0) != 0) {
			continue;
		}
		feathers::PresetMeta meta;
		ui::PresetData data;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(f.bytes.begin(), f.bytes.end()), &meta, &data.bytes));
		out.push_back(std::move(data));
	}
	REQUIRE_FALSE(out.empty());
	return out;
}

} // namespace

TEST_CASE("replaced chains, road 1: a device window open on a device in a chain the preset replaces") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	for (const ui::PresetData& preset : shippedRackPresets()) {
		for (const int view : { 0, 1, 3 }) {
			CAPTURE(view);
			Venue v;
			OpenRack open(v, view);
			ui::FxLaneRail& rail = open.panel->railForTest(0);
			ui::ChainColumn* column = rail.columnForTest();
			REQUIRE(column != nullptr);
			column->expandDevice(open.chain(0).deviceAtPosition(0));
			open.tick(3);
			REQUIRE(open.layers.overlay().panelCount() >= 1); // the device's window is really open (else this case drives nothing)
			ui::ContainerPresets presets = ui::makeRackDevicePresets(v.bridge(), open.id, nullptr);
			REQUIRE(presets.provider->apply(preset));
			open.tick(24);
			MESSAGE("panels after the load: ", open.layers.overlay().panelCount());
		}
	}
}

TEST_CASE("replaced chains, road 2: a chain's rename field and a device's rename field open when the preset lands") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	for (const ui::PresetData& preset : shippedRackPresets()) {
		for (const bool chainField : { true, false }) {
			for (const int view : { 0, 1, 3 }) {
				CAPTURE(chainField);
				CAPTURE(view);
				Venue v;
				OpenRack open(v, view);
				if (chainField) {
					open.panel->chainRenameForTest().begin(1, open.panel->railForTest(1), visage::Bounds { 0.0f, 0.0f, 120.0f, 20.0f });
					REQUIRE(open.panel->chainRenameForTest().renaming());
				}
				else {
					ui::ChainColumn* column = open.panel->railForTest(0).columnForTest();
					REQUIRE(column != nullptr);
					column->renameForTest().begin(open.chain(0), open.chain(0).deviceAtPosition(0), *column, visage::Bounds { 0.0f, 0.0f, 120.0f, 20.0f });
					REQUIRE(column->renameForTest().renaming());
				}
				ui::ContainerPresets presets = ui::makeRackDevicePresets(v.bridge(), open.id, nullptr);
				REQUIRE(presets.provider->apply(preset));
				open.tick(24);
				// AND A COMMIT AFTER IT (Enter on a field whose chain or device is gone) changes nothing and breaks nothing.
				if (chainField) {
					(void)open.panel->chainRenameForTest().renaming();
				}
				open.tick(4);
			}
		}
	}
}

TEST_CASE("replaced chains, road 3: undo and redo of a Rack preset loaded from the header menu, the window open") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	size_t n = 0;
	for (const feathers::FactoryPresetFile& f : feathers::factoryPresetFiles()) {
		if (f.path.rfind("Racks/", 0) != 0) {
			continue;
		}
		feathers::PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(f.bytes.begin(), f.bytes.end()), &meta, &payload));
		REQUIRE(feathers::PresetStore::save(std::filesystem::path(std::string(f.path)).stem().string(), payload, kRackPresetBank));
		++n;
	}
	REQUIRE(n > 0);
	for (const int view : { 0, 1, 2, 3 }) {
		CAPTURE(view);
		Venue v;
		OpenRack open(v, view);
		ui::EditHistory history;
		ui::DevicePresetMenu menu(v.bridge(), history, nullptr);
		const auto list = menu.list(open.id);
		REQUIRE(list.size() >= 2);
		for (const auto& entry : list) {
			if (entry.name == "Init") {
				continue;
			}
			REQUIRE(menu.apply(open.id, entry));
			open.tick(12);
			history.undo();
			open.tick(12);
			history.redo();
			open.tick(12);
			history.undo();
			open.tick(6);
		}
	}
}

TEST_CASE("replaced chains, road 5: the host's state load into a Rack whose window is open, audio running") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	for (const ui::PresetData& preset : shippedRackPresets()) {
		// THE STATE TO LOAD: another instance holding the shipped preset, saved through the CLAP state road.
		Venue a;
		{
			ui::ContainerPresets presets = ui::makeRackDevicePresets(a.bridge(), a.bridge().deviceAtPosition(0), nullptr);
			REQUIRE(presets.provider->apply(preset));
		}
		for (int i = 0; i < 6; ++i) {
			a.run(512);
			a.mainThread();
		}
		const std::vector<unsigned char> saved = save(a.plugin);
		REQUIRE(saved.size() > 64);
		for (const int view : { 0, 1, 2, 3 }) {
			for (const bool twice : { false, true }) {
				CAPTURE(view);
				CAPTURE(twice);
				Venue v;
				OpenRack open(v, view);
				REQUIRE(load(v.plugin, saved));
				open.tick(24);
				if (twice) {
					REQUIRE(load(v.plugin, saved));
					open.tick(24);
				}
			}
		}
	}
}
