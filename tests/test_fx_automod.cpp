/*
 * Copyright © 2026 Owlet Records
 *
 * This file is part of Feathers.
 *
 * Feathers is free software: you can redistribute it and/or modify it under the terms of the GNU
 * General Public License as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * Feathers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even
 * the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with Feathers. If not, see
 * <https://www.gnu.org/licenses/>.
 */

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "fx/automod/automod.h"
#include "fx/automod/automod_phase_space.h"
#include "character/phi_triangle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <limits>
#include <string_view>
#include <vector>

using namespace feathers::fx::automod;

namespace {

constexpr double kSr = 48000.0;

/// A COORDINATE AT WHICH EVERY ROAD IS LIVE — and finding it was necessary, not fussy.
///
/// THE FIRST FIXTURE USED (0.4, 0.6, 0.3) AND HAD TWO ROADS DEAD AT ONCE. Measured there:
/// `filterModDepth` is EXACTLY 0 and `envDepthInfluence` is EXACTLY 0, so the filter was never swept
/// and the envelope never steered anything. Every case in this file was exercising a device with two
/// of its four mechanisms switched off, and a known-bad that DELETED the envelope's process call
/// came back green because of it.
///
/// The phi triangles have deadzones — that is what a duty cycle under 1 means — so an arbitrary
/// coordinate has a real chance of sitting in one, and three coordinates make it likely. This one
/// was found by sweeping 60^3 positions for the one that maximises the WEAKEST of the four roads:
/// filter sweep 0.513, envelope steer -0.551, comb mix 0.430, comb range 147 samples.
///
/// THE GENERAL LESSON, and it is the third time this arc has met it: a fixture on a field with
/// deadzones must be CHOSEN by measurement, not picked for looking reasonable.
Settings liveSettings() {
	Settings s;
	s.enabled = true;
	s.type = 0.0339f;
	s.flavor = 0.6949f;
	s.mod = 0.0339f;
	s.rate = -1.0f; // octaves: 0.5X
	s.depth = 0.8f;
	s.mix = 1.0f;
	s.lfoMode = LfoModeChoice::Free;
	return s;
}

/// A signal with real structure — a note-like burst, so the envelope follower has something to
/// follow and the filter something to sweep across.
std::vector<float> source(int n) {
	std::vector<float> v(static_cast<size_t>(n));
	for (int i = 0; i < n; ++i) {
		const float t = static_cast<float>(i) / static_cast<float>(kSr);
		const float env = (i < n / 3) ? 1.0f : 0.25f;
		v[static_cast<size_t>(i)] =
		    env * 0.5f
		    * (std::sin(2.0f * 3.14159265f * 220.0f * t) + 0.4f * std::sin(2.0f * 3.14159265f * 990.0f * t));
	}
	return v;
}

/// Render `in` through a fresh device, chopped into blocks of `block`.
std::vector<float> renderStereo(const Settings& s, const std::vector<float>& in, int block) {
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	int done = 0;
	const int total = static_cast<int>(in.size());
	while (done < total) {
		const int n = std::min(block, total - done);
		a.processStereo(l.data() + done, r.data() + done, n);
		done += n;
	}
	return l;
}

} // namespace

TEST_CASE("backlog EH: a bypass resets automod, so re-enabling does not replay the comb's line") {
	// THE LOUDEST OF FOUR (shape 6): the comb kept its delay line, the filter its state, across a
	// bypass, so switching the device back on replayed seconds-old audio out of the comb. The multimode
	// filter's rule — the first bypassed block resets, once — applied here. Loud in; bypassed for a
	// block; enabled again on SILENCE: nothing out from the first sample.
	Settings s = liveSettings();
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);
	std::vector<float> l = source(8192);
	std::vector<float> r = l;
	a.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

	s.enabled = false;
	a.setTarget(s);
	std::vector<float> quiet(64, 0.0f), quietR(64, 0.0f);
	a.processStereo(quiet.data(), quietR.data(), 64);

	s.enabled = true;
	a.setTarget(s);
	std::vector<float> after(1024, 0.0f), afterR(1024, 0.0f);
	a.processStereo(after.data(), afterR.data(), static_cast<int>(after.size()));
	float ring = 0.0f;
	for (size_t i = 0; i < after.size(); ++i) {
		ring = std::max(ring, std::max(std::fabs(after[i]), std::fabs(afterR[i])));
	}
	CAPTURE(ring);
	CHECK(ring < 1e-6f);
}

TEST_CASE("automod: bypassed, it does not read or write one sample") {
	// THE BYPASS CONTRACT every device here keeps, and it is stated as "not written" rather than
	// "unchanged" because those differ: a device that wrote the same values back would pass a
	// comparison and still be doing work in a chain that expects none.
	Settings s = liveSettings();
	s.enabled = false;

	const std::vector<float> in = source(2048);
	std::vector<float> l = in;
	std::vector<float> r = in;

	Automod a;
	a.prepare(kSr);
	a.setTarget(s);
	a.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

	for (size_t i = 0; i < in.size(); ++i) {
		CAPTURE(i);
		CHECK(l[i] == in[i]);
		CHECK(r[i] == in[i]);
	}
	// AND IT DID NOT EVEN DERIVE — a bypassed device does no work at all.
	CHECK(a.deriveCountForTest() == 0);
}

TEST_CASE("automod: at zero mix the output is the input, though the device is running") {
	// NOT THE SAME CLAIM AS BYPASS, and the difference is the point: here the device IS running, its
	// LFO advancing and its filter ringing, and the wet/dry blend is what makes it inaudible. A
	// device that only passed through when DISABLED would fail this.
	Settings s = liveSettings();
	s.mix = 0.0f;

	const std::vector<float> in = source(2048);
	const std::vector<float> out = renderStereo(s, in, 256);

	for (size_t i = 0; i < in.size(); ++i) {
		CAPTURE(i);
		CHECK(out[i] == doctest::Approx(in[i]).epsilon(1e-6));
	}
}

TEST_CASE("automod: the same audio renders identically however the host chops it") {
	// THE CENTRAL CORRECTNESS PROPERTY, and the first draft of this device FAILED IT — it read the
	// LFO once per `processStereo` call, so the modulation rate was a function of the host's buffer
	// size. `multimode_filter.h` states the rule and `test_synth.cpp` pins it for a whole
	// performance; the fix was to anchor the tick grid to the STREAM.
	const std::vector<float> in = source(4096);

	const std::vector<float> at1 = renderStereo(liveSettings(), in, 1);
	const std::vector<float> at37 = renderStereo(liveSettings(), in, 37);
	const std::vector<float> at512 = renderStereo(liveSettings(), in, 512);

	for (size_t i = 0; i < in.size(); ++i) {
		CAPTURE(i);
		CHECK(at1[i] == at37[i]);
		CHECK(at1[i] == at512[i]);
	}

	// NON-VACUITY: the device actually did something, so three identical silences cannot pass.
	float diff = 0.0f;
	for (size_t i = 0; i < in.size(); ++i) {
		diff += std::fabs(at1[i] - in[i]);
	}
	CHECK(diff > 1.0f);
}

TEST_CASE("automod: the derive runs when a coordinate moves and not otherwise") {
	// A CLAIM ABOUT WORK NOT DONE, which leaves no trace in the audio — so it needs a counter, which
	// is what `deriveCountForTest` is for. The gate is an equality on the arguments rather than a set
	// of prev-fields, so this also pins that a value returned whole cannot go stale.
	Automod a;
	a.prepare(kSr);
	Settings s = liveSettings();
	a.setTarget(s);

	std::vector<float> l(512, 0.0f);
	std::vector<float> r(512, 0.0f);

	a.processStereo(l.data(), r.data(), 512);
	const long first = a.deriveCountForTest();
	CHECK(first == 1);

	// Eight more blocks with nothing moved: still one.
	for (int i = 0; i < 8; ++i) {
		a.processStereo(l.data(), r.data(), 512);
	}
	CHECK(a.deriveCountForTest() == first);

	// A COORDINATE MOVES: exactly one more.
	s.mod = 0.7f;
	a.setTarget(s);
	a.processStereo(l.data(), r.data(), 512);
	CHECK(a.deriveCountForTest() == first + 1);

	// AND A NON-COORDINATE DOES NOT. Mix is a per-sample gain and reaches nothing the field decides.
	s.mix = 0.3f;
	a.setTarget(s);
	a.processStereo(l.data(), r.data(), 512);
	CHECK(a.deriveCountForTest() == first + 1);

	// BUT THE RATE DOES, because the springs scale with it — the one non-obvious member of the key.
	s.rate = -0.2f; // octaves: a little under 1X, the same key change the old 0.9 was
	a.setTarget(s);
	a.processStereo(l.data(), r.data(), 512);
	CHECK(a.deriveCountForTest() == first + 2);
}

TEST_CASE("automod: a higher note raises the filter's corner and shortens the comb's delay") {
	// TWO VALUES, READ THROUGH SEAMS, because neither is recoverable from the output — a filtered
	// buffer does not carry its corner and a comb's output carries every delay it has ever had.
	//
	// THEY MOVE IN OPPOSITE DIRECTIONS, and that is the claim: a comb's delay is the RECIPROCAL of
	// its pitch, so an octave up doubles the cutoff and halves the delay. A port that tracked both
	// the same way would sound plausible and be wrong.
	// AT ZERO DEPTH, so the numbers read back are the PITCH RATIO alone. With the LFO live the delay
	// also carries wherever the sweep happened to be, and a ratio taken across two different sweep
	// positions is not a statement about pitch tracking — the first draft asserted 4x against a
	// measured 6.21 for exactly that reason.
	// AT THE ORIGIN COORDINATE, WHERE THE LFO'S WAVEFORM IS FLAT — which `test_phi_lfo_shape.cpp`
	// pins as the one coordinate whose shape collapses to rest. That makes all four taps exactly
	// zero, so the springs snap to zero and STAY there, and the delay read back is the pitch ratio
	// and nothing else.
	//
	// ZERO DEPTH ALONE WAS NOT ENOUGH, which is the mistake worth recording: the springs are snapped
	// to the LFO's reading when the shape is built, so they START at the tap value and walk toward
	// the target. At a comb spring of 1.8 Hz that walk takes seconds, and a 1024-sample run measured
	// the residual rather than the ratio — 6.13 against an expected 4.
	Settings s = liveSettings();
	s.mod = 0.0f;
	s.depth = 0.0f;
	s.manual = 0.0f;
	std::vector<float> l(1024, 0.1f);
	std::vector<float> r(1024, 0.1f);

	auto runAtNote = [&](float semis) {
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		feathers::device::NoteContext note;
		note.pitchSemitones = semis;
		note.gate = true;
		a.setNoteContext(note);
		std::vector<float> ll = l;
		std::vector<float> rr = r;
		a.processStereo(ll.data(), rr.data(), 1024);
		return std::make_pair(a.filterCutoffForTest(), a.combDelayForTest());
	};

	const auto [cutoffLow, delayLow] = runAtNote(48.0f);  // an octave below middle C
	const auto [cutoffHigh, delayHigh] = runAtNote(72.0f); // an octave above

	CHECK(cutoffHigh > cutoffLow);
	CHECK(delayHigh < delayLow);

	// AND BY THE RATIO THE OCTAVES IMPLY, within the clamps: two octaves apart is 4x on the cutoff.
	CHECK(cutoffHigh / cutoffLow == doctest::Approx(4.0f).epsilon(0.05));
	CHECK(delayLow / delayHigh == doctest::Approx(4.0f).epsilon(0.05));
}

TEST_CASE("automod: a note restarts the LFO in Retrigger and leaves it alone in Free") {
	// READ THE PHASE, not the output — the waveform passes the same level several times a cycle, so
	// a value cannot say whether it restarted.
	std::vector<float> l(4096, 0.0f);
	std::vector<float> r(4096, 0.0f);

	auto phaseAfterNote = [&](LfoModeChoice mode) {
		Settings s = liveSettings();
		s.lfoMode = mode;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		std::vector<float> ll = l;
		std::vector<float> rr = r;
		a.processStereo(ll.data(), rr.data(), 4096);
		const double before = a.lfo().phase();

		feathers::device::NoteContext note;
		note.pitchSemitones = 60.0f;
		note.gate = true;
		a.setNoteContext(note);
		return std::make_pair(before, a.lfo().phase());
	};

	const auto [freeBefore, freeAfter] = phaseAfterNote(LfoModeChoice::Free);
	CHECK(freeAfter == freeBefore);
	CHECK(freeBefore > 0.0); // NON-VACUITY: it had actually travelled

	const auto [retrigBefore, retrigAfter] = phaseAfterNote(LfoModeChoice::Retrigger);
	CHECK(retrigBefore > 0.0);
	CHECK(retrigAfter != retrigBefore);

	// A HELD GATE MUST NOT RESTART IT AGAIN — the EDGE is what triggers, not the level.
	Settings s = liveSettings();
	s.lfoMode = LfoModeChoice::Retrigger;
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);
	feathers::device::NoteContext note;
	note.pitchSemitones = 60.0f;
	note.gate = true;
	a.setNoteContext(note);
	std::vector<float> ll = l;
	std::vector<float> rr = r;
	a.processStereo(ll.data(), rr.data(), 4096);
	const double travelled = a.lfo().phase();
	a.setNoteContext(note); // same gate, still held
	CHECK(a.lfo().phase() == travelled);
}

