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

// DROPPED NOTES, COUNTED: the whole plugin through its CLAP entry, driven by a host note stream and by the Pattern on the
// transport, at many block sizes and seeds, with an editor thread posting keys at the same time. Every note is followed
// to the voices: what the host sent, what the voices were handed (`delivered_note`), and what the voice manager made of
// it (`synth_reading`'s voice counts). A drop is a host On with no voice start, or an On or Off missing from the
// per-key sequence the voices were handed.
//
// The cases run a few seeds under ctest. `FEATHERS_NOTE_DROPS_RUNS=<n>` runs n seeds per configuration and prints the
// tally, which is how a drop RATE is measured rather than one green run believed.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "clap/clap.h"

#include "mod/arp/humanize.h"
#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_types.h"
#include "plugin/feathers_test_chain.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
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

namespace {

const void* CLAP_ABI hostGetExtension(const clap_host*, const char*) {
	return nullptr;
}
void CLAP_ABI hostRequestRestart(const clap_host*) {}
void CLAP_ABI hostRequestProcess(const clap_host*) {}
std::atomic<bool> gCallbackRequested { false };
void CLAP_ABI hostRequestCallback(const clap_host*) {
	gCallbackRequested.store(true);
}

struct SaveSink {
	std::vector<uint8_t> bytes;
	static int64_t CLAP_ABI write(const clap_ostream* s, const void* buffer, uint64_t size) {
		auto* self = static_cast<SaveSink*>(s->ctx);
		const auto* from = static_cast<const uint8_t*>(buffer);
		self->bytes.insert(self->bytes.end(), from, from + size);
		return static_cast<int64_t>(size);
	}
};

const clap_host kHost = {
	CLAP_VERSION_INIT, nullptr, "feathers note-drop tests", "Owlet Records", "", "1.0",
	hostGetExtension, hostRequestRestart, hostRequestProcess, hostRequestCallback,
};

bool CLAP_ABI dropOutput(const clap_output_events*, const clap_event_header*) {
	return true;
}

int runsWanted(int fallback) {
	if (const char* s = std::getenv("FEATHERS_NOTE_DROPS_RUNS"); s != nullptr && std::atoi(s) > 0) {
		return std::atoi(s);
	}
	return fallback;
}

/// ONE HOST EVENT: a note or a transport, at a frame offset in its block.
struct Events {
	std::vector<clap_event_note> notes;
	std::vector<clap_event_transport> transports;
	std::vector<const clap_event_header*> order;
	void build() {
		order.clear();
		for (const auto& n : notes) {
			order.push_back(&n.header);
		}
		for (const auto& t : transports) {
			order.push_back(&t.header);
		}
		std::stable_sort(order.begin(), order.end(),
		                 [](const clap_event_header* a, const clap_event_header* b) { return a->time < b->time; });
	}
	static uint32_t CLAP_ABI size(const clap_input_events* list) {
		return static_cast<uint32_t>(static_cast<const Events*>(list->ctx)->order.size());
	}
	static const clap_event_header* CLAP_ABI get(const clap_input_events* list, uint32_t i) {
		return static_cast<const Events*>(list->ctx)->order[i];
	}
	clap_input_events input() const { return { const_cast<Events*>(this), size, get }; }
};

/// What the voices were handed, by kind and key, oldest first.
struct Handed {
	uint32_t kind;
	int32_t key;
	uint64_t sample;
};

class Instance {
public:
	explicit Instance(uint32_t maxFrames) : maxFrames_(maxFrames) {
		REQUIRE(clap_entry.init("feathers-note-drop-test"));
		const auto* factory = static_cast<const clap_plugin_factory*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
		REQUIRE(factory != nullptr);
		plugin_ = factory->create_plugin(factory, &kHost, factory->get_plugin_descriptor(factory, 0)->id);
		REQUIRE(plugin_ != nullptr);
		REQUIRE(plugin_->init(plugin_));
		chain_ = static_cast<const clap_plugin_feathers_test_chain*>(plugin_->get_extension(plugin_, CLAP_EXT_FEATHERS_TEST_CHAIN));
		REQUIRE(chain_ != nullptr);
		left_.assign(maxFrames, 0.0f);
		right_.assign(maxFrames, 0.0f);
	}
	~Instance() {
		if (started_) {
			plugin_->stop_processing(plugin_);
			plugin_->deactivate(plugin_);
		}
		plugin_->destroy(plugin_);
		clap_entry.deinit();
	}
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	const clap_plugin* plugin() const { return plugin_; }
	const clap_plugin_feathers_test_chain* chain() const { return chain_; }

