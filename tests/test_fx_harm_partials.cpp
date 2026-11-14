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

#include "fx/harm/harm.h"
#include "fx/harm/harm_envelope.h"
#include "fx/harm/harm_types.h"
#include "harm_reference.h"

#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

using namespace feathers::fx::harm;

namespace {

constexpr double kRate = 48000.0;

/// A repeatable noisy-but-tonal input, so a notch has something to take out and a bit-for-bit comparison has something to
/// disagree about.
std::vector<float> testInput(size_t frames, uint32_t seed) {
	std::vector<float> out(frames);
	uint32_t x = seed;
	for (size_t i = 0; i < frames; ++i) {
		x = x * 1664525u + 1013904223u;
		const float noise = (static_cast<float>(x >> 8) / static_cast<float>(1u << 24) - 0.5f) * 0.3f;
		out[i] = noise + 0.4f * std::sin(static_cast<float>(i) * 0.0575f);
	}
	return out;
}

size_t indexOfRatio(int num, int den) { return harmonicIndexOf(num, den); }

struct Scenario {
	const char* name;
	size_t harmonic;
	bool highpass;
	float width, depth, level, fine, phase, spread;
	float porta, attack, release;
	bool depthFollowsEnv;
};

Settings settingsFor(const Scenario& sc) {
	Settings s;
	s.harmonic = sc.harmonic;
	s.highpass = sc.highpass;
	s.width = sc.width;
	s.depth = sc.depth;
	s.level = sc.level;
	s.fineSemitones = sc.fine;
	s.phase = sc.phase;
	s.spread = sc.spread;
	s.portaSeconds = sc.porta;
	s.attackSeconds = sc.attack;
	s.releaseSeconds = sc.release;
	s.depthFollowsEnv = sc.depthFollowsEnv;
	s.noteSemitones = 52.0f;
	return s;
}

/// Runs any HARM-shaped object over `in` in blocks of `block`: the gate is up for the first 60 % and then falls, the note
/// changes once partway through, and a Level window is handed in for the middle third.
template <class H>
std::pair<std::vector<float>, std::vector<float>> render(H& h, Settings s, const std::vector<float>& in, int block,
                                                       bool stereo, const std::vector<float>* levelWindow) {
	std::vector<float> l = in;
	std::vector<float> r = in;
	const size_t n = in.size();
	for (size_t pos = 0; pos < n; pos += static_cast<size_t>(block)) {
		const int take = static_cast<int>(std::min<size_t>(static_cast<size_t>(block), n - pos));
		s.gate = pos < n * 6 / 10;
		s.noteSemitones = pos < n / 2 ? 52.0f : 55.0f;
		h.setTarget(s);
		const bool windowed = levelWindow != nullptr && pos >= n / 3 && pos < 2 * n / 3;
		if (windowed) {
			h.setWindowRuns(levelWindow->data() + pos, nullptr, nullptr);
		}
		if (stereo) {
			h.processStereo(l.data() + pos, r.data() + pos, take);
		}
		else {
			h.processMono(l.data() + pos, take);
		}
	}
	return { l, r };
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b) {
	return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0;
}

} // namespace

