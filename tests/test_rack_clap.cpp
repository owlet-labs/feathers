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
#include "doctest.h"
#include "menu_placement_check.h"
#include <thread>
#include <atomic>

#include "device/format_value.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/filter/filter_types.h"
#include "fx/sklitter/sklitter_types.h"
#include "fx/spectral/spectral_types.h"
#include "plugin/ui/generic_card.h"
#include "plugin/ui/knob.h"
#include "plugin/ui/mod_state.h"
#include "plugin/ui/source_arm.h"
#include "plugin/ui/switch_chip.h"
#include "plugin/ui/value_box.h"
#include <visage/app.h>
#include "fx/resonate/resonate_types.h"
#include "hostslots/slot_host.h"
#include "plugin/clap_checking.h"
#include "hostslots/slot_pool.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/mseg/mseg_types.h"
#include "mod/lfo/lfo.h"
#include "plugin/device_preset.h"
#include "plugin/device_registry.h"
#include "rack/rack_axes.h"
#include "rack/rack_bridge.h"
#include "rack/rack_clap.h"
#include "rack/rack_device.h"
#include "rack/rack_engine.h"
#include "rack/rack_gui.h"
#include "rack/rack_editor.h"
#include "plugin/ui/chrome_hue.h"
#include "plugin/ui/chrome_hue_prefs.h"
#include "plugin/editor_prefs.h"
#include "plugin/ui/settings_screen.h"
#include "plugin/ui/libra_door.h"
#include "plugin/feathers_test_chain.h"
#include "rack/rack_test_ext.h"
#include "state/state_trailer.h"
#include "plugin/ui/device_control.h"
#include "fx/harm/harm_types.h"
#include "mod/arp/pattern_types.h"
#include "fx/automod/automod.h"
#include "fx/disperser/disperser.h"
#include "fx/phaser/phaser.h"
#include "fx/waypoint/waypoint_types.h"
#include "mod/noteexpr/noteexpr_types.h"
#include "mod/tracker/tracker_types.h"
#include "mod/arp/arp_types.h"
#include "mod/arp/pattern_types.h"
#include "spectrum.h"
#include "plugin/ui/preset_browser.h"
#include "plugin/editor_prefs.h"
#include "plugin/ui/plugin_window.h"
#include "plugin/ui/rail.h"
#include "plugin/ui/theme.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <limits>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <cstring>
#include <map>
#include <limits>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unistd.h>

#if defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#endif

// THE RACK, DRIVEN THROUGH THE REAL CLAP API — ports, parameters, notes and audio, with a host.
//
// The engine's own suite proves the arithmetic with no host at all; this one proves the BINDING, which
// is the part a suite over `RackEngine` cannot see: whether a parameter event reaches a macro, whether
// a note reaches the gate, whether the input actually arrives at the output.

using namespace feathers;

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

/// The smallest host clap-helpers will accept. Every callback is a no-op: this venue never asks the
/// plugin to restart or to call back, so a host that recorded such requests would only be recording
/// that they did not happen.
clap_host makeHost() {
	clap_host h {};
	h.clap_version = CLAP_VERSION_INIT;
	h.name = "rack-venue";
	h.vendor = "Owlet";
	h.url = "";
	h.version = "1";
	// TIMER SUPPORT IS ADVERTISED, and it has to be: `guiCreate` refuses on Linux unless the host offers
	// posix-fd or a timer, because an editor the host never drives is a dead one that looks alive. A
	// venue that did not offer one would be testing that refusal rather than the editor.
	h.get_extension = [](const clap_host*, const char* id) -> const void* {
		static const clap_host_timer_support kTimer {
			[](const clap_host*, uint32_t, clap_id* id) {
				*id = 1;
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

/// A parameter event queue holding one event.
struct OneEvent {
	clap_event_param_value value {};
	clap_input_events in {};

	OneEvent(clap_id id, double v) {
		value.header.size = sizeof(value);
		value.header.time = 0;
		value.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		value.header.type = CLAP_EVENT_PARAM_VALUE;
		value.header.flags = 0;
		value.param_id = id;
		value.value = v;
		value.note_id = -1;
		value.port_index = -1;
		value.channel = -1;
		value.key = -1;
		in.ctx = this;
		in.size = [](const clap_input_events*) -> uint32_t { return 1; };
		in.get = [](const clap_input_events* list, uint32_t) -> const clap_event_header* {
			return &static_cast<const OneEvent*>(list->ctx)->value.header;
		};
	}
};

struct NoteEvent {
	clap_event_note note {};
	clap_input_events in {};

	NoteEvent(uint16_t type) {
		note.header.size = sizeof(note);
		note.header.time = 0;
		note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		note.header.type = type;
		note.header.flags = 0;
		note.note_id = 1;
		note.port_index = 0;
		note.channel = 0;
		note.key = 60;
		note.velocity = 1.0;
		in.ctx = this;
		in.size = [](const clap_input_events*) -> uint32_t { return 1; };
		in.get = [](const clap_input_events* list, uint32_t) -> const clap_event_header* {
			return &static_cast<const NoteEvent*>(list->ctx)->note.header;
		};
	}
};

clap_input_events emptyEvents() {
	clap_input_events in {};
	in.ctx = nullptr;
	in.size = [](const clap_input_events*) -> uint32_t { return 0; };
	in.get = [](const clap_input_events*, uint32_t) -> const clap_event_header* { return nullptr; };
	return in;
}

/// A control's index by its declared name — the same lookup two cases below already spell inline.
size_t controlNamed(const device::Descriptor& d, const char* name) {
	for (size_t i = 0; i < d.params.size(); ++i) {
		if (std::string(d.params[i].name).find(name) != std::string::npos) {
			return i;
		}
	}
	return d.params.size();
}

clap_output_events sinkEvents() {
	clap_output_events out {};
	out.ctx = nullptr;
	out.try_push = [](const clap_output_events*, const clap_event_header*) { return true; };
	return out;
}

/// A plugin, initialised and activated, with its buffers.
struct Venue {
	clap_host host = makeHost();
	const clap_plugin* plugin = nullptr;
	std::vector<float> l, r, inL, inR;
	clap_audio_buffer out {}, in {};
	float* outPtr[2] {};
	float* inPtr[2] {};

	explicit Venue(int frames = 512) : Venue(makeHost(), frames) {}
	/// A venue on a host of the case's own (the A9 cases' rescan log).
	explicit Venue(const clap_host& h, int frames = 512, bool noteRack = false)
	    : host(h), l(frames), r(frames), inL(frames), inR(frames) {
		plugin = noteRack ? rack::createNoteRack(&host) : rack::create(&host);
		REQUIRE(plugin != nullptr);
		REQUIRE(plugin->init(plugin));
		REQUIRE(plugin->activate(plugin, 48000.0, 1, static_cast<uint32_t>(frames)));
		REQUIRE(plugin->start_processing(plugin));
		outPtr[0] = l.data();
		outPtr[1] = r.data();
		inPtr[0] = inL.data();
		inPtr[1] = inR.data();
		out.data32 = outPtr;
		out.channel_count = 2;
		in.data32 = inPtr;
		in.channel_count = 2;
	}
	~Venue() {
		if (plugin != nullptr) {
			plugin->stop_processing(plugin);
			plugin->deactivate(plugin);
			plugin->destroy(plugin);
		}
	}

	const clap_plugin_feathers_rack_test* test() const {
		return static_cast<const clap_plugin_feathers_rack_test*>(
		    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	}

	/// Fill the input with something audible, so a device that does nothing is distinguishable.
	void tone() {
		for (size_t i = 0; i < inL.size(); ++i) {
			inL[i] = inR[i] =
			    0.5f * std::sin(6.2831853f * 220.0f * static_cast<float>(i) / 48000.0f);
		}
	}

	/// **WHAT A HOST DOES AFTER A LOAD — and since CD commit 3 a case has to do it too.**
	///
	/// On the ACTIVE road a patch is handed to the drain, so its refusals do not exist until a drain
	/// runs; a case that asks the tray what was said the instant `load` returns is asking before there
	/// is an answer. One block carries the patch (`process` is where the drain lives) and one timer
	/// tick is the main-thread visit that says what it refused (`collectLandedPatch`). This venue is a
	/// host that never processes on its own, so both have to be spelled.
	void settle() {
		run();
		const auto* timer = static_cast<const clap_plugin_timer_support*>(
		    plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
		if (timer != nullptr) {
			timer->on_timer(plugin, 0);
		}
	}

	clap_process_status run(const clap_input_events* events = nullptr) {
		clap_input_events none = emptyEvents();
		clap_output_events sink = sinkEvents();
		clap_process p {};
		p.frames_count = static_cast<uint32_t>(l.size());
		p.audio_inputs = &in;
		p.audio_inputs_count = 1;
		p.audio_outputs = &out;
		p.audio_outputs_count = 1;
		p.in_events = events != nullptr ? events : &none;
		p.out_events = &sink;
		return plugin->process(plugin, &p);
	}
};

/// **SIZE THE WINDOW THE WAY THE CLAP CONTRACT SAYS A HOST DOES: `adjust_size`, THEN `set_size` WITH
/// ITS ANSWER.** These cases called `set_size(kWidth, kHeight)` bare, which a release build survives and
/// a build without NDEBUG does not: clap-helpers terminates on a size `adjust_size` would have changed,
/// and it would have — the open canvas raises the floor above `kHeight` (720x560 against 720x594). Found
/// when the sanitizer build, which strips NDEBUG, first ran this suite (2026-10-03).
bool hostSetSize(const clap_plugin_gui* gui, const clap_plugin* plugin, uint32_t width, uint32_t height) {
	if (!gui->adjust_size(plugin, &width, &height)) {
		return false;
	}
	return gui->set_size(plugin, width, height);
}

} // namespace

TEST_CASE("the rack declares itself an audio effect with one stereo port each way") {
	const clap_plugin_descriptor* d = rack::descriptor();
	REQUIRE(d != nullptr);
	REQUIRE(std::string(d->id) == "com.owlet.feathers-chain");
	// THE PRODUCT IS "Feathers Chain" since backlog NP (2026-10-18); "Rack" now names parallel chains.
	CHECK(std::string(d->name) == "Feathers Chain");
	bool isEffect = false;
	for (const char* const* f = d->features; f != nullptr && *f != nullptr; ++f) {
		if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0) {
			isEffect = true;
		}
	}
	REQUIRE(isEffect);

	Venue v;
	const auto* ports = static_cast<const clap_plugin_audio_ports*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_AUDIO_PORTS));
	REQUIRE(ports != nullptr);
	REQUIRE(ports->count(v.plugin, true) == 2); // In, and the Sidechain (design §10.1)
	REQUIRE(ports->count(v.plugin, false) == 1);
	clap_audio_port_info info {};
	REQUIRE(ports->get(v.plugin, 0, true, &info));
	REQUIRE(info.channel_count == 2);
	CHECK((info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0);
}

TEST_CASE("the surface is eight macros then kHostSlotCapacity slots") {
	// NAMED FROM THE CONSTANT (256 since 2026-09-26, 64 before), so the next growth does not falsify it.
	constexpr uint32_t kSlots = hostslots::kHostSlotCapacity;
	// **WAS "the surface is eight macros and nothing else"** — a 2026-08-30 decision, retired for slots only
	// on 2026-09-25, when the standalone rack came to own a pool: 8 macros, then the slots.
	// What that decision refused — a surface that GROWS with the tenants — is still refused: the count is fixed,
	// and a fresh rack's slots name no device at all.
	Venue v;
	const auto* params =
	    static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	REQUIRE(params->count(v.plugin) == 8 + kSlots);

	// THE MACROS FIRST, unchanged: ids 0..7, named as every build before this one named them.
	for (uint32_t i = 0; i < 8; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, i, &info));
		CAPTURE(i);
		CHECK(info.id == i);
		CHECK(std::string(info.name) == "Macro " + std::to_string(i + 1));
		CHECK(std::string(info.module) == "Macros");
	}
	// THEN THE SLOTS, ids 0x1000 + n in order — the scheme Feathers uses (`hostslots/slot_host.h`), spelled
	// here rather than read from it. EMPTY on a fresh rack: "Slot N", hidden, 0..1, automatable and modulatable.
	for (uint32_t n = 0; n < kSlots; ++n) {
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, 8 + n, &info));
		CAPTURE(n);
		CHECK(info.id == 0x1000u + n);
		CHECK(std::string(info.name) == "Slot " + std::to_string(n + 1));
		CHECK(std::string(info.module) == "Slots");
		CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) != 0);
		CHECK((info.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0);
		CHECK((info.flags & CLAP_PARAM_IS_MODULATABLE) != 0);
		CHECK(info.min_value == 0.0);
		CHECK(info.max_value == 1.0);
		double value = -1.0;
		REQUIRE(params->get_value(v.plugin, info.id, &value));
		CHECK(value == 0.0);
	}
	// AND NOTHING PAST THEM.
	// **ONLY WHERE A MISBEHAVING HOST IS SURVIVED.** Asking past the end IS host misbehaviour, and a build
	// without NDEBUG (a Debug build, and the sanitizer build, which strips it on purpose) has clap-helpers
	// TERMINATE on it before the plugin is reached — `rack_clap.cpp`'s PluginBase. That is the developer
	// half of the convention working, not a defect; the refusal asserted here is the musician's half.
#if defined(NDEBUG)
	clap_param_info past {};
	CHECK_FALSE(params->get_info(v.plugin, 8 + kSlots, &past));
	double none = -1.0;
	CHECK_FALSE(params->get_value(v.plugin, 0x1000u + kSlots, &none));
#endif
}

TEST_CASE("the rack's param index clap-helpers is told is the get_info walk's for every id, -1 for every other, and builds no info") {
	// `FeathersPlugin`'s case in test_clap_render, for the rack's surface: macros by id, then the slots.
	Venue v;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const auto* params = static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	const uint32_t count = params->count(v.plugin);
	std::vector<int32_t> byId(hostslots::kFirstSlotId + hostslots::kHostSlotCapacity + 64, -1);
	for (uint32_t i = 0; i < count; ++i) {
		clap_param_info info {};
		REQUIRE(params->get_info(v.plugin, i, &info));
		REQUIRE(info.id < byId.size());
		REQUIRE(byId[info.id] == -1);
		byId[info.id] = static_cast<int32_t>(i);
	}
	size_t known = 0;
	for (clap_id id = 0; id < byId.size(); ++id) {
		known += byId[id] >= 0 ? 1 : 0;
		if (chain->param_index_of(v.plugin, id) != byId[id]) {
			CAPTURE(id);
			CHECK(chain->param_index_of(v.plugin, id) == byId[id]);
		}
	}
	CHECK(known == count);
	CHECK(chain->param_index_of(v.plugin, UINT32_MAX) == -1);

	// NO WALK OF OURS, at the checking level this build runs (`plugin/clap_checking.h`): none at Minimal (release);
	// at Maximal (debug, the sanitizer gate) clap-helpers' range validation walks every param once per call.
	const uint64_t perCall = kClapChecking == clap::helpers::CheckingLevel::Maximal ? count : 0;
	clap_param_info last {};
	REQUIRE(params->get_info(v.plugin, count - 1, &last));
	double value = 0.0;
	uint64_t before = chain->param_info_builds(v.plugin);
	CHECK(params->get_value(v.plugin, last.id, &value));
	CHECK(chain->param_info_builds(v.plugin) - before == perCall);
	char text[256];
	before = chain->param_info_builds(v.plugin);
	CHECK(params->value_to_text(v.plugin, last.id, value, text, sizeof(text)));
	CHECK(chain->param_info_builds(v.plugin) - before == perCall);
}

TEST_CASE("an empty rack passes audio through unchanged") {
	Venue v;
	v.tone();
	REQUIRE(v.run() == CLAP_PROCESS_CONTINUE);
	REQUIRE(v.l == v.inL);
	REQUIRE(v.r == v.inR);
}

TEST_CASE("a device added through the venue is heard") {
	Venue v;
	const auto* t = v.test();
	REQUIRE(t != nullptr);
	REQUIRE(t->add_device(v.plugin, fx::filter::kTraits.tag) != 0); // an instance id, never 0

	v.tone();
	v.run();
	REQUIRE_FALSE(v.l == v.inL);
}

TEST_CASE("the chain venue can set a control on a device it has only just added, before any block") {
	// **THE DEPENDENCY CD COMMIT 2 CREATED, PINNED SO IT CANNOT BE QUIETLY UNDONE.**
	//
	// `chain_add_device` used to write the engine and `chain_set_control` used to read the engine's
	// descriptor back, and the two agreed because the write had already landed. Both now go through
	// the bridge, so the add is QUEUED — the engine does not hold the device until the next drain —
	// and `set_control` must take the descriptor from the MIRROR, which the add wrote optimistically.
	//
	// Point `chain_set_control` back at `engine_.slotAtLane(slot).desc` and this case is the one that
	// notices: the descriptor is null, every control lookup refuses, and a venue that adds a device
	// and configures it in the same breath — which is what `guihost` does — silently configures
	// nothing. That is why the two lines are pinned together here rather than left to agree.
	Venue v;
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);

	// ADDED THROUGH THE RACK TEST ROAD, which answers the device's ID (kill-lanes §2.4b) — the chain road
	// answers a POSITION, and the two are different numbers on purpose. Both go through the bridge, so the
	// dependency this case pins (the add is queued, the mirror answers before the drain) is unchanged.
	const auto* t = v.test();
	REQUIRE(t != nullptr);
	const uint32_t id = t->add_device(v.plugin, fx::filter::kTraits.tag);
	REQUIRE(id != 0);
	const uint32_t position = 0; // the only device, so the chain road's number for it

	// NO BLOCK BETWEEN THE TWO. That is the whole point of the case; a `v.run()` here would drain the
	// add and the engine would answer, which is exactly the coverage that would be lost.
	const size_t cutoff = controlNamed(fx::filter::kDescriptor, "Cutoff");
	REQUIRE(cutoff < fx::filter::kDescriptor.params.size());
	const device::ParamDesc& pd = fx::filter::kDescriptor.params[cutoff];
	const double want = pd.def == pd.min ? pd.max : pd.min;
	REQUIRE(chain->set_control(v.plugin, position, pd.name, want));

	// AND THE VALUE ARRIVES once a block has carried the queue — asserted as a VALUE rather than as
	// "the sound changed", which would pass for the add alone.
	v.tone();
	v.run();
	CHECK(t->resolved_field(v.plugin, id, cutoff) == doctest::Approx(static_cast<float>(want)));
}

TEST_CASE("a macro arrives as a parameter event and reads back through the host's own road") {
	Venue v;
	const auto* params =
	    static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
	double value = -1.0;
	REQUIRE(params->get_value(v.plugin, 0, &value));
	REQUIRE(value == doctest::Approx(0.0));

	OneEvent e(0, 0.75);
	v.run(&e.in);
	REQUIRE(params->get_value(v.plugin, 0, &value));
	REQUIRE(value == doctest::Approx(0.75));
}

TEST_CASE("a macro routed at a device control moves that control's RESOLVED VALUE") {
	// END TO END, through the real API: a DAW's automation lane reaches a device control it has never
	// heard of, by way of a route the user drew. Asserted as a VALUE.
	Venue v;
	const auto* t = v.test();

	const uint32_t slot = t->add_device(v.plugin, fx::filter::kTraits.tag); // its ID (kill-lanes §2.4b)
	REQUIRE(slot != 0);

	size_t cutoff = fx::filter::kDescriptor.params.size();
	for (size_t i = 0; i < fx::filter::kDescriptor.params.size(); ++i) {
		if (std::string(fx::filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	REQUIRE(cutoff < fx::filter::kDescriptor.params.size());

	v.tone();
	v.run();
	const float before = t->resolved_field(v.plugin, slot, cutoff);

	t->set_route(v.plugin, 0, 0, slot, static_cast<uint32_t>(cutoff), 0.5f); // macro 0 -> this device's cutoff
	OneEvent e(0, 1.0);
	v.run(&e.in);
	const float after = t->resolved_field(v.plugin, slot, cutoff);

	REQUIRE(after > before);
}

TEST_CASE("a note reaches the gate, so an envelope in the rack starts") {
	Venue v;
	const auto* t = v.test();

	const uint32_t fxSlot = t->add_device(v.plugin, fx::filter::kTraits.tag);
	const uint32_t envSlot = t->add_device(v.plugin, mod::dahdsr::kTraits.tag);
	REQUIRE(envSlot != 0);

	size_t cutoff = 0;
	for (size_t i = 0; i < fx::filter::kDescriptor.params.size(); ++i) {
		if (std::string(fx::filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	t->set_route(v.plugin, envSlot, 0, fxSlot, static_cast<uint32_t>(cutoff), 0.5f); // the envelope's output 0 -> cutoff

	v.tone();
	v.run();
	const float idle = t->resolved_field(v.plugin, fxSlot, cutoff);

	NoteEvent on(CLAP_EVENT_NOTE_ON);
	v.run(&on.in);
	for (int i = 0; i < 8; ++i) {
		v.run();
	}
	const float held = t->resolved_field(v.plugin, fxSlot, cutoff);
	REQUIRE(held > idle);

	// AND LETTING GO RELEASES IT ALL THE WAY BACK TO THE BASE.
	//
	// "It went down" is NOT enough and a known-bad proved it: deleting the note-off handler leaves the
	// gate latched, and a latched envelope still DECAYS from its peak to its sustain — so the value
	// falls either way and the case passed while the handler was gone. The assertion has to be that it
	// returns to where an unrouted control sits, which only a real release can do.
	const float base = t->resolved_field(v.plugin, fxSlot, cutoff);
	NoteEvent off(CLAP_EVENT_NOTE_OFF);
	v.run(&off.in);
	for (int i = 0; i < 400; ++i) {
		v.run();
	}
	const float released = t->resolved_field(v.plugin, fxSlot, cutoff);
	REQUIRE(released < held);
	REQUIRE(released == doctest::Approx(idle).epsilon(0.02));
	(void)base;
}

TEST_CASE("a second note under a held one restarts a rack MSEG, though the gate never moved") {
	// RETRIG FAILED WHEN THE ENVELOPE HAD NOT COMPLETED: a new note left the output at its
	// last level instead of starting it from the beginning. THE RACK'S HALF OF IT: the engine derived its
	// retrigger from the gate's rising edge and from nothing else, and a note under a note already
	// held raises no edge — so a legato line never restarted anything, whatever Single Trigger said.
	//
	// READ THROUGH A ROUTE, which is a linear road: resolved = base + amount * unit * source. The
	// MSEG's default shape rises from 0 to 1 and holds there under the gate, so the routed control
	// sits at base + K once the shape has arrived. A restart puts the shape back near 0 on the very
	// next block, so the offset falls to a small fraction of K; a shape that did not restart stays at
	// K, and one restarted from where it stood stays at K too, since the start of the default shape
	// is a rise and it was already at the top.
	Venue v;
	const auto* t = v.test();

	const uint32_t fxSlot = t->add_device(v.plugin, fx::filter::kTraits.tag);
	const uint32_t msegSlot = t->add_device(v.plugin, mod::mseg::kTraits.tag);
	REQUIRE(msegSlot != 0);

	size_t cutoff = 0;
	for (size_t i = 0; i < fx::filter::kDescriptor.params.size(); ++i) {
		if (std::string(fx::filter::kDescriptor.params[i].name).find("Cutoff") != std::string::npos) {
			cutoff = i;
			break;
		}
	}
	t->set_route(v.plugin, msegSlot, 0, fxSlot, static_cast<uint32_t>(cutoff), 0.5f); // the MSEG's output 0 -> cutoff

	v.tone();
	v.run();
	const float idle = t->resolved_field(v.plugin, fxSlot, cutoff);

	NoteEvent first(CLAP_EVENT_NOTE_ON);
	first.note.key = 60;
	first.note.note_id = 1;
	v.run(&first.in);
	// The default window is one second and the sustain breakpoint is at the end of segment 1, so
	// two thirds of a second in it is parked at the top. 100 blocks of 512 is just over a second.
	for (int i = 0; i < 100; ++i) {
		v.run();
	}
	const float held = t->resolved_field(v.plugin, fxSlot, cutoff);
	CAPTURE(idle);
	CAPTURE(held);
	REQUIRE(held > idle);
	const float span = held - idle;

	SUBCASE("the control: another block with nothing struck stays parked") {
		v.run();
		const float still = t->resolved_field(v.plugin, fxSlot, cutoff);
		CHECK(still == doctest::Approx(held).epsilon(0.001));
	}

	SUBCASE("a second key, struck while the first is held") {
		NoteEvent second(CLAP_EVENT_NOTE_ON);
		second.note.key = 67;
		second.note.note_id = 2;
		v.run(&second.in);
		const float after = t->resolved_field(v.plugin, fxSlot, cutoff);
		CAPTURE(after);
		// One block is 8 ticks of a 333 ms first segment: about 3% of the way back up from the start
		// level. A shape that did not restart reads the full span.
		CHECK(after - idle < 0.1f * span);
	}
}

TEST_CASE("rack plugin: a note's offset and velocity cross the plugin to the chain and back out") {
	// **THE PLUGIN'S HALF OF THE AUDIO+NOTES ROAD.** The plugin's catalogue is the build's, so a probe
	// device cannot be seated through CLAP; test_rack_engine.cpp seats one in the engine and drives it
	// as `process` does. What only this venue can see is the translation either side of it: the header
	// time becomes `timeSamples` (`collectNote`), the chain walks the notes, and what left it goes to
	// the host at the same time (`emitNotes`). An audio device in the chain passes them untouched.
	Venue v;
	REQUIRE(v.test()->add_device(v.plugin, fx::filter::kTraits.tag) != 0);
	v.settle();

	NoteEvent on(CLAP_EVENT_NOTE_ON);
	on.note.header.time = 100;
	on.note.key = 67;
	on.note.note_id = 5;
	on.note.velocity = 0.375;

	struct Captured {
		std::vector<clap_event_note> notes;
	} captured;
	clap_output_events sink {};
	sink.ctx = &captured;
	sink.try_push = [](const clap_output_events* list, const clap_event_header* h) {
		if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_NOTE_ON) {
			static_cast<Captured*>(list->ctx)->notes.push_back(*reinterpret_cast<const clap_event_note*>(h));
		}
		return true;
	};
	clap_process p {};
	p.frames_count = static_cast<uint32_t>(v.l.size());
	p.audio_inputs = &v.in;
	p.audio_inputs_count = 1;
	p.audio_outputs = &v.out;
	p.audio_outputs_count = 1;
	p.in_events = &on.in;
	p.out_events = &sink;
	REQUIRE(v.plugin->process(v.plugin, &p) == CLAP_PROCESS_CONTINUE);

	REQUIRE(captured.notes.size() == 1);
	CHECK(captured.notes[0].header.time == 100);
	CHECK(captured.notes[0].key == 67);
	CHECK(captured.notes[0].note_id == 5);
	CHECK(captured.notes[0].velocity == 0.375);
}

TEST_CASE("hardening: notes drained in a block with no output bus are dropped, not replayed with the next block") {
	Venue v;
	REQUIRE(v.test()->add_device(v.plugin, fx::filter::kTraits.tag) != 0);
	v.settle();

	NoteEvent on(CLAP_EVENT_NOTE_ON);
	on.note.key = 67;
	on.note.note_id = 9;
	on.note.velocity = 0.5;

	struct Captured {
		std::vector<clap_event_note> notes;
	};
	const auto sinkOf = [](Captured* c) {
		clap_output_events sink {};
		sink.ctx = c;
		sink.try_push = [](const clap_output_events* list, const clap_event_header* h) {
			if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_NOTE_ON) {
				static_cast<Captured*>(list->ctx)->notes.push_back(*reinterpret_cast<const clap_event_note*>(h));
			}
			return true;
		};
		return sink;
	};

	// THE BLOCK THAT HAS NO OUTPUT: the note is drained and there is nothing to deliver it into.
	Captured none;
	clap_output_events noneSink = sinkOf(&none);
	clap_process busless {};
	busless.frames_count = static_cast<uint32_t>(v.l.size());
	busless.audio_inputs = &v.in;
	busless.audio_inputs_count = 1;
	busless.audio_outputs_count = 0;
	busless.in_events = &on.in;
	busless.out_events = &noneSink;
	REQUIRE(v.plugin->process(v.plugin, &busless) == CLAP_PROCESS_CONTINUE);

	// THE NEXT BLOCK, with an output bus and NO events: a note left standing would come out of it.
	Captured next;
	clap_output_events nextSink = sinkOf(&next);
	clap_process p {};
	p.frames_count = static_cast<uint32_t>(v.l.size());
	p.audio_inputs = &v.in;
	p.audio_inputs_count = 1;
	p.audio_outputs = &v.out;
	p.audio_outputs_count = 1;
	p.out_events = &nextSink;
	REQUIRE(v.plugin->process(v.plugin, &p) == CLAP_PROCESS_CONTINUE);
	CHECK(next.notes.size() == 0);
}

TEST_CASE("hardening: a macro written past its range is held to 0..1 and one that is not a number is not a write") {
	Venue v;
	const auto* params =
	    static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
	double value = -1.0;

	OneEvent over(0, 7.5);
	v.run(&over.in);
	REQUIRE(params->get_value(v.plugin, 0, &value));
	CHECK(value == 1.0);

	OneEvent under(0, -3.0);
	v.run(&under.in);
	REQUIRE(params->get_value(v.plugin, 0, &value));
	CHECK(value == 0.0);

	OneEvent good(0, 0.25);
	v.run(&good.in);
	OneEvent nan(0, std::numeric_limits<double>::quiet_NaN());
	v.run(&nan.in);
	REQUIRE(params->get_value(v.plugin, 0, &value));
	CHECK(value == 0.25); // the NaN left what the macro held

	OneEvent inf(0, std::numeric_limits<double>::infinity());
	v.run(&inf.in);
	REQUIRE(params->get_value(v.plugin, 0, &value));
	CHECK(value == 0.25);
}


//------------------------------------------------------------------------------------------------//
// THE EDITOR, THROUGH THE HOST'S OWN ROAD.                                                         //
//------------------------------------------------------------------------------------------------//
//
// A REPAINT CANNOT BE OBSERVED HERE and nothing below claims one. What these check is the LIFECYCLE:
// that the plugin offers a gui at all, that it opens, that it reports a size a host can use, and that
// a second open does not rebuild the window.
//
// **AND THEY STOP SHORT OF EMBEDDING, WHICH IS SAID HERE BECAUSE THE TIMING GIVES IT AWAY.** This
// suite runs in ~0.03 s where Feathers' `gui-windowed` takes ~10 s, and the difference is real: an
// `ApplicationWindow` is CONSTRUCTED by `create` but only SHOWN by `set_parent`, which needs a real
// parent window nobody hands us here. `get_size` returning 720x560 rather than a native size is the
// same fact from the other end — that is `PluginWindow::size`'s fallback path, taken because
// `app->window()` is still null.
//
// So these cases are not vacuous — a known-bad making `create` refuse takes two of them red — but
// they cover the lifecycle UP TO the embed and not the embed itself. **The rack has no equivalent of
// `test_gui_windowed` yet**, and until it does, "the rack's editor appears in a host" is untested.
// Feathers' shell is shared and is exercised by that suite, which is most of the risk; what is
// unexercised is this file's own wiring of it.

TEST_CASE("the rack offers a gui, and only for the api it can embed into") {
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
	if (!rack::RackGui::isAvailable()) {
		REQUIRE(gui == nullptr);
		return;
	}
	REQUIRE(gui != nullptr);
#if defined(__APPLE__)
	REQUIRE(gui->is_api_supported(v.plugin, CLAP_WINDOW_API_COCOA, false));
#else
	REQUIRE(gui->is_api_supported(v.plugin, CLAP_WINDOW_API_X11, false));
#endif
	// FLOATING IS A WINDOW THE PLUGIN OWNS AND PLACES, which is deferred — so it must be refused
	// rather than accepted and then not delivered.
	REQUIRE_FALSE(gui->is_api_supported(v.plugin, CLAP_WINDOW_API_X11, true));
}

TEST_CASE("the editor opens, reports a usable size, and a second open reuses the window") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
	REQUIRE(gui != nullptr);

#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));

	uint32_t w = 0, h = 0;
	REQUIRE(gui->get_size(v.plugin, &w, &h));
	REQUIRE(w > 0);
	REQUIRE(h > 0);

	// A SECOND OPEN MUST NOT REBUILD. Rebuilding destroys a window whose swapchain threads are alive,
	// which wedges the host's main thread — see plugin_window.h. It cannot be observed from here as a
	// deadlock without risking one, so what is checked is the observable half: the editor is still
	// there and still reports the same size.
	gui->destroy(v.plugin);
	REQUIRE(gui->create(v.plugin, api, false));
	uint32_t w2 = 0, h2 = 0;
	REQUIRE(gui->get_size(v.plugin, &w2, &h2));
	REQUIRE(w2 == w);
	REQUIRE(h2 == h);

	gui->destroy(v.plugin);
}

TEST_CASE("the rack's editor reports the lane plus the canvas, and a fold changes the size it reports") {
	// THE CANVAS IS ROOM THE EDITOR ASKS THE HOST FOR: the
	// logical width `RackGui` answers is the lane's plus the canvas's, and a fold moves it. The ask
	// itself needs a native window to compare against and is proved by Feathers' windowed suite; what
	// is a value here is the size the host is told, before and after.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
	// BEFORE THE EDITOR EXISTS the canvas is as a fresh one is — FOLDED, the strip (2026-09-14) — so the
	// first size a host is told is the lane plus the strip.
	const uint32_t folded = rack::RackEditor::kHeight + static_cast<uint32_t>(ui::Rail::kCollapsedWidth);
	// OPEN AT THE CANVAS'S DEFAULT BAND: `kExtent` under the lane's cap, the lane's own height (backlog LX).
	const uint32_t open = rack::RackEditor::kHeight
	                      + static_cast<uint32_t>(std::min(ui::ExpandedCanvas::kExtent,
	                                                       static_cast<float>(rack::RackEditor::kHeight)
	                                                           * ui::ExpandedCanvas::kMaxShare));
	CHECK(gui.logicalHeight() == folded);
	CHECK(gui.minimumHeight() == folded);
	CHECK(gui.logicalWidth() == static_cast<uint32_t>(rack::RackEditor::kWidth));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui.create(api, false));
	REQUIRE(gui.editorForTest() != nullptr);
	CHECK(gui.logicalHeight() == folded);
	REQUIRE(gui.editorForTest()->canvas().toggleCollapsed()); // opened
	CHECK(gui.logicalHeight() == open);
	CHECK(gui.minimumHeight() == open);
	// AND THE FLOOR `adjust_size` APPLIES FOLLOWS: a host asking for less is raised to the open height.
	uint32_t w = 1, h = 1;
	REQUIRE(gui.adjustSize(&w, &h));
	CHECK(h >= open);
	REQUIRE(gui.editorForTest()->canvas().toggleCollapsed()); // folded again
	CHECK(gui.logicalHeight() == folded);
	REQUIRE(gui.editorForTest()->canvas().toggleCollapsed()); // and open, for the partition below
	// AND THE PARTITION: a moved one is a different preferred height, and the patch carries it.
	REQUIRE(gui.editorForTest()->canvas().setOpenExtent(300.0f));
	gui.editorForTest()->canvas().onExtentCommit();
	CHECK(gui.logicalHeight() == rack::RackEditor::kHeight + 300u);
	CHECK(bridge.canvasView().extent == doctest::Approx(300.0f));
	gui.destroy();
}

#if defined(__linux__)
namespace {

/// WHAT THE HOST WAS ASKED FOR, kept so a case can say what the editor requested on open.
struct ResizeLog {
	int count = 0;
	uint32_t width = 0;
	uint32_t height = 0;
};
ResizeLog gResizeLog;

/// `makeHost` plus the gui extension, because `request_resize` is how the editor says the size it wants and
/// clap-helpers will not speak it to a host that offers no gui extension. The request is RECORDED AND NOT
/// HONOURED, which is what Bitwig does at open (`tools/bitwighost.cpp`): the window stays the size `get_size`
/// answered until the hand drags it.
clap_host makeGuiHost() {
	clap_host h = makeHost();
	h.get_extension = [](const clap_host* host, const char* id) -> const void* {
		static const clap_host_gui kGui {
			[](const clap_host*) {},
			[](const clap_host*, uint32_t w, uint32_t hgt) {
				++gResizeLog.count;
				gResizeLog.width = w;
				gResizeLog.height = hgt;
				return true;
			},
			[](const clap_host*) { return true; },
			[](const clap_host*) { return true; },
			[](const clap_host*, bool) {},
		};
		if (id != nullptr && std::strcmp(id, CLAP_EXT_GUI) == 0) {
			return &kGui;
		}
		return makeHost().get_extension(host, id);
	};
	return h;
}

} // namespace

TEST_CASE("the rack's window, opened the way Bitwig opens it, takes its first drag at the size "
          "dragged to or at one unit tall — never a taller default") {
	// It once opened smaller and then snapped to a too-large default on the first resize; with the canvas
	// closed it need be no larger by default than one vertical unit. The road a host takes: create,
	// set_scale, get_size, a container that size, set_parent, show; then the hand drags an edge, which is
	// adjust_size and set_size with its answer.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	// ONE VERTICAL UNIT, restated rather than read back: the closed canvas's strip (34), the macro row (106),
	// a pad (12), one card unit (336) and the pad under it (12) — `RackEditor::resized` lays exactly these.
	const double oneUnit = static_cast<double>(ui::Rail::kCollapsedWidth) + 106.0 + 12.0
	                       + static_cast<double>(ui::kCardUnitHeight) + 12.0;
	REQUIRE(oneUnit == doctest::Approx(500.0));
	// THE UI SCALE IS A MULTIPLIER ON THE HOST'S; the venue's scratch config normally holds none (1.0).
	feathers::EditorPrefs prefs;
	prefs.load();
	const double uiScale = std::clamp(prefs.number(ui::PluginWindow::kUiScaleKey, 1.0), 1.0, 2.0);

	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	for (const double hostScale : { 1.0, 1.75 }) {
		CAPTURE(hostScale);
		const double scale = hostScale * uiScale;
		const auto unitRows = static_cast<uint32_t>(std::ceil(oneUnit * scale));
		gResizeLog = {};
		Venue v { makeGuiHost() };
		const auto* gui = static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
		REQUIRE(gui != nullptr);
		REQUIRE(gui->create(v.plugin, CLAP_WINDOW_API_X11, false));
		REQUIRE(gui->set_scale(v.plugin, hostScale));
		uint32_t openW = 0, openH = 0;
		REQUIRE(gui->get_size(v.plugin, &openW, &openH));
		const int screen = DefaultScreen(display);
		::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, openW, openH, 0,
		                                      BlackPixel(display, screen), BlackPixel(display, screen));
		XFlush(display);
		clap_window window {};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = parent;
		REQUIRE(gui->set_parent(v.plugin, &window));
		REQUIRE(gui->show(v.plugin));
		// WHAT THE EDITOR ASKED FOR ON OPEN: one unit tall, at the scale.
		if (gResizeLog.count > 0) {
			CHECK(gResizeLog.height <= unitRows);
		}

		// A DRAG ONE ROW TALLER: the window lands where it was dragged, or at one unit — never above it.
		uint32_t w = openW;
		uint32_t h = openH + 1;
		REQUIRE(gui->adjust_size(v.plugin, &w, &h));
		REQUIRE(gui->set_size(v.plugin, w, h));
		uint32_t gotW = 0, gotH = 0;
		REQUIRE(gui->get_size(v.plugin, &gotW, &gotH));
		CHECK(gotH == std::max(openH + 1, unitRows));

		// A DRAG TO A HEIGHT ABOVE ONE UNIT is kept exactly: the hand's size, not a default.
		const uint32_t dragged = unitRows + 40;
		w = gotW;
		h = dragged;
		REQUIRE(gui->adjust_size(v.plugin, &w, &h));
		CHECK(h == dragged);
		REQUIRE(gui->set_size(v.plugin, w, h));
		REQUIRE(gui->get_size(v.plugin, &gotW, &gotH));
		CHECK(gotH == dragged);
		gui->destroy(v.plugin);
		XDestroyWindow(display, parent);
	}
	XCloseDisplay(display);
}

TEST_CASE("THE DECISION: the rack opened in Bitwig's order (no set_scale) opens at its scaled size held to the "
          "screen, and its first drag never snaps") {
	// Both remedies at once: `get_size` before the embed answers the SCALED size, and the open size and `adjust_size`'s floor are held
	// to the usable screen. Bitwig's order (`tools/bitwighost.cpp`): create, get_size, a container that size, set_parent —
	// and no set_scale, so the scale is the display's (`FEATHERS_UI_SCALE` stands in for it here) times the UI scale.
	// THE SCREEN IS STUBBED (`PluginWindow::setUsableScreenForTest`): one that holds the design, one that does not.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	feathers::EditorPrefs prefs;
	prefs.load();
	const double uiScale = std::clamp(prefs.number(ui::PluginWindow::kUiScaleKey, 1.0), 1.0, 2.0);
	// THE DESIGN, restated: the lane's 720 plus the macro column at its left (12 + 2 x 100 Macros column; backlog OC); one vertical
	// unit tall with the canvas closed (LX: 34 + 106 + 12 + 336 + 12).
	const double designW = 720.0 + 12.0 + 200.0;
	const double designH = 500.0;
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	for (const char* scaleText : { "1", "1.75" }) {
		for (const auto& [screenW, screenH] : { std::pair<uint32_t, uint32_t> { 2560, 1400 }, { 1200, 800 } }) {
			CAPTURE(scaleText);
			CAPTURE(screenW);
			::setenv("FEATHERS_UI_SCALE", scaleText, 1);
			ui::PluginWindow::setUsableScreenForTest(screenW, screenH);
			const double scale = std::atof(scaleText) * uiScale;
			const auto wantW = std::min(static_cast<uint32_t>(std::ceil(designW * scale)), screenW);
			const auto wantH = std::min(static_cast<uint32_t>(std::ceil(designH * scale)), screenH);
			Venue v { makeGuiHost() };
			const auto* gui = static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
			REQUIRE(gui != nullptr);
			REQUIRE(gui->create(v.plugin, CLAP_WINDOW_API_X11, false));
			uint32_t openW = 0, openH = 0;
			REQUIRE(gui->get_size(v.plugin, &openW, &openH));
			// THE OPEN SIZE: the design at the scale, held to the screen.
			CHECK(openW == wantW);
			CHECK(openH == wantH);
			const int screen = DefaultScreen(display);
			::Window parent = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, openW, openH, 0,
			                                      BlackPixel(display, screen), BlackPixel(display, screen));
			XFlush(display);
			clap_window window {};
			window.api = CLAP_WINDOW_API_X11;
			window.x11 = parent;
			REQUIRE(gui->set_parent(v.plugin, &window));
			// THE WINDOW IS THE SIZE THE HOST WAS TOLD.
			uint32_t w = 0, h = 0;
			REQUIRE(gui->get_size(v.plugin, &w, &h));
			CHECK(w == openW);
			CHECK(h == openH);
			// THE FIRST DRAG, a row shorter: the floor is the open size, so it holds there — never taller, never past the
			// screen.
			w = openW;
			h = openH - 1;
			REQUIRE(gui->adjust_size(v.plugin, &w, &h));
			CHECK(w == openW);
			CHECK(h == openH);
			CHECK(h <= screenH);
			CHECK(w <= screenW);
			// AND A ROW TALLER, where the screen has it: kept exactly.
			if (openH < screenH) {
				w = openW;
				h = openH + 1;
				REQUIRE(gui->adjust_size(v.plugin, &w, &h));
				CHECK(h == openH + 1);
			}
			gui->destroy(v.plugin);
			XDestroyWindow(display, parent);
		}
	}
	::unsetenv("FEATHERS_UI_SCALE");
	ui::PluginWindow::setUsableScreenForTest(0, 0);
	XCloseDisplay(display);
}

