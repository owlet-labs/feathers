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

// THE DELAY WITH A REAL FEEDBACK LANE, PRICED — `bench_delay_modes` prices the DSP with a fake gain lane; this one stands
// the device up through its own ops (the holder, a real `rack::InnerChain`, a Bus Utility inside), so the lane's per-call
// overhead (the drain, the mirror publish, the latency walk, the inner engine's block) is in the number. NOT A TEST.
// Every configuration runs once per round, round-robin, each a fresh instance, so a load swing lands on all alike;
// reported as the median and min over rounds of the per-run mean µs per 1024-frame host block, thread CPU time.
// Bench only at load under 10. Run: `taskset -c 15 build/tests/bench_delay_lane [rounds]`.
//
// **`--render <path>` PRICES THE SOUND INSTEAD** (backlog PV): a fixed 6 s signal (decaying bursts at four levels, so
// the compander moves, then a tail) through four Buckets configurations, written as raw float32 stereo interleaved,
// configuration after configuration. Two builds' files compared sample by sample give the sound change of a cost
// lever as a number (max abs and RMS difference).

#include "fx/busutil/busutil_types.h"
#include "fx/delay/delay.h"
#include "fx/delay/delay_lanes.h"
#include "fx/delay/delay_types.h"
#include "plugin/device_registry.h"
#include "rack/inner_chain.h"
#include "rack/rack_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <new>
#include <string>
#include <vector>

using namespace feathers;
namespace dl = feathers::fx::delay;

