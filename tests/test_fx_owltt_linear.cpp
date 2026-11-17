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

#include "dsp/linear_crossover.h"
#include "fx/owltt/owltt.h"
#include "fx/owltt/owltt_types.h"
#include "portable_aligned_alloc.h"
#include "test_heap.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <new>
#include <numbers>
#include <string>
#include <vector>

// THE ALLOCATION PROBE, counting only while armed (see test_linear_crossover.cpp for what it can and cannot see).
namespace {
struct Allocs {
	bool armed = false;
	size_t allocations = 0;
};
Allocs allocs;
void* countedAlloc(size_t bytes, size_t align) {
	if (allocs.armed) {
		++allocs.allocations;
	}
	void* p = feathers::test::portableAlignedAlloc(bytes, align);
	if (p == nullptr) {
		throw std::bad_alloc {};
	}
	return p;
}
} // namespace
void* operator new(size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new[](size_t bytes) { return countedAlloc(bytes, alignof(std::max_align_t)); }
void* operator new(size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void* operator new[](size_t bytes, std::align_val_t align) { return countedAlloc(bytes, static_cast<size_t>(align)); }
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, size_t, std::align_val_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"

using namespace feathers::fx::owltt;

namespace {

constexpr double kRate = 48000.0;

Settings patchDefault() {
	Row row {};
	for (size_t i = 0; i < row.size(); ++i) {
		row[i] = static_cast<float>(kParamDescs[i].def);
	}
	return stateOf(row);
}

/// Linear at the patch default with the dynamics at rest (Ratio 0, no clipper): the device is then linear and the bands, unprocessed.
Settings linearAtRest() {
	Settings s = patchDefault();
	s.mode = 11.0f;
	s.ratio = 0.0f;
	s.softClip = false;
	s.blend = 1.0f;
	return s;
}

struct Noise {
	uint32_t s;
	float next() {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(static_cast<double>(s) / 2147483647.5 - 1.0);
	}
};

/// `in` through a fresh device, `chunk` frames a call; the stereo input is (in, 0.7 in) so the channels differ.
void run(const Settings& s, std::vector<float>& l, std::vector<float>& r, int chunk = 256, double rate = kRate) {
	Owltt o;
	o.prepare(rate);
	o.setTarget(s);
	for (size_t at = 0; at < l.size(); at += static_cast<size_t>(chunk)) {
		const int n = static_cast<int>(std::min(l.size() - at, static_cast<size_t>(chunk)));
		o.processStereo(&l[at], &r[at], n);
	}
}

/// The restated latency: kernel centre plus block, from the stated sizes (2048 / 4096 / 8192 taps, blocks of a sixteenth).
uint32_t statedLatency(double rate) {
	const uint32_t taps = rate > 100000.0 ? 8192 : rate > 50000.0 ? 4096 : 2048;
	return taps / 2 + taps / 16;
}

double rmsOf(const std::vector<float>& v, size_t from) {
	double sq = 0.0;
	for (size_t i = from; i < v.size(); ++i) {
		sq += static_cast<double>(v[i]) * v[i];
	}
	return std::sqrt(sq / static_cast<double>(v.size() - from));
}

} // namespace

TEST_CASE("Linear: Mode offers it as the 11th choice, after the ten that keep their numbers") {
	const auto& d = kParamDescs[static_cast<size_t>(Param::Mode)];
	CHECK(d.max == 14.0);
	CHECK(d.min == 1.0);
	REQUIRE(std::size(kModeNames) == 14);
	CHECK(std::string(kModeNames[10]) == "Linear (94 Hz)");
	CHECK(std::string(kModeNames[9]) == "Twist3"); // the ten before it keep their numbers
	CHECK(d.def == 6.0);
}

TEST_CASE("Linear: the reported latency is the measured impulse delay, and moves with the lookahead's only by addition") {
	REQUIRE(kAudioOps.latencySamples != nullptr);
	for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
		CAPTURE(rate);
		Settings s = linearAtRest();
		Row row = rowOf(s);
		CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate) == statedLatency(rate));
		s.mode = 6.0f;
		row = rowOf(s);
		CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate) == 0u); // the other modes add none
		s.mode = 11.0f;
		s.lookahead = 5; // 5 ms
		row = rowOf(s);
		CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), rate)
		      == statedLatency(rate) + static_cast<uint32_t>(std::lround(0.005 * rate)));
	}
	// THE IMPULSE'S DELAY through the real device, at two rates, Blend 1 and Blend 0 (the dry): both land on the reported sample.
	for (const double rate : { 48000.0, 96000.0 }) {
		for (const float blend : { 1.0f, 0.0f, 0.5f }) {
			CAPTURE(rate);
			CAPTURE(blend);
			Settings s = linearAtRest();
			s.blend = blend;
			std::vector<float> l(8192, 0.0f), r(8192, 0.0f);
			l[0] = r[0] = 0.5f;
			run(s, l, r, 256, rate);
			size_t peakAt = 0;
			for (size_t i = 0; i < l.size(); ++i) {
				if (std::fabs(l[i]) > std::fabs(l[peakAt])) {
					peakAt = i;
				}
			}
			CHECK(peakAt == statedLatency(rate));
			CHECK(std::fabs(l[peakAt] - 0.5f) < 1.0e-3f);
		}
	}
}