namespace {

/// The windows the server says are inside `container`, and the first of them.
unsigned int windowsInside(Display* display, ::Window container, ::Window* first) {
	::Window root = 0, parentOf = 0, *kids = nullptr;
	unsigned int count = 0;
	if (XQueryTree(display, container, &root, &parentOf, &kids, &count) == 0) {
		return 0;
	}
	*first = count > 0 ? kids[0] : 0;
	if (kids != nullptr) {
		XFree(kids);
	}
	return count;
}

::Window makeContainer(Display* display) {
	const int screen = DefaultScreen(display);
	const ::Window w = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, 720, 500, 0,
	                                       BlackPixel(display, screen), BlackPixel(display, screen));
	XSync(display, False);
	return w;
}

} // namespace

TEST_CASE("backlog GQ: a reparent the X server refuses is HEARD — a BadWindow on ReparentWindow naming the dead parent is "
          "counted, and set_parent says false") {
	// `reparentTo` sent its requests and closed the connection after an XFlush, which never waits: the server's error
	// came back to nobody, and an embed into a window that does not exist reported success. It is synced under a
	// scoped handler now. PROVOKED, the two-way method from GP: a container made and destroyed, so its id is dead.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	// THE FIRST EMBED, in a live container: the one road that builds the window rather than moving it.
	const ::Window live = makeContainer(display);
	clap_window window {};
	window.api = CLAP_WINDOW_API_X11;
	window.x11 = live;
	REQUIRE(gui.setParent(&window));
	::Window editorWindow = 0;
	REQUIRE(windowsInside(display, live, &editorWindow) == 1);
	gui.destroy();
	CHECK(gui.lastXReport().errors == 0); // a detach from a live container is answered with nothing

	// A REOPEN INTO A CONTAINER THAT NO LONGER EXISTS.
	const ::Window dead = makeContainer(display);
	XDestroyWindow(display, dead);
	XSync(display, False);
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	window.x11 = dead;
	CHECK_FALSE(gui.setParent(&window));
	const ui::PluginWindow::XReport& report = gui.lastXReport();
	CHECK(report.errors >= 1);
	CHECK(report.code == BadWindow);
	CHECK(report.request == X_ReparentWindow);
	CHECK(report.resource == dead);
	// OUR window is fine — it was the destination that was gone — so this is a failed embed, not a rebuild.
	CHECK_FALSE(report.selfGone);
	CHECK(gui.hasWindow());
	CHECK(gui.windowRebuilds() == 0);

	// AND THE NEXT REAL CONTAINER TAKES THE SAME WINDOW, answered with nothing.
	const ::Window again = makeContainer(display);
	window.x11 = again;
	REQUIRE(gui.setParent(&window));
	CHECK(gui.lastXReport().errors == 0);
	::Window inside = 0;
	CHECK(windowsInside(display, again, &inside) == 1);
	CHECK(inside == editorWindow);
	gui.destroy();
	XDestroyWindow(display, live);
	XDestroyWindow(display, again);
	XCloseDisplay(display);
}

TEST_CASE("backlog GP: the rack, its container destroyed BEFORE guiDestroy, comes back in the next container every round, "
          "and hasWindow stops answering true about a dead window") {
	// X destroys the editor's window with the container. It used to stay dead for that instance: rounds 2 and 3 empty,
	// `hasWindow` true, set_parent true. The server now says so at the detach (a BadWindow naming OUR window), and the
	// next set_parent builds a fresh window and moves the editor into it.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
	rack::RackEditor* editor = nullptr;
	::Window previous = 0;
	for (int round = 1; round <= 3; ++round) {
		CAPTURE(round);
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		if (round == 1) {
			editor = gui.editorForTest();
			REQUIRE(editor != nullptr);
		}
		const ::Window container = makeContainer(display);
		clap_window window {};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = container;
		REQUIRE(gui.setParent(&window));
		CHECK(gui.hasWindow());
		::Window editorWindow = 0;
		CHECK(windowsInside(display, container, &editorWindow) == 1);
		if (round > 1) {
			CHECK(editorWindow != previous);
		}
		previous = editorWindow;
		CHECK(gui.windowRebuilds() == round - 1);
		// THE SAME EDITOR, moved rather than rebuilt: everything in it survives the window it lived in.
		CHECK(gui.editorForTest() == editor);
		CHECK(editor->parent() != nullptr);

		XDestroyWindow(display, container);
		XSync(display, False);
		gui.destroy();
		// THE DETACH WAS REFUSED BECAUSE OUR WINDOW IS GONE, and the window says so instead of answering true.
		CHECK(gui.lastXReport().selfGone);
		CHECK(gui.lastXReport().resource == previous);
		CHECK_FALSE(gui.hasWindow());
	}
	XCloseDisplay(display);
}

namespace {

bool effectivelyVisible(const visage::Frame* frame) {
	for (; frame != nullptr; frame = frame->parent()) {
		if (!frame->isVisible()) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("backlog GP: the abandoned windows are exactly one per container-first close — none per frame, none per ordinary "
          "open") {
	// The leak is accepted, BOUNDED. A dead window is abandoned rather than destroyed (its swapchain
	// may not be torn down after its window is gone, visage 018cd86), so each container-first close costs one window
	// for the life of the process. This pins the "one": not one per beat drawn, not one per ordinary open/close, and
	// not one per close that was never followed by an open.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
	const size_t before = ui::PluginWindow::abandonedWindowsForTest();
	const auto open = [&] {
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		const ::Window container = makeContainer(display);
		clap_window window {};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = container;
		REQUIRE(gui.setParent(&window));
		for (int beat = 0; beat < 20; ++beat) {
			gui.tick();
			gui.processEvents();
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
		return container;
	};
	const auto closeContainerFirst = [&](::Window container) {
		XDestroyWindow(display, container);
		XSync(display, False);
		gui.destroy();
	};
	const auto closeOrdinary = [&](::Window container) {
		gui.destroy();
		XDestroyWindow(display, container);
		XSync(display, False);
	};

	// THREE ORDINARY ROUNDS, drawn: nothing abandoned.
	for (int round = 0; round < 3; ++round) {
		closeOrdinary(open());
	}
	CHECK(ui::PluginWindow::abandonedWindowsForTest() - before == 0);
	// A CONTAINER-FIRST CLOSE abandons nothing yet; the reopen that rebuilds abandons ONE, and drawing it adds none.
	closeContainerFirst(open());
	CHECK(ui::PluginWindow::abandonedWindowsForTest() - before == 0);
	::Window container = open();
	CHECK(ui::PluginWindow::abandonedWindowsForTest() - before == 1);
	// ORDINARY ROUNDS AFTER IT: still one.
	closeOrdinary(container);
	for (int round = 0; round < 2; ++round) {
		closeOrdinary(open());
	}
	CHECK(ui::PluginWindow::abandonedWindowsForTest() - before == 1);
	// A SECOND EVENT: two.
	closeContainerFirst(open());
	container = open();
	CHECK(ui::PluginWindow::abandonedWindowsForTest() - before == 2);
	CHECK(gui.windowRebuilds() == 2);
	closeOrdinary(container);
	XCloseDisplay(display);
}

TEST_CASE("backlog GM/GP: SKLITTER's card in a real rack window has every visible control DRAWN and in the window, in each "
          "open order and after a GP rebuild") {
	// GM: SKLITTER's Mix and Density missing from a rack card until a resize. GP-bis reads it as a
	// frame that never drew: full-redraw replays a region's shapes and never makes them, so a control whose region is
	// EMPTY after the editor has drawn is that fault. The device arrives in each order a host and a hand can give it:
	// in the patch before the editor exists, between create and set_parent, after the embed. And a fourth: after GP's
	// rebuild, where every frame has MOVED to a new window, and must be inside that window rather than the dead one.
	//
	// THE INSTRUMENT CAN SEE THE FAULT (measured while writing this): ticking without ever pumping the window builds
	// the card and draws nothing, and every one of its controls (six then; seven since KX's Freeze) reads UNDRAWN. NOT REPRODUCED in any order below,
	// at a UI scale of 1 or 1.75, so GM stays open: this pins the orders that are fine, it does not close GM.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Display* display = XOpenDisplay(nullptr);
	REQUIRE(display != nullptr);
	const int screen = DefaultScreen(display);
	const auto embed = [&](rack::RackGui& gui) {
		uint32_t w = 0, h = 0;
		REQUIRE(gui.getSize(&w, &h));
		const ::Window container = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, w, h, 0,
		                                               BlackPixel(display, screen), BlackPixel(display, screen));
		XMapWindow(display, container);
		XSync(display, False);
		clap_window window {};
		window.api = CLAP_WINDOW_API_X11;
		window.x11 = container;
		REQUIRE(gui.setParent(&window));
		return container;
	};
	const auto pump = [](rack::RackGui& gui) {
		for (int beat = 0; beat < 40; ++beat) {
			gui.tick();
			gui.processEvents();
			std::this_thread::sleep_for(std::chrono::milliseconds(16));
		}
	};
	for (int order = 0; order < 4; ++order) {
		CAPTURE(order);
		rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
		rack::RackBridge bridge { engine };
		rack::RackGui gui { bridge };
		DeviceInstanceId id = kNoInstance;
		const auto add = [&] {
			id = bridge.addDevice(fx::sklitter::kTraits.tag);
			REQUIRE(id != kNoInstance);
			bridge.drain();
		};
		if (order == 0 || order == 3) {
			add();
		}
		REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
		if (order == 1) {
			add();
		}
		::Window container = embed(gui);
		if (order == 2) {
			add();
		}
		pump(gui);
		if (order == 3) {
			// GP: the container dies first, then guiDestroy; the reopen rebuilds.
			XDestroyWindow(display, container);
			XSync(display, False);
			gui.destroy();
			REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
			container = embed(gui);
			REQUIRE(gui.windowRebuilds() == 1);
			pump(gui);
		}
		rack::RackEditor* editor = gui.editorForTest();
		REQUIRE(editor != nullptr);
		auto* card = dynamic_cast<ui::GenericCard*>(editor->cardOfDeviceForTest(id));
		REQUIRE(card != nullptr);
		// IN THE WINDOW THERE IS: the live window's frame is among the card's ancestors (visage puts its own top-level
		// frame above it, so it is not the root).
		bool inLiveWindow = false;
		for (const visage::Frame* up = card; up != nullptr; up = up->parent()) {
			inLiveWindow = inLiveWindow || up == gui.appForTest();
		}
		CHECK(inLiveWindow);
		CHECK(card->hiddenCount() == 0);
		size_t visible = 0;
		size_t undrawn = 0;
		const auto look = [&](visage::Frame* f) {
			if (!effectivelyVisible(f) || f->width() <= 0.0f || f->height() <= 0.0f) {
				return;
			}
			++visible;
			if (f->region()->isEmpty()) {
				++undrawn;
			}
		};
		for (const auto& knob : card->knobsForTest()) {
			look(knob.get());
		}
		for (const auto& box : card->boxesForTest()) {
			look(box.get());
		}
		for (const auto& chip : card->chipsForTest()) {
			look(chip.get());
		}
		CHECK(visible == 8); // six until backlog KX put Freeze on the card's face, seven until MJ put Record Gate there
		CHECK(undrawn == 0);
		gui.destroy();
		XDestroyWindow(display, container);
		XSync(display, False);
	}
	XCloseDisplay(display);
}
#endif

TEST_CASE("backlog DB: the rack GUI's beat is what scrolls a card held at the lane's edge — the tick reaches the editor's drag, so a still pointer at the right edge moves the chain one step per beat") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui.create(api, false));
	rack::RackEditor* editor = gui.editorForTest();
	REQUIRE(editor != nullptr);
	std::vector<DeviceInstanceId> ids;
	for (size_t i = 0; i < rack::kRackAxes.deviceSlots; ++i) {
		ids.push_back(bridge.addDevice(fx::filter::kTraits.tag));
		REQUIRE(ids.back() != kNoInstance);
	}
	bridge.drain();
	gui.tick(); // the beat builds the cards
	editor->setBounds(0, 0, rack::RackEditor::kWidth, rack::RackEditor::kHeight);
	editor->resized();
	REQUIRE(editor->maxScroll() > 0.0f);
	const DeviceInstanceId held = ids[0];
	const visage::Bounds card = editor->cardBoundsForTest(held);
	REQUIRE(card.width() > 0.0f);
	editor->cardDragForTest(held, ui::FxCard::DragPhase::Begin, { 10.0f, 10.0f });
	editor->cardDragForTest(held, ui::FxCard::DragPhase::Move,
	                        { rack::RackEditor::kWidth - 4.0f - card.x(), 10.0f }); // held at the right edge
	REQUIRE(editor->scroll() == doctest::Approx(0.0f));
	gui.tick();
	CHECK(editor->scroll() == doctest::Approx(rack::RackEditor::kEdgeScrollStep));
	gui.tick();
	CHECK(editor->scroll() == doctest::Approx(2.0f * rack::RackEditor::kEdgeScrollStep));
	editor->cardDragForTest(held, ui::FxCard::DragPhase::End, { 10.0f, 10.0f });
	gui.destroy();
}

TEST_CASE("adjust_size never answers with the size we already are") {
	// The clap-helpers default did exactly that on our behalf, which told every host that dragged an
	// edge to put the window back where it started. Feathers found it; the rack inherits the fix
	// through the shared shell, and this is the case that says so.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	// THE GUI HAS TO EXIST FIRST, and clap-helpers is right to refuse otherwise — a host asking a
	// plugin to adjust a size before it has an editor is asking about nothing. A first draft of this
	// case skipped the create and read that correct refusal as a failure.
	REQUIRE(gui->create(v.plugin, api, false));
	uint32_t w = 4000, h = 3000;
	REQUIRE(gui->adjust_size(v.plugin, &w, &h));
	REQUIRE(w >= rack::RackEditor::kWidth);
	REQUIRE(h >= rack::RackEditor::kHeight);

	// IDEMPOTENT: adjusting an already-adjusted size returns it unchanged, which is what a host relies
	// on when it asks, applies, and asks again.
	const uint32_t w1 = w, h1 = h;
	REQUIRE(gui->adjust_size(v.plugin, &w, &h));
	REQUIRE(w == w1);
	REQUIRE(h == h1);

	gui->destroy(v.plugin);
}

//------------------------------------------------------------------------------------------------//
// THE EDITOR ASSEMBLED IN A REAL WINDOW — the gap the cases above stop short of.                   //
//------------------------------------------------------------------------------------------------//
//
// `create` CONSTRUCTS a window; it takes a SIZE to make one with real bounds, which is what
// `EditorFixture` in `test_gui_windowed.cpp` does and why that suite can talk about layout at all.
// These drive the same path for the rack: window -> `Client::layout()` -> editor -> cards.
//
// STILL NOT AN EMBED. `set_parent` wants a parent window from a host and nothing here is one. What
// these close is the wiring between the shared shell and THIS surface, which was the untested half —
// the shell itself is exercised by Feathers' windowed suite.

TEST_CASE("the editor is laid out with real bounds once the window has a size") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	REQUIRE(hostSetSize(gui, v.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));

	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	REQUIRE(ext != nullptr);
	rack::RackEditor* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));
	REQUIRE(editor != nullptr);

	// A SURFACE AT ZERO IS A SURFACE NOBODY CAN PRESS — and it is what a layout that never ran looks
	// like, which is exactly the failure the shell's `syncScaleAndLayout` exists to prevent.
	REQUIRE(editor->width() > 0);
	REQUIRE(editor->height() > 0);

	// AND IT IS IN THE WINDOW'S TREE, which the bounds cannot tell you.
	//
	// FOUND BY A KNOWN-BAD: deleting `app->addChild(editor_.get())` left every assertion above green.
	// An editor with real bounds that is not a child of anything is laid out, sized, and INVISIBLE —
	// it never draws and never receives a click, and nothing that measures it can see the difference.
	// A frame's geometry says what it would look like; its parent says whether anyone will look.
	REQUIRE(editor->parent() != nullptr);

	// AND THE MACRO KNOBS WITH IT. They are built in the editor's constructor and placed by `resized`;
	// a knob at zero means the layout ran on an editor with no bounds.
	REQUIRE(editor->macrosForTest().size() == 8);
	for (const auto& knob : editor->macrosForTest()) {
		REQUIRE(knob != nullptr);
		REQUIRE(knob->width() > 0);
	}
	gui->destroy(v.plugin);
}

TEST_CASE("a device added while the editor is open gets a card with real bounds") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	REQUIRE(hostSetSize(gui, v.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));

	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	rack::RackEditor* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));
	REQUIRE(editor != nullptr);

	REQUIRE(ext->add_device(v.plugin, fx::filter::kTraits.tag) != 0); // an instance id, never 0
	// THE CHAIN CHANGED FROM OUTSIDE THE EDITOR, which is the case `RackGui::tick` reconciles — a
	// device added by a patch load or by a host is not one this surface asked for.
	ext->sync_cards(v.plugin);

	REQUIRE(editor->cardsForTest().at(0) != nullptr);
	REQUIRE(editor->cardsForTest().at(0)->width() > 0);
	REQUIRE(editor->cardsForTest().at(0)->height() > 0);

	// AND IT SITS BESIDE THE MACROS COLUMN rather than on top of the macros — the one thing the layout has to get right that a
	// "did it get bounds" check cannot see.
	REQUIRE(editor->cardsForTest().at(0)->x() >= rack::RackEditor::kLaneLeft);
	REQUIRE(editor->cardsForTest().at(0)->x() >= editor->macrosForTest()[1]->x() + editor->macrosForTest()[1]->width());
	gui->destroy(v.plugin);
}

//------------------------------------------------------------------------------------------------//
// A REFUSAL IS SAID.                                                                               //
//------------------------------------------------------------------------------------------------//
//
// `loadRack` counts the routes it dropped and names the first. That was captured and shown to nobody
// for as long as the rack had no surface to say it on — the honesty rule's failure case in its
// quietest form: the build knew, and did not mention it.

namespace {

/// A patch with one route pointing at a slot that will hold nothing — the refusal path, built by
/// hand because no rack can produce a route it would itself refuse.
std::vector<char> patchWithADoomedRoute(bool withRoute = true) {
	// **A DOCUMENT, WRITTEN BY HAND** (backlog JF — a patch is JSON now, which a hand can write): NO devices
	// at all, every macro 0 and unipolar, the canvas at its default, and one route naming a device BY ID
	// that no record carries, and a control of it by key. Without the route it is the same patch, clean.
	const std::string routes =
	    withRoute ? R"([{"src": {"macro": 0}, "dst": {"device": 42, "control": "filter_cutoff_wide"}, "amount": 0.5}])" : "[]";
	const std::string doc = R"({"format": "feathers.rack", "major": 1, "chains": [], "routes": )" + routes
	                        + R"(, "macros": [0, 0, 0, 0, 0, 0, 0, 0], "bipolar": [0, 0, 0, 0, 0, 0, 0, 0],)"
	                        + R"( "canvas": {"extent": 0, "collapsed": false}})";
	return { doc.begin(), doc.end() };
}

struct ReadStream {
	std::vector<char> bytes;
	size_t at = 0;
	clap_istream in {};
	explicit ReadStream(std::vector<char> b) : bytes(std::move(b)) {
		in.ctx = this;
		in.read = [](const clap_istream* s, void* buf, uint64_t n) -> int64_t {
			auto* m = static_cast<ReadStream*>(s->ctx);
			const size_t left = m->bytes.size() - m->at;
			const size_t take = n < left ? static_cast<size_t>(n) : left;
			std::memcpy(buf, m->bytes.data() + m->at, take);
			m->at += take;
			return static_cast<int64_t>(take);
		};
	}
};

} // namespace

TEST_CASE("a patch loaded while blocks run is the rack when the load returns, and the engine becomes it at the next drain") {
	// **THE ACTIVE ROAD, WITH A HOST THAT IS ACTUALLY PROCESSING** — which the rest of this file's venue
	// is not, because it never calls `process` on its own. A thread runs blocks; the main thread loads.
	//
	// **THIS WAS "...LANDS AT A DRAIN, AND THE LOAD WAITS FOR IT"** (CD commit 3): the load handed a patch
	// to the drain and waited, and what to return when the wait expired was an open design question
	// (backlog CD). Desired state retired the wait and the question
	// with it: the load replaces the record where it is called and returns. The time is still printed —
	// it is now the cost of building the patch's devices, not of waiting for a block.
	Venue v(256);
	const auto* state =
	    static_cast<const clap_plugin_state*>(v.plugin->get_extension(v.plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);

	// A MACRO MOVED OFF ITS DEFAULT FIRST, so the assertion after the load is about the patch rather
	// than about a rack that was already zero.
	{
		OneEvent e(0, 0.75);
		v.run(&e.in);
		double before = -1.0;
		const auto* p0 =
		    static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
		REQUIRE(p0 != nullptr);
		REQUIRE(p0->get_value(v.plugin, 0, &before));
		REQUIRE(before == doctest::Approx(0.75));
	}

	// **AND ONLY NOW DOES THE AUDIO THREAD START.** The block above calls `run()` on THIS thread;
	// starting the loop first would put two threads inside `process()` at once, which is not a
	// measurement of anything and is a data race in its own right.
	std::atomic<bool> stop { false };
	std::atomic<int> blocks { 0 };
	std::thread audio([&] {
		while (!stop.load(std::memory_order_relaxed)) {
			v.run();
			blocks.fetch_add(1, std::memory_order_release);
		}
	});
	// THE WINDOW MUST BE OPEN BEFORE THE LOAD, or the measurement is of a stopped host.
	while (blocks.load(std::memory_order_acquire) == 0) {
		std::this_thread::yield();
	}

	ReadStream s(patchWithADoomedRoute());
	const auto began = std::chrono::steady_clock::now();
	const bool ok = state->load(v.plugin, &s.in);
	const auto waited =
	    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began);

	stop.store(true, std::memory_order_relaxed);
	audio.join();

	MESSAGE("stateLoad on an ACTIVE, PROCESSING host took "
	        << waited.count() << " us at 256 frames / 48 kHz (one block is ~5333 us) — no wait for a drain");
	CHECK(ok);
	// THE NEXT BLOCK'S DRAIN — the one the engine becomes the patch at. Run here, after the audio thread
	// has stopped, so the value below does not depend on which side of the load its last block fell.
	v.run();
	// **AND THE PATCH'S CONTENT ARRIVED — a VALUE, not "something happened".** This patch holds no
	// devices, so the macros are what it has to say: all eight at 0. Macro 0 was driven to 0.75 before
	// the load precisely so that "the load reset it" is distinguishable from "it was never set" —
	// CLAUDE.md's rule, and the reason the parameter event above is not decoration.
	const auto* params =
	    static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	double macro0 = -1.0;
	REQUIRE(params->get_value(v.plugin, 0, &macro0));
	CHECK(macro0 == doctest::Approx(0.0));
}

TEST_CASE("a routing the rules drop is SAID, not merely counted") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	REQUIRE(hostSetSize(gui, v.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));

	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	auto* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));
	REQUIRE(editor != nullptr);

	const auto* state =
	    static_cast<const clap_plugin_state*>(v.plugin->get_extension(v.plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	ReadStream s(patchWithADoomedRoute());
	REQUIRE(state->load(v.plugin, &s.in));
	v.settle(); // the load says its refusals itself; the settle lets the editor paint them

	// The tray holds the text the moment it is said; the fade is a separate concern.
	const std::string said = editor->notifierTextForTest();
	CAPTURE(said);
	REQUIRE_FALSE(said.empty());
	REQUIRE(said.find("Dropped") != std::string::npos);
	// AND IT NAMES THE FIRST ONE, in terms a player can act on rather than as a bare count — here, that the
	// device it named (by id, v5) is not in this rack, rather than a neighbour's slot it would have moved to.
	//
	// **THE WORDING MOVED WITH THE CONTRACT (kill-lanes step 3b) and the claim did not.** It read "no
	// longer here", which a coordinate one past the axis could only be read as; a refusal now carries the
	// endpoint the PATCH ASKED FOR, so the honest sentence is that this rack does not hold that device —
	// it may never have held it, which "no longer" quietly asserted. The thing under test is unchanged:
	// the notice must say WHICH thing was dropped and that it is not here, not merely that something was.
	REQUIRE(said.find("a device this rack does not hold") != std::string::npos);
	gui->destroy(v.plugin);
}

TEST_CASE("a load that refused nothing says nothing") {
	// Otherwise the tray would announce every patch load, and a message that always appears is a
	// message nobody reads — which would make the case above pass for the wrong reason forever.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	auto* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));

	const auto* state =
	    static_cast<const clap_plugin_state*>(v.plugin->get_extension(v.plugin, CLAP_EXT_STATE));
	ReadStream s(patchWithADoomedRoute(false)); // the same patch, without its route
	REQUIRE(state->load(v.plugin, &s.in));
	v.settle();
	REQUIRE(editor->notifierTextForTest().empty());
	gui->destroy(v.plugin);
}

TEST_CASE("a refusal that arrives BEFORE the editor is held, and said when it opens") {
	// THE USUAL ORDER, not an edge case: a host loads a patch long before anyone opens the editor. A
	// refusal that could only be said to an open editor would be a refusal nobody ever hears.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* state =
	    static_cast<const clap_plugin_state*>(v.plugin->get_extension(v.plugin, CLAP_EXT_STATE));
	ReadStream s(patchWithADoomedRoute());
	REQUIRE(state->load(v.plugin, &s.in)); // no editor exists yet
	v.settle();

	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	auto* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));
	REQUIRE(editor != nullptr);
	REQUIRE(editor->notifierTextForTest().find("Dropped") != std::string::npos);
	gui->destroy(v.plugin);
}

