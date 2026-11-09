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

#include "mod/arp/arp.h"
#include "mod/arp/arp_core.h"
#include "mod/arp/arp_types.h"
#include "fx/sklitter/sklitter_types.h"
#include "mod/arp/humanize.h"
#include "mod/arp/step_emitter.h"
#include "mod/arp/pattern_types.h"
#include "mod/tracker/tracker_types.h"
#include "plugin/device_registry.h"
#include "plugin/synth.h"
#include "rack/rack_engine.h"
#include "rack/rack_state.h"
#include "rack_endpoints.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// THE CLASSIC ARP, BY VALUE (CLAUDE.md "assert the value, not the buffer"). Every case
// reads note EVENTS and the SAMPLE each one landed on, never a render. The expected samples are restated here from
// the design (slot g starts at g × the step length, plus Nudge and Swing; the first sample at or after it), not taken
// from the arp's own arithmetic.

namespace arp = feathers::mod::arp;
namespace device = feathers::device;
using Kind = device::NoteEvent::Kind;

namespace {

constexpr double kRate = 48000.0;

/// ONE EVENT THE ARP WROTE, and the absolute sample it was written at.
struct Logged {
	Kind kind;
	int16_t key;
	double velocity;
	int64_t sample;
};

/// **THE SYNTH'S PROTOCOL, AROUND ONE ARP** — host notes through `processNotes`, then the clock call at that sample
/// and at every sample it asks for next (`Synth::emitLaneNotes` and `render`'s cut). The song runs at `bpm` from
/// `startBeats` at sample 0 while `playing`.
struct Drive {
	arp::Arp a;
	arp::Settings s;
	double bpm = 120.0;
	bool playing = true;
	bool haveTransport = true;
	double startBeats = 0.0;
	int64_t songOrigin = 0; ///< the sample at which the song stood at `startBeats`
	int64_t now = 0;
	size_t capacity = 256;
	/// THE LONGEST CHUNK: 32 as the synth renders, or unbounded to show the arp's own `samplesUntilNext` lands every onset.
	int64_t chunk = 32;
	std::vector<Logged> log;
	/// EVERYTHING EVER WRITTEN — `log` may be cleared by a case to look at one stretch; this never is.
	std::vector<Logged> all;
	/// Absolute samples at which the arp was asked — the chunk cuts it caused.
	std::vector<int64_t> calls;

	Drive() {
		a.prepare(kRate);
		// THE CASES HERE WERE WRITTEN AGAINST THE FREE CLOCK BEING ON (Only w/ Transport off); the device's default is now On
		// (by design), asserted by value in "arp: Only w/ Transport is On by default".
		s.run = arp::RunMode::Always;
		apply();
	}
	void apply() {
		const arp::Row row = arp::rowOf(s);
		a.setFields(row.data(), row.size());
	}
	double secondsPerBeat() const { return 60.0 / bpm; }
	device::TransportClock clockAt(int64_t sample) const {
		device::TransportClock c = device::TransportClock::fromSecondsPerBeat(secondsPerBeat());
		if (haveTransport) {
			const double beats =
			    startBeats + (playing ? static_cast<double>(sample - songOrigin) / kRate / secondsPerBeat() : 0.0);
			c.songPositionBeats = beats;
			c.songPositionSeconds = beats * secondsPerBeat();
			c.playing = playing;
		}
		return c;
	}
	/// Put the song at `beats` from this sample on (a locate, a loop, a scrub).
	void locate(double beats) {
		startBeats = beats;
		songOrigin = now;
	}
	void note(Kind kind, int16_t key, double velocity = 1.0) {
		device::NoteEvent e {};
		e.kind = kind;
		e.noteId = 7;
		e.channel = 0;
		e.key = key;
		e.velocity = velocity;
		std::array<device::NoteEvent, 8> out {};
		size_t n = 0;
		a.processNotes(&e, 1, out.data(), out.size(), &n);
		for (size_t i = 0; i < n; ++i) {
			log.push_back({ out[i].kind, out[i].key, out[i].velocity, now });
			all.push_back(log.back());
		}
	}
	void press(std::initializer_list<int> keys, double velocity = 1.0) {
		for (const int k : keys) {
			note(Kind::On, static_cast<int16_t>(k), velocity);
		}
	}
	void release(std::initializer_list<int> keys) {
		for (const int k : keys) {
			note(Kind::Off, static_cast<int16_t>(k));
		}
	}
	/// RUN TO `until` (exclusive), calling the arp where the synth would.
	void runTo(int64_t until) {
		std::vector<device::NoteEvent> out(capacity + 1);
		while (now < until) {
			uint32_t next = 0;
			const size_t n = a.emit(clockAt(now), static_cast<uint64_t>(now), out.data(), capacity, &next);
			calls.push_back(now);
			REQUIRE(n <= capacity);
			for (size_t i = 0; i < n; ++i) {
				log.push_back({ out[i].kind, out[i].key, out[i].velocity, now });
				all.push_back(log.back());
			}
			REQUIRE(next >= 1);
			// AT MOST A RENDER CHUNK (32 samples), as `Synth::render` cuts: the arp is asked again whether or not it asked.
			now += std::min<int64_t>({ static_cast<int64_t>(next), chunk, until - now });
		}
	}
	std::vector<Logged> ons() const {
		std::vector<Logged> r;
		for (const Logged& l : log) {
			if (l.kind == Kind::On) {
				r.push_back(l);
			}
		}
		return r;
	}
	std::vector<int> onKeys() const {
		std::vector<int> r;
		for (const Logged& l : ons()) {
			r.push_back(l.key);
		}
		return r;
	}
	std::vector<int64_t> onSamples() const {
		std::vector<int64_t> r;
		for (const Logged& l : ons()) {
			r.push_back(l.sample);
		}
		return r;
	}
};

/// **NO STUCK NOTES**: walking the log, every key's ons and offs alternate and end off.
void checkBalanced(const std::vector<Logged>& log) {
	std::map<int, int> sounding;
	for (const Logged& l : log) {
		if (l.kind == Kind::On) {
			CHECK_MESSAGE(sounding[l.key] == 0, "an on for key " << l.key << " at " << l.sample << " while it sounds");
			++sounding[l.key];
		}
		else if (l.kind == Kind::Off) {
			CHECK_MESSAGE(sounding[l.key] == 1, "an off for key " << l.key << " at " << l.sample << " that is not sounding");
			sounding[l.key] = 0;
		}
	}
	for (const auto& [key, n] : sounding) {
		CHECK_MESSAGE(n == 0, "key " << key << " left sounding");
	}
}

/// The independent restatement: slot g's onset sample for a step of `stepSamples`.
int64_t onsetSample(int64_t g, double stepSamples, double nudge = 0.0, double swing = 0.0) {
	const double u = static_cast<double>(g) + nudge + ((g % 2) != 0 ? swing * 0.5 : 0.0);
	return static_cast<int64_t>(std::ceil(u * stepSamples - 1e-6));
}

double stepSamplesOf(double divisionBeats, double bpm) {
	return divisionBeats * 60.0 / bpm * kRate;
}

/// Ends the chord, runs on a while, and checks nothing is left sounding.
void endAndCheck(Drive& d, std::initializer_list<int> keys) {
	d.release(keys);
	d.runTo(d.now + 48000);
	CHECK(d.a.soundingForTest() == 0);
	checkBalanced(d.all);
}

} // namespace

//------------------------------------------------------------------------------------------------//
// The clock                                                                                      //
//------------------------------------------------------------------------------------------------//

TEST_CASE("arp: step onsets land on the exact sample at several tempos and divisions, dotted and triplet included") {
	struct Case {
		double bpm;
		size_t timeBase;
		double beats;
	};
	// Time Base index = division index + 1 (Hz is 0). 1/16 = 7, 1/16. = 8, 1/16T = 9, 1/8T = 12, 1/4 = 13.
	const Case cases[] = {
		{ 120.0, 7, 0.25 },   { 120.0, 8, 0.375 },        { 120.0, 9, 0.25 * 2.0 / 3.0 },
		{ 140.0, 7, 0.25 },   { 97.3, 12, 0.5 * 2.0 / 3.0 }, { 133.0, 13, 1.0 },
		{ 87.0, 4, 0.125 },
	};
	for (const int64_t chunk : { int64_t { 32 }, int64_t { 1 } << 40 })
	for (const Case& c : cases) {
		CAPTURE(chunk);
		CAPTURE(c.bpm);
		CAPTURE(c.timeBase);
		Drive d;
		d.chunk = chunk; // unbounded: only the arp's own count of samples to its next onset cuts the run
		d.bpm = c.bpm;
		d.s.timeBase = c.timeBase;
		d.s.gate = 0.5f;
		d.apply();
		d.press({ 60 });
		const double step = stepSamplesOf(c.beats, c.bpm);
		d.runTo(static_cast<int64_t>(step * 12.0) + 10);
		const std::vector<int64_t> got = d.onSamples();
		REQUIRE(got.size() == 13);
		for (size_t g = 0; g < got.size(); ++g) {
			CHECK(got[g] == onsetSample(static_cast<int64_t>(g), step));
		}
		// THE ARP WAS ASKED EXACTLY THERE, which is what the synth's chunk cut is.
		for (const int64_t at : got) {
			CHECK(std::find(d.calls.begin(), d.calls.end(), at) != d.calls.end());
		}
		endAndCheck(d, { 60 });
	}
}

TEST_CASE("arp: the Swing 16 feel delays every second sixteenth by amount × half a step; the first of each pair stays on the grid") {
	Drive d;
	d.s.feel = 3; // Swing 16: the MPC's 50% + 25% × amount, so amount a puts an off-beat a/2 of a step late
	d.s.feelAmount = 0.5f;
	d.apply();
	d.press({ 60 });
	const double step = stepSamplesOf(0.25, 120.0);
	d.runTo(static_cast<int64_t>(step * 8.0) + 1);
	const std::vector<int64_t> got = d.onSamples();
	REQUIRE(got.size() == 9);
	for (size_t g = 0; g < got.size(); ++g) {
		CHECK(got[g] == onsetSample(static_cast<int64_t>(g), step, 0.0, 0.5));
	}
	CHECK(got[1] == 6000 + 1500); // a second step a quarter-step late at 50%
	CHECK(got[2] == 12000);
	endAndCheck(d, { 60 });
}

TEST_CASE("arp: Rate in Hz runs on the song's seconds, unsynced to the tempo") {
	Drive d;
	d.s.timeBase = 0;                                 // Hz
	d.s.rate = static_cast<float>(std::log2(8.0));    // 8 steps a second
	d.apply();
	d.bpm = 93.0; // irrelevant to an Hz rate
	d.press({ 60 });
	d.runTo(48000 + 1);
	const std::vector<int64_t> got = d.onSamples();
	REQUIRE(got.size() == 9);
	for (size_t g = 0; g < got.size(); ++g) {
		CHECK(got[g] == onsetSample(static_cast<int64_t>(g), 6000.0));
	}
	endAndCheck(d, { 60 });
}

