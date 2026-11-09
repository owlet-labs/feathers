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

// AN AUDIO ENGINE RESTART MUST NOT COST THE PATCH (seen on a Mac: the plugin spontaneously forgot its
// state, and the only thing that may have happened was a Bitwig audio engine restart).
//
// A restart reaches a plugin as one of a few sequences, every one through the real CLAP entry points here:
//   (a) stop_processing, deactivate, activate (a new rate and block size), start_processing — no state call;
//   (b) the same with the editor open;
//   (c) save, destroy, create, init, load into a fresh instance, then activate;
//   (d) load BEFORE the first activate, or activate before the load.
// Each starts from a patch that is NOT the default — parameters, devices in the lane with their controls
// moved, a leaf device's BODY (Featherverb's IR) loaded from a patch and then EDITED in the running
// instance, renamed macros, pinned host slots — and asserts the whole state afterwards BY VALUE: every
// parameter (value and name), every lane device's every control as the bridge AND the engine hold it, every
// body, every slot; and the saved bytes, as a second assertion. Then a STRESS loop does the same with
// process() running on another thread.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "clap/clap.h"

#include "fx/conv/ir_ref.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/filter/filter_types.h"
#include "fx/resonate/resonate_lanes.h"
#include "fx/resonate/resonate_types.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"
#include "fx/shaper/shaper_types.h"
#include "mod/arp/arp_types.h"
#include "mod/arp/pattern.h"
#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_types.h"
#include "mod/dahdsr/dahdsr_types.h"
#include "mod/lfo/lfo_types.h"
#include "plugin/device_registry.h"
#include "plugin/feathers_test_chain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

extern "C" const clap_plugin_entry clap_entry;

// ONE SOURCE, TWO BINARIES: compiled against Feathers' entry, and again against the standalone rack's with
// FEATHERS_RESTART_RACK=1 (tests/CMakeLists.txt). The sequences and the assertions are the same; only the patch
// differs where the plugins do (the rack has no strip, so its LFO sits in the chain).
#ifndef FEATHERS_RESTART_RACK
#define FEATHERS_RESTART_RACK 0
#endif
#if FEATHERS_RESTART_RACK
#define PLUGIN_NAME "rack "
#else
#define PLUGIN_NAME ""
#endif

namespace {

//------------------------------------------------------------------------------------------------//
// A HOST THAT OFFERS A TIMER, so the editor may be created (Linux refuses an editor to a host with   //
// neither a timer nor fd support), and remembers a request for a main-thread visit.                   //
//------------------------------------------------------------------------------------------------//

std::atomic<bool> gCallbackRequested { false };

bool CLAP_ABI timerRegister(const clap_host*, uint32_t, clap_id* id) {
	*id = 1;
	return true;
}
bool CLAP_ABI timerUnregister(const clap_host*, clap_id) {
	return true;
}
const clap_host_timer_support kTimer { timerRegister, timerUnregister };

const void* CLAP_ABI hostGetExtension(const clap_host*, const char* id) {
	return std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0 ? &kTimer : nullptr;
}
void CLAP_ABI hostRequestRestart(const clap_host*) {}
void CLAP_ABI hostRequestProcess(const clap_host*) {}
void CLAP_ABI hostRequestCallback(const clap_host*) {
	gCallbackRequested.store(true);
}

const clap_host kHost = {
	CLAP_VERSION_INIT, nullptr, "feathers restart tests", "Owlet Records", "", "1.0",
	hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback,
};

constexpr const char* kEmbedApiHere =
#if defined(__APPLE__)
    CLAP_WINDOW_API_COCOA;
#else
    CLAP_WINDOW_API_X11;
#endif

//------------------------------------------------------------------------------------------------//
// STREAMS AND EVENTS                                                                               //
//------------------------------------------------------------------------------------------------//

struct Bytes {
	std::vector<uint8_t> bytes;
	size_t at = 0;
	static int64_t CLAP_ABI writeFn(const clap_ostream* s, const void* buffer, uint64_t size) {
		auto* self = static_cast<Bytes*>(s->ctx);
		const auto* from = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), from, from + size);
		return static_cast<int64_t>(size);
	}
	static int64_t CLAP_ABI readFn(const clap_istream* s, void* buffer, uint64_t size) {
		auto* self = static_cast<Bytes*>(s->ctx);
		const size_t take = std::min<size_t>(static_cast<size_t>(size), self->bytes.size() - self->at);
		std::memcpy(buffer, self->bytes.data() + self->at, take);
		self->at += take;
		return static_cast<int64_t>(take);
	}
	clap_ostream out() { return { this, writeFn }; }
	clap_istream in() {
		at = 0;
		return { this, readFn };
	}
};

struct ParamEvents {
	std::vector<clap_event_param_value> events;
	void set(clap_id id, double value) {
		clap_event_param_value e {};
		e.header.size = sizeof(e);
		e.header.type = CLAP_EVENT_PARAM_VALUE;
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.param_id = id;
		e.note_id = -1;
		e.port_index = -1;
		e.channel = -1;
		e.key = -1;
		e.value = value;
		events.push_back(e);
	}
	static uint32_t CLAP_ABI size(const clap_input_events* list) {
		return static_cast<uint32_t>(static_cast<const ParamEvents*>(list->ctx)->events.size());
	}
	static const clap_event_header* CLAP_ABI get(const clap_input_events* list, uint32_t i) {
		return &static_cast<const ParamEvents*>(list->ctx)->events[i].header;
	}
	clap_input_events input() const { return { const_cast<ParamEvents*>(this), size, get }; }
};

bool CLAP_ABI dropOutput(const clap_output_events*, const clap_event_header*) {
	return true;
}

//------------------------------------------------------------------------------------------------//
// ONE INSTANCE, DRIVEN BY HAND — every CLAP call is the case's to make, in the case's order.        //
//------------------------------------------------------------------------------------------------//

constexpr uint32_t kMaxFramesEver = 2048;

