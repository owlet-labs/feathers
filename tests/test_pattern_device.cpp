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

#include "device/state_blocks.h"
#include "mod/arp/pattern_device.h"
#include "mod/arp/pattern_types.h"
#include "plugin/params.h"
#include "plugin/synth.h"

#include <chrono>
#include <functional>
#include <tuple>
#include <ctime>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

// THE PATTERN DEVICE, BY VALUE. Every case reads note EVENTS and
// the SAMPLE each landed on. The expected keys come from the built cycle (`cycleForTest`, held by test_arp_pattern's
// own cases); what is checked here is the road from that cycle to events: which pattern, which sample, which step.

namespace pat = feathers::mod::arp::pat;
namespace genes = feathers::mod::arp::genes;
namespace device = feathers::device;
using Kind = device::NoteEvent::Kind;
using StepKind = feathers::mod::arp::pattern::PatternStep::Kind;

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

/// THE SYNTH'S PROTOCOL AROUND ONE DEVICE, as test_arp's `Drive`: host notes through `processNotes`, then the clock call
/// at every chunk (≤ 32 samples) and at every sample it asks for.
struct Drive {
	pat::PatternDevice d;
	pat::Settings s;
	double bpm = 120.0;
	bool playing = true;
	double startBeats = 0.0;
	int64_t songOrigin = 0;
	int64_t now = 0;
	/// The sample of the newest clock call — where the sources were last computed.
	int64_t lastCall = 0;
	std::vector<Logged> log;

	Drive() {
		d.prepare(kRate);
		// DETAIL OFF BY DEFAULT HERE: a ratchet or a grace note adds hits the plain cases do not restate.
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
			lastCall = now;
			for (size_t i = 0; i < n; ++i) {
				log.push_back({ out[i].kind, out[i].key, out[i].velocity, now, out[i].expressionId, out[i].expressionValue });
			}
			REQUIRE(next >= 1);
			now += std::min<int64_t>({ static_cast<int64_t>(next), 32, until - now });
		}
	}
	std::vector<Logged> onsAt(int64_t sample) const {
		std::vector<Logged> r;
		for (const Logged& l : log) {
			if (l.kind == Kind::On && l.sample == sample) {
				r.push_back(l);
			}
		}
		return r;
	}
};

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

void releaseAndDrain(Drive& d, std::initializer_list<int> keys) {
	for (const int k : keys) {
		d.note(Kind::Off, k);
	}
	d.runTo(d.now + 4 * kStep);
}

} // namespace

TEST_CASE("pattern device: a Melody plays its built cycle — every note on its slot's sample, with the key the cycle names") {
	Drive d;
	d.s.patterns[0].zone = 300;
	d.s.patterns[0].gamma = 7.0;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(40 * kStep);
	const auto& cycle = d.d.cycleForTest(0);
	REQUIRE(cycle.playable);
	int notes = 0;
	for (int64_t g = 1; g < 40; ++g) {
		const size_t k = static_cast<size_t>(g % 16);
		const auto& st = cycle.steps[k];
		const std::vector<Logged> ons = d.onsAt(g * kStep);
		CAPTURE(g);
		if (st.kind == StepKind::Note) {
			REQUIRE(ons.size() == 1);
			CHECK(ons[0].key == cycle.keyAt(k, g / 16));
			CHECK(ons[0].velocity == doctest::Approx(st.velocity));
			++notes;
		}
		else {
			CHECK(ons.empty());
		}
	}
	CHECK(notes > 10);
	releaseAndDrain(d, { 60 });
	checkBalanced(d.log);
}

TEST_CASE("pattern device: a chord step strikes all its tones on ONE sample; a held common tone is not struck again") {
	Drive d;
	d.s.patterns[0].mode = genes::Mode::Chords;
	d.s.patterns[0].size = feathers::mod::arp::theory::ChordSize::Seventh;
	int checkedChords = 0;
	for (int place = 0; place < 6; ++place) {
		d.s.patterns[0].zone = 100 * place + 17;
		d.apply();
		d.log.clear();
		d.note(Kind::On, 57);
		const int64_t from = d.now;
		d.runTo(from + 33 * kStep);
		const auto& cycle = d.d.cycleForTest(0);
		const int64_t g0 = (from + kStep - 1) / kStep;
		for (int64_t g = g0 + 1; g < g0 + 30; ++g) {
			const size_t k = static_cast<size_t>(g % 16);
			const auto& st = cycle.steps[k];
			if (st.kind != StepKind::Note) {
				continue;
			}
			int struck = 0;
			for (int i = 0; i < st.count; ++i) {
				struck += ((st.holdMask >> i) & 1u) == 0u ? 1 : 0;
			}
			const std::vector<Logged> ons = d.onsAt(g * kStep);
			CAPTURE(g);
			// EVERY TONE NOT HELD, ON THIS SAMPLE (a held tone whose note had ended is struck again: at most count).
			CHECK(static_cast<int>(ons.size()) >= struck);
			CHECK(static_cast<int>(ons.size()) <= st.count);
			++checkedChords;
		}
		releaseAndDrain(d, { 57 });
		checkBalanced(d.log);
	}
	CHECK(checkedChords > 20);
}

TEST_CASE("pattern device: a selector change lands on the NEXT step, and the notes sounding then keep their offs") {
	Drive d;
	d.s.patterns[0].zone = 200;
	d.s.patterns[1].enable = true;
	d.s.patterns[1].zone = 700;
	d.s.patterns[1].degree = 4;
	for (auto& q : d.s.patterns) {
		q.pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f; // every step sounds, so the switch is visible
		q.pins[static_cast<size_t>(genes::Pin::Hold)] = 0.0f;
		q.gate = 0.9f;
	}
	d.s.selector = 0.1f;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(4 * kStep + kStep / 2);
	CHECK(d.d.playingForTest() == 0);
	d.s.selector = 0.9f; // pattern 2's band
	d.apply();
	const size_t before = d.log.size();
	d.runTo(5 * kStep - 1);
	// NOTHING BETWEEN THE CHANGE AND THE NEXT ONSET except step 4's own off, where its gate puts it.
	const double gate4 = static_cast<double>(d.d.cycleForTest(0).steps[4].gate);
	for (size_t i = before; i < d.log.size(); ++i) {
		CHECK(d.log[i].kind == Kind::Off);
		CHECK(d.log[i].sample == 4 * kStep + static_cast<int64_t>(std::ceil(gate4 * kStep - 1e-6)));
	}
	CHECK(d.d.playingForTest() == 0);
	d.runTo(9 * kStep);
	CHECK(d.d.playingForTest() == 1);
	for (int64_t g = 5; g < 9; ++g) {
		const std::vector<Logged> ons = d.onsAt(g * kStep);
		REQUIRE(ons.size() == 1);
		CHECK(ons[0].key == d.d.cycleForTest(1).keyAt(static_cast<size_t>(g % 16), 0));
	}
	releaseAndDrain(d, { 60 });
	checkBalanced(d.log);
}

TEST_CASE("pattern device: with no pattern in range it is silent and swallows the keys; a solo plays whatever the selector says") {
	Drive d;
	d.s.patterns[0].enable = false;
	d.apply();
	d.note(Kind::On, 60);
	d.runTo(8 * kStep);
	CHECK(d.log.empty());
	CHECK(d.d.playingForTest() == pat::kPatterns);
	d.s.patterns[2].solo = true; // solo on a pattern switched off: an explicit ask
	d.s.selector = 0.0f;
	d.apply();
	d.runTo(16 * kStep);
	CHECK(d.d.playingForTest() == 2);
	int heard = 0;
	for (const Logged& l : d.log) {
		heard += l.kind == Kind::On ? 1 : 0;
	}
	CHECK(heard > 0);
	releaseAndDrain(d, { 60 });
	checkBalanced(d.log);
}

TEST_CASE("pattern device: TB-Mono slides overlap — the next note's on lands before the sliding note's off") {
	Drive d;
	d.s.patterns[0].mode = genes::Mode::TbMono;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Glide)] = 1.0f; // every step that can, slides
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Hold)] = 0.0f;
	d.apply();
	d.note(Kind::On, 48);
	d.runTo(20 * kStep);
	int overlaps = 0;
	for (int64_t g = 1; g < 19; ++g) {
		const std::vector<Logged> ons = d.onsAt(g * kStep);
		const std::vector<Logged> next = d.onsAt((g + 1) * kStep);
		if (ons.size() != 1 || next.size() != 1 || ons[0].key == next[0].key) {
			continue;
		}
		// THE SLID NOTE'S OFF: after the next on, by the 2 ms overlap.
		int64_t offAt = -1;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::Off && l.key == ons[0].key && l.sample > ons[0].sample) {
				offAt = l.sample;
				break;
			}
		}
		CHECK(offAt > next[0].sample);
		CHECK(offAt - next[0].sample <= static_cast<int64_t>(0.002 * kRate) + 1);
		++overlaps;
	}
	CHECK(overlaps > 5);
	releaseAndDrain(d, { 48 });
	checkBalanced(d.log);
}

TEST_CASE("pattern device: the same song position plays the same notes — a loop back replays them exactly") {
	Drive d;
	d.s.patterns[0].zone = 512;
	d.s.patterns[0].mode = genes::Mode::Chords;
	d.apply();
	d.note(Kind::On, 62);
	d.runTo(32 * kStep);
	std::vector<Logged> first;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On && l.sample >= 8 * kStep && l.sample < 24 * kStep) {
			first.push_back(l);
		}
	}
	d.locate(2.0); // beat 2 = step 8
	const int64_t back = d.now;
	d.runTo(back + 16 * kStep);
	std::vector<Logged> again;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On && l.sample >= back && l.sample < back + 16 * kStep) {
			again.push_back(l);
		}
	}
	REQUIRE(first.size() == again.size());
	for (size_t i = 0; i < first.size(); ++i) {
		CHECK(first[i].key == again[i].key);
		CHECK(first[i].sample - 8 * kStep == again[i].sample - back);
	}
	releaseAndDrain(d, { 62 });
	checkBalanced(d.log);
}

TEST_CASE("pattern device: changing Mode, pattern and length mid-play leaves no note sounding") {
	Drive d;
	d.s.patterns[1].enable = true;
	d.s.patterns[1].mode = genes::Mode::Chords;
	d.s.patterns[1].timeBase = 10; // 1/8: a different step length
	d.apply();
	d.note(Kind::On, 60);
	d.note(Kind::On, 64);
	uint64_t h = 0x9E3779B97F4A7C15ull;
	for (int round = 0; round < 60; ++round) {
		h ^= h >> 12;
		h ^= h << 25;
		h ^= h >> 27;
		const uint64_t r = h * 0x2545F4914F6CDD1Dull;
		d.s.selector = static_cast<float>(r % 100) / 100.0f;
		d.s.patterns[r % 2].mode = static_cast<genes::Mode>((r >> 8) % pat::kModeChoices);
		d.s.patterns[0].length = 1 + (r >> 16) % 24;
		d.apply();
		d.runTo(d.now + static_cast<int64_t>((r >> 24) % (3 * kStep)) + 1);
	}
	releaseAndDrain(d, { 60, 64 });
	checkBalanced(d.log);
	CHECK(d.d.soundingForTest() == 0);
}

TEST_CASE("pattern device: the hand's block — a residue moves the pattern, and it and Latch's chord survive a save into a FRESH instance") {
	// THE RESIDUE (a past rotate's bank) IS SOUND: a pattern with one decodes to a different place.
	std::array<float, pat::kHandWords> words {};
	{
		device::BlockWriter w(words.data(), words.size());
		for (size_t p = 0; p < pat::kPatterns; ++p) {
			for (size_t i = 0; i < 2 * genes::kRowCount; ++i) {
				w.f64(p == 0 ? 0.37 + 0.01 * static_cast<double>(i) : 0.0);
			}
			w.u16(0x0FFFu & ~(1u << static_cast<unsigned>(genes::Pin::Density))); // Density off on every pattern
			w.f32(1.0f);
		}
		REQUIRE(w.ok());
	}
	Drive a;
	a.s.latch = true;
	a.apply();
	REQUIRE(a.d.writeHand(words.data(), words.size()));
	a.note(Kind::On, 60);
	a.note(Kind::Off, 60); // latched
	a.runTo(16 * kStep);
	Drive plain;
	plain.s.latch = true;
	plain.apply();
	plain.note(Kind::On, 60);
	plain.note(Kind::Off, 60);
	plain.runTo(16 * kStep);
	bool differs = false;
	for (size_t k = 0; k < 16; ++k) {
		differs = differs || a.d.cycleForTest(0).keyAt(k, 0) != plain.d.cycleForTest(0).keyAt(k, 0);
		CHECK(a.d.cycleForTest(0).steps[k].kind != StepKind::Rest); // Density's switch is off: no rests
	}
	CHECK(differs);
	// SAVE (both blocks), LOAD INTO A FRESH INSTANCE, PLAY ON: the same notes from step 16.
	std::array<float, feathers::mod::arp::kPlayWords> play {};
	a.d.tick(); // the host's modulator tick publishes what a save harvests
	REQUIRE(a.d.readPlay(play.data(), play.size()) > 0);
	std::array<float, pat::kHandWords> hand {};
	REQUIRE(a.d.readHand(hand.data(), hand.size()) > 0);
	Drive b;
	REQUIRE(pat::restoreBlock(&b.d, 0, play.data(), play.size()));
	REQUIRE(pat::restoreBlock(&b.d, 1, hand.data(), hand.size()));
	b.s.latch = true;
	b.apply();
	b.now = 16 * kStep;
	b.songOrigin = 0;
	a.runTo(32 * kStep);
	b.runTo(32 * kStep);
	for (int64_t g = 17; g < 32; ++g) {
		const auto x = a.onsAt(g * kStep);
		const auto y = b.onsAt(g * kStep);
		REQUIRE(x.size() == y.size());
		for (size_t i = 0; i < x.size(); ++i) {
			CHECK(x[i].key == y[i].key);
		}
	}
}

TEST_CASE("pattern device: TB-Glide holds ONE note across a slide run and bends it by TUNING expressions on the 303 lag") {
	// DESIGN §11.3, RESTATED: inside a run the next steps strike nothing; the held note's tuning moves from where it stands
	// toward (step key − held key) as target + (from − target)·e^(−t/τ), τ the pattern's Glide Time (22 ms by default),
	// t from the step's onset, written every 64 samples; it ends with ONE off and no reset.
	// TWICE: at the default and at a τ the knob sets, so the bend is shown to follow the KNOB and not a constant.
	for (const double tauAsked : { 0.0, 0.1 }) {
	CAPTURE(tauAsked);
	Drive d;
	if (tauAsked > 0.0) {
		d.s.patterns[0].glideTime = static_cast<float>(std::log2(tauAsked));
	}
	// THE τ THE DEVICE IS GIVEN: the field as stored (a float), exponentiated.
	const double tau = std::exp2(static_cast<double>(d.s.patterns[0].glideTime));
	CHECK(tau == doctest::Approx(tauAsked > 0.0 ? tauAsked : pat::kTb303SlideSeconds).epsilon(1e-6));
	d.s.patterns[0].mode = genes::Mode::TbGlide;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Glide)] = 1.0f;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Hold)] = 0.0f;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Range)] = 1.0f;
	d.apply();
	d.note(Kind::On, 48);
	d.runTo(17 * kStep);
	const auto& cycle = d.d.cycleForTest(0);
	// THE RUN STARTING AT SLOT 1 (slot 0 may begin mid-run of nothing): walk the cycle, restating what each step means.
	int bends = 0;
	int expressionsChecked = 0;
	size_t i = 0;
	for (int64_t g = 1; g < 15; ++g) {
		const auto& st = cycle.steps[static_cast<size_t>(g % 16)];
		const auto& prev = cycle.steps[static_cast<size_t>((g - 1) % 16)];
		const std::vector<Logged> ons = d.onsAt(g * kStep);
		if (prev.slide && prev.kind == StepKind::Note && g > 1) {
			CHECK(ons.empty()); // A CONTINUED RUN STRIKES NOTHING
			++bends;
		}
		(void)st;
		(void)i;
	}
	// EVERY TUNING EVENT IS THE LAG'S VALUE AT ITS SAMPLE: reconstruct each segment from the events themselves (its
	// start sample is the step onset before it; its target, the step's key minus the held key).
	int16_t held = -1;
	double from = 0.0;
	double target = 0.0;
	int64_t segment = -1;
	double lastValue = 0.0;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On) {
			held = l.key;
			from = 0.0;
			target = 0.0;
			segment = -1;
			lastValue = 0.0;
			continue;
		}
		if (l.kind != Kind::Expression) {
			continue;
		}
		CHECK(l.key == held);
		CHECK(l.expressionId == pat::kTuningExpression);
		const int64_t onset = (l.sample / kStep) * kStep;
		if (onset != segment) {
			// A NEW SEGMENT: it starts from where the last one had got to at this onset.
			const double t = static_cast<double>(onset - (segment < 0 ? onset : segment)) / kRate;
			from = segment < 0 ? 0.0 : target + (from - target) * std::exp(-t / tau);
			segment = onset;
			const auto& st = cycle.steps[static_cast<size_t>((onset / kStep) % 16)];
			target = static_cast<double>(cycle.keyAt(static_cast<size_t>((onset / kStep) % 16), 0) - held);
			(void)st;
		}
		const double t = static_cast<double>(l.sample - segment) / kRate;
		const double want = target + (from - target) * std::exp(-t / tau);
		const bool settled = std::abs(want - target) <= 0.005 || std::abs(l.expressionValue - target) < 1e-12;
		if (!settled) {
			CHECK(l.expressionValue == doctest::Approx(want).epsilon(1e-9));
		}
		lastValue = l.expressionValue;
		++expressionsChecked;
	}
	(void)lastValue;
	MESSAGE("continued steps " << bends << ", tuning events checked " << expressionsChecked);
	CHECK(bends > 3);
	CHECK(expressionsChecked > 20);
	releaseAndDrain(d, { 48 });
	checkBalanced(d.log);
	}
}


