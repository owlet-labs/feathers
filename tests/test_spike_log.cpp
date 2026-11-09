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

// THE CPU SPIKE RECORDER (backlog SR, src/plugin/spike_log.h): the recorder against a real RackEngine, which times each
// device into the costs the recorder hands it. The plugins' glue is FEATHERS_DIAGNOSTIC-only and thin (time the call,
// read four counters, endBlock); what is asserted here is everything it relies on.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "fx/delay/delay_types.h"
#include "fx/featherverb/featherverb_types.h"
#include "fx/filter/filter_types.h"
#include "fx/shaper/shaper_types.h"
#include "plugin/device_registry.h"
#include "plugin/spike_log.h"
#include "fx/sklitter/sklitter.h"
#include "fx/sklitter/sklitter_types.h"
#include "rack/inner_chain.h"
#include "rack/parallel_rack.h"
#include "rack/rack_bridge.h"
#include "rack/rack_engine.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <new>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

using namespace feathers;

//------------------------------------------------------------------------------------------------//
// THE ALLOCATION COUNTER — this binary's operator new, as test_rack_engine's.                      //
//------------------------------------------------------------------------------------------------//

namespace {
std::atomic<bool> g_count { false };
std::atomic<uint64_t> g_allocs { 0 };
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
[[gnu::noinline]] void* operator new(std::size_t n) {
	if (g_count.load(std::memory_order_relaxed)) {
		g_allocs.fetch_add(1, std::memory_order_relaxed);
	}
	void* p = feathers::test::heapAlloc(n == 0 ? 1 : n);
	if (p == nullptr) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, std::align_val_t al) {
	if (g_count.load(std::memory_order_relaxed)) {
		g_allocs.fetch_add(1, std::memory_order_relaxed);
	}
	void* p = nullptr;
	const std::size_t a = static_cast<std::size_t>(al);
	if (feathers::test::heapPosixMemalign(&p, a < sizeof(void*) ? sizeof(void*) : a, n == 0 ? 1 : n) != 0) {
		throw std::bad_alloc();
	}
	return p;
}
void* operator new[](std::size_t n, std::align_val_t al) { return operator new(n, al); }
[[gnu::noinline]] void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#pragma GCC diagnostic pop

namespace {

//------------------------------------------------------------------------------------------------//
// A DELIBERATELY SLOW DEVICE: it busy-waits `g_slowUs` microseconds in every bus pass.              //
//------------------------------------------------------------------------------------------------//

std::atomic<int> g_slowUs { 0 };
/// How the slow device spends its time: spinning on the CPU, or asleep (off it, as a preempted thread is).
std::atomic<bool> g_slowSleeps { false };
/// Spend `g_slowUs` once, in the first pass that sees it, rather than in every pass (the engine cuts a block into ticks).
std::atomic<bool> g_slowOnce { false };

/// Spins for `us`; returns the wall time it actually took (a loaded host can take the thread off the CPU inside it).
uint64_t busyWait(int us) {
	const uint64_t from = rack::costClockNs();
	const uint64_t until = from + static_cast<uint64_t>(us) * 1000u;
	uint64_t now = from;
	while (now < until) {
		now = rack::costClockNs();
	}
	return now - from;
}
/// The wall time the slow device last spent, in ns, measured by itself around its wait.
std::atomic<uint64_t> g_slowSpentNs { 0 };
/// Spin on the THREAD'S CPU CLOCK instead of the wall clock: the work is then `g_slowUs` of CPU however long a loaded host
/// keeps the thread off the CPU inside it. The CPU time it spent, summed over its passes.
std::atomic<bool> g_slowSpinsCpu { false };
std::atomic<uint64_t> g_slowCpuSpentNs { 0 };

const device::AudioOps kSlowOps {
	.stateBytes = sizeof(int),
	.stateAlign = alignof(int),
	.construct = [](void* s) { new (s) int(0); },
	.destruct = [](void*) {},
	.prepare = [](void*, double, int) {},
	.reset = [](void*) {},
	.setFields = [](void*, const float*, size_t) {},
	.processStereo =
	    [](void*, float*, float*, int) {
		    const int us = g_slowOnce.load() ? g_slowUs.exchange(0) : g_slowUs.load();
		    if (us > 0 && g_slowSleeps.load()) {
			    std::this_thread::sleep_for(std::chrono::microseconds(us));
		    }
		    else if (us > 0 && g_slowSpinsCpu.load()) {
			    const uint64_t from = spike::threadCpuNs();
			    uint64_t now = from;
			    while (now - from < static_cast<uint64_t>(us) * 1000u) {
				    now = spike::threadCpuNs();
			    }
			    g_slowCpuSpentNs += now - from;
		    }
		    else if (us > 0) {
			    g_slowSpentNs = busyWait(us);
		    }
	    },
};

constexpr device::Descriptor kSlow {
	.traits = { .tag = "SLOW", .name = "Slow", .hasCard = false, .hasExpanded = false },
	.params = {},
	.outputs = {},
	.audio = &kSlowOps,
	.category = device::Category::Utility,
};

constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 8, .outputStride = 4 };
constexpr double kRate = 48000.0;
constexpr int kFrames = 256;
constexpr double kBudgetUs = kFrames * 1e6 / kRate; // 5333.3 us

struct Rig {
	rack::RackEngine engine { kAxes, hostCatalogue() };
	spike::Recorder recorder;
	std::vector<float> l = std::vector<float>(kFrames, 0.1f);
	std::vector<float> r = std::vector<float>(kFrames, 0.1f);
	bool recording = true;
	/// `record` false: no recorder at all, as in a ship build (nothing activated, so the engines' scopes see none).
	explicit Rig(double threshold = spike::kDefaultThreshold, bool record = true) : recording(record) {
		seatedDeviceForTest() = &kSlow;
		engine.prepare(kRate, 4096);
		if (record) {
			recorder.activate(kRate, threshold);
		}
	}
	~Rig() { seatedDeviceForTest() = nullptr; }
	/// One block as a plugin's process() runs it, the recorder timing it from entry to exit.
	void block() {
		if (!recording) {
			engine.processStereo(l.data(), r.data(), kFrames, backplane::unknownBlock());
			return;
		}
		const spike::BlockStart start = recorder.beginBlock();
		engine.processStereo(l.data(), r.data(), kFrames, backplane::unknownBlock());
		spike::BlockFacts facts;
		facts.frames = kFrames;
		facts.voices = 3;
		facts.songBeats = 12.5;
		facts.songSeconds = 6.25;
		facts.flags = spike::kFlagNotes;
		recorder.endBlock(start, facts);
	}
};

} // namespace