//------------------------------------------------------------------------------------------------//
// The pattern                                                                                    //
//------------------------------------------------------------------------------------------------//

TEST_CASE("arp: each Order plays its sequence (the chord pressed E, G, C)") {
	struct Case {
		arp::Order order;
		std::vector<int> expect;
	};
	const Case cases[] = {
		{ arp::Order::Up, { 60, 64, 67, 60, 64, 67, 60, 64 } },
		{ arp::Order::Down, { 67, 64, 60, 67, 64, 60, 67, 64 } },
		{ arp::Order::UpDown, { 60, 64, 67, 64, 60, 64, 67, 64 } },
		{ arp::Order::UpDownRepeat, { 60, 64, 67, 67, 64, 60, 60, 64 } },
		{ arp::Order::Played, { 64, 67, 60, 64, 67, 60, 64, 67 } },
	};
	for (const Case& c : cases) {
		CAPTURE(static_cast<int>(c.order));
		Drive d;
		d.s.order = c.order;
		d.apply();
		d.press({ 64, 67, 60 });
		d.runTo(6000 * 7 + 1);
		CHECK(d.onKeys() == c.expect);
		endAndCheck(d, { 64, 67, 60 });
	}
}

TEST_CASE("arp: Up-Down with a two-note chord plays each end once; with one note it repeats the note") {
	Drive d;
	d.s.order = arp::Order::UpDown;
	d.apply();
	d.press({ 60, 72 });
	d.runTo(6000 * 3 + 1);
	CHECK(d.onKeys() == std::vector<int> { 60, 72, 60, 72 });
	endAndCheck(d, { 60, 72 });
}

TEST_CASE("arp: Random is deterministic per Seed and step — the same position draws the same note, another seed differs") {
	const auto run = [](int seed, double fromBeats) {
		Drive d;
		d.s.order = arp::Order::Random;
		d.s.seed = seed;
		d.apply();
		d.locate(fromBeats);
		d.press({ 60, 62, 64, 65, 67, 69, 71 });
		d.runTo(6000 * 31 + 1);
		endAndCheck(d, { 60, 62, 64, 65, 67, 69, 71 });
		return d.onKeys();
	};
	const std::vector<int> a = run(17, 0.0);
	REQUIRE(a.size() == 32);
	CHECK(run(17, 0.0) == a);
	// FROM STEP 8 (beat 2), THE SAME SEED GIVES WHAT THE FIRST RUN PLAYED FROM ITS STEP 8: nothing is a running state.
	const std::vector<int> later = run(17, 2.0);
	CHECK(std::vector<int>(later.begin(), later.begin() + 24) == std::vector<int>(a.begin() + 8, a.end()));
	const std::vector<int> other = run(18, 0.0);
	CHECK(other != a);
	// THE DRAW IS THE SEED'S AND THE STEP'S: restated here as the Up pattern indexed by the step's dice.
	const int up[] = { 60, 62, 64, 65, 67, 69, 71 };
	for (size_t k = 0; k < a.size(); ++k) {
		CHECK(a[k] == up[arp::stepDice(17, static_cast<int64_t>(k)) % 7]);
	}
	// AND IT USES MORE THAN ONE NOTE (a dice stuck on one index would satisfy everything above).
	CHECK(std::set<int>(a.begin(), a.end()).size() >= 4);
}

TEST_CASE("arp: Octaves, Each Note walks a note through its octaves; Whole Chord walks the chord once per octave") {
	SUBCASE("Each Note") {
		Drive d;
		d.s.octaves = 2;
		d.s.octaveMode = arp::OctaveMode::EachNote;
		d.apply();
		d.press({ 60, 64, 67 });
		d.runTo(6000 * 6 + 1);
		CHECK(d.onKeys() == std::vector<int> { 60, 72, 64, 76, 67, 79, 60 });
		endAndCheck(d, { 60, 64, 67 });
	}
	SUBCASE("Whole Chord") {
		Drive d;
		d.s.octaves = 2;
		d.s.octaveMode = arp::OctaveMode::WholeChord;
		d.apply();
		d.press({ 60, 64, 67 });
		d.runTo(6000 * 6 + 1);
		CHECK(d.onKeys() == std::vector<int> { 60, 64, 67, 72, 76, 79, 60 });
		endAndCheck(d, { 60, 64, 67 });
	}
	SUBCASE("four octaves Down") {
		Drive d;
		d.s.octaves = 4;
		d.s.order = arp::Order::Down;
		d.s.octaveMode = arp::OctaveMode::WholeChord;
		d.apply();
		d.press({ 60, 67 });
		d.runTo(6000 * 7 + 1);
		CHECK(d.onKeys() == std::vector<int> { 103, 96, 91, 84, 79, 72, 67, 60 });
		endAndCheck(d, { 60, 67 });
	}
}

//------------------------------------------------------------------------------------------------//
// Gate and phase                                                                                 //
//------------------------------------------------------------------------------------------------//

TEST_CASE("arp: Gate sets each note's length as a share of its step; 100% ends on the next onset, over 100% ties") {
	const auto offsOf = [](const Drive& d) {
		std::vector<int64_t> r;
		for (const Logged& l : d.log) {
			if (l.kind == Kind::Off) {
				r.push_back(l.sample);
			}
		}
		return r;
	};
	SUBCASE("25%") {
		Drive d;
		d.s.gate = 0.25f;
		d.apply();
		d.press({ 60, 64 });
		d.runTo(6000 * 2);
		CHECK(offsOf(d) == std::vector<int64_t> { 1500, 7500 });
		endAndCheck(d, { 60, 64 });
	}
	SUBCASE("100%: the off lands on the next onset's sample, before its on") {
		Drive d;
		d.s.gate = 1.0f;
		d.apply();
		d.press({ 60, 64 });
		d.runTo(6000 + 1);
		REQUIRE(d.log.size() == 3);
		CHECK(d.log[0].kind == Kind::On);
		CHECK(d.log[1].kind == Kind::Off);
		CHECK(d.log[1].sample == 6000);
		CHECK(d.log[2].kind == Kind::On);
		CHECK(d.log[2].sample == 6000);
		endAndCheck(d, { 60, 64 });
	}
	SUBCASE("150%: notes overlap — the first ends half a step after the second starts") {
		Drive d;
		d.s.gate = 1.5f;
		d.apply();
		d.press({ 60, 64 });
		d.runTo(9000 + 1);
		REQUIRE(d.log.size() == 3);
		CHECK(d.log[1].kind == Kind::On);
		CHECK(d.log[1].sample == 6000);
		CHECK(d.log[2].kind == Kind::Off);
		CHECK(d.log[2].key == 60);
		CHECK(d.log[2].sample == 9000);
		endAndCheck(d, { 60, 64 });
	}
	SUBCASE("a tie into the same key HOLDS one note: one on, and the off at the run's end") {
		Drive d;
		d.s.gate = 1.5f;
		d.apply();
		d.press({ 60 });
		d.runTo(6000 * 4 + 1);
		REQUIRE(d.log.size() == 1); // the first step's on, held through steps 1, 2 and 3
		CHECK(d.log[0].kind == Kind::On);
		CHECK(d.log[0].sample == 0);
		endAndCheck(d, { 60 }); // the release ends the run: one off
		CHECK(d.log.back().kind == Kind::Off);
	}
	SUBCASE("felt, the gate is a share of the step as it falls") {
		Drive d;
		d.s.gate = 0.5f;
		d.s.feel = 3; // Swing 16
		d.s.feelAmount = 0.5f;
		d.apply();
		d.press({ 60, 64 });
		d.runTo(12000);
		// Step 0 runs 0..7500 (the second is a quarter step late), step 1 runs 7500..12000.
		CHECK(offsOf(d) == std::vector<int64_t> { 3750, 9750 });
		endAndCheck(d, { 60, 64 });
	}
}

TEST_CASE("arp: Offset rotates the pattern by whole steps without moving an onset; Nudge moves every onset") {
	SUBCASE("Offset +1") {
		Drive d;
		d.s.offset = 1;
		d.apply();
		d.press({ 60, 64, 67 });
		d.runTo(6000 * 3 + 1);
		CHECK(d.onKeys() == std::vector<int> { 67, 60, 64, 67 }); // slot 0 plays pattern[−1 mod 3]
		CHECK(d.onSamples() == std::vector<int64_t> { 0, 6000, 12000, 18000 });
		endAndCheck(d, { 60, 64, 67 });
	}
	SUBCASE("Offset −2") {
		Drive d;
		d.s.offset = -2;
		d.apply();
		d.press({ 60, 64, 67 });
		d.runTo(6000 * 2 + 1);
		CHECK(d.onKeys() == std::vector<int> { 67, 60, 64 });
		endAndCheck(d, { 60, 64, 67 });
	}
	SUBCASE("Nudge +25%") {
		Drive d;
		d.s.nudge = 0.25f;
		d.apply();
		d.press({ 60, 64 });
		d.runTo(6000 * 2 + 1500 + 1);
		CHECK(d.onSamples() == std::vector<int64_t> { 1500, 7500, 13500 });
		CHECK(d.onKeys() == std::vector<int> { 60, 64, 60 });
		endAndCheck(d, { 60, 64 });
	}
	SUBCASE("Nudge −10% at 140 bpm") {
		Drive d;
		d.bpm = 140.0;
		d.s.nudge = -0.1f;
		d.apply();
		d.locate(1.0); // a beat in, so slot 4's onset (−0.1 of a step before the beat) is ahead
		d.press({ 60 });
		const double step = stepSamplesOf(0.25, 140.0);
		d.runTo(static_cast<int64_t>(step * 3.0));
		const std::vector<int64_t> got = d.onSamples();
		REQUIRE(got.size() == 3);
		for (size_t i = 0; i < got.size(); ++i) {
			// Slot 4 (at −0.1 of a step) is behind the locate, so the first is slot 5: 0.9 of a step after it, and the
			// song stood at slot 4.0 on sample 0.
			const auto g = static_cast<int64_t>(i) + 1;
			CHECK(got[i] == onsetSample(g, step, static_cast<double>(-0.1f)));
		}
		endAndCheck(d, { 60 });
	}
}

//------------------------------------------------------------------------------------------------//
// The song                                                                                       //
//------------------------------------------------------------------------------------------------//