TEST_CASE("synth (T10): a TUNING expression that lands mid-render reaches the oscillator as a line over the control tick, not a jump") {
	// TB-GLIDE'S QUESTION OF THE SYNTH (design question 7, decided). MEASURED BEFORE THE FIX: a +2 st TUNING landed
	// whole within 8 samples — a jump, so a bend written every 64 samples was a staircase. Now it is a straight line over
	// the 64-sample tick (`rampToPitch`, backlog FK's road): restated here as 2·min(1, n/64) after n samples.
	feathers::ParamSet params;
	feathers::Synth synth { params };
	synth.activate(kRate, 4096);
	std::vector<float> l(4096), r(4096);
	synth.noteOn(0, 60, -1, 1.0);
	synth.render(l.data(), r.data(), 256);
	const double base = synth.voicePitchForTest(0);
	synth.noteExpression(0, 60, -1, pat::kTuningExpression, 2.0);
	std::string trace;
	double previous = 0.0;
	for (int i = 1; i <= 12; ++i) {
		synth.render(l.data(), r.data(), 8);
		const double moved = synth.voicePitchForTest(0) - base;
		trace += std::to_string(moved).substr(0, 6) + " ";
		CAPTURE(i);
		CHECK(moved >= previous - 1e-12);
		if (i * 8 < 64) {
			CHECK(moved < 2.0 - 1e-6); // NOT YET THERE: it ramps
		}
		previous = moved;
	}
	MESSAGE("pitch above the note every 8 samples after a +2 st TUNING: " << trace);
	CHECK(previous == doctest::Approx(2.0).epsilon(1e-9));
}

namespace {

/// A DRIVE WHERE EVERY STEP STRIKES ONE NOTE (no rests, ties, slides, ratchets, octaves): its ons are the grid itself.
void plainLine(Drive& d) {
	for (auto& q : d.s.patterns) {
		for (const genes::Pin p : { genes::Pin::Density, genes::Pin::Hold, genes::Pin::Glide, genes::Pin::Detail }) {
			q.pins[static_cast<size_t>(p)] = 0.0f;
		}
	}
	d.apply();
}

} // namespace

TEST_CASE("humanize: every note lands on its straight slot plus the feel's shift — Swing 16 restated as the MPC's odd-step lateness") {
	namespace hz = feathers::mod::arp::humanize;
	for (int m = 1; m < static_cast<int>(hz::Mode::kCount); ++m) {
		const auto mode = static_cast<hz::Mode>(m);
		if (mode == hz::Mode::StrumUp || mode == hz::Mode::StrumDown || mode == hz::Mode::Flam) {
			continue; // they move tones and graces, not the slot (below)
		}
		Drive d;
		d.s.feel = { mode, 0.8, 11 };
		plainLine(d);
		d.note(Kind::On, 60);
		d.runTo(40 * kStep);
		CAPTURE(hz::kModeNames[m]);
		for (int64_t g = 2; g < 38; ++g) {
			// THE FEEL AS THE DEVICE HOLDS IT — its amount went through a float field (0.8 is 0.800000012 there).
			const double shift = hz::shiftSteps(d.d.settingsForTest().feel, g, static_cast<double>(kStep) / kRate, 0.25);
			const auto want = static_cast<int64_t>(std::ceil((static_cast<double>(g) + shift) * kStep - 1e-6));
			CAPTURE(g);
			CAPTURE(want);
			CHECK(d.onsAt(want).size() == 1);
			if (mode == hz::Mode::Swing16) {
				// THE MPC'S SWING, independently: the odd sixteenths late by 2·(0.5 + 0.25·amount) − 1 of a step.
				const double a = d.d.settingsForTest().feel.amount;
				CHECK(shift == doctest::Approx((g % 2 == 1) ? 2.0 * (0.5 + 0.25 * a) - 1.0 : 0.0).epsilon(1e-9));
			}
		}
		releaseAndDrain(d, { 60 });
		checkBalanced(d.log);
	}
}

TEST_CASE("humanize Free: the walk reads Free Phase — unbounded, never Amount — and lands every note there") {
	namespace hz = feathers::mod::arp::humanize;
	// THE ONSETS A FREE 16 AT PHASE `phase` (and Amount `amount`) PLAYS, slot by slot.
	const auto onsets = [](double phase, double amount) {
		Drive d;
		d.s.feel = { hz::Mode::Free16, amount, 0, 0.0 };
		d.s.freePhase = phase;
		plainLine(d);
		d.note(Kind::On, 60);
		d.runTo(40 * kStep);
		const pat::Settings& s = d.d.settingsForTest();
		// THE PHASE AS STORED (through a float field).
		CHECK(s.feel.freePhase == static_cast<double>(static_cast<float>(phase)));
		std::vector<int64_t> at;
		for (int64_t g = 2; g < 38; ++g) {
			// THE SAME FEEL RESTATED BY HAND — the mode, and the phase as the field says — never the device's own Feel.
			const hz::Feel restated { hz::Mode::Free16, 0.0, 0, static_cast<double>(static_cast<float>(phase)) };
			const double shift = hz::shiftSteps(restated, g, static_cast<double>(kStep) / kRate, 0.25);
			const auto want = static_cast<int64_t>(std::ceil((static_cast<double>(g) + shift) * kStep - 1e-6));
			CAPTURE(g);
			CHECK(d.onsAt(want).size() == 1);
			at.push_back(want);
		}
		releaseAndDrain(d, { 60 });
		checkBalanced(d.log);
		return at;
	};
	const auto a = onsets(2.37, 0.2);
	CHECK(onsets(2.37, 0.9) == a); // Amount is not Free's: it moves nothing here
	CHECK(onsets(2.42, 0.2) != a); // the phase moves it
	// UNBOUNDED, AND IT DOES NOT WRAP: a whole unit on is another groove, and so is the next.
	const auto p0 = onsets(0.3, 0.5);
	const auto p1 = onsets(1.3, 0.5);
	const auto p40 = onsets(40.3, 0.5);
	CHECK(p0 != p1);
	CHECK(p1 != p40);
	CHECK(p0 != p40);
	// HONESTY: Amount is not drawn in the Free feels, Free Phase only there; Fine is retired.
	const uint32_t freeBits = hz::kFreeModes;
	CHECK((pat::kParamDescs[pat::at(pat::Global::HumanizeAmount)].gateMask & freeBits) == 0u);
	CHECK((pat::kParamDescs[pat::at(pat::Global::HumanizeAmount)].gateMask & (1u << static_cast<unsigned>(hz::Mode::Swing16))) != 0u);
	CHECK(pat::kParamDescs[pat::at(pat::Global::FreePhase)].gateMask == freeBits);
	CHECK(pat::kParamDescs[pat::at(pat::Global::FreePhase)].offsetsMayLeaveRange);
	CHECK(pat::kParamDescs[pat::at(pat::Global::FreePhase)].digitBox);
	// THE ROW CARRIES IT.
	pat::Settings s;
	s.freePhase = 123.5;
	const pat::Row row = pat::rowOf(s);
	const pat::Settings back = pat::settingsOf(row.data(), row.size());
	CHECK(back.freePhase == 123.5);
	CHECK(back.feel.freePhase == 123.5);
}

namespace {

/// **WOBBLE RESTATED BY HAND** (design §12-ter), from the doc rather than from `humanize.cpp`: per field f a rate offset
/// c_f (timing ½, velocity ⅓, length ¼, run ⅕); a step's wave is a triangle of duty 2^−level walked at φ^(1/(j + c_f))
/// times that duty, zero before its rise; its sign is −1 where frac((j + c_f)·φ) < ½; the run is
/// min(1, 4|b|)·sin 2π(j/G + frac(|b|/φ)).
struct WobbleRef {
	static constexpr double phi = 1.6180339887498948482;
	static double fr(double x) { return x - std::floor(x); }
	static int level(int j) {
		int n = 0;
		for (unsigned s = static_cast<unsigned>(j); s != 0 && (s & 1u) == 0; s >>= 1) {
			++n;
		}
		return n;
	}
	static double wave(double c, int j, double b) {
		const double duty = j > 0 ? std::pow(0.5, level(j)) : 1.0;
		const double p = fr(std::fabs(b) * std::pow(phi, 1.0 / (j + c)) * duty);
		if (p < 1.0 - duty) {
			return 0.0;
		}
		const double q = (p - (1.0 - duty)) / duty;
		return 1.0 - std::fabs(2.0 * q - 1.0);
	}
	static double sign(double c, int j) { return fr((j + c) * phi) < 0.5 ? -1.0 : 1.0; }
	static double shift(double b, int j, int grid) {
		const double run = std::min(1.0, 4.0 * std::fabs(b)) * std::sin(6.283185307179586 * (double(j) / grid + fr(std::fabs(b) / phi)));
		return std::clamp(0.32 * sign(0.5, j) * wave(0.5, j, b) + 0.18 * run, -0.45, 0.45);
	}
	static double velocity(double b, int j) {
		const double w = wave(1.0 / 3.0, j, b);
		if (sign(1.0 / 3.0, j) > 0.0 || j == 0) {
			return 1.0 + 0.25 * w;
		}
		return 1.0 - 0.55 * (j % 2 == 1 ? 1.0 : 0.5) * w;
	}
	static double length(double b, int j) { return 1.0 - 0.7 * wave(0.25, j, b); }
	/// THE EXTRAS: a FIXED duty d (burst .16, octave .3, strum .2, slide .5), the same rate law, firing over the half.
	static double extra(double c, double d, int j, double b) {
		const double p = fr(std::fabs(b) * std::pow(phi, 1.0 / (j + c)) * d);
		if (p < 1.0 - d) {
			return 0.0;
		}
		const double q = (p - (1.0 - d)) / d;
		return 1.0 - std::fabs(2.0 * q - 1.0);
	}
	static bool burst(double b, int j) { return extra(1.0 / 6.0, 0.16, j, b) > 0.5; }
	static bool octave(double b, int j) { return extra(1.0 / 7.0, 0.3, j, b) > 0.5; }
	static bool strum(double b, int j) { return extra(1.0 / 9.0, 0.2, j, b) > 0.5; }
	static bool slide(double b, int j) { return extra(0.4, 0.5, j, b) > 0.5; }
	static bool anyExtra(double b, int j) { return burst(b, j) || octave(b, j) || strum(b, j) || slide(b, j); }
	/// THE FIRST PHASE (from 1, by 0.01; float-exact steps) at which at least `n` of a bar's steps fire each listed extra.
	static double phaseWhere(std::initializer_list<bool (*)(double, int)> extras, int n) {
		for (int i = 100; i < 100000; ++i) {
			const auto b = static_cast<double>(static_cast<float>(i * 0.01));
			bool all = true;
			for (const auto f : extras) {
				int c = 0;
				for (int j = 0; j < 16; ++j) {
					c += f(b, j) ? 1 : 0;
				}
				all = all && c >= n;
			}
			if (all) {
				return b;
			}
		}
		return 0.0;
	}
};

} // namespace

TEST_CASE("wobble: the zone restated by hand — each field's wave, sign and the run, at every step of 16- and 24-step bars, phases far and near and below 0") {
	namespace wb = feathers::mod::arp::humanize::wobble;
	int moved = 0;
	for (const double b : { 0.013, 0.37, 2.9, 41.3, -1.7, 1234.567 }) {
		for (const int grid : { 16, 24 }) {
			for (int j = 0; j < grid; ++j) {
				CAPTURE(b);
				CAPTURE(grid);
				CAPTURE(j);
				CHECK(wb::shiftSteps(b, j, grid) == doctest::Approx(WobbleRef::shift(b, j, grid)).epsilon(1e-12).scale(1.0));
				CHECK(wb::velocity(b, j) == doctest::Approx(WobbleRef::velocity(b, j)).epsilon(1e-12));
				CHECK(wb::length(b, j) == doctest::Approx(WobbleRef::length(b, j)).epsilon(1e-12));
				moved += std::fabs(WobbleRef::shift(b, j, grid)) > 0.05 ? 1 : 0;
			}
		}
	}
	CHECK(moved > 50); // the zone moves things: the comparisons above are not of zeros
}

TEST_CASE("wobble guards: phase 0 is exactly nothing; the downbeat never dips; nothing is lengthened; no onset reaches its neighbour, alone or over Swing at full") {
	namespace hz = feathers::mod::arp::humanize;
	namespace wb = hz::wobble;
	// PHASE 0, TO THE BIT, through the feel's own readers.
	for (int64_t g = -40; g < 200; ++g) {
		const hz::Feel straight {};
		CHECK(hz::shiftSteps(straight, g, 0.125, 0.25) == 0.0);
		CHECK(hz::velocityScale(straight, g, 0.25) == 1.0);
		CHECK(hz::lengthScale(straight, g, 0.25) == 1.0);
		const hz::Feel swing { hz::Mode::Swing16, 0.7, 0, 0.0, 0.0 };
		const hz::Feel swingNoWobble { hz::Mode::Swing16, 0.7, 0, 0.0 };
		CHECK(hz::shiftSteps(swing, g, 0.125, 0.25) == hz::shiftSteps(swingNoWobble, g, 0.125, 0.25));
	}
	for (double b = -20.0; b < 60.0; b += 0.0371) {
		CAPTURE(b);
		CHECK(wb::velocity(b, 0) >= 1.0);
		for (int j = 0; j < 16; ++j) {
			CHECK(wb::length(b, j) <= 1.0);
			CHECK(wb::length(b, j) >= 0.3 - 1e-12);
		}
	}
	// NO REORDER: slot g's onset g + shift is strictly increasing, whatever the phase and the feel under it.
	for (const hz::Mode mode : { hz::Mode::Straight, hz::Mode::Swing16, hz::Mode::Free16, hz::Mode::Drunk }) {
		for (const double b : { 0.29, 3.3, 17.77, 400.01 }) {
			hz::Feel f { mode, 1.0, 9, 5.5, b };
			double last = -1e9;
			for (int64_t g = 0; g < 10000; ++g) {
				const double at = static_cast<double>(g) + hz::shiftSteps(f, g, 0.125, 0.25);
				CAPTURE(g);
				REQUIRE(at > last);
				last = at;
			}
		}
	}
}

TEST_CASE("wobble in the Pattern device: every note's onset, velocity and length, restated by hand at its step, under Straight") {
	namespace hz = feathers::mod::arp::humanize;
	const double b = 3.71;
	Drive d;
	d.s.feel = { hz::Mode::Straight, 0.0, 0, 0.0, b };
	plainLine(d);
	d.note(Kind::On, 60);
	d.runTo(40 * kStep);
	REQUIRE(d.d.settingsForTest().feel.wobble == static_cast<double>(static_cast<float>(b)));
	const double bb = d.d.settingsForTest().feel.wobble;
	const auto& cycle = d.d.cycleForTest(0);
	REQUIRE(cycle.length > 0);
	const auto onsetOf = [&](int64_t g) { return static_cast<double>(g) + WobbleRef::shift(bb, static_cast<int>(g % 16), 16); };
	int lengths = 0;
	for (int64_t g = 2; g < 36; ++g) {
		CAPTURE(g);
		const auto want = static_cast<int64_t>(std::ceil(onsetOf(g) * kStep - 1e-6));
		const auto ons = d.onsAt(want);
		REQUIRE(ons.size() == 1);
		const auto& st = cycle.steps[static_cast<size_t>(g % static_cast<int64_t>(cycle.length))];
		const double vel = std::clamp(static_cast<double>(st.velocity) * WobbleRef::velocity(bb, static_cast<int>(g % 16)), 0.0, 1.0);
		CHECK(ons[0].velocity == doctest::Approx(vel).epsilon(1e-9));
		// ITS OFF: the gate, clipped, of the shifted step (a slide or a tie is not clipped; plainLine has neither).
		REQUIRE(!st.slide);
		if (st.gate <= 1.0f && !WobbleRef::burst(bb, static_cast<int>(g % 16))) { // (a burst re-times the hits: its own case)
			const double clipped = static_cast<double>(st.gate) * WobbleRef::length(bb, static_cast<int>(g % 16));
			const double off = onsetOf(g) + clipped * (onsetOf(g + 1) - onsetOf(g));
			int64_t got = -1;
			for (const Logged& l : d.log) {
				if (l.kind == Kind::Off && l.key == ons[0].key && l.sample > want) {
					got = l.sample;
					break;
				}
			}
			CHECK(std::llabs(got - static_cast<int64_t>(std::ceil(off * kStep - 1e-6))) <= 1);
			++lengths;
		}
	}
	CHECK(lengths > 20);
	releaseAndDrain(d, { 60 });
	checkBalanced(d.log);
}