TEST_CASE("SR: a deliberately slow device produces a spike record naming it by tag and slot, with the right share") {
	Rig rig;
	const size_t filter = rig.engine.addDevice(fx::filter::kTraits.tag);
	const size_t slow = rig.engine.addDevice("SLOW");
	REQUIRE(filter != rack::RackEngine::kNoSlot);
	REQUIRE(slow != rack::RackEngine::kNoSlot);
	rig.block(); // settled
	spike::Record dummy;
	while (rig.recorder.popForTest(&dummy)) {
	}
	g_slowUs = 4000; // 75% of a 5.33 ms budget, by itself
	rig.block();
	g_slowUs = 0;
	spike::Record rec;
	REQUIRE(rig.recorder.popForTest(&rec));
	CHECK(rec.frames == static_cast<uint32_t>(kFrames));
	CHECK(rec.sampleRate == static_cast<float>(kRate));
	// THE SHARE IS THE ELAPSED TIME OVER THE BUDGET, and the elapsed time covers at least the device's wait.
	CHECK(rec.share == doctest::Approx(rec.elapsedUs / kBudgetUs).epsilon(1e-4));
	CHECK(rec.elapsedUs >= 4000.0f);
	CHECK(rec.share >= 0.75f);
	// THE WORST DEVICE, BY TAG AND SLOT, at what it spent.
	REQUIRE(rec.devices >= 1);
	CHECK(std::string(rec.top[0].tag) == "SLOW");
	CHECK(rec.top[0].slot() == slow);
	CHECK(rec.top[0].us >= 4000.0f);
	CHECK(rec.top[0].us <= rec.elapsedUs);
	// AND THE OTHER DEVICE IS THERE TOO, cheaper, second.
	REQUIRE(rec.devices >= 2);
	CHECK(rec.top[1].slot() == filter);
	CHECK(rec.top[1].us < rec.top[0].us);
	// WHAT THE BLOCK CARRIED.
	CHECK(rec.voices == 3);
	CHECK(rec.songBeats == 12.5);
	CHECK(rec.flags == spike::kFlagNotes);
	const std::string line = spike::lineOf(rec, "feathers");
	CHECK(line.find("\"event\":\"spike\"") != std::string::npos);
	CHECK(line.find("\"tag\":\"SLOW\"") != std::string::npos);
	CHECK(line.find("\"flags\":[\"notes\"]") != std::string::npos);
	// ONE SPIKE, ONE RECORD.
	CHECK_FALSE(rig.recorder.popForTest(&rec));
}

TEST_CASE("SR: a normal render at a sane load records nothing") {
	Rig rig;
	REQUIRE(rig.engine.addDevice(fx::filter::kTraits.tag) != rack::RackEngine::kNoSlot);
	REQUIRE(rig.engine.addDevice("SLOW") != rack::RackEngine::kNoSlot);
	g_slowUs = 0;
	for (int b = 0; b < 400; ++b) {
		rig.block();
	}
	// NO RECORD OF REAL WORK. A loaded host (a parallel ctest) can take the thread off the CPU inside a block, and that
	// block is a true spike of WALL time — the very thing v3 detects (batch 71 went red here). So what must hold is
	// that every record that fired is explained by the OS: its own CPU time under the threshold share of the budget, or
	// flagged `preempted`. A device that really spends the time is neither, and stays red.
	int fired = 0;
	int ownWork = 0;
	spike::Record rec;
	while (rig.recorder.popForTest(&rec)) {
		++fired;
		const bool explained = rec.preempted || (std::isfinite(rec.cpuUs) && rec.cpuUs / kBudgetUs < spike::kDefaultThreshold);
		if (!explained) {
			++ownWork;
			MESSAGE("unexplained: wall " << rec.elapsedUs << " us, cpu " << rec.cpuUs << " us, nivcsw " << rec.nivcsw);
		}
	}
	MESSAGE(fired << " block(s) fired, all explained by the host unless listed");
	CHECK(ownWork == 0);
}

TEST_CASE("SR: when the ring overflows the records past its room are dropped and counted, and the block never waits") {
	Rig rig(1e-9); // every block a spike
	REQUIRE(rig.engine.addDevice(fx::filter::kTraits.tag) != rack::RackEngine::kNoSlot);
	constexpr int kBlocks = 300;
	for (int b = 0; b < kBlocks; ++b) {
		rig.block();
	}
	size_t popped = 0;
	spike::Record rec;
	while (rig.recorder.popForTest(&rec)) {
		++popped;
	}
	// A RING OF N HOLDS N - 1 (one slot tells full from empty).
	CHECK(popped == spike::kRingRecords - 1);
	CHECK(rig.recorder.drops() == static_cast<uint64_t>(kBlocks) - (spike::kRingRecords - 1));
	// AND IT TAKES MORE AGAIN ONCE DRAINED.
	rig.block();
	CHECK(rig.recorder.popForTest(&rec));
}