	void start() {
		REQUIRE(plugin_->activate(plugin_, 48000.0, 1, maxFrames_));
		REQUIRE(plugin_->start_processing(plugin_));
		started_ = true;
		REQUIRE(chain_->delivered_note(plugin_, UINT32_MAX, nullptr, nullptr, nullptr));
	}
	/// One block of `frames` with `events`; what the voices were handed is appended to `handed`.
	void run(uint32_t frames, const Events& events, const clap_event_transport* transport, std::vector<Handed>* handed) {
		const clap_input_events in = events.input();
		const clap_output_events out { nullptr, dropOutput };
		float* channels[2] = { left_.data(), right_.data() };
		clap_audio_buffer output {};
		output.data32 = channels;
		output.channel_count = 2;
		clap_process process {};
		process.steady_time = steady_;
		process.frames_count = frames;
		process.audio_outputs = &output;
		process.audio_outputs_count = 1;
		process.in_events = &in;
		process.out_events = &out;
		process.transport = transport;
		steady_ += frames;
		(void)plugin_->process(plugin_, &process);
		uint32_t kind = 0;
		int32_t key = 0;
		uint64_t sample = 0;
		for (uint32_t i = 0; chain_->delivered_note(plugin_, i, &kind, &key, &sample); ++i) {
			if (handed != nullptr && kind <= 2) {
				handed->push_back(Handed { kind, key, sample });
			}
		}
		REQUIRE(chain_->delivered_note(plugin_, UINT32_MAX, nullptr, nullptr, nullptr));
	}
	double reading(uint32_t what) const {
		double v = -1.0;
		REQUIRE(chain_->synth_reading(plugin_, what, &v));
		return v;
	}

private:
	const clap_plugin* plugin_ = nullptr;
	const clap_plugin_feathers_test_chain* chain_ = nullptr;
	std::vector<float> left_, right_;
	uint32_t maxFrames_;
	int64_t steady_ = 0;
	bool started_ = false;
};

clap_event_note noteEvent(uint16_t type, uint32_t time, int16_t key, int32_t id) {
	clap_event_note e {};
	e.header.size = sizeof(e);
	e.header.time = time;
	e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	e.header.type = type;
	e.note_id = id;
	e.port_index = 0;
	e.channel = 0;
	e.key = key;
	e.velocity = 0.8;
	return e;
}

clap_event_transport transportAt(double beats, double bpm, bool playing) {
	clap_event_transport t {};
	t.header.size = sizeof(t);
	t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
	t.header.type = CLAP_EVENT_TRANSPORT;
	t.flags = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | (playing ? static_cast<uint32_t>(CLAP_TRANSPORT_IS_PLAYING) : 0u);
	t.tempo = bpm;
	t.song_pos_beats = static_cast<clap_beattime>(std::llround(beats * CLAP_BEATTIME_FACTOR));
	return t;
}

/// THE BLOCK SIZES A RUN USES: a fixed one, or (0) a different one every block, as a host splitting its blocks at
/// automation does.
const std::vector<uint32_t> kBlockSizes = { 32, 64, 256, 1024, 37, 113, 999, 0 };

uint32_t nextBlock(uint32_t fixed, uint32_t maxFrames, std::mt19937& rng) {
	if (fixed != 0) {
		return fixed;
	}
	return std::uniform_int_distribution<uint32_t>(1, maxFrames)(rng);
}

//------------------------------------------------------------------------------------------------//
// (a) A HOST NOTE STREAM                                                                           //
//------------------------------------------------------------------------------------------------//

struct HostNote {
	int64_t on = 0;
	int64_t off = 0;
	int16_t key = 60;
	int32_t id = 0;
};

struct Tally {
	uint64_t runs = 0;
	uint64_t notes = 0;
	uint64_t droppedOns = 0;   ///< host ons the voices were not handed, or handed and no voice started
	uint64_t droppedOffs = 0;  ///< host offs the voices were not handed
	uint64_t refused = 0;      ///< voices asked for and not given
	uint64_t stuck = 0;        ///< voices still held after every note was let go
	uint64_t badRuns = 0;
	uint64_t uiPosted = 0;
	uint64_t uiRefused = 0;
	void print(const char* what) const {
		std::printf("%s: runs %llu, notes %llu, dropped ons %llu, dropped offs %llu, refused %llu, stuck %llu, bad runs %llu, "
		            "ui posted %llu, ui refused by a full ring %llu\n",
		            what, static_cast<unsigned long long>(runs), static_cast<unsigned long long>(notes),
		            static_cast<unsigned long long>(droppedOns), static_cast<unsigned long long>(droppedOffs),
		            static_cast<unsigned long long>(refused), static_cast<unsigned long long>(stuck),
		            static_cast<unsigned long long>(badRuns), static_cast<unsigned long long>(uiPosted),
		            static_cast<unsigned long long>(uiRefused));
	}
};

/// ONE RUN OF A HOST STREAM: `count` notes over `length` samples on a few keys, landing anywhere — on block edges on purpose
/// — overlapping on one key, some On and Off at one sample, some shorter than a block; and, with `editor`, a second thread
/// posting preview keys (on keys no host note uses) the whole time.
void hostStreamRun(uint32_t seed, uint32_t fixedBlock, bool editor, Tally& tally) {
	std::mt19937 rng(seed);
	constexpr int64_t kLength = 48000 * 2;
	constexpr int kCount = 320;
	const uint32_t maxFrames = 1024;
	Instance inst(maxFrames);
	inst.start();
	if (std::getenv("FEATHERS_NOTE_DROPS_DEBUG")) { inst.run(64, Events {}, nullptr, nullptr); std::printf("start: managed %g sounding %g att %g sus %g rel %g steal %g keys %g\n", inst.reading(0), inst.reading(1), inst.reading(2), inst.reading(3), inst.reading(4), inst.reading(5), inst.reading(6)); }

	// THE BLOCK EDGES, decided first so notes can be put on them.
	std::vector<int64_t> edges { 0 };
	{
		std::mt19937 blockRng(seed ^ 0xB10Cu);
		while (edges.back() < kLength + 48000) {
			edges.push_back(edges.back() + nextBlock(fixedBlock, maxFrames, blockRng));
		}
	}
	std::vector<HostNote> notes;
	std::uniform_int_distribution<int64_t> where(0, kLength);
	std::uniform_int_distribution<int> keyOf(0, 5);
	std::uniform_int_distribution<int> shape(0, 99);
	const auto lastEdge = static_cast<size_t>(std::upper_bound(edges.begin(), edges.end(), kLength) - edges.begin()) - 1;
	std::uniform_int_distribution<size_t> edgeOf(1, lastEdge);
	for (int i = 0; i < kCount; ++i) {
		HostNote n;
		const int s = shape(rng);
		if (s < 20) {
			n.on = edges[edgeOf(rng)]; // the first sample of a block
		}
		else if (s < 35) {
			n.on = edges[edgeOf(rng)] - 1; // the last sample of a block
		}
		else {
			n.on = where(rng);
		}
		const int d = shape(rng);
		int64_t length = 0;
		if (d < 15) {
			length = 0; // On and Off at one sample
		}
		else if (d < 40) {
			length = std::uniform_int_distribution<int64_t>(1, 63)(rng);
		}
		else if (d < 55) {
			// TO A BLOCK EDGE: the off on the next block's first sample
			const auto next = std::upper_bound(edges.begin(), edges.end(), n.on);
			length = *next - n.on;
		}
		else {
			length = std::uniform_int_distribution<int64_t>(64, 6000)(rng);
		}
		n.off = n.on + length; // the blocks run a second past kLength, so every off is sent
		n.key = static_cast<int16_t>(48 + 3 * keyOf(rng));
		n.id = i;
		notes.push_back(n);
	}

	// THE HOST'S EVENTS IN ITS ORDER: by time; at one sample, the offs of earlier notes, then the ons, then the offs of
	// notes struck at that sample (an On and Off at one sample keeps its order).
	struct Ev {
		int64_t time;
		int phase;
		int seq;
		uint16_t type;
		int16_t key;
		int32_t id;
	};
	std::vector<Ev> events;
	for (int i = 0; i < kCount; ++i) {
		const HostNote& n = notes[static_cast<size_t>(i)];
		events.push_back(Ev { n.on, 1, i, CLAP_EVENT_NOTE_ON, n.key, n.id });
		events.push_back(Ev { n.off, n.off == n.on ? 2 : 0, i, CLAP_EVENT_NOTE_OFF, n.key, n.id });
	}
	std::sort(events.begin(), events.end(),
	          [](const Ev& a, const Ev& b) { return std::tie(a.time, a.phase, a.seq) < std::tie(b.time, b.phase, b.seq); });
	// WHAT EACH KEY SHOULD BE HANDED, in order.
	std::map<int, std::vector<uint32_t>> expected;
	for (const Ev& e : events) {
		expected[e.key].push_back(e.type == CLAP_EVENT_NOTE_ON ? 0u : 1u);
	}

	// THE EDITOR: a second thread posting preview keys 100..107 while the blocks run, each an on and then its off.
	std::atomic<bool> stop { false };
	std::atomic<uint64_t> uiPostedOns { 0 }, uiPostedOffs { 0 }, uiRefused { 0 };
	std::thread poster;
	if (editor) {
		poster = std::thread([&] {
			std::mt19937 r(seed * 7919u + 1);
			while (!stop.load(std::memory_order_relaxed)) {
				const int key = 100 + static_cast<int>(r() % 8);
				if (inst.chain()->post_note(inst.plugin(), key, true, 0.7f)) {
					uiPostedOns.fetch_add(1);
					std::this_thread::sleep_for(std::chrono::microseconds(r() % 200));
					while (!inst.chain()->post_note(inst.plugin(), key, false, 0.0f)) {
						uiRefused.fetch_add(1);
						std::this_thread::yield();
						if (stop.load(std::memory_order_relaxed)) {
							break;
						}
					}
					uiPostedOffs.fetch_add(1);
				}
				else {
					uiRefused.fetch_add(1);
				}
				std::this_thread::sleep_for(std::chrono::microseconds(r() % 300));
			}
		});
	}

	std::vector<Handed> handed;
	size_t next = 0;
	for (size_t b = 0; b + 1 < edges.size(); ++b) {
		const int64_t start = edges[b];
		const auto frames = static_cast<uint32_t>(edges[b + 1] - start);
		Events block;
		while (next < events.size() && events[next].time < start + frames) {
			const Ev& e = events[next++];
			block.notes.push_back(noteEvent(e.type, static_cast<uint32_t>(e.time - start), e.key, e.id));
		}
		block.build();
		inst.run(frames, block, nullptr, &handed);
		if (editor && start >= kLength) {
			stop.store(true);
		}
	}
	stop.store(true);
	if (poster.joinable()) {
		poster.join();
	}
	// THE EDITOR'S LAST KEYS, drained.
	for (int b = 0; b < 4; ++b) {
		inst.run(256, Events {}, nullptr, &handed);
	}

	// PER KEY: what was handed against what was sent.
	std::map<int, std::vector<uint32_t>> got;
	uint64_t uiOns = 0, uiOffs = 0, hostOnsHanded = 0;
	for (const Handed& h : handed) {
		if (h.key >= 100) {
			(h.kind == 0 ? uiOns : uiOffs) += 1;
			continue;
		}
		got[h.key].push_back(h.kind);
		hostOnsHanded += h.kind == 0 ? 1 : 0;
	}
	bool bad = false;
	for (const auto& [key, want] : expected) {
		const std::vector<uint32_t>& have = got[key];
		const auto ons = [](const std::vector<uint32_t>& v) { return std::count(v.begin(), v.end(), 0u); };
		const auto offs = [](const std::vector<uint32_t>& v) { return std::count(v.begin(), v.end(), 1u); };
		if (ons(have) < ons(want)) {
			tally.droppedOns += static_cast<uint64_t>(ons(want) - ons(have));
		}
		if (offs(have) < offs(want)) {
			tally.droppedOffs += static_cast<uint64_t>(offs(want) - offs(have));
		}
		if (have != want) {
			bad = true;
		}
	}
	const auto starts = static_cast<uint64_t>(inst.reading(CLAP_FEATHERS_SYNTH_VOICE_STARTS)
	                                          + inst.reading(CLAP_FEATHERS_SYNTH_VOICE_RETRIGGERS));
	const uint64_t handedOns = hostOnsHanded + uiOns;
	if (starts < handedOns) {
		tally.droppedOns += handedOns - starts;
		bad = true;
	}
	const auto refused = static_cast<uint64_t>(inst.reading(CLAP_FEATHERS_SYNTH_VOICE_REFUSED));
	tally.refused += refused;
	if (std::getenv("FEATHERS_NOTE_DROPS_DEBUG")) { std::printf("end: managed %g sounding %g att %g sus %g rel %g steal %g keys %g\n", inst.reading(0), inst.reading(1), inst.reading(2), inst.reading(3), inst.reading(4), inst.reading(5), inst.reading(6)); for (const auto& [key, want] : expected) { const auto& have = got[key]; if (have != want) { std::printf("key %d want %zu have %zu\n", key, want.size(), have.size()); size_t i = 0; while (i < want.size() && i < have.size() && want[i] == have[i]) ++i; std::printf("  first diff at %zu\n", i); } } }
	// EVERY NOTE WAS LET GO: nothing may still be held.
	const auto held = static_cast<uint64_t>(inst.reading(CLAP_FEATHERS_SYNTH_ATTACK) + inst.reading(CLAP_FEATHERS_SYNTH_SUSTAIN));
	tally.stuck += held;
	if (uiOns != uiPostedOns.load() || uiOffs != uiPostedOffs.load()) {
		tally.droppedOns += uiPostedOns.load() > uiOns ? uiPostedOns.load() - uiOns : 0;
		tally.droppedOffs += uiPostedOffs.load() > uiOffs ? uiPostedOffs.load() - uiOffs : 0;
		bad = true;
	}
	tally.uiPosted += uiPostedOns.load();
	tally.uiRefused += uiRefused.load();
	tally.badRuns += (bad || refused > 0 || held > 0) ? 1 : 0;
	tally.notes += static_cast<uint64_t>(kCount);
	++tally.runs;
}

//------------------------------------------------------------------------------------------------//
// (b) THE PATTERN ON THE TRANSPORT                                                                 //
//------------------------------------------------------------------------------------------------//

struct PatternRun {
	std::vector<std::pair<int64_t, int>> ons;  ///< (sample, key) handed to the voices
	std::vector<std::pair<int64_t, int>> offs;
	uint64_t starts = 0;
	uint64_t refused = 0;
	uint64_t held = 0;
	bool pairsOk = true; ///< per key, the voices were handed On, Off, On, Off …
	std::vector<int64_t> wraps; ///< the samples the loop wrapped at
	double refusedFirstBlock = 0.0; ///< chain publishes the first block could not take whole
	double refusedAll = 0.0;
	double adoptMaxNs = 0.0; ///< the longest the audio thread's adopt path took
	double takeMaxNs = 0.0;   ///< and the take alone
};

/// MORE OF THE PATTERN'S CONTROLS for the next runs, by name (a case sets them and clears them).
std::vector<std::pair<std::string, double>> gPatternControls;

/// THE PATTERN IN THE STRIP, a chord held the whole run, the transport at `bpm` from beat `startBeat` and looping back by
/// `loopBeats` (0: no loop) — the host's loop wrap on its own sample (`wrapMode`). Block sizes as above.
PatternRun patternRun(uint32_t fixedBlock, uint32_t blockSeed, double bpm, double loopBeats, int wrapMode, double mode,
                      const std::vector<int16_t>& chord, int64_t length, int churn = 0, uint32_t playSeed = 0) {
	namespace pt = feathers::mod::arp::pat;
	const uint32_t maxFrames = 1024;
	Instance inst(maxFrames);
	const uint32_t slot = inst.chain()->add_mod_device(inst.plugin(), pt::kTraits.tag);
	REQUIRE(slot != UINT32_MAX);
	const uint32_t at = slot | CLAP_FEATHERS_TEST_STRIP;
	if (mode >= 0.0) {
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, "P1 Mode", mode));
	}
	for (const auto& [name, value] : gPatternControls) {
		REQUIRE(inst.chain()->set_control(inst.plugin(), at, name.c_str(), value));
	}
	inst.start();
	std::mt19937 rng(blockSeed);
	// THE EDITOR'S HAND MEANWHILE (`churn`): a knob on the lane's filter turned, and the Pattern's Latch written to the value it
	// already has — between blocks on this thread (1), or from a second thread while the blocks run (2).
	std::mt19937 churnRng(blockSeed * 13u + 7u);
	const auto touch = [&inst, at](std::mt19937& g) {
		const char* only = std::getenv("FEATHERS_NOTE_DROPS_TOUCH"); // "filter" or "latch": one hand alone (diagnosis)
		if (only == nullptr || std::strcmp(only, "filter") == 0) {
			(void)inst.chain()->set_control(inst.plugin(), 0, "Filter Cutoff", std::uniform_real_distribution<double>(200.0, 8000.0)(g));
		}
		if (only == nullptr || std::strcmp(only, "latch") == 0) {
			(void)inst.chain()->set_control(inst.plugin(), at, "Latch", 0.0);
		}
	};
	std::atomic<bool> stopChurn { false };
	std::atomic<bool> goChurn { std::getenv("FEATHERS_NOTE_DROPS_CHURN_AFTER") == nullptr };
	std::thread churner;
	if (churn >= 2) {
		churner = std::thread([&] {
			std::mt19937 g(blockSeed * 17u + 3u);
			while (!stopChurn.load(std::memory_order_relaxed)) {
				if (!goChurn.load()) {
					std::this_thread::yield();
					continue;
				}
				touch(g);
				// AND WHAT A HOST'S MAIN THREAD DOES BESIDE IT (3): the visit the plugin asked for, and now and then a save.
				// A HAND EDIT OF THE PATTERN (4): its hand block written whole, as the window's lane writes it — the default hand, so
				// the notes it should play do not change.
				if (churn == 4) {
					static const std::vector<float> words = [] {
						std::vector<float> w(feathers::mod::arp::pat::kHandWords, 0.0f);
						feathers::mod::arp::pat::writeHandState(feathers::mod::arp::pat::HandState {}, w.data());
						return w;
					}();
					if (!inst.chain()->set_device_block(inst.plugin(), at, "pattern_hand", words.data(), words.size())) {
						std::fprintf(stderr, "HAND WRITE REFUSED\n");
					}
				}
				if (churn == 3) {
					if (gCallbackRequested.exchange(false)) {
						inst.plugin()->on_main_thread(inst.plugin());
					}
					if (g() % 16 == 0) {
						const auto* state = static_cast<const clap_plugin_state*>(inst.plugin()->get_extension(inst.plugin(), CLAP_EXT_STATE));
						SaveSink sink;
						clap_ostream out { &sink, SaveSink::write };
						(void)state->save(inst.plugin(), &out);
					}
				}
				if (churn != 5) { // 5: a hand that never rests, to measure the race's mechanism at a rate
					std::this_thread::sleep_for(std::chrono::microseconds(g() % 500));
				}
			}
		});
	}
	std::vector<Handed> handed;
	PatternRun r;
	// THE HOST'S LOOP, exactly: wrap k lands on the first sample at or after k loop lengths, and the song position there is
	// what is left over. `wrapMode` 1 sends the wrap as a transport event inside the block; 2 cuts the block at it.
	const double samplesPerBeat = 48000.0 * 60.0 / bpm;
	const double loopSamples = loopBeats * samplesPerBeat;
	const auto wrapAt = [&](int64_t k) {
		return k == 0 ? int64_t { 0 } : static_cast<int64_t>(std::ceil(static_cast<double>(k) * loopSamples));
	};
	const auto beatsAt = [&](int64_t s) {
		if (loopBeats <= 0.0) {
			return static_cast<double>(s) / samplesPerBeat;
		}
		int64_t k = static_cast<int64_t>(std::floor(static_cast<double>(s) / loopSamples));
		while (wrapAt(k + 1) <= s) {
			++k;
		}
		while (k > 0 && wrapAt(k) > s) {
			--k;
		}
		return (static_cast<double>(s) - static_cast<double>(k) * loopSamples) / samplesPerBeat;
	};
	// A PLAYER (`playSeed`): from the second beat on, a new chord of one to four keys every half beat to two beats, the last
	// one let go at the same sample (or a little before), each key at its own offset of up to 40 samples.
	struct Played {
		int64_t at;
		uint16_t type;
		int16_t key;
		int32_t id;
	};
	std::vector<Played> played;
	if (playSeed != 0) {
		std::mt19937 p(playSeed);
		int64_t t = static_cast<int64_t>(samplesPerBeat);
		std::vector<std::pair<int16_t, int32_t>> down;
		int32_t id = 1000;
		while (t < length - 48000) {
			for (const auto& [k, i] : down) {
				played.push_back(Played { t - static_cast<int64_t>(p() % 30), CLAP_EVENT_NOTE_OFF, k, i });
			}
			down.clear();
			const int n = 1 + static_cast<int>(p() % 4);
			for (int j = 0; j < n; ++j) {
				const auto k = static_cast<int16_t>(48 + p() % 24);
				if (std::any_of(down.begin(), down.end(), [k](const auto& d) { return d.first == k; })) {
					continue;
				}
				down.emplace_back(k, id);
				played.push_back(Played { t + static_cast<int64_t>(p() % 40), CLAP_EVENT_NOTE_ON, k, id++ });
			}
			t += static_cast<int64_t>(samplesPerBeat * (0.5 + 0.25 * static_cast<double>(p() % 7)));
		}
		for (const auto& [k, i] : down) {
			played.push_back(Played { t, CLAP_EVENT_NOTE_OFF, k, i });
		}
		std::stable_sort(played.begin(), played.end(), [](const Played& a, const Played& b) { return a.at < b.at; });
	}
	size_t nextPlayed = 0;
	int64_t rendered = 0;
	int64_t nextWrap = 1;
	bool first = true;
	while (rendered < length) {
		uint32_t frames = nextBlock(fixedBlock, maxFrames, rng);
		Events block;
		if (first) {
			for (const int16_t k : chord) {
				block.notes.push_back(noteEvent(CLAP_EVENT_NOTE_ON, 0, k, k));
			}
			first = false;
		}
		if (loopBeats > 0.0) {
			while (wrapAt(nextWrap) <= rendered) {
				r.wraps.push_back(wrapAt(nextWrap++));
			}
			const int64_t w = wrapAt(nextWrap);
			if (w < rendered + frames) {
				if (wrapMode == 2) {
					frames = static_cast<uint32_t>(w - rendered); // the host cuts its block at the wrap
				}
				else {
					block.transports.push_back(transportAt(beatsAt(w), bpm, true));
					block.transports.back().header.time = static_cast<uint32_t>(w - rendered);
				}
			}
		}
		while (nextPlayed < played.size() && played[nextPlayed].at < rendered + frames) {
			const Played& e = played[nextPlayed++];
			block.notes.push_back(noteEvent(e.type, static_cast<uint32_t>(std::max<int64_t>(e.at - rendered, 0)), e.key, e.id));
		}
		if (churn == 1 && churnRng() % 8 == 0) {
			touch(churnRng);
		}
		block.build();
		const clap_event_transport transport = transportAt(beatsAt(rendered), bpm, true);
		inst.run(frames, block, &transport, &handed);
		if (rendered == 0) {
			r.refusedFirstBlock = inst.reading(CLAP_FEATHERS_SYNTH_CHAIN_ADOPT_REFUSED);
		}
		rendered += frames;
		if (const char* after = std::getenv("FEATHERS_NOTE_DROPS_CHURN_AFTER"); after != nullptr && rendered >= std::atoll(after)) {
			goChurn.store(true);
		}
	}
	stopChurn.store(true);
	if (churner.joinable()) {
		churner.join();
	}
	while (loopBeats > 0.0 && wrapAt(nextWrap) <= rendered) {
		r.wraps.push_back(wrapAt(nextWrap++));
	}
	std::map<int, uint32_t> last;
	for (const Handed& h : handed) {
		if (static_cast<int64_t>(h.sample) >= length) {
			break; // a block running past the run's length is not compared (the reference stops on it)
		}
		if (h.kind == 0) {
			r.ons.emplace_back(static_cast<int64_t>(h.sample), h.key);
		}
		else if (h.kind == 1) {
			r.offs.emplace_back(static_cast<int64_t>(h.sample), h.key);
		}
		// PAIRING: an On on a key the voices hold sounding was not ended first.
		if (h.kind == 0 && last.count(h.key) != 0 && last[h.key] == 0) {
			r.pairsOk = false;
		}
		last[h.key] = h.kind;
	}
	r.starts = static_cast<uint64_t>(inst.reading(CLAP_FEATHERS_SYNTH_VOICE_STARTS)
	                                 + inst.reading(CLAP_FEATHERS_SYNTH_VOICE_RETRIGGERS));
	r.refused = static_cast<uint64_t>(inst.reading(CLAP_FEATHERS_SYNTH_VOICE_REFUSED));
	r.refusedAll = inst.reading(CLAP_FEATHERS_SYNTH_CHAIN_ADOPT_REFUSED);
	r.adoptMaxNs = inst.reading(CLAP_FEATHERS_SYNTH_CHAIN_ADOPT_MAX_NS);
	r.takeMaxNs = inst.reading(CLAP_FEATHERS_SYNTH_CHAIN_TAKE_MAX_NS);
	return r;
}

