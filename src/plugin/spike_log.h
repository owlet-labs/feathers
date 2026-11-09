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
#pragma once

// THE CPU SPIKE RECORDER (backlog SR;). A glitch in a live session left no record: nothing
// timed the plugin's own process(). Now, in a FEATHERS_DIAGNOSTIC build (every install, no ship), each plugin times every
// block against its budget (frames / sample rate) and, past a threshold share of it, writes one fixed-size record into a
// lock-free single-producer ring sized at activate. A main-thread timer drains the ring into
// `<date>.spikes.jsonl` beside the likes log, with a once-a-minute summary line.
//
// THE AUDIO THREAD'S HALF (`Recorder::beginBlock` / `endBlock`): two cycle-counter reads and ONE thread-CPU-clock read per
// block, a histogram bin and a counter, and, only on a spike, the second thread-CPU read, a walk of the block's cost tree
// for the worst few devices, one wall-clock read and one ring push. No allocation, no lock. The devices and stages are
// timed on EVERY block into a `rack::CostTree` bound to the thread for the block (two counter reads per device per pass,
// `rack::CostScope`), because a spike is known only at the block's end and the block's own costs are the ones worth
// naming — affordable because the counter is a register read (`rack::costTicks`: this host's `steady_clock` is a 1.2 us
// HPET syscall).
//
// **WALL TIME AND THE THREAD'S OWN CPU TIME (second pass).** The counter measures
// WALL time, so a block the OS took the thread off the CPU for reads as a slow block. `CLOCK_THREAD_CPUTIME_ID` is this
// thread's CPU time, so `wall - cpu` is the time it was not running: preempted, blocked, faulting. It is a syscall here
// (MEASURED 0.8 us a read on this host), so it is read at the block's start always and at its end ONLY on a spike — the
// end is needed only to explain a record. One read a block, not two; the spike's own read lands after the wall clock
// stopped, so it only ever biases `cpu_us` up by about a microsecond.
//
// THIS LIBRARY IS COMPILED IN EVERY BUILD AND LINKED ONLY WHERE NAMED: the two plugins reach it under
// `#ifdef FEATHERS_DIAGNOSTIC`, so a ship build has no recorder, no timer and no clock read in process(), and its engines'
// cost scopes see no recorder (`rack::gCostTreesLive` is 0) and read no clock.

#include "rack/device_cost.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace feathers::spike {

/// The record format's version, in every line's `"v"` (2: cpu and preempted time, the stages, paths into containers; 3: nivcsw and preempted).
inline constexpr int kFormatVersion = 3;
/// The worst devices a record names, across the whole tree (a container and the devices inside it alike).
inline constexpr size_t kTopDevices = 8;
/// A device's path in a record: its ancestors' tags and their chains' names, `SKLITTER/post/DELA`, cut at this length.
inline constexpr size_t kPathChars = 64;
/// The depth a record keeps each step's slot for.
inline constexpr size_t kPathDepth = 6;
/// Records the ring holds between drains (one drain a second); a burst past it is dropped and counted.
inline constexpr size_t kRingRecords = 256;
/// The default threshold: half the block's budget.
inline constexpr double kDefaultThreshold = 0.5;
/// The histogram behind the summary's p99: shares 0..2 in 1% bins, the last bin everything above.
inline constexpr size_t kShareBins = 201;
/// THE PER-MINUTE LIMIT: at most this many
/// detailed spike lines per recorder between two summaries. The rest are counted in the summary's `suppressed` and
/// `suppressed_worst`, except the minute's WORST record, which is held and written in detail just before the summary when it
/// was one of the suppressed.
inline constexpr size_t kDetailPerMinute = 60;
/// THE DAILY SIZE CAP: once the day's `.spikes.jsonl` reaches this many bytes, `append` writes no more spike lines into it,
/// writes one `"event":"cap"` line, and keeps writing the summaries. `$FEATHERS_SPIKE_LOG_CAP_MB` overrides it.
inline constexpr uint64_t kDailyCapBytes = 32ull * 1024 * 1024;

/// What else ran in the block, as cheap to know as a counter compare.
enum Flag : uint32_t {
	kFlagNotes = 1u << 0,      ///< note or parameter events arrived this block
	kFlagRebake = 1u << 1,     ///< the character bank built a table (a rebake)
	kFlagLikesFit = 1u << 2,   ///< the likes' phase warp was refitted (a new serial)
	kFlagLatency = 1u << 3,    ///< the plugin asked the host to restart for a latency change
	kFlagTransport = 1u << 4,  ///< the transport started, stopped or jumped
	kFlagFirst = 1u << 5,      ///< the first block after activate
};