TEST_CASE("the (+) adds a device to the rack, and the card appears") {
	// END TO END THROUGH THE SURFACE: the gesture a player makes, not the extension a venue uses.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	REQUIRE(hostSetSize(gui, v.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));

	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	auto* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));
	REQUIRE(editor != nullptr);
	REQUIRE(editor->cardCountForTest() == 0); // nothing yet

	rack::AddDeviceRow* add = editor->addRowForTest();
	REQUIRE(add != nullptr);
	// THE PRESS OPENS FEATHERS' ADD MENU (backlog KP); the pick is the filter, by its name in the menu.
	add->onMouseDown().callback(visage::MouseEvent {});
	visage::PopupMenuFrame* popup = menu_check::newestPopup(*add);
	REQUIRE(popup != nullptr);
	REQUIRE(menu_check::pickRow(*popup, fx::filter::kTraits.name));

	// THE DEVICE IS IN THE ENGINE and the CARD IS ON THE SURFACE — two facts, because either alone
	// would be a rack that half-added something.
	REQUIRE(editor->cardsForTest().at(0) != nullptr);
	gui->destroy(v.plugin);
}

//------------------------------------------------------------------------------------------------//
// TWO BUGS FOUND BY HAND, NOT BY A SUITE — the regressions for them.                               //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the (+) is a spine in the rack's lane, the lane's height tall, and a press does not resize it") {
	// "add device shows nothing but close" was the row keeping the size it was given while its inline list
	// grew. The list is gone (backlog KP); what remains to pin is the spine's own size.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	REQUIRE(hostSetSize(gui, v.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));
	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	auto* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));
	rack::AddDeviceRow* add = editor->addRowForTest();
	REQUIRE(add != nullptr);

	// MEASURED ALONG THE FLOW AXIS, WHICH IS HORIZONTAL: the rack's chain runs left-to-right.
	const float shut = add->width();
	REQUIRE(shut == doctest::Approx(rack::AddDeviceRow::kShutWidth));
	// AND THE CROSS AXIS IS THE LANE'S, not the bar's: a row sized to `kBarHeight` in a horizontal
	// lane would be a 26px sliver beside full-height cards.
	REQUIRE(add->height() > rack::AddDeviceRow::kBarHeight);
	// THE (+) NO LONGER OPENS A LIST INSIDE ITSELF (backlog KP) — a press opens the add menu — so it
	// keeps this size whatever a hand does with it.
	add->onMouseDown().callback(visage::MouseEvent {});
	REQUIRE(add->width() == doctest::Approx(shut));
	gui->destroy(v.plugin);
}

TEST_CASE("a macro moved by the host moves on the face") {
	// "moving macros in daw doesnt seem to move the macros in the plugin": a knob reads the engine
	// when it DRAWS, and nothing was asking it to draw. Asserted as a count produced BY the
	// redrawing, so a road that quietly stops cannot look like one with nothing to do.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	auto* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));

	editor->syncMacros();               // settle the first-draw diff
	const size_t before = editor->macroRedrawsForTest();
	editor->syncMacros();
	REQUIRE(editor->macroRedrawsForTest() == before); // a still panel repaints nothing

	OneEvent e(3, 0.62);
	v.run(&e.in);                       // the host moves macro 4
	editor->syncMacros();
	REQUIRE(editor->macroRedrawsForTest() == before + 1); // exactly the one that moved
	gui->destroy(v.plugin);
}

// ---------------------------------------------------------------------------------------------
// THE SHAPE OF THE GESTURE THAT CRASHED: editor open, audio running, press "add device".
//
// EVERY OTHER CASE IN THIS FILE IS SINGLE-THREADED, which is why the whole suite ran clean under
// ThreadSanitizer while a race was shipping. **A sanitiser only sees races that HAPPEN**, and none of
// them can happen in a venue where `process` and the editor take turns on one thread. The engine's
// own suite grew a threaded case for the same reason; this is that case one layer up, where the real
// plugin, the real editor and the real CLAP entry points are.
TEST_CASE("adding a device from the editor while the host is processing") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	Venue v;
	const auto* gui =
	    static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
	REQUIRE(gui != nullptr);
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui->create(v.plugin, api, false));
	REQUIRE(hostSetSize(gui, v.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));
	const auto* ext = static_cast<const clap_plugin_feathers_rack_test*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_RACK_TEST));
	REQUIRE(ext != nullptr);
	auto* editor = static_cast<rack::RackEditor*>(ext->editor(v.plugin));
	REQUIRE(editor != nullptr);

	std::atomic<bool> stop { false };
	std::atomic<size_t> blocks { 0 };
	std::thread audio([&] {
		while (!stop.load(std::memory_order_relaxed)) {
			v.run();
			blocks.fetch_add(1, std::memory_order_relaxed);
		}
	});

	// THE WINDOW HAS TO BE OPEN BEFORE THE PRESS, and the count is asserted rather than assumed — the
	// engine's version of this case first reported zero blocks and would otherwise have been a green
	// run of a window that never existed.
	while (blocks.load(std::memory_order_relaxed) == 0) {
		std::this_thread::yield();
	}

	// THE EDITOR'S OWN ROAD, not the engine's: this is what a press on the `(+)` reaches, so the case
	// covers `syncCards` and the card build as well as `addDevice`.
	editor->addDeviceByTag(fx::filter::kTraits.tag);
	editor->addDeviceByTag(mod::dahdsr::kTraits.tag);

	const size_t seen = blocks.load(std::memory_order_relaxed);
	while (blocks.load(std::memory_order_relaxed) < seen + 32) {
		std::this_thread::yield();
	}

	stop.store(true, std::memory_order_relaxed);
	audio.join();
	CHECK(blocks.load() > 0);
	gui->destroy(v.plugin);
}

namespace {

/// THE FIRST FRAME OF TYPE `T` UNDER `frame`, depth first — a case finds the arm the editor built rather than one of
/// its own.
template <typename T>
T* firstUnder(visage::Frame* frame) {
	if (auto* hit = dynamic_cast<T*>(frame); hit != nullptr) {
		return hit;
	}
	for (visage::Frame* child : frame->children()) {
		if (T* found = firstUnder<T>(child); found != nullptr) {
			return found;
		}
	}
	return nullptr;
}

/// WHERE `WindowEventHandler::handleMouseDown` PUTS THE KEYBOARD FOCUS after a press on `pressed`: the first frame
/// up from it that accepts keystrokes, or nothing.
visage::Frame* focusAfterPressOn(visage::Frame* pressed) {
	visage::Frame* frame = pressed;
	while (frame != nullptr && !frame->acceptsKeystrokes()) {
		frame = frame->parent();
	}
	return frame;
}

/// THE WALK `WindowEventHandler::handleKeyDown` MAKES: from the focus up through every ancestor that accepts
/// keystrokes, until one takes the key. True when one did.
bool keyFrom(visage::Frame* focused, const visage::KeyEvent& e) {
	visage::Frame* frame = focused;
	bool used = false;
	while (!used && frame != nullptr) {
		used = frame->processKeyPress(e);
		frame = frame->parent();
		while (frame != nullptr && !frame->acceptsKeystrokes()) {
			frame = frame->parent();
		}
	}
	return used;
}

} // namespace

TEST_CASE("backlog NI: Esc leaves modulation assign mode in the rack's editor, by the road a keystroke travels") {
	// Esc did not leave mod mode in the rack. Feathers' editor root takes Esc (`leaveAssignMode`);
	// the rack's window accepted no keystrokes at all, so after the press that ARMS a macro the focus was
	// `RackEditor` (Delete and Backspace only), the walk up found nobody else, and Esc was delivered nowhere.
	// Driven as visage drives it: the focus a press on the macro's own arm leaves, then the key walked up from it.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
#if defined(__APPLE__)
	REQUIRE(gui.create(CLAP_WINDOW_API_COCOA, false));
#else
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
#endif
	rack::RackEditor* editor = gui.editorForTest();
	REQUIRE(editor != nullptr);
	ui::ModState& mod = gui.modForTest();
	ui::SourceArm* arm = firstUnder<ui::SourceArm>(editor);
	REQUIRE(arm != nullptr);
	// THE ARM IS IN THE WINDOW — the walk from it passes the window whose handler is under test, or this proves
	// nothing. (visage wraps the window in its own top-level frame, so the window is an ancestor, not the root.)
	bool inWindow = false;
	for (const visage::Frame* up = arm; up != nullptr; up = up->parent()) {
		inWindow = inWindow || up == gui.appForTest();
	}
	REQUIRE(inWindow);

	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
	visage::MouseEvent press {};

	SUBCASE("a macro armed by a press on its arm, Esc, disarmed — and the key is taken") {
		arm->processMouseDown(press);
		REQUIRE(mod.armed());
		const backplane::Source armed = mod.source();
		visage::Frame* focused = focusAfterPressOn(arm);
		REQUIRE(focused != nullptr);
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(mod.armed());
		// LEAVING THE MODE IS NOT FORGETTING THE SOURCE — Feathers' rule.
		CHECK(mod.source() == armed);
	}

	SUBCASE("an arm inside a nested chain is the hand's one arm too, and Esc takes it off") {
		ui::ModState inner;
		inner.setParent(&mod);
		inner.arm(true);
		REQUIRE(inner.armedLocally());
		REQUIRE_FALSE(mod.armedLocally());
		CHECK(keyFrom(focusAfterPressOn(arm), escape));
		CHECK_FALSE(inner.armedLocally());
		inner.setParent(nullptr);
	}

	SUBCASE("Esc with nothing armed is not the rack's key") {
		REQUIRE_FALSE(mod.armed());
		CHECK_FALSE(keyFrom(focusAfterPressOn(arm), escape));
		CHECK_FALSE(mod.armed());
	}
	gui.destroy();
}

TEST_CASE("backlog NJ: the rack's LFO, FREE in Hz, follows the song's seconds through the rack's CLAP road") {
	// the design: phase = Rate x song position in SECONDS + Phase while the transport plays. Driven by
	// a host's own blocks (sizes that do not divide the 64-frame tick), a tempo change halfway, and a jump; read off
	// the engine's published output after each block, against the wave at the phase the design names at the block's
	// last tick. The wave is evaluated by the device's own `eval` — the claim is the PHASE, which is restated here.
	constexpr double kSr = 48000.0;
	constexpr int kTick = 64;
	Venue v(1024);
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t id = v.test()->add_device(v.plugin, "LFO");
	REQUIRE(id != 0);
	REQUIRE(chain->set_control(v.plugin, 0, "Waveform", 0.0)); // Sine
	REQUIRE(chain->set_control(v.plugin, 0, "Mode", 3.0));     // Free
	REQUIRE(chain->set_control(v.plugin, 0, "Time Base", 0.0)); // Hz
	REQUIRE(chain->set_control(v.plugin, 0, "Rate", 1.0));      // log2 Hz: 2 Hz
	REQUIRE(chain->set_control(v.plugin, 0, "Phase", 0.25));
	v.run(); // the pre-roll: a whole block, no transport
	long long host = 1024; // frames the venue has processed — the tick grid counts from activation
	const auto* bridge = static_cast<rack::RackBridge*>(v.test()->bridge(v.plugin));
	mod::lfo::Settings s {};
	s.wave = mod::lfo::Waveform::Sine;
	s.mode = mod::Mode::Free;
	const auto expectedAt = [](double seconds) {
		const double turns = 2.0 * seconds + 0.25;
		return turns - std::floor(turns);
	};
	double seconds = 11.0;
	double beats = 22.0;
	const int sizes[] = { 441, 32, 1000, 512, 7, 300, 1024, 64, 129 };
	size_t checked = 0;
	for (int b = 0; b < 40; ++b) {
		CAPTURE(b);
		if (b == 30) {
			seconds = 93.5; // a playhead jump
		}
		const int n = sizes[b % 9];
		const double tempo = b < 15 ? 120.0 : 157.0; // a tempo change: the beats lurch, the seconds do not
		clap_event_transport tp {};
		tp.header.size = sizeof(tp);
		tp.header.type = CLAP_EVENT_TRANSPORT;
		tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
		           | CLAP_TRANSPORT_IS_PLAYING;
		tp.tempo = tempo;
		tp.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
		tp.song_pos_seconds = static_cast<clap_sectime>(std::llround(seconds * CLAP_SECTIME_FACTOR));
		clap_input_events none = emptyEvents();
		clap_output_events sink = sinkEvents();
		clap_process p {};
		p.frames_count = static_cast<uint32_t>(n);
		p.transport = &tp;
		p.audio_inputs = &v.in;
		p.audio_inputs_count = 1;
		p.audio_outputs = &v.out;
		p.audio_outputs_count = 1;
		p.in_events = &none;
		p.out_events = &sink;
		v.plugin->process(v.plugin, &p);
		const long long lastTick = ((host + n - 1) / kTick) * kTick;
		if (lastTick >= host) {
			const double at = static_cast<double>(seconds) + static_cast<double>(lastTick - host) / kSr;
			const float* cell = bridge->engineForTest().outputCellOf(id, 0);
			REQUIRE(cell != nullptr);
			CHECK(*cell == doctest::Approx(mod::lfo::eval(s, static_cast<float>(expectedAt(at)))).epsilon(1e-3));
			++checked;
		}
		seconds += n / kSr;
		beats += n / kSr * tempo / 60.0;
		host += n;
	}
	CHECK(checked > 30);
}

TEST_CASE("backlog NJ (a): the rack's device host answers the host's tempo, as Feathers' does — the LFO card asks it") {
	// `DeviceHost::secondsPerBeat` is what the LFO card draws its smoothing against (`lfo::rateHz`). Feathers' bridge
	// answered the host's tempo; the rack's answered the default zero whatever the host said. Asked through the
	// interface the card holds, after a block of the rack's own CLAP road.
	Venue v(512);
	const backplane::DeviceHost& host = *static_cast<rack::RackBridge*>(v.test()->bridge(v.plugin));
	v.run(); // no transport
	CHECK(host.secondsPerBeat() == 0.0);
	clap_event_transport tp {};
	tp.header.size = sizeof(tp);
	tp.header.type = CLAP_EVENT_TRANSPORT;
	tp.flags = CLAP_TRANSPORT_HAS_TEMPO;
	tp.tempo = 150.0;
	clap_input_events none = emptyEvents();
	clap_output_events sink = sinkEvents();
	clap_process p {};
	p.frames_count = 512;
	p.transport = &tp;
	p.audio_inputs = &v.in;
	p.audio_inputs_count = 1;
	p.audio_outputs = &v.out;
	p.audio_outputs_count = 1;
	p.in_events = &none;
	p.out_events = &sink;
	v.plugin->process(v.plugin, &p);
	CHECK(host.secondsPerBeat() == doctest::Approx(0.4));
	// AND A HOST THAT STOPS SAYING IS NOT REMEMBERED AS STILL SAYING IT.
	v.run();
	CHECK(host.secondsPerBeat() == 0.0);
}

TEST_CASE("backlog NJ (b): a transport the host sends INSIDE a block re-anchors the rack's clock from its sample on") {
	// Feathers re-anchors its song clock at each mid-block `CLAP_EVENT_TRANSPORT` (`applyTransport`); the rack dropped
	// them, so its clock ran on from the block's start across a locate. A locate at frame 256 of a 512-frame block: the
	// block's last tick (frame 448) must stand 192 frames past the LOCATE, read off the LFO the clock drives — FREE in
	// Hz at 2 Hz, so its phase is 2 x song seconds (NJ). The phase is the claim; the device's own `eval` turns it into
	// the published value.
	constexpr double kSr = 48000.0;
	Venue v(512);
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t id = v.test()->add_device(v.plugin, "LFO");
	REQUIRE(id != 0);
	REQUIRE(chain->set_control(v.plugin, 0, "Waveform", 0.0)); // Sine
	REQUIRE(chain->set_control(v.plugin, 0, "Mode", 3.0));     // Free
	REQUIRE(chain->set_control(v.plugin, 0, "Time Base", 0.0)); // Hz
	REQUIRE(chain->set_control(v.plugin, 0, "Rate", 1.0));      // 2 Hz
	v.run();
	const auto* bridge = static_cast<rack::RackBridge*>(v.test()->bridge(v.plugin));
	mod::lfo::Settings s {};
	s.wave = mod::lfo::Waveform::Sine;
	s.mode = mod::Mode::Free;
	const auto transportAt = [](double seconds, uint32_t time) {
		clap_event_transport tp {};
		tp.header.size = sizeof(tp);
		tp.header.type = CLAP_EVENT_TRANSPORT;
		tp.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		tp.header.time = time;
		tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
		tp.tempo = 120.0;
		tp.song_pos_seconds = static_cast<clap_sectime>(std::llround(seconds * CLAP_SECTIME_FACTOR));
		return tp;
	};
	const auto phaseValue = [&](double seconds) {
		const double turns = 2.0 * seconds;
		return mod::lfo::eval(s, static_cast<float>(turns - std::floor(turns)));
	};
	const auto block = [&](double seconds, const clap_event_transport* inside) {
		clap_event_transport tp = transportAt(seconds, 0);
		struct One {
			const clap_event_header* e;
		} one { inside != nullptr ? &inside->header : nullptr };
		clap_input_events in {};
		in.ctx = &one;
		in.size = [](const clap_input_events* list) -> uint32_t {
			return static_cast<const One*>(list->ctx)->e != nullptr ? 1u : 0u;
		};
		in.get = [](const clap_input_events* list, uint32_t) { return static_cast<const One*>(list->ctx)->e; };
		clap_output_events sink = sinkEvents();
		clap_process p {};
		p.frames_count = 512;
		p.transport = &tp;
		p.audio_inputs = &v.in;
		p.audio_inputs_count = 1;
		p.audio_outputs = &v.out;
		p.audio_outputs_count = 1;
		p.in_events = &in;
		p.out_events = &sink;
		v.plugin->process(v.plugin, &p);
		const float* cell = bridge->engineForTest().outputCellOf(id, 0);
		REQUIRE(cell != nullptr);
		return *cell;
	};
	(void)block(20.0, nullptr); // anchor the lock on a playing song
	// WITHOUT AN EVENT the last tick reads the block's own clock, 448 frames on — the control, so the case below
	// cannot pass on a value the two roads share.
	const double plain = 20.0 + 512.0 / kSr;
	CHECK(block(plain, nullptr) == doctest::Approx(phaseValue(plain + 448.0 / kSr)).epsilon(1e-4));
	const double from = plain + 512.0 / kSr;
	const double locate = 61.3;
	const clap_event_transport inside = transportAt(locate, 256);
	const float got = block(from, &inside);
	CHECK(got == doctest::Approx(phaseValue(locate + 192.0 / kSr)).epsilon(1e-4));
	REQUIRE(std::abs(phaseValue(locate + 192.0 / kSr) - phaseValue(from + 448.0 / kSr)) > 0.05);
	// AND IT IS THAT BLOCK'S: the next block, with no event, reads its own transport again.
	const double after = from + 512.0 / kSr;
	CHECK(block(after, nullptr) == doctest::Approx(phaseValue(after + 448.0 / kSr)).epsilon(1e-4));
}


TEST_CASE("backlog NN: in the rack's editor Esc closes the most recently opened thing — a window, an arm, a nested arm") {
	// Esc closes the most recently opened thing, in both plugins. NI gave the rack's window an Esc that disarmed
	// the tree; it now closes the top of the editor's one stack, on which the whole follow-tree's arm is one entry
	// beside the windows. Keys travel from the focus a press on the macro's arm leaves, as in NI's case.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
#if defined(__APPLE__)
	REQUIRE(gui.create(CLAP_WINDOW_API_COCOA, false));
#else
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
#endif
	rack::RackEditor* editor = gui.editorForTest();
	REQUIRE(editor != nullptr);
	ui::ModState& mod = gui.modForTest();
	ui::SourceArm* arm = firstUnder<ui::SourceArm>(editor);
	REQUIRE(arm != nullptr);
	ui::ExpandedOverlay& overlay = editor->overlayForTest();
	visage::Frame* const focused = focusAfterPressOn(arm);
	REQUIRE(focused != nullptr);
	const visage::KeyEvent escape { visage::KeyCode::Escape, 0, true };
	visage::MouseEvent press {};
	const auto openWindow = [&] {
		const ui::SurfaceId id = editor->surfaceLayersForTest().present(std::make_unique<visage::Frame>(),
		                                                                ui::SurfaceNeed::SpansGesture, "Window", {});
		REQUIRE(id != ui::kNoSurface);
		REQUIRE(overlay.open());
	};

	SUBCASE("a window, then the macro armed by its press: Esc disarms, the window stays; again, the window closes") {
		openWindow();
		arm->processMouseDown(press);
		REQUIRE(mod.armed());
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(mod.armed());
		CHECK(overlay.open());
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(overlay.open());
		CHECK_FALSE(keyFrom(focused, escape));
	}

	SUBCASE("the macro armed, then a window: Esc closes the window, the arm stays; again, it disarms") {
		arm->processMouseDown(press);
		REQUIRE(mod.armed());
		openWindow();
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(overlay.open());
		CHECK(mod.armed());
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(mod.armed());
		CHECK_FALSE(keyFrom(focused, escape));
	}

	SUBCASE("a window, then an arm in a nested chain: Esc takes the nested arm off and leaves the window") {
		ui::ModState inner;
		inner.setParent(&mod);
		openWindow();
		inner.arm(true);
		REQUIRE(inner.armedLocally());
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(inner.armedLocally());
		CHECK(overlay.open());
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(overlay.open());
		inner.setParent(nullptr);
	}

	SUBCASE("a nested arm, then a window: Esc closes the window and the nested arm stays") {
		ui::ModState inner;
		inner.setParent(&mod);
		inner.arm(true);
		openWindow();
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(overlay.open());
		CHECK(inner.armedLocally());
		CHECK(keyFrom(focused, escape));
		CHECK_FALSE(inner.armedLocally());
		inner.setParent(nullptr);
	}

	SUBCASE("nothing open: the key is the host's") {
		CHECK_FALSE(keyFrom(focused, escape));
	}
	overlay.dismiss();
	mod.disarmTree();
	gui.destroy();
}
TEST_CASE("backlog IN: SKLITTER at its defaults plays the loop on the song's grid through the rack's CLAP road, after a block with no transport") {
	// **THE HOST'S ROAD, BECAUSE THE DEFECT LIVED IN WHAT A HOST DOES FIRST.** A DAW processes before it
	// plays; the venue's settling block carries no transport at all. MEASURED before the fix: every loop
	// played LATE by exactly that block — 1024, 2048 or 4096 frames for a venue activated at that size —
	// at 480-, 512- and 1024-frame blocks and in both lock settings, because the ring had started filling
	// with no song to anchor to and nothing looked again when the song arrived.
	//
	// Every input frame names itself (`frameCode` in the device suite, restated: offset from zero, sign
	// flipping each frame), so an output frame says which song frame it was recorded at.
	constexpr double kSr = 48000.0;
	constexpr double kSpb = 0.5;
	constexpr long long kBar = 96000;
	for (const int block : { 512, 1024, 441 }) {
		for (const bool locked : { false, true }) {
			CAPTURE(block);
			CAPTURE(locked);
			Venue v(1024);
			const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
			    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
			REQUIRE(chain != nullptr);
			REQUIRE(v.test()->add_device(v.plugin, "SKLITTER") != 0);
			v.run(); // the pre-roll: a whole block, no transport
			REQUIRE(chain->set_control(v.plugin, 0, "Transport Locked", locked ? 1.0 : 0.0));

			long long frame = 0;
			long long wholeCodes = 0;
			long long onPhase = 0;
			bool pressed = false;
			const auto roll = [&](long long until, bool listen) {
				while (frame < until) {
					const int n = block;
					for (int k = 0; k < n; ++k) {
						const long long f = frame + k;
						v.inL[static_cast<size_t>(k)] = v.inR[static_cast<size_t>(k)] =
						    (1000000.0f + static_cast<float>(f)) * ((f & 1) != 0 ? -1.0f : 1.0f);
					}
					const double beats = static_cast<double>(frame) / (kSpb * kSr);
					clap_event_transport tp {};
					tp.header.size = sizeof(tp);
					tp.header.type = CLAP_EVENT_TRANSPORT;
					tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
					           | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
					tp.tempo = 60.0 / kSpb;
					tp.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
					tp.song_pos_seconds =
					    static_cast<clap_sectime>(std::llround(static_cast<double>(frame) / kSr * CLAP_SECTIME_FACTOR));
					clap_input_events none = emptyEvents();
					clap_output_events sink = sinkEvents();
					clap_process p {};
					p.frames_count = static_cast<uint32_t>(n);
					p.transport = &tp;
					p.audio_inputs = &v.in;
					p.audio_inputs_count = 1;
					p.audio_outputs = &v.out;
					p.audio_outputs_count = 1;
					p.in_events = &none;
					p.out_events = &sink;
					v.plugin->process(v.plugin, &p);
					for (int k = 0; listen && k < n; ++k) {
						const double a = std::fabs(static_cast<double>(v.l[static_cast<size_t>(k)]));
						const long long src = std::llround(a) - 1000000;
						const long long f = frame + k;
						if (std::fabs(a - std::round(a)) > 0.01 || src < 0 || src == f) {
							continue; // a fade, or the dry input
						}
						++wholeCodes;
						onPhase += (((f - src) % kBar) + kBar) % kBar == 0 ? 1 : 0;
					}
					frame += n;
				}
			};
			roll(kBar * 9 / 4 + 3 * kBar / 8, false); // listening to beat 9.5, off the grid
			REQUIRE(chain->set_control(v.plugin, 0, "Engage", 1.0));
			pressed = true;
			roll(frame + kBar / 2, false);
			roll(frame + kBar * 3, pressed);
			CAPTURE(onPhase);
			CAPTURE(wholeCodes);
			REQUIRE(wholeCodes > kBar * 5 / 2); // it played: not a pass-through reading as in phase
			// ~0.15% of whole codes are a fade's gain landing on an integer, not an offset (device suite).
			CHECK(static_cast<double>(onPhase) >= 0.995 * static_cast<double>(wholeCodes));
		}
	}
}

//------------------------------------------------------------------------------------------------//
// MACRO A9 — THE DAW SEES A MACRO'S NAME, on the rack as on Feathers (`test_clap_render.cpp`). The     //
// flags asserted are `clap/ext/params.h`'s own constants: INFO for a name change, never ALL.          //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE VENUE'S HOST, PLUS `clap_host_params`, WRITING DOWN EVERY RESCAN. `host_data` points at the log,
/// so the log must outlive the venue built on it.
struct RescanLog {
	std::vector<clap_param_rescan_flags> rescans;

	clap_host host() {
		clap_host h = makeHost();
		h.host_data = this;
		h.get_extension = [](const clap_host* self, const char* id) -> const void* {
			static const clap_host_params kParams {
				[](const clap_host* hh, clap_param_rescan_flags flags) {
					static_cast<RescanLog*>(hh->host_data)->rescans.push_back(flags);
				},
				[](const clap_host*, clap_id, clap_param_clear_flags) {},
				[](const clap_host*) {},
			};
			if (id != nullptr && std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
				return &kParams;
			}
			return makeHost().get_extension(self, id); // the timer, as every venue offers
		};
		return h;
	}
};

std::string rackHostNameOf(const clap_plugin* plugin, clap_id id) {
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

void rackRenameMacro(const clap_plugin* plugin, uint32_t index, const char* name) {
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(chain->rename_macro != nullptr);
	REQUIRE(chain->rename_macro(plugin, index, name));
}

} // namespace

TEST_CASE("the rack: an unrenamed macro keeps its default name in paramsInfo") {
	Venue v;
	// THE NAMES EVERY BUILD BEFORE THIS ONE REPORTED, spelled rather than read from our table.
	for (clap_id id = 0; id < 8; ++id) {
		CAPTURE(id);
		CHECK(rackHostNameOf(v.plugin, id) == "Macro " + std::to_string(id + 1));
	}
	// AND A NAME TAKEN AWAY AGAIN IS THE DEFAULT, not the name it had.
	rackRenameMacro(v.plugin, 7, "Tail");
	REQUIRE(rackHostNameOf(v.plugin, 7) == "Tail");
	rackRenameMacro(v.plugin, 7, "");
	CHECK(rackHostNameOf(v.plugin, 7) == "Macro 8");
}

TEST_CASE("the rack: paramsInfo reports a renamed macro's name") {
	Venue v;
	rackRenameMacro(v.plugin, 4, "Space");
	CHECK(rackHostNameOf(v.plugin, 4) == "Space");
	CHECK(rackHostNameOf(v.plugin, 3) == "Macro 4");
	CHECK(rackHostNameOf(v.plugin, 5) == "Macro 6");
}

TEST_CASE("the rack: a rename asks the host to rescan info, once") {
	RescanLog log;
	Venue v(log.host());
	const size_t before = log.rescans.size();
	rackRenameMacro(v.plugin, 0, "Drive");
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
	// NOT ALL: the list and the ids did not move, and ALL is refused to an active plugin.
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_ALL) == 0);
	CHECK(rackHostNameOf(v.plugin, 0) == "Drive");
	rackRenameMacro(v.plugin, 0, "Drive"); // the name it has: not a rename, and costs the host nothing
	CHECK(log.rescans.size() == before + 1);
}