TEST_CASE("Linear: unprocessed bands are flat to 0.05 dB from 20 Hz to 20 kHz at Blend 1 and at Blend 50%, and the 50% blend is still in time") {
	for (const double hz : { 20.0, 40.0, 100.0, 200.0, 440.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 }) {
		CAPTURE(hz);
		std::vector<float> in(48000), a, b;
		for (size_t i = 0; i < in.size(); ++i) {
			in[i] = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
		}
		const double want = rmsOf(in, 24000);
		for (const float blend : { 1.0f, 0.5f }) {
			Settings s = linearAtRest();
			s.blend = blend;
			a = in;
			b = in;
			run(s, a, b);
			CHECK(std::fabs(20.0 * std::log10(rmsOf(a, 24000) / want)) < 0.05);
			// IN TIME: the output is the input delayed by exactly the latency, so the error against it is at the float's floor.
			double worst = 0.0;
			const size_t lat = statedLatency(kRate);
			for (size_t i = 2 * lat; i < a.size(); ++i) {
				worst = std::max(worst, static_cast<double>(std::fabs(a[i] - in[i - lat])));
			}
			CHECK(worst < 1.0e-4);
		}
	}
}

TEST_CASE("Linear: bypassed, the device still delays by the reported latency, to the bit, with and without a lookahead") {
	for (const uint32_t look : { 0u, 5u }) {
		CAPTURE(look);
		Settings s = linearAtRest();
		s.enabled = false;
		s.lookahead = look;
		std::vector<float> in(6000), l, r;
		Noise n { 0x243f6a88u };
		for (float& v : in) {
			v = 0.4f * n.next();
		}
		l = in;
		r = in;
		run(s, l, r);
		const size_t lat = statedLatency(kRate) + static_cast<size_t>(std::lround(0.001 * look * kRate));
		size_t same = 0, head = 0;
		for (size_t i = 0; i < in.size(); ++i) {
			if (i < lat) {
				head += l[i] == 0.0f;
			}
			else {
				same += l[i] == in[i - lat];
			}
		}
		CHECK(head == lat);
		CHECK(same == in.size() - lat);
	}
}