class Instance {
public:
	Instance() {
		REQUIRE(clap_entry.init("feathers-restart-test"));
		const auto* factory =
		    static_cast<const clap_plugin_factory*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
		REQUIRE(factory != nullptr);
		plugin_ = factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
		REQUIRE(plugin_ != nullptr);
		REQUIRE(plugin_->init(plugin_));
		state_ = static_cast<const clap_plugin_state*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
		params_ = static_cast<const clap_plugin_params*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
		chain_ = static_cast<const clap_plugin_feathers_test_chain*>(
		    plugin_->get_extension(plugin_, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(state_ != nullptr);
		REQUIRE(params_ != nullptr);
		REQUIRE(chain_ != nullptr);
		left_.assign(kMaxFramesEver, 0.0f);
		right_.assign(kMaxFramesEver, 0.0f);
	}
	~Instance() {
		if (processing_) {
			plugin_->stop_processing(plugin_);
		}
		if (active_) {
			plugin_->deactivate(plugin_);
		}
		if (gui_ != nullptr) {
			gui_->destroy(plugin_);
		}
		plugin_->destroy(plugin_);
		clap_entry.deinit();
	}
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	const clap_plugin* plugin() const { return plugin_; }
	const clap_plugin_feathers_test_chain* chain() const { return chain_; }
	const clap_plugin_params* params() const { return params_; }

	void activate(double rate, uint32_t maxFrames) {
		REQUIRE(!active_);
		REQUIRE(plugin_->activate(plugin_, rate, 1, maxFrames));
		active_ = true;
		maxFrames_ = maxFrames;
	}
	void deactivate() {
		REQUIRE(active_);
		plugin_->deactivate(plugin_);
		active_ = false;
	}
	void start() {
		REQUIRE(plugin_->start_processing(plugin_));
		processing_ = true;
	}
	void stop() {
		plugin_->stop_processing(plugin_);
		processing_ = false;
	}
	/// The full restart a host's engine makes: (a).
	void restart(double rate, uint32_t maxFrames) {
		stop();
		deactivate();
		activate(rate, maxFrames);
		start();
	}
	/// One block of `maxFrames_`, with whatever parameter events. Audio thread (or the main thread with none).
	clap_process_status run(const ParamEvents* events = nullptr) {
		const ParamEvents none;
		return runWith((events != nullptr ? *events : none).input());
	}
	/// One block with any input list — a case's own notes, say.
	clap_process_status runWith(const clap_input_events in) {
		const clap_output_events out { nullptr, dropOutput };
		float* channels[2] = { left_.data(), right_.data() };
		clap_audio_buffer output {};
		output.data32 = channels;
		output.channel_count = 2;
		clap_process process {};
		process.steady_time = steady_;
		process.frames_count = maxFrames_;
		process.audio_outputs = &output;
		process.audio_outputs_count = 1;
		process.in_events = &in;
		process.out_events = &out;
		// A HOST TRANSPORT, when a case asks for one (`transport`): its song position advances with each block.
		clap_event_transport transport {};
		if (transport_.has) {
			transport.header.size = sizeof(transport);
			transport.header.type = CLAP_EVENT_TRANSPORT;
			transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE
			                  | (transport_.playing ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u);
			transport.tempo = transport_.bpm;
			transport.song_pos_beats = static_cast<clap_beattime>(std::llround(transport_.beats * CLAP_BEATTIME_FACTOR));
			process.transport = &transport;
			if (transport_.playing) {
				transport_.beats += static_cast<double>(maxFrames_) / 48000.0 * transport_.bpm / 60.0;
			}
		}
		steady_ += maxFrames_;
		return plugin_->process(plugin_, &process);
	}
	/// A main-thread visit, if the plugin asked for one — what a host does with `request_callback`.
	void pumpMainThread() {
		if (gCallbackRequested.exchange(false)) {
			plugin_->on_main_thread(plugin_);
		}
	}
	void openEditor() {
		gui_ = static_cast<const clap_plugin_gui*>(plugin_->get_extension(plugin_, CLAP_EXT_GUI));
		REQUIRE(gui_ != nullptr);
		REQUIRE(gui_->create(plugin_, kEmbedApiHere, false));
	}
	void closeEditor() {
		gui_->destroy(plugin_);
		gui_ = nullptr;
	}

	Bytes save() const {
		Bytes b;
		clap_ostream out = b.out();
		REQUIRE(state_->save(plugin_, &out));
		return b;
	}
	bool load(Bytes& b) const {
		clap_istream in = b.in();
		return state_->load(plugin_, &in);
	}

private:
	const clap_plugin* plugin_ = nullptr;
	const clap_plugin_state* state_ = nullptr;
	const clap_plugin_params* params_ = nullptr;
	const clap_plugin_feathers_test_chain* chain_ = nullptr;
	const clap_plugin_gui* gui_ = nullptr;
	std::vector<float> left_, right_;
	uint32_t maxFrames_ = 512;
	int64_t steady_ = 0;

public:
	/// THE HOST'S TRANSPORT, from the next block on: playing or stopped, at a tempo (48 kHz is assumed for its advance).
	void transport(bool playing, double bpm = 120.0) {
		transport_.has = true;
		transport_.playing = playing;
		transport_.bpm = bpm;
	}

private:
	struct {
		bool has = false;
		bool playing = false;
		double bpm = 120.0;
		double beats = 0.0;
	} transport_;
	bool active_ = false;
	bool processing_ = false;
};

//------------------------------------------------------------------------------------------------//
// THE NON-DEFAULT PATCH                                                                            //
//------------------------------------------------------------------------------------------------//

/// One device the patch holds: its test position (the lane's, or the strip's with `CLAP_FEATHERS_TEST_STRIP`)
/// and its tag.
struct Dev {
	uint32_t position;
	const char* tag;
};

/// What the patch holds after `buildPatch`: the lane's default filter, a Featherverb, a Shaper, and in the
/// modulator strip the default envelope and an LFO — every one with its controls moved.
std::vector<Dev> gDevices = {
	{ 0, feathers::fx::filter::kTraits.tag },
	{ 1, feathers::fx::featherverb::kTraits.tag },
	{ 2, feathers::fx::shaper::kTraits.tag },
};
constexpr uint32_t kVerbAt = 1;

/// A Featherverb body naming generated room `room` with a stretch — a body no fresh device has.
std::vector<uint8_t> verbBody(uint8_t room, float stretch) {
	feathers::fx::conv::IrRef ref;
	ref.generated = room;
	ref.shape.stretch = stretch;
	ref.shape.begin = 0.125f;
	return feathers::fx::conv::encodeIrRef(ref);
}

/// Every control of every lane device moved off its default, deterministically.
void moveEveryControl(Instance& inst, double fraction) {
	for (const Dev& dev : gDevices) {
		const uint32_t pos = dev.position;
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dev.tag);
		REQUIRE(desc != nullptr);
		for (size_t i = 0; i < desc->params.size(); ++i) {
			const auto& p = desc->params[i];
			if (p.name == nullptr || !(p.max > p.min)) {
				continue;
			}
			double v = p.min + fraction * (p.max - p.min);
			if (v == p.def) {
				v = p.min + (fraction * 0.5) * (p.max - p.min);
			}
			(void)inst.chain()->set_control(inst.plugin(), pos, p.name, v);
		}
	}
}

/// THE PATCH EVERY CASE STARTS FROM, built in `inst` (active and processing), then SAVED AND LOADED BACK so
/// the bodies came from a load — as a real project's do — and only THEN edited in the running instances: the
/// Featherverb's IR picked again, a control moved. That edit after the load lives only in the running
/// Featherverb, which is the state an engine restart must not lose.
void buildPatch(Instance& inst) {
#if FEATHERS_RESTART_RACK
	// THE RACK STARTS EMPTY: the same three devices at the same positions, then an LFO in the chain itself.
	for (size_t i = 0; i < 3; ++i) {
		REQUIRE(inst.chain()->add_device(inst.plugin(), gDevices[i].tag) == i);
	}
	const uint32_t lfo = inst.chain()->add_device(inst.plugin(), feathers::mod::lfo::kTraits.tag);
	REQUIRE(lfo == 3);
	if (gDevices.size() == 3) {
		gDevices.push_back({ lfo, feathers::mod::lfo::kTraits.tag });
	}
#else
	for (size_t i = 1; i < 3; ++i) {
		REQUIRE(inst.chain()->add_device(inst.plugin(), gDevices[i].tag) == i);
	}
	// THE STRIP: its default envelope (slot 0) and an LFO, both with their controls moved below.
	const uint32_t lfo = inst.chain()->add_mod_device(inst.plugin(), feathers::mod::lfo::kTraits.tag);
	REQUIRE(lfo != UINT32_MAX);
	if (gDevices.size() == 3) {
		gDevices.push_back({ CLAP_FEATHERS_TEST_STRIP | 0u, feathers::mod::dahdsr::kTraits.tag });
		gDevices.push_back({ CLAP_FEATHERS_TEST_STRIP | lfo, feathers::mod::lfo::kTraits.tag });
	}
#endif
	moveEveryControl(inst, 0.31);
	// PARAMETERS: a spread of the table, each to a third of its range.
	ParamEvents events;
	const uint32_t n = inst.params()->count(inst.plugin());
	for (uint32_t i = 0; i < n; i += 7) {
		clap_param_info info {};
		REQUIRE(inst.params()->get_info(inst.plugin(), i, &info));
		if (info.max_value > info.min_value && (info.flags & CLAP_PARAM_IS_READONLY) == 0) {
			events.set(info.id, info.min_value + 0.33 * (info.max_value - info.min_value));
		}
	}
	(void)inst.run(&events);
	REQUIRE(inst.chain()->rename_macro(inst.plugin(), 0, "Brightness"));
	REQUIRE(inst.chain()->rename_macro(inst.plugin(), 2, "Space"));
	REQUIRE(inst.chain()->pin_slot(inst.plugin(), 2, "Shaper Drive") != UINT32_MAX);
	REQUIRE(inst.chain()->pin_slot(inst.plugin(), kVerbAt, "Featherverb Mix") != UINT32_MAX);
	inst.pumpMainThread();
	// A DEVICE ADDED WITH NO EDITOR OPEN IS BUILT AT THE NEXT LOAD OR ACTIVATE (nothing else provisions without
	// an editor): a save and a load here builds them, so the Featherverb has a running instance to edit.
	{
		Bytes first = inst.save();
		REQUIRE(inst.load(first));
		(void)inst.run();
	}
	const std::vector<uint8_t> loaded = verbBody(2, 1.5f);
	REQUIRE(inst.chain()->edit_device_body(inst.plugin(), kVerbAt, loaded.data(), loaded.size()));
	(void)inst.run();

	// AND AGAIN, so the body it now holds is one that CAME FROM A LOAD — as a project's does.
	Bytes saved = inst.save();
	REQUIRE(inst.load(saved));
	(void)inst.run();
	(void)inst.run();

	// AFTER THE LOAD: the edits that live only in running instances, and one more control.
	const std::vector<uint8_t> edited = verbBody(3, 2.25f);
	REQUIRE(inst.chain()->edit_device_body(inst.plugin(), kVerbAt, edited.data(), edited.size()));
	REQUIRE(inst.chain()->set_control(inst.plugin(), 2, "Shaper Drive", 0.77));
	(void)inst.run();
	(void)inst.run();
}

//------------------------------------------------------------------------------------------------//
// THE WHOLE STATE, BY VALUE                                                                         //
//------------------------------------------------------------------------------------------------//

struct Snapshot {
	std::vector<std::tuple<clap_id, std::string, double>> params;
	std::vector<std::vector<uint8_t>> bodies;
	std::vector<std::vector<double>> knobs;
	std::vector<std::vector<double>> engine;
	std::vector<uint32_t> slots;
	uint32_t devices = 0;
	std::vector<uint8_t> saved;
};

/// Taken with no block in flight. `engine` is the ENGINE's base for each control — what the rebuilt device runs.
Snapshot snapshot(Instance& inst) {
	Snapshot s;
	const uint32_t n = inst.params()->count(inst.plugin());
	for (uint32_t i = 0; i < n; ++i) {
		clap_param_info info {};
		REQUIRE(inst.params()->get_info(inst.plugin(), i, &info));
		double value = 0.0;
		REQUIRE(inst.params()->get_value(inst.plugin(), info.id, &value));
		s.params.emplace_back(info.id, info.name, value);
	}
	s.devices = inst.chain()->device_count(inst.plugin());
	for (const Dev& dev : gDevices) {
		const uint32_t pos = dev.position;
		Bytes body;
		clap_ostream out = body.out();
		(void)inst.chain()->device_body(inst.plugin(), pos, &out);
		s.bodies.push_back(body.bytes);
		const feathers::device::Descriptor* desc = feathers::deviceForTag(dev.tag);
		std::vector<double> knobs;
		std::vector<double> engine;
		for (size_t i = 0; i < desc->params.size(); ++i) {
			double knob = -12345.0;
			double base = -12345.0;
			double resolved = 0.0;
			(void)inst.chain()->knob_value(inst.plugin(), pos, desc->params[i].name, &knob);
			(void)inst.chain()->engine_control(inst.plugin(), pos, desc->params[i].name, &base, &resolved);
			knobs.push_back(knob);
			engine.push_back(base);
		}
		s.knobs.push_back(knobs);
		s.engine.push_back(engine);
	}
	for (uint32_t slot = 0; slot < 8; ++slot) {
		s.slots.push_back(inst.chain()->slot_state(inst.plugin(), slot));
	}
	s.saved = inst.save().bytes;
	return s;
}

/// Every value, each named on failure — then the bytes, as the second assertion.
void requireSame(const Snapshot& before, const Snapshot& after) {
	REQUIRE(after.params.size() == before.params.size());
	for (size_t i = 0; i < before.params.size(); ++i) {
		INFO("param " << std::get<0>(before.params[i]) << " '" << std::get<1>(before.params[i]) << "'");
		CHECK(std::get<0>(after.params[i]) == std::get<0>(before.params[i]));
		CHECK(std::get<1>(after.params[i]) == std::get<1>(before.params[i]));
		CHECK(std::get<2>(after.params[i]) == std::get<2>(before.params[i]));
	}
	CHECK(after.devices == before.devices);
	for (size_t pos = 0; pos < before.bodies.size(); ++pos) {
		INFO("device at position " << gDevices[pos].position << " " << std::string(gDevices[pos].tag));
		CHECK(after.bodies[pos] == before.bodies[pos]);
		const feathers::device::Descriptor* desc = feathers::deviceForTag(gDevices[pos].tag);
		for (size_t i = 0; i < before.knobs[pos].size(); ++i) {
			INFO("control '" << desc->params[i].name << "'");
			CHECK(after.knobs[pos][i] == before.knobs[pos][i]);
			CHECK(after.engine[pos][i] == before.engine[pos][i]);
		}
	}
	CHECK(after.slots == before.slots);
	CHECK(after.saved == before.saved);
}

/// The patch is not the default one — so a restart that put back defaults could not pass by accident.
void requireNotDefault(const Snapshot& s) {
	Instance fresh;
	fresh.activate(48000.0, 512);
	fresh.start();
	(void)fresh.run();
	const Snapshot d = snapshot(fresh);
	REQUIRE(s.saved != d.saved);
	REQUIRE(s.devices == (FEATHERS_RESTART_RACK ? 4u : 3u));
	REQUIRE(s.bodies[kVerbAt] == verbBody(3, 2.25f));
	bool renamed = false;
	for (const auto& p : s.params) {
		renamed = renamed || std::get<1>(p) == "Brightness";
	}
	REQUIRE(renamed);
}

} // namespace

TEST_CASE(PLUGIN_NAME "restart (a): stop, deactivate, activate at 44.1k and a new block size, start — the whole patch survives") {
	Instance inst;
	inst.activate(48000.0, 512);
	inst.start();
	buildPatch(inst);
	const Snapshot before = snapshot(inst);
	requireNotDefault(before);

	inst.restart(44100.0, 256);
	(void)inst.run();
	(void)inst.run();
	requireSame(before, snapshot(inst));

	// AND BACK, at a third block size: two restarts in a row.
	inst.restart(48000.0, 1024);
	(void)inst.run();
	requireSame(before, snapshot(inst));
}

