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

#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_types.h"
#include "device/format_value.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

// PATTERN'S CYCLE PLAY MODE, BY VALUE. Which pattern plays at a song
// position is DERIVED from that position, so every case reads the pattern and step the device played at a slot
// (`playheadForTest`'s ordinal: pattern << 6 | step) and the walk's own segments, never a difference between renders.

namespace pat = feathers::mod::arp::pat;
namespace genes = feathers::mod::arp::genes;
namespace device = feathers::device;
using Kind = device::NoteEvent::Kind;

namespace {

constexpr double kRate = 48000.0;
constexpr int64_t kStep = 6000; // a sixteenth at 120 bpm

struct Logged {
	Kind kind;
	int16_t key;
	double velocity;
	int64_t sample;
	int32_t expressionId = -1;
	double expressionValue = 0.0;
};

/// THE SYNTH'S PROTOCOL AROUND ONE DEVICE (test_pattern_device's `Drive`), plus what the device played at each call: the
/// playhead's ordinal sampled right after every call that sounded a slot.
struct Drive {
	pat::PatternDevice d;
	pat::Settings s;
	double bpm = 120.0;
	bool playing = true;
	double startBeats = 0.0;
	int64_t songOrigin = 0;
	int64_t now = 0;
	std::vector<Logged> log;
	/// WHAT PLAYED: every slot the device played a step on — its sample, the song position there, the pattern, the step.
	struct Played {
		int64_t sample;
		double beats;
		size_t pattern;
		size_t step;
		/// THE IDENTITY GATES (design §7.10) right after the slot: tab N's `tabActive`.
		std::array<float, pat::kPatterns> gates {};
	};
	std::vector<Played> played;
	uint64_t slotsSeen = 0;

	Drive() {
		d.prepare(kRate);
		for (auto& q : s.patterns) {
			q.pins[static_cast<size_t>(genes::Pin::Detail)] = 0.0f;
		}
		apply();
	}
	void apply() {
		const pat::Row row = pat::rowOf(s);
		d.setFields(row.data(), row.size());
	}
	device::TransportClock clockAt(int64_t sample) const {
		device::TransportClock c = device::TransportClock::fromSecondsPerBeat(60.0 / bpm);
		const double beats = startBeats + (playing ? static_cast<double>(sample - songOrigin) / kRate / (60.0 / bpm) : 0.0);
		c.songPositionBeats = beats;
		c.songPositionSeconds = beats * 60.0 / bpm;
		c.playing = playing;
		return c;
	}
	void locate(double beats) {
		startBeats = beats;
		songOrigin = now;
	}
	void note(Kind kind, int key) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = 3;
		e.key = static_cast<int16_t>(key);
		e.velocity = 0.9;
		std::array<device::NoteEvent, 8> out {};
		size_t n = 0;
		d.processNotes(&e, 1, out.data(), out.size(), &n);
		for (size_t i = 0; i < n; ++i) {
			log.push_back({ out[i].kind, out[i].key, out[i].velocity, now });
		}
	}
	void runTo(int64_t until) {
		std::vector<device::NoteEvent> out(257);
		while (now < until) {
			uint32_t next = 0;
			const size_t n = d.emit(clockAt(now), static_cast<uint64_t>(now), out.data(), 256, &next);
			for (size_t i = 0; i < n; ++i) {
				log.push_back({ out[i].kind, out[i].key, out[i].velocity, now, out[i].expressionId, out[i].expressionValue });
			}
			if (d.slotsPlayedForTest() != slotsSeen) {
				REQUIRE(d.slotsPlayedForTest() == slotsSeen + 1); // one slot a call
				slotsSeen = d.slotsPlayedForTest();
				const uint32_t o = d.lastOrdinalForTest();
				played.push_back({ now, clockAt(now).songPositionBeats, o >> pat::PatternDevice::kPlayheadStepBits,
				                   o & ((1u << pat::PatternDevice::kPlayheadStepBits) - 1u) });
				for (size_t p = 0; p < pat::kPatterns; ++p) {
					played.back().gates[p] = d.tabActive(p);
				}
			}
			REQUIRE(next >= 1);
			now += std::min<int64_t>({ static_cast<int64_t>(next), 32, until - now });
		}
	}
};

/// FNV-1a over every logged event, every field bit for bit — the selector road's fingerprint.
uint64_t hashOf(const std::vector<Logged>& log) {
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&](const void* p, size_t n) {
		const auto* b = static_cast<const unsigned char*>(p);
		for (size_t i = 0; i < n; ++i) {
			h = (h ^ b[i]) * 1099511628211ull;
		}
	};
	for (const Logged& l : log) {
		const auto kind = static_cast<int32_t>(l.kind);
		mix(&kind, sizeof kind);
		mix(&l.key, sizeof l.key);
		mix(&l.velocity, sizeof l.velocity);
		mix(&l.sample, sizeof l.sample);
		mix(&l.expressionId, sizeof l.expressionId);
		mix(&l.expressionValue, sizeof l.expressionValue);
	}
	return h;
}

void checkBalanced(const std::vector<Logged>& log) {
	std::map<int, int> sounding;
	for (const Logged& l : log) {
		if (l.kind == Kind::On) {
			CHECK_MESSAGE(sounding[l.key] == 0, "an on for key " << l.key << " at " << l.sample << " while it sounds");
			++sounding[l.key];
		}
		else if (l.kind == Kind::Off) {
			CHECK_MESSAGE(sounding[l.key] == 1, "an off for key " << l.key << " at " << l.sample << " not sounding");
			sounding[l.key] = 0;
		}
	}
	for (const auto& [key, n] : sounding) {
		CHECK_MESSAGE(n == 0, "key " << key << " left sounding");
	}
}

/// ONE TAB, as a case states it.
struct Tab {
	bool enable = true;
	size_t length = 4;
	int repeats = 1;
	pat::NextAction next = pat::NextAction::Next;
	size_t timeBase = 7; // 1/16 = 6000 samples at 120 bpm; 10 = 1/8 (12000); 9 = 1/16T (4000)
	bool solo = false;
};

int64_t stepSamplesOf(size_t timeBase) {
	switch (timeBase) {
	case 7: return 6000;
	case 10: return 12000;
	case 9: return 4000;
	case 4: return 3000;
	default: FAIL("a time base the reference does not know"); return 0;
	}
}

/// A DEVICE ON CYCLE with these tabs: every step a note (Density and Hold off), a key held from sample 0.
void setUp(Drive& d, const std::array<Tab, pat::kPatterns>& tabs) {
	d.s.playMode = pat::PlayMode::Cycle;
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		auto& q = d.s.patterns[p];
		q.enable = tabs[p].enable;
		q.solo = tabs[p].solo;
		q.length = tabs[p].length;
		q.timeBase = tabs[p].timeBase;
		q.zone = 120 + 230 * static_cast<int>(p);
		q.gate = 0.5f;
		q.pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
		q.pins[static_cast<size_t>(genes::Pin::Hold)] = 0.0f;
		d.s.cycle[p].repeats = tabs[p].repeats;
		d.s.cycle[p].next = tabs[p].next;
	}
	d.apply();
}

/// **THE REFERENCE WALK, restated in samples** (120 bpm, 48 kHz: every step a whole number of samples): the first tab in
/// range from sample 0; a segment is Repeats × Length of its pattern's steps starting at its first step at or after the
/// boundary; Next / Previous wrap in tab order over the tabs in range; Loop stays; Stop ends. Random is not restated (its
/// dice are the device's own; cases check its properties). Every slot before `until`.
std::vector<Drive::Played> reference(const std::array<Tab, pat::kPatterns>& tabs, int64_t until) {
	bool anySolo = false;
	for (const Tab& t : tabs) {
		anySolo = anySolo || t.solo;
	}
	std::vector<size_t> range;
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		if (anySolo ? tabs[p].solo : tabs[p].enable) {
			range.push_back(p);
		}
	}
	std::vector<Drive::Played> out;
	if (range.empty()) {
		return out;
	}
	size_t pos = 0;
	int64_t start = 0;
	for (int guard = 0; guard < 100000; ++guard) {
		const size_t p = range[pos];
		const int64_t step = stepSamplesOf(tabs[p].timeBase);
		const int64_t first = (start + step - 1) / step;
		const int64_t span = static_cast<int64_t>(tabs[p].length) * tabs[p].repeats;
		const int64_t end = (first + span) * step;
		for (int64_t slot = first; slot * step < std::min(end, until); ++slot) {
			out.push_back({ slot * step, static_cast<double>(slot * step) / 24000.0, p,
			                static_cast<size_t>((slot - first) % static_cast<int64_t>(tabs[p].length)) });
		}
		if (end >= until) {
			break;
		}
		switch (tabs[p].next) {
		case pat::NextAction::Next: pos = (pos + 1) % range.size(); break;
		case pat::NextAction::Previous: pos = (pos + range.size() - 1) % range.size(); break;
		case pat::NextAction::Loop: break;
		case pat::NextAction::Stop: return out;
		default: FAIL("Random is not restated"); return out;
		}
		start = end;
	}
	return out;
}

void checkAgainst(const std::vector<Drive::Played>& got, const std::vector<Drive::Played>& want, int64_t from = 0) {
	std::vector<Drive::Played> g;
	for (const auto& p : got) {
		if (p.sample >= from) {
			g.push_back(p);
		}
	}
	std::vector<Drive::Played> w;
	for (const auto& p : want) {
		if (p.sample >= from) {
			w.push_back(p);
		}
	}
	CHECK(g.size() == w.size());
	for (size_t i = 0; i < std::min(g.size(), w.size()); ++i) {
		CAPTURE(i);
		CAPTURE(w[i].sample);
		CHECK(g[i].sample == w[i].sample);
		CHECK(g[i].pattern == w[i].pattern);
		CHECK(g[i].step == w[i].step);
		if (g[i].sample != w[i].sample || g[i].pattern != w[i].pattern || g[i].step != w[i].step) {
			break; // the first divergence says it; the rest only repeat it
		}
	}
}

} // namespace

// THE SELECTOR ROAD IS BIT-IDENTICAL TO THE BUILD BEFORE CYCLE (42e01f77): this script's event stream was hashed on that
// build, unchanged, and the hash pinned here. Four patterns of different Modes, Lengths and Rates (a sixteenth, an eighth,
// a sixteenth triplet), the selector automated across them mid-step, a swing, a locate back, a transport stop under held
// keys onto the free clock, and a restart.
TEST_CASE("cycle: Selector mode plays exactly the events it played before Cycle existed — a pinned hash of a 4-pattern selector script") {
	Drive d;
	d.s.feel.mode = static_cast<feathers::mod::arp::humanize::Mode>(1);
	d.s.feel.amount = 0.6;
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		d.s.patterns[p].enable = true;
		d.s.patterns[p].zone = 150 + 210 * static_cast<int>(p);
		d.s.patterns[p].gate = 0.7f;
	}
	d.s.patterns[1].mode = genes::Mode::Chords;
	d.s.patterns[1].timeBase = 10; // 1/8
	d.s.patterns[2].length = 12;
	d.s.patterns[2].timeBase = 9; // 1/16T
	d.s.patterns[3].mode = genes::Mode::TbGlide;
	d.s.patterns[3].length = 7;
	d.s.run = feathers::mod::arp::RunMode::Always;
	d.apply();
	d.note(Kind::On, 48);
	d.note(Kind::On, 55);
	const float sel[] = { 0.1f, 0.4f, 0.6f, 0.9f, 0.3f, 0.7f, 0.05f, 0.95f };
	for (int i = 0; i < 8; ++i) {
		d.s.selector = sel[i];
		d.apply();
		d.runTo(d.now + 13 * kStep + 1234);
	}
	d.locate(3.0);
	d.runTo(d.now + 20 * kStep);
	d.playing = false; // onto the free clock under the held keys
	d.runTo(d.now + 20 * kStep);
	d.playing = true;
	d.locate(0.0);
	d.s.selector = 0.55f;
	d.apply();
	d.runTo(d.now + 30 * kStep);
	d.note(Kind::Off, 48);
	d.note(Kind::Off, 55);
	d.runTo(d.now + 8 * kStep);
	MESSAGE("selector hash " << hashOf(d.log) << " over " << d.log.size() << " events");
	CHECK(d.log.size() > 200);
	CHECK(hashOf(d.log) == 1614324029044058665ull); // measured on 42e01f77, before Cycle
}