TEST_CASE("Linear: crossovers that move (knob sweep and Key Track) allocate nothing, rebuild and swap, and the unprocessed sum stays exactly the delayed input") {
	Settings s = linearAtRest();
	s.keyTrack = true;
	Owltt o;
	o.prepare(kRate);
	o.setTarget(s);
	const size_t lat = statedLatency(kRate);
	const size_t total = 96000;
	std::vector<float> in(total), out;
	Noise n { 77u };
	for (size_t i = 0; i < total; ++i) {
		in[i] = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * 3000.0 * static_cast<double>(i) / kRate)) + 0.1f * n.next();
	}
	out = in;
	std::vector<float> r = in;
	allocs.allocations = 0;
	allocs.armed = true;
	for (size_t at = 0, block = 0; at < total; at += 256, ++block) {
		// The knobs sweep and a different note is held every 20 blocks.
		s.lowCrossover = static_cast<float>(100.0 + 600.0 * (0.5 - 0.5 * std::cos(static_cast<double>(block) * 0.05)));
		s.highCrossover = static_cast<float>(1500.0 + 1500.0 * (0.5 - 0.5 * std::cos(static_cast<double>(block) * 0.03)));
		o.setTarget(s);
		feathers::device::NoteContext note;
		note.gate = true;
		note.pitchSemitones = static_cast<float>(48 + (block / 20) % 12);
		o.setNoteContext(note);
		o.processStereo(&out[at], &r[at], 256);
	}
	allocs.armed = false;
	CHECK(allocs.allocations == 0);
	CHECK(o.linearForTest().swapsForTest() > 20); // not vacuous: kernels really were rebuilt and swapped
	double worst = 0.0;
	for (size_t i = 2 * lat; i < total; ++i) {
		worst = std::max(worst, static_cast<double>(std::fabs(out[i] - in[i - lat])));
	}
	CHECK(worst < 1.0e-4); // no click: the bands' sum is the delayed input through every swap
}

TEST_CASE("Linear: after the input stops the output reaches exact zero within the kernel's tail, at the patch default and at rest, Blend 1 and 50%") {
	for (const bool atRest : { true, false }) {
		for (const float blend : { 1.0f, 0.5f }) {
			CAPTURE(atRest);
			CAPTURE(blend);
			Settings s = atRest ? linearAtRest() : patchDefault();
			s.mode = 11.0f;
			s.blend = blend;
			const size_t speech = 24000;
			std::vector<float> l(speech + 24000, 0.0f), r = l;
			Noise n { 5u };
			for (size_t i = 0; i < speech; ++i) {
				l[i] = 0.5f * n.next();
				r[i] = 0.5f * n.next();
			}
			run(s, l, r);
			// The tail: the latency, then the kernel's length and the convolution's block-and-partition wait.
			const size_t tail = speech + statedLatency(kRate) + 2048 + 5 * 128;
			size_t nonzero = 0;
			for (size_t i = tail; i < l.size(); ++i) {
				nonzero += l[i] != 0.0f || r[i] != 0.0f;
			}
			CHECK(nonzero == 0u);
			// NOT VACUOUS: the tail before it does ring.
			size_t ringing = 0;
			for (size_t i = speech + 64; i < speech + statedLatency(kRate); ++i) {
				ringing += l[i] != 0.0f;
			}
			CHECK(ringing > 100u);
		}
	}
}

TEST_CASE("Linear steepness: Linear, Linear Mild and Linear Gentle report one latency, land the impulse on it, and sum flat") {
	REQUIRE(std::size(kModeNames) == 14);
	CHECK(std::string(kModeNames[11]) == "Linear Mild (186 Hz)");
	CHECK(std::string(kModeNames[12]) == "Linear Gentle (368 Hz)");
	CHECK(kParamDescs[static_cast<size_t>(Param::Mode)].max == 14.0);
	for (const float mode : { 11.0f, 12.0f, 13.0f }) {
		CAPTURE(mode);
		Settings s = linearAtRest();
		s.mode = mode;
		const Row row = rowOf(s);
		CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), kRate) == statedLatency(kRate)); // THE SAME FOR ALL THREE
		std::vector<float> l(4096, 0.0f), r(4096, 0.0f);
		l[0] = r[0] = 0.5f;
		run(s, l, r);
		size_t peakAt = 0;
		for (size_t i = 0; i < l.size(); ++i) {
			if (std::fabs(l[i]) > std::fabs(l[peakAt])) {
				peakAt = i;
			}
		}
		CHECK(peakAt == statedLatency(kRate));
		// FLAT: noise through the unprocessed bands is the input, late by the latency, to the float.
		std::vector<float> in(12000), a, b;
		Noise n { 31u };
		for (float& v : in) {
			v = 0.4f * n.next();
		}
		a = in;
		b = in;
		run(s, a, b);
		double worst = 0.0;
		const size_t lat = statedLatency(kRate);
		for (size_t i = 2 * lat; i < in.size(); ++i) {
			worst = std::max(worst, static_cast<double>(std::fabs(a[i] - in[i - lat])));
		}
		CHECK(worst < 1.0e-4);
	}
}

