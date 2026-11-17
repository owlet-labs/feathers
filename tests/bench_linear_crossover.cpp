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

// THE LINEAR CROSSOVER'S COST: OWLTT's Mode 11 against LR4 Full, and a Bands rack on the Linear slope against LR4, per stereo
// sample frame at 48 kHz in 256-sample blocks. Static corners, then corners moving every block (a sweep that keeps the kernels
// rebuilding and swapping, the worst case). NOT A TEST: it asserts nothing and ctest never runs it (bench_render's posture).
//
// The rows ALTERNATE inside each repetition and the repetitions rotate the order, because this is a shared host whose
// load swings (memory: benchmarking on a shared host): take the medians, and quote the load beside them.

#include "composite/composite_types.h"
#include "fx/owltt/owltt.h"
#include "fx/owltt/owltt_types.h"
#include "plugin/device_registry.h"
#include "rack/parallel_rack.h"
#include "rack/rack_device.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <vector>

namespace {

using namespace feathers;
namespace pr = feathers::rack::parallel;
namespace ow = feathers::fx::owltt;

volatile float g_sink = 0.0f;
constexpr int kBlock = 256;
constexpr int kBlocks = 48000 * 4 / kBlock;

std::vector<float> program() {
	std::vector<float> x(static_cast<size_t>(kBlock));
	uint32_t s = 12345u;
	for (size_t i = 0; i < x.size(); ++i) {
		s = s * 1664525u + 1013904223u;
		x[i] = 0.3f * (static_cast<float>(s >> 8) / 8388608.0f - 1.0f) + 0.2f * static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
	}
	return x;
}

double owlttNs(int mode, bool moving) {
	ow::Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(ow::kParamDescs[i].def);
	}
	ow::Settings s = ow::stateOf(row);
	s.mode = static_cast<float>(mode);
	ow::Owltt o;
	o.prepare(48000.0);
	o.setTarget(s);
	const std::vector<float> in = program();
	std::vector<float> l = in, r = in;
	const auto t0 = std::chrono::steady_clock::now();
	for (int b = 0; b < kBlocks; ++b) {
		if (moving) {
			const double t = 0.5 - 0.5 * std::cos(b * 0.05);
			s.lowCrossover = static_cast<float>(100.0 + 500.0 * t);
			s.highCrossover = static_cast<float>(1500.0 + 1500.0 * t);
			o.setTarget(s);
		}
		l = in;
		r = in;
		o.processStereo(l.data(), r.data(), kBlock);
	}
	const auto t1 = std::chrono::steady_clock::now();
	g_sink = l[7];
	return std::chrono::duration<double, std::nano>(t1 - t0).count() / (static_cast<double>(kBlocks) * kBlock);
}

double rackNs(pr::Slope slope, pr::Filtering filtering, int bandChoice, bool moving) {
	device::Catalogue catalogue { nullptr, [](void*, const char* tag) -> const device::Descriptor* { return deviceForTag(tag); } };
	void* mem = ::operator new(pr::kAudioOps.stateBytes, std::align_val_t { pr::kAudioOps.stateAlign });
	pr::kAudioOps.construct(mem);
	pr::kContainerOps.adopt(mem, catalogue);
	pr::kAudioOps.prepare(mem, 48000.0, 4096);
	std::vector<float> fields(static_cast<size_t>(pr::Param::kCount));
	for (size_t i = 0; i < fields.size(); ++i) {
		fields[i] = static_cast<float>(pr::kParamDescs[i].def);
	}
	const int bands = pr::kBandCounts[bandChoice];
	for (int k = 1; k < bands; ++k) {
		pr::rackOf(mem)->addChain();
	}
	fields[static_cast<size_t>(pr::Param::Mode)] = static_cast<float>(pr::Mode::Bands);
	fields[static_cast<size_t>(pr::Param::BandCount)] = static_cast<float>(bandChoice);
	fields[static_cast<size_t>(pr::Param::Slope)] = static_cast<float>(slope);
	fields[static_cast<size_t>(pr::Param::Filtering)] = static_cast<float>(filtering);
	const std::vector<float> in = program();
	std::vector<float> l = in, r = in;
	const auto step = [&](int b) {
		if (moving) {
			const double t = 0.5 - 0.5 * std::cos(b * 0.05);
			fields[static_cast<size_t>(pr::Param::Cut1)] = static_cast<float>(std::log2(100.0 + 500.0 * t));
			fields[static_cast<size_t>(pr::Param::Cut2)] = static_cast<float>(std::log2(1000.0 + 600.0 * t));
		}
		l = in;
		r = in;
		pr::kAudioOps.setFields(mem, fields.data(), fields.size());
		pr::kAudioOps.processStereo(mem, l.data(), r.data(), kBlock);
	};
	for (int b = 0; b < 8; ++b) {
		step(b); // the structure lands
	}
	const auto t0 = std::chrono::steady_clock::now();
	for (int b = 0; b < kBlocks; ++b) {
		step(b);
	}
	const auto t1 = std::chrono::steady_clock::now();
	g_sink = l[7];
	pr::kAudioOps.destruct(mem);
	::operator delete(mem, std::align_val_t { pr::kAudioOps.stateAlign });
	return std::chrono::duration<double, std::nano>(t1 - t0).count() / (static_cast<double>(kBlocks) * kBlock);
}

struct Row {
	std::string name;
	std::function<double()> run;
	std::vector<double> ns;
};

} // namespace

int main() {
	std::vector<Row> rows;
	rows.push_back({ "OWLTT LR4 Full, static", [] { return owlttNs(8, false); }, {} });
	rows.push_back({ "OWLTT Linear, static", [] { return owlttNs(11, false); }, {} });
	rows.push_back({ "OWLTT LR4 Full, corners moving", [] { return owlttNs(8, true); }, {} });
	rows.push_back({ "OWLTT Linear, corners moving", [] { return owlttNs(11, true); }, {} });
	for (const int choice : { 1, 2 }) {
		const std::string n = std::to_string(pr::kBandCounts[choice]) + "-band rack, ";
		rows.push_back({ n + "LR4 Before, static", [choice] { return rackNs(pr::Slope::Lr4, pr::Filtering::Before, choice, false); }, {} });
		rows.push_back({ n + "Linear Before, static", [choice] { return rackNs(pr::Slope::Linear, pr::Filtering::Before, choice, false); }, {} });
		rows.push_back({ n + "Linear After, static", [choice] { return rackNs(pr::Slope::Linear, pr::Filtering::After, choice, false); }, {} });
		rows.push_back({ n + "Linear Both, static", [choice] { return rackNs(pr::Slope::Linear, pr::Filtering::Both, choice, false); }, {} });
		rows.push_back({ n + "LR4 Before, corners moving", [choice] { return rackNs(pr::Slope::Lr4, pr::Filtering::Before, choice, true); }, {} });
		rows.push_back({ n + "Linear Before, corners moving", [choice] { return rackNs(pr::Slope::Linear, pr::Filtering::Before, choice, true); }, {} });
	}
	constexpr int kReps = 7;
	for (int rep = 0; rep < kReps; ++rep) {
		for (size_t i = 0; i < rows.size(); ++i) {
			Row& row = rows[(i + static_cast<size_t>(rep) * 3) % rows.size()];
			row.ns.push_back(row.run());
		}
	}
	for (Row& row : rows) {
		std::sort(row.ns.begin(), row.ns.end());
		std::printf("%-44s median %8.1f ns/frame   min %8.1f   max %8.1f\n", row.name.c_str(), row.ns[row.ns.size() / 2], row.ns.front(), row.ns.back());
	}
	return 0;
}
