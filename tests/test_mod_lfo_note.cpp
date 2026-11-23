/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// THE LFO'S NOTE TIME BASE: the wave runs at the played note's frequency times Rate (a ratio), at the control tick's
// rate, clamped to a quarter of it. This binary replaces global operator new to count allocations on the tick road.

#include "test_heap.h"
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "mod/lfo/lfo.h"
#include "mod/lfo/lfo_types.h"
#include "mod/per_note.h"

#include "device/registry.h"
#include "device/tick_window.h"

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <new>
#include <memory>
#include <string>
#include <vector>

namespace {
std::atomic<long> gNews { 0 };

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
void operator delete(void* p) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p) noexcept { feathers::test::heapFree(p); }
void operator delete(void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
void operator delete[](void* p, std::size_t) noexcept { feathers::test::heapFree(p); }
#include "nothrow_new_forwarding.h"
#pragma GCC diagnostic pop

using namespace feathers;
using namespace feathers::mod::lfo;

namespace {

constexpr double kSampleRate = 48000.0;

/// A KEY'S FREQUENCY, restated from the platform's contract (MIDI: A4 = key 69 = 440 Hz), not from our own helper.
double keyHz(int key) {
	return 440.0 * std::pow(2.0, (key - 69) / 12.0);
}

Settings noteSine(double ratio) {
	Settings s;
	s.wave = Waveform::Sine;
	s.bipolar = true;
	s.mode = feathers::mod::Mode::Free;
	s.timeBase = kNoteSyncBase;
	s.timeScale = static_cast<float>(std::log2(ratio));
	s.phase = 0.1f; // off the zero crossings: a quarter-rate wave samples 0, 1, 0, -1 from phase 0
	return s;
}

/// THE FREQUENCY OF THE OUTPUT ITSELF — rising zero crossings of the published value per second, over `seconds`.
double measuredHz(Shape& shape, const Settings& s, double seconds) {
	const double tickSeconds = static_cast<double>(feathers::mod::kBlockSamples) / kSampleRate;
	const int ticks = static_cast<int>(seconds / tickSeconds);
	int crossings = 0;
	float prev = 0.0f;
	for (int k = 0; k < ticks; ++k) {
		shape.tick(s, false, false, feathers::mod::TransportClock {});
		const float v = shape.value();
		if (k > 0 && prev < 0.0f && v >= 0.0f) {
			++crossings;
		}
		prev = v;
	}
	return crossings / (static_cast<double>(ticks) * tickSeconds);
}

} // namespace

TEST_CASE("lfo Note: the wave runs at the played note's frequency times Rate, measured from the output") {
	REQUIRE(std::string(kTimeBaseNames[kNoteSyncBase]) == "Note");
	constexpr int kA1 = 33; // 55 Hz
	for (const double ratio : { 1.0, 2.0, 3.0 }) {
		const Settings s = noteSine(ratio);
		Shape shape;
		shape.prepare(kSampleRate);
		shape.setNotePitch(static_cast<float>(kA1));
		const double hz = measuredHz(shape, s, 8.0);
		CHECK(hz == doctest::Approx(keyHz(kA1) * ratio).epsilon(0.01));
		// AND THE QUANTITY, exactly: the frequency the tick ran the wave at.
		CHECK(shape.effectiveHzForTest() == doctest::Approx(keyHz(kA1) * ratio).epsilon(1e-6));
	}
	SUBCASE("a different note is a different frequency") {
		const Settings s = noteSine(1.0);
		Shape shape;
		shape.prepare(kSampleRate);
		shape.setNotePitch(45.0f); // A2, 110 Hz
		CHECK(measuredHz(shape, s, 8.0) == doctest::Approx(110.0).epsilon(0.01));
		CHECK(shape.effectiveHzForTest() == doctest::Approx(110.0).epsilon(1e-6));
	}
}

TEST_CASE("lfo Note: before any note the wave sits on C3, and it keeps the note it last had") {
	const Settings s = noteSine(1.0);
	Shape shape;
	shape.prepare(kSampleRate);
	shape.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(130.8128).epsilon(1e-4)); // MIDI 48
	shape.setNotePitch(52.0f); // E3
	shape.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(keyHz(52)).epsilon(1e-6));
	shape.reset(); // a release or a reset does not forget the note
	shape.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(keyHz(52)).epsilon(1e-6));
}