//------------------------------------------------------------------------------------------------//
// LR8 FULL (Mode 14): 48 dB/oct, an IIR, no latency                                              //
//------------------------------------------------------------------------------------------------//

namespace {

/// The RMS gain of the low band of an LR8 Full crossover (corners `lo`, `hi`) for a unit sine at `hz`.
double lr8LowGain(double lo, double hi, double hz) {
	feathers::fx::owltt::LR8CrossoverFull x;
	x.setLowCrossover(static_cast<float>(lo), 48000.0f);
	x.setHighCrossover(static_cast<float>(hi), 48000.0f);
	double sq = 0.0, in = 0.0;
	for (int i = 0; i < 48000; ++i) {
		const auto v = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * i / 48000.0));
		feathers::fx::owltt::CrossoverBands b {};
		x.process(v, b);
		if (i >= 24000) {
			sq += static_cast<double>(b.low) * b.low;
			in += static_cast<double>(v) * v;
		}
	}
	return std::sqrt(sq / in);
}

} // namespace

TEST_CASE("LR8 Full: Mode 14 is the last choice, the ten before keep their numbers, and it adds no latency") {
	REQUIRE(std::size(kModeNames) == 14);
	CHECK(std::string(kModeNames[13]) == "LR8 Full (48 dB/oct)");
	Settings s = linearAtRest();
	s.mode = 14.0f;
	const Row row = rowOf(s);
	CHECK(kAudioOps.latencySamples(nullptr, row.data(), row.size(), kRate) == 0u);
}

TEST_CASE("LR8 Full: the low band falls 48 dB an octave past the corner, as the Butterworth-4 squared says") {
	// RESTATED from the prototype: |LP| = 1 / (1 + w^8), w = tan(pi f / fs) / tan(pi fc / fs) (the bilinear warp).
	const auto expected = [](double hz, double fc) {
		const double w = std::tan(std::numbers::pi * hz / kRate) / std::tan(std::numbers::pi * fc / kRate);
		return 20.0 * std::log10(1.0 / (1.0 + std::pow(w, 8.0)));
	};
	const double a = 20.0 * std::log10(lr8LowGain(1000.0, 8000.0, 2000.0));
	const double b = 20.0 * std::log10(lr8LowGain(1000.0, 8000.0, 4000.0));
	CHECK(std::fabs(a - expected(2000.0, 1000.0)) < 0.3);
	CHECK(std::fabs(b - expected(4000.0, 1000.0)) < 0.5);
	CHECK(std::fabs((a - b) - 48.0) < 2.0); // the slope an octave out
	// And the corner is -6 dB, as every LR is.
	CHECK(std::fabs(20.0 * std::log10(lr8LowGain(1000.0, 8000.0, 1000.0)) + 6.02) < 0.2);
}

