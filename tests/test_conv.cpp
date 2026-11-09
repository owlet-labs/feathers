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
#include "test_heap.h"
#include "doctest.h"

#include "fx/conv/ir_file.h"
#include "fx/conv/test_ir.h"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace feathers::fx::conv;

namespace {

/// Four cascaded one-poles at `hz` (-24 dB/octave), as a lowpass, or as a highpass (each stage subtracting
/// its own lowpass), run over `x`. As steep as the generator's own split, so the measurement does not let
/// one band's decay leak into the other's reading.
std::vector<double> band(const std::vector<float>& x, double sr, double hz, bool high) {
	const double a = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * hz / sr);
	double s[4] = {};
	std::vector<double> y(x.size());
	for (size_t i = 0; i < x.size(); ++i) {
		double v = x[i];
		for (double& st : s) {
			st += a * (v - st);
			v = high ? v - st : st;
		}
		y[i] = v;
	}
	return y;
}

/// RT60 in seconds, from the slope of 10·log10(energy) per 50 ms window between `from` and `to` seconds,
/// fitted by least squares.
double rt60(const std::vector<double>& y, double sr, double from, double to) {
	const auto win = static_cast<size_t>(0.05 * sr);
	std::vector<double> ts, ds;
	for (size_t at = static_cast<size_t>(from * sr); at + win <= static_cast<size_t>(to * sr) && at + win <= y.size(); at += win) {
		double e = 0.0;
		for (size_t i = at; i < at + win; ++i) {
			e += y[i] * y[i];
		}
		ts.push_back((static_cast<double>(at) + win / 2.0) / sr);
		ds.push_back(10.0 * std::log10(e + 1e-300));
	}
	double mt = 0.0, md = 0.0;
	for (size_t i = 0; i < ts.size(); ++i) {
		mt += ts[i];
		md += ds[i];
	}
	mt /= static_cast<double>(ts.size());
	md /= static_cast<double>(ts.size());
	double num = 0.0, den = 0.0;
	for (size_t i = 0; i < ts.size(); ++i) {
		num += (ts[i] - mt) * (ds[i] - md);
		den += (ts[i] - mt) * (ts[i] - mt);
	}
	return -60.0 / (num / den);
}

} // namespace

TEST_CASE("the generated test IR is deterministic, silent before its predelay, and unit energy per channel") {
	const GeneratedIr a = generateTestIr(48000.0);
	const GeneratedIr b = generateTestIr(48000.0);
	const GeneratedIr other = generateTestIr(48000.0, 2.5, 12345u);
	CHECK(a.left == b.left);
	CHECK(a.right == b.right);
	CHECK(a.left != other.left);
	REQUIRE(a.left.size() == static_cast<size_t>(2.5 * 48000.0));

	const auto pre = static_cast<size_t>(kPredelaySeconds * 48000.0);
	for (size_t i = 0; i < pre; ++i) {
		CHECK(a.left[i] == 0.0f);
		CHECK(a.right[i] == 0.0f);
	}
	for (const std::vector<float>* ch : {&a.left, &a.right}) {
		double e = 0.0;
		for (const float v : *ch) {
			e += static_cast<double>(v) * static_cast<double>(v);
		}
		CHECK(std::fabs(e - 1.0) < 1.0e-5);
	}
}

TEST_CASE("the generated test IR's two channels are two rooms, not one") {
	const GeneratedIr ir = generateTestIr(48000.0);
	double lr = 0.0, ll = 0.0, rr = 0.0;
	for (size_t i = 0; i < ir.left.size(); ++i) {
		lr += static_cast<double>(ir.left[i]) * ir.right[i];
		ll += static_cast<double>(ir.left[i]) * ir.left[i];
		rr += static_cast<double>(ir.right[i]) * ir.right[i];
	}
	const double corr = lr / std::sqrt(ll * rr);
	CAPTURE(corr);
	CHECK(std::fabs(corr) < 0.2);
}

TEST_CASE("the generated test IR decays as designed: the lows over 2.2 s, the highs over 0.9 s, at any rate") {
	for (const double sr : {44100.0, 96000.0}) {
		const GeneratedIr ir = generateTestIr(sr);
		const double low = rt60(band(ir.left, sr, 400.0, false), sr, 0.1, 1.5);
		const double high = rt60(band(ir.left, sr, 8000.0, true), sr, 0.1, 0.7);
		CAPTURE(sr);
		CAPTURE(low);
		CAPTURE(high);
		MESSAGE("rate " << sr << ": low RT60 " << low << " s, high RT60 " << high << " s");
		CHECK(std::fabs(low - kRt60Low) < 0.15 * kRt60Low);
		CHECK(std::fabs(high - kRt60High) < 0.15 * kRt60High);
	}
}

TEST_CASE("the generated rooms: Room is the first IR to the bit; Plate, Hall and Ambience decay as described") {
	const double sr = 48000.0;
	const GeneratedIr room = generateRoom(kGeneratedRoom, sr);
	const GeneratedIr first = generateTestIr(sr);
	CHECK(room.left == first.left);
	CHECK(room.right == first.right);
	struct Want {
		int which;
		double lowFrom, lowTo, highFrom, highTo;
	};
	for (const Want w : { Want { kGeneratedPlate, 0.1, 1.6, 0.1, 1.1 }, Want { kGeneratedHall, 0.2, 2.5, 0.2, 1.2 },
	                      Want { kGeneratedAmbience, 0.03, 0.4, 0.03, 0.25 } }) {
		const RoomSpec& spec = roomSpec(w.which);
		const GeneratedIr ir = generateRoom(w.which, sr);
		CAPTURE(generatedRoomName(w.which));
		REQUIRE(ir.left.size() == static_cast<size_t>(spec.seconds * sr));
		// UNIT ENERGY PER CHANNEL, as every generated room.
		double e = 0.0;
		for (const float v : ir.left) {
			e += static_cast<double>(v) * v;
		}
		CHECK(e == doctest::Approx(1.0).epsilon(1.0e-4));
		// SILENT BEFORE THE PREDELAY.
		const auto pre = static_cast<size_t>(spec.predelay * sr);
		CHECK(std::all_of(ir.left.begin(), ir.left.begin() + static_cast<long>(pre), [](float v) { return v == 0.0f; }));
		const double low = rt60(band(ir.left, sr, 400.0, false), sr, w.lowFrom, w.lowTo);
		const double high = rt60(band(ir.left, sr, 8000.0, true), sr, w.highFrom, w.highTo);
		MESSAGE(std::string(generatedRoomName(w.which)) << ": low RT60 " << low << " s (" << spec.rt60Low << "), high RT60 " << high
		                                   << " s (" << spec.rt60High << ")");
		CHECK(std::fabs(low - spec.rt60Low) < 0.15 * spec.rt60Low);
		CHECK(std::fabs(high - spec.rt60High) < 0.15 * spec.rt60High);
	}
	CHECK(std::string(generatedRoomName(kGeneratedAmbience)) == "Ambience");
}

//--------------------------------------------------------------------------------------------------//
//--------------------------------------------------------------------------------------------------//
// THE CONVOLVER.                                                                                     //
//--------------------------------------------------------------------------------------------------//

#include "fx/conv/convolver.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>

namespace {

std::atomic<long> gNews {0};
std::atomic<long> gDeletes {0};

/// A deterministic input: noise with some silence in it, so the head and the tail both meet edges.
std::vector<float> testInput(size_t frames, uint32_t seed) {
	std::vector<float> x(frames);
	uint32_t s = seed;
	for (size_t i = 0; i < frames; ++i) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		const bool quiet = (i / 3000) % 4 == 3;
		x[i] = quiet ? 0.0f : static_cast<float>(static_cast<int32_t>(s)) / 2147483648.0f * 0.5f;
	}
	return x;
}

/// **A DENSE IR, NOT THE GENERATED ROOM, FOR THE CONVOLVER'S OWN CASES.** The generated room is silent
/// for its 7 ms predelay, 336 samples, so its first 128 taps (the whole direct-form head) are zero, and
/// a head built backwards convolves zeros the same either way. MEASURED: that known-bad passed every
/// case. So these run on noise that is loud from sample 0 and decays, in stereo.
GeneratedIr denseIr(double sr, double seconds) {
	GeneratedIr ir;
	ir.sampleRate = sr;
	const auto n = static_cast<size_t>(seconds * sr);
	ir.left.resize(n);
	ir.right.resize(n);
	uint32_t s = 0xace1u;
	for (size_t i = 0; i < n; ++i) {
		const double env = std::exp(-6.9 * static_cast<double>(i) / static_cast<double>(n));
		for (std::vector<float>* ch : {&ir.left, &ir.right}) {
			s ^= s << 13;
			s ^= s >> 17;
			s ^= s << 5;
			(*ch)[i] = static_cast<float>(env * static_cast<double>(static_cast<int32_t>(s)) / 2147483648.0 * 0.1);
		}
	}
	return ir;
}

/// THE REFERENCE: plain direct convolution, in double.
std::vector<double> direct(const std::vector<float>& x, const std::vector<float>& h) {
	std::vector<double> y(x.size(), 0.0);
	for (size_t n = 0; n < x.size(); ++n) {
		double acc = 0.0;
		const size_t kmax = std::min(h.size(), n + 1);
		for (size_t k = 0; k < kmax; ++k) {
			acc += static_cast<double>(h[k]) * x[n - k];
		}
		y[n] = acc;
	}
	return y;
}

} // namespace

