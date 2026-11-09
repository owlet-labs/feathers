/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

// THE PATCH BENCHMARK — a WHOLE PLUGIN, stood up through clap_entry as a host stands one up, a patch loaded
// through the real state road (a `.fpreset`) or built through the test-chain seam, and timed per process()
// call in three phases: IDLE (no notes), HELD (notes down) and TAIL (released). NOT A TEST: ctest never runs it.
// bench_render prices the synth below the plugin; this prices what a host pays (the perf survey, 2026-10-29).
//
// IT IS BUILT FOR A LOADED HOST. Each block is timed twice: per-THREAD CPU (CLOCK_THREAD_CPUTIME_ID, the primary
// number — what the block cost this thread, preemption excluded) and wall time (steady_clock, which a busy host
// inflates). A wall spike with no thread-CPU spike under it is the host, not the plugin. The process's CPU over the
// held phase less this thread's is what OTHER threads spent (the additive phase optimiser's, A-PH). /proc/loadavg is
// printed with every run, and involuntary context switches (RUSAGE_THREAD) per phase say how often we were preempted.
//
// AND IT WATCHES THE AUDIO THREAD'S CONTRACT: every `operator new` and every pthread mutex lock/trylock made on the
// process() thread INSIDE process() is counted (the mutex calls are interposed by defining them in this executable,
// which the dynamic linker resolves first). Both counters prove themselves once at start-up (a counted new and a
// counted std::mutex lock must read 1), so a zero printed later is not a counter that cannot count.
//
// A PLAN, NOT A SHELL LOOP. `--plan FILE` holds one configuration per line (`label arg arg ...`, `#` comments), and
// `--rounds N` runs every line once per round, ROUND-ROBIN, each run a fresh plugin instance — so a load swing lands
// on every configuration alike instead of on whichever ran during it (memory: benchmarking on a shared host). The
// per-run rows go to stdout as CSV; the per-configuration summary over rounds (median and min of the per-run mean,
// p99 and max) goes to stderr at the end.
//
// A CONFIGURATION'S ARGS: `--preset PATH` (a .fpreset, decoded and loaded through clap_plugin_state), `--engine
// v5|additive|resonate` (Osc Type), `--voice-count N`, `--param ID=VALUE` (a host param, ID decimal or 0x hex),
// `--add TAG` (a lane device), `--mod TAG` (a strip modulator), `--set POS:Control=VALUE` (POS a lane position, or
// `sN` for strip slot N), `--route SRC:DST:AMT` (a core route), `--remove POS` (after everything else: the ablation a
// per-device breakdown is computed from), `--notes N` `--note K` `--step S` (N notes from key K, S semitones apart),
// `--frames F`, `--idle S` `--held S` `--tail S` (seconds per phase), `--automate ID` (that host param swept every
// block, a slow sine over its range), `--sidechain 1` (white noise at -12 dBFS on input 0, for a vocoder),
// `--reactivate 0` (skip the reactivate that ends every build), `--dump 1` (round 0's held blocks to `<label>.blocks`),
// `--save NAME` (round 0 saves the built patch through the browser's road as a `test` preset in the plugin bank's `Test`
// category under `$FEATHERS_PRESET_ROOT`; tests/bench_patch_plans/save-test-presets.plan makes the shipped set).
// A `--set` control name writes `_` for a space (`--set 1:Rack_Mode=5`), because a plan line splits on spaces; a
// `--remove sN` (a strip slot) is refused by the seam today, so a strip modulator is silenced with `--set sN:Enabled=0`.
// `--routes N` (N core routes over (source, destination) pairs, the way test_clap_render makes 520), `--droute
// STRIP:OUT:POS:Control=AMT` (a strip modulator slot STRIP, output OUT, to the control of lane position POS or `sN`).
// `--sp SLOT:Name=VALUE` (a per-sound Spectral drawer switch of the engine: FilterOn CombOn TiltOn PartialsOn VocodeOn ...).
// `--list 1` alone prints the devices this build will place.
// `--stages 1` binds a `rack::CostTree` to every block (the spike recorder's stage timers, backlog SR) and prints each
// stage's own µs per block as `STAGE,round,label,phase,stage,p50,p99,max,mean,load` lines on stderr — the per-stage A/B the
// idle-overhead pass (2026-11-05) was measured with. `--profile 1` samples round 0's idle phase (`--profile 2`: its held phase) with SIGPROF into
// `<label>.prof` (one backtrace per line, offsets into this executable, for `addr2line -fCe`): this host has no perf.
//
// TWO THINGS THE FIRST DRAFT GOT WRONG, KEPT HERE SO THE NEXT ONE DOES NOT: (1) a device added through the seam is
// BUILT only by the editor's tick, a state load or an activate, so with no editor it never ran and every `--add` cost
// nothing — every build now ends in a reactivate (a preset's own devices were always built, by the state load);
// (2) on this host thread CPU is NOT load-proof: it moved 2x between load 27 and load 11 (a 15 W laptop part, the
// clock shared across cores and the SMT sibling busy). Compare configurations INSIDE one plan, where they interleave;
// a ratio between two of them held to a few points across that 2x, an absolute number did not. The `cal_us` column
// (a fixed kernel timed beside every run) was meant to normalise that and does not track it: recorded, not used.
//
// RUN IT FROM THE REPO ROOT with HOME pointed at a scratch directory (the plugin's preset store reads HOME), pinned:
//     HOME=$scratch taskset -c 15 build/tests/bench_patch --plan tests/bench_patch_plans/presets.plan --rounds 10

#include "test_heap.h"
#include "clap/clap.h"

#include "plugin/feathers_test_chain.h"
#include "plugin/params.h"
#include "plugin/preset_store.h"
#include "rack/device_cost.h"
#include "portable_aligned_alloc.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dlfcn.h>
#include <execinfo.h>
#include <signal.h>
#include <sys/time.h>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <pthread.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

extern "C" const clap_plugin_entry clap_entry;

namespace {

thread_local bool tCounting = false; ///< true only on the process() thread, inside process()
std::atomic<long> gAllocations { 0 };
std::atomic<long> gLocks { 0 };

using MutexFn = int (*)(pthread_mutex_t*);
MutexFn realLock = nullptr;
MutexFn realTryLock = nullptr;

void resolveRealMutex() {
	realLock = reinterpret_cast<MutexFn>(dlsym(RTLD_NEXT, "pthread_mutex_lock"));
	realTryLock = reinterpret_cast<MutexFn>(dlsym(RTLD_NEXT, "pthread_mutex_trylock"));
}

} // namespace

extern "C" int pthread_mutex_lock(pthread_mutex_t* m) {
	if (tCounting) {
		gLocks.fetch_add(1, std::memory_order_relaxed);
	}
	if (realLock == nullptr) {
		resolveRealMutex();
	}
	return realLock(m);
}