/// THE DIFFERENCE BETWEEN TWO RUNS' ONS: how many of `reference`'s are missing from `run` (and the other way).
std::pair<size_t, size_t> onsMissing(const std::vector<std::pair<int64_t, int>>& reference,
                                     const std::vector<std::pair<int64_t, int>>& run) {
	std::multiset<std::pair<int64_t, int>> a(reference.begin(), reference.end());
	std::multiset<std::pair<int64_t, int>> b(run.begin(), run.end());
	size_t missing = 0, extra = 0;
	for (const auto& e : a) {
		if (b.count(e) < a.count(e)) {
			++missing;
		}
	}
	for (const auto& e : b) {
		if (a.count(e) < b.count(e)) {
			++extra;
		}
	}
	return { missing, extra };
}

} // namespace

TEST_CASE("note drops (a): a host note stream — block edges, same-key overlaps, On and Off at one sample, notes shorter than "
          "a block, an editor thread posting keys — every On and Off reaches the voices, every On starts a voice") {
	const int runs = runsWanted(2);
	Tally all;
	for (const uint32_t block : kBlockSizes) {
		for (const bool editor : { false, true }) {
			Tally t;
			for (int s = 0; s < runs; ++s) {
				hostStreamRun(static_cast<uint32_t>(1000 + s), block, editor, t);
			}
			char what[96];
			std::snprintf(what, sizeof what, "host stream, block %u%s, editor %s", block, block == 0 ? " (varied)" : "",
			              editor ? "on" : "off");
			t.print(what);
			CAPTURE(block);
			CAPTURE(editor);
			CHECK(t.droppedOns == 0);
			CHECK(t.droppedOffs == 0);
			CHECK(t.refused == 0);
			CHECK(t.stuck == 0);
			CHECK(t.badRuns == 0);
			all.runs += t.runs;
			all.notes += t.notes;
			all.droppedOns += t.droppedOns;
			all.droppedOffs += t.droppedOffs;
			all.badRuns += t.badRuns;
		}
	}
	all.print("host stream, all");
}