TEST_CASE("only the first partial active renders the very floats the single-partial device did") {
	// THE IDENTITY GUARANTEE, by comparison with a copy of the device taken before it grew partials (tests/harm_reference.h).
	// Notch and High-pass, mono and stereo, several ratios, four block sizes — and partials 2 to 4 hold non-default,
	// loud settings while OFF, because inactive must mean "contributes nothing", not "happens to be quiet".
	const std::array<Scenario, 6> scenarios { {
	    { "unison notch", kUnisonHarmonic, false, 0.3f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.2f, false },
	    { "fifth, shallow notch, detuned", 0, false, 0.6f, 0.5f, 0.7f, 3.0f, 0.25f, 0.5f, 0.0f, 0.005f, 0.05f, false },
	    { "octave, depth follows env", 0, false, 0.1f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.02f, 0.01f, 0.1f, true },
	    { "two octaves, high-pass", 0, true, 0.3f, 1.0f, 0.8f, 0.0f, 0.1f, 1.0f, 0.0f, 0.0f, 0.3f, false },
	    { "sub-harmonic, porta", 0, false, 0.9f, 0.8f, 0.5f, -2.0f, 0.5f, 0.3f, 0.05f, 0.002f, 0.02f, false },
	    { "unison high-pass, slow attack", kUnisonHarmonic, true, 0.3f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.05f, 0.1f, false },
	} };
	const std::array<size_t, 6> harmonics { kUnisonHarmonic, indexOfRatio(3, 2), indexOfRatio(2, 1), indexOfRatio(4, 1),
		                                    indexOfRatio(1, 2), kUnisonHarmonic };
	const std::vector<float> in = testInput(9600, 7);
	std::vector<float> window(in.size());
	for (size_t i = 0; i < window.size(); ++i) {
		window[i] = 0.5f + 0.5f * std::sin(static_cast<float>(i) * 0.01f);
	}

	int compared = 0;
	for (size_t si = 0; si < scenarios.size(); ++si) {
		Scenario sc = scenarios[si];
		sc.harmonic = harmonics[si];
		for (const int block : { 32, 64, 480, 1024 }) {
			for (const bool stereo : { true, false }) {
				Settings s = settingsFor(sc);
				// the other three partials: loud, different, and off
				for (PartialSettings& p : s.more) {
					p.active = false;
					p.level = 1.0f;
					p.width = 0.9f;
					p.harmonic = indexOfRatio(5, 1);
				}
				harmref::Harm ref;
				ref.prepare(kRate);
				Harm now;
				now.prepare(kRate);
				const auto a = render(ref, s, in, block, stereo, si == 1 ? &window : nullptr);
				const auto b = render(now, s, in, block, stereo, si == 1 ? &window : nullptr);
				INFO(sc.name << " block " << block << (stereo ? " stereo" : " mono"));
				CHECK(sameBits(a.first, b.first));
				if (stereo) {
					CHECK(sameBits(a.second, b.second));
				}
				++compared;
			}
		}
	}
	CHECK(compared == 6 * 4 * 2);

	SUBCASE("non-vacuity: the reference does something, and turning a second partial on changes the output") {
		Settings s = settingsFor(scenarios[0]);
		harmref::Harm ref;
		ref.prepare(kRate);
		const auto a = render(ref, s, in, 480, true, nullptr);
		CHECK(!sameBits(a.first, in));
		s.more[0].active = true;
		Harm now;
		now.prepare(kRate);
		const auto b = render(now, s, in, 480, true, nullptr);
		CHECK(!sameBits(a.first, b.first));
	}
}

namespace {

/// The level of a sine in `x` at `hz`, by projection onto sine and cosine over the whole span.
double amplitudeAt(const std::vector<float>& x, size_t from, double hz) {
	double re = 0.0;
	double im = 0.0;
	for (size_t i = from; i < x.size(); ++i) {
		const double w = 2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate;
		re += x[i] * std::cos(w);
		im += x[i] * std::sin(w);
	}
	return 2.0 * std::hypot(re, im) / static_cast<double>(x.size() - from);
}

/// Upward zero crossings per second over `x[from..]`, interpolated, so the estimate is good to a small fraction of a cycle.
double crossingHz(const std::vector<float>& x, size_t from) {
	double first = -1.0;
	double last = -1.0;
	int count = 0;
	for (size_t i = from + 1; i < x.size(); ++i) {
		if (x[i - 1] < 0.0f && x[i] >= 0.0f) {
			const double at = static_cast<double>(i - 1) + static_cast<double>(-x[i - 1]) / static_cast<double>(x[i] - x[i - 1]);
			if (first < 0.0) {
				first = at;
			}
			last = at;
			++count;
		}
	}
	return count < 2 ? 0.0 : (count - 1) * kRate / (last - first);
}

/// THE PITCH A RATIO MAKES, restated from the table's integers and the plugin's A4 = 440 convention — not from `ratioAt`.
double expectedHz(float note, int num, int den) {
	return 440.0 * std::pow(2.0, (static_cast<double>(note) - 69.0) / 12.0) * num / den;
}

Settings onlyPartial(int k, size_t harmonic, float level) {
	Settings s;
	s.active = false;
	for (PartialSettings& p : s.more) {
		p.active = false;
	}
	s.noteSemitones = 48.0f;
	s.gate = true;
	s.attackSeconds = 0.0f;
	if (k == 0) {
		s.active = true;
		s.harmonic = harmonic;
		s.level = level;
	}
	else {
		s.more[static_cast<size_t>(k - 1)].active = true;
		s.more[static_cast<size_t>(k - 1)].harmonic = harmonic;
		s.more[static_cast<size_t>(k - 1)].level = level;
	}
	return s;
}

std::vector<float> runLeft(const Settings& s, const std::vector<float>& in) {
	Harm h;
	h.prepare(kRate);
	h.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	h.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	return l;
}

} // namespace