TEST_CASE("wobble never clips a slide: in TB-Mono every slide's off still lands past the next note's on") {
	namespace hz = feathers::mod::arp::humanize;
	Drive d;
	d.s.feel = { hz::Mode::Straight, 0.0, 0, 0.0, 7.3 };
	d.s.patterns[0].mode = genes::Mode::TbMono;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Glide)] = 1.0f; // every step that can, slides
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Hold)] = 0.0f;
	d.apply();
	d.note(Kind::On, 48);
	d.runTo(40 * kStep);
	const double b = d.d.settingsForTest().feel.wobble;
	const auto at = [&](int64_t g) {
		return static_cast<int64_t>(std::ceil((static_cast<double>(g) + WobbleRef::shift(b, static_cast<int>(g % 16), 16)) * kStep - 1e-6));
	};
	int overlaps = 0;
	int clippedElsewhere = 0;
	for (int64_t g = 1; g < 38; ++g) {
		const std::vector<Logged> ons = d.onsAt(at(g));
		const std::vector<Logged> next = d.onsAt(at(g + 1));
		if (ons.size() != 1 || next.size() != 1 || ons[0].key == next[0].key) {
			continue;
		}
		int64_t offAt = -1;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::Off && l.key == ons[0].key && l.sample > ons[0].sample) {
				offAt = l.sample;
				break;
			}
		}
		CAPTURE(g);
		CHECK(offAt > next[0].sample); // THE SLIDE STILL OVERLAPS, however short Wobble would have clipped the step
		clippedElsewhere += WobbleRef::length(b, static_cast<int>(g % 16)) < 0.8 ? 1 : 0;
		++overlaps;
	}
	CHECK(overlaps > 10);
	CHECK(clippedElsewhere > 0); // Wobble would have clipped some of these steps: the guard is what kept them
	releaseAndDrain(d, { 48 });
	checkBalanced(d.log);
}

TEST_CASE("wobble is a function of the song position: a locate plays exactly what the run from the start played there") {
	namespace hz = feathers::mod::arp::humanize;
	const auto run = [](double locateBeats, int64_t from, int64_t to) {
		Drive d;
		d.s.feel = { hz::Mode::Swing16, 0.4, 0, 0.0, 9.17 };
		plainLine(d);
		d.locate(locateBeats);
		d.note(Kind::On, 60);
		d.runTo(to);
		std::vector<std::pair<int64_t, double>> out;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::On && l.sample >= from) {
				out.emplace_back(l.sample + static_cast<int64_t>(locateBeats * 4.0) * kStep, l.velocity);
			}
		}
		return out;
	};
	const auto whole = run(0.0, 0, 64 * kStep);
	const auto tail = run(8.0, 0, 32 * kStep); // starts at beat 8 = slot 32
	std::vector<std::pair<int64_t, double>> wholeTail;
	for (const auto& e : whole) {
		if (e.first >= 32 * kStep + kStep && e.first < 64 * kStep) {
			wholeTail.push_back(e);
		}
	}
	std::vector<std::pair<int64_t, double>> tailCut;
	for (const auto& e : tail) {
		if (e.first >= 32 * kStep + kStep && e.first < 64 * kStep) {
			tailCut.push_back(e);
		}
	}
	REQUIRE(wholeTail.size() > 20);
	CHECK(tailCut == wholeTail);
}

TEST_CASE("wobble extras restated: each fires where its fixed-duty triangle is over the half, and over a long walk fires on half its duty — a burst 8%, an octave 15%, a strum 10%, a slide 25%") {
	namespace wb = feathers::mod::arp::humanize::wobble;
	using F = wb::Field;
	for (const double b : { 0.77, 5.5, 63.25, -9.1, 777.7 }) {
		for (int j = 0; j < 24; ++j) {
			CAPTURE(b);
			CAPTURE(j);
			CHECK(wb::fires(F::Burst, j, b) == WobbleRef::burst(b, j));
			CHECK(wb::fires(F::Octave, j, b) == WobbleRef::octave(b, j));
			CHECK(wb::fires(F::Strum, j, b) == WobbleRef::strum(b, j));
			CHECK(wb::fires(F::Slide, j, b) == WobbleRef::slide(b, j));
		}
	}
	// THE DUTY, MEASURED OVER THE WALK (every step of the bar, phases 0..400).
	const auto share = [](bool (*f)(double, int)) {
		int64_t on = 0;
		int64_t all = 0;
		for (double b = 0.0; b < 400.0; b += 0.01) {
			for (int j = 0; j < 16; ++j) {
				on += f(b, j) ? 1 : 0;
				++all;
			}
		}
		return static_cast<double>(on) / static_cast<double>(all);
	};
	CHECK(share(WobbleRef::burst) == doctest::Approx(0.08).epsilon(0.1));
	CHECK(share(WobbleRef::octave) == doctest::Approx(0.15).epsilon(0.1));
	CHECK(share(WobbleRef::strum) == doctest::Approx(0.10).epsilon(0.1));
	CHECK(share(WobbleRef::slide) == doctest::Approx(0.25).epsilon(0.1));
	for (int j = 0; j < 16; ++j) {
		CHECK_FALSE(wb::fires(F::Burst, j, 0.0)); // never at phase 0
		CHECK_FALSE(wb::fires(F::Slide, j, 0.0));
	}
}

TEST_CASE("wobble extras by Mode: a slide only in the TB modes, a burst only where ratchets strike (never on a slide), an octave never in Drum, Modulation or TB-Glide, a strum only on a chord in Chords or Progression; none at Wobble 0") {
	namespace hz = feathers::mod::arp::humanize;
	namespace pn = feathers::mod::arp::pattern;
	using M = genes::Mode;
	const double b = WobbleRef::phaseWhere({ WobbleRef::burst, WobbleRef::octave, WobbleRef::strum, WobbleRef::slide }, 2);
	REQUIRE(b > 0.0);
	for (const bool zero : { false, true }) {
		Drive d;
		d.s.feel = { hz::Mode::Straight, 0.0, 0, 0.0, zero ? 0.0 : b };
		d.apply();
		const double bb = d.d.settingsForTest().feel.wobble;
		pn::PatternStep note;
		note.kind = pn::PatternStep::Kind::Note;
		note.count = 1;
		pn::PatternStep chord = note;
		chord.count = 3;
		pn::PatternStep slid = note;
		slid.slide = true;
		int fired = 0;
		for (int m = 0; m < static_cast<int>(M::kCount); ++m) {
			const auto mode = static_cast<M>(m);
			for (int64_t g = 0; g < 32; ++g) {
				CAPTURE(genes::kModeNames[m]);
				CAPTURE(g);
				const int j = static_cast<int>(g % 16);
				const auto x = d.d.wobbleExtrasForTest(mode, note, g);
				const auto xc = d.d.wobbleExtrasForTest(mode, chord, g);
				const auto xs = d.d.wobbleExtrasForTest(mode, slid, g);
				const bool live = !zero;
				const bool tb = mode == M::TbMono || mode == M::TbGlide;
				CHECK(x.slide == (live && tb && WobbleRef::slide(bb, j)));
				const bool ratchets = mode == M::Melody || mode == M::Chords || mode == M::TbMono;
				const bool burst = live && ratchets && !x.slide && WobbleRef::burst(bb, j);
				CHECK(x.burst == (burst ? (WobbleRef::sign(1.0 / 6.0, j) > 0.0 ? 4 : 3) : 0));
				CHECK(xs.burst == 0); // never a slide
				const bool pitched = mode == M::Melody || mode == M::MelodyMono || mode == M::Chords || mode == M::TbMono
				                     || mode == M::Progression;
				CHECK(x.octave == (live && pitched && WobbleRef::octave(bb, j) ? (WobbleRef::sign(1.0 / 7.0, j) > 0.0 ? 12 : -12) : 0));
				CHECK_FALSE(x.strum); // one tone: nothing to strum
				const bool strums = live && (mode == M::Chords || mode == M::Progression) && WobbleRef::strum(bb, j);
				CHECK(xc.strum == strums);
				if (strums) {
					CHECK(xc.strumDown == (WobbleRef::sign(1.0 / 9.0, j) < 0.0));
				}
				fired += (x.slide || x.burst != 0 || x.octave != 0 || xc.strum) ? 1 : 0;
			}
		}
		if (zero) {
			CHECK(fired == 0);
		}
		else {
			CHECK(fired > 20);
		}
	}
}

TEST_CASE("wobble extras in the device, by value: an octave jump plays the decoded key ±12, a burst strikes 3 or 4 hits in its step, a chord strums its tones a share of the step apart, a TB step slides into the next") {
	namespace hz = feathers::mod::arp::humanize;
	const auto onsetOf = [](double bb, int64_t g) { return static_cast<double>(g) + WobbleRef::shift(bb, static_cast<int>(g % 16), 16); };
	const auto sampleOf = [](double u) { return static_cast<int64_t>(std::ceil(u * kStep - 1e-6)); };
	SUBCASE("Melody: octaves and bursts") {
		const double b = WobbleRef::phaseWhere({ WobbleRef::octave, WobbleRef::burst }, 2);
		Drive plain;
		plainLine(plain);
		plain.note(Kind::On, 60);
		plain.runTo(40 * kStep);
		Drive d;
		d.s.feel = { hz::Mode::Straight, 0.0, 0, 0.0, b };
		plainLine(d);
		d.note(Kind::On, 60);
		d.runTo(40 * kStep);
		const double bb = d.d.settingsForTest().feel.wobble;
		int octaves = 0;
		int bursts = 0;
		for (int64_t g = 2; g < 36; ++g) {
			CAPTURE(g);
			const int j = static_cast<int>(g % 16);
			const auto base = plain.onsAt(g * kStep);
			REQUIRE(base.size() == 1);
			const int64_t from = sampleOf(onsetOf(bb, g));
			const int64_t to = sampleOf(onsetOf(bb, g + 1));
			std::vector<Logged> inStep;
			for (const Logged& l : d.log) {
				if (l.kind == Kind::On && l.sample >= from && l.sample < to) {
					inStep.push_back(l);
				}
			}
			REQUIRE(!inStep.empty());
			const int jump = WobbleRef::octave(bb, j) ? (WobbleRef::sign(1.0 / 7.0, j) > 0.0 ? 12 : -12) : 0;
			CHECK(inStep[0].key == base[0].key + jump);
			octaves += jump != 0 ? 1 : 0;
			const size_t hits = WobbleRef::burst(bb, j) ? (WobbleRef::sign(1.0 / 6.0, j) > 0.0 ? 4u : 3u) : 1u;
			CHECK(inStep.size() == hits);
			bursts += hits > 1 ? 1 : 0;
		}
		CHECK(octaves > 0);
		CHECK(bursts > 0);
		releaseAndDrain(d, { 60 });
		checkBalanced(d.log);
	}
	SUBCASE("Chords: strums") {
		const double b = WobbleRef::phaseWhere({ WobbleRef::strum }, 3);
		Drive d;
		d.s.feel = { hz::Mode::Straight, 0.0, 0, 0.0, b };
		d.s.patterns[0].mode = genes::Mode::Chords;
		plainLine(d);
		d.note(Kind::On, 60);
		d.runTo(40 * kStep);
		const double bb = d.d.settingsForTest().feel.wobble;
		int strums = 0;
		for (int64_t g = 2; g < 36; ++g) {
			const int j = static_cast<int>(g % 16);
			if (!WobbleRef::strum(bb, j) || WobbleRef::burst(bb, j)) {
				continue;
			}
			CAPTURE(g);
			const double on = onsetOf(bb, g);
			const double len = onsetOf(bb, g + 1) - on;
			std::vector<int64_t> at;
			for (const Logged& l : d.log) {
				if (l.kind == Kind::On && l.sample >= sampleOf(on) && l.sample < sampleOf(on + len)) {
					at.push_back(l.sample);
				}
			}
			if (at.size() < 2) {
				continue; // a held common tone, or a one-tone chord: nothing to spread
			}
			std::sort(at.begin(), at.end());
			const auto n = static_cast<double>(d.d.cycleForTest(0).steps[static_cast<size_t>(g % static_cast<int64_t>(d.d.cycleForTest(0).length))].count);
			REQUIRE(n >= 2.0);
			for (size_t i = 0; i < at.size(); ++i) {
				CHECK(std::llabs(at[i] - sampleOf(on + 0.3 * len * static_cast<double>(i) / (n - 1.0))) <= 1);
			}
			++strums;
		}
		CHECK(strums > 0);
		releaseAndDrain(d, { 60 });
		checkBalanced(d.log);
	}
	SUBCASE("TB-Mono: a forced slide overlaps the next note") {
		const double b = WobbleRef::phaseWhere({ WobbleRef::slide }, 4);
		Drive d;
		d.s.feel = { hz::Mode::Straight, 0.0, 0, 0.0, b };
		d.s.patterns[0].mode = genes::Mode::TbMono;
		d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Glide)] = 0.0f; // the terrain slides nowhere: Wobble's alone
		d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
		d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Hold)] = 0.0f;
		d.apply();
		d.note(Kind::On, 48);
		d.runTo(40 * kStep);
		const double bb = d.d.settingsForTest().feel.wobble;
		const auto& cycle = d.d.cycleForTest(0);
		int slides = 0;
		for (int64_t g = 2; g < 36; ++g) {
			const int j = static_cast<int>(g % 16);
			const auto& st = cycle.steps[static_cast<size_t>(g % static_cast<int64_t>(cycle.length))];
			const auto ons = d.onsAt(sampleOf(onsetOf(bb, g)));
			const auto next = d.onsAt(sampleOf(onsetOf(bb, g + 1)));
			if (ons.size() != 1 || next.size() != 1 || ons[0].key == next[0].key || st.slide) {
				continue;
			}
			int64_t offAt = -1;
			for (const Logged& l : d.log) {
				if (l.kind == Kind::Off && l.key == ons[0].key && l.sample > ons[0].sample) {
					offAt = l.sample;
					break;
				}
			}
			CAPTURE(g);
			CHECK((offAt > next[0].sample) == WobbleRef::slide(bb, j)); // slides exactly where Wobble says, overlapping
			slides += WobbleRef::slide(bb, j) ? 1 : 0;
		}
		CHECK(slides > 0);
		releaseAndDrain(d, { 48 });
		checkBalanced(d.log);
	}
}

TEST_CASE("humanize: Straight, and every feel at amount 0, plays exactly the events of no feel at all") {
	namespace hz = feathers::mod::arp::humanize;
	Drive plain;
	plain.s.feel = { hz::Mode::Straight, 0.0, 0 };
	plain.s.patterns[0].mode = genes::Mode::Chords;
	plain.apply();
	plain.note(Kind::On, 60);
	plain.runTo(24 * kStep);
	for (int m = 0; m < static_cast<int>(hz::Mode::kCount); ++m) {
		Drive d;
		d.s.feel = { static_cast<hz::Mode>(m), 0.0, 5 };
		d.s.patterns[0].mode = genes::Mode::Chords;
		d.apply();
		d.note(Kind::On, 60);
		d.runTo(24 * kStep);
		CAPTURE(hz::kModeNames[m]);
		REQUIRE(d.log.size() == plain.log.size());
		for (size_t i = 0; i < d.log.size(); ++i) {
			CHECK(d.log[i].kind == plain.log[i].kind);
			CHECK(d.log[i].key == plain.log[i].key);
			CHECK(d.log[i].sample == plain.log[i].sample);
			CHECK(d.log[i].velocity == plain.log[i].velocity);
		}
	}
}

TEST_CASE("humanize: the same feel at any render size lands the same events, and onsets never cross over 10^4 slots") {
	namespace hz = feathers::mod::arp::humanize;
	for (int m = 1; m < static_cast<int>(hz::Mode::kCount); ++m) {
		const hz::Feel feel { static_cast<hz::Mode>(m), 1.0, 3 };
		CAPTURE(hz::kModeNames[m]);
		// MONOTONIC, by value, at a long step and at a very short one (where milliseconds are most of a step).
		for (const double stepSeconds : { 0.125, 0.02 }) {
			double last = -1e9;
			for (int64_t g = -100; g < 10000; ++g) {
				const double at = static_cast<double>(g) + hz::shiftSteps(feel, g, stepSeconds, 0.25);
				CHECK(at > last);
				last = at;
			}
		}
	}
	// THE RENDER SIZE: 32-sample chunks and 7-sample chunks give the same events.
	for (const hz::Mode mode : { hz::Mode::Loose, hz::Mode::Drunk, hz::Mode::Groove8, hz::Mode::StrumUp, hz::Mode::Flam }) {
		Drive a;
		Drive b;
		for (Drive* d : { &a, &b }) {
			d->s.feel = { mode, 0.9, 21 };
			d->s.patterns[0].mode = genes::Mode::Chords;
			d->apply();
			d->note(Kind::On, 62);
		}
		a.runTo(20 * kStep);
		while (b.now < 20 * kStep) {
			const int64_t until = std::min<int64_t>(b.now + 7, 20 * kStep);
			b.runTo(until);
		}
		CAPTURE(hz::kModeNames[static_cast<size_t>(mode)]);
		REQUIRE(a.log.size() == b.log.size());
		for (size_t i = 0; i < a.log.size(); ++i) {
			CHECK(a.log[i].sample == b.log[i].sample);
			CHECK(a.log[i].key == b.log[i].key);
		}
	}
}