TEST_CASE(PLUGIN_NAME "GO: activate hands the host's longest block to the engine by value; at the SAME rate a longer "
                      "one is handed on, and a shorter one is grow-only in Feathers and handed on in the rack") {
	// **BACKLOG GO.** `max_frames_count` is the number every device sizes its scratch to (`AudioOps::prepare`). The
	// later activates keep the rate — the road a latency restart takes, which keeps the voices — so a LONGER block
	// must not be mistaken for that road, or the devices keep scratch sized to the old buffer.
	//
	// **AND A SHORTER ONE DIFFERS BY PLUGIN, ON PURPOSE.** Feathers' lane is grow-only at a kept rate (by
	// decision, `Synth::activate`): its devices' scratch already fits, and re-preparing would cut their tails under
	// voices it keeps. The standalone rack re-prepares its whole engine on every activate, as it always has.
	Instance inst;
	REQUIRE(inst.chain()->max_frames != nullptr);
	REQUIRE(inst.chain()->prepare_count != nullptr);
	CHECK(inst.chain()->max_frames(inst.plugin()) == static_cast<uint32_t>(feathers::device::kDefaultMaxFrames)); // no host yet
	inst.activate(48000.0, 333);
	CHECK(inst.chain()->max_frames(inst.plugin()) == 333u);
	inst.start();
	(void)inst.run();
	inst.restart(48000.0, 777);
	CHECK(inst.chain()->max_frames(inst.plugin()) == 777u);
	(void)inst.run();
	const uint64_t prepared = inst.chain()->prepare_count(inst.plugin());
	inst.restart(48000.0, 256);
	CHECK(inst.chain()->max_frames(inst.plugin()) == (FEATHERS_RESTART_RACK ? 256u : 777u));
	CHECK(inst.chain()->prepare_count(inst.plugin()) == prepared + (FEATHERS_RESTART_RACK ? 1u : 0u));
	(void)inst.run();
}

TEST_CASE(PLUGIN_NAME "restart (b): the same with the editor open") {
#if defined(__linux__)
	if (std::getenv("DISPLAY") == nullptr) {
		MESSAGE("skipped: no DISPLAY — run under scripts/venue-xvfb.sh (ctest -R engine-restart)");
		return;
	}
#endif
	Instance inst;
	inst.activate(48000.0, 512);
	inst.start();
	buildPatch(inst);
	inst.openEditor();
	(void)inst.run();
	inst.pumpMainThread();
	const Snapshot before = snapshot(inst);
	requireNotDefault(before);

	inst.restart(44100.0, 256);
	(void)inst.run();
	inst.pumpMainThread();
	(void)inst.run();
	requireSame(before, snapshot(inst));
	inst.closeEditor();
	requireSame(before, snapshot(inst));
}

TEST_CASE(PLUGIN_NAME "restart (c): save, destroy, create, init, load into a fresh instance, then activate") {
	Snapshot before;
	Bytes saved;
	{
		Instance inst;
		inst.activate(48000.0, 512);
		inst.start();
		buildPatch(inst);
		before = snapshot(inst);
		requireNotDefault(before);
		saved = inst.save();
	}
	Instance fresh;
	REQUIRE(fresh.load(saved));
	fresh.activate(44100.0, 256);
	fresh.start();
	(void)fresh.run();
	(void)fresh.run();
	requireSame(before, snapshot(fresh));
}

TEST_CASE(PLUGIN_NAME "restart (d): a load before the first activate and a load after it land the same patch") {
	Snapshot before;
	Bytes saved;
	{
		Instance inst;
		inst.activate(48000.0, 512);
		inst.start();
		buildPatch(inst);
		before = snapshot(inst);
		saved = inst.save();
	}
	SUBCASE("load, then activate") {
		Instance fresh;
		REQUIRE(fresh.load(saved));
		fresh.activate(48000.0, 512);
		fresh.start();
		(void)fresh.run();
		requireSame(before, snapshot(fresh));
	}
	SUBCASE("activate, then load, with blocks either side") {
		Instance fresh;
		fresh.activate(44100.0, 256);
		fresh.start();
		(void)fresh.run();
		REQUIRE(fresh.load(saved));
		(void)fresh.run();
		(void)fresh.run();
		requireSame(before, snapshot(fresh));
	}
	SUBCASE("load while inactive between two activations") {
		Instance fresh;
		fresh.activate(48000.0, 512);
		fresh.start();
		(void)fresh.run();
		fresh.stop();
		fresh.deactivate();
		REQUIRE(fresh.load(saved));
		fresh.activate(44100.0, 256);
		fresh.start();
		(void)fresh.run();
		requireSame(before, snapshot(fresh));
	}
	SUBCASE("load while inactive, an edit in the device's own editor, then activate: the edit survives") {
		// THE WINDOW A RESTART OPENS: with the engine stopped, a load BUILDS its devices (main thread) but no
		// block adopts them, so they wait in the mailbox — and activate DROPS what waits there, to build it again
		// at the new rate, from the STORED body. An edit made in the waiting instance (its handle is filed at
		// build, so an open panel reaches it at once) lived only there.
		Instance fresh;
		fresh.activate(48000.0, 512);
		fresh.start();
		(void)fresh.run();
		fresh.stop();
		fresh.deactivate();
		Bytes older;
		{
			// THE SAME PROJECT WITH THE EARLIER IR, so the edit below is the one that makes it the patch.
			Instance writer;
			writer.activate(48000.0, 512);
			writer.start();
			REQUIRE(writer.load(saved));
			(void)writer.run();
			const std::vector<uint8_t> old = verbBody(2, 1.5f);
			REQUIRE(writer.chain()->edit_device_body(writer.plugin(), kVerbAt, old.data(), old.size()));
			older = writer.save();
		}
		REQUIRE(fresh.load(older));
		const std::vector<uint8_t> edited = verbBody(3, 2.25f);
		REQUIRE(fresh.chain()->edit_device_body(fresh.plugin(), kVerbAt, edited.data(), edited.size()));
		fresh.activate(44100.0, 256);
		fresh.start();
		(void)fresh.run();
		(void)fresh.run();
		requireSame(before, snapshot(fresh));
	}
}

TEST_CASE(PLUGIN_NAME "restart stress: process() on its own thread while the main thread saves, loads and restarts — every save round-trips") {
	Instance inst;
	inst.activate(48000.0, 512);
	inst.start();
	buildPatch(inst);
	const Snapshot before = snapshot(inst);
	Bytes reference;
	reference.bytes = before.saved;
	// THE SAME PATCH WITH THE FEATHERVERB'S EARLIER IR — what the project file holds before the edit the loop makes.
	Bytes stale;
	{
		const std::vector<uint8_t> old = verbBody(2, 1.5f);
		REQUIRE(inst.chain()->edit_device_body(inst.plugin(), kVerbAt, old.data(), old.size()));
		stale = inst.save();
		const std::vector<uint8_t> edited = verbBody(3, 2.25f);
		REQUIRE(inst.chain()->edit_device_body(inst.plugin(), kVerbAt, edited.data(), edited.size()));
		REQUIRE(inst.save().bytes == reference.bytes);
		REQUIRE(stale.bytes != reference.bytes);
	}

	// THE AUDIO THREAD: blocks for as long as it is allowed to; on `pause` it stops processing (its own call,
	// `[audio-thread]`), says so, and waits for `resume` to start again (also its own call).
	std::atomic<bool> quit { false };
	std::atomic<bool> pause { false };
	std::atomic<bool> paused { false };
	std::atomic<long> blocks { 0 };
	std::thread audio([&] {
		while (!quit.load()) {
			if (pause.load()) {
				inst.stop();
				paused.store(true);
				while (pause.load() && !quit.load()) {
					std::this_thread::yield();
				}
				if (quit.load()) {
					break;
				}
				inst.start();
				paused.store(false);
				continue;
			}
			(void)inst.run();
			blocks.fetch_add(1);
		}
	});
	auto stopAudio = [&] {
		pause.store(true);
		while (!paused.load()) {
			std::this_thread::yield();
		}
	};
	// AND WAIT FOR THE AUDIO THREAD TO HAVE STARTED AGAIN, so the next pause cannot read the previous "paused".
	auto resumeAudio = [&] {
		pause.store(false);
		while (paused.load()) {
			std::this_thread::yield();
		}
	};

	std::mt19937 rng(20261003);
	const double rates[] = { 44100.0, 48000.0, 96000.0 };
	const uint32_t sizes[] = { 64, 256, 512, 1024 };
	// EACH LOAD AND EACH IR EDIT REBUILDS THE FEATHERVERB'S GENERATED ROOM, a kernel to compute: 1-3 s an
	// iteration on this shared host, so 25 keeps the case inside run-cases's 120 s bound.
	// `FEATHERS_RESTART_ITERATIONS=300` runs the long soak by hand.
	const int kIterations = std::getenv("FEATHERS_RESTART_ITERATIONS") != nullptr ? std::atoi(std::getenv("FEATHERS_RESTART_ITERATIONS")) : 25;
	int differing = 0;
	int falseSaves = 0;
	int falseLoads = 0;
	for (int it = 0; it < kIterations; ++it) {
		const int move = static_cast<int>(rng() % 4);
		if (move == 0 || move == 3) {
			// A RESTART: the audio thread stops processing, the main thread deactivates and activates.
			stopAudio();
			inst.deactivate();
			inst.activate(rates[rng() % 3], sizes[rng() % 4]);
			resumeAudio();
		}
		if (move == 1 || move == 3) {
			// A LOAD OF THE PATCH ITSELF, while blocks run — state.load is [main-thread], process may run.
			// THE PROJECT AS IT WAS SAVED, THEN THE EDIT MADE SINCE: the Featherverb's IR picked again in its running
			// instance, so the stored body is the OLD one and only the instance holds the patch — the state a restart
			// must not lose.
			if (!inst.load(stale)) {
				++falseLoads;
			}
			const std::vector<uint8_t> edited = verbBody(3, 2.25f);
			if (!inst.chain()->edit_device_body(inst.plugin(), kVerbAt, edited.data(), edited.size())) {
				++falseLoads;
			}
		}
		// SOMETIMES A PAUSE FIRST, SO THE SAVE LANDS AT A DIFFERENT POINT IN THE AUDIO THREAD'S BLOCK.
		if (rng() % 2 == 0) {
			std::this_thread::sleep_for(std::chrono::microseconds(rng() % 2000));
		}
		Bytes now;
		clap_ostream out = now.out();
		const auto* state = static_cast<const clap_plugin_state*>(inst.plugin()->get_extension(inst.plugin(), CLAP_EXT_STATE));
		if (!state->save(inst.plugin(), &out)) {
			++falseSaves;
			continue;
		}
		if (now.bytes != reference.bytes) {
			++differing;
			if (differing <= 3) {
				MESSAGE("iteration " << it << " (move " << move << "): the save differs — " << now.bytes.size()
				                     << " bytes against " << reference.bytes.size());
			}
		}
	}
	quit.store(true);
	pause.store(false);
	audio.join();
	MESSAGE(kIterations << " iterations, " << blocks.load() << " blocks on the audio thread");
	CHECK(falseSaves == 0);
	CHECK(falseLoads == 0);
	CHECK(differing == 0);
	// AND THE WHOLE STATE AT THE END, BY VALUE, with no block in flight.
	(void)inst.run();
	(void)inst.run();
	requireSame(before, snapshot(inst));
}

