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

#include "mod/arp/pattern.h"

#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>

// THE PATTERN BUILDER, BY VALUE (terrain-arp design §5, §8–§11). Every expectation is a fact about the KEYS a step
// strikes — their pitch classes, their intervals, their kinds — read off the built cycle, over many places and every
// Mode, rather than a fixed pattern pinned (the pinned patterns are the audition's to set).

namespace pat = feathers::mod::arp::pattern;
namespace genes = feathers::mod::arp::genes;
namespace th = feathers::mod::arp::theory;
namespace arp = feathers::mod::arp;
using Kind = pat::PatternStep::Kind;
using genes::Mode;
using genes::Pin;

namespace {

/// THIS THREAD'S CPU time in us (CLOCK_THREAD_CPUTIME_ID): a load-independent cost, unlike a wall-clock bound (backlog PL).
double threadCpuUs() {
	timespec t {};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return static_cast<double>(t.tv_sec) * 1e6 + static_cast<double>(t.tv_nsec) * 1e-3;
}

struct Dice {
	uint64_t h = 0x2545F4914F6CDD1Dull;
	double unit() {
		h ^= h >> 12;
		h ^= h << 25;
		h ^= h >> 27;
		return static_cast<double>((h * 0x2545F4914F6CDD1Dull) >> 11) / 9007199254740992.0;
	}
	genes::Place place() {
		genes::Place p;
		for (size_t i = 0; i < genes::kRowCount; ++i) {
			p.phase[i] = unit() * 37.0;
			p.rate[i] = (unit() - 0.5) * 0.2;
		}
		return p;
	}
};

/// A CHORD HELD AT ONCE: one press, so every key shares one order — as the emitter gives keys struck together
/// (`StepEmitter`; backlog QR: Transpose's root is the newest press, and of a chord at once its lowest key).
arp::HeldChord chordOf(std::initializer_list<int> keys) {
	arp::HeldChord c;
	for (const int k : keys) {
		c.press(static_cast<int16_t>(k), 0, 0.9f, 1);
	}
	return c;
}

constexpr Mode kModes[] = { Mode::Melody, Mode::MelodyMono, Mode::Chords, Mode::TbMono, Mode::TbGlide };

bool sameSteps(const pat::PatternCycle& a, const pat::PatternCycle& b) {
	if (a.length != b.length || a.playable != b.playable || a.pitchLength != b.pitchLength) {
		return false;
	}
	for (size_t k = 0; k < a.length; ++k) {
		const pat::PatternStep& x = a.steps[k];
		const pat::PatternStep& y = b.steps[k];
		if (x.kind != y.kind || x.count != y.count || x.keys != y.keys || x.gate != y.gate || x.velocity != y.velocity
		    || x.accent != y.accent || x.slide != y.slide || x.ratchet != y.ratchet || x.holdMask != y.holdMask) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("pattern: the same place, recipe and keys build the same cycle; with no key held nothing plays") {
	Dice dice;
	for (int t = 0; t < 100; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : kModes) {
			pat::Recipe r;
			r.mode = m;
			r.length = 1 + static_cast<size_t>(dice.unit() * 64.0);
			pat::PatternCycle a;
			pat::PatternCycle b;
			pat::build(p, r, chordOf({ 60, 64 }), a);
			pat::build(p, r, chordOf({ 60, 64 }), b);
			CHECK(sameSteps(a, b));
			pat::PatternCycle none;
			pat::build(p, r, arp::HeldChord {}, none);
			CHECK_FALSE(none.playable);
		}
	}
}

TEST_CASE("pattern: every key it strikes is in the scale on its root — every scale, every Mode, both Keys modes") {
	Dice dice;
	for (size_t sc = 0; sc < th::kScaleCount; ++sc) {
		for (const Mode m : kModes) {
			for (const pat::Keys keys : { pat::Keys::Transpose, pat::Keys::Pool }) {
				const genes::Place p = dice.place();
				pat::Recipe r;
				r.mode = m;
				r.keys = keys;
				r.scale = sc;
				r.root = 2; // D
				r.size = th::ChordSize::Seventh;
				const int lowest = 62; // D: under Transpose the root is the lowest held key
				pat::PatternCycle c;
				pat::build(p, r, chordOf({ 66, lowest, 69 }), c);
				const uint16_t mask = th::kScales[sc].mask;
				const int tonic = keys == pat::Keys::Transpose ? lowest : 2;
				for (size_t k = 0; k < c.length; ++k) {
					const pat::PatternStep& s = c.steps[k];
					if (s.kind != Kind::Note) {
						continue;
					}
					for (int i = 0; i < s.count; ++i) {
						const int key = i == 0 ? c.keyAt(k, 3) : s.keys[static_cast<size_t>(i)];
						const int pc = (((key - tonic) % 12) + 12) % 12;
						CAPTURE(th::kScales[sc].name);
						CAPTURE(genes::kModeNames[static_cast<size_t>(m)]);
						CHECK(((mask >> pc) & 1u) == 1u);
					}
				}
			}
		}
	}
}

TEST_CASE("pattern: under Transpose, moving the lowest held key by n moves every key of every Mode by n") {
	Dice dice;
	for (int t = 0; t < 60; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : kModes) {
			pat::Recipe r;
			r.mode = m;
			r.scale = static_cast<size_t>(dice.unit() * static_cast<double>(th::kScaleCount));
			r.size = th::ChordSize::Seventh;
			pat::PatternCycle a;
			pat::PatternCycle b;
			pat::build(p, r, chordOf({ 55, 59, 62 }), a);
			pat::build(p, r, chordOf({ 60, 64, 67 }), b);
			for (size_t k = 0; k < a.length; ++k) {
				CHECK(a.steps[k].kind == b.steps[k].kind);
				for (int i = 0; i < a.steps[k].count; ++i) {
					const int ka = i == 0 ? a.keyAt(k, 5) : a.steps[k].keys[static_cast<size_t>(i)];
					const int kb = i == 0 ? b.keyAt(k, 5) : b.steps[k].keys[static_cast<size_t>(i)];
					CAPTURE(genes::kModeNames[static_cast<size_t>(m)]);
					CHECK(kb - ka == 5);
				}
			}
		}
	}
}

TEST_CASE("pattern: each pin switched off is its neutral, by value — a drone on the root, no rests, ties, accents, slides, octaves or ratchets") {
	Dice dice;
	for (int t = 0; t < 100; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : kModes) {
			pat::Recipe r;
			r.mode = m;
			r.degree = 0;
			r.fixedVelocity = true;
			r.velocity = 0.6f;
			for (size_t pin = 0; pin < genes::kPinCount; ++pin) {
				r.pins.on[pin] = false;
			}
			pat::PatternCycle c;
			pat::build(p, r, chordOf({ 57, 60 }), c);
			for (size_t k = 0; k < c.length; ++k) {
				const pat::PatternStep& s = c.steps[k];
				CAPTURE(genes::kModeNames[static_cast<size_t>(m)]);
				CHECK(s.kind == Kind::Note);
				CHECK_FALSE(s.accent);
				CHECK_FALSE(s.slide);
				CHECK(s.ratchet == 1);
				CHECK_FALSE(s.grace);
				CHECK(s.octave == 0);
				CHECK(s.velocity == 0.6f);
				if (m != Mode::Chords) {
					CHECK(c.keyAt(k, 0) == 57); // THE DRONE: the root (the lowest key) every step
				}
				else {
					CHECK(s.degree == 0); // the Degree box's chord, every step
				}
			}
		}
	}
}

