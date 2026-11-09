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

#include "fx/delay/delay.h"
#include "fx/delay/delay_types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

// **THIS SUITE IS BUILT ON VALUE ASSERTIONS, NOT ON COMPARING TWO RENDERS**, and that is CLAUDE.md's
// hardest-won testing rule rather than a style preference: *"A CLAIM ABOUT A VALUE CANNOT BE
// INFERRED FROM A RENDERED BUFFER"*. A comparison between two renders is evidence that SOMETHING
// changed and never that the right thing did — it passes whenever anything differs, so it is
// weakest exactly where a bug is subtle. Four instances of that failure are recorded in CLAUDE.md
// and every one was a PARTIAL effect surviving where a total one would have been caught.
//
// A delay is the same shape. "The repeats moved when I turned Time" is true of a device that put the
// tap in the wrong place, and "it sounds granular" is true of a granulator reading the wrong region.
// So the questions here are asked as numbers: WHERE is the tap, in samples; WHICH channel carries
// the second repeat; HOW MANY grains are in flight. `tapSeconds` is a pure function and needs no
// seam at all; the rest read the test-only accessors on `Delay`, which exist for this reason.

namespace {

using namespace feathers;
using namespace feathers::fx::delay;

constexpr double kRate = 48000.0;

/// 120 BPM — half a second a beat, so a 1/8 is 0.25 s and the numbers below are checkable by hand.
device::TransportClock at120() { return device::TransportClock::fromSecondsPerBeat(0.5); }

Settings defaults() { return stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings; }

/// THE INDEX OF THE LARGEST SAMPLE. Which is where a tap IS, when the input was one impulse.
size_t peakIndex(const std::vector<float>& v) {
	size_t best = 0;
	float bestValue = -1.0f;
	for (size_t i = 0; i < v.size(); ++i) {
		const float a = std::fabs(v[i]);
		if (a > bestValue) {
			bestValue = a;
			best = i;
		}
	}
	return best;
}

/// ONE IMPULSE IN, N FRAMES OUT. The device is driven in ONE call; the partition sweep in
/// `test_fx_devices.cpp` is what covers block-size independence, so nothing here needs to re-ask it.
void renderImpulse(Delay& d, std::vector<float>& l, std::vector<float>& r) {
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
}

} // namespace

TEST_CASE("the sync list is the shared division table with the free times in front of it") {
	// **BUILT, NOT RETYPED** — the point of `makeSyncNames`. A hand-written copy of 27 division names
	// is a copy that drifts, and this device is the third to read that table.
	// AND `Note` AT THE END (delay-love S6b): appended, so every older choice keeps its index and its label.
	REQUIRE(kSyncChoiceCount == kFreeTimeCount + device::kDivisionCount + 1);
	CHECK(std::string_view(kSyncNames[kSyncNoteChoice]) == "Note");
	CHECK(isNoteTime(static_cast<int>(kSyncNoteChoice)));
	CHECK_FALSE(isSyncedTime(static_cast<int>(kSyncNoteChoice)));
	CHECK_FALSE(isFreeTime(static_cast<int>(kSyncNoteChoice)));

	for (size_t i = 0; i < kFreeTimeCount; ++i) {
		CHECK(std::string_view(kSyncNames[i]) == std::string_view(kFreeTimeNames[i]));
		CHECK(isFreeTime(static_cast<int>(i)));
		CHECK_FALSE(isSyncedTime(static_cast<int>(i)));
	}
	for (size_t i = 0; i < device::kDivisionCount; ++i) {
		const auto choice = static_cast<int>(kFreeTimeCount + i);
		CHECK(std::string_view(kSyncNames[choice]) == std::string_view(device::kDivisionNames[i]));
		CHECK(isSyncedTime(choice));
		CHECK_FALSE(isFreeTime(choice));
		// **THE OFFSET IS THE TRAP, and this is what `syncDivisionBeats` exists to hide.** A reader
		// doing the arithmetic by hand is a reader who will one day forget the three free entries and
		// select a division two places off — silently, because every index is a valid division.
		CHECK(syncDivisionBeats(choice) == device::kDivisionBeats[i]);
	}

	// PAST BOTH ENDS IS NEITHER, rather than wrapping onto something plausible.
	CHECK_FALSE(isFreeTime(-1));
	CHECK_FALSE(isSyncedTime(-1));
	CHECK_FALSE(isSyncedTime(static_cast<int>(kSyncChoiceCount)));
}

TEST_CASE("the tap is the value the controls say, and the two timelines are read differently") {
	Settings s = defaults();
	s.timeMultiple = 1.0;

	SUBCASE("a free time ignores the tempo entirely") {
		// **THE WHOLE POINT OF A FREE ENTRY**: its period is a number of seconds and has nothing to do
		// with the song, so deriving it from beats would make a deliberately unmusical time drift with
		// the tempo. Asserted as the same number under two different clocks rather than as "it works".
		for (size_t i = 0; i < kFreeTimeCount; ++i) {
			s.sync = i;
			CHECK(tapSeconds(s, at120()) == doctest::Approx(kFreeTimeSeconds[i]));
			CHECK(tapSeconds(s, device::TransportClock::unknown())
			      == doctest::Approx(kFreeTimeSeconds[i]));
			CHECK(tapSeconds(s, device::TransportClock::fromSecondsPerBeat(2.0))
			      == doctest::Approx(kFreeTimeSeconds[i]));
		}
	}

	SUBCASE("a division is N beats, so it moves with the tempo") {
		s.sync = kDefaultSync; // 1/8, half a beat
		REQUIRE(std::string_view(kSyncNames[s.sync]) == "1/8");
		CHECK(tapSeconds(s, at120()) == doctest::Approx(0.25));
		// HALF THE TEMPO IS TWICE THE TAP — the property that keeps a synced delay on the grid, and
		// the one a seconds-derived period would break.
		CHECK(tapSeconds(s, device::TransportClock::fromSecondsPerBeat(1.0)) == doctest::Approx(0.5));
		CHECK(tapSeconds(s, device::TransportClock::fromSecondsPerBeat(0.25))
		      == doctest::Approx(0.125));
	}

	SUBCASE("with no tempo a division falls back to one second a beat") {
		s.sync = kDefaultSync;
		CHECK(tapSeconds(s, device::TransportClock::unknown()) == doctest::Approx(0.5));
		// **A STOPPED HOST IS NOT A HOST WITHOUT A TEMPO**, and `haveTempo()` is independent of
		// `playing` for exactly this reason. A transport that is parked still reports its tempo, so
		// the fallback above must NOT be reached here.
		device::TransportClock stopped = at120();
		stopped.playing = false;
		CHECK(tapSeconds(s, stopped) == doctest::Approx(0.25));
	}

	SUBCASE("Time multiplies whichever base the list selected — one meaning, both kinds") {
		s.sync = kDefaultSync; // 0.25 s at 120
		s.timeMultiple = 2.0;
		CHECK(tapSeconds(s, at120()) == doctest::Approx(0.5));
		s.timeMultiple = 0.5;
		CHECK(tapSeconds(s, at120()) == doctest::Approx(0.125));

		s.sync = 1; // "100 ms"
		s.timeMultiple = 2.0;
		CHECK(tapSeconds(s, at120()) == doctest::Approx(0.2));
		s.timeMultiple = 0.5;
		CHECK(tapSeconds(s, at120()) == doctest::Approx(0.05));
	}

	SUBCASE("the clamp is a real behaviour and both ends of it hold") {
		// **THE LONG END IS REACHABLE IN ORDINARY USE AT SLOW TEMPOS**, which is why it is asserted
		// rather than treated as a guard against the impossible. `4 bar.` is 24 beats; at one second a
		// beat that is 24 s against a 16 s line.
		s.sync = kSyncNoteChoice - 2; // (the divisions end where Note begins, delay-love S6b) // "4 bar."
		REQUIRE(std::string_view(kSyncNames[s.sync]) == "4 bar.");
		s.timeMultiple = 1.0;
		CHECK(tapSeconds(s, device::TransportClock::fromSecondsPerBeat(1.0))
		      == doctest::Approx(kMaxDelaySeconds));
		// And at a tempo where it fits, it is NOT clamped — or the assertion above would pass for a
		// function that returns the maximum always.
		CHECK(tapSeconds(s, device::TransportClock::fromSecondsPerBeat(0.25))
		      == doctest::Approx(6.0));

		s.sync = 0; // "10 ms"
		s.timeMultiple = std::exp2(kTimeLog2Min);
		CHECK(tapSeconds(s, at120()) == doctest::Approx(0.0025));
		CHECK(tapSeconds(s, at120()) >= kMinDelaySeconds);
	}
}

TEST_CASE("the tap lands at the sample the tap time names") {
	// **THE VALUE ASSERTION THIS WHOLE FILE IS ABOUT.** "The output changed when I moved Time" is
	// true of a delay that puts the repeat in the wrong place. Where the impulse comes back out is a
	// NUMBER, and a number is what gets checked.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());

	Settings s = defaults();
	s.enabled = true;
	s.mix = 1.0f;      // wet only, so the peak found is the tap and not the dry impulse
	s.feedback = 0.0f; // one repeat, so there is exactly one peak to find
	s.spread = 0.0f;
	s.mode = Mode::Clean;

	SUBCASE("a free 100 ms tap is 4800 samples at 48 kHz") {
		s.sync = 1;
		s.timeMultiple = 1.0;
		d.setTarget(s);
		std::vector<float> l(20000, 0.0f);
		std::vector<float> r(20000, 0.0f);
		renderImpulse(d, l, r);

		CHECK(d.tapSecondsForTest() == doctest::Approx(0.1));
		CHECK(peakIndex(l) == 4800);
		CHECK(peakIndex(r) == 4800);
		// THE TAP IS THE IMPULSE, UNCHANGED — with no feedback and no tone in the forward path there
		// is nothing between the input and the output but the line.
		CHECK(l[4800] == doctest::Approx(1.0));
	}

	SUBCASE("a synced 1/8 at 120 BPM is 12000 samples") {
		s.sync = kDefaultSync;
		s.timeMultiple = 1.0;
		d.setTarget(s);
		std::vector<float> l(30000, 0.0f);
		std::vector<float> r(30000, 0.0f);
		renderImpulse(d, l, r);

		CHECK(d.tapSecondsForTest() == doctest::Approx(0.25));
		CHECK(peakIndex(l) == 12000);
	}

	SUBCASE("Time x2 puts it at twice the sample") {
		s.sync = 1; // 100 ms
		s.timeMultiple = 2.0;
		d.setTarget(s);
		std::vector<float> l(20000, 0.0f);
		std::vector<float> r(20000, 0.0f);
		renderImpulse(d, l, r);

		CHECK(d.tapSecondsForTest() == doctest::Approx(0.2));
		CHECK(peakIndex(l) == 9600);
	}
}

TEST_CASE("feedback puts a second repeat one tap later, at the feedback's own level") {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());

	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1; // 100 ms -> 4800
	s.timeMultiple = 1.0;
	s.feedback = 0.5f;
	s.tone = 0.0f; // flat, or the second repeat is filtered and the level below is not the claim
	s.spread = 0.0f;
	s.mode = Mode::Clean;
	d.setTarget(s);

	std::vector<float> l(20000, 0.0f);
	std::vector<float> r(20000, 0.0f);
	renderImpulse(d, l, r);

	// **THREE REPEATS AT THE DECLARED RATIO, which is the claim a "did the buffer change" check
	// cannot make.** Feedback 0.5 means each repeat is half the one before it; a device that fed
	// back the wrong amount, or fed back the DRY signal, or tapped the wrong place would still
	// produce a buffer that differed from the feedback-free one.
	CHECK(l[4800] == doctest::Approx(1.0));
	CHECK(l[9600] == doctest::Approx(0.5));
	CHECK(l[14400] == doctest::Approx(0.25));
}

TEST_CASE("Spread moves the right channel only, and the left keeps meaning what the controls say") {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());

	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1; // 100 ms -> 4800
	s.timeMultiple = 1.0;
	s.feedback = 0.0f;
	s.mode = Mode::Clean;
	s.spread = 1.0f;
	d.setTarget(s);

	std::vector<float> l(20000, 0.0f);
	std::vector<float> r(20000, 0.0f);
	renderImpulse(d, l, r);

	// **THE ASYMMETRY IS THE DESIGN, ASSERTED AS TWO DIFFERENT NUMBERS.** A symmetric spread would
	// move BOTH taps, so the delay a hand dialled in would be the time on NEITHER channel. The left
	// stays exactly where Sync and Time put it.
	CHECK(d.tapSecondsForTest() == doctest::Approx(0.1));
	CHECK(d.tapSecondsRightForTest() == doctest::Approx(0.1 * (1.0 + kSpreadFraction)));
	CHECK(peakIndex(l) == 4800);
	CHECK(peakIndex(r) == static_cast<size_t>(std::lround(4800.0 * (1.0 + kSpreadFraction))));

	SUBCASE("and at zero the two channels are the same tap") {
		Delay flat;
		flat.prepare(kRate);
		flat.setTransportClock(at120());
		Settings z = s;
		z.spread = 0.0f;
		flat.setTarget(z);
		std::vector<float> zl(20000, 0.0f);
		std::vector<float> zr(20000, 0.0f);
		renderImpulse(flat, zl, zr);
		CHECK(flat.tapSecondsForTest() == doctest::Approx(flat.tapSecondsRightForTest()));
		CHECK(peakIndex(zl) == peakIndex(zr));
	}
}

TEST_CASE("Ping-Pong crosses the feedback and not the first tap") {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());

	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1; // 100 ms -> 4800
	s.timeMultiple = 1.0;
	s.feedback = 0.5f;
	s.tone = 0.0f;
	s.spread = 0.0f;
	s.mode = Mode::Clean;
	s.pingPong = PingPong::Stereo;
	d.setTarget(s);

	// ONE CHANNEL ONLY, or there is nothing for a cross to do — two identical channels swapped are
	// two identical channels. **This is also why the registry's audibility sweep can see this
	// control at all**: it drives both channels with the same probe, and only an arrangement with
	// Spread up makes the two differ.
	std::vector<float> l(20000, 0.0f);
	std::vector<float> r(20000, 0.0f);
	l[0] = 1.0f;
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

	// **THE FIRST TAP STAYS WHERE IT WAS RECORDED — that is what makes this ping-pong rather than a
	// channel swap.** Repeat 1 on the left, repeat 2 on the right, repeat 3 back on the left.
	CHECK(l[4800] == doctest::Approx(1.0));
	CHECK(r[4800] == doctest::Approx(0.0));
	CHECK(r[9600] == doctest::Approx(0.5));
	CHECK(l[9600] == doctest::Approx(0.0));
	CHECK(l[14400] == doctest::Approx(0.25));
	CHECK(r[14400] == doctest::Approx(0.0));
}

TEST_CASE("the granulator runs iff the mode says so AND Grain Size is off its bottom stop") {
	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1;
	s.timeMultiple = 1.0;
	s.feedback = 0.0f;
	s.densityHz = 50.0;
	s.grainSeconds = 0.05;

	std::vector<float> l(20000, 0.0f);
	std::vector<float> r(20000, 0.0f);

	SUBCASE("Clean mode starts no grains at all") {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		s.mode = Mode::Clean;
		d.setTarget(s);
		renderImpulse(d, l, r);
		CHECK(d.activeGrainsForTest() == 0);
	}

	SUBCASE("Granular mode has grains in flight") {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		s.mode = Mode::Granular;
		d.setTarget(s);
		renderImpulse(d, l, r);
		// **A COUNT, NOT "the buffer differs".** 50 grains a second of 50 ms each is an overlap of
		// 2.5, so some are always in flight; a granulator that spawned nothing would still hand back
		// a buffer unlike the clean one (silence is a difference).
		CHECK(d.activeGrainsForTest() > 0);
		CHECK(d.activeGrainsForTest() <= kMaxGrains);
	}

	SUBCASE("Grain Size at the bottom means OFF, and off is the clean tap EXACTLY") {
		// **THE `Unit::LogSeconds` CONVENTION, HONOURED RATHER THAN DECLARED.** The value box prints
		// "off" at `desc.min`, so the device must be off there or the card is promising a stage it
		// does not skip. Asserted as SAMPLE-FOR-SAMPLE EQUALITY with Clean mode rather than as "it
		// sounds similar" — a granulator that merely passed most of the signal would pass that.
		Delay granularOff;
		granularOff.prepare(kRate);
		granularOff.setTransportClock(at120());
		Settings off = s;
		off.mode = Mode::Granular;
		off.grainSeconds = 0.0; // what stateOf produces at the range's minimum
		granularOff.setTarget(off);
		std::vector<float> gl(20000, 0.0f);
		std::vector<float> gr(20000, 0.0f);
		renderImpulse(granularOff, gl, gr);
		CHECK(granularOff.activeGrainsForTest() == 0);

		Delay clean;
		clean.prepare(kRate);
		clean.setTransportClock(at120());
		Settings c = s;
		c.mode = Mode::Clean;
		clean.setTarget(c);
		std::vector<float> cl(20000, 0.0f);
		std::vector<float> cr(20000, 0.0f);
		renderImpulse(clean, cl, cr);

		CHECK(gl == cl);
		CHECK(gr == cr);
	}
}

TEST_CASE("the row codec round-trips, and the Grain Size OFF position survives the trip") {
	// **THE ONE FIELD THAT IS NOT ITS OWN INVERSE.** Every other control encodes as itself; grain
	// size encodes zero seconds as the range's MINIMUM, because `log2(0)` is negative infinity. A
	// round trip is the only thing that catches an encode and a decode disagreeing about that.
	State s {};
	s.settings.grainSeconds = 0.0;
	const Row row = rowOf(s);
	CHECK(row[static_cast<size_t>(Param::GrainSize)]
	      == doctest::Approx(static_cast<float>(kGrainSizeLog2Min)));
	const State back = stateOf(row, static_cast<size_t>(Param::kCount));
	CHECK(back.settings.grainSeconds == 0.0);

	SUBCASE("and a real grain size round-trips as itself") {
		State g {};
		g.settings.grainSeconds = 0.05;
		const State again = stateOf(rowOf(g), static_cast<size_t>(Param::kCount));
		CHECK(again.settings.grainSeconds == doctest::Approx(0.05));
	}

	SUBCASE("a short payload leaves the missing fields at their declared defaults") {
		// A ROW WRITTEN BY AN OLDER BUILD IS SHORTER, and what it omits must not read as zero.
		const State partial = stateOf(Row {}, 0);
		CHECK(partial.settings.sync == kDefaultSync);
		CHECK(partial.settings.mix
		      == doctest::Approx(kParamDescs[static_cast<size_t>(Param::Mix)].def));
		CHECK(partial.settings.feedback
		      == doctest::Approx(kParamDescs[static_cast<size_t>(Param::Feedback)].def));
	}
}

