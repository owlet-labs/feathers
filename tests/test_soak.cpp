// Copyright (C) 2026 Owlet Records
//
// This file is part of Feathers.
//
// Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
// General Public License as published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
// the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
// Public License for more details.
//
// You should have received a copy of the GNU General Public License along with Feathers. If not, see
// <https://www.gnu.org/licenses/>.

// **THE SOAK** (on macOS, after a long session playing Pattern in TB-Glide mode and switching
// engines, the plugin's CPU had crept up and a restart brought it back down). Something ACCUMULATED. This binary drives
// the whole plugin through its CLAP process road — the Pattern in TB-Glide feeding the voices, keys held and slid
// legato, the Engine switched every few seconds across every engine — and reads, BY VALUE, everything that could grow:
// voices per stage, keys the voices think are down, the lane's invented notes, rings and retired storage, allocations
// made inside process(), and the thread CPU of each block. After warm-up every one of them must be FLAT.
//
// The short cases run in ctest. The long one is `[soak]` and skipped unless asked for (`--no-skip`).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "clap/clap.h"

#include "mod/arp/genes.h"
#include "mod/arp/pattern_types.h"
#include "plugin/feathers_test_chain.h"
#include "plugin/params.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <new>
#include <string>
#include <tuple>
#include <vector>

extern "C" const clap_plugin_entry clap_entry;

//------------------------------------------------------------------------------------------------//
// ALLOCATIONS INSIDE process(): counted by replacing the global operator new for this binary.      //
//------------------------------------------------------------------------------------------------//