TEST_CASE("the rack: a state load that renames three macros rescans info once") {
	// THE PATCH, SAVED BY A RACK WHOSE MACROS WERE NAMED — the real save road, not bytes of ours.
	std::vector<char> patch;
	{
		Venue writer;
		rackRenameMacro(writer.plugin, 1, "One");
		rackRenameMacro(writer.plugin, 2, "Two");
		rackRenameMacro(writer.plugin, 6, "Six");
		const auto* state =
		    static_cast<const clap_plugin_state*>(writer.plugin->get_extension(writer.plugin, CLAP_EXT_STATE));
		REQUIRE(state != nullptr);
		clap_ostream out {};
		out.ctx = &patch;
		out.write = [](const clap_ostream* s, const void* buf, uint64_t n) -> int64_t {
			auto* bytes = static_cast<std::vector<char>*>(s->ctx);
			const char* from = static_cast<const char*>(buf);
			bytes->insert(bytes->end(), from, from + n);
			return static_cast<int64_t>(n);
		};
		REQUIRE(state->save(writer.plugin, &out));
	}

	RescanLog log;
	Venue v(log.host());
	const auto* state = static_cast<const clap_plugin_state*>(v.plugin->get_extension(v.plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	auto infoSince = [&](size_t from) {
		size_t n = 0;
		for (size_t i = from; i < log.rescans.size(); ++i) {
			n += (log.rescans[i] & CLAP_PARAM_RESCAN_INFO) != 0 ? 1 : 0;
		}
		return n;
	};

	const size_t before = log.rescans.size();
	{
		ReadStream s(patch);
		REQUIRE(state->load(v.plugin, &s.in));
	}
	// ONE CALL FOR THE WHOLE LOAD, not one per renamed macro.
	CHECK(log.rescans.size() == before + 1);
	CHECK(infoSince(before) == 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_ALL) == 0);
	CHECK(rackHostNameOf(v.plugin, 1) == "One");
	CHECK(rackHostNameOf(v.plugin, 2) == "Two");
	CHECK(rackHostNameOf(v.plugin, 6) == "Six");
	CHECK(rackHostNameOf(v.plugin, 0) == "Macro 1");

	// THE SAME PATCH AGAIN RENAMES NOTHING, AND ASKS NOTHING — so the one above was the load's renames.
	const size_t reload = log.rescans.size();
	{
		ReadStream s(patch);
		REQUIRE(state->load(v.plugin, &s.in));
	}
	CHECK(infoSince(reload) == 0);
}

//------------------------------------------------------------------------------------------------//
// HOST SLOTS T10 — THE STANDALONE RACK OWNS A POOL (decided 2026-09-25). Feathers'                   //
// write roads, on the rack's own CLAP road (`test_clap_render.cpp` has Feathers' cases). Every claim  //
// is about a VALUE read where it lives (CLAUDE.md): the ENGINE's base and resolved value              //
// (`engine_control`), what the KNOB reads (`knob_value`), the POOL's state (`slot_state`), and what   //
// the plugin pushed to the host's output queue (`SlotLog`). Every expected number is restated from   //
// the platform's contract — the declared range and `fromNormal`/`coerceControl` — never read back   //
// from the road under test.                                                                        //
// (hs-int x mainline: one anonymous namespace below holds both sections' helpers.)                 //
//------------------------------------------------------------------------------------------------//
// BACKLOG JV AND JW — WHAT THE HOST IS TOLD. Nearly all of the rack is not a parameter, so after an     //
// edit only `clap_host_state.mark_dirty` gives the DAW its "modified" mark (JV); after a load, only     //
// `CLAP_PARAM_RESCAN_VALUES` tells it the macros moved under its cached copy (JW). Both are counted     //
// through a host of the case's own, and the flags are `clap/ext/params.h`'s constants.                 //
//------------------------------------------------------------------------------------------------//

namespace {

constexpr clap_id kSlot0 = 0x1000u;
constexpr clap_id kSlot1 = 0x1001u;
const std::string kDot = "\xc2\xb7"; // "·"

const clap_plugin_feathers_test_chain* slotChainOf(const clap_plugin* plugin) {
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    plugin->get_extension(plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	return chain;
}

const clap_plugin_params* slotParamsOf(const clap_plugin* plugin) {
	const auto* params = static_cast<const clap_plugin_params*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	return params;
}

const device::ParamDesc& filterControl(const char* key) {
	for (const device::ParamDesc& p : fx::filter::kDescriptor.params) {
		if (p.key != nullptr && std::strcmp(p.key, key) == 0) {
			return p;
		}
	}
	FAIL("the filter declares no control keyed " << key);
	return fx::filter::kDescriptor.params[0];
}

uint32_t stateNumber(hostslots::SlotState s) { return static_cast<uint32_t>(s); }

/// WHAT A HOST WRITE OF `normal` IS IN THE CONTROL'S UNITS, stored as the engine stores a base: the declared
/// range (`fromNormal`), then the float the row holds, coerced by the declaration.
double hostValueOf(const device::ParamDesc& desc, double normal) {
	return static_cast<double>(device::coerceControl(desc, static_cast<float>(device::fromNormal(desc, normal))));
}

/// THE HOST'S MAIN-THREAD CALLBACK, as a host runs it after `request_callback`.
void mainThreadVisit(const clap_plugin* plugin) { plugin->on_main_thread(plugin); }

/// A HOST'S EVENT LIST: param values and mods, in the order pushed.
struct SlotEvents {
	std::vector<clap_event_param_value> values;
	std::vector<clap_event_param_mod> mods;
	std::vector<const clap_event_header*> order;
	clap_input_events in {};

	void pushParam(clap_id id, double value) {
		clap_event_param_value e {};
		e.header.size = sizeof(e);
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = CLAP_EVENT_PARAM_VALUE;
		e.param_id = id;
		e.note_id = -1;
		e.port_index = -1;
		e.channel = -1;
		e.key = -1;
		e.value = value;
		values.push_back(e);
	}
	void pushMod(clap_id id, double amount) {
		clap_event_param_mod e {};
		e.header.size = sizeof(e);
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = CLAP_EVENT_PARAM_MOD;
		e.param_id = id;
		e.note_id = -1;
		e.port_index = -1;
		e.channel = -1;
		e.key = -1;
		e.amount = amount;
		mods.push_back(e);
	}
	/// The list as CLAP reads it — built once every event is pushed, so no pointer moves under it.
	const clap_input_events* input() {
		order.clear();
		for (const auto& e : values) {
			order.push_back(&e.header);
		}
		for (const auto& e : mods) {
			order.push_back(&e.header);
		}
		in.ctx = this;
		in.size = [](const clap_input_events* l) -> uint32_t {
			return static_cast<uint32_t>(static_cast<const SlotEvents*>(l->ctx)->order.size());
		};
		in.get = [](const clap_input_events* l, uint32_t i) -> const clap_event_header* {
			return static_cast<const SlotEvents*>(l->ctx)->order[i];
		};
		return &in;
	}
};

/// THE HOST'S OUTPUT QUEUE, WRITTEN DOWN: every param value and gesture the plugin pushed.
struct SlotLog {
	struct Event {
		uint16_t type = 0;
		clap_id id = CLAP_INVALID_ID;
		double value = 0.0;
	};
	std::vector<Event> events;
	clap_output_events out {};

	SlotLog() {
		out.ctx = this;
		out.try_push = [](const clap_output_events* o, const clap_event_header* h) {
			auto* log = static_cast<SlotLog*>(o->ctx);
			if (h->type == CLAP_EVENT_PARAM_VALUE) {
				const auto* e = reinterpret_cast<const clap_event_param_value*>(h);
				log->events.push_back({ h->type, e->param_id, e->value });
			}
			else if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
				const auto* e = reinterpret_cast<const clap_event_param_gesture*>(h);
				log->events.push_back({ h->type, e->param_id, 0.0 });
			}
			return true;
		};
	}
	std::vector<Event> on(clap_id id) const {
		std::vector<Event> out;
		for (const Event& e : events) {
			if (e.id == id) {
				out.push_back(e);
			}
		}
		return out;
	}
};

/// ONE BLOCK, with the host's events, its output queue written down, and a transport carrying `flags`.
clap_process_status runBlock(Venue& v, const clap_input_events* events = nullptr, SlotLog* log = nullptr,
                             uint32_t transportFlags = 0) {
	clap_input_events none = emptyEvents();
	clap_output_events sink = sinkEvents();
	clap_event_transport transport {};
	transport.header.size = sizeof(transport);
	transport.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	transport.header.type = CLAP_EVENT_TRANSPORT;
	transport.flags = transportFlags;
	clap_process p {};
	p.frames_count = static_cast<uint32_t>(v.l.size());
	p.audio_inputs = &v.in;
	p.audio_inputs_count = 1;
	p.audio_outputs = &v.out;
	p.audio_outputs_count = 1;
	p.in_events = events != nullptr ? events : &none;
	p.out_events = log != nullptr ? &log->out : &sink;
	p.transport = &transport;
	return v.plugin->process(v.plugin, &p);
}

struct EngineReading {
	double base = -1.0;
	double resolved = -1.0;
};

EngineReading engineOf(const clap_plugin* plugin, const char* control) {
	EngineReading r;
	REQUIRE(slotChainOf(plugin)->engine_control(plugin, 0, control, &r.base, &r.resolved));
	return r;
}

double knobOf(const clap_plugin* plugin, const char* control) {
	double v = -1.0;
	REQUIRE(slotChainOf(plugin)->knob_value(plugin, 0, control, &v));
	return v;
}

uint32_t slotStateOf(const clap_plugin* plugin, uint32_t n) { return slotChainOf(plugin)->slot_state(plugin, n); }

void drag(const clap_plugin* plugin, const char* control, std::initializer_list<double> values) {
	const std::vector<double> v(values);
	REQUIRE(slotChainOf(plugin)->drag_control(plugin, 0, control, v.data(), static_cast<uint32_t>(v.size())));
}

clap_param_info slotInfoOf(const clap_plugin* plugin, uint32_t n) {
	clap_param_info info {};
	REQUIRE(slotParamsOf(plugin)->get_info(plugin, 8 + n, &info));
	REQUIRE(info.id == 0x1000u + n);
	return info;
}

/// A rack with one filter at position 0, its cutoff PINNED on slot 0 and BOUND: the pin, the main-thread visit
/// that publishes the binding, then a block so the engine has adopted it.
void rackWithPinnedCutoff(Venue& v) {
	const clap_plugin_feathers_test_chain* chain = slotChainOf(v.plugin);
	REQUIRE(chain->add_device(v.plugin, fx::filter::kTraits.tag) == 0u);
	REQUIRE(chain->pin_slot(v.plugin, 0, "Filter Cutoff") == 0u);
	mainThreadVisit(v.plugin);
	runBlock(v);
}

std::vector<char> saveRackState(const clap_plugin* plugin) {
	const auto* state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	std::vector<char> bytes;
	clap_ostream out {};
	out.ctx = &bytes;
	out.write = [](const clap_ostream* s, const void* buf, uint64_t n) -> int64_t {
		auto* b = static_cast<std::vector<char>*>(s->ctx);
		const char* from = static_cast<const char*>(buf);
		b->insert(b->end(), from, from + n);
		return static_cast<int64_t>(n);
	};
	REQUIRE(state->save(plugin, &out));
	return bytes;
}

/// THE VENUE'S HOST, PLUS `clap_host_state` AND `clap_host_params`, COUNTING WHAT THE PLUGIN SAYS.
struct HostLog {
	int dirty = 0;
	std::vector<clap_param_rescan_flags> rescans;

	clap_host host() {
		clap_host h = makeHost();
		h.host_data = this;
		h.get_extension = [](const clap_host* self, const char* id) -> const void* {
			static const clap_host_state kState {
				[](const clap_host* hh) { ++static_cast<HostLog*>(hh->host_data)->dirty; },
			};
			static const clap_host_params kParams {
				[](const clap_host* hh, clap_param_rescan_flags flags) {
					static_cast<HostLog*>(hh->host_data)->rescans.push_back(flags);
				},
				[](const clap_host*, clap_id, clap_param_clear_flags) {},
				[](const clap_host*) {},
			};
			if (id != nullptr && std::strcmp(id, CLAP_EXT_STATE) == 0) {
				return &kState;
			}
			if (id != nullptr && std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
				return &kParams;
			}
			static const clap_host_tail kTail {
				[](const clap_host* hh) { ++static_cast<HostLog*>(hh->host_data)->tailChanged; },
			};
			if (id != nullptr && std::strcmp(id, CLAP_EXT_TAIL) == 0) {
				return &kTail;
			}
			return makeHost().get_extension(self, id); // the timer, as every venue offers
		};
		return h;
	}
	int tailChanged = 0;
};

/// ONE MAIN-THREAD VISIT FROM THE HOST — the timer — which is where the plugin says what the visit's edits did.
void visit(const clap_plugin* plugin) {
	const auto* timer =
	    static_cast<const clap_plugin_timer_support*>(plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
	REQUIRE(timer != nullptr);
	timer->on_timer(plugin, 0);
}

rack::RackBridge& bridgeOf(const Venue& v) {
	REQUIRE(v.test() != nullptr);
	REQUIRE(v.test()->bridge != nullptr);
	auto* bridge = static_cast<rack::RackBridge*>(v.test()->bridge(v.plugin));
	REQUIRE(bridge != nullptr);
	return *bridge;
}

std::vector<char> saveState(const clap_plugin* plugin) {
	std::vector<char> bytes;
	const auto* state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	clap_ostream out {};
	out.ctx = &bytes;
	out.write = [](const clap_ostream* s, const void* buf, uint64_t n) -> int64_t {
		auto* b = static_cast<std::vector<char>*>(s->ctx);
		const char* from = static_cast<const char*>(buf);
		b->insert(b->end(), from, from + n);
		return static_cast<int64_t>(n);
	};
	REQUIRE(state->save(plugin, &out));
	return bytes;
}

bool loadRackState(const clap_plugin* plugin, const std::vector<char>& bytes) {
	const auto* state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	ReadStream s(bytes);
	return state->load(plugin, &s.in);
}

bool mentions(const std::vector<char>& bytes, const std::string& word) {
	return std::string(bytes.begin(), bytes.end()).find(word) != std::string::npos;
}

bool loadState(const clap_plugin* plugin, std::vector<char> bytes) {
	const auto* state = static_cast<const clap_plugin_state*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
	REQUIRE(state != nullptr);
	ReadStream s(std::move(bytes));
	return state->load(plugin, &s.in);
}

/// A VALUE OF CONTROL `c` THAT IS NOT THE ONE IT HOLDS — its max, or its min when it is at max.
float otherValue(const rack::RackBridge& bridge, DeviceInstanceId id, size_t c) {
	const device::Descriptor* d = bridge.descriptorOf(id);
	REQUIRE(d != nullptr);
	REQUIRE(c < d->params.size());
	const float max = static_cast<float>(d->params[c].max);
	return bridge.fieldAt(id, c) != max ? max : static_cast<float>(d->params[c].min);
}

/// THE FIRST CONTROL A MACRO MAY BE ROUTED AT — a switch or a Choice is refused by the routing rule.
size_t routableControl(const rack::RackBridge& bridge, DeviceInstanceId id) {
	const device::Descriptor* d = bridge.descriptorOf(id);
	REQUIRE(d != nullptr);
	for (size_t c = 0; c < d->params.size(); ++c) {
		if (d->params[c].modulatable) {
			return c;
		}
	}
	FAIL("no modulatable control");
	return 0;
}

} // namespace

TEST_CASE("a slot in the rack plugin records a drag") {
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rackWithPinnedCutoff(v);
	// THE HOST SEES THE SLOT BY ITS CONTROL'S NAME, and no longer hidden.
	const clap_param_info info = slotInfoOf(v.plugin, 0);
	CHECK(std::string(info.name) == "Filter " + kDot + " Cutoff");
	CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) == 0);

	const double a = std::log2(300.0);
	const double b = std::log2(900.0);
	const double c = std::log2(4000.0);
	// THE CARD'S OWN WIDGET over the rack's bridge — `DeviceControl::beginGesture` / `setValue` / `endGesture`,
	// which reach the bridge's `begin/endControlGesture` (a `DeviceHost` virtual) and `setField`.
	drag(v.plugin, "Filter Cutoff", {a, b, c});

	SlotLog log;
	runBlock(v, nullptr, &log);
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 5);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[4].type == CLAP_EVENT_PARAM_GESTURE_END);
	const double values[3] = {a, b, c};
	for (size_t i = 0; i < 3; ++i) {
		CAPTURE(i);
		CHECK(ev[i + 1].type == CLAP_EVENT_PARAM_VALUE);
		// THE STORED VALUE'S POSITION, restated from the declared range and sent as the float the ring holds.
		const double stored = static_cast<double>(device::coerceControl(desc, static_cast<float>(values[i])));
		CHECK(ev[i + 1].value == static_cast<double>(static_cast<float>(device::toNormal(desc, stored))));
	}
	// NOTHING ELSE ON ANY PARAMETER, and nothing at the next block: the ring was drained once.
	CHECK(log.events.size() == 5);
	SlotLog next;
	runBlock(v, nullptr, &next);
	CHECK(next.events.empty());
	// AND THE HAND'S LAST VALUE IS WHAT THE ENGINE RUNS — the bus was seeded, so the host's last value (none)
	// did not put anything back over it.
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == static_cast<double>(static_cast<float>(c)));

	// A CONTROL WITH NO SLOT is given one by the hand and bound at its Begin (a first grab is serviced there,
	// `RackBridge::beginControlGesture`): the next block carries its drag on the NEW slot, never on this one.
	drag(v.plugin, "Filter Res", {0.1, 0.7});
	SlotLog unslotted;
	runBlock(v, nullptr, &unslotted);
	CHECK(unslotted.on(kSlot0).empty());
	const std::vector<SlotLog::Event> fresh = unslotted.on(kSlot0 + 1);
	REQUIRE(fresh.size() >= 2);
	CHECK(fresh[0].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(fresh[1].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(fresh.back().type == CLAP_EVENT_PARAM_GESTURE_END);

	// AND A DRAG WHILE THE HOST RECORDS ANCHORS THE SLOT (design §5); the one above, not recording, did not.
	mainThreadVisit(v.plugin);
	REQUIRE(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Pinned));
	runBlock(v, nullptr, nullptr, CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_IS_RECORDING);
	drag(v.plugin, "Filter Cutoff", {std::log2(500.0)});
	mainThreadVisit(v.plugin);
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Anchored));
}

TEST_CASE("host automation on a rack slot moves the control") {
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rackWithPinnedCutoff(v);
	const double expected = hostValueOf(desc, 0.3);
	// NOT VACUOUS: the control does not already stand there.
	REQUIRE(engineOf(v.plugin, "Filter Cutoff").base != expected);

	SlotEvents write;
	write.pushParam(kSlot0, 0.3);
	runBlock(v, write.input());
	// IN THAT BLOCK, with no main-thread visit in between: the engine read the bus itself.
	const EngineReading r = engineOf(v.plugin, "Filter Cutoff");
	CHECK(r.base == expected);
	CHECK(r.resolved == expected);
	// NOT ANCHORED UNTIL THE VISIT — the audio thread only sets a bit — and then the knob and the host follow.
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Pinned));
	mainThreadVisit(v.plugin);
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Anchored));
	CHECK(knobOf(v.plugin, "Filter Cutoff") == expected);
	double hostReads = -1.0;
	REQUIRE(slotParamsOf(v.plugin)->get_value(v.plugin, kSlot0, &hostReads));
	CHECK(hostReads == doctest::Approx(device::toNormal(desc, expected)).epsilon(1e-9));
	// THE SLOT'S TEXT IS THE FILTER'S OWN, through `device::formatValue` — restated, not read back.
	char text[64] {};
	REQUIRE(slotParamsOf(v.plugin)->value_to_text(v.plugin, kSlot0, 0.3, text, sizeof(text)));
	CHECK(std::string(text) == device::formatValue(desc, device::fromNormal(desc, 0.3)));
	// A MACRO IS STILL A MACRO: the slot's write moved no macro.
	double macro = -1.0;
	REQUIRE(slotParamsOf(v.plugin)->get_value(v.plugin, 0, &macro));
	CHECK(macro == 0.0);

	// GLOBAL MODULATION ON THE SLOT: an offset of amount × the control's route span on top of the base, which
	// does not move — restated here from the declaration.
	SlotEvents mod;
	mod.pushMod(kSlot0, 0.1);
	runBlock(v, mod.input());
	const EngineReading m = engineOf(v.plugin, "Filter Cutoff");
	CHECK(m.base == expected);
	CHECK(m.resolved
	      == static_cast<double>(static_cast<float>(
	          device::withRouteOffset(desc, expected, 0.1 * device::routeSpanOf(desc)))));
	CHECK(m.resolved != expected);
	SlotEvents back;
	back.pushMod(kSlot0, 0.0);
	runBlock(v, back.input());
	CHECK(engineOf(v.plugin, "Filter Cutoff").resolved == expected);
}

TEST_CASE("the rack: a drag on a freshly slotted control reaches the host on that slot") {
	// T9 IN THE STANDALONE RACK: its knobs are `DeviceControl`s over the rack's bridge, whose `touchControl`
	// reaches the same `SlotOwner::touch` as Feathers'. So the same stream, restated rather than shared.
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rack::RackBridge& bridge = bridgeOf(v);
	REQUIRE(bridge.addDevice(fx::filter::kTraits.tag) != kNoInstance);
	v.settle();
	mainThreadVisit(v.plugin);
	REQUIRE(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Empty));

	const double a = std::log2(300.0);
	const double c = std::log2(4000.0);
	const auto sentAs = [&](double v) {
		return static_cast<double>(
		    static_cast<float>(device::toNormal(desc, static_cast<double>(device::coerceControl(desc, static_cast<float>(v))))));
	};
	// THE BEGIN BINDS THE SLOT AND NAMES IT (a first grab is serviced there, 2026-11-09; it was the next visit's):
	// named before the hand's call returns, and the very next block carries the takeover at the control's value
	// before the hand, then the hand's gesture from there — every value it made, which the late binding lost.
	const double before = engineOf(v.plugin, "Filter Cutoff").base;
	drag(v.plugin, "Filter Cutoff", {a, c});
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Recent));
	CHECK(std::string(slotInfoOf(v.plugin, 0).name) == "Filter " + kDot + " Cutoff");
	SlotLog log;
	runBlock(v, nullptr, &log);
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 6);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_VALUE); // the takeover, outside any gesture
	CHECK(ev[0].value == sentAs(before));
	CHECK(ev[1].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[2].value == sentAs(before));
	CHECK(ev[3].value == sentAs(a));
	CHECK(ev[4].value == sentAs(c));
	CHECK(ev[5].type == CLAP_EVENT_PARAM_GESTURE_END);
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == static_cast<double>(static_cast<float>(c)));
}

TEST_CASE("the rack: a host mod on a Pinned slot anchors it") {
	Venue v;
	rackWithPinnedCutoff(v);
	REQUIRE(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Pinned));
	SlotEvents rest;
	rest.pushMod(kSlot0, 0.0);
	runBlock(v, rest.input());
	mainThreadVisit(v.plugin);
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Pinned)); // a modulator at rest: nothing
	SlotEvents mod;
	mod.pushMod(kSlot0, 0.1);
	runBlock(v, mod.input());
	mainThreadVisit(v.plugin);
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Anchored));
}

TEST_CASE("a rack slot write while asleep reaches the control through flush") {
	// NO BLOCK RUNS HERE AFTER THE SETUP: a host whose rack is asleep (the transport stopped, the plugin
	// silent) moves parameters through `params.flush` alone (plan §0.13), and before T10 the rack's flush
	// ignored its output queue entirely.
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rackWithPinnedCutoff(v);

	// A HAND ON THE KNOB, and a host write, both in one flush.
	drag(v.plugin, "Filter Cutoff", {std::log2(2000.0)});
	SlotEvents events;
	events.pushParam(kSlot0, 0.25);
	SlotLog log;
	slotParamsOf(v.plugin)->flush(v.plugin, events.input(), &log.out);
	// THE HAND REACHED THE HOST THROUGH THE FLUSH: Begin, Value, End on the slot.
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);

	// THE HOST'S WRITE REACHED THE CONTROL: after the visit the knob reads it and the slot is anchored…
	mainThreadVisit(v.plugin);
	const double expected = hostValueOf(desc, 0.25);
	CHECK(knobOf(v.plugin, "Filter Cutoff") == expected);
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Anchored));
	// …and the engine runs it at its next block, with no process() having carried the event.
	runBlock(v);
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == expected);
}

TEST_CASE("the rack's slots survive save and reload with anchors") {
	const device::ParamDesc& cutoff = filterControl("filter_cutoff_wide");
	const double hand = static_cast<double>(static_cast<float>(std::log2(700.0)));
	std::vector<char> saved;
	{
		Venue writer;
		rackWithPinnedCutoff(writer);
		REQUIRE(slotChainOf(writer.plugin)->set_control(writer.plugin, 0, "Filter Cutoff", std::log2(700.0)));
		REQUIRE(slotChainOf(writer.plugin)->touch_slot(writer.plugin, 0, "Filter Res") == 1u);
		mainThreadVisit(writer.plugin);
		REQUIRE(slotStateOf(writer.plugin, 1) == stateNumber(hostslots::SlotState::Recent));
		// THE HOST WRITES RES, and NO visit sees it before the save: `stateSave` services first.
		SlotEvents write;
		write.pushParam(kSlot1, 0.6);
		runBlock(writer, write.input());
		REQUIRE(slotStateOf(writer.plugin, 1) == stateNumber(hostslots::SlotState::Recent));
		saved = saveRackState(writer.plugin);
		CHECK(slotStateOf(writer.plugin, 1) == stateNumber(hostslots::SlotState::Anchored));
	}
	CHECK(mentions(saved, "hostSlots"));

	// A FRESH RACK, THE HOST'S LOAD, AND NOTHING ELSE — no host event, no visit of our own.
	RescanLog rescans;
	Venue v(rescans.host());
	const size_t before = rescans.rescans.size();
	REQUIRE(loadRackState(v.plugin, saved));
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Pinned));
	CHECK(slotStateOf(v.plugin, 1) == stateNumber(hostslots::SlotState::Anchored));
	CHECK(std::string(slotInfoOf(v.plugin, 0).name) == "Filter " + kDot + " Cutoff");
	CHECK(std::string(slotInfoOf(v.plugin, 1).name) == "Filter " + kDot + " Res");
	CHECK(std::string(slotInfoOf(v.plugin, 2).name) == "Slot 3");
	// ONE RESCAN FOR THE LOAD, carrying the slots' renames.
	REQUIRE(rescans.rescans.size() == before + 1);
	CHECK((rescans.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
	CHECK((rescans.rescans.back() & CLAP_PARAM_RESCAN_ALL) == 0);
	// THE VALUES CAME WITH THE PATCH: the hand's cutoff, the host's res, and the host reads them on the slots.
	CHECK(knobOf(v.plugin, "Filter Cutoff") == hand);
	CHECK(knobOf(v.plugin, "Filter Res") == hostValueOf(filterControl("filter_res"), 0.6));
	double hostReads = -1.0;
	REQUIRE(slotParamsOf(v.plugin)->get_value(v.plugin, kSlot0, &hostReads));
	CHECK(hostReads == doctest::Approx(device::toNormal(cutoff, hand)).epsilon(1e-9));

	// AND THE SLOTS DRIVE THEIR CONTROLS STRAIGHT AWAY: a host write in the first block lands in that block.
	SlotEvents write;
	write.pushParam(kSlot0, 0.8);
	runBlock(v, write.input());
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == hostValueOf(cutoff, 0.8));

	// A PATCH THAT MAPPED NOTHING SAYS NOTHING OF SLOTS, byte-for-byte what it was before slots existed.
	Venue empty;
	REQUIRE(slotChainOf(empty.plugin)->add_device(empty.plugin, fx::filter::kTraits.tag) == 0u);
	CHECK_FALSE(mentions(saveRackState(empty.plugin), "hostSlots"));
}

TEST_CASE("removing a device from the rack frees its slot, anchored or not") {
	Venue v;
	rackWithPinnedCutoff(v);
	SlotEvents write;
	write.pushParam(kSlot0, 0.5);
	runBlock(v, write.input());
	mainThreadVisit(v.plugin);
	REQUIRE(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Anchored));

	REQUIRE(slotChainOf(v.plugin)->remove_device(v.plugin, 0));
	mainThreadVisit(v.plugin);
	// By design an anchored slot is simply freed back into the pool.
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Empty));
	const clap_param_info info = slotInfoOf(v.plugin, 0);
	CHECK(std::string(info.name) == "Slot 1");
	CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) != 0);
	// AND A HOST WRITE TO THE FREED SLOT DRIVES NOTHING and anchors nothing.
	SlotEvents after;
	after.pushParam(kSlot0, 0.9);
	runBlock(v, after.input());
	mainThreadVisit(v.plugin);
	CHECK(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Empty));
}

TEST_CASE("a rack bridge that owns no pool writes no hostSlots and ignores them on load") {
	// THE NESTED CASE (design §7): a rack inside a chain is built without a pool, so its body never carries
	// slots, and a body that does is not this level's to apply. The pool's patch comes from an owning bridge.
	rack::RackEngine ownerEngine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge owner(ownerEngine, nullptr, rack::RackBridge::HostSlots::OwnPool);
	const DeviceInstanceId filter = owner.addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	REQUIRE(owner.hostSlots() != nullptr);
	const std::optional<hostslots::Target> target = owner.hostSlots()->targetOf(filter, 0);
	REQUIRE(target.has_value());
	REQUIRE(owner.hostSlots()->pool().pin(*target).slot == 0u);
	const rack::RackPatch patch = owner.patch();
	REQUIRE(patch.hostSlots.has_value());

	rack::RackEngine nestedEngine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge nested(nestedEngine);
	CHECK(nested.hostSlots() == nullptr);
	(void)nested.loadPatch(patch);
	CHECK(nested.chainLength() == 1);
	CHECK(nested.hostSlots() == nullptr);
	CHECK_FALSE(nested.patch().hostSlots.has_value());
	CHECK(nested.serviceHostSlots() == 0);
}

//------------------------------------------------------------------------------------------------//
// THE T6b REVIEW'S RULES ON THE RACK'S ROADS — they live in `rack::SlotOwner` since the integration, so //
// both plugins have them; `test_clap_render.cpp` has Feathers' cases. The rack has no road that        //
// rewrites a whole row: a card's settings road writes field by field through `setField`, so the rule   //
// that keeps an unmirrored host write is the skip of a write that changed nothing.                     //
//------------------------------------------------------------------------------------------------//

TEST_CASE("KO, the rack: a takeover tells the host to re-read the slot's VALUE, and the value it reads is the control's") {
	// Feathers' case of the same name, on the rack's own service road: a host that cached a slot while it was
	// Empty holds 0.0 for it until a rescan asks it to read the value again.
	HostLog log;
	Venue v(log.host());
	const clap_plugin_feathers_test_chain* chain = slotChainOf(v.plugin);
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	REQUIRE(chain->add_device(v.plugin, fx::filter::kTraits.tag) == 0u);
	const double cutoff = std::log2(2500.0);
	REQUIRE(chain->set_control(v.plugin, 0, "Filter Cutoff", cutoff));
	visit(v.plugin);
	runBlock(v);
	double cached = -1.0;
	REQUIRE(slotParamsOf(v.plugin)->get_value(v.plugin, kSlot0, &cached));
	REQUIRE(cached == 0.0); // EMPTY: what the host caches
	const size_t before = log.rescans.size();

	REQUIRE(chain->touch_slot(v.plugin, 0, "Filter Cutoff") == 0u);
	visit(v.plugin);
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
	double reads = -1.0;
	REQUIRE(slotParamsOf(v.plugin)->get_value(v.plugin, kSlot0, &reads));
	const double stored = static_cast<double>(device::coerceControl(desc, static_cast<float>(cutoff)));
	CHECK(reads == doctest::Approx(device::toNormal(desc, stored)).epsilon(1e-9));
	CHECK(reads != 0.0);
}

TEST_CASE("KO, the rack: editing one control of a device never puts another of its controls back, on any device") {
	// THE DEFECT: controls clicked back to their defaults after another parameter on the same device
	// changed. The rack holds every kind of device, modulators included, so each one is
	// set up alone and EVERY control a hand can name is dragged away from its default by the widget's own
	// sequence — each drag also taking a Recent host slot (T9) — with a visit and a block between. Then every
	// control is read where it lives: the knob and the engine's base. Expected numbers are the declaration's.
	std::vector<std::string> tags;
	{
		Venue lister;
		const clap_plugin_feathers_test_chain* chain = slotChainOf(lister.plugin);
		char tag[64] {};
		for (uint32_t i = 0; chain->available_device(lister.plugin, i, tag, sizeof(tag)); ++i) {
			tags.emplace_back(tag);
		}
	}
	REQUIRE_FALSE(tags.empty());
	size_t checked = 0;
	for (const std::string& tag : tags) {
		CAPTURE(tag);
		const device::Descriptor* desc = deviceForTag(tag.c_str());
		REQUIRE(desc != nullptr);
		Venue v;
		const clap_plugin_feathers_test_chain* chain = slotChainOf(v.plugin);
		if (chain->add_device(v.plugin, tag.c_str()) != 0u) {
			continue; // a device the rack will not take alone
		}
		mainThreadVisit(v.plugin);
		runBlock(v);

		struct Want {
			std::string name;
			double value;
		};
		std::vector<Want> wants;
		for (const device::ParamDesc& p : desc->params) {
			if (p.name == nullptr) {
				continue;
			}
			size_t same = 0;
			for (const device::ParamDesc& q : desc->params) {
				same += q.name != nullptr && std::strcmp(q.name, p.name) == 0 ? 1 : 0;
			}
			if (same != 1) {
				continue;
			}
			const double normal = device::toNormal(p, p.def) < 0.5 ? 0.7 : 0.3;
			const double value = hostValueOf(p, normal);
			if (value == static_cast<double>(device::coerceControl(p, static_cast<float>(p.def)))) {
				continue;
			}
			wants.push_back({ p.name, value });
		}
		for (const Want& w : wants) {
			CAPTURE(w.name);
			REQUIRE(chain->drag_control(v.plugin, 0, w.name.c_str(), &w.value, 1));
			mainThreadVisit(v.plugin);
			visit(v.plugin);
			runBlock(v);
		}
		runBlock(v);
		mainThreadVisit(v.plugin);
		visit(v.plugin);
		runBlock(v);
		for (const Want& w : wants) {
			CAPTURE(w.name);
			double knob = -1.0;
			REQUIRE(chain->knob_value(v.plugin, 0, w.name.c_str(), &knob));
			CHECK(knob == w.value);
			double base = -1.0;
			REQUIRE(chain->engine_control(v.plugin, 0, w.name.c_str(), &base, nullptr));
			CHECK(base == w.value);
			++checked;
		}
	}
	MESSAGE("controls checked: " << checked << " over " << tags.size() << " devices");
	CHECK(checked > 100);
}

TEST_CASE("the rack: a write of the value already there keeps an unmirrored host write, and tells the host nothing") {
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rackWithPinnedCutoff(v);
	const double recorded = knobOf(v.plugin, "Filter Cutoff");

	// THE HOST WRITES THE CUTOFF, and no visit mirrors it: the engine has the host's, the record its own.
	SlotEvents write;
	write.pushParam(kSlot0, 0.85);
	runBlock(v, write.input());
	const double host = hostValueOf(desc, 0.85);
	REQUIRE(host != recorded);
	REQUIRE(engineOf(v.plugin, "Filter Cutoff").base == host);
	REQUIRE(knobOf(v.plugin, "Filter Cutoff") == recorded);

	// A SETTINGS ROAD PUTS EVERY FIELD, the ones it did not change at the record's value — `setField` of the
	// value already there, with no hand on the control (`ControlGestureScope` opens none on a field it carries).
	REQUIRE(slotChainOf(v.plugin)->set_control(v.plugin, 0, "Filter Cutoff", recorded));
	SlotLog log;
	runBlock(v, nullptr, &log);
	runBlock(v);
	// THE HOST'S CUTOFF STANDS, and the host heard nothing outside a gesture.
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == host);
	CHECK(log.on(kSlot0).empty());
	// AND THE VISIT MIRRORS THE HOST'S VALUE INTO THE KNOB, rather than the other way round.
	mainThreadVisit(v.plugin);
	runBlock(v);
	CHECK(knobOf(v.plugin, "Filter Cutoff") == host);
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == host);
}

TEST_CASE("the rack: a HAND dragging to the record's value over an unmirrored host write wins") {
	// THE OTHER HALF OF THE RULE ABOVE: the same value, written with a hand on the control, is an edit.
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rackWithPinnedCutoff(v);
	const double recorded = knobOf(v.plugin, "Filter Cutoff");
	SlotEvents write;
	write.pushParam(kSlot0, 0.85);
	runBlock(v, write.input());
	REQUIRE(engineOf(v.plugin, "Filter Cutoff").base == hostValueOf(desc, 0.85));

	drag(v.plugin, "Filter Cutoff", {recorded});
	SlotLog log;
	runBlock(v, nullptr, &log);
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == recorded);
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[1].value == static_cast<double>(static_cast<float>(device::toNormal(desc, static_cast<float>(recorded)))));
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	mainThreadVisit(v.plugin);
	runBlock(v);
	CHECK(knobOf(v.plugin, "Filter Cutoff") == recorded);
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == recorded);
}

TEST_CASE("the rack: a double-click reset on a slotted control emits Begin, Value, End") {
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rackWithPinnedCutoff(v);
	REQUIRE(slotChainOf(v.plugin)->set_control(v.plugin, 0, "Filter Cutoff", std::log2(3000.0)));
	runBlock(v); // the set_control's bare Value goes out here, and is not what this case is about

	REQUIRE(slotChainOf(v.plugin)->reset_control(v.plugin, 0, "Filter Cutoff"));
	SlotLog log;
	runBlock(v, nullptr, &log);
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	const float def = device::coerceControl(desc, static_cast<float>(desc.def));
	CHECK(ev[1].value == static_cast<double>(static_cast<float>(device::toNormal(desc, def))));
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == static_cast<double>(def));
}

