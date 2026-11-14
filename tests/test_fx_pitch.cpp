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

#include "fx/pitch/pitch.h"
#include "fx/pitch/pitch_types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <set>
#include <string_view>
#include <vector>

// **VALUE ASSERTIONS, NOT TWO RENDERS COMPARED** — CLAUDE.md's rule, and a pitch shifter is its
// natural prey: "the output changed when I turned Shift" is true of a shifter playing the wrong
// interval. So the interval is asked as a NUMBER three ways, each nearer the ear than the last: the
// ratio `ratioFor` computes, the rate a head's delay actually moves at, and the frequency measured
// off the output.

namespace {

using namespace feathers;
using namespace feathers::fx::pitch;

constexpr double kRate = 48000.0;
/// 60 ms at 48 kHz — the default grain, in frames.
constexpr double kDefaultGrain = 2880.0;

Settings defaults() { return stateOf(rowOf(State {}), static_cast<size_t>(Param::kCount)).settings; }

double semis(double n) { return std::exp2(n / 12.0); }

std::vector<float> sine(double hz, size_t frames, float amp = 0.5f) {
	std::vector<float> v(frames);
	for (size_t i = 0; i < frames; ++i) {
		v[i] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
	}
	return v;
}

/// DETERMINISTIC NOISE in [-0.5, 0.5) — an LCG, so the suite owns its input.
std::vector<float> noise(size_t frames, uint32_t seed = 12345u) {
	std::vector<float> v(frames);
	uint32_t x = seed;
	for (float& s : v) {
		x = x * 1664525u + 1013904223u;
		s = static_cast<float>(x >> 8) / 16777216.0f - 0.5f;
	}
	return v;
}

/// THE FREQUENCY OF A SIGNAL, from its rising zero crossings with the crossing instant interpolated.
/// Only meaningful over a stretch with no splice in it — the frequency case says why.
double measuredHz(const std::vector<float>& v, size_t from, size_t to) {
	double first = -1.0;
	double last = -1.0;
	int count = 0;
	for (size_t i = from + 1; i < to; ++i) {
		if (v[i - 1] < 0.0f && v[i] >= 0.0f) {
			const double t = static_cast<double>(i - 1)
			                 + static_cast<double>(-v[i - 1]) / static_cast<double>(v[i] - v[i - 1]);
			if (first < 0.0) {
				first = t;
			}
			last = t;
			++count;
		}
	}
	if (count < 2) {
		return 0.0;
	}
	return static_cast<double>(count - 1) / (last - first) * kRate;
}

/// RUN A MONO-IN-BOTH-CHANNELS SIGNAL THROUGH, in one call.
void run(PitchShifter& p, std::vector<float>& l, std::vector<float>& r) {
	p.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
}

} // namespace

TEST_CASE("the scale menu is Free plus SKLITTER's twelve, built from SKLITTER's own table") {
	REQUIRE(kScaleChoiceCount == 13);
	CHECK(std::string_view(kScaleNames[0]) == "Free");
	for (size_t i = 0; i < kSklitterScaleCount; ++i) {
		CHECK(std::string_view(kScaleNames[1 + i]) == std::string_view(fx::sklitter::kPitchScaleNames[i]));
	}
	// AND THE FIXED SHIFTS ARE NOT IN IT — SKLITTER's thirteenth entry is where they begin.
	CHECK(std::string_view(fx::sklitter::kPitchScaleNames[kSklitterScaleCount]) == "+1");

	const device::ParamDesc& scale = kParamDescs[static_cast<size_t>(Param::Scale)];
	CHECK(scale.max == doctest::Approx(12.0));
	CHECK(scale.names.size() == kScaleChoiceCount);
	CHECK(scale.def == 0.0); // Free is the default: a pitch shifter that snaps before it is asked to
	                         // is a knob that skips.
}