namespace {
thread_local bool tInProcess = false;
std::atomic<uint64_t> gProcessAllocations { 0 };
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t size) {
	if (tInProcess) {
		gProcessAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	if (void* p = feathers::test::heapAlloc(size == 0 ? 1 : size)) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
	return ::operator new(size);
}
void* operator new(std::size_t size, std::align_val_t al) {
	if (tInProcess) {
		gProcessAllocations.fetch_add(1, std::memory_order_relaxed);
	}
	const std::size_t a = static_cast<std::size_t>(al);
	const std::size_t rounded = (size + a - 1) / a * a;
	if (void* p = feathers::test::heapAlignedAlloc(a, rounded == 0 ? a : rounded)) {
		return p;
	}
	throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t al) {
	return ::operator new(size, al);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
	try {
		return ::operator new(size);
	} catch (...) {
		return nullptr;
	}
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
	try {
		return ::operator new(size);
	} catch (...) {
		return nullptr;
	}
}
void operator delete(void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, std::size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::size_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
	feathers::test::heapFree(p);
}

#pragma GCC diagnostic pop

namespace {

//------------------------------------------------------------------------------------------------//
// A HOST THAT OFFERS A TIMER and remembers a request for a main-thread visit (test_engine_restart's). //
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
	CLAP_VERSION_INIT, nullptr, "feathers soak", "Owlet Records", "", "1.0",
	hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback,
};

bool CLAP_ABI dropOutput(const clap_output_events*, const clap_event_header*) {
	return true;
}

/// ONE BLOCK'S INPUT: notes and parameter values, all at frame 0, in the order pushed.
struct Events {
	union Event {
		clap_event_header header;
		clap_event_note note;
		clap_event_param_value param;
	};
	std::vector<Event> events;
	/// A note at frame 0. `noteId` and `channel` default to the key's own id and channel 0; -1 is CLAP's wildcard.
	void note(uint16_t type, int16_t key, int32_t noteId = -2, int16_t channel = 0) {
		Event e {};
		e.note.header.size = sizeof(clap_event_note);
		e.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.note.header.type = type;
		e.note.note_id = noteId == -2 ? key : noteId;
		e.note.port_index = 0;
		e.note.channel = channel;
		e.note.key = key;
		e.note.velocity = 0.8;
		events.push_back(e);
	}
	void param(clap_id id, double value) {
		Event e {};
		e.param.header.size = sizeof(clap_event_param_value);
		e.param.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.param.header.type = CLAP_EVENT_PARAM_VALUE;
		e.param.param_id = id;
		e.param.note_id = -1;
		e.param.port_index = -1;
		e.param.channel = -1;
		e.param.key = -1;
		e.param.value = value;
		events.push_back(e);
	}
	static uint32_t CLAP_ABI size(const clap_input_events* list) {
		return static_cast<uint32_t>(static_cast<const Events*>(list->ctx)->events.size());
	}
	static const clap_event_header* CLAP_ABI get(const clap_input_events* list, uint32_t i) {
		return &static_cast<const Events*>(list->ctx)->events[i].header;
	}
	clap_input_events input() const { return { const_cast<Events*>(this), size, get }; }
};

constexpr double kRate = 48000.0;

class Instance {
public:
	explicit Instance(uint32_t block) : block_(block) {
		REQUIRE(clap_entry.init("feathers-soak"));
		const auto* factory = static_cast<const clap_plugin_factory*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
		REQUIRE(factory != nullptr);
		plugin_ = factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
		REQUIRE(plugin_ != nullptr);
		REQUIRE(plugin_->init(plugin_));
		chain_ = static_cast<const clap_plugin_feathers_test_chain*>(plugin_->get_extension(plugin_, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain_ != nullptr);
		REQUIRE(chain_->synth_reading != nullptr);
		left_.assign(block_, 0.0f);
		right_.assign(block_, 0.0f);
	}
	~Instance() {
		plugin_->stop_processing(plugin_);
		plugin_->deactivate(plugin_);
		plugin_->destroy(plugin_);
		clap_entry.deinit();
	}
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	const clap_plugin* plugin() const { return plugin_; }
	const clap_plugin_feathers_test_chain* chain() const { return chain_; }

	void start() {
		REQUIRE(plugin_->activate(plugin_, kRate, 1, block_));
		REQUIRE(plugin_->start_processing(plugin_));
	}
	/// A HOST'S SLEEP AND WAKE (`stop_processing` then `start_processing`), and its RESTART (deactivate, activate).
	void wake() {
		plugin_->stop_processing(plugin_);
		REQUIRE(plugin_->start_processing(plugin_));
	}
	void restart() {
		plugin_->stop_processing(plugin_);
		plugin_->deactivate(plugin_);
		start();
	}
	/// One block, the transport playing at 120 bpm; returns the thread CPU it took, in microseconds.
	double run(const Events& events) {
		const clap_input_events in = events.input();
		const clap_output_events out { nullptr, dropOutput };
		float* channels[2] = { left_.data(), right_.data() };
		clap_audio_buffer output {};
		output.data32 = channels;
		output.channel_count = 2;
		clap_event_transport transport {};
		transport.header.size = sizeof(transport);
		transport.header.type = CLAP_EVENT_TRANSPORT;
		transport.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_IS_PLAYING;
		transport.tempo = 120.0;
		transport.song_pos_beats = static_cast<clap_beattime>(beats_ * static_cast<double>(CLAP_BEATTIME_FACTOR));
		clap_process process {};
		process.steady_time = steady_;
		process.frames_count = block_;
		process.audio_outputs = &output;
		process.audio_outputs_count = 1;
		process.in_events = &in;
		process.out_events = &out;
		process.transport = &transport;
		timespec a {}, b {};
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &a);
		tInProcess = true;
		(void)plugin_->process(plugin_, &process);
		tInProcess = false;
		clock_gettime(CLOCK_THREAD_CPUTIME_ID, &b);
		steady_ += block_;
		beats_ += static_cast<double>(block_) / kRate * 2.0;
		// THE HOST'S MAIN THREAD, visited between blocks as a host would when asked.
		if (gCallbackRequested.exchange(false)) {
			plugin_->on_main_thread(plugin_);
		}
		return static_cast<double>(b.tv_sec - a.tv_sec) * 1e6 + static_cast<double>(b.tv_nsec - a.tv_nsec) * 1e-3;
	}
	double reading(uint32_t what) const {
		double v = -1.0;
		REQUIRE(chain_->synth_reading(plugin_, what, &v));
		return v;
	}
	uint32_t block() const { return block_; }
	/// The song position the next block plays at, in beats — set to stand a fresh instance where a live one is.
	double beats() const { return beats_; }
	void setBeats(double beats) { beats_ = beats; }
	std::vector<uint8_t> save() const {
		std::vector<uint8_t> bytes;
		clap_ostream out { &bytes, [](const clap_ostream* s, const void* buffer, uint64_t size) -> int64_t {
			                  auto* b = static_cast<std::vector<uint8_t>*>(s->ctx);
			                  const auto* from = static_cast<const uint8_t*>(buffer);
			                  b->insert(b->end(), from, from + size);
			                  return static_cast<int64_t>(size);
		                  } };
		REQUIRE(state()->save(plugin_, &out));
		return bytes;
	}
	bool load(const std::vector<uint8_t>& bytes) const {
		struct Reader {
			const std::vector<uint8_t>* bytes;
			size_t at = 0;
		} reader { &bytes };
		clap_istream in { &reader, [](const clap_istream* s, void* buffer, uint64_t size) -> int64_t {
			                 auto* r = static_cast<Reader*>(s->ctx);
			                 const size_t take = std::min<size_t>(static_cast<size_t>(size), r->bytes->size() - r->at);
			                 std::memcpy(buffer, r->bytes->data() + r->at, take);
			                 r->at += take;
			                 return static_cast<int64_t>(take);
		                 } };
		return state()->load(plugin_, &in);
	}
	const clap_plugin_state* state() const {
		return static_cast<const clap_plugin_state*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
	}

private:
	const clap_plugin* plugin_ = nullptr;
	const clap_plugin_feathers_test_chain* chain_ = nullptr;
	uint32_t block_;
	std::vector<float> left_, right_;
	int64_t steady_ = 0;
	double beats_ = 0.0;
};

constexpr std::array<const char*, CLAP_FEATHERS_SYNTH_READING_COUNT> kReadingNames {
	"managed", "sounding", "attack", "sustain", "release", "stealing", "keysDown", "laneInvented", "modRings", "retired",
	"pending", "patHeld", "patSounding", "patLowest", "patNewest",
};

/// One window's readings: the MAXIMUM of each count over the window, the median block CPU, the allocations in process().
struct Window {
	std::array<double, CLAP_FEATHERS_SYNTH_READING_COUNT> peak {};
	double medianMicros = 0.0;
	uint64_t allocations = 0;
};

struct SoakPlan {
	double seconds = 60.0;
	double windowSeconds = 10.0;
	double keySeconds = 0.75;    ///< a new key slid in legato this often
	double engineSeconds = 3.0;  ///< the Engine switched this often
	bool switchEngines = true;
	bool endWithRelease = true;  ///< let every key go for the final window
	uint32_t block = 512;
	int64_t firstKeyBlock = 8; ///< the first key goes down this many blocks in (0: in the very first block)
	/// HOW A KEY IS LET GO: 0 its own id and channel; 1 note id -1 (CLAP's wildcard); 2 note id AND channel -1;
	/// 3 the off BEFORE the next on in the same block (not a slide); 4 cycle through 0..3 key by key.
	int offStyle = 0;
	/// Every `engineOnKeyEvery`th key change ALSO switches the Engine in the same block (0: never) — an off delivered
	/// during an engine switch.
	int engineOnKeyEvery = 0;
	/// Save the live instance here (seconds; < 0: never), load it into a FRESH one stood at the same song position, and
	/// drive both alike for `compareSeconds`, comparing what each hands its voices.
	double saveAt = -1.0;
	double compareSeconds = 8.0;
	/// `VoiceMode` (0 Poly, 1 Mono, 2 Legato; -1 the default) and Glide (0..1, the param's own scale; -1 the default).
	int voiceMode = -1;
	double glide = -1.0;
	bool modulators = false; ///< an LFO, a DAHDSR and an MSEG routed to voice parameters, and core routes
};

/// **THE NOTE ROAD, AUDITED BY VALUE**: what the host sent into the Pattern against what the Pattern holds, and every on
/// the voices were handed against its off.
struct NoteAudit {
	uint64_t hostOns = 0, hostOffs = 0;
	uint64_t voiceOns = 0, voiceOffs = 0, voiceChokes = 0;
	/// An off or choke handed the voices for a key with no on open there: a MISMATCHED off.
	uint64_t strayOffs = 0;
	/// Blocks where the Pattern's held set was not exactly the one key the host holds (size, or its newest key).
	uint64_t heldMismatchBlocks = 0;
	std::string firstHeldMismatch;
	/// Ons still open on the voices' side at the end (after the release window): LOST offs.
	uint64_t openAtEnd = 0;
	std::string openKeys;
};

/// **LIVE AGAINST RELOADED**: blocks compared and the ones where what each instance handed its voices (the ons, by key and
/// sample in the block) or the pitches its held voices sound differ.
struct ReloadAudit {
	bool ran = false;
	uint64_t blocks = 0, onMismatches = 0, pitchMismatches = 0, ons = 0;
	std::string first;
};

struct SoakResult {
	std::vector<Window> windows;
	NoteAudit notes;
	ReloadAudit reload;
};

/// The voice-side note events handed since the last call (each as kind, key, sample), and the log cleared.
std::vector<std::array<int64_t, 3>> drainDelivered(const Instance& inst) {
	std::vector<std::array<int64_t, 3>> out;
	uint32_t kind = 0;
	int32_t key = 0;
	uint64_t sample = 0;
	for (uint32_t i = 0; inst.chain()->delivered_note(inst.plugin(), i, &kind, &key, &sample); ++i) {
		out.push_back({ static_cast<int64_t>(kind), key, static_cast<int64_t>(sample) });
	}
	(void)inst.chain()->delivered_note(inst.plugin(), UINT32_MAX, nullptr, nullptr, nullptr);
	return out;
}

/// The pitches (semitones, rounded to a cent) of every voice in Attack or Sustain, sorted.
std::vector<long> heldPitches(const Instance& inst) {
	std::vector<long> out;
	for (uint32_t s = 0;; ++s) {
		double stage = 0;
		if (!inst.chain()->voice_reading(inst.plugin(), s, CLAP_FEATHERS_VOICE_STAGE, &stage)) {
			break;
		}
		if (stage == 1 || stage == 2) {
			double pitch = 0;
			(void)inst.chain()->voice_reading(inst.plugin(), s, CLAP_FEATHERS_VOICE_PITCH, &pitch);
			out.push_back(std::lround(pitch * 100.0));
		}
	}
	std::sort(out.begin(), out.end());
	return out;
}

constexpr std::array<int16_t, 7> kSoakKeys { 48, 51, 55, 53, 58, 46, 60 };

/// A NEW KEY INTO `ev` for the soak: the next one down, then the last one let go in `style` (SoakPlan::offStyle).
void slideTo(Events& ev, int16_t& held, int16_t next, int style, NoteAudit& audit) {
	const auto off = [&](int16_t key) {
		++audit.hostOffs;
		switch (style) {
		case 1: ev.note(CLAP_EVENT_NOTE_OFF, key, -1, 0); break;
		case 2: ev.note(CLAP_EVENT_NOTE_OFF, key, -1, -1); break;
		default: ev.note(CLAP_EVENT_NOTE_OFF, key); break;
		}
	};
	if (style == 3 && held >= 0 && held != next) {
		off(held); // the off FIRST: no overlap
	}
	ev.note(CLAP_EVENT_NOTE_ON, next);
	++audit.hostOns;
	if (style != 3 && held >= 0 && held != next) {
		off(held);
	}
	held = next;
}

/// **THE SOAK**: the Pattern in TB-Glide on the strip, a key always held and the next one pressed BEFORE the last is let
/// go (a legato slide), the Engine walked through every engine. One `Window` per `windowSeconds`, and the audits.
SoakResult soak(const SoakPlan& plan) {
	namespace pt = feathers::mod::arp::pat;
	namespace gn = feathers::mod::arp::genes;
	SoakResult result;
	NoteAudit& audit = result.notes;
	auto inst = std::make_unique<Instance>(plan.block);
	const uint32_t slot = inst->chain()->add_mod_device(inst->plugin(), pt::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	REQUIRE(inst->chain()->set_control(inst->plugin(), slot | CLAP_FEATHERS_TEST_STRIP, "P1 Mode",
	                                   static_cast<double>(gn::Mode::TbGlide)));
	// A PATCH THAT MODULATES (plan.modulators): an LFO, a DAHDSR and an MSEG on the strip, each routed to a voice
	// parameter, and a few core routes — so modulator state, rings and route resolution are in the soak too.
	if (plan.modulators) {
		int routed = 0;
		uint32_t dest = 0;
		for (const char* tag : { "LFO", "DHDS", "MSEG" }) {
			const uint32_t m = inst->chain()->add_mod_device(inst->plugin(), tag);
			MESSAGE("add " << tag << " -> " << m);
			if (m == UINT32_MAX) {
				continue;
			}
			for (int tries = 0; tries < 16; ++tries, ++dest) {
				const std::string control = "#" + std::to_string(dest);
				if (inst->chain()->set_device_route(inst->plugin(), m, 0, slot | CLAP_FEATHERS_TEST_STRIP, control.c_str(), 0.3)) {
					++routed;
					++dest;
					break;
				}
			}
		}
		for (uint32_t s = 0; s < 4; ++s) {
			routed += inst->chain()->set_core_route(inst->plugin(), s, s + 1, 0.2) ? 1 : 0;
		}
		MESSAGE("modulator routes made: " << routed);
		REQUIRE(routed >= 3);
	}
	inst->start();
	// THE VOICE MODE AND GLIDE, as a host param write in the first block (the reported session played Mono, probably Legato with glide).
	if (plan.voiceMode >= 0 || plan.glide >= 0.0) {
		Events mode;
		if (plan.voiceMode >= 0) {
			mode.param(static_cast<clap_id>(feathers::ParamId::VoiceMode), static_cast<double>(plan.voiceMode));
		}
		if (plan.glide >= 0.0) {
			mode.param(static_cast<clap_id>(feathers::ParamId::GlideTime), plan.glide);
		}
		(void)inst->run(mode);
	}

	const auto blocksOf = [&](double seconds) { return static_cast<int64_t>(seconds * kRate / plan.block); };
	const int64_t total = blocksOf(plan.seconds);
	const int64_t perWindow = std::max<int64_t>(1, blocksOf(plan.windowSeconds));
	const int64_t perKey = std::max<int64_t>(2, blocksOf(plan.keySeconds));
	const int64_t perEngine = std::max<int64_t>(1, blocksOf(plan.engineSeconds));
	const int64_t releaseAt = plan.endWithRelease ? total - perWindow : total + 1;
	const int64_t saveAt = plan.saveAt >= 0.0 ? blocksOf(plan.saveAt) : -1;
	const size_t engines = feathers::kOscTypeNames.size();

	Window w;
	std::vector<double> micros;
	std::vector<std::tuple<int64_t, size_t, double>> cpuByEngine; // (block, engine, thread µs)
	uint64_t allocsAtWindow = gProcessAllocations.load();
	int16_t held = -1;
	size_t keyAt = 0;
	size_t changes = 0;
	size_t engine = static_cast<size_t>(feathers::OscType::V5);
	std::array<int, 128> open {}; // ons handed the voices, per key, not yet ended
	const auto account = [&](const std::vector<std::array<int64_t, 3>>& events) {
		for (const auto& e : events) {
			const size_t k = static_cast<size_t>(e[1] & 127);
			if (e[0] == 0) {
				++audit.voiceOns;
				++open[k];
			}
			else if (e[0] == 1 || e[0] == 2) {
				++(e[0] == 1 ? audit.voiceOffs : audit.voiceChokes);
				if (open[k] > 0) {
					--open[k];
				}
				else if (e[0] == 1) {
					++audit.strayOffs; // a choke after its off is the synth ending a voice it already let go — not counted
				}
			}
		}
	};
	(void)drainDelivered(*inst);
	for (int64_t b = 0; b < total; ++b) {
		Events ev;
		if (b >= plan.firstKeyBlock && b < releaseAt) {
			bool switchNow = plan.switchEngines && b > 0 && b % perEngine == 0;
			if ((b - plan.firstKeyBlock) % perKey == 0) {
				const int style = plan.offStyle == 4 ? static_cast<int>(changes % 4) : plan.offStyle;
				slideTo(ev, held, kSoakKeys[keyAt++ % kSoakKeys.size()], style, audit);
				++changes;
				if (plan.engineOnKeyEvery > 0 && changes % static_cast<size_t>(plan.engineOnKeyEvery) == 0) {
					switchNow = true;
				}
			}
			if (switchNow) {
				engine = (engine + 1) % engines;
				ev.param(static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(engine));
			}
		}
		else if (b == releaseAt && held >= 0) {
			ev.note(CLAP_EVENT_NOTE_OFF, held);
			++audit.hostOffs;
			held = -1;
		}
		micros.push_back(inst->run(ev));
		cpuByEngine.emplace_back(b, engine, micros.back());
		account(drainDelivered(*inst));
		// THE PATTERN'S INPUT: it holds exactly the one key the host holds, and that key is its newest.
		const double pHeld = inst->reading(CLAP_FEATHERS_SYNTH_PATTERN_HELD);
		const double pNewest = inst->reading(CLAP_FEATHERS_SYNTH_PATTERN_NEWEST);
		const double want = held >= 0 ? 1.0 : 0.0;
		if (pHeld != want || (held >= 0 && pNewest != held)) {
			if (audit.heldMismatchBlocks++ == 0) {
				char line[200];
				std::snprintf(line, sizeof line, "block %lld (%.3f s), engine %s: host holds %d, Pattern holds %.0f (newest %.0f, lowest %.0f)",
				              static_cast<long long>(b), static_cast<double>(b * plan.block) / kRate, feathers::kOscTypeNames[engine],
				              held, pHeld, pNewest, inst->reading(CLAP_FEATHERS_SYNTH_PATTERN_LOWEST));
				audit.firstHeldMismatch = line;
			}
		}
		for (uint32_t r = 0; r < CLAP_FEATHERS_SYNTH_READING_COUNT; ++r) {
			w.peak[r] = std::max(w.peak[r], inst->reading(r));
		}
		if ((b + 1) % perWindow == 0) {
			std::nth_element(micros.begin(), micros.begin() + static_cast<long>(micros.size() / 2), micros.end());
			w.medianMicros = micros[micros.size() / 2];
			const uint64_t now = gProcessAllocations.load();
			w.allocations = now - allocsAtWindow;
			allocsAtWindow = now;
			result.windows.push_back(w);
			w = Window {};
			micros.clear();
		}
		if (b == saveAt) {
			// **LIVE AGAINST RELOADED** (after a restart, octave settings were found to need re-adjusting
			// to sane values). The live instance saved here; a FRESH one loads it and stands at the same song position; both
			// are then played alike — the same key changes, no engine switch — and must hand their voices the same notes and
			// sound the same pitches. The first second is skipped: the live one is mid-slide from its own past.
			ReloadAudit& ra = result.reload;
			ra.ran = true;
			const std::vector<uint8_t> state = inst->save();
			Instance fresh(plan.block);
			REQUIRE(fresh.load(state));
			fresh.start();
			fresh.setBeats(inst->beats());
			// EIGHT BLOCKS BEFORE THE FIRST KEY, both instances alike: a key in a fresh instance's very first block passes the
			// Pattern by (reported 2026-11-09; its own case below), which is not what this comparison asks about.
			for (int warm = 0; warm < 8; ++warm) {
				(void)inst->run(Events {});
				(void)fresh.run(Events {});
			}
			account(drainDelivered(*inst));
			(void)drainDelivered(fresh);
			int16_t heldLive = held, heldFresh = -1;
			NoteAudit scratch;
			const int64_t cmpBlocks = blocksOf(plan.compareSeconds);
			const int64_t settle = blocksOf(1.0);
			for (int64_t c = 0; c < cmpBlocks; ++c) {
				Events evLive, evFresh;
				if (c % perKey == 0) {
					const int16_t next = kSoakKeys[(keyAt + static_cast<size_t>(c / perKey)) % kSoakKeys.size()];
					slideTo(evLive, heldLive, next, 0, audit);
					slideTo(evFresh, heldFresh, next, 0, scratch);
				}
				(void)inst->run(evLive);
				(void)fresh.run(evFresh);
				const auto live = drainDelivered(*inst);
				account(live);
				const auto other = drainDelivered(fresh);
				if (const char* range = std::getenv("FEATHERS_SOAK_EVENTS")) {
					const long from = std::atol(range);
					if (c >= from && c < from + 80) {
						for (const auto& e : live) {
							std::printf("  c %lld live  kind %lld key %lld at %lld\n", static_cast<long long>(c), static_cast<long long>(e[0]),
							            static_cast<long long>(e[1]), static_cast<long long>(e[2] % static_cast<int64_t>(plan.block)));
						}
						for (const auto& e : other) {
							std::printf("  c %lld fresh kind %lld key %lld at %lld\n", static_cast<long long>(c), static_cast<long long>(e[0]),
							            static_cast<long long>(e[1]), static_cast<long long>(e[2] % static_cast<int64_t>(plan.block)));
						}
					}
				}
				const auto onsOf = [&](const std::vector<std::array<int64_t, 3>>& events, const Instance& who) {
					std::vector<std::pair<int64_t, int64_t>> ons;
					for (const auto& e : events) {
						if (e[0] == 0) {
							ons.emplace_back(e[1], e[2] % static_cast<int64_t>(who.block()));
						}
					}
					return ons;
				};
				if (c < settle) {
					continue;
				}
				++ra.blocks;
				const auto a = onsOf(live, *inst);
				const auto o = onsOf(other, fresh);
				ra.ons += a.size();
				const auto pa = heldPitches(*inst);
				const auto po = heldPitches(fresh);
				const bool onDiff = a != o;
				const bool pitchDiff = pa != po;
				ra.onMismatches += onDiff ? 1 : 0;
				ra.pitchMismatches += pitchDiff ? 1 : 0;
				if ((onDiff || pitchDiff) && (ra.first.empty() || std::getenv("FEATHERS_SOAK_TRACE") != nullptr)) {
					const auto join = [](const std::vector<long>& v) {
						std::string s;
						for (const long x : v) {
							s += std::to_string(x) + " ";
						}
						return s;
					};
					ra.first = "compare block " + std::to_string(c) + " (engine " + feathers::kOscTypeNames[engine] + "): live ons "
					           + std::to_string(a.size()) + ", fresh ons " + std::to_string(o.size()) + "; live pitches [" + join(pa)
					           + "], fresh [" + join(po) + "]";
					if (std::getenv("FEATHERS_SOAK_TRACE") != nullptr) {
						std::printf("%s\n", ra.first.c_str());
					}
				}
			}
			held = heldLive;
			keyAt += static_cast<size_t>(cmpBlocks / perKey + 1);
		}
	}
	// PER ENGINE, THE FIRST QUARTER OF THE LOADED SPAN AGAINST THE LAST: a creep shows as a rise within one engine, which a
	// window median mixing engines can hide.
	if (std::getenv("FEATHERS_SOAK_PER_ENGINE") != nullptr) {
		const auto median = [](std::vector<double> v) {
			if (v.empty()) {
				return 0.0;
			}
			std::nth_element(v.begin(), v.begin() + static_cast<long>(v.size() / 2), v.end());
			return v[v.size() / 2];
		};
		for (size_t e = 0; e < engines; ++e) {
			std::vector<double> early, late;
			for (const auto& [blk, eng, us] : cpuByEngine) {
				if (eng == e && blk < releaseAt / 4) {
					early.push_back(us);
				}
				else if (eng == e && blk >= releaseAt * 3 / 4 && blk < releaseAt) {
					late.push_back(us);
				}
			}
			std::printf("engine %-12s first quarter %7.1f us (n %zu)  last quarter %7.1f us (n %zu)\n", feathers::kOscTypeNames[e],
			            median(early), early.size(), median(late), late.size());
		}
	}
	// THE FINAL INSTANT, after the release window: what is still standing.
	Window last;
	for (uint32_t r = 0; r < CLAP_FEATHERS_SYNTH_READING_COUNT; ++r) {
		last.peak[r] = inst->reading(r);
	}
	result.windows.push_back(last);
	for (size_t k = 0; k < open.size(); ++k) {
		if (open[k] > 0) {
			audit.openAtEnd += static_cast<uint64_t>(open[k]);
			audit.openKeys += std::to_string(k) + " ";
		}
	}
	if (std::getenv("FEATHERS_SOAK_DUMP") != nullptr) {
		for (uint32_t s = 0;; ++s) {
			double stage = 0, key = 0, id = 0;
			if (!inst->chain()->voice_reading(inst->plugin(), s, CLAP_FEATHERS_VOICE_STAGE, &stage)) {
				break;
			}
			(void)inst->chain()->voice_reading(inst->plugin(), s, CLAP_FEATHERS_VOICE_KEY, &key);
			(void)inst->chain()->voice_reading(inst->plugin(), s, CLAP_FEATHERS_VOICE_NOTE_ID, &id);
			if (stage != 0) {
				std::printf("voice %u stage %.0f key %.0f id %.0f\n", s, stage, key, id);
			}
		}
	}
	return result;
}

void printAudit(const SoakResult& r) {
	const NoteAudit& a = r.notes;
	std::printf("host ons %llu offs %llu | voices ons %llu offs %llu chokes %llu stray offs %llu | open at end %llu [%s]\n",
	            static_cast<unsigned long long>(a.hostOns), static_cast<unsigned long long>(a.hostOffs),
	            static_cast<unsigned long long>(a.voiceOns), static_cast<unsigned long long>(a.voiceOffs),
	            static_cast<unsigned long long>(a.voiceChokes), static_cast<unsigned long long>(a.strayOffs),
	            static_cast<unsigned long long>(a.openAtEnd), a.openKeys.c_str());
	std::printf("Pattern held-set mismatch blocks %llu%s%s\n", static_cast<unsigned long long>(a.heldMismatchBlocks),
	            a.firstHeldMismatch.empty() ? "" : ", first: ", a.firstHeldMismatch.c_str());
	if (r.reload.ran) {
		std::printf("reload: %llu blocks compared, %llu ons, on mismatches %llu, pitch mismatches %llu%s%s\n",
		            static_cast<unsigned long long>(r.reload.blocks), static_cast<unsigned long long>(r.reload.ons),
		            static_cast<unsigned long long>(r.reload.onMismatches), static_cast<unsigned long long>(r.reload.pitchMismatches),
		            r.reload.first.empty() ? "" : ", first: ", r.reload.first.c_str());
	}
	std::fflush(stdout);
}

void print(const std::vector<Window>& windows) {
	std::string line = "win  cpu_us  allocs";
	for (const char* n : kReadingNames) {
		line += std::string("  ") + n;
	}
	std::printf("%s\n", line.c_str());
	for (size_t i = 0; i < windows.size(); ++i) {
		const Window& w = windows[i];
		std::printf("%3zu %7.1f %7llu", i, w.medianMicros, static_cast<unsigned long long>(w.allocations));
		for (const double v : w.peak) {
			std::printf("  %5.0f", v);
		}
		std::printf("%s\n", i + 1 == windows.size() ? "   (final instant)" : "");
	}
	std::fflush(stdout);
}

} // namespace

TEST_CASE("soak [soak]: the Pattern in TB-Glide, keys slid legato, the Engine switched every 3 s across every engine, for "
          "ten minutes — everything flat after warm-up" * doctest::skip()) {
	SoakPlan plan;
	if (const char* s = std::getenv("FEATHERS_SOAK_SECONDS")) {
		plan.seconds = std::atof(s);
	}
	else {
		plan.seconds = 600.0;
	}
	plan.windowSeconds = 30.0;
	// THE NOTE ROAD'S VARIANTS (the rack session's suspects for a stale held key), by environment: FEATHERS_SOAK_OFFS
	// (SoakPlan::offStyle), FEATHERS_SOAK_ENGINE_ON_KEY (an engine switch in the block of every Nth key change),
	// FEATHERS_SOAK_SAVE_AT (seconds: live against reloaded).
	if (const char* s = std::getenv("FEATHERS_SOAK_OFFS")) {
		plan.offStyle = std::atoi(s);
	}
	if (const char* s = std::getenv("FEATHERS_SOAK_ENGINE_ON_KEY")) {
		plan.engineOnKeyEvery = std::atoi(s);
	}
	if (const char* s = std::getenv("FEATHERS_SOAK_VOICE_MODE")) {
		plan.voiceMode = std::atoi(s);
	}
	plan.modulators = std::getenv("FEATHERS_SOAK_MODS") != nullptr;
	if (const char* s = std::getenv("FEATHERS_SOAK_GLIDE")) {
		plan.glide = std::atof(s);
	}
	if (const char* s = std::getenv("FEATHERS_SOAK_SAVE_AT")) {
		plan.saveAt = std::atof(s);
	}
	const SoakResult r = soak(plan);
	const auto& windows = r.windows;
	print(windows);
	printAudit(r);
	CHECK(r.notes.heldMismatchBlocks == 0);
	CHECK(r.notes.strayOffs == 0);
	CHECK(r.notes.openAtEnd == 0);
	CHECK(r.reload.onMismatches == 0);
	CHECK(r.reload.pitchMismatches == 0);
	REQUIRE(windows.size() >= 4);
	const Window& final = windows.back();
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_SOUNDING_VOICES] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_MANAGED_VOICES] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_KEYS_DOWN] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_LANE_INVENTED] == 0);
	for (size_t i = 1; i + 1 < windows.size(); ++i) {
		CAPTURE(i);
		CHECK(windows[i].allocations == 0);
	}
}