TEST_CASE("arp: a scrub or a loop gives the same notes at the same song position, with nothing stored") {
	Drive first;
	first.s.order = arp::Order::UpDown;
	first.s.octaves = 2;
	first.apply();
	first.press({ 60, 63, 67 });
	first.runTo(6000 * 16);
	const std::vector<int> straight = first.onKeys();
	REQUIRE(straight.size() == 16);
	// THE LOOP: back to beat 1 (slot 4) and on — the notes from there are the first pass's from slot 4.
	first.log.clear();
	first.locate(1.0);
	first.runTo(first.now + 6000 * 8);
	CHECK(first.onKeys() == std::vector<int>(straight.begin() + 4, straight.begin() + 12));
	// A SCRUB MID-STEP waits for the next onset: to beat 2.1 → slot 9 at beat 2.25, which is 0.15 beat away.
	first.log.clear();
	first.locate(2.1);
	const int64_t from = first.now;
	first.runTo(first.now + 6000 * 3);
	REQUIRE(!first.ons().empty());
	CHECK(first.ons()[0].sample - from == 3600);
	CHECK(first.ons()[0].key == straight[9]);
	endAndCheck(first, { 60, 63, 67 });
	// A FRESH ARP STARTED AT BEAT 2 plays what the first played from slot 8.
	Drive fresh;
	fresh.s = first.s;
	fresh.apply();
	fresh.locate(2.0);
	fresh.press({ 60, 63, 67 });
	fresh.runTo(6000 * 8);
	CHECK(fresh.onKeys() == std::vector<int>(straight.begin() + 8, straight.end()));
	endAndCheck(fresh, { 60, 63, 67 });
}

TEST_CASE("arp: a loop back past a sounding note ends it at once") {
	Drive d;
	d.s.gate = 1.5f;
	d.apply();
	d.press({ 60, 64 });
	d.runTo(6000 * 3 + 100);
	d.log.clear();
	d.locate(0.0);
	d.runTo(d.now + 1);
	// Slot 3's note (and slot 2's tie) were sounding; both end on the loop's first sample, before slot 0's on.
	REQUIRE(d.log.size() >= 2);
	size_t firstOn = d.log.size();
	for (size_t i = 0; i < d.log.size(); ++i) {
		if (d.log[i].kind == Kind::On) {
			firstOn = std::min(firstOn, i);
		}
	}
	for (size_t i = 0; i < firstOn; ++i) {
		CHECK(d.log[i].kind == Kind::Off);
	}
	endAndCheck(d, { 60, 64 });
}

TEST_CASE("arp: a chord change mid-bar jumps to the bar position in the new pattern — it does not restart") {
	Drive d;
	d.press({ 60, 64, 67 });
	d.runTo(6000 * 5); // slots 0..4 played
	// THE NEW CHORD ON SLOT 5'S SAMPLE: offs, then ons, before the arp's call at that sample (render cuts there).
	d.release({ 60, 64, 67 });
	d.press({ 62, 65, 69, 71 });
	d.runTo(6000 * 7 + 1);
	const std::vector<int> keys = d.onKeys();
	REQUIRE(keys.size() == 8);
	CHECK(std::vector<int>(keys.begin(), keys.begin() + 5) == std::vector<int> { 60, 64, 67, 60, 64 });
	// Slot 5 of a four-note Up pattern is entry 1, slot 6 entry 2, slot 7 entry 3. A restart would give 62, 65, 69.
	CHECK(std::vector<int>(keys.begin() + 5, keys.end()) == std::vector<int> { 65, 69, 71 });
	endAndCheck(d, { 62, 65, 69, 71 });
}

TEST_CASE("arp: transport stopped, Stop with Transport off — a free clock starts at the first key press") {
	Drive d;
	d.playing = false;
	d.locate(3.3); // stopped somewhere off the grid; the free clock does not care
	d.runTo(1000);
	d.press({ 60, 64 });
	d.runTo(1000 + 6000 * 3 + 1);
	CHECK(d.onSamples() == std::vector<int64_t> { 1000, 7000, 13000, 19000 });
	CHECK(d.onKeys() == std::vector<int> { 60, 64, 60, 64 });
	endAndCheck(d, { 60, 64 });
	// A SECOND CHORD AFTER LETTING GO starts the free clock again, at its own press.
	d.log.clear();
	d.runTo(d.now + 777);
	const int64_t press = d.now;
	d.press({ 67 });
	d.runTo(d.now + 6000 + 1);
	CHECK(d.onSamples() == std::vector<int64_t> { press, press + 6000 });
	endAndCheck(d, { 67 });
}

TEST_CASE("arp: transport stopped, Stop with Transport on — it plays nothing, and the transport stopping ends its notes") {
	Drive d;
	d.s.run = arp::RunMode::Transport;
	d.s.gate = 1.5f;
	d.apply();
	d.playing = false;
	d.press({ 60, 64 });
	d.runTo(48000);
	CHECK(d.log.empty());
	// THE TRANSPORT PLAYS: it plays on the song's grid. Then STOPS mid-note: offs at once, and silence.
	d.playing = true;
	d.locate(0.0);
	d.runTo(d.now + 6000 * 2 + 3000);
	CHECK(d.ons().size() == 3);
	d.locate(d.clockAt(d.now).songPositionBeats);
	d.playing = false;
	const int64_t stopAt = d.now;
	d.runTo(d.now + 48000);
	CHECK(d.ons().size() == 3);
	CHECK(d.a.soundingForTest() == 0);
	CHECK(d.log.back().kind == Kind::Off);
	CHECK(d.log.back().sample == stopAt);
	endAndCheck(d, { 60, 64 });
}

TEST_CASE("arp: a transport restart re-phases a free-running arp onto the song grid, from either setting") {
	for (const bool stopWith : { false, true }) {
		CAPTURE(stopWith);
		Drive d;
		d.s.run = stopWith ? arp::RunMode::Transport : arp::RunMode::Always;
		d.apply();
		d.playing = false;
		d.runTo(1000);
		d.press({ 60, 64 });
		d.runTo(20000);
		d.log.clear();
		// THE TRANSPORT STARTS at beat 10.1: the song's next slot is 41, at 10.25 beats — 0.15 beat (3600 samples) on.
		d.playing = true;
		d.locate(10.1);
		d.runTo(d.now + 6000 * 2);
		if (!stopWith) {
			REQUIRE(!d.log.empty());
			CHECK(d.log.front().kind == Kind::Off); // the free clock's note ends as the song takes over
			CHECK(d.log.front().sample == 20000);
		}
		REQUIRE(d.ons().size() == 2);
		CHECK(d.ons()[0].sample == 23600);
		CHECK(d.ons()[1].sample == 29600);
		CHECK(d.ons()[0].key == 64); // slot 41 of a two-note Up pattern
		endAndCheck(d, { 60, 64 });
	}
}

TEST_CASE("arp: with the transport stopping under held keys and Stop with Transport off, it carries on from the song's phase") {
	Drive d;
	d.press({ 60, 64 });
	d.runTo(6000 * 2 + 2000); // slot 2 sounding, 2000 samples in
	d.locate(d.clockAt(d.now).songPositionBeats);
	d.playing = false;
	const int64_t stop = d.now;
	d.log.clear();
	d.runTo(d.now + 6000 * 2);
	// The note under the stop ends there; the free clock goes on in the song's phase: the next step 4000 on.
	REQUIRE(!d.ons().empty());
	CHECK(d.ons()[0].sample == stop + 4000);
	CHECK(d.ons()[0].key == 64); // slot 3
	endAndCheck(d, { 60, 64 });
}

//------------------------------------------------------------------------------------------------//
// Latch, enable, and never losing an off                                                         //
//------------------------------------------------------------------------------------------------//

TEST_CASE("arp: Latch holds the chord after release; a chord played after letting go replaces it; Latch off ends it") {
	Drive d;
	d.s.latch = true;
	d.apply();
	d.press({ 60, 64 });
	d.runTo(3000);
	d.release({ 60, 64 });
	d.runTo(6000 * 4);
	CHECK(d.onKeys() == std::vector<int> { 60, 64, 60, 64 });
	// A NEW CHORD after everything was let go replaces the latched one.
	d.press({ 67 });
	d.press({ 72 }); // added while 67 is still down
	d.runTo(6000 * 6);
	CHECK(d.onKeys() == std::vector<int> { 60, 64, 60, 64, 67, 72 });
	d.release({ 67, 72 });
	d.runTo(6000 * 7);
	CHECK(d.onKeys().size() == 7); // still latched
	// LATCH OFF with nothing held: the arp ends at once.
	d.s.latch = false;
	d.apply();
	d.runTo(d.now + 48000);
	CHECK(d.onKeys().size() == 7);
	CHECK(d.a.soundingForTest() == 0);
	checkBalanced(d.log);
}

TEST_CASE("arp: switched off, the played notes pass through; switched on with keys down, their offs go first") {
	Drive d;
	d.s.enabled = false;
	d.apply();
	d.press({ 60, 64 });
	REQUIRE(d.log.size() == 2); // passed straight through
	CHECK(d.log[0].kind == Kind::On);
	d.runTo(12000);
	CHECK(d.log.size() == 2); // and nothing of its own
	d.s.enabled = true;
	d.apply();
	d.runTo(12001);
	// THE TWO PASSED KEYS END, THEN ITS OWN STEP STARTS (slot 2, on this sample).
	REQUIRE(d.log.size() == 5);
	CHECK(d.log[2].kind == Kind::Off);
	CHECK(d.log[3].kind == Kind::Off);
	CHECK(d.log[4].kind == Kind::On);
	CHECK(d.log[4].sample == 12000);
	// SWITCHED OFF MID-NOTE: its note ends at once.
	d.runTo(13000);
	d.s.enabled = false;
	d.apply();
	d.runTo(14000);
	CHECK(d.a.soundingForTest() == 0);
	d.release({ 60, 64 }); // pass through, as offs for keys that are not sounding — harmless and balanced below
	std::vector<Logged> ownAndPassed;
	for (const Logged& l : d.log) {
		if (!(l.kind == Kind::Off && l.sample == d.now && (l.key == 60 || l.key == 64) && &l >= &d.log[d.log.size() - 2])) {
			ownAndPassed.push_back(l);
		}
	}
	checkBalanced(ownAndPassed);
}

TEST_CASE("arp: a full output buffer drops the later event — an on — and delays an off by a sample, never loses it") {
	SUBCASE("room for one: at a 100% gate the off is written and the on dropped") {
		Drive d;
		d.capacity = 1;
		d.s.gate = 1.0f;
		d.apply();
		d.press({ 60, 64 });
		d.runTo(6000 * 4 + 1);
		// Each onset sample has an off due and an on due; only the off fits.
		for (const Logged& l : d.log) {
			CHECK(l.sample % 6000 == 0);
		}
		endAndCheck(d, { 60, 64 });
	}
	SUBCASE("two offs due at one sample with room for one: the second is written one sample later") {
		Drive d;
		d.s.gate = 2.0f;
		d.apply();
		d.press({ 60, 64, 67 });
		d.runTo(6000 + 100); // slots 0 and 1 sounding
		REQUIRE(d.a.soundingForTest() == 2);
		d.capacity = 1;
		d.s.enabled = false;
		d.apply();
		const int64_t at = d.now;
		d.log.clear();
		d.runTo(d.now + 10);
		REQUIRE(d.log.size() == 2);
		CHECK(d.log[0].kind == Kind::Off);
		CHECK(d.log[0].sample == at);
		CHECK(d.log[1].kind == Kind::Off);
		CHECK(d.log[1].sample == at + 1);
		CHECK(d.a.soundingForTest() == 0);
	}
	SUBCASE("no room at all: nothing is written and nothing is left sounding") {
		Drive d;
		d.capacity = 0;
		d.press({ 60 });
		d.runTo(48000);
		CHECK(d.log.empty());
		CHECK(d.a.soundingForTest() == 0);
	}
}