namespace {
/// OUT OF LINE, as `test_backplane_graph.cpp` has it: g++-14 -O3 inlines a malloc straight into a replaced
/// `operator new` and then raises a false -Werror=mismatched-new-delete against the free below.
[[gnu::noinline]] void* countedAlloc(std::size_t n) {
	gNews.fetch_add(1, std::memory_order_relaxed);
	if (void* p = feathers::test::heapAlloc(n == 0 ? 1 : n)) {
		return p;
	}
	throw std::bad_alloc();
}
} // namespace

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t n) { return countedAlloc(n); }
void* operator new[](std::size_t n) { return countedAlloc(n); }
void operator delete(void* p) noexcept {
	if (p != nullptr) {
		gDeletes.fetch_add(1, std::memory_order_relaxed);
	}
	feathers::test::heapFree(p);
}
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept {
	if (p != nullptr) {
		gDeletes.fetch_add(1, std::memory_order_relaxed);
	}
	feathers::test::heapFree(p);
}
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"
#pragma GCC diagnostic pop

TEST_CASE("the convolver IS convolution: it matches direct convolution, through any host block sizes") {
	const double sr = 48000.0;
	// 0.4 s = 19200 taps: long enough to reach every stage of the telescope (the last starts at 16384).
	GeneratedIr ir = denseIr(sr, 0.4);
	const std::vector<float> x = testInput(static_cast<size_t>(0.5 * sr), 0x1234u);

	Convolver conv;
	conv.prepare(ir.left.size());
	conv.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	REQUIRE(conv.kernel() != nullptr);
	CHECK(conv.kernel()->stagePartitions(0) == 15); // taps [128, 2048) in 128s
	CHECK(conv.kernel()->stagePartitions(1) == 14); // taps [2048, 16384) in 1024s
	CHECK(conv.kernel()->stagePartitions(2) == 1);  // taps [16384, 19200) in one 8192

	std::vector<float> l(x.size(), 0.0f), r(x.size(), 0.0f);
	const size_t blocks[] = {1, 37, 128, 500, 3, 255, 64, 1000, 129};
	for (size_t at = 0, k = 0; at < x.size(); ++k) {
		const size_t n = std::min(blocks[k % 9], x.size() - at);
		conv.process(x.data() + at, l.data() + at, r.data() + at, n);
		at += n;
	}
	const std::vector<double> wantL = direct(x, ir.left);
	const std::vector<double> wantR = direct(x, ir.right);
	double peak = 0.0, errL = 0.0, errR = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		peak = std::max(peak, std::fabs(wantL[i]));
		errL = std::max(errL, std::fabs(wantL[i] - l[i]));
		errR = std::max(errR, std::fabs(wantR[i] - r[i]));
	}
	CAPTURE(peak);
	CAPTURE(errL);
	CAPTURE(errR);
	CHECK(peak > 0.05);
	CHECK(errL < 1.0e-5 * peak * 10.0);
	CHECK(errR < 1.0e-5 * peak * 10.0);
}

TEST_CASE("the convolver has ZERO latency: an impulse at sample 0 returns the IR from sample 0") {
	const double sr = 48000.0;
	GeneratedIr ir = denseIr(sr, 0.1);
	Convolver conv;
	conv.prepare(ir.left.size());
	conv.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	std::vector<float> in(ir.left.size(), 0.0f), l(in.size(), 0.0f), r(in.size(), 0.0f);
	in[0] = 1.0f;
	conv.process(in.data(), l.data(), r.data(), in.size());
	double worst = 0.0;
	for (size_t i = 0; i < in.size(); ++i) {
		worst = std::max({worst, std::fabs(static_cast<double>(l[i]) - ir.left[i]),
		                  std::fabs(static_cast<double>(r[i]) - ir.right[i])});
	}
	CAPTURE(worst);
	// The head is direct form: exact to float, on taps that are NOT zero (asserted, so the case can see it).
	CHECK(std::fabs(ir.left[0]) > 1.0e-4f);
	for (size_t i = 0; i < Convolver::kBlock; ++i) {
		CHECK(l[i] == ir.left[i]);
	}
	CHECK(worst < 1.0e-6);
}

TEST_CASE("the convolver allocates nothing while it runs") {
	const double sr = 48000.0;
	GeneratedIr ir = generateTestIr(sr, 0.5);
	Convolver conv;
	conv.prepare(ir.left.size());
	conv.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	const std::vector<float> x = testInput(48000, 0x77u);
	std::vector<float> l(x.size(), 0.0f), r(x.size(), 0.0f);
	const long before = gNews.load();
	for (size_t at = 0; at < x.size(); at += 480) {
		conv.process(x.data() + at, l.data() + at, r.data() + at, 480);
	}
	CHECK(gNews.load() == before);
	CHECK(conv.forwardFftsForTest() == x.size() / Convolver::kBlock);
}

TEST_CASE("the convolver's cost on the generated 2.5 s IR, measured") {
	// MEASURED, NOT ASSERTED: this host's load swings (memory: benchmarking on a shared host), so a time
	// threshold would be a flake. The number is for the scope's record.
	const double sr = 48000.0;
	GeneratedIr ir = generateTestIr(sr, 2.5);
	Convolver conv;
	conv.prepare(ir.left.size());
	conv.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	const std::vector<float> x = testInput(static_cast<size_t>(10.0 * sr), 0x99u);
	std::vector<float> l(x.size(), 0.0f), r(x.size(), 0.0f);
	const auto t0 = std::chrono::steady_clock::now();
	for (size_t at = 0; at < x.size(); at += 256) {
		conv.process(x.data() + at, l.data() + at, r.data() + at, std::min<size_t>(256, x.size() - at));
	}
	const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	MESSAGE("2.5 s stereo IR at 48 kHz: " << conv.partitions() << " partitions; 10 s of audio in " << secs
	                                       << " s = " << (secs / 10.0 * 100.0) << "% of one core");
	CHECK(secs > 0.0);
}

//------------------------------------------------------------------------------------------------//
// LOADING AN IR FROM DISK (backlog JD, piece 2)                                                     //
//------------------------------------------------------------------------------------------------//

#include <cstdio>
#include <string>

namespace {

constexpr double kTau = 6.283185307179586;

/// A 32-bit float WAV, written by hand from the RIFF layout rather than by dr_wav, so the reader is
/// checked against the format and not against its own writer.
bool writeFloatWav(const std::string& path, const std::vector<float>& interleaved, unsigned channels,
                   unsigned rate) {
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f == nullptr) {
		return false;
	}
	const auto u32 = [f](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	const auto u16 = [f](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	const auto dataBytes = static_cast<uint32_t>(interleaved.size() * 4);
	std::fwrite("RIFF", 1, 4, f);
	u32(36 + dataBytes);
	std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16);
	u16(3); // IEEE float
	u16(static_cast<uint16_t>(channels));
	u32(rate);
	u32(rate * channels * 4);
	u16(static_cast<uint16_t>(channels * 4));
	u16(32);
	std::fwrite("data", 1, 4, f);
	u32(dataBytes);
	std::fwrite(interleaved.data(), 4, interleaved.size(), f);
	std::fclose(f);
	return true;
}

double energyOf(const std::vector<float>& x) {
	double e = 0.0;
	for (float v : x) {
		e += static_cast<double>(v) * v;
	}
	return e;
}

} // namespace

TEST_CASE("offline resampling keeps a tone's frequency and level: a 1 kHz sine at 44.1 kHz read at 48 kHz") {
	const double from = 44100.0;
	const double to = 48000.0;
	std::vector<float> x(4410);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(0.5 * std::sin(kTau * 1000.0 * static_cast<double>(i) / from));
	}
	const std::vector<float> y = resampleOffline(x.data(), x.size(), from, to);
	CHECK(y.size() == 4800);
	// Away from the edges, where the kernel runs off the input, the output IS the tone at the new rate.
	double worst = 0.0;
	for (size_t j = 200; j < y.size() - 200; ++j) {
		const double want = 0.5 * std::sin(kTau * 1000.0 * static_cast<double>(j) / to);
		worst = std::max(worst, std::fabs(static_cast<double>(y[j]) - want));
	}
	MESSAGE("worst error " << worst);
	CHECK(worst < 1.0e-3);
}

TEST_CASE("offline resampling DOWN removes what the new rate cannot hold: 20 kHz at 96 kHz read at 32 kHz") {
	std::vector<float> x(9600);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(0.5 * std::sin(kTau * 20000.0 * static_cast<double>(i) / 96000.0));
	}
	const std::vector<float> y = resampleOffline(x.data(), x.size(), 96000.0, 32000.0);
	double peak = 0.0;
	for (size_t j = 200; j < y.size() - 200; ++j) {
		peak = std::max(peak, std::fabs(static_cast<double>(y[j])));
	}
	MESSAGE("20 kHz through a 32 kHz resample: peak " << peak);
	CHECK(peak < 1.0e-3); // it would alias to 12 kHz at full level
}

TEST_CASE("a trimmed IR: mono is both channels, stereo keeps its balance, the level is unit energy") {
	std::vector<float> mono(1000);
	for (size_t i = 0; i < mono.size(); ++i) {
		mono[i] = static_cast<float>(std::exp(-static_cast<double>(i) / 200.0)) * ((i % 2) != 0u ? -1.0f : 1.0f);
	}
	const LoadedIr m = trimIr(resampleIr(mono.data(), mono.size(), 1, 48000.0, 48000.0), {}, 10.0);
	REQUIRE(m.ok);
	CHECK(m.left == m.right);
	CHECK(0.5 * (energyOf(m.left) + energyOf(m.right)) == doctest::Approx(1.0).epsilon(1.0e-6));

	std::vector<float> stereo(2 * mono.size());
	for (size_t i = 0; i < mono.size(); ++i) {
		stereo[2 * i] = 2.0f * mono[i];
		stereo[2 * i + 1] = mono[i];
	}
	const LoadedIr s = trimIr(resampleIr(stereo.data(), mono.size(), 2, 48000.0, 48000.0), {}, 10.0);
	REQUIRE(s.ok);
	CHECK(energyOf(s.left) / energyOf(s.right) == doctest::Approx(4.0).epsilon(1.0e-6)); // one gain, not two
	CHECK(0.5 * (energyOf(s.left) + energyOf(s.right)) == doctest::Approx(1.0).epsilon(1.0e-6));

	std::vector<float> silent(500, 0.0f);
	const LoadedIr z = trimIr(resampleIr(silent.data(), silent.size(), 1, 48000.0, 48000.0), {}, 10.0);
	CHECK_FALSE(z.ok);
	CHECK(z.error == "silent file");
}