namespace {

/// Every voice slot's stage, counted: [Idle, Attack, Sustain, Release, Stealing].
std::array<int, 5> stagesOf(const Instance& inst) {
	std::array<int, 5> n {};
	for (uint32_t s = 0;; ++s) {
		double stage = 0;
		if (!inst.chain()->voice_reading(inst.plugin(), s, CLAP_FEATHERS_VOICE_STAGE, &stage)) {
			break;
		}
		++n[static_cast<size_t>(stage)];
	}
	return n;
}

} // namespace

TEST_CASE("a Drum One-shot hit let go on Drum and then carried onto another engine is released there: every voice returns to "
          "Idle, none sustains the new engine's sound for good") {
	// THE SOAK'S FIND (2026-11-08): `releaseVoice` leaves a One-shot voice in Sustain for the drum road to end at exact
	// silence, and that road asks only while the engine is Drum. Switched away first, the voice sustained the new engine at
	// its key until a steal took it — one more voice rendering per hit, which is the CPU that was seen to creep up.
	for (const auto& [to, name] : { std::pair { feathers::OscType::V5, "V5" }, std::pair { feathers::OscType::Additive, "Additive" },
	                                std::pair { feathers::OscType::Pluck, "Pluck" }, std::pair { feathers::OscType::Resonate, "Resonate" } }) {
		CAPTURE(name);
		Instance inst(256);
		inst.start();
		const auto engineTo = [](feathers::OscType t) {
			Events e;
			e.param(static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(t));
			return e;
		};
		(void)inst.run(engineTo(feathers::OscType::Drum));
		for (int b = 0; b < 8; ++b) {
			(void)inst.run(Events {});
		}
		Events on;
		on.note(CLAP_EVENT_NOTE_ON, 51); // pad H, the default kit's Ride: a 2 s hit
		(void)inst.run(on);
		Events off;
		off.note(CLAP_EVENT_NOTE_OFF, 51);
		(void)inst.run(off);
		// NON-VACUITY: on Drum the let-go One-shot is still held — Sustain, its key up — which is the state that leaked.
		REQUIRE(inst.reading(CLAP_FEATHERS_SYNTH_KEYS_DOWN) == 0);
		REQUIRE(inst.reading(CLAP_FEATHERS_SYNTH_SUSTAIN) + inst.reading(CLAP_FEATHERS_SYNTH_ATTACK) == 1);
		(void)inst.run(engineTo(to));
		// TWO SECONDS on the new engine: far past any release edge.
		for (int b = 0; b < static_cast<int>(2.0 * kRate / 256); ++b) {
			(void)inst.run(Events {});
		}
		const auto stages = stagesOf(inst);
		CHECK(stages[1] == 0);
		CHECK(stages[2] == 0);
		CHECK(stages[3] == 0);
		CHECK(stages[4] == 0);
		CHECK(inst.reading(CLAP_FEATHERS_SYNTH_SOUNDING_VOICES) == 0);
		CHECK(inst.reading(CLAP_FEATHERS_SYNTH_MANAGED_VOICES) == 0);
	}
}

