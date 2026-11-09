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

#include "mod/arp/theory.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace feathers::mod::arp::theory {

namespace {
/// Floor division, so negative degrees walk down octaves rather than toward zero.
int floorDiv(int a, int b) noexcept {
	const int q = a / b;
	return (a % b != 0 && ((a < 0) != (b < 0))) ? q - 1 : q;
}
} // namespace

Scale scaleOf(size_t index) noexcept {
	const uint16_t mask = kScales[std::min(index, kScaleCount - 1)].mask;
	Scale s;
	for (int pc = 0; pc < 12; ++pc) {
		if ((mask >> pc) & 1u) {
			s.steps[static_cast<size_t>(s.count++)] = static_cast<int8_t>(pc);
		}
	}
	return s;
}

int keyOfDegree(const Scale& s, int rootKey, int degree) noexcept {
	const int q = floorDiv(degree, s.count);
	const int r = degree - q * s.count;
	return rootKey + 12 * q + s.steps[static_cast<size_t>(r)];
}

int degreeAtOrBelow(const Scale& s, int rootKey, int key) noexcept {
	const int rel = key - rootKey;
	const int q = floorDiv(rel, 12);
	const int pc = rel - 12 * q;
	int r = 0;
	for (int i = 0; i < s.count; ++i) {
		if (s.steps[static_cast<size_t>(i)] <= pc) {
			r = i;
		}
	}
	return q * s.count + r;
}

int snapToScale(const Scale& s, int rootKey, int key) noexcept {
	const int d = degreeAtOrBelow(s, rootKey, key);
	const int lo = keyOfDegree(s, rootKey, d);
	const int hi = keyOfDegree(s, rootKey, d + 1);
	return key - lo <= hi - key ? lo : hi;
}

size_t chordDegrees(ChordSize size, int thin, int* out) noexcept {
	static constexpr int kTable[][kMaxTones + 1] = {
		// count, then the degrees above the chord's own
		{ 1, 0 },                       // Single
		{ 2, 0, 4 },                    // Power
		{ 3, 0, 2, 4 },                 // Triad
		{ 3, 0, 1, 4 },                 // Sus2
		{ 3, 0, 3, 4 },                 // Sus4
		{ 4, 0, 2, 4, 5 },              // 6th
		{ 4, 0, 2, 4, 6 },              // 7th
		{ 4, 0, 2, 4, 8 },              // add9
		{ 5, 0, 2, 4, 6, 8 },           // 9th
		{ 6, 0, 2, 4, 6, 8, 10 },       // 11th
		{ 7, 0, 2, 4, 6, 8, 10, 12 },   // 13th
	};
	const auto& row = kTable[std::min(static_cast<size_t>(size), static_cast<size_t>(ChordSize::kCount) - 1)];
	size_t n = 0;
	for (int i = 0; i < row[0]; ++i) {
		out[n++] = row[i + 1];
	}
	const auto drop = [&](int degree) {
		for (size_t i = 0; i < n; ++i) {
			if (out[i] == degree) {
				std::copy(out + i + 1, out + n, out + i);
				--n;
				return;
			}
		}
	};
	// THINNING (the Size gene): the 5th goes first, then the root — and never below three tones' worth of a chord, so
	// a triad thins to a dyad (root and 3rd) and a 7th to a shell (3rd and 7th).
	if (thin >= 1 && n >= 3) {
		drop(4);
	}
	if (thin >= 2 && n >= 3) {
		drop(0);
	}
	return n;
}

Voicing voiceChord(const Scale& s, int rootKey, int degree, ChordSize size, int thin, int inversion, Spread spread,
                   int octaves) noexcept {
	std::array<int, kMaxTones> degrees {};
	const size_t n = chordDegrees(size, thin, degrees.data());
	Voicing v;
	v.count = static_cast<int>(n);
	std::array<int, 2 * kMaxTones> k {};
	for (size_t i = 0; i < n; ++i) {
		k[i] = keyOfDegree(s, rootKey, degree + degrees[i]);
	}
	// AN INSERTION SORT OF AT MOST SEVEN: `std::sort` on a prefix of a fixed array draws g++-14's -Warray-bounds (it
	// cannot see `n <= kMaxTones`), and seven keys gain nothing from a heap.
	const auto sortKeys = [&]() {
		for (size_t i = 1; i < n && i < k.size(); ++i) {
			const int key = k[i];
			size_t j = i;
			for (; j > 0 && k[j - 1] > key; --j) {
				k[j] = k[j - 1];
			}
			k[j] = key;
		}
	};
	sortKeys();
	const int ni = static_cast<int>(n);
	const int inv = ((inversion % ni) + ni) % ni;
	for (int i = 0; i < inv; ++i) {
		k[static_cast<size_t>(i)] += 12;
	}
	sortKeys();
	switch (spread) {
	case Spread::Close: break;
	case Spread::Drop2:
		if (ni >= 3) {
			k[static_cast<size_t>(ni - 2)] -= 12;
		}
		break;
	case Spread::Drop3:
		if (ni >= 4) {
			k[static_cast<size_t>(ni - 3)] -= 12;
		}
		break;
	case Spread::Drop24:
		if (ni >= 4) {
			k[static_cast<size_t>(ni - 2)] -= 12;
			k[static_cast<size_t>(ni - 4)] -= 12;
		}
		break;
	case Spread::Open:
	case Spread::Wide:
		for (int i = 1; i < ni; i += 2) {
			k[static_cast<size_t>(i)] += 12;
		}
		if (spread == Spread::Wide && ni >= 2) {
			k[0] -= 12;
		}
		break;
	case Spread::kCount: break;
	}
	sortKeys();
	for (size_t i = 0; i < n; ++i) {
		v.keys[i] = static_cast<int16_t>(k[i] + 12 * octaves);
	}
	return v;
}