TEST_CASE("a partial alone is a sine at the note times its own ratio, at the level its Level implies") {
	const std::vector<float> silence(48000, 0.0f);
	struct Pick {
		int partial;
		int num;
		int den;
		float level;
	};
	for (const Pick pick : { Pick { 0, 3, 2, 0.5f }, Pick { 1, 2, 1, 0.5f }, Pick { 2, 3, 1, 0.25f }, Pick { 3, 4, 1, 0.8f },
	                         Pick { 3, 5, 4, 1.0f } }) {
		INFO("partial " << pick.partial + 1 << " ratio " << pick.num << ":" << pick.den);
		const Settings s = onlyPartial(pick.partial, indexOfRatio(pick.num, pick.den), pick.level);
		const std::vector<float> out = runLeft(s, silence);
		const double hz = expectedHz(48.0f, pick.num, pick.den);
		CHECK(crossingHz(out, 4800) == doctest::Approx(hz).epsilon(2.0e-4));
		// THE DOCUMENTED GAIN LAW, restated: Level, times the envelope (full: attack 0), times the pink scale sqrt(40/Hz)
		// above 40 Hz.
		const double expected = static_cast<double>(pick.level) * std::sqrt(40.0 / hz);
		CHECK(amplitudeAt(out, 4800, hz) == doctest::Approx(expected).epsilon(2.0e-3));
	}

	SUBCASE("every partial's notch, alone, is the first partial's notch with the same controls — to the bit") {
		const std::vector<float> in = testInput(9600, 3);
		for (int k = 1; k < kPartialCount; ++k) {
			Settings a = onlyPartial(0, indexOfRatio(5, 4), 0.0f);
			a.width = 0.4f;
			a.depth = 0.7f;
			a.fineSemitones = 1.5f;
			Settings b = onlyPartial(k, indexOfRatio(5, 4), 0.0f);
			PartialSettings& p = b.more[static_cast<size_t>(k - 1)];
			p.width = 0.4f;
			p.depth = 0.7f;
			p.fineSemitones = 1.5f;
			CHECK(sameBits(runLeft(a, in), runLeft(b, in)));
			// and with its sine up, high-pass too: the whole unit, not just the notch
			a.level = 0.6f;
			p.level = 0.6f;
			a.highpass = true;
			p.highpass = true;
			CHECK(sameBits(runLeft(a, in), runLeft(b, in)));
		}
	}

	SUBCASE("the notch takes 1 - depth of the input at its centre, per partial") {
		for (int k = 0; k < kPartialCount; ++k) {
			INFO("partial " << k + 1);
			Settings s = onlyPartial(k, indexOfRatio(3, 2), 0.0f);
			float* depth = k == 0 ? &s.depth : &s.more[static_cast<size_t>(k - 1)].depth;
			*depth = 0.6f;
			const double hz = expectedHz(48.0f, 3, 2);
			std::vector<float> in(96000);
			for (size_t i = 0; i < in.size(); ++i) {
				in[i] = 0.5f * static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
			}
			const std::vector<float> out = runLeft(s, in);
			CHECK(amplitudeAt(out, 48000, hz) == doctest::Approx(0.5 * (1.0 - 0.6)).epsilon(5.0e-3));
			// non-vacuity: with the partial off, the same input comes out whole
			Settings off = s;
			off.active = false;
			for (PartialSettings& p : off.more) {
				p.active = false;
			}
			CHECK(amplitudeAt(runLeft(off, in), 48000, hz) == doctest::Approx(0.5).epsilon(1.0e-3));
		}
	}
}