TEST_CASE("pattern: chords — a chord step strikes its tones together, a tie only holds the SAME degree, a held step holds its common tones") {
	Dice dice;
	int ties = 0;
	int holds = 0;
	for (int t = 0; t < 300; ++t) {
		const genes::Place p = dice.place();
		pat::Recipe r;
		r.mode = Mode::Chords;
		r.size = th::ChordSize::Seventh;
		pat::PatternCycle c;
		pat::build(p, r, chordOf({ 60 }), c);
		int prevDegree = 0;
		bool prevSounds = false;
		const pat::PatternStep* prevNote = nullptr;
		for (size_t k = 0; k < c.length; ++k) {
			const pat::PatternStep& s = c.steps[k];
			if (s.kind == Kind::Note) {
				CHECK(s.count >= 2);
				for (int i = 1; i < s.count; ++i) {
					CHECK(s.keys[static_cast<size_t>(i)] > s.keys[static_cast<size_t>(i - 1)]);
				}
				for (int i = 0; i < s.count; ++i) {
					if ((s.holdMask >> i) & 1u) {
						++holds;
						REQUIRE(prevNote != nullptr);
						bool shared = false;
						for (int j = 0; j < prevNote->count; ++j) {
							shared = shared || prevNote->keys[static_cast<size_t>(j)] == s.keys[static_cast<size_t>(i)];
						}
						CHECK(shared);
					}
				}
				prevNote = &s;
			}
			if (s.kind == Kind::Tie) {
				++ties;
				CHECK(prevSounds);
				CHECK(s.degree == prevDegree);
			}
			if (s.kind == Kind::Rest) {
				prevNote = nullptr;
			}
			prevSounds = s.kind != Kind::Rest;
			prevDegree = s.degree;
		}
	}
	MESSAGE("ties " << ties << ", held tones " << holds);
	CHECK(ties > 0);
	CHECK(holds > 0);
}

TEST_CASE("pattern: TB's Pitch on Notes walks its own pitch sequence on the NOTE steps — the 303's polymeter, by its closed form") {
	Dice dice;
	int differs = 0;
	for (int t = 0; t < 200; ++t) {
		const genes::Place p = dice.place();
		pat::Recipe r;
		r.mode = Mode::TbMono;
		r.length = 16;
		pat::PatternCycle c;
		pat::build(p, r, chordOf({ 48 }), c);
		REQUIRE(c.pitchLength >= 3);
		REQUIRE(c.pitchLength <= 16);
		int n = 0;
		for (size_t k = 0; k < c.length; ++k) {
			if (c.steps[k].kind == Kind::Note) {
				CHECK(c.steps[k].noteIndex == n);
				++n;
			}
			else {
				CHECK(c.steps[k].noteIndex == -1);
			}
		}
		CHECK(c.notesPerCycle == n);
		for (int64_t cycle = 0; cycle < 5; ++cycle) {
			for (size_t k = 0; k < c.length; ++k) {
				const pat::PatternStep& s = c.steps[k];
				if (s.kind != Kind::Note) {
					continue;
				}
				const int64_t i = (cycle * n + s.noteIndex) % c.pitchLength;
				CHECK(c.keyAt(k, cycle) == c.pitchSeq[static_cast<size_t>(i)] + s.octave);
				if (cycle == 1 && c.keyAt(k, 1) != c.keyAt(k, 0)) {
					++differs;
				}
			}
		}
	}
	// NOT VACUOUS: the notes do move between cycles when P does not divide the note count.
	CHECK(differs > 0);
}

TEST_CASE("pattern: a tied note's gate runs through its ties; a slide reaches past the next onset; TB gates are half a step") {
	Dice dice;
	for (int t = 0; t < 300; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : kModes) {
			pat::Recipe r;
			r.mode = m;
			pat::PatternCycle c;
			pat::build(p, r, chordOf({ 60 }), c);
			for (size_t k = 0; k < c.length; ++k) {
				const pat::PatternStep& s = c.steps[k];
				if (s.kind != Kind::Note) {
					continue;
				}
				size_t ties = 0;
				while (k + ties + 1 < c.length && c.steps[k + ties + 1].kind == Kind::Tie) {
					++ties;
				}
				CHECK(s.gate >= static_cast<float>(ties));
				if (s.slide) {
					CHECK(s.gate >= static_cast<float>(ties) + 1.0f);
				}
				if (genes::isTb(m) && ties == 0 && !s.slide) {
					CHECK(s.gate == 0.5f);
				}
			}
		}
	}
}