TEST_CASE("cycle: Next, Previous, Loop and Stop across boundaries — mixed Lengths, Repeats and Rates, each slot's pattern and step against the reference walk") {
	using A = pat::NextAction;
	struct Case {
		const char* what;
		std::array<Tab, pat::kPatterns> tabs;
	};
	const Case cases[] = {
		{ "Next wraps, a 1/8 and a 1/16T among 1/16s",
		  { { { true, 4, 2, A::Next, 7 }, { true, 3, 1, A::Next, 10 }, { true, 6, 1, A::Next, 9 }, { false } } } },
		{ "Previous wraps backwards from the first tab",
		  { { { true, 5, 1, A::Previous, 7 }, { true, 3, 2, A::Previous, 9 }, { false }, { true, 2, 3, A::Previous, 10 } } } },
		{ "Loop holds the second tab for ever",
		  { { { true, 3, 2, A::Next, 7 }, { true, 5, 1, A::Loop, 10 }, { true, 4, 1, A::Next, 7 }, { false } } } },
		{ "Stop after the second tab: silence",
		  { { { true, 4, 1, A::Next, 9 }, { true, 3, 3, A::Stop, 7 }, { true, 4, 1, A::Next, 7 }, { false } } } },
		{ "a 1/16T boundary that is not on the 1/8 grid: the next tab starts on its first step after it",
		  { { { true, 5, 1, A::Next, 9 }, { true, 2, 1, A::Next, 10 }, { false }, { false } } } },
	};
	for (const Case& c : cases) {
		CAPTURE(c.what);
		Drive d;
		setUp(d, c.tabs);
		d.note(Kind::On, 60);
		const int64_t until = 64 * kStep;
		d.runTo(until);
		checkAgainst(d.played, reference(c.tabs, until));
		d.note(Kind::Off, 60);
		d.runTo(d.now + 8 * kStep);
		checkBalanced(d.log);
	}
}

TEST_CASE("cycle: a disabled tab is skipped; one solo pins the walk to it; two solos cycle between them alone") {
	using A = pat::NextAction;
	const std::array<std::array<Tab, pat::kPatterns>, 3> cases { {
		{ { { true, 4, 1, A::Next, 7 }, { false, 3, 1, A::Next, 7 }, { true, 2, 2, A::Next, 10 }, { true, 3, 1, A::Next, 9 } } },
		{ { { true, 4, 1, A::Next, 7 }, { true, 3, 1, A::Next, 7, true }, { true, 2, 2, A::Next, 10 }, { true, 3, 1, A::Next, 9 } } },
		{ { { true, 4, 1, A::Next, 7 }, { false, 3, 2, A::Next, 7, true }, { true, 2, 2, A::Next, 10 }, { true, 3, 1, A::Next, 9, true } } },
	} };
	for (size_t i = 0; i < cases.size(); ++i) {
		CAPTURE(i);
		Drive d;
		setUp(d, cases[i]);
		d.note(Kind::On, 60);
		d.runTo(48 * kStep);
		checkAgainst(d.played, reference(cases[i], 48 * kStep));
		std::set<size_t> heard;
		for (const auto& p : d.played) {
			heard.insert(p.pattern);
		}
		if (i == 0) {
			CHECK(heard == std::set<size_t> { 0, 2, 3 }); // the disabled tab never plays
		}
		if (i == 1) {
			CHECK(heard == std::set<size_t> { 1 }); // pinned
		}
		if (i == 2) {
			CHECK(heard == std::set<size_t> { 1, 3 }); // a solo counts on a disabled tab, as the selector's does
		}
	}
}

TEST_CASE("cycle: a locate, a loop and a scrub land on the walk — the same pattern and step at the same song position as the run from the start") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 5, 2, A::Next, 7 }, { true, 3, 1, A::Previous, 10 },
		                                           { true, 7, 1, A::Next, 9 }, { true, 2, 3, A::Next, 7 } } };
	const int64_t until = 160 * kStep;
	const std::vector<Drive::Played> want = reference(tabs, until);
	// A LOCATE to each of these beats, into a FRESH device each time: what plays from there is the walk's.
	for (const double beats : { 0.0, 1.0, 3.25, 7.5, 13.0, 21.75, 30.0 }) {
		CAPTURE(beats);
		Drive d;
		setUp(d, tabs);
		d.note(Kind::On, 60);
		d.locate(beats);
		const auto at = static_cast<int64_t>(beats * 24000.0);
		d.runTo(until - at);
		std::vector<Drive::Played> shifted;
		for (auto p : d.played) {
			p.sample += at; // the device's samples count from the locate
			shifted.push_back(p);
		}
		checkAgainst(shifted, want, at);
	}
	// A DAW LOOP over beats 6..10, three times round, in ONE device: every pass plays what the run from the start played.
	Drive d;
	setUp(d, tabs);
	d.note(Kind::On, 60);
	d.runTo(6 * 24000);
	for (int pass = 0; pass < 3; ++pass) {
		CAPTURE(pass);
		const size_t from = d.played.size();
		d.locate(6.0);
		d.runTo(d.now + 4 * 24000);
		size_t matched = 0;
		for (size_t i = from; i < d.played.size(); ++i) {
			const auto song = static_cast<int64_t>(std::llround(d.played[i].beats * 24000.0));
			for (const auto& w : want) {
				if (w.sample == song) {
					CHECK(d.played[i].pattern == w.pattern);
					CHECK(d.played[i].step == w.step);
					++matched;
				}
			}
		}
		CHECK(matched == d.played.size() - from);
		CHECK(matched > 10);
	}
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("cycle: Random is a pure function of Seed and the boundary — the same seed walks the same, another seed walks otherwise, and it never repeats the tab it leaves") {
	using A = pat::NextAction;
	std::array<Tab, pat::kPatterns> tabs { { { true, 2, 1, A::Random, 7 }, { true, 3, 1, A::Random, 7 },
		                                     { true, 2, 1, A::Random, 10 }, { true, 1, 2, A::Random, 7 } } };
	const auto walk = [&](int seed, double locateBeats) {
		Drive d;
		setUp(d, tabs);
		d.s.feel.seed = seed;
		d.apply();
		d.note(Kind::On, 60);
		d.locate(locateBeats);
		d.runTo(200 * kStep);
		std::vector<std::pair<int64_t, size_t>> seq; // (song sample, pattern) at each slot
		for (const auto& p : d.played) {
			seq.emplace_back(std::llround(p.beats * 24000.0), p.pattern);
		}
		return seq;
	};
	const auto a = walk(7, 0.0);
	const auto b = walk(7, 0.0);
	CHECK(a == b);
	// THE SAME SEED, LOCATED: every slot after the locate agrees with the run from the start.
	const auto c = walk(7, 9.5);
	size_t agreed = 0;
	for (const auto& [song, p] : c) {
		for (const auto& [s0, p0] : a) {
			if (s0 == song) {
				CHECK(p == p0);
				++agreed;
			}
		}
	}
	CHECK(agreed > 50);
	// ANOTHER SEED: another walk.
	const auto e = walk(8, 0.0);
	CHECK(e != a);
	// NEVER THE TAB IT LEAVES, and every tab reached: the walk's own segments.
	pat::PatternDevice dev;
	Drive probe;
	setUp(probe, tabs);
	probe.s.feel.seed = 7;
	probe.apply();
	pat::CycleWalk w;
	w.configure(probe.d.cycleConfigForTest(true, 0.5));
	std::set<size_t> reached;
	pat::CycleSegment s = w.first();
	for (int i = 0; i < 400; ++i) {
		const pat::CycleSegment n = w.after(s);
		CHECK(n.pattern != s.pattern);
		CHECK(n.pattern < pat::kPatterns);
		CHECK(n.index == s.index + 1);
		reached.insert(n.pattern);
		s = n;
	}
	CHECK(reached.size() == 4);
}

TEST_CASE("cycle: Stop silences from its boundary until playback starts again before it — a locate back, or a fresh free-clock start") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 4, 1, A::Next, 7 }, { true, 4, 2, A::Stop, 7 }, { false }, { false } } };
	Drive d;
	setUp(d, tabs);
	d.note(Kind::On, 60);
	d.runTo(40 * kStep);
	// 4 + 8 steps, then nothing: every slot played is before sample 12 steps; nothing sounds after the last off.
	REQUIRE(!d.played.empty());
	CHECK(d.played.size() == 12);
	CHECK(d.played.back().sample == 11 * kStep);
	CHECK(d.d.cycleStoppedForTest());
	for (const Logged& l : d.log) {
		CHECK(l.sample < 13 * kStep);
	}
	// A LOCATE BACK TO THE START plays again, the walk from the origin.
	const size_t before = d.played.size();
	d.locate(0.0);
	d.runTo(d.now + 6 * kStep);
	REQUIRE(d.played.size() > before);
	CHECK(d.played[before].pattern == 0);
	CHECK(d.played[before].step == 0);
	CHECK_FALSE(d.d.cycleStoppedForTest());
	d.note(Kind::Off, 60);
	d.runTo(d.now + 30 * kStep); // past the stop again
	checkBalanced(d.log);
	// THE FREE CLOCK: stopped at its Stop, a fresh press (its clock restarts at the origin) plays again.
	Drive f;
	f.playing = false;
	f.s.run = feathers::mod::arp::RunMode::Always;
	setUp(f, tabs);
	f.note(Kind::On, 60);
	f.runTo(30 * kStep);
	CHECK(f.played.size() == 12);
	f.note(Kind::Off, 60);
	f.runTo(f.now + 2 * kStep);
	const size_t was = f.played.size();
	f.note(Kind::On, 62);
	f.runTo(f.now + 2 * kStep);
	REQUIRE(f.played.size() > was);
	CHECK(f.played[was].pattern == 0);
	CHECK(f.played[was].step == 0);
	f.note(Kind::Off, 62);
	f.runTo(f.now + 30 * kStep);
	checkBalanced(f.log);
}