TEST_CASE("lfo Note: nothing caps the frequency — a ratio of 8 on A1 runs at 440 Hz, rateMul counts in it") {
	const Settings s = noteSine(8.0);
	Shape shape;
	shape.prepare(kSampleRate);
	shape.setNotePitch(33.0f); // 55 Hz x 8
	shape.tick(s, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(440.0).epsilon(1e-6));
	shape.tick(noteSine(1.0), false, false, feathers::mod::TransportClock {}, 2.0f);
	CHECK(shape.effectiveHzForTest() == doctest::Approx(110.0).epsilon(1e-6));
}

TEST_CASE("lfo Note: the other bases are untouched by the note") {
	Settings hz = noteSine(1.0);
	hz.timeBase = 0; // Hz: Rate is a frequency, 1X = 1 Hz
	Shape shape;
	shape.prepare(kSampleRate);
	shape.setNotePitch(80.0f);
	shape.tick(hz, false, false, feathers::mod::TransportClock {});
	CHECK(shape.effectiveHzForTest() == doctest::Approx(1.0).epsilon(1e-6));
	CHECK_FALSE(hz.synced());
	Settings div = hz;
	div.timeBase = 13; // 1/4
	CHECK(div.synced());
	CHECK_FALSE(div.noteTracked());
}

TEST_CASE("lfo Note under Poly: every voice's wave runs at its own note; the shared wave at the host's last") {
	using Poly = feathers::mod::PolyShape<Shape, Settings>;
	Poly poly;
	poly.prepare(kSampleRate);
	poly.settings = noteSine(1.0);
	poly.settings.mode = feathers::mod::Mode::LfoRetrig;
	poly.settings.poly = true;
	REQUIRE(poly.poly());
	const device::NoteEvent in[2] = {
		{ device::NoteEvent::Kind::On, 0, 1, 0, 0, 33, 1.0 },
		{ device::NoteEvent::Kind::On, 0, 2, 0, 0, 45, 1.0 },
	};
	device::NoteEvent out[4] {};
	size_t made = 0;
	poly.processNotes(in, 2, out, 4, &made, false, [](Shape& shape, const feathers::mod::PerNoteTable<Shape>::Note*) {
		shape.reset();
	});
	REQUIRE(made == 2);
	device::ModTickContext ctx {};
	ctx.gate = true;
	ctx.notePitch = 52.0f; // the host's last note: E3, 164.81 Hz
	poly.tick(ctx);
	CHECK(poly.shapeForNote(1, 0, 33).effectiveHzForTest() == doctest::Approx(55.0).epsilon(1e-6));
	CHECK(poly.shapeForNote(2, 0, 45).effectiveHzForTest() == doctest::Approx(110.0).epsilon(1e-6));
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(keyHz(52)).epsilon(1e-6));
}

TEST_CASE("lfo Note: the shared wave follows ModTickContext::notePitch when Poly is off") {
	using Poly = feathers::mod::PolyShape<Shape, Settings>;
	Poly poly;
	poly.prepare(kSampleRate);
	poly.settings = noteSine(1.0);
	device::ModTickContext ctx {};
	CHECK(ctx.notePitch == 48.0f); // the contract's default is C3
	poly.tick(ctx);
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(keyHz(48)).epsilon(1e-6));
	ctx.notePitch = 69.0f;
	poly.tick(ctx);
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(440.0).epsilon(1e-6));
	ctx.notePitch = 33.0f;
	poly.tick(ctx);
	CHECK(poly.mono.effectiveHzForTest() == doctest::Approx(keyHz(33)).epsilon(1e-6));
}