TEST_CASE("the tail reaches EXACT zero rather than decaying into the denormals") {
	// The registry-wide sweep asks this of every device; this asks it of the MECHANISM, because the
	// sweep can only see the samples and the flag is what says the line itself went quiet.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());

	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 0; // 10 ms, so the tail is short enough to drive to the end here
	s.timeMultiple = 1.0;
	s.feedback = 0.5f;
	s.tone = 0.0f;
	s.mode = Mode::Clean;
	d.setTarget(s);

	std::vector<float> l(2048, 0.0f);
	std::vector<float> r(2048, 0.0f);
	renderImpulse(d, l, r);
	CHECK_FALSE(d.silentForTest());

	// 0.5 per 10 ms tap from 1.0 needs ~24 taps to pass 1e-7; 400 blocks of 2048 is 17 s of room.
	for (int b = 0; b < 400 && !d.silentForTest(); ++b) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	}
	CHECK(d.silentForTest());

	// AND THE OUTPUT IS EXACTLY ZERO, not merely small — the claim the flush exists to make.
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	for (size_t i = 0; i < l.size(); ++i) {
		REQUIRE(l[i] == 0.0f);
		REQUIRE(r[i] == 0.0f);
	}
}

TEST_CASE("reset leaves the device as if just switched on, random stream included") {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());

	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1;
	s.timeMultiple = 1.0;
	s.feedback = 0.5f;
	s.mode = Mode::Granular;
	s.grainSeconds = 0.02;
	s.densityHz = 100.0;
	s.spray = 0.8f;
	d.setTarget(s);

	std::vector<float> a(12000, 0.0f);
	std::vector<float> b(12000, 0.0f);
	renderImpulse(d, a, b);

	d.reset();
	std::vector<float> c(12000, 0.0f);
	std::vector<float> e(12000, 0.0f);
	renderImpulse(d, c, e);

	// **BIT-FOR-BIT, AND THE RANDOM STREAM IS WHY THIS IS WORTH ASSERTING.** A granulator that reset
	// its line but kept its RNG would produce a different spray on the second run — "as if just
	// switched on" failing in the one place a buffer-clearing reset cannot see.
	CHECK(a == c);
	CHECK(b == e);
	CHECK(d.activeGrainsForTest() > 0);
}

TEST_CASE("a hot signal through extreme settings stays finite, feedback past unity included") {
	// Self-oscillation is a deliberate state — `kFeedbackMax` is past 1 on purpose — so the claim is
	// BOUNDED, not decaying. The soft clip is what makes those two compatible.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());

	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 0; // 10 ms, so the loop goes round often within the render
	s.timeMultiple = 1.0;
	s.feedback = static_cast<float>(kFeedbackMax);
	s.tone = 0.0f;
	s.mode = Mode::Granular;
	s.grainSeconds = 0.05;
	s.densityHz = 200.0; // the densest this device offers: maximum overlap, maximum summing
	s.spray = 1.0f;
	d.setTarget(s);

	std::vector<float> l(4096, 0.0f);
	std::vector<float> r(4096, 0.0f);
	for (int b = 0; b < 200; ++b) {
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.9f;
			r[i] = -0.9f;
		}
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		for (size_t i = 0; i < l.size(); ++i) {
			REQUIRE(std::isfinite(l[i]));
			REQUIRE(std::isfinite(r[i]));
		}
	}
}

TEST_CASE("Tone darkens the repeats rather than the first tap") {
	// **WHICH TAP THE FILTER IS ON IS THE CLAIM**, and it is the one thing a "the sound changed"
	// check cannot distinguish: a tone control wired to the OUTPUT would also change the sound, and
	// would also darken. So the first tap is asserted UNCHANGED and the second asserted quieter.
	const auto run = [](float tone, std::vector<float>& l, std::vector<float>& r) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1; // 100 ms -> 4800
		s.timeMultiple = 1.0;
		s.feedback = 0.9f;
		s.tone = tone;
		s.spread = 0.0f;
		s.mode = Mode::Clean;
		d.setTarget(s);
		l.assign(20000, 0.0f);
		r.assign(20000, 0.0f);
		renderImpulse(d, l, r);
	};

	std::vector<float> flatL;
	std::vector<float> flatR;
	std::vector<float> darkL;
	std::vector<float> darkR;
	run(0.0f, flatL, flatR);
	run(-1.0f, darkL, darkR);

	// THE FIRST TAP IS THE INPUT, WHATEVER THE TONE IS — the filter is in the loop, and the loop has
	// not been round yet.
	CHECK(flatL[4800] == doctest::Approx(1.0));
	CHECK(darkL[4800] == doctest::Approx(1.0));

	// THE SECOND IS NOT. An impulse is broadband, and a lowpass at 200 Hz spreads it over time rather
	// than passing it — so its PEAK is far below the flat build's.
	CHECK(std::fabs(darkL[9600]) < std::fabs(flatL[9600]) * 0.5f);
}

TEST_CASE("the face declares a place for every control, and the sections open where they should") {
	// **A `faceOrder` IS A FILTER AS WELL AS AN ORDER, and that is the hazard.** `GenericCard` draws
	// the controls this table names and silently draws no others — so a control left out of it is a
	// control a hand cannot reach, with nothing red anywhere. The count alone does not catch it
	// either: a table that names one control twice and omits another is the right length.
	std::vector<size_t> seen;
	for (const uint8_t index : kFaceOrder) {
		CHECK(index < static_cast<uint8_t>(Param::kCount));
		CHECK(std::find(seen.begin(), seen.end(), static_cast<size_t>(index)) == seen.end());
		seen.push_back(index);
	}
	// EVERY CONTROL BUT THE ENABLE, which the shared card head draws as its square rather than the
	// grid — so listing it here would give one field two controls. AND BUT CLEAR (delay-love S4), a momentary: a grid cell
	// would latch it, so the window holds its button and the card does not. AND BUT DUCK, ITS RELEASE, FREEZE AND SPRAY
	// (off the card, which had grown too large, but kept in the window): the window offers every control (test_gui_widgets_devices' S2 case walks them all), so none is out of
	// reach — only off the card. Named here so that any OTHER omission is still red.
	const auto offCard = [](size_t p) {
		for (const Param q : { Param::Enabled, Param::Clear, Param::Duck, Param::DuckRelease, Param::Freeze, Param::Spray }) {
			if (p == static_cast<size_t>(q)) {
				return true;
			}
		}
		return false;
	};
	CHECK(seen.size() == static_cast<size_t>(Param::kCount) - 6u);
	for (size_t p = 0; p < static_cast<size_t>(Param::kCount); ++p) {
		const bool listed = std::find(seen.begin(), seen.end(), p) != seen.end();
		CAPTURE(kParamDescs[p].name);
		CHECK(listed == !offCard(p));
	}

	SUBCASE("every section opens at a control the face actually draws") {
		// A heading keyed to a control that is not on the face is a heading that never appears.
		for (const device::FaceSection& sec : kFaceSections) {
			CAPTURE(sec.label);
			CHECK(std::find(seen.begin(), seen.end(), static_cast<size_t>(sec.firstControl))
			      != seen.end());
		}
	}

	SUBCASE("GRAIN and TAPE each hold only controls gated on their own mode, so each comes and goes whole") {
		// **WHAT HAS TO HOLD FOR THE EXPAND PATTERN TO WORK: every control under a mode's heading
		// is gated on THAT mode.** Then the titled block is absent wherever it does nothing and
		// appears when the mode moves, rather than leaving a heading over hidden rows. A future
		// ungated control filed under either would strand its section, and this goes red.
		//
		// **AND IT IS WHAT LETS THE TWO SHARE ONE ROOM** (tape-specific params fit in the same space
		// and follow the same expand pattern as grain). `Mode`
		// is exclusive, so if each group is gated on its own value then at most one is ever live and
		// the card is the same height in Granular as in Tape.
		//
		// **WALKED SECTION BY SECTION, NOT "FROM HERE TO THE END".** The first version of this case
		// ran from GRAIN's first control to the end of the face order — true while GRAIN was last,
		// and red the moment TAPE was listed after it, reading a Tape mask where it expected
		// Granular. The bound is the NEXT section's opening control.
		const auto sectionOf = [](std::string_view label) -> const device::FaceSection* {
			for (const device::FaceSection& sec : kFaceSections) {
				if (std::string_view(sec.label) == label) {
					return &sec;
				}
			}
			return nullptr;
		};
		const auto opensASection = [](uint8_t control) {
			for (const device::FaceSection& sec : kFaceSections) {
				if (sec.firstControl == control) {
					return true;
				}
			}
			return false;
		};

		// (GRAIN HELD FIVE FROM SWARM, 2026-11-05, until Spray left the card, 2026-11-06 — four again, one row like the rest;
		// BBD is the fourth tenant.)
		struct Expect {
			const char* label;
			uint32_t mask;
			size_t count;
		};
		const Expect expected[] = {
			{ "GRAIN", kGranularOnly, 4 },
			{ "TAPE", kTapeOnly, 4 },
			{ "TAPS", kMultiTapOnly, 4 },
			{ "BUCKETS", kBbdOnly, 4 },
		};
		for (const auto& [label, mask, count] : expected) {
			CAPTURE(label);
			const device::FaceSection* sec = sectionOf(label);
			REQUIRE(sec != nullptr);
			const auto* first = std::find(std::begin(kFaceOrder), std::end(kFaceOrder),
			                              sec->firstControl);
			REQUIRE(first != std::end(kFaceOrder));
			size_t counted = 0;
			for (const uint8_t* it = first; it != std::end(kFaceOrder); ++it) {
				if (it != first && opensASection(*it)) {
					break; // the next section begins here
				}
				CAPTURE(kParamDescs[*it].name);
				CHECK(kParamDescs[*it].gateControl == static_cast<int16_t>(Param::Mode));
				CHECK(kParamDescs[*it].gateMask == mask);
				++counted;
			}
			// NON-VACUITY, and the reason the two fit one room: four each.
			CHECK(counted == count);
		}

		// AND THE TWO MASKS DO NOT OVERLAP, or both sections could be live at once and the card
		// would be two rows taller than either mode alone.
		CHECK((kGranularOnly & kTapeOnly) == 0u);
		CHECK((kGranularOnly & kMultiTapOnly) == 0u);
		CHECK((kTapeOnly & kMultiTapOnly) == 0u);

		// AND `Mode` ITSELF IS UNDER NEITHER — it is on the top row.
		const auto* modeAt = std::find(std::begin(kFaceOrder), std::end(kFaceOrder),
		                               static_cast<uint8_t>(Param::Mode));
		const auto* grainAt = std::find(std::begin(kFaceOrder), std::end(kFaceOrder),
		                                sectionOf("GRAIN")->firstControl);
		REQUIRE(modeAt != std::end(kFaceOrder));
		CHECK(modeAt < grainAt);
	}

	SUBCASE("the top row is sync, time, mode, mix — four CELLS, and Dub rides in Mode's") {
		// One row of Sync, Time, Mode and Mix, with Mode and the Dub button double-stacked into one
		// knob height.
		//
		// **THE ROW IS COUNTED IN CELLS, NOT IN ENTRIES, and that distinction is the whole of the
		// stacking feature.** `Dub` sits between `Mode` and `Mix` in the face ORDER — it has to,
		// because `stacked` means "the cell before me in this order" — while occupying no cell of
		// its own. So the top row is five entries and four columns, and an assertion written over
		// raw indices reads `Dub` where it expects `Mix`. This one walks cells.
		std::vector<Param> topRow;
		for (const uint8_t index : kFaceOrder) {
			if (!kParamDescs[index].stacked) {
				topRow.push_back(static_cast<Param>(index));
			}
			if (topRow.size() == 4u) {
				break;
			}
		}
		REQUIRE(topRow.size() == 4u);
		CHECK(topRow[0] == Param::Sync);
		CHECK(topRow[1] == Param::Time);
		CHECK(topRow[2] == Param::Mode);
		CHECK(topRow[3] == Param::Mix);

		// **AND `Dub` IMMEDIATELY FOLLOWS `Mode`**, which is what puts it in Mode's cell rather than
		// Time's. The flag alone does not say which cell; the adjacency does.
		const auto* dubAt = std::find(std::begin(kFaceOrder), std::end(kFaceOrder),
		                              static_cast<uint8_t>(Param::Dub));
		REQUIRE(dubAt != std::begin(kFaceOrder));
		REQUIRE(dubAt != std::end(kFaceOrder));
		CHECK(kParamDescs[static_cast<size_t>(Param::Dub)].stacked);
		CHECK(*(dubAt - 1) == static_cast<uint8_t>(Param::Mode));

		// **AND NOTHING OPENS A SECTION ON THEM**, or the row would be split by a heading landing in
		// the middle of it — `faceRowsOf` always breaks a row at a section.
		for (const device::FaceSection& sec : kFaceSections) {
			CAPTURE(sec.label);
			for (size_t k = 1; k < topRow.size(); ++k) {
				CHECK(sec.firstControl != static_cast<uint8_t>(topRow[k]));
			}
			CHECK(sec.firstControl != static_cast<uint8_t>(Param::Dub));
		}
	}
}

TEST_CASE("Mode and Ping-Pong are pickers, Enabled is a switch, and the descriptor is what says so") {
	// **`Off`/`On` IS A SWITCH; ANY OTHER NAMED CHOICE IS A PICKER, however few entries it has.**
	// Every card surface used to branch on `names.size() == 2`, which was right for the only
	// two-valued controls that existed and wrong the moment one had NAMED states: a lit chip
	// captioned "Delay Mode" never says the word "Granular".
	//
	// **AND THE COUNT IS WRONG ABOUT THE FUTURE, which is what fixed the shape.** Mode is a
	// dropdown, to leave room for future modes such as analog and tape. A control that
	// is a chip at two entries and a dropdown at three changes shape under the player the day a mode
	// lands. Asserted on the DECLARATION here; the widget choice that follows from it is asserted on
	// the card, in the widget suite.
	CHECK_FALSE(device::isOffOnChoice(kParamDescs[static_cast<size_t>(Param::Mode)]));
	// PING-PONG STOPPED BEING A SWITCH ON 2026-10-11 (Off / Stereo / Classic): three named states are a picker.
	CHECK_FALSE(device::isOffOnChoice(kParamDescs[static_cast<size_t>(Param::PingPong)]));
	CHECK(device::isOffOnChoice(kParamDescs[static_cast<size_t>(Param::Enabled)]));

	SUBCASE("and adding Analog or Tape must not turn it back into a switch") {
		// The forward-looking half, pinned so it cannot regress: whatever Mode grows to, it is never
		// an Off/On pair, so it never becomes a chip again.
		CHECK(std::size(kModeNames) >= 2u);
		CHECK(std::string_view(kModeNames[0]) != "Off");
	}
}

TEST_CASE("the mono road is the stereo road's LEFT channel, with the channels made to differ") {
	// **THE REGISTRY SWEEP ASKS THIS OF EVERY DEVICE AND CANNOT ASK IT OF THIS ONE.**
	// `test_fx_devices.cpp`'s mono case drives a probe far shorter than a half-second tap, so the
	// delay's wet never arrives inside it: both channels are pure scaled dry and agree trivially.
	// Its non-vacuity guard does not catch that either — the guard asks whether the device CHANGED
	// the buffer, and `Mix` at 0.6 scales the dry signal, so the device looks busy while the thing
	// under test is silent. A sweep passing for a reason unrelated to its claim.
	//
	// **AND THE CHANNELS HAVE TO BE MADE TO DIFFER, which is the other half.** With Spread at zero
	// and Ping-Pong off the two channels are the same signal, and "mono is the left channel" is true
	// of any implementation that returns either one. Spread is what makes the two roads distinguish
	// themselves.
	const auto render = [](float spread, bool pingPong, bool mono) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1; // 100 ms -> 4800 frames, so several taps land inside the render
		s.timeMultiple = 1.0;
		s.feedback = 0.5f;
		s.spread = spread;
		s.pingPong = pingPong ? PingPong::Stereo : PingPong::Off;
		s.mode = Mode::Clean;
		d.setTarget(s);

		std::vector<float> l(20000, 0.0f);
		std::vector<float> r(20000, 0.0f);
		l[0] = 1.0f;
		r[0] = 1.0f;
		if (mono) {
			d.processMono(l.data(), static_cast<int>(l.size()));
			return l;
		}
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		return l;
	};

	SUBCASE("with Spread up") {
		const std::vector<float> stereoLeft = render(1.0f, false, false);
		const std::vector<float> monoOut = render(1.0f, false, true);
		// NON-VACUITY: the spread must actually have separated the channels, or this compares a
		// device with itself.
		Delay probe;
		probe.prepare(kRate);
		probe.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1;
		s.timeMultiple = 1.0;
		s.spread = 1.0f;
		s.mode = Mode::Clean;
		probe.setTarget(s);
		std::vector<float> pl(256, 0.0f);
		std::vector<float> pr(256, 0.0f);
		probe.processStereo(pl.data(), pr.data(), 256);
		CHECK(probe.tapSecondsForTest() != probe.tapSecondsRightForTest());

		CHECK(monoOut == stereoLeft);
	}

	SUBCASE("with Ping-Pong on") {
		const std::vector<float> stereoLeft = render(0.0f, true, false);
		const std::vector<float> monoOut = render(0.0f, true, true);
		CHECK(monoOut == stereoLeft);
	}
}