TEST_CASE("arp: no stuck notes across releases, a deactivate and every order, rate and gate") {
	for (int order = 0; order < static_cast<int>(arp::Order::kCount); ++order) {
		for (const float gate : { 0.01f, 0.5f, 1.0f, 1.3f, 2.0f }) {
			CAPTURE(order);
			CAPTURE(gate);
			Drive d;
			d.s.order = static_cast<arp::Order>(order);
			d.s.gate = gate;
			d.s.octaves = 3;
			d.s.feel = 3;
			d.s.feelAmount = 0.4f;
			d.s.timeBase = 4; // 1/32
			d.apply();
			d.press({ 60, 63, 67, 70 });
			d.runTo(10000);
			d.release({ 63 });
			d.runTo(17777);
			d.press({ 62 });
			d.runTo(30000);
			endAndCheck(d, { 60, 67, 70, 62 });
		}
	}
	// A DEACTIVATE (the host silences its voices): nothing is left for the arp to end later.
	Drive d;
	d.s.gate = 2.0f;
	d.apply();
	d.press({ 60, 64 });
	d.runTo(9000);
	REQUIRE(d.a.soundingForTest() > 0);
	d.a.reset();
	CHECK(d.a.soundingForTest() == 0);
}

//------------------------------------------------------------------------------------------------//
// What it publishes and what it stores                                                           //
//------------------------------------------------------------------------------------------------//

TEST_CASE("arp: its sources are the step's place in the pattern, the note's pitch and its velocity") {
	Drive d;
	d.press({ 64 }, 0.25);
	d.press({ 60 }, 0.5);
	d.press({ 67 }, 0.75);
	d.runTo(1);
	CHECK(d.a.outputAt(arp::kStepOutput) == doctest::Approx(0.0f));
	CHECK(d.a.outputAt(arp::kPitchOutput) == doctest::Approx(60.0f / 127.0f));
	CHECK(d.a.outputAt(arp::kVelocityOutput) == doctest::Approx(0.5f));
	d.runTo(6001);
	CHECK(d.a.outputAt(arp::kStepOutput) == doctest::Approx(0.5f));
	CHECK(d.a.outputAt(arp::kPitchOutput) == doctest::Approx(64.0f / 127.0f));
	CHECK(d.a.outputAt(arp::kVelocityOutput) == doctest::Approx(0.25f));
	d.runTo(12001);
	CHECK(d.a.outputAt(arp::kStepOutput) == doctest::Approx(1.0f));
	CHECK(d.a.outputAt(arp::kPitchOutput) == doctest::Approx(67.0f / 127.0f));
	// FIXED VELOCITY replaces the played one, in the event and in the source.
	d.s.fixedVelocity = true;
	d.s.velocity = 0.3f;
	d.apply();
	d.runTo(18001);
	CHECK(d.a.outputAt(arp::kVelocityOutput) == doctest::Approx(0.3f));
	CHECK(d.ons().back().velocity == doctest::Approx(0.3));
	endAndCheck(d, { 64, 60, 67 });
}

TEST_CASE("arp: Latch's chord and the free clock's phase survive a save and a load into a fresh instance") {
	const device::StateOps& ops = *arp::kDescriptor.state;
	REQUIRE(ops.blocks.size() == 1);
	std::vector<float> block(arp::kPlayWords);

	Drive d;
	d.s.latch = true;
	d.apply();
	d.playing = false; // the free clock
	d.press({ 64, 60, 67 }, 0.6);
	d.release({ 64, 60, 67 });
	// TO A CHUNK START: the phase saved is the one at the last clock call, which the synth makes every <= 32 samples.
	d.runTo(6000 * 2 + 1216 + 1);
	d.a.tick(); // the publish a host harvests before a save
	REQUIRE(ops.read(&d.a, 0, block.data(), block.size()) != 0);
	const double phase = d.a.freePhaseForTest();
	CHECK(phase == doctest::Approx(2.0 + 1216.0 / 6000.0).epsilon(1e-9));

	// THE FRESH INSTANCE: same settings, the block restored, nothing played into it.
	Drive fresh;
	fresh.s = d.s;
	fresh.apply();
	fresh.playing = false;
	REQUIRE(ops.restore(&fresh.a, 0, block.data(), block.size()));
	CHECK(fresh.a.chordForTest() == d.a.chordForTest());
	REQUIRE(fresh.a.chordForTest().size() == 3);
	CHECK(fresh.a.chordForTest()[0].velocity == doctest::Approx(0.6f));
	// IT PLAYS ON FROM THE SAVED PHASE: the next step is 6000 − 1216 samples away, and it is slot 3.
	fresh.runTo(1);
	CHECK(fresh.a.freePhaseForTest() == phase); // to the bit: the block carries the double
	fresh.runTo(6000);
	REQUIRE(!fresh.ons().empty());
	CHECK(fresh.ons()[0].sample == 6000 - 1216);
	CHECK(fresh.ons()[0].key == 60); // slot 3 of Up 60, 64, 67

	SUBCASE("the control: a fresh instance with nothing restored plays nothing") {
		Drive bare;
		bare.s = d.s;
		bare.apply();
		bare.playing = false;
		bare.runTo(48000);
		CHECK(bare.log.empty());
	}
	SUBCASE("unlatched, the chord is not stored — it is the host's to send again") {
		Drive u;
		u.playing = false;
		u.press({ 60 });
		u.runTo(100);
		u.a.tick();
		REQUIRE(ops.read(&u.a, 0, block.data(), block.size()) != 0);
		Drive v;
		REQUIRE(ops.restore(&v.a, 0, block.data(), block.size()));
		CHECK(v.a.chordForTest().empty());
	}
}

//------------------------------------------------------------------------------------------------//
// In the synth                                                                                   //
//------------------------------------------------------------------------------------------------//

namespace {

struct SynthRig {
	feathers::ParamSet params;
	feathers::Synth synth { params };
	feathers::FxChain chain {};
	feathers::ModRack strip = feathers::defaultModRack();
	double bpm = 120.0;
	bool playing = true; ///< false: the host says stopped (its song position frozen where it was)

	SynthRig() { synth.activate(kRate, 4096); }

	void seat(std::initializer_list<feathers::DeviceRow> rows) {
		strip = feathers::defaultModRack();
		size_t i = 0;
		for (const feathers::DeviceRow& r : rows) {
			strip.device[i++] = r;
		}
		strip.count = i;
		synth.provisionDevices(chain, strip);
		synth.setModRack(strip);
		render(0, 64, 64); // the strip adopts its devices at a render
		synth.clearDeliveredForTest();
	}
	/// Renders [from, to) in `block`-sample host blocks, the song at beat 0 on sample `songZero`.
	void render(int64_t from, int64_t to, int block, int64_t songZero = 0) {
		std::vector<float> l(static_cast<size_t>(block)), r(static_cast<size_t>(block));
		synth.setTempo(bpm);
		for (int64_t at = from; at < to;) {
			const int n = static_cast<int>(std::min<int64_t>(block, to - at));
			const double beats = static_cast<double>(at - songZero) / kRate * bpm / 60.0;
			synth.setTransport(beats * 60.0 / bpm, beats, playing);
			synth.render(l.data(), r.data(), n);
			at += n;
		}
	}
};

feathers::DeviceRow arpRow(feathers::DeviceInstanceId id, const arp::Settings& s) {
	feathers::DeviceRow row = feathers::defaultRowFor(arp::kDescriptor, { id });
	const arp::Row fields = arp::rowOf(s);
	for (size_t i = 0; i < fields.size(); ++i) {
		row.field[i] = fields[i];
	}
	return row;
}

} // namespace

TEST_CASE("arp in the synth: its steps reach the voices on their own samples, whatever the host's block size") {
	for (const int block : { 64, 100, 512, 4096 }) {
		CAPTURE(block);
		SynthRig rig;
		rig.seat({ arpRow(60, arp::Settings {}) });
		const int64_t zero = static_cast<int64_t>(rig.synth.samplesRenderedForTest());
		rig.synth.noteOn(0, 60, 1, 1.0);
		rig.synth.noteOn(0, 64, 2, 1.0);
		rig.synth.noteOn(0, 67, 3, 1.0);
		rig.render(0, 6000 * 4 + 1, block, 0);
		std::vector<std::pair<int64_t, int>> ons;
		for (const auto& d : rig.synth.deliveredForTest()) {
			if (d.event.kind == Kind::On) {
				ons.emplace_back(static_cast<int64_t>(d.sample) - zero, d.event.key);
				CHECK(d.event.noteId == -1);
			}
		}
		const std::vector<std::pair<int64_t, int>> expect { { 0, 60 }, { 6000, 64 }, { 12000, 67 }, { 18000, 60 },
			                                                { 24000, 64 } };
		CHECK(ons == expect);
		// THE HELD CHORD ITSELF NEVER REACHED THE VOICES: one arp note sounds at a time at a 50% gate.
		CHECK(rig.synth.managedVoiceCount() <= 1);
		rig.synth.noteOff(0, 60, 1, 0.0);
		rig.synth.noteOff(0, 64, 2, 0.0);
		rig.synth.noteOff(0, 67, 3, 0.0);
		rig.render(6000 * 4 + 1, 6000 * 6, block, 0);
		std::map<int, int> balance;
		for (const auto& d : rig.synth.deliveredForTest()) {
			balance[d.event.key] += d.event.kind == Kind::On ? 1 : (d.event.kind == Kind::Off ? -1 : 0);
		}
		for (const auto& [key, n] : balance) {
			CHECK_MESSAGE(n == 0, "key " << key);
		}
		CHECK(rig.synth.managedVoiceCount() == 0);
	}
}

