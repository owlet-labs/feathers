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
#include "plugin/spike_log.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <limits>
#include <system_error>
#if defined(__unix__) || defined(__APPLE__)
#include <time.h>
#endif
#if defined(__linux__)
#include <sys/resource.h>
#endif

namespace feathers::spike {

void Ring::reserve(size_t records) {
	if (slots_.size() != records) {
		slots_.assign(records, Record {});
		head_.store(0, std::memory_order_relaxed);
		tail_.store(0, std::memory_order_relaxed);
	}
}

bool Ring::push(const Record& record) noexcept {
	const size_t n = slots_.size();
	if (n == 0) {
		drops_.fetch_add(1, std::memory_order_relaxed);
		return false;
	}
	const size_t head = head_.load(std::memory_order_relaxed);
	const size_t next = (head + 1) % n;
	if (next == tail_.load(std::memory_order_acquire)) {
		drops_.fetch_add(1, std::memory_order_relaxed); // full: dropped and counted, never waited for
		return false;
	}
	slots_[head] = record;
	head_.store(next, std::memory_order_release);
	return true;
}

bool Ring::pop(Record* out) noexcept {
	const size_t n = slots_.size();
	const size_t tail = tail_.load(std::memory_order_relaxed);
	if (n == 0 || tail == head_.load(std::memory_order_acquire)) {
		return false;
	}
	*out = slots_[tail];
	tail_.store((tail + 1) % n, std::memory_order_release);
	return true;
}

uint64_t threadCpuNs() noexcept {
#if defined(__unix__) || defined(__APPLE__)
	timespec ts {};
	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) {
		return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000u + static_cast<uint64_t>(ts.tv_nsec);
	}
#endif
	return kNoCpuTime;
}

uint64_t threadInvoluntarySwitches() noexcept {
#if defined(__linux__) && defined(RUSAGE_THREAD)
	rusage ru {};
	if (getrusage(RUSAGE_THREAD, &ru) == 0) {
		return static_cast<uint64_t>(ru.ru_nivcsw);
	}
#endif
	return kNoSwitches;
}

Recorder::~Recorder() {
	if (live_) {
		rack::gCostTreesLive.fetch_sub(1, std::memory_order_relaxed);
	}
}

void Recorder::activate(double sampleRate, double threshold) {
	ring_.reserve(kRingRecords);
	sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
	threshold_ = threshold > 0.0 ? threshold : kDefaultThreshold;
	tree_.clear();
	if (!live_) {
		live_ = true;
		rack::gCostTreesLive.fetch_add(1, std::memory_order_relaxed); // from now on the engines' cost scopes look
	}
	// THE COUNTER'S RATE, a first reading over 2 ms (main thread; refined at every drain).
	calTicks_ = rack::costTicks();
	calNs_ = rack::costClockNs();
	while (rack::costClockNs() - calNs_ < 2'000'000u) {
	}
	calibrate();
}

void Recorder::calibrate() noexcept {
	const uint64_t ticks = rack::costTicks() - calTicks_;
	const uint64_t ns = rack::costClockNs() - calNs_;
	if (ticks > 0 && ns > 0) {
		nsPerTick_.store(static_cast<double>(ns) / static_cast<double>(ticks), std::memory_order_relaxed);
	}
}

BlockStart Recorder::beginBlock() noexcept {
	tree_.clear();
	BlockStart start;
	start.outer = rack::bindCostTree(&tree_);
	start.nivcsw = threadInvoluntarySwitches();
	start.cpuNs = threadCpuNs();
	start.ticks = rack::costTicks(); // last: the CPU clock's own read is not the block's
	return start;
}

namespace {

/// A STEP OF A PATH: `/`, the chain's name and number when it has them (`post/`, `chain2/`), then the tag.
void appendStep(char* out, size_t& at, const rack::CostNode& n, bool first) noexcept {
	const auto put = [&](const char* s) {
		for (; s != nullptr && *s != '\0' && at + 1 < kPathChars; ++s) {
			out[at++] = *s;
		}
	};
	if (!first) {
		put("/");
		if (n.lane != nullptr) {
			put(n.lane);
			if (n.laneIndex >= 0) {
				char digits[8];
				std::snprintf(digits, sizeof digits, "%d", static_cast<int>(n.laneIndex));
				put(digits);
			}
			put("/");
		}
	}
	put(n.tag != nullptr ? n.tag : "?");
	out[at] = '\0';
}

} // namespace