TEST_CASE("the granulator gives the same samples in a different partition") {
	// **THE REGISTRY SWEEP CANNOT ASK THIS OF GRANULAR MODE.** `test_fx_devices.cpp`'s partition
	// case drives `variedRowFor`, which deliberately HOLDS every stepped control at its default —
	// *"moving a Type or a Choice changes which DSP runs"* — and `Delay Mode` is stepped. So the
	// sweep tests the clean tap's partition-independence and never starts a single grain.
	//
	// **WHICH IS THE HALF THAT COULD PLAUSIBLY BE BLOCK-DEPENDENT.** The clean path is a ring read
	// per sample with nothing remembered across a call. The granulator carries a spawn accumulator,
	// a pool of grains with their own read heads and ages, and an RNG whose stream position IS
	// state — any of which, if advanced per BLOCK rather than per SAMPLE, would make the sound
	// depend on the host's buffer size. That is inaudible in a test that always uses one buffer
	// size and immediately audible to a player who changes theirs.
	const auto render = [](const std::vector<int>& blocks) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1; // 100 ms
		s.timeMultiple = 1.0;
		s.feedback = 0.45f;
		s.mode = Mode::Granular;
		s.grainSeconds = 0.03;
		s.densityHz = 80.0;  // overlap 2.4 — grains genuinely on top of each other
		s.spray = 0.7f;      // and the RNG genuinely consulted, once per grain
		s.pitchSemitones = 5.0f;
		d.setTarget(s);

		std::vector<float> l(24000, 0.0f);
		std::vector<float> r(24000, 0.0f);
		// A CONTINUOUS INPUT rather than one impulse, so every block has something to granulate and
		// a partition seam has material either side of it to get wrong.
		for (size_t i = 0; i < l.size(); ++i) {
			const auto t = static_cast<float>(i);
			l[i] = 0.6f * std::sin(0.01f * t);
			r[i] = 0.6f * std::sin(0.0131f * t);
		}
		int at = 0;
		size_t b = 0;
		while (at < static_cast<int>(l.size())) {
			const int n = std::min(blocks[b % blocks.size()],
			                       static_cast<int>(l.size()) - at);
			d.processStereo(l.data() + at, r.data() + at, n);
			at += n;
			++b;
		}
		return std::make_pair(l, r);
	};

	// ONE BIG BLOCK, a ragged partition, and a partition whose pieces are not powers of two — the
	// last because every accumulator bug I can think of hides behind a block size that divides
	// something else evenly.
	const auto whole = render({ 24000 });
	const auto ragged = render({ 64, 1, 512, 3, 2048, 7 });
	const auto odd = render({ 97, 331, 13 });

	CHECK(ragged.first == whole.first);
	CHECK(ragged.second == whole.second);
	CHECK(odd.first == whole.first);
	CHECK(odd.second == whole.second);

	SUBCASE("and the run is not silent, or the comparison is between two empty buffers") {
		float peak = 0.0f;
		for (const float v : whole.first) {
			peak = std::max(peak, std::fabs(v));
		}
		CAPTURE(peak);
		CHECK(peak > 1.0e-3f);
	}
}

TEST_CASE("Dub sends into the line instead of balancing against it, and the dry stays whole") {
	// A dub mode switches Mix to control the input sent into the wet chain, with the dry summed at
	// 100%.
	const auto make = [](bool dub, float mix) {
		Settings s = defaults();
		s.sync = 1; // 100 ms -> 4800 frames
		s.timeMultiple = 1.0;
		s.feedback = 0.5f;
		s.tone = 0.0f;
		s.spread = 0.0f;
		s.mode = Mode::Clean;
		s.dub = dub;
		s.mix = mix;
		return s;
	};

	SUBCASE("the dry passes at FULL level, whatever the send is set to") {
		// **THE CLAIM IS A NUMBER AT A KNOWN SAMPLE.** At sample 0 the line is empty, so whatever
		// comes out is the dry path alone — which is exactly where "100% dry summed" is checkable.
		for (const float mix : { 0.0f, 0.35f, 1.0f }) {
			Delay d;
			d.prepare(kRate);
			d.setTransportClock(at120());
			d.setTarget(make(true, mix));
			std::vector<float> l(8000, 0.0f);
			std::vector<float> r(8000, 0.0f);
			renderImpulse(d, l, r);
			CAPTURE(mix);
			CHECK(l[0] == doctest::Approx(1.0));
		}
		// AND THE CONTRAST, or the check above is true of any device that passes its input. With Dub
		// OFF the same knob is a crossfade, so the dry is scaled by it and sample 0 moves.
		Delay off;
		off.prepare(kRate);
		off.setTransportClock(at120());
		off.setTarget(make(false, 0.35f));
		std::vector<float> l(8000, 0.0f);
		std::vector<float> r(8000, 0.0f);
		renderImpulse(off, l, r);
		CHECK(l[0] == doctest::Approx(0.65));
	}

	SUBCASE("the send scales what reaches the line, so the first repeat follows it") {
		for (const float mix : { 0.25f, 0.5f, 1.0f }) {
			Delay d;
			d.prepare(kRate);
			d.setTransportClock(at120());
			d.setTarget(make(true, mix));
			std::vector<float> l(8000, 0.0f);
			std::vector<float> r(8000, 0.0f);
			renderImpulse(d, l, r);
			CAPTURE(mix);
			// The tap is the input times the send, at full wet level — not a crossfade of the two.
			CHECK(l[4800] == doctest::Approx(mix));
		}
	}

	SUBCASE("and with the send at zero nothing enters the line at all") {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		d.setTarget(make(true, 0.0f));
		std::vector<float> l(8000, 0.0f);
		std::vector<float> r(8000, 0.0f);
		renderImpulse(d, l, r);
		CHECK(l[4800] == doctest::Approx(0.0));
	}
}

TEST_CASE("the dub move: cutting the send leaves what is already in the line washing out") {
	// **THE ONE BEHAVIOUR A CROSSFADE CANNOT IMITATE AT ANY SETTING, and the reason Dub is a routing
	// change rather than a second taper on the same knob.** Feed a phrase in, cut the send, and the
	// repeats already circulating keep decaying through their own feedback. A crossfade taken to
	// zero acts AFTER the line and mutes the tail with the send.
	//
	// Asserted as two numbers on ONE device rather than as a comparison of two renders: the tap
	// before the cut, and the tap after it, on the same instance.
	const auto run = [](bool dub) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.sync = 1; // 100 ms -> 4800
		s.timeMultiple = 1.0;
		s.feedback = 0.8f; // a tail with several audible repeats in it
		s.tone = 0.0f;
		s.spread = 0.0f;
		s.mode = Mode::Clean;
		s.dub = dub;
		s.mix = 1.0f;
		d.setTarget(s);

		// One impulse in, then run past the first repeat.
		std::vector<float> l(7000, 0.0f);
		std::vector<float> r(7000, 0.0f);
		l[0] = 1.0f;
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		const float firstTap = l[4800];

		// **NOW CUT IT.** Nothing else changes.
		s.mix = 0.0f;
		d.setTarget(s);

		// **MEASURED AFTER A FULL LAP HAS BEEN OVERWRITTEN, and the window is the whole assertion.**
		// A first draft took the peak over the entire post-cut render and a known-bad walked
		// straight through it: putting the send on the FEEDBACK as well as the input
		// (`(in + fb) * send`) kills the tail, but the first 4800 frames still play ring content
		// written BEFORE the cut, so the peak was high either way. The window has to start past one
		// tap, where the only thing that can still be sounding is material the loop re-circulated
		// after the send closed.
		const int tapFrames = 4800;
		std::vector<float> l2(3 * tapFrames, 0.0f);
		std::vector<float> r2(3 * tapFrames, 0.0f);
		d.processStereo(l2.data(), r2.data(), static_cast<int>(l2.size()));
		float after = 0.0f;
		for (size_t i = static_cast<size_t>(tapFrames) + 100u; i < l2.size(); ++i) {
			after = std::max(after, std::fabs(l2[i]));
		}
		return std::make_pair(firstTap, after);
	};

	const auto dub = run(true);
	const auto crossfade = run(false);

	// NON-VACUITY: both builds heard the first repeat, so "the tail continued" is about a tail that
	// existed.
	CHECK(dub.first == doctest::Approx(1.0));
	CHECK(crossfade.first > 0.5f);

	// **DUB KEEPS WASHING OUT.** Feedback 0.8 means the second repeat is ~0.8 of the first.
	CHECK(dub.second > 0.5f);

	// **THE CROSSFADE GOES SILENT**, because its knob is after the line rather than before it.
	CHECK(crossfade.second == doctest::Approx(0.0));
}

TEST_CASE("moving the delay time does not step the signal — it glides, or it fades") {
	// When the delay time changes, does the tap repitch or crossfade? The answer at the
	// time was NEITHER: `tapFrames` was a per-block number read straight into `readLerp`, so the head
	// jumped and the output stepped.
	//
	// **MEASURED BEFORE THE FIX: a step of 0.173 against a typical sample-to-sample slope of 0.031 —
	// 5.5x the signal's own slope.** That is a click, not a transition. After: 0.91x gliding and
	// 1.16x fading, both at or near what the waveform was doing anyway.
	//
	// **THE CLAIM IS A RATIO, NOT AN ABSOLUTE**, because the right bound depends on how fast the
	// probe tone is moving. Comparing the worst step against the same signal's own typical step is
	// what makes the number mean "audible discontinuity" rather than "loud".
	const auto worstStepRatio = [](Mode mode) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1; // 100 ms
		s.timeMultiple = 1.0;
		s.feedback = 0.5f;
		s.tone = 0.0f;
		s.spread = 0.0f;
		s.mode = mode;
		// **GRANULAR'S GRAINS ARE TURNED OFF EVEN IN GRANULAR MODE**, so the only thing varying
		// between the three runs is how the TAP moves. A granulator running would put its own
		// envelope edges into the measurement and the case would be about two things.
		s.grainSeconds = 0.0;
		d.setTarget(s);

		std::vector<float> l(48000, 0.0f);
		std::vector<float> r(48000, 0.0f);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
			r[i] = l[i];
		}
		d.processStereo(l.data(), r.data(), 20000);
		const float lastBefore = l[19999];

		s.timeMultiple = 1.5; // one musical notch, the move a hand makes
		d.setTarget(s);
		d.processStereo(l.data() + 20000, r.data() + 20000, 8000);

		float typical = 0.0f;
		for (size_t i = 10001; i < 19999; ++i) {
			typical = std::max(typical, std::fabs(l[i] - l[i - 1]));
		}
		float worst = std::fabs(l[20000] - lastBefore);
		for (size_t i = 20001; i < 28000; ++i) {
			worst = std::max(worst, std::fabs(l[i] - l[i - 1]));
		}
		// NON-VACUITY: a silent run would have a typical of zero and a ratio of nonsense.
		REQUIRE(typical > 0.001f);
		return worst / typical;
	};

	// **TWO, NOT 5.5 AND NOT 1.0.** A glide legitimately moves the head, so the wet is read slightly
	// faster or slower than it was written and neighbouring samples CAN differ a little more than
	// the source did; a crossfade sums two decorrelated taps for 15 ms. The bound is set where a
	// discontinuity lives, well below the 5.5x that was there and above the ~1.2x the fix produces,
	// so it fails on a jump rather than on a tuning change to the glide time.
	for (const Mode mode : { Mode::Clean, Mode::Granular, Mode::Xfade }) {
		CAPTURE(static_cast<int>(mode));
		CHECK(worstStepRatio(mode) < 2.0f);
	}
}

TEST_CASE("Tape: the wow bends the pitch by the deviation it is asked for, not by a fixed offset") {
	// **THE CONTROL IS A PITCH DEVIATION AND THE MODULATION IS A POSITION, and the conversion is the
	// one piece of arithmetic in the tape machine that can be silently wrong.** A head offset of
	// `A·sin(2πft)` bends the pitch by `A·2πf/sr`; the device solves `A` from the deviation. Get that
	// backwards and Wow still wobbles — it just wobbles by an amount that depends on its own rate,
	// and no "the sound changed" check would ever say so.
	//
	// **MEASURED OFF THE OUTPUT'S ZERO CROSSINGS**, which is as close to reading the value as a
	// pitch gets: a steady tone through the tap comes back at its own frequency when the machine is
	// steady, and swings about it when it is not. The swing is the claim.
	const auto periodSpread = [](float wow, float flutter) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1; // 100 ms
		s.timeMultiple = 1.0;
		s.feedback = 0.0f; // one pass, so the deviation measured is ONE application of the wobble
		s.tone = 0.0f;
		s.spread = 0.0f;
		s.mode = Mode::Tape;
		s.wow = wow;
		s.flutter = flutter;
		s.saturation = 0.0f; // transparent, so the waveform's crossings are the tone's own
		s.hfLoss = 0.0f;
		d.setTarget(s);

		// 1 kHz for four seconds: long enough to contain several turns of the slowest wow component.
		const size_t n = 4 * 48000;
		std::vector<float> l(n, 0.0f);
		std::vector<float> r(n, 0.0f);
		for (size_t i = 0; i < n; ++i) {
			l[i] = 0.5f * static_cast<float>(std::sin(6.28318530717958647692 * 1000.0
			                                          * static_cast<double>(i) / kRate));
			r[i] = l[i];
		}
		d.processStereo(l.data(), r.data(), static_cast<int>(n));

		// Rising zero crossings, interpolated, after the tap has arrived.
		std::vector<double> crossings;
		for (size_t i = 9600; i + 1 < n; ++i) {
			if (l[i] <= 0.0f && l[i + 1] > 0.0f) {
				const double frac = static_cast<double>(-l[i]) / static_cast<double>(l[i + 1] - l[i]);
				crossings.push_back(static_cast<double>(i) + frac);
			}
		}
		REQUIRE(crossings.size() > 1000u); // non-vacuity: the tone really came through
		double lo = 1.0e9;
		double hi = 0.0;
		for (size_t k = 1; k < crossings.size(); ++k) {
			const double period = crossings[k] - crossings[k - 1];
			lo = std::min(lo, period);
			hi = std::max(hi, period);
		}
		const double nominal = kRate / 1000.0; // 48 samples
		return std::make_pair((nominal - lo) / nominal, (hi - nominal) / nominal);
	};

	SUBCASE("a steady machine returns the tone at its own pitch") {
		const auto steady = periodSpread(0.0f, 0.0f);
		// NOT EXACTLY ZERO — the crossings are interpolated — but far inside anything a wobble makes.
		CHECK(steady.first < 0.0005);
		CHECK(steady.second < 0.0005);
	}

	SUBCASE("full Wow swings the pitch by about what it declares, and no more") {
		const auto wobbly = periodSpread(1.0f, 0.0f);
		// **THE BOUND IS THE DECLARED DEVIATION.** Two components at half each, so their peaks can
		// align to the full `kWowMaxDeviation` and never past it. A depth stated in FRAMES rather
		// than solved from the deviation lands wildly outside this at either rate.
		CHECK(wobbly.first > 0.25 * kWowMaxDeviation);  // it genuinely wobbles
		CHECK(wobbly.second > 0.25 * kWowMaxDeviation);
		CHECK(wobbly.first < 1.25 * kWowMaxDeviation);  // and by no more than it says
		CHECK(wobbly.second < 1.25 * kWowMaxDeviation);
	}

	SUBCASE("Flutter is shallower than Wow at the same setting, which is what the two ranges say") {
		const auto wow = periodSpread(1.0f, 0.0f);
		const auto flutter = periodSpread(0.0f, 1.0f);
		CHECK(flutter.second > 0.25 * kFlutterMaxDeviation);
		CHECK(flutter.second < 1.25 * kFlutterMaxDeviation);
		CHECK(flutter.second < wow.second);
	}
}

TEST_CASE("Tape: Saturation at zero is transparent, and at full it compresses rather than gets louder") {
	const auto firstTapPeak = [](float saturation, float level) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1; // 100 ms -> 4800
		s.timeMultiple = 1.0;
		s.feedback = 0.0f;
		s.tone = 0.0f;
		s.spread = 0.0f;
		s.mode = Mode::Tape;
		s.wow = 0.0f;
		s.flutter = 0.0f;
		s.hfLoss = 0.0f;
		s.saturation = saturation;
		d.setTarget(s);
		std::vector<float> l(8000, 0.0f);
		std::vector<float> r(8000, 0.0f);
		l[0] = level;
		r[0] = level;
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		return l[4800];
	};

	// **A SATURATION KNOB THAT COLOURS THE SOUND AT 0% IS A KNOB WHOSE RANGE STARTS IN THE WRONG
	// PLACE.** `tanh(d·x)/d` tends to `x` as `d` tends to zero, and this is the assertion that it
	// was implemented that way rather than as a bare `tanh(x)`, which bends 0.8 down to 0.66.
	CHECK(firstTapPeak(0.0f, 0.8f) == doctest::Approx(0.8));

	// AT FULL IT COMPRESSES: the ceiling is `1/drive`, so a hot input comes back well below itself…
	const float hot = firstTapPeak(1.0f, 0.8f);
	CHECK(hot < 0.4f);
	CHECK(hot <= static_cast<float>(1.0 / kTapeMaxDrive) + 1.0e-4f);
	// …WHILE A QUIET ONE IS BARELY TOUCHED — unity small-signal gain, which is what separates a
	// saturator from an attenuator. A quiet input scaled down as much as a loud one is just a fader.
	const float quiet = firstTapPeak(1.0f, 0.01f);
	CHECK(quiet == doctest::Approx(0.01).epsilon(0.02));
}

TEST_CASE("Tape: HF Loss is per pass, so the repeats darken cumulatively") {
	// **"PER PASS" IS THE CLAIM, and one application of a filter cannot show it.** The second repeat
	// has been through the loss twice, so for a broadband impulse its PEAK falls faster than the
	// feedback alone explains. Compared against the same machine with the loss off, where the ratio
	// between repeats is the feedback and nothing else.
	const auto repeats = [](float hfLoss) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1; // 100 ms -> 4800
		s.timeMultiple = 1.0;
		s.feedback = 0.8f;
		s.tone = 0.0f;
		s.spread = 0.0f;
		s.mode = Mode::Tape;
		s.wow = 0.0f;
		s.flutter = 0.0f;
		s.saturation = 0.0f;
		s.hfLoss = hfLoss;
		d.setTarget(s);
		std::vector<float> l(20000, 0.0f);
		std::vector<float> r(20000, 0.0f);
		renderImpulse(d, l, r);
		const auto peakNear = [&l](size_t at) {
			float best = 0.0f;
			for (size_t i = at - 40; i < at + 400 && i < l.size(); ++i) {
				best = std::max(best, std::fabs(l[i]));
			}
			return best;
		};
		return std::array<float, 3> { peakNear(4800), peakNear(9600), peakNear(14400) };
	};

	const auto open = repeats(0.0f);
	const auto lossy = repeats(1.0f);

	// WITH THE LOSS OFF the repeats fall by the feedback, near enough (the wide-open pole at 18 kHz
	// takes a little off an impulse's peak, which is why this is a band and not an equality).
	CHECK(open[1] / open[0] > 0.6f);

	// WITH IT ON each repeat loses more than the feedback takes…
	CHECK(lossy[1] / lossy[0] < open[1] / open[0]);
	// …AND THE THIRD HAS LOST MORE AGAIN, relative to the open machine, than the second had. That is
	// what "cumulative" means and what a filter on the OUTPUT would not do: there, every repeat
	// would be darkened once and the ratios between them would match the open machine's.
	CHECK(lossy[2] / open[2] < lossy[1] / open[1]);
}

TEST_CASE("the tape controls do nothing outside Tape mode") {
	// **THE OTHER HALF OF THE GATE.** The card hides these four outside Tape; this is the engine
	// agreeing. A control hidden on the card but live in the DSP is the honesty rule failing in the
	// direction nobody looks — the sound changes and no control on screen accounts for it.
	const auto render = [](float all) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1;
		s.timeMultiple = 1.0;
		s.feedback = 0.6f;
		s.mode = Mode::Clean;
		s.wow = all;
		s.flutter = all;
		s.saturation = all;
		s.hfLoss = all;
		d.setTarget(s);
		std::vector<float> l(20000, 0.0f);
		std::vector<float> r(20000, 0.0f);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.7f * std::sin(0.03f * static_cast<float>(i));
			r[i] = l[i];
		}
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		return l;
	};
	CHECK(render(0.0f) == render(1.0f));
}