TEST_CASE("automod: the phase sink reports where the LFO is, at the phase the taps were read at") {
	// **A PHASE CANNOT BE RECOVERED FROM THE OUTPUT.** CLAUDE.md's rule, and this waveform is the
	// worst case of it: four taps of one shape at four offsets, summed into a filter and a comb. So
	// the value is asserted directly, against an independent restatement of `setRateHz` +
	// `advance` rather than against the device's own `lfo().phase()`, which would be the same road.
	Settings s = liveSettings();
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);

	std::vector<float> reported;
	a.setReadingsCallback([&reported](float phase, float) { reported.push_back(phase); });

	constexpr int kTicks = 40;
	const std::vector<float> in = source(kTickSamples * kTicks);
	std::vector<float> l = in;
	std::vector<float> r = in;
	a.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

	// ONE PER TICK — the seam both process functions funnel through, so neither can be forgotten.
	REQUIRE(reported.size() == static_cast<size_t>(kTicks));

	// A PREPARED DEVICE HAS NOT MOVED, so the first report is the origin. This is also what makes
	// the arithmetic below anchored rather than merely self-consistent.
	CHECK(reported.front() == 0.0f);

	// **THE INDEPENDENT RESTATEMENT.** `setRateHz` rounds the rate into a 2^32 turn increment and
	// `advance` multiplies it modularly, so the k-th tick starts at `k * kTickSamples * inc` turns.
	// The rounding is part of the restatement rather than a tolerance: it is what the device does.
	const double hz = a.effectiveRateHz();
	REQUIRE(hz > 0.0f); // NON-VACUITY: a still LFO would satisfy every step check below
	const auto inc = static_cast<uint32_t>(std::llround(hz / kSr * 4294967296.0));
	const auto turnAt = [inc](uint64_t samples) {
		return static_cast<double>(static_cast<uint32_t>(static_cast<uint64_t>(inc) * samples))
		       / 4294967296.0;
	};
	for (size_t k = 0; k < reported.size(); ++k) {
		CAPTURE(k);
		// AN ABSOLUTE BOUND, not `Approx().epsilon()`: the quantities live in 0..1 where a relative
		// tolerance is not a comparison at all (playbook, and it cost three assertions there).
		CHECK(std::fabs(static_cast<double>(reported[k]) - turnAt(k * kTickSamples)) < 1e-6);
	}
	// AND IT WENT SOMEWHERE, STRICTLY — the second assertion whose job is to prove the first is not
	// vacuous. The loop above already cannot be satisfied by a still LFO (`turnAt` is non-zero for
	// every k > 0), so this is a cross-check rather than the bound doing the work.
	//
	// **THE NUMBER IS MEASURED, NOT CHOSEN.** At this fixture's rate forty ticks is 0.00260 turns.
	// The first draft asked for 0.05 because 0.05 looked reasonable and it failed — which is the
	// third time in this file a fixture's magnitude has had to come from the instrument.
	CHECK(reported.back() > 0.002f);
	for (size_t k = 1; k < reported.size(); ++k) {
		CAPTURE(k);
		CHECK(reported[k] > reported[k - 1]);
	}

	// **PUBLISHED BEFORE THE ADVANCE, WHICH IS THE HALF A DIFFERENCE CHECK CANNOT SEE.** `lfo_.tick`
	// reads then advances, so the last report is one tick BEHIND where the LFO now is. Publishing
	// after the tick would leave the two equal and put a marker one tick ahead of the sound it marks
	// — a defect worth exactly 1.33 ms, invisible in any picture, and fatal to the claim the marker
	// makes.
	CHECK(std::fabs(a.lfo().phase() - turnAt(static_cast<uint64_t>(kTicks) * kTickSamples)) < 1e-6);
	CHECK(a.lfo().phase() != static_cast<double>(reported.back()));

	SUBCASE("the mono path publishes too, because the seam is the tick and not the caller") {
		Automod mono;
		mono.prepare(kSr);
		mono.setTarget(s);
		size_t count = 0;
		float last = -1.0f;
		mono.setReadingsCallback([&count, &last](float phase, float) {
			++count;
			last = phase;
		});
		std::vector<float> m = in;
		mono.processMono(m.data(), static_cast<int>(m.size()));
		CHECK(count == static_cast<size_t>(kTicks));
		CHECK(std::fabs(static_cast<double>(last)
		                - turnAt(static_cast<uint64_t>(kTicks - 1) * kTickSamples))
		      < 1e-6);
	}

	SUBCASE("a bypassed device publishes nothing at all") {
		// NOT A GAP: a bypassed device never reaches `beginTick`, its LFO never moves, and the last
		// number anybody published is still the true one.
		Settings off = s;
		off.enabled = false;
		Automod dark;
		dark.prepare(kSr);
		dark.setTarget(off);
		size_t count = 0;
		dark.setReadingsCallback([&count](float, float) { ++count; });
		std::vector<float> ll = in;
		std::vector<float> rr = in;
		dark.processStereo(ll.data(), rr.data(), static_cast<int>(ll.size()));
		CHECK(count == 0);
	}

	SUBCASE("Stop publishes, and publishes the same number every time") {
		// The mode that freezes the phase rather than ignoring it — so the marker holds still rather
		// than disappearing, which is what a player watching a stopped LFO should see.
		Settings stop = s;
		stop.lfoMode = LfoModeChoice::Stop;
		Automod frozen;
		frozen.prepare(kSr);
		frozen.setTarget(stop);
		std::vector<float> seen;
		frozen.setReadingsCallback([&seen](float phase, float) { seen.push_back(phase); });
		std::vector<float> ll = in;
		std::vector<float> rr = in;
		frozen.processStereo(ll.data(), rr.data(), static_cast<int>(ll.size()));
		REQUIRE(seen.size() == static_cast<size_t>(kTicks));
		for (const float phase : seen) {
			CHECK(phase == 0.0f);
		}
	}
}

TEST_CASE("automod: Stop hands the modulation to Manual, which is what the mode name promises") {
	// THE MODE THE LFO ITSELF DOES NOT KNOW ABOUT — `RunMode` has three entries and this is the
	// fourth choice, because "ignore the LFO" is the EFFECT's decision about which signal to read.
	//
	// AND MANUAL IS NOT SCALED BY DEPTH IN THIS MODE, which is the detail a port would lose: if it
	// were, Manual would be a product of two knobs and the mode would go silent at zero depth.
	Settings s = liveSettings();
	s.lfoMode = LfoModeChoice::Stop;
	s.depth = 0.0f;
	s.manual = 1.0f;

	const std::vector<float> in = source(2048);
	const std::vector<float> atManualUp = renderStereo(s, in, 256);

	s.manual = -1.0f;
	const std::vector<float> atManualDown = renderStereo(s, in, 256);

	// The two must differ: Manual reached the sound at zero depth.
	float diff = 0.0f;
	for (size_t i = 0; i < in.size(); ++i) {
		diff += std::fabs(atManualUp[i] - atManualDown[i]);
	}
	CHECK(diff > 1.0f);

	// AND THE LFO DID NOT ADVANCE — Stop means frozen, not merely ignored.
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	a.processStereo(l.data(), r.data(), 2048);
	CHECK(a.lfo().phase() == 0.0);
}

TEST_CASE("automod: the envelope steers the filter, so a loud input lands somewhere a quiet one does not") {
	// FOUND BY A KNOWN-BAD THAT DELETED THE ENVELOPE'S `process` CALL AND CAME BACK GREEN. The whole
	// component was wired in and nothing asserted it reached the sound — which is the "name the
	// consumer" rule failing in the one place it is hardest to see, because the envelope DOES have a
	// consumer, and that consumer was multiplying by a coefficient that happened to be zero.
	//
	// READ THE CUTOFF, not the buffer: the claim is that the corner moves with the input's LEVEL, and
	// a filtered signal does not carry its corner.
	// THE TRIM IS SET RATHER THAN INHERITED, because there is now a control that governs exactly the
	// quantity this case measures. Depending on its default would make this an assertion about a
	// TUNING DECISION — how much steering ships — rather than about the mechanism, and the two should
	// fail separately. MEASURED: at the declared default of -18 dB the corners differ by 4.07%, just
	// under this case's 5% bar, so leaving it implicit turned a passing mechanism into a red case.
	// **MEASURED OVER THE RUN, NOT AT ONE INSTANT (backlog IS).** This read the corner at the last sample
	// of an 8192-sample run and asked the two to differ by 1.5% there. When the filter got one corner per
	// ear that single reading fell to 1.2% — while the STEER the mechanism computes stayed identical to six
	// digits (mean spring-target difference 0.136222 on the old engine and the new, measured). What moved
	// was which corner is read and where the springs start, not the envelope's road. So the claim is now
	// stated as what it is: the envelope's steer (read through its seam), and the corner's AVERAGE
	// distance over the run, which one lucky or unlucky instant cannot decide.
	struct Run {
		std::vector<float> corner;
		std::vector<float> target;
	};
	const auto runAt = [](float amplitude, float envTrimDb) {
		Automod a;
		a.prepare(kSr);
		Settings live = liveSettings();
		live.envTrim = envTrimDb;
		a.setTarget(live);
		Run out;
		std::vector<float> l(static_cast<size_t>(kTickSamples));
		std::vector<float> r(static_cast<size_t>(kTickSamples));
		for (int b = 0; b < 256; ++b) {
			for (int i = 0; i < kTickSamples; ++i) {
				const float t = static_cast<float>(b * kTickSamples + i) / static_cast<float>(kSr);
				l[static_cast<size_t>(i)] = amplitude * std::sin(2.0f * 3.14159265f * 220.0f * t);
				r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
			}
			a.processStereo(l.data(), r.data(), kTickSamples);
			out.corner.push_back(a.filterCutoffForTest());
			out.target.push_back(a.springTargetForTest(0));
		}
		return out;
	};
	// THE LAST THREE QUARTERS, once the follower has settled on the level.
	const auto meanOctaves = [](const Run& x, const Run& y) {
		double sum = 0.0;
		for (size_t i = 64; i < x.corner.size(); ++i) {
			sum += std::fabs(std::log2(static_cast<double>(x.corner[i]) / static_cast<double>(y.corner[i])));
		}
		return sum / static_cast<double>(x.corner.size() - 64);
	};
	const auto meanSteer = [](const Run& x, const Run& y) {
		double sum = 0.0;
		for (size_t i = 64; i < x.target.size(); ++i) {
			sum += std::fabs(static_cast<double>(x.target[i]) - static_cast<double>(y.target[i]));
		}
		return sum / static_cast<double>(x.target.size() - 64);
	};

	const Run quiet = runAt(0.02f, 0.0f);
	const Run loud = runAt(0.9f, 0.0f);

	// THE STEER, BY ITS SEAM — the mechanism's own number, which the change to one corner per ear did not
	// touch. MEASURED 0.136 here.
	CHECK(meanSteer(loud, quiet) > 0.1);
	// AND THE CORNER HEARS IT — MEASURED a mean 0.033 octaves apart (0.046 with the old averaged corner).
	// The direction is the field's to choose, since `envDepthInfluence` is bipolar.
	CHECK(meanOctaves(loud, quiet) > 0.02);

	// AND THE TRIM IS WHAT DECIDES HOW MUCH, which is the new control's whole claim. At the firmware's
	// own operating point the steering all but vanishes — `ONE_Q31 / EFFECTIVE_0DBFS_Q31` is 128, so
	// the hardware's envelope enters the depth term at 1/128 and moves the corner by well under a
	// percent. That is the parity end of the knob, and it must be reachable.
	const Run quietAtParity = runAt(0.02f, -42.1f);
	const Run loudAtParity = runAt(0.9f, -42.1f);
	CHECK(meanOctaves(loudAtParity, quietAtParity) < 0.2 * meanOctaves(loud, quiet));
	CHECK(meanSteer(loudAtParity, quietAtParity) < 0.2 * meanSteer(loud, quiet));

	// NON-VACUITY: every corner is a real corner inside the filter's range, so "one of them collapsed"
	// cannot pass for a difference.
	for (const Run* run : { &quiet, &loud }) {
		for (const float c : run->corner) {
			CHECK(c > kFilterMinHz);
			CHECK(c < static_cast<float>(kSr) * kFilterNyquistFraction);
		}
	}
}

namespace {

/// Feed `frames` of a 220 Hz tone at `amplitude` in tick-sized chunks, calling `after` once per
/// chunk with the chunk index — so a case can read the block seams at known block boundaries.
template <typename F>
void feedToneInTicks(Automod& a, float amplitude, int frames, F&& after) {
	std::vector<float> l(static_cast<size_t>(kTickSamples));
	std::vector<float> r(static_cast<size_t>(kTickSamples));
	int chunk = 0;
	for (int at = 0; at + kTickSamples <= frames; at += kTickSamples, ++chunk) {
		for (int i = 0; i < kTickSamples; ++i) {
			const float t = static_cast<float>(at + i) / static_cast<float>(kSr);
			l[static_cast<size_t>(i)] = amplitude * std::sin(2.0f * 3.14159265f * 220.0f * t);
			r[static_cast<size_t>(i)] = l[static_cast<size_t>(i)];
		}
		a.processStereo(l.data(), r.data(), kTickSamples);
		after(chunk);
	}
}

} // namespace

TEST_CASE("parity: the envelope's three roads into the filter target restate the firmware's sum — depth, "
          "derivative→depth and value — read off the block that used them") {
	// THE REFERENCE IS `automodulator.cpp`'s per-buffer block (envelopeSteerFor in automod.h carries
	// it): target = (lfo + env·valueInfluence) · saturate(depth + depth·depthInfluence·env +
	// depth·derivInfluence·derivNorm) + manual. For a month this device computed all five influences
	// and read one (backlog EH); the derivative and value roads are the ones this case can see.
	//
	// EVERY CLAIM IS ABOUT A NUMBER, read through the seams, because the three roads are not
	// separable in the audio: a swell of depth and a shift of value both move the same corner.
	Settings live = liveSettings();
	live.envTrim = 0.0f; // 0 dB into the follower, so a 0.8 tone is a real level rather than 0.1
	live.manual = 0.15f; // so the "+ manual" term is visibly a term
	Automod a;
	a.prepare(kSr);
	a.setTarget(live);

	// One silent tick so the derive has run and the influences can be read and checked for teeth.
	{
		std::vector<float> z(static_cast<size_t>(kTickSamples), 0.0f), z2 = z;
		a.processStereo(z.data(), z2.data(), kTickSamples);
	}
	const AutomodDerived d = a.derived();
	REQUIRE(std::fabs(d.envDepthInfluence) > 0.2f);      // measured -0.550 at this coordinate
	REQUIRE(std::fabs(d.envDerivDepthInfluence) > 0.2f); // measured -0.467
	REQUIRE(std::fabs(d.envValueInfluence) > 0.2f);      // measured -0.467
	const float depth = live.depth;

	// AN INDEPENDENT RESTATEMENT of the block's scale from its inputs — the follower's bounded level
	// and the normalised derivative — so the seam's number is checked against the formula rather
	// than against itself.
	const auto expectedScale = [&](float env, float derivNorm) {
		return std::clamp(depth + depth * d.envDepthInfluence * env + depth * d.envDerivDepthInfluence * derivNorm,
		                  -1.0f, 1.0f);
	};

	// THE ATTACK. A tone from silence: the follower rises, the derivative is positive and large for a
	// few blocks, then settles. Read every block; keep the one where the derivative peaked.
	float peakDerivNorm = 0.0f;
	float scaleAtPeak = 0.0f;
	float envAtPeak = 0.0f;
	feedToneInTicks(a, 0.8f, 16 * kTickSamples, [&](int) {
		// THE SUM, at every block: target == (tap + envValue) * scale + manual.
		for (int ch = 0; ch < 2; ++ch) {
			const float restated =
			    (a.lfoTapForTest(ch) + a.envValueForTest(ch)) * a.envScaleForTest(ch) + live.manual;
			CHECK(a.springTargetForTest(ch) == doctest::Approx(restated).epsilon(1e-5));
		}
		if (a.derivNormForTest(0) > peakDerivNorm) {
			peakDerivNorm = a.derivNormForTest(0);
			scaleAtPeak = a.envScaleForTest(0);
			envAtPeak = a.envelopeForTest();
		}
	});
	CAPTURE(peakDerivNorm);
	REQUIRE(peakDerivNorm > 0.3f); // a real attack reached the derivative road
	// THE DERIVATIVE ROAD MOVED THE SCALE, by the formula and by enough to hear: the scale at the peak
	// is the formula's value WITH the derivative term, and differs from the same formula WITHOUT it.
	CHECK(scaleAtPeak == doctest::Approx(expectedScale(envAtPeak, peakDerivNorm)).epsilon(1e-4));
	CHECK(std::fabs(scaleAtPeak - expectedScale(envAtPeak, 0.0f)) > 0.03f);

	// SETTLED. A second of tone: the derivative is back near zero, the level road alone steers the
	// scale, and the VALUE road is a live term in the target.
	feedToneInTicks(a, 0.8f, static_cast<int>(kSr), [](int) {});
	const float env = a.envelopeForTest();
	CAPTURE(env);
	REQUIRE(env > 0.3f);
	CHECK(std::fabs(a.derivNormForTest(0)) < 0.02f);
	CHECK(a.envScaleForTest(0) == doctest::Approx(expectedScale(env, a.derivNormForTest(0))).epsilon(1e-4));
	CHECK(a.envValueForTest(0) == doctest::Approx(env * d.envValueInfluence).epsilon(1e-5));
	CHECK(std::fabs(a.envValueForTest(0)) > 0.1f); // non-vacuous: the value road carries something
	for (int ch = 0; ch < 2; ++ch) {
		const float restated =
		    (a.lfoTapForTest(ch) + a.envValueForTest(ch)) * a.envScaleForTest(ch) + live.manual;
		CHECK(a.springTargetForTest(ch) == doctest::Approx(restated).epsilon(1e-5));
	}
}

TEST_CASE("parity: at Depth 0 the envelope steers nothing — Manual reaches the sound unscaled, as the "
          "firmware gates every envelope term by depth") {
	// THE OLD ROAD multiplied the spring's output by `1 + env * influence`, and at Depth 0 that output
	// IS Manual — so a loud input moved a corner the hand had parked. The firmware multiplies each
	// envelope term by |depth| and adds Manual after, so Depth 0 is Manual and nothing else.
	Settings live = liveSettings();
	live.envTrim = 0.0f;
	live.depth = 0.0f;
	live.manual = 0.3f;

	const auto cutoffAfter = [&](float amplitude) {
		Automod a;
		a.prepare(kSr);
		a.setTarget(live);
		feedToneInTicks(a, amplitude, static_cast<int>(kSr), [&](int) {
			// THE TARGET IS MANUAL, EXACTLY, on every block — whatever the envelope is doing.
			CHECK(a.springTargetForTest(0) == doctest::Approx(live.manual).epsilon(1e-6));
			CHECK(a.springTargetForTest(1) == doctest::Approx(live.manual).epsilon(1e-6));
		});
		// NON-VACUITY: the follower did see the tone, so "nothing steered" is a gate and not an
		// absence of signal.
		if (amplitude > 0.5f) {
			CHECK(a.envelopeForTest() > 0.3f);
		}
		return a.filterCutoffForTest();
	};
	const float quiet = cutoffAfter(0.01f);
	const float loud = cutoffAfter(0.9f);
	CHECK(loud == doctest::Approx(quiet).epsilon(1e-4));
	CHECK(quiet > kFilterMinHz);
}