TEST_CASE("cycle: a switch lands on the boundary step, the notes sounding then keep their song-position offs, and nothing is left sounding") {
	using A = pat::NextAction;
	// A 1/16 PROGRESSION INTO A 1/8 MELODY: each chord is legato, so the last chord of the progression sounds 2% of a
	// sixteenth past the boundary. Its tones must end THERE — on the 1/16 grid's time, carried onto the 1/8 grid — not be
	// cut at the switch, and not run on to twice as late.
	const std::array<Tab, pat::kPatterns> tabs { { { true, 16, 1, A::Next, 7 }, { true, 2, 1, A::Next, 10 }, { false }, { false } } };
	Drive d;
	setUp(d, tabs);
	d.s.patterns[0].mode = genes::Mode::Progression;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(400 * kStep);
	const int64_t span = static_cast<int64_t>(d.d.cycleForTest(0).length);
	REQUIRE(span > 0);
	const int64_t boundary = span * kStep;
	REQUIRE(boundary < 300 * kStep);
	// THE BOUNDARY: the 1/8 tab's step 0 on its sample, then its step 1 an eighth on, then the progression again.
	size_t at = 0;
	while (at < d.played.size() && d.played[at].pattern == 0) {
		++at;
	}
	REQUIRE(at + 2 < d.played.size());
	CHECK(at == static_cast<size_t>(span));
	CHECK(d.played[at].sample == boundary);
	CHECK(d.played[at].step == 0);
	CHECK(d.played[at + 1].sample == boundary + 12000);
	CHECK(d.played[at + 1].pattern == 1);
	CHECK(d.played[at + 2].sample == boundary + 24000);
	CHECK(d.played[at + 2].pattern == 0);
	CHECK(d.played[at + 2].step == 0);
	// THE OFFS ACROSS IT: every note sounding over the boundary (and not struck again there) ends at the boundary plus the
	// legato overlap of a SIXTEENTH — 2% of 6000 samples, so 120 — never at the boundary, never an eighth's 240.
	std::map<int, int64_t> onAt;
	std::set<int> struckAtBoundary;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On && l.sample == boundary) {
			struckAtBoundary.insert(l.key);
		}
	}
	size_t across = 0;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On) {
			onAt[l.key] = l.sample;
		}
		else if (l.kind == Kind::Off && onAt.count(l.key) != 0 && onAt[l.key] < boundary && l.sample >= boundary
		         && l.sample < boundary + 12000 && struckAtBoundary.count(l.key) == 0) {
			CAPTURE(l.key);
			CHECK(std::llabs(l.sample - (boundary + 120)) <= 1);
			++across;
		}
	}
	CHECK(across > 0);
	d.note(Kind::Off, 60);
	d.runTo(d.now + 40 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("cycle: a change to Repeats, Next, Enable or Length mid-segment waits for the segment's boundary; then the walk is the new row's") {
	using A = pat::NextAction;
	std::array<Tab, pat::kPatterns> tabs { { { true, 4, 2, A::Next, 7 }, { true, 4, 1, A::Next, 7 }, { false }, { false } } };
	Drive d;
	setUp(d, tabs);
	d.note(Kind::On, 60);
	d.runTo(5 * kStep + 100); // inside tab 1's segment (Repeats 2 × 4 steps), its second time through
	// THE NEW ROW: tab 1 once. Taken at once it would put slots 4..7 in TAB 2's segment — so slot 6 would be tab 2's.
	d.s.cycle[0].repeats = 1;
	d.apply();
	d.runTo(16 * kStep - 1);
	// THE OLD ROW TO ITS BOUNDARY AT 8 STEPS (slots 6, 7: tab 1's steps 2, 3); FROM IT, THE NEW ROW, derived from the origin:
	// slots 8..11 are its third segment (tab 1, steps 0..3), slots 12..15 its fourth (tab 2).
	REQUIRE(d.played.size() == 16);
	for (size_t i = 0; i < 16; ++i) {
		CAPTURE(i);
		CHECK(d.played[i].sample == static_cast<int64_t>(i) * kStep);
		CHECK(d.played[i].pattern == (i >= 12 ? 1u : 0u));
		CHECK(d.played[i].step == i % 4);
	}
	// NOW LOOP ON TAB 1, changed inside tab 2's segment: taken at its end (16), and from there tab 1 for ever.
	tabs[0].repeats = 1;
	d.s.cycle[0].next = A::Loop;
	d.apply();
	d.runTo(16 * kStep + 100);
	d.played.clear();
	d.runTo(28 * kStep);
	for (const auto& p : d.played) {
		CAPTURE(p.sample);
		CHECK(p.pattern == 0);
		CHECK(p.step == static_cast<size_t>(p.sample / kStep) % 4);
	}
	// AND THE OPPOSITE WAY: Enable a third tab and change Next while tab 1 loops — taken at the next repeat edge (a Loop's
	// boundary is every Repeats × Length), not before.
	d.s.cycle[0].next = A::Next;
	d.s.patterns[2].enable = true;
	d.s.patterns[2].length = 3;
	d.apply();
	const int64_t changedAt = d.now;
	d.runTo(40 * kStep);
	const int64_t edge = ((changedAt + 4 * kStep - 1) / (4 * kStep)) * (4 * kStep);
	bool sawOther = false;
	for (const auto& p : d.played) {
		if (p.sample < edge) {
			CHECK(p.pattern == 0);
		}
		sawOther = sawOther || p.pattern != 0;
	}
	CHECK(sawOther);
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("cycle: the free clock walks in slots from its own origin — each tab's Repeats × Length of its own steps") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 3, 2, A::Next, 7 }, { true, 2, 1, A::Next, 10 }, { false }, { false } } };
	Drive d;
	d.playing = false;
	d.s.run = feathers::mod::arp::RunMode::Always;
	setUp(d, tabs);
	d.note(Kind::On, 60);
	d.runTo(60 * kStep);
	// THE REFERENCE IN SLOTS: 6 of tab 1 (6000 samples each), 2 of tab 2 (12000 each), round again — within a sample (the
	// free clock re-origins at the call after a rate change).
	std::vector<std::pair<int64_t, size_t>> want;
	int64_t t = 0;
	while (t < 60 * kStep) {
		for (int i = 0; i < 6 && t < 60 * kStep; ++i, t += 6000) {
			want.emplace_back(t, 0);
		}
		for (int i = 0; i < 2 && t < 60 * kStep; ++i, t += 12000) {
			want.emplace_back(t, 1);
		}
	}
	REQUIRE(d.played.size() == want.size());
	for (size_t i = 0; i < want.size(); ++i) {
		CAPTURE(i);
		CHECK(std::llabs(d.played[i].sample - want[i].first) <= 1);
		CHECK(d.played[i].pattern == want[i].second);
	}
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("cycle: a locate far into the song is a bounded walk, and a locate back near it costs at most one stride") {
	pat::CycleConfig c;
	c.count = 4;
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		c.order[p] = static_cast<uint8_t>(p);
		c.span[p] = static_cast<int64_t>(p + 1);
		c.unit[p] = 0.25;
		c.next[p] = pat::NextAction::Random;
	}
	c.seed = 3;
	pat::CycleWalk w;
	w.configure(c);
	// 100 000 beats on: about 160 000 segments of 2.5 steps on average, walked ONCE.
	const pat::CycleSegment far = w.at(100000.0);
	CHECK(far.contains(100000.0));
	const uint64_t walked = w.stepsForTest();
	CHECK(walked > 100000);
	// THE SAME POSITION AGAIN, and one a little before: from a checkpoint, never from the origin.
	const pat::CycleSegment again = w.at(100000.0);
	CHECK(again.index == far.index);
	const pat::CycleSegment back = w.at(99000.0);
	CHECK(back.contains(99000.0));
	CHECK(w.stepsForTest() - walked <= static_cast<uint64_t>(w.strideForTest()));
	// AND IT IS THE SAME SEGMENT a walk from the origin, one step at a time, reaches.
	pat::CycleSegment s = w.first();
	while (!s.contains(99000.0)) {
		s = w.after(s);
	}
	CHECK(s.index == back.index);
	CHECK(s.pattern == back.pattern);
	CHECK(s.first == back.first);
	// A LOOPING WALK never ends its segment, so it costs nothing however far.
	c.next[1] = pat::NextAction::Loop;
	c.next[0] = pat::NextAction::Next;
	w.configure(c);
	const pat::CycleSegment loop = w.at(1.0e7);
	CHECK(loop.pattern == 1);
	CHECK(w.stepsForTest() <= 2);
}

TEST_CASE("cycle: Play Mode, Repeats and Next survive a save into a FRESH instance and play the same there") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 3, 3, A::Previous, 7 }, { true, 2, 5, A::Random, 10 },
		                                           { true, 4, 16, A::Loop, 9 }, { true, 5, 2, A::Stop, 7 } } };
	Drive a;
	setUp(a, tabs);
	a.s.feel.seed = 41;
	a.apply();
	// THE ROW IS WHAT A PATCH STORES, by key: read back from the device's own decode.
	const pat::Row saved = pat::rowOf(a.d.settingsForTest());
	Drive b;
	b.d.setFields(saved.data(), saved.size());
	CHECK(b.d.settingsForTest().playMode == pat::PlayMode::Cycle);
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		CAPTURE(p);
		CHECK(b.d.settingsForTest().cycle[p].repeats == tabs[p].repeats);
		CHECK(b.d.settingsForTest().cycle[p].next == tabs[p].next);
	}
	CHECK(b.d.settingsForTest() == a.d.settingsForTest());
	a.note(Kind::On, 60);
	b.note(Kind::On, 60);
	a.runTo(120 * kStep);
	b.runTo(120 * kStep);
	REQUIRE(a.played.size() > 15);
	REQUIRE(a.played.size() == b.played.size());
	for (size_t i = 0; i < a.played.size(); ++i) {
		CHECK(a.played[i].pattern == b.played[i].pattern);
		CHECK(a.played[i].step == b.played[i].step);
		CHECK(a.played[i].sample == b.played[i].sample);
	}
	// AND THE KEYS ARE IN THE LEDGER'S SHAPE: a key a control.
	CHECK(std::string(pat::kParamDescs[pat::at(pat::Global::PlayMode)].key) == "play_mode");
	CHECK(std::string(pat::kParamDescs[pat::at(2, pat::Field::Repeats)].key) == "p3.repeats");
	CHECK(std::string(pat::kParamDescs[pat::at(3, pat::Field::Next)].key) == "p4.next");
	CHECK(pat::kParamCount == 292); // 242 with Cycle, + Transpose (device + tabs), + each tab's Span, Fill, Start, End, Fill Trigger, + Modulation's four, + Free Phase, + the note range's two
}

TEST_CASE("cycle text: Play Mode and Next print their names and parse back; Repeats reads '3×' and parses '3', '3x', '3×'") {
	const device::TextView view {};
	const device::ParamDesc& mode = pat::kParamDescs[pat::at(pat::Global::PlayMode)];
	CHECK(device::formatValue(mode, 0.0) == "Selector");
	CHECK(device::formatValue(mode, 1.0) == "Cycle");
	CHECK(device::parseValue(mode, "cycle", 0.0) == 1.0);
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		CAPTURE(p);
		const device::ParamDesc& next = pat::kParamDescs[pat::at(p, pat::Field::Next)];
		for (size_t a = 0; a < pat::kNextCount; ++a) {
			CHECK(device::formatValue(next, static_cast<double>(a)) == pat::kNextNames[a]);
			CHECK(device::parseValue(next, pat::kNextNames[a], 0.0) == static_cast<double>(a));
		}
		const size_t rep = pat::at(p, pat::Field::Repeats);
		char buf[64];
		for (int r = 1; r <= pat::kMaxRepeats; ++r) {
			REQUIRE(pat::kTextOps.format(rep, static_cast<double>(r), view, buf, sizeof buf));
			CHECK(std::string(buf) == std::to_string(r) + "×");
			for (const std::string& typed : { std::to_string(r), std::to_string(r) + "x", std::to_string(r) + " ×",
			                                  std::string(buf) }) {
				CAPTURE(typed);
				double v = -1.0;
				CHECK(pat::kTextOps.parse(rep, typed.c_str(), view, &v));
				CHECK(v == static_cast<double>(r));
			}
		}
		double v = -1.0;
		CHECK_FALSE(pat::kTextOps.parse(rep, "often", view, &v));
	}
}

TEST_CASE("seed: it re-rolls Cycle's Random Next from its own salted stream, and leaves every decoded pattern as it was") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 2, 1, A::Random, 7 }, { true, 2, 1, A::Random, 7 },
		                                           { true, 2, 1, A::Random, 7 }, { true, 2, 1, A::Random, 7 } } };
	std::map<int, std::vector<size_t>> walks;
	std::map<int, std::vector<feathers::mod::arp::pattern::PatternCycle>> cycles;
	for (const int seed : { 0, 1, 2, 999 }) {
		Drive d;
		setUp(d, tabs);
		d.s.feel.seed = seed;
		d.apply();
		d.note(Kind::On, 60);
		d.runTo(120 * kStep);
		for (size_t i = 0; i < d.played.size(); i += 2) {
			walks[seed].push_back(d.played[i].pattern); // one entry a segment (2 steps each)
		}
		for (size_t p = 0; p < pat::kPatterns; ++p) {
			cycles[seed].push_back(d.d.cycleForTest(p));
		}
	}
	// EVERY SEED WALKS ITS OWN WAY ...
	CHECK(walks[0] != walks[1]);
	CHECK(walks[1] != walks[2]);
	CHECK(walks[2] != walks[999]);
	// ... AND THE PATTERNS ARE THE SAME AT EVERY SEED (Seed is the dice's, not the terrain's).
	for (const int seed : { 1, 2, 999 }) {
		for (size_t p = 0; p < pat::kPatterns; ++p) {
			CAPTURE(seed);
			CAPTURE(p);
			const auto& a = cycles[0][p];
			const auto& b = cycles[seed][p];
			REQUIRE(a.length == b.length);
			for (size_t k = 0; k < a.length; ++k) {
				CHECK(static_cast<int>(a.steps[k].kind) == static_cast<int>(b.steps[k].kind));
				CHECK(a.steps[k].keys[0] == b.steps[k].keys[0]);
				CHECK(a.steps[k].velocity == b.steps[k].velocity);
				CHECK(a.steps[k].gate == b.steps[k].gate);
			}
		}
	}
	// THE STREAM IS SALTED: Random's pick at a boundary is not the raw seed's hash (the stream Humanize would read).
	pat::CycleConfig c;
	c.count = 4;
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		c.order[p] = static_cast<uint8_t>(p);
		c.next[p] = A::Random;
	}
	size_t differ = 0;
	for (int seed = 0; seed < 64; ++seed) {
		c.seed = seed;
		std::vector<size_t> a;
		std::vector<size_t> b;
		pat::CycleConfig other = c;
		other.seed = seed + 1;
		for (int64_t i = 1; i < 32; ++i) {
			a.push_back(pat::cycleNext(c, 0, i));
			b.push_back(pat::cycleNext(other, 0, i));
		}
		differ += a != b ? 1u : 0u;
	}
	CHECK(differ == 64);
	// SEED IS NOT GATED ON HUMANIZE ANY MORE: it is always a live control.
	CHECK(pat::kParamDescs[pat::at(pat::Global::Seed)].gateControl < 0);
}

TEST_CASE("cycle: the decode memo keys on Cycle's fields — a change of Play Mode, Repeats or Next alone is decoded and seen") {
	Drive d;
	const uint64_t before = d.d.decodesForTest();
	d.apply(); // the same bits: no decode
	CHECK(d.d.decodesForTest() == before);
	d.s.playMode = pat::PlayMode::Cycle;
	d.apply();
	CHECK(d.d.decodesForTest() == before + 1);
	CHECK(d.d.settingsForTest().playMode == pat::PlayMode::Cycle);
	d.s.cycle[2].repeats = 7;
	d.apply();
	CHECK(d.d.decodesForTest() == before + 2);
	CHECK(d.d.settingsForTest().cycle[2].repeats == 7);
	d.s.cycle[3].next = pat::NextAction::Stop;
	d.apply();
	CHECK(d.d.decodesForTest() == before + 3);
	CHECK(d.d.settingsForTest().cycle[3].next == pat::NextAction::Stop);
}