TEST_CASE("two partials sound the sum of their sines, and both notches are in the path") {
	const std::vector<float> silence(9600, 0.0f);
	Settings both = onlyPartial(0, kUnisonHarmonic, 0.5f);
	both.more[1].active = true;
	both.more[1].harmonic = indexOfRatio(3, 1);
	both.more[1].level = 0.25f;
	const Settings one = onlyPartial(0, kUnisonHarmonic, 0.5f);
	const Settings three = onlyPartial(3, indexOfRatio(3, 1), 0.25f);
	// partial 3 of `both` is more[1]; partial 4 of `three` is more[2] — the same ratio and level, in a different seat
	Settings threeSeat = onlyPartial(2, indexOfRatio(3, 1), 0.25f);
	const std::vector<float> a = runLeft(both, silence);
	const std::vector<float> b = runLeft(one, silence);
	const std::vector<float> c = runLeft(threeSeat, silence);
	double worst = 0.0;
	for (size_t i = 0; i < a.size(); ++i) {
		worst = std::max(worst, static_cast<double>(std::fabs(a[i] - (b[i] + c[i]))));
	}
	CHECK(worst < 1.0e-6);
	CHECK(amplitudeAt(a, 1000, expectedHz(48.0f, 1, 1)) > 0.1); // non-vacuity: there is something to add
	(void)three;

	SUBCASE("each partial's notch removes its own centre from a two-tone input") {
		const double f1 = expectedHz(48.0f, 1, 1);
		const double f3 = expectedHz(48.0f, 3, 1);
		std::vector<float> in(96000);
		for (size_t i = 0; i < in.size(); ++i) {
			const double t = static_cast<double>(i) / kRate;
			in[i] = static_cast<float>(0.4 * std::sin(2.0 * std::numbers::pi * f1 * t) + 0.4 * std::sin(2.0 * std::numbers::pi * f3 * t));
		}
		Settings s = both;
		s.level = 0.0f;
		s.more[1].level = 0.0f;
		s.depth = 1.0f;
		s.more[1].depth = 0.5f;
		const std::vector<float> out = runLeft(s, in);
		CHECK(amplitudeAt(out, 48000, f1) < 0.4 * 0.02);
		CHECK(amplitudeAt(out, 48000, f3) == doctest::Approx(0.4 * 0.5).epsilon(0.03));
	}
}

TEST_CASE("an inactive partial does no work and keeps no state") {
	const std::vector<float> in = testInput(9600, 11);
	Settings s = onlyPartial(0, kUnisonHarmonic, 1.0f);
	s.more[0].level = 1.0f;
	s.more[0].harmonic = indexOfRatio(2, 1);
	Harm h;
	h.prepare(kRate);
	h.setTarget(s);
	std::vector<float> l = in;
	std::vector<float> r = in;
	h.processStereo(l.data(), r.data(), static_cast<int>(l.size()));
	CHECK(h.partialSamplesForTest(0) == in.size());
	for (int k = 1; k < kPartialCount; ++k) {
		CHECK(h.partialSamplesForTest(k) == 0);
		CHECK(h.partialGridPointsForTest(k) == 0);
		CHECK(h.phaseForTest(k) == 0.0);
		CHECK(h.currentHzForTest(k) == 0.0);
		CHECK(h.notchDepthForTest(k) == 0.0);
	}
	CHECK(h.partialGridPointsForTest(0) == in.size() / 16);

	SUBCASE("switching one on starts counting it, and switching it off again forgets its state") {
		s.more[0].active = true;
		h.setTarget(s);
		l = in;
		r = in;
		h.processStereo(l.data(), r.data(), 4800);
		CHECK(h.partialSamplesForTest(1) == 4800);
		CHECK(h.currentHzForTest(1) > 0.0);
		s.more[0].active = false;
		h.setTarget(s);
		h.processStereo(l.data(), r.data(), 480);
		CHECK(h.partialSamplesForTest(1) == 4800); // not one more
		CHECK(h.currentHzForTest(1) == 0.0);
		CHECK(h.phaseForTest(1) == 0.0);
	}
}

namespace {

/// The retention of a one-pole whose time constant is the control's own float value, as the device reads it.
double poleRetention(float seconds) { return std::exp(-1.0 / (static_cast<double>(seconds) * kRate)); }

} // namespace