namespace {

/// A multi-tap machine with everything that is not the claim turned off: wet only, no spread, flat
/// tone, 100 ms base (4800 frames), level taps, centred.
Settings multiTapBase(int taps) {
	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1; // 100 ms -> 4800
	s.timeMultiple = 1.0;
	s.feedback = 0.0f;
	s.tone = 0.0f;
	s.spread = 0.0f;
	s.mode = Mode::MultiTap;
	s.taps = taps;
	s.tapPattern = patternCoordForShape(0.0f);
	// LEVEL TAPS, EXACTLY: the Arch family at its centre is every gain 1 (`tapLevelOf`), where Fade can only come within
	// a rounding of a level slope through its coordinate (backlog MH).
	s.tapLevels = tapCoordIn(static_cast<int>(TapLevels::Arch), 0.5f);
	s.tapWidth = 0.0f;
	return s;
}

std::pair<std::vector<float>, std::vector<float>> multiTapImpulse(const Settings& s, size_t frames,
                                                                  bool leftOnly = false) {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	d.setTarget(s);
	std::vector<float> l(frames, 0.0f);
	std::vector<float> r(frames, 0.0f);
	l[0] = 1.0f;
	r[0] = leftOnly ? 0.0f : 1.0f;
	d.processStereo(l.data(), r.data(), static_cast<int>(frames));
	return { l, r };
}

} // namespace

TEST_CASE("Multi-Tap: the taps land at fractions of the base, and the last one lands AT it") {
	// **WHERE THE TAPS ARE IS A LIST OF NUMBERS, so that is what is asserted.** Four even taps of a
	// 4800-frame base are at 1200, 2400, 3600 and 4800 — and the last being AT the base is the
	// property that keeps `Sync` and `Time` meaning the length of the pattern in this mode.
	SUBCASE("four even taps") {
		const auto out = multiTapImpulse(multiTapBase(4), 8000);
		// Four level taps, power-normalised: 1/sqrt(4) each.
		for (const size_t at : { size_t { 1200 }, size_t { 2400 }, size_t { 3600 }, size_t { 4800 } }) {
			CAPTURE(at);
			CHECK(out.first[at] == doctest::Approx(0.5));
		}
		// AND NOTHING BETWEEN THEM — a tap in the wrong place would also put energy somewhere.
		CHECK(out.first[600] == doctest::Approx(0.0));
		CHECK(out.first[1800] == doctest::Approx(0.0));
		CHECK(out.first[4200] == doctest::Approx(0.0));
	}

	SUBCASE("Shape right of centre squares the spacing, and the last tap does not move") {
		Settings s = multiTapBase(4);
		s.tapPattern = patternCoordForShape(1.0f); // p = 2: (k/4)^2 of 4800 -> 300, 1200, 2700, 4800
		const auto out = multiTapImpulse(s, 8000);
		for (const size_t at : { size_t { 300 }, size_t { 1200 }, size_t { 2700 }, size_t { 4800 } }) {
			CAPTURE(at);
			CHECK(out.first[at] == doctest::Approx(0.5));
		}
		// the even positions that are NOT also squared positions are now empty
		CHECK(out.first[2400] == doctest::Approx(0.0));
		CHECK(out.first[3600] == doctest::Approx(0.0));
	}

	SUBCASE("Taps changes how many there are, not how long the pattern is") {
		const auto two = multiTapImpulse(multiTapBase(2), 8000);
		CHECK(two.first[2400] == doctest::Approx(1.0 / std::sqrt(2.0)));
		CHECK(two.first[4800] == doctest::Approx(1.0 / std::sqrt(2.0)));
		CHECK(two.first[1200] == doctest::Approx(0.0));
		const auto eight = multiTapImpulse(multiTapBase(8), 8000);
		CHECK(eight.first[600] == doctest::Approx(1.0 / std::sqrt(8.0)));
		CHECK(eight.first[4800] == doctest::Approx(1.0 / std::sqrt(8.0)));
	}
}

TEST_CASE("Multi-Tap: the pattern REPEATS through the loop rather than re-tapping itself") {
	// **THE DECISION THAT MAKES IT A MULTI-TAP DELAY RATHER THAN A DIFFUSER.** The loop is fed from
	// the base tap's raw read. Fed from the summed taps instead, every tap re-enters the line and
	// re-taps: the first tap's echo would land at 1200 + 1200 = 2400 ON TOP of the second tap, and
	// cycle two would be sixteen echoes instead of four.
	//
	// So the claim is two numbers: cycle two is cycle one again at the feedback's level, and the
	// taps of cycle one are NOT contaminated by re-tapped copies of each other.
	Settings s = multiTapBase(4);
	s.feedback = 0.5f;
	const auto out = multiTapImpulse(s, 12000);

	// CYCLE ONE, uncontaminated: exactly the no-feedback levels.
	CHECK(out.first[1200] == doctest::Approx(0.5));
	CHECK(out.first[2400] == doctest::Approx(0.5)); // <- 0.5 + a re-tap if the sum fed the loop
	CHECK(out.first[3600] == doctest::Approx(0.5));
	CHECK(out.first[4800] == doctest::Approx(0.5));

	// CYCLE TWO: the same four, one base later, at the feedback's level.
	CHECK(out.first[6000] == doctest::Approx(0.25));
	CHECK(out.first[7200] == doctest::Approx(0.25));
	CHECK(out.first[8400] == doctest::Approx(0.25));
	CHECK(out.first[9600] == doctest::Approx(0.25));
}

TEST_CASE("Multi-Tap: Slope shapes what is heard and leaves the loop alone") {
	Settings s = multiTapBase(4);
	s.tapLevels = levelsCoordForSlope(-1.0f); // full fade-out: gains 1, 2/3, 1/3, 0
	s.feedback = 0.5f;
	const auto out = multiTapImpulse(s, 12000);

	// The heard taps fall linearly to nothing…
	const float norm = 1.0f / std::sqrt(1.0f + 4.0f / 9.0f + 1.0f / 9.0f);
	CHECK(out.first[1200] == doctest::Approx(norm));
	CHECK(out.first[2400] == doctest::Approx(norm * 2.0f / 3.0f));
	CHECK(out.first[3600] == doctest::Approx(norm / 3.0f));
	CHECK(out.first[4800] == doctest::Approx(0.0)); // the last tap is SILENT to the ear…

	// …**AND THE PATTERN STILL REPEATS**, because the loop reads the base tap raw rather than
	// through its gain. A loop fed through the gain would be starved here and cycle two would be
	// silence — `Slope` quietly acting as a second Feedback knob.
	CHECK(out.first[6000] == doctest::Approx(norm * 0.5f));
}

TEST_CASE("Multi-Tap: Width alternates the taps across the field") {
	Settings s = multiTapBase(4);
	s.tapWidth = 1.0f;
	const auto out = multiTapImpulse(s, 8000);
	// Hard left, hard right, in turn. Each channel carries two unit taps: norm = 1/sqrt(2).
	const float g = 1.0f / std::sqrt(2.0f);
	CHECK(out.first[1200] == doctest::Approx(g));
	CHECK(out.second[1200] == doctest::Approx(0.0));
	CHECK(out.first[2400] == doctest::Approx(0.0));
	CHECK(out.second[2400] == doctest::Approx(g));
	CHECK(out.first[3600] == doctest::Approx(g));
	CHECK(out.second[3600] == doctest::Approx(0.0));
	CHECK(out.second[4800] == doctest::Approx(g));

	SUBCASE("and at zero every tap is centred") {
		const auto centred = multiTapImpulse(multiTapBase(4), 8000);
		for (const size_t at : { size_t { 1200 }, size_t { 2400 }, size_t { 3600 }, size_t { 4800 } }) {
			CHECK(centred.first[at] == doctest::Approx(centred.second[at]));
		}
	}
}

TEST_CASE("the multi-tap controls do nothing outside Multi-Tap mode") {
	// The engine's half of the gate — see the Tape case of the same name for why it is asked.
	const auto render = [](int taps, float rest) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1;
		s.timeMultiple = 1.0;
		s.feedback = 0.6f;
		s.mode = Mode::Clean;
		s.taps = taps;
		s.tapPattern = patternCoordForShape(rest);
		s.tapLevels = levelsCoordForSlope(rest);
		s.tapWidth = std::fabs(rest);
		d.setTarget(s);
		std::vector<float> l(20000, 0.0f);
		std::vector<float> r(20000, 0.0f);
		for (size_t i = 0; i < l.size(); ++i) {
			l[i] = 0.7f * std::sin(0.03f * static_cast<float>(i));
			r[i] = l[i];
		}
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		return l;
	};
	CHECK(render(2, 0.0f) == render(8, 1.0f));
}

//------------------------------------------------------------------------------------------------//
// THE TAPS STRIP'S READINGS (backlog LF).                                                        //
//                                                                                                //
// The strip draws what the device publishes, so the claim is about the PUBLISHED NUMBERS: that    //
// they are the heads the device actually read at, and that the strip's expansion of them lands    //
// where the audio does. Each restated independently of `delay.cpp` — the tap from the sync list  //
// by hand, the multi-tap geometry from its formula, the tape's wobble from its declared depth.    //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE HOST'S EAR, as a test owns it: the last publish, kept whole.
struct Captured {
	std::array<float, kReadingCount> values {};
	size_t count = 0;
	int publishes = 0;
};

void attach(Delay& d, Captured& c) {
	d.setReadingsSink(device::ReadingsSink { &c, [](void* context, const float* values, size_t count) {
		                                        auto* self = static_cast<Captured*>(context);
		                                        self->count = count;
		                                        for (size_t i = 0; i < std::min(count, self->values.size()); ++i) {
			                                        self->values[i] = values[i];
		                                        }
		                                        ++self->publishes;
	                                        } });
}

TapPicture decoded(const Captured& c) {
	TapPicture p;
	REQUIRE(decodeTapPicture(c.values.data(), c.count, &p));
	return p;
}

/// `frames` of a steady sine through `d`, in blocks of 256 — the grains want something to read, and the
/// strip's numbers do not depend on what it is.
void runSine(Delay& d, int frames) {
	std::vector<float> l(256), r(256);
	for (int done = 0; done < frames; done += 256) {
		const int n = std::min(256, frames - done);
		for (int i = 0; i < n; ++i) {
			l[static_cast<size_t>(i)] = 0.5f * std::sin(0.02f * static_cast<float>(done + i));
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		d.processStereo(l.data(), r.data(), n);
	}
}

bool near(double a, double b, double tol = 1.0e-6) { return std::fabs(a - b) <= tol; }

} // namespace

TEST_CASE("taps strip: the readings are the heads the device read at, in every mode, sync on and off") {
	// THE TAP, BY HAND: 100 ms from the free list, or a 1/8 at 120 BPM (0.25 s), each times x1.5; Spread
	// 40% pushes the right channel a quarter of that further, so 1.1x.
	struct Sync {
		size_t choice;
		double base;
	};
	for (const Sync sync : { Sync { 1, 0.100 }, Sync { kDefaultSync, 0.250 } }) {
		for (int m = 0; m < static_cast<int>(Mode::kCount); ++m) {
			const auto mode = static_cast<Mode>(m);
			CAPTURE(sync.choice);
			CAPTURE(m);
			Delay d;
			d.prepare(kRate);
			d.setTransportClock(at120());
			Captured c;
			attach(d, c);
			Settings s = defaults();
			s.sync = sync.choice;
			s.timeMultiple = 1.5;
			s.spread = 0.4f;
			s.mode = mode;
			d.setTarget(s);
			constexpr int kFrames = 9600;
			runSine(d, kFrames);

			REQUIRE(c.count == kReadingCount);
			CHECK(c.publishes == kFrames / 256 + 1);
			const TapPicture p = decoded(c);
			const double tapL = sync.base * 1.5;
			const double tapR = tapL * (1.0 + kSpreadFraction * 0.4);

			// THE AXIS'S UNIT IS SYNC'S BASE ALONE — the multiplier is the hand's, read off the field.
			CHECK(near(p.axisUnit, sync.base));
			CHECK(near(p.feedback, 0.35));
			CHECK(p.pingPong == false);
			CHECK(near(p.wetGain, 0.35));
			CHECK(near(p.dryGain, 0.65));

			// AND THE PUBLISHED HEADS ARE THE ENGINE'S OWN, read through its seam rather than its readings.
			CHECK(near(p.head[0], d.headSecondsForTest(0)));
			CHECK(near(p.head[1], d.headSecondsForTest(1)));

			switch (mode) {
			case Mode::Clean:
			case Mode::Xfade:
			case Mode::Bbd: // one glided head, as Clean (2026-11-05)
				CHECK(p.layout == TapLayout::Single);
				CHECK(near(p.head[0], tapL));
				CHECK(near(p.head[1], tapR));
				CHECK(p.tapCount == 1);
				CHECK(p.fadeNew == 1.0f);
				CHECK(p.fadeOld == 0.0f);
				break;
			case Mode::Tape: {
				CHECK(p.layout == TapLayout::Single);
				// **THE WOBBLE IS ON THE HEAD**, restated from its declared depths: each component's
				// deviation split evenly, `A = deviation * sr / 2 pi f`, read at the last sample's phase.
				constexpr double kTwoPi = 6.28318530717958647692;
				const double j = static_cast<double>(kFrames - 1);
				const auto component = [&](double share, double maxDev, double hz) {
					const double depth = 0.5 * share * maxDev * kRate / (kTwoPi * hz);
					const double phase = j * hz / kRate;
					return depth * std::sin(kTwoPi * (phase - std::floor(phase)));
				};
				const double wobble = component(0.3, kWowMaxDeviation, kWowHzA)
				                      + component(0.3, kWowMaxDeviation, kWowHzB)
				                      + component(0.25, kFlutterMaxDeviation, kFlutterHzA)
				                      + component(0.25, kFlutterMaxDeviation, kFlutterHzB);
				REQUIRE(std::fabs(wobble) > 1.0); // a frame or more, or this would not show the wobble at all
				CHECK(near(p.head[0], tapL + wobble / kRate));
				CHECK(near(p.head[1], tapR + wobble / kRate));
				break;
			}
			case Mode::Granular: {
				CHECK(p.layout == TapLayout::Granular);
				CHECK(near(p.head[0], tapL));
				// THE RIGHT CHANNEL'S GRAINS READ THE SPREAD BEHIND THE LEFT'S — the same tap as the others.
				CHECK(near(p.head[1], tapR));
				CHECK(near(p.grainSpread, tapR - tapL));
				// ONE PASS'S WINDOW at Pitch 0 is the spray alone: 25% of half the tap, either way.
				CHECK(near(p.passEarly, 0.25 * kSprayFraction * tapL));
				CHECK(near(p.passLate, 0.25 * kSprayFraction * tapL));
				REQUIRE(d.activeGrainsForTest() > 0);
				CHECK(p.grainCount == std::min(d.activeGrainsForTest(), kGrainReadings));
				for (size_t i = 0; i < p.grainCount; ++i) {
					CAPTURE(i);
					CHECK(std::fabs(p.grainDelay[i] - tapL) <= p.passEarly + 1.0e-6);
					CHECK(p.grainWeight[i] >= 0.0f);
					CHECK(p.grainWeight[i] <= 1.0f);
				}
				break;
			}
			case Mode::MultiTap: {
				CHECK(p.layout == TapLayout::MultiTap);
				CHECK(near(p.head[0], tapL));
				CHECK(near(p.head[1], tapR));
				// THE GEOMETRY, FROM ITS FORMULA: four taps at k/4 of the head (Shape 0), levels running
				// down by Slope -0.35, alternating sides by Width 0.5, power-normalised on the louder side.
				REQUIRE(p.tapCount == 4);
				std::array<double, 4> gl {};
				std::array<double, 4> gr {};
				double eL = 0.0;
				double eR = 0.0;
				for (int k = 0; k < 4; ++k) {
					const double level = 1.0 - 0.35 * static_cast<double>(k) / 3.0;
					const double pan = (k % 2 == 0) ? -0.5 : 0.5;
					gl[static_cast<size_t>(k)] = level * (pan > 0.0 ? 1.0 - pan : 1.0);
					gr[static_cast<size_t>(k)] = level * (pan < 0.0 ? 1.0 + pan : 1.0);
					eL += gl[static_cast<size_t>(k)] * gl[static_cast<size_t>(k)];
					eR += gr[static_cast<size_t>(k)] * gr[static_cast<size_t>(k)];
				}
				const double norm = 1.0 / std::sqrt(std::max(eL, eR));
				for (size_t k = 0; k < 4; ++k) {
					CAPTURE(k);
					CHECK(near(p.tapFrac[k], static_cast<double>(k + 1) / 4.0));
					CHECK(near(p.tapGain[0][k], gl[k] * norm, 1.0e-5));
					CHECK(near(p.tapGain[1][k], gr[k] * norm, 1.0e-5));
					CHECK(near(p.tapGain[0][k], d.tapGainForTest(0, k), 1.0e-6));
					CHECK(near(p.tapGain[1][k], d.tapGainForTest(1, k), 1.0e-6));
				}
				break;
			}
			case Mode::Reverse: {
				// THE CHUNK IS THE TAP, AND THE PHASE IS WHERE THE LEADING HEAD IS IN IT: seated at frame 0 and
				// turning over every chunk, so after `kFrames` it is `kFrames mod chunk` in (backlog MB).
				CHECK(p.layout == TapLayout::Reverse);
				CHECK(near(p.head[0], tapL));
				CHECK(near(p.head[1], tapR));
				CHECK(p.tapCount == 1);
				for (int ch = 0; ch < 2; ++ch) {
					CAPTURE(ch);
					const double chunk = std::round((ch == 0 ? tapL : tapR) * kRate);
					CHECK(near(d.reverseChunkSecondsForTest(ch), chunk / kRate));
					CHECK(near(p.revPhase[static_cast<size_t>(ch)], std::fmod(kFrames, chunk) / chunk, 1.0e-6));
				}
				break;
			}
			case Mode::kCount:
				break;
			}
		}
	}
}