TEST_CASE("the rack: a control destroyed mid-drag still ends its host gesture") {
	Venue v;
	rackWithPinnedCutoff(v);
	REQUIRE(slotChainOf(v.plugin)->abandon_drag(v.plugin, 0, "Filter Cutoff", std::log2(750.0)));
	SlotLog log;
	runBlock(v, nullptr, &log);
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	// AND THE NEXT DRAG IS ITS OWN GESTURE, not one the dead control left open.
	drag(v.plugin, "Filter Cutoff", {std::log2(900.0)});
	SlotLog next;
	runBlock(v, nullptr, &next);
	const std::vector<SlotLog::Event> ev2 = next.on(kSlot0);
	REQUIRE(ev2.size() == 3);
	CHECK(ev2[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev2[2].type == CLAP_EVENT_PARAM_GESTURE_END);
}

//------------------------------------------------------------------------------------------------//
// THE INTEGRATION REVIEW'S CASES — undo as a hand, the scope's "unchanged", the remap and load       //
// windows, a publish that throws, and a slot gained mid-drag. The first is on the rack's CLAP road;  //
// the rest drive `RackBridge` and its `SlotOwner` directly, because each claim is about an instant   //
// between two calls (inside the owner's publish, between a load and the next visit) that no CLAP     //
// call can stand in.                                                                                //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the rack: undo back to a value wins over an unmirrored host write") {
	Venue v;
	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	rackWithPinnedCutoff(v);
	const clap_plugin_feathers_test_chain* chain = slotChainOf(v.plugin);
	const double r = static_cast<double>(device::coerceControl(desc, static_cast<float>(std::log2(2000.0))));
	REQUIRE(chain->set_control(v.plugin, 0, "Filter Cutoff", r));
	drag(v.plugin, "Filter Cutoff", {std::log2(5000.0)}); // the undo step: r -> 5000
	REQUIRE(chain->set_control(v.plugin, 0, "Filter Cutoff", r)); // the record back at r
	runBlock(v);
	mainThreadVisit(v.plugin);
	REQUIRE(knobOf(v.plugin, "Filter Cutoff") == r);

	SlotEvents write;
	write.pushParam(kSlot0, 0.9);
	runBlock(v, write.input()); // unmirrored
	const double host = hostValueOf(desc, 0.9);
	REQUIRE(host != r);
	REQUIRE(engineOf(v.plugin, "Filter Cutoff").base == host);

	REQUIRE(chain->undo(v.plugin)); // writes r, which the record already holds
	SlotLog log;
	runBlock(v, nullptr, &log);
	runBlock(v);
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == r);
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[1].value == static_cast<double>(static_cast<float>(device::toNormal(desc, static_cast<float>(r)))));
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	mainThreadVisit(v.plugin);
	runBlock(v);
	CHECK(knobOf(v.plugin, "Filter Cutoff") == r);
}

namespace {

/// **A HOST WHOSE ROWS STOP SHORT**, over a real rack bridge: every call goes to the bridge except the two a
/// `ControlGestureScope` reads a row by — no field present, and 0 read past it (Feathers' `fieldAt` rule).
/// `RackBridge` is final, so this wraps rather than derives.
struct ShortRowHost final : backplane::DeviceHost {
	explicit ShortRowHost(rack::RackBridge& b) : bridge(b) {}
	rack::RackBridge& bridge;
	const backplane::Axes& axes() const noexcept override { return bridge.axes(); }
	backplane::DeviceView devices() const noexcept override { return bridge.devices(); }
	float fieldAt(DeviceInstanceId, size_t) const override { return 0.0f; }
	void setField(DeviceInstanceId d, size_t c, float v) override { bridge.setField(d, c, v); }
	size_t fieldsPresentAt(DeviceInstanceId) const override { return 0; }
	double sampleRate() const noexcept override { return bridge.sampleRate(); }
	size_t readingCountAt(DeviceInstanceId d) const noexcept override { return bridge.readingCountAt(d); }
	float readingAt(DeviceInstanceId d, size_t i) const noexcept override { return bridge.readingAt(d, i); }
	float routeAmount(backplane::Source s, backplane::Destination d) const override { return bridge.routeAmount(s, d); }
	void setRouteAmount(backplane::Source s, backplane::Destination d, float a) override { bridge.setRouteAmount(s, d, a); }
	bool routedElsewhere(backplane::Destination d, backplane::Source e) const override { return bridge.routedElsewhere(d, e); }
	void beginRouteGesture() override {}
	void endRouteGesture() override {}
	void beginControlGesture(DeviceInstanceId d, size_t c) override { bridge.beginControlGesture(d, c); }
	void endControlGesture(DeviceInstanceId d, size_t c) override { bridge.endControlGesture(d, c); }
	void forEachRouteFrom(backplane::Source s, void* ctx, void (*f)(void*, backplane::Destination)) const override {
		bridge.forEachRouteFrom(s, ctx, f);
	}
	std::string destinationName(backplane::Destination d) const override { return bridge.destinationName(d); }
};

/// An outermost rack bridge that owns a pool, holding one filter — the object the standalone plugin holds.
struct OwnedRack {
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	std::unique_ptr<rack::RackBridge> bridge;
	DeviceInstanceId filter = kNoInstance;
	const device::Descriptor* desc = nullptr;
	static constexpr size_t kCutoff = static_cast<size_t>(fx::filter::Param::Cutoff);
	static constexpr size_t kRes = static_cast<size_t>(fx::filter::Param::Resonance);

	OwnedRack() {
		engine.prepare(48000.0, 4096);
		bridge = std::make_unique<rack::RackBridge>(engine, nullptr, rack::RackBridge::HostSlots::OwnPool);
		filter = bridge->addDevice(fx::filter::kTraits.tag);
		bridge->drain();
		REQUIRE(filter != kNoInstance);
		desc = bridge->descriptorOf(filter);
		REQUIRE(desc != nullptr);
		REQUIRE(std::string(desc->params[kCutoff].key) == "filter_cutoff_wide");
		REQUIRE(std::string(desc->params[kRes].key) == "filter_res");
	}
	rack::SlotOwner& slots() { return *bridge->hostSlots(); }
	/// **SLOTS 0 AND 1 TRADE TARGETS** — a remap of both, through the pool's own `restore` (the pool alone:
	/// not `SlotOwner::restore`, which is a load). A freed slot is the LAST Empty a pin takes (it keeps its
	/// note for an undo), so a free-then-pin cannot put a new control on the same slot.
	void swapSlots01() {
		hostslots::PoolSnapshot s = slots().pool().snapshot();
		REQUIRE(s.slots.size() == 2);
		std::swap(s.slots[0].target, s.slots[1].target);
		REQUIRE(slots().pool().restore(s));
	}
	hostslots::SlotIndex pin(size_t control) {
		const std::optional<hostslots::Target> t = slots().targetOf(filter, control);
		REQUIRE(t.has_value());
		return slots().pool().pin(*t).slot;
	}
};

struct Drained {
	hostslots::SlotEventKind kind;
	uint16_t slot;
	float value;
};
std::vector<Drained> drainAll(rack::SlotOwner& slots) {
	std::vector<Drained> out;
	slots.outbox().drain([&out](hostslots::SlotEventKind kind, uint16_t slot, float value) noexcept {
		out.push_back({ kind, slot, value });
		return true;
	});
	return out;
}
std::vector<Drained> on(const std::vector<Drained>& all, uint16_t slot) {
	std::vector<Drained> out;
	for (const Drained& d : all) {
		if (d.slot == slot) {
			out.push_back(d);
		}
	}
	return out;
}

} // namespace

TEST_CASE("backlog JV: every edit road that changes the rack's patch marks the host dirty once, and the same edit again marks nothing") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	const DeviceInstanceId filter = bridge.addDevice(fx::filter::kTraits.tag);
	const DeviceInstanceId env = bridge.addDevice(mod::dahdsr::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	REQUIRE(env != kNoInstance);
	v.settle();
	visit(v.plugin);

	// A VISIT WITH NO EDIT SAYS NOTHING — the zero every row below is measured from.
	const int quiet = log.dirty;
	visit(v.plugin);
	visit(v.plugin);
	REQUIRE(log.dirty == quiet);

	const float fieldTo = otherValue(bridge, filter, 0);
	const backplane::Source macro0 = backplane::Source::core(0);
	const backplane::Destination filterControl = backplane::Destination::control(filter, routableControl(bridge, filter));
	DeviceInstanceId added = kNoInstance;
	struct Road {
		const char* name;
		std::function<void()> edit;
		bool idempotent; // the same edit made again changes nothing
	};
	const std::vector<Road> roads {
		{ "a device added", [&] { added = bridge.addDevice(fx::filter::kTraits.tag); }, false },
		{ "a device added before another", [&] { (void)bridge.addDeviceBefore(filter, fx::filter::kTraits.tag); }, false },
		{ "a device moved", [&] { REQUIRE(bridge.moveDeviceTo(added, 0)); }, false },
		{ "a device removed", [&] { bridge.removeDevice(added); }, false },
		{ "a control set", [&] { bridge.setField(filter, 0, fieldTo); }, true },
		{ "a route drawn",
		  [&] {
			  bridge.setRouteAmount(macro0, filterControl, 0.5f);
			  REQUIRE(bridge.routeAmount(macro0, filterControl) == 0.5f); // drawn, not refused
		  },
		  true },
		{ "a route taken away", [&] { bridge.setRouteAmount(macro0, filterControl, 0.0f); }, true },
		{ "a macro made bipolar", [&] { bridge.setMacroBipolar(2, true); }, true },
		{ "a macro's type", [&] { bridge.setMacroType(3, device::MacroType::Latch); }, true },
		{ "a macro renamed", [&] { bridge.setMacroName(4, "Air"); }, true },
		{ "an output relabelled", [&] { bridge.setOutputLabel(env, 0, "Swell"); }, true },
		{ "a card folded", [&] { bridge.setDeviceFolded(filter, true); }, true },
		{ "the canvas moved", [&] { bridge.setCanvasView({ 123.0f, true }); }, true },
	};
	for (const Road& road : roads) {
		const std::string roadName = road.name;
		CAPTURE(roadName);
		const int before = log.dirty;
		road.edit();
		visit(v.plugin);
		CHECK(log.dirty == before + 1);
		// ONCE PER VISIT, NOT PER CALL: a second visit with nothing new says nothing.
		visit(v.plugin);
		CHECK(log.dirty == before + 1);
		if (road.idempotent) {
			road.edit(); // what it already is: not an edit
			visit(v.plugin);
			CHECK(log.dirty == before + 1);
		}
	}
	// NOT VACUOUS: the edits took — the record holds each one.
	CHECK(bridge.fieldAt(filter, 0) == fieldTo);
	CHECK(bridge.routeAmount(macro0, filterControl) == 0.0f);
	CHECK(bridge.macroBipolar(2));
	CHECK(bridge.macroType(3) == device::MacroType::Latch);
	CHECK(bridge.macroName(4) == "Air");
	CHECK(bridge.outputLabel(env, 0) == "Swell");
	CHECK(bridge.deviceFolded(filter));
	CHECK(bridge.canvasView() == rack::CanvasView { 123.0f, true });
	CHECK(bridge.positionOf(added) == rack::RackBridge::kNoSlot);
}

TEST_CASE("backlog JV: several edits in one visit are one mark, and a macro's value, which the host sent, is none") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	visit(v.plugin);
	const int before = log.dirty;
	bridge.setMacroName(0, "One");
	bridge.setMacroName(1, "Two");
	bridge.setMacroBipolar(1, true);
	visit(v.plugin);
	CHECK(log.dirty == before + 1);

	// A PARAMETER EVENT IS THE HOST'S OWN WRITE — it knows; marking it would be noise.
	OneEvent e(5, 0.75);
	v.run(&e.in);
	visit(v.plugin);
	CHECK(log.dirty == before + 1);
	CHECK(bridge.macro(5) == 0.75f); // exact in binary: the value the host sent
}

TEST_CASE("backlog JV: an edit inside a chain in the rack marks the rack's patch — the chain's body is saved in it") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	const DeviceInstanceId chain = bridge.addDevice(rack::kDeviceDescriptor.traits.tag);
	REQUIRE(chain != kNoInstance);
	v.settle();
	visit(v.plugin);
	rack::RackBridge* inner = rack::bridgeOf(bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);

	int before = log.dirty;
	const DeviceInstanceId innerFilter = inner->addDevice(fx::filter::kTraits.tag);
	REQUIRE(innerFilter != kNoInstance);
	visit(v.plugin);
	CHECK(log.dirty == before + 1);

	before = log.dirty;
	inner->setMacroName(0, "Deep");
	visit(v.plugin);
	CHECK(log.dirty == before + 1);

	// AN OUTER MACRO ROUTED AT A CONTROL INSIDE — the inner-route road, through the container's ops.
	v.settle();
	visit(v.plugin);
	before = log.dirty;
	REQUIRE(bridge.setInnerRouteAmount(backplane::Source::core(1), chain, backplane::Destination::control(innerFilter, routableControl(*inner, innerFilter)),
	                                   0.5f));
	visit(v.plugin);
	CHECK(log.dirty == before + 1);
	CHECK(bridge.innerRouteAmount(backplane::Source::core(1), chain, backplane::Destination::control(innerFilter, routableControl(*inner, innerFilter)))
	      == 0.5f);

	// AND THE SAVE CARRIES WHAT WAS MARKED: the inner device is in the rack's patch.
	const rack::RackPatch saved = bridge.patch();
	REQUIRE(saved.device.size() == 1);
	CHECK(!saved.device[0].body.empty());
}

TEST_CASE("backlog JV: a body edited through its surface marks the patch — a preset loaded into a Featherverb") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	const DeviceInstanceId verb = bridge.addDevice(fx::featherverb::kTraits.tag);
	REQUIRE(verb != kNoInstance);
	v.settle();
	visit(v.plugin);
	const device::Descriptor* desc = bridge.descriptorOf(verb);
	REQUIRE(desc != nullptr);
	REQUIRE(desc->hasBody());

	// THE INIT PRESET ON A FRESH DEVICE: every field it writes is the value already held, so the only edit a
	// host could be told of is the body's — which went in through the surface, where no host verb looks.
	const int before = log.dirty;
	const std::vector<uint8_t> init = initDevicePreset(*desc);
	REQUIRE(applyDevicePreset(bridge, *desc, verb, init, {}) == DevicePresetLoad::Loaded);
	visit(v.plugin);
	CHECK(log.dirty == before + 1);

	// A DEVICE THIS RACK DOES NOT HOLD MARKS NOTHING.
	bridge.bodyEdited(verb + 1000);
	visit(v.plugin);
	CHECK(log.dirty == before + 1);
}

TEST_CASE("backlog JV: a load is not an edit — opening a patch marks nothing, on either road") {
	std::vector<char> patch;
	{
		Venue writer;
		rack::RackBridge& b = bridgeOf(writer);
		const DeviceInstanceId f = b.addDevice(fx::filter::kTraits.tag);
		const DeviceInstanceId chain = b.addDevice(rack::kDeviceDescriptor.traits.tag);
		REQUIRE(f != kNoInstance);
		REQUIRE(chain != kNoInstance);
		b.setField(f, 0, otherValue(b, f, 0));
		b.setMacroBipolar(1, true);
		b.setMacroType(2, device::MacroType::Momentary);
		b.setMacroName(3, "Named");
		b.setDeviceFolded(f, true);
		b.setCanvasView({ 80.0f, false });
		writer.settle();
		rack::RackBridge* inner = rack::bridgeOf(b.containerSurfaceOf(chain));
		REQUIRE(inner != nullptr);
		REQUIRE(inner->addDevice(fx::filter::kTraits.tag) != kNoInstance);
		patch = saveState(writer.plugin);
	}
	HostLog log;
	Venue v(log.host());
	visit(v.plugin);
	const int before = log.dirty;
	REQUIRE(loadState(v.plugin, patch));
	// THE VISITS AFTER A LOAD — where the chain inside is built and its body loaded.
	v.settle();
	visit(v.plugin);
	visit(v.plugin);
	CHECK(log.dirty == before);
	// NOT VACUOUS: the patch loaded, with the modes that would each have marked.
	rack::RackBridge& bridge = bridgeOf(v);
	CHECK(bridge.chainLength() == 2);
	CHECK(bridge.macroBipolar(1));
	CHECK(bridge.macroType(2) == device::MacroType::Momentary);
	CHECK(bridge.macroName(3) == "Named");

	// INACTIVE, THE OTHER ROAD THROUGH `stateLoad` — the same answer.
	HostLog idle;
	{
		clap_host h = idle.host();
		const clap_plugin* p = rack::create(&h);
		REQUIRE(p != nullptr);
		REQUIRE(p->init(p));
		REQUIRE(loadState(p, patch));
		visit(p);
		CHECK(idle.dirty == 0);
		p->destroy(p);
	}
}

TEST_CASE("backlog JW: a state load tells the host the values moved — RESCAN_VALUES, once, and read back as the patch's") {
	std::vector<char> patch;
	{
		Venue writer;
		OneEvent e(2, 0.625);
		writer.run(&e.in);
		patch = saveState(writer.plugin);
	}
	HostLog log;
	Venue v(log.host());
	const auto* params = static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
	REQUIRE(params != nullptr);
	const size_t before = log.rescans.size();
	REQUIRE(loadState(v.plugin, patch));
	REQUIRE(log.rescans.size() == before + 1);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
	// NOTHING RENAMED, SO NO INFO; AND NEVER ALL, which an active plugin may not ask for.
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_INFO) == 0);
	CHECK((log.rescans.back() & CLAP_PARAM_RESCAN_ALL) == 0);
	// WHAT THE HOST READS WHEN IT RESCANS IS THE PATCH'S VALUE, at once.
	double value = -1.0;
	REQUIRE(params->get_value(v.plugin, 2, &value));
	CHECK(value == 0.625); // exact in binary

	// INACTIVE TOO — the other road through `stateLoad`.
	HostLog idle;
	clap_host h = idle.host();
	const clap_plugin* p = rack::create(&h);
	REQUIRE(p != nullptr);
	REQUIRE(p->init(p));
	REQUIRE(loadState(p, patch));
	REQUIRE(idle.rescans.size() == 1);
	CHECK((idle.rescans.back() & CLAP_PARAM_RESCAN_VALUES) != 0);
	const auto* idleParams = static_cast<const clap_plugin_params*>(p->get_extension(p, CLAP_EXT_PARAMS));
	REQUIRE(idleParams != nullptr);
	value = -1.0;
	REQUIRE(idleParams->get_value(p, 2, &value));
	CHECK(value == 0.625); // exact in binary
	p->destroy(p);
}

TEST_CASE("backlog JW: a refused load changed nothing, so it tells the host nothing — Feathers' answer") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	bridge.setMacroName(0, "Kept");
	visit(v.plugin);
	const size_t rescans = log.rescans.size();
	const int dirty = log.dirty;
	REQUIRE_FALSE(loadState(v.plugin, std::vector<char> { 'n', 'o', 't', ' ', 'a', ' ', 'r', 'a', 'c', 'k' }));
	visit(v.plugin);
	CHECK(log.rescans.size() == rescans);
	CHECK(log.dirty == dirty);
	CHECK(bridge.macroName(0) == "Kept");
}

//------------------------------------------------------------------------------------------------//
// THE TAIL — the rule (2026-09-25): a host-facing "done" is decided by the OUTPUT of the whole chain.       //
// The rack never answers SLEEP, so a CLAP host keeps calling it through a reverb's tail; and it reports //
// `clap_plugin_tail`, which clap-wrapper turns into VST3's getTailSamples, so a VST3 host does too.   //
// Modelled on test_clap_render's "a reverb's tail keeps the plugin awake".                            //
//------------------------------------------------------------------------------------------------//

namespace {

uint32_t rackTail(const clap_plugin* plugin) {
	const auto* tail = static_cast<const clap_plugin_tail*>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
	REQUIRE(tail != nullptr);
	return tail->get(plugin);
}

float peakOfBlock(const std::vector<float>& l, const std::vector<float>& r) {
	float peak = 0.0f;
	for (size_t i = 0; i < l.size(); ++i) {
		peak = std::max({ peak, std::fabs(l[i]), std::fabs(r[i]) });
	}
	return peak;
}

} // namespace

TEST_CASE("a gesture scope carrying fields while host automation is live keeps the host's value") {
	// A ROW WRITER (`ControlGestureScope`) PUTS THE CUTOFF AT THE VALUE THE RECORD HOLDS — its declared
	// default, PAST `fieldsPresent` on a fresh device — and changes the resonance, while the host's write to
	// the cutoff sits unmirrored on the bus. The cutoff is carried: no gesture, no seed, nothing to the host.
	// A HOST WHOSE ROW STOPS SHORT OF THE CUTOFF, as a scope sees it — `fieldsPresentAt` 0 and the raw field
	// 0 past it, Feathers' `fieldAt` rule. Neither bridge's own roads produce one today (the rack's applier
	// fills every declared field), so the host is made to answer it; the slot owner's record is untouched.
	OwnedRack r;
	ShortRowHost shortRows { *r.bridge };
	REQUIRE(r.pin(OwnedRack::kCutoff) == 0u);
	REQUIRE(r.pin(OwnedRack::kRes) == 1u);
	(void)r.bridge->serviceHostSlots();
	(void)drainAll(r.slots());
	REQUIRE(shortRows.fieldsPresentAt(r.filter) <= OwnedRack::kCutoff); // NOT VACUOUS: past the row
	const float held = static_cast<float>(r.desc->params[OwnedRack::kCutoff].def);
	REQUIRE(r.slots().recordHolds(r.filter, OwnedRack::kCutoff, held)); // the owner's record: the default

	REQUIRE(r.slots().onHostValue(0, 0.9));
	const float host = r.slots().bus().load(0).value;
	REQUIRE(host != held);
	const float res = static_cast<float>(r.desc->params[OwnedRack::kRes].def) == 0.5f ? 0.25f : 0.5f;
	{
		backplane::ControlGestureScope scope(shortRows, r.filter);
		scope.setField(OwnedRack::kCutoff, held);
		scope.setField(OwnedRack::kRes, res);
	}
	CHECK(r.slots().bus().load(0).value == host);
	const std::vector<Drained> ev = drainAll(r.slots());
	CHECK(on(ev, 0).empty());
	const std::vector<Drained> resEv = on(ev, 1);
	REQUIRE(resEv.size() == 3);
	CHECK(resEv[0].kind == hostslots::SlotEventKind::Begin);
	CHECK(resEv[1].kind == hostslots::SlotEventKind::Value);
	CHECK(resEv[2].kind == hostslots::SlotEventKind::End);
	CHECK(r.slots().bus().load(1).value == device::coerceControl(r.desc->params[OwnedRack::kRes], res));
	// AND THE VISIT MIRRORS THE HOST'S CUTOFF INTO THE RECORD.
	(void)r.bridge->serviceHostSlots();
	CHECK(r.slots().recordHolds(r.filter, OwnedRack::kCutoff,
	                            device::coerceControl(r.desc->params[OwnedRack::kCutoff], host)));
}

TEST_CASE("a gesture scope compares the COERCED value: an out-of-range write of the held value is carried") {
	OwnedRack r;
	const device::ParamDesc& cut = r.desc->params[OwnedRack::kCutoff];
	r.bridge->setField(r.filter, OwnedRack::kCutoff, static_cast<float>(cut.max));
	REQUIRE(r.pin(OwnedRack::kCutoff) == 0u);
	(void)r.bridge->serviceHostSlots();
	(void)drainAll(r.slots());
	REQUIRE(r.slots().onHostValue(0, 0.2));
	const float host = r.slots().bus().load(0).value;
	REQUIRE(host != static_cast<float>(cut.max));
	{
		backplane::ControlGestureScope scope(*r.bridge, r.filter);
		scope.setField(OwnedRack::kCutoff, static_cast<float>(cut.max) + 1000.0f); // coerces to the max held
	}
	CHECK(r.slots().bus().load(0).value == host);
	CHECK(drainAll(r.slots()).empty());
}

TEST_CASE("the remap window: a host write while the declaration is withheld is dropped, published after the publish") {
	OwnedRack r;
	REQUIRE(r.pin(OwnedRack::kCutoff) == 0u);
	REQUIRE(r.pin(OwnedRack::kRes) == 1u);
	(void)r.bridge->serviceHostSlots();
	REQUIRE(r.slots().descOf(0) == &r.desc->params[OwnedRack::kCutoff]);
	r.swapSlots01(); // slot 0 now names the resonance
	(void)r.slots().reconcileIfMoved();
	const uint32_t genBefore = r.slots().bus().load(0).gen;
	const device::ParamDesc* atPublish = &r.desc->params[OwnedRack::kCutoff];
	bool accepted = true;
	uint32_t genInWindow = 0;
	int published = 0;
	const rack::SlotOwner::Visit visit = r.slots().service([&] {
		++published;
		atPublish = r.slots().descOf(0);
		accepted = r.slots().onHostValue(0, 0.7);
		genInWindow = r.slots().bus().load(0).gen;
	});
	CHECK(visit.recordChanged);
	CHECK(published == 1);
	CHECK(atPublish == nullptr); // withheld until the record binding the new target is out
	CHECK_FALSE(accepted);       // so the write in the window is dropped, not scaled by either range
	CHECK(genInWindow == r.slots().bus().load(0).gen);
	CHECK(r.slots().bus().load(0).gen != genBefore); // the rebind's own seed moved it; the write did not
	CHECK(r.slots().descOf(0) == &r.desc->params[OwnedRack::kRes]); // and handed over after the publish
	CHECK(r.slots().onHostValue(0, 0.7));
}

TEST_CASE("a publish that throws withholds the declarations and is retried at the next visit") {
	OwnedRack r;
	REQUIRE(r.pin(OwnedRack::kCutoff) == 0u);
	(void)r.slots().reconcileIfMoved();
	const rack::SlotOwner::Visit first = r.slots().service([] { throw std::runtime_error("publish failed"); });
	CHECK(first.recordChanged);
	CHECK(r.slots().descOf(0) == nullptr);
	int calls = 0;
	const rack::SlotOwner::Visit second = r.slots().service([&] { ++calls; });
	CHECK(second.recordChanged); // owed, though nothing else changed
	CHECK(calls == 1);
	CHECK(r.slots().descOf(0) == &r.desc->params[OwnedRack::kCutoff]);
	int later = 0;
	(void)r.slots().service([&] { ++later; });
	CHECK(later == 0); // paid: nothing owed now
}

TEST_CASE("a control bound mid-drag opens its Begin at its first Value there") {
	OwnedRack r;
	ui::DeviceControl knob(*r.bridge, r.filter, *r.desc, OwnedRack::kCutoff);
	// SINCE T9 THE HAND'S OWN TOUCH gives the control slot 0 at this Begin (Recent); it is not BOUND until the
	// visit, so nothing is told the host yet.
	knob.beginGesture();
	CHECK(drainAll(r.slots()).empty());
	REQUIRE(r.pin(OwnedRack::kCutoff) == 0u); // the slot the touch took, now pinned
	(void)r.bridge->serviceHostSlots();
	knob.setValue(std::log2(700.0));
	knob.endGesture();
	// THE BINDING VISIT reports the takeover (a plain Value) and opens the hand's gesture (Begin, and its value
	// again); the drag's own Value and End follow inside it.
	const std::vector<Drained> ev = drainAll(r.slots());
	REQUIRE(ev.size() == 5);
	CHECK(ev[0].kind == hostslots::SlotEventKind::Value);
	CHECK(ev[1].kind == hostslots::SlotEventKind::Begin);
	CHECK(ev[2].kind == hostslots::SlotEventKind::Value);
	CHECK(ev[3].kind == hostslots::SlotEventKind::Value);
	CHECK(ev[4].kind == hostslots::SlotEventKind::End);
	for (const Drained& d : ev) {
		CHECK(d.slot == 0u);
	}
}

TEST_CASE("a control remapped mid-drag ends its old slot's gesture and opens its new slot's at the first Value") {
	OwnedRack r;
	ui::DeviceControl knob(*r.bridge, r.filter, *r.desc, OwnedRack::kCutoff);
	REQUIRE(r.pin(OwnedRack::kCutoff) == 0u);
	REQUIRE(r.pin(OwnedRack::kRes) == 1u);
	(void)r.bridge->serviceHostSlots();
	(void)drainAll(r.slots());
	knob.beginGesture(); // Begin on slot 0
	r.swapSlots01(); // THE CUTOFF MOVES TO SLOT 1 AND THE RESONANCE TAKES SLOT 0, mid-drag
	(void)r.bridge->serviceHostSlots();
	knob.setValue(std::log2(700.0));
	knob.endGesture();
	const std::vector<Drained> ev = drainAll(r.slots());
	REQUIRE(ev.size() == 5);
	CHECK((ev[0].kind == hostslots::SlotEventKind::Begin && ev[0].slot == 0u));
	CHECK((ev[1].kind == hostslots::SlotEventKind::End && ev[1].slot == 0u));
	CHECK((ev[2].kind == hostslots::SlotEventKind::Begin && ev[2].slot == 1u));
	CHECK((ev[3].kind == hostslots::SlotEventKind::Value && ev[3].slot == 1u));
	CHECK((ev[4].kind == hostslots::SlotEventKind::End && ev[4].slot == 1u));
}

TEST_CASE("the load window: a host write before the rebind is dropped and anchors nothing in the restored pool") {
	OwnedRack r;
	REQUIRE(r.pin(OwnedRack::kCutoff) == 0u);
	(void)r.bridge->serviceHostSlots();
	const rack::RackPatch patch = r.bridge->patch();
	REQUIRE(r.slots().onHostValue(0, 0.9)); // the OLD pool's write, its bit still untaken
	(void)r.bridge->loadPatch(patch);
	REQUIRE(r.slots().pool().state(0) == hostslots::SlotState::Pinned);
	CHECK(r.slots().descOf(0) == nullptr);
	CHECK_FALSE(r.slots().onHostValue(0, 0.3)); // in the window: dropped
	(void)r.bridge->serviceHostSlots();
	CHECK(r.slots().pool().state(0) == hostslots::SlotState::Pinned); // not anchored by the old write
	CHECK(r.slots().descOf(0) == &r.desc->params[OwnedRack::kCutoff]);
}

TEST_CASE("the rack's tail: a reverb keeps ringing after the input stops, and the rack never sleeps while its output sounds") {
	Venue v(256);
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	const uint32_t slot = chain->add_device(v.plugin, fx::featherverb::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(chain->set_control(v.plugin, slot, "Featherverb Mix", 1.0));
	REQUIRE(chain->set_control(v.plugin, slot, "Featherverb Decay", 0.5));
	v.settle();

	// A HALF SECOND OF TONE INTO THE REVERB, then the input stops dead.
	v.tone();
	for (int b = 0; b < 48000 / 2 / 256; ++b) {
		REQUIRE(v.run() == CLAP_PROCESS_CONTINUE);
	}
	std::fill(v.inL.begin(), v.inL.end(), 0.0f);
	std::fill(v.inR.begin(), v.inR.end(), 0.0f);

	// THE PROMISE, EVERY BLOCK OF THREE SECONDS OF SILENT INPUT: never SLEEP over a block that sounds.
	int sounding = 0;
	int sleptWhileSounding = 0;
	float firstPeak = -1.0f;
	for (int b = 0; b < 3 * 48000 / 256; ++b) {
		const clap_process_status status = v.run();
		const float peak = peakOfBlock(v.l, v.r);
		if (firstPeak < 0.0f) {
			firstPeak = peak;
		}
		sounding += peak > 0.0f ? 1 : 0;
		sleptWhileSounding += (status == CLAP_PROCESS_SLEEP && peak > 0.0f) ? 1 : 0;
		// THE RACK NEVER SLEEPS AT ALL: "done" is not decided here, the host keeps calling.
		CHECK(status == CLAP_PROCESS_CONTINUE);
	}
	CAPTURE(firstPeak);
	CAPTURE(sounding);
	CHECK(sleptWhileSounding == 0);
	// NOT VACUOUS: the tail did ring on past the input — over a second of blocks carried sound.
	CHECK(firstPeak > 1e-3f);
	CHECK(sounding > 48000 / 256);
}

TEST_CASE("the rack's tail extension: infinite while an audio device is in the rack, zero without one, and the host is told when it moves") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	CHECK(rackTail(v.plugin) == 0); // an empty rack is a wire
	const int changedBefore = log.tailChanged;

	// A MODULATOR ALONE MAKES NO SOUND AFTER ITS INPUT: still zero, and nothing to tell.
	const DeviceInstanceId env = bridge.addDevice(mod::dahdsr::kTraits.tag);
	REQUIRE(env != kNoInstance);
	visit(v.plugin);
	CHECK(rackTail(v.plugin) == 0);
	v.run(); // `changed` is [audio-thread]: the next block tells it
	CHECK(log.tailChanged == changedBefore);

	// A REVERB: infinite — SLEEP-by-output decides when it has finished, not a number guessed here.
	const DeviceInstanceId verb = bridge.addDevice(fx::featherverb::kTraits.tag);
	REQUIRE(verb != kNoInstance);
	visit(v.plugin);
	CHECK(rackTail(v.plugin) == UINT32_MAX);
	v.run(); // `changed` is [audio-thread]: the next block tells it
	CHECK(log.tailChanged == changedBefore + 1);
	visit(v.plugin); // unchanged: told once
	v.run(); // `changed` is [audio-thread]: the next block tells it
	CHECK(log.tailChanged == changedBefore + 1);

	// AND BACK: the last audio device removed.
	bridge.removeDevice(verb);
	visit(v.plugin);
	CHECK(rackTail(v.plugin) == 0);
	v.run(); // `changed` is [audio-thread]: the next block tells it
	CHECK(log.tailChanged == changedBefore + 2);

	// A LOAD SETS IT TOO, with no visit: the patch's chain, not the one it replaced.
	std::vector<char> patch;
	{
		Venue writer;
		REQUIRE(bridgeOf(writer).addDevice(fx::filter::kTraits.tag) != kNoInstance);
		patch = saveState(writer.plugin);
	}
	REQUIRE(loadState(v.plugin, patch));
	CHECK(rackTail(v.plugin) == UINT32_MAX);
	v.run(); // `changed` is [audio-thread]: the next block tells it
	CHECK(log.tailChanged == changedBefore + 3);
}

TEST_CASE("rack plugin: Resonate ringing after its input stops keeps the rack awake — SLEEP is never promised while it sounds") {
	// THE RACK PLUGIN NEVER RETURNS SLEEP (rack_clap.cpp answers CONTINUE on every road) and reports an
	// infinite clap_plugin_tail while an audio device is in it, so this is satisfied by construction. It is
	// here as the GUARD the plan's TAILS addendum asks for, modelled on test_clap_render's "a reverb's tail
	// keeps the plugin awake": the day the rack learns to sleep, it must learn it from its OUTPUT, and
	// Resonate is the device whose output outlives its input by design.
	Venue v;
	const auto* t = v.test();
	REQUIRE(t != nullptr);
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(t->add_device(v.plugin, fx::resonate::kTraits.tag) != 0);
	visit(v.plugin); // the tail is decided on the main thread, at a visit
	CHECK(rackTail(v.plugin) == UINT32_MAX); // an audio device: the host is told the tail is open-ended
	// Bow 0: the string moves only when the input plays it, so this is a tail that ENDS. The Drone (on by
	// default) is the head it is heard through.
	REQUIRE(chain->set_control(v.plugin, 0, "Resonate Bow", 0.0));
	v.tone();
	REQUIRE(v.run() == CLAP_PROCESS_CONTINUE);
	std::fill(v.inL.begin(), v.inL.end(), 0.0f);
	std::fill(v.inR.begin(), v.inR.end(), 0.0f);

	int soundingBlocks = 0;
	int sleptWhileSounding = 0;
	bool reachedSilence = false;
	// TWO MINUTES: the drone rings for its Decay, 12 s by default since 2026-09-28 (times its string's scale).
	for (int b = 0; b < 120 * 48000 / 512; ++b) {
		const clap_process_status status = v.run();
		float peak = 0.0f;
		for (size_t i = 0; i < v.l.size(); ++i) {
			peak = std::max({ peak, std::fabs(v.l[i]), std::fabs(v.r[i]) });
		}
		if (peak > 0.0f) {
			++soundingBlocks;
			sleptWhileSounding += (status == CLAP_PROCESS_SLEEP) ? 1 : 0;
		}
		else {
			reachedSilence = true;
		}
	}
	CAPTURE(soundingBlocks);
	CHECK(soundingBlocks > 48000 / 512 / 5); // it rang past the input for well over 200 ms
	CHECK(sleptWhileSounding == 0);
	CHECK(reachedSilence); // and the tail did end, at exact zero
}

