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

#include "mod/arp/theory.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <vector>

// THE PATTERN DEVICE'S THEORY, BY VALUE (terrain-arp design §9, §10). Expected keys are written out from the music
// (C major's ii7 is D F A C), and the voice-leading choices are checked against an independent brute force: the least
// total motion over EVERY pairing of tones, not the sorted pairing the code uses.

namespace th = feathers::mod::arp::theory;

namespace {

size_t scaleIndex(const char* name) {
	for (size_t i = 0; i < th::kScaleCount; ++i) {
		if (std::string(th::kScales[i].name) == name) {
			return i;
		}
	}
	FAIL("no scale " << name);
	return 0;
}

std::vector<int> keys(const th::Voicing& v) {
	return { v.keys.begin(), v.keys.begin() + v.count };
}

/// THE LEAST TOTAL MOTION over every pairing of two equal-sized voicings: the brute force `leadCost` must equal.
int bruteCost(const th::Voicing& a, const th::Voicing& b) {
	std::vector<int> perm(static_cast<size_t>(b.count));
	for (size_t i = 0; i < perm.size(); ++i) {
		perm[i] = static_cast<int>(i);
	}
	int best = std::numeric_limits<int>::max();
	do {
		int c = 0;
		for (int i = 0; i < a.count; ++i) {
			c += std::abs(a.keys[static_cast<size_t>(i)] - b.keys[static_cast<size_t>(perm[static_cast<size_t>(i)])]);
		}
		best = std::min(best, c);
	} while (std::next_permutation(perm.begin(), perm.end()));
	return best;
}

} // namespace

TEST_CASE("theory: every scale is a distinct set with its root, and its degrees climb through its own classes") {
	std::set<uint16_t> masks;
	std::set<std::string> names;
	for (size_t i = 0; i < th::kScaleCount; ++i) {
		const th::ScaleDef& d = th::kScales[i];
		CAPTURE(d.name);
		CHECK((d.mask & 1u) == 1u);
		CHECK(d.mask < 0x1000u);
		CHECK(masks.insert(d.mask).second);
		CHECK(names.insert(d.name).second);
		const th::Scale s = th::scaleOf(i);
		CHECK(s.count == std::popcount(static_cast<unsigned>(d.mask)));
		for (int root = 0; root < 12; ++root) {
			int last = std::numeric_limits<int>::min();
			for (int degree = -40; degree <= 40; ++degree) {
				const int k = th::keyOfDegree(s, 48 + root, degree);
				CHECK(k > last);
				last = k;
				const int pc = (((k - (48 + root)) % 12) + 12) % 12;
				CHECK(((d.mask >> pc) & 1u) == 1u);
				CHECK(th::keyOfDegree(s, 48 + root, degree + s.count) == k + 12);
				CHECK(th::degreeAtOrBelow(s, 48 + root, k) == degree);
			}
		}
	}
	CHECK(th::kScaleCount == 46);
}

TEST_CASE("theory: a key snaps to the nearest key of the scale, a tie snapping down") {
	for (size_t i = 0; i < th::kScaleCount; ++i) {
		const th::Scale s = th::scaleOf(i);
		const uint16_t mask = th::kScales[i].mask;
		for (int root = 0; root < 12; ++root) {
			for (int key = 0; key < 128; ++key) {
				// BRUTE FORCE: scan outward, down first, so a tie goes down.
				int want = key;
				for (int d = 0; d < 12; ++d) {
					const auto in = [&](int k) { return ((mask >> ((((k - root) % 12) + 12) % 12)) & 1u) == 1u; };
					if (in(key - d)) {
						want = key - d;
						break;
					}
					if (in(key + d)) {
						want = key + d;
						break;
					}
				}
				CAPTURE(th::kScales[i].name);
				CAPTURE(root);
				CAPTURE(key);
				CHECK(th::snapToScale(s, root, key) == want);
			}
		}
	}
}