TEST_CASE("automod: mono renders the stereo path's left channel") {
	// ONE CODE PATH, ASSERTED. A duplicated sample loop is a second thing to keep in step, and the
	// way to know it has not drifted is to check that mono IS the left channel rather than merely
	// resembling it.
	const std::vector<float> in = source(2048);
	const std::vector<float> stereoLeft = renderStereo(liveSettings(), in, 256);

	Automod a;
	a.prepare(kSr);
	a.setTarget(liveSettings());
	std::vector<float> mono = in;
	int done = 0;
	while (done < static_cast<int>(mono.size())) {
		const int n = std::min(256, static_cast<int>(mono.size()) - done);
		a.processMono(mono.data() + done, n);
		done += n;
	}

	for (size_t i = 0; i < in.size(); ++i) {
		CAPTURE(i);
		CHECK(mono[i] == stereoLeft[i]);
	}
}

TEST_CASE("automod: no coordinate, note or signal produces a non-finite sample") {
	// THE SWEEP THAT MATTERS FOR A DEVICE WITH FEEDBACK IN IT. A comb with feedback and a resonant
	// filter are both things that can run away, and the coordinates that drive them come from a field
	// nobody has bounded by hand.
	const std::vector<float> in = source(1024);

	for (float t : { 0.0f, 0.33f, 0.66f, 1.0f }) {
		for (float f : { 0.0f, 0.5f, 1.0f }) {
			for (float m : { 0.0f, 0.5f, 1.0f }) {
				Settings s = liveSettings();
				s.type = t;
				s.flavor = f;
				s.mod = m;
				s.depth = 1.0f;
				s.mix = 1.0f;
				const std::vector<float> out = renderStereo(s, in, 128);
				CAPTURE(t);
				CAPTURE(f);
				CAPTURE(m);
				float peak = 0.0f;
				for (float v : out) {
					REQUIRE(std::isfinite(v));
					peak = std::max(peak, std::fabs(v));
				}
				// AND BOUNDED, not merely finite — a comb ringing toward full scale is a fault even
				// while every sample is a number.
				CHECK(peak < 20.0f);
			}
		}
	}
}

TEST_CASE("automod: a non-finite control cannot reach the audio") {
	const float nan = std::numeric_limits<float>::quiet_NaN();
	Settings s = liveSettings();
	s.type = nan;
	s.flavor = nan;
	s.mod = nan;
	s.gamma = nan;
	s.rate = nan;
	s.depth = nan;
	s.manual = nan;
	s.mix = nan;

	const std::vector<float> in = source(1024);
	const std::vector<float> out = renderStereo(s, in, 128);
	for (float v : out) {
		REQUIRE(std::isfinite(v));
	}

	// A good neighbour, so "it silenced everything" cannot pass for a fix.
	const std::vector<float> good = renderStereo(liveSettings(), in, 128);
	float diff = 0.0f;
	for (size_t i = 0; i < in.size(); ++i) {
		diff += std::fabs(good[i] - in[i]);
	}
	CHECK(diff > 1.0f);
}

TEST_CASE("automod: the filter's base corner is a BRIGHT FLOOR, not a position across the range") {
	// THE RULE THIS PORT GOT WRONG ONCE, AND A CASE SO IT CANNOT BE UNDONE QUIETLY.
	//
	// `filterCutoffBase` is an OFFSET FROM A BRIGHT FLOOR in the firmware — `kCutoffMid +
	// base * halfRange/2`, so the corner spans 50.1% to 75.0% of its range and NEVER goes below the
	// midpoint. The first port read it as an absolute position across 60 Hz..12 kHz, so a base of 0
	// landed at 60 Hz — and because that triangle has a 25% deadzone, whole flavour bands sat there.
	// MEASURED: 14.69% of the coordinate cube below -18 dB and init at -24.4 dB, which is what was
	// heard as "completely quiet".
	//
	// SO THE CLAIM IS ABOUT WHERE THE FLOOR IS, and it is asserted at the seam because a corner is not
	// recoverable from a filtered buffer.
	Settings s = liveSettings();
	s.mod = 0.0f;    // the flat-waveform coordinate: no LFO travel, so the corner is the BASE alone
	s.depth = 0.0f;  // and no modulation on top of it
	s.manual = 0.0f;

	// A COORDINATE WHOSE `filterCutoffBase` IS ZERO — the deadzone that used to mean 60 Hz. Flavour
	// 0.40 measures exactly 0.0000, so this is the worst case rather than an average one.
	s.flavor = 0.40f;
	Automod low;
	low.prepare(kSr);
	low.setTarget(s);
	std::vector<float> l(4096, 0.05f);
	std::vector<float> r(4096, 0.05f);
	low.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

	REQUIRE(low.derived().filterCutoffBase == doctest::Approx(0.0f).scale(1.0).epsilon(1e-4));
	// THE FLOOR IS BRIGHT. Not "above 60 Hz" — in the kilohertz, where an auto-wah's rest position
	// belongs. A bound rather than a value, because the exact floor is a tuning number and this case
	// is about which ORDER OF MAGNITUDE it lives in.
	CHECK(low.filterCutoffForTest() > 3000.0f);
	CHECK(low.filterCutoffForTest() < 8000.0f);

	// AND THE FIELD PUSHES IT UP FROM THERE, never down: a base of 1 is above a base of 0.
	s.flavor = 0.20f; // measures cutoffBase 0.9023 — near the top of the field's travel
	Automod high;
	high.prepare(kSr);
	high.setTarget(s);
	std::vector<float> l2(4096, 0.05f);
	std::vector<float> r2(4096, 0.05f);
	high.processStereo(l2.data(), r2.data(), static_cast<int>(l2.size()));

	REQUIRE(high.derived().filterCutoffBase > 0.8f);
	CHECK(high.filterCutoffForTest() > low.filterCutoffForTest());
}

TEST_CASE("automod: the tail reaches EXACT silence after a second of silent input") {
	// A COMB WITH FEEDBACK DOES NOT REACH ZERO ON ITS OWN — it decays geometrically and arrives only
	// when its last denormal flushes. MEASURED before the flush existed: still writing at block 396
	// of 400, about 4.2 seconds after the burst. Nobody hears that, and every downstream device pays
	// for it in denormal handling.
	//
	// THE CONDITION IS ON THE INPUT, not the output, which is `multimode_filter.h`'s rule and its
	// reason: a delay line holds a full-scale sample while the output between taps is nothing, so
	// only the input can promise there is nothing left to come out.
	// AT A COORDINATE WHERE THE NATURAL DECAY DOES NOT GET THERE ON ITS OWN, and choosing it took a
	// sweep. THE FIRST DRAFT USED type 0.5 AND WAS VACUOUS: measured, the tail there reaches exact
	// zero at 0.78 s all by itself, which is BEFORE the flush arms at 1.0 s — so the case passed with
	// the flush disarmed and proved nothing. A known-bad that stopped the flush ever arming was GREEN.
	//
	// Swept 11 x 11 x 5 coordinates for the longest natural tail: at (0, 0, 0) it does not reach zero
	// inside 0.96 s at all, so the flush is the only thing that ends it. That is also the init point,
	// which is a pleasing place for the guarantee to be exercised.
	Settings s = liveSettings();
	s.type = 0.0f;
	s.flavor = 0.0f;
	s.mod = 0.0f;
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);

	// A burst, then silence for longer than the rest window.
	std::vector<float> l(2048), r(2048);
	for (size_t i = 0; i < l.size(); ++i) {
		l[i] = 0.4f * std::sin(2.0f * 3.14159265f * 300.0f * static_cast<float>(i) / static_cast<float>(kSr));
		r[i] = l[i];
	}
	a.processStereo(l.data(), r.data(), static_cast<int>(l.size()));

	float peak = 0.0f;
	for (float v : l) {
		peak = std::max(peak, std::fabs(v));
	}
	REQUIRE(peak > 1e-4f); // NON-VACUITY: the burst reached the output

	const int rest = static_cast<int>(kRestSilentSeconds * kSr) + 4096;
	std::vector<float> sl(static_cast<size_t>(rest), 0.0f);
	std::vector<float> sr(static_cast<size_t>(rest), 0.0f);
	a.processStereo(sl.data(), sr.data(), rest);

	// EXACT ZERO in the last block, not "small".
	for (size_t i = sl.size() - 512; i < sl.size(); ++i) {
		CAPTURE(i);
		CHECK(sl[i] == 0.0f);
		CHECK(sr[i] == 0.0f);
	}

	// AND IT STILL WORKS AFTERWARDS — a flush that left the device dead would pass the check above.
	std::vector<float> again = l;
	std::vector<float> again2 = r;
	a.processStereo(again.data(), again2.data(), static_cast<int>(again.size()));
	float peak2 = 0.0f;
	for (float v : again) {
		peak2 = std::max(peak2, std::fabs(v));
	}
	CHECK(peak2 > 1e-4f);
}

TEST_CASE("automod: the phase controls reach far enough that the field never repeats inside them") {
	// THE DEFECT: gamma stopped at 64 — and the comment justifying 64 claimed the field "never
	// repeats within a session's worth of turning", which was an assumption written as a fact.
	//
	// IT DOES NOT REPEAT AT ALL. This case is the measurement that replaced the assumption: the
	// derive at a large phase must still differ from the derive at zero, so a cap can only ever be a
	// REACH limit rather than a period. If somebody later finds a period and caps at it, this fails
	// and they will have to say so.
	AutomodCoords base;
	base.lfoHz = 1.0f;
	const AutomodDerived ref = deriveAutomod(base);

	for (float g : { 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 256.0f, 512.0f, 1024.0f }) {
		AutomodCoords c = base;
		c.gammaPhase = g;
		CAPTURE(g);
		CHECK_FALSE(deriveAutomod(c) == ref);
	}

	// AND THE CAP IS WHERE THE STORAGE STOPS COUNTING — PINNED BY THE MECHANISM, NOT BY THE NUMBER.
	//
	// A device row stores these as FLOAT, and a float's step grows with magnitude. The honest cap is
	// the last value at which adding ONE LAP still changes anything; past 2^24 the ulp is 2, so
	// `gamma + 1.0 == gamma` and the control is dead at its own maximum.
	//
	// **ASSERTED AS THE PROPERTY RATHER THAN AS 16777215**, so this fails if anyone raises the
	// constant AND it fails for the actual reason — a comparison against a literal would only say
	// the number changed, not that the control stopped working.
	// **THE THREE COORDINATES THEMSELVES, WHERE THIS NAMED THEIR PHASE HALVES (BJ piece 3).** The
	// halves collapsed into them, and the property under test moved with the reach: it is the
	// COLLAPSED control that now runs to +-2^24 and must not wall at zero.
	for (Param p : { Param::Gamma, Param::Type, Param::Flavor, Param::Mod }) {
		CAPTURE(static_cast<int>(p));
		const feathers::device::ParamDesc& d = kParamDescs[static_cast<size_t>(p)];
		// **SYMMETRIC, AND IT ASSERTED `d.min == 0.0` UNTIL `test_device_ranges.cpp` FOUND THE WALL.**
		// All four default to 0, so a floor at 0 walls the first BACKWARDS lap at the place every
		// patch starts — `ZoneCoordBox::onCarry` clamps each carried lap into `min..max` and simply
		// stops carrying. The old assertion was pinning the defect: it read like a statement about the
		// reach and was a statement about one end of it.
		//
		// THE OTHER DIRECTION IS ALREADY PROVEN NEXT DOOR. *"a negative phase is a point on the line,
		// finite and not floored to zero"* asserts the derive at -2.66 differs from the derive at 0
		// and that `wrapPhase(-0.25) == 0.75` at the primitive — so the engine wanted this range all
		// along and only the declaration disagreed.
		CHECK(d.min == -d.max);
		// **THE PROPERTY IS THAT THE DEFAULT IS NOT AT A WALL, and it was spelled `def == 0` only
		// because all four defaults happened to be zero (BJ piece 3 ended that: the collapsed
		// coordinates default to their measured 0.30 / 0.70 / 0.10, gamma still to 0).**
		//
		// Zero was never the thing being asserted. The defect this line was written for is a floor at
		// the place every patch STARTS — the first backwards lap hits it and the carry stops — so what
		// has to be true is that the default sits strictly inside the range, whatever the default is.
		// Asserting the number would now fail for three controls that are perfectly well declared, and
		// would have gone on passing for a badly declared one whose default happened to be zero.
		CHECK(d.def > d.min);
		CHECK(d.def < d.max);
		// **THE PHASES ARM NOW, AND GAMMA STILL DOES NOT.** DECIDED, consistent with the firmware's
		// conventions — the coarse of a pair is the phase OFFSET and
		// never arms, the FINE is the phase and does. This case used to assert the opposite for all
		// four, on the reasoning that a wide range is free when nothing routes to it.
		//
		// **THE WIDE RANGE IS STILL FREE, AND THE REASON MOVED RATHER THAN VANISHED.** A route's reach
		// is `ParamDesc::modSpan` — one coarse unit, 0.125 — not the declared range, so the 2^24 is a
		// drag window rather than a modulation domain and there is no automation resolution being
		// spent on it. See `device::ParamDesc::modSpan` for why `zones` could not say this.
		if (p == Param::Gamma) {
			// GAMMA NEVER ARMS: unbounded, and a ValueBox everywhere by design.
			CHECK_FALSE(d.modulatable);
		}
		else {
			CHECK(d.modulatable);
			CHECK(d.modSpan == doctest::Approx(kOneZone));
		}

		// AT THE DECLARED MAXIMUM, one lap must still move the stored value...
		const float top = static_cast<float>(d.max);
		CHECK(top + 1.0f != top);
		// ...and must move it by ONE lap rather than two, which is the ragged regime past 2^24.
		CHECK((top + 1.0f) - top == doctest::Approx(1.0f));

		// AND THE CAP MUST SIT **AT** THE BOUNDARY, NOT SOMEWHERE COMFORTABLY BELOW IT — one lap
		// further on, a lap must stop moving. That is what makes this the LARGEST working value
		// rather than merely a working one.
		//
		// **WITHOUT THIS THE CASE DID NOT PROTECT THE REACH AT ALL**, which a known-bad found:
		// putting the cap back to 1024 — the exact regression in which gamma still stopped at 1024 — left
		// every assertion above green, because a lap moves perfectly well at 1024.
		// The mechanism was pinned and the REQUIREMENT was not.
		const float justPast = top + 1.0f;
		CHECK(justPast + 1.0f == justPast);

		// ...AND IT MUST STILL CHANGE THE SOUND. The storage moving is necessary and not sufficient:
		// what matters is that the field reads differently there, which is the whole point of being
		// able to drive gamma that far.
		AutomodCoords lo;
		lo.lfoHz = 1.0f;
		lo.gammaPhase = top;
		AutomodCoords hi = lo;
		hi.gammaPhase = top + 1.0f;
		CHECK_FALSE(deriveAutomod(lo) == deriveAutomod(hi));

		// **AND THE WHOLE PARAGRAPH ABOVE, ASKED AT THE BOTTOM.** Float ULP is symmetric about zero,
		// so every step of that derivation holds at `-kPhaseMax` — but "it must" and "it does" are
		// different claims, and the mirroring is new. A lap still moves at the declared minimum, moves
		// by one rather than two, stops moving one lap further out, and changes the field.
		const float bottom = static_cast<float>(d.min);
		CHECK(bottom - 1.0f != bottom);
		CHECK(bottom - (bottom - 1.0f) == doctest::Approx(1.0f));
		const float justBelow = bottom - 1.0f;
		CHECK(justBelow - 1.0f == justBelow);

		AutomodCoords deep;
		deep.lfoHz = 1.0f;
		deep.gammaPhase = bottom;
		AutomodCoords deeper = deep;
		deeper.gammaPhase = bottom + 1.0f;
		CHECK_FALSE(deriveAutomod(deep) == deriveAutomod(deeper));
	}
}