//------------------------------------------------------------------------------------------------//
// HOST SLOTS x JV — the pool is saved with the rack's patch and a slot's state is no parameter, so a //
// verb that changes it, and a slot the host's write NEWLY anchors, must mark the host dirty; the     //
// mirror of the host's value into the record must not (the host owns its automation).                //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the rack: Expose and Unpin mark the host dirty; a verb that changes nothing marks nothing") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	const DeviceInstanceId filter = bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	v.settle();
	visit(v.plugin);
	const device::Descriptor* desc = bridge.descriptorOf(filter);
	REQUIRE(desc != nullptr);
	ui::DeviceControl cutoff(bridge, filter, *desc, static_cast<size_t>(fx::filter::Param::Cutoff));
	const int quiet = log.dirty;
	visit(v.plugin);
	REQUIRE(log.dirty == quiet);

	REQUIRE(cutoff.exposeToHost(nullptr));
	visit(v.plugin);
	CHECK(log.dirty == quiet + 1);
	CHECK_FALSE(cutoff.exposeToHost(nullptr)); // pinned already: nothing moved
	visit(v.plugin);
	CHECK(log.dirty == quiet + 1);

	REQUIRE(cutoff.unpinFromHost());
	visit(v.plugin);
	CHECK(log.dirty == quiet + 2);
	CHECK_FALSE(cutoff.unpinFromHost());
	visit(v.plugin);
	CHECK(log.dirty == quiet + 2);
}

TEST_CASE("the rack: a slot the host newly anchors marks the host dirty; mirroring its later writes does not") {
	HostLog log;
	Venue v(log.host());
	rack::RackBridge& bridge = bridgeOf(v);
	const DeviceInstanceId filter = bridge.addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	v.settle();
	const device::Descriptor* desc = bridge.descriptorOf(filter);
	REQUIRE(desc != nullptr);
	ui::DeviceControl cutoff(bridge, filter, *desc, static_cast<size_t>(fx::filter::Param::Cutoff));
	REQUIRE(cutoff.exposeToHost(nullptr));
	visit(v.plugin); // bound; the pin's own mark taken
	runBlock(v);
	const int quiet = log.dirty;

	SlotEvents first;
	first.pushParam(kSlot0, 0.3);
	runBlock(v, first.input());
	visit(v.plugin);
	REQUIRE(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Anchored));
	CHECK(log.dirty == quiet + 1);

	// ALREADY ANCHORED: the value is mirrored into the record, and nothing saved about the pool changed.
	SlotEvents second;
	second.pushParam(kSlot0, 0.6);
	runBlock(v, second.input());
	visit(v.plugin);
	CHECK(knobOf(v.plugin, "Filter Cutoff") == hostValueOf(filterControl("filter_cutoff_wide"), 0.6)); // mirrored
	CHECK(log.dirty == quiet + 1);
	// (Releasing the anchor was a menu verb that marked dirty too; it was dropped on 2026-09-26 — no UI offers
	// it, because the host's lanes stay on the id whatever the plugin does.)
}

TEST_CASE("the rack: activate binds a slot pinned since the last visit, so the first block's host write reaches it") {
	// JM's activate drains the record in place of the first block (the latency is the record's). A pin made
	// since the last main-thread visit must be bound in that same record, or the first host write on the
	// slot is dropped (no declaration yet) — the restart a latency change asks for is exactly such a road.
	Venue v;
	const clap_plugin_feathers_test_chain* chain = slotChainOf(v.plugin);
	REQUIRE(chain->add_device(v.plugin, fx::filter::kTraits.tag) == 0u);
	REQUIRE(chain->pin_slot(v.plugin, 0, "Filter Cutoff") == 0u); // no visit follows
	v.plugin->stop_processing(v.plugin);
	v.plugin->deactivate(v.plugin);
	REQUIRE(v.plugin->activate(v.plugin, 48000.0, 1, static_cast<uint32_t>(v.l.size())));
	REQUIRE(v.plugin->start_processing(v.plugin));

	SlotEvents write;
	write.pushParam(kSlot0, 0.3);
	runBlock(v, write.input());
	CHECK(engineOf(v.plugin, "Filter Cutoff").base == hostValueOf(filterControl("filter_cutoff_wide"), 0.3));
}

TEST_CASE("backlog KT: in the standalone rack a Filter cuts and a HARM adds its sub at the played note, Poly off and on, alone and beside each other") {
	// The rack half of the KT hunt: it has no voices, so Poly is a setting both devices carry and both run on the
	// bus — the sub at the note's half and the 400 Hz lowpass's cut at 1540 Hz are the claims, against the device off.
	constexpr int kBlocks = 64;
	constexpr int N = 512 * kBlocks;
	const feathers::test::Window win(N);
	// order: 0 = harm alone, 1 = filter alone, 2 = filter then harm, 3 = harm then filter
	const auto run = [&](int order, double poly, bool enabled) {
		Venue v;
		const auto* t = v.test();
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
		std::vector<const char*> tags;
		if (order == 0) tags = {fx::harm::kTraits.tag};
		if (order == 1) tags = {fx::filter::kTraits.tag};
		if (order == 2) tags = {fx::filter::kTraits.tag, fx::harm::kTraits.tag};
		if (order == 3) tags = {fx::harm::kTraits.tag, fx::filter::kTraits.tag};
		for (const char* tag : tags) {
			REQUIRE(t->add_device(v.plugin, tag) != 0);
		}
		for (uint32_t p = 0; p < tags.size(); ++p) {
			if (std::string(tags[p]) == fx::harm::kTraits.tag) {
				REQUIRE(chain->set_control(v.plugin, p, "Harm Enabled", enabled ? 1.0 : 0.0));
				REQUIRE(chain->set_control(v.plugin, p, "Harm Harmonic", 4.0));
				REQUIRE(chain->set_control(v.plugin, p, "Harm Level", 0.5));
				REQUIRE(chain->set_control(v.plugin, p, "Harm Poly", poly));
			}
			else {
				REQUIRE(chain->set_control(v.plugin, p, "Filter Enabled", enabled ? 1.0 : 0.0));
				REQUIRE(chain->set_control(v.plugin, p, "Filter Cutoff", std::log2(400.0)));
				REQUIRE(chain->set_control(v.plugin, p, "Filter Res", 0.0));
				REQUIRE(chain->set_control(v.plugin, p, "Filter Poly", poly));
			}
		}
		v.run();
		NoteEvent on(CLAP_EVENT_NOTE_ON);
		std::vector<float> L;
		uint32_t at = 0;
		for (int b = 0; b < kBlocks; ++b) {
			for (size_t i = 0; i < v.inL.size(); ++i, ++at) {
				const float s = 0.3f * std::sin(6.2831853f * 220.0f * at / 48000.0f) + 0.3f * std::sin(6.2831853f * 1540.0f * at / 48000.0f);
				v.inL[i] = v.inR[i] = s;
			}
			v.run(b == 0 ? &on.in : nullptr);
			L.insert(L.end(), v.l.begin(), v.l.end());
		}
		return L;
	};
	for (int order = 0; order < 4; ++order) {
		for (double poly : {0.0, 1.0}) {
			const auto a = run(order, poly, true);
			const auto b = run(order, poly, false);
			const auto m = [&](const std::vector<float>& x, double hz) { return feathers::test::magnitudeAt(x, win, 48000.0, hz); };
			CAPTURE(order);
			CAPTURE(poly);
			const bool hasHarm = order != 1;
			const bool hasFilter = order != 0;
			const double cut = 20.0 * std::log10(m(a, 1540.0) / m(b, 1540.0));
			CAPTURE(cut);
			CHECK(m(b, 130.81) < 1e-3);
			if (hasHarm) {
				CHECK(m(a, 130.81) > 0.1); // the sub of key 60, at half its 261.6 Hz
			}
			else {
				CHECK(m(a, 130.81) < 1e-3);
			}
			if (hasFilter) {
				CHECK(cut < -20.0);
			}
			else {
				CHECK(std::abs(cut) < 0.5);
			}
		}
	}
}

TEST_CASE("backlog KS: closing the rack's editor ends assign mode — the editor is kept across the close, so its arm "
          "would otherwise reopen with it") {
	// The editor came back frozen after a close with macro assign mode on. The rack's
	// ModState lives in RackGui, which the plugin keeps for its whole life, like the window.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui.create(api, false));
	ui::ModState& mod = gui.modForTest();
	mod.focus(backplane::Source::core(0)); // Macro 1, the rack's own source
	mod.arm(true);
	REQUIRE(mod.armed());
	gui.destroy();
	CHECK_FALSE(mod.armed());
	REQUIRE(gui.create(api, false));
	CHECK_FALSE(mod.armed());
	gui.destroy();
}

TEST_CASE("backlog KS: closing the rack's editor closes a preset prompt open inside it") {
	// The rack's browsers live on device panels (Featherverb, SKLITTER); any of them in the rack editor's
	// tree is closed with it, so its KeyboardClaim does not outlive the editor. A browser seated under the
	// editor's window stands in for one.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	REQUIRE(gui.create(api, false));
	REQUIRE(gui.editorForTest() != nullptr);
	visage::Frame* top = gui.editorForTest()->parent(); // the window the editor is seated in
	REQUIRE(top != nullptr);
	ui::PresetBrowser browser(std::vector<std::string> { "Init" }, std::vector<ui::PresetData> { ui::PresetData { { 1 } } },
	                          "KS", ui::PresetBrowser::kPanelStyle);
	browser.setBounds(0.0f, 0.0f, browser.totalWidth(), ui::PresetBrowser::kHeight);
	top->addChild(&browser);
	browser.chooseFromMenu(ui::PresetBrowser::kMenuSave);
	REQUIRE(browser.prompt() != ui::PresetBrowser::Prompt::Closed);
	gui.destroy();
	CHECK(browser.prompt() == ui::PresetBrowser::Prompt::Closed);
	top->removeChild(&browser);
}

TEST_CASE("a rack state load refused part way changes nothing, and the event log names the refusal") {
	// THE RACK'S LOAD WAS ALREADY PARSE-THEN-APPLY (`stateLoad`: "nothing is changed by a stream that does not
	// parse whole"); this pins it by VALUE, and pins the log line that records it (plugin/event_log.h). The log
	// goes to a scratch file and back to `off` — never the user's.
	const std::string logPath = (std::filesystem::temp_directory_path()
	                             / ("feathers-rack-events-" + std::to_string(::getpid()) + ".log"))
	                                .string();
	std::filesystem::remove(logPath);
	::setenv("FEATHERS_EVENT_LOG", logPath.c_str(), 1);
	{
		Venue v;
		const auto* state = static_cast<const clap_plugin_state*>(v.plugin->get_extension(v.plugin, CLAP_EXT_STATE));
		const auto* params = static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
		REQUIRE(state != nullptr);
		REQUIRE(params != nullptr);
		{
			OneEvent e(0, 0.75);
			v.run(&e.in);
		}
		std::vector<char> saved;
		clap_ostream out {};
		out.ctx = &saved;
		out.write = [](const clap_ostream* s, const void* buf, uint64_t n) -> int64_t {
			auto* bytes = static_cast<std::vector<char>*>(s->ctx);
			bytes->insert(bytes->end(), static_cast<const char*>(buf), static_cast<const char*>(buf) + n);
			return static_cast<int64_t>(n);
		};
		REQUIRE(state->save(v.plugin, &out));
		REQUIRE(saved.size() > 40);
		// MACRO 0 MOVES, then a HALF of that patch (at 0.75) is loaded: refused, and macro 0 stays where it is.
		{
			OneEvent e(0, 0.25);
			v.run(&e.in);
		}
		const size_t savedBytes = saved.size();
		saved.resize(savedBytes / 2);
		ReadStream half(saved);
		CHECK_FALSE(state->load(v.plugin, &half.in));
		double macro0 = -1.0;
		REQUIRE(params->get_value(v.plugin, 0, &macro0));
		CHECK(macro0 == doctest::Approx(0.25));

		std::ifstream in(logPath);
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK(text.find("com.owlet.feathers-chain") != std::string::npos);
		CHECK(text.find(" create\n") != std::string::npos);
		CHECK(text.find(" state-save ok bytes=" + std::to_string(savedBytes) + "\n") != std::string::npos);
		CHECK(text.find(" state-load FAILED bytes=" + std::to_string(savedBytes / 2) + " not-loaded=1")
		      != std::string::npos);
		CHECK(text.find("nothing changed") != std::string::npos);
	}
	::setenv("FEATHERS_EVENT_LOG", "off", 1);
	std::filesystem::remove(logPath);
}

TEST_CASE("the rack's state ends in its end marker: cut anywhere or flipped it is refused with nothing changed, and an unmarked older state still loads") {
	// THE SAME END MARKER AS FEATHERS' (`state/state_trailer.h`), on the rack's own state. The
	// rack's patch is a JSON document, so a state saved before the marker — which has none — is still read: its
	// closing brace is its end, and a cut one fails to parse.
	const std::string logPath = (std::filesystem::temp_directory_path()
	                             / ("feathers-rack-marker-" + std::to_string(::getpid()) + ".log"))
	                                .string();
	std::filesystem::remove(logPath);
	::setenv("FEATHERS_EVENT_LOG", logPath.c_str(), 1);
	{
		Venue v;
		const auto* state = static_cast<const clap_plugin_state*>(v.plugin->get_extension(v.plugin, CLAP_EXT_STATE));
		const auto* params = static_cast<const clap_plugin_params*>(v.plugin->get_extension(v.plugin, CLAP_EXT_PARAMS));
		REQUIRE(state != nullptr);
		REQUIRE(params != nullptr);
		const auto saveNow = [&] {
			std::vector<char> bytes;
			clap_ostream out {};
			out.ctx = &bytes;
			out.write = [](const clap_ostream* s, const void* buf, uint64_t n) -> int64_t {
				auto* b = static_cast<std::vector<char>*>(s->ctx);
				b->insert(b->end(), static_cast<const char*>(buf), static_cast<const char*>(buf) + n);
				return static_cast<int64_t>(n);
			};
			REQUIRE(state->save(v.plugin, &out));
			return bytes;
		};
		const auto macro0 = [&] {
			double value = -1.0;
			REQUIRE(params->get_value(v.plugin, 0, &value));
			return value;
		};
		{
			OneEvent e(0, 0.75);
			v.run(&e.in);
		}
		const std::vector<char> incoming = saveNow();
		const auto asBytes = [](const std::vector<char>& c) {
			return std::vector<uint8_t>(c.begin(), c.end());
		};
		size_t payload = 0;
		REQUIRE(feathers::state::checkTrailer(asBytes(incoming), &payload) == feathers::state::TrailerCheck::Ok);
		REQUIRE(payload == incoming.size() - feathers::state::kTrailerBytes);
		{
			OneEvent e(0, 0.25);
			v.run(&e.in);
		}
		const std::vector<char> live = saveNow();

		// CUT: through the document, inside the marker, one byte short. (A cut that keeps the whole document and
		// loses only the marker is read as an unmarked state — which it then is, whole: nothing is lost.)
		for (const size_t cut : { payload / 7, payload / 2, payload - 20, payload + 4, incoming.size() - 1 }) {
			CAPTURE(cut);
			ReadStream s(std::vector<char>(incoming.begin(), incoming.begin() + static_cast<std::ptrdiff_t>(cut)));
			CHECK_FALSE(state->load(v.plugin, &s.in));
			CHECK(macro0() == doctest::Approx(0.25));
			CHECK(saveNow() == live);
		}
		// FLIPPED: one bit in the document.
		{
			std::vector<char> flipped = incoming;
			flipped[payload / 2] = static_cast<char>(flipped[payload / 2] ^ 0x04);
			ReadStream s(flipped);
			CHECK_FALSE(state->load(v.plugin, &s.in));
			CHECK(macro0() == doctest::Approx(0.25));
			CHECK(saveNow() == live);
		}
		// AN UNMARKED STATE — what every rack build before the marker saved — loads, and is the patch by value.
		{
			ReadStream s(std::vector<char>(incoming.begin(), incoming.begin() + static_cast<std::ptrdiff_t>(payload)));
			CHECK(state->load(v.plugin, &s.in));
			CHECK(macro0() == doctest::Approx(0.75));
			CHECK(saveNow() == incoming); // and its resave carries the marker
		}

		std::ifstream in(logPath);
		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		CHECK(text.find("state-load FAILED stage=end-marker bytes=" + std::to_string(incoming.size()) + " why=checksum")
		      != std::string::npos);
		CHECK(text.find("end-marker=none (legacy)") != std::string::npos);
	}
	::setenv("FEATHERS_EVENT_LOG", "off", 1);
	std::filesystem::remove(logPath);
}

//------------------------------------------------------------------------------------------------//
// MONOPHONIC KEY TRACKING ON THE BUS, AND THE NOTE ROAD'S GATE (2026-10-04).                       //
//------------------------------------------------------------------------------------------------//
//
// The rack is an audio EFFECT: one summed stream and the host's notes, no voices. A device's Keytrack
// follows ONE note there: the collapsed note (the last key still held) by default, or — for a device AFTER a
// Tracker in the chain — the nearest upstream Tracker's chosen note. Every claim below
// is about a VALUE read off the device's own state through the real CLAP entry, never about a rendered buffer.

namespace {

/// A BLOCK'S WORTH OF NOTE EVENTS — ons, offs, chokes and expressions, each at its own sample.
struct NoteStream {
	struct Ev {
		clap_event_note note {};
		clap_event_note_expression expression {};
		bool isExpression = false;
	};
	std::vector<Ev> events;
	clap_input_events in {};

	NoteStream() {
		in.ctx = this;
		in.size = [](const clap_input_events* list) -> uint32_t {
			return static_cast<uint32_t>(static_cast<const NoteStream*>(list->ctx)->events.size());
		};
		in.get = [](const clap_input_events* list, uint32_t i) -> const clap_event_header* {
			const Ev& e = static_cast<const NoteStream*>(list->ctx)->events[i];
			return e.isExpression ? &e.expression.header : &e.note.header;
		};
	}
	NoteStream(const NoteStream&) = delete;
	NoteStream& operator=(const NoteStream&) = delete;

	NoteStream& note(uint16_t type, int16_t key, uint32_t time = 0, int32_t noteId = -1) {
		Ev e;
		e.note.header.size = sizeof(e.note);
		e.note.header.time = time;
		e.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.note.header.type = type;
		e.note.note_id = noteId;
		e.note.port_index = 0;
		e.note.channel = 0;
		e.note.key = key;
		e.note.velocity = 1.0;
		events.push_back(e);
		return *this;
	}
	NoteStream& on(int16_t key, uint32_t time = 0) { return note(CLAP_EVENT_NOTE_ON, key, time); }
	NoteStream& off(int16_t key, uint32_t time = 0) { return note(CLAP_EVENT_NOTE_OFF, key, time); }
	NoteStream& expression(int16_t key, clap_note_expression id, double value, uint32_t time = 0) {
		Ev e;
		e.isExpression = true;
		e.expression.header.size = sizeof(e.expression);
		e.expression.header.time = time;
		e.expression.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.expression.header.type = CLAP_EVENT_NOTE_EXPRESSION;
		e.expression.expression_id = id;
		e.expression.note_id = -1;
		e.expression.port_index = 0;
		e.expression.channel = 0;
		e.expression.key = key;
		e.expression.value = value;
		events.push_back(e);
		return *this;
	}
};

/// ONE BLOCK CARRYING `events`.
void play(Venue& v, NoteStream& events) { v.run(&events.in); }

/// THE DEVICE'S OWN STORAGE IN THE ENGINE, by the id the test road answered — what its test seams read.
const void* storageOf(const Venue& v, uint32_t id) {
	const rack::RackEngine& engine = bridgeOf(v).engineForTest();
	const size_t slot = engine.slotOf(id);
	REQUIRE(slot != rack::RackEngine::kNoSlot);
	const void* storage = engine.storageForTest(slot);
	REQUIRE(storage != nullptr);
	return storage;
}

/// A CONTROL BY ITS DECLARED NAME on the device at `position`, through the chain venue.
void setByName(Venue& v, uint32_t position, const char* name, double value) {
	const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
	    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
	REQUIRE(chain != nullptr);
	REQUIRE(chain->set_control(v.plugin, position, name, value));
}

/// THE RULE'S ANSWER, RESTATED: which key each rule picks from what is held, in arrival order. An independent
/// restatement of the intended path rather than a call into `HeldKeys`, so a fault there cannot agree with itself.
int expectedKey(mod::tracker::Rule rule, const std::vector<int>& held, int whenEmpty) {
	if (held.empty()) {
		return whenEmpty;
	}
	switch (rule) {
	case mod::tracker::Rule::Lowest: return *std::min_element(held.begin(), held.end());
	case mod::tracker::Rule::Highest: return *std::max_element(held.begin(), held.end());
	default: return held.back();
	}
}

/// THE SCRIPT EVERY KEYTRACK CASE PLAYS: C3, then C5 above it, then C2 under both, then each released in
/// turn, newest first — chosen so each rule gives a DIFFERENT note at some step (Last 36 where Highest is
/// 72 and Lowest 36; Last 72 where Lowest is 48), and the last release leaves nothing held.
struct Step {
	bool on;
	int key;
};
constexpr Step kScript[] = { { true, 48 }, { true, 72 }, { true, 36 }, { false, 36 }, { false, 72 }, { false, 48 } };

/// WALK THE SCRIPT, one block per step, and at each step hand `check` the note EACH rule should have picked —
/// indexed by `mod::tracker::Rule` — so one walk can check devices governed by different rules.
template <typename Check>
void walkScriptAll(Venue& v, Check check) {
	std::vector<int> held;
	std::array<int, 3> last { 60, 60, 60 };
	for (const Step& s : kScript) {
		NoteStream events;
		if (s.on) {
			events.on(static_cast<int16_t>(s.key), 100);
			held.push_back(s.key);
		}
		else {
			events.off(static_cast<int16_t>(s.key), 100);
			held.erase(std::find(held.begin(), held.end(), s.key));
		}
		play(v, events);
		// WITH NOTHING HELD THE NOTE STAYS WHERE IT WAS — no snap back to the reference key, which would be a jump.
		for (size_t r = 0; r < last.size(); ++r) {
			last[r] = expectedKey(static_cast<mod::tracker::Rule>(r), held, last[r]);
		}
		CAPTURE(s.on);
		CAPTURE(s.key);
		check(last);
	}
}

/// THE SAME WALK, for a device governed by one rule.
template <typename Check>
void walkScript(Venue& v, mod::tracker::Rule rule, Check check) {
	walkScriptAll(v, [&](const std::array<int, 3>& byRule) {
		CAPTURE(static_cast<int>(rule));
		check(byRule[static_cast<size_t>(rule)]);
	});
}

/// A TRACKER ADDED AT THE CHAIN'S END, at `position`, set to `rule` (and `glide`). Answers its id.
uint32_t addTracker(Venue& v, uint32_t position, mod::tracker::Rule rule, double glide = 0.0) {
	const uint32_t id = v.test()->add_device(v.plugin, mod::tracker::kTraits.tag);
	REQUIRE(id != 0);
	setByName(v, position, "Rule", static_cast<double>(rule));
	setByName(v, position, "Glide", glide);
	return id;
}

/// A BUS FILTER ADDED AT THE CHAIN'S END, at `position`, Keytrack on and Poly off. Answers its id.
uint32_t addKeytrackedFilter(Venue& v, uint32_t position) {
	const uint32_t id = v.test()->add_device(v.plugin, fx::filter::kTraits.tag);
	REQUIRE(id != 0);
	setByName(v, position, "Filter Keytrack", 1.0);
	setByName(v, position, "Filter Poly", 0.0);
	return id;
}

/// THE FILTER'S KEY OFFSET, read off its own target — the number its corner moves by.
float keyOffsetOf(const Venue& v, uint32_t id) {
	return fx::filter::opsTargetForTest(storageOf(v, id)).keyOffsetSemitones;
}

constexpr mod::tracker::Rule kRules[] = { mod::tracker::Rule::Last, mod::tracker::Rule::Lowest,
	                                      mod::tracker::Rule::Highest };

} // namespace

TEST_CASE("rack keytrack: with no Tracker, a bus Filter (Keytrack on, Poly off) follows the last held note — corner = knob x 2^((note-60)/12)") {
	Venue v;
	const uint32_t id = addKeytrackedFilter(v, 0);
	v.tone();
	v.settle();
	const float knob = bridgeOf(v).fieldAt(id, static_cast<size_t>(fx::filter::Param::Cutoff));
	const auto cornerIs = [&](int note) {
		const fx::filter::Settings& target = fx::filter::opsTargetForTest(storageOf(v, id));
		CHECK(target.keyOffsetSemitones == doctest::Approx(static_cast<float>(note - fx::filter::kKeytrackReferenceKey)));
		CHECK(target.cutoffLog2Hz == doctest::Approx(knob));
		// THE CORNER ITSELF, in hertz: the knob's, times 2^((note - 60) / 12).
		const double corner = std::exp2(static_cast<double>(target.cutoffLog2Hz) + target.keyOffsetSemitones / 12.0);
		CHECK(corner == doctest::Approx(std::exp2(static_cast<double>(knob)) * std::exp2((note - 60) / 12.0)).epsilon(1e-6));
	};
	SUBCASE("C2, then C4 over it, then C4 released, then C2 released") {
		NoteStream c2;
		c2.on(36, 100);
		play(v, c2);
		cornerIs(36);
		NoteStream c4;
		c4.on(60, 100);
		play(v, c4);
		cornerIs(60);
		NoteStream upC4;
		upC4.off(60, 100);
		play(v, upC4);
		cornerIs(36); // back to the one still held
		NoteStream upC2;
		upC2.off(36, 100);
		play(v, upC2);
		cornerIs(36); // nothing held: it stays — no snap to the reference key
	}
	SUBCASE("the whole script, by the last held note") {
		walkScript(v, mod::tracker::Rule::Last, cornerIs);
	}
}

TEST_CASE("rack keytrack: Keytrack OFF leaves the bus Filter at the knob whatever is held — the control for the case above") {
	Venue v;
	const uint32_t id = v.test()->add_device(v.plugin, fx::filter::kTraits.tag);
	REQUIRE(id != 0);
	setByName(v, 0, "Filter Keytrack", 0.0);
	v.tone();
	v.settle();
	walkScript(v, mod::tracker::Rule::Last, [&](int) { CHECK(keyOffsetOf(v, id) == 0.0f); });
}

TEST_CASE("rack keytrack: with no Tracker, a bus Waypoint filter with Keytrack on follows the last held note") {
	Venue v;
	const uint32_t id = v.test()->add_device(v.plugin, fx::waypoint::kTraits.tag);
	REQUIRE(id != 0);
	setByName(v, 0, "Keytrack", 1.0);
	setByName(v, 0, "Poly", 0.0);
	v.tone();
	v.settle();
	walkScript(v, mod::tracker::Rule::Last, [&](int note) {
		CHECK(fx::waypoint::targetForTest(storageOf(v, id)).keyOffsetSemitones
		      == doctest::Approx(static_cast<float>(note - fx::filter::kKeytrackReferenceKey)));
	});
}

TEST_CASE("rack keytrack: a Tracker upstream hands the Filter after it the note its Rule picks") {
	for (const mod::tracker::Rule rule : kRules) {
		Venue v;
		(void)addTracker(v, 0, rule);
		const uint32_t id = addKeytrackedFilter(v, 1);
		v.tone();
		v.settle();
		walkScript(v, rule, [&](int note) {
			CHECK(keyOffsetOf(v, id) == doctest::Approx(static_cast<float>(note - fx::filter::kKeytrackReferenceKey)));
		});
	}
}

TEST_CASE("rack keytrack: a Filter BEFORE the Tracker keeps the collapsed note; the one after follows the Tracker") {
	Venue v;
	const uint32_t before = addKeytrackedFilter(v, 0);
	(void)addTracker(v, 1, mod::tracker::Rule::Lowest);
	const uint32_t after = addKeytrackedFilter(v, 2);
	v.tone();
	v.settle();
	walkScriptAll(v, [&](const std::array<int, 3>& byRule) {
		CHECK(keyOffsetOf(v, before) == doctest::Approx(static_cast<float>(byRule[0] - 60)));
		CHECK(keyOffsetOf(v, after) == doctest::Approx(static_cast<float>(byRule[1] - 60)));
	});
}

TEST_CASE("rack keytrack: the reported chain — filter1 → Tracker(Lowest) → filter2 → Tracker(Highest) → filter3, each following its nearest upstream one") {
	Venue v;
	const uint32_t filter1 = addKeytrackedFilter(v, 0);
	(void)addTracker(v, 1, mod::tracker::Rule::Lowest);
	const uint32_t filter2 = addKeytrackedFilter(v, 2);
	(void)addTracker(v, 3, mod::tracker::Rule::Highest);
	const uint32_t filter3 = addKeytrackedFilter(v, 4);
	v.tone();
	v.settle();
	walkScriptAll(v, [&](const std::array<int, 3>& byRule) {
		CHECK(keyOffsetOf(v, filter1) == doctest::Approx(static_cast<float>(byRule[0] - 60))); // the default, Last
		CHECK(keyOffsetOf(v, filter2) == doctest::Approx(static_cast<float>(byRule[1] - 60))); // Lowest
		CHECK(keyOffsetOf(v, filter3) == doctest::Approx(static_cast<float>(byRule[2] - 60))); // Highest
	});
}

TEST_CASE("rack keytrack: a Phaser before a Tracker keeps the collapsed note; one after follows the Tracker") {
	Venue v;
	const uint32_t before = v.test()->add_device(v.plugin, fx::phaser::kTraits.tag);
	REQUIRE(before != 0);
	setByName(v, 0, "Phaser Key Track", 1.0);
	(void)addTracker(v, 1, mod::tracker::Rule::Highest);
	const uint32_t after = v.test()->add_device(v.plugin, fx::phaser::kTraits.tag);
	REQUIRE(after != 0);
	setByName(v, 2, "Phaser Key Track", 1.0);
	v.tone();
	v.settle();
	walkScriptAll(v, [&](const std::array<int, 3>& byRule) {
		CHECK(static_cast<const fx::phaser::Phaser*>(storageOf(v, before))->noteSemitonesForTest() == doctest::Approx(byRule[0]));
		CHECK(static_cast<const fx::phaser::Phaser*>(storageOf(v, after))->noteSemitonesForTest() == doctest::Approx(byRule[2]));
	});
}

TEST_CASE("rack keytrack: a switched-off Tracker hands nothing on — the devices after it keep the collapsed note") {
	Venue v;
	(void)addTracker(v, 0, mod::tracker::Rule::Lowest);
	setByName(v, 0, "Enabled", 0.0);
	const uint32_t id = addKeytrackedFilter(v, 1);
	v.tone();
	v.settle();
	walkScript(v, mod::tracker::Rule::Last, [&](int note) {
		CHECK(keyOffsetOf(v, id) == doctest::Approx(static_cast<float>(note - 60)));
	});
}

TEST_CASE("rack keytrack: the Tracker's Glide glides the note it hands on; at zero it jumps") {
	// THE GLIDED PITCH IS THE HONEST ONE: Glide is the Tracker's "how long the pitch takes to slide between
	// notes", and a keytracked corner after it is following that pitch. Zero is the Rule's key exactly.
	Venue v;
	(void)addTracker(v, 0, mod::tracker::Rule::Last, 0.5);
	const uint32_t id = addKeytrackedFilter(v, 1);
	v.tone();
	v.settle();
	NoteStream first;
	first.on(48, 0);
	play(v, first);
	CHECK(keyOffsetOf(v, id) == doctest::Approx(-12.0f)); // the first note arrives where it is played
	NoteStream second;
	second.on(72, 0);
	play(v, second);
	const float partWay = keyOffsetOf(v, id);
	CAPTURE(partWay);
	CHECK(partWay > -12.0f + 1.0f);
	CHECK(partWay < 12.0f - 0.1f);
	for (int i = 0; i < 200; ++i) {
		v.run();
	}
	CHECK(keyOffsetOf(v, id) == doctest::Approx(12.0f).epsilon(1e-3));
}

TEST_CASE("rack keytrack: the Phaser, the Disperser and Automod after a Tracker set to Lowest follow the lowest held note") {
	// THESE THREE ALREADY FOLLOWED THE RACK'S NOTE (the collapsed one). What is new is that a Tracker upstream
	// chooses it. The phaser latches only while a key is down and the others follow the held pitch, so all three
	// hold the last note after the release.
	Venue v;
	(void)addTracker(v, 0, mod::tracker::Rule::Lowest);
	const uint32_t phaser = v.test()->add_device(v.plugin, fx::phaser::kTraits.tag);
	const uint32_t disperser = v.test()->add_device(v.plugin, fx::disperser::kTraits.tag);
	const uint32_t automod = v.test()->add_device(v.plugin, fx::automod::kTraits.tag);
	REQUIRE(phaser != 0);
	REQUIRE(disperser != 0);
	REQUIRE(automod != 0);
	setByName(v, 1, "Phaser Key Track", 1.0);
	setByName(v, 2, "Disperser Key Track", 1.0);
	setByName(v, 3, "Keytrack", 1.0);
	v.tone();
	v.settle();
	walkScript(v, mod::tracker::Rule::Lowest, [&](int note) {
		CHECK(static_cast<const fx::phaser::Phaser*>(storageOf(v, phaser))->noteSemitonesForTest() == doctest::Approx(note));
		CHECK(static_cast<const fx::disperser::Disperser*>(storageOf(v, disperser))->noteSemitonesForTest()
		      == doctest::Approx(note));
		CHECK(static_cast<const fx::automod::Automod*>(storageOf(v, automod))->noteSemitonesForTest()
		      == doctest::Approx(static_cast<float>(note)));
	});
}