TEST_CASE("pattern: what one build costs — the heaviest recipe (64 steps of Chords, 13ths, Auto voicing) — reported, with a generous ceiling") {
	// MEASURED, for design §10.2's "the decode cost is measured at step 3": the device rebuilds on a control or a key
	// change, so this is a per-change cost, not a per-sample one. The ceiling only catches a blow-up; the number is the
	// report (a shared host's load swings it).
	Dice dice;
	pat::Recipe r;
	r.mode = Mode::Chords;
	r.length = 64;
	r.size = th::ChordSize::Thirteenth;
	pat::PatternCycle c;
	const auto held = chordOf({ 60, 64, 67, 71 });
	double worst = 0.0;
	double total = 0.0;
	constexpr int kRuns = 50;
	for (int t = 0; t < kRuns; ++t) {
		const genes::Place p = dice.place();
		const double t0 = threadCpuUs();
		pat::build(p, r, held, c);
		const double us = threadCpuUs() - t0;
		worst = std::max(worst, us);
		total += us;
	}
	r.mode = Mode::Melody;
	const auto t1 = std::chrono::steady_clock::now();
	for (int t = 0; t < kRuns; ++t) {
		pat::build(dice.place(), r, held, c);
	}
	const double melody = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / kRuns;
	MESSAGE("Chords 64 x 13th: mean " << total / kRuns << " us, worst " << worst << " us; Melody 64: mean " << melody << " us");
	CHECK(worst < 50000.0);
}

//------------------------------------------------------------------------------------------------//
// PROGRESSION (design §11-quater)                                                             //
//------------------------------------------------------------------------------------------------//

namespace {

/// A PHASE AT WHICH `row` READS `value` (the first, scanning a unit of phase finely) — so a case can stand at a family,
/// a Density or a cadence by naming the reading rather than hunting for a place.
double phaseReading(genes::Row row, float value) {
	double best = 0.0;
	float err = 2.0f;
	for (int i = 0; i < 40000; ++i) {
		const double ph = static_cast<double>(i) / 40000.0;
		const float e = std::abs(genes::readRow(row, ph) - value);
		if (e < err) {
			err = e;
			best = ph;
		}
	}
	return best;
}

/// THE PLAIN PROGRESSION: every pin off (its neutral 0: no leaps, no borrowing, no extensions, no cadence, a loop, no
/// pushes) but Density and Hold, ON at the field's own value (amount 0.5), which `place` then sets by reading.
struct ProgressionCase {
	genes::Place place {};
	pat::Recipe recipe {};
	ProgressionCase(float family, float density, float hold) {
		recipe.mode = Mode::Progression;
		recipe.stepBeats = 1.0; // a quarter a step: four a bar
		for (size_t pin = 0; pin < genes::kPinCount; ++pin) {
			recipe.pins.on[pin] = false;
		}
		// VOICE LEADING AT ITS DEFAULT, 1: these cases were written against the strict least motion, which that is.
		recipe.pins.on[static_cast<size_t>(Pin::VoiceLeading)] = true;
		recipe.pins.amount[static_cast<size_t>(Pin::VoiceLeading)] = 1.0f;
		set(genes::Row::WideMotif, family);
		set(genes::Row::WideRegister, 0.5f);
		pin(Pin::Density, density);
		pin(Pin::Hold, hold);
	}
	void set(genes::Row row, float value) { place.phase[genes::at(row)] = phaseReading(row, value); }
	void pin(Pin p, float value) {
		recipe.pins.on[static_cast<size_t>(p)] = true;
		recipe.pins.amount[static_cast<size_t>(p)] = 0.5f;
		set(genes::pinRow(p), value);
	}
	/// The degree each new chord strikes, in order.
	std::vector<int> degrees(const arp::HeldChord& held) const {
		pat::PatternCycle c;
		pat::build(place, recipe, held, c);
		std::vector<int> out;
		for (size_t k = 0; k < c.length; ++k) {
			if (c.steps[k].chordStart) {
				out.push_back(c.steps[k].degree);
			}
		}
		return out;
	}
};

/// THE FAMILIES' DEGREES, RESTATED from the design's table (not read back from `familyDegrees`).
const std::vector<std::vector<int>> kFamilyTable = {
	{ 0, 4, 5, 3 },                               // pop loop
	{ 0, 3, 4, 5 },                               // deceptive
	{ 0, 5, 3, 0 },                               // plagal
	{ 0, 3 },                                     // vamp i–iv
	{ 0, 6 },                                     // vamp i–bVII
	{ 0, 6, 5, 4 },                               // descending
	{ 0, 0, 0, 0, 3, 3, 0, 0, 4, 3, 0, 4 },       // 12-bar blues
	{ 0, 5, 1, 4 },                               // turnaround
	{ 1, 4, 0, 0 },                               // ii–V–I
	{ 0, 3, 6, 2, 5, 1, 4, 0 },                   // circle of fifths
};

float familyReading(size_t f) {
	return (static_cast<float>(f) + 0.02f) / static_cast<float>(pat::kFamilies);
}

} // namespace

TEST_CASE("progression: each family's degrees at its own place, a chord a bar; the numerals of the pop loop in Major and in Minor") {
	REQUIRE(kFamilyTable.size() == pat::kFamilies);
	for (size_t f = 0; f < pat::kFamilies; ++f) {
		CAPTURE(pat::kFamilyNames[f]);
		const ProgressionCase pc(familyReading(f), 0.5f, 1.0f);
		CHECK(pc.degrees(chordOf({ 60 })) == kFamilyTable[f]);
		pat::PatternCycle c;
		pat::build(pc.place, pc.recipe, chordOf({ 60 }), c);
		CHECK(c.length == 4 * kFamilyTable[f].size()); // a chord a bar, four quarter-steps a bar
	}
	const auto numerals = [](size_t scale) {
		ProgressionCase pc(familyReading(0), 0.5f, 1.0f);
		pc.recipe.scale = scale;
		pat::PatternCycle c;
		pat::build(pc.place, pc.recipe, chordOf({ 60 }), c);
		std::vector<std::string> out;
		for (size_t k = 0; k < c.length; ++k) {
			if (c.steps[k].chordStart) {
				char n[16];
				pat::numeralOf(c.steps[k], n, sizeof n);
				out.emplace_back(n);
			}
		}
		return out;
	};
	CHECK(numerals(0) == std::vector<std::string> { "I", "V", "vi", "IV" });
	CHECK(numerals(5) == std::vector<std::string> { "i", "v", "bVI", "iv" }); // Minor
}