TEST_CASE("SR: nothing allocates on the audio thread — an ordinary block, a spike, and a spike into a full ring") {
	Rig rig(1e-9);
	REQUIRE(rig.engine.addDevice(fx::filter::kTraits.tag) != rack::RackEngine::kNoSlot);
	REQUIRE(rig.engine.addDevice("SLOW") != rack::RackEngine::kNoSlot);
	rig.block(); // the first block's own one-off work
	g_allocs = 0;
	g_count = true;
	for (int b = 0; b < 300; ++b) { // fills the ring and overflows it
		rig.block();
	}
	g_count = false;
	CHECK(g_allocs.load() == 0);
	CHECK(rig.recorder.drops() > 0);
}

TEST_CASE("SR: the drain writes each spike as a line and, a minute on, a summary of the blocks with their worst and p99 share") {
	// EVERY BLOCK RECORDED (a threshold of almost nothing), so the summary's worst and p99 are checked against the shares
	// the blocks actually had, read from their own lines — not against "an ordinary block is far under the threshold",
	// which a loaded host breaks by preempting one (2026-11-04: red in a loop beside 18 busy processes).
	Rig rig(1e-9);
	rig.recorder.setDetailPerMinute(1000); // every block's line: the per-minute limit has its own cases below
	REQUIRE(rig.engine.addDevice("SLOW") != rack::RackEngine::kNoSlot);
	std::vector<std::string> lines;
	rig.recorder.drain("feathers", 600.0, &lines); // starts the minute
	CHECK(lines.empty());
	for (int b = 0; b < 99; ++b) {
		rig.block();
	}
	g_slowUs = 4500;
	rig.block();
	g_slowUs = 0;
	rig.recorder.drain("feathers", 630.0, &lines); // the same minute: every block's line, no summary
	REQUIRE(lines.size() == 100);
	std::vector<double> shares;
	int slowLines = 0;
	for (const std::string& line : lines) {
		CHECK(line.find("\"event\":\"spike\"") != std::string::npos);
		const size_t at = line.find("\"share\":");
		REQUIRE(at != std::string::npos);
		shares.push_back(std::atof(line.c_str() + at + 8));
		slowLines += line.find("\"tag\":\"SLOW\"") != std::string::npos ? 1 : 0;
	}
	CHECK(slowLines >= 1); // the slow block names its device
	std::vector<double> sorted = shares;
	std::sort(sorted.begin(), sorted.end());
	// THE SUMMARY'S OWN RULE, RESTATED: 1% bins, the first whose running count reaches 99 of 100, reported as its upper edge.
	const double expectedP99 = (std::floor(sorted[98] * 100.0) + 1.0) / 100.0;
	const double expectedWorst = sorted.back();
	lines.clear();
	rig.recorder.drain("feathers", 661.0, &lines); // a new minute: the summary
	REQUIRE(lines.size() == 1);
	const std::string& s = lines[0];
	CHECK(s.find("\"event\":\"summary\"") != std::string::npos);
	CHECK(s.find("\"blocks\":100") != std::string::npos);
	// p99 OF 100 BLOCKS IS THE 99TH's BIN; THE WORST IS THE LARGEST SHARE, and at least the 4.5 ms block's.
	const auto field = [&](const char* key) {
		const size_t at = s.find(key);
		REQUIRE(at != std::string::npos);
		return std::atof(s.c_str() + at + std::strlen(key));
	};
	// EXACT, unless the 99th share sits within the lines' print precision of a bin edge, where either neighbour is honest.
	const double edge = std::abs(sorted[98] * 100.0 - std::round(sorted[98] * 100.0));
	MESSAGE("99th share " << sorted[98] << ", expected p99 " << expectedP99 << ", reported " << field("\"p99\":"));
	if (edge > 0.01) {
		CHECK(field("\"p99\":") == doctest::Approx(expectedP99).epsilon(1e-9));
	}
	else {
		CHECK(std::abs(field("\"p99\":") - expectedP99) <= 0.0101);
	}
	CHECK(field("\"worst\":") == doctest::Approx(expectedWorst).epsilon(1e-3));
	CHECK(field("\"worst\":") >= 0.84);

	// AND THE FILE: the day's `.spikes.jsonl` in the directory the variable names.
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("feathers-spike-test-" + std::to_string(::getpid()));
	std::filesystem::remove_all(dir);
	::setenv("FEATHERS_SPIKE_LOG_DIR", dir.string().c_str(), 1);
	REQUIRE(spike::append({ "{\"a\":1}", "{\"b\":2}" }, 661.0));
	const std::filesystem::path file = spike::fileFor(dir, 661.0);
	CHECK(file.filename().string().ends_with(".spikes.jsonl"));
	std::ifstream in(file);
	std::string first, second;
	std::getline(in, first);
	std::getline(in, second);
	CHECK(first == "{\"a\":1}");
	CHECK(second == "{\"b\":2}");
	::setenv("FEATHERS_SPIKE_LOG_DIR", "", 1);
	CHECK_FALSE(spike::append({ "{}" }, 661.0)); // empty: logging off
	std::filesystem::remove_all(dir);
}

//------------------------------------------------------------------------------------------------//
// THE TRIM (one day's file once reached 2 GB): a limit                                           //
// on detailed lines per minute, the minute's worst always in detail, and a daily size cap. Records  //
// are pushed by hand with chosen shares, so none of these cases depends on the host's load.          //
//------------------------------------------------------------------------------------------------//