TEST_CASE("arp in the synth: a Tracker to its right hears its steps, one to its left hears the held chord") {
	// THE TRACKER'S OWN OUTPUT, read by value: (key − Root 60) / (Spread 2 × 12). Both follow Highest, so the left one,
	// hearing the chord, reads its top key; the right one, hearing one arp step at a time, reads that step.
	namespace trk = feathers::mod::tracker;
	SynthRig rig;
	feathers::DeviceRow left = feathers::defaultRowFor(trk::kDescriptor, { 70 });
	left.field[static_cast<size_t>(trk::Param::Rule)] = static_cast<float>(trk::Rule::Highest);
	feathers::DeviceRow right = feathers::defaultRowFor(trk::kDescriptor, { 71 });
	right.field[static_cast<size_t>(trk::Param::Rule)] = static_cast<float>(trk::Rule::Highest);
	rig.seat({ left, arpRow(72, arp::Settings {}), right });
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.synth.noteOn(0, 64, 2, 1.0);
	rig.synth.noteOn(0, 67, 3, 1.0);
	rig.render(0, 6000 + 128, 64, 0); // slot 1 sounding: 64 (slot 0's 60 ended at 3000)
	const feathers::LaneRack& lane = rig.synth.laneForTest();
	CHECK(lane.outputOf(70, 0) == doctest::Approx((67.0f - 60.0f) / 24.0f));
	CHECK(lane.outputOf(71, 0) == doctest::Approx((64.0f - 60.0f) / 24.0f));
	rig.render(6000 + 128, 12000 + 128, 64, 0); // slot 2: 67
	CHECK(lane.outputOf(71, 0) == doctest::Approx((67.0f - 60.0f) / 24.0f));
	rig.render(12000 + 128, 18000 + 128, 64, 0); // slot 3: 60, the only step sounding
	CHECK(lane.outputOf(71, 0) == doctest::Approx(0.0f));
	CHECK(lane.outputOf(70, 0) == doctest::Approx((67.0f - 60.0f) / 24.0f)); // still the chord's top
}

TEST_CASE("arp in the synth: with no arp, the clock call delivers nothing and asks for no cut") {
	SynthRig rig;
	rig.seat({});
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.render(0, 4096, 512, 0);
	const auto delivered = rig.synth.deliveredForTest();
	REQUIRE(delivered.size() == 1); // the host's own note, once
	CHECK(delivered[0].event.key == 60);
	CHECK(delivered[0].event.noteId == 1);
}

TEST_CASE("arp in the synth: the strip publishes its newest steps — the road the card's step lane reads") {
	// A HOST PUBLISHES A STRIP DEVICE'S RECENT EVENTS BESIDE ITS PLAYHEAD (`Synth::playheadOf`), so an arp with no
	// playhead would leave the card's lane empty in the product while every case on the device passed. Read here as the
	// bridge reads it: the published playhead's events, newest first, each its grid slot and key / 127.
	SynthRig rig;
	rig.seat({ arpRow(80, arp::Settings {}) });
	rig.synth.noteOn(0, 60, 1, 1.0);
	rig.synth.noteOn(0, 64, 2, 1.0);
	rig.synth.noteOn(0, 67, 3, 1.0);
	rig.render(0, 6000 * 4 + 64, 64, 0); // slots 0..4: 60, 64, 67, 60, 64
	const feathers::mod::Playhead where = rig.synth.rackPlayheads()[0];
	CHECK(where.ordinal == 4u);
	const std::vector<int> keys { 64, 60, 67, 64, 60 };
	for (size_t i = 0; i < keys.size(); ++i) {
		CAPTURE(i);
		CHECK(where.recent[i].ordinal == 4u - static_cast<uint32_t>(i));
		CHECK(std::lround(where.recent[i].value * 127.0f) == keys[i]);
	}
	rig.synth.noteOff(0, 60, 1, 0.0);
	rig.synth.noteOff(0, 64, 2, 0.0);
	rig.synth.noteOff(0, 67, 3, 0.0);
	rig.render(6000 * 4 + 64, 6000 * 6, 64, 0);
}

TEST_CASE("arp: a route into Time Base steps it through whole divisions — the engine resolves a whole index and the arp "
          "plays at that division") {
	// Time Base is a modulation destination. A Choice is stepped, so a modulation snaps:
	// read by value, the resolved field is a whole index at every macro position, the arp decodes it as that index,
	// and it moves away from the base when the macro is up.
	namespace backplane = feathers::backplane;
	constexpr backplane::Axes kAxes { .coreSources = 8, .coreDestinations = 0, .deviceSlots = 4, .outputStride = 4 };
	feathers::rack::RackEngine rack(kAxes, feathers::hostCatalogue());
	rack.prepare(kRate, 4096);
	const size_t slot = rack.addDevice(arp::kTraits.tag);
	REQUIRE(slot != feathers::rack::RackEngine::kNoSlot);
	const size_t control = arp::at(arp::Param::TimeBase);
	REQUIRE(arp::kDescriptor.params[control].modulatable);
	const backplane::Destination dest =
	    feathers::test::ordinalOf(rack, slot, arp::kDescriptor.nthModulatableOfControl(control));
	rack.setRouteAmount(feathers::test::macro(0), dest, 0.5f);
	std::vector<float> l(64, 0.0f), r(64, 0.0f);
	const float base = rack.fieldAt(slot, control);
	CHECK(base == static_cast<float>(arp::kDefaultTimeBase));
	std::vector<float> seen;
	for (const float macro : { 0.0f, 0.1f, 0.25f, 0.4f, 0.6f, 0.8f, 1.0f }) {
		CAPTURE(macro);
		rack.setMacro(0, macro);
		rack.processStereo(l.data(), r.data(), 64, backplane::unknownBlock());
		const float resolved = rack.resolvedFieldForTest(slot, control);
		CHECK(resolved == std::round(resolved)); // stepped: a whole index, never between two divisions
		CHECK(resolved >= 0.0f);
		CHECK(resolved <= static_cast<float>(arp::kTimeBaseCount - 1));
		const auto* dev = static_cast<const arp::Arp*>(rack.storageForTest(slot));
		REQUIRE(dev != nullptr);
		CHECK(dev->settingsForTest().timeBase == static_cast<size_t>(resolved)); // what the arp plays at
		seen.push_back(resolved);
	}
	CHECK(seen.front() == base);
	CHECK(seen.back() > base); // it moved, by whole steps
	CHECK(std::set<float>(seen.begin(), seen.end()).size() >= 3);
}

/// THE PINNED RUN'S VALUES. Captured at 22be7b40 before the clock moved (step 0 held them bit for bit); RE-PINNED
/// 2026-10-30 AS AN INTENDED CHANGE — two decisions: a tie into the same key holds one note, and the Feel (Swing 16
/// here) replaced the Swing knob.
constexpr size_t kPinnedEvents = 62;
constexpr size_t kPinnedCalls = 8153;
constexpr uint64_t kPinnedHash = 12850714829338137184ull;

TEST_CASE("arp: a long scripted run is pinned event for event — the guard for lifting the clock into a shared core") {
	// **A REFACTOR GUARD, NOT A SPEC** (terrain-arp design §20 step 0). The other cases restate the design; this one pins
	// what the classic arp DID before its clock moved into `StepEmitter`, across every road at once — orders, octaves,
	// swing, nudge, ties, latch, a scrub, a loop, a stop into the free clock, a Rate move, a disable — as one hash of
	// (kind, key, velocity bits, sample) and the count of clock calls. A deliberate change to the arp's behaviour
	// changes the hash; a refactor must not.
	Drive d;
	d.s.order = arp::Order::UpDownRepeat;
	d.s.octaves = 2;
	d.s.gate = 1.3f;
	d.s.feel = 3; // Swing 16, which replaced the Swing knob (2026-10-30)
	d.s.feelAmount = 0.4f;
	d.s.nudge = 0.1f;
	d.apply();
	d.press({ 60, 64, 67 }, 0.7);
	d.runTo(48000);
	d.locate(13.37); // a scrub
	d.runTo(72000);
	d.s.order = arp::Order::Random;
	d.s.seed = 42;
	d.s.latch = true;
	d.apply();
	d.release({ 60, 64, 67 });
	d.runTo(96000);
	d.locate(2.0); // a loop back
	d.runTo(120000);
	d.playing = false; // the transport stops under the latched chord: the free clock carries on
	d.runTo(150000);
	d.s.rate = 0.5f; // a Rate move on the free clock
	d.s.order = arp::Order::Played;
	d.s.fixedVelocity = true;
	d.apply();
	d.runTo(180000);
	d.s.enabled = false;
	d.apply();
	d.runTo(190000);
	d.s.enabled = true;
	d.s.latch = false;
	d.apply();
	d.press({ 62, 69 }, 0.5);
	d.playing = true;
	d.locate(40.0);
	d.runTo(230000);
	d.release({ 62, 69 });
	d.runTo(260000);
	uint64_t h = 1469598103934665603ull;
	const auto mix = [&](uint64_t v) {
		for (int b = 0; b < 8; ++b) {
			h = (h ^ ((v >> (8 * b)) & 0xffu)) * 1099511628211ull;
		}
	};
	for (const Logged& l : d.all) {
		mix(static_cast<uint64_t>(l.kind));
		mix(static_cast<uint64_t>(static_cast<uint16_t>(l.key)));
		uint64_t bits = 0;
		std::memcpy(&bits, &l.velocity, sizeof bits);
		mix(bits);
		mix(static_cast<uint64_t>(l.sample));
	}
	MESSAGE("events " << d.all.size() << " calls " << d.calls.size() << " hash " << h);
	checkBalanced(d.all);
	CHECK(d.all.size() == kPinnedEvents);
	CHECK(d.calls.size() == kPinnedCalls);
	CHECK(h == kPinnedHash);
}

TEST_CASE("arp core: the grid's per-slot shift moves exactly its slot, and firstAtOrAfter still finds the first onset") {
	// The Pattern device's Humanize road (terrain-arp design §12): onset(g) = g + nudge + swing/2 on odd slots + shift(g).
	// Restated here rather than read back: a null hook is the classic grid bit for bit.
	const auto shift = [](const void*, int64_t g) { return (g % 3 == 0) ? -0.4 : ((g % 3 == 1) ? 0.25 : 0.0); };
	arp::StepGrid plain { 0.1, 0.4 };
	arp::StepGrid shifted { 0.1, 0.4 };
	shifted.shift = shift;
	for (int64_t g = -7; g < 40; ++g) {
		CAPTURE(g);
		const double base = static_cast<double>(g) + 0.1 + ((g % 2) == 0 ? 0.0 : 0.2);
		CHECK(plain.onset(g) == base);
		CHECK(shifted.onset(g) == base + shift(nullptr, g));
	}
	// The first onset at or after u, found by brute force over a wide window, for u across the shifted onsets.
	for (int i = -200; i < 2000; ++i) {
		const double u = static_cast<double>(i) * 0.0173;
		int64_t want = -100;
		while (shifted.onset(want) < u - 1e-9) {
			++want;
		}
		CAPTURE(u);
		CHECK(shifted.firstAtOrAfter(u, 1e-9) == want);
	}
}