TEST_CASE("the snap: hand-checked intervals, ties to the smaller shift") {
	constexpr size_t kFree = 0;
	constexpr size_t kChromatic = 1;
	constexpr size_t kMajor = 2;
	constexpr size_t kMajTri = 9;

	CHECK(snapSemitones(3.7, kFree) == 3.7);
	CHECK(snapSemitones(-11.2, kFree) == -11.2);

	CHECK(snapSemitones(3.4, kChromatic) == 3.0);
	CHECK(snapSemitones(3.6, kChromatic) == 4.0);
	CHECK(snapSemitones(3.5, kChromatic) == 3.0);   // the tie
	CHECK(snapSemitones(-3.5, kChromatic) == -3.0); // and its mirror

	CHECK(snapSemitones(7.0, kMajor) == 7.0);
	CHECK(snapSemitones(1.2, kMajor) == 2.0);
	CHECK(snapSemitones(1.0, kMajor) == 0.0); // 0 and 2 are equally near; 0 is the smaller shift
	CHECK(snapSemitones(3.0, kMajor) == 2.0);
	CHECK(snapSemitones(6.0, kMajor) == 5.0);
	CHECK(snapSemitones(8.4, kMajor) == 9.0);
	CHECK(snapSemitones(13.0, kMajor) == 12.0);
	// DOWNWARD IS BY PITCH CLASS: -5 is a fourth down, which lands on the fifth, which belongs.
	CHECK(snapSemitones(-5.0, kMajor) == -5.0);
	// -1 IS THE LEADING TONE, pitch class 11, and it belongs. (This line first read `== 0.0`: the
	// hand-check was wrong and the code was right, which the brute-force case below had already said.)
	CHECK(snapSemitones(-1.0, kMajor) == -1.0);
	CHECK(snapSemitones(-2.0, kMajor) == -1.0); // class 10 does not; -1 and -3 tie
	CHECK(snapSemitones(-6.0, kMajor) == -5.0);

	CHECK(snapSemitones(5.0, kMajTri) == 4.0);
	CHECK(snapSemitones(10.0, kMajTri) == 12.0);
	CHECK(snapSemitones(-24.0, kMajTri) == -24.0);
}

TEST_CASE("the snap: for every scale and every knob position, the result belongs and nothing nearer does") {
	// THE INDEPENDENT RESTATEMENT: enumerate every in-scale interval across the range from the mask
	// and find the nearest by brute force. Shares no arithmetic with `snapSemitones` but the masks.
	for (size_t scale = 1; scale < kScaleChoiceCount; ++scale) {
		CAPTURE(scale);
		std::vector<int> degrees;
		for (int n = -36; n <= 36; ++n) {
			const int pc = ((n % 12) + 12) % 12;
			if ((kScaleMasks[scale] >> pc) & 1u) {
				degrees.push_back(n);
			}
		}
		// EVERY SCALE HAS ITS ROOT, or a Shift of zero would not be no shift.
		REQUIRE((kScaleMasks[scale] & 1u) == 1u);

		for (int q = -96; q <= 96; ++q) {
			const double shift = static_cast<double>(q) * 0.25;
			CAPTURE(shift);
			const double got = snapSemitones(shift, scale);
			REQUIRE(std::find(degrees.begin(), degrees.end(), static_cast<int>(got)) != degrees.end());
			CHECK(got == std::round(got));
			CHECK(std::abs(got) <= 24.0);
			for (const int d : degrees) {
				const double mine = std::abs(got - shift);
				const double theirs = std::abs(static_cast<double>(d) - shift);
				CHECK(mine <= theirs + 1e-9);
				if (std::abs(mine - theirs) <= 1e-9) {
					CHECK(std::abs(got) <= std::abs(static_cast<double>(d)));
				}
			}
			// IDEMPOTENT — a snapped value is a degree, so snapping it again moves nothing.
			CHECK(snapSemitones(got, scale) == got);
		}
	}
}

TEST_CASE("the ratio: Scale snaps Shift alone; Fine and Width are added after it") {
	Settings s = defaults();
	CHECK(ratioFor(s, false) == 1.0);
	CHECK(ratioFor(s, true) == 1.0);

	s.shiftSemitones = 7.0f;
	CHECK(ratioFor(s, false) == doctest::Approx(semis(7.0)));

	s.scale = 2; // Major
	s.shiftSemitones = 8.4f;
	CHECK(ratioFor(s, false) == doctest::Approx(semis(9.0)));

	// FINE SURVIVES THE LOCK. Snapping the SUM would swallow it — 9.5 would go back to 9.
	s.fineCents = 50.0f;
	CHECK(ratioFor(s, false) == doctest::Approx(semis(9.5)));

	// **AND THE POINT THAT ACTUALLY TELLS THE TWO ORDERS APART.** The line above passed against a
	// known-bad that snapped Shift PLUS Fine, because 8.4 + 0.5 snaps to the same 9 and Fine was
	// still added after. Here Fine carries the sum across a boundary: Shift 9.9 snaps to 9 and Fine
	// makes it 9.6; snapping the sum, 10.5 goes to 11 and comes out 11.6 — a whole tone sharp.
	s.shiftSemitones = 9.9f;
	s.fineCents = 60.0f;
	CHECK(ratioFor(s, false) == doctest::Approx(semis(9.6)));
	s.shiftSemitones = 8.4f;

	// WIDTH: HALF EACH WAY, so the pair straddles the pitch the controls name.
	s.fineCents = 0.0f;
	s.width = 1.0f;
	CHECK(ratioFor(s, false) == doctest::Approx(semis(9.0 - 0.25)));
	CHECK(ratioFor(s, true) == doctest::Approx(semis(9.0 + 0.25)));
	CHECK(ratioFor(s, true) / ratioFor(s, false) == doctest::Approx(semis(kWidthCentsMax / 100.0)));
}