/// EVERY LOOP PASS AGAINST THE FIRST: the ons of each whole pass after it, as (samples from the wrap, key), against the first
/// pass's — a step lost at a wrap or a block edge is missing from its pass. One sample of slack, for the wrap's own rounding.
static std::pair<size_t, size_t> passesMissing(const std::vector<std::pair<int64_t, int>>& ons, const std::vector<int64_t>& wraps,
                                               size_t* passesCompared) {
	std::vector<int64_t> starts { 0 };
	starts.insert(starts.end(), wraps.begin(), wraps.end());
	std::vector<std::vector<std::pair<int64_t, int>>> passes(starts.size());
	for (const auto& [s, k] : ons) {
		const size_t p = static_cast<size_t>(std::upper_bound(starts.begin(), starts.end(), s) - starts.begin()) - 1;
		passes[p].emplace_back(s - starts[p], k);
	}
	const auto has = [](const std::vector<std::pair<int64_t, int>>& in, std::pair<int64_t, int> e) {
		return std::any_of(in.begin(), in.end(),
		                   [&](const auto& x) { return x.second == e.second && std::llabs(x.first - e.first) <= 1; });
	};
	size_t missing = 0, extra = 0;
	*passesCompared = 0;
	// THE LAST PASS IS CUT SHORT BY THE RUN'S END, so it is not compared.
	for (size_t p = 1; p + 1 < passes.size(); ++p) {
		++*passesCompared;
		for (const auto& e : passes[0]) {
			missing += has(passes[p], e) ? 0 : 1;
		}
		for (const auto& e : passes[p]) {
			extra += has(passes[0], e) ? 0 : 1;
		}
	}
	return { missing, extra };
}