TEST_CASE("humanize: a strum lands each chord tone a share of the step later, in pitch order; a flam's grace comes first") {
	namespace hz = feathers::mod::arp::humanize;
	for (const hz::Mode mode : { hz::Mode::StrumUp, hz::Mode::StrumDown }) {
		Drive d;
		d.s.feel = { mode, 1.0, 0 };
		d.s.patterns[0].mode = genes::Mode::Chords;
		d.s.patterns[0].size = feathers::mod::arp::theory::ChordSize::Seventh;
		plainLine(d);
		d.note(Kind::On, 57);
		d.runTo(20 * kStep);
		const auto& cycle = d.d.cycleForTest(0);
		int strums = 0;
		for (int64_t g = 2; g < 18; ++g) {
			const auto& st = cycle.steps[static_cast<size_t>(g % 16)];
			if (st.kind != StepKind::Note || st.holdMask != 0 || st.count < 2) {
				continue;
			}
			for (int i = 0; i < st.count; ++i) {
				const int j = mode == hz::Mode::StrumDown ? st.count - 1 - i : i;
				const double at = static_cast<double>(g) + hz::strumShare(d.d.settingsForTest().feel, j, st.count);
				const auto sample = static_cast<int64_t>(std::ceil(at * kStep - 1e-6));
				bool found = false;
				for (const Logged& l : d.log) {
					found = found || (l.kind == Kind::On && l.key == st.keys[static_cast<size_t>(i)] && l.sample == sample);
				}
				CAPTURE(g);
				CAPTURE(i);
				CHECK(found);
			}
			++strums;
		}
		CHECK(strums > 4);
		releaseAndDrain(d, { 57 });
		checkBalanced(d.log);
	}
	// THE FLAM, on a line: a soft grace on the moved-early onset, then the note on its straight sample.
	Drive f;
	f.s.feel = { hz::Mode::Flam, 1.0, 0 };
	plainLine(f);
	f.note(Kind::On, 60);
	f.runTo(10 * kStep);
	const double lead = hz::flamSteps(f.d.settingsForTest().feel, static_cast<double>(kStep) / kRate);
	REQUIRE(lead > 0.0);
	for (int64_t g = 2; g < 9; ++g) {
		const auto early = static_cast<int64_t>(std::ceil((static_cast<double>(g) - lead) * kStep - 1e-6));
		CHECK(f.onsAt(early).size() == 1);
		CHECK(f.onsAt(g * kStep).size() == 1);
	}
	releaseAndDrain(f, { 60 });
	checkBalanced(f.log);
}

TEST_CASE("pattern device: the Accent Sweep source is the 303's stacking charge — the last accents, each decaying, by value") {
	// DESIGN §13 RESTATED: Σ over the newest eight accented onsets of 0.4·e^(−age/0.3 s), at most 1, at the clock call's
	// sample — derived from the pattern and the grid, so it needs nothing stored.
	Drive d;
	d.s.patterns[0].mode = genes::Mode::TbMono;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Accent)] = 0.7f;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
	d.apply();
	d.note(Kind::On, 48);
	int checked = 0;
	for (int64_t until = 3 * kStep + 1000; until < 30 * kStep; until += 2 * kStep + 777) {
		d.runTo(until);
		const auto& cycle = d.d.cycleForTest(0);
		const double u = static_cast<double>(d.lastCall) / kStep;
		double sum = 0.0;
		int found = 0;
		// AS IF THE PATTERN HAD ALWAYS BEEN PLAYING — the slots before the first are read from the cycle too, which is
		// what makes the value a function of the position alone (a scrub reads the same).
		for (int64_t g = static_cast<int64_t>(std::floor(u)); g > static_cast<int64_t>(std::floor(u)) - 64
		                                                       && found < pat::kAccentSweepDepth; --g) {
			const auto& st = cycle.steps[static_cast<size_t>(((g % 16) + 16) % 16)];
			if (st.kind == StepKind::Note && st.accent) {
				sum += pat::kAccentSweepCharge
				       * std::exp(-(static_cast<double>(d.lastCall - g * kStep) / kRate) / pat::kAccentSweepSeconds);
				++found;
			}
		}
		CAPTURE(until);
		CHECK(d.d.outputAt(static_cast<size_t>(pat::Output::AccentSweep))
		      == doctest::Approx(std::min(1.0, sum)).epsilon(1e-5));
		checked += sum > 0.05 ? 1 : 0;
	}
	CHECK(checked > 5);
}

namespace {

/// A TEXT VIEW OVER A ROW, as a host slot builds one (`hostslots::TextSite`).
struct RowView {
	pat::Row row {};
	static float field(const void* host, size_t control) { return static_cast<const RowView*>(host)->row[control]; }
	device::TextView view() const {
		device::TextView v;
		v.host = this;
		v.field = &RowView::field;
		return v;
	}
};

std::string format(const RowView& rv, size_t control, double value) {
	char out[128] = "";
	REQUIRE(pat::kTextOps.format(control, value, rv.view(), out, sizeof out));
	return out;
}

} // namespace

TEST_CASE("pattern text: the selector names the pattern it picks — '2 · D Dorian', a chord's degree and inversion, '(solo)', '— (none enabled)'") {
	pat::Settings s;
	s.keys = feathers::mod::arp::pattern::Keys::Pool;
	s.patterns[1].enable = true;
	s.patterns[1].root = 2;
	s.patterns[1].scale = 1; // Dorian
	s.patterns[2].enable = true;
	s.patterns[2].mode = genes::Mode::Chords;
	s.patterns[2].scale = 5; // Minor
	s.patterns[2].degree = 3;
	s.patterns[2].inversion = 1; // the 1st (0 is root position; Auto is theory::kAuto)
	RowView rv;
	rv.row = pat::rowOf(s);
	const size_t sel = pat::at(pat::Global::Selector);
	// THREE IN RANGE: the selector's thirds.
	CHECK(format(rv, sel, 0.1) == "1 · C Major");
	CHECK(format(rv, sel, 0.5) == "2 · D Dorian");
	CHECK(format(rv, sel, 0.9) == "3 · C Minor, iv, 1st inv");
	// PARSE BACK: a pattern's number lands in its band.
	double v = -1.0;
	REQUIRE(pat::kTextOps.parse(sel, "Pattern 3", rv.view(), &v));
	CHECK(format(rv, sel, v) == "3 · C Minor, iv, 1st inv");
	REQUIRE(pat::kTextOps.parse(sel, "2 · D Dorian", rv.view(), &v));
	CHECK(format(rv, sel, v) == "2 · D Dorian");
	CHECK_FALSE(pat::kTextOps.parse(sel, "4", rv.view(), &v)); // pattern 4 is not in range
	// SOLO: the solo pins it, and says so.
	s.patterns[3].solo = true;
	rv.row = pat::rowOf(s);
	CHECK(format(rv, sel, 0.1) == "4 · C Major (solo)");
	// NONE IN RANGE.
	for (auto& q : s.patterns) {
		q.enable = false;
		q.solo = false;
	}
	rv.row = pat::rowOf(s);
	CHECK(format(rv, sel, 0.5) == "\u2014 (none enabled)");
	// UNDER TRANSPOSE THE ROOT IS THE KEY'S, so it is not named.
	s.keys = feathers::mod::arp::pattern::Keys::Transpose;
	s.patterns[1].enable = true;
	rv.row = pat::rowOf(s);
	CHECK(format(rv, sel, 0.5) == "2 · Dorian");
}

TEST_CASE("pattern text: Root, Scale, Degree, Inversion and Mode print as themselves and parse back, every value") {
	pat::Settings s;
	s.patterns[0].mode = genes::Mode::Chords;
	s.patterns[0].size = feathers::mod::arp::theory::ChordSize::Seventh;
	RowView rv;
	rv.row = pat::rowOf(s);
	const auto trip = [&](pat::Field f, int count) {
		const size_t c = pat::at(0, f);
		for (int i = 0; i < count; ++i) {
			const std::string text = format(rv, c, i);
			double back = -1.0;
			CAPTURE(text);
			REQUIRE(pat::kTextOps.parse(c, text.c_str(), rv.view(), &back));
			CHECK(back == static_cast<double>(i));
		}
	};
	trip(pat::Field::Root, 12);
	trip(pat::Field::Scale, static_cast<int>(feathers::mod::arp::theory::kScaleCount));
	trip(pat::Field::Mode, static_cast<int>(pat::kModeChoices));
	trip(pat::Field::Degree, 7);
	trip(pat::Field::Inversion, 5);
	// THE NUMERALS ARE CASED BY QUALITY: C major's 7ths, I to vii.
	const size_t deg = pat::at(0, pat::Field::Degree);
	const std::vector<std::string> want { "Imaj7", "ii7", "iii7", "IVmaj7", "V7", "vi7", "viiø7" };
	for (int d = 0; d < 7; ++d) {
		CHECK(format(rv, deg, d) == want[static_cast<size_t>(d)]);
	}
	// AN INVERSION PAST THE CHORD'S TONES SAYS WHAT PLAYS.
	s.patterns[0].size = feathers::mod::arp::theory::ChordSize::Triad;
	rv.row = pat::rowOf(s);
	CHECK(format(rv, pat::at(0, pat::Field::Inversion), 4) == "3rd → root (Triad)");
	// SPELLINGS: either accidental, either case.
	double v = -1.0;
	REQUIRE(pat::kTextOps.parse(pat::at(0, pat::Field::Root), "db", rv.view(), &v));
	CHECK(v == 1.0);
	REQUIRE(pat::kTextOps.parse(pat::at(0, pat::Field::Scale), "phrygian d", rv.view(), &v));
	CHECK(feathers::mod::arp::theory::kScales[static_cast<size_t>(v)].name == std::string("Phrygian Dominant"));
	// IN THE LINE MODES THE DEGREE IS A TRANSPOSITION.
	s.patterns[0].mode = genes::Mode::Melody;
	rv.row = pat::rowOf(s);
	CHECK(format(rv, deg, 3) == "+3 degrees");
	REQUIRE(pat::kTextOps.parse(deg, "+3 degrees", rv.view(), &v));
	CHECK(v == 3.0);
}

TEST_CASE("pattern text: a hand change of a control the words follow owes a rescan — every control that says so") {
	// THE DECLARATION `SlotOwner::retextIfWordsFollow` READS (`ParamDesc::retextsWords`): the selector's words follow
	// each pattern's Enable, Solo, Mode, Root, Scale, Degree, Size and Inversion, and the global Keys.
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		for (const pat::Field f : { pat::Field::Enable, pat::Field::Solo, pat::Field::Mode, pat::Field::Root, pat::Field::Scale,
		                            pat::Field::Degree, pat::Field::Size, pat::Field::Inversion }) {
			CHECK(pat::kParamDescs[pat::at(p, f)].retextsWords);
		}
		CHECK_FALSE(pat::kParamDescs[pat::at(p, pat::Field::Rate)].retextsWords);
	}
	CHECK(pat::kParamDescs[pat::at(pat::Global::Keys)].retextsWords);
	CHECK_FALSE(pat::kParamDescs[pat::at(pat::Global::Selector)].retextsWords);
}

#include "mod/arp/pattern_likes.h"

TEST_CASE("likes: a cycle's fingerprint, by value — rhythm, contour, register, articulation, chords and the family, each block summed to 1") {
	namespace lk = feathers::mod::arp::likes;
	namespace pn = feathers::mod::arp::pattern;
	pn::PatternCycle c;
	c.length = 4;
	c.playable = true;
	// C4, rest, E4 (accented, sliding), tie.
	c.steps[0].kind = pn::PatternStep::Kind::Note;
	c.steps[0].keys[0] = 60;
	c.steps[0].count = 1;
	c.steps[0].velocity = 0.5f;
	c.steps[1].kind = pn::PatternStep::Kind::Rest;
	c.steps[2].kind = pn::PatternStep::Kind::Note;
	c.steps[2].keys[0] = 64;
	c.steps[2].count = 1;
	c.steps[2].velocity = 1.0f;
	c.steps[2].accent = true;
	c.steps[2].slide = true;
	c.steps[3].kind = pn::PatternStep::Kind::Tie;
	const lk::Fingerprint f = lk::fingerprintOf(c, genes::Mode::TbMono, 60);
	// RHYTHM: onsets at 0/4 and 2/4 of the cycle → bins 0 and 8, weighted 0.5 and 1.0, normalised.
	CHECK(f[0] == doctest::Approx(1.0 / 3.0));
	CHECK(f[8] == doctest::Approx(2.0 / 3.0));
	// CONTOUR: one interval, +4 semitones, clamped to +5 at most: bin 4 + 6 = 10.
	CHECK(f[lk::kRhythm + 10] == doctest::Approx(1.0));
	// REGISTER: both in the band (key − 60 + 18) / 6 = 3.
	CHECK(f[lk::kRhythm + lk::kContour + 3] == doctest::Approx(1.0));
	// ARTICULATION: note 2, tie 1, rest 1, accent 1, slide 1, ratchet 0 → over 6.
	const size_t art = lk::kRhythm + lk::kContour + lk::kRegister;
	CHECK(f[art + 0] == doctest::Approx(2.0 / 6.0));
	CHECK(f[art + 1] == doctest::Approx(1.0 / 6.0));
	CHECK(f[art + 3] == doctest::Approx(1.0 / 6.0));
	// THE FAMILY: TB, one-hot.
	CHECK(f[lk::kFingerprint - 1] == 1.0f);
	CHECK(lk::distance(f, f) == 0.0);
	CHECK(lk::distance(f, lk::fingerprintOf(c, genes::Mode::Melody, 60)) == lk::kAcrossFamilies);
}

TEST_CASE("likes: the block keeps each slot's likes — a like written into a device is the like a FRESH instance restores") {
	namespace lk = feathers::mod::arp::likes;
	lk::AllLikes all {};
	all[2].count = 2;
	all[2].like[0].placeX = 0.25f;
	all[2].like[0].heard[5] = 0.75f;
	all[2].like[1].gamma = 12.0f;
	all[2].like[1].point[40] = -0.125f; // the full point survives too
	all[3].count = 1;
	all[3].like[0].zone = 300.0f;
	std::vector<float> words(lk::kBlockWords);
	lk::write(all, words.data());
	pat::PatternDevice a;
	REQUIRE(pat::applyBlock(&a, pat::kLikesBlock, words.data(), words.size()));
	std::vector<float> saved(lk::kBlockWords);
	REQUIRE(pat::readBlock(&a, pat::kLikesBlock, saved.data(), saved.size()) > 0);
	pat::PatternDevice b;
	REQUIRE(pat::restoreBlock(&b, pat::kLikesBlock, saved.data(), saved.size()));
	std::vector<float> back(lk::kBlockWords);
	REQUIRE(pat::readBlock(&b, pat::kLikesBlock, back.data(), back.size()) > 0);
	lk::AllLikes got {};
	REQUIRE(lk::read(back.data(), back.size(), &got));
	CHECK(got[0].count == 0);
	CHECK(got[2].count == 2);
	CHECK(got[2].like[0].placeX == 0.25f);
	CHECK(got[2].like[0].heard[5] == 0.75f);
	CHECK(got[2].like[1].gamma == 12.0f);
	CHECK(got[2].like[1].point[40] == -0.125f);
	CHECK(got[3].like[0].zone == 300.0f);
	// THE SCORE: a fingerprint liked scores 1 against its own slot.
	CHECK(lk::score(got[2], got[2].like[0].heard) == doctest::Approx(1.0));
}

TEST_CASE("likes: a stored point lands where it was taken on its own plane, and follows the pattern through a turn of Rotate") {
	namespace gn = feathers::mod::arp::genes;
	pat::PatternSettings q;
	q.zone = 400;
	q.gamma = 1.5;
	q.rotate = 0.3f;
	q.placeX = 0.2f;
	q.placeY = -0.15f;
	pat::HandSlot hand;
	// ON ITS OWN PLANE: the place it was decoded at, to float storage's precision.
	const auto pointAt = [&](const pat::PatternSettings& at, const pat::HandSlot& h) {
		return pat::pointOf(pat::PatternDevice::placeOf(at, h.residue));
	};
	const auto here = pointAt(q, hand);
	pat::PlaneMark m = pat::markOn(q, hand.residue, here);
	REQUIRE(m.solved);
	CHECK(m.x == doctest::Approx(0.2).epsilon(1e-4));
	CHECK(m.y == doctest::Approx(-0.15).epsilon(1e-4));
	CHECK(m.offPlane < 1e-4);
	// AWAY FROM THE PUCK: a like at (0.6, 0.1) on the same plane.
	pat::PatternSettings away = q;
	away.placeX = 0.6f;
	away.placeY = 0.1f;
	const auto there = pointAt(away, hand);
	// THE TURN, folded as the window folds it: the pattern under the puck stays, so the like taken there stays under it;
	// the like away from the puck is now nearest somewhere else, and that is where its mark goes.
	const pat::HandSlot turned = pat::foldRotate(hand, q, 0.9f);
	pat::PatternSettings after = q;
	after.rotate = 0.9f;
	m = pat::markOn(after, turned.residue, here);
	CHECK(m.x == doctest::Approx(0.2).epsilon(1e-4));
	CHECK(m.y == doctest::Approx(-0.15).epsilon(1e-4));
	const pat::PlaneMark moved = pat::markOn(after, turned.residue, there);
	REQUIRE(moved.solved);
	CHECK(std::hypot(moved.x - 0.6, moved.y - 0.1) > 0.05);
	// AND IT IS THE NEAREST PLACE: decoding the plane at the mark is at least as close to the point as at the stale (0.6, 0.1).
	const auto missAt = [&](double x, double y) {
		pat::PatternSettings c = after;
		c.placeX = static_cast<float>(x);
		c.placeY = static_cast<float>(y);
		const auto p = pointAt(c, turned);
		double sum = 0.0;
		for (size_t i = 0; i < p.size(); ++i) {
			sum += (static_cast<double>(p[i]) - there[i]) * (static_cast<double>(p[i]) - there[i]);
		}
		return sum;
	};
	CHECK(missAt(moved.x, moved.y) < missAt(0.6, 0.1));
	CHECK(gn::kRowCount * 2 == here.size());
}