struct DeviceSpend {
	char tag[13] = {};       ///< its own tag
	char path[kPathChars] = {};
	std::array<uint16_t, kPathDepth> slots {}; ///< the slot at each step of the path, outermost first
	uint8_t depth = 0;       ///< 0: a device in the lane itself
	float us = 0.0f;         ///< everything it spent this block, the devices inside it included
	[[nodiscard]] uint16_t slot() const noexcept { return slots[depth < kPathDepth ? depth : kPathDepth - 1]; }
};

/// ONE SPIKE, fixed size so the ring never allocates.
struct Record {
	double unixSeconds = 0.0;
	uint32_t frames = 0;
	float sampleRate = 0.0f;
	float elapsedUs = 0.0f; ///< wall time
	float share = 0.0f;     ///< elapsed / budget
	float cpuUs = 0.0f;       ///< the thread's own CPU time over the block; NaN where the platform gives none
	float preemptedUs = 0.0f; ///< wall - cpu, floored at 0: the time the thread was not running; NaN with cpu
	int32_t nivcsw = -1;      ///< the thread's involuntary context switches over the block (`ru_nivcsw`); -1 where the platform gives none
	bool preempted = false;   ///< `nivcsw > 0` and `cpu_us` under `kPreemptedCpuShare` of the wall time: the OS took the block
	float devicesUs = 0.0f;   ///< the lane's devices (depth 0) summed: each counts what it holds
	float otherUs = 0.0f;     ///< wall - devices
	std::array<float, rack::kStageCount> stagesUs {}; ///< each stage's own time (`rack::Stage`), the devices taken out
	uint16_t voices = 0;
	double songBeats = 0.0;   ///< NaN when the host gives none
	double songSeconds = 0.0; ///< NaN when the host gives none
	uint32_t flags = 0;
	uint16_t timed = 0;       ///< devices timed this block, at every depth
	uint32_t untimed = 0;     ///< device passes the tree had no room for
	uint8_t devices = 0;
	std::array<DeviceSpend, kTopDevices> top {};
};

/// A SINGLE-PRODUCER, SINGLE-CONSUMER RING: the audio thread pushes, the main thread pops. Sized once, on the main thread.
class Ring {
public:
	void reserve(size_t records);
	/// AUDIO THREAD. False, and a drop counted, when full: it never waits.
	bool push(const Record& record) noexcept;
	/// MAIN THREAD.
	bool pop(Record* out) noexcept;
	[[nodiscard]] uint64_t drops() const noexcept { return drops_.load(std::memory_order_relaxed); }
	[[nodiscard]] size_t capacity() const noexcept { return slots_.size(); }

private:
	std::vector<Record> slots_;
	std::atomic<size_t> head_ { 0 }; ///< next write (producer)
	std::atomic<size_t> tail_ { 0 }; ///< next read (consumer)
	std::atomic<uint64_t> drops_ { 0 };
};

/// What the block ran with, handed in at its end.
struct BlockFacts {
	uint32_t frames = 0;
	uint16_t voices = 0;
	double songBeats = 0.0;
	double songSeconds = 0.0;
	uint32_t flags = 0;
};

/// The block's start, as `beginBlock` took it.
struct BlockStart {
	uint64_t ticks = 0;
	uint64_t cpuNs = 0; ///< `kNoCpuTime` where the platform gives none
	uint64_t nivcsw = 0; ///< `kNoSwitches` where the platform gives none
	rack::CostTree* outer = nullptr; ///< what was bound before, put back at the end
};

/// The thread CPU clock's "none".
inline constexpr uint64_t kNoCpuTime = UINT64_MAX;
/// THIS THREAD'S CPU TIME in nanoseconds (`CLOCK_THREAD_CPUTIME_ID`), or `kNoCpuTime`. A syscall on Linux (0.8 us here).
[[nodiscard]] uint64_t threadCpuNs() noexcept;
/// The involuntary-switch count's "none".
inline constexpr uint64_t kNoSwitches = UINT64_MAX;
/// THIS THREAD'S INVOLUNTARY CONTEXT SWITCHES so far (`getrusage(RUSAGE_THREAD).ru_nivcsw`, Linux), or `kNoSwitches`. A
/// syscall: read at every block's start and, like the CPU clock, at the end only of a spike.
[[nodiscard]] uint64_t threadInvoluntarySwitches() noexcept;
/// A SPIKE IS `preempted` when the thread was switched out involuntarily AND spent less than this share of the wall time on
/// the CPU: most of the block was the OS's, not ours (backlog SR, the rack session's Delay profile, 2026-11-04).
inline constexpr double kPreemptedCpuShare = 0.5;

class Recorder {
public:
	Recorder() = default;
	~Recorder();
	Recorder(const Recorder&) = delete;
	Recorder& operator=(const Recorder&) = delete;