TEST_CASE("arp: a held same-key tie leaves no note sounding on a stop, a switch-off or a chord change mid-tie") {
	SUBCASE("the transport stops under Stop with Transport") {
		Drive d;
		d.s.gate = 1.8f;
		d.s.run = arp::RunMode::Transport;
		d.apply();
		d.press({ 60 });
		d.runTo(6000 * 3 + 2000);
		d.playing = false;
		d.runTo(6000 * 5);
		checkBalanced(d.all);
		CHECK(d.a.soundingForTest() == 0);
	}
	SUBCASE("switched off mid-tie") {
		Drive d;
		d.s.gate = 1.8f;
		d.apply();
		d.press({ 60 });
		d.runTo(6000 * 2 + 3000);
		d.s.enabled = false;
		d.apply();
		d.runTo(6000 * 4);
		CHECK(d.a.soundingForTest() == 0);
		checkBalanced(d.all); // (a release now would pass straight through, as the arp is off)
	}
	SUBCASE("a chord change mid-tie: the held key runs to its end, the new keys overlap, nothing is left") {
		Drive d;
		d.s.gate = 1.8f;
		d.apply();
		d.press({ 60 });
		d.runTo(6000 * 2 + 3000);
		d.press({ 64, 67 });
		d.runTo(6000 * 6);
		d.release({ 60 });
		d.runTo(6000 * 8);
		endAndCheck(d, { 64, 67 });
		checkBalanced(d.all);
	}
}

TEST_CASE("arp: every Feel lands each step on its slot plus the shared Humanize shift, restated per slot") {
	namespace hz = feathers::mod::arp::humanize;
	for (size_t feel = 1; feel < arp::kFeelCount; ++feel) {
		Drive d;
		d.s.feel = feel;
		d.s.feelAmount = 0.8f;
		d.s.freePhase = 2.63;
		d.apply();
		d.press({ 60 });
		d.runTo(6000 * 33);
		const auto& held = d.a.settingsForTest();
		// RESTATED FROM THE FIELDS: the mode, Amount the depth, and Free Phase the position (backlog QA).
		const hz::Feel f { arp::Arp::feelModeOf(held.feel), static_cast<double>(held.feelAmount), 0, held.freePhase };
		const std::vector<int64_t> got = d.onSamples();
		REQUIRE(got.size() >= 32);
		CAPTURE(arp::kFeelNames[feel]);
		for (int64_t g = 0; g < 32; ++g) {
			const double at = static_cast<double>(g) + hz::shiftSteps(f, g, 0.125, 0.25);
			CAPTURE(g);
			CHECK(got[static_cast<size_t>(g)] == static_cast<int64_t>(std::ceil(at * 6000.0 - 1e-6)));
		}
		endAndCheck(d, { 60 });
	}
}

TEST_CASE("arp: the Feel list IS SKLITTER's groove list — the same ten names in the same order, one implementation") {
	REQUIRE(arp::kFeelCount == std::size(feathers::fx::sklitter::kGrooveModeNames));
	for (size_t i = 0; i < arp::kFeelCount; ++i) {
		CAPTURE(i);
		CHECK(std::string(arp::kFeelNames[i]) == feathers::fx::sklitter::kGrooveModeNames[i]);
		CHECK(std::string(feathers::mod::arp::humanize::kModeNames[i]) == feathers::fx::sklitter::kGrooveModeNames[i]);
	}
}

TEST_CASE("arp: Only w/ Transport is On by default — with the transport stopped, a held chord plays nothing until it plays") {
	// Only w/ Transport defaults to on.
	CHECK(arp::Settings {}.run == arp::RunMode::Transport);
	CHECK(arp::kParamDescs[arp::at(arp::Param::Run)].def == 1.0); // Transport
	Drive d;
	d.s = arp::Settings {};
	d.apply();
	d.playing = false;
	d.note(Kind::On, 60);
	d.runTo(4 * 6000);
	CHECK(d.ons().empty());
}

TEST_CASE("step emitter: a key the arp took before Thru came to name it is released as the arp's own — its off neither passes nor leaves it held") {
	namespace a = feathers::mod::arp;
	using K = feathers::device::NoteEvent::Kind;
	a::StepEmitter e;
	e.prepare(48000.0);
	a::ClockSettings c; // Thru off, Gate: every key is a trigger
	e.setSettings(c);
	const auto send = [&](K kind, int key) {
		feathers::device::NoteEvent ev {};
		ev.kind = kind;
		ev.noteId = -1;
		ev.key = static_cast<int16_t>(key);
		ev.velocity = 0.9;
		std::array<feathers::device::NoteEvent, 4> out {};
		size_t n = 0;
		e.processNotes(&ev, 1, out.data(), out.size(), &n);
		return n;
	};
	CHECK(send(K::On, 60) == 0); // taken by the arp
	CHECK(send(K::On, 64) == 0);
	REQUIRE(e.chord().size() == 2);
	// THRU NOW NAMES 60 (a switch to a pattern with Thru under the held keys).
	c.thru = true;
	c.thruKeys[0] = uint64_t { 1 } << 60;
	e.setSettings(c);
	CHECK(send(K::Off, 60) == 0); // not passed: the synth never had an on for it
	REQUIRE(e.chord().size() == 1); // and released from the chord, by value: only 64 is held
	CHECK(e.chord()[0].key == 64);
	// A KEY THRU NAMES FROM ITS ON passes both ways and never reaches the chord.
	CHECK(send(K::On, 60) == 1);
	CHECK(e.chord().size() == 1);
	CHECK(send(K::Off, 60) == 1);
	CHECK(send(K::Off, 64) == 0);
	CHECK(e.chord().empty());
}

TEST_CASE("arp Free: the walk reads Free Phase — unbounded, never Feel Amount — as the Pattern device's does") {
	namespace hz = feathers::mod::arp::humanize;
	const size_t free16 = static_cast<size_t>(hz::Mode::Free16);
	const auto onsets = [&](double phase, float amount) {
		Drive d;
		d.s.feel = free16;
		d.s.feelAmount = amount;
		d.s.freePhase = phase;
		d.apply();
		d.press({ 60 });
		d.runTo(6000 * 33);
		std::vector<int64_t> got = d.onSamples();
		REQUIRE(got.size() >= 32);
		got.resize(32);
		// THE POSITION RESTATED FROM THE VALUE AS SET (through a float field).
		const hz::Feel f { hz::Mode::Free16, 0.0, 0, static_cast<double>(static_cast<float>(phase)) };
		for (int64_t g = 0; g < 32; ++g) {
			CAPTURE(g);
			const double at = static_cast<double>(g) + hz::shiftSteps(f, g, 0.125, 0.25);
			CHECK(got[static_cast<size_t>(g)] == static_cast<int64_t>(std::ceil(at * 6000.0 - 1e-6)));
		}
		endAndCheck(d, { 60 });
		return got;
	};
	const auto a = onsets(5.3, 0.1f);
	CHECK(onsets(5.3, 0.9f) == a); // Feel Amount is not Free's
	CHECK(onsets(5.35, 0.1f) != a); // the phase moves it
	CHECK(onsets(0.3, 0.5f) != onsets(1.3, 0.5f)); // a whole unit on is another groove: no wrap
	// HONESTY: Amount is not drawn in Free; Free Phase only there (Fine retired); the two arps' descriptors agree.
	namespace pat = feathers::mod::arp::pat;
	CHECK((arp::kParamDescs[arp::at(arp::Param::FeelAmount)].gateMask & hz::kFreeModes) == 0u);
	CHECK(arp::kParamDescs[arp::at(arp::Param::FreePhase)].gateMask == hz::kFreeModes);
	CHECK(std::string(arp::kParamDescs[arp::at(arp::Param::FreePhase)].key) == "free_phase");
	CHECK(arp::kParamDescs[arp::at(arp::Param::FreePhase)].max == pat::kParamDescs[pat::at(pat::Global::FreePhase)].max);
}

TEST_CASE("arp Wobble: each step's onset, velocity and length are humanize's wobble at its place in the bar; a gate over 100% (a tie) is never clipped") {
	namespace hz = feathers::mod::arp::humanize;
	namespace wb = hz::wobble;
	for (const float gate : { 0.6f, 1.8f }) {
		CAPTURE(gate);
		Drive d;
		d.s.wobble = 4.44;
		d.s.gate = gate;
		d.s.fixedVelocity = true;
		d.s.velocity = 0.7f;
		d.apply();
		d.press({ 60, 64, 67 });
		d.runTo(6000 * 33);
		const double b = d.a.settingsForTest().wobble;
		REQUIRE(b == static_cast<double>(4.44f));
		const auto onsetOf = [&](int64_t g) { return static_cast<double>(g) + wb::shiftSteps(b, static_cast<int32_t>(g % 16), 16); };
		std::vector<Logged> ons = d.ons();
		REQUIRE(ons.size() >= 32);
		int moved = 0;
		for (int64_t g = 0; g < 30; ++g) {
			CAPTURE(g);
			const Logged& on = ons[static_cast<size_t>(g)];
			CHECK(on.sample == static_cast<int64_t>(std::ceil(onsetOf(g) * 6000.0 - 1e-6)));
			CHECK(on.velocity == doctest::Approx(std::clamp(0.7 * wb::velocity(b, static_cast<int32_t>(g % 16)), 0.0, 1.0)).epsilon(1e-6));
			moved += on.sample != g * 6000 ? 1 : 0;
			if (gate <= 1.0f) {
				const double clipped = 0.6 * wb::length(b, static_cast<int32_t>(g % 16));
				const double off = onsetOf(g) + clipped * (onsetOf(g + 1) - onsetOf(g));
				int64_t got = -1;
				for (const Logged& l : d.all) {
					if (l.kind == Kind::Off && l.key == on.key && l.sample > on.sample) {
						got = l.sample;
						break;
					}
				}
				CHECK(std::llabs(got - static_cast<int64_t>(std::ceil(off * 6000.0 - 1e-6))) <= 1);
			}
			else {
				// A TIE: its off lands past the next step's onset — not clipped back inside its own step.
				int64_t got = -1;
				for (const Logged& l : d.all) {
					if (l.kind == Kind::Off && l.key == on.key && l.sample > on.sample) {
						got = l.sample;
						break;
					}
				}
				if (got >= 0) {
					CHECK(got > static_cast<int64_t>(std::ceil(onsetOf(g + 1) * 6000.0 - 1e-6)));
				}
			}
		}
		CHECK(moved > 10);
		endAndCheck(d, { 60, 64, 67 });
	}
}