TEST_CASE(PLUGIN_NAME "a switched-off Phaser/Flanger stays off with the editor open and ticking: a restart, then save while deactivated and a fresh instance loaded before activate, editor reopened") {
	// Each time the audio engine was turned off and on, a disabled Phaser/Flanger re-enabled itself
	// (Bitwig's "terminate process" engine stop dropping an edit it had not fetched yet; test_clap_render holds the
	// plugin's half on every road without an editor). This is the same with the editor's timer and the main-thread
	// visits running, as they do while the card is clicked.
#if defined(__linux__)
	if (std::getenv("DISPLAY") == nullptr) {
		MESSAGE("skipped: no DISPLAY — run under scripts/venue-xvfb.sh (ctest -R engine-restart)");
		return;
	}
#endif
	const auto tick = [](Instance& inst, int n) {
		const auto* timer = static_cast<const clap_plugin_timer_support*>(
		    inst.plugin()->get_extension(inst.plugin(), CLAP_EXT_TIMER_SUPPORT));
		REQUIRE(timer != nullptr);
		for (int i = 0; i < n; ++i) {
			(void)inst.run();
			timer->on_timer(inst.plugin(), 1);
			inst.plugin()->on_main_thread(inst.plugin());
		}
	};
	const auto requireOff = [](Instance& inst, uint32_t at) {
		double knob = -1.0, base = -1.0;
		REQUIRE(inst.chain()->knob_value(inst.plugin(), at, "Phaser Enabled", &knob));
		REQUIRE(inst.chain()->engine_control(inst.plugin(), at, "Phaser Enabled", &base, nullptr));
		CHECK(knob == 0.0);
		CHECK(base == 0.0);
	};
	Bytes saved;
	uint32_t at = UINT32_MAX;
	{
		Instance inst;
		inst.activate(44100.0, 1024); // Bitwig's own rate and block on the host where it was seen
		inst.start();
		inst.openEditor();
		at = inst.chain()->add_device(inst.plugin(), "PHFL");
		REQUIRE(at != UINT32_MAX);
		tick(inst, 10);
		const double flanger = 1.0, off = 0.0;
		REQUIRE(inst.chain()->drag_control(inst.plugin(), at, "Phaser Mode", &flanger, 1));
		tick(inst, 10);
		REQUIRE(inst.chain()->drag_control(inst.plugin(), at, "Phaser Enabled", &off, 1));
		tick(inst, 10);
		// THE ENGINE ADOPTS A DEVICE ADDED WHILE ACTIVE AT THE NEXT ACTIVATE (assumption L5), so it is read after it.
		inst.restart(44100.0, 1024);
		tick(inst, 10);
		requireOff(inst, at);
		inst.stop();
		inst.deactivate();
		saved = inst.save();
		inst.closeEditor();
	}
	Instance fresh;
	REQUIRE(fresh.load(saved));
	fresh.activate(44100.0, 1024);
	fresh.start();
	fresh.openEditor();
	tick(fresh, 30);
	requireOff(fresh, at);
	fresh.closeEditor();
}

namespace {

/// RESONATE'S BODY WITH A FEATHERVERB IN ITS DRONE FX LANE AND A BUS UTILITY IN WET FX — through the device's own
/// container ops, as its panel's lane editors leave it.
std::vector<uint8_t> resonateLanesBody() {
	namespace rsn = feathers::fx::resonate;
	const feathers::device::AudioOps& audio = *rsn::kDescriptor.audio;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	rsn::kDescriptor.container->adopt(mem, feathers::hostCatalogue());
	audio.prepare(mem, 48000.0, 4096);
	REQUIRE(rsn::laneChainOf(mem, rsn::Lane::Drone)->bridge()->addDevice(feathers::fx::featherverb::kTraits.tag)
	        != feathers::kNoInstance);
	REQUIRE(rsn::laneChainOf(mem, rsn::Lane::Wet)->bridge()->addDevice("busu") != feathers::kNoInstance);
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

} // namespace

TEST_CASE(PLUGIN_NAME "Resonate's fx lanes survive a restart and a save into a fresh instance, body for body") {
	// THE CONTAINER BODY OF THE THIRD HOLDER (backlog LC: "test_engine_restart already covers SKLITTER; extend it to
	// the new holder"). The lanes' contents are an edit in the running instance, as a lane editor makes them.
	const std::vector<uint8_t> lanes = resonateLanesBody();
	Instance inst;
	inst.activate(48000.0, 512);
	inst.start();
	const uint32_t at = inst.chain()->add_device(inst.plugin(), feathers::fx::resonate::kTraits.tag);
	REQUIRE(at != UINT32_MAX);
	inst.restart(48000.0, 512); // ACTIVATE PROVISIONS what the bridge holds (the road a loaded patch takes)
	(void)inst.run();
	REQUIRE(inst.chain()->edit_device_body(inst.plugin(), at, lanes.data(), lanes.size()));
	(void)inst.run();
	const auto bodyOf = [&](const Instance& i) {
		Bytes b;
		clap_ostream out = b.out();
		REQUIRE(i.chain()->device_body(i.plugin(), at, &out));
		return b.bytes;
	};
	const std::vector<uint8_t> before = bodyOf(inst);
	CHECK(before == lanes); // non-vacuity: the edit is what the instance now holds

	inst.restart(44100.0, 256);
	(void)inst.run();
	(void)inst.run();
	CHECK(bodyOf(inst) == before);

	Bytes saved = inst.save();
	Instance fresh;
	REQUIRE(fresh.load(saved));
	fresh.activate(48000.0, 512);
	fresh.start();
	(void)fresh.run();
	CHECK(bodyOf(fresh) == before);
}

#if !FEATHERS_RESTART_RACK
namespace {

/// A CASE'S OWN NOTES, as a host sends them: CLAP note events at frame offsets in one block.
struct NoteList {
	std::vector<clap_event_note> events;
	void push(uint16_t type, int16_t key) {
		clap_event_note e {};
		e.header.size = sizeof(e);
		e.header.time = 0;
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = type;
		e.note_id = key;
		e.port_index = 0;
		e.channel = 0;
		e.key = key;
		e.velocity = 0.75;
		events.push_back(e);
	}
	static uint32_t CLAP_ABI size(const clap_input_events* list) {
		return static_cast<uint32_t>(static_cast<const NoteList*>(list->ctx)->events.size());
	}
	static const clap_event_header* CLAP_ABI get(const clap_input_events* list, uint32_t i) {
		return &static_cast<const NoteList*>(list->ctx)->events[i].header;
	}
	clap_input_events input() const { return { const_cast<NoteList*>(this), size, get }; }
};

/// THE ONS THE VOICES WERE HANDED, (sample, key), read through the plugin's own seam (`delivered_note`).
std::vector<std::pair<int64_t, int>> onsOf(const Instance& inst) {
	std::vector<std::pair<int64_t, int>> ons;
	uint32_t kind = 0;
	int32_t key = 0;
	uint64_t sample = 0;
	for (uint32_t i = 0; inst.chain()->delivered_note(inst.plugin(), i, &kind, &key, &sample); ++i) {
		if (kind == 0) {
			ons.emplace_back(static_cast<int64_t>(sample), key);
		}
	}
	return ons;
}

} // namespace

TEST_CASE("the Arp's latched chord and free clock survive a WHOLE-PLUGIN save into a fresh instance: it plays on with the "
          "same notes at the same samples, within a render chunk") {
	// What cannot be derived — Latch's chord and the free clock's phase — rides in the
	// arp's `arp_play` block, harvested into the plugin's state. The transport is stopped (no transport at all), so the
	// clock is the arp's own free one: 8 steps a second, a step every 6000 samples, from the first key.
	constexpr uint32_t kBlock = 32;
	namespace ap = feathers::mod::arp;
	Instance inst;
	const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), ap::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
	REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Latch", 1.0));
	REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Run", 0.0)); // ON by default; this case free-runs
	REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Time Base", 0.0)); // Hz
	REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Rate", 3.0));      // 2^3 = 8 steps a second
	inst.activate(48000.0, kBlock);
	inst.start();
	int64_t rendered = 0;
	const auto block = [&](Instance& i, const NoteList* notes = nullptr) {
		const NoteList none;
		(void)i.runWith((notes != nullptr ? *notes : none).input());
		rendered += kBlock;
	};
	block(inst);
	REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	const int64_t press = rendered;
	NoteList chord;
	chord.push(CLAP_EVENT_NOTE_ON, 64);
	chord.push(CLAP_EVENT_NOTE_ON, 60);
	chord.push(CLAP_EVENT_NOTE_ON, 67);
	block(inst, &chord);
	NoteList release;
	release.push(CLAP_EVENT_NOTE_OFF, 64);
	release.push(CLAP_EVENT_NOTE_OFF, 60);
	release.push(CLAP_EVENT_NOTE_OFF, 67);
	block(inst, &release); // LATCHED: it plays on with nothing held
	while (rendered < press + 6000 * 2 + 1500) {
		block(inst); // mid-way into the third step: the free phase is 2.25 steps
	}
	const std::vector<std::pair<int64_t, int>> before = onsOf(inst);
	REQUIRE(before.size() == 3);
	CHECK(before[0] == std::pair<int64_t, int> { press, 60 }); // non-vacuity: the free clock started at the press
	CHECK(before[2] == std::pair<int64_t, int> { press + 12000, 67 });

	Bytes saved = inst.save();
	const int64_t savedAt = rendered;
	{
		// THE BLOCK IS IN THE BYTES, so a red below is the restore and not the save.
		const std::string key = "arp_play";
		CHECK(std::search(saved.bytes.begin(), saved.bytes.end(), key.begin(), key.end()) != saved.bytes.end());
	}
	REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	constexpr int kBlocksAfter = 1200; // 38400 samples: six more steps
	for (int b = 0; b < kBlocksAfter; ++b) {
		block(inst);
	}
	std::vector<std::pair<int64_t, int>> straight = onsOf(inst);
	for (auto& [sample, key] : straight) {
		sample -= savedAt;
	}
	REQUIRE(straight.size() == 6);

