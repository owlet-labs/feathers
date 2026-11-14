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

// The plugin as a host sees it: through clap_entry, the factory, and the C function pointers on
// clap_plugin. Nothing here touches Synth or ParamSet directly.
//
// The other suites test what the voice layer decides. This one tests that a host can reach it at
// all — that the lifecycle is honoured in the order a host calls it, that events in a clap_process
// arrive as notes, and that the samples come back in the buffers the host supplied. Every one of
// those is a wiring question, and wiring is exactly what a unit test of the layer underneath cannot
// see.
//
// The entry point is compiled into this executable rather than dlopen()ed. What that gives up is
// the export table of the built .clap; what it buys is a test that runs identically on all three
// platforms of the CI matrix, where dlopen, LoadLibrary and bundle layout are three different
// problems. Task 12's clap-wrapper build and a real host are what exercise the loading itself.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "clap/clap.h"

#include "hostslots/slot_host.h"
#include "plugin/clap_checking.h"
#include "plugin/audition.h"
#include "plugin/fx_chain.h"
#include "plugin/gui_bridge.h"
#include "plugin/mod_rack.h"
#include "plugin/synth.h"
#include "plugin/params.h"
#include "plugin/sound_variations.h"
#include "plugin/state_block_order.h"
#include "plugin/factory_presets.h"
#include "plugin/preset_store.h"
#include "state/state_trailer.h"
#include "plugin/feathers_test_chain.h"
#include "mod/arp/arp_types.h"
#include "mod/arp/pattern_types.h"
#include "mod/lfo/lfo_types.h"
#include "plugin/gui.h"
#include "plugin/editor_prefs.h"
#include "plugin/ui/plugin_window.h"
#include "plugin/ui/chrome_hue.h"
#include "plugin/factory_presets.h"
#include "plugin/preset_store.h"
#include "fx/shaper/shaper_types.h"
#include "fx/inject/inject_types.h"
#include "fx/spectral/spectral_types.h"
#include "fx/filter/filter_types.h"
#include "device/format_value.h"
#include "plugin/device_registry.h"
#include "fx/delay/delay_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/resonate/resonate_types.h"
#include "fx/resonate/resonate_lanes.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"
#include "rack/parallel_rack.h"
#include "rack/rack_device.h"
#include "fx/eq/eq_types.h"
#include "fx/harm/harm_types.h"
#include "fx/spectral/spectral_types.h"
#include "fx/phaser/phaser_types.h"
#include "fx/owltt/owltt_types.h"
#include "fx/clip/clip_types.h"
#include "visage_ui/events.h"
#include "portable_aligned_alloc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <new>
#include <string>
#include <string_view>
#include <vector>
#include <thread>
#include <chrono>
#include <filesystem>
#include <map>
#include <span>
#include <fstream>
#include <iterator>
#include <unistd.h>

#if defined(__linux__)
#include <X11/Xlib.h>
// Xlib's `None` macro would rewrite this file's `Placed::None`.
#undef None
#endif

extern "C" const clap_plugin_entry clap_entry;

// OL (asan-triage 2026-10-19): the gate builds with -UNDEBUG, where visage's InstanceCounter<Window> asserts at static
// destruction that no Window is alive, and `PluginWindow` abandons dead windows on purpose (backlog GP). Release them
// once every case has run, so the process exits clean; the leak stays in the plugin.
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

namespace {

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kMaxFrames = 512;

//----------------------//
// Allocation counting  //
//----------------------//

/// Every allocation the process ever makes, counted.
///
/// A plugin that allocates on the audio thread is a plugin that will one day take a page fault, or
/// a malloc lock, in the middle of somebody's take. It is the kind of bug no listening test finds
/// and no assertion about samples can catch, so it is counted directly: the count is read either
/// side of a process() call and has to be unchanged.
///
/// Replacing the global operators is the portable half of the trick — every allocation C++ makes
/// goes through them, on every platform, with no linker games. What it does not see is a bare
/// malloc() from inside a C library. Nothing on this path has one, and if something acquires one
/// this test will not notice; that is the limit of the claim, and it is worth having anyway.
std::atomic<long> gAllocations{0};

} // namespace

// GCC MISREADS A CORRECT REPLACEMENT ALLOCATOR, 53 TIMES ACROSS TWO FILES.
//
// `operator new` here calls std::malloc/std::aligned_alloc and `operator delete` calls std::free,
// which is the pairing the standard requires of a replacement pair and is what makes the counting
// below possible at all. -Wmismatched-new-delete sees `free()` inside a function named
// `operator delete` and reports the pair it is looking at as mismatched — it reasons about the
// NAMES rather than about which allocator actually produced the pointer, and a replacement pair is
// exactly the case where those come apart.
//
// SUPPRESSED RATHER THAN WORKED AROUND, and scoped to these functions rather than to the file,
// because there is nothing to fix: any rewrite that silences it would break the pairing. It is
// suppressed AT ALL because 53 false positives are not untidiness — they were the noise floor that
// hid a real dangling-pointer warning in env_card.cpp for a day, in the fix for a bug that had been
// reported by name. A warning nobody reads is worse than no warning.
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
	// A float-only device's state asks for 4 — see portable_aligned_alloc.h for what macOS does with it.
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
#include "nothrow_new_forwarding.h"

namespace {

//-----------------//
// A minimal host  //
//-----------------//

const void* CLAP_ABI hostGetExtension(const clap_host*, const char*) {
	// A host is allowed to support nothing. Answering null to everything is the harshest legal
	// environment, and the one where a plugin that assumed an extension would crash.
	return nullptr;
}

void CLAP_ABI hostRequestRestart(const clap_host*) {}
void CLAP_ABI hostRequestProcess(const clap_host*) {}
void CLAP_ABI hostRequestCallback(const clap_host*) {}

const clap_host kHost = {
    CLAP_VERSION_INIT, nullptr,       "feathers tests",     "Owlet Records",
    "",                "1.0",         hostGetExtension,     hostRequestRestart,
    hostRequestProcess, hostRequestCallback,
};

//-------------------//
// Event plumbing    //
//-------------------//

/// A clap_input_events over a list of events, which is all the interface really is. Storage is a
/// byte buffer because the event types have different sizes and a host hands out a pointer to a
/// header.
class EventList {
public:
	void pushNote(uint16_t type, uint32_t time, int16_t key, int32_t noteId, double velocity) {
		clap_event_note event{};
		event.header.size = sizeof(event);
		event.header.time = time;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = type;
		event.header.flags = 0;
		event.note_id = noteId;
		event.port_index = 0;
		event.channel = 0;
		event.key = key;
		event.velocity = velocity;
		notes_.push_back(event);
		order_.push_back({Kind::Note, notes_.size() - 1});
	}

	void pushParam(uint32_t time, clap_id paramId, double value) {
		clap_event_param_value event{};
		event.header.size = sizeof(event);
		event.header.time = time;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_VALUE;
		event.header.flags = 0;
		event.param_id = paramId;
		event.cookie = nullptr;
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.value = value;
		params_.push_back(event);
		order_.push_back({Kind::Param, params_.size() - 1});
	}

	/// A global `CLAP_EVENT_PARAM_MOD` (no note, key or channel): the host's modulation of the whole param.
	void pushParamMod(uint32_t time, clap_id paramId, double amount) {
		clap_event_param_mod event{};
		event.header.size = sizeof(event);
		event.header.time = time;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_PARAM_MOD;
		event.header.flags = 0;
		event.param_id = paramId;
		event.cookie = nullptr;
		event.note_id = -1;
		event.port_index = -1;
		event.channel = -1;
		event.key = -1;
		event.amount = amount;
		mods_.push_back(event);
		order_.push_back({Kind::Mod, mods_.size() - 1});
	}

	/// A MIDI 1.0 event as a host on the MIDI dialect sends it: three data bytes, the status byte's low nibble the channel.
	void pushMidi(uint32_t time, uint8_t status, uint8_t data1, uint8_t data2) {
		clap_event_midi event{};
		event.header.size = sizeof(event);
		event.header.time = time;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_MIDI;
		event.header.flags = 0;
		event.port_index = 0;
		event.data[0] = status;
		event.data[1] = data1;
		event.data[2] = data2;
		midis_.push_back(event);
		order_.push_back({Kind::Midi, midis_.size() - 1});
	}

	/// A `CLAP_EVENT_TRANSPORT` at `time` — a host announcing a transport change inside the block.
	void pushTransport(uint32_t time, const clap_event_transport& transport) {
		clap_event_transport event = transport;
		event.header.size = sizeof(event);
		event.header.time = time;
		event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		event.header.type = CLAP_EVENT_TRANSPORT;
		event.header.flags = 0;
		transports_.push_back(event);
		order_.push_back({Kind::Transport, transports_.size() - 1});
	}

	void clear() {
		midis_.clear();
		notes_.clear();
		params_.clear();
		mods_.clear();
		transports_.clear();
		order_.clear();
	}

	clap_input_events input() const {
		// ctx is a void*, and the plugin only ever reads through it.
		return clap_input_events{const_cast<EventList*>(this), sizeFn, getFn};
	}

private:
	enum class Kind : uint8_t { Param, Note, Mod, Midi, Transport };
	struct Ref {
		Kind kind;
		size_t index;
	};

	static uint32_t CLAP_ABI sizeFn(const clap_input_events* list) {
		return static_cast<uint32_t>(static_cast<const EventList*>(list->ctx)->order_.size());
	}

	static const clap_event_header* CLAP_ABI getFn(const clap_input_events* list, uint32_t index) {
		const auto* self = static_cast<const EventList*>(list->ctx);
		const Ref& ref = self->order_[index];
		switch (ref.kind) {
		case Kind::Note:
			return &self->notes_[ref.index].header;
		case Kind::Mod:
			return &self->mods_[ref.index].header;
		case Kind::Midi:
			return &self->midis_[ref.index].header;
		case Kind::Transport:
			return &self->transports_[ref.index].header;
		case Kind::Param:
			break;
		}
		return &self->params_[ref.index].header;
	}

	// Reserved up front so a push during a test cannot be the allocation the counter catches, and so
	// the pointers handed to the plugin stay valid.
	std::vector<clap_event_note> notes_{[] {
		std::vector<clap_event_note> v;
		v.reserve(64);
		return v;
	}()};
	std::vector<clap_event_param_value> params_{[] {
		std::vector<clap_event_param_value> v;
		v.reserve(64);
		return v;
	}()};
	std::vector<clap_event_midi> midis_{[] {
		std::vector<clap_event_midi> v;
		v.reserve(64);
		return v;
	}()};
	std::vector<clap_event_param_mod> mods_{[] {
		std::vector<clap_event_param_mod> v;
		v.reserve(64);
		return v;
	}()};
	std::vector<clap_event_transport> transports_{[] {
		std::vector<clap_event_transport> v;
		v.reserve(8);
		return v;
	}()};
	std::vector<Ref> order_{[] {
		std::vector<Ref> v;
		v.reserve(128);
		return v;
	}()};
};

bool CLAP_ABI outputTryPush(const clap_output_events*, const clap_event_header*) {
	return true;
}

/// **WHAT THE PLUGIN TOLD THE HOST** — every param value and gesture it pushed, in order, as a host's output
/// queue receives them. Reserved up front, so a push inside `process` is not an allocation the counter
/// catches.
struct OutputLog {
	struct Event {
		uint16_t type = 0;
		clap_id id = 0;
		double value = 0.0;
		int32_t noteId = -1; ///< a NOTE_END's, as the host issued it
		int16_t key = -1;
		int16_t channel = -1;
		int16_t port = -1;
	};
	std::vector<Event> events;

	OutputLog() { events.reserve(1024); }

	static bool CLAP_ABI tryPush(const clap_output_events* list, const clap_event_header* h) {
		auto* self = static_cast<OutputLog*>(list->ctx);
		if (self->events.size() == self->events.capacity() || h->space_id != CLAP_CORE_EVENT_SPACE_ID) {
			return self->events.size() != self->events.capacity();
		}
		Event e;
		e.type = h->type;
		if (h->type == CLAP_EVENT_PARAM_VALUE) {
			const auto* v = reinterpret_cast<const clap_event_param_value*>(h);
			e.id = v->param_id;
			e.value = v->value;
		}
		else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
			e.id = reinterpret_cast<const clap_event_param_gesture*>(h)->param_id;
		}
		else if (h->type == CLAP_EVENT_NOTE_END) {
			const auto* n = reinterpret_cast<const clap_event_note*>(h);
			e.noteId = n->note_id;
			e.key = n->key;
			e.channel = n->channel;
			e.port = n->port_index;
		}
		else {
			return true;
		}
		self->events.push_back(e);
		return true;
	}
	clap_output_events out() { return clap_output_events{this, tryPush}; }

	/// The events on one param id, in order.
	[[nodiscard]] std::vector<Event> on(clap_id id) const {
		std::vector<Event> out;
		for (const Event& e : events) {
			if (e.id == id) {
				out.push_back(e);
			}
		}
		return out;
	}
};

/// The plugin, stood up the way a host stands one up, and torn down the same way.
class HostedPlugin {
public:
	/// The default host answers null to every extension; a case models a different host by passing
	/// its own (the DE case's, whose timer registers and never fires).
	///
	/// `beforeActivate`, when given, runs between `init` and the first `activate` — a host that loads a saved
	/// state into a fresh instance before it ever activates it (Bitwig's plugin host, on every engine start).
	explicit HostedPlugin(const clap_host* host = &kHost, void (*beforeActivate)(const clap_plugin*, void*) = nullptr,
	                      void* context = nullptr) {
		REQUIRE(clap_entry.init("feathers-test"));

		const auto* factory =
		    static_cast<const clap_plugin_factory*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
		REQUIRE(factory != nullptr);
		REQUIRE(factory->get_plugin_count(factory) == 1);

		const clap_plugin_descriptor* descriptor = factory->get_plugin_descriptor(factory, 0);
		REQUIRE(descriptor != nullptr);

		plugin_ = factory->create_plugin(factory, host, descriptor->id);
		REQUIRE(plugin_ != nullptr);
		REQUIRE(plugin_->init(plugin_));
		if (beforeActivate != nullptr) {
			beforeActivate(plugin_, context);
		}
		REQUIRE(plugin_->activate(plugin_, kSampleRate, 1, kMaxFrames));
		REQUIRE(plugin_->start_processing(plugin_));

		left_.resize(kMaxFrames, 0.0f);
		right_.resize(kMaxFrames, 0.0f);
		channels_[0] = left_.data();
		channels_[1] = right_.data();
	}

	~HostedPlugin() {
		plugin_->stop_processing(plugin_);
		plugin_->deactivate(plugin_);
		plugin_->destroy(plugin_);
		clap_entry.deinit();
	}

	HostedPlugin(const HostedPlugin&) = delete;
	HostedPlugin& operator=(const HostedPlugin&) = delete;

	/// Room for blocks of `frames` — for a case that reactivates with a larger `max_frames_count`.
	void growBuffers(uint32_t frames) {
		left_.resize(std::max<size_t>(left_.size(), frames), 0.0f);
		right_.resize(std::max<size_t>(right_.size(), frames), 0.0f);
		channels_[0] = left_.data();
		channels_[1] = right_.data();
	}

	/// One process() call. Returns its status; the samples land in `left()` and `right()`.
	/// `log`, when given, receives what the plugin pushes to the host's output queue.
	clap_process_status run(uint32_t frames, const EventList& events, OutputLog* log = nullptr) {
		clap_audio_buffer output{};
		output.data32 = channels_;
		output.data64 = nullptr;
		output.channel_count = 2;
		output.latency = 0;
		output.constant_mask = 0;

		const clap_input_events in = events.input();
		const clap_output_events out = log != nullptr ? log->out() : clap_output_events{nullptr, outputTryPush};

		clap_process process{};
		process.steady_time = steadyTime_;
		process.frames_count = frames;
		process.transport = transport_.has_value() ? &transport_.value() : nullptr;
		process.audio_inputs = inputs_;
		process.audio_inputs_count = inputCount_;
		process.audio_outputs = &output;
		process.audio_outputs_count = 1;
		process.in_events = &in;
		process.out_events = &out;

		steadyTime_ += frames;

		const long before = gAllocations.load(std::memory_order_relaxed);
		const clap_process_status status = plugin_->process(plugin_, &process);
		allocationsInLastProcess_ = gAllocations.load(std::memory_order_relaxed) - before;
		return status;
	}

	/// THE HOST'S TRANSPORT, which a real one may or may not offer. Absent by default, because that
	/// is the state every other case in this file was written in and the state a host is entitled to
	/// leave us in.
	void setTransport(double songSeconds, bool playing, bool secondsTimeline = true) {
		clap_event_transport t{};
		t.header.size = sizeof(t);
		t.header.type = CLAP_EVENT_TRANSPORT;
		t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		// The ternary's arms are an enumerator and a literal, which -Wextra flags as mixing an
		// enumerated with a non-enumerated type. CLAP's transport flags are a plain uint32 bitfield
		// rather than a scoped enum, so the cast states the type the field already has.
		t.flags = playing ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u;
		if (secondsTimeline) {
			t.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
			t.song_pos_seconds =
			    static_cast<clap_sectime>(std::llround(songSeconds * CLAP_SECTIME_FACTOR));
		}
		else {
			// The other road: beats and a tempo, no seconds timeline. 120 bpm makes a beat half a
			// second, so the same instant is twice the beat count.
			t.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_TEMPO;
			t.tempo = 120.0;
			t.song_pos_beats =
			    static_cast<clap_beattime>(std::llround(songSeconds * 2.0 * CLAP_BEATTIME_FACTOR));
		}
		transport_ = t;
	}

	void clearTransport() { transport_.reset(); }

	/// THE TRANSPORT AS GIVEN, for a case that builds its own (`songTransport`).
	void setTransportEvent(const clap_event_transport& t) { transport_ = t; }

	/// THE HOST'S AUDIO INPUTS for every later `run` (the sidechain, input 0): none by default.
	void setInputs(const clap_audio_buffer* inputs, uint32_t count) {
		inputs_ = inputs;
		inputCount_ = count;
	}

	/// A transport carrying only these FLAGS — no timeline, no tempo — for a case about what the flags say
	/// (`CLAP_TRANSPORT_IS_RECORDING`).
	void setTransportFlags(uint32_t flags) {
		clap_event_transport t{};
		t.header.size = sizeof(t);
		t.header.type = CLAP_EVENT_TRANSPORT;
		t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		t.flags = flags;
		transport_ = t;
	}

	const std::vector<float>& left() const { return left_; }
	const std::vector<float>& right() const { return right_; }
	long allocationsInLastProcess() const { return allocationsInLastProcess_; }
	const clap_plugin* plugin() const { return plugin_; }

private:
	const clap_plugin* plugin_{nullptr};
	std::vector<float> left_;
	std::vector<float> right_;
	float* channels_[2]{};
	int64_t steadyTime_{0};
	long allocationsInLastProcess_{0};
	std::optional<clap_event_transport> transport_;
	const clap_audio_buffer* inputs_ = nullptr;
	uint32_t inputCount_ = 0;
};

float peakOf(const std::vector<float>& x, uint32_t frames) {
	float peak = 0.0f;
	for (uint32_t i = 0; i < frames; i++) {
		peak = std::max(peak, std::abs(x[i]));
	}
	return peak;
}

bool allFinite(const std::vector<float>& x, uint32_t frames) {
	for (uint32_t i = 0; i < frames; i++) {
		if (!std::isfinite(x[i])) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("a note played through the C API sounds, and stops sounding") {
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 256;

	// An explicit character, set the way a host sets one, so the test does not rest on whichever
	// zone the plugin happens to load with.
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::Path), 0.0);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);

	CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
	CHECK(allFinite(host.left(), kFrames));
	const float first = peakOf(host.left(), kFrames);
	CAPTURE(first);
	CHECK(first > 0.01f);

	events.clear();
	float held = 0.0f;
	for (int block = 0; block < 8; block++) {
		CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
		held = std::max(held, peakOf(host.left(), kFrames));
		// Stereo out of a mono synth: both channels carry the note.
		CHECK(host.left() == host.right());
	}
	CHECK(held > 0.05f);

	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();

	// The tail runs, then the plugin says it has nothing left to do. SLEEP is a promise, and it must
	// not be made while a release is still sounding. THE RELEASE IS THE DEFAULT PATCH'S ENVELOPE, routed
	// to Level (backlog EF) and a second long — a long release by design — so the bound is a few seconds of
	// blocks, not a few blocks.
	clap_process_status status = CLAP_PROCESS_CONTINUE;
	int blocks = 0;
	constexpr int kBlockBound = 4 * 48000 / static_cast<int>(kFrames); // four seconds at the rig's rate
	while (status != CLAP_PROCESS_SLEEP && blocks < kBlockBound) {
		status = host.run(kFrames, events);
		CHECK(allFinite(host.left(), kFrames));
		blocks++;
	}
	CAPTURE(blocks);
	CHECK(status == CLAP_PROCESS_SLEEP);

	// The block SLEEP is returned FROM is the one the last voice died during, so it still carries the
	// end of the release. It is the block after that must be silent, and that is the promise SLEEP
	// actually makes.
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_SLEEP);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);

	// And it took a release to get there rather than ending the moment the key came up.
	CHECK(blocks > 1);
}

TEST_CASE("additive: a note through the C API sounds, and once the release has run SLEEP is promised over exact silence") {
	// THE SLEEP RULE (CLAUDE.md: decided by the output) for the sixth engine. The partial bank reaches EXACT zero when its
	// targets do (design §5.4: a ramp's last sample is set, sub-floor targets are 0), so the release of the default
	// patch's envelope ends in a silent block and the plugin may sleep; and while the release still sounds it must not.
	// The copy of "a note played through the C API" above, on Additive, chosen the way a host chooses it.
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 256;
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType),
	                 static_cast<double>(feathers::OscType::Additive));
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
	events.clear();
	// THE ENGINE REALLY IS ADDITIVE: the host's write landed (a case that kept playing V5 would pass the rest).
	{
		const auto* params = static_cast<const clap_plugin_params*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_PARAMS));
		REQUIRE(params != nullptr);
		double engine = -1.0;
		REQUIRE(params->get_value(host.plugin(), static_cast<clap_id>(feathers::ParamId::OscType), &engine));
		CHECK(engine == static_cast<double>(feathers::OscType::Additive));
	}
	float held = 0.0f;
	for (int block = 0; block < 8; block++) {
		CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
		CHECK(allFinite(host.left(), kFrames));
		held = std::max(held, peakOf(host.left(), kFrames));
	}
	CAPTURE(held);
	CHECK(held > 0.05f);

	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();
	clap_process_status status = CLAP_PROCESS_CONTINUE;
	int blocks = 0;
	int soundingBeforeSleep = 0;
	constexpr int kBlockBound = 4 * 48000 / static_cast<int>(kFrames);
	while (status != CLAP_PROCESS_SLEEP && blocks < kBlockBound) {
		status = host.run(kFrames, events);
		soundingBeforeSleep += peakOf(host.left(), kFrames) > 0.0f ? 1 : 0;
		blocks++;
	}
	CAPTURE(blocks);
	CHECK(status == CLAP_PROCESS_SLEEP);
	// THE BLOCK THAT RETURNED SLEEP IS ITSELF EXACTLY SILENT, both channels: the promise is made by the output (CLAUDE.md;
	// the reverb-tail guard's shape), never by the voice count alone — the block the last voice dies in carries the end
	// of its release, and a SLEEP returned from it would let a host park the plugin mid-sample.
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
	CHECK(peakOf(host.right(), kFrames) == 0.0f);
	// The block after SLEEP is EXACTLY silent, both ears: no denormal and no DC left by the bank.
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_SLEEP);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
	CHECK(peakOf(host.right(), kFrames) == 0.0f);
	// And the release did sound on past the key: it took a release, not an instant cut.
	CHECK(blocks > 1);
	CHECK(soundingBeforeSleep > 0);
}

TEST_CASE("pluck: a ringing string keeps the plugin awake, and once it is exactly silent SLEEP is promised over exact silence") {
	// THE SLEEP RULE (CLAUDE.md, decided by the output) for the Karplus-Strong engine, copied from the Additive case above: a
	// string falls below −160 dB and is then ZEROED (KsString, design §1.7), so a decaying string ends on exact silence and
	// the plugin may sleep; while it rings it must not. Release at its top ("ring on"), so after the key comes up the string
	// rings for its own Decay rather than being damped, and the voice stays for as long as the default envelope keeps it.
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 256;
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Pluck));
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::KsDecay), std::log2(0.4));
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 48, 1, 1.0);
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
	events.clear();
	{
		const auto* params = static_cast<const clap_plugin_params*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_PARAMS));
		REQUIRE(params != nullptr);
		double engine = -1.0;
		REQUIRE(params->get_value(host.plugin(), static_cast<clap_id>(feathers::ParamId::OscType), &engine));
		CHECK(engine == static_cast<double>(feathers::OscType::Pluck));
	}
	float held = 0.0f;
	for (int block = 0; block < 8; block++) {
		CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
		CHECK(allFinite(host.left(), kFrames));
		held = std::max(held, peakOf(host.left(), kFrames));
	}
	CAPTURE(held);
	CHECK(held > 0.05f);
	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 48, 1, 0.0);
	host.run(kFrames, events);
	events.clear();
	clap_process_status status = CLAP_PROCESS_CONTINUE;
	int blocks = 0;
	int sleptWhileSounding = 0;
	float before = 1.0f;
	constexpr int kBlockBound = 30 * 48000 / static_cast<int>(kFrames);
	while (status != CLAP_PROCESS_SLEEP && blocks < kBlockBound) {
		status = host.run(kFrames, events);
		before = peakOf(host.left(), kFrames);
		sleptWhileSounding += status == CLAP_PROCESS_SLEEP && before > 0.0f ? 1 : 0;
		blocks++;
	}
	CAPTURE(blocks);
	CHECK(status == CLAP_PROCESS_SLEEP);
	CHECK(sleptWhileSounding == 0);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
	CHECK(peakOf(host.right(), kFrames) == 0.0f);
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_SLEEP);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
}

TEST_CASE("pluck: a released voice ends once its string is exactly silent, before the envelope's release would end it") {
	// THE DESIGN DECISION (2026-10-31; KS design §1.7): the default envelope's release holds a released voice for its own
	// length, and a short Pluck Release silences the string well inside it. The voice is then handed back, so the plugin sleeps
	// sooner than the same patch on an engine whose sound the envelope alone ends. By value: the blocks until SLEEP, Pluck
	// against V5 on the same patch and note, and both SLEEPs over exact silence.
	const auto blocksToSleep = [](feathers::OscType engine) {
		HostedPlugin host;
		EventList events;
		constexpr uint32_t kFrames = 256;
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(engine));
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::KsRelease), std::log2(0.02));
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		host.run(kFrames, events);
		events.clear();
		for (int b = 0; b < 8; ++b) {
			host.run(kFrames, events);
		}
		events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
		host.run(kFrames, events);
		events.clear();
		int blocks = 0;
		clap_process_status status = CLAP_PROCESS_CONTINUE;
		while (status != CLAP_PROCESS_SLEEP && blocks < 4 * 48000 / static_cast<int>(kFrames)) {
			status = host.run(kFrames, events);
			++blocks;
		}
		CHECK(status == CLAP_PROCESS_SLEEP);
		CHECK(peakOf(host.left(), kFrames) == 0.0f);
		return blocks;
	};
	const int pluck = blocksToSleep(feathers::OscType::Pluck);
	const int v5 = blocksToSleep(feathers::OscType::V5);
	CAPTURE(pluck);
	CAPTURE(v5);
	CHECK(pluck * 2 < v5);
}

TEST_CASE("pluck: process allocates nothing — notes, a stolen voice, a moving puck, releases and an engine switch") {
	// "process allocates nothing" above, on the Karplus-Strong engine: every string is sized at activate (KsString::prepare,
	// for C0), and a pluck builds in the synth's shared PluckBuilder, so no note, steal, puck move or release allocates.
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 128;
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Pluck));
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::KsWidth), 0.2);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	events.clear();
	for (int i = 0; i < 20; i++) {
		events.pushNote(CLAP_EVENT_NOTE_ON, static_cast<uint32_t>(i * 4), static_cast<int16_t>(24 + i * 3), i + 2, 1.0);
	}
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	for (int block = 0; block < 8; block++) {
		events.clear();
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 100.0 + 40.0 * static_cast<double>(block));
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::Path), 0.1 * static_cast<double>(block));
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AKsStretch), 0.125 * static_cast<double>(block));
		host.run(kFrames, events);
		CAPTURE(block);
		CHECK(host.allocationsInLastProcess() == 0);
	}
	events.clear();
	for (int i = 0; i < 20; i++) {
		events.pushNote(CLAP_EVENT_NOTE_OFF, static_cast<uint32_t>(i * 4), static_cast<int16_t>(24 + i * 3), i + 2, 0.0);
	}
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	events.clear();
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::V5));
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 64, 40, 1.0);
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	events.clear();
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Pluck));
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
}

TEST_CASE("drum: a ringing ride keeps the plugin awake, and once it is exactly silent SLEEP is promised over exact silence") {
	// THE SLEEP RULE (CLAUDE.md, decided by the output) for the drum engine: every mode and envelope of a hit is
	// snapped to exact zero below −160 dB, and a One-shot voice then ends itself.
	// The ride is a One-shot pad, so no note-off is sent at all: the hit alone decides.
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 256;
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Drum));
	events.pushParam(0, static_cast<clap_id>(feathers::drumParam(7, feathers::DrumControl::Decay)), std::log2(0.4));
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 51, 1, 1.0);
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
	events.clear();
	float held = 0.0f;
	for (int block = 0; block < 8; block++) {
		CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
		CHECK(allFinite(host.left(), kFrames));
		held = std::max(held, peakOf(host.left(), kFrames));
	}
	CAPTURE(held);
	CHECK(held > 1.0e-3f);
	clap_process_status status = CLAP_PROCESS_CONTINUE;
	int blocks = 0;
	int sleptWhileSounding = 0;
	constexpr int kBlockBound = 30 * 48000 / static_cast<int>(kFrames);
	while (status != CLAP_PROCESS_SLEEP && blocks < kBlockBound) {
		status = host.run(kFrames, events);
		sleptWhileSounding += status == CLAP_PROCESS_SLEEP && peakOf(host.left(), kFrames) > 0.0f ? 1 : 0;
		blocks++;
	}
	CAPTURE(blocks);
	CHECK(status == CLAP_PROCESS_SLEEP);
	CHECK(sleptWhileSounding == 0);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
	CHECK(peakOf(host.right(), kFrames) == 0.0f);
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_SLEEP);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
}

TEST_CASE("drum: process allocates nothing — kit notes, a layer, a choke, a pad's steal, the Walk and an engine switch") {
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 128;
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Drum));
	events.pushParam(0, static_cast<clap_id>(feathers::drumParam(2, feathers::DrumControl::Pan)), -0.4);
	events.pushParam(0, static_cast<clap_id>(feathers::drumParam(4, feathers::DrumControl::Note)), 36.0);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 46, 1, 1.0);
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	events.clear();
	// Every pad, the layered kick-and-clap, the closed hat choking the open, a second kick stealing the first.
	const int16_t keys[] { 36, 38, 42, 39, 45, 50, 51, 36, 60 };
	int id = 2;
	for (const int16_t k : keys) {
		events.pushNote(CLAP_EVENT_NOTE_ON, static_cast<uint32_t>(id * 4), k, id, 0.8);
		++id;
	}
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	for (int block = 0; block < 8; block++) {
		events.clear();
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::DrumWalkX), 0.1 * static_cast<double>(block));
		events.pushParam(0, static_cast<clap_id>(feathers::drumParam(1, feathers::DrumControl::FieldTone)),
		                 0.125 * static_cast<double>(block));
		host.run(kFrames, events);
		CAPTURE(block);
		CHECK(host.allocationsInLastProcess() == 0);
	}
	events.clear();
	events.pushParam(0, static_cast<clap_id>(feathers::drumParam(5, feathers::DrumControl::Kind)), 5.0); // a String tom
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 45, 40, 1.0);
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	events.clear();
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::V5));
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 64, 41, 1.0);
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
	events.clear();
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Drum));
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::DrumMode), 0.0); // Pitched
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 64, 42, 1.0);
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
}

TEST_CASE("a reverb's tail keeps the plugin awake: SLEEP is never promised while the output is still sounding") {
	// A crackle in the LATE, QUIET tail of every reverb that decays (Owl, which sustains, was clean),
	// at any buffer size and either Oversample. The plugin returned SLEEP the moment the last VOICE died — "release
	// tails and steal fades are voices" — while the fx chain was still ringing, so a host may stop calling it, or call it
	// only now and then, and the tail comes out in fragments that stop dead. SLEEP now also needs the block to be silent.
	HostedPlugin host;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(host.plugin(), feathers::fx::featherverb::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(host.plugin(), slot, "Featherverb Mix", 1.0));
	REQUIRE(chain->set_control(host.plugin(), slot, "Featherverb Decay", 0.5));
	// ACTIVATE PROVISIONS the chain the bridge holds (the road a loaded patch takes).
	const clap_plugin* p = host.plugin();
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));

	constexpr uint32_t kFrames = 256;
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	events.clear();
	for (int b = 0; b < 20; ++b) {
		host.run(kFrames, events);
	}
	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();

	// THE PROMISE, EVERY BLOCK: a block that returns SLEEP is followed by silence. Run until the plugin sleeps (or 30 s).
	int blocks = 0;
	int soundingAfterSleep = 0;
	bool slept = false;
	float peakWhenSlept = 0.0f;
	while (blocks < 30 * 48000 / static_cast<int>(kFrames)) {
		const clap_process_status status = host.run(kFrames, events);
		++blocks;
		if (status == CLAP_PROCESS_SLEEP) {
			slept = true;
			host.run(kFrames, events);
			peakWhenSlept = peakOf(host.left(), kFrames);
			soundingAfterSleep += peakWhenSlept > 0.0f ? 1 : 0;
			break;
		}
	}
	CAPTURE(blocks);
	CAPTURE(peakWhenSlept);
	CHECK(soundingAfterSleep == 0);
	// And the tail did ring on past the voice: the plugin stayed awake for it (a second or more of blocks).
	CHECK(blocks > 48000 / static_cast<int>(kFrames));
	CHECK(slept); // it does sleep in the end: a decaying reverb reaches exact silence
}

TEST_CASE("LW: HARM behind a linear-phase EQ keeps the plugin awake until it has been handed its late note — SLEEP is never promised before") {
	// THE SLEEP RULE (CLAUDE.md), copied from the reverb case above for the latency domains (backlog LW). HARM behind a
	// linear-phase EQ is handed its note eq::linearLatency late, and its harmonic starts from the NOTE, not from anything
	// the EQ holds: a host that parked the plugin in between would never hear it.
	HostedPlugin host;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t eq = chain->add_device(host.plugin(), feathers::fx::eq::kTraits.tag);
	REQUIRE(eq != UINT32_MAX);
	REQUIRE(chain->set_control(host.plugin(), eq, "Phase", 1.0));
	const uint32_t harm = chain->add_device(host.plugin(), feathers::fx::harm::kTraits.tag);
	REQUIRE(harm != UINT32_MAX);
	const clap_plugin* p = host.plugin();
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));

	constexpr uint32_t kFrames = 256;
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	events.clear();
	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();
	int blocks = 2;
	int soundingAfterSleep = 0;
	bool slept = false;
	while (blocks < 30 * 48000 / static_cast<int>(kFrames)) {
		const clap_process_status status = host.run(kFrames, events);
		++blocks;
		if (status == CLAP_PROCESS_SLEEP) {
			slept = true;
			for (int b = 0; b < 200; ++b) {
				host.run(kFrames, events);
				soundingAfterSleep += peakOf(host.left(), kFrames) > 0.0f ? 1 : 0;
			}
			break;
		}
	}
	CAPTURE(blocks);
	CHECK(soundingAfterSleep == 0);
	CHECK(static_cast<uint32_t>(blocks) * kFrames > feathers::fx::eq::linearLatency(48000.0)); // awake past the late note
	CHECK(slept);
}

TEST_CASE("a delay's repeats keep the plugin awake: SLEEP is never promised while a repeat is still to come") {
	// THE SLEEP RULE (CLAUDE.md), copied from the reverb case above for the delay (backlog MC). A delay is the device
	// whose OUTPUT can be exactly silent while its line still holds sound: between a short note and its first repeat,
	// and between repeats. A SLEEP there would let a host park the plugin and lose every repeat. EVERY MODE: each reads
	// the one line, and each must keep the plugin awake while it holds a repeat (Reverse joins when it lands).
	for (int mode = 0; mode < static_cast<int>(feathers::fx::delay::Mode::kCount); ++mode) {
		CAPTURE(mode);
		HostedPlugin host;
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		const uint32_t slot = chain->add_device(host.plugin(), feathers::fx::delay::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		REQUIRE(chain->set_control(host.plugin(), slot, "Delay Mix", 1.0));        // wet only: the gap is silent
		REQUIRE(chain->set_control(host.plugin(), slot, "Delay Sync", 2.0));       // 1 s
		REQUIRE(chain->set_control(host.plugin(), slot, "Delay Feedback", 0.3));
		REQUIRE(chain->set_control(host.plugin(), slot, "Delay Mode", static_cast<double>(mode)));
		const clap_plugin* p = host.plugin();
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));

		constexpr uint32_t kFrames = 256;
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		host.run(kFrames, events);
		events.clear();
		for (int b = 0; b < 4; ++b) {
			host.run(kFrames, events);
		}
		events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
		host.run(kFrames, events);
		events.clear();
		// THE PROMISE: after the first SLEEP, nothing more sounds — run on as if the host had kept calling, for 6 s. The
		// window is 60 s because the line is 16 s long and holds the tail until a whole line of exact zeroes has passed.
		bool slept = false;
		int sleptAt = -1;
		int soundingAfterSleep = 0;
		for (int b = 0; b < 60 * 48000 / static_cast<int>(kFrames); ++b) {
			const clap_process_status status = host.run(kFrames, events);
			if (slept && peakOf(host.left(), kFrames) + peakOf(host.right(), kFrames) > 0.0f) {
				++soundingAfterSleep;
			}
			if (status == CLAP_PROCESS_SLEEP && !slept) {
				slept = true;
				sleptAt = b;
			}
			if (slept && b > sleptAt + 6 * 48000 / static_cast<int>(kFrames)) {
				break;
			}
		}
		CAPTURE(sleptAt);
		CHECK(soundingAfterSleep == 0);
		CHECK(slept);
	}
}

TEST_CASE("NOISE on the bus keeps the plugin awake between sparse clicks, and lets it sleep at Level 0") {
	// THE SLEEP RULE (CLAUDE.md), copied from the reverb case for a GENERATOR (backlog MZ). Impulses at one a second leave
	// whole blocks EXACTLY silent between clicks, with no note held — the case a silent-output test would sleep through
	// and lose every click after. `holdsTail` says "more is coming" while Level is up; at Level 0 nothing is.
	HostedPlugin host;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(host.plugin(), feathers::fx::noise::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(host.plugin(), slot, "Noise Poly", 0.0));   // one on the bus
	REQUIRE(chain->set_control(host.plugin(), slot, "Noise Source", 1.0)); // Impulses
	REQUIRE(chain->set_control(host.plugin(), slot, "Noise Timing", 0.0)); // Regular
	REQUIRE(chain->set_control(host.plugin(), slot, "Noise Jitter", 0.0));
	REQUIRE(chain->set_control(host.plugin(), slot, "Noise Density", 0.0)); // 2^0 = one a second
	REQUIRE(chain->set_control(host.plugin(), slot, "Noise Level", 0.5));
	const clap_plugin* p = host.plugin();
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));

	constexpr uint32_t kFrames = 256;
	EventList events;
	int slept = 0;
	int silentAwake = 0; // exactly silent blocks the plugin stayed awake through
	int sounding = 0;    // blocks a click reached
	for (int b = 0; b < 6 * 48000 / static_cast<int>(kFrames); ++b) {
		const clap_process_status status = host.run(kFrames, events);
		const bool heard = peakOf(host.left(), kFrames) + peakOf(host.right(), kFrames) > 0.0f;
		sounding += heard ? 1 : 0;
		silentAwake += !heard && status != CLAP_PROCESS_SLEEP ? 1 : 0;
		slept += status == CLAP_PROCESS_SLEEP ? 1 : 0;
	}
	CAPTURE(sounding);
	CAPTURE(silentAwake);
	CHECK(slept == 0);
	CHECK(silentAwake > 100); // non-vacuity: the gaps really were exactly silent
	CHECK(sounding >= 5);     // and the clicks kept coming, about one a second for six

	// AT LEVEL 0 IT ADDS NOTHING AND HOLDS NOTHING: the plugin may sleep, and nothing sounds after it does.
	REQUIRE(chain->set_control(host.plugin(), slot, "Noise Level", 0.0));
	bool sleptAtZero = false;
	int soundingAfterSleep = 0;
	for (int b = 0; b < 4 * 48000 / static_cast<int>(kFrames); ++b) {
		const clap_process_status status = host.run(kFrames, events);
		if (sleptAtZero && peakOf(host.left(), kFrames) + peakOf(host.right(), kFrames) > 0.0f) {
			++soundingAfterSleep;
		}
		sleptAtZero = sleptAtZero || status == CLAP_PROCESS_SLEEP;
	}
	CHECK(sleptAtZero);
	CHECK(soundingAfterSleep == 0);
}

namespace {

/// RESONATE'S BODY WITH A FEATHERVERB IN ITS DRONE FX LANE (Mix 1, a long Decay) — built through the device's own
/// container ops, exactly as its panel's lane editor leaves it, and saved as a patch would save it.
std::vector<uint8_t> resonateBodyWithDroneReverb() {
	namespace rsn = feathers::fx::resonate;
	const feathers::device::AudioOps& audio = *rsn::kDescriptor.audio;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	rsn::kDescriptor.container->adopt(mem, feathers::hostCatalogue());
	audio.prepare(mem, 48000.0, 4096);
	feathers::rack::RackBridge* lane = rsn::laneChainOf(mem, rsn::Lane::Drone)->bridge();
	REQUIRE(lane != nullptr);
	const feathers::DeviceInstanceId verb = lane->addDevice(feathers::fx::featherverb::kTraits.tag);
	REQUIRE(verb != feathers::kNoInstance);
	const auto& params = feathers::fx::featherverb::kDescriptor.params;
	for (size_t i = 0; i < params.size(); ++i) {
		if (std::string_view(params[i].name) == "Featherverb Mix") {
			lane->setField(verb, i, 1.0f);
		}
		if (std::string_view(params[i].name) == "Featherverb Decay") {
			lane->setField(verb, i, 0.6f);
		}
	}
	std::vector<uint8_t> body;
	const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                                       auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                                       out->insert(out->end(), data, data + bytes);
	                                       } };
	REQUIRE(rsn::kDescriptor.container->saveState(mem, sink));
	audio.destruct(mem);
	::operator delete(mem, std::align_val_t { audio.stateAlign });
	return body;
}

/// BLOCKS FROM A NOTE'S RELEASE TO SLEEP through a Resonate (its own drone short), with or without a reverb in its
/// DRONE FX lane; and how many blocks SOUNDED after a SLEEP (the promise: none).
struct SleepRun {
	int blocks = 0;
	bool slept = false;
	int soundingAfterSleep = 0;
};

SleepRun resonateSleep(bool reverbInDroneLane) {
	HostedPlugin host;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(host.plugin(), feathers::fx::resonate::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(host.plugin(), slot, "Resonate Mix", 1.0));
	REQUIRE(chain->set_control(host.plugin(), slot, "Resonate Drone Decay", -3.0));
	const clap_plugin* p = host.plugin();
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));
	constexpr uint32_t kFrames = 256;
	EventList events;
	host.run(kFrames, events);
	if (reverbInDroneLane) {
		const std::vector<uint8_t> body = resonateBodyWithDroneReverb();
		REQUIRE(chain->edit_device_body(p, slot, body.data(), body.size()));
	}
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	events.clear();
	for (int b = 0; b < 20; ++b) {
		host.run(kFrames, events);
	}
	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();
	SleepRun run;
	while (run.blocks < 40 * 48000 / static_cast<int>(kFrames)) {
		const clap_process_status status = host.run(kFrames, events);
		++run.blocks;
		if (status == CLAP_PROCESS_SLEEP) {
			run.slept = true;
			host.run(kFrames, events);
			run.soundingAfterSleep += peakOf(host.left(), kFrames) > 0.0f ? 1 : 0;
			break;
		}
	}
	return run;
}

} // namespace

TEST_CASE("a reverb in Resonate's DRONE FX lane keeps the plugin awake: SLEEP waits for the lane's tail, not the string") {
	// THE SLEEP RULE (CLAUDE.md) FOR A TAIL INSIDE A DEVICE: Resonate's own string parks once its drone has rung down,
	// while a reverb in its DRONE FX lane is still ringing — and the output, not the string, decides SLEEP.
	const SleepRun alone = resonateSleep(false);
	const SleepRun lane = resonateSleep(true);
	CAPTURE(alone.blocks);
	CAPTURE(lane.blocks);
	CHECK(alone.slept);
	CHECK(lane.slept); // a decaying reverb ends in exact zero, so the plugin does sleep in the end
	CHECK(alone.soundingAfterSleep == 0);
	CHECK(lane.soundingAfterSleep == 0);
	// AND THE LANE'S TAIL IS WHAT KEPT IT AWAKE: a second and more past the string alone.
	CHECK(lane.blocks > alone.blocks + 48000 / 256);
}

TEST_CASE("a linear-phase EQ keeps the plugin awake while a note is still inside its latency") {
	// THE REVERB CASE'S PROMISE, FOR A DEVICE THAT HOLDS AUDIO WITHOUT SOUNDING IT (EQ step 7). A flat
	// linear-phase EQ is a pure delay of 9216 samples: a note shorter than that has ended — no voice sounds —
	// while every block the plugin has put out is still exactly silent, because the note is in the delay.
	// A SLEEP there is a promise broken the moment the note comes out, and a host holding the plugin asleep
	// never hears it. So after the first SLEEP nothing may sound, and the note must have come out before it.
	HostedPlugin host;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(host.plugin(), feathers::fx::eq::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(host.plugin(), slot, "Phase", 1.0));
	const clap_plugin* p = host.plugin();
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));

	constexpr uint32_t kFrames = 256;
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	events.clear();
	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();

	int blocks = 0;
	int soundingBefore = 0;
	int soundingAfterSleep = 0;
	bool slept = false;
	while (blocks < 10 * 48000 / static_cast<int>(kFrames)) {
		const clap_process_status status = host.run(kFrames, events);
		++blocks;
		const bool sounding = peakOf(host.left(), kFrames) > 0.0f;
		if (slept) {
			soundingAfterSleep += sounding ? 1 : 0;
		}
		else {
			soundingBefore += sounding ? 1 : 0;
			slept = status == CLAP_PROCESS_SLEEP;
		}
	}
	CAPTURE(blocks);
	CHECK(soundingBefore > 0);       // the note came out, through the delay, before any SLEEP
	CHECK(soundingAfterSleep == 0);  // and nothing sounded after one
	CHECK(slept);                    // it does sleep in the end
}

TEST_CASE("Resonate in the fx lane keeps the plugin awake while it rings after the notes, at any Bow") {
	// THE SAME GUARD AS THE REVERB'S, FOR THE DEVICE WHOSE OUTPUT OUTLIVES ITS INPUT BY DESIGN. The synth
	// plays a note into the lane; the note and its voice end; Resonate's string, plucked and bowed by that
	// voice, is still being heard through its drone head — a sympathetic string the input played, ringing down
	// on its decay (2026-09-26). SLEEP must wait for the output.
	auto render = [](double bow, int seconds, int& blocksBeforeSleep, int& soundingAfterSleep, int& silentBlocks) {
		HostedPlugin host;
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		const uint32_t slot = chain->add_device(host.plugin(), feathers::fx::resonate::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		REQUIRE(chain->set_control(host.plugin(), slot, "Resonate Bow", bow));
		const clap_plugin* p = host.plugin();
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));

		constexpr uint32_t kFrames = 256;
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		host.run(kFrames, events);
		events.clear();
		for (int b = 0; b < 20; ++b) {
			host.run(kFrames, events);
		}
		events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
		host.run(kFrames, events);
		events.clear();

		blocksBeforeSleep = -1;
		soundingAfterSleep = 0;
		silentBlocks = 0;
		for (int b = 0; b < seconds * 48000 / static_cast<int>(kFrames); ++b) {
			const clap_process_status status = host.run(kFrames, events);
			silentBlocks += peakOf(host.left(), kFrames) == 0.0f ? 1 : 0;
			if (status == CLAP_PROCESS_SLEEP) {
				blocksBeforeSleep = b + 1;
				host.run(kFrames, events);
				soundingAfterSleep = peakOf(host.left(), kFrames) > 0.0f ? 1 : 0;
				break;
			}
		}
	};
	// BOW 0: a tail that ends. It must not sleep while sounding, must ring past the voice, and does sleep.
	int before = 0;
	int soundingAfter = 0;
	int silent = 0;
	render(0.0, 120, before, soundingAfter, silent); // the drone rings for its Decay, 12 s by default since 2026-09-28
	CAPTURE(before);
	MESSAGE("Resonate at Bow 0 in the lane: slept after " << before << " blocks of 256 past the note-off");
	CHECK(soundingAfter == 0);
	CHECK(before > 48000 / 256 / 5); // rang well past 200 ms
	CHECK(before > 0);               // and slept in the end
	// BOW UP (its default, 0.25): the string keeps itself moving, but with no note held the drone is what reads
	// it, and the drone is played by the input (2026-09-26): it rings down too. The promise is the same —
	// never SLEEP while sounding — and it does end. (Before the drone followed the input, this rang for ever.)
	render(0.25, 120, before, soundingAfter, silent);
	CAPTURE(before);
	MESSAGE("Resonate at Bow 0.25 in the lane: slept after " << before << " blocks of 256 past the note-off");
	CHECK(soundingAfter == 0);
	CHECK(before > 48000 / 256 / 5);
	CHECK(before > 0);
	(void)silent;
}

TEST_CASE("process allocates nothing") {
	// The claim that matters for a real-time thread, and the one the voice pool, the preallocated
	// voice manager and the character bank's fixed slots exist to support.
	// First, that the counter is capable of counting at all. A test that watches for allocations
	// through an interposition that silently stopped working would pass forever and mean nothing, so
	// the setup — which does allocate, the voice manager alone is a megabyte of key state — has to
	// register.
	const long beforeSetup = gAllocations.load(std::memory_order_relaxed);
	HostedPlugin host;
	REQUIRE(gAllocations.load(std::memory_order_relaxed) > beforeSetup);

	EventList events;
	constexpr uint32_t kFrames = 128;

	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	// The first call is where anything lazy would happen — the first character build, the first
	// voice, the first table — so it is checked like all the others rather than excused.
	CHECK(host.allocationsInLastProcess() == 0);

	events.clear();
	// Sixteen voices, which is the polyphony limit, so the pool is exhausted and the next note has to
	// steal. Both are places a naive implementation would reach for the heap.
	for (int i = 0; i < 20; i++) {
		events.pushNote(CLAP_EVENT_NOTE_ON, static_cast<uint32_t>(i * 4), static_cast<int16_t>(48 + i),
		                i + 2, 1.0);
	}
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);

	events.clear();
	// A character change every block, which is what an automated zone does: the bank rebuilds tables
	// and swaps them, on the audio thread, from fixed storage.
	for (int block = 0; block < 8; block++) {
		events.clear();
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone),
		                 100.0 + 40.0 * static_cast<double>(block));
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::Path),
		                 0.1 * static_cast<double>(block));
		host.run(kFrames, events);
		CAPTURE(block);
		CHECK(host.allocationsInLastProcess() == 0);
	}

	events.clear();
	for (int i = 0; i < 20; i++) {
		events.pushNote(CLAP_EVENT_NOTE_OFF, static_cast<uint32_t>(i * 4),
		                static_cast<int16_t>(48 + i), i + 2, 0.0);
	}
	host.run(kFrames, events);
	CHECK(host.allocationsInLastProcess() == 0);
}

TEST_CASE("events land on the sample they are stamped with") {
	// A note-on at frame 200 of a 256-sample block must leave the first 200 samples untouched. If
	// events were applied at the front of the block instead — which is what the binding did before
	// the voice layer — every note in a take would be early by up to a block.
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 256;
	constexpr uint32_t kOnset = 200;

	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
	events.pushNote(CLAP_EVENT_NOTE_ON, kOnset, 60, 1, 1.0);
	host.run(kFrames, events);

	for (uint32_t i = 0; i < kOnset; i++) {
		REQUIRE(host.left()[i] == 0.0f);
	}
	// And something after it, which is what says the silence above was timing rather than a note that
	// never started. THE LEVEL IS THE DEFAULT PATCH'S ENVELOPE (backlog EF), a control-rate modulator
	// ticked every 64 samples: a note landing between two ticks is gated at the next one, so the tail of
	// this block may still read zero, and the sound is asserted over this block AND the next.
	float after = 0.0f;
	for (uint32_t i = kOnset; i < kFrames; i++) {
		after = std::max(after, std::abs(host.left()[i]));
	}
	events.clear();
	host.run(kFrames, events);
	for (uint32_t i = 0; i < kFrames; i++) {
		after = std::max(after, std::abs(host.left()[i]));
	}
	CHECK(after > 0.0f);
}

TEST_CASE("a note expression is ignored without disturbing the note") {
	// Declared unsupported, so a host should not send one — but hosts do, and the note has to keep
	// playing rather than the plugin misreading a field it does not implement.
	HostedPlugin host;
	EventList events;
	constexpr uint32_t kFrames = 256;

	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	events.clear();
	host.run(kFrames, events);
	const float before = peakOf(host.left(), kFrames);

	clap_event_note_expression expression{};
	expression.header.size = sizeof(expression);
	expression.header.time = 0;
	expression.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	expression.header.type = CLAP_EVENT_NOTE_EXPRESSION;
	expression.header.flags = 0;
	expression.expression_id = CLAP_NOTE_EXPRESSION_VOLUME;
	expression.note_id = 1;
	expression.port_index = 0;
	expression.channel = 0;
	expression.key = 60;
	expression.value = 0.25;

	struct OneEvent {
		const clap_event_header* header;
		static uint32_t CLAP_ABI sizeFn(const clap_input_events*) { return 1; }
		static const clap_event_header* CLAP_ABI getFn(const clap_input_events* list, uint32_t) {
			return static_cast<const OneEvent*>(list->ctx)->header;
		}
	} one{&expression.header};

	const clap_input_events in{&one, OneEvent::sizeFn, OneEvent::getFn};
	const clap_output_events out{nullptr, outputTryPush};

	std::vector<float> left(kFrames, 0.0f);
	std::vector<float> right(kFrames, 0.0f);
	float* channels[2] = {left.data(), right.data()};
	clap_audio_buffer output{};
	output.data32 = channels;
	output.channel_count = 2;

	clap_process process{};
	process.frames_count = kFrames;
	process.audio_outputs = &output;
	process.audio_outputs_count = 1;
	process.in_events = &in;
	process.out_events = &out;

	CHECK(host.plugin()->process(host.plugin(), &process) == CLAP_PROCESS_CONTINUE);
	CHECK(allFinite(left, kFrames));
	CHECK(peakOf(left, kFrames) == doctest::Approx(before).epsilon(0.2));
}

/// **A NOTE IN A FLUSH IS HOST MISBEHAVIOUR, AND ONLY A RELEASE BUILD SURVIVES IT.** Without NDEBUG (a
/// Debug build, and the sanitizer build, which strips it on purpose) `feathers_clap.cpp`'s PluginBase has
/// clap-helpers TERMINATE on it before the plugin sees the list — the developer half of the convention.
/// The case below is about the musician's half, so it runs where that half is the one compiled.
#if defined(NDEBUG)
constexpr bool kHostMisbehaviourTerminates = false;
#else
constexpr bool kHostMisbehaviourTerminates = true;
#endif

TEST_CASE("a flush carries parameters and nothing else" * doctest::skip(kHostMisbehaviourTerminates)) {
	// clap_plugin_params.flush is how a host moves parameters while the plugin is NOT processing, and
	// it may arrive on the main thread. A note event in that list is a host bug, and obeying it would
	// start a voice from whatever thread the flush came in on, into a synth the audio thread may be
	// reading. The contract says parameters; the threading says the same thing more loudly.
	HostedPlugin host;
	const auto* params =
	    static_cast<const clap_plugin_params*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);

	EventList events;
	events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);

	const clap_input_events in = events.input();
	const clap_output_events out{nullptr, outputTryPush};
	params->flush(host.plugin(), &in, &out);

	// The parameter took effect...
	double zone = 0.0;
	REQUIRE(params->get_value(host.plugin(), static_cast<clap_id>(feathers::ParamId::AZone), &zone));
	CHECK(zone == doctest::Approx(17.0));

	// ...and the note in the same list did not. Silence, and nothing to sleep off.
	EventList empty;
	constexpr uint32_t kFrames = 256;
	CHECK(host.run(kFrames, empty) == CLAP_PROCESS_SLEEP);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
}

TEST_CASE("a host that cannot drive the editor is refused an editor, and still gets audio") {
	// THE PLATFORM'S EMBED API, one per backend: X11 on Linux, Cocoa on macOS.
	constexpr const char* kEmbedApi =
#if defined(__APPLE__)
	    CLAP_WINDOW_API_COCOA;
#else
	    CLAP_WINDOW_API_X11;
#endif
	// The X11 editor only ever draws inside main-thread time the host hands back, through
	// clap_host_posix_fd_support or clap_host_timer_support. This test's host answers null to every
	// extension, so it offers neither — and on X11 the honest answer to it is no editor, rather
	// than a window that opens and then never paints or responds.
	//
	// macOS is not that: AppKit delivers paint (the MTKView display link) and input (the responder
	// chain) itself, and the extensions only carry parameter changes — so a host offering neither
	// still gets an editor there. What it loses is the promptness of the parameter tick, and
	// nothing else.
	//
	// The half that matters more either way: whatever the editor does, the synth plays. Nothing on
	// the path from a note to a sample runs through the GUI.
	HostedPlugin host;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_GUI));

	if (gui != nullptr) {
		// The backend is built, so the API question and the can-you-drive-it question have different
		// answers: the platform's embedding is supported in principle...
		CHECK(gui->is_api_supported(host.plugin(), kEmbedApi, false));
		CHECK_FALSE(gui->is_api_supported(host.plugin(), kEmbedApi, true));
#if defined(__APPLE__)
		// ...and creating one against this host is granted: AppKit drives the editor, so this host
		// can show it. Destroyed here because clap-helpers reports a leak to the host otherwise.
		CHECK(gui->create(host.plugin(), kEmbedApi, false));
		gui->destroy(host.plugin());
#else
		// ...and creating one against this host is still refused.
		CHECK_FALSE(gui->create(host.plugin(), kEmbedApi, false));
#endif
	}
	// On a platform with no backend the extension is absent entirely, which is the same promise made
	// a different way: a host is never offered an editor that cannot open.

	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	constexpr uint32_t kFrames = 512;
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_CONTINUE);
	CHECK(peakOf(host.left(), kFrames) > 0.0f);
}

namespace {

/// A stream that keeps its bytes, so a save can be handed straight back to a load.
struct StateBytes {
	std::vector<uint8_t> bytes;
	size_t read = 0;

	static int64_t CLAP_ABI writeFn(const clap_ostream* stream, const void* buffer, uint64_t size) {
		auto* self = static_cast<StateBytes*>(stream->ctx);
		const auto* from = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), from, from + size);
		return static_cast<int64_t>(size);
	}
	static int64_t CLAP_ABI readFn(const clap_istream* stream, void* buffer, uint64_t size) {
		auto* self = static_cast<StateBytes*>(stream->ctx);
		const size_t left = self->bytes.size() - self->read;
		const size_t take = size < left ? static_cast<size_t>(size) : left;
		std::memcpy(buffer, self->bytes.data() + self->read, take);
		self->read += take;
		return static_cast<int64_t>(take);
	}
	clap_ostream out() { return clap_ostream{this, writeFn}; }
	clap_istream in() { return clap_istream{this, readFn}; }
};

bool containsTag(const std::vector<uint8_t>& bytes, const char* tag) {
	const std::string needle(tag);
	return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

} // namespace

TEST_CASE("a patch round-trips through the real state road, gamma block and all") {
	// THE SEAM WALKED WHOLE. Every piece of the gamma block is tested on its own — the writer, the
	// reader, the bridge, the decode — and this is the one case that runs them through the plugin's
	// actual clap_plugin_state, which is the only path a host ever uses.
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	// The block is actually in the stream rather than merely believed to be — a save that quietly
	// skipped it would round-trip perfectly and lose the base on every patch.
	CHECK(containsTag(saved.bytes, "FSG1"));

	clap_istream in = saved.in();
	CHECK(state->load(host.plugin(), &in));
}

namespace {

/// Where a tagged block begins, searched from `from` so the blocks are located in the order they are
/// written rather than at whichever four bytes of parameter data happen to spell a tag first.
/// Returns the size when there is no such block.
size_t blockAt(const std::vector<uint8_t>& bytes, const char* tag, size_t from = 0) {
	const std::string needle(tag);
	const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(from);
	return static_cast<size_t>(std::search(begin, bytes.end(), needle.begin(), needle.end())
	                           - bytes.begin());
}

/// Every slot's live place as a fresh host holds it — the parameter table's own defaults. What
/// the plugin's save writes into the record's dead x,y slots, so a hand-built patch that wants to
/// resave bytes-identical passes exactly this.
std::array<feathers::AnchorPoint, feathers::kMaxSounds> defaultLivePlaces() {
	std::array<feathers::AnchorPoint, feathers::kMaxSounds> places {};
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		places[slot] = {
			feathers::findParam(
			    static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::PlaceX)))
			    ->def,
			feathers::findParam(
			    static_cast<clap_id>(feathers::soundParam(slot, feathers::SoundField::PlaceY)))
			    ->def
		};
	}
	return places;
}

/// The bytes from a block's tag to the end of the patch, ready to be read by that block's loader.
StateBytes from(const std::vector<uint8_t>& bytes, const char* tag) {
	StateBytes slice;
	const size_t at = blockAt(bytes, tag);
	REQUIRE(at < bytes.size());
	slice.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
	return slice;
}

/// THE PLUGIN'S OWN SAVE WITHOUT ITS END MARKER (kStateVersion 50, `state/state_trailer.h`) — what a hand-built
/// patch is spliced from. REQUIREs the marker, so a save that lost it cannot be spliced from unnoticed.
std::vector<uint8_t> payloadOf(const std::vector<uint8_t>& sealed) {
	size_t payload = 0;
	REQUIRE(feathers::state::checkTrailer(sealed, &payload) == feathers::state::TrailerCheck::Ok);
	return { sealed.begin(), sealed.begin() + static_cast<std::ptrdiff_t>(payload) };
}

/// A HAND-BUILT PATCH, SEALED as the plugin seals its own save, so it loads as the whole v50 stream it is.
void seal(StateBytes& patch) {
	feathers::state::appendTrailer(patch.bytes);
}

/// A HAND-BUILT PATCH THAT STOPPED SHORT, FINISHED: every block from `tag` on taken from `saved` (the plugin's own
/// save), then sealed. Since 50 a stream missing a block is refused, so a case about one block writes the rest.
void finishFrom(StateBytes& patch, const std::vector<uint8_t>& saved, const char* tag) {
	const std::vector<uint8_t> payload = payloadOf(saved);
	const size_t at = blockAt(payload, tag);
	REQUIRE(at < payload.size());
	patch.bytes.insert(patch.bytes.end(), payload.begin() + static_cast<std::ptrdiff_t>(at), payload.end());
	seal(patch);
}

} // namespace

TEST_CASE("a hues permutation rides the real save and load") {
	// **ITS MIGRATION HALF IS RETIRED (2026-10-03, kStateVersion 50).** This case was "a patch written before
	// colours could travel opens wearing the identity": a stream that ended before the hues block read as the
	// identity. No stream this build reads can end there any more — a 50 stream is refused without its end
	// marker and a 49 one without every block (see "a stream cut at any block boundary…" below) — so that half
	// pinned a road that no longer exists. What is left is the round trip, BY VALUE.
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const size_t tail = blockAt(saved.bytes, "FSH1");
	REQUIRE(tail < saved.bytes.size());

	feathers::SoundHues permuted;
	permuted.index = { 3, 0, 7, 1, 6, 2, 5, 4 };
	StateBytes withHues;
	withHues.bytes.assign(saved.bytes.begin(), saved.bytes.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream withOut = withHues.out();
	REQUIRE(feathers::saveSoundHues(&withOut, permuted));
	finishFrom(withHues, saved.bytes, "FSC1");
	clap_istream withIn = withHues.in();
	REQUIRE(state->load(host.plugin(), &withIn));

	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(state->save(host.plugin(), &resavedOut));
	StateBytes slice = from(resaved.bytes, "FSH1");
	clap_istream huesIn = slice.in();
	feathers::SoundHues read;
	bool present = false;
	REQUIRE(feathers::loadSoundHues(&huesIn, &read, &present));
	CHECK(present);
	CHECK(read == permuted);
	CHECK(resaved.bytes == withHues.bytes);
}

TEST_CASE("backlog EG: the macros' polarity rides the real save and load as the last block") {
	// ITS SECOND HALF — "a patch from before it reads as every macro unipolar" — IS RETIRED with kStateVersion 50:
	// no stream this build reads may end before a block (see "a stream cut at any block boundary…").
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	// A SAVE FROM A FRESH INSTANCE ENDS WITH THE BLOCK, every macro unipolar.
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const size_t tail = blockAt(saved.bytes, "FSMB");
	REQUIRE(tail < saved.bytes.size());
	{
		StateBytes slice = from(saved.bytes, "FSMB");
		clap_istream in = slice.in();
		feathers::MacroModes read;
		bool present = false;
		REQUIRE(feathers::loadMacroModes(&in, &read, &present));
		CHECK(present);
		CHECK(read == feathers::MacroModes {});
	}

	// A PATCH WITH MACRO 1 AND 3 BIPOLAR, made by replacing the block: loaded, then saved again, the
	// modes are the ones that went in — read back by value, not by "the bytes differ".
	feathers::MacroModes modes;
	modes.bipolar[1] = 1;
	modes.bipolar[3] = 1;
	StateBytes with;
	with.bytes.assign(saved.bytes.begin(), saved.bytes.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream withOut = with.out();
	REQUIRE(feathers::saveMacroModes(&withOut, modes));
	// AND THE SPECTRAL BLOCK AFTER IT (kStateVersion 51), as the plugin wrote it: since 50 every block must be present.
	REQUIRE(feathers::saveSpectralState(&withOut, feathers::SpectralState {}));
	REQUIRE(feathers::saveTerrainLikes(&withOut, feathers::TerrainLikes {})); // and the likes (kStateVersion 52)
	REQUIRE(feathers::savePresetIdentity(&withOut, feathers::PresetIdentity {})); // and the preset's name after it (kStateVersion 53)
	REQUIRE(feathers::saveDrumLanes(&withOut, feathers::DrumLanes {})); // and the pad lanes after it (kStateVersion 54)
	seal(with);
	clap_istream withIn = with.in();
	REQUIRE(state->load(host.plugin(), &withIn));
	{
		StateBytes resaved;
		clap_ostream resavedOut = resaved.out();
		REQUIRE(state->save(host.plugin(), &resavedOut));
		StateBytes slice = from(resaved.bytes, "FSMB");
		clap_istream in = slice.in();
		feathers::MacroModes read;
		bool present = false;
		REQUIRE(feathers::loadMacroModes(&in, &read, &present));
		CHECK(present);
		CHECK(read == modes);
	}
}

namespace {

/// Little-endian exact bits — the stream's own float contract, restated here so a hand-built stream
/// does not borrow the writer it is testing.
void appendFloat(std::vector<uint8_t>& bytes, float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 4; ++i) {
		bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffU));
	}
}

} // namespace

TEST_CASE("macro modes round-trip type and name") {
	using feathers::device::MacroType;
	static_assert(feathers::kMacroCount == 4, "the fixture below names four macros");
	feathers::MacroModes modes;
	modes.bipolar = { 1, 0, 1, 0 };
	modes.type = { MacroType::Latch, MacroType::Momentary, MacroType::Knob, MacroType::Knob };
	const std::string full24 = "abcdefghijklmnopqrstuvwx";
	REQUIRE(full24.size() == feathers::device::kMacroNameMax);
	modes.name = { std::string("Wobble"), std::string(), std::string("\xC3\x9C" "n\xC3\xAF"), full24 };

	StateBytes bytes;
	clap_ostream out = bytes.out();
	REQUIRE(feathers::saveMacroModes(&out, modes));
	clap_istream in = bytes.in();
	feathers::MacroModes read;
	bool present = false;
	REQUIRE(feathers::loadMacroModes(&in, &read, &present));
	CHECK(present);
	CHECK(bytes.read == bytes.bytes.size());
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		CAPTURE(i);
		CHECK(read.bipolar[i] == modes.bipolar[i]);
		CHECK(read.type[i] == modes.type[i]);
		CHECK(read.name[i] == modes.name[i]);
	}
	CHECK(read.type[0] == MacroType::Latch);
	CHECK(read.type[1] == MacroType::Momentary);
	CHECK(read.name[0] == "Wobble");
	CHECK(read.name[1].empty());
	CHECK(read.name[2] == "\xC3\x9C" "n\xC3\xAF");
	CHECK(read.name[3] == full24);
	CHECK(read == modes);
}

TEST_CASE("macro name longer than the cap is cut on a code point") {
	// 23 ASCII bytes and then a two-byte U+00E9: 25 bytes, so the cap at 24 falls INSIDE the é. The
	// cut must back up to 23 rather than keep a lone lead byte.
	const std::string ascii23(23, 'a');
	const std::string over = ascii23 + "\xC3\xA9";
	REQUIRE(over.size() == 25);
	CHECK(feathers::device::clampMacroName(over) == ascii23);
	CHECK(feathers::device::clampMacroName(std::string(30, 'z')) == std::string(24, 'z'));
	CHECK(feathers::device::clampMacroName("short") == "short");

	feathers::MacroModes modes;
	modes.name[2] = over;
	StateBytes bytes;
	clap_ostream out = bytes.out();
	REQUIRE(feathers::saveMacroModes(&out, modes));
	clap_istream in = bytes.in();
	feathers::MacroModes read;
	bool present = false;
	REQUIRE(feathers::loadMacroModes(&in, &read, &present));
	CHECK(present);
	CHECK(read.name[2] == ascii23);
	CHECK(read.name[2].size() == 23);
}

TEST_CASE("a malformed name length is refused") {
	// Hand-built: the tag, four macros, four unipolar flags, then macro 0's type (Knob) and a name
	// length of 1000 — past the cap, so the stream is malformed however many bytes follow it.
	std::vector<uint8_t> raw = { 'F', 'S', 'M', 'B' };
	appendFloat(raw, static_cast<float>(feathers::kMacroCount));
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		appendFloat(raw, 0.0f);
	}
	appendFloat(raw, 0.0f);
	appendFloat(raw, 1000.0f);
	raw.insert(raw.end(), 1000, static_cast<uint8_t>('x'));

	StateBytes bytes;
	bytes.bytes = raw;
	clap_istream in = bytes.in();
	feathers::MacroModes read;
	bool present = false;
	CHECK_FALSE(feathers::loadMacroModes(&in, &read, &present));
	CHECK_FALSE(present);

	// And the same stream with a sane length loads, so the refusal above is about the length and
	// not about something else in the hand-built bytes.
	std::vector<uint8_t> sane = { 'F', 'S', 'M', 'B' };
	appendFloat(sane, static_cast<float>(feathers::kMacroCount));
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		appendFloat(sane, 0.0f);
	}
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		appendFloat(sane, 0.0f);
		appendFloat(sane, i == 0 ? 3.0f : 0.0f);
		if (i == 0) {
			sane.insert(sane.end(), { 'x', 'y', 'z' });
		}
	}
	StateBytes good;
	good.bytes = sane;
	clap_istream goodIn = good.in();
	REQUIRE(feathers::loadMacroModes(&goodIn, &read, &present));
	CHECK(present);
	CHECK(read.name[0] == "xyz");
}

TEST_CASE("a macro type out of range is clamped on load") {
	// Hand-built like the case above: macro 0's type is NaN, macro 1's is 7 (past the last type), the
	// rest are Knob and Latch, every name empty. A type is clamped, not refused — the stream loads.
	std::vector<uint8_t> raw = { 'F', 'S', 'M', 'B' };
	appendFloat(raw, static_cast<float>(feathers::kMacroCount));
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		appendFloat(raw, 0.0f);
	}
	const std::array<float, 4> types = { std::numeric_limits<float>::quiet_NaN(), 7.0f, 0.0f, 1.0f };
	static_assert(feathers::kMacroCount == types.size(), "one hand-built type per macro");
	for (size_t i = 0; i < feathers::kMacroCount; ++i) {
		appendFloat(raw, types[i]);
		appendFloat(raw, 0.0f);
	}
	StateBytes bytes;
	bytes.bytes = raw;
	clap_istream in = bytes.in();
	feathers::MacroModes read;
	bool present = false;
	REQUIRE(feathers::loadMacroModes(&in, &read, &present));
	CHECK(present);
	CHECK(read.type[0] == feathers::device::MacroType::Knob);
	// PAST THE LAST IS THE LAST — Relative since backlog PQ (the absolute three end at Momentary).
	CHECK(read.type[1] == feathers::device::MacroType::Relative);
	CHECK(read.type[2] == feathers::device::MacroType::Knob);
	CHECK(read.type[3] == feathers::device::MacroType::Latch);
}

TEST_CASE("a fractional name length is refused") {
	// 2.5 bytes is not a length, and a misread length desynchronises every block after it — refused,
	// as a negative or oversized one is. The sane twin loads, so the refusal is about the fraction.
	const auto build = [](float length) {
		std::vector<uint8_t> raw = { 'F', 'S', 'M', 'B' };
		appendFloat(raw, static_cast<float>(feathers::kMacroCount));
		for (size_t i = 0; i < feathers::kMacroCount; ++i) {
			appendFloat(raw, 0.0f);
		}
		for (size_t i = 0; i < feathers::kMacroCount; ++i) {
			appendFloat(raw, 0.0f);
			appendFloat(raw, i == 0 ? length : 0.0f);
			if (i == 0) {
				raw.insert(raw.end(), { 'a', 'b', 'c' });
			}
		}
		return raw;
	};
	StateBytes bad;
	bad.bytes = build(2.5f);
	clap_istream badIn = bad.in();
	feathers::MacroModes read;
	bool present = false;
	CHECK_FALSE(feathers::loadMacroModes(&badIn, &read, &present));
	CHECK_FALSE(present);

	StateBytes good;
	good.bytes = build(3.0f);
	clap_istream goodIn = good.in();
	REQUIRE(feathers::loadMacroModes(&goodIn, &read, &present));
	CHECK(read.name[0] == "abc");
}

TEST_CASE("a patch with routes, a gamma base and a variation survives the real save and load") {
	// THE ROUND TRIP THE FORMAT NEVER HAD, and the one that catches a class the block-level cases
	// cannot see: every tagged block is tested against its own reader over a stream the test wrote
	// itself, so a stateLoad that reads a block TWICE — or in the wrong order, or not at all — passes
	// all of them and still loses the value on every preset a host opens. What is pinned here is the
	// plugin's own clap_plugin_state, both directions, carrying values that are not the defaults.
	//
	// The patch is built by rewriting the tail of a real save with the very writers stateSave uses.
	// That indirection is not a shortcut: none of these three has a parameter behind it — all are
	// editor-written state — so there is no way to set one through the C surface a host has, and the
	// bytes below are byte-for-byte what this plugin writes once the editor has been used. Gamma,
	// variations, hues and the chains being the last four blocks is what makes truncate-and-append exact — and the tail has to be written in FULL, because a resave writes
	// every block the plugin has and a short tail is simply a shorter file than the one that returns.
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));

	const size_t tail = blockAt(saved.bytes, "FSG1");
	REQUIRE(tail < saved.bytes.size());

	feathers::SoundGamma gamma;
	gamma.base[2] = -17.25;
	feathers::SoundVariations variations;
	// With a stored width (V18): the resave-identical subcase and the variation subcase below
	// carry it through the REAL clap_plugin_state path, both directions. The BANKED sibling holds
	// the non-default values; the ACTIVE entry is written to match the patch's own parameters,
	// because since V19 the active stance IS the live record and the save banks it from the params
	// — a file whose active entry disagreed with its params is a pre-V19 state that a resave heals,
	// which would (correctly) break the bytes-identical claim below.
	REQUIRE(feathers::storeVariation(variations, 1, { 0.33, 217.0, 0.05 }));
	// The ACTIVE entry, mirror-consistent with the head: the save banks the whole explore box from
	// the live state, so the entry must equal B's — width/offset/gamma base/drift/plane/residue at
	// their defaults, zone at its table default.
	variations.sound[1].values[1] = {
		0.0,
		feathers::findParam(static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Zone)))
		    ->def
	};
	// AND ITS KNOBS AND DRAWER at their table defaults (a stance carries them since 2026-10-10), the curve flat, as B's are.
	for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
		variations.sound[1].values[1].knobs[k] =
		    feathers::findParam(static_cast<clap_id>(feathers::perSoundKnobParam(1, k)))->def;
	}
	// ONE ROUTE, macro 2 into destination 1 at 0.75 — a value rather than a dense table (kill-lanes §2.4b).
	constexpr float kRouteAmount = 0.75f;
	// AND THE COLOURS, which are a name rather than a sound and so have to survive the file: a reorder
	// that was not saved puts every colour back the next time the patch opens, which is the bug the
	// hues block exists to end, arriving a session later. A real permutation, so an identity written
	// by mistake would not pass.
	feathers::SoundHues hues;
	hues.index = { 3, 0, 7, 1, 6, 2, 5, 4 };

	StateBytes patch;
	patch.bytes.assign(saved.bytes.begin(), saved.bytes.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream patchOut = patch.out();
	REQUIRE(feathers::saveSoundGamma(&patchOut, gamma));
	const std::array<feathers::AnchorPoint, feathers::kMaxSounds> livePlaces = defaultLivePlaces();
	REQUIRE(feathers::saveSoundVariations(&patchOut, variations, livePlaces));
	REQUIRE(feathers::saveSoundHues(&patchOut, hues));
	// AND BOTH CHAINS AND THE ROUTES, the last block — one block since chain-as-a-device item 6, in the
	// rack's own format. The chains a fresh instance has, taken from the plugin's OWN save rather than
	// built here: nothing below adds a device, so those are the chains this patch can hold, and what the
	// block is pinned for is its PLACE — after the hues, last — and that its routes survive. The route is
	// the one `routes` names, spelled as the file spells it: macro 2 by index, destination 1 by ParamId.
	//
	// THE TAIL HAS TO BE WRITTEN IN FULL — a resave writes every block the plugin has, so a tail that
	// stopped short would be a shorter file than the one that comes back, and the bytes-identical
	// subcase below is what says so.
	feathers::rack::RackPatch chains;
	{
		StateBytes fresh = from(saved.bytes, "FSC1");
		clap_istream freshIn = fresh.in();
		bool present = false;
		REQUIRE(feathers::loadChainsBlock(&freshIn, &chains, &present));
		REQUIRE(present);
	}
	// A FRESH INSTANCE ROUTES EXACTLY ONE THING — the default patch's envelope into Level (backlog EF),
	// the strip's one device by its id, the parameter by ParamId — so the one below makes two.
	REQUIRE(chains.route.size() == 1);
	// THE ENVELOPE'S OUTPUT BY ITS KEY (backlog JF), not its index.
	CHECK(chains.route[0].source
	      == feathers::rack::PatchCoord { static_cast<uint32_t>(feathers::rack::SourceKind::Device),
	                                          static_cast<uint32_t>(feathers::kDefaultRackInstance), 0,
	                                          feathers::mod::dahdsr::kOutputs[0].key });
	CHECK(chains.route[0].destination
	      == feathers::rack::PatchCoord { static_cast<uint32_t>(feathers::rack::DestKind::Param),
	                                          static_cast<uint32_t>(feathers::ParamId::VoiceLevel), 0, {} });
	CHECK(chains.route[0].amount == doctest::Approx(1.0f));
	feathers::rack::RackPatch::Route route;
	route.source = { static_cast<uint32_t>(feathers::rack::SourceKind::Macro), 2, 0, {} };
	route.destination = { static_cast<uint32_t>(feathers::rack::DestKind::Param),
		                  static_cast<uint32_t>(feathers::destinationParamId(1)), 0, {} };
	route.amount = kRouteAmount;
	chains.route.push_back(route);
	REQUIRE(feathers::saveChainsBlock(&patchOut, chains));
	// AND THE LAST BLOCK, the macros' polarity (backlog EG), every macro unipolar — written so the
	// bytes-identical subcase below compares a whole patch with a whole resave.
	REQUIRE(feathers::saveMacroModes(&patchOut, feathers::MacroModes {}));
	// AND THE SPECTRAL DRAWER'S STATE (kStateVersion 51), at its defaults, so the patch is whole.
	REQUIRE(feathers::saveSpectralState(&patchOut, feathers::SpectralState {}));
	REQUIRE(feathers::saveTerrainLikes(&patchOut, feathers::TerrainLikes {})); // AND THE LIKES (kStateVersion 52), none
	REQUIRE(feathers::savePresetIdentity(&patchOut, feathers::PresetIdentity {})); // and the preset's name after it (kStateVersion 53)
	REQUIRE(feathers::saveDrumLanes(&patchOut, feathers::DrumLanes {})); // and the pad lanes after it (kStateVersion 54)
	seal(patch); // and its end marker, as every save since kStateVersion 50 writes

	clap_istream in = patch.in();
	REQUIRE(state->load(host.plugin(), &in));

	// Saving again is how the load is questioned: every block writes what the plugin is holding NOW,
	// so a value that did not survive comes back as its default here.
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(state->save(host.plugin(), &resavedOut));

	// THE CHAINS BLOCK AS THE RESAVE WROTE IT, for the two subcases that read it.
	const auto resavedChains = [&] {
		StateBytes slice = from(resaved.bytes, "FSC1");
		clap_istream chainsIn = slice.in();
		feathers::rack::RackPatch read;
		bool present = false;
		REQUIRE(feathers::loadChainsBlock(&chainsIn, &read, &present));
		CHECK(present);
		return read;
	};

	SUBCASE("the routes are still there") {
		// BOTH: the default patch's envelope route (backlog EF) the fresh instance already held, and the
		// macro route pushed above — the whole list, so a load that dropped or doubled one is caught.
		const feathers::rack::RackPatch read = resavedChains();
		REQUIRE(read.route.size() == 2);
		CHECK(read.route[0] == chains.route[0]);
		CHECK(read.route[1].source == route.source);
		CHECK(read.route[1].destination == route.destination);
		CHECK(read.route[1].amount == doctest::Approx(0.75f));
	}

	SUBCASE("the colours are still the ones the list was wearing") {
		StateBytes slice = from(resaved.bytes, "FSH1");
		clap_istream huesIn = slice.in();
		feathers::SoundHues read;
		bool present = false;
		REQUIRE(feathers::loadSoundHues(&huesIn, &read, &present));
		CHECK(present);
		CHECK(read == hues);
	}

	SUBCASE("and the lane still holds the filter") {
		const feathers::rack::RackPatch read = resavedChains();
		// THE SAME DEVICES, rows, ids and lanes as went in — and among them, in the fx lane (chain 1),
		// the filter a fresh instance has.
		CHECK(read.device == chains.device);
		bool filterInLane = false;
		for (const auto& d : read.device) {
			filterInLane = filterInLane
			               || (d.chain == 1 && std::memcmp(d.tag.data(), feathers::fx::filter::kTraits.tag, 4) == 0);
		}
		CHECK(filterInLane);
	}

	SUBCASE("so is the gamma base") {
		StateBytes slice = from(resaved.bytes, "FSG1");
		clap_istream gammaIn = slice.in();
		feathers::SoundGamma read;
		bool present = false;
		REQUIRE(feathers::loadSoundGamma(&gammaIn, &read, &present));
		CHECK(present);
		CHECK(read.base[2] == doctest::Approx(-17.25));
	}

	SUBCASE("and so is the variation") {
		// FSV2 since V18 — the stances carry their ride-along values, so the block writes the
		// bumped tag; loadSoundVariations still reads both generations. THE RENAME IS A DECISION, NOT
		// DRIFT: the stream is positional with no lengths, so growing the stances under the old tag
		// would hand an FSV1 reader's leftover bytes to the next block's tag read — a silent
		// misparse — where the bump makes an old build refuse the whole load deterministically.
		// Refusal beats misparse; V2's precedent accepts the refusal.
		StateBytes slice = from(resaved.bytes, "FSV2");
		clap_istream variationsIn = slice.in();
		feathers::SoundVariations read;
		bool present = false;
		REQUIRE(feathers::loadSoundVariations(&variationsIn, &read, &present));
		CHECK(present);
		CHECK(read == variations);
	}

	SUBCASE("and the patch as a whole is the one that went in") {
		// The catch-all the three cases above cannot be, and the one that will answer for whatever
		// block is appended next: a patch saved from a loaded patch is the same bytes. It costs one
		// comparison and covers every block at once, including the parameters.
		//
		// Reported as the OFFSET that differs rather than as the two vectors, which is the difference
		// between a failure that names the block and one that prints two kilobytes of decimal.
		CHECK(resaved.bytes.size() == patch.bytes.size());
		const auto differs = std::mismatch(resaved.bytes.begin(), resaved.bytes.end(),
		                                   patch.bytes.begin(), patch.bytes.end());
		CHECK(static_cast<size_t>(differs.first - resaved.bytes.begin()) == patch.bytes.size());
	}
}

TEST_CASE("a save banks the active stance from the parameters, healing a file that disagrees") {
	// THE MUTATION-PROVEN GAP, closed: the review deleted feathers_clap's banking loop outright and
	// every suite stayed green, because the one real-path round trip crafts its active entry to
	// EQUAL the params — a save writing the stale stored entry verbatim passed everything. This
	// patch is the disagreeing shape itself, the pre-V19 file the heal comment was only claiming:
	// an FSV2 active entry at values the parameters do not hold. The resave must carry the PARAMS'
	// values in the active entry — V19: the active stance IS the live record, and a file must never
	// reopen with the two disagreeing — with the stale values as this case's own known-bad: a save
	// without the banking writes 0.9s here and fails every active-entry line below.
	HostedPlugin host;
	const auto* state = static_cast<const clap_plugin_state*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const size_t tail = blockAt(saved.bytes, "FSG1");
	REQUIRE(tail < saved.bytes.size());

	feathers::SoundVariations variations;
	REQUIRE(feathers::storeVariation(variations, 1, { 0.33, 640.0 })); // the banked sibling
	variations.sound[1].values[1] = { 0.9, 999.0, 0.9 }; // the ACTIVE entry, disagreeing with B's
	                                                     // live explore box in the saved head

	StateBytes patch;
	patch.bytes.assign(saved.bytes.begin(), saved.bytes.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream patchOut = patch.out();
	REQUIRE(feathers::saveSoundGamma(&patchOut, feathers::SoundGamma {}));
	const std::array<feathers::AnchorPoint, feathers::kMaxSounds> livePlaces = defaultLivePlaces();
	REQUIRE(feathers::saveSoundVariations(&patchOut, variations, livePlaces));

	// THE HUES, THE CHAINS AND THE MACROS FROM THE PLUGIN'S OWN SAVE, AND THE END MARKER: since kStateVersion 50 a
	// stream that stops after the variations is refused, where it used to read the rest as absent.
	finishFrom(patch, saved.bytes, "FSH1");
	clap_istream in = patch.in();
	REQUIRE(state->load(host.plugin(), &in));
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(state->save(host.plugin(), &resavedOut));

	StateBytes slice = from(resaved.bytes, "FSV2");
	clap_istream variationsIn = slice.in();
	feathers::SoundVariations read;
	bool present = false;
	REQUIRE(feathers::loadSoundVariations(&variationsIn, &read, &present));
	REQUIRE(present);
	REQUIRE(read.sound[1].count == 2);
	// The banked sibling survived verbatim.
	CHECK(read.sound[1].values[0].width == doctest::Approx(0.33));
	CHECK(read.sound[1].values[0].zone == doctest::Approx(640.0));
	// THE PIN: the active entry was banked from the live explore box, not copied stale — the 0.9s
	// are gone and the live defaults stand in their place.
	CHECK(read.sound[1].values[1].width == doctest::Approx(0.0));
	CHECK(read.sound[1].values[1].zone
	      == doctest::Approx(
	          feathers::findParam(
	              static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Zone)))
	              ->def));
	CHECK(read.sound[1].values[1].offset == doctest::Approx(0.0));
}

namespace {

/// Raw FSV1 floats for the legacy tail below — the fixture idiom from the block-level suite: the
/// bytes are the contract, spelled as an old build actually wrote them.
void pushLegacyFloat(std::vector<uint8_t>& bytes, float value) {
	uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	for (int i = 0; i < 4; ++i) {
		bytes.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffU));
	}
}

} // namespace

TEST_CASE("an FSV1 tail through the real load fills its stance widths from the live parameter") {
	// THE OTHER MUTATION-PROVEN GAP: the glue branch in feathers_clap's variations loader — the one
	// that turns "widths absent" into the slot's LIVE width — could be disabled green, because the
	// unit fixture and the fill helper were tested but nothing drove an FSV1 tail through the real
	// clap_plugin_state->load. This does, with a NON-DEFAULT live width set through the C surface,
	// so the fill provably reads the parameter rather than a default: disable the glue and every
	// width below reads 0 instead of 0.42. The observable is V13b-era behaviour preserved — a
	// recall of an old stance is a width no-op, because every stored width equals the live one.
	HostedPlugin host;
	EventList events;
	events.pushParam(0, static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Width)),
	                 0.42);
	// No note is sounding, so the plugin may honestly answer SLEEP — the event landing is what
	// matters, not the render staying awake.
	REQUIRE(host.run(64, events) != CLAP_PROCESS_ERROR);

	const auto* state = static_cast<const clap_plugin_state*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const size_t tail = blockAt(saved.bytes, "FSG1");
	REQUIRE(tail < saved.bytes.size());

	StateBytes patch;
	patch.bytes.assign(saved.bytes.begin(), saved.bytes.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream patchOut = patch.out();
	REQUIRE(feathers::saveSoundGamma(&patchOut, feathers::SoundGamma {}));
	// The legacy tail, byte for byte: tag, two header counts, and slot 1 carrying two stances of
	// places only — what every build in the V13-to-V18 window wrote.
	patch.bytes.push_back('F');
	patch.bytes.push_back('S');
	patch.bytes.push_back('V');
	patch.bytes.push_back('1');
	pushLegacyFloat(patch.bytes, 8.0f); // slots
	pushLegacyFloat(patch.bytes, 8.0f); // stances per sound
	for (int slot = 0; slot < 8; ++slot) {
		if (slot == 1) {
			pushLegacyFloat(patch.bytes, 2.0f); // count
			pushLegacyFloat(patch.bytes, 1.0f); // active
			pushLegacyFloat(patch.bytes, 0.25f);
			pushLegacyFloat(patch.bytes, -0.5f);
			pushLegacyFloat(patch.bytes, -0.125f);
			pushLegacyFloat(patch.bytes, 0.75f);
			continue;
		}
		pushLegacyFloat(patch.bytes, 0.0f);
		pushLegacyFloat(patch.bytes, 0.0f);
	}

	// THE HUES, THE CHAINS AND THE MACROS FROM THE PLUGIN'S OWN SAVE, AND THE END MARKER: since kStateVersion 50 a
	// stream that stops after the variations is refused, where it used to read the rest as absent.
	finishFrom(patch, saved.bytes, "FSH1");
	clap_istream in = patch.in();
	REQUIRE(state->load(host.plugin(), &in));
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(state->save(host.plugin(), &resavedOut));

	StateBytes slice = from(resaved.bytes, "FSV2");
	clap_istream variationsIn = slice.in();
	feathers::SoundVariations read;
	bool present = false;
	REQUIRE(feathers::loadSoundVariations(&variationsIn, &read, &present));
	REQUIRE(present);
	REQUIRE(read.sound[1].count == 2);
	// The stored (non-active) stance gained the LIVE values — its legacy places are dropped whole
	// (place left the stance, V20).
	CHECK(read.sound[1].values[0].width == doctest::Approx(0.42));
	CHECK(read.sound[1].values[0].zone
	      == doctest::Approx(
	          feathers::findParam(
	              static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Zone)))
	              ->def));
	// The active entry banks from the parameters — its width is the same live 0.42, which is the
	// recall-no-op made visible: every record of this old patch stands at the width it loaded with.
	CHECK(read.sound[1].values[1].width == doctest::Approx(0.42));
}

TEST_CASE("the host's transport reaches the oscillator, by either timeline and by neither") {
	// THE BINDING'S HALF OF THE TRANSPORT LOCK. The synth suite pins what a song position DOES to a
	// phase; this pins that the number arriving there is the one the host actually sent, through
	// the fixed-point conversion — which is the step that would fail as a slow drift rather than as
	// a wrong note, and so would never be noticed in a single block.
	constexpr uint32_t kFrames = 512;
	// A song position with no round-number sympathy, so a conversion that dropped or doubled the
	// fixed-point factor lands somewhere obviously different rather than somewhere plausible.
	constexpr double kPosition = 3.7115;

	const auto renderAt = [](auto&& configure) {
		HostedPlugin host;
		EventList events;
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscMode), 1.0); // Analog
		host.run(kFrames, events);
		configure(host);
		EventList note;
		note.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		host.run(kFrames, note);
		return host.left();
	};

	// SECONDS AND BEATS MUST AGREE ABOUT THE SAME INSTANT. At 120 bpm a beat is half a second, so
	// the beats road carries twice the number and has to come back to the same phase. If either
	// conversion were wrong this is where it shows, and it shows as a different note rather than as
	// a drift nobody attributes.
	const std::vector<float> bySeconds =
	    renderAt([](HostedPlugin& h) { h.setTransport(kPosition, true, true); });
	const std::vector<float> byBeats =
	    renderAt([](HostedPlugin& h) { h.setTransport(kPosition, true, false); });
	REQUIRE(bySeconds.size() == byBeats.size());
	float worst = 0.0f;
	for (size_t i = 0; i < bySeconds.size(); ++i) {
		worst = std::max(worst, std::abs(bySeconds[i] - byBeats[i]));
	}
	// Not bit-identical: the two roads round a different fixed-point quantity. Close enough that the
	// phase agrees to well inside a grid slot, which is the claim worth making.
	CHECK(worst < 1e-3f);

	// A DIFFERENT POSITION IS A DIFFERENT PHASE, or the transport is being taken and discarded —
	// which every check above would survive.
	const std::vector<float> elsewhere =
	    renderAt([](HostedPlugin& h) { h.setTransport(kPosition + 0.31, true, true); });
	float apart = 0.0f;
	for (size_t i = 0; i < bySeconds.size(); ++i) {
		apart = std::max(apart, std::abs(bySeconds[i] - elsewhere[i]));
	}
	CHECK(apart > 1e-3f);

	// AND NO TRANSPORT AT ALL IS NOT POSITION ZERO. A host that offers none leaves Analog on the
	// per-voice behaviour it shipped with; treating null as zero would silently turn it into Digi
	// for every such host, which is the failure this fallback exists to prevent.
	const std::vector<float> none = renderAt([](HostedPlugin& h) { h.clearTransport(); });
	const std::vector<float> atZero =
	    renderAt([](HostedPlugin& h) { h.setTransport(0.0, true, true); });
	// At song position zero frac(f0 * 0) is 0, which is also where a fresh voice sits — so these
	// two agree, and that agreement is exactly why "null means zero" would have been invisible.
	float nullVsZero = 0.0f;
	for (size_t i = 0; i < none.size(); ++i) {
		nullVsZero = std::max(nullVsZero, std::abs(none[i] - atZero[i]));
	}
	CHECK(nullVsZero == doctest::Approx(0.0f));
	// The claim that has teeth: null is NOT the same as the position under test.
	float nullVsPosition = 0.0f;
	for (size_t i = 0; i < none.size(); ++i) {
		nullVsPosition = std::max(nullVsPosition, std::abs(none[i] - bySeconds[i]));
	}
	CHECK(nullVsPosition > 1e-3f);
}

TEST_CASE("the chains block round-trips a chain that has been edited — and the LANE-FREE guard it used to "
          "be is retired, not re-expressed") {
	// **THE NAME CHANGED WITH THE BODY, DELIBERATELY (kill-lanes step 3c).** It was called *"BX GUARD:
	// the chains block is LANE-FREE, so where a device's lane is cannot move the format"*, and it no
	// longer enforces that. A case name is how a reader asks "is this property guarded?" — leaving the
	// old one on a body that had stopped guarding it would answer yes and be the hollow-pass shape one
	// level up, where no assertion is even involved. The successor guard belongs at step 5, which asserts
	// that no public header declares a lane at all.
	//
	// **THE FILE NAMES NO LANE** — devices in chain order under their ids, and routes by id. It was so for
	// Feathers' old fx chain block, and for one commit on 2026-09-11 the one format carried a lane. The
	// design: no lanes — the system is a graph, addresses are not sequential and ordering comes only from
	// the graph. That is now true BY CONSTRUCTION rather than by this case: there is no lane in a view, in a
	// row, or in the state, so there is nothing left that could reach the file.
	//
	// **THE DIVERGENCE HALF OF THIS CASE IS GONE, AND IT IS GONE BECAUSE IT BECAME IMPOSSIBLE TO STATE
	// (kill-lanes step 3c).** It used to edit the chain until its lanes were NOT its positions, assert
	// that divergence (`CHECK(diverged)`), and only then compare the bytes — the divergence being what
	// made the byte match mean something. A chain is dense in `[0, count)` now and a device has no lane
	// to diverge, so both that assertion and the `CHECK(lanesMoved)` on the reload were deleted rather
	// than reworded: each would have been true by construction, which is worse than absent. **The guard
	// this case was built to be — "a lane in the file would show up as a byte difference" — is therefore
	// RETIRED, not re-expressed.** What is left below is the plain round trip, and that is all it claims.
	//
	// The edit history is KEPT (three devices, the middle one deleted) because the round trip should run
	// over a chain that has been edited rather than a virgin one — but note it no longer proves anything
	// on its own the way the divergence did.
	//
	// **WHAT THE BYTES' GREEN DOES NOT MEAN — a boundary found by walking into it, step 5 commit 2.** It
	// compares a save against a re-save of ITS OWN RELOAD, so both walks being wrong IN THE SAME WAY
	// passes. MEASURED then: it stayed green through a flip that wrote a cleared record for a device
	// removed from the middle, because both saves wrote the same wrong file. So it also reads the ORDER
	// back as a value; read a green on the bytes as "the format did not move", never as "it is right".
	namespace filter = feathers::fx::filter;
	namespace shaper = feathers::fx::shaper;

	feathers::FxChain chain = feathers::defaultFxChain();

	// AN EDITED CHAIN — three devices, the middle one deleted, so the rows below it have shifted up.
	// This was once the manoeuvre that pulled lane and position apart; it is kept because a round trip
	// over a chain that has been through a removal is a better exercise than one over a virgin chain,
	// and for no stronger reason than that.
	const size_t a = feathers::addFxDevice(&chain, filter::kTraits.tag);
	const size_t b = feathers::addFxDevice(&chain, shaper::kTraits.tag);
	const size_t c = feathers::addFxDevice(&chain, filter::kTraits.tag);
	REQUIRE(a < chain.device.size());
	REQUIRE(b < chain.device.size());
	REQUIRE(c < chain.device.size());
	REQUIRE(feathers::removeFxDevice(&chain, b));

	// THROUGH THE BRIDGE THE STATE ROAD SAVES FROM: the chain into a record, the record as the one
	// format's block, the block into a second bridge's record, and that record saved again.
	feathers::ParamBridge writer;
	writer.restoreFxChain(chain);
	StateBytes first;
	clap_ostream firstOut = first.out();
	REQUIRE(feathers::saveChainsBlock(&firstOut, writer.chainsPatch()));

	feathers::rack::RackPatch patch;
	bool present = false;
	clap_istream in = first.in();
	REQUIRE(feathers::loadChainsBlock(&in, &patch, &present));
	CHECK(present);
	feathers::ParamBridge reader;
	CHECK(reader.restoreChains(patch).routes.count == 0);
	const feathers::FxChain reloaded = reader.fxChain();
	REQUIRE(reloaded.count == chain.count);

	// **THE SAME DEVICES IN THE SAME ORDER, UNDER THE SAME IDS** — the content claim, and since step 3c
	// the only one this loop can make. The `CHECK(lanesMoved)` that stood underneath it asked whether the
	// loader had minted its own lanes, different from the saved ones; there are no lanes to mint and no
	// second numbering for the loader to disagree with, so it was deleted rather than made trivially true.
	for (size_t at = 0; at < chain.count; ++at) {
		CAPTURE(at);
		CHECK(feathers::fxDeviceAtPosition(reloaded, at).tag() == feathers::fxDeviceAtPosition(chain, at).tag());
		CHECK(feathers::fxDeviceAtPosition(reloaded, at).instanceId() == feathers::fxDeviceAtPosition(chain, at).instanceId());
	}

	StateBytes second;
	clap_ostream secondOut = second.out();
	REQUIRE(feathers::saveChainsBlock(&secondOut, reader.chainsPatch()));
	// **AND BYTE FOR BYTE** — the format did not move between a save and its reload.
	CHECK(second.bytes == first.bytes);
}

TEST_CASE("BX GUARD: deleting a device above another leaves the survivor's routing on it") {
	// **THE PROPERTY A HAND DEPENDS ON:** route AT a device, delete the one above it, and the route must
	// still land on the same device — not on its neighbour, and not nowhere.
	//
	// **THE MECHANISM THAT USED TO DELIVER IT IS GONE, AND THE PROPERTY IS NOT (kill-lanes step 3c).**
	// This case was written when `removeFxDevice` shifted POSITIONS down and left LANES alone: the route
	// stayed pointed at its device because the device kept the routing column it had been given, and the
	// case said so by capturing `keeperLane` before the removal and re-asserting it after. Both halves of
	// that are now impossible to state — there is no lane, a device's view coordinate IS its position, and
	// a survivor that slides up has therefore CHANGED coordinate by construction. **A coordinate captured
	// before a structural change and compared to itself after is asserting the invariant this arc deleted**,
	// so `CHECK(fxLaneAtPosition(chain, nowAt) == keeperLane)` was removed rather than rewritten.
	//
	// **WHAT CARRIES THE PROPERTY INSTEAD IS THE ENDPOINT**, so the case asks in those terms. A route names
	// its device by id and its control by index; neither moves when a device above is deleted, and that —
	// not a preserved column — is what now keeps the hand's route where the hand put it. The same shift also
	// moves the case OFF the free functions: `removeFxDevice(FxChain*, size_t)` no longer takes a
	// `ModRoutes*` because routes do not live beside the chain any more, so a flat table set up here would
	// be untouched by the removal and every assertion against it would pass by construction. The bridge is
	// where the chain and the routes are the same record, so the removal runs through the bridge.
	namespace filter = feathers::fx::filter;
	namespace shaper = feathers::fx::shaper;

	feathers::ParamBridge bridge;
	const size_t doomed = bridge.addFxDevice(filter::kTraits.tag);
	const size_t keeper = bridge.addFxDevice(shaper::kTraits.tag);
	REQUIRE(doomed < feathers::kMaxFxSlots);
	REQUIRE(keeper < feathers::kMaxFxSlots);
	REQUIRE(keeper == doomed + 1); // the keeper is BELOW the doomed one, which is the whole setup

	const feathers::DeviceInstanceId keeperId = bridge.fxDeviceAt(keeper);
	REQUIRE(keeperId != feathers::kNoInstance);
	const feathers::device::Descriptor* keeperDesc = bridge.descriptorOf(keeperId);
	REQUIRE(keeperDesc != nullptr);

	// A MACRO INTO THE KEEPER'S FIRST MODULATABLE CONTROL, BY ENDPOINT — the device by its id, the
	// control by its own index, which is what a route is made of since kill-lanes step 2.
	const size_t control = keeperDesc->controlOfNthModulatableBesideEnable(0);
	REQUIRE(control < keeperDesc->params.size());
	const feathers::backplane::Source macro = feathers::backplane::Source::core(0);
	const feathers::backplane::Destination keeperControl =
	    feathers::backplane::Destination::control(keeperId, control);
	bridge.setRouteAmount(macro, keeperControl, 0.5f);
	REQUIRE(bridge.routeAmount(macro, keeperControl) == doctest::Approx(0.5f));

	// COUNTED FROM WHAT IS THERE, not from a number typed here: a fresh bridge already holds a device,
	// so an assertion of "one left" would be about the fixture rather than the removal.
	const size_t before = bridge.fxDeviceCount();
	REQUIRE(bridge.removeFxDevice(doomed));
	CHECK(bridge.fxDeviceCount() == before - 1);

	// IT SLID INTO THE HOLE THE DELETION LEFT — re-derived at the moment of use rather than remembered,
	// because the position is exactly what the removal was entitled to change.
	CHECK(bridge.fxDeviceAt(doomed) == keeperId);
	// **AND THE ROUTE IS STILL THERE, ON THE SAME DEVICE.** Asked by the endpoint it was written with, so
	// a removal that re-pointed the route at the keeper's old neighbour — or dropped it — reds this.
	CHECK(bridge.routeAmount(macro, keeperControl) == doctest::Approx(0.5f));
	// AND THE TWO SEAMS AGREE ABOUT WHERE IT IS — the same fact read from both ends, which is what would
	// catch a removal that left the chain reading right while the record still parked the device elsewhere.
	CHECK(bridge.fxSlotOf(keeperId) == doomed);
}


// ---------------------------------------------------------------------------------------------------
// DE — THE FIRST MAC BUILD (2026-09-07): no effect worked besides the default filter (not even another
// filter added later). The device built at activate worked; every device added afterwards was inert
// with its card live. Reproduced HERE, on Linux, with a host that behaves as that one did: it ANSWERS timer
// support (so the editor may be created), registers the timer, and never delivers it.
// ---------------------------------------------------------------------------------------------------
namespace {

/// A host whose timer registers and never fires — the shape of a macOS host as seen from the plugin.
/// No posix fd support (macOS has no X11 fd), and no other extension.
struct SilentTimerHost {
	static bool CLAP_ABI registerTimer(const clap_host*, uint32_t, clap_id* id) {
		*id = 7;
		return true;
	}
	static bool CLAP_ABI unregisterTimer(const clap_host*, clap_id) { return true; }
	static const void* CLAP_ABI getExtension(const clap_host*, const char* id) {
		static const clap_host_timer_support kTimer{registerTimer, unregisterTimer};
		if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) {
			return &kTimer;
		}
		return nullptr;
	}
	static const clap_host& host() {
		static const clap_host kSilent = {
		    CLAP_VERSION_INIT, nullptr,       "feathers tests (silent timer)", "Owlet Records",
		    "",                "1.0",         getExtension,     hostRequestRestart,
		    hostRequestProcess, hostRequestCallback,
		};
		return kSilent;
	}
};

/// The plugin's embed API on this platform — what a real host would pass to gui.create.
constexpr const char* kEmbedApiHere =
#if defined(__APPLE__)
    CLAP_WINDOW_API_COCOA;
#else
    CLAP_WINDOW_API_X11;
#endif

} // namespace

#if defined(__linux__)
namespace {

/// A HOST WITH A TIMER AND THE GUI EXTENSION, the requests recorded and NOT honoured — Bitwig at open
/// (`tools/bitwighost.cpp`).
struct BitwigOrderHost {
	static inline int resizeRequests = 0;
	static const void* CLAP_ABI getExtension(const clap_host* h, const char* id) {
		static const clap_host_gui kGui {
		    [](const clap_host*) {},
		    [](const clap_host*, uint32_t, uint32_t) {
			    ++resizeRequests;
			    return true;
		    },
		    [](const clap_host*) { return true; },
		    [](const clap_host*) { return true; },
		    [](const clap_host*, bool) {},
		};
		if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
			return &kGui;
		}
		return SilentTimerHost::getExtension(h, id);
	}
	static const clap_host& host() {
		static const clap_host kHostWithGui = {
		    CLAP_VERSION_INIT, nullptr,       "feathers tests (Bitwig's gui order)", "Owlet Records",
		    "",                "1.0",         getExtension,     hostRequestRestart,
		    hostRequestProcess, hostRequestCallback,
		};
		return kHostWithGui;
	}
};

} // namespace

TEST_CASE("the synth opened in Bitwig's order (no set_scale) opens at its scaled size held to the "
          "screen, and its first drag never snaps") {
	// It opened at the LOGICAL design (1240x700) and the first drag's floor, the design times the scale, snapped it to
	// 2170x1285 at 1.75 — past a 1920x1080 screen. Now `get_size` before the embed answers the scaled size, and that size
	// and `adjust_size`'s floor are held to the usable screen. The screen is STUBBED (`setUsableScreenForTest`); the
	// display's scale is `FEATHERS_UI_SCALE`, which this host's lack of set_scale leaves in charge.
	if (std::getenv("DISPLAY") == nullptr) {
		MESSAGE("skipped: no DISPLAY — run under scripts/venue-xvfb.sh (ctest -R clap-render-display)");
		return;
	}
	feathers::EditorPrefs prefs;
	prefs.load();
	const double uiScale = std::clamp(prefs.number(feathers::ui::PluginWindow::kUiScaleKey, 1.0), 1.0, 2.0);
	// THE DESIGN, restated: 1240 x 700, plus the closed canvas's 34-row strip.
	const double designW = 1240.0;
	const double designH = 700.0 + 34.0;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	for (const char* scaleText : { "1", "1.75" }) {
		// A SCREEN THAT HOLDS THE DESIGN AT 1.75, AND A COMMON ONE: 1920x1080 less a panel.
		for (const auto& [screenW, screenH] : { std::pair<uint32_t, uint32_t> { 2560, 1400 }, { 1920, 1050 } }) {
			CAPTURE(scaleText);
			CAPTURE(screenW);
			::setenv("FEATHERS_UI_SCALE", scaleText, 1);
			feathers::ui::PluginWindow::setUsableScreenForTest(screenW, screenH);
			const double scale = std::atof(scaleText) * uiScale;
			const auto wantW = std::min(static_cast<uint32_t>(std::ceil(designW * scale)), screenW);
			const auto wantH = std::min(static_cast<uint32_t>(std::ceil(designH * scale)), screenH);
			HostedPlugin h(&BitwigOrderHost::host());
			const auto* gui = static_cast<const clap_plugin_gui*>(h.plugin()->get_extension(h.plugin(), CLAP_EXT_GUI));
			REQUIRE(gui != nullptr);
			REQUIRE(gui->create(h.plugin(), CLAP_WINDOW_API_X11, false));
			uint32_t openW = 0, openH = 0;
			REQUIRE(gui->get_size(h.plugin(), &openW, &openH));
			CHECK(openW == wantW);
			CHECK(openH == wantH);
			CHECK(openW <= screenW);
			CHECK(openH <= screenH);
			const int screen = DefaultScreen(display);
			::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, openW, openH, 0,
			                                      BlackPixel(display, screen), BlackPixel(display, screen));
			XFlush(display);
			clap_window window {};
			window.api = CLAP_WINDOW_API_X11;
			window.x11 = parent;
			REQUIRE(gui->set_parent(h.plugin(), &window));
			uint32_t w = 0, hh = 0;
			REQUIRE(gui->get_size(h.plugin(), &w, &hh));
			CHECK(w == openW);
			CHECK(hh == openH);
			// THE FIRST DRAG, a row shorter: it holds at the open size — the old road answered 2170x1285 here.
			w = openW;
			hh = openH - 1;
			REQUIRE(gui->adjust_size(h.plugin(), &w, &hh));
			CHECK(w == openW);
			CHECK(hh == openH);
			CHECK(w <= screenW);
			CHECK(hh <= screenH);
			if (openH < screenH) {
				w = openW;
				hh = openH + 1;
				REQUIRE(gui->adjust_size(h.plugin(), &w, &hh));
				CHECK(hh == openH + 1);
			}
			gui->destroy(h.plugin());
			XDestroyWindow(display, parent);
		}
	}
	::unsetenv("FEATHERS_UI_SCALE");
	feathers::ui::PluginWindow::setUsableScreenForTest(0, 0);
	XCloseDisplay(display);
}

TEST_CASE("backlog GP: a host that destroys its container BEFORE guiDestroy gets an editor in the next container, every "
          "round — three rounds, the editor's windows counted inside each container") {
	// X destroys a window's children with it, so the editor's own window dies with the container. It used to stay dead:
	// rounds 2 and 3 handed the host an empty container and set_parent said true (`cyclehost`, reproduced 2026-09-20).
	// The count is of windows ACTUALLY INSIDE the host's container, from the server; and a round after the first must
	// hold a NEW window, because the old one no longer exists.
	if (std::getenv("DISPLAY") == nullptr) {
		MESSAGE("skipped: no DISPLAY — run under scripts/venue-xvfb.sh (ctest -R clap-render-display)");
		return;
	}
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	HostedPlugin h(&BitwigOrderHost::host());
	const auto* gui = static_cast<const clap_plugin_gui*>(h.plugin()->get_extension(h.plugin(), CLAP_EXT_GUI));
	REQUIRE(gui != nullptr);
	::Window previous = 0;
	for (int round = 1; round <= 3; ++round) {
		CAPTURE(round);
		REQUIRE(gui->create(h.plugin(), CLAP_WINDOW_API_X11, false));
		uint32_t w = 0, hh = 0;
		REQUIRE(gui->get_size(h.plugin(), &w, &hh));
		::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, hh, 0,
		                                      BlackPixel(display, screen), BlackPixel(display, screen));
		XSync(display, False);
		clap_window window {};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = parent;
		REQUIRE(gui->set_parent(h.plugin(), &window));
		CHECK(gui->show(h.plugin()));
		::Window root = 0, parentOf = 0, *kids = nullptr;
		unsigned int count = 0;
		REQUIRE(XQueryTree(display, parent, &root, &parentOf, &kids, &count) != 0);
		const ::Window editorWindow = count > 0 ? kids[0] : 0;
		if (kids != nullptr) {
			XFree(kids);
		}
		CHECK(count == 1);
		if (round > 1) {
			CHECK(editorWindow != previous);
		}
		previous = editorWindow;
		// THE HOST'S ORDER UNDER TEST: its container first, then guiDestroy.
		XDestroyWindow(display, parent);
		XSync(display, False);
		gui->destroy(h.plugin());
	}
	XCloseDisplay(display);
}
#endif

TEST_CASE("DE, found on macOS: a device added after init reaches the synth from the EDITOR'S OWN heartbeat, on a host whose timer never fires") {
	namespace shaper = feathers::fx::shaper;
#if defined(_WIN32)
	// NO EDITOR ON WINDOWS YET (PluginWindow::isAvailable): the gui extension is absent, so the editor's heartbeat this
	// case is about does not exist there. It returns with the embed.
	MESSAGE("skipped: no Windows editor embed yet");
	return;
#elif defined(__linux__)
	// THE EDITOR NEEDS A DISPLAY TO BE CREATED ON X11 — visage asks X for the monitor's scale inside
	// `setWindowDimensions` and faults without one (MEASURED under ASan: XLockDisplay on a null
	// Display from PluginWindow::create). The headless `clap-render` entry skips this case; the
	// `clap-render-display` entry runs the same binary under scripts/venue-xvfb.sh, where it executes.
	if (std::getenv("DISPLAY") == nullptr) {
		MESSAGE("skipped: no DISPLAY — run under scripts/venue-xvfb.sh (ctest -R clap-render-display)");
		return;
	}
#endif
	// THREE INSTANCES, IDENTICAL EVENT STREAMS, so their renders are comparable sample for sample
	// (the engine carries no randomness): DRY has no shaper; GOOD adds one and is provisioned by a
	// road that always worked (activate provisions the chain it is handed); MAC adds one and gets
	// NO host callback at all — only visage's own Heartbeat, fired the way AppKit fires it.
	//
	// NOT the host's timer callback for GOOD, and the first draft was: the plugin registers its
	// timer in guiSetParent, which this case never reaches (no window), so `on_timer` by hand
	// reached nothing and the reference rendered DRY — the reference was vacuous, and REQUIRE_FALSE
	// on it is what said so.
	auto open = [](HostedPlugin& h) -> const clap_plugin_gui* {
		const auto* gui = static_cast<const clap_plugin_gui*>(
		    h.plugin()->get_extension(h.plugin(), CLAP_EXT_GUI));
		REQUIRE(gui != nullptr);
		// The host OFFERS timer support, so on X11 the editor is granted; its tree is built and its
		// Heartbeat started at create. No set_parent: the window never has to exist for this.
		REQUIRE(gui->create(h.plugin(), kEmbedApiHere, false));
		return gui;
	};
	auto addShaper = [](HostedPlugin& h) {
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    h.plugin()->get_extension(h.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		// Through the bridge, exactly the call the editor's lane makes — and nothing else. The test
		// chain adds; it does NOT provision.
		const uint32_t slot = chain->add_device(h.plugin(), shaper::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		// Audible by construction: full knee and drive, so a provisioned shaper cannot pass dry.
		REQUIRE(chain->set_control(h.plugin(), slot, "Shaper Knee", 1.0));
		REQUIRE(chain->set_control(h.plugin(), slot, "Shaper Drive", 1.0));
	};
	constexpr uint32_t kFrames = 256;
	// Long enough for the shaper's STAGED table build (two entries a frame) and its fade to finish,
	// so the last block compared is the shaper at work and not the shaper still building.
	constexpr int kBlocks = 24;
	auto renderNote = [](HostedPlugin& h) {
		EventList on;
		on.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		REQUIRE(h.run(kFrames, on) == CLAP_PROCESS_CONTINUE);
		EventList none;
		for (int i = 1; i < kBlocks; ++i) {
			REQUIRE(h.run(kFrames, none) == CLAP_PROCESS_CONTINUE);
		}
		return std::vector<float>(h.left().begin(), h.left().begin() + kFrames);
	};
	auto sameSamples = [](const std::vector<float>& a, const std::vector<float>& b) {
		if (a.size() != b.size()) {
			return false;
		}
		for (size_t i = 0; i < a.size(); ++i) {
			if (a[i] != b[i]) {
				return false;
			}
		}
		return true;
	};

	std::vector<float> dry;
	{
		HostedPlugin h(&SilentTimerHost::host());
		const clap_plugin_gui* gui = open(h);
		dry = renderNote(h);
		gui->destroy(h.plugin());
	}
	REQUIRE(peakOf(dry, kFrames) > 0.0f);

	std::vector<float> good;
	{
		HostedPlugin h(&SilentTimerHost::host());
		const clap_plugin_gui* gui = open(h);
		addShaper(h);
		// THE ROAD THAT ALWAYS WORKED: activate provisions whatever chain the bridge holds (the same
		// road a loaded patch takes). Deactivate and re-activate, as a host does on a rate change.
		const clap_plugin* p = h.plugin();
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, kSampleRate, 1, kMaxFrames));
		REQUIRE(p->start_processing(p));
		good = renderNote(h);
		gui->destroy(h.plugin());
	}
	// The shaper at full knee and drive audibly changes the note — the reference is not vacuous.
	REQUIRE_FALSE(sameSamples(good, dry));

	std::vector<float> mac;
	{
		HostedPlugin h(&SilentTimerHost::host());
		const clap_plugin_gui* gui = open(h);
		addShaper(h);
		// THE MAC HOST SEEN: no host callback ever. Only the editor's own Heartbeat, which visage fires from
		// its clock — so wait out the period and check the timers, as AppKit's main loop would.
		std::this_thread::sleep_for(std::chrono::milliseconds(feathers::Gui::kTimerPeriodMs + 8));
		visage::EventManager::instance().checkEventTimers();
		mac = renderNote(h);
		gui->destroy(h.plugin());
	}
	// BY VALUE: the heartbeat-provisioned shaper renders EXACTLY what the host-timer-provisioned one
	// did. Before the fix this read equal to DRY: the card was on and the synth had never been told.
	CHECK(sameSamples(mac, good));
	CHECK_FALSE(sameSamples(mac, dry));
}

// ---------------------------------------------------------------------------------------------------
// THE DENORMAL GUARD. The plugin never set flush-to-zero itself (MEASURED 2026-09-07: nothing in src/
// touched MXCSR or FPCR); it lived on the host's audio-thread setting, which no host promises.
// ---------------------------------------------------------------------------------------------------
#include "plugin/denormal_guard.h"

TEST_CASE("the denormal guard flushes a denormal to zero inside its scope and puts the host's control word back after") {
	const unsigned long long before = feathers::DenormalGuard::currentControlWord();
	// A value in the denormal range, kept out of the compiler's hands: with FTZ/DAZ (or FZ) set, an
	// operation on it yields exactly zero; without, a tiny nonzero.
	volatile float tiny = 1.0e-40f;
	{
		const feathers::DenormalGuard guard;
		const volatile float product = tiny * 1.0f;
		if (feathers::DenormalGuard::available()) {
			CHECK(product == 0.0f);
		}
		else {
			MESSAGE("no flush-to-zero bit on this platform — restore asserted only");
		}
	}
	// AND THE HOST'S WORD IS EXACTLY WHAT IT WAS, whatever it was.
	CHECK(feathers::DenormalGuard::currentControlWord() == before);
}

// THE SAME TWO PROMISES MADE BY process() ITSELF, which the case above cannot see: it holds the class,
// and deleting the guard from process() left it green. The probe (denormal_probe.h) reads the FPU from
// the host's `in_events->size()`, which the plugin calls from inside the block.
#include "denormal_probe.h"

namespace {
/// One block with the probe as the host's input events; returns the control word the host sees AFTER.
unsigned long long processWithProbe(const clap_plugin* plugin, DenormalProbe& probe) {
	std::vector<float> left(kMaxFrames, 0.0f), right(kMaxFrames, 0.0f);
	float* channels[2] = {left.data(), right.data()};
	clap_audio_buffer output{};
	output.data32 = channels;
	output.channel_count = 2;
	const clap_input_events in = probe.events();
	const clap_output_events out{nullptr, outputTryPush};
	clap_process process{};
	process.frames_count = kMaxFrames;
	process.audio_outputs = &output;
	process.audio_outputs_count = 1;
	process.in_events = &in;
	process.out_events = &out;
	REQUIRE(plugin->process(plugin, &process) != CLAP_PROCESS_ERROR);
	return feathers::DenormalGuard::currentControlWord();
}
} // namespace

TEST_CASE("feathers.clap process() flushes denormals inside the block and hands the host back its own control word, flushing or not") {
	HostedPlugin h;
	// A HOST THAT SETS NOTHING: the test thread's own word, which must not already flush, or the
	// inside reading below proves nothing.
	REQUIRE_FALSE(DenormalProbe::flushesNow());
	// Read AFTER the probe: a denormal multiplied without DAZ raises the sticky DE flag, which is part of the
	// word, so a word read before it would differ from one read after by the test's own arithmetic.
	const unsigned long long hostWord = feathers::DenormalGuard::currentControlWord();
	DenormalProbe plain;
	const unsigned long long afterPlain = processWithProbe(h.plugin(), plain);
	REQUIRE(plain.called);
	if (feathers::DenormalGuard::available()) {
		CHECK(plain.flushedInside);
		CHECK(plain.wordInside != hostWord);
	}
	CHECK(afterPlain == hostWord); // BY VALUE: not "the flush bits are clear", the word as it was
	CHECK_FALSE(DenormalProbe::flushesNow());

	// A HOST THAT ALREADY FLUSHES (Bitwig does): its word comes back with the bits still SET, so a
	// guard that "restores" by clearing them is caught here and not only one that forgets.
	{
		const feathers::DenormalGuard hostsOwn;
		const unsigned long long flushingWord = feathers::DenormalGuard::currentControlWord();
		DenormalProbe flushing;
		const unsigned long long afterFlushing = processWithProbe(h.plugin(), flushing);
		REQUIRE(flushing.called);
		CHECK(afterFlushing == flushingWord);
		if (feathers::DenormalGuard::available()) {
			CHECK(DenormalProbe::flushesNow());
		}
	}
	CHECK(feathers::DenormalGuard::currentControlWord() == hostWord);
}

// ---------------------------------------------------------------------------------------------------
// MACRO A9 — THE DAW SEES A MACRO'S NAME. A rename changes what `params.get_info` answers for the
// macro's id, and the host is asked to re-read it with `CLAP_PARAM_RESCAN_INFO`: the flag
// `clap/ext/params.h` gives a name change ("New info takes effect immediately"), as against
// `CLAP_PARAM_RESCAN_ALL`, which "can only be used while the plugin is deactivated". The flag values
// asserted below are the header's constants, never numbers of ours.
// ---------------------------------------------------------------------------------------------------
namespace {

/// A HOST THAT OFFERS `clap_host_params` AND WRITES DOWN EVERY RESCAN IT IS ASKED FOR, in order, with
/// its flags. It must not move once a plugin holds it: `host_data` points at it.
struct RescanHost {
	std::vector<clap_param_rescan_flags> rescans;
	clap_host host {};

	RescanHost() {
		host = kHost;
		host.name = "feathers tests (rescan log)";
		host.host_data = this;
		host.get_extension = getExtension;
	}
	RescanHost(const RescanHost&) = delete;
	RescanHost& operator=(const RescanHost&) = delete;

	static const void* CLAP_ABI getExtension(const clap_host*, const char* id) {
		static const clap_host_params kParams { rescan, clear, requestFlush };
		return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &kParams : nullptr;
	}
	static void CLAP_ABI rescan(const clap_host* h, clap_param_rescan_flags flags) {
		static_cast<RescanHost*>(h->host_data)->rescans.push_back(flags);
	}
	static void CLAP_ABI clear(const clap_host*, clap_id, clap_param_clear_flags) {}
	static void CLAP_ABI requestFlush(const clap_host*) {}

	/// How many of the rescans since `from` carried INFO.
	[[nodiscard]] size_t infoRescansSince(size_t from) const {
		size_t n = 0;
		for (size_t i = from; i < rescans.size(); ++i) {
			n += (rescans[i] & CLAP_PARAM_RESCAN_INFO) != 0 ? 1 : 0;
		}
		return n;
	}
};

/// WHAT THE HOST IS TOLD PARAMETER `id` IS CALLED — by walking `get_info` for the id, as a host does,
/// rather than by an index of ours.
std::string hostNameOf(const clap_plugin* plugin, clap_id id) {
	const auto* params = static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	for (uint32_t i = 0; i < params->count(plugin); ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(plugin, i, &info));
		if (info.id == id) {
			return info.name;
		}
	}
	FAIL("no parameter with id " << id);
	return {};
}

clap_id macroId(size_t index) {
	return static_cast<clap_id>(feathers::macroParam(index));
}

/// Rename a macro through the test road — the caption's own bridge call.
void renameMacro(const clap_plugin* plugin, uint32_t index, const char* name) {
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(chain->rename_macro != nullptr);
	REQUIRE(chain->rename_macro(plugin, index, name));
}

} // namespace

TEST_CASE("an unrenamed macro keeps its default name in paramsInfo") {
	HostedPlugin h;
	// THE NAMES EVERY BUILD BEFORE THIS ONE REPORTED — spelled here, not read from our table, because
	// they are what a DAW already has on its lanes.
	CHECK(hostNameOf(h.plugin(), macroId(0)) == "Macro 1");
	CHECK(hostNameOf(h.plugin(), macroId(1)) == "Macro 2");
	CHECK(hostNameOf(h.plugin(), macroId(2)) == "Macro 3");
	CHECK(hostNameOf(h.plugin(), macroId(3)) == "Macro 4");
	// AND A NAME TAKEN AWAY AGAIN IS THE DEFAULT, not the name it had.
	renameMacro(h.plugin(), 0, "Cutoff");
	REQUIRE(hostNameOf(h.plugin(), macroId(0)) == "Cutoff");
	renameMacro(h.plugin(), 0, "");
	CHECK(hostNameOf(h.plugin(), macroId(0)) == "Macro 1");
}

TEST_CASE("paramsInfo reports a renamed macro's name") {
	HostedPlugin h;
	renameMacro(h.plugin(), 1, "Wobble");
	CHECK(hostNameOf(h.plugin(), macroId(1)) == "Wobble");
	// ONLY THAT ONE: its neighbours keep theirs, and the id it is reported under is the one it had.
	CHECK(hostNameOf(h.plugin(), macroId(0)) == "Macro 1");
	CHECK(hostNameOf(h.plugin(), macroId(2)) == "Macro 3");
}

TEST_CASE("a rename asks the host to rescan info, once") {
	RescanHost log;
	HostedPlugin h(&log.host);
	const size_t before = log.rescans.size();
	renameMacro(h.plugin(), 2, "Grit");
	REQUIRE(log.rescans.size() == before + 1);
	const clap_param_rescan_flags flags = log.rescans.back();
	CHECK((flags & CLAP_PARAM_RESCAN_INFO) != 0);
	// NOT ALL: the list and the ids did not move, and ALL is refused to an active plugin.
	CHECK((flags & CLAP_PARAM_RESCAN_ALL) == 0);
	// AND THE NAME IS THERE BY THE TIME THE HOST RE-READS IT.
	CHECK(hostNameOf(h.plugin(), macroId(2)) == "Grit");

	// THE SAME NAME AGAIN IS NOT A RENAME, and costs the host nothing.
	renameMacro(h.plugin(), 2, "Grit");
	CHECK(log.rescans.size() == before + 1);
}

TEST_CASE("a state load that renames three macros rescans info once") {
	// THE PATCH, MADE THE WAY A USER MAKES ONE: three macros named in one instance, saved through the
	// real state road.
	StateBytes saved;
	{
		HostedPlugin writer;
		renameMacro(writer.plugin(), 0, "Alpha");
		renameMacro(writer.plugin(), 1, "Beta");
		renameMacro(writer.plugin(), 3, "Delta");
		const auto* state = static_cast<const clap_plugin_state*>(
		    writer.plugin()->get_extension(writer.plugin(), CLAP_EXT_STATE));
		REQUIRE(state != nullptr);
		clap_ostream out = saved.out();
		REQUIRE(state->save(writer.plugin(), &out));
	}

	RescanHost log;
	HostedPlugin h(&log.host);
	const auto* state =
	    static_cast<const clap_plugin_state*>(h.plugin()->get_extension(h.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	const size_t before = log.rescans.size();
	{
		clap_istream in = saved.in();
		REQUIRE(state->load(h.plugin(), &in));
	}
	// ONE CALL FOR THE WHOLE LOAD — the values' rescan the load always sent, now carrying INFO too —
	// not one per renamed macro.
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_ALL) == 0);
	CHECK(hostNameOf(h.plugin(), macroId(0)) == "Alpha");
	CHECK(hostNameOf(h.plugin(), macroId(1)) == "Beta");
	CHECK(hostNameOf(h.plugin(), macroId(2)) == "Macro 3");
	CHECK(hostNameOf(h.plugin(), macroId(3)) == "Delta");

	// THE SAME PATCH AGAIN RENAMES NOTHING, so INFO is not sent — which is what makes the first INFO
	// the load's own and not a flag this road always sets.
	const size_t reload = log.rescans.size();
	saved.read = 0;
	{
		clap_istream in = saved.in();
		REQUIRE(state->load(h.plugin(), &in));
	}
	REQUIRE(log.rescans.size() == reload + 1);
	CHECK(log.infoRescansSince(reload) == 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
}

// ---------------------------------------------------------------------------------------------------
// HOST SLOTS, THE READ SIDE (backlog JU, host slots T6a): 64 params past the core
// table, at ids 0x1000.., named by the pool and read through the control each one maps. Every id and flag
// asserted below is spelled from the plan and the CLAP header, never read back from our constants — except
// the core count, which is the table's own. The plan's case name said "146"; voice stacking (Stack, Stack
// Detune) made it 148, so the case names the constant rather than a number the next core param would falsify.
// ---------------------------------------------------------------------------------------------------
namespace {

const clap_plugin_params* paramsOf(const clap_plugin* plugin) {
	const auto* params = static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	return params;
}

const clap_plugin_feathers_test_chain* testChainOf(const clap_plugin* plugin) {
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	return chain;
}

/// Slot `n`'s info, found by its CLAP id as a host finds it.
clap_param_info slotInfo(const clap_plugin* plugin, uint32_t n) {
	const clap_plugin_params* params = paramsOf(plugin);
	for (uint32_t i = 0; i < params->count(plugin); ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(plugin, i, &info));
		if (info.id == 0x1000u + n) {
			return info;
		}
	}
	FAIL("no slot " << n);
	return {};
}

double slotValue(const clap_plugin* plugin, uint32_t n) {
	double value = -1.0;
	REQUIRE(paramsOf(plugin)->get_value(plugin, 0x1000u + n, &value));
	return value;
}

std::string slotText(const clap_plugin* plugin, uint32_t n, double value) {
	char text[CLAP_NAME_SIZE] {};
	REQUIRE(paramsOf(plugin)->value_to_text(plugin, 0x1000u + n, value, text, sizeof(text)));
	return text;
}

/// The filter's declaration of the control whose key is `key`, from the registry the plugin compiles.
const feathers::device::ParamDesc& filterParam(const char* key) {
	const feathers::device::Descriptor* desc = feathers::deviceForTag(feathers::fx::filter::kTraits.tag);
	REQUIRE(desc != nullptr);
	for (const feathers::device::ParamDesc& p : desc->params) {
		if (std::strcmp(p.key, key) == 0) {
			return p;
		}
	}
	FAIL("the filter declares no " << key);
	return desc->params[0];
}

void mainThreadVisit(const clap_plugin* plugin) { plugin->on_main_thread(plugin); }

bool saveState(const clap_plugin* plugin, StateBytes* into) {
	const auto* state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	clap_ostream out = into->out();
	return state->save(plugin, &out);
}

bool loadState(const clap_plugin* plugin, StateBytes* from) {
	const auto* state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	from->read = 0;
	clap_istream in = from->in();
	return state->load(plugin, &in);
}

// " · " in UTF-8, spelled once.
const std::string kDot = "\xc2\xb7";

} // namespace

TEST_CASE("the host sees kParamCount + kHostSlotCapacity params; slot ids run from 0x1000") {
	// NAMED FROM THE CONSTANT (256 since 2026-09-26, 64 before), so the next growth does not falsify it.
	constexpr uint32_t kSlots = feathers::hostslots::kHostSlotCapacity;
	HostedPlugin h;
	const clap_plugin_params* params = paramsOf(h.plugin());
	const uint32_t core = static_cast<uint32_t>(feathers::kParamCount);
	REQUIRE(params->count(h.plugin()) == core + kSlots);
	for (uint32_t i = 0; i < core; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(h.plugin(), i, &info));
		// NO CORE ID MAY SIT IN THE SLOTS' RANGE, or one id would name two parameters.
		CHECK(info.id < 0x1000u);
	}
	for (uint32_t n = 0; n < kSlots; ++n) {
		clap_param_info info {};
		REQUIRE(params->get_info(h.plugin(), core + n, &info));
		CHECK(info.id == 0x1000u + n);
	}
	clap_param_info first {};
	clap_param_info last {};
	REQUIRE(params->get_info(h.plugin(), core, &first));
	REQUIRE(params->get_info(h.plugin(), core + kSlots - 1, &last));
	CHECK(first.id == 0x1000u);
	CHECK(last.id == 0x1000u + kSlots - 1);
	// (Nothing past the last is probed: clap-helpers counts an out-of-range index or an unknown id as HOST
	// misbehaviour, and a debug build terminates on it.)
}

TEST_CASE("an empty slot is named 'Slot N' and hidden") {
	HostedPlugin h;
	for (uint32_t n = 0; n < feathers::hostslots::kHostSlotCapacity; ++n) {
		const clap_param_info info = slotInfo(h.plugin(), n);
		CHECK(std::string(info.name) == "Slot " + std::to_string(n + 1));
		CHECK(std::string(info.module) == "Slots");
		CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) != 0);
		CHECK((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
		CHECK((info.flags & CLAP_PARAM_IS_MODULATABLE) != 0);
		CHECK((info.flags & CLAP_PARAM_IS_STEPPED) == 0);
		CHECK(info.min_value == 0.0);
		CHECK(info.max_value == 1.0);
		CHECK(info.default_value == 0.0);
		CHECK(slotValue(h.plugin(), n) == 0.0);
		// AND IT PARSES NOTHING: text-to-value is not in v1.
		double parsed = 0.0;
		CHECK_FALSE(paramsOf(h.plugin())->text_to_value(h.plugin(), 0x1000u + n, "0.5", &parsed));
	}
}

TEST_CASE("a pinned slot is named 'Filter · Cutoff' and its text is the filter's") {
	HostedPlugin h;
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	// A fresh instance's lane holds one filter, at position 0 (`defaultFxChain`).
	REQUIRE(chain->device_count(h.plugin()) == 1);
	const double cutoff = std::log2(1200.0);
	REQUIRE(chain->set_control(h.plugin(), 0, "Filter Cutoff", cutoff));
	REQUIRE(chain->pin_slot(h.plugin(), 0, "Filter Cutoff") == 0u);

	const clap_param_info info = slotInfo(h.plugin(), 0);
	CHECK(std::string(info.name) == "Filter " + kDot + " Cutoff");
	CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) == 0);
	CHECK(std::string(info.module) == "Slots");

	// THE VALUE: the cutoff's position along its declared range, restated here from the range itself
	// (log2 0.5 .. log2 24000 since Z8, `filter_types.h`), not from `toNormal`.
	const double expected = (std::log2(1200.0) - std::log2(0.5)) / (std::log2(24000.0) - std::log2(0.5));
	CHECK(slotValue(h.plugin(), 0) == doctest::Approx(expected).epsilon(1e-6));

	// THE TEXT IS THE FILTER'S: "1.2kHz" at the knob's value, and the ends of the sweep read as the ends.
	CHECK(slotText(h.plugin(), 0, slotValue(h.plugin(), 0)) == "1.2kHz");
	CHECK(slotText(h.plugin(), 0, 0.0) == "0.5Hz");
	CHECK(slotText(h.plugin(), 0, 1.0) == "24kHz");
	// AND IT IS formatValue's OWN TEXT, not a copy of it that agrees today.
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	CHECK(slotText(h.plugin(), 0, 0.37) == feathers::device::formatValue(desc, feathers::device::fromNormal(desc, 0.37)));

	// ITS NEIGHBOUR IS UNTOUCHED.
	CHECK(std::string(slotInfo(h.plugin(), 1).name) == "Slot 2");
}

TEST_CASE("KO: a takeover tells the host to re-read the slot's VALUE, and the value it reads is the control's") {
	// A SLOT THE HOST SAW EMPTY READ 0.0, and a host keeps what it read until it is told to read again. A rename
	// rescan of INFO|TEXT alone left that 0.0 in the host's model of a slot that now drives a control standing
	// somewhere else — a value the host could later write back. So the takeover's rescan carries VALUES, and what
	// the host then reads is the control's own position, restated from the declaration.
	RescanHost log;
	HostedPlugin h(&log.host);
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	const double cutoff = std::log2(2500.0);
	REQUIRE(chain->set_control(h.plugin(), 0, "Filter Cutoff", cutoff));
	mainThreadVisit(h.plugin());
	REQUIRE(slotValue(h.plugin(), 0) == 0.0); // EMPTY: what the host caches
	const size_t before = log.rescans.size();

	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	mainThreadVisit(h.plugin());
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
	const double stored = static_cast<double>(feathers::device::coerceControl(desc, static_cast<float>(cutoff)));
	CHECK(slotValue(h.plugin(), 0) == doctest::Approx(feathers::device::toNormal(desc, stored)).epsilon(1e-9));
	CHECK(slotValue(h.plugin(), 0) != 0.0);
}

TEST_CASE("three takeovers in one visit cost one RESCAN_INFO") {
	RescanHost log;
	HostedPlugin h(&log.host);
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	const size_t before = log.rescans.size();
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Res") == 1u);
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Drive") == 2u);
	// NOTHING YET: a touch owes the host a rename, and the visit pays it.
	CHECK(log.rescans.size() == before);

	mainThreadVisit(h.plugin());
	CHECK(log.infoRescansSince(before) == 1);
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_TEXT) != 0);
	// AND THE VALUES, IN THE SAME ONE (backlog KO): three slots changed hands, so all three read new values.
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_ALL) == 0);
	CHECK(std::string(slotInfo(h.plugin(), 2).name) == "Filter " + kDot + " Drive");

	// A VISIT WITH NOTHING OWED SENDS NOTHING.
	mainThreadVisit(h.plugin());
	CHECK(log.rescans.size() == before + 1);
	// AND A TOUCH OF A CONTROL THAT ALREADY HAS ITS SLOT RENAMES NOTHING.
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Res") == 1u);
	mainThreadVisit(h.plugin());
	CHECK(log.rescans.size() == before + 1);
}

namespace {

/// A HOST THAT ANSWERS A RESCAN THE WAY A DAW MAY: synchronously, inside the call, re-reading every parameter's
/// info, value and text that the flags name. It counts our callbacks and times the walk, for the measurement below.
/// A real host may defer or skip parts of this; the count is what CLAP permits it to ask, not what Bitwig does.
struct ReReadingHost {
	clap_host host {};
	const clap_plugin* plugin = nullptr;
	size_t infos = 0;
	size_t values = 0;
	size_t texts = 0;
	size_t rescans = 0;
	double walkSeconds = 0.0;
	double infoSeconds = 0.0;
	double valueSeconds = 0.0;
	double slotValueSeconds = 0.0;
	double coreTextSeconds = 0.0;
	double slotTextSeconds = 0.0;

	ReReadingHost() {
		host = kHost;
		host.name = "feathers tests (re-reading host)";
		host.host_data = this;
		host.get_extension = getExtension;
	}
	ReReadingHost(const ReReadingHost&) = delete;
	ReReadingHost& operator=(const ReReadingHost&) = delete;

	static const void* CLAP_ABI getExtension(const clap_host*, const char* id) {
		static const clap_host_params kParams { rescan, clear, requestFlush };
		return std::strcmp(id, CLAP_EXT_PARAMS) == 0 ? &kParams : nullptr;
	}
	static void CLAP_ABI rescan(const clap_host* h, clap_param_rescan_flags flags) {
		auto* self = static_cast<ReReadingHost*>(h->host_data);
		++self->rescans;
		if (self->plugin == nullptr) {
			return;
		}
		const auto* params =
		    static_cast<const clap_plugin_params*>(self->plugin->get_extension(self->plugin, CLAP_EXT_PARAMS));
		const auto start = std::chrono::steady_clock::now();
		const uint32_t count = params->count(self->plugin);
		using Clock = std::chrono::steady_clock;
		const auto since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
		for (uint32_t i = 0; i < count; ++i) {
			clap_param_info info {};
			auto t = Clock::now();
			const bool got = params->get_info(self->plugin, i, &info);
			self->infoSeconds += since(t);
			if (!got) {
				continue;
			}
			const bool slot = feathers::hostslots::isSlotId(info.id);
			if ((flags & CLAP_PARAM_RESCAN_INFO) != 0) {
				++self->infos;
			}
			double value = 0.0;
			if ((flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT)) != 0) {
				t = Clock::now();
				(void)params->get_value(self->plugin, info.id, &value);
				(slot ? self->slotValueSeconds : self->valueSeconds) += since(t);
				self->values += (flags & CLAP_PARAM_RESCAN_VALUES) != 0 ? 1 : 0;
			}
			if ((flags & CLAP_PARAM_RESCAN_TEXT) != 0) {
				char text[256];
				t = Clock::now();
				(void)params->value_to_text(self->plugin, info.id, value, text, sizeof(text));
				(slot ? self->slotTextSeconds : self->coreTextSeconds) += since(t);
				++self->texts;
			}
		}
		self->walkSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}
	static void CLAP_ABI clear(const clap_host*, clap_id, clap_param_clear_flags) {}
	static void CLAP_ABI requestFlush(const clap_host*) {}
};

} // namespace

TEST_CASE("MEASURE: a first grab's rescan, re-read by a host that walks every parameter, against a re-grab") {
	// SLOT-RESCAN-COST: what a first grab (an unslotted control taking a slot) costs the main thread on OUR side,
	// against a re-grab of a control that already owns its slot. The visit is timed end to end; the host's walk
	// inside it is timed separately, so the plugin's own share of the visit is the difference.
	ReReadingHost log;
	HostedPlugin h(&log.host);
	log.plugin = h.plugin();
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	mainThreadVisit(h.plugin());
	const auto* params = static_cast<const clap_plugin_params*>(h.plugin()->get_extension(h.plugin(), CLAP_EXT_PARAMS));
	const uint32_t count = params->count(h.plugin());
	const auto timedVisit = [&] {
		const auto start = std::chrono::steady_clock::now();
		mainThreadVisit(h.plugin());
		return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	};
	const char* controls[] = { "Filter Cutoff", "Filter Res", "Filter Drive" };
	for (const char* control : controls) {
		const size_t rescans = log.rescans;
		const size_t infos = log.infos;
		const size_t values = log.values;
		const size_t texts = log.texts;
		const double walk = log.walkSeconds;
		const double infoT = log.infoSeconds;
		const double valueT = log.valueSeconds;
		const double slotValueT = log.slotValueSeconds;
		const double coreT = log.coreTextSeconds;
		const double slotT = log.slotTextSeconds;
		REQUIRE(chain->touch_slot(h.plugin(), 0, control) != UINT32_MAX);
		const double first = timedVisit();
		const size_t firstRescans = log.rescans - rescans;
		const std::string name = control;
		MESSAGE("FIRST grab of " << name << ": params " << count << ", rescans " << firstRescans << ", get_info "
		                         << log.infos - infos << ", get_value " << log.values - values << ", value_to_text "
		                         << log.texts - texts << ", host walk " << (log.walkSeconds - walk) * 1e3 << " ms, visit "
		                         << first * 1e3 << " ms; split ms: get_info " << (log.infoSeconds - infoT) * 1e3
		                         << ", core get_value " << (log.valueSeconds - valueT) * 1e3 << ", slot get_value "
		                         << (log.slotValueSeconds - slotValueT) * 1e3 << ", core text "
		                         << (log.coreTextSeconds - coreT) * 1e3 << ", slot text " << (log.slotTextSeconds - slotT) * 1e3);
		CHECK(firstRescans == 1);
		const size_t afterFirst = log.rescans;
		REQUIRE(chain->touch_slot(h.plugin(), 0, control) != UINT32_MAX);
		const double again = timedVisit();
		MESSAGE("RE-GRAB of " << name << ": rescans " << log.rescans - afterFirst << ", visit " << again * 1e3 << " ms");
		CHECK(log.rescans == afterFirst);
	}
	// AND A HAND'S GRAB, TIMED WHOLE: a first grab now pays its takeover visit (rebind and rescan, the host's walk
	// included) inside the hand's own call; a re-grab pays none.
	const uint32_t seated = chain->add_mod_device(h.plugin(), feathers::mod::lfo::kTraits.tag);
	REQUIRE(seated != UINT32_MAX);
	mainThreadVisit(h.plugin());
	const double rate[2] = { 0.3, 0.6 };
	for (const char* which : { "FIRST ", "RE-" }) {
		const size_t rescans = log.rescans;
		const auto start = std::chrono::steady_clock::now();
		REQUIRE(chain->drag_control(h.plugin(), CLAP_FEATHERS_TEST_STRIP | seated, "Rate", rate, 2));
		const double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 1e3;
		MESSAGE(std::string(which) << "GRAB of LFO Rate, the hand's call: " << ms << " ms, rescans inside it "
		                           << log.rescans - rescans);
	}
}

TEST_CASE("the param index clap-helpers is told is the get_info walk's for every id, -1 for every other, and builds no info") {
	// clap-helpers asks `getParamIndexForParamId` on every value / value_to_text / text_to_value call. Its own version
	// walks every param's info; ours answers in O(1). THE CLAIM IS TWO-SIDED: the same answer as the walk (restated
	// here from `get_info`, the host's own road) for every id a host could send, AND no `clap_param_info` built to give it.
	HostedPlugin h;
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	const auto* params = static_cast<const clap_plugin_params*>(h.plugin()->get_extension(h.plugin(), CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	const uint32_t count = params->count(h.plugin());
	REQUIRE(count == feathers::kParamCount + feathers::hostslots::kDeclaredSlots);
	std::map<clap_id, int32_t> walk;
	for (uint32_t i = 0; i < count; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(h.plugin(), i, &info));
		REQUIRE(walk.count(info.id) == 0); // ids are unique, or the walk's "first match" would be the answer
		walk[info.id] = static_cast<int32_t>(i);
	}
	// EVERY ID UP PAST THE LAST SLOT: the core span, the gap between it and the slots, the slots, and beyond — plus the
	// far end of the id space.
	std::vector<clap_id> ids;
	for (clap_id id = 0; id < feathers::hostslots::kFirstSlotId + feathers::hostslots::kDeclaredSlots + 64; ++id) {
		ids.push_back(id);
	}
	ids.push_back(UINT32_MAX - 1);
	ids.push_back(UINT32_MAX);
	size_t known = 0;
	for (const clap_id id : ids) {
		const auto at = walk.find(id);
		const int32_t expected = at != walk.end() ? at->second : -1;
		known += at != walk.end() ? 1 : 0;
		if (chain->param_index_of(h.plugin(), id) != expected) {
			CAPTURE(id);
			CHECK(chain->param_index_of(h.plugin(), id) == expected);
		}
	}
	CHECK(known == count); // every param's id was among those asked

	// AND NO WALK OF OURS: a value, a text and a parse of the LAST param (the walk's worst case) build no
	// `clap_param_info` for the id check. AT THE CHECKING LEVEL THIS BUILD RUNS (`plugin/clap_checking.h`): Minimal
	// (release) builds none; Maximal (debug, the sanitizer gate) adds clap-helpers' own range validation, which walks
	// every param per call — exactly `count`, so a linear id check on top of it would still show (up to 2 × count).
	const uint64_t perCall = feathers::kClapChecking == clap::helpers::CheckingLevel::Maximal ? count : 0;
	clap_param_info last {};
	REQUIRE(params->get_info(h.plugin(), count - 1, &last));
	double value = 0.0;
	uint64_t before = chain->param_info_builds(h.plugin());
	CHECK(params->get_value(h.plugin(), last.id, &value));
	CHECK(chain->param_info_builds(h.plugin()) - before == perCall);
	char text[256];
	before = chain->param_info_builds(h.plugin());
	CHECK(params->value_to_text(h.plugin(), last.id, value, text, sizeof(text)));
	CHECK(chain->param_info_builds(h.plugin()) - before == perCall);
	double parsed = 0.0;
	before = chain->param_info_builds(h.plugin());
	// An Empty slot parses nothing; the id check still runs. clap-helpers validates a parse's range only when it parsed.
	const bool read = params->text_to_value(h.plugin(), last.id, text, &parsed);
	CHECK(chain->param_info_builds(h.plugin()) - before == (read ? perCall : 0));
	// An unknown id is refused, as the base refuses it, and builds nothing. Release only: at Maximal a host asking for
	// an unknown id is MISBEHAVING and the debug handler terminates the process (clap-helpers' contract).
	if (feathers::kClapChecking == clap::helpers::CheckingLevel::Minimal) {
		before = chain->param_info_builds(h.plugin());
		CHECK_FALSE(params->get_value(h.plugin(), feathers::hostslots::kFirstSlotId - 1, &value));
		CHECK(chain->param_info_builds(h.plugin()) - before == 0);
	}
}

TEST_CASE("a first grab's rescan builds each param's info ONCE: the host's own get_info and nothing per value or text") {
	// THE QUADRATIC, AS A COUNT. A host answering the rescan re-reads every param's info, value and text; with an
	// O(n) id check behind each value and text that was about 3n² builds. Now the only builds of OURS are the host's n
	// get_infos. At Maximal (debug, the sanitizer gate) clap-helpers' range validation adds exactly n per value and per
	// text call (`plugin/clap_checking.h`), 2n² in all — exact, so a linear id check on top would still show.
	ReReadingHost log;
	HostedPlugin h(&log.host);
	log.plugin = h.plugin();
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	mainThreadVisit(h.plugin());
	const auto* params = static_cast<const clap_plugin_params*>(h.plugin()->get_extension(h.plugin(), CLAP_EXT_PARAMS));
	const uint32_t count = params->count(h.plugin());
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	const size_t rescans = log.rescans;
	const uint64_t before = chain->param_info_builds(h.plugin());
	mainThreadVisit(h.plugin());
	REQUIRE(log.rescans == rescans + 1);
	const uint64_t validation =
	    feathers::kClapChecking == clap::helpers::CheckingLevel::Maximal ? 2ull * count * count : 0ull;
	CHECK(chain->param_info_builds(h.plugin()) - before == count + validation);
}

TEST_CASE("a first grab is named to the host BEFORE its Begin: the rescan lands with the grab, and the next block carries the "
          "Begin on the named slot; a re-grab is unchanged — fx lane and strip") {
	// A HOST'S RECENTLY-TOUCHED LIST is fed by the Begin. A first grab's Begin used to wait a visit, and reached the host on
	// a slot it last knew as a hidden "Slot N"; a re-grab's went straight out. Now `beginControlGesture` services a first
	// grab at once: by the time the hand's call returns, the host has had its rescan, and the very next block carries the
	// takeover's Value, then the Begin, on the slot now named.
	struct Lane {
		const char* what;
		bool strip;
		const char* control;
		const char* named;
	};
	for (const Lane& lane : { Lane { "fx lane", false, "Filter Cutoff", "Filter \xc2\xb7 Cutoff" },
	                          Lane { "strip", true, "Rate", "LFO \xc2\xb7 Rate" } }) {
		CAPTURE(lane.what);
		RescanHost rescans;
		HostedPlugin h(&rescans.host);
		const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
		uint32_t at = 0;
		if (lane.strip) {
			const uint32_t seated = chain->add_mod_device(h.plugin(), feathers::mod::lfo::kTraits.tag);
			REQUIRE(seated != UINT32_MAX);
			at = CLAP_FEATHERS_TEST_STRIP | seated;
		}
		mainThreadVisit(h.plugin());
		EventList none;
		{
			OutputLog settle;
			h.run(256, none, &settle);
		}
		const auto slotEvents = [&](const OutputLog& out) {
			std::vector<uint16_t> kinds;
			for (const OutputLog::Event& e : out.events) {
				if (e.id == feathers::hostslots::kFirstSlotId) {
					kinds.push_back(e.type);
				}
			}
			return kinds;
		};
		double drive[2] = { 0.3, 0.6 };
		if (!lane.strip) {
			drive[0] = std::log2(500.0);
			drive[1] = std::log2(2000.0);
		}

		// FIRST GRAB: the rescan is had inside the hand's own call — no visit, no block in between.
		const size_t before = rescans.rescans.size();
		REQUIRE(chain->drag_control(h.plugin(), at, lane.control, drive, 2));
		REQUIRE(rescans.rescans.size() == before + 1);
		CHECK((rescans.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
		CHECK(std::string(slotInfo(h.plugin(), 0).name) == lane.named);
		CHECK((slotInfo(h.plugin(), 0).flags & CLAP_PARAM_IS_HIDDEN) == 0);
		OutputLog first;
		h.run(256, none, &first);
		const std::vector<uint16_t> f = slotEvents(first);
		REQUIRE(f.size() >= 3);
		CHECK(f[0] == CLAP_EVENT_PARAM_VALUE);         // the takeover, outside the gesture
		CHECK(f[1] == CLAP_EVENT_PARAM_GESTURE_BEGIN); // the hand's Begin, in the very next block
		CHECK(f.back() == CLAP_EVENT_PARAM_GESTURE_END);
		CHECK(std::count(f.begin(), f.end(), CLAP_EVENT_PARAM_GESTURE_BEGIN) == 1);
		// AND THE NEXT VISIT OWES NOTHING: the takeover was paid at the grab.
		const size_t paid = rescans.rescans.size();
		mainThreadVisit(h.plugin());
		CHECK(rescans.rescans.size() == paid);

		// RE-GRAB: unchanged — no rescan, and the block carries Begin, Values, End.
		REQUIRE(chain->drag_control(h.plugin(), at, lane.control, drive, 2));
		CHECK(rescans.rescans.size() == paid);
		OutputLog again;
		h.run(256, none, &again);
		const std::vector<uint16_t> r = slotEvents(again);
		REQUIRE(r.size() >= 2);
		CHECK(r.front() == CLAP_EVENT_PARAM_GESTURE_BEGIN);
		CHECK(r.back() == CLAP_EVENT_PARAM_GESTURE_END);
	}
}

TEST_CASE("MEASURE: the ORDER a host hears a first grab and a re-grab in, an fx-lane control against a strip control") {
	// WHAT THE HOST SEES, step by step: the rescans (main thread, inside a visit) and the output events (drained in
	// `process`), for a hand on an UNSLOTTED control and then on the same control again. Logged, not asserted: this
	// is the instrument for "a first grab does not reach the top of the host's recently-touched list".
	struct Lane {
		const char* what;
		bool strip;
		const char* control;
	};
	for (const Lane& lane : { Lane { "fx lane", false, "Filter Cutoff" }, Lane { "strip", true, "Rate" } }) {
		RescanHost rescans;
		HostedPlugin h(&rescans.host);
		const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
		uint32_t at = 0;
		if (lane.strip) {
			const uint32_t seated = chain->add_mod_device(h.plugin(), feathers::mod::lfo::kTraits.tag);
			REQUIRE(seated != UINT32_MAX);
			at = CLAP_FEATHERS_TEST_STRIP | seated;
		}
		mainThreadVisit(h.plugin());
		EventList none;
		{
			OutputLog settle;
			h.run(256, none, &settle);
		}
		std::string timeline;
		const auto step = [&](const char* label, bool visit) {
			const size_t before = rescans.rescans.size();
			if (visit) {
				mainThreadVisit(h.plugin());
			}
			OutputLog out;
			h.run(256, none, &out);
			timeline += std::string("\n  ") + label + ": rescans " + std::to_string(rescans.rescans.size() - before);
			for (const OutputLog::Event& e : out.events) {
				if (!feathers::hostslots::isSlotId(e.id)) {
					continue;
				}
				const char* kind = e.type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? "Begin"
				                   : e.type == CLAP_EVENT_PARAM_GESTURE_END ? "End"
				                                                            : "Value";
				timeline += std::string(" ") + kind + "@slot" + std::to_string(e.id - feathers::hostslots::kFirstSlotId + 1);
			}
		};
		const double values[2] = { 0.3, 0.6 };
		const double* drive = values;
		double scaled[2];
		if (!lane.strip) {
			scaled[0] = std::log2(500.0);
			scaled[1] = std::log2(2000.0);
			drive = scaled;
		}
		// THE HAND'S OWN CALL, TIMED: a first grab now pays its rescan inside it; a re-grab does not.
		const auto timedDrag = [&] {
			const size_t before = rescans.rescans.size();
			const auto start = std::chrono::steady_clock::now();
			REQUIRE(chain->drag_control(h.plugin(), at, lane.control, drive, 2));
			const double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() * 1e3;
			timeline += "\n  the grab's own call: " + std::to_string(ms) + " ms, rescans inside it "
			            + std::to_string(rescans.rescans.size() - before);
		};
		timedDrag();
		step("FIRST grab, its block", false);
		step("next visit + block", true);
		const std::string name = slotInfo(h.plugin(), 0).name;
		timedDrag();
		step("RE-GRAB, its block", false);
		step("next visit + block", true);
		MESSAGE(std::string(lane.what) << " (" << std::string(lane.control) << "), slot 1 named \"" << name << "\":" << timeline);
	}
}

TEST_CASE("a state load restoring slots rescans once") {
	StateBytes saved;
	double cutoffValue = 0.0;
	{
		HostedPlugin writer;
		const clap_plugin_feathers_test_chain* chain = testChainOf(writer.plugin());
		REQUIRE(chain->set_control(writer.plugin(), 0, "Filter Cutoff", std::log2(440.0)));
		REQUIRE(chain->pin_slot(writer.plugin(), 0, "Filter Cutoff") == 0u);
		REQUIRE(chain->pin_slot(writer.plugin(), 0, "Filter Res") == 1u);
		REQUIRE(chain->touch_slot(writer.plugin(), 0, "Filter Drive") == 2u);
		cutoffValue = slotValue(writer.plugin(), 0);
		REQUIRE(saveState(writer.plugin(), &saved));
	}
	REQUIRE(containsTag(saved.bytes, "hostSlots"));

	RescanHost log;
	HostedPlugin h(&log.host);
	const size_t before = log.rescans.size();
	REQUIRE(loadState(h.plugin(), &saved));
	// ONE CALL FOR THE WHOLE LOAD: the values' rescan, carrying the slots' INFO with it.
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_ALL) == 0);
	// AND NOTHING LEFT OWED FOR THE NEXT VISIT TO PAY TWICE.
	mainThreadVisit(h.plugin());
	CHECK(log.rescans.size() == before + 1);

	CHECK(std::string(slotInfo(h.plugin(), 0).name) == "Filter " + kDot + " Cutoff");
	CHECK(std::string(slotInfo(h.plugin(), 1).name) == "Filter " + kDot + " Res");
	CHECK(std::string(slotInfo(h.plugin(), 2).name) == "Filter " + kDot + " Drive");
	CHECK(std::string(slotInfo(h.plugin(), 3).name) == "Slot 4");
	CHECK(slotValue(h.plugin(), 0) == doctest::Approx(cutoffValue).epsilon(1e-9));
	CHECK(slotText(h.plugin(), 0, slotValue(h.plugin(), 0)) == "440Hz");
}

TEST_CASE("removing a device frees its slot; undo restores it") {
	RescanHost log;
	HostedPlugin h(&log.host);
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	REQUIRE(chain->set_control(h.plugin(), 0, "Filter Cutoff", std::log2(2500.0)));
	REQUIRE(chain->pin_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	mainThreadVisit(h.plugin());
	const double pinned = slotValue(h.plugin(), 0);
	REQUIRE(pinned > 0.0);

	size_t before = log.rescans.size();
	REQUIRE(chain->remove_device(h.plugin(), 0));
	REQUIRE(chain->device_count(h.plugin()) == 0);
	mainThreadVisit(h.plugin());
	CHECK(log.infoRescansSince(before) == 1);
	clap_param_info freed = slotInfo(h.plugin(), 0);
	CHECK(std::string(freed.name) == "Slot 1");
	CHECK((freed.flags & CLAP_PARAM_IS_HIDDEN) != 0);
	CHECK(slotValue(h.plugin(), 0) == 0.0);

	before = log.rescans.size();
	REQUIRE(chain->undo_remove(h.plugin()));
	REQUIRE(chain->device_count(h.plugin()) == 1);
	mainThreadVisit(h.plugin());
	CHECK(log.infoRescansSince(before) == 1);
	const clap_param_info back = slotInfo(h.plugin(), 0);
	CHECK(std::string(back.name) == "Filter " + kDot + " Cutoff");
	CHECK((back.flags & CLAP_PARAM_IS_HIDDEN) == 0);
	// THE SAME CONTROL, READ AGAIN — the device came back under its id with its row.
	CHECK(slotValue(h.plugin(), 0) == doctest::Approx(pinned).epsilon(1e-9));
	CHECK(slotText(h.plugin(), 0, slotValue(h.plugin(), 0)) == "2.5kHz");
}

TEST_CASE("a same-id, different-tag replacement frees the slot and rescans once") {
	RescanHost log;
	HostedPlugin h(&log.host);
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	// A SECOND DEVICE, of a kind that is not the filter, at position 1.
	const std::string filterTag = feathers::fx::filter::kTraits.tag;
	std::string otherTag;
	char tag[64] {};
	for (uint32_t i = 0; chain->available_device(h.plugin(), i, tag, sizeof(tag)); ++i) {
		if (filterTag != tag) {
			otherTag = tag;
			break;
		}
	}
	REQUIRE_FALSE(otherTag.empty());
	REQUIRE(chain->add_device(h.plugin(), otherTag.c_str()) == 1u);
	const feathers::device::Descriptor* other = feathers::deviceForTag(otherTag.c_str());
	REQUIRE(other != nullptr);
	REQUIRE(chain->pin_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	REQUIRE(chain->pin_slot(h.plugin(), 1, other->params[0].name) == 1u);
	mainThreadVisit(h.plugin());
	const std::string pinnedName = slotInfo(h.plugin(), 1).name;
	REQUIRE(pinnedName != "Slot 2");

	// A FILTER STAMPED ONTO POSITION 1 UNDER THE SAME ID: nothing was removed, so only a reconcile can see it.
	const size_t before = log.rescans.size();
	REQUIRE(chain->replace_with_filter(h.plugin(), 1));
	REQUIRE(chain->device_count(h.plugin()) == 2);
	mainThreadVisit(h.plugin());
	// EXACTLY ONE RESCAN, carrying INFO: the freed slot's new name reaches the host.
	CHECK(log.rescans.size() == before + 1);
	CHECK(log.infoRescansSince(before) == 1);
	const clap_param_info freed = slotInfo(h.plugin(), 1);
	CHECK(std::string(freed.name) == "Slot 2");
	CHECK((freed.flags & CLAP_PARAM_IS_HIDDEN) != 0);
	// THE OTHER SLOT IS UNTOUCHED.
	CHECK(std::string(slotInfo(h.plugin(), 0).name) == "Filter " + kDot + " Cutoff");
	// AND A VISIT WITH NOTHING MOVED SENDS NOTHING MORE.
	mainThreadVisit(h.plugin());
	CHECK(log.rescans.size() == before + 1);
}

TEST_CASE("a preset load keeps the pool and frees only unresolved targets") {
	// THE PRESET: a fresh instance's lane — the one default filter, under the default filter's id — captured
	// through the preset browser's own road.
	StateBytes preset;
	{
		HostedPlugin maker;
		const clap_plugin_feathers_test_chain* chain = testChainOf(maker.plugin());
		REQUIRE(chain->set_control(maker.plugin(), 0, "Filter Cutoff", std::log2(300.0)));
		clap_ostream out = preset.out();
		REQUIRE(chain->capture_preset(maker.plugin(), &out));
	}

	RescanHost log;
	HostedPlugin h(&log.host);
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	// A SECOND DEVICE, which the preset does not have: the first other tag the lane will take.
	const std::string filterTag = feathers::fx::filter::kTraits.tag;
	std::string otherTag;
	char tag[64] {};
	for (uint32_t i = 0; chain->available_device(h.plugin(), i, tag, sizeof(tag)); ++i) {
		if (filterTag != tag) {
			otherTag = tag;
			break;
		}
	}
	REQUIRE_FALSE(otherTag.empty());
	REQUIRE(chain->add_device(h.plugin(), otherTag.c_str()) == 1u);
	const feathers::device::Descriptor* other = feathers::deviceForTag(otherTag.c_str());
	REQUIRE(other != nullptr);
	REQUIRE(chain->pin_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	REQUIRE(chain->pin_slot(h.plugin(), 1, other->params[0].name) == 1u);
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Res") == 2u);
	mainThreadVisit(h.plugin());
	const std::string otherName = slotInfo(h.plugin(), 1).name;
	REQUIRE(otherName != "Slot 2");

	// A PRESET CARRIES NO SLOTS, and a project does.
	{
		StateBytes captured;
		clap_ostream out = captured.out();
		REQUIRE(chain->capture_preset(h.plugin(), &out));
		CHECK_FALSE(containsTag(captured.bytes, "hostSlots"));
		StateBytes project;
		REQUIRE(saveState(h.plugin(), &project));
		CHECK(containsTag(project.bytes, "hostSlots"));
	}

	const size_t before = log.rescans.size();
	{
		preset.read = 0;
		clap_istream in = preset.in();
		REQUIRE(chain->load_preset(h.plugin(), &in));
	}
	REQUIRE(chain->device_count(h.plugin()) == 1);
	// THE FILTER'S SLOTS STAY — the same id, the same kind, the same keys — and READ THE PRESET'S VALUE.
	CHECK(std::string(slotInfo(h.plugin(), 0).name) == "Filter " + kDot + " Cutoff");
	CHECK(std::string(slotInfo(h.plugin(), 2).name) == "Filter " + kDot + " Res");
	CHECK(slotText(h.plugin(), 0, slotValue(h.plugin(), 0)) == "300Hz");
	// THE OTHER DEVICE'S SLOT IS FREED: its device is not in the preset.
	const clap_param_info freed = slotInfo(h.plugin(), 1);
	CHECK(std::string(freed.name) == "Slot 2");
	CHECK((freed.flags & CLAP_PARAM_IS_HIDDEN) != 0);
	// ONE RESCAN FOR THE LOAD, INFO INCLUDED (the freed slot's rename rides the values' rescan).
	CHECK(log.rescans.size() == before + 1);
	CHECK(log.infoRescansSince(before) == 1);

	// AND THE PROJECT STILL SAVES THE POOL IT KEPT.
	StateBytes after;
	REQUIRE(saveState(h.plugin(), &after));
	CHECK(containsTag(after.bytes, "hostSlots"));
	HostedPlugin reopened;
	REQUIRE(loadState(reopened.plugin(), &after));
	CHECK(std::string(slotInfo(reopened.plugin(), 0).name) == "Filter " + kDot + " Cutoff");
	CHECK(std::string(slotInfo(reopened.plugin(), 1).name) == "Slot 2");
}

// ---------------------------------------------------------------------------------------------------
// HOST SLOTS T6b — THE WRITE ROADS. Every claim is about a VALUE, read where it lives (CLAUDE.md): the
// ENGINE's base and resolved value (`engine_control`), what the KNOB reads (`knob_value`), the POOL's state
// (`slot_state`), and what the plugin pushed to the host's output queue (`OutputLog`). None is inferred from a
// render. Every expected number is restated from the platform's contract — the declared range and
// `fromNormal`/`coerceControl` — never read back from the road under test.
// ---------------------------------------------------------------------------------------------------
namespace {

constexpr clap_id kSlot0 = 0x1000u;
constexpr uint32_t kBlock = 256;

uint32_t stateNumber(feathers::hostslots::SlotState s) { return static_cast<uint32_t>(s); }

struct EngineReading {
	double base = -1.0;
	double resolved = -1.0;
};

EngineReading engineOf(const clap_plugin* plugin, const char* control) {
	EngineReading r;
	REQUIRE(testChainOf(plugin)->engine_control(plugin, 0, control, &r.base, &r.resolved));
	return r;
}

double knobOf(const clap_plugin* plugin, const char* control) {
	double v = -1.0;
	REQUIRE(testChainOf(plugin)->knob_value(plugin, 0, control, &v));
	return v;
}

uint32_t slotStateOf(const clap_plugin* plugin, uint32_t n) { return testChainOf(plugin)->slot_state(plugin, n); }

/// WHAT A HOST WRITE OF `normal` IS IN THE CONTROL'S UNITS, stored as the engine stores a base: the declared
/// range (`fromNormal`), then the float the row holds, coerced by the declaration.
double hostValueOf(const feathers::device::ParamDesc& desc, double normal) {
	return static_cast<double>(
	    feathers::device::coerceControl(desc, static_cast<float>(feathers::device::fromNormal(desc, normal))));
}

/// Slot 0 on the default filter's cutoff, bound: pinned, then the main-thread visit that publishes the
/// binding, then a block so the engine has adopted it.
void pinCutoff(HostedPlugin& h) {
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	REQUIRE(chain->device_count(h.plugin()) == 1);
	REQUIRE(chain->pin_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	mainThreadVisit(h.plugin());
	EventList none;
	h.run(kBlock, none);
}

/// One block carrying one host write of `normal` to slot 0.
void hostWrites(HostedPlugin& h, double normal) {
	EventList events;
	events.pushParam(0, kSlot0, normal);
	h.run(kBlock, events);
}

void drag(const clap_plugin* plugin, const char* control, std::initializer_list<double> values) {
	const std::vector<double> v(values);
	REQUIRE(testChainOf(plugin)->drag_control(plugin, 0, control, v.data(), static_cast<uint32_t>(v.size())));
}

} // namespace

TEST_CASE("host PARAM_VALUE on a slot moves the device's resolved value in that block") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	const double expected = hostValueOf(desc, 0.3);
	// NOT VACUOUS: the control does not already stand there.
	REQUIRE(engineOf(h.plugin(), "Filter Cutoff").base != expected);

	hostWrites(h, 0.3);
	// IN THAT BLOCK, with no main-thread visit in between: the engine read the bus itself.
	const EngineReading r = engineOf(h.plugin(), "Filter Cutoff");
	CHECK(r.base == expected);
	CHECK(r.resolved == expected);
	// AND THE AUDIO THREAD ALLOCATED NOTHING TO DO IT.
	CHECK(h.allocationsInLastProcess() == 0);

	// A SECOND WRITE LATER IN A BLOCK lands too — the bus is read at every block's top, so it is there by the
	// next one.
	EventList later;
	later.pushParam(100, kSlot0, 0.8);
	h.run(kBlock, later);
	EventList none;
	h.run(kBlock, none);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == hostValueOf(desc, 0.8));
}

TEST_CASE("a host write anchors the slot at the next main-thread visit") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	// A TOUCH (Recent), the state a host write has to protect.
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	mainThreadVisit(h.plugin());
	REQUIRE(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Recent));

	hostWrites(h, 0.62);
	// NOT BEFORE THE VISIT: the audio thread only sets a bit.
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Recent));
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
	// AND THE VISIT MIRRORED THE VALUE: the knob reads what the host wrote, and so does the host.
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == hostValueOf(desc, 0.62));
	CHECK(slotValue(h.plugin(), 0) == doctest::Approx(feathers::device::toNormal(desc, hostValueOf(desc, 0.62))).epsilon(1e-9));
	// A SLOT NOBODY WROTE IS NOT ANCHORED BY ITS NEIGHBOUR'S WRITE.
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Res") == 1u);
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 1) == stateNumber(feathers::hostslots::SlotState::Recent));
}

TEST_CASE("stateSave folds an unserviced host write into an anchor") {
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	StateBytes saved;
	{
		HostedPlugin h;
		REQUIRE(testChainOf(h.plugin())->touch_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
		mainThreadVisit(h.plugin());
		hostWrites(h, 0.41);
		// NO VISIT: the write is still only a bit on the bus when the host saves.
		REQUIRE(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Recent));
		REQUIRE(saveState(h.plugin(), &saved));
		CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
	}
	// THE SAVED PROJECT, reopened with no host event: anchored, and at the host's value.
	HostedPlugin reopened;
	REQUIRE(loadState(reopened.plugin(), &saved));
	CHECK(slotStateOf(reopened.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
	CHECK(knobOf(reopened.plugin(), "Filter Cutoff") == hostValueOf(desc, 0.41));
}

TEST_CASE("a knob drag on a slotted control emits Begin, Value…, End on the slot id") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	const double a = std::log2(300.0);
	const double b = std::log2(900.0);
	const double c = std::log2(4000.0);
	drag(h.plugin(), "Filter Cutoff", {a, b, c});

	OutputLog log;
	EventList none;
	h.run(kBlock, none, &log);
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 5);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[4].type == CLAP_EVENT_PARAM_GESTURE_END);
	const double values[3] = {a, b, c};
	for (size_t i = 0; i < 3; ++i) {
		CHECK(ev[i + 1].type == CLAP_EVENT_PARAM_VALUE);
		// THE STORED VALUE'S POSITION, restated from the declared range and sent as the float the ring holds.
		const double stored = static_cast<double>(feathers::device::coerceControl(desc, static_cast<float>(values[i])));
		CHECK(ev[i + 1].value == static_cast<double>(static_cast<float>(feathers::device::toNormal(desc, stored))));
	}
	// NOTHING ELSE ON ANY SLOT, and nothing on the slot at the next block: the ring was drained once.
	CHECK(log.events.size() == 5);
	OutputLog next;
	h.run(kBlock, none, &next);
	CHECK(next.on(kSlot0).empty());
	// AND THE HAND'S LAST VALUE IS WHAT THE ENGINE RUNS.
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == static_cast<double>(static_cast<float>(c)));

	// A CONTROL WITH NO SLOT is given one by the hand, and bound at its Begin (a first grab is serviced there,
	// `ParamBridge::beginControlGesture`): the very next block carries its drag on the NEW slot, never on this one.
	// "a drag on a freshly slotted control reaches the host on that slot" has the whole stream.
	drag(h.plugin(), "Filter Res", {0.1, 0.7});
	OutputLog unslotted;
	h.run(kBlock, none, &unslotted);
	CHECK(unslotted.on(kSlot0).empty());
	const std::vector<OutputLog::Event> fresh = unslotted.on(kSlot0 + 1);
	REQUIRE(fresh.size() >= 2);
	CHECK(fresh[0].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(fresh[1].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(fresh.back().type == CLAP_EVENT_PARAM_GESTURE_END);
}

TEST_CASE("a drag while IS_RECORDING anchors; without it, it does not") {
	HostedPlugin h;
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Res") == 1u);
	mainThreadVisit(h.plugin());
	EventList none;

	// PLAYING, NOT RECORDING.
	h.setTransportFlags(CLAP_TRANSPORT_IS_PLAYING);
	h.run(kBlock, none);
	drag(h.plugin(), "Filter Cutoff", {std::log2(500.0), std::log2(700.0)});
	h.run(kBlock, none);
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Recent));

	// RECORDING.
	h.setTransportFlags(CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_IS_RECORDING);
	h.run(kBlock, none);
	drag(h.plugin(), "Filter Res", {0.3, 0.4});
	CHECK(slotStateOf(h.plugin(), 1) == stateNumber(feathers::hostslots::SlotState::Recent)); // not until the visit
	h.run(kBlock, none);
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 1) == stateNumber(feathers::hostslots::SlotState::Anchored));
	// THE OTHER SLOT, dragged before the host recorded, is still not anchored.
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Recent));

	// AND THE RECORDING STOPS: a later drag anchors nothing new.
	REQUIRE(chain->touch_slot(h.plugin(), 0, "Filter Drive") == 2u);
	mainThreadVisit(h.plugin());
	h.setTransportFlags(CLAP_TRANSPORT_IS_PLAYING);
	h.run(kBlock, none);
	drag(h.plugin(), "Filter Drive", {0.2, 0.5});
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 2) == stateNumber(feathers::hostslots::SlotState::Recent));
}

// ---------------------------------------------------------------------------------------------------
// HOST SLOTS T9 — THE RECENT RING. A hand on an unslotted control gives it a slot; nothing else does.
// ---------------------------------------------------------------------------------------------------
namespace {

/// Every slot of the pool Empty — what "nothing allocated" is, read off the pool itself.
bool poolAllEmpty(const clap_plugin* plugin) {
	for (uint32_t n = 0; n < feathers::hostslots::kHostSlotCapacity; ++n) {
		if (slotStateOf(plugin, n) != stateNumber(feathers::hostslots::SlotState::Empty)) {
			return false;
		}
	}
	return true;
}

/// The normalised float a hand's value of `v` reaches the host as: coerced by the declaration, placed in its
/// range, and carried as the float the outbox holds.
double sentAs(const feathers::device::ParamDesc& desc, double v) {
	const double stored = static_cast<double>(feathers::device::coerceControl(desc, static_cast<float>(v)));
	return static_cast<double>(static_cast<float>(feathers::device::toNormal(desc, stored)));
}

} // namespace

TEST_CASE("touching an unslotted control gives it a Recent slot and the host hears its name") {
	RescanHost log;
	HostedPlugin h(&log.host);
	REQUIRE(poolAllEmpty(h.plugin()));
	const size_t before = log.rescans.size();

	// A HAND ON THE KNOB (`DeviceControl::beginGesture`, as a widget drives it) — the first Begin takes slot 0.
	drag(h.plugin(), "Filter Res", {0.3});
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Recent));
	CHECK(slotStateOf(h.plugin(), 1) == stateNumber(feathers::hostslots::SlotState::Empty));
	// THE NAME IS SENT BY THE GRAB ITSELF (a first grab is serviced at its Begin, 2026-11-09), so the host knows the
	// slot before the Begin reaches it; the next visit owes nothing more.
	CHECK(log.infoRescansSince(before) == 1);
	CHECK(log.rescans.size() == before + 1);
	mainThreadVisit(h.plugin());
	CHECK(log.rescans.size() == before + 1);
	const clap_param_info info = slotInfo(h.plugin(), 0);
	CHECK(std::string(info.name) == "Filter " + kDot + " Res");
	CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) == 0);

	// A SECOND HAND ON THE SAME CONTROL allocates nothing and renames nothing: the slot is its own.
	drag(h.plugin(), "Filter Res", {0.4});
	mainThreadVisit(h.plugin());
	CHECK(log.rescans.size() == before + 1);
	CHECK(slotStateOf(h.plugin(), 1) == stateNumber(feathers::hostslots::SlotState::Empty));
}

TEST_CASE("a drag on a freshly slotted control reaches the host on that slot") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	const double a = std::log2(300.0);
	const double b = std::log2(900.0);
	const double c = std::log2(4000.0);
	EventList none;

	// THE FIRST DRAG: begin, three values, end. THE BEGIN BINDS THE SLOT (a first grab is serviced there, 2026-11-09;
	// it was the next visit's, and the intermediate values a and b were lost to the host): the takeover as a plain
	// Value at the control's value BEFORE the hand, then the hand's gesture from there — Begin, that value again,
	// every value the hand made, End — all in the very next block.
	const double before = knobOf(h.plugin(), "Filter Cutoff"); // the record's value, which the takeover reports
	drag(h.plugin(), "Filter Cutoff", {a, b, c});
	OutputLog log;
	h.run(kBlock, none, &log);
	// The engine runs the hand's value: it reads the record.
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == static_cast<double>(static_cast<float>(c)));
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 7);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[0].value == sentAs(desc, before));
	CHECK(ev[1].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[2].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[2].value == sentAs(desc, before));
	CHECK(ev[3].value == sentAs(desc, a));
	CHECK(ev[4].value == sentAs(desc, b));
	CHECK(ev[5].value == sentAs(desc, c));
	CHECK(ev[6].type == CLAP_EVENT_PARAM_GESTURE_END);
	CHECK(log.events.size() == 7);
	// AND THE NEXT VISIT ADDS NOTHING: the takeover was reported once, at the grab.
	mainThreadVisit(h.plugin());
	OutputLog after;
	h.run(kBlock, none, &after);
	CHECK(after.on(kSlot0).empty());

	// THE NEXT DRAG IS AN ORDINARY SLOTTED ONE: Begin, every Value, End — T6b's road, on the same slot.
	drag(h.plugin(), "Filter Cutoff", {a, b});
	OutputLog second;
	h.run(kBlock, none, &second);
	const std::vector<OutputLog::Event> ev2 = second.on(kSlot0);
	REQUIRE(ev2.size() == 4);
	CHECK(ev2[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev2[1].value == sentAs(desc, a));
	CHECK(ev2[2].value == sentAs(desc, b));
	CHECK(ev2[3].type == CLAP_EVENT_PARAM_GESTURE_END);
}

TEST_CASE("backlog NG, Feathers at rest: no block and no main-thread visit tells the host anything — no event, no rescan") {
	// THE BEHAVING PLUGIN'S HALF of the rack's case of the same name (test_rack_clap): Bitwig ranks its
	// recently-used list from these events, so a plugin at rest must send none, or what it names rises untouched.
	RescanHost host;
	HostedPlugin h(&host.host);
	EventList none;
	h.run(kBlock, none);
	mainThreadVisit(h.plugin());
	const size_t rescansBefore = host.rescans.size();
	for (int i = 0; i < 16; ++i) {
		CAPTURE(i);
		OutputLog log;
		h.run(kBlock, none, &log);
		mainThreadVisit(h.plugin());
		CHECK(log.events.empty());
	}
	CHECK(host.rescans.size() == rescansBefore);
	// NOT VACUOUS: a hand on a knob in the same venue is heard — its name at the grab, its touch at the next block
	// (the takeover's Value, then Begin, the value again, the hand's one value, End).
	drag(h.plugin(), "Filter Cutoff", {std::log2(500.0)});
	CHECK(host.infoRescansSince(rescansBefore) == 1);
	OutputLog moved;
	h.run(kBlock, none, &moved);
	CHECK(moved.on(kSlot0).size() == 5);
}

TEST_CASE("a first drag on an unslotted control while IS_RECORDING anchors its new slot") {
	// THE BEGIN THAT OPENS AT THE BINDING is a Begin while recording (T6b's rule), so the slot is anchored — at the
	// visit AFTER the binding, because each visit folds the bits before it rebinds. The binding is the grab's own
	// (a first grab is serviced at its Begin, 2026-11-09), so the first visit after the drag is that visit.
	HostedPlugin h;
	EventList none;
	h.setTransportFlags(CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_IS_RECORDING);
	h.run(kBlock, none);
	drag(h.plugin(), "Filter Res", {0.3, 0.6});
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Recent));
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
}

TEST_CASE("a host write or a load never allocates a slot") {
	HostedPlugin h;
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	REQUIRE(poolAllEmpty(h.plugin()));
	// A WRITE THAT IS NOT A HAND (the settings road's `set_control`): no touch.
	REQUIRE(chain->set_control(h.plugin(), 0, "Filter Res", 0.5));
	// A HOST WRITE ON AN EMPTY SLOT'S ID: dropped, nothing allocated.
	hostWrites(h, 0.3);
	mainThreadVisit(h.plugin());
	CHECK(poolAllEmpty(h.plugin()));

	// A PRESET LOAD AND A PROJECT LOAD, of a patch whose chain holds the same filter: nothing allocated.
	StateBytes preset;
	{
		clap_ostream out = preset.out();
		REQUIRE(chain->capture_preset(h.plugin(), &out));
	}
	{
		clap_istream in = preset.in();
		REQUIRE(chain->load_preset(h.plugin(), &in));
	}
	mainThreadVisit(h.plugin());
	CHECK(poolAllEmpty(h.plugin()));
	StateBytes project;
	REQUIRE(saveState(h.plugin(), &project));
	HostedPlugin reopened;
	REQUIRE(loadState(reopened.plugin(), &project));
	mainThreadVisit(reopened.plugin());
	CHECK(poolAllEmpty(reopened.plugin()));

	// NOT VACUOUS: a hand on the same control DOES allocate — and NOT slot 0, whose id the host wrote above:
	// the host mark (2026-09-26) puts it after every clean slot, so the hand gets slot 1.
	drag(h.plugin(), "Filter Res", {0.2});
	CHECK(slotStateOf(h.plugin(), 1) == stateNumber(feathers::hostslots::SlotState::Recent));
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Empty));
}

TEST_CASE("a host mod on a Pinned slot anchors it") {
	// A host modulator stays on the slot's id whatever the plugin does, so a non-zero
	// modulation anchors exactly as a host write does. A zero amount (the modulator at rest) does not.
	HostedPlugin h;
	pinCutoff(h);
	REQUIRE(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Pinned));
	EventList rest;
	rest.pushParamMod(0, kSlot0, 0.0);
	h.run(kBlock, rest);
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Pinned));

	EventList mod;
	mod.pushParamMod(0, kSlot0, 0.1);
	h.run(kBlock, mod);
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Pinned)); // not until the visit
	mainThreadVisit(h.plugin());
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
}

TEST_CASE("global PARAM_MOD offsets the control; the base is unchanged") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	const double base = static_cast<double>(static_cast<float>(std::log2(1000.0)));
	REQUIRE(testChainOf(h.plugin())->set_control(h.plugin(), 0, "Filter Cutoff", std::log2(1000.0)));
	EventList none;
	h.run(kBlock, none);
	REQUIRE(engineOf(h.plugin(), "Filter Cutoff").resolved == base);

	EventList mod;
	mod.pushParamMod(0, kSlot0, 0.1);
	h.run(kBlock, mod);
	const EngineReading r = engineOf(h.plugin(), "Filter Cutoff");
	CHECK(r.base == base);
	// base + amount × the control's route span, through the shared rule — restated here from the declaration.
	const double expected = static_cast<double>(static_cast<float>(
	    feathers::device::withRouteOffset(desc, base, 0.1 * feathers::device::routeSpanOf(desc))));
	CHECK(r.resolved == expected);
	CHECK(r.resolved != base);
	// THE KNOB'S VALUE IS ITS BASE: modulation writes no value. (It DOES anchor since 2026-09-26 — its own case,
	// "a host mod on a Pinned slot anchors it".)
	mainThreadVisit(h.plugin());
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == base);

	// AND A MODULATION OF ZERO TAKES THE OFFSET AWAY: it is the host's current amount, not a delta.
	EventList back;
	back.pushParamMod(0, kSlot0, 0.0);
	h.run(kBlock, back);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").resolved == base);
}

TEST_CASE("a host slot write while asleep reaches the control through flush") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	EventList none;
	// ASLEEP: nothing sounds, so the plugin said SLEEP and the host moves params through flush from here.
	REQUIRE(h.run(kBlock, none) == CLAP_PROCESS_SLEEP);

	// A HAND ON THE KNOB while asleep, and a host write, both in one flush.
	drag(h.plugin(), "Filter Cutoff", {std::log2(2000.0)});
	EventList events;
	events.pushParam(0, kSlot0, 0.25);
	OutputLog log;
	{
		const clap_input_events in = events.input();
		const clap_output_events out = log.out();
		paramsOf(h.plugin())->flush(h.plugin(), &in, &out);
	}
	// THE HAND REACHED THE HOST THROUGH THE FLUSH: Begin, Value, End on the slot.
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);

	// THE HOST'S WRITE REACHED THE CONTROL: the knob and the host read it after the visit, the slot is anchored…
	mainThreadVisit(h.plugin());
	const double expected = hostValueOf(desc, 0.25);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == expected);
	CHECK(slotStateOf(h.plugin(), 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
	// …and the engine runs it at its next block, with no process() having carried the event.
	h.run(kBlock, none);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == expected);
}

TEST_CASE("a hand edit on a bound control is not reverted by the next block") {
	HostedPlugin h;
	pinCutoff(h);
	// THE BUS HOLDS A HOST VALUE, mirrored into the record.
	hostWrites(h, 0.2);
	mainThreadVisit(h.plugin());
	// THEN A HAND.
	const double hand = std::log2(5000.0);
	REQUIRE(testChainOf(h.plugin())->set_control(h.plugin(), 0, "Filter Cutoff", hand));
	EventList none;
	h.run(kBlock, none);
	h.run(kBlock, none);
	const double expected = static_cast<double>(static_cast<float>(hand));
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == expected);
	// AND A VISIT DOES NOT MIRROR THE HOST'S OLDER VALUE BACK OVER IT.
	mainThreadVisit(h.plugin());
	h.run(kBlock, none);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == expected);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == expected);
}

TEST_CASE("a patch load on a bound control is not reverted") {
	const double loaded = static_cast<double>(static_cast<float>(std::log2(350.0)));
	StateBytes patch;
	{
		HostedPlugin writer;
		REQUIRE(testChainOf(writer.plugin())->set_control(writer.plugin(), 0, "Filter Cutoff", std::log2(350.0)));
		REQUIRE(testChainOf(writer.plugin())->pin_slot(writer.plugin(), 0, "Filter Cutoff") == 0u);
		REQUIRE(saveState(writer.plugin(), &patch));
	}
	HostedPlugin h;
	pinCutoff(h);
	hostWrites(h, 0.9);
	mainThreadVisit(h.plugin());
	REQUIRE(engineOf(h.plugin(), "Filter Cutoff").base != loaded);

	REQUIRE(loadState(h.plugin(), &patch));
	EventList none;
	h.run(kBlock, none);
	h.run(kBlock, none);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == loaded);
	mainThreadVisit(h.plugin());
	h.run(kBlock, none);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == loaded);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == loaded);
}

TEST_CASE("undo on a bound control is not reverted") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	const double before = knobOf(h.plugin(), "Filter Cutoff");
	drag(h.plugin(), "Filter Cutoff", {std::log2(6000.0), std::log2(8000.0)});
	EventList none;
	h.run(kBlock, none);
	REQUIRE(engineOf(h.plugin(), "Filter Cutoff").base == static_cast<double>(static_cast<float>(std::log2(8000.0))));

	REQUIRE(testChainOf(h.plugin())->undo(h.plugin()));
	OutputLog log;
	h.run(kBlock, none, &log);
	h.run(kBlock, none);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == before);
	mainThreadVisit(h.plugin());
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == before);
	// AND THE HOST HEARS THE UNDONE VALUE on the slot, INSIDE ITS OWN GESTURE — an undo is an explicit act,
	// a hand's in all but the pointer (integration review; it used to be a bare Value outside any gesture).
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[1].value == static_cast<double>(static_cast<float>(feathers::device::toNormal(desc, before))));
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
}

TEST_CASE("undo back to a value wins over an unmirrored host write") {
	// THE RECORD HOLDS THE UNDO'S TARGET and the host has moved the bus since: the one case the no-hand rule
	// (`SlotOwner::onHandWrite`) would skip. An undo is explicit, so it must win, inside a gesture.
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	const double r = static_cast<double>(static_cast<float>(std::log2(2000.0)));
	REQUIRE(testChainOf(h.plugin())->set_control(h.plugin(), 0, "Filter Cutoff", r));
	drag(h.plugin(), "Filter Cutoff", {std::log2(5000.0)}); // the undo step: r -> 5000
	REQUIRE(testChainOf(h.plugin())->set_control(h.plugin(), 0, "Filter Cutoff", r)); // the record back at r
	EventList none;
	h.run(kBlock, none);
	mainThreadVisit(h.plugin());
	REQUIRE(knobOf(h.plugin(), "Filter Cutoff") == r);

	hostWrites(h, 0.9); // unmirrored
	const double host = hostValueOf(desc, 0.9);
	REQUIRE(host != r);
	REQUIRE(engineOf(h.plugin(), "Filter Cutoff").base == host);

	REQUIRE(testChainOf(h.plugin())->undo(h.plugin())); // writes r, which the record already holds
	OutputLog log;
	h.run(kBlock, none, &log);
	h.run(kBlock, none);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == r);
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[1].value == static_cast<double>(static_cast<float>(feathers::device::toNormal(desc, r))));
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	mainThreadVisit(h.plugin());
	h.run(kBlock, none);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == r);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == r);
}

TEST_CASE("a hand dragging to the record's value over an unmirrored host write wins") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	const double recorded = knobOf(h.plugin(), "Filter Cutoff");
	hostWrites(h, 0.85);
	REQUIRE(engineOf(h.plugin(), "Filter Cutoff").base == hostValueOf(desc, 0.85));

	drag(h.plugin(), "Filter Cutoff", {recorded});
	OutputLog log;
	EventList none;
	h.run(kBlock, none, &log);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == recorded);
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	mainThreadVisit(h.plugin());
	h.run(kBlock, none);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == recorded);
}

TEST_CASE("a write of the record's value with no hand leaves an unmirrored host write to the mirror") {
	// THE NO-HAND RULE (`SlotOwner::onHandWrite`): a row writer carrying the control at the record's value.
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	const double recorded = knobOf(h.plugin(), "Filter Cutoff");
	hostWrites(h, 0.85);
	const double host = hostValueOf(desc, 0.85);
	REQUIRE(host != recorded);

	REQUIRE(testChainOf(h.plugin())->set_control(h.plugin(), 0, "Filter Cutoff", recorded));
	OutputLog log;
	EventList none;
	h.run(kBlock, none, &log);
	h.run(kBlock, none);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == host);
	CHECK(log.on(kSlot0).empty());
	mainThreadVisit(h.plugin());
	h.run(kBlock, none);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == host);
}

// ---------------------------------------------------------------------------------------------------
// T6b REVIEW — a road that rewrites a whole row (or a whole chain) must not put the record's copy of a
// bound control over a host write the mirror has not copied yet.
// ---------------------------------------------------------------------------------------------------

TEST_CASE("a settings-road rewrite of the row keeps an unmirrored host write on another bound control") {
	// WHAT THE SETTINGS ROAD WILL WRITE: `setFilterSettings` of a default filter state, whose cutoff is read
	// here off a throwaway instance rather than restated, so the case sets the record's cutoff to exactly it —
	// the rewrite then CARRIES the cutoff unchanged and CHANGES the resonance.
	double carriedCutoff = 0.0;
	double writtenRes = 0.0;
	{
		HostedPlugin probe;
		REQUIRE(testChainOf(probe.plugin())->replace_with_filter(probe.plugin(), 0));
		carriedCutoff = knobOf(probe.plugin(), "Filter Cutoff");
		writtenRes = knobOf(probe.plugin(), "Filter Res");
	}
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
	REQUIRE(chain->set_control(h.plugin(), 0, "Filter Cutoff", carriedCutoff));
	REQUIRE(chain->set_control(h.plugin(), 0, "Filter Res", writtenRes == 0.7 ? 0.3 : 0.7));
	REQUIRE(chain->pin_slot(h.plugin(), 0, "Filter Cutoff") == 0u);
	REQUIRE(chain->pin_slot(h.plugin(), 0, "Filter Res") == 1u);
	mainThreadVisit(h.plugin());
	EventList none;
	h.run(kBlock, none);

	// THE HOST WRITES THE CUTOFF, and no visit mirrors it.
	hostWrites(h, 0.85);
	const double hostCutoff = hostValueOf(desc, 0.85);
	REQUIRE(hostCutoff != carriedCutoff);
	REQUIRE(engineOf(h.plugin(), "Filter Cutoff").base == hostCutoff);

	// A SETTINGS-ROAD REWRITE OF THE WHOLE ROW (`storeRowOf`), carrying the record's older cutoff.
	REQUIRE(chain->replace_with_filter(h.plugin(), 0));
	h.run(kBlock, none);
	h.run(kBlock, none);
	// THE HOST'S CUTOFF STANDS, and the resonance the row did change took.
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == hostCutoff);
	CHECK(engineOf(h.plugin(), "Filter Res").base == writtenRes);
	// AND THE VISIT MIRRORS THE HOST'S VALUE INTO THE KNOB, rather than the other way round.
	mainThreadVisit(h.plugin());
	h.run(kBlock, none);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == hostCutoff);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == hostCutoff);
}

TEST_CASE("a chain rebuild keeps an unmirrored host write on a bound control") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	hostWrites(h, 0.15);
	const double hostCutoff = hostValueOf(desc, 0.15);
	REQUIRE(knobOf(h.plugin(), "Filter Cutoff") != hostCutoff); // not mirrored yet

	// ADDING A DEVICE REBUILDS THE LANE (`rebuildChain`), carrying the filter's row as the record has it.
	const std::string filterTag = feathers::fx::filter::kTraits.tag;
	char tag[64] {};
	std::string otherTag;
	for (uint32_t i = 0; testChainOf(h.plugin())->available_device(h.plugin(), i, tag, sizeof(tag)); ++i) {
		if (filterTag != tag) {
			otherTag = tag;
			break;
		}
	}
	REQUIRE_FALSE(otherTag.empty());
	REQUIRE(testChainOf(h.plugin())->add_device(h.plugin(), otherTag.c_str()) == 1u);
	EventList none;
	h.run(kBlock, none);
	h.run(kBlock, none);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == hostCutoff);
	mainThreadVisit(h.plugin());
	h.run(kBlock, none);
	CHECK(knobOf(h.plugin(), "Filter Cutoff") == hostCutoff);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == hostCutoff);
}

TEST_CASE("a double-click reset on a slotted control emits Begin, Value, End") {
	HostedPlugin h;
	const feathers::device::ParamDesc& desc = filterParam("filter_cutoff_wide");
	pinCutoff(h);
	REQUIRE(testChainOf(h.plugin())->set_control(h.plugin(), 0, "Filter Cutoff", std::log2(3000.0)));
	EventList none;
	h.run(kBlock, none); // the set_control's bare Value goes out here, and is not what this case is about

	REQUIRE(testChainOf(h.plugin())->reset_control(h.plugin(), 0, "Filter Cutoff"));
	OutputLog log;
	h.run(kBlock, none, &log);
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[1].value == static_cast<double>(static_cast<float>(
	                         feathers::device::toNormal(desc, feathers::device::coerceControl(desc, static_cast<float>(desc.def))))));
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	CHECK(engineOf(h.plugin(), "Filter Cutoff").base == static_cast<double>(static_cast<float>(desc.def)));
}

TEST_CASE("a control destroyed mid-drag still ends its host gesture") {
	HostedPlugin h;
	pinCutoff(h);
	REQUIRE(testChainOf(h.plugin())->abandon_drag(h.plugin(), 0, "Filter Cutoff", std::log2(750.0)));
	OutputLog log;
	EventList none;
	h.run(kBlock, none, &log);
	const std::vector<OutputLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	// AND THE NEXT DRAG IS ITS OWN GESTURE, not one the dead control left open.
	drag(h.plugin(), "Filter Cutoff", {std::log2(900.0)});
	OutputLog next;
	h.run(kBlock, none, &next);
	const std::vector<OutputLog::Event> ev2 = next.on(kSlot0);
	REQUIRE(ev2.size() == 3);
	CHECK(ev2[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev2[2].type == CLAP_EVENT_PARAM_GESTURE_END);
}

TEST_CASE("a write of the value already there tells the host nothing") {
	// A ROW WRITER PUTS EVERY FIELD; the ones it did not change must not reach the host outside a gesture.
	HostedPlugin h;
	pinCutoff(h);
	const double now = knobOf(h.plugin(), "Filter Cutoff");
	REQUIRE(testChainOf(h.plugin())->set_control(h.plugin(), 0, "Filter Cutoff", now));
	OutputLog log;
	EventList none;
	h.run(kBlock, none, &log);
	CHECK(log.on(kSlot0).empty());
}

TEST_CASE("this binary's aligned new answers an alignment below a pointer's, as the rack asks for a float-only device") {
	// macOS CI run 37412368692 aborted the case below: the rack allocates a device's state with
	// `new (std::align_val_t { stateAlign }) std::byte[bytes]`, a float-only state (Stereo's, among three) asks for 4,
	// and this file's replacement handed the 4 to an `aligned_alloc` that refuses it there. The request
	// here is that one's shape (36 bytes at 4); the answer must be storage, aligned at least as asked.
	void* p = nullptr;
	CHECK_NOTHROW(p = ::operator new[](36, std::align_val_t { 4 }));
	REQUIRE(p != nullptr);
	CHECK(reinterpret_cast<std::uintptr_t>(p) % 4 == 0);
	::operator delete[](p, std::align_val_t { 4 });
	// AND THE STORAGE BEHIND IT REFUSES WHAT macOS REFUSES, on this platform too — the half that makes
	// the line above able to go red where we build (portable_aligned_alloc.h).
	CHECK(feathers::test::strictAlignedAlloc(4, 36) == nullptr);
	CHECK(feathers::test::strictAlignedAlloc(8, 36) == nullptr);
	void* q = feathers::test::strictAlignedAlloc(8, 40);
	CHECK(q != nullptr);
	feathers::test::heapFree(q);
}

TEST_CASE("KO: editing one control of a lane device never puts another of its controls back, on any device") {
	// THE SYMPTOM: device controls randomly snapped back to their defaults after another control on the
	// same device changed — on devices, not on the synth's own params. Every hand edit here is
	// the widget's own sequence (`drag_control`: begin, value, end), so each one also takes a Recent host slot
	// (T9) and the ring recycles as the controls outnumber it — the road a device's controls have and the
	// synth's do not. Every control is set away from its default, one at a time with a main-thread visit and a
	// block between, and then EVERY control is read back where it lives: what the knob reads and the engine's
	// base. The expected number is the declaration's (`fromNormal`, `coerceControl`), never read back.
	std::vector<std::string> tags;
	{
		HostedPlugin lister;
		const clap_plugin_feathers_test_chain* probe = testChainOf(lister.plugin());
		char tag[64] {};
		for (uint32_t i = 0; probe->available_device(lister.plugin(), i, tag, sizeof(tag)); ++i) {
			tags.emplace_back(tag);
		}
	}
	REQUIRE_FALSE(tags.empty());
	EventList none;
	for (const std::string& tag : tags) {
		CAPTURE(tag);
		const feathers::device::Descriptor* desc = feathers::deviceForTag(tag.c_str());
		REQUIRE(desc != nullptr);
		HostedPlugin h;
		const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
		const uint32_t at = chain->add_device(h.plugin(), tag.c_str());
		REQUIRE(at != UINT32_MAX);
		// ACTIVATE PROVISIONS THE LANE IT IS HANDED (the road a loaded patch takes), so the engine holds it.
		const clap_plugin* p = h.plugin();
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));

		// THE CONTROLS A HAND CAN NAME UNAMBIGUOUSLY, each with a value its default is not.
		struct Want {
			const char* name;
			const feathers::device::ParamDesc* desc;
			double value;
		};
		std::vector<Want> wants;
		for (const feathers::device::ParamDesc& p : desc->params) {
			if (p.name == nullptr) {
				continue;
			}
			size_t same = 0;
			for (const feathers::device::ParamDesc& q : desc->params) {
				same += q.name != nullptr && std::strcmp(q.name, p.name) == 0 ? 1 : 0;
			}
			if (same != 1) {
				continue;
			}
			const double defNormal = feathers::device::toNormal(p, p.def);
			const double normal = defNormal < 0.5 ? 0.7 : 0.3;
			const double value = hostValueOf(p, normal);
			if (value == static_cast<double>(feathers::device::coerceControl(p, static_cast<float>(p.def)))) {
				continue;
			}
			wants.push_back({ p.name, &p, value });
		}
		for (const Want& w : wants) {
			const std::string name = w.name;
			CAPTURE(name);
			REQUIRE(chain->drag_control(h.plugin(), at, w.name, &w.value, 1));
			mainThreadVisit(h.plugin());
			h.run(kBlock, none);
		}
		h.run(kBlock, none);
		mainThreadVisit(h.plugin());
		h.run(kBlock, none);
		for (const Want& w : wants) {
			const std::string name = w.name;
			CAPTURE(name);
			double knob = -1.0;
			REQUIRE(chain->knob_value(h.plugin(), at, w.name, &knob));
			CHECK(knob == w.value);
			double base = -1.0;
			REQUIRE(chain->engine_control(h.plugin(), at, w.name, &base, nullptr));
			CHECK(base == w.value);
		}
	}
}

TEST_CASE("KO: in a FULL lane, edits taken in turn across the devices never put another device's control back") {
	// THE CROWDED HALF OF THE CASE ABOVE: eight devices in one lane (the default filter, then the registry's
	// devices in turn, each TWICE where it fits, so two of a kind sit side by side), and the edits interleaved
	// round-robin — one control of each device, then the next control of each — so every edit lands between
	// edits of OTHER devices and the Recent ring holds many devices' controls at once.
	std::vector<std::string> tags;
	{
		HostedPlugin lister;
		const clap_plugin_feathers_test_chain* probe = testChainOf(lister.plugin());
		char tag[64] {};
		for (uint32_t i = 0; probe->available_device(lister.plugin(), i, tag, sizeof(tag)); ++i) {
			tags.emplace_back(tag);
			tags.emplace_back(tag);
		}
	}
	EventList none;
	size_t checked = 0;
	for (size_t first = 0; first < tags.size(); first += feathers::kMaxFxDevices - 1) {
		HostedPlugin h;
		const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
		struct Want {
			uint32_t at;
			std::string name;
			double value;
		};
		std::vector<std::vector<Want>> perDevice;
		for (size_t t = first; t < tags.size() && t < first + feathers::kMaxFxDevices - 1; ++t) {
			const uint32_t at = chain->add_device(h.plugin(), tags[t].c_str());
			if (at == UINT32_MAX) {
				continue;
			}
			const feathers::device::Descriptor* desc = feathers::deviceForTag(tags[t].c_str());
			REQUIRE(desc != nullptr);
			std::vector<Want> wants;
			for (const feathers::device::ParamDesc& p : desc->params) {
				size_t same = 0;
				for (const feathers::device::ParamDesc& q : desc->params) {
					same += p.name != nullptr && q.name != nullptr && std::strcmp(q.name, p.name) == 0 ? 1 : 0;
				}
				if (same != 1) {
					continue;
				}
				// THE SECOND OF A KIND TAKES THE OTHER SIDE, so the two cannot agree by accident.
				const bool twin = t > first && tags[t] == tags[t - 1];
				const double normal = (feathers::device::toNormal(p, p.def) < 0.5) != twin ? 0.7 : 0.3;
				const double value = hostValueOf(p, normal);
				if (value == static_cast<double>(feathers::device::coerceControl(p, static_cast<float>(p.def)))) {
					continue;
				}
				wants.push_back({ at, p.name, value });
			}
			perDevice.push_back(std::move(wants));
		}
		const clap_plugin* p = h.plugin();
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));
		size_t longest = 0;
		for (const std::vector<Want>& w : perDevice) {
			longest = std::max(longest, w.size());
		}
		for (size_t i = 0; i < longest; ++i) {
			for (const std::vector<Want>& wants : perDevice) {
				if (i >= wants.size()) {
					continue;
				}
				const Want& w = wants[i];
				REQUIRE(chain->drag_control(h.plugin(), w.at, w.name.c_str(), &w.value, 1));
				h.run(kBlock, none);
			}
			mainThreadVisit(h.plugin());
			h.run(kBlock, none);
		}
		mainThreadVisit(h.plugin());
		h.run(kBlock, none);
		h.run(kBlock, none);
		for (const std::vector<Want>& wants : perDevice) {
			for (const Want& w : wants) {
				CAPTURE(w.at);
				CAPTURE(w.name);
				double knob = -1.0;
				REQUIRE(chain->knob_value(h.plugin(), w.at, w.name.c_str(), &knob));
				CHECK(knob == w.value);
				double base = -1.0;
				REQUIRE(chain->engine_control(h.plugin(), w.at, w.name.c_str(), &base, nullptr));
				CHECK(base == w.value);
				++checked;
			}
		}
	}
	MESSAGE("controls checked: " << checked);
	CHECK(checked > 200);
}

TEST_CASE("the tail extension: infinite while the fx lane holds an audio device") {
	// clap-wrapper turns `clap_plugin_tail` into VST3's getTailSamples, and a VST3 host told nothing may stop
	// calling the plugin once no note is held — the reverb cut where the last voice ended. INFINITE, not a
	// guessed length: SLEEP on an exactly silent block is what decides the tail has finished (the case above).
	HostedPlugin host;
	const clap_plugin* p = host.plugin();
	const auto* tail = static_cast<const clap_plugin_tail*>(p->get_extension(p, CLAP_EXT_TAIL));
	REQUIRE(tail != nullptr);
	const auto* chain =
	    static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	// THE FX LANE TAKES AUDIO DEVICES ONLY (`addFxDevice` refuses a modulator), so a lane holding any is infinite.
	const uint32_t held = chain->device_count(p);
	CHECK(tail->get(p) == (held > 0 ? UINT32_MAX : 0u));

	REQUIRE(chain->add_device(p, feathers::fx::featherverb::kTraits.tag) != UINT32_MAX);
	// ACTIVATE IS A MAIN-THREAD VISIT THAT READS THE LANE (the road a loaded patch takes).
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));
	CHECK(tail->get(p) == UINT32_MAX);
}

TEST_CASE("JM: Feathers reports no latency while no device declares one — 0 through clap_plugin_latency, the host told nothing and asked for no restart, a lane of real devices included") {
	// **THE INVARIANT OF BACKLOG JM**: with no latency-declaring device the
	// plugin's answer is 0 and nothing new reaches the host. A host that OFFERS `clap_host_latency` and counts
	// both calls, so a silence here is a count of zero rather than a host that could not have heard.
	struct Told {
		int changed = 0;
		int restarts = 0;
	} told;
	clap_host host = kHost;
	host.host_data = &told;
	host.get_extension = [](const clap_host*, const char* id) -> const void* {
		static const clap_host_latency kLatency {
			[](const clap_host* h) { ++static_cast<Told*>(h->host_data)->changed; },
		};
		return id != nullptr && std::strcmp(id, CLAP_EXT_LATENCY) == 0 ? &kLatency : nullptr;
	};
	host.request_restart = [](const clap_host* h) { ++static_cast<Told*>(h->host_data)->restarts; };

	HostedPlugin hosted(&host);
	const clap_plugin* p = hosted.plugin();
	const auto* latency = static_cast<const clap_plugin_latency*>(p->get_extension(p, CLAP_EXT_LATENCY));
	REQUIRE(latency != nullptr);
	CHECK(latency->get(p) == 0u);

	const auto* chain =
	    static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(chain->add_device(p, feathers::fx::featherverb::kTraits.tag) != UINT32_MAX);
	REQUIRE(chain->add_device(p, feathers::fx::shaper::kTraits.tag) != UINT32_MAX);
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	hosted.run(kMaxFrames, events);
	events.clear();
	for (int block = 0; block < 8; ++block) {
		hosted.run(kMaxFrames, events);
	}
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, kSampleRate, 1, kMaxFrames));
	REQUIRE(p->start_processing(p));
	CHECK(latency->get(p) == 0u);
	CHECK(told.changed == 0);
	CHECK(told.restarts == 0);
}

namespace {

/// A note through the C API, held `holdSeconds`, released; then blocks run until the plugin says SLEEP or `boundSeconds`
/// pass. Returns the seconds from the key to SLEEP, or a negative number when it never came. With `soundAfter`, the host
/// goes on calling for `afterSeconds` more as if it had not slept, and counts the blocks that SOUND — the promise a
/// SLEEP makes is that there are none (backlog MC).
double secondsFromKeyToSleep(HostedPlugin& host, double holdSeconds, double boundSeconds, int* soundAfter = nullptr,
                             double afterSeconds = 20.0) {
	constexpr uint32_t kFrames = 256;
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	events.clear();
	for (int b = 0; b < static_cast<int>(holdSeconds * kSampleRate / kFrames); ++b) {
		host.run(kFrames, events);
	}
	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();
	const int bound = static_cast<int>(boundSeconds * kSampleRate / kFrames);
	for (int b = 1; b <= bound; ++b) {
		if (host.run(kFrames, events) == CLAP_PROCESS_SLEEP) {
			if (soundAfter != nullptr) {
				*soundAfter = 0;
				for (int a = 0; a < static_cast<int>(afterSeconds * kSampleRate / kFrames); ++a) {
					host.run(kFrames, events);
					*soundAfter += peakOf(host.left(), kFrames) + peakOf(host.right(), kFrames) > 0.0f ? 1 : 0;
				}
			}
			return static_cast<double>(b * kFrames) / kSampleRate;
		}
	}
	return -1.0;
}

} // namespace

TEST_CASE("THE DEFAULT PATCH LETS A RELEASED NOTE GO: a fresh instance, a note held and released with nothing touched — SLEEP after the default envelope's one-second release, not before it and not never") {
	// Since 2026-10-03 a released voice lives while what is ROUTED into its Voice Level stands above the floor
	// (by design, only Voice Level > 0 keeps a voice). So anything a default patch routes into Voice
	// Level that rests ABOVE the floor would hold every released voice for ever. The default patch routes exactly
	// one thing there — the strip's DAHDSR, one-second release, resting at 0 (`ParamBridge::routeDefaultEnvelope`) —
	// and this is the whole plugin proving it lets go.
	HostedPlugin host;
	const double seconds = secondsFromKeyToSleep(host, 0.3, 6.0);
	CAPTURE(seconds);
	CHECK(seconds > 0.5);  // the release ran: a voice cut at the key would SLEEP within a block or two
	CHECK(seconds < 1.5);  // and ended: one second of release plus margin
}

TEST_CASE("EVERY FACTORY PRESET LETS A RELEASED NOTE GO — loaded through the real state road, a note held and released, SLEEP within ninety seconds") {
	// The same claim as the default patch's, for every `.fpreset` the plugin carries: a preset routing a source
	// that rests above the floor (Note Expressions' neutral, a macro, an envelope ending high) into Voice Level
	// would hold its released voices for ever under the 2026-10-03 rule. Each preset's seconds are printed.
	size_t tried = 0;
	for (const feathers::FactoryPresetFile& file : feathers::factoryPresetFiles()) {
		if (!file.path.starts_with("Feathers/")) {
			continue; // device presets load into one device, not the plugin
		}
		CAPTURE(std::string(file.path));
		HostedPlugin host;
		const auto* state =
		    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
		REQUIRE(state != nullptr);
		feathers::PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(file.bytes.begin(), file.bytes.end()), &meta, &payload));
		if (std::find(meta.tags.begin(), meta.tags.end(), "test") != meta.tags.end()) {
			// THE SURVEY'S STRESS PATCHES (README, "The `test` tag") are not rendered to SLEEP here: ninety of them,
			// up to ninety seconds each, would make this case minutes long, and each is Init or a factory preset with
			// one device added or removed — devices whose tails test_fx_devices' silence sweep already holds.
			continue;
		}
		StateBytes bytes;
		bytes.bytes = payload;
		clap_istream in = bytes.in();
		REQUIRE(state->load(host.plugin(), &in));
		// NINETY SECONDS, MEASURED (2026-10-12): the worst is wood_elves_pan at 73.2 s, a real fx tail (-90 dB at 40 s);
		// then pan_tanpura 60.7 s (its delay's 16 s line after the last repeat at 46 s), machine_elf_book_of_dreams 47.1 s
		// (last sound at 32 s, then its own delay's line), baby_elf_bass 16.1 s, Init 1.0 s. 90 s is 23% over the worst.
		// TWO OF THEM SLEPT TOO SOON until backlog MC, which was the defect: machine_elf said SLEEP at 30 s and sounded in
		// 4118 more blocks, pan_tanpura at 31 s and sounded in 1935 more. And for one round machine_elf needed 166 s, held
		// awake by two loops decaying geometrically under -140 dBFS without ever arriving at zero (SKLITTER's capture,
		// Featherverb's convolver feedback) while its output was exactly silent; both loops are now floored at -140 dBFS.
		// AND THE PROMISE ITSELF: nothing sounds in the 20 s after the SLEEP, had the host gone on calling.
		int soundAfter = -1;
		const double seconds = secondsFromKeyToSleep(host, 0.3, 90.0, &soundAfter);
		MESSAGE(std::string(file.path) << ": SLEEP " << seconds << " s after the key, " << soundAfter << " blocks sounding after it");
		CHECK(seconds > 0.0);
		CHECK(soundAfter == 0);
		++tried;
	}
	CHECK(tried > 0);
}

TEST_CASE("pluck: every Pluck factory preset loads into a fresh instance on Pluck and sounds, and saved again reloads value "
          "for value") {
	// THE STARTER PRESETS (tests/bench_patch_plans/save-pluck-presets.plan), read back through the browser's load road into an
	// instance that never held anything else: the engine is Pluck, a note sounds and stays finite, and the patch saved from
	// that instance and loaded into ANOTHER fresh one carries every Pluck control and every sound's string scalers, value for
	// value (CLAUDE.md's persistence shape: capture, save, load into a fresh instance, the same values).
	using feathers::ParamId;
	std::vector<clap_id> ids;
	for (const ParamId id : { ParamId::KsDecay, ParamId::KsDecayTrack, ParamId::KsRelease, ParamId::KsHardness,
	                          ParamId::KsPickup, ParamId::KsWidth, ParamId::KsSeed, ParamId::OscType }) {
		ids.push_back(static_cast<clap_id>(id));
	}
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		for (const ParamId id : { feathers::ksBrightnessParam(slot), feathers::ksSustainParam(slot), feathers::ksPickParam(slot),
		                          feathers::ksNoiseParam(slot), feathers::ksStretchParam(slot) }) {
			ids.push_back(static_cast<clap_id>(id));
		}
	}
	const auto valuesOf = [&](const clap_plugin* p) {
		const auto* params = static_cast<const clap_plugin_params*>(p->get_extension(p, CLAP_EXT_PARAMS));
		std::vector<double> out;
		for (const clap_id id : ids) {
			double v = 0.0;
			REQUIRE(params->get_value(p, id, &v));
			out.push_back(v);
		}
		return out;
	};
	size_t tried = 0;
	for (const feathers::FactoryPresetFile& file : feathers::factoryPresetFiles()) {
		if (!file.path.starts_with("Feathers/Plucks/")) {
			continue;
		}
		CAPTURE(std::string(file.path));
		feathers::PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(file.bytes.begin(), file.bytes.end()), &meta, &payload));
		CHECK(std::find(meta.tags.begin(), meta.tags.end(), "factory") != meta.tags.end());
		HostedPlugin host;
		const auto* chain = testChainOf(host.plugin());
		StateBytes bytes;
		bytes.bytes = payload;
		clap_istream in = bytes.in();
		REQUIRE(chain->load_preset(host.plugin(), &in));
		const std::vector<double> loaded = valuesOf(host.plugin());
		CHECK(loaded[7] == static_cast<double>(feathers::OscType::Pluck)); // ids[7] is Osc Type
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 48, 1, 0.8);
		float peak = 0.0f;
		for (int block = 0; block < 16; ++block) {
			CHECK(host.run(256, events) != CLAP_PROCESS_ERROR);
			events.clear();
			CHECK(allFinite(host.left(), 256));
			peak = std::max(peak, peakOf(host.left(), 256));
		}
		CAPTURE(peak);
		CHECK(peak > 0.02f);
		CHECK(peak < 1.5f);
		StateBytes saved;
		REQUIRE(saveState(host.plugin(), &saved));
		HostedPlugin fresh;
		REQUIRE(loadState(fresh.plugin(), &saved));
		CHECK(valuesOf(fresh.plugin()) == loaded);
		++tried;
	}
	CHECK(tried == 5);
}

TEST_CASE("drum: every Drum factory kit loads into a fresh instance on Drum and sounds, and saved again reloads value for value") {
	// THE STARTER KITS (tests/bench_patch_plans/save-drum-presets.plan), read back through the browser's load road into a fresh
	// instance: the engine is Drum, a kick (Kit) or a C3 (Pitched) sounds and stays finite, and the patch saved from it and
	// loaded into ANOTHER fresh one carries the engine-wide drum controls and every pad's every control, value for value.
	using feathers::ParamId;
	std::vector<clap_id> ids;
	for (const ParamId id : { ParamId::OscType, ParamId::DrumMode, ParamId::DrumPitchedKind, ParamId::DrumWalkX,
	                          ParamId::DrumWalkY, ParamId::DrumSeed }) {
		ids.push_back(static_cast<clap_id>(id));
	}
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		for (size_t c = 0; c < feathers::kDrumControlCount; ++c) {
			ids.push_back(static_cast<clap_id>(feathers::drumParam(slot, static_cast<feathers::DrumControl>(c))));
		}
	}
	const auto valuesOf = [&](const clap_plugin* p) {
		const auto* params = static_cast<const clap_plugin_params*>(p->get_extension(p, CLAP_EXT_PARAMS));
		std::vector<double> out;
		for (const clap_id id : ids) {
			double v = 0.0;
			REQUIRE(params->get_value(p, id, &v));
			out.push_back(v);
		}
		return out;
	};
	size_t tried = 0;
	for (const feathers::FactoryPresetFile& file : feathers::factoryPresetFiles()) {
		if (!file.path.starts_with("Feathers/Drums/")) {
			continue;
		}
		CAPTURE(std::string(file.path));
		feathers::PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(file.bytes.begin(), file.bytes.end()), &meta, &payload));
		CHECK(std::find(meta.tags.begin(), meta.tags.end(), "factory") != meta.tags.end());
		HostedPlugin host;
		const auto* chain = testChainOf(host.plugin());
		StateBytes bytes;
		bytes.bytes = payload;
		clap_istream in = bytes.in();
		REQUIRE(chain->load_preset(host.plugin(), &in));
		const std::vector<double> loaded = valuesOf(host.plugin());
		CHECK(loaded[0] == static_cast<double>(feathers::OscType::Drum));
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, loaded[1] == 1.0 ? 36 : 48, 1, 0.8);
		float peak = 0.0f;
		for (int block = 0; block < 16; ++block) {
			CHECK(host.run(256, events) != CLAP_PROCESS_ERROR);
			events.clear();
			CHECK(allFinite(host.left(), 256));
			peak = std::max(peak, peakOf(host.left(), 256));
		}
		CAPTURE(peak);
		CHECK(peak > 0.02f);
		CHECK(peak < 1.5f);
		StateBytes saved;
		REQUIRE(saveState(host.plugin(), &saved));
		HostedPlugin fresh;
		REQUIRE(loadState(fresh.plugin(), &saved));
		CHECK(valuesOf(fresh.plugin()) == loaded);
		++tried;
	}
	CHECK(tried == 6);
}

TEST_CASE("every test-tagged factory preset loads into a fresh instance and renders a block") {
	// THE SURVEY'S STRESS PATCHES (presets/factory/README.md, "The `test` tag"), written by bench_patch through the
	// browser's save road (`capture_preset`), so each is read back through the browser's load road, into an instance
	// that has never held anything else, and played: a note on, eight blocks, every sample finite and no error.
	size_t tried = 0;
	for (const feathers::FactoryPresetFile& file : feathers::factoryPresetFiles()) {
		feathers::PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(file.bytes.begin(), file.bytes.end()), &meta, &payload));
		if (std::find(meta.tags.begin(), meta.tags.end(), "test") == meta.tags.end()) {
			continue;
		}
		CAPTURE(std::string(file.path));
		CHECK(file.path.starts_with("Feathers/Test/")); // where the README says they live
		HostedPlugin host;
		const auto* chain = testChainOf(host.plugin());
		StateBytes bytes;
		bytes.bytes = payload;
		clap_istream in = bytes.in();
		REQUIRE(chain->load_preset(host.plugin(), &in));
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 48, 1, 0.8);
		for (int block = 0; block < 8; ++block) {
			CAPTURE(block);
			CHECK(host.run(256, events) != CLAP_PROCESS_ERROR);
			events.clear();
			CHECK(allFinite(host.left(), 256));
			CHECK(allFinite(host.right(), 256));
		}
		++tried;
	}
	// NON-VACUITY: the folder holds the survey's 81, and a filter that matched nothing would pass above.
	CHECK(tried >= 81);
}

//------------------------------------------------------------------------------------------------//
// UNBOUNDED DEVICES, STEP 7 — THE PLUGIN WITH SIXTY-FOUR DEVICES                                 //
//------------------------------------------------------------------------------------------------//

TEST_CASE("unbounded devices step 7: sixty-four devices in the lane, every block allocation-free, saved and "
          "reloaded whole") {
	constexpr uint32_t kDevices = 64;
	constexpr uint32_t kFrames = 256;
	const double cutoff = std::log2(440.0);
	StateBytes saved;
	{
		HostedPlugin h;
		const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
		for (uint32_t i = chain->device_count(h.plugin()); i < kDevices; ++i) {
			CAPTURE(i);
			REQUIRE(chain->add_device(h.plugin(), feathers::fx::filter::kTraits.tag) == i);
		}
		REQUIRE(chain->device_count(h.plugin()) == kDevices);
		REQUIRE(chain->set_control(h.plugin(), kDevices - 1, "Filter Cutoff", cutoff));
		// ACTIVATE PROVISIONS the chain the bridge holds (the road a loaded patch takes), and asks for the room.
		const clap_plugin* p = h.plugin();
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));
		// EVERY BLOCK, THE FIRST INCLUDED: the first is where the drains take the grown chains, the grown engine
		// and the bigger published state — the moves this whole arc exists to make allocation-free.
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		for (int block = 0; block < 8; ++block) {
			CAPTURE(block);
			h.run(kFrames, events);
			events.clear();
			CHECK(h.allocationsInLastProcess() == 0);
		}
		REQUIRE(saveState(h.plugin(), &saved));
	}

	HostedPlugin r;
	REQUIRE(loadState(r.plugin(), &saved));
	const clap_plugin_feathers_test_chain* chain = testChainOf(r.plugin());
	CHECK(chain->device_count(r.plugin()) == kDevices);
	double read = 0.0;
	REQUIRE(chain->knob_value(r.plugin(), kDevices - 1, "Filter Cutoff", &read));
	CHECK(read == doctest::Approx(cutoff));
	EventList events;
	for (int block = 0; block < 4; ++block) {
		CAPTURE(block);
		r.run(kFrames, events);
		CHECK(r.allocationsInLastProcess() == 0);
	}
}

TEST_CASE("backlog OJ: 520 routes made by hand in a running plugin, every block allocation-free, all adopted by value, "
          "saved and reloaded whole") {
	// THE PLUGIN'S ROAD, wired as it ships (`wireRouteGrowth` in the constructor): routes made one at a time through
	// the bridge's `setRouteAmount` with blocks running between, past the engine's starting room (512), each publish
	// growing the list and the engine's stores on the main thread first. Every block from activation is counted at
	// zero allocations; every route is in the engine's snapshot by value; and a fresh instance loaded from the save
	// holds them all.
	constexpr uint32_t kRoutes = 520;
	constexpr uint32_t kFrames = 256;
	const auto amountOf = [](size_t k) { return static_cast<double>(0.001f * static_cast<float>(1 + k)); };
	std::vector<std::pair<uint32_t, uint32_t>> made;
	uint32_t initial = 0;
	StateBytes saved;
	{
		HostedPlugin h;
		const clap_plugin_feathers_test_chain* chain = testChainOf(h.plugin());
		REQUIRE(chain->set_core_route != nullptr);
		EventList events;
		h.run(kFrames, events); // the first block, before any route
		initial = chain->engine_route_count(h.plugin()); // the default patch's own
		for (uint32_t s = 0; s < feathers::kCoreSourceCount && made.size() < kRoutes; ++s) {
			for (uint32_t d = 0; d < feathers::kParamDestinationCount && made.size() < kRoutes; ++d) {
				if (chain->set_core_route(h.plugin(), s, d, amountOf(made.size()))) {
					made.emplace_back(s, d);
					if (made.size() % 32 == 0) {
						h.run(kFrames, events);
						CHECK(h.allocationsInLastProcess() == 0);
					}
				}
			}
		}
		REQUIRE(made.size() == kRoutes);
		h.run(kFrames, events);
		CHECK(h.allocationsInLastProcess() == 0);
		CHECK(chain->route_room_refusals(h.plugin()) == 0);
		CHECK(chain->engine_route_count(h.plugin()) == initial + kRoutes);
		for (size_t k = 0; k < made.size(); ++k) {
			CAPTURE(k);
			CHECK(chain->engine_core_route_amount(h.plugin(), made[k].first, made[k].second)
			      == static_cast<double>(static_cast<float>(amountOf(k))));
		}
		REQUIRE(saveState(h.plugin(), &saved));
	}

	HostedPlugin r;
	REQUIRE(loadState(r.plugin(), &saved));
	const clap_plugin_feathers_test_chain* chain = testChainOf(r.plugin());
	EventList events;
	for (int block = 0; block < 2; ++block) {
		r.run(kFrames, events);
		CHECK(r.allocationsInLastProcess() == 0);
	}
	CHECK(chain->route_room_refusals(r.plugin()) == 0);
	CHECK(chain->engine_route_count(r.plugin()) == initial + kRoutes);
	for (size_t k = 0; k < made.size(); ++k) {
		CAPTURE(k);
		CHECK(chain->engine_core_route_amount(r.plugin(), made[k].first, made[k].second)
		      == static_cast<double>(static_cast<float>(amountOf(k))));
	}
}

namespace {

/// Every parameter the plugin reports, by id, through the host's own road (`clap_plugin_params`).
std::vector<std::pair<clap_id, double>> allParamValues(const clap_plugin* plugin) {
	const auto* params = static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	std::vector<std::pair<clap_id, double>> out;
	const uint32_t n = params->count(plugin);
	for (uint32_t i = 0; i < n; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(plugin, i, &info));
		double value = 0.0;
		REQUIRE(params->get_value(plugin, info.id, &value));
		out.emplace_back(info.id, value);
	}
	return out;
}

/// `bytes` with the gamma block replaced by one holding `gamma` — the block is FSG1 up to the next tag (the
/// variations', FSV<n>), found by searching rather than by a length this file would have to restate.
std::vector<uint8_t> withGamma(const std::vector<uint8_t>& sealed, const feathers::SoundGamma& gamma) {
	// SPLICED IN THE PAYLOAD, THEN SEALED AGAIN: the plugin's save ends in its end marker (kStateVersion 50).
	const std::vector<uint8_t> bytes = payloadOf(sealed);
	const size_t at = blockAt(bytes, "FSG1");
	const size_t next = blockAt(bytes, "FSV", at);
	REQUIRE(at < bytes.size());
	REQUIRE(next < bytes.size());
	StateBytes spliced;
	spliced.bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(at));
	clap_ostream out = spliced.out();
	REQUIRE(feathers::saveSoundGamma(&out, gamma));
	spliced.bytes.insert(spliced.bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(next), bytes.end());
	seal(spliced);
	return spliced.bytes;
}

/// THE EVENT LOG POINTED AT A SCRATCH FILE for one case, and put back to `off` after — never the user's file.
struct ScratchEventLog {
	std::string path;
	ScratchEventLog() {
		path = (std::filesystem::temp_directory_path() / ("feathers-events-" + std::to_string(::getpid()) + ".log")).string();
		std::filesystem::remove(path);
		::setenv("FEATHERS_EVENT_LOG", path.c_str(), 1);
	}
	~ScratchEventLog() {
		::setenv("FEATHERS_EVENT_LOG", "off", 1);
		std::error_code ec;
		std::filesystem::remove(path, ec);
	}
	std::string text() const {
		std::ifstream in(path);
		return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
};

} // namespace

TEST_CASE("a state load refused inside a later block changes NOTHING — every parameter and every block is the patch it had") {
	// THE PARTIAL-LOAD HAZARD (the plugin appeared to forget its state spontaneously). The
	// walk used to COMMIT the parameters and then apply each block as it read it, so a stream refused in the
	// chains block left new parameters, a new gamma, and the old rest — a patch nobody saved, under a `false`
	// the host does nothing with. Now nothing is applied until everything has parsed. Asserted by VALUE: each
	// parameter as the host reads it, the gamma block's own number, and the resaved bytes whole.
	const ScratchEventLog log;
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	// THE LIVE PATCH: AZone 5 and a gamma base of -2 on sound 0, loaded whole so it is a real patch.
	{
		EventList events;
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 5.0);
		(void)host.run(64, events); // a quiet block may answer SLEEP; the parameter lands either way
		StateBytes saved;
		clap_ostream out = saved.out();
		REQUIRE(state->save(host.plugin(), &out));
		feathers::SoundGamma gamma;
		gamma.base[0] = -2.0;
		StateBytes live;
		live.bytes = withGamma(saved.bytes, gamma);
		clap_istream in = live.in();
		REQUIRE(state->load(host.plugin(), &in));
	}
	const auto paramsBefore = allParamValues(host.plugin());
	StateBytes before;
	{
		clap_ostream out = before.out();
		REQUIRE(state->save(host.plugin(), &out));
	}
	REQUIRE(paramsBefore.size() > 100);

	// THE INCOMING PATCH: AZone 17 and a gamma of +1.5 — different in a parameter AND in a block that comes
	// BEFORE the one that will fail, which is exactly what the old order committed — and a chains block that
	// refuses its bytes, so the gamma parsed and the chains did not.
	StateBytes incoming;
	{
		EventList events;
		events.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
		(void)host.run(64, events); // a quiet block may answer SLEEP; the parameter lands either way
		StateBytes saved;
		clap_ostream out = saved.out();
		REQUIRE(state->save(host.plugin(), &out));
		feathers::SoundGamma gamma;
		gamma.base[0] = 1.5;
		incoming.bytes = withGamma(saved.bytes, gamma);
		// A CHAINS BLOCK A WRITER COULD NOT HAVE MADE, SEALED: the end marker vouches for the stream, so the walk
		// reaches the chains and refuses THERE — the case the all-or-nothing load exists for. (A cut stream is the
		// end marker's to refuse; "a stream cut at any block boundary…" has those.)
		incoming.bytes = payloadOf(incoming.bytes);
		const size_t chains = blockAt(incoming.bytes, "FSC1");
		REQUIRE(chains + 8 < incoming.bytes.size());
		incoming.bytes[chains + 8] = static_cast<uint8_t>('#'); // the document's first byte: not JSON
		seal(incoming);
		// AND BACK TO THE LIVE PATCH, which is what the refused load below must leave in place.
		clap_istream restore = before.in();
		REQUIRE(state->load(host.plugin(), &restore));
		before.read = 0;
	}
	REQUIRE(allParamValues(host.plugin()) == paramsBefore);

	clap_istream in = incoming.in();
	CHECK_FALSE(state->load(host.plugin(), &in));

	// EVERY PARAMETER, BY VALUE.
	const auto paramsAfter = allParamValues(host.plugin());
	REQUIRE(paramsAfter.size() == paramsBefore.size());
	for (size_t i = 0; i < paramsBefore.size(); ++i) {
		INFO("param id " << paramsBefore[i].first);
		CHECK(paramsAfter[i].first == paramsBefore[i].first);
		CHECK(paramsAfter[i].second == paramsBefore[i].second);
	}
	CHECK(paramsAfter == paramsBefore);

	// THE BLOCK BEFORE THE FAILURE, BY VALUE — the gamma the old order would have applied.
	StateBytes after;
	clap_ostream out = after.out();
	REQUIRE(state->save(host.plugin(), &out));
	{
		StateBytes slice = from(after.bytes, "FSG1");
		clap_istream gin = slice.in();
		feathers::SoundGamma read;
		bool present = false;
		REQUIRE(feathers::loadSoundGamma(&gin, &read, &present));
		CHECK(present);
		CHECK(read.base[0] == -2.0);
	}
	// AND THE WHOLE PATCH, every block: the save after the refusal is the save before it.
	CHECK(after.bytes == before.bytes);

	// THE LOG SAYS WHICH STAGE, AND WHY — the instrument this needs.
	const std::string text = log.text();
	CHECK(text.find("state-load FAILED stage=chains") != std::string::npos);
	CHECK(text.find("block-refused-its-bytes") != std::string::npos);
	CHECK(text.find("nothing changed") != std::string::npos);
}

TEST_CASE("a state load with the wrong version is refused at the parameters, logged with the version it found") {
	const ScratchEventLog log;
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	REQUIRE(!saved.bytes.empty());
	CHECK(saved.bytes[0] == feathers::kStateVersion);
	saved.bytes[0] = static_cast<uint8_t>(feathers::kStateVersionUntrailered - 1); // 48: older than any this build reads
	clap_istream in = saved.in();
	CHECK_FALSE(state->load(host.plugin(), &in));
	const std::string text = log.text();
	CHECK(text.find("state-save ok bytes=" + std::to_string(saved.bytes.size())) != std::string::npos);
	CHECK(text.find("state-load FAILED stage=params at=1 bytes=" + std::to_string(saved.bytes.size())
	                + " why=version-mismatch: saved by version " + std::to_string(feathers::kStateVersionUntrailered - 1)
	                + ", this build reads " + std::to_string(feathers::kStateVersionUntrailered) + " to "
	                + std::to_string(feathers::kStateVersion))
	      != std::string::npos);
	// AND THE LIFECYCLE AROUND IT, in order, from this instance.
	const size_t create = text.find(" create\n");
	const size_t init = text.find(" init format=clap host=\"");
	const size_t activate = text.find(" activate rate=48000 frames=1..");
	REQUIRE(create != std::string::npos);
	REQUIRE(init != std::string::npos);
	REQUIRE(activate != std::string::npos);
	CHECK(create < init);
	CHECK(init < activate);
}

namespace {

/// A LIVE PATCH THAT IS NOT THE DEFAULT, and an INCOMING one that differs from it — so a load that wrongly went
/// through could not pass by changing nothing. `incoming` is a whole, sealed v50 save.
struct TwoPatches {
	std::vector<uint8_t> live;
	std::vector<uint8_t> incoming;
};

TwoPatches twoPatches(HostedPlugin& host, const clap_plugin_state* state) {
	TwoPatches out;
	const auto saveNow = [&] {
		StateBytes b;
		clap_ostream o = b.out();
		REQUIRE(state->save(host.plugin(), &o));
		return b.bytes;
	};
	EventList toIncoming;
	toIncoming.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 17.0);
	(void)host.run(64, toIncoming);
	feathers::SoundGamma gamma;
	gamma.base[0] = 1.5;
	out.incoming = withGamma(saveNow(), gamma);
	EventList toLive;
	toLive.pushParam(0, static_cast<clap_id>(feathers::ParamId::AZone), 5.0);
	(void)host.run(64, toLive);
	gamma.base[0] = -2.0;
	StateBytes live;
	live.bytes = withGamma(saveNow(), gamma);
	clap_istream in = live.in();
	REQUIRE(state->load(host.plugin(), &in));
	out.live = saveNow();
	REQUIRE(out.live != out.incoming);
	return out;
}

/// The offsets a stream can be cut at: every block boundary (where each tag starts, and the payload's end), and
/// a point inside every block — the parameters' record and each tagged block — and inside the trailer.
std::vector<size_t> cutOffsets(const std::vector<uint8_t>& sealed) {
	const std::vector<uint8_t> payload = payloadOf(sealed);
	std::vector<size_t> cuts = { 1, 5, 40 }; // inside the parameters' record
	size_t from = 0;
	for (const char* tag : { "FSL1", "FST2", "FSP1", "FSR", "FSG1", "FSV", "FSH1", "FSC1", "FSMB" }) {
		const size_t at = blockAt(payload, tag, from);
		REQUIRE(at < payload.size());
		cuts.push_back(at);     // the boundary before this block
		cuts.push_back(at + 2); // inside its tag
		cuts.push_back(at + 7); // inside its body
		from = at + 4;
	}
	cuts.push_back(payload.size());                  // every block, no end marker
	cuts.push_back(payload.size() + 4);              // the marker's magic, no length
	cuts.push_back(sealed.size() - 1);               // one byte short
	return cuts;
}

} // namespace

TEST_CASE("a stream cut at any block boundary, or inside any block, is refused with nothing changed") {
	// The stream cannot be made atomic, so an end marker makes a truncated one detectable. Before kStateVersion
	// 50 a stream cut at a block boundary LOADED — every block after the cut read as
	// "a patch from before this block", the chains as a fresh instance's — and that is the shape of a patch that
	// forgot its devices. Now the end marker is the last thing written, so a cut anywhere has lost it.
	const ScratchEventLog log;
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	const TwoPatches patches = twoPatches(host, state);
	const auto paramsBefore = allParamValues(host.plugin());
	for (const size_t cut : cutOffsets(patches.incoming)) {
		CAPTURE(cut);
		StateBytes cutStream;
		cutStream.bytes.assign(patches.incoming.begin(), patches.incoming.begin() + static_cast<std::ptrdiff_t>(cut));
		clap_istream in = cutStream.in();
		CHECK_FALSE(state->load(host.plugin(), &in));
		CHECK(allParamValues(host.plugin()) == paramsBefore);
		StateBytes after;
		clap_ostream out = after.out();
		REQUIRE(state->save(host.plugin(), &out));
		CHECK(after.bytes == patches.live);
	}
	CHECK(log.text().find("stage=end-marker") != std::string::npos);
	CHECK(log.text().find("why=no-end-marker; nothing changed") != std::string::npos);
	// AND THE WHOLE STREAM DOES LOAD — so the refusals above are about the cuts, not about the patch.
	StateBytes whole;
	whole.bytes = patches.incoming;
	clap_istream in = whole.in();
	CHECK(state->load(host.plugin(), &in));
	CHECK(allParamValues(host.plugin()) != paramsBefore);
}

TEST_CASE("a flipped byte is refused by the checksum, and a wrong length by the length, with nothing changed") {
	const ScratchEventLog log;
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	const TwoPatches patches = twoPatches(host, state);
	const auto paramsBefore = allParamValues(host.plugin());
	const size_t payload = patches.incoming.size() - feathers::state::kTrailerBytes;
	// ONE BIT, in the parameters, in the gamma block, and in the chains document.
	for (const size_t at : { size_t { 20 }, blockAt(patches.incoming, "FSG1") + 6, blockAt(patches.incoming, "FSC1") + 20 }) {
		CAPTURE(at);
		REQUIRE(at < payload);
		StateBytes flipped;
		flipped.bytes = patches.incoming;
		flipped.bytes[at] ^= 0x04;
		clap_istream in = flipped.in();
		CHECK_FALSE(state->load(host.plugin(), &in));
		CHECK(allParamValues(host.plugin()) == paramsBefore);
	}
	CHECK(log.text().find("why=checksum; nothing changed") != std::string::npos);
	// THE LENGTH FIELD SAYING ONE BYTE MORE THAN THERE IS.
	StateBytes longer;
	longer.bytes = patches.incoming;
	longer.bytes[payload + 4] = static_cast<uint8_t>(longer.bytes[payload + 4] + 1);
	clap_istream in = longer.in();
	CHECK_FALSE(state->load(host.plugin(), &in));
	CHECK(allParamValues(host.plugin()) == paramsBefore);
	CHECK(log.text().find("why=length-mismatch; nothing changed") != std::string::npos);
	StateBytes after;
	clap_ostream out = after.out();
	REQUIRE(state->save(host.plugin(), &out));
	CHECK(after.bytes == patches.live);
}

TEST_CASE("a version 49 stream, which has no end marker, loads whole — and is refused missing any block") {
	// PROJECTS MAY NOW BE KEPT, so 49 is read. Its stand-in for the marker: every 49 writer wrote every block.
	const ScratchEventLog log;
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	const TwoPatches patches = twoPatches(host, state);
	const auto paramsBefore = allParamValues(host.plugin());
	// A 49 STREAM IS A 50 PAYLOAD WITH ITS FIRST BYTE 49 AND NO TRAILER — the format did not change otherwise.
	std::vector<uint8_t> v49 = payloadOf(patches.incoming);
	v49[0] = feathers::kStateVersionUntrailered;
	std::vector<size_t> cuts;
	for (const size_t cut : cutOffsets(patches.incoming)) {
		if (cut < v49.size()) {
			cuts.push_back(cut);
		}
	}
	for (const size_t cut : cuts) {
		CAPTURE(cut);
		StateBytes cutStream;
		cutStream.bytes.assign(v49.begin(), v49.begin() + static_cast<std::ptrdiff_t>(cut));
		clap_istream in = cutStream.in();
		CHECK_FALSE(state->load(host.plugin(), &in));
		CHECK(allParamValues(host.plugin()) == paramsBefore);
	}
	CHECK(log.text().find("v49-missing-block") != std::string::npos);
	// THE WHOLE 49 STREAM LOADS, AND IS THE INCOMING PATCH BY VALUE: its resave is the incoming 50 save exactly.
	StateBytes whole;
	whole.bytes = v49;
	clap_istream in = whole.in();
	REQUIRE(state->load(host.plugin(), &in));
	StateBytes after;
	clap_ostream out = after.out();
	REQUIRE(state->save(host.plugin(), &out));
	CHECK(after.bytes == patches.incoming);
	CHECK(log.text().find("state-load ok bytes=" + std::to_string(v49.size()) + " version=49 end-marker=none (v49)")
	      != std::string::npos);
}

TEST_CASE("a version 50 save ends in its end marker, and round-trips exactly into a fresh instance") {
	const ScratchEventLog log;
	std::vector<uint8_t> saved;
	{
		HostedPlugin host;
		const auto* state =
		    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
		saved = twoPatches(host, state).live;
	}
	REQUIRE(saved[0] == feathers::kStateVersion);
	size_t payload = 0;
	REQUIRE(feathers::state::checkTrailer(saved, &payload) == feathers::state::TrailerCheck::Ok);
	CHECK(payload == saved.size() - feathers::state::kTrailerBytes);
	CHECK(std::memcmp(saved.data() + payload, "FEND", 4) == 0);
	// THE CRC, RESTATED HERE BY ITS DEFINITION'S CHECK VALUE, so the one the stream carries is the standard one.
	const std::string check = "123456789";
	CHECK(feathers::state::crc32(std::span(reinterpret_cast<const uint8_t*>(check.data()), check.size())) == 0xCBF43926u);
	HostedPlugin fresh;
	const auto* state =
	    static_cast<const clap_plugin_state*>(fresh.plugin()->get_extension(fresh.plugin(), CLAP_EXT_STATE));
	StateBytes in;
	in.bytes = saved;
	clap_istream is = in.in();
	REQUIRE(state->load(fresh.plugin(), &is));
	StateBytes again;
	clap_ostream os = again.out();
	REQUIRE(state->save(fresh.plugin(), &os));
	CHECK(again.bytes == saved);
	CHECK(log.text().find("version=" + std::to_string(feathers::kStateVersion) + " end-marker=ok") != std::string::npos);
}

namespace {

/// A HOST STREAM THAT TAKES `limit` BYTES AND THEN REFUSES — a disk that filled, say.
struct ShortStream {
	std::vector<uint8_t> bytes;
	size_t limit = 0;
	static int64_t CLAP_ABI writeFn(const clap_ostream* stream, const void* buffer, uint64_t size) {
		auto* self = static_cast<ShortStream*>(stream->ctx);
		const size_t room = self->limit - std::min(self->limit, self->bytes.size());
		const size_t take = std::min<size_t>(room, static_cast<size_t>(size));
		if (take == 0) {
			return -1;
		}
		const auto* from = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), from, from + take);
		return static_cast<int64_t>(take);
	}
	clap_ostream out() { return clap_ostream { this, writeFn }; }
};

} // namespace

TEST_CASE("a save the host's stream cuts short returns false and is logged as a short write — and cannot load") {
	const ScratchEventLog log;
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	ShortStream shortStream;
	shortStream.limit = 1000;
	clap_ostream out = shortStream.out();
	CHECK_FALSE(state->save(host.plugin(), &out));
	CHECK(shortStream.bytes.size() == 1000);
	CHECK(log.text().find("state-save FAILED stage=host-stream bytes=1000 of ") != std::string::npos);
	CHECK(log.text().find("why=host-short-write") != std::string::npos);
	// WHAT THE HOST KEPT, IF IT KEPT IT, IS REFUSED ON THE WAY BACK — the end marker is the last thing written.
	StateBytes kept;
	kept.bytes = shortStream.bytes;
	clap_istream in = kept.in();
	CHECK_FALSE(state->load(host.plugin(), &in));
}

TEST_CASE("every shipped factory preset of the synth loads under the version 49 rule — each one whole") {
	// THE FACTORY FILES ARE VERSION 49 (written before the end marker), so they are read by 49's rule: every block
	// present. A shipped preset that a cut-stream refusal turned away would be "Init does nothing" for every user.
	HostedPlugin host;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	size_t seen = 0;
	for (const feathers::FactoryPresetFile& file : feathers::factoryPresetFiles()) {
		if (!file.path.starts_with("Feathers/")) {
			continue; // a device's bank, which is a rack document and not this plugin's state
		}
		CAPTURE(std::string(file.path));
		feathers::PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(file.bytes.begin(), file.bytes.end()), &meta, &payload));
		REQUIRE(!payload.empty());
		StateBytes stream;
		stream.bytes = payload;
		clap_istream in = stream.in();
		CHECK(chain->load_preset(host.plugin(), &in));
		++seen;
	}
	CHECK(seen >= 1);
}

//==================================================================================================
// A HOST RESTART KEEPS THE HELD NOTES (switching the TZ switch killed sound until the next note).
// A latency that moves makes the plugin ask for a restart,
// and the host does stop, deactivate, activate, start — while still holding the key, so a voice the restart
// cleared stays silent until the next note. The host below honours `request_restart` that way, on the next
// block boundary, as Bitwig does.
//==================================================================================================

namespace {

struct RestartingHost {
	int restarts = 0;     ///< how many the plugin asked for
	int honoured = 0;     ///< how many were carried out
	int latencyTold = 0;  ///< `clap_host_latency.changed` calls
	double rate = kSampleRate; ///< the rate a restart it honours reactivates at
	clap_host host = kHost;

	RestartingHost() {
		host.host_data = this;
		host.get_extension = [](const clap_host*, const char* id) -> const void* {
			static const clap_host_latency kLatency {
				[](const clap_host* h) { ++static_cast<RestartingHost*>(h->host_data)->latencyTold; },
			};
			return id != nullptr && std::strcmp(id, CLAP_EXT_LATENCY) == 0 ? &kLatency : nullptr;
		};
		host.request_restart = [](const clap_host* h) { ++static_cast<RestartingHost*>(h->host_data)->restarts; };
	}

	static void reactivate(const clap_plugin* p, double rate) {
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, rate, 1, kMaxFrames));
		REQUIRE(p->start_processing(p));
	}

	/// One block, then any restart the plugin asked for during it.
	float block(HostedPlugin& hosted, const EventList& events, uint32_t frames) {
		hosted.run(frames, events);
		const float peak = std::max(peakOf(hosted.left(), frames), peakOf(hosted.right(), frames));
		if (restarts > honoured) {
			honoured = restarts;
			reactivate(hosted.plugin(), rate);
		}
		return peak;
	}
};

} // namespace

TEST_CASE("a held note keeps sounding across a host restart — a Through Zero flip, a Mode flip with Through Zero on, and the EQ's Phase switch") {
	enum class Flip { ThroughZero, ModeWithTz, EqPhase };
	for (const Flip flip : { Flip::ThroughZero, Flip::ModeWithTz, Flip::EqPhase }) {
		CAPTURE(static_cast<int>(flip));
		RestartingHost rh;
		HostedPlugin hosted(&rh.host);
		const clap_plugin* p = hosted.plugin();
		const auto* chain = testChainOf(p);
		const char* tag = flip == Flip::EqPhase ? feathers::fx::eq::kTraits.tag : feathers::fx::phaser::kTraits.tag;
		const uint32_t slot = chain->add_device(p, tag);
		REQUIRE(slot != UINT32_MAX);
		if (flip == Flip::ModeWithTz) {
			REQUIRE(chain->set_control(p, slot, "Phaser Mode", 1.0));
			REQUIRE(chain->set_control(p, slot, "Phaser Through Zero", 1.0));
		}
		else if (flip == Flip::ThroughZero) {
			REQUIRE(chain->set_control(p, slot, "Phaser Mode", 1.0));
		}
		RestartingHost::reactivate(p, kSampleRate); // ACTIVATE PROVISIONS the chain the bridge holds
		constexpr uint32_t kFrames = 256;
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		rh.block(hosted, events, kFrames);
		events.clear();
		float before = 0.0f;
		for (int b = 0; b < 40; ++b) {
			before = std::max(before, rh.block(hosted, events, kFrames));
		}
		REQUIRE(before > 0.05f); // the note sounds before the flip
		const int restartsBefore = rh.restarts;
		switch (flip) {
		case Flip::ThroughZero: REQUIRE(chain->set_control(p, slot, "Phaser Through Zero", 1.0)); break;
		case Flip::ModeWithTz: REQUIRE(chain->set_control(p, slot, "Phaser Mode", 0.0)); break;
		case Flip::EqPhase: REQUIRE(chain->set_control(p, slot, "Phase", 1.0)); break;
		}
		// THE KEY IS STILL HELD: no event follows. The restart lands within a block or two; the 9216-sample linear-phase
		// EQ needs ~36 blocks to come out, so the note is judged over the last 40 of 120.
		float after = 0.0f;
		int silentLate = 0;
		for (int b = 0; b < 120; ++b) {
			const float peak = rh.block(hosted, events, kFrames);
			if (b >= 80) {
				after = std::max(after, peak);
				silentLate += peak == 0.0f ? 1 : 0;
			}
		}
		// THE ROAD WAS TAKEN: the Mode flip and the EQ's Phase move the latency, so the host restarted the plugin.
		if (flip != Flip::ThroughZero) {
			CHECK(rh.restarts > restartsBefore);
			CHECK(rh.honoured == rh.restarts);
		}
		CAPTURE(after);
		CHECK(silentLate == 0);
		CHECK(after > 0.05f);
	}
}

TEST_CASE("ND: OWLTT's Lookahead is the plugin's latency, and a held note keeps sounding across the restart a change of it asks for") {
	// BACKLOG ND: the lookahead is a latency, so choosing one asks the host for a restart — and the restart must not kill
	// the held note (b5d148e9's road). The plugin's answer through `clap_plugin_latency` is the 5 ms the device delays by.
	RestartingHost rh;
	HostedPlugin hosted(&rh.host);
	const clap_plugin* p = hosted.plugin();
	const auto* chain = testChainOf(p);
	const uint32_t slot = chain->add_device(p, feathers::fx::owltt::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	RestartingHost::reactivate(p, kSampleRate);
	const auto* latency = static_cast<const clap_plugin_latency*>(p->get_extension(p, CLAP_EXT_LATENCY));
	REQUIRE(latency != nullptr);
	CHECK(latency->get(p) == 0u); // Off: no latency
	constexpr uint32_t kFrames = 256;
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	rh.block(hosted, events, kFrames);
	events.clear();
	float before = 0.0f;
	for (int b = 0; b < 40; ++b) {
		before = std::max(before, rh.block(hosted, events, kFrames));
	}
	REQUIRE(before > 0.05f);
	const int restartsBefore = rh.restarts;
	REQUIRE(chain->set_control(p, slot, "Lookahead", 5.0)); // "5 ms"
	float after = 0.0f;
	int silentLate = 0;
	for (int b = 0; b < 60; ++b) {
		const float peak = rh.block(hosted, events, kFrames);
		if (b >= 20) {
			after = std::max(after, peak);
			silentLate += peak == 0.0f ? 1 : 0;
		}
	}
	CHECK(rh.restarts > restartsBefore);
	CHECK(rh.honoured == rh.restarts);
	CHECK(latency->get(p) == static_cast<uint32_t>(std::lround(0.005 * kSampleRate)));
	CAPTURE(after);
	CHECK(silentLate == 0);
	CHECK(after > 0.05f);
}

TEST_CASE("NR: CLIP's Oversample is the plugin's latency, and a held note keeps sounding across the restart each toggle asks for") {
	// BACKLOG NR: 4x delays the output by `kOversampleLatency`, so the device declares it; toggling Oversample moves the
	// latency, the plugin asks for a restart, and the restart must not kill the held note (b5d148e9's road). Both ways.
	RestartingHost rh;
	HostedPlugin hosted(&rh.host);
	const clap_plugin* p = hosted.plugin();
	const auto* chain = testChainOf(p);
	const uint32_t slot = chain->add_device(p, feathers::fx::clip::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	RestartingHost::reactivate(p, kSampleRate);
	const auto* latency = static_cast<const clap_plugin_latency*>(p->get_extension(p, CLAP_EXT_LATENCY));
	REQUIRE(latency != nullptr);
	constexpr uint32_t kFrames = 256;
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	rh.block(hosted, events, kFrames);
	events.clear();
	float before = 0.0f;
	for (int b = 0; b < 40; ++b) {
		before = std::max(before, rh.block(hosted, events, kFrames));
	}
	REQUIRE(before > 0.05f);
	// 4x IS THE DEFAULT: the lane's latency lands with the chain (a restart of its own, honoured above).
	CHECK(latency->get(p) == feathers::fx::clip::kOversampleLatency);
	for (const double oversample : { 0.0, 1.0 }) {
		CAPTURE(oversample);
		const int restartsBefore = rh.restarts;
		REQUIRE(chain->set_control(p, slot, "Clip Oversample", oversample));
		// THE KEY IS STILL HELD: no event follows.
		float after = 0.0f;
		int silentLate = 0;
		for (int b = 0; b < 60; ++b) {
			const float peak = rh.block(hosted, events, kFrames);
			if (b >= 20) {
				after = std::max(after, peak);
				silentLate += peak == 0.0f ? 1 : 0;
			}
		}
		CHECK(rh.restarts > restartsBefore);
		CHECK(rh.honoured == rh.restarts);
		CHECK(latency->get(p) == (oversample >= 0.5 ? feathers::fx::clip::kOversampleLatency : 0u));
		CAPTURE(after);
		CHECK(silentLate == 0);
		CHECK(after > 0.05f);
	}
}

TEST_CASE("a Through Zero flip in the Flanger asks the host for no restart: the latency does not move") {
	RestartingHost rh;
	HostedPlugin hosted(&rh.host);
	const clap_plugin* p = hosted.plugin();
	const auto* chain = testChainOf(p);
	const auto* latency = static_cast<const clap_plugin_latency*>(p->get_extension(p, CLAP_EXT_LATENCY));
	REQUIRE(latency != nullptr);
	const uint32_t slot = chain->add_device(p, feathers::fx::phaser::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(p, slot, "Phaser Mode", 1.0));
	RestartingHost::reactivate(p, kSampleRate);
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	rh.block(hosted, events, 256);
	events.clear();
	// THE LANE ADOPTS ITS DEVICES AT THE FIRST BLOCK (assumption L5), so the Flanger's latency costs that one restart.
	for (int b = 0; b < 4; ++b) {
		rh.block(hosted, events, 256);
	}
	const uint32_t flanger = latency->get(p);
	CHECK(flanger == static_cast<uint32_t>(std::lround(0.008 * kSampleRate))); // 384: the Flanger reports the 8 ms
	const int restartsBefore = rh.restarts;
	for (const double tz : { 1.0, 0.0, 1.0 }) {
		CAPTURE(tz);
		REQUIRE(chain->set_control(p, slot, "Phaser Through Zero", tz));
		for (int b = 0; b < 20; ++b) {
			rh.block(hosted, events, 256);
		}
		double base = -1.0;
		REQUIRE(chain->engine_control(p, slot, "Phaser Through Zero", &base, nullptr));
		CHECK(base == tz); // the flip reached the engine — so a missing restart is not a missing flip
		CHECK(rh.restarts == restartsBefore);
		CHECK(latency->get(p) == flanger);
	}
	// NON-VACUITY: the Mode flip DOES move it, and asks.
	REQUIRE(chain->set_control(p, slot, "Phaser Mode", 0.0));
	for (int b = 0; b < 4; ++b) {
		rh.block(hosted, events, 256);
	}
	CHECK(rh.restarts == restartsBefore + 1);
	CHECK(latency->get(p) == 0u);
}

TEST_CASE("a reactivation at a CHANGED sample rate still clears the voices, and a new note sounds") {
	RestartingHost rh;
	HostedPlugin hosted(&rh.host);
	const clap_plugin* p = hosted.plugin();
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	rh.block(hosted, events, 256);
	events.clear();
	float before = 0.0f;
	for (int b = 0; b < 20; ++b) {
		before = std::max(before, rh.block(hosted, events, 256));
	}
	REQUIRE(before > 0.05f);
	// THE SAME RATE KEEPS IT (the restart road)...
	RestartingHost::reactivate(p, kSampleRate);
	float same = 0.0f;
	for (int b = 0; b < 10; ++b) {
		same = std::max(same, rh.block(hosted, events, 256));
	}
	CHECK(same > 0.05f);
	// ...A NEW RATE DOES NOT: every voice's state is sized for the old one.
	RestartingHost::reactivate(p, 96000.0);
	float changed = 0.0f;
	for (int b = 0; b < 10; ++b) {
		hosted.run(256, events);
		changed = std::max(changed, peakOf(hosted.left(), 256));
	}
	CHECK(changed == 0.0f);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 64, 2, 1.0);
	hosted.run(256, events);
	events.clear();
	float fresh = 0.0f;
	for (int b = 0; b < 10; ++b) {
		hosted.run(256, events);
		fresh = std::max(fresh, peakOf(hosted.left(), 256));
	}
	CHECK(fresh > 0.05f);
}

TEST_CASE("GO: a same-rate reactivation keeps the held note and the longest block is GROW-ONLY — 256 -> 4096 re-prepares every device at 4096, 4096 -> 256 re-prepares nothing") {
	// THE SAFETY QUESTION OF THE KEPT-VOICE ROAD: a host may reactivate at the same rate with a different
	// `max_frames_count`, and that road keeps the voices. Since backlog GO the block IS read: `AudioOps::prepare`
	// carries it and devices size their scratch to it. The decision (2026-10-11): at a kept rate the lane's
	// devices are re-prepared ONLY when the block GROWS — a shrink's scratch already fits, and re-preparing would cut
	// every tail under a voice the road keeps. So the case runs EVERY device the lane accepts, at 256, then 4096,
	// then 256 again, with a note held across both reactivations — under the ASan build this is the overflow probe;
	// here it asserts the note, the samples, the block the devices were handed and how many times they were prepared.
	RestartingHost rh;
	HostedPlugin hosted(&rh.host);
	const clap_plugin* p = hosted.plugin();
	const auto* chain = testChainOf(p);
	int placed = 0;
	char tag[64] = {};
	for (uint32_t i = 0; chain->available_device(p, i, tag, sizeof(tag)); ++i) {
		const uint32_t slot = chain->add_device(p, tag);
		placed += slot != UINT32_MAX ? 1 : 0;
		// A SIDECHAIN INJECT AT ITS DEFAULT (Mix 100%) REPLACES THE SIGNAL WITH A SIDECHAIN THIS HOST NEVER CONNECTS: silence, by
		// design. The case wants the note to sound through every device, so this one is set to pass what arrives.
		if (slot != UINT32_MAX && std::string(tag) == feathers::fx::inject::kTraits.tag) {
			REQUIRE(chain->set_control(p, slot, "Inject Mix", 0.0));
		}
		// THE ARP AND PATTERN TAKE THE KEY (backlog QU: the first block's key is theirs too) and, at their default Run
		// (Transport), play nothing while this host's transport is stopped — so the held note would sound through neither,
		// and the case would hear only the tails the growth re-prepare cuts. Always: they play from the key, as the case
		// means every device to.
		if (slot != UINT32_MAX && (std::string(tag) == "ARP" || std::string(tag) == "PAT")) {
			REQUIRE(chain->set_control(p, slot, "Run", 0.0));
		}
	}
	CAPTURE(placed);
	REQUIRE(placed >= 10); // the whole catalogue, not a token device
	const auto activateWith = [&](uint32_t maxFrames) {
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, kSampleRate, 1, maxFrames));
		REQUIRE(p->start_processing(p));
	};
	activateWith(256);
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	hosted.run(256, events);
	events.clear();
	float before = 0.0f;
	for (int b = 0; b < 200; ++b) {
		before = std::max(before, rh.block(hosted, events, 256)); // honours the restart a latent device asks for
	}
	REQUIRE(before > 0.0f);
	const int restarts = rh.restarts;
	const uint64_t preparedAt256 = chain->prepare_count(p);
	REQUIRE(chain->max_frames(p) == kMaxFrames); // the constructor's 512: the 256 that followed was already a shrink
	activateWith(4096);
	// THE GROWTH IS HANDED ON, BY VALUE, IN ONE PREPARE.
	CHECK(chain->max_frames(p) == 4096u);
	CHECK(chain->prepare_count(p) == preparedAt256 + 1);
	hosted.growBuffers(4096);
	int silent = 0;
	bool finite = true;
	for (int b = 0; b < 12; ++b) {
		hosted.run(4096, events);
		float peak = 0.0f;
		for (uint32_t i = 0; i < 4096; ++i) {
			finite = finite && std::isfinite(hosted.left()[i]) && std::isfinite(hosted.right()[i]);
			peak = std::max({ peak, std::fabs(hosted.left()[i]), std::fabs(hosted.right()[i]) });
		}
		silent += b >= 4 && peak == 0.0f ? 1 : 0; // past the longest latency in the lane (the linear-phase EQ's)
	}
	CHECK(finite);
	CHECK(silent == 0);
	CHECK(rh.restarts == restarts); // the larger block moved no latency: no restart hid a cleared voice

	// AND THE SHRINK RE-PREPARES NOTHING: the devices keep the 4096 they were prepared with (their scratch fits a
	// 256-frame block), the prepare count does not move — so no device's state was rebuilt — and the note plays on.
	const uint64_t preparedAt4096 = chain->prepare_count(p);
	activateWith(256);
	CHECK(chain->max_frames(p) == 4096u);
	CHECK(chain->prepare_count(p) == preparedAt4096);
	int silentAfter = 0;
	for (int b = 0; b < 32; ++b) {
		hosted.run(256, events);
		float peak = 0.0f;
		for (uint32_t i = 0; i < 256; ++i) {
			finite = finite && std::isfinite(hosted.left()[i]) && std::isfinite(hosted.right()[i]);
			peak = std::max({ peak, std::fabs(hosted.left()[i]), std::fabs(hosted.right()[i]) });
		}
		silentAfter += peak == 0.0f ? 1 : 0; // no latency to refill: nothing was re-prepared
	}
	CHECK(finite);
	CHECK(silentAfter == 0);
	CHECK(rh.restarts == restarts);
}

//==================================================================================================
// A SWITCHED-OFF DEVICE STAYS OFF THROUGH EVERY HOST ROAD (a disabled Phaser/Flanger re-enabled itself
// each time the audio engine was turned off and on). The cause was Bitwig, not the plugin: its
// engine start runs a FRESH plugin-host process, which creates each instance and loads it from
// ~/.BitwigStudio/plugin-states/<uuid>.clap-preset before the first activate, and a "terminate process" engine
// stop kills the old process before Bitwig has fetched the edits made since its last fetch. Edits given time to be
// fetched survive. These cases hold the plugin's half: on every road a host can take, the enable comes back as it
// was left — what the card shows, what the engine runs, and (for the Phaser/Flanger) what is heard — and the
// enable click tells the host the state is dirty, which is what lets it refresh its copy.
//==================================================================================================

namespace {

/// `HostedPlugin`'s `beforeActivate`: load `bytes` (a `StateBytes*`) into the fresh instance.
void loadBeforeActivate(const clap_plugin* fresh, void* bytes) {
	REQUIRE(loadState(fresh, static_cast<StateBytes*>(bytes)));
}

/// THE ROADS A HOST TAKES BETWEEN AN EDIT AND THE NEXT BLOCK THAT RUNS IT.
enum class HostRoad {
	SameRate,      ///< stop, deactivate, activate at the rate already running, start (a restart request)
	NewRate,       ///< the same at 96 kHz
	FreshInstance, ///< save while deactivated, then a new instance loaded BEFORE its first activate (Bitwig's engine start)
	LiveLoad,      ///< save while deactivated, then a load into an instance already running
};
constexpr HostRoad kHostRoads[] = { HostRoad::SameRate, HostRoad::NewRate, HostRoad::FreshInstance, HostRoad::LiveLoad };

const char* roadName(HostRoad road) {
	switch (road) {
	case HostRoad::SameRate: return "same-rate reactivation";
	case HostRoad::NewRate: return "new-rate reactivation";
	case HostRoad::FreshInstance: return "fresh instance, load before activate";
	case HostRoad::LiveLoad: return "load into a running instance";
	}
	return "?";
}

/// Eight blocks, each followed by a main-thread visit, honouring any restart the plugin asks for.
void settle(RestartingHost& rh, HostedPlugin& h) {
	const EventList none;
	for (int b = 0; b < 8; ++b) {
		rh.block(h, none, kBlock);
		mainThreadVisit(h.plugin());
	}
}

/// `setUp(rh, h)` on a fresh instance (active, processing), then `road`, then `read(rh, h)` on the instance the
/// road leaves running — the same one for a reactivation, a new one for a load.
template <class SetUp, class Read>
void throughRoad(HostRoad road, SetUp&& setUp, Read&& read) {
	StateBytes saved;
	{
		RestartingHost rh;
		HostedPlugin h(&rh.host);
		setUp(rh, h);
		const clap_plugin* p = h.plugin();
		if (road == HostRoad::SameRate || road == HostRoad::NewRate) {
			rh.rate = road == HostRoad::NewRate ? 96000.0 : kSampleRate;
			RestartingHost::reactivate(p, rh.rate);
			settle(rh, h);
			read(rh, h);
			return;
		}
		// THE ENGINE GOES OFF FIRST: the host saves an instance it has already deactivated. Activated again after,
		// only so the destructor's stop/deactivate pair with a start/activate.
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(saveState(p, &saved));
		REQUIRE(p->activate(p, kSampleRate, 1, kMaxFrames));
		REQUIRE(p->start_processing(p));
	}
	RestartingHost rh;
	HostedPlugin h(&rh.host, road == HostRoad::FreshInstance ? loadBeforeActivate : nullptr, &saved);
	if (road == HostRoad::LiveLoad) {
		rh.block(h, EventList {}, kBlock);
		REQUIRE(loadState(h.plugin(), &saved));
	}
	settle(rh, h);
	read(rh, h);
}

/// The tags the lane accepts, as the plugin lists them.
std::vector<std::string> laneTags() {
	std::vector<std::string> tags;
	HostedPlugin lister;
	const clap_plugin_feathers_test_chain* chain = testChainOf(lister.plugin());
	char tag[64] {};
	for (uint32_t i = 0; chain->available_device(lister.plugin(), i, tag, sizeof(tag)); ++i) {
		tags.emplace_back(tag);
	}
	return tags;
}

/// What the card's square reads (`knob_value`) and what the engine runs (`engine_control`'s base).
struct EnableReading {
	double knob = -1.0;
	double engine = -1.0;
};
EnableReading enableOf(const clap_plugin* p, uint32_t at, const char* control) {
	EnableReading r;
	REQUIRE(testChainOf(p)->knob_value(p, at, control, &r.knob));
	REQUIRE(testChainOf(p)->engine_control(p, at, control, &r.engine, nullptr));
	return r;
}

/// The card's click: one gesture (`FxCard::toggleEnable`'s road), then the main-thread visit.
void clickTo(const clap_plugin* p, uint32_t at, const char* control, double value) {
	REQUIRE(testChainOf(p)->drag_control(p, at, control, &value, 1));
	mainThreadVisit(p);
}

/// A host that offers `clap_host_state` and counts `mark_dirty`.
struct DirtyHost {
	int dirty = 0;
	clap_host host = kHost;
	DirtyHost() {
		host.host_data = this;
		host.get_extension = [](const clap_host*, const char* id) -> const void* {
			static const clap_host_state kState {
				[](const clap_host* h) { ++static_cast<DirtyHost*>(h->host_data)->dirty; },
			};
			return id != nullptr && std::strcmp(id, CLAP_EXT_STATE) == 0 ? &kState : nullptr;
		};
	}
};

} // namespace

TEST_CASE("a switched-off device stays off through every host road — every lane device, the latent modes too: same- and new-rate reactivation, save-while-deactivated then load before activate, load into a running instance") {
	const std::vector<std::string> tags = laneTags();
	REQUIRE_FALSE(tags.empty());
	int checked = 0;
	for (const std::string& tag : tags) {
		const feathers::device::Descriptor* desc = feathers::deviceForTag(tag.c_str());
		REQUIRE(desc != nullptr);
		if (desc->traits.enableControl < 0) {
			continue;
		}
		const char* enable = desc->params[static_cast<size_t>(desc->traits.enableControl)].name;
		// THE LATENT MODES TOO: a device reporting a latency costs the host a restart of its own on the fresh roads.
		const char* latentControl = tag == feathers::fx::phaser::kTraits.tag ? "Phaser Mode"
		                            : tag == feathers::fx::eq::kTraits.tag    ? "Phase"
		                                                                      : nullptr;
		for (const bool latent : { false, true }) {
			if (latent && latentControl == nullptr) {
				continue;
			}
			for (const HostRoad road : kHostRoads) {
				CAPTURE(tag);
				CAPTURE(latent);
				CAPTURE(roadName(road));
				uint32_t at = UINT32_MAX;
				throughRoad(
				    road,
				    [&](RestartingHost& rh, HostedPlugin& h) {
					    const clap_plugin* p = h.plugin();
					    at = testChainOf(p)->add_device(p, tag.c_str());
					    REQUIRE(at != UINT32_MAX);
					    if (latent) {
						    clickTo(p, at, latentControl, 1.0);
					    }
					    RestartingHost::reactivate(p, kSampleRate); // ACTIVATE PROVISIONS the chain the bridge holds
					    settle(rh, h);
					    clickTo(p, at, enable, 0.0);
					    settle(rh, h);
					    const EnableReading before = enableOf(p, at, enable);
					    REQUIRE(before.knob == 0.0); // NOT VACUOUS: off before the road
					    REQUIRE(before.engine == 0.0);
				    },
				    [&](RestartingHost&, HostedPlugin& h) {
					    const EnableReading after = enableOf(h.plugin(), at, enable);
					    CHECK(after.knob == 0.0);
					    CHECK(after.engine == 0.0);
				    });
				++checked;
			}
		}
	}
	CHECK(checked >= 4 * 4); // the sweep reached devices, not an empty list
}

TEST_CASE("a switched-off Phaser/Flanger is heard as off through every host road — Phaser mode is the dry exactly, Flanger mode the dry exactly the reported latency late") {
	// BY THE SAMPLES, against an instance with no Phaser at all taken through the same road: a stored field that
	// reads Off while the device runs on would pass the record case above and fail here. NOT VACUOUS: the same
	// comparison with the device switched on differs.
	const EventList none;
	EventList noteOn;
	noteOn.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	enum class Placed { Off, On, None };
	for (const double mode : { 0.0, 1.0 }) {
		for (const HostRoad road : kHostRoads) {
			CAPTURE(mode);
			CAPTURE(roadName(road));
			const auto render = [&](Placed placed, uint32_t* latencyOut) {
				std::vector<float> out;
				throughRoad(
				    road,
				    [&](RestartingHost& rh, HostedPlugin& h) {
					    const clap_plugin* p = h.plugin();
					    if (placed != Placed::None) {
						    const uint32_t at = testChainOf(p)->add_device(p, feathers::fx::phaser::kTraits.tag);
						    REQUIRE(at != UINT32_MAX);
						    clickTo(p, at, "Phaser Mode", mode);
						    clickTo(p, at, "Phaser Enabled", placed == Placed::On ? 1.0 : 0.0);
					    }
					    RestartingHost::reactivate(p, kSampleRate);
					    settle(rh, h);
				    },
				    [&](RestartingHost& rh, HostedPlugin& h) {
					    const clap_plugin* p = h.plugin();
					    const auto* latency = static_cast<const clap_plugin_latency*>(p->get_extension(p, CLAP_EXT_LATENCY));
					    REQUIRE(latency != nullptr);
					    *latencyOut = latency->get(p);
					    for (int b = 0; b < 40; ++b) {
						    rh.block(h, b == 0 ? noteOn : none, kBlock);
						    mainThreadVisit(p);
						    out.insert(out.end(), h.left().begin(), h.left().begin() + kBlock);
					    }
				    });
				return out;
			};
			uint32_t lag = 0, lagOn = 0, lagNone = 0;
			const std::vector<float> off = render(Placed::Off, &lag);
			const std::vector<float> on = render(Placed::On, &lagOn);
			const std::vector<float> dry = render(Placed::None, &lagNone);
			// THE LATENCY IS THE MODE'S, ENABLED OR NOT (`latencySamplesFor`): 0 for the Phaser, 8 ms for the Flanger.
			const double rate = road == HostRoad::NewRate ? 96000.0 : kSampleRate;
			CHECK(lag == (mode == 0.0 ? 0u : static_cast<uint32_t>(std::lround(0.008 * rate))));
			CHECK(lagOn == lag);
			CHECK(lagNone == 0u);
			REQUIRE(off.size() == dry.size());
			REQUIRE(off.size() > lag);
			int differOff = 0, differOn = 0;
			double peak = 0.0;
			for (size_t i = lag; i < off.size(); ++i) {
				differOff += off[i] != dry[i - lag] ? 1 : 0;
				differOn += on[i] != dry[i - lag] ? 1 : 0;
				peak = std::max(peak, static_cast<double>(std::abs(dry[i - lag])));
			}
			CHECK(peak > 0.05); // a note sounded, so a match is a match of something
			CHECK(differOff == 0);
			CHECK(differOn > 0);
		}
	}
}

TEST_CASE("a Phaser switched off by the HOST, on the slot its card's click gave it, stays off through every host road") {
	// The enable reaches a host slot on its first click (T9); the host's write is the road a DAW's own panel,
	// automation or remote control takes. It anchors the slot, and the anchored value is what must come back.
	for (const HostRoad road : kHostRoads) {
		CAPTURE(roadName(road));
		uint32_t at = UINT32_MAX;
		throughRoad(
		    road,
		    [&](RestartingHost& rh, HostedPlugin& h) {
			    const clap_plugin* p = h.plugin();
			    at = testChainOf(p)->add_device(p, feathers::fx::phaser::kTraits.tag);
			    REQUIRE(at != UINT32_MAX);
			    RestartingHost::reactivate(p, kSampleRate);
			    clickTo(p, at, "Phaser Enabled", 0.0);
			    rh.block(h, EventList {}, kBlock);
			    clickTo(p, at, "Phaser Enabled", 1.0);
			    rh.block(h, EventList {}, kBlock);
			    REQUIRE(slotStateOf(p, 0) == stateNumber(feathers::hostslots::SlotState::Recent));
			    EventList write;
			    write.pushParam(0, kSlot0, 0.0);
			    rh.block(h, write, kBlock);
			    settle(rh, h);
			    REQUIRE(slotStateOf(p, 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
			    REQUIRE(enableOf(p, at, "Phaser Enabled").engine == 0.0);
		    },
		    [&](RestartingHost&, HostedPlugin& h) {
			    const clap_plugin* p = h.plugin();
			    const EnableReading after = enableOf(p, at, "Phaser Enabled");
			    CHECK(after.knob == 0.0);
			    CHECK(after.engine == 0.0);
			    CHECK(slotStateOf(p, 0) == stateNumber(feathers::hostslots::SlotState::Anchored));
			    CHECK(slotValue(p, 0) == 0.0);
		    });
	}
}

TEST_CASE("two instances in one process, loaded before activate in either order: the switched-off Phaser in one stays off beside a switched-on one in the other") {
	// Bitwig's engine start puts every instance of a project in ONE plugin-host process, so anything process-wide
	// is shared between them.
	const EventList none;
	for (const bool offFirst : { true, false }) {
		CAPTURE(offFirst);
		StateBytes savedOff, savedOn;
		uint32_t at = UINT32_MAX;
		{
			RestartingHost ra, rb;
			HostedPlugin a(&ra.host), b(&rb.host);
			at = testChainOf(a.plugin())->add_device(a.plugin(), feathers::fx::phaser::kTraits.tag);
			REQUIRE(testChainOf(b.plugin())->add_device(b.plugin(), feathers::fx::phaser::kTraits.tag) == at);
			clickTo(a.plugin(), at, "Phaser Enabled", 0.0);
			for (HostedPlugin* h : { &a, &b }) {
				RestartingHost::reactivate(h->plugin(), kSampleRate);
				h->run(kBlock, none);
				mainThreadVisit(h->plugin());
			}
			REQUIRE(saveState(a.plugin(), &savedOff));
			REQUIRE(saveState(b.plugin(), &savedOn));
		}
		RestartingHost ra, rb;
		std::optional<HostedPlugin> off, on;
		if (offFirst) {
			off.emplace(&ra.host, loadBeforeActivate, &savedOff);
			on.emplace(&rb.host, loadBeforeActivate, &savedOn);
		}
		else {
			on.emplace(&rb.host, loadBeforeActivate, &savedOn);
			off.emplace(&ra.host, loadBeforeActivate, &savedOff);
		}
		settle(ra, *off);
		settle(rb, *on);
		const EnableReading o = enableOf(off->plugin(), at, "Phaser Enabled");
		const EnableReading n = enableOf(on->plugin(), at, "Phaser Enabled");
		CHECK(o.knob == 0.0);
		CHECK(o.engine == 0.0);
		CHECK(n.knob == 1.0);
		CHECK(n.engine == 1.0);
	}
}

TEST_CASE("switching a lane device off tells the host its state is dirty — every device with an enable") {
	// THE HOST'S COPY IS ONLY AS FRESH AS ITS LAST FETCH, and `mark_dirty` is what asks for one: a host that
	// restarts from a copy fetched before the click brings the device back on (the Bitwig report above).
	int checked = 0;
	for (const std::string& tag : laneTags()) {
		const feathers::device::Descriptor* desc = feathers::deviceForTag(tag.c_str());
		REQUIRE(desc != nullptr);
		if (desc->traits.enableControl < 0) {
			continue;
		}
		CAPTURE(tag);
		DirtyHost dh;
		HostedPlugin h(&dh.host);
		const clap_plugin* p = h.plugin();
		const uint32_t at = testChainOf(p)->add_device(p, tag.c_str());
		REQUIRE(at != UINT32_MAX);
		const EventList none;
		mainThreadVisit(p);
		h.run(kBlock, none);
		mainThreadVisit(p);
		const int before = dh.dirty;
		clickTo(p, at, desc->params[static_cast<size_t>(desc->traits.enableControl)].name, 0.0);
		CHECK(dh.dirty > before);
		++checked;
	}
	CHECK(checked >= 4);
}

TEST_CASE("spectral state: the block round-trips the master switch and every sound's curve, skips a key it does not know, and ignores a known key at the wrong length") {
	feathers::SpectralState state;
	state.on = false;
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		state.curve[slot].on = slot % 2 == 0;
		for (size_t b = 0; b < feathers::spectral::kCurveBands; ++b) {
			state.curve[slot].gain[b] = 0.25f + 0.03f * static_cast<float>(b) + 0.1f * static_cast<float>(slot);
		}
	}
	StateBytes bytes;
	clap_ostream out = bytes.out();
	REQUIRE(feathers::saveSpectralState(&out, state));
	clap_istream in = bytes.in();
	feathers::SpectralState read;
	bool present = false;
	REQUIRE(feathers::loadSpectralState(&in, &read, &present));
	CHECK(present);
	CHECK(bytes.read == bytes.bytes.size());
	CHECK(read == state);

	// HAND-BUILT: the tag, four entries — a key from some other build (skipped), `spectral_on` at the WRONG length (its
	// default stands), sound C's curve switch as written, and sound D's gains with three MALFORMED bands (a NaN, a −1 and
	// a 9: pinned, not refused — NaN to unity, the others into [0, kSpectralCurveGainMax]). Then a byte past the block,
	// which nothing may have read.
	const auto appendKey = [](std::vector<uint8_t>& raw, const std::string& key, std::initializer_list<float> values) {
		appendFloat(raw, static_cast<float>(key.size()));
		raw.insert(raw.end(), key.begin(), key.end());
		appendFloat(raw, static_cast<float>(values.size()));
		for (const float v : values) {
			appendFloat(raw, v);
		}
	};
	std::vector<uint8_t> raw = { 'F', 'S', 'S', 'D' };
	appendFloat(raw, 4.0f);
	appendKey(raw, "spectral.vowel.depth", { 0.5f, 0.25f });
	appendKey(raw, feathers::kSpectralOnKey, { 0.0f, 0.0f });
	appendKey(raw, feathers::kSpectralCurveOnKeys[2], { 1.0f });
	{
		const std::string key = feathers::kSpectralCurveGainKeys[3];
		appendFloat(raw, static_cast<float>(key.size()));
		raw.insert(raw.end(), key.begin(), key.end());
		appendFloat(raw, static_cast<float>(feathers::spectral::kCurveBands));
		for (size_t b = 0; b < feathers::spectral::kCurveBands; ++b) {
			appendFloat(raw, b == 0 ? std::numeric_limits<float>::quiet_NaN() : b == 1 ? -1.0f : b == 2 ? 9.0f : 0.5f);
		}
	}
	raw.push_back(0xAB);
	StateBytes hand;
	hand.bytes = raw;
	clap_istream handIn = hand.in();
	feathers::SpectralState got;
	REQUIRE(feathers::loadSpectralState(&handIn, &got, &present));
	CHECK(present);
	CHECK(got.on); // the wrong-length entry left the default
	CHECK(got.curve[2].on);
	CHECK_FALSE(got.curve[0].on);
	CHECK(got.curve[2].gain == feathers::spectral::unityCurve());
	CHECK(got.curve[3].gain[0] == 1.0f);                             // NaN: unity
	CHECK(got.curve[3].gain[1] == 0.0f);                             // −1: the floor
	CHECK(got.curve[3].gain[2] == feathers::kSpectralCurveGainMax); // 9: the shared bound
	CHECK(got.curve[3].gain[3] == 0.5f);                             // the rest as written
	CHECK(hand.read == raw.size() - 1); // and the block ended where it said it would
}

TEST_CASE("OE: the spectral block carries the engine vocoder's held envelope under vocode.freeze — written only when given, read back word for word") {
	feathers::VocodeFreezeBlock held;
	held.holding = true;
	held.binHz = 48000.0 / 480.0;
	held.frozenHz = 196.0;
	for (size_t g = 0; g < held.grid.size(); ++g) {
		held.grid[g] = -40.0f - 0.25f * static_cast<float>(g);
	}
	std::array<float, feathers::kVocodeFreezeFloats> words {};
	feathers::writeVocodeFreeze(held, words.data());
	// WITH IT: the words come back as written, and decode to the block.
	{
		StateBytes bytes;
		clap_ostream out = bytes.out();
		REQUIRE(feathers::saveSpectralState(&out, feathers::SpectralState {}, words.data()));
		clap_istream in = bytes.in();
		feathers::SpectralState read;
		bool present = false;
		REQUIRE(feathers::loadSpectralState(&in, &read, &present));
		CHECK(bytes.read == bytes.bytes.size());
		CHECK(read.vocodeFreeze == words);
		feathers::VocodeFreezeBlock back;
		REQUIRE(feathers::readVocodeFreezeBlock(read.vocodeFreeze.data(), read.vocodeFreeze.size(), &back));
		CHECK(back.holding);
		CHECK(back.binHz == held.binHz);
		CHECK(back.frozenHz == held.frozenHz);
		CHECK(back.grid == held.grid);
	}
	// WITHOUT IT: no key, and a load reads the empty words, which decode to nothing.
	{
		StateBytes bytes;
		clap_ostream out = bytes.out();
		REQUIRE(feathers::saveSpectralState(&out, feathers::SpectralState {}));
		clap_istream in = bytes.in();
		feathers::SpectralState read;
		bool present = false;
		REQUIRE(feathers::loadSpectralState(&in, &read, &present));
		feathers::VocodeFreezeBlock back;
		CHECK_FALSE(feathers::readVocodeFreezeBlock(read.vocodeFreeze.data(), read.vocodeFreeze.size(), &back));
	}
	// A DAMAGED WORD (not a whole 16-bit number) is refused.
	std::array<float, feathers::kVocodeFreezeFloats> bad = words;
	bad[7] = 0.5f;
	feathers::VocodeFreezeBlock back;
	CHECK_FALSE(feathers::readVocodeFreezeBlock(bad.data(), bad.size(), &back));
}

TEST_CASE("spectral state: the master switch and each sound's drawn curve ride the plugin's own save and load, by value") {
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const std::vector<uint8_t> payload = payloadOf(saved.bytes);
	const size_t tail = blockAt(payload, "FSSD", blockAt(payload, "FSMB"));
	REQUIRE(tail < payload.size());
	{
		// A FRESH INSTANCE SAVES THE DEFAULTS: the drawer on, the curve flat and off.
		StateBytes slice;
		slice.bytes.assign(payload.begin() + static_cast<std::ptrdiff_t>(tail), payload.end());
		clap_istream in = slice.in();
		feathers::SpectralState read;
		bool present = false;
		REQUIRE(feathers::loadSpectralState(&in, &read, &present));
		CHECK(present);
		CHECK(read == feathers::SpectralState {});
	}
	// THE BLOCK REPLACED: the drawer OFF and a drawn curve, loaded, then saved again — read back by value.
	feathers::SpectralState drawn;
	drawn.on = false;
	drawn.curve[0].on = true;
	drawn.curve[0].gain[0] = 0.5f;
	drawn.curve[5].gain[17] = 2.0f;
	StateBytes with;
	with.bytes.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream withOut = with.out();
	REQUIRE(feathers::saveSpectralState(&withOut, drawn));
	REQUIRE(feathers::saveTerrainLikes(&withOut, feathers::TerrainLikes {})); // and the likes after it (kStateVersion 52)
	REQUIRE(feathers::savePresetIdentity(&withOut, feathers::PresetIdentity {})); // and the preset's name after it (kStateVersion 53)
	REQUIRE(feathers::saveDrumLanes(&withOut, feathers::DrumLanes {})); // and the pad lanes after it (kStateVersion 54)
	seal(with);
	clap_istream withIn = with.in();
	REQUIRE(state->load(host.plugin(), &withIn));
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(state->save(host.plugin(), &resavedOut));
	const std::vector<uint8_t> again = payloadOf(resaved.bytes);
	StateBytes slice;
	slice.bytes.assign(again.begin() + static_cast<std::ptrdiff_t>(blockAt(again, "FSSD", blockAt(again, "FSMB"))),
	                   again.end());
	clap_istream in = slice.in();
	feathers::SpectralState read;
	bool present = false;
	REQUIRE(feathers::loadSpectralState(&in, &read, &present));
	CHECK(present);
	CHECK(read == drawn);

	SUBCASE("a v50 patch loads with the spectral drawer at its defaults") {
		// THE SAVE BEFORE THE DRAWER: the same payload with the spectral block cut off, version byte 50, sealed. Loaded over
		// the instance holding the drawn state above, it puts the defaults back — what a patch from before the drawer was.
		StateBytes v50;
		v50.bytes.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(tail));
		v50.bytes[0] = feathers::kStateVersionTrailered;
		seal(v50);
		clap_istream v50In = v50.in();
		REQUIRE(state->load(host.plugin(), &v50In));
		StateBytes after;
		clap_ostream afterOut = after.out();
		REQUIRE(state->save(host.plugin(), &afterOut));
		const std::vector<uint8_t> p = payloadOf(after.bytes);
		StateBytes s;
		s.bytes.assign(p.begin() + static_cast<std::ptrdiff_t>(blockAt(p, "FSSD", blockAt(p, "FSMB"))), p.end());
		clap_istream sIn = s.in();
		feathers::SpectralState back;
		REQUIRE(feathers::loadSpectralState(&sIn, &back, &present));
		CHECK(back == feathers::SpectralState {});
	}
	SUBCASE("and a v51 patch without the block is refused, as every stream missing a block it should carry is") {
		StateBytes cut;
		cut.bytes.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(tail));
		seal(cut);
		clap_istream cutIn = cut.in();
		CHECK_FALSE(state->load(host.plugin(), &cutIn));
	}
}

TEST_CASE("stance wire: each stance's knobs and drawn curve round-trip, and a record short of them live-fills them") {
	// THE FSV RECORD GREW BY THE SOUND'S KNOBS, ITS CURVE'S SWITCH AND ITS 64 BANDS (stances carry them),
	// appended after every older member: a stance round-trips them by value, and a record that ENDS before them (a stream
	// from before the growth) takes the slot's live values for them — the migration fill's rule — and keeps every older
	// member exactly as it read it.
	feathers::SoundVariations variations;
	feathers::SoundVariations::List& list = variations.sound[0];
	list.count = 2;
	list.active = 1;
	for (size_t at = 0; at < 2; ++at) {
		feathers::RideAlongValues& v = list.values[at];
		v.width = 0.2 + 0.3 * static_cast<double>(at);
		v.zone = 100.0 + 50.0 * static_cast<double>(at);
		for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
			v.knobs[k] = 0.01 * static_cast<double>(k + 1) + static_cast<double>(at);
		}
		v.curve.on = at == 1;
		v.curve.gain[at + 3] = 0.25f;
	}
	std::array<feathers::AnchorPoint, feathers::kMaxSounds> places {};
	StateBytes bytes;
	clap_ostream out = bytes.out();
	REQUIRE(feathers::saveSoundVariations(&out, variations, places));
	clap_istream in = bytes.in();
	feathers::SoundVariations read;
	bool present = false;
	size_t valuesRead = 0;
	REQUIRE(feathers::loadSoundVariations(&in, &read, &present, &valuesRead));
	CHECK(present);
	CHECK(valuesRead == feathers::kRideAlongWireValues);
	CHECK(read == variations);

	// A RECORD SHORT OF THE NEW MEMBERS: the loader says how many it read; everything from there is the live fill.
	feathers::SoundVariations shortRecord = variations;
	std::array<feathers::RideAlongValues, feathers::kMaxSounds> live {};
	for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
		live[0].knobs[k] = 0.5;
	}
	live[0].curve.on = true;
	live[0].curve.gain.fill(0.75f);
	live[0].width = 0.99; // an OLDER member: the fill must not touch it
	feathers::fillAbsentStanceValues(shortRecord, live, feathers::kRideAlongCurveWire);
	for (size_t at = 0; at < 2; ++at) {
		CAPTURE(at);
		CHECK(shortRecord.sound[0].values[at].knobs == live[0].knobs);
		CHECK(shortRecord.sound[0].values[at].curve == live[0].curve);
		CHECK(shortRecord.sound[0].values[at].width == variations.sound[0].values[at].width);
		CHECK(shortRecord.sound[0].values[at].zone == variations.sound[0].values[at].zone);
	}
}

TEST_CASE("stance wire: a plugin load of stances short of the curve and the knobs fills them from the live params and the FLAT curve") {
	// THE PLUGIN-LEVEL FILL (FeathersPlugin's variations commit): a record that ends before the curve members takes the live
	// params for the knobs and the FLAT curve — not the bridge's, which still holds the PREVIOUS patch's curves because the
	// `spectral` block commits after this one. Driven through the real state road: a patch loaded first gives sound B a drawn
	// curve, then a patch whose FSV2 record is truncated at kRideAlongCurveWire (and whose own spectral block draws B a
	// curve too) is loaded; the banked stance must read back flat with its knobs at the live params.
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const std::vector<uint8_t> payload = payloadOf(saved.bytes);
	const size_t fsv = blockAt(payload, "FSV2");
	const size_t hues = blockAt(payload, "FSH1", fsv);
	const size_t spectralAt = blockAt(payload, "FSSD", blockAt(payload, "FSMB"));
	REQUIRE(fsv < hues);
	REQUIRE(hues < spectralAt);
	REQUIRE(spectralAt < payload.size());
	feathers::SpectralState drawn;
	drawn.curve[1].on = true;
	drawn.curve[1].gain.fill(2.5f);
	const auto spectralBytes = [&](const feathers::SpectralState& s) {
		StateBytes b;
		clap_ostream o = b.out();
		REQUIRE(feathers::saveSpectralState(&o, s));
		REQUIRE(feathers::saveTerrainLikes(&o, feathers::TerrainLikes {})); // the likes after it (kStateVersion 52)
		REQUIRE(feathers::savePresetIdentity(&o, feathers::PresetIdentity {})); // and the preset's name after it (kStateVersion 53)
		REQUIRE(feathers::saveDrumLanes(&o, feathers::DrumLanes {})); // and the pad lanes after it (kStateVersion 54)
		return b.bytes;
	};
	// 1. A PATCH THAT DRAWS SOUND B A CURVE, so the bridge holds a non-flat one when the short record is committed.
	{
		StateBytes first;
		first.bytes.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(spectralAt));
		const std::vector<uint8_t> s = spectralBytes(drawn);
		first.bytes.insert(first.bytes.end(), s.begin(), s.end());
		seal(first);
		clap_istream in = first.in();
		REQUIRE(state->load(host.plugin(), &in));
	}
	// 2. THE SHORT RECORD: FSV2, the three counts with perStance = kRideAlongCurveWire, sound B two stances (the banked
	//    one stored, the active one the live box), each record truncated before the curve.
	const auto appendDouble = [](std::vector<uint8_t>& raw, double v) {
		uint64_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		for (int i = 0; i < 8; ++i) {
			raw.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffU));
		}
	};
	feathers::RideAlongValues banked;
	banked.width = 0.33;
	banked.zone = 217.0;
	banked.offset = 0.05;
	feathers::RideAlongValues active;
	active.zone = feathers::findParam(static_cast<clap_id>(feathers::soundParam(1, feathers::SoundField::Zone)))->def;
	std::vector<uint8_t> fsvBytes = { 'F', 'S', 'V', '2' };
	appendFloat(fsvBytes, static_cast<float>(feathers::kMaxSounds));
	appendFloat(fsvBytes, static_cast<float>(feathers::kMaxVariations));
	appendFloat(fsvBytes, static_cast<float>(feathers::kRideAlongCurveWire));
	for (size_t slot = 0; slot < feathers::kMaxSounds; ++slot) {
		const bool b = slot == 1;
		appendFloat(fsvBytes, b ? 2.0f : 0.0f); // count
		appendFloat(fsvBytes, b ? 1.0f : 0.0f); // active
		if (!b) {
			continue;
		}
		for (const feathers::RideAlongValues* v : { &banked, &active }) {
			appendFloat(fsvBytes, 0.0f); // the dead place slots
			appendFloat(fsvBytes, 0.0f);
			for (size_t member = 0; member < feathers::kRideAlongCurveWire; ++member) {
				appendDouble(fsvBytes, feathers::rideAlongWireValue(*v, member));
			}
		}
	}
	StateBytes shortPatch;
	shortPatch.bytes.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(fsv));
	shortPatch.bytes.insert(shortPatch.bytes.end(), fsvBytes.begin(), fsvBytes.end());
	shortPatch.bytes.insert(shortPatch.bytes.end(), payload.begin() + static_cast<std::ptrdiff_t>(hues),
	                        payload.begin() + static_cast<std::ptrdiff_t>(spectralAt));
	const std::vector<uint8_t> s = spectralBytes(drawn);
	shortPatch.bytes.insert(shortPatch.bytes.end(), s.begin(), s.end());
	seal(shortPatch);
	clap_istream shortIn = shortPatch.in();
	REQUIRE(state->load(host.plugin(), &shortIn));

	// 3. READ IT BACK through a resave: the banked stance's curve is FLAT, its knobs are the live params (B's defaults), and
	//    the members it did carry are what it carried.
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(state->save(host.plugin(), &resavedOut));
	StateBytes slice = from(payloadOf(resaved.bytes), "FSV2");
	clap_istream in = slice.in();
	feathers::SoundVariations read;
	bool present = false;
	REQUIRE(feathers::loadSoundVariations(&in, &read, &present));
	REQUIRE(present);
	REQUIRE(read.sound[1].count == 2);
	const feathers::RideAlongValues& got = read.sound[1].values[0];
	CHECK(got.curve == feathers::spectral::DrawnCurve {});
	for (size_t k = 0; k < feathers::kPerSoundKnobCount; ++k) {
		CAPTURE(k);
		CHECK(got.knobs[k] == feathers::findParam(static_cast<clap_id>(feathers::perSoundKnobParam(1, k)))->def);
	}
	CHECK(got.width == doctest::Approx(0.33));
	CHECK(got.zone == doctest::Approx(217.0));
}

TEST_CASE("backlog LG: the chrome hue is never in the patch — a state saved at +120 degrees is the state saved at 0, "
          "byte for byte") {
	// THE HUE IS A DESK PREFERENCE (editor prefs, `chrome_hue_prefs.h`): two people sharing a patch do not share
	// a taste in colour. Asserted on the bytes a host stores, with the offset in force in this process.
	struct Reset {
		~Reset() { (void)feathers::ui::setChromeHueOffset(0.0); }
	} reset;
	REQUIRE(feathers::ui::chromeHueOffset() == 0.0);
	HostedPlugin h;
	StateBytes atZero;
	REQUIRE(saveState(h.plugin(), &atZero));
	(void)feathers::ui::setChromeHueOffset(120.0);
	REQUIRE(feathers::ui::chromeHueOffset() == 120.0);
	StateBytes turned;
	REQUIRE(saveState(h.plugin(), &turned));
	CHECK(turned.bytes == atZero.bytes);
	CHECK_FALSE(containsTag(turned.bytes, "ui.hue"));
}

TEST_CASE("a per-voice Spectral keeps the plugin awake while a voice's delayed tail drains") {
	// THE EQ CASE'S PROMISE, PER VOICE: at Fine a Poly Spectral holds each voice's audio 40 ms, and the Synth holds the voice
	// that long after its level closes. A SLEEP before the tail is out is a broken promise; a voice freed at level-close cuts
	// the last 40 ms instead. So the tail must END WHERE THE BUS ONE DOES (the same device on the mix, the same latency).
	const auto render = [](bool poly, int& lastSounding, int& soundingAfterSleep, bool& slept) {
		HostedPlugin host;
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		const uint32_t slot = chain->add_device(host.plugin(), feathers::fx::spectral::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		REQUIRE(chain->set_control(host.plugin(), slot, "Spectral Poly", poly ? 1.0 : 0.0));
		REQUIRE(chain->set_control(host.plugin(), slot, "Spectral Resolution", 2.0));
		const clap_plugin* p = host.plugin();
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));
		constexpr uint32_t kFrames = 64;
		EventList events;
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		host.run(kFrames, events);
		events.clear();
		events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
		host.run(kFrames, events);
		events.clear();
		lastSounding = -1;
		soundingAfterSleep = 0;
		slept = false;
		for (int b = 0; b < 4 * 48000 / static_cast<int>(kFrames); ++b) {
			const clap_process_status status = host.run(kFrames, events);
			const bool sounding = peakOf(host.left(), kFrames) > 0.0f;
			if (sounding) {
				lastSounding = b;
			}
			if (slept) {
				soundingAfterSleep += sounding ? 1 : 0;
			}
			else {
				slept = status == CLAP_PROCESS_SLEEP;
			}
		}
	};
	int lastPoly = 0, lastBus = 0, afterPoly = 0, afterBus = 0;
	bool sleptPoly = false, sleptBus = false;
	render(true, lastPoly, afterPoly, sleptPoly);
	render(false, lastBus, afterBus, sleptBus);
	CAPTURE(lastPoly);
	CAPTURE(lastBus);
	CHECK(lastBus > 0);
	CHECK(std::abs(lastPoly - lastBus) <= 1); // the whole tail came out, per voice as on the bus
	CHECK(afterPoly == 0);                     // nothing after a SLEEP
	CHECK(sleptPoly);
}

TEST_CASE("a held note keeps sounding across the restart a per-voice Spectral's Resolution asks for") {
	RestartingHost rh;
	HostedPlugin hosted(&rh.host);
	const clap_plugin* p = hosted.plugin();
	const auto* chain = testChainOf(p);
	const uint32_t slot = chain->add_device(p, feathers::fx::spectral::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(p, slot, "Spectral Poly", 1.0));
	RestartingHost::reactivate(p, kSampleRate);
	constexpr uint32_t kFrames = 256;
	EventList events;
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	rh.block(hosted, events, kFrames);
	events.clear();
	float before = 0.0f;
	for (int b = 0; b < 40; ++b) {
		before = std::max(before, rh.block(hosted, events, kFrames));
	}
	REQUIRE(before > 0.05f);
	const int restartsBefore = rh.restarts;
	REQUIRE(chain->set_control(p, slot, "Spectral Resolution", 2.0)); // Balanced to Fine: the latency moves
	float after = 0.0f;
	int silentLate = 0;
	for (int b = 0; b < 80; ++b) {
		const float peak = rh.block(hosted, events, kFrames);
		if (b >= 40) {
			after = std::max(after, peak);
			silentLate += peak == 0.0f ? 1 : 0;
		}
	}
	CHECK(rh.restarts > restartsBefore);
	CHECK(rh.honoured == rh.restarts);
	CHECK(silentLate == 0);
	CHECK(after > 0.05f);
}

TEST_CASE("Feathers' sidechain port as the host reads it: one non-main stereo input named Sidechain, the output still main") {
	HostedPlugin host;
	const auto* ports = static_cast<const clap_plugin_audio_ports*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_AUDIO_PORTS));
	REQUIRE(ports != nullptr);
	REQUIRE(ports->count(host.plugin(), true) == 1);
	REQUIRE(ports->count(host.plugin(), false) == 1);
	clap_audio_port_info in {};
	REQUIRE(ports->get(host.plugin(), 0, true, &in));
	CHECK(std::string(in.name) == "Sidechain");
	CHECK((in.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0);
	CHECK(in.channel_count == 2);
	CHECK(std::string(in.port_type) == CLAP_PORT_STEREO);
	CHECK(in.in_place_pair == CLAP_INVALID_ID);
	clap_audio_port_info out {};
	REQUIRE(ports->get(host.plugin(), 0, false, &out));
	CHECK((out.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0);
	CHECK(std::string(out.name) == "Main");
}

TEST_CASE("a sidechain fed through Feathers' process() reads as sounding on the fx lane's Spectral, and keeps Feathers awake with no note playing") {
	HostedPlugin host;
	const clap_plugin* p = host.plugin();
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(chain->device_reading != nullptr);
	const uint32_t slot = chain->add_device(p, feathers::fx::spectral::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(p, slot, "Spectral Vocode", 1.0));
	// ACTIVATE PROVISIONS THE CHAIN IT IS HANDED (no editor in this case to provision it).
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));

	constexpr uint32_t kFrames = 256;
	std::vector<float> sideL(kFrames), sideR(kFrames);
	float* sidePtr[2] { sideL.data(), sideR.data() };
	clap_audio_buffer side {};
	side.data32 = sidePtr;
	side.channel_count = 2;
	host.setInputs(&side, 1);
	uint32_t seed = 1;
	EventList events;
	int slept = 0;
	for (int b = 0; b < 64; ++b) {
		for (uint32_t i = 0; i < kFrames; ++i) {
			seed = seed * 1664525u + 1013904223u;
			sideL[i] = sideR[i] = 0.25f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
		}
		slept += host.run(kFrames, events) == CLAP_PROCESS_SLEEP ? 1 : 0;
	}
	float connected = -1.0f;
	CHECK(chain->device_reading(p, slot, static_cast<uint32_t>(feathers::fx::spectral::kReadingSideSounding), &connected));
	CHECK(connected == 1.0f);
	CAPTURE(slept);
	CHECK(slept == 0);
}

namespace {

/// A CHAIN'S BODY WITH A SPECTRAL INSIDE, its Vocode on — built through the Chain's own container ops, as its panel leaves it.
std::vector<uint8_t> chainBodyWithVocoder() {
	namespace sp = feathers::fx::spectral;
	const feathers::device::Descriptor& chain = feathers::rack::kDeviceDescriptor;
	const feathers::device::AudioOps& audio = *chain.audio;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	chain.container->adopt(mem, feathers::hostCatalogue());
	audio.prepare(mem, 48000.0, 4096);
	feathers::rack::RackBridge* inner = feathers::rack::bridgeOf(mem);
	REQUIRE(inner != nullptr);
	const feathers::DeviceInstanceId id = inner->addDevice(sp::kTraits.tag);
	REQUIRE(id != feathers::kNoInstance);
	inner->setField(id, static_cast<size_t>(sp::Param::VocodeOn), 1.0f);
	std::vector<uint8_t> body;
	const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                                       auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                                       out->insert(out->end(), data, data + bytes);
	                                       } };
	REQUIRE(chain.container->saveState(mem, sink));
	audio.destruct(mem);
	::operator delete(mem, std::align_val_t { audio.stateAlign });
	return body;
}

/// FEATHERS PLAYING A NOTE through a vocoder in its fx lane (directly, or inside a Chain), the sidechain silent or sounding: the
/// output's energy over the last second. A vocoder hears the sidechain only if it reaches it, so silence against sound is the
/// sidechain's road, end to end through process().
double vocodedEnergy(bool inChain, bool sounding) {
	HostedPlugin host;
	const clap_plugin* p = host.plugin();
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(p, inChain ? feathers::rack::kDeviceDescriptor.traits.tag
	                                                    : feathers::fx::spectral::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	if (!inChain) {
		REQUIRE(chain->set_control(p, slot, "Spectral Vocode", 1.0));
	}
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));
	constexpr uint32_t kFrames = 256;
	EventList events;
	host.run(kFrames, events);
	if (inChain) {
		const std::vector<uint8_t> body = chainBodyWithVocoder();
		REQUIRE(chain->edit_device_body(p, slot, body.data(), body.size()));
	}
	std::vector<float> sideL(kFrames), sideR(kFrames);
	float* sidePtr[2] { sideL.data(), sideR.data() };
	clap_audio_buffer side {};
	side.data32 = sidePtr;
	side.channel_count = 2;
	host.setInputs(&side, 1);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 48, 1, 1.0);
	uint32_t seed = 5;
	double energy = 0.0;
	const int blocks = 2 * 48000 / static_cast<int>(kFrames);
	for (int b = 0; b < blocks; ++b) {
		for (uint32_t i = 0; i < kFrames; ++i) {
			seed = seed * 1664525u + 1013904223u;
			sideL[i] = sideR[i] = sounding ? 0.25f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) : 0.0f;
		}
		host.run(kFrames, events);
		events.clear();
		if (b >= blocks / 2) {
			for (uint32_t i = 0; i < kFrames; ++i) {
				energy += static_cast<double>(host.left()[i]) * host.left()[i] + static_cast<double>(host.right()[i]) * host.right()[i];
			}
		}
	}
	host.setInputs(nullptr, 0);
	return energy;
}

} // namespace

TEST_CASE("the sidechain reaches a vocoder in Feathers' fx lane and inside a Chain there, through process()") {
	// A HELD NOTE IS THE CARRIER. A silent sidechain gives EXACT silence (Vocode never falls back to the dry note); a sounding one
	// gives sound — which it can only do if the sidechain got to the device, in the lane and one level down.
	for (const bool inChain : { false, true }) {
		CAPTURE(inChain);
		CHECK(vocodedEnergy(inChain, false) == 0.0);
		CHECK(vocodedEnergy(inChain, true) > 1e-3);
	}
}

namespace {

/// A RACK'S BODY (backlog NO): chain 1 empty, chain 2 a Featherverb at full wet with a half decay — built through the
/// Rack's own container ops, as its window leaves it.
std::vector<uint8_t> rackBodyWithReverb() {
	namespace pr = feathers::rack::parallel;
	namespace fv = feathers::fx::featherverb;
	const feathers::device::AudioOps& audio = *pr::kDescriptor.audio;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	pr::kDescriptor.container->adopt(mem, feathers::hostCatalogue());
	audio.prepare(mem, 48000.0, 4096);
	pr::Rack& rack = *pr::rackOf(mem);
	REQUIRE(rack.addChain() == 1);
	feathers::rack::RackBridge* chain = rack.chainAt(1)->bridge();
	REQUIRE(chain != nullptr);
	const feathers::DeviceInstanceId reverb = chain->addDevice(fv::kTraits.tag);
	REQUIRE(reverb != feathers::kNoInstance);
	chain->setField(reverb, static_cast<size_t>(fv::Param::Mix), 1.0f);
	chain->setField(reverb, static_cast<size_t>(fv::Param::Decay), 0.5f);
	std::vector<uint8_t> body;
	const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                                       auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                                       out->insert(out->end(), data, data + bytes);
	                                       } };
	REQUIRE(pr::kDescriptor.container->saveState(mem, sink));
	audio.destruct(mem);
	::operator delete(mem, std::align_val_t { audio.stateAlign });
	return body;
}

} // namespace

TEST_CASE("NO: a reverb in one of a Rack's chains keeps the plugin awake — SLEEP is decided by the rack's SUM, never while a chain still rings") {
	// THE SLEEP RULE (CLAUDE.md), copied from the reverb case for the Rack: an empty sibling chain is silent the moment
	// the note ends, the reverb's chain is not, and the output — the sum — decides. Measured against the same rack with
	// its chains EMPTY, so the voice's own release cannot be what kept it awake.
	const auto run = [](bool withReverb, int& blocks, int& soundingAfterSleep) {
		HostedPlugin host;
		const clap_plugin* p = host.plugin();
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		const uint32_t slot = chain->add_device(p, feathers::rack::parallel::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));
		constexpr uint32_t kFrames = 256;
		EventList events;
		host.run(kFrames, events);
		if (withReverb) {
			const std::vector<uint8_t> body = rackBodyWithReverb();
			REQUIRE(chain->edit_device_body(p, slot, body.data(), body.size()));
		}
		host.run(kFrames, events);
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		host.run(kFrames, events);
		events.clear();
		for (int b = 0; b < 20; ++b) {
			host.run(kFrames, events);
		}
		events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
		host.run(kFrames, events);
		events.clear();
		blocks = 0;
		soundingAfterSleep = 0;
		bool slept = false;
		while (blocks < 30 * 48000 / static_cast<int>(kFrames)) {
			const clap_process_status status = host.run(kFrames, events);
			++blocks;
			if (status == CLAP_PROCESS_SLEEP) {
				slept = true;
				host.run(kFrames, events);
				soundingAfterSleep += peakOf(host.left(), kFrames) > 0.0f ? 1 : 0;
				break;
			}
		}
		return slept;
	};
	int emptyBlocks = 0;
	int emptySounding = 0;
	int reverbBlocks = 0;
	int reverbSounding = 0;
	const bool emptySlept = run(false, emptyBlocks, emptySounding);
	const bool reverbSlept = run(true, reverbBlocks, reverbSounding);
	CAPTURE(emptyBlocks);
	CAPTURE(reverbBlocks);
	CHECK(emptySlept);
	CHECK(reverbSlept); // a decaying reverb ends in exact zero, so the plugin does sleep in the end
	CHECK(emptySounding == 0);
	CHECK(reverbSounding == 0);
	// THE REVERB'S CHAIN IS WHAT KEPT IT AWAKE: a second and more past the same rack with nothing in it.
	CHECK(reverbBlocks > emptyBlocks + 48000 / 256);
}

TEST_CASE("NO: a delay in one of a Rack's chains keeps the plugin awake through the SILENT gap before its repeat — the rack's holdsTail asks every chain that is heard") {
	// THE CASE THE REVERB ONE CANNOT MAKE: a reverb's output never goes silent while it rings, so the silent-block half of
	// the rule keeps the plugin awake whatever the rack says. A wet-only delay with a long time leaves seconds of EXACT
	// silence after the voice ends and before the repeat, and only the rack's `holdsTail` can say a repeat is coming.
	// The empty sibling chain is MUTED, so the gap is silent in the sum too.
	namespace pr = feathers::rack::parallel;
	namespace dl = feathers::fx::delay;
	const auto body = [] {
		const feathers::device::AudioOps& audio = *pr::kDescriptor.audio;
		void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
		audio.construct(mem);
		pr::kDescriptor.container->adopt(mem, feathers::hostCatalogue());
		audio.prepare(mem, 48000.0, 4096);
		pr::Rack& rack = *pr::rackOf(mem);
		REQUIRE(rack.addChain() == 1);
		rack.setMuted(0, true);
		feathers::rack::RackBridge* chain = rack.chainAt(1)->bridge();
		const feathers::DeviceInstanceId delay = chain->addDevice(dl::kTraits.tag);
		REQUIRE(delay != feathers::kNoInstance);
		chain->setField(delay, static_cast<size_t>(dl::Param::Mix), 1.0f);
		chain->setField(delay, static_cast<size_t>(dl::Param::Sync), 2.0f); // 1 s
		chain->setField(delay, static_cast<size_t>(dl::Param::Time), static_cast<float>(dl::kTimeLog2Max));
		chain->setField(delay, static_cast<size_t>(dl::Param::Feedback), 0.0f);
		std::vector<uint8_t> out;
		const feathers::device::ByteSink sink { &out, [](void* ctx, const uint8_t* data, size_t bytes) {
			                                       auto* v = static_cast<std::vector<uint8_t>*>(ctx);
			                                       v->insert(v->end(), data, data + bytes);
		                                       } };
		REQUIRE(pr::kDescriptor.container->saveState(mem, sink));
		audio.destruct(mem);
		::operator delete(mem, std::align_val_t { audio.stateAlign });
		return out;
	}();
	HostedPlugin host;
	const clap_plugin* p = host.plugin();
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(p, pr::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));
	constexpr uint32_t kFrames = 256;
	EventList events;
	host.run(kFrames, events);
	REQUIRE(chain->edit_device_body(p, slot, body.data(), body.size()));
	host.run(kFrames, events);
	events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	host.run(kFrames, events);
	events.clear();
	for (int b = 0; b < 4; ++b) {
		host.run(kFrames, events);
	}
	events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	host.run(kFrames, events);
	events.clear();
	bool slept = false;
	int sleptAt = -1;
	int soundingAfterSleep = 0;
	int soundingBlocks = 0;
	for (int b = 0; b < 60 * 48000 / static_cast<int>(kFrames); ++b) {
		const clap_process_status status = host.run(kFrames, events);
		const bool sounding = peakOf(host.left(), kFrames) + peakOf(host.right(), kFrames) > 0.0f;
		soundingBlocks += sounding ? 1 : 0;
		if (slept && sounding) {
			++soundingAfterSleep;
		}
		if (status == CLAP_PROCESS_SLEEP && !slept) {
			slept = true;
			sleptAt = b;
		}
		if (slept && b > sleptAt + 6 * 48000 / static_cast<int>(kFrames)) {
			break;
		}
	}
	CAPTURE(sleptAt);
	CAPTURE(soundingBlocks);
	CHECK(soundingBlocks > 0); // the repeat came out
	CHECK(soundingAfterSleep == 0);
	CHECK(slept);
}

TEST_CASE("NW: a Sidechain Inject in the fx lane puts the sidechain out with no note playing, keeps Feathers awake while it sounds, and lets it sleep once it stops") {
	// COPIED FROM the sidechain-awake case above, for the device that MATERIALISES the sidechain: the output is the
	// sidechain (Mix 100%, Gain 0 dB), so the plugin must not be parked while it carries sound, and may sleep over exact silence.
	HostedPlugin host;
	const clap_plugin* p = host.plugin();
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(p, feathers::fx::inject::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	// ACTIVATE PROVISIONS THE CHAIN IT IS HANDED (no editor in this case to provision it).
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));

	constexpr uint32_t kFrames = 256;
	std::vector<float> sideL(kFrames), sideR(kFrames);
	float* sidePtr[2] { sideL.data(), sideR.data() };
	clap_audio_buffer side {};
	side.data32 = sidePtr;
	side.channel_count = 2;
	host.setInputs(&side, 1);
	uint32_t seed = 1;
	EventList events;
	int slept = 0;
	float outPeak = 0.0f;
	float sidePeak = 0.0f;
	for (int b = 0; b < 64; ++b) {
		for (uint32_t i = 0; i < kFrames; ++i) {
			seed = seed * 1664525u + 1013904223u;
			sideL[i] = sideR[i] = 0.25f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
		}
		slept += host.run(kFrames, events) == CLAP_PROCESS_SLEEP ? 1 : 0;
		if (b >= 8) {
			outPeak = std::max(outPeak, peakOf(host.left(), kFrames));
			sidePeak = std::max(sidePeak, peakOf(sideL, kFrames));
		}
	}
	CAPTURE(slept);
	CHECK(slept == 0);
	// THE OUTPUT IS THE SIDECHAIN'S SOUND: audible, and at the level it came in at (Mix 100%, 0 dB) to the output stage's own scale.
	CHECK(outPeak > 0.1f);
	CHECK(outPeak <= sidePeak * 1.01f);
	// THE SIDECHAIN STOPS (the host hands zeros): once the presence's second has passed the plugin may sleep, over exact silence.
	std::fill(sideL.begin(), sideL.end(), 0.0f);
	std::fill(sideR.begin(), sideR.end(), 0.0f);
	clap_process_status status = CLAP_PROCESS_CONTINUE;
	int blocks = 0;
	while (status != CLAP_PROCESS_SLEEP && blocks < 400) {
		status = host.run(kFrames, events);
		++blocks;
	}
	CHECK(status == CLAP_PROCESS_SLEEP);
	CHECK(host.run(kFrames, events) == CLAP_PROCESS_SLEEP);
	CHECK(peakOf(host.left(), kFrames) == 0.0f);
	CHECK(peakOf(host.right(), kFrames) == 0.0f);
}

TEST_CASE("ON: the terrain's likes ride the real save and load into a FRESH instance, value by value, as the last block") {
	// CAPTURED STATE (CLAUDE.md's persistence rule): a like's place and the partial envelope it was made of. Captured,
	// saved, loaded into a fresh instance, saved again — and read back by value, envelope word for word.
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const size_t tail = blockAt(saved.bytes, "FSLK");
	REQUIRE(tail < saved.bytes.size());
	{
		// A FRESH INSTANCE HOLDS NONE.
		StateBytes slice = from(saved.bytes, "FSLK");
		clap_istream in = slice.in();
		feathers::TerrainLikes read;
		bool present = false;
		REQUIRE(feathers::loadTerrainLikes(&in, &read, &present));
		CHECK(present);
		CHECK(read.likes.empty());
	}

	feathers::TerrainLikes likes;
	for (int i = 0; i < 3; ++i) {
		feathers::TerrainLike like;
		like.x = -0.0123456789012345 * (i + 1);
		like.y = 0.0310000000000007 / (i + 1);
		like.home = 1.25 + 0.001 * i;
		like.engine = static_cast<uint16_t>(i == 1 ? 0 : 2); // two engines
		like.slot = static_cast<uint16_t>(i);
		for (size_t k = 0; k < like.spectrum.size(); ++k) {
			like.spectrum[k] = 0.731f / static_cast<float>(k + 1 + i) + 1e-7f * static_cast<float>(k);
		}
		// AND ITS FULL POINT (v4), every row a distinct double.
		like.pointCount = static_cast<uint16_t>(feathers::kTerrainLikePoint - 7 * i);
		for (size_t k = 0; k < like.pointCount; ++k) {
			like.point[k] = 0.4567890123456789 * static_cast<double>(k + 1) - 3.0 * i + 1e-13 * static_cast<double>(k);
		}
		// AND ITS ANGLES AS HEARD (v5, the P-MÖBIUS likes warp), every site a distinct float in [0, 1).
		for (size_t k = 0; k < like.angles.size(); ++k) {
			like.angles[k] = std::fmod(0.123f * static_cast<float>(k + 1) + 0.01f * static_cast<float>(i), 1.0f);
		}
		likes.likes.push_back(like);
	}
	StateBytes with;
	with.bytes.assign(saved.bytes.begin(), saved.bytes.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream withOut = with.out();
	REQUIRE(feathers::saveTerrainLikes(&withOut, likes));
	REQUIRE(feathers::savePresetIdentity(&withOut, feathers::PresetIdentity {})); // and the preset's name after it (kStateVersion 53)
	REQUIRE(feathers::saveDrumLanes(&withOut, feathers::DrumLanes {})); // and the pad lanes after it (kStateVersion 54)
	seal(with);

	HostedPlugin fresh;
	const auto* freshState =
	    static_cast<const clap_plugin_state*>(fresh.plugin()->get_extension(fresh.plugin(), CLAP_EXT_STATE));
	clap_istream withIn = with.in();
	REQUIRE(freshState->load(fresh.plugin(), &withIn));
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(freshState->save(fresh.plugin(), &resavedOut));
	StateBytes slice = from(resaved.bytes, "FSLK");
	clap_istream in = slice.in();
	feathers::TerrainLikes read;
	bool present = false;
	REQUIRE(feathers::loadTerrainLikes(&in, &read, &present));
	CHECK(present);
	REQUIRE(read.likes.size() == likes.likes.size());
	for (size_t i = 0; i < likes.likes.size(); ++i) {
		CHECK(read.likes[i].x == likes.likes[i].x);
		CHECK(read.likes[i].y == likes.likes[i].y);
		CHECK(read.likes[i].home == likes.likes[i].home);
		CHECK(read.likes[i].spectrum == likes.likes[i].spectrum);
		CHECK(read.likes[i].engine == likes.likes[i].engine);
		CHECK(read.likes[i].slot == likes.likes[i].slot);
		CHECK(read.likes[i].pointCount == likes.likes[i].pointCount);
		CHECK(read.likes[i].point == likes.likes[i].point);
		CHECK(read.likes[i].angles == likes.likes[i].angles);
	}
}

TEST_CASE("ON: a save with no likes carries no likes table — the block is its tag and a zero count, and a fresh instance loads it") {
	// THE PERF SURVEY'S FINDING: the keyed entry is kTerrainLikesFloats words
	// whatever the like count, so every patch saved with NO likes carried ~360 KB of zeros. An empty set now writes the
	// tag and an entry count of 0 — still present, because a 52 stream without the block is refused as cut.
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const std::vector<uint8_t> payload = payloadOf(saved.bytes);
	const size_t at = blockAt(payload, "FSLK");
	REQUIRE(at < payload.size());
	// "FSLK", then 0.0f — and after it only the `preset` block (kStateVersion 53, backlog QC), here naming none, and the pad
	// lanes' "FDL1" and 0.0f (kStateVersion 54, drum proposal §17 P4), here holding none.
	StateBytes noName;
	clap_ostream noNameOut = noName.out();
	REQUIRE(feathers::savePresetIdentity(&noNameOut, feathers::PresetIdentity {}));
	CHECK(payload.size() - at == 8 + noName.bytes.size() + 8);
	CHECK(blockAt(payload, feathers::kTerrainLikesKey) == payload.size()); // the table's key is not written at all
	CHECK(saved.bytes.size() < 64 * 1024); // the whole default patch; it was ~373 KB with the table
	// AND IT LOADS INTO A FRESH INSTANCE, which saves the same bytes back: nothing was lost on the way.
	HostedPlugin fresh;
	const auto* freshState =
	    static_cast<const clap_plugin_state*>(fresh.plugin()->get_extension(fresh.plugin(), CLAP_EXT_STATE));
	clap_istream in = saved.in();
	REQUIRE(freshState->load(fresh.plugin(), &in));
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(freshState->save(fresh.plugin(), &resavedOut));
	CHECK(payloadOf(resaved.bytes).size() == payload.size());
	// A LIKED SET STILL WRITES THE TABLE, and rides a fresh instance by value: the case above
	// ("the terrain's likes ride the real save and load into a FRESH instance").
	feathers::TerrainLikes one;
	one.likes.push_back({ 0.5, -0.25, 2.0, { 1.0f, 0.5f } });
	StateBytes block;
	clap_ostream blockOut = block.out();
	REQUIRE(feathers::saveTerrainLikes(&blockOut, one));
	// ONLY THE ONE SLOT IN USE (backlog OQ), never the full ~360 KB table: tag, entry count, key length, key, float
	// count, then the floats.
	CHECK(block.bytes.size()
	      == 4 + 4 + 4 + std::strlen(feathers::kTerrainLikesKey) + 4 + 4 * feathers::terrainLikesFloatsFor(1));
	CHECK(block.bytes.size() < 4 * feathers::kTerrainLikesFloats / 32);
	clap_istream blockIn = block.in();
	feathers::TerrainLikes read;
	bool present = false;
	REQUIRE(feathers::loadTerrainLikes(&blockIn, &read, &present));
	CHECK(present);
	CHECK(read == one);
}

TEST_CASE("ON: the likes block refuses a damaged like whole, ignores its key at a wrong length, and skips a key it does not know") {
	feathers::TerrainLikes one;
	one.likes.push_back({ 0.5, -0.25, 2.0, { 1.0f, 0.5f } });
	// THE WORDS: a count past the cap, or a non-finite value inside the count, is no likes — never some of them.
	std::vector<float> words(feathers::kTerrainLikesFloats, 0.0f);
	feathers::writeTerrainLikes(one, words.data());
	feathers::TerrainLikes read;
	REQUIRE(feathers::readTerrainLikes(words.data(), words.size(), &read));
	CHECK(read == one);
	std::vector<float> tooMany = words;
	tooMany[0] = static_cast<float>(feathers::kMaxTerrainLikes + 1);
	CHECK_FALSE(feathers::readTerrainLikes(tooMany.data(), tooMany.size(), &read));
	CHECK_FALSE(feathers::readTerrainLikes(words.data(), words.size() - 1, &read));
	// A SHORT ENTRY (backlog OQ) — one slot — reads the same like; a count past its slots is refused.
	std::vector<float> shortWords(feathers::terrainLikesFloatsFor(1), 0.0f);
	feathers::writeTerrainLikes(one, shortWords.data(), shortWords.size());
	REQUIRE(feathers::readTerrainLikes(shortWords.data(), shortWords.size(), &read));
	CHECK(read == one);
	CHECK(std::equal(shortWords.begin(), shortWords.end(), words.begin())); // the full table's prefix, word for word
	shortWords[0] = 2.0f;
	CHECK_FALSE(feathers::readTerrainLikes(shortWords.data(), shortWords.size(), &read));

	// THE BLOCK: a known key at a wrong length leaves no likes, and an unknown key before it is read past.
	StateBytes bytes;
	for (const char c : std::string("FSLK")) {
		bytes.bytes.push_back(static_cast<uint8_t>(c));
	}
	appendFloat(bytes.bytes, 2.0f);
	const auto entry = [&](const std::string& key, size_t count) {
		appendFloat(bytes.bytes, static_cast<float>(key.size()));
		for (const char c : key) {
			bytes.bytes.push_back(static_cast<uint8_t>(c));
		}
		appendFloat(bytes.bytes, static_cast<float>(count));
		for (size_t i = 0; i < count; ++i) {
			appendFloat(bytes.bytes, i < words.size() ? words[i] : 0.0f);
		}
	};
	entry("terrain.dislikes", 3);
	entry(feathers::kTerrainLikesKey, feathers::kTerrainLikesFloats - 1);
	clap_istream in = bytes.in();
	bool present = false;
	feathers::TerrainLikes loaded = one;
	REQUIRE(feathers::loadTerrainLikes(&in, &loaded, &present));
	CHECK(present);
	CHECK(loaded.likes.empty());
}

//------------------------------------------------------------------------------------------------//
// THE RACK, STAGE 2 STEP 1 (rack modes)                                                            //
//------------------------------------------------------------------------------------------------//

namespace {

/// A RACK'S BODY HOLDING `chains` EMPTY CHAINS — built through the Rack's own container ops, as its window leaves it.
std::vector<uint8_t> rackBodyOfEmptyChains(size_t chains) {
	namespace pr = feathers::rack::parallel;
	const feathers::device::AudioOps& audio = *pr::kDescriptor.audio;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	pr::kDescriptor.container->adopt(mem, feathers::hostCatalogue());
	audio.prepare(mem, 48000.0, 4096);
	pr::Rack& rack = *pr::rackOf(mem);
	for (size_t k = 1; k < chains; ++k) {
		REQUIRE(rack.addChain() == k);
	}
	std::vector<uint8_t> body;
	const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                                       auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                                       out->insert(out->end(), data, data + bytes);
	                                       } };
	REQUIRE(pr::kDescriptor.container->saveState(mem, sink));
	audio.destruct(mem);
	::operator delete(mem, std::align_val_t { audio.stateAlign });
	return body;
}

} // namespace

TEST_CASE("Rack stage 2: a reverb in the SELECTED chain keeps the plugin awake; in an UNSELECTED chain it does not — SLEEP follows what the rack lets through") {
	// THE NO CASE ABOVE, IN SELECT: chain 1 empty, chain 2 the reverb. Position 1 hears the reverb's chain; position 0
	// hears the empty one, and the reverb, still running and still ringing, reaches nothing — so it must not keep the
	// plugin awake (CLAUDE.md's SLEEP rule: the decision follows the output). Measured against an all-empty rack.
	namespace pr = feathers::rack::parallel;
	const auto run = [](bool withReverb, double position, int& blocks, int& soundingAfterSleep) {
		HostedPlugin host;
		const clap_plugin* p = host.plugin();
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		const uint32_t slot = chain->add_device(p, pr::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));
		constexpr uint32_t kFrames = 256;
		EventList events;
		host.run(kFrames, events);
		const std::vector<uint8_t> body = withReverb ? rackBodyWithReverb() : rackBodyOfEmptyChains(2);
		REQUIRE(chain->edit_device_body(p, slot, body.data(), body.size()));
		REQUIRE(chain->set_control(p, slot, "Rack Mode", static_cast<double>(pr::Mode::Select)));
		REQUIRE(chain->set_control(p, slot, "Rack Position", position));
		host.run(kFrames, events);
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		host.run(kFrames, events);
		events.clear();
		for (int b = 0; b < 20; ++b) {
			host.run(kFrames, events);
		}
		events.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
		host.run(kFrames, events);
		events.clear();
		blocks = 0;
		soundingAfterSleep = 0;
		bool slept = false;
		while (blocks < 30 * 48000 / static_cast<int>(kFrames)) {
			const clap_process_status status = host.run(kFrames, events);
			++blocks;
			if (status == CLAP_PROCESS_SLEEP) {
				slept = true;
				host.run(kFrames, events);
				soundingAfterSleep += peakOf(host.left(), kFrames) > 0.0f ? 1 : 0;
				break;
			}
		}
		return slept;
	};
	int emptyBlocks = 0, emptySounding = 0;
	int selectedBlocks = 0, selectedSounding = 0;
	int unselectedBlocks = 0, unselectedSounding = 0;
	CHECK(run(false, 1.0, emptyBlocks, emptySounding));
	CHECK(run(true, 1.0, selectedBlocks, selectedSounding));
	CHECK(run(true, 0.0, unselectedBlocks, unselectedSounding));
	CAPTURE(emptyBlocks);
	CAPTURE(selectedBlocks);
	CAPTURE(unselectedBlocks);
	CHECK(emptySounding == 0);
	CHECK(selectedSounding == 0);
	CHECK(unselectedSounding == 0);
	// SELECTED: the reverb's chain kept it awake, a second and more past the empty rack.
	CHECK(selectedBlocks > emptyBlocks + 48000 / 256);
	// UNSELECTED: it sleeps exactly when the empty rack does — the ringing chain reaches nothing.
	CHECK(unselectedBlocks == emptyBlocks);
}

TEST_CASE("Rack stage 2: the DAW reads the rack's position as a chain — 'Chain 2' in Select, 'Chain 2 → 3, 40%' in Blend, the unit in Parallel — types a chain back, and is owed a TEXT rescan when the chain count moves") {
	namespace pr = feathers::rack::parallel;
	RescanHost log;
	HostedPlugin h(&log.host);
	const clap_plugin* p = h.plugin();
	const clap_plugin_feathers_test_chain* chain = testChainOf(p);
	const uint32_t at = chain->add_device(p, pr::kTraits.tag);
	REQUIRE(at != UINT32_MAX);
	// RE-ACTIVATED AND ONE BLOCK RUN, as the NO cases do, so the running rack is filed (what a body edit loads into).
	p->stop_processing(p);
	p->deactivate(p);
	REQUIRE(p->activate(p, 48000.0, 1, 4096));
	REQUIRE(p->start_processing(p));
	EventList events;
	h.run(256, events);
	const std::vector<uint8_t> three = rackBodyOfEmptyChains(3);
	REQUIRE(chain->edit_device_body(p, at, three.data(), three.size()));
	REQUIRE(chain->set_control(p, at, "Rack Mode", static_cast<double>(pr::Mode::Select)));
	const uint32_t n = chain->pin_slot(p, at, "Rack Position");
	REQUIRE(n != UINT32_MAX);
	mainThreadVisit(p);
	const clap_id id = 0x1000u + n;

	// SELECT, THREE CHAINS: the nearest chain, 1-based — the DSP's own choice (`selectedChain`).
	CHECK(slotText(p, n, 0.0) == "Chain 1");
	CHECK(slotText(p, n, 0.5) == "Chain 2");
	CHECK(slotText(p, n, 0.8) == "Chain 3");
	// AND BACK: a chain's number is its centre, as a 0..1 slot value (the position's range is 0..1).
	double v = -1.0;
	REQUIRE(paramsOf(p)->text_to_value(p, id, "Chain 2", &v));
	CHECK(v == 0.5);
	REQUIRE(paramsOf(p)->text_to_value(p, id, "3", &v));
	CHECK(v == 1.0);

	// BLEND: between two chains, and how far. THE MODE CHANGES THE WORDS, so the host is owed a TEXT rescan for it.
	mainThreadVisit(p);
	const size_t beforeMode = log.rescans.size();
	REQUIRE(chain->set_control(p, at, "Rack Mode", static_cast<double>(pr::Mode::Blend)));
	mainThreadVisit(p);
	REQUIRE(log.rescans.size() == beforeMode + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_TEXT) != 0);
	CHECK(slotText(p, n, 0.7) == "Chain 2 \xE2\x86\x92 3, 40%");
	REQUIRE(paramsOf(p)->text_to_value(p, id, "Chain 2 \xE2\x86\x92 3, 40%", &v));
	CHECK(v == doctest::Approx(0.7).epsilon(1e-9));

	// PARALLEL: the declared unit, exactly as every slot printed before.
	REQUIRE(chain->set_control(p, at, "Rack Mode", static_cast<double>(pr::Mode::Parallel)));
	const feathers::device::ParamDesc& desc = pr::kParamDescs[static_cast<size_t>(pr::Param::Position)];
	CHECK(slotText(p, n, 0.5) == feathers::device::formatValue(desc, 0.5));

	// A CHAIN ADDED: the same value names another chain, so the host is told to re-read the text.
	REQUIRE(chain->set_control(p, at, "Rack Mode", static_cast<double>(pr::Mode::Select)));
	mainThreadVisit(p);
	const size_t before = log.rescans.size();
	const std::vector<uint8_t> four = rackBodyOfEmptyChains(4);
	REQUIRE(chain->edit_device_body(p, at, four.data(), four.size()));
	mainThreadVisit(p);
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_TEXT) != 0);
	CHECK(slotText(p, n, 1.0 / 3.0) == "Chain 2");
	CHECK(slotText(p, n, 0.5) == "Chain 3"); // 1.5 of 3 rounds up: what was chain 2 of three is chain 3 of four
	// THE SAME COUNT AGAIN OWES NOTHING.
	REQUIRE(chain->edit_device_body(p, at, four.data(), four.size()));
	mainThreadVisit(p);
	CHECK(log.rescans.size() == before + 1);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG OQ — STATE SIZE AND SAVE COST: MEASURED, BUDGETED, GUARDED                                //
//------------------------------------------------------------------------------------------------//

namespace {

/// ONE PATCH'S NUMBERS: the saved state's bytes, and the median save and load times on the main thread (the save
/// includes the extra-state harvest, `ModOps::publishExtraState`'s snapshot, which every save takes).
struct OqCost {
	size_t bytes = 0;
	double saveMs = 0.0;
	double loadMs = 0.0;
};

double oqMedian(std::vector<double> v) {
	std::sort(v.begin(), v.end());
	return v[v.size() / 2];
}

/// THE PATCH IN `host`, measured: saved `rounds` times, and loaded into `rounds` FRESH instances (built before the clock
/// starts, so construction is not counted).
OqCost oqMeasure(HostedPlugin& host, int rounds = 5) {
	OqCost c;
	std::vector<double> saves;
	StateBytes saved;
	for (int i = 0; i < rounds; ++i) {
		StateBytes s;
		const auto t0 = std::chrono::steady_clock::now();
		REQUIRE(saveState(host.plugin(), &s));
		saves.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
		saved = s;
	}
	c.bytes = saved.bytes.size();
	c.saveMs = oqMedian(saves);
	std::vector<double> loads;
	for (int i = 0; i < rounds; ++i) {
		HostedPlugin fresh;
		StateBytes in = saved;
		const auto t0 = std::chrono::steady_clock::now();
		REQUIRE(loadState(fresh.plugin(), &in));
		loads.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
	}
	c.loadMs = oqMedian(loads);
	return c;
}

/// A RACK BODY FOR THE STRESS PATCH: four chains of six devices each, every chain ending in a Rack of its own (nesting
/// two deep), built through the Rack's own container ops as its window leaves it.
std::vector<uint8_t> oqStressRackBody(const std::vector<std::string>& audioTags) {
	namespace pr = feathers::rack::parallel;
	const feathers::device::AudioOps& audio = *pr::kDescriptor.audio;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	pr::kDescriptor.container->adopt(mem, feathers::hostCatalogue());
	audio.prepare(mem, 48000.0, 4096);
	pr::Rack& rack = *pr::rackOf(mem);
	for (size_t k = 1; k < 4; ++k) {
		REQUIRE(rack.addChain() == k);
	}
	size_t next = 0;
	for (size_t k = 0; k < 4; ++k) {
		feathers::rack::RackBridge* chain = rack.chainAt(k)->bridge();
		REQUIRE(chain != nullptr);
		for (int d = 0; d < 5; ++d) {
			chain->addDevice(audioTags[next++ % audioTags.size()].c_str());
		}
		CHECK(chain->addDevice(pr::kTraits.tag) != feathers::kNoInstance);
	}
	std::vector<uint8_t> body;
	const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                                       auto* out = static_cast<std::vector<uint8_t>*>(ctx);
		                                       out->insert(out->end(), data, data + bytes);
	                                       } };
	REQUIRE(pr::kDescriptor.container->saveState(mem, sink));
	audio.destruct(mem);
	::operator delete(mem, std::align_val_t { audio.stateAlign });
	return body;
}

/// **THE STRESS PATCH** (backlog OQ): every device the lane offers, cycled to 48 in the lane; a Rack of four chains
/// nesting a Rack in each; a Spectral frozen in Vocode on a played note (its captured envelope is extra state); 520
/// routes by hand (the matrix past the engine's starting room, backlog OJ); and the cap of 64 terrain likes, each with its
/// full point and angles. Saved, then loaded into a fresh instance — returned there, so what is measured is a patch as a
/// host would hold it.
void oqBuildStress(HostedPlugin& out) {
	StateBytes saved;
	{
		HostedPlugin h;
		const clap_plugin* p = h.plugin();
		const clap_plugin_feathers_test_chain* chain = testChainOf(p);
		std::vector<std::string> tags;
		char tag[64];
		for (uint32_t i = 0; chain->available_device(p, i, tag, sizeof tag); ++i) {
			tags.emplace_back(tag);
		}
		REQUIRE(tags.size() > 20);
		std::vector<std::string> audioTags;
		for (const std::string& t : tags) {
			const feathers::device::Descriptor* d = feathers::deviceForTag(t.c_str());
			if (d != nullptr && d->audio != nullptr && d->container == nullptr) {
				audioTags.push_back(t);
			}
		}
		REQUIRE(!audioTags.empty());
		while (chain->device_count(p) < 48) {
			const uint32_t at = chain->device_count(p);
			if (chain->add_device(p, tags[at % tags.size()].c_str()) == UINT32_MAX) {
				REQUIRE(chain->add_device(p, feathers::fx::filter::kTraits.tag) != UINT32_MAX);
			}
		}
		const uint32_t spectral = chain->add_device(p, feathers::fx::spectral::kTraits.tag);
		REQUIRE(spectral != UINT32_MAX);
		const uint32_t rackSlot = chain->add_device(p, feathers::rack::parallel::kTraits.tag);
		REQUIRE(rackSlot != UINT32_MAX);
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, 48000.0, 1, 4096));
		REQUIRE(p->start_processing(p));
		EventList events;
		h.run(256, events);
		const std::vector<uint8_t> body = oqStressRackBody(audioTags);
		REQUIRE(chain->edit_device_body(p, rackSlot, body.data(), body.size()));
		CHECK(chain->set_control(p, spectral, "Spectral Vocode Freeze", 1.0));
		events.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
		for (int b = 0; b < 40; ++b) {
			h.run(256, events);
			events.clear();
		}
		uint32_t routes = 0;
		for (uint32_t s = 0; s < feathers::kCoreSourceCount && routes < 520; ++s) {
			for (uint32_t d = 0; d < feathers::kParamDestinationCount && routes < 520; ++d) {
				if (chain->set_core_route(p, s, d, 0.001 * static_cast<double>(1 + routes))) {
					++routes;
					if (routes % 32 == 0) {
						h.run(256, events);
					}
				}
			}
		}
		CHECK(routes == 520);
		h.run(256, events);
		REQUIRE(saveState(p, &saved));
	}
	// THE LIKES, AT THE CAP: spliced in as the last block (the ON case's road), since the editor's heart has no test seam.
	const std::vector<uint8_t> payload = payloadOf(saved.bytes);
	const size_t tail = blockAt(payload, "FSLK");
	REQUIRE(tail < payload.size());
	feathers::TerrainLikes likes;
	for (size_t i = 0; i < feathers::kMaxTerrainLikes; ++i) {
		feathers::TerrainLike like;
		like.x = 0.01 * static_cast<double>(i);
		like.y = -0.02 * static_cast<double>(i);
		like.home = 1.0;
		like.engine = static_cast<uint16_t>(i % 3);
		like.slot = static_cast<uint16_t>(i % 8);
		for (size_t k = 0; k < like.spectrum.size(); ++k) {
			like.spectrum[k] = 1.0f / static_cast<float>(k + 1 + i);
		}
		like.pointCount = static_cast<uint16_t>(feathers::kTerrainLikePoint);
		for (size_t k = 0; k < like.point.size(); ++k) {
			like.point[k] = 0.001 * static_cast<double>(k + i);
		}
		for (size_t k = 0; k < like.angles.size(); ++k) {
			like.angles[k] = std::fmod(0.013f * static_cast<float>(k + i), 1.0f);
		}
		likes.likes.push_back(like);
	}
	StateBytes with;
	with.bytes.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream withOut = with.out();
	REQUIRE(feathers::saveTerrainLikes(&withOut, likes));
	REQUIRE(feathers::savePresetIdentity(&withOut, feathers::PresetIdentity {})); // and the preset's name after it (kStateVersion 53)
	REQUIRE(feathers::saveDrumLanes(&withOut, feathers::DrumLanes {})); // and the pad lanes after it (kStateVersion 54)
	seal(with);
	REQUIRE(loadState(out.plugin(), &with));
	CHECK(testChainOf(out.plugin())->device_count(out.plugin()) >= 50);
}

} // namespace

/// **THE BUDGETS (backlog OQ), from the measurements in the backlog's OQ section.** A shipped patch, and the stress patch
/// at the caps a hand can reach. Each is roughly twice the largest measured, so ordinary growth passes and a block that
/// starts writing a fixed table whatever it holds (the likes' 360 KB was that) fails here first.
/// MEASURED 2026-10-30: default 12,759 B; the largest of 86 shipped presets 36,080 B (wood_elves_pan); the stress patch
/// 531,540 B, of which 360,228 B is its 64 likes (each like 5.6 KB: a double is four 16-bit words in four floats). NO TIME BUDGET: the times are recorded in the backlog but swing with
/// this shared host's load, and a guard on them would be a flake.
constexpr size_t kOqPatchBudget = 80u * 1024u;
constexpr size_t kOqStressBudget = 1024u * 1024u;

TEST_CASE("OQ: MEASURED — the saved state's size and its save and load time: the default patch, every shipped preset, and a stress patch") {
	// THE HOST'S LOAD WHEN THE TIMES WERE TAKEN, printed beside them: on this shared host a time without it means little.
	if (std::FILE* f = std::fopen("/proc/loadavg", "r")) {
		char line[128] {};
		if (std::fgets(line, sizeof line, f) != nullptr) {
			MESSAGE("OQ host load (/proc/loadavg): " << line);
		}
		std::fclose(f);
	}
	size_t largest = 0;
	std::string largestName;
	{
		HostedPlugin host;
		const OqCost c = oqMeasure(host);
		MESSAGE("OQ default patch: " << c.bytes << " B, save " << c.saveMs << " ms, load " << c.loadMs << " ms");
		CHECK(c.bytes <= kOqPatchBudget);
	}
	size_t presets = 0;
	double worstSave = 0.0, worstLoad = 0.0;
	for (const feathers::FactoryPresetFile& file : feathers::factoryPresetFiles()) {
		if (!file.path.starts_with("Feathers/")) {
			continue;
		}
		CAPTURE(std::string(file.path));
		feathers::PresetMeta meta;
		std::vector<uint8_t> payload;
		REQUIRE(feathers::PresetStore::decode(std::vector<uint8_t>(file.bytes.begin(), file.bytes.end()), &meta, &payload));
		const bool test = std::find(meta.tags.begin(), meta.tags.end(), "test") != meta.tags.end();
		HostedPlugin host;
		StateBytes bytes;
		bytes.bytes = payload;
		clap_istream in = bytes.in();
		if (test) {
			REQUIRE(testChainOf(host.plugin())->load_preset(host.plugin(), &in));
		} else {
			REQUIRE(loadState(host.plugin(), &bytes));
		}
		const OqCost c = oqMeasure(host, 3);
		if (!test) {
			MESSAGE("OQ " << std::string(file.path) << ": " << c.bytes << " B, save " << c.saveMs << " ms, load " << c.loadMs << " ms");
		}
		CHECK(c.bytes <= kOqPatchBudget);
		if (c.bytes > largest) {
			largest = c.bytes;
			largestName = std::string(file.path);
		}
		worstSave = std::max(worstSave, c.saveMs);
		worstLoad = std::max(worstLoad, c.loadMs);
		++presets;
	}
	MESSAGE("OQ " << presets << " shipped presets: largest " << largest << " B (" << largestName << "), worst save "
	              << worstSave << " ms, worst load " << worstLoad << " ms");
	CHECK(presets > 80);
	HostedPlugin stress;
	oqBuildStress(stress);
	const OqCost c = oqMeasure(stress);
	MESSAGE("OQ stress patch: " << c.bytes << " B, save " << c.saveMs << " ms, load " << c.loadMs << " ms");
	{
		StateBytes s;
		REQUIRE(saveState(stress.plugin(), &s));
		const std::vector<uint8_t> payload = payloadOf(s.bytes);
		const size_t at = blockAt(payload, "FSLK");
		REQUIRE(at < payload.size());
		MESSAGE("OQ stress patch: the likes block is " << payload.size() - at << " B of it");
		// THE CAP'S TABLE, AND ONLY BECAUSE ALL 64 ARE USED: 64 slots of the like layout (backlog OQ's trim).
		CHECK(payload.size() - at >= 4 * feathers::terrainLikesFloatsFor(feathers::kMaxTerrainLikes));
	}
	CHECK(c.bytes <= kOqStressBudget);
}

TEST_CASE("an Arp in Feathers' modulator lane steps on the host's song grid to the exact sample — the host's fixed-point "
          "position is not rounded a sample late") {
	// CLAP carries the song position in fixed point (2^31 per beat), so a block that starts ON a sample arrives a hair past
	// it; read as given, every transport-locked step landed one sample late (the rack plugin measured 6001 for 6000,
	// 2026-10-31). 120 bpm, 1/16: a step every 6000 samples from beat 0, in host blocks of 512.
	// SEATED BEFORE THE ACTIVATE, which provisions what the bridge holds (the road a loaded patch takes): an add while
	// active waits for a main-thread visit this harness does not make.
	HostedPlugin host(&kHost, [](const clap_plugin* p, void*) {
		const auto* c = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(c != nullptr);
		REQUIRE(c->add_mod_device(p, feathers::mod::arp::kTraits.tag) != UINT32_MAX);
	});
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	EventList none;
	host.setTransport(0.0, false, false);
	host.run(kMaxFrames, none); // the strip adopts its device at a block
	REQUIRE(chain->delivered_note(host.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	int64_t at = kMaxFrames; // samples rendered before this block
	const int64_t zero = at;  // the song's beat 0 stands here
	EventList chord;
	chord.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	chord.pushNote(CLAP_EVENT_NOTE_ON, 0, 64, 2, 1.0);
	chord.pushNote(CLAP_EVENT_NOTE_ON, 0, 67, 3, 1.0);
	bool first = true;
	while (at < zero + 6000 * 4 + 1000) {
		host.setTransport(static_cast<double>(at - zero) / kSampleRate, true, false); // beats + tempo, as a DAW sends
		host.run(kMaxFrames, first ? chord : none);
		first = false;
		at += kMaxFrames;
	}
	std::vector<int64_t> ons;
	uint32_t kind = 0;
	int32_t key = 0;
	uint64_t sample = 0;
	for (uint32_t i = 0; chain->delivered_note(host.plugin(), i, &kind, &key, &sample); ++i) {
		if (kind == 0) {
			ons.push_back(static_cast<int64_t>(sample) - zero);
		}
	}
	REQUIRE(ons.size() == 5);
	for (size_t i = 0; i < ons.size(); ++i) {
		CAPTURE(i);
		CHECK(ons[i] == static_cast<int64_t>(i) * 6000);
	}
}

#ifdef FEATHERS_DIAGNOSTIC
namespace {
/// The number after `"key":` in `s` from `from`, or NaN (`null`, or absent).
double jsonNumber(const std::string& s, const std::string& key, size_t from = 0) {
	const size_t at = s.find("\"" + key + "\":", from);
	if (at == std::string::npos) {
		return std::nan("");
	}
	const char* p = s.c_str() + at + key.size() + 3;
	char* end = nullptr;
	const double v = std::strtod(p, &end);
	return end == p ? std::nan("") : v;
}
} // namespace

TEST_CASE("SR [measure]: where an IDLE 1024-frame block's time goes outside the devices, by stage") {
	// THE RACK SESSION'S SECOND FINDING (2026-11-04): idle in Bitwig (no voices, transport stopped), a 1024-frame
	// block's p99 is ~1.5 ms while the devices total ~300 us. The real plugin, a Delay in the fx lane, the transport
	// stopped, no notes; every block recorded (a threshold of almost nothing), drained by deactivate every 200 blocks
	// (the ring holds 255), and each field's median and p99 read back from the log.
	const std::filesystem::path dir =
	    std::filesystem::temp_directory_path() / ("feathers-idle-spikes-" + std::to_string(::getpid()));
	std::filesystem::remove_all(dir);
	::setenv("FEATHERS_SPIKE_LOG_DIR", dir.string().c_str(), 1);
	::setenv("FEATHERS_SPIKE_THRESHOLD", "1e-9", 1);
	constexpr uint32_t kFrames = 1024;
	{
		HostedPlugin host;
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    host.plugin()->get_extension(host.plugin(), CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		REQUIRE(chain->add_device(host.plugin(), feathers::fx::delay::kTraits.tag) != UINT32_MAX);
		const clap_plugin* p = host.plugin();
		host.growBuffers(kFrames);
		host.setTransport(0.0, false);
		EventList none;
		for (int round = 0; round < 6; ++round) {
			p->stop_processing(p);
			p->deactivate(p); // drains the last round into the log
			REQUIRE(p->activate(p, kSampleRate, 1, kFrames));
			REQUIRE(p->start_processing(p));
			for (int b = 0; b < 200; ++b) {
				(void)host.run(kFrames, none);
			}
		}
	} // and the last round
	::unsetenv("FEATHERS_SPIKE_THRESHOLD");
	std::vector<std::string> fields = { "us", "cpu_us", "devices_us", "other_us", "unattributed_us" };
	for (const char* s : { "events", "inputs", "params", "voices", "bus", "mods", "rebake", "resolve", "publish" }) {
		fields.emplace_back(std::string("stages.") + s);
	}
	std::vector<std::vector<double>> values(fields.size());
	size_t lines = 0;
	size_t rebakes = 0; // blocks whose flags say the bank built a table
	std::error_code ec;
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		std::ifstream in(entry.path());
		for (std::string line; std::getline(in, line);) {
			if (line.find("\"event\":\"spike\"") == std::string::npos || line.find("\"frames\":1024") == std::string::npos) {
				continue;
			}
			++lines;
			// IN THE FLAGS ARRAY ONLY: every v2+ line also has `"stages":{…"rebake":…}`, and counting the whole line once
			// reported a build on every idle block (2026-11-04) when there were none.
			const size_t flags = line.find("\"flags\":[");
			if (flags != std::string::npos && line.find("\"rebake\"", flags) < line.find(']', flags)) {
				++rebakes;
			}
			const size_t stages = line.find("\"stages\":{");
			for (size_t f = 0; f < fields.size(); ++f) {
				double v = 0.0;
				if (fields[f].rfind("stages.", 0) == 0) {
					const size_t close = line.find('}', stages);
					const std::string inside = line.substr(stages, close - stages);
					v = jsonNumber(inside, fields[f].substr(7));
					if (std::isnan(v)) {
						v = 0.0; // a stage that did not run this block
					}
				}
				else {
					v = jsonNumber(line, fields[f]);
				}
				values[f].push_back(v);
			}
		}
	}
	std::filesystem::remove_all(dir);
	::setenv("FEATHERS_SPIKE_LOG_DIR", "", 1);
	REQUIRE(lines >= 1000);
	double load = -1.0;
	if (std::FILE* fl = std::fopen("/proc/loadavg", "r")) {
		if (std::fscanf(fl, "%lf", &load) != 1) {
			load = -1.0;
		}
		std::fclose(fl);
	}
	std::string report = "idle, 1024 frames, a Delay, " + std::to_string(lines) + " blocks (" + std::to_string(rebakes) + " flagged rebake), load " + std::to_string(load) + ":";
	for (size_t f = 0; f < fields.size(); ++f) {
		std::vector<double>& v = values[f];
		std::sort(v.begin(), v.end());
		const double median = v[v.size() / 2];
		const double p99 = v[std::min(v.size() - 1, v.size() * 99 / 100)];
		char buf[160];
		std::snprintf(buf, sizeof buf, "\n  %-16s median %9.1f us   p99 %9.1f us", fields[f].c_str(), median, p99);
		report += buf;
	}
	MESSAGE(report);
}
#endif

TEST_CASE("drum pad lanes (§17 P4): a pad's fx lane rides the real save and load into a FRESH instance as the last block, "
          "body for body; a fresh instance saves none") {
	HostedPlugin host;
	const auto* state =
	    static_cast<const clap_plugin_state*>(host.plugin()->get_extension(host.plugin(), CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	StateBytes saved;
	clap_ostream out = saved.out();
	REQUIRE(state->save(host.plugin(), &out));
	const size_t tail = blockAt(saved.bytes, "FDL1");
	REQUIRE(tail < saved.bytes.size());
	{
		StateBytes slice = from(saved.bytes, "FDL1");
		clap_istream in = slice.in();
		feathers::DrumLanes read;
		bool present = false;
		REQUIRE(feathers::loadDrumLanes(&in, &read, &present));
		CHECK(present);
		for (const auto& body : read) {
			CHECK(body.empty());
		}
	}
	// A REAL LANE'S BODY, made the way the kit window makes one: pad F's lane, a Bus Utility at 0.3.
	feathers::ParamSet synthParams;
	auto synth = std::make_unique<feathers::Synth>(synthParams);
	synth->setCatalogue(feathers::hostCatalogue());
	synth->activate(48000.0, 512);
	feathers::rack::InnerChain* lane = synth->ensurePadLane(5);
	REQUIRE(lane != nullptr);
	const auto id = lane->bridge()->addDevice("busu");
	REQUIRE(id != 0u);
	lane->bridge()->setField(id, 1, 0.3f);
	std::vector<uint8_t> body;
	const feathers::device::ByteSink sink { &body, [](void* ctx, const uint8_t* data, size_t bytes) {
		                                       auto* v = static_cast<std::vector<uint8_t>*>(ctx);
		                                       v->insert(v->end(), data, data + bytes);
	                                       } };
	REQUIRE(lane->saveState(sink));
	REQUIRE_FALSE(body.empty());
	feathers::DrumLanes lanes;
	lanes[5] = body;
	StateBytes with;
	with.bytes.assign(saved.bytes.begin(), saved.bytes.begin() + static_cast<std::ptrdiff_t>(tail));
	clap_ostream withOut = with.out();
	REQUIRE(feathers::saveDrumLanes(&withOut, lanes));
	seal(with);

	HostedPlugin fresh;
	const auto* freshState =
	    static_cast<const clap_plugin_state*>(fresh.plugin()->get_extension(fresh.plugin(), CLAP_EXT_STATE));
	clap_istream withIn = with.in();
	REQUIRE(freshState->load(fresh.plugin(), &withIn));
	StateBytes resaved;
	clap_ostream resavedOut = resaved.out();
	REQUIRE(freshState->save(fresh.plugin(), &resavedOut));
	StateBytes slice = from(resaved.bytes, "FDL1");
	clap_istream in = slice.in();
	feathers::DrumLanes read;
	bool present = false;
	REQUIRE(feathers::loadDrumLanes(&in, &read, &present));
	REQUIRE(present);
	for (size_t pad = 0; pad < read.size(); ++pad) {
		CAPTURE(pad);
		CHECK(read[pad] == (pad == 5 ? body : std::vector<uint8_t> {}));
	}
}

TEST_CASE("Pattern's Go pressed and released between two blocks in Feathers' modulator lane: the engine reads the rise for a block, then the fall lands at the next main-thread visit (lead, 2026-11-07: the press was LOST)") {
	// THE FEATHERS ROAD (`ParamBridge::setDeviceControl` → the chains publish → `Synth::adoptChains`), the rack's twin
	// (test_rack_resolve "pattern Go pressed and released between two blocks …"). By value: the ENGINE's base for the
	// trigger, which is what the device reads.
	static uint32_t seated = UINT32_MAX;
	HostedPlugin host(&kHost, [](const clap_plugin* p, void*) {
		const auto* c = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(c != nullptr);
		seated = c->add_mod_device(p, feathers::mod::arp::pat::kTraits.tag);
		REQUIRE(seated != UINT32_MAX);
	});
	const clap_plugin* p = host.plugin();
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t at = CLAP_FEATHERS_TEST_STRIP | seated;
	EventList none;
	host.setTransport(0.0, true, false);
	host.run(kMaxFrames, none);
	mainThreadVisit(p);
	REQUIRE(chain->set_control(p, at, "P2 Fill", 1.0));
	host.run(kMaxFrames, none);
	mainThreadVisit(p);
	const auto engineBase = [&] {
		double base = -1.0;
		REQUIRE(chain->engine_control(p, at, "P2 Fill Trigger", &base, nullptr));
		return base;
	};
	REQUIRE(engineBase() == 0.0);
	// THE PRESS AND THE RELEASE, BOTH BEFORE THE NEXT BLOCK.
	REQUIRE(chain->set_control(p, at, "P2 Fill Trigger", 1.0));
	REQUIRE(chain->release_control != nullptr);
	REQUIRE(chain->release_control(p, at, "P2 Fill Trigger", 0.0)); // the momentary's release road
	host.run(kMaxFrames, none);
	CHECK(engineBase() == 1.0); // the block READ the rise: the device saw the press
	mainThreadVisit(p);          // the fall lands now that the rise was adopted
	host.run(kMaxFrames, none);
	CHECK(engineBase() == 0.0); // and the trigger is back at rest
	// AN ORDINARY FALL WITH NO RISE IN FLIGHT IS NOT HELD.
	REQUIRE(chain->set_control(p, at, "P2 Fill", 0.0));
	host.run(kMaxFrames, none);
	double fill = -1.0;
	REQUIRE(chain->engine_control(p, at, "P2 Fill", &fill, nullptr));
	CHECK(fill == 0.0);
}

//------------------------------------------------------------------------------------------------//
// A VARIANT LOAD UNDER A HELD NOTE (variant-seamless, 2026-11-07: switching variations should not wait for a new note,
// and should ideally come in in phase). A variant chip loads a whole
// preset on the preset road (`guiLoadState`), and nothing on that road resets a voice: the parameters commit and the
// next tick's character reaches every sounding voice. These cases pin that, BY VALUE through `voice_reading`.
//------------------------------------------------------------------------------------------------//

namespace {

/// The one voice the case's note took: the slot whose edge is up. -1 when none is.
int soundingVoice(const clap_plugin* p) {
	const auto* chain = testChainOf(p);
	REQUIRE(chain->voice_reading != nullptr);
	for (uint32_t slot = 0; slot < static_cast<uint32_t>(feathers::kVoiceSlots); ++slot) {
		double level = 0.0;
		REQUIRE(chain->voice_reading(p, slot, CLAP_FEATHERS_VOICE_LEVEL, &level));
		if (level > 0.0) {
			return static_cast<int>(slot);
		}
	}
	return -1;
}

double engineParam(const clap_plugin* p) {
	const auto* params = static_cast<const clap_plugin_params*>(p->get_extension(p, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	double engine = -1.0;
	REQUIRE(params->get_value(p, static_cast<clap_id>(feathers::ParamId::OscType), &engine));
	return engine;
}

double voiceValue(const clap_plugin* p, int slot, uint32_t what) {
	double v = 0.0;
	REQUIRE(testChainOf(p)->voice_reading(p, static_cast<uint32_t>(slot), what, &v));
	return v;
}

/// Load `bytes` on the PRESET road — the one a variant chip takes — and give the main thread its visit.
void loadOnPresetRoad(const clap_plugin* p, StateBytes* bytes) {
	bytes->read = 0;
	clap_istream in = bytes->in();
	REQUIRE(testChainOf(p)->load_preset(p, &in));
	mainThreadVisit(p);
}

/// The largest step between neighbouring samples of the left channel over `blocks` blocks, carrying `last` across them.
float largestStep(HostedPlugin& hosted, uint32_t frames, int blocks, float* last, float* peak = nullptr) {
	EventList none;
	float step = 0.0f;
	for (int b = 0; b < blocks; ++b) {
		(void)hosted.run(frames, none);
		for (uint32_t s = 0; s < frames; ++s) {
			const float x = hosted.left()[s];
			step = std::max(step, std::fabs(x - *last));
			if (peak != nullptr) {
				*peak = std::max(*peak, std::fabs(x));
			}
			*last = x;
		}
	}
	return step;
}

} // namespace

TEST_CASE("a variant load keeps a held note: it sounds on, its pitch reaches the variant's, and its phase runs on unbroken") {
	// THE GENTLE PATCH: Basic Shapes at its default mix, a sine, so a click is a step far above the sine's own slope.
	HostedPlugin hosted;
	const clap_plugin* p = hosted.plugin();
	EventList setup;
	setup.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::BasicShapes));
	(void)hosted.run(64, setup);
	StateBytes base;
	REQUIRE(saveState(p, &base));
	EventList toFifth;
	toFifth.pushParam(0, static_cast<clap_id>(feathers::ParamId::TransposeSemi), 7.0);
	(void)hosted.run(64, toFifth);
	StateBytes fifth; // variant 2: the same sound a fifth up
	REQUIRE(saveState(p, &fifth));
	EventList toTriangle;
	toTriangle.pushParam(0, static_cast<clap_id>(feathers::ParamId::TransposeSemi), 0.0);
	toTriangle.pushParam(0, static_cast<clap_id>(feathers::ParamId::ShapesSine), 0.0);
	toTriangle.pushParam(0, static_cast<clap_id>(feathers::ParamId::ShapesTriangle), 1.0);
	(void)hosted.run(64, toTriangle);
	StateBytes triangle; // variant 3: same pitch, another shape
	REQUIRE(saveState(p, &triangle));
	loadOnPresetRoad(p, &base);

	constexpr uint32_t kF = 256;
	EventList on;
	on.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	(void)hosted.run(kF, on);
	float last = 0.0f;
	float peak = 0.0f;
	(void)largestStep(hosted, kF, 20, &last); // past the attack
	const float steadyStep = largestStep(hosted, kF, 20, &last, &peak);
	REQUIRE(peak > 0.1f);
	const int slot = soundingVoice(p);
	REQUIRE(slot >= 0);
	const double pitch = voiceValue(p, slot, CLAP_FEATHERS_VOICE_PITCH);
	CHECK(pitch == doctest::Approx(60.0).epsilon(1e-9));

	SUBCASE("a pitch variant: the held voice reaches the variant's pitch, without a click") {
		loadOnPresetRoad(p, &fifth);
		const float acrossStep = largestStep(hosted, kF, 4, &last);
		CHECK(soundingVoice(p) == slot); // the same voice, still up: no reset, no new note
		CHECK(voiceValue(p, slot, CLAP_FEATHERS_VOICE_PITCH) == doctest::Approx(67.0).epsilon(1e-9));
		float after = 0.0f;
		(void)largestStep(hosted, kF, 10, &last, &after);
		CHECK(after > 0.5f * peak);
		// A sine a fifth up has a slope 1.5x the root's; a click or a phase reset is a step of the order of the peak.
		CAPTURE(steadyStep);
		CAPTURE(acrossStep);
		CHECK(acrossStep < 1.6f * steadyStep);
	}
	SUBCASE("a shape variant at the same pitch: the oscillator's phase runs on by exactly one block's advance") {
		const double phase0 = voiceValue(p, slot, CLAP_FEATHERS_VOICE_PHASE);
		loadOnPresetRoad(p, &triangle);
		EventList none;
		(void)hosted.run(kF, none);
		CHECK(soundingVoice(p) == slot);
		const double hz = 440.0 * std::exp2((pitch - 69.0) / 12.0);
		double expected = phase0 + hz * static_cast<double>(kF) / kSampleRate;
		expected -= std::floor(expected);
		const double phase1 = voiceValue(p, slot, CLAP_FEATHERS_VOICE_PHASE);
		double diff = std::fabs(phase1 - expected);
		diff = std::min(diff, 1.0 - diff); // on the circle
		CAPTURE(phase0);
		CAPTURE(phase1);
		CAPTURE(expected);
		CHECK(diff < 1e-6);
		CHECK(voiceValue(p, slot, CLAP_FEATHERS_VOICE_PITCH) == doctest::Approx(60.0).epsilon(1e-9));
	}
}

TEST_CASE("a variant load that changes the engine hands a held note to the new engine: it sounds on, on Additive") {
	// Morph, Stair, V5, Resonate, Basic Shapes and Additive all render a held voice from the next tick's character, so a
	// held note crosses an engine change sounding. (Pluck and Drum excite only at a note-on: K12, KS design §10.5.)
	HostedPlugin hosted;
	const clap_plugin* p = hosted.plugin();
	EventList ev;
	(void)hosted.run(64, ev);
	StateBytes v5;
	REQUIRE(saveState(p, &v5));
	EventList toAdditive;
	toAdditive.pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Additive));
	(void)hosted.run(64, toAdditive);
	StateBytes additive;
	REQUIRE(saveState(p, &additive));
	REQUIRE(loadState(p, &v5)); // the starting patch on the host's road: only the switch takes the preset road
	mainThreadVisit(p);
	REQUIRE(engineParam(p) == static_cast<double>(feathers::OscType::V5));
	constexpr uint32_t kF = 256;
	EventList on;
	on.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	(void)hosted.run(kF, on);
	float last = 0.0f;
	(void)largestStep(hosted, kF, 20, &last);
	const int slot = soundingVoice(p);
	REQUIRE(slot >= 0);
	loadOnPresetRoad(p, &additive);
	CHECK(engineParam(p) == static_cast<double>(feathers::OscType::Additive)); // the switch happened
	float peak = 0.0f;
	(void)largestStep(hosted, kF, 20, &last, &peak);
	CHECK(soundingVoice(p) == slot);
	CHECK(peak > 0.1f);
}

TEST_CASE("first block after activate: a host key with Pattern seated reaches the voices and its note-off reaches them too — never a stuck voice; and Pattern holds nothing once the key is let go (backlog QR, the soak's first-block lead)") {
	namespace pt = feathers::mod::arp::pat;
	for (const pt::Mode mode : { pt::Mode::Melody, pt::Mode::TbGlide }) {
		CAPTURE(static_cast<int>(mode));
		static uint32_t seated = UINT32_MAX;
		HostedPlugin host(&kHost, [](const clap_plugin* p, void*) {
			const auto* c = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
			REQUIRE(c != nullptr);
			seated = c->add_mod_device(p, pt::kTraits.tag);
			REQUIRE(seated != UINT32_MAX);
		});
		const clap_plugin* p = host.plugin();
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		REQUIRE(chain->set_control(p, CLAP_FEATHERS_TEST_STRIP | seated, "P1 Mode", static_cast<double>(mode)));
		// STOPPED: Pattern (Run on Transport) plays nothing, so whatever reaches the voices is the host's key itself.
		host.setTransport(0.0, false, false);
		EventList on;
		on.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 7, 1.0);
		host.run(kMaxFrames, on); // THE VERY FIRST BLOCK after activate
		EventList none;
		for (int b = 0; b < 20; ++b) {
			host.run(kMaxFrames, none);
		}
		EventList off;
		off.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 7, 0.0);
		host.run(kMaxFrames, off);
		for (int b = 0; b < 20; ++b) {
			host.run(kMaxFrames, none);
		}
		// EVERY ON THE VOICES WERE HANDED HAS ITS OFF.
		std::map<int32_t, int> balance;
		uint32_t kind = 0;
		int32_t key = 0;
		uint64_t sample = 0;
		size_t delivered = 0;
		for (uint32_t i = 0; chain->delivered_note(p, i, &kind, &key, &sample); ++i) {
			balance[key] += kind == 0 ? 1 : (kind == 1 || kind == 2 ? -1 : 0);
			++delivered;
		}
		for (const auto& [k, n] : balance) {
			CAPTURE(k);
			CHECK(n <= 0); // no voice left on
		}
		// AND PATTERN HOLDS NOTHING: the transport runs and, with no key down, it plays nothing (Trig Gate).
		REQUIRE(chain->delivered_note(p, UINT32_MAX, nullptr, nullptr, nullptr));
		for (int b = 0; b < 200; ++b) {
			host.setTransport(static_cast<double>(b * kMaxFrames) / kSampleRate, true, false);
			host.run(kMaxFrames, none);
		}
		size_t onsAfter = 0;
		for (uint32_t i = 0; chain->delivered_note(p, i, &kind, &key, &sample); ++i) {
			onsAfter += kind == 0 ? 1 : 0;
		}
		CHECK(onsAfter == 0);
		MESSAGE("mode " << static_cast<int>(mode) << ": the voices were handed " << delivered << " events around the key");
	}
}

TEST_CASE("a key in the first block after start, or after a state load into a running instance, is Pattern's: it plays exactly what the same key plays once Pattern is long installed, and its off leaves nothing sounding (backlog QU)") {
	namespace pt = feathers::mod::arp::pat;
	// THE KEY'S NOTES, `lead` blocks after the start (or the load), as (sample from the key, key) — what Pattern makes of it.
	const auto notesOf = [](bool afterLoad, int lead) {
		static uint32_t seated = UINT32_MAX;
		HostedPlugin host(&kHost, [](const clap_plugin* p, void*) {
			const auto* c = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
			REQUIRE(c != nullptr);
			seated = c->add_mod_device(p, pt::kTraits.tag);
			REQUIRE(seated != UINT32_MAX);
		});
		const clap_plugin* p = host.plugin();
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		REQUIRE(chain->set_control(p, CLAP_FEATHERS_TEST_STRIP | seated, "Run", 0.0)); // Always: it plays from the key, song or not
		EventList none;
		if (afterLoad) {
			for (int b = 0; b < 8; ++b) {
				host.run(kMaxFrames, none);
			}
			StateBytes s;
			REQUIRE(saveState(p, &s));
			REQUIRE(loadState(p, &s));
		}
		host.setTransport(0.0, false, false); // stopped: Always runs Pattern's own clock from the first key
		for (int b = 0; b < lead; ++b) {
			host.run(kMaxFrames, none);
		}
		REQUIRE(chain->delivered_note(p, UINT32_MAX, nullptr, nullptr, nullptr));
		EventList on;
		on.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 7, 1.0);
		host.run(kMaxFrames, on);
		for (int b = 0; b < 120; ++b) {
			host.run(kMaxFrames, none);
		}
		EventList off;
		off.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 7, 0.0);
		host.run(kMaxFrames, off);
		for (int b = 0; b < 60; ++b) {
			host.run(kMaxFrames, none);
		}
		std::vector<std::pair<int64_t, int32_t>> ons;
		std::map<int32_t, int> balance;
		uint32_t kind = 0;
		int32_t key = 0;
		uint64_t sample = 0;
		uint64_t first = 0;
		bool haveFirst = false;
		for (uint32_t i = 0; chain->delivered_note(p, i, &kind, &key, &sample); ++i) {
			balance[key] += kind == 0 ? 1 : (kind == 1 || kind == 2 ? -1 : 0);
			if (kind == 0) {
				if (!haveFirst) {
					first = sample;
					haveFirst = true;
				}
				ons.emplace_back(static_cast<int64_t>(sample - first), key);
			}
		}
		for (const auto& [k, n] : balance) {
			CAPTURE(k);
			CHECK(n == 0); // nothing left sounding
		}
		return ons;
	};
	for (const bool afterLoad : { false, true }) {
		CAPTURE(afterLoad);
		const auto first = notesOf(afterLoad, 0);
		const auto later = notesOf(afterLoad, 6);
		REQUIRE(later.size() >= 2); // the reference plays: not vacuous
		CHECK(first == later);      // the first block's key is Pattern's, as a later one is
	}
}

//------------------------------------------------------------------------------------------------//
// THE MIDI 1.0 DIALECT. Expected values are restated from the MIDI 1.0 specification (status byte,  //
// 14-bit bend centred on 8192, CC 64 sustain at 64 and above, CC 120 / 123) and from CLAP's note   //
// event definition (note id -1 for a host with none, velocity 0..1) -- never read back from the    //
// code under test.                                                                                 //
//------------------------------------------------------------------------------------------------//

namespace {

struct Delivered {
	uint32_t kind;
	int32_t key;
	uint64_t sample;
	bool operator==(const Delivered& o) const { return kind == o.kind && key == o.key && sample == o.sample; }
};

std::vector<Delivered> deliveredOf(const clap_plugin* p) {
	std::vector<Delivered> out;
	Delivered d{};
	for (uint32_t i = 0; testChainOf(p)->delivered_note(p, i, &d.kind, &d.key, &d.sample); ++i) {
		out.push_back(d);
	}
	return out;
}

double synthValue(const clap_plugin* p, uint32_t what) {
	double v = -1.0;
	REQUIRE(testChainOf(p)->synth_reading(p, what, &v));
	return v;
}

/// Whether any voice is in `stage` (`Voice::Stage` as a number).
int voicesInStage(const clap_plugin* p, uint32_t stage) {
	int n = 0;
	for (uint32_t slot = 0; slot < static_cast<uint32_t>(feathers::kVoiceSlots); ++slot) {
		if (voiceValue(p, static_cast<int>(slot), CLAP_FEATHERS_VOICE_STAGE) == static_cast<double>(stage)) {
			++n;
		}
	}
	return n;
}

constexpr uint32_t kStageSustain = 2;
constexpr uint32_t kStageRelease = 3;

/// The pitch of the one sounding voice after a few blocks, with `events` in the first.
double pitchAfter(HostedPlugin& host, const EventList& events) {
	EventList none;
	host.run(kBlock, events);
	for (int i = 0; i < 4; ++i) {
		host.run(kBlock, none);
	}
	const int slot = soundingVoice(host.plugin());
	REQUIRE(slot >= 0);
	return voiceValue(host.plugin(), slot, CLAP_FEATHERS_VOICE_PITCH);
}

} // namespace

TEST_CASE("MIDI dialect: a note on and off reaches the voices as the equivalent CLAP note does, key and sample and pitch") {
	for (const uint32_t offset : { 0u, 37u, 200u }) {
		CAPTURE(offset);
		HostedPlugin clapHost;
		HostedPlugin midiHost;
		EventList none;
		clapHost.run(kBlock, none);
		midiHost.run(kBlock, none);
		REQUIRE(testChainOf(clapHost.plugin())->delivered_note(clapHost.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
		REQUIRE(testChainOf(midiHost.plugin())->delivered_note(midiHost.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));

		EventList viaClap;
		viaClap.pushNote(CLAP_EVENT_NOTE_ON, offset, 64, -1, 100.0 / 127.0);
		EventList viaMidi;
		viaMidi.pushMidi(offset, 0x90, 64, 100);
		const double clapPitch = pitchAfter(clapHost, viaClap);
		const double midiPitch = pitchAfter(midiHost, viaMidi);
		CHECK(midiPitch == clapPitch);
		CHECK(midiPitch == doctest::Approx(64.0).epsilon(1e-9)); // key 64, no transposition in the default patch
		CHECK(synthValue(midiHost.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 1.0);

		// THE NOTE IS HANDED TO THE VOICES AT ITS OWN SAMPLE: the block starts at 256 (one block was rendered first).
		const std::vector<Delivered> clapSeen = deliveredOf(clapHost.plugin());
		const std::vector<Delivered> midiSeen = deliveredOf(midiHost.plugin());
		REQUIRE(midiSeen.size() == 1);
		CHECK(midiSeen == clapSeen);
		CHECK(midiSeen[0].kind == 0);
		CHECK(midiSeen[0].key == 64);
		CHECK(midiSeen[0].sample == kBlock + offset);

		// The 0x80 off, at its own offset in a later block, ends the key as the CLAP off does.
		const uint64_t before = 6 * static_cast<uint64_t>(kBlock);
		EventList offClap;
		offClap.pushNote(CLAP_EVENT_NOTE_OFF, 11, 64, -1, 64.0 / 127.0);
		EventList offMidi;
		offMidi.pushMidi(11, 0x80, 64, 64);
		clapHost.run(kBlock, offClap);
		midiHost.run(kBlock, offMidi);
		CHECK(synthValue(midiHost.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 0.0);
		const std::vector<Delivered> midiAll = deliveredOf(midiHost.plugin());
		REQUIRE(midiAll.size() == 2);
		CHECK(midiAll == deliveredOf(clapHost.plugin()));
		CHECK(midiAll[1].kind == 1);
		CHECK(midiAll[1].key == 64);
		CHECK(midiAll[1].sample == before + 11);
		CHECK(voicesInStage(midiHost.plugin(), kStageRelease) == 1);
	}
}

TEST_CASE("MIDI dialect: a note on with velocity 0 is a note off, at its sample") {
	HostedPlugin host;
	EventList none;
	EventList on;
	on.pushMidi(0, 0x90, 60, 90);
	host.run(kBlock, on);
	REQUIRE(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 1.0);
	REQUIRE(testChainOf(host.plugin())->delivered_note(host.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	EventList zero;
	zero.pushMidi(73, 0x90, 60, 0);
	host.run(kBlock, zero);
	CHECK(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 0.0);
	const std::vector<Delivered> seen = deliveredOf(host.plugin());
	REQUIRE(seen.size() == 1);
	CHECK(seen[0].kind == 1);
	CHECK(seen[0].key == 60);
	CHECK(seen[0].sample == kBlock + 73);
	CHECK(voicesInStage(host.plugin(), kStageRelease) == 1);
}

TEST_CASE("MIDI dialect: the channel is the status byte's low nibble") {
	HostedPlugin host;
	EventList on;
	on.pushMidi(0, 0x95, 60, 100); // channel 5
	on.pushMidi(0, 0x90, 67, 100); // channel 0
	host.run(kBlock, on);
	REQUIRE(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 2.0);
	// Channel 5's key 60 off on channel 0 releases nothing: key 60 is not down there.
	EventList wrong;
	wrong.pushMidi(0, 0x80, 60, 0);
	host.run(kBlock, wrong);
	CHECK(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 2.0);
	EventList right;
	right.pushMidi(0, 0x85, 60, 0);
	host.run(kBlock, right);
	CHECK(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 1.0);
}

TEST_CASE("MIDI dialect: pitch bend 16383, 12288, 8192 and 0 bend by +1, 4096/8191, 0 and -1 of the bend range, on its own channel") {
	// THE EXPECTED PITCH IS THE KEY PLUS THE WHEEL (the MIDI 1.0 14-bit value, centre 8192, the upper half over 8191 so
	// 16383 is exactly +1 and the lower over 8192 so 0 is exactly -1) TIMES THE RANGE THE `Bend Range` PARAM NAMES.
	struct Range { double index; double semitones; };
	for (const Range range : { Range { 3.0, 12.0 }, Range { 1.0, 2.0 } }) {
		CAPTURE(range.semitones);
		HostedPlugin host;
		EventList setup;
		setup.pushParam(0, static_cast<clap_id>(feathers::ParamId::BendRange), range.index);
		setup.pushMidi(0, 0x90, 60, 100);
		const double centred = pitchAfter(host, setup);
		REQUIRE(centred == doctest::Approx(60.0).epsilon(1e-9));
		struct Case { int bend; double wheel; };
		for (const Case c : { Case { 16383, 1.0 }, Case { 12288, 4096.0 / 8191.0 }, Case { 8192, 0.0 }, Case { 0, -1.0 },
		                      Case { 4096, -0.5 } }) {
			CAPTURE(c.bend);
			EventList bend;
			bend.pushMidi(0, 0xE0, static_cast<uint8_t>(c.bend & 0x7F), static_cast<uint8_t>(c.bend >> 7));
			const double pitch = pitchAfter(host, bend);
			CHECK(pitch == doctest::Approx(60.0 + c.wheel * range.semitones).epsilon(1e-9));
		}
		// A bend on ANOTHER channel leaves this channel's voice where it was.
		EventList centre;
		centre.pushMidi(0, 0xE0, 0x00, 0x40);
		REQUIRE(pitchAfter(host, centre) == doctest::Approx(60.0).epsilon(1e-9));
		EventList other;
		other.pushMidi(0, 0xE3, 0x7F, 0x7F);
		CHECK(pitchAfter(host, other) == doctest::Approx(60.0).epsilon(1e-9));
	}
}

TEST_CASE("MIDI dialect: a pitch bend takes effect at its sample, not at the block start") {
	// The bend rides the note lane's clock like a note: the same voice, bent at sample 200 of a block, is still at its
	// centred pitch when a block ending before 200 is read -- so the block is cut at the event.
	HostedPlugin host;
	EventList on;
	on.pushMidi(0, 0x90, 60, 100);
	const double base = pitchAfter(host, on);
	REQUIRE(base == doctest::Approx(60.0).epsilon(1e-9));
	EventList late;
	late.pushMidi(200, 0xE0, 0x7F, 0x7F); // +1 at sample 200
	EventList none;
	host.run(200, none); // a short block that ends exactly where the event would sit
	const int slot = soundingVoice(host.plugin());
	REQUIRE(slot >= 0);
	CHECK(voiceValue(host.plugin(), slot, CLAP_FEATHERS_VOICE_PITCH) == doctest::Approx(60.0).epsilon(1e-9));
	host.run(kBlock, late);
	for (int i = 0; i < 4; ++i) {
		host.run(kBlock, none);
	}
	CHECK(voiceValue(host.plugin(), slot, CLAP_FEATHERS_VOICE_PITCH) == doctest::Approx(72.0).epsilon(1e-9));
}

TEST_CASE("MIDI dialect: CC 64 at 64 holds a released note, at 63 does not, and the pedal coming up releases it") {
	EventList none;
	// The note is struck in one block and let go in the next, as a player does: a gate that opens and shuts inside one
	// control tick never strikes, which is not what this case is about.
	const auto play = [&](HostedPlugin& h, uint8_t pedalStatus, uint8_t pedalValue) {
		EventList strike;
		strike.pushMidi(0, pedalStatus, 64, pedalValue);
		strike.pushMidi(0, 0x90, 60, 100);
		h.run(kBlock, strike);
		h.run(kBlock, none);
		REQUIRE(voicesInStage(h.plugin(), kStageSustain) == 1);
		EventList lift;
		lift.pushMidi(10, 0x80, 60, 0);
		h.run(kBlock, lift);
		h.run(kBlock, none);
	};
	HostedPlugin host;
	play(host, 0xB0, 64); // down
	// Released by the key but held by the pedal: the voice is still in its sustain, not its release.
	CHECK(voicesInStage(host.plugin(), kStageSustain) == 1);
	CHECK(voicesInStage(host.plugin(), kStageRelease) == 0);
	EventList up;
	up.pushMidi(0, 0xB0, 64, 0);
	host.run(kBlock, up);
	CHECK(voicesInStage(host.plugin(), kStageSustain) == 0);
	CHECK(voicesInStage(host.plugin(), kStageRelease) == 1);

	SUBCASE("a value of 63 is up") {
		HostedPlugin other;
		play(other, 0xB0, 63);
		CHECK(voicesInStage(other.plugin(), kStageSustain) == 0);
		CHECK(voicesInStage(other.plugin(), kStageRelease) == 1);
	}
	SUBCASE("the pedal is per channel") {
		HostedPlugin other;
		play(other, 0xB1, 127); // down on channel 1 only
		CHECK(voicesInStage(other.plugin(), kStageSustain) == 0);
		CHECK(voicesInStage(other.plugin(), kStageRelease) == 1);
	}
}

TEST_CASE("MIDI dialect: CC 123 releases every note of its channel only; CC 120 chokes them; other CCs and messages do nothing") {
	const auto keysDown = [](HostedPlugin& h) { return synthValue(h.plugin(), CLAP_FEATHERS_SYNTH_KEYS_DOWN); };
	EventList none;
	{
		HostedPlugin host;
		EventList on;
		on.pushMidi(0, 0x90, 60, 100);
		on.pushMidi(0, 0x90, 64, 100);
		on.pushMidi(0, 0x91, 67, 100);
		host.run(kBlock, on);
		REQUIRE(keysDown(host) == 3.0);
		EventList off;
		off.pushMidi(0, 0xB1, 123, 0); // channel 1's
		host.run(kBlock, off);
		CHECK(keysDown(host) == 2.0); // channel 0's two keys stay down
		CHECK(voicesInStage(host.plugin(), kStageRelease) == 1);
		CHECK(voicesInStage(host.plugin(), kStageSustain) == 2);
	}
	{
		HostedPlugin host;
		EventList on;
		on.pushMidi(0, 0x90, 60, 100);
		on.pushMidi(0, 0x90, 64, 100);
		on.pushMidi(0, 0x91, 67, 100);
		host.run(kBlock, on);
		EventList off;
		off.pushMidi(0, 0xB1, 120, 0); // channel 1's
		host.run(kBlock, off);
		CHECK(keysDown(host) == 2.0);
		// Choked: faded out by the steal fade and ended well inside the default release, which is a second.
		for (int i = 0; i < 8; ++i) {
			host.run(kBlock, none);
		}
		CHECK(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_SOUNDING_VOICES) == 2.0);
		CHECK(voicesInStage(host.plugin(), kStageSustain) == 2);
	}
	{
		HostedPlugin host;
		EventList on;
		on.pushMidi(0, 0x90, 60, 100);
		host.run(kBlock, on);
		EventList noise;
		noise.pushMidi(0, 0xB0, 1, 127);   // mod wheel: no destination in v1
		noise.pushMidi(0, 0xB0, 7, 0);     // channel volume
		noise.pushMidi(0, 0xD0, 99, 0);    // channel pressure
		noise.pushMidi(0, 0xC0, 5, 0);     // program change
		noise.pushMidi(0, 0xA0, 60, 100);  // polyphonic pressure
		noise.pushMidi(0, 0xF8, 0, 0);     // a system message
		host.run(kBlock, noise);
		CHECK(keysDown(host) == 1.0);
		CHECK(voicesInStage(host.plugin(), kStageSustain) == 1);
		CHECK(voicesInStage(host.plugin(), kStageRelease) == 0);
	}
}

namespace {
/// A DAW'S TRANSPORT: both timelines and a tempo, the song at `beats`.
clap_event_transport songTransport(double beats, double bpm, bool playing) {
	clap_event_transport t {};
	t.header.size = sizeof(t);
	t.header.type = CLAP_EVENT_TRANSPORT;
	t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
	if (playing) {
		t.flags |= CLAP_TRANSPORT_IS_PLAYING;
	}
	t.tempo = bpm;
	t.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
	t.song_pos_seconds = static_cast<clap_sectime>(std::llround(beats * 60.0 / bpm * CLAP_SECTIME_FACTOR));
	return t;
}

/// ONE HOST BLOCK of a transport sweep: its length, the transport it opens with, and optionally a transport event inside
/// it and a key pressed in it.
struct SweepBlock {
	uint32_t frames = 0;
	bool playing = false;
	double beats = 0.0;
	bool event = false;
	uint32_t eventAt = 0;
	bool eventPlaying = false;
	double eventBeats = 0.0;
	bool key = false;
	uint32_t keyAt = 0;
};

/// **WHERE THE STEPS FALL, RESTATED FROM THE HOST'S OWN WORD**: every 1/16 (a quarter beat) the song passes while the
/// transport plays, as the sample the host would put it on — counted from the instance's first render, as
/// `delivered_note` counts. A stretch that plays from song position p at block sample s for n samples holds every grid
/// point in [p, p + n samples).
std::vector<int64_t> expectedSteps(const std::vector<SweepBlock>& blocks, double bpm) {
	const double samplesPerBeat = 60.0 / bpm * kSampleRate;
	std::vector<int64_t> out;
	int64_t at = 0;
	for (const SweepBlock& b : blocks) {
		const auto stretch = [&](uint32_t from, double beats) {
			const double end = beats + static_cast<double>(b.frames - from) / samplesPerBeat;
			for (auto g = static_cast<int64_t>(std::ceil(beats / 0.25 - 1e-9)); static_cast<double>(g) * 0.25 < end - 1e-9; ++g) {
				const double samples = (static_cast<double>(g) * 0.25 - beats) * samplesPerBeat;
				out.push_back(at + from + static_cast<int64_t>(std::ceil(samples - 1e-6)));
			}
		};
		if (b.playing) {
			stretch(0, b.beats);
		}
		else if (b.event && b.eventPlaying) {
			stretch(b.eventAt, b.eventBeats);
		}
		at += b.frames;
	}
	// A POINT IN A STRETCH'S LAST FRACTION OF A SAMPLE FALLS ON THE NEXT SAMPLE: past the run's end it is never rendered,
	// and where a loop wraps there it is the same instant as the loop's start, one onset.
	std::erase_if(out, [&](int64_t s) { return s >= at; });
	out.erase(std::unique(out.begin(), out.end()), out.end());
	return out;
}

/// THE HOST'S TIMELINE FOR ONE SCENARIO of the sweep (`kSweepScenarios`), in blocks of `frames` at `bpm`, the key
/// pressed before the play (`keyBefore`) or in the play's own block, at the play's sample.
std::vector<SweepBlock> sweepTimeline(int scenario, uint32_t frames, double bpm, bool keyBefore) {
	const double beatsPerSample = bpm / 60.0 / kSampleRate;
	std::vector<SweepBlock> out;
	const auto stopped = [&](int n, double beats) {
		for (int i = 0; i < n; ++i) {
			out.push_back(SweepBlock { .frames = frames, .playing = false, .beats = beats });
		}
	};
	// PLAYS for `beatsLong` beats of song from `beats`, block-aligned; the first block takes the key if it is pressed with
	// the play. Returns the position the song reached.
	const auto play = [&](double beats, double beatsLong, bool keyHere) {
		const double end = beats + beatsLong;
		bool first = true;
		while (beats < end) {
			SweepBlock b { .frames = frames, .playing = true, .beats = beats };
			b.key = first && keyHere;
			out.push_back(b);
			first = false;
			beats += frames * beatsPerSample;
		}
		return beats;
	};
	stopped(2, 0.0);
	if (keyBefore) {
		out.push_back(SweepBlock { .frames = frames, .playing = false, .beats = 0.0, .key = true, .keyAt = frames / 3 });
	}
	stopped(6, 0.0);
	const bool keyWithPlay = !keyBefore;
	switch (scenario) {
	case 0: // ON THE BOUNDARY AT SONG ZERO
		play(0.0, 3.0, keyWithPlay);
		break;
	case 1: // FROM BAR 3
		play(8.0, 3.0, keyWithPlay);
		break;
	case 2: { // THE PLAY ANNOUNCED INSIDE THE BLOCK, at a sample past its start (song zero there)
		const uint32_t o = std::max<uint32_t>(1, frames * 5 / 8);
		SweepBlock b { .frames = frames, .playing = false, .beats = 0.0, .event = true, .eventAt = o, .eventPlaying = true,
			           .eventBeats = 0.0, .key = keyWithPlay, .keyAt = o };
		out.push_back(b);
		play((frames - o) * beatsPerSample, 3.0, false);
		break;
	}
	case 3: { // THE SAME, FROM BAR 3
		const uint32_t o = std::max<uint32_t>(1, frames * 5 / 8);
		SweepBlock b { .frames = frames, .playing = false, .beats = 8.0, .event = true, .eventAt = o, .eventPlaying = true,
			           .eventBeats = 8.0, .key = keyWithPlay, .keyAt = o };
		out.push_back(b);
		play(8.0 + (frames - o) * beatsPerSample, 3.0, false);
		break;
	}
	case 4: // A PRE-ROLL: the first playing block a hair before the song's zero (negative)
		play(-0.01, 3.0, keyWithPlay);
		break;
	case 5: // A PRE-ROLL INTO BAR 2: a few samples before the boundary
		play(4.0 - 37.0 * beatsPerSample, 3.0, keyWithPlay);
		break;
	case 6: // STOP, THEN START AGAIN AT THE SAME PLACE
		play(4.0, 1.5, keyWithPlay);
		stopped(6, 4.0);
		play(4.0, 1.5, false);
		break;
	case 7: { // A ONE-BEAT LOOP, THE HOST CUTTING THE BLOCK AT ITS END
		double beats = 0.0;
		bool first = true;
		for (int pass = 0; pass < 4; ++pass) {
			while (beats < 1.0 - 1e-12) {
				const double left = (1.0 - beats) / beatsPerSample;
				const auto n = static_cast<uint32_t>(std::min<double>(frames, std::ceil(left - 1e-9)));
				SweepBlock b { .frames = n, .playing = true, .beats = beats, .key = first && keyWithPlay };
				out.push_back(b);
				first = false;
				beats += n * beatsPerSample;
			}
			beats -= 1.0;
		}
		break;
	}
	case 8: { // A ONE-BEAT LOOP, THE HOST JUMPING AT THE NEXT BLOCK (no cut): four beats of song played
		double beats = 0.0;
		double total = 0.0;
		bool first = true;
		while (total < 4.0) {
			out.push_back(SweepBlock { .frames = frames, .playing = true, .beats = beats, .key = first && keyWithPlay });
			first = false;
			beats += frames * beatsPerSample;
			total += frames * beatsPerSample;
			if (beats >= 1.0) {
				beats -= 1.0;
			}
		}
		break;
	}
	default:
		break;
	}
	return out;
}
inline constexpr int kSweepScenarios = 9;

/// RUNS `blocks` through a fresh instance with the note device `tag` in its modulator lane (set up by `configure`) and
/// returns the samples of every note-on the voices were handed.
std::vector<int64_t> sweepOns(const char* tag, bool (*configure)(const clap_plugin*, const clap_plugin_feathers_test_chain*, uint32_t),
                              const std::vector<SweepBlock>& blocks, uint32_t maxFrames, double bpm) {
	static const char* seatedTag = nullptr;
	static uint32_t seated = UINT32_MAX;
	seatedTag = tag;
	HostedPlugin host(&kHost, [](const clap_plugin* p, void*) {
		const auto* c = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(c != nullptr);
		seated = c->add_mod_device(p, seatedTag);
		REQUIRE(seated != UINT32_MAX);
	});
	const clap_plugin* p = host.plugin();
	if (maxFrames > kMaxFrames) {
		p->stop_processing(p);
		p->deactivate(p);
		REQUIRE(p->activate(p, kSampleRate, 1, maxFrames));
		REQUIRE(p->start_processing(p));
		host.growBuffers(maxFrames);
	}
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(configure(p, chain, CLAP_FEATHERS_TEST_STRIP | seated));
	std::vector<int64_t> ons;
	uint32_t kind = 0;
	int32_t key = 0;
	uint64_t sample = 0;
	REQUIRE(chain->delivered_note(p, UINT32_MAX, nullptr, nullptr, nullptr));
	EventList events;
	for (const SweepBlock& b : blocks) {
		events.clear();
		host.setTransportEvent(songTransport(b.beats, bpm, b.playing));
		// IN TIME ORDER: a transport event before a key at the same sample (the key is pressed into the playing song).
		if (b.event && (!b.key || b.eventAt <= b.keyAt)) {
			events.pushTransport(b.eventAt, songTransport(b.eventBeats, bpm, b.eventPlaying));
		}
		if (b.key) {
			events.pushNote(CLAP_EVENT_NOTE_ON, b.keyAt, 60, 1, 1.0);
		}
		if (b.event && b.key && b.eventAt > b.keyAt) {
			events.pushTransport(b.eventAt, songTransport(b.eventBeats, bpm, b.eventPlaying));
		}
		host.run(b.frames, events);
		// DRAINED EVERY BLOCK, so the seam's log (1024 events) never wraps.
		for (uint32_t i = 0; chain->delivered_note(p, i, &kind, &key, &sample); ++i) {
			if (kind == 0) {
				ons.push_back(static_cast<int64_t>(sample));
			}
		}
		REQUIRE(chain->delivered_note(p, UINT32_MAX, nullptr, nullptr, nullptr));
	}
	return ons;
}

/// THE SWEEP ITSELF, over every scenario, block size, tempo and key timing; returns the failures, described.
std::vector<std::string> transportSweep(const char* tag,
                                        bool (*configure)(const clap_plugin*, const clap_plugin_feathers_test_chain*, uint32_t),
                                        bool withKey = true) {
	std::vector<std::string> failures;
	size_t runs = 0;
	for (const uint32_t frames : { 32u, 64u, 480u, 1024u }) {
		for (const double bpm : { 120.0, 133.3 }) {
			for (int scenario = 0; scenario < kSweepScenarios; ++scenario) {
				for (const bool keyBefore : { true, false }) {
					std::vector<SweepBlock> blocks = sweepTimeline(scenario, frames, bpm, keyBefore);
					if (!withKey) {
						for (SweepBlock& b : blocks) {
							b.key = false; // NO KEY AT ALL: the pattern runs on the transport alone
						}
					}
					const std::vector<int64_t> want = expectedSteps(blocks, bpm);
					const std::vector<int64_t> got = sweepOns(tag, configure, blocks, std::max(frames, kMaxFrames), bpm);
					++runs;
					bool same = want.size() == got.size() && !want.empty();
					for (size_t i = 0; same && i < want.size(); ++i) {
						same = std::llabs(want[i] - got[i]) <= 1;
					}
					if (!same) {
						std::string s = "frames " + std::to_string(frames) + " bpm " + std::to_string(bpm) + " scenario "
						                + std::to_string(scenario) + (keyBefore ? " key before" : " key with play") + ": want "
						                + std::to_string(want.size()) + " got " + std::to_string(got.size()) + " [";
						for (size_t i = 0; i < std::max(want.size(), got.size()) && i < 6; ++i) {
							s += (i < want.size() ? std::to_string(want[i]) : "-") + "/" + (i < got.size() ? std::to_string(got[i]) : "-") + " ";
						}
						failures.push_back(s + "]");
					}
				}
			}
		}
	}
	REQUIRE(runs == 4 * 2 * kSweepScenarios * 2);
	return failures;
}
} // namespace

TEST_CASE("RA: an Arp in Feathers' modulator lane plays every 1/16 the transport passes over a held key — from song zero, "
          "bar 3, inside the block, a pre-roll, a restart and a loop, at 32 to 1024 frames and 120 or 133.3 bpm") {
	const auto configure = [](const clap_plugin*, const clap_plugin_feathers_test_chain*, uint32_t) { return true; };
	const std::vector<std::string> failures = transportSweep(feathers::mod::arp::kTraits.tag, configure);
	for (const std::string& f : failures) {
		MESSAGE(f);
	}
	CHECK(failures.empty());
}

TEST_CASE("RA: a Pattern in Feathers' modulator lane, every step a note, plays every 1/16 the transport passes over a held "
          "key — from song zero, bar 3, inside the block, a pre-roll, a restart and a loop, at 32 to 1024 frames and 120 or "
          "133.3 bpm") {
	// EVERY STEP A NOTE: no rests (Density), no ties (Hold), no grace notes (Detail).
	const auto configure = [](const clap_plugin* p, const clap_plugin_feathers_test_chain* c, uint32_t slot) {
		return c->set_control(p, slot, "P1 Density", 0.0) && c->set_control(p, slot, "P1 Hold", 0.0)
		       && c->set_control(p, slot, "P1 Detail", 0.0);
	};
	const std::vector<std::string> failures = transportSweep(feathers::mod::arp::pat::kTraits.tag, configure);
	for (const std::string& f : failures) {
		MESSAGE(f);
	}
	CHECK(failures.empty());
}

TEST_CASE("RA: a Pattern on Trig Transport in Feathers' modulator lane, no key held, plays every 1/16 the transport passes — "
          "from song zero, bar 3, inside the block, a pre-roll, a restart and a loop, at 32 to 1024 frames and 120 or 133.3 bpm") {
	// THE NOTES THAT TRIGGER ON TRANSPORT PLAY with nothing else to wake them: Trig Transport plays its idle chord unkeyed.
	const auto configure = [](const clap_plugin* p, const clap_plugin_feathers_test_chain* c, uint32_t slot) {
		return c->set_control(p, slot, "P1 Density", 0.0) && c->set_control(p, slot, "P1 Hold", 0.0)
		       && c->set_control(p, slot, "P1 Detail", 0.0) && c->set_control(p, slot, "P1 Trig", 1.0);
	};
	const std::vector<std::string> failures = transportSweep(feathers::mod::arp::pat::kTraits.tag, configure, false);
	for (const std::string& f : failures) {
		MESSAGE(f);
	}
	CHECK(failures.empty());
}

namespace {
/// **A SELF-CLOCKED NOTE DEVICE KEEPS THE PLUGIN AWAKE BETWEEN ITS STEPS** (RB): the note device `tag` in the modulator
/// lane (set up by `configure`), one step a bar at 120 bpm, a note far shorter than the step, so the voice's release has
/// run out long before the next step and blocks go exactly silent with a step still to come. Runs `held` seconds with the
/// transport `playing` and, when `key`, a key struck and let go at once under Latch (the chord plays on with no host key
/// down: a key held at the host holds the default patch's sound up through the gap, and the gap would not be silent);
/// then lets go (Latch off; with no key, the device switched off) and stops the transport, and reports what it saw.
struct WakeRun {
	int sleepsWhileScheduled = 0; // SLEEP while the device still had a step to play: the defect
	int silentWhileScheduled = 0; // exactly silent blocks in that stretch: what makes the first count mean something
	size_t ons = 0;               // the steps that sounded in it
	bool sleptAfter = false;      // SLEEP reached once the key is let go / the transport stopped
	int blocksToSleep = 0;
};
WakeRun wakeRun(const char* tag, bool (*configure)(const clap_plugin*, const clap_plugin_feathers_test_chain*, uint32_t),
                bool key, bool playing, double held) {
	static const char* seatedTag = nullptr;
	static uint32_t seated = UINT32_MAX;
	seatedTag = tag;
	HostedPlugin host(&kHost, [](const clap_plugin* p, void*) {
		const auto* c = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(c != nullptr);
		seated = c->add_mod_device(p, seatedTag);
		REQUIRE(seated != UINT32_MAX);
	});
	const clap_plugin* p = host.plugin();
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(configure(p, chain, CLAP_FEATHERS_TEST_STRIP | seated));
	constexpr uint32_t kFrames = 256;
	constexpr double kBpm = 120.0;
	EventList none;
	host.setTransportEvent(songTransport(0.0, kBpm, false));
	for (int b = 0; b < 8; ++b) {
		host.run(kFrames, none); // the strip adopts its device and its controls
	}
	REQUIRE(chain->delivered_note(p, UINT32_MAX, nullptr, nullptr, nullptr));
	WakeRun r;
	EventList on;
	on.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	EventList off;
	off.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	const auto heldBlocks = static_cast<int>(held * kSampleRate / kFrames);
	double beats = 0.0;
	uint32_t kind = 0;
	int32_t k = 0;
	uint64_t sample = 0;
	for (int b = 0; b < heldBlocks; ++b) {
		host.setTransportEvent(songTransport(beats, kBpm, playing));
		const clap_process_status status = host.run(kFrames, b == 0 && key ? on : (b == 1 && key ? off : none));
		if (playing) {
			beats += kFrames * kBpm / 60.0 / kSampleRate;
		}
		r.sleepsWhileScheduled += status == CLAP_PROCESS_SLEEP ? 1 : 0;
		r.silentWhileScheduled += peakOf(host.left(), kFrames) == 0.0f && peakOf(host.right(), kFrames) == 0.0f ? 1 : 0;
	}
	for (uint32_t i = 0; chain->delivered_note(p, i, &kind, &k, &sample); ++i) {
		r.ons += kind == 0 ? 1 : 0;
	}
	// LET GO: Latch off; with no key, the device switched off (a stopped pattern on Trig Transport is armed, and stays
	// awake for the transport by design).
	REQUIRE(chain->set_control(p, CLAP_FEATHERS_TEST_STRIP | seated, key ? "Latch" : "Enabled", 0.0));
	for (int b = 0; b < 30 * static_cast<int>(kSampleRate) / static_cast<int>(kFrames); ++b) {
		host.setTransportEvent(songTransport(beats, kBpm, false));
		const clap_process_status status = host.run(kFrames, none);
		++r.blocksToSleep;
		if (status == CLAP_PROCESS_SLEEP) {
			r.sleptAfter = true;
			break;
		}
	}
	return r;
}
} // namespace

TEST_CASE("RB: a latched chord under an Arp keeps the plugin awake between its steps — SLEEP is never promised while a step is "
          "still to come, on the free clock or the song's, and once Latch lets go SLEEP is reached") {
	// ONE STEP A BAR (2 s at 120 bpm), the shortest gate: each note is 20 ms and its release a second, so most of every
	// step is exactly silent. Run Always, so the stopped transport plays it on the free clock; Latch, so the chord plays
	// on after the key is up.
	const auto configure = [](const clap_plugin* p, const clap_plugin_feathers_test_chain* c, uint32_t slot) {
		return c->set_control(p, slot, "Time Base", 19.0) && c->set_control(p, slot, "Gate", 0.01)
		       && c->set_control(p, slot, "Run", 0.0) && c->set_control(p, slot, "Latch", 1.0);
	};
	for (const bool playing : { false, true }) {
		CAPTURE(playing);
		const WakeRun r = wakeRun(feathers::mod::arp::kTraits.tag, configure, true, playing, 7.0);
		CAPTURE(r.ons);
		CAPTURE(r.silentWhileScheduled);
		CAPTURE(r.blocksToSleep);
		CHECK(r.ons == 4);                     // steps at 0, 2, 4 and 6 s: the device did play on
		CHECK(r.silentWhileScheduled > 100);   // the gaps were silent: not vacuous
		CHECK(r.sleepsWhileScheduled == 0);    // and SLEEP was never promised in them
		CHECK(r.sleptAfter);                   // let go, it sleeps — not a plugin kept awake for ever
	}
}

TEST_CASE("RB: a Pattern on Trig Transport keeps the plugin awake between its steps while the transport plays, with no key "
          "held — and once it is switched off and the transport stops SLEEP is reached") {
	const auto configure = [](const clap_plugin* p, const clap_plugin_feathers_test_chain* c, uint32_t slot) {
		return c->set_control(p, slot, "P1 Time Base", 19.0) && c->set_control(p, slot, "P1 Gate", 0.05)
		       && c->set_control(p, slot, "P1 Density", 0.0) && c->set_control(p, slot, "P1 Hold", 0.0)
		       && c->set_control(p, slot, "P1 Detail", 0.0) && c->set_control(p, slot, "P1 Trig", 1.0);
	};
	const WakeRun r = wakeRun(feathers::mod::arp::pat::kTraits.tag, configure, false, true, 7.0);
	CAPTURE(r.ons);
	CAPTURE(r.silentWhileScheduled);
	CAPTURE(r.blocksToSleep);
	CHECK(r.ons == 4);
	CHECK(r.silentWhileScheduled > 100);
	CHECK(r.sleepsWhileScheduled == 0);
	CHECK(r.sleptAfter);
}

TEST_CASE("RB: a stopped Pattern on Trig Transport is armed — the plugin stays awake with no key and no event, the "
          "transport starting in a block with no input event plays its first step on the block's first sample, and "
          "switched off it lets the plugin sleep") {
	// CLAP promises a sleeping plugin a call only on an event or a change in its audio input. A transport start is
	// neither, so a pattern that starts with the transport must not have let the plugin sleep while it waited.
	static uint32_t seated = UINT32_MAX;
	HostedPlugin host(&kHost, [](const clap_plugin* p, void*) {
		const auto* c = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(c != nullptr);
		seated = c->add_mod_device(p, feathers::mod::arp::pat::kTraits.tag);
		REQUIRE(seated != UINT32_MAX);
	});
	const clap_plugin* p = host.plugin();
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(p->get_extension(p, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = CLAP_FEATHERS_TEST_STRIP | seated;
	REQUIRE(chain->set_control(p, slot, "P1 Trig", 1.0)); // Transport
	REQUIRE(chain->set_control(p, slot, "P1 Density", 0.0)); // every step a note, so step 0 sounds
	REQUIRE(chain->set_control(p, slot, "P1 Hold", 0.0));
	constexpr uint32_t kFrames = 256;
	constexpr double kBpm = 120.0;
	EventList none;
	int64_t at = 0; // samples rendered, as `delivered_note` counts them
	int sleeps = 0;
	int silent = 0;
	host.setTransportEvent(songTransport(0.0, kBpm, false));
	for (int b = 0; b < 400; ++b) { // two seconds stopped, no key, no event
		const clap_process_status status = host.run(kFrames, none);
		at += kFrames;
		sleeps += status == CLAP_PROCESS_SLEEP ? 1 : 0;
		silent += peakOf(host.left(), kFrames) == 0.0f && peakOf(host.right(), kFrames) == 0.0f ? 1 : 0;
	}
	CHECK(silent == 400);  // nothing sounded: only the armed pattern could keep it awake
	CHECK(sleeps == 0);
	REQUIRE(chain->delivered_note(p, UINT32_MAX, nullptr, nullptr, nullptr));
	// THE TRANSPORT STARTS AT SONG ZERO, in a block that carries no event at all.
	const int64_t start = at;
	double beats = 0.0;
	for (int b = 0; b < 4; ++b) {
		host.setTransportEvent(songTransport(beats, kBpm, true));
		host.run(kFrames, none);
		at += kFrames;
		beats += kFrames * kBpm / 60.0 / kSampleRate;
	}
	uint32_t kind = 0;
	int32_t key = 0;
	uint64_t sample = 0;
	int64_t first = -1;
	for (uint32_t i = 0; first < 0 && chain->delivered_note(p, i, &kind, &key, &sample); ++i) {
		if (kind == 0) {
			first = static_cast<int64_t>(sample);
		}
	}
	CHECK(first == start); // step 0, on the song's zero: the block's first sample
	// SWITCHED OFF, AND THE TRANSPORT STOPPED: it sleeps.
	REQUIRE(chain->set_control(p, slot, "Enabled", 0.0));
	bool slept = false;
	host.setTransportEvent(songTransport(beats, kBpm, false));
	for (int b = 0; b < 30 * static_cast<int>(kSampleRate) / static_cast<int>(kFrames) && !slept; ++b) {
		slept = host.run(kFrames, none) == CLAP_PROCESS_SLEEP;
	}
	CHECK(slept);
}

//------------------------------------------------------------------------------------------------//
// CLAP_EVENT_NOTE_END. The contract is the header's: the plugin tells the host, after processing the //
// frames, that the voice for a note the HOST issued has ended, carrying the port, channel, key and //
// note id the host's note-on carried.                                                              //
//------------------------------------------------------------------------------------------------//

namespace {

std::vector<OutputLog::Event> noteEndsIn(const OutputLog& log) {
	std::vector<OutputLog::Event> out;
	for (const OutputLog::Event& e : log.events) {
		if (e.type == CLAP_EVENT_NOTE_END) {
			out.push_back(e);
		}
	}
	return out;
}

/// Run blocks, appending what the plugin sent, until `blocks` have gone or `stop` says so.
template <class Stop>
int runCollecting(HostedPlugin& host, OutputLog& log, int blocks, Stop stop) {
	EventList none;
	int ran = 0;
	while (ran < blocks) {
		host.run(kBlock, none, &log);
		++ran;
		if (stop()) {
			break;
		}
	}
	return ran;
}

} // namespace

TEST_CASE("NOTE_END: a host note gets exactly one, with its port, channel, key and id, once its release tail has ended") {
	HostedPlugin host;
	OutputLog log;
	EventList on;
	on.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 7, 1.0);
	host.run(kBlock, on, &log);
	runCollecting(host, log, 8, [] { return false; });
	CHECK(noteEndsIn(log).empty()); // sounding and held: not over
	EventList off;
	off.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 7, 0.0);
	host.run(kBlock, off, &log);
	CHECK(noteEndsIn(log).empty()); // released, tail still running
	// The tail runs for about a second; the end comes in the block the last voice stops sounding in, never before.
	int blocks = 0;
	while (noteEndsIn(log).empty() && blocks < 4 * 48000 / static_cast<int>(kBlock)) {
		CHECK(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_SOUNDING_VOICES) >= 1.0);
		runCollecting(host, log, 1, [] { return false; });
		++blocks;
	}
	REQUIRE(noteEndsIn(log).size() == 1);
	CHECK(synthValue(host.plugin(), CLAP_FEATHERS_SYNTH_SOUNDING_VOICES) == 0.0);
	CHECK(blocks > 1);
	const OutputLog::Event end = noteEndsIn(log)[0];
	CHECK(end.noteId == 7);
	CHECK(end.key == 60);
	CHECK(end.channel == 0);
	CHECK(end.port == 0);
	runCollecting(host, log, 100, [] { return false; });
	CHECK(noteEndsIn(log).size() == 1); // once
}

TEST_CASE("NOTE_END: a MIDI-dialect note (id -1) ends with id -1 on its own channel") {
	HostedPlugin host;
	OutputLog log;
	EventList on;
	on.pushMidi(0, 0x92, 64, 100); // channel 2
	host.run(kBlock, on, &log);
	EventList off;
	off.pushMidi(0, 0x82, 64, 0);
	host.run(kBlock, off, &log);
	runCollecting(host, log, 4 * 48000 / static_cast<int>(kBlock), [&] { return !noteEndsIn(log).empty(); });
	REQUIRE(noteEndsIn(log).size() == 1);
	CHECK(noteEndsIn(log)[0].noteId == -1);
	CHECK(noteEndsIn(log)[0].key == 64);
	CHECK(noteEndsIn(log)[0].channel == 2);
}

TEST_CASE("NOTE_END: a stolen voice's old note is ended once, at the steal") {
	HostedPlugin host;
	OutputLog log;
	EventList none;
	EventList setup;
	setup.pushParam(0, static_cast<clap_id>(feathers::ParamId::VoiceCount), 2.0);
	host.run(kBlock, setup, &log);
	const int16_t keys[3] = { 60, 64, 67 };
	for (int i = 0; i < 3; ++i) {
		EventList on;
		on.pushNote(CLAP_EVENT_NOTE_ON, 0, keys[i], 1 + i, 1.0);
		host.run(kBlock, on, &log);
		runCollecting(host, log, 3, [] { return false; });
		if (i < 2) {
			CHECK(noteEndsIn(log).empty());
		}
	}
	// Two voices, three notes: the oldest (id 1, key 60) was stolen and is over, though its key is still down.
	REQUIRE(noteEndsIn(log).size() == 1);
	CHECK(noteEndsIn(log)[0].noteId == 1);
	CHECK(noteEndsIn(log)[0].key == 60);
	// Releasing everything ends the other two once each, and the stolen one is not ended again.
	EventList off;
	for (int i = 0; i < 3; ++i) {
		off.pushNote(CLAP_EVENT_NOTE_OFF, 0, keys[i], 1 + i, 0.0);
	}
	host.run(kBlock, off, &log);
	runCollecting(host, log, 8 * 48000 / static_cast<int>(kBlock), [&] { return noteEndsIn(log).size() >= 3; });
	runCollecting(host, log, 100, [] { return false; });
	std::map<int32_t, int> seen;
	for (const OutputLog::Event& e : noteEndsIn(log)) {
		++seen[e.noteId];
	}
	CHECK(seen.size() == 3);
	for (const auto& [id, n] : seen) {
		CAPTURE(id);
		CHECK(n == 1);
	}
}

TEST_CASE("NOTE_END: a choke ends the note in the block it is sent in, once") {
	HostedPlugin host;
	OutputLog log;
	EventList on;
	on.pushNote(CLAP_EVENT_NOTE_ON, 0, 62, 5, 1.0);
	host.run(kBlock, on, &log);
	runCollecting(host, log, 4, [] { return false; });
	REQUIRE(noteEndsIn(log).empty());
	EventList choke;
	choke.pushNote(CLAP_EVENT_NOTE_CHOKE, 0, 62, 5, 0.0);
	host.run(kBlock, choke, &log);
	REQUIRE(noteEndsIn(log).size() == 1);
	CHECK(noteEndsIn(log)[0].noteId == 5);
	CHECK(noteEndsIn(log)[0].key == 62);
	runCollecting(host, log, 100, [] { return false; });
	CHECK(noteEndsIn(log).size() == 1); // the steal fade's end adds none
}

TEST_CASE("NOTE_END: N notes give N ends, one per id, none repeated over many blocks") {
	HostedPlugin host;
	OutputLog log;
	constexpr int kN = 6;
	EventList on;
	for (int i = 0; i < kN; ++i) {
		on.pushNote(CLAP_EVENT_NOTE_ON, static_cast<uint32_t>(i * 3), static_cast<int16_t>(48 + i * 2), 10 + i, 1.0);
	}
	host.run(kBlock, on, &log);
	runCollecting(host, log, 6, [] { return false; });
	EventList off;
	for (int i = 0; i < kN; ++i) {
		off.pushNote(CLAP_EVENT_NOTE_OFF, 0, static_cast<int16_t>(48 + i * 2), 10 + i, 0.0);
	}
	host.run(kBlock, off, &log);
	runCollecting(host, log, 8 * 48000 / static_cast<int>(kBlock), [&] { return noteEndsIn(log).size() >= kN; });
	runCollecting(host, log, 300, [] { return false; });
	std::map<int32_t, std::pair<int, int>> seen; // id -> count, key
	for (const OutputLog::Event& e : noteEndsIn(log)) {
		++seen[e.noteId].first;
		seen[e.noteId].second = e.key;
	}
	REQUIRE(seen.size() == static_cast<size_t>(kN));
	for (int i = 0; i < kN; ++i) {
		CAPTURE(i);
		CHECK(seen[10 + i].first == 1);
		CHECK(seen[10 + i].second == 48 + i * 2);
	}
}

TEST_CASE("NOTE_END: voices an arp plays on keys and ids the host never sent produce no end for them") {
	constexpr int kBlocks = 12;
	uint32_t seated = UINT32_MAX;
	HostedPlugin host(&kHost, [](const clap_plugin* p, void* out) {
		*static_cast<uint32_t*>(out) = testChainOf(p)->add_mod_device(p, feathers::mod::arp::kTraits.tag);
	}, &seated);
	REQUIRE(seated != UINT32_MAX);
	const auto* chain = testChainOf(host.plugin());
	EventList none;
	host.setTransport(0.0, false, false);
	host.run(kMaxFrames, none);
	mainThreadVisit(host.plugin());
	const double two[2] = { 2.0, 2.0 };
	REQUIRE(chain->drag_control(host.plugin(), CLAP_FEATHERS_TEST_STRIP | seated, "Octaves", two, 2));
	REQUIRE(chain->delivered_note(host.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	OutputLog log;
	int64_t at = kMaxFrames;
	EventList chord;
	chord.pushNote(CLAP_EVENT_NOTE_ON, 0, 60, 1, 1.0);
	chord.pushNote(CLAP_EVENT_NOTE_ON, 0, 64, 2, 1.0);
	bool first = true;
	for (int b = 0; b < kBlocks * 200; ++b) {
		host.setTransport(static_cast<double>(at) / kSampleRate, true, false);
		host.run(kMaxFrames, first ? chord : none, &log);
		first = false;
		at += kMaxFrames;
	}
	EventList off;
	off.pushNote(CLAP_EVENT_NOTE_OFF, 0, 60, 1, 0.0);
	off.pushNote(CLAP_EVENT_NOTE_OFF, 0, 64, 2, 0.0);
	host.setTransport(static_cast<double>(at) / kSampleRate, true, false);
	host.run(kMaxFrames, off, &log);
	at += kMaxFrames;
	for (int b = 0; b < 400; ++b) {
		host.setTransport(static_cast<double>(at) / kSampleRate, true, false);
		host.run(kMaxFrames, none, &log);
		at += kMaxFrames;
	}
	// NOT VACUOUS: the arp did play keys the host never sent.
	bool inventedKey = false;
	uint32_t kind = 0;
	int32_t key = 0;
	uint64_t sample = 0;
	for (uint32_t i = 0; chain->delivered_note(host.plugin(), i, &kind, &key, &sample); ++i) {
		inventedKey = inventedKey || (kind == 0 && key != 60 && key != 64);
	}
	REQUIRE(inventedKey);
	// What the host hears is only about the notes it sent: keys 60 and 64 with ids 1 and 2, each at most once.
	std::map<int32_t, int> seen;
	for (const OutputLog::Event& e : noteEndsIn(log)) {
		CAPTURE(e.key);
		CAPTURE(e.noteId);
		CHECK(((e.key == 60 && e.noteId == 1) || (e.key == 64 && e.noteId == 2)));
		++seen[e.key];
	}
	for (const auto& [k, n] : seen) {
		CAPTURE(k);
		CHECK(n == 1);
	}
}