TEST_CASE("arp in the synth: Pattern and the classic Arp leave no voice sounding when the transport stops, a preset reloads "
          "the strip, or the device leaves (Pattern must always send the note off)") {
	// THE SYNTH'S LANE ROAD (`Synth::emitLaneNotes`), not the rack plugin's: what an arp made goes straight to the voices,
	// so a missing off is a voice that never ends. Gate at its longest, so a note is sounding at the event.
	namespace pt = arp::pat;
	const auto patternRow = [](feathers::DeviceInstanceId id) {
		feathers::DeviceRow row = feathers::defaultRowFor(pt::kDescriptor, { id });
		row.field[pt::at(0, pt::Field::Gate)] = 1.0f;
		return row;
	};
	const auto classicRow = [](feathers::DeviceInstanceId id) {
		arp::Settings s;
		s.gate = 1.0f;
		return arpRow(id, s);
	};
	for (const bool pattern : { true, false }) {
		CAPTURE(pattern);
		const auto row = [&](feathers::DeviceInstanceId id) { return pattern ? patternRow(id) : classicRow(id); };
		const auto run = [&](auto&& event) {
			SynthRig rig;
			rig.seat({ row(80) });
			rig.synth.noteOn(0, 48, 1, 1.0);
			rig.synth.noteOn(0, 55, 2, 1.0);
			int64_t at = 0;
			while (rig.synth.managedVoiceCount() == 0 && at < 48000) {
				rig.render(at, at + 256, 256);
				at += 256;
			}
			REQUIRE(rig.synth.managedVoiceCount() > 0);
			event(rig, at);
			rig.render(at, at + 4096, 256);
			at += 4096;
			rig.synth.noteOff(0, 48, 1, 0.0);
			rig.synth.noteOff(0, 55, 2, 0.0);
			rig.playing = false; // and nothing plays on after the keys are let go
			rig.render(at, at + 48000, 256);
			std::map<int, int> balance;
			for (const auto& d : rig.synth.deliveredForTest()) {
				balance[d.event.key] += d.event.kind == Kind::On ? 1 : (d.event.kind == Kind::Off ? -1 : 0);
			}
			for (const auto& [key, n] : balance) {
				CHECK_MESSAGE(n <= 0, "key " << key << " left sounding");
			}
			CHECK(rig.synth.managedVoiceCount() == 0);
		};
		// NOT SUBCASEs: doctest enters a SUBCASE inside a loop on the first pass only.
		{
			INFO("the transport stops");
			run([](SynthRig& rig, int64_t) { rig.playing = false; });
		}
		{
			INFO("a preset reloads the same strip (a new instance under the same id)");
			run([&](SynthRig& rig, int64_t) {
				feathers::ModRack again = feathers::defaultModRack();
				again.device[0] = row(80);
				again.count = 1;
				rig.synth.provisionDevices(rig.chain, again);
				rig.synth.setModRack(again);
			});
		}
		{
			INFO("a preset with no arp replaces the strip");
			run([&](SynthRig& rig, int64_t) {
				const feathers::ModRack none = feathers::defaultModRack();
				rig.synth.provisionDevices(rig.chain, none);
				rig.synth.setModRack(none);
			});
		}
		{
			INFO("a different arp kind under the same id");
			run([&](SynthRig& rig, int64_t) {
				feathers::ModRack other = feathers::defaultModRack();
				other.device[0] = pattern ? classicRow(80) : patternRow(80);
				other.count = 1;
				rig.synth.provisionDevices(rig.chain, other);
				rig.synth.setModRack(other);
			});
		}
	}
}

//------------------------------------------------------------------------------------------------//
// Run: Note (starts on a new note and plays as long as the note is held)                     //
//------------------------------------------------------------------------------------------------//

TEST_CASE("arp: Run Note — the first key starts step 1 on its own sample at the song's tempo whatever the transport, legato "
          "carries on, the last release stops it, and the next key starts over") {
	for (const bool playing : { true, false }) {
		CAPTURE(playing);
		Drive d;
		d.s.run = arp::RunMode::Note;
		d.apply();
		d.playing = playing;
		d.locate(3.3); // off the song's grid: Note does not look at it
		d.runTo(1234);
		// THE FIRST KEY: step 1 (the pattern's first note) lands on its sample, then every 6000 (1/16 at 120 bpm).
		d.press({ 60, 64 });
		d.runTo(1234 + 6000 * 3 + 1);
		CHECK(d.onSamples() == std::vector<int64_t> { 1234, 7234, 13234, 19234 });
		CHECK(d.onKeys() == std::vector<int> { 60, 64, 60, 64 });
		// LEGATO: a key added and the first let go with one still down — the clock carries on, nothing restarts.
		d.log.clear();
		d.press({ 67 });
		d.release({ 60 });
		d.runTo(1234 + 6000 * 6 + 1);
		CHECK(d.onSamples() == std::vector<int64_t> { 25234, 31234, 37234 });
		// THE LAST RELEASE STOPS IT: its notes end, nothing more plays.
		d.release({ 64, 67 });
		d.runTo(d.now + 48000);
		CHECK(d.a.soundingForTest() == 0);
		const size_t onsAfter = d.ons().size();
		d.runTo(d.now + 48000);
		CHECK(d.ons().size() == onsAfter);
		// AND THE NEXT KEY STARTS OVER: step 1 on its own sample.
		d.log.clear();
		const int64_t again = d.now + 555;
		d.runTo(again);
		d.press({ 72, 76 });
		d.runTo(again + 6000 + 1);
		CHECK(d.onSamples() == std::vector<int64_t> { again, again + 6000 });
		CHECK(d.onKeys() == std::vector<int> { 72, 76 });
		endAndCheck(d, { 72, 76 });
	}
}

TEST_CASE("arp: Run Note with Latch — the latched chord plays on after the keys are let go, and a fresh chord starts over from step 1 on its own sample") {
	Drive d;
	d.s.run = arp::RunMode::Note;
	d.s.latch = true;
	d.apply();
	d.press({ 60, 64 });
	d.runTo(6000 * 2 + 10);
	d.release({ 60, 64 });
	d.runTo(6000 * 4 + 10);
	CHECK(d.ons().size() == 5); // latched: it plays on
	d.log.clear();
	const int64_t fresh = 6000 * 4 + 2500; // off the running grid
	d.runTo(fresh);
	d.press({ 67, 71 });
	d.runTo(fresh + 6000 + 1);
	CHECK(d.onSamples() == std::vector<int64_t> { fresh, fresh + 6000 });
	CHECK(d.onKeys() == std::vector<int> { 67, 71 }); // the new chord, from its first step
	d.s.latch = false; // Latch off under the held chord, then the keys let go (once: a second off would pass on, QU)
	d.apply();
	endAndCheck(d, { 67, 71 });
}

TEST_CASE("arp: a patch that stored the retired Only w/ Transport switch loads as Run — Off as Always, On as Transport; one that names Run keeps it") {
	// `RetiredKey::carriedInto` (the old key is retired and migrated): the switch's labels read through Run's aliases.
	const auto load = [](std::initializer_list<std::pair<const char*, const char*>> labels) {
		feathers::rack::RackPatch::Device dev;
		for (const auto& [key, label] : labels) {
			feathers::rack::RackPatch::Value v;
			v.key = key;
			v.kind = feathers::rack::RackPatch::Value::Kind::Label;
			v.text = label;
			dev.values.push_back(v);
		}
		std::vector<float> fields(arp::kDescriptor.params.size(), 0.0f);
		feathers::rack::LoadReport report;
		(void)feathers::rack::rowOfValues(arp::kDescriptor, dev, std::span<float>(fields.data(), fields.size()), &report, "arp",
		                                  nullptr);
		return fields[arp::at(arp::Param::Run)];
	};
	CHECK(load({ { "stop_with_transport", "Off" } }) == static_cast<float>(arp::RunMode::Always));
	CHECK(load({ { "stop_with_transport", "On" } }) == static_cast<float>(arp::RunMode::Transport));
	CHECK(load({ { "run", "Note" } }) == static_cast<float>(arp::RunMode::Note));
	// THE NEW KEY WINS where a patch names both, in either order.
	CHECK(load({ { "stop_with_transport", "Off" }, { "run", "Note" } }) == static_cast<float>(arp::RunMode::Note));
	CHECK(load({ { "run", "Note" }, { "stop_with_transport", "Off" } }) == static_cast<float>(arp::RunMode::Note));
	CHECK(load({}) == static_cast<float>(arp::RunMode::Transport)); // neither: the default
}

TEST_CASE("pattern in the synth, Keys Transpose: a new key moves the notes from the next step — legato or re-struck, Latch off or on, under every Run (backlog QR)") {
	// THE SYNTH'S NOTE LANE (`Synth::emitLaneNotes`), by value: each step's key, indexed by its slot from the clock's origin,
	// after the switch from A (60) to B (64) is the same slot's key under A plus 4. The root is the NEWEST press (design §7.7,
	// backlog QR), so B rules from its press whether A is let go, still down, or held by Latch.
	namespace pt = arp::pat;
	constexpr int kStep = 6000; // 1/16 at 120 bpm, 48 kHz
	constexpr int kA = 60;
	constexpr int kB = 64;
	struct Variant {
		const char* name;
		bool legato;
		bool latch;
		arp::RunMode run;
	};
	const Variant variants[] = {
		{ "re-struck, Transport", false, false, arp::RunMode::Transport },
		{ "legato, Transport", true, false, arp::RunMode::Transport },
		{ "re-struck, Always", false, false, arp::RunMode::Always },
		{ "legato, Always", true, false, arp::RunMode::Always },
		{ "re-struck, Note", false, false, arp::RunMode::Note },
		{ "legato, Note", true, false, arp::RunMode::Note },
		{ "re-struck, Latch, Transport", false, true, arp::RunMode::Transport },
		{ "legato, Latch, Transport", true, true, arp::RunMode::Transport },
		{ "re-struck, Latch, Note", false, true, arp::RunMode::Note },
	};
	for (const Variant& v : variants) {
		const std::string variant = v.name;
		CAPTURE(variant);
		SynthRig rig;
		feathers::DeviceRow row = feathers::defaultRowFor(pt::kDescriptor, { 81 });
		row.field[pt::at(pt::Global::Latch)] = v.latch ? 1.0f : 0.0f;
		row.field[pt::at(pt::Global::Run)] = static_cast<float>(v.run);
		rig.seat({ row });
		const auto zero = static_cast<int64_t>(rig.synth.samplesRenderedForTest());
		rig.synth.noteOn(0, kA, 1, 1.0);
		rig.render(0, 32 * kStep, 64, 0); // two cycles under A
		// THE SWITCH, mid-step before slot 48 (a cycle boundary).
		const int64_t at = 48 * kStep - kStep / 2;
		rig.render(32 * kStep, at, 64, 0);
		if (v.legato) {
			rig.synth.noteOn(0, kB, 2, 1.0);
			rig.render(at, at + 64, 64, 0);
			rig.synth.noteOff(0, kA, 1, 0.0);
		}
		else {
			rig.synth.noteOff(0, kA, 1, 0.0);
			rig.render(at, at + 64, 64, 0);
			rig.synth.noteOn(0, kB, 2, 1.0);
		}
		const int64_t pressB = at + 64; // in the render's own samples, as `s` below is
		rig.render(at + 64, 96 * kStep, 64, 0);
		// THE SLOTS: under A from 0 (the song, and Note's first key at 0); under B from 48, or — Note re-struck — from B's press.
		const bool restart = v.run == arp::RunMode::Note && !v.legato;
		std::map<int64_t, int> underA;
		std::map<int64_t, int> underB;
		for (const auto& d : rig.synth.deliveredForTest()) {
			if (d.event.kind != Kind::On) {
				continue;
			}
			const int64_t s = static_cast<int64_t>(d.sample) - zero;
			if (s < 32 * kStep) {
				if (s % kStep == 0) {
					underA[s / kStep] = d.event.key;
				}
			}
			else if (restart ? s >= pressB : s >= 48 * kStep) {
				const int64_t rel = restart ? s - pressB : s - 48 * kStep;
				if (rel % kStep == 0) {
					underB[rel / kStep] = d.event.key;
				}
			}
		}
		REQUIRE(underA.size() >= 8);
		size_t compared = 0;
		for (const auto& [slot, key] : underB) {
			const auto a = underA.find(slot % 32);
			if (a == underA.end()) {
				continue;
			}
			CAPTURE(slot);
			CHECK(key == a->second + (kB - kA));
			++compared;
		}
		CHECK(compared >= 8); // not vacuous: B played, on the slots A played
	}
}