TEST_CASE("LR8 Full: the bands sum flat (an allpass) within 0.1 dB through the device, Blend 1 and 50%, 20 Hz to 20 kHz") {
	for (const double hz : { 20.0, 100.0, 200.0, 440.0, 1000.0, 2000.0, 5000.0, 12000.0, 20000.0 }) {
		CAPTURE(hz);
		std::vector<float> in(48000), a, b;
		for (size_t i = 0; i < in.size(); ++i) {
			in[i] = 0.3f * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
		}
		const double want = rmsOf(in, 24000);
		for (const float blend : { 1.0f, 0.5f }) {
			Settings s = linearAtRest();
			s.mode = 14.0f;
			s.blend = blend;
			a = in;
			b = in;
			run(s, a, b);
			CHECK(std::fabs(20.0 * std::log10(rmsOf(a, 24000) / want)) < 0.1);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// THE SLOPES IN THE LABELS                                                                        //
//------------------------------------------------------------------------------------------------//

namespace {

/// The slope a label states, "(N dB/oct)": N, or 0 when it states none.
double statedSlope(const char* label) {
	const std::string s = label;
	const auto open = s.find('(');
	if (open == std::string::npos || s.find(" dB/oct)") == std::string::npos) {
		return 0.0;
	}
	return std::stod(s.substr(open + 1));
}

/// The low band's measured slope, dB/oct, from `hz` and `2 hz` (both in the stopband, above float's noise floor).
template <class Crossover>
double measuredSlope(double fc, double hz) {
	const auto gain = [&](double f) {
		Crossover x;
		x.setLowCrossover(static_cast<float>(fc), 48000.0f);
		x.setHighCrossover(15000.0f, 48000.0f);
		double sq = 0.0, in = 0.0;
		for (int i = 0; i < 48000; ++i) {
			const auto v = static_cast<float>(std::sin(2.0 * std::numbers::pi * f * i / 48000.0));
			feathers::fx::owltt::CrossoverBands b {};
			x.process(v, b);
			if (i >= 24000) {
				sq += static_cast<double>(b.low) * b.low;
				in += static_cast<double>(v) * v;
			}
		}
		return 20.0 * std::log10(std::sqrt(sq / in));
	};
	return gain(hz) - gain(2.0 * hz);
}

} // namespace

TEST_CASE("Mode labels: every slope a label states is the slope measured an octave past the corner, within 2 dB/oct") {
	using namespace feathers::fx::owltt;
	// The five Linkwitz-Riley modes, by index (value - 1), each measured where the stopband is above the noise floor: 4 fc to 8 fc for the
	// 12 and 24 dB pairs, 2 fc to 4 fc for the 48 dB pair (past that it is under -96 dB, near float's own floor).
	struct Row { size_t index; double (*measure)(); };
	const auto lr2fast = [] { return measuredSlope<LR2CrossoverFast>(500.0, 2000.0); };
	const auto lr2full = [] { return measuredSlope<LR2CrossoverFull>(500.0, 2000.0); };
	const auto lr4fast = [] { return measuredSlope<LR4CrossoverFast>(500.0, 2000.0); };
	const auto lr4full = [] { return measuredSlope<LR4CrossoverFull>(500.0, 2000.0); };
	const auto lr8full = [] { return measuredSlope<LR8CrossoverFull>(500.0, 1000.0); };
	const std::array<std::pair<size_t, double>, 5> measured {{ { 4, lr2fast() }, { 5, lr2full() }, { 6, lr4fast() }, { 7, lr4full() }, { 13, lr8full() } }};
	for (const auto& [index, slope] : measured) {
		CAPTURE(index);
		CAPTURE(kModeNames[index]);
		CHECK(statedSlope(kModeNames[index]) > 0.0);
		CHECK(std::fabs(slope - statedSlope(kModeNames[index])) < 2.0);
	}
	// AND NO OTHER LABEL STATES A dB/oct (the linear ones state a width in Hz, the creative allpass splits nothing).
	for (size_t i = 0; i < std::size(kModeNames); ++i) {
		const bool lr = i == 4 || i == 5 || i == 6 || i == 7 || i == 13;
		CHECK((statedSlope(kModeNames[i]) > 0.0) == lr);
	}
}

TEST_CASE("Mode labels: a label stored before the slopes were added still names its mode") {
	using namespace feathers::fx::owltt;
	const auto& d = kParamDescs[static_cast<size_t>(Param::Mode)];
	const std::pair<const char*, size_t> old[] = { { "LR2 Fast", 4 }, { "LR2 Full", 5 }, { "LR4 Fast", 6 }, { "LR4 Full", 7 },
		                                           { "Linear", 10 }, { "Linear Mild", 11 }, { "Linear Gentle", 12 }, { "LR8 Full", 13 } };
	for (const auto& [label, index] : old) {
		CAPTURE(label);
		bool found = false;
		for (const auto& a : d.choiceAliases) {
			if (std::string(a.from) == label) {
				found = true;
				CHECK(std::string(a.to) == kModeNames[index]); // lands on the same index, so the same value
			}
		}
		CHECK(found);
	}
}