TEST_CASE("pattern text: Glide Time reads in milliseconds, the 303's 22 ms named TB, and parses back") {
	RowView rv;
	rv.row = pat::rowOf(pat::Settings {});
	const auto view = rv.view();
	const size_t c = pat::at(1, pat::Field::GlideTime);
	char out[64];
	REQUIRE(pat::kTextOps.format(c, pat::kGlideTimeDef, view, out, sizeof out));
	CHECK(std::string(out) == "22 ms (TB)");
	REQUIRE(pat::kTextOps.format(c, std::log2(0.1), view, out, sizeof out));
	CHECK(std::string(out) == "100 ms");
	REQUIRE(pat::kTextOps.format(c, pat::kGlideTimeMin, view, out, sizeof out));
	CHECK(std::string(out) == "2.0 ms");
	double v = 0.0;
	REQUIRE(pat::kTextOps.parse(c, "22 ms (TB)", view, &v));
	CHECK(v == doctest::Approx(pat::kGlideTimeDef));
	REQUIRE(pat::kTextOps.parse(c, "tb", view, &v));
	CHECK(v == pat::kGlideTimeDef);
	REQUIRE(pat::kTextOps.parse(c, "0.1 s", view, &v));
	CHECK(v == doctest::Approx(std::log2(0.1)));
	REQUIRE(pat::kTextOps.parse(c, "9000", view, &v));
	CHECK(v == pat::kGlideTimeMax);
	CHECK_FALSE(pat::kTextOps.parse(c, "slow", view, &v));
	// HONESTY: the control is gated to TB-Glide alone.
	CHECK(pat::kParamDescs[c].gateControl == static_cast<int16_t>(pat::at(1, pat::Field::Mode)));
	CHECK(pat::kParamDescs[c].gateMask == pat::modeBit(genes::Mode::TbGlide));
}

namespace {

/// A SLOT'S LIKES AT REAL PLACES: each like's full point decoded where it stands (its fingerprint is not read by the warp).
feathers::mod::arp::likes::SlotLikes likesAt(uint32_t seed, size_t count) {
	namespace lk = feathers::mod::arp::likes;
	lk::SlotLikes slot;
	std::mt19937 rng(seed);
	std::uniform_real_distribution<double> u(-3.0, 3.0);
	pat::PatternSettings q;
	q.zone = 300;
	for (size_t i = 0; i < count; ++i) {
		q.placeX = static_cast<float>(u(rng));
		q.placeY = static_cast<float>(u(rng));
		slot.like[i].placeX = q.placeX;
		slot.like[i].placeY = q.placeY;
		slot.like[i].point = pat::pointOf(pat::PatternDevice::placeOf(q, feathers::SliceResidue<genes::kRowCount> {}));
	}
	slot.count = count;
	return slot;
}

std::vector<float> likesWords(const feathers::mod::arp::likes::AllLikes& all) {
	std::vector<float> words(feathers::mod::arp::likes::kBlockWords);
	feathers::mod::arp::likes::write(all, words.data());
	return words;
}

/// THIS THREAD'S CPU time in ms (CLOCK_THREAD_CPUTIME_ID): what a call COSTS the caller, whatever the host is doing. A wall-clock bound
/// on a call that must not fit measures the scheduler at load 20-50 (backlog PL); the fit it must not run is SECONDS of CPU.
double threadCpuMs() {
	timespec t {};
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
	return static_cast<double>(t.tv_sec) * 1e3 + static_cast<double>(t.tv_nsec) * 1e-6;
}

double msSince(std::chrono::steady_clock::time_point t0) {
	return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

} // namespace

TEST_CASE("likes warp: a write returns at once and the pattern plays plain; the background fit, once published, equals the synchronous fit bit for bit and the pattern plays it") {
	namespace lk = feathers::mod::arp::likes;
	Drive d;
	d.s.likesWarp = true;
	d.s.patterns[0].zone = 300;
	d.apply();
	lk::AllLikes all {};
	all[0] = likesAt(3, 6);
	all[2] = likesAt(4, 3);
	const std::vector<float> words = likesWords(all);
	const double t0 = threadCpuMs();
	REQUIRE(pat::applyBlock(&d.d, pat::kLikesBlock, words.data(), words.size()));
	const double writeMs = threadCpuMs() - t0;
	MESSAGE("the likes' write cost this thread " << writeMs << " ms of CPU");
	CHECK(writeMs < 5.0);
	// UNTIL A FIT IS PUBLISHED, PLAIN: a step plays at once, built with no warp.
	CHECK(d.d.publishedSerialForTest() == 0);
	d.note(Kind::On, 48);
	d.runTo(2 * kStep);
	CHECK(d.d.builtWarpSerialForTest(0) == 0);
	d.d.waitForWarpForTest();
	REQUIRE(d.d.publishedSerialForTest() == d.d.warpRequestForTest());
	CHECK(d.d.warpPublishesForTest() == 1);
	// BIT FOR BIT, slot by slot; a slot with no likes has none.
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		CAPTURE(p);
		const genes::Warp* published = d.d.publishedWarpForTest(p);
		if (all[p].count == 0) {
			CHECK(published == nullptr);
			continue;
		}
		REQUIRE(published != nullptr);
		auto sync = std::make_unique<genes::Warp>();
		REQUIRE(pat::fitLikesWarp(all[p], *sync));
		CHECK(genes::sameWarp(*published, *sync));
	}
	// AND THE PATTERN PLAYS IT: the next build reads the published warp, by its serial, and the cycle is the warped decode.
	d.runTo(4 * kStep);
	CHECK(d.d.builtWarpSerialForTest(0) == d.d.publishedSerialForTest());
	pat::Settings s = d.d.settingsForTest();
	std::array<bool, genes::kPinCount> on {};
	on = genes::kPinOnByDefault; // the switches a fresh pattern has (Ratchet and Approach opt in)
	feathers::mod::arp::pattern::Recipe r = pat::recipeFor(s, 0, on);
	feathers::mod::arp::HeldChord held;
	held.press(48, 0, 1.0f, 1);
	feathers::mod::arp::pattern::PatternCycle warped;
	feathers::mod::arp::pattern::PatternCycle plain;
	feathers::mod::arp::pattern::build(pat::PatternDevice::placeOf(s.patterns[0], feathers::SliceResidue<genes::kRowCount> {}), r,
	                                   held, plain);
	r.warp = d.d.publishedWarpForTest(0);
	feathers::mod::arp::pattern::build(pat::PatternDevice::placeOf(s.patterns[0], feathers::SliceResidue<genes::kRowCount> {}), r,
	                                   held, warped);
	const auto& played = d.d.cycleForTest(0);
	bool differs = false;
	for (size_t k = 0; k < warped.length; ++k) {
		CAPTURE(k);
		CHECK(played.steps[k].kind == warped.steps[k].kind);
		CHECK(played.steps[k].keys[0] == warped.steps[k].keys[0]);
		differs = differs || plain.steps[k].kind != warped.steps[k].kind || plain.steps[k].keys[0] != warped.steps[k].keys[0];
	}
	CHECK(differs); // the second assertion: the warp is not the identity here
	// SWITCHED OFF, plain again.
	d.s.likesWarp = false;
	d.apply();
	d.runTo(6 * kStep);
	CHECK(d.d.builtWarpSerialForTest(0) == 0);
	releaseAndDrain(d, { 48 });
}

TEST_CASE("likes warp: a load of 64 likes does not block and plays plain at once; a teardown with the fit in flight stops it promptly") {
	namespace lk = feathers::mod::arp::likes;
	lk::AllLikes all {};
	for (size_t p = 0; p < pat::kPatterns; ++p) {
		all[p] = likesAt(20 + static_cast<uint32_t>(p), lk::kMaxLikes);
	}
	const std::vector<float> words = likesWords(all);
	auto d = std::make_unique<Drive>();
	d->s.likesWarp = true;
	d->apply();
	const double t0 = threadCpuMs();
	REQUIRE(pat::restoreBlock(&d->d, pat::kLikesBlock, words.data(), words.size()));
	const double loadMs = threadCpuMs() - t0;
	MESSAGE("the load cost this thread " << loadMs << " ms of CPU");
	CHECK(loadMs < 5.0);
	d->note(Kind::On, 48);
	d->runTo(2 * kStep);
	CHECK(d->d.publishedSerialForTest() == 0); // ~7 s of fitting: nothing yet
	CHECK(d->d.builtWarpSerialForTest(0) == 0);
	// THE FIT IS RUNNING, not merely queued, when the device goes: it has seconds of work and this gives the worker 50 ms.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	REQUIRE(d->d.publishedSerialForTest() == 0);
	const auto t1 = std::chrono::steady_clock::now();
	d.reset(); // THE TEARDOWN, the fit in flight
	const double teardownMs = msSince(t1);
	MESSAGE("teardown with the fit in flight took " << teardownMs << " ms");
	// WALL time, because a join waits without spending CPU. Load-independent by margin: an uncancelled fit is ~7 s unloaded and only
	// longer under load, so 3 s separates "stopped" from "ran to the end" at any load a 100 ms bound would have flaked at.
	CHECK(teardownMs < 3000.0);
}

TEST_CASE("likes warp: a fit superseded by a newer like never publishes — only the newest request's fit is ever seen") {
	namespace lk = feathers::mod::arp::likes;
	Drive d;
	lk::AllLikes a {};
	a[0] = likesAt(31, lk::kMaxLikes); // ~1.7 s of fitting
	lk::AllLikes b {};
	b[0] = likesAt(32, 2);
	const std::vector<float> wa = likesWords(a);
	const std::vector<float> wb = likesWords(b);
	REQUIRE(pat::applyBlock(&d.d, pat::kLikesBlock, wa.data(), wa.size()));
	const uint64_t first = d.d.warpRequestForTest();
	REQUIRE(pat::applyBlock(&d.d, pat::kLikesBlock, wb.data(), wb.size()));
	const uint64_t second = d.d.warpRequestForTest();
	REQUIRE(second == first + 1);
	d.d.waitForWarpForTest();
	CHECK(d.d.warpPublishesForTest() == 1);
	CHECK(d.d.publishedSerialForTest() == second);
	auto sync = std::make_unique<genes::Warp>();
	REQUIRE(pat::fitLikesWarp(b[0], *sync));
	REQUIRE(d.d.publishedWarpForTest(0) != nullptr);
	CHECK(genes::sameWarp(*d.d.publishedWarpForTest(0), *sync));
}

namespace {

namespace dpn = feathers::mod::arp::pattern;

/// A DRUM CYCLE AT A PLACE, by the builder (gate only: a held key, any key).
dpn::PatternCycle drumAt(double x, double y, size_t length, const genes::Pins& pins, const dpn::DrumLanes& lanes = {},
                         int heldKey = 60) {
	pat::PatternSettings q;
	q.mode = genes::Mode::Drum;
	q.zone = 210;
	q.placeX = static_cast<float>(x);
	q.placeY = static_cast<float>(y);
	dpn::Recipe r;
	r.mode = genes::Mode::Drum;
	r.length = length;
	r.pins = pins;
	r.drum = lanes;
	r.voice = false;
	feathers::mod::arp::HeldChord held;
	held.press(static_cast<int16_t>(heldKey), 0, 0.8f, 1);
	dpn::PatternCycle cycle;
	dpn::build(pat::PatternDevice::placeOf(q, feathers::SliceResidue<genes::kRowCount> {}), r, held, cycle);
	return cycle;
}

uint8_t laneBit(dpn::DrumLane l) {
	return static_cast<uint8_t>(1u << static_cast<unsigned>(l));
}

} // namespace

TEST_CASE("drum: the lane rules hold at every place — the kick keeps the downbeat, the snare the backbeat, an open hat is choked by the next closed hat") {
	std::mt19937 rng(41);
	std::uniform_real_distribution<double> u(-3.0, 3.0);
	genes::Pins sparse;
	sparse.amount[static_cast<size_t>(genes::Pin::Density)] = 0.0f; // the least density there is
	genes::Pins full;
	full.amount[static_cast<size_t>(genes::Pin::Density)] = 1.0f;
	full.amount[static_cast<size_t>(genes::Pin::Hold)] = 1.0f; // open hats wherever the gene allows
	int opens = 0;
	for (int i = 0; i < 200; ++i) {
		const double x = u(rng);
		const double y = u(rng);
		CAPTURE(x);
		CAPTURE(y);
		// AT THE LEAST DENSITY: the kick on every bar's downbeat and nowhere else; no snare on the backbeat.
		const dpn::PatternCycle lo = drumAt(x, y, 64, sparse);
		REQUIRE(lo.playable);
		for (size_t k = 0; k < 64; ++k) {
			const bool kick = (lo.steps[k].drumHits & laneBit(dpn::DrumLane::Kick)) != 0;
			CHECK(kick == (k % 16 == 0));
		}
		// AT FULL DENSITY: the snare on every backbeat.
		const dpn::PatternCycle hi = drumAt(x, y, 64, full);
		for (size_t k = 0; k < 64; ++k) {
			if (k % 16 == 4 || k % 16 == 12) {
				CHECK((hi.steps[k].drumHits & laneBit(dpn::DrumLane::Snare)) != 0);
			}
			if (k % 16 == 0) {
				CHECK((hi.steps[k].drumHits & laneBit(dpn::DrumLane::Kick)) != 0);
			}
			// THE CHOKE, restated: an open hat runs 0.5 + 3·Hold steps or to the next closed hat, whichever is first.
			if ((hi.steps[k].drumHits & laneBit(dpn::DrumLane::OpenHat)) != 0) {
				++opens;
				float want = 0.5f + 3.0f * 1.0f;
				for (size_t d = 1; d <= 64; ++d) {
					if ((hi.steps[(k + d) % 64].drumHits & laneBit(dpn::DrumLane::ClosedHat)) != 0) {
						want = std::min(want, static_cast<float>(d));
						break;
					}
				}
				CHECK(hi.steps[k].drumLength[static_cast<size_t>(dpn::DrumLane::OpenHat)] == want);
				// AND NEVER BESIDE A CLOSED HAT ON ITS OWN STEP.
				CHECK((hi.steps[k].drumHits & laneBit(dpn::DrumLane::ClosedHat)) == 0);
			}
		}
	}
	CHECK(opens > 10); // the choke was exercised
}

TEST_CASE("drum: deterministic and gate only — the same place gives the same beat, whatever key is held") {
	genes::Pins pins;
	const dpn::PatternCycle a = drumAt(0.7, -1.2, 32, pins, {}, 48);
	const dpn::PatternCycle b = drumAt(0.7, -1.2, 32, pins, {}, 48);
	const dpn::PatternCycle c = drumAt(0.7, -1.2, 32, pins, {}, 84);
	for (size_t k = 0; k < 32; ++k) {
		CAPTURE(k);
		CHECK(a.steps[k].drumHits == b.steps[k].drumHits);
		CHECK(a.steps[k].drumHits == c.steps[k].drumHits); // the key held is a gate, not a pitch
		CHECK(a.steps[k].drumAccent == c.steps[k].drumAccent);
		CHECK(a.steps[k].drumVelocity == c.steps[k].drumVelocity);
	}
	// NOTHING HELD, NOTHING PLAYS.
	pat::PatternSettings q;
	q.mode = genes::Mode::Drum;
	dpn::Recipe r;
	r.mode = genes::Mode::Drum;
	feathers::mod::arp::HeldChord none;
	dpn::PatternCycle silent;
	dpn::build(pat::PatternDevice::placeOf(q, feathers::SliceResidue<genes::kRowCount> {}), r, none, silent);
	CHECK_FALSE(silent.playable);
}

TEST_CASE("drum: a fill closes every fourth bar — at full Answer the last quarter of bar 4 is toms, and no hats") {
	genes::Pins pins;
	pins.amount[static_cast<size_t>(genes::Pin::Answer)] = 1.0f;
	const dpn::PatternCycle c = drumAt(-0.4, 0.9, 64, pins);
	const uint8_t hats = laneBit(dpn::DrumLane::ClosedHat) | laneBit(dpn::DrumLane::OpenHat) | laneBit(dpn::DrumLane::Ride);
	const uint8_t toms = laneBit(dpn::DrumLane::LowTom) | laneBit(dpn::DrumLane::HighTom);
	for (size_t k = 48 + 12; k < 64; ++k) {
		CAPTURE(k);
		CHECK((c.steps[k].drumHits & hats) == 0);
		CHECK((c.steps[k].drumHits & toms) != 0);
	}
	// AND BAR 1'S LAST QUARTER IS NO FILL (not a phrase's end): its hats are where its hats are.
	bool hatsInBar1 = false;
	for (size_t k = 12; k < 16; ++k) {
		hatsInBar1 = hatsInBar1 || (c.steps[k].drumHits & hats) != 0;
	}
	CHECK(hatsInBar1);
}