	// THE FRESH INSTANCE: nothing but the bytes. Its samples count from its own first block.
	Instance fresh;
	REQUIRE(fresh.load(saved));
	fresh.activate(48000.0, kBlock);
	fresh.start();
	REQUIRE(fresh.chain()->delivered_note(fresh.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	for (int b = 0; b < kBlocksAfter; ++b) {
		(void)fresh.run();
	}
	const std::vector<std::pair<int64_t, int>> loaded = onsOf(fresh);
	REQUIRE(loaded.size() == straight.size());
	for (size_t i = 0; i < loaded.size(); ++i) {
		CAPTURE(i);
		CHECK(loaded[i].second == straight[i].second);
		// THE PHASE SAVED IS THE ONE AT THE LAST CLOCK CALL, at most one render chunk (32 samples) before the save.
		CHECK(std::abs(loaded[i].first - straight[i].first) <= 32);
	}
	CHECK(straight[0].second == 60); // slot 3 of Up 60, 64, 67: the chord came back, in its order
}

TEST_CASE("the Pattern's hand edits are HEARD through the whole plugin: an edit made as the window makes it changes that step's "
          "note in what the voices are handed, whatever key is held, and no other step") {
	// FOUND AUDITIONING e3b854e3: edits in the editor took effect on the controls but were not heard in the
	// pattern. The REAL ROAD: the edit built as the window's lane builds it (its decode
	// over a held C4, `editOf` on the step), written through the bridge's `DeviceHost::setBlock`, a key other than C4 held,
	// and the notes read where the voices receive them.
	constexpr uint32_t kBlock = 32;
	namespace pt = feathers::mod::arp::pat;
	namespace pn = feathers::mod::arp::pattern;
	const auto run = [&](const std::vector<float>* edits, int heldKey) {
		Instance inst;
		const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), pt::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Run", 0.0)); // no transport here: free-run
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "P1 Time Base", 0.0));       // Hz
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "P1 Rate", 3.0));            // 8 steps a second
		if (edits != nullptr) {
			REQUIRE(inst.chain()->set_device_block(inst.plugin(), at, "pattern.edits.v4", edits->data(), edits->size()));
		}
		inst.activate(48000.0, kBlock);
		inst.start();
		int64_t rendered = 0;
		(void)inst.run();
		rendered += kBlock;
		REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
		const int64_t press = rendered;
		NoteList key;
		key.push(CLAP_EVENT_NOTE_ON, heldKey);
		(void)inst.runWith(key.input());
		rendered += kBlock;
		while (rendered < press + 6000 * 16) {
			(void)inst.run();
			rendered += kBlock;
		}
		std::map<int64_t, std::vector<int>> byStep;
		for (const auto& [sample, k] : onsOf(inst)) {
			byStep[(sample - press + 3000) / 6000].push_back(k);
		}
		return byStep;
	};
	// THE WINDOW'S DECODE: the focused pattern at its defaults, over a held C4.
	const pt::Settings s = pt::settingsOf(nullptr, 0);
	feathers::mod::arp::HeldChord c4;
	c4.press(60, 0, 0.8f, 1);
	pn::PatternCycle lane;
	pn::Recipe recipe = pt::recipeFor(s, 0, pt::HandSlot {}.pinOn);
	pn::build(pt::PatternDevice::placeOf(s.patterns[0], feathers::SliceResidue<feathers::mod::arp::genes::kRowCount> {}), recipe,
	          c4, lane);
	size_t step = 0;
	while (step < lane.length && lane.steps[step].kind != pn::PatternStep::Kind::Note) {
		++step;
	}
	REQUIRE(step < lane.length);
	for (const int held : { 48, 60, 67 }) {
		CAPTURE(held);
		const auto plain = run(nullptr, held);
		REQUIRE(plain.count(static_cast<int64_t>(step)) == 1);
		const int terrainKey = plain.at(static_cast<int64_t>(step))[0];
		// THE EDIT, AS THE LANE MAKES IT: the step dragged two semitones up from where the window drew it.
		pt::EditState state {};
		state[0].anchor = pt::anchorNow(0, [&](size_t i) { return static_cast<float>(pt::kParamDescs[i].def); });
		state[0].mode = feathers::mod::arp::genes::Mode::Melody;
		pn::StepEdit& e = state[0].steps[step];
		e = pn::editOf(lane.steps[step], false);
		pn::setEditKey(e, lane, lane.keyAt(step, 0) + 2);
		std::vector<float> words(pt::kEditsWords);
		pt::writeEditState(state, words.data());
		const auto edited = run(&words, held);
		for (const auto& [g, keys] : plain) {
			CAPTURE(g);
			if (g == static_cast<int64_t>(step)) {
				REQUIRE(edited.count(g) == 1);
				CHECK(edited.at(g) == std::vector<int> { terrainKey + 2 }); // HEARD, at the held key's transposition
			}
			else {
				CHECK(edited.count(g) == 1);
				if (edited.count(g) == 1) {
					CHECK(edited.at(g) == keys); // and no other step moved
				}
			}
		}
	}
}

namespace {

/// ONE KEY PRESSED AND LET GO ON A STRIP DEVICE, the transport as asked; the note-ons the voices were handed, by sample
/// from the press.
std::vector<std::pair<int64_t, int>> pressAndRelease(const char* tag, bool onlyWithTransport, bool transportPlaying,
                                                     int64_t holdSamples, int64_t afterSamples, bool press = true) {
	constexpr uint32_t kBlock = 32;
	Instance inst;
	const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), tag);
	REQUIRE(slot != UINT32_MAX);
	const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
	// LATCH AT ITS DEFAULT (no write); Only w/ Transport as asked.
	REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Run", onlyWithTransport ? 1.0 : 0.0));
	inst.transport(transportPlaying);
	inst.activate(48000.0, kBlock);
	inst.start();
	int64_t rendered = 0;
	(void)inst.run();
	rendered += kBlock;
	REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	const int64_t pressAt = rendered;
	if (press) {
		NoteList on;
		on.push(CLAP_EVENT_NOTE_ON, 60);
		(void)inst.runWith(on.input());
		rendered += kBlock;
	}
	while (rendered < pressAt + holdSamples) {
		(void)inst.run();
		rendered += kBlock;
	}
	if (press) {
		NoteList off;
		off.push(CLAP_EVENT_NOTE_OFF, 60);
		(void)inst.runWith(off.input());
		rendered += kBlock;
	}
	while (rendered < pressAt + holdSamples + afterSamples) {
		(void)inst.run();
		rendered += kBlock;
	}
	std::vector<std::pair<int64_t, int>> ons = onsOf(inst);
	for (auto& [sample, key] : ons) {
		sample -= pressAt;
	}
	return ons;
}

} // namespace

TEST_CASE("the reported idle condition, through the whole plugin: transport stopped, no key held, Latch at its default — SILENCE, on both "
          "arps, Only w/ Transport Off and On; and On, a held key plays nothing until the transport runs") {
	// THE DEFECT: with the transport off and no notes held, the arp was playing. Latch defaults
	// Off on both (asserted); a key pressed and let go must leave nothing sounding after its release plus one step.
	namespace ap = feathers::mod::arp;
	namespace pt = feathers::mod::arp::pat;
	CHECK(ap::kParamDescs[ap::at(ap::Param::Latch)].def == 0.0);
	CHECK(pt::kParamDescs[pt::at(pt::Global::Latch)].def == 0.0);
	constexpr int64_t kStepSamples = 6000; // a sixteenth at 120 bpm, which the free clock also runs at by default
	for (const char* tag : { ap::kTraits.tag, pt::kTraits.tag }) {
		for (const bool only : { false, true }) {
			CAPTURE(tag);
			CAPTURE(only);
			// NOTHING EVER HELD: nothing at all.
			CHECK(pressAndRelease(tag, only, false, 0, 16 * kStepSamples, false).empty());
			// A KEY HELD FOR FOUR STEPS AND LET GO: nothing after the release plus one step.
			const auto ons = pressAndRelease(tag, only, false, 4 * kStepSamples, 16 * kStepSamples);
			for (const auto& [sample, key] : ons) {
				CAPTURE(sample);
				CHECK(sample <= 5 * kStepSamples);
			}
			if (only) {
				CHECK(ons.empty()); // ON, THE TRANSPORT STOPPED: a held key plays nothing
			}
			else {
				CHECK_FALSE(ons.empty()); // OFF: the free clock plays while the key is held (the case is not vacuous)
			}
		}
		// AND ON, THE TRANSPORT RUNNING: the held key plays.
		CAPTURE(tag);
		CHECK_FALSE(pressAndRelease(tag, true, true, 4 * kStepSamples, 0).empty());
	}
}

TEST_CASE("the Pattern, transport running: a key held changes the next step's pitches — Transpose moves them by the root, Pool "
          "draws them from the keys held") {
	constexpr uint32_t kBlock = 32;
	constexpr int64_t kStep = 6000;
	namespace pt = feathers::mod::arp::pat;
	const auto play = [&](bool pool, const std::vector<int>& first, const std::vector<int>& second) {
		Instance inst;
		const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), pt::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Keys", pool ? 1.0 : 0.0));
		inst.transport(true);
		inst.activate(48000.0, kBlock);
		inst.start();
		int64_t rendered = 0;
		(void)inst.run();
		rendered += kBlock;
		REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
		NoteList on;
		for (const int k : first) {
			on.push(CLAP_EVENT_NOTE_ON, k);
		}
		(void)inst.runWith(on.input());
		rendered += kBlock;
		while (rendered < 16 * kStep) {
			(void)inst.run();
			rendered += kBlock;
		}
		NoteList change;
		for (const int k : first) {
			change.push(CLAP_EVENT_NOTE_OFF, k);
		}
		for (const int k : second) {
			change.push(CLAP_EVENT_NOTE_ON, k);
		}
		(void)inst.runWith(change.input());
		rendered += kBlock;
		while (rendered < 32 * kStep) {
			(void)inst.run();
			rendered += kBlock;
		}
		std::map<int64_t, std::vector<int>> byStep;
		for (const auto& [sample, key] : onsOf(inst)) {
			byStep[sample / kStep].push_back(key);
		}
		return byStep;
	};
	// TRANSPOSE: the second bar is the first bar seven semitones up, step for step (the held key is the root).
	const auto t = play(false, { 48 }, { 55 });
	int compared = 0;
	for (int64_t g = 1; g < 16; ++g) {
		if (t.count(g) == 1 && t.count(g + 16) == 1) {
			CAPTURE(g);
			CHECK(t.at(g + 16).size() == t.at(g).size());
			for (size_t i = 0; i < std::min(t.at(g).size(), t.at(g + 16).size()); ++i) {
				CHECK(t.at(g + 16)[i] == t.at(g)[i] + 7);
			}
			++compared;
		}
	}
	CHECK(compared >= 3); // the default pattern rests often; every step that plays in both bars is compared
	// POOL: every pitch is one of the keys held then, in some octave.
	const auto p = play(true, { 60, 64 }, { 62, 67 });
	int checked = 0;
	for (const auto& [g, keys] : p) {
		for (const int k : keys) {
			CAPTURE(g);
			CAPTURE(k);
			const int pc = ((k % 12) + 12) % 12;
			if (g >= 1 && g < 16) {
				CHECK((pc == 0 || pc == 4));
				++checked;
			}
			else if (g >= 17 && g < 32) {
				CHECK((pc == 2 || pc == 7));
				++checked;
			}
		}
	}
	CHECK(checked >= 6); // was > 8 while Detail's ratchets struck some steps twice (Ratchet is opt in since 2026-11-04)
}