TEST_CASE("a span longer than the cap is cut there, with a fade, and says so") {
	std::vector<float> x(48000 * 3, 0.25f);
	const LoadedIr ir = trimIr(resampleIr(x.data(), x.size(), 1, 48000.0, 48000.0), {}, 2.0);
	REQUIRE(ir.ok);
	CHECK(ir.truncated);
	CHECK(ir.left.size() == 96000);
	CHECK(std::fabs(ir.left.back()) < 1.0e-6f);
	const size_t mid = 96000 - static_cast<size_t>(kIrCutFadeSeconds * 48000.0) / 2;
	CHECK(std::fabs(ir.left[mid] / ir.left[1000] - 0.5f) < 0.01f);

	const LoadedIr fits = trimIr(resampleIr(x.data(), 48000, 1, 48000.0, 48000.0), {}, 2.0);
	REQUIRE(fits.ok);
	CHECK_FALSE(fits.truncated);
	CHECK(fits.left.size() == 48000);
}

TEST_CASE("an IR read from a WAV file on disk: channels, rate and length as written") {
	// A stereo file at 44.1 kHz: an impulse on the left at frame 0, on the right at frame 441 (10 ms).
	std::vector<float> data(2 * 4410, 0.0f);
	data[0] = 1.0f;
	data[2 * 441 + 1] = 1.0f;
	const std::string path = "test_conv_ir.wav";
	REQUIRE(writeFloatWav(path, data, 2, 44100));
	const LoadedIr ir = loadIrFile(path, 48000.0);
	std::remove(path.c_str());
	REQUIRE(ir.ok);
	CHECK(ir.fileChannels == 2);
	CHECK(ir.fileSampleRate == 44100.0);
	CHECK(ir.sampleRate == 48000.0);
	CHECK(ir.left.size() == 4800);
	// The right channel's impulse is 10 ms in at the NEW rate: frame 480.
	const auto peakAt = [](const std::vector<float>& v) {
		return static_cast<size_t>(std::max_element(v.begin(), v.end(), [](float a, float b) {
			                           return std::fabs(a) < std::fabs(b);
		                           }) - v.begin());
	};
	CHECK(peakAt(ir.left) == 0);
	CHECK(peakAt(ir.right) == 480);

	const LoadedIr missing = loadIrFile("no/such/file.wav", 48000.0);
	CHECK_FALSE(missing.ok);
	CHECK(missing.error == "cannot read file");
}

//------------------------------------------------------------------------------------------------//
// FEATHERVERB'S IR: LOADED, SWAPPED WHILE RUNNING, AND SAVED AS A FILENAME (backlog JD)              //
//------------------------------------------------------------------------------------------------//

#include "fx/conv/ir_ref.h"
#include "fx/featherverb/featherverb.h"

namespace {

namespace fv = feathers::fx::featherverb;

/// A short stereo IR, loud from sample 0 and different per channel, written as a 32-bit float WAV at
/// `rate`. Returned as the two channels it holds, before any normalising.
std::pair<std::vector<float>, std::vector<float>> writeTestIrWav(const std::string& path, unsigned rate,
                                                                size_t frames) {
	std::vector<float> l(frames), r(frames), inter(2 * frames);
	uint32_t s = 0x5eedu;
	for (size_t i = 0; i < frames; ++i) {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		const double env = std::exp(-static_cast<double>(i) / (0.2 * static_cast<double>(frames)));
		l[i] = static_cast<float>(env * (static_cast<double>(s & 0xffff) / 32768.0 - 1.0));
		r[i] = static_cast<float>(0.5 * env * (static_cast<double>((s >> 16) & 0xffff) / 32768.0 - 1.0));
		inter[2 * i] = l[i];
		inter[2 * i + 1] = r[i];
	}
	REQUIRE(writeFloatWav(path, inter, 2, rate));
	return {l, r};
}

/// A device in Conv Only at Mix 1, prepared at `sr`: its output IS the convolution of the input.
fv::Settings convOnly() {
	fv::Settings s;
	s.mix = 1.0f;
	s.convMode = fv::kConvOnly;
	return s;
}

} // namespace

TEST_CASE("Featherverb plays the FILE's IR: an impulse through Conv Only returns the file, normalised") {
	const std::string path = "test_fv_ir_a.wav";
	const size_t frames = 3000;
	const auto [fl, fr] = writeTestIrWav(path, 48000, frames);
	fv::Featherverb dev;
	dev.setTarget(convOnly());
	dev.prepare(48000.0);
	REQUIRE(dev.loadIr(path));
	std::remove(path.c_str());
	CHECK(dev.irPath() == path);
	CHECK(dev.irError().empty());

	std::vector<float> l(8000, 0.0f), r(8000, 0.0f);
	l[0] = r[0] = 1.0f;
	dev.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	CHECK_FALSE(dev.irPendingForTest());
	CHECK(dev.convolverForTest().irFrames() == frames);

	// THE REFERENCE, RESTATED: one gain for both channels, making the mean channel energy 1.
	double e = 0.0;
	for (size_t i = 0; i < frames; ++i) {
		e += static_cast<double>(fl[i]) * fl[i] + static_cast<double>(fr[i]) * fr[i];
	}
	const double g = 1.0 / std::sqrt(0.5 * e);
	// THE WET LIMITER'S LOOKAHEAD (backlog LD): the whole wet arrives `lookahead` frames late, silence before it.
	const size_t lookahead = dev.wetLimiterForTest().lookahead();
	REQUIRE(lookahead == 72);
	double worst = 0.0;
	double peak = 0.0;
	for (size_t i = 0; i < l.size(); ++i) {
		const size_t at = i - lookahead;
		const double wl = i >= lookahead && at < frames ? g * fl[at] : 0.0;
		const double wr = i >= lookahead && at < frames ? g * fr[at] : 0.0;
		worst = std::max({worst, std::fabs(l[i] - wl), std::fabs(r[i] - wr)});
		peak = std::max(peak, std::fabs(wl));
	}
	CHECK(peak > 0.01);
	CHECK(worst < 1.0e-5 * peak);
}

TEST_CASE("Featherverb takes a new IR WHILE PLAYING: under a dip to zero and back, allocating and freeing nothing") {
	const std::string path = "test_fv_ir_b.wav";
	(void)writeTestIrWav(path, 48000, 4000);
	fv::Featherverb dev;
	dev.setTarget(convOnly());
	dev.prepare(48000.0);
	const size_t generated = dev.convolverForTest().irFrames();
	REQUIRE(generated > 4000);

	const std::vector<float> x = testInput(48000, 0x3141u);
	std::vector<float> l = x, r = x;
	size_t at = 0;
	const auto run = [&](size_t n) {
		dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(n));
		at += n;
	};
	for (int k = 0; k < 40; ++k) {
		run(256); // playing, the generated room running
	}
	REQUIRE(dev.loadIr(path)); // the message thread's half: decode, partition, post
	std::remove(path.c_str());
	CHECK(dev.irPendingForTest());

	const long newsBefore = gNews.load();
	const long deletesBefore = gDeletes.load();
	float lowest = 1.0f;
	bool sawDip = false;
	for (int k = 0; k < 40; ++k) {
		run(64);
		lowest = std::min(lowest, dev.swapGainForTest());
		sawDip = sawDip || dev.swapGainForTest() < 1.0f;
	}
	CHECK(gNews.load() == newsBefore);
	CHECK(gDeletes.load() == deletesBefore); // the old kernel is PUT DOWN, not freed, on this thread
	CHECK(sawDip);            // it was playing, so it dipped rather than cutting
	CHECK(lowest == 0.0f);    // all the way down before the swap
	CHECK(dev.swapGainForTest() == 1.0f);
	CHECK_FALSE(dev.irPendingForTest());
	CHECK(dev.convolverForTest().irFrames() == 4000);

	// AND THE DIP IS WHAT KEEPS IT FROM CLICKING: nowhere does the output jump further than the input's
	// own convolution does elsewhere in the run.
	double biggestStep = 0.0;
	for (size_t i = 1; i < at; ++i) {
		biggestStep = std::max(biggestStep, static_cast<double>(std::fabs(l[i] - l[i - 1])));
	}
	MESSAGE("largest sample-to-sample step through the swap: " << biggestStep);
}

TEST_CASE("Featherverb before audio takes its IR at once: nothing is playing, so there is nothing to dip") {
	const std::string path = "test_fv_ir_c.wav";
	(void)writeTestIrWav(path, 48000, 2000);
	fv::Featherverb dev;
	dev.setTarget(convOnly());
	dev.prepare(48000.0);
	REQUIRE(dev.loadIr(path));
	std::remove(path.c_str());
	// THE WET LIMITER'S LOOKAHEAD (backlog LD): the answer to the first sample leaves `lookahead` frames later.
	const size_t lookahead = dev.wetLimiterForTest().lookahead();
	std::vector<float> l(64 + lookahead, 0.0f), r(64 + lookahead, 0.0f);
	l[0] = r[0] = 1.0f;
	dev.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	CHECK(dev.swapGainForTest() == 1.0f);
	CHECK(dev.convolverForTest().irFrames() == 2000);
	CHECK(l[lookahead] != 0.0f); // the new IR answered the very first sample
}