namespace {

spike::Record recordWithShare(float share, double t) {
	spike::Record r;
	r.unixSeconds = t;
	r.frames = 1024;
	r.sampleRate = 48000.0f;
	r.share = share;
	r.elapsedUs = share * 21333.3f;
	r.voices = 2;
	return r;
}

double fieldOf(const std::string& line, const char* key) {
	const size_t at = line.find(key);
	REQUIRE(at != std::string::npos);
	return std::atof(line.c_str() + at + std::strlen(key));
}

/// A recorder with nothing timed: only its ring and drain are used.
struct DrainRig {
	spike::Recorder recorder;
	DrainRig() { recorder.activate(kRate, spike::kDefaultThreshold); }
};

} // namespace

TEST_CASE("SR trim: past the per-minute limit records are not written, and the summary counts them and their worst share") {
	DrainRig rig;
	std::vector<std::string> lines;
	rig.recorder.drain("feathers", 600.0, &lines); // starts the minute
	REQUIRE(lines.empty());
	const size_t n = spike::kDetailPerMinute;
	REQUIRE(n == 60);
	// THE FIRST N, each share 1.0 + i/1000, so the minute's worst is among them and nothing is held back.
	std::vector<spike::Record> pushed;
	for (size_t i = 0; i < n; ++i) {
		pushed.push_back(recordWithShare(1.0f + static_cast<float>(i) / 1000.0f, 600.0 + static_cast<double>(i) * 0.01));
	}
	// THEN 25 MORE, under every one of the first N; their worst is 0.9.
	for (int i = 0; i < 25; ++i) {
		pushed.push_back(recordWithShare(i == 7 ? 0.9f : 0.3f + static_cast<float>(i) / 1000.0f, 601.0 + i * 0.01));
	}
	// Two drains in the same minute (the ring holds 255), the limit spanning both.
	for (size_t i = 0; i < 50; ++i) {
		REQUIRE(rig.recorder.pushForTest(pushed[i]));
	}
	rig.recorder.drain("feathers", 610.0, &lines);
	CHECK(lines.size() == 50);
	for (size_t i = 50; i < pushed.size(); ++i) {
		REQUIRE(rig.recorder.pushForTest(pushed[i]));
	}
	rig.recorder.drain("feathers", 620.0, &lines);
	REQUIRE(lines.size() == n); // the limit: 60 lines, no summary yet
	// THE FIRST N ARE UNCHANGED IN FORMAT: each is exactly `lineOf` of the record pushed, in order.
	for (size_t i = 0; i < n; ++i) {
		CHECK(lines[i] == spike::lineOf(pushed[i], "feathers"));
	}
	lines.clear();
	rig.recorder.drain("feathers", 661.0, &lines); // a new minute: the summary, and no held record (the worst was written)
	REQUIRE(lines.size() == 1);
	CHECK(lines[0].find("\"event\":\"summary\"") != std::string::npos);
	CHECK(fieldOf(lines[0], "\"suppressed\":") == 25.0);
	CHECK(fieldOf(lines[0], "\"suppressed_worst\":") == doctest::Approx(0.9).epsilon(1e-6));
	// AND THE NEXT MINUTE STARTS AFRESH: a record is written again, and its summary suppresses nothing.
	REQUIRE(rig.recorder.pushForTest(recordWithShare(0.4f, 662.0)));
	lines.clear();
	rig.recorder.drain("feathers", 670.0, &lines);
	CHECK(lines.size() == 1);
	lines.clear();
	rig.recorder.drain("feathers", 721.0, &lines);
	REQUIRE(lines.size() == 1);
	CHECK(fieldOf(lines[0], "\"suppressed\":") == 0.0);
	CHECK(lines[0].find("\"suppressed_worst\":null") != std::string::npos);
}

TEST_CASE("SR trim: the minute's worst block is written in detail at the summary even when it arrived past the limit") {
	DrainRig rig;
	std::vector<std::string> lines;
	rig.recorder.drain("feathers", 600.0, &lines);
	const size_t n = spike::kDetailPerMinute;
	for (size_t i = 0; i < n; ++i) {
		REQUIRE(rig.recorder.pushForTest(recordWithShare(0.5f, 600.0 + static_cast<double>(i) * 0.01)));
	}
	// PAST THE LIMIT: a 0.7, the worst at 2.5, then a 1.1 after it (the worst must be kept, not the latest).
	const spike::Record worst = recordWithShare(2.5f, 605.5);
	REQUIRE(rig.recorder.pushForTest(recordWithShare(0.7f, 605.0)));
	REQUIRE(rig.recorder.pushForTest(worst));
	REQUIRE(rig.recorder.pushForTest(recordWithShare(1.1f, 606.0)));
	rig.recorder.drain("feathers", 610.0, &lines);
	REQUIRE(lines.size() == n);
	for (const std::string& line : lines) {
		CHECK(fieldOf(line, "\"share\":") == doctest::Approx(0.5).epsilon(1e-6));
	}
	lines.clear();
	rig.recorder.drain("feathers", 661.0, &lines);
	REQUIRE(lines.size() == 2); // the held worst, in detail, then the summary
	CHECK(lines[0] == spike::lineOf(worst, "feathers"));
	CHECK(lines[1].find("\"event\":\"summary\"") != std::string::npos);
	// THE WORST WAS WRITTEN, SO IT IS NOT SUPPRESSED: two records never got a line, the worst of them 1.1.
	CHECK(fieldOf(lines[1], "\"suppressed\":") == 2.0);
	CHECK(fieldOf(lines[1], "\"suppressed_worst\":") == doctest::Approx(1.1).epsilon(1e-6));
}