TEST_CASE("drum: the device sounds every hit on its lane's note, never a lane switched off, never one lane twice on a step, and the lane fields reach a fresh instance") {
	Drive d;
	d.s.patterns[0].mode = genes::Mode::Drum;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Octave)] = 0.0f; // no rolls: one stroke a hit
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 1.0f;
	// THE LANE FIELDS: an acoustic kick, the clap off.
	d.s.patterns[0].drum.note[static_cast<size_t>(dpn::DrumLane::Kick)] = 35;
	d.s.patterns[0].drum.on &= static_cast<uint8_t>(~laneBit(dpn::DrumLane::Clap));
	d.apply();
	const std::array<dpn::DrumLanes, 1> lanes { d.s.patterns[0].drum };
	d.note(Kind::On, 72);
	d.runTo(33 * kStep);
	std::set<int> allowed;
	for (size_t l = 0; l < dpn::kDrumLanes; ++l) {
		if (lanes[0].laneOn(l)) {
			allowed.insert(lanes[0].note[l]);
		}
	}
	std::map<std::pair<int64_t, int>, int> perStepKey;
	int ons = 0;
	for (const Logged& l : d.log) {
		if (l.kind != Kind::On) {
			continue;
		}
		CAPTURE(l.key);
		CHECK(allowed.count(l.key) == 1); // every note in the lane map, the clap's never
		CHECK(l.key != 72);               // the held key's pitch is never played
		++perStepKey[{ l.sample / kStep, l.key }];
		++ons;
	}
	for (const auto& [at, n] : perStepKey) {
		CHECK(n == 1);
	}
	CHECK(ons > 20);
	// THE KICK AT EVERY BAR'S DOWNBEAT, on its own note.
	CHECK(perStepKey.count({ 0, 35 }) == 1);
	CHECK(perStepKey.count({ 16, 35 }) == 1);
	// THE LANE FIELDS INTO A FRESH INSTANCE: the row a save keeps, read back field by field.
	const pat::Row row = pat::rowOf(d.s);
	pat::PatternDevice fresh;
	fresh.setFields(row.data(), row.size());
	CHECK(fresh.settingsForTest().patterns[0].drum == lanes[0]);
	releaseAndDrain(d, { 72 });
	checkBalanced(d.log);
}

TEST_CASE("edits: an edit changes exactly that step's emitted note, a step the terrain no longer decodes the same plays the terrain's, and a live overlay saves into a fresh instance") {
	// THE PLAIN RUN: what the terrain plays, step by step.
	const auto onsByStep = [](const Drive& d) {
		std::map<int64_t, std::vector<int>> out;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::On) {
				out[l.sample / kStep].push_back(l.key);
			}
		}
		return out;
	};
	Drive plain;
	plain.note(Kind::On, 60);
	plain.runTo(16 * kStep);
	const auto base = onsByStep(plain);
	const dpn::PatternCycle terrain = plain.d.cycleForTest(0);
	size_t k = 0;
	while (k < 16 && terrain.steps[k].kind != dpn::PatternStep::Kind::Note) {
		++k;
	}
	REQUIRE(k < 16);
	// THE EDIT: step k sounds two semitones up, signed with the terrain's step it was made over.
	pat::EditState edits {};
	edits[0].mode = genes::Mode::Melody;
	dpn::StepEdit& e = edits[0].steps[k];
	e = dpn::editOf(terrain.steps[k], false);
	dpn::setEditKey(e, terrain, terrain.keyAt(k, 0) + 2);
	std::vector<float> words(pat::kEditsWords);
	pat::writeEditState(edits, words.data());
	Drive d;
	REQUIRE(pat::applyBlock(&d.d, pat::kEditsBlock, words.data(), words.size()));
	d.note(Kind::On, 60);
	d.runTo(16 * kStep);
	const auto got = onsByStep(d);
	for (int64_t g = 0; g < 16; ++g) {
		CAPTURE(g);
		const auto b = base.count(g) ? base.at(g) : std::vector<int> {};
		const auto o = got.count(g) ? got.at(g) : std::vector<int> {};
		if (g == static_cast<int64_t>(k)) {
			REQUIRE(o.size() == 1);
			CHECK(o[0] == terrain.steps[k].keys[0] + 2);
		}
		else {
			CHECK(o == b); // every other step the terrain's
		}
	}
	// A STEP THE TERRAIN DECODES DIFFERENTLY NOW (the signature no longer matches) plays the terrain's own.
	pat::EditState stale = edits;
	stale[0].steps[k].origDegree = static_cast<int16_t>(terrain.steps[k].degree + 1);
	pat::writeEditState(stale, words.data());
	Drive s;
	REQUIRE(pat::applyBlock(&s.d, pat::kEditsBlock, words.data(), words.size()));
	s.note(Kind::On, 60);
	s.runTo(16 * kStep);
	CHECK(onsByStep(s) == base);
	// INTO A FRESH INSTANCE: the overlay as it was.
	pat::writeEditState(edits, words.data());
	std::vector<float> saved(pat::kEditsWords);
	REQUIRE(pat::readBlock(&d.d, pat::kEditsBlock, saved.data(), saved.size()) > 0);
	CHECK(pat::editStateOf(saved.data(), saved.size()) == edits);
	Drive fresh;
	REQUIRE(pat::restoreBlock(&fresh.d, pat::kEditsBlock, saved.data(), saved.size()));
	fresh.note(Kind::On, 60);
	fresh.runTo(16 * kStep);
	CHECK(onsByStep(fresh) == got);
	releaseAndDrain(d, { 60 });
	releaseAndDrain(plain, { 60 });
}

TEST_CASE("pattern: Only w/ Transport is On by default — with the transport stopped, a held key plays nothing") {
	// Only w/ Transport defaults to on.
	CHECK(pat::Settings {}.run == feathers::mod::arp::RunMode::Transport);
	CHECK(pat::kParamDescs[pat::at(pat::Global::Run)].def == 1.0); // Transport
	Drive d;
	d.s = pat::Settings {};
	d.apply();
	d.playing = false;
	d.note(Kind::On, 60);
	d.runTo(4 * kStep);
	int ons = 0;
	for (const Logged& l : d.log) {
		ons += l.kind == Kind::On ? 1 : 0;
	}
	CHECK(ons == 0);
}

TEST_CASE("pattern: the playhead is the step the device just played — every step, rests too, across a loop wrap and a pattern switch; idle when nothing plays") {
	// The pattern view has a playhead. BY VALUE: after the call that plays step g, the published
	// playhead is (playing pattern << 6) | (g mod Length).
	const auto playheadNow = [](const Drive& d) {
		uint32_t ordinal = 0;
		float phase = 0.0f;
		d.d.playhead(&ordinal, &phase);
		return ordinal;
	};
	Drive d;
	d.s.patterns[0].length = 5; // a loop that does not divide the bar, so the wrap is not at a bar line
	d.s.patterns[1].length = 3;
	d.s.patterns[1].enable = true;
	d.apply();
	CHECK(playheadNow(d) == 0xFFFFFFFFu); // nothing held: idle
	d.note(Kind::On, 60);
	for (int64_t g = 0; g < 12; ++g) {
		d.runTo(g * kStep + 100);
		CAPTURE(g);
		CHECK(playheadNow(d) == ((0u << pat::PatternDevice::kPlayheadStepBits) | static_cast<uint32_t>(g % 5)));
	}
	// THE SWITCH: the selector to pattern 2 lands at the next step; the playhead names it and its own step.
	d.s.selector = 0.75f;
	d.apply();
	REQUIRE(pat::selected(d.s) == 1);
	for (int64_t g = 13; g < 20; ++g) {
		d.runTo(g * kStep + 100);
		CAPTURE(g);
		CHECK((playheadNow(d) >> pat::PatternDevice::kPlayheadStepBits) == 1u);
		CHECK((playheadNow(d) & 63u) == static_cast<uint32_t>(g % 3));
	}
	// LET GO: idle.
	d.note(Kind::Off, 60);
	d.runTo(22 * kStep);
	CHECK(playheadNow(d) == 0xFFFFFFFFu);
	releaseAndDrain(d, {});
}

//------------------------------------------------------------------------------------------------//
// THRU AND TRIG: an option to let incoming notes through, and incoming notes as triggers          //
//------------------------------------------------------------------------------------------------//

namespace {

/// A DRUM PATTERN WITH THE KICK AND SNARE LANES OFF — a player plays those, the pattern plays the rest.
void drumWithoutKickAndSnare(Drive& d, pat::Thru thru, uint8_t trig) {
	d.s.patterns[0].mode = genes::Mode::Drum;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Octave)] = 0.0f; // no rolls
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 1.0f;
	d.s.patterns[0].drum.on &= static_cast<uint8_t>(~(laneBit(dpn::DrumLane::Kick) | laneBit(dpn::DrumLane::Snare)));
	d.s.patterns[0].thru = thru;
	d.s.patterns[0].trig = trig;
	d.apply();
}

int onsOf(const std::vector<Logged>& log, int key) {
	return static_cast<int>(std::count_if(log.begin(), log.end(), [&](const Logged& l) { return l.kind == Kind::On && l.key == key; }));
}

} // namespace

TEST_CASE("thru: Drum's Auto passes the kit's notes untouched at their samples while the pattern plays the other lanes from the transport; Off swallows them as before") {
	REQUIRE(pat::thruOn([] {
		pat::PatternSettings q;
		q.mode = genes::Mode::Drum;
		return q;
	}()));
	Drive d;
	drumWithoutKickAndSnare(d, pat::Thru::Auto, 0);
	const int kick = d.s.patterns[0].drum.note[static_cast<size_t>(dpn::DrumLane::Kick)];
	const int snare = d.s.patterns[0].drum.note[static_cast<size_t>(dpn::DrumLane::Snare)];
	// NO KEY HELD, TRANSPORT PLAYING: Auto's Trig is Transport, so the pattern plays.
	d.runTo(8 * kStep + 1234);
	d.note(Kind::On, kick); // off the grid on purpose: it passes on ITS sample
	const int64_t kickOn = d.now;
	d.runTo(10 * kStep + 77);
	d.note(Kind::On, snare);
	const int64_t snareOn = d.now;
	d.runTo(12 * kStep + 5);
	d.note(Kind::Off, kick);
	const int64_t kickOff = d.now;
	d.runTo(20 * kStep);
	d.note(Kind::Off, snare);
	d.runTo(24 * kStep);
	// THE KICK AND SNARE ARE THE PLAYER'S, EXACTLY: one on each, on its sample, and the kick's off on its.
	CHECK(onsOf(d.log, kick) == 1);
	CHECK(onsOf(d.log, snare) == 1);
	CHECK(d.onsAt(kickOn).size() >= 1);
	CHECK(std::any_of(d.log.begin(), d.log.end(), [&](const Logged& l) { return l.kind == Kind::On && l.key == kick && l.sample == kickOn; }));
	CHECK(std::any_of(d.log.begin(), d.log.end(), [&](const Logged& l) { return l.kind == Kind::On && l.key == snare && l.sample == snareOn; }));
	CHECK(std::any_of(d.log.begin(), d.log.end(), [&](const Logged& l) { return l.kind == Kind::Off && l.key == kick && l.sample == kickOff; }));
	// THE PATTERN'S OWN: the other lanes' notes, from the transport, before any key and after.
	int patternOns = 0;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On && l.key != kick && l.key != snare) {
			++patternOns;
			CHECK(l.sample % kStep == 0); // on the grid
		}
	}
	CHECK(patternOns > 10);
	// STOP: the pattern's notes end; nothing is left sounding.
	d.playing = false;
	d.runTo(d.now + 2 * kStep);
	checkBalanced(d.log);

	// THRU OFF: the old swallowing — the kick is a key that gates the pattern, never an output.
	Drive off;
	drumWithoutKickAndSnare(off, pat::Thru::Off, 0);
	off.runTo(4 * kStep);
	CHECK(off.log.empty()); // Gate (Auto without Thru): nothing held, nothing plays
	off.note(Kind::On, kick);
	off.runTo(12 * kStep);
	CHECK(onsOf(off.log, kick) == 0);
	CHECK(off.log.size() > 4); // the key gates the pattern on
	releaseAndDrain(off, { kick });
	checkBalanced(off.log);
}

TEST_CASE("thru: an incoming note on an ENABLED lane's note at that lane's sample — both sound, and every on has its off") {
	Drive d;
	d.s.patterns[0].mode = genes::Mode::Drum;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Octave)] = 0.0f;
	d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 1.0f;
	d.apply();
	const int kick = d.s.patterns[0].drum.note[static_cast<size_t>(dpn::DrumLane::Kick)];
	d.runTo(16 * kStep); // the next bar's downbeat: the kick lane plays here
	d.note(Kind::On, kick);
	d.runTo(16 * kStep + 1);
	int at = 0;
	for (const Logged& l : d.onsAt(16 * kStep)) {
		at += l.key == kick ? 1 : 0;
	}
	CHECK(at == 2); // the player's and the pattern's
	d.note(Kind::Off, kick);
	d.runTo(24 * kStep);
	d.playing = false;
	d.runTo(d.now + 2 * kStep);
	int ons = 0;
	int offs = 0;
	for (const Logged& l : d.log) {
		if (l.key == kick) {
			ons += l.kind == Kind::On ? 1 : 0;
			offs += l.kind == Kind::Off ? 1 : 0;
		}
	}
	CHECK(ons == offs); // nothing sticks
	CHECK(ons >= 2);
}

TEST_CASE("trig: Restart — a trigger (a non-kit note) restarts the cycle at step 0 on its sample and is not output; a kit note passes and restarts nothing; the same restart sample plays the same notes") {
	const auto run = [](int64_t restartAt, bool kitNoteToo) {
		Drive d;
		drumWithoutKickAndSnare(d, pat::Thru::On, 3); // Restart
		d.runTo(restartAt);
		d.note(Kind::On, 72);
		const int64_t at = d.now;
		if (kitNoteToo) {
			d.runTo(at + 3 * kStep + 500);
			d.note(Kind::On, 38); // the snare, the player's: passes, restarts nothing
		}
		d.runTo(at + 32 * kStep);
		d.note(Kind::Off, 72);
		d.runTo(at + 34 * kStep);
		return std::pair { d.log, at };
	};
	auto [a, at] = run(5 * kStep + 3001, false);
	CHECK(onsOf(a, 72) == 0); // a trigger is swallowed
	// AFTER THE RESTART, every pattern on is on the restart's grid: (sample − restart) a whole number of steps.
	int after = 0;
	for (const Logged& l : a) {
		if (l.kind == Kind::On && l.sample >= at) {
			CHECK((l.sample - at) % kStep == 0);
			++after;
		}
	}
	CHECK(after > 10);
	CHECK(std::any_of(a.begin(), a.end(), [&](const Logged& l) { return l.kind == Kind::On && l.sample == at; })); // step 0 on its sample
	// DETERMINISM: a restart at another sample plays the same notes, the same distance after it.
	auto [b, bt] = run(9 * kStep + 17, false);
	std::vector<std::pair<int64_t, int>> ra;
	std::vector<std::pair<int64_t, int>> rb;
	for (const Logged& l : a) {
		if (l.kind == Kind::On && l.sample >= at && l.sample < at + 32 * kStep) {
			ra.emplace_back(l.sample - at, l.key);
		}
	}
	for (const Logged& l : b) {
		if (l.kind == Kind::On && l.sample >= bt && l.sample < bt + 32 * kStep) {
			rb.emplace_back(l.sample - bt, l.key);
		}
	}
	std::sort(ra.begin(), ra.end());
	std::sort(rb.begin(), rb.end());
	CHECK(ra == rb);
	// A KIT NOTE PASSES AND RESTARTS NOTHING: the pattern stays on the trigger's grid.
	auto [c, ct] = run(5 * kStep + 3001, true);
	CHECK(onsOf(c, 38) == 1);
	for (const Logged& l : c) {
		if (l.kind == Kind::On && l.sample >= ct && l.key != 38) {
			CHECK((l.sample - ct) % kStep == 0);
		}
	}
}

TEST_CASE("trig: Gate plays only while a trigger is held; Transport ignores triggers — the same notes with one or without") {
	Drive g;
	drumWithoutKickAndSnare(g, pat::Thru::On, 2); // Gate
	g.runTo(8 * kStep);
	CHECK(g.log.empty());
	g.note(Kind::On, 72);
	g.runTo(24 * kStep);
	const size_t whileHeld = g.log.size();
	CHECK(whileHeld > 4);
	CHECK(onsOf(g.log, 72) == 0);
	g.note(Kind::Off, 72);
	g.runTo(25 * kStep);
	const size_t settled = g.log.size();
	g.runTo(40 * kStep);
	CHECK(g.log.size() == settled); // released: nothing more
	checkBalanced(g.log);

	const auto transport = [](bool trigger) {
		Drive d;
		drumWithoutKickAndSnare(d, pat::Thru::On, 1); // Transport
		d.runTo(6 * kStep + 999);
		if (trigger) {
			d.note(Kind::On, 72);
		}
		d.runTo(30 * kStep);
		std::vector<std::pair<int64_t, int>> ons;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::On) {
				ons.emplace_back(l.sample, l.key);
			}
		}
		return ons;
	};
	CHECK(transport(true) == transport(false));
}

TEST_CASE("thru and trig text: Auto says what it resolves to — 'Auto (On)' in Drum, 'Auto (Off)' and 'Auto (Gate)' in Melody, 'Auto (Transport)' with Thru — and reads back") {
	pat::Settings s;
	s.patterns[0].mode = genes::Mode::Drum;
	RowView rv;
	rv.row = pat::rowOf(s);
	const size_t thru = pat::at(0, pat::Field::Thru);
	const size_t trig = pat::at(0, pat::Field::Trig);
	CHECK(format(rv, thru, 0.0) == "Auto (On)");
	CHECK(format(rv, trig, 0.0) == "Auto (Transport)");
	CHECK(format(rv, trig, 3.0) == "Restart");
	s.patterns[0].mode = genes::Mode::Melody;
	rv.row = pat::rowOf(s);
	CHECK(format(rv, thru, 0.0) == "Auto (Off)");
	CHECK(format(rv, trig, 0.0) == "Auto (Gate)");
	s.patterns[0].thru = pat::Thru::On;
	rv.row = pat::rowOf(s);
	CHECK(format(rv, trig, 0.0) == "Auto (Transport)");
	double v = -1.0;
	REQUIRE(pat::kTextOps.parse(thru, "Auto (On)", rv.view(), &v));
	CHECK(v == 0.0);
	REQUIRE(pat::kTextOps.parse(trig, "restart", rv.view(), &v));
	CHECK(v == 3.0);
}