TEST_CASE("lfo Note: the tick road allocates nothing") {
	using Poly = feathers::mod::PolyShape<Shape, Settings>;
	auto poly = std::make_unique<Poly>();
	poly->prepare(kSampleRate);
	poly->settings = noteSine(1.0);
	poly->settings.mode = feathers::mod::Mode::LfoRetrig;
	poly->settings.poly = true;
	const device::NoteEvent in[1] = { { device::NoteEvent::Kind::On, 0, 1, 0, 0, 40, 1.0 } };
	device::NoteEvent out[2] {};
	size_t made = 0;
	device::ModTickContext ctx {};
	ctx.gate = true;
	const long before = gNews.load();
	poly->processNotes(in, 1, out, 2, &made, false, [](Shape& shape, const feathers::mod::PerNoteTable<Shape>::Note*) {
		shape.reset();
	});
	for (int k = 0; k < 2000; ++k) {
		ctx.notePitch = static_cast<float>(40 + k % 20);
		poly->tick(ctx);
	}
	CHECK(gNews.load() == before);
}

TEST_CASE("lfo Note: the base and the ratio survive save and load") {
	Settings s = noteSine(3.0);
	const Row row = rowOf(s);
	const Settings back = settingsOf(row, row.size());
	CHECK(back.noteTracked());
	CHECK(back.timeBase == kNoteSyncBase);
	CHECK(back.timeScale == doctest::Approx(std::log2(3.0f)));
	// The base is stored by its label: the Choice's own name table says "Note" at the stored index.
	const auto& desc = kParamDescs[static_cast<size_t>(Param::TimeBase)];
	CHECK(std::string(desc.names[static_cast<size_t>(row[static_cast<size_t>(Param::TimeBase)])]) == "Note");
}

//------------------------------------------------------------------------------------------------//
// THE TICK AS A CURVE                                                                            //
//------------------------------------------------------------------------------------------------//

#include <complex>
#include <numbers>

namespace {

/// THE PER-SAMPLE STREAM `ticks` consecutive windows make, or the ramp's where a window is declined.
std::vector<float> windowStream(Shape& shape, const Settings& s, int ticks, bool* allRendered = nullptr) {
	std::vector<float> out;
	out.reserve(static_cast<size_t>(ticks) * 64);
	float last = 0.0f;
	bool all = true;
	for (int t = 0; t < ticks; ++t) {
		shape.tick(s, false, false, feathers::mod::TransportClock {});
		float w[feathers::device::kTickWindowStride] {};
		if (shape.renderWindow(false, w)) {
			out.insert(out.end(), w, w + 64);
		}
		else {
			all = false;
			const float next = shape.value();
			for (int i = 0; i < 64; ++i) {
				out.push_back(last + (next - last) * static_cast<float>(i + 1) / 64.0f);
			}
		}
		last = out.back();
	}
	if (allRendered != nullptr) {
		*allRendered = all;
	}
	return out;
}

/// A GOERTZEL SCAN for the strongest frequency in [lo, hi] Hz at `step` resolution, Hann windowed.
double peakHz(const std::vector<float>& x, double lo, double hi, double step) {
	double bestHz = lo, best = -1.0;
	const size_t n = x.size();
	for (double f = lo; f <= hi; f += step) {
		const double w = 2.0 * std::numbers::pi * f / kSampleRate;
		std::complex<double> acc = 0.0;
		for (size_t i = 0; i < n; ++i) {
			const double hann = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
			acc += static_cast<double>(x[i]) * hann * std::polar(1.0, -w * static_cast<double>(i));
		}
		if (std::abs(acc) > best) {
			best = std::abs(acc);
			bestHz = f;
		}
	}
	return bestHz;
}

void fft(std::vector<std::complex<double>>& a) {
	const size_t n = a.size();
	for (size_t i = 1, j = 0; i < n; ++i) {
		size_t bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(a[i], a[j]);
		}
	}
	for (size_t len = 2; len <= n; len <<= 1) {
		const std::complex<double> wl = std::polar(1.0, -2.0 * std::numbers::pi / static_cast<double>(len));
		for (size_t i = 0; i < n; i += len) {
			std::complex<double> w = 1.0;
			for (size_t k = 0; k < len / 2; ++k) {
				const auto u = a[i + k], v = a[i + k + len / 2] * w;
				a[i + k] = u + v;
				a[i + k + len / 2] = u - v;
				w *= wl;
			}
		}
	}
}