TEST_CASE("a HOST choke lets a held key go on both arps — keyed or wildcard — so nothing plays after the next step with "
          "Only w/ Transport Off and the transport stopped") {
	// CLAP's NOTE_CHOKE ends the note; before 2026-11-04 the arps took a keyed one for the synth's own steal and kept the
	// key held, which with the free clock is "transport off, no notes held, it was playing". The steal still keeps
	// it (`NoteEvent::fromSteal`; test_synth's Mono-return cases).
	constexpr uint32_t kBlock = 32;
	constexpr int64_t kStep = 6000;
	namespace ap = feathers::mod::arp;
	namespace pt = feathers::mod::arp::pat;
	for (const char* tag : { ap::kTraits.tag, pt::kTraits.tag }) {
		for (const bool wildcard : { false, true }) {
			CAPTURE(tag);
			CAPTURE(wildcard);
			Instance inst;
			const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), tag);
			REQUIRE(slot != UINT32_MAX);
			const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
			REQUIRE(inst.chain()->set_control(inst.plugin(), at, "Run", 0.0));
			inst.transport(false);
			inst.activate(48000.0, kBlock);
			inst.start();
			int64_t rendered = 0;
			(void)inst.run();
			rendered += kBlock;
			REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
			const int64_t press = rendered;
			NoteList on;
			on.push(CLAP_EVENT_NOTE_ON, 60);
			on.push(CLAP_EVENT_NOTE_ON, 64);
			(void)inst.runWith(on.input());
			rendered += kBlock;
			while (rendered < press + 4 * kStep) {
				(void)inst.run();
				rendered += kBlock;
			}
			// THE HOST CHOKES: each key by name, or everything at once. No note-off ever follows.
			NoteList choke;
			if (wildcard) {
				choke.push(CLAP_EVENT_NOTE_CHOKE, -1);
			}
			else {
				choke.push(CLAP_EVENT_NOTE_CHOKE, 60);
				choke.push(CLAP_EVENT_NOTE_CHOKE, 64);
			}
			(void)inst.runWith(choke.input());
			const int64_t chokedAt = rendered;
			rendered += kBlock;
			while (rendered < press + 16 * kStep) {
				(void)inst.run();
				rendered += kBlock;
			}
			const auto ons = onsOf(inst);
			CHECK_FALSE(ons.empty()); // it played while held (not vacuous)
			for (const auto& [sample, key] : ons) {
				CAPTURE(sample);
				CHECK(sample <= chokedAt + kStep);
			}
		}
	}
}

TEST_CASE("thru through the whole plugin: a Drum pattern with its kick and snare lanes off hands the voices the player's kick and snare at their samples and its own other lanes from the transport; Thru Off swallows them; nothing is left sounding after a stop") {
	// The use: program the kick and snare by hand while the pattern's other hits keep playing.
	constexpr uint32_t kBlock = 32;
	namespace pt = feathers::mod::arp::pat;
	const auto run = [&](double thru) {
		Instance inst;
		const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), pt::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "P1 Mode", 5.0)); // Drum
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "P1 Kick", 0.0));
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "P1 Snare", 0.0));
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "P1 Thru", thru));
		inst.transport(true);
		inst.activate(48000.0, kBlock);
		inst.start();
		int64_t rendered = 0;
		REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr)); // from the first render
		std::map<std::string, int64_t> when;
		const auto send = [&](uint16_t type, int16_t key, const char* name) {
			NoteList n;
			n.push(type, key);
			when[name] = rendered;
			(void)inst.runWith(n.input());
			rendered += kBlock;
		};
		const auto until = [&](int64_t s) {
			while (rendered < s) {
				(void)inst.run();
				rendered += kBlock;
			}
		};
		until(6000 * 3 + 64 * kBlock / 32 + 160);
		send(CLAP_EVENT_NOTE_ON, 36, "kick on");
		until(6000 * 5 + 96);
		send(CLAP_EVENT_NOTE_ON, 38, "snare on");
		until(6000 * 9);
		send(CLAP_EVENT_NOTE_OFF, 36, "kick off");
		send(CLAP_EVENT_NOTE_OFF, 38, "snare off");
		until(6000 * 20);
		inst.transport(false); // STOP
		until(6000 * 24);
		std::vector<std::tuple<uint32_t, int, int64_t>> events;
		uint32_t kind = 0;
		int32_t key = 0;
		uint64_t sample = 0;
		for (uint32_t i = 0; inst.chain()->delivered_note(inst.plugin(), i, &kind, &key, &sample); ++i) {
			events.emplace_back(kind, key, static_cast<int64_t>(sample));
		}
		return std::pair { events, when };
	};
	const auto count = [](const auto& events, uint32_t kind, int key) {
		return std::count_if(events.begin(), events.end(), [&](const auto& e) { return std::get<0>(e) == kind && std::get<1>(e) == key; });
	};
	const auto has = [](const auto& events, uint32_t kind, int key, int64_t sample) {
		return std::any_of(events.begin(), events.end(), [&](const auto& e) {
			return std::get<0>(e) == kind && std::get<1>(e) == key && std::get<2>(e) == sample;
		});
	};
	const auto [on, when] = run(2.0); // Thru On
	CHECK(count(on, 0, 36) == 1);
	CHECK(count(on, 0, 38) == 1);
	CHECK(has(on, 0, 36, when.at("kick on")));
	CHECK(has(on, 0, 38, when.at("snare on")));
	CHECK(has(on, 1, 36, when.at("kick off")));
	CHECK(has(on, 1, 38, when.at("snare off")));
	int lanes = 0;
	std::map<int, int> balance;
	for (const auto& [k, key, s] : on) {
		lanes += k == 0 && key != 36 && key != 38 ? 1 : 0;
		balance[key] += k == 0 ? 1 : (k == 1 ? -1 : 0);
	}
	CHECK(lanes > 5); // the pattern's hats and toms, from the transport, no key held
	for (const auto& [key, n] : balance) {
		CAPTURE(key);
		CHECK(n == 0); // after the stop, nothing left sounding
	}
	const auto [off, whenOff] = run(1.0); // Thru Off: the old swallowing
	CHECK(count(off, 0, 36) == 0);
	CHECK(count(off, 0, 38) == 0);
}
#endif // !FEATHERS_RESTART_RACK — the voices' seam (`delivered_note`) is Feathers' alone

#if !FEATHERS_RESTART_RACK
namespace {

namespace ptr = feathers::mod::arp::pat;
namespace pnr = feathers::mod::arp::pattern;
namespace gnr = feathers::mod::arp::genes;

constexpr int64_t kStepSamples = 6000; // 120 bpm, a 16th, 48 kHz
constexpr int64_t kStepsHeard = 32;    // two 16-step cycles

/// **ONE KEY HELD, THE TRANSPORT RUNNING, THROUGH THE WHOLE PLUGIN**: the ons the voices were handed, grouped by the step
/// they fall in, each as (offset in the step, key). The key goes down in the second block, so step 0 is never heard.
std::map<int64_t, std::vector<std::pair<int64_t, int>>> heardBySteps(bool classicArp, const std::vector<std::pair<const char*, double>>& controls,
                                                                     const ptr::HandState* hand = nullptr) {
	constexpr uint32_t kBlock = 32;
	Instance inst;
	const uint32_t slot =
	    inst.chain()->add_mod_device(inst.plugin(), classicArp ? feathers::mod::arp::kTraits.tag : ptr::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
	for (const auto& [name, value] : controls) {
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, name, value));
	}
	if (hand != nullptr) {
		std::vector<float> words(ptr::kHandWords, 0.0f);
		ptr::writeHandState(*hand, words.data());
		REQUIRE(inst.chain()->set_device_block(inst.plugin(), at, "pattern_hand", words.data(), words.size()));
	}
	inst.transport(true);
	inst.activate(48000.0, kBlock);
	inst.start();
	(void)inst.run();
	REQUIRE(inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
	NoteList key;
	key.push(CLAP_EVENT_NOTE_ON, 60);
	(void)inst.runWith(key.input());
	for (int64_t rendered = 2 * kBlock; rendered < kStepsHeard * kStepSamples; rendered += kBlock) {
		(void)inst.run();
	}
	std::map<int64_t, std::vector<std::pair<int64_t, int>>> bySteps;
	for (const auto& [sample, k] : onsOf(inst)) {
		bySteps[sample / kStepSamples].emplace_back(sample % kStepSamples, k);
	}
	return bySteps;
}

/// NO KEY IS STRUCK TWICE IN ONE STEP — the audible double tap, as a count.
int doubleTaps(const std::map<int64_t, std::vector<std::pair<int64_t, int>>>& bySteps) {
	int doubles = 0;
	for (const auto& [g, ons] : bySteps) {
		std::map<int, int> perKey;
		for (const auto& [offset, k] : ons) {
			doubles += perKey[k]++ > 0 ? 1 : 0;
		}
	}
	return doubles;
}

} // namespace

TEST_CASE("Pattern at its defaults strikes no key twice in a step, through the whole plugin, in every pitched Mode — and Melody "
          "plays exactly one on per sounding step, on the grid; the classic Arp exactly one per step") {
	// THE DEFECT: Pattern in Melody mode (and perhaps others) double-tapped notes. They were Detail's RATCHETS — two
	// steps in sixteen at its default — so ratchets became opt-in, a field scaler: the Ratchet pin, Off.
	for (const auto& [mode, name] : std::vector<std::pair<gnr::Mode, const char*>> {
	         { gnr::Mode::Melody, "Melody" }, { gnr::Mode::MelodyMono, "Melody Mono" }, { gnr::Mode::Chords, "Chords" },
	         { gnr::Mode::TbMono, "TB-Mono" }, { gnr::Mode::TbGlide, "TB-Glide" } }) {
		CAPTURE(name);
		const auto heard = heardBySteps(false, { { "P1 Mode", static_cast<double>(mode) } });
		size_t ons = 0;
		for (const auto& [g, list] : heard) {
			ons += list.size();
		}
		CHECK(ons >= 6); // non-vacuity: the pattern plays
		CHECK(doubleTaps(heard) == 0);
		if (mode == gnr::Mode::Melody) {
			// THE PROBE'S 12 BECAME 8 (over samples 0..192000 inclusive): over steps 1..31 here, 11 ons become 7 — one per
			// sounding step (4, 8, 13, 16, 20, 24, 29), each on its step's own sample.
			CHECK(ons == 7);
			for (const auto& [g, list] : heard) {
				CAPTURE(g);
				CHECK(list.size() == 1);
				CHECK(list[0].first == 0);
			}
		}
	}
	// THE CLASSIC ARP: one on per step, every step heard, on the grid — unaffected by the Pattern's pins.
	const auto arp = heardBySteps(true, {});
	CHECK(arp.size() == static_cast<size_t>(kStepsHeard - 1));
	for (const auto& [g, list] : arp) {
		CAPTURE(g);
		REQUIRE(list.size() == 1);
		CHECK(list[0] == std::pair<int64_t, int> { 0, 60 });
	}
}