TEST_CASE("a head's delay moves at one minus the ratio, a reversed head's at one plus it") {
	// THE MECHANISM, READ DIRECTLY. At an octave up a head consumes two frames for each one written,
	// so its delay falls a frame a sample from a seat one grain back; an octave down it rises half
	// a frame a sample from zero.
	std::vector<float> l = noise(100);
	std::vector<float> r = l;

	PitchShifter up;
	up.prepare(kRate);
	Settings s = defaults();
	s.shiftSemitones = 12.0f;
	up.setTarget(s);
	run(up, l, r);
	CHECK(up.ratioLeftForTest() == doctest::Approx(2.0));
	CHECK(up.headDelayForTest(0, 0) == doctest::Approx(kDefaultGrain - 100.0));
	CHECK(up.headDelayForTest(1, 1) == doctest::Approx(kDefaultGrain - 100.0));

	PitchShifter down;
	down.prepare(kRate);
	s.shiftSemitones = -12.0f;
	down.setTarget(s);
	l = noise(100);
	r = l;
	run(down, l, r);
	CHECK(down.headDelayForTest(0, 0) == doctest::Approx(50.0));

	PitchShifter back;
	back.prepare(kRate);
	s.shiftSemitones = 12.0f;
	s.mode = Mode::Reverse;
	back.setTarget(s);
	l = noise(100);
	r = l;
	run(back, l, r);
	CHECK(back.headDelayForTest(0, 0) == doctest::Approx(300.0));
}

TEST_CASE("a shift of zero is the input, to the bit") {
	// NOT "NEARLY". Write-then-read puts a zero delay on THIS sample, and the head mix is written
	// `b + (a - b) * share` so that two heads on one sample give that sample whatever the share.
	for (const Mode mode : { Mode::Smooth, Mode::Tight }) {
		CAPTURE(static_cast<int>(mode));
		PitchShifter p;
		p.prepare(kRate);
		Settings s = defaults();
		s.mode = mode;
		p.setTarget(s);
		const std::vector<float> in = noise(20000);
		std::vector<float> l = in;
		std::vector<float> r = noise(20000, 777u);
		const std::vector<float> inR = r;
		run(p, l, r);
		CHECK(l == in);
		CHECK(r == inR);
	}
}

TEST_CASE("Enabled off leaves the buffer alone") {
	PitchShifter p;
	p.prepare(kRate);
	Settings s = defaults();
	s.enabled = false;
	s.shiftSemitones = 7.0f;
	p.setTarget(s);
	const std::vector<float> in = noise(4096);
	std::vector<float> l = in;
	std::vector<float> r = in;
	run(p, l, r);
	CHECK(l == in);
}

TEST_CASE("a bypass clears the shifter: re-enabled into silence it is exactly silent, not the audio from before") {
	// `!enabled` returned with the line, the heads' delays and the feedback loop as they were, so a re-enable replayed
	// what was playing when the device was switched off.
	PitchShifter p;
	p.prepare(kRate);
	Settings s = defaults();
	s.shiftSemitones = 7.0f;
	s.feedback = 0.5f;
	p.setTarget(s);
	std::vector<float> l = noise(30000);
	std::vector<float> r = noise(30000, 777u);
	run(p, l, r);
	// NON-VACUITY: it was sounding.
	float peak = 0.0f;
	for (size_t i = l.size() - 2000; i < l.size(); ++i) {
		peak = std::max(peak, std::fabs(l[i]));
	}
	REQUIRE(peak > 0.01f);

	Settings off = s;
	off.enabled = false;
	p.setTarget(off);
	std::vector<float> bypassed = noise(9600, 5u);
	std::vector<float> bypassedR = bypassed;
	run(p, bypassed, bypassedR);

	p.setTarget(s);
	std::vector<float> silent(30000, 0.0f);
	std::vector<float> silentR(30000, 0.0f);
	run(p, silent, silentR);
	size_t nonZero = 0;
	for (size_t i = 0; i < silent.size(); ++i) {
		nonZero += (silent[i] != 0.0f || silentR[i] != 0.0f) ? 1 : 0;
	}
	CHECK(nonZero == 0u);
}