TEST_CASE("and on Drum itself the let-go One-shot rings out and ends by the drum road — the fix leaves it ringing, not released "
          "at once") {
	Instance inst(256);
	inst.start();
	Events drum;
	drum.param(static_cast<clap_id>(feathers::ParamId::OscType), static_cast<double>(feathers::OscType::Drum));
	(void)inst.run(drum);
	for (int b = 0; b < 8; ++b) {
		(void)inst.run(Events {});
	}
	Events on;
	on.note(CLAP_EVENT_NOTE_ON, 51); // pad H, the default kit's Ride: a 2 s hit
	(void)inst.run(on);
	Events off;
	off.note(CLAP_EVENT_NOTE_OFF, 51);
	(void)inst.run(off);
	// A FEW BLOCKS LATER, STILL HELD: the One-shot plays its hit whatever the key did (drum proposal D4).
	for (int b = 0; b < 4; ++b) {
		(void)inst.run(Events {});
	}
	CHECK(stagesOf(inst)[2] == 1);
	for (int b = 0; b < static_cast<int>(20.0 * kRate / 256); ++b) {
		(void)inst.run(Events {});
	}
	CHECK(inst.reading(CLAP_FEATHERS_SYNTH_SOUNDING_VOICES) == 0);
	CHECK(inst.reading(CLAP_FEATHERS_SYNTH_MANAGED_VOICES) == 0);
}