TEST_CASE("automod: the DECLARED DEFAULTS sweep audibly and sit near unity") {
	// THE DEFECT: some settings, including the default/init setting, made Automod completely quiet.
	// The level half was two porting bugs. THE OTHER HALF SURVIVED THE FIX: at (0, 0, 0) the LFO's
	// waveform collapses to a flat line, so the device was a STATIC filter at its own default —
	// measured at **0.000 octaves of cutoff travel**. A wah that does not sweep reads as broken
	// however loud it is.
	//
	// THE DEFAULTS ARE NOW CHOSEN BY MEASUREMENT and this case is what holds them there. It asserts
	// the three properties the choice was made on, NOT the coordinates — so a bank retune that moves
	// the field is free to move the numbers, and fails only if the defaults stop being usable.
	Settings s;                       // exactly the declared defaults, nothing overridden...
	s.enabled = true;                 // ...except the bypass, which is off by convention
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);

	float lowest = 1e9f;
	float highest = -1e9f;
	double outSq = 0.0;
	double inSq = 0.0;
	long counted = 0;
	for (int b = 0; b < 400; ++b) {
		std::vector<float> l(512);
		std::vector<float> r(512);
		std::vector<float> dry(512);
		for (int i = 0; i < 512; ++i) {
			float x = 0.0f;
			for (int k = 1; k <= 60; ++k) {
				x += std::sin(2.0f * 3.14159265f * 110.0f * static_cast<float>(k)
				              * static_cast<float>(b * 512 + i) / static_cast<float>(kSr))
				     / std::sqrt(static_cast<float>(k));
			}
			dry[i] = 0.06f * x;
			l[i] = dry[i];
			r[i] = dry[i];
		}
		a.processStereo(l.data(), r.data(), 512);
		if (b > 20) { // past the springs' settling
			lowest = std::min(lowest, a.filterCutoffForTest());
			highest = std::max(highest, a.filterCutoffForTest());
			for (int i = 0; i < 512; ++i) {
				outSq += static_cast<double>(l[i]) * l[i];
				inSq += static_cast<double>(dry[i]) * dry[i];
			}
			counted += 512;
		}
	}

	// 1. IT SWEEPS. Measured at 1.97 octaves; the bar is well under that so a retune has room, and
	//    well over zero so the static default this replaced cannot come back.
	const float octaves = std::log2(highest / std::max(lowest, 1.0f));
	CAPTURE(octaves);
	CHECK(octaves > 0.8f);
	CHECK(octaves < 3.0f); // and it is a wah, not a siren

	// 2. IT SITS NEAR UNITY. Measured at -3.4 dB.
	const double db = 20.0 * std::log10(std::sqrt(outSq / static_cast<double>(counted))
	                                    / std::sqrt(inSq / static_cast<double>(counted)));
	CAPTURE(db);
	CHECK(db > -6.0);
	CHECK(db < 1.0);

	// 3. EVERY ROAD IS LIVE, which is what stops the default sitting in a deadzone the way the test
	//    fixture once did. Read off the derive rather than inferred from the sound.
	const AutomodDerived& d = a.derived();
	CHECK(d.filterModDepth > 0.1f);
	CHECK(std::fabs(d.envDepthInfluence) > 0.1f);
	CHECK(d.comb.mix > 0.1f);
	CHECK(d.comb.modRangeSamples > 20.0f);
}

TEST_CASE("automod: a synced division locks the LFO's cycle to the host's tempo") {
	// THE VALUE, COMPUTED FROM THE TEMPO rather than compared against a render. A division is a CYCLE
	// LENGTH, so the rate is its reciprocal: at 120 bpm a beat is half a second, so a 1/4 is 2 Hz and
	// a 1/8 is 4 Hz. `effectiveRateHz()` is the real road `refresh()` takes, not a restatement of it.
	Automod a;
	a.prepare(kSr);
	a.setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(0.5)); // 120 bpm

	Settings s;
	s.enabled = true;
	// The quarter note is index 12 in the division table, so choice 13 — asserted through the
	// conversion rather than hardcoded, so a table reorder moves both together.
	int quarter = -1;
	int eighth = -1;
	for (size_t i = 0; i < feathers::device::kDivisionCount; ++i) {
		if (std::string_view(feathers::device::kDivisionNames[i]) == "1/4") {
			quarter = static_cast<int>(i + kFreeRateCount);
		}
		if (std::string_view(feathers::device::kDivisionNames[i]) == "1/8") {
			eighth = static_cast<int>(i + kFreeRateCount);
		}
	}
	REQUIRE(quarter > 0);
	REQUIRE(eighth > 0);

	s.syncDivision = quarter;
	a.setTarget(s);
	CHECK(a.effectiveRateHz() == doctest::Approx(2.0f).epsilon(1e-4));

	s.syncDivision = eighth;
	a.setTarget(s);
	CHECK(a.effectiveRateHz() == doctest::Approx(4.0f).epsilon(1e-4));

	// AND THE TEMPO IS READ, not latched: halving it halves the rate.
	a.setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(1.0)); // 60 bpm
	CHECK(a.effectiveRateHz() == doctest::Approx(2.0f).epsilon(1e-4));

	SUBCASE("and RATE SCALES A SYNCED DIVISION rather than being ignored by it") {
		// **THE CHANGE THIS CASE EXISTS FOR.** Rate used to be consulted in NO case under sync, which
		// is how a default that made sync "actually in sync" could be asked of a control sync
		// did not read. It is now a multiplier on the selected entry, everywhere.
		s.syncDivision = eighth;
		a.setTransportClock(feathers::device::TransportClock::fromSecondsPerBeat(0.5)); // 120 bpm
		s.rate = 0.0f; // octaves: 1X
		a.setTarget(s);
		const float unity = a.effectiveRateHz();
		CHECK(unity == doctest::Approx(4.0f).epsilon(1e-4));

		// HALF SPEED AND DOUBLE SPEED, reciprocal about unity.
		s.rate = -1.0f; // an octave down
		a.setTarget(s);
		const float slow = a.effectiveRateHz();
		s.rate = 1.0f; // an octave up
		a.setTarget(s);
		const float fast = a.effectiveRateHz();
		CHECK(unity / slow == doctest::Approx(fast / unity).epsilon(1e-4));
		CHECK(slow < unity);
		CHECK(unity < fast);
	}

	SUBCASE("and a FREE entry is a rate the list names, scaled by the same knob") {
		// The free entries are the dropdown's first two, so the knob means the same thing whether the
		// list selected a division or a frequency — which is the whole point of moving free rates
		// into the list.
		s.syncDivision = 1; // "1 Hz"
		s.rate = 0.0f; // octaves: 1X
		a.setTarget(s);
		CHECK(a.effectiveRateHz() == doctest::Approx(1.0f).epsilon(1e-4));

		s.syncDivision = 0; // "0.1 Hz"
		a.setTarget(s);
		CHECK(a.effectiveRateHz() == doctest::Approx(0.1f).epsilon(1e-4));

		// AND THE MULTIPLIER APPLIES HERE TOO.
		s.rate = 2.0f;
		a.setTarget(s);
		CHECK(a.effectiveRateHz() == doctest::Approx(0.4f).epsilon(1e-4));
	}

	SUBCASE("and a synced division with NO tempo falls back to a free rate, not to silence") {
		// THE HONEST DEGRADATION. A standalone build and a host reporting no timeline are real
		// states; a device that stopped moving in them would be broken rather than principled. The
		// control still reads "1/8" because that is what it will do the moment a tempo arrives.
		s.syncDivision = eighth;
		s.rate = 0.0f; // octaves: 1X
		a.setTarget(s);
		a.setTransportClock(feathers::device::TransportClock::unknown());
		CHECK(a.effectiveRateHz() == doctest::Approx(nominalRateHz(s)).epsilon(1e-4));
		// NON-VACUITY: the fallback is not accidentally equal to the synced answer.
		CHECK(nominalRateHz(s) != doctest::Approx(4.0f));
	}
}

TEST_CASE("automod: a row round-trips through settings, and a short row takes declared defaults") {
	Settings s = liveSettings();
	s.gamma = 3.5f;
	s.manual = -0.25f;
	s.lfoMode = LfoModeChoice::Once;
	CHECK(settingsOf(rowOf(s)) == s);

	// AN OUT-OF-RANGE MODE FROM A FILE IS CLAMPED, not cast through. A patch is data and the guard
	// belongs on the road the data takes — the playbook's 8a.
	Row bad = rowOf(s);
	bad[static_cast<size_t>(Param::LfoMode)] = 99.0f;
	CHECK(settingsOf(bad).lfoMode == LfoModeChoice::Stop);
	bad[static_cast<size_t>(Param::LfoMode)] = -5.0f;
	CHECK(settingsOf(bad).lfoMode == LfoModeChoice::Free);
}

TEST_CASE("automod: Freq moves the filter and the comb in OPPOSITE directions, by two octaves") {
	// **THE CONTROL THE PORT MISSED: the Freq macro.** It is the firmware's
	// `GLOBAL_AUTOMOD_FREQ`, held outside `AutomodulatorParams` so the mod matrix could reach it —
	// which is exactly why a port reading that struct could not see it. `automod_types.h` carries the
	// diagnosis; this pins the behaviour.
	//
	// ASSERTED ON THE VALUES, not on a rendered buffer: the corner and the delay are both readable
	// through seams that exist for this reason, and neither is recoverable from the output.
	const auto measure = [](float freq, float semis) {
		Automod fx;
		fx.prepare(48000.0);
		Settings s = liveSettings();
		s.freq = freq;
		// STOPPED, so the LFO contributes nothing and the only thing moving is Freq. **TRUE ONLY SINCE
		// BACKLOG IS**: before it, `refresh()` snapped the springs to the RAW LFO tap even in Stop, so
		// the corner and the delay both carried a constant offset this case had to design around.
		s.lfoMode = LfoModeChoice::Stop;
		s.manual = 0.0f;
		fx.setTarget(s);
		feathers::device::NoteContext note;
		note.pitchSemitones = semis;
		fx.setNoteContext(note);
		std::vector<float> l(256, 0.0f);
		std::vector<float> r(256, 0.0f);
		fx.processStereo(l.data(), r.data(), 256);
		return std::pair { fx.filterCutoffForTest(), fx.combDelayForTest() };
	};

	// **THE FILTER IS MEASURED AN OCTAVE DOWN**, so two octaves up from its corner stays under Nyquist:
	// at note 60 the corner sits at ~5.65 kHz and four times that is past the 21.6 kHz ceiling, so the
	// ratio would be measuring the clamp (3.82 against 4). The old snap's offset had hidden that by
	// pulling every corner down.
	const float midHz = measure(0.0f, 48.0f).first;
	const float upHz = measure(1.0f, 48.0f).first;
	const float downHz = measure(-1.0f, 48.0f).first;
	const float midDelay = measure(0.0f, 60.0f).second;
	const float upDelay = measure(1.0f, 60.0f).second;
	const float downDelay = measure(-1.0f, 60.0f).second;

	// OPPOSITE DIRECTIONS — brighter corner, shorter delay. The reason is the one note tracking is
	// written under: a comb's delay is the reciprocal of its pitch.
	CHECK(upHz > midHz);
	CHECK(upDelay < midDelay);
	CHECK(downHz < midHz);
	CHECK(downDelay > midDelay);

	// TWO OCTAVES EITHER WAY, AS A RATIO — which is what the control is. Both are purely multiplicative
	// now that Stop means no modulation at all, so both ratios are read directly.
	CHECK(upHz / midHz == doctest::Approx(std::exp2(kFreqOctaves)).epsilon(1e-4));
	CHECK(midHz / downHz == doctest::Approx(std::exp2(kFreqOctaves)).epsilon(1e-4));
	CHECK(midDelay / upDelay == doctest::Approx(std::exp2(kFreqOctaves)).epsilon(1e-3));
	CHECK(downDelay / midDelay == doctest::Approx(std::exp2(kFreqOctaves)).epsilon(1e-3));

	// AND NEITHER END IS AGAINST A BOUND, or the ratios above would be measuring the clamp.
	CHECK(upHz < 48000.0f * kFilterNyquistFraction);
	CHECK(upDelay > kCombMinDelaySamples);
	CHECK(downDelay < kCombMaxDelaySamples);

	// AND ZERO IS NEUTRAL, which is what makes it safe as a default: a device that moved its own
	// spectrum on load would be one whose coordinates did not mean what they say.
	Automod plain;
	plain.prepare(48000.0);
	Settings noFreq = liveSettings();
	noFreq.lfoMode = LfoModeChoice::Stop;
	noFreq.manual = 0.0f;
	plain.setTarget(noFreq);
	std::vector<float> l(256, 0.0f);
	std::vector<float> r(256, 0.0f);
	plain.processStereo(l.data(), r.data(), 256);
	CHECK(plain.filterCutoffForTest() == doctest::Approx(measure(0.0f, 60.0f).first));
	CHECK(plain.combDelayForTest() == doctest::Approx(midDelay));
}

TEST_CASE("automod: the phase decode inverts, so a pad can draw its own puck") {
	// **THIS CASE EXISTS BECAUSE THE FIRST DECODE DID NOT INVERT AND ONLY A ROUND-TRIP SAID SO.**
	//
	// The shape is derived from `deriveAutomod`: it folds each coordinate as
	// `coord + coordPhase + gammaPhase`, so gamma is the COMMON MODE and the three offsets are what
	// is left — and the part of three numbers a common term cannot express is exactly
	// two-dimensional. That is why a pad is the right widget and why it needs no third axis.
	//
	// THE FIRST FORM WAS `max(0, cos)`, borrowed from `shaper_curve.h`'s six half-rectified bases. It
	// gives non-negative lobes and a zero centre, both of which a lapping phase counter needs, and it
	// is NOT INVERTIBLE: three axes 120 degrees apart each cover 180, so within 30 degrees of an axis
	// exactly ONE lobe is non-zero — and one number cannot carry a radius and an angle at once. The
	// pad could not have drawn its own puck over a third of its face. Worst round-trip error measured
	// at 0.99 of the pad's radius, before any widget existed.
	//
	// `(1 + cos) / 2` INVERTS BY CONSTRUCTION because the three lobes SUM TO A CONSTANT — the cosines
	// at 120 degrees cancel — so the sum names the radius and the differences name the angle. **If
	// somebody ever makes these half-rectified again because it looks more idiomatic for this
	// codebase, this case is what says no.**
	namespace amd = feathers::fx::automod;

	SUBCASE("every puck position and rotation round-trips") {
		float worst = 0.0f;
		int sampled = 0;
		for (int ri = 0; ri < 7; ++ri) {
			const float rotation = static_cast<float>(ri) * 0.7f;
			for (int deg = 0; deg < 360; deg += 7) {
				for (int rr = 1; rr <= 10; ++rr) {
					const float angle = static_cast<float>(deg) * 3.14159265f / 180.0f;
					const float radius = static_cast<float>(rr) / 10.0f;
					const float x = radius * std::cos(angle);
					const float y = radius * std::sin(angle);
					const amd::PhaseOffsets o = amd::phaseOffsetsFor(x, y, rotation);
					const auto [bx, by] = amd::puckForOffsets(o, rotation);
					worst = std::max(worst, std::hypot(bx - x, by - y));
					++sampled;
				}
			}
		}
		CAPTURE(worst);
		// NON-VACUITY: the sweep must actually have run, or "worst stayed small" is a statement about
		// an empty loop.
		REQUIRE(sampled == 3640);
		CHECK(worst < 1e-4f);
	}

	SUBCASE("the centre is exactly no offset, in both directions") {
		// A DECODE WHOSE CENTRE WAS NOT ZERO WOULD MAKE "no phase offset" UNREACHABLE FROM THE PAD
		// while remaining reachable from the row — a surface that cannot express a state its own
		// device has.
		const amd::PhaseOffsets centre = amd::phaseOffsetsFor(0.0f, 0.0f, 0.0f);
		CHECK(centre.type == doctest::Approx(0.0f));
		CHECK(centre.flavour == doctest::Approx(0.0f));
		CHECK(centre.mod == doctest::Approx(0.0f));
		const auto [px, py] = amd::puckForOffsets({}, 0.0f);
		CHECK(px == doctest::Approx(0.0f));
		CHECK(py == doctest::Approx(0.0f));
	}

	SUBCASE("the three lobes sum to a constant, which is what makes the inverse exact") {
		// THE PROPERTY THE WHOLE DESIGN RESTS ON, asserted directly rather than inferred from the
		// round-trip passing. If this holds and the round-trip fails, the bug is in the inverse; if
		// this fails, the bug is in the decode. Two cases that can point at different halves.
		for (int deg = 0; deg < 360; deg += 11) {
			const float angle = static_cast<float>(deg) * 3.14159265f / 180.0f;
			const amd::PhaseOffsets o = amd::phaseOffsetsFor(std::cos(angle), std::sin(angle), 0.0f);
			CAPTURE(deg);
			CHECK(o.type + o.flavour + o.mod == doctest::Approx(1.5f * amd::kPadPhaseSpan));
		}
	}

	SUBCASE("no lobe is ever negative, because a phase is a counter floored at zero") {
		for (int i = 0; i < 2000; ++i) {
			const float a = static_cast<float>(i) * 0.0031415f;
			const amd::PhaseOffsets o = amd::phaseOffsetsFor(std::cos(a), std::sin(a), a * 0.7f);
			CAPTURE(i);
			REQUIRE(o.type >= 0.0f);
			REQUIRE(o.flavour >= 0.0f);
			REQUIRE(o.mod >= 0.0f);
		}
	}
}