TEST_CASE("taps strip: a moving Time publishes where the head HAS GOT TO, and an Xfade publishes both heads") {
	SUBCASE("a glide: the head travels at the bound, half a frame a sample, behind the target") {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Captured c;
		attach(d, c);
		Settings s = defaults();
		s.sync = 1; // 100 ms -> 4800 frames
		s.timeMultiple = 1.0;
		s.mode = Mode::Clean;
		d.setTarget(s);
		runSine(d, 256);
		CHECK(near(decoded(c).head[0], 0.100));
		// THE HAND (or a modulator) DOUBLES IT. The one-pole asks for 4800 * (1 - e^(-1/3840)) = 1.25 frames
		// on the first sample, past the 0.5 bound, so for 256 samples the head moves exactly 0.5 a sample;
		// the last sample READ 255 steps in.
		s.timeMultiple = 2.0;
		d.setTarget(s);
		runSine(d, 256);
		CHECK(near(d.tapSecondsForTest(), 0.200));
		CHECK(near(decoded(c).head[0], (4800.0 + 255.0 * kGlideMaxFramesPerSample) / kRate));
	}
	SUBCASE("an Xfade: the old head and the new, at the weights the last sample used") {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Captured c;
		attach(d, c);
		Settings s = defaults();
		s.sync = 1;
		s.timeMultiple = 1.0;
		s.mode = Mode::Xfade;
		d.setTarget(s);
		runSine(d, 256);
		s.timeMultiple = 2.0;
		d.setTarget(s);
		runSine(d, 64);
		const TapPicture p = decoded(c);
		CHECK(near(p.head[0], 0.200));
		CHECK(near(p.fadeFrom[0], 0.100));
		// 64 samples into a 15 ms fade (720 samples): the last read at 63/720 of the quarter turn.
		const double angle = 63.0 / (kXfadeSeconds * kRate) * 1.57079632679;
		CHECK(near(p.fadeNew, std::sin(angle), 1.0e-5));
		CHECK(near(p.fadeOld, std::cos(angle), 1.0e-5));
		CHECK(near(p.fadeNew, d.fadeNewForTest()));
		CHECK(near(p.fadeOld, d.fadeOldForTest()));
		// AND THE STRIP DRAWS BOTH on the first pass.
		const auto ticks = stripTicks(p, 1.0);
		int firstPassLeftOwn = 0;
		for (const StripTick& t : ticks) {
			if (t.pass == 0 && t.channel == 0 && t.origin == 0) {
				++firstPassLeftOwn;
			}
		}
		CHECK(firstPassLeftOwn == 2);
	}
}

TEST_CASE("taps strip: the strip draws exactly the arrivals the audio has, ping-pong and spread included") {
	// **THE EXPANSION AGAINST THE DEVICE'S OWN OUTPUT.** One impulse into ONE input; every tick the strip
	// draws for that input must be a sample of exactly its gain on exactly its channel, and the other
	// channel must be silent there. Spread 40% makes the two heads differ, so a crossed arrival lands
	// at a time no uncrossed one can — which is what pins left against right.
	for (const bool pingPong : { false, true }) {
		for (const Mode mode : { Mode::Clean, Mode::MultiTap }) {
			for (int input = 0; input < 2; ++input) {
				CAPTURE(static_cast<int>(pingPong));
				CAPTURE(static_cast<int>(mode));
				CAPTURE(input);
				Delay d;
				d.prepare(kRate);
				d.setTransportClock(at120());
				Captured c;
				attach(d, c);
				Settings s = defaults();
				s.mix = 1.0f;
				s.sync = 1; // 100 ms -> 4800; the right channel 5280
				s.timeMultiple = 1.0;
				s.feedback = 0.5f;
				s.tone = 0.0f;
				s.spread = 0.4f;
				s.pingPong = pingPong ? PingPong::Stereo : PingPong::Off;
				s.mode = mode;
				s.taps = 3;
				s.tapPattern = patternCoordForShape(0.0f);
				s.tapLevels = levelsCoordForSlope(-0.5f);
				s.tapWidth = 0.6f;
				d.setTarget(s);
				std::vector<float> l(40000, 0.0f);
				std::vector<float> r(40000, 0.0f);
				(input == 0 ? l : r)[0] = 1.0f;
				d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

				const TapPicture p = decoded(c);
				const auto ticks = stripTicks(p, 0.8);
				int checked = 0;
				for (const StripTick& t : ticks) {
					if (t.origin != input || t.pass < 0) {
						continue;
					}
					CAPTURE(t.seconds);
					CAPTURE(t.channel);
					CAPTURE(t.pass);
					const auto at = static_cast<size_t>(std::lround(t.seconds * kRate));
					REQUIRE(at < l.size());
					const std::vector<float>& heard = t.channel == 0 ? l : r;
					const std::vector<float>& other = t.channel == 0 ? r : l;
					CHECK(std::fabs(heard[at] - t.gain) < 1.0e-5f);
					CHECK(std::fabs(other[at]) < 1.0e-6f);
					++checked;
				}
				// NOT VACUOUS: the first pass and at least three repeats (Multi-Tap: three taps each).
				CHECK(checked >= (mode == Mode::MultiTap ? 12 : 4));
				// **AND THE CONVERSE: EVERY ARRIVAL THE AUDIO HAS, THE STRIP DRAWS.** The loop above cannot see
				// a tick the strip LEFT OUT — it only visits the ticks that exist. So walk the audio: every
				// sample above the strip's floor inside the span is a drawn tick on that channel, at that time.
				const auto spanFrames = static_cast<size_t>(0.8 * kRate);
				for (int ch = 0; ch < 2; ++ch) {
					const std::vector<float>& heard = ch == 0 ? l : r;
					for (size_t i = 1; i + 1 < spanFrames; ++i) { // inside the span: an edge tick rounds either way
						if (!(std::fabs(heard[i]) > kStripGainFloor * 1.01f)) {
							continue;
						}
						CAPTURE(ch);
						CAPTURE(i);
						bool drawn = false;
						for (const StripTick& t : ticks) {
							drawn = drawn
							        || (t.origin == input && t.channel == ch && t.pass >= 0
							            && static_cast<size_t>(std::lround(t.seconds * kRate)) == i);
						}
						CHECK(drawn);
					}
				}
				// **PING-PONG ALTERNATES THE OUTPUT PASS BY PASS; WITHOUT IT, AN INPUT STAYS ON ITS SIDE.**
				for (const StripTick& t : ticks) {
					if (t.origin == input && t.pass >= 0) {
						CAPTURE(t.pass);
						CHECK(t.channel == (pingPong ? (input + t.pass) % 2 : input));
					}
				}
			}
		}
	}
}

//------------------------------------------------------------------------------------------------//
// PING-PONG: OFF, STEREO, CLASSIC.                                                          //
//------------------------------------------------------------------------------------------------//

namespace {

/// ONE IMPULSE ON THE INPUTS GIVEN, through Clean at 100 ms, Feedback 50%, wet only, flat, no Spread.
std::pair<std::vector<float>, std::vector<float>> pingPongImpulse(PingPong pp, float inL, float inR, Mode mode = Mode::Clean) {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1; // 100 ms -> 4800
	s.timeMultiple = 1.0;
	s.feedback = 0.5f;
	s.tone = 0.0f;
	s.spread = 0.0f;
	s.mode = mode;
	s.pingPong = pp;
	d.setTarget(s);
	std::vector<float> l(40000, 0.0f);
	std::vector<float> r(40000, 0.0f);
	l[0] = inL;
	r[0] = inR;
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	return { l, r };
}

} // namespace

TEST_CASE("Ping-Pong Classic: a centred sound bounces — L only, R only, L only — where Stereo repeats it on both sides") {
	// **THE ARRIVAL TABLE, BY VALUE** (the protocol for the strip's ticks on both sides at once). Repeat k lands at
	// k x 4800 at 0.5^(k-1). Classic: the mono sum enters the LEFT line, so odd repeats are left only, even ones right only.
	const auto [cl, cr] = pingPongImpulse(PingPong::Classic, 1.0f, 1.0f);
	const auto [sl, sr] = pingPongImpulse(PingPong::Stereo, 1.0f, 1.0f);
	for (size_t k = 1; k <= 6; ++k) {
		CAPTURE(k);
		const size_t at = k * 4800;
		const auto level = static_cast<float>(std::pow(0.5, static_cast<double>(k - 1)));
		const bool left = (k % 2) == 1;
		CHECK(cl[at] == doctest::Approx(left ? level : 0.0f));
		CHECK(cr[at] == doctest::Approx(left ? 0.0f : level));
		CHECK(std::fabs(left ? cr[at] : cl[at]) < 1.0e-7f); // the other side EXACTLY quiet
		// AND STEREO, THE OLD `On`, IS UNCHANGED: a centred sound on both sides at once.
		CHECK(sl[at] == doctest::Approx(level));
		CHECK(sr[at] == doctest::Approx(level));
	}
	// NOTHING ELSE ARRIVES: the six repeats are the whole response.
	for (size_t i = 1; i < 6 * 4800 + 1; ++i) {
		if (i % 4800 != 0) {
			CAPTURE(i);
			REQUIRE(cl[i] == 0.0f);
			REQUIRE(cr[i] == 0.0f);
		}
	}
	SUBCASE("a hard-left or hard-right sound enters at half, on the LEFT either way — the input is summed to mono") {
		for (int side = 0; side < 2; ++side) {
			CAPTURE(side);
			const auto [l, r] = pingPongImpulse(PingPong::Classic, side == 0 ? 1.0f : 0.0f, side == 0 ? 0.0f : 1.0f);
			CHECK(l[4800] == doctest::Approx(0.5f));
			CHECK(r[4800] == 0.0f);
			CHECK(l[9600] == 0.0f);
			CHECK(r[9600] == doctest::Approx(0.25f));
		}
	}
	SUBCASE("in every mode: the first repeat of a centred sound is on the left alone") {
		for (int m = 0; m < static_cast<int>(Mode::kCount); ++m) {
			CAPTURE(m);
			const auto [l, r] = pingPongImpulse(PingPong::Classic, 1.0f, 1.0f, static_cast<Mode>(m));
			float peakL = 0.0f;
			float peakR = 0.0f;
			for (size_t i = 1; i < 4800 + 600; ++i) { // the first pass and its spray, before any crossed repeat (Multi-Tap: 6000)
				peakL = std::max(peakL, std::fabs(l[i]));
				peakR = std::max(peakR, std::fabs(r[i]));
			}
			// (A GRAIN CATCHING ONE IMPULSE IS CHANCE at 20 grains a second, so Granular is held only to the silent right.)
			CHECK((static_cast<Mode>(m) == Mode::Granular || peakL > 0.05f));
			CHECK(peakR == 0.0f);
		}
	}
}

TEST_CASE("Ping-Pong: an old patch's On is Stereo, and Classic is a third label rather than a reused number") {
	const device::ParamDesc& p = kParamDescs[static_cast<size_t>(Param::PingPong)];
	REQUIRE(p.names.size() == 3u);
	CHECK(std::string_view(p.names[0]) == "Off");
	CHECK(std::string_view(p.names[1]) == "Stereo");
	CHECK(std::string_view(p.names[2]) == "Classic");
	REQUIRE(p.choiceAliases.size() == 1u);
	CHECK(std::string_view(p.choiceAliases[0].from) == "On");
	CHECK(std::string_view(p.choiceAliases[0].to) == "Stereo");
	CHECK(std::string_view(p.key) == "delay_ping_pong");
	// THE ROW ROUND-TRIPS ALL THREE.
	for (int v = 0; v < 3; ++v) {
		State s {};
		setValueOf(&s, Param::PingPong, static_cast<float>(v));
		CHECK(static_cast<int>(s.settings.pingPong) == v);
		CHECK(valueOf(s, Param::PingPong) == static_cast<float>(v));
	}
}

TEST_CASE("Ping-Pong Off and Stereo render exactly what Off and On rendered before Classic existed") {
	// **BIT-IDENTITY, PINNED** against the base commit (069e4aec), where the control was Off/On: the same varied run in
	// every mode, the control set through the ROW (0 and 1) so the same source compiled on both sides of the change.
	constexpr uint64_t kBefore[5][2] = {
		{ 12132660070385994937ull, 11945431633193679884ull }, // Clean
		{ 7557455269966070638ull, 658852885498340416ull },     // Granular
		{ 12132660070385994937ull, 11945431633193679884ull }, // Xfade (no Time move here, so Clean's samples)
		{ 10059250209216719347ull, 9600843503445172251ull },  // Tape
		{ 6115197750883036662ull, 9219119414863966793ull },   // Multi-Tap
	};
	for (int m = 0; m < 5; ++m) {
		for (int pp = 0; pp < 2; ++pp) {
			CAPTURE(m);
			CAPTURE(pp);
			Delay d;
			d.prepare(kRate);
			d.setTransportClock(at120());
			State st {};
			st.settings = defaults();
			st.settings.mode = static_cast<Mode>(m);
			st.settings.sync = 1;
			st.settings.timeMultiple = 1.3;
			st.settings.feedback = 0.7f;
			st.settings.tone = -0.2f;
			st.settings.spread = 0.3f;
			st.settings.mix = 0.6f;
			setValueOf(&st, Param::PingPong, static_cast<float>(pp));
			d.setTarget(st.settings);
			constexpr size_t kFrames = 24000;
			std::vector<float> l(kFrames), r(kFrames);
			uint32_t seed = 0x2468aceu;
			for (size_t i = 0; i < kFrames; ++i) {
				seed = seed * 1664525u + 1013904223u;
				const float noise = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
				l[i] = 0.4f * std::sin(0.013f * static_cast<float>(i)) + 0.1f * noise;
				r[i] = 0.3f * std::sin(0.029f * static_cast<float>(i)) - 0.1f * noise;
			}
			for (size_t done = 0; done < kFrames; done += 300) {
				d.processStereo(l.data() + done, r.data() + done, static_cast<int>(std::min<size_t>(300, kFrames - done)));
			}
			uint64_t h = 1469598103934665603ull;
			for (const std::vector<float>* v : { &l, &r }) {
				for (const float x : *v) {
					uint32_t bits = 0;
					std::memcpy(&bits, &x, sizeof bits);
					h = (h ^ bits) * 1099511628211ull;
				}
			}
			if (h != kBefore[m][pp]) {
				MESSAGE("mode " << m << " pingpong " << pp << " hash " << h);
			}
#if defined(__x86_64__) && defined(__linux__) // the hash was taken on Linux x86-64; arm64 rounds differently (CLAUDE.md: platform-pinned)
			CHECK(h == kBefore[m][pp]);
#else
		(void)h; // computed on every platform, compared only where it was pinned
#endif
		}
	}
}

TEST_CASE("taps strip: Classic Ping-Pong draws ONE chain that bounces, exactly the arrivals a centred impulse has") {
	for (const Mode mode : { Mode::Clean, Mode::MultiTap }) {
		CAPTURE(static_cast<int>(mode));
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Captured c;
		attach(d, c);
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 1;
		s.timeMultiple = 1.0;
		s.feedback = 0.5f;
		s.tone = 0.0f;
		s.spread = 0.4f;
		s.pingPong = PingPong::Classic;
		s.mode = mode;
		s.taps = 3;
		s.tapLevels = levelsCoordForSlope(-0.5f);
		s.tapWidth = 0.6f;
		d.setTarget(s);
		std::vector<float> l(40000, 0.0f);
		std::vector<float> r(40000, 0.0f);
		l[0] = 1.0f; // A CENTRED impulse — the strip's picture is an impulse on both inputs
		r[0] = 1.0f;
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		const TapPicture p = decoded(c);
		CHECK(p.pingPong);
		CHECK(p.classic);
		const auto ticks = stripTicks(p, 0.8);
		int checked = 0;
		for (const StripTick& t : ticks) {
			if (t.pass < 0) {
				continue;
			}
			CAPTURE(t.seconds);
			CAPTURE(t.channel);
			CAPTURE(t.pass);
			// ONE CHAIN, ENTERING ON THE LEFT, AND IT ALTERNATES PASS BY PASS.
			CHECK(t.origin == 0);
			CHECK(t.channel == t.pass % 2);
			const auto at = static_cast<size_t>(std::lround(t.seconds * kRate));
			REQUIRE(at < l.size());
			const std::vector<float>& heard = t.channel == 0 ? l : r;
			const std::vector<float>& other = t.channel == 0 ? r : l;
			CHECK(std::fabs(heard[at] - t.gain) < 1.0e-5f);
			CHECK(std::fabs(other[at]) < 1.0e-6f);
			++checked;
		}
		CHECK(checked >= (mode == Mode::MultiTap ? 12 : 4));
		// THE CONVERSE: every sample above the floor is a drawn tick on its side.
		const auto spanFrames = static_cast<size_t>(0.8 * kRate);
		for (int ch = 0; ch < 2; ++ch) {
			const std::vector<float>& heard = ch == 0 ? l : r;
			for (size_t i = 1; i + 1 < spanFrames; ++i) {
				if (!(std::fabs(heard[i]) > kStripGainFloor * 1.01f)) {
					continue;
				}
				CAPTURE(ch);
				CAPTURE(i);
				bool drawn = false;
				for (const StripTick& t : ticks) {
					drawn = drawn || (t.channel == ch && t.pass >= 0 && static_cast<size_t>(std::lround(t.seconds * kRate)) == i);
				}
				CHECK(drawn);
			}
		}
	}
}


//------------------------------------------------------------------------------------------------//
// REVERSE (backlog MB).                                                                          //
//                                                                                                //
// Every claim below is checked against an INDEPENDENT RESTATEMENT of the intended path, not      //
// against another render: the line is `in + feedback·line(t − L)` (the forward loop), and the    //
// output is, for every mirror `M` on the half-chunk grid with `0 ≤ m − M < L`,                    //
// `sin²(π(m − M)/L) · line(2M − m)`. With the heads seated at frame 0 the grid is every multiple  //
// of `L/2`, and the trailing head's first mirror is `−L/2`, which reads before the recording.     //
//------------------------------------------------------------------------------------------------//

namespace {

/// THE LINE: what the device writes, with the loop flat (Tone 0) and below the clip — `in + fb·line(t − L)`.
std::vector<double> reverseLine(const std::vector<float>& in, size_t chunk, double feedback) {
	std::vector<double> line(in.size(), 0.0);
	for (size_t t = 0; t < in.size(); ++t) {
		line[t] = static_cast<double>(in[t]) + (t >= chunk ? feedback * line[t - chunk] : 0.0);
	}
	return line;
}

/// THE OUTPUT AT `m`, restated: every mirror within one chunk behind `m`, at its `sin²` weight.
double reverseAt(const std::vector<double>& line, size_t chunk, size_t m) {
	const auto L = static_cast<long long>(chunk);
	const auto mm = static_cast<long long>(m);
	double y = 0.0;
	// THE GRID IS EVERY L/2 FROM -L/2; the mirrors with 0 <= m - M < L are the last two at or before m.
	for (long long M = -L / 2; M <= mm; M += L / 2) {
		const long long into = mm - M;
		if (into < 0 || into >= L) {
			continue;
		}
		const long long at = 2 * M - mm;
		const double sn = std::sin(3.14159265358979323846 * static_cast<double>(into) / static_cast<double>(L));
		if (at >= 0 && at < static_cast<long long>(line.size()) && into > 0) {
			y += sn * sn * line[static_cast<size_t>(at)];
		}
	}
	return y;
}

/// A RAMP OF 64 RISING SAMPLES at `start`, on both inputs — the asymmetric probe: forwards it rises, reversed it falls.
std::vector<float> rampAt(size_t frames, size_t start) {
	std::vector<float> v(frames, 0.0f);
	for (size_t j = 0; j < 64; ++j) {
		v[start + j] = static_cast<float>(j + 1) / 64.0f;
	}
	return v;
}

Settings reverseBase() {
	Settings s = defaults();
	s.mix = 1.0f; // wet only, so every sample is the reverse's
	s.sync = 1;   // 100 ms -> a 4800-frame chunk
	s.timeMultiple = 1.0;
	s.feedback = 0.0f;
	s.tone = 0.0f;
	s.spread = 0.0f;
	s.mode = Mode::Reverse;
	return s;
}

} // namespace