namespace {

constexpr double kRate = 48000.0;
constexpr int kBlock = 1024;
constexpr int kBlocks = 48000 * 4 / kBlock;

volatile float g_sink = 0.0f;

double threadSeconds() {
	timespec t {};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return static_cast<double>(t.tv_sec) + 1e-9 * static_cast<double>(t.tv_nsec);
}

enum class Lane { None, Unity, Off };

struct Config {
	const char* name;
	dl::Mode mode;
	Lane lane;
	float compander = -1.0f; ///< < 0: the default
	float noise = -1.0f;
	double timeMultiple = 1.0;
};

/// THE DEVICE STOOD UP AS `usPerBlock` AND `render` BOTH RUN IT.
void* standUp(const Config& c) {
	const device::AudioOps& audio = *dl::kDescriptor.audio;
	const device::ContainerOps& container = *dl::kDescriptor.container;
	void* mem = ::operator new(audio.stateBytes, std::align_val_t { audio.stateAlign });
	audio.construct(mem);
	container.adopt(mem, hostCatalogue());
	audio.prepare(mem, kRate, kBlock);
	dl::Settings s = dl::stateOf(dl::rowOf(dl::State {}), static_cast<size_t>(dl::Param::kCount)).settings;
	s.mode = c.mode;
	s.mix = 0.5f;
	s.feedback = 0.5f;
	if (c.compander >= 0.0f) {
		s.bbdCompander = c.compander;
	}
	if (c.noise >= 0.0f) {
		s.bbdNoise = c.noise;
	}
	s.timeMultiple = c.timeMultiple;
	const dl::Row row = dl::rowOf(dl::State { s });
	audio.setFields(mem, row.data(), row.size());
	audio.setTransportClock(mem, device::TransportClock::fromSecondsPerBeat(0.5));
	if (c.lane != Lane::None) {
		rack::RackBridge& lane = *dl::ensureFeedbackLane(mem)->bridge();
		const DeviceInstanceId util = lane.addDevice(fx::busutil::kDescriptor.traits.tag);
		if (c.lane == Lane::Off) {
			lane.setField(util, static_cast<size_t>(fx::busutil::kDescriptor.traits.enableControl), 0.0f);
		}
	}
	return mem;
}

void tearDown(void* mem) {
	const device::AudioOps& audio = *dl::kDescriptor.audio;
	audio.destruct(mem);
	::operator delete(mem, std::align_val_t { audio.stateAlign });
}

double usPerBlock(const Config& c) {
	const device::AudioOps& audio = *dl::kDescriptor.audio;
	void* mem = standUp(c);
	std::vector<float> l(kBlock);
	std::vector<float> r(kBlock);
	uint32_t seed = 1;
	double total = 0.0;
	for (int b = -20; b < kBlocks; ++b) {
		for (int i = 0; i < kBlock; ++i) {
			seed = seed * 1664525u + 1013904223u;
			l[static_cast<size_t>(i)] = 0.2f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		const double t0 = threadSeconds();
		audio.processStereo(mem, l.data(), r.data(), kBlock);
		const double t1 = threadSeconds();
		if (b >= 0) {
			total += t1 - t0;
		}
		g_sink = g_sink + l[0];
	}
	tearDown(mem);
	return 1e6 * total / static_cast<double>(kBlocks);
}

/// THE FIXED SIGNAL: a 110 Hz saw burst every 0.75 s, decaying over 0.3 s, its peak walking 1.0, 0.3, 0.05, 0.6 so the
/// compander's follower sweeps its range; silence after 4.5 s so the tail (and the noise riding it) is in the file.
float signalAt(int n) {
	const int period = static_cast<int>(0.75 * kRate);
	const int k = n / period;
	if (n >= static_cast<int>(4.5 * kRate)) {
		return 0.0f;
	}
	constexpr float kPeaks[] = { 1.0f, 0.3f, 0.05f, 0.6f };
	const double t = static_cast<double>(n % period) / kRate;
	const double saw = 2.0 * (t * 110.0 - std::floor(t * 110.0)) - 1.0;
	return static_cast<float>(kPeaks[k % 4] * std::exp(-t / 0.1) * saw);
}

int render(const char* path) {
	const Config configs[] = {
		{ "Buckets", dl::Mode::Bbd, Lane::None },
		{ "Buckets loud noise, full compander", dl::Mode::Bbd, Lane::None, 1.0f, 1.0f },
		{ "Buckets + unity lane", dl::Mode::Bbd, Lane::Unity, 1.0f, 1.0f },
		{ "Buckets + unity lane, short", dl::Mode::Bbd, Lane::Unity, 1.0f, 1.0f, 1.0 / 16.0 },
	};
	FILE* f = std::fopen(path, "wb");
	if (f == nullptr) {
		return 1;
	}
	const device::AudioOps& audio = *dl::kDescriptor.audio;
	constexpr int kFrames = 6 * 48000;
	for (const Config& c : configs) {
		void* mem = standUp(c);
		std::vector<float> l(kBlock);
		std::vector<float> r(kBlock);
		std::vector<float> out(2 * kBlock);
		for (int at = 0; at < kFrames; at += kBlock) {
			for (int i = 0; i < kBlock; ++i) {
				l[static_cast<size_t>(i)] = signalAt(at + i);
				r[static_cast<size_t>(i)] = 0.5f * l[static_cast<size_t>(i)];
			}
			audio.processStereo(mem, l.data(), r.data(), kBlock);
			for (int i = 0; i < kBlock; ++i) {
				out[static_cast<size_t>(2 * i)] = l[static_cast<size_t>(i)];
				out[static_cast<size_t>(2 * i + 1)] = r[static_cast<size_t>(i)];
			}
			std::fwrite(out.data(), sizeof(float), out.size(), f);
		}
		tearDown(mem);
	}
	std::fclose(f);
	return 0;
}

} // namespace

int main(int argc, char** argv) {
	if (argc > 2 && std::string(argv[1]) == "--render") {
		return render(argv[2]);
	}
	const int rounds = argc > 1 ? std::max(3, std::atoi(argv[1])) : 9;
	const Config configs[] = {
		{ "Clean", dl::Mode::Clean, Lane::None },     { "Clean + unity lane", dl::Mode::Clean, Lane::Unity },
		{ "Clean + off lane", dl::Mode::Clean, Lane::Off }, { "Buckets", dl::Mode::Bbd, Lane::None },
		{ "Buckets + unity lane", dl::Mode::Bbd, Lane::Unity }, { "Buckets + off lane", dl::Mode::Bbd, Lane::Off },
	};
	constexpr size_t kConfigs = std::size(configs);
	std::vector<double> us[kConfigs];
	for (int round = 0; round < rounds; ++round) {
		for (size_t k = 0; k < kConfigs; ++k) {
			const size_t at = (k + static_cast<size_t>(round)) % kConfigs;
			us[at].push_back(usPerBlock(configs[at]));
		}
	}
	double load = 0.0;
	if (FILE* f = std::fopen("/proc/loadavg", "r"); f != nullptr) {
		if (std::fscanf(f, "%lf", &load) != 1) {
			load = -1.0;
		}
		std::fclose(f);
	}
	std::printf("DELAY + LANE COST (us thread CPU per %d-frame block; median / min of %d rounds; load %.1f)\n", kBlock,
	            rounds, load);
	for (size_t k = 0; k < kConfigs; ++k) {
		std::sort(us[k].begin(), us[k].end());
		std::printf("  %-24s %8.1f  (min %8.1f)\n", configs[k].name, us[k][us[k].size() / 2], us[k][0]);
	}
	return 0;
}