TEST_CASE("the two heads sum to exactly one: DC in is DC out, in every mode") {
	for (const Mode mode : { Mode::Smooth, Mode::Tight, Mode::Reverse }) {
		CAPTURE(static_cast<int>(mode));
		PitchShifter p;
		p.prepare(kRate);
		Settings s = defaults();
		s.shiftSemitones = 7.0f;
		s.mode = mode;
		s.jitter = 0.5f;
		p.setTarget(s);
		const auto total = static_cast<size_t>(kDefaultGrain * 10.0);
		std::vector<float> l(total, 0.5f);
		std::vector<float> r(total, 0.5f);
		run(p, l, r);
		// PAST THE WARM-UP — until then a head is still reading from before the recording began.
		const auto from = static_cast<size_t>(kDefaultGrain * 5.0);
		bool allExact = true;
		for (size_t i = from; i < total; ++i) {
			allExact = allExact && l[i] == 0.5f && r[i] == 0.5f;
		}
		CHECK(allExact);
		// AND THE GRAINS DID TURN OVER, or the above is a statement about one head.
		CHECK(p.reseatsForTest() >= 18);
	}
}

TEST_CASE("the window: zero at each head's own wrap, and Tight's solo stretch is where Smooth has none") {
	for (const double fade : { kSmoothFade, kTightFade }) {
		CAPTURE(fade);
		// HEAD 0 RE-SEATS AT PHASE 0 AND MUST BE SILENT THERE; head 1 at one half, likewise.
		CHECK(headZeroShare(0.0, fade) == 0.0);
		CHECK(headZeroShare(0.5, fade) == 1.0);
		CHECK(headZeroShare(fade * 0.5, fade) == doctest::Approx(0.5));
		CHECK(headZeroShare(0.5 + fade * 0.5, fade) == doctest::Approx(0.5));
		// AND IT ARRIVES THERE CONTINUOUSLY — the sample before a wrap is nearly silent, not merely
		// the sample after.
		CHECK(headZeroShare(0.999999, fade) < 1e-6);
		CHECK(headZeroShare(0.499999, fade) > 1.0 - 1e-6);
	}
	CHECK(headZeroShare(0.25, kTightFade) == 1.0);
	CHECK(headZeroShare(0.75, kTightFade) == 0.0);
	CHECK(headZeroShare(0.25, kSmoothFade) == doctest::Approx(0.5));

	CHECK(fadeFor(Mode::Smooth) == kSmoothFade);
	CHECK(fadeFor(Mode::Tight) == kTightFade);
	CHECK(fadeFor(Mode::Reverse) == kSmoothFade);
	CHECK(kTightFade < kSmoothFade);
	CHECK(kSmoothFade <= 0.5); // past one half a head would still be sounding at its own wrap
}

TEST_CASE("Mode reaches the sound: a quarter through the cycle Tight is head 0 alone, Smooth is both") {
	// A RAMP IN, so a head's contribution is a NUMBER — the ramp's value at the frame it reads —
	// and the output says which heads it is made of.
	constexpr double kSlope = 1.0e-5;
	const auto total = static_cast<size_t>(kDefaultGrain * 4.25);
	for (const Mode mode : { Mode::Tight, Mode::Smooth }) {
		CAPTURE(static_cast<int>(mode));
		PitchShifter p;
		p.prepare(kRate);
		Settings s = defaults();
		s.mode = mode;
		s.shiftSemitones = -12.0f; // slope +0.5 a sample; the heads sit at different delays
		p.setTarget(s);
		std::vector<float> l(total);
		for (size_t i = 0; i < total; ++i) {
			l[i] = static_cast<float>(kSlope * static_cast<double>(i));
		}
		std::vector<float> r = l;
		run(p, l, r);
		// THE SEAM READS ONE SAMPLE LATE — the delays have been advanced past the last output.
		const double d0 = p.headDelayForTest(0, 0) - 0.5;
		const double d1 = p.headDelayForTest(0, 1) - 0.5;
		REQUIRE(std::abs(d0 - d1) > 100.0);
		const double last = static_cast<double>(total - 1);
		const double head0 = kSlope * (last - d0);
		const double head1 = kSlope * (last - d1);
		const double share = headZeroShare(0.25, fadeFor(mode));
		const double want = head1 + (head0 - head1) * share;
		CAPTURE(want);
		CHECK(static_cast<double>(l[total - 1]) == doctest::Approx(want).epsilon(1e-4));
		if (mode == Mode::Tight) {
			CHECK(static_cast<double>(l[total - 1]) == doctest::Approx(head0).epsilon(1e-4));
		}
		else {
			CHECK(std::abs(static_cast<double>(l[total - 1]) - head0) > 0.1 * std::abs(head0 - head1));
		}
	}
}