/// THE STRONGEST COMPONENT THAT IS NOT A HARMONIC OF `f0` (folded into 0..fs/2), in dB below the fundamental, over the
/// Hann-windowed spectrum of `x` (a power of two long). Bins within 10 of a harmonic are leakage, not alias.
double spurDb(const std::vector<float>& x, double f0) {
	const size_t n = x.size();
	std::vector<std::complex<double>> a(n);
	double mean = 0.0;
	for (const float v : x) {
		mean += static_cast<double>(v);
	}
	mean /= static_cast<double>(n);
	for (size_t i = 0; i < n; ++i) {
		const double ph = 2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n);
		const double hann = 0.35875 - 0.48829 * std::cos(ph) + 0.14128 * std::cos(2.0 * ph) - 0.01168 * std::cos(3.0 * ph); // Blackman-Harris
		a[i] = (static_cast<double>(x[i]) - mean) * hann;
	}
	fft(a);
	const double binHz = kSampleRate / static_cast<double>(n);
	std::vector<bool> harmonic(n / 2, false);
	for (int h = 1; h * f0 < kSampleRate / 2.0; ++h) { // the TRUE harmonics only: an alias is a spur
		double f = std::fmod(h * f0, kSampleRate);
		if (f > kSampleRate / 2.0) {
			f = kSampleRate - f;
		}
		const long centre = std::lround(f / binHz);
		for (long b = centre - 10; b <= centre + 10; ++b) {
			if (b >= 0 && static_cast<size_t>(b) < n / 2) {
				harmonic[static_cast<size_t>(b)] = true;
			}
		}
	}
	const long fund = std::lround(f0 / binHz);
	double fundamental = 0.0, spur = 0.0;
	for (size_t b = 8; b < n / 2; ++b) { // past the window's DC lobe
		const double m = std::abs(a[b]);
		if (std::labs(static_cast<long>(b) - fund) <= 4) {
			fundamental = std::max(fundamental, m);
		}
		else if (!harmonic[b]) {
			spur = std::max(spur, m);
		}
	}
	return 20.0 * std::log10(spur / fundamental + 1e-30);
}

} // namespace

TEST_CASE("lfo window: A3 at ratio 1 is 220 Hz and at ratio 2 is 440 Hz, measured from the per-sample window, past the tick's own Nyquist") {
	for (const double ratio : { 1.0, 2.0 }) {
		const Settings s = noteSine(ratio);
		Shape shape;
		shape.prepare(kSampleRate);
		shape.setNotePitch(57.0f); // A3
		bool all = false;
		const std::vector<float> x = windowStream(shape, s, 750, &all); // one second
		CHECK(all); // every tick rendered its curve, none fell back to the ramp
		const double want = 220.0 * ratio;
		CHECK(peakHz(x, want * 0.9, want * 1.1, want * 0.0005) == doctest::Approx(want).epsilon(0.005));
	}
}