void Recorder::fill(Record& r, double nsPerTick) const noexcept {
	const auto usOf = [&](uint64_t ticks) { return static_cast<float>(static_cast<double>(ticks) * nsPerTick * 1e-3); };
	double devicesUs = 0.0;
	r.timed = static_cast<uint16_t>(tree_.count());
	r.untimed = tree_.lost();
	// THE WORST DEVICES ACROSS THE TREE, by a selection into a small sorted array: kTopDevices compares per node.
	for (size_t i = 0; i < tree_.count(); ++i) {
		const rack::CostNode& c = tree_.node(i);
		const float us = usOf(c.ticks);
		if (c.depth == 0) {
			devicesUs += us;
		}
		if (c.ticks == 0 || c.tag == nullptr) {
			continue;
		}
		size_t at = r.devices;
		while (at > 0 && r.top[at - 1].us < us) {
			--at;
		}
		if (at >= kTopDevices) {
			continue;
		}
		const size_t last = std::min<size_t>(r.devices, kTopDevices - 1);
		for (size_t k = last; k > at; --k) {
			r.top[k] = r.top[k - 1];
		}
		DeviceSpend& d = r.top[at];
		d = DeviceSpend {};
		std::memcpy(d.tag, c.tag, std::min(std::strlen(c.tag), sizeof d.tag - 1));
		d.depth = c.depth;
		d.us = us;
		// THE PATH, outermost first: the chain of parents, gathered innermost first.
		std::array<uint16_t, 16> chain {};
		size_t steps = 0;
		for (uint16_t n = static_cast<uint16_t>(i); n < rack::CostTree::kMaxNodes && steps < chain.size();
		     n = tree_.node(n).parent) {
			chain[steps++] = n;
		}
		size_t chars = 0;
		for (size_t s = 0; s < steps; ++s) {
			const rack::CostNode& step = tree_.node(chain[steps - 1 - s]);
			appendStep(d.path, chars, step, s == 0);
			if (s < kPathDepth) {
				d.slots[s] = step.slot;
			}
		}
		r.devices = static_cast<uint8_t>(std::min<size_t>(r.devices + 1, kTopDevices));
	}
	r.devicesUs = static_cast<float>(devicesUs);
	r.otherUs = r.elapsedUs - r.devicesUs;
	for (size_t s = 0; s < rack::kStageCount; ++s) {
		r.stagesUs[s] = usOf(tree_.stageTicks(s));
	}
}

void Recorder::endBlock(const BlockStart& start, const BlockFacts& facts) noexcept {
	const uint64_t endTicks = rack::costTicks();
	(void)rack::bindCostTree(start.outer);
	const double nsPerTick = nsPerTick_.load(std::memory_order_relaxed);
	const double elapsedUs = static_cast<double>(endTicks - start.ticks) * nsPerTick * 1e-3;
	const double budgetUs = facts.frames > 0 ? static_cast<double>(facts.frames) * 1e6 / sampleRate_ : 0.0;
	const double share = budgetUs > 0.0 ? elapsedUs / budgetUs : 0.0;

	// THE MINUTE'S STATS, every block: one counter, one bin, one compare-and-swap only when the worst moves.
	blocks_.fetch_add(1, std::memory_order_relaxed);
	const auto bin = static_cast<size_t>(std::clamp(share * 100.0, 0.0, static_cast<double>(kShareBins - 1)));
	bins_[bin].fetch_add(1, std::memory_order_relaxed);
	const uint32_t bits = std::bit_cast<uint32_t>(static_cast<float>(share));
	uint32_t worst = worstBits_.load(std::memory_order_relaxed);
	while (bits > worst && !worstBits_.compare_exchange_weak(worst, bits, std::memory_order_relaxed)) {
	}

	if (share >= threshold_ && facts.frames > 0) {
		// THE CPU CLOCK'S SECOND READ, ONLY HERE: a spike is the only block whose CPU time anybody reads.
		const uint64_t cpuEnd = start.cpuNs != kNoCpuTime ? threadCpuNs() : kNoCpuTime;
		const uint64_t switchesEnd = start.nivcsw != kNoSwitches ? threadInvoluntarySwitches() : kNoSwitches;
		Record r;
		r.unixSeconds = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
		r.frames = facts.frames;
		r.sampleRate = static_cast<float>(sampleRate_);
		r.elapsedUs = static_cast<float>(elapsedUs);
		r.share = static_cast<float>(share);
		if (cpuEnd != kNoCpuTime && cpuEnd >= start.cpuNs) {
			const double cpuUs = static_cast<double>(cpuEnd - start.cpuNs) * 1e-3;
			r.cpuUs = static_cast<float>(cpuUs);
			r.preemptedUs = static_cast<float>(std::max(0.0, elapsedUs - cpuUs));
		}
		else {
			r.cpuUs = std::numeric_limits<float>::quiet_NaN();
			r.preemptedUs = std::numeric_limits<float>::quiet_NaN();
		}
		if (switchesEnd != kNoSwitches && switchesEnd >= start.nivcsw) {
			r.nivcsw = static_cast<int32_t>(std::min<uint64_t>(switchesEnd - start.nivcsw, INT32_MAX));
		}
		// PREEMPTED: switched out by the scheduler AND mostly off the CPU. A switch alone is not enough (a 1 ms slice taken
		// from a 10 ms block of real work is still our 9 ms), and low CPU alone is not either (a voluntary wait, a page in).
		r.preempted = r.nivcsw > 0 && std::isfinite(r.cpuUs) && r.cpuUs < kPreemptedCpuShare * elapsedUs;
		r.voices = facts.voices;
		r.songBeats = facts.songBeats;
		r.songSeconds = facts.songSeconds;
		r.flags = facts.flags;
		fill(r, nsPerTick);
		(void)ring_.push(r);
	}
}