TEST_CASE("automod: Rotate turns the phase triple about its mean, and is the IDENTITY at zero") {
	using namespace feathers::fx::automod;

	// **THE IDENTITY SUBCASE IS THE ONE THAT EXISTS FOR A REASON.** The first implementation of this
	// rotation was `phaseOffsetsFor(puckForOffsets(o), theta)` — a composition of two functions this
	// file already pins as exact inverses. They ARE exact inverses, on the pad's own manifold. The
	// card's three ZoneCoordBox widgets do not write on that manifold: (8, 0, 0) is dialable there,
	// and the composition rewrote it to roughly (5.33, 1.33, 1.33) with the knob reading ZERO.
	//
	// A difference check would have passed that. "Rotation changes the phases" was true, and true for
	// the wrong reason, which is this repo's most-repeated bug shape. So the assertion here is on the
	// VALUE, at the one setting where the value is known independently of any of this code: the input.
	SUBCASE("zero rotation returns the input exactly, on the pad's manifold and off it") {
		const PhaseOffsets cases[] = {
			{ 8.0f, 0.0f, 0.0f },   // a corner of the legal range, far off the pad's manifold
			{ 0.0f, 3.0f, -2.0f },  // negative, which the pad never produces and automation can
			{ 1.0f, 1.0f, 1.0f },   // pure mean, no component to turn
			{ 0.0f, 0.0f, 0.0f },
			phaseOffsetsFor(0.6f, -0.3f, 0.0f), // and one that IS on the manifold
		};
		for (const auto& in : cases) {
			const PhaseOffsets out = rotatePhases(in, 0.0f);
			CHECK(out.type == doctest::Approx(in.type).epsilon(1e-6));
			CHECK(out.flavour == doctest::Approx(in.flavour).epsilon(1e-6));
			CHECK(out.mod == doctest::Approx(in.mod).epsilon(1e-6));
		}
	}

	SUBCASE("a full turn is the identity, and the mean is preserved at every angle") {
		const PhaseOffsets in { 5.0f, 1.0f, -2.0f };
		const float mean = (in.type + in.flavour + in.mod) / 3.0f;
		for (int step = 0; step <= 24; ++step) {
			const float theta = static_cast<float>(step) * 6.2831853f / 24.0f;
			const PhaseOffsets out = rotatePhases(in, theta);
			CHECK((out.type + out.flavour + out.mod) / 3.0f == doctest::Approx(mean).epsilon(1e-5));
		}
		const PhaseOffsets full = rotatePhases(in, 6.2831853f);
		CHECK(full.type == doctest::Approx(in.type).epsilon(1e-4));
		CHECK(full.flavour == doctest::Approx(in.flavour).epsilon(1e-4));
		CHECK(full.mod == doctest::Approx(in.mod).epsilon(1e-4));
	}

	// A third of a turn is a CYCLIC PERMUTATION of the three lobes — that is what "rotate the basis"
	// has to mean when the basis is three axes 120 degrees apart, and it is checkable without
	// reference to the implementation's trigonometry.
	SUBCASE("a third of a turn permutes the lobes") {
		const PhaseOffsets in { 5.0f, 1.0f, -2.0f };
		const PhaseOffsets out = rotatePhases(in, 2.0943951f);
		CHECK(out.type == doctest::Approx(in.mod).epsilon(1e-4));
		CHECK(out.flavour == doctest::Approx(in.type).epsilon(1e-4));
		CHECK(out.mod == doctest::Approx(in.flavour).epsilon(1e-4));
	}

	// A SUBCASE USED TO STAND HERE asserting that Rotate "reaches coordsOf, so the knob alone moves
	// the phases". That was the defect heard in use, and its inverse is now pinned as a value by the
	// case below this one — the standing phases are bit-identical across a Rotate change, and the
	// next DECODE is what lands somewhere else.
}

// **WHAT WAS HEARD, NAMED AS A VALUE.** In the tested build Rotate changed the sound with the other
// controls stationary, which it must not. THE CONTRACT IS FEATHERS' OWN, and it is a decision rather
// than a taste: `src/dsp/slice_basis.h` states it — a rotation that changed the sound would be
// redundant with gamma, so the sound stays the same unless you move in x/y/gamma — and
// `src/dsp/character_bank.h`'s banked residue (2026-08-13) extends it to a sound that has already
// travelled: re-aiming its plane leaves it standing where it is. A rotate is where SUBSEQUENT travel
// goes, never a change to the sound that is standing there.
//
// Automod's Rotate is the same word and must keep the same contract. So this case asserts the
// INVARIANT: with the three phases and gamma held, the coordinates the device derives from — and
// the derive itself — are identical at rotate 0 and rotate X, at the pad's centre AND off it.
//
// THE OFF-CENTRE SUBCASE WAS RED WHEN THIS CASE WAS WRITTEN (2026-09-01), and that red was the
// report: `coordsOf` re-aimed the standing triple through `rotatePhases` on the way into the derive,
// which is exactly the "redundant with gamma" spelling the contract forbids. The rotation now lives
// only in the pad's decode — see `coordsOf` for why this device needs no banked residue where
// Explore does — and the known-bad is the one-line restoration of that call, which reddens the
// off-centre subcase and nothing else.
//
// NOT THE SUM. `rotatePhases` turns the triple about its own mean, so the SUM of the three phases is
// preserved by construction (8.0498 both ways in the measurement below) while the sound changes.
// A sum assertion would be green on the defect; the three phases and the derive are what must hold.
TEST_CASE("automod: Rotate alone leaves the derived phases standing, at the pad's centre and off it") {
	using namespace feathers::fx::automod;
	constexpr float kTheta = 1.0f; // an awkward angle — not a third, not a half, not a whole turn

	// THE COORDINATES TWO SETTINGS DERIVE FROM, differing in Rotate alone.
	const auto pair = [&](const PhaseOffsets& phases, float gamma) {
		Settings s {};
		s.type = phases.type;
		s.flavor = phases.flavour;
		s.mod = phases.mod;
		s.gamma = gamma;
		s.rotate = 0.0f;
		const AutomodCoords flat = coordsOf(s, nominalRateHz(s));
		s.rotate = kTheta;
		const AutomodCoords turned = coordsOf(s, nominalRateHz(s));
		return std::pair<AutomodCoords, AutomodCoords> { flat, turned };
	};
	// EXACT, not Approx: a standing sound must come back bit-identical, because the derive downstream
	// compares bits and the honesty sweep compares rendered samples.
	const auto standing = [](const AutomodCoords& a, const AutomodCoords& b) {
		CAPTURE(a.type);
		CAPTURE(a.flavor);
		CAPTURE(a.mod);
		CAPTURE(b.type);
		CAPTURE(b.flavor);
		CAPTURE(b.mod);
		CHECK(b.type == a.type);
		CHECK(b.flavor == a.flavor);
		CHECK(b.mod == a.mod);
		CHECK(b.type == a.type);
		CHECK(b.flavor == a.flavor);
		CHECK(b.mod == a.mod);
		CHECK(b.gammaPhase == a.gammaPhase);
		CHECK(deriveAutomod(a) == deriveAutomod(b));
	};

	SUBCASE("at the centre, with gamma seated, Rotate changes nothing") {
		const auto [flat, turned] = pair({ 0.0f, 0.0f, 0.0f }, 0.4f);
		standing(flat, turned);
		CHECK(turned.gammaPhase == 0.4f); // gamma rides beside the rotation, not through it
	}

	SUBCASE("a pure-mean triple from the coord boxes stands too") {
		const auto [flat, turned] = pair({ 1.0f, 1.0f, 1.0f }, 0.0f);
		standing(flat, turned);
		CHECK(turned.type == 1.0f);
	}

	// **THE ONE THAT WAS RED, AND THE NUMBERS IT PRINTED WERE THE REPORT.** MEASURED 2026-09-01 on
	// the defect, rotate 0 -> 1.0 rad with the puck at (0.6, -0.3) and nothing else touched:
	//   phases (5.0833, 0.4441, 2.5225) -> (4.9898, 2.7175, 0.3426); sum 8.0498 both times;
	//   deriveAutomod differed. The turned triple was exactly `phaseOffsetsFor(0.6, -0.3, 1.0)` —
	//   the pad's decode at the re-aimed angle — handed to the derive as if the hand had dragged.
	// The REQUIRE is what proves the equality is not vacuous: the puck IS off the centre, so a
	// rotation that reached the standing sound would have something to move.
	SUBCASE("off the centre, with the puck held still, Rotate alone leaves the sound standing") {
		constexpr float kX = 0.6f;
		constexpr float kY = -0.3f;
		const PhaseOffsets held = phaseOffsetsFor(kX, kY, 0.0f);
		REQUIRE(std::fabs(held.type - held.flavour) > 1.0f); // off the centre, by a wide margin
		const auto [flat, turned] = pair(held, 0.0f);
		standing(flat, turned);
	}

	// A ROTATION THAT KEEPS THE SOUND STANDING STILL HAS TO GO SOMEWHERE, and where it goes is the
	// pad's decode: the same puck position, under a different Rotate, names a different triple for
	// the NEXT drag. This is the half of the contract that must remain true after any fix — it is
	// what stops the fix from making Rotate a dead control — and it is asserted through the decode's
	// own rotation argument rather than through `coordsOf`.
	SUBCASE("the same puck position under a different Rotate names a different triple for the next drag") {
		const PhaseOffsets standard = phaseOffsetsFor(0.6f, -0.3f, 0.0f);
		const PhaseOffsets aimed = phaseOffsetsFor(0.6f, -0.3f, kTheta);
		CHECK(std::fabs(aimed.flavour - standard.flavour) > 1.0f);
		CHECK((aimed.type + aimed.flavour + aimed.mod)
		      == doctest::Approx(standard.type + standard.flavour + standard.mod).epsilon(1e-5));
		// AND THE TWO DECODES ARE DIFFERENT SOUNDS ONCE WRITTEN, so a fix that made Rotate do
		// nothing at all — the visible-and-dead control the honesty rule complains about — fails here
		// rather than passing on the standing-sound half alone.
		Settings s {};
		s.type = standard.type;
		s.flavor = standard.flavour;
		s.mod = standard.mod;
		const AutomodDerived wasStandard = deriveAutomod(coordsOf(s, nominalRateHz(s)));
		s.type = aimed.type;
		s.flavor = aimed.flavour;
		s.mod = aimed.mod;
		CHECK_FALSE(deriveAutomod(coordsOf(s, nominalRateHz(s))) == wasStandard);
	}

	// THE PARAM ROW CARRIES THE ROTATION IN ITS OWN FIELD AND THE PHASES UNTURNED. That is the
	// engine's half of "turning Rotate does not write the phase params": a round trip through the
	// row leaves Type, Flavour and Mod exactly as they were dialled, whatever Rotate
	// says. The UI's half — that the pad does not write them either — is in test_gui_widgets.cpp.
	SUBCASE("the row keeps the phases unturned and the rotation in its own field") {
		Settings s {};
		const PhaseOffsets dialled = phaseOffsetsFor(0.6f, -0.3f, 0.0f);
		s.type = dialled.type;
		s.flavor = dialled.flavour;
		s.mod = dialled.mod;
		s.rotate = kTheta;
		const Settings back = settingsOf(rowOf(s));
		CHECK(back.type == dialled.type);
		CHECK(back.flavor == dialled.flavour);
		CHECK(back.mod == dialled.mod);
		CHECK(back.rotate == kTheta);
	}
}

// **WHAT THE DERIVE DOES WITH A NEGATIVE PHASE**, pinned as a value because the question came up
// while measuring Rotate: the coord boxes can dial (8, 0, 0), automation can write below zero, and
// the pad's own decode never produces a negative — so nothing had said what one MEANS downstream.
//
// THE CONTRACT, READ OFF THE CHARACTER LAYER RATHER THAN ASSERTED ABOUT IT: every field is a bank of
// phi triangles, each evaluated at `wrapPhase((phase + offset) * phiFreq)` — a floor-wrap to [0, 1).
// So a negative phase is a legal point on the same unbounded line the positive ones sit on, wrapped
// per triangle by that triangle's own period, and NOT floored to zero. There is no single lap: the
// phi frequencies are incommensurate, so "the same phase modulo the lap" is not a statement the
// derive can make. What it can promise is finite, distinct from the floor, and floor-wrapped — and
// those three are what this pins.
TEST_CASE("automod: a negative phase is a point on the line, finite and not floored to zero") {
	using namespace feathers::fx::automod;
	const auto at = [](float typePhase) {
		Settings s {};
		s.type = typePhase;
		return deriveAutomod(coordsOf(s, nominalRateHz(s)));
	};
	const AutomodDerived below = at(-2.66f); // the value `rotatePhases({8,0,0}, 1.0)` produced
	const AutomodDerived floor = at(0.0f);

	// FINITE, in every float the derive publishes — walked as bytes so a field added later is
	// covered without this case learning its name.
	static_assert(std::is_trivially_copyable_v<AutomodDerived>);
	for (size_t i = 0; i + sizeof(float) <= sizeof(AutomodDerived); i += sizeof(float)) {
		float v = 0.0f;
		std::memcpy(&v, reinterpret_cast<const unsigned char*>(&below) + i, sizeof v);
		CAPTURE(i);
		CHECK(std::isfinite(v));
	}
	// NOT FLOORED: a derive that clamped the phase at zero would answer with the floor's own reading.
	CHECK_FALSE(below == floor);
	// AND FLOOR-WRAPPED, stated at the primitive that decides it rather than inferred from a render:
	// -0.25 of a triangle's period is the same place as +0.75 of it.
	CHECK(owlet::dsp::phi::wrapPhase(-0.25) == doctest::Approx(0.75));
	CHECK(owlet::dsp::phi::wrapPhase(-2.25) == doctest::Approx(0.75));
}