TEST_CASE("SR trim: at the daily size cap spike lines stop, one cap line is written, and summaries go on") {
	const std::filesystem::path dir = std::filesystem::temp_directory_path() / ("feathers-spike-cap-" + std::to_string(::getpid()));
	std::filesystem::remove_all(dir);
	std::filesystem::create_directories(dir);
	const std::filesystem::path file = dir / "day.spikes.jsonl";
	const std::string spikeLine = spike::lineOf(recordWithShare(0.8f, 700.0), "feathers");
	const std::string summaryLine = "{\"v\":3,\"event\":\"summary\",\"plugin\":\"feathers\",\"t\":720,\"blocks\":5}";
	const uint64_t cap = 3 * (spikeLine.size() + 1) + 10; // the fourth spike line takes the file over the cap
	// FIRST APPEND: five spikes. Four are written (the fourth crosses the cap), then the cap line; the fifth is not.
	REQUIRE(spike::appendCapped(file, { spikeLine, spikeLine, spikeLine, spikeLine, spikeLine }, 700.0, cap));
	// SECOND APPEND, already over: spikes dropped, the summary kept, and no second cap line.
	REQUIRE(spike::appendCapped(file, { spikeLine, summaryLine, spikeLine }, 720.0, cap));
	std::ifstream in(file);
	std::vector<std::string> got;
	for (std::string line; std::getline(in, line);) {
		got.push_back(line);
	}
	REQUIRE(got.size() == 6);
	for (size_t i = 0; i < 4; ++i) {
		CHECK(got[i] == spikeLine); // unchanged in format up to the cap
	}
	CHECK(got[4].find("\"event\":\"cap\"") != std::string::npos);
	CHECK(fieldOf(got[4], "\"cap_bytes\":") == static_cast<double>(cap));
	CHECK(got[5] == summaryLine);
	size_t capLines = 0;
	for (const std::string& line : got) {
		capLines += line.find("\"event\":\"cap\"") != std::string::npos ? 1 : 0;
	}
	CHECK(capLines == 1);

	// THE CAP'S DEFAULT AND ITS VARIABLE, which `append` reads.
	::unsetenv("FEATHERS_SPIKE_LOG_CAP_MB");
	CHECK(spike::capBytesFromEnvironment() == 32ull * 1024 * 1024);
	::setenv("FEATHERS_SPIKE_LOG_CAP_MB", "2", 1);
	CHECK(spike::capBytesFromEnvironment() == 2ull * 1024 * 1024);
	::setenv("FEATHERS_SPIKE_LOG_CAP_MB", "nonsense", 1);
	CHECK(spike::capBytesFromEnvironment() == spike::kDailyCapBytes);
	::unsetenv("FEATHERS_SPIKE_LOG_CAP_MB");
	std::filesystem::remove_all(dir);
}

//------------------------------------------------------------------------------------------------//
// THE SECOND PASS: CPU time beside wall time, the time outside                                     //
// the devices broken into stages, and the devices inside containers named by their path.             //
//------------------------------------------------------------------------------------------------//

namespace {

/// The one record a block left, after the ring was emptied before it.
spike::Record onlyRecord(Rig& rig) {
	spike::Record rec;
	REQUIRE(rig.recorder.popForTest(&rec));
	spike::Record extra;
	CHECK_FALSE(rig.recorder.popForTest(&extra));
	return rec;
}

void emptyRing(Rig& rig) {
	spike::Record dummy;
	while (rig.recorder.popForTest(&dummy)) {
	}
}

/// A slow block's record: `us` microseconds in the SLOW device, asleep or spinning.
spike::Record slowBlock(Rig& rig, int us, bool sleeps) {
	emptyRing(rig);
	g_slowSleeps = sleeps;
	g_slowUs = us;
	rig.block();
	g_slowUs = 0;
	g_slowSleeps = false;
	return onlyRecord(rig);
}

/// The record's entry at `path`, or null.
const spike::DeviceSpend* entryAt(const spike::Record& rec, const char* path) {
	for (size_t i = 0; i < rec.devices; ++i) {
		if (std::string(rec.top[i].path) == path) {
			return &rec.top[i];
		}
	}
	return nullptr;
}

std::string pathsOf(const spike::Record& rec) {
	std::string s;
	for (size_t i = 0; i < rec.devices; ++i) {
		s += std::string(i == 0 ? "" : " ") + rec.top[i].path;
	}
	return s;
}

} // namespace

TEST_CASE("SR: a device that sleeps is time OFF the CPU — preempted_us carries the sleep and cpu_us stays small") {
	// SLEEPING IS THE THREAD OFF THE CPU, which is what a host's preemption is to the block: wall time the plugin did not
	// spend. A 4 ms sleep in a 5.33 ms budget is a spike; its CPU time is the engine's own few microseconds.
	Rig rig;
	REQUIRE(rig.engine.addDevice("SLOW") != rack::RackEngine::kNoSlot);
	rig.block();
	const spike::Record rec = slowBlock(rig, 4000, true);
	REQUIRE(std::isfinite(rec.cpuUs));
	REQUIRE(std::isfinite(rec.preemptedUs));
	MESSAGE("asleep: wall " << rec.elapsedUs << " us, cpu " << rec.cpuUs << " us, preempted " << rec.preemptedUs << " us");
	CHECK(rec.elapsedUs >= 4000.0f);
	CHECK(rec.preemptedUs >= 3800.0f);                       // the sleep, less a scheduler's rounding
	CHECK(rec.preemptedUs <= rec.elapsedUs);
	CHECK(rec.cpuUs < 0.15f * rec.elapsedUs);                // a sleeping thread spends next to nothing
	CHECK(rec.preemptedUs == doctest::Approx(rec.elapsedUs - rec.cpuUs).epsilon(1e-3));
	// AND THE DEVICE STILL OWNS THE WALL TIME: what the record names is where the block was, not what it did.
	REQUIRE(rec.devices >= 1);
	CHECK(std::string(rec.top[0].path) == "SLOW");
	CHECK(rec.top[0].us >= 4000.0f);
}