namespace {

void appendNumber(std::string& out, double v, int digits) {
	if (!std::isfinite(v)) {
		out += "null";
		return;
	}
	char buffer[64];
	std::snprintf(buffer, sizeof buffer, "%.*g", digits, v);
	out += buffer;
}

void appendFlags(std::string& out, uint32_t flags) {
	static constexpr std::pair<uint32_t, const char*> kNames[] = {
		{ kFlagNotes, "notes" },    { kFlagRebake, "rebake" },       { kFlagLikesFit, "likes-fit" },
		{ kFlagLatency, "latency" }, { kFlagTransport, "transport" }, { kFlagFirst, "first-block" },
	};
	out += '[';
	bool first = true;
	for (const auto& [bit, name] : kNames) {
		if ((flags & bit) != 0) {
			out += first ? "\"" : ",\"";
			out += name;
			out += '"';
			first = false;
		}
	}
	out += ']';
}

} // namespace

std::string lineOf(const Record& r, const char* plugin) {
	std::string out = "{\"v\":" + std::to_string(kFormatVersion) + ",\"event\":\"spike\",\"plugin\":\"";
	out += plugin != nullptr ? plugin : "?";
	out += "\",\"t\":";
	appendNumber(out, r.unixSeconds, 16);
	out += ",\"frames\":" + std::to_string(r.frames) + ",\"rate\":";
	appendNumber(out, r.sampleRate, 9);
	out += ",\"us\":";
	appendNumber(out, r.elapsedUs, 6);
	out += ",\"share\":";
	appendNumber(out, r.share, 4);
	out += ",\"cpu_us\":";
	appendNumber(out, r.cpuUs, 6);
	out += ",\"preempted_us\":";
	appendNumber(out, r.preemptedUs, 6);
	out += ",\"nivcsw\":" + (r.nivcsw >= 0 ? std::to_string(r.nivcsw) : std::string("null"));
	out += r.preempted ? ",\"preempted\":true" : ",\"preempted\":false";
	out += ",\"devices_us\":";
	appendNumber(out, r.devicesUs, 6);
	out += ",\"other_us\":";
	appendNumber(out, r.otherUs, 6);
	// THE STAGES, each its own time; only those that ran. What is left of `other_us` after them is unattributed.
	out += ",\"stages\":{";
	double staged = 0.0;
	bool first = true;
	for (size_t s = 0; s < rack::kStageCount; ++s) {
		if (r.stagesUs[s] == 0.0f) {
			continue;
		}
		out += first ? "\"" : ",\"";
		out += rack::kStageNames[s];
		out += "\":";
		appendNumber(out, r.stagesUs[s], 6);
		staged += r.stagesUs[s];
		first = false;
	}
	out += "},\"unattributed_us\":";
	appendNumber(out, r.otherUs - staged, 6);
	out += ",\"voices\":" + std::to_string(r.voices) + ",\"beats\":";
	appendNumber(out, r.songBeats, 9);
	out += ",\"seconds\":";
	appendNumber(out, r.songSeconds, 9);
	out += ",\"flags\":";
	appendFlags(out, r.flags);
	out += ",\"timed\":" + std::to_string(r.timed);
	if (r.untimed > 0) {
		out += ",\"untimed\":" + std::to_string(r.untimed);
	}
	out += ",\"devices\":[";
	for (size_t i = 0; i < r.devices; ++i) {
		const DeviceSpend& d = r.top[i];
		out += i == 0 ? "{\"tag\":\"" : ",{\"tag\":\"";
		out += d.tag;
		out += "\",\"slot\":" + std::to_string(d.slot()) + ",\"path\":\"";
		out += d.path;
		out += "\",\"slots\":[";
		for (size_t s = 0; s <= d.depth && s < kPathDepth; ++s) {
			out += (s == 0 ? "" : ",") + std::to_string(d.slots[s]);
		}
		out += "],\"us\":";
		appendNumber(out, d.us, 6);
		out += '}';
	}
	out += "]}";
	return out;
}