TEST_CASE("Pattern's Ratchet pin, switched ON, brings the ratchets back through the whole plugin exactly where its field says — "
          "the steps whose Detail row reads under it, 2-4 hits evenly in the step; Detail at any amount strikes none") {
	// THE REFERENCE, restated from the field rather than read from the builder: the pin's value at this place (the house law
	// at 0.5 is the field itself), and each step's per-step Detail row against it — `pattern.cpp`'s threshold and hit count.
	const ptr::Settings s = ptr::settingsOf(nullptr, 0);
	const ptr::PatternSettings& q = s.patterns[0];
	const gnr::Place place = ptr::PatternDevice::placeOf(q, feathers::SliceResidue<gnr::kRowCount> {});
	const float field = gnr::readRow(gnr::Row::WideDetail, place.phase[gnr::at(gnr::Row::WideDetail)]);
	// The cycle's rests and keys, from a build with Ratchet off (what a step IS; whether it ratchets is the field's).
	feathers::mod::arp::HeldChord c4;
	c4.press(60, 0, 0.8f, 1);
	pnr::PatternCycle plain;
	pnr::build(place, ptr::recipeFor(s, 0, ptr::HandSlot {}.pinOn), c4, plain);
	REQUIRE(plain.length == 16);
	std::map<int64_t, std::vector<std::pair<int64_t, int>>> want;
	int ratcheted = 0;
	int extraHits = 0;
	for (int64_t g = 1; g < kStepsHeard; ++g) {
		const auto k = static_cast<size_t>(g % 16);
		const pnr::PatternStep& st = plain.steps[k];
		if (st.kind != pnr::PatternStep::Kind::Note) {
			continue;
		}
		const float d = gnr::stepOf(place, k, plain.length)[gnr::at(gnr::Row::Detail)];
		const int hits = d < field ? 2 + std::min(2, static_cast<int>(3.0f * d / field)) : 1;
		ratcheted += hits > 1 ? 1 : 0;
		extraHits += hits - 1;
		for (int j = 0; j < hits; ++j) {
			want[g].emplace_back(kStepSamples * j / hits, plain.keyAt(k, g / 16));
		}
	}
	REQUIRE(ratcheted >= 2); // non-vacuity: this place ratchets (the steps that were heard)

	ptr::HandState hand {};
	hand[0].pinOn[static_cast<size_t>(gnr::Pin::Ratchet)] = true;
	const auto on = heardBySteps(false, {}, &hand);
	CHECK(on.size() == want.size());
	for (const auto& [g, list] : want) {
		CAPTURE(g);
		REQUIRE(on.count(g) == 1);
		REQUIRE(on.at(g).size() == list.size());
		for (size_t i = 0; i < list.size(); ++i) {
			CHECK(std::abs(on.at(g)[i].first - list[i].first) <= 1);
			CHECK(on.at(g)[i].second == list[i].second);
		}
	}
	CHECK(doubleTaps(on) == extraHits); // every ratcheted step strikes its key again, once per extra hit

	// DETAIL NO LONGER RATCHETS, at any amount (Ratchet left Off) — while in Melody Mono it still adds its grace notes.
	for (const double detail : { 0.0, 0.5, 0.8, 1.0 }) {
		CAPTURE(detail);
		CHECK(doubleTaps(heardBySteps(false, { { "P1 Detail", detail } })) == 0);
	}
	const auto graces = heardBySteps(false, { { "P1 Mode", static_cast<double>(gnr::Mode::MelodyMono) }, { "P1 Detail", 1.0 } });
	int twoKeySteps = 0;
	for (const auto& [g, list] : graces) {
		twoKeySteps += list.size() == 2 && list[0].second != list[1].second ? 1 : 0;
	}
	CHECK(twoKeySteps >= 2);
}
#endif // !FEATHERS_RESTART_RACK

#if !FEATHERS_RESTART_RACK
TEST_CASE("Chords at its defaults plays DYADS — root and 3rd, every chord — because this place's Size gene thins the Triad "
          "(thin 1 drops the 5th, theory::chordDegrees): by design, not a missing tone; the voices are handed exactly those keys") {
	// SEEN 2026-11-04 while chasing the double tap: a held C4 under the default Triad sounded 64+72, then 60+64. The Size
	// gene's reading at the default place, restated (`pattern.cpp`: thin = size < 0.6 ? 0 : size < 0.85 ? 1 : 2).
	namespace th = feathers::mod::arp::theory;
	ptr::Settings s = ptr::settingsOf(nullptr, 0);
	s.patterns[0].mode = gnr::Mode::Chords;
	const gnr::Place place = ptr::PatternDevice::placeOf(s.patterns[0], feathers::SliceResidue<gnr::kRowCount> {});
	const float size = gnr::readRow(gnr::Row::WideSize, place.phase[gnr::at(gnr::Row::WideSize)]);
	CAPTURE(size);
	CHECK(size >= 0.6f);
	CHECK(size < 0.85f); // thin 1: the 5th dropped, the root kept
	int degrees[th::kMaxTones] {};
	REQUIRE(th::chordDegrees(th::ChordSize::Triad, 1, degrees) == 2);
	CHECK(degrees[0] == 0);
	CHECK(degrees[1] == 2); // root and 3rd
	feathers::mod::arp::HeldChord c4;
	c4.press(60, 0, 0.8f, 1);
	pnr::PatternCycle cycle;
	pnr::build(place, ptr::recipeFor(s, 0, ptr::HandSlot {}.pinOn), c4, cycle);
	const th::Scale scale = th::scaleOf(s.patterns[0].scale);
	int chords = 0;
	for (size_t k = 0; k < cycle.length; ++k) {
		const pnr::PatternStep& st = cycle.steps[k];
		if (st.kind != pnr::PatternStep::Kind::Note) {
			continue;
		}
		CAPTURE(k);
		REQUIRE(st.count == 2);
		// EACH TONE IS THE CHORD'S ROOT OR ITS 3RD, by pitch class, and both are there.
		const int root = ((th::keyOfDegree(scale, 60, st.degree) % 12) + 12) % 12;
		const int third = ((th::keyOfDegree(scale, 60, st.degree + 2) % 12) + 12) % 12;
		std::set<int> classes;
		for (int i = 0; i < st.count; ++i) {
			classes.insert(((st.keys[static_cast<size_t>(i)] % 12) + 12) % 12);
		}
		CHECK(classes == std::set<int> { root, third });
		++chords;
	}
	CHECK(chords >= 3);
	// AND WHAT THE VOICES ARE HANDED IS THAT: two ons per sounding step, both on the step's sample.
	const auto heard = heardBySteps(false, { { "P1 Mode", static_cast<double>(gnr::Mode::Chords) } });
	REQUIRE(!heard.empty());
	for (const auto& [g, list] : heard) {
		CAPTURE(g);
		const pnr::PatternStep& st = cycle.steps[static_cast<size_t>(g % 16)];
		std::set<int> keys;
		for (const auto& [offset, k] : list) {
			CHECK(offset == 0);
			keys.insert(k);
		}
		// A HELD COMMON TONE runs on instead of re-striking (design §10.1), so a step may strike fewer than its count.
		CHECK(keys.size() <= 2);
		for (const int k : keys) {
			CHECK((k == st.keys[0] || k == st.keys[1]));
		}
	}
}
#endif // !FEATHERS_RESTART_RACK

#if !FEATHERS_RESTART_RACK
TEST_CASE("Pattern's VOICE LEADING pin reaches the voices through the whole plugin: in Progression, each strike is handed the "
          "keys the cycle is voiced with at that amount — 1 (the default, the least motion) and 0 (block chords) — and they differ") {
	// Progressions mode has a Voice Leading field. The reference is the device's own
	// decode (`recipeFor` + `build`, what the card's lane draws), at the amount set through the plugin's control.
	feathers::mod::arp::HeldChord c4;
	c4.press(60, 0, 0.8f, 1);
	const auto expected = [&](float amount) {
		ptr::Settings s = ptr::settingsOf(nullptr, 0);
		s.patterns[0].mode = gnr::Mode::Progression;
		s.patterns[0].pins[static_cast<size_t>(gnr::Pin::VoiceLeading)] = amount;
		pnr::PatternCycle cycle;
		pnr::build(ptr::PatternDevice::placeOf(s.patterns[0], feathers::SliceResidue<gnr::kRowCount> {}),
		           ptr::recipeFor(s, 0, ptr::HandSlot {}.pinOn), c4, cycle);
		return cycle;
	};
	std::map<double, std::map<int64_t, std::set<int>>> heardAt;
	for (const double amount : { 1.0, 0.0 }) {
		CAPTURE(amount);
		const pnr::PatternCycle cycle = expected(static_cast<float>(amount));
		REQUIRE(cycle.length > 0);
		const auto heard = heardBySteps(false, { { "P1 Mode", static_cast<double>(gnr::Mode::Progression) },
		                                         { "P1 Voice Leading", amount } });
		int strikes = 0;
		for (const auto& [g, list] : heard) {
			CAPTURE(g);
			const pnr::PatternStep& st = cycle.steps[static_cast<size_t>(g) % cycle.length];
			REQUIRE(st.kind == pnr::PatternStep::Kind::Note);
			std::set<int> keys;
			for (const auto& [offset, k] : list) {
				keys.insert(k);
			}
			// EVERY KEY HANDED IS ONE OF THIS STEP'S VOICING (a held common tone runs on rather than re-striking).
			for (const int k : keys) {
				CHECK(std::find(st.keys.begin(), st.keys.begin() + st.count, k) != st.keys.begin() + st.count);
			}
			heardAt[amount][g] = keys;
			strikes += st.chordStart ? 1 : 0;
		}
		CHECK(strikes >= 2);
	}
	CHECK(heardAt[1.0] != heardAt[0.0]); // non-vacuity: the pin moved what the voices were handed
}
#endif // !FEATHERS_RESTART_RACK

#if !FEATHERS_RESTART_RACK
TEST_CASE("Pattern's APPROACH pin through the whole plugin: in Chords and Progression each pickup step hands the voices ONE "
          "note, the decode's pickup key, on its step's sample, and the change after it strikes the new chord; switched on, "
          "it survives a save into a FRESH instance, which hands the voices the same notes") {
	// Progression and Chord are the initial pins. The reference is the device's own decode at the
	// same switches (`recipeFor` + `build`: what the card's lane draws), so this is the ROAD — the hand block's switch, the
	// pin's field and the emitter — not the builder, which test_arp_pattern restates by value.
	constexpr uint32_t kBlock = 32;
	feathers::mod::arp::HeldChord c4;
	c4.press(60, 0, 0.8f, 1);
	for (const gnr::Mode mode : { gnr::Mode::Chords, gnr::Mode::Progression }) {
		CAPTURE(static_cast<int>(mode));
		ptr::HandState hand {};
		hand[0].pinOn[static_cast<size_t>(gnr::Pin::Approach)] = true;
		ptr::Settings s = ptr::settingsOf(nullptr, 0);
		s.patterns[0].mode = mode;
		s.patterns[0].pins[static_cast<size_t>(gnr::Pin::Approach)] = 1.0f;
		pnr::PatternCycle cycle;
		int pickupSteps = 0;
		// THE FIRST PLACE ACROSS (Place X) WHOSE CYCLE HAS PICKUPS: the default place's Chords never changes degree.
		double placeX = 0.0;
		for (int i = 0; i < 64 && pickupSteps < 2; ++i) {
			placeX = 0.37 * i;
			s.patterns[0].placeX = static_cast<float>(placeX);
			pnr::build(ptr::PatternDevice::placeOf(s.patterns[0], feathers::SliceResidue<gnr::kRowCount> {}),
			           ptr::recipeFor(s, 0, hand[0].pinOn), c4, cycle);
			pickupSteps = 0;
			for (size_t k = 0; k < cycle.length; ++k) {
				pickupSteps += cycle.steps[k].approach != pnr::ApproachKind::None ? 1 : 0;
			}
		}
		REQUIRE(pickupSteps >= 2); // non-vacuity: a place with changes the pin reaches
		CAPTURE(placeX);

		const std::vector<std::pair<const char*, double>> controls { { "P1 Mode", static_cast<double>(mode) },
			                                                         { "P1 Approach", 1.0 },
			                                                         { "P1 Place X", placeX } };
		const auto heard = heardBySteps(false, controls, &hand);
		// EVERY PICKUP STEP THE RENDER REACHES (steps 1..31: the key goes down in the second block) is heard, alone.
		int heardPickups = 0;
		for (int64_t g = 1; g < kStepsHeard; ++g) {
			CAPTURE(g);
			const pnr::PatternStep& st = cycle.steps[static_cast<size_t>(g) % cycle.length];
			if (st.approach == pnr::ApproachKind::None) {
				continue;
			}
			REQUIRE(heard.count(g) == 1);
			REQUIRE(heard.at(g).size() == 1);
			CHECK(heard.at(g)[0] == std::pair<int64_t, int> { 0, st.keys[0] });
			++heardPickups;
		}
		CHECK(heardPickups >= 1); // non-vacuity: the render reached one
		// AND OFF (the default switch), no pickup reaches the voices: the steps the decode replaces strike their chord.
		const auto off = heardBySteps(false, controls);
		CHECK(off != heard);

		// SAVE → A FRESH INSTANCE: the switch (hand block) and the amount (a field) both come back.
		Instance inst;
		const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), ptr::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
		for (const auto& [name, value] : controls) {
			REQUIRE(inst.chain()->set_control(inst.plugin(), at, name, value));
		}
		std::vector<float> words(ptr::kHandWords, 0.0f);
		ptr::writeHandState(hand, words.data());
		REQUIRE(inst.chain()->set_device_block(inst.plugin(), at, "pattern_hand", words.data(), words.size()));
		inst.activate(48000.0, kBlock);
		inst.start();
		(void)inst.run();
		Bytes saved = inst.save();
		Instance fresh;
		REQUIRE(fresh.load(saved));
		fresh.transport(true);
		fresh.activate(48000.0, kBlock);
		fresh.start();
		(void)fresh.run();
		REQUIRE(fresh.chain()->delivered_note(fresh.plugin(), UINT32_MAX, nullptr, nullptr, nullptr));
		NoteList key;
		key.push(CLAP_EVENT_NOTE_ON, 60);
		(void)fresh.runWith(key.input());
		for (int64_t rendered = 2 * kBlock; rendered < kStepsHeard * kStepSamples; rendered += kBlock) {
			(void)fresh.run();
		}
		std::map<int64_t, std::vector<std::pair<int64_t, int>>> loaded;
		for (const auto& [sample, k] : onsOf(fresh)) {
			loaded[sample / kStepSamples].emplace_back(sample % kStepSamples, k);
		}
		CHECK(loaded == heard);
	}
}
#endif // !FEATHERS_RESTART_RACK