TEST_CASE("note drops (b): the Pattern on a running, looping transport hands the voices the same ons at every block size and "
          "in every loop pass, every one starts a voice, and each key is ended before it is struck again") {
	const int runs = runsWanted(1);
	constexpr int64_t kLength = 48000 * 8;
	uint64_t totalOns = 0, totalMissing = 0, totalExtra = 0, totalUnstarted = 0, totalRefused = 0, badPairs = 0, compared = 0;
	uint64_t passOns = 0, passMissing = 0, passExtra = 0, passes = 0;
	for (int s = 0; s < runs; ++s) {
		std::mt19937 rng(static_cast<uint32_t>(77 + s));
		const double bpm = 90.0 + std::uniform_real_distribution<double>(0.0, 80.0)(rng);
		const double loop = s % 3 == 0 ? 0.0 : static_cast<double>(4 * (1 + rng() % 2));
		const int wrapMode = 1 + static_cast<int>(rng() % 2);
		const double mode = static_cast<double>(rng() % 5);
		const std::vector<int16_t> chord = { 60, 64, 67 };
		CAPTURE(s);
		CAPTURE(bpm);
		CAPTURE(loop);
		CAPTURE(wrapMode);
		CAPTURE(mode);
		// THE REFERENCE: one sample a block, so no block edge can fall anywhere but on a sample.
		const PatternRun reference = patternRun(1, 1, bpm, loop, wrapMode, mode, chord, kLength);
		CHECK(reference.ons.size() >= 8); // non-vacuity: the pattern plays
		for (const uint32_t block : kBlockSizes) {
			CAPTURE(block);
			const PatternRun run = patternRun(block, static_cast<uint32_t>(s * 31 + 5), bpm, loop, wrapMode, mode, chord, kLength);
			const auto [missing, extra] = onsMissing(reference.ons, run.ons);
			totalOns += reference.ons.size();
			totalMissing += missing;
			totalExtra += extra;
			totalUnstarted += run.starts < run.ons.size() ? run.ons.size() - run.starts : 0;
			totalRefused += run.refused;
			badPairs += run.pairsOk ? 0 : 1;
			++compared;
			CHECK(missing == 0);
			CHECK(extra == 0);
			CHECK(run.starts >= run.ons.size());
			CHECK(run.refused == 0);
			CHECK(run.pairsOk);
			if (loop > 0.0) {
				size_t n = 0;
				const auto [pm, pe] = passesMissing(run.ons, run.wraps, &n);
				passes += n;
				passMissing += pm;
				passExtra += pe;
				passOns += run.ons.size();
				CHECK(pm == 0);
				CHECK(pe == 0);
			}
		}
	}
	std::printf("pattern: runs %llu compared against block 1, reference ons %llu, missing %llu, extra %llu, unstarted %llu, "
	            "refused %llu, bad pairing %llu; loop passes %llu over %llu ons, missing %llu, extra %llu\n",
	            static_cast<unsigned long long>(compared), static_cast<unsigned long long>(totalOns),
	            static_cast<unsigned long long>(totalMissing), static_cast<unsigned long long>(totalExtra),
	            static_cast<unsigned long long>(totalUnstarted), static_cast<unsigned long long>(totalRefused),
	            static_cast<unsigned long long>(badPairs), static_cast<unsigned long long>(passes),
	            static_cast<unsigned long long>(passOns), static_cast<unsigned long long>(passMissing),
	            static_cast<unsigned long long>(passExtra));
}