void Recorder::drain(const char* plugin, double unixNow, std::vector<std::string>* lines) {
	calibrate(); // the counter's rate over everything since activate: better every second
	Record r;
	// THE PER-MINUTE LIMIT: the first `detailPerMinute_` records since the last summary are written as they come; the rest
	// are counted, and the worst of them is held so the minute's worst block still gets its detailed line.
	while (ring_.pop(&r)) {
		if (detailed_ < detailPerMinute_) {
			lines->push_back(lineOf(r, plugin));
			++detailed_;
			detailedWorst_ = std::max(detailedWorst_, r.share);
			continue;
		}
		++suppressed_;
		if (!havePending_ || r.share > pending_.share) {
			if (havePending_) {
				suppressedWorst_ = std::max(suppressedWorst_, pending_.share); // displaced: one of the others now
			}
			pending_ = r;
			havePending_ = true;
		}
		else {
			suppressedWorst_ = std::max(suppressedWorst_, r.share);
		}
	}
	// THE SUMMARY, once a minute (by the wall clock's minute), of every block since the last one.
	const double minute = std::floor(unixNow / 60.0);
	if (minuteStarted_ < 0.0) {
		minuteStarted_ = minute;
		return;
	}
	if (minute <= minuteStarted_) {
		return;
	}
	minuteStarted_ = minute;
	// THE MINUTE'S WORST, IN DETAIL: when the worst record was suppressed, it is written now, just before the summary (its
	// `t` is earlier than the lines before it), and it no longer counts as suppressed.
	// `suppressed_worst` is then the worst share among the records that never got a line: `suppressedWorst_` holds every
	// suppressed share but the pending record's, so it is exact either way.
	uint64_t suppressed = suppressed_;
	float suppressedWorst = suppressedWorst_;
	if (havePending_ && pending_.share > detailedWorst_) {
		lines->push_back(lineOf(pending_, plugin));
		--suppressed;
	}
	else if (havePending_) {
		suppressedWorst = std::max(suppressedWorst, pending_.share);
	}
	detailed_ = 0;
	detailedWorst_ = -1.0f;
	suppressed_ = 0;
	suppressedWorst_ = -1.0f;
	havePending_ = false;
	const uint64_t blocks = blocks_.exchange(0, std::memory_order_relaxed);
	const float worst = std::bit_cast<float>(worstBits_.exchange(0, std::memory_order_relaxed));
	std::array<uint32_t, kShareBins> bins {};
	uint64_t counted = 0;
	for (size_t i = 0; i < kShareBins; ++i) {
		bins[i] = bins_[i].exchange(0, std::memory_order_relaxed);
		counted += bins[i];
	}
	// p99: the first bin whose running count reaches 99% of the blocks; reported as its upper edge.
	double p99 = 0.0;
	uint64_t running = 0;
	for (size_t i = 0; i < kShareBins && counted > 0; ++i) {
		running += bins[i];
		if (static_cast<double>(running) >= 0.99 * static_cast<double>(counted)) {
			p99 = static_cast<double>(i + 1) / 100.0;
			break;
		}
	}
	const uint64_t drops = ring_.drops();
	std::string out = "{\"v\":" + std::to_string(kFormatVersion) + ",\"event\":\"summary\",\"plugin\":\"";
	out += plugin != nullptr ? plugin : "?";
	out += "\",\"t\":";
	appendNumber(out, unixNow, 16);
	out += ",\"blocks\":" + std::to_string(blocks) + ",\"worst\":";
	appendNumber(out, worst, 4);
	out += ",\"p99\":";
	appendNumber(out, p99, 4);
	out += ",\"threshold\":";
	appendNumber(out, threshold_, 4);
	out += ",\"dropped\":" + std::to_string(drops - dropsReported_);
	out += ",\"suppressed\":" + std::to_string(suppressed) + ",\"suppressed_worst\":";
	appendNumber(out, suppressed > 0 ? static_cast<double>(suppressedWorst) : std::numeric_limits<double>::quiet_NaN(), 4);
	out += "}";
	dropsReported_ = drops;
	lines->push_back(std::move(out));
}