TEST_CASE("theory: chords are stacked in the scale — C major's I and ii7, A harmonic minor's V7, a pentatonic's open triad") {
	const th::Scale major = th::scaleOf(scaleIndex("Major"));
	using CS = th::ChordSize;
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Triad, 0, 0, th::Spread::Close, 0)) == std::vector<int> { 60, 64, 67 });
	CHECK(keys(th::voiceChord(major, 60, 1, CS::Seventh, 0, 0, th::Spread::Close, 0))
	      == std::vector<int> { 62, 65, 69, 72 });
	CHECK(keys(th::voiceChord(major, 60, 4, CS::Sus4, 0, 0, th::Spread::Close, 0)) == std::vector<int> { 67, 72, 74 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Thirteenth, 0, 0, th::Spread::Close, 0))
	      == std::vector<int> { 60, 64, 67, 71, 74, 77, 81 });
	const th::Scale hm = th::scaleOf(scaleIndex("Harmonic Minor"));
	CHECK(keys(th::voiceChord(hm, 57, 4, CS::Seventh, 0, 0, th::Spread::Close, 0)) == std::vector<int> { 64, 68, 71, 74 });
	const th::Scale pent = th::scaleOf(scaleIndex("Minor Pentatonic"));
	CHECK(keys(th::voiceChord(pent, 57, 0, CS::Triad, 0, 0, th::Spread::Close, 0)) == std::vector<int> { 57, 62, 67 });
	// THINNING: the 5th first, then the root; a 7th thins to its shell (3rd and 7th), a triad no further than a dyad.
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Seventh, 1, 0, th::Spread::Close, 0)) == std::vector<int> { 60, 64, 71 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Seventh, 2, 0, th::Spread::Close, 0)) == std::vector<int> { 64, 71 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Triad, 2, 0, th::Spread::Close, 0)) == std::vector<int> { 60, 64 });
}

TEST_CASE("theory: inversions raise the lowest tones; Drop 2, Drop 3, Open and Wide open a close voicing") {
	const th::Scale major = th::scaleOf(scaleIndex("Major"));
	using CS = th::ChordSize;
	using SP = th::Spread;
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Triad, 0, 1, SP::Close, 0)) == std::vector<int> { 64, 67, 72 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Triad, 0, 2, SP::Close, 0)) == std::vector<int> { 67, 72, 76 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Triad, 0, 3, SP::Close, 0)) == std::vector<int> { 60, 64, 67 });
	// C maj7 close 60 64 67 71
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Seventh, 0, 0, SP::Drop2, 0)) == std::vector<int> { 55, 60, 64, 71 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Seventh, 0, 0, SP::Drop3, 0)) == std::vector<int> { 52, 60, 67, 71 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Seventh, 0, 0, SP::Drop24, 0)) == std::vector<int> { 48, 55, 64, 71 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Seventh, 0, 0, SP::Open, 0)) == std::vector<int> { 60, 67, 76, 83 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Seventh, 0, 0, SP::Wide, 0)) == std::vector<int> { 48, 67, 76, 83 });
	CHECK(keys(th::voiceChord(major, 60, 0, CS::Triad, 0, 0, SP::Close, -1)) == std::vector<int> { 48, 52, 55 });
}

TEST_CASE("theory: leadCost with as many tones is the least motion over every pairing") {
	const th::Scale major = th::scaleOf(0);
	for (int a = 0; a < 7; ++a) {
		for (int b = 0; b < 7; ++b) {
			for (int inv = 0; inv < 4; ++inv) {
				for (int sp = 0; sp < 6; ++sp) {
					const th::Voicing x = th::voiceChord(major, 60, a, th::ChordSize::Seventh, 0, inv, static_cast<th::Spread>(sp), 0);
					const th::Voicing y = th::voiceChord(major, 60, b, th::ChordSize::Seventh, 0, 0, th::Spread::Close, 0);
					CHECK(th::leadCost(x, y) == bruteCost(x, y));
				}
			}
		}
	}
}

TEST_CASE("theory: a step is voiced as the least motion inside the window — checked against every candidate by brute force") {
	const th::Scale minor = th::scaleOf(scaleIndex("Minor"));
	const th::Window w { 48, 84 };
	th::Voicing prev = th::voiceChord(minor, 57, 0, th::ChordSize::Seventh, 0, 0, th::Spread::Close, 0);
	for (int degree = -3; degree < 10; ++degree) {
		th::ChordSpec spec;
		spec.degree = degree;
		spec.size = th::ChordSize::Seventh;
		spec.inversion = th::kAuto;
		const th::Voicing got = th::voiceStep(minor, 57, spec, w, prev, 0);
		// BRUTE FORCE: every inversion at every octave, inside the window; least motion over every pairing.
		int best = std::numeric_limits<int>::max();
		for (int inv = 0; inv < 4; ++inv) {
			for (int oct = -5; oct <= 5; ++oct) {
				const th::Voicing c = th::voiceChord(minor, 57, degree, th::ChordSize::Seventh, 0, inv, th::Spread::Close, oct);
				if (c.keys[0] >= w.low && c.keys[3] <= w.high) {
					best = std::min(best, bruteCost(c, prev));
				}
			}
		}
		CAPTURE(degree);
		CHECK(got.keys[0] >= w.low);
		CHECK(got.keys[3] <= w.high);
		CHECK(bruteCost(got, prev) == best);
		prev = got;
	}
}