TEST_CASE("the output is at the shifted frequency, measured") {
	struct Row {
		size_t scale;
		float shift;
		double expectSemis;
	};
	const Row rows[] = {
		{ 0, 7.0f, 7.0 },    // Free, a fifth up
		{ 0, -12.0f, -12.0 }, // an octave down
		{ 0, 12.0f, 12.0 },
		{ 2, 8.4f, 9.0 },    // Major: the knob at 8.4 PLAYS a sixth — the lock, end to end
		{ 5, 4.0f, 3.0 },    // MinPent: 4 is not in it; 3 and 5 tie, and 3 is the smaller
	};
	// **MEASURED INSIDE ONE GRAIN, WHERE ONE HEAD HAS THE OUTPUT TO ITSELF.** The first draft counted
	// crossings across two seconds and read an octave down as 206.7 Hz against 220 — not a defect:
	// every splice slips the phase by the same fraction of a cycle, 16.7 times a second, and a
	// crossing counter books that as frequency. (It is what a fixed grain SOUNDS like, too, and is
	// Jitter's reason for existing.) In `Tight` mode head 0 is alone from 12% to 50% of its cycle,
	// and there the output is one sine at the shifted pitch and nothing else.
	const double f0 = 1000.0;
	for (const Row& row : rows) {
		CAPTURE(row.scale);
		CAPTURE(row.shift);
		PitchShifter p;
		p.prepare(kRate);
		Settings s = defaults();
		s.mode = Mode::Tight;
		s.scale = row.scale;
		s.shiftSemitones = row.shift;
		p.setTarget(s);
		std::vector<float> l = sine(f0, static_cast<size_t>(kDefaultGrain * 8.0));
		std::vector<float> r = l;
		run(p, l, r);
		const auto from = static_cast<size_t>(kDefaultGrain * (6.0 + kTightFade)) + 4;
		const auto to = static_cast<size_t>(kDefaultGrain * 6.5) - 4;
		const double got = measuredHz(l, from, to);
		const double want = f0 * semis(row.expectSemis);
		CAPTURE(got);
		CAPTURE(want);
		CHECK(std::abs(got / want - 1.0) < 0.004);
	}
}

TEST_CASE("Reverse plays the grain backwards, at the shifted speed") {
	// A RAMP IN. Where head 0 has the output to itself — phase one half, where its share is one and
	// the share's own slope is zero — the output's slope is the grain's: minus the ratio times the
	// ramp's. Forward it would be plus.
	constexpr float kSlope = 1.0e-5f;
	for (const float shift : { 0.0f, 12.0f }) {
		CAPTURE(shift);
		PitchShifter p;
		p.prepare(kRate);
		Settings s = defaults();
		s.mode = Mode::Reverse;
		s.shiftSemitones = shift;
		p.setTarget(s);
		const auto total = static_cast<size_t>(kDefaultGrain * 6.0);
		std::vector<float> l(total);
		for (size_t i = 0; i < total; ++i) {
			l[i] = kSlope * static_cast<float>(i);
		}
		std::vector<float> r = l;
		run(p, l, r);
		// THE FIFTH CYCLE'S MIDPOINT — late enough that the grain lies wholly inside the recording
		// even at an octave up.
		const auto mid = static_cast<size_t>(kDefaultGrain * 4.5);
		const double slope = (static_cast<double>(l[mid + 8]) - static_cast<double>(l[mid - 8])) / 16.0;
		const double want = -static_cast<double>(kSlope) * semis(static_cast<double>(shift));
		CAPTURE(slope);
		CAPTURE(want);
		CHECK(std::abs(slope / want - 1.0) < 0.05);
	}
}

TEST_CASE("moving Shift changes a head's slope and never its position") {
	// THE INTEGRATED DELAY'S WHOLE POINT. The textbook form computes the delay from the phase and
	// the CURRENT ratio, so a moved Shift teleports the head. Here the head is where it was.
	PitchShifter p;
	p.prepare(kRate);
	Settings s = defaults();
	s.shiftSemitones = 12.0f;
	p.setTarget(s);
	std::vector<float> l = sine(440.0, 1000);
	std::vector<float> r = l;
	run(p, l, r);
	const double before = p.headDelayForTest(0, 0);
	REQUIRE(before == doctest::Approx(kDefaultGrain - 1000.0));

	s.shiftSemitones = 0.0f;
	p.setTarget(s);
	std::vector<float> one = { 0.0f };
	std::vector<float> oneR = { 0.0f };
	run(p, one, oneR);
	CHECK(p.headDelayForTest(0, 0) == before); // slope is now zero, and the head has not moved

	s.shiftSemitones = -12.0f;
	p.setTarget(s);
	run(p, one, oneR);
	CHECK(p.headDelayForTest(0, 0) == doctest::Approx(before + 0.5));
}