TEST_CASE("note drops (c): the editor's hand on the lane while the Pattern plays — between blocks and from a second thread — "
          "loses no step and adds none") {
	const int runs = runsWanted(1);
	constexpr int64_t kLength = 48000 * 8;
	uint64_t compared = 0, refOns = 0, missingAll = 0, extraAll = 0;
	for (int s = 0; s < runs; ++s) {
		std::mt19937 rng(static_cast<uint32_t>(501 + s));
		const double bpm = 90.0 + std::uniform_real_distribution<double>(0.0, 80.0)(rng);
		const double mode = static_cast<double>(rng() % 5);
		const std::vector<int16_t> chord = { 60, 64, 67 };
		CAPTURE(s);
		CAPTURE(bpm);
		CAPTURE(mode);
		for (const uint32_t play : { 0u, static_cast<uint32_t>(9001 + s) }) {
		CAPTURE(play);
		const std::vector<int16_t> held = play == 0 ? chord : std::vector<int16_t> {};
		const PatternRun reference = patternRun(64, 1, bpm, 4.0, 1, mode, held, kLength, 0, play);
		CHECK(reference.ons.size() >= 8);
		for (const int churn : { 1, 2, 3, 4 }) {
			for (const uint32_t block : { 64u, 256u, 0u }) {
				CAPTURE(churn);
				CAPTURE(block);
				const PatternRun run = patternRun(block, static_cast<uint32_t>(s * 31 + 5), bpm, 4.0, 1, mode, held, kLength, churn, play);
				const auto [missing, extra] = onsMissing(reference.ons, run.ons);
				++compared;
				if (std::getenv("FEATHERS_NOTE_DROPS_DEBUG") != nullptr) {
					std::printf("  churn %d block %u: missing %zu extra %zu, adopt refused first block %g, all %g\n", churn, block, missing, extra, run.refusedFirstBlock, run.refusedAll);
				}
				if (std::getenv("FEATHERS_NOTE_DROPS_DEBUG") != nullptr && (missing + extra) > 0) {
					std::multiset<std::pair<int64_t, int>> b(run.ons.begin(), run.ons.end());
					for (const auto& e : reference.ons) {
						if (b.count(e) == 0) {
							std::printf("  missing %lld key %d\n", static_cast<long long>(e.first), e.second);
						}
					}
					std::multiset<std::pair<int64_t, int>> a(reference.ons.begin(), reference.ons.end());
					for (const auto& e : run.ons) {
						if (a.count(e) == 0) {
							std::printf("  extra %lld key %d\n", static_cast<long long>(e.first), e.second);
						}
					}
				}
				refOns += reference.ons.size();
				missingAll += missing;
				extraAll += extra;
				CHECK(missing == 0);
				CHECK(extra == 0);
				CHECK(run.refused == 0);
			}
		}
		}
	}
	std::printf("pattern with the editor's hand: runs %llu, reference ons %llu, missing %llu, extra %llu\n",
	            static_cast<unsigned long long>(compared), static_cast<unsigned long long>(refOns),
	            static_cast<unsigned long long>(missingAll), static_cast<unsigned long long>(extraAll));
}