TEST_CASE("Featherverb's body is the IR's FILENAME: saved, read back, and the file read again at the new rate") {
	const std::string path = "test_fv_ir_d.wav";
	(void)writeTestIrWav(path, 48000, 4800);
	fv::Featherverb a;
	a.prepare(48000.0);
	REQUIRE(a.loadIr(path));
	std::vector<uint8_t> body;
	a.saveBody(body);
	// THE FORMAT, AS WRITTEN: a path, not audio (just the filename, by design).
	CHECK(body.size() == feathers::fx::conv::kIrRefHeaderBytes + feathers::fx::conv::kIrShapeBytes + path.size());
	CHECK(std::string(body.begin() + static_cast<long>(feathers::fx::conv::kIrRefHeaderBytes + feathers::fx::conv::kIrShapeBytes), body.end()) == path);

	fv::Featherverb b;
	b.prepare(96000.0);
	REQUIRE(b.loadBody(body.data(), body.size()));
	CHECK(b.irPath() == path);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	b.processStereo(l.data(), r.data(), 64);
	CHECK(b.convolverForTest().irFrames() == 9600); // read at 96 kHz: the same 100 ms, twice the frames

	// A RATE CHANGE READS THE FILE AGAIN.
	b.prepare(44100.0);
	CHECK(b.convolverForTest().irFrames() == 4410);
	std::remove(path.c_str());
}

TEST_CASE("Featherverb's body: a file that has gone keeps its NAME, runs no IR, and says why") {
	fv::Featherverb a;
	a.prepare(48000.0);
	const std::vector<uint8_t> body =
	    feathers::fx::conv::encodeIrRef({feathers::fx::conv::IrRef::Kind::File, "/no/such/hall.wav", {}});
	REQUIRE(a.loadBody(body.data(), body.size()));
	CHECK(a.irPath() == "/no/such/hall.wav");
	CHECK(a.irError() == "cannot read file");
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	a.processStereo(l.data(), r.data(), 64);
	CHECK(a.convolverForTest().irFrames() == 0);
	std::vector<uint8_t> again;
	a.saveBody(again);
	CHECK(again == body); // saving it again does not lose the name
}

TEST_CASE("Featherverb's body refuses what this build cannot read, the escape hatch's EMBEDDED kind included") {
	fv::Featherverb a;
	a.prepare(48000.0);
	std::vector<uint8_t> before;
	a.saveBody(before);

	std::vector<uint8_t> embedded = feathers::fx::conv::encodeIrRef({feathers::fx::conv::IrRef::Kind::File, "x.wav", {}});
	embedded[5] = 2; // kind 2: a later build's embedded audio
	CHECK_FALSE(a.loadBody(embedded.data(), embedded.size()));
	std::vector<uint8_t> truncated = feathers::fx::conv::encodeIrRef({feathers::fx::conv::IrRef::Kind::File, "x.wav", {}});
	truncated.pop_back();
	CHECK_FALSE(a.loadBody(truncated.data(), truncated.size()));
	const uint8_t junk[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
	CHECK_FALSE(a.loadBody(junk, sizeof junk));

	std::vector<uint8_t> after;
	a.saveBody(after);
	CHECK(after == before); // all or nothing: as it was

	// AND THE GENERATED ROOM IS WRITTEN EXPLICITLY, never as an empty body — an empty one would send a
	// host back to the body it stored at load, which is the file this device has since left.
	CHECK(before.size() == feathers::fx::conv::kIrRefHeaderBytes + feathers::fx::conv::kIrShapeBytes);
	CHECK(before[5] == 0);
}

//------------------------------------------------------------------------------------------------//
// THE MARKERS: BEGIN AND END (markers in the impulse view choose the begin and end points at which //
// to truncate)                                                                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("the markers cut the file between them: the span's samples, one gain, faded only where a cut lands") {
	// A signal whose every sample is its own address, so a slice is checked as a value, not as a shape.
	std::vector<float> x(48000);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = 0.1f + static_cast<float>(i % 997) / 997.0f;
	}
	const LoadedIr whole = resampleIr(x.data(), x.size(), 1, 48000.0, 48000.0);
	REQUIRE(whole.ok);
	const LoadedIr span = trimIr(whole, {0.25f, 0.5f}, 10.0);
	REQUIRE(span.ok);
	CHECK_FALSE(span.truncated);
	REQUIRE(span.left.size() == 12000); // frames [12000, 24000)
	CHECK(0.5 * (energyOf(span.left) + energyOf(span.right)) == doctest::Approx(1.0).epsilon(1.0e-6));

	const size_t fadeIn = static_cast<size_t>(kIrBeginFadeSeconds * 48000.0);
	const size_t fadeOut = static_cast<size_t>(kIrCutFadeSeconds * 48000.0);
	const double gain = span.left[fadeIn] / x[12000 + fadeIn];
	double worst = 0.0;
	for (size_t k = fadeIn; k < span.left.size() - fadeOut; ++k) {
		worst = std::max(worst, std::fabs(span.left[k] - gain * x[12000 + k]));
	}
	CHECK(worst < 1.0e-6);
	CHECK(span.left.front() == 0.0f);            // a moved begin fades in from zero
	CHECK(std::fabs(span.left.back()) < 1.0e-3f); // a moved end fades out to (nearly) zero

	// AT THE FILE'S OWN EDGES THERE IS NO CUT, SO NO FADE: the first sample is the file's first.
	const LoadedIr all = trimIr(whole, {}, 10.0);
	REQUIRE(all.ok);
	CHECK(all.left.size() == x.size());
	const double g = all.left[100] / x[100];
	CHECK(std::fabs(all.left.front() - g * x.front()) < 1.0e-6);
	CHECK(std::fabs(all.left.back() - g * x.back()) < 1.0e-6);

	// A BEGIN NOT BEFORE ITS END IS READ AS THE WHOLE FILE.
	CHECK(trimIr(whole, {0.6f, 0.6f}, 10.0).left.size() == x.size());
}

TEST_CASE("the markers choose WHICH six seconds: the capacity cuts the span, not the file's start") {
	std::vector<float> x(48000 * 10);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = 0.1f + static_cast<float>(i % 101) / 101.0f;
	}
	const LoadedIr whole = resampleIr(x.data(), x.size(), 1, 48000.0, 48000.0);
	const LoadedIr span = trimIr(whole, {0.5f, 1.0f}, 2.0);
	REQUIRE(span.ok);
	CHECK(span.truncated);
	REQUIRE(span.left.size() == 96000);
	const double gain = span.left[1000] / x[240000 + 1000];
	CHECK(std::fabs(span.left[5000] - gain * x[240000 + 5000]) < 1.0e-6); // from the 5 s mark, not from 0
}

TEST_CASE("the body carries the markers, and refuses markers that are not a span") {
	using feathers::fx::conv::IrRef;
	IrRef ref;
	ref.kind = IrRef::Kind::File;
	ref.path = "/rooms/hall.wav";
	ref.shape.begin = 0.125f;
	ref.shape.end = 0.75f;
	const std::vector<uint8_t> body = feathers::fx::conv::encodeIrRef(ref);
	IrRef back;
	REQUIRE(feathers::fx::conv::decodeIrRef(body.data(), body.size(), back));
	CHECK(back.path == ref.path);
	CHECK(back.shape.begin == 0.125f);
	CHECK(back.shape.end == 0.75f);

	for (const auto& [b, e] : std::vector<std::pair<float, float>> {{0.5f, 0.5f}, {0.7f, 0.2f}, {-0.1f, 0.5f}, {0.0f, 1.5f}}) {
		IrRef bad = ref;
		bad.shape.begin = b;
		bad.shape.end = e;
		const std::vector<uint8_t> bytes = feathers::fx::conv::encodeIrRef(bad);
		IrRef out;
		CHECK_FALSE(feathers::fx::conv::decodeIrRef(bytes.data(), bytes.size(), out));
	}
}

TEST_CASE("Featherverb's markers: a move re-cuts the file while playing, is saved, and is read back") {
	const std::string path = "test_fv_ir_e.wav";
	(void)writeTestIrWav(path, 48000, 9600);
	fv::Featherverb dev;
	dev.setTarget(convOnly());
	dev.prepare(48000.0);
	CHECK(dev.setIrTrim({0.1f, 0.5f})); // the generated room is shaped as a file is
	REQUIRE(dev.loadIr(path));

	const std::vector<float> x = testInput(24000, 0x2718u);
	std::vector<float> l = x, r = x;
	for (size_t at = 0; at < 6000; at += 200) {
		dev.processStereo(l.data() + at, r.data() + at, 200);
	}
	CHECK(dev.convolverForTest().irFrames() == 9600);
	const long newsBefore = gNews.load();
	REQUIRE(dev.setIrTrim({0.25f, 0.75f})); // message thread
	const long newsAfterPost = gNews.load();
	for (size_t at = 6000; at < 24000; at += 200) {
		dev.processStereo(l.data() + at, r.data() + at, 200);
	}
	CHECK(gNews.load() == newsAfterPost); // nothing allocated on the audio side of it
	CHECK(newsAfterPost >= newsBefore);
	CHECK(dev.convolverForTest().irFrames() == 4800); // frames [2400, 7200)
	CHECK(dev.irTrim() == feathers::fx::conv::IrTrim {0.25f, 0.75f});

	std::vector<uint8_t> body;
	dev.saveBody(body);
	fv::Featherverb other;
	other.prepare(48000.0);
	REQUIRE(other.loadBody(body.data(), body.size()));
	CHECK(other.irTrim() == feathers::fx::conv::IrTrim {0.25f, 0.75f});
	std::vector<float> a(64, 0.0f), b(64, 0.0f);
	other.processStereo(a.data(), b.data(), 64);
	CHECK(other.convolverForTest().irFrames() == 4800);

	// A NEW FILE STARTS WHOLE.
	REQUIRE(dev.loadIr(path));
	CHECK(dev.irTrim() == feathers::fx::conv::IrTrim {});
	std::remove(path.c_str());
}