TEST_CASE("rack keytrack: a per-note tuning expression bends the rack's note, as it bends Feathers' bus note") {
	Venue v;
	const uint32_t id = v.test()->add_device(v.plugin, fx::filter::kTraits.tag);
	REQUIRE(id != 0);
	setByName(v, 0, "Filter Keytrack", 1.0);
	v.tone();
	v.settle();
	NoteStream struck;
	struck.on(67, 0).expression(67, CLAP_NOTE_EXPRESSION_TUNING, 2.0, 10);
	play(v, struck);
	CHECK(fx::filter::opsTargetForTest(storageOf(v, id)).keyOffsetSemitones == doctest::Approx(69.0f - 60.0f));
	CHECK(bridgeOf(v).engineForTest().noteContextForTest().gate); // an expression lifts no key
	// A FRESH STRIKE OF THE KEY IS UNTUNED.
	NoteStream again;
	again.off(67, 0).on(67, 20);
	play(v, again);
	CHECK(fx::filter::opsTargetForTest(storageOf(v, id)).keyOffsetSemitones == doctest::Approx(7.0f));
}

//------------------------------------------------------------------------------------------------//
// THE GATE, FROM THE NOTE ROAD, AT THE NOTE'S OWN SAMPLE.                                          //
//------------------------------------------------------------------------------------------------//

namespace {

/// A DAHDSR WITH POLY OFF ROUTED AT A FILTER'S CUTOFF — the shared-shape envelope, which reads the gate.
struct GatedEnvelope {
	Venue v;
	uint32_t fx = 0;
	uint32_t env = 0;
	size_t cutoff = 0;
	float idle = 0.0f;

	GatedEnvelope() {
		const auto* t = v.test();
		fx = t->add_device(v.plugin, fx::filter::kTraits.tag);
		env = t->add_device(v.plugin, mod::dahdsr::kTraits.tag);
		REQUIRE(env != 0);
		setByName(v, 1, "Env Poly", 0.0);
		cutoff = controlNamed(fx::filter::kDescriptor, "Cutoff");
		t->set_route(v.plugin, env, 0, fx, static_cast<uint32_t>(cutoff), 0.5f);
		v.tone();
		v.settle();
		idle = resolved();
	}
	float resolved() const { return v.test()->resolved_field(v.plugin, fx, cutoff); }
	bool gate() const { return bridgeOf(v).engineForTest().gate(); }
};

} // namespace

TEST_CASE("rack gate: a note that starts and ends inside one host block still strikes a Poly-off envelope") {
	// THE GATE WAS A COUNTER IN THE WRAPPER, applied at the top of the host block: an on and its off in one block
	// queued "true" then "false", and the block ran with the gate shut.
	GatedEnvelope g;
	NoteStream blip;
	blip.on(60, 0).off(60, 256);
	play(g.v, blip);
	CAPTURE(g.idle);
	CAPTURE(g.resolved());
	CHECK(g.resolved() > g.idle + 0.01f);
	CHECK_FALSE(g.gate()); // and it is shut again by the block's end
}

TEST_CASE("rack gate: the gate opens in the control tick that holds the note, not at the top of the host block") {
	// THE TICK, NOT THE SAMPLE, and the same grid Feathers' lane runs on: the block is cut every 64 samples and
	// each piece's notes open the gate at that piece's start. So a note at 448 and a note at 511 — one tick — leave
	// the envelope exactly where each other does, one tick in; a note at 0 leaves it eight ticks in. The wrapper
	// opened the gate at the top of the host block for all three, so all three read alike.
	const auto after = [](uint32_t at) {
		GatedEnvelope g;
		NoteStream on;
		on.on(60, at);
		play(g.v, on);
		REQUIRE(g.gate());
		return std::pair { g.idle, g.resolved() };
	};
	const auto [idle, atZero] = after(0);
	const float at448 = after(448).second;
	const float at511 = after(511).second;
	CAPTURE(idle);
	CAPTURE(atZero);
	CAPTURE(at448);
	CAPTURE(at511);
	CHECK(at448 > idle);                        // it struck, in the block's last tick
	CHECK(at511 == doctest::Approx(at448));     // and the same tick is the same instant
	CHECK(std::abs(atZero - at448) > 0.01f);    // eight ticks of envelope are not one
}

TEST_CASE("rack gate: an off for a key nobody holds leaves the gate open under the key that is held") {
	GatedEnvelope g;
	NoteStream on;
	on.on(60);
	play(g.v, on);
	REQUIRE(g.gate());
	NoteStream stray;
	stray.off(61);
	play(g.v, stray);
	CHECK(g.gate());
}

TEST_CASE("rack gate: a wildcard choke (key -1) releases every held key") {
	GatedEnvelope g;
	NoteStream chord;
	chord.on(60).on(64).on(67);
	play(g.v, chord);
	REQUIRE(g.gate());
	NoteStream choke;
	choke.note(CLAP_EVENT_NOTE_CHOKE, -1);
	play(g.v, choke);
	CHECK_FALSE(g.gate());
	CHECK_FALSE(bridgeOf(g.v).engineForTest().noteContextForTest().gate);
}

//------------------------------------------------------------------------------------------------//
// NOTE EXPRESSIONS REACH THE RACK, AND LEAVE IT AS EXPRESSIONS.                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("rack expressions: pressure reaches a Note Expressions modulator, and passes through as an expression, never a choke") {
	Venue v;
	const auto* t = v.test();
	const uint32_t fx = t->add_device(v.plugin, fx::filter::kTraits.tag);
	const uint32_t nexp = t->add_device(v.plugin, mod::noteexpr::kTraits.tag);
	REQUIRE(nexp != 0);
	const size_t cutoff = controlNamed(fx::filter::kDescriptor, "Cutoff");
	t->set_route(v.plugin, nexp, CLAP_NOTE_EXPRESSION_PRESSURE, fx, static_cast<uint32_t>(cutoff), 0.5f);
	v.tone();
	v.settle();
	NoteStream on;
	on.on(60);
	play(v, on);
	const float before = t->resolved_field(v.plugin, fx, cutoff);

	struct Captured {
		int expressions = 0;
		int chokes = 0;
		double value = -1.0;
	} captured;
	clap_output_events sink {};
	sink.ctx = &captured;
	sink.try_push = [](const clap_output_events* list, const clap_event_header* h) {
		auto* c = static_cast<Captured*>(list->ctx);
		if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_NOTE_EXPRESSION) {
			++c->expressions;
			c->value = reinterpret_cast<const clap_event_note_expression*>(h)->value;
		}
		if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_NOTE_CHOKE) {
			++c->chokes;
		}
		return true;
	};
	NoteStream press;
	press.expression(60, CLAP_NOTE_EXPRESSION_PRESSURE, 1.0, 0);
	clap_process p {};
	p.frames_count = static_cast<uint32_t>(v.l.size());
	p.audio_inputs = &v.in;
	p.audio_inputs_count = 1;
	p.audio_outputs = &v.out;
	p.audio_outputs_count = 1;
	p.in_events = &press.in;
	p.out_events = &sink;
	REQUIRE(v.plugin->process(v.plugin, &p) == CLAP_PROCESS_CONTINUE);

	const float after = t->resolved_field(v.plugin, fx, cutoff);
	CAPTURE(before);
	CAPTURE(after);
	CHECK(after > before + 0.01f);
	CHECK(captured.expressions == 1);
	CHECK(captured.value == 1.0);
	CHECK(captured.chokes == 0);
	CHECK(bridgeOf(v).engineForTest().gate()); // pressure lifted no key
}


// ---------------------------------------------------------------------------------------------------
// THE RACK'S process() FLUSHES DENORMALS AND PUTS THE HOST'S CONTROL WORD BACK (denormal_guard.h). The
// guard's own case in test_clap_render holds the class; this holds the rack's call site, whose deletion
// nothing else noticed. The probe reads the FPU from the host's `in_events->size()`, inside the block.
// ---------------------------------------------------------------------------------------------------
#include "denormal_probe.h"

TEST_CASE("the rack's process() flushes denormals inside the block and hands the host back its own control word, flushing or not") {
	Venue v;
	v.settle();
	// `settle()` has already run one process(), so a rack that leaves flush-to-zero set on the host's thread
	// is red HERE, before the probe's own block.
	REQUIRE_FALSE(DenormalProbe::flushesNow());
	// Read after the probe, whose multiply raises the sticky DE flag (part of the word).
	const unsigned long long hostWord = feathers::DenormalGuard::currentControlWord();
	DenormalProbe plain;
	clap_input_events plainEvents = plain.events();
	REQUIRE(v.run(&plainEvents) != CLAP_PROCESS_ERROR);
	REQUIRE(plain.called);
	if (feathers::DenormalGuard::available()) {
		CHECK(plain.flushedInside);
		CHECK(plain.wordInside != hostWord);
	}
	CHECK(feathers::DenormalGuard::currentControlWord() == hostWord); // BY VALUE
	CHECK_FALSE(DenormalProbe::flushesNow());

	// A host that already flushes keeps its bits SET: restored, not cleared.
	{
		const feathers::DenormalGuard hostsOwn;
		const unsigned long long flushingWord = feathers::DenormalGuard::currentControlWord();
		DenormalProbe flushing;
		clap_input_events flushingEvents = flushing.events();
		REQUIRE(v.run(&flushingEvents) != CLAP_PROCESS_ERROR);
		REQUIRE(flushing.called);
		CHECK(feathers::DenormalGuard::currentControlWord() == flushingWord);
		if (feathers::DenormalGuard::available()) {
			CHECK(DenormalProbe::flushesNow());
		}
	}
	CHECK(feathers::DenormalGuard::currentControlWord() == hostWord);
}

TEST_CASE("backlog LG: the rack's Libra mark opens the same settings screen, and its macros make room for it") {
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	// A SCRATCH DESK ONLY (backlog Q): this case writes the hue preference, and the venue's scratch
	// XDG_CONFIG_HOME is the only place it may land.
	const char* xdg = std::getenv("XDG_CONFIG_HOME");
	REQUIRE(xdg != nullptr);
	REQUIRE(std::string(xdg).find("feathers-venue-config") != std::string::npos);
	struct Reset {
		~Reset() {
			(void)ui::setChromeHueOffset(0.0);
			(void)ui::setAccentHueOffset(0.0);
		}
	} reset;
	rack::RackEngine engine { rack::kRackAxes, hostCatalogue() };
	rack::RackBridge bridge { engine };
	rack::RackGui gui { bridge };
	REQUIRE(gui.create(CLAP_WINDOW_API_X11, false));
	rack::RackEditor* editor = gui.editorForTest();
	REQUIRE(editor != nullptr);
	ui::LibraDoor* door = editor->settingsDoorForTest();
	ui::SettingsScreen* screen = gui.settingsForTest();
	REQUIRE(door != nullptr);
	REQUIRE(screen != nullptr);
	CHECK(door->isVisible());
	CHECK(door->markBounds().width() > 0.0f);
	// THE MACROS START UNDER THE DOOR (backlog OC: the door heads the macro column).
	REQUIRE_FALSE(editor->macrosForTest().empty());
	CHECK(editor->macrosForTest().front()->y() >= door->y() + door->height());
	CHECK_FALSE(screen->isVisible());

	visage::MouseEvent press;
	press.position = { door->width() * 0.5f, door->height() * 0.5f };
	door->mouseDown(press);
	CHECK(screen->isVisible());
	REQUIRE(gui.appForTest() != nullptr);
	CHECK(gui.appForTest()->children().back() == screen);

	// THE HUE, through the screen, onto the rack's desk.
	visage::MouseEvent onStrip;
	onStrip.position = { screen->hueX(60.0), screen->hueStripBounds().y() + 2.0f };
	screen->mouseDown(onStrip);
	screen->mouseUp(onStrip);
	CHECK(ui::chromeHueOffset() == 60.0);
	feathers::EditorPrefs again;
	again.load();
	CHECK(again.number(ui::kChromeHuePrefKey, 0.0) == 60.0);
	// THE ACCENT HUE, the second axis, onto the rack's desk through the rack's own watch.
	visage::MouseEvent onAccent;
	onAccent.position = { screen->accentX(-30.0), screen->accentStripBounds().y() + 2.0f };
	screen->mouseDown(onAccent);
	screen->mouseUp(onAccent);
	CHECK(ui::accentHueOffset() == -30.0);
	CHECK(ui::chromeHueOffset() == 60.0);
	{
		feathers::EditorPrefs accentDesk;
		accentDesk.load();
		CHECK(accentDesk.number(ui::kAccentHuePrefKey, 0.0) == -30.0);
	}

	visage::MouseEvent close;
	close.position = { screen->closeBounds().x() + 4.0f, screen->closeBounds().y() + 4.0f };
	screen->mouseDown(close);
	CHECK_FALSE(screen->isVisible());
	(void)ui::setChromeHueOffset(0.0); // writes 0 back through the rack's watch
	(void)ui::setAccentHueOffset(0.0);
	gui.destroy();
}

TEST_CASE("the rack's sidechain port as the host reads it: input 1, non-main stereo, named Sidechain") {
	Venue v;
	const auto* ports = static_cast<const clap_plugin_audio_ports*>(v.plugin->get_extension(v.plugin, CLAP_EXT_AUDIO_PORTS));
	REQUIRE(ports != nullptr);
	REQUIRE(ports->count(v.plugin, true) == 2);
	clap_audio_port_info info {};
	REQUIRE(ports->get(v.plugin, 1, true, &info));
	CHECK(std::string(info.name) == "Sidechain");
	CHECK((info.flags & CLAP_AUDIO_PORT_IS_MAIN) == 0); // an aux input: the main input stays the carrier
	CHECK(info.channel_count == 2);
	CHECK(std::string(info.port_type) == CLAP_PORT_STEREO);
	CHECK(info.in_place_pair == CLAP_INVALID_ID);
	clap_audio_port_info main {};
	REQUIRE(ports->get(v.plugin, 0, true, &main));
	CHECK(info.id != main.id);
}

TEST_CASE("a sidechain fed through the rack's process() reads as sounding on a Spectral at the top and inside a Chain") {
	Venue v;
	v.tone();
	rack::RackBridge& bridge = bridgeOf(v);
	const DeviceInstanceId top = bridge.addDevice(fx::spectral::kTraits.tag);
	const DeviceInstanceId chain = bridge.addDevice(rack::kDeviceDescriptor.traits.tag);
	REQUIRE(top != kNoInstance);
	REQUIRE(chain != kNoInstance);
	v.settle();
	visit(v.plugin);
	rack::RackBridge* inner = rack::bridgeOf(bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);
	const DeviceInstanceId nested = inner->addDevice(fx::spectral::kTraits.tag);
	REQUIRE(nested != kNoInstance);
	v.settle();
	visit(v.plugin);

	// THE HOST'S SIDECHAIN: input 1, a noise a host would hand once something is routed to it (no constant mask).
	std::vector<float> sideL(v.l.size()), sideR(v.l.size());
	uint32_t seed = 1;
	for (size_t i = 0; i < sideL.size(); ++i) {
		seed = seed * 1664525u + 1013904223u;
		sideL[i] = sideR[i] = 0.25f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
	}
	float* sidePtr[2] { sideL.data(), sideR.data() };
	clap_audio_buffer ins[2] { v.in, {} };
	ins[1].data32 = sidePtr;
	ins[1].channel_count = 2;
	clap_input_events none = emptyEvents();
	clap_output_events sink = sinkEvents();
	for (int b = 0; b < 16; ++b) {
		v.tone();
		clap_process p {};
		p.frames_count = static_cast<uint32_t>(v.l.size());
		p.audio_inputs = ins;
		p.audio_inputs_count = 2;
		p.audio_outputs = &v.out;
		p.audio_outputs_count = 1;
		p.in_events = &none;
		p.out_events = &sink;
		REQUIRE(v.plugin->process(v.plugin, &p) == CLAP_PROCESS_CONTINUE);
	}
	visit(v.plugin);
	// WHAT THE CARD READS (SpectralSource::sidechainSounding): the whole picture, and its sounding flag at 1.
	CHECK(bridge.readingCountAt(top) >= fx::spectral::kReadingCount);
	CHECK(bridge.readingAt(top, fx::spectral::kReadingSideSounding) == 1.0f);
	CHECK(inner->readingCountAt(nested) >= fx::spectral::kReadingCount);
	CHECK(inner->readingAt(nested, fx::spectral::kReadingSideSounding) == 1.0f);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG NG — WHAT THE HOST HEARS, EVENT BY EVENT. In Bitwig the rack's macros stayed on top            //
// of the host's recently-used list, and a touched knob did not rise with its name; Feathers behaved.  //
// Bitwig ranks that list from the plugin's output events and names its rows from `get_info` after a  //
// rescan. MEASURED, both plugins driven through their CLAP roads, out-queue written down per block:   //
// (a) at rest both said nothing; (b) a top-level knob's first touch was the same stream in both — the //
// name (RESCAN_VALUES|INFO|TEXT) at the visit, then Value, Begin, Value, End on the slot; but a knob   //
// INSIDE A NESTED CHAIN had its slot renamed and NO event on it; (c) a macro moved in the rack's      //
// editor reached the host with NOTHING — its knob's Begin/End were empty and `setValue` wrote only    //
// the engine — where Feathers sends Begin, Value, End on the macro's id. These cases hold all three.  //
//------------------------------------------------------------------------------------------------//

namespace {

/// Every macro's id, read off the host's own road (`get_info`), so the cases below assert ids the host shows.
std::vector<clap_id> macroIdsOf(const clap_plugin* plugin) {
	std::vector<clap_id> ids;
	for (uint32_t i = 0; i < 8; ++i) {
		clap_param_info info {};
		REQUIRE(slotParamsOf(plugin)->get_info(plugin, i, &info));
		REQUIRE(std::string(info.module) == "Macros");
		ids.push_back(info.id);
	}
	return ids;
}

/// A HAND ON MACRO `index`'S KNOB in the rack's editor: the knob's own `MacroControl`, over the plugin's bridge.
void dragMacro(rack::RackBridge& bridge, size_t index, std::initializer_list<double> values) {
	rack::MacroControl macro(bridge, index, "Macro");
	macro.beginGesture();
	for (const double v : values) {
		macro.setValue(v);
	}
	macro.endGesture();
}

} // namespace

TEST_CASE("backlog NG, the rack at rest: no block and no main-thread visit tells the host anything — no event, no rescan") {
	HostLog host;
	Venue v(host.host());
	rack::RackBridge& bridge = bridgeOf(v);
	REQUIRE(bridge.addDevice(fx::filter::kTraits.tag) != kNoInstance);
	v.settle();
	visit(v.plugin);
	const size_t rescansBefore = host.rescans.size();
	for (int i = 0; i < 16; ++i) {
		CAPTURE(i);
		SlotLog log;
		runBlock(v, nullptr, &log);
		visit(v.plugin);
		mainThreadVisit(v.plugin);
		CHECK(log.events.empty());
	}
	CHECK(host.rescans.size() == rescansBefore);
	// NOT VACUOUS: the same venue, with a hand on a knob, does tell the host — so the silence above is the
	// rack's, not a log that cannot hear.
	drag(v.plugin, "Filter Cutoff", { std::log2(500.0) });
	visit(v.plugin);
	SlotLog moved;
	runBlock(v, nullptr, &moved);
	CHECK(!moved.events.empty());
	CHECK(host.rescans.size() == rescansBefore + 1);
}

TEST_CASE("backlog NG, the rack: a macro moved in the editor reaches the host as Begin, every Value, End on the macro's id") {
	Venue v;
	rack::RackBridge& bridge = bridgeOf(v);
	const std::vector<clap_id> ids = macroIdsOf(v.plugin);
	dragMacro(bridge, 2, { 0.25, 0.5, 0.75 });

	SlotLog log;
	runBlock(v, nullptr, &log);
	REQUIRE(log.events.size() == 5);
	const std::vector<SlotLog::Event> ev = log.on(ids[2]);
	REQUIRE(ev.size() == 5); // every event on THAT macro's id, none on another parameter
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	const double values[3] = { 0.25, 0.5, 0.75 };
	for (size_t i = 0; i < 3; ++i) {
		CAPTURE(i);
		CHECK(ev[i + 1].type == CLAP_EVENT_PARAM_VALUE);
		CHECK(ev[i + 1].value == static_cast<double>(static_cast<float>(values[i]))); // the float the macro holds
	}
	CHECK(ev[4].type == CLAP_EVENT_PARAM_GESTURE_END);
	// AND WHAT THE HOST HEARD IS WHAT IT READS BACK, and what the rack runs.
	double hostReads = -1.0;
	REQUIRE(slotParamsOf(v.plugin)->get_value(v.plugin, ids[2], &hostReads));
	CHECK(hostReads == static_cast<double>(static_cast<float>(0.75)));
	// DRAINED ONCE: the next block says nothing.
	SlotLog next;
	runBlock(v, nullptr, &next);
	CHECK(next.events.empty());

	// A MACRO THE HOST WROTE IS NOT ECHOED BACK — the host sent it, and an echo would rank it as touched.
	OneEvent hostWrite(ids[5], 0.4);
	SlotLog echo;
	runBlock(v, &hostWrite.in, &echo);
	CHECK(echo.events.empty());
	REQUIRE(slotParamsOf(v.plugin)->get_value(v.plugin, ids[5], &hostReads));
	CHECK(hostReads == static_cast<double>(static_cast<float>(0.4))); // and the write did land
}

TEST_CASE("backlog NG, the rack: a macro moved while the rack is not processing reaches the host through params.flush") {
	Venue v;
	rack::RackBridge& bridge = bridgeOf(v);
	const std::vector<clap_id> ids = macroIdsOf(v.plugin);
	dragMacro(bridge, 0, { 0.6 });
	clap_input_events none = emptyEvents();
	SlotLog log;
	slotParamsOf(v.plugin)->flush(v.plugin, &none, &log.out);
	const std::vector<SlotLog::Event> ev = log.on(ids[0]);
	REQUIRE(ev.size() == 3);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[1].value == static_cast<double>(static_cast<float>(0.6)));
	CHECK(ev[2].type == CLAP_EVENT_PARAM_GESTURE_END);
	CHECK(log.events.size() == 3);
}

TEST_CASE("backlog NG, the rack: the FIRST touch of a knob inside a nested chain reaches the host on its slot, after its name") {
	HostLog host;
	Venue v(host.host());
	rack::RackBridge& bridge = bridgeOf(v);
	const DeviceInstanceId chain = bridge.addDevice(rack::kDeviceDescriptor.traits.tag);
	REQUIRE(chain != kNoInstance);
	v.settle();
	visit(v.plugin);
	rack::RackBridge* inner = rack::bridgeOf(bridge.containerSurfaceOf(chain));
	REQUIRE(inner != nullptr);
	const DeviceInstanceId filter = inner->addDevice(fx::filter::kTraits.tag);
	REQUIRE(filter != kNoInstance);
	v.settle();
	visit(v.plugin);
	REQUIRE(slotStateOf(v.plugin, 0) == stateNumber(hostslots::SlotState::Empty));

	const device::ParamDesc& desc = filterControl("filter_cutoff_wide");
	const device::Descriptor* d = inner->descriptorOf(filter);
	REQUIRE(d != nullptr);
	const size_t cutoff = static_cast<size_t>(&desc - fx::filter::kDescriptor.params.data());
	REQUIRE(d->params[cutoff].key == desc.key);
	const double c = std::log2(4000.0);
	{
		// THE CARD'S OWN WIDGET over the NESTED bridge, as the chain's panel builds it.
		ui::DeviceControl knob(*inner, filter, *d, cutoff);
		knob.beginGesture();
		knob.setValue(std::log2(300.0));
		knob.setValue(c);
		knob.endGesture();
	}
	const size_t rescansBefore = host.rescans.size();
	visit(v.plugin);
	// THE NAME FIRST: the host is told to re-read, and reads the nested control's path.
	REQUIRE(host.rescans.size() == rescansBefore + 1);
	CHECK((host.rescans.back() & CLAP_PARAM_RESCAN_INFO) != 0);
	const clap_param_info info = slotInfoOf(v.plugin, 0);
	CHECK(std::string(info.name) == "Chain " + kDot + " Filter " + kDot + " Cutoff");
	CHECK((info.flags & CLAP_PARAM_IS_HIDDEN) == 0);

	// THEN THE TOUCH, at the next block: the takeover outside any gesture, then the edit as one gesture,
	// carrying where the hand left the control — restated from the declaration, not read back.
	const double sent = static_cast<double>(
	    static_cast<float>(device::toNormal(desc, static_cast<double>(device::coerceControl(desc, static_cast<float>(c))))));
	SlotLog log;
	runBlock(v, nullptr, &log);
	const std::vector<SlotLog::Event> ev = log.on(kSlot0);
	REQUIRE(ev.size() == 4);
	CHECK(ev[0].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[0].value == sent);
	CHECK(ev[1].type == CLAP_EVENT_PARAM_GESTURE_BEGIN);
	CHECK(ev[2].type == CLAP_EVENT_PARAM_VALUE);
	CHECK(ev[2].value == sent);
	CHECK(ev[3].type == CLAP_EVENT_PARAM_GESTURE_END);
	CHECK(log.events.size() == 4);
	// ONCE: the debt is paid, and the next visit and block say nothing.
	visit(v.plugin);
	SlotLog next;
	runBlock(v, nullptr, &next);
	CHECK(next.events.empty());
}

//------------------------------------------------------------------------------------------------//
// THE RACK'S NOTE OUTPUT (2026-10-31): an arp in the rack plays any instrument                                     //
//------------------------------------------------------------------------------------------------//

namespace {

/// ONE EVENT THE RACK HANDED THE HOST, and the absolute sample it lands on (block start + header time).
struct Left {
	uint16_t type;
	int16_t key;
	int32_t noteId;
	int64_t sample;
	int32_t expressionId = -1;
	double value = 0.0;
	friend bool operator==(const Left&, const Left&) = default;
};

/// A HOST BLOCK WITH A TRANSPORT AND AN OUTPUT QUEUE THAT KEEPS WHAT IT IS GIVEN — the venue's run, plus those two.
struct NoteHost {
	explicit NoteHost(Venue& venue) : v(venue) {}
	Venue& v;
	std::vector<Left> left;
	int64_t at = 0;            ///< the absolute sample the next block starts at
	bool playing = true;
	double bpm = 120.0;
	double songBeatsAtZero = 0.0;
	double stoppedAt = 0.0;     ///< where a stopped host says the song stands
	bool ordered = true;       ///< CLAP asks for an output queue in time order; every push checks it
	bool audio = true;         ///< false: a Note Rack, handed no audio buffers at all

	clap_process_status block(NoteStream* events = nullptr) {
		clap_event_transport tp {};
		tp.header.size = sizeof(tp);
		tp.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		tp.header.type = CLAP_EVENT_TRANSPORT;
		tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
		           | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
		tp.tempo = bpm;
		const double beats = playing ? songBeatsAtZero + static_cast<double>(at) / 48000.0 * bpm / 60.0 : stoppedAt;
		tp.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * static_cast<double>(CLAP_BEATTIME_FACTOR)));
		tp.song_pos_seconds =
		    static_cast<clap_sectime>(std::llround(beats * 60.0 / bpm * static_cast<double>(CLAP_SECTIME_FACTOR)));
		clap_input_events none = emptyEvents();
		clap_output_events sink {};
		sink.ctx = this;
		sink.try_push = [](const clap_output_events* list, const clap_event_header* h) {
			auto* self = static_cast<NoteHost*>(list->ctx);
			if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) {
				return true;
			}
			const int64_t sample = self->at + static_cast<int64_t>(h->time);
			if (!self->left.empty() && sample < self->left.back().sample) {
				self->ordered = false;
			}
			if (h->type == CLAP_EVENT_NOTE_EXPRESSION) {
				const auto* x = reinterpret_cast<const clap_event_note_expression*>(h);
				self->left.push_back({ h->type, x->key, x->note_id, sample, x->expression_id, x->value });
			}
			else if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE) {
				const auto* n = reinterpret_cast<const clap_event_note*>(h);
				self->left.push_back({ h->type, n->key, n->note_id, sample });
			}
			return true;
		};
		clap_process p {};
		p.frames_count = static_cast<uint32_t>(v.l.size());
		p.audio_inputs = audio ? &v.in : nullptr;
		p.audio_inputs_count = audio ? 1u : 0u;
		p.audio_outputs = audio ? &v.out : nullptr;
		p.audio_outputs_count = audio ? 1u : 0u;
		p.transport = &tp;
		p.in_events = events != nullptr ? &events->in : &none;
		p.out_events = &sink;
		const clap_process_status status = v.plugin->process(v.plugin, &p);
		at += static_cast<int64_t>(v.l.size());
		return status;
	}
	std::vector<Left> ons() const {
		std::vector<Left> r;
		for (const Left& e : left) {
			if (e.type == CLAP_EVENT_NOTE_ON) {
				r.push_back(e);
			}
		}
		return r;
	}
	/// NO STUCK NOTES: every key's ons and offs alternate and end off.
	void checkBalanced() const {
		std::map<int, int> sounding;
		for (const Left& e : left) {
			if (e.type == CLAP_EVENT_NOTE_ON) {
				CHECK_MESSAGE(sounding[e.key] == 0, "an on for " << e.key << " at " << e.sample << " while it sounds");
				sounding[e.key] = 1;
			}
			else if (e.type == CLAP_EVENT_NOTE_OFF || e.type == CLAP_EVENT_NOTE_CHOKE) {
				sounding[e.key] = 0;
			}
		}
		for (const auto& [key, n] : sounding) {
			CHECK_MESSAGE(n == 0, "key " << key << " left sounding");
		}
	}
};

} // namespace

TEST_CASE("rack note out: with no note device, exactly the notes that came in leave, at the same samples") {
	// PASS-THROUGH (rack-note-lane §3): ordinary MIDI-thru, bit for bit — kinds, keys, ids, expressions and offsets.
	Venue v(512);
	NoteHost h { v };
	NoteStream in;
	in.note(CLAP_EVENT_NOTE_ON, 60, 0, 11)
	    .note(CLAP_EVENT_NOTE_ON, 64, 37, 12)
	    .expression(60, CLAP_NOTE_EXPRESSION_TUNING, 0.5, 100)
	    .note(CLAP_EVENT_NOTE_OFF, 60, 300, 11)
	    .note(CLAP_EVENT_NOTE_OFF, 64, 511, 12);
	REQUIRE(h.block(&in) == CLAP_PROCESS_CONTINUE);
	const std::vector<Left> want { { CLAP_EVENT_NOTE_ON, 60, 11, 0 },
		                           { CLAP_EVENT_NOTE_ON, 64, 12, 37 },
		                           { CLAP_EVENT_NOTE_EXPRESSION, 60, -1, 100, CLAP_NOTE_EXPRESSION_TUNING, 0.5 },
		                           { CLAP_EVENT_NOTE_OFF, 60, 11, 300 },
		                           { CLAP_EVENT_NOTE_OFF, 64, 12, 511 } };
	CHECK(h.left == want);
	CHECK(h.ordered);
}

TEST_CASE("rack note out: an Arp in the rack plays its steps to the host on their exact samples, and the chord itself "
          "does not leave") {
	// 120 bpm, 1/16: a step every 6000 samples, from the song's beat 0, in host blocks of 512 (no block edge on a step).
	namespace ap = mod::arp;
	Venue v(512);
	REQUIRE(v.test()->add_device(v.plugin, ap::kTraits.tag) != 0);
	v.settle(); // the rack adopts its device at a block
	NoteHost h { v };
	h.at = 0;
	NoteStream chord;
	chord.note(CLAP_EVENT_NOTE_ON, 64, 0, 1).note(CLAP_EVENT_NOTE_ON, 60, 0, 2).note(CLAP_EVENT_NOTE_ON, 67, 0, 3);
	REQUIRE(h.block(&chord) == CLAP_PROCESS_CONTINUE);
	while (h.at < 6000 * 4 + 1000) {
		REQUIRE(h.block() == CLAP_PROCESS_CONTINUE); // AN EMITTING RACK NEVER ASKS TO SLEEP (CLAUDE.md's host rule)
	}
	const std::vector<Left> ons = h.ons();
	REQUIRE(ons.size() == 5);
	const int keys[] = { 60, 64, 67, 60, 64 };
	for (size_t i = 0; i < ons.size(); ++i) {
		CAPTURE(i);
		CHECK(ons[i].sample == static_cast<int64_t>(i) * 6000);
		CHECK(ons[i].key == keys[i]);
		CHECK(ons[i].noteId == -1); // invented: CLAP's "unspecified" (rack-note-lane §3.1)
	}
	CHECK(h.ordered);
	// THE CHORD'S OFFS DO NOT LEAVE EITHER, and releasing it ends the arp's note.
	NoteStream release;
	release.note(CLAP_EVENT_NOTE_OFF, 64, 0, 1).note(CLAP_EVENT_NOTE_OFF, 60, 0, 2).note(CLAP_EVENT_NOTE_OFF, 67, 0, 3);
	h.block(&release);
	for (int b = 0; b < 30; ++b) {
		h.block();
	}
	for (const Left& e : h.left) {
		CHECK(e.noteId == -1);
	}
	h.checkBalanced();
}

TEST_CASE("rack note out: no stuck notes — removing the Arp mid-note, a re-activate, a bypass and a transport stop "
          "each end what it had sounding") {
	namespace ap = mod::arp;
	Venue v(512);
	const uint32_t id = v.test()->add_device(v.plugin, ap::kTraits.tag);
	REQUIRE(id != 0);
	v.settle();
	rack::RackBridge& bridge = bridgeOf(v);
	const size_t gate = static_cast<size_t>(ap::Param::Gate);
	bridge.setField(id, gate, 2.0f); // 200%: a note is always sounding
	const auto& engine = bridge.engineForTest();
	NoteHost h { v };
	NoteStream chord;
	chord.on(60).on(64);
	h.block(&chord);
	for (int b = 0; b < 20; ++b) {
		h.block();
	}
	REQUIRE(engine.inventedHeldForTest() > 0);

	SUBCASE("removed mid-note: its notes end in the next block") {
		bridge.removeDevice(id);
		h.block(); // through THIS host's queue: the block the removal lands in carries the offs
		h.block();
		CHECK(engine.inventedHeldForTest() == 0);
		h.checkBalanced();
	}
	SUBCASE("a re-activate: the arp forgot, the instrument did not — ended in the first block after") {
		v.plugin->stop_processing(v.plugin);
		v.plugin->deactivate(v.plugin);
		REQUIRE(v.plugin->activate(v.plugin, 48000.0, 1, 512));
		REQUIRE(v.plugin->start_processing(v.plugin));
		NoteStream release;
		release.off(60).off(64);
		h.block(&release);
		CHECK(engine.inventedHeldForTest() == 0);
		h.checkBalanced();
	}
	SUBCASE("bypassed (Enabled off): its notes end") {
		bridge.setField(id, static_cast<size_t>(ap::Param::Enabled), 0.0f);
		h.block();
		h.block();
		CHECK(engine.inventedHeldForTest() == 0);
		h.checkBalanced();
	}
	SUBCASE("the transport stops under Stop with Transport: its notes end") {
		bridge.setField(id, static_cast<size_t>(ap::Param::Run), 1.0f);
		h.block();
		h.playing = false;
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0; // where it stopped
		h.block();
		h.block();
		CHECK(engine.inventedHeldForTest() == 0);
		h.checkBalanced();
	}
	CHECK(h.ordered);
}