int leadCost(const Voicing& a, const Voicing& prev) noexcept {
	if (a.count == 0 || prev.count == 0) {
		return 0;
	}
	int cost = 0;
	if (a.count == prev.count) {
		for (int i = 0; i < a.count; ++i) {
			cost += std::abs(a.keys[static_cast<size_t>(i)] - prev.keys[static_cast<size_t>(i)]);
		}
		return cost;
	}
	const auto nearest = [](int key, const Voicing& in) {
		int best = std::numeric_limits<int>::max();
		for (int i = 0; i < in.count; ++i) {
			best = std::min(best, std::abs(key - in.keys[static_cast<size_t>(i)]));
		}
		return best;
	};
	for (int i = 0; i < a.count; ++i) {
		cost += nearest(a.keys[static_cast<size_t>(i)], prev);
	}
	for (int i = 0; i < prev.count; ++i) {
		cost += nearest(prev.keys[static_cast<size_t>(i)], a);
	}
	return cost;
}

PitchClasses pitchClassesOf(const Voicing& v) noexcept {
	PitchClasses set = 0;
	for (int i = 0; i < v.count; ++i) {
		const int pc = ((v.keys[static_cast<size_t>(i)] % 12) + 12) % 12;
		set = static_cast<PitchClasses>(set | (1u << pc));
	}
	return set;
}

int countIn(PitchClasses set, PitchClasses of) noexcept {
	int n = 0;
	for (int pc = 0; pc < 12; ++pc) {
		if ((set >> pc) & (of >> pc) & 1u) {
			++n;
		}
	}
	return n;
}

int pullDegree(const Scale& s, int rootKey, const ChordSpec& spec, PitchClasses held) noexcept {
	if (countIn(held, held) < 2 || spec.scale >= 0) { // a chord from outside the scale keeps its degree
		return spec.degree;
	}
	const auto holds = [&](int degree) {
		return countIn(pitchClassesOf(voiceChord(s, rootKey, degree, spec.size, spec.thin, 0, Spread::Close, 0)), held);
	};
	// THE FIELD'S OWN DEGREE FIRST, so it keeps a tie; then the lower neighbour before the upper.
	int best = spec.degree;
	int bestHolds = holds(spec.degree);
	for (const int d : { spec.degree - 1, spec.degree + 1 }) {
		const int h = holds(d);
		if (h > bestHolds) {
			best = d;
			bestHolds = h;
		}
	}
	return best;
}