TEST_CASE("Reverse: a ramp comes back reversed, mirrored about the chunk edges, and its two returns sum to it") {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	d.setTarget(reverseBase());
	constexpr size_t kChunk = 4800;
	constexpr size_t kStart = 3000; // 1800 before the leading head's next mirror, at 4800
	std::vector<float> l = rampAt(20000, kStart);
	std::vector<float> r = l;
	const std::vector<double> line = reverseLine(l, kChunk, 0.0);
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	CHECK(near(d.reverseChunkSecondsForTest(0), 0.1));
	CHECK(near(d.tapSecondsForTest(), 0.1));

	// THE WHOLE OUTPUT, BOTH CHANNELS, AGAINST THE RESTATEMENT.
	for (size_t m = 0; m < l.size(); ++m) {
		CAPTURE(m);
		const double want = reverseAt(line, kChunk, m);
		REQUIRE(std::fabs(l[m] - want) < 1.0e-6);
		REQUIRE(std::fabs(r[m] - want) < 1.0e-6);
	}

	// **AND THE CLAIMS IN THE TERMS A HAND USES.** The probe's sample j (rising, (j+1)/64) went in at 3000+j. The mirror
	// at 4800 plays it back at 9600 − (3000+j) = 6600 − j, and the one half a chunk later, at 7200, at 11400 − j: two
	// returns, exactly one Time apart, each within two Times of the input, the window putting sin² on one and cos² on
	// the other so they add back to the sample.
	for (size_t j = 0; j < 64; ++j) {
		CAPTURE(j);
		const double v = static_cast<double>(j + 1) / 64.0;
		const size_t first = 6600 - j;
		const size_t second = 11400 - j;
		CHECK(second - first == kChunk);
		CHECK(first - (kStart + j) <= 2 * kChunk);
		const double into = 4800.0 - static_cast<double>(kStart + j);
		const double wA = std::pow(std::sin(3.14159265358979323846 * into / 4800.0), 2.0);
		CHECK(std::fabs(l[first] - wA * v) < 1.0e-6);
		CHECK(std::fabs(l[second] - (1.0 - wA) * v) < 1.0e-6);
		CHECK(std::fabs(l[first] + l[second] - v) < 1.0e-6);
	}
	// REVERSED: walking each return forwards in time (j falling) the values FALL — the ramp rose going in.
	for (size_t m = 6537 + 1; m <= 6600; ++m) {
		CAPTURE(m);
		CHECK(l[m] < l[m - 1]); // a falling ramp: the rising one, backwards
	}
	for (size_t m = 11337 + 1; m <= 11400; ++m) {
		CAPTURE(m);
		CHECK(l[m] < l[m - 1]);
	}
	// NOT VACUOUS: the returns are there and loud, and nothing else is.
	CHECK(l[6537] > 0.3f);
	CHECK(l[11337] > 0.1f); // the cos² return, about 0.18 of the ramp
}

TEST_CASE("Reverse: no step at a chunk edge — each head is silent at the instant it jumps") {
	// A STEADY SINE (0.5 at 0.02 rad/sample, so its own step is at most 0.01) through the reverse. Reversed, it is the
	// same sine running the other way, with the same step bound; the window adds at most (π/L)·|a − b| ≤ π/L between
	// the two heads. A head that jumped at full weight would step by up to the sine's whole swing.
	for (const float spread : { 0.0f, 0.4f }) {
		CAPTURE(spread);
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = reverseBase();
		s.spread = spread;
		d.setTarget(s);
		constexpr size_t kFrames = 48000;
		std::vector<float> l(kFrames), r(kFrames);
		for (size_t i = 0; i < kFrames; ++i) {
			l[i] = 0.5f * std::sin(0.02f * static_cast<float>(i));
			r[i] = l[i];
		}
		const std::vector<float> in = l;
		for (size_t done = 0; done < kFrames; done += 256) {
			const auto n = static_cast<int>(std::min<size_t>(256, kFrames - done));
			d.processStereo(l.data() + done, r.data() + done, n);
		}
		for (int ch = 0; ch < 2; ++ch) {
			CAPTURE(ch);
			const size_t chunk = ch == 0 ? 4800 : static_cast<size_t>(std::lround(4800.0 * (1.0 + kSpreadFraction * spread)));
			const double bound = 0.0101 + 3.14159265358979323846 / static_cast<double>(chunk);
			const std::vector<float>& y = ch == 0 ? l : r;
			double worstEdge = 0.0;
			double worstHard = 0.0;
			// FROM TWO CHUNKS IN, where the line behind every head is full of sine.
			for (size_t m = 2 * chunk; m < kFrames; ++m) {
				const double step = std::fabs(static_cast<double>(y[m]) - static_cast<double>(y[m - 1]));
				CAPTURE(m);
				REQUIRE(step <= bound);
				if (m % (chunk / 2) == 0) {
					worstEdge = std::max(worstEdge, step);
				}
				if (m % chunk == 0) {
					// THE SAME EDGE UNWINDOWED, restated: the leading head alone. At `m` it has just jumped to the
					// mirror `m` and reads one frame back, `m − 1`; a sample earlier it was at the end of the chunk
					// before, mirror `m − L`, reading `2(m − L) − (m − 1)`, the chunk's OLDEST sample.
					const double hardNow = in[m - 1];
					const double hardBefore = in[m + 1 - 2 * chunk];
					worstHard = std::max(worstHard, std::fabs(hardNow - hardBefore));
				}
			}
			CAPTURE(worstEdge);
			CAPTURE(worstHard);
			CHECK(worstEdge <= bound);
			// NOT VACUOUS: without the window the edges WOULD step, by far more than the bound.
			CHECK(worstHard > 10.0 * bound);
		}
	}
}

TEST_CASE("Reverse: a synced Time sets the chunk, and the tempo moves it") {
	for (const double secondsPerBeat : { 0.5, 0.4 }) {
		CAPTURE(secondsPerBeat);
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(device::TransportClock::fromSecondsPerBeat(secondsPerBeat));
		Settings s = reverseBase();
		s.sync = kDefaultSync; // 1/8: half a beat
		d.setTarget(s);
		const auto chunk = static_cast<size_t>(std::lround(0.5 * secondsPerBeat * kRate)); // 12000, then 9600
		std::vector<float> l(40000, 0.0f);
		l[1000] = 1.0f;
		std::vector<float> r = l;
		const std::vector<double> line = reverseLine(l, chunk, 0.0);
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		CHECK(near(d.reverseChunkSecondsForTest(0), 0.5 * secondsPerBeat));
		for (size_t m = 0; m < l.size(); ++m) {
			CAPTURE(m);
			REQUIRE(std::fabs(l[m] - reverseAt(line, chunk, m)) < 1.0e-6);
		}
		// BY HAND: the impulse at 1000 is mirrored about the half-chunk grid's next two points after it.
		const size_t m1 = chunk / 2;
		const size_t m2 = chunk;
		CHECK(l[2 * m1 - 1000] > 0.0f);
		CHECK(l[2 * m2 - 1000] > 0.0f);
		CHECK(std::fabs(l[2 * m1 - 1000] + l[2 * m2 - 1000] - 1.0f) < 1.0e-6f);
	}
}

TEST_CASE("Reverse: each repeat is the reversed chunk again, one Time later, at the feedback's level") {
	// **THE FEEDBACK IS THE FORWARD READ, SO EVERY REPEAT STAYS REVERSED.** Fed back as heard, the second pass would be
	// the first reversed again — the ramp would come back RISING on every other repeat.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = reverseBase();
	s.feedback = 0.5f;
	d.setTarget(s);
	constexpr size_t kChunk = 4800;
	constexpr size_t kStart = 3000;
	std::vector<float> l = rampAt(40000, kStart);
	std::vector<float> r = l;
	const std::vector<double> line = reverseLine(l, kChunk, 0.5);
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	for (size_t m = 0; m < l.size(); ++m) {
		CAPTURE(m);
		REQUIRE(std::fabs(l[m] - reverseAt(line, kChunk, m)) < 1.0e-5);
	}
	// BY HAND, pass by pass: pass k is the first pass shifted k chunks and scaled 0.5^k — and still FALLING.
	for (size_t k = 0; k < 4; ++k) {
		CAPTURE(k);
		const double level = std::pow(0.5, static_cast<double>(k));
		for (size_t j = 0; j < 64; ++j) {
			CAPTURE(j);
			const double v = static_cast<double>(j + 1) / 64.0;
			const double into = 4800.0 - static_cast<double>(kStart + j);
			const double wA = std::pow(std::sin(3.14159265358979323846 * into / 4800.0), 2.0);
			// THE FIRST RETURN OF PASS k, at sin²; and from pass 1 on, pass k−1's SECOND return (cos², twice the level)
			// lands on the same sample, because it is one Time after its first. Both reversed, so they add.
			const size_t at = 6600 - j + k * kChunk;
			const double expect = k == 0 ? wA * v : level * v * (wA + 2.0 * (1.0 - wA));
			CHECK(std::fabs(l[at] - expect) < 1.0e-6);
		}
		const size_t lo = 6537 + k * kChunk;
		for (size_t m = lo + 1; m <= lo + 63; ++m) {
			CAPTURE(m);
			CHECK(l[m] < l[m - 1]);
		}
	}
}

TEST_CASE("Reverse: the tail reaches EXACT zero, and so does every other mode's") {
	for (int m = 0; m < static_cast<int>(Mode::kCount); ++m) {
		CAPTURE(m);
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mix = 1.0f;
		s.sync = 0; // 10 ms
		s.timeMultiple = 1.0;
		s.feedback = 0.5f;
		s.tone = 0.0f;
		s.mode = static_cast<Mode>(m);
		d.setTarget(s);
		std::vector<float> l(2048, 0.0f);
		std::vector<float> r(2048, 0.0f);
		renderImpulse(d, l, r);
		CHECK_FALSE(d.silentForTest());
		for (int b = 0; b < 400 && !d.silentForTest(); ++b) {
			std::fill(l.begin(), l.end(), 0.0f);
			std::fill(r.begin(), r.end(), 0.0f);
			d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		}
		CHECK(d.silentForTest());
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
		for (size_t i = 0; i < l.size(); ++i) {
			REQUIRE(l[i] == 0.0f);
			REQUIRE(r[i] == 0.0f);
		}
	}
}

TEST_CASE("Reverse: every control the card shows changes the sound, and every one it hides does not") {
	// **THE HONESTY RULE, BOTH DIRECTIONS, IN THIS MODE.** What the card shows is `device::relevantFor` against Mode =
	// Reverse — the same question `GenericCard` asks. Each control is moved off its default alone and the render compared
	// with the default's; shown means it must differ, hidden means it must be the same to the bit.
	const auto render = [](const State& state) {
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		d.setTarget(state.settings);
		constexpr size_t kFrames = 24000;
		std::vector<float> l(kFrames), r(kFrames);
		for (size_t i = 0; i < kFrames; ++i) {
			// TWO DIFFERENT INPUTS, so Ping-Pong's crossing is audible.
			l[i] = 0.5f * std::sin(0.031f * static_cast<float>(i));
			r[i] = 0.4f * std::sin(0.017f * static_cast<float>(i) + 1.0f);
		}
		d.processStereo(l.data(), r.data(), static_cast<int>(kFrames));
		l.insert(l.end(), r.begin(), r.end());
		return l;
	};
	State base {};
	base.settings = reverseBase();
	base.settings.mix = 0.5f;
	base.settings.feedback = 0.5f;
	const std::vector<float> reference = render(base);
	REQUIRE(std::any_of(reference.begin(), reference.end(), [](float v) { return std::fabs(v) > 0.1f; }));
	int shown = 0;
	int hidden = 0;
	for (size_t p = 0; p < static_cast<size_t>(Param::kCount); ++p) {
		const auto param = static_cast<Param>(p);
		// THE S4 PAIR THAT CANNOT SPEAK ALONE (delay-love): Duck Release moves only a Duck off centre, and Clear is an EDGE —
		// a level held from the first block is none. And Duck, which this case moves to −1, a full GATE, which a steady loud
		// sine holds wide open — no change, honestly. Each has its own cases in `test_delay_lane`.
		if (param == Param::Enabled || param == Param::Mode || param == Param::DuckRelease || param == Param::Clear
		    || param == Param::Duck) {
			continue;
		}
		const device::ParamDesc& desc = kParamDescs[p];
		CAPTURE(desc.name);
		const float now = valueOf(base, param);
		// THE FAR END FROM WHERE IT SITS — for a switch, the other state.
		const float moved = std::fabs(now - static_cast<float>(desc.max)) > std::fabs(now - static_cast<float>(desc.min))
		                        ? static_cast<float>(desc.max)
		                        : static_cast<float>(desc.min);
		State changed = base;
		setValueOf(&changed, param, moved);
		REQUIRE(valueOf(changed, param) != now);
		const bool relevant = device::relevantFor(desc, static_cast<double>(static_cast<int>(Mode::Reverse)));
		CAPTURE(relevant);
		CHECK((render(changed) != reference) == relevant);
		(relevant ? shown : hidden) += 1;
	}
	// THE COUNTS, so a gate that silently changed would show here too: Sync, Time, Feedback, Tone, Ping-Pong, Spread,
	// Mix and Dub shown, and Freeze (delay-love S4); the four grain, four tape and four multi-tap controls hidden.
	CHECK(shown == 9); // and Freeze (S4), which holds whatever the line has
	CHECK(hidden == 17); // and Swarm and the BBD four (2026-11-05)
}

TEST_CASE("Reverse: the strip draws exactly the arrivals the audio has, wherever in its chunk the impulse lands") {
	// The LF case's shape for Reverse, with one difference that is the point: a reverse arrival depends on WHERE in its
	// chunk the line is when the impulse goes in, so the picture is taken AFTER a priming run, and the impulse is the
	// very next sample — the instant the published phase describes.
	for (const size_t prime : { size_t { 0 }, size_t { 1234 }, size_t { 4100 } }) {
		for (const PingPong pingPong : { PingPong::Off, PingPong::Stereo, PingPong::Classic }) {
			for (int input = 0; input < 2; ++input) {
				CAPTURE(prime);
				CAPTURE(static_cast<int>(pingPong));
				CAPTURE(input);
				Delay d;
				d.prepare(kRate);
				d.setTransportClock(at120());
				Captured c;
				attach(d, c);
				Settings s = reverseBase();
				s.feedback = 0.5f;
				// THE RIGHT CHUNK 5400. Half rather than the LF case's 0.4, because 0.4f is not exact and a chunk of 5280.0000072
				// frames walks its mirrors off the sample grid, a little further each chunk.
				s.spread = 0.5f;
				s.pingPong = pingPong;
				if (pingPong == PingPong::Classic && input == 1) {
					continue; // CLASSIC IS ONE CHAIN, a centred impulse, drawn as origin 0
				}
				d.setTarget(s);
				std::vector<float> pl(std::max<size_t>(prime, 1), 0.0f);
				std::vector<float> pr(std::max<size_t>(prime, 1), 0.0f);
				d.processStereo(pl.data(), pr.data(), static_cast<int>(prime == 0 ? 1 : prime));
				const TapPicture p = decoded(c);
				REQUIRE(p.layout == TapLayout::Reverse);
				std::vector<float> l(40000, 0.0f);
				std::vector<float> r(40000, 0.0f);
				(input == 0 ? l : r)[0] = 1.0f;
				if (pingPong == PingPong::Classic) {
					r[0] = 1.0f; // centred: Classic sums both into the left line
				}
				d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

				const auto ticks = stripTicks(p, 0.8);
				// **TWO RETURNS CAN LAND ON ONE SAMPLE, AND THEN WHAT IS HEARD IS THEIR SUM.** A pass's two returns are one
				// chunk apart and each pass is one chunk after the last, so pass k's later return falls exactly on pass
				// k+1's earlier one — both drawn, as two ticks; the audio has one sample carrying both.
				//
				// **AND ONE OF THE TWO MAY BE UNDER THE FLOOR WHILE THEIR SUM IS OVER IT.** The strip floors each arrival
				// on its own, so the only thing it may leave out of a sample is a part smaller than the floor — never
				// more, and never anything extra (the difference is never negative).
				const auto undrawnOk = [](float missing) {
					return missing > -1.0e-5f && missing < kStripGainFloor;
				};
				const auto drawnAt = [&](int ch, size_t i) {
					float sum = 0.0f;
					for (const StripTick& t : ticks) {
						if (t.origin == input && t.channel == ch && t.pass >= 0
						    && static_cast<size_t>(std::lround(t.seconds * kRate)) == i) {
							sum += t.gain;
						}
					}
					return sum;
				};
				int checked = 0;
				for (const StripTick& t : ticks) {
					if (t.origin != input || t.pass < 0) {
						continue;
					}
					CAPTURE(t.seconds);
					CAPTURE(t.channel);
					CAPTURE(t.pass);
					const auto at = static_cast<size_t>(std::lround(t.seconds * kRate));
					REQUIRE(at < l.size());
					const std::vector<float>& heard = t.channel == 0 ? l : r;
					CHECK(undrawnOk(heard[at] - drawnAt(t.channel, at)));
					// THE WHISKER IS THE CHUNK IT IS HEARD IN, and the tick sits inside it.
					CHECK(t.early >= 0.0);
					CHECK(t.late > 0.0);
					CHECK(near(t.early + t.late, p.head[static_cast<size_t>(t.channel)], 1.0e-6));
					++checked;
				}
				CHECK(checked >= 6);
				// THE CONVERSE: every arrival above the floor inside the span is a drawn tick.
				const auto spanFrames = static_cast<size_t>(0.8 * kRate);
				for (int ch = 0; ch < 2; ++ch) {
					const std::vector<float>& heard = ch == 0 ? l : r;
					for (size_t i = 1; i + 1 < spanFrames; ++i) {
						if (!(std::fabs(heard[i]) > kStripGainFloor * 1.01f)) {
							continue;
						}
						CAPTURE(ch);
						CAPTURE(i);
						CHECK(undrawnOk(heard[i] - drawnAt(ch, i)));
					}
				}
			}
		}
	}
}