TEST_CASE("cycle: each tab's own transpose switches with it — at the boundary step, and nowhere else") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 4, 1, A::Next, 7 }, { true, 4, 1, A::Next, 7 }, { false }, { false } } };
	const auto run = [&](std::array<int, 2> t0, std::array<int, 2> t1) {
		Drive d;
		setUp(d, tabs);
		d.s.patternTranspose[0] = t0;
		d.s.patternTranspose[1] = t1;
		d.apply();
		d.note(Kind::On, 60);
		d.runTo(32 * kStep);
		std::map<int64_t, int> ons; // sample -> key
		for (const Logged& l : d.log) {
			if (l.kind == Kind::On) {
				ons[l.sample] = l.key;
			}
		}
		return std::pair { ons, d.played };
	};
	const auto [plainOns, plainPlayed] = run({ 0, 0 }, { 0, 0 });
	const auto [movedOns, movedPlayed] = run({ 0, 0 }, { 0, 1 }); // tab 2 an octave up
	REQUIRE(plainOns.size() == movedOns.size());
	REQUIRE(plainPlayed.size() == movedPlayed.size());
	size_t up = 0;
	size_t same = 0;
	for (const auto& p : plainPlayed) {
		const auto it = plainOns.find(p.sample);
		if (it == plainOns.end()) {
			continue; // a rest
		}
		CAPTURE(p.sample);
		// TAB 2'S SEGMENTS (steps 4-7, 12-15, ...) AN OCTAVE UP, TAB 1'S AS THEY WERE — the switch exactly at the boundary.
		const bool tab2 = (p.sample / kStep / 4) % 2 == 1;
		CHECK(p.pattern == (tab2 ? 1u : 0u));
		CHECK(movedOns.at(p.sample) == it->second + (tab2 ? 12 : 0));
		(tab2 ? up : same) += 1;
	}
	CHECK(up > 4);
	CHECK(same > 4);
}

//------------------------------------------------------------------------------------------------//
// SPAN, FILL AND THE PHASE MARKERS (design §7.8)                                              //
//------------------------------------------------------------------------------------------------//

namespace {

/// WHAT EACH SLOT PLAYED, as (pattern, step), sixteenth by sixteenth from the origin — rests included (the playhead).
struct Run {
	std::vector<Logged> log;
	std::vector<Drive::Played> played;
};

std::vector<std::pair<size_t, size_t>> slots(const std::vector<Drive::Played>& played, int64_t count) {
	std::vector<std::pair<size_t, size_t>> out(static_cast<size_t>(count), { 99, 99 });
	for (const auto& p : played) {
		const int64_t s = std::llround(p.beats * 4.0); // a sixteenth a slot at 120 bpm
		if (s >= 0 && s < count && p.sample == s * kStep) {
			out[static_cast<size_t>(s)] = { p.pattern, p.step };
		}
	}
	return out;
}

/// THE SPEC'S EXAMPLE: with all 4 tabs on Next and tab 4 a half-bar fill, the walk goes 1 -> 2 -> 3 (first half) ->
/// 4 (fill) -> 1. Three 16-step sixteenth tabs and a half-bar (8-step) fill, every Next; `start`/`end` its window.
Run walkExample(int start, int end, int64_t until) {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 16, 1, A::Next, 7 }, { true, 16, 1, A::Next, 7 },
		                                           { true, 16, 1, A::Next, 7 }, { true, 8, 1, A::Next, 7 } } };
	Drive d;
	setUp(d, tabs);
	d.s.phase[3].fill = true;
	d.s.phase[3].start = start;
	d.s.phase[3].end = end;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(until);
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	return Run { d.log, d.played };
}

/// THE EXPECTED SLOTS OF THE SPEC'S EXAMPLE with the fill's window [start, end), restated by hand: 1, 2, 3 a bar each; the fill
/// laid so its last step ends with 3's bar (fill step k at slot 40 + k), its window sounding in 3's place; then 1 again.
std::vector<std::pair<size_t, size_t>> walkExpected(int start, int end, int64_t count) {
	std::vector<std::pair<size_t, size_t>> out;
	for (int64_t s = 0; s < count; ++s) {
		const int64_t bar = s / 16;
		const int64_t k = s % 16;
		const auto tab = static_cast<size_t>(bar % 3);
		if (tab == 2 && k >= 8 + start && k < 8 + end) {
			out.emplace_back(3, static_cast<size_t>(k - 8)); // the fill's step k − 8
		}
		else {
			out.emplace_back(tab, static_cast<size_t>(k));
		}
	}
	return out;
}

} // namespace

TEST_CASE("fill: the spec's example by value over two cycles — 1, 2, 3's first half, the fill over 3's second half, 1 — and End inside the slot hands back to the rest of 3") {
	const int64_t count = 96; // two walks round: six bars
	for (const auto& [start, end] : { std::pair { 0, 8 }, std::pair { 0, 4 }, std::pair { 2, 6 }, std::pair { 6, 64 } }) {
		CAPTURE(start);
		CAPTURE(end);
		const Run d = walkExample(start, end, count * kStep);
		const auto got = slots(d.played, count);
		const auto want = walkExpected(start, std::min(end, 8), count);
		for (int64_t s = 0; s < count; ++s) {
			CAPTURE(s);
			CHECK(got[static_cast<size_t>(s)] == want[static_cast<size_t>(s)]);
		}
		// THE IDENTITY GATES (design §7.10) by value at every slot: the tab the restated walk says sounds reads 1, the
		// others 0 — the fill's tab during its window, and 3 not then.
		for (const auto& p : d.played) {
			const int64_t s = std::llround(p.beats * 4.0);
			if (s < 0 || s >= count) {
				continue;
			}
			CAPTURE(s);
			for (size_t t = 0; t < pat::kPatterns; ++t) {
				CAPTURE(t);
				CHECK(p.gates[t] == (t == want[static_cast<size_t>(s)].first ? 1.0f : 0.0f));
			}
		}
		checkBalanced(d.log);
	}
	// THE DEFAULT END (64, the most a cycle has) IS THE FILL'S LENGTH: the same slots as End = 8, event for event.
	const Run a = walkExample(0, 64, count * kStep);
	const Run b = walkExample(0, 8, count * kStep);
	REQUIRE(a.log.size() == b.log.size());
	for (size_t i = 0; i < a.log.size(); ++i) {
		CHECK(a.log[i].key == b.log[i].key);
		CHECK(a.log[i].sample == b.log[i].sample);
	}
}

TEST_CASE("fill: End never falls at or before Start — the window clamps to one step; and the notes cut at both edges of the window, none stuck") {
	const int64_t count = 48;
	const Run d = walkExample(5, 3, count * kStep);
	const auto got = slots(d.played, count);
	const auto want = walkExpected(5, 6, count); // [5, 6): one step
	for (int64_t s = 0; s < count; ++s) {
		CAPTURE(s);
		CHECK(got[static_cast<size_t>(s)] == want[static_cast<size_t>(s)]);
	}
	// AT THE CUTS: every note struck by 3 before the window has ended by the window's first sample; every fill note by its
	// last; and nothing is left sounding.
	std::map<int, int64_t> onAt;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On) {
			onAt[l.key] = l.sample;
		}
		else if (l.kind == Kind::Off && onAt.count(l.key) != 0) {
			const int64_t on = onAt[l.key];
			if (on >= 32 * kStep && on < 45 * kStep) {
				CHECK(l.sample <= 45 * kStep);
			}
			if (on == 45 * kStep) {
				CHECK(l.sample <= 46 * kStep);
			}
		}
	}
	checkBalanced(d.log);
}

TEST_CASE("fill edges: two fills in a row (the first merges, the second is passed), a fill first in the order (the walk starts on a pattern), every tab a fill (they play as patterns)") {
	using A = pat::NextAction;
	const auto run = [](std::array<bool, pat::kPatterns> fills, std::array<size_t, pat::kPatterns> lengths, int64_t count) {
		std::array<Tab, pat::kPatterns> tabs {};
		for (size_t p = 0; p < pat::kPatterns; ++p) {
			tabs[p] = { true, lengths[p], 1, A::Next, 7 };
		}
		Drive d;
		setUp(d, tabs);
		for (size_t p = 0; p < pat::kPatterns; ++p) {
			d.s.phase[p].fill = fills[p];
		}
		d.apply();
		d.note(Kind::On, 60);
		d.runTo(count * kStep);
		return slots(d.played, count);
	};
	// 1 NORMAL, 2 AND 3 FILLS (4 steps each), 4 NORMAL: 1, with fill 2 over its last 4; fill 3 passed; 4; then 1 again.
	{
		const auto got = run({ false, true, true, false }, { 8, 4, 4, 8 }, 32);
		for (int64_t s = 0; s < 32; ++s) {
			CAPTURE(s);
			const int64_t k = s % 8;
			const bool one = (s / 8) % 2 == 0;
			std::pair<size_t, size_t> want { one ? 0u : 3u, static_cast<size_t>(k) };
			if (one && k >= 4) {
				want = { 1u, static_cast<size_t>(k - 4) };
			}
			CHECK(got[static_cast<size_t>(s)] == want);
		}
	}
	// A FILL FIRST: tab 1 a fill — the walk starts on 2, then 3 with fill 1 over its tail, then 2.
	{
		const auto got = run({ true, false, false, false }, { 4, 8, 8, 8 }, 48);
		CHECK(got[0] == std::pair<size_t, size_t> { 1, 0 });
		CHECK(got[8] == std::pair<size_t, size_t> { 2, 0 });
		CHECK(got[16] == std::pair<size_t, size_t> { 3, 0 });
		CHECK(got[20] == std::pair<size_t, size_t> { 0, 0 }); // fill 1 over 4's last four (4 → next is 1, a fill → then 2)
		CHECK(got[24] == std::pair<size_t, size_t> { 1, 0 });
	}
	// EVERY TAB A FILL: they play as the patterns they are.
	{
		const auto got = run({ true, true, true, true }, { 4, 4, 4, 4 }, 16);
		for (int64_t s = 0; s < 16; ++s) {
			CHECK(got[static_cast<size_t>(s)] == std::pair<size_t, size_t> { static_cast<size_t>(s / 4), static_cast<size_t>(s % 4) });
		}
	}
}

TEST_CASE("start phase: a normal tab with Start 4 of 16 plays 4..15 then 0..3 in its slot, every cycle, and its slot does not move") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 16, 1, A::Next, 7 }, { true, 8, 2, A::Next, 7 }, { false }, { false } } };
	Drive d;
	setUp(d, tabs);
	d.s.phase[0].start = 4;
	d.s.phase[1].start = 3;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(96 * kStep);
	const auto got = slots(d.played, 96);
	for (int64_t s = 0; s < 96; ++s) {
		CAPTURE(s);
		const int64_t at = s % 32;
		const std::pair<size_t, size_t> want = at < 16 ? std::pair<size_t, size_t> { 0, static_cast<size_t>((at + 4) % 16) }
		                                               : std::pair<size_t, size_t> { 1, static_cast<size_t>((at - 16 + 3) % 8) };
		CHECK(got[static_cast<size_t>(s)] == want);
	}
	checkBalanced(d.log);
}

TEST_CASE("span: each tab's slot is whole bars, the pattern looping inside it from the slot's start, so mixed Lengths stay on the bar line — and what sounds at a slot's end is cut there") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 5, 1, A::Next, 7 }, { true, 3, 2, A::Next, 10 }, { true, 7, 1, A::Next, 7 }, { false } } };
	Drive d;
	setUp(d, tabs);
	d.s.phase[0].spanBars = 1; // 16 sixteenths: 5-step pattern ×3 + 1
	d.s.phase[1].spanBars = 1; // Repeats 2: two 1-bar slots of an eighth pattern (8 steps each), each restarting it
	d.s.phase[2].spanBars = 2; // 32 sixteenths of a 7-step pattern
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(64 * kStep + 8);
	// THE BOUNDARIES: 0 (tab 1), bar 1 and bar 2 (tab 2's two slots), bar 3 (tab 3, two bars), bar 5 (tab 1 again) — every one
	// on a bar line, whatever the lengths.
	std::map<int64_t, std::pair<size_t, size_t>> at;
	for (const auto& p : d.played) {
		at[p.sample] = { p.pattern, p.step };
	}
	CHECK(at.at(0) == std::pair<size_t, size_t> { 0, 0 });
	CHECK(at.at(15 * kStep) == std::pair<size_t, size_t> { 0, 0 }); // 15 % 5
	CHECK(at.at(16 * kStep) == std::pair<size_t, size_t> { 1, 0 });
	CHECK(at.at(30 * kStep) == std::pair<size_t, size_t> { 1, 1 }); // an eighth pattern: 7 % 3
	CHECK(at.at(32 * kStep) == std::pair<size_t, size_t> { 1, 0 }); // its second Span slot restarts it
	CHECK(at.at(48 * kStep) == std::pair<size_t, size_t> { 2, 0 });
	CHECK(at.at(63 * kStep) == std::pair<size_t, size_t> { 2, 1 }); // 15 % 7
	REQUIRE(at.count(80 * kStep) == 0); // not reached yet
	d.runTo(96 * kStep);
	for (const auto& p : d.played) {
		at[p.sample] = { p.pattern, p.step };
	}
	CHECK(at.at(80 * kStep) == std::pair<size_t, size_t> { 0, 0 }); // bar 5: tab 1 again, after tab 3's two bars
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("span cuts at the slot's end: a legato Progression in a one-bar Span ends every chord by the bar line, and nothing hangs on") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 16, 1, A::Next, 7 }, { true, 16, 1, A::Next, 7 }, { false }, { false } } };
	for (const int span : { 0, 1 }) {
		CAPTURE(span);
		Drive d;
		setUp(d, tabs);
		d.s.patterns[0].mode = genes::Mode::Progression;
		d.s.phase[0].spanBars = span;
		d.s.phase[1].spanBars = span;
		d.apply();
		d.note(Kind::On, 60);
		d.runTo(64 * kStep);
		const int64_t boundary = span == 1 ? 16 * kStep : static_cast<int64_t>(d.d.cycleForTest(0).length) * kStep;
		std::map<int, int64_t> onAt;
		size_t atBoundary = 0;
		size_t past = 0;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::On) {
				onAt[l.key] = l.sample;
			}
			else if (l.kind == Kind::Off && onAt.count(l.key) != 0 && onAt[l.key] < boundary) {
				atBoundary += l.sample == boundary ? 1u : 0u;
				past += l.sample > boundary ? 1u : 0u;
			}
		}
		if (span == 1) {
			CHECK(past == 0);      // CUT: nothing struck in the slot sounds past its end
			CHECK(atBoundary > 0); // and the cut is real (a legato chord was sounding there)
		}
		d.note(Kind::Off, 60);
		d.runTo(d.now + 16 * kStep);
		checkBalanced(d.log);
	}
}