TEST_CASE("progression: the device strikes each chord of the cycle and holds it through its ties; a locate mid-progression plays what the run from the start played there; Degree is the chord's; the Mode reaches a fresh instance") {
	// Progression mode. The cycle is restated through the builder's own road (the device's
	// place and recipe over the held key) and the device's notes are checked against it, step by step.
	namespace pn = feathers::mod::arp::pattern;
	const auto setUp = [](Drive& d) {
		d.s.patterns[0].mode = genes::Mode::Progression;
		d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Accent)] = 0.0f; // no pushes: every chord on its change
		d.apply();
	};
	Drive a;
	setUp(a);
	std::array<bool, genes::kPinCount> on {};
	on = genes::kPinOnByDefault; // the switches a fresh pattern has (Ratchet and Approach opt in)
	feathers::mod::arp::HeldChord held;
	held.press(60, 0, 0.9f, 1);
	pn::PatternCycle cycle;
	pn::build(pat::PatternDevice::placeOf(a.s.patterns[0], feathers::SliceResidue<genes::kRowCount> {}), pat::recipeFor(a.s, 0, on),
	          held, cycle);
	REQUIRE(cycle.length >= 8);
	a.note(Kind::On, 60);
	const auto steps = static_cast<int64_t>(2 * cycle.length);
	a.runTo(steps * kStep);
	int strikes = 0;
	for (int64_t g = 0; g < steps; ++g) {
		const pn::PatternStep& st = cycle.steps[static_cast<size_t>(g) % cycle.length];
		std::multiset<int> got;
		for (const Logged& l : a.onsAt(g * kStep)) {
			got.insert(l.key);
		}
		std::multiset<int> want;
		if (st.kind == pn::PatternStep::Kind::Note) {
			for (int i = 0; i < st.count; ++i) {
				if (g == 0 || ((st.holdMask >> i) & 1u) == 0u) {
					want.insert(st.keys[static_cast<size_t>(i)]);
				}
			}
			++strikes;
		}
		CAPTURE(g);
		CHECK(got == want); // a tie strikes nothing: the chord before sounds on
	}
	CHECK(strikes >= 2);
	// DEGREE: the last chord struck, as a share of the scale.
	const pn::PatternStep& last = cycle.steps[static_cast<size_t>(steps - 1) % cycle.length];
	CHECK(a.d.outputAt(static_cast<size_t>(pat::Output::Degree)) == doctest::Approx(static_cast<float>(((last.degree % 7) + 7) % 7) / 6.0f));
	// A LOCATE to a step past the first strike of the second chord: from the next strike on, the same notes.
	size_t second = 1;
	while (second < cycle.length && !cycle.steps[second].chordStart) {
		++second;
	}
	REQUIRE(second < cycle.length);
	const auto from = static_cast<int64_t>(second) + 1;
	Drive b;
	setUp(b);
	b.locate(static_cast<double>(from) * 0.25);
	b.note(Kind::On, 60);
	b.runTo((steps - from) * kStep);
	int compared = 0;
	bool started = false;
	for (int64_t g = from; g < steps; ++g) {
		const pn::PatternStep& st = cycle.steps[static_cast<size_t>(g) % cycle.length];
		if (!started) {
			started = st.chordStart; // the first strike after the locate has nothing sounding to hold over
			continue;
		}
		std::multiset<int> x;
		std::multiset<int> y;
		for (const Logged& l : a.onsAt(g * kStep)) {
			x.insert(l.key);
		}
		for (const Logged& l : b.onsAt((g - from) * kStep)) {
			y.insert(l.key);
		}
		CAPTURE(g);
		CHECK(x == y);
		compared += x.empty() ? 0 : 1;
	}
	CHECK(compared >= 1);
	// THE MODE INTO A FRESH INSTANCE, by its row.
	const pat::Row row = pat::rowOf(a.s);
	pat::PatternDevice fresh;
	fresh.setFields(row.data(), row.size());
	CHECK(fresh.settingsForTest().patterns[0].mode == genes::Mode::Progression);
	releaseAndDrain(a, { 60 });
	checkBalanced(a.log);
	releaseAndDrain(b, { 60 });
	checkBalanced(b.log);
}

TEST_CASE("pattern text: a Progression's selector text reads its chords in numerals, as the lane decodes them — '1 · Major, I → V → vi → IV'") {
	namespace pn = feathers::mod::arp::pattern;
	pat::Settings s;
	s.patterns[0].mode = genes::Mode::Progression;
	RowView rv;
	rv.row = pat::rowOf(s);
	// THE LANE'S DECODE: over a held C4, every pin on, no residue.
	std::array<bool, genes::kPinCount> on {};
	on = genes::kPinOnByDefault; // the switches a fresh pattern has (Ratchet and Approach opt in)
	feathers::mod::arp::HeldChord held;
	held.press(60, 0, 0.8f, 1);
	pn::PatternCycle cycle;
	pn::build(pat::PatternDevice::placeOf(s.patterns[0], feathers::SliceResidue<genes::kRowCount> {}), pat::recipeFor(s, 0, on),
	          held, cycle);
	std::string want = "1 · Major, "; // Transpose: the key names the root, so the text does not
	int shown = 0;
	for (size_t k = 0; k < cycle.length; ++k) {
		if (!cycle.steps[k].chordStart) {
			continue;
		}
		if (shown == 6) {
			want += " → ...";
			break;
		}
		char n[16];
		pn::numeralOf(cycle.steps[k], n, sizeof n);
		want += (shown == 0 ? "" : " → ") + std::string(n);
		++shown;
	}
	REQUIRE(shown >= 2);
	CHECK(format(rv, pat::at(pat::Global::Selector), 0.1) == want);
}

TEST_CASE("approach: a SCRUB plays the same pickups — a device located mid-song hands out, step for step, the notes a device "
          "that played from the top hands out there (Chords and Progression, Approach on at the most)") {
	// Determinism from the song position (design §6): the cycle is the place's alone, and the emitter plays the slot the
	// song stands on. 120 bpm, a 16th a step: 6000 samples.
	constexpr int64_t kStep = 6000;
	for (const genes::Mode mode : { genes::Mode::Chords, genes::Mode::Progression }) {
		CAPTURE(static_cast<int>(mode));
		const auto drive = [&](Drive& d) {
			d.s.patterns[0].mode = mode;
			d.s.patterns[0].pins[static_cast<size_t>(genes::Pin::Approach)] = 1.0f;
			d.apply();
			pat::HandState hand {};
			hand[0].pinOn[static_cast<size_t>(genes::Pin::Approach)] = true;
			std::array<float, pat::kHandWords> words {};
			pat::writeHandState(hand, words.data());
			REQUIRE(d.d.writeHand(words.data(), words.size()));
		};
		Drive top;
		drive(top);
		top.note(Kind::On, 60);
		top.runTo(64 * kStep);
		Drive mid;
		drive(mid);
		mid.locate(32 * 0.25); // the song at step 32
		mid.note(Kind::On, 60);
		mid.runTo(32 * kStep);
		std::map<int64_t, std::vector<int>> a;
		std::map<int64_t, std::vector<int>> b;
		for (const Logged& l : top.log) {
			if (l.kind == Kind::On && l.sample >= 34 * kStep) {
				a[l.sample].push_back(l.key);
			}
		}
		for (const Logged& l : mid.log) {
			if (l.kind == Kind::On && l.sample + 32 * kStep >= 34 * kStep) {
				b[l.sample + 32 * kStep].push_back(l.key);
			}
		}
		CHECK(!a.empty());
		CHECK(a == b);
	}
}

TEST_CASE("pattern device: a row handed again with the same bits is not decoded again, and any changed bit is") {
	// THE IDLE-OVERHEAD PASS (2026-11-05): the rack hands a Pattern its row on every resolve, about 85 times a 1024-frame
	// block on a measured real patch, and `settingsOf` decoded all 233 fields each time. `setFields` now decodes only a row whose
	// bits differ from the last one decoded. The settings are asserted against `settingsOf` itself, the independent road.
	pat::PatternDevice d;
	d.prepare(kRate);
	pat::Settings s;
	pat::Row row = pat::rowOf(s);
	d.setFields(row.data(), row.size());
	const uint64_t once = d.decodesForTest();
	CHECK(once == 1);
	for (int i = 0; i < 50; ++i) {
		d.setFields(row.data(), row.size());
	}
	CHECK(d.decodesForTest() == once); // AT REST: no decode
	CHECK(d.settingsForTest() == pat::settingsOf(row.data(), row.size()));
	// ONE FIELD MOVED: decoded, and the settings are the new row's.
	const size_t rate = pat::at(1, pat::Field::Rate);
	row[rate] = row[rate] + 0.25f;
	d.setFields(row.data(), row.size());
	CHECK(d.decodesForTest() == once + 1);
	CHECK(d.settingsForTest() == pat::settingsOf(row.data(), row.size()));
	CHECK_FALSE(d.settingsForTest() == pat::settingsOf(pat::rowOf(s).data(), row.size())); // NOT VACUOUS
	// THE SAME VALUE IN OTHER BITS IS A CHANGE (−0 for +0): compared as bits, never as numbers.
	const size_t nudge = pat::at(pat::Global::Nudge);
	REQUIRE(row[nudge] == 0.0f);
	row[nudge] = -0.0f;
	d.setFields(row.data(), row.size());
	CHECK(d.decodesForTest() == once + 2);
	// A SHORTER ROW IS A CHANGE: the fields past it read as their defaults.
	d.setFields(row.data(), row.size() - 1);
	CHECK(d.decodesForTest() == once + 3);
	CHECK(d.settingsForTest() == pat::settingsOf(row.data(), row.size() - 1));
}

//------------------------------------------------------------------------------------------------//
// TRANSPOSE: steps and octaves on the pattern                                                  //
//------------------------------------------------------------------------------------------------//

namespace {

/// **THE REFERENCE**, restated from the scale's own mask and nothing of the device's: the keys of `scale` spelled up from
/// `root` (every octave from 10 below to 10 above), a key's index among them (the one at or below it, and its offset),
/// moved `steps` along the list, then `octaves` × 12.
int referenceTranspose(size_t scale, int root, int key, int steps, int octaves) {
	const uint16_t mask = feathers::mod::arp::theory::kScales[scale].mask;
	std::vector<int> keys;
	for (int o = -10; o <= 10; ++o) {
		for (int pc = 0; pc < 12; ++pc) {
			if (((mask >> pc) & 1u) != 0u) {
				keys.push_back(root + 12 * o + pc);
			}
		}
	}
	size_t at = 0;
	while (at + 1 < keys.size() && keys[at + 1] <= key) {
		++at;
	}
	const int off = key - keys[at];
	const int moved = keys[static_cast<size_t>(static_cast<int>(at) + steps)] + off;
	return std::clamp(moved + 12 * octaves, 0, 127);
}

/// One run of 32 steps with C3 held, the settings as `tune` leaves them; every on and off.
std::vector<Logged> transposeRun(const std::function<void(pat::Settings&)>& tune, int steps, int octaves,
                                 const std::vector<float>* edits = nullptr, int held = 48) {
	Drive d;
	tune(d.s);
	d.s.transposeSteps = steps;
	d.s.transposeOctaves = octaves;
	d.apply();
	if (edits != nullptr) {
		REQUIRE(pat::applyBlock(&d.d, pat::kEditsBlock, edits->data(), edits->size()));
	}
	d.note(Kind::On, held);
	d.runTo(32 * kStep);
	d.note(Kind::Off, held);
	d.runTo(d.now + 8 * kStep);
	checkBalanced(d.log);
	std::vector<Logged> out;
	for (const Logged& l : d.log) {
		if (l.kind == Kind::On || l.kind == Kind::Off) {
			out.push_back(l);
		}
	}
	return out;
}

} // namespace

TEST_CASE("transpose: in every pitched Mode and scale shape, +N steps lands each note N degrees up its scale and an octave is exactly 12 — every event at its own sample") {
	const genes::Mode modes[] = { genes::Mode::Melody, genes::Mode::MelodyMono, genes::Mode::Chords, genes::Mode::TbMono,
		                          genes::Mode::TbGlide, genes::Mode::Progression };
	// A 7-NOTE, A 5-NOTE, A 6-NOTE AND AN 8-NOTE SCALE (Major, Minor Pentatonic, Whole Tone, Bebop Dominant).
	const size_t scales[] = { 0, 17, 38, 43 };
	const std::pair<int, int> moves[] = { { 2, 0 }, { -3, 0 }, { 9, 1 }, { 0, -2 }, { 14, 0 }, { -14, 2 } };
	size_t checked = 0;
	for (const genes::Mode mode : modes) {
		for (const size_t scale : scales) {
			const auto tune = [&](pat::Settings& s) {
				s.patterns[0].mode = mode;
				s.patterns[0].scale = scale;
				s.patterns[0].zone = 333;
				s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
				s.noteLow = 0; // THE ARITHMETIC ALONE: the note range (backlog QT) would fold the octaves taken down below its floor
			};
			const std::vector<Logged> plain = transposeRun(tune, 0, 0);
			REQUIRE(plain.size() > 8);
			for (const auto& [steps, octaves] : moves) {
				CAPTURE(static_cast<int>(mode));
				CAPTURE(scale);
				CAPTURE(steps);
				CAPTURE(octaves);
				const std::vector<Logged> moved = transposeRun(tune, steps, octaves);
				REQUIRE(moved.size() == plain.size());
				for (size_t i = 0; i < plain.size(); ++i) {
					CAPTURE(i);
					CHECK(moved[i].kind == plain[i].kind);
					CHECK(moved[i].sample == plain[i].sample);
					CHECK(moved[i].velocity == plain[i].velocity);
					// THE SCALE IS SPELLED FROM THE HELD C3 (Keys: Transpose takes the root from the lowest key held).
					CHECK(moved[i].key == referenceTranspose(scale, 48, plain[i].key, steps, octaves));
					if (steps == 0) {
						CHECK(moved[i].key == plain[i].key + 12 * octaves); // AN OCTAVE IS EXACTLY 12
					}
					++checked;
				}
			}
		}
	}
	CHECK(checked > 5000);
}

TEST_CASE("transpose: zero steps and zero octaves play exactly the events of no transpose, and a Drum is never moved") {
	const auto melody = [](pat::Settings& s) { s.patterns[0].zone = 777; };
	const std::vector<Logged> a = transposeRun(melody, 0, 0);
	Drive d; // THE DESCRIPTORS' DEFAULTS ARE ZERO, so a patch that never touched them is the build before them
	CHECK(pat::kParamDescs[pat::at(pat::Global::TransposeSteps)].def == 0.0);
	CHECK(pat::kParamDescs[pat::at(pat::Global::TransposeOctaves)].def == 0.0);
	CHECK(d.s.transposeSteps == 0);
	CHECK(pat::settingsOf(nullptr, 0).transposeOctaves == 0);
	// DRUM: its lanes are fixed note numbers — every event the same, transposed or not.
	const auto drum = [](pat::Settings& s) { s.patterns[0].mode = genes::Mode::Drum; };
	const std::vector<Logged> plain = transposeRun(drum, 0, 0);
	REQUIRE(plain.size() > 8);
	for (const auto& [steps, octaves] : { std::pair { 5, 0 }, std::pair { 0, 3 }, std::pair { -7, -2 } }) {
		const std::vector<Logged> moved = transposeRun(drum, steps, octaves);
		REQUIRE(moved.size() == plain.size());
		for (size_t i = 0; i < plain.size(); ++i) {
			CHECK(moved[i].key == plain[i].key);
			CHECK(moved[i].sample == plain[i].sample);
		}
	}
	// AND THE UI'S HONESTY: shown while a pitched pattern is in range, hidden when every one in range is a Drum.
	pat::Settings s;
	s.patterns[1].enable = true;
	s.patterns[1].mode = genes::Mode::Drum;
	const auto fieldOf = [&](const pat::Settings& st) {
		return [row = pat::rowOf(st)](size_t i) { return row[i]; };
	};
	CHECK(pat::transposeLive(fieldOf(s)));
	s.patterns[0].mode = genes::Mode::Drum;
	CHECK_FALSE(pat::transposeLive(fieldOf(s)));
	s.patterns[2].solo = true; // a soloed Melody is what plays
	CHECK(pat::transposeLive(fieldOf(s)));
}

TEST_CASE("transpose: a hand's edit moves with it — the edited step is transposed like every other, and the edit is kept") {
	Drive plain;
	plain.note(Kind::On, 48);
	plain.runTo(16 * kStep);
	const dpn::PatternCycle terrain = plain.d.cycleForTest(0);
	size_t k = 0;
	while (k < 16 && terrain.steps[k].kind != dpn::PatternStep::Kind::Note) {
		++k;
	}
	REQUIRE(k < 16);
	pat::EditState edits {};
	edits[0].mode = genes::Mode::Melody;
	dpn::StepEdit& e = edits[0].steps[k];
	e = dpn::editOf(terrain.steps[k], false);
	const int editKey = terrain.keyAt(k, 0) + 4; // a hand's pitch, a major third up (C major from C3)
	dpn::setEditKey(e, terrain, editKey);
	std::vector<float> words(pat::kEditsWords);
	pat::writeEditState(edits, words.data());
	const auto none = [](pat::Settings&) {};
	const std::vector<Logged> edited = transposeRun(none, 0, 0, &words);
	const std::vector<Logged> moved = transposeRun(none, 3, 1, &words);
	REQUIRE(moved.size() == edited.size());
	bool sawEdit = false;
	for (size_t i = 0; i < edited.size(); ++i) {
		CHECK(moved[i].key == referenceTranspose(0, 48, edited[i].key, 3, 1));
		if (edited[i].kind == Kind::On && edited[i].sample == static_cast<int64_t>(k) * kStep) {
			CHECK(edited[i].key == editKey);
			sawEdit = true;
		}
	}
	CHECK(sawEdit);
}