TEST_CASE("progression: Density is the changes a bar — one every two bars, one a bar, two a bar — at a sixteenth a step") {
	for (const auto& [density, block] : { std::pair { 0.1f, 32u }, std::pair { 0.5f, 16u }, std::pair { 0.9f, 8u } }) {
		CAPTURE(density);
		ProgressionCase pc(familyReading(0), density, 1.0f);
		pc.recipe.stepBeats = 0.25;
		pat::PatternCycle c;
		pat::build(pc.place, pc.recipe, chordOf({ 60 }), c);
		std::vector<size_t> starts;
		for (size_t k = 0; k < c.length; ++k) {
			if (c.steps[k].chordStart) {
				starts.push_back(k);
			}
		}
		REQUIRE(starts.size() >= 2);
		for (size_t i = 0; i < starts.size(); ++i) {
			CHECK(starts[i] == i * block);
		}
		// HOLD 1: every chord sustained whole — a strike, then ties to the next.
		for (size_t k = 0; k < c.length; ++k) {
			CHECK((c.steps[k].kind == Kind::Note) == c.steps[k].chordStart);
		}
	}
}

TEST_CASE("progression: Answer puts a cadence at the phrase end — none at 0, then authentic V–I, plagal IV–I, deceptive V–vi") {
	const auto lastTwo = [](float answer, bool on) {
		ProgressionCase pc(familyReading(0), 0.5f, 1.0f);
		if (on) {
			pc.pin(Pin::Answer, answer);
		}
		const std::vector<int> d = pc.degrees(chordOf({ 60 }));
		return std::pair { d[d.size() - 2], d[d.size() - 1] };
	};
	CHECK(lastTwo(0.0f, false) == std::pair { 5, 3 }); // the pop loop's own ending
	CHECK(lastTwo(0.2f, true) == std::pair { 4, 0 });
	CHECK(lastTwo(0.5f, true) == std::pair { 3, 0 });
	CHECK(lastTwo(0.9f, true) == std::pair { 4, 5 });
}

TEST_CASE("progression: every strike is the least-motion voicing from the one before, among every inversion at every octave in the window") {
	Dice dice;
	int checked = 0;
	for (int t = 0; t < 60; ++t) {
		ProgressionCase pc(static_cast<float>(dice.unit()), static_cast<float>(dice.unit()), 0.0f);
		pc.recipe.inversion = th::kAuto;
		pat::PatternCycle c;
		pat::build(pc.place, pc.recipe, chordOf({ 60 }), c);
		const int centre = 60; // the register read 0.5: the window sits on the tonic
		const th::Window window { centre - 9, centre + 16 };
		const th::Scale scale = th::scaleOf(pc.recipe.scale);
		const pat::PatternStep* prev = nullptr;
		for (size_t k = 0; k < c.length + 1; ++k) {
			const pat::PatternStep& s = c.steps[k % c.length];
			if (s.kind != Kind::Note) {
				continue;
			}
			if (prev != nullptr) {
				th::Voicing was;
				was.count = prev->count;
				std::copy(prev->keys.begin(), prev->keys.begin() + prev->count, was.keys.begin());
				th::Voicing now;
				now.count = s.count;
				std::copy(s.keys.begin(), s.keys.begin() + s.count, now.keys.begin());
				int best = 1 << 30;
				for (int inv = 0; inv < s.count; ++inv) {
					const th::Voicing base = th::voiceChord(scale, 60, s.degree, th::ChordSize::Triad, 0, inv, th::Spread::Close, 0);
					for (int oct = -4; oct <= 4; ++oct) {
						th::Voicing v = base;
						for (int i = 0; i < v.count; ++i) {
							v.keys[static_cast<size_t>(i)] = static_cast<int16_t>(v.keys[static_cast<size_t>(i)] + 12 * oct);
						}
						if (v.keys[0] >= window.low && v.keys[static_cast<size_t>(v.count - 1)] <= window.high) {
							best = std::min(best, th::leadCost(v, was));
						}
					}
				}
				CHECK(th::leadCost(now, was) == best);
				++checked;
			}
			prev = &s;
		}
	}
	CHECK(checked > 100);
}

TEST_CASE("progression: under Pool a chord holding none of the held notes gives way to its third-related neighbour that does; Transpose only moves the tonic") {
	ProgressionCase pc(familyReading(0), 0.5f, 1.0f); // I V vi IV
	pc.recipe.keys = pat::Keys::Pool;
	pc.recipe.root = 0;
	// E held: V (G B D) gives way to iii (E G B), IV (F A C) to vi (A C E); I and vi already hold it.
	CHECK(pc.degrees(chordOf({ 64 })) == std::vector<int> { 0, 2, 5, 5 });
	// G held: vi (A C E) gives way to I a third above (C E G); IV (F A C) has no neighbour a third away holding G (vi, ii),
	// so it keeps its own.
	CHECK(pc.degrees(chordOf({ 67 })) == std::vector<int> { 0, 4, 0, 3 });
	// TRANSPOSE: the same loop, every key moved by the lowest held key's interval.
	ProgressionCase tc(familyReading(0), 0.5f, 0.3f);
	pat::PatternCycle a;
	pat::PatternCycle b;
	pat::build(tc.place, tc.recipe, chordOf({ 60, 64 }), a);
	pat::build(tc.place, tc.recipe, chordOf({ 62, 64 }), b);
	CHECK(tc.degrees(chordOf({ 60, 64 })) == std::vector<int> { 0, 4, 5, 3 }); // no steering under Transpose
	REQUIRE(a.length == b.length);
	for (size_t k = 0; k < a.length; ++k) {
		REQUIRE(a.steps[k].count == b.steps[k].count);
		for (int i = 0; i < a.steps[k].count; ++i) {
			CHECK(b.steps[k].keys[static_cast<size_t>(i)] - a.steps[k].keys[static_cast<size_t>(i)] == 2);
		}
	}
}