TEST_CASE("the soak, short: the Pattern in TB-Glide, keys slid legato, the Engine switched every 3 s through every engine "
          "twice — then every key let go: every voice Idle, no key down, no invented note, nothing allocated in process()") {
	SoakPlan plan;
	plan.seconds = 60.0;
	plan.windowSeconds = 12.0;
	plan.engineSeconds = 3.0;
	const SoakResult r = soak(plan);
	const auto& windows = r.windows;
	CHECK(r.notes.heldMismatchBlocks == 0);
	CHECK(r.notes.strayOffs == 0);
	CHECK(r.notes.openAtEnd == 0);
	CHECK(r.notes.voiceOns >= 60); // non-vacuity: the Pattern played
	REQUIRE(windows.size() == 6);
	const Window& final = windows.back();
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_SOUNDING_VOICES] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_MANAGED_VOICES] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_KEYS_DOWN] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_LANE_INVENTED] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_LANE_MOD_RINGS] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_LANE_RETIRED] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_LANE_PENDING] == 0);
	for (size_t i = 0; i + 1 < windows.size(); ++i) {
		CAPTURE(i);
		CHECK(windows[i].allocations == 0);
	}
	// NON-VACUITY: the soak played — voices sounded and a key was down in every loaded window.
	for (size_t i = 0; i + 2 < windows.size(); ++i) {
		CAPTURE(i);
		CHECK(windows[i].peak[CLAP_FEATHERS_SYNTH_SOUNDING_VOICES] >= 1);
		CHECK(windows[i].peak[CLAP_FEATHERS_SYNTH_KEYS_DOWN] >= 1);
	}
}