TEST_CASE("SR: a device that spins is time ON the CPU — cpu_us carries the wall time and preempted_us stays small") {
	// THE DEVICE SPINS ON ITS OWN CPU CLOCK, so its work is 4 ms of CPU however loaded the host is (a wall-clock spin was
	// preempted in 29 of 30 runs beside 18 busy processes, 2026-11-04, and then really did spend less CPU than wall).
	// What must hold: cpu_us carries all of that work; wall - cpu is the preempted time exactly; and a block the
	// scheduler never switched out (nivcsw 0) has no preempted time to speak of and is not flagged.
	Rig rig;
	REQUIRE(rig.engine.addDevice("SLOW") != rack::RackEngine::kNoSlot);
	rig.block();
	g_slowSpinsCpu = true;
	g_slowCpuSpentNs = 0;
	const spike::Record rec = slowBlock(rig, 4000, false);
	g_slowSpinsCpu = false;
	const double spentUs = static_cast<double>(g_slowCpuSpentNs.load()) * 1e-3;
	REQUIRE(std::isfinite(rec.cpuUs));
	MESSAGE("spinning: wall " << rec.elapsedUs << " us, cpu " << rec.cpuUs << " us, the device's own " << spentUs
	                          << " us, preempted " << rec.preemptedUs << " us, nivcsw " << rec.nivcsw);
	REQUIRE(spentUs >= 4000.0);
	CHECK(rec.cpuUs >= spentUs - 1.0);         // the same clock: the block's CPU holds the device's, to float rounding
	CHECK(rec.cpuUs <= rec.elapsedUs + 50.0f); // the spike's own CPU read lands after the wall clock stopped
	CHECK(std::abs(rec.preemptedUs - std::max(0.0f, rec.elapsedUs - rec.cpuUs)) < 0.05f); // absolute: floats near 16000 us
	if (rec.nivcsw == 0) {
		CHECK(rec.preemptedUs < 0.1f * rec.elapsedUs);
		CHECK_FALSE(rec.preempted);
	}
	if (rec.preempted) {
		CHECK(rec.nivcsw > 0);
	}
}

TEST_CASE("SR: other_us is the wall time outside the devices, and each stage counts only its own time") {
	// A BLOCK SHAPED AS A PLUGIN'S: 1.5 ms spent in a stage around nothing (Inputs), then the engine inside a Bus stage
	// with a 2 ms device in it. Inputs must read its 1.5 ms; Bus must read only the engine's own overhead, because the
	// device inside it is taken out; the devices and other_us must add up to the wall time.
	Rig rig;
	const size_t slow = rig.engine.addDevice("SLOW");
	REQUIRE(slow != rack::RackEngine::kNoSlot);
	rig.block();
	emptyRing(rig);
	const spike::BlockStart start = rig.recorder.beginBlock();
	uint64_t inputsNs = 0; // what the wait really took: a loaded host stretches it, so bounds are against THIS
	{
		const rack::StageScope inputs(rack::Stage::Inputs);
		inputsNs = busyWait(1500);
	}
	g_slowOnce = true;
	g_slowSpentNs = 0;
	g_slowUs = 2000;
	{
		const rack::StageScope bus(rack::Stage::Bus);
		rig.engine.processStereo(rig.l.data(), rig.r.data(), kFrames, backplane::unknownBlock());
	}
	g_slowUs = 0;
	g_slowOnce = false;
	spike::BlockFacts facts;
	facts.frames = kFrames;
	rig.recorder.endBlock(start, facts);
	const spike::Record rec = onlyRecord(rig);
	const auto stage = [&](rack::Stage s) { return rec.stagesUs[static_cast<size_t>(s)]; };
	MESSAGE("wall " << rec.elapsedUs << ", devices " << rec.devicesUs << ", other " << rec.otherUs << ", inputs "
	                << stage(rack::Stage::Inputs) << ", bus " << stage(rack::Stage::Bus) << ", resolve "
	                << stage(rack::Stage::Resolve));
	// THE DEVICES: the one slow device, summed at the top of the tree.
	const double inputsUs = static_cast<double>(inputsNs) * 1e-3;
	const double slowUs = static_cast<double>(g_slowSpentNs.load()) * 1e-3;
	MESSAGE("measured by the waits themselves: inputs " << inputsUs << " us, device " << slowUs << " us");
	REQUIRE(slowUs >= 2000.0);
	CHECK(rec.devicesUs >= slowUs - 20.0); // two clocks: the counter (calibrated) against the wait's steady_clock
	CHECK(rec.devicesUs < slowUs + 300.0);
	// OTHER = WALL - DEVICES, and it holds the 1.5 ms of Inputs.
	CHECK(rec.otherUs == doctest::Approx(rec.elapsedUs - rec.devicesUs).epsilon(1e-4));
	CHECK(rec.otherUs + rec.devicesUs == doctest::Approx(rec.elapsedUs).epsilon(1e-4));
	CHECK(rec.otherUs >= inputsUs - 50.0);
	// EACH STAGE ITS OWN: Inputs is its 1.5 ms; Bus is the engine around the device, NOT the device.
	// the wait reads steady_clock, the stage the calibrated counter: they agree to a few tens of microseconds
	CHECK(stage(rack::Stage::Inputs) >= inputsUs - 50.0);
	CHECK(stage(rack::Stage::Inputs) < inputsUs + 200.0);
	CHECK(stage(rack::Stage::Bus) < 0.5 * slowUs); // the device's 2 ms is not in it (a loaded host may stretch the rest)
	CHECK(stage(rack::Stage::Bus) >= 0.0f);
	// AND THE STAGES NEVER CLAIM MORE THAN IS OUTSIDE THE DEVICES (the engine's Resolve nests in Bus and comes out of it).
	float staged = 0.0f;
	for (float s : rec.stagesUs) {
		staged += s;
	}
	CHECK(staged <= rec.otherUs + 1.0f);
	// What this block did outside a stage is a few microseconds of glue — PLUS any time the thread was off the CPU between
	// stages, which is wall time no stage owns (MEASURED on a loaded macOS CI runner, 2026-11-08: 296 µs short with the
	// fixed 200 µs slack alone). So the slack carries the record's own preemption, not a bigger constant.
	CHECK(staged >= rec.otherUs - 200.0f - std::max(0.0f, rec.preemptedUs));
}