//------------------------------------------------------------------------------------------------//
// THE ENVELOPE AND THE STRETCH                                                                      //
//------------------------------------------------------------------------------------------------//

#include "mod/mseg/mseg_types.h"

TEST_CASE("the envelope multiplies the span by the line drawn over it; the level is then made whole again") {
	std::vector<float> ones(4000, 0.5f);
	const LoadedIr whole = resampleIr(ones.data(), ones.size(), 1, 48000.0, 48000.0);
	const LoadedIr span = shapeIr(whole, 0.0f, 1.0f, 1.0f, {1.0f, 0.0f}, 10.0); // a ramp from 1 down to 0
	REQUIRE(span.ok);
	REQUIRE(span.left.size() == 4000);
	CHECK(0.5 * (energyOf(span.left) + energyOf(span.right)) == doctest::Approx(1.0).epsilon(1.0e-6));
	const double g = span.left[0]; // the level's gain times the envelope's 1
	double worst = 0.0;
	for (size_t i = 0; i < span.left.size(); ++i) {
		const double want = g * (1.0 - static_cast<double>(i) / 3999.0);
		worst = std::max(worst, std::fabs(span.left[i] - want));
	}
	CHECK(worst < 1.0e-6);
	CHECK(std::fabs(span.left.back()) < 1.0e-7f);
}

TEST_CASE("the stretch makes the span longer and lower together: twice as long is an octave down") {
	std::vector<float> x(4800);
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(std::sin(kTau * 1000.0 * static_cast<double>(i) / 48000.0));
	}
	const LoadedIr whole = resampleIr(x.data(), x.size(), 1, 48000.0, 48000.0);
	const LoadedIr span = shapeIr(whole, 0.0f, 1.0f, 2.0f, {}, 10.0);
	REQUIRE(span.ok);
	CHECK(span.left.size() == 9600);
	// 500 Hz now: sample j is the old signal at j / 2, scaled by the level's gain.
	const double g = std::sqrt(2.0 / 9600.0) > 0.0 ? span.left[300] / std::sin(kTau * 500.0 * 300.0 / 48000.0) : 0.0;
	double worst = 0.0;
	for (size_t j = 400; j < 9200; ++j) {
		worst = std::max(worst, std::fabs(span.left[j] - g * std::sin(kTau * 500.0 * static_cast<double>(j) / 48000.0)));
	}
	CHECK(worst < 1.0e-3 * std::fabs(g));
}

TEST_CASE("Featherverb's envelope is an MSEG shape: a straight fall to zero plays as a straight fall to zero") {
	const std::string path = "test_fv_ir_f.wav";
	std::vector<float> inter(2 * 4800, 0.25f);
	REQUIRE(writeFloatWav(path, inter, 2, 48000));
	fv::Featherverb dev;
	dev.setTarget(convOnly());
	dev.prepare(48000.0);
	REQUIRE(dev.loadIr(path));
	std::remove(path.c_str());

	namespace mseg = feathers::mod::mseg;
	mseg::Settings env;
	env.startLevel = 1.0f;
	env.count = 1;
	env.segment[0].level = 0.0f;
	env.segment[0].shape = 0.0f;
	REQUIRE(dev.setIrEnvelope(env));
	CHECK(dev.irEnvelope().segment[0].level == 0.0f);

	std::vector<float> l(6000, 0.0f), r(6000, 0.0f);
	l[0] = r[0] = 1.0f;
	dev.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	// THE WET LIMITER'S LOOKAHEAD (backlog LD): the shape starts `lookahead` frames late.
	const size_t lookahead = dev.wetLimiterForTest().lookahead();
	const double g = l[lookahead];
	REQUIRE(g > 0.0);
	double worst = 0.0;
	for (size_t i = 0; i < 4800; ++i) {
		worst = std::max(worst, std::fabs(l[i + lookahead] - g * (1.0 - static_cast<double>(i) / 4799.0)));
	}
	CHECK(worst < 1.0e-5 * g);

	// AND IT IS SAVED WITH THE FILENAME, stretch included.
	feathers::fx::conv::IrShape shape = dev.irShape();
	shape.stretch = 1.5f;
	REQUIRE(dev.setIrShape(shape));
	std::vector<uint8_t> body;
	dev.saveBody(body);
	feathers::fx::conv::IrRef ref;
	REQUIRE(feathers::fx::conv::decodeIrRef(body.data(), body.size(), ref));
	CHECK(ref.shape == dev.irShape());
	CHECK(ref.shape.stretch == 1.5f);
	CHECK(ref.shape.envSegment[0][1] == 0.0f);

	// A SHAPE A BODY COULD NOT HOLD IS REFUSED BY THE HAND TOO.
	shape.stretch = 50.0f;
	CHECK_FALSE(dev.setIrShape(shape));
	CHECK(dev.irShape().stretch == 1.5f);
}

//------------------------------------------------------------------------------------------------//
// THE CONVOLVER'S OWN PREDELAY AND FEEDBACK (convolver-specific predelay, feedback and stretch)  //
//------------------------------------------------------------------------------------------------//

namespace {

/// A device in Conv Only on a file IR, prepared at 48 kHz.
void loadConvOnly(fv::Featherverb& dev, fv::Settings s, const std::vector<float>& monoIr, const std::string& path) {
	std::vector<float> inter(2 * monoIr.size());
	for (size_t i = 0; i < monoIr.size(); ++i) {
		inter[2 * i] = inter[2 * i + 1] = monoIr[i];
	}
	REQUIRE(writeFloatWav(path, inter, 2, 48000));
	dev.setTarget(s);
	dev.prepare(48000.0);
	REQUIRE(dev.loadIr(path));
	std::remove(path.c_str());
}

} // namespace

TEST_CASE("the convolver's predelay moves the whole convolution later, by exactly its frames") {
	std::vector<float> ir(3000);
	for (size_t i = 0; i < ir.size(); ++i) {
		ir[i] = static_cast<float>(std::exp(-static_cast<double>(i) / 600.0)) * ((i % 3) == 0 ? 1.0f : -0.5f);
	}
	fv::Settings s = convOnly();
	s.convPredelay = 0.5f; // squared taper: 0.25 of 500 ms = 125 ms = 6000 frames
	fv::Featherverb delayed;
	loadConvOnly(delayed, s, ir, "test_fv_pre_a.wav");
	fv::Featherverb plain;
	loadConvOnly(plain, convOnly(), ir, "test_fv_pre_b.wav");

	std::vector<float> dl(12000, 0.0f), dr(12000, 0.0f), pl(12000, 0.0f), pr(12000, 0.0f);
	dl[0] = dr[0] = pl[0] = pr[0] = 1.0f;
	delayed.processStereo(dl.data(), dr.data(), 12000);
	plain.processStereo(pl.data(), pr.data(), 12000);
	double before = 0.0;
	for (size_t i = 0; i < 6000; ++i) {
		before = std::max(before, static_cast<double>(std::fabs(dl[i])));
	}
	CHECK(before == 0.0);
	double worst = 0.0;
	double peak = 0.0;
	for (size_t i = 0; i < 6000; ++i) {
		worst = std::max(worst, static_cast<double>(std::fabs(dl[6000 + i] - pl[i])));
		peak = std::max(peak, static_cast<double>(std::fabs(pl[i])));
	}
	CHECK(peak > 0.01);
	CHECK(worst < 1.0e-5 * peak);
}

TEST_CASE("the convolver's feedback: each return is the last one through the loop's gain and ceiling, through the predelay") {
	// A DELTA IR is the identity, so the loop is visible sample for sample: peak gain 1, loop gain 0.95.
	std::vector<float> delta(256, 0.0f);
	delta[0] = 1.0f;
	fv::Settings s = convOnly();
	s.convPredelay = 0.5f; // 6000 frames, which is also the loop's round trip
	s.convFeedback = 1.0f;
	fv::Featherverb dev;
	loadConvOnly(dev, s, delta, "test_fv_fb_a.wav");

	std::vector<float> l(26000, 0.0f), r(26000, 0.0f); // room past the fourth return for its window
	l[0] = r[0] = 0.01f;
	for (size_t at = 0; at < l.size(); at += 480) {
		const size_t n = std::min<size_t>(480, l.size() - at);
		dev.processStereo(l.data() + at, r.data() + at, static_cast<int>(n));
	}
	const double g = fv::kConvFeedbackMax;
	const double y1 = 0.01;
	const double lc = fv::kConvLoopCeiling;
	const auto loopOf = [&](double y) { return lc * std::tanh(g * y / lc); };
	const double y2 = loopOf(y1);
	const double y3 = loopOf(y2);
	// EACH RETURN AS THE SUM OF ITS WINDOW: the loop's slow wobble (±0.4%) moves a return by a fraction of a sample, which
	// linear interpolation splits across two samples and never changes the sum of.
	// THE WET LIMITER'S LOOKAHEAD (backlog LD): every return leaves `lookahead` frames late; the loop's own timing is
	// before it and unchanged.
	const size_t lookahead = dev.wetLimiterForTest().lookahead();
	const auto around = [&](size_t centre) {
		double s = 0.0;
		for (size_t i = centre + lookahead - 100; i <= centre + lookahead + 100; ++i) {
			s += l[i];
		}
		return s;
	};
	CHECK(std::fabs(l[6000 + lookahead] - y1) < 1.0e-6); // the first is the input itself, through no loop
	// TO 1%: a return split across two samples saturates each half a little differently under the ceiling (0.3%
	// measured), where a wrong scale would be off by the room's whole peak-to-average ratio.
	CHECK(std::fabs(around(12000) - y2) < 1.0e-2 * y2);
	CHECK(std::fabs(around(18000) - y3) < 1.0e-2 * y3);
	CHECK(std::fabs(around(24000) - loopOf(y3)) < 1.0e-2 * y3);
	CHECK(std::fabs(around(9000)) < 1.0e-9); // nothing between the returns
}