TEST_CASE("the envelope's stages sit where an independent restatement of the law puts them") {
	// delay 10 ms, attack tau 5 ms, hold 20 ms, decay tau 10 ms to a sustain of 0.4, release tau 50 ms
	Settings s = onlyPartial(0, kUnisonHarmonic, 1.0f);
	s.delaySeconds = 0.010f;
	s.attackSeconds = 0.005f;
	s.holdSeconds = 0.020f;
	s.decaySeconds = 0.010f;
	s.sustain = 0.4f;
	s.releaseSeconds = 0.050f;

	const double sus = static_cast<double>(0.4f); // the control's own float
	const double ca = poleRetention(0.005f);
	const double cd = poleRetention(0.010f);
	const double cr = poleRetention(0.050f);
	const int delayN = 480;
	const int holdN = 960;
	// the attack hands over when 1 - level <= 1e-3, i.e. at the first n with ca^n <= 1e-3 — the law, restated
	int peakN = 1;
	while (std::pow(ca, peakN) > 1.0e-3) {
		++peakN;
	}

	Harm h;
	h.prepare(kRate);
	h.setTarget(s);
	std::vector<float> l(1, 0.0f);
	std::vector<float> r(1, 0.0f);
	int done = 0;
	const auto advanceTo = [&](int n) {
		while (done < n) {
			h.processStereo(l.data(), r.data(), 1);
			l[0] = r[0] = 0.0f;
			++done;
		}
		return h.envelopeForTest();
	};

	CHECK(advanceTo(delayN) == 0.0);
	CHECK(h.envelopeStageForTest() == Envelope::Stage::Attack);
	CHECK(advanceTo(delayN + 240) == doctest::Approx(1.0 - std::pow(ca, 240)).epsilon(1.0e-9));
	CHECK(advanceTo(delayN + peakN + 480) == doctest::Approx(1.0 - std::pow(ca, peakN + 480)).epsilon(1.0e-9));
	CHECK(h.envelopeStageForTest() == Envelope::Stage::Hold);
	// the decay begins after the hold, from where the attack's law has carried the level
	const int decayStart = delayN + peakN + holdN;
	const double peak = 1.0 - std::pow(ca, peakN + holdN);
	CHECK(h.envelopeStageForTest() == Envelope::Stage::Hold);
	CHECK(advanceTo(decayStart) == doctest::Approx(peak).epsilon(1.0e-9));
	CHECK(advanceTo(decayStart + 480) == doctest::Approx(sus + (peak - sus) * std::pow(cd, 480)).epsilon(1.0e-9));
	CHECK(h.envelopeStageForTest() == Envelope::Stage::Decay);
	const double reached = advanceTo(decayStart + 20000);
	CHECK(reached == doctest::Approx(sus).epsilon(1.0e-6));
	CHECK(h.envelopeStageForTest() == Envelope::Stage::Sustain);

	// release: from wherever the sustain left it
	s.gate = false;
	h.setTarget(s);
	const double from = h.envelopeForTest();
	for (int i = 0; i < 480; ++i) {
		h.processStereo(l.data(), r.data(), 1);
		l[0] = r[0] = 0.0f;
	}
	CHECK(h.envelopeForTest() == doctest::Approx(from * std::pow(cr, 480)).epsilon(1.0e-9));
	CHECK(h.envelopeStageForTest() == Envelope::Stage::Release);

	SUBCASE("the drawn shape is the closed form of the same law") {
		const EnvelopeTimes t { static_cast<double>(0.010f), static_cast<double>(0.005f), static_cast<double>(0.020f),
		                        static_cast<double>(0.010f), sus, static_cast<double>(0.050f) };
		const auto segs = envelopeSegments(t);
		REQUIRE(segs.size() == 6);
		CHECK(segs[1].stage == Envelope::Stage::Attack);
		// the attack segment ends where the stepping envelope hands over, to within a sample
		CHECK(segs[1].seconds * kRate == doctest::Approx(peakN).epsilon(2.0e-3));
		CHECK(envelopeLevelAt(segs[1], t, 240.0 / kRate) == doctest::Approx(1.0 - std::pow(ca, 240)).epsilon(1.0e-9));
		CHECK(envelopeLevelAt(segs[3], t, 480.0 / kRate) == doctest::Approx(sus + (1.0 - sus) * std::pow(cd, 480)).epsilon(1.0e-9));
		CHECK(envelopeLevelAt(segs[5], t, 480.0 / kRate) == doctest::Approx(sus * std::pow(cr, 480)).epsilon(1.0e-9));
	}
}