TEST_CASE("fills and phases are scrub-safe: a locate anywhere plays what the run from the start played there") {
	const int64_t count = 96;
	const auto want = walkExpected(2, 7, count);
	for (const double beats : { 2.0, 9.0, 10.5, 11.0, 13.75, 20.0 }) {
		CAPTURE(beats);
		using A = pat::NextAction;
		const std::array<Tab, pat::kPatterns> tabs { { { true, 16, 1, A::Next, 7 }, { true, 16, 1, A::Next, 7 },
			                                           { true, 16, 1, A::Next, 7 }, { true, 8, 1, A::Next, 7 } } };
		Drive d;
		setUp(d, tabs);
		d.s.phase[3].fill = true;
		d.s.phase[3].start = 2;
		d.s.phase[3].end = 7;
		d.apply();
		d.note(Kind::On, 60);
		d.locate(beats);
		d.runTo(count * kStep - static_cast<int64_t>(beats * 24000.0));
		size_t matched = 0;
		for (const auto& p : d.played) {
			const int64_t s = std::llround(p.beats * 4.0);
			if (s < count) {
				CHECK(std::pair<size_t, size_t> { p.pattern, p.step } == want[static_cast<size_t>(s)]);
				++matched;
			}
		}
		CHECK(matched > 10);
		checkBalanced(d.log);
	}
}

TEST_CASE("fill trigger: a rise arms the fill, which plays over the tail of the CURRENT repeat (Selector too), then the pattern carries on; a rise while one is armed is ignored") {
	Drive d;
	d.s.patterns[0].length = 16;
	d.s.patterns[0].zone = 300;
	d.s.patterns[1].enable = false; // a fill need not be in range to be triggered
	d.s.patterns[1].length = 8;
	d.s.patterns[1].zone = 700;
	d.s.phase[1].fill = true;
	d.s.phase[1].start = 2;
	d.s.phase[1].end = 6;
	for (auto& q : d.s.patterns) {
		q.pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
	}
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(3 * kStep + 10);
	const auto fire = [&] {
		d.s.fillTrigger[1] = 1.0f;
		d.apply();
		d.s.fillTrigger[1] = 0.0f;
		d.apply();
	};
	fire();
	CHECK(d.d.armedFillForTest() == 1);
	fire(); // ignored: one is armed
	d.runTo(64 * kStep);
	// THE FILL'S WINDOW [2, 6) of its 8 steps, laid so its last step ends with the repeat at slot 16: slots 10..13.
	const auto got = slots(d.played, 64);
	for (int64_t s = 0; s < 64; ++s) {
		CAPTURE(s);
		const bool fill = s >= 10 && s < 14;
		const std::pair<size_t, size_t> want = fill ? std::pair<size_t, size_t> { 1, static_cast<size_t>(s - 8) }
		                                            : std::pair<size_t, size_t> { 0, static_cast<size_t>(s % 16) };
		CHECK(got[static_cast<size_t>(s)] == want);
	}
	CHECK(d.d.armedFillForTest() == pat::kPatterns);
	CHECK(d.d.activeFillForTest() == pat::kPatterns);
	// FIRED PAST ITS WINDOW'S START (slot 12 of a repeat): it waits for the next repeat's end, slots 26..29 of the next.
	d.runTo(64 * kStep + 12 * kStep + 10);
	fire();
	d.runTo(96 * kStep);
	const auto later = slots(d.played, 96);
	CHECK(later[74].first == 0); // slot 64 + 10: this repeat's window had begun before the fire
	CHECK(later[90] == std::pair<size_t, size_t> { 1, 2 }); // the next repeat's window, 80 + 10
	CHECK(later[93] == std::pair<size_t, size_t> { 1, 5 });
	CHECK(later[94] == std::pair<size_t, size_t> { 0, 14 });
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("fill at its OWN rate (by design): an eighth-note fill over a sixteenth bar ends with the bar, its steps on its own grid; End hands back to the bar's own step there; a triplet fill too; and a triggered fill likewise") {
	using A = pat::NextAction;
	// THE SAMPLES OF EVERY SLOT PLAYED, as (sample, pattern, step).
	const auto run = [](size_t fillBase, int fillLength, int start, int end, int64_t until) {
		const std::array<Tab, pat::kPatterns> tabs { { { true, 16, 1, A::Next, 7 }, { true, 16, 1, A::Next, 7 },
			                                           { true, 16, 1, A::Next, 7 },
			                                           { true, static_cast<size_t>(fillLength), 1, A::Next, fillBase } } };
		Drive d;
		setUp(d, tabs);
		d.s.phase[3].fill = true;
		d.s.phase[3].start = start;
		d.s.phase[3].end = end;
		d.apply();
		d.note(Kind::On, 60);
		d.runTo(until);
		d.note(Kind::Off, 60);
		d.runTo(d.now + 8 * kStep);
		checkBalanced(d.log);
		std::vector<std::tuple<int64_t, size_t, size_t>> out;
		for (const auto& p : d.played) {
			out.emplace_back(p.sample, p.pattern, p.step);
		}
		return out;
	};
	const auto check = [](const std::vector<std::tuple<int64_t, size_t, size_t>>& got,
	                      const std::vector<std::tuple<int64_t, size_t, size_t>>& want) {
		REQUIRE(got.size() == want.size());
		for (size_t i = 0; i < want.size(); ++i) {
			CAPTURE(i);
			CHECK(got[i] == want[i]);
		}
	};
	// THE EXPECTED: tabs 1 and 2 a bar of sixteenths each; tab 3's bar until the fill; the fill's steps at its own spacing,
	// ending with the bar (sample 288000 = beat 12); then tab 3's own steps again if End leaves room; then tab 1.
	const auto expect = [](int64_t fillSpacing, int fillLength, int start, int end) {
		std::vector<std::tuple<int64_t, size_t, size_t>> want;
		for (int64_t s = 0; s < 32; ++s) {
			want.emplace_back(s * kStep, static_cast<size_t>(s / 16), static_cast<size_t>(s % 16));
		}
		const int64_t barEnd = 48 * kStep;
		const int64_t from = barEnd - static_cast<int64_t>(fillLength - start) * fillSpacing;
		const int64_t to = barEnd - static_cast<int64_t>(fillLength - end) * fillSpacing;
		for (int64_t s = 32; s < 48; ++s) {
			if (s * kStep < from || s * kStep >= to) {
				want.emplace_back(s * kStep, 2, static_cast<size_t>(s - 32));
			}
			if (s * kStep < from && (s + 1) * kStep > from) {
				// (a fill window between two sixteenths would start here: not in these cases)
			}
		}
		for (int k = start; k < end; ++k) {
			want.emplace_back(barEnd - static_cast<int64_t>(fillLength - k) * fillSpacing, 3, static_cast<size_t>(k));
		}
		want.emplace_back(barEnd, 0, 0);
		std::sort(want.begin(), want.end());
		return want;
	};
	// AN EIGHTH-NOTE FILL OF 4 STEPS (half a bar), its whole window, then End = 2.
	check(run(10, 4, 0, 4, 48 * kStep + 1), expect(12000, 4, 0, 4));
	check(run(10, 4, 0, 2, 48 * kStep + 1), expect(12000, 4, 0, 2));
	check(run(10, 4, 1, 3, 48 * kStep + 1), expect(12000, 4, 1, 3));
	// A SIXTEENTH-TRIPLET FILL OF 6 (one beat).
	check(run(9, 6, 0, 6, 48 * kStep + 1), expect(4000, 6, 0, 6));
	// TRIGGERED, under Selector: tab 1 a bar of sixteenths, tab 2 an eighth-note fill of 4 — over the current repeat's
	// second half at its own spacing, then tab 1 carries on.
	Drive d;
	d.s.patterns[0].length = 16;
	d.s.patterns[0].zone = 300;
	d.s.patterns[1].enable = false;
	d.s.patterns[1].length = 4;
	d.s.patterns[1].timeBase = 10;
	d.s.patterns[1].zone = 700;
	d.s.phase[1].fill = true;
	for (auto& q : d.s.patterns) {
		q.pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
	}
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(2 * kStep + 10);
	d.s.fillTrigger[1] = 1.0f;
	d.apply();
	d.s.fillTrigger[1] = 0.0f;
	d.apply();
	d.runTo(32 * kStep);
	std::vector<std::tuple<int64_t, size_t, size_t>> got;
	for (const auto& p : d.played) {
		got.emplace_back(p.sample, p.pattern, p.step);
	}
	std::vector<std::tuple<int64_t, size_t, size_t>> want;
	for (int64_t s = 0; s < 32; ++s) {
		if (s < 8 || s >= 16) {
			want.emplace_back(s * kStep, 0, static_cast<size_t>(s % 16));
		}
	}
	for (int k = 0; k < 4; ++k) {
		want.emplace_back(8 * kStep + k * 12000, 1, static_cast<size_t>(k));
	}
	std::sort(want.begin(), want.end());
	check(got, want);
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("fill at its own rate, between the slots under it: a three-sixteenth fill over a bar of eighths starts on its own sixteenth (the look-ahead), with the eighth before it cut there") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 8, 1, A::Next, 10 }, { true, 3, 1, A::Next, 7 }, { false }, { false } } };
	Drive d;
	setUp(d, tabs);
	d.s.phase[1].fill = true;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(48000 + 10);
	// TAB 1: eighths at 0, 12000, ..., 72000 (its bar runs to 96000); the fill's sixteenths at 96000 − 3·6000 = 78000,
	// 84000, 90000 — between tab 1's eighths at 72000 and 84000; tab 1's eighth at 84000 is the fill's.
	std::vector<std::tuple<int64_t, size_t, size_t>> got;
	for (const auto& p : d.played) {
		got.emplace_back(p.sample, p.pattern, p.step);
	}
	const std::vector<std::tuple<int64_t, size_t, size_t>> want {
		{ 0, 0, 0 }, { 12000, 0, 1 }, { 24000, 0, 2 }, { 36000, 0, 3 }, { 48000, 0, 4 },
	};
	REQUIRE(got.size() == want.size());
	d.runTo(96000 + 10);
	got.clear();
	for (const auto& p : d.played) {
		got.emplace_back(p.sample, p.pattern, p.step);
	}
	const std::vector<std::tuple<int64_t, size_t, size_t>> bar {
		{ 0, 0, 0 },     { 12000, 0, 1 }, { 24000, 0, 2 }, { 36000, 0, 3 }, { 48000, 0, 4 }, { 60000, 0, 5 },
		{ 72000, 0, 6 }, { 78000, 1, 0 }, { 84000, 1, 1 }, { 90000, 1, 2 }, { 96000, 0, 0 },
	};
	REQUIRE(got.size() == bar.size());
	for (size_t i = 0; i < bar.size(); ++i) {
		CAPTURE(i);
		CHECK(got[i] == bar[i]);
	}
	// THE EIGHTH AT 72000 IS CUT WHERE THE FILL COMES IN (78000), whatever its gate.
	int64_t on72 = -1;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On && l.sample == 72000) {
			on72 = l.key;
		}
		if (l.kind == Kind::Off && l.key == on72 && on72 >= 0 && l.sample > 72000) {
			CHECK(l.sample <= 78000);
			on72 = -1;
		}
	}
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