TEST_CASE("note drops (d): a chord struck on the first block a just-added Pattern plays, while the editor publishes — the "
          "Pattern takes every key, so it plays what it plays with no hand on the editor") {
	// THE STARTUP RACE MEASURED: the Pattern is added before activate, a chord goes down on the first sample, and a second
	// thread writes a control the whole time. Each run is compared with the same run and no hand; the tally says how often
	// the first block found a publish it could not take whole, and how often the run lost steps.
	const int runs = runsWanted(8);
	constexpr int64_t kLength = 48000 * 3 / 2;
	const std::vector<int16_t> chord = { 60, 64, 67 };
	const double bpm = 120.0;
	const double mode = 2.0; // Chords: every held key is heard in what it plays
	const PatternRun reference = patternRun(64, 1, bpm, 0.0, 1, mode, chord, kLength);
	REQUIRE(reference.ons.size() >= 4);
	int refusedFirst = 0, dropRuns = 0, both = 0;
	double refusedBlocks = 0.0;
	double adoptMaxNs = 0.0;
	double takeMaxNs = 0.0;
	std::vector<double> takes;
	for (int s = 0; s < runs; ++s) {
		const PatternRun run = patternRun(64, 1, bpm, 0.0, 1, mode, chord, kLength, 2);
		const auto [missing, extra] = onsMissing(reference.ons, run.ons);
		const bool refused = run.refusedFirstBlock > 0.0;
		refusedBlocks += run.refusedAll;
		adoptMaxNs = std::max(adoptMaxNs, run.adoptMaxNs);
		takeMaxNs = std::max(takeMaxNs, run.takeMaxNs);
		takes.push_back(run.takeMaxNs);
		const bool dropped = missing + extra > 0;
		refusedFirst += refused ? 1 : 0;
		dropRuns += dropped ? 1 : 0;
		both += refused && dropped ? 1 : 0;
		CAPTURE(s);
		CHECK(run.refusedAll == 0.0); // NO BLOCK RUNS ON A STALE STATE: the adopt is never refused
		CHECK(missing == 0);
		CHECK(extra == 0);
	}
	std::printf("startup race: runs %d, first block refused a publish %d, runs that lost or added steps %d, both %d; blocks run on a "
	            "stale chain state %g; longest adopt path %.1f us, longest take %.1f us (median of the runs' longest %.2f us)\n",
	            runs, refusedFirst, dropRuns, both, refusedBlocks, adoptMaxNs / 1000.0, takeMaxNs / 1000.0,
	            takes.empty() ? 0.0 : (std::nth_element(takes.begin(), takes.begin() + static_cast<std::ptrdiff_t>(takes.size() / 2), takes.end()), takes[takes.size() / 2] / 1000.0));
}