TEST_CASE("with all four partials active the tail ends at exact zero and the envelope is done") {
	Settings s;
	s.noteSemitones = 60.0f;
	s.releaseSeconds = 0.05f;
	s.attackSeconds = 0.001f;
	s.gate = true;
	for (PartialSettings& p : s.more) {
		p.active = true;
	}
	Harm h;
	h.prepare(kRate);
	h.setTarget(s);
	std::vector<float> l(4800, 0.0f);
	std::vector<float> r(4800, 0.0f);
	h.processStereo(l.data(), r.data(), 4800);
	double loud = 0.0;
	for (const float v : l) {
		loud = std::max(loud, static_cast<double>(std::fabs(v)));
	}
	CHECK(loud > 0.5);
	s.gate = false;
	h.setTarget(s);
	bool silent = false;
	for (int block = 0; block < 100 && !silent; ++block) {
		std::fill(l.begin(), l.end(), 0.0f);
		std::fill(r.begin(), r.end(), 0.0f);
		h.processStereo(l.data(), r.data(), 4800);
		silent = std::all_of(l.begin(), l.end(), [](float v) { return v == 0.0f; })
		         && std::all_of(r.begin(), r.end(), [](float v) { return v == 0.0f; });
	}
	CHECK(silent);
	CHECK(h.envelopeForTest() == 0.0);
	CHECK(h.envelopeStageForTest() == Envelope::Stage::Idle);
}

TEST_CASE("every new parameter round-trips through a fresh instance, and an old row loads as one partial with a plain envelope") {
	// every new control set away from its default (derived from the descriptors, not from the struct)
	Row row = rowOf(State {});
	for (size_t i = static_cast<size_t>(Param::Partial1Active); i < static_cast<size_t>(Param::kCount); ++i) {
		const feathers::device::ParamDesc& d = kParamDescs[i];
		const double span = d.max - d.min;
		double v = d.min + span * 0.37;
		if (d.stepped) {
			v = std::round(d.def == d.min ? d.max : d.min);
			if (i == static_cast<size_t>(partialParam(1, PartialField::Harmonic))) {
				v = 33.0;
			}
		}
		row[i] = static_cast<float>(v);
	}
	const State loaded = stateOf(row, static_cast<size_t>(Param::kCount));
	const Row back = rowOf(loaded);
	for (size_t i = static_cast<size_t>(Param::Partial1Active); i < static_cast<size_t>(Param::kCount); ++i) {
		INFO(kParamDescs[i].key);
		if (kParamDescs[i].unit == feathers::device::Unit::LogSeconds) {
			CHECK(back[i] == doctest::Approx(row[i]).epsilon(1.0e-6));
		}
		else {
			CHECK(back[i] == row[i]);
		}
		CHECK(back[i] != static_cast<float>(kParamDescs[i].def)); // non-vacuity: every one moved
	}

	// the same through the device's own ops, into a fresh instance
	alignas(64) std::array<unsigned char, 8192> mem {};
	REQUIRE(kAudioOps.stateBytes <= mem.size());
	kAudioOps.construct(mem.data());
	kAudioOps.setFields(mem.data(), row.data(), row.size());
	CHECK(opsTargetForTest(mem.data()) == [&] {
		Settings expect = loaded.settings;
		return expect;
	}());
	kAudioOps.destruct(mem.data());

	SUBCASE("an old row — sixteen fields — has one partial, the others off, and an envelope that is only A and R") {
		Row old = row;
		const State fromOld = stateOf(old, 16);
		CHECK(fromOld.settings.active);
		for (const PartialSettings& p : fromOld.settings.more) {
			CHECK(!p.active);
		}
		CHECK(fromOld.settings.delaySeconds == 0.0f);
		CHECK(fromOld.settings.holdSeconds == 0.0f);
		CHECK(fromOld.settings.decaySeconds == 0.0f);
		CHECK(fromOld.settings.sustain == 1.0f);
		CHECK(fromOld.settings == stateOf(rowOf(State {}), 16).settings);
	}
}