TEST_CASE("SR: a slow device inside a Rack's chain and inside SKLITTER's post chain is named by its path") {
	Rig rig;
	const size_t rackSlot = rig.engine.addDevice(rack::parallel::kTraits.tag);
	const size_t sklitterSlot = rig.engine.addDevice(fx::sklitter::kDescriptor.traits.tag);
	REQUIRE(rackSlot != rack::RackEngine::kNoSlot);
	REQUIRE(sklitterSlot != rack::RackEngine::kNoSlot);
	rig.block(); // provisioned
	// THE SLOW DEVICE INTO THE RACK'S FIRST CHAIN, AND INTO SKLITTER'S POST CHAIN — by their own bridges, as an editor does.
	void* rackStorage = const_cast<void*>(rig.engine.storageForTest(rackSlot));
	void* sklitterStorage = const_cast<void*>(rig.engine.storageForTest(sklitterSlot));
	REQUIRE(rackStorage != nullptr);
	REQUIRE(sklitterStorage != nullptr);
	rack::InnerChain* chain = rack::parallel::rackOf(rackStorage)->chainAt(0);
	rack::InnerChain* post = fx::sklitter::postChainOf(sklitterStorage);
	REQUIRE(chain != nullptr);
	REQUIRE(post != nullptr);
	REQUIRE(chain->bridge() != nullptr);
	REQUIRE(post->bridge() != nullptr);
	REQUIRE(chain->bridge()->addDevice("SLOW") != kNoInstance);
	REQUIRE(post->bridge()->addDevice("SLOW") != kNoInstance);
	for (int b = 0; b < 8; ++b) {
		rig.block(); // the inner drains land the devices
	}
	const spike::Record rec = slowBlock(rig, 1500, false); // 1.5 ms in EACH slow device: 3 ms, a spike
	MESSAGE("paths: " << pathsOf(rec));
	const std::string rackPath = std::string(rack::parallel::kTraits.tag) + "/chain1/SLOW";
	const std::string sklitterPath = std::string(fx::sklitter::kDescriptor.traits.tag) + "/post/SLOW";
	const spike::DeviceSpend* inRack = entryAt(rec, rackPath.c_str());
	const spike::DeviceSpend* inPost = entryAt(rec, sklitterPath.c_str());
	REQUIRE(inRack != nullptr);
	REQUIRE(inPost != nullptr);
	// EACH AT ITS OWN TIME, ONE LEVEL DOWN, UNDER THE SLOT ITS CONTAINER HOLDS IN THE LANE.
	CHECK(inRack->us >= 1500.0f);
	CHECK(inPost->us >= 1500.0f);
	CHECK(inRack->depth == 1);
	CHECK(inPost->depth == 1);
	CHECK(inRack->slots[0] == rackSlot);
	CHECK(inPost->slots[0] == sklitterSlot);
	CHECK(std::string(inRack->tag) == "SLOW");
	// THE CONTAINERS ARE THERE TOO, each holding at least what is inside it — and the lane's total counts them once.
	const spike::DeviceSpend* rackEntry = entryAt(rec, rack::parallel::kTraits.tag);
	const spike::DeviceSpend* sklitterEntry = entryAt(rec, fx::sklitter::kDescriptor.traits.tag);
	REQUIRE(rackEntry != nullptr);
	REQUIRE(sklitterEntry != nullptr);
	CHECK(rackEntry->us >= inRack->us);
	CHECK(sklitterEntry->us >= inPost->us);
	CHECK(rec.devicesUs >= 3000.0f);
	CHECK(rec.devicesUs <= rec.elapsedUs);
	CHECK(rec.devicesUs == doctest::Approx(rackEntry->us + sklitterEntry->us).epsilon(1e-3));
	// AND THE LINE CARRIES THE PATH AND THE SLOTS.
	const std::string line = spike::lineOf(rec, "feathers");
	CHECK(line.find("\"path\":\"" + rackPath + "\",\"slots\":[" + std::to_string(rackSlot) + ",") != std::string::npos);
	CHECK(line.find("\"v\":3") != std::string::npos);
	CHECK(line.find("\"cpu_us\":") != std::string::npos);
	CHECK(line.find("\"preempted_us\":") != std::string::npos);
	CHECK(line.find("\"other_us\":") != std::string::npos);
}