TEST_CASE("lfo window: the shaped waves leave no alias above a stated floor, and the corners beat reading the wave at the samples") {
	// THE METHOD: polyBLEP at a jump and polyBLAMP at a turn of slope, two samples wide. MEASURED, the strongest
	// spur that is no harmonic of the note (Blackman-Harris, 16384 samples, folded aliases counted), against the same wave
	// read point by point at the same sample times. The floors below are those numbers with margin.
	struct Row {
		Waveform wave;
		int key;
		double floorDb; ///< the window must stay under this, in dB below the fundamental
		double gainDb;  ///< and be at least this much cleaner than the point-sampled wave
	};
	const Row rows[] = {
		{ Waveform::Square, 57, -45.0, 5.0 },   // 220 Hz: measured -49.4 against -41.3
		{ Waveform::Square, 84, -32.0, 5.0 },   // 1046 Hz: measured -35.2 against -27.3
		{ Waveform::Triangle, 84, -58.0, 5.0 }, // measured -62.4 against -54.6
		{ Waveform::Sine, 84, -90.0, -1.0 },    // no corners: as clean as the wave itself, measured -104
	};
	for (const Row& row : rows) {
		Settings s = noteSine(1.0);
		s.wave = row.wave;
		Shape shape;
		shape.prepare(kSampleRate);
		shape.setNotePitch(static_cast<float>(row.key));
		const double f0 = keyHz(row.key);
		const std::vector<float> curve = windowStream(shape, s, 256);
		std::vector<float> naive;
		for (size_t i = 0; i < curve.size(); ++i) {
			const double ph = static_cast<double>(s.phase) + f0 * static_cast<double>(i + 1) / kSampleRate;
			naive.push_back(eval(s, static_cast<float>(ph - std::floor(ph))));
		}
		const double window = spurDb(curve, f0);
		const double point = spurDb(naive, f0);
		CAPTURE(static_cast<int>(row.wave));
		CAPTURE(row.key);
		CHECK(window < row.floorDb);
		CHECK(point - window > row.gainDb);
		MESSAGE("wave ", static_cast<int>(row.wave), " key ", row.key, ": window ", window, " dB, point-sampled ", point, " dB");
	}
}

TEST_CASE("lfo window: at an ordinary rate the ramp stands untouched; at 20 Hz the curve strays by the amount the maths says") {
	Settings s;
	s.wave = Waveform::Sine;
	s.mode = feathers::mod::Mode::Free;
	s.timeBase = 0;
	s.timeScale = 0.0f; // Hz base: 1X is 1 Hz
	Shape shape;
	shape.prepare(kSampleRate);
	float w[feathers::device::kTickWindowStride] {};
	for (int t = 0; t < 40; ++t) {
		shape.tick(s, false, false, feathers::mod::TransportClock {});
		CHECK_FALSE(shape.renderWindow(false, w)); // 1 Hz: the curve is within 1e-4 of the ramp: nothing changes
	}
	// 5 Hz (timeScale log2 5 is within the Hz base's 10 Hz ceiling) strays past the bound: the curve is rendered.
	s.timeScale = static_cast<float>(std::log2(5.0));
	Shape five;
	five.prepare(kSampleRate);
	int rendered = 0;
	for (int t = 0; t < 150; ++t) {
		five.tick(s, false, false, feathers::mod::TransportClock {});
		rendered += five.renderWindow(false, w) ? 1 : 0;
	}
	CHECK(rendered > 0);
	CHECK(rendered < 150); // and not every tick: where the sine is nearly straight the ramp stands
	// THE SCALE OF THE CHANGE at 5 Hz, restated from the second derivative: |f''| T^2 / 8 for a unipolar sine (amplitude 0.5)
	// at its steepest curvature, T the tick. Measured as the curve's worst distance from the chord the ramp draws.
	double worst = 0.0;
	Shape check;
	check.prepare(kSampleRate);
	float prevValue = 0.0f;
	for (int t = 0; t < 400; ++t) {
		check.tick(s, false, false, feathers::mod::TransportClock {});
		if (check.renderWindow(false, w) && t > 0) {
			for (int i = 0; i < 64; ++i) {
				const float ramp = prevValue + (check.value() - prevValue) * static_cast<float>(i + 1) / 64.0f;
				worst = std::max(worst, static_cast<double>(std::abs(w[i] - ramp)));
			}
		}
		prevValue = check.value();
	}
	const double tick = 64.0 / kSampleRate;
	const double expected = 0.5 * std::pow(2.0 * std::numbers::pi * 5.0, 2.0) * tick * tick / 8.0;
	CHECK(worst == doctest::Approx(expected).epsilon(0.25));
	MESSAGE("5 Hz sine: worst curve-vs-ramp ", worst, " of full scale (predicted ", expected, ")");
}