TEST_CASE("a Shift move does not step the output") {
	// END TO END, as the second assertion. A sine through a move from 0 to +7 and on to -5: no
	// sample-to-sample step larger than the steepest sine in play could make by itself.
	PitchShifter p;
	p.prepare(kRate);
	Settings s = defaults();
	p.setTarget(s);
	const std::vector<float> in = sine(440.0, 48000, 0.5f);
	std::vector<float> out;
	out.reserve(in.size());
	const float shifts[] = { 0.0f, 7.0f, -5.0f, 12.0f };
	size_t at = 0;
	for (const float shift : shifts) {
		s.shiftSemitones = shift;
		p.setTarget(s);
		std::vector<float> l(in.begin() + static_cast<long>(at), in.begin() + static_cast<long>(at + 12000));
		std::vector<float> r = l;
		run(p, l, r);
		out.insert(out.end(), l.begin(), l.end());
		at += 12000;
	}
	float worst = 0.0f;
	for (size_t i = 1; i < out.size(); ++i) {
		worst = std::max(worst, std::fabs(out[i] - out[i - 1]));
	}
	// 0.5 * 2*pi*f/rate at 880 Hz — the octave up, the steepest thing here — is 0.0576.
	const double steepest = 0.5 * 2.0 * std::numbers::pi * 880.0 / kRate;
	CAPTURE(worst);
	CHECK(static_cast<double>(worst) < steepest * 1.5);
}

TEST_CASE("Jitter throws the seat within one grain, the same throw for both channels") {
	PitchShifter p;
	p.prepare(kRate);
	Settings s = defaults();
	s.jitter = 1.0f;
	p.setTarget(s);
	std::set<long> seats;
	std::vector<float> one = { 0.1f };
	std::vector<float> oneR = { 0.1f };
	uint64_t seen = p.reseatsForTest();
	for (int i = 0; i < static_cast<int>(kDefaultGrain * 8.0); ++i) {
		run(p, one, oneR);
		if (p.reseatsForTest() != seen) {
			seen = p.reseatsForTest();
			// AT A RATIO OF ONE THE SLOPE IS ZERO, so whichever head just re-seated is sitting
			// exactly on its seat: head 0 if the phase just wrapped, head 1 otherwise. Read both and
			// require both inside the grain.
			for (int h = 0; h < 2; ++h) {
				const double d = p.headDelayForTest(0, h);
				CHECK(d >= 0.0);
				CHECK(d < kDefaultGrain);
				CHECK(d == p.headDelayForTest(1, h));
				seats.insert(std::lround(d));
			}
		}
	}
	// THEY DIFFER FROM ONE ANOTHER — a constant "scatter" would satisfy everything above.
	CHECK(seats.size() >= 8);
}

TEST_CASE("the mono road is the LEFT channel, asserted with Width up so the channels differ") {
	Settings s = defaults();
	s.shiftSemitones = 7.0f;
	s.width = 1.0f;

	PitchShifter stereo;
	stereo.prepare(kRate);
	stereo.setTarget(s);
	const std::vector<float> in = sine(440.0, 24000);
	std::vector<float> l = in;
	std::vector<float> r = in;
	run(stereo, l, r);

	PitchShifter mono;
	mono.prepare(kRate);
	mono.setTarget(s);
	std::vector<float> m = in;
	mono.processMono(m.data(), static_cast<int>(m.size()));

	CHECK(m == l);
	CHECK(l != r); // or "the left" is not a claim
}

TEST_CASE("Feedback: the loop is one grain long, and each lap is scaled by the knob") {
	// AT A SHIFT OF ZERO the staircase is flat and the loop is a plain echo, which is the form the
	// numbers can be read off: an impulse returns after one grain, then a grain and a sample after
	// that (the fed-back sample is the PREVIOUS one's wet), at the knob's gain each time.
	PitchShifter p;
	p.prepare(kRate);
	Settings s = defaults();
	s.feedback = 0.5f;
	p.setTarget(s);
	const auto g = static_cast<size_t>(kDefaultGrain);
	std::vector<float> l(g * 4, 0.0f);
	l[0] = 1.0f;
	std::vector<float> r = l;
	run(p, l, r);

	CHECK(p.headDelayForTest(0, 0) == doctest::Approx(kDefaultGrain));
	CHECK(l[g] == 1.0f);
	CHECK(l[2 * g + 1] == 0.5f);
	CHECK(l[3 * g + 2] == 0.25f);
	// AND NOTHING ELSE — the three are the only non-zero samples.
	CHECK(std::count_if(l.begin(), l.end(), [](float v) { return v != 0.0f; }) == 3);
}