TEST_CASE("progression: Leap borrows from the parallel scale or puts the next chord's secondary dominant; Detail extends; the cycle is pure") {
	Dice dice;
	int borrowed = 0;
	int secondary = 0;
	int extended = 0;
	for (int t = 0; t < 200; ++t) {
		const genes::Place p = dice.place();
		pat::Recipe r;
		r.mode = Mode::Progression;
		r.stepBeats = 1.0;
		const pat::Progression pr = pat::progressionOf(p, r, chordOf({ 60 }));
		const th::Scale major = th::scaleOf(0);
		for (size_t i = 0; i < pr.count; ++i) {
			const pat::ProgressionChord& ch = pr.chord[i];
			if (ch.scale == 5) {
				++borrowed; // the parallel minor, on the same tonic
				CHECK(ch.tonic == 0);
			}
			if (ch.scale == 4) {
				++secondary; // Mixolydian a fifth above the next chord's root: its dominant seventh
				const pat::ProgressionChord& to = pr.chord[(i + 1) % pr.count];
				const int toRoot = to.scale < 0 ? th::keyOfDegree(major, 0, to.degree) % 12
				                                : (to.tonic + th::keyOfDegree(th::scaleOf(static_cast<size_t>(to.scale)), 0, to.degree)) % 12;
				CHECK(ch.tonic == (toRoot + 7) % 12);
				CHECK(ch.degree == 0);
			}
			extended += ch.size != th::ChordSize::Triad ? 1 : 0;
		}
		pat::PatternCycle a;
		pat::PatternCycle b;
		pat::build(p, r, chordOf({ 60 }), a);
		pat::build(p, r, chordOf({ 60 }), b);
		CHECK(sameSteps(a, b));
	}
	CHECK(borrowed > 0);
	CHECK(secondary > 0);
	CHECK(extended > 0);
}

TEST_CASE("progression: a puck move morphs the progression a chord at a time — across every family's bin, no two neighbouring places differ in more than one chord") {
	std::vector<int> before;
	int changes = 0;
	for (int i = 0; i <= 2000; ++i) {
		const float x = std::min(0.9999f, static_cast<float>(i) / 2000.0f);
		const ProgressionCase pc(x, 0.5f, 1.0f);
		const std::vector<int> now = pc.degrees(chordOf({ 60 }));
		if (!before.empty() && now.size() == before.size()) {
			int differ = 0;
			for (size_t j = 0; j < now.size(); ++j) {
				differ += now[j] != before[j] ? 1 : 0;
			}
			CAPTURE(x);
			CHECK(differ <= 1);
			changes += differ;
		}
		before = now;
	}
	CHECK(changes > 10); // it does move
}

TEST_CASE("pattern: RATCHETS ARE THE RATCHET PIN'S, opt in — none by default or from Detail at any amount; switched on, the "
          "steps whose Detail row reads under it, 2-4 hits; none in a Mode that does not read it") {
	// Ratchets are opt in, through a field scaler: ratchets driven by Detail alone sounded like double-tapped notes.
	Dice dice;
	int ratchetsOn = 0;
	for (int t = 0; t < 100; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : { Mode::Melody, Mode::Chords, Mode::TbMono, Mode::TbGlide, Mode::MelodyMono }) {
			CAPTURE(genes::kModeNames[static_cast<size_t>(m)]);
			const auto ratchets = [&](const pat::Recipe& r) {
				pat::PatternCycle c;
				pat::build(p, r, chordOf({ 60 }), c);
				std::vector<int> hits(c.length, 1);
				for (size_t k = 0; k < c.length; ++k) {
					hits[k] = c.steps[k].ratchet;
				}
				return hits;
			};
			pat::Recipe r;
			r.mode = m;
			const std::vector<int> none(ratchets(r).size(), 1);
			CHECK(ratchets(r) == none); // THE DEFAULT: Ratchet Off
			r.pins.amount[static_cast<size_t>(Pin::Detail)] = 1.0f;
			CHECK(ratchets(r) == none); // DETAIL, at its most, strikes none
			r.pins.amount[static_cast<size_t>(Pin::Detail)] = 0.5f;
			r.pins.on[static_cast<size_t>(Pin::Ratchet)] = true;
			r.pins.amount[static_cast<size_t>(Pin::Ratchet)] = 1.0f; // the most: every note step whose row reads under 1
			const std::vector<int> on = ratchets(r);
			pat::PatternCycle plain;
			pat::Recipe plainRecipe;
			plainRecipe.mode = m;
			pat::build(p, plainRecipe, chordOf({ 60 }), plain);
			for (size_t k = 0; k < on.size(); ++k) {
				CAPTURE(k);
				if (!genes::pinRead(m, Pin::Ratchet) || plain.steps[k].kind != Kind::Note) {
					CHECK(on[k] == 1);
					continue;
				}
				// THE FIELD'S OWN READING: at amount 1 the pin is 1, so hits = 2 + min(2, ⌊3·d⌋) on every note step.
				const float d = genes::stepOf(p, k, plain.length)[genes::at(genes::Row::Detail)];
				CHECK(on[k] == (d < 1.0f ? 2 + std::min(2, static_cast<int>(3.0f * d)) : 1));
				ratchetsOn += on[k] > 1 ? 1 : 0;
			}
		}
	}
	CHECK(ratchetsOn > 100); // non-vacuity
}