namespace {

/// A GUEST EMITTER: an On at sample 0, a TUNING expression at 100 and its Off at 200 (Pattern's TB-Glide in miniature,
/// which is not on mainline yet) — so the expression road is proven by value before the device that will use it.
struct GuestEmitter {
	uint64_t next = 0;
	int stage = 0;
};
const device::NoteOps kGuestNoteOps {
	.stateBytes = sizeof(GuestEmitter),
	.stateAlign = alignof(GuestEmitter),
	.construct = [](void* s) { new (s) GuestEmitter(); },
	.destruct = [](void* s) { static_cast<GuestEmitter*>(s)->~GuestEmitter(); },
	.prepare = [](void*, double) {},
	.setFields = [](void*, const float*, size_t) {},
	.processNotes = [](void*, const device::NoteEvent* in, size_t n, device::NoteEvent* out, size_t cap, size_t* count) {
		const size_t k = std::min(n, cap);
		std::copy_n(in, k, out);
		*count = k;
	},
	.emitNotes = [](void* s, const device::TransportClock&, uint64_t now, device::NoteEvent* out, size_t cap,
	                uint32_t* until) -> size_t {
		auto& g = *static_cast<GuestEmitter*>(s);
		size_t made = 0;
		if (now >= g.next && g.stage < 3 && cap > 0) {
			device::NoteEvent e {};
			e.noteId = -1;
			e.key = 60;
			e.kind = g.stage == 0 ? device::NoteEvent::Kind::On
			         : g.stage == 1 ? device::NoteEvent::Kind::Expression
			                        : device::NoteEvent::Kind::Off;
			if (g.stage == 1) {
				e.expressionId = CLAP_NOTE_EXPRESSION_TUNING;
				e.expressionValue = 2.0;
			}
			out[made++] = e;
			++g.stage;
			g.next = g.stage == 1 ? 100 : 200;
		}
		*until = g.stage < 3 ? static_cast<uint32_t>(std::max<uint64_t>(g.next - now, 1)) : UINT32_MAX;
		return made;
	},
};
inline constexpr const char* kGuestTags[] = { "note" };
const device::Descriptor kGuestEmitter {
	.traits = { .tag = "GUESTEMT", .name = "Guest Emitter", .hasCard = false, .hasExpanded = false, .tags = kGuestTags },
	.params = {},
	.outputs = {},
	.notes = &kGuestNoteOps,
};

} // namespace

TEST_CASE("rack note out: what an emitter makes leaves the engine on its own samples — its TUNING expression too") {
	device::Catalogue catalogue;
	catalogue.find = [](void*, const char* tag) -> const device::Descriptor* {
		return device::sameTag(tag, kGuestEmitter.traits.tag) ? &kGuestEmitter : hostCatalogue()(tag);
	};
	constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 4 };
	rack::RackEngine engine(kAxes, catalogue);
	engine.prepare(48000.0, 512);
	REQUIRE(engine.addDevice(kGuestEmitter.traits.tag) != rack::RackEngine::kNoSlot);
	std::vector<float> l(512, 0.0f), r(512, 0.0f);
	engine.processStereo(l.data(), r.data(), 512, backplane::unknownBlock());
	const std::span<const device::NoteEvent> out = engine.blockNotesOut();
	REQUIRE(out.size() == 3);
	CHECK(out[0].kind == device::NoteEvent::Kind::On);
	CHECK(out[0].timeSamples == 0);
	CHECK(out[1].kind == device::NoteEvent::Kind::Expression);
	CHECK(out[1].timeSamples == 100);
	CHECK(out[1].expressionId == CLAP_NOTE_EXPRESSION_TUNING);
	CHECK(out[1].expressionValue == 2.0);
	CHECK(out[2].kind == device::NoteEvent::Kind::Off);
	CHECK(out[2].timeSamples == 200);
	CHECK(engine.inventedHeldForTest() == 0);
}

TEST_CASE("rack note out: Pattern's TB-Glide bends its held note with TUNING expressions, and they reach the host on "
          "their samples, aimed at a note that is sounding") {
	// THE EXPRESSION ROAD WITH THE DEVICE THAT USES IT (the guest case above proves the engine half): one held note per slide
	// run, its tuning written as the 303 lag moves (test_pattern_device pins the values; this, that they leave the rack).
	namespace pt = mod::arp::pat;
	Venue v(512);
	const uint32_t id = v.test()->add_device(v.plugin, pt::kTraits.tag);
	REQUIRE(id != 0);
	v.settle();
	rack::RackBridge& bridge = bridgeOf(v);
	const auto exact = [](const char* name) {
		for (size_t i = 0; i < pt::kDescriptor.params.size(); ++i) {
			if (std::string(pt::kDescriptor.params[i].name) == name) {
				return i;
			}
		}
		FAIL("no control named " << name);
		return pt::kDescriptor.params.size();
	};
	bridge.setField(id, exact("P1 Mode"), 4.0f); // TB-Glide
	bridge.setField(id, exact("P1 Glide"), 1.0f);
	bridge.setField(id, exact("P1 Density"), 0.0f);
	bridge.setField(id, exact("P1 Hold"), 0.0f);
	bridge.setField(id, exact("P1 Range"), 1.0f);
	NoteHost h { v };
	NoteStream key;
	key.on(48);
	h.block(&key);
	for (int b = 0; b < 300; ++b) {
		h.block();
	}
	std::map<int, int> sounding;
	int tuning = 0;
	for (const Left& e : h.left) {
		if (e.type == CLAP_EVENT_NOTE_ON) {
			sounding[e.key] = 1;
		}
		else if (e.type == CLAP_EVENT_NOTE_OFF) {
			sounding[e.key] = 0;
		}
		else if (e.type == CLAP_EVENT_NOTE_EXPRESSION && e.expressionId == CLAP_NOTE_EXPRESSION_TUNING) {
			++tuning;
			CHECK_MESSAGE(sounding[e.key] == 1, "a bend at " << e.sample << " for key " << e.key << " that is not sounding");
		}
	}
	CHECK(tuning > 0); // not vacuous: the 303 lag was written out
	CHECK(h.ordered);
	NoteStream release;
	release.off(48);
	h.block(&release);
	for (int b = 0; b < 40; ++b) {
		h.block();
	}
	h.checkBalanced();
}

TEST_CASE("thru through the rack plugin: a Drum Pattern upstream of the DAW's drum rack passes the player's kick and snare out at their samples, plays its other lanes from the transport, swallows them with Thru Off, and leaves nothing sounding after a stop") {
	// The use: a chain plugin holding a Drum-mode pattern upstream of a DAW drum rack, with the kick and snare
	// programmed by hand while the pattern's other hits keep playing.
	constexpr uint32_t kFrames = 256;
	const auto run = [&](double thru) {
		Venue v(kFrames);
		const auto* chain = static_cast<const clap_plugin_feathers_test_chain*>(
		    v.plugin->get_extension(v.plugin, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain != nullptr);
		REQUIRE(v.test()->add_device(v.plugin, "PAT") != 0);
		REQUIRE(chain->set_control(v.plugin, 0, "P1 Mode", 5.0)); // Drum
		REQUIRE(chain->set_control(v.plugin, 0, "P1 Kick", 0.0));
		REQUIRE(chain->set_control(v.plugin, 0, "P1 Snare", 0.0));
		REQUIRE(chain->set_control(v.plugin, 0, "P1 Thru", thru));
		struct Captured {
			int64_t base = 0;
			std::vector<std::tuple<uint16_t, int, int64_t>> events; // type, key, absolute sample
		} captured;
		clap_output_events sink {};
		sink.ctx = &captured;
		sink.try_push = [](const clap_output_events* list, const clap_event_header* h) {
			auto* c = static_cast<Captured*>(list->ctx);
			if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF)) {
				const auto* n = reinterpret_cast<const clap_event_note*>(h);
				c->events.emplace_back(h->type, n->key, c->base + h->time);
			}
			return true;
		};
		double beats = 0.0;
		bool playing = true;
		const auto block = [&](const clap_input_events* in) {
			clap_event_transport tp {};
			tp.header.size = sizeof(tp);
			tp.header.type = CLAP_EVENT_TRANSPORT;
			tp.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (playing ? CLAP_TRANSPORT_IS_PLAYING : 0);
			tp.tempo = 120.0;
			tp.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
			clap_input_events none = emptyEvents();
			clap_process p {};
			p.frames_count = kFrames;
			p.transport = &tp;
			p.audio_inputs = &v.in;
			p.audio_inputs_count = 1;
			p.audio_outputs = &v.out;
			p.audio_outputs_count = 1;
			p.in_events = in != nullptr ? in : &none;
			p.out_events = &sink;
			REQUIRE(v.plugin->process(v.plugin, &p) != CLAP_PROCESS_ERROR);
			captured.base += kFrames;
			if (playing) {
				beats += kFrames / 48000.0 * 2.0;
			}
		};
		std::map<std::string, int64_t> when;
		const auto send = [&](uint16_t type, int key, uint32_t offset, const char* name) {
			NoteEvent e(type);
			e.note.key = static_cast<int16_t>(key);
			e.note.note_id = key;
			e.note.header.time = offset;
			when[name] = captured.base + offset;
			block(&e.in);
		};
		for (int b = 0; b < 70; ++b) {
			block(nullptr);
		}
		send(CLAP_EVENT_NOTE_ON, 36, 77, "kick on");
		for (int b = 0; b < 40; ++b) {
			block(nullptr);
		}
		send(CLAP_EVENT_NOTE_ON, 38, 5, "snare on");
		for (int b = 0; b < 40; ++b) {
			block(nullptr);
		}
		send(CLAP_EVENT_NOTE_OFF, 36, 200, "kick off");
		send(CLAP_EVENT_NOTE_OFF, 38, 0, "snare off");
		for (int b = 0; b < 200; ++b) {
			block(nullptr);
		}
		playing = false; // STOP
		for (int b = 0; b < 40; ++b) {
			block(nullptr);
		}
		return std::pair { captured.events, when };
	};
	const auto count = [](const auto& events, uint16_t type, int key) {
		return std::count_if(events.begin(), events.end(), [&](const auto& e) { return std::get<0>(e) == type && std::get<1>(e) == key; });
	};
	const auto has = [](const auto& events, uint16_t type, int key, int64_t sample) {
		return std::any_of(events.begin(), events.end(), [&](const auto& e) {
			return std::get<0>(e) == type && std::get<1>(e) == key && std::get<2>(e) == sample;
		});
	};
	const auto [on, when] = run(2.0);
	CHECK(count(on, CLAP_EVENT_NOTE_ON, 36) == 1);
	CHECK(count(on, CLAP_EVENT_NOTE_ON, 38) == 1);
	CHECK(has(on, CLAP_EVENT_NOTE_ON, 36, when.at("kick on")));
	CHECK(has(on, CLAP_EVENT_NOTE_ON, 38, when.at("snare on")));
	CHECK(has(on, CLAP_EVENT_NOTE_OFF, 36, when.at("kick off")));
	CHECK(has(on, CLAP_EVENT_NOTE_OFF, 38, when.at("snare off")));
	int lanes = 0;
	std::map<int, int> balance;
	for (const auto& [type, key, s] : on) {
		lanes += type == CLAP_EVENT_NOTE_ON && key != 36 && key != 38 ? 1 : 0;
		balance[key] += type == CLAP_EVENT_NOTE_ON ? 1 : -1;
	}
	CHECK(lanes > 5);
	for (const auto& [key, n] : balance) {
		CAPTURE(key);
		CHECK(n == 0);
	}
	const auto [off, whenOff] = run(1.0);
	CHECK(count(off, CLAP_EVENT_NOTE_ON, 36) == 0);
	CHECK(count(off, CLAP_EVENT_NOTE_ON, 38) == 0);
}

//------------------------------------------------------------------------------------------------//
// THE NOTE RACK: the Chain's engine and editor as a CLAP note effect                                         //
//------------------------------------------------------------------------------------------------//

TEST_CASE("note rack: a note effect with a note port each way and no audio, offering only modulators and note effects") {
	const clap_plugin_descriptor* d = rack::noteRackDescriptor();
	REQUIRE(d != nullptr);
	bool noteEffect = false;
	for (const char* const* f = d->features; f != nullptr && *f != nullptr; ++f) {
		noteEffect = noteEffect || std::string(*f) == CLAP_PLUGIN_FEATURE_NOTE_EFFECT;
	}
	CHECK(noteEffect);
	Venue v(makeHost(), 512, true);
	const auto* notes = static_cast<const clap_plugin_note_ports*>(v.plugin->get_extension(v.plugin, CLAP_EXT_NOTE_PORTS));
	REQUIRE(notes != nullptr);
	CHECK(notes->count(v.plugin, true) == 1);
	CHECK(notes->count(v.plugin, false) == 1);
	const auto* audio = static_cast<const clap_plugin_audio_ports*>(v.plugin->get_extension(v.plugin, CLAP_EXT_AUDIO_PORTS));
	if (audio != nullptr) {
		CHECK(audio->count(v.plugin, true) == 0);
		CHECK(audio->count(v.plugin, false) == 0);
	}
	// THE EDITOR'S OFFER, asked of the plugin's own editor.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
	const auto* gui = static_cast<const clap_plugin_gui*>(v.plugin->get_extension(v.plugin, CLAP_EXT_GUI));
	REQUIRE(gui != nullptr);
#if defined(__APPLE__)
	REQUIRE(gui->create(v.plugin, CLAP_WINDOW_API_COCOA, false));
#else
	REQUIRE(gui->create(v.plugin, CLAP_WINDOW_API_X11, false));
#endif
	auto* editor = static_cast<rack::RackEditor*>(v.test()->editor(v.plugin));
	REQUIRE(editor != nullptr);
	const std::vector<const device::Descriptor*> offered = editor->offer();
	bool arp = false;
	bool pattern = false;
	bool lfo = false;
	for (const device::Descriptor* o : offered) {
		CAPTURE(o->traits.name);
		CHECK(belongsInModulationRack(*o)); // no audio effect, no container
		arp = arp || device::sameTag(o->traits.tag, mod::arp::kTraits.tag);
		pattern = pattern || device::sameTag(o->traits.tag, mod::arp::pat::kTraits.tag);
		lfo = lfo || device::sameTag(o->traits.tag, mod::lfo::kTraits.tag);
	}
	CHECK(arp);
	CHECK(pattern);
	CHECK(lfo);
	gui->destroy(v.plugin);
	// AND THE CHAIN'S EDITOR STILL OFFERS AUDIO EFFECTS: the narrowing is the Note Rack's alone.
	Venue chainVenue;
	const auto* chainGui = static_cast<const clap_plugin_gui*>(chainVenue.plugin->get_extension(chainVenue.plugin, CLAP_EXT_GUI));
#if defined(__APPLE__)
	REQUIRE(chainGui->create(chainVenue.plugin, CLAP_WINDOW_API_COCOA, false));
#else
	REQUIRE(chainGui->create(chainVenue.plugin, CLAP_WINDOW_API_X11, false));
#endif
	auto* chainEditor = static_cast<rack::RackEditor*>(chainVenue.test()->editor(chainVenue.plugin));
	REQUIRE(chainEditor != nullptr);
	bool filter = false;
	for (const device::Descriptor* o : chainEditor->offer()) {
		filter = filter || device::sameTag(o->traits.tag, fx::filter::kTraits.tag);
	}
	CHECK(filter);
	chainGui->destroy(chainVenue.plugin);
}

TEST_CASE("note rack: Pattern in Drum passes the kit's notes through (Thru) and plays its lanes; nothing sticks on a stop "
          "or a deactivate, and it never asks to SLEEP") {
	namespace pt = mod::arp::pat;
	Venue v(makeHost(), 512, true);
	const uint32_t id = v.test()->add_device(v.plugin, pt::kTraits.tag);
	REQUIRE(id != 0);
	rack::RackBridge& bridge = bridgeOf(v);
	size_t mode = pt::kDescriptor.params.size();
	for (size_t i = 0; i < pt::kDescriptor.params.size(); ++i) {
		if (std::string(pt::kDescriptor.params[i].name) == "P1 Mode") {
			mode = i;
		}
	}
	REQUIRE(mode < pt::kDescriptor.params.size());
	bridge.setField(id, mode, 5.0f); // Drum: Thru resolves On (pattern_types.h)
	NoteHost h { v };
	h.audio = false;
	REQUIRE(h.block() == CLAP_PROCESS_CONTINUE); // the rack adopts its device at a block
	h.left.clear();
	const int64_t kitAt = h.at + 100;
	NoteStream kit;
	kit.note(CLAP_EVENT_NOTE_ON, 36, 100, 77);
	REQUIRE(h.block(&kit) == CLAP_PROCESS_CONTINUE);
	NoteStream kitOff;
	kitOff.note(CLAP_EVENT_NOTE_OFF, 36, 10, 77);
	const int64_t kitOffAt = h.at + 10;
	REQUIRE(h.block(&kitOff) == CLAP_PROCESS_CONTINUE);
	for (int b = 0; b < 200; ++b) {
		REQUIRE(h.block() == CLAP_PROCESS_CONTINUE); // AN EMITTING NOTE RACK NEVER ASKS TO SLEEP
	}
	// THRU: the kit's note left on its own sample, with its own id.
	const Left on { CLAP_EVENT_NOTE_ON, 36, 77, kitAt };
	const Left off { CLAP_EVENT_NOTE_OFF, 36, 77, kitOffAt };
	CHECK(std::find(h.left.begin(), h.left.end(), on) != h.left.end());
	CHECK(std::find(h.left.begin(), h.left.end(), off) != h.left.end());
	// AND ITS LANES: notes it made itself.
	size_t invented = 0;
	for (const Left& e : h.left) {
		invented += e.type == CLAP_EVENT_NOTE_ON && e.noteId == -1 ? 1 : 0;
	}
	CHECK(invented > 0);
	CHECK(h.ordered);

	SUBCASE("the transport stops: whatever it made is ended") {
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0;
		h.playing = false;
		for (int b = 0; b < 40; ++b) {
			h.block();
		}
		h.checkBalanced();
	}
	SUBCASE("a deactivate: the first block after ends what it had sounding") {
		v.plugin->stop_processing(v.plugin);
		v.plugin->deactivate(v.plugin);
		REQUIRE(v.plugin->activate(v.plugin, 48000.0, 1, 512));
		REQUIRE(v.plugin->start_processing(v.plugin));
		h.playing = false;
		h.stoppedAt = 0.0;
		for (int b = 0; b < 40; ++b) {
			h.block();
		}
		h.checkBalanced();
	}
}

#ifdef FEATHERS_DIAGNOSTIC
TEST_CASE("SR: feathers-chain takes its spike threshold from editor.conf's spike_threshold, and the variable still wins") {
	// FEATHERS.CLAP READ `spike_threshold` FROM editor.conf; feathers-chain read only `$FEATHERS_SPIKE_THRESHOLD` (backlog
	// SR). The real plugin, activated with each threshold, its spikes counted in the
	// file its deactivate drains to. This binary's desk is a scratch one (test_sandbox), so editor.conf is not the user's.
	const std::filesystem::path dir =
	    std::filesystem::temp_directory_path() / ("feathers-chain-spikes-" + std::to_string(::getpid()));
	::setenv("FEATHERS_SPIKE_LOG_DIR", dir.string().c_str(), 1);
	::unsetenv("FEATHERS_SPIKE_THRESHOLD");
	const auto spikesWith = [&](double prefsThreshold) {
		{
			EditorPrefs prefs;
			prefs.load();
			prefs.setNumber("spike_threshold", prefsThreshold);
		}
		std::filesystem::remove_all(dir);
		{
			Venue v;
			for (int b = 0; b < 4; ++b) {
				v.run();
			}
		} // deactivate drains the ring into the day's file
		size_t spikes = 0;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
			std::ifstream in(entry.path());
			for (std::string line; std::getline(in, line);) {
				if (line.find("\"event\":\"spike\"") != std::string::npos
				    && line.find("\"plugin\":\"feathers-chain\"") != std::string::npos) {
					++spikes;
				}
			}
		}
		return spikes;
	};
	// editor.conf ALONE: a threshold of almost nothing records every block; a threshold of ten budgets records none.
	CHECK(spikesWith(1e-6) >= 4);
	CHECK(spikesWith(10.0) == 0);
	// AND THE VARIABLE WINS OVER IT, BOTH WAYS.
	::setenv("FEATHERS_SPIKE_THRESHOLD", "10", 1);
	CHECK(spikesWith(1e-6) == 0);
	::setenv("FEATHERS_SPIKE_THRESHOLD", "1e-6", 1);
	CHECK(spikesWith(10.0) >= 4);
	::unsetenv("FEATHERS_SPIKE_THRESHOLD");
	{
		EditorPrefs prefs;
		prefs.load();
		prefs.setNumber("spike_threshold", 0.5);
	}
	::setenv("FEATHERS_SPIKE_LOG_DIR", "off", 1);
	std::filesystem::remove_all(dir);
}
#endif

TEST_CASE("rack note out: Pattern leaves nothing sounding (it must always send the note off when the transport stops or the preset changes under it)") {
	namespace pt = mod::arp::pat;
	namespace gn = mod::arp::genes;
	Venue v(512);
	const uint32_t id = v.test()->add_device(v.plugin, pt::kTraits.tag);
	REQUIRE(id != 0);
	v.settle();
	rack::RackBridge& bridge = bridgeOf(v);
	const auto& engine = bridge.engineForTest();
	const auto set = [&](size_t index, float value) { bridge.setField(id, index, value); };
	NoteHost h { v };
	const auto start = [&](float mode) {
		set(pt::at(0, pt::Field::Mode), mode);
		set(pt::at(0, pt::Field::Gate), 1.0f); // the longest gate: a note is sounding most of the time
		NoteStream chord;
		chord.on(48).on(55);
		h.block(&chord);
		int b = 0;
		while (engine.inventedHeldForTest() == 0 && b++ < 400) {
			h.block();
		}
		REQUIRE(engine.inventedHeldForTest() > 0);
	};
	const auto ended = [&] {
		for (int b = 0; b < 40; ++b) { // past any step's own gate: an off at its time counts
			h.block();
		}
		CHECK(engine.inventedHeldForTest() == 0);
		h.checkBalanced();
		CHECK(h.ordered);
	};
	SUBCASE("the transport stops (Only w/ Transport, the default)") {
		start(static_cast<float>(gn::Mode::Melody));
		h.playing = false;
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0;
		ended();
	}
	SUBCASE("the transport stops under TB-Glide (one held, bent note)") {
		start(static_cast<float>(gn::Mode::TbGlide));
		h.playing = false;
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0;
		ended();
	}
	SUBCASE("the transport stops in Drum (gate-only kit notes)") {
		start(static_cast<float>(gn::Mode::Drum));
		h.playing = false;
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0;
		ended();
	}
	SUBCASE("a whole-rack patch loads under it (the same patch: a new Pattern takes its place)") {
		start(static_cast<float>(gn::Mode::Melody));
		const rack::RackPatch patch = bridge.patch();
		(void)bridge.loadPatch(patch);
		ended();
	}
	SUBCASE("a Mode change mid-note (Melody to Drum)") {
		start(static_cast<float>(gn::Mode::Melody));
		set(pt::at(0, pt::Field::Mode), static_cast<float>(gn::Mode::Drum));
		NoteStream release;
		release.off(48).off(55);
		h.block(&release);
		for (int b = 0; b < 30; ++b) {
			h.block(); // the Melody note ends at its own time; Drum (Trig: Transport) plays on
		}
		h.playing = false; // and the transport stops the kit
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0;
		ended();
	}
	SUBCASE("switched off (Enabled) mid-note") {
		start(static_cast<float>(gn::Mode::Melody));
		set(pt::at(pt::Global::Enabled), 0.0f);
		ended();
	}
	SUBCASE("the chord released mid-note") {
		start(static_cast<float>(gn::Mode::Melody));
		NoteStream release;
		release.off(48).off(55);
		h.block(&release);
		ended();
	}
	// THRU KEYS ARE THE PLAYER'S, NOT THE PATTERN'S: a stop ends what the Pattern made and leaves a held thru key
	// sounding until its own release. (Thru On in Melody: every key passes, Trig resolves to Transport, so the Pattern
	// plays its idle chord on its own.)
	const auto sounding = [&](int key) {
		bool on = false;
		for (const Left& e : h.left) {
			if (e.key == key) {
				on = e.type == CLAP_EVENT_NOTE_ON;
			}
		}
		return on;
	};
	SUBCASE("the transport stops under held Thru keys: they keep sounding until released") {
		set(pt::at(0, pt::Field::Thru), static_cast<float>(pt::Thru::On));
		start(static_cast<float>(gn::Mode::Melody));
		REQUIRE(sounding(48));
		REQUIRE(sounding(55));
		h.playing = false;
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0;
		for (int b = 0; b < 40; ++b) {
			h.block();
		}
		CHECK(sounding(48)); // still held: no off for it
		CHECK(sounding(55));
		CHECK_FALSE(sounding(60)); // the Pattern's own (idle chord C4) note ended
		NoteStream release;
		release.off(48).off(55);
		h.block(&release);
		ended();
	}
	SUBCASE("a whole-rack patch loads under held Thru keys: they keep sounding, and their release ends them") {
		// THE NEW INSTANCE NEVER SAW THEM PASS, and needs not: Thru names the key and the arp does not hold it, so the
		// player's off passes (`StepEmitter::emit`'s Off). Only what the Pattern MADE is ended at the load.
		set(pt::at(0, pt::Field::Thru), static_cast<float>(pt::Thru::On));
		start(static_cast<float>(gn::Mode::Melody));
		REQUIRE(sounding(48));
		const rack::RackPatch patch = bridge.patch();
		(void)bridge.loadPatch(patch);
		for (int b = 0; b < 40; ++b) {
			h.block();
		}
		CHECK(sounding(48)); // the player's, still held
		CHECK(sounding(55));
		NoteStream release;
		release.off(48).off(55);
		h.block(&release);
		ended();
	}
	SUBCASE("a different emitter replaces it in one edit (the count holds; the instance does not)") {
		start(static_cast<float>(gn::Mode::Melody));
		bridge.removeDevice(id);
		CHECK(v.test()->add_device(v.plugin, mod::arp::kTraits.tag) != 0);
		h.block(); // through the host's queue, not `settle`'s, which drops what the block emits
		if (const auto* timer = static_cast<const clap_plugin_timer_support*>(v.plugin->get_extension(v.plugin, CLAP_EXT_TIMER_SUPPORT))) {
			timer->on_timer(v.plugin, 0);
		}
		ended();
	}
}

TEST_CASE("rack note out: the classic Arp leaves nothing sounding (the same roads as Pattern's)") {
	namespace ap = mod::arp;
	Venue v(512);
	const uint32_t id = v.test()->add_device(v.plugin, ap::kTraits.tag);
	REQUIRE(id != 0);
	v.settle();
	rack::RackBridge& bridge = bridgeOf(v);
	const auto& engine = bridge.engineForTest();
	NoteHost h { v };
	bridge.setField(id, ap::at(ap::Param::Gate), 1.0f);
	NoteStream chord;
	chord.on(48).on(55);
	h.block(&chord);
	int b = 0;
	while (engine.inventedHeldForTest() == 0 && b++ < 400) {
		h.block();
	}
	REQUIRE(engine.inventedHeldForTest() > 0);
	SUBCASE("the transport stops") {
		h.playing = false;
		h.stoppedAt = h.songBeatsAtZero + static_cast<double>(h.at) / 48000.0 * h.bpm / 60.0;
	}
	SUBCASE("a whole-rack patch loads under it") {
		const rack::RackPatch patch = bridge.patch();
		(void)bridge.loadPatch(patch);
	}
	SUBCASE("switched off") {
		bridge.setField(id, ap::at(ap::Param::Enabled), 0.0f);
	}
	for (int i = 0; i < 40; ++i) {
		h.block();
	}
	CHECK(engine.inventedHeldForTest() == 0);
	h.checkBalanced();
	CHECK(h.ordered);
}

TEST_CASE("QN: the Rack plugin's Macros column, folded, is folded again after a save into a FRESH instance — and open is open") {
	// THE FOLD IS PATCH STATE (`RackBridge::macrosCollapsed`, written by the editor's rail and read on its beat): the
	// plugin's own save carries it, a fresh instance loads it, and that instance's editor opens folded, the lane at the strip.
	if (!rack::RackGui::isAvailable()) {
		return;
	}
#if defined(__APPLE__)
	const char* api = CLAP_WINDOW_API_COCOA;
#else
	const char* api = CLAP_WINDOW_API_X11;
#endif
	for (const bool folded : { true, false }) {
		CAPTURE(folded);
		std::vector<char> saved;
		{
			Venue writer;
			const auto* gui = static_cast<const clap_plugin_gui*>(writer.plugin->get_extension(writer.plugin, CLAP_EXT_GUI));
			REQUIRE(gui->create(writer.plugin, api, false));
			REQUIRE(hostSetSize(gui, writer.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));
			auto* editor = static_cast<rack::RackEditor*>(writer.test()->editor(writer.plugin));
			REQUIRE(editor != nullptr);
			(void)editor->setMacrosCollapsed(folded); // the rail's own road, which writes the patch
			auto* bridge = static_cast<rack::RackBridge*>(writer.test()->bridge(writer.plugin));
			REQUIRE(bridge != nullptr);
			CHECK(bridge->macrosCollapsed() == folded);
			saved = saveRackState(writer.plugin);
			gui->destroy(writer.plugin);
		}
		Venue fresh;
		const auto* state = static_cast<const clap_plugin_state*>(fresh.plugin->get_extension(fresh.plugin, CLAP_EXT_STATE));
		ReadStream s(saved);
		REQUIRE(state->load(fresh.plugin, &s.in));
		fresh.settle();
		auto* bridge = static_cast<rack::RackBridge*>(fresh.test()->bridge(fresh.plugin));
		CHECK(bridge->macrosCollapsed() == folded);
		const auto* gui = static_cast<const clap_plugin_gui*>(fresh.plugin->get_extension(fresh.plugin, CLAP_EXT_GUI));
		REQUIRE(gui->create(fresh.plugin, api, false));
		REQUIRE(hostSetSize(gui, fresh.plugin, rack::RackEditor::kWidth, rack::RackEditor::kHeight));
		fresh.settle(); // the beat that applies the patch's fold
		auto* editor = static_cast<rack::RackEditor*>(fresh.test()->editor(fresh.plugin));
		REQUIRE(editor != nullptr);
		CHECK(editor->macrosCollapsed() == folded);
		CHECK(editor->laneLeft()
		      == doctest::Approx(folded ? ui::Rail::kCollapsedWidth + 6.0f : rack::RackEditor::kLaneLeft));
		gui->destroy(fresh.plugin);
	}
}

TEST_CASE("rack note out: Pattern in Keys Transpose follows a new key from the next step — legato or re-struck (backlog QR)") {
	namespace pt = mod::arp::pat;
	constexpr int64_t kStep = 6000; // 1/16 at 120 bpm, 48 kHz
	for (const bool legato : { false, true }) {
		CAPTURE(legato);
		Venue v(512);
		const uint32_t id = v.test()->add_device(v.plugin, pt::kTraits.tag);
		REQUIRE(id != 0);
		v.settle();
		NoteHost h { v };
		NoteStream a;
		a.on(60);
		h.block(&a);
		while (h.at < 32 * kStep) {
			h.block();
		}
		while (h.at < 48 * kStep - kStep / 2) {
			h.block();
		}
		NoteStream change;
		if (legato) {
			change.on(64).off(60);
		}
		else {
			change.off(60).on(64);
		}
		h.block(&change);
		while (h.at < 96 * kStep) {
			h.block();
		}
		NoteStream off;
		off.off(64);
		h.block(&off);
		std::map<int64_t, int> underA;
		std::map<int64_t, int> underB;
		for (const Left& e : h.left) {
			if (e.type != CLAP_EVENT_NOTE_ON || e.sample % kStep != 0) {
				continue;
			}
			const int64_t slot = e.sample / kStep;
			if (slot < 32) {
				underA[slot] = e.key;
			}
			else if (slot >= 48 && slot < 96) {
				underB[slot - 48] = e.key;
			}
		}
		REQUIRE(underA.size() >= 8);
		size_t compared = 0;
		for (const auto& [slot, key] : underB) {
			const auto found = underA.find(slot % 32);
			if (found == underA.end()) {
				continue;
			}
			CAPTURE(slot);
			CHECK(key == found->second + 4);
			++compared;
		}
		CHECK(compared >= 8);
	}
}

TEST_CASE("rack note out: a key passing through when Pattern is added still gets its off out — never a stuck note (backlog QU)") {
	namespace pt = mod::arp::pat;
	for (const int32_t offId : { 7, -1 }) {
		CAPTURE(offId);
		Venue v(512);
		NoteHost h { v };
		NoteStream on;
		on.note(CLAP_EVENT_NOTE_ON, 60, 0, 7);
		h.block(&on); // an empty rack: the key goes straight out
		REQUIRE(h.ons().size() == 1);
		REQUIRE(v.test()->add_device(v.plugin, pt::kTraits.tag) != 0);
		h.block();
		if (const auto* timer = static_cast<const clap_plugin_timer_support*>(v.plugin->get_extension(v.plugin, CLAP_EXT_TIMER_SUPPORT))) {
			timer->on_timer(v.plugin, 0);
		}
		h.playing = false; // Pattern (Run on Transport) plays nothing of its own
		for (int b = 0; b < 4; ++b) {
			h.block();
		}
		NoteStream off;
		off.note(CLAP_EVENT_NOTE_OFF, 60, 0, offId);
		h.block(&off);
		h.checkBalanced();
		size_t offs = 0;
		for (const Left& e : h.left) {
			if (e.type == CLAP_EVENT_NOTE_OFF && e.key == 60) {
				++offs;
				CHECK(e.noteId == 7); // the on's id, whatever the off carried
			}
		}
		CHECK(offs == 1);
	}
}