//------------------------------------------------------------------------------------------------//
// Identity outputs and the Modulation mode (design §7.10)                                        //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE LANE LEVELS RESTATED from a decoded cycle: a hit is its velocity; between hits the level holds; the steps before a
/// lane's first hit hold its LAST hit's level (the loop wraps). `live`: the lanes that hit at all and are on.
struct Levels {
	std::vector<std::array<double, 8>> at;
	uint8_t live = 0;
};
Levels restate(const feathers::mod::arp::pattern::PatternCycle& c, uint8_t on) {
	namespace pn = feathers::mod::arp::pattern;
	Levels out;
	out.at.resize(c.length);
	for (size_t l = 0; l < 8; ++l) {
		std::vector<int> hits;
		for (size_t k = 0; k < c.length; ++k) {
			if (c.steps[k].kind == pn::PatternStep::Kind::Note && ((c.steps[k].drumHits >> l) & 1u) != 0u) {
				hits.push_back(static_cast<int>(k));
			}
		}
		for (size_t k = 0; k < c.length; ++k) {
			int last = hits.empty() ? -1 : hits.back(); // wraps: before the first hit, the cycle's last
			for (const int h : hits) {
				if (h <= static_cast<int>(k)) {
					last = h;
				}
			}
			out.at[k][l] = last < 0 ? 0.0 : static_cast<double>(c.steps[static_cast<size_t>(last)].drumVelocity[l]);
		}
		if (!hits.empty() && ((on >> l) & 1u) != 0u) {
			out.live = static_cast<uint8_t>(out.live | (1u << l));
		}
	}
	return out;
}

/// A HAND-MADE FOUR-STEP CYCLE: lane 0 hits at 1 (0.6) and 3 (0.2); lane 1 never; lane 2 at 0 (1.0).
feathers::mod::arp::pattern::PatternCycle handCycle() {
	namespace pn = feathers::mod::arp::pattern;
	pn::PatternCycle c;
	c.length = 4;
	c.playable = true;
	const auto hit = [&](size_t k, size_t l, float v) {
		c.steps[k].kind = pn::PatternStep::Kind::Note;
		c.steps[k].drumHits = static_cast<uint8_t>(c.steps[k].drumHits | (1u << l));
		c.steps[k].drumVelocity[l] = v;
	};
	hit(1, 0, 0.6f);
	hit(3, 0, 0.2f);
	hit(0, 2, 1.0f);
	return c;
}

/// ONE MODULATION TAB alone under the transport, no key held: sixteenths, `length` steps, Hold, the Drum decode's place.
void setUpModulation(Drive& d, size_t length) {
	auto& q = d.s.patterns[0];
	q.mode = genes::Mode::Modulation;
	q.length = length;
	q.timeBase = 7;
	q.zone = 300;
	d.apply();
}

/// The device read at sample `t`: its last call is AT `t`.
void callAt(Drive& d, int64_t t) {
	d.runTo(t);
	d.runTo(t + 1);
}

} // namespace

TEST_CASE("modulation levels: a hit is its velocity, the level holds between hits and wraps round the loop; a lane that never hits, or is off, is not live") {
	namespace pn = feathers::mod::arp::pattern;
	const pn::PatternCycle c = handCycle();
	pat::ModLevels lv {};
	uint8_t live = 0;
	pat::modLevelsOf(c, 0xFF, lv, &live);
	CHECK(live == 0b101);
	const double lane0[] = { 0.2, 0.6, 0.6, 0.2 }; // step 0 holds step 3's hit: the loop wraps
	for (size_t k = 0; k < 4; ++k) {
		CAPTURE(k);
		CHECK(std::abs(lv[k][0] - lane0[k]) < 1e-6);
		CHECK(std::abs(lv[k][2] - 1.0) < 1e-6);
	}
	pat::modLevelsOf(c, 0b001, lv, &live);
	CHECK(live == 0b001);
	// AGAINST THE RESTATEMENT, too.
	const Levels r = restate(c, 0xFF);
	pat::modLevelsOf(c, 0xFF, lv, &live);
	CHECK(live == r.live);
	for (size_t k = 0; k < 4; ++k) {
		for (size_t l = 0; l < 8; ++l) {
			if (((r.live >> l) & 1u) != 0u) {
				CHECK(std::abs(lv[k][l] - r.at[k][l]) < 1e-6);
			}
		}
	}
}

TEST_CASE("modulation smoothing: Hold is the step's level; Ramp arrives on the next step's level over the last share of the step; Glide is the periodic one-pole slew, continuous across every step") {
	namespace pn = feathers::mod::arp::pattern;
	const pn::PatternCycle c = handCycle();
	pat::ModLevels lv {};
	uint8_t live = 0;
	pat::modLevelsOf(c, 0xFF, lv, &live);
	using S = pat::ModSmooth;
	for (const double f : { 0.0, 0.3, 0.99 }) {
		CHECK(std::abs(pat::modLevelAt(lv, 4, 2, f, 0, S::Hold, 1.0) - 0.6) < 1e-6);
	}
	// RAMP, time 0.5: flat for the first half, then a line from 0.6 (step 2) to 0.2 (step 3), arriving at the step's end.
	CHECK(std::abs(pat::modLevelAt(lv, 4, 2, 0.25, 0, S::Ramp, 0.5) - 0.6) < 1e-6);
	CHECK(std::abs(pat::modLevelAt(lv, 4, 2, 0.75, 0, S::Ramp, 0.5) - 0.4) < 1e-6);
	CHECK(std::abs(pat::modLevelAt(lv, 4, 2, 1.0, 0, S::Ramp, 0.5) - 0.2) < 1e-6);
	CHECK(std::abs(pat::modLevelAt(lv, 4, 3, 1.0, 0, S::Ramp, 4.0) - 0.2) < 1e-6); // step 3 → step 0 (wraps), 0.2 → 0.2
	CHECK(std::abs(pat::modLevelAt(lv, 4, 1, 0.5, 0, S::Ramp, 4.0) - 0.6) < 1e-6);  // Ramp reads at most the whole step
	CHECK(std::abs(pat::modLevelAt(lv, 4, 3, 0.5, 0, S::Ramp, 4.0) - 0.2) < 1e-6);
	CHECK(std::abs(pat::modLevelAt(lv, 4, 2, 0.5, 0, S::Ramp, 4.0) - 0.4) < 1e-6);
	// GLIDE, against the slew SIMULATED step by step for many loops from 0: each step starts where the last ended.
	for (const double tau : { 0.25, 1.0, 4.0 }) {
		CAPTURE(tau);
		double v = 0.0;
		std::array<double, 4> startOf {};
		for (int loop = 0; loop < 400; ++loop) {
			for (size_t k = 0; k < 4; ++k) {
				startOf[k] = v;
				v = lv[k][0] + (v - lv[k][0]) * std::exp(-1.0 / tau);
			}
		}
		for (size_t k = 0; k < 4; ++k) {
			CAPTURE(k);
			CHECK(std::abs(pat::modLevelAt(lv, 4, k, 0.0, 0, S::Glide, tau) - startOf[k]) < 1e-9);
			const double mid = lv[k][0] + (startOf[k] - lv[k][0]) * std::exp(-0.5 / tau);
			CHECK(std::abs(pat::modLevelAt(lv, 4, k, 0.5, 0, S::Glide, tau) - mid) < 1e-9);
			// CONTINUOUS: a step's end is the next step's start.
			CHECK(std::abs(pat::modLevelAt(lv, 4, k, 1.0, 0, S::Glide, tau) - pat::modLevelAt(lv, 4, (k + 1) % 4, 0.0, 0, S::Glide, tau)) < 1e-9);
		}
	}
	// TIME 0 IS HOLD, for both.
	CHECK(std::abs(pat::modLevelAt(lv, 4, 2, 0.9, 0, S::Glide, 0.0) - 0.6) < 1e-6);
	CHECK(std::abs(pat::modLevelAt(lv, 4, 2, 0.9, 0, S::Ramp, 0.0) - 0.6) < 1e-6);
}

TEST_CASE("modulation output: Bipolar is 2v − 1; Identity rests every lane at 0; a lane that is not live rests at 0 in either polarity") {
	namespace pn = feathers::mod::arp::pattern;
	const pn::PatternCycle c = handCycle();
	pat::ModLevels lv {};
	uint8_t live = 0;
	pat::modLevelsOf(c, 0xFF, lv, &live);
	pat::ModLanes m;
	CHECK(pat::modOutput(lv, live, 4, 1, 0.5, 0, m) == doctest::Approx(0.6));
	m.bipolar = true;
	CHECK(pat::modOutput(lv, live, 4, 1, 0.5, 0, m) == doctest::Approx(0.2));
	CHECK(pat::modOutput(lv, live, 4, 0, 0.5, 0, m) == doctest::Approx(-0.6));
	CHECK(pat::modOutput(lv, live, 4, 1, 0.5, 1, m) == 0.0f); // lane 1 never hits
	m.neutral = true;
	for (size_t l = 0; l < 8; ++l) {
		CHECK(pat::modOutput(lv, live, 4, 1, 0.5, l, m) == 0.0f);
	}
}

TEST_CASE("modulation device: a Modulation tab runs off the transport with no key held, plays NO notes, and its eight lanes read the restated levels at every step") {
	Drive d;
	setUpModulation(d, 16);
	const auto& q = d.s.patterns[0];
	const int64_t slots = 40;
	callAt(d, 0);
	const auto& cycle = d.d.cycleForTest(0);
	REQUIRE(cycle.length == 16);
	const Levels r = restate(cycle, q.drum.on);
	REQUIRE(r.live != 0);
	REQUIRE(d.d.modLiveForTest(0) == r.live);
	int varying = 0;
	for (int64_t s = 0; s < slots; ++s) {
		CAPTURE(s);
		callAt(d, s * kStep + kStep / 2);
		CHECK(d.d.tabActive(0) == 1.0f);
		for (size_t t = 1; t < pat::kPatterns; ++t) {
			CHECK(d.d.tabActive(t) == 0.0f);
		}
		for (size_t l = 0; l < 8; ++l) {
			CAPTURE(l);
			const bool liveLane = ((r.live >> l) & 1u) != 0u;
			const double want = liveLane ? r.at[static_cast<size_t>(s % 16)][l] : 0.0;
			CHECK(std::abs(d.d.modLane(0, l) - want) < 1e-6);
			CHECK(d.d.modLane(1, l) == 0.0f);
			if (liveLane && s > 0 && r.at[static_cast<size_t>(s % 16)][l] != r.at[static_cast<size_t>((s - 1) % 16)][l]) {
				++varying;
			}
		}
	}
	CHECK(varying > 0); // the lanes move: the check above is not of a constant
	for (const Logged& l : d.log) {
		CHECK(l.kind != Kind::On); // no note struck
	}
	// A KEY PASSES STRAIGHT THROUGH (Thru, every key: nothing is played instead).
	d.note(Kind::On, 64);
	REQUIRE(!d.log.empty());
	CHECK(d.log.back().kind == Kind::On);
	CHECK(d.log.back().key == 64);
	d.note(Kind::Off, 64);
	// THE TRANSPORT STOPS: after two steps' time the gate and the lanes are back at identity.
	d.playing = false;
	callAt(d, d.now + 3 * kStep);
	CHECK(d.d.tabActive(0) == 0.0f);
	for (size_t l = 0; l < 8; ++l) {
		CHECK(d.d.modLane(0, l) == 0.0f);
	}
}

TEST_CASE("modulation device: Identity forces every lane to 0 at once; a lane switched off rests at 0 while the others move on; Bipolar and Ramp reach the device") {
	Drive d;
	setUpModulation(d, 16);
	callAt(d, 0);
	const Levels r = restate(d.d.cycleForTest(0), 0xFF);
	REQUIRE(r.live != 0);
	const size_t lane = static_cast<size_t>(std::countr_zero(static_cast<unsigned>(r.live)));
	// RAMP 1 AND BIPOLAR, at a quarter of each step.
	d.s.modLanes[0].smooth = pat::ModSmooth::Ramp;
	d.s.modLanes[0].time = 1.0f;
	d.s.modLanes[0].bipolar = true;
	d.apply();
	for (int64_t s = 16; s < 32; ++s) {
		CAPTURE(s);
		callAt(d, s * kStep + kStep / 4);
		const double a = r.at[static_cast<size_t>(s % 16)][lane];
		const double b = r.at[static_cast<size_t>((s + 1) % 16)][lane];
		CHECK(std::abs(d.d.modLane(0, lane) - (2.0 * (a + (b - a) * 0.25) - 1.0)) < 1e-4);
	}
	// IDENTITY.
	d.s.modLanes[0].neutral = true;
	d.apply();
	callAt(d, 33 * kStep + kStep / 2);
	for (size_t l = 0; l < 8; ++l) {
		CHECK(d.d.modLane(0, l) == 0.0f);
	}
	CHECK(d.d.tabActive(0) == 1.0f); // the tab still sounds: only its lanes are neutral
	// ONE LANE OFF: it rests at 0; another live lane still reads its level.
	d.s.modLanes[0].neutral = false;
	d.s.modLanes[0].smooth = pat::ModSmooth::Hold;
	d.s.modLanes[0].bipolar = false;
	d.s.patterns[0].drum.on = static_cast<uint8_t>(0xFF & ~(1u << lane));
	d.apply();
	callAt(d, 34 * kStep + kStep / 2);
	CHECK(d.d.modLane(0, lane) == 0.0f);
	const uint8_t others = static_cast<uint8_t>(d.d.modLiveForTest(0));
	REQUIRE(others != 0);
	REQUIRE(((others >> lane) & 1u) == 0u);
	const size_t other = static_cast<size_t>(std::countr_zero(static_cast<unsigned>(others)));
	const Levels r2 = restate(d.d.cycleForTest(0), d.s.patterns[0].drum.on);
	CHECK(std::abs(d.d.modLane(0, other) - r2.at[34 % 16][other]) < 1e-6);
}