TEST_CASE("theory: a cycle is voiced until it wraps, so every step — the first too — is led from the one before it") {
	const th::Scale major = th::scaleOf(0);
	const th::Window w { 48, 84 };
	const int degrees[] = { 0, 5, 3, 4, 0, 1, 4, 6 };
	std::array<th::ChordSpec, 8> specs {};
	std::array<const th::ChordSpec*, 9> ptr {};
	for (size_t i = 0; i < 8; ++i) {
		specs[i].degree = degrees[i];
		specs[i].size = th::ChordSize::Seventh;
		specs[i].inversion = th::kAuto;
		ptr[i] = &specs[i];
	}
	ptr[8] = nullptr; // a step with no chord is skipped by the chain
	std::array<th::Voicing, 9> out {};
	th::voiceCycle(major, 60, ptr.data(), 9, w, 0, out.data());
	CHECK(out[8].count == 0);
	for (size_t i = 0; i < 8; ++i) {
		const th::Voicing& before = out[(i + 7) % 8];
		CAPTURE(i);
		CHECK(out[i] == th::voiceStep(major, 60, specs[i], w, before, 0));
	}
	// AND IT IS A FUNCTION OF ITS INPUTS: the same call again gives the same voicings.
	std::array<th::Voicing, 9> again {};
	th::voiceCycle(major, 60, ptr.data(), 9, w, 0, again.data());
	for (size_t i = 0; i < 9; ++i) {
		CHECK(again[i] == out[i]);
	}
}

TEST_CASE("theory: two or more held notes pull a chord to the neighbouring degree that holds them, and into a voicing that does") {
	const th::Scale major = th::scaleOf(0);
	const auto pc = [](std::initializer_list<int> classes) {
		th::PitchClasses s = 0;
		for (const int c : classes) {
			s = static_cast<th::PitchClasses>(s | (1u << c));
		}
		return s;
	};
	th::ChordSpec spec;
	spec.size = th::ChordSize::Triad;
	spec.inversion = th::kAuto;
	// ii (D F A) with E and G held: I (C E G) and iii (E G B) both hold two; the lower wins the neighbours' tie.
	spec.degree = 1;
	CHECK(th::pullDegree(major, 60, spec, pc({ 4, 7 })) == 0);
	// V (G B D) with F and A held: IV (F A C) holds both, vi (A C E) one.
	spec.degree = 4;
	CHECK(th::pullDegree(major, 60, spec, pc({ 5, 9 })) == 3);
	// The field's own degree keeps a tie: I (C E G) with C and G held — I holds both, so does nothing better.
	spec.degree = 0;
	CHECK(th::pullDegree(major, 60, spec, pc({ 0, 7 })) == 0);
	// ONE held class pulls nothing.
	spec.degree = 1;
	CHECK(th::pullDegree(major, 60, spec, pc({ 4 })) == 1);
	// THE VOICING: a C triad with E held in a window that admits several placements keeps E (it always does, as every
	// C triad holds E) — so test a Size where tones are dropped: a thinned 7th (shell 3rd + 7th) with B held holds B.
	spec.degree = 0;
	spec.size = th::ChordSize::Seventh;
	spec.thin = 2;
	const th::Voicing v = th::voiceStep(major, 60, spec, th::Window { 48, 84 }, th::Voicing {}, pc({ 11, 4 }));
	CHECK(th::countIn(th::pitchClassesOf(v), pc({ 11, 4 })) == 2);
}

TEST_CASE("theory: across many progressions the cycle settles — its first step is led from its last") {
	// MEASURED, not assumed: the passes stop when one ends where it began. Over a spread of progressions, sizes, spreads
	// and lengths, count how many settle (the wrap then holds exactly); the bound is that all of them do.
	const th::Window w { 48, 84 };
	int settled = 0;
	int total = 0;
	uint64_t h = 0x9E3779B97F4A7C15ull;
	const auto next = [&]() {
		h ^= h >> 12;
		h ^= h << 25;
		h ^= h >> 27;
		return h * 0x2545F4914F6CDD1Dull;
	};
	for (int trial = 0; trial < 2000; ++trial) {
		const th::Scale s = th::scaleOf(next() % th::kScaleCount);
		const size_t length = 2 + next() % 15;
		std::array<th::ChordSpec, 16> specs {};
		std::array<const th::ChordSpec*, 16> ptr {};
		const auto size = static_cast<th::ChordSize>(1 + next() % 10);
		const auto spread = static_cast<th::Spread>(next() % 6);
		for (size_t i = 0; i < length; ++i) {
			specs[i].degree = static_cast<int>(next() % 7);
			specs[i].size = size;
			specs[i].spread = spread;
			specs[i].inversion = th::kAuto;
			ptr[i] = &specs[i];
		}
		std::array<th::Voicing, 16> out {};
		th::voiceCycle(s, 60, ptr.data(), length, w, 0, out.data());
		++total;
		if (out[0] == th::voiceStep(s, 60, specs[0], w, out[length - 1], 0)) {
			++settled;
		}
	}
	MESSAGE("settled " << settled << " of " << total);
	CHECK(settled == total);
}