TEST_CASE("automod's phase space: 39 coordinates, home bit for bit, and a plane that opens the rest") {
	using namespace feathers::fx::automod;
	namespace ps = feathers::fx::automod::phase_space;

	// **DELIBERATELY NOT DYADIC, AND A KNOWN-BAD IS WHY.** The first version of this case used 2.75,
	// 0.125, 0.375 and so on — every one exactly representable in float. A mutation narrowing the
	// accumulation to float PASSED it: the values survived the round trip, so a bit-for-bit claim had
	// nothing to bite on. Values whose double form is not a float makes the exactness assertion mean
	// what it says.
	//
	// A test whose inputs cannot express the failure is a test that reports on nothing, and no amount
	// of `==` in the assertion fixes it — the same shape as the venue rules: a measurement must be
	// shown capable of failing before its success counts.
	//
	// **ONE VALUE PER AXIS SINCE THE COLLAPSE (BJ piece 3), where this set a coordinate AND its phase.**
	// The two were summed by the derive; there is one param now, so the second assignment is gone
	// rather than folded in — folding it would have made the stored coordinate `float(2.7183f +
	// 0.1234f)`, a float sum, which is precisely the rounding this case exists to detect and would
	// have quietly disarmed it.
	AutomodCoords in {};
	in.type = 2.7183f;
	in.flavor = 1.4142f;
	in.mod = 0.3333f;
	in.gammaPhase = 0.3691f;
	in.lfoHz = 1.0f;

	// THE THREE SUMS THE DERIVE COMPUTES, spelled here so the case does not depend on a helper that
	// takes the coords struct — see `homeFor` on why it takes phases instead.
	const auto sum3 = [](const AutomodCoords& c) {
		const auto f = [](float v) { return std::isfinite(v) ? static_cast<double>(v) : 0.0; };
		return std::array<double, 3> { f(c.type) + f(c.gammaPhase),
		                               f(c.flavor) + f(c.gammaPhase),
		                               f(c.mod) + f(c.gammaPhase) };
	};
	const auto s0 = sum3(in);
	const ps::Phases home = ps::homeFor(s0[0], s0[1], s0[2]);
	const ps::Basis basis = ps::basisFor(0.0, ps::planeHomeOf(home));

	// **THE ASSERTION THE WHOLE DESIGN RESTS ON: AT ZERO TRAVEL, NOTHING MOVES — EXACTLY.**
	//
	// Not "approximately", and not "the sound is similar". `SliceBasis::phaseFor` multiplies both
	// basis rows by the travel, so at zero travel both products are exactly +/-0.0 and adding that to
	// a finite value is exact. That is the property that lets this land WITHOUT changing the device:
	// the plane can exist, be rotated to any orientation, and the sound is what it always was until a
	// hand moves the puck.
	//
	// **AND IT IS ASSERTED AT EVERY ORIENTATION**, because "whatever the basis" is the interesting
	// half of the claim. A rotation that moved a standing sound is exactly the bug I shipped in
	// `coordsOf` this week, arrived at from the other direction.
	SUBCASE("zero travel returns home bit for bit, at any orientation") {
		for (const float spin : { 0.0f, 0.3f, 1.0f, -2.5f, 3.14159f }) {
			CAPTURE(spin);
			ps::Basis turned = basis;
			// AN ARBITRARY REORIENTATION, not a realistic one — the guarantee must not depend on the
			// plane being sensible.
			for (size_t i = 0; i < ps::kTriangles; ++i) {
				turned.x[i] = std::cos(spin + static_cast<float>(i));
				turned.y[i] = std::sin(spin - static_cast<float>(i));
			}
			const ps::Phases at = ps::phasesAt(home, turned, 0.0, 0.0);
			for (size_t i = 0; i < ps::kTriangles; ++i) {
				CAPTURE(i);
				// == , not Approx. The claim is bit-for-bit and an epsilon would hide the bug.
				CHECK(at[i] == home[i]);
			}
		}
	}

	// HOME IS THE PRESENT BEHAVIOUR WRITTEN AS A VECTOR, which is what makes the migration safe: the
	// derive today evaluates every Type triangle at `type + gammaPhase`, and so does this.
	SUBCASE("home restates what the derive already computes, per group") {
		const auto f = [](float v) { return static_cast<double>(v); };
		const double type = f(2.7183f) + f(0.3691f);
		const double flavour = f(1.4142f) + f(0.3691f);
		const double mod = f(0.3333f) + f(0.3691f);
		// **ASKED THROUGH THE CONTROL AXES, which is now what home is made of.** BY DESIGN a control is a
		// direction, and the axis IS the identity matrix for its parameter set — so the expected value
		// at a triangle is whichever control's run contains it.
		const auto within = [](const ps::ControlAxis& a, size_t i) {
			return i >= a.first && i < a.first + a.count;
		};
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			CAPTURE(i);
			const double want = within(ps::kTypeAxis, i)      ? type
			                    : within(ps::kFlavourAxis, i) ? flavour
			                                                  : mod;
			CHECK(home[i] == doctest::Approx(want).epsilon(1e-12));
		}
		// AND THE THREE AXES PARTITION THE SPACE — no triangle in two runs, none in none. A layout
		// that overlapped would double a control's push and leave another silent, and the values above
		// would still look plausible.
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			CAPTURE(i);
			const int covered = static_cast<int>(within(ps::kTypeAxis, i))
			                    + static_cast<int>(within(ps::kFlavourAxis, i))
			                    + static_cast<int>(within(ps::kModAxis, i));
			CHECK(covered == 1);
		}
		// GAMMA'S AXIS IS THE WHOLE SPACE — the unit vector, rank 39 — "getting in an airplane".
		CHECK(ps::kGammaAxis.first == 0);
		CHECK(ps::kGammaAxis.count == ps::kTriangles);
	}

	// **GAMMA IS A SHIFT ALONG THE UNIT VECTOR**, which is the design's formulation and is checkable: moving
	// gamma alone must move every one of the 39 coordinates by the same amount.
	SUBCASE("gamma moves all 39 coordinates equally — the unit-vector shift") {
		AutomodCoords moved = in;
		moved.gammaPhase = in.gammaPhase + 0.25f;
		const auto s1 = sum3(moved);
		const ps::Phases after = ps::homeFor(s1[0], s1[1], s1[2]);
		// **THE EXPECTED SHIFT IS THE FLOAT DELTA, NOT THE LITERAL 0.25.** `gammaPhase` is a float, so
		// `0.3691f + 0.25f` minus `0.3691f` is not 0.25 in double — asserting against the literal at a
		// 1e-9 epsilon failed here the moment the inputs stopped being dyadic, which is the second
		// thing the non-dyadic values caught.
		const double delta = static_cast<double>(moved.gammaPhase) - static_cast<double>(in.gammaPhase);
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			CAPTURE(i);
			// EXACTLY EQUAL TO EACH OTHER is the claim worth making — gamma is ONE shift along the
			// unit vector, so all 39 coordinates move by the identical amount, not merely by amounts
			// that round to the same number.
			CHECK(after[i] - home[i] == after[0] - home[0]);
			CHECK(after[i] - home[i] == doctest::Approx(delta).epsilon(1e-9));
		}
	}

	// **THE PLANE IS BORN OPEN, BY DESIGN, AND THAT IS THE REASON THE AXES ARE NOT NAMED.**
	// At rotate 0 this is already a real plane — not the degenerate one where both travel directions
	// carry every triangle identically, which would make X and Y two more gammas. An earlier version
	// of this case asserted "X moves Type's six, Y moves Flavour's fifteen"; that plane was replaced
	// with Explore's.
	SUBCASE("rotate 0 is already an open plane, not a degenerate one") {
		const ps::Phases pushX = ps::phasesAt(home, basis, 1.0, 0.0);
		const ps::Phases pushY = ps::phasesAt(home, basis, 0.0, 1.0);
		size_t moved = 0;
		size_t differ = 0;
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			if (pushX[i] != home[i]) {
				++moved;
			}
			if (pushX[i] != pushY[i]) {
				++differ;
			}
		}
		CAPTURE(moved);
		CAPTURE(differ);
		// TRAVEL REACHES MOST OF THE SPACE, and the two directions are genuinely different — if X and
		// Y agreed everywhere the plane would be a line and travel would be one control.
		CHECK(moved > ps::kTriangles / 2);
		CHECK(differ > ps::kTriangles / 2);
	}

	// **AND TURNING THE GENERATOR TURNS THE PLANE.** Not asserted on a sound — on the coefficients,
	// because "the sound changed" would pass for any reason at all.
	SUBCASE("a different rotate is a different plane") {
		const ps::Basis turned = ps::basisFor(0.7, ps::planeHomeOf(home));
		size_t differing = 0;
		for (size_t i = 0; i < 2 * ps::kTriangles; ++i) {
			if (basis.x[i] != turned.x[i] || basis.y[i] != turned.y[i]) {
				++differing;
			}
		}
		CAPTURE(differing);
		CHECK(differing > ps::kTriangles);
	}

	// AND THE COUNT ITSELF, because it is the number a miscount would silently change: 30 is what you
	// get from `automod_derived.h` alone, missing the nine inside `lfoShapeAt`.
	SUBCASE("the space is 39 wide, not 30") {
		CHECK(ps::kTriangles == 39);
		CHECK(ps::kTypeTriangles + ps::kFlavourTriangles + ps::kModTriangles == ps::kTriangles);
		CHECK(ps::kModTriangles == 18);
	}
}


TEST_CASE("automod's folded travel: the blob rides the row, and a huge one costs no precision") {
	using namespace feathers::fx::automod;
	namespace ps = phase_space;

	AutomodCoords base {};
	base.type = 0.4321f;
	base.flavor = 0.6789f;
	base.mod = 0.2468f;
	base.gammaPhase = 0.1111f;
	base.lfoHz = 1.0f;

	SUBCASE("an absent blob is zero, and zero changes nothing") {
		// **THE MIGRATION GUARANTEE.** Every patch that predates the blob has a shorter
		// `fieldsPresent`, so these floats read as zero — and zero added in double to each coordinate
		// is exact, so the derive is what it always was. Bit for bit, not approximately.
		//
		// **COMPARED BY THE STRUCT'S OWN `operator==`, NOT BY memcmp.** `AutomodDerived` has a `bool`
		// followed by a struct, so it carries padding bytes, and memcmp reads them: their value is
		// indeterminate on every compiler, EQUAL BY ACCIDENT on g++ 14/x86-64, and different on Apple
		// clang/arm64 — the fourth macOS CI run (34109928824, 2026-09-07) built and linked green and
		// this line was its one red, `113 == 0`, with every derived value identical. The defaulted
		// equality compares every member and nothing else. It differs from a bit comparison only at
		// the sign of zero (−0.0 == +0.0) and at NaN (never equal to itself), so a derive that
		// produced a NaN fails here where memcmp would have passed it.
		AutomodCoords zeroed = base; // foldedTravel is value-initialised
		const AutomodDerived a = deriveAutomod(zeroed);
		AutomodCoords explicitly = base;
		explicitly.foldedTravel.fill(0.0f);
		const AutomodDerived b = deriveAutomod(explicitly);
		CHECK(a == b);
	}

	SUBCASE("a non-zero blob moves the derive, per coordinate") {
		// NOT "the sound changed" — WHICH coordinate moved. A blob wired to the wrong index, or
		// applied rank-1 instead of per-triangle, would change the sound and fail this.
		const AutomodDerived ref = deriveAutomod(base);
		size_t reached = 0;
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			AutomodCoords one = base;
			one.foldedTravel[i] = 0.37f;
			const AutomodDerived moved = deriveAutomod(one);
			// `!=` for the same reason the subcase above uses `==`: over padding bytes, memcmp could
			// count a coordinate as reached when nothing derived had moved — a vacuous pass, the worse
			// direction.
			if (moved != ref) {
				++reached;
			}
		}
		CAPTURE(reached);
		// Every coordinate is reachable through the blob, for the same reason and with the same
		// caveat as the phase sweep: the banks are half-rectified, so this is asked at one position
		// and a few will be quiet here. Most must move.
		// MEASURED at 27 of 39 at this position. The banks are half-rectified, so a triangle
		// contributing nothing here is the normal state — the sweep case above proves every one is
		// reachable SOMEWHERE, and this one only has to show the blob is wired per-coordinate rather
		// than rank-1.
		CHECK(reached * 2 > ps::kTriangles);
	}

	// **THE UNBOUNDED-ACCUMULATION GUARANTEE, BY DESIGN:** residue does not feed back into home, and
	// unbounded accumulation is fine as long as the surfaces are engineered not to saturate a
	// double.
	//
	// **AND THE FIRST VERSION OF THIS CASE ASSERTED SOMETHING FALSE ABOUT THE FIELD.** It compared a
	// residue of 1e7 against zero and expected the same sound, on the reasoning that the triangles are
	// periodic and 1e7 is a whole number of laps. It is not: `evalTriangle` computes
	// `wrapPhase((phase + offset) * phiFreq * freqMult)`, so triangle `i` repeats every `1/phiFreq_i`,
	// and the phi frequencies are irrational relative to 1 and to each other — which is the whole
	// point of the bank. **No number of "laps" returns any triangle to where it was.** The case failed,
	// and it was the case that was wrong.
	//
	// What IS true, and is the requirement: the stored float must not lose the fraction. A float at
	// 1e7 has a spacing of 1.0, so half a lap of travel is simply gone at that magnitude. The writer
	// wraps per triangle before storing (`wrapForStorage`), which keeps every triangle at exactly the
	// value it had while keeping the number small.
	SUBCASE("a huge residue wraps to the same sound, per triangle") {
		// THE ACCUMULATOR IS DOUBLE and holds the whole journey; the row holds what the wrap makes of
		// it. This is the write path the pad will use.
		std::array<double, kFoldedTravelFloats> accumulator {};
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			const double period = trianglePeriods()[i];
			REQUIRE(period > 0.0);
			accumulator[i] = period * 1.0e6 + period * 0.37;
		}
		AutomodCoords stored = base;
		wrapForStorage(accumulator, stored.foldedTravel);

		// THE WRAP KEPT THE FRACTION AND DROPPED THE LAPS: every stored value inside its own period.
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			CAPTURE(i);
			CHECK(stored.foldedTravel[i] >= 0.0f);
			CHECK(static_cast<double>(stored.foldedTravel[i]) < trianglePeriods()[i] * 1.000001);
		}

		AutomodCoords fraction = base;
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			fraction.foldedTravel[i] = static_cast<float>(trianglePeriods()[i] * 0.37);
		}
		const AutomodDerived viaWrap = deriveAutomod(stored);
		const AutomodDerived direct = deriveAutomod(fraction);
		const auto close = [](float x, float y) { return std::fabs(x - y) < 1e-3f; };
		CHECK(close(viaWrap.comb.mix, direct.comb.mix));
		CHECK(close(viaWrap.comb.feedback, direct.comb.feedback));
		CHECK(close(viaWrap.svfFeedback, direct.svfFeedback));
		CHECK(close(viaWrap.lfoStartPhase, direct.lfoStartPhase));

		// **AND THE UNWRAPPED FLOAT REALLY WOULD HAVE LOST IT.** Two journeys a hundredth of a period
		// apart, converted to float at a million periods out, land on the same number — so a writer
		// that skipped the wrap could not tell them apart. This is what the wrap rescues, asserted
		// rather than asserted-to-be-obvious.
		size_t indistinguishable = 0;
		for (size_t i = 0; i < ps::kTriangles; ++i) {
			const double period = trianglePeriods()[i];
			const auto a = static_cast<float>(period * 1.0e6 + period * 0.37);
			const auto b = static_cast<float>(period * 1.0e6 + period * 0.38);
			if (a == b) {
				++indistinguishable;
			}
		}
		CAPTURE(indistinguishable);
		CHECK(indistinguishable > 0);
	}
}