TEST_CASE("Reverse is appended: every other mode renders exactly what it rendered before Reverse existed") {
	// **BIT-IDENTITY, PINNED.** Each mode before MB, through a varied arrangement (Ping-Pong, Spread, a tilt, a Time
	// change mid-run so the glide and the crossfade run, partitioned blocks), hashed to the bit. The hashes were taken
	// from the base commit (069e4aec) with this same case, before any Reverse code existed.
	// NOT RE-PINNED BY THE SONG LOCK (swarm-seed, 2026-11-05: Granular's spray and grid follow the song while the transport
	// PLAYS): this case's clock is tempo only, stopped, so Granular here is the free-running road and its hash holds.
	// The locked road is pinned by value in test_delay_lane ("Granular at Swarm 0 on a playing transport").
	constexpr uint64_t kBefore[5][2] = {
		{ 4041540501156196413ull, 9701567240405602275ull },   // Clean
		{ 14750341262547897042ull, 17005230659754523574ull }, // Granular
		{ 13250104186186188277ull, 11765695336750829885ull }, // Xfade
		{ 6106809880562900538ull, 14603129618117831405ull },  // Tape
		{ 17065128062658915373ull, 8012860432561908459ull },  // Multi-Tap
	};
	const auto hashOf = [](const std::vector<float>& a, const std::vector<float>& b) {
		uint64_t h = 1469598103934665603ull;
		for (const std::vector<float>* v : { &a, &b }) {
			for (const float x : *v) {
				uint32_t bits = 0;
				std::memcpy(&bits, &x, sizeof bits);
				h = (h ^ bits) * 1099511628211ull;
			}
		}
		return h;
	};
	for (int m = 0; m < 5; ++m) {
		for (int variant = 0; variant < 2; ++variant) {
			CAPTURE(m);
			CAPTURE(variant);
			Delay d;
			d.prepare(kRate);
			d.setTransportClock(at120());
			Settings s = defaults();
			s.mode = static_cast<Mode>(m);
			s.sync = variant == 0 ? 1 : kDefaultSync;
			s.timeMultiple = 1.3;
			s.feedback = 0.7f;
			s.tone = variant == 0 ? -0.3f : 0.4f;
			s.spread = 0.3f;
			s.pingPong = variant == 0 ? PingPong::Stereo : PingPong::Off;
			s.dub = variant == 1;
			s.mix = 0.6f;
			d.setTarget(s);
			constexpr size_t kFrames = 30000;
			std::vector<float> l(kFrames), r(kFrames);
			uint32_t seed = 0x1234567u;
			for (size_t i = 0; i < kFrames; ++i) {
				seed = seed * 1664525u + 1013904223u;
				const float noise = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
				l[i] = 0.4f * std::sin(0.013f * static_cast<float>(i)) + 0.1f * noise;
				r[i] = 0.3f * std::sin(0.029f * static_cast<float>(i)) - 0.1f * noise;
			}
			for (size_t done = 0; done < kFrames; done += 300) {
				if (done == kFrames / 2) {
					s.timeMultiple = 0.8;
					d.setTarget(s);
				}
				const auto n = static_cast<int>(std::min<size_t>(300, kFrames - done));
				d.processStereo(l.data() + done, r.data() + done, n);
			}
			const uint64_t h = hashOf(l, r);
			if (h != kBefore[m][variant]) {
				MESSAGE("mode " << m << " variant " << variant << " hash " << h);
			}
#if defined(__x86_64__) && defined(__linux__) // the hash was taken on Linux x86-64; arm64 rounds differently (CLAUDE.md: platform-pinned)
			CHECK(h == kBefore[m][variant]);
#else
		(void)h; // computed on every platform, compared only where it was pinned
#endif
		}
	}
}

TEST_CASE("Reverse: the chunk stops at half the line, and every other mode still reaches the whole line") {
	// A CHUNK READS BACK TO TWICE ITS LENGTH (`kReverseMaxFraction`), so Reverse's tap is clamped at half the line;
	// the clamp must not leak into the modes that read only once.
	for (int m = 0; m < static_cast<int>(Mode::kCount); ++m) {
		CAPTURE(m);
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(device::TransportClock::fromSecondsPerBeat(1.0)); // 60 BPM
		Settings s = defaults();
		s.sync = kSyncNoteChoice - 1; // the longest division: far past 16 s at x4
		s.timeMultiple = 4.0;
		s.mode = static_cast<Mode>(m);
		d.setTarget(s);
		std::vector<float> l(256, 0.0f), r(256, 0.0f);
		d.processStereo(l.data(), r.data(), 256);
		if (s.mode == Mode::Reverse) {
			const double lineFrames = kMaxDelaySeconds * kRate;
			CHECK(d.tapSecondsForTest() == doctest::Approx((lineFrames - 2.0) * kReverseMaxFraction / kRate));
			CHECK(d.reverseChunkSecondsForTest(0) == doctest::Approx((lineFrames - 2.0) * kReverseMaxFraction / kRate));
		}
		else {
			CHECK(d.tapSecondsForTest() == doctest::Approx(kMaxDelaySeconds));
			// AND THE HEAD THE SAMPLE LOOP PLACED IS PAST HALF THE LINE — the clamp is not on the read either.
			CHECK(d.headSecondsForTest(0) > 0.75 * kMaxDelaySeconds);
		}
	}
}

//------------------------------------------------------------------------------------------------//
// MULTI-TAP UNDER MOVING CONTROLS (backlog MD).                                              //
//------------------------------------------------------------------------------------------------//

TEST_CASE("MD: where nothing moves, Multi-Tap and Reverse render exactly what they rendered before MD") {
	// **BIT-IDENTITY, PINNED**, beside the MB case above (which already pins the five older modes at default taps):
	// Multi-Tap at two, five and eight taps with Shape, Slope and Width off their defaults, Spread on, and a Time
	// change mid-run (the head's glide is not MD's to change), plus Reverse. The hashes were taken from the base
	// commit (3113f6e4) with this same case, before any MD code existed.
	// **ARRANGEMENT 1 RE-PINNED FOR BACKLOG MH (2026-10-30).** Shape now arrives as a zoned coordinate, and 0.45 comes back
	// through it within a rounding of itself rather than to the bit, so its render moved by that rounding; the other three
	// (−0.6, 0.9 and Reverse) still match the pre-MD hashes exactly, which is what this case pins for MD.
	constexpr uint64_t kBefore[4] = { 7331649104543081227ull, 3306836962085708367ull, 10615122473985989193ull,
	                                 1899099145083361903ull };
	const auto hashOf = [](const std::vector<float>& a, const std::vector<float>& b) {
		uint64_t h = 1469598103934665603ull;
		for (const std::vector<float>* v : { &a, &b }) {
			for (const float x : *v) {
				uint32_t bits = 0;
				std::memcpy(&bits, &x, sizeof bits);
				h = (h ^ bits) * 1099511628211ull;
			}
		}
		return h;
	};
	struct Arrangement {
		Mode mode;
		int taps;
		float shape;
	};
	const Arrangement arrangements[4] = {
		{ Mode::MultiTap, 2, -0.6f }, { Mode::MultiTap, 5, 0.45f }, { Mode::MultiTap, 8, 0.9f }, { Mode::Reverse, 4, 0.0f }
	};
	for (size_t a = 0; a < 4; ++a) {
		CAPTURE(a);
		Delay d;
		d.prepare(kRate);
		d.setTransportClock(at120());
		Settings s = defaults();
		s.mode = arrangements[a].mode;
		s.taps = arrangements[a].taps;
		s.tapPattern = patternCoordForShape(arrangements[a].shape);
		s.tapLevels = levelsCoordForSlope(0.4f);
		s.tapWidth = 0.7f;
		s.sync = 1;
		s.timeMultiple = 1.3;
		s.feedback = 0.6f;
		s.tone = -0.2f;
		s.spread = 0.3f;
		s.pingPong = PingPong::Stereo;
		s.mix = 0.6f;
		d.setTarget(s);
		constexpr size_t kFrames = 30000;
		std::vector<float> l(kFrames), r(kFrames);
		uint32_t seed = 0x7654321u;
		for (size_t i = 0; i < kFrames; ++i) {
			seed = seed * 1664525u + 1013904223u;
			const float noise = static_cast<float>(seed >> 8) / 16777216.0f - 0.5f;
			l[i] = 0.4f * std::sin(0.017f * static_cast<float>(i)) + 0.1f * noise;
			r[i] = 0.3f * std::sin(0.023f * static_cast<float>(i)) - 0.1f * noise;
		}
		for (size_t done = 0; done < kFrames; done += 200) {
			if (done == kFrames / 2) {
				s.timeMultiple = 0.7;
				d.setTarget(s);
			}
			const auto n = static_cast<int>(std::min<size_t>(200, kFrames - done));
			d.processStereo(l.data() + done, r.data() + done, n);
		}
		const uint64_t h = hashOf(l, r);
		if (h != kBefore[a]) {
			MESSAGE("arrangement " << a << " hash " << h);
		}
#if defined(__x86_64__) && defined(__linux__) // the hash was taken on Linux x86-64; arm64 rounds differently (CLAUDE.md: platform-pinned)
		CHECK(h == kBefore[a]);
#else
		(void)h; // computed on every platform, compared only where it was pinned
#endif
	}
}

namespace {

/// THE WORST SAMPLE STEP WHILE `move` RUNS, as a multiple of the most a sinusoid of the output's own local level
/// can step: `2·A·sin(ω/2)`, with A the largest sample within a period and a half either side. **NOT AGAINST THE
/// STILL OUTPUT'S STEP**, as the Time case above measures, because here the taps comb the tone: eight level taps
/// nearly null it at rest, and moving them legitimately brings it back up, which a still reference reads as a
/// click. A smooth output stays at or under 1 however its level moves; a jumped read does not. Controls are moved
/// every 64 frames, as a modulator moves them.
float multiTapStepRatio(int taps, const std::function<void(Settings&, int)>& move, int frames) {
	constexpr float kOmega = 0.05f;
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = multiTapBase(taps);
	s.feedback = 0.4f;
	s.timeMultiple = 2.0; // 200 ms -> 9600 frames
	d.setTarget(s);
	const int warm = 24000;
	std::vector<float> l(static_cast<size_t>(warm + frames));
	std::vector<float> r(l.size());
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = 0.5f * std::sin(kOmega * static_cast<float>(i));
		r[i] = l[i];
	}
	for (int at = 0; at < static_cast<int>(l.size()); at += 64) {
		if (at >= warm) {
			Settings m = s;
			move(m, at - warm);
			d.setTarget(m);
		}
		d.processStereo(l.data() + at, r.data() + at, std::min(64, static_cast<int>(l.size()) - at));
	}
	constexpr size_t kReach = 192; // a period and a half of the tone
	float worst = 0.0f;
	float loudest = 0.0f;
	for (size_t i = static_cast<size_t>(warm); i + kReach < l.size(); ++i) {
		float peak = 0.0f;
		for (size_t j = i - kReach; j <= i + kReach; ++j) {
			peak = std::max(peak, std::fabs(l[j]));
		}
		loudest = std::max(loudest, peak);
		const float bound = 2.0f * peak * std::sin(0.5f * kOmega);
		worst = std::max(worst, std::fabs(l[i] - l[i - 1]) / std::max(bound, 1.0e-4f));
	}
	REQUIRE(loudest > 0.05f); // NOT VACUOUS: the tone is through the taps
	return worst;
}

} // namespace

TEST_CASE("MD: moving Shape or Taps does not step the signal — the taps crossfade and their gains ramp") {
	// Shape made bad artefacts and clicks when modulated or moved by hand.
	// **MEASURED BEFORE THE FIX** (`bench_delay_multitap`): a Shape sweep stepped the output by 6.2-6.7x its own
	// worst still step, an LFO on Shape by 24x, one Taps step by 6.1x. The tap times were re-derived per block and
	// read at once, so every tap jumped by `head · Δfrac`. Measured by THIS case's ratio (the step against the most a
	// sinusoid at the local level can step): 7.9-26 with the fades and ramps taken out, 1.01-1.23 with them. The
	// bound is the Time case's 2: a jump lives well above it, and an honest moving tap below.
	SUBCASE("a hand sweeping Shape end to end in a second, at eight taps and at four") {
		const auto sweep = [](Settings& s, int at) {
			const double t = static_cast<double>(at) / kRate;
			s.tapPattern = patternCoordForShape(static_cast<float>(t < 1.0 ? -1.0 + 2.0 * t : 3.0 - 2.0 * t));
		};
		CHECK(multiTapStepRatio(8, sweep, 96000) < 2.0f);
		CHECK(multiTapStepRatio(4, sweep, 96000) < 2.0f);
	}
	SUBCASE("an LFO on Shape, fast and full, and slow and shallow") {
		CHECK(multiTapStepRatio(8,
		                        [](Settings& s, int at) {
			                        s.tapPattern = patternCoordForShape(static_cast<float>(std::sin(6.283185307 * 2.0 * at / kRate)));
		                        },
		                        96000)
		      < 2.0f);
		CHECK(multiTapStepRatio(8,
		                        [](Settings& s, int at) {
			                        s.tapPattern = patternCoordForShape(static_cast<float>(0.3 * std::sin(6.283185307 * 0.2 * at / kRate)));
		                        },
		                        96000)
		      < 2.0f);
	}
	SUBCASE("Taps stepped by a hand, four to five and back") {
		CHECK(multiTapStepRatio(4, [](Settings& s, int at) { s.taps = (at / 24000) % 2 == 0 ? 5 : 4; }, 96000) < 2.0f);
	}
}

TEST_CASE("MD: a tap that moves crossfades to its new place, and once the fade lands it reads exactly there") {
	// Four even taps of 4800 (slots counted back from the base: slot 3 is the first tap, at 1/4), then Shape to
	// the right end: (k/4)^2 of 4800 -> 300, 1200, 2700, 4800.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = multiTapBase(4);
	d.setTarget(s);
	std::vector<float> l(8000, 0.0f);
	std::vector<float> r(8000, 0.0f);
	d.processStereo(l.data(), r.data(), 256);
	s.tapPattern = patternCoordForShape(1.0f);
	d.setTarget(s);
	d.processStereo(l.data(), r.data(), 64);
	// MID-FADE: each moving tap is between its old place and its new one; the base tap never moves.
	const double fadeFrames = std::round(kTapFadeSeconds * kRate);
	for (size_t slot = 1; slot < 4; ++slot) {
		CAPTURE(slot);
		const double along = static_cast<double>(4 - slot) / 4.0;
		CHECK(d.slotFadingForTest(slot));
		CHECK(d.slotFromFractionForTest(slot) == along);
		CHECK(d.slotFractionForTest(slot) == std::pow(along, 2.0));
	}
	CHECK_FALSE(d.slotFadingForTest(0));
	CHECK(d.slotFractionForTest(0) == 1.0);
	// LANDED: one fade's worth later, no slot is fading.
	d.processStereo(l.data(), r.data(), static_cast<int>(fadeFrames));
	for (size_t slot = 0; slot < 4; ++slot) {
		CAPTURE(slot);
		CHECK_FALSE(d.slotFadingForTest(slot));
		CHECK(d.slotFractionForTest(slot) == std::pow(static_cast<double>(4 - slot) / 4.0, 2.0));
	}
	// AND IT READS EXACTLY THERE: an impulse now lands at the new places at full level, and at none of the old.
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	l[0] = 1.0f;
	r[0] = 1.0f;
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	for (const size_t at : { size_t { 300 }, size_t { 1200 }, size_t { 2700 }, size_t { 4800 } }) {
		CAPTURE(at);
		CHECK(l[at] == doctest::Approx(0.5));
		CHECK(r[at] == doctest::Approx(0.5));
	}
	CHECK(l[2400] == doctest::Approx(0.0));
	CHECK(l[3600] == doctest::Approx(0.0));
}

TEST_CASE("MD: a tap Taps adds fades in, and a tap it removes fades out where it stood") {
	// Four to five: the new tap is slot 4, at 1/5 of the head, fading in from silence over `kTapFadeSeconds`;
	// five back to four: it fades out. The gains are read off the slots, before the power normalisation.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = multiTapBase(4);
	d.setTarget(s);
	std::vector<float> l(4096, 0.0f);
	std::vector<float> r(4096, 0.0f);
	d.processStereo(l.data(), r.data(), 256);
	CHECK(d.slotGainForTest(4, 0) == 0.0f);
	const auto fadeFrames = static_cast<int>(std::lround(kTapFadeSeconds * kRate)); // 480

	s.taps = 5;
	d.setTarget(s);
	d.processStereo(l.data(), r.data(), 120);
	// A QUARTER OF THE WAY IN after a quarter of the fade — a ramp, not a step.
	CHECK(d.slotGainForTest(4, 0) == doctest::Approx(120.0 / fadeFrames).epsilon(1.0e-4));
	CHECK(d.slotGainForTest(4, 1) == doctest::Approx(120.0 / fadeFrames).epsilon(1.0e-4));
	CHECK(d.slotFractionForTest(4) == doctest::Approx(0.2));
	d.processStereo(l.data(), r.data(), fadeFrames);
	CHECK(d.slotGainForTest(4, 0) == 1.0f);
	CHECK(d.tapCountForTest() == 5);
	CHECK(d.tapGainForTest(0, 4) == doctest::Approx(1.0 / std::sqrt(5.0)));

	s.taps = 4;
	d.setTarget(s);
	d.processStereo(l.data(), r.data(), 360);
	CHECK(d.slotGainForTest(4, 0) == doctest::Approx(120.0 / fadeFrames).epsilon(1.0e-4));
	CHECK(d.slotFractionForTest(4) == doctest::Approx(0.2)); // fading out WHERE IT STOOD
	d.processStereo(l.data(), r.data(), fadeFrames);
	CHECK(d.slotGainForTest(4, 0) == 0.0f);
	CHECK(d.slotGainForTest(4, 1) == 0.0f);
	CHECK(d.tapCountForTest() == 4);
	CHECK(d.tapGainForTest(0, 0) == doctest::Approx(0.5));
}

TEST_CASE("MD: Taps is a modulation destination, still stepped, so a route steps it") {
	const fx::ParamDesc& taps = kParamDescs[static_cast<size_t>(Param::Taps)];
	CHECK(taps.modulatable);
	CHECK(taps.stepped);
	CHECK(std::string_view(taps.key) == "delay_taps"); // the same meaning, the same key
	// A ROUTE LANDS ON A WHOLE COUNT, inside the range.
	CHECK(device::withRouteOffset(taps, 4.0, 1.4) == 5.0);
	CHECK(device::withRouteOffset(taps, 4.0, 9.0) == static_cast<double>(kMaxTaps));
}