#if !FEATHERS_RESTART_RACK
// ---- WHICH PRESET THE PATCH CAME FROM (backlog QC: after a Bitwig audio engine restart, Feathers forgot which preset
// it had loaded and showed Init, though it kept the parameters from that preset) ----

namespace {

struct NamedPreset {
	std::string name;
	std::string category;
};

NamedPreset presetIdentityOf(const Instance& inst) {
	REQUIRE(inst.chain()->preset_identity != nullptr);
	char name[256] {};
	char category[256] {};
	REQUIRE(inst.chain()->preset_identity(inst.plugin(), name, sizeof(name), category, sizeof(category)));
	return { name, category };
}

} // namespace

TEST_CASE("QC: the preset the patch came from — a VARIANT, in a folder — survives deactivate/activate and a save into a "
          "FRESH instance, by value") {
	Bytes saved;
	{
		Instance inst;
		inst.activate(48000.0, 512);
		inst.start();
		buildPatch(inst);
		// THE ROAD THE EDITOR'S BROWSER TAKES when it loads or saves (`ParamBridge::setPresetIdentity`): chip 3 of Glass.
		REQUIRE(inst.chain()->set_preset_identity(inst.plugin(), "Glass_variant_3", "Pads"));
		// (a) STOP, DEACTIVATE, ACTIVATE AT ANOTHER RATE: the same instance, the same name.
		inst.restart(44100.0, 256);
		(void)inst.run();
		const NamedPreset after = presetIdentityOf(inst);
		CHECK(after.name == "Glass_variant_3");
		CHECK(after.category == "Pads");
		saved = inst.save();
	}
	// (c) THE ENGINE RESTART THAT REBUILDS: a fresh instance, the host's bytes loaded, then activated.
	Instance fresh;
	CHECK(presetIdentityOf(fresh).name.empty()); // a fresh instance names none until the load
	REQUIRE(fresh.load(saved));
	fresh.activate(48000.0, 512);
	fresh.start();
	(void)fresh.run();
	const NamedPreset loaded = presetIdentityOf(fresh);
	CHECK(loaded.name == "Glass_variant_3");
	CHECK(loaded.category == "Pads");

	// A PRESET FILE NAMES NO PRESET: its capture writes none, and loading one leaves the name to the browser.
	Bytes presetFile;
	{
		clap_ostream out = presetFile.out();
		REQUIRE(fresh.chain()->capture_preset(fresh.plugin(), &out));
	}
	REQUIRE(fresh.chain()->set_preset_identity(fresh.plugin(), "Other", ""));
	Instance third;
	REQUIRE(third.load(presetFile)); // a preset file loaded as a whole state: it names nothing
	CHECK(presetIdentityOf(third).name.empty());
}

TEST_CASE("QC: with the editor open, a restart keeps the patch's preset name, and a fresh instance's editor opens on it") {
#if defined(__linux__)
	if (std::getenv("DISPLAY") == nullptr) {
		MESSAGE("skipped: no DISPLAY — run under scripts/venue-xvfb.sh (ctest -R engine-restart)");
		return;
	}
#endif
	Bytes saved;
	{
		Instance inst;
		inst.activate(48000.0, 512);
		inst.start();
		// NAMED BEFORE THE EDITOR OPENS, so its face is put on the name at create (a host load under an OPEN editor is
		// followed on the editor's tick: test_gui_widgets_presets' QC case, which can tick it).
		REQUIRE(inst.chain()->set_preset_identity(inst.plugin(), "Init", ""));
		inst.openEditor();
		inst.pumpMainThread();
		{
			char face[256] {};
			REQUIRE(inst.chain()->preset_face(inst.plugin(), face, sizeof(face)));
			REQUIRE(std::string(face) == "Init");
		}
		inst.restart(44100.0, 256);
		(void)inst.run();
		inst.pumpMainThread();
		CHECK(presetIdentityOf(inst).name == "Init");
		char face[256] {};
		REQUIRE(inst.chain()->preset_face(inst.plugin(), face, sizeof(face)));
		CHECK(std::string(face) == "Init"); // the editor's browser follows the patch's name (the factory Init: always in the bank)
		saved = inst.save();
		inst.closeEditor();
	}
	Instance fresh;
	REQUIRE(fresh.load(saved));
	fresh.activate(48000.0, 512);
	fresh.openEditor();
	fresh.pumpMainThread();
	char face[256] {};
	REQUIRE(fresh.chain()->preset_face(fresh.plugin(), face, sizeof(face)));
	CHECK(std::string(face) == "Init");
	fresh.closeEditor();
}
#endif // !FEATHERS_RESTART_RACK

// ---- QC, BOTH PLUGINS: the rack plugin's own browser and every device panel's browser (2026-11-07) ----

namespace {

std::pair<std::string, std::string> topPresetOf(const Instance& inst) {
	REQUIRE(inst.chain()->preset_identity != nullptr);
	char name[256] {};
	char category[256] {};
	REQUIRE(inst.chain()->preset_identity(inst.plugin(), name, sizeof(name), category, sizeof(category)));
	return { name, category };
}

std::pair<std::string, std::string> devicePresetOf(const Instance& inst, uint32_t position) {
	REQUIRE(inst.chain()->device_preset != nullptr);
	char name[256] {};
	char category[256] {};
	REQUIRE(inst.chain()->device_preset(inst.plugin(), position, name, sizeof(name), category, sizeof(category)));
	return { name, category };
}

} // namespace

TEST_CASE(PLUGIN_NAME "QC: the plugin's preset and a DEVICE PANEL's preset (a variant, in a folder) survive deactivate/activate "
                      "and a save into a FRESH instance, by value") {
	Bytes saved;
	{
		Instance inst;
		inst.activate(48000.0, 512);
		inst.start();
		buildPatch(inst);
		REQUIRE(inst.chain()->set_preset_identity(inst.plugin(), "Glass", "Pads"));
		// THE DEVICE AT POSITION 0 — on the road its panel's browser takes (`DeviceHost::setDevicePreset`).
		REQUIRE(inst.chain()->set_device_preset(inst.plugin(), 0, "Dub_variant_2", "Tape"));
		REQUIRE(inst.chain()->set_device_preset(inst.plugin(), 1, "", "")); // the second names none
		inst.restart(44100.0, 256);
		(void)inst.run();
		CHECK(topPresetOf(inst) == std::pair<std::string, std::string> { "Glass", "Pads" });
		CHECK(devicePresetOf(inst, 0) == std::pair<std::string, std::string> { "Dub_variant_2", "Tape" });
		saved = inst.save();
	}
	Instance fresh;
	REQUIRE(fresh.load(saved));
	fresh.activate(48000.0, 512);
	fresh.start();
	(void)fresh.run();
	CHECK(topPresetOf(fresh) == std::pair<std::string, std::string> { "Glass", "Pads" });
	CHECK(devicePresetOf(fresh, 0) == std::pair<std::string, std::string> { "Dub_variant_2", "Tape" });
	CHECK(devicePresetOf(fresh, 1) == std::pair<std::string, std::string> { "", "" });
	// AND THE FRESH INSTANCE SAVES THE SAME NAMES BACK.
	Bytes again = fresh.save();
	Instance third;
	REQUIRE(third.load(again));
	CHECK(devicePresetOf(third, 0) == std::pair<std::string, std::string> { "Dub_variant_2", "Tape" });
}

#if FEATHERS_RESTART_RACK
TEST_CASE(PLUGIN_NAME "QC: the rack plugin's editor opens on its patch's preset, through a restart and in a fresh instance") {
#if defined(__linux__)
	if (std::getenv("DISPLAY") == nullptr) {
		MESSAGE("skipped: no DISPLAY — run under scripts/venue-xvfb.sh (ctest -R rack-restart)");
		return;
	}
#endif
	Bytes saved;
	{
		Instance inst;
		inst.activate(48000.0, 512);
		inst.start();
		REQUIRE(inst.chain()->set_preset_identity(inst.plugin(), "Init", "")); // the chain bank's factory Init
		inst.openEditor();
		inst.pumpMainThread();
		char face[256] {};
		REQUIRE(inst.chain()->preset_face(inst.plugin(), face, sizeof(face)));
		CHECK(std::string(face) == "Init");
		inst.restart(44100.0, 256);
		(void)inst.run();
		inst.pumpMainThread();
		REQUIRE(inst.chain()->preset_face(inst.plugin(), face, sizeof(face)));
		CHECK(std::string(face) == "Init");
		saved = inst.save();
		inst.closeEditor();
	}
	Instance fresh;
	REQUIRE(fresh.load(saved));
	fresh.activate(48000.0, 512);
	fresh.openEditor();
	fresh.pumpMainThread();
	char face[256] {};
	REQUIRE(fresh.chain()->preset_face(fresh.plugin(), face, sizeof(face)));
	CHECK(std::string(face) == "Init");
	fresh.closeEditor();
}
#endif