extern "C" int pthread_mutex_trylock(pthread_mutex_t* m) {
	if (tCounting) {
		gLocks.fetch_add(1, std::memory_order_relaxed);
	}
	if (realTryLock == nullptr) {
		resolveRealMutex();
	}
	return realTryLock(m);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(size_t size) {
	if (tCounting) {
		gAllocations.fetch_add(1, std::memory_order_relaxed);
	}
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
	if (tCounting) {
		gAllocations.fetch_add(1, std::memory_order_relaxed);
	}
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

constexpr double kSampleRate = 48000.0;
constexpr uint32_t kMaxFrames = 1024;

const void* CLAP_ABI hostGetExtension(const clap_host*, const char*) {
	return nullptr;
}
/// A HOST HONOURS request_restart: adding a lane device asks for one, and until the host reactivates the engine does
/// not run it (measured: a Featherverb or Resonate added through the seam cost nothing until a restart). Read by build().
std::atomic<int> gRestartsAsked { 0 };
void CLAP_ABI hostRequestRestart(const clap_host*) {
	gRestartsAsked.fetch_add(1, std::memory_order_relaxed);
}
void CLAP_ABI hostRequestProcess(const clap_host*) {}
/// AND request_callback: a host calls on_main_thread soon after. The bench does it between blocks, outside the timing,
/// so a device the editor road adds is provisioned and handed to the engine the way a real host lets it be.
std::atomic<int> gCallbacksAsked { 0 };
void CLAP_ABI hostRequestCallback(const clap_host*) {
	gCallbacksAsked.fetch_add(1, std::memory_order_relaxed);
}

const clap_host kHost = {
	CLAP_VERSION_INIT, nullptr, "feathers bench", "Owlet Records", "", "1.0", hostGetExtension, hostRequestRestart,
	hostRequestProcess, hostRequestCallback,
};

bool CLAP_ABI outputTryPush(const clap_output_events*, const clap_event_header*) {
	return true;
}

/// The input events of one block: notes and param values, in the order pushed (callers push in time order).
class EventList {
public:
	EventList() {
		notes_.reserve(64);
		params_.reserve(64);
		order_.reserve(128);
	}
	void pushNote(uint16_t type, uint32_t time, int16_t key, int32_t noteId, double velocity) {
		clap_event_note e {};
		e.header.size = sizeof(e);
		e.header.time = time;
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = type;
		e.note_id = noteId;
		e.port_index = 0;
		e.channel = 0;
		e.key = key;
		e.velocity = velocity;
		notes_.push_back(e);
		order_.push_back({ true, notes_.size() - 1 });
	}
	void pushParam(uint32_t time, clap_id id, double value) {
		clap_event_param_value e {};
		e.header.size = sizeof(e);
		e.header.time = time;
		e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
		e.header.type = CLAP_EVENT_PARAM_VALUE;
		e.param_id = id;
		e.note_id = -1;
		e.port_index = -1;
		e.channel = -1;
		e.key = -1;
		e.value = value;
		params_.push_back(e);
		order_.push_back({ false, params_.size() - 1 });
	}
	void clear() {
		notes_.clear();
		params_.clear();
		order_.clear();
	}
	clap_input_events input() const { return clap_input_events { const_cast<EventList*>(this), sizeFn, getFn }; }

private:
	struct Ref {
		bool note;
		size_t index;
	};
	static uint32_t CLAP_ABI sizeFn(const clap_input_events* list) {
		return static_cast<uint32_t>(static_cast<const EventList*>(list->ctx)->order_.size());
	}
	static const clap_event_header* CLAP_ABI getFn(const clap_input_events* list, uint32_t index) {
		const auto* self = static_cast<const EventList*>(list->ctx);
		const Ref& r = self->order_[index];
		return r.note ? &self->notes_[r.index].header : &self->params_[r.index].header;
	}
	std::vector<clap_event_note> notes_;
	std::vector<clap_event_param_value> params_;
	std::vector<Ref> order_;
};

struct StateBytes {
	std::vector<uint8_t> bytes;
	size_t read = 0;
	static int64_t CLAP_ABI readFn(const clap_istream* stream, void* buffer, uint64_t size) {
		auto* self = static_cast<StateBytes*>(stream->ctx);
		const size_t left = self->bytes.size() - self->read;
		const size_t take = size < left ? static_cast<size_t>(size) : left;
		std::memcpy(buffer, self->bytes.data() + self->read, take);
		self->read += take;
		return static_cast<int64_t>(take);
	}
	clap_istream in() { return clap_istream { this, readFn }; }
};

double threadSeconds() {
	timespec t {};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return static_cast<double>(t.tv_sec) + 1e-9 * static_cast<double>(t.tv_nsec);
}
double processSeconds() {
	timespec t {};
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
	return static_cast<double>(t.tv_sec) + 1e-9 * static_cast<double>(t.tv_nsec);
}
long involuntarySwitches() {
#if defined(RUSAGE_THREAD)
	rusage u {};
	getrusage(RUSAGE_THREAD, &u);
	return u.ru_nivcsw;
#else
	// RUSAGE_THREAD is Linux-only (macOS has no per-thread rusage through getrusage). Report 0 rather than the
	// whole process's count, which would mix other threads' preemptions into a per-thread phase figure.
	return 0;
#endif
}
std::string loadavg() {
	std::ifstream f("/proc/loadavg");
	std::string a;
	f >> a;
	return a;
}

struct Config {
	std::string label;
	std::string preset;
	int engine = -1; ///< an OscType value, or -1 to leave the patch's
	int voiceCount = 0;
	std::vector<std::pair<clap_id, double>> params;
	std::vector<std::string> add;
	std::vector<std::string> mods;
	std::vector<std::string> sets;
	std::vector<std::string> routes;
	std::vector<std::string> deviceRoutes; ///< `--droute STRIP:OUT:POS:Control=AMT`
	int manyRoutes = 0; ///< `--routes N`: N core routes, walked over (source, destination) pairs
	std::vector<int> removes;
	int notes = 4;
	int note = 48;
	int step = 7;
	uint32_t frames = 256;
	double idle = 1.0;
	double held = 4.0;
	double tail = 3.0;
	long automate = -1;
	int sidechain = 0;
	int reactivate = 1; ///< 0: skip the reactivate that ends every build (the A/B that showed it matters)
	std::string save; ///< non-empty: round 0 saves the built patch as the `test` preset of this name (`_` for a space)
	/// THE SAVE'S CATEGORY AND TAGS (`--category Plucks --tags factory,pluck`): `Test` and `test` unless a line says otherwise
	/// (the Karplus-Strong starter presets, 2026-10-31, are sounds rather than stress patches).
	std::string category = "Test";
	std::vector<std::string> tags { "test" };
	std::string wav; ///< non-empty: round 0's idle, held and tail output as a 32-bit float stereo WAV at this path (A/B renders, backlog PS)
	/// `--body POS:FILE`: after the build, FILE's bytes loaded as the BODY of the lane device at POS (`edit_device_body`, the device's own
	/// editor road) — a Delay's feedback lane, a chain's contents: what the seam cannot build by tag (backlog PS).
	std::vector<std::string> bodies;
	int dump = 0; ///< 1: round 0's held-phase per-block thread CPU (us) to `<label>.blocks` in the working directory
	/// 1: bind a `rack::CostTree` to every block and print each STAGE's per-block p50/p99/max (the spike recorder's stages,
	/// backlog SR) per phase as `STAGE,...` lines on stderr. Off by default: the tree costs a counter read per scope.
	int stages = 0;
	int profile = 0; ///< 1: round 0's idle phase, 2: its held phase, sampled by SIGPROF into `<label>.prof` (`profStart`)
};

clap_id parseId(const std::string& s) {
	return static_cast<clap_id>(std::strtoul(s.c_str(), nullptr, 0));
}

bool parseConfig(const std::vector<std::string>& tok, Config* c) {
	if (tok.empty()) {
		return false;
	}
	c->label = tok[0];
	for (size_t i = 1; i + 1 < tok.size(); i += 2) {
		const std::string& k = tok[i];
		const std::string& v = tok[i + 1];
		if (k == "--preset") {
			c->preset = v;
		}
		else if (k == "--engine") {
			c->engine = v == "additive"   ? static_cast<int>(feathers::OscType::Additive)
			            : v == "resonate" ? static_cast<int>(feathers::OscType::Resonate)
			            : v == "pluck"    ? static_cast<int>(feathers::OscType::Pluck)
			            : v == "drum"     ? static_cast<int>(feathers::OscType::Drum)
			                              : static_cast<int>(feathers::OscType::V5);
		}
		else if (k == "--voice-count") {
			c->voiceCount = std::atoi(v.c_str());
		}
		else if (k == "--param") {
			const size_t eq = v.find('=');
			c->params.emplace_back(parseId(v.substr(0, eq)), std::atof(v.c_str() + eq + 1));
		}
		else if (k == "--add") {
			c->add.push_back(v);
		}
		else if (k == "--mod") {
			c->mods.push_back(v);
		}
		else if (k == "--set") {
			c->sets.push_back(v);
		}
		else if (k == "--route") {
			c->routes.push_back(v);
		}
		else if (k == "--sp") { // a per-sound Spectral drawer control: SLOT:Name=VALUE (the id is spectralParam's, not a hand-computed hex)
			static const std::pair<const char*, feathers::SpectralControl> kNames[] = {
				{ "FilterOn", feathers::SpectralControl::FilterOn }, { "CombOn", feathers::SpectralControl::CombOn },
				{ "FormantShiftOn", feathers::SpectralControl::FormantShiftOn }, { "TiltOn", feathers::SpectralControl::TiltOn },
				{ "OddEvenOn", feathers::SpectralControl::OddEvenOn }, { "FormantOn", feathers::SpectralControl::FormantOn },
				{ "PartialsOn", feathers::SpectralControl::PartialsOn }, { "DampOn", feathers::SpectralControl::DampOn },
				{ "StretchOn", feathers::SpectralControl::StretchOn }, { "ShiftOn", feathers::SpectralControl::ShiftOn },
				{ "Filter2On", feathers::SpectralControl::Filter2On }, { "VocodeOn", feathers::SpectralControl::VocodeOn },
			};
			const size_t colon = v.find(':');
			const size_t eq = v.rfind('=');
			bool found = false;
			if (colon != std::string::npos && eq != std::string::npos) {
				const std::string name = v.substr(colon + 1, eq - colon - 1);
				for (const auto& [n, control] : kNames) {
					if (name == n) {
						c->params.emplace_back(static_cast<clap_id>(feathers::spectralParam(static_cast<size_t>(std::atoi(v.c_str())), control)),
						                       std::atof(v.c_str() + eq + 1));
						found = true;
					}
				}
			}
			if (!found) {
				std::fprintf(stderr, "bench_patch: %s: bad --sp %s\n", c->label.c_str(), v.c_str());
				return false;
			}
		}
		else if (k == "--droute") {
			c->deviceRoutes.push_back(v);
		}
		else if (k == "--routes") {
			c->manyRoutes = std::atoi(v.c_str());
		}
		else if (k == "--remove") {
			// `sN` names strip slot N (the seam's strip bit); strip slots sort above every lane position, so they go first
			c->removes.push_back(v[0] == 's' ? static_cast<int>(CLAP_FEATHERS_TEST_STRIP) | std::atoi(v.c_str() + 1) : std::atoi(v.c_str()));
		}
		else if (k == "--notes") {
			c->notes = std::atoi(v.c_str());
		}
		else if (k == "--note") {
			c->note = std::atoi(v.c_str());
		}
		else if (k == "--step") {
			c->step = std::atoi(v.c_str());
		}
		else if (k == "--frames") {
			c->frames = static_cast<uint32_t>(std::atoi(v.c_str()));
		}
		else if (k == "--idle") {
			c->idle = std::atof(v.c_str());
		}
		else if (k == "--held") {
			c->held = std::atof(v.c_str());
		}
		else if (k == "--tail") {
			c->tail = std::atof(v.c_str());
		}
		else if (k == "--automate") {
			c->automate = static_cast<long>(parseId(v));
		}
		else if (k == "--category") {
			c->category = v;
			std::replace(c->category.begin(), c->category.end(), '_', ' ');
		}
		else if (k == "--tags") {
			c->tags.clear();
			for (size_t at = 0; at <= v.size();) {
				const size_t comma = std::min(v.find(',', at), v.size());
				if (comma > at) {
					c->tags.push_back(v.substr(at, comma - at));
				}
				at = comma + 1;
			}
		}
		else if (k == "--save") {
			c->save = v;
			std::replace(c->save.begin(), c->save.end(), '_', ' ');
		}
		else if (k == "--reactivate") {
			c->reactivate = std::atoi(v.c_str());
		}
		else if (k == "--body") {
			c->bodies.push_back(v);
		}
		else if (k == "--wav") {
			c->wav = v;
		}
		else if (k == "--dump") {
			c->dump = std::atoi(v.c_str());
		}
		else if (k == "--profile") {
			c->profile = std::atoi(v.c_str());
		}
		else if (k == "--stages") {
			c->stages = std::atoi(v.c_str());
		}
		else if (k == "--sidechain") {
			c->sidechain = std::atoi(v.c_str());
		}
		else {
			std::fprintf(stderr, "bench_patch: %s: unknown option %s\n", c->label.c_str(), k.c_str());
			return false;
		}
	}
	if (c->frames == 0 || c->frames > kMaxFrames) {
		std::fprintf(stderr, "bench_patch: %s: --frames must be 1..%u\n", c->label.c_str(), kMaxFrames);
		return false;
	}
	return true;
}

/// `--profile 1`: a SIGPROF sampler over the idle phase. Each sample is a backtrace (up to 32 frames) into a fixed buffer;
/// at the end the frames go to `<label>.prof` as offsets into the executable, one sample per line, for `addr2line -fCe`.
constexpr size_t kProfDepth = 32;
constexpr size_t kProfSamples = 200000;
std::vector<void*> gProf;          // kProfSamples * kProfDepth, sized before the timer starts
std::atomic<size_t> gProfCount { 0 };
std::array<uint8_t, kProfSamples> gProfDepths {};
volatile sig_atomic_t gProfOn = 0;
void profHandler(int) {
	if (gProfOn == 0) {
		return;
	}
	const size_t i = gProfCount.load(std::memory_order_relaxed);
	if (i >= kProfSamples) {
		return;
	}
	const int n = backtrace(&gProf[i * kProfDepth], static_cast<int>(kProfDepth));
	gProfDepths[i] = static_cast<uint8_t>(n);
	gProfCount.store(i + 1, std::memory_order_relaxed);
}
void profStart() {
	if (gProf.empty()) {
		gProf.resize(kProfSamples * kProfDepth);
		void* warm[4];
		(void)backtrace(warm, 4); // loads libgcc before the first signal
		struct sigaction sa {};
		sa.sa_handler = profHandler;
		sa.sa_flags = SA_RESTART;
		sigaction(SIGPROF, &sa, nullptr);
	}
	itimerval t {};
	t.it_interval.tv_usec = 100;
	t.it_value.tv_usec = 100;
	setitimer(ITIMER_PROF, &t, nullptr);
	gProfOn = 1;
}
void profStop(const std::string& label) {
	gProfOn = 0;
	itimerval t {};
	setitimer(ITIMER_PROF, &t, nullptr);
	Dl_info self {};
	dladdr(reinterpret_cast<void*>(&profHandler), &self);
	const auto base = reinterpret_cast<uintptr_t>(self.dli_fbase);
	std::ofstream out(label + ".prof");
	const size_t n = gProfCount.exchange(0);
	for (size_t i = 0; i < n; ++i) {
		// Frame 0-1 are the handler and the signal trampoline.
		for (size_t d = 2; d < gProfDepths[i]; ++d) {
			const auto pc = reinterpret_cast<uintptr_t>(gProf[i * kProfDepth + d]);
			char hex[24];
			std::snprintf(hex, sizeof(hex), "%lx ", static_cast<unsigned long>(pc - base - (d == 2 ? 0 : 1)));
			out << hex;
		}
		out << '\n';
	}
}

/// `--stages 1`: microseconds per cost-counter tick (TSC), measured once against steady_clock at start-up.
double gUsPerTick = 0.0;
void calibrateTicks() {
	const auto w0 = std::chrono::steady_clock::now();
	const uint64_t t0 = feathers::rack::costTicks();
	while (std::chrono::steady_clock::now() - w0 < std::chrono::milliseconds(50)) {
	}
	const uint64_t t1 = feathers::rack::costTicks();
	const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - w0).count();
	gUsPerTick = us / static_cast<double>(t1 - t0);
}

/// One phase's per-block samples and its counters.
struct Phase {
	std::vector<double> cpuUs;
	std::vector<double> wallUs;
	long allocations = 0;
	long locks = 0;
	long preempted = 0;
	double otherThreadsUs = 0.0; ///< process CPU less this thread's, over the phase
	int firstSleepBlock = -1;    ///< the first block that returned SLEEP, or -1
	double checksum = 0.0;
	uint64_t hash = 1469598103934665603ull; ///< FNV-1a over the output's BITS, L then R per block: the bit-identity of a change (backlog PS)
	std::vector<float>* wav = nullptr;       ///< non-null: the output, interleaved, appended (--wav)
	std::array<std::vector<double>, feathers::rack::kStageCount> stageUs; ///< `--stages 1`: each stage's own µs per block
	std::vector<double> devicesUs;                                           ///< `--stages 1`: the depth-0 devices' µs per block
};

double percentile(std::vector<double> v, double p) {
	if (v.empty()) {
		return 0.0;
	}
	std::sort(v.begin(), v.end());
	const double at = p * static_cast<double>(v.size() - 1);
	const size_t lo = static_cast<size_t>(at);
	const size_t hi = std::min(lo + 1, v.size() - 1);
	return v[lo] + (v[hi] - v[lo]) * (at - static_cast<double>(lo));
}
double mean(const std::vector<double>& v) {
	double s = 0.0;
	for (double x : v) {
		s += x;
	}
	return v.empty() ? 0.0 : s / static_cast<double>(v.size());
}

class Bench {
public:
	explicit Bench(const Config& c) : c_(c) {
		const auto* factory = static_cast<const clap_plugin_factory*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
		const clap_plugin_descriptor* d = factory->get_plugin_descriptor(factory, 0);
		plugin_ = factory->create_plugin(factory, &kHost, d->id);
		plugin_->init(plugin_);
		plugin_->activate(plugin_, kSampleRate, 1, kMaxFrames);
		plugin_->start_processing(plugin_);
		left_.assign(kMaxFrames, 0.0f);
		right_.assign(kMaxFrames, 0.0f);
		channels_[0] = left_.data();
		channels_[1] = right_.data();
		scL_.assign(kMaxFrames, 0.0f);
		scR_.assign(kMaxFrames, 0.0f);
		scChannels_[0] = scL_.data();
		scChannels_[1] = scR_.data();
		chain_ = static_cast<const clap_plugin_feathers_test_chain*>(
		    plugin_->get_extension(plugin_, CLAP_EXT_FEATHERS_TEST_CHAIN));
		params_ = static_cast<const clap_plugin_params*>(plugin_->get_extension(plugin_, CLAP_EXT_PARAMS));
	}
	~Bench() {
		plugin_->stop_processing(plugin_);
		plugin_->deactivate(plugin_);
		plugin_->destroy(plugin_);
	}
	Bench(const Bench&) = delete;
	Bench& operator=(const Bench&) = delete;

	const clap_plugin_feathers_test_chain* chain() const { return chain_; }

	/// Builds the configuration's patch. False (and says why) when any piece of it was refused.
	bool build() {
		if (!c_.preset.empty() && !loadPreset()) {
			return false;
		}
		for (const std::string& tag : c_.add) {
			if (chain_->add_device(plugin_, tag.c_str()) == UINT32_MAX) {
				return refuse("--add " + tag);
			}
		}
		for (const std::string& tag : c_.mods) {
			if (chain_->add_mod_device(plugin_, tag.c_str()) == UINT32_MAX) {
				return refuse("--mod " + tag);
			}
		}
		for (const std::string& s : c_.sets) {
			const size_t colon = s.find(':');
			const size_t eq = s.rfind('=');
			if (colon == std::string::npos || eq == std::string::npos) {
				return refuse("--set " + s);
			}
			const std::string pos = s.substr(0, colon);
			const uint32_t at = pos[0] == 's' ? (CLAP_FEATHERS_TEST_STRIP | static_cast<uint32_t>(std::atoi(pos.c_str() + 1)))
			                                  : static_cast<uint32_t>(std::atoi(pos.c_str()));
			std::string control = s.substr(colon + 1, eq - colon - 1);
			std::replace(control.begin(), control.end(), '_', ' '); // a plan line splits on spaces: `Rack_Mode`

			if (!chain_->set_control(plugin_, at, control.c_str(), std::atof(s.c_str() + eq + 1))) {
				return refuse("--set " + s);
			}
		}
		for (const std::string& r : c_.routes) {
			unsigned src = 0;
			unsigned dst = 0;
			double amt = 0.0;
			if (std::sscanf(r.c_str(), "%u:%u:%lf", &src, &dst, &amt) != 3 || !chain_->set_core_route(plugin_, src, dst, amt)) {
				return refuse("--route " + r);
			}
		}
		for (const std::string& r : c_.deviceRoutes) {
			unsigned strip = 0;
			unsigned out = 0;
			char pos[16] = {};
			char control[96] = {};
			double amt = 0.0;
			if (std::sscanf(r.c_str(), "%u:%u:%15[^:]:%95[^=]=%lf", &strip, &out, pos, control, &amt) != 5
			    || chain_->set_device_route == nullptr) {
				return refuse("--droute " + r);
			}
			const uint32_t at = pos[0] == 's' ? (CLAP_FEATHERS_TEST_STRIP | static_cast<uint32_t>(std::atoi(pos + 1)))
			                                  : static_cast<uint32_t>(std::atoi(pos));
			std::string name = control;
			std::replace(name.begin(), name.end(), '_', ' ');
			if (!chain_->set_device_route(plugin_, strip, out, at, name.c_str(), amt)) {
				return refuse("--droute " + r);
			}
		}
		int madeRoutes = 0;
		for (uint32_t s = 0; s < feathers::kCoreSourceCount && madeRoutes < c_.manyRoutes; ++s) {
			for (uint32_t d = 0; d < feathers::kParamDestinationCount && madeRoutes < c_.manyRoutes; ++d) {
				if (chain_->set_core_route(plugin_, s, d, 0.001 * (1 + madeRoutes))) {
					++madeRoutes;
				}
			}
		}
		if (madeRoutes != c_.manyRoutes) {
			return refuse("--routes " + std::to_string(c_.manyRoutes) + " (only " + std::to_string(madeRoutes) + " were accepted)");
		}
		std::vector<int> removes = c_.removes;
		std::sort(removes.rbegin(), removes.rend()); // highest first, so the earlier positions do not move
		for (int pos : removes) {
			if (!chain_->remove_device(plugin_, static_cast<uint32_t>(pos))) {
				return refuse("--remove " + std::to_string(pos));
			}
		}
		plugin_->on_main_thread(plugin_);
		// A DEVICE ADDED THROUGH THE SEAM IS BUILT ONLY BY THE EDITOR'S TICK (`provisionAndDrain`), a state load or an
		// activate; with no editor open nothing builds it and the engine runs without it (MEASURED: a Featherverb or a
		// Resonate added this way cost nothing). So every build ends in a reactivate, as a host reloading a project does.
		restarted_ = gRestartsAsked.exchange(0) != 0;
		if (c_.reactivate != 0) {
			plugin_->stop_processing(plugin_);
			plugin_->deactivate(plugin_);
			plugin_->activate(plugin_, kSampleRate, 1, kMaxFrames);
			plugin_->start_processing(plugin_);
		}
		return true;
	}

	bool restarted_ = false; ///< the build asked for a restart, and was given one

	/// **SAVE THE BUILT PATCH AS A PRESET, THROUGH THE BROWSER'S OWN ROAD** — `capture_preset` (what the browser's Save
	/// writes) into `PresetStore::save`, in the plugin bank's `Test` category, tagged `test`. It lands under the preset
	/// ROOT (`$FEATHERS_PRESET_ROOT`, which the caller points at a scratch directory); copying it into
	/// presets/factory/Feathers/Test/ is what ships it. Called after the warm-up, so the drains have run.
	bool save(const std::string& name) const {
		std::vector<uint8_t> payload;
		clap_ostream out { &payload, [](const clap_ostream* s, const void* buffer, uint64_t size) -> int64_t {
			                  auto* bytes = static_cast<std::vector<uint8_t>*>(s->ctx);
			                  const auto* from = static_cast<const uint8_t*>(buffer);
			                  bytes->insert(bytes->end(), from, from + size);
			                  return static_cast<int64_t>(size);
		                  } };
		feathers::PresetMeta meta;
		meta.tags = c_.tags;
		meta.saved = static_cast<int64_t>(std::time(nullptr));
		if (!chain_->capture_preset(plugin_, &out) || payload.empty()
		    || !feathers::PresetStore::save(name, payload, feathers::kPluginBank, c_.category, meta)) {
			return refuse("--save " + name + " (is FEATHERS_PRESET_ROOT set?)");
		}
		std::fprintf(stderr, "bench_patch: %s: saved %s\n", c_.label.c_str(),
		             feathers::PresetStore::pathFor(name, feathers::kPluginBank, c_.category).string().c_str());
		return true;
	}

	/// The --body loads, after the build (its reactivate would otherwise rebuild the device from the stored state).
	bool loadBodies() const {
		for (const std::string& b : c_.bodies) {
			const size_t colon = b.find(':');
			std::ifstream f(b.substr(colon + 1), std::ios::binary);
			const std::vector<char> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (colon == std::string::npos || bytes.empty() || chain_->edit_device_body == nullptr
			    || !chain_->edit_device_body(plugin_, static_cast<uint32_t>(std::atoi(b.c_str())), bytes.data(), bytes.size())) {
				return refuse("--body " + b);
			}
		}
		return true;
	}

	uint32_t deviceCount() const { return chain_->device_count(plugin_); }

	/// What the patch IS once built, read back from the plugin rather than assumed from the args.
	void describe() const {
		double osc = -1.0;
		double voices = -1.0;
		double stack = -1.0;
		params_->get_value(plugin_, static_cast<clap_id>(feathers::ParamId::OscType), &osc);
		params_->get_value(plugin_, static_cast<clap_id>(feathers::ParamId::VoiceCount), &voices);
		params_->get_value(plugin_, static_cast<clap_id>(feathers::ParamId::StackCount), &stack);
		const int type = static_cast<int>(osc);
		std::fprintf(stderr, "bench_patch: %s: engine %s, voice count %g, stack %g, %u lane devices%s\n", c_.label.c_str(),
		             type >= 0 && type < static_cast<int>(feathers::kOscTypeNames.size()) ? feathers::kOscTypeNames[static_cast<size_t>(type)] : "?",
		             voices, stack, deviceCount(), restarted_ ? ", the plugin asked for a restart" : "");
		std::fprintf(stderr, "bench_patch: %s: core routes adopted by the engine: %u\n", c_.label.c_str(),
		             chain_->engine_route_count(plugin_));
		// A ROUTE THAT DOES NOTHING COSTS NOTHING: say where a device route's destination stands (resolved against base).
		for (const std::string& r : c_.deviceRoutes) {
			unsigned strip = 0;
			unsigned out = 0;
			char pos[16] = {};
			char control[96] = {};
			double amt = 0.0;
			if (std::sscanf(r.c_str(), "%u:%u:%15[^:]:%95[^=]=%lf", &strip, &out, pos, control, &amt) == 5 && pos[0] != 's') {
				std::string name = control;
				std::replace(name.begin(), name.end(), '_', ' ');
				double base = 0.0;
				double resolved = 0.0;
				if (chain_->engine_control(plugin_, static_cast<uint32_t>(std::atoi(pos)), name.c_str(), &base, &resolved)) {
					std::fprintf(stderr, "bench_patch: %s: route %s: base %g, resolved %g\n", c_.label.c_str(), r.c_str(), base, resolved);
				}
			}
		}
	}

	/// Runs `blocks` blocks into `phase`; `first` events go into the first block only.
	void run(int blocks, const EventList& first, Phase* phase) {
		phase->cpuUs.reserve(static_cast<size_t>(blocks));
		phase->wallUs.reserve(static_cast<size_t>(blocks));
		const long preemptedBefore = involuntarySwitches();
		const double processBefore = processSeconds();
		const double threadBefore = threadSeconds();
		for (int b = 0; b < blocks; ++b) {
			events_.clear();
			const EventList& in = b == 0 ? first : events_;
			if (b != 0 && c_.automate >= 0) {
				events_.pushParam(0, static_cast<clap_id>(c_.automate), automated());
			}
			if (c_.sidechain != 0) {
				for (uint32_t i = 0; i < c_.frames; ++i) {
					scL_[i] = scR_[i] = noise_(rng_);
				}
			}
			if (gCallbacksAsked.exchange(0) != 0) {
				plugin_->on_main_thread(plugin_);
			}
			const long allocBefore = gAllocations.load(std::memory_order_relaxed);
			const long locksBefore = gLocks.load(std::memory_order_relaxed);
			const auto wall0 = std::chrono::steady_clock::now();
			const double cpu0 = threadSeconds();
			if (c_.stages != 0) {
				tree_.clear();
				(void)feathers::rack::bindCostTree(&tree_);
			}
			tCounting = true;
			const clap_process_status status = process(in);
			tCounting = false;
			const double cpu1 = threadSeconds();
			const auto wall1 = std::chrono::steady_clock::now();
			if (c_.stages != 0) {
				(void)feathers::rack::bindCostTree(nullptr);
				for (size_t s = 0; s < feathers::rack::kStageCount; ++s) {
					// A stage's own ticks less its nested devices': unsigned on purpose in the tree, signed here.
					phase->stageUs[s].push_back(static_cast<double>(static_cast<int64_t>(tree_.stageTicks(s))) * gUsPerTick);
				}
				uint64_t devices = 0;
				for (size_t i = 0; i < tree_.count(); ++i) {
					if (tree_.node(i).depth == 0) {
						devices += tree_.node(i).ticks;
					}
				}
				phase->devicesUs.push_back(static_cast<double>(devices) * gUsPerTick);
			}
			phase->allocations += gAllocations.load(std::memory_order_relaxed) - allocBefore;
			phase->locks += gLocks.load(std::memory_order_relaxed) - locksBefore;
			phase->cpuUs.push_back((cpu1 - cpu0) * 1e6);
			phase->wallUs.push_back(std::chrono::duration<double, std::micro>(wall1 - wall0).count());
			if (status == CLAP_PROCESS_SLEEP && phase->firstSleepBlock < 0) {
				phase->firstSleepBlock = b;
			}
			for (uint32_t i = 0; i < c_.frames; ++i) {
				phase->checksum += std::fabs(static_cast<double>(left_[i])) + std::fabs(static_cast<double>(right_[i]));
			}
			for (const std::vector<float>* ch : { &left_, &right_ }) {
				const auto* bytes = reinterpret_cast<const uint8_t*>(ch->data());
				for (size_t i = 0; i < sizeof(float) * c_.frames; ++i) {
					phase->hash = (phase->hash ^ bytes[i]) * 1099511628211ull;
				}
			}
			if (phase->wav != nullptr) {
				for (uint32_t i = 0; i < c_.frames; ++i) {
					phase->wav->push_back(left_[i]);
					phase->wav->push_back(right_[i]);
				}
			}
		}
		const double threadAfter = threadSeconds();
		const double processAfter = processSeconds();
		phase->otherThreadsUs = ((processAfter - processBefore) - (threadAfter - threadBefore)) * 1e6;
		phase->preempted = involuntarySwitches() - preemptedBefore;
	}

	void prime(EventList* first) const {
		if (c_.engine >= 0) {
			first->pushParam(0, static_cast<clap_id>(feathers::ParamId::OscType), c_.engine);
		}
		if (c_.voiceCount > 0) {
			first->pushParam(0, static_cast<clap_id>(feathers::ParamId::VoiceCount), c_.voiceCount);
		}
		for (const auto& [id, value] : c_.params) {
			first->pushParam(0, id, value);
		}
	}

private:
	bool refuse(const std::string& what) const {
		std::fprintf(stderr, "bench_patch: %s: refused %s\n", c_.label.c_str(), what.c_str());
		return false;
	}

	bool loadPreset() {
		std::ifstream f(c_.preset, std::ios::binary);
		const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		feathers::PresetMeta meta;
		StateBytes bytes;
		if (file.empty() || !feathers::PresetStore::decode(file, &meta, &bytes.bytes)) {
			return refuse("--preset " + c_.preset + " (unreadable or not a preset)");
		}
		const auto* state = static_cast<const clap_plugin_state*>(plugin_->get_extension(plugin_, CLAP_EXT_STATE));
		clap_istream in = bytes.in();
		if (state == nullptr || !state->load(plugin_, &in)) {
			return refuse("--preset " + c_.preset + " (the state road refused it)");
		}
		return true;
	}

	double automated() {
		if (!automateRange_) {
			clap_param_info info {};
			automateMin_ = 0.0;
			automateMax_ = 1.0;
			for (uint32_t i = 0; i < params_->count(plugin_); ++i) {
				if (params_->get_info(plugin_, i, &info) && info.id == static_cast<clap_id>(c_.automate)) {
					automateMin_ = info.min_value;
					automateMax_ = info.max_value;
				}
			}
			automateRange_ = true;
		}
		automatePhase_ += 0.01;
		const double u = 0.5 + 0.4 * std::sin(automatePhase_);
		return automateMin_ + u * (automateMax_ - automateMin_);
	}

	clap_process_status process(const EventList& events) {
		clap_audio_buffer output {};
		output.data32 = channels_;
		output.channel_count = 2;
		clap_audio_buffer input {};
		input.data32 = scChannels_;
		input.channel_count = 2;
		const clap_input_events in = events.input();
		const clap_output_events out { nullptr, outputTryPush };
		clap_process p {};
		p.steady_time = steady_;
		p.frames_count = c_.frames;
		p.audio_inputs = c_.sidechain != 0 ? &input : nullptr;
		p.audio_inputs_count = c_.sidechain != 0 ? 1 : 0;
		p.audio_outputs = &output;
		p.audio_outputs_count = 1;
		p.in_events = &in;
		p.out_events = &out;
		steady_ += c_.frames;
		return plugin_->process(plugin_, &p);
	}

	const Config& c_;
	feathers::rack::CostTree tree_; ///< `--stages 1`: bound to the process() thread around each block
	const clap_plugin* plugin_ = nullptr;
	const clap_plugin_feathers_test_chain* chain_ = nullptr;
	const clap_plugin_params* params_ = nullptr;
	std::vector<float> left_, right_, scL_, scR_;
	float* channels_[2] {};
	float* scChannels_[2] {};
	int64_t steady_ = 0;
	EventList events_;
	std::mt19937 rng_ { 7 };
	std::normal_distribution<float> noise_ { 0.0f, 0.25f };
	bool automateRange_ = false;
	double automateMin_ = 0.0;
	double automateMax_ = 1.0;
	double automatePhase_ = 0.0;
};

/// A FIXED PIECE OF DSP-SHAPED WORK (a one-pole lowpass and a tanh over 4096 samples, 64 passes), timed in thread CPU
/// microseconds and the best of three taken. It is the same work every time, so its time says how fast THIS core is
/// running right now (clock, SMT sibling, caches): a run's cost times (the quietest calibration / its own) is the run
/// on a quieter host. DERIVED, not measured: it assumes the patch slows as this kernel does.
double calibrate() {
	static float buffer[4096];
	double best = 1e30;
	for (int attempt = 0; attempt < 3; ++attempt) {
		const double t0 = threadSeconds();
		float state = 0.0f;
		for (int pass = 0; pass < 64; ++pass) {
			for (float& x : buffer) {
				state += 0.01f * ((x + 0.37f) - state);
				x = std::tanh(state * 1.7f) * 0.5f;
			}
		}
		best = std::min(best, (threadSeconds() - t0) * 1e6);
	}
	return best;
}

/// THE A/B RENDER (backlog PS): interleaved stereo as a 32-bit float WAV at the bench's rate, so a sound-changing lever can be
/// be auditioned beside the build it changes.
bool writeWav(const std::string& path, const std::vector<float>& interleaved) {
	std::ofstream out(path, std::ios::binary);
	if (!out) {
		return false;
	}
	const auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
	const auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
	const auto bytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
	const auto rate = static_cast<uint32_t>(kSampleRate);
	out.write("RIFF", 4);
	u32(36 + bytes);
	out.write("WAVEfmt ", 8);
	u32(16);
	u16(3); // IEEE float
	u16(2);
	u32(rate);
	u32(rate * 2 * 4);
	u16(8);
	u16(32);
	out.write("data", 4);
	u32(bytes);
	out.write(reinterpret_cast<const char*>(interleaved.data()), static_cast<std::streamsize>(bytes));
	return static_cast<bool>(out);
}

int blocksFor(double seconds, uint32_t frames) {
	return std::max(1, static_cast<int>(std::lround(seconds * kSampleRate / static_cast<double>(frames))));
}

/// One run's summary of one phase, kept for the over-rounds summary.
struct Row {
	double cpuMean, cpuP50, cpuP99, cpuMax, wallMean, wallP99, wallMax, other;
	long allocations, locks;
};

void printRow(int round, const Config& c, const char* phase, const Phase& p, uint32_t devices, const std::string& load,
              Row* row) {
	const double budgetUs = 1e6 * static_cast<double>(c.frames) / kSampleRate;
	row->cpuMean = mean(p.cpuUs);
	row->cpuP50 = percentile(p.cpuUs, 0.5);
	row->cpuP99 = percentile(p.cpuUs, 0.99);
	row->cpuMax = percentile(p.cpuUs, 1.0);
	row->wallMean = mean(p.wallUs);
	row->wallP99 = percentile(p.wallUs, 0.99);
	row->wallMax = percentile(p.wallUs, 1.0);
	row->other = p.cpuUs.empty() ? 0.0 : p.otherThreadsUs / static_cast<double>(p.cpuUs.size());
	row->allocations = p.allocations;
	row->locks = p.locks;
	std::printf("%d,%s,%s,%u,%u,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.3f,%.2f,%ld,%ld,%ld,%d,%.6g,%s,%016llx\n", round,
	            c.label.c_str(), phase, c.frames, devices, p.cpuUs.size(), row->cpuMean, row->cpuP50, row->cpuP99,
	            row->cpuMax, row->wallMean, row->wallP99, row->wallMax, 100.0 * row->cpuMean / budgetUs,
	            100.0 * row->cpuP99 / budgetUs, row->other, p.allocations, p.locks, p.preempted, p.firstSleepBlock,
	            p.checksum, load.c_str(), static_cast<unsigned long long>(p.hash));
	std::fflush(stdout);
}

bool readPlan(const char* path, std::vector<Config>* plan) {
	std::ifstream f(path);
	if (!f) {
		std::fprintf(stderr, "bench_patch: cannot read plan %s\n", path);
		return false;
	}
	std::string line;
	while (std::getline(f, line)) {
		if (line.empty() || line[0] == '#') {
			continue;
		}
		std::istringstream words(line);
		std::vector<std::string> tok { std::istream_iterator<std::string>(words), std::istream_iterator<std::string>() };
		if (tok.empty()) {
			continue;
		}
		Config c;
		if (!parseConfig(tok, &c)) {
			return false;
		}
		plan->push_back(c);
	}
	return !plan->empty();
}

/// THE COUNTERS PROVE THEY CAN COUNT before any zero they print is believed.
bool selfTest() {
	const long a0 = gAllocations.load();
	const long l0 = gLocks.load();
	tCounting = true;
	int* volatile p = new int(7);
	static std::mutex m;
	m.lock();
	m.unlock();
	tCounting = false;
	delete p;
	const bool ok = gAllocations.load() - a0 == 1 && gLocks.load() - l0 == 1;
	if (!ok) {
		std::fprintf(stderr, "bench_patch: SELF-TEST FAILED: allocations %ld, locks %ld (each should be 1)\n",
		             gAllocations.load() - a0, gLocks.load() - l0);
	}
	return ok;
}

} // namespace