TEST_CASE("MD: a multi-tap tail with Shape and Taps still moving reaches EXACT zero") {
	// The crossfading heads read the same line as every other head, so `quietRun_` covers them (`holdsTail`,
	// backlog MC): a tail whose taps are still moving dies to zero like a still one.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = multiTapBase(6);
	s.sync = 0; // 10 ms
	s.feedback = 0.5f;
	d.setTarget(s);
	std::vector<float> l(2048, 0.0f);
	std::vector<float> r(2048, 0.0f);
	renderImpulse(d, l, r);
	CHECK(d.holdsTail());
	int block = 0;
	for (; block < 400 && !d.silentForTest(); ++block) {
		s.tapPattern = patternCoordForShape(static_cast<float>(std::sin(0.3 * block)));
		s.taps = 2 + block % 7;
		d.setTarget(s);
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	}
	CHECK(d.silentForTest());
	CHECK_FALSE(d.holdsTail());
	std::fill(l.begin(), l.end(), 0.0f);
	std::fill(r.begin(), r.end(), 0.0f);
	s.tapPattern = patternCoordForShape(-0.7f);
	s.taps = 3;
	d.setTarget(s);
	d.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	for (size_t i = 0; i < l.size(); ++i) {
		REQUIRE(l[i] == 0.0f);
		REQUIRE(r[i] == 0.0f);
	}
}

TEST_CASE("MD: a slow Shape LFO leaves the tone alone — the taps crossfade linearly, not at equal power") {
	// **THE LAW, BY ITS MEASUREMENT** (`bench_delay_multitap`, Shape at 0.2 Hz over 30%). A gentle LFO moves each
	// tap a little every fade, so the two reads are nearly one signal, and equal-power weights swell them by up to
	// 3 dB a fade, back to back: a ripple at the fade rate. Measured as the output's energy more than ~40 Hz from
	// the 440 Hz tone: -36 dB before MD, -19 dB with equal power, -39 dB linear. The bound sits between.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = defaults();
	s.mix = 1.0f;
	s.sync = 1;
	s.timeMultiple = 2.0;
	s.feedback = 0.4f;
	s.tone = 0.0f;
	s.spread = 0.0f;
	s.mode = Mode::MultiTap;
	s.taps = 8;
	d.setTarget(s);
	constexpr double kTwoPi = 6.28318530717958647692;
	constexpr int kWarm = 24000;
	constexpr int kFrames = kWarm + 4 * 48000;
	std::vector<float> l(kFrames), r(kFrames);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = 0.5f * static_cast<float>(std::sin(kTwoPi * 440.0 * static_cast<double>(i) / kRate));
		r[i] = l[i];
	}
	for (int at = 0; at < kFrames; at += 64) {
		if (at >= kWarm) {
			Settings m = s;
			m.tapPattern = patternCoordForShape(static_cast<float>(0.3 * std::sin(kTwoPi * 0.2 * static_cast<double>(at - kWarm) / kRate)));
			d.setTarget(m);
		}
		d.processStereo(l.data() + at, r.data() + at, std::min(64, kFrames - at));
	}
	// THE TONE IS WHAT A 12.5 ms AVERAGE OF THE 440 Hz DEMODULATION KEEPS; THE REST IS EVERYTHING ELSE.
	constexpr int kWindow = 600;
	const int n = kFrames - kWarm;
	std::vector<double> cr(static_cast<size_t>(n) + 1, 0.0), ci(static_cast<size_t>(n) + 1, 0.0);
	for (int i = 0; i < n; ++i) {
		const double ph = kTwoPi * 440.0 * static_cast<double>(i + kWarm) / kRate;
		const double y = l[static_cast<size_t>(i + kWarm)];
		cr[static_cast<size_t>(i) + 1] = cr[static_cast<size_t>(i)] + y * std::cos(ph);
		ci[static_cast<size_t>(i) + 1] = ci[static_cast<size_t>(i)] - y * std::sin(ph);
	}
	double total = 0.0;
	double rest = 0.0;
	for (int i = kWindow / 2; i < n - kWindow / 2; ++i) {
		const auto lo = static_cast<size_t>(i - kWindow / 2);
		const auto hi = static_cast<size_t>(i + kWindow / 2);
		const double re = (cr[hi] - cr[lo]) / kWindow;
		const double im = (ci[hi] - ci[lo]) / kWindow;
		const double ph = kTwoPi * 440.0 * static_cast<double>(i + kWarm) / kRate;
		const double y = l[static_cast<size_t>(i + kWarm)];
		const double tone = 2.0 * (re * std::cos(ph) - im * std::sin(ph));
		total += y * y;
		rest += (y - tone) * (y - tone);
	}
	REQUIRE(total > 1.0); // NOT VACUOUS: the tone is through the taps
	const double restDb = 10.0 * std::log10(rest / total);
	CAPTURE(restDb);
	CHECK(restDb < -30.0);
}

TEST_CASE("MD: the multi-tap's fast read returns the shared read's answer to the bit, wrap and unfilled line included") {
	// `readTap` replaces `readLerp`'s three modulos with an index stepped back from the write head; the claim is
	// that nothing else changed, so it is asserted sample for sample, bits and all, across every way the index
	// can fall: before the line has filled (a negative position), straddling the wrap, one frame behind, and
	// the whole line behind.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = defaults();
	s.mix = 1.0f;
	s.feedback = 0.0f;
	s.mode = Mode::Clean;
	d.setTarget(s);
	const size_t len = d.lineLengthForTest();
	REQUIRE(len > 1000);
	uint32_t seed = 0x2468aceu;
	std::vector<float> l(4096), r(4096);
	const auto feed = [&](size_t frames) {
		while (frames > 0) {
			const size_t n = std::min(frames, l.size());
			for (size_t i = 0; i < n; ++i) {
				seed = seed * 1664525u + 1013904223u;
				l[i] = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
				seed = seed * 1664525u + 1013904223u;
				r[i] = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
			}
			d.processStereo(l.data(), r.data(), static_cast<int>(n));
			frames -= n;
		}
	};
	const auto sameBits = [](float a, float b) {
		uint32_t x = 0;
		uint32_t y = 0;
		std::memcpy(&x, &a, sizeof x);
		std::memcpy(&y, &b, sizeof y);
		return x == y;
	};
	int compared = 0;
	int differing = 0;
	const auto sweep = [&]() {
		const auto now = static_cast<double>(d.writeCounterForTest());
		const auto top = static_cast<double>(len);
		// THE WRAP, WHEREVER THE WRITE HEAD IS: `w + 1` behind reads the line's last frame and then its first, and `w`
		// behind reads its first; `w + 0.625` reads BETWEEN the two, which is the only read the wrap of the second index
		// can change. `w` is the ring's write position, `writeCounter % length`.
		const double w = std::fmod(now, top);
		for (const double back : { 1.0, 1.25, 1.999, 2.0, 7.5, 1000.3, top * 0.5 + 0.77, top - 1.5, top - 1.0 - 1.0e-6,
		                           top - 1.0, std::max(1.0, w), w + 1.0, w + 0.625 }) {
			for (const double nudge : { 0.0, 0.125, 0.5, 0.875 }) {
				float tap[2] = {};
				float lerp[2] = {};
				d.readBothForTest(now - back - nudge, tap, lerp); // at most `top - 0.125` behind
				compared += 2;
				differing += sameBits(tap[0], lerp[0]) ? 0 : 1;
				differing += sameBits(tap[1], lerp[1]) ? 0 : 1;
			}
		}
	};
	feed(3001); // NOT YET FILLED: most of these positions are before frame 0
	sweep();
	feed(len - 3001 - 17); // the write head 17 frames short of the wrap
	sweep();
	feed(40); // just past it
	sweep();
	feed(len / 3 + 5); // well into the second lap
	sweep();
	CHECK(compared == 4 * 13 * 4 * 2);
	CHECK(differing == 0);
}

//--------------------------------------------------------------------------------------------------//
// BACKLOG MH — PATTERN AND LEVELS AS ZONE CONTROLS. Every claim is a list of numbers: where each tap  //
// falls and what it weighs, restated here from the family's definition rather than read back.        //
//--------------------------------------------------------------------------------------------------//

namespace {

/// THE DEVICE'S ASKED-FOR GEOMETRY after one block: fractions and left gains before the power normalisation.
struct Geometry {
	std::vector<double> frac;
	std::vector<float> gain;
};
Geometry geometryOf(float pattern, float levels, int taps) {
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = multiTapBase(taps);
	s.tapPattern = pattern;
	s.tapLevels = levels;
	d.setTarget(s);
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	d.processStereo(l.data(), r.data(), 256);
	Geometry g;
	for (int k = 0; k < taps; ++k) {
		g.frac.push_back(d.tapFractionForTest(static_cast<size_t>(k)));
		g.gain.push_back(d.tapGainForTest(0, static_cast<size_t>(k)));
	}
	return g;
}

/// GAINS AS RATIOS TO THE LOUDEST, so the power normalisation (a common factor) drops out of the comparison.
std::vector<double> ratios(const std::vector<float>& g) {
	float top = 0.0f;
	for (const float x : g) {
		top = std::max(top, x);
	}
	std::vector<double> out;
	for (const float x : g) {
		out.push_back(top > 0.0f ? static_cast<double>(x) / static_cast<double>(top) : 0.0);
	}
	return out;
}

} // namespace

TEST_CASE("MH: both defaults are the centre of zone 0, which is the old Shape 0 and Slope −0.35 to the bit") {
	CHECK(kParamDescs[static_cast<size_t>(Param::TapPattern)].def == static_cast<double>(kTapCoordDefault));
	CHECK(kParamDescs[static_cast<size_t>(Param::TapLevels)].def == static_cast<double>(kTapCoordDefault));
	CHECK(Settings {}.tapPattern == kTapCoordDefault);
	CHECK(Settings {}.tapLevels == kTapCoordDefault);
	const TapZone z = decodeTapCoord(kTapCoordDefault);
	CHECK(z.zone == 0);
	CHECK(z.position == 0.5f);
	CHECK(curveShapeOf(z.position) == 0.0f);
	CHECK(fadeSlopeOf(z.position) == -0.35f);
	// AND THE DEVICE AT ITS DEFAULTS PLACES AND WEIGHS THE TAPS BY THE OLD EXPRESSIONS, EXACTLY.
	Delay d;
	d.prepare(kRate);
	d.setTransportClock(at120());
	Settings s = defaults();
	s.mode = Mode::MultiTap;
	s.tapWidth = 0.0f;
	d.setTarget(s);
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	d.processStereo(l.data(), r.data(), 256);
	const int n = s.taps;
	float energy = 0.0f;
	std::vector<float> old;
	for (int k = 0; k < n; ++k) {
		const double along = static_cast<double>(k + 1) / static_cast<double>(n);
		CHECK(d.tapFractionForTest(static_cast<size_t>(k)) == std::pow(along, std::exp2(0.0 * kShapeOctaves)));
		const float t = static_cast<float>(k) / static_cast<float>(n - 1);
		const float slope = -0.35f;
		old.push_back(1.0f + slope * t);
		energy += old.back() * old.back();
	}
	const float norm = 1.0f / std::sqrt(energy);
	for (int k = 0; k < n; ++k) {
		CAPTURE(k);
		CHECK(d.tapGainForTest(0, static_cast<size_t>(k)) == old[static_cast<size_t>(k)] * norm);
	}
}

TEST_CASE("MH: Pattern's families place the taps by their own definitions, the last always at the full time") {
	constexpr int n = 4;
	const double phi = (1.0 + std::sqrt(5.0)) / 2.0;
	SUBCASE("Curve at its right edge is the old Shape +1: (k/N)^2") {
		const Geometry g = geometryOf(tapCoordIn(static_cast<int>(TapPattern::Curve), 0.999f), levelsCoordForSlope(-0.35f), n);
		for (int k = 0; k < n; ++k) {
			CHECK(g.frac[static_cast<size_t>(k)] == doctest::Approx(std::pow((k + 1) / 4.0, 2.0)));
		}
	}
	SUBCASE("Golden at its right edge: each gap phi times the one before") {
		const Geometry g = geometryOf(tapCoordIn(static_cast<int>(TapPattern::Golden), 0.999f), kTapCoordDefault, n);
		const double total = 1.0 + phi + phi * phi + phi * phi * phi;
		CHECK(g.frac[0] == doctest::Approx(1.0 / total));
		CHECK(g.frac[1] == doctest::Approx((1.0 + phi) / total));
		CHECK(g.frac[2] == doctest::Approx((1.0 + phi + phi * phi) / total));
		CHECK(g.frac[3] == 1.0);
		// and at its centre it is even
		const Geometry even = geometryOf(tapCoordIn(static_cast<int>(TapPattern::Golden), 0.5f), kTapCoordDefault, n);
		for (int k = 0; k < n; ++k) {
			CHECK(even.frac[static_cast<size_t>(k)] == doctest::Approx((k + 1) / 4.0));
		}
	}
	SUBCASE("Swing: every other tap late by half the position of a step") {
		const Geometry g = geometryOf(tapCoordIn(static_cast<int>(TapPattern::Swing), 0.6f), kTapCoordDefault, n);
		CHECK(g.frac[0] == doctest::Approx((1.0 + 0.3) / 4.0));
		CHECK(g.frac[1] == doctest::Approx(2.0 / 4.0));
		CHECK(g.frac[2] == doctest::Approx((3.0 + 0.3) / 4.0));
		CHECK(g.frac[3] == 1.0);
	}
	SUBCASE("Scatter: each tap nudged by its own fixed amount, the same every time, the order kept") {
		const Geometry g = geometryOf(tapCoordIn(static_cast<int>(TapPattern::Scatter), 0.8f), kTapCoordDefault, n);
		const Geometry again = geometryOf(tapCoordIn(static_cast<int>(TapPattern::Scatter), 0.8f), kTapCoordDefault, n);
		for (int k = 0; k < n - 1; ++k) {
			CAPTURE(k);
			CHECK(g.frac[static_cast<size_t>(k)]
			      == doctest::Approx((k + 1 + 0.45 * 0.8 * static_cast<double>(tapScatterOf(k))) / 4.0));
			CHECK(g.frac[static_cast<size_t>(k)] == again.frac[static_cast<size_t>(k)]);
			CHECK(g.frac[static_cast<size_t>(k)] < g.frac[static_cast<size_t>(k + 1)]);
		}
		CHECK(g.frac[3] == 1.0);
		// NOT VACUOUS: the nudges are not all zero.
		CHECK(std::abs(tapScatterOf(0)) + std::abs(tapScatterOf(1)) + std::abs(tapScatterOf(2)) > 0.1f);
	}
	SUBCASE("a lap later is the same place: the coordinate keeps turning") {
		const float c = tapCoordIn(static_cast<int>(TapPattern::Swing), 0.6f);
		const Geometry a = geometryOf(c, kTapCoordDefault, n);
		const Geometry b = geometryOf(c + 3.0f, kTapCoordDefault, n);
		for (int k = 0; k < n; ++k) {
			CHECK(a.frac[static_cast<size_t>(k)] == doctest::Approx(b.frac[static_cast<size_t>(k)]));
		}
	}
}

TEST_CASE("MH: Levels' families weigh the taps by their own definitions") {
	constexpr int n = 5;
	SUBCASE("Fade's left edge is the old Slope −1: falling to nothing") {
		const auto r = ratios(geometryOf(kTapCoordDefault, tapCoordIn(static_cast<int>(TapLevels::Fade), 0.0f), n).gain);
		for (int k = 0; k < n; ++k) {
			CHECK(r[static_cast<size_t>(k)] == doctest::Approx(1.0 - k / 4.0).epsilon(1e-5));
		}
	}
	SUBCASE("Arch at its right edge: the middle tap loudest, the ends silent") {
		const auto r = ratios(geometryOf(kTapCoordDefault, tapCoordIn(static_cast<int>(TapLevels::Arch), 0.999f), n).gain);
		const double want[] = { 0.0, 0.5, 1.0, 0.5, 0.0 };
		for (int k = 0; k < n; ++k) {
			CHECK(r[static_cast<size_t>(k)] == doctest::Approx(want[k]).epsilon(1e-5));
		}
	}
	SUBCASE("Alternate: the odd taps lowered by the position") {
		const auto r = ratios(geometryOf(kTapCoordDefault, tapCoordIn(static_cast<int>(TapLevels::Alternate), 0.75f), n).gain);
		const double want[] = { 1.0, 0.25, 1.0, 0.25, 1.0 };
		for (int k = 0; k < n; ++k) {
			CHECK(r[static_cast<size_t>(k)] == doctest::Approx(want[k]).epsilon(1e-5));
		}
	}
	SUBCASE("Scatter: each tap lowered by its own fixed amount") {
		const auto g = geometryOf(kTapCoordDefault, tapCoordIn(static_cast<int>(TapLevels::Scatter), 0.6f), n).gain;
		const auto r = ratios(g);
		std::vector<double> want;
		double top = 0.0;
		for (int k = 0; k < n; ++k) {
			want.push_back(1.0 - 0.6 * 0.5 * (1.0 + static_cast<double>(tapScatterOf(k + 101))));
			top = std::max(top, want.back());
		}
		for (int k = 0; k < n; ++k) {
			CHECK(r[static_cast<size_t>(k)] == doctest::Approx(want[static_cast<size_t>(k)] / top).epsilon(1e-5));
		}
	}
}

TEST_CASE("MH: in every family pair the tail reaches EXACT zero after the input stops") {
	for (int p = 0; p < static_cast<int>(kTapZones); ++p) {
		for (int v = 0; v < static_cast<int>(kTapZones); ++v) {
			CAPTURE(p);
			CAPTURE(v);
			Delay d;
			d.prepare(kRate);
			d.setTransportClock(at120());
			Settings s = multiTapBase(6);
			s.feedback = 0.5f;
			s.tapPattern = tapCoordIn(p, 0.7f);
			s.tapLevels = tapCoordIn(v, 0.7f);
			d.setTarget(s);
			std::vector<float> l(512);
			std::vector<float> r(512);
			for (size_t i = 0; i < l.size(); ++i) {
				l[i] = 0.5f * std::sin(0.05f * static_cast<float>(i));
				r[i] = l[i];
			}
			d.processStereo(l.data(), r.data(), 512);
			bool silent = false;
			for (int block = 0; block < 2000 && !silent; ++block) {
				std::fill(l.begin(), l.end(), 0.0f);
				std::fill(r.begin(), r.end(), 0.0f);
				d.processStereo(l.data(), r.data(), 512);
				silent = d.silentForTest()
				         && std::all_of(l.begin(), l.end(), [](float x) { return x == 0.0f; })
				         && std::all_of(r.begin(), r.end(), [](float x) { return x == 0.0f; });
			}
			CHECK(silent);
		}
	}
}