TEST_CASE("transpose: Thru notes pass as they came — a held key is never transposed") {
	Drive d;
	d.s.patterns[0].thru = pat::Thru::On;
	d.s.transposeSteps = 5;
	d.s.transposeOctaves = 1;
	d.apply();
	d.note(Kind::On, 62);
	REQUIRE(!d.log.empty());
	CHECK(d.log.front().kind == Kind::On);
	CHECK(d.log.front().key == 62);
	d.note(Kind::Off, 62);
	CHECK(d.log.back().kind == Kind::Off);
	CHECK(d.log.back().key == 62);
}

TEST_CASE("transpose: the DAW reads '+2 steps', '−1 step', '+1 oct', '−2 oct' and parses them back; both survive a save into a FRESH instance") {
	RowView rv;
	rv.row = pat::rowOf(pat::Settings {});
	const auto view = rv.view();
	const size_t steps = pat::at(pat::Global::TransposeSteps);
	const size_t oct = pat::at(pat::Global::TransposeOctaves);
	char out[32];
	const std::pair<double, const char*> stepWords[] = { { 2.0, "+2 steps" }, { -1.0, "−1 step" }, { 0.0, "0 steps" },
		                                                 { 14.0, "+14 steps" } };
	for (const auto& [v, words] : stepWords) {
		REQUIRE(pat::kTextOps.format(steps, v, view, out, sizeof out));
		CHECK(std::string(out) == words);
		double back = 99.0;
		REQUIRE(pat::kTextOps.parse(steps, words, view, &back));
		CHECK(back == v);
	}
	const std::pair<double, const char*> octWords[] = { { 1.0, "+1 oct" }, { -2.0, "−2 oct" }, { 0.0, "0 oct" } };
	for (const auto& [v, words] : octWords) {
		REQUIRE(pat::kTextOps.format(oct, v, view, out, sizeof out));
		CHECK(std::string(out) == words);
		double back = 99.0;
		REQUIRE(pat::kTextOps.parse(oct, words, view, &back));
		CHECK(back == v);
	}
	double v = 0.0;
	CHECK(pat::kTextOps.parse(steps, "-3", view, &v));
	CHECK(v == -3.0);
	CHECK(pat::kTextOps.parse(steps, "99", view, &v));
	CHECK(v == 14.0);
	CHECK_FALSE(pat::kTextOps.parse(steps, "up", view, &v));
	CHECK_FALSE(pat::kTextOps.parse(oct, "2 steps", view, &v));
	// A SAVE: the row by key, into a fresh instance — and it plays the same.
	Drive a;
	a.s.transposeSteps = -4;
	a.s.transposeOctaves = 2;
	a.apply();
	const pat::Row saved = pat::rowOf(a.d.settingsForTest());
	Drive b;
	b.d.setFields(saved.data(), saved.size());
	CHECK(b.d.settingsForTest().transposeSteps == -4);
	CHECK(b.d.settingsForTest().transposeOctaves == 2);
	CHECK(std::string(pat::kParamDescs[steps].key) == "transpose_steps");
	CHECK(std::string(pat::kParamDescs[oct].key) == "transpose_octaves");
	CHECK(pat::kParamCount == 292); // + each tab's Transpose, Span, Fill, Start, End, Fill Trigger, Modulation's four, Free Phase, the note range's two
	a.note(Kind::On, 48);
	b.note(Kind::On, 48);
	a.runTo(16 * kStep);
	b.runTo(16 * kStep);
	REQUIRE(a.log.size() == b.log.size());
	for (size_t i = 0; i < a.log.size(); ++i) {
		CHECK(a.log[i].key == b.log[i].key);
		CHECK(a.log[i].sample == b.log[i].sample);
	}
}

TEST_CASE("transpose per pattern: a pattern's own steps and octaves move its notes, and the device's pair ADDS to them — one sum, applied once") {
	const std::tuple<int, int, int, int> cases[] = { { 3, 0, 0, 0 }, { 0, 1, 0, 0 }, { 3, 1, -5, 1 }, { -7, -1, 9, 2 },
		                                             { 14, 0, 14, 0 } };
	for (const size_t scale : { size_t { 0 }, size_t { 17 } }) {
		const auto base = [&](pat::Settings& s) {
			s.patterns[0].scale = scale;
			s.patterns[0].zone = 333;
			s.patterns[0].pins[static_cast<size_t>(genes::Pin::Density)] = 0.0f;
		};
		const std::vector<Logged> plain = transposeRun(base, 0, 0);
		for (const auto& [ps, po, gs, go] : cases) {
			CAPTURE(scale);
			CAPTURE(ps);
			CAPTURE(po);
			CAPTURE(gs);
			CAPTURE(go);
			const auto tune = [&](pat::Settings& s) {
				base(s);
				s.patternTranspose[0] = { ps, po };
			};
			const std::vector<Logged> moved = transposeRun(tune, gs, go);
			REQUIRE(moved.size() == plain.size());
			for (size_t i = 0; i < plain.size(); ++i) {
				CHECK(moved[i].sample == plain[i].sample);
				CHECK(moved[i].key == referenceTranspose(scale, 48, plain[i].key, ps + gs, po + go));
			}
		}
	}
	// ANOTHER TAB'S TRANSPOSE MOVES NOTHING HERE: only the playing pattern's is read.
	const auto other = [](pat::Settings& s) {
		s.patterns[0].zone = 333;
		s.patternTranspose[1] = { 5, 2 };
	};
	const auto none = [](pat::Settings& s) { s.patterns[0].zone = 333; };
	const std::vector<Logged> a = transposeRun(other, 0, 0);
	const std::vector<Logged> b = transposeRun(none, 0, 0);
	REQUIRE(a.size() == b.size());
	for (size_t i = 0; i < a.size(); ++i) {
		CHECK(a[i].key == b[i].key);
	}
	// THE DAW'S WORDS, the per-pattern pair's too; the keys and the budget.
	RowView rv;
	rv.row = pat::rowOf(pat::Settings {});
	char out[32];
	REQUIRE(pat::kTextOps.format(pat::at(2, pat::Field::TransposeSteps), -3.0, rv.view(), out, sizeof out));
	CHECK(std::string(out) == "−3 steps");
	REQUIRE(pat::kTextOps.format(pat::at(2, pat::Field::TransposeOctaves), 1.0, rv.view(), out, sizeof out));
	CHECK(std::string(out) == "+1 oct");
	double v = 0.0;
	REQUIRE(pat::kTextOps.parse(pat::at(2, pat::Field::TransposeSteps), "+4 steps", rv.view(), &v));
	CHECK(v == 4.0);
	CHECK(std::string(pat::kParamDescs[pat::at(3, pat::Field::TransposeSteps)].key) == "p4.transpose_steps");
	CHECK(std::string(pat::kParamDescs[pat::at(0, pat::Field::TransposeOctaves)].key) == "p1.transpose_octaves");
	CHECK(pat::kParamCount == 292);
	// DRUM HIDES THE PATTERN'S PAIR, as it hides its other pitch boxes.
	CHECK(pat::drumHides(pat::at(1, pat::Field::TransposeSteps), genes::Mode::Drum));
	CHECK_FALSE(pat::drumHides(pat::at(1, pat::Field::TransposeSteps), genes::Mode::Melody));
	// SAVED BY KEY into a fresh instance.
	pat::Settings s;
	s.patternTranspose[2] = { -6, 3 };
	const pat::Row row = pat::rowOf(s);
	CHECK(pat::settingsOf(row.data(), row.size()).patternTranspose[2] == std::array<int, 2> { -6, 3 });
}

namespace {

/// **A V3 EDITS BLOB, WRITTEN AS V3 WAS** (before cells, 2026-11-07): the layout restated here from the v3 writer, not
/// called from it, so the migration is checked against the format a saved preset actually holds.
std::vector<float> v3Blob(const pat::EditState& state) {
	std::vector<float> out(pat::kEditsWordsV3);
	feathers::device::BlockWriter w(out.data(), out.size());
	for (const pat::EditOverlay& o : state) {
		for (const float a : o.anchor) {
			w.f32(a);
		}
		w.u16(static_cast<uint32_t>(o.mode));
		for (const dpn::StepEdit& e : o.steps) {
			w.u16((e.on ? 1u : 0u) | (static_cast<uint32_t>(e.kind) << 1) | (e.accent ? 8u : 0u) | (e.slide ? 16u : 0u)
			      | (e.pitched ? 32u : 0u));
			w.u16(static_cast<uint32_t>(std::clamp<int>(e.key + 256, 0, 511)));
			w.f32(e.velocity);
			w.f32(e.gate);
			w.u16(static_cast<uint32_t>(e.drumHits) | (static_cast<uint32_t>(e.drumAccent) << 8));
			w.u16(static_cast<uint32_t>(e.origKind) | (static_cast<uint32_t>(e.origHits) << 8));
			w.u16(static_cast<uint32_t>(e.origDegree + 512));
			w.u16(static_cast<uint32_t>(e.origOctave + 512));
		}
	}
	return out;
}

/// A DRUM-DECODED DRIVE (Drum or Modulation) run 16 steps, and its cycle.
struct Kit {
	Drive d;
	explicit Kit(genes::Mode mode, bool bipolar = false) {
		d.s.patterns[0].mode = mode;
		d.s.modLanes[0].bipolar = bipolar;
		d.apply();
		d.note(Kind::On, 36);
		d.runTo(16 * kStep);
	}
};

} // namespace

TEST_CASE("edits v3 → v4: a v3 blob loads into a fresh instance with every edit intact by value and no cell set; a v4 beside it wins whatever the order") {
	Kit plain(genes::Mode::Melody);
	const dpn::PatternCycle terrain = plain.d.d.cycleForTest(0);
	pat::EditState edits {};
	edits[0].mode = genes::Mode::Melody;
	edits[0].anchor = { 0.25f, -1.5f, 7.0f, 0.5f, 0.125f };
	edits[2].mode = genes::Mode::Drum;
	for (size_t k = 0; k < 16; k += 3) {
		dpn::StepEdit& e = edits[0].steps[k];
		e = dpn::editOf(terrain.steps[k], false);
		e.velocity = 0.3f + 0.04f * static_cast<float>(k);
		e.gate = 0.25f * static_cast<float>(k % 4 + 1);
		e.accent = k % 2 == 0;
		e.slide = k % 3 == 0;
		e.pitched = true;
		e.key = static_cast<int16_t>(k) - 5;
	}
	edits[2].steps[5].on = true;
	edits[2].steps[5].drumHits = 0b1010'0101;
	edits[2].steps[5].drumAccent = 0b0000'0100;
	edits[2].steps[5].origHits = 0b0000'0001;
	const std::vector<float> v3 = v3Blob(edits);
	REQUIRE(v3.size() == pat::kEditsWordsV3);
	REQUIRE(pat::kEditsWordsV3 != pat::kEditsWords);
	// THE CODEC: v3 in, v4's state out — every field as written, no cell anywhere.
	const pat::EditState read = pat::editStateOf(v3.data(), v3.size());
	CHECK(read == edits);
	for (const pat::EditOverlay& o : read) {
		for (const dpn::StepEdit& e : o.steps) {
			CHECK(e.cellSet == 0u);
		}
	}
	// THE DEVICE: restored into a FRESH instance under the v3 key, it saves (and plays) as v4 with the same edits.
	pat::PatternDevice fresh;
	REQUIRE(pat::restoreBlock(&fresh, pat::kEditsBlockV3, v3.data(), v3.size()));
	std::vector<float> saved(pat::kEditsWords);
	REQUIRE(pat::readBlock(&fresh, pat::kEditsBlock, saved.data(), saved.size()) > 0);
	CHECK(pat::editStateOf(saved.data(), saved.size()) == edits);
	// A V4 AND A STALE V3 IN ONE PATCH: v4 wins, in either order.
	pat::EditState other {};
	other[1].mode = genes::Mode::Chords;
	other[1].steps[2].on = true;
	std::vector<float> v4(pat::kEditsWords);
	pat::writeEditState(other, v4.data());
	for (const bool v3First : { true, false }) {
		CAPTURE(v3First);
		pat::PatternDevice both;
		if (v3First) {
			REQUIRE(pat::restoreBlock(&both, pat::kEditsBlockV3, v3.data(), v3.size()));
			REQUIRE(pat::restoreBlock(&both, pat::kEditsBlock, v4.data(), v4.size()));
		}
		else {
			REQUIRE(pat::restoreBlock(&both, pat::kEditsBlock, v4.data(), v4.size()));
			REQUIRE(pat::restoreBlock(&both, pat::kEditsBlockV3, v3.data(), v3.size()));
		}
		std::vector<float> got(pat::kEditsWords);
		REQUIRE(pat::readBlock(&both, pat::kEditsBlock, got.data(), got.size()) > 0);
		CHECK(pat::editStateOf(got.data(), got.size()) == other);
	}
}

TEST_CASE("cell levels by value: Drum's velocity of a hit, Accent adding a quarter on top; Modulation's value at a cell (a hit there, held on), in both polarities; every cell nobody set exactly as before") {
	for (const genes::Mode mode : { genes::Mode::Drum, genes::Mode::Modulation }) {
		for (const bool bipolar : { false, true }) {
			if (mode == genes::Mode::Drum && bipolar) {
				continue;
			}
			CAPTURE(static_cast<int>(mode));
			CAPTURE(bipolar);
			Kit base(mode, bipolar);
			const dpn::PatternCycle terrain = base.d.d.cycleForTest(0);
			REQUIRE(terrain.length >= 16);
			// A STEP WITH A HIT on some lane `a`, and a lane `b` that does not hit there.
			size_t k = 0;
			while (k < terrain.length && terrain.steps[k].drumHits == 0u) {
				++k;
			}
			REQUIRE(k < terrain.length);
			const auto a = static_cast<size_t>(std::countr_zero(static_cast<unsigned>(terrain.steps[k].drumHits)));
			size_t b = 0;
			while (b < 8 && ((terrain.steps[k].drumHits >> b) & 1u) != 0u) {
				++b;
			}
			REQUIRE(b < 8);
			pat::EditState edits {};
			edits[0].mode = mode;
			dpn::StepEdit& e = edits[0].steps[k];
			e = dpn::editOf(terrain.steps[k], true);
			const float levelA = pat::quantizeLevel(0.31f);
			const float levelB = pat::quantizeLevel(0.77f);
			e.cellLevel[a] = levelA;
			e.cellLevel[b] = levelB;
			e.cellSet = static_cast<uint8_t>((1u << a) | (1u << b));
			e.drumAccent = static_cast<uint8_t>(e.drumAccent & ~(1u << a));
			std::vector<float> words(pat::kEditsWords);
			pat::writeEditState(edits, words.data());
			// THE BLOCK ROUND-TRIPS THE LEVELS EXACTLY.
			REQUIRE(pat::editStateOf(words.data(), words.size()) == edits);
			Kit edited(mode, bipolar);
			REQUIRE(pat::applyBlock(&edited.d.d, pat::kEditsBlock, words.data(), words.size()));
			edited.d.runTo(32 * kStep);
			const dpn::PatternCycle got = edited.d.d.cycleForTest(0);
			CHECK(got.steps[k].drumVelocity[a] == levelA); // the hit's velocity IS the level
			if (mode == genes::Mode::Modulation) {
				CHECK(((got.steps[k].drumHits >> b) & 1u) == 1u); // a set cell is a hit there
				CHECK(got.steps[k].drumVelocity[b] == levelB);
				pat::ModLevels lv {};
				uint8_t live = 0;
				pat::modLevelsOf(got, 0xFF, lv, &live);
				CHECK(lv[k][b] == levelB);
				// AND THE LANE'S OUTPUT AT THAT STEP, in the tab's polarity.
				const pat::ModLanes m { pat::ModSmooth::Hold, 0.5f, bipolar, false };
				CHECK(pat::modOutput(lv, live, got.length, k, 0.5, b, m) == doctest::Approx(bipolar ? 2.0 * levelB - 1.0 : levelB));
			}
			else {
				CHECK(((got.steps[k].drumHits >> b) & 1u) == 0u); // Drum: a level on a lane that does not hit adds no hit
			}
			// EVERY OTHER CELL AND STEP EXACTLY AS THE TERRAIN DECODED IT.
			for (size_t s = 0; s < terrain.length; ++s) {
				for (size_t l = 0; l < 8; ++l) {
					if (s == k && (l == a || l == b)) {
						continue;
					}
					CAPTURE(s);
					CAPTURE(l);
					CHECK(got.steps[s].drumVelocity[l] == terrain.steps[s].drumVelocity[l]);
					CHECK(((got.steps[s].drumHits >> l) & 1u) == ((terrain.steps[s].drumHits >> l) & 1u));
				}
			}
			// ACCENT ON TOP: lane a accented, its level plus a quarter.
			e.drumAccent = static_cast<uint8_t>(e.drumAccent | (1u << a));
			pat::writeEditState(edits, words.data());
			Kit accented(mode, bipolar);
			REQUIRE(pat::applyBlock(&accented.d.d, pat::kEditsBlock, words.data(), words.size()));
			accented.d.runTo(32 * kStep);
			CHECK(accented.d.d.cycleForTest(0).steps[k].drumVelocity[a] == std::min(1.0f, levelA + 0.25f));
		}
	}
}