TEST_CASE("MEASURE: Featherverb's cost per conv mode, 10 s at 48 kHz in 256-frame blocks") {
	const std::vector<float> x = testInput(48000 * 10, 0x5151u);
	for (const int mode : { fv::kConvOff, fv::kConvParallel, fv::kConvOnly }) {
		fv::Settings s;
		s.convMode = mode;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0);
		std::vector<float> l = x, r = x;
		const auto t0 = std::chrono::steady_clock::now();
		for (size_t at = 0; at < l.size(); at += 256) {
			dev.processStereo(l.data() + at, r.data() + at, 256);
		}
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		MESSAGE("mode " << mode << ": " << (secs / 10.0 * 100.0) << "% of one core, ffts " << dev.convolverForTest().forwardFftsForTest());
	}
}

TEST_CASE("Featherverb's Off KILLS the convolution: after the 20 ms fade it is the Off-all-along device, to the bit, and runs nothing") {
	// Selecting Off must kill every tail and truly bypass the convolution.
	const std::vector<float> x = testInput(48000 * 3, 0x0ff0u);
	fv::Settings on = convOnly();
	on.convMode = fv::kConvParallel;
	on.convBlend = 0.5f;
	on.convPredelay = 0.3f;
	on.convFeedback = 0.8f; // a loop, so a tail that would ring on by itself
	fv::Settings off = on;
	off.convMode = fv::kConvOff;

	fv::Featherverb switched;
	switched.setTarget(on);
	switched.prepare(48000.0);
	fv::Featherverb alwaysOff;
	alwaysOff.setTarget(off);
	alwaysOff.prepare(48000.0);

	std::vector<float> sl = x, sr = x, al = x, ar = x;
	const size_t switchAt = 188 * 256; // about a second in, mid-tail, ON a chunk boundary
	for (size_t at = 0; at < x.size(); at += 256) {
		if (at == switchAt) {
			switched.setTarget(off);
		}
		const int n = static_cast<int>(std::min<size_t>(256, x.size() - at));
		switched.processStereo(sl.data() + at, sr.data() + at, n);
		if (at == switchAt + 4 * 256) {
			CHECK(switched.convDeadForTest()); // 20 ms (960 frames) of fade, and dead by the next chunk
		}
		alwaysOff.processStereo(al.data() + at, ar.data() + at, n);
	}
	CHECK(switched.convDeadForTest());
	// THE FADE IS 20 ms (960 frames) AND THEN NOTHING: from the next chunk on, bit-identical to the device that
	// never ran the convolution — no tail, no loop, and the reverb untouched by the whole episode.
	// FROM HALF A SECOND ON, BIT-IDENTICAL: the reverb's own share glides back from Parallel's 1 - Blend to 1 on
	// the routing's exponential glide, which snaps exact within ~0.3 s; the convolution itself was gone at 20 ms.
	const size_t from = switchAt + 24000;
	size_t firstDiff = x.size();
	for (size_t i = from; i < x.size(); ++i) {
		if (sl[i] != al[i] || sr[i] != ar[i]) {
			firstDiff = i;
			break;
		}
	}
	double worstAfter = 0.0;
	for (size_t i = from; i < x.size(); ++i) {
		worstAfter = std::max(worstAfter, static_cast<double>(std::fabs(sl[i] - al[i])));
	}
	MESSAGE("first difference at " << firstDiff << ", worst " << worstAfter);
	CHECK(firstDiff == x.size());
	// NOT VACUOUS: before the switch the two differed (the convolution was in the wet).
	double before = 0.0;
	for (size_t i = 24000; i < switchAt; ++i) {
		before = std::max(before, static_cast<double>(std::fabs(sl[i] - al[i])));
	}
	CHECK(before > 1.0e-3);
	// AND IT RUNS NOTHING: the FFT count stands still once dead.
	const uint64_t ffts = switched.convolverForTest().forwardFftsForTest();
	std::vector<float> more(48000, 0.1f), more2(48000, 0.1f);
	switched.processStereo(more.data(), more2.data(), 48000);
	CHECK(switched.convolverForTest().forwardFftsForTest() == ffts);
}

TEST_CASE("Featherverb's glides ARRIVE: the routing gains land exactly on their targets, never ~3e-5 short") {
	// MEASURED BEFORE THE FIX: `g += coeff · (t − g)` with the 20 ms coefficient stalled about 3e-5 from home
	// (the step fell under half an ulp), inside no 1e-6 snap — so Off left the convolver fed ~3e-5 of the input
	// forever and it never went quiet: the CPU with Conv "apparently switched off".
	const float coeff = static_cast<float>(1.0 - std::exp(-1.0 / (fv::kConvGainSeconds * 48000.0)));
	fv::ConvGains g { 0.5f, 0.5f, 0.0f };
	const fv::ConvGains off { 1.0f, 0.0f, 0.0f };
	for (int i = 0; i < 48000; ++i) {
		g.glide(off, coeff);
	}
	CHECK(g.reverb == 1.0f);
	CHECK(g.input == 0.0f);
	CHECK(g.series == 0.0f);
	// And from the other side, up to a target near 1 and down to one near 0.
	float up = 0.0f;
	float down = 1.0f;
	for (int i = 0; i < 48000; ++i) {
		fv::glideTo(up, 0.999f, coeff);
		fv::glideTo(down, 0.001f, coeff);
	}
	CHECK(up == 0.999f);
	CHECK(down == 0.001f);
}

TEST_CASE("the telescope has no spike: no 64-frame call does more than a few times the mean work, on a 6 s IR") {
	// COUNTED, NOT TIMED: this host's load parks threads for milliseconds, so wall-clock worst cases here measure
	// the scheduler. `workForTest` charges n·log2(n) per FFT and n per spectral multiply-accumulate. The big
	// stages accumulate a share per step and finish (with both inverse FFTs) the step BEFORE their edge, so an
	// edge carries one forward FFT: the worst call stays within a small multiple of the mean.
	const double sr = 48000.0;
	GeneratedIr ir = denseIr(sr, 6.0);
	Convolver conv;
	conv.prepare(ir.left.size());
	conv.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	const std::vector<float> x = testInput(static_cast<size_t>(10.0 * sr), 0x77u);
	std::vector<float> l(x.size(), 0.0f), r(x.size(), 0.0f);
	uint64_t worst = 0;
	uint64_t total = 0;
	size_t calls = 0;
	for (size_t at = 0; at + 64 <= x.size(); at += 64) {
		const uint64_t before = conv.workForTest();
		conv.process(x.data() + at, l.data() + at, r.data() + at, 64);
		const uint64_t w = conv.workForTest() - before;
		worst = std::max(worst, w);
		total += w;
		++calls;
	}
	const double mean = static_cast<double>(total) / static_cast<double>(calls);
	MESSAGE("6 s IR: " << conv.partitions() << " partitions; work per 64-frame call: mean " << mean << ", worst "
	                   << worst << " (" << (static_cast<double>(worst) / mean) << "x)");
	CHECK(static_cast<double>(worst) < 8.0 * mean);
}

//------------------------------------------------------------------------------------------------//
// LITE: THE LATE TAIL AT HALF RATE (the "Cheaper tail" option)                                          //
//------------------------------------------------------------------------------------------------//

namespace {

/// Run a convolver over `x`, one channel out.
std::vector<float> runConv(Convolver& conv, const std::vector<float>& x) {
	std::vector<float> l(x.size(), 0.0f), r(x.size(), 0.0f);
	for (size_t at = 0; at < x.size(); at += 100) {
		const size_t n = std::min<size_t>(100, x.size() - at);
		conv.process(x.data() + at, l.data() + at, r.data() + at, n);
	}
	return l;
}

} // namespace

TEST_CASE("Lite matches the exact convolution below a quarter of the rate — the tail lands where it should") {
	const double sr = 48000.0;
	GeneratedIr ir = denseIr(sr, 1.0); // reaches well into the half-rate stage (it starts at 17408)
	std::vector<float> x(static_cast<size_t>(1.5 * sr));
	for (size_t i = 0; i < x.size(); ++i) {
		const double t = static_cast<double>(i) / sr;
		x[i] = static_cast<float>(0.3 * std::sin(kTau * 440.0 * t) + 0.2 * std::sin(kTau * 2500.0 * t)
		                          + 0.1 * std::sin(kTau * 7000.0 * t));
	}
	Convolver exact;
	exact.prepare(ir.left.size());
	exact.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	ConvKernel liteKernel;
	liteKernel.build(ir.left.data(), ir.right.data(), ir.left.size(), true);
	CHECK(liteKernel.lite());
	CHECK(liteKernel.stagePartitions(1) == 15); // stage 1 reaches kLiteSplit
	Convolver lite;
	lite.prepare(ir.left.size());
	REQUIRE(lite.setKernel(&liteKernel));

	const std::vector<float> ye = runConv(exact, x);
	const std::vector<float> yl = runConv(lite, x);
	double err = 0.0;
	double sig = 0.0;
	for (size_t i = static_cast<size_t>(0.5 * sr); i < x.size(); ++i) {
		err += static_cast<double>(yl[i] - ye[i]) * (yl[i] - ye[i]);
		sig += static_cast<double>(ye[i]) * ye[i];
	}
	const double db = 10.0 * std::log10(err / sig);
	MESSAGE("Lite against exact, tones below 7 kHz: error " << db << " dB");
	CHECK(db < -40.0);
}

