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

// THE LOOKAHEAD LIMITER (`dsp/lookahead_limiter.h`, backlog LD and LE), as values: the ceiling never passed, the gain's
// step and change of slope inside the bounds its construction promises, a pure delay to the bit below the ceiling, the
// release's time constant as asked, the stereo link, and no allocation outside `prepare`.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "test_heap.h"
#include "doctest.h"

#include "dsp/lookahead_limiter.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

namespace {

std::atomic<long> gNews { 0 };

/// OUT OF LINE, as `test_conv.cpp` has it: g++-14 -O3 inlines a malloc straight into a replaced `operator new` and then
/// raises a false -Werror=mismatched-new-delete against the free below.
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

namespace {

using feathers::dsp::LookaheadLimiter;

constexpr double kRate = 48000.0;

LookaheadLimiter made(size_t lookahead, float ceiling, bool link, double release = 0.1, size_t maxLookahead = 0) {
	LookaheadLimiter lim;
	lim.prepare(kRate, maxLookahead == 0 ? lookahead : maxLookahead);
	lim.setParams({ .ceiling = ceiling, .lookahead = lookahead, .releaseSeconds = release, .link = link });
	return lim;
}

/// White noise in [-amp, amp), from a fixed stream.
std::vector<float> noise(size_t n, float amp, uint32_t seed) {
	std::vector<float> x(n);
	for (float& v : x) {
		seed = seed * 1664525u + 1013904223u;
		v = amp * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
	}
	return x;
}

/// LOUD MATERIAL THAT ATTACKS EVERY WAY: noise at 3x, a 100 Hz sine stepping between 0.5x and 10x the ceiling (a
/// 26 dB jump in one sample), lone spikes at 50x, and quiet stretches between so the release runs.
std::vector<float> hard(size_t n, float ceiling, uint32_t seed) {
	std::vector<float> x = noise(n, 3.0f * ceiling, seed);
	for (size_t i = 0; i < n; ++i) {
		const size_t seg = (i / 4800) % 4;
		if (seg == 1) {
			const float a = (i / 600) % 2 == 0 ? 0.5f : 10.0f;
			x[i] = a * ceiling * static_cast<float>(std::sin(6.283185307179586 * 100.0 * static_cast<double>(i) / kRate));
		}
		else if (seg == 2) {
			x[i] = i % 997 == 0 ? 50.0f * ceiling : 0.01f * x[i];
		}
		else if (seg == 3) {
			x[i] *= 0.1f;
		}
	}
	return x;
}

void run(LookaheadLimiter& lim, std::vector<float>& l, std::vector<float>& r, size_t block) {
	for (size_t at = 0; at < l.size(); at += block) {
		lim.process(l.data() + at, r.data() + at, static_cast<int>(std::min(block, l.size() - at)));
	}
}

} // namespace

TEST_CASE("the lookahead limiter below its ceiling is a pure delay, to the bit: fresh, after limiting, linked or not") {
	for (const size_t la : { size_t { 1 }, size_t { 16 }, size_t { 72 }, size_t { 144 } }) {
		for (const bool link : { true, false }) {
			CAPTURE(la);
			CAPTURE(link);
			// FRESH: zeros for the lookahead, then the input.
			LookaheadLimiter lim = made(la, 1.0f, link);
			const std::vector<float> x = noise(20000, 0.99f, 0x1234u);
			const std::vector<float> y = noise(20000, 0.5f, 0x4321u);
			std::vector<float> l = x, r = y;
			run(lim, l, r, 100);
			long firstBad = -1;
			for (size_t i = 0; i < x.size() && firstBad < 0; ++i) {
				const float wl = i < la ? 0.0f : x[i - la];
				const float wr = i < la ? 0.0f : y[i - la];
				firstBad = l[i] != wl || r[i] != wr ? static_cast<long>(i) : -1;
			}
			CHECK(firstBad == -1);
			CHECK(lim.eventsForTest().limited == 0);
			// AFTER LIMITING: a loud second, then 0.99 for three — once the 100 ms release has taken the envelope back
			// under the ceiling, the output is the input delayed again, EXACTLY (the gain is 1, not 0.99999994).
			std::vector<float> z = noise(4 * 48000, 0.99f, 0x5555u);
			for (size_t i = 0; i < 48000; ++i) {
				z[i] *= 4.0f;
			}
			std::vector<float> zl = z, zr = z;
			run(lim, zl, zr, 64);
			CHECK(lim.eventsForTest().limited > 1000); // not vacuous
			firstBad = -1;
			for (size_t i = 2 * 48000; i < z.size() && firstBad < 0; ++i) {
				firstBad = zl[i] != z[i - la] || zr[i] != z[i - la] ? static_cast<long>(i) : -1;
			}
			CHECK(firstBad == -1);
		}
	}
}

TEST_CASE("the lookahead limiter's held peak is the window's maximum, exactly, every sample — against a brute-force scan") {
	// THE RUNNING MAXIMUM IS A MONOTONE QUEUE in a ring, and a ring one slot short lets a push overwrite the front (the
	// maximum) — which no output-level case caught: a first draft did exactly that on a falling sine and passed all of
	// them, until a change of indexing turned the silent overwrite into a crash. So the queue is read by value here.
	for (const size_t la : { size_t { 1 }, size_t { 2 }, size_t { 16 }, size_t { 72 } }) {
		for (const bool link : { true, false }) {
			CAPTURE(la);
			CAPTURE(link);
			LookaheadLimiter lim = made(la, 1.0f, link);
			const std::vector<float> x = hard(48000, 1.0f, 0x21u);
			const std::vector<float> y = hard(48000, 1.0f, 0x12u);
			long firstBad = -1;
			for (size_t i = 0; i < x.size() && firstBad < 0; ++i) {
				float l = x[i];
				float r = y[i];
				lim.process(&l, &r, 1);
				float want = 0.0f;
				for (size_t j = i >= la ? i - la : 0; j <= i; ++j) {
					want = std::max(want, link ? std::max(std::fabs(x[j]), std::fabs(y[j])) : std::fabs(x[j]));
				}
				firstBad = lim.heldForTest(0) != want ? static_cast<long>(i) : -1;
			}
			CHECK(firstBad == -1);
		}
	}
}

TEST_CASE("the lookahead limiter never passes its ceiling: noise, a 26 dB step, lone spikes, any lookahead, linked or not") {
	for (const float ceiling : { 1.0f, 0.25f }) {
		for (const size_t la : { size_t { 1 }, size_t { 16 }, size_t { 72 }, size_t { 144 } }) {
			for (const bool link : { true, false }) {
				CAPTURE(ceiling);
				CAPTURE(la);
				CAPTURE(link);
				LookaheadLimiter lim = made(la, ceiling, link);
				std::vector<float> l = hard(96000, ceiling, 0x77u);
				std::vector<float> r = hard(96000, ceiling, 0x99u);
				run(lim, l, r, 37);
				float peak = 0.0f;
				for (size_t i = 0; i < l.size(); ++i) {
					peak = std::max({ peak, std::fabs(l[i]), std::fabs(r[i]) });
				}
				CHECK(peak <= ceiling);
				CHECK(peak > 0.9f * ceiling); // not vacuously far under it
			}
		}
	}
}

TEST_CASE("the lookahead limiter's gain is a ramp: its step at most 1/boxA, its change of slope at most 2/(boxA·boxB)") {
	// READ BY VALUE, one sample at a time (`gainForTest`), and checked against the output too: the sample out is the
	// sample in, `lookahead` earlier, times that gain (or the ceiling, where the last clamp caught an ulp).
	for (const size_t la : { size_t { 16 }, size_t { 72 }, size_t { 144 } }) {
		for (const bool link : { true, false }) {
			CAPTURE(la);
			CAPTURE(link);
			LookaheadLimiter lim = made(la, 1.0f, link);
			const double stepBound = 1.0 / static_cast<double>(lim.boxA());
			const double bendBound = 2.0 / static_cast<double>(lim.boxA() * lim.boxB());
			CHECK(lim.boxA() + lim.boxB() == la + 2); // the two boxes together span exactly the hold
			const std::vector<float> x = hard(96000, 1.0f, 0x31u);
			const std::vector<float> y = hard(96000, 1.0f, 0x13u);
			double worstStep = 0.0;
			double worstBend = 0.0;
			double worstProduct = 0.0;
			double worstOver = -1.0;
			double prev[2] = { 1.0, 1.0 };
			double prevStep[2] = { 0.0, 0.0 };
			uint64_t ramping = 0;
			for (size_t i = 0; i < x.size(); ++i) {
				float l = x[i];
				float r = y[i];
				lim.process(&l, &r, 1);
				for (int ch = 0; ch < 2; ++ch) {
					const double g = lim.gainForTest(ch);
					const double step = g - prev[ch];
					worstStep = std::max(worstStep, std::fabs(step));
					worstBend = std::max(worstBend, std::fabs(step - prevStep[ch]));
					ramping += step != 0.0 ? 1 : 0;
					prevStep[ch] = step;
					prev[ch] = g;
					if (i >= la) {
						const double in = ch == 0 ? x[i - la] : y[i - la];
						const double out = ch == 0 ? l : r;
						const double want = std::clamp(static_cast<double>(static_cast<float>(in) * static_cast<float>(g)), -1.0, 1.0);
						worstProduct = std::max(worstProduct, std::fabs(out - want));
						// THE GAIN DOES THE WORK, the last clamp only catches rounding: unclamped, in · g is at the
						// ceiling to a few ulps — or the "never above it" case would be the clamp's guarantee alone.
						worstOver = std::max(worstOver, std::fabs(in * g) - 1.0);
					}
				}
			}
			CAPTURE(worstStep);
			CAPTURE(worstBend);
			CHECK(ramping > 1000); // not vacuous: the gain moved, a lot
			CHECK(worstStep <= stepBound + 1.0e-6);
			CHECK(worstBend <= bendBound + 1.0e-6);
			CHECK(worstProduct == 0.0); // the gain read is the gain applied
			CAPTURE(worstOver);
			CHECK(worstOver < 1.0e-6);
			CHECK(worstOver > -1.0e-3); // and it does reach the ceiling: not vacuously far under
		}
	}
}

TEST_CASE("the lookahead limiter's release is the time constant asked for: the envelope falls by e per release time") {
	// ONE SPIKE of 100x the ceiling, then silence. The envelope holds it for the lookahead and the spike's own sample,
	// then falls as `peak · (1 - k)^m` with `k = 1 - exp(-1 / (release · rate))` — so ONE RELEASE TIME after the hold it
	// is the peak over e, and two after, over e². Read by value, against that closed form.
	for (const double release : { 0.02, 0.1, 0.5 }) {
		CAPTURE(release);
		const size_t la = 72;
		LookaheadLimiter lim = made(la, 1.0f, true, release);
		const size_t tau = static_cast<size_t>(std::lround(release * kRate));
		std::vector<double> env;
		for (size_t i = 0; i < la + 2 * tau + 10; ++i) {
			float l = i == 0 ? 100.0f : 0.0f;
			float r = l;
			lim.process(&l, &r, 1);
			env.push_back(lim.envelopeForTest(0));
		}
		CHECK(env[0] == 100.0);
		CHECK(env[la] == 100.0); // held through the lookahead
		CHECK(env[la + 1] < 100.0); // and falling the sample after
		CHECK(env[la + tau] == doctest::Approx(100.0 * std::exp(-1.0)).epsilon(1.0e-3));
		CHECK(env[la + 2 * tau] == doctest::Approx(100.0 * std::exp(-2.0)).epsilon(1.0e-3));
	}
}

TEST_CASE("the lookahead limiter's link: one gain for both channels; unlinked, a loud left leaves the right a pure delay") {
	const size_t la = 72;
	const std::vector<float> loud = noise(48000, 4.0f, 0xabcu);
	const std::vector<float> quiet = noise(48000, 0.5f, 0xcbau);
	{
		LookaheadLimiter lim = made(la, 1.0f, false);
		std::vector<float> l = loud, r = quiet;
		run(lim, l, r, 128);
		bool rightExact = true;
		for (size_t i = la; i < quiet.size(); ++i) {
			rightExact = rightExact && r[i] == quiet[i - la];
		}
		CHECK(rightExact);
		CHECK(lim.eventsForTest().limited > 1000);
	}
	{
		LookaheadLimiter lim = made(la, 1.0f, true);
		float worstRatioGap = 0.0f;
		uint64_t pulledDown = 0;
		for (size_t i = 0; i < loud.size(); ++i) {
			float l = loud[i];
			float r = quiet[i];
			lim.process(&l, &r, 1);
			worstRatioGap = std::max(worstRatioGap, std::fabs(lim.gainForTest(0) - lim.gainForTest(1)));
			pulledDown += i >= la && std::fabs(r) < std::fabs(quiet[i - la]) ? 1 : 0;
		}
		CHECK(worstRatioGap == 0.0f);
		CHECK(pulledDown > 1000); // the quiet right follows the loud left down
	}
}

TEST_CASE("the lookahead limiter allocates only in prepare: process and a change of lookahead allocate nothing") {
	LookaheadLimiter lim;
	lim.prepare(kRate, 256);
	std::vector<float> l = hard(48000, 1.0f, 0x1u), r = hard(48000, 1.0f, 0x2u);
	const long before = gNews.load();
	for (const size_t la : { size_t { 256 }, size_t { 1 }, size_t { 72 }, size_t { 999 } }) {
		lim.setParams({ .ceiling = 1.0f, .lookahead = la, .releaseSeconds = 0.05, .link = la % 2 == 0 });
		for (size_t at = 0; at < l.size(); at += 512) {
			lim.process(l.data() + at, r.data() + at, static_cast<int>(std::min<size_t>(512, l.size() - at)));
		}
	}
	CHECK(gNews.load() == before);
	CHECK(lim.lookahead() == 256); // 999 is clamped to what `prepare` sized for
}

TEST_CASE("the lookahead limiter on a mono road (left and right the same buffer) is the left channel, limited") {
	LookaheadLimiter a = made(72, 1.0f, true);
	LookaheadLimiter b = made(72, 1.0f, true);
	std::vector<float> x = hard(48000, 1.0f, 0x5u);
	std::vector<float> mono = x, l = x, r = x;
	run(a, mono, mono, 100);
	run(b, l, r, 100);
	CHECK(mono == l);
}

namespace {

/// FNV-1a over the bits of both channels of a render through `process` at Featherverb's settings (ceiling 1, 72
/// samples, 100 ms, linked or not): seeded noise stepping between 0.5 and 3.0 every 125 ms, in uneven blocks.
uint64_t processHash(bool link) {
	feathers::dsp::LookaheadLimiter lim;
	lim.prepare(48000.0, 72);
	lim.setParams({ .ceiling = 1.0f, .lookahead = 72, .releaseSeconds = 0.1, .link = link });
	uint32_t s = 0x2545f491u;
	const auto noise = [&s] {
		s ^= s << 13;
		s ^= s >> 17;
		s ^= s << 5;
		return static_cast<float>(static_cast<double>(s) / 2147483647.5 - 1.0);
	};
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&h](float v) {
		uint32_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		for (int k = 0; k < 4; ++k) {
			h ^= (bits >> (8 * k)) & 0xffu;
			h *= 1099511628211ull;
		}
	};
	constexpr int kBlocks[] = { 64, 13, 256, 1, 100 };
	int t = 0;
	for (int n = 0; t < 96000; ++n) {
		const int frames = kBlocks[n % 5];
		std::vector<float> l(static_cast<size_t>(frames)), r(static_cast<size_t>(frames));
		for (size_t i = 0; i < l.size(); ++i, ++t) {
			const float env = (t / 6000) % 2 == 1 ? 3.0f : 0.5f;
			l[i] = env * noise();
			r[i] = env * 0.5f * noise();
		}
		lim.process(l.data(), r.data(), frames);
		for (size_t i = 0; i < l.size(); ++i) {
			mix(l[i]);
			mix(r[i]);
		}
	}
	return h;
}

} // namespace

TEST_CASE("LE: `process` is bit-identical to the block before LE, so Featherverb's wet limiter is too") {
	// BACKLOG LE added `processSidechain` and the opt-in `releaseInDecibels`, and moved `process` onto the loop they
	// share. THE HASHES WERE TAKEN FROM THE BLOCK AT v1-implementation deae20fe (its header compiled alone, g++-14
	// -O2 -ffp-contract=off), the build before LE: the claim is "unchanged to the bit" for the one road Featherverb
	// uses (`featherverb.cpp`: `process`, linked, at its own constants, none of which LE touched).
	const uint64_t linked = processHash(true);
	const uint64_t unlinked = processHash(false);
	MESSAGE("linked " << linked << " unlinked " << unlinked);
	CHECK(linked == 10332942447482285412ull);
	CHECK(unlinked == 1334431537010085452ull);
}