TEST_CASE("Feedback off: no loop delay, and a hot input is not clipped by a control that is off") {
	PitchShifter p;
	p.prepare(kRate);
	p.setTarget(defaults());
	std::vector<float> l(256, 1.75f);
	std::vector<float> r(256, -1.75f);
	run(p, l, r);
	CHECK(p.headDelayForTest(0, 0) == 0.0);
	CHECK(std::all_of(l.begin(), l.end(), [](float v) { return v == 1.75f; }));
	CHECK(std::all_of(r.begin(), r.end(), [](float v) { return v == -1.75f; }));
}

TEST_CASE("Feedback at its maximum stays bounded, and ends in exact silence") {
	PitchShifter p;
	p.prepare(kRate);
	Settings s = defaults();
	s.feedback = static_cast<float>(kFeedbackMax);
	s.shiftSemitones = 7.0f;
	s.width = 1.0f;
	p.setTarget(s);

	std::vector<float> l = sine(220.0, 48000 * 4, 1.0f);
	std::vector<float> r = l;
	run(p, l, r);
	float peak = 0.0f;
	bool finite = true;
	for (size_t i = 0; i < l.size(); ++i) {
		finite = finite && std::isfinite(l[i]) && std::isfinite(r[i]);
		peak = std::max({ peak, std::fabs(l[i]), std::fabs(r[i]) });
	}
	CHECK(finite);
	// `softClip` asymptotes to 2 on the way INTO the line; out of it, two heads at most sum to that.
	CHECK(peak <= 2.0f);
	CHECK(peak > 0.5f);

	// THEN SILENCE IN. The loop decays to the floor, is flushed, and the line empties.
	std::vector<float> zl(48000 * 30, 0.0f);
	std::vector<float> zr = zl;
	run(p, zl, zr);
	size_t lastLive = 0;
	for (size_t i = 0; i < zl.size(); ++i) {
		if (zl[i] != 0.0f || zr[i] != 0.0f) {
			lastLive = i;
		}
	}
	MESSAGE("feedback tail at maximum: last non-zero sample at " << static_cast<double>(lastLive) / kRate << " s");
	const size_t tail = 48000;
	const bool silent = std::all_of(zl.end() - static_cast<long>(tail), zl.end(), [](float v) { return v == 0.0f; })
	                    && std::all_of(zr.end() - static_cast<long>(tail), zr.end(), [](float v) { return v == 0.0f; });
	CHECK(silent);
}

TEST_CASE("reset is as-if-just-switched-on: the same input gives the same output again") {
	Settings s = defaults();
	s.shiftSemitones = -7.0f;
	s.jitter = 1.0f;
	s.width = 0.6f;
	s.feedback = 0.7f;
	s.mode = Mode::Tight;

	PitchShifter p;
	p.prepare(kRate);
	p.setTarget(s);
	const std::vector<float> in = noise(30000);
	std::vector<float> l1 = in;
	std::vector<float> r1 = in;
	run(p, l1, r1);

	p.reset();
	CHECK(p.reseatsForTest() == 0);
	std::vector<float> l2 = in;
	std::vector<float> r2 = in;
	run(p, l2, r2);
	CHECK(l1 == l2);
	CHECK(r1 == r2);
	CHECK(l1 != in); // it did something
}

TEST_CASE("the same samples in a different partition are the same samples out") {
	Settings s = defaults();
	s.shiftSemitones = 5.0f;
	s.jitter = 0.8f;
	s.width = 1.0f;
	s.feedback = 0.6f;
	const std::vector<float> in = noise(20000);

	PitchShifter whole;
	whole.prepare(kRate);
	whole.setTarget(s);
	std::vector<float> l1 = in;
	std::vector<float> r1 = in;
	run(whole, l1, r1);

	PitchShifter parts;
	parts.prepare(kRate);
	parts.setTarget(s);
	std::vector<float> l2 = in;
	std::vector<float> r2 = in;
	const int sizes[] = { 1, 7, 64, 333, 1024 };
	size_t at = 0;
	size_t k = 0;
	while (at < in.size()) {
		const auto n = std::min(static_cast<size_t>(sizes[k++ % std::size(sizes)]), in.size() - at);
		parts.processStereo(l2.data() + at, r2.data() + at, static_cast<int>(n));
		at += n;
	}
	CHECK(l1 == l2);
	CHECK(r1 == r2);
}