TEST_CASE("modulation device: Glide's line is a function of the song position — a locate lands on the value the run from the start had there") {
	const auto setUpGlide = [](Drive& d) {
		setUpModulation(d, 16);
		d.s.modLanes[0].smooth = pat::ModSmooth::Glide;
		d.s.modLanes[0].time = 2.0f;
		d.apply();
	};
	Drive a;
	setUpGlide(a);
	const int64_t t = 37 * kStep + 1234;
	callAt(a, t);
	Drive b;
	setUpGlide(b);
	b.locate(static_cast<double>(30 * kStep) / kRate * 2.0); // beats at 120 bpm
	callAt(b, t - 30 * kStep);
	REQUIRE(a.d.modLiveForTest(0) != 0);
	for (size_t l = 0; l < 8; ++l) {
		CAPTURE(l);
		CHECK(std::abs(a.d.modLane(0, l) - b.d.modLane(0, l)) < 1e-5);
	}
}

TEST_CASE("identity gates in Cycle with a Modulation tab: each slot's gate is its tab's alone, and the Modulation lanes read 0 while the other tab sounds") {
	using A = pat::NextAction;
	const std::array<Tab, pat::kPatterns> tabs { { { true, 4, 1, A::Next, 7 }, { true, 4, 1, A::Next, 7 },
		                                           { false, 4, 1, A::Next, 7 }, { false, 4, 1, A::Next, 7 } } };
	Drive d;
	setUp(d, tabs);
	d.s.patterns[1].mode = genes::Mode::Modulation;
	d.apply();
	d.note(Kind::On, 60);
	int modulated = 0;
	for (int64_t s = 0; s < 24; ++s) {
		CAPTURE(s);
		callAt(d, s * kStep + kStep / 2);
		const size_t tab = static_cast<size_t>((s / 4) % 2);
		for (size_t t = 0; t < pat::kPatterns; ++t) {
			CHECK(d.d.tabActive(t) == (t == tab ? 1.0f : 0.0f));
		}
		for (size_t l = 0; l < 8; ++l) {
			if (tab == 0) {
				CHECK(d.d.modLane(1, l) == 0.0f);
			}
			else if (d.d.modLane(1, l) != 0.0f) {
				++modulated;
			}
		}
	}
	CHECK(modulated > 0);
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("modulation honesty: what a Modulation tab does not read is not drawn, and what it reads is") {
	using M = genes::Mode;
	using F = pat::Field;
	const auto hides = [](F f, M m) { return pat::drumHides(pat::at(0, f), m); };
	for (const F f : { F::Root, F::Scale, F::Degree, F::Size, F::Inversion, F::Spread, F::PitchOnNotes, F::GlideTime,
	                   F::TransposeSteps, F::TransposeOctaves, F::Thru, F::Trig }) {
		CAPTURE(static_cast<int>(f));
		CHECK(hides(f, M::Modulation));
	}
	CHECK_FALSE(hides(F::Thru, M::Drum));
	CHECK_FALSE(hides(F::Accent, M::Modulation)); // Accent shapes the hits' velocities, so the levels
	CHECK_FALSE(hides(F::ModTime, M::Modulation));
	CHECK(hides(F::ModTime, M::Melody));
	const auto gated = [](F f) { return pat::kParamDescs[pat::at(0, f)].gateMask; };
	const uint32_t mod = pat::modeBit(M::Modulation);
	CHECK((gated(F::ModSmooth) & mod) != 0u);
	CHECK((gated(F::ModPolarity) & mod) != 0u);
	CHECK((gated(F::ModNeutral) & mod) != 0u);
	CHECK(gated(F::ModSmooth) == mod);
	CHECK((gated(F::Gate) & mod) == 0u);
	CHECK((gated(F::Length) & mod) != 0u);
	CHECK((pat::kParamDescs[pat::drumOnAt(0, 0)].gateMask & mod) != 0u);
	CHECK((pat::kParamDescs[pat::drumNoteAt(0, 0)].gateMask & mod) == 0u);
	CHECK(pat::kParamDescs[pat::at(0, F::ModTime)].gateControl == static_cast<int16_t>(pat::at(0, F::ModSmooth)));
	CHECK_FALSE(genes::pinRead(M::Modulation, genes::Pin::Octave));
	CHECK_FALSE(genes::pinRead(M::Modulation, genes::Pin::Ratchet));
	CHECK(genes::pinRead(M::Modulation, genes::Pin::Detail));
	CHECK(genes::pinRead(M::Modulation, genes::Pin::Density));
	pat::PatternSettings q;
	q.mode = M::Modulation;
	CHECK(pat::thruOn(q));
	CHECK(pat::trigOf(q) == feathers::mod::arp::TrigMode::Transport);
}

TEST_CASE("modulation text and state: Mode prints and parses Modulation; Smooth Time reads in steps; the four fields survive the row") {
	const device::ParamDesc& mode = pat::kParamDescs[pat::at(0, pat::Field::Mode)];
	CHECK(device::formatValue(mode, 7.0) == "Modulation");
	const device::TextView view {};
	double v = -1.0;
	REQUIRE(pat::kTextOps.parse(pat::at(0, pat::Field::Mode), "Modulation", view, &v));
	CHECK(v == 7.0);
	char buf[64];
	REQUIRE(pat::kTextOps.format(pat::at(2, pat::Field::ModTime), 1.5, view, buf, sizeof buf));
	CHECK(std::string(buf) == "1.50 steps");
	pat::Settings s;
	s.modLanes[2] = { pat::ModSmooth::Ramp, 0.75f, true, true };
	s.modLanes[3].smooth = pat::ModSmooth::Glide;
	const pat::Row row = pat::rowOf(s);
	const pat::Settings back = pat::settingsOf(row.data(), row.size());
	CHECK(back.modLanes == s.modLanes);
	CHECK(std::string(pat::kParamDescs[pat::at(0, pat::Field::ModSmooth)].key) == "p1.mod_smooth");
	CHECK(std::string(pat::kParamDescs[pat::at(3, pat::Field::ModNeutral)].key) == "p4.mod_neutral");
}

TEST_CASE("fill on the walk: four tabs on Random with one a fill — the fill fires on a quarter of the picks, merges over the tail each time, never twice running, and the same seed fires it on the same boundaries") {
	using A = pat::NextAction;
	std::array<Tab, pat::kPatterns> tabs {};
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		tabs[p] = { true, 16, 1, A::Random, 7 };
	}
	const auto walkOf = [&](int seed) {
		Drive d;
		setUp(d, tabs);
		d.s.phase[3].fill = true;
		d.s.feel.seed = seed;
		d.apply();
		pat::CycleWalk w;
		w.configure(d.d.cycleConfigForTest(true, 0.5));
		return w;
	};
	pat::CycleWalk w = walkOf(7);
	pat::CycleSegment s = w.first();
	int64_t picks = 0;
	int64_t fillPicks = 0;
	std::array<int64_t, pat::kPatterns> played {};
	std::vector<int64_t> fired;
	for (int i = 0; i < 4000; ++i) {
		REQUIRE(s.pattern < 3); // a fill never takes a segment of its own
		++played[s.pattern];
		// EVERY WALK STEP IS A PICK: the segment's own, plus the fill's when one merges (it counts one step: `nextIndex`).
		const int64_t steps = s.nextIndex - s.index;
		REQUIRE((steps == 1 || steps == 2));
		CHECK((steps == 2) == (s.fill == 3)); // a merged fill is exactly the extra step
		picks += steps;
		if (s.fill == 3) {
			++fillPicks;
			fired.push_back(s.index);
			CHECK(s.nextPattern != 3);
		}
		s = w.after(s);
	}
	const double share = static_cast<double>(fillPicks) / static_cast<double>(picks);
	MESSAGE("seed 7: the fill on " << fillPicks << " of " << picks << " picks (" << share << ")");
	CHECK(share == doctest::Approx(0.25).epsilon(0.08));
	// THE OTHERS SHARE THE SEGMENTS EVENLY.
	for (size_t p = 0; p < 3; ++p) {
		CHECK(static_cast<double>(played[p]) / 4000.0 == doctest::Approx(1.0 / 3.0).epsilon(0.1));
	}
	// DETERMINISTIC: the same seed fires on the same boundaries; another seed elsewhere.
	pat::CycleWalk again = walkOf(7);
	pat::CycleWalk other = walkOf(8);
	pat::CycleSegment a = again.first();
	pat::CycleSegment o = other.first();
	std::vector<int64_t> firedAgain;
	std::vector<int64_t> firedOther;
	for (int i = 0; i < 4000; ++i) {
		if (a.fill == 3) {
			firedAgain.push_back(a.index);
		}
		if (o.fill == 3) {
			firedOther.push_back(o.index);
		}
		a = again.after(a);
		o = other.after(o);
	}
	CHECK(firedAgain == fired);
	CHECK(firedOther != fired);
	// A LOCATE lands on the walk: the segment at a time far in, asked cold, is the one the run from the start reached.
	pat::CycleWalk run = walkOf(7);
	pat::CycleSegment r = run.first();
	for (int i = 0; i < 1500; ++i) {
		r = run.after(r);
	}
	pat::CycleWalk cold = walkOf(7);
	const pat::CycleSegment c = cold.at(0.5 * (r.start + r.end));
	CHECK(c.index == r.index);
	CHECK(c.pattern == r.pattern);
	CHECK(c.fill == r.fill);
}

TEST_CASE("fill on the walk, heard: on Random every boundary the walk merges the fill over, the fill's own steps play the tail of that segment, and nowhere else") {
	using A = pat::NextAction;
	std::array<Tab, pat::kPatterns> tabs {};
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		tabs[p] = { true, p == 3 ? size_t { 4 } : size_t { 8 }, 1, A::Random, 7 };
	}
	Drive d;
	setUp(d, tabs);
	d.s.phase[3].fill = true;
	d.apply();
	d.note(Kind::On, 60);
	const int64_t count = 8 * 40;
	d.runTo(count * kStep);
	const auto got = slots(d.played, count);
	pat::CycleWalk w;
	w.configure(d.d.cycleConfigForTest(true, 0.5));
	int fills = 0;
	for (pat::CycleSegment s = w.first(); s.start < static_cast<double>(count) / 4.0; s = w.after(s)) {
		const auto first = static_cast<int64_t>(std::llround(std::max(0.0, s.start) * 4.0)); // sixteenths
		for (int64_t k = 0; k < 8 && first + k < count; ++k) {
			CAPTURE(first + k);
			const bool tail = s.fill == 3 && k >= 4;
			const std::pair<size_t, size_t> want = tail ? std::pair<size_t, size_t> { 3, static_cast<size_t>(k - 4) }
			                                            : std::pair<size_t, size_t> { s.pattern, static_cast<size_t>(k) };
			CHECK(got[static_cast<size_t>(first + k)] == want);
		}
		fills += s.fill == 3 ? 1 : 0;
	}
	CHECK(fills > 3);
	d.note(Kind::Off, 60);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
}

TEST_CASE("two fills on Random: a fill the walk picks straight after a fill is passed through (only the first merges), and the walk carries on from its Next") {
	using A = pat::NextAction;
	std::array<Tab, pat::kPatterns> tabs {};
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		tabs[p] = { true, 8, 1, A::Random, 7 };
	}
	Drive d;
	setUp(d, tabs);
	d.s.phase[2].fill = true;
	d.s.phase[3].fill = true;
	d.apply();
	pat::CycleWalk w;
	w.configure(d.d.cycleConfigForTest(true, 0.5));
	pat::CycleSegment s = w.first();
	int passed = 0;
	for (int i = 0; i < 2000; ++i) {
		REQUIRE(s.pattern < 2);
		REQUIRE(s.nextPattern < 2); // the walk never lands a segment on a fill
		const int64_t steps = s.nextIndex - s.index;
		// ONE FILL MERGES (`s.fill`); EVERY FURTHER STEP WAS A FILL PASSED THROUGH.
		CHECK((steps > 1) == (s.fill < pat::kPatterns));
		passed += steps > 2 ? 1 : 0;
		s = w.after(s);
	}
	CHECK(passed > 0); // the second-fill-in-a-row case did happen
}