TEST_CASE("pattern: the VOICE LEADING pin reaches the voicing in Progression and Chords — 1 by default (the least motion, "
          "unchanged), Off the same as 0, led cycles moving less than unled ones far more often than more, and the same inputs give the same "
          "cycle") {
	// A voice-leading field for Progression mode (provisional design, to be confirmed).
	Dice dice;
	int differs = 0;
	int ledLess = 0;
	int ledMore = 0;
	int compared = 0;
	for (int t = 0; t < 150; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : { Mode::Progression, Mode::Chords }) {
			CAPTURE(genes::kModeNames[static_cast<size_t>(m)]);
			const auto cycleAt = [&](float amount, bool on) {
				pat::Recipe r;
				r.mode = m;
				r.stepBeats = 0.25;
				r.pins.amount[static_cast<size_t>(Pin::VoiceLeading)] = amount;
				r.pins.on[static_cast<size_t>(Pin::VoiceLeading)] = on;
				pat::PatternCycle c;
				pat::build(p, r, chordOf({ 60 }), c);
				return c;
			};
			// THE CHORDS' MOTION, as played: each struck chord's keys against the struck chord before (the wrap included).
			const auto motion = [](const pat::PatternCycle& c) {
				std::vector<th::Voicing> chords;
				for (size_t k = 0; k < c.length; ++k) {
					const pat::PatternStep& st = c.steps[k];
					if (st.kind == Kind::Note && st.count > 1) {
						th::Voicing v;
						v.count = st.count;
						for (int i = 0; i < st.count; ++i) {
							v.keys[static_cast<size_t>(i)] = st.keys[static_cast<size_t>(i)];
						}
						chords.push_back(v);
					}
				}
				long total = 0;
				for (size_t i = 0; i < chords.size(); ++i) {
					total += th::leadCost(chords[i], chords[(i + chords.size() - 1) % chords.size()]);
				}
				return total;
			};
			const pat::PatternCycle byDefault = [&] {
				pat::Recipe r;
				r.mode = m;
				r.stepBeats = 0.25;
				pat::PatternCycle c;
				pat::build(p, r, chordOf({ 60 }), c);
				return c;
			}();
			CHECK(sameSteps(byDefault, cycleAt(1.0f, true)));   // THE DEFAULT IS 1: today's leading
			CHECK(sameSteps(cycleAt(0.0f, true), cycleAt(0.8f, false))); // OFF IS 0, whatever the amount
			CHECK(sameSteps(cycleAt(0.3f, true), cycleAt(0.3f, true)));  // deterministic
			// THE CHAIN IS MEASURED, NOT PROMISED (test_arp_theory says why): led moves less far more often than more.
			ledLess += motion(cycleAt(1.0f, true)) < motion(cycleAt(0.0f, true)) ? 1 : 0;
			ledMore += motion(cycleAt(1.0f, true)) > motion(cycleAt(0.0f, true)) ? 1 : 0;
			differs += sameSteps(cycleAt(0.0f, true), byDefault) ? 0 : 1;
			++compared;
		}
	}
	MESSAGE("places where unled voicings differ from led ones: " << differs << " of " << compared << "; led moves less in "
	                                                               << ledLess << ", more in " << ledMore);
	CHECK(ledLess > 2 * ledMore); // MEASURED 129 against 42 over these 300 (the octave folds and held tones add to the chain)
	CHECK(differs * 4 > compared); // non-vacuity: the pin moves the voicing
}

//------------------------------------------------------------------------------------------------//
// APPROACH (design §10-ter: Progression and Chords initially make one note more likely)                    //
//------------------------------------------------------------------------------------------------//

namespace {

/// A CYCLE IN Chords OR Progression, Approach switched on at `amount` (or left at the default: Off).
pat::PatternCycle approached(const genes::Place& p, Mode m, std::optional<float> amount, int voiceLeadingPercent = 100) {
	pat::Recipe r;
	r.mode = m;
	r.stepBeats = 0.25;
	r.pins.amount[static_cast<size_t>(Pin::VoiceLeading)] = static_cast<float>(voiceLeadingPercent) / 100.0f;
	if (amount.has_value()) {
		r.pins.on[static_cast<size_t>(Pin::Approach)] = true;
		r.pins.amount[static_cast<size_t>(Pin::Approach)] = *amount;
	}
	pat::PatternCycle c;
	pat::build(p, r, chordOf({ 60 }), c);
	return c;
}

/// THE SCALE KEYS, restated from the scale's pitch classes (not from `theory::keyOfDegree`): the n-th key of C major
/// strictly below or above `target`.
int scaleKeyFrom(int target, int n, bool below) {
	const th::Scale s = th::scaleOf(0); // the default scale box: index 0
	const auto inScale = [&](int key) {
		const int pc = ((key - 60) % 12 + 12) % 12;
		for (int i = 0; i < s.count; ++i) {
			if (s.steps[static_cast<size_t>(i)] == pc) {
				return true;
			}
		}
		return false;
	};
	int key = target;
	for (int found = 0; found < n;) {
		key += below ? -1 : 1;
		found += inScale(key) ? 1 : 0;
	}
	return key;
}

/// THE CHANGE A PICKUP LEADS INTO: the first step after it that is not a pickup.
size_t changeAfter(const pat::PatternCycle& c, size_t k) {
	size_t at = (k + 1) % c.length;
	while (c.steps[at].approach != pat::ApproachKind::None) {
		at = (at + 1) % c.length;
	}
	return at;
}

float slideAt(const genes::Place& p, size_t k, size_t length) {
	return genes::stepOf(p, k % length, length)[genes::at(genes::Row::Slide)];
}

/// THE CHANGES OF A PLAIN CYCLE (no pickups), restated: Progression's `chordStart`; in Chords a struck step whose degree
/// differs from the struck step before it. Returns, for change `at`, the steps back to the change before it (0: not one).
size_t gapBefore(const pat::PatternCycle& plain, size_t at, bool progression) {
	const size_t L = plain.length;
	const auto isChange = [&](size_t k) {
		const pat::PatternStep& st = plain.steps[k];
		if (st.kind != Kind::Note || st.count < 1) {
			return false;
		}
		if (progression) {
			return st.chordStart;
		}
		for (size_t back = 1; back < L; ++back) {
			const pat::PatternStep& q = plain.steps[(k + L - back) % L];
			if (q.kind == Kind::Note && q.count >= 1) {
				return q.degree != st.degree;
			}
		}
		return false;
	};
	if (!isChange(at)) {
		return 0;
	}
	size_t gap = 1;
	while (gap < L && !isChange((at + L - gap) % L)) {
		++gap;
	}
	return gap;
}

} // namespace