	/// MAIN THREAD, at activate: the ring (once), the rate and the threshold (a share of the budget, > 0).
	void activate(double sampleRate, double threshold);
	/// AUDIO THREAD, at the block's start: the counter, the thread's CPU time, and this recorder's cost tree emptied and
	/// bound to the thread, so every engine the block runs times its devices into it.
	[[nodiscard]] BlockStart beginBlock() noexcept;
	/// AUDIO THREAD, at the block's end: the stats always; a record past the threshold; the tree unbound.
	void endBlock(const BlockStart& start, const BlockFacts& facts) noexcept;

	/// MAIN THREAD: every waiting record as a JSON line, and — once `unixNow` has moved into a new minute since the last —
	/// the summary line of the blocks since then. `plugin` names the plugin in every line.
	void drain(const char* plugin, double unixNow, std::vector<std::string>* lines);

	[[nodiscard]] uint64_t drops() const noexcept { return ring_.drops(); }
	[[nodiscard]] double threshold() const noexcept { return threshold_; }
	/// TEST ONLY: pop the next record without formatting it.
	bool popForTest(Record* out) noexcept { return ring_.pop(out); }
	/// TEST ONLY: push a record as the audio thread would, so the drain's limits can be checked on chosen shares.
	bool pushForTest(const Record& record) noexcept { return ring_.push(record); }
	/// MAIN THREAD: the detailed lines allowed between two summaries (`kDetailPerMinute` unless a test raises it).
	void setDetailPerMinute(size_t n) noexcept { detailPerMinute_ = n; }

private:
	Ring ring_;
	double sampleRate_ = 48000.0;
	double threshold_ = kDefaultThreshold;
	rack::CostTree tree_;
	bool live_ = false; ///< counted in `rack::gCostTreesLive`
	// THE MINUTE'S STATS — written by the audio thread, taken (exchanged to zero) by the main thread's summary.
	std::atomic<uint64_t> blocks_ { 0 };
	std::atomic<uint32_t> worstBits_ { 0 }; ///< the worst share's float bits (shares are non-negative: bits order as values)
	std::array<std::atomic<uint32_t>, kShareBins> bins_ {};
	double minuteStarted_ = -1.0;
	// THE COUNTER'S RATE: nanoseconds per tick, measured over 2 ms at activate and refined at every drain against the
	// span since activate (main thread writes, audio thread reads).
	std::atomic<double> nsPerTick_ { 1.0 };
	uint64_t calTicks_ = 0;
	uint64_t calNs_ = 0;
	void calibrate() noexcept;
	void fill(Record& r, double nsPerTick) const noexcept;
	uint64_t dropsReported_ = 0;
	// THE PER-MINUTE LIMIT (main thread only: the drain's side, never the audio thread's).
	size_t detailPerMinute_ = kDetailPerMinute;
	size_t detailed_ = 0;        ///< detailed lines written since the last summary
	float detailedWorst_ = -1.0f; ///< the worst share among them
	uint64_t suppressed_ = 0;    ///< records not written in detail since the last summary
	float suppressedWorst_ = -1.0f;
	bool havePending_ = false;   ///< the worst suppressed record, held for the summary
	Record pending_ {};
};

/// The record as one JSON line (no newline).
std::string lineOf(const Record& record, const char* plugin);

/// WHERE THE LINES GO: `$FEATHERS_SPIKE_LOG_DIR` when set (EMPTY or `off`: logging off); else the likes log's own directory
/// (`$XDG_DATA_HOME/feathers/likes-log/`, else `~/.local/share/feathers/likes-log/`).
std::filesystem::path directory();
/// The day's file in `dir`: `<YYYY-MM-DD>.spikes.jsonl`, by the local date at `unixSeconds`.
std::filesystem::path fileFor(const std::filesystem::path& dir, double unixSeconds);
/// Appends `lines` to the day's file under the daily cap (`capBytesFromEnvironment`); false if logging is off or the write
/// failed. MAIN THREAD.
bool append(const std::vector<std::string>& lines, double unixNow) noexcept;
/// THE CAPPED APPEND ITSELF: while `file` is under `capBytes`, every line; from the line that takes it to the cap on, only the
/// lines that are not spikes (the summaries), and one `"event":"cap"` line written right after the line that crossed it —
/// once per file, since a file's size only grows. MAIN THREAD.
bool appendCapped(const std::filesystem::path& file, const std::vector<std::string>& lines, double unixNow,
                  uint64_t capBytes) noexcept;
/// THE CAP: `$FEATHERS_SPIKE_LOG_CAP_MB` (MiB, >= 0; 0 writes no spike lines at all) when it parses, else `kDailyCapBytes`.
uint64_t capBytesFromEnvironment();
/// THE THRESHOLD: `$FEATHERS_SPIKE_THRESHOLD` when it parses to a share in (0, 10], else `fallback`.
double thresholdFromEnvironment(double fallback);

} // namespace feathers::spike