TEST_CASE("lfo window: Poly gives each note's own curve, and the right ear leads by Stereo Phase") {
	using Poly = feathers::mod::PolyShape<Shape, Settings>;
	Poly poly;
	poly.prepare(kSampleRate);
	poly.settings = noteSine(1.0);
	poly.settings.mode = feathers::mod::Mode::LfoRetrig;
	poly.settings.poly = true;
	poly.settings.stereoPhase = static_cast<float>(std::numbers::pi / 2.0); // a quarter cycle
	const device::NoteEvent in[2] = {
		{ device::NoteEvent::Kind::On, 0, 1, 0, 0, 57, 1.0 }, // 220 Hz
		{ device::NoteEvent::Kind::On, 0, 2, 0, 0, 69, 1.0 }, // 440 Hz
	};
	device::NoteEvent out[4] {};
	size_t made = 0;
	poly.processNotes(in, 2, out, 4, &made, false, [](Shape& shape, const feathers::mod::PerNoteTable<Shape>::Note*) { shape.reset(); });
	device::ModTickContext ctx {};
	ctx.gate = true;
	std::vector<float> a, b, aRight;
	for (int t = 0; t < 750; ++t) {
		poly.tick(ctx);
		float w[feathers::device::kTickWindowStride] {};
		REQUIRE(poly.shapeForNote(1, 0, 57).renderWindow(false, w));
		a.insert(a.end(), w, w + 64);
		REQUIRE(poly.shapeForNote(1, 0, 57).renderWindow(true, w));
		aRight.insert(aRight.end(), w, w + 64);
		REQUIRE(poly.shapeForNote(2, 0, 69).renderWindow(false, w));
		b.insert(b.end(), w, w + 64);
	}
	CHECK(peakHz(a, 198.0, 242.0, 0.1) == doctest::Approx(220.0).epsilon(0.005));
	CHECK(peakHz(b, 396.0, 484.0, 0.2) == doctest::Approx(440.0).epsilon(0.005));
	// THE RIGHT EAR IS THE LEFT, A QUARTER CYCLE AHEAD: a sine's right ear is the left's derivative (cosine in phase).
	double dot = 0.0, norm = 0.0;
	for (size_t i = 4096; i < a.size() - 4096; ++i) {
		const double r = aRight[i] - 0.5;
		const double ahead = a[i + 1] - a[i - 1]; // the slope: a quarter cycle ahead of the value, for a sine
		dot += r * ahead;
		norm += std::abs(r * ahead);
	}
	CHECK(dot / norm > 0.95);
}

TEST_CASE("lfo window: smoothing runs per sample, and no window allocates") {
	Settings s = noteSine(1.0);
	s.smoothing = -8.0f; // 4 ms
	Shape shape;
	shape.prepare(kSampleRate);
	shape.setNotePitch(81.0f); // 880 Hz
	std::vector<float> x(64 * 400);
	const long before = gNews.load();
	float w[feathers::device::kTickWindowStride] {};
	for (int t = 0; t < 400; ++t) {
		shape.tick(s, false, false, feathers::mod::TransportClock {});
		REQUIRE(shape.renderWindow(false, w));
		std::copy(w, w + 64, x.begin() + t * 64);
	}
	CHECK(gNews.load() == before);
	// A ONE-POLE of tau 4 ms on an 880 Hz sine: gain 1/sqrt(1 + (2 pi f tau)^2), restated.
	const double tau = std::exp2(-8.0);
	const double gain = 1.0 / std::sqrt(1.0 + std::pow(2.0 * std::numbers::pi * 880.0 * tau, 2.0));
	double lo = 1.0, hi = -1.0;
	for (size_t i = x.size() / 2; i < x.size(); ++i) {
		lo = std::min(lo, static_cast<double>(x[i]));
		hi = std::max(hi, static_cast<double>(x[i]));
	}
	CHECK((hi - lo) / 2.0 == doctest::Approx(gain).epsilon(0.03));
}