TEST_CASE("the note road, audited: every way a key is let go (its own id, note id -1, note id and channel -1, off before the "
          "next on) with an engine switch in the block of every third key change — the Pattern holds exactly the host's key, "
          "every on the voices are handed is ended, and a save loaded into a fresh instance plays the same notes at the same "
          "pitches") {
	SoakPlan plan;
	plan.seconds = 48.0;
	plan.windowSeconds = 12.0;
	plan.offStyle = 4;
	plan.engineOnKeyEvery = 3;
	plan.saveAt = 30.0;
	const SoakResult r = soak(plan);
	printAudit(r);
	CHECK(r.notes.heldMismatchBlocks == 0);
	CHECK(r.notes.strayOffs == 0);
	CHECK(r.notes.openAtEnd == 0);
	CHECK(r.notes.voiceOns >= 60);
	REQUIRE(r.reload.ran);
	CHECK(r.reload.ons >= 10); // non-vacuity: the compared span played
	CHECK(r.reload.onMismatches == 0);
	CHECK(r.reload.pitchMismatches == 0);
	const Window& final = r.windows.back();
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_SOUNDING_VOICES] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_KEYS_DOWN] == 0);
	CHECK(final.peak[CLAP_FEATHERS_SYNTH_PATTERN_HELD] == 0);
}