namespace {

/// A SPREAD OF RANDOM CYCLES (the settling case's generator), each handed to `visit(scale, specs, length)`.
template <typename Visit>
void randomCycles(int trials, Visit visit) {
	uint64_t h = 0xD1B54A32D192ED03ull;
	const auto next = [&]() {
		h ^= h >> 12;
		h ^= h << 25;
		h ^= h >> 27;
		return h * 0x2545F4914F6CDD1Dull;
	};
	for (int trial = 0; trial < trials; ++trial) {
		const th::Scale s = th::scaleOf(next() % th::kScaleCount);
		const size_t length = 2 + next() % 15;
		std::array<th::ChordSpec, 16> specs {};
		const auto size = static_cast<th::ChordSize>(1 + next() % 10);
		const auto spread = static_cast<th::Spread>(next() % 6);
		const int inversion = next() % 3 == 0 ? static_cast<int>(next() % 3) : th::kAuto;
		for (size_t i = 0; i < length; ++i) {
			specs[i].degree = static_cast<int>(next() % 7);
			specs[i].size = size;
			specs[i].spread = spread;
			specs[i].inversion = inversion;
		}
		visit(s, specs, length);
	}
}

/// THE CYCLE AT ONE VOICE LEADING, and its total motion (each chord from the one before, the wrap included).
std::array<th::Voicing, 16> voicedAt(const th::Scale& s, std::array<th::ChordSpec, 16> specs, size_t length, float leading) {
	std::array<const th::ChordSpec*, 16> ptr {};
	for (size_t i = 0; i < length; ++i) {
		specs[i].leading = leading;
		ptr[i] = &specs[i];
	}
	std::array<th::Voicing, 16> out {};
	th::voiceCycle(s, 60, ptr.data(), length, th::Window { 48, 84 }, 0, out.data());
	return out;
}
long motionOf(const std::array<th::Voicing, 16>& v, size_t length) {
	long m = 0;
	for (size_t i = 0; i < length; ++i) {
		m += th::leadCost(v[i], v[(i + length - 1) % length]);
	}
	return m;
}

} // namespace

TEST_CASE("theory: VOICE LEADING 0 voices every chord as it would stand alone, whatever came before it") {
	// Progressions mode has a Voice Leading field. At 0, block chords: each the
	// voicing a chord with nothing before it gets (`voiceStep` from an empty reference, the window's centre).
	const th::Window w { 48, 84 };
	int chords = 0;
	randomCycles(1000, [&](const th::Scale& s, const std::array<th::ChordSpec, 16>& specs, size_t length) {
		const auto out = voicedAt(s, specs, length, 0.0f);
		for (size_t i = 0; i < length; ++i) {
			th::ChordSpec alone = specs[i];
			alone.leading = 1.0f; // irrelevant from an empty reference: the two terms are the same cost
			CHECK(out[i] == th::voiceStep(s, 60, alone, w, th::Voicing {}, 0));
			++chords;
		}
	});
	CHECK(chords > 5000);
}