TEST_CASE("pattern Keys Transpose: the newest press is the root — a chord struck at once spells from its lowest key; a key added over a held one moves the pattern to it; letting it go returns to the one still held (backlog QR)") {
	namespace pt = arp::pat;
	constexpr int kStep = 6000;
	// THE STEP KEYS OF ONE CYCLE (slots 16..31), after `press` (at 0) and `then` (just before slot 16), on the synth's lane.
	const auto cycleAfter = [&](std::initializer_list<int> press, std::initializer_list<int> add, std::initializer_list<int> release) {
		SynthRig rig;
		rig.seat({ feathers::defaultRowFor(pt::kDescriptor, { 82 }) });
		const auto zero = static_cast<int64_t>(rig.synth.samplesRenderedForTest());
		int id = 1;
		for (const int k : press) {
			rig.synth.noteOn(0, static_cast<int16_t>(k), id++, 1.0);
		}
		rig.render(0, 16 * kStep - kStep / 2, 64, 0);
		for (const int k : add) {
			rig.synth.noteOn(0, static_cast<int16_t>(k), id++, 1.0);
		}
		rig.render(16 * kStep - kStep / 2, 16 * kStep - kStep / 2 + 64, 64, 0);
		for (const int k : release) {
			rig.synth.noteOff(0, static_cast<int16_t>(k), 0, 0.0);
		}
		rig.render(16 * kStep - kStep / 2 + 64, 32 * kStep, 64, 0);
		std::map<int64_t, int> keys;
		for (const auto& d : rig.synth.deliveredForTest()) {
			const int64_t s = static_cast<int64_t>(d.sample) - zero;
			if (d.event.kind == Kind::On && s >= 16 * kStep && s < 32 * kStep && s % kStep == 0) {
				keys[s / kStep - 16] = d.event.key;
			}
		}
		return keys;
	};
	const std::map<int64_t, int> alone60 = cycleAfter({ 60 }, {}, {});
	REQUIRE(alone60.size() >= 4);
	const auto shifted = [](const std::map<int64_t, int>& m, int by) {
		std::map<int64_t, int> out;
		for (const auto& [slot, key] : m) {
			out[slot] = key + by;
		}
		return out;
	};
	// A CHORD STRUCK AT ONCE: one press, its lowest key the root — as 60 alone.
	CHECK(cycleAfter({ 64, 60, 67 }, {}, {}) == alone60);
	// A KEY ADDED OVER A HELD ONE (no Latch): the pattern moves to it (the change QR makes: it stayed on the lowest).
	CHECK(cycleAfter({ 60 }, { 64 }, {}) == shifted(alone60, 4));
	// LETTING THE NEWEST GO RETURNS TO THE ONE STILL HELD.
	CHECK(cycleAfter({ 60 }, { 64 }, { 64 }) == alone60);
	// A LOWER KEY ADDED moves it down too: newest, not lowest.
	CHECK(cycleAfter({ 60 }, { 55 }, {}) == shifted(alone60, -5));
}

TEST_CASE("a key already sounding through the lane when an arp arrives: its off still reaches the voices — the classic Arp and Pattern, a plain off and a wildcard one (backlog QU)") {
	// THE ROAD THE FIRST BLOCK TAKES (test_clap_render's repro): the key reaches the voices through an EMPTY lane, then the
	// device is installed, then the off comes. The lane seeds the new device with the keys sounding through it, so the off
	// goes on — with the on's own note id where the host's is a wildcard.
	namespace pt = arp::pat;
	const auto offsFor = [](const SynthRig& rig, int key) {
		std::vector<int32_t> ids;
		for (const auto& d : rig.synth.deliveredForTest()) {
			if (d.event.kind == Kind::Off && d.event.key == key) {
				ids.push_back(d.event.noteId);
			}
		}
		return ids;
	};
	for (const bool pattern : { false, true }) {
		for (const int32_t offId : { 7, -1 }) {
			CAPTURE(pattern);
			CAPTURE(offId);
			SynthRig rig;
			rig.render(0, 64, 64); // the lane empty
			rig.synth.noteOn(0, 60, 7, 1.0);
			rig.render(64, 128, 64);
			bool passed = false;
			for (const auto& d : rig.synth.deliveredForTest()) {
				passed = passed || (d.event.kind == Kind::On && d.event.key == 60 && d.event.noteId == 7);
			}
			REQUIRE(passed); // the voices have it
			rig.seat({ pattern ? feathers::defaultRowFor(pt::kDescriptor, { 83 }) : arpRow(83, arp::Settings {}) });
			rig.render(128, 6000, 64);
			rig.synth.noteOff(0, 60, offId, 0.0);
			rig.render(6000, 12000, 64);
			const std::vector<int32_t> offs = offsFor(rig, 60);
			REQUIRE(offs.size() == 1);
			CHECK(offs[0] == 7); // the on's own id, whatever the host's off carried
			CHECK(rig.synth.managedVoiceCount() == 0);
		}
	}
}

TEST_CASE("a wildcard off for a key that passed before Pattern arrived carries the on's own id, never a wildcard (backlog QU)") {
	namespace pt = arp::pat;
	SynthRig rig;
	rig.render(0, 64, 64);
	rig.synth.noteOn(0, 60, 7, 1.0); // through the empty lane: a voice with the host's id
	rig.render(64, 128, 64);
	feathers::DeviceRow row = feathers::defaultRowFor(pt::kDescriptor, { 84 });
	row.field[pt::at(0, pt::Field::Trig)] = 1.0f; // Transport: it plays its idle chord, rooted on C4 (60), with no key held
	row.field[pt::at(0, pt::Field::Gate)] = 2.0f; // long notes, so one is sounding when the off comes
	row.field[pt::at(0, pt::Field::Length)] = 1.0f; // one step: every step its root, 60
	rig.seat({ row });
	rig.render(128, 6000 * 4 + 100, 64, 0);
	// PATTERN IS SOUNDING ITS OWN 60 (no id), beside the host's (id 7).
	bool own = false;
	for (const auto& d : rig.synth.deliveredForTest()) {
		own = own || (d.event.kind == Kind::On && d.event.key == 60 && d.event.noteId < 0);
	}
	REQUIRE(own);
	// ONE VOICE FOR KEY 60 NOW: a same-key on takes the voice the host's held (the synth's rule), so the host's id 7 sounds
	// nowhere and Pattern's 60 is what sounds. A wildcard off passed on as it came would end it.
	const size_t before = rig.synth.managedVoiceCount();
	REQUIRE(before >= 1);
	rig.synth.noteOff(0, 60, -1, 0.0); // the host's off, a wildcard id
	rig.render(6000 * 4 + 100, 6000 * 4 + 228, 64, 0);
	std::vector<int32_t> offIds;
	for (const auto& d : rig.synth.deliveredForTest()) {
		if (d.event.kind == Kind::Off && d.event.key == 60 && d.sample >= static_cast<uint64_t>(6000 * 4 + 100)) {
			offIds.push_back(d.event.noteId);
		}
	}
	REQUIRE(offIds.size() == 1);
	CHECK(offIds[0] == 7); // the host's voice, by its own id — never a wildcard off for every key-60 voice
	// NOT ASSERTED, and why: sst-voicemanager matches an off's id only against a voice that HAS one, and the arp's own notes
	// carry none (−1), so even an off with id 7 ends Pattern's key-60 voice. Pattern's note surviving needs its notes to
	// carry ids of their own: open in backlog QU.
	(void)before;
}

TEST_CASE("Mono and Legato: a key that passed before Pattern arrived leaves no stale key in the voice's note stack — when Pattern's notes end, the voice does not fall back to it (backlog QU)") {
	// A MONO VOICE KEEPS A STACK OF HELD KEYS (last-note priority): a passed key whose off was swallowed stayed in it, and
	// when the arp's own notes released the voice fell back to the stale key — the old note "returning", which reads as a
	// new key not transposing. With the off passed on, the stack empties and the voice goes quiet.
	namespace pt = arp::pat;
	for (const feathers::VoiceMode mode : { feathers::VoiceMode::Mono, feathers::VoiceMode::Legato }) {
		CAPTURE(static_cast<int>(mode));
		SynthRig rig;
		rig.params.setBase(static_cast<clap_id>(feathers::ParamId::VoiceMode), static_cast<double>(mode));
		rig.render(0, 64, 64); // the lane empty
		rig.synth.noteOn(0, 60, 7, 1.0); // passes through to the voices
		rig.render(64, 128, 64);
		feathers::DeviceRow row = feathers::defaultRowFor(pt::kDescriptor, { 85 });
		rig.seat({ row });
		rig.synth.noteOn(0, 64, 8, 1.0); // a key Pattern takes: it plays over it
		rig.render(128, 6000 * 8, 64, 0);
		bool played = false;
		for (const auto& d : rig.synth.deliveredForTest()) {
			played = played || (d.event.kind == Kind::On && d.event.noteId < 0);
		}
		REQUIRE(played); // Pattern's own steps reached the mono voice
		rig.synth.noteOff(0, 60, 7, 0.0); // the passed key let go
		rig.synth.noteOff(0, 64, 8, 0.0); // and Pattern's key: its notes end
		rig.render(6000 * 8, 6000 * 16, 64, 0);
		CHECK(rig.synth.managedVoiceCount() == 0); // no fall-back to a stale 60: nothing sounds
	}
}