TEST_CASE("a key pressed in the FIRST block after start, with the Pattern on the strip, is the Pattern's: it never reaches the "
          "voices on its own, and its off leaves nothing sounding") {
	// RED ON 2026-11-09, REPORTED BEFORE FIXING (the lead routes note-road causes): in the FIRST block after an instance's
	// first start the host's key passes the Pattern by — it reaches the voices at sample 0 as the host's own note, the Pattern
	// never holds it, and its off is swallowed, so the voice sounds for good (settle 0). After a state LOAD into a running
	// instance a key in the next block is lost on the Pattern's side instead (settle 12: held 0, nothing stuck). Every other
	// settle is green. Run with --no-skip.
	namespace pt = feathers::mod::arp::pat;
	// 0..3: the key that many blocks after the first start; 10: after 8 blocks and a host sleep-and-wake; 11: after a restart;
	// 12: after a state LOAD into the running instance.
	for (const int settle : { 0, 1, 2, 8, 10, 11, 12 }) {
		CAPTURE(settle);
		Instance inst(512);
		const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), pt::kTraits.tag);
		REQUIRE(slot != UINT32_MAX);
		inst.start();
		for (int b = 0; b < (settle >= 10 ? 8 : settle); ++b) {
			(void)inst.run(Events {});
		}
		if (settle == 10) {
			inst.wake();
		}
		else if (settle == 11) {
			inst.restart();
		}
		else if (settle == 12) {
			REQUIRE(inst.load(inst.save()));
		}
		(void)drainDelivered(inst);
		Events on;
		on.note(CLAP_EVENT_NOTE_ON, 60);
		(void)inst.run(on);
		// THE HOST'S OWN NOTE (id 60) NEVER REACHES THE VOICES: whatever they are handed in this block is the Pattern's.
		int passed = 0;
		for (const auto& e : drainDelivered(inst)) {
			passed += (e[0] == 0 && e[2] % 512 == 0) ? 1 : 0;
		}
		double held = 0;
		CHECK(inst.chain()->synth_reading(inst.plugin(), CLAP_FEATHERS_SYNTH_PATTERN_HELD, &held));
		CHECK(held == 1);
		for (int b = 0; b < 40; ++b) {
			(void)inst.run(Events {});
		}
		Events off;
		off.note(CLAP_EVENT_NOTE_OFF, 60);
		(void)inst.run(off);
		for (int b = 0; b < static_cast<int>(3.0 * kRate / 512); ++b) {
			(void)inst.run(Events {});
		}
		CHECK(inst.reading(CLAP_FEATHERS_SYNTH_SOUNDING_VOICES) == 0);
		CHECK(inst.reading(CLAP_FEATHERS_SYNTH_PATTERN_HELD) == 0);
		MESSAGE("settle " << settle << ": ons at sample 0 of the key's block " << passed);
	}
}