TEST_CASE("approach: OFF BY DEFAULT and bit-identical — no pickup anywhere, and amount 0, or any amount switched off, is the "
          "default cycle step for step, in Chords and Progression") {
	Dice dice;
	for (int t = 0; t < 200; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : { Mode::Chords, Mode::Progression }) {
			CAPTURE(genes::kModeNames[static_cast<size_t>(m)]);
			const pat::PatternCycle plain = approached(p, m, std::nullopt);
			for (size_t k = 0; k < plain.length; ++k) {
				CHECK(plain.steps[k].approach == pat::ApproachKind::None);
			}
			CHECK(sameSteps(plain, approached(p, m, 0.0f)));
			pat::Recipe off;
			off.mode = m;
			off.stepBeats = 0.25;
			off.pins.amount[static_cast<size_t>(Pin::Approach)] = 0.9f; // switched off: the amount is only remembered
			pat::PatternCycle c;
			pat::build(p, off, chordOf({ 60 }), c);
			CHECK(sameSteps(plain, c));
		}
	}
}

TEST_CASE("approach: every pickup RESOLVES into a tone of the next chord by its kind's interval — scale steps, chromatic "
          "half steps, an anticipation — from the side the voice comes from; the count, kind and voice are the field's "
          "readings; the chord before is cut where the first pickup starts") {
	Dice dice;
	std::array<int, 4> kinds {};
	int pickups = 0;
	for (int t = 0; t < 300; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : { Mode::Chords, Mode::Progression }) {
			CAPTURE(genes::kModeNames[static_cast<size_t>(m)]);
			const pat::PatternCycle plain = approached(p, m, std::nullopt);
			const pat::PatternCycle c = approached(p, m, 1.0f); // at the most every change the room allows is approached
			REQUIRE(c.length == plain.length);
			const size_t L = c.length;
			for (size_t k = 0; k < L; ++k) {
				const pat::PatternStep& st = c.steps[k];
				if (st.approach == pat::ApproachKind::None) {
					continue;
				}
				CAPTURE(k);
				++pickups;
				++kinds[static_cast<size_t>(st.approach)];
				const size_t at = changeAfter(c, k);
				const pat::PatternStep& to = c.steps[at];
				// THE CHANGE IS THE CHORD THE TERRAIN STRUCK THERE, untouched (its keys are the voicing — Voice Leading's).
				REQUIRE(to.kind == Kind::Note);
				CHECK(to.keys == plain.steps[at].keys);
				CHECK(st.kind == Kind::Note);
				CHECK(st.count == 1);
				CHECK(st.ratchet == 1);
				CHECK(st.approachIndex == static_cast<uint8_t>((at + L - k) % L));
				// THE FIELD'S READINGS, restated: the kind at the change minus one, the voice at minus two.
				const float kindReading = slideAt(p, at + L - 1, L);
				const pat::ApproachKind kind = kindReading < pat::kApproachScale ? pat::ApproachKind::Scale
				                               : kindReading < pat::kApproachChromatic ? pat::ApproachKind::Chromatic
				                                                                       : pat::ApproachKind::Anticipation;
				CHECK(st.approach == kind);
				const float voiceReading = slideAt(p, at + L - 2, L);
				const int n = to.count;
				int index = n - 1;
				if (voiceReading >= pat::kApproachTop) {
					index = voiceReading < pat::kApproachBass
					            ? 0
					            : std::clamp(static_cast<int>((voiceReading - pat::kApproachBass) / (1.0f - pat::kApproachBass) * n), 0, n - 1);
				}
				CHECK(st.approachTarget == to.keys[static_cast<size_t>(index)]); // THE VOICE CHOSEN IS THE ONE APPROACHED
				// THE INTERVAL, from the side the pickup stands on.
				const int target = st.approachTarget;
				const int j = st.approachIndex;
				const bool below = st.keys[0] < target || (st.keys[0] == target && j == 1);
				switch (st.approach) {
				case pat::ApproachKind::Scale: CHECK(st.keys[0] == scaleKeyFrom(target, j, below)); break;
				case pat::ApproachKind::Chromatic: CHECK(std::abs(st.keys[0] - target) == j); break;
				case pat::ApproachKind::Anticipation:
					CHECK(st.keys[0] == (j == 1 ? target : scaleKeyFrom(target, j - 1, below)));
					if (j == 1) {
						CHECK(((to.holdMask >> index) & 1u) != 0u); // the change HOLDS the anticipated tone
					}
					break;
				case pat::ApproachKind::None: break;
				}
				// THE COUNT, restated: the change's reading under the pin (1 here), capped by the room since the change before.
				if (j == 1) {
					const float share = slideAt(p, at, L);
					const int want = share < pat::kApproachOne ? 1 : (share < pat::kApproachTwo ? 2 : 3);
					int count = 0;
					while (c.steps[(at + L - static_cast<size_t>(count) - 1) % L].approach != pat::ApproachKind::None) {
						++count;
					}
					CHECK(count <= want);
					// THE CHORD BEFORE IS CUT where the first pickup starts.
					const size_t first = (at + L - static_cast<size_t>(count)) % L;
					for (size_t back = 1; back < L; ++back) {
						const pat::PatternStep& prev = c.steps[(first + L - back) % L];
						if (prev.kind == Kind::Note) {
							if (prev.approach == pat::ApproachKind::None) {
								CHECK(prev.gate <= static_cast<float>(back) + 1e-6f);
							}
							break;
						}
						if (prev.kind == Kind::Rest) {
							break;
						}
					}
				}
			}
		}
	}
	MESSAGE("pickups " << pickups << ": scale " << kinds[1] << ", chromatic " << kinds[2] << ", anticipation " << kinds[3]);
	CHECK(kinds[1] > 0);
	CHECK(kinds[2] > 0);
	CHECK(kinds[3] > 0);
}