TEST_CASE("Go's stuck-on candidates, by value (Go once stuck on, a rare bug): a stopped transport, Fill switched off, Play Mode switched, a second Go while one is armed, a press and release inside one block") {
	using A = pat::NextAction;
	const auto setUpFill = [](Drive& d) {
		std::array<Tab, pat::kPatterns> tabs {};
		for (size_t p = 0; p < pat::kPatterns; ++p) {
			tabs[p] = { p < 2, 16, 1, A::Loop, 7 };
		}
		setUp(d, tabs);
		d.s.playMode = pat::PlayMode::Selector;
		d.s.phase[2].fill = true; // tab 3 a fill (out of range: Go alone reaches it)
		d.s.phase[3].fill = true;
		d.apply();
		d.note(Kind::On, 60);
		d.runTo(4 * kStep);
	};
	const auto go = [](Drive& d, size_t tab) {
		d.s.fillTrigger[tab] = 1.0f;
		d.apply();
		d.s.fillTrigger[tab] = 0.0f;
		d.apply();
	};
	SUBCASE("a stopped transport: the armed fill waits (pending) and fires on the first boundary after play resumes") {
		Drive d;
		setUpFill(d);
		d.playing = false;
		d.runTo(d.now + 64);
		go(d, 2);
		CHECK(d.d.armedFillForTest() == 2u);
		d.runTo(d.now + 64 * kStep);
		CHECK(d.d.armedFillForTest() == 2u); // NOTHING PLAYS, SO NOTHING FIRES: pending, by design (the Go shows it now)
		d.playing = true;
		d.locate(static_cast<double>(d.now) / 24000.0);
		d.runTo(d.now + 20 * kStep);
		CHECK(d.d.armedFillForTest() == pat::kPatterns);
		d.note(Kind::Off, 60);
	}
	SUBCASE("Fill switched off while armed: the arm is dropped — a tab that is no longer a fill never fires as one") {
		Drive d;
		setUpFill(d);
		go(d, 2);
		REQUIRE(d.d.armedFillForTest() == 2u);
		d.s.phase[2].fill = false;
		d.apply();
		CHECK(d.d.armedFillForTest() == pat::kPatterns);
		d.runTo(d.now + 32 * kStep);
		CHECK(d.d.activeFillForTest() == pat::kPatterns);
		d.note(Kind::Off, 60);
	}
	SUBCASE("Play Mode switched while armed: it still fires (Cycle's road takes the arm too)") {
		Drive d;
		setUpFill(d);
		go(d, 3);
		REQUIRE(d.d.armedFillForTest() == 3u);
		d.s.playMode = pat::PlayMode::Cycle;
		d.apply();
		d.runTo(d.now + 40 * kStep);
		CHECK(d.d.armedFillForTest() == pat::kPatterns);
		d.note(Kind::Off, 60);
	}
	SUBCASE("a second Go while one is armed is ignored, and its own edge re-arms: a later press works") {
		Drive d;
		setUpFill(d);
		go(d, 2);
		go(d, 3);
		CHECK(d.d.armedFillForTest() == 2u);
		d.runTo(d.now + 40 * kStep); // tab 3's fill fires and ends
		CHECK(d.d.armedFillForTest() == pat::kPatterns);
		REQUIRE(d.d.activeFillForTest() == pat::kPatterns);
		go(d, 3);
		CHECK(d.d.armedFillForTest() == 3u); // nothing left high: the press counts
		d.note(Kind::Off, 60);
	}
	SUBCASE("a press and release inside one block: the device never sees the rise (the row it reads is the block's)") {
		Drive d;
		setUpFill(d);
		d.s.fillTrigger[2] = 0.0f; // up and down between two rows: the row the device reads says 0 both times
		d.apply();
		CHECK(d.d.armedFillForTest() == pat::kPatterns); // the DEVICE reads one row a block; the hosts keep a quick press's rise up for a block (`rack::HeldFalls`; test_rack_resolve / test_clap_render "… pressed and released between two blocks …")
		d.note(Kind::Off, 60);
	}
}

TEST_CASE("a fill tab with E off never joins the walk; Go still fires it (by design)") {
	using A = pat::NextAction;
	std::array<Tab, pat::kPatterns> tabs {};
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		tabs[p] = { p != 3, 16, 1, A::Random, 7 }; // tab 4: E off
	}
	Drive d;
	setUp(d, tabs);
	d.s.phase[3].fill = true;
	d.apply();
	// THE WALK, BY VALUE: four thousand segments, the fill merged over none of them.
	pat::CycleWalk w;
	w.configure(d.d.cycleConfigForTest(true, 0.5));
	pat::CycleSegment s = w.first();
	int merged = 0;
	for (int i = 0; i < 4000; ++i) {
		REQUIRE(s.pattern < 3);
		merged += s.fill == 3 ? 1 : 0;
		s = w.after(s);
	}
	CHECK(merged == 0);
	// AND THE CONTROL: E on, the same walk merges it (the case is not vacuous).
	d.s.patterns[3].enable = true;
	d.apply();
	w.configure(d.d.cycleConfigForTest(true, 0.5));
	s = w.first();
	for (int i = 0; i < 4000; ++i) {
		merged += s.fill == 3 ? 1 : 0;
		s = w.after(s);
	}
	CHECK(merged > 500);
	// GO STILL FIRES IT WITH E OFF: armed, then fired at the next boundary.
	d.s.patterns[3].enable = false;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(2 * kStep + 10);
	d.s.fillTrigger[3] = 1.0f;
	d.apply();
	d.s.fillTrigger[3] = 0.0f;
	d.apply();
	CHECK(d.d.armedFillForTest() == 3u);
	d.runTo(40 * kStep);
	CHECK(d.d.armedFillForTest() == pat::kPatterns); // fired
	const auto got = slots(d.played, 40);
	bool heard = false;
	for (const auto& [pattern, step] : got) {
		heard = heard || pattern == 3;
	}
	CHECK(heard);
}

TEST_CASE("Run Note: the cycle walk, and the fill merged over its tail, count from the first key — on that key's own sample, transport playing or not — and the next key after a full release starts the walk over") {
	using A = pat::NextAction;
	// THE SPEC'S EXAMPLE (1 -> 2 -> 3 (first half) -> 4 (fill) -> 1): three 16-step tabs and an 8-step fill, every Next.
	const std::array<Tab, pat::kPatterns> tabs { { { true, 16, 1, A::Next, 7 }, { true, 16, 1, A::Next, 7 },
		                                           { true, 16, 1, A::Next, 7 }, { true, 8, 1, A::Next, 7 } } };
	const auto want = [](int64_t k) -> std::pair<size_t, size_t> {
		const int64_t c = k % 48;
		if (c < 16) {
			return { 0, static_cast<size_t>(c) };
		}
		if (c < 32) {
			return { 1, static_cast<size_t>(c - 16) };
		}
		if (c < 40) {
			return { 2, static_cast<size_t>(c - 32) };
		}
		return { 3, static_cast<size_t>(c - 40) }; // the fill over 3's second half
	};
	for (const bool playing : { true, false }) {
		CAPTURE(playing);
		Drive d;
		setUp(d, tabs);
		d.s.phase[3].fill = true;
		d.s.run = feathers::mod::arp::RunMode::Note;
		d.apply();
		d.playing = playing;
		d.locate(3.3); // off the song's grid: Note does not read it
		const int64_t first = 1234;
		d.runTo(first);
		d.note(Kind::On, 60);
		d.runTo(first + 60 * kStep + 1);
		REQUIRE(d.played.size() >= 60);
		for (int64_t k = 0; k < 60; ++k) {
			CAPTURE(k);
			CHECK(d.played[static_cast<size_t>(k)].sample == first + k * kStep);
			CHECK(std::pair<size_t, size_t> { d.played[static_cast<size_t>(k)].pattern, d.played[static_cast<size_t>(k)].step } == want(k));
		}
		// THE LAST RELEASE STOPS IT; THE NEXT KEY STARTS THE WALK OVER, from tab 1's first step, on its own sample.
		d.note(Kind::Off, 60);
		d.runTo(d.now + 40 * kStep);
		const size_t before = d.played.size();
		d.runTo(d.now + 40 * kStep);
		CHECK(d.played.size() == before); // nothing played while no key is held
		const int64_t again = d.now + 777;
		d.runTo(again);
		d.note(Kind::On, 62);
		d.runTo(again + 20 * kStep + 1);
		REQUIRE(d.played.size() >= before + 20);
		for (int64_t k = 0; k < 20; ++k) {
			CAPTURE(k);
			const Drive::Played& p = d.played[before + static_cast<size_t>(k)];
			CHECK(p.sample == again + k * kStep);
			CHECK(std::pair<size_t, size_t> { p.pattern, p.step } == want(k));
		}
		d.note(Kind::Off, 62);
	}
}

TEST_CASE("note range: the fold — below Lowest up by octaves, above Highest down, a range under an octave takes the nearer bound, and Highest below Lowest reads as Lowest (backlog QT)") {
	CHECK(pat::foldToRange(28, 23, 127) == 28);  // inside: untouched
	CHECK(pat::foldToRange(16, 23, 127) == 28);  // E-1 up an octave: E1 (in the plugin's names, 28 is E0)
	CHECK(pat::foldToRange(-1, 23, 127) == 23);  // far below: up as many octaves as it takes, pitch class kept
	CHECK(pat::foldToRange(11, 23, 127) == 23);
	CHECK(pat::foldToRange(130, 0, 127) == 118); // past MIDI: down, never clamped
	CHECK(pat::foldToRange(80, 23, 72) == 68);   // above Highest: down
	CHECK(pat::foldToRange(66, 60, 64) == 64);   // under an octave, no octave of it fits: the nearer bound
	CHECK(pat::foldToRange(58, 60, 64) == 60);
	CHECK(pat::foldToRange(55, 60, 50) == 60);   // Highest below Lowest: Lowest
}

TEST_CASE("note range: over a sweep of the walk in every pitched Mode at a low root, nothing plays below Lowest or above Highest; the defaults move only notes under the floor, and how many (backlog QT)") {
	using M = genes::Mode;
	const M modes[] = { M::Melody, M::MelodyMono, M::Chords, M::TbMono, M::TbGlide, M::Progression };
	const auto run = [](M mode, int zone, int root, int low, int high) {
		Drive d;
		d.s.patterns[0].mode = mode;
		d.s.patterns[0].zone = zone;
		d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Octave)] = 0.6f; // the octave gene at work
		d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Range)] = 0.8f;
		d.s.noteLow = low;
		d.s.noteHigh = high;
		d.apply();
		d.note(Kind::On, root);
		d.runTo(64 * kStep);
		std::vector<int> keys;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::On) {
				keys.push_back(l.key);
			}
		}
		return keys;
	};
	for (const int root : { 28, 60 }) { // E1 (a bass root) and C4
		CAPTURE(root);
		size_t notes = 0;
		size_t moved = 0;
		for (const M mode : modes) {
			CAPTURE(static_cast<int>(mode));
			for (int z = 0; z < 24; ++z) {
				const int zone = 37 * z + 5;
				const std::vector<int> folded = run(mode, zone, root, pat::kNoteLowDefault, pat::kNoteHighDefault);
				const std::vector<int> free = run(mode, zone, root, 0, 127);
				REQUIRE(folded.size() == free.size()); // the range moves notes, never adds or drops one
				for (size_t i = 0; i < folded.size(); ++i) {
					CHECK(folded[i] >= pat::kNoteLowDefault);
					// ONLY A NOTE UNDER THE FLOOR MOVES, and up by whole octaves.
					if (free[i] >= pat::kNoteLowDefault) {
						CHECK(folded[i] == free[i]);
					}
					else {
						CHECK((folded[i] - free[i]) % 12 == 0);
						++moved;
					}
				}
				notes += folded.size();
				// AND A CEILING: Lowest E1, Highest C4 — every note inside.
				for (const int k : run(mode, zone, root, 28, 60)) {
					CHECK(k >= 28);
					CHECK(k <= 60);
				}
			}
		}
		MESSAGE("root " << root << ": the default range moved " << moved << " of " << notes << " notes ("
		                << (notes > 0 ? 100.0 * static_cast<double>(moved) / static_cast<double>(notes) : 0.0) << "%)");
		CHECK(notes > 1000); // not vacuous
		if (root == 60) {
			CHECK(moved == 0); // the default floor changes nothing played around C4
		}
		else {
			CHECK(moved > 0); // at E1 the walk does reach under the floor, and the fold is what stops it
		}
	}
}

TEST_CASE("note range: Wobble's octave jump down from E1 lands on E1 under the default floor (backlog QT)") {
	// A DRONE ON THE ROOT (Motion off, no octave gene, no answer), so every unjumped note is E1 (28) and Wobble's extra is the
	// only octave: +12 to 40, -12 to 16, which the floor folds back to 28.
	const auto keysWith = [](int low) {
		Drive d;
		auto& q = d.s.patterns[0];
		q.mode = genes::Mode::Melody;
		for (const genes::Pin p : { genes::Pin::Motion, genes::Pin::Octave, genes::Pin::Answer, genes::Pin::Leap, genes::Pin::Hold,
		                            genes::Pin::Density }) { // (no ties, every step a note: setUp's pins)
			q.pins[static_cast<size_t>(p)] = 0.0f;
		}
		d.s.feel.wobble = 9.1; // a position whose octave field fires both ways over 400 steps
		d.s.noteLow = low;
		d.apply();
		d.note(Kind::On, 28);
		d.runTo(400 * kStep);
		std::set<int> keys;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::On) {
				keys.insert(l.key);
			}
		}
		return keys;
	};
	const std::set<int> free = keysWith(0);
	CHECK(free.count(16) == 1); // without a floor the jump down happens: not vacuous
	CHECK(free.count(40) == 1);
	const std::set<int> folded = keysWith(pat::kNoteLowDefault);
	CHECK(folded == std::set<int> { 28, 40 });
}