int main(int argc, char** argv) {
	resolveRealMutex();
	calibrateTicks();
	if (!selfTest()) {
		return 3;
	}
	if (!clap_entry.init("feathers-bench")) {
		return 2;
	}
	if (argc == 3 && std::strcmp(argv[1], "--list") == 0) {
		Config c;
		Bench b(c);
		char tag[64];
		for (uint32_t i = 0; b.chain()->available_device(nullptr, i, tag, sizeof(tag)); ++i) {
			std::printf("%s\n", tag);
		}
		clap_entry.deinit();
		return 0;
	}
	const char* planPath = nullptr;
	int rounds = 10;
	for (int i = 1; i + 1 < argc; i += 2) {
		if (std::strcmp(argv[i], "--plan") == 0) {
			planPath = argv[i + 1];
		}
		else if (std::strcmp(argv[i], "--rounds") == 0) {
			rounds = std::atoi(argv[i + 1]);
		}
	}
	std::vector<Config> plan;
	if (planPath == nullptr || !readPlan(planPath, &plan)) {
		std::fprintf(stderr, "usage: bench_patch --plan FILE [--rounds N] | --list 1\n");
		return 2;
	}
	std::printf("round,label,phase,frames,devices,blocks,cpu_mean_us,cpu_p50_us,cpu_p99_us,cpu_max_us,wall_mean_us,"
	            "wall_p99_us,wall_max_us,cpu_mean_pct_rt,cpu_p99_pct_rt,other_threads_us_per_block,allocs,locks,"
	            "preempted,first_sleep_block,checksum,loadavg1,cal_us\n");
	static const char* const kPhases[] = { "idle", "held", "tail" };
	// THE TREES GO LIVE only when a line asks `--stages 1`, so every other plan runs the scopes' one relaxed load as a ship
	// build does.
	if (std::any_of(plan.begin(), plan.end(), [](const Config& c) { return c.stages != 0; })) {
		feathers::rack::gCostTreesLive.fetch_add(1, std::memory_order_relaxed);
	}
	std::map<std::string, std::vector<Row>> summary; // label/phase -> one Row per round
	for (int round = 0; round < rounds; ++round) {
		for (const Config& c : plan) {
			// THE CALIBRATION, either side of the run: the host's clock and its SMT sibling move thread CPU too (MEASURED 2x on
			// this 15 W laptop part between load 13 and 27), so a fixed piece of work timed beside every run is the yardstick.
			const double cal0 = calibrate();
			std::string load = loadavg();
			Bench b(c);
			if (!b.build()) {
				return 4;
			}
			const uint32_t devices = b.deviceCount();
			EventList first;
			b.prime(&first);
			Phase warm;
			if (!b.loadBodies()) {
				return 4;
			}
			b.run(blocksFor(0.25, c.frames), first, &warm); // the drains, the first bake: not any phase's cost
			if (round == 0) {
				b.describe();
				if (!c.save.empty() && !b.save(c.save)) {
					return 5;
				}
			}
			const EventList none;
			std::vector<float> wav;
			Phase idle;
			Phase held;
			Phase tail;
			if (!c.wav.empty() && round == 0) {
				idle.wav = held.wav = tail.wav = &wav;
			}
			if (c.profile == 1 && round == 0) {
				profStart();
			}
			b.run(blocksFor(c.idle, c.frames), none, &idle);
			if (c.profile == 1 && round == 0) {
				profStop(c.label);
			}
			EventList on;
			for (int n = 0; n < c.notes; ++n) {
				on.pushNote(CLAP_EVENT_NOTE_ON, 0, static_cast<int16_t>(c.note + n * c.step), n, 0.8);
			}
			if (c.profile == 2 && round == 0) {
				profStart();
			}
			b.run(blocksFor(c.held, c.frames), on, &held);
			if (c.profile == 2 && round == 0) {
				profStop(c.label);
			}
			if (c.dump != 0 && round == 0) {
				std::ofstream out(c.label + ".blocks");
				for (size_t i = 0; i < held.cpuUs.size(); ++i) {
					out << i << ' ' << held.cpuUs[i] << ' ' << held.wallUs[i] << '\n';
				}
			}
			EventList off;
			for (int n = 0; n < c.notes; ++n) {
				off.pushNote(CLAP_EVENT_NOTE_OFF, 0, static_cast<int16_t>(c.note + n * c.step), n, 0.0);
			}
			b.run(blocksFor(c.tail, c.frames), off, &tail);
			if (!wav.empty() && !writeWav(c.wav, wav)) {
				std::fprintf(stderr, "bench_patch: %s: cannot write --wav %s\n", c.label.c_str(), c.wav.c_str());
				return 6;
			}
			char cal[32];
			std::snprintf(cal, sizeof(cal), ",%.2f", 0.5 * (cal0 + calibrate()));
			load += cal;
			const Phase* phases[] = { &idle, &held, &tail };
			for (int p = 0; p < 3; ++p) {
				Row row {};
				printRow(round, c, kPhases[p], *phases[p], devices, load, &row);
				summary[c.label + "/" + kPhases[p]].push_back(row);
				if (c.stages != 0) {
					// STAGE,round,label,phase,stage,p50,p99,max,mean — the stage's own µs per block (devices taken out).
					const auto line = [&](const char* name, const std::vector<double>& v) {
						std::fprintf(stderr, "STAGE,%d,%s,%s,%s,%.2f,%.2f,%.2f,%.2f,%s\n", round, c.label.c_str(), kPhases[p],
						             name, percentile(v, 0.5), percentile(v, 0.99), percentile(v, 1.0), mean(v),
						             load.c_str());
					};
					for (size_t s = 0; s < feathers::rack::kStageCount; ++s) {
						line(feathers::rack::kStageNames[s], phases[p]->stageUs[s]);
					}
					line("devices", phases[p]->devicesUs);
				}
			}
		}
	}
	std::fprintf(stderr, "label/phase,n,cpu_mean_med,cpu_mean_min,cpu_mean_max,cpu_p99_med,cpu_max_med,cpu_max_max,"
	                     "wall_mean_med,wall_p99_med,wall_max_med,other_med,allocs_max,locks_max\n");
	for (const Config& c : plan) {
		for (const char* phase : kPhases) {
			const std::vector<Row>& rows = summary[c.label + "/" + phase];
			auto col = [&](double Row::*f) {
				std::vector<double> v;
				for (const Row& r : rows) {
					v.push_back(r.*f);
				}
				return v;
			};
			long allocs = 0;
			long locks = 0;
			for (const Row& r : rows) {
				allocs = std::max(allocs, r.allocations);
				locks = std::max(locks, r.locks);
			}
			const auto cm = col(&Row::cpuMean);
			std::fprintf(stderr, "%s/%s,%zu,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%ld,%ld\n", c.label.c_str(),
			             phase, rows.size(), percentile(cm, 0.5), percentile(cm, 0.0), percentile(cm, 1.0),
			             percentile(col(&Row::cpuP99), 0.5), percentile(col(&Row::cpuMax), 0.5),
			             percentile(col(&Row::cpuMax), 1.0), percentile(col(&Row::wallMean), 0.5),
			             percentile(col(&Row::wallP99), 0.5), percentile(col(&Row::wallMax), 0.5),
			             percentile(col(&Row::other), 0.5), allocs, locks);
		}
	}
	clap_entry.deinit();
	return 0;
}