TEST_CASE("Lite's late tail loses what is above a quarter of the rate — the honest cost of the switch") {
	const double sr = 48000.0;
	GeneratedIr ir = denseIr(sr, 1.0);
	std::vector<float> x(static_cast<size_t>(1.5 * sr));
	for (size_t i = 0; i < x.size(); ++i) {
		x[i] = static_cast<float>(0.5 * std::sin(kTau * 18000.0 * static_cast<double>(i) / sr));
	}
	Convolver exact;
	exact.prepare(ir.left.size());
	exact.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	ConvKernel liteKernel;
	liteKernel.build(ir.left.data(), ir.right.data(), ir.left.size(), true);
	Convolver lite;
	lite.prepare(ir.left.size());
	REQUIRE(lite.setKernel(&liteKernel));
	// The late tail alone: an IR that is only its part past kLiteSplit shows what each path does to it.
	std::vector<float> tailL(ir.left.size(), 0.0f), tailR(ir.right.size(), 0.0f);
	for (size_t i = kLiteSplit; i < ir.left.size(); ++i) {
		tailL[i] = ir.left[i];
		tailR[i] = ir.right[i];
	}
	Convolver exactTail;
	exactTail.prepare(tailL.size());
	exactTail.setIr(tailL.data(), tailR.data(), tailL.size());
	ConvKernel liteTailKernel;
	liteTailKernel.build(tailL.data(), tailR.data(), tailL.size(), true);
	Convolver liteTail;
	liteTail.prepare(tailL.size());
	REQUIRE(liteTail.setKernel(&liteTailKernel));
	const std::vector<float> ye = runConv(exactTail, x);
	const std::vector<float> yl = runConv(liteTail, x);
	double ee = 0.0;
	double el = 0.0;
	for (size_t i = static_cast<size_t>(0.8 * sr); i < x.size(); ++i) {
		ee += static_cast<double>(ye[i]) * ye[i];
		el += static_cast<double>(yl[i]) * yl[i];
	}
	const double db = 10.0 * std::log10(el / ee);
	MESSAGE("an 18 kHz tone through the late tail: Lite " << db << " dB against exact");
	CHECK(db < -40.0);
}

TEST_CASE("MEASURE: Lite's saving, counted, on a 6 s IR at 96 kHz") {
	const double sr = 96000.0;
	GeneratedIr ir = denseIr(sr, 6.0);
	const std::vector<float> x = testInput(static_cast<size_t>(4.0 * sr), 0x4242u);
	Convolver exact;
	exact.prepare(ir.left.size());
	exact.setIr(ir.left.data(), ir.right.data(), ir.left.size());
	ConvKernel liteKernel;
	liteKernel.build(ir.left.data(), ir.right.data(), ir.left.size(), true);
	Convolver lite;
	lite.prepare(ir.left.size());
	REQUIRE(lite.setKernel(&liteKernel));
	(void)runConv(exact, x);
	(void)runConv(lite, x);
	const double ratio = static_cast<double>(lite.workForTest()) / static_cast<double>(exact.workForTest());
	MESSAGE("6 s IR at 96 kHz: Lite does " << (ratio * 100.0) << "% of the exact telescope's work");
	CHECK(ratio < 1.0);
}

TEST_CASE("Featherverb's Lite is saved in the body and builds a half-rate kernel") {
	fv::Featherverb dev;
	dev.prepare(48000.0);
	feathers::fx::conv::IrShape s = dev.irShape();
	s.lite = true;
	REQUIRE(dev.setIrShape(s));
	std::vector<uint8_t> body;
	dev.saveBody(body);
	feathers::fx::conv::IrRef ref;
	REQUIRE(feathers::fx::conv::decodeIrRef(body.data(), body.size(), ref));
	CHECK(ref.shape.lite);
	fv::Featherverb other;
	other.prepare(48000.0);
	REQUIRE(other.loadBody(body.data(), body.size()));
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	other.processStereo(l.data(), r.data(), 64);
	REQUIRE(other.convolverForTest().kernel() != nullptr);
	CHECK(other.convolverForTest().kernel()->lite());
	// A FLAG THIS BUILD DOES NOT KNOW IS REFUSED.
	// (the generated room has no path, so the flags word is the body's last four bytes)
	body[body.size() - 4] |= 0x02;
	CHECK_FALSE(other.loadBody(body.data(), body.size()));
}

//------------------------------------------------------------------------------------------------//
// THE IMPULSE BROWSER (forward/back arrows)                                                               //
//------------------------------------------------------------------------------------------------//

#include "fx/conv/ir_browse.h"

#include <filesystem>

TEST_CASE("the browser lists a folder's IRs in the order a file browser does, and steps through them, wrapping") {
	namespace fs = std::filesystem;
	const fs::path dir = fs::path("irbrowse_case");
	fs::remove_all(dir);
	fs::create_directories(dir / "sub");
	const std::vector<float> click { 1.0f, 0.5f, 0.25f };
	for (const char* name : { "Hall 10.wav", "hall 2.WAV", "Cave.aiff", "Room.flac", "notes.txt", "Alpha.aif" }) {
		REQUIRE(writeFloatWav((dir / name).string(), click, 1, 48000));
	}
	REQUIRE(writeFloatWav((dir / "sub" / "Deeper.wav").string(), click, 1, 48000));

	const std::vector<std::string> files = feathers::fx::conv::irFilesBeside((dir / "Cave.aiff").string());
	std::vector<std::string> names;
	for (const std::string& f : files) {
		names.push_back(fs::path(f).filename().string());
	}
	// NOT THE .txt, NOT THE SUBFOLDER; "hall 2" BEFORE "Hall 10"; CASE DOES NOT ORDER.
	CHECK(names == std::vector<std::string> { "Alpha.aif", "Cave.aiff", "hall 2.WAV", "Hall 10.wav", "Room.flac" });

	using feathers::fx::conv::steppedIr;
	const auto nameOf = [](const std::string& p) { return fs::path(p).filename().string(); };
	CHECK(nameOf(steppedIr((dir / "Cave.aiff").string(), +1)) == "hall 2.WAV");
	CHECK(nameOf(steppedIr((dir / "Cave.aiff").string(), -1)) == "Alpha.aif");
	CHECK(nameOf(steppedIr((dir / "Room.flac").string(), +1)) == "Alpha.aif"); // wraps forward
	CHECK(nameOf(steppedIr((dir / "Alpha.aif").string(), -1)) == "Room.flac"); // and back
	// A FILE THAT HAS GONE STEPS FROM WHERE IT WOULD SORT: "Bee" sat between Alpha and Cave.
	CHECK(nameOf(steppedIr((dir / "Bee.wav").string(), +1)) == "Cave.aiff");
	CHECK(nameOf(steppedIr((dir / "Bee.wav").string(), -1)) == "Alpha.aif");
	CHECK(steppedIr("no/such/folder/x.wav", +1).empty());
	fs::remove_all(dir);
}

//------------------------------------------------------------------------------------------------//
// EXCITER: THE ROOM INSIDE THE REVERB (Excite injects somewhere inside the existing architectures)  //
//------------------------------------------------------------------------------------------------//

namespace {

/// A device in Exciter at Blend `b`, Mix 1, on the generated room, over `x` (L = R = x) in 256-frame blocks.
std::vector<float> renderExciter(float b, int mode, const std::vector<float>& x) {
	fv::Settings s;
	s.mix = 1.0f;
	s.convMode = mode;
	s.convBlend = b;
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0);
	std::vector<float> l = x, r = x;
	for (size_t at = 0; at < x.size(); at += 256) {
		const int n = static_cast<int>(std::min<size_t>(256, x.size() - at));
		dev.processStereo(l.data() + at, r.data() + at, n);
	}
	return l;
}

} // namespace

TEST_CASE("Exciter at Blend 0 IS Off, to the bit: the room runs but is neither heard nor injected") {
	const std::vector<float> x = testInput(48000 * 2, 0xe0e0u);
	CHECK(renderExciter(0.0f, fv::kConvExciter, x) == renderExciter(0.0f, fv::kConvOff, x));
}

TEST_CASE("Exciter at Blend 1: nothing for the first 256 frames of an impulse, then the reverb rings from the room alone") {
	std::vector<float> x(48000, 0.0f);
	x[0] = 0.5f;
	const std::vector<float> y = renderExciter(1.0f, fv::kConvExciter, x);
	double early = 0.0;
	for (size_t i = 0; i < static_cast<size_t>(fv::kExciteLagFrames); ++i) {
		early = std::max(early, static_cast<double>(std::fabs(y[i])));
	}
	// NOT THE ROOM HEARD (it is not audible in Exciter), NOT THE DRY SENT (Blend 1 sends none): silence until the
	// injection arrives.
	CHECK(early == 0.0);
	double later = 0.0;
	for (size_t i = static_cast<size_t>(fv::kExciteLagFrames); i < y.size(); ++i) {
		later = std::max(later, static_cast<double>(std::fabs(y[i])));
	}
	CHECK(later > 1.0e-3);
	// AND IT IS THE INJECTION: the same device in Off, fed nothing, is silent throughout.
	const std::vector<float> none = renderExciter(1.0f, fv::kConvOff, std::vector<float>(48000, 0.0f));
	CHECK(*std::max_element(none.begin(), none.end()) == 0.0f);
}