TEST_CASE("SR: nothing allocates on the audio thread with containers in the lane and every block a spike") {
	Rig rig(1e-9);
	const size_t rackSlot = rig.engine.addDevice(rack::parallel::kTraits.tag);
	const size_t sklitterSlot = rig.engine.addDevice(fx::sklitter::kDescriptor.traits.tag);
	REQUIRE(rackSlot != rack::RackEngine::kNoSlot);
	REQUIRE(sklitterSlot != rack::RackEngine::kNoSlot);
	rig.block();
	REQUIRE(rack::parallel::rackOf(const_cast<void*>(rig.engine.storageForTest(rackSlot)))->chainAt(0)->bridge()->addDevice("SLOW")
	        != kNoInstance);
	REQUIRE(fx::sklitter::postChainOf(const_cast<void*>(rig.engine.storageForTest(sklitterSlot)))->bridge()->addDevice("SLOW")
	        != kNoInstance);
	for (int b = 0; b < 8; ++b) {
		rig.block();
	}
	emptyRing(rig);
	g_allocs = 0;
	g_count = true;
	for (int b = 0; b < 300; ++b) { // every one a record with paths and the CPU clock's second read; the ring overflows
		rig.block();
	}
	g_count = false;
	CHECK(g_allocs.load() == 0);
	spike::Record rec;
	REQUIRE(rig.recorder.popForTest(&rec));
	CHECK(rec.timed >= 4); // the two containers and the device in each
}

TEST_CASE("SR [measure]: what the recorder and the per-device timing cost a block with nothing spiking") {
	// A REAL CHAIN (filter, delay, reverb, shaper), 2000 blocks: with NO recorder at all (a ship build: nothing activated,
	// so every scope sees none) against the recorder binding its tree every block, ALTERNATED and the minimum of five
	// taken, the host's load beside it. And the thread CPU clock's own price, since the recorder reads it once a block.
	const auto run = [](bool on) {
		Rig rig(spike::kDefaultThreshold, on);
		for (const char* tag : { fx::filter::kTraits.tag, fx::delay::kTraits.tag, fx::featherverb::kTraits.tag, fx::shaper::kTraits.tag }) {
			(void)rig.engine.addDevice(tag);
		}
		for (int b = 0; b < 50; ++b) {
			rig.block();
		}
		const auto t0 = std::chrono::steady_clock::now();
		for (int b = 0; b < 2000; ++b) {
			rig.block();
		}
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 2000.0;
	};
	double off = 1e9, on = 1e9;
	for (int round = 0; round < 5; ++round) {
		off = std::min(off, run(false));
		on = std::min(on, run(true));
	}
	double cpuRead = 1e9;
	for (int round = 0; round < 5; ++round) {
		uint64_t sink = 0;
		const auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < 20000; ++i) {
			sink += spike::threadCpuNs();
		}
		cpuRead = std::min(cpuRead, std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() / 20000.0);
		CHECK(sink > 0);
	}
	double load = -1.0;
	if (std::FILE* f = std::fopen("/proc/loadavg", "r")) {
		if (std::fscanf(f, "%lf", &load) != 1) {
			load = -1.0;
		}
		std::fclose(f);
	}
	MESSAGE("a 4-device chain, 256 frames: " << off << " us a block without the recorder, " << on << " us with it ("
	                                          << (on - off) << " us, " << 100.0 * (on - off) / off << "%); one thread-CPU read "
	                                          << cpuRead << " ns; load " << load);
	CHECK(on > 0.0);
}

#if defined(__linux__)
TEST_CASE("SR: a block the scheduler takes the CPU from counts its involuntary switches and is flagged preempted") {
	// THE RACK SESSION'S FINDING (2026-11-04): the 4-12 ms Delay spikes seen live were blocks with an involuntary context switch
	// and a few hundred microseconds of CPU. So the record carries `nivcsw` and a derived `preempted`. Made to happen here:
	// the audio thread and two spinning competitors pinned to ONE CPU, so the scheduler must share it three ways and the
	// slow device's 30 ms spin is switched out involuntarily, getting about a third of the wall time.
	Rig rig;
	REQUIRE(rig.engine.addDevice("SLOW") != rack::RackEngine::kNoSlot);
	rig.block();
	cpu_set_t was;
	CPU_ZERO(&was);
	REQUIRE(pthread_getaffinity_np(pthread_self(), sizeof was, &was) == 0);
	int cpu = 0;
	while (cpu < CPU_SETSIZE && !CPU_ISSET(cpu, &was)) {
		++cpu;
	}
	REQUIRE(cpu < CPU_SETSIZE);
	cpu_set_t one;
	CPU_ZERO(&one);
	CPU_SET(cpu, &one);
	std::atomic<bool> stop { false };
	std::atomic<int> pinned { 0 };
	const auto compete = [&] {
		(void)pthread_setaffinity_np(pthread_self(), sizeof one, &one);
		pinned.fetch_add(1);
		while (!stop.load(std::memory_order_relaxed)) {
		}
	};
	REQUIRE(pthread_setaffinity_np(pthread_self(), sizeof one, &one) == 0);
	std::thread a(compete);
	std::thread b(compete);
	while (pinned.load() < 2) {
		std::this_thread::yield();
	}
	g_slowOnce = true; // one 30 ms spin, not one per control tick
	const spike::Record rec = slowBlock(rig, 30000, false);
	g_slowOnce = false;
	stop = true;
	a.join();
	b.join();
	REQUIRE(pthread_setaffinity_np(pthread_self(), sizeof was, &was) == 0);
	MESSAGE("shared CPU: wall " << rec.elapsedUs << " us, cpu " << rec.cpuUs << " us, nivcsw " << rec.nivcsw);
	REQUIRE(std::isfinite(rec.cpuUs));
	CHECK(rec.nivcsw > 0);
	CHECK(rec.cpuUs < 0.5f * rec.elapsedUs);
	CHECK(rec.preempted);
	const std::string line = spike::lineOf(rec, "feathers");
	CHECK(line.find("\"nivcsw\":" + std::to_string(rec.nivcsw) + ",") != std::string::npos);
	CHECK(line.find("\"preempted\":true") != std::string::npos);
	CHECK(line.find("\"v\":3") != std::string::npos);
}
#endif