TEST_CASE("automod's Free mode takes its phase from the song, on the timeline its own rate uses") {
	// **BACKLOG DQ.** In Free mode Automod's phase locks to the transport — derived from the DAW
	// playhead time modulo the Automod period — and Free always locks.
	//
	// **ASSERTED ON THE PHASE, NEVER ON THE TAPS.** Four readings of one shape cannot be unpicked back
	// into a phase, and a comparison of two renders passes whenever ANYTHING differs — this file's
	// oldest rule, and the reason `Lfo::phase()` exists at all.
	using namespace feathers::fx::automod;
	Automod fx;
	fx.prepare(48000.0);

	Settings s {};
	s.enabled = true;
	s.lfoMode = LfoModeChoice::Free;
	fx.setTarget(s);

	// ONE BLOCK IS ONE TICK, so the phase read afterwards is the one the lock put there.
	std::array<float, kTickSamples> left {};
	std::array<float, kTickSamples> right {};
	const auto runOneTick = [&] {
		left.fill(0.0f);
		right.fill(0.0f);
		fx.processStereo(left.data(), right.data(), kTickSamples);
	};

	SUBCASE("backlog EH: a transport a hair before zero locks to the shape's start, not to one past uint32") {
		// `turns - floor(turns)` rounds to EXACTLY 1.0 for turns in (-2^-54, 0), and 1.0 x 2^32 is one
		// past uint32's range — an undefined cast (shape 3). The sibling converter carried the `< 1`
		// guard; the lock did not. Asserted by value: the phase at -1e-17 s is the phase at 0 s.
		s.syncDivision = 1; // 1 Hz: one turn per second
		s.rate = 0.0f;
		fx.setTarget(s);
		feathers::device::TransportClock tc {};
		tc.playing = true;
		tc.songPositionSeconds = 0.0;
		fx.setTransportClock(tc);
		runOneTick();
		const double atZero = fx.lfoPhaseForTest();
		tc.songPositionSeconds = -1e-17;
		fx.setTransportClock(tc);
		runOneTick();
		CHECK(fx.lfoPhaseForTest() == doctest::Approx(atZero).epsilon(1e-6));
	}

	SUBCASE("a FREE Hz rate reads SECONDS, because its period is a number of seconds") {
		// 1 Hz at 100%: the period is one second, so a playhead at 3.25 s is a quarter through.
		s.syncDivision = 1; // kFreeRateHz[1] == 1 Hz
		s.rate = 0.0f; // octaves: 1X
		fx.setTarget(s);

		feathers::device::TransportClock tc {};
		tc.playing = true;
		tc.songPositionSeconds = 3.25;
		fx.setTransportClock(tc);

		const std::optional<double> turns = fx.lockedTurns();
		REQUIRE(turns.has_value());
		// THE WHOLE NUMBER OF CYCLES IS KEPT — `lockPhaseTo` wraps, so the caller does not have to,
		// and asserting the unwrapped value says the derivation is `position / period` rather than
		// something that happens to agree after a modulo.
		CHECK(*turns == doctest::Approx(3.25));

		// **THE PHASE IS ASSERTED AS A DIFFERENCE, because `lockPhaseTo` adds the SHAPE'S start phase
		// and it should.** Song position zero ought to be the shape's start rather than an arbitrary
		// point in it, so the absolute reading carries an offset this case has no business restating —
		// a quarter of a cycle apart is the claim, and it holds whatever the shape starts at.
		runOneTick();
		const double atQuarter = fx.lfoPhaseForTest();

		tc.songPositionSeconds = 3.0; // a whole number of cycles: back to the shape's start
		fx.setTransportClock(tc);
		runOneTick();
		const double atStart = fx.lfoPhaseForTest();

		const double apart = atQuarter - atStart - std::floor(atQuarter - atStart);
		CHECK(apart == doctest::Approx(0.25).epsilon(0.002));
	}

	SUBCASE("a SYNCED division reads BEATS, so a tempo change moves the cycle with the music") {
		// **THIS IS THE HALF THAT WAS OPEN** — what is most natural in a sync mode. Beats, because the period IS a number of beats: bar 3 beat 1 is the same phase whatever the
		// tempo has done in between, where a seconds-derived phase would go discontinuous at every
		// tempo change.
		s.syncDivision = static_cast<int>(kFreeRateCount); // the first musical division
		s.rate = 0.0f; // octaves: 1X
		fx.setTarget(s);

		const double periodBeats = syncDivisionBeats(s.syncDivision);
		REQUIRE(periodBeats > 0.0);

		feathers::device::TransportClock tc = feathers::device::TransportClock::fromSecondsPerBeat(0.5); // 120 bpm
		tc.playing = true;
		tc.songPositionBeats = periodBeats * 2.5; // two and a half cycles in
		tc.songPositionSeconds = 999.0;           // deliberately WRONG, and deliberately ignored
		fx.setTransportClock(tc);

		const std::optional<double> turns = fx.lockedTurns();
		REQUIRE(turns.has_value());
		// **2.5 AND NOT 999-DERIVED**: the seconds above are nonsense on purpose, so a synced rate
		// that reached for them would fail loudly rather than agree by coincidence.
		CHECK(*turns == doctest::Approx(2.5));

		// AND THE SAME BEAT AT A DIFFERENT TEMPO IS THE SAME PHASE, which is what "on the grid" means
		// and what a seconds-derived phase could not promise.
		feathers::device::TransportClock faster = feathers::device::TransportClock::fromSecondsPerBeat(0.25); // 240 bpm
		faster.playing = true;
		faster.songPositionBeats = tc.songPositionBeats;
		faster.songPositionSeconds = 111.0;
		fx.setTransportClock(faster);
		CHECK(*fx.lockedTurns() == doctest::Approx(2.5));
	}

	SUBCASE("a STOPPED transport does not lock, so the LFO keeps running instead of freezing") {
		// `Synth::lockedPhaseFor` settled this for oscillator phase and the reasoning transfers: a
		// stopped host reports the SAME position for as long as the user leaves it there, so locking
		// would hand every audition one frozen phase.
		s.syncDivision = 1;
		s.rate = 0.0f; // octaves: 1X
		fx.setTarget(s);

		feathers::device::TransportClock tc {};
		tc.playing = false;
		tc.songPositionSeconds = 3.25;
		fx.setTransportClock(tc);
		CHECK_FALSE(fx.lockedTurns().has_value());

		// AND IT REALLY DOES KEEP RUNNING — the phase advances across ticks rather than sitting where
		// a lock would have pinned it.
		runOneTick();
		const double first = fx.lfoPhaseForTest();
		runOneTick();
		CHECK(fx.lfoPhaseForTest() != doctest::Approx(first));
	}

	SUBCASE("no timeline at all is not a position, and the accumulator is left alone") {
		// Standalone, and any host that sends neither timeline. Today's behaviour exactly, which is
		// what makes this feature host-dependent by construction — better said out loud than found.
		s.syncDivision = 1;
		fx.setTarget(s);
		feathers::device::TransportClock tc {};
		tc.playing = true; // rolling, but with nowhere to be
		fx.setTransportClock(tc);
		CHECK_FALSE(fx.lockedTurns().has_value());
	}

	SUBCASE("the RATE multiplier shortens the period, so 200% is twice round") {
		// `effectiveRateHz` multiplies the rate up, so the period is the division DIVIDED by the
		// multiplier. A lock that ignored it would drift against the sound it is supposed to describe.
		s.syncDivision = static_cast<int>(kFreeRateCount);
		s.rate = 2.0f;
		fx.setTarget(s);

		feathers::device::TransportClock tc = feathers::device::TransportClock::fromSecondsPerBeat(0.5);
		tc.playing = true;
		tc.songPositionBeats = syncDivisionBeats(s.syncDivision); // one DIVISION in
		fx.setTransportClock(tc);

		// **ONE DIVISION IN IS EXACTLY `multiplier` CYCLES, and the multiplier is read from the curve
		// rather than restated.** The Rate control is EXPONENTIAL — `exp2(2 * (v - 1))`, so its top is
		// 4x and not 2x — and a case that hard-coded a number here would be asserting a linear taper
		// this device does not have. MEASURED: the first draft of this line expected 2.0 and read 4.0.
		const double multiplier = static_cast<double>(rateMultiplierFor(s.rate));
		REQUIRE(multiplier > 1.0); // the subcase is vacuous at unity
		CHECK(*fx.lockedTurns() == doctest::Approx(multiplier));
	}
}

TEST_CASE("backlog FK: a moving Mix is followed per sample inside the call — every sample is the dry and the wet mixed at that sample's value") {
	// Restated independently: Mix does not reach the filter, the comb or the springs, so a twin at Mix 1 is the wet
	// signal and the input is the dry; each output sample must be dry + (wet - dry) * m[i]. Through the ops road.
	constexpr int kFrames = 64;
	const std::vector<float> in = source(4096);
	const std::vector<float> wet = renderStereo(liveSettings(), in, kFrames);
	std::array<float, kFrames> run {};
	for (int i = 0; i < kFrames; ++i) {
		run[static_cast<size_t>(i)] = static_cast<float>(i) / static_cast<float>(kFrames - 1);
	}
	std::array<const float*, 32> runs {};
	runs[static_cast<size_t>(Param::Mix)] = run.data();
	REQUIRE(kAudioOps.setFieldWindows != nullptr);

	Settings s = liveSettings();
	s.mix = 0.4f; // the scalar the window replaces
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	for (size_t at = 0; at + kFrames <= in.size(); at += kFrames) {
		kAudioOps.setFieldWindows(&a, runs.data(), runs.size(), kFrames);
		a.processStereo(l.data() + at, r.data() + at, kFrames);
	}
	float worst = 0.0f;
	float apart = 0.0f;
	for (size_t i = 0; i < in.size(); ++i) {
		const float m = run[i % kFrames];
		worst = std::max(worst, std::abs(l[i] - (in[i] + (wet[i] - in[i]) * m)));
		apart = std::max(apart, std::abs(wet[i] - in[i]));
	}
	CAPTURE(worst);
	CHECK(apart > 0.05f); // not vacuous: the device changes the sound
	CHECK(worst < 1e-5f);
}

//------------------------------------------------------------------------------------------------//
// BACKLOG IS — the Automod DSP review (approved, plus a stereo width knob so the width can be
// disabled).
//------------------------------------------------------------------------------------------------//

namespace {

/// Where the filter's LEFT corner sits in modulation units — `cornerHzFor` inverted, at no pitch or Freq.
float impliedModulation(const Automod& a) {
	const AutomodDerived& d = a.derived();
	return std::log2(a.filterCutoffForTest() / baseCutoffHzFor(d.filterCutoffBase))
	       / (d.filterModDepth * kFilterModOctaves);
}

void runQuiet(Automod& a, int frames) {
	for (int i = 0; i < frames; ++i) {
		float l = 0.1f;
		float r = 0.1f;
		a.processStereo(&l, &r, 1);
	}
}

} // namespace

TEST_CASE("IS: a rebuilt waveform moves the springs with their TARGET — no lunge in Stop, none in Free, and Depth survives a moving coordinate") {
	// MEASURED BEFORE THE FIX: a 0.002 Type nudge threw the corner 1.09 octaves in Stop and 1.0 in Free,
	// and a Type that moved every tick left the corner on the RAW tap to 0.0004.
	SUBCASE("Stop: the target is Manual, so a nudge moves nothing but the base") {
		Settings s = liveSettings();
		s.lfoMode = LfoModeChoice::Stop;
		s.manual = 0.3f;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		runQuiet(a, 1);
		// THE FIRST BUILD PUTS THE SPRING ON ITS TARGET — at the first sample, not after a walk.
		CHECK(impliedModulation(a) == doctest::Approx(0.3f).epsilon(1e-3));
		runQuiet(a, 4096 - 1); // to a tick boundary, so the nudge is read by the next sample
		s.type += 0.002f;
		a.setTarget(s);
		runQuiet(a, 1);
		CHECK(a.deriveCountForTest() == 2); // the nudge DID rebuild — or this proves nothing
		CHECK(impliedModulation(a) == doctest::Approx(0.3f).epsilon(1e-3));
	}
	SUBCASE("Free: across a nudge the corner moves by what the TARGET moved, and no more") {
		Settings s = liveSettings();
		s.depth = 0.5f;
		s.manual = 0.0f;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		runQuiet(a, 96000);
		const float before = impliedModulation(a);
		const float targetBefore = a.springTargetForTest(0);
		s.type += 0.002f;
		a.setTarget(s);
		runQuiet(a, 1);
		REQUIRE(a.deriveCountForTest() == 2);
		const float jump = impliedModulation(a) - before;
		const float targetMoved = a.springTargetForTest(0) - targetBefore;
		CAPTURE(jump);
		CAPTURE(targetMoved);
		// ONE SAMPLE OF SPRING TRAVEL PLUS WHAT THE REBUILD MOVED THE TARGET BY — measured 1.0 octave of
		// modulation-units jump before the fix, against a target that moved by a hundredth of that.
		CHECK(std::fabs(jump) < std::fabs(targetMoved) + 0.02f);
	}
	SUBCASE("a Type moving EVERY tick leaves the corner where a still Type puts it — Depth is not bypassed") {
		const auto trace = [](bool wobble) {
			Settings s = liveSettings();
			s.depth = 0.3f;
			s.manual = 0.0f;
			s.rate = 2.0f;
			Automod a;
			a.prepare(kSr);
			a.setTarget(s);
			runQuiet(a, 48000);
			std::vector<float> out;
			std::vector<float> raw;
			for (int t = 0; t < 750; ++t) {
				if (wobble) {
					s.type = 0.0339f + 0.0005f * static_cast<float>(t % 2);
					a.setTarget(s);
				}
				runQuiet(a, kTickSamples);
				out.push_back(impliedModulation(a));
				raw.push_back(a.lfoTapForTest(0));
			}
			return std::pair { out, raw };
		};
		const auto [still, stillRaw] = trace(false);
		const auto [moving, movingRaw] = trace(true);
		double drift = 0.0;
		double offRaw = 0.0;
		for (size_t i = 0; i < still.size(); ++i) {
			drift += std::fabs(moving[i] - still[i]);
			offRaw += std::fabs(moving[i] - movingRaw[i]);
		}
		drift /= static_cast<double>(still.size());
		offRaw /= static_cast<double>(still.size());
		CAPTURE(drift);
		CAPTURE(offRaw);
		// THE MOVING RUN TRACKS THE STILL ONE — a 0.0005 Type wobble is a small change of sound.
		CHECK(drift < 0.03);
		// AND IS NOT THE RAW TAP, which is where the bug put it (0.0004 from it, measured). At depth 0.3
		// the target is under a third of the tap, so a run on the target sits well away from it.
		CHECK(offRaw > 0.1);
	}
}