TEST_CASE("approach: ONE pickup far more often than two, three rarely; the kinds and the voices in their weights — measured "
          "over many changes against the shares the field is read with") {
	// At amount 1 every change reads its share straight off the Slide row, uniform over places: so the counts among
	// changes with room for three are the weights themselves.
	Dice dice;
	std::array<int, 4> count {};
	std::array<int, 4> kind {};
	int top = 0;
	int bass = 0;
	int any = 0;
	for (int t = 0; t < 1500; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : { Mode::Chords, Mode::Progression }) {
			const pat::PatternCycle plain = approached(p, m, std::nullopt);
			const pat::PatternCycle c = approached(p, m, 1.0f);
			const size_t L = c.length;
			for (size_t k = 0; k < L; ++k) {
				const pat::PatternStep& st = c.steps[k];
				if (st.approach == pat::ApproachKind::None || st.approachIndex != 1) {
					continue;
				}
				const size_t at = (k + 1) % L;
				int n = 0;
				while (c.steps[(at + L - static_cast<size_t>(n) - 1) % L].approach != pat::ApproachKind::None && n < 4) {
					++n;
				}
				// ROOM FOR THREE (the change before is at least four steps back), so no count was capped.
				if (gapBefore(plain, at, m == Mode::Progression) >= 4) {
					++count[static_cast<size_t>(n)];
				}
				++kind[static_cast<size_t>(st.approach)];
				top += st.approachVoice == pat::ApproachVoice::Top ? 1 : 0;
				bass += st.approachVoice == pat::ApproachVoice::Bass ? 1 : 0;
				any += st.approachVoice == pat::ApproachVoice::Any ? 1 : 0;
			}
		}
	}
	const double changes = count[1] + count[2] + count[3];
	const double kinds = kind[1] + kind[2] + kind[3];
	const double voices = top + bass + any;
	MESSAGE("counts 1/2/3: " << count[1] << " " << count[2] << " " << count[3] << "; kinds " << kind[1] << " " << kind[2]
	                         << " " << kind[3] << "; voices " << top << " " << bass << " " << any);
	REQUIRE(changes > 1000);
	CHECK(std::abs(count[1] / changes - 0.75) < 0.05);
	CHECK(std::abs(count[2] / changes - 0.20) < 0.05);
	CHECK(std::abs(count[3] / changes - 0.05) < 0.03);
	CHECK(count[1] > 3 * count[2]); // ONE FAR MORE OFTEN THAN TWO
	CHECK(std::abs(kind[1] / kinds - 0.5) < 0.06);
	CHECK(std::abs(kind[2] / kinds - 0.3) < 0.06);
	CHECK(std::abs(kind[3] / kinds - 0.2) < 0.06);
	CHECK(std::abs(top / voices - 0.5) < 0.06);
	CHECK(std::abs(bass / voices - 0.25) < 0.06);
	CHECK(std::abs(any / voices - 0.25) < 0.06);
}

TEST_CASE("approach: the amount is how often — more changes approached as it rises; the same inputs give the same cycle; "
          "Ratchet on leaves the pickups single; a hand edit on a pickup step wins") {
	Dice dice;
	long approachedAt[3] = {};
	for (int t = 0; t < 300; ++t) {
		const genes::Place p = dice.place();
		for (const Mode m : { Mode::Chords, Mode::Progression }) {
			const float amounts[3] = { 0.2f, 0.6f, 1.0f };
			for (int a = 0; a < 3; ++a) {
				const pat::PatternCycle c = approached(p, m, amounts[a]);
				for (size_t k = 0; k < c.length; ++k) {
					approachedAt[a] += c.steps[k].approach != pat::ApproachKind::None && c.steps[k].approachIndex == 1 ? 1 : 0;
				}
				CHECK(sameSteps(c, approached(p, m, amounts[a]))); // DETERMINISTIC: a pure function of the place
			}
			// RATCHET ON AT THE MOST: chords ratchet, pickups never.
			pat::Recipe r;
			r.mode = m;
			r.stepBeats = 0.25;
			r.pins.on[static_cast<size_t>(Pin::Approach)] = true;
			r.pins.amount[static_cast<size_t>(Pin::Approach)] = 1.0f;
			r.pins.on[static_cast<size_t>(Pin::Ratchet)] = true;
			r.pins.amount[static_cast<size_t>(Pin::Ratchet)] = 1.0f;
			pat::PatternCycle c;
			pat::build(p, r, chordOf({ 60 }), c);
			for (size_t k = 0; k < c.length; ++k) {
				if (c.steps[k].approach != pat::ApproachKind::None) {
					CHECK(c.steps[k].ratchet == 1);
				}
			}
		}
	}
	MESSAGE("changes approached at 20/60/100%: " << approachedAt[0] << " " << approachedAt[1] << " " << approachedAt[2]);
	CHECK(approachedAt[0] < approachedAt[1]);
	CHECK(approachedAt[1] < approachedAt[2]);

	// A HAND EDIT ON A PICKUP STEP WINS (design §15-bis): the overlay applies over the decode, pickups included.
	Dice more;
	bool edited = false;
	for (int t = 0; t < 50 && !edited; ++t) {
		const genes::Place p = more.place();
		pat::PatternCycle c = approached(p, Mode::Chords, 1.0f);
		for (size_t k = 0; k < c.length; ++k) {
			if (c.steps[k].approach == pat::ApproachKind::None) {
				continue;
			}
			std::array<pat::StepEdit, genes::kMaxSteps> edits {};
			pat::StepEdit e = pat::editOf(c.steps[k], false);
			pat::setEditKey(e, c, c.steps[k].keys[0] + 5);
			edits[k] = e;
			const int want = c.steps[k].keys[0] + 5;
			pat::applyEdits(edits, false, c);
			CHECK(c.steps[k].edited);
			CHECK(c.keyAt(k, 0) == want);
			edited = true;
			break;
		}
	}
	CHECK(edited);
}