TEST_CASE("theory: VOICE LEADING 1 is the least motion exactly — the voicing every chord had before the pin, chosen over every "
          "candidate by brute force") {
	// The default `ChordSpec::leading` IS 1, so every other case in this file is this one too; here it is said outright,
	// against an independent restatement: from the same chord before, no candidate the window holds moves less.
	const th::Window w { 48, 84 };
	randomCycles(400, [&](const th::Scale& s, const std::array<th::ChordSpec, 16>& specs, size_t length) {
		const auto out = voicedAt(s, specs, length, 1.0f);
		for (size_t i = 0; i < length; ++i) {
			const th::Voicing& prev = out[(i + length - 1) % length];
			const int chosen = th::leadCost(out[i], prev);
			// EVERY CANDIDATE: each inversion (or the fixed one) at every octave whose keys all sit in the window.
			std::array<int, th::kMaxTones> degrees {};
			const int n = static_cast<int>(th::chordDegrees(specs[i].size, specs[i].thin, degrees.data()));
			const int first = specs[i].inversion == th::kAuto ? 0 : specs[i].inversion;
			const int last = specs[i].inversion == th::kAuto ? n - 1 : specs[i].inversion;
			bool anyInside = false;
			int least = std::numeric_limits<int>::max();
			for (int inv = first; inv <= last; ++inv) {
				const th::Voicing base = th::voiceChord(s, 60, specs[i].degree, specs[i].size, 0, inv, specs[i].spread, 0);
				for (int oct = -6; oct <= 6; ++oct) {
					th::Voicing c = base;
					for (int t = 0; t < c.count; ++t) {
						c.keys[static_cast<size_t>(t)] = static_cast<int16_t>(c.keys[static_cast<size_t>(t)] + 12 * oct);
					}
					if (c.count > 0 && c.keys[0] >= w.low && c.keys[static_cast<size_t>(c.count - 1)] <= w.high) {
						anyInside = true;
						least = std::min(least, th::leadCost(c, prev));
					}
				}
			}
			if (anyInside) {
				CHECK(chosen == least);
			}
		}
	});
}

TEST_CASE("theory: VOICE LEADING MORPHS — from a fixed chord before, the motion it chooses never rises as the value rises; over "
          "whole cycles the total motion falls from 0 to 1; the same inputs give the same voicings") {
	const th::Window w { 48, 84 };
	constexpr int kSweep = 21;
	// ONE STEP, ONE REFERENCE: a parametric minimum of L·a + (1 − L)·b is monotone in L, so this holds without exception.
	int steps = 0;
	int moved = 0;
	randomCycles(600, [&](const th::Scale& s, const std::array<th::ChordSpec, 16>& specs, size_t length) {
		const auto led = voicedAt(s, specs, length, 1.0f);
		for (size_t i = 0; i < length; ++i) {
			const th::Voicing& prev = led[(i + length - 1) % length];
			int before = std::numeric_limits<int>::max();
			int atZero = 0;
			for (int j = 0; j < kSweep; ++j) {
				th::ChordSpec spec = specs[i];
				spec.leading = static_cast<float>(j) / static_cast<float>(kSweep - 1);
				const int motion = th::leadCost(th::voiceStep(s, 60, spec, w, prev, 0), prev);
				CAPTURE(j);
				CHECK(motion <= before);
				before = motion;
				atZero = j == 0 ? motion : atZero;
			}
			moved += before < atZero ? 1 : 0;
			++steps;
		}
	});
	MESSAGE("steps whose motion the sweep lowered: " << moved << " of " << steps);
	CHECK(moved > steps / 20); // non-vacuity: the value does something
	// WHOLE CYCLES ARE MEASURED, NOT PROMISED. Each chord is led from a chord that itself moves with the value, and the
	// greedy least motion can walk a cycle toward a window edge it must then jump back from — so a strictly led cycle CAN
	// move more in total than the block chords (MEASURED below; the per-step law above is the guarantee). What is asserted
	// is the shape: led cycles move less than unled ones far more often than the reverse, and the voicing is a pure function.
	int rose = 0;
	int ledLess = 0;
	int ledMore = 0;
	int cycles = 0;
	randomCycles(600, [&](const th::Scale& s, const std::array<th::ChordSpec, 16>& specs, size_t length) {
		long before = std::numeric_limits<long>::max();
		bool everRose = false;
		for (int j = 0; j < kSweep; ++j) {
			const long m = motionOf(voicedAt(s, specs, length, static_cast<float>(j) / (kSweep - 1)), length);
			everRose = everRose || m > before;
			before = m;
		}
		const long led = motionOf(voicedAt(s, specs, length, 1.0f), length);
		const long unled = motionOf(voicedAt(s, specs, length, 0.0f), length);
		ledLess += led < unled ? 1 : 0;
		ledMore += led > unled ? 1 : 0;
		rose += everRose ? 1 : 0;
		++cycles;
		// DETERMINISM: the same inputs, the same voicings, at any value.
		CHECK(voicedAt(s, specs, length, 0.37f) == voicedAt(s, specs, length, 0.37f));
	});
	MESSAGE("whole cycles: led moves less than unled in " << ledLess << ", more in " << ledMore << " of " << cycles
	                                                      << "; total motion rose somewhere in the sweep in " << rose);
	CHECK(ledLess > 4 * ledMore);
}