TEST_CASE("IS: each ear has its own filter corner, and Width scales how far apart the ears run — 0 is mono") {
	Settings s = liveSettings();
	s.depth = 0.8f;
	const auto derived = deriveAutomod(coordsOf(s, 1.0f));
	// NON-VACUITY: this coordinate HAS a stereo offset for Width to scale.
	const float offset = derived.stereoPhaseOffset - std::floor(derived.stereoPhaseOffset);
	REQUIRE(std::min(offset, 1.0f - offset) > 0.05f);

	SUBCASE("Width 0: the two ears are one — every sample, bit for bit") {
		s.width = 0.0f;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		for (int i = 0; i < 48000; ++i) {
			float l = 0.1f;
			float r = 0.1f;
			a.processStereo(&l, &r, 1);
			if (a.filterCutoffForTest() != a.filterCutoffRightForTest()) {
				FAIL("the ears split at sample " << i);
			}
		}
		CHECK(a.lfoTapForTest(0) == a.lfoTapForTest(1));
	}
	SUBCASE("Width w: the right tap is the shape read w of the offset later — by value") {
		for (const float w : { 1.0f, 0.5f }) {
			CAPTURE(w);
			s.width = w;
			Automod a;
			a.prepare(kSr);
			a.setTarget(s);
			runQuiet(a, kTickSamples * 37);
			// THE TAP THIS TICK WAS READ AT THE PHASE BEFORE THE TICK'S ADVANCE — so step the phase back.
			const double inc = static_cast<double>(a.effectiveRateHz()) * kTickSamples / kSr;
			const float phase = static_cast<float>(a.lfoPhaseForTest() - inc);
			const float left = feathers::character::evalLfoShape(a.derived().shape, phase);
			const float right = feathers::character::evalLfoShape(a.derived().shape, phase + derived.stereoPhaseOffset * w);
			CHECK(a.lfoTapForTest(0) == doctest::Approx(left).epsilon(1e-3));
			CHECK(a.lfoTapForTest(1) == doctest::Approx(right).epsilon(1e-3));
		}
	}
	SUBCASE("Width 1: the ears' corners part, which is what the averaged corner could never do") {
		s.width = 1.0f;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		float widest = 0.0f;
		for (int t = 0; t < 1500; ++t) {
			runQuiet(a, kTickSamples);
			widest = std::max(widest, std::fabs(std::log2(a.filterCutoffForTest() / a.filterCutoffRightForTest())));
		}
		CAPTURE(widest);
		CHECK(widest > 0.1f);
	}
	SUBCASE("a Width turn GLIDES — the right corner does not jump when the knob moves") {
		s.width = 1.0f;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		runQuiet(a, 24000);
		const long derives = a.deriveCountForTest();
		float last = a.filterCutoffRightForTest();
		s.width = 0.0f;
		a.setTarget(s);
		float biggest = 0.0f;
		for (int i = 0; i < 256; ++i) {
			runQuiet(a, 1);
			const float now = a.filterCutoffRightForTest();
			biggest = std::max(biggest, std::fabs(std::log2(now / last)));
			last = now;
		}
		CHECK(a.deriveCountForTest() == derives); // Width is not a coordinate: nothing re-derived
		CAPTURE(biggest);
		CHECK(biggest < 0.01f); // a spring's step, not a jump
	}
}

TEST_CASE("IS: the band blend and the comb's base delay ramp across a tick instead of stepping at its start") {
	SUBCASE("the band blend") {
		// A COORDINATE WHOSE BLEND THE LFO MODULATES — searched, so the case cannot pass on a static one.
		Settings s = liveSettings();
		s.rate = 2.0f;
		bool found = false;
		for (int k = 0; k < 400 && !found; ++k) {
			s.flavor = static_cast<float>(k) / 400.0f;
			found = !deriveAutomod(coordsOf(s, 1.0f)).staticFilterMix;
		}
		REQUIRE(found);
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		runQuiet(a, kTickSamples * 4);
		float biggest = 0.0f;
		float atBoundary = 0.0f;
		int movedMidTick = 0;
		int midTickSamples = 0;
		FilterMix last = a.filterMixForTest();
		for (int i = 0; i < kTickSamples * 400; ++i) {
			runQuiet(a, 1);
			const FilterMix now = a.filterMixForTest();
			const float step = std::fabs(now.low - last.low) + std::fabs(now.band - last.band) + std::fabs(now.high - last.high);
			biggest = std::max(biggest, step);
			if (i % kTickSamples != 0) {
				++midTickSamples;
				movedMidTick += step > 0.0f;
			}
			if (i % kTickSamples == 0) {
				atBoundary = std::max(atBoundary, step);
			}
			last = now;
		}
		CAPTURE(biggest);
		CAPTURE(atBoundary);
		CHECK(biggest > 0.0f);           // it moves at all
		CHECK(biggest < 0.01f);          // in small steps
		// **AND BETWEEN THE TICKS, NOT AT THEM** — the discriminating claim. A stair changes only at a tick's
		// first sample and is still inside the bar above at this coordinate (a known-bad that removed the
		// ramp stayed green on it); a ramp moves on nearly every sample of the tick.
		CAPTURE(movedMidTick);
		CHECK(movedMidTick * 10 > midTickSamples * 9);
	}
	SUBCASE("the comb's base across a note change") {
		Settings s = liveSettings();
		s.lfoMode = LfoModeChoice::Stop; // so the delay is the base alone
		s.manual = 0.0f;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		runQuiet(a, kTickSamples * 8);
		const float from = a.combDelayForTest();
		feathers::device::NoteContext note;
		note.pitchSemitones = 72.0f; // an octave up: the delay HALVES
		a.setNoteContext(note);
		float biggest = 0.0f;
		float last = from;
		for (int i = 0; i < kTickSamples * 3; ++i) {
			runQuiet(a, 1);
			biggest = std::max(biggest, std::fabs(a.combDelayForTest() - last));
			last = a.combDelayForTest();
		}
		CHECK(last == doctest::Approx(from * 0.5f).epsilon(1e-3)); // it arrived
		CAPTURE(biggest);
		// ONE TICK'S WORTH PER SAMPLE — the whole halving spread over 64 samples, not taken in one.
		CHECK(biggest <= (from * 0.5f) / static_cast<float>(kTickSamples) + 1e-3f);
	}
}

TEST_CASE("IS: the MONO road bypasses as the stereo one does — re-enabled, it is a fresh device") {
	Settings s = liveSettings();
	std::vector<float> in = source(8192);
	const auto render = [&](Automod& a) {
		std::vector<float> m = in;
		a.processMono(m.data(), static_cast<int>(m.size()));
		return m;
	};
	Automod used;
	used.prepare(kSr);
	used.setTarget(s);
	render(used); // fill the comb's line
	s.enabled = false;
	used.setTarget(s);
	std::vector<float> silence(64, 0.0f);
	used.processMono(silence.data(), 64);
	s.enabled = true;
	used.setTarget(s);

	Automod fresh;
	fresh.prepare(kSr);
	fresh.setTarget(s);
	const std::vector<float> a = render(used);
	const std::vector<float> b = render(fresh);
	size_t differ = 0;
	for (size_t i = 0; i < a.size(); ++i) {
		differ += a[i] != b[i];
	}
	CHECK(differ == 0);
}

TEST_CASE("IS: Width is declared as an amount — 0..1, default the field's own stereo, modulatable") {
	const feathers::device::ParamDesc& d = kParamDescs[static_cast<size_t>(Param::Width)];
	CHECK(std::string_view(d.name) == "Width");
	CHECK(d.min == 0.0);
	CHECK(d.max == 1.0);
	CHECK(d.def == 1.0);
	CHECK(d.modulatable);
	CHECK(Settings {}.width == static_cast<float>(d.def));
	Row row = rowOf(Settings {});
	row[static_cast<size_t>(Param::Width)] = 7.0f;
	CHECK(settingsOf(row).width == 1.0f);
	row[static_cast<size_t>(Param::Width)] = std::numeric_limits<float>::quiet_NaN();
	CHECK(settingsOf(row).width == 1.0f);
	row[static_cast<size_t>(Param::Width)] = 0.25f;
	CHECK(rowOf(settingsOf(row))[static_cast<size_t>(Param::Width)] == 0.25f);
}

TEST_CASE("IS: across a rebuild the spring moves by exactly what its target moved at that phase — restated from the two shapes") {
	// THE FIRMWARE'S RULE, BY VALUE: no lunge (the spring must not be left where it was to glide to the new
	// shape's value) and no snap (it must not be put on the raw tap). The shift is restated here from the
	// two waveforms read at the same phase, through the steer's scale — independently of the engine.
	Settings s = liveSettings();
	s.depth = 0.8f;
	s.manual = 0.0f;
	Automod a;
	a.prepare(kSr);
	a.setTarget(s);
	runQuiet(a, kTickSamples * 400);
	const float before = a.filterSpringForTest(0);
	const feathers::character::LfoShape oldShape = a.derived().shape;
	const float phase = static_cast<float>(a.lfoPhaseForTest());
	const float scale = a.envScaleForTest(0);
	s.mod = 0.45f; // a big move of MOD, which is what reshapes the LFO (Type does not reach its shape)
	a.setTarget(s);
	runQuiet(a, 1);
	REQUIRE(a.deriveCountForTest() == 2);
	const float shift = (feathers::character::evalLfoShape(a.derived().shape, phase)
	                     - feathers::character::evalLfoShape(oldShape, phase))
	                    * scale;
	CAPTURE(before);
	CAPTURE(scale);
	CAPTURE(phase);
	CAPTURE(feathers::character::evalLfoShape(oldShape, phase));
	CAPTURE(feathers::character::evalLfoShape(a.derived().shape, phase));
	CAPTURE(shift);
	CAPTURE(a.filterSpringForTest(0));
	REQUIRE(std::fabs(shift) > 0.05f); // non-vacuity: the rebuild moved the target
	// ONE SAMPLE OF SPRING TRAVEL IS ALL THAT MAY SEPARATE THEM.
	CHECK(std::fabs(a.filterSpringForTest(0) - (before + shift)) < 0.05f * std::fabs(shift));
}

TEST_CASE("IS.5: Keytrack is ONE switch for both filters — on, the corner doubles and the delay halves an octave up; off, neither moves") {
	// One global keytrack switch enables or disables it for all the filters, not just the comb. STOPPED, so the springs sit on Manual and the two numbers are the base
	// times the pitch ratio alone.
	const auto measure = [](bool keytrack, float semis) {
		Settings s = liveSettings();
		s.lfoMode = LfoModeChoice::Stop;
		s.manual = 0.0f;
		s.keytrack = keytrack;
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		feathers::device::NoteContext note;
		note.pitchSemitones = semis;
		a.setNoteContext(note);
		runQuiet(a, kTickSamples * 4);
		return std::pair { a.filterCutoffForTest(), a.combDelayForTest() };
	};
	const auto [hz60, delay60] = measure(true, 60.0f);
	const auto [hz72, delay72] = measure(true, 72.0f);
	CHECK(hz72 / hz60 == doctest::Approx(2.0f).epsilon(1e-4));
	CHECK(delay60 / delay72 == doctest::Approx(2.0f).epsilon(1e-3));
	// AND THE RIGHT EAR TRACKS TOO — the per-ear corner shares the ratio.
	const auto [offHz72, offDelay72] = measure(false, 72.0f);
	CHECK(offHz72 == hz60);
	CHECK(offDelay72 == delay60);
	// AND A LOW NOTE, THE OTHER WAY — so "off" is not "clamped to one direction".
	const auto [offHz48, offDelay48] = measure(false, 48.0f);
	CHECK(offHz48 == hz60);
	CHECK(offDelay48 == delay60);
	// DECLARED AS A SWITCH, ON BY DEFAULT: both filters tracked before it existed.
	const feathers::device::ParamDesc& d = kParamDescs[static_cast<size_t>(Param::Keytrack)];
	CHECK(d.stepped);
	CHECK(d.def == 1.0);
	CHECK(Settings {}.keytrack);
	Row row = rowOf(Settings {});
	row[static_cast<size_t>(Param::Keytrack)] = 0.0f;
	CHECK_FALSE(settingsOf(row).keytrack);
}

TEST_CASE("automod's Free LFO under a Rate route follows the song's step — it is not scrubbed by the period moving") {
	// **THE MODULATORS' DEFECT, IN THE FX LANE (option A, 2026-09-25).** Rate is a route
	// destination here, and the lock placed the phase at `position / period` every tick — so a route
	// moving the period threw the phase by `turns × Δperiod / period`, further the later in the song.
	// Now the song's STEP is followed against the period as it is now (`device::TransportLock`).
	//
	// ASSERTED ON THE PHASE, against an independent accumulator `T += Δbeats / P_now`. The shape's start
	// phase is an offset this case has no business restating, so it is read off the first tick and held.
	using namespace feathers::fx::automod;
	Automod fx;
	fx.prepare(48000.0);
	Settings s {};
	s.enabled = true;
	s.lfoMode = LfoModeChoice::Free;
	s.syncDivision = static_cast<int>(kFreeRateCount) + 3; // a musical division
	s.rate = 0.0f;
	fx.setTarget(s);
	const double divisionBeats = syncDivisionBeats(s.syncDivision);
	REQUIRE(divisionBeats > 0.0);

	std::array<float, kTickSamples> left {};
	std::array<float, kTickSamples> right {};
	constexpr double kSecondsPerBeat = 0.5;
	const double tickBeats = static_cast<double>(kTickSamples) / 48000.0 / kSecondsPerBeat;
	feathers::device::TransportClock tc = feathers::device::TransportClock::fromSecondsPerBeat(kSecondsPerBeat);
	tc.playing = true;
	const auto circular = [](double a, double b) {
		double d = (a - std::floor(a)) - (b - std::floor(b));
		return std::abs(d - std::round(d));
	};

	double expected = 0.0;
	double offset = 0.0;
	double worst = 0.0;
	double quotientMiss = 0.0;
	for (int k = 0; k < 3000; ++k) {
		s.rate = 0.5f * static_cast<float>(std::sin(2.0 * M_PI * k / 700.0)); // a route, as it resolves
		fx.setTarget(s);
		tc.songPositionBeats = 64.0 + k * tickBeats;
		fx.setTransportClock(tc);
		const double period = divisionBeats / rateMultiplierFor(s.rate);
		expected = k == 0 ? tc.songPositionBeats / period : expected + tickBeats / period;
		left.fill(0.0f);
		right.fill(0.0f);
		fx.processStereo(left.data(), right.data(), kTickSamples);
		// THE TICK AFTER THE LOCK advances by one tick at the current rate.
		const double want = expected + tickBeats / period;
		if (k == 0) {
			offset = fx.lfoPhaseForTest() - want;
		}
		worst = std::max(worst, circular(fx.lfoPhaseForTest() - offset, want));
		quotientMiss = std::max(quotientMiss, circular(tc.songPositionBeats / period, expected));
	}
	CAPTURE(worst);
	CAPTURE(quotientMiss);
	CHECK(worst < 2e-3);
	REQUIRE(quotientMiss > 0.3); // the old rule would have been elsewhere: the check above is not vacuous
}

TEST_CASE("automod: at rest on silence the chain is skipped, and the output and the resumed phase are bit-identical to running it") {
	// After the flush the envelope, filter and comb hold exact zeros, so a zero frame through them is a zero frame out.
	// The reference is the SAME device with the skip off (the full chain every frame), over burst, silence past the
	// flush, and a second burst: the LFO and springs must have advanced as if nothing was skipped, so the second burst
	// comes out to the bit. The counters say the skip happened and where.
	const Settings s = liveSettings();
	const int rest = static_cast<int>(kRestSilentSeconds * kSr) + 20000;
	const std::vector<float> burst = source(4096);
	const auto render = [&](bool skip, size_t* chained, size_t* skipped) {
		Automod a;
		a.prepare(kSr);
		a.setTarget(s);
		a.setRestSkipForTest(skip);
		std::vector<float> out;
		const auto feed = [&](const std::vector<float>& in) {
			std::vector<float> l = in;
			std::vector<float> r = in;
			for (size_t at = 0; at < l.size(); at += 256) {
				const int n = static_cast<int>(std::min<size_t>(256, l.size() - at));
				a.processStereo(l.data() + at, r.data() + at, n);
			}
			out.insert(out.end(), l.begin(), l.end());
			out.insert(out.end(), r.begin(), r.end());
		};
		feed(burst);
		const size_t before = a.framesChainedForTest();
		feed(std::vector<float>(static_cast<size_t>(rest), 0.0f));
		*chained = a.framesChainedForTest() - before;
		feed(burst);
		*skipped = a.framesSkippedForTest();
		return out;
	};
	size_t chainedRef = 0;
	size_t skippedRef = 0;
	size_t chained = 0;
	size_t skipped = 0;
	const std::vector<float> ref = render(false, &chainedRef, &skippedRef);
	const std::vector<float> got = render(true, &chained, &skipped);
	CHECK(got == ref);
	CHECK(skippedRef == 0u);
	CHECK(chainedRef == static_cast<size_t>(rest));
	// 20000 frames of the silence are past the flush; only the frames before it ran the chain.
	CHECK(skipped >= 19000u);
	CHECK(chained + skipped >= static_cast<size_t>(rest));
	CHECK(chained <= static_cast<size_t>(rest) - 19000u);
	// NON-VACUITY: the second burst is a real signal.
	CHECK(std::any_of(got.begin() + static_cast<std::ptrdiff_t>(burst.size() + rest) * 2 - static_cast<std::ptrdiff_t>(burst.size()),
	                  got.end(), [](float v) { return v != 0.0f; }));
}