TEST_CASE("the codec: defaults, a round trip, a short payload, and coercion at the ends") {
	const Settings d = defaults();
	CHECK(d.enabled);
	CHECK(d.scale == 0);
	CHECK(d.shiftSemitones == 0.0f);
	CHECK(d.fineCents == 0.0f);
	CHECK(d.mix == 1.0f);
	CHECK(d.mode == Mode::Smooth);
	CHECK(d.sizeMs == static_cast<float>(kSizeMsDefault));
	CHECK(d.jitter == 0.0f);
	CHECK(d.width == 0.0f);
	CHECK(d.feedback == 0.0f);
	// AND `Settings {}` AGREES WITH THE DESCRIPTOR, or a default-constructed device is not the one
	// the card shows.
	CHECK(Settings {} == d);

	State s;
	s.settings.scale = 6;
	s.settings.shiftSemitones = -9.0f;
	s.settings.fineCents = 33.0f;
	s.settings.mix = 0.4f;
	s.settings.mode = Mode::Reverse;
	s.settings.sizeMs = 120.0f;
	s.settings.jitter = 0.3f;
	s.settings.width = 0.7f;
	s.settings.feedback = 0.6f;
	CHECK(stateOf(rowOf(s), static_cast<size_t>(Param::kCount)) == s);

	// A PAYLOAD THAT STOPS AFTER Shift KEEPS EVERYTHING PAST IT AT ITS DEFAULT.
	const State shortRead = stateOf(rowOf(s), static_cast<size_t>(Param::Shift) + 1);
	CHECK(shortRead.settings.shiftSemitones == -9.0f);
	CHECK(shortRead.settings.mix == 1.0f);
	CHECK(shortRead.settings.feedback == 0.0f);

	State c;
	setValueOf(&c, Param::Shift, 99.0f);
	CHECK(c.settings.shiftSemitones == 24.0f);
	setValueOf(&c, Param::Feedback, 5.0f);
	CHECK(c.settings.feedback == static_cast<float>(kFeedbackMax));
	setValueOf(&c, Param::Scale, 3.4f);
	CHECK(c.settings.scale == 3);
	setValueOf(&c, Param::Size, 0.0f);
	CHECK(c.settings.sizeMs == static_cast<float>(kSizeMsMin));
}

TEST_CASE("the face: every control once, two rows of four, Mode under Scale") {
	std::set<uint8_t> seen;
	size_t cells = 0;
	for (const uint8_t index : kFaceOrder) {
		CHECK(seen.insert(index).second);
		REQUIRE(index < std::size(kParamDescs));
		if (!kParamDescs[index].stacked) {
			++cells;
		}
	}
	// EVERYTHING BUT THE ENABLE, which the card's head draws.
	CHECK(seen.size() == static_cast<size_t>(Param::kCount) - 1);
	CHECK(seen.count(static_cast<uint8_t>(Param::Enabled)) == 0);

	CHECK(kTraits.faceColumns == 4);
	CHECK(cells == 8);

	// MODE IS THE ONE STACKED CONTROL AND IT FOLLOWS SCALE — `stacked` means "under the previous".
	REQUIRE(std::size(kFaceOrder) >= 2);
	CHECK(kFaceOrder[0] == static_cast<uint8_t>(Param::Scale));
	CHECK(kFaceOrder[1] == static_cast<uint8_t>(Param::Mode));
	for (size_t i = 0; i < std::size(kParamDescs); ++i) {
		CHECK(kParamDescs[i].stacked == (i == static_cast<size_t>(Param::Mode)));
	}
	// THE TOP ROW IS THE FOUR A PLAYER REACHES FOR: Scale, Shift, Fine, Mix.
	CHECK(kFaceOrder[2] == static_cast<uint8_t>(Param::Shift));
	CHECK(kFaceOrder[3] == static_cast<uint8_t>(Param::Fine));
	CHECK(kFaceOrder[4] == static_cast<uint8_t>(Param::Mix));

	// BOTH PICKERS ARE PICKERS — more than two names, so `isOffOnChoice` is false and the card gives
	// them a menu rather than a chip that cycles.
	CHECK_FALSE(device::isOffOnChoice(kParamDescs[static_cast<size_t>(Param::Scale)]));
	CHECK_FALSE(device::isOffOnChoice(kParamDescs[static_cast<size_t>(Param::Mode)]));
	CHECK(kParamDescs[static_cast<size_t>(Param::Mode)].names.size() == static_cast<size_t>(Mode::kCount));
}