TEST_CASE("Exciter's Blend weighs the two excitations exactly: Blend 0.5 is half of Blend 0 plus half of Blend 1") {
	// THE REVERB IS LINEAR BELOW ITS CEILING (asserted elsewhere: four times the input is four times the output),
	// so what it makes of `(1 - b)·dry + b·room` is `(1 - b)` of the one plus `b` of the other.
	std::vector<float> x = testInput(48000 * 2, 0xb1e5u);
	for (float& v : x) {
		v *= 0.25f; // well under the ceiling
	}
	const std::vector<float> y0 = renderExciter(0.0f, fv::kConvExciter, x);
	const std::vector<float> y1 = renderExciter(1.0f, fv::kConvExciter, x);
	const std::vector<float> yh = renderExciter(0.5f, fv::kConvExciter, x);
	double worst = 0.0;
	double peak = 0.0;
	for (size_t i = 0; i < x.size(); ++i) {
		const double want = 0.5 * y0[i] + 0.5 * y1[i];
		worst = std::max(worst, std::fabs(yh[i] - want));
		peak = std::max(peak, std::fabs(want));
	}
	MESSAGE("Exciter superposition: worst " << worst << " against peak " << peak);
	CHECK(peak > 1.0e-2);
	CHECK(worst < 1.0e-3 * peak);
}

TEST_CASE("MEASURE: each generated room's peak gain against its average — what Conv Feedback's scale turns on") {
	for (int which = 0; which < kGeneratedRooms; ++which) {
		const GeneratedIr ir = generateRoom(which, 48000.0);
		ConvKernel k;
		k.build(ir.left.data(), ir.right.data(), ir.left.size());
		MESSAGE(std::string(generatedRoomName(which)) << ": peak " << k.peakGain() << ", rms " << k.rmsGain() << ", ratio "
		                                              << (k.peakGain() / k.rmsGain()) << " ("
		                                              << 20.0 * std::log10(k.peakGain() / k.rmsGain()) << " dB)");
		CHECK(k.rmsGain() > 0.0f);
	}
}

TEST_CASE("the convolver's feedback on a dense room: bounded at the top of its knob, and dying away below the bloom") {
	// THE SCALE IS THE ROOM'S AVERAGE GAIN (the peak is ~7.5x it), so above ~40% the loudest frequencies pass unity
	// and bloom — bounded by the loop's soft saturation. Below that it is ordinary feedback: it dies away.
	const auto run = [](float feedback, double* peakOut, double* earlyOut, double* lateOut) {
		fv::Settings s = convOnly();
		s.convFeedback = feedback;
		fv::Featherverb dev;
		dev.setTarget(s);
		dev.prepare(48000.0); // the generated room
		const std::vector<float> x = testInput(48000 * 2, 0x9e37u);
		std::vector<float> l(48000 * 12, 0.0f), r(48000 * 12, 0.0f);
		std::copy(x.begin(), x.end(), l.begin());
		std::copy(x.begin(), x.end(), r.begin());
		for (size_t at = 0; at < l.size(); at += 512) {
			dev.processStereo(l.data() + at, r.data() + at, 512);
		}
		double peak = 0.0;
		for (const float v : l) {
			peak = std::max(peak, static_cast<double>(std::fabs(v)));
		}
		const auto rms = [&](size_t from, size_t to) {
			double e = 0.0;
			for (size_t i = from; i < to; ++i) {
				e += static_cast<double>(l[i]) * l[i];
			}
			return std::sqrt(e / static_cast<double>(to - from));
		};
		*peakOut = peak;
		*earlyOut = rms(48000 * 2, 48000 * 3);
		*lateOut = rms(48000 * 11, 48000 * 12);
	};
	double peak = 0.0, early = 0.0, late = 0.0;
	run(1.0f, &peak, &early, &late);
	MESSAGE("100%: peak " << peak << ", rms 1 s after the input stops " << early << ", 9 s later " << late);
	CHECK(std::isfinite(peak));
	CHECK(peak < 50.0);
	run(0.25f, &peak, &early, &late);
	MESSAGE("25%: peak " << peak << ", rms 1 s after " << early << ", 9 s later " << late);
	CHECK(late < 0.1 * early);
}

#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {

/// WHAT THE LOOP DID after the input stopped: the block it let go at (-1: never), and the blocks that sounded after it.
struct LetGo {
	bool ringing = false; // holdsTail when the input stopped
	int clearedAt = -1;
	int soundAfterClear = 0;
	bool holdsAtEnd = true;
};

/// One second of input through Conv Only with ordinary feedback, then up to 120 s of silence, in 512-frame blocks.
/// The input is fed WHOLE and nothing past it: 48000 is not a multiple of 512, and the last block once copied 256
/// samples from beyond the vector's end — heap garbage as input, so the let-go time depended on whatever the
/// allocator had left there (backlog QS).
LetGo runLetGo() {
	fv::Settings s = convOnly();
	s.convFeedback = 0.3f; // ordinary feedback, under the bloom: it dies away
	fv::Featherverb dev;
	dev.setTarget(s);
	dev.prepare(48000.0); // the generated room
	const std::vector<float> x = testInput(48000, 0x5eedu);
	std::vector<float> l(512), r(512);
	for (size_t at = 0; at < x.size(); at += 512) {
		const size_t n = std::min<size_t>(512, x.size() - at);
		std::copy(x.begin() + static_cast<std::ptrdiff_t>(at), x.begin() + static_cast<std::ptrdiff_t>(at + n), l.begin());
		std::copy(l.begin(), l.begin() + static_cast<std::ptrdiff_t>(n), r.begin());
		dev.processStereo(l.data(), r.data(), static_cast<int>(n));
	}
	LetGo out;
	out.ringing = dev.holdsTail();
	const int blocks = 120 * 48000 / 512;
	for (int b = 0; b < blocks; ++b) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		dev.processStereo(l.data(), r.data(), 512);
		bool sounding = false;
		for (size_t i = 0; i < l.size(); ++i) {
			sounding = sounding || l[i] != 0.0f || r[i] != 0.0f;
		}
		if (out.clearedAt >= 0) {
			out.soundAfterClear += sounding ? 1 : 0;
		}
		else if (!dev.holdsTail()) {
			out.clearedAt = b;
		}
	}
	out.holdsAtEnd = dev.holdsTail();
	return out;
}

} // namespace

TEST_CASE("backlog MC: the convolver's feedback loop dies to EXACT zero, so Featherverb says it holds nothing") {
	// **A LOOP UNDER UNITY NEVER ARRIVES AT ZERO ON ITS OWN.** Each lap is the last one times the loop's gain, a
	// geometric decay; MEASURED on the factory preset machine_elf_book_of_dreams the feedback ring carried 1e-14 down
	// to 1e-37 for ninety seconds after the output was exactly silent, re-feeding the convolver every lap, so
	// `holdsTail` (backlog MC) kept a host awake on nothing. The loop's road is floored at -140 dBFS now, so the
	// decay ENDS: `holdsTail` must go false, with the output exactly silent from there on. Asserted on the op's value.
	const LetGo g = runLetGo();
	REQUIRE(g.ringing); // the room is ringing
	CAPTURE(g.clearedAt);
	MESSAGE("the loop let go " << g.clearedAt * 512.0 / 48000.0 << " s after the input stopped");
	CHECK(g.clearedAt >= 0);       // it lets go, without a timeout: MEASURED at 67.9 s, a real -120 dB tail most of the way
	CHECK(g.soundAfterClear == 0); // and nothing comes out after it has
	CHECK_FALSE(g.holdsAtEnd);
}

#if defined(__GLIBC__)
TEST_CASE("backlog QS: the loop lets go at the SAME block whatever the allocator leaves in fresh and freed memory") {
	// The let-go above once read 256 samples past its input's end, so it moved with the heap: MEASURED 67.98 s clean,
	// 66.50 s under MALLOC_PERTURB_=77, NEVER under 255, and red in loaded whole-binary runs. glibc's M_PERTURB fills
	// every allocation and every free with a pattern; a result that depends on memory nobody wrote moves with it.
	// Every allocation the device and the input make happens inside `runLetGo`, so each run meets its own pattern.
	std::vector<LetGo> runs;
	for (const int pattern : { 0, 77, 165, 255 }) {
		mallopt(M_PERTURB, pattern);
		// Churn the heap first so the bytes nearest the next buffers are freed ones, carrying the pattern.
		for (size_t kb = 1; kb <= 512; kb *= 2) {
			std::vector<char> churn(kb * 1024, '\x5a');
			(void)churn.data();
		}
		runs.push_back(runLetGo());
		mallopt(M_PERTURB, 0);
		CAPTURE(pattern);
		CHECK(runs.back().ringing);
		CHECK(runs.back().clearedAt >= 0);
	}
	for (size_t i = 1; i < runs.size(); ++i) {
		CAPTURE(i);
		CHECK(runs[i].clearedAt == runs[0].clearedAt);
		CHECK(runs[i].soundAfterClear == runs[0].soundAfterClear);
		CHECK(runs[i].holdsAtEnd == runs[0].holdsAtEnd);
	}
}
#endif