TEST_CASE("note drops (e): the Pattern's feel — Humanize at full amount in every mode, Nudge either way — hands the voices the "
          "same ons at every block size and in every loop pass") {
	namespace hz = feathers::mod::arp::humanize;
	const int runs = runsWanted(1);
	constexpr int64_t kLength = 48000 * 2;
	uint64_t compared = 0, refOns = 0, missingAll = 0, extraAll = 0, passMissing = 0, passExtra = 0, passes = 0;
	for (int s = 0; s < runs; ++s) {
		for (int h = 0; h < static_cast<int>(hz::Mode::kCount); ++h) {
			if (const char* only = std::getenv("FEATHERS_NOTE_DROPS_HUMANIZE"); only != nullptr && std::atoi(only) != h) {
				continue;
			}
			std::mt19937 rng(static_cast<uint32_t>(3001 + s * 17 + h));
			const double bpm = 90.0 + std::uniform_real_distribution<double>(0.0, 80.0)(rng);
			const double nudge = std::uniform_real_distribution<double>(-0.5, 0.5)(rng);
			const double mode = static_cast<double>(rng() % 5);
			const int wrapMode = 1 + static_cast<int>(rng() % 2);
			gPatternControls = { { "Humanize", static_cast<double>(h) }, { "Humanize Amount", 1.0 }, { "Nudge", nudge } };
			CAPTURE(s);
			CAPTURE(h);
			CAPTURE(nudge);
			CAPTURE(bpm);
			CAPTURE(mode);
			const std::vector<int16_t> chord = { 60, 64, 67 };
			const PatternRun reference = patternRun(1, 1, bpm, 4.0, wrapMode, mode, chord, kLength);
			CHECK(reference.ons.size() >= 2);
			for (const uint32_t block : { 999u, 0u }) {
				CAPTURE(block);
				const PatternRun run = patternRun(block, static_cast<uint32_t>(s * 31 + h), bpm, 4.0, wrapMode, mode, chord, kLength);
				const auto [missing, extra] = onsMissing(reference.ons, run.ons);
				if (std::getenv("FEATHERS_NOTE_DROPS_DEBUG") != nullptr && missing + extra > 0) {
					for (const auto& e : reference.ons) {
						std::printf("  ref %lld %d\n", static_cast<long long>(e.first), e.second);
					}
					for (const auto& e : run.ons) {
						std::printf("  run %lld %d\n", static_cast<long long>(e.first), e.second);
					}
				}
				size_t n = 0;
				const auto [pm, pe] = passesMissing(run.ons, run.wraps, &n);
				++compared;
				refOns += reference.ons.size();
				missingAll += missing;
				extraAll += extra;
				passMissing += pm;
				passExtra += pe;
				passes += n;
				CHECK(missing == 0);
				CHECK(extra == 0);
				CHECK(run.refused == 0);
				CHECK(run.pairsOk);
			}
		}
	}
	gPatternControls.clear();
	std::printf("pattern feel: runs %llu, reference ons %llu, missing %llu, extra %llu; loop passes %llu, missing %llu, extra %llu\n",
	            static_cast<unsigned long long>(compared), static_cast<unsigned long long>(refOns),
	            static_cast<unsigned long long>(missingAll), static_cast<unsigned long long>(extraAll),
	            static_cast<unsigned long long>(passes), static_cast<unsigned long long>(passMissing),
	            static_cast<unsigned long long>(passExtra));
}