std::filesystem::path directory() {
	if (const char* v = std::getenv("FEATHERS_SPIKE_LOG_DIR"); v != nullptr) {
		const std::string s = v;
		if (s.empty() || s == "off") {
			return {};
		}
		return std::filesystem::path(s);
	}
	if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && xdg[0] != '\0') {
		return std::filesystem::path(xdg) / "feathers" / "likes-log";
	}
#if defined(_WIN32)
	if (const char* local = std::getenv("LOCALAPPDATA"); local != nullptr && local[0] != '\0') {
		return std::filesystem::path(local) / "feathers" / "likes-log";
	}
	return {};
#else
	if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
		return std::filesystem::path(home) / ".local" / "share" / "feathers" / "likes-log";
	}
	return {};
#endif
}

std::filesystem::path fileFor(const std::filesystem::path& dir, double unixSeconds) {
	const auto t = static_cast<std::time_t>(unixSeconds);
	std::tm local {};
#if defined(_WIN32)
	localtime_s(&local, &t);
#else
	localtime_r(&t, &local);
#endif
	char name[40];
	std::snprintf(name, sizeof name, "%04d-%02d-%02d.spikes.jsonl", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
	return dir / name;
}

bool append(const std::vector<std::string>& lines, double unixNow) noexcept {
	if (lines.empty()) {
		return true;
	}
	try {
		const std::filesystem::path dir = directory();
		if (dir.empty()) {
			return false;
		}
		std::error_code ec;
		std::filesystem::create_directories(dir, ec);
		return appendCapped(fileFor(dir, unixNow), lines, unixNow, capBytesFromEnvironment());
	}
	catch (...) {
		return false;
	}
}

namespace {

/// A detailed spike line, as `lineOf` writes it: everything else (the summaries, the cap line) is kept past the cap.
bool isSpikeLine(const std::string& line) noexcept {
	const size_t at = line.find("\"event\":\"spike\"");
	return at != std::string::npos && at < 32;
}

} // namespace

bool appendCapped(const std::filesystem::path& file, const std::vector<std::string>& lines, double unixNow,
                  uint64_t capBytes) noexcept {
	if (lines.empty()) {
		return true;
	}
	try {
		std::error_code ec;
		uint64_t size = std::filesystem::file_size(file, ec);
		if (ec) {
			size = 0; // a new day's file
		}
		std::ofstream out(file, std::ios::app);
		if (!out) {
			return false;
		}
		for (const std::string& line : lines) {
			if (size >= capBytes && isSpikeLine(line)) {
				continue;
			}
			const bool under = size < capBytes;
			out << line << '\n';
			size += line.size() + 1;
			if (under && size >= capBytes) {
				// THE CAP LINE, ONCE: only the line that takes the file from under the cap to it writes one. Two processes
				// appending in the same instant could each see themselves cross; that costs a second cap line, nothing more.
				std::string cap = "{\"v\":" + std::to_string(kFormatVersion) + ",\"event\":\"cap\",\"t\":";
				appendNumber(cap, unixNow, 16);
				cap += ",\"cap_bytes\":" + std::to_string(capBytes) +
				       ",\"note\":\"daily size cap reached: no more spike lines in this file; summaries continue\"}";
				out << cap << '\n';
				size += cap.size() + 1;
			}
		}
		return static_cast<bool>(out);
	}
	catch (...) {
		return false;
	}
}

uint64_t capBytesFromEnvironment() {
	if (const char* v = std::getenv("FEATHERS_SPIKE_LOG_CAP_MB"); v != nullptr && v[0] != '\0') {
		char* end = nullptr;
		const double mb = std::strtod(v, &end);
		if (end != v && mb >= 0.0 && mb <= 1e6) {
			return static_cast<uint64_t>(mb * 1024.0 * 1024.0);
		}
	}
	return kDailyCapBytes;
}

double thresholdFromEnvironment(double fallback) {
	if (const char* v = std::getenv("FEATHERS_SPIKE_THRESHOLD"); v != nullptr && v[0] != '\0') {
		char* end = nullptr;
		const double d = std::strtod(v, &end);
		if (end != v && d > 0.0 && d <= 10.0) {
			return d;
		}
	}
	return fallback;
}

} // namespace feathers::spike