Voicing voiceStep(const Scale& s, int rootKey, const ChordSpec& spec, const Window& w, const Voicing& prev,
                  PitchClasses held) noexcept {
	struct Candidate {
		Voicing v;
		double score = 0.0;
		long sum = 0;
		int inversion = 0;
		bool inside = false;
		long offCentre = 0;
	};
	// THE CENTRE, the window's middle as one key: what a chord with nothing before it is measured against — and what an
	// unled chord (Voice Leading 0) is always measured against.
	Voicing centre {};
	centre.count = 1;
	centre.keys[0] = static_cast<int16_t>((w.low + w.high) / 2);
	const Voicing reference = prev.count == 0 ? centre : prev;
	// VOICE LEADING (`ChordSpec::leading`): 1 is exactly the old score, so the centre term is not even computed there.
	const double lead = std::clamp(static_cast<double>(spec.leading), 0.0, 1.0);
	std::array<int, kMaxTones> degrees {};
	const int n = static_cast<int>(chordDegrees(spec.size, spec.thin, degrees.data()));
	const int firstInv = spec.inversion == kAuto ? 0 : spec.inversion;
	const int lastInv = spec.inversion == kAuto ? n - 1 : spec.inversion;
	std::array<Candidate, kMaxTones * 11> c {};
	size_t count = 0;
	bool anyInside = false;
	// ONE VOICING PER INVERSION, THEN ONLY THE OCTAVES THAT MATTER: every placement that fits the window, or — when none
	// fits — the three nearest its centre. MEASURED (test_arp_pattern's cost case): eleven fixed octaves per inversion
	// made a 64-step cycle of 13ths cost ~1.3 ms a build, nearly all of it placements no rule could choose.
	for (int inv = firstInv; inv <= lastInv; ++inv) {
		const Voicing base = spec.scale >= 0 ? voiceChord(scaleOf(static_cast<size_t>(spec.scale)), rootKey + spec.tonic, spec.degree,
		                                                  spec.size, spec.thin, inv, spec.spread, 0)
		                                     : voiceChord(s, rootKey, spec.degree, spec.size, spec.thin, inv, spec.spread, 0);
		if (base.count == 0) {
			continue;
		}
		const int lowKey = base.keys[0];
		const int highKey = base.keys[static_cast<size_t>(base.count - 1)];
		int first = -floorDiv(lowKey - w.low, 12);           // the lowest octave whose bottom is in the window
		int last = floorDiv(w.high - highKey, 12);           // the highest whose top is
		if (first > last) {
			long sum = 0;
			for (int i = 0; i < base.count; ++i) {
				sum += base.keys[static_cast<size_t>(i)];
			}
			const double mean = static_cast<double>(sum) / base.count;
			const int nearest = static_cast<int>(std::lround((0.5 * (w.low + w.high) - mean) / 12.0));
			first = nearest - 1;
			last = nearest + 1;
		}
		for (int oct = first; oct <= last && count < c.size(); ++oct) {
			Candidate& k = c[count++];
			k.v = base;
			for (int i = 0; i < k.v.count; ++i) {
				k.v.keys[static_cast<size_t>(i)] = static_cast<int16_t>(k.v.keys[static_cast<size_t>(i)] + 12 * oct);
			}
			k.inversion = inv;
			k.inside = k.v.keys[0] >= w.low && k.v.keys[static_cast<size_t>(k.v.count - 1)] <= w.high;
			anyInside = anyInside || k.inside;
			for (int i = 0; i < k.v.count; ++i) {
				k.sum += k.v.keys[static_cast<size_t>(i)];
			}
			const double led = static_cast<double>(leadCost(k.v, reference));
			const double unled = lead < 1.0 ? static_cast<double>(leadCost(k.v, centre)) : 0.0;
			k.score = lead * led + (1.0 - lead) * unled
			          - static_cast<double>(kHeldWeight) * countIn(pitchClassesOf(k.v), held);
			const long centre2 = static_cast<long>(w.low + w.high) * k.v.count;
			k.offCentre = std::labs(2 * k.sum - centre2);
		}
	}
	const auto better = [&](const Candidate& a, const Candidate& b) {
		if (!anyInside) {
			if (a.offCentre != b.offCentre) {
				return a.offCentre < b.offCentre;
			}
		}
		if (a.score != b.score) {
			return a.score < b.score;
		}
		if (a.sum != b.sum) {
			return a.sum < b.sum;
		}
		return a.inversion < b.inversion;
	};
	const Candidate* best = nullptr;
	const Candidate* second = nullptr;
	for (size_t i = 0; i < count; ++i) {
		const Candidate& k = c[i];
		if (anyInside && !k.inside) {
			continue;
		}
		if (best == nullptr || better(k, *best)) {
			if (best != nullptr && !(best->v == k.v)) {
				second = best;
			}
			best = &k;
		}
		else if (!(k.v == best->v) && (second == nullptr || better(k, *second))) {
			second = &k;
		}
	}
	if (best == nullptr) {
		return Voicing {};
	}
	return spec.second && second != nullptr ? second->v : best->v;
}

void voiceCycle(const Scale& s, int rootKey, const ChordSpec* const* specs, size_t length, const Window& w,
                PitchClasses held, Voicing* out) noexcept {
	length = std::min(length, kMaxCycle);
	// PASS 1 LEADS FROM NOTHING; EACH LATER PASS FROM THE LAST CHORD OF THE PASS BEFORE, until a pass ends on the chord it
	// started from — then the first step is led from the last one exactly, and the wrap is as smooth as any other step.
	// MEASURED (test_arp_theory): two passes were NOT enough — the second pass's last chord can differ from the first's,
	// so its first step had been led from a chord that no longer plays. Bounded, so a cycle that never settles still
	// ends, deterministically, on its last pass.
	Voicing prev {};
	for (int pass = 0; pass < kMaxPasses; ++pass) {
		const Voicing start = prev;
		for (size_t i = 0; i < length; ++i) {
			if (specs[i] == nullptr) {
				out[i] = Voicing {};
				continue;
			}
			ChordSpec spec = *specs[i];
			spec.degree = pullDegree(s, rootKey, spec, held);
			out[i] = voiceStep(s, rootKey, spec, w, prev, held);
			prev = out[i];
		}
		if (pass > 0 && prev == start) {
			return;
		}
	}
}

} // namespace feathers::mod::arp::theory
